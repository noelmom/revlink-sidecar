#include "revlink_wifi_store.h"

#include <stdint.h>
#include <string.h>

#include "nvs.h"

#define REVLINK_WIFI_STORE_NAMESPACE "revlink_wifi"
#define REVLINK_WIFI_STORE_KEY "preferred_v1"
#define REVLINK_WIFI_STORE_MAGIC 0x524C5746U
#define REVLINK_WIFI_STORE_SCHEMA 1U

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    char ssid[REVLINK_WIFI_SSID_CAPACITY];
    char password[REVLINK_WIFI_PASSWORD_CAPACITY];
} revlink_wifi_store_record_t;

static void clear_sensitive(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;
    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

esp_err_t revlink_wifi_store_load(
    revlink_wifi_credentials_t *credentials
)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    revlink_wifi_credentials_clear(credentials);

    nvs_handle_t handle;
    esp_err_t status = nvs_open(
        REVLINK_WIFI_STORE_NAMESPACE,
        NVS_READONLY,
        &handle
    );
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status != ESP_OK) {
        return status;
    }

    revlink_wifi_store_record_t record = {0};
    size_t size = sizeof(record);
    status = nvs_get_blob(
        handle,
        REVLINK_WIFI_STORE_KEY,
        &record,
        &size
    );
    nvs_close(handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        status = ESP_ERR_NOT_FOUND;
    }
    if (status == ESP_OK
        && (size != sizeof(record)
            || record.magic != REVLINK_WIFI_STORE_MAGIC
            || record.schema != REVLINK_WIFI_STORE_SCHEMA
            || record.size != sizeof(record))) {
        status = ESP_ERR_INVALID_RESPONSE;
    }
    if (status == ESP_OK
        && !revlink_wifi_credentials_assign(
            credentials,
            record.ssid,
            record.password
        )) {
        status = ESP_ERR_INVALID_RESPONSE;
    }
    clear_sensitive(&record, sizeof(record));
    if (status != ESP_OK) {
        revlink_wifi_credentials_clear(credentials);
    }
    return status;
}

esp_err_t revlink_wifi_store_save(
    const revlink_wifi_credentials_t *credentials
)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    revlink_wifi_credentials_t validated = {0};
    if (!revlink_wifi_credentials_assign(
        &validated,
        credentials->ssid,
        credentials->password
    )) {
        return ESP_ERR_INVALID_ARG;
    }

    revlink_wifi_store_record_t record = {
        .magic = REVLINK_WIFI_STORE_MAGIC,
        .schema = REVLINK_WIFI_STORE_SCHEMA,
        .size = (uint16_t)sizeof(record),
    };
    memcpy(record.ssid, validated.ssid, sizeof(record.ssid));
    memcpy(record.password, validated.password, sizeof(record.password));
    revlink_wifi_credentials_clear(&validated);

    nvs_handle_t handle;
    esp_err_t status = nvs_open(
        REVLINK_WIFI_STORE_NAMESPACE,
        NVS_READWRITE,
        &handle
    );
    if (status == ESP_OK) {
        status = nvs_set_blob(
            handle,
            REVLINK_WIFI_STORE_KEY,
            &record,
            sizeof(record)
        );
        if (status == ESP_OK) {
            status = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    clear_sensitive(&record, sizeof(record));
    return status;
}

esp_err_t revlink_wifi_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t status = nvs_open(
        REVLINK_WIFI_STORE_NAMESPACE,
        NVS_READWRITE,
        &handle
    );
    if (status != ESP_OK) {
        return status;
    }
    status = nvs_erase_key(handle, REVLINK_WIFI_STORE_KEY);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        status = ESP_OK;
    } else if (status == ESP_OK) {
        status = nvs_commit(handle);
    }
    nvs_close(handle);
    return status;
}
