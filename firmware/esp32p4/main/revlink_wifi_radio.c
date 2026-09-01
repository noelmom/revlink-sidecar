#include "revlink_wifi_radio.h"

#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

#if CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#endif
#include "esp_log.h"
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "revlink_identity.h"

#define REVLINK_WIFI_SCAN_RECORD_LIMIT 24U
#define REVLINK_WIFI_SCAN_TASK_STACK 4096U
#define REVLINK_WIFI_HOTSPOT_STARTED_BIT BIT2
#define REVLINK_WIFI_HOTSPOT_FAILED_BIT BIT3

static portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static revlink_wifi_radio_snapshot_t current_snapshot = {
    .state = REVLINK_WIFI_RADIO_DISABLED,
    .strongest_rssi = INT8_MIN,
};
static revlink_wifi_visible_network_t
    visible_networks[REVLINK_WIFI_VISIBLE_NETWORK_LIMIT];
static size_t visible_network_count;
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
static bool local_identity_ready;
static char local_identity_ssid[REVLINK_IDENTITY_SSID_CAPACITY];
static char local_identity_hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY];
static uint16_t preferred_collision_index;
#endif

static void publish_snapshot(
    revlink_wifi_radio_state_t state,
    uint16_t access_point_count,
    int8_t strongest_rssi,
    uint8_t strongest_channel,
    esp_err_t last_error
)
{
    taskENTER_CRITICAL(&snapshot_lock);
    const uint8_t hotspot_client_count =
        state == REVLINK_WIFI_RADIO_HOTSPOT_READY
        || state == REVLINK_WIFI_RADIO_HOTSPOT_STARTING
        ? current_snapshot.hotspot_client_count
        : 0U;
    current_snapshot = (revlink_wifi_radio_snapshot_t){
        .state = state,
        .access_point_count = access_point_count,
        .strongest_rssi = strongest_rssi,
        .strongest_channel = strongest_channel,
        .hotspot_client_count = hotspot_client_count,
        .last_error = last_error,
    };
    taskEXIT_CRITICAL(&snapshot_lock);
}

#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
static void publish_hotspot_client_delta(int delta)
{
    taskENTER_CRITICAL(&snapshot_lock);
    int count = (int)current_snapshot.hotspot_client_count + delta;
    if (count < 0) {
        count = 0;
    } else if (count > UINT8_MAX) {
        count = UINT8_MAX;
    }
    current_snapshot.hotspot_client_count = (uint8_t)count;
    taskEXIT_CRITICAL(&snapshot_lock);
}
#endif

revlink_wifi_radio_snapshot_t revlink_wifi_radio_snapshot(void)
{
    revlink_wifi_radio_snapshot_t snapshot;
    taskENTER_CRITICAL(&snapshot_lock);
    snapshot = current_snapshot;
    taskEXIT_CRITICAL(&snapshot_lock);
    return snapshot;
}

size_t revlink_wifi_radio_visible_networks(
    revlink_wifi_visible_network_t *networks,
    size_t capacity
)
{
    if (networks == NULL || capacity == 0U) {
        return 0U;
    }
    taskENTER_CRITICAL(&snapshot_lock);
    const size_t count =
        visible_network_count < capacity ? visible_network_count : capacity;
    memcpy(networks, visible_networks, count * sizeof(*networks));
    taskEXIT_CRITICAL(&snapshot_lock);
    return count;
}

#if CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
static const char *TAG = "revlink_wifi";
static atomic_bool scan_in_progress = ATOMIC_VAR_INIT(false);
static atomic_bool radio_started = ATOMIC_VAR_INIT(false);
static atomic_bool wifi_driver_started = ATOMIC_VAR_INIT(false);
static esp_netif_t *station_netif;
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
#define REVLINK_WIFI_JOIN_CONNECTED_BIT BIT0
#define REVLINK_WIFI_JOIN_FAILED_BIT BIT1

static EventGroupHandle_t join_events;
static atomic_bool join_in_progress = ATOMIC_VAR_INIT(false);
static atomic_bool onboarding_hotspot_overlap = ATOMIC_VAR_INIT(false);
static esp_netif_t *hotspot_netif;

