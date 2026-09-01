#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revlink_status_model.h"

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    revlink_status_model_t model;
    revlink_status_model_init(&model);

    revlink_status_view_t view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_BOOTING, "starts on boot view");

    revlink_status_model_set_boot_complete(&model, true);
    view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_WAITING, "waits for AccessPort");
    require(
        strcmp(view.headline, "NO DEVICE") == 0,
        "explains that no AccessPort is attached"
    );

    revlink_device_snapshot_t device = {
        .state = REVLINK_DEVICE_AVAILABLE,
    };
    revlink_status_model_set_device(&model, &device);
    view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_READY, "shows ready device");
    require(
        strcmp(view.headline, "ACCESSPORT") == 0
            && strcmp(view.detail, "DEVICE CONNECTED") == 0
            && strcmp(view.footer, "READY TO SYNC") == 0,
        "ready copy does not imply an AccessPort operating mode"
    );
    require(!view.show_progress, "ready screen does not show an empty bar");
    revlink_status_model_set_vehicle(
        &model,
        "2018 USDM WRX MT COBB Custom Features Gen2"
    );
    revlink_status_model_set_part_number(&model, "AP3-SUB-004");
    view = revlink_status_model_view(&model);
    require(
        strcmp(view.headline, "2018 USDM WRX MT") == 0,
        "ready screen identifies the connected vehicle"
    );
    require(
        strcmp(view.detail, "AP3-SUB-004 CONNECTED") == 0,
        "ready screen identifies the connected AccessPort part"
    );
    require(
        strcmp(view.footer, "READY TO SYNC") == 0,
        "vehicle identity keeps the ready action visible"
    );

    revlink_network_snapshot_t network = {
        .state = REVLINK_NETWORK_RECONNECTING,
        .phase_elapsed_ms = 5500U,
        .phase_timeout_ms = 30000U,
    };
    revlink_status_model_set_network(&model, &network);
    view = revlink_status_model_view(&model);
    require(
        view.kind == REVLINK_STATUS_WIFI_RECONNECTING,
        "shows Wi-Fi reconnect countdown"
    );
    require(view.countdown_seconds == 25U, "rounds countdown up");
    require(view.progress_percent == 18U, "shows reconnect budget progress");
    require(
        strcmp(view.footer, "HOTSPOT NEXT") == 0,
        "explains reconnect fallback"
    );

    revlink_sync_snapshot_t sync = {
        .state = REVLINK_SYNC_RUNNING,
        .candidates = 4U,
        .downloaded = 1U,
        .skipped = 1U,
        .pending = 2U,
    };
    revlink_status_model_set_sync(&model, &sync);
    view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_SYNCING, "shows active sync");
    require(view.progress_percent == 50U, "computes bounded progress");
    require(view.show_progress, "active sync shows transfer progress");
    require(
        strcmp(view.footer, "KEEP CONNECTED") == 0,
        "shows transfer safety instruction"
    );

    sync.state = REVLINK_SYNC_IDLE;
    revlink_status_model_set_sync(&model, &sync);
    network.phase_elapsed_ms = 29501U;
    revlink_status_model_set_network(&model, &network);
    view = revlink_status_model_view(&model);
    require(view.countdown_seconds == 1U, "keeps final partial second visible");

    network.phase_elapsed_ms = 30000U;
    revlink_status_model_set_network(&model, &network);
    view = revlink_status_model_view(&model);
    require(view.countdown_seconds == 0U, "countdown reaches zero");

    sync.state = REVLINK_SYNC_RUNNING;
    sync.close_recovery_attempt = true;
    revlink_status_model_set_sync(&model, &sync);
    view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_RECOVERING, "shows close recovery");

    sync = (revlink_sync_snapshot_t){
        .state = REVLINK_SYNC_COMPLETED,
    };
    revlink_status_model_set_sync(&model, &sync);
    view = revlink_status_model_view(&model);
    require(
        view.kind == REVLINK_STATUS_WIFI_RECONNECTING,
        "reconnect notice supersedes stale completion"
    );

    network.state = REVLINK_NETWORK_CLIENT_READY;
    revlink_status_model_set_network(&model, &network);
    view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_COMPLETE, "shows completion");
    require(view.progress_percent == 100U, "completion is 100 percent");
    require(
        strcmp(view.headline, "2018 USDM WRX MT") == 0
            && strcmp(view.detail, "BACKUP COMPLETE") == 0
            && strcmp(view.footer, "SAFE TO DISCONNECT") == 0,
        "completion keeps the vehicle and gives a precise next action"
    );

    device.state = REVLINK_DEVICE_WAITING;
    revlink_status_model_set_device(&model, &device);
    view = revlink_status_model_view(&model);
    require(
        view.kind == REVLINK_STATUS_WAITING,
        "physical detach supersedes stale completion"
    );
    require(
        strcmp(view.detail, "ACCESSPORT OFFLINE") == 0,
        "detached screen clearly identifies the AccessPort"
    );

    device.state = REVLINK_DEVICE_CONFLICT;
    device.eligible_device_count = 2U;
    device.conflict_recovery_required = true;
    revlink_status_model_set_device(&model, &device);
    view = revlink_status_model_view(&model);
    require(
        view.kind == REVLINK_STATUS_ATTENTION
            && strcmp(view.headline, "MULTIPLE DEVICES") == 0
            && strcmp(view.detail, "UNPLUG ALL DEVICES") == 0
            && strcmp(view.footer, "THEN RECONNECT ONE") == 0,
        "multiple AccessPorts receive an explicit fail-closed display"
    );

    device.state = REVLINK_DEVICE_FAULTED;
    revlink_status_model_set_device(&model, &device);
    view = revlink_status_model_view(&model);
    require(
        view.kind == REVLINK_STATUS_ATTENTION,
        "USB fault supersedes stale completion"
    );

    device.state = REVLINK_DEVICE_AVAILABLE;
    revlink_status_model_set_device(&model, &device);
    sync.state = REVLINK_SYNC_FAILED;
    revlink_status_model_set_sync(&model, &sync);
    view = revlink_status_model_view(&model);
    require(view.kind == REVLINK_STATUS_ATTENTION, "shows failed sync");

    puts("status model tests passed");
    return 0;
}
