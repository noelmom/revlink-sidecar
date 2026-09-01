#include "revlink_update_health.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static revlink_update_health_observation_t healthy_observation(void)
{
    return (revlink_update_health_observation_t){
        .nvs = REVLINK_HEALTH_READY,
        .storage = REVLINK_HEALTH_READY,
        .display = REVLINK_HEALTH_READY,
        .network = REVLINK_HEALTH_READY,
        .portal = REVLINK_HEALTH_READY,
        .usb = REVLINK_HEALTH_READY,
        .safety_policy = REVLINK_HEALTH_READY,
    };
}

static revlink_update_health_gate_t make_gate(bool display_required)
{
    revlink_update_health_gate_t gate;
    const revlink_update_health_config_t config = {
        .timeout_ms = 30000U,
        .display_required = display_required,
    };
    assert(revlink_update_health_init(&gate, &config));
    return gate;
}

static void test_invalid_configuration(void)
{
    revlink_update_health_gate_t gate;
    assert(!revlink_update_health_init(&gate, NULL));
    assert(!revlink_update_health_init(
        NULL,
        &(revlink_update_health_config_t){.timeout_ms = 1U}
    ));
    assert(!revlink_update_health_init(
        &gate,
        &(revlink_update_health_config_t){.timeout_ms = 0U}
    ));
}

static void test_all_required_services_become_ready(void)
{
    revlink_update_health_gate_t gate = make_gate(true);
    revlink_update_health_observation_t observation =
        healthy_observation();
    observation.portal = REVLINK_HEALTH_UNKNOWN;

    revlink_update_health_snapshot_t snapshot =
        revlink_update_health_observe(&gate, &observation, 1000U);
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_WAITING);
    assert(snapshot.blocker == REVLINK_UPDATE_BLOCKER_PORTAL);
    assert(snapshot.elapsed_ms == 1000U);

    observation.portal = REVLINK_HEALTH_READY;
    snapshot = revlink_update_health_observe(
        &gate,
        &observation,
        500U
    );
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_READY);
    assert(snapshot.blocker == REVLINK_UPDATE_BLOCKER_NONE);
    assert(snapshot.elapsed_ms == 1500U);
    assert(strcmp(
        revlink_update_health_state_name(snapshot.state),
        "ready"
    ) == 0);
}

static void test_recoverable_storage_and_optional_display(void)
{
    revlink_update_health_gate_t gate = make_gate(false);
    revlink_update_health_observation_t observation =
        healthy_observation();
    observation.storage = REVLINK_HEALTH_RECOVERABLE;
    observation.display = REVLINK_HEALTH_FAILED;

    const revlink_update_health_snapshot_t snapshot =
        revlink_update_health_observe(&gate, &observation, 10U);
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_READY);
}

static void test_required_display_and_timeout(void)
{
    revlink_update_health_gate_t gate = make_gate(true);
    revlink_update_health_observation_t observation =
        healthy_observation();
    observation.display = REVLINK_HEALTH_UNKNOWN;

    revlink_update_health_snapshot_t snapshot =
        revlink_update_health_observe(&gate, &observation, 29999U);
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_WAITING);
    assert(snapshot.blocker == REVLINK_UPDATE_BLOCKER_DISPLAY);

    snapshot = revlink_update_health_observe(
        &gate,
        &observation,
        1U
    );
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_TIMED_OUT);
    assert(snapshot.blocker == REVLINK_UPDATE_BLOCKER_DISPLAY);

    observation.display = REVLINK_HEALTH_READY;
    snapshot = revlink_update_health_observe(
        &gate,
        &observation,
        1U
    );
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_TIMED_OUT);
}

static void test_safety_violation_is_immediate_and_terminal(void)
{
    revlink_update_health_gate_t gate = make_gate(false);
    revlink_update_health_observation_t observation =
        healthy_observation();
    observation.nvs = REVLINK_HEALTH_UNKNOWN;
    observation.safety_policy = REVLINK_HEALTH_FAILED;

    revlink_update_health_snapshot_t snapshot =
        revlink_update_health_observe(&gate, &observation, 0U);
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_REJECTED);
    assert(snapshot.blocker == REVLINK_UPDATE_BLOCKER_SAFETY_POLICY);
    assert(strcmp(
        revlink_update_blocker_name(snapshot.blocker),
        "safety-policy"
    ) == 0);

    observation = healthy_observation();
    snapshot = revlink_update_health_observe(
        &gate,
        &observation,
        1U
    );
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_REJECTED);
}

static void test_invalid_observation_is_rejected(void)
{
    revlink_update_health_gate_t gate = make_gate(false);
    revlink_update_health_observation_t observation =
        healthy_observation();
    observation.usb = (revlink_health_value_t)99;

    const revlink_update_health_snapshot_t snapshot =
        revlink_update_health_observe(&gate, &observation, 0U);
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_REJECTED);
    assert(snapshot.blocker == REVLINK_UPDATE_BLOCKER_NONE);
}

static void test_elapsed_time_saturates(void)
{
    revlink_update_health_gate_t gate;
    assert(revlink_update_health_init(
        &gate,
        &(revlink_update_health_config_t){
            .timeout_ms = UINT32_MAX,
        }
    ));
    revlink_update_health_observation_t observation =
        healthy_observation();
    observation.network = REVLINK_HEALTH_UNKNOWN;

    revlink_update_health_snapshot_t snapshot =
        revlink_update_health_observe(
            &gate,
            &observation,
            UINT32_MAX - 1U
        );
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_WAITING);
    snapshot = revlink_update_health_observe(
        &gate,
        &observation,
        100U
    );
    assert(snapshot.elapsed_ms == UINT32_MAX);
    assert(snapshot.state == REVLINK_UPDATE_HEALTH_TIMED_OUT);
}

int main(void)
{
    test_invalid_configuration();
    test_all_required_services_become_ready();
    test_recoverable_storage_and_optional_display();
    test_required_display_and_timeout();
    test_safety_violation_is_immediate_and_terminal();
    test_invalid_observation_is_rejected();
    test_elapsed_time_saturates();
    puts("revlink update-health host tests passed");
    return 0;
}
