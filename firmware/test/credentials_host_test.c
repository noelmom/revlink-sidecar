#include <assert.h>
#include <string.h>

#include "revlink_credentials.h"

int main(void)
{
    revlink_wifi_credentials_t credentials = {0};
    assert(
        revlink_wifi_credentials_assign(
            &credentials,
            "Garage WiFi",
            "safe-test-password"
        )
    );
    assert(strcmp(credentials.ssid, "Garage WiFi") == 0);
    assert(strcmp(credentials.password, "safe-test-password") == 0);

    assert(
        revlink_wifi_credentials_assign(
            &credentials,
            "Open Network",
            ""
        )
    );
    assert(strcmp(credentials.ssid, "Open Network") == 0);
    assert(credentials.password[0] == '\0');

    assert(!revlink_wifi_credentials_assign(&credentials, "", "12345678"));
    assert(
        !revlink_wifi_credentials_assign(
            &credentials,
            "Garage WiFi",
            "short"
        )
    );
    assert(
        !revlink_wifi_credentials_assign(
            &credentials,
            "bad\nssid",
            "safe-test-password"
        )
    );
    assert(revlink_wifi_password_valid("12345678", false));
    assert(!revlink_wifi_password_valid("", false));
    assert(revlink_wifi_password_valid("", true));

    revlink_wifi_credentials_clear(&credentials);
    for (size_t index = 0U; index < sizeof(credentials); ++index) {
        assert(((const unsigned char *)&credentials)[index] == 0U);
    }
    return 0;
}
