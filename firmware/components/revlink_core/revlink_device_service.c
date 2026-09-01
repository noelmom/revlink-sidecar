#include "revlink_device_service.h"

#include <string.h>

static bool identity_matches(
    const revlink_device_identity_t *left,
    const revlink_device_identity_t *right
)
{
    return left->vendor_id == right->vendor_id
        && left->product_id == right->product_id
        && left->address == right->address
        && left->attachment_generation == right->attachment_generation;
}

static void publish_snapshot(revlink_device_service_t *service)
{
    if (service->observer != NULL) {
        service->observer(service->observer_context, &service->snapshot);
    }
}

static bool event_has_topology_revision(
    const revlink_device_event_t *event
)
{
    return event->kind == REVLINK_DEVICE_EVENT_ATTACHED
        || event->kind == REVLINK_DEVICE_EVENT_ACCEPTED
        || event->kind == REVLINK_DEVICE_EVENT_DETACHED
        || event->kind == REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED;
}

void revlink_device_service_init(
    revlink_device_service_t *service,
    revlink_device_state_observer_t observer,
    void *observer_context
)
{
    if (service == NULL) {
        return;
    }
    memset(service, 0, sizeof(*service));
    service->snapshot.state = REVLINK_DEVICE_STOPPED;
    service->observer = observer;
    service->observer_context = observer_context;
}

revlink_core_status_t revlink_device_service_handle(
    revlink_device_service_t *service,
    const revlink_device_event_t *event
)
{
    if (service == NULL || event == NULL) {
        return REVLINK_CORE_INVALID_ARGUMENT;
    }
    if (event_has_topology_revision(event)
        && event->topology_revision != 0U
        && event->topology_revision < service->snapshot.topology_revision) {
        return REVLINK_CORE_INVALID_TRANSITION;
    }

    const revlink_device_state_t current = service->snapshot.state;
    revlink_device_state_t next = current;

    switch (event->kind) {
    case REVLINK_DEVICE_EVENT_MONITOR_STARTED:
        if (current != REVLINK_DEVICE_STOPPED) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        next = REVLINK_DEVICE_WAITING;
        break;
    case REVLINK_DEVICE_EVENT_ATTACHED:
        if (current != REVLINK_DEVICE_WAITING) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        service->snapshot.identity = event->identity;
        service->snapshot.eligible_device_count = 0U;
        next = REVLINK_DEVICE_INSPECTING;
        break;
    case REVLINK_DEVICE_EVENT_ACCEPTED:
        if (current != REVLINK_DEVICE_INSPECTING
            || !identity_matches(
                &service->snapshot.identity,
                &event->identity
            )) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        service->snapshot.identity = event->identity;
        service->snapshot.eligible_device_count = 1U;
        next = REVLINK_DEVICE_AVAILABLE;
        break;
    case REVLINK_DEVICE_EVENT_SESSION_OPENED:
        if (current != REVLINK_DEVICE_AVAILABLE) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        next = REVLINK_DEVICE_SESSION_ACTIVE;
        break;
    case REVLINK_DEVICE_EVENT_SESSION_CLOSED:
        if (current == REVLINK_DEVICE_CONFLICT) {
            break;
        }
        if (current != REVLINK_DEVICE_SESSION_ACTIVE) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        next = REVLINK_DEVICE_AVAILABLE;
        break;
    case REVLINK_DEVICE_EVENT_DETACHED:
        if (current == REVLINK_DEVICE_STOPPED
            || current == REVLINK_DEVICE_WAITING) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        if (current == REVLINK_DEVICE_CONFLICT) {
            service->snapshot.eligible_device_count =
                event->eligible_device_count;
            if (event->eligible_device_count != 0U) {
                break;
            }
            service->snapshot.conflict_recovery_required = false;
        } else {
            service->snapshot.eligible_device_count = 0U;
        }
        memset(
            &service->snapshot.identity,
            0,
            sizeof(service->snapshot.identity)
        );
        next = REVLINK_DEVICE_WAITING;
        break;
    case REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED:
        if (current == REVLINK_DEVICE_STOPPED
            || event->eligible_device_count < 2U) {
            return REVLINK_CORE_INVALID_TRANSITION;
        }
        memset(
            &service->snapshot.identity,
            0,
            sizeof(service->snapshot.identity)
        );
        service->snapshot.eligible_device_count =
            event->eligible_device_count;
        service->snapshot.conflict_recovery_required = true;
        next = REVLINK_DEVICE_CONFLICT;
        break;
    case REVLINK_DEVICE_EVENT_FAILURE:
        service->snapshot.last_platform_error = event->platform_error;
        if (current != REVLINK_DEVICE_CONFLICT) {
            next = REVLINK_DEVICE_FAULTED;
        }
        break;
    default:
        return REVLINK_CORE_INVALID_ARGUMENT;
    }

    service->snapshot.state = next;
    if (event_has_topology_revision(event)
        && event->topology_revision != 0U) {
        service->snapshot.topology_revision = event->topology_revision;
    }
    publish_snapshot(service);
    return REVLINK_CORE_OK;
}

revlink_device_snapshot_t revlink_device_service_snapshot(
    const revlink_device_service_t *service
)
{
    revlink_device_snapshot_t snapshot = {0};
    if (service != NULL) {
        snapshot = service->snapshot;
    }
    return snapshot;
}

const char *revlink_device_state_name(revlink_device_state_t state)
{
    switch (state) {
    case REVLINK_DEVICE_STOPPED:
        return "stopped";
    case REVLINK_DEVICE_WAITING:
        return "waiting";
    case REVLINK_DEVICE_INSPECTING:
        return "inspecting";
    case REVLINK_DEVICE_AVAILABLE:
        return "available";
    case REVLINK_DEVICE_SESSION_ACTIVE:
        return "session-active";
    case REVLINK_DEVICE_CONFLICT:
        return "multiple-devices";
    case REVLINK_DEVICE_FAULTED:
        return "faulted";
    default:
        return "unknown";
    }
}

const char *revlink_core_status_name(revlink_core_status_t status)
{
    switch (status) {
    case REVLINK_CORE_OK:
        return "ok";
    case REVLINK_CORE_INVALID_ARGUMENT:
        return "invalid-argument";
    case REVLINK_CORE_INVALID_TRANSITION:
        return "invalid-transition";
    case REVLINK_CORE_NOT_AUTHORIZED:
        return "not-authorized";
    case REVLINK_CORE_NOT_SUPPORTED:
        return "not-supported";
    case REVLINK_CORE_PLATFORM_ERROR:
        return "platform-error";
    default:
        return "unknown";
    }
}