static void clear_sensitive(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;
    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

static void wifi_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    if (join_events == NULL) {
        return;
    }
    if (
        atomic_load(&join_in_progress)
        && event_base == IP_EVENT
        && event_id == IP_EVENT_STA_GOT_IP
    ) {
        xEventGroupSetBits(join_events, REVLINK_WIFI_JOIN_CONNECTED_BIT);
    } else if (
        event_base == WIFI_EVENT
        && event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        if (disconnected != NULL) {
            ESP_LOGW(
                TAG,
                "C6 station disconnected; reason=%u",
                (unsigned int)disconnected->reason
            );
        }
        if (atomic_load(&join_in_progress)) {
            xEventGroupSetBits(join_events, REVLINK_WIFI_JOIN_FAILED_BIT);
        } else if (
            revlink_wifi_radio_snapshot().state
            == REVLINK_WIFI_RADIO_CONNECTED
        ) {
            const revlink_wifi_radio_snapshot_t previous =
                revlink_wifi_radio_snapshot();
            publish_snapshot(
                REVLINK_WIFI_RADIO_FAILED,
                previous.access_point_count,
                previous.strongest_rssi,
                previous.strongest_channel,
                ESP_ERR_WIFI_CONN
            );
        }
    } else if (
        event_base == IP_EVENT
        && event_id == IP_EVENT_STA_LOST_IP
        && !atomic_load(&join_in_progress)
        && revlink_wifi_radio_snapshot().state
            == REVLINK_WIFI_RADIO_CONNECTED
    ) {
        const revlink_wifi_radio_snapshot_t previous =
            revlink_wifi_radio_snapshot();
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            previous.access_point_count,
            previous.strongest_rssi,
            previous.strongest_channel,
            ESP_FAIL
        );
    } else if (
        event_base == WIFI_EVENT
        && event_id == WIFI_EVENT_AP_START
    ) {
        xEventGroupSetBits(join_events, REVLINK_WIFI_HOTSPOT_STARTED_BIT);
    } else if (
        event_base == WIFI_EVENT
        && event_id == WIFI_EVENT_AP_STACONNECTED
    ) {
        publish_hotspot_client_delta(1);
    } else if (
        event_base == WIFI_EVENT
        && event_id == WIFI_EVENT_AP_STADISCONNECTED
    ) {
        publish_hotspot_client_delta(-1);
    }
}
#endif

static bool printable_ssid(const uint8_t *ssid, size_t *length)
{
    size_t size = 0U;
    while (size < 32U && ssid[size] != 0U) {
        if (ssid[size] < 0x20U || ssid[size] > 0x7eU) {
            return false;
        }
        ++size;
    }
    if (size == 0U || size >= 33U) {
        return false;
    }
    *length = size;
    return true;
}

static size_t cache_visible_records(
    const wifi_ap_record_t *records,
    uint16_t record_count
)
{
    revlink_wifi_visible_network_t cache[
        REVLINK_WIFI_VISIBLE_NETWORK_LIMIT
    ] = {0};
    size_t count = 0U;
    for (uint16_t index = 0U; index < record_count; ++index) {
        size_t ssid_length = 0U;
        if (!printable_ssid(records[index].ssid, &ssid_length)) {
            continue;
        }
        size_t duplicate = count;
        for (size_t entry = 0U; entry < count; ++entry) {
            if (
                strncmp(
                    cache[entry].ssid,
                    (const char *)records[index].ssid,
                    sizeof(cache[entry].ssid)
                ) == 0
            ) {
                duplicate = entry;
                break;
            }
        }
        if (duplicate < count) {
            if (records[index].rssi > cache[duplicate].rssi) {
                cache[duplicate].rssi = records[index].rssi;
            }
            cache[duplicate].secured =
                cache[duplicate].secured
                || records[index].authmode != WIFI_AUTH_OPEN;
            continue;
        }
        if (count >= REVLINK_WIFI_VISIBLE_NETWORK_LIMIT) {
            continue;
        }
        memcpy(cache[count].ssid, records[index].ssid, ssid_length);
        cache[count].ssid[ssid_length] = '\0';
        cache[count].rssi = records[index].rssi;
        cache[count].secured = records[index].authmode != WIFI_AUTH_OPEN;
        ++count;
    }
    for (size_t left = 0U; left < count; ++left) {
        for (size_t right = left + 1U; right < count; ++right) {
            if (cache[right].rssi > cache[left].rssi) {
                const revlink_wifi_visible_network_t swap = cache[left];
                cache[left] = cache[right];
                cache[right] = swap;
            }
        }
    }
    taskENTER_CRITICAL(&snapshot_lock);
    memset(visible_networks, 0, sizeof(visible_networks));
    memcpy(visible_networks, cache, count * sizeof(*cache));
    visible_network_count = count;
    taskEXIT_CRITICAL(&snapshot_lock);
    return count;
}

