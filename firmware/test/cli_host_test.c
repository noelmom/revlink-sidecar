#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "revlink_cli.h"

int main(void)
{
    revlink_control_request_t request = {0};
    assert(revlink_cli_parse("status", &request) == REVLINK_CLI_PARSED);
    assert(request.command == REVLINK_CONTROL_GET_STATUS);
    assert(revlink_cli_parse("sync", &request) == REVLINK_CLI_PARSED);
    assert(request.command == REVLINK_CONTROL_REQUEST_SYNC);
    assert(revlink_cli_parse("cancel", &request) == REVLINK_CLI_PARSED);
    assert(request.command == REVLINK_CONTROL_CANCEL_SYNC);
    assert(revlink_cli_parse("auto on", &request) == REVLINK_CLI_PARSED);
    assert(request.command == REVLINK_CONTROL_SET_AUTO_SYNC);
    assert(request.enabled);
    assert(revlink_cli_parse("auto off", &request) == REVLINK_CLI_PARSED);
    assert(!request.enabled);
    assert(revlink_cli_parse("help", &request) == REVLINK_CLI_HELP);
    assert(revlink_cli_parse("delete everything", &request)
        == REVLINK_CLI_INVALID);

    const revlink_control_response_t response = {
        .status = REVLINK_CONTROL_OK,
        .snapshot = {
            .device = {
                .state = REVLINK_DEVICE_AVAILABLE,
            },
            .sync_policy = {
                .auto_sync_on_attach = false,
            },
            .sync = {
                .state = REVLINK_SYNC_RUNNING,
                .candidates = 12,
                .downloaded = 2,
                .skipped = 7,
                .pending = 3,
            },
            .writes_compiled = false,
        },
    };
    char output[256] = {0};
    assert(revlink_cli_format_response(
        &response,
        output,
        sizeof(output)
    ));
    assert(strstr(output, "device=available") != NULL);
    assert(strstr(output, "sync=running") != NULL);
    assert(strstr(output, "progress=9/12") != NULL);
    assert(strstr(output, "writes=locked") != NULL);

    puts("revlink CLI host tests passed");
    return 0;
}
