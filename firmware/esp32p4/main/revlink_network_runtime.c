#include "revlink_network_runtime.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "revlink_credentials.h"
#include "revlink_wifi_store.h"

/*
 * Raised from 4 KiB while investigating a stack protection fault that rebooted
 * the board every few tens of seconds.
 *
 * Measurement since then says this task was NOT the one overflowing: its peak
 * use is around 1.2 KiB, comfortably inside the old 4 KiB. The extra room is
 * kept because it costs a few KiB of a 32 MiB budget and this task calls into
 * the esp_hosted RPC stack, whose depth is not ours to control — but do not
 * read this size as a diagnosis. See the note in revlink_wifi_radio.c.
 */
#define REVLINK_NETWORK_RUNTIME_TASK_STACK 8192U
#define REVLINK_NETWORK_RUNTIME_TICK_MS 500U
#define REVLINK_NETWORK_STARTUP_TIMEOUT_MS 20000U
#define REVLINK_NETWORK_STARTUP_ATTEMPT_LIMIT 2U
#define REVLINK_NETWORK_RECONNECT_TIMEOUT_MS 30000U
#define REVLINK_NETWORK_HEALTH_PROBE_INTERVAL_MS 15000U
#define REVLINK_NETWORK_HEALTH_PROBE_TIMEOUT_MS 1000U
#define REVLINK_NETWORK_HEALTH_FAILURE_THRESHOLD 3U
#define REVLINK_NETWORK_FAULT_REBOOT_DELAY_MS 10000U
#define REVLINK_NETWORK_REBOOT_GUARD_RESET_MS 60000U
#define REVLINK_NETWORK_ACTION_LIMIT 8U
#define REVLINK_NETWORK_REBOOT_GUARD_MAGIC 0x524C4E47U
static const char *TAG = "revlink_network";
static SemaphoreHandle_t runtime_mutex;
static revlink_network_coordinator_t coordinator;
static atomic_bool runtime_started = ATOMIC_VAR_INIT(false);
static bool station_configured;
static bool station_credentials_persistent;
static bool hotspot_configured;
static bool awaiting_hotspot_credential;
static char station_ssid[REVLINK_WIFI_SSID_CAPACITY];
static char station_password[REVLINK_WIFI_PASSWORD_CAPACITY];
static char hotspot_password[REVLINK_WIFI_PASSWORD_CAPACITY];
static revlink_network_id_t station_network_id;
static esp_err_t last_station_error = ESP_OK;

typedef struct {
    uint32_t magic;
    bool network_reboot_used;
} revlink_network_reboot_guard_t;

static RTC_DATA_ATTR revlink_network_reboot_guard_t reboot_guard;

