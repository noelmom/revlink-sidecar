#ifndef REVLINK_POWER_BUTTON_H
#define REVLINK_POWER_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_POWER_BUTTON_NO_ACTION = 0,
    REVLINK_POWER_BUTTON_SHORT_PRESS_RELEASED,
    REVLINK_POWER_BUTTON_SHUTDOWN_REQUESTED,
} revlink_power_button_action_t;

typedef struct {
    uint32_t debounce_ms;
    uint32_t shutdown_hold_ms;
} revlink_power_button_config_t;

typedef struct {
    revlink_power_button_config_t config;
    uint32_t candidate_ms;
    uint32_t pressed_ms;
    bool candidate_pressed;
    bool stable_pressed;
    bool shutdown_latched;
} revlink_power_button_t;

bool revlink_power_button_init(
    revlink_power_button_t *button,
    const revlink_power_button_config_t *config
);

revlink_power_button_action_t revlink_power_button_update(
    revlink_power_button_t *button,
    bool raw_pressed,
    uint32_t elapsed_ms
);

#ifdef __cplusplus
}
#endif

#endif
