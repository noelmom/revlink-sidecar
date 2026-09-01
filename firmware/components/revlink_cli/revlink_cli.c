#include "revlink_cli.h"

#include <stdio.h>
#include <string.h>

revlink_cli_parse_status_t revlink_cli_parse(
    const char *line,
    revlink_control_request_t *request
)
{
    if (line == NULL || request == NULL) {
        return REVLINK_CLI_INVALID;
    }

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        return REVLINK_CLI_HELP;
    }
    if (strcmp(line, "status") == 0) {
        *request = (revlink_control_request_t){
            .command = REVLINK_CONTROL_GET_STATUS,
        };
        return REVLINK_CLI_PARSED;
    }
    if (strcmp(line, "sync") == 0) {
        *request = (revlink_control_request_t){
            .command = REVLINK_CONTROL_REQUEST_SYNC,
        };
        return REVLINK_CLI_PARSED;
    }
    if (strcmp(line, "cancel") == 0) {
        *request = (revlink_control_request_t){
            .command = REVLINK_CONTROL_CANCEL_SYNC,
        };
        return REVLINK_CLI_PARSED;
    }
    if (strcmp(line, "auto on") == 0 || strcmp(line, "auto off") == 0) {
        *request = (revlink_control_request_t){
            .command = REVLINK_CONTROL_SET_AUTO_SYNC,
            .enabled = strcmp(line, "auto on") == 0,
        };
        return REVLINK_CLI_PARSED;
    }
    return REVLINK_CLI_INVALID;
}

bool revlink_cli_format_response(
    const revlink_control_response_t *response,
    char *output,
    size_t output_capacity
)
{
    if (response == NULL || output == NULL || output_capacity == 0U) {
        return false;
    }

    const revlink_control_snapshot_t *snapshot = &response->snapshot;
    const unsigned int completed =
        (unsigned int)snapshot->sync.downloaded
        + (unsigned int)snapshot->sync.skipped;
    const int written = snprintf(
        output,
        output_capacity,
        "status=%s device=%s sync=%s auto=%s progress=%u/%u "
        "downloaded=%u skipped=%u pending=%u writes=%s shutdown=%s",
        revlink_control_status_name(response->status),
        revlink_device_state_name(snapshot->device.state),
        revlink_sync_state_name(snapshot->sync.state),
        snapshot->sync_policy.auto_sync_on_attach ? "on" : "off",
        completed,
        (unsigned int)snapshot->sync.candidates,
        (unsigned int)snapshot->sync.downloaded,
        (unsigned int)snapshot->sync.skipped,
        (unsigned int)snapshot->sync.pending,
        snapshot->writes_compiled ? "available" : "locked",
        snapshot->shutdown_requested ? "yes" : "no"
    );
    return written >= 0 && (size_t)written < output_capacity;
}

const char *revlink_cli_help(void)
{
    return "commands: status | sync | cancel | auto on | auto off | help";
}