static void clear_sensitive(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;
    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

static revlink_network_id_t network_id_for_ssid(const char *ssid)
{
    uint32_t value = 2166136261U;
    for (const unsigned char *cursor = (const unsigned char *)ssid;
         *cursor != '\0';
         ++cursor) {
        value ^= *cursor;
        value *= 16777619U;
    }
    return value == 0U ? 1U : value;
}

static void assign_station_credential(
    const revlink_wifi_credentials_t *credential
)
{
    clear_sensitive(station_ssid, sizeof(station_ssid));
    clear_sensitive(station_password, sizeof(station_password));
    const size_t ssid_length =
        strnlen(credential->ssid, sizeof(station_ssid));
    const size_t password_length =
        strnlen(credential->password, sizeof(station_password));
    memcpy(station_ssid, credential->ssid, ssid_length);
    memcpy(station_password, credential->password, password_length);
    station_network_id = network_id_for_ssid(station_ssid);
    station_configured = true;
}

static esp_err_t load_saved_station(void)
{
    revlink_wifi_credentials_t credential = {0};
    const esp_err_t status = revlink_wifi_store_load(&credential);
    if (status == ESP_OK) {
        assign_station_credential(&credential);
        station_credentials_persistent = true;
    }
    revlink_wifi_credentials_clear(&credential);
    return status;
}

static void save_working_station(void)
{
    revlink_wifi_credentials_t credential = {0};
    if (!revlink_wifi_credentials_assign(
        &credential,
        station_ssid,
        station_password
    )) {
        station_credentials_persistent = false;
        return;
    }
    const esp_err_t status = revlink_wifi_store_save(&credential);
    revlink_wifi_credentials_clear(&credential);
    station_credentials_persistent = status == ESP_OK;
    if (status == ESP_OK) {
        ESP_LOGI(TAG, "Preferred station credential saved");
    } else {
        ESP_LOGE(
            TAG,
            "Preferred station credential could not be saved: %s",
            esp_err_to_name(status)
        );
    }
}

static esp_err_t execute_action(revlink_network_action_t action)
{
    for (size_t iteration = 0U;
         iteration < REVLINK_NETWORK_ACTION_LIMIT;
         ++iteration) {
        revlink_network_event_t event = {0};
        esp_err_t platform_status = ESP_OK;

        switch (action.kind) {
        case REVLINK_NETWORK_ACTION_NONE:
            return ESP_OK;

        case REVLINK_NETWORK_ACTION_SCAN_SAVED:
            if (station_configured) {
                event.kind = REVLINK_NETWORK_EVENT_SCAN_SELECTED;
                event.network_id = station_network_id;
            } else {
                event.kind = REVLINK_NETWORK_EVENT_SCAN_EMPTY;
            }
            break;

        case REVLINK_NETWORK_ACTION_CONNECT_SAVED:
            if (
                !station_configured
                || action.network_id != station_network_id
            ) {
                platform_status = ESP_ERR_NOT_FOUND;
            } else {
                platform_status = revlink_wifi_radio_connect_ephemeral(
                    station_ssid,
                    station_password,
                    REVLINK_NETWORK_STARTUP_TIMEOUT_MS
                );
            }
            last_station_error = platform_status;
            event.kind = platform_status == ESP_OK
                ? REVLINK_NETWORK_EVENT_CLIENT_CONNECTED
                : REVLINK_NETWORK_EVENT_CLIENT_FAILED;
            event.platform_error = platform_status;
            break;

        case REVLINK_NETWORK_ACTION_RECOVER_SAVED:
            if (
                !station_configured
                || action.network_id != station_network_id
            ) {
                platform_status = ESP_ERR_NOT_FOUND;
            } else {
                ESP_LOGW(
                    TAG,
                    "Station data path is stale; restarting the radio stack"
                );
                platform_status =
                    revlink_wifi_radio_connect_ephemeral(
                        station_ssid,
                        station_password,
                        REVLINK_NETWORK_STARTUP_TIMEOUT_MS
                    );
            }
            last_station_error = platform_status;
            event.kind = platform_status == ESP_OK
                ? REVLINK_NETWORK_EVENT_CLIENT_CONNECTED
                : REVLINK_NETWORK_EVENT_CLIENT_FAILED;
            event.platform_error = platform_status;
            break;

        case REVLINK_NETWORK_ACTION_PROBE_CLIENT:
            platform_status = revlink_wifi_radio_probe_gateway(
                REVLINK_NETWORK_HEALTH_PROBE_TIMEOUT_MS
            );
            event.kind = platform_status == ESP_OK
                ? REVLINK_NETWORK_EVENT_CLIENT_HEALTHY
                : REVLINK_NETWORK_EVENT_CLIENT_UNHEALTHY;
            event.platform_error = platform_status;
            break;

        case REVLINK_NETWORK_ACTION_START_HOTSPOT:
            if (!hotspot_configured) {
                awaiting_hotspot_credential = true;
                ESP_LOGI(
                    TAG,
                    "Fallback hotspot is waiting for a RAM-only credential"
                );
                return ESP_OK;
            }
            awaiting_hotspot_credential = false;
            platform_status = revlink_wifi_radio_start_hotspot_ephemeral(
                hotspot_password,
                REVLINK_NETWORK_STARTUP_TIMEOUT_MS
            );
            event.kind = platform_status == ESP_OK
                ? REVLINK_NETWORK_EVENT_HOTSPOT_STARTED
                : REVLINK_NETWORK_EVENT_HOTSPOT_FAILED;
            event.platform_error = platform_status;
            break;

        case REVLINK_NETWORK_ACTION_STOP_RADIO:
            return revlink_wifi_radio_stop();

        default:
            return ESP_ERR_INVALID_ARG;
        }

        const revlink_network_status_t status =
            revlink_network_coordinator_handle(
                &coordinator,
                &event,
                &action
            );
        if (status != REVLINK_NETWORK_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t handle_event(const revlink_network_event_t *event)
{
    revlink_network_action_t action = {0};
    const revlink_network_status_t status =
        revlink_network_coordinator_handle(
            &coordinator,
            event,
            &action
        );
    if (status == REVLINK_NETWORK_TRANSFER_LOCKED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status != REVLINK_NETWORK_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return execute_action(action);
}

static esp_err_t restart_policy(void)
{
    revlink_network_event_t event = {
        .kind = REVLINK_NETWORK_EVENT_STOP,
    };
    esp_err_t status = handle_event(&event);
    if (status != ESP_OK) {
        return status;
    }
    awaiting_hotspot_credential = false;
    event = (revlink_network_event_t){
        .kind = REVLINK_NETWORK_EVENT_START,
    };
    return handle_event(&event);
}

static void runtime_task(void *context)
{
    (void)context;
    uint32_t fault_elapsed_ms = 0U;
    uint32_t stable_elapsed_ms = 0U;
    /*
     * Report the closest this task has come to exhausting its stack, so a
     * future overflow arrives with evidence instead of requiring the panic to
     * be caught live on a serial cable. Logged only when it drops to a new
     * low, so a healthy device stays quiet.
     */
    UBaseType_t stack_low_water = UINT32_MAX;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REVLINK_NETWORK_RUNTIME_TICK_MS));

        /*
         * ESP-IDF's uxTaskGetStackHighWaterMark returns bytes, not words as
         * vanilla FreeRTOS does. Do not scale it.
         */
        const UBaseType_t headroom = uxTaskGetStackHighWaterMark(NULL);
        if (headroom < stack_low_water) {
            stack_low_water = headroom;
            ESP_LOGI(
                TAG,
                "network task stack: %u bytes free of %u (peak use %u)",
                (unsigned int)headroom,
                (unsigned int)REVLINK_NETWORK_RUNTIME_TASK_STACK,
                (unsigned int)(REVLINK_NETWORK_RUNTIME_TASK_STACK - headroom)
            );
        }
        if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const revlink_network_snapshot_t network =
            revlink_network_coordinator_snapshot(&coordinator);
        const revlink_wifi_radio_snapshot_t radio =
            revlink_wifi_radio_snapshot();
        if (
            network.state == REVLINK_NETWORK_CLIENT_READY
            && radio.state == REVLINK_WIFI_RADIO_FAILED
        ) {
            const revlink_network_event_t lost = {
                .kind = REVLINK_NETWORK_EVENT_CLIENT_LOST,
                .platform_error = radio.last_error,
            };
            (void)handle_event(&lost);
        }

        const revlink_network_event_t tick = {
            .kind = REVLINK_NETWORK_EVENT_TICK,
            .elapsed_ms = REVLINK_NETWORK_RUNTIME_TICK_MS,
        };
        (void)handle_event(&tick);

        const revlink_network_snapshot_t after_tick =
            revlink_network_coordinator_snapshot(&coordinator);
        if (
            (after_tick.state == REVLINK_NETWORK_CLIENT_READY
             || after_tick.state == REVLINK_NETWORK_HOTSPOT_READY)
            && !after_tick.transfer_active
        ) {
            fault_elapsed_ms = 0U;
            if (stable_elapsed_ms
                < REVLINK_NETWORK_REBOOT_GUARD_RESET_MS) {
                stable_elapsed_ms += REVLINK_NETWORK_RUNTIME_TICK_MS;
            }
            if (
                stable_elapsed_ms
                    >= REVLINK_NETWORK_REBOOT_GUARD_RESET_MS
                && reboot_guard.network_reboot_used
            ) {
                reboot_guard = (revlink_network_reboot_guard_t){
                    .magic = REVLINK_NETWORK_REBOOT_GUARD_MAGIC,
                    .network_reboot_used = false,
                };
                ESP_LOGI(
                    TAG,
                    "Network reboot guard rearmed after stable operation"
                );
            }
        } else if (
            after_tick.state == REVLINK_NETWORK_FAULTED
            && !after_tick.transfer_active
        ) {
            stable_elapsed_ms = 0U;
            fault_elapsed_ms += REVLINK_NETWORK_RUNTIME_TICK_MS;
            if (
                fault_elapsed_ms >= REVLINK_NETWORK_FAULT_REBOOT_DELAY_MS
                && !reboot_guard.network_reboot_used
            ) {
                reboot_guard.magic = REVLINK_NETWORK_REBOOT_GUARD_MAGIC;
                reboot_guard.network_reboot_used = true;
                ESP_LOGE(
                    TAG,
                    "Network recovery exhausted; performing one guarded reboot"
                );
                xSemaphoreGive(runtime_mutex);
                vTaskDelay(pdMS_TO_TICKS(100U));
                esp_restart();
            }
        } else {
            fault_elapsed_ms = 0U;
            stable_elapsed_ms = 0U;
        }
        xSemaphoreGive(runtime_mutex);
    }
}

esp_err_t revlink_network_runtime_start(void)
{
    if (atomic_load(&runtime_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime_mutex = xSemaphoreCreateMutex();
    if (runtime_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!revlink_network_coordinator_init(
        &coordinator,
        &(revlink_network_config_t){
            .startup_timeout_ms = REVLINK_NETWORK_STARTUP_TIMEOUT_MS,
            .startup_attempt_limit =
                REVLINK_NETWORK_STARTUP_ATTEMPT_LIMIT,
            .reconnect_timeout_ms = REVLINK_NETWORK_RECONNECT_TIMEOUT_MS,
            .health_probe_interval_ms =
                REVLINK_NETWORK_HEALTH_PROBE_INTERVAL_MS,
            .health_failure_threshold =
                REVLINK_NETWORK_HEALTH_FAILURE_THRESHOLD,
        }
    )) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reboot_guard.magic != REVLINK_NETWORK_REBOOT_GUARD_MAGIC) {
        reboot_guard = (revlink_network_reboot_guard_t){
            .magic = REVLINK_NETWORK_REBOOT_GUARD_MAGIC,
            .network_reboot_used = false,
        };
    }
    const esp_err_t saved_status = load_saved_station();
    if (saved_status == ESP_OK) {
        ESP_LOGI(TAG, "Saved preferred station is available");
    } else if (saved_status != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(
            TAG,
            "Saved preferred station is unavailable: %s",
            esp_err_to_name(saved_status)
        );
    }
    esp_err_t status = revlink_wifi_radio_start();
    if (status != ESP_OK) {
        return status;
    }
    const esp_err_t scan_status = revlink_wifi_radio_scan_visible();
    if (scan_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Startup network scan unavailable: %s; fallback remains available",
            esp_err_to_name(scan_status)
        );
    }
    const revlink_network_event_t start = {
        .kind = REVLINK_NETWORK_EVENT_START,
    };
    status = handle_event(&start);
    if (status != ESP_OK) {
        return status;
    }
    const BaseType_t created = xTaskCreate(
        runtime_task,
        "revlink_network",
        REVLINK_NETWORK_RUNTIME_TASK_STACK,
        NULL,
        4,
        NULL
    );
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&runtime_started, true);
    ESP_LOGI(
        TAG,
        "Network coordinator started; working station credentials persist"
    );
    return ESP_OK;
}

esp_err_t revlink_network_runtime_configure_station(
    const char *ssid,
    const char *password
)
{
    revlink_wifi_credentials_t credential = {0};
    if (
        !atomic_load(&runtime_started)
        || !revlink_wifi_credentials_assign(&credential, ssid, password)
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
        revlink_wifi_credentials_clear(&credential);
        return ESP_ERR_TIMEOUT;
    }
    assign_station_credential(&credential);
    revlink_wifi_credentials_clear(&credential);
    station_credentials_persistent = false;

    const revlink_network_snapshot_t network =
        revlink_network_coordinator_snapshot(&coordinator);
    esp_err_t status = ESP_OK;
    if (
        network.state == REVLINK_NETWORK_HOTSPOT_STARTING
        && awaiting_hotspot_credential
    ) {
        status = restart_policy();
    } else {
        const revlink_network_event_t retry = {
            .kind = REVLINK_NETWORK_EVENT_RETRY_SAVED,
        };
        status = handle_event(&retry);
    }
    const revlink_network_snapshot_t result =
        revlink_network_coordinator_snapshot(&coordinator);
    if (
        status == ESP_OK
        && result.state == REVLINK_NETWORK_CLIENT_READY
    ) {
        save_working_station();
    } else if (
        status == ESP_OK
        && last_station_error != ESP_OK
    ) {
        /*
         * Hotspot restoration may succeed after a failed station join. Report
         * the requested join result to the caller, not the fallback result.
         */
        status = last_station_error;
    }
    xSemaphoreGive(runtime_mutex);
    return status;
}

esp_err_t revlink_network_runtime_last_station_error(void)
{
    if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t status = last_station_error;
    xSemaphoreGive(runtime_mutex);
    return status;
}

esp_err_t revlink_network_runtime_forget_station(void)
{
    if (!atomic_load(&runtime_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t erase_status = revlink_wifi_store_erase();
    if (erase_status != ESP_OK) {
        xSemaphoreGive(runtime_mutex);
        return erase_status;
    }
    clear_sensitive(station_ssid, sizeof(station_ssid));
    clear_sensitive(station_password, sizeof(station_password));
    station_network_id = 0U;
    station_configured = false;
    station_credentials_persistent = false;
    const esp_err_t status = restart_policy();
    xSemaphoreGive(runtime_mutex);
    return status;
}

esp_err_t revlink_network_runtime_configure_hotspot_ephemeral(
    const char *password
)
{
    if (
        !atomic_load(&runtime_started)
        || !revlink_wifi_password_valid(password, false)
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    clear_sensitive(hotspot_password, sizeof(hotspot_password));
    const size_t password_length =
        strnlen(password, sizeof(hotspot_password));
    memcpy(hotspot_password, password, password_length);
    hotspot_configured = true;

    esp_err_t status = ESP_OK;
    const revlink_network_snapshot_t network =
        revlink_network_coordinator_snapshot(&coordinator);
    if (
        network.state == REVLINK_NETWORK_HOTSPOT_STARTING
        && awaiting_hotspot_credential
    ) {
        status = execute_action((revlink_network_action_t){
            .kind = REVLINK_NETWORK_ACTION_START_HOTSPOT,
        });
    }
    xSemaphoreGive(runtime_mutex);
    return status;
}

esp_err_t revlink_network_runtime_force_hotspot(void)
{
    if (!atomic_load(&runtime_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const revlink_network_event_t event = {
        .kind = REVLINK_NETWORK_EVENT_FORCE_HOTSPOT,
    };
    const esp_err_t status = handle_event(&event);
    xSemaphoreGive(runtime_mutex);
    return status;
}

esp_err_t revlink_network_runtime_set_transfer_active(bool active)
{
    if (!atomic_load(&runtime_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const revlink_network_snapshot_t snapshot =
        revlink_network_coordinator_snapshot(&coordinator);
    if (snapshot.transfer_active == active) {
        xSemaphoreGive(runtime_mutex);
        return ESP_OK;
    }
    const revlink_network_event_t event = {
        .kind = active
            ? REVLINK_NETWORK_EVENT_TRANSFER_STARTED
            : REVLINK_NETWORK_EVENT_TRANSFER_FINISHED,
    };
    const esp_err_t status = handle_event(&event);
    xSemaphoreGive(runtime_mutex);
    return status;
}

revlink_network_runtime_snapshot_t revlink_network_runtime_snapshot(void)
{
    revlink_network_runtime_snapshot_t snapshot = {0};
    if (
        runtime_mutex == NULL
        || xSemaphoreTake(runtime_mutex, portMAX_DELAY) != pdTRUE
    ) {
        return snapshot;
    }
    snapshot = (revlink_network_runtime_snapshot_t){
        .coordinator = revlink_network_coordinator_snapshot(&coordinator),
        .radio = revlink_wifi_radio_snapshot(),
        .station_configured = station_configured,
        .station_credentials_persistent =
            station_credentials_persistent,
        .hotspot_configured = hotspot_configured,
        .awaiting_hotspot_credential = awaiting_hotspot_credential,
    };
    if (
        snapshot.coordinator.state == REVLINK_NETWORK_CLIENT_READY
        && station_configured
    ) {
        const size_t ssid_length =
            strnlen(station_ssid, sizeof(snapshot.connected_ssid) - 1U);
        memcpy(snapshot.connected_ssid, station_ssid, ssid_length);
        snapshot.connected_ssid[ssid_length] = '\0';
    }
    xSemaphoreGive(runtime_mutex);
    return snapshot;
}
