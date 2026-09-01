#ifndef REVLINK_STAGED_MAP_H
#define REVLINK_STAGED_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A staged map is a map file that has been uploaded to the Sidecar's microSD
 * card while no AccessPort was attached, to be written to the device during a
 * later attach.
 *
 * Two properties make that safe, and both live here so they can be tested
 * without hardware:
 *
 *   1. A staged map records the identity of the AccessPort it was staged for.
 *      It is never written to a different device, because a map built for one
 *      car must never reach another.
 *   2. The decision to write is a pure function of observable state. It is
 *      deliberately not spread across the portal, the sync coordinator, and
 *      the USB transport.
 *
 * This component performs no I/O. The platform layer owns the filesystem and
 * the USB transport; it serializes a record here, and asks here whether a
 * write is permitted.
 */

#define REVLINK_STAGED_MAP_MAGIC 0x544C5652u /* "RVLT", little-endian */
#define REVLINK_STAGED_MAP_VERSION 1u

#define REVLINK_STAGED_MAP_SHA256_BYTES 32U
#define REVLINK_STAGED_MAP_NAME_CAPACITY 128U
#define REVLINK_STAGED_MAP_PATH_CAPACITY 256U
#define REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY 40U
#define REVLINK_STAGED_MAP_SERIAL_CAPACITY 64U

/*
 * Fixed-size on-disk record: magic, version, kind, size, digest, four bounded
 * strings, and a CRC-32 over everything preceding it. Fixed size keeps the
 * reader from trusting any length field found in a corrupt file.
 */
#define REVLINK_STAGED_MAP_RECORD_BYTES 540U

typedef enum {
    REVLINK_STAGED_MAP_OK = 0,
    REVLINK_STAGED_MAP_INVALID_ARGUMENT,
    REVLINK_STAGED_MAP_TRUNCATED,
    REVLINK_STAGED_MAP_BAD_MAGIC,
    REVLINK_STAGED_MAP_UNSUPPORTED_VERSION,
    REVLINK_STAGED_MAP_BAD_CHECKSUM,
    REVLINK_STAGED_MAP_MALFORMED_FIELD,
} revlink_staged_map_status_t;

typedef enum {
    REVLINK_STAGED_MAP_KIND_MAP = 0,
    REVLINK_STAGED_MAP_KIND_STARTUP_IMAGE = 1,
} revlink_staged_map_kind_t;

typedef struct {
    revlink_staged_map_kind_t kind;
    uint32_t size;
    uint8_t sha256[REVLINK_STAGED_MAP_SHA256_BYTES];
    char name[REVLINK_STAGED_MAP_NAME_CAPACITY];
    char destination[REVLINK_STAGED_MAP_PATH_CAPACITY];
    /*
     * The AccessPort this payload was staged for. Both fields are required;
     * an unpinned record is refused at decode time, so a record written by an
     * older build can never be interpreted as "applies to anything".
     */
    char target_part_number[REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY];
    char target_serial[REVLINK_STAGED_MAP_SERIAL_CAPACITY];
} revlink_staged_map_record_t;

/*
 * Serialize into exactly REVLINK_STAGED_MAP_RECORD_BYTES. Unused string bytes
 * are zero-filled so the encoding is deterministic and the digest of the
 * metadata file is stable for a given record.
 */
revlink_staged_map_status_t revlink_staged_map_encode(
    const revlink_staged_map_record_t *record,
    uint8_t *buffer,
    size_t capacity,
    size_t *written
);

revlink_staged_map_status_t revlink_staged_map_decode(
    const uint8_t *buffer,
    size_t length,
    revlink_staged_map_record_t *record
);

const char *revlink_staged_map_status_name(
    revlink_staged_map_status_t status
);

/* ------------------------------------------------------------------ */
/* Auto-apply decision                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    /* The only value that permits a device write. */
    REVLINK_STAGED_MAP_APPLY_ALLOWED = 0,
    REVLINK_STAGED_MAP_APPLY_WRITES_NOT_COMPILED,
    REVLINK_STAGED_MAP_APPLY_CONSENT_DISABLED,
    REVLINK_STAGED_MAP_APPLY_AUTO_APPLY_DISABLED,
    REVLINK_STAGED_MAP_APPLY_NOTHING_STAGED,
    REVLINK_STAGED_MAP_APPLY_UNPINNED,
    REVLINK_STAGED_MAP_APPLY_NO_DEVICE,
    REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH,
    REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE,
    REVLINK_STAGED_MAP_APPLY_TRANSFER_BUSY,
    REVLINK_STAGED_MAP_APPLY_RECOVERY_REQUIRED,
    REVLINK_STAGED_MAP_APPLY_ALREADY_ATTEMPTED,
} revlink_staged_map_apply_decision_t;

typedef struct {
    /* Build and owner gates. Both must be true, independently. */
    bool writes_compiled;
    bool consent_enabled;
    bool auto_apply_enabled;

    /* Staging state. */
    bool staged;
    char target_part_number[REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY];
    char target_serial[REVLINK_STAGED_MAP_SERIAL_CAPACITY];

    /* Attached device, as identified by the read-only identity handshake. */
    bool device_identified;
    char attached_part_number[REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY];
    char attached_serial[REVLINK_STAGED_MAP_SERIAL_CAPACITY];

    /*
     * Sync state. A write is permitted only after a clean, complete read-only
     * sync: an interrupted or partial batch means the cache does not yet
     * reflect the device, and a write would race the remaining work.
     */
    bool sync_completed_clean;
    size_t sync_pending;

    /* Transport state. */
    bool transfer_running;
    bool recovery_required;

    /*
     * One automatic attempt per attach. A failed write is never retried
     * automatically; recovery is a deliberate human action.
     */
    bool already_attempted_this_attach;
} revlink_staged_map_apply_context_t;

/*
 * Returns REVLINK_STAGED_MAP_APPLY_ALLOWED only when every gate passes.
 * Checks are ordered from most static to most transient so the reported
 * reason is the most useful one to show an owner.
 */
revlink_staged_map_apply_decision_t revlink_staged_map_evaluate_apply(
    const revlink_staged_map_apply_context_t *context
);

const char *revlink_staged_map_apply_decision_name(
    revlink_staged_map_apply_decision_t decision
);

/*
 * True when the decision describes a condition that could still change during
 * this attach (so a caller may re-evaluate after more work completes) rather
 * than a settled refusal.
 */
bool revlink_staged_map_apply_decision_is_transient(
    revlink_staged_map_apply_decision_t decision
);

#ifdef __cplusplus
}
#endif

#endif
