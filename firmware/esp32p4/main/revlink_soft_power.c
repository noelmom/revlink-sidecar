#include "revlink_soft_power.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "revlink_power_button.h"
#include "revlink_runtime.h"
#include "revlink_sd_storage.h"
#include "revlink_sidecar_identity.h"
#include "revlink_status_oled.h"
#include "revlink_storage_recovery.h"
#include "revlink_usb_link.h"
#include "revlink_wifi_radio.h"

#define REVLINK_BOOT_BUTTON_GPIO GPIO_NUM_35
#define REVLINK_BUTTON_SAMPLE_MS 25U
#define REVLINK_BUTTON_DEBOUNCE_MS 50U
#define REVLINK_BUTTON_HOLD_MS 2000U
#define REVLINK_SYNC_CANCEL_TIMEOUT_MS 30000U
#define REVLINK_MULTI_PRESS_WINDOW_MS 650U
#define REVLINK_QR_PRESS_COUNT 2U
#define REVLINK_LOCAL_SSID_CAPACITY 24U
#define REVLINK_LOCAL_HOSTNAME_CAPACITY 64U
#define REVLINK_STORAGE_CONFIRMATION_TIMEOUT_MS 20000U
#define REVLINK_STORAGE_RESULT_DISPLAY_MS 1500U

static const char *TAG = "revlink_power";
static bool task_started;

static void show_storage_step_one(revlink_sd_storage_state_t state)
{
    switch (state) {
    case REVLINK_SD_STORAGE_MISSING:
        revlink_status_oled_show_storage_error(
            REVLINK_OLED_STORAGE_MISSING
        );
        break;
    case REVLINK_SD_STORAGE_UNREADABLE:
        revlink_status_oled_show_storage_error(
            REVLINK_OLED_STORAGE_UNREADABLE
        );
        break;
    case REVLINK_SD_STORAGE_ERROR:
    case REVLINK_SD_STORAGE_UNKNOWN:
        revlink_status_oled_show_storage_error(
            REVLINK_OLED_STORAGE_ERROR
        );
        break;
    case REVLINK_SD_STORAGE_MOUNTED:
    default:
        break;
    }
}

static bool sync_is_active(revlink_sync_state_t state)
{
    return state == REVLINK_SYNC_QUEUED
        || state == REVLINK_SYNC_RUNNING
        || state == REVLINK_SYNC_CANCELLING;
}

static bool wait_for_sync_to_stop(void)
{
    uint32_t waited_ms = 0U;
    while (sync_is_active(revlink_runtime_sync_snapshot().state)) {
        if (waited_ms >= REVLINK_SYNC_CANCEL_TIMEOUT_MS) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100U));
        waited_ms += 100U;
    }
    return true;
}

static void perform_shutdown(void)
{
    ESP_LOGW(
        TAG,
        "Soft shutdown requested; new syncs are now blocked"
    );
    const revlink_sync_coordinator_status_t prepare_status =
        revlink_runtime_prepare_shutdown();
    if (prepare_status != REVLINK_SYNC_COORDINATOR_OK) {
        ESP_LOGE(
            TAG,
            "Soft shutdown blocked while preparing runtime: status=%d",
            prepare_status
        );
        return;
    }

    if (!wait_for_sync_to_stop()) {
        ESP_LOGE(
            TAG,
            "Soft shutdown aborted: sync did not become quiescent within "
            "%u ms; reset or remove power only after activity stops",
            (unsigned int)REVLINK_SYNC_CANCEL_TIMEOUT_MS
        );
        return;
    }

    const esp_err_t usb_status = revlink_usb_link_disconnect();
    if (usb_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Logical USB disconnect returned %s; continuing after the "
            "sync reached a terminal state",
            esp_err_to_name(usb_status)
        );
    }

    const esp_err_t storage_status = revlink_sd_storage_stop();
    if (storage_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Soft shutdown aborted to protect storage: %s",
            esp_err_to_name(storage_status)
        );
        return;
    }

    ESP_LOGW(
        TAG,
        "microSD is cleanly unmounted; entering deep sleep. "
        "Wake by RESET or by cycling external power"
    );
    vTaskDelay(pdMS_TO_TICKS(100U));
    esp_deep_sleep_start();
}

