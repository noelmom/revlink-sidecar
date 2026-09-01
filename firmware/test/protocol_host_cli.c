#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revlink_accessport_protocol.h"

#define RECORD_BUFFER_SIZE (UINT16_MAX + 7U)

typedef struct {
    uint8_t bytes[32];
    size_t length;
} test_sink_t;

static bool test_payload_sink(
    void *context,
    const uint8_t *data,
    size_t length
)
{
    test_sink_t *sink = (test_sink_t *)context;
    if (length > sizeof(sink->bytes) - sink->length) {
        return false;
    }
    memcpy(&sink->bytes[sink->length], data, length);
    sink->length += length;
    return true;
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int parse_hex(const char *hex, uint8_t *output, size_t capacity, size_t *length)
{
    const size_t hex_length = strlen(hex);
    if ((hex_length & 1U) != 0U || hex_length / 2U > capacity) {
        return 0;
    }
    for (size_t index = 0; index < hex_length / 2U; ++index) {
        const int high = hex_nibble(hex[index * 2U]);
        const int low = hex_nibble(hex[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return 0;
        }
        output[index] = (uint8_t)((high << 4) | low);
    }
    *length = hex_length / 2U;
    return 1;
}

static void print_hex(const uint8_t *value, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        printf("%02x", value[index]);
    }
    putchar('\n');
}

static int parse_u32(const char *value, uint32_t *output)
{
    char *end = NULL;
    const unsigned long long parsed = strtoull(value, &end, 0);
    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed > UINT32_MAX) {
        return 0;
    }
    *output = (uint32_t)parsed;
    return 1;
}

