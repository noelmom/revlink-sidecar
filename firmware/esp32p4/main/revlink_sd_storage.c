#include "revlink_sd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "revlink_local_metadata.h"
#include "revlink_sync_history.h"
#include "revlink_sync_manifest.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#define REVLINK_SD_MOUNT_POINT "/sdcard"
#define REVLINK_SD_TEST_BYTES (1024U * 1024U)
#define REVLINK_SD_CHUNK_BYTES 4096U
#define REVLINK_SD_DOWNLOAD_NAME_CAPACITY 128U
#define REVLINK_SD_DOWNLOAD_PATH_CAPACITY 320U
#define REVLINK_SD_DOWNLOAD_MAX_BYTES (8U * 1024U * 1024U)
#define REVLINK_SD_MANIFEST_MAX_BYTES (72U * 1024U)
#define REVLINK_SD_HISTORY_MAX_BYTES (320U * 1024U)
#define REVLINK_SD_ANNOTATIONS_MAX_BYTES (256U * 1024U)
#define REVLINK_SD_DEVICE_KEY_HEX_BYTES 24U
#define REVLINK_SD_LAST_DEVICE_PATH \
    REVLINK_SD_MOUNT_POINT "/revlink/last-device"
#define REVLINK_SD_LAST_DEVICE_TEMP_PATH \
    REVLINK_SD_MOUNT_POINT "/revlink/last-device.tmp"
#define REVLINK_SD_LAST_DEVICE_BACKUP_PATH \
    REVLINK_SD_MOUNT_POINT "/revlink/last-device.bak"
#define REVLINK_SD_STARTUP_SCREEN_BYTES 153600U

static const char *TAG = "revlink_sd";

static sdmmc_host_t storage_host;
#if CONFIG_REVLINK_SD_SPI_TRANSPORT
static sdspi_device_config_t storage_spi_slot;
static bool storage_spi_bus_initialized;
#else
static sdmmc_slot_config_t storage_slot;
#endif
static sdmmc_card_t *storage_card;
static sd_pwr_ctrl_handle_t storage_power;
static volatile revlink_sd_storage_state_t storage_state =
    REVLINK_SD_STORAGE_UNKNOWN;
static volatile esp_err_t storage_last_error = ESP_OK;

typedef struct {
    FILE *stream;
    psa_hash_operation_t hash;
    bool hash_active;
    revlink_sd_file_kind_t kind;
    uint32_t expected_size;
    uint32_t written_size;
    uint32_t device_time_raw;
    char device_path[REVLINK_SYNC_PATH_CAPACITY];
    char temporary_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char final_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
} download_writer_t;

typedef struct {
    bool selected;
    revlink_ap_device_info_t identity;
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    char base[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char current_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char history_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char object_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char map_object_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char image_object_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char screenshot_object_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char startup_profile_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char temporary_dir[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char identity_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char manifest_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char manifest_temp_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char manifest_backup_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char history_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char history_temp_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char history_backup_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
} storage_device_t;

struct revlink_sd_cached_reader {
    FILE *stream;
    uint32_t remaining;
};

static download_writer_t download_writer;
static storage_device_t storage_device;
static revlink_sync_manifest_t *sync_manifest;
static revlink_sync_history_t *sync_history;
static bool sync_manifest_ready;
static bool sync_history_ready;
static SemaphoreHandle_t portal_snapshot_mutex;
static revlink_sd_portal_snapshot_t portal_snapshot;
static void *trusted_time_context;
static revlink_sd_trusted_time_t trusted_time_provider;
static void *identity_observer_context;
static revlink_sd_identity_observer_t identity_observer;
static FILE *startup_profile_stream;
static uint32_t startup_profile_written;
static char startup_profile_id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY];
static char startup_profile_name[REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY];
static char startup_profile_key[REVLINK_SD_DEVICE_KEY_CAPACITY];
static char startup_profile_temp[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];

/*
 * One device-listing pass. Paths are buffered as a packed run of NUL
 * terminated strings rather than applied on arrival: the listing precedes the
 * downloads it triggers, so a file seen for the first time on this sync has
 * no manifest entry yet when it is observed.
 *
 * The pool is deliberately finite, and running out of it is treated as
 * failure of the whole pass rather than as a shorter listing. Truncating the
 * evidence and then acting on it is how a partial listing turns into a false
 * claim that the owner's files are gone.
 */
#define REVLINK_SD_DEVICE_SCAN_POOL_BYTES 16384U

static char *device_scan_pool;
static size_t device_scan_used;
static bool device_scan_active;
static bool device_scan_overflowed;

void revlink_sd_storage_configure_time_source(
    void *context,
    revlink_sd_trusted_time_t trusted_time
)
{
    trusted_time_context = context;
    trusted_time_provider = trusted_time;
}

void revlink_sd_storage_configure_identity_observer(
    void *context,
    revlink_sd_identity_observer_t observer
)
{
    identity_observer_context = context;
    identity_observer = observer;
}

static uint64_t trusted_time_now(void)
{
    return trusted_time_provider != NULL
        ? trusted_time_provider(trusted_time_context)
        : 0U;
}

static esp_err_t format_path(
    char *output,
    size_t capacity,
    const char *format,
    ...
);
static esp_err_t derive_device_key(
    const char *serial,
    char output[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U]
);
static esp_err_t make_directory(const char *path);
static revlink_sd_file_kind_t kind_for_manifest_path(const char *path);
static bool copy_identity_value(
    const char *contents,
    const char *name,
    char *target,
    size_t target_capacity
);
static esp_err_t selected_startup_profile_dir(
    char *directory,
    size_t directory_capacity,
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U]
);
static esp_err_t storage_start_internal(bool format_authorized);

_Static_assert(
    REVLINK_SD_PORTAL_PATH_CAPACITY >= REVLINK_SYNC_PATH_CAPACITY,
    "Portal path projection must hold every manifest path"
);

