#include "revlink_backup.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "revlink_sd_storage.h"

#define BACKUP_ROOT "/sdcard/revlink/devices"
#define BACKUP_MOUNT_PREFIX "/sdcard/"
#define BACKUP_PENDING "/sdcard/.revlink-restore.pending"
#define BACKUP_PARTIAL_SUFFIX ".restore-partial"
#define BACKUP_MAGIC "RVLBKP01"
#define BACKUP_VERSION 1U
#define BACKUP_HEADER_BYTES 32U
#define BACKUP_ENTRY_BYTES 304U
#define BACKUP_PATH_BYTES 256U
#define BACKUP_BUFFER_BYTES 4096U
#define BACKUP_MAX_DEPTH 10U
#define BACKUP_MAX_FILES 4096U
#define BACKUP_MAX_FILE_BYTES (64ULL * 1024ULL * 1024ULL)
#define BACKUP_MAX_ARCHIVE_BYTES (512ULL * 1024ULL * 1024ULL)
#define BACKUP_DEVICE_CAPACITY 16U

static const char *TAG = "revlink_backup";
static FILE *stage_stream;
static uint64_t stage_expected;
static uint64_t stage_written;
static revlink_backup_summary_t staged_summary;

typedef struct {
    uint32_t files;
    uint64_t data_bytes;
} scan_summary_t;

typedef struct {
    revlink_backup_write_t write;
    void *context;
} export_context_t;