static int run_self_test(void)
{
    static const uint8_t expected[] = {
        0x02, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x26, 0x16, 0x00, 0x00, 0x00,
        0x73, 0x65, 0x72, 0x69, 0x61, 0x6c, 0x69, 0x7a, 0x61, 0x74, 0x69,
        0x6f, 0x6e, 0x3a, 0x3a, 0x61, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65,
        0x03, 0x04, 0x04, 0x04, 0x08, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x39, 0xdf, 0xa4, 0x58,
    };
    uint8_t record[sizeof(expected)] = {0};
    size_t length = 0;
    revlink_ap_record_view_t view = {0};

    if (revlink_ap_build_list(NULL, 0, record, sizeof(record), &length) != REVLINK_AP_OK
        || length != sizeof(expected)
        || memcmp(record, expected, sizeof(expected)) != 0
        || revlink_ap_validate_record(record, length) != REVLINK_AP_OK
        || revlink_ap_parse_record(record, length, &view) != REVLINK_AP_OK
        || view.opcode != REVLINK_AP_OPCODE_LIST
        || view.payload_length != 4U) {
        fputs("root listing vector failed\n", stderr);
        return 1;
    }

    record[length - 1U] ^= 0xffU;
    if (revlink_ap_validate_record(record, length) != REVLINK_AP_INVALID_CHECKSUM) {
        fputs("corrupt checksum was accepted\n", stderr);
        return 1;
    }

    static const uint8_t expected_identity_query[] = {
        0x02, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x28, 0xc9, 0xea, 0xb7, 0x04,
    };
    if (revlink_ap_identity_handshake_request(
            REVLINK_AP_IDENTITY_RESPONSE_INDEX,
            record,
            sizeof(record),
            &length
        ) != REVLINK_AP_OK
        || length != sizeof(expected_identity_query)
        || memcmp(record, expected_identity_query, length) != 0
        || revlink_ap_identity_handshake_request(
            REVLINK_AP_IDENTITY_HANDSHAKE_COUNT,
            record,
            sizeof(record),
            &length
        ) != REVLINK_AP_INVALID_ARGUMENT) {
        fputs("identity handshake vector failed\n", stderr);
        return 1;
    }

    static const uint8_t expected_disconnect[] = {
        0x02, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x05, 0xf0, 0xbb, 0x5c, 0xb1,
    };
    static const uint8_t expected_disconnect_ack[] = {
        0x02, 0x00, 0x00, 0x00, 0x05, 0x00,
        0x01, 0x35, 0x82, 0x6c, 0x4d, 0x38,
    };
    if (revlink_ap_disconnect_request(
            record,
            sizeof(record),
            &length
        ) != REVLINK_AP_OK
        || length != sizeof(expected_disconnect)
        || memcmp(record, expected_disconnect, length) != 0
        || !revlink_ap_is_disconnect_ack(
            expected_disconnect_ack,
            sizeof(expected_disconnect_ack)
        )
        || revlink_ap_is_disconnect_ack(
            expected_disconnect_ack,
            sizeof(expected_disconnect_ack) - 1U
        )) {
        fputs("disconnect handshake vector failed\n", stderr);
        return 1;
    }
    if (!revlink_ap_is_plain_ack(
            expected_disconnect_ack,
            sizeof(expected_disconnect_ack),
            0x35U
        )
        || revlink_ap_is_plain_ack(
            expected_disconnect_ack,
            sizeof(expected_disconnect_ack),
            0x07U
        )) {
        fputs("plain acknowledgement validation failed\n", stderr);
        return 1;
    }
    static const uint8_t path_not_found[] = {
        0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x02,
        0x33, 0x38, 0x67, 0xed, 0xe2, 0xd1,
    };
    uint16_t plain_error = 0U;
    if (!revlink_ap_plain_error_code(
            path_not_found,
            sizeof(path_not_found),
            &plain_error
        )
        || plain_error != 38U
        || revlink_ap_plain_error_code(
               path_not_found,
               sizeof(path_not_found) - 1U,
               &plain_error
           )
        || revlink_ap_plain_error_code(
               expected_disconnect_ack,
               sizeof(expected_disconnect_ack),
               &plain_error
           )) {
        fputs("plain error response validation failed\n", stderr);
        return 1;
    }

    static const uint8_t streaming_payload[] = {
        0x52, 0x65, 0x76, 0x4c, 0x69, 0x6e, 0x6b,
        0x00, 0x7f, 0x80, 0xff,
    };
    uint8_t complete_chunk[
        REVLINK_AP_CHUNK_PREFIX_SIZE
        + sizeof(streaming_payload)
        + REVLINK_AP_CHECKSUM_SIZE
    ] = {0};
    uint8_t chunk_prefix[REVLINK_AP_CHUNK_PREFIX_SIZE] = {0};
    size_t complete_chunk_length = 0U;
    revlink_ap_jamcrc_t streaming_crc = {0};
    if (revlink_ap_build_chunk(
            streaming_payload,
            sizeof(streaming_payload),
            REVLINK_AP_UPLOAD_CHUNK_FLAG,
            complete_chunk,
            sizeof(complete_chunk),
            &complete_chunk_length
        ) != REVLINK_AP_OK
        || revlink_ap_build_chunk_prefix(
            sizeof(streaming_payload),
            REVLINK_AP_UPLOAD_CHUNK_FLAG,
            chunk_prefix
        ) != REVLINK_AP_OK
        || memcmp(
            complete_chunk,
            chunk_prefix,
            sizeof(chunk_prefix)
        ) != 0) {
        fputs("streaming chunk prefix failed\n", stderr);
        return 1;
    }
    revlink_ap_jamcrc_init(&streaming_crc);
    revlink_ap_jamcrc_update(
        &streaming_crc,
        chunk_prefix,
        sizeof(chunk_prefix)
    );
    revlink_ap_jamcrc_update(
        &streaming_crc,
        streaming_payload,
        4U
    );
    revlink_ap_jamcrc_update(
        &streaming_crc,
        &streaming_payload[4],
        sizeof(streaming_payload) - 4U
    );
    const uint32_t expected_streaming_crc =
        ((uint32_t)complete_chunk[complete_chunk_length - 4U] << 24U)
        | ((uint32_t)complete_chunk[complete_chunk_length - 3U] << 16U)
        | ((uint32_t)complete_chunk[complete_chunk_length - 2U] << 8U)
        | complete_chunk[complete_chunk_length - 1U];
    if (revlink_ap_jamcrc_finish_zeroed_trailer(&streaming_crc)
        != expected_streaming_crc) {
        fputs("incremental upload checksum failed\n", stderr);
        return 1;
    }

    static const uint8_t identity_text[] =
        "v1.7.4.2-23138\n"
        "AP3-SUB-004\n"
        "SUB0406661\n"
        "Installed\n"
        "2018 USDM WRX MT\n"
        "00000\n";
    uint8_t identity_payload[sizeof(identity_text) + 4U] = {0};
    const size_t identity_text_length = sizeof(identity_text) - 1U;
    identity_payload[0] = (uint8_t)identity_text_length;
    identity_payload[1] = (uint8_t)(identity_text_length >> 8U);
    identity_payload[2] = (uint8_t)(identity_text_length >> 16U);
    identity_payload[3] = (uint8_t)(identity_text_length >> 24U);
    memcpy(&identity_payload[4], identity_text, identity_text_length);
    revlink_ap_device_info_t device_info = {0};
    if (revlink_ap_parse_device_identity_payload(
            identity_payload,
            identity_text_length + 4U,
            &device_info
        ) != REVLINK_AP_OK
        || strcmp(device_info.firmware, "v1.7.4.2-23138") != 0
        || strcmp(device_info.part_number, "AP3-SUB-004") != 0
        || strcmp(device_info.serial, "SUB0406661") != 0
        || strcmp(device_info.install_status, "Installed") != 0
        || strcmp(device_info.vehicle, "2018 USDM WRX MT") != 0
        || strcmp(device_info.trailing_field, "00000") != 0) {
        fputs("identity payload parser failed\n", stderr);
        return 1;
    }
    identity_payload[4U + strlen("v1.7.4.2-23138\nAP3-SUB-004\nSUB")] = '/';
    if (revlink_ap_parse_device_identity_payload(
            identity_payload,
            identity_text_length + 4U,
            &device_info
        ) != REVLINK_AP_INVALID_FRAME) {
        fputs("invalid identity serial was accepted\n", stderr);
        return 1;
    }

    if (revlink_ap_build_list(
            (const uint8_t *)"maps",
            4U,
            record,
            8U,
            &length
        ) != REVLINK_AP_BUFFER_TOO_SMALL) {
        fputs("small output buffer was accepted\n", stderr);
        return 1;
    }

    static const uint8_t listing_payload[] = {
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x04, 0x00, 0x00, 0x00, 'm', 'a', 'p', 's',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01,
        0x04, 0x00, 0x00, 0x00, 'm', 'a', 'p', 's',
    };
    revlink_ap_listing_entry_t listing_entry = {0};
    size_t listing_count = 0;
    if (revlink_ap_parse_listing_payload(
            listing_payload,
            sizeof(listing_payload),
            &listing_entry,
            1U,
            &listing_count
        ) != REVLINK_AP_OK
        || listing_count != 1U
        || listing_entry.name_length != 4U
        || memcmp(listing_entry.name, "maps", 4U) != 0
        || !listing_entry.is_directory
        || listing_entry.path_length != 4U
        || memcmp(listing_entry.path, "maps", 4U) != 0) {
        fputs("listing parser failed\n", stderr);
        return 1;
    }

    static const uint8_t chunk_payload[] = {0x00, 0x01, 0x02, 0xff};
    uint8_t chunk[REVLINK_AP_CHUNK_PREFIX_SIZE + sizeof(chunk_payload)
                  + REVLINK_AP_CHECKSUM_SIZE] = {0};
    size_t chunk_length = 0;
    if (revlink_ap_build_chunk(
            chunk_payload,
            sizeof(chunk_payload),
            REVLINK_AP_UPLOAD_CHUNK_FLAG,
            chunk,
            sizeof(chunk),
            &chunk_length
        ) != REVLINK_AP_OK
        || chunk_length != sizeof(chunk)
        || revlink_ap_validate_chunk(chunk, chunk_length) != REVLINK_AP_OK) {
        fputs("upload chunk vector failed\n", stderr);
        return 1;
    }
    chunk[chunk_length - 1U] ^= 0xffU;
    if (revlink_ap_validate_chunk(chunk, chunk_length) != REVLINK_AP_INVALID_CHECKSUM) {
        fputs("corrupt chunk checksum was accepted\n", stderr);
        return 1;
    }

    static const uint8_t download_payload[] = "streamed-download";
    uint8_t response[128] = {0};
    size_t response_length = 0;
    static const uint8_t response_payload[] = {0x00, 0x01};
    if (revlink_ap_build_record(
            REVLINK_AP_OPCODE_RESPONSE,
            response_payload,
            sizeof(response_payload),
            response,
            sizeof(response),
            &response_length
        ) != REVLINK_AP_OK
        || revlink_ap_build_chunk(
            download_payload,
            sizeof(download_payload) - 1U,
            REVLINK_AP_DOWNLOAD_CHUNK_FLAG,
            &response[response_length],
            sizeof(response) - response_length,
            &chunk_length
        ) != REVLINK_AP_OK) {
        fputs("download decoder fixture build failed\n", stderr);
        return 1;
    }
    const size_t combined_length = response_length + chunk_length;
    revlink_ap_download_decoder_t decoder;
    revlink_ap_download_decoder_init(
        &decoder,
        sizeof(download_payload) - 1U
    );
    test_sink_t sink = {0};
    for (size_t offset = 0; offset < combined_length; offset += 3U) {
        const size_t remaining = combined_length - offset;
        const size_t count = remaining < 3U ? remaining : 3U;
        if (revlink_ap_download_decoder_feed(
                &decoder,
                &response[offset],
                count,
                test_payload_sink,
                &sink
            ) != REVLINK_AP_OK) {
            fputs("streamed download decoder rejected valid data\n", stderr);
            return 1;
        }
    }
    if (!revlink_ap_download_decoder_complete(&decoder)
        || sink.length != sizeof(download_payload) - 1U
        || memcmp(
            sink.bytes,
            download_payload,
            sizeof(download_payload) - 1U
        ) != 0) {
        fputs("streamed download decoder output failed\n", stderr);
        return 1;
    }

    memset(
        &response[combined_length - REVLINK_AP_CHECKSUM_SIZE],
        0,
        REVLINK_AP_CHECKSUM_SIZE
    );
    revlink_ap_download_decoder_init(
        &decoder,
        sizeof(download_payload) - 1U
    );
    memset(&sink, 0, sizeof(sink));
    if (revlink_ap_download_decoder_feed(
            &decoder,
            response,
            combined_length,
            test_payload_sink,
            &sink
        ) != REVLINK_AP_OK
        || !revlink_ap_download_decoder_complete(&decoder)
        || sink.length != sizeof(download_payload) - 1U) {
        fputs("device-observed zero download trailer was rejected\n", stderr);
        return 1;
    }

    response[combined_length - 1U] = 0x01U;
    revlink_ap_download_decoder_init(
        &decoder,
        sizeof(download_payload) - 1U
    );
    memset(&sink, 0, sizeof(sink));
    if (revlink_ap_download_decoder_feed(
            &decoder,
            response,
            combined_length,
            test_payload_sink,
            &sink
        ) != REVLINK_AP_INVALID_CHECKSUM) {
        fputs("corrupt download trailer was accepted\n", stderr);
        return 1;
    }

    revlink_ap_upload_kind_t kind = 0;
    static const uint8_t map_path[] = "maps/ZZ_TEST ROUNDTRIP v400.ptm";
    static const uint8_t startup_path[] = "images/startup_screen.fb";
    static const uint8_t csv_path[] = "datalog/datalog35-dup.csv";
    if (revlink_ap_validate_upload_target(
            map_path,
            sizeof(map_path) - 1U,
            41469U,
            &kind
        ) != REVLINK_AP_OK
        || kind != REVLINK_AP_UPLOAD_MAP
        || revlink_ap_validate_upload_target(
            startup_path,
            sizeof(startup_path) - 1U,
            REVLINK_AP_STARTUP_SCREEN_BYTES,
            &kind
        ) != REVLINK_AP_OK
        || kind != REVLINK_AP_UPLOAD_STARTUP_SCREEN
        || revlink_ap_validate_upload_target(
            csv_path,
            sizeof(csv_path) - 1U,
            46568U,
            &kind
        ) != REVLINK_AP_UPLOAD_TARGET_REJECTED
        || revlink_ap_validate_upload_target(
            startup_path,
            sizeof(startup_path) - 1U,
            REVLINK_AP_STARTUP_SCREEN_BYTES - 1U,
            &kind
        ) != REVLINK_AP_UPLOAD_TARGET_REJECTED) {
        fputs("upload target policy failed\n", stderr);
        return 1;
    }

    puts("protocol unit self-test PASSED");
    return 0;
}

