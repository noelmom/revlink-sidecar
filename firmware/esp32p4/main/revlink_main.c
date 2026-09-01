#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "revlink_accessport_usb.h"
#include "revlink_application.h"
#include "revlink_control_service.h"
#include "revlink_dev_console.h"
#include "revlink_network_runtime.h"
#include "revlink_onboarding.h"
#include "revlink_portal.h"
#include "revlink_local_metadata.h"
#include "revlink_map_upload.h"
#include "revlink_runtime.h"
#include "revlink_sd_storage.h"
#include "revlink_sidecar_identity.h"
#include "revlink_soft_power.h"
#include "revlink_status_oled.h"
#include "revlink_time.h"
#include "revlink_usb_link.h"
#include "revlink_wifi_radio.h"

static const char *TAG = "revlink_p4";
static revlink_application_t application;
static nvs_handle_t settings_handle;
static bool settings_ready;
static bool application_ready;
static revlink_control_service_t control_service;
static bool control_ready;
static atomic_bool shutdown_requested = ATOMIC_VAR_INIT(false);
static revlink_time_service_t trusted_time_service;
static SemaphoreHandle_t connected_accessport_mutex;
static bool connected_accessport_known;
static revlink_ap_device_info_t connected_accessport_identity;
#if CONFIG_REVLINK_USB_PC_MODE_EXIT_ACCEPTANCE
static bool pc_mode_exit_scheduled;
static void pc_mode_exit_acceptance_task(void *context);
#endif
#if CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE
static atomic_uint session_cycles_completed = ATOMIC_VAR_INIT(0U);
static atomic_bool session_cycle_reenumeration_seen =
    ATOMIC_VAR_INIT(false);
static atomic_bool session_cycle_repeat_scheduled =
    ATOMIC_VAR_INIT(false);
static void session_cycle_acceptance_task(void *context);
#endif
#if CONFIG_REVLINK_USB_CLOSE_RECOVERY_ACCEPTANCE
static atomic_bool close_recovery_failure_seen = ATOMIC_VAR_INIT(false);
#endif
static atomic_bool usb_link_recovery_scheduled = ATOMIC_VAR_INIT(false);
static void usb_link_recovery_task(void *context);
static atomic_bool auto_sync_retry_scheduled = ATOMIC_VAR_INIT(false);
static atomic_uint auto_sync_retry_generation = ATOMIC_VAR_INIT(0U);
static void auto_sync_retry_task(void *context);
static void schedule_auto_sync_retry(revlink_application_t *target);

#define REVLINK_SETTINGS_NAMESPACE "revlink"
#define REVLINK_AUTO_SYNC_KEY "auto_sync"
#define REVLINK_WRITE_CONSENT_KEY "write_ok"
#define REVLINK_MAP_AUTO_APPLY_KEY "map_auto"

static uint64_t monotonic_ms(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static uint64_t storage_trusted_time(void *context)
{
    return revlink_time_now(context, NULL);
}

static void repair_unknown_initial_sync_times(uint64_t utc_seconds)
{
    if (utc_seconds < REVLINK_TIME_MINIMUM_TRUSTED_UTC) return;

    revlink_sd_portal_snapshot_t *snapshot =
        calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) {
        ESP_LOGW(TAG, "Initial sync timestamp repair allocation failed");
        return;
    }
    if (revlink_sd_portal_snapshot(snapshot) != ESP_OK) {
        free(snapshot);
        return;
    }

    bool repair_needed = false;
    for (size_t index = 0U; index < snapshot->listed_files; ++index) {
        if (snapshot->files[index].initial_sync_utc == 0U) {
            repair_needed = true;
            break;
        }
    }
    free(snapshot);
    if (!repair_needed) return;

    size_t manifest_updated = 0U;
    size_t history_updated = 0U;
    const esp_err_t status =
        revlink_local_metadata_backfill_initial_sync(
            utc_seconds,
            &manifest_updated,
            &history_updated
        );
    if (status == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(
            TAG,
            "Initial sync timestamp repair deferred until storage is idle"
        );
        return;
    }
    if (status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Initial sync timestamp repair failed: %s",
            esp_err_to_name(status)
        );
        return;
    }
    ESP_LOGI(
        TAG,
        "Initial sync timestamps repaired from trusted UTC: "
        "current=%u history=%u",
        (unsigned int)manifest_updated,
        (unsigned int)history_updated
    );
}

#if CONFIG_REVLINK_ONBOARDING_ACCEPTANCE
static bool observe_local_client_time(void *context, uint64_t utc_seconds)
{
    revlink_time_service_t *service = context;
    revlink_time_source_t source = REVLINK_TIME_UNTRUSTED;
    const uint64_t current_utc = revlink_time_now(service, &source);
    if (current_utc != 0U) {
        repair_unknown_initial_sync_times(current_utc);
        return false;
    }
    const bool accepted = revlink_time_observe(
        service,
        REVLINK_TIME_CLIENT,
        utc_seconds
    );
    if (accepted) {
        ESP_LOGI(
            TAG,
            "Trusted UTC initialized from local portal client: %" PRIu64,
            utc_seconds
        );
        repair_unknown_initial_sync_times(utc_seconds);
    }
    return accepted;
}
#endif

static void restore_trusted_time_from_system_rtc(void)
{
    struct timeval time_value = {0};
    if (gettimeofday(&time_value, NULL) != 0 || time_value.tv_sec < 0) {
        ESP_LOGW(TAG, "System RTC could not be read");
        return;
    }
    if (!revlink_time_observe(
            &trusted_time_service,
            REVLINK_TIME_RTC,
            (uint64_t)time_value.tv_sec
        )) {
        ESP_LOGI(
            TAG,
            "System RTC has no trusted UTC yet; awaiting network time"
        );
        return;
    }
    ESP_LOGI(
        TAG,
        "Trusted UTC restored from system RTC: %" PRIu64,
        (uint64_t)time_value.tv_sec
    );
}

static void set_connected_accessport_identity(
    void *context,
    const revlink_ap_device_info_t *identity
)
{
    (void)context;
    if (
        connected_accessport_mutex != NULL
        && xSemaphoreTake(
               connected_accessport_mutex,
               pdMS_TO_TICKS(250)
           ) == pdTRUE
    ) {
        connected_accessport_known =
            identity != NULL && identity->serial[0] != '\0';
        connected_accessport_identity =
            connected_accessport_known
                ? *identity
                : (revlink_ap_device_info_t){0};
        xSemaphoreGive(connected_accessport_mutex);
    }
#if CONFIG_REVLINK_STATUS_OLED
    revlink_status_oled_set_accessport_identity(
        identity != NULL ? identity->vehicle : NULL,
        identity != NULL ? identity->part_number : NULL
    );
#endif
}

