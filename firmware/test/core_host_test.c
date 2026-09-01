#include <stdio.h>
#include <stdlib.h>

#include "revlink_application.h"

static int observer_calls;
static int sync_requests;
static int sync_recoveries;
static int sync_cancels;
static int sync_observer_calls;
static int sync_request_failures_remaining;

static revlink_sync_status_t request_sync(void *context)
{
    (void)context;
    ++sync_requests;
    if (sync_request_failures_remaining > 0) {
        --sync_request_failures_remaining;
        return REVLINK_SYNC_TRANSPORT_ERROR;
    }
    return REVLINK_SYNC_OK;
}

static revlink_sync_status_t recover_session(void *context)
{
    (void)context;
    ++sync_recoveries;
    return REVLINK_SYNC_OK;
}

static revlink_sync_status_t cancel_sync(void *context)
{
    (void)context;
    ++sync_cancels;
    return REVLINK_SYNC_OK;
}

static void count_sync_observer(
    void *context,
    const revlink_sync_snapshot_t *snapshot
)
{
    (void)context;
    (void)snapshot;
    ++sync_observer_calls;
}

static void count_observer(
    void *context,
    const revlink_device_snapshot_t *snapshot
)
{
    (void)context;
    (void)snapshot;
    ++observer_calls;
}

static void require_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "core host test failed: %s\n", message);
        exit(1);
    }
}

static revlink_device_identity_t topology_identity(
    uint8_t address,
    uint32_t attachment_generation
)
{
    return (revlink_device_identity_t){
        .vendor_id = 0x1a84,
        .product_id = 0x0121,
        .address = address,
        .configuration_count = 4U,
        .matching_configuration_count = 1U,
        .interface_number = 0U,
        .bulk_out_endpoint = 0x03U,
        .bulk_in_endpoint = 0x82U,
        .bulk_max_packet_size = 512U,
        .attachment_generation = attachment_generation,
        .high_speed = true,
    };
}

static void test_bounded_auto_sync_retry(void)
{
    const int request_base = sync_requests;
    revlink_device_identity_t identity = topology_identity(9U, 90U);
    revlink_device_event_t event = {
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
    };
    const revlink_application_config_t config = {
        .sync_policy = {
            .auto_sync_on_attach = true,
        },
        .sync_request = request_sync,
        .sync_cancel = cancel_sync,
    };

    revlink_application_t queue_race = {0};
    require_true(
        revlink_application_init(&queue_race, &config) == REVLINK_CORE_OK,
        "auto-retry queue-race init"
    );
    require_true(
        revlink_application_handle_device_event(&queue_race, &event)
            == REVLINK_CORE_OK,
        "auto-retry queue-race monitor"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = identity,
    };
    require_true(
        revlink_application_handle_device_event(&queue_race, &event)
            == REVLINK_CORE_OK,
        "auto-retry queue-race attach"
    );
    sync_request_failures_remaining = 1;
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&queue_race, &event)
            == REVLINK_CORE_OK
            && sync_requests == request_base + 1,
        "first attach-time auto-sync request may fail transiently"
    );
    require_true(
        revlink_application_auto_sync_retry_needed(&queue_race),
        "transient queue failure exposes one bounded retry"
    );
    require_true(
        revlink_application_retry_auto_sync(&queue_race) == REVLINK_SYNC_OK
            && sync_requests == request_base + 2,
        "bounded queue-race retry succeeds"
    );
    require_true(
        !revlink_application_auto_sync_retry_needed(&queue_race),
        "successful queue-race retry consumes the retry budget"
    );

    revlink_application_t early_failure = {0};
    identity.address = 10U;
    identity.attachment_generation = 91U;
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
    };
    require_true(
        revlink_application_init(&early_failure, &config) == REVLINK_CORE_OK,
        "auto-retry early-failure init"
    );
    require_true(
        revlink_application_handle_device_event(&early_failure, &event)
            == REVLINK_CORE_OK,
        "auto-retry early-failure monitor"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = identity,
    };
    require_true(
        revlink_application_handle_device_event(&early_failure, &event)
            == REVLINK_CORE_OK,
        "auto-retry early-failure attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&early_failure, &event)
            == REVLINK_CORE_OK,
        "auto-retry early-failure acceptance"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&early_failure, &event)
            == REVLINK_CORE_OK,
        "auto-retry early-failure session open"
    );
    revlink_sync_event_t sync_event = {
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&early_failure, &sync_event)
            == REVLINK_SYNC_OK,
        "auto-retry early-failure sync start"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&early_failure, &event)
            == REVLINK_CORE_OK,
        "auto-retry early-failure session close"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_FAILED,
        .platform_error = -1,
    };
    require_true(
        revlink_application_handle_sync_event(&early_failure, &sync_event)
            == REVLINK_SYNC_OK
            && revlink_application_auto_sync_retry_needed(&early_failure),
        "zero-byte early transport failure exposes one bounded retry"
    );
    require_true(
        revlink_application_retry_auto_sync(&early_failure)
            == REVLINK_SYNC_OK,
        "bounded early transport retry succeeds"
    );
}

