#ifndef REVLINK_MAP_UPLOAD_H
#define REVLINK_MAP_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "revlink_accessport_protocol.h"
#include "revlink_accessport_usb.h"
#include "revlink_staged_map.h"

typedef struct {
    bool writes_compiled;
    bool consent_enabled;
    bool auto_apply_enabled;
    bool staged;
    bool recovery_required;
    revlink_ap_upload_kind_t kind;
    revlink_accessport_upload_state_t state;
    esp_err_t platform_error;
    char name[REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY];
    char destination[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    /*
     * The AccessPort this staged payload belongs to. Empty until a target is
     * pinned; an unpinned payload can still be applied manually from the
     * portal with the device in front of you, but never automatically.
     */
    char target_part_number[REVLINK_AP_PART_NUMBER_CAPACITY];
    char target_serial[REVLINK_AP_SERIAL_CAPACITY];
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

/*
 * Owner preference for writing a staged map during the next attach-time sync.
 * Independent of write consent: enabling auto-apply does nothing unless
 * consent is also enabled, and disabling it never blocks a manual apply.
 */
esp_err_t revlink_map_upload_set_auto_apply(bool enabled);

esp_err_t revlink_map_upload_stage_begin(
    const char *name,
    const char *destination,
    uint32_t size
);

/*
 * Pin the payload being staged to one AccessPort. Call between stage_begin
 * and stage_commit. The pin is persisted with the payload and re-checked
 * immediately before any automatic write.
 */
esp_err_t revlink_map_upload_stage_set_target(
    const char *part_number,
    const char *serial
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

/*
 * Clears the one-automatic-attempt-per-attach latch. Call when an AccessPort
 * is newly attached, not when a sync completes.
 */
void revlink_map_upload_notify_attach(void);

/*
 * Evaluate every gate and, only if all of them pass, start writing the staged
 * payload to the attached AccessPort. Intended to be called once a clean
 * attach-time sync has completed.
 *
 * `decision` always receives the reason, including on refusal, so the caller
 * can log or surface it. Returns ESP_OK only when a write was actually
 * started.
 */
esp_err_t revlink_map_upload_auto_apply(
    const revlink_ap_device_info_t *identity,
    bool sync_completed_clean,
    size_t sync_pending,
    revlink_staged_map_apply_decision_t *decision
);

esp_err_t revlink_map_upload_snapshot(
    revlink_map_upload_snapshot_t *snapshot
);

#endif