esp_err_t revlink_runtime_connected_accessport_snapshot(
    bool *known,
    revlink_ap_device_info_t *identity
)
{
    if (
        known == NULL || identity == NULL
        || connected_accessport_mutex == NULL
        || xSemaphoreTake(
               connected_accessport_mutex,
               pdMS_TO_TICKS(250)
           ) != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    *known = connected_accessport_known;
    *identity = connected_accessport_identity;
    xSemaphoreGive(connected_accessport_mutex);
    return ESP_OK;
}

#if CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
static void network_time_synchronized(struct timeval *time_value)
{
    if (time_value == NULL || time_value->tv_sec < 0) return;
    if (revlink_time_observe(
            &trusted_time_service,
            REVLINK_TIME_NETWORK,
            (uint64_t)time_value->tv_sec
        )) {
        ESP_LOGI(
            TAG,
            "Trusted UTC synchronized from network: %" PRIu64,
            (uint64_t)time_value->tv_sec
        );
        repair_unknown_initial_sync_times((uint64_t)time_value->tv_sec);
    }
}
#endif

static revlink_control_status_t map_sync_status(
    revlink_sync_status_t status
)
{
    switch (status) {
    case REVLINK_SYNC_OK:
        return REVLINK_CONTROL_OK;
    case REVLINK_SYNC_INVALID_ARGUMENT:
        return REVLINK_CONTROL_INVALID_ARGUMENT;
    case REVLINK_SYNC_INVALID_STATE:
        return REVLINK_CONTROL_INVALID_STATE;
    case REVLINK_SYNC_TRANSPORT_ERROR:
        return REVLINK_CONTROL_TRANSPORT_ERROR;
    default:
        return REVLINK_CONTROL_TRANSPORT_ERROR;
    }
}

static revlink_control_status_t read_control_snapshot(
    void *context,
    revlink_control_snapshot_t *snapshot
)
{
    (void)context;
    if (!application_ready || snapshot == NULL) {
        return REVLINK_CONTROL_INVALID_STATE;
    }
    *snapshot = (revlink_control_snapshot_t){
        .device = revlink_device_service_snapshot(
            &application.device_service
        ),
        .sync_policy = revlink_application_sync_policy(&application),
        .sync = revlink_application_sync_snapshot(&application),
        .writes_compiled =
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            true,
#else
            false,
#endif
        .deletes_compiled = false,
        .shutdown_requested = atomic_load(&shutdown_requested),
    };
    return REVLINK_CONTROL_OK;
}

static revlink_control_status_t control_set_auto_sync(
    void *context,
    bool enabled
)
{
    (void)context;
    return map_sync_status(revlink_runtime_set_auto_sync(enabled));
}

static revlink_control_status_t control_request_sync(void *context)
{
    (void)context;
    return map_sync_status(revlink_runtime_request_sync());
}

static revlink_control_status_t control_cancel_sync(void *context)
{
    (void)context;
    return map_sync_status(revlink_runtime_cancel_sync());
}

revlink_control_status_t revlink_runtime_control_execute(
    const revlink_control_request_t *request,
    revlink_control_response_t *response
)
{
    if (!control_ready) {
        return REVLINK_CONTROL_INVALID_STATE;
    }
    return revlink_control_service_execute(
        &control_service,
        request,
        response
    );
}

static revlink_sync_status_t request_usb_sync(void *context)
{
    (void)context;
    if (
        revlink_sd_storage_status().state
        != REVLINK_SD_STORAGE_MOUNTED
    ) {
        ESP_LOGW(
            TAG,
            "Sync rejected because local microSD storage is unavailable"
        );
        return REVLINK_SYNC_INVALID_STATE;
    }
    return revlink_accessport_usb_request_sync() == ESP_OK
        ? REVLINK_SYNC_OK
        : REVLINK_SYNC_TRANSPORT_ERROR;
}

static revlink_sync_status_t recover_usb_session(void *context)
{
    (void)context;
    return revlink_accessport_usb_request_close_recovery() == ESP_OK
        ? REVLINK_SYNC_OK
        : REVLINK_SYNC_TRANSPORT_ERROR;
}

static revlink_sync_status_t cancel_usb_sync(void *context)
{
    (void)context;
    return revlink_accessport_usb_cancel_sync() == ESP_OK
        ? REVLINK_SYNC_OK
        : REVLINK_SYNC_TRANSPORT_ERROR;
}

static void log_sync_state(
    void *context,
    const revlink_sync_snapshot_t *snapshot
)
{
    (void)context;
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    const bool transfer_active =
        snapshot->state == REVLINK_SYNC_QUEUED
        || snapshot->state == REVLINK_SYNC_RUNNING
        || snapshot->state == REVLINK_SYNC_CANCELLING;
    const bool transfer_terminal =
        snapshot->state == REVLINK_SYNC_COMPLETED
        || snapshot->state == REVLINK_SYNC_FAILED
        || snapshot->state == REVLINK_SYNC_CANCELLED;
    if (transfer_active || transfer_terminal) {
        const esp_err_t network_status =
            revlink_network_runtime_set_transfer_active(transfer_active);
        if (network_status != ESP_OK) {
            ESP_LOGW(
                TAG,
                "network transfer guard update failed: %s",
                esp_err_to_name(network_status)
            );
        }
    }
#endif
#if CONFIG_REVLINK_STATUS_OLED
    revlink_status_oled_update_sync(snapshot);
#endif
    ESP_LOGI(
        TAG,
        "sync state=%s candidates=%u downloaded=%u skipped=%u "
        "bytes=%" PRIu32 " pending=%u recovery=%s data_done=%s close_sent=%s "
        "ack_0x35=%s error=%d",
        revlink_sync_state_name(snapshot->state),
        (unsigned int)snapshot->candidates,
        (unsigned int)snapshot->downloaded,
        (unsigned int)snapshot->skipped,
        snapshot->downloaded_bytes,
        (unsigned int)snapshot->pending,
        snapshot->close_recovery_attempt ? "yes" : "no",
        snapshot->data_phase_completed ? "yes" : "no",
        snapshot->session_close_sent ? "yes" : "no",
        snapshot->session_close_acknowledged ? "yes" : "no",
        snapshot->last_platform_error
    );
}

static void handle_sync_event(
    void *context,
    const revlink_sync_event_t *event
)
{
    revlink_application_t *target = (revlink_application_t *)context;
    const revlink_sync_status_t status =
        revlink_application_handle_sync_event(target, event);
    if (status != REVLINK_SYNC_OK) {
        ESP_LOGE(TAG, "sync event=%d rejected status=%d", event->kind, status);
    }
    const bool recovery_queued =
        status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_FAILED
        && !event->close_recovery_attempt
        && event->data_phase_completed
        && !event->session_close_acknowledged
        && revlink_application_sync_snapshot(target).state
            == REVLINK_SYNC_QUEUED;
    if (recovery_queued) {
        ESP_LOGW(
            TAG,
            "unclean read-only close detected; one bounded initialized "
            "session-close recovery queued"
        );
    }
    if (status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_FAILED
        && revlink_application_auto_sync_retry_needed(target)) {
        schedule_auto_sync_retry(target);
    }

    const bool continuation_needed =
        status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_COMPLETED
        && !event->close_recovery_attempt
        && event->data_phase_completed
        && event->session_close_sent
        && event->session_close_acknowledged
        && event->pending > 0U
        && event->downloaded > 0U
        && !atomic_load(&shutdown_requested);
    if (continuation_needed) {
        const revlink_sync_status_t continuation_status =
            revlink_application_request_sync(target);
        if (continuation_status == REVLINK_SYNC_OK) {
            ESP_LOGI(
                TAG,
                "bounded sync batch completed with %u files pending; "
                "continuation queued",
                (unsigned int)event->pending
            );
        } else {
            ESP_LOGW(
                TAG,
                "bounded sync batch left %u files pending but continuation "
                "could not be queued: status=%d",
                (unsigned int)event->pending,
                continuation_status
            );
        }
    }

#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    /*
     * A staged map is written only once the read-only inventory is fully
     * synchronized: after a clean acknowledged close, with nothing pending,
     * and with no continuation batch queued. Every remaining gate — owner
     * consent, the auto-apply preference, and the pinned AccessPort identity
     * — is evaluated inside the write service.
     */
    if (status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_COMPLETED
        && !continuation_needed
        && !atomic_load(&shutdown_requested)) {
        bool identified = false;
        revlink_ap_device_info_t attached = {0};
        if (revlink_runtime_connected_accessport_snapshot(
                &identified,
                &attached
            ) == ESP_OK
            && identified) {
            revlink_staged_map_apply_decision_t decision =
                REVLINK_STAGED_MAP_APPLY_NOTHING_STAGED;
            const esp_err_t applied = revlink_map_upload_auto_apply(
                &attached,
                event->data_phase_completed
                    && event->session_close_acknowledged,
                event->pending,
                &decision
            );
            if (applied == ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "staged map write started after attach-time sync"
                );
            }
        }
    }
#endif

    const bool unclean_terminal_close =
        status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_FAILED
        && event->data_phase_completed
        && !event->session_close_acknowledged;
    if (unclean_terminal_close
        && !atomic_exchange(&usb_link_recovery_scheduled, true)) {
        ESP_LOGW(
            TAG,
            "verified backup ended with an unclean AccessPort close; "
            "bounded logical USB recovery scheduled"
        );
        const BaseType_t created = xTaskCreate(
            usb_link_recovery_task,
            "revlink_usb_recover",
            3072,
            NULL,
            5,
            NULL
        );
        if (created != pdPASS) {
            atomic_store(&usb_link_recovery_scheduled, false);
            ESP_LOGE(TAG, "unable to create logical USB recovery task");
        }
    }
#if CONFIG_REVLINK_USB_CLOSE_RECOVERY_ACCEPTANCE
    if (recovery_queued) {
        atomic_store(&close_recovery_failure_seen, true);
    } else if (status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_COMPLETED
        && event->close_recovery_attempt
        && atomic_load(&close_recovery_failure_seen)
        && event->session_close_sent
        && event->session_close_acknowledged) {
        ESP_LOGW(
            TAG,
            "CLOSE-RECOVERY ACCEPTANCE PASSED: injected unclean close "
            "recovered by one bounded acknowledged session close"
        );
    }
#endif

#if CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE
    if (status == REVLINK_SYNC_OK
        && event->kind == REVLINK_SYNC_EVENT_COMPLETED) {
        const unsigned int completed =
            atomic_fetch_add(&session_cycles_completed, 1U) + 1U;
        ESP_LOGW(
            TAG,
            "SESSION-CYCLE ACCEPTANCE: clean cycle %u/%u; "
            "close_sent=%s ack_0x35=%s",
            completed,
            CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE_COUNT,
            event->session_close_sent ? "yes" : "no",
            event->session_close_acknowledged ? "yes" : "no"
        );
    } else if (status == REVLINK_SYNC_OK
        && (event->kind == REVLINK_SYNC_EVENT_FAILED
            || event->kind == REVLINK_SYNC_EVENT_CANCELLED)) {
        ESP_LOGE(
            TAG,
            "SESSION-CYCLE ACCEPTANCE FAILED: terminal event=%d "
            "close_sent=%s ack_0x35=%s error=%d recovery_queued=%s",
            event->kind,
            event->session_close_sent ? "yes" : "no",
            event->session_close_acknowledged ? "yes" : "no",
            event->platform_error,
            recovery_queued ? "yes" : "no"
        );
    }
#endif

#if CONFIG_REVLINK_USB_PC_MODE_EXIT_ACCEPTANCE
    const bool terminal =
        event->kind == REVLINK_SYNC_EVENT_COMPLETED
        || event->kind == REVLINK_SYNC_EVENT_FAILED
        || event->kind == REVLINK_SYNC_EVENT_CANCELLED;
    if (terminal && !pc_mode_exit_scheduled) {
        pc_mode_exit_scheduled = true;
        ESP_LOGW(
            TAG,
            "PC-MODE EXIT TEST: terminal sync event=%d received after "
            "session cleanup; logical root-port release scheduled",
            event->kind
        );
        const BaseType_t created = xTaskCreate(
            pc_mode_exit_acceptance_task,
            "revlink_pc_exit",
            3072,
            NULL,
            5,
            NULL
        );
        if (created != pdPASS) {
            ESP_LOGE(
                TAG,
                "PC-MODE EXIT TEST: unable to create root release task"
            );
        }
    }
#endif
}

