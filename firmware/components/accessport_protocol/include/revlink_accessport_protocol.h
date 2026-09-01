#ifndef REVLINK_ACCESSPORT_PROTOCOL_H
#define REVLINK_ACCESSPORT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_AP_PREFIX_SIZE 11U
#define REVLINK_AP_CHECKSUM_SIZE 4U
#define REVLINK_AP_CHUNK_PREFIX_SIZE 7U
#define REVLINK_AP_UPLOAD_CHUNK_FLAG 0x23U
#define REVLINK_AP_DOWNLOAD_CHUNK_FLAG 0x01U
#define REVLINK_AP_MAX_CHUNK_PAYLOAD ((1U << 24U) - 5U)
#define REVLINK_AP_STARTUP_SCREEN_BYTES 153600U
#define REVLINK_AP_DOWNLOAD_RESPONSE_RECORD_CAPACITY 4096U
#define REVLINK_AP_OPCODE_RESPONSE 0x1601U
#define REVLINK_AP_OPCODE_SELECT_MAP 0x1612U
#define REVLINK_AP_OPCODE_DOWNLOAD 0x1620U
#define REVLINK_AP_OPCODE_TEMP_NOTICE 0x1621U
#define REVLINK_AP_OPCODE_UPLOAD 0x1622U
#define REVLINK_AP_OPCODE_LIST 0x1626U
#define REVLINK_AP_IDENTITY_HANDSHAKE_COUNT 4U
#define REVLINK_AP_IDENTITY_RESPONSE_INDEX 2U
#define REVLINK_AP_DISCONNECT_REQUEST_SIZE 11U
#define REVLINK_AP_DISCONNECT_ACK_SIZE 12U
#define REVLINK_AP_FIRMWARE_CAPACITY 64U
#define REVLINK_AP_PART_NUMBER_CAPACITY 40U
#define REVLINK_AP_SERIAL_CAPACITY 64U
#define REVLINK_AP_INSTALL_STATUS_CAPACITY 40U
#define REVLINK_AP_VEHICLE_CAPACITY 192U
#define REVLINK_AP_TRAILING_FIELD_CAPACITY 40U

typedef enum {
    REVLINK_AP_OK = 0,
    REVLINK_AP_INVALID_ARGUMENT,
    REVLINK_AP_BUFFER_TOO_SMALL,
    REVLINK_AP_RECORD_TOO_LARGE,
    REVLINK_AP_INVALID_FRAME,
    REVLINK_AP_INVALID_CHECKSUM,
    REVLINK_AP_SINK_ERROR,
    REVLINK_AP_UPLOAD_TARGET_REJECTED,
} revlink_ap_status_t;

typedef enum {
    REVLINK_AP_UPLOAD_MAP = 1,
    REVLINK_AP_UPLOAD_STARTUP_SCREEN = 2,
} revlink_ap_upload_kind_t;

typedef struct {
    uint16_t opcode;
    const uint8_t *payload;
    size_t payload_length;
} revlink_ap_record_view_t;

typedef struct {
    const uint8_t *name;
    size_t name_length;
    uint32_t device_time_raw;
    uint32_t size;
    bool is_directory;
    const uint8_t *path;
    size_t path_length;
} revlink_ap_listing_entry_t;

typedef struct {
    char firmware[REVLINK_AP_FIRMWARE_CAPACITY];
    char part_number[REVLINK_AP_PART_NUMBER_CAPACITY];
    char serial[REVLINK_AP_SERIAL_CAPACITY];
    char install_status[REVLINK_AP_INSTALL_STATUS_CAPACITY];
    char vehicle[REVLINK_AP_VEHICLE_CAPACITY];
    char trailing_field[REVLINK_AP_TRAILING_FIELD_CAPACITY];
} revlink_ap_device_info_t;

typedef enum {
    REVLINK_AP_DOWNLOAD_DECODER_ARCHIVE = 0,
    REVLINK_AP_DOWNLOAD_DECODER_CHUNK_HEADER,
    REVLINK_AP_DOWNLOAD_DECODER_PAYLOAD,
    REVLINK_AP_DOWNLOAD_DECODER_TRAILER,
    REVLINK_AP_DOWNLOAD_DECODER_COMPLETE,
} revlink_ap_download_decoder_state_t;

typedef struct {
    revlink_ap_download_decoder_state_t state;
    size_t expected_payload_size;
    uint8_t archive[REVLINK_AP_DOWNLOAD_RESPONSE_RECORD_CAPACITY];
    size_t archive_size;
    size_t archive_target_size;
    uint8_t chunk_header[REVLINK_AP_CHUNK_PREFIX_SIZE];
    size_t chunk_header_size;
    size_t payload_size;
    size_t payload_received;
    uint8_t trailer[REVLINK_AP_CHECKSUM_SIZE];
    size_t trailer_size;
    uint32_t chunk_crc;
    size_t trailing_bytes;
} revlink_ap_download_decoder_t;

typedef bool (*revlink_ap_payload_sink_t)(
    void *context,
    const uint8_t *data,
    size_t length
);

typedef struct {
    uint32_t value;
} revlink_ap_jamcrc_t;

/*
 * Incremental JAMCRC helpers for transports that must stream a file without
 * buffering the complete upload chunk in RAM.
 */
void revlink_ap_jamcrc_init(revlink_ap_jamcrc_t *state);
void revlink_ap_jamcrc_update(
    revlink_ap_jamcrc_t *state,
    const uint8_t *data,
    size_t length
);
uint32_t revlink_ap_jamcrc_finish_zeroed_trailer(
    revlink_ap_jamcrc_t *state
);

