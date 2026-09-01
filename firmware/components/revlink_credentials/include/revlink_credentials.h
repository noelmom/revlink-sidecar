#ifndef REVLINK_CREDENTIALS_H
#define REVLINK_CREDENTIALS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_WIFI_SSID_CAPACITY 33U
#define REVLINK_WIFI_PASSWORD_CAPACITY 64U

typedef struct {
    char ssid[REVLINK_WIFI_SSID_CAPACITY];
    char password[REVLINK_WIFI_PASSWORD_CAPACITY];
} revlink_wifi_credentials_t;

/*
 * Copies and validates one station credential without allocating memory.
 * Passwords may be empty for an open network or 8-63 printable ASCII bytes.
 */
bool revlink_wifi_credentials_assign(
    revlink_wifi_credentials_t *credentials,
    const char *ssid,
    const char *password
);

bool revlink_wifi_password_valid(
    const char *password,
    bool allow_empty
);

/*
 * Overwrites the complete credential object through a volatile pointer.
 */
void revlink_wifi_credentials_clear(
    revlink_wifi_credentials_t *credentials
);

#ifdef __cplusplus
}
#endif

#endif
