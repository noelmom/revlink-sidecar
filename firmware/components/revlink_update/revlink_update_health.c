#include "revlink_update_health.h"

#include <limits.h>
#include <stddef.h>

static bool valid_health_value(revlink_health_value_t value)
{
    return value >= REVLINK_HEALTH_UNKNOWN
        && value <= REVLINK_HEALTH_FAILED;
}

static uint32_t saturating_add(uint32_t left, uint32_t right)
{
    if (UINT32_MAX - left < right) {
        return UINT32_MAX;
    }
    return left + right;
}

static bool ready(revlink_health_value_t value)
{
    return value == REVLINK_HEALTH_READY;
}

static revlink_update_blocker_t first_blocker(
    const revlink_update_health_gate_t *gate,
    const revlink_update_health_observation_t *observation
)
{
    if (!ready(observation->nvs)) {
        return REVLINK_UPDATE_BLOCKER_NVS;
    }
    if (!ready(observation->storage)
        && observation->storage != REVLINK_HEALTH_RECOVERABLE) {
        return REVLINK_UPDATE_BLOCKER_STORAGE;
    }
    if (gate->config.display_required
        && !ready(observation->display)) {
        return REVLINK_UPDATE_BLOCKER_DISPLAY;
    }
    if (!ready(observation->network)) {
        return REVLINK_UPDATE_BLOCKER_NETWORK;
    }
    if (!ready(observation->portal)) {
        return REVLINK_UPDATE_BLOCKER_PORTAL;
    }
    if (!ready(observation->usb)) {
        return REVLINK_UPDATE_BLOCKER_USB;
    }
    if (!ready(observation->safety_policy)) {
        return REVLINK_UPDATE_BLOCKER_SAFETY_POLICY;
    }
    return REVLINK_UPDATE_BLOCKER_NONE;
}

bool revlink_update_health_init(
    revlink_update_health_gate_t *gate,
    const revlink_update_health_config_t *config
)
{
    if (gate == NULL || config == NULL || config->timeout_ms == 0U) {
        return false;
    }
    *gate = (revlink_update_health_gate_t){
        .config = *config,
        .snapshot = {
            .state = REVLINK_UPDATE_HEALTH_WAITING,
            .blocker = REVLINK_UPDATE_BLOCKER_NVS,
        },
    };
    return true;
}

revlink_update_health_snapshot_t revlink_update_health_observe(
    revlink_update_health_gate_t *gate,
    const revlink_update_health_observation_t *observation,
    uint32_t elapsed_ms
)
{
    if (gate == NULL || observation == NULL) {
        return (revlink_update_health_snapshot_t){
            .state = REVLINK_UPDATE_HEALTH_REJECTED,
        };
    }
    if (gate->snapshot.state != REVLINK_UPDATE_HEALTH_WAITING) {
        return gate->snapshot;
    }
    if (!valid_health_value(observation->nvs)
        || !valid_health_value(observation->storage)
        || !valid_health_value(observation->display)
        || !valid_health_value(observation->network)
        || !valid_health_value(observation->portal)
        || !valid_health_value(observation->usb)
        || !valid_health_value(observation->safety_policy)) {
        gate->snapshot.state = REVLINK_UPDATE_HEALTH_REJECTED;
        gate->snapshot.blocker = REVLINK_UPDATE_BLOCKER_NONE;
        return gate->snapshot;
    }

    gate->snapshot.elapsed_ms = saturating_add(
        gate->snapshot.elapsed_ms,
        elapsed_ms
    );
    gate->snapshot.blocker = first_blocker(gate, observation);

    if (observation->safety_policy == REVLINK_HEALTH_FAILED) {
        gate->snapshot.state = REVLINK_UPDATE_HEALTH_REJECTED;
        gate->snapshot.blocker = REVLINK_UPDATE_BLOCKER_SAFETY_POLICY;
    } else if (gate->snapshot.blocker == REVLINK_UPDATE_BLOCKER_NONE) {
        gate->snapshot.state = REVLINK_UPDATE_HEALTH_READY;
    } else if (gate->snapshot.elapsed_ms >= gate->config.timeout_ms) {
        gate->snapshot.state = REVLINK_UPDATE_HEALTH_TIMED_OUT;
    }
    return gate->snapshot;
}

revlink_update_health_snapshot_t revlink_update_health_snapshot(
    const revlink_update_health_gate_t *gate
)
{
    if (gate == NULL) {
        return (revlink_update_health_snapshot_t){
            .state = REVLINK_UPDATE_HEALTH_REJECTED,
        };
    }
    return gate->snapshot;
}

const char *revlink_update_health_state_name(
    revlink_update_health_state_t state
)
{
    switch (state) {
        case REVLINK_UPDATE_HEALTH_WAITING:
            return "waiting";
        case REVLINK_UPDATE_HEALTH_READY:
            return "ready";
        case REVLINK_UPDATE_HEALTH_TIMED_OUT:
            return "timed-out";
        case REVLINK_UPDATE_HEALTH_REJECTED:
            return "rejected";
        default:
            return "unknown";
    }
}

const char *revlink_update_blocker_name(revlink_update_blocker_t blocker)
{
    switch (blocker) {
        case REVLINK_UPDATE_BLOCKER_NONE:
            return "none";
        case REVLINK_UPDATE_BLOCKER_NVS:
            return "nvs";
        case REVLINK_UPDATE_BLOCKER_STORAGE:
            return "storage";
        case REVLINK_UPDATE_BLOCKER_DISPLAY:
            return "display";
        case REVLINK_UPDATE_BLOCKER_NETWORK:
            return "network";
        case REVLINK_UPDATE_BLOCKER_PORTAL:
            return "portal";
        case REVLINK_UPDATE_BLOCKER_USB:
            return "usb";
        case REVLINK_UPDATE_BLOCKER_SAFETY_POLICY:
            return "safety-policy";
        default:
            return "unknown";
    }
}
