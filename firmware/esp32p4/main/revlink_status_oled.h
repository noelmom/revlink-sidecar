#ifndef REVLINK_STATUS_OLED_H
#define REVLINK_STATUS_OLED_H

#include <stdint.h>

#include "esp_err.h"
#include "revlink_device_service.h"
#include "revlink_network_coordinator.h"
#include "revlink_sync_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_OLED_STORAGE_NORMAL = 0,
    REVLINK_OLED_STORAGE_MISSING,
    REVLINK_OLED_STORAGE_UNREADABLE,
    REVLINK_OLED_STORAGE_ERROR,
    REVLINK_OLED_STORAGE_CONFIRM_FORMAT,
    REVLINK_OLED_STORAGE_FORMATTING,
    REVLINK_OLED_STORAGE_FORMAT_COMPLETE,
    REVLINK_OLED_STORAGE_FORMAT_FAILED,
} revlink_oled_storage_state_t;

esp_err_t revlink_status_oled_start(void);

void revlink_status_oled_boot_complete(void);

void revlink_status_oled_update_device(
    const revlink_device_snapshot_t *snapshot
);

void revlink_status_oled_update_sync(
    const revlink_sync_snapshot_t *snapshot
);

void revlink_status_oled_update_network(
    const revlink_network_snapshot_t *snapshot,
    const char *connected_ssid
);

void revlink_status_oled_set_vehicle(const char *vehicle);

void revlink_status_oled_set_accessport_identity(
    const char *vehicle,
    const char *part_number
);

/*
 * Shows a physical, out-of-band setup credential. The password is copied only
 * into volatile display state. Hiding preserves that RAM-only copy so a
 * network fallback can restore the same credential without regenerating it.
 */
void revlink_status_oled_show_hotspot(
    const char *ssid,
    const char *password
);

void revlink_status_oled_hide_hotspot(void);

void revlink_status_oled_restore_hotspot(void);

void revlink_status_oled_clear_hotspot(void);

/*
 * Builds a standard Wi-Fi QR from the volatile hotspot credential and shows
 * it for a bounded interval. The credential is never persisted or logged.
 */
esp_err_t revlink_status_oled_show_hotspot_qr(void);

void revlink_status_oled_hide_hotspot_qr(void);

bool revlink_status_oled_hotspot_qr_visible(void);

/*
 * Shows the active mDNS hostname for a bounded interval while the Sidecar is
 * connected to a preferred Wi-Fi network. The hostname excludes ".local".
 */
esp_err_t revlink_status_oled_show_local_url(const char *hostname);

void revlink_status_oled_hide_local_url(void);

bool revlink_status_oled_local_url_visible(void);

void revlink_status_oled_show_storage_error(
    revlink_oled_storage_state_t state
);

void revlink_status_oled_show_storage_format_warning(
    uint32_t seconds_remaining
);

void revlink_status_oled_show_storage_formatting(void);

void revlink_status_oled_show_storage_format_complete(void);

void revlink_status_oled_show_storage_format_failed(void);

#ifdef __cplusplus
}
#endif

#endif
