#ifndef REVLINK_SYNC_MANIFEST_H
#define REVLINK_SYNC_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "revlink_sync_presence.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_SYNC_MANIFEST_CAPACITY 128U
#define REVLINK_SYNC_PATH_CAPACITY 256U
#define REVLINK_SYNC_CACHE_NAME_CAPACITY 160U
#define REVLINK_SYNC_SHA256_BYTES 32U

typedef enum {
    REVLINK_SYNC_OK = 0,
    REVLINK_SYNC_INVALID_ARGUMENT,
    REVLINK_SYNC_INVALID_FORMAT,
    REVLINK_SYNC_CAPACITY_EXCEEDED,
    REVLINK_SYNC_BUFFER_TOO_SMALL,
    REVLINK_SYNC_ALLOCATION_FAILED,
} revlink_sync_status_t;

typedef struct {
    char path[REVLINK_SYNC_PATH_CAPACITY];
    uint32_t device_time_raw;
    uint32_t size;
    uint64_t initial_sync_utc;
    uint8_t sha256[REVLINK_SYNC_SHA256_BYTES];
    char cache_name[REVLINK_SYNC_CACHE_NAME_CAPACITY];
    revlink_sync_presence_t presence;
} revlink_sync_manifest_entry_t;

typedef struct {
    revlink_sync_manifest_entry_t entries[REVLINK_SYNC_MANIFEST_CAPACITY];
    size_t count;
} revlink_sync_manifest_t;

void revlink_sync_manifest_init(revlink_sync_manifest_t *manifest);

const revlink_sync_manifest_entry_t *revlink_sync_manifest_find(
    const revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length
);

bool revlink_sync_manifest_metadata_matches(
    const revlink_sync_manifest_entry_t *entry,
    uint32_t device_time_raw,
    uint32_t size
);

revlink_sync_status_t revlink_sync_manifest_upsert(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *cache_name
);

/*
 * initial_sync_utc is zero when no trusted wall clock exists. Existing
 * identical content keeps its original value, including an unknown zero.
 */
revlink_sync_status_t revlink_sync_manifest_upsert_at(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    uint64_t initial_sync_utc,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *cache_name
);

revlink_sync_status_t revlink_sync_manifest_serialize(
    const revlink_sync_manifest_t *manifest,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_sync_status_t revlink_sync_manifest_parse(
    const char *input,
    size_t input_length,
    revlink_sync_manifest_t *manifest
);

/*
 * Records what is known about one entry's presence on the device. Returns
 * false when the path is not in the manifest, which is not an error: a device
 * listing mentions files that were never cached.
 */
bool revlink_sync_manifest_set_presence(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length,
    revlink_sync_presence_t presence
);

/*
 * Drops one entry. Returns false when the path is not catalogued.
 *
 * Order is not part of the manifest's meaning, so the last entry is moved
 * into the gap rather than shifting the tail. Callers that hold an index
 * across this call are wrong to; find by path instead.
 */
bool revlink_sync_manifest_remove(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length
);

/*
 * How many entries carry this digest. The cache is content-addressed, so two
 * paths with identical bytes share one object on the card, and removing an
 * entry must not remove an object another entry still points at.
 */
size_t revlink_sync_manifest_digest_users(
    const revlink_sync_manifest_t *manifest,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES]
);

const char *revlink_sync_status_name(revlink_sync_status_t status);

#ifdef __cplusplus
}
#endif

#endif
