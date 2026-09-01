#ifndef REVLINK_WIFI_RADIO_H
#define REVLINK_WIFI_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_WIFI_RADIO_DISABLED = 0,
    REVLINK_WIFI_RADIO_STARTING,
    REVLINK_WIFI_RADIO_SCANNING,
    REVLINK_WIFI_RADIO_READY,
    REVLINK_WIFI_RADIO_CONNECTING,
    REVLINK_WIFI_RADIO_CONNECTED,
    REVLINK_WIFI_RADIO_HOTSPOT_STARTING,
    REVLINK_WIFI_RADIO_HOTSPOT_READY,
    REVLINK_WIFI_RADIO_FAILED,
} revlink_wifi_radio_state_t;

typedef struct {
    revlink_wifi_radio_state_t state;
    uint16_t access_point_count;
    int8_t strongest_rssi;
    uint8_t strongest_channel;
    uint8_t hotspot_client_count;
    esp_err_t last_error;
} revlink_wifi_radio_snapshot_t;

#define REVLINK_WIFI_VISIBLE_NETWORK_LIMIT 12U

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;
} revlink_wifi_visible_network_t;

/*
 * Starts the P4-to-C6 hosted Wi-Fi transport and one bounded scan.
 *
 * This acceptance adapter does not accept credentials, connect to a network,
 * persist Wi-Fi configuration, expose a server, or interact with AccessPort
 * USB. Network identities are intentionally excluded from its snapshot/logs.
 */
esp_err_t revlink_wifi_radio_start(void);

revlink_wifi_radio_snapshot_t revlink_wifi_radio_snapshot(void);

/*
 * Starts one bounded anonymous scan. It leaves an active hotspot, if any, and
 * returns the radio to station mode before scanning. Network identities are
 * intentionally excluded from the public snapshot and logs.
 */
esp_err_t revlink_wifi_radio_scan_anonymous(void);

/*
 * Performs the same bounded scan synchronously and caches only printable,
 * visible SSIDs for local onboarding. Identities are never logged.
 */
esp_err_t revlink_wifi_radio_scan_visible(void);

size_t revlink_wifi_radio_visible_networks(
    revlink_wifi_visible_network_t *networks,
    size_t capacity
);

/*
 * Performs one bounded station join with credentials supplied by a trusted
 * local caller. Wi-Fi storage remains RAM-only. The implementation never logs
 * the SSID or password and clears its temporary configuration before return.
 */
esp_err_t revlink_wifi_radio_connect_ephemeral(
    const char *ssid,
    const char *password,
    uint32_t timeout_ms
);

/*
 * Starts a WPA2 fallback hotspot with an SSID generated from the last eight
 * factory-MAC hex digits: RevLink-XXXXXXXX. A bounded numeric suffix is
 * selected if startup scanning already found that SSID nearby. The password
 * is supplied by a trusted caller, kept only in RAM, never logged, and must
 * contain 8-63 printable ASCII characters.
 */
esp_err_t revlink_wifi_radio_start_hotspot_ephemeral(
    const char *password,
    uint32_t timeout_ms
);

/*
 * Returns the generated local identity without exposing credentials.
 * The hostname is lowercase and excludes ".local".
 */
esp_err_t revlink_wifi_radio_local_identity(
    char *ssid,
    size_t ssid_capacity,
    char *hostname,
    size_t hostname_capacity
);

/*
 * Supplies the persisted collision suffix before the local identity is first
 * resolved. Zero selects the unsuffixed base; values 2-99 select "-N".
 */
esp_err_t revlink_wifi_radio_set_local_collision_index(uint16_t index);

/*
 * Returns the active hotspot IPv4 address in network byte order.
 */
esp_err_t revlink_wifi_radio_hotspot_ipv4(uint32_t *address);

/*
 * Ends the temporary AP+station onboarding overlap after the browser has
 * received a confirmed result. The station association remains active.
 */
esp_err_t revlink_wifi_radio_finish_onboarding_transition(void);

/*
 * Stops station/hotspot operation while leaving the hosted radio initialized.
 */
esp_err_t revlink_wifi_radio_stop(void);

/*
 * Resolves one fixed public hostname through the configured DNS server.
 * Success demonstrates bidirectional IP traffic without logging addresses.
 */
esp_err_t revlink_wifi_radio_probe_dns(void);

/*
 * Sends one bounded ICMP echo to the DHCP gateway. Unlike the public DNS
 * acceptance probe, this proves the local station data path without requiring
 * internet access.
 */
esp_err_t revlink_wifi_radio_probe_gateway(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
