#include "revlink_safety_policy.h"

#include <stddef.h>

revlink_safety_policy_t revlink_safety_policy_default(void)
{
    const revlink_safety_policy_t policy = {
        .allow_device_writes = false,
        .allow_device_deletes = false,
    };
    return policy;
}

revlink_core_status_t revlink_safety_policy_authorize(
    const revlink_safety_policy_t *policy,
    revlink_operation_t operation
)
{
    if (policy == NULL) {
        return REVLINK_CORE_INVALID_ARGUMENT;
    }

    switch (operation) {
    case REVLINK_OPERATION_DISCOVER:
    case REVLINK_OPERATION_READ_DEVICE:
        return REVLINK_CORE_OK;
    case REVLINK_OPERATION_WRITE_MAP:
    case REVLINK_OPERATION_WRITE_STARTUP_SCREEN:
        return policy->allow_device_writes
            ? REVLINK_CORE_OK
            : REVLINK_CORE_NOT_AUTHORIZED;
    case REVLINK_OPERATION_DELETE_DEVICE_FILE:
        return policy->allow_device_writes && policy->allow_device_deletes
            ? REVLINK_CORE_OK
            : REVLINK_CORE_NOT_AUTHORIZED;
    default:
        return REVLINK_CORE_INVALID_ARGUMENT;
    }
}