#define REVLINK_AUTO_SYNC_RETRY_DELAY_MS 1500U

static void schedule_auto_sync_retry(revlink_application_t *target)
{
    if (target == NULL
        || !revlink_application_auto_sync_retry_needed(target)
        || atomic_exchange(&auto_sync_retry_scheduled, true)) {
        return;
    }
    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&target->device_service);
    atomic_store(
        &auto_sync_retry_generation,
        device.identity.attachment_generation
    );
    const BaseType_t created = xTaskCreate(
        auto_sync_retry_task,
        "revlink_autoretry",
        3072,
        target,
        5,
        NULL
    );
    if (created != pdPASS) {
        atomic_store(&auto_sync_retry_scheduled, false);
        ESP_LOGE(TAG, "unable to create bounded auto-sync retry task");
        return;
    }
    ESP_LOGW(
        TAG,
        "attach-time auto-sync was not ready; one retry scheduled in %u ms",
        REVLINK_AUTO_SYNC_RETRY_DELAY_MS
    );
}

static void auto_sync_retry_task(void *context)
{
    revlink_application_t *target = (revlink_application_t *)context;
    const unsigned int expected_generation =
        atomic_load(&auto_sync_retry_generation);
    vTaskDelay(pdMS_TO_TICKS(REVLINK_AUTO_SYNC_RETRY_DELAY_MS));

    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&target->device_service);
    revlink_sync_status_t status = REVLINK_SYNC_INVALID_STATE;
    if (!atomic_load(&shutdown_requested)
        && device.identity.attachment_generation == expected_generation) {
        status = revlink_application_retry_auto_sync(target);
    }
    atomic_store(&auto_sync_retry_scheduled, false);

    if (status == REVLINK_SYNC_OK) {
        ESP_LOGI(TAG, "bounded attach-time auto-sync retry queued");
    } else if (device.identity.attachment_generation == expected_generation) {
        ESP_LOGW(
            TAG,
            "bounded attach-time auto-sync retry was not accepted: status=%d",
            status
        );
    } else if (!atomic_load(&shutdown_requested)
        && revlink_application_auto_sync_retry_needed(target)) {
        /* A new attachment arrived while the old delayed task was pending. */
        schedule_auto_sync_retry(target);
    }
    vTaskDelete(NULL);
}

