#include "revlink_network_coordinator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static revlink_network_coordinator_t make_coordinator(void)
{
    revlink_network_coordinator_t coordinator;
    assert(revlink_network_coordinator_init(
        &coordinator,
        &(revlink_network_config_t){
            .startup_timeout_ms = 20000U,
            .startup_attempt_limit = 2U,
            .reconnect_timeout_ms = 10000U,
            .health_probe_interval_ms = 15000U,
            .health_failure_threshold = 3U,
        }
    ));
    return coordinator;
}

static revlink_network_action_t handle(
    revlink_network_coordinator_t *coordinator,
    revlink_network_event_t event,
    revlink_network_status_t expected_status
)
{
    revlink_network_action_t action = {
        .kind = REVLINK_NETWORK_ACTION_STOP_RADIO,
        .network_id = UINT32_MAX,
    };
    assert(
        revlink_network_coordinator_handle(coordinator, &event, &action)
            == expected_status
    );
    return action;
}

static void start_and_select(
    revlink_network_coordinator_t *coordinator,
    revlink_network_id_t network_id
)
{
    revlink_network_action_t action = handle(
        coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_START,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_SCAN_SAVED);

    action = handle(
        coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_SCAN_SELECTED,
            .network_id = network_id,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_CONNECT_SAVED);
    assert(action.network_id == network_id);
}

static void connect_client(
    revlink_network_coordinator_t *coordinator,
    revlink_network_id_t network_id
)
{
    start_and_select(coordinator, network_id);
    const revlink_network_action_t action = handle(
        coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_CLIENT_CONNECTED,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    const revlink_network_snapshot_t snapshot =
        revlink_network_coordinator_snapshot(coordinator);
    assert(snapshot.state == REVLINK_NETWORK_CLIENT_READY);
    assert(snapshot.selected_network_id == network_id);
}

static void test_invalid_configuration(void)
{
    revlink_network_coordinator_t coordinator;
    assert(!revlink_network_coordinator_init(&coordinator, NULL));
    assert(!revlink_network_coordinator_init(
        &coordinator,
        &(revlink_network_config_t){
            .startup_timeout_ms = 0U,
            .startup_attempt_limit = 2U,
            .reconnect_timeout_ms = 10000U,
            .health_probe_interval_ms = 15000U,
            .health_failure_threshold = 3U,
        }
    ));
    assert(!revlink_network_coordinator_init(
        &coordinator,
        &(revlink_network_config_t){
            .startup_timeout_ms = 20000U,
            .startup_attempt_limit = 0U,
            .reconnect_timeout_ms = 10000U,
            .health_probe_interval_ms = 15000U,
            .health_failure_threshold = 3U,
        }
    ));
    assert(!revlink_network_coordinator_init(
        &coordinator,
        &(revlink_network_config_t){
            .startup_timeout_ms = 20000U,
            .startup_attempt_limit = 2U,
            .reconnect_timeout_ms = 10000U,
            .health_probe_interval_ms = 0U,
            .health_failure_threshold = 3U,
        }
    ));
}

static void test_first_boot_falls_back_to_hotspot(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_START,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_SCAN_SAVED);

    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_SCAN_EMPTY,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_START_HOTSPOT);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).state
            == REVLINK_NETWORK_HOTSPOT_STARTING
    );

    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_HOTSPOT_STARTED,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).state
            == REVLINK_NETWORK_HOTSPOT_READY
    );
}

static void test_saved_network_is_sticky_while_healthy(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    connect_client(&coordinator, 7U);

    revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TICK,
            .elapsed_ms = 15000U,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_PROBE_CLIENT);
    assert(action.network_id == 7U);
    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_CLIENT_HEALTHY,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).state
            == REVLINK_NETWORK_CLIENT_READY
    );
}

static void test_stale_client_requires_three_failures_then_recovers(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    connect_client(&coordinator, 29U);

    for (uint32_t failure = 1U; failure <= 3U; ++failure) {
        const revlink_network_action_t probe = handle(
            &coordinator,
            (revlink_network_event_t){
                .kind = REVLINK_NETWORK_EVENT_TICK,
                .elapsed_ms = 15000U,
            },
            REVLINK_NETWORK_OK
        );
        assert(probe.kind == REVLINK_NETWORK_ACTION_PROBE_CLIENT);

        const revlink_network_action_t recovery = handle(
            &coordinator,
            (revlink_network_event_t){
                .kind = REVLINK_NETWORK_EVENT_CLIENT_UNHEALTHY,
                .platform_error = -30,
            },
            REVLINK_NETWORK_OK
        );
        assert(
            recovery.kind
                == (failure < 3U
                    ? REVLINK_NETWORK_ACTION_NONE
                    : REVLINK_NETWORK_ACTION_RECOVER_SAVED)
        );
        if (failure == 3U) {
            assert(recovery.network_id == 29U);
        }
    }
    const revlink_network_snapshot_t snapshot =
        revlink_network_coordinator_snapshot(&coordinator);
    assert(snapshot.state == REVLINK_NETWORK_RECONNECTING);
    assert(snapshot.consecutive_health_failures == 3U);
}

