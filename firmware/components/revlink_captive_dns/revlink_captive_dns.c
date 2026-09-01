#include "revlink_captive_dns.h"

#include <stdbool.h>
#include <string.h>

#define DNS_HEADER_SIZE 12U
#define DNS_TYPE_A 1U
#define DNS_CLASS_IN 1U
#define DNS_QUESTION_TRAILER_SIZE 4U
#define DNS_A_ANSWER_SIZE 16U

static uint16_t read_u16(const uint8_t *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8U) | value[1]);
}

static void write_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8U);
    target[1] = (uint8_t)value;
}

static size_t question_end(const uint8_t *query, size_t query_size)
{
    size_t cursor = DNS_HEADER_SIZE;
    while (cursor < query_size) {
        const uint8_t label_size = query[cursor++];
        if (label_size == 0U) {
            return cursor + DNS_QUESTION_TRAILER_SIZE <= query_size
                ? cursor + DNS_QUESTION_TRAILER_SIZE
                : 0U;
        }
        if (
            (label_size & 0xc0U) != 0U
            || label_size > 63U
            || cursor + label_size > query_size
        ) {
            return 0U;
        }
        cursor += label_size;
    }
    return 0U;
}

revlink_captive_dns_status_t revlink_captive_dns_build_response(
    const uint8_t *query,
    size_t query_size,
    uint32_t ipv4_network_order,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_size
)
{
    if (
        query == NULL
        || response == NULL
        || response_size == NULL
    ) {
        return REVLINK_CAPTIVE_DNS_INVALID_ARGUMENT;
    }
    *response_size = 0U;
    if (query_size < DNS_HEADER_SIZE) {
        return REVLINK_CAPTIVE_DNS_MALFORMED;
    }

    const uint16_t request_flags = read_u16(&query[2]);
    const uint16_t question_count = read_u16(&query[4]);
    if (
        (request_flags & 0x8000U) != 0U
        || (request_flags & 0x7800U) != 0U
        || question_count != 1U
    ) {
        return REVLINK_CAPTIVE_DNS_IGNORED;
    }

    const size_t end = question_end(query, query_size);
    if (end == 0U) {
        return REVLINK_CAPTIVE_DNS_MALFORMED;
    }
    const uint16_t question_type = read_u16(&query[end - 4U]);
    const uint16_t question_class = read_u16(&query[end - 2U]);
    const bool answer_a =
        question_type == DNS_TYPE_A && question_class == DNS_CLASS_IN;
    const size_t required = end + (answer_a ? DNS_A_ANSWER_SIZE : 0U);
    if (response_capacity < required) {
        return REVLINK_CAPTIVE_DNS_NO_SPACE;
    }

    memcpy(response, query, end);
    const uint16_t response_flags =
        (uint16_t)(0x8400U | (request_flags & 0x0100U));
    write_u16(&response[2], response_flags);
    write_u16(&response[4], 1U);
    write_u16(&response[6], answer_a ? 1U : 0U);
    write_u16(&response[8], 0U);
    write_u16(&response[10], 0U);

    if (answer_a) {
        uint8_t *answer = &response[end];
        answer[0] = 0xc0U;
        answer[1] = 0x0cU;
        write_u16(&answer[2], DNS_TYPE_A);
        write_u16(&answer[4], DNS_CLASS_IN);
        memset(&answer[6], 0, 4U);
        write_u16(&answer[10], 4U);
        memcpy(&answer[12], &ipv4_network_order, 4U);
    }

    *response_size = required;
    return REVLINK_CAPTIVE_DNS_OK;
}
