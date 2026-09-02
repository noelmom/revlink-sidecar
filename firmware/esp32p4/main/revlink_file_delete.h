#ifndef REVLINK_FILE_DELETE_H
#define REVLINK_FILE_DELETE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "revlink_accessport_protocol.h"
#include "revlink_accessport_usb.h"

/*
 * Guarded AccessPort file deletion, kept apart from the map-write service on
 * purpose.
 *
 * Consent to delete is not consent to write and is not implied by it. Someone
 * who agreed to copy maps onto a device has not thereby agreed to let this
 * remove files from it, and deletion has no undo: the AccessPort keeps no
 * recycle bin, and RevLink holds nothing back beyond whatever the Sidecar had
 * already synchronised. Sharing a consent flag between the two would quietly
 * convert one decision into the other.
 */

typedef struct {
    bool deletes_compiled;
    bool consent_enabled;
    bool recovery_required;
    revlink_accessport_delete_state_t state;
    esp_err_t platform_error;
    char path[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
} revlink_file_delete_snapshot_t;

/* Starts locked. The runtime may restore persisted consent afterwards. */
esp_err_t revlink_file_delete_start(void);

esp_err_t revlink_file_delete_set_consent(bool enabled);

/*
 * Delete one file from the attached AccessPort. `path` must name a file
 * directly inside maps/ or datalog/; the transport re-validates it, confirms
 * the file is present, and confirms it is gone afterwards.
 *
 * `forget_cached_copy` additionally drops the Sidecar's own cached copy —
 * but only once the device has confirmed the removal. The order matters: if
 * the cache went first and the device delete then failed, the owner would
 * have lost their only spare copy of a file that is still on the AccessPort.
 * Removing the cache alone does not go through here at all; it never touches
 * a device, so it does not need one attached. See revlink_sd_forget_cached().
 */
esp_err_t revlink_file_delete_request(
    const revlink_ap_device_info_t *identity,
    const char *path,
    bool forget_cached_copy
);

esp_err_t revlink_file_delete_snapshot(
    revlink_file_delete_snapshot_t *snapshot
);

#endif
