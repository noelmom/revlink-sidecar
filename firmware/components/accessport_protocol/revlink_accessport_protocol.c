#include "revlink_accessport_protocol.h"

#include <stdbool.h>
#include <string.h>

#define REVLINK_AP_BOOST_HEADER_SIZE 31U
#define REVLINK_AP_RECORD_OVERHEAD \
    (REVLINK_AP_PREFIX_SIZE + REVLINK_AP_BOOST_HEADER_SIZE + REVLINK_AP_CHECKSUM_SIZE)
#define REVLINK_AP_MAX_RECORD_SIZE (UINT16_MAX + 7U)

static const uint8_t BOOST_HEADER[REVLINK_AP_BOOST_HEADER_SIZE] = {
    's', 'e', 'r', 'i', 'a', 'l', 'i', 'z', 'a', 't', 'i', 'o', 'n',
    ':', ':', 'a', 'r', 'c', 'h', 'i', 'v', 'e',
    0x03, 0x04, 0x04, 0x04, 0x08, 0x01, 0x00, 0x00, 0x00,
};

static const uint8_t IDENTITY_HANDSHAKE
    [REVLINK_AP_IDENTITY_HANDSHAKE_COUNT][11] = {
    {0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0xcd, 0xdb, 0x75, 0x01},
    {0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x7f, 0xfb, 0xa9, 0x11},
    {0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x28, 0xc9, 0xea, 0xb7, 0x04},
    {0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x1f, 0xda, 0xeb, 0xd3, 0x92},
};

static const uint8_t DISCONNECT_REQUEST[REVLINK_AP_DISCONNECT_REQUEST_SIZE] = {
    0x02, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x05, 0xf0, 0xbb, 0x5c, 0xb1,
};

static const uint8_t DISCONNECT_ACK[REVLINK_AP_DISCONNECT_ACK_SIZE] = {
    0x02, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x01, 0x35, 0x82, 0x6c, 0x4d, 0x38,
};

typedef struct {
    uint8_t *cursor;
    size_t remaining;
} byte_writer_t;

static void store_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void store_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void store_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void store_u24_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 16U);
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)value;
}

static void store_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint16_t load_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint16_t load_u16_le(const uint8_t *input)
{
    return (uint16_t)(input[0] | ((uint16_t)input[1] << 8U));
}

static uint32_t load_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U)
        | ((uint32_t)input[1] << 16U)
        | ((uint32_t)input[2] << 8U)
        | input[3];
}

static uint32_t load_u32_le(const uint8_t *input)
{
    return input[0]
        | ((uint32_t)input[1] << 8U)
        | ((uint32_t)input[2] << 16U)
        | ((uint32_t)input[3] << 24U);
}

static uint32_t load_u24_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 16U)
        | ((uint32_t)input[1] << 8U)
        | input[2];
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

void revlink_ap_jamcrc_init(revlink_ap_jamcrc_t *state)
{
    if (state != NULL) {
        state->value = UINT32_MAX;
    }
}

void revlink_ap_jamcrc_update(
    revlink_ap_jamcrc_t *state,
    const uint8_t *data,
    size_t length
)
{
    if (state == NULL || (length > 0U && data == NULL)) {
        return;
    }
    state->value = crc32_update(state->value, data, length);
}

uint32_t revlink_ap_jamcrc_finish_zeroed_trailer(
    revlink_ap_jamcrc_t *state
)
{
    static const uint8_t zero_trailer[REVLINK_AP_CHECKSUM_SIZE] = {0};
    if (state == NULL) {
        return 0U;
    }
    state->value = crc32_update(
        state->value,
        zero_trailer,
        sizeof(zero_trailer)
    );
    return state->value;
}

static bool writer_bytes(byte_writer_t *writer, const uint8_t *value, size_t length)
{
    if (length > writer->remaining || (length > 0 && value == NULL)) {
        return false;
    }
    if (length > 0) {
        memcpy(writer->cursor, value, length);
        writer->cursor += length;
        writer->remaining -= length;
    }
    return true;
}

static bool writer_zeros(byte_writer_t *writer, size_t length)
{
    if (length > writer->remaining) {
        return false;
    }
    memset(writer->cursor, 0, length);
    writer->cursor += length;
    writer->remaining -= length;
    return true;
}

static bool writer_lp(byte_writer_t *writer, const uint8_t *value, size_t length)
{
    if (length > UINT32_MAX || writer->remaining < 4U) {
        return false;
    }
    store_u32_le(writer->cursor, (uint32_t)length);
    writer->cursor += 4U;
    writer->remaining -= 4U;
    return writer_bytes(writer, value, length);
}