static void usb_link_recovery_task(void *context)
{
    (void)context;

    /*
     * A clean 0x05 close normally re-enumerates in under one second. Give
     * that path—and the application's single initialized close retry—time to
     * finish before touching the logical root port.
     */
    vTaskDelay(pdMS_TO_TICKS(1800));
    for (unsigned int check = 0U; check < 8U; ++check) {
        const revlink_sync_state_t sync_state =
            revlink_application_sync_snapshot(&application).state;
        if (sync_state != REVLINK_SYNC_QUEUED
            && sync_state != REVLINK_SYNC_RUNNING
            && sync_state != REVLINK_SYNC_CANCELLING) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&application.device_service);
    if (device.state == REVLINK_DEVICE_AVAILABLE) {
        ESP_LOGI(
            TAG,
            "AccessPort re-enumerated before logical USB recovery was needed"
        );
        atomic_store(&usb_link_recovery_scheduled, false);
        vTaskDelete(NULL);
        return;
    }

    const esp_err_t stop_status =
        revlink_accessport_usb_set_root_port_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(150));
    const esp_err_t start_status =
        stop_status == ESP_OK
            ? revlink_accessport_usb_set_root_port_enabled(true)
            : stop_status;
    if (stop_status == ESP_OK && start_status == ESP_OK) {
        ESP_LOGI(
            TAG,
            "unclean-close logical USB recovery completed; "
            "awaiting fresh AccessPort enumeration"
        );
    } else {
        ESP_LOGE(
            TAG,
            "unclean-close logical USB recovery failed: stop=%s start=%s",
            esp_err_to_name(stop_status),
            esp_err_to_name(start_status)
        );
    }
    atomic_store(&usb_link_recovery_scheduled, false);
    vTaskDelete(NULL);
}

#if CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE
static void session_cycle_acceptance_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(300));
    const revlink_sync_status_t status = revlink_runtime_request_sync();
    atomic_store(&session_cycle_repeat_scheduled, false);
    if (status != REVLINK_SYNC_OK) {
        ESP_LOGE(
            TAG,
            "SESSION-CYCLE ACCEPTANCE FAILED: unable to queue next cycle "
            "status=%d",
            status
        );
    }
    vTaskDelete(NULL);
}
#endif

#if CONFIG_REVLINK_USB_PC_MODE_EXIT_ACCEPTANCE
static void pc_mode_exit_acceptance_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(250));
    const esp_err_t release_status =
        revlink_accessport_usb_set_root_port_enabled(false);
    ESP_LOGW(
        TAG,
        "PC-MODE EXIT TEST COMPLETE: logical root-port stop=%s; "
        "observe whether Gauges returned without unplugging",
        esp_err_to_name(release_status)
    );
    vTaskDelete(NULL);
}
#endif

static revlink_sync_policy_t load_sync_policy(void)
{
    revlink_sync_policy_t policy = {
        .auto_sync_on_attach =
#if CONFIG_REVLINK_SYNC_AUTO_ON_ATTACH_DEFAULT
            true,
#else
            false,
#endif
    };

    const esp_err_t init_status = nvs_flash_init();
    if (init_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "settings unavailable; sync default retained: %s",
            esp_err_to_name(init_status)
        );
        return policy;
    }
    const esp_err_t open_status = nvs_open(
        REVLINK_SETTINGS_NAMESPACE,
        NVS_READWRITE,
        &settings_handle
    );
    if (open_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "settings namespace unavailable; sync default retained: %s",
            esp_err_to_name(open_status)
        );
        return policy;
    }
    settings_ready = true;

    uint8_t stored = 0U;
    const esp_err_t read_status =
        nvs_get_u8(settings_handle, REVLINK_AUTO_SYNC_KEY, &stored);
    if (read_status == ESP_OK) {
        policy.auto_sync_on_attach = stored != 0U;
    } else if (read_status != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(
            TAG,
            "auto-sync setting unreadable; default retained: %s",
            esp_err_to_name(read_status)
        );
    }
    return policy;
}

static bool load_map_auto_apply(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!settings_ready) return false;

    uint8_t stored = 0U;
    const esp_err_t read_status = nvs_get_u8(
        settings_handle,
        REVLINK_MAP_AUTO_APPLY_KEY,
        &stored
    );
    if (read_status == ESP_OK) return stored != 0U;
    if (read_status != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(
            TAG,
            "auto-apply setting unreadable; staged maps stay manual: %s",
            esp_err_to_name(read_status)
        );
        return false;
    }
    /* Never set: fall back to the build default. */
#ifdef CONFIG_REVLINK_MAP_AUTO_APPLY_DEFAULT
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

static bool load_write_consent(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!settings_ready) return false;

    uint8_t stored = 0U;
    const esp_err_t read_status = nvs_get_u8(
        settings_handle,
        REVLINK_WRITE_CONSENT_KEY,
        &stored
    );
    if (read_status == ESP_OK) return stored != 0U;
    if (read_status != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(
            TAG,
            "write-consent setting unreadable; writes remain locked: %s",
            esp_err_to_name(read_status)
        );
    }
