#include "revlink_power_button.h"

#include <limits.h>
#include <string.h>

static uint32_t saturated_add(uint32_t value, uint32_t increment)
{
    return increment > UINT32_MAX - value
        ? UINT32_MAX
        : value + increment;
}

bool revlink_power_button_init(
    revlink_power_button_t *button,
    const revlink_power_button_config_t *config
)
{
    if (button == NULL || config == NULL
        || config->debounce_ms == 0U
        || config->shutdown_hold_ms == 0U) {
        return false;
    }

    memset(button, 0, sizeof(*button));
    button->config = *config;
    return true;
}

revlink_power_button_action_t revlink_power_button_update(
    revlink_power_button_t *button,
    bool raw_pressed,
    uint32_t elapsed_ms
)
{
    if (button == NULL || elapsed_ms == 0U) {
        return REVLINK_POWER_BUTTON_NO_ACTION;
    }

    if (raw_pressed != button->candidate_pressed) {
        button->candidate_pressed = raw_pressed;
        button->candidate_ms = elapsed_ms;
    } else {
        button->candidate_ms =
            saturated_add(button->candidate_ms, elapsed_ms);
    }

    bool stable_changed = false;
    bool short_press_released = false;
    if (button->candidate_pressed != button->stable_pressed
        && button->candidate_ms >= button->config.debounce_ms) {
        button->stable_pressed = button->candidate_pressed;
        stable_changed = true;
        if (!button->stable_pressed) {
            short_press_released = !button->shutdown_latched;
            button->shutdown_latched = false;
        }
        button->pressed_ms = 0U;
    }

    if (button->stable_pressed && !stable_changed) {
        button->pressed_ms =
            saturated_add(button->pressed_ms, elapsed_ms);
    }

    if (button->stable_pressed && !button->shutdown_latched
        && button->pressed_ms >= button->config.shutdown_hold_ms) {
        button->shutdown_latched = true;
        return REVLINK_POWER_BUTTON_SHUTDOWN_REQUESTED;
    }
    if (short_press_released) {
        return REVLINK_POWER_BUTTON_SHORT_PRESS_RELEASED;
    }
    return REVLINK_POWER_BUTTON_NO_ACTION;
}