static int output_result(
    revlink_ap_status_t status,
    const uint8_t *record,
    size_t length
)
{
    if (status != REVLINK_AP_OK) {
        fprintf(stderr, "%s\n", revlink_ap_status_name(status));
        return 1;
    }
    print_hex(record, length);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t record[RECORD_BUFFER_SIZE];
    size_t length = 0;

    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        return run_self_test();
    }
    if (argc == 3 && strcmp(argv[1], "validate") == 0) {
        if (!parse_hex(argv[2], record, sizeof(record), &length)) {
            fputs("invalid hex\n", stderr);
            return 2;
        }
        const revlink_ap_status_t status = revlink_ap_validate_record(record, length);
        if (status != REVLINK_AP_OK) {
            fprintf(stderr, "%s\n", revlink_ap_status_name(status));
            return 1;
        }
        puts("ok");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        const revlink_ap_status_t status = revlink_ap_build_list(
            (const uint8_t *)argv[2],
            strlen(argv[2]),
            record,
            sizeof(record),
            &length
        );
        return output_result(status, record, length);
    }
    if (argc == 3 && strcmp(argv[1], "select-map") == 0) {
        const revlink_ap_status_t status = revlink_ap_build_select_map(
            (const uint8_t *)argv[2],
            strlen(argv[2]),
            record,
            sizeof(record),
            &length
        );
        return output_result(status, record, length);
    }
    if (argc == 4 && strcmp(argv[1], "download") == 0) {
        const revlink_ap_status_t status = revlink_ap_build_download(
            (const uint8_t *)argv[2],
            strlen(argv[2]),
            (const uint8_t *)argv[3],
            strlen(argv[3]),
            record,
            sizeof(record),
            &length
        );
        return output_result(status, record, length);
    }
    if (argc == 3 && strcmp(argv[1], "temp-notice") == 0) {
        const revlink_ap_status_t status = revlink_ap_build_temp_notice(
            (const uint8_t *)argv[2],
            strlen(argv[2]),
            record,
            sizeof(record),
            &length
        );
        return output_result(status, record, length);
    }
    if (argc == 6 && strcmp(argv[1], "upload") == 0) {
        uint32_t modification_time = 0;
        uint32_t file_size = 0;
        if (!parse_u32(argv[4], &modification_time) || !parse_u32(argv[5], &file_size)) {
            fputs("invalid upload integer\n", stderr);
            return 2;
        }
        const revlink_ap_status_t status = revlink_ap_build_upload(
            (const uint8_t *)argv[2],
            strlen(argv[2]),
            (const uint8_t *)argv[3],
            strlen(argv[3]),
            modification_time,
            file_size,
            record,
            sizeof(record),
            &length
        );
        return output_result(status, record, length);
    }
    if (argc == 3 && strcmp(argv[1], "chunk") == 0) {
        static uint8_t payload[RECORD_BUFFER_SIZE];
        size_t payload_length = 0;
        if (!parse_hex(argv[2], payload, sizeof(payload), &payload_length)) {
            fputs("invalid payload hex\n", stderr);
            return 2;
        }
        const revlink_ap_status_t status = revlink_ap_build_chunk(
            payload,
            payload_length,
            REVLINK_AP_UPLOAD_CHUNK_FLAG,
            record,
            sizeof(record),
            &length
        );
        return output_result(status, record, length);
    }

    fprintf(
        stderr,
        "usage: %s self-test|validate HEX|list PATH|select-map NAME|"
        "download NAME PATH|temp-notice PATH|upload NAME PATH MTIME SIZE|"
        "chunk PAYLOAD_HEX\n",
        argv[0]
    );
    return 2;
}