#endif
    return false;
}

revlink_sync_status_t revlink_runtime_set_auto_sync(bool enabled)
{
    if (!application_ready || !settings_ready
        || atomic_load(&shutdown_requested)) {
        return REVLINK_SYNC_INVALID_STATE;
    }
    const esp_err_t set_status = nvs_set_u8(
        settings_handle,
        REVLINK_AUTO_SYNC_KEY,
        enabled ? 1U : 0U
    );
    if (set_status != ESP_OK || nvs_commit(settings_handle) != ESP_OK) {
        return REVLINK_SYNC_TRANSPORT_ERROR;
    }
    const revlink_sync_policy_t policy = {
        .auto_sync_on_attach = enabled,
    };
    return revlink_application_set_sync_policy(&application, &policy);
}

esp_err_t revlink_runtime_set_write_consent(bool enabled)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!settings_ready || atomic_load(&shutdown_requested)) {
        return ESP_ERR_INVALID_STATE;
    }

    revlink_map_upload_snapshot_t previous = {0};
    const esp_err_t snapshot_status =
        revlink_map_upload_snapshot(&previous);
    if (snapshot_status != ESP_OK) return snapshot_status;
    const esp_err_t consent_status =
        revlink_map_upload_set_consent(enabled);
    if (consent_status != ESP_OK) return consent_status;

    const esp_err_t set_status = nvs_set_u8(
        settings_handle,
        REVLINK_WRITE_CONSENT_KEY,
        enabled ? 1U : 0U
    );
    const esp_err_t commit_status =
        set_status == ESP_OK ? nvs_commit(settings_handle) : set_status;
    if (set_status == ESP_OK && commit_status == ESP_OK) return ESP_OK;

    (void)revlink_map_upload_set_consent(previous.consent_enabled);
    (void)nvs_set_u8(
        settings_handle,
        REVLINK_WRITE_CONSENT_KEY,
        previous.consent_enabled ? 1U : 0U
    );
    (void)nvs_commit(settings_handle);
    return set_status != ESP_OK ? set_status : commit_status;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_runtime_set_map_auto_apply(bool enabled)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!settings_ready || atomic_load(&shutdown_requested)) {
        return ESP_ERR_INVALID_STATE;
    }

    revlink_map_upload_snapshot_t previous = {0};
    const esp_err_t snapshot_status =
        revlink_map_upload_snapshot(&previous);
    if (snapshot_status != ESP_OK) return snapshot_status;
    const esp_err_t apply_status =
        revlink_map_upload_set_auto_apply(enabled);
    if (apply_status != ESP_OK) return apply_status;

    const esp_err_t set_status = nvs_set_u8(
        settings_handle,
        REVLINK_MAP_AUTO_APPLY_KEY,
        enabled ? 1U : 0U
    );
    const esp_err_t commit_status =
        set_status == ESP_OK ? nvs_commit(settings_handle) : set_status;
    if (set_status == ESP_OK && commit_status == ESP_OK) return ESP_OK;

    (void)revlink_map_upload_set_auto_apply(previous.auto_apply_enabled);
    return set_status != ESP_OK ? set_status : commit_status;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

revlink_sync_status_t revlink_runtime_request_sync(void)
{
    return application_ready && !atomic_load(&shutdown_requested)
        && revlink_sd_storage_status().state
            == REVLINK_SD_STORAGE_MOUNTED
        ? revlink_application_request_sync(&application)
        : REVLINK_SYNC_INVALID_STATE;
}

revlink_sync_status_t revlink_runtime_cancel_sync(void)
{
    return application_ready
        ? revlink_application_cancel_sync(&application)
        : REVLINK_SYNC_INVALID_STATE;
}

revlink_sync_status_t revlink_runtime_prepare_shutdown(void)
{
    if (!application_ready) {
        return REVLINK_SYNC_INVALID_STATE;
    }
    const bool already_requested =
        atomic_exchange(&shutdown_requested, true);
    if (!already_requested) {
        const revlink_sync_policy_t disabled_policy = {
            .auto_sync_on_attach = false,
        };
        const revlink_sync_status_t policy_status =
            revlink_application_set_sync_policy(
                &application,
                &disabled_policy
            );
        if (policy_status != REVLINK_SYNC_OK) {
            atomic_store(&shutdown_requested, false);
            return policy_status;
        }
    }

    const revlink_sync_state_t state =
        revlink_application_sync_snapshot(&application).state;
    if (state == REVLINK_SYNC_QUEUED || state == REVLINK_SYNC_RUNNING) {
        return revlink_application_cancel_sync(&application);
    }
    return REVLINK_SYNC_OK;
}

revlink_sync_policy_t revlink_runtime_sync_policy(void)
{
    return application_ready
        ? revlink_application_sync_policy(&application)
        : (revlink_sync_policy_t){0};
}

revlink_sync_snapshot_t revlink_runtime_sync_snapshot(void)
{
    return application_ready
        ? revlink_application_sync_snapshot(&application)
        : (revlink_sync_snapshot_t){0};
}

#if CONFIG_REVLINK_USB_IDLE_RELEASE_ACCEPTANCE
static bool idle_release_scheduled;

static void idle_release_acceptance_task(void *context)
{
    (void)context;
    ESP_LOGW(
        TAG,
        "IDLE RELEASE TEST: logical root port will stop in five seconds"
    );
    vTaskDelay(pdMS_TO_TICKS(5000));
    const esp_err_t status =
        revlink_accessport_usb_set_root_port_enabled(false);
    ESP_LOGW(
        TAG,
        "IDLE RELEASE TEST COMPLETE: logical port stop=%s; "
        "observe whether Gauges returned",
        esp_err_to_name(status)
    );
    vTaskDelete(NULL);
}
#endif

#define REVLINK_STORAGE_START_ATTEMPTS 3U

static esp_err_t start_storage_with_retry(void)
{
    esp_err_t status = ESP_FAIL;
    for (
        unsigned int attempt = 1U;
        attempt <= REVLINK_STORAGE_START_ATTEMPTS;
        ++attempt
    ) {
        status = revlink_sd_storage_start();
        if (status == ESP_OK) {
            if (attempt > 1U) {
                ESP_LOGI(
                    TAG,
                    "microSD recovered on startup attempt %u/%u",
                    attempt,
                    REVLINK_STORAGE_START_ATTEMPTS
                );
            }
            return ESP_OK;
        }
        if (attempt < REVLINK_STORAGE_START_ATTEMPTS) {
            ESP_LOGW(
                TAG,
                "microSD startup attempt %u/%u failed (%s); retrying",
                attempt,
                REVLINK_STORAGE_START_ATTEMPTS,
                esp_err_to_name(status)
            );
            vTaskDelay(pdMS_TO_TICKS(750));
        }
    }
    return status;
}