static esp_err_t perform_scan(void)
{
    publish_snapshot(
        REVLINK_WIFI_RADIO_SCANNING,
        0U,
        INT8_MIN,
        0U,
        ESP_OK
    );
    ESP_LOGI(
        TAG,
        "C6 radio scan started; network identities will not be logged"
    );

    const wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0U,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t status = esp_wifi_scan_start(&scan_config, true);
    if (status != ESP_OK) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        ESP_LOGE(TAG, "C6 radio scan failed: %s", esp_err_to_name(status));
        return status;
    }

    uint16_t total_count = 0U;
    status = esp_wifi_scan_get_ap_num(&total_count);
    if (status != ESP_OK) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        ESP_LOGE(
            TAG,
            "Unable to read C6 scan result count: %s",
            esp_err_to_name(status)
        );
        return status;
    }

    uint16_t record_count = total_count;
    if (record_count > REVLINK_WIFI_SCAN_RECORD_LIMIT) {
        record_count = REVLINK_WIFI_SCAN_RECORD_LIMIT;
    }
    wifi_ap_record_t *records = NULL;
    if (record_count > 0U) {
        records = calloc(record_count, sizeof(*records));
        if (records == NULL) {
            publish_snapshot(
                REVLINK_WIFI_RADIO_FAILED,
                total_count,
                INT8_MIN,
                0U,
                ESP_ERR_NO_MEM
            );
            ESP_LOGE(TAG, "Unable to allocate bounded C6 scan records");
            return ESP_ERR_NO_MEM;
        }
        status = esp_wifi_scan_get_ap_records(&record_count, records);
        if (status != ESP_OK) {
            free(records);
            publish_snapshot(
                REVLINK_WIFI_RADIO_FAILED,
                total_count,
                INT8_MIN,
                0U,
                status
            );
            ESP_LOGE(
                TAG,
                "Unable to read bounded C6 scan records: %s",
                esp_err_to_name(status)
            );
            return status;
        }
    }

    int8_t strongest_rssi = INT8_MIN;
    uint8_t strongest_channel = 0U;
    for (uint16_t index = 0U; index < record_count; ++index) {
        if (records[index].rssi > strongest_rssi) {
            strongest_rssi = records[index].rssi;
            strongest_channel = records[index].primary;
        }
    }
    const size_t cached_count = cache_visible_records(records, record_count);
    free(records);

    publish_snapshot(
        REVLINK_WIFI_RADIO_READY,
        total_count,
        strongest_rssi,
        strongest_channel,
        ESP_OK
    );
    ESP_LOGI(
        TAG,
        "C6 scan complete: detected=%u visible=%u strongest=%d dBm "
        "channel=%u; network identities suppressed",
        (unsigned int)total_count,
        (unsigned int)cached_count,
        strongest_rssi,
        (unsigned int)strongest_channel
    );
    return ESP_OK;
}

static void scan_task(void *context)
{
    (void)context;
    (void)perform_scan();
    atomic_store(&scan_in_progress, false);
    vTaskDelete(NULL);
}

static esp_err_t restart_wifi(
    wifi_mode_t mode,
    wifi_interface_t interface,
    wifi_config_t *configuration
)
{
    esp_err_t status = esp_wifi_stop();
    if (status != ESP_OK && status != ESP_ERR_WIFI_NOT_STARTED) {
        return status;
    }
    atomic_store(&wifi_driver_started, false);
    status = esp_wifi_set_mode(mode);
    if (status == ESP_OK && configuration != NULL) {
        status = esp_wifi_set_config(interface, configuration);
    }
    if (status == ESP_OK) {
        status = esp_wifi_start();
        if (status == ESP_OK) {
            atomic_store(&wifi_driver_started, true);
        }
    }
    return status;
}

