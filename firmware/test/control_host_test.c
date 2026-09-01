#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revlink_control_service.h"

typedef struct {
    revlink_control_snapshot_t snapshot;
    int snapshot_reads;
    int policy_updates;
    int sync_requests;
    int sync_cancels;
    revlink_control_status_t action_status;
} fake_backend_t;

static void require_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "control host test failed: %s\n", message);
        exit(1);
    }
}

static revlink_control_status_t read_snapshot(
    void *context,
    revlink_control_snapshot_t *snapshot
)
{
    fake_backend_t *backend = (fake_backend_t *)context;
    ++backend->snapshot_reads;
    *snapshot = backend->snapshot;
    return REVLINK_CONTROL_OK;
}

static revlink_control_status_t set_auto_sync(void *context, bool enabled)
{
    fake_backend_t *backend = (fake_backend_t *)context;
    ++backend->policy_updates;
    if (backend->action_status != REVLINK_CONTROL_OK) {
        return backend->action_status;
    }
    backend->snapshot.sync_policy.auto_sync_on_attach = enabled;
    return REVLINK_CONTROL_OK;
}

static revlink_control_status_t request_sync(void *context)
{
    fake_backend_t *backend = (fake_backend_t *)context;
    ++backend->sync_requests;
    if (backend->action_status != REVLINK_CONTROL_OK) {
        return backend->action_status;
    }
    backend->snapshot.sync.state = REVLINK_SYNC_QUEUED;
    return REVLINK_CONTROL_OK;
}

static revlink_control_status_t cancel_sync(void *context)
{
    fake_backend_t *backend = (fake_backend_t *)context;
    ++backend->sync_cancels;
    if (backend->action_status != REVLINK_CONTROL_OK) {
        return backend->action_status;
    }
    backend->snapshot.sync.state = REVLINK_SYNC_CANCELLING;
    return REVLINK_CONTROL_OK;
}

int main(void)
{
    fake_backend_t backend = {
        .snapshot = {
            .device = {
                .state = REVLINK_DEVICE_AVAILABLE,
                .identity = {
                    .vendor_id = 0x1a84,
                    .product_id = 0x0121,
                    .high_speed = true,
                },
            },
            .sync = {
                .state = REVLINK_SYNC_IDLE,
            },
        },
        .action_status = REVLINK_CONTROL_OK,
    };
    const revlink_control_service_config_t config = {
        .context = &backend,
        .read_snapshot = read_snapshot,
        .set_auto_sync = set_auto_sync,
        .request_sync = request_sync,
        .cancel_sync = cancel_sync,
    };
    revlink_control_service_t service = {0};
    require_true(
        revlink_control_service_init(&service, &config)
            == REVLINK_CONTROL_OK,
        "service initialization"
    );

    revlink_control_response_t response = {0};
    revlink_control_request_t request = {
        .command = REVLINK_CONTROL_GET_STATUS,
    };
    require_true(
        revlink_control_service_execute(&service, &request, &response)
            == REVLINK_CONTROL_OK,
        "status command"
    );
    require_true(
        response.snapshot.device.identity.vendor_id == 0x1a84
            && response.snapshot.device.identity.high_speed,
        "status snapshot preserves device identity"
    );

    request = (revlink_control_request_t){
        .command = REVLINK_CONTROL_SET_AUTO_SYNC,
        .enabled = true,
    };
    require_true(
        revlink_control_service_execute(&service, &request, &response)
            == REVLINK_CONTROL_OK,
        "enable auto-sync"
    );
    require_true(
        backend.policy_updates == 1
            && response.snapshot.sync_policy.auto_sync_on_attach,
        "auto-sync mutation returns updated snapshot"
    );

    request.command = REVLINK_CONTROL_REQUEST_SYNC;
    require_true(
        revlink_control_service_execute(&service, &request, &response)
            == REVLINK_CONTROL_OK,
        "manual sync"
    );
    require_true(
        backend.sync_requests == 1
            && response.snapshot.sync.state == REVLINK_SYNC_QUEUED,
        "manual sync returns queued state"
    );

    request.command = REVLINK_CONTROL_CANCEL_SYNC;
    require_true(
        revlink_control_service_execute(&service, &request, &response)
            == REVLINK_CONTROL_OK,
        "cancel sync"
    );
    require_true(
        backend.sync_cancels == 1
            && response.snapshot.sync.state == REVLINK_SYNC_CANCELLING,
        "cancel returns cancelling state"
    );

    backend.action_status = REVLINK_CONTROL_INVALID_STATE;
    request.command = REVLINK_CONTROL_REQUEST_SYNC;
    require_true(
        revlink_control_service_execute(&service, &request, &response)
            == REVLINK_CONTROL_INVALID_STATE,
        "backend rejection is preserved"
    );
    require_true(
        response.status == REVLINK_CONTROL_INVALID_STATE
            && response.snapshot.device.state == REVLINK_DEVICE_AVAILABLE,
        "rejected action still returns current snapshot"
    );

    request.command = (revlink_control_command_t)99;
    require_true(
        revlink_control_service_execute(&service, &request, &response)
            == REVLINK_CONTROL_NOT_SUPPORTED,
        "unknown command fails closed"
    );
    require_true(
        strcmp(
            revlink_control_status_name(response.status),
            "not-supported"
        ) == 0,
        "stable status name"
    );

    puts("control host tests passed");
    return 0;
}