static bool writer_u32_le(byte_writer_t *writer, uint32_t value)
{
    if (writer->remaining < 4U) {
        return false;
    }
    store_u32_le(writer->cursor, value);
    writer->cursor += 4U;
    writer->remaining -= 4U;
    return true;
}

static bool checked_add(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static revlink_ap_status_t begin_record(
    uint16_t opcode,
    size_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *record_length,
    byte_writer_t *payload_writer
)
{
    size_t total_length = 0;
    if (output == NULL || record_length == NULL || payload_writer == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (!checked_add(REVLINK_AP_RECORD_OVERHEAD, payload_length, &total_length)
        || total_length > REVLINK_AP_MAX_RECORD_SIZE) {
        return REVLINK_AP_RECORD_TOO_LARGE;
    }
    if (output_capacity < total_length) {
        return REVLINK_AP_BUFFER_TOO_SMALL;
    }

    memset(output, 0, total_length);
    output[0] = 0x02;
    store_u16_be(&output[3], (uint16_t)(total_length - 7U));
    store_u16_le(&output[6], opcode);
    memcpy(&output[REVLINK_AP_PREFIX_SIZE], BOOST_HEADER, sizeof(BOOST_HEADER));

    payload_writer->cursor = &output[REVLINK_AP_PREFIX_SIZE + REVLINK_AP_BOOST_HEADER_SIZE];
    payload_writer->remaining = payload_length;
    *record_length = total_length;
    return REVLINK_AP_OK;
}

static revlink_ap_status_t finish_record(
    uint8_t *output,
    size_t record_length,
    const byte_writer_t *payload_writer,
    size_t *output_length
)
{
    if (output_length == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (payload_writer->remaining != 0U) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    const uint32_t crc = revlink_ap_jamcrc_zeroed_trailer(output, record_length);
    store_u32_be(&output[record_length - REVLINK_AP_CHECKSUM_SIZE], crc);
    *output_length = record_length;
    return REVLINK_AP_OK;
}

uint32_t revlink_ap_jamcrc_zeroed_trailer(const uint8_t *record, size_t length)
{
    if (record == NULL || length < REVLINK_AP_CHECKSUM_SIZE) {
        return 0U;
    }

    uint32_t crc = UINT32_MAX;
    const size_t trailer_offset = length - REVLINK_AP_CHECKSUM_SIZE;
    crc = crc32_update(crc, record, trailer_offset);
    static const uint8_t zero_trailer[REVLINK_AP_CHECKSUM_SIZE] = {0};
    crc = crc32_update(crc, zero_trailer, sizeof(zero_trailer));
    return crc;
}

revlink_ap_status_t revlink_ap_build_record(
    uint16_t opcode,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (payload_length > 0U && payload == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        opcode,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    if (!writer_bytes(&writer, payload, payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

revlink_ap_status_t revlink_ap_build_list(
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    size_t payload_length = 0;
    if ((path_length > 0U && path == NULL)
        || !checked_add(4U, path_length, &payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        REVLINK_AP_OPCODE_LIST,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    if (!writer_lp(&writer, path, path_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

revlink_ap_status_t revlink_ap_build_select_map(
    const uint8_t *name,
    size_t name_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    size_t payload_length = 0;
    if ((name_length > 0U && name == NULL)
        || !checked_add(32U, name_length, &payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        REVLINK_AP_OPCODE_SELECT_MAP,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    if (!writer_zeros(&writer, 2U)
        || !writer_lp(&writer, name, name_length)
        || !writer_zeros(&writer, 26U)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

revlink_ap_status_t revlink_ap_build_download(
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    size_t payload_length = 37U;
    if ((name_length > 0U && name == NULL) || (path_length > 0U && path == NULL)
        || !checked_add(payload_length, name_length, &payload_length)
        || !checked_add(payload_length, path_length, &payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        REVLINK_AP_OPCODE_DOWNLOAD,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    const uint8_t marker[] = {0x00, 0x01};
    if (!writer_bytes(&writer, marker, sizeof(marker))
        || !writer_lp(&writer, name, name_length)
        || !writer_zeros(&writer, 27U)
        || !writer_lp(&writer, path, path_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

revlink_ap_status_t revlink_ap_build_temp_notice(
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    size_t payload_length = 0;
    if ((path_length > 0U && path == NULL)
        || !checked_add(4U, path_length, &payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        REVLINK_AP_OPCODE_TEMP_NOTICE,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    if (!writer_lp(&writer, path, path_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

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
)
{
    size_t payload_length = 37U;
    if ((name_length > 0U && name == NULL) || (path_length > 0U && path == NULL)
        || !checked_add(payload_length, name_length, &payload_length)
        || !checked_add(payload_length, path_length, &payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        REVLINK_AP_OPCODE_UPLOAD,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    const uint8_t marker[] = {0x00, 0x01};
    if (!writer_bytes(&writer, marker, sizeof(marker))
        || !writer_lp(&writer, name, name_length)
        || !writer_u32_le(&writer, modification_time)
        || !writer_u32_le(&writer, file_size)
        || !writer_zeros(&writer, 19U)
        || !writer_lp(&writer, path, path_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

revlink_ap_status_t revlink_ap_build_chunk(
    const uint8_t *payload,
    size_t payload_length,
    uint8_t direction_flag,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    size_t total_length = 0;
    if ((payload_length > 0U && payload == NULL)
        || output == NULL
        || output_length == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (payload_length > REVLINK_AP_MAX_CHUNK_PAYLOAD
        || !checked_add(
            REVLINK_AP_CHUNK_PREFIX_SIZE + REVLINK_AP_CHECKSUM_SIZE,
            payload_length,
            &total_length
        )) {
        return REVLINK_AP_RECORD_TOO_LARGE;
    }
    if (output_capacity < total_length) {
        return REVLINK_AP_BUFFER_TOO_SMALL;
    }

    const revlink_ap_status_t prefix_status =
        revlink_ap_build_chunk_prefix(
            payload_length,
            direction_flag,
            output
        );
    if (prefix_status != REVLINK_AP_OK) {
        return prefix_status;
    }
    if (payload_length > 0U) {
        memcpy(&output[REVLINK_AP_CHUNK_PREFIX_SIZE], payload, payload_length);
    }
    memset(
        &output[REVLINK_AP_CHUNK_PREFIX_SIZE + payload_length],
        0,
        REVLINK_AP_CHECKSUM_SIZE
    );
    const uint32_t crc = revlink_ap_jamcrc_zeroed_trailer(output, total_length);
    store_u32_be(&output[total_length - REVLINK_AP_CHECKSUM_SIZE], crc);
    *output_length = total_length;
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_build_chunk_prefix(
    size_t payload_length,
    uint8_t direction_flag,
    uint8_t output[REVLINK_AP_CHUNK_PREFIX_SIZE]
)
{
    if (output == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (payload_length > REVLINK_AP_MAX_CHUNK_PAYLOAD) {
        return REVLINK_AP_RECORD_TOO_LARGE;
    }
    output[0] = 0x02;
    output[1] = 0x00;
    store_u24_be(
        &output[2],
        (uint32_t)(payload_length + REVLINK_AP_CHECKSUM_SIZE)
    );
    output[5] = 0x00;
    output[6] = direction_flag;
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_validate_chunk(const uint8_t *chunk, size_t length)
{
    if (chunk == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (length < REVLINK_AP_CHUNK_PREFIX_SIZE + REVLINK_AP_CHECKSUM_SIZE
        || chunk[0] != 0x02
        || chunk[1] != 0x00
        || chunk[5] != 0x00
        || (size_t)load_u24_be(&chunk[2]) + REVLINK_AP_CHUNK_PREFIX_SIZE != length) {
        return REVLINK_AP_INVALID_FRAME;
    }
    if (revlink_ap_jamcrc_zeroed_trailer(chunk, length)
        != load_u32_be(&chunk[length - REVLINK_AP_CHECKSUM_SIZE])) {
        return REVLINK_AP_INVALID_CHECKSUM;
    }
    return REVLINK_AP_OK;
}

bool revlink_ap_is_plain_ack(
    const uint8_t *response,
    size_t response_length,
    uint8_t expected_subtype
)
{
    if (response == NULL
        || response_length != REVLINK_AP_DISCONNECT_ACK_SIZE
        || response[0] != 0x02U
        || response[1] != 0x00U
        || response[2] != 0x00U
        || response[5] != 0x00U
        || response[6] != 0x01U
        || response[7] != expected_subtype
        || (size_t)load_u16_be(&response[3]) + 7U != response_length) {
        return false;
    }
    return revlink_ap_jamcrc_zeroed_trailer(response, response_length)
        == load_u32_be(
            &response[response_length - REVLINK_AP_CHECKSUM_SIZE]
        );
}

bool revlink_ap_plain_error_code(
    const uint8_t *response,
    size_t response_length,
    uint16_t *error_code
)
{
    if (response == NULL || error_code == NULL
        || response_length < 12U
        || response[0] != 0x02U
        || response[1] != 0x00U
        || response[2] != 0x00U
        || response[5] != 0x00U
        || response[6] != 0x02U
        || (size_t)load_u16_be(&response[3]) + 7U != response_length
        || revlink_ap_jamcrc_zeroed_trailer(response, response_length)
            != load_u32_be(
                   &response[response_length - REVLINK_AP_CHECKSUM_SIZE]
               )) {
        return false;
    }

    const size_t payload_length =
        response_length - 7U - REVLINK_AP_CHECKSUM_SIZE;
    uint32_t parsed = 0U;
    for (size_t index = 0U; index < payload_length; ++index) {
        const uint8_t value = response[7U + index];
        if (value < '0' || value > '9') {
            return false;
        }
        parsed = parsed * 10U + (uint32_t)(value - '0');
        if (parsed > UINT16_MAX) {
            return false;
        }
    }
    *error_code = (uint16_t)parsed;
    return true;
}

static bool ascii_equal_case_insensitive(uint8_t left, uint8_t right)
{
    if (left >= 'A' && left <= 'Z') {
        left = (uint8_t)(left + ('a' - 'A'));
    }
    if (right >= 'A' && right <= 'Z') {
        right = (uint8_t)(right + ('a' - 'A'));
    }
    return left == right;
}

/*
 * Body-identical to a download; only the opcode differs. Kept as its own
 * function rather than a parameter on the download builder so a delete can
 * never be produced by accident from a read path.
 */
revlink_ap_status_t revlink_ap_build_delete(
    const uint8_t *name,
    size_t name_length,
    const uint8_t *path,
    size_t path_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    size_t payload_length = 37U;
    if ((name_length > 0U && name == NULL) || (path_length > 0U && path == NULL)
        || !checked_add(payload_length, name_length, &payload_length)
        || !checked_add(payload_length, path_length, &payload_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t record_length = 0;
    byte_writer_t writer = {0};
    revlink_ap_status_t status = begin_record(
        REVLINK_AP_OPCODE_DELETE,
        payload_length,
        output,
        output_capacity,
        &record_length,
        &writer
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    const uint8_t marker[] = {0x00, 0x01};
    if (!writer_bytes(&writer, marker, sizeof(marker))
        || !writer_lp(&writer, name, name_length)
        || !writer_zeros(&writer, 27U)
        || !writer_lp(&writer, path, path_length)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    return finish_record(output, record_length, &writer, output_length);
}

revlink_ap_status_t revlink_ap_validate_delete_target(
    const uint8_t *path,
    size_t path_length
)
{
    static const uint8_t maps_prefix[] = "maps/";
    static const uint8_t datalog_prefix[] = "datalog/";

    if (path == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t file_offset = 0U;
    if (path_length > sizeof(maps_prefix) - 1U
        && memcmp(path, maps_prefix, sizeof(maps_prefix) - 1U) == 0) {
        file_offset = sizeof(maps_prefix) - 1U;
    } else if (path_length > sizeof(datalog_prefix) - 1U
        && memcmp(path, datalog_prefix, sizeof(datalog_prefix) - 1U) == 0) {
        file_offset = sizeof(datalog_prefix) - 1U;
    } else {
        return REVLINK_AP_UPLOAD_TARGET_REJECTED;
    }

    /*
     * One file, directly inside that directory. A separator here would mean a
     * subdirectory, and ".." anywhere would mean traversal; neither is a thing
     * this product deletes.
     */
    for (size_t index = file_offset; index < path_length; ++index) {
        if (path[index] == '\0' || path[index] == '/' || path[index] == '\\'
            || (path[index] == '.' && index + 1U < path_length
                && path[index + 1U] == '.')) {
            return REVLINK_AP_UPLOAD_TARGET_REJECTED;
        }
    }
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_validate_upload_target(
    const uint8_t *path,
    size_t path_length,
    uint32_t file_size,
    revlink_ap_upload_kind_t *kind
)
{
    static const uint8_t startup_path[] = "images/startup_screen.fb";
    static const uint8_t maps_prefix[] = "maps/";
    static const uint8_t map_extension[] = ".ptm";

    if (path == NULL || kind == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (path_length == sizeof(startup_path) - 1U
        && memcmp(path, startup_path, sizeof(startup_path) - 1U) == 0) {
        if (file_size != REVLINK_AP_STARTUP_SCREEN_BYTES) {
            return REVLINK_AP_UPLOAD_TARGET_REJECTED;
        }
        *kind = REVLINK_AP_UPLOAD_STARTUP_SCREEN;
        return REVLINK_AP_OK;
    }

    if (path_length <= sizeof(maps_prefix) - 1U + sizeof(map_extension) - 1U
        || memcmp(path, maps_prefix, sizeof(maps_prefix) - 1U) != 0
        || file_size == 0U
        || file_size > REVLINK_AP_MAX_CHUNK_PAYLOAD) {
        return REVLINK_AP_UPLOAD_TARGET_REJECTED;
    }

    const size_t file_offset = sizeof(maps_prefix) - 1U;
    for (size_t index = file_offset; index < path_length; ++index) {
        if (path[index] == '\0' || path[index] == '/' || path[index] == '\\'
            || (path[index] == '.' && index + 1U < path_length && path[index + 1U] == '.')) {
            return REVLINK_AP_UPLOAD_TARGET_REJECTED;
        }
    }
    const size_t extension_offset = path_length - (sizeof(map_extension) - 1U);
    for (size_t index = 0; index < sizeof(map_extension) - 1U; ++index) {
        if (!ascii_equal_case_insensitive(
                path[extension_offset + index],
                map_extension[index]
            )) {
            return REVLINK_AP_UPLOAD_TARGET_REJECTED;
        }
    }

    *kind = REVLINK_AP_UPLOAD_MAP;
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_validate_record(const uint8_t *record, size_t length)
{
    if (record == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (length < REVLINK_AP_RECORD_OVERHEAD
        || length > REVLINK_AP_MAX_RECORD_SIZE
        || record[0] != 0x02
        || record[1] != 0x00
        || record[2] != 0x00
        || record[5] != 0x00
        || record[8] != 0x00
        || record[9] != 0x00
        || record[10] != 0x00
        || (size_t)load_u16_be(&record[3]) + 7U != length) {
        return REVLINK_AP_INVALID_FRAME;
    }
    if (revlink_ap_jamcrc_zeroed_trailer(record, length)
        != load_u32_be(&record[length - REVLINK_AP_CHECKSUM_SIZE])) {
        return REVLINK_AP_INVALID_CHECKSUM;
    }
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_parse_record(
    const uint8_t *record,
    size_t length,
    revlink_ap_record_view_t *view
)
{
    if (view == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    const revlink_ap_status_t status = revlink_ap_validate_record(record, length);
    if (status != REVLINK_AP_OK) {
        return status;
    }
    if (memcmp(&record[REVLINK_AP_PREFIX_SIZE], BOOST_HEADER, sizeof(BOOST_HEADER)) != 0) {
        return REVLINK_AP_INVALID_FRAME;
    }

    view->opcode = load_u16_le(&record[6]);
    view->payload = &record[REVLINK_AP_PREFIX_SIZE + REVLINK_AP_BOOST_HEADER_SIZE];
    view->payload_length = length - REVLINK_AP_RECORD_OVERHEAD;
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_identity_handshake_request(
    size_t index,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (output == NULL || output_length == NULL
        || index >= REVLINK_AP_IDENTITY_HANDSHAKE_COUNT) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (output_capacity < sizeof(IDENTITY_HANDSHAKE[index])) {
        return REVLINK_AP_BUFFER_TOO_SMALL;
    }
    memcpy(
        output,
        IDENTITY_HANDSHAKE[index],
        sizeof(IDENTITY_HANDSHAKE[index])
    );
    *output_length = sizeof(IDENTITY_HANDSHAKE[index]);
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_disconnect_request(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (output == NULL || output_length == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (output_capacity < sizeof(DISCONNECT_REQUEST)) {
        return REVLINK_AP_BUFFER_TOO_SMALL;
    }
    memcpy(output, DISCONNECT_REQUEST, sizeof(DISCONNECT_REQUEST));
    *output_length = sizeof(DISCONNECT_REQUEST);
    return REVLINK_AP_OK;
}

bool revlink_ap_is_disconnect_ack(
    const uint8_t *response,
    size_t response_length
)
{
    return response != NULL
        && response_length == sizeof(DISCONNECT_ACK)
        && memcmp(response, DISCONNECT_ACK, sizeof(DISCONNECT_ACK)) == 0;
}

static bool read_lp_view(
    const uint8_t *payload,
    size_t payload_length,
    size_t *offset,
    const uint8_t **value,
    size_t *value_length
)
{
    if (*offset > payload_length || payload_length - *offset < 4U) {
        return false;
    }
    const uint32_t encoded_length = load_u32_le(&payload[*offset]);
    *offset += 4U;
    if ((size_t)encoded_length > payload_length - *offset) {
        return false;
    }
    *value = &payload[*offset];
    *value_length = (size_t)encoded_length;
    *offset += (size_t)encoded_length;
    return true;
}

static bool copy_identity_field(
    const uint8_t *value,
    size_t length,
    char *output,
    size_t output_capacity,
    bool serial_field
)
{
    if (value == NULL || output == NULL || length == 0U
        || length >= output_capacity) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t byte = value[index];
        if (serial_field) {
            const bool allowed =
                (byte >= 'a' && byte <= 'z')
                || (byte >= 'A' && byte <= 'Z')
                || (byte >= '0' && byte <= '9')
                || byte == '-' || byte == '_';
            if (!allowed) {
                return false;
            }
        } else if (byte < 0x20U || byte == 0x7fU) {
            return false;
        }
    }
    memcpy(output, value, length);
    output[length] = '\0';
    return true;
}

revlink_ap_status_t revlink_ap_parse_device_identity_payload(
    const uint8_t *payload,
    size_t payload_length,
    revlink_ap_device_info_t *identity
)
{
    if (payload == NULL || identity == NULL) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t offset = 0U;
    const uint8_t *text = NULL;
    size_t text_length = 0U;
    if (!read_lp_view(
            payload,
            payload_length,
            &offset,
            &text,
            &text_length
        )
        || offset != payload_length || text_length == 0U) {
        return REVLINK_AP_INVALID_FRAME;
    }

    const uint8_t *fields[6] = {0};
    size_t lengths[6] = {0};
    size_t field_count = 0U;
    size_t start = 0U;
    for (size_t index = 0U; index <= text_length; ++index) {
        if (index == text_length || text[index] == '\n') {
            size_t length = index - start;
            if (length > 0U && text[start + length - 1U] == '\r') {
                --length;
            }
            if (length > 0U) {
                if (field_count >= 6U) {
                    return REVLINK_AP_INVALID_FRAME;
                }
                fields[field_count] = &text[start];
                lengths[field_count] = length;
                ++field_count;
            }
            start = index + 1U;
        }
    }
    if (field_count != 6U) {
        return REVLINK_AP_INVALID_FRAME;
    }

    revlink_ap_device_info_t parsed = {0};
    if (!copy_identity_field(
            fields[0],
            lengths[0],
            parsed.firmware,
            sizeof(parsed.firmware),
            false
        )
        || !copy_identity_field(
            fields[1],
            lengths[1],
            parsed.part_number,
            sizeof(parsed.part_number),
            false
        )
        || !copy_identity_field(
            fields[2],
            lengths[2],
            parsed.serial,
            sizeof(parsed.serial),
            true
        )
        || !copy_identity_field(
            fields[3],
            lengths[3],
            parsed.install_status,
            sizeof(parsed.install_status),
            false
        )
        || !copy_identity_field(
            fields[4],
            lengths[4],
            parsed.vehicle,
            sizeof(parsed.vehicle),
            false
        )
        || !copy_identity_field(
            fields[5],
            lengths[5],
            parsed.trailing_field,
            sizeof(parsed.trailing_field),
            false
        )) {
        return REVLINK_AP_INVALID_FRAME;
    }
    *identity = parsed;
    return REVLINK_AP_OK;
}

revlink_ap_status_t revlink_ap_parse_listing_payload(
    const uint8_t *payload,
    size_t payload_length,
    revlink_ap_listing_entry_t *entries,
    size_t entry_capacity,
    size_t *entry_count
)
{
    if (payload == NULL || entry_count == NULL
        || (entry_capacity > 0U && entries == NULL)) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }
    if (payload_length < 8U
        || payload[0] != 0x00U
        || payload[1] != 0x00U
        || payload[6] != 0x00U
        || payload[7] != 0x01U) {
        return REVLINK_AP_INVALID_FRAME;
    }

    const uint32_t encoded_count = load_u32_le(&payload[2]);
    size_t offset = 8U;
    for (uint32_t index = 0; index < encoded_count; ++index) {
        const uint8_t *name = NULL;
        const uint8_t *path = NULL;
        size_t name_length = 0;
        size_t path_length = 0;
        if (!read_lp_view(
                payload,
                payload_length,
                &offset,
                &name,
                &name_length
            )) {
            return REVLINK_AP_INVALID_FRAME;
        }

        const size_t metadata_length = index == 0U ? 27U : 25U;
        if (offset > payload_length
            || metadata_length > payload_length - offset) {
            return REVLINK_AP_INVALID_FRAME;
        }
        const uint32_t device_time_raw = load_u32_le(&payload[offset]);
        const uint32_t size = load_u32_le(&payload[offset + 4U]);
        const bool is_directory =
            payload[offset + metadata_length - 1U] == 0x01U;
        offset += metadata_length;

        if (!read_lp_view(
                payload,
                payload_length,
                &offset,
                &path,
                &path_length
            )) {
            return REVLINK_AP_INVALID_FRAME;
        }

        if ((size_t)index < entry_capacity) {
            entries[index] = (revlink_ap_listing_entry_t){
                .name = name,
                .name_length = name_length,
                .device_time_raw = device_time_raw,
                .size = size,
                .is_directory = is_directory,
                .path = path,
                .path_length = path_length,
            };
        }
    }
    if (offset != payload_length) {
        return REVLINK_AP_INVALID_FRAME;
    }

    *entry_count = (size_t)encoded_count;
    return (size_t)encoded_count > entry_capacity
        ? REVLINK_AP_BUFFER_TOO_SMALL
        : REVLINK_AP_OK;
}

void revlink_ap_download_decoder_init(
    revlink_ap_download_decoder_t *decoder,
    size_t expected_payload_size
)
{
    if (decoder == NULL) {
        return;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->state = REVLINK_AP_DOWNLOAD_DECODER_ARCHIVE;
    decoder->expected_payload_size = expected_payload_size;
    decoder->chunk_crc = UINT32_MAX;
}

static revlink_ap_status_t validate_download_archive(
    revlink_ap_download_decoder_t *decoder
)
{
    revlink_ap_record_view_t record = {0};
    const revlink_ap_status_t status = revlink_ap_parse_record(
        decoder->archive,
        decoder->archive_size,
        &record
    );
    if (status != REVLINK_AP_OK) {
        return status;
    }
    return record.opcode == REVLINK_AP_OPCODE_RESPONSE
        ? REVLINK_AP_OK
        : REVLINK_AP_INVALID_FRAME;
}

revlink_ap_status_t revlink_ap_download_decoder_feed(
    revlink_ap_download_decoder_t *decoder,
    const uint8_t *data,
    size_t length,
    revlink_ap_payload_sink_t sink,
    void *sink_context
)
{
    if (decoder == NULL || (length > 0U && data == NULL) || sink == NULL
        || decoder->expected_payload_size == 0U) {
        return REVLINK_AP_INVALID_ARGUMENT;
    }

    size_t offset = 0;
    while (offset < length) {
        if (decoder->state == REVLINK_AP_DOWNLOAD_DECODER_COMPLETE) {
            decoder->trailing_bytes += length - offset;
            return REVLINK_AP_OK;
        }

        if (decoder->state == REVLINK_AP_DOWNLOAD_DECODER_ARCHIVE) {
            size_t needed = decoder->archive_target_size > 0U
                ? decoder->archive_target_size - decoder->archive_size
                : 5U - decoder->archive_size;
            const size_t available = length - offset;
            const size_t count = needed < available ? needed : available;
            memcpy(
                &decoder->archive[decoder->archive_size],
                &data[offset],
                count
            );
            decoder->archive_size += count;
            offset += count;

            if (decoder->archive_size >= 3U
                && (decoder->archive[0] != 0x02U
                    || decoder->archive[1] != 0x00U
                    || decoder->archive[2] != 0x00U)) {
                return REVLINK_AP_INVALID_FRAME;
            }
            if (decoder->archive_size == 5U
                && decoder->archive_target_size == 0U) {
                decoder->archive_target_size =
                    (size_t)load_u16_be(&decoder->archive[3]) + 7U;
                if (decoder->archive_target_size < REVLINK_AP_RECORD_OVERHEAD
                    || decoder->archive_target_size > sizeof(decoder->archive)) {
                    return REVLINK_AP_INVALID_FRAME;
                }
            }
            if (decoder->archive_target_size > 0U
                && decoder->archive_size == decoder->archive_target_size) {
                const revlink_ap_status_t status =
                    validate_download_archive(decoder);
                if (status != REVLINK_AP_OK) {
                    return status;
                }
                decoder->state = REVLINK_AP_DOWNLOAD_DECODER_CHUNK_HEADER;
            }
            continue;
        }

        if (decoder->state == REVLINK_AP_DOWNLOAD_DECODER_CHUNK_HEADER) {
            const size_t needed =
                sizeof(decoder->chunk_header) - decoder->chunk_header_size;
            const size_t available = length - offset;
            const size_t count = needed < available ? needed : available;
            memcpy(
                &decoder->chunk_header[decoder->chunk_header_size],
                &data[offset],
                count
            );
            decoder->chunk_header_size += count;
            offset += count;
            if (decoder->chunk_header_size == sizeof(decoder->chunk_header)) {
                const uint32_t declared =
                    load_u24_be(&decoder->chunk_header[2]);
                if (decoder->chunk_header[0] != 0x02U
                    || decoder->chunk_header[1] != 0x00U
                    || decoder->chunk_header[5] != 0x00U
                    || decoder->chunk_header[6]
                        != REVLINK_AP_DOWNLOAD_CHUNK_FLAG
                    || declared < REVLINK_AP_CHECKSUM_SIZE) {
                    return REVLINK_AP_INVALID_FRAME;
                }
                decoder->payload_size =
                    (size_t)declared - REVLINK_AP_CHECKSUM_SIZE;
                if (decoder->payload_size != decoder->expected_payload_size) {
                    return REVLINK_AP_INVALID_FRAME;
                }
                decoder->chunk_crc = crc32_update(
                    decoder->chunk_crc,
                    decoder->chunk_header,
                    sizeof(decoder->chunk_header)
                );
                decoder->state = REVLINK_AP_DOWNLOAD_DECODER_PAYLOAD;
            }
            continue;
        }

        if (decoder->state == REVLINK_AP_DOWNLOAD_DECODER_PAYLOAD) {
            const size_t needed =
                decoder->payload_size - decoder->payload_received;
            const size_t available = length - offset;
            const size_t count = needed < available ? needed : available;
            if (count > 0U
                && !sink(sink_context, &data[offset], count)) {
                return REVLINK_AP_SINK_ERROR;
            }
            decoder->chunk_crc = crc32_update(
                decoder->chunk_crc,
                &data[offset],
                count
            );
            decoder->payload_received += count;
            offset += count;
            if (decoder->payload_received == decoder->payload_size) {
                decoder->state = REVLINK_AP_DOWNLOAD_DECODER_TRAILER;
            }
            continue;
        }

        if (decoder->state == REVLINK_AP_DOWNLOAD_DECODER_TRAILER) {
            const size_t needed =
                sizeof(decoder->trailer) - decoder->trailer_size;
            const size_t available = length - offset;
            const size_t count = needed < available ? needed : available;
            memcpy(
                &decoder->trailer[decoder->trailer_size],
                &data[offset],
                count
            );
            decoder->trailer_size += count;
            offset += count;
            if (decoder->trailer_size == sizeof(decoder->trailer)) {
                static const uint8_t zero_trailer[
                    REVLINK_AP_CHECKSUM_SIZE
                ] = {0};
                decoder->chunk_crc = crc32_update(
                    decoder->chunk_crc,
                    zero_trailer,
                    sizeof(zero_trailer)
                );
                const bool trailer_zero =
                    memcmp(
                        decoder->trailer,
                        zero_trailer,
                        sizeof(zero_trailer)
                    ) == 0;
                const bool trailer_valid =
                    load_u32_be(decoder->trailer) == decoder->chunk_crc;
                if (!trailer_zero && !trailer_valid) {
                    return REVLINK_AP_INVALID_CHECKSUM;
                }
                decoder->state = REVLINK_AP_DOWNLOAD_DECODER_COMPLETE;
            }
        }
    }
    return REVLINK_AP_OK;
}

bool revlink_ap_download_decoder_complete(
    const revlink_ap_download_decoder_t *decoder
)
{
    return decoder != NULL
        && decoder->state == REVLINK_AP_DOWNLOAD_DECODER_COMPLETE;
}

const char *revlink_ap_status_name(revlink_ap_status_t status)
{
    switch (status) {
    case REVLINK_AP_OK:
        return "ok";
    case REVLINK_AP_INVALID_ARGUMENT:
        return "invalid argument";
    case REVLINK_AP_BUFFER_TOO_SMALL:
        return "buffer too small";
    case REVLINK_AP_RECORD_TOO_LARGE:
        return "record too large";
    case REVLINK_AP_INVALID_FRAME:
        return "invalid frame";
    case REVLINK_AP_INVALID_CHECKSUM:
        return "invalid checksum";
    case REVLINK_AP_SINK_ERROR:
        return "sink error";
    case REVLINK_AP_UPLOAD_TARGET_REJECTED:
        return "upload target rejected";
    default:
        return "unknown status";
    }
}