static esp_err_t prepare_station_join(
    wifi_config_t *configuration,
    revlink_wifi_radio_state_t previous_state,
    bool *restarted
)
{
    *restarted = false;
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t mode_status = esp_wifi_get_mode(&mode);
    if (
        mode_status == ESP_OK
        && mode == WIFI_MODE_STA
        && atomic_load(&wifi_driver_started)
        && previous_state == REVLINK_WIFI_RADIO_READY
    ) {
        /*
         * The startup scan leaves a running station interface behind. Reuse
         * it instead of immediately stopping and starting the hosted C6 a
         * second time. Back-to-back restarts have produced association
         * failures on otherwise valid saved credentials after a P4 reset.
         */
        return esp_wifi_set_config(WIFI_IF_STA, configuration);
    }

    *restarted = true;
    return restart_wifi(WIFI_MODE_STA, WIFI_IF_STA, configuration);
}
#endif

esp_err_t revlink_wifi_radio_start(void)
{
#if !CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
    publish_snapshot(
        REVLINK_WIFI_RADIO_DISABLED,
        0U,
        INT8_MIN,
        0U,
        ESP_OK
    );
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (atomic_load(&radio_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    publish_snapshot(
        REVLINK_WIFI_RADIO_STARTING,
        0U,
        INT8_MIN,
        0U,
        ESP_OK
    );

    esp_err_t status = esp_netif_init();
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        return status;
    }
    status = esp_event_loop_create_default();
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        return status;
    }
    station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == NULL) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            ESP_FAIL
        );
        return ESP_FAIL;
    }
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    hotspot_netif = esp_netif_create_default_wifi_ap();
    if (hotspot_netif == NULL) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            ESP_FAIL
        );
        return ESP_FAIL;
    }
#endif

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    status = esp_wifi_init(&wifi_config);
    if (status == ESP_OK) {
        status = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (status == ESP_OK) {
        status = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (status == ESP_OK) {
        status = esp_wifi_start();
        if (status == ESP_OK) {
            atomic_store(&wifi_driver_started, true);
        }
    }
    if (status != ESP_OK) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        return status;
    }

#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    join_events = xEventGroupCreate();
    if (join_events == NULL) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            ESP_ERR_NO_MEM
        );
        return ESP_ERR_NO_MEM;
    }
    status = esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL
    );
    if (status == ESP_OK) {
        status = esp_event_handler_register(
            IP_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        );
    }
    if (status != ESP_OK) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        return status;
    }
#endif
    atomic_store(&radio_started, true);
    publish_snapshot(
        REVLINK_WIFI_RADIO_READY,
        0U,
        INT8_MIN,
        0U,
        ESP_OK
    );
    return ESP_OK;
#endif
}

esp_err_t revlink_wifi_radio_scan_anonymous(void)
{
#if !CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        !atomic_load(&radio_started)
        || atomic_exchange(&scan_in_progress, true)
    ) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    if (atomic_load(&join_in_progress)) {
        atomic_store(&scan_in_progress, false);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    esp_err_t status = restart_wifi(WIFI_MODE_STA, WIFI_IF_STA, NULL);
    if (status != ESP_OK) {
        atomic_store(&scan_in_progress, false);
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            status
        );
        return status;
    }
    const BaseType_t created = xTaskCreate(
        scan_task,
        "revlink_wifi_scan",
        REVLINK_WIFI_SCAN_TASK_STACK,
        NULL,
        4,
        NULL
    );
    if (created != pdPASS) {
        atomic_store(&scan_in_progress, false);
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            0U,
            ESP_ERR_NO_MEM
        );
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

esp_err_t revlink_wifi_radio_scan_visible(void)
{
#if !CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        !atomic_load(&radio_started)
        || atomic_exchange(&scan_in_progress, true)
    ) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    if (atomic_load(&join_in_progress)) {
        atomic_store(&scan_in_progress, false);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    esp_err_t status = restart_wifi(WIFI_MODE_STA, WIFI_IF_STA, NULL);
    if (status == ESP_OK) {
        status = perform_scan();
    }
    atomic_store(&scan_in_progress, false);
    return status;
#endif
}

