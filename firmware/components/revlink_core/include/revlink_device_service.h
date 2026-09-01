#ifndef REVLINK_DEVICE_SERVICE_H
#define REVLINK_DEVICE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_CORE_OK = 0,
    REVLINK_CORE_INVALID_ARGUMENT,
    REVLINK_CORE_INVALID_TRANSITION,
    REVLINK_CORE_NOT_AUTHORIZED,
    REVLINK_CORE_NOT_SUPPORTED,
    REVLINK_CORE_PLATFORM_ERROR,
} revlink_core_status_t;

typedef enum {
    REVLINK_DEVICE_STOPPED = 0,
    REVLINK_DEVICE_WAITING,
    REVLINK_DEVICE_INSPECTING,
    REVLINK_DEVICE_AVAILABLE,
    REVLINK_DEVICE_SESSION_ACTIVE,
    REVLINK_DEVICE_CONFLICT,
    REVLINK_DEVICE_FAULTED,
} revlink_device_state_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t address;
    uint8_t configuration_count;
    uint8_t matching_configuration_count;
    uint8_t interface_number;
    uint8_t bulk_out_endpoint;
    uint8_t bulk_in_endpoint;
    uint16_t bulk_max_packet_size;
    uint32_t attachment_generation;
    bool high_speed;
} revlink_device_identity_t;

typedef enum {
    REVLINK_DEVICE_EVENT_MONITOR_STARTED = 0,
    REVLINK_DEVICE_EVENT_ATTACHED,
    REVLINK_DEVICE_EVENT_ACCEPTED,
    REVLINK_DEVICE_EVENT_SESSION_OPENED,
    REVLINK_DEVICE_EVENT_SESSION_CLOSED,
    REVLINK_DEVICE_EVENT_DETACHED,
    REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED,
    REVLINK_DEVICE_EVENT_FAILURE,
} revlink_device_event_kind_t;

typedef struct {
    revlink_device_event_kind_t kind;
    revlink_device_identity_t identity;
    int platform_error;
    uint8_t eligible_device_count;
    /*
     * Monotonic transport-owned revision for eligible-device topology
     * changes. Zero is reserved for platform-neutral legacy/test events.
     * Events older than the latest nonzero revision are rejected.
     */
    uint32_t topology_revision;
    bool software_reenumeration;
} revlink_device_event_t;

typedef struct {
    revlink_device_state_t state;
    revlink_device_identity_t identity;
    int last_platform_error;
    uint8_t eligible_device_count;
    uint32_t topology_revision;
    bool conflict_recovery_required;
} revlink_device_snapshot_t;

typedef void (*revlink_device_state_observer_t)(
    void *context,
    const revlink_device_snapshot_t *snapshot
);

typedef struct {
    revlink_device_snapshot_t snapshot;
    revlink_device_state_observer_t observer;
    void *observer_context;
} revlink_device_service_t;

void revlink_device_service_init(
    revlink_device_service_t *service,
    revlink_device_state_observer_t observer,
    void *observer_context
);

revlink_core_status_t revlink_device_service_handle(
    revlink_device_service_t *service,
    const revlink_device_event_t *event
);

revlink_device_snapshot_t revlink_device_service_snapshot(
    const revlink_device_service_t *service
);

const char *revlink_device_state_name(revlink_device_state_t state);
const char *revlink_core_status_name(revlink_core_status_t status);

#ifdef __cplusplus
}
#endif

#endif