static void publish_portal_snapshot(void)
{
    if (portal_snapshot_mutex == NULL) {
        return;
    }

    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    if (
        storage_card != NULL
        && esp_vfs_fat_info(
               REVLINK_SD_MOUNT_POINT,
               &total_bytes,
               &free_bytes
           ) != ESP_OK
    ) {
        total_bytes = 0U;
        free_bytes = 0U;
    }

    if (xSemaphoreTake(portal_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portal_snapshot.mounted = storage_card != NULL;
    portal_snapshot.total_bytes = total_bytes;
    portal_snapshot.free_bytes = free_bytes;
    portal_snapshot.session_selected = storage_device.selected;
    if (
        storage_device.selected
        && sync_manifest_ready
        && sync_manifest != NULL
    ) {
        portal_snapshot.namespace_known = true;
        portal_snapshot.device = storage_device.identity;
        portal_snapshot.total_files = sync_manifest->count;
        portal_snapshot.listed_files =
            sync_manifest->count < REVLINK_SD_PORTAL_INVENTORY_CAPACITY
            ? sync_manifest->count
            : REVLINK_SD_PORTAL_INVENTORY_CAPACITY;
        memset(
            portal_snapshot.files,
            0,
            sizeof(portal_snapshot.files)
        );
        for (size_t index = 0U;
             index < portal_snapshot.listed_files;
             ++index) {
            const revlink_sync_manifest_entry_t *source =
                &sync_manifest->entries[index];
            revlink_sd_portal_file_t *target =
                &portal_snapshot.files[index];
            memcpy(
                target->path,
                source->path,
                strlen(source->path) + 1U
            );
            target->kind = kind_for_manifest_path(source->path);
            target->device_time_raw = source->device_time_raw;
            target->size = source->size;
            target->initial_sync_utc = source->initial_sync_utc;
            memcpy(
                target->sha256,
                source->sha256,
                sizeof(target->sha256)
            );
            target->presence = source->presence;
        }
    }
    xSemaphoreGive(portal_snapshot_mutex);
}

esp_err_t revlink_sd_portal_snapshot(
    revlink_sd_portal_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = portal_snapshot;
    xSemaphoreGive(portal_snapshot_mutex);
    return ESP_OK;
}

esp_err_t revlink_sd_portal_io_status(
    bool *mounted,
    bool *session_selected
)
{
    if (mounted == NULL || session_selected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    *mounted = portal_snapshot.mounted;
    *session_selected = portal_snapshot.session_selected;
    xSemaphoreGive(portal_snapshot_mutex);
    return ESP_OK;
}

bool revlink_sd_selected_device_matches(
    const revlink_ap_device_info_t *identity
)
{
    if (identity == NULL || identity->serial[0] == '\0'
        || portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE) {
        return false;
    }
    const bool matches =
        portal_snapshot.namespace_known
        && portal_snapshot.device.serial[0] != '\0'
        && strcmp(portal_snapshot.device.serial, identity->serial) == 0;
    xSemaphoreGive(portal_snapshot_mutex);
    return matches;
}

static esp_err_t selected_startup_profile_dir(
    char *directory,
    size_t directory_capacity,
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U]
)
{
    if (directory == NULL || key == NULL || storage_card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    revlink_ap_device_info_t identity = {0};
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool known = portal_snapshot.namespace_known;
    identity = portal_snapshot.device;
    xSemaphoreGive(portal_snapshot_mutex);
    if (!known || identity.serial[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (derive_device_key(identity.serial, key) != ESP_OK
        || format_path(
               directory,
               directory_capacity,
               REVLINK_SD_MOUNT_POINT
               "/revlink/devices/%s/startup-library",
               key
           ) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t annotation_paths(
    char *path,
    size_t path_capacity,
    char *temporary,
    size_t temporary_capacity,
    char *backup,
    size_t backup_capacity
)
{
    revlink_ap_device_info_t identity = {0};
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool known = portal_snapshot.namespace_known;
    identity = portal_snapshot.device;
    xSemaphoreGive(portal_snapshot_mutex);
    if (!known || identity.serial[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    if (derive_device_key(identity.serial, key) != ESP_OK
        || format_path(
               path,
               path_capacity,
               REVLINK_SD_MOUNT_POINT
               "/revlink/devices/%s/annotations.tsv",
               key
           ) != ESP_OK
        || format_path(temporary, temporary_capacity, "%s.tmp", path) != ESP_OK
        || format_path(backup, backup_capacity, "%s.bak", path) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t load_annotations(
    const char *path,
    const char *temporary,
    const char *backup,
    revlink_sync_annotations_t *annotations
)
{
    revlink_sync_annotations_init(annotations);
    struct stat info;
    if (stat(path, &info) != 0) {
        if (errno != ENOENT) return ESP_FAIL;
        struct stat backup_info;
        if (stat(backup, &backup_info) == 0) {
            if (rename(backup, path) != 0 || stat(path, &info) != 0) {
                return ESP_FAIL;
            }
        } else {
            unlink(temporary);
            return ESP_OK;
        }
    }
    if (info.st_size <= 0
        || (uint64_t)info.st_size > REVLINK_SD_ANNOTATIONS_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *stream = fopen(path, "rb");
    char *contents = malloc((size_t)info.st_size);
    if (stream == NULL || contents == NULL) {
        if (stream != NULL) fclose(stream);
        free(contents);
        return ESP_ERR_NO_MEM;
    }
    const size_t count = fread(contents, 1U, (size_t)info.st_size, stream);
    bool failed = count != (size_t)info.st_size || ferror(stream);
    if (fclose(stream) != 0) failed = true;
    if (failed) {
        free(contents);
        return ESP_FAIL;
    }
    const revlink_sync_annotation_status_t status =
        revlink_sync_annotations_parse(contents, count, annotations);
    free(contents);
    if (status != REVLINK_SYNC_ANNOTATION_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    unlink(temporary);
    unlink(backup);
    return ESP_OK;
}

static esp_err_t save_annotations(
    const char *path,
    const char *temporary,
    const char *backup,
    const revlink_sync_annotations_t *annotations
)
{
    char *serialized = malloc(REVLINK_SD_ANNOTATIONS_MAX_BYTES);
    if (serialized == NULL) return ESP_ERR_NO_MEM;
    size_t length = 0U;
    const revlink_sync_annotation_status_t encode =
        revlink_sync_annotations_serialize(
            annotations,
            serialized,
            REVLINK_SD_ANNOTATIONS_MAX_BYTES,
            &length
        );
    if (encode != REVLINK_SYNC_ANNOTATION_OK) {
        free(serialized);
        return ESP_ERR_INVALID_SIZE;
    }
    unlink(temporary);
    FILE *stream = fopen(temporary, "wb");
    if (stream == NULL) {
        free(serialized);
        return ESP_FAIL;
    }
    bool failed =
        fwrite(serialized, 1U, length, stream) != length
        || fflush(stream) != 0 || fsync(fileno(stream)) != 0;
    if (fclose(stream) != 0) failed = true;
    free(serialized);
    if (failed) {
        unlink(temporary);
        return ESP_FAIL;
    }
    struct stat current;
    const bool had_current = stat(path, &current) == 0;
    unlink(backup);
    if (had_current && rename(path, backup) != 0) {
        unlink(temporary);
        return ESP_FAIL;
    }
    if (rename(temporary, path) != 0) {
        if (had_current) rename(backup, path);
        unlink(temporary);
        return ESP_FAIL;
    }
    unlink(backup);
    return ESP_OK;
}

esp_err_t revlink_sd_annotations_snapshot(
    revlink_sync_annotations_t *annotations
)
{
    if (annotations == NULL) return ESP_ERR_INVALID_ARG;
    char path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char temporary[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char backup[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    esp_err_t status = annotation_paths(
        path, sizeof(path), temporary, sizeof(temporary), backup, sizeof(backup)
    );
    if (status == ESP_OK) {
        status = load_annotations(path, temporary, backup, annotations);
    }
    return status;
}

esp_err_t revlink_sd_annotation_set(
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const char *note,
    size_t note_length
)
{
    if (sha256 == NULL || note == NULL) return ESP_ERR_INVALID_ARG;
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    bool known_version = false;
    for (
        size_t index = 0U;
        index < portal_snapshot.listed_files;
        ++index
    ) {
        if (memcmp(
                portal_snapshot.files[index].sha256,
                sha256,
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES
            ) == 0) {
            known_version = true;
            break;
        }
    }
    xSemaphoreGive(portal_snapshot_mutex);
    if (!known_version) return ESP_ERR_NOT_FOUND;

    char path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char temporary[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char backup[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    esp_err_t status = annotation_paths(
        path, sizeof(path), temporary, sizeof(temporary), backup, sizeof(backup)
    );
    revlink_sync_annotations_t *annotations = calloc(1U, sizeof(*annotations));
    if (annotations == NULL) return ESP_ERR_NO_MEM;
    if (status == ESP_OK) {
        status = load_annotations(path, temporary, backup, annotations);
    }
    if (
        status == ESP_OK
        && revlink_sync_annotations_set(
               annotations,
               sha256,
               note,
               note_length,
               trusted_time_now()
           ) != REVLINK_SYNC_ANNOTATION_OK
    ) {
        status = ESP_ERR_INVALID_ARG;
    }
    if (status == ESP_OK) {
        status = save_annotations(
            path, temporary, backup, annotations
        );
    }
    free(annotations);
    return status;
}

esp_err_t revlink_sd_annotation_set_map(
    const uint8_t log_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const uint8_t map_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
)
{
    if (log_sha256 == NULL) return ESP_ERR_INVALID_ARG;
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    bool known_log = false;
    bool known_map = map_sha256 == NULL;
    for (
        size_t index = 0U;
        index < portal_snapshot.listed_files;
        ++index
    ) {
        const revlink_sd_portal_file_t *file =
            &portal_snapshot.files[index];
        if (
            file->kind == REVLINK_SD_FILE_DATALOG
            && memcmp(
                   file->sha256,
                   log_sha256,
                   REVLINK_SYNC_ANNOTATION_SHA256_BYTES
               ) == 0
        ) {
            known_log = true;
        }
        if (
            map_sha256 != NULL
            && file->kind == REVLINK_SD_FILE_MAP
            && memcmp(
                   file->sha256,
                   map_sha256,
                   REVLINK_SYNC_ANNOTATION_SHA256_BYTES
               ) == 0
        ) {
            known_map = true;
        }
    }
    xSemaphoreGive(portal_snapshot_mutex);
    if (!known_log || !known_map) return ESP_ERR_NOT_FOUND;

    char path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char temporary[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char backup[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    esp_err_t status = annotation_paths(
        path, sizeof(path), temporary, sizeof(temporary), backup, sizeof(backup)
    );
    revlink_sync_annotations_t *annotations = calloc(1U, sizeof(*annotations));
    if (annotations == NULL) return ESP_ERR_NO_MEM;
    if (status == ESP_OK) {
        status = load_annotations(path, temporary, backup, annotations);
    }
    if (
        status == ESP_OK
        && revlink_sync_annotations_set_map(
               annotations,
               log_sha256,
               map_sha256,
               trusted_time_now()
           ) != REVLINK_SYNC_ANNOTATION_OK
    ) {
        status = ESP_ERR_INVALID_ARG;
    }
    if (status == ESP_OK) {
        status = save_annotations(
            path, temporary, backup, annotations
        );
    }
    free(annotations);
    return status;
}

static bool path_has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) {
        return false;
    }
    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length
        && memcmp(
               path + path_length - suffix_length,
               suffix,
               suffix_length
           ) == 0;
}

esp_err_t revlink_sd_cached_reader_open(
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    revlink_sd_cached_reader_t **reader,
    revlink_sd_cached_file_info_t *info
)
{
    if (sha256 == NULL || reader == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *reader = NULL;
    memset(info, 0, sizeof(*info));

    revlink_sd_portal_file_t selected = {0};
    revlink_ap_device_info_t identity = {0};
    bool found = false;
    if (
        portal_snapshot_mutex == NULL
        || xSemaphoreTake(portal_snapshot_mutex, pdMS_TO_TICKS(250))
            != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    if (portal_snapshot.mounted && portal_snapshot.namespace_known) {
        identity = portal_snapshot.device;
        for (
            size_t index = 0U;
            index < portal_snapshot.listed_files;
            ++index
        ) {
            if (memcmp(
                    portal_snapshot.files[index].sha256,
                    sha256,
                    REVLINK_SYNC_ANNOTATION_SHA256_BYTES
                ) == 0) {
                selected = portal_snapshot.files[index];
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(portal_snapshot_mutex);
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    char digest_text[REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U + 1U];
    static const char hex[] = "0123456789abcdef";
    for (
        size_t index = 0U;
        index < REVLINK_SYNC_ANNOTATION_SHA256_BYTES;
        ++index
    ) {
        digest_text[index * 2U] = hex[sha256[index] >> 4U];
        digest_text[index * 2U + 1U] = hex[sha256[index] & 0x0fU];
    }
    digest_text[sizeof(digest_text) - 1U] = '\0';

    const bool gzip_encoded =
        selected.kind == REVLINK_SD_FILE_DATALOG
        && path_has_suffix(selected.path, ".csv.gz");
    const char *extension =
        selected.kind == REVLINK_SD_FILE_MAP ? ".ptm"
        : selected.kind == REVLINK_SD_FILE_STARTUP_SCREEN ? ".fb"
        : selected.kind == REVLINK_SD_FILE_SCREENSHOT
            ? (path_has_suffix(selected.path, ".bmp") ? ".bmp" : ".png")
        : gzip_encoded ? ".csv.gz" : ".csv";
    const char *collection =
        selected.kind == REVLINK_SD_FILE_MAP ? "maps"
        : selected.kind == REVLINK_SD_FILE_STARTUP_SCREEN ? "images"
        : selected.kind == REVLINK_SD_FILE_SCREENSHOT ? "screenshots"
        : "datalogs";
    char object_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (
        derive_device_key(identity.serial, key) != ESP_OK
        || format_path(
               object_path,
               sizeof(object_path),
               REVLINK_SD_MOUNT_POINT
               "/revlink/devices/%s/objects/%s/%s%s",
               key,
               collection,
               digest_text,
               extension
           ) != ESP_OK
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat object;
    if (
        stat(object_path, &object) != 0
        || !S_ISREG(object.st_mode)
        || object.st_size < 0
        || (uint64_t)object.st_size != selected.size
    ) {
        ESP_LOGW(
            TAG,
            "Portal cache object unavailable or size mismatch: errno=%d",
            errno
        );
        return ESP_ERR_INVALID_RESPONSE;
    }

    revlink_sd_cached_reader_t *opened = calloc(1U, sizeof(*opened));
    if (opened == NULL) {
        return ESP_ERR_NO_MEM;
    }
    opened->stream = fopen(object_path, "rb");
    if (opened->stream == NULL) {
        free(opened);
        return ESP_FAIL;
    }
    opened->remaining = selected.size;
    memcpy(info->path, selected.path, strlen(selected.path) + 1U);
    info->kind = selected.kind;
    info->size = selected.size;
    info->gzip_encoded = gzip_encoded;
    *reader = opened;
    return ESP_OK;
}

esp_err_t revlink_sd_cached_reader_read(
    revlink_sd_cached_reader_t *reader,
    uint8_t *buffer,
    size_t capacity,
    size_t *count
)
{
    if (
        reader == NULL
        || reader->stream == NULL
        || buffer == NULL
        || capacity == 0U
        || count == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    if (reader->remaining == 0U) {
        return ESP_OK;
    }
    const size_t requested =
        reader->remaining < capacity ? reader->remaining : capacity;
    const size_t received = fread(buffer, 1U, requested, reader->stream);
    if (received == 0U && ferror(reader->stream)) {
        return ESP_FAIL;
    }
    reader->remaining -= (uint32_t)received;
    *count = received;
    return ESP_OK;
}

void revlink_sd_cached_reader_close(revlink_sd_cached_reader_t *reader)
{
    if (reader == NULL) {
        return;
    }
    if (reader->stream != NULL) {
        fclose(reader->stream);
    }
    free(reader);
}

static bool startup_profile_id_is_valid(const char *id)
{
    if (id == NULL) return false;
    const size_t length =
        strnlen(id, REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY);
    if (length == 0U || length >= REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char value = id[index];
        if (!((value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9')
                || value == '-')) {
            return false;
        }
    }
    return true;
}

static bool startup_profile_id_from_name(
    const char *name,
    char output[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY]
)
{
    if (name == NULL || output == NULL) return false;
    const size_t length =
        strnlen(name, REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY);
    if (length == 0U || length >= REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY) {
        return false;
    }
    size_t used = 0U;
    bool separator = false;
    for (size_t index = 0U; index < length
         && used + 1U < REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY; ++index) {
        unsigned char value = (unsigned char)name[index];
        if (value >= 'A' && value <= 'Z') {
            value = (unsigned char)(value + ('a' - 'A'));
        }
        if ((value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')) {
            output[used++] = (char)value;
            separator = false;
        } else if (used > 0U && !separator) {
            output[used++] = '-';
            separator = true;
        }
    }
    while (used > 0U && output[used - 1U] == '-') --used;
    output[used] = '\0';
    return used > 0U;
}

void revlink_sd_startup_profile_abort(void)
{
    if (startup_profile_stream != NULL) {
        fclose(startup_profile_stream);
        startup_profile_stream = NULL;
    }
    if (startup_profile_temp[0] != '\0') {
        unlink(startup_profile_temp);
    }
    startup_profile_written = 0U;
    startup_profile_id[0] = '\0';
    startup_profile_name[0] = '\0';
    startup_profile_key[0] = '\0';
    startup_profile_temp[0] = '\0';
}

esp_err_t revlink_sd_startup_profile_begin(
    const char *name,
    uint32_t size
)
{
    if (startup_profile_stream != NULL
        || size != REVLINK_SD_STARTUP_SCREEN_BYTES
        || !startup_profile_id_from_name(name, startup_profile_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    char directory[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (selected_startup_profile_dir(
            directory,
            sizeof(directory),
            startup_profile_key
        ) != ESP_OK
        || make_directory(directory) != ESP_OK) {
        revlink_sd_startup_profile_abort();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t name_length =
        strnlen(name, REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY);
    memcpy(startup_profile_name, name, name_length + 1U);
    if (format_path(
            startup_profile_temp,
            sizeof(startup_profile_temp),
            "%s/.profile.tmp",
            directory
        ) != ESP_OK) {
        revlink_sd_startup_profile_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    unlink(startup_profile_temp);
    startup_profile_stream = fopen(startup_profile_temp, "wb");
    if (startup_profile_stream == NULL) {
        revlink_sd_startup_profile_abort();
        return ESP_FAIL;
    }
    startup_profile_written = 0U;
    return ESP_OK;
}

esp_err_t revlink_sd_startup_profile_write(
    const uint8_t *data,
    size_t length
)
{
    if (data == NULL || length == 0U || startup_profile_stream == NULL
        || length > REVLINK_SD_STARTUP_SCREEN_BYTES
            - startup_profile_written
        || fwrite(data, 1U, length, startup_profile_stream) != length) {
        revlink_sd_startup_profile_abort();
        return ESP_FAIL;
    }
    startup_profile_written += (uint32_t)length;
    return ESP_OK;
}

esp_err_t revlink_sd_startup_profile_commit(char *id, size_t capacity)
{
    char current_key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    char directory[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (id == NULL || capacity <= strlen(startup_profile_id)
        || startup_profile_stream == NULL
        || startup_profile_written != REVLINK_SD_STARTUP_SCREEN_BYTES
        || selected_startup_profile_dir(
               directory,
               sizeof(directory),
               current_key
           ) != ESP_OK
        || strcmp(startup_profile_key, current_key) != 0) {
        revlink_sd_startup_profile_abort();
        return ESP_ERR_INVALID_STATE;
    }
    const bool flushed =
        fflush(startup_profile_stream) == 0
        && fsync(fileno(startup_profile_stream)) == 0
        && fclose(startup_profile_stream) == 0;
    startup_profile_stream = NULL;
    char framebuffer_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char name_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (!flushed
        || format_path(
               framebuffer_path,
               sizeof(framebuffer_path),
               "%s/%s.fb",
               directory,
               startup_profile_id
           ) != ESP_OK
        || format_path(
               name_path,
               sizeof(name_path),
               "%s/%s.name",
               directory,
               startup_profile_id
           ) != ESP_OK) {
        revlink_sd_startup_profile_abort();
        return ESP_FAIL;
    }
    unlink(framebuffer_path);
    if (rename(startup_profile_temp, framebuffer_path) != 0) {
        revlink_sd_startup_profile_abort();
        return ESP_FAIL;
    }
    FILE *name_stream = fopen(name_path, "wb");
    const size_t name_length = strlen(startup_profile_name);
    bool name_saved = name_stream != NULL;
    if (name_saved) {
        name_saved =
            fwrite(startup_profile_name, 1U, name_length, name_stream)
                == name_length
            && fflush(name_stream) == 0
            && fsync(fileno(name_stream)) == 0;
        if (fclose(name_stream) != 0) name_saved = false;
    }
    if (!name_saved) {
        unlink(framebuffer_path);
        unlink(name_path);
        revlink_sd_startup_profile_abort();
        return ESP_FAIL;
    }
    memcpy(id, startup_profile_id, strlen(startup_profile_id) + 1U);
    startup_profile_written = 0U;
    startup_profile_id[0] = '\0';
    startup_profile_name[0] = '\0';
    startup_profile_key[0] = '\0';
    startup_profile_temp[0] = '\0';
    return ESP_OK;
}

/*
 * Keep every unique startup framebuffer as a reusable profile. The current
 * manifest intentionally follows the device path, so without this snapshot a
 * later replacement would make the previous screen disappear from the normal
 * UI even though its immutable cache object still exists.
 */
static esp_err_t preserve_synced_startup_profile(
    const char *source_path,
    const uint8_t digest[REVLINK_SYNC_SHA256_BYTES]
)
{
    if (source_path == NULL || digest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char digest_prefix[13];
    for (size_t index = 0U; index < 6U; ++index) {
        snprintf(&digest_prefix[index * 2U], 3U, "%02x", digest[index]);
    }
    digest_prefix[12] = '\0';

    char name[REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY];
    const int name_count = snprintf(
        name,
        sizeof(name),
        "Synced startup %s",
        digest_prefix
    );
    char id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY];
    if (name_count < 0 || (size_t)name_count >= sizeof(name)
        || !startup_profile_id_from_name(name, id)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char directory[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    char profile_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (selected_startup_profile_dir(
            directory,
            sizeof(directory),
            key
        ) != ESP_OK
        || make_directory(directory) != ESP_OK
        || format_path(
               profile_path,
               sizeof(profile_path),
               "%s/%s.fb",
               directory,
               id
           ) != ESP_OK) {
        return ESP_FAIL;
    }

    struct stat existing;
    if (stat(profile_path, &existing) == 0
        && existing.st_size == REVLINK_SD_STARTUP_SCREEN_BYTES) {
        return ESP_OK;
    }

    FILE *source = fopen(source_path, "rb");
    if (source == NULL) {
        return ESP_FAIL;
    }
    esp_err_t status = revlink_sd_startup_profile_begin(
        name,
        REVLINK_SD_STARTUP_SCREEN_BYTES
    );
    uint8_t *buffer = NULL;
    if (status == ESP_OK) {
        buffer = malloc(REVLINK_SD_CHUNK_BYTES);
        if (buffer == NULL) status = ESP_ERR_NO_MEM;
    }
    uint32_t copied = 0U;
    while (status == ESP_OK && copied < REVLINK_SD_STARTUP_SCREEN_BYTES) {
        const size_t requested =
            REVLINK_SD_STARTUP_SCREEN_BYTES - copied < REVLINK_SD_CHUNK_BYTES
            ? REVLINK_SD_STARTUP_SCREEN_BYTES - copied
            : REVLINK_SD_CHUNK_BYTES;
        const size_t count = fread(buffer, 1U, requested, source);
        if (count != requested
            || revlink_sd_startup_profile_write(buffer, count) != ESP_OK) {
            status = ESP_FAIL;
            break;
        }
        copied += (uint32_t)count;
    }
    free(buffer);
    if (fclose(source) != 0 && status == ESP_OK) status = ESP_FAIL;
    if (status != ESP_OK) {
        revlink_sd_startup_profile_abort();
        return status;
    }

    char committed_id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY];
    status = revlink_sd_startup_profile_commit(
        committed_id,
        sizeof(committed_id)
    );
    if (status == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Preserved synced startup screen as reusable profile: id=%s",
            committed_id
        );
    }
    return status;
}

esp_err_t revlink_sd_startup_profiles_snapshot(
    revlink_sd_startup_profiles_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    char profile_directory[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    const esp_err_t directory_status = selected_startup_profile_dir(
        profile_directory,
        sizeof(profile_directory),
        key
    );
    if (directory_status != ESP_OK) {
        return directory_status;
    }
    DIR *directory = opendir(profile_directory);
    if (directory == NULL) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    struct dirent *entry = NULL;
    while (snapshot->count < REVLINK_SD_STARTUP_PROFILE_CAPACITY
        && (entry = readdir(directory)) != NULL) {
        const size_t length = strlen(entry->d_name);
        if (length <= 3U || strcmp(entry->d_name + length - 3U, ".fb") != 0) {
            continue;
        }
        revlink_sd_startup_profile_t *profile =
            &snapshot->profiles[snapshot->count];
        const size_t id_length = length - 3U;
        if (id_length >= sizeof(profile->id)) continue;
        memcpy(profile->id, entry->d_name, id_length);
        profile->id[id_length] = '\0';
        if (!startup_profile_id_is_valid(profile->id)) continue;
        char framebuffer_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
        char name_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
        if (format_path(
                framebuffer_path,
                sizeof(framebuffer_path),
                "%s/%s",
                profile_directory,
                entry->d_name
            ) != ESP_OK
            || format_path(
                   name_path,
                   sizeof(name_path),
                   "%s/%s.name",
                   profile_directory,
                   profile->id
               ) != ESP_OK) {
            continue;
        }
        struct stat info;
        if (stat(framebuffer_path, &info) != 0
            || info.st_size != REVLINK_SD_STARTUP_SCREEN_BYTES) {
            continue;
        }
        profile->size = REVLINK_SD_STARTUP_SCREEN_BYTES;
        FILE *name_stream = fopen(name_path, "rb");
        if (name_stream != NULL) {
            const size_t count = fread(
                profile->name,
                1U,
                sizeof(profile->name) - 1U,
                name_stream
            );
            profile->name[count] = '\0';
            fclose(name_stream);
        }
        if (profile->name[0] == '\0') {
            memcpy(profile->name, profile->id, id_length + 1U);
        }
        ++snapshot->count;
    }
    closedir(directory);
    return ESP_OK;
}

esp_err_t revlink_sd_startup_profile_reader_open(
    const char *id,
    revlink_sd_cached_reader_t **reader,
    revlink_sd_startup_profile_t *profile
)
{
    if (!startup_profile_id_is_valid(id) || reader == NULL
        || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    revlink_sd_startup_profiles_snapshot_t snapshot;
    if (revlink_sd_startup_profiles_snapshot(&snapshot) != ESP_OK) {
        return ESP_FAIL;
    }
    const revlink_sd_startup_profile_t *selected = NULL;
    for (size_t index = 0U; index < snapshot.count; ++index) {
        if (strcmp(snapshot.profiles[index].id, id) == 0) {
            selected = &snapshot.profiles[index];
            break;
        }
    }
    if (selected == NULL) return ESP_ERR_NOT_FOUND;
    char profile_directory[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U];
    if (selected_startup_profile_dir(
            profile_directory,
            sizeof(profile_directory),
            key
        ) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (format_path(
            path,
            sizeof(path),
            "%s/%s.fb",
            profile_directory,
            id
        ) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    revlink_sd_cached_reader_t *opened = calloc(1U, sizeof(*opened));
    if (opened == NULL) return ESP_ERR_NO_MEM;
    opened->stream = fopen(path, "rb");
    if (opened->stream == NULL) {
        free(opened);
        return ESP_FAIL;
    }
    opened->remaining = REVLINK_SD_STARTUP_SCREEN_BYTES;
    *profile = *selected;
    *reader = opened;
    return ESP_OK;
}

bool revlink_sd_cached_file_matches(
    const char *path,
    uint32_t size,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
)
{
    if (path == NULL || sha256 == NULL || !storage_device.selected
        || !sync_manifest_ready || sync_manifest == NULL) {
        return false;
    }
    const size_t path_length = strnlen(path, REVLINK_SYNC_PATH_CAPACITY);
    if (path_length == 0U || path_length >= REVLINK_SYNC_PATH_CAPACITY) {
        return false;
    }
    const revlink_sync_manifest_entry_t *entry =
        revlink_sync_manifest_find(
            sync_manifest,
            (const uint8_t *)path,
            path_length
        );
    return entry != NULL
        && entry->size == size
        && memcmp(entry->sha256, sha256, REVLINK_SYNC_SHA256_BYTES) == 0;
}

static esp_err_t make_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "mkdir failed for %s: errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t format_path(char *output, size_t capacity, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vsnprintf(output, capacity, format, arguments);
    va_end(arguments);
    return count < 0 || (size_t)count >= capacity
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}

static esp_err_t provision_layout(void)
{
    static const char *const paths[] = {
        REVLINK_SD_MOUNT_POINT "/revlink",
        REVLINK_SD_MOUNT_POINT "/revlink/devices",
        REVLINK_SD_MOUNT_POINT "/revlink/system",
        REVLINK_SD_MOUNT_POINT "/revlink/system/config",
        REVLINK_SD_MOUNT_POINT "/revlink/system/updates",
        REVLINK_SD_MOUNT_POINT "/revlink/system/uploads",
        REVLINK_SD_MOUNT_POINT "/revlink/system/recovery",
        REVLINK_SD_MOUNT_POINT "/revlink/system/acceptance",
    };

    for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        const esp_err_t status = make_directory(paths[index]);
        if (status != ESP_OK) {
            return status;
        }
    }
    return ESP_OK;
}

static bool valid_download_name(const uint8_t *name, size_t length)
{
    if (name == NULL || length == 0U
        || length >= REVLINK_SD_DOWNLOAD_NAME_CAPACITY) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const uint8_t value = name[index];
        const bool allowed =
            (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9')
            || value == '.' || value == '_' || value == '-'
            || value == ' ' || value == '(' || value == ')';
        if (!allowed) {
            return false;
        }
    }
    return !(length == 1U && name[0] == '.')
        && !(length == 2U && name[0] == '.' && name[1] == '.');
}

static bool name_has_extension(
    const uint8_t *name,
    size_t name_length,
    const char *extension
)
{
    const size_t extension_length = strlen(extension);
    return name_length > extension_length
        && memcmp(
               name + name_length - extension_length,
               extension,
               extension_length
           ) == 0;
}

static bool classify_download_path(
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    revlink_sd_file_kind_t *kind
)
{
    static const uint8_t datalog_prefix[] = "datalog/";
    static const uint8_t map_prefix[] = "maps/";
    static const uint8_t startup_path[] = "images/startup_screen.fb";
    static const uint8_t screenshot_prefix[] = "screenshots/";
    if (!valid_download_name(name, name_length) || path == NULL
        || kind == NULL) {
        return false;
    }

    if (path_length == sizeof(datalog_prefix) - 1U + name_length
        && memcmp(path, datalog_prefix, sizeof(datalog_prefix) - 1U) == 0
        && memcmp(
               path + sizeof(datalog_prefix) - 1U,
               name,
               name_length
           ) == 0
        && (name_has_extension(name, name_length, ".csv")
            || name_has_extension(name, name_length, ".csv.gz"))) {
        *kind = REVLINK_SD_FILE_DATALOG;
        return true;
    }

    if (path_length == sizeof(map_prefix) - 1U + name_length
        && memcmp(path, map_prefix, sizeof(map_prefix) - 1U) == 0
        && memcmp(
               path + sizeof(map_prefix) - 1U,
               name,
               name_length
           ) == 0
        && name_has_extension(name, name_length, ".ptm")) {
        *kind = REVLINK_SD_FILE_MAP;
        return true;
    }
    if (path_length == sizeof(startup_path) - 1U
        && name_length == sizeof("startup_screen.fb") - 1U
        && memcmp(path, startup_path, sizeof(startup_path) - 1U) == 0
        && memcmp(name, "startup_screen.fb", name_length) == 0) {
        *kind = REVLINK_SD_FILE_STARTUP_SCREEN;
        return true;
    }
    if (path_length == sizeof(screenshot_prefix) - 1U + name_length
        && memcmp(
               path,
               screenshot_prefix,
               sizeof(screenshot_prefix) - 1U
           ) == 0
        && memcmp(
               path + sizeof(screenshot_prefix) - 1U,
               name,
               name_length
           ) == 0
        && (name_has_extension(name, name_length, ".png")
            || name_has_extension(name, name_length, ".bmp"))) {
        *kind = REVLINK_SD_FILE_SCREENSHOT;
        return true;
    }
    return false;
}

static revlink_sd_file_kind_t kind_for_manifest_path(const char *path)
{
    if (path != NULL && strncmp(path, "maps/", 5U) == 0) {
        return REVLINK_SD_FILE_MAP;
    }
    if (path != NULL && strcmp(path, "images/startup_screen.fb") == 0) {
        return REVLINK_SD_FILE_STARTUP_SCREEN;
    }
    if (path != NULL && strncmp(path, "screenshots/", 12U) == 0) {
        return REVLINK_SD_FILE_SCREENSHOT;
    }
    return REVLINK_SD_FILE_DATALOG;
}

static const char *object_dir_for_kind(revlink_sd_file_kind_t kind)
{
    switch (kind) {
        case REVLINK_SD_FILE_MAP:
            return storage_device.map_object_dir;
        case REVLINK_SD_FILE_STARTUP_SCREEN:
            return storage_device.image_object_dir;
        case REVLINK_SD_FILE_SCREENSHOT:
            return storage_device.screenshot_object_dir;
        default:
            return storage_device.object_dir;
    }
}

void revlink_sd_download_abort(void *context)
{
    (void)context;
    if (download_writer.stream != NULL) {
        fclose(download_writer.stream);
        download_writer.stream = NULL;
    }
    if (download_writer.hash_active) {
        psa_hash_abort(&download_writer.hash);
        download_writer.hash_active = false;
    }
    if (download_writer.temporary_path[0] != '\0') {
        unlink(download_writer.temporary_path);
    }
    memset(&download_writer, 0, sizeof(download_writer));
}

esp_err_t revlink_sd_download_begin(
    void *context,
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t expected_size
)
{
    (void)context;
    revlink_sd_file_kind_t kind = REVLINK_SD_FILE_DATALOG;
    if (!storage_device.selected || !sync_manifest_ready
        || !sync_history_ready || download_writer.stream != NULL
        || !classify_download_path(
               name,
               name_length,
               path,
               path_length,
               &kind
           )
        || path_length >= sizeof(download_writer.device_path)
        || expected_size == 0U
        || expected_size > REVLINK_SD_DOWNLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    char safe_name[REVLINK_SD_DOWNLOAD_NAME_CAPACITY];
    memcpy(safe_name, name, name_length);
    safe_name[name_length] = '\0';
    const int temporary_count = snprintf(
        download_writer.temporary_path,
        sizeof(download_writer.temporary_path),
        "%s/%s.part",
        storage_device.temporary_dir,
        safe_name
    );
    if (temporary_count < 0
        || (size_t)temporary_count >= sizeof(download_writer.temporary_path)) {
        revlink_sd_download_abort(NULL);
        return ESP_ERR_INVALID_SIZE;
    }

    unlink(download_writer.temporary_path);

    download_writer.stream = fopen(download_writer.temporary_path, "wb");
    if (download_writer.stream == NULL) {
        ESP_LOGE(TAG, "Unable to create temporary cache file: errno=%d", errno);
        revlink_sd_download_abort(NULL);
        return ESP_FAIL;
    }
    download_writer.hash =
        (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(
            &download_writer.hash,
            PSA_ALG_SHA_256
        ) != PSA_SUCCESS) {
        revlink_sd_download_abort(NULL);
        return ESP_FAIL;
    }
    download_writer.hash_active = true;
    download_writer.kind = kind;
    download_writer.expected_size = expected_size;
    download_writer.device_time_raw = device_time_raw;
    memcpy(download_writer.device_path, path, path_length);
    download_writer.device_path[path_length] = '\0';
    ESP_LOGI(
        TAG,
        "Atomic %s cache started: name=%s expected_bytes=%" PRIu32,
        kind == REVLINK_SD_FILE_MAP ? "map" : "datalog",
        safe_name,
        expected_size
    );
    return ESP_OK;
}

bool revlink_sd_download_write(
    void *context,
    const uint8_t *data,
    size_t length
)
{
    (void)context;
    if (download_writer.stream == NULL || data == NULL
        || length > download_writer.expected_size
            - download_writer.written_size) {
        return false;
    }
    if (length == 0U) {
        return true;
    }
    if (fwrite(data, 1, length, download_writer.stream) != length
        || psa_hash_update(
               &download_writer.hash,
               data,
               length
           ) != PSA_SUCCESS) {
        ESP_LOGE(
            TAG,
            "File cache write failed at offset=%" PRIu32,
            download_writer.written_size
        );
        return false;
    }
    download_writer.written_size += (uint32_t)length;
    return true;
}

static esp_err_t hash_file(
    const char *path,
    uint8_t digest[32],
    uint32_t *size
)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return ESP_FAIL;
    }
    uint8_t *buffer = malloc(REVLINK_SD_CHUNK_BYTES);
    if (buffer == NULL) {
        fclose(stream);
        return ESP_ERR_NO_MEM;
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(buffer);
        fclose(stream);
        return ESP_FAIL;
    }

    esp_err_t status = ESP_OK;
    uint32_t total = 0U;
    while (true) {
        const size_t count = fread(buffer, 1, REVLINK_SD_CHUNK_BYTES, stream);
        if (count > 0U) {
            if (count > UINT32_MAX - total
                || psa_hash_update(&hash, buffer, count) != PSA_SUCCESS) {
                status = ESP_FAIL;
                break;
            }
            total += (uint32_t)count;
        }
        if (count < REVLINK_SD_CHUNK_BYTES) {
            if (ferror(stream)) {
                status = ESP_FAIL;
            }
            break;
        }
    }
    fclose(stream);
    free(buffer);

    size_t digest_length = 0U;
    if (status == ESP_OK) {
        const psa_status_t hash_status = psa_hash_finish(
            &hash,
            digest,
            32U,
            &digest_length
        );
        if (hash_status != PSA_SUCCESS || digest_length != 32U) {
            status = ESP_FAIL;
        }
    } else {
        psa_hash_abort(&hash);
    }
    if (status == ESP_OK) {
        *size = total;
    }
    return status;
}

static esp_err_t load_sync_manifest(void)
{
    if (!storage_device.selected || sync_manifest == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    revlink_sync_manifest_init(sync_manifest);
    sync_manifest_ready = false;

    struct stat info;
    if (stat(storage_device.manifest_path, &info) != 0) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "Unable to inspect sync manifest: errno=%d", errno);
            return ESP_FAIL;
        }
        struct stat backup;
        if (stat(storage_device.manifest_backup_path, &backup) == 0) {
            if (rename(
                    storage_device.manifest_backup_path,
                    storage_device.manifest_path
                ) != 0
                || stat(storage_device.manifest_path, &info) != 0) {
                ESP_LOGE(
                    TAG,
                    "Unable to recover sync manifest backup: errno=%d",
                    errno
                );
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "Recovered interrupted sync manifest update");
        } else {
            unlink(storage_device.manifest_temp_path);
            sync_manifest_ready = true;
            ESP_LOGI(TAG, "Sync manifest initialized empty");
            return ESP_OK;
        }
    }
    if (info.st_size <= 0
        || (uint64_t)info.st_size > REVLINK_SD_MANIFEST_MAX_BYTES) {
        ESP_LOGE(
            TAG,
            "Sync manifest has invalid size: bytes=%" PRIu64,
            (uint64_t)info.st_size
        );
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *stream = fopen(storage_device.manifest_path, "rb");
    char *contents = malloc((size_t)info.st_size);
    if (stream == NULL || contents == NULL) {
        if (stream != NULL) {
            fclose(stream);
        }
        free(contents);
        return ESP_ERR_NO_MEM;
    }
    const size_t count =
        fread(contents, 1, (size_t)info.st_size, stream);
    bool read_failed =
        count != (size_t)info.st_size || ferror(stream);
    if (fclose(stream) != 0) {
        read_failed = true;
    }
    if (read_failed) {
        free(contents);
        ESP_LOGE(TAG, "Unable to read complete sync manifest");
        return ESP_FAIL;
    }

    const revlink_sync_status_t parse_status =
        revlink_sync_manifest_parse(contents, count, sync_manifest);
    free(contents);
    if (parse_status != REVLINK_SYNC_OK) {
        ESP_LOGE(
            TAG,
            "Refusing corrupt sync manifest: %s",
            revlink_sync_status_name(parse_status)
        );
        return ESP_ERR_INVALID_RESPONSE;
    }

    unlink(storage_device.manifest_temp_path);
    unlink(storage_device.manifest_backup_path);
    sync_manifest_ready = true;
    ESP_LOGI(
        TAG,
        "Sync manifest loaded: entries=%u",
        (unsigned int)sync_manifest->count
    );
    return ESP_OK;
}

static esp_err_t save_sync_manifest(
    const revlink_sync_manifest_t *candidate
)
{
    if (candidate == NULL || !sync_manifest_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    char *serialized = malloc(REVLINK_SD_MANIFEST_MAX_BYTES);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t serialized_length = 0U;
    const revlink_sync_status_t serialize_status =
        revlink_sync_manifest_serialize(
            candidate,
            serialized,
            REVLINK_SD_MANIFEST_MAX_BYTES,
            &serialized_length
        );
    if (serialize_status != REVLINK_SYNC_OK) {
        free(serialized);
        ESP_LOGE(
            TAG,
            "Unable to serialize sync manifest: %s",
            revlink_sync_status_name(serialize_status)
        );
        return ESP_ERR_INVALID_SIZE;
    }

    unlink(storage_device.manifest_temp_path);
    FILE *stream = fopen(storage_device.manifest_temp_path, "wb");
    if (stream == NULL) {
        free(serialized);
        return ESP_FAIL;
    }
    bool failed =
        fwrite(serialized, 1, serialized_length, stream) != serialized_length
        || fflush(stream) != 0 || fsync(fileno(stream)) != 0;
    if (fclose(stream) != 0) {
        failed = true;
    }
    free(serialized);
    if (failed) {
        ESP_LOGE(TAG, "Atomic sync manifest update failed: errno=%d", errno);
        unlink(storage_device.manifest_temp_path);
        return ESP_FAIL;
    }

    struct stat current;
    const bool had_current =
        stat(storage_device.manifest_path, &current) == 0;
    unlink(storage_device.manifest_backup_path);
    if (had_current
        && rename(
               storage_device.manifest_path,
               storage_device.manifest_backup_path
           ) != 0) {
        ESP_LOGE(TAG, "Unable to stage existing manifest: errno=%d", errno);
        unlink(storage_device.manifest_temp_path);
        return ESP_FAIL;
    }
    if (rename(
            storage_device.manifest_temp_path,
            storage_device.manifest_path
        ) != 0) {
        const int publish_errno = errno;
        if (had_current) {
            rename(
                storage_device.manifest_backup_path,
                storage_device.manifest_path
            );
        }
        unlink(storage_device.manifest_temp_path);
        ESP_LOGE(
            TAG,
            "Unable to publish new sync manifest: errno=%d",
            publish_errno
        );
        return ESP_FAIL;
    }
    unlink(storage_device.manifest_backup_path);
    return ESP_OK;
}

/*
 * True when this exact path was reported by the listing pass in progress.
 * Linear over a packed string pool: at most a few hundred short paths against
 * at most 128 manifest entries, once per sync.
 */
static bool device_scan_saw(const char *path)
{
    if (device_scan_pool == NULL || path == NULL) {
        return false;
    }
    size_t offset = 0U;
    while (offset < device_scan_used) {
        const char *candidate = device_scan_pool + offset;
        const size_t length = strlen(candidate);
        if (strcmp(candidate, path) == 0) {
            return true;
        }
        offset += length + 1U;
    }
    return false;
}

static void device_scan_release(void)
{
    free(device_scan_pool);
    device_scan_pool = NULL;
    device_scan_used = 0U;
    device_scan_active = false;
    device_scan_overflowed = false;
}

void revlink_sd_device_scan_begin(void *context)
{
    (void)context;
    device_scan_release();
    device_scan_pool = malloc(REVLINK_SD_DEVICE_SCAN_POOL_BYTES);
    if (device_scan_pool == NULL) {
        /*
         * Without a buffer there is no evidence to gather, so the pass never
         * opens and end() will leave every presence flag as it found it.
         */
        ESP_LOGW(TAG, "Device listing pass skipped: no memory for the path pool");
        return;
    }
    device_scan_active = true;
}

void revlink_sd_device_scan_observe(
    void *context,
    const uint8_t *path,
    size_t path_length
)
{
    (void)context;
    if (!device_scan_active || path == NULL || path_length == 0U) {
        return;
    }
    if (path_length >= REVLINK_SYNC_PATH_CAPACITY
        || path_length + 1U
            > REVLINK_SD_DEVICE_SCAN_POOL_BYTES - device_scan_used) {
        device_scan_overflowed = true;
        return;
    }
    memcpy(device_scan_pool + device_scan_used, path, path_length);
    device_scan_pool[device_scan_used + path_length] = '\0';
    device_scan_used += path_length + 1U;
}

void revlink_sd_device_scan_end(void *context, bool complete)
{
    (void)context;
    if (!device_scan_active) {
        device_scan_release();
        return;
    }
    if (!complete || device_scan_overflowed || !sync_manifest_ready
        || sync_manifest == NULL) {
        ESP_LOGI(
            TAG,
            "Device listing pass discarded: complete=%s overflow=%s",
            complete ? "yes" : "no",
            device_scan_overflowed ? "yes" : "no"
        );
        device_scan_release();
        return;
    }

    size_t on_device = 0U;
    size_t absent = 0U;
    for (size_t index = 0U; index < sync_manifest->count; ++index) {
        revlink_sync_manifest_entry_t *entry = &sync_manifest->entries[index];
        if (device_scan_saw(entry->path)) {
            entry->presence = REVLINK_SYNC_PRESENCE_ON_DEVICE;
            ++on_device;
        } else {
            entry->presence = REVLINK_SYNC_PRESENCE_ABSENT;
            ++absent;
        }
    }
    const esp_err_t saved = save_sync_manifest(sync_manifest);
    ESP_LOGI(
        TAG,
        "Device listing pass applied: on_device=%u absent=%u save=%s",
        (unsigned int)on_device,
        (unsigned int)absent,
        esp_err_to_name(saved)
    );
    device_scan_release();
    publish_portal_snapshot();
}

esp_err_t revlink_sd_mark_absent(const char *path)
{
    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!sync_manifest_ready || sync_manifest == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t length = strlen(path);
    if (!revlink_sync_manifest_set_presence(
            sync_manifest,
            (const uint8_t *)path,
            length,
            REVLINK_SYNC_PRESENCE_ABSENT
        )) {
        /*
         * Nothing cached under that path. A delete of a file the Sidecar had
         * never synchronised is a perfectly ordinary thing to do.
         */
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t status = save_sync_manifest(sync_manifest);
    publish_portal_snapshot();
    return status;
}

static esp_err_t load_sync_history(void)
{
    if (!storage_device.selected || sync_history == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    revlink_sync_history_init(sync_history);
    sync_history_ready = false;

    struct stat info;
    if (stat(storage_device.history_path, &info) != 0) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "Unable to inspect version history: errno=%d", errno);
            return ESP_FAIL;
        }
        struct stat backup;
        if (stat(storage_device.history_backup_path, &backup) == 0) {
            if (rename(
                    storage_device.history_backup_path,
                    storage_device.history_path
                ) != 0
                || stat(storage_device.history_path, &info) != 0) {
                ESP_LOGE(
                    TAG,
                    "Unable to recover version history backup: errno=%d",
                    errno
                );
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "Recovered interrupted version history update");
        } else {
            unlink(storage_device.history_temp_path);
            sync_history_ready = true;
            ESP_LOGI(TAG, "Version history initialized empty");
            return ESP_OK;
        }
    }
    if (info.st_size <= 0
        || (uint64_t)info.st_size > REVLINK_SD_HISTORY_MAX_BYTES) {
        ESP_LOGE(
            TAG,
            "Version history has invalid size: bytes=%" PRIu64,
            (uint64_t)info.st_size
        );
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *stream = fopen(storage_device.history_path, "rb");
    char *contents = malloc((size_t)info.st_size);
    if (stream == NULL || contents == NULL) {
        if (stream != NULL) {
            fclose(stream);
        }
        free(contents);
        return ESP_ERR_NO_MEM;
    }
    const size_t count = fread(contents, 1, (size_t)info.st_size, stream);
    bool failed = count != (size_t)info.st_size || ferror(stream);
    if (fclose(stream) != 0) {
        failed = true;
    }
    if (failed) {
        free(contents);
        return ESP_FAIL;
    }
    const revlink_sync_status_t parse_status =
        revlink_sync_history_parse(contents, count, sync_history);
    free(contents);
    if (parse_status != REVLINK_SYNC_OK) {
        ESP_LOGE(
            TAG,
            "Refusing corrupt version history: %s",
            revlink_sync_status_name(parse_status)
        );
        return ESP_ERR_INVALID_RESPONSE;
    }
    unlink(storage_device.history_temp_path);
    unlink(storage_device.history_backup_path);
    sync_history_ready = true;
    ESP_LOGI(
        TAG,
        "Version history loaded: versions=%u next=%" PRIu32,
        (unsigned int)sync_history->count,
        sync_history->next_sequence
    );
    return ESP_OK;
}

static esp_err_t save_sync_history(const revlink_sync_history_t *candidate)
{
    if (candidate == NULL || !sync_history_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    char *serialized = malloc(REVLINK_SD_HISTORY_MAX_BYTES);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t serialized_length = 0U;
    const revlink_sync_status_t serialize_status =
        revlink_sync_history_serialize(
            candidate,
            serialized,
            REVLINK_SD_HISTORY_MAX_BYTES,
            &serialized_length
        );
    if (serialize_status != REVLINK_SYNC_OK) {
        free(serialized);
        return ESP_ERR_INVALID_SIZE;
    }

    unlink(storage_device.history_temp_path);
    FILE *stream = fopen(storage_device.history_temp_path, "wb");
    if (stream == NULL) {
        free(serialized);
        return ESP_FAIL;
    }
    bool failed =
        fwrite(serialized, 1, serialized_length, stream) != serialized_length
        || fflush(stream) != 0 || fsync(fileno(stream)) != 0;
    if (fclose(stream) != 0) {
        failed = true;
    }
    free(serialized);
    if (failed) {
        unlink(storage_device.history_temp_path);
        return ESP_FAIL;
    }

    struct stat current;
    const bool had_current =
        stat(storage_device.history_path, &current) == 0;
    unlink(storage_device.history_backup_path);
    if (had_current
        && rename(
               storage_device.history_path,
               storage_device.history_backup_path
           ) != 0) {
        unlink(storage_device.history_temp_path);
        return ESP_FAIL;
    }
    if (rename(
            storage_device.history_temp_path,
            storage_device.history_path
        ) != 0) {
        if (had_current) {
            rename(
                storage_device.history_backup_path,
                storage_device.history_path
            );
        }
        unlink(storage_device.history_temp_path);
        return ESP_FAIL;
    }
    unlink(storage_device.history_backup_path);
    return ESP_OK;
}

static esp_err_t derive_device_key(
    const char *serial,
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U]
)
{
    static const char domain[] = "revlink-device-v1:";
    if (serial == NULL || serial[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    uint8_t digest[32];
    size_t digest_length = 0U;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS
        || psa_hash_update(
               &hash,
               (const uint8_t *)domain,
               sizeof(domain) - 1U
           ) != PSA_SUCCESS
        || psa_hash_update(
               &hash,
               (const uint8_t *)serial,
               strlen(serial)
           ) != PSA_SUCCESS
        || psa_hash_finish(
               &hash,
               digest,
               sizeof(digest),
               &digest_length
           ) != PSA_SUCCESS
        || digest_length != sizeof(digest)) {
        psa_hash_abort(&hash);
        return ESP_FAIL;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U;
         index < REVLINK_SD_DEVICE_KEY_HEX_BYTES / 2U;
         ++index) {
        key[index * 2U] = hex[digest[index] >> 4U];
        key[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    key[REVLINK_SD_DEVICE_KEY_HEX_BYTES] = '\0';
    return ESP_OK;
}

static esp_err_t write_identity_file(
    const revlink_ap_device_info_t *identity
)
{
    char temporary[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (format_path(
            temporary,
            sizeof(temporary),
            "%s.tmp",
            storage_device.identity_path
        ) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    unlink(temporary);
    FILE *stream = fopen(temporary, "wb");
    if (stream == NULL) {
        return ESP_FAIL;
    }
    const int count = fprintf(
        stream,
        "REVLINK-DEVICE\t1\n"
        "key=%s\n"
        "serial=%s\n"
        "part_number=%s\n"
        "firmware=%s\n"
        "install_status=%s\n"
        "vehicle=%s\n",
        storage_device.key,
        identity->serial,
        identity->part_number,
        identity->firmware,
        identity->install_status,
        identity->vehicle
    );
    bool failed = count <= 0 || fflush(stream) != 0
        || fsync(fileno(stream)) != 0;
    if (fclose(stream) != 0) {
        failed = true;
    }
    if (failed || rename(temporary, storage_device.identity_path) != 0) {
        unlink(temporary);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool refresh_identity_field(
    char *stored,
    size_t capacity,
    const char *authoritative
)
{
    if (
        stored == NULL || capacity == 0U || authoritative == NULL
        || authoritative[0] == '\0'
        || strcmp(stored, authoritative) == 0
    ) {
        return false;
    }
    const size_t length = strnlen(authoritative, capacity);
    if (length >= capacity) {
        return false;
    }
    memcpy(stored, authoritative, length + 1U);
    return true;
}

static esp_err_t ensure_identity_file(
    const revlink_ap_device_info_t *identity
)
{
    struct stat info;
    if (stat(storage_device.identity_path, &info) != 0) {
        return errno == ENOENT ? write_identity_file(identity) : ESP_FAIL;
    }
    if (info.st_size <= 0 || info.st_size > 1024) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *stream = fopen(storage_device.identity_path, "rb");
    char contents[1025] = {0};
    if (stream == NULL) {
        return ESP_FAIL;
    }
    const size_t count = fread(contents, 1U, (size_t)info.st_size, stream);
    bool failed =
        count != (size_t)info.st_size || ferror(stream);
    if (fclose(stream) != 0) {
        failed = true;
    }
    if (failed) {
        return ESP_FAIL;
    }
    contents[count] = '\0';

    char stored_key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    revlink_ap_device_info_t stored = {0};
    if (
        !copy_identity_value(
            contents, "key", stored_key, sizeof(stored_key)
        )
        || strcmp(stored_key, storage_device.key) != 0
        || !copy_identity_value(
            contents, "serial", stored.serial, sizeof(stored.serial)
        )
        || strcmp(stored.serial, identity->serial) != 0
    ) {
        ESP_LOGE(TAG, "Device namespace identity mismatch; refusing cache");
        return ESP_ERR_INVALID_STATE;
    }

    const bool has_part_number = copy_identity_value(
        contents,
        "part_number",
        stored.part_number,
        sizeof(stored.part_number)
    );
    const bool has_firmware = copy_identity_value(
        contents,
        "firmware",
        stored.firmware,
        sizeof(stored.firmware)
    );
    const bool has_install_status = copy_identity_value(
        contents,
        "install_status",
        stored.install_status,
        sizeof(stored.install_status)
    );
    const bool has_vehicle = copy_identity_value(
        contents,
        "vehicle",
        stored.vehicle,
        sizeof(stored.vehicle)
    );
    bool rewrite = !has_part_number || !has_firmware
        || !has_install_status || !has_vehicle;
    rewrite |= refresh_identity_field(
        stored.part_number,
        sizeof(stored.part_number),
        identity->part_number
    );
    rewrite |= refresh_identity_field(
        stored.firmware,
        sizeof(stored.firmware),
        identity->firmware
    );
    rewrite |= refresh_identity_field(
        stored.install_status,
        sizeof(stored.install_status),
        identity->install_status
    );
    rewrite |= refresh_identity_field(
        stored.vehicle,
        sizeof(stored.vehicle),
        identity->vehicle
    );
    if (!rewrite) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Refreshing cached identity metadata for %s",
        storage_device.key
    );
    return write_identity_file(&stored);
}

static esp_err_t migrate_legacy_ledger_file(
    const char *destination,
    const char *legacy
)
{
    struct stat info;
    if (stat(destination, &info) == 0) {
        return ESP_OK;
    }
    if (errno != ENOENT) {
        return ESP_FAIL;
    }
    if (stat(legacy, &info) != 0) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (rename(legacy, destination) != 0) {
        ESP_LOGE(
            TAG,
            "Unable to migrate legacy content ledger: errno=%d",
            errno
        );
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Migrated legacy datalog ledger into shared file inventory");
    return ESP_OK;
}

static bool valid_device_key(const char *key)
{
    if (key == NULL || strlen(key) != REVLINK_SD_DEVICE_KEY_HEX_BYTES) {
        return false;
    }
    for (size_t index = 0U;
         index < REVLINK_SD_DEVICE_KEY_HEX_BYTES;
         ++index) {
        const char value = key[index];
        if (!((value >= '0' && value <= '9')
              || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool copy_identity_value(
    const char *contents,
    const char *name,
    char *target,
    size_t target_capacity
)
{
    if (
        contents == NULL || name == NULL || target == NULL
        || target_capacity == 0U
    ) {
        return false;
    }
    char prefix[40];
    const int prefix_length = snprintf(prefix, sizeof(prefix), "%s=", name);
    if (
        prefix_length <= 0
        || (size_t)prefix_length >= sizeof(prefix)
    ) {
        return false;
    }

    const char *cursor = contents;
    while ((cursor = strstr(cursor, prefix)) != NULL) {
        if (cursor == contents || cursor[-1] == '\n') {
            const char *value = cursor + (size_t)prefix_length;
            const char *end = strchr(value, '\n');
            if (end == NULL) {
                end = value + strlen(value);
            }
            const size_t length = (size_t)(end - value);
            if (length >= target_capacity) {
                return false;
            }
            memcpy(target, value, length);
            target[length] = '\0';
            return true;
        }
        cursor += (size_t)prefix_length;
    }
    return false;
}

static esp_err_t load_identity_for_key(
    const char *key,
    revlink_ap_device_info_t *identity
)
{
    if (!valid_device_key(key) || identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (format_path(
            path,
            sizeof(path),
            REVLINK_SD_MOUNT_POINT "/revlink/devices/%s/identity.txt",
            key
        ) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat info;
    if (stat(path, &info) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (info.st_size <= 0 || info.st_size > 1024) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *stream = fopen(path, "rb");
    char contents[1025] = {0};
    if (stream == NULL) {
        return ESP_FAIL;
    }
    const size_t count = fread(contents, 1U, (size_t)info.st_size, stream);
    bool failed =
        count != (size_t)info.st_size || ferror(stream);
    if (fclose(stream) != 0) {
        failed = true;
    }
    if (failed) {
        return ESP_FAIL;
    }
    contents[count] = '\0';
    if (strncmp(contents, "REVLINK-DEVICE\t1\n", 17U) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char stored_key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    revlink_ap_device_info_t parsed = {0};
    if (
        !copy_identity_value(
            contents, "key", stored_key, sizeof(stored_key)
        )
        || strcmp(stored_key, key) != 0
        || !copy_identity_value(
            contents,
            "serial",
            parsed.serial,
            sizeof(parsed.serial)
        )
        || parsed.serial[0] == '\0'
        || !copy_identity_value(
            contents,
            "part_number",
            parsed.part_number,
            sizeof(parsed.part_number)
        )
        || !copy_identity_value(
            contents,
            "firmware",
            parsed.firmware,
            sizeof(parsed.firmware)
        )
        || !copy_identity_value(
            contents,
            "install_status",
            parsed.install_status,
            sizeof(parsed.install_status)
        )
        || !copy_identity_value(
            contents,
            "vehicle",
            parsed.vehicle,
            sizeof(parsed.vehicle)
        )
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    char derived_key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    if (
        derive_device_key(parsed.serial, derived_key) != ESP_OK
        || strcmp(derived_key, key) != 0
    ) {
        ESP_LOGE(TAG, "Cached identity does not match its device namespace");
        return ESP_ERR_INVALID_STATE;
    }
    *identity = parsed;
    return ESP_OK;
}

static esp_err_t read_last_device_key(
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U]
)
{
    struct stat info;
    if (stat(REVLINK_SD_LAST_DEVICE_PATH, &info) != 0) {
        if (errno != ENOENT) {
            return ESP_FAIL;
        }
        struct stat backup;
        if (stat(REVLINK_SD_LAST_DEVICE_BACKUP_PATH, &backup) != 0) {
            return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
        if (rename(
                REVLINK_SD_LAST_DEVICE_BACKUP_PATH,
                REVLINK_SD_LAST_DEVICE_PATH
            ) != 0
            || stat(REVLINK_SD_LAST_DEVICE_PATH, &info) != 0) {
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "Recovered interrupted last-device marker update");
    }
    if (info.st_size <= 0 || info.st_size > 128) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *stream = fopen(REVLINK_SD_LAST_DEVICE_PATH, "rb");
    char contents[129] = {0};
    if (stream == NULL) {
        return ESP_FAIL;
    }
    const size_t count = fread(contents, 1U, (size_t)info.st_size, stream);
    bool failed =
        count != (size_t)info.st_size || ferror(stream);
    if (fclose(stream) != 0) {
        failed = true;
    }
    if (failed) {
        return ESP_FAIL;
    }
    contents[count] = '\0';
    if (
        strncmp(contents, "REVLINK-LAST-DEVICE\t1\n", 22U) != 0
        || !copy_identity_value(
            contents,
            "key",
            key,
            REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U
        )
        || !valid_device_key(key)
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t write_last_device_key(const char *key)
{
    if (!valid_device_key(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    unlink(REVLINK_SD_LAST_DEVICE_TEMP_PATH);
    FILE *stream = fopen(REVLINK_SD_LAST_DEVICE_TEMP_PATH, "wb");
    if (stream == NULL) {
        return ESP_FAIL;
    }
    const int count = fprintf(
        stream,
        "REVLINK-LAST-DEVICE\t1\nkey=%s\n",
        key
    );
    bool failed =
        count <= 0 || fflush(stream) != 0 || fsync(fileno(stream)) != 0;
    if (fclose(stream) != 0) {
        failed = true;
    }
    if (failed) {
        unlink(REVLINK_SD_LAST_DEVICE_TEMP_PATH);
        return ESP_FAIL;
    }

    struct stat current;
    const bool had_current =
        stat(REVLINK_SD_LAST_DEVICE_PATH, &current) == 0;
    unlink(REVLINK_SD_LAST_DEVICE_BACKUP_PATH);
    if (
        had_current
        && rename(
               REVLINK_SD_LAST_DEVICE_PATH,
               REVLINK_SD_LAST_DEVICE_BACKUP_PATH
           ) != 0
    ) {
        unlink(REVLINK_SD_LAST_DEVICE_TEMP_PATH);
        return ESP_FAIL;
    }
    if (rename(
            REVLINK_SD_LAST_DEVICE_TEMP_PATH,
            REVLINK_SD_LAST_DEVICE_PATH
        ) != 0) {
        if (had_current) {
            rename(
                REVLINK_SD_LAST_DEVICE_BACKUP_PATH,
                REVLINK_SD_LAST_DEVICE_PATH
            );
        }
        unlink(REVLINK_SD_LAST_DEVICE_TEMP_PATH);
        return ESP_FAIL;
    }
    unlink(REVLINK_SD_LAST_DEVICE_BACKUP_PATH);
    return ESP_OK;
}

static esp_err_t discover_single_device_key(
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U]
)
{
    const char *root = REVLINK_SD_MOUNT_POINT "/revlink/devices";
    DIR *directory = opendir(root);
    if (directory == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    size_t matches = 0U;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!valid_device_key(entry->d_name)) {
            continue;
        }
        char identity_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
        struct stat info;
        if (
            format_path(
                identity_path,
                sizeof(identity_path),
                "%s/%s/identity.txt",
                root,
                entry->d_name
            ) != ESP_OK
            || stat(identity_path, &info) != 0
            || !S_ISREG(info.st_mode)
        ) {
            continue;
        }
        ++matches;
        if (matches == 1U) {
            memcpy(
                key,
                entry->d_name,
                REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U
            );
        }
    }
    closedir(directory);
    if (matches == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    if (matches > 1U) {
        ESP_LOGW(
            TAG,
            "Multiple cached device namespaces found without a last-device "
            "pointer; waiting for authoritative AccessPort identity"
        );
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t revlink_sd_select_device(
    void *context,
    const revlink_ap_device_info_t *identity
)
{
    (void)context;
    if (identity == NULL || identity->serial[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    revlink_sd_download_abort(NULL);
    sync_manifest_ready = false;
    sync_history_ready = false;
    memset(&storage_device, 0, sizeof(storage_device));
    storage_device.identity = *identity;
    if (derive_device_key(identity->serial, storage_device.key) != ESP_OK
        || format_path(
               storage_device.base,
               sizeof(storage_device.base),
               REVLINK_SD_MOUNT_POINT "/revlink/devices/%s",
               storage_device.key
           ) != ESP_OK
        || format_path(
               storage_device.current_dir,
               sizeof(storage_device.current_dir),
               "%s/current",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.history_dir,
               sizeof(storage_device.history_dir),
               "%s/history",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.object_dir,
               sizeof(storage_device.object_dir),
               "%s/objects/datalogs",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.map_object_dir,
               sizeof(storage_device.map_object_dir),
               "%s/objects/maps",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.image_object_dir,
               sizeof(storage_device.image_object_dir),
               "%s/objects/images",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.screenshot_object_dir,
               sizeof(storage_device.screenshot_object_dir),
               "%s/objects/screenshots",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.startup_profile_dir,
               sizeof(storage_device.startup_profile_dir),
               "%s/startup-library",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.temporary_dir,
               sizeof(storage_device.temporary_dir),
               "%s/tmp",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.identity_path,
               sizeof(storage_device.identity_path),
               "%s/identity.txt",
               storage_device.base
           ) != ESP_OK
        || format_path(
               storage_device.manifest_path,
               sizeof(storage_device.manifest_path),
               "%s/inventory.manifest",
               storage_device.current_dir
           ) != ESP_OK
        || format_path(
               storage_device.history_path,
               sizeof(storage_device.history_path),
               "%s/inventory.versions",
               storage_device.history_dir
           ) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (format_path(
            storage_device.manifest_temp_path,
            sizeof(storage_device.manifest_temp_path),
            "%s.tmp",
            storage_device.manifest_path
        ) != ESP_OK
        || format_path(
               storage_device.manifest_backup_path,
               sizeof(storage_device.manifest_backup_path),
               "%s.bak",
               storage_device.manifest_path
           ) != ESP_OK
        || format_path(
               storage_device.history_temp_path,
               sizeof(storage_device.history_temp_path),
               "%s.tmp",
               storage_device.history_path
           ) != ESP_OK
        || format_path(
               storage_device.history_backup_path,
               sizeof(storage_device.history_backup_path),
               "%s.bak",
               storage_device.history_path
           ) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *paths[] = {
        storage_device.base,
        storage_device.current_dir,
        storage_device.history_dir,
    };
    for (size_t index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        if (make_directory(paths[index]) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    char object_parent[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (format_path(
            object_parent,
            sizeof(object_parent),
            "%s/objects",
            storage_device.base
        ) != ESP_OK
        || make_directory(object_parent) != ESP_OK
        || make_directory(storage_device.object_dir) != ESP_OK
        || make_directory(storage_device.map_object_dir) != ESP_OK
        || make_directory(storage_device.image_object_dir) != ESP_OK
        || make_directory(storage_device.screenshot_object_dir) != ESP_OK
        || make_directory(storage_device.startup_profile_dir) != ESP_OK
        || make_directory(storage_device.temporary_dir) != ESP_OK) {
        return ESP_FAIL;
    }
    storage_device.selected = true;
    if (ensure_identity_file(identity) != ESP_OK) {
        storage_device.selected = false;
        return ESP_ERR_INVALID_STATE;
    }
    char legacy_manifest[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    char legacy_history[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    if (format_path(
            legacy_manifest,
            sizeof(legacy_manifest),
            "%s/datalogs.manifest",
            storage_device.current_dir
        ) != ESP_OK
        || format_path(
               legacy_history,
               sizeof(legacy_history),
               "%s/datalogs.versions",
               storage_device.history_dir
           ) != ESP_OK
        || migrate_legacy_ledger_file(
               storage_device.manifest_path,
               legacy_manifest
           ) != ESP_OK
        || migrate_legacy_ledger_file(
               storage_device.history_path,
               legacy_history
           ) != ESP_OK) {
        storage_device.selected = false;
        return ESP_FAIL;
    }
    if (sync_manifest == NULL) {
        sync_manifest = malloc(sizeof(*sync_manifest));
    }
    if (sync_history == NULL) {
        sync_history = malloc(sizeof(*sync_history));
    }
    if (sync_manifest == NULL || sync_history == NULL) {
        storage_device.selected = false;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t status = load_sync_history();
    if (status == ESP_OK) {
        status = load_sync_manifest();
    }
    if (status != ESP_OK) {
        storage_device.selected = false;
        sync_manifest_ready = false;
        sync_history_ready = false;
        return status;
    }
    ESP_LOGI(
        TAG,
        "Per-device namespace selected: key=%s serial=%s current=%u versions=%u",
        storage_device.key,
        identity->serial,
        (unsigned int)sync_manifest->count,
        (unsigned int)sync_history->count
    );
    if (identity_observer != NULL) {
        identity_observer(identity_observer_context, identity);
    }
    const esp_err_t pointer_status =
        write_last_device_key(storage_device.key);
    if (pointer_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to persist last device namespace: %s",
            esp_err_to_name(pointer_status)
        );
    }
    publish_portal_snapshot();
    return ESP_OK;
}

void revlink_sd_release_device(void *context)
{
    (void)context;
    revlink_sd_download_abort(NULL);
    revlink_sd_startup_profile_abort();
    sync_manifest_ready = false;
    sync_history_ready = false;
    if (sync_manifest != NULL) {
        revlink_sync_manifest_init(sync_manifest);
    }
    if (sync_history != NULL) {
        revlink_sync_history_init(sync_history);
    }
    if (storage_device.selected) {
        ESP_LOGI(
            TAG,
            "Per-device namespace released: key=%s",
            storage_device.key
        );
    }
    if (
        portal_snapshot_mutex != NULL
        && xSemaphoreTake(portal_snapshot_mutex, portMAX_DELAY) == pdTRUE
    ) {
        portal_snapshot.session_selected = false;
        xSemaphoreGive(portal_snapshot_mutex);
    }
    memset(&storage_device, 0, sizeof(storage_device));
}

static esp_err_t restore_portal_namespace(void)
{
    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    esp_err_t status = read_last_device_key(key);
    if (status != ESP_OK) {
        memset(key, 0, sizeof(key));
        status = discover_single_device_key(key);
        if (status != ESP_OK) {
            return status;
        }
        ESP_LOGI(
            TAG,
            "Migrating the sole legacy device namespace into boot "
            "rehydration metadata"
        );
    }

    revlink_ap_device_info_t identity = {0};
    status = load_identity_for_key(key, &identity);
    if (status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Last device namespace identity is unavailable: %s",
            esp_err_to_name(status)
        );
        return status;
    }
    status = revlink_sd_select_device(NULL, &identity);
    if (status != ESP_OK) {
        return status;
    }
    const size_t files =
        sync_manifest != NULL ? sync_manifest->count : 0U;
    revlink_sd_release_device(NULL);
    ESP_LOGI(
        TAG,
        "Restored cached AccessPort identity and %u files for the portal: "
        "part=%s firmware=%s vehicle=%s",
        (unsigned int)files,
        identity.part_number,
        identity.firmware,
        identity.vehicle
    );
    return ESP_OK;
}

esp_err_t revlink_sd_cached_devices_snapshot(
    revlink_sd_cached_devices_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (storage_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char selected_key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    if (read_last_device_key(selected_key) != ESP_OK) {
        selected_key[0] = '\0';
    }

    const char *root = REVLINK_SD_MOUNT_POINT "/revlink/devices";
    DIR *directory = opendir(root);
    if (directory == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    struct dirent *entry;
    while (
        snapshot->count < REVLINK_SD_CACHED_DEVICE_CAPACITY
        && (entry = readdir(directory)) != NULL
    ) {
        if (!valid_device_key(entry->d_name)) {
            continue;
        }
        revlink_ap_device_info_t identity = {0};
        if (load_identity_for_key(entry->d_name, &identity) != ESP_OK) {
            continue;
        }
        revlink_sd_cached_device_t *cached =
            &snapshot->devices[snapshot->count++];
        memcpy(cached->key, entry->d_name, sizeof(cached->key));
        cached->identity = identity;
        cached->selected =
            selected_key[0] != '\0'
            && strcmp(selected_key, entry->d_name) == 0;
    }
    closedir(directory);

    for (size_t index = 1U; index < snapshot->count; ++index) {
        revlink_sd_cached_device_t moving = snapshot->devices[index];
        size_t position = index;
        while (position > 0U) {
            const revlink_sd_cached_device_t *previous =
                &snapshot->devices[position - 1U];
            const int order =
                moving.selected != previous->selected
                    ? (moving.selected ? -1 : 1)
                    : strcmp(
                        moving.identity.part_number,
                        previous->identity.part_number
                    );
            if (order >= 0) {
                break;
            }
            snapshot->devices[position] = *previous;
            --position;
        }
        snapshot->devices[position] = moving;
    }
    return ESP_OK;
}

esp_err_t revlink_sd_select_cached_device(const char *key)
{
    if (!valid_device_key(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (storage_device.selected || download_writer.stream != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    revlink_ap_device_info_t identity = {0};
    esp_err_t status = load_identity_for_key(key, &identity);
    if (status != ESP_OK) {
        return status;
    }
    status = revlink_sd_select_device(NULL, &identity);
    if (status == ESP_OK) {
        revlink_sd_release_device(NULL);
    }
    return status;
}

esp_err_t revlink_local_metadata_backfill_initial_sync(
    uint64_t initial_sync_utc,
    size_t *manifest_updated,
    size_t *history_updated
)
{
    if (
        initial_sync_utc == 0U
        || manifest_updated == NULL
        || history_updated == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest_updated = 0U;
    *history_updated = 0U;
    if (
        storage_card == NULL
        || storage_device.selected
        || download_writer.stream != NULL
        || download_writer.hash_active
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    esp_err_t status = read_last_device_key(key);
    if (status == ESP_ERR_NOT_FOUND) {
        status = discover_single_device_key(key);
    }
    if (status != ESP_OK) {
        return status;
    }

    revlink_ap_device_info_t identity = {0};
    status = load_identity_for_key(key, &identity);
    if (status != ESP_OK) {
        return status;
    }
    status = revlink_sd_select_device(NULL, &identity);
    if (status != ESP_OK) {
        return status;
    }

    size_t changed_history = 0U;
    for (size_t index = 0U; index < sync_history->count; ++index) {
        if (sync_history->entries[index].initial_sync_utc == 0U) {
            sync_history->entries[index].initial_sync_utc = initial_sync_utc;
            ++changed_history;
        }
    }
    if (changed_history > 0U) {
        status = save_sync_history(sync_history);
        if (status != ESP_OK) {
            (void)load_sync_history();
            revlink_sd_release_device(NULL);
            return status;
        }
    }
    *history_updated = changed_history;

    size_t changed_manifest = 0U;
    for (size_t index = 0U; index < sync_manifest->count; ++index) {
        if (sync_manifest->entries[index].initial_sync_utc == 0U) {
            sync_manifest->entries[index].initial_sync_utc = initial_sync_utc;
            ++changed_manifest;
        }
    }
    if (changed_manifest > 0U) {
        status = save_sync_manifest(sync_manifest);
        if (status != ESP_OK) {
            (void)load_sync_manifest();
            revlink_sd_release_device(NULL);
            return status;
        }
    }
    *manifest_updated = changed_manifest;

    publish_portal_snapshot();
    ESP_LOGI(
        TAG,
        "Unknown Initial sync metadata backfilled: epoch=%" PRIu64
        " current=%u history=%u",
        initial_sync_utc,
        (unsigned int)changed_manifest,
        (unsigned int)changed_history
    );
    revlink_sd_release_device(NULL);
    return ESP_OK;
}

static bool datalog_number(const char *path, unsigned int *number)
{
    static const char prefix[] = "datalog/datalog";
    if (
        path == NULL
        || number == NULL
        || strncmp(path, prefix, sizeof(prefix) - 1U) != 0
    ) {
        return false;
    }
    const char *digits = path + sizeof(prefix) - 1U;
    char *suffix = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(digits, &suffix, 10);
    if (
        errno != 0
        || suffix == digits
        || parsed == 0U
        || parsed > 9999U
        || (
            strcmp(suffix, ".csv") != 0
            && strcmp(suffix, ".csv.gz") != 0
        )
    ) {
        return false;
    }
    *number = (unsigned int)parsed;
    return true;
}

static revlink_sync_manifest_entry_t *current_datalog_number(
    unsigned int number
)
{
    for (size_t index = 0U; index < sync_manifest->count; ++index) {
        unsigned int candidate = 0U;
        if (
            datalog_number(
                sync_manifest->entries[index].path,
                &candidate
            )
            && candidate == number
        ) {
            return &sync_manifest->entries[index];
        }
    }
    return NULL;
}

esp_err_t revlink_local_metadata_resequence_wrapped_datalogs(
    uint64_t first_initial_sync_utc,
    uint32_t increment_seconds,
    size_t *manifest_updated,
    size_t *history_updated
)
{
    if (
        first_initial_sync_utc == 0U
        || increment_seconds == 0U
        || manifest_updated == NULL
        || history_updated == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest_updated = 0U;
    *history_updated = 0U;
    if (
        storage_card == NULL
        || storage_device.selected
        || download_writer.stream != NULL
        || download_writer.hash_active
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    char key[REVLINK_SD_DEVICE_KEY_HEX_BYTES + 1U] = {0};
    esp_err_t status = read_last_device_key(key);
    if (status == ESP_ERR_NOT_FOUND) {
        status = discover_single_device_key(key);
    }
    if (status != ESP_OK) {
        return status;
    }

    revlink_ap_device_info_t identity = {0};
    status = load_identity_for_key(key, &identity);
    if (status != ESP_OK) {
        return status;
    }
    status = revlink_sd_select_device(NULL, &identity);
    if (status != ESP_OK) {
        return status;
    }

    uint64_t timestamp = first_initial_sync_utc;
    for (unsigned int phase = 0U; phase < 2U; ++phase) {
        const unsigned int first = phase == 0U ? 20U : 1U;
        const unsigned int last = phase == 0U ? 58U : 2U;
        for (unsigned int number = first; number <= last; ++number) {
            revlink_sync_manifest_entry_t *current =
                current_datalog_number(number);
            if (current == NULL) {
                continue;
            }
            current->initial_sync_utc = timestamp;
            ++*manifest_updated;
            for (size_t index = 0U; index < sync_history->count; ++index) {
                revlink_sync_history_entry_t *history =
                    &sync_history->entries[index];
                if (
                    strcmp(history->path, current->path) == 0
                    && memcmp(
                        history->sha256,
                        current->sha256,
                        REVLINK_SYNC_SHA256_BYTES
                    ) == 0
                ) {
                    history->initial_sync_utc = timestamp;
                    ++*history_updated;
                }
            }
            if (UINT64_MAX - timestamp < increment_seconds) {
                revlink_sd_release_device(NULL);
                return ESP_ERR_INVALID_ARG;
            }
            timestamp += increment_seconds;
        }
    }

    if (*history_updated > 0U) {
        status = save_sync_history(sync_history);
        if (status != ESP_OK) {
            (void)load_sync_history();
            (void)load_sync_manifest();
            revlink_sd_release_device(NULL);
            return status;
        }
    }
    if (*manifest_updated > 0U) {
        status = save_sync_manifest(sync_manifest);
        if (status != ESP_OK) {
            (void)load_sync_history();
            (void)load_sync_manifest();
            revlink_sd_release_device(NULL);
            return status;
        }
    }

    publish_portal_snapshot();
    ESP_LOGI(
        TAG,
        "Wrapped datalog chronology resequenced: start=%" PRIu64
        " step=%" PRIu32 " current=%u history=%u",
        first_initial_sync_utc,
        increment_seconds,
        (unsigned int)*manifest_updated,
        (unsigned int)*history_updated
    );
    revlink_sd_release_device(NULL);
    return ESP_OK;
}

bool revlink_sd_download_is_current(
    void *context,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t expected_size
)
{
    (void)context;
    if (!sync_manifest_ready || path == NULL
        || path_length >= REVLINK_SYNC_PATH_CAPACITY) {
        return false;
    }
    const revlink_sync_manifest_entry_t *entry =
        revlink_sync_manifest_find(sync_manifest, path, path_length);
    if (!revlink_sync_manifest_metadata_matches(
            entry,
            device_time_raw,
            expected_size
        )) {
        return false;
    }

    char path_copy[REVLINK_SYNC_PATH_CAPACITY];
    memcpy(path_copy, path, path_length);
    path_copy[path_length] = '\0';
    const revlink_sd_file_kind_t kind = kind_for_manifest_path(path_copy);
    char cache_path[REVLINK_SD_DOWNLOAD_PATH_CAPACITY];
    const int path_count = snprintf(
        cache_path,
        sizeof(cache_path),
        "%s/%s",
        object_dir_for_kind(kind),
        entry->cache_name
    );
    if (path_count < 0 || (size_t)path_count >= sizeof(cache_path)) {
        return false;
    }
    uint8_t digest[REVLINK_SYNC_SHA256_BYTES];
    uint32_t actual_size = 0U;
    if (hash_file(cache_path, digest, &actual_size) != ESP_OK
        || actual_size != entry->size
        || memcmp(digest, entry->sha256, sizeof(digest)) != 0) {
        ESP_LOGW(
            TAG,
            "Manifest cache verification failed; scheduling re-download: %.*s",
            (int)path_length,
            (const char *)path
        );
        return false;
    }
    if (kind == REVLINK_SD_FILE_STARTUP_SCREEN
        && preserve_synced_startup_profile(cache_path, entry->sha256)
            != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Current startup screen is cached but reusable profile preservation "
            "will be retried on the next sync"
        );
    }
    ESP_LOGI(
        TAG,
        "Incremental cache HIT: path=%.*s bytes=%" PRIu32,
        (int)path_length,
        (const char *)path,
        expected_size
    );
    return true;
}

esp_err_t revlink_sd_download_commit(void *context)
{
    (void)context;
    if (download_writer.stream == NULL
        || download_writer.written_size != download_writer.expected_size) {
        revlink_sd_download_abort(NULL);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t status = ESP_OK;
    if (fflush(download_writer.stream) != 0
        || fsync(fileno(download_writer.stream)) != 0
        || fclose(download_writer.stream) != 0) {
        ESP_LOGE(TAG, "File cache flush failed: errno=%d", errno);
        status = ESP_FAIL;
    }
    download_writer.stream = NULL;

    uint8_t digest[32];
    size_t digest_length = 0;
    if (status == ESP_OK) {
        const psa_status_t hash_status = psa_hash_finish(
            &download_writer.hash,
            digest,
            sizeof(digest),
            &digest_length
        );
        download_writer.hash_active = false;
        if (hash_status != PSA_SUCCESS || digest_length != sizeof(digest)) {
            status = ESP_FAIL;
        }
    }
    if (status != ESP_OK) {
        revlink_sd_download_abort(NULL);
        return status;
    }

    struct stat temporary;
    if (stat(download_writer.temporary_path, &temporary) != 0
        || temporary.st_size != download_writer.expected_size) {
        ESP_LOGE(TAG, "Temporary file cache verification failed: errno=%d", errno);
        revlink_sd_download_abort(NULL);
        return ESP_FAIL;
    }

    char digest_text[65];
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(&digest_text[index * 2], 3, "%02x", digest[index]);
    }
    digest_text[64] = '\0';

    const size_t device_path_length = strlen(download_writer.device_path);
    static const char gzip_extension[] = ".csv.gz";
    const char *extension =
        download_writer.kind == REVLINK_SD_FILE_MAP ? ".ptm"
        : download_writer.kind == REVLINK_SD_FILE_STARTUP_SCREEN ? ".fb"
        : download_writer.kind == REVLINK_SD_FILE_SCREENSHOT
            ? (device_path_length >= 4U
                && strcmp(
                    download_writer.device_path + device_path_length - 4U,
                    ".bmp"
                ) == 0 ? ".bmp" : ".png")
        : (device_path_length >= sizeof(gzip_extension) - 1U
            && memcmp(
                   download_writer.device_path + device_path_length
                       - (sizeof(gzip_extension) - 1U),
                   gzip_extension,
                   sizeof(gzip_extension) - 1U
               ) == 0
            ? gzip_extension
            : ".csv");
    char object_name[REVLINK_SYNC_CACHE_NAME_CAPACITY];
    if (format_path(
            object_name,
            sizeof(object_name),
            "%s%s",
            digest_text,
            extension
        ) != ESP_OK
        || format_path(
               download_writer.final_path,
               sizeof(download_writer.final_path),
               "%s/%s",
               object_dir_for_kind(download_writer.kind),
               object_name
           ) != ESP_OK) {
        revlink_sd_download_abort(NULL);
        return ESP_ERR_INVALID_SIZE;
    }

    bool deduplicated = false;
    struct stat existing;
    if (stat(download_writer.final_path, &existing) == 0) {
        uint8_t existing_digest[32];
        uint32_t existing_size = 0U;
        if (hash_file(
                download_writer.final_path,
                existing_digest,
                &existing_size
            ) != ESP_OK) {
            ESP_LOGE(TAG, "Unable to verify existing cache file");
            revlink_sd_download_abort(NULL);
            return ESP_FAIL;
        }
        if (existing_size == download_writer.written_size
            && memcmp(existing_digest, digest, sizeof(digest)) == 0) {
            unlink(download_writer.temporary_path);
            deduplicated = true;
        } else {
            ESP_LOGE(TAG, "Refusing SHA-256 object collision");
            revlink_sd_download_abort(NULL);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (!deduplicated
        && rename(
               download_writer.temporary_path,
               download_writer.final_path
           ) != 0) {
        ESP_LOGE(TAG, "Atomic file cache commit failed: errno=%d", errno);
        revlink_sd_download_abort(NULL);
        return ESP_FAIL;
    }

    revlink_sync_history_t *history_candidate =
        malloc(sizeof(*history_candidate));
    revlink_sync_manifest_t *manifest_candidate =
        malloc(sizeof(*manifest_candidate));
    if (history_candidate == NULL || manifest_candidate == NULL) {
        free(history_candidate);
        free(manifest_candidate);
        memset(&download_writer, 0, sizeof(download_writer));
        return ESP_ERR_NO_MEM;
    }
    *history_candidate = *sync_history;
    uint32_t history_sequence = 0U;
    const uint64_t initial_sync_utc = trusted_time_now();
    const revlink_sync_status_t history_status =
        revlink_sync_history_record_at(
            history_candidate,
            (const uint8_t *)download_writer.device_path,
            device_path_length,
            download_writer.device_time_raw,
            download_writer.written_size,
            initial_sync_utc,
            digest,
            object_name,
            &history_sequence
        );
    if (history_status != REVLINK_SYNC_OK
        || save_sync_history(history_candidate) != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Immutable object preserved but history publication failed: %s",
            revlink_sync_status_name(history_status)
        );
        free(history_candidate);
        free(manifest_candidate);
        memset(&download_writer, 0, sizeof(download_writer));
        return ESP_FAIL;
    }
    *sync_history = *history_candidate;
    free(history_candidate);

    *manifest_candidate = *sync_manifest;
    const revlink_sync_status_t manifest_status =
        revlink_sync_manifest_upsert_at(
            manifest_candidate,
            (const uint8_t *)download_writer.device_path,
            device_path_length,
            download_writer.device_time_raw,
            download_writer.written_size,
            initial_sync_utc,
            digest,
            object_name
        );
    if (manifest_status != REVLINK_SYNC_OK
        || save_sync_manifest(manifest_candidate) != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Object and history preserved but current manifest failed: %s",
            revlink_sync_status_name(manifest_status)
        );
        free(manifest_candidate);
        memset(&download_writer, 0, sizeof(download_writer));
        return ESP_FAIL;
    }
    *sync_manifest = *manifest_candidate;
    free(manifest_candidate);

    if (download_writer.kind == REVLINK_SD_FILE_STARTUP_SCREEN
        && preserve_synced_startup_profile(
               download_writer.final_path,
               digest
           ) != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Startup screen cache committed but reusable profile preservation "
            "will be retried on the next sync"
        );
    }

    ESP_LOGI(
        TAG,
        "Immutable %s object %s: path=%s bytes=%" PRIu32
        " sha256=%s current_entries=%u history_sequence=%" PRIu32
        " history_versions=%u",
        download_writer.kind == REVLINK_SD_FILE_MAP ? "map"
            : download_writer.kind == REVLINK_SD_FILE_STARTUP_SCREEN
                ? "startup screen"
            : download_writer.kind == REVLINK_SD_FILE_SCREENSHOT
                ? "screenshot" : "datalog",
        deduplicated ? "DEDUPLICATED" : "PASSED",
        download_writer.final_path,
        download_writer.written_size,
        digest_text,
        (unsigned int)sync_manifest->count,
        history_sequence,
        (unsigned int)sync_history->count
    );
    publish_portal_snapshot();
    memset(&download_writer, 0, sizeof(download_writer));
    return ESP_OK;
}

#if CONFIG_REVLINK_SD_FORMAT_ACCEPTANCE
static void fill_test_chunk(uint8_t *buffer, size_t length, size_t offset)
{
    for (size_t index = 0; index < length; ++index) {
        const uint32_t position = (uint32_t)(offset + index);
        buffer[index] = (uint8_t)((position * 31U + 17U) ^ (position >> 8U));
    }
}

static esp_err_t write_acceptance_file(
    const char *temporary_path,
    uint8_t expected_digest[32]
)
{
    FILE *stream = fopen(temporary_path, "wb");
    if (stream == NULL) {
        ESP_LOGE(TAG, "Unable to open acceptance file: errno=%d", errno);
        return ESP_FAIL;
    }

    uint8_t *buffer = malloc(REVLINK_SD_CHUNK_BYTES);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate acceptance write buffer");
        fclose(stream);
        return ESP_ERR_NO_MEM;
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(buffer);
        fclose(stream);
        return ESP_FAIL;
    }

    esp_err_t status = ESP_OK;
    for (size_t offset = 0; offset < REVLINK_SD_TEST_BYTES;
         offset += REVLINK_SD_CHUNK_BYTES) {
        fill_test_chunk(buffer, REVLINK_SD_CHUNK_BYTES, offset);
        if (fwrite(buffer, 1, REVLINK_SD_CHUNK_BYTES, stream)
                != REVLINK_SD_CHUNK_BYTES
            || psa_hash_update(
                   &hash,
                   buffer,
                   REVLINK_SD_CHUNK_BYTES
               ) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Acceptance write failed at offset %u", (unsigned)offset);
            status = ESP_FAIL;
            break;
        }
    }

    if (status == ESP_OK
        && (fflush(stream) != 0 || fsync(fileno(stream)) != 0)) {
        ESP_LOGE(TAG, "Acceptance flush failed: errno=%d", errno);
        status = ESP_FAIL;
    }
    if (fclose(stream) != 0) {
        ESP_LOGE(TAG, "Acceptance close failed: errno=%d", errno);
        status = ESP_FAIL;
    }
    size_t digest_length = 0;
    if (status == ESP_OK) {
        const psa_status_t hash_status = psa_hash_finish(
            &hash,
            expected_digest,
            32,
            &digest_length
        );
        if (hash_status != PSA_SUCCESS || digest_length != 32) {
            status = ESP_FAIL;
        }
    } else {
        psa_hash_abort(&hash);
    }
    free(buffer);
    return status;
}

static esp_err_t verify_acceptance_file(
    const char *path,
    const uint8_t expected_digest[32]
)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        ESP_LOGE(TAG, "Unable to reopen acceptance file: errno=%d", errno);
        return ESP_FAIL;
    }

    uint8_t *buffer = malloc(REVLINK_SD_CHUNK_BYTES);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate acceptance read buffer");
        fclose(stream);
        return ESP_ERR_NO_MEM;
    }
    uint8_t actual_digest[32];
    size_t total = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(buffer);
        fclose(stream);
        return ESP_FAIL;
    }

    esp_err_t status = ESP_OK;
    while (true) {
        const size_t count =
            fread(buffer, 1, REVLINK_SD_CHUNK_BYTES, stream);
        if (count > 0) {
            total += count;
            if (psa_hash_update(&hash, buffer, count) != PSA_SUCCESS) {
                status = ESP_FAIL;
                break;
            }
        }
        if (count < REVLINK_SD_CHUNK_BYTES) {
            if (ferror(stream)) {
                ESP_LOGE(TAG, "Acceptance read failed: errno=%d", errno);
                status = ESP_FAIL;
            }
            break;
        }
    }
    fclose(stream);
    free(buffer);

    size_t digest_length = 0;
    if (status == ESP_OK) {
        const psa_status_t hash_status = psa_hash_finish(
            &hash,
            actual_digest,
            sizeof(actual_digest),
            &digest_length
        );
        if (hash_status != PSA_SUCCESS
            || digest_length != sizeof(actual_digest)) {
            status = ESP_FAIL;
        }
    } else {
        psa_hash_abort(&hash);
    }

    if (status != ESP_OK
        || total != REVLINK_SD_TEST_BYTES
        || memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0) {
        ESP_LOGE(TAG, "Acceptance checksum verification failed");
        return ESP_FAIL;
    }

    char digest_text[65];
    for (size_t index = 0; index < sizeof(actual_digest); ++index) {
        snprintf(&digest_text[index * 2], 3, "%02x", actual_digest[index]);
    }
    digest_text[64] = '\0';
    ESP_LOGI(
        TAG,
        "microSD remount verification PASSED: bytes=%u sha256=%s",
        (unsigned)total,
        digest_text
    );
    return ESP_OK;
}

static esp_err_t write_marker(const char *digest_text)
{
    const char *path =
        REVLINK_SD_MOUNT_POINT "/revlink/system/acceptance/sd-card-ok.txt";
    FILE *stream = fopen(path, "w");
    if (stream == NULL) {
        return ESP_FAIL;
    }
    fprintf(
        stream,
        "RevLink microSD acceptance passed\n"
        "filesystem=FAT\n"
        "bus=SDMMC-4bit-20MHz\n"
        "test_bytes=%u\n"
        "sha256=%s\n",
        (unsigned)REVLINK_SD_TEST_BYTES,
        digest_text
    );
    bool failed = false;
    if (fflush(stream) != 0) {
        failed = true;
    }
    if (fsync(fileno(stream)) != 0) {
        failed = true;
    }
    if (fclose(stream) != 0) {
        failed = true;
    }
    return failed ? ESP_FAIL : ESP_OK;
}
#endif

static esp_err_t mount_card(bool format_if_mount_failed)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 8,
        .allocation_unit_size = 32U * 1024U,
    };
    storage_card = NULL;
#if CONFIG_REVLINK_SD_SPI_TRANSPORT
    return esp_vfs_fat_sdspi_mount(
        REVLINK_SD_MOUNT_POINT,
        &storage_host,
        &storage_spi_slot,
        &mount_config,
        &storage_card
    );
#else
    return esp_vfs_fat_sdmmc_mount(
        REVLINK_SD_MOUNT_POINT,
        &storage_host,
        &storage_slot,
        &mount_config,
        &storage_card
    );
#endif
}

#if CONFIG_REVLINK_SD_SPI_TRANSPORT
static revlink_sd_storage_state_t classify_failed_mount(void)
{
    sdspi_dev_handle_t handle = -1;
    const esp_err_t attach_status =
        sdspi_host_init_device(&storage_spi_slot, &handle);
    if (attach_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "microSD presence probe could not attach SPI device: %s",
            esp_err_to_name(attach_status)
        );
        return REVLINK_SD_STORAGE_ERROR;
    }

    sdmmc_host_t probe_host = storage_host;
    probe_host.slot = handle;
    sdmmc_card_t probe_card = {0};
    const esp_err_t probe_status =
        sdmmc_card_init(&probe_host, &probe_card);
    const esp_err_t remove_status = sdspi_host_remove_device(handle);
    if (remove_status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "microSD presence probe cleanup failed: %s",
            esp_err_to_name(remove_status)
        );
    }
    if (probe_status == ESP_OK) {
        ESP_LOGW(
            TAG,
            "microSD responds but its filesystem is unreadable"
        );
        return REVLINK_SD_STORAGE_UNREADABLE;
    }
    ESP_LOGW(
        TAG,
        "No responding microSD card detected: %s",
        esp_err_to_name(probe_status)
    );
    return REVLINK_SD_STORAGE_MISSING;
}
#endif

static void log_capacity(void)
{
    uint64_t total = 0;
    uint64_t available = 0;
    const esp_err_t status =
        esp_vfs_fat_info(REVLINK_SD_MOUNT_POINT, &total, &available);
    if (status != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to read filesystem capacity: %s",
            esp_err_to_name(status)
        );
        return;
    }
    ESP_LOGI(
        TAG,
        "microSD filesystem ready: total=%" PRIu64 " bytes available=%" PRIu64
        " bytes",
        total,
        available
    );
}

static void log_acceptance_marker(void)
{
    const char *path =
        REVLINK_SD_MOUNT_POINT "/revlink/system/acceptance/sd-card-ok.txt";
    struct stat marker;
    if (stat(path, &marker) == 0 && marker.st_size > 0) {
        ESP_LOGI(
            TAG,
            "microSD acceptance marker preserved: bytes=%" PRIu64,
            (uint64_t)marker.st_size
        );
    } else {
        ESP_LOGW(TAG, "microSD acceptance marker is not present yet");
    }
}

#if CONFIG_REVLINK_SD_FORMAT_ACCEPTANCE
static esp_err_t format_and_accept(void)
{
    ESP_LOGW(TAG, "AUTHORIZED ONE-TIME FORMAT: erasing the microSD filesystem");
    esp_err_t status = esp_vfs_fat_sdcard_format(
        REVLINK_SD_MOUNT_POINT,
        storage_card
    );
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "microSD format failed: %s", esp_err_to_name(status));
        return status;
    }
    ESP_LOGI(TAG, "microSD format completed");

    status = provision_layout();
    if (status != ESP_OK) {
        return status;
    }

    const char *temporary_path =
        REVLINK_SD_MOUNT_POINT "/revlink/system/acceptance/storage-test.tmp";
    const char *accepted_path =
        REVLINK_SD_MOUNT_POINT "/revlink/system/acceptance/storage-test.bin";
    unlink(temporary_path);
    unlink(accepted_path);

    uint8_t expected_digest[32];
    status = write_acceptance_file(temporary_path, expected_digest);
    if (status != ESP_OK) {
        return status;
    }
    if (rename(temporary_path, accepted_path) != 0) {
        ESP_LOGE(TAG, "Atomic acceptance rename failed: errno=%d", errno);
        return ESP_FAIL;
    }

    esp_vfs_fat_sdcard_unmount(REVLINK_SD_MOUNT_POINT, storage_card);
    storage_card = NULL;
    status = mount_card(false);
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "microSD remount failed: %s", esp_err_to_name(status));
        return status;
    }

    status = verify_acceptance_file(accepted_path, expected_digest);
    if (status != ESP_OK) {
        return status;
    }

    char digest_text[65];
    for (size_t index = 0; index < sizeof(expected_digest); ++index) {
        snprintf(&digest_text[index * 2], 3, "%02x", expected_digest[index]);
    }
    digest_text[64] = '\0';
    status = write_marker(digest_text);
    if (status != ESP_OK) {
        return status;
    }
    log_acceptance_marker();
    if (unlink(accepted_path) != 0) {
        ESP_LOGW(TAG, "Unable to remove acceptance payload: errno=%d", errno);
    }

    log_capacity();
    return ESP_OK;
}
#endif

static esp_err_t storage_start_internal(bool format_authorized)
{
    storage_state = REVLINK_SD_STORAGE_UNKNOWN;
    storage_last_error = ESP_OK;
    if (portal_snapshot_mutex == NULL) {
        portal_snapshot_mutex = xSemaphoreCreateMutex();
    }
    if (portal_snapshot_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&portal_snapshot, 0, sizeof(portal_snapshot));

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Unable to initialize SHA-256 provider");
        return ESP_FAIL;
    }

    const sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    esp_err_t status =
        sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &storage_power);
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "Unable to enable microSD LDO: %s", esp_err_to_name(status));
        return status;
    }

#if CONFIG_REVLINK_SD_SPI_TRANSPORT
    /*
     * A CPU reset can leave the removable card powered in its previous native
     * SD mode. SPI mode is selected only during card initialization, so force
     * a card-only power cycle before driving CS and issuing SPI commands.
     */
    status = sd_pwr_ctrl_del_on_chip_ldo(storage_power);
    storage_power = NULL;
    if (status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to power-cycle microSD before SPI startup: %s",
            esp_err_to_name(status)
        );
        return status;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    status = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &storage_power);
    if (status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to restore microSD power after SPI reset: %s",
            esp_err_to_name(status)
        );
        return status;
    }

    storage_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    storage_host.slot = SPI3_HOST;
    storage_host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    storage_host.pwr_ctrl_handle = storage_power;

    const spi_bus_config_t bus_config = {
        .mosi_io_num = GPIO_NUM_44,
        .miso_io_num = GPIO_NUM_39,
        .sclk_io_num = GPIO_NUM_43,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = REVLINK_SD_CHUNK_BYTES,
    };
    status = spi_bus_initialize(
        storage_host.slot,
        &bus_config,
        SPI_DMA_CH_AUTO
    );
    if (status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to initialize isolated microSD SPI3 bus: %s",
            esp_err_to_name(status)
        );
        sd_pwr_ctrl_del_on_chip_ldo(storage_power);
        storage_power = NULL;
        return status;
    }
    storage_spi_bus_initialized = true;
    storage_spi_slot =
        (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    storage_spi_slot.host_id = storage_host.slot;
    storage_spi_slot.gpio_cs = GPIO_NUM_42;
    ESP_LOGI(
        TAG,
        "microSD transport: SPI3 CLK=43 MOSI=44 MISO=39 CS=42"
    );
#else
    storage_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    storage_host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    storage_host.pwr_ctrl_handle = storage_power;

    storage_slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    storage_slot.width = 4;
    storage_slot.clk = 43;
    storage_slot.cmd = 44;
    storage_slot.d0 = 39;
    storage_slot.d1 = 40;
    storage_slot.d2 = 41;
    storage_slot.d3 = 42;
    storage_slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    status = mount_card(format_authorized);
#if CONFIG_REVLINK_SD_FORMAT_ACCEPTANCE
    if (status != ESP_OK && !format_authorized) {
        ESP_LOGW(
            TAG,
            "Existing filesystem did not mount (%s); formatting is authorized",
            esp_err_to_name(status)
        );
        status = mount_card(true);
    }
#endif
    if (status != ESP_OK) {
        storage_last_error = status;
#if CONFIG_REVLINK_SD_SPI_TRANSPORT
        storage_state = classify_failed_mount();
#else
        storage_state = status == ESP_FAIL
            ? REVLINK_SD_STORAGE_UNREADABLE
            : REVLINK_SD_STORAGE_MISSING;
#endif
        ESP_LOGE(
            TAG,
            "microSD mount failed%s: %s",
            format_authorized ? " after authorized format attempt" :
                " without automatic formatting",
            esp_err_to_name(status)
        );
#if CONFIG_REVLINK_SD_SPI_TRANSPORT
        if (storage_spi_bus_initialized) {
            spi_bus_free(storage_host.slot);
            storage_spi_bus_initialized = false;
        }
#endif
        sd_pwr_ctrl_del_on_chip_ldo(storage_power);
        storage_power = NULL;
        return status;
    }

    sdmmc_card_print_info(stdout, storage_card);
    if (format_authorized) {
        ESP_LOGW(
            TAG,
            "AUTHORIZED PHYSICAL FORMAT completed; all previous card data "
            "was erased"
        );
    }

#if CONFIG_REVLINK_SD_FORMAT_ACCEPTANCE
    storage_state = REVLINK_SD_STORAGE_MOUNTED;
    storage_last_error = ESP_OK;
    return format_and_accept();
#else
    status = provision_layout();
    if (status != ESP_OK) {
        const esp_err_t provision_error = status;
        const esp_err_t cleanup_status = revlink_sd_storage_stop();
        if (cleanup_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "microSD cleanup after layout failure also failed: %s",
                esp_err_to_name(cleanup_status)
            );
        }
        storage_state = REVLINK_SD_STORAGE_ERROR;
        storage_last_error = provision_error;
        return provision_error;
    }

    storage_state = REVLINK_SD_STORAGE_MOUNTED;
    storage_last_error = ESP_OK;
    const esp_err_t restore_status = restore_portal_namespace();
    if (
        restore_status != ESP_OK
        && restore_status != ESP_ERR_NOT_FOUND
        && restore_status != ESP_ERR_INVALID_STATE
    ) {
        ESP_LOGW(
            TAG,
            "Cached portal namespace was not restored: %s",
            esp_err_to_name(restore_status)
        );
    }
    log_capacity();
    log_acceptance_marker();
    publish_portal_snapshot();
    if (restore_status == ESP_OK) {
        ESP_LOGI(
            TAG,
            "microSD mounted without formatting; cached portal "
            "namespace restored"
        );
    } else {
        ESP_LOGI(
            TAG,
            "microSD mounted without formatting; device namespace "
            "pending authoritative AccessPort identity"
        );
    }
    return ESP_OK;
#endif
}

esp_err_t revlink_sd_storage_start(void)
{
    return storage_start_internal(false);
}

revlink_sd_storage_status_t revlink_sd_storage_status(void)
{
    return (revlink_sd_storage_status_t){
        .state = storage_state,
        .error = storage_last_error,
    };
}

esp_err_t revlink_sd_storage_format_unreadable(void)
{
    if (
        storage_state != REVLINK_SD_STORAGE_UNREADABLE
        || storage_card != NULL
        || storage_power != NULL
#if CONFIG_REVLINK_SD_SPI_TRANSPORT
        || storage_spi_bus_initialized
#endif
    ) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(
        TAG,
        "Authorized physical recovery is formatting the unreadable microSD; "
        "all card data will be lost"
    );
    return storage_start_internal(true);
}

esp_err_t revlink_sd_storage_stop(void)
{
    revlink_sd_download_abort(NULL);
    revlink_sd_release_device(NULL);

    if (storage_card != NULL) {
        const esp_err_t unmount_status =
            esp_vfs_fat_sdcard_unmount(
                REVLINK_SD_MOUNT_POINT,
                storage_card
            );
        storage_card = NULL;
        if (unmount_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "microSD unmount failed: %s",
                esp_err_to_name(unmount_status)
            );
            return unmount_status;
        }
    }

    free(sync_manifest);
    sync_manifest = NULL;
    free(sync_history);
    sync_history = NULL;

#if CONFIG_REVLINK_SD_SPI_TRANSPORT
    if (storage_spi_bus_initialized) {
        const esp_err_t bus_status = spi_bus_free(storage_host.slot);
        if (bus_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Unable to release microSD SPI3 bus: %s",
                esp_err_to_name(bus_status)
            );
            return bus_status;
        }
        storage_spi_bus_initialized = false;
    }
#endif
    if (storage_power != NULL) {
        const esp_err_t power_status =
            sd_pwr_ctrl_del_on_chip_ldo(storage_power);
        if (power_status != ESP_OK) {
            ESP_LOGE(
                TAG,
                "microSD LDO release failed: %s",
                esp_err_to_name(power_status)
            );
            return power_status;
        }
        storage_power = NULL;
    }
    if (portal_snapshot_mutex != NULL) {
        if (
            xSemaphoreTake(portal_snapshot_mutex, portMAX_DELAY) == pdTRUE
        ) {
            memset(&portal_snapshot, 0, sizeof(portal_snapshot));
            xSemaphoreGive(portal_snapshot_mutex);
        }
        vSemaphoreDelete(portal_snapshot_mutex);
        portal_snapshot_mutex = NULL;
    }
    storage_state = REVLINK_SD_STORAGE_UNKNOWN;
    storage_last_error = ESP_OK;
    ESP_LOGI(TAG, "microSD unmounted and powered down");
    return ESP_OK;
}
