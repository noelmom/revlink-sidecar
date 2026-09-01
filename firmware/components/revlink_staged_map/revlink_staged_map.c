#include "revlink_staged_map.h"

#include <string.h>

/*
 * On-disk layout, little-endian, total REVLINK_STAGED_MAP_RECORD_BYTES:
 *
 *   0   magic               u32
 *   4   version             u32
 *   8   kind                u32
 *  12   size                u32
 *  16   sha256              32 bytes
 *  48   name                128 bytes, NUL-terminated
 * 176   destination         256 bytes, NUL-terminated
 * 432   target_part_number   40 bytes, NUL-terminated
 * 472   target_serial        64 bytes, NUL-terminated
 * 536   crc32               u32, over bytes 0..535
 */
#define OFFSET_MAGIC 0U
#define OFFSET_VERSION 4U
#define OFFSET_KIND 8U
#define OFFSET_SIZE 12U
#define OFFSET_SHA256 16U
#define OFFSET_NAME 48U
#define OFFSET_DESTINATION 176U
#define OFFSET_PART_NUMBER 432U
#define OFFSET_SERIAL 472U
#define OFFSET_CRC 536U

static uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void put_u32(uint8_t *buffer, size_t offset, uint32_t value)
{
    buffer[offset + 0U] = (uint8_t)(value & 0xFFu);
    buffer[offset + 1U] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[offset + 2U] = (uint8_t)((value >> 16) & 0xFFu);
    buffer[offset + 3U] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *buffer, size_t offset)
{
    return (uint32_t)buffer[offset + 0U]
        | ((uint32_t)buffer[offset + 1U] << 8)
        | ((uint32_t)buffer[offset + 2U] << 16)
        | ((uint32_t)buffer[offset + 3U] << 24);
}

/* Copy a required, non-empty, NUL-terminated string into a fixed field. */
static bool put_string(
    uint8_t *buffer,
    size_t offset,
    size_t capacity,
    const char *value
)
{
    if (value == NULL) return false;
    const size_t length = strnlen(value, capacity);
    if (length == 0U || length >= capacity) return false;
    memset(buffer + offset, 0, capacity);
    memcpy(buffer + offset, value, length);
    return true;
}

static bool get_string(
    const uint8_t *buffer,
    size_t offset,
    size_t capacity,
    char *output
)
{
    const size_t length =
        strnlen((const char *)(buffer + offset), capacity);
    if (length == 0U || length >= capacity) return false;
    memcpy(output, buffer + offset, length);
    output[length] = '\0';
    return true;
}

revlink_staged_map_status_t revlink_staged_map_encode(
    const revlink_staged_map_record_t *record,
    uint8_t *buffer,
    size_t capacity,
    size_t *written
)
{
    if (record == NULL || buffer == NULL
        || capacity < REVLINK_STAGED_MAP_RECORD_BYTES) {
        return REVLINK_STAGED_MAP_INVALID_ARGUMENT;
    }
    if (record->size == 0U
        || (record->kind != REVLINK_STAGED_MAP_KIND_MAP
            && record->kind != REVLINK_STAGED_MAP_KIND_STARTUP_IMAGE)) {
        return REVLINK_STAGED_MAP_INVALID_ARGUMENT;
    }

    memset(buffer, 0, REVLINK_STAGED_MAP_RECORD_BYTES);
    put_u32(buffer, OFFSET_MAGIC, REVLINK_STAGED_MAP_MAGIC);
    put_u32(buffer, OFFSET_VERSION, REVLINK_STAGED_MAP_VERSION);
    put_u32(buffer, OFFSET_KIND, (uint32_t)record->kind);
    put_u32(buffer, OFFSET_SIZE, record->size);
    memcpy(
        buffer + OFFSET_SHA256,
        record->sha256,
        REVLINK_STAGED_MAP_SHA256_BYTES
    );

    if (!put_string(
            buffer,
            OFFSET_NAME,
            REVLINK_STAGED_MAP_NAME_CAPACITY,
            record->name
        )
        || !put_string(
               buffer,
               OFFSET_DESTINATION,
               REVLINK_STAGED_MAP_PATH_CAPACITY,
               record->destination
           )
        || !put_string(
               buffer,
               OFFSET_PART_NUMBER,
               REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY,
               record->target_part_number
           )
        || !put_string(
               buffer,
               OFFSET_SERIAL,
               REVLINK_STAGED_MAP_SERIAL_CAPACITY,
               record->target_serial
           )) {
        memset(buffer, 0, REVLINK_STAGED_MAP_RECORD_BYTES);
        return REVLINK_STAGED_MAP_INVALID_ARGUMENT;
    }

    put_u32(buffer, OFFSET_CRC, crc32_ieee(buffer, OFFSET_CRC));
    if (written != NULL) *written = REVLINK_STAGED_MAP_RECORD_BYTES;
    return REVLINK_STAGED_MAP_OK;
}