esp_err_t revlink_wifi_radio_connect_ephemeral(
    const char *ssid,
    const char *password,
    uint32_t timeout_ms
)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    (void)ssid;
    (void)password;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        ssid == NULL
        || password == NULL
        || timeout_ms == 0U
        || join_events == NULL
        || atomic_load(&scan_in_progress)
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_length = strnlen(ssid, sizeof(((wifi_config_t *)0)->sta.ssid));
    const size_t password_length =
        strnlen(password, sizeof(((wifi_config_t *)0)->sta.password));
    if (
        ssid_length == 0U
        || ssid_length >= sizeof(((wifi_config_t *)0)->sta.ssid)
        || password_length >= sizeof(((wifi_config_t *)0)->sta.password)
        || atomic_load(&join_in_progress)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t configuration = {0};
    memcpy(configuration.sta.ssid, ssid, ssid_length);
    memcpy(configuration.sta.password, password, password_length);
    configuration.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    configuration.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    configuration.sta.threshold.authmode = WIFI_AUTH_OPEN;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    const revlink_wifi_radio_snapshot_t previous_snapshot =
        revlink_wifi_radio_snapshot();
    publish_snapshot(
        REVLINK_WIFI_RADIO_CONNECTING,
        previous_snapshot.access_point_count,
        previous_snapshot.strongest_rssi,
        previous_snapshot.strongest_channel,
        ESP_OK
    );
    ESP_LOGI(
        TAG,
        "Ephemeral C6 station join started; network identity is suppressed"
    );

    /*
     * Do not depend on AP+STA overlap. A hosted C6 can reject a valid station
     * join when the setup hotspot and selected network require incompatible
     * channels. The coordinator restores the same hotspot credential if this
     * clean station transition fails.
    */
    atomic_store(&onboarding_hotspot_overlap, false);
    bool restarted = false;
    esp_err_t status = prepare_station_join(
        &configuration,
        previous_snapshot.state,
        &restarted
    );
    clear_sensitive(&configuration, sizeof(configuration));
    if (status == ESP_OK && restarted) {
        /*
         * Hosted Wi-Fi start events are asynchronous. Give the C6 a short,
         * bounded settling interval before requesting association.
         */
        vTaskDelay(pdMS_TO_TICKS(200U));
    }
    xEventGroupClearBits(
        join_events,
        REVLINK_WIFI_JOIN_CONNECTED_BIT | REVLINK_WIFI_JOIN_FAILED_BIT
    );
    atomic_store(&join_in_progress, true);
    if (status == ESP_OK) {
        status = esp_wifi_connect();
    }
    if (status != ESP_OK) {
        atomic_store(&join_in_progress, false);
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            previous_snapshot.access_point_count,
            previous_snapshot.strongest_rssi,
            previous_snapshot.strongest_channel,
            status
        );
        return status;
    }

    const EventBits_t result = xEventGroupWaitBits(
        join_events,
        REVLINK_WIFI_JOIN_CONNECTED_BIT | REVLINK_WIFI_JOIN_FAILED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );
    atomic_store(&join_in_progress, false);
    if ((result & REVLINK_WIFI_JOIN_CONNECTED_BIT) != 0U) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_CONNECTED,
            previous_snapshot.access_point_count,
            previous_snapshot.strongest_rssi,
            previous_snapshot.strongest_channel,
            ESP_OK
        );
        ESP_LOGI(
            TAG,
            "Ephemeral C6 station association and DHCP PASSED"
        );
        return ESP_OK;
    }

    status = (result & REVLINK_WIFI_JOIN_FAILED_BIT) != 0U
        ? ESP_ERR_WIFI_CONN
        : ESP_ERR_TIMEOUT;
    publish_snapshot(
        REVLINK_WIFI_RADIO_FAILED,
        previous_snapshot.access_point_count,
        previous_snapshot.strongest_rssi,
        previous_snapshot.strongest_channel,
        status
    );
    ESP_LOGE(TAG, "Ephemeral C6 station join failed: %s", esp_err_to_name(status));
    return status;
#endif
}

esp_err_t revlink_wifi_radio_finish_onboarding_transition(void)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        revlink_wifi_radio_snapshot().state != REVLINK_WIFI_RADIO_CONNECTED
        || atomic_load(&join_in_progress)
        || atomic_load(&scan_in_progress)
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t status = esp_wifi_set_mode(WIFI_MODE_STA);
    if (status == ESP_OK) {
        atomic_store(&onboarding_hotspot_overlap, false);
        ESP_LOGI(
            TAG,
            "Onboarding overlap completed; station mode remains active"
        );
    }
    return status;