static void require_conflict_for_enumeration_order(
    uint8_t first_address,
    uint8_t second_address,
    uint32_t revision_base
)
{
    revlink_application_t application = {0};
    const revlink_application_config_t config = {
        .sync_request = request_sync,
        .sync_cancel = cancel_sync,
    };
    require_true(
        revlink_application_init(&application, &config)
            == REVLINK_CORE_OK,
        "topology-order application init"
    );
    revlink_device_event_t event = {
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "topology-order monitor start"
    );

    const revlink_device_identity_t first =
        topology_identity(first_address, revision_base);
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = first,
        .eligible_device_count = 1U,
        .topology_revision = revision_base,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "topology-order first attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "topology-order first accept"
    );

    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED,
        .identity = topology_identity(
            second_address,
            revision_base + 1U
        ),
        .eligible_device_count = 2U,
        .topology_revision = revision_base + 1U,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "topology-order second device enters conflict"
    );
    const revlink_device_snapshot_t snapshot =
        revlink_device_service_snapshot(&application.device_service);
    require_true(
        snapshot.state == REVLINK_DEVICE_CONFLICT
            && snapshot.eligible_device_count == 2U
            && snapshot.conflict_recovery_required
            && snapshot.topology_revision == revision_base + 1U,
        "enumeration order produces the same fail-closed conflict"
    );
    require_true(
        revlink_application_request_sync(&application)
            == REVLINK_SYNC_INVALID_STATE,
        "enumeration-order conflict blocks manual sync"
    );
}