revlink_staged_map_status_t revlink_staged_map_decode(
    const uint8_t *buffer,
    size_t length,
    revlink_staged_map_record_t *record
)
{
    if (buffer == NULL || record == NULL) {
        return REVLINK_STAGED_MAP_INVALID_ARGUMENT;
    }
    if (length < REVLINK_STAGED_MAP_RECORD_BYTES) {
        return REVLINK_STAGED_MAP_TRUNCATED;
    }
    if (get_u32(buffer, OFFSET_MAGIC) != REVLINK_STAGED_MAP_MAGIC) {
        return REVLINK_STAGED_MAP_BAD_MAGIC;
    }
    if (get_u32(buffer, OFFSET_VERSION) != REVLINK_STAGED_MAP_VERSION) {
        return REVLINK_STAGED_MAP_UNSUPPORTED_VERSION;
    }
    if (get_u32(buffer, OFFSET_CRC) != crc32_ieee(buffer, OFFSET_CRC)) {
        return REVLINK_STAGED_MAP_BAD_CHECKSUM;
    }

    const uint32_t kind = get_u32(buffer, OFFSET_KIND);
    const uint32_t size = get_u32(buffer, OFFSET_SIZE);
    if (size == 0U
        || (kind != (uint32_t)REVLINK_STAGED_MAP_KIND_MAP
            && kind != (uint32_t)REVLINK_STAGED_MAP_KIND_STARTUP_IMAGE)) {
        return REVLINK_STAGED_MAP_MALFORMED_FIELD;
    }

    revlink_staged_map_record_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.kind = (revlink_staged_map_kind_t)kind;
    decoded.size = size;
    memcpy(
        decoded.sha256,
        buffer + OFFSET_SHA256,
        REVLINK_STAGED_MAP_SHA256_BYTES
    );

    /*
     * Every string is required. A record without both target fields is
     * refused rather than treated as applying to any attached device.
     */
    if (!get_string(
            buffer,
            OFFSET_NAME,
            REVLINK_STAGED_MAP_NAME_CAPACITY,
            decoded.name
        )
        || !get_string(
               buffer,
               OFFSET_DESTINATION,
               REVLINK_STAGED_MAP_PATH_CAPACITY,
               decoded.destination
           )
        || !get_string(
               buffer,
               OFFSET_PART_NUMBER,
               REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY,
               decoded.target_part_number
           )
        || !get_string(
               buffer,
               OFFSET_SERIAL,
               REVLINK_STAGED_MAP_SERIAL_CAPACITY,
               decoded.target_serial
           )) {
        return REVLINK_STAGED_MAP_MALFORMED_FIELD;
    }

    *record = decoded;
    return REVLINK_STAGED_MAP_OK;
}

