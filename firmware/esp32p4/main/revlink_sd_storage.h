#ifndef REVLINK_SD_STORAGE_H
#define REVLINK_SD_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "revlink_accessport_protocol.h"
#include "revlink_sync_annotations.h"
#include "revlink_sync_presence.h"

#define REVLINK_SD_PORTAL_INVENTORY_CAPACITY \
    REVLINK_SYNC_ANNOTATION_CAPACITY
#define REVLINK_SD_PORTAL_PATH_CAPACITY 256U
#define REVLINK_SD_DEVICE_KEY_CAPACITY 25U
#define REVLINK_SD_CACHED_DEVICE_CAPACITY 16U
#define REVLINK_SD_STARTUP_PROFILE_CAPACITY 16U
#define REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY 41U
#define REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY 81U

typedef enum {
    REVLINK_SD_FILE_DATALOG = 0,
    REVLINK_SD_FILE_MAP,
    REVLINK_SD_FILE_STARTUP_SCREEN,
    REVLINK_SD_FILE_SCREENSHOT,
} revlink_sd_file_kind_t;

typedef enum {
    REVLINK_SD_STORAGE_UNKNOWN = 0,
    REVLINK_SD_STORAGE_MOUNTED,
    REVLINK_SD_STORAGE_MISSING,
    REVLINK_SD_STORAGE_UNREADABLE,
    REVLINK_SD_STORAGE_ERROR,
} revlink_sd_storage_state_t;

typedef struct {
    revlink_sd_storage_state_t state;
    esp_err_t error;
} revlink_sd_storage_status_t;

typedef struct {
    char path[REVLINK_SD_PORTAL_PATH_CAPACITY];
    revlink_sd_file_kind_t kind;
    uint32_t device_time_raw;
    uint32_t size;
    uint64_t initial_sync_utc;
    uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    revlink_sync_presence_t presence;
} revlink_sd_portal_file_t;

typedef struct {
    bool mounted;
    bool namespace_known;
    bool session_selected;
    uint64_t total_bytes;
    uint64_t free_bytes;
    size_t total_files;
    size_t listed_files;
    revlink_ap_device_info_t device;
    revlink_sd_portal_file_t
        files[REVLINK_SD_PORTAL_INVENTORY_CAPACITY];
} revlink_sd_portal_snapshot_t;

typedef struct {
    char key[REVLINK_SD_DEVICE_KEY_CAPACITY];
    revlink_ap_device_info_t identity;
    bool selected;
} revlink_sd_cached_device_t;

typedef struct {
    size_t count;
    revlink_sd_cached_device_t
        devices[REVLINK_SD_CACHED_DEVICE_CAPACITY];
} revlink_sd_cached_devices_snapshot_t;

typedef struct revlink_sd_cached_reader revlink_sd_cached_reader_t;

typedef struct {
    char id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY];
    char name[REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY];
    uint32_t size;
} revlink_sd_startup_profile_t;

typedef struct {
    size_t count;
    revlink_sd_startup_profile_t
        profiles[REVLINK_SD_STARTUP_PROFILE_CAPACITY];
} revlink_sd_startup_profiles_snapshot_t;

typedef struct {
    char path[REVLINK_SD_PORTAL_PATH_CAPACITY];
    revlink_sd_file_kind_t kind;
    uint32_t size;
    bool gzip_encoded;
} revlink_sd_cached_file_info_t;

typedef uint64_t (*revlink_sd_trusted_time_t)(void *context);
typedef void (*revlink_sd_identity_observer_t)(
    void *context,
    const revlink_ap_device_info_t *identity
);

void revlink_sd_storage_configure_time_source(
    void *context,
    revlink_sd_trusted_time_t trusted_time
);

void revlink_sd_storage_configure_identity_observer(
    void *context,
    revlink_sd_identity_observer_t observer
);

esp_err_t revlink_sd_storage_start(void);
esp_err_t revlink_sd_storage_stop(void);

/*
 * Returns the last physical/filesystem classification without touching the
 * card. Only REVLINK_SD_STORAGE_UNREADABLE is eligible for guarded format.
 */
revlink_sd_storage_status_t revlink_sd_storage_status(void);

/*
 * Destructive recovery entry point. This refuses to run unless the most
 * recent probe proved that a card responded but its filesystem did not mount.
 * Physical confirmation is owned by the BOOT-button recovery state machine.
 */
esp_err_t revlink_sd_storage_format_unreadable(void);

esp_err_t revlink_sd_select_device(
    void *context,
    const revlink_ap_device_info_t *identity
);

void revlink_sd_release_device(void *context);

bool revlink_sd_download_is_current(
    void *context,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t expected_size
);

esp_err_t revlink_sd_download_begin(
    void *context,
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t expected_size
);

bool revlink_sd_download_write(
    void *context,
    const uint8_t *data,
    size_t length
);

