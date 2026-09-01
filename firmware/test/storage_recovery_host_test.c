#include "revlink_storage_recovery.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define CONFIRMATION_TIMEOUT_MS 20000U

static void test_missing_card_never_offers_format(void)
{
    revlink_storage_recovery_t recovery;
    assert(revlink_storage_recovery_init(
        &recovery,
        false,
        CONFIRMATION_TIMEOUT_MS
    ));
    assert(
        recovery.state == REVLINK_STORAGE_RECOVERY_UNAVAILABLE
    );
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_NO_ACTION
    );
}

static void test_format_requires_two_separate_double_presses(void)
{
    revlink_storage_recovery_t recovery;
    assert(revlink_storage_recovery_init(
        &recovery,
        true,
        CONFIRMATION_TIMEOUT_MS
    ));
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_SHOW_WARNING
    );
    assert(
        recovery.state
            == REVLINK_STORAGE_RECOVERY_AWAITING_CONFIRMATION
    );
    assert(
        revlink_storage_recovery_seconds_remaining(&recovery) == 20U
    );
    assert(
        revlink_storage_recovery_tick(&recovery, 1250U)
            == REVLINK_STORAGE_RECOVERY_NO_ACTION
    );
    assert(
        revlink_storage_recovery_seconds_remaining(&recovery) == 19U
    );
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_FORMAT_REQUESTED
    );
    assert(recovery.state == REVLINK_STORAGE_RECOVERY_FORMATTING);
}

static void test_warning_times_out_and_returns_to_step_one(void)
{
    revlink_storage_recovery_t recovery;
    assert(revlink_storage_recovery_init(
        &recovery,
        true,
        CONFIRMATION_TIMEOUT_MS
    ));
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_SHOW_WARNING
    );
    assert(
        revlink_storage_recovery_tick(
            &recovery,
            CONFIRMATION_TIMEOUT_MS - 1U
        ) == REVLINK_STORAGE_RECOVERY_NO_ACTION
    );
    assert(
        revlink_storage_recovery_seconds_remaining(&recovery) == 1U
    );
    assert(
        revlink_storage_recovery_tick(&recovery, 1U)
            == REVLINK_STORAGE_RECOVERY_WARNING_TIMED_OUT
    );
    assert(
        recovery.state
            == REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST
    );
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_SHOW_WARNING
    );
}

static void test_format_failure_rearms_step_one(void)
{
    revlink_storage_recovery_t recovery;
    assert(revlink_storage_recovery_init(
        &recovery,
        true,
        CONFIRMATION_TIMEOUT_MS
    ));
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_SHOW_WARNING
    );
    assert(
        revlink_storage_recovery_double_press(&recovery)
            == REVLINK_STORAGE_RECOVERY_FORMAT_REQUESTED
    );
    revlink_storage_recovery_format_failed(&recovery);
    assert(
        recovery.state
            == REVLINK_STORAGE_RECOVERY_AWAITING_REQUEST
    );
}

static void test_invalid_inputs_are_safe(void)
{
    revlink_storage_recovery_t recovery;
    assert(!revlink_storage_recovery_init(
        NULL,
        true,
        CONFIRMATION_TIMEOUT_MS
    ));
    assert(!revlink_storage_recovery_init(&recovery, true, 0U));
    assert(
        revlink_storage_recovery_double_press(NULL)
            == REVLINK_STORAGE_RECOVERY_NO_ACTION
    );
    assert(
        revlink_storage_recovery_tick(NULL, 1U)
            == REVLINK_STORAGE_RECOVERY_NO_ACTION
    );
    assert(revlink_storage_recovery_seconds_remaining(NULL) == 0U);
}

int main(void)
{
    test_missing_card_never_offers_format();
    test_format_requires_two_separate_double_presses();
    test_warning_times_out_and_returns_to_step_one();
    test_format_failure_rearms_step_one();
    test_invalid_inputs_are_safe();
    puts("revlink storage-recovery host tests passed");
    return 0;
}
