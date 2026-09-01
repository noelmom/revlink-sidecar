#include "revlink_network_coordinator.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint32_t saturated_add(uint32_t value, uint32_t increment)
{
    return increment > UINT32_MAX - value
        ? UINT32_MAX
        : value + increment;
}

static void clear_action(revlink_network_action_t *action)
{
    *action = (revlink_network_action_t){
        .kind = REVLINK_NETWORK_ACTION_NONE,
        .network_id = 0U,
    };
}

static void transition(
    revlink_network_coordinator_t *coordinator,
    revlink_network_state_t state
)
{
    coordinator->snapshot.state = state;
    coordinator->snapshot.phase_elapsed_ms = 0U;
    if (state != REVLINK_NETWORK_CLIENT_READY) {
        coordinator->snapshot.health_elapsed_ms = 0U;
    }
    switch (state) {
    case REVLINK_NETWORK_SEARCHING:
    case REVLINK_NETWORK_CONNECTING:
        coordinator->snapshot.phase_timeout_ms =
            coordinator->config.startup_timeout_ms;
        break;
    case REVLINK_NETWORK_RECONNECTING:
        coordinator->snapshot.phase_timeout_ms =
            coordinator->config.reconnect_timeout_ms;
        break;
    default:
        coordinator->snapshot.phase_timeout_ms = 0U;
        break;
    }
}

static void request_hotspot(
    revlink_network_coordinator_t *coordinator,
    revlink_network_action_t *action
)
{
    transition(coordinator, REVLINK_NETWORK_HOTSPOT_STARTING);
    coordinator->snapshot.selected_network_id = 0U;
    action->kind = REVLINK_NETWORK_ACTION_START_HOTSPOT;
}

static bool is_ready_state(revlink_network_state_t state)
{
    return state == REVLINK_NETWORK_CLIENT_READY
        || state == REVLINK_NETWORK_HOTSPOT_READY;
}

bool revlink_network_coordinator_init(
    revlink_network_coordinator_t *coordinator,
    const revlink_network_config_t *config
)
{
    if (coordinator == NULL || config == NULL
        || config->startup_timeout_ms == 0U
        || config->startup_attempt_limit == 0U
        || config->reconnect_timeout_ms == 0U
        || config->health_probe_interval_ms == 0U
        || config->health_failure_threshold == 0U) {
        return false;
    }
    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->config = *config;
    coordinator->snapshot.state = REVLINK_NETWORK_STOPPED;
    return true;
}

static revlink_network_status_t handle_tick(
    revlink_network_coordinator_t *coordinator,
    const revlink_network_event_t *event,
    revlink_network_action_t *action
)
{
    if (event->elapsed_ms == 0U) {
        return REVLINK_NETWORK_INVALID_ARGUMENT;
    }

    const revlink_network_state_t state = coordinator->snapshot.state;
    if (state == REVLINK_NETWORK_CLIENT_READY) {
        if (coordinator->snapshot.transfer_active) {
            coordinator->snapshot.health_elapsed_ms = 0U;
            return REVLINK_NETWORK_OK;
        }
        coordinator->snapshot.health_elapsed_ms = saturated_add(
            coordinator->snapshot.health_elapsed_ms,
            event->elapsed_ms
        );
        if (coordinator->snapshot.health_elapsed_ms
            >= coordinator->config.health_probe_interval_ms) {
            coordinator->snapshot.health_elapsed_ms = 0U;
            action->kind = REVLINK_NETWORK_ACTION_PROBE_CLIENT;
            action->network_id =
                coordinator->snapshot.selected_network_id;
        }
        return REVLINK_NETWORK_OK;
    }
    if (state != REVLINK_NETWORK_SEARCHING
        && state != REVLINK_NETWORK_CONNECTING
        && state != REVLINK_NETWORK_RECONNECTING) {
        return REVLINK_NETWORK_OK;
    }
    coordinator->snapshot.phase_elapsed_ms = saturated_add(
        coordinator->snapshot.phase_elapsed_ms,
        event->elapsed_ms
    );
    const uint32_t timeout = coordinator->snapshot.phase_timeout_ms;
    if (coordinator->snapshot.phase_elapsed_ms >= timeout) {
        request_hotspot(coordinator, action);
    }
    return REVLINK_NETWORK_OK;
}

