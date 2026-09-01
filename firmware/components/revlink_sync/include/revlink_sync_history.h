#ifndef REVLINK_SYNC_HISTORY_H
#define REVLINK_SYNC_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "revlink_sync_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_SYNC_HISTORY_CAPACITY 512U

typedef struct {
    uint32_t sequence;
    char path[REVLINK_SYNC_PATH_CAPACITY];
    uint32_t device_time_raw;
    uint32_t size;
    uint64_t initial_sync_utc;
    uint8_t sha256[REVLINK_SYNC_SHA256_BYTES];
    char object_name[REVLINK_SYNC_CACHE_NAME_CAPACITY];
} revlink_sync_history_entry_t;

typedef struct {
    revlink_sync_history_entry_t entries[REVLINK_SYNC_HISTORY_CAPACITY];
    size_t count;
    uint32_t next_sequence;
} revlink_sync_history_t;

void revlink_sync_history_init(revlink_sync_history_t *history);

const revlink_sync_history_entry_t *revlink_sync_history_find_version(
    const revlink_sync_history_t *history,
    const uint8_t *path,
    size_t path_length,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES]
);

/*
 * Record an immutable observed version. Re-observing the same path and digest
 * is idempotent. Reusing a path with different bytes appends a new sequence.
 */
revlink_sync_status_t revlink_sync_history_record(
    revlink_sync_history_t *history,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *object_name,
    uint32_t *sequence
);

revlink_sync_status_t revlink_sync_history_record_at(
    revlink_sync_history_t *history,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    uint64_t initial_sync_utc,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *object_name,
    uint32_t *sequence
);

revlink_sync_status_t revlink_sync_history_serialize(
    const revlink_sync_history_t *history,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_sync_status_t revlink_sync_history_parse(
    const char *input,
    size_t input_length,
    revlink_sync_history_t *history
);

#ifdef __cplusplus
}
#endif

#endif
