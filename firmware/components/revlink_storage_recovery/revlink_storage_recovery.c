#include "revlink_storage_recovery.h"

#include <limits.h>
#include <string.h>

static uint32_t saturated_add(uint32_t value, uint32_t increment)
{
    return increment > UINT32_MAX - value
        ? UINT32_MAX
        : value + increment;
}

bool revlink_storage_recovery_init(
    revlink_storage_recovery_t *recovery,
    bool format_available,
    uint32_t confirmation_timeout_ms
)
{
    if (recovery == NULL || confirmation_timeout_ms == 0U) {
        return false;
    }
    memset(recovery, 0, sizeof(*recovery));
    recovery->state = format_available
        ? REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST
        : REVLINK_STORAGE_RECOVERY_UNAVAILABLE;
    recovery->confirmation_timeout_ms = confirmation_timeout_ms;
    return true;
}

revlink_storage_recovery_action_t
revlink_storage_recovery_double_press(
    revlink_storage_recovery_t *recovery
)
{
    if (recovery == NULL) {
        return REVLINK_STORAGE_RECOVERY_NO_ACTION;
    }
    if (recovery->state == REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST) {
        recovery->state =
            REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION;
        recovery->confirmation_elapsed_ms = 0U;
        return REVLINK_STORAGE_RECOVERY_SHOW_WARNING;
    }
    if (
        recovery->state
            == REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION
        && recovery->confirmation_elapsed_ms
            < recovery->confirmation_timeout_ms
    ) {
        recovery->state = REVLINK_STORAGE_RECOVERY_FORMATTING;
        return REVLINK_STORAGE_RECOVERY_FORMAT_REQUESTED;
    }
    return REVLINK_STORAGE_RECOVERY_NO_ACTION;
}

revlink_storage_recovery_action_t revlink_storage_recovery_tick(
    revlink_storage_recovery_t *recovery,
    uint32_t elapsed_ms
)
{
    if (
        recovery == NULL || elapsed_ms == 0U
        || recovery->state
            != REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION
    ) {
        return REVLINK_STORAGE_RECOVERY_NO_ACTION;
    }
    recovery->confirmation_elapsed_ms = saturated_add(
        recovery->confirmation_elapsed_ms,
        elapsed_ms
    );
    if (
        recovery->confirmation_elapsed_ms
        < recovery->confirmation_timeout_ms
    ) {
        return REVLINK_STORAGE_RECOVERY_NO_ACTION;
    }
    recovery->confirmation_elapsed_ms = 0U;
    recovery->state = REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST;
    return REVLINK_STORAGE_RECOVERY_WARNING_TIMED_OUT;
}

uint32_t revlink_storage_recovery_seconds_remaining(
    const revlink_storage_recovery_t *recovery
)
{
    if (
        recovery == NULL
        || recovery->state
            != REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION
        || recovery->confirmation_elapsed_ms
            >= recovery->confirmation_timeout_ms
    ) {
        return 0U;
    }
    const uint32_t remaining_ms =
        recovery->confirmation_timeout_ms
        - recovery->confirmation_elapsed_ms;
    return (remaining_ms + 999U) / 1000U;
}

void revlink_storage_recovery_format_failed(
    revlink_storage_recovery_t *recovery
)
{
    if (recovery == NULL) {
        return;
    }
    recovery->confirmation_elapsed_ms = 0U;
    recovery->state = REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST;
}