/*
 * Return the capture-verified CRC-32/JAMCRC value for a record. The final four
 * bytes are treated as zero, matching the AccessPort check-field convention.
 */
uint32_t revlink_ap_jamcrc_zeroed_trailer(const uint8_t *record, size_t length);

/*
 * Build the common Boost archive record. This primitive is transport-agnostic
 * and never transmits anything.
 */
revlink_ap_status_t revlink_ap_build_record(
    uint16_t opcode,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_ap_status_t revlink_ap_build_list(
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_ap_status_t revlink_ap_build_select_map(
    const uint8_t *name,
    size_t name_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_ap_status_t revlink_ap_build_download(
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_ap_status_t revlink_ap_build_temp_notice(
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

/*
 * Build the capture-verified 0x1622 upload request and its following file
 * chunk. These functions only serialize bytes. A transport must independently
 * enforce administrator capability, user consent, path restrictions, and the
 * ready/completion acknowledgement sequence before sending them.
 */
revlink_ap_status_t revlink_ap_build_upload(
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    uint32_t modification_time,
    uint32_t file_size,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

revlink_ap_status_t revlink_ap_build_chunk(
    const uint8_t *payload,
    size_t payload_length,
    uint8_t direction_flag,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

/*
 * Build only the seven-byte upload/download chunk prefix. A streaming
 * transport can hash this prefix, the file bytes, and a zero trailer before
 * sending the same prefix and file followed by the resulting big-endian CRC.
 */
revlink_ap_status_t revlink_ap_build_chunk_prefix(
    size_t payload_length,
    uint8_t direction_flag,
    uint8_t output[REVLINK_AP_CHUNK_PREFIX_SIZE]
);

revlink_ap_status_t revlink_ap_validate_chunk(
    const uint8_t *chunk,
    size_t length
);

/*
 * Validate one complete non-archive acknowledgement record and require the
 * class-0x01 payload to contain exactly the requested subtype.
 */
bool revlink_ap_is_plain_ack(
    const uint8_t *response,
    size_t response_length,
    uint8_t expected_subtype
);

/*
 * Validate one complete class-0x02 plain error/NAK record whose payload is an
 * ASCII decimal error code. Returns false for malformed records, bad checksums,
 * non-error classes, or non-numeric payloads.
 */
bool revlink_ap_plain_error_code(
    const uint8_t *response,
    size_t response_length,
    uint16_t *error_code
);

/*
 * Enforce RevLink's product write allowlist. Only a .ptm file directly inside
 * maps/ or the exact 153,600-byte images/startup_screen.fb is authorized.
 * Captured CSV uploads remain protocol evidence and are intentionally rejected.
 */
revlink_ap_status_t revlink_ap_validate_upload_target(
    const uint8_t *path,
    size_t path_length,
    uint32_t file_size,
    revlink_ap_upload_kind_t *kind
);

revlink_ap_status_t revlink_ap_validate_record(
    const uint8_t *record,
    size_t length
);

revlink_ap_status_t revlink_ap_parse_record(
    const uint8_t *record,
    size_t length,
    revlink_ap_record_view_t *view
);

/*
 * Return one of the four capture-verified, read-only session/identity mini
 * requests. The third response contains the true AccessPort serial. USB
 * descriptor serials are placeholders and must never key persistent data.
 */
revlink_ap_status_t revlink_ap_identity_handshake_request(
    size_t index,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

/*
 * Build the capture-verified polite session close (mini subtype 0x05).
 * A successful device acknowledgement is mini subtype 0x35. The AccessPort
 * normally re-enumerates shortly afterward, so a disappearing USB handle
 * after the OUT transfer is also an expected transport outcome.
 */
revlink_ap_status_t revlink_ap_disconnect_request(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

bool revlink_ap_is_disconnect_ack(
    const uint8_t *response,
    size_t response_length
);

/*
 * Parse the six-line identity payload inside a validated 0x1601 record.
 * Every field is copied into a bounded, NUL-terminated destination.
 */
revlink_ap_status_t revlink_ap_parse_device_identity_payload(
    const uint8_t *payload,
    size_t payload_length,
    revlink_ap_device_info_t *identity
);

/*
 * Parse the capture-verified 0x1601 directory-listing payload exposed by
 * revlink_ap_parse_record(). Entry strings are non-owning views into payload.
 *
 * The function validates the complete payload even when entry_capacity is
 * smaller than the reported entry count. In that case it returns
 * REVLINK_AP_BUFFER_TOO_SMALL and reports the required count.
 */
revlink_ap_status_t revlink_ap_parse_listing_payload(
    const uint8_t *payload,
    size_t payload_length,
    revlink_ap_listing_entry_t *entries,
    size_t entry_capacity,
    size_t *entry_count
);

/*
 * Incrementally decode a download response without buffering file content.
 * The archive response record is checksum-validated, then the following
 * transfer chunk is bounded to expected_payload_size and streamed to sink.
 * Download chunks with the device-observed zero trailer or a valid JAMCRC
 * trailer are accepted. Bytes following a complete chunk are counted but
 * never passed to the payload sink.
 */
void revlink_ap_download_decoder_init(
    revlink_ap_download_decoder_t *decoder,
    size_t expected_payload_size
);

revlink_ap_status_t revlink_ap_download_decoder_feed(
    revlink_ap_download_decoder_t *decoder,
    const uint8_t *data,
    size_t length,
    revlink_ap_payload_sink_t sink,
    void *sink_context
);

bool revlink_ap_download_decoder_complete(
    const revlink_ap_download_decoder_t *decoder
);

const char *revlink_ap_status_name(revlink_ap_status_t status);

#ifdef __cplusplus
}
#endif

#endif