static void test_topology_revision_safety(void)
{
    require_conflict_for_enumeration_order(1U, 7U, 100U);
    require_conflict_for_enumeration_order(7U, 1U, 200U);

    revlink_application_t application = {0};
    const revlink_application_config_t config = {
        .sync_request = request_sync,
        .sync_cancel = cancel_sync,
    };
    require_true(
        revlink_application_init(&application, &config)
            == REVLINK_CORE_OK,
        "stale-topology application init"
    );
    revlink_device_event_t event = {
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "stale-topology monitor start"
    );
    const revlink_device_identity_t first =
        topology_identity(2U, 41U);
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = first,
        .eligible_device_count = 1U,
        .topology_revision = 10U,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "stale-topology first attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "stale-topology first accept"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED,
        .identity = topology_identity(6U, 42U),
        .eligible_device_count = 2U,
        .topology_revision = 11U,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "stale-topology conflict"
    );

    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_DETACHED,
        .identity = first,
        .eligible_device_count = 0U,
        .topology_revision = 10U,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_INVALID_TRANSITION,
        "stale detach revision is rejected"
    );
    revlink_device_snapshot_t snapshot =
        revlink_device_service_snapshot(&application.device_service);
    require_true(
        snapshot.state == REVLINK_DEVICE_CONFLICT
            && snapshot.eligible_device_count == 2U
            && snapshot.topology_revision == 11U,
        "stale detach cannot clear a newer conflict"
    );

    event.topology_revision = 12U;
    event.eligible_device_count = 1U;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "current first detach revision is accepted"
    );
    snapshot = revlink_device_service_snapshot(
        &application.device_service
    );
    require_true(
        snapshot.state == REVLINK_DEVICE_CONFLICT
            && snapshot.eligible_device_count == 1U
            && snapshot.topology_revision == 12U,
        "one stable remaining device does not resume work"
    );

    event.identity = topology_identity(6U, 42U);
    event.topology_revision = 13U;
    event.eligible_device_count = 0U;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "current zero-device detach clears conflict"
    );
    snapshot = revlink_device_service_snapshot(
        &application.device_service
    );
    require_true(
        snapshot.state == REVLINK_DEVICE_WAITING
            && !snapshot.conflict_recovery_required
            && snapshot.topology_revision == 13U,
        "complete current topology detach permits deliberate recovery"
    );

    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED,
        .eligible_device_count = 2U,
        .topology_revision = 12U,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_INVALID_TRANSITION,
        "late conflict event from an older revision is rejected"
    );
    snapshot = revlink_device_service_snapshot(
        &application.device_service
    );
    require_true(
        snapshot.state == REVLINK_DEVICE_WAITING
            && snapshot.topology_revision == 13U,
        "late stale event cannot replace the newer zero-device topology"
    );
}

