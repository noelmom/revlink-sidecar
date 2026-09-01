#include "revlink_application.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "revlink_accessport_protocol.h"

static const uint8_t ROOT_LIST_VECTOR[] = {
    0x02, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x26, 0x16, 0x00, 0x00, 0x00,
    0x73, 0x65, 0x72, 0x69, 0x61, 0x6c, 0x69, 0x7a, 0x61, 0x74, 0x69,
    0x6f, 0x6e, 0x3a, 0x3a, 0x61, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65,
    0x03, 0x04, 0x04, 0x04, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x39, 0xdf, 0xa4, 0x58,
};

#define REVLINK_AUTO_SYNC_RETRY_LIMIT 1U

static bool sync_state_is_active(revlink_sync_state_t state)
{
    return state == REVLINK_SYNC_QUEUED
        || state == REVLINK_SYNC_RUNNING
        || state == REVLINK_SYNC_CANCELLING;
}

static revlink_sync_status_t request_automatic_sync(
    revlink_application_t *application,
    bool retry
)
{
    if (retry) {
        ++application->auto_sync_retry_count;
    }
    application->auto_sync_retry_eligible = false;
    const revlink_sync_status_t status = revlink_sync_coordinator_request(
        &application->sync_coordinator
    );
    if (status == REVLINK_SYNC_OK) {
        application->auto_sync_attempted = true;
        application->auto_sync_active = true;
        application->unclean_close_recovery_attempted = false;
    } else if (!retry) {
        /*
         * USB acceptance and the transaction queue settle independently.
         * Leave exactly one delayed recovery opportunity when the first
         * attach-time request loses that race.
         */
        application->auto_sync_retry_eligible = true;
    }
    return status;
}

static void handle_device_snapshot(
    void *context,
    const revlink_device_snapshot_t *snapshot
)
{
    revlink_application_t *application = (revlink_application_t *)context;
    if (application == NULL || snapshot == NULL) {
        return;
    }

    const revlink_device_state_t previous =
        application->previous_device_state;
    application->previous_device_state = snapshot->state;

    if (snapshot->state == REVLINK_DEVICE_WAITING
        && !application->preserve_attachment_across_reenumeration
        && !application->auto_sync_disarmed_by_conflict) {
        application->auto_sync_attempted = false;
        application->auto_sync_active = false;
        application->auto_sync_retry_eligible = false;
        application->auto_sync_retry_count = 0U;
    } else if (snapshot->state == REVLINK_DEVICE_INSPECTING
        && previous == REVLINK_DEVICE_WAITING
        && !application->preserve_attachment_across_reenumeration
        && !application->auto_sync_disarmed_by_conflict) {
        application->auto_sync_attempted = false;
        application->auto_sync_active = false;
        application->auto_sync_retry_eligible = false;
        application->auto_sync_retry_count = 0U;
    }

    if (application->state_observer != NULL) {
        application->state_observer(
            application->state_observer_context,
            snapshot
        );
    }

    if (snapshot->state == REVLINK_DEVICE_AVAILABLE
        && previous == REVLINK_DEVICE_INSPECTING
        && application->sync_coordinator.policy.auto_sync_on_attach
        && !application->auto_sync_attempted) {
        (void)request_automatic_sync(application, false);
    }
    if (snapshot->state == REVLINK_DEVICE_AVAILABLE
        && previous == REVLINK_DEVICE_INSPECTING) {
        application->preserve_attachment_across_reenumeration = false;
    }
}

revlink_core_status_t revlink_application_init(
    revlink_application_t *application,
    const revlink_application_config_t *config
)
{
    if (application == NULL || config == NULL
        || config->sync_request == NULL || config->sync_cancel == NULL
        || (config->retry_unclean_readonly_close_once
            && config->sync_recover_session == NULL)) {
        return REVLINK_CORE_INVALID_ARGUMENT;
    }

    memset(application, 0, sizeof(*application));
    application->safety_policy = revlink_safety_policy_default();
    application->safety_policy.allow_device_writes =
        config->allow_device_writes;
    application->safety_policy.allow_device_deletes =
        config->allow_device_deletes;
    application->state_observer = config->state_observer;
    application->state_observer_context = config->state_observer_context;
    application->retry_unclean_readonly_close_once =
        config->retry_unclean_readonly_close_once;
    application->previous_device_state = REVLINK_DEVICE_STOPPED;
    const revlink_sync_coordinator_config_t sync_config = {
        .transport_context = config->sync_transport_context,
        .request = config->sync_request,
        .recover_session = config->sync_recover_session,
        .cancel = config->sync_cancel,
        .observer_context = config->sync_observer_context,
        .observer = config->sync_observer,
    };
    if (revlink_sync_coordinator_init(
            &application->sync_coordinator,
            &sync_config,
            &config->sync_policy
        ) != REVLINK_SYNC_OK) {
        return REVLINK_CORE_INVALID_ARGUMENT;
    }
    revlink_device_service_init(
        &application->device_service,
        handle_device_snapshot,
        application
    );
    return REVLINK_CORE_OK;
}

