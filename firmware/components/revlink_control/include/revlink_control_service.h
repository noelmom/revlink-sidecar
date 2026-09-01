#ifndef REVLINK_CONTROL_SERVICE_H
#define REVLINK_CONTROL_SERVICE_H

#include <stdbool.h>

#include "revlink_device_service.h"
#include "revlink_sync_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_CONTROL_OK = 0,
    REVLINK_CONTROL_INVALID_ARGUMENT,
    REVLINK_CONTROL_INVALID_STATE,
    REVLINK_CONTROL_TRANSPORT_ERROR,
    REVLINK_CONTROL_NOT_SUPPORTED,
} revlink_control_status_t;

typedef enum {
    REVLINK_CONTROL_GET_STATUS = 0,
    REVLINK_CONTROL_SET_AUTO_SYNC,
    REVLINK_CONTROL_REQUEST_SYNC,
    REVLINK_CONTROL_CANCEL_SYNC,
} revlink_control_command_t;

typedef struct {
    revlink_device_snapshot_t device;
    revlink_sync_policy_t sync_policy;
    revlink_sync_snapshot_t sync;
    bool writes_compiled;
    bool deletes_compiled;
    bool shutdown_requested;
} revlink_control_snapshot_t;

typedef struct {
    revlink_control_command_t command;
    bool enabled;
} revlink_control_request_t;

typedef struct {
    revlink_control_status_t status;
    revlink_control_snapshot_t snapshot;
} revlink_control_response_t;

typedef revlink_control_status_t (*revlink_control_read_snapshot_t)(
    void *context,
    revlink_control_snapshot_t *snapshot
);

typedef revlink_control_status_t (*revlink_control_set_auto_sync_t)(
    void *context,
    bool enabled
);

typedef revlink_control_status_t (*revlink_control_action_t)(void *context);

typedef struct {
    void *context;
    revlink_control_read_snapshot_t read_snapshot;
    revlink_control_set_auto_sync_t set_auto_sync;
    revlink_control_action_t request_sync;
    revlink_control_action_t cancel_sync;
} revlink_control_service_config_t;

typedef struct {
    revlink_control_service_config_t config;
    bool initialized;
} revlink_control_service_t;

revlink_control_status_t revlink_control_service_init(
    revlink_control_service_t *service,
    const revlink_control_service_config_t *config
);

revlink_control_status_t revlink_control_service_execute(
    revlink_control_service_t *service,
    const revlink_control_request_t *request,
    revlink_control_response_t *response
);

const char *revlink_control_status_name(revlink_control_status_t status);

#ifdef __cplusplus
}
#endif

#endif
