#include "revlink_map_upload.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "revlink_sd_storage.h"

#define REVLINK_UPLOAD_DIRECTORY "/sdcard/revlink/system/uploads"
#define REVLINK_UPLOAD_TEMP_PATH REVLINK_UPLOAD_DIRECTORY "/map.tmp"
#define REVLINK_UPLOAD_STAGE_PATH REVLINK_UPLOAD_DIRECTORY "/map.stage"
#define REVLINK_UPLOAD_AUDIT_PATH \
    "/sdcard/revlink/system/acceptance/map-write-audit.log"

typedef struct {
    SemaphoreHandle_t mutex;
    bool started;
    bool consent_enabled;
    bool staged;
    bool staging;
    bool source_open;
    bool recovery_required;
    revlink_ap_upload_kind_t kind;
    FILE *stage_stream;
    FILE *source_stream;
    psa_hash_operation_t stage_hash;
    bool stage_hash_active;
    uint32_t expected_size;
    uint32_t written_size;
    revlink_accessport_upload_state_t state;
    esp_err_t platform_error;
    char name[REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY];
    char destination[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    uint8_t sha256[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES];
} map_upload_service_t;

#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
static const char *TAG = "revlink_map_write";
static map_upload_service_t service;

static bool copy_bounded(char *output, size_t capacity, const char *input)
{
    if (output == NULL || input == NULL || capacity == 0U) return false;
    const size_t length = strnlen(input, capacity);
    if (length == 0U || length >= capacity) return false;
    memcpy(output, input, length + 1U);
    return true;
}

static bool take_lock(void)
{
    return service.mutex != NULL
        && xSemaphoreTake(service.mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void give_lock(void)
{
    xSemaphoreGive(service.mutex);
}

static void reset_stage_writer_locked(void)
{
    if (service.stage_stream != NULL) {
        fclose(service.stage_stream);
        service.stage_stream = NULL;
    }
    if (service.stage_hash_active) {
        psa_hash_abort(&service.stage_hash);
        service.stage_hash_active = false;
    }
    service.staging = false;
    service.expected_size = 0U;
    service.written_size = 0U;
    unlink(REVLINK_UPLOAD_TEMP_PATH);
}

static esp_err_t source_open(
    void *context,
    const revlink_accessport_map_upload_request_t *request
)
{
    (void)context;
    if (request == NULL || !take_lock()) return ESP_ERR_INVALID_STATE;
    const bool matches =
        service.started && service.consent_enabled && service.staged
        && service.state == REVLINK_ACCESSPORT_UPLOAD_RUNNING
        && strcmp(request->name, service.name) == 0
        && strcmp(request->path, service.destination) == 0
        && request->size == service.expected_size
        && memcmp(
               request->source_sha256,
               service.sha256,
               sizeof(service.sha256)
           ) == 0;
    if (!matches || service.source_open) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    service.source_stream = fopen(REVLINK_UPLOAD_STAGE_PATH, "rb");
    if (service.source_stream == NULL) {
        give_lock();
        return ESP_ERR_NOT_FOUND;
    }
    service.source_open = true;
    give_lock();
    return ESP_OK;
}

static esp_err_t source_read(
    void *context,
    uint8_t *buffer,
    size_t capacity,
    size_t *count
)
{
    (void)context;
    if (buffer == NULL || count == NULL || !take_lock()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!service.source_open || service.source_stream == NULL) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    *count = fread(buffer, 1U, capacity, service.source_stream);
    const bool failed = *count == 0U && ferror(service.source_stream);
    give_lock();
    return failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t source_rewind(void *context)
{
    (void)context;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    const bool valid =
        service.source_open && service.source_stream != NULL;
    if (valid) {
        clearerr(service.source_stream);
        rewind(service.source_stream);
    }
    give_lock();
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void source_close(void *context)
{
    (void)context;
    if (!take_lock()) return;
    if (service.source_stream != NULL) {
        fclose(service.source_stream);
        service.source_stream = NULL;
    }
    service.source_open = false;
    give_lock();
}

static bool cached_file_matches(
    void *context,
    const char *path,
    uint32_t size,
    const uint8_t sha256[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES]
)
{
    (void)context;
    return revlink_sd_cached_file_matches(path, size, sha256);
}

static void append_audit(
    const revlink_accessport_upload_event_t *event
)
{
    FILE *stream = fopen(REVLINK_UPLOAD_AUDIT_PATH, "a");
    if (stream == NULL) {
        ESP_LOGE(TAG, "Unable to append map-write audit: errno=%d", errno);
        return;
    }
    char digest[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES * 2U + 1U];
    for (size_t index = 0U; index < sizeof(event->request.source_sha256);
         ++index) {
        (void)snprintf(
            digest + index * 2U,
            sizeof(digest) - index * 2U,
            "%02x",
            event->request.source_sha256[index]
        );
    }
    (void)fprintf(
        stream,
        "utc=%lld part=%s serial=%s destination=%s size=%" PRIu32
        " sha256=%s outcome=%s error=%d recovery=%s\n",
        (long long)time(NULL),
        event->request.expected_part_number,
        event->request.expected_serial,
        event->request.path,
        event->request.size,
        digest,
        event->state == REVLINK_ACCESSPORT_UPLOAD_VERIFIED
            ? "verified" : "failed",
        event->platform_error,
        event->recovery_required ? "required" : "clear"
    );
    (void)fflush(stream);
    (void)fsync(fileno(stream));
    fclose(stream);
}

static void observe_upload(
    void *context,
    const revlink_accessport_upload_event_t *event
)
{
    (void)context;
    if (event == NULL || !take_lock()) return;
    service.state = event->state;
    service.platform_error = event->platform_error;
    service.recovery_required = event->recovery_required;
    give_lock();
    if (event->state == REVLINK_ACCESSPORT_UPLOAD_VERIFIED
        || event->state == REVLINK_ACCESSPORT_UPLOAD_FAILED) {
        append_audit(event);
    }
}
#endif

esp_err_t revlink_map_upload_start(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (service.started) return ESP_ERR_INVALID_STATE;
    if (mkdir(REVLINK_UPLOAD_DIRECTORY, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    service.mutex = xSemaphoreCreateMutex();
    if (service.mutex == NULL) return ESP_ERR_NO_MEM;
    service.started = true;
    service.consent_enabled = false;
    service.state = REVLINK_ACCESSPORT_UPLOAD_IDLE;
    service.platform_error = ESP_OK;
    unlink(REVLINK_UPLOAD_TEMP_PATH);
    ESP_LOGW(
        TAG,
        "Map-write capability compiled; runtime consent is OFF"
    );
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_map_upload_source(
    revlink_accessport_upload_source_t *source
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!service.started || source == NULL) return ESP_ERR_INVALID_STATE;
    *source = (revlink_accessport_upload_source_t){
        .context = NULL,
        .open = source_open,
        .read = source_read,
        .rewind = source_rewind,
        .close = source_close,
        .cached_file_matches = cached_file_matches,
        .observe = observe_upload,
    };
    return ESP_OK;
#else
    (void)source;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_map_upload_set_consent(bool enabled)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!service.started || !take_lock()) return ESP_ERR_INVALID_STATE;
    if (service.state == REVLINK_ACCESSPORT_UPLOAD_RUNNING) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    service.consent_enabled = enabled;
    give_lock();
    ESP_LOGW(TAG, "Runtime map-write consent: %s", enabled ? "ON" : "OFF");
    return ESP_OK;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_map_upload_stage_begin(
    const char *name,
    const char *destination,
    uint32_t size
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    revlink_ap_upload_kind_t kind = REVLINK_AP_UPLOAD_MAP;
    const char *destination_name =
        destination != NULL ? strrchr(destination, '/') : NULL;
    destination_name =
        destination_name != NULL ? destination_name + 1U : NULL;
    if (!service.started || name == NULL || destination == NULL
        || destination_name == NULL || strcmp(name, destination_name) != 0
        || size == 0U || size > REVLINK_AP_MAX_CHUNK_PAYLOAD
        || revlink_ap_validate_upload_target(
               (const uint8_t *)destination,
               strlen(destination),
               size,
               &kind
           ) != REVLINK_AP_OK
        || !take_lock()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (service.staging || service.source_open
        || service.state == REVLINK_ACCESSPORT_UPLOAD_RUNNING) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    reset_stage_writer_locked();
    if (!copy_bounded(service.name, sizeof(service.name), name)
        || !copy_bounded(
               service.destination,
               sizeof(service.destination),
               destination
           )) {
        give_lock();
        return ESP_ERR_INVALID_ARG;
    }
    service.stage_stream = fopen(REVLINK_UPLOAD_TEMP_PATH, "wb");
    service.stage_hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (service.stage_stream == NULL
        || psa_hash_setup(&service.stage_hash, PSA_ALG_SHA_256)
            != PSA_SUCCESS) {
        reset_stage_writer_locked();
        give_lock();
        return ESP_FAIL;
    }
    service.stage_hash_active = true;
    service.staging = true;
    service.staged = false;
    service.expected_size = size;
    service.kind = kind;
    service.written_size = 0U;
    service.state = REVLINK_ACCESSPORT_UPLOAD_IDLE;
    service.platform_error = ESP_OK;
    give_lock();
    return ESP_OK;
#else
    (void)name;
    (void)destination;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_map_upload_stage_write(
    const uint8_t *data,
    size_t length
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (data == NULL || length == 0U || !take_lock()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!service.staging || service.stage_stream == NULL
        || length > service.expected_size - service.written_size
        || fwrite(data, 1U, length, service.stage_stream) != length
        || psa_hash_update(&service.stage_hash, data, length)
            != PSA_SUCCESS) {
        reset_stage_writer_locked();
        give_lock();
        return ESP_FAIL;
    }
    service.written_size += (uint32_t)length;
    give_lock();
    return ESP_OK;
#else
    (void)data;
    (void)length;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_map_upload_stage_commit(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    size_t digest_length = 0U;
    const bool valid =
        service.staging && service.stage_stream != NULL
        && service.written_size == service.expected_size
        && fflush(service.stage_stream) == 0
        && fsync(fileno(service.stage_stream)) == 0
        && psa_hash_finish(
               &service.stage_hash,
               service.sha256,
               sizeof(service.sha256),
               &digest_length
           ) == PSA_SUCCESS
        && digest_length == sizeof(service.sha256);
    service.stage_hash_active = false;
    if (service.stage_stream != NULL) {
        fclose(service.stage_stream);
        service.stage_stream = NULL;
    }
    service.staging = false;
    if (valid) {
        /*
         * FatFS does not replace an existing destination during rename().
         * Staging metadata is intentionally RAM-only, so a prior boot may
         * leave an inert map.stage file behind. Remove that stale payload
         * only after the replacement has been fully written, flushed,
         * fsynced, and hashed.
         */
        unlink(REVLINK_UPLOAD_STAGE_PATH);
    }
    if (!valid
        || rename(
               REVLINK_UPLOAD_TEMP_PATH,
               REVLINK_UPLOAD_STAGE_PATH
           ) != 0) {
        unlink(REVLINK_UPLOAD_TEMP_PATH);
        service.staged = false;
        give_lock();
        return ESP_FAIL;
    }
    service.staged = true;
    give_lock();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void revlink_map_upload_stage_abort(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!take_lock()) return;
    reset_stage_writer_locked();
    give_lock();
#endif
}

esp_err_t revlink_map_upload_request(
    const revlink_ap_device_info_t *identity
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (identity == NULL || identity->part_number[0] == '\0'
        || identity->serial[0] == '\0' || !take_lock()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_REVLINK_WRITE_ACCEPTANCE_PART_NUMBER[0] != '\0'
        && strcmp(
               identity->part_number,
               CONFIG_REVLINK_WRITE_ACCEPTANCE_PART_NUMBER
           ) != 0) {
        give_lock();
        ESP_LOGE(
            TAG,
            "Write refused: attached part does not match acceptance pin"
        );
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!service.started || !service.consent_enabled || !service.staged
        || revlink_accessport_usb_write_recovery_required()
        || service.state == REVLINK_ACCESSPORT_UPLOAD_RUNNING) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    revlink_accessport_map_upload_request_t request = {
        .modification_time = (uint32_t)time(NULL),
        .size = service.expected_size,
    };
    const bool copied =
        copy_bounded(request.name, sizeof(request.name), service.name)
        && copy_bounded(
               request.path,
               sizeof(request.path),
               service.destination
           )
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
    memcpy(
        request.source_sha256,
        service.sha256,
        sizeof(request.source_sha256)
    );
    if (copied) {
        service.state = REVLINK_ACCESSPORT_UPLOAD_RUNNING;
        service.platform_error = ESP_OK;
    }
    give_lock();
    if (!copied) return ESP_ERR_INVALID_ARG;
    const esp_err_t status =
        revlink_accessport_usb_request_map_upload(&request);
    if (status != ESP_OK && take_lock()) {
        service.state = REVLINK_ACCESSPORT_UPLOAD_FAILED;
        service.platform_error = status;
        give_lock();
    }
    return status;
#else
    (void)identity;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_map_upload_snapshot(
    revlink_map_upload_snapshot_t *snapshot
)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = (revlink_map_upload_snapshot_t){
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
        .writes_compiled = true,
#else
        .writes_compiled = false,
#endif
    };
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!service.started || !take_lock()) return ESP_ERR_INVALID_STATE;
    snapshot->consent_enabled = service.consent_enabled;
    snapshot->staged = service.staged;
    snapshot->recovery_required =
        service.recovery_required
        || revlink_accessport_usb_write_recovery_required();
    snapshot->state = service.state;
    snapshot->platform_error = service.platform_error;
    snapshot->size = service.expected_size;
    snapshot->kind = service.kind;
    memcpy(snapshot->name, service.name, sizeof(snapshot->name));
    memcpy(
        snapshot->destination,
        service.destination,
        sizeof(snapshot->destination)
    );
    memcpy(snapshot->sha256, service.sha256, sizeof(snapshot->sha256));
    give_lock();
#endif
    return ESP_OK;
}
