#ifndef REVLINK_SYNC_ANNOTATIONS_H
#define REVLINK_SYNC_ANNOTATIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_SYNC_ANNOTATION_CAPACITY 128U
#define REVLINK_SYNC_NOTE_CAPACITY 768U
#define REVLINK_SYNC_ANNOTATION_SHA256_BYTES 32U

typedef enum {
    REVLINK_SYNC_ANNOTATION_OK = 0,
    REVLINK_SYNC_ANNOTATION_INVALID_ARGUMENT,
    REVLINK_SYNC_ANNOTATION_INVALID_FORMAT,
    REVLINK_SYNC_ANNOTATION_CAPACITY_EXCEEDED,
    REVLINK_SYNC_ANNOTATION_BUFFER_TOO_SMALL,
    REVLINK_SYNC_ANNOTATION_ALLOCATION_FAILED,
} revlink_sync_annotation_status_t;

typedef struct {
    uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    uint64_t updated_at_utc;
    char note[REVLINK_SYNC_NOTE_CAPACITY];
    bool has_map_sha256;
    uint8_t map_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
} revlink_sync_annotation_t;

typedef struct {
    revlink_sync_annotation_t entries[REVLINK_SYNC_ANNOTATION_CAPACITY];
    size_t count;
} revlink_sync_annotations_t;

void revlink_sync_annotations_init(revlink_sync_annotations_t *annotations);

const revlink_sync_annotation_t *revlink_sync_annotations_find(
    const revlink_sync_annotations_t *annotations,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
);

/*
 * Notes are version-scoped by SHA-256. updated_at_utc is zero when no trusted
 * wall clock is available. An empty note removes the annotation.
 */
revlink_sync_annotation_status_t revlink_sync_annotations_set(
    revlink_sync_annotations_t *annotations,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const char *note,
    size_t note_length,
    uint64_t updated_at_utc
);

/*
 * Links an exact datalog version to an exact cached map version. Passing NULL
 * removes only the map link; a saved note remains intact.
 */
revlink_sync_annotation_status_t revlink_sync_annotations_set_map(
    revlink_sync_annotations_t *annotations,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const uint8_t map_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    uint64_t updated_at_utc
);

revlink_sync_annotation_status_t revlink_sync_annotations_serialize(
    const revlink_sync_annotations_t *annotations,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_sync_annotation_status_t revlink_sync_annotations_parse(
    const char *input,
    size_t input_length,
    revlink_sync_annotations_t *annotations
);

#ifdef __cplusplus
}
#endif

#endif
