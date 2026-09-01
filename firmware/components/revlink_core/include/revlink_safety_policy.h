#ifndef REVLINK_SAFETY_POLICY_H
#define REVLINK_SAFETY_POLICY_H

#include <stdbool.h>

#include "revlink_device_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_OPERATION_DISCOVER = 0,
    REVLINK_OPERATION_READ_DEVICE,
    REVLINK_OPERATION_WRITE_MAP,
    REVLINK_OPERATION_WRITE_STARTUP_SCREEN,
    REVLINK_OPERATION_DELETE_DEVICE_FILE,
} revlink_operation_t;

typedef struct {
    bool allow_device_writes;
    bool allow_device_deletes;
} revlink_safety_policy_t;

revlink_safety_policy_t revlink_safety_policy_default(void);

revlink_core_status_t revlink_safety_policy_authorize(
    const revlink_safety_policy_t *policy,
    revlink_operation_t operation
);

#ifdef __cplusplus
}
#endif

#endif