static void test_transfer_pauses_health_probes(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    connect_client(&coordinator, 31U);
    (void)handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TRANSFER_STARTED,
        },
        REVLINK_NETWORK_OK
    );
    const revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TICK,
            .elapsed_ms = UINT32_MAX,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).health_elapsed_ms
            == 0U
    );
}

static void test_lost_client_retries_same_network_then_falls_back(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    connect_client(&coordinator, 42U);

    revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_CLIENT_LOST,
            .platform_error = -9,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_CONNECT_SAVED);
    assert(action.network_id == 42U);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).state
            == REVLINK_NETWORK_RECONNECTING
    );
    assert(
        revlink_network_coordinator_snapshot(&coordinator).phase_timeout_ms
            == 10000U
    );

    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_CLIENT_FAILED,
            .platform_error = -10,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).state
            == REVLINK_NETWORK_RECONNECTING
    );

    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TICK,
            .elapsed_ms = 9999U,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TICK,
            .elapsed_ms = 1U,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_START_HOTSPOT);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).selected_network_id
            == 0U
    );
}

static void test_startup_timeout_is_bounded(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    start_and_select(&coordinator, 5U);

    const revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TICK,
            .elapsed_ms = 20000U,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_START_HOTSPOT);
}

static void test_startup_join_retries_once_then_falls_back(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    start_and_select(&coordinator, 73U);

    revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_CLIENT_FAILED,
            .platform_error = -203,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_RECOVER_SAVED);
    assert(action.network_id == 73U);
    revlink_network_snapshot_t snapshot =
        revlink_network_coordinator_snapshot(&coordinator);
    assert(snapshot.state == REVLINK_NETWORK_CONNECTING);
    assert(snapshot.startup_attempt_count == 2U);

    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_CLIENT_FAILED,
            .platform_error = -203,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_START_HOTSPOT);
    snapshot = revlink_network_coordinator_snapshot(&coordinator);
    assert(snapshot.state == REVLINK_NETWORK_HOTSPOT_STARTING);
    assert(snapshot.selected_network_id == 0U);
}

static void test_transfer_lock_blocks_user_network_switches(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    connect_client(&coordinator, 11U);

    revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TRANSFER_STARTED,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);

    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_FORCE_HOTSPOT,
        },
        REVLINK_NETWORK_TRANSFER_LOCKED
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);
    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_RETRY_SAVED,
        },
        REVLINK_NETWORK_TRANSFER_LOCKED
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_NONE);

    (void)handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TRANSFER_FINISHED,
        },
        REVLINK_NETWORK_OK
    );
    action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_FORCE_HOTSPOT,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_START_HOTSPOT);
}

static void test_hotspot_can_retry_saved_networks_explicitly(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    (void)handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_START,
        },
        REVLINK_NETWORK_OK
    );
    (void)handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_SCAN_EMPTY,
        },
        REVLINK_NETWORK_OK
    );
    (void)handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_HOTSPOT_STARTED,
        },
        REVLINK_NETWORK_OK
    );

    const revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_RETRY_SAVED,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_SCAN_SAVED);
    assert(
        revlink_network_coordinator_snapshot(&coordinator).state
            == REVLINK_NETWORK_SEARCHING
    );
}

static void test_stop_clears_transient_state(void)
{
    revlink_network_coordinator_t coordinator = make_coordinator();
    connect_client(&coordinator, 19U);
    (void)handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_TRANSFER_STARTED,
        },
        REVLINK_NETWORK_OK
    );

    const revlink_network_action_t action = handle(
        &coordinator,
        (revlink_network_event_t){
            .kind = REVLINK_NETWORK_EVENT_STOP,
        },
        REVLINK_NETWORK_OK
    );
    assert(action.kind == REVLINK_NETWORK_ACTION_STOP_RADIO);
    const revlink_network_snapshot_t snapshot =
        revlink_network_coordinator_snapshot(&coordinator);
    assert(snapshot.state == REVLINK_NETWORK_STOPPED);
    assert(snapshot.selected_network_id == 0U);
    assert(snapshot.phase_timeout_ms == 0U);
    assert(snapshot.health_elapsed_ms == 0U);
    assert(snapshot.consecutive_health_failures == 0U);
    assert(!snapshot.transfer_active);
}

int main(void)
{
    test_invalid_configuration();
    test_first_boot_falls_back_to_hotspot();
    test_saved_network_is_sticky_while_healthy();
    test_stale_client_requires_three_failures_then_recovers();
    test_transfer_pauses_health_probes();
    test_lost_client_retries_same_network_then_falls_back();
    test_startup_timeout_is_bounded();
    test_startup_join_retries_once_then_falls_back();
    test_transfer_lock_blocks_user_network_switches();
    test_hotspot_can_retry_saved_networks_explicitly();
    test_stop_clears_transient_state();
    puts("revlink network-coordinator host tests passed");
    return 0;
}