#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
#define WRITE_CAPABILITY_TEXT "available (runtime consent still required)"
#define BUILD_ALLOW_DEVICE_WRITES true
#else
#define WRITE_CAPABILITY_TEXT "disabled"
#define BUILD_ALLOW_DEVICE_WRITES false
#endif

#if CONFIG_REVLINK_USB_ROOT_PORT_POWER
#define ROOT_PORT_POWER_TEXT "enabled"
#else
#define ROOT_PORT_POWER_TEXT "disabled"
#endif

#if CONFIG_REVLINK_USB_INTERFACE_CLAIM_ACCEPTANCE
#define INTERFACE_ACCEPTANCE_TEXT "enabled (zero transfers)"
#else
#define INTERFACE_ACCEPTANCE_TEXT "disabled"
#endif

#if CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE
#define ROOT_LIST_ACCEPTANCE_TEXT \
    "enabled (one bounded request per physical attachment)"
#else
#define ROOT_LIST_ACCEPTANCE_TEXT "disabled"
#endif

#if CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE
#define DOWNLOAD_ACCEPTANCE_TEXT \
    "enabled (one bounded cache per physical attachment)"
#else
#define DOWNLOAD_ACCEPTANCE_TEXT "disabled"
#endif

#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE
#define INCREMENTAL_SYNC_ACCEPTANCE_TEXT \
    "enabled (four files / 16 MiB maximum)"
#else
#define INCREMENTAL_SYNC_ACCEPTANCE_TEXT "disabled"
#endif

static void log_device_state(
    void *context,
    const revlink_device_snapshot_t *snapshot
)
{
    (void)context;
#if CONFIG_REVLINK_STATUS_OLED
    revlink_status_oled_update_device(snapshot);
#endif
    ESP_LOGI(
        TAG,
        "device state=%s vid=0x%04x pid=0x%04x address=%u "
        "high_speed=%s bulk_mps=%u",
        revlink_device_state_name(snapshot->state),
        snapshot->identity.vendor_id,
        snapshot->identity.product_id,
        snapshot->identity.address,
        snapshot->identity.high_speed ? "yes" : "no",
        snapshot->identity.bulk_max_packet_size
    );

#if CONFIG_REVLINK_USB_IDLE_RELEASE_ACCEPTANCE
    if (
        snapshot->state == REVLINK_DEVICE_AVAILABLE
        && !idle_release_scheduled
    ) {
        idle_release_scheduled = true;
        const BaseType_t created = xTaskCreate(
            idle_release_acceptance_task,
            "revlink_idle_release",
            3072,
            NULL,
            5,
            NULL
        );
        if (created != pdPASS) {
            ESP_LOGE(TAG, "IDLE RELEASE TEST: unable to create release task");
        }
    }
#endif
}

static void handle_usb_event(
    void *context,
    const revlink_device_event_t *event
)
{
    /*
     * A cached dataset is not proof of which AccessPort is physically
     * connected. Clear the live identity at a genuine attach/detach boundary
     * and preserve it only through the expected software re-enumeration after
     * a polite session close.
     */
    if (
        (
            event->kind == REVLINK_DEVICE_EVENT_ATTACHED
            || event->kind == REVLINK_DEVICE_EVENT_DETACHED
        )
        && !event->software_reenumeration
    ) {
        set_connected_accessport_identity(NULL, NULL);
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
        /*
         * A genuine attach/detach boundary starts a new automatic-write
         * opportunity. The software re-enumeration that follows a polite
         * session close deliberately does not, so a single physical attach
         * gets exactly one automatic attempt.
         */
        revlink_map_upload_notify_attach();
#endif
    }
#if CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE
    if (event->kind == REVLINK_DEVICE_EVENT_ATTACHED
        && event->software_reenumeration) {
        atomic_store(&session_cycle_reenumeration_seen, true);
    }
#endif
    revlink_application_t *target = (revlink_application_t *)context;
    const revlink_core_status_t status =
        revlink_application_handle_device_event(target, event);
    if (status != REVLINK_CORE_OK) {
        ESP_LOGE(
            TAG,
            "device event=%d rejected: %s",
            event->kind,
            revlink_core_status_name(status)
        );
    }
    if (status == REVLINK_CORE_OK
        && event->kind == REVLINK_DEVICE_EVENT_ACCEPTED
        && revlink_application_auto_sync_retry_needed(target)) {
        schedule_auto_sync_retry(target);
    }

#if CONFIG_REVLINK_RUNTIME_SYNC
    if (
        status == REVLINK_CORE_OK
        && event->kind == REVLINK_DEVICE_EVENT_ACCEPTED
        && !event->software_reenumeration
    ) {
        const revlink_sync_state_t sync_state =
            revlink_application_sync_snapshot(target).state;
        if (
            sync_state != REVLINK_SYNC_QUEUED
            && sync_state != REVLINK_SYNC_RUNNING
            && sync_state != REVLINK_SYNC_CANCELLING
        ) {
            const esp_err_t identity_status =
                revlink_accessport_usb_request_identity();
            if (identity_status != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Unable to queue attached AccessPort identity: %s",
                    esp_err_to_name(identity_status)
                );
            }
        }
    }
#endif

#if CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE
    if (status == REVLINK_CORE_OK
        && event->kind == REVLINK_DEVICE_EVENT_ACCEPTED
        && atomic_exchange(
            &session_cycle_reenumeration_seen,
            false
        )) {
        const unsigned int completed =
            atomic_load(&session_cycles_completed);
        if (completed
            >= CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE_COUNT) {
            ESP_LOGW(
                TAG,
                "SESSION-CYCLE ACCEPTANCE PASSED: %u/%u acknowledged "
                "cycles returned through software re-enumeration; "
                "no further sync scheduled",
                completed,
                CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE_COUNT
            );
        } else if (!atomic_exchange(
            &session_cycle_repeat_scheduled,
            true
        )) {
            const BaseType_t created = xTaskCreate(
                session_cycle_acceptance_task,
                "revlink_cycle",
                3072,
                NULL,
                5,
                NULL
            );
            if (created != pdPASS) {
                atomic_store(&session_cycle_repeat_scheduled, false);
                ESP_LOGE(
                    TAG,
                    "SESSION-CYCLE ACCEPTANCE FAILED: unable to create "
                    "next-cycle task"
                );
            }
        }
    }
#endif
}