static void put_u16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)value;
    buffer[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *buffer, uint32_t value)
{
    for (size_t index = 0U; index < 4U; ++index) {
        buffer[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void put_u64(uint8_t *buffer, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        buffer[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint16_t get_u16(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8U);
}

static uint32_t get_u32(const uint8_t *buffer)
{
    uint32_t value = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        value |= (uint32_t)buffer[index] << (index * 8U);
    }
    return value;
}

static uint64_t get_u64(const uint8_t *buffer)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        value |= (uint64_t)buffer[index] << (index * 8U);
    }
    return value;
}

static bool storage_is_idle(void)
{
    bool mounted = false;
    bool session_selected = false;
    return revlink_sd_portal_io_status(&mounted, &session_selected) == ESP_OK
        && mounted && !session_selected;
}

static bool ignored_name(const char *name)
{
    const size_t length = strlen(name);
    return strcmp(name, "tmp") == 0
        || (length >= 4U && strcmp(name + length - 4U, ".tmp") == 0)
        || (length >= 4U && strcmp(name + length - 4U, ".bak") == 0)
        || strstr(name, ".partial") != NULL
        || strstr(name, BACKUP_PARTIAL_SUFFIX) != NULL;
}

static bool safe_relative_path(const char *path)
{
    static const char prefix[] = "revlink/devices/";
    const size_t length = strnlen(path, BACKUP_PATH_BYTES);
    if (length < sizeof(prefix) || length >= BACKUP_PATH_BYTES
        || strncmp(path, prefix, sizeof(prefix) - 1U) != 0
        || path[length - 1U] == '/' || strstr(path, "\\") != NULL) {
        return false;
    }
    const char *segment = path;
    while (*segment != '\0') {
        const char *slash = strchr(segment, '/');
        const size_t segment_length =
            slash == NULL ? strlen(segment) : (size_t)(slash - segment);
        if (segment_length == 0U
            || (segment_length == 1U && segment[0] == '.')
            || (segment_length == 2U
                && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        for (size_t index = 0U; index < segment_length; ++index) {
            if (!isprint((unsigned char)segment[index])) return false;
        }
        if (slash == NULL) break;
        segment = slash + 1U;
    }
    return true;
}

static esp_err_t hash_stream(
    FILE *stream,
    uint64_t size,
    uint8_t digest[32]
)
{
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    psa_status_t status =
        psa_hash_setup(&operation, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) return ESP_FAIL;
    /*
     * On the heap, not the stack. This runs at the leaf of a directory walk
     * that recurses up to BACKUP_MAX_DEPTH, on the HTTP server task, with
     * export_one's own transfer buffer already live one frame up. Two 4 KiB
     * stack buffers under ten levels of recursion overflow any stack this
     * server can reasonably be given.
     */
    uint8_t *buffer = malloc(BACKUP_BUFFER_BYTES);
    if (buffer == NULL) {
        psa_hash_abort(&operation);
        return ESP_ERR_NO_MEM;
    }
    uint64_t remaining = size;
    while (remaining > 0U) {
        const size_t requested =
            remaining < BACKUP_BUFFER_BYTES
                ? (size_t)remaining : BACKUP_BUFFER_BYTES;
        const size_t count = fread(buffer, 1U, requested, stream);
        if (count != requested
            || psa_hash_update(&operation, buffer, count) != PSA_SUCCESS) {
            psa_hash_abort(&operation);
            free(buffer);
            return ESP_FAIL;
        }
        remaining -= count;
    }
    free(buffer);
    size_t digest_length = 0U;
    status = psa_hash_finish(
        &operation,
        digest,
        32U,
        &digest_length
    );
    return status == PSA_SUCCESS && digest_length == 32U
        ? ESP_OK : ESP_FAIL;
}

static esp_err_t hash_file(
    const char *path,
    uint64_t size,
    uint8_t digest[32]
)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) return ESP_FAIL;
    const esp_err_t status = hash_stream(stream, size, digest);
    fclose(stream);
    return status;
}

static esp_err_t walk_scan(
    const char *absolute,
    unsigned depth,
    scan_summary_t *summary
)
{
    if (depth > BACKUP_MAX_DEPTH) return ESP_ERR_INVALID_SIZE;
    DIR *directory = opendir(absolute);
    if (directory == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    esp_err_t status = ESP_OK;
    struct dirent *entry;
    while (status == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' || ignored_name(entry->d_name)) continue;
        char path[384U];
        const int count = snprintf(
            path,
            sizeof(path),
            "%s/%s",
            absolute,
            entry->d_name
        );
        if (count <= 0 || (size_t)count >= sizeof(path)) {
            status = ESP_ERR_INVALID_SIZE;
            break;
        }
        struct stat info;
        if (stat(path, &info) != 0) {
            status = ESP_FAIL;
        } else if (S_ISDIR(info.st_mode)) {
            status = walk_scan(path, depth + 1U, summary);
        } else if (S_ISREG(info.st_mode)) {
            if ((uint64_t)info.st_size > BACKUP_MAX_FILE_BYTES
                || summary->files >= BACKUP_MAX_FILES
                || UINT64_MAX - summary->data_bytes
                    < (uint64_t)info.st_size) {
                status = ESP_ERR_INVALID_SIZE;
            } else {
                summary->files++;
                summary->data_bytes += (uint64_t)info.st_size;
            }
        }
    }
    closedir(directory);
    return status;
}

static esp_err_t export_one(
    const char *absolute,
    const char *relative,
    uint64_t size,
    export_context_t *output
)
{
    if (!safe_relative_path(relative)) return ESP_ERR_INVALID_ARG;
    uint8_t digest[32U];
    esp_err_t status = hash_file(absolute, size, digest);
    if (status != ESP_OK) return status;

    /* Per-file progress; verbose by design, so raise the log level to see it. */
    ESP_LOGD(
        TAG,
        "export %s (%llu bytes) heap=%u largest=%u stack_free=%u",
        relative,
        (unsigned long long)size,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned int)uxTaskGetStackHighWaterMark(NULL)
    );

    uint8_t header[BACKUP_ENTRY_BYTES] = {0};
    memcpy(header, "RLE1", 4U);
    const size_t path_length = strlen(relative);
    put_u16(header + 4U, (uint16_t)path_length);
    put_u64(header + 8U, size);
    memcpy(header + 16U, digest, sizeof(digest));
    memcpy(header + 48U, relative, path_length);
    status = output->write(output->context, header, sizeof(header));
    if (status != ESP_OK) return status;

    FILE *stream = fopen(absolute, "rb");
    if (stream == NULL) return ESP_FAIL;
    /* Heap, for the same reason as hash_stream: this frame sits under the
     * recursive walk and must not carry 4 KiB of its own. */
    uint8_t *buffer = malloc(BACKUP_BUFFER_BYTES);
    if (buffer == NULL) {
        fclose(stream);
        return ESP_ERR_NO_MEM;
    }
    uint64_t remaining = size;
    while (status == ESP_OK && remaining > 0U) {
        const size_t requested =
            remaining < BACKUP_BUFFER_BYTES
                ? (size_t)remaining : BACKUP_BUFFER_BYTES;
        const size_t count = fread(buffer, 1U, requested, stream);
        if (count != requested) {
            status = ESP_FAIL;
            break;
        }
        status = output->write(output->context, buffer, count);
        if (status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "chunk write failed at %llu of %llu bytes remaining: %d",
                (unsigned long long)remaining,
                (unsigned long long)size,
                (int)status
            );
        }
        remaining -= count;
    }
    free(buffer);
    fclose(stream);
    return status;
}

