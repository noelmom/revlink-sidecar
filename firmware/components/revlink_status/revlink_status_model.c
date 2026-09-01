#include "revlink_status_model.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint8_t sync_progress(const revlink_sync_snapshot_t *sync)
{
    if (sync->state == REVLINK_SYNC_COMPLETED) {
        return 100U;
    }
    if (sync->candidates == 0U) {
        return 0U;
    }
    size_t finished = sync->downloaded + sync->skipped;
    if (finished > sync->candidates) {
        finished = sync->candidates;
    }
    return (uint8_t)((finished * 100U) / sync->candidates);
}

static uint8_t network_progress(const revlink_network_snapshot_t *network)
{
    if (network->phase_timeout_ms == 0U) {
        return 0U;
    }
    const uint32_t elapsed =
        network->phase_elapsed_ms > network->phase_timeout_ms
            ? network->phase_timeout_ms
            : network->phase_elapsed_ms;
    return (uint8_t)(
        ((uint64_t)elapsed * 100U) / network->phase_timeout_ms
    );
}

static uint16_t network_seconds_remaining(
    const revlink_network_snapshot_t *network
)
{
    if (
        network->phase_timeout_ms == 0U
        || network->phase_elapsed_ms >= network->phase_timeout_ms
    ) {
        return 0U;
    }
    const uint32_t remaining =
        network->phase_timeout_ms - network->phase_elapsed_ms;
    const uint32_t seconds =
        remaining / 1000U + (remaining % 1000U == 0U ? 0U : 1U);
    return seconds > UINT16_MAX ? UINT16_MAX : (uint16_t)seconds;
}

void revlink_status_model_init(revlink_status_model_t *model)
{
    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->device.state = REVLINK_DEVICE_STOPPED;
    model->sync.state = REVLINK_SYNC_IDLE;
}

void revlink_status_model_set_boot_complete(
    revlink_status_model_t *model,
    bool complete
)
{
    if (model != NULL) {
        model->boot_complete = complete;
    }
}

void revlink_status_model_set_device(
    revlink_status_model_t *model,
    const revlink_device_snapshot_t *snapshot
)
{
    if (model != NULL && snapshot != NULL) {
        model->device = *snapshot;
    }
}

void revlink_status_model_set_sync(
    revlink_status_model_t *model,
    const revlink_sync_snapshot_t *snapshot
)
{
    if (model != NULL && snapshot != NULL) {
        model->sync = *snapshot;
    }
}

void revlink_status_model_set_network(
    revlink_status_model_t *model,
    const revlink_network_snapshot_t *snapshot
)
{
    if (model != NULL && snapshot != NULL) {
        model->network = *snapshot;
    }
}

void revlink_status_model_set_vehicle(
    revlink_status_model_t *model,
    const char *vehicle
)
{
    if (model == NULL) {
        return;
    }
    memset(model->vehicle_label, 0, sizeof(model->vehicle_label));
    if (vehicle == NULL || vehicle[0] == '\0') {
        return;
    }

    static const char *suffixes[] = {
        " COBB",
        " CCF",
        " ACCESSPORT",
    };
    size_t source_length = strlen(vehicle);
    for (size_t index = 0U;
         index < sizeof(suffixes) / sizeof(suffixes[0]);
         ++index) {
        const char *suffix = strstr(vehicle, suffixes[index]);
        if (suffix != NULL && (size_t)(suffix - vehicle) < source_length) {
            source_length = (size_t)(suffix - vehicle);
        }
    }

    while (source_length > 0U && vehicle[source_length - 1U] == ' ') {
        --source_length;
    }
    if (source_length >= sizeof(model->vehicle_label)) {
        source_length = sizeof(model->vehicle_label) - 1U;
        while (source_length > 0U && vehicle[source_length] != ' ') {
            --source_length;
        }
        if (source_length == 0U) {
            source_length = sizeof(model->vehicle_label) - 1U;
        }
    }
    memcpy(model->vehicle_label, vehicle, source_length);
    model->vehicle_label[source_length] = '\0';
}

void revlink_status_model_set_part_number(
    revlink_status_model_t *model,
    const char *part_number
)
{
    if (model == NULL) {
        return;
    }
    memset(model->part_number, 0, sizeof(model->part_number));
    memset(model->connected_label, 0, sizeof(model->connected_label));
    if (part_number == NULL || part_number[0] == '\0') {
        return;
    }

    const size_t source_length = strnlen(
        part_number,
        sizeof(model->part_number)
    );
    if (source_length == 0U || source_length >= sizeof(model->part_number)) {
        return;
    }
    memcpy(model->part_number, part_number, source_length);
    model->part_number[source_length] = '\0';
    const int count = snprintf(
        model->connected_label,
        sizeof(model->connected_label),
        "%s CONNECTED",
        model->part_number
    );
    if (count <= 0 || (size_t)count >= sizeof(model->connected_label)) {
        memset(model->part_number, 0, sizeof(model->part_number));
        memset(model->connected_label, 0, sizeof(model->connected_label));
    }
}