revlink_network_status_t revlink_network_coordinator_handle(
    revlink_network_coordinator_t *coordinator,
    const revlink_network_event_t *event,
    revlink_network_action_t *action
)
{
    if (coordinator == NULL || event == NULL || action == NULL) {
        return REVLINK_NETWORK_INVALID_ARGUMENT;
    }
    clear_action(action);
    revlink_network_snapshot_t *snapshot = &coordinator->snapshot;

    switch (event->kind) {
    case REVLINK_NETWORK_EVENT_START:
        if (snapshot->state != REVLINK_NETWORK_STOPPED
            && snapshot->state != REVLINK_NETWORK_FAULTED) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = 0;
        snapshot->selected_network_id = 0U;
        snapshot->startup_attempt_count = 0U;
        snapshot->transfer_active = false;
        transition(coordinator, REVLINK_NETWORK_SEARCHING);
        action->kind = REVLINK_NETWORK_ACTION_SCAN_SAVED;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_SCAN_SELECTED:
        if (snapshot->state != REVLINK_NETWORK_SEARCHING
            || event->network_id == 0U) {
            return event->network_id == 0U
                ? REVLINK_NETWORK_INVALID_ARGUMENT
                : REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->selected_network_id = event->network_id;
        snapshot->startup_attempt_count = 1U;
        transition(coordinator, REVLINK_NETWORK_CONNECTING);
        action->kind = REVLINK_NETWORK_ACTION_CONNECT_SAVED;
        action->network_id = event->network_id;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_SCAN_EMPTY:
        if (snapshot->state != REVLINK_NETWORK_SEARCHING) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        request_hotspot(coordinator, action);
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_CLIENT_CONNECTED:
        if (snapshot->state != REVLINK_NETWORK_CONNECTING
            && snapshot->state != REVLINK_NETWORK_RECONNECTING) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = 0;
        snapshot->health_elapsed_ms = 0U;
        snapshot->consecutive_health_failures = 0U;
        transition(coordinator, REVLINK_NETWORK_CLIENT_READY);
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_CLIENT_FAILED:
        if (snapshot->state != REVLINK_NETWORK_CONNECTING
            && snapshot->state != REVLINK_NETWORK_RECONNECTING) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = event->platform_error;
        if (snapshot->state == REVLINK_NETWORK_RECONNECTING) {
            /*
             * A disconnect callback can arrive immediately while the radio
             * continues its bounded reconnect policy. Preserve the grace
             * period instead of flapping straight into hotspot mode.
             */
            return REVLINK_NETWORK_OK;
        }
        if (
            snapshot->startup_attempt_count
                < coordinator->config.startup_attempt_limit
        ) {
            snapshot->startup_attempt_count++;
            transition(coordinator, REVLINK_NETWORK_CONNECTING);
            action->kind = REVLINK_NETWORK_ACTION_RECOVER_SAVED;
            action->network_id = snapshot->selected_network_id;
            return REVLINK_NETWORK_OK;
        }
        request_hotspot(coordinator, action);
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_CLIENT_LOST:
        if (snapshot->state != REVLINK_NETWORK_CLIENT_READY
            || snapshot->selected_network_id == 0U) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = event->platform_error;
        transition(coordinator, REVLINK_NETWORK_RECONNECTING);
        action->kind = REVLINK_NETWORK_ACTION_CONNECT_SAVED;
        action->network_id = snapshot->selected_network_id;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_CLIENT_HEALTHY:
        if (snapshot->state != REVLINK_NETWORK_CLIENT_READY) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = 0;
        snapshot->consecutive_health_failures = 0U;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_CLIENT_UNHEALTHY:
        if (snapshot->state != REVLINK_NETWORK_CLIENT_READY) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        if (snapshot->transfer_active) {
            return REVLINK_NETWORK_TRANSFER_LOCKED;
        }
        snapshot->last_platform_error = event->platform_error;
        snapshot->consecutive_health_failures = saturated_add(
            snapshot->consecutive_health_failures,
            1U
        );
        if (snapshot->consecutive_health_failures
            >= coordinator->config.health_failure_threshold) {
            transition(coordinator, REVLINK_NETWORK_RECONNECTING);
            action->kind = REVLINK_NETWORK_ACTION_RECOVER_SAVED;
            action->network_id = snapshot->selected_network_id;
        }
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_HOTSPOT_STARTED:
        if (snapshot->state != REVLINK_NETWORK_HOTSPOT_STARTING) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = 0;
        transition(coordinator, REVLINK_NETWORK_HOTSPOT_READY);
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_HOTSPOT_FAILED:
        if (snapshot->state != REVLINK_NETWORK_HOTSPOT_STARTING) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->last_platform_error = event->platform_error;
        transition(coordinator, REVLINK_NETWORK_FAULTED);
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_RETRY_SAVED:
        if (!is_ready_state(snapshot->state)) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        if (snapshot->transfer_active) {
            return REVLINK_NETWORK_TRANSFER_LOCKED;
        }
        snapshot->last_platform_error = 0;
        snapshot->selected_network_id = 0U;
        transition(coordinator, REVLINK_NETWORK_SEARCHING);
        action->kind = REVLINK_NETWORK_ACTION_SCAN_SAVED;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_FORCE_HOTSPOT:
        if (snapshot->state == REVLINK_NETWORK_STOPPED
            || snapshot->state == REVLINK_NETWORK_HOTSPOT_STARTING
            || snapshot->state == REVLINK_NETWORK_HOTSPOT_READY) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        if (snapshot->transfer_active) {
            return REVLINK_NETWORK_TRANSFER_LOCKED;
        }
        request_hotspot(coordinator, action);
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_TRANSFER_STARTED:
        if (!is_ready_state(snapshot->state)
            || snapshot->transfer_active) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->transfer_active = true;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_TRANSFER_FINISHED:
        if (!snapshot->transfer_active) {
            return REVLINK_NETWORK_INVALID_STATE;
        }
        snapshot->transfer_active = false;
        return REVLINK_NETWORK_OK;

    case REVLINK_NETWORK_EVENT_TICK:
        return handle_tick(coordinator, event, action);

    case REVLINK_NETWORK_EVENT_STOP:
        snapshot->state = REVLINK_NETWORK_STOPPED;
        snapshot->selected_network_id = 0U;
        snapshot->phase_elapsed_ms = 0U;
        snapshot->phase_timeout_ms = 0U;
        snapshot->startup_attempt_count = 0U;
        snapshot->health_elapsed_ms = 0U;
        snapshot->consecutive_health_failures = 0U;
        snapshot->last_platform_error = 0;
        snapshot->transfer_active = false;
        action->kind = REVLINK_NETWORK_ACTION_STOP_RADIO;
        return REVLINK_NETWORK_OK;

    default:
        return REVLINK_NETWORK_INVALID_ARGUMENT;
    }
}

revlink_network_snapshot_t revlink_network_coordinator_snapshot(
    const revlink_network_coordinator_t *coordinator
)
{
    return coordinator != NULL
        ? coordinator->snapshot
        : (revlink_network_snapshot_t){0};
}

const char *revlink_network_state_name(revlink_network_state_t state)
{
    switch (state) {
    case REVLINK_NETWORK_STOPPED:
        return "stopped";
    case REVLINK_NETWORK_SEARCHING:
        return "searching";
    case REVLINK_NETWORK_CONNECTING:
        return "connecting";
    case REVLINK_NETWORK_CLIENT_READY:
        return "client-ready";
    case REVLINK_NETWORK_RECONNECTING:
        return "reconnecting";
    case REVLINK_NETWORK_HOTSPOT_STARTING:
        return "hotspot-starting";
    case REVLINK_NETWORK_HOTSPOT_READY:
        return "hotspot-ready";
    case REVLINK_NETWORK_FAULTED:
        return "faulted";
    default:
        return "unknown";
    }
}