#endif
}

esp_err_t revlink_wifi_radio_start_hotspot_ephemeral(
    const char *password,
    uint32_t timeout_ms
)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    (void)password;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        password == NULL
        || timeout_ms == 0U
        || join_events == NULL
        || !atomic_load(&radio_started)
        || atomic_load(&join_in_progress)
        || atomic_load(&scan_in_progress)
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t password_length =
        strnlen(password, sizeof(((wifi_config_t *)0)->ap.password));
    if (password_length < 8U || password_length > 63U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < password_length; ++index) {
        if (password[index] < 0x20 || password[index] > 0x7e) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /*
     * A failed onboarding join already returned the AP+STA radio to AP mode.
     * Preserve that running AP and its connected setup client instead of
     * restarting Wi-Fi merely to satisfy the coordinator's fallback action.
     */
    if (atomic_exchange(&onboarding_hotspot_overlap, false)) {
        const esp_err_t overlap_status = esp_wifi_set_mode(WIFI_MODE_AP);
        if (overlap_status != ESP_OK) {
            publish_snapshot(
                REVLINK_WIFI_RADIO_FAILED,
                0U,
                INT8_MIN,
                6U,
                overlap_status
            );
            return overlap_status;
        }
        publish_snapshot(
            REVLINK_WIFI_RADIO_HOTSPOT_READY,
            0U,
            INT8_MIN,
            6U,
            ESP_OK
        );
        ESP_LOGI(
            TAG,
            "C6 onboarding fallback preserved the active setup hotspot"
        );
        return ESP_OK;
    }

    char ssid[sizeof(((wifi_config_t *)0)->ap.ssid)] = {0};
    char hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY] = {0};
    esp_err_t status = revlink_wifi_radio_local_identity(
        ssid,
        sizeof(ssid),
        hostname,
        sizeof(hostname)
    );
    if (status != ESP_OK) {
        return status;
    }
    const size_t ssid_length = strnlen(ssid, sizeof(ssid));

    wifi_config_t configuration = {0};
    memcpy(configuration.ap.ssid, ssid, ssid_length);
    configuration.ap.ssid_len = (uint8_t)ssid_length;
    memcpy(configuration.ap.password, password, password_length);
    configuration.ap.channel = 6U;
    configuration.ap.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.ap.max_connection = 4U;
    configuration.ap.ssid_hidden = 0U;
    configuration.ap.pmf_cfg.capable = true;
    configuration.ap.pmf_cfg.required = false;

    xEventGroupClearBits(
        join_events,
        REVLINK_WIFI_HOTSPOT_STARTED_BIT | REVLINK_WIFI_HOTSPOT_FAILED_BIT
    );
    publish_snapshot(
        REVLINK_WIFI_RADIO_HOTSPOT_STARTING,
        0U,
        INT8_MIN,
        6U,
        ESP_OK
    );
    ESP_LOGI(
        TAG,
        "C6 fallback hotspot start requested; identity and credential "
        "are suppressed"
    );
    status = restart_wifi(WIFI_MODE_AP, WIFI_IF_AP, &configuration);
    clear_sensitive(&configuration, sizeof(configuration));
    clear_sensitive(ssid, sizeof(ssid));
    clear_sensitive(hostname, sizeof(hostname));
    if (status != ESP_OK) {
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            6U,
            status
        );
        return status;
    }

    const EventBits_t result = xEventGroupWaitBits(
        join_events,
        REVLINK_WIFI_HOTSPOT_STARTED_BIT | REVLINK_WIFI_HOTSPOT_FAILED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );
    if ((result & REVLINK_WIFI_HOTSPOT_STARTED_BIT) == 0U) {
        status = ESP_ERR_TIMEOUT;
        publish_snapshot(
            REVLINK_WIFI_RADIO_FAILED,
            0U,
            INT8_MIN,
            6U,
            status
        );
        return status;
    }
    publish_snapshot(
        REVLINK_WIFI_RADIO_HOTSPOT_READY,
        0U,
        INT8_MIN,
        6U,
        ESP_OK
    );
    ESP_LOGI(TAG, "C6 fallback hotspot is ready on the private AP interface");
    return ESP_OK;
#endif
}