bool revlink_application_protocol_self_test(void)
{
    uint8_t record[sizeof(ROOT_LIST_VECTOR)] = {0};
    size_t record_length = 0;
    revlink_ap_record_view_t view = {0};
    const revlink_ap_status_t status = revlink_ap_build_list(
        NULL,
        0,
        record,
        sizeof(record),
        &record_length
    );

    return status == REVLINK_AP_OK
        && record_length == sizeof(ROOT_LIST_VECTOR)
        && memcmp(record, ROOT_LIST_VECTOR, sizeof(ROOT_LIST_VECTOR)) == 0
        && revlink_ap_validate_record(record, record_length) == REVLINK_AP_OK
        && revlink_ap_parse_record(record, record_length, &view)
            == REVLINK_AP_OK
        && view.opcode == REVLINK_AP_OPCODE_LIST;
}

revlink_core_status_t revlink_application_handle_device_event(
    revlink_application_t *application,
    const revlink_device_event_t *event
)
{
    if (application == NULL || event == NULL) {
        return REVLINK_CORE_INVALID_ARGUMENT;
    }
    const revlink_device_snapshot_t before =
        revlink_device_service_snapshot(&application->device_service);
    if (event->topology_revision != 0U
        && event->topology_revision < before.topology_revision) {
        return REVLINK_CORE_INVALID_TRANSITION;
    }
    if (event->kind == REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED) {
        application->preserve_attachment_across_reenumeration = false;
        application->auto_sync_attempted = true;
        application->auto_sync_active = false;
        application->auto_sync_retry_eligible = false;
        application->auto_sync_disarmed_by_conflict = true;
        const revlink_sync_snapshot_t sync =
            revlink_sync_coordinator_snapshot(&application->sync_coordinator);
        if (sync.state == REVLINK_SYNC_QUEUED
            || sync.state == REVLINK_SYNC_RUNNING) {
            (void)revlink_sync_coordinator_cancel(
                &application->sync_coordinator
            );
        }
    }
    if (event->kind == REVLINK_DEVICE_EVENT_DETACHED
        && event->software_reenumeration) {
        application->preserve_attachment_across_reenumeration = true;
    } else if (event->kind == REVLINK_DEVICE_EVENT_ATTACHED) {
        application->preserve_attachment_across_reenumeration =
            event->software_reenumeration;
    } else if (event->kind == REVLINK_DEVICE_EVENT_DETACHED) {
        application->preserve_attachment_across_reenumeration = false;
    }
    return revlink_device_service_handle(
        &application->device_service,
        event
    );
}

revlink_core_status_t revlink_application_authorize(
    const revlink_application_t *application,
    revlink_operation_t operation
)
{
    if (application == NULL) {
        return REVLINK_CORE_INVALID_ARGUMENT;
    }
    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&application->device_service);
    if (device.state == REVLINK_DEVICE_CONFLICT
        && operation != REVLINK_OPERATION_DISCOVER) {
        return REVLINK_CORE_NOT_AUTHORIZED;
    }
    return revlink_safety_policy_authorize(
        &application->safety_policy,
        operation
    );
}

revlink_sync_status_t revlink_application_set_sync_policy(
    revlink_application_t *application,
    const revlink_sync_policy_t *policy
)
{
    if (application == NULL || policy == NULL) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }
    const revlink_sync_status_t status = revlink_sync_coordinator_set_policy(
        &application->sync_coordinator,
        policy
    );
    if (status == REVLINK_SYNC_OK && !policy->auto_sync_on_attach) {
        application->auto_sync_active = false;
        application->auto_sync_retry_eligible = false;
    }
    return status;
}

