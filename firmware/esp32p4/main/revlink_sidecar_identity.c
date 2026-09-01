#include "revlink_sidecar_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "revlink_wifi_radio.h"

#define REVLINK_IDENTITY_NAMESPACE "sidecar_id"
#define REVLINK_IDENTITY_KEY "device_id"
#define REVLINK_COLLISION_KEY "name_suffix"

static bool identity_ready;
static revlink_sidecar_identity_t current_identity;
static uint8_t current_hardware_mac[REVLINK_IDENTITY_MAC_BYTES];

static uint16_t collision_index_from_hostname(
    const char *hostname,
    const uint8_t mac[REVLINK_IDENTITY_MAC_BYTES]
)
{
    char base_ssid[REVLINK_IDENTITY_SSID_CAPACITY] = {0};
    char base_hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY] = {0};
    char mac_text[REVLINK_IDENTITY_MAC_TEXT_CAPACITY] = {0};
    if (
        !revlink_identity_format_local(
            mac,
            0U,
            base_ssid,
            sizeof(base_ssid),
            base_hostname,
            sizeof(base_hostname),
            mac_text,
            sizeof(mac_text)
        )
        || strncmp(hostname, base_hostname, strlen(base_hostname)) != 0
    ) {
        return 0U;
    }
    const char *suffix = hostname + strlen(base_hostname);
    if (*suffix == '\0') {
        return 0U;
    }
    unsigned int index = 0U;
    char trailing = '\0';
    if (
        sscanf(suffix, "-%u%c", &index, &trailing) == 1
        && index >= 2U
        && index <= UINT16_MAX
    ) {
        return (uint16_t)index;
    }
    return 0U;
}

static esp_err_t load_or_create_device_id(
    char device_id[REVLINK_IDENTITY_DEVICE_ID_CAPACITY]
)
{
    const esp_err_t init_status = nvs_flash_init();
    if (init_status != ESP_OK) {
        return init_status;
    }
    nvs_handle_t handle;
    esp_err_t status = nvs_open(
        REVLINK_IDENTITY_NAMESPACE,
        NVS_READWRITE,
        &handle
    );
    if (status != ESP_OK) {
        return status;
    }

    size_t length = REVLINK_IDENTITY_DEVICE_ID_CAPACITY;
    status = nvs_get_str(handle, REVLINK_IDENTITY_KEY, device_id, &length);
    if (
        status == ESP_OK
        && revlink_identity_device_id_valid(device_id)
    ) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (status != ESP_ERR_NVS_NOT_FOUND && status != ESP_OK) {
        nvs_close(handle);
        return status;
    }

    uint8_t random_bytes[REVLINK_IDENTITY_RANDOM_BYTES] = {0};
    esp_fill_random(random_bytes, sizeof(random_bytes));
    if (!revlink_identity_format_device_id(
            random_bytes,
            device_id,
            REVLINK_IDENTITY_DEVICE_ID_CAPACITY
        )) {
        memset(random_bytes, 0, sizeof(random_bytes));
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    memset(random_bytes, 0, sizeof(random_bytes));
    status = nvs_set_str(handle, REVLINK_IDENTITY_KEY, device_id);
    if (status == ESP_OK) {
        status = nvs_commit(handle);
    }
    nvs_close(handle);
    return status;
}

static esp_err_t load_collision_index(uint16_t *index)
{
    if (index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t status = nvs_open(
        REVLINK_IDENTITY_NAMESPACE,
        NVS_READONLY,
        &handle
    );
    if (status != ESP_OK) {
        return status;
    }
    status = nvs_get_u16(handle, REVLINK_COLLISION_KEY, index);
    nvs_close(handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        *index = 0U;
        return ESP_OK;
    }
    if (status == ESP_OK && (*index == 1U || *index > 99U)) {
        *index = 0U;
    }
    return status;
}

static esp_err_t persist_collision_index(uint16_t index)
{
    nvs_handle_t handle;
    esp_err_t status = nvs_open(
        REVLINK_IDENTITY_NAMESPACE,
        NVS_READWRITE,
        &handle
    );
    if (status != ESP_OK) {
        return status;
    }
    status = nvs_set_u16(handle, REVLINK_COLLISION_KEY, index);
    if (status == ESP_OK) {
        status = nvs_commit(handle);
    }
    nvs_close(handle);
    return status;
}

esp_err_t revlink_sidecar_identity_init(void)
{
    if (identity_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t mac[REVLINK_IDENTITY_MAC_BYTES] = {0};
    esp_err_t status = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (status != ESP_OK) {
        return status;
    }

    revlink_sidecar_identity_t identity = {0};
    status = load_or_create_device_id(identity.device_id);
    if (status != ESP_OK) {
        return status;
    }
    uint16_t persisted_collision_index = 0U;
    status = load_collision_index(&persisted_collision_index);
    if (status != ESP_OK) {
        return status;
    }
    status = revlink_wifi_radio_set_local_collision_index(
        persisted_collision_index
    );
    if (status != ESP_OK) {
        return status;
    }
    status = revlink_wifi_radio_local_identity(
        identity.ssid,
        sizeof(identity.ssid),
        identity.hostname,
        sizeof(identity.hostname)
    );
    if (status != ESP_OK) {
        return status;
    }
    char formatted_ssid[REVLINK_IDENTITY_SSID_CAPACITY] = {0};
    char formatted_hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY] = {0};
    if (!revlink_identity_format_local(
            mac,
            0U,
            formatted_ssid,
            sizeof(formatted_ssid),
            formatted_hostname,
            sizeof(formatted_hostname),
            identity.hardware_mac,
            sizeof(identity.hardware_mac)
        )) {
        return ESP_ERR_INVALID_SIZE;
    }
    identity.collision_index =
        collision_index_from_hostname(identity.hostname, mac);
    if (identity.collision_index != persisted_collision_index) {
        status = persist_collision_index(identity.collision_index);
        if (status != ESP_OK) {
            return status;
        }
    }

    current_identity = identity;
    memcpy(current_hardware_mac, mac, sizeof(current_hardware_mac));
    identity_ready = true;
    return ESP_OK;
}

esp_err_t revlink_sidecar_identity_snapshot(
    revlink_sidecar_identity_t *identity
)
{
    if (!identity_ready || identity == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *identity = current_identity;
    char active_hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY] = {0};
    if (mdns_hostname_get(active_hostname) == ESP_OK) {
        const int length = snprintf(
            identity->hostname,
            sizeof(identity->hostname),
            "%s",
            active_hostname
        );
        if (
            length <= 0
            || (size_t)length >= sizeof(identity->hostname)
        ) {
            return ESP_ERR_INVALID_SIZE;
        }
        identity->collision_index = collision_index_from_hostname(
            identity->hostname,
            current_hardware_mac
        );
    }
    return ESP_OK;
}