static esp_err_t walk_export(
    const char *absolute,
    const char *relative,
    unsigned depth,
    export_context_t *output
)
{
    if (depth > BACKUP_MAX_DEPTH) return ESP_ERR_INVALID_SIZE;
    DIR *directory = opendir(absolute);
    if (directory == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    esp_err_t status = ESP_OK;
    struct dirent *entry;
    while (status == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' || ignored_name(entry->d_name)) continue;
        char child_absolute[384U];
        char child_relative[BACKUP_PATH_BYTES];
        const int absolute_count = snprintf(
            child_absolute,
            sizeof(child_absolute),
            "%s/%s",
            absolute,
            entry->d_name
        );
        const int relative_count = snprintf(
            child_relative,
            sizeof(child_relative),
            "%s/%s",
            relative,
            entry->d_name
        );
        if (absolute_count <= 0
            || (size_t)absolute_count >= sizeof(child_absolute)
            || relative_count <= 0
            || (size_t)relative_count >= sizeof(child_relative)) {
            status = ESP_ERR_INVALID_SIZE;
            break;
        }
        struct stat info;
        if (stat(child_absolute, &info) != 0) {
            status = ESP_FAIL;
        } else if (S_ISDIR(info.st_mode)) {
            ESP_LOGD(TAG, "descend depth=%u %s", depth + 1U, child_relative);
            status = walk_export(
                child_absolute,
                child_relative,
                depth + 1U,
                output
            );
        } else if (S_ISREG(info.st_mode)) {
            status = export_one(
                child_absolute,
                child_relative,
                (uint64_t)info.st_size,
                output
            );
        }
    }
    closedir(directory);
    return status;
}

esp_err_t revlink_backup_export(
    revlink_backup_write_t write,
    void *context,
    revlink_backup_summary_t *summary
)
{
    if (write == NULL || summary == NULL) return ESP_ERR_INVALID_ARG;
    if (!storage_is_idle()) return ESP_ERR_INVALID_STATE;
    scan_summary_t scan = {0};
    esp_err_t status = walk_scan(BACKUP_ROOT, 0U, &scan);
    if (status != ESP_OK) return status;

    memset(summary, 0, sizeof(*summary));
    summary->file_count = scan.files;
    summary->data_bytes = scan.data_bytes;
    summary->created_utc = (uint64_t)time(NULL);
    summary->archive_bytes = BACKUP_HEADER_BYTES
        + (uint64_t)scan.files * BACKUP_ENTRY_BYTES + scan.data_bytes;

    uint8_t header[BACKUP_HEADER_BYTES] = {0};
    memcpy(header, BACKUP_MAGIC, 8U);
    put_u32(header + 8U, BACKUP_VERSION);
    put_u32(header + 12U, scan.files);
    put_u64(header + 16U, scan.data_bytes);
    put_u64(header + 24U, summary->created_utc);
    status = write(context, header, sizeof(header));
    if (status != ESP_OK) return status;
    export_context_t output = {.write = write, .context = context};
    return walk_export(
        BACKUP_ROOT,
        "revlink/devices",
        0U,
        &output
    );
}

static bool read_exact(FILE *stream, uint8_t *buffer, size_t length)
{
    return fread(buffer, 1U, length, stream) == length;
}

static void digest_hex(const uint8_t digest[32], char output[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < 32U; ++index) {
        output[index * 2U] = hex[digest[index] >> 4U];
        output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    output[64] = '\0';
}

static bool token_equal(const char *left, const char *right)
{
    if (left == NULL || right == NULL
        || strlen(left) != 64U || strlen(right) != 64U) return false;
    unsigned difference = 0U;
    for (size_t index = 0U; index < 64U; ++index) {
        difference |= (unsigned)(unsigned char)left[index]
            ^ (unsigned)(unsigned char)right[index];
    }
    return difference == 0U;
}

static bool remember_device(
    const char *path,
    char devices[BACKUP_DEVICE_CAPACITY][25U],
    uint32_t *count
)
{
    static const char prefix[] = "revlink/devices/";
    const char *key = path + sizeof(prefix) - 1U;
    const char *slash = strchr(key, '/');
    if (slash == NULL || (size_t)(slash - key) != 24U) return false;
    for (size_t index = 0U; index < 24U; ++index) {
        if (!isxdigit((unsigned char)key[index])) return false;
    }
    for (uint32_t index = 0U; index < *count; ++index) {
        if (strncmp(devices[index], key, 24U) == 0) return true;
    }
    if (*count >= BACKUP_DEVICE_CAPACITY) return false;
    memcpy(devices[*count], key, 24U);
    devices[*count][24] = '\0';
    (*count)++;
    return true;
}

static esp_err_t validate_pending(revlink_backup_summary_t *summary)
{
    FILE *stream = fopen(BACKUP_PENDING, "rb");
    if (stream == NULL) return ESP_ERR_NOT_FOUND;
    psa_hash_operation_t archive_hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&archive_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(stream);
        return ESP_FAIL;
    }
    uint8_t header[BACKUP_HEADER_BYTES];
    if (!read_exact(stream, header, sizeof(header))
        || psa_hash_update(&archive_hash, header, sizeof(header))
            != PSA_SUCCESS
        || memcmp(header, BACKUP_MAGIC, 8U) != 0
        || get_u32(header + 8U) != BACKUP_VERSION) {
        psa_hash_abort(&archive_hash);
        fclose(stream);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t files = get_u32(header + 12U);
    const uint64_t expected_data = get_u64(header + 16U);
    if (files > BACKUP_MAX_FILES) {
        psa_hash_abort(&archive_hash);
        fclose(stream);
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t data_bytes = 0U;
    char devices[BACKUP_DEVICE_CAPACITY][25U] = {{0}};
    uint32_t device_count = 0U;
    uint8_t buffer[BACKUP_BUFFER_BYTES];
    esp_err_t result = ESP_OK;
    for (uint32_t index = 0U; result == ESP_OK && index < files; ++index) {
        uint8_t entry[BACKUP_ENTRY_BYTES];
        if (!read_exact(stream, entry, sizeof(entry))
            || psa_hash_update(&archive_hash, entry, sizeof(entry))
                != PSA_SUCCESS
            || memcmp(entry, "RLE1", 4U) != 0) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        const uint16_t path_length = get_u16(entry + 4U);
        const uint64_t size = get_u64(entry + 8U);
        if (path_length == 0U || path_length >= BACKUP_PATH_BYTES
            || size > BACKUP_MAX_FILE_BYTES
            || entry[48U + path_length] != 0U) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        char path[BACKUP_PATH_BYTES] = {0};
        memcpy(path, entry + 48U, path_length);
        if (!safe_relative_path(path)
            || !remember_device(path, devices, &device_count)) {
            result = ESP_ERR_INVALID_ARG;
            break;
        }
        psa_hash_operation_t file_hash = PSA_HASH_OPERATION_INIT;
        if (psa_hash_setup(&file_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            result = ESP_FAIL;
            break;
        }
        uint64_t remaining = size;
        while (remaining > 0U) {
            const size_t requested =
                remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            if (!read_exact(stream, buffer, requested)
                || psa_hash_update(&file_hash, buffer, requested)
                    != PSA_SUCCESS
                || psa_hash_update(&archive_hash, buffer, requested)
                    != PSA_SUCCESS) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            remaining -= requested;
        }
        uint8_t digest[32U];
        size_t digest_length = 0U;
        if (result != ESP_OK
            || psa_hash_finish(
                &file_hash,
                digest,
                sizeof(digest),
                &digest_length
            ) != PSA_SUCCESS
            || digest_length != sizeof(digest)
            || memcmp(digest, entry + 16U, sizeof(digest)) != 0) {
            psa_hash_abort(&file_hash);
            result = ESP_ERR_INVALID_CRC;
            break;
        }
        if (UINT64_MAX - data_bytes < size) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        data_bytes += size;
    }
    if (result == ESP_OK
        && (data_bytes != expected_data || fgetc(stream) != EOF)) {
        result = ESP_ERR_INVALID_SIZE;
    }
    uint8_t archive_digest[32U];
    size_t archive_digest_length = 0U;
    if (result == ESP_OK
        && (psa_hash_finish(
                &archive_hash,
                archive_digest,
                sizeof(archive_digest),
                &archive_digest_length
            ) != PSA_SUCCESS
            || archive_digest_length != sizeof(archive_digest))) {
        result = ESP_FAIL;
    } else if (result != ESP_OK) {
        psa_hash_abort(&archive_hash);
    }
    if (result == ESP_OK) {
        memset(summary, 0, sizeof(*summary));
        summary->file_count = files;
        summary->device_count = device_count;
        summary->data_bytes = data_bytes;
        summary->archive_bytes = stage_written;
        summary->created_utc = get_u64(header + 24U);
        digest_hex(archive_digest, summary->token);
    }
    fclose(stream);
    return result;
}

esp_err_t revlink_backup_stage_begin(uint64_t archive_bytes)
{
    if (!storage_is_idle()) return ESP_ERR_INVALID_STATE;
    if (archive_bytes < BACKUP_HEADER_BYTES
        || archive_bytes > BACKUP_MAX_ARCHIVE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    revlink_backup_stage_abort();
    unlink(BACKUP_PENDING);
    stage_stream = fopen(BACKUP_PENDING, "wb");
    if (stage_stream == NULL) return ESP_FAIL;
    stage_expected = archive_bytes;
    stage_written = 0U;
    memset(&staged_summary, 0, sizeof(staged_summary));
    return ESP_OK;
}

esp_err_t revlink_backup_stage_write(const uint8_t *data, size_t length)
{
    if (stage_stream == NULL || data == NULL
        || stage_written + length > stage_expected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fwrite(data, 1U, length, stage_stream) != length) return ESP_FAIL;
    stage_written += length;
    return ESP_OK;
}

esp_err_t revlink_backup_stage_commit(revlink_backup_summary_t *summary)
{
    if (stage_stream == NULL || summary == NULL
        || stage_written != stage_expected) {
        revlink_backup_stage_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    if (fflush(stage_stream) != 0 || fsync(fileno(stage_stream)) != 0) {
        revlink_backup_stage_abort();
        return ESP_FAIL;
    }
    fclose(stage_stream);
    stage_stream = NULL;
    const esp_err_t status = validate_pending(&staged_summary);
    if (status != ESP_OK) {
        unlink(BACKUP_PENDING);
        memset(&staged_summary, 0, sizeof(staged_summary));
        return status;
    }
    *summary = staged_summary;
    return ESP_OK;
}

void revlink_backup_stage_abort(void)
{
    if (stage_stream != NULL) fclose(stage_stream);
    stage_stream = NULL;
    stage_expected = 0U;
    stage_written = 0U;
    memset(&staged_summary, 0, sizeof(staged_summary));
    unlink(BACKUP_PENDING);
}

static esp_err_t ensure_parent_directories(char *path)
{
    for (char *cursor = path + strlen(BACKUP_MOUNT_PREFIX);
         *cursor != '\0'; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(path, 0775) != 0 && errno != EEXIST) {
            *cursor = '/';
            return ESP_FAIL;
        }
        *cursor = '/';
    }
    return ESP_OK;
}

static esp_err_t skip_bytes(FILE *stream, uint64_t bytes)
{
    uint8_t buffer[BACKUP_BUFFER_BYTES];
    while (bytes > 0U) {
        const size_t requested =
            bytes < sizeof(buffer) ? (size_t)bytes : sizeof(buffer);
        if (!read_exact(stream, buffer, requested)) return ESP_FAIL;
        bytes -= requested;
    }
    return ESP_OK;
}

esp_err_t revlink_backup_restore_merge(
    const char *token,
    revlink_backup_restore_result_t *result
)
{
    if (result == NULL || !token_equal(token, staged_summary.token)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_is_idle()) return ESP_ERR_INVALID_STATE;
    FILE *archive = fopen(BACKUP_PENDING, "rb");
    if (archive == NULL) return ESP_ERR_NOT_FOUND;
    uint8_t global[BACKUP_HEADER_BYTES];
    if (!read_exact(archive, global, sizeof(global))) {
        fclose(archive);
        return ESP_FAIL;
    }
    const uint32_t files = get_u32(global + 12U);
    memset(result, 0, sizeof(*result));
    esp_err_t status = ESP_OK;
    uint8_t buffer[BACKUP_BUFFER_BYTES];
    for (uint32_t index = 0U; status == ESP_OK && index < files; ++index) {
        uint8_t entry[BACKUP_ENTRY_BYTES];
        if (!read_exact(archive, entry, sizeof(entry))) {
            status = ESP_FAIL;
            break;
        }
        const uint16_t path_length = get_u16(entry + 4U);
        const uint64_t size = get_u64(entry + 8U);
        char relative[BACKUP_PATH_BYTES] = {0};
        memcpy(relative, entry + 48U, path_length);
        char destination[384U];
        const int destination_count = snprintf(
            destination,
            sizeof(destination),
            "%s%s",
            BACKUP_MOUNT_PREFIX,
            relative
        );
        if (destination_count <= 0
            || (size_t)destination_count >= sizeof(destination)) {
            status = ESP_ERR_INVALID_SIZE;
            break;
        }
        struct stat existing;
        if (stat(destination, &existing) == 0) {
            uint8_t digest[32U];
            if (S_ISREG(existing.st_mode)
                && (uint64_t)existing.st_size == size
                && hash_file(destination, size, digest) == ESP_OK
                && memcmp(digest, entry + 16U, sizeof(digest)) == 0) {
                result->identical_files++;
            } else {
                result->conflicting_files++;
            }
            status = skip_bytes(archive, size);
            continue;
        }
        if (errno != ENOENT || ensure_parent_directories(destination) != ESP_OK) {
            status = ESP_FAIL;
            break;
        }
        char partial[sizeof(destination) + sizeof(BACKUP_PARTIAL_SUFFIX)];
        const int partial_count = snprintf(
            partial,
            sizeof(partial),
            "%s%s",
            destination,
            BACKUP_PARTIAL_SUFFIX
        );
        if (partial_count <= 0 || (size_t)partial_count >= sizeof(partial)) {
            status = ESP_ERR_INVALID_SIZE;
            break;
        }
        FILE *output = fopen(partial, "wb");
        if (output == NULL) {
            status = ESP_FAIL;
            break;
        }
        psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
        if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            fclose(output);
            unlink(partial);
            status = ESP_FAIL;
            break;
        }
        uint64_t remaining = size;
        while (status == ESP_OK && remaining > 0U) {
            const size_t requested =
                remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            if (!read_exact(archive, buffer, requested)
                || fwrite(buffer, 1U, requested, output) != requested
                || psa_hash_update(&hash, buffer, requested) != PSA_SUCCESS) {
                status = ESP_FAIL;
                break;
            }
            remaining -= requested;
        }
        uint8_t digest[32U];
        size_t digest_length = 0U;
        if (status == ESP_OK
            && (fflush(output) != 0 || fsync(fileno(output)) != 0
                || psa_hash_finish(
                    &hash,
                    digest,
                    sizeof(digest),
                    &digest_length
                ) != PSA_SUCCESS
                || digest_length != sizeof(digest)
                || memcmp(digest, entry + 16U, sizeof(digest)) != 0)) {
            status = ESP_ERR_INVALID_CRC;
        } else if (status != ESP_OK) {
            psa_hash_abort(&hash);
        }
        fclose(output);
        if (status == ESP_OK && rename(partial, destination) == 0) {
            result->restored_files++;
            result->restored_bytes += size;
        } else {
            unlink(partial);
            if (status == ESP_OK) status = ESP_FAIL;
        }
    }
    fclose(archive);
    if (status == ESP_OK) {
        unlink(BACKUP_PENDING);
        memset(&staged_summary, 0, sizeof(staged_summary));
        ESP_LOGI(
            TAG,
            "Merge restore complete: restored=%" PRIu32
            " identical=%" PRIu32 " conflicts=%" PRIu32,
            result->restored_files,
            result->identical_files,
            result->conflicting_files
        );
    }
    return status;
}
