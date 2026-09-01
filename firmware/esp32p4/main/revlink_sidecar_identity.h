#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "revlink_identity.h"

typedef struct {
    char device_id[REVLINK_IDENTITY_DEVICE_ID_CAPACITY];
    char hardware_mac[REVLINK_IDENTITY_MAC_TEXT_CAPACITY];
    char ssid[REVLINK_IDENTITY_SSID_CAPACITY];
    char hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY];
    uint16_t collision_index;
} revlink_sidecar_identity_t;

/*
 * Initializes the product identity after the Wi-Fi transport and NVS are
 * available. The internal ID is generated once and persisted; it is not a
 * credential and must never be used as one.
 */
esp_err_t revlink_sidecar_identity_init(void);

/*
 * Returns a copy of the identity. Once mDNS is active, hostname reflects the
 * responder's actual collision-resolved name.
 */
esp_err_t revlink_sidecar_identity_snapshot(
    revlink_sidecar_identity_t *identity
);
