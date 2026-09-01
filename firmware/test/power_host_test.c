#include "revlink_power_button.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static revlink_power_button_t make_button(void)
{
    revlink_power_button_t button;
    const revlink_power_button_config_t config = {
        .debounce_ms = 50U,
        .shutdown_hold_ms = 2000U,
    };
    assert(revlink_power_button_init(&button, &config));
    return button;
}

static revlink_power_button_action_t sample(
    revlink_power_button_t *button,
    bool pressed,
    uint32_t count
)
{
    revlink_power_button_action_t action = REVLINK_POWER_BUTTON_NO_ACTION;
    for (uint32_t index = 0; index < count; ++index) {
        const revlink_power_button_action_t current =
            revlink_power_button_update(button, pressed, 25U);
        if (current != REVLINK_POWER_BUTTON_NO_ACTION) {
            assert(action == REVLINK_POWER_BUTTON_NO_ACTION);
            action = current;
        }
    }
    return action;
}

static void test_invalid_configuration(void)
{
    revlink_power_button_t button;
    assert(!revlink_power_button_init(&button, NULL));
    assert(!revlink_power_button_init(
        NULL,
        &(revlink_power_button_config_t){
            .debounce_ms = 50U,
            .shutdown_hold_ms = 2000U,
        }
    ));
    assert(!revlink_power_button_init(
        &button,
        &(revlink_power_button_config_t){
            .debounce_ms = 0U,
            .shutdown_hold_ms = 2000U,
        }
    ));
}

static void test_bounce_is_ignored_and_short_press_is_reported(void)
{
    revlink_power_button_t button = make_button();
    assert(sample(&button, true, 1U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(sample(&button, false, 1U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(sample(&button, true, 1U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(sample(&button, false, 2U) == REVLINK_POWER_BUTTON_NO_ACTION);

    assert(sample(&button, true, 2U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(sample(&button, true, 40U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(
        sample(&button, false, 2U)
            == REVLINK_POWER_BUTTON_SHORT_PRESS_RELEASED
    );
}

static void test_long_press_triggers_once_and_rearms(void)
{
    revlink_power_button_t button = make_button();
    assert(sample(&button, true, 2U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(sample(&button, true, 79U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(
        sample(&button, true, 1U)
            == REVLINK_POWER_BUTTON_SHUTDOWN_REQUESTED
    );
    assert(sample(&button, true, 100U) == REVLINK_POWER_BUTTON_NO_ACTION);

    assert(sample(&button, false, 2U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(sample(&button, true, 2U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(
        sample(&button, true, 80U)
            == REVLINK_POWER_BUTTON_SHUTDOWN_REQUESTED
    );
}

static void test_elapsed_time_saturates_safely(void)
{
    revlink_power_button_t button = make_button();
    assert(sample(&button, true, 2U) == REVLINK_POWER_BUTTON_NO_ACTION);
    assert(
        revlink_power_button_update(&button, true, UINT32_MAX)
            == REVLINK_POWER_BUTTON_SHUTDOWN_REQUESTED
    );
    assert(
        revlink_power_button_update(&button, true, UINT32_MAX)
            == REVLINK_POWER_BUTTON_NO_ACTION
    );
}

int main(void)
{
    test_invalid_configuration();
    test_bounce_is_ignored_and_short_press_is_reported();
    test_long_press_triggers_once_and_rearms();
    test_elapsed_time_saturates_safely();
    puts("revlink power-button host tests passed");
    return 0;
}
