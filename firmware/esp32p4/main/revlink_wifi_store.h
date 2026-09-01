#pragma once

#include "esp_err.h"
#include "revlink_credentials.h"

/*
 * ESP32-P4 persistence adapter for one preferred station credential.
 *
 * The credential is stored as a versioned, atomic NVS blob. Callers must
 * still clear their RAM copies. This adapter never logs credential material.
 */
esp_err_t revlink_wifi_store_load(
    revlink_wifi_credentials_t *credentials
);

esp_err_t revlink_wifi_store_save(
    const revlink_wifi_credentials_t *credentials
);

esp_err_t revlink_wifi_store_erase(void);
