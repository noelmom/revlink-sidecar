#ifndef REVLINK_NETWORK_COORDINATOR_H
#define REVLINK_NETWORK_COORDINATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Network identities are opaque handles owned by the credential store and
 * radio adapter. Zero is reserved to mean that no saved network was selected.
 */
typedef uint32_t revlink_network_id_t;

typedef enum {
    REVLINK_NETWORK_STOPPED = 0,
    REVLINK_NETWORK_SEARCHING,
    REVLINK_NETWORK_CONNECTING,
    REVLINK_NETWORK_CLIENT_READY,
    REVLINK_NETWORK_RECONNECTING,
    REVLINK_NETWORK_HOTSPOT_STARTING,
    REVLINK_NETWORK_HOTSPOT_READY,
    REVLINK_NETWORK_FAULTED,
} revlink_network_state_t;

typedef enum {
    REVLINK_NETWORK_EVENT_START = 0,
    REVLINK_NETWORK_EVENT_SCAN_SELECTED,
    REVLINK_NETWORK_EVENT_SCAN_EMPTY,
    REVLINK_NETWORK_EVENT_CLIENT_CONNECTED,
    REVLINK_NETWORK_EVENT_CLIENT_FAILED,
    REVLINK_NETWORK_EVENT_CLIENT_LOST,
    REVLINK_NETWORK_EVENT_CLIENT_HEALTHY,
    REVLINK_NETWORK_EVENT_CLIENT_UNHEALTHY,
    REVLINK_NETWORK_EVENT_HOTSPOT_STARTED,
    REVLINK_NETWORK_EVENT_HOTSPOT_FAILED,
    REVLINK_NETWORK_EVENT_RETRY_SAVED,
    REVLINK_NETWORK_EVENT_FORCE_HOTSPOT,
    REVLINK_NETWORK_EVENT_TRANSFER_STARTED,
    REVLINK_NETWORK_EVENT_TRANSFER_FINISHED,
    REVLINK_NETWORK_EVENT_TICK,
    REVLINK_NETWORK_EVENT_STOP,
} revlink_network_event_kind_t;

typedef struct {
    revlink_network_event_kind_t kind;
    revlink_network_id_t network_id;
    uint32_t elapsed_ms;
    int platform_error;
} revlink_network_event_t;

typedef enum {
    REVLINK_NETWORK_ACTION_NONE = 0,
    REVLINK_NETWORK_ACTION_SCAN_SAVED,
    REVLINK_NETWORK_ACTION_CONNECT_SAVED,
    REVLINK_NETWORK_ACTION_RECOVER_SAVED,
    REVLINK_NETWORK_ACTION_PROBE_CLIENT,
    REVLINK_NETWORK_ACTION_START_HOTSPOT,
    REVLINK_NETWORK_ACTION_STOP_RADIO,
} revlink_network_action_kind_t;

typedef struct {
    revlink_network_action_kind_t kind;
    revlink_network_id_t network_id;
} revlink_network_action_t;

typedef enum {
    REVLINK_NETWORK_OK = 0,
    REVLINK_NETWORK_INVALID_ARGUMENT,
    REVLINK_NETWORK_INVALID_STATE,
    REVLINK_NETWORK_TRANSFER_LOCKED,
} revlink_network_status_t;

typedef struct {
    uint32_t startup_timeout_ms;
    uint32_t startup_attempt_limit;
    uint32_t reconnect_timeout_ms;
    uint32_t health_probe_interval_ms;
    uint32_t health_failure_threshold;
} revlink_network_config_t;

typedef struct {
    revlink_network_state_t state;
    revlink_network_id_t selected_network_id;
    uint32_t phase_elapsed_ms;
    uint32_t phase_timeout_ms;
    uint32_t startup_attempt_count;
    uint32_t health_elapsed_ms;
    uint32_t consecutive_health_failures;
    int last_platform_error;
    bool transfer_active;
} revlink_network_snapshot_t;

typedef struct {
    revlink_network_config_t config;
    revlink_network_snapshot_t snapshot;
} revlink_network_coordinator_t;

bool revlink_network_coordinator_init(
    revlink_network_coordinator_t *coordinator,
    const revlink_network_config_t *config
);

revlink_network_status_t revlink_network_coordinator_handle(
    revlink_network_coordinator_t *coordinator,
    const revlink_network_event_t *event,
    revlink_network_action_t *action
);

revlink_network_snapshot_t revlink_network_coordinator_snapshot(
    const revlink_network_coordinator_t *coordinator
);

const char *revlink_network_state_name(revlink_network_state_t state);

#ifdef __cplusplus
}
#endif

#endif
