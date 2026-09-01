#include "revlink_credentials.h"

#include <stdint.h>
#include <string.h>

static bool printable_ascii(const char *value, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char character = (unsigned char)value[index];
        if (character < 0x20U || character > 0x7eU) {
            return false;
        }
    }
    return true;
}

void revlink_wifi_credentials_clear(
    revlink_wifi_credentials_t *credentials
)
{
    if (credentials == NULL) {
        return;
    }
    volatile uint8_t *cursor = (volatile uint8_t *)credentials;
    for (size_t remaining = sizeof(*credentials); remaining > 0U; --remaining) {
        *cursor++ = 0U;
    }
}

bool revlink_wifi_password_valid(
    const char *password,
    bool allow_empty
)
{
    if (password == NULL) {
        return false;
    }
    const size_t length =
        strnlen(password, REVLINK_WIFI_PASSWORD_CAPACITY);
    return (
        (length == 0U && allow_empty)
        || (length >= 8U && length < REVLINK_WIFI_PASSWORD_CAPACITY)
    ) && printable_ascii(password, length);
}

bool revlink_wifi_credentials_assign(
    revlink_wifi_credentials_t *credentials,
    const char *ssid,
    const char *password
)
{
    if (credentials == NULL || ssid == NULL || password == NULL) {
        return false;
    }

    const size_t ssid_length = strnlen(ssid, REVLINK_WIFI_SSID_CAPACITY);
    const size_t password_length =
        strnlen(password, REVLINK_WIFI_PASSWORD_CAPACITY);
    if (
        ssid_length == 0U
        || ssid_length >= REVLINK_WIFI_SSID_CAPACITY
        || !printable_ascii(ssid, ssid_length)
        || !revlink_wifi_password_valid(password, true)
    ) {
        return false;
    }

    revlink_wifi_credentials_clear(credentials);
    memcpy(credentials->ssid, ssid, ssid_length);
    memcpy(credentials->password, password, password_length);
    return true;
}