esp_err_t revlink_wifi_radio_local_identity(
    char *ssid,
    size_t ssid_capacity,
    char *hostname,
    size_t hostname_capacity
)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    (void)ssid;
    (void)ssid_capacity;
    (void)hostname;
    (void)hostname_capacity;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        ssid == NULL
        || hostname == NULL
        || ssid_capacity == 0U
        || hostname_capacity == 0U
        || !atomic_load(&radio_started)
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&snapshot_lock);
    if (local_identity_ready) {
        const int ssid_length =
            snprintf(ssid, ssid_capacity, "%s", local_identity_ssid);
        const int hostname_length = snprintf(
            hostname,
            hostname_capacity,
            "%s",
            local_identity_hostname
        );
        taskEXIT_CRITICAL(&snapshot_lock);
        return ssid_length > 0
            && hostname_length > 0
            && (size_t)ssid_length < ssid_capacity
            && (size_t)hostname_length < hostname_capacity
            ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    revlink_wifi_visible_network_t
        networks[REVLINK_WIFI_VISIBLE_NETWORK_LIMIT];
    const size_t network_count = visible_network_count;
    memcpy(networks, visible_networks, network_count * sizeof(*networks));
    taskEXIT_CRITICAL(&snapshot_lock);

    uint8_t mac[REVLINK_IDENTITY_MAC_BYTES] = {0};
    esp_err_t status = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (status != ESP_OK) {
        return status;
    }

    char candidate_ssid[REVLINK_IDENTITY_SSID_CAPACITY] = {0};
    char candidate_hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY] = {0};
    char mac_text[REVLINK_IDENTITY_MAC_TEXT_CAPACITY] = {0};
    taskENTER_CRITICAL(&snapshot_lock);
    uint16_t collision_index = preferred_collision_index;
    taskEXIT_CRITICAL(&snapshot_lock);
    bool available = false;
    while (!available && collision_index <= 99U) {
        if (!revlink_identity_format_local(
                mac,
                collision_index,
                candidate_ssid,
                sizeof(candidate_ssid),
                candidate_hostname,
                sizeof(candidate_hostname),
                mac_text,
                sizeof(mac_text)
            )) {
            return ESP_ERR_INVALID_SIZE;
        }
        available = true;
        for (size_t index = 0U; index < network_count; ++index) {
            if (strcmp(networks[index].ssid, candidate_ssid) == 0) {
                available = false;
                break;
            }
        }
        if (!available) {
            collision_index = collision_index == 0U
                ? 2U : (uint16_t)(collision_index + 1U);
        }
    }
    if (!available) {
        return ESP_ERR_NOT_FOUND;
    }

    taskENTER_CRITICAL(&snapshot_lock);
    if (!local_identity_ready) {
        memcpy(
            local_identity_ssid,
            candidate_ssid,
            sizeof(local_identity_ssid)
        );
        memcpy(
            local_identity_hostname,
            candidate_hostname,
            sizeof(local_identity_hostname)
        );
        local_identity_ready = true;
    }
    const int ssid_length =
        snprintf(ssid, ssid_capacity, "%s", local_identity_ssid);
    const int hostname_length = snprintf(
        hostname,
        hostname_capacity,
        "%s",
        local_identity_hostname
    );
    taskEXIT_CRITICAL(&snapshot_lock);
    return ssid_length > 0
        && hostname_length > 0
        && (size_t)ssid_length < ssid_capacity
        && (size_t)hostname_length < hostname_capacity
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
#endif
}

esp_err_t revlink_wifi_radio_set_local_collision_index(uint16_t index)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (index == 1U || index > 99U) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&snapshot_lock);
    if (local_identity_ready) {
        taskEXIT_CRITICAL(&snapshot_lock);
        return ESP_ERR_INVALID_STATE;
    }
    preferred_collision_index = index;
    taskEXIT_CRITICAL(&snapshot_lock);
    return ESP_OK;
#endif
}

esp_err_t revlink_wifi_radio_hotspot_ipv4(uint32_t *address)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    (void)address;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (address == NULL || hotspot_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t ip = {0};
    const esp_err_t status = esp_netif_get_ip_info(hotspot_netif, &ip);
    if (status != ESP_OK) {
        return status;
    }
    *address = ip.ip.addr;
    return ip.ip.addr == 0U ? ESP_ERR_INVALID_STATE : ESP_OK;
#endif
}

