#ifndef REVLINK_SYNC_COORDINATOR_H
#define REVLINK_SYNC_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_SYNC_OK = 0,
    REVLINK_SYNC_INVALID_ARGUMENT,
    REVLINK_SYNC_INVALID_STATE,
    REVLINK_SYNC_TRANSPORT_ERROR,
} revlink_sync_status_t;

typedef enum {
    REVLINK_SYNC_IDLE = 0,
    REVLINK_SYNC_QUEUED,
    REVLINK_SYNC_RUNNING,
    REVLINK_SYNC_CANCELLING,
    REVLINK_SYNC_COMPLETED,
    REVLINK_SYNC_FAILED,
    REVLINK_SYNC_CANCELLED,
} revlink_sync_state_t;

typedef enum {
    REVLINK_SYNC_EVENT_STARTED = 0,
    REVLINK_SYNC_EVENT_PROGRESS,
    REVLINK_SYNC_EVENT_COMPLETED,
    REVLINK_SYNC_EVENT_FAILED,
    REVLINK_SYNC_EVENT_CANCELLED,
} revlink_sync_event_kind_t;

typedef struct {
    bool auto_sync_on_attach;
} revlink_sync_policy_t;

typedef struct {
    revlink_sync_event_kind_t kind;
    size_t candidates;
    size_t downloaded;
    size_t skipped;
    uint32_t downloaded_bytes;
    size_t pending;
    bool close_recovery_attempt;
    bool data_phase_completed;
    bool session_close_sent;
    bool session_close_acknowledged;
    int platform_error;
} revlink_sync_event_t;

typedef struct {
    revlink_sync_state_t state;
    size_t candidates;
    size_t downloaded;
    size_t skipped;
    uint32_t downloaded_bytes;
    size_t pending;
    bool close_recovery_attempt;
    bool data_phase_completed;
    bool session_close_sent;
    bool session_close_acknowledged;
    int last_platform_error;
} revlink_sync_snapshot_t;

typedef revlink_sync_status_t (*revlink_sync_transport_request_t)(
    void *context
);

typedef revlink_sync_status_t (*revlink_sync_transport_cancel_t)(
    void *context
);

typedef void (*revlink_sync_observer_t)(
    void *context,
    const revlink_sync_snapshot_t *snapshot
);

typedef struct {
    void *transport_context;
    revlink_sync_transport_request_t request;
    revlink_sync_transport_request_t recover_session;
    revlink_sync_transport_cancel_t cancel;
    void *observer_context;
    revlink_sync_observer_t observer;
} revlink_sync_coordinator_config_t;

typedef struct {
    revlink_sync_policy_t policy;
    revlink_sync_snapshot_t snapshot;
    revlink_sync_coordinator_config_t config;
} revlink_sync_coordinator_t;

revlink_sync_status_t revlink_sync_coordinator_init(
    revlink_sync_coordinator_t *coordinator,
    const revlink_sync_coordinator_config_t *config,
    const revlink_sync_policy_t *policy
);

revlink_sync_status_t revlink_sync_coordinator_set_policy(
    revlink_sync_coordinator_t *coordinator,
    const revlink_sync_policy_t *policy
);

revlink_sync_policy_t revlink_sync_coordinator_policy(
    const revlink_sync_coordinator_t *coordinator
);

revlink_sync_status_t revlink_sync_coordinator_request(
    revlink_sync_coordinator_t *coordinator
);

revlink_sync_status_t revlink_sync_coordinator_recover_session(
    revlink_sync_coordinator_t *coordinator
);

revlink_sync_status_t revlink_sync_coordinator_cancel(
    revlink_sync_coordinator_t *coordinator
);

revlink_sync_status_t revlink_sync_coordinator_handle_event(
    revlink_sync_coordinator_t *coordinator,
    const revlink_sync_event_t *event
);

revlink_sync_snapshot_t revlink_sync_coordinator_snapshot(
    const revlink_sync_coordinator_t *coordinator
);

const char *revlink_sync_state_name(revlink_sync_state_t state);

#ifdef __cplusplus
}
#endif

#endif
