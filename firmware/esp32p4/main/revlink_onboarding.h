#pragma once

#include "esp_err.h"

/*
 * Starts the local onboarding HTTP, captive-DNS, and mDNS adapters.
 * The HTTP API accepts credentials only while the coordinator is serving its
 * private fallback hotspot. Credentials are handed directly to the network
 * runtime, zeroized from request buffers, and persisted only after a
 * successful station association. This adapter never logs them.
 */
esp_err_t revlink_onboarding_start(void);