esp_err_t revlink_wifi_radio_stop(void)
{
#if !CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!atomic_load(&radio_started)
        || atomic_load(&scan_in_progress)
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
        || atomic_load(&join_in_progress)
#endif
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t status = esp_wifi_stop();
    if (status == ESP_OK || status == ESP_ERR_WIFI_NOT_STARTED) {
        atomic_store(&wifi_driver_started, false);
        atomic_store(&onboarding_hotspot_overlap, false);
        publish_snapshot(
            REVLINK_WIFI_RADIO_READY,
            0U,
            INT8_MIN,
            0U,
            ESP_OK
        );
        return ESP_OK;
    }
    publish_snapshot(
        REVLINK_WIFI_RADIO_FAILED,
        0U,
        INT8_MIN,
        0U,
        status
    );
    return status;
#endif
}

esp_err_t revlink_wifi_radio_probe_dns(void)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (revlink_wifi_radio_snapshot().state != REVLINK_WIFI_RADIO_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    const int status = getaddrinfo("example.com", NULL, &hints, &result);
    if (result != NULL) {
        freeaddrinfo(result);
    }
    if (status != 0) {
        ESP_LOGE(TAG, "Fixed DNS traffic probe failed");
        return ESP_FAIL;
    }
    ESP_LOGI(
        TAG,
        "C6 NETWORK ACCEPTANCE PASSED: association, DHCP, and DNS traffic"
    );
    return ESP_OK;
#endif
}

#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
typedef struct {
    SemaphoreHandle_t done;
    bool received_reply;
} revlink_gateway_probe_t;

static void gateway_probe_success(esp_ping_handle_t handle, void *context)
{
    (void)handle;
    revlink_gateway_probe_t *probe = context;
    probe->received_reply = true;
}

static void gateway_probe_timeout(esp_ping_handle_t handle, void *context)
{
    (void)handle;
    (void)context;
}

static void gateway_probe_finished(esp_ping_handle_t handle, void *context)
{
    (void)handle;
    revlink_gateway_probe_t *probe = context;
    if (probe != NULL && probe->done != NULL) {
        xSemaphoreGive(probe->done);
    }
}
#endif

esp_err_t revlink_wifi_radio_probe_gateway(uint32_t timeout_ms)
{
#if !CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (
        timeout_ms == 0U
        || station_netif == NULL
        || revlink_wifi_radio_snapshot().state
            != REVLINK_WIFI_RADIO_CONNECTED
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t access_point = {0};
    esp_netif_ip_info_t ip = {0};
    if (
        esp_wifi_sta_get_ap_info(&access_point) != ESP_OK
        || esp_netif_get_ip_info(station_netif, &ip) != ESP_OK
        || ip.ip.addr == 0U
        || ip.gw.addr == 0U
    ) {
        return ESP_FAIL;
    }

    revlink_gateway_probe_t probe = {
        .done = xSemaphoreCreateBinary(),
        .received_reply = false,
    };
    if (probe.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1U;
    config.interval_ms = 100U;
    config.timeout_ms = timeout_ms;
    config.data_size = 16U;
    ip_addr_set_ip4_u32_val(config.target_addr, ip.gw.addr);
    const esp_ping_callbacks_t callbacks = {
        .cb_args = &probe,
        .on_ping_success = gateway_probe_success,
        .on_ping_timeout = gateway_probe_timeout,
        .on_ping_end = gateway_probe_finished,
    };
    esp_ping_handle_t handle = NULL;
    esp_err_t status =
        esp_ping_new_session(&config, &callbacks, &handle);
    if (status == ESP_OK) {
        status = esp_ping_start(handle);
    }
    if (status == ESP_OK) {
        const TickType_t wait = pdMS_TO_TICKS(timeout_ms + 500U);
        if (xSemaphoreTake(probe.done, wait) != pdTRUE) {
            status = ESP_ERR_TIMEOUT;
        } else if (!probe.received_reply) {
            status = ESP_ERR_TIMEOUT;
        }
    }
    if (handle != NULL) {
        (void)esp_ping_stop(handle);
        (void)esp_ping_delete_session(handle);
    }
    vSemaphoreDelete(probe.done);
    return status;
#endif
}