revlink_status_view_t revlink_status_model_view(
    const revlink_status_model_t *model
)
{
    if (model == NULL || !model->boot_complete) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_BOOTING,
            .headline = "STARTING",
            .detail = "SYSTEM CHECK",
            .footer = "REVLINK",
            .progress_indeterminate = true,
        };
    }

    const revlink_sync_snapshot_t *sync = &model->sync;
    switch (sync->state) {
    case REVLINK_SYNC_QUEUED:
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_SYNC_QUEUED,
            .headline = "SYNC QUEUED",
            .detail = "PREPARING BACKUP",
            .footer = "PLEASE WAIT",
            .show_progress = true,
            .progress_indeterminate = true,
        };
    case REVLINK_SYNC_RUNNING:
        if (sync->close_recovery_attempt) {
            return (revlink_status_view_t){
                .kind = REVLINK_STATUS_RECOVERING,
                .headline = "RECOVERING",
                .detail = "SAFE USB CLOSE",
                .footer = "KEEP CONNECTED",
                .show_progress = true,
                .progress_indeterminate = true,
            };
        }
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_SYNCING,
            .headline = "SYNCING",
            .detail = "BACKING UP LOGS",
            .footer = "KEEP CONNECTED",
            .progress_percent = sync_progress(sync),
            .show_progress = true,
            .progress_indeterminate = sync->candidates == 0U,
        };
    case REVLINK_SYNC_CANCELLING:
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_CANCELLING,
            .headline = "FINISHING",
            .detail = "CLOSING SAFELY",
            .footer = "KEEP CONNECTED",
            .show_progress = true,
            .progress_indeterminate = true,
        };
    case REVLINK_SYNC_COMPLETED:
    case REVLINK_SYNC_FAILED:
    case REVLINK_SYNC_CANCELLED:
    case REVLINK_SYNC_IDLE:
        break;
    }

    const revlink_network_snapshot_t *network = &model->network;
    if (network->state == REVLINK_NETWORK_RECONNECTING) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_WIFI_RECONNECTING,
            .headline = "WIFI LOST",
            .detail = "RETRYING",
            .footer = "HOTSPOT NEXT",
            .progress_percent = network_progress(network),
            .countdown_seconds = network_seconds_remaining(network),
        };
    }

    /*
     * A terminal sync result describes the previous operation. Physical
     * device state is current, so a detached or faulted AccessPort must not
     * leave a stale "safe to use" completion screen visible indefinitely.
     */
    if (
        model->device.state == REVLINK_DEVICE_STOPPED
        || model->device.state == REVLINK_DEVICE_WAITING
    ) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_WAITING,
            .headline = "NO DEVICE",
            .detail = "ACCESSPORT OFFLINE",
            .footer = "CONNECT TO SYNC",
        };
    }
    if (model->device.state == REVLINK_DEVICE_FAULTED) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_ATTENTION,
            .headline = "ATTENTION",
            .detail = "USB NEEDS CHECK",
            .footer = "OPEN REVLINK",
        };
    }
    if (model->device.state == REVLINK_DEVICE_CONFLICT) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_ATTENTION,
            .headline = "MULTIPLE DEVICES",
            .detail = "UNPLUG ALL DEVICES",
            .footer = "THEN RECONNECT ONE",
        };
    }

    if (sync->state == REVLINK_SYNC_COMPLETED) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_COMPLETE,
            .headline = model->vehicle_label[0] != '\0'
                ? model->vehicle_label
                : "SYNC COMPLETE",
            .detail = "BACKUP COMPLETE",
            .footer = "SAFE TO DISCONNECT",
            .progress_percent = 100U,
        };
    }
    if (sync->state == REVLINK_SYNC_FAILED) {
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_ATTENTION,
            .headline = "ATTENTION",
            .detail = "SYNC NEEDS CHECK",
            .footer = "OPEN REVLINK",
        };
    }

    switch (model->device.state) {
    case REVLINK_DEVICE_STOPPED:
    case REVLINK_DEVICE_WAITING:
        break;
    case REVLINK_DEVICE_INSPECTING:
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_INSPECTING,
            .headline = "CHECKING",
            .detail = "ACCESSPORT",
            .footer = "USB HIGH SPEED",
            .progress_indeterminate = true,
        };
    case REVLINK_DEVICE_AVAILABLE:
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_READY,
            .headline = model->vehicle_label[0] != '\0'
                ? model->vehicle_label
                : "ACCESSPORT",
            .detail = model->connected_label[0] != '\0'
                ? model->connected_label
                : "DEVICE CONNECTED",
            .footer = "READY TO SYNC",
        };
    case REVLINK_DEVICE_SESSION_ACTIVE:
        return (revlink_status_view_t){
            .kind = REVLINK_STATUS_SYNCING,
            .headline = "CONNECTED",
            .detail = "SESSION ACTIVE",
            .footer = "KEEP CONNECTED",
            .progress_indeterminate = true,
        };
    case REVLINK_DEVICE_CONFLICT:
        break;
    case REVLINK_DEVICE_FAULTED:
        break;
    }

    return (revlink_status_view_t){
        .kind = REVLINK_STATUS_ATTENTION,
        .headline = "ATTENTION",
        .detail = "STATUS UNKNOWN",
        .footer = "OPEN REVLINK",
    };
}
