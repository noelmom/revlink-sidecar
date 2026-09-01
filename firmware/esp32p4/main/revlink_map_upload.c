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
#define REVLINK_UPLOAD_META_PATH REVLINK_UPLOAD_DIRECTORY "/map.meta"
#define REVLINK_UPLOAD_META_TEMP_PATH REVLINK_UPLOAD_DIRECTORY "/map.meta.tmp"
#define REVLINK_UPLOAD_AUDIT_PATH \
    "/sdcard/revlink/system/acceptance/map-write-audit.log"

#define REVLINK_UPLOAD_VERIFY_CHUNK_BYTES 4096U

typedef struct {
    SemaphoreHandle_t mutex;
    bool started;
    bool consent_enabled;
    bool auto_apply_enabled;
    bool staged;
    bool staging;
    bool source_open;
    bool recovery_required;
    /*
     * One automatic attempt per attach. Cleared by
     * revlink_map_upload_notify_attach(), never by a sync completing, so a
     * failed automatic write is not retried by a second sync batch.
     */
    bool auto_apply_attempted;
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
    char target_part_number[REVLINK_AP_PART_NUMBER_CAPACITY];
    char target_serial[REVLINK_AP_SERIAL_CAPACITY];
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

/*
 * Remove both halves of a staged payload from the card. Metadata first: a
 * payload without metadata is inert, but metadata without a payload could be
 * misread. This does not touch in-memory staging state, so it is safe to call
 * while a replacement payload is mid-commit.
 */
static void remove_staged_files_locked(void)
{
    unlink(REVLINK_UPLOAD_META_PATH);
    unlink(REVLINK_UPLOAD_META_TEMP_PATH);
    unlink(REVLINK_UPLOAD_STAGE_PATH);
}

/* Remove the files and forget the payload entirely. */
static void discard_staged_locked(void)
{
    remove_staged_files_locked();
    service.staged = false;
    service.target_part_number[0] = '\0';
    service.target_serial[0] = '\0';
}

/* Write the metadata sidecar atomically: temp file, flush, fsync, rename. */
static bool write_metadata_locked(void)
{
    revlink_staged_map_record_t record;
    memset(&record, 0, sizeof(record));
    record.kind = service.kind == REVLINK_AP_UPLOAD_STARTUP_SCREEN
        ? REVLINK_STAGED_MAP_KIND_STARTUP_IMAGE
        : REVLINK_STAGED_MAP_KIND_MAP;
    record.size = service.expected_size;
    memcpy(record.sha256, service.sha256, sizeof(record.sha256));
    if (!copy_bounded(record.name, sizeof(record.name), service.name)
        || !copy_bounded(
               record.destination,
               sizeof(record.destination),
               service.destination
           )
        || !copy_bounded(
               record.target_part_number,
               sizeof(record.target_part_number),
               service.target_part_number
           )
        || !copy_bounded(
               record.target_serial,
               sizeof(record.target_serial),
               service.target_serial
           )) {
        return false;
    }

    uint8_t encoded[REVLINK_STAGED_MAP_RECORD_BYTES];
    if (revlink_staged_map_encode(
            &record,
            encoded,
            sizeof(encoded),
            NULL
        ) != REVLINK_STAGED_MAP_OK) {
        return false;
    }

    FILE *stream = fopen(REVLINK_UPLOAD_META_TEMP_PATH, "wb");
    if (stream == NULL) return false;
    const bool written =
        fwrite(encoded, 1U, sizeof(encoded), stream) == sizeof(encoded)
        && fflush(stream) == 0 && fsync(fileno(stream)) == 0;
    fclose(stream);
    if (!written) {
        unlink(REVLINK_UPLOAD_META_TEMP_PATH);
        return false;
    }
    /* FatFS rename() does not replace an existing destination. */
    unlink(REVLINK_UPLOAD_META_PATH);
    if (rename(
            REVLINK_UPLOAD_META_TEMP_PATH,
            REVLINK_UPLOAD_META_PATH
        ) != 0) {
        unlink(REVLINK_UPLOAD_META_TEMP_PATH);
        return false;
    }
    return true;
}

/* Recompute SHA-256 over the staged payload and compare with the record. */
static bool staged_payload_digest_matches(
    const revlink_staged_map_record_t *record
)
{
    FILE *stream = fopen(REVLINK_UPLOAD_STAGE_PATH, "rb");
    if (stream == NULL) return false;

    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(stream);
        return false;
    }

    uint8_t buffer[REVLINK_UPLOAD_VERIFY_CHUNK_BYTES];
    uint32_t total = 0U;
    bool ok = true;
    for (;;) {
        const size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        if (count == 0U) {
            ok = ferror(stream) == 0;
            break;
        }
        /*
         * total <= record->size is an invariant, so this subtraction cannot
         * underflow. Stop at the first byte past the recorded length instead
         * of hashing an arbitrarily long file before rejecting it.
         */
        if ((uint32_t)count > record->size - total
            || psa_hash_update(&operation, buffer, count) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        total += (uint32_t)count;
    }
    fclose(stream);

    uint8_t digest[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES];
    size_t digest_length = 0U;
    if (!ok || total != record->size
        || psa_hash_finish(
               &operation,
               digest,
               sizeof(digest),
               &digest_length
           ) != PSA_SUCCESS
        || digest_length != sizeof(digest)) {
        psa_hash_abort(&operation);
        return false;
    }
    return memcmp(digest, record->sha256, sizeof(digest)) == 0;
}

/*
 * Restore a payload staged before the last restart. Anything unverifiable is
 * discarded rather than carried forward: a staged write must be exactly what
 * the owner uploaded, or nothing at all.
 */
static void restore_staged_locked(void)
{
    FILE *stream = fopen(REVLINK_UPLOAD_META_PATH, "rb");
    if (stream == NULL) {
        /* No metadata: any payload left behind is inert. Clear it. */
        unlink(REVLINK_UPLOAD_STAGE_PATH);
        return;
    }
    uint8_t encoded[REVLINK_STAGED_MAP_RECORD_BYTES];
    const size_t read = fread(encoded, 1U, sizeof(encoded), stream);
    fclose(stream);

    revlink_staged_map_record_t record;
    const revlink_staged_map_status_t status =
        revlink_staged_map_decode(encoded, read, &record);
    if (status != REVLINK_STAGED_MAP_OK) {
        ESP_LOGW(
            TAG,
            "Discarding staged map: metadata %s",
            revlink_staged_map_status_name(status)
        );
        discard_staged_locked();
        return;
    }

    /* Re-validate the destination against the current product policy, so a
     * record written by a build with a wider policy cannot widen this one. */
    revlink_ap_upload_kind_t kind = REVLINK_AP_UPLOAD_MAP;
    if (revlink_ap_validate_upload_target(
            (const uint8_t *)record.destination,
            strlen(record.destination),
            record.size,
            &kind
        ) != REVLINK_AP_OK) {
        ESP_LOGW(
            TAG,
            "Discarding staged map: destination is not permitted by the "
            "current write policy"
        );
        discard_staged_locked();
        return;
    }

    if (!staged_payload_digest_matches(&record)) {
        ESP_LOGW(
            TAG,
            "Discarding staged map: payload does not match its recorded "
            "size and SHA-256"
        );
        discard_staged_locked();
        return;
    }

    service.kind = kind;
    service.expected_size = record.size;
    memcpy(service.sha256, record.sha256, sizeof(service.sha256));
    if (!copy_bounded(service.name, sizeof(service.name), record.name)
        || !copy_bounded(
               service.destination,
               sizeof(service.destination),
               record.destination
           )
        || !copy_bounded(
               service.target_part_number,
               sizeof(service.target_part_number),
               record.target_part_number
           )
        || !copy_bounded(
               service.target_serial,
               sizeof(service.target_serial),
               record.target_serial
           )) {
        discard_staged_locked();
        return;
    }
    service.staged = true;
    ESP_LOGW(
        TAG,
        "Restored staged %s '%s' (%" PRIu32 " bytes) pinned to part=%s",
        kind == REVLINK_AP_UPLOAD_STARTUP_SCREEN ? "startup image" : "map",
        service.destination,
        service.expected_size,
        service.target_part_number
    );
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
    if (event->state == REVLINK_ACCESSPORT_UPLOAD_VERIFIED) {
        /*
         * The payload reached the device and read back byte-for-byte, so it
         * is no longer pending anything. Leaving it staged would have the
         * portal promise a transfer that already happened, and would offer
         * the same map again on the next attach — where the device would
         * refuse it as an existing destination.
         *
         * A failed write deliberately keeps its payload: recovery is the
         * owner's decision, and they need something to retry.
         */
        discard_staged_locked();
    }
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
    service.auto_apply_enabled = false;
    service.auto_apply_attempted = false;
    service.state = REVLINK_ACCESSPORT_UPLOAD_IDLE;
    service.platform_error = ESP_OK;
    /* A partial payload from an interrupted upload is never resumable. */
    unlink(REVLINK_UPLOAD_TEMP_PATH);
    unlink(REVLINK_UPLOAD_META_TEMP_PATH);
    restore_staged_locked();
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
    /* A new payload starts unpinned; the caller must pin it before commit. */
    service.target_part_number[0] = '\0';
    service.target_serial[0] = '\0';
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

esp_err_t revlink_map_upload_stage_set_target(
    const char *part_number,
    const char *serial
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (part_number == NULL || serial == NULL || !take_lock()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!service.staging) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!copy_bounded(
            service.target_part_number,
            sizeof(service.target_part_number),
            part_number
        )
        || !copy_bounded(
               service.target_serial,
               sizeof(service.target_serial),
               serial
           )) {
        service.target_part_number[0] = '\0';
        service.target_serial[0] = '\0';
        give_lock();
        return ESP_ERR_INVALID_ARG;
    }
    give_lock();
    return ESP_OK;
#else
    (void)part_number;
    (void)serial;
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
         * Drop the previous payload and its metadata together, only after
         * the replacement has been fully written, flushed, fsynced, and
         * hashed. The pin for the payload being committed lives in memory and
         * is deliberately left intact here.
         */
        remove_staged_files_locked();
    }
    if (!valid
        || rename(
               REVLINK_UPLOAD_TEMP_PATH,
               REVLINK_UPLOAD_STAGE_PATH
           ) != 0) {
        unlink(REVLINK_UPLOAD_TEMP_PATH);
        discard_staged_locked();
        give_lock();
        return ESP_FAIL;
    }
    /*
     * Metadata carries the digest, the destination, and the AccessPort the
     * payload is pinned to, and it is what lets a staged write survive a
     * power cycle. It is only written for a pinned payload: a record with no
     * target would have to be interpreted on the next boot as applying to
     * whatever happens to be attached, which is exactly what must not happen.
     *
     * An unpinned payload still stages normally and can be applied by hand,
     * it simply does not persist across a restart.
     */
    if (service.target_serial[0] != '\0') {
        if (!write_metadata_locked()) {
            ESP_LOGE(
                TAG,
                "Unable to persist staged-map metadata; discarding"
            );
            discard_staged_locked();
            give_lock();
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(
            TAG,
            "Staged payload has no target AccessPort; it can be applied "
            "manually but will not survive a restart or auto-apply"
        );
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

esp_err_t revlink_map_upload_discard(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!service.started || !take_lock()) return ESP_ERR_INVALID_STATE;
    if (service.staging
        || service.state == REVLINK_ACCESSPORT_UPLOAD_RUNNING) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    const bool had_payload = service.staged;
    discard_staged_locked();
    service.name[0] = '\0';
    service.destination[0] = '\0';
    service.expected_size = 0U;
    service.state = REVLINK_ACCESSPORT_UPLOAD_IDLE;
    service.platform_error = ESP_OK;
    give_lock();
    if (had_payload) ESP_LOGW(TAG, "Staged map discarded by the owner");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
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

esp_err_t revlink_map_upload_set_auto_apply(bool enabled)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!service.started || !take_lock()) return ESP_ERR_INVALID_STATE;
    service.auto_apply_enabled = enabled;
    give_lock();
    ESP_LOGW(
        TAG,
        "Staged-map auto-apply on next sync: %s",
        enabled ? "ON" : "OFF"
    );
    return ESP_OK;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void revlink_map_upload_notify_attach(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (!service.started || !take_lock()) return;
    service.auto_apply_attempted = false;
    give_lock();
#endif
}

esp_err_t revlink_map_upload_auto_apply(
    const revlink_ap_device_info_t *identity,
    bool sync_completed_clean,
    size_t sync_pending,
    revlink_staged_map_apply_decision_t *decision
)
{
    revlink_staged_map_apply_decision_t local =
        REVLINK_STAGED_MAP_APPLY_WRITES_NOT_COMPILED;
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (identity == NULL) {
        local = REVLINK_STAGED_MAP_APPLY_NO_DEVICE;
        if (decision != NULL) *decision = local;
        return ESP_ERR_INVALID_ARG;
    }
    if (!service.started || !take_lock()) {
        if (decision != NULL) *decision = local;
        return ESP_ERR_INVALID_STATE;
    }

    revlink_staged_map_apply_context_t context;
    memset(&context, 0, sizeof(context));
    context.writes_compiled = true;
    context.consent_enabled = service.consent_enabled;
    context.auto_apply_enabled = service.auto_apply_enabled;
    context.staged = service.staged;
    context.device_identified = identity->serial[0] != '\0';
    context.sync_completed_clean = sync_completed_clean;
    context.sync_pending = sync_pending;
    context.transfer_running =
        service.state == REVLINK_ACCESSPORT_UPLOAD_RUNNING;
    context.recovery_required =
        service.recovery_required
        || revlink_accessport_usb_write_recovery_required();
    context.already_attempted_this_attach = service.auto_apply_attempted;
    (void)copy_bounded(
        context.target_part_number,
        sizeof(context.target_part_number),
        service.target_part_number
    );
    (void)copy_bounded(
        context.target_serial,
        sizeof(context.target_serial),
        service.target_serial
    );
    (void)copy_bounded(
        context.attached_part_number,
        sizeof(context.attached_part_number),
        identity->part_number
    );
    (void)copy_bounded(
        context.attached_serial,
        sizeof(context.attached_serial),
        identity->serial
    );

    local = revlink_staged_map_evaluate_apply(&context);
    if (local != REVLINK_STAGED_MAP_APPLY_ALLOWED) {
        give_lock();
        if (decision != NULL) *decision = local;
        /*
         * Transient refusals are normal and quiet. A settled refusal with a
         * staged payload present is worth surfacing, because the owner is
         * waiting for a write that will not happen on its own.
         */
        if (context.staged
            && !revlink_staged_map_apply_decision_is_transient(local)) {
            ESP_LOGW(
                TAG,
                "Staged map not applied automatically: %s",
                revlink_staged_map_apply_decision_name(local)
            );
        }
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Latch the attempt before releasing the lock. A write that fails is not
     * retried by a later sync in the same attach; recovery is deliberate.
     */
    service.auto_apply_attempted = true;
    char applied_destination[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    char applied_target[REVLINK_AP_PART_NUMBER_CAPACITY];
    memcpy(
        applied_destination,
        service.destination,
        sizeof(applied_destination)
    );
    memcpy(applied_target, service.target_part_number, sizeof(applied_target));
    give_lock();

    ESP_LOGW(
        TAG,
        "Applying staged map '%s' to pinned AccessPort part=%s",
        applied_destination,
        applied_target
    );
    if (decision != NULL) *decision = local;
    return revlink_map_upload_request(identity);
#else
    (void)identity;
    (void)sync_completed_clean;
    (void)sync_pending;
    if (decision != NULL) *decision = local;
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
    snapshot->auto_apply_enabled = service.auto_apply_enabled;
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
    memcpy(
        snapshot->target_part_number,
        service.target_part_number,
        sizeof(snapshot->target_part_number)
    );
    memcpy(
        snapshot->target_serial,
        service.target_serial,
        sizeof(snapshot->target_serial)
    );
    memcpy(snapshot->sha256, service.sha256, sizeof(snapshot->sha256));
    give_lock();
#endif
    return ESP_OK;
}