void app_main(void)
{
    esp_chip_info_t chip_info = {0};
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read flash size");
    }

    const unsigned int psram_size =
        (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(
        TAG,
        "RevLink ESP32-P4 acceptance: cores=%u revision=%u "
        "flash=%" PRIu32 " bytes psram=%u bytes",
        (unsigned int)chip_info.cores,
        (unsigned int)chip_info.revision,
        flash_size,
        psram_size
    );

    if (flash_size != 16U * 1024U * 1024U) {
        ESP_LOGE(TAG, "Hardware gate FAILED: expected 16 MB flash");
        return;
    }
    if (psram_size != 32U * 1024U * 1024U) {
        ESP_LOGE(TAG, "Hardware gate FAILED: expected 32 MB PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Hardware memory gate PASSED");

    const revlink_time_config_t time_config = {
        .context = NULL,
        .monotonic_ms = monotonic_ms,
    };
    if (!revlink_time_init(&trusted_time_service, &time_config)) {
        ESP_LOGE(TAG, "Trusted-time service initialization FAILED");
        return;
    }
    restore_trusted_time_from_system_rtc();
    connected_accessport_mutex = xSemaphoreCreateMutex();
    if (connected_accessport_mutex == NULL) {
        ESP_LOGE(TAG, "Connected AccessPort identity mutex allocation failed");
        return;
    }
    revlink_sd_storage_configure_time_source(
        &trusted_time_service,
        storage_trusted_time
    );
#if CONFIG_REVLINK_ONBOARDING_ACCEPTANCE
    revlink_portal_configure_time_observer(
        &trusted_time_service,
        observe_local_client_time
    );
#endif

#if CONFIG_REVLINK_STATUS_OLED
    const esp_err_t display_status = revlink_status_oled_start();
    if (display_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Optional status display unavailable: %s",
            esp_err_to_name(display_status)
        );
    }
#endif

    revlink_sync_policy_t sync_policy = load_sync_policy();
#if CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE
    sync_policy.auto_sync_on_attach = true;
    ESP_LOGW(
        TAG,
        "SESSION-CYCLE ACCEPTANCE: forcing one initial auto-sync and %u "
        "total acknowledged cycles regardless of saved policy",
        CONFIG_REVLINK_USB_SESSION_CYCLE_ACCEPTANCE_COUNT
    );
#endif
#if CONFIG_REVLINK_USB_PC_MODE_EXIT_ACCEPTANCE
    sync_policy.auto_sync_on_attach = true;
    ESP_LOGW(
        TAG,
        "PC-MODE EXIT TEST: forcing one auto-sync regardless of saved policy"
    );
#endif
#if CONFIG_REVLINK_USB_CLOSE_RECOVERY_ACCEPTANCE
    sync_policy.auto_sync_on_attach = true;
    ESP_LOGW(
        TAG,
        "CLOSE-RECOVERY ACCEPTANCE: forcing one initial read-only sync; "
        "writes remain disabled"
    );
#endif
    const revlink_application_config_t application_config = {
        .allow_device_writes = BUILD_ALLOW_DEVICE_WRITES,
        .allow_device_deletes = false,
        .state_observer = log_device_state,
        .sync_policy = sync_policy,
        .sync_request = request_usb_sync,
        .sync_recover_session = recover_usb_session,
        .sync_cancel = cancel_usb_sync,
        .sync_observer = log_sync_state,
        .retry_unclean_readonly_close_once = true,
    };
    if (
        revlink_application_init(&application, &application_config)
            != REVLINK_CORE_OK
        || !revlink_application_protocol_self_test()
    ) {
        ESP_LOGE(TAG, "AccessPort offline protocol self-test FAILED");
        return;
    }
    ESP_LOGI(TAG, "AccessPort offline protocol self-test PASSED");
    application_ready = true;
#if CONFIG_REVLINK_STATUS_OLED
    revlink_status_oled_boot_complete();
#endif

    const revlink_control_service_config_t control_config = {
        .context = NULL,
        .read_snapshot = read_control_snapshot,
        .set_auto_sync = control_set_auto_sync,
        .request_sync = control_request_sync,
        .cancel_sync = control_cancel_sync,
    };
    if (
        revlink_control_service_init(&control_service, &control_config)
            != REVLINK_CONTROL_OK
    ) {
        ESP_LOGE(TAG, "Local control service initialization FAILED");
        return;
    }
    control_ready = true;
    ESP_LOGI(
        TAG,
        "Local control service ready: status, auto-sync, sync, cancel"
    );
#if CONFIG_REVLINK_DEV_CONSOLE
    const esp_err_t console_status = revlink_dev_console_start();
    if (console_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Optional development console unavailable: %s",
            esp_err_to_name(console_status)
        );
    }
#endif
    ESP_LOGI(TAG, "AccessPort storage-write capability: %s", WRITE_CAPABILITY_TEXT);
    ESP_LOGI(TAG, "High-speed USB root-port power: %s", ROOT_PORT_POWER_TEXT);
    ESP_LOGI(
        TAG,
        "USB interface claim/release acceptance: %s",
        INTERFACE_ACCEPTANCE_TEXT
    );
    ESP_LOGI(
        TAG,
        "USB root-list read-only acceptance: %s",
        ROOT_LIST_ACCEPTANCE_TEXT
    );
    ESP_LOGI(
        TAG,
        "USB datalog download acceptance: %s",
        DOWNLOAD_ACCEPTANCE_TEXT
    );
    ESP_LOGI(
        TAG,
        "USB incremental sync acceptance: %s",
        INCREMENTAL_SYNC_ACCEPTANCE_TEXT
    );
    ESP_LOGI(
        TAG,
        "Runtime sync coordinator: %s; auto-sync on attach=%s",
#if CONFIG_REVLINK_RUNTIME_SYNC
        "enabled",
#else
        "disabled",
#endif
        sync_policy.auto_sync_on_attach ? "enabled" : "disabled"
    );

    const esp_err_t storage_status = start_storage_with_retry();
    if (storage_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "microSD unavailable after retries: %s; continuing in "
            "degraded portal mode so recovery controls remain reachable",
            esp_err_to_name(storage_status)
        );
#if CONFIG_REVLINK_STATUS_OLED
        const revlink_sd_storage_state_t state =
            revlink_sd_storage_status().state;
        revlink_status_oled_show_storage_error(
            state == REVLINK_SD_STORAGE_MISSING
                ? REVLINK_OLED_STORAGE_MISSING
                : state == REVLINK_SD_STORAGE_UNREADABLE
                    ? REVLINK_OLED_STORAGE_UNREADABLE
                    : REVLINK_OLED_STORAGE_ERROR
        );
