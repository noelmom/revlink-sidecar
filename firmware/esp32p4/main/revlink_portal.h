#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef bool (*revlink_portal_time_observer_t)(
    void *context,
    uint64_t utc_seconds
);

/*
 * Supplies a local-browser time observation to the platform composition
 * layer. The observer decides whether the value is needed and acceptable.
 */
void revlink_portal_configure_time_observer(
    void *context,
    revlink_portal_time_observer_t observer
);

/*
 * Registers the read-only product status surface and the bounded sync-policy
 * controls on an existing local HTTP server.
 */
esp_err_t revlink_portal_register(httpd_handle_t server);

/* Serves the embedded product portal shell. */
esp_err_t revlink_portal_page_handler(httpd_req_t *request);
