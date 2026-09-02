#include "revlink_file_delete.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "revlink_sd_storage.h"

#define REVLINK_DELETE_AUDIT_PATH \
    "/sdcard/revlink/system/acceptance/file-delete-audit.log"

#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
static const char *TAG = "revlink_file_delete";

typedef struct {
    SemaphoreHandle_t mutex;
    bool started;
    bool consent_enabled;
    bool recovery_required;
    revlink_accessport_delete_state_t state;
    esp_err_t platform_error;
    char path[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    /* Whether the in-flight delete should also drop the Sidecar's copy. Read
     * only after the device confirms removal, so a failed delete leaves the
     * local copy exactly where it was. */
    bool forget_cached_copy;
} file_delete_service_t;

static file_delete_service_t service;

static bool take_lock(void)
{
    return service.mutex != NULL
        && xSemaphoreTake(service.mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void give_lock(void)
{
    xSemaphoreGive(service.mutex);
}

static bool copy_bounded(char *output, size_t capacity, const char *input)
{
    if (output == NULL || input == NULL || capacity == 0U) return false;
    const size_t length = strnlen(input, capacity);
    if (length == 0U || length >= capacity) return false;
    memcpy(output, input, length + 1U);
    return true;
}

/*
 * Every outcome is written to the card before it is reported anywhere else.
 * A delete leaves no other trace: the file is gone, and without a record there
 * is nothing to say what happened or which device it happened to.
 */
static void append_audit(const revlink_accessport_delete_event_t *event)
{
    FILE *stream = fopen(REVLINK_DELETE_AUDIT_PATH, "a");
    if (stream == NULL) {
        ESP_LOGE(TAG, "Unable to append delete audit: errno=%d", errno);
        return;
    }
    (void)fprintf(
        stream,
        "utc=%lld part=%s serial=%s path=%s outcome=%s error=%d recovery=%s\n",
        (long long)time(NULL),
        event->request.expected_part_number,
        event->request.expected_serial,
        event->request.path,
        event->state == REVLINK_ACCESSPORT_DELETE_REMOVED
            ? "removed" : "failed",
        event->platform_error,
        event->recovery_required ? "required" : "clear"
    );
    (void)fflush(stream);
    (void)fsync(fileno(stream));
    fclose(stream);
}

static void observe_delete(
    void *context,
    const revlink_accessport_delete_event_t *event
)
{
    (void)context;
    if (event == NULL || !take_lock()) return;
    service.state = event->state;
    service.platform_error = event->platform_error;
    service.recovery_required = event->recovery_required;
    give_lock();
    if (event->state == REVLINK_ACCESSPORT_DELETE_REMOVED
        || event->state == REVLINK_ACCESSPORT_DELETE_FAILED) {
        append_audit(event);
    }
    if (event->state == REVLINK_ACCESSPORT_DELETE_REMOVED) {
        /*
         * The Sidecar keeps its own copy, so the row stays in the portal --
         * correctly, because the file is still downloadable from here. What
         * changes is that the AccessPort no longer has it, and waiting for
         * the next sync to notice would leave the portal offering a delete
         * that can only fail. The transport re-listed the directory and found
         * it gone before reporting REMOVED, so this is confirmation, not an
         * assumption.
         */
        const esp_err_t marked = revlink_sd_mark_absent(event->request.path);
        if (marked != ESP_OK && marked != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(
                TAG,
                "Deleted '%s' but could not record it as gone: %s",
                event->request.path,
                esp_err_to_name(marked)
            );
        }
        bool also_forget = false;
        if (take_lock()) {
            also_forget = service.forget_cached_copy;
            service.forget_cached_copy = false;
            give_lock();
        }
        if (also_forget) {
            /*
             * Only now. The owner asked for both copies to go, and the device
             * has confirmed its own is gone; had it failed, the cached copy
             * would still be the only one left and dropping it first would
             * have destroyed exactly the thing that made the failure
             * recoverable.
             */
            const esp_err_t forgotten =
                revlink_sd_forget_cached(event->request.path);
            if (forgotten != ESP_OK && forgotten != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(
                    TAG,
                    "Deleted '%s' from the device but kept the cached copy: "
                    "%s",
                    event->request.path,
                    esp_err_to_name(forgotten)
                );
            }
        }
    } else if (event->state == REVLINK_ACCESSPORT_DELETE_FAILED) {
        if (take_lock()) {
            service.forget_cached_copy = false;
            give_lock();
        }
    }
}
#endif

esp_err_t revlink_file_delete_start(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
    if (service.started) return ESP_ERR_INVALID_STATE;
    service.mutex = xSemaphoreCreateMutex();
    if (service.mutex == NULL) return ESP_ERR_NO_MEM;

    const revlink_accessport_delete_sink_t sink = {
        .context = NULL,
        .observe = observe_delete,
    };
    const esp_err_t status =
        revlink_accessport_usb_configure_delete_sink(&sink);
    if (status != ESP_OK) {
        vSemaphoreDelete(service.mutex);
        service.mutex = NULL;
        return status;
    }

    service.started = true;
    service.consent_enabled = false;
    service.state = REVLINK_ACCESSPORT_DELETE_IDLE;
    service.platform_error = ESP_OK;
    ESP_LOGW(
        TAG,
        "File-delete capability compiled; runtime consent is OFF"
    );
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_file_delete_set_consent(bool enabled)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
    if (!service.started || !take_lock()) return ESP_ERR_INVALID_STATE;
    if (service.state == REVLINK_ACCESSPORT_DELETE_RUNNING) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    service.consent_enabled = enabled;
    give_lock();
    ESP_LOGW(TAG, "Runtime file-delete consent: %s", enabled ? "ON" : "OFF");
    return ESP_OK;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_file_delete_request(
    const revlink_ap_device_info_t *identity,
    const char *path,
    bool forget_cached_copy
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
    if (identity == NULL || path == NULL
        || identity->part_number[0] == '\0'
        || identity->serial[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t path_length = strnlen(
        path,
        REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY
    );
    if (path_length == 0U
        || path_length >= REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY
        || revlink_ap_validate_delete_target(
               (const uint8_t *)path,
               path_length
           ) != REVLINK_AP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * The file name the device expects is the last path segment. The allowlist
     * has already refused anything containing a separator past the directory,
     * so there is exactly one.
     */
    const char *separator = strrchr(path, '/');
    if (separator == NULL || separator[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!service.started || !service.consent_enabled
        || service.state == REVLINK_ACCESSPORT_DELETE_RUNNING
        || service.recovery_required) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    revlink_accessport_delete_request_t request = {0};
    const bool copied =
        copy_bounded(request.path, sizeof(request.path), path)
        && copy_bounded(request.name, sizeof(request.name), separator + 1U)
        && copy_bounded(
               request.expected_part_number,
               sizeof(request.expected_part_number),
               identity->part_number
           )
        && copy_bounded(
               request.expected_serial,
               sizeof(request.expected_serial),
               identity->serial
           );
    if (copied) {
        service.state = REVLINK_ACCESSPORT_DELETE_RUNNING;
        service.platform_error = ESP_OK;
        service.forget_cached_copy = forget_cached_copy;
        (void)copy_bounded(service.path, sizeof(service.path), path);
    }
    give_lock();
    if (!copied) return ESP_ERR_INVALID_ARG;

    ESP_LOGW(
        TAG,
        "Deleting '%s' from AccessPort part=%s",
        request.path,
        request.expected_part_number
    );
    const esp_err_t status = revlink_accessport_usb_request_delete(&request);
    if (status != ESP_OK && take_lock()) {
        service.state = REVLINK_ACCESSPORT_DELETE_FAILED;
        service.platform_error = status;
        give_lock();
    }
    return status;
#else
    (void)identity;
    (void)path;
    (void)forget_cached_copy;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_file_delete_snapshot(
    revlink_file_delete_snapshot_t *snapshot
)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = (revlink_file_delete_snapshot_t){
#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
        .deletes_compiled = true,
#else
        .deletes_compiled = false,
#endif
    };
#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
    if (!service.started || !take_lock()) return ESP_ERR_INVALID_STATE;
    snapshot->consent_enabled = service.consent_enabled;
    snapshot->recovery_required = service.recovery_required;
    snapshot->state = service.state;
    snapshot->platform_error = service.platform_error;
    memcpy(snapshot->path, service.path, sizeof(snapshot->path));
    give_lock();
#endif
    return ESP_OK;
}