int main(void)
{
    revlink_application_t application = {0};
    const revlink_application_config_t config = {
        .allow_device_writes = false,
        .allow_device_deletes = false,
        .state_observer = count_observer,
        .sync_request = request_sync,
        .sync_cancel = cancel_sync,
        .sync_observer = count_sync_observer,
    };
    require_true(
        revlink_application_init(&application, &config) == REVLINK_CORE_OK,
        "application init"
    );
    require_true(
        revlink_application_protocol_self_test(),
        "protocol self-test"
    );
    require_true(
        revlink_application_authorize(
            &application,
            REVLINK_OPERATION_READ_DEVICE
        ) == REVLINK_CORE_OK,
        "read must be allowed"
    );
    require_true(
        revlink_application_authorize(
            &application,
            REVLINK_OPERATION_WRITE_MAP
        ) == REVLINK_CORE_NOT_AUTHORIZED,
        "map write must default to denied"
    );
    require_true(
        revlink_application_authorize(
            &application,
            REVLINK_OPERATION_WRITE_STARTUP_SCREEN
        ) == REVLINK_CORE_NOT_AUTHORIZED,
        "startup-screen write must default to denied"
    );
    require_true(
        revlink_application_authorize(
            &application,
            REVLINK_OPERATION_DELETE_DEVICE_FILE
        ) == REVLINK_CORE_NOT_AUTHORIZED,
        "delete must default to denied"
    );

    revlink_application_t write_enabled_application = {0};
    const revlink_application_config_t write_enabled_config = {
        .allow_device_writes = true,
        .allow_device_deletes = false,
        .sync_request = request_sync,
        .sync_cancel = cancel_sync,
    };
    require_true(
        revlink_application_init(
            &write_enabled_application,
            &write_enabled_config
        ) == REVLINK_CORE_OK,
        "write-enabled application init"
    );
    require_true(
        revlink_application_authorize(
            &write_enabled_application,
            REVLINK_OPERATION_WRITE_MAP
        ) == REVLINK_CORE_OK,
        "explicit capability must permit map writes"
    );
    require_true(
        revlink_application_authorize(
            &write_enabled_application,
            REVLINK_OPERATION_DELETE_DEVICE_FILE
        ) == REVLINK_CORE_NOT_AUTHORIZED,
        "delete requires a separate capability"
    );

    const revlink_device_identity_t identity = {
        .vendor_id = 0x1a84,
        .product_id = 0x0121,
        .address = 3,
        .configuration_count = 4,
        .matching_configuration_count = 4,
        .interface_number = 0,
        .bulk_out_endpoint = 0x03,
        .bulk_in_endpoint = 0x82,
        .bulk_max_packet_size = 512,
        .high_speed = true,
    };
    revlink_device_event_t event = {
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = identity,
    };
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_INVALID_TRANSITION,
        "attach before monitor start must be rejected"
    );

    event.kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "monitor start"
    );
    event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "device attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "device acceptance"
    );
    require_true(sync_requests == 0, "auto-sync must default off");
    require_true(
        revlink_application_request_sync(&application) == REVLINK_SYNC_OK,
        "manual sync request"
    );
    require_true(sync_requests == 1, "manual request transport call");
    revlink_sync_event_t sync_event = {
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&application, &sync_event)
            == REVLINK_SYNC_OK,
        "sync started"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_PROGRESS,
        .candidates = 6,
        .downloaded = 2,
        .skipped = 3,
        .downloaded_bytes = 1234,
        .pending = 1,
    };
    require_true(
        revlink_application_handle_sync_event(&application, &sync_event)
            == REVLINK_SYNC_OK,
        "sync progress"
    );
    sync_event.kind = REVLINK_SYNC_EVENT_COMPLETED;
    sync_event.data_phase_completed = true;
    sync_event.session_close_sent = true;
    sync_event.session_close_acknowledged = true;
    require_true(
        revlink_application_handle_sync_event(&application, &sync_event)
            == REVLINK_SYNC_OK,
        "sync completed"
    );
    const revlink_sync_snapshot_t sync_snapshot =
        revlink_application_sync_snapshot(&application);
    require_true(
        sync_snapshot.state == REVLINK_SYNC_COMPLETED
            && sync_snapshot.downloaded == 2
            && sync_snapshot.skipped == 3
            && sync_snapshot.data_phase_completed
            && sync_snapshot.session_close_sent
            && sync_snapshot.session_close_acknowledged,
        "sync progress snapshot"
    );
    require_true(
        revlink_application_request_sync(&application) == REVLINK_SYNC_OK,
        "repeat manual sync"
    );
    require_true(
        revlink_application_cancel_sync(&application) == REVLINK_SYNC_OK,
        "cancel queued sync"
    );
    require_true(sync_cancels == 1, "cancel transport call");
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&application, &sync_event)
            == REVLINK_SYNC_OK,
        "cancelled queued sync may start cooperatively"
    );
    sync_event.kind = REVLINK_SYNC_EVENT_CANCELLED;
    require_true(
        revlink_application_handle_sync_event(&application, &sync_event)
            == REVLINK_SYNC_OK,
        "sync cancelled"
    );
    require_true(
        revlink_application_sync_snapshot(&application).state
            == REVLINK_SYNC_CANCELLED,
        "cancelled snapshot"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "session open"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "session close"
    );
    event.kind = REVLINK_DEVICE_EVENT_DETACHED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "device detach"
    );

    const revlink_device_snapshot_t snapshot =
        revlink_device_service_snapshot(&application.device_service);
    require_true(
        snapshot.state == REVLINK_DEVICE_WAITING,
        "service must return to waiting"
    );
    require_true(
        snapshot.identity.vendor_id == 0,
        "detached identity must be cleared"
    );

    const revlink_device_identity_t second_identity = {
        .vendor_id = 0x1a84,
        .product_id = 0x0121,
        .address = 7,
        .configuration_count = 4,
        .matching_configuration_count = 4,
        .interface_number = 0,
        .bulk_out_endpoint = 0x03,
        .bulk_in_endpoint = 0x82,
        .bulk_max_packet_size = 512,
        .high_speed = true,
    };
    event.identity = second_identity;
    event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "different device attach after detach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "different device acceptance"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "different device session open"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "different device session close"
    );
    event.kind = REVLINK_DEVICE_EVENT_DETACHED;
    require_true(
        revlink_application_handle_device_event(&application, &event)
            == REVLINK_CORE_OK,
        "different device detach"
    );
    require_true(
        revlink_device_service_snapshot(&application.device_service).state
            == REVLINK_DEVICE_WAITING,
        "service must accept another attachment without restart"
    );
    require_true(observer_calls == 11, "observer event count");
    require_true(sync_observer_calls == 8, "sync observer event count");

    revlink_application_t automatic = {0};
    const revlink_application_config_t automatic_config = {
        .sync_policy = {
            .auto_sync_on_attach = true,
        },
        .sync_request = request_sync,
        .sync_cancel = cancel_sync,
    };
    require_true(
        revlink_application_init(&automatic, &automatic_config)
            == REVLINK_CORE_OK,
        "automatic application init"
    );
    event.identity = identity;
    event.kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "automatic monitor start"
    );
    event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "automatic attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "automatic acceptance"
    );
    require_true(sync_requests == 3, "one auto request on acceptance");
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "automatic session open"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&automatic, &sync_event)
            == REVLINK_SYNC_OK,
        "automatic sync start"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "automatic session close"
    );
    sync_event.kind = REVLINK_SYNC_EVENT_COMPLETED;
    require_true(
        revlink_application_handle_sync_event(&automatic, &sync_event)
            == REVLINK_SYNC_OK,
        "automatic sync completion"
    );
    require_true(sync_requests == 3, "session close must not auto-loop");

    event.kind = REVLINK_DEVICE_EVENT_DETACHED;
    event.software_reenumeration = true;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "polite disconnect re-enumeration"
    );
    event.identity = second_identity;
    event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
    event.software_reenumeration = true;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "software re-attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "software re-attach acceptance"
    );
    require_true(
        sync_requests == 3,
        "polite disconnect re-enumeration must not auto-sync again"
    );

    for (uint8_t cycle = 0; cycle < 10U; ++cycle) {
        require_true(
            revlink_application_request_sync(&automatic) == REVLINK_SYNC_OK,
            "repeat-cycle manual sync request"
        );
        event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
        event.software_reenumeration = false;
        require_true(
            revlink_application_handle_device_event(&automatic, &event)
                == REVLINK_CORE_OK,
            "repeat-cycle session open"
        );
        sync_event = (revlink_sync_event_t){
            .kind = REVLINK_SYNC_EVENT_STARTED,
        };
        require_true(
            revlink_application_handle_sync_event(&automatic, &sync_event)
                == REVLINK_SYNC_OK,
            "repeat-cycle sync start"
        );
        event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
        require_true(
            revlink_application_handle_device_event(&automatic, &event)
                == REVLINK_CORE_OK,
            "repeat-cycle session close"
        );
        sync_event = (revlink_sync_event_t){
            .kind = REVLINK_SYNC_EVENT_COMPLETED,
            .session_close_sent = true,
            .session_close_acknowledged = true,
        };
        require_true(
            revlink_application_handle_sync_event(&automatic, &sync_event)
                == REVLINK_SYNC_OK,
            "repeat-cycle sync completion"
        );

        event.kind = REVLINK_DEVICE_EVENT_DETACHED;
        event.software_reenumeration = true;
        require_true(
            revlink_application_handle_device_event(&automatic, &event)
                == REVLINK_CORE_OK,
            "repeat-cycle polite detach"
        );
        event.identity.address = (uint8_t)(5U + cycle);
        event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
        require_true(
            revlink_application_handle_device_event(&automatic, &event)
                == REVLINK_CORE_OK,
            "repeat-cycle software re-attach"
        );
        event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
        require_true(
            revlink_application_handle_device_event(&automatic, &event)
                == REVLINK_CORE_OK,
            "repeat-cycle software re-attach acceptance"
        );
        require_true(
            sync_requests == 4 + cycle,
            "repeat-cycle software re-enumeration must not auto-loop"
        );
    }

    event.kind = REVLINK_DEVICE_EVENT_DETACHED;
    event.software_reenumeration = false;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "physical detach clears attachment latch"
    );
    event.identity = identity;
    event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "physical re-attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&automatic, &event)
            == REVLINK_CORE_OK,
        "physical re-attach acceptance"
    );
    require_true(
        sync_requests == 14,
        "physical re-attach must permit one new automatic sync"
    );

    revlink_application_t recovery = {0};
    const revlink_application_config_t recovery_config = {
        .sync_request = request_sync,
        .sync_recover_session = recover_session,
        .sync_cancel = cancel_sync,
        .retry_unclean_readonly_close_once = true,
    };
    require_true(
        revlink_application_init(&recovery, &recovery_config)
            == REVLINK_CORE_OK,
        "recovery application init"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
        .identity = identity,
    };
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery monitor start"
    );
    event.kind = REVLINK_DEVICE_EVENT_ATTACHED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery device attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery device acceptance"
    );

    const int recovery_request_base = sync_requests;
    const int recovery_attempt_base = sync_recoveries;
    require_true(
        revlink_application_request_sync(&recovery) == REVLINK_SYNC_OK,
        "recovery initial request"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery initial session open"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&recovery, &sync_event)
            == REVLINK_SYNC_OK,
        "recovery initial sync start"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery initial session close"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_FAILED,
        .data_phase_completed = true,
        .session_close_sent = true,
        .session_close_acknowledged = false,
        .platform_error = -1,
    };
    require_true(
        revlink_application_handle_sync_event(&recovery, &sync_event)
            == REVLINK_SYNC_OK,
        "unclean close queues one complete recovery session"
    );
    require_true(
        sync_requests == recovery_request_base + 1
            && sync_recoveries == recovery_attempt_base + 1,
        "unclean close performs exactly one dedicated recovery request"
    );
    require_true(
        revlink_application_sync_snapshot(&recovery).state
            == REVLINK_SYNC_QUEUED,
        "recovery session must be queued"
    );

    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery retry session open"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_STARTED,
        .close_recovery_attempt = true,
    };
    require_true(
        revlink_application_handle_sync_event(&recovery, &sync_event)
            == REVLINK_SYNC_OK,
        "recovery retry sync start"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "recovery retry session close"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_FAILED,
        .close_recovery_attempt = true,
        .data_phase_completed = true,
        .session_close_sent = true,
        .session_close_acknowledged = false,
        .platform_error = -1,
    };
    require_true(
        revlink_application_handle_sync_event(&recovery, &sync_event)
            == REVLINK_SYNC_OK,
        "second unclean close is terminal"
    );
    require_true(
        sync_requests == recovery_request_base + 1
            && sync_recoveries == recovery_attempt_base + 1,
        "second unclean close must not loop"
    );
    require_true(
        revlink_application_sync_snapshot(&recovery).state
            == REVLINK_SYNC_FAILED,
        "exhausted recovery remains failed"
    );

    require_true(
        revlink_application_request_sync(&recovery) == REVLINK_SYNC_OK,
        "new explicit sync resets recovery budget"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "new explicit session open"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&recovery, &sync_event)
            == REVLINK_SYNC_OK,
        "new explicit sync start"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED;
    require_true(
        revlink_application_handle_device_event(&recovery, &event)
            == REVLINK_CORE_OK,
        "new explicit session close"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_FAILED,
        .data_phase_completed = false,
        .session_close_sent = false,
        .session_close_acknowledged = false,
        .platform_error = -1,
    };
    require_true(
        revlink_application_handle_sync_event(&recovery, &sync_event)
            == REVLINK_SYNC_OK,
        "data-phase failure remains terminal"
    );
    require_true(
        sync_requests == recovery_request_base + 2
            && sync_recoveries == recovery_attempt_base + 1,
        "data-phase failure must not consume an automatic retry"
    );

    revlink_application_t conflict = {0};
    const revlink_application_config_t conflict_config = {
        .sync_policy = {
            .auto_sync_on_attach = true,
        },
        .sync_request = request_sync,
        .sync_recover_session = recover_session,
        .sync_cancel = cancel_sync,
        .retry_unclean_readonly_close_once = true,
    };
    require_true(
        revlink_application_init(&conflict, &conflict_config)
            == REVLINK_CORE_OK,
        "conflict application init"
    );
    const int conflict_request_base = sync_requests;
    const int conflict_cancel_base = sync_cancels;
    revlink_device_identity_t pinned_identity = identity;
    pinned_identity.attachment_generation = 41U;
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
    };
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "conflict monitor start"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = pinned_identity,
        .eligible_device_count = 1U,
    };
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "conflict first attach"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "conflict first acceptance"
    );
    require_true(
        sync_requests == conflict_request_base + 1,
        "first eligible device may auto-sync"
    );
    event.kind = REVLINK_DEVICE_EVENT_SESSION_OPENED;
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "conflict pinned session opens"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_STARTED,
    };
    require_true(
        revlink_application_handle_sync_event(&conflict, &sync_event)
            == REVLINK_SYNC_OK,
        "conflict pinned sync starts"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED,
        .eligible_device_count = 2U,
    };
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "second eligible device enters conflict"
    );
    revlink_device_snapshot_t conflict_snapshot =
        revlink_device_service_snapshot(&conflict.device_service);
    require_true(
        conflict_snapshot.state == REVLINK_DEVICE_CONFLICT
            && conflict_snapshot.eligible_device_count == 2U
            && conflict_snapshot.conflict_recovery_required,
        "conflict is first-class and fail closed"
    );
    require_true(
        sync_cancels == conflict_cancel_base + 1,
        "active read-only sync receives cooperative cancellation"
    );
    require_true(
        revlink_application_request_sync(&conflict)
            == REVLINK_SYNC_INVALID_STATE,
        "manual sync is blocked during conflict"
    );
    require_true(
        revlink_application_authorize(
            &conflict,
            REVLINK_OPERATION_READ_DEVICE
        ) == REVLINK_CORE_NOT_AUTHORIZED,
        "application authorization also blocks device reads"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED,
        .identity = pinned_identity,
    };
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "pinned session may close without clearing conflict"
    );
    sync_event = (revlink_sync_event_t){
        .kind = REVLINK_SYNC_EVENT_CANCELLED,
        .session_close_sent = true,
        .session_close_acknowledged = true,
    };
    require_true(
        revlink_application_handle_sync_event(&conflict, &sync_event)
            == REVLINK_SYNC_OK,
        "conflicted read-only sync finishes cancellation"
    );
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_DETACHED,
        .identity = pinned_identity,
        .eligible_device_count = 1U,
    };
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK
            && revlink_device_service_snapshot(&conflict.device_service).state
                == REVLINK_DEVICE_CONFLICT,
        "removing only one device does not recover"
    );
    event.eligible_device_count = 0U;
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK
            && revlink_device_service_snapshot(&conflict.device_service).state
                == REVLINK_DEVICE_WAITING,
        "complete detach clears the topology conflict"
    );
    pinned_identity.attachment_generation = 42U;
    event = (revlink_device_event_t){
        .kind = REVLINK_DEVICE_EVENT_ATTACHED,
        .identity = pinned_identity,
        .eligible_device_count = 1U,
    };
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "one deliberate reattachment is inspected"
    );
    event.kind = REVLINK_DEVICE_EVENT_ACCEPTED;
    require_true(
        revlink_application_handle_device_event(&conflict, &event)
            == REVLINK_CORE_OK,
        "one deliberate reattachment is accepted"
    );
    require_true(
        sync_requests == conflict_request_base + 1,
        "auto-sync remains disarmed after conflict recovery"
    );
    require_true(
        revlink_application_request_sync(&conflict) == REVLINK_SYNC_OK
            && sync_requests == conflict_request_base + 2,
        "explicit customer sync recovers normal operation"
    );

    test_topology_revision_safety();
    test_bounded_auto_sync_retry();

    puts("core lifecycle and safety tests PASSED");
    return 0;
}