static esp_err_t show_network_shortcut(void)
{
    const esp_err_t qr_status = revlink_status_oled_show_hotspot_qr();
    if (qr_status == ESP_OK) {
        return ESP_OK;
    }

    revlink_sidecar_identity_t identity = {0};
    const esp_err_t identity_status =
        revlink_sidecar_identity_snapshot(&identity);
    if (identity_status != ESP_OK) {
        return identity_status;
    }
    return revlink_status_oled_show_local_url(identity.hostname);
}

static void soft_power_task(void *context)
{
    (void)context;
    revlink_power_button_t button;
    const revlink_power_button_config_t config = {
        .debounce_ms = REVLINK_BUTTON_DEBOUNCE_MS,
        .shutdown_hold_ms = REVLINK_BUTTON_HOLD_MS,
    };
    if (!revlink_power_button_init(&button, &config)) {
        ESP_LOGE(TAG, "Unable to initialize power-button controller");
        vTaskDelete(NULL);
        return;
    }

    uint8_t short_press_count = 0U;
    uint32_t short_press_window_ms = 0U;
    const revlink_sd_storage_status_t initial_storage =
        revlink_sd_storage_status();
    revlink_storage_recovery_t storage_recovery;
    if (!revlink_storage_recovery_init(
            &storage_recovery,
            initial_storage.state == REVLINK_SD_STORAGE_UNREADABLE,
            REVLINK_STORAGE_CONFIRMATION_TIMEOUT_MS
        )) {
        ESP_LOGE(TAG, "Unable to initialize storage-recovery controller");
        vTaskDelete(NULL);
        return;
    }
    if (initial_storage.state != REVLINK_SD_STORAGE_MOUNTED) {
        show_storage_step_one(initial_storage.state);
    }

    while (true) {
        const bool pressed = gpio_get_level(REVLINK_BOOT_BUTTON_GPIO) == 0;
        const revlink_power_button_action_t action =
            revlink_power_button_update(
                &button,
                pressed,
                REVLINK_BUTTON_SAMPLE_MS
            );
        if (action == REVLINK_POWER_BUTTON_SHUTDOWN_REQUESTED) {
            short_press_count = 0U;
            short_press_window_ms = 0U;
            perform_shutdown();
        } else if (
            action == REVLINK_POWER_BUTTON_SHORT_PRESS_RELEASED
        ) {
            if (
                revlink_status_oled_hotspot_qr_visible()
                || revlink_status_oled_local_url_visible()
            ) {
                revlink_status_oled_hide_hotspot_qr();
                revlink_status_oled_hide_local_url();
                short_press_count = 0U;
                short_press_window_ms = 0U;
            } else {
                if (short_press_count < UINT8_MAX) {
                    ++short_press_count;
                }
                short_press_window_ms = 0U;
            }
        }

        if (short_press_count > 0U) {
            short_press_window_ms += REVLINK_BUTTON_SAMPLE_MS;
            if (short_press_window_ms >= REVLINK_MULTI_PRESS_WINDOW_MS) {
                if (short_press_count == REVLINK_QR_PRESS_COUNT) {
                    const revlink_storage_recovery_action_t
                        recovery_action =
                            revlink_storage_recovery_double_press(
                                &storage_recovery
                            );
                    if (
                        recovery_action
                            == REVLINK_STORAGE_RECOVERY_SHOW_WARNING
                    ) {
                        ESP_LOGW(
                            TAG,
                            "microSD format warning armed for %u seconds; "
                            "a second physical double-press is required",
                            (unsigned int)(
                                REVLINK_STORAGE_CONFIRMATION_TIMEOUT_MS
                                / 1000U
                            )
                        );
                        revlink_status_oled_show_storage_format_warning(
                            revlink_storage_recovery_seconds_remaining(
                                &storage_recovery
                            )
                        );
                    } else if (
                        recovery_action
                            == REVLINK_STORAGE_RECOVERY_FORMAT_REQUESTED
                    ) {
                        revlink_status_oled_show_storage_formatting();
                        const esp_err_t format_status =
                            revlink_sd_storage_format_unreadable();
                        if (format_status == ESP_OK) {
                            revlink_status_oled_show_storage_format_complete();
                            ESP_LOGW(
                                TAG,
                                "microSD recovery complete; restarting to "
                                "activate all storage-backed services"
                            );
                            vTaskDelay(pdMS_TO_TICKS(
                                REVLINK_STORAGE_RESULT_DISPLAY_MS
                            ));
                            esp_restart();
                        }
                        ESP_LOGE(
                            TAG,
                            "microSD recovery format failed: %s",
                            esp_err_to_name(format_status)
                        );
                        revlink_status_oled_show_storage_format_failed();
                        vTaskDelay(pdMS_TO_TICKS(
                            REVLINK_STORAGE_RESULT_DISPLAY_MS
                        ));
                        const revlink_sd_storage_status_t failed_storage =
                            revlink_sd_storage_status();
                        (void)revlink_storage_recovery_init(
                            &storage_recovery,
                            failed_storage.state
                                == REVLINK_SD_STORAGE_UNREADABLE,
                            REVLINK_STORAGE_CONFIRMATION_TIMEOUT_MS
                        );
                        show_storage_step_one(failed_storage.state);
                    } else if (
                        storage_recovery.state
                            == REVLINK_STORAGE_RECOVERY_UNAVAILABLE
                        && revlink_sd_storage_status().state
                            == REVLINK_SD_STORAGE_MOUNTED
                    ) {
                        const esp_err_t shortcut_status =
                            show_network_shortcut();
                        if (shortcut_status != ESP_OK) {
                            ESP_LOGW(
                                TAG,
                                "Network shortcut is unavailable in the "
                                "current network state: %s",
                                esp_err_to_name(shortcut_status)
                            );
                        }
                    }
                }
                short_press_count = 0U;
                short_press_window_ms = 0U;
            }
        }
        const revlink_storage_recovery_action_t recovery_tick =
            revlink_storage_recovery_tick(
                &storage_recovery,
                REVLINK_BUTTON_SAMPLE_MS
            );
        if (
            storage_recovery.state
                == REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION
        ) {
            revlink_status_oled_show_storage_format_warning(
                revlink_storage_recovery_seconds_remaining(
                    &storage_recovery
                )
            );
        } else if (
            recovery_tick
                == REVLINK_STORAGE_RECOVERY_WARNING_TIMED_OUT
        ) {
            ESP_LOGI(
                TAG,
                "microSD format confirmation timed out; no data was changed"
            );
            show_storage_step_one(
                revlink_sd_storage_status().state
            );
        }
        vTaskDelay(pdMS_TO_TICKS(REVLINK_BUTTON_SAMPLE_MS));
    }
}

esp_err_t revlink_soft_power_start(void)
{
    if (task_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << REVLINK_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t gpio_status = gpio_config(&button_config);
    if (gpio_status != ESP_OK) {
        return gpio_status;
    }

    const BaseType_t created = xTaskCreate(
        soft_power_task,
        "revlink_power",
        4096,
        NULL,
        4,
        NULL
    );
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    task_started = true;
    ESP_LOGI(
        TAG,
        "BOOT/GPIO35 gestures armed: double-press Wi-Fi QR/local URL; "
        "unreadable storage requires two double-presses within %u ms; "
        "hold for %u ms to shut down",
        (unsigned int)REVLINK_STORAGE_CONFIRMATION_TIMEOUT_MS,
        (unsigned int)REVLINK_BUTTON_HOLD_MS
    );
    return ESP_OK;
}
