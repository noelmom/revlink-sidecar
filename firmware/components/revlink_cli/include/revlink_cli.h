#ifndef REVLINK_CLI_H
#define REVLINK_CLI_H

#include <stdbool.h>
#include <stddef.h>

#include "revlink_control_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_CLI_PARSED = 0,
    REVLINK_CLI_HELP,
    REVLINK_CLI_INVALID,
} revlink_cli_parse_status_t;

revlink_cli_parse_status_t revlink_cli_parse(
    const char *line,
    revlink_control_request_t *request
);

bool revlink_cli_format_response(
    const revlink_control_response_t *response,
    char *output,
    size_t output_capacity
);

const char *revlink_cli_help(void);

#ifdef __cplusplus
}
#endif

#endif
