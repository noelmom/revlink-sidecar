#ifndef REVLINK_MAP_UPLOAD_H
#define REVLINK_MAP_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "revlink_accessport_protocol.h"
#include "revlink_accessport_usb.h"

typedef struct {
    bool writes_compiled;
    bool consent_enabled;
    bool staged;
    bool recovery_required;
    revlink_ap_upload_kind_t kind;
    revlink_accessport_upload_state_t state;
    esp_err_t platform_error;
    char name[REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY];
    char destination[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    uint32_t size;
    uint8_t sha256[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES];
} revlink_map_upload_snapshot_t;

/*
 * Starts the isolated, microSD-backed write service in its locked state. The
 * runtime owner-settings adapter may restore persisted consent after startup.
 */
esp_err_t revlink_map_upload_start(void);

esp_err_t revlink_map_upload_source(
    revlink_accessport_upload_source_t *source
);

esp_err_t revlink_map_upload_set_consent(bool enabled);

esp_err_t revlink_map_upload_stage_begin(
    const char *name,
    const char *destination,
    uint32_t size
);

esp_err_t revlink_map_upload_stage_write(
    const uint8_t *data,
    size_t length
);

esp_err_t revlink_map_upload_stage_commit(void);

void revlink_map_upload_stage_abort(void);

esp_err_t revlink_map_upload_request(
    const revlink_ap_device_info_t *identity
);

esp_err_t revlink_map_upload_snapshot(
    revlink_map_upload_snapshot_t *snapshot
);

#endif
