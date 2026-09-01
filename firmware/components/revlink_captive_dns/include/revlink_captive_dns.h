#ifndef REVLINK_CAPTIVE_DNS_H
#define REVLINK_CAPTIVE_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_CAPTIVE_DNS_OK = 0,
    REVLINK_CAPTIVE_DNS_IGNORED,
    REVLINK_CAPTIVE_DNS_INVALID_ARGUMENT,
    REVLINK_CAPTIVE_DNS_MALFORMED,
    REVLINK_CAPTIVE_DNS_NO_SPACE,
} revlink_captive_dns_status_t;

/*
 * Builds a bounded authoritative response for one ordinary DNS question.
 * IPv4 addresses are supplied in network byte order. A/IN questions receive
 * one zero-TTL answer; unsupported question types receive a valid empty
 * response so clients can continue with IPv4.
 */
revlink_captive_dns_status_t revlink_captive_dns_build_response(
    const uint8_t *query,
    size_t query_size,
    uint32_t ipv4_network_order,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_size
);

#ifdef __cplusplus
}
#endif

#endif
