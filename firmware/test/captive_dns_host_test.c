#include "revlink_captive_dns.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "captive DNS host test failed: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static size_t build_query(uint8_t *query, uint16_t type)
{
    const uint8_t prefix[] = {
        0x12, 0x34, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm', 0x00,
    };
    memcpy(query, prefix, sizeof(prefix));
    query[sizeof(prefix)] = (uint8_t)(type >> 8U);
    query[sizeof(prefix) + 1U] = (uint8_t)type;
    query[sizeof(prefix) + 2U] = 0x00;
    query[sizeof(prefix) + 3U] = 0x01;
    return sizeof(prefix) + 4U;
}

int main(void)
{
    uint8_t query[128] = {0};
    uint8_t response[128] = {0};
    size_t response_size = 0U;
    const size_t query_size = build_query(query, 1U);
    const uint8_t address[] = {192U, 168U, 4U, 1U};
    uint32_t address_network_order = 0U;
    memcpy(&address_network_order, address, sizeof(address));

    require(
        revlink_captive_dns_build_response(
            query,
            query_size,
            address_network_order,
            response,
            sizeof(response),
            &response_size
        ) == REVLINK_CAPTIVE_DNS_OK,
        "A query should produce a response"
    );
    require(response_size == query_size + 16U, "A answer size");
    require(response[0] == 0x12 && response[1] == 0x34, "transaction ID");
    require(response[2] == 0x85 && response[3] == 0x00, "response flags");
    require(response[6] == 0x00 && response[7] == 0x01, "answer count");
    require(
        memcmp(&response[response_size - 4U], address, sizeof(address)) == 0,
        "captive address"
    );

    const size_t aaaa_size = build_query(query, 28U);
    require(
        revlink_captive_dns_build_response(
            query,
            aaaa_size,
            address_network_order,
            response,
            sizeof(response),
            &response_size
        ) == REVLINK_CAPTIVE_DNS_OK,
        "AAAA query should receive an empty response"
    );
    require(response_size == aaaa_size, "empty response size");
    require(response[6] == 0x00 && response[7] == 0x00, "empty answer count");

    query[12] = 0xc0U;
    require(
        revlink_captive_dns_build_response(
            query,
            aaaa_size,
            address_network_order,
            response,
            sizeof(response),
            &response_size
        ) == REVLINK_CAPTIVE_DNS_MALFORMED,
        "compressed question names are rejected"
    );
    require(
        revlink_captive_dns_build_response(
            query,
            4U,
            address_network_order,
            response,
            sizeof(response),
            &response_size
        ) == REVLINK_CAPTIVE_DNS_MALFORMED,
        "short packet"
    );

    puts("revlink captive DNS host tests passed");
    return EXIT_SUCCESS;
}
