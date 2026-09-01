#ifndef REVLINK_STATUS_MODEL_H
#define REVLINK_STATUS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "revlink_device_service.h"
#include "revlink_network_coordinator.h"
#include "revlink_sync_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_STATUS_VEHICLE_LABEL_CAPACITY 24U
#define REVLINK_STATUS_PART_NUMBER_CAPACITY 24U
#define REVLINK_STATUS_CONNECTED_LABEL_CAPACITY 36U

typedef enum {
    REVLINK_STATUS_BOOTING = 0,
    REVLINK_STATUS_WAITING,
    REVLINK_STATUS_INSPECTING,
    REVLINK_STATUS_READY,
    REVLINK_STATUS_SYNC_QUEUED,
    REVLINK_STATUS_SYNCING,
    REVLINK_STATUS_RECOVERING,
    REVLINK_STATUS_COMPLETE,
    REVLINK_STATUS_CANCELLING,
    REVLINK_STATUS_WIFI_RECONNECTING,
    REVLINK_STATUS_ATTENTION,
} revlink_status_kind_t;

typedef struct {
    revlink_device_snapshot_t device;
    revlink_network_snapshot_t network;
    revlink_sync_snapshot_t sync;
    char vehicle_label[REVLINK_STATUS_VEHICLE_LABEL_CAPACITY];
    char part_number[REVLINK_STATUS_PART_NUMBER_CAPACITY];
    char connected_label[REVLINK_STATUS_CONNECTED_LABEL_CAPACITY];
    bool boot_complete;
} revlink_status_model_t;

typedef struct {
    revlink_status_kind_t kind;
    const char *headline;
    const char *detail;
    const char *footer;
    uint8_t progress_percent;
    uint16_t countdown_seconds;
    bool show_progress;
    bool progress_indeterminate;
} revlink_status_view_t;

void revlink_status_model_init(revlink_status_model_t *model);

void revlink_status_model_set_boot_complete(
    revlink_status_model_t *model,
    bool complete
);

void revlink_status_model_set_device(
    revlink_status_model_t *model,
    const revlink_device_snapshot_t *snapshot
);

void revlink_status_model_set_sync(
    revlink_status_model_t *model,
    const revlink_sync_snapshot_t *snapshot
);

void revlink_status_model_set_network(
    revlink_status_model_t *model,
    const revlink_network_snapshot_t *snapshot
);

/*
 * Supplies the vehicle identity learned from the AccessPort. Product/vendor
 * suffixes are removed and the result is bounded to fit the 128 px display.
 * NULL or an empty string restores the generic AccessPort fallback.
 */
void revlink_status_model_set_vehicle(
    revlink_status_model_t *model,
    const char *vehicle
);

/*
 * Supplies the part number learned from the same authoritative identity
 * handshake. The ready screen renders "<part> CONNECTED"; NULL or an empty
 * string restores the generic AccessPort fallback.
 */
void revlink_status_model_set_part_number(
    revlink_status_model_t *model,
    const char *part_number
);

revlink_status_view_t revlink_status_model_view(
    const revlink_status_model_t *model
);

#ifdef __cplusplus
}
#endif

#endif
