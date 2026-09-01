#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revlink_identity.h"

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    const uint8_t mac[REVLINK_IDENTITY_MAC_BYTES] = {
        0xac, 0xeb, 0xe6, 0x39, 0xe8, 0x71,
    };
    char ssid[REVLINK_IDENTITY_SSID_CAPACITY] = {0};
    char hostname[REVLINK_IDENTITY_HOSTNAME_CAPACITY] = {0};
    char mac_text[REVLINK_IDENTITY_MAC_TEXT_CAPACITY] = {0};
    require(
        revlink_identity_format_local(
            mac,
            0U,
            ssid,
            sizeof(ssid),
            hostname,
            sizeof(hostname),
            mac_text,
            sizeof(mac_text)
        ),
        "formats base local identity"
    );
    require(
        strcmp(ssid, "RevLink-E639E871") == 0,
        "SSID uses the last eight MAC hex digits"
    );
    require(
        strcmp(hostname, "revlink-e639e871") == 0,
        "hostname uses lowercase last-eight suffix"
    );
    require(
        strcmp(mac_text, "ac:eb:e6:39:e8:71") == 0,
        "formats full hardware MAC"
    );

    require(
        revlink_identity_format_local(
            mac,
            2U,
            ssid,
            sizeof(ssid),
            hostname,
            sizeof(hostname),
            mac_text,
            sizeof(mac_text)
        ),
        "formats collision fallback"
    );
    require(
        strcmp(ssid, "RevLink-E639E871-2") == 0
            && strcmp(hostname, "revlink-e639e871-2") == 0,
        "collision fallback stays paired"
    );
    require(
        !revlink_identity_format_local(
            mac,
            1U,
            ssid,
            sizeof(ssid),
            hostname,
            sizeof(hostname),
            mac_text,
            sizeof(mac_text)
        ),
        "rejects ambiguous collision index one"
    );

    uint8_t random_bytes[REVLINK_IDENTITY_RANDOM_BYTES] = {0};
    for (size_t index = 0U; index < sizeof(random_bytes); ++index) {
        random_bytes[index] = (uint8_t)index;
    }
    char device_id[REVLINK_IDENTITY_DEVICE_ID_CAPACITY] = {0};
    require(
        revlink_identity_format_device_id(
            random_bytes,
            device_id,
            sizeof(device_id)
        ),
        "formats stable internal ID"
    );
    require(
        strcmp(
            device_id,
            "r1_000102030405060708090a0b0c0d0e0f"
        ) == 0,
        "internal ID carries 128 random bits"
    );
    require(
        revlink_identity_device_id_valid(device_id),
        "accepts a generated internal ID"
    );
    device_id[3] = 'G';
    require(
        !revlink_identity_device_id_valid(device_id),
        "rejects non-canonical internal ID"
    );

    puts("identity tests passed");
    return 0;
}
