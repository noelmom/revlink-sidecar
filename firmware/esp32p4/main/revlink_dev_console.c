#include "revlink_dev_console.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "revlink_cli.h"
#if CONFIG_REVLINK_LOCAL_METADATA_BACKFILL_ACCEPTANCE
#include "revlink_local_metadata.h"
#endif
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
#include "revlink_network_runtime.h"
#endif
#include "revlink_runtime.h"
#include "revlink_sidecar_identity.h"
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
#include "revlink_wifi_radio.h"
#endif

#define REVLINK_CONSOLE_LINE_CAPACITY 64U
#define REVLINK_CONSOLE_RESPONSE_CAPACITY 256U
#define REVLINK_CONSOLE_TASK_STACK_BYTES 12288U
#define REVLINK_WIFI_SSID_CAPACITY 33U
#define REVLINK_WIFI_PASSWORD_CAPACITY 65U

static const char *TAG = "revlink_console";
static bool task_started;

#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
static void clear_sensitive(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;
    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

static bool read_private_line(char *output, size_t capacity, bool allow_empty)
{
    size_t length = 0U;
    while (true) {
        const int character = getchar();
        if (character == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        if (character == '\r' || character == '\n') {
            if (length > 0U || allow_empty) {
                output[length] = '\0';
                return true;
            }
            continue;
        }
        if (character < 0x20 || character > 0x7e) {
            continue;
        }
        if (length + 1U >= capacity) {
            clear_sensitive(output, capacity);
            return false;
        }
        output[length++] = (char)character;
    }
}

static void execute_wifi_join(void)
{
    char ssid[REVLINK_WIFI_SSID_CAPACITY] = {0};
    char password[REVLINK_WIFI_PASSWORD_CAPACITY] = {0};

    printf("SSID (input hidden): ");
    if (!read_private_line(ssid, sizeof(ssid), false)) {
        printf("\nerror: SSID too long\n");
        return;
    }
    printf("\nPassword (input hidden; blank allowed): ");
    if (!read_private_line(password, sizeof(password), true)) {
        clear_sensitive(ssid, sizeof(ssid));
        printf("\nerror: password too long\n");
        return;
    }
    printf("\nAttempting preferred Wi-Fi connection...\n");
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    const esp_err_t join_status =
        revlink_network_runtime_configure_station(ssid, password);
#else
    const esp_err_t join_status =
        revlink_wifi_radio_connect_ephemeral(ssid, password, 20000U);
#endif
    clear_sensitive(ssid, sizeof(ssid));
    clear_sensitive(password, sizeof(password));
    if (join_status != ESP_OK) {
        printf("wifi acceptance: join failed (%s)\n",
            esp_err_to_name(join_status));
        return;
    }
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    const esp_err_t transition_status =
        revlink_wifi_radio_finish_onboarding_transition();
    if (transition_status != ESP_OK) {
        printf(
            "wifi runtime: joined; transition failed (%s)\n",
            esp_err_to_name(transition_status)
        );
        return;
    }
    printf(
        "wifi runtime: preferred network connected; "
        "check wifi-status for persistence state\n"
    );
#else
    const esp_err_t dns_status = revlink_wifi_radio_probe_dns();
    if (dns_status != ESP_OK) {
        printf("wifi acceptance: joined; DNS probe failed (%s)\n",
            esp_err_to_name(dns_status));
        return;
    }
    printf("wifi acceptance: PASS (association + DHCP + DNS)\n");
#endif
}

#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
static void execute_wifi_hotspot(void)
{
    char password[REVLINK_WIFI_PASSWORD_CAPACITY] = {0};
    printf("Hotspot password (input hidden; 8-63 characters): ");
    if (!read_private_line(password, sizeof(password), false)) {
        printf("\nerror: password too long\n");
        return;
    }
    const esp_err_t status =
        revlink_network_runtime_configure_hotspot_ephemeral(password);
    clear_sensitive(password, sizeof(password));
    if (status != ESP_OK) {
        printf(
            "\nerror: unable to start fallback hotspot (%s)\n",
            esp_err_to_name(status)
        );
        return;
    }
    printf(
        "\nFallback hotspot ready. SSID is RevLink-<device suffix>; "
        "credential remains RAM-only.\n"
    );
}

static void execute_wifi_status(void)
{
    const revlink_network_runtime_snapshot_t snapshot =
        revlink_network_runtime_snapshot();
    printf(
        "network=%s radio=%u station=%s saved=%s hotspot=%s "
        "waiting-credential=%s "
        "aps=%u clients=%u transfer=%s health-failures=%u error=%d\n",
        revlink_network_state_name(snapshot.coordinator.state),
        (unsigned int)snapshot.radio.state,
        snapshot.station_configured ? "configured" : "none",
        snapshot.station_credentials_persistent ? "yes" : "no",
        snapshot.hotspot_configured ? "configured" : "none",
        snapshot.awaiting_hotspot_credential ? "yes" : "no",
        (unsigned int)snapshot.radio.access_point_count,
        (unsigned int)snapshot.radio.hotspot_client_count,
        snapshot.coordinator.transfer_active ? "active" : "idle",
        (unsigned int)snapshot.coordinator.consecutive_health_failures,
        (int)snapshot.coordinator.last_platform_error
    );
}
#endif
#endif

static void execute_line(const char *line)
{
#if CONFIG_REVLINK_LOCAL_METADATA_BACKFILL_ACCEPTANCE
    static const char resequence_prefix[] =
        "resequence-wrapped-datalogs ";
    if (strncmp(
            line,
            resequence_prefix,
            sizeof(resequence_prefix) - 1U
        ) == 0) {
        const char *value = line + sizeof(resequence_prefix) - 1U;
        char *separator = NULL;
        errno = 0;
        const unsigned long long first = strtoull(value, &separator, 10);
        if (
            errno != 0
            || separator == value
            || separator == NULL
            || *separator != ' '
        ) {
            printf("error: expected <unix-epoch> <seconds>\n");
            return;
        }
        char *end = NULL;
        errno = 0;
        const unsigned long step = strtoul(separator + 1U, &end, 10);
        if (
            errno != 0
            || end == separator + 1U
            || end == NULL
            || *end != '\0'
            || first == 0U
            || step == 0U
            || step > UINT32_MAX
        ) {
            printf("error: expected <unix-epoch> <seconds>\n");
            return;
        }
        size_t manifest_updated = 0U;
        size_t history_updated = 0U;
        const esp_err_t status =
            revlink_local_metadata_resequence_wrapped_datalogs(
                (uint64_t)first,
                (uint32_t)step,
                &manifest_updated,
                &history_updated
            );
        if (status != ESP_OK) {
            printf(
                "local datalog resequence failed (%s)\n",
                esp_err_to_name(status)
            );
            return;
        }
        printf(
            "local datalog resequence: start=%llu step=%lu "
            "current=%u history=%u\n",
            first,
            step,
            (unsigned int)manifest_updated,
            (unsigned int)history_updated
        );
        return;
    }
    static const char backfill_prefix[] = "backfill-initial-sync ";
    if (strncmp(
            line,
            backfill_prefix,
            sizeof(backfill_prefix) - 1U
        ) == 0) {
        const char *value = line + sizeof(backfill_prefix) - 1U;
        char *end = NULL;
        errno = 0;
        const unsigned long long parsed = strtoull(value, &end, 10);
        if (
            errno != 0
            || end == value
            || end == NULL
            || *end != '\0'
            || parsed == 0U
        ) {
            printf("error: expected a non-zero Unix epoch\n");
            return;
        }
        size_t manifest_updated = 0U;
        size_t history_updated = 0U;
        const esp_err_t status =
            revlink_local_metadata_backfill_initial_sync(
                (uint64_t)parsed,
                &manifest_updated,
                &history_updated
            );
        if (status != ESP_OK) {
            printf(
                "local metadata backfill failed (%s)\n",
                esp_err_to_name(status)
            );
            return;
        }
        printf(
            "local metadata backfill: epoch=%llu current=%u history=%u\n",
            parsed,
            (unsigned int)manifest_updated,
            (unsigned int)history_updated
        );
        return;
    }
#endif
    if (strcmp(line, "identity") == 0) {
        revlink_sidecar_identity_t identity = {0};
        const esp_err_t status =
            revlink_sidecar_identity_snapshot(&identity);
        if (status != ESP_OK) {
            printf("identity unavailable (%s)\n", esp_err_to_name(status));
            return;
        }
        printf(
            "device_id=%s hostname=%s ssid=%s hardware_mac=%s "
            "collision=%u\n",
            identity.device_id,
            identity.hostname,
            identity.ssid,
            identity.hardware_mac,
            (unsigned int)identity.collision_index
        );
        return;
    }
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    if (strcmp(line, "wifi-join") == 0) {
        execute_wifi_join();
        return;
    }
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    if (strcmp(line, "wifi-hotspot") == 0) {
        execute_wifi_hotspot();
        return;
    }
    if (strcmp(line, "wifi-fallback") == 0) {
        const esp_err_t status = revlink_network_runtime_force_hotspot();
        printf(
            "wifi fallback: %s\n",
            status == ESP_OK ? "requested" : esp_err_to_name(status)
        );
        return;
    }
    if (strcmp(line, "wifi-status") == 0) {
        execute_wifi_status();
        return;
    }
#endif
#endif
    revlink_control_request_t request = {0};
    const revlink_cli_parse_status_t parse_status =
        revlink_cli_parse(line, &request);
    if (parse_status == REVLINK_CLI_HELP) {
        printf("%s\n", revlink_cli_help());
        return;
    }
    if (parse_status != REVLINK_CLI_PARSED) {
        printf("error: unknown command; %s\n", revlink_cli_help());
        return;
    }

    revlink_control_response_t response = {0};
    (void)revlink_runtime_control_execute(&request, &response);
    char formatted[REVLINK_CONSOLE_RESPONSE_CAPACITY] = {0};
    if (!revlink_cli_format_response(
        &response,
        formatted,
        sizeof(formatted)
    )) {
        printf("error: unable to format response\n");
        return;
    }
    printf("%s\n", formatted);
}

static void console_task(void *context)
{
    (void)context;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    char line[REVLINK_CONSOLE_LINE_CAPACITY] = {0};
    size_t line_length = 0U;
    bool line_overflow = false;

    printf("\nRevLink development control ready\n%s\nidentity\nrevlink> ",
        revlink_cli_help());
#if CONFIG_REVLINK_WIFI_JOIN_ACCEPTANCE
    printf("\nLocal radio acceptance command available: wifi-join\nrevlink> ");
#if CONFIG_REVLINK_NETWORK_RUNTIME_ACCEPTANCE
    printf(
        "\nProduct-network commands: wifi-hotspot, wifi-status, "
        "wifi-fallback\nrevlink> "
    );
#endif
#endif
#if CONFIG_REVLINK_LOCAL_METADATA_BACKFILL_ACCEPTANCE
    printf(
        "\nMaintenance commands available: "
        "backfill-initial-sync <unix-epoch>, "
        "resequence-wrapped-datalogs <unix-epoch> <seconds>\nrevlink> "
    );
#endif
    while (true) {
        const int character = getchar();
        if (character == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        if (character == '\r' || character == '\n') {
            if (line_overflow) {
                printf("error: command too long\n");
            } else if (line_length > 0U) {
                line[line_length] = '\0';
                execute_line(line);
            }
            line_length = 0U;
            line_overflow = false;
            line[0] = '\0';
            printf("revlink> ");
            continue;
        }
        if (character < 0x20 || character > 0x7e || line_overflow) {
            continue;
        }
        if (line_length + 1U >= sizeof(line)) {
            line_overflow = true;
            continue;
        }
        line[line_length++] = (char)character;
    }
}

esp_err_t revlink_dev_console_start(void)
{
    if (task_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const BaseType_t created = xTaskCreate(
        console_task,
        "revlink_console",
        REVLINK_CONSOLE_TASK_STACK_BYTES,
        NULL,
        4,
        NULL
    );
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Unable to create development console task");
        return ESP_ERR_NO_MEM;
    }
    task_started = true;
    return ESP_OK;
}