#endif
    }
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (storage_status == ESP_OK) {
        const esp_err_t upload_status = revlink_map_upload_start();
        if (upload_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Guarded map-write service failed to start: %s",
                esp_err_to_name(upload_status)
            );
            return;
        }
        const bool saved_write_consent = load_write_consent();
        const esp_err_t consent_status =
            revlink_map_upload_set_consent(saved_write_consent);
        if (consent_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Unable to restore write-consent setting: %s",
                esp_err_to_name(consent_status)
            );
            return;
        }
        const esp_err_t auto_apply_status =
            revlink_map_upload_set_auto_apply(load_map_auto_apply());
        if (auto_apply_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Unable to restore staged-map auto-apply setting: %s",
                esp_err_to_name(auto_apply_status)
            );
            return;
        }
    } else {
        ESP_LOGW(
            TAG,
            "Guarded map-write service is disabled until microSD recovery"
        );
    }
#endif
    const uint64_t restored_utc =
        revlink_time_now(&trusted_time_service, NULL);
    if (restored_utc != 0U) {
        repair_unknown_initial_sync_times(restored_utc);
    }

#if CONFIG_REVLINK_WIFI_SCAN_ACCEPTANCE
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    const esp_err_t wifi_status = revlink_network_runtime_start();
#else
    const esp_err_t wifi_status = revlink_wifi_radio_start();
#endif
    if (wifi_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Onboard C6 radio acceptance FAILED to start: %s",
            esp_err_to_name(wifi_status)
        );
        return;
    }
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_config.sync_cb = network_time_synchronized;
    const esp_err_t time_status = esp_netif_sntp_init(&sntp_config);
    if (time_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Network time unavailable; Initial sync will remain unknown: %s",
            esp_err_to_name(time_status)
        );
    }
#if !CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    const esp_err_t scan_status = revlink_wifi_radio_scan_anonymous();
    if (scan_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Onboard C6 anonymous scan FAILED to start: %s",
            esp_err_to_name(scan_status)
        );
        return;
    }
#endif
    ESP_LOGI(
        TAG,
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
        "Onboard C6 product-network acceptance started; successful station "
        "credentials persist; hotspot credentials remain RAM-only"
#elif CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
        "Onboard C6 radio acceptance started; connection requires an "
        "explicit local RAM-only command"
#else
        "Onboard C6 radio acceptance started; credentials and connection "
        "are disabled"
#endif
    );
    const esp_err_t identity_status = revlink_sidecar_identity_init();
    if (identity_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "RevLink Sidecar identity initialization FAILED: %s",
            esp_err_to_name(identity_status)
        );
        return;
    }
    ESP_LOGI(
        TAG,
        "RevLink Sidecar identity ready; identifiers are available locally"
    );
#if CONFIG_REVLINK_ONBOARDING_ACCEPTANCE
    const esp_err_t onboarding_status = revlink_onboarding_start();
    if (onboarding_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Local onboarding acceptance FAILED to start: %s",
            esp_err_to_name(onboarding_status)
        );
        return;
    }
#endif
#endif

#if CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
    const revlink_accessport_download_sink_t download_sink = {
        .context = NULL,
        .select_device = revlink_sd_select_device,
        .release_device = revlink_sd_release_device,
        .is_current = revlink_sd_download_is_current,
        .begin = revlink_sd_download_begin,
        .write = revlink_sd_download_write,
        .commit = revlink_sd_download_commit,
        .abort = revlink_sd_download_abort,
    };
    const esp_err_t sink_status =
        revlink_accessport_usb_configure_download_sink(&download_sink);
    if (sink_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to configure atomic microSD download sink: %s",
            esp_err_to_name(sink_status)
        );
        return;
    }
    const revlink_accessport_sync_observer_config_t sync_observer = {
        .context = &application,
        .observer = handle_sync_event,
    };
    const esp_err_t observer_status =
        revlink_accessport_usb_configure_sync_observer(&sync_observer);
    if (observer_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to configure sync progress observer: %s",
            esp_err_to_name(observer_status)
        );
        return;
    }
    const revlink_accessport_identity_observer_config_t identity_observer = {
        .context = NULL,
        .observer = set_connected_accessport_identity,
    };
    const esp_err_t identity_observer_status =
        revlink_accessport_usb_configure_identity_observer(
            &identity_observer
        );
    if (identity_observer_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to configure AccessPort identity observer: %s",
            esp_err_to_name(identity_observer_status)
        );
        return;
    }
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (storage_status == ESP_OK) {
        revlink_accessport_upload_source_t upload_source = {0};
        const esp_err_t upload_source_status =
            revlink_map_upload_source(&upload_source);
        if (upload_source_status != ESP_OK
            || revlink_accessport_usb_configure_upload_source(&upload_source)
                != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Unable to configure guarded microSD map source"
            );
            return;
        }
    }
#endif
#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE
    ESP_LOGW(
        TAG,
        "Starting bounded manifest-based incremental datalog + map sync; "
        "four files / 16 MiB maximum, no retries, AccessPort writes disabled"
    );
#elif CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE
    ESP_LOGW(
        TAG,
        "Starting attachment-scoped bounded file download to atomic "
        "microSD cache; "
        "no retries and AccessPort writes disabled"
    );
#elif CONFIG_REVLINK_RUNTIME_SYNC
    ESP_LOGI(
        TAG,
        "Runtime sync ready; awaiting saved auto-sync policy or manual request"
    );
#endif
#elif CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE
    ESP_LOGW(
        TAG,
        "Starting attachment-scoped bounded read-only root listing; "
        "no retries and device writes disabled"
    );
#elif CONFIG_REVLINK_USB_INTERFACE_CLAIM_ACCEPTANCE
    ESP_LOGI(
        TAG,
        "Starting descriptor scan plus claim/release acceptance; "
        "no bulk transfers"
    );
#else
    ESP_LOGI(
        TAG,
        "Starting descriptor-only host; no interface claims or bulk transfers"
    );
#endif

    const esp_err_t usb_status = revlink_accessport_usb_start(
        handle_usb_event,
        &application
    );
    if (usb_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to start USB enumeration: %s",
            esp_err_to_name(usb_status)
        );
        return;
    }

    const revlink_usb_link_capabilities_t link_capabilities =
        revlink_usb_link_capabilities();
    ESP_LOGI(
        TAG,
        "USB link control: physical data isolation=%s, "
        "physical VBUS isolation=%s",
        link_capabilities.physical_data_isolation ? "yes" : "no",
        link_capabilities.physical_vbus_isolation ? "yes" : "no"
    );
    const esp_err_t power_status = revlink_soft_power_start();
    if (power_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to arm BOOT-button soft shutdown: %s",
            esp_err_to_name(power_status)
        );
    }
}
