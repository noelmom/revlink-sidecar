#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "revlink_network_coordinator.h"
#include "revlink_wifi_radio.h"

#define REVLINK_NETWORK_SSID_CAPACITY 33U

typedef struct {
    revlink_network_snapshot_t coordinator;
    revlink_wifi_radio_snapshot_t radio;
    char connected_ssid[REVLINK_NETWORK_SSID_CAPACITY];
    bool station_configured;
    bool station_credentials_persistent;
    bool hotspot_configured;
    bool awaiting_hotspot_credential;
} revlink_network_runtime_snapshot_t;

/*
 * Starts the product network coordinator and the P4-to-C6 radio adapter.
 * A valid saved station credential is loaded before the initial scan.
 */
esp_err_t revlink_network_runtime_start(void);

/*
 * Replaces the preferred station candidate and immediately retries it. A
 * successful association is saved atomically; a failed candidate never
 * overwrites the last working saved credential. Supplied buffers are copied
 * and never logged.
 */
esp_err_t revlink_network_runtime_configure_station(
    const char *ssid,
    const char *password
);

/* Returns the result of the most recent station association attempt. */
esp_err_t revlink_network_runtime_last_station_error(void);

/* Erases the preferred station and returns the network policy to fallback. */
esp_err_t revlink_network_runtime_forget_station(void);

/*
 * Supplies the RAM-only WPA2 fallback-hotspot credential. If fallback is
 * already pending, the hotspot starts immediately.
 */
esp_err_t revlink_network_runtime_configure_hotspot_ephemeral(
    const char *password
);

esp_err_t revlink_network_runtime_force_hotspot(void);
esp_err_t revlink_network_runtime_set_transfer_active(bool active);

revlink_network_runtime_snapshot_t revlink_network_runtime_snapshot(void);
