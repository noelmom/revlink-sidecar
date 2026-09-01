#include "revlink_identity.h"

#include <stdio.h>
#include <string.h>

bool revlink_identity_format_local(
    const uint8_t hardware_mac[REVLINK_IDENTITY_MAC_BYTES],
    uint16_t collision_index,
    char *ssid,
    size_t ssid_capacity,
    char *hostname,
    size_t hostname_capacity,
    char *mac_text,
    size_t mac_text_capacity
)
{
    if (
        hardware_mac == NULL
        || ssid == NULL
        || hostname == NULL
        || mac_text == NULL
        || collision_index == 1U
    ) {
        return false;
    }

    const char *suffix_format = collision_index == 0U ? "" : "-%u";
    char suffix[8] = {0};
    const int suffix_length = collision_index == 0U
        ? 0
        : snprintf(
            suffix,
            sizeof(suffix),
            suffix_format,
            (unsigned int)collision_index
        );
    if (
        suffix_length < 0
        || (size_t)suffix_length >= sizeof(suffix)
    ) {
        return false;
    }

    const int ssid_length = snprintf(
        ssid,
        ssid_capacity,
        "RevLink-%02X%02X%02X%02X%s",
        hardware_mac[2],
        hardware_mac[3],
        hardware_mac[4],
        hardware_mac[5],
        suffix
    );
    const int hostname_length = snprintf(
        hostname,
        hostname_capacity,
        "revlink-%02x%02x%02x%02x%s",
        hardware_mac[2],
        hardware_mac[3],
        hardware_mac[4],
        hardware_mac[5],
        suffix
    );
    const int mac_length = snprintf(
        mac_text,
        mac_text_capacity,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        hardware_mac[0],
        hardware_mac[1],
        hardware_mac[2],
        hardware_mac[3],
        hardware_mac[4],
        hardware_mac[5]
    );
    return ssid_length > 0
        && hostname_length > 0
        && mac_length > 0
        && (size_t)ssid_length < ssid_capacity
        && (size_t)hostname_length < hostname_capacity
        && (size_t)mac_length < mac_text_capacity;
}

bool revlink_identity_format_device_id(
    const uint8_t random_bytes[REVLINK_IDENTITY_RANDOM_BYTES],
    char *device_id,
    size_t capacity
)
{
    static const char hex[] = "0123456789abcdef";
    if (
        random_bytes == NULL
        || device_id == NULL
        || capacity < REVLINK_IDENTITY_DEVICE_ID_CAPACITY
    ) {
        return false;
    }
    memcpy(device_id, "r1_", 3U);
    for (size_t index = 0U; index < REVLINK_IDENTITY_RANDOM_BYTES; ++index) {
        device_id[3U + index * 2U] = hex[random_bytes[index] >> 4U];
        device_id[4U + index * 2U] = hex[random_bytes[index] & 0x0fU];
    }
    device_id[REVLINK_IDENTITY_DEVICE_ID_CAPACITY - 1U] = '\0';
    return true;
}

bool revlink_identity_device_id_valid(const char *device_id)
{
    if (
        device_id == NULL
        || strnlen(
            device_id,
            REVLINK_IDENTITY_DEVICE_ID_CAPACITY + 1U
        ) != REVLINK_IDENTITY_DEVICE_ID_CAPACITY - 1U
        || memcmp(device_id, "r1_", 3U) != 0
    ) {
        return false;
    }
    for (
        size_t index = 3U;
        index < REVLINK_IDENTITY_DEVICE_ID_CAPACITY - 1U;
        ++index
    ) {
        const char value = device_id[index];
        if (
            !((value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f'))
        ) {
            return false;
        }
    }
    return true;
}
