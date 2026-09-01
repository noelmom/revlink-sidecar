#ifndef REVLINK_IDENTITY_H
#define REVLINK_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_IDENTITY_MAC_BYTES 6U
#define REVLINK_IDENTITY_RANDOM_BYTES 16U
#define REVLINK_IDENTITY_DEVICE_ID_CAPACITY 36U
#define REVLINK_IDENTITY_MAC_TEXT_CAPACITY 18U
#define REVLINK_IDENTITY_SSID_CAPACITY 33U
#define REVLINK_IDENTITY_HOSTNAME_CAPACITY 64U

/*
 * Formats a Sidecar-local identity from the hardware MAC.
 *
 * collision_index == 0 produces the base names:
 *   RevLink-A1B2C3D4
 *   revlink-a1b2c3d4
 *
 * collision_index >= 2 appends the same deterministic suffix to both names.
 * Index 1 is intentionally rejected so the visible suffix never implies that
 * an unsuffixed "-1" device exists.
 */
bool revlink_identity_format_local(
    const uint8_t hardware_mac[REVLINK_IDENTITY_MAC_BYTES],
    uint16_t collision_index,
    char *ssid,
    size_t ssid_capacity,
    char *hostname,
    size_t hostname_capacity,
    char *mac_text,
    size_t mac_text_capacity
);

/* Formats a stable, non-secret identifier as r1_<32 lowercase hex digits>. */
bool revlink_identity_format_device_id(
    const uint8_t random_bytes[REVLINK_IDENTITY_RANDOM_BYTES],
    char *device_id,
    size_t capacity
);

bool revlink_identity_device_id_valid(const char *device_id);

#ifdef __cplusplus
}
#endif

#endif