revlink_sync_policy_t revlink_application_sync_policy(
    const revlink_application_t *application
)
{
    if (application == NULL) {
        const revlink_sync_policy_t empty = {0};
        return empty;
    }
    return revlink_sync_coordinator_policy(
        &application->sync_coordinator
    );
}

revlink_sync_status_t revlink_application_request_sync(
    revlink_application_t *application
)
{
    if (application == NULL) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }
    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&application->device_service);
    if (device.state != REVLINK_DEVICE_AVAILABLE) {
        return REVLINK_SYNC_INVALID_STATE;
    }
    const revlink_sync_status_t status = revlink_sync_coordinator_request(
        &application->sync_coordinator
    );
    if (status == REVLINK_SYNC_OK) {
        application->auto_sync_attempted = true;
        application->auto_sync_active = false;
        application->auto_sync_retry_eligible = false;
        application->auto_sync_disarmed_by_conflict = false;
        application->unclean_close_recovery_attempted = false;
    }
    return status;
}

bool revlink_application_auto_sync_retry_needed(
    const revlink_application_t *application
)
{
    if (application == NULL
        || !application->sync_coordinator.policy.auto_sync_on_attach
        || application->auto_sync_disarmed_by_conflict
        || !application->auto_sync_retry_eligible
        || application->auto_sync_retry_count
            >= REVLINK_AUTO_SYNC_RETRY_LIMIT) {
        return false;
    }
    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&application->device_service);
    const revlink_sync_snapshot_t sync =
        revlink_sync_coordinator_snapshot(&application->sync_coordinator);
    return device.state == REVLINK_DEVICE_AVAILABLE
        && !device.conflict_recovery_required
        && !sync_state_is_active(sync.state);
}

revlink_sync_status_t revlink_application_retry_auto_sync(
    revlink_application_t *application
)
{
    if (application == NULL) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }
    if (!revlink_application_auto_sync_retry_needed(application)) {
        return REVLINK_SYNC_INVALID_STATE;
    }
    return request_automatic_sync(application, true);
}

revlink_sync_status_t revlink_application_cancel_sync(
    revlink_application_t *application
)
{
    if (application == NULL) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }
    return revlink_sync_coordinator_cancel(
        &application->sync_coordinator
    );
}

revlink_sync_status_t revlink_application_handle_sync_event(
    revlink_application_t *application,
    const revlink_sync_event_t *event
)
{
    if (application == NULL || event == NULL) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }
    const revlink_sync_status_t status =
        revlink_sync_coordinator_handle_event(
        &application->sync_coordinator,
        event
    );
    if (status != REVLINK_SYNC_OK) {
        return status;
    }

    if (event->kind == REVLINK_SYNC_EVENT_FAILED
        && application->auto_sync_active
        && !event->close_recovery_attempt
        && !event->data_phase_completed
        && event->downloaded == 0U
        && event->downloaded_bytes == 0U
        && application->auto_sync_retry_count
            < REVLINK_AUTO_SYNC_RETRY_LIMIT) {
        application->auto_sync_retry_eligible = true;
    } else if (event->kind == REVLINK_SYNC_EVENT_COMPLETED
        || event->kind == REVLINK_SYNC_EVENT_CANCELLED
        || event->kind == REVLINK_SYNC_EVENT_FAILED) {
        application->auto_sync_active = false;
        application->auto_sync_retry_eligible = false;
    }

    const bool recoverable_close_failure =
        event->kind == REVLINK_SYNC_EVENT_FAILED
        && !event->close_recovery_attempt
        && event->data_phase_completed
        && !event->session_close_acknowledged;
    if (!application->retry_unclean_readonly_close_once
        || !recoverable_close_failure
        || application->unclean_close_recovery_attempted) {
        return REVLINK_SYNC_OK;
    }

    const revlink_device_snapshot_t device =
        revlink_device_service_snapshot(&application->device_service);
    if (device.state != REVLINK_DEVICE_AVAILABLE
        || device.conflict_recovery_required) {
        return REVLINK_SYNC_OK;
    }

    application->unclean_close_recovery_attempted = true;
    return revlink_sync_coordinator_recover_session(
        &application->sync_coordinator
    );
}

revlink_sync_snapshot_t revlink_application_sync_snapshot(
    const revlink_application_t *application
)
{
    if (application == NULL) {
        const revlink_sync_snapshot_t empty = {0};
        return empty;
    }
    return revlink_sync_coordinator_snapshot(
        &application->sync_coordinator
    );
}