esp_err_t revlink_sd_download_commit(void *context);

void revlink_sd_download_abort(void *context);

/*
 * Returns a bounded, immutable projection for user-facing status surfaces.
 * The last authoritative device namespace remains published after the USB
 * file session closes; session_selected only describes the current transfer.
 */
esp_err_t revlink_sd_portal_snapshot(
    revlink_sd_portal_snapshot_t *snapshot
);

/*
 * Returns only the two storage gates needed by streaming services without
 * copying the full bounded file inventory onto a small HTTP/task stack.
 */
esp_err_t revlink_sd_portal_io_status(
    bool *mounted,
    bool *session_selected
);

bool revlink_sd_selected_device_matches(
    const revlink_ap_device_info_t *identity
);

esp_err_t revlink_sd_cached_devices_snapshot(
    revlink_sd_cached_devices_snapshot_t *snapshot
);

/*
 * Switch the offline portal projection to an existing serial-isolated cache
 * namespace. The caller must ensure that no USB storage session is active.
 */
esp_err_t revlink_sd_select_cached_device(const char *key);

/*
 * One pass of a device listing.
 *
 * The three calls exist rather than one because absence is only ever proved
 * by a listing that finished. begin() opens a pass, observe() records a path
 * the device reported, and end() applies the result only when the caller can
 * say every collection was listed. A sync that was cancelled, failed, or was
 * cut short by a transport error ends with complete=false and changes
 * nothing: the previous evidence, however old, is better than a fresh guess.
 *
 * Observations are buffered rather than applied as they arrive because a
 * listing runs before the downloads it triggers, so a file first seen on this
 * sync has no manifest entry yet at the moment it is observed.
 */
void revlink_sd_device_scan_begin(void *context);

void revlink_sd_device_scan_observe(
    void *context,
    const uint8_t *path,
    size_t path_length
);

void revlink_sd_device_scan_end(void *context, bool complete);

/*
 * Records that a file is no longer on the AccessPort, without waiting for the
 * next listing to confirm it. Used after a delete the device acknowledged and
 * that was re-listed as gone.
 */
esp_err_t revlink_sd_mark_absent(const char *path);

/*
 * Removes the Sidecar's own cached copy of a file: its manifest entry, its
 * object on the card, and any note or map tag attached to that exact version.
 *
 * The cache is content-addressed, so two catalogued paths holding identical
 * bytes share one object. The object is unlinked only once nothing else
 * refers to its digest; the manifest is the reference count.
 *
 * This is the one destructive operation with no copy left anywhere. Deleting
 * from the AccessPort leaves the Sidecar's copy; deleting from the Sidecar
 * leaves the AccessPort's. Doing both leaves nothing, and the Sidecar has no
 * recycle bin either.
 */
esp_err_t revlink_sd_forget_cached(const char *path);

esp_err_t revlink_sd_annotations_snapshot(
    revlink_sync_annotations_t *annotations
);

esp_err_t revlink_sd_annotation_set(
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const char *note,
    size_t note_length
);

/*
 * Associates a cached datalog version with a cached map version in the same
 * selected device namespace. Passing NULL removes the map association.
 */
esp_err_t revlink_sd_annotation_set_map(
    const uint8_t log_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const uint8_t map_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
);

/*
 * Opens an immutable, content-addressed cache object published by the portal
 * snapshot. The digest is the authority; caller-provided paths are never
 * accepted. Readers remain valid while a later incremental sync publishes a
 * new manifest because committed cache objects are never modified in place.
 */
esp_err_t revlink_sd_cached_reader_open(
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    revlink_sd_cached_reader_t **reader,
    revlink_sd_cached_file_info_t *info
);

esp_err_t revlink_sd_cached_reader_read(
    revlink_sd_cached_reader_t *reader,
    uint8_t *buffer,
    size_t capacity,
    size_t *count
);

void revlink_sd_cached_reader_close(revlink_sd_cached_reader_t *reader);

esp_err_t revlink_sd_startup_profile_begin(
    const char *name,
    uint32_t size
);

esp_err_t revlink_sd_startup_profile_write(
    const uint8_t *data,
    size_t length
);

esp_err_t revlink_sd_startup_profile_commit(char *id, size_t capacity);

void revlink_sd_startup_profile_abort(void);

esp_err_t revlink_sd_startup_profiles_snapshot(
    revlink_sd_startup_profiles_snapshot_t *snapshot
);

esp_err_t revlink_sd_startup_profile_reader_open(
    const char *id,
    revlink_sd_cached_reader_t **reader,
    revlink_sd_startup_profile_t *profile
);

/*
 * Verify the just-published manifest entry for a device read-back. This is a
 * metadata/digest check only and never trusts a browser-supplied cache path.
 */
bool revlink_sd_cached_file_matches(
    const char *path,
    uint32_t size,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
);

#endif
