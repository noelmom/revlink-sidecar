#ifndef REVLINK_APPLICATION_H
#define REVLINK_APPLICATION_H

#include <stdbool.h>

#include "revlink_device_service.h"
#include "revlink_safety_policy.h"
#include "revlink_sync_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool allow_device_writes;
    bool allow_device_deletes;
    revlink_device_state_observer_t state_observer;
    void *state_observer_context;
    revlink_sync_policy_t sync_policy;
    revlink_sync_transport_request_t sync_request;
    revlink_sync_transport_request_t sync_recover_session;
    revlink_sync_transport_cancel_t sync_cancel;
    void *sync_transport_context;
    revlink_sync_observer_t sync_observer;
    void *sync_observer_context;
    bool retry_unclean_readonly_close_once;
} revlink_application_config_t;

typedef struct {
    revlink_device_service_t device_service;
    revlink_safety_policy_t safety_policy;
    revlink_sync_coordinator_t sync_coordinator;
    revlink_device_state_observer_t state_observer;
    void *state_observer_context;
    revlink_device_state_t previous_device_state;
    bool auto_sync_attempted;
    bool preserve_attachment_across_reenumeration;
    bool retry_unclean_readonly_close_once;
    bool unclean_close_recovery_attempted;
    bool auto_sync_disarmed_by_conflict;
    bool auto_sync_active;
    bool auto_sync_retry_eligible;
    uint8_t auto_sync_retry_count;
} revlink_application_t;

revlink_core_status_t revlink_application_init(
    revlink_application_t *application,
    const revlink_application_config_t *config
);

bool revlink_application_protocol_self_test(void);

revlink_core_status_t revlink_application_handle_device_event(
    revlink_application_t *application,
    const revlink_device_event_t *event
);

revlink_core_status_t revlink_application_authorize(
    const revlink_application_t *application,
    revlink_operation_t operation
);

revlink_sync_coordinator_status_t revlink_application_set_sync_policy(
    revlink_application_t *application,
    const revlink_sync_policy_t *policy
);

revlink_sync_policy_t revlink_application_sync_policy(
    const revlink_application_t *application
);

revlink_sync_coordinator_status_t revlink_application_request_sync(
    revlink_application_t *application
);

/*
 * Retry one transient attach-time automatic-sync failure. The application
 * owns the retry budget and revalidates device/coordinator state; the
 * platform owns the delay so this core remains scheduler independent.
 */
revlink_sync_coordinator_status_t revlink_application_retry_auto_sync(
    revlink_application_t *application
);

bool revlink_application_auto_sync_retry_needed(
    const revlink_application_t *application
);

revlink_sync_coordinator_status_t revlink_application_cancel_sync(
    revlink_application_t *application
);

revlink_sync_coordinator_status_t revlink_application_handle_sync_event(
    revlink_application_t *application,
    const revlink_sync_event_t *event
);

revlink_sync_snapshot_t revlink_application_sync_snapshot(
    const revlink_application_t *application
);

#ifdef __cplusplus
}
#endif

#endif
