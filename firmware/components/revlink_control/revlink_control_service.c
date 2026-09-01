#include "revlink_control_service.h"

#include <stddef.h>
#include <string.h>

revlink_control_status_t revlink_control_service_init(
    revlink_control_service_t *service,
    const revlink_control_service_config_t *config
)
{
    if (service == NULL || config == NULL
        || config->read_snapshot == NULL
        || config->set_auto_sync == NULL
        || config->request_sync == NULL
        || config->cancel_sync == NULL) {
        return REVLINK_CONTROL_INVALID_ARGUMENT;
    }

    memset(service, 0, sizeof(*service));
    service->config = *config;
    service->initialized = true;
    return REVLINK_CONTROL_OK;
}

revlink_control_status_t revlink_control_service_execute(
    revlink_control_service_t *service,
    const revlink_control_request_t *request,
    revlink_control_response_t *response
)
{
    if (service == NULL || request == NULL || response == NULL) {
        return REVLINK_CONTROL_INVALID_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));
    if (!service->initialized) {
        response->status = REVLINK_CONTROL_INVALID_STATE;
        return response->status;
    }

    revlink_control_status_t status = REVLINK_CONTROL_OK;
    switch (request->command) {
    case REVLINK_CONTROL_GET_STATUS:
        break;
    case REVLINK_CONTROL_SET_AUTO_SYNC:
        status = service->config.set_auto_sync(
            service->config.context,
            request->enabled
        );
        break;
    case REVLINK_CONTROL_REQUEST_SYNC:
        status = service->config.request_sync(service->config.context);
        break;
    case REVLINK_CONTROL_CANCEL_SYNC:
        status = service->config.cancel_sync(service->config.context);
        break;
    default:
        status = REVLINK_CONTROL_NOT_SUPPORTED;
        break;
    }

    const revlink_control_status_t snapshot_status =
        service->config.read_snapshot(
            service->config.context,
            &response->snapshot
        );
    if (status == REVLINK_CONTROL_OK
        && snapshot_status != REVLINK_CONTROL_OK) {
        status = snapshot_status;
    }
    response->status = status;
    return status;
}

const char *revlink_control_status_name(revlink_control_status_t status)
{
    switch (status) {
    case REVLINK_CONTROL_OK:
        return "ok";
    case REVLINK_CONTROL_INVALID_ARGUMENT:
        return "invalid-argument";
    case REVLINK_CONTROL_INVALID_STATE:
        return "invalid-state";
    case REVLINK_CONTROL_TRANSPORT_ERROR:
        return "transport-error";
    case REVLINK_CONTROL_NOT_SUPPORTED:
        return "not-supported";
    default:
        return "unknown";
    }
}