const char *revlink_staged_map_status_name(
    revlink_staged_map_status_t status
)
{
    switch (status) {
    case REVLINK_STAGED_MAP_OK:
        return "ok";
    case REVLINK_STAGED_MAP_INVALID_ARGUMENT:
        return "invalid-argument";
    case REVLINK_STAGED_MAP_TRUNCATED:
        return "truncated";
    case REVLINK_STAGED_MAP_BAD_MAGIC:
        return "bad-magic";
    case REVLINK_STAGED_MAP_UNSUPPORTED_VERSION:
        return "unsupported-version";
    case REVLINK_STAGED_MAP_BAD_CHECKSUM:
        return "bad-checksum";
    case REVLINK_STAGED_MAP_MALFORMED_FIELD:
        return "malformed-field";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */

static bool bounded_equal(const char *a, const char *b, size_t capacity)
{
    const size_t length = strnlen(a, capacity);
    if (length == 0U || length >= capacity) return false;
    if (strnlen(b, capacity) != length) return false;
    return memcmp(a, b, length) == 0;
}

revlink_staged_map_apply_decision_t revlink_staged_map_evaluate_apply(
    const revlink_staged_map_apply_context_t *context
)
{
    if (context == NULL) {
        return REVLINK_STAGED_MAP_APPLY_WRITES_NOT_COMPILED;
    }

    /* Build gate, then owner gates. Independent by design. */
    if (!context->writes_compiled) {
        return REVLINK_STAGED_MAP_APPLY_WRITES_NOT_COMPILED;
    }
    if (!context->consent_enabled) {
        return REVLINK_STAGED_MAP_APPLY_CONSENT_DISABLED;
    }
    if (!context->auto_apply_enabled) {
        return REVLINK_STAGED_MAP_APPLY_AUTO_APPLY_DISABLED;
    }

    if (!context->staged) {
        return REVLINK_STAGED_MAP_APPLY_NOTHING_STAGED;
    }

    /* An unpinned staged payload is never applied automatically. */
    if (strnlen(
            context->target_part_number,
            REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY
        ) == 0U
        || strnlen(
               context->target_serial,
               REVLINK_STAGED_MAP_SERIAL_CAPACITY
           ) == 0U) {
        return REVLINK_STAGED_MAP_APPLY_UNPINNED;
    }

    if (!context->device_identified
        || strnlen(
               context->attached_serial,
               REVLINK_STAGED_MAP_SERIAL_CAPACITY
           ) == 0U) {
        return REVLINK_STAGED_MAP_APPLY_NO_DEVICE;
    }

    /*
     * Serial is the identity that must match; part number is checked too so a
     * reused or malformed serial cannot alone authorize a write.
     */
    if (!bounded_equal(
            context->target_serial,
            context->attached_serial,
            REVLINK_STAGED_MAP_SERIAL_CAPACITY
        )
        || !bounded_equal(
               context->target_part_number,
               context->attached_part_number,
               REVLINK_STAGED_MAP_PART_NUMBER_CAPACITY
           )) {
        return REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH;
    }

    if (context->recovery_required) {
        return REVLINK_STAGED_MAP_APPLY_RECOVERY_REQUIRED;
    }
    if (context->already_attempted_this_attach) {
        return REVLINK_STAGED_MAP_APPLY_ALREADY_ATTEMPTED;
    }
    if (context->transfer_running) {
        return REVLINK_STAGED_MAP_APPLY_TRANSFER_BUSY;
    }
    if (!context->sync_completed_clean || context->sync_pending != 0U) {
        return REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE;
    }

    return REVLINK_STAGED_MAP_APPLY_ALLOWED;
}

const char *revlink_staged_map_apply_decision_name(
    revlink_staged_map_apply_decision_t decision
)
{
    switch (decision) {
    case REVLINK_STAGED_MAP_APPLY_ALLOWED:
        return "allowed";
    case REVLINK_STAGED_MAP_APPLY_WRITES_NOT_COMPILED:
        return "writes-not-compiled";
    case REVLINK_STAGED_MAP_APPLY_CONSENT_DISABLED:
        return "consent-disabled";
    case REVLINK_STAGED_MAP_APPLY_AUTO_APPLY_DISABLED:
        return "auto-apply-disabled";
    case REVLINK_STAGED_MAP_APPLY_NOTHING_STAGED:
        return "nothing-staged";
    case REVLINK_STAGED_MAP_APPLY_UNPINNED:
        return "unpinned";
    case REVLINK_STAGED_MAP_APPLY_NO_DEVICE:
        return "no-device";
    case REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH:
        return "target-mismatch";
    case REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE:
        return "sync-incomplete";
    case REVLINK_STAGED_MAP_APPLY_TRANSFER_BUSY:
        return "transfer-busy";
    case REVLINK_STAGED_MAP_APPLY_RECOVERY_REQUIRED:
        return "recovery-required";
    case REVLINK_STAGED_MAP_APPLY_ALREADY_ATTEMPTED:
        return "already-attempted";
    }
    return "unknown";
}

bool revlink_staged_map_apply_decision_is_transient(
    revlink_staged_map_apply_decision_t decision
)
{
    switch (decision) {
    case REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE:
    case REVLINK_STAGED_MAP_APPLY_TRANSFER_BUSY:
        return true;
    default:
        return false;
    }
}
