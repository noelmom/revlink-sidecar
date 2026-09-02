#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "revlink_accessport_protocol.h"
#include "revlink_control_service.h"
#include "revlink_sync_coordinator.h"

revlink_control_status_t revlink_runtime_control_execute(
    const revlink_control_request_t *request,
    revlink_control_response_t *response
);

revlink_sync_coordinator_status_t revlink_runtime_set_auto_sync(bool enabled);
esp_err_t revlink_runtime_set_write_consent(bool enabled);
esp_err_t revlink_runtime_set_map_auto_apply(bool enabled);
esp_err_t revlink_runtime_set_delete_consent(bool enabled);
revlink_sync_coordinator_status_t revlink_runtime_request_sync(void);
revlink_sync_coordinator_status_t revlink_runtime_cancel_sync(void);
revlink_sync_coordinator_status_t revlink_runtime_prepare_shutdown(void);
revlink_sync_policy_t revlink_runtime_sync_policy(void);
revlink_sync_snapshot_t revlink_runtime_sync_snapshot(void);

esp_err_t revlink_runtime_connected_accessport_snapshot(
    bool *known,
    revlink_ap_device_info_t *identity
);
