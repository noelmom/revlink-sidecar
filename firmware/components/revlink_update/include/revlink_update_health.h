#ifndef REVLINK_UPDATE_HEALTH_H
#define REVLINK_UPDATE_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_HEALTH_UNKNOWN = 0,
    REVLINK_HEALTH_READY,
    REVLINK_HEALTH_RECOVERABLE,
    REVLINK_HEALTH_FAILED,
} revlink_health_value_t;

typedef enum {
    REVLINK_UPDATE_HEALTH_WAITING = 0,
    REVLINK_UPDATE_HEALTH_READY,
    REVLINK_UPDATE_HEALTH_TIMED_OUT,
    REVLINK_UPDATE_HEALTH_REJECTED,
} revlink_update_health_state_t;

typedef enum {
    REVLINK_UPDATE_BLOCKER_NONE = 0,
    REVLINK_UPDATE_BLOCKER_NVS,
    REVLINK_UPDATE_BLOCKER_STORAGE,
    REVLINK_UPDATE_BLOCKER_DISPLAY,
    REVLINK_UPDATE_BLOCKER_NETWORK,
    REVLINK_UPDATE_BLOCKER_PORTAL,
    REVLINK_UPDATE_BLOCKER_USB,
    REVLINK_UPDATE_BLOCKER_SAFETY_POLICY,
} revlink_update_blocker_t;

typedef struct {
    uint32_t timeout_ms;
    bool display_required;
} revlink_update_health_config_t;

typedef struct {
    revlink_health_value_t nvs;
    revlink_health_value_t storage;
    revlink_health_value_t display;
    revlink_health_value_t network;
    revlink_health_value_t portal;
    revlink_health_value_t usb;
    revlink_health_value_t safety_policy;
} revlink_update_health_observation_t;

typedef struct {
    revlink_update_health_state_t state;
    revlink_update_blocker_t blocker;
    uint32_t elapsed_ms;
} revlink_update_health_snapshot_t;

typedef struct {
    revlink_update_health_config_t config;
    revlink_update_health_snapshot_t snapshot;
} revlink_update_health_gate_t;

bool revlink_update_health_init(
    revlink_update_health_gate_t *gate,
    const revlink_update_health_config_t *config
);

revlink_update_health_snapshot_t revlink_update_health_observe(
    revlink_update_health_gate_t *gate,
    const revlink_update_health_observation_t *observation,
    uint32_t elapsed_ms
);

revlink_update_health_snapshot_t revlink_update_health_snapshot(
    const revlink_update_health_gate_t *gate
);

const char *revlink_update_health_state_name(
    revlink_update_health_state_t state
);

const char *revlink_update_blocker_name(revlink_update_blocker_t blocker);

#ifdef __cplusplus
}
#endif

#endif
