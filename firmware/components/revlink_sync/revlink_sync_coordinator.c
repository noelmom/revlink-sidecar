#include "revlink_sync_coordinator.h"

#include <string.h>

static void publish(revlink_sync_coordinator_t *coordinator)
{
    if (coordinator->config.observer != NULL) {
        coordinator->config.observer(
            coordinator->config.observer_context,
            &coordinator->snapshot
        );
    }
}

static bool is_active(revlink_sync_state_t state)
{
    return state == REVLINK_SYNC_QUEUED
        || state == REVLINK_SYNC_RUNNING
        || state == REVLINK_SYNC_CANCELLING;
}

revlink_sync_coordinator_status_t revlink_sync_coordinator_init(
    revlink_sync_coordinator_t *coordinator,
    const revlink_sync_coordinator_config_t *config,
    const revlink_sync_policy_t *policy
)
{
    if (coordinator == NULL || config == NULL || policy == NULL
        || config->request == NULL || config->cancel == NULL) {
        return REVLINK_SYNC_COORDINATOR_INVALID_ARGUMENT;
    }

    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->config = *config;
    coordinator->policy = *policy;
    coordinator->snapshot.state = REVLINK_SYNC_IDLE;
    return REVLINK_SYNC_COORDINATOR_OK;
}

revlink_sync_coordinator_status_t revlink_sync_coordinator_set_policy(
    revlink_sync_coordinator_t *coordinator,
    const revlink_sync_policy_t *policy
)
{
    if (coordinator == NULL || policy == NULL) {
        return REVLINK_SYNC_COORDINATOR_INVALID_ARGUMENT;
    }
    coordinator->policy = *policy;
    return REVLINK_SYNC_COORDINATOR_OK;
}

revlink_sync_policy_t revlink_sync_coordinator_policy(
    const revlink_sync_coordinator_t *coordinator
)
{
    revlink_sync_policy_t policy = {0};
    if (coordinator != NULL) {
        policy = coordinator->policy;
    }
    return policy;
}

static revlink_sync_coordinator_status_t request_operation(
    revlink_sync_coordinator_t *coordinator,
    revlink_sync_transport_request_t request,
    bool close_recovery_attempt
)
{
    if (coordinator == NULL) {
        return REVLINK_SYNC_COORDINATOR_INVALID_ARGUMENT;
    }
    if (is_active(coordinator->snapshot.state)) {
        return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
    }
    if (request == NULL) {
        return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
    }
    const revlink_sync_coordinator_status_t status =
        request(coordinator->config.transport_context);
    if (status != REVLINK_SYNC_COORDINATOR_OK) {
        return status;
    }

    memset(&coordinator->snapshot, 0, sizeof(coordinator->snapshot));
    coordinator->snapshot.state = REVLINK_SYNC_QUEUED;
    coordinator->snapshot.close_recovery_attempt = close_recovery_attempt;
    publish(coordinator);
    return REVLINK_SYNC_COORDINATOR_OK;
}

revlink_sync_coordinator_status_t revlink_sync_coordinator_request(
    revlink_sync_coordinator_t *coordinator
)
{
    return request_operation(
        coordinator,
        coordinator != NULL ? coordinator->config.request : NULL,
        false
    );
}

revlink_sync_coordinator_status_t revlink_sync_coordinator_recover_session(
    revlink_sync_coordinator_t *coordinator
)
{
    return request_operation(
        coordinator,
        coordinator != NULL ? coordinator->config.recover_session : NULL,
        true
    );
}

revlink_sync_coordinator_status_t revlink_sync_coordinator_cancel(
    revlink_sync_coordinator_t *coordinator
)
{
    if (coordinator == NULL) {
        return REVLINK_SYNC_COORDINATOR_INVALID_ARGUMENT;
    }
    if (coordinator->snapshot.state != REVLINK_SYNC_QUEUED
        && coordinator->snapshot.state != REVLINK_SYNC_RUNNING) {
        return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
    }
    const revlink_sync_coordinator_status_t status =
        coordinator->config.cancel(coordinator->config.transport_context);
    if (status != REVLINK_SYNC_COORDINATOR_OK) {
        return status;
    }
    coordinator->snapshot.state = REVLINK_SYNC_CANCELLING;
    publish(coordinator);
    return REVLINK_SYNC_COORDINATOR_OK;
}

revlink_sync_coordinator_status_t revlink_sync_coordinator_handle_event(
    revlink_sync_coordinator_t *coordinator,
    const revlink_sync_event_t *event
)
{
    if (coordinator == NULL || event == NULL) {
        return REVLINK_SYNC_COORDINATOR_INVALID_ARGUMENT;
    }

    revlink_sync_state_t state = coordinator->snapshot.state;
    switch (event->kind) {
    case REVLINK_SYNC_EVENT_STARTED:
        if (state == REVLINK_SYNC_CANCELLING) {
            break;
        }
        if (state != REVLINK_SYNC_QUEUED) {
            return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
        }
        state = REVLINK_SYNC_RUNNING;
        break;
    case REVLINK_SYNC_EVENT_PROGRESS:
        if (state != REVLINK_SYNC_RUNNING
            && state != REVLINK_SYNC_CANCELLING) {
            return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
        }
        break;
    case REVLINK_SYNC_EVENT_COMPLETED:
        if (state != REVLINK_SYNC_RUNNING) {
            return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
        }
        state = REVLINK_SYNC_COMPLETED;
        break;
    case REVLINK_SYNC_EVENT_FAILED:
        if (!is_active(state)) {
            return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
        }
        state = REVLINK_SYNC_FAILED;
        break;
    case REVLINK_SYNC_EVENT_CANCELLED:
        if (!is_active(state)) {
            return REVLINK_SYNC_COORDINATOR_INVALID_STATE;
        }
        state = REVLINK_SYNC_CANCELLED;
        break;
    default:
        return REVLINK_SYNC_COORDINATOR_INVALID_ARGUMENT;
    }

    coordinator->snapshot.state = state;
    coordinator->snapshot.candidates = event->candidates;
    coordinator->snapshot.downloaded = event->downloaded;
    coordinator->snapshot.skipped = event->skipped;
    coordinator->snapshot.downloaded_bytes = event->downloaded_bytes;
    coordinator->snapshot.pending = event->pending;
    coordinator->snapshot.close_recovery_attempt =
        event->close_recovery_attempt;
    coordinator->snapshot.data_phase_completed =
        event->data_phase_completed;
    coordinator->snapshot.session_close_sent = event->session_close_sent;
    coordinator->snapshot.session_close_acknowledged =
        event->session_close_acknowledged;
    coordinator->snapshot.last_platform_error = event->platform_error;
    publish(coordinator);
    return REVLINK_SYNC_COORDINATOR_OK;
}

revlink_sync_snapshot_t revlink_sync_coordinator_snapshot(
    const revlink_sync_coordinator_t *coordinator
)
{
    revlink_sync_snapshot_t snapshot = {0};
    if (coordinator != NULL) {
        snapshot = coordinator->snapshot;
    }
    return snapshot;
}

const char *revlink_sync_state_name(revlink_sync_state_t state)
{
    switch (state) {
    case REVLINK_SYNC_IDLE:
        return "idle";
    case REVLINK_SYNC_QUEUED:
        return "queued";
    case REVLINK_SYNC_RUNNING:
        return "running";
    case REVLINK_SYNC_CANCELLING:
        return "cancelling";
    case REVLINK_SYNC_COMPLETED:
        return "completed";
    case REVLINK_SYNC_FAILED:
        return "failed";
    case REVLINK_SYNC_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
    }
}
