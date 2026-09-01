#ifndef REVLINK_STORAGE_RECOVERY_H
#define REVLINK_STORAGE_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_STORAGE_RECOVERY_UNAVAILABLE = 0,
    REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST,
    REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION,
    REVLINK_STORAGE_RECOVERY_FORMATTING,
} revlink_storage_recovery_state_t;

typedef enum {
    REVLINK_STORAGE_RECOVERY_NO_ACTION = 0,
    REVLINK_STORAGE_RECOVERY_SHOW_WARNING,
    REVLINK_STORAGE_RECOVERY_WARNING_TIMED_OUT,
    REVLINK_STORAGE_RECOVERY_FORMAT_REQUESTED,
} revlink_storage_recovery_action_t;

typedef struct {
    revlink_storage_recovery_state_t state;
    uint32_t confirmation_timeout_ms;
    uint32_t confirmation_elapsed_ms;
} revlink_storage_recovery_t;

bool revlink_storage_recovery_init(
    revlink_storage_recovery_t *recovery,
    bool format_available,
    uint32_t confirmation_timeout_ms
);

revlink_storage_recovery_action_t
revlink_storage_recovery_double_press(
    revlink_storage_recovery_t *recovery
);

revlink_storage_recovery_action_t revlink_storage_recovery_tick(
    revlink_storage_recovery_t *recovery,
    uint32_t elapsed_ms
);

uint32_t revlink_storage_recovery_seconds_remaining(
    const revlink_storage_recovery_t *recovery
);

void revlink_storage_recovery_format_failed(
    revlink_storage_recovery_t *recovery
);

#ifdef __cplusplus
}
#endif

#endif
