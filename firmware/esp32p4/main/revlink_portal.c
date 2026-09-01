#include "revlink_portal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "revlink_accessport_catalog.h"
#include "revlink_backup.h"
#include "revlink_control_service.h"
#include "revlink_device_service.h"
#include "revlink_map_upload.h"
#include "revlink_network_runtime.h"
#include "revlink_runtime.h"
#include "revlink_sd_storage.h"
#include "revlink_sidecar_identity.h"
#include "revlink_sync_coordinator.h"

extern const uint8_t portal_index_html_start[]
    asm("_binary_index_html_start");
extern const uint8_t portal_index_html_end[]
    asm("_binary_index_html_end");
extern const uint8_t portal_assets_accessport_render_webp_start[]
    asm("_binary_accessport_render_webp_start");
extern const uint8_t portal_assets_accessport_render_webp_end[]
    asm("_binary_accessport_render_webp_end");
extern const uint8_t portal_assets_revlink_wordmark_png_start[]
    asm("_binary_revlink_wordmark_png_start");
extern const uint8_t portal_assets_revlink_wordmark_png_end[]
    asm("_binary_revlink_wordmark_png_end");

#define PORTAL_ACTION_BODY_LIMIT 32U
#define PORTAL_NOTE_BODY_LIMIT 2600U
#define PORTAL_LOG_MAP_BODY_LIMIT 160U
#define PORTAL_TIME_BODY_LIMIT 32U
#define PORTAL_TIME_MINIMUM_UTC 1577836800ULL
#define PORTAL_TIME_MAXIMUM_UTC 4102444800ULL
#define PORTAL_FILE_QUERY_LIMIT 96U
#define PORTAL_FILE_STREAM_BYTES 4096U
#define PORTAL_MAP_STREAM_BYTES 4096U
#define PORTAL_STARTUP_APPLY_BODY_LIMIT 96U
#define PORTAL_BACKUP_RESTORE_BODY_LIMIT 80U

static void *portal_time_context;
static revlink_portal_time_observer_t portal_time_observer;

static bool refresh_identity_field(
    char *cached,
    size_t capacity,
    const char *attached
)
{
    if (
        cached == NULL || capacity == 0U || attached == NULL
        || attached[0] == '\0' || strcmp(cached, attached) == 0
    ) {
        return false;
    }
    const size_t length = strnlen(attached, capacity);
    if (length >= capacity) {
        return false;
    }
    memcpy(cached, attached, length + 1U);
    return true;
}

static void refresh_selected_identity_from_attached(
    revlink_sd_portal_snapshot_t *storage,
    bool attached_known,
    const revlink_ap_device_info_t *attached
)
{
    if (
        storage == NULL || !storage->namespace_known || !attached_known
        || attached == NULL || attached->serial[0] == '\0'
        || strcmp(storage->device.serial, attached->serial) != 0
    ) {
        return;
    }
    refresh_identity_field(
        storage->device.part_number,
        sizeof(storage->device.part_number),
        attached->part_number
    );
    refresh_identity_field(
        storage->device.firmware,
        sizeof(storage->device.firmware),
        attached->firmware
    );
    refresh_identity_field(
        storage->device.install_status,
        sizeof(storage->device.install_status),
        attached->install_status
    );
    refresh_identity_field(
        storage->device.vehicle,
        sizeof(storage->device.vehicle),
        attached->vehicle
    );
}

void revlink_portal_configure_time_observer(
    void *context,
    revlink_portal_time_observer_t observer
)
{
    portal_time_context = context;
    portal_time_observer = observer;
}

static const char *TAG = "revlink_portal";

/*
 * Handlers run on the HTTP server task, so this measures the deepest that
 * task has been. Reported only when it reaches a new low, so a healthy
 * Sidecar stays quiet and a shrinking margin is visible before it becomes a
 * stack protection fault. ESP-IDF returns bytes here, not words.
 */
static void report_http_stack_headroom(void)
{
    static UBaseType_t lowest = (UBaseType_t)-1;
    const UBaseType_t headroom = uxTaskGetStackHighWaterMark(NULL);
    if (headroom < lowest) {
        lowest = headroom;
        ESP_LOGI(TAG, "http task stack: %u bytes free", (unsigned int)headroom);
    }
}

static esp_err_t send_json(
    httpd_req_t *request,
    const char *status,
    const char *json
)
{
    report_http_stack_headroom();
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_sendstr(request, json);
}

static bool portal_header_is_valid(httpd_req_t *request)
{
    const size_t length =
        httpd_req_get_hdr_value_len(request, "X-RevLink-Portal");
    if (length != 1U) {
        return false;
    }
    char value[2] = {0};
    return httpd_req_get_hdr_value_str(
        request,
        "X-RevLink-Portal",
        value,
        sizeof(value)
    ) == ESP_OK && value[0] == '1';
}

static const char *control_http_status(revlink_control_status_t status)
{
    switch (status) {
    case REVLINK_CONTROL_OK:
        return HTTPD_200;
    case REVLINK_CONTROL_INVALID_ARGUMENT:
        return HTTPD_400;
    case REVLINK_CONTROL_INVALID_STATE:
        return "409 Conflict";
    case REVLINK_CONTROL_NOT_SUPPORTED:
        return "501 Not Implemented";
    case REVLINK_CONTROL_TRANSPORT_ERROR:
    default:
        return "503 Service Unavailable";
    }
}

static const char *upload_state_name(
    revlink_accessport_upload_state_t state
)
{
    switch (state) {
    case REVLINK_ACCESSPORT_UPLOAD_RUNNING:
        return "running";
    case REVLINK_ACCESSPORT_UPLOAD_VERIFIED:
        return "verified";
    case REVLINK_ACCESSPORT_UPLOAD_FAILED:
        return "failed";
    case REVLINK_ACCESSPORT_UPLOAD_IDLE:
    default:
        return "idle";
    }
}

static const char *upload_kind_name(revlink_ap_upload_kind_t kind)
{
    return kind == REVLINK_AP_UPLOAD_STARTUP_SCREEN
        ? "startup" : "map";
}

static bool json_escape(
    const char *input,
    char *output,
    size_t capacity
)
{
    if (input == NULL || output == NULL || capacity < 3U) {
        return false;
    }
    size_t used = 0U;
    output[used++] = '"';
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)input[index];
        if (value == '"' || value == '\\') {
            if (used + 2U >= capacity) {
                return false;
            }
            output[used++] = '\\';
            output[used++] = (char)value;
        } else if (value < 0x20U) {
            if (used + 6U >= capacity) {
                return false;
            }
            const int length = snprintf(
                &output[used],
                capacity - used,
                "\\u%04x",
                value
            );
            if (length != 6) {
                return false;
            }
            used += 6U;
        } else {
            if (used + 1U >= capacity) {
                return false;
            }
            output[used++] = (char)value;
        }
    }
    if (used + 2U > capacity) {
        return false;
    }
    output[used++] = '"';
    output[used] = '\0';
    return true;
}

static int portal_hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool decode_digest(
    const char *text,
    size_t length,
    uint8_t digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
)
{
    if (text == NULL || digest == NULL
        || length != REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U) {
        return false;
    }
    for (
        size_t index = 0U;
        index < REVLINK_SYNC_ANNOTATION_SHA256_BYTES;
        ++index
    ) {
        const int high = portal_hex_value(text[index * 2U]);
        const int low = portal_hex_value(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        digest[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

static bool form_decode(
    const char *input,
    size_t input_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (input == NULL || output == NULL || output_length == NULL) return false;
    size_t used = 0U;
    for (size_t index = 0U; index < input_length; ++index) {
        uint8_t value = (uint8_t)input[index];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (index + 2U >= input_length) return false;
            const int high = portal_hex_value(input[index + 1U]);
            const int low = portal_hex_value(input[index + 2U]);
            if (high < 0 || low < 0) return false;
            value = (uint8_t)((high << 4U) | low);
            index += 2U;
        }
        if (value == '\r') continue;
        if (value == '\0' || used + 1U >= output_capacity) return false;
        output[used++] = (char)value;
    }
    output[used] = '\0';
    *output_length = used;
    return true;
}

static esp_err_t portal_status_handler(httpd_req_t *request)
{
    const revlink_control_request_t control_request = {
        .command = REVLINK_CONTROL_GET_STATUS,
    };
    revlink_control_response_t control = {0};
    const revlink_control_status_t control_status =
        revlink_runtime_control_execute(&control_request, &control);
    if (control_status != REVLINK_CONTROL_OK) {
        return send_json(
            request,
            control_http_status(control_status),
            "{\"error\":\"Runtime status is unavailable\"}"
        );
    }

    revlink_sd_portal_snapshot_t *storage = calloc(1U, sizeof(*storage));
    if (storage == NULL) {
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Storage status allocation failed\"}"
        );
    }
    const esp_err_t storage_status =
        revlink_sd_portal_snapshot(storage);
    if (storage_status != ESP_OK) {
        free(storage);
        return send_json(
            request,
            "503 Service Unavailable",
            "{\"error\":\"Storage status is unavailable\"}"
        );
    }

    revlink_sidecar_identity_t sidecar = {0};
    if (revlink_sidecar_identity_snapshot(&sidecar) != ESP_OK) {
        free(storage);
        return send_json(
            request,
            "503 Service Unavailable",
            "{\"error\":\"Sidecar identity is unavailable\"}"
        );
    }
    const revlink_network_runtime_snapshot_t network =
        revlink_network_runtime_snapshot();
    revlink_map_upload_snapshot_t upload = {0};
    if (revlink_map_upload_snapshot(&upload) != ESP_OK) {
        upload.writes_compiled = control.snapshot.writes_compiled;
    }
    bool attached_identity_known = false;
    revlink_ap_device_info_t attached_identity = {0};
    if (revlink_runtime_connected_accessport_snapshot(
            &attached_identity_known,
            &attached_identity
        ) != ESP_OK) {
        attached_identity_known = false;
    }
    refresh_selected_identity_from_attached(
        storage,
        attached_identity_known,
        &attached_identity
    );
    revlink_accessport_catalog_entry_t selected_catalog = {0};
    revlink_accessport_catalog_entry_t attached_catalog = {0};
    const bool selected_catalog_supported =
        storage->namespace_known
        && revlink_accessport_catalog_lookup(
            storage->device.part_number,
            &selected_catalog
        );
    const bool attached_catalog_supported =
        attached_identity_known
        && revlink_accessport_catalog_lookup(
            attached_identity.part_number,
            &attached_catalog
        );

    char serial[REVLINK_AP_SERIAL_CAPACITY * 2U + 3U];
    char part_number[REVLINK_AP_PART_NUMBER_CAPACITY * 2U + 3U];
    char firmware[REVLINK_AP_FIRMWARE_CAPACITY * 2U + 3U];
    char vehicle[REVLINK_AP_VEHICLE_CAPACITY * 2U + 3U];
    char install_status[REVLINK_AP_INSTALL_STATUS_CAPACITY * 2U + 3U];
    char attached_serial[REVLINK_AP_SERIAL_CAPACITY * 2U + 3U];
    char attached_part_number[
        REVLINK_AP_PART_NUMBER_CAPACITY * 2U + 3U
    ];
    char attached_firmware[REVLINK_AP_FIRMWARE_CAPACITY * 2U + 3U];
    char attached_vehicle[REVLINK_AP_VEHICLE_CAPACITY * 2U + 3U];
    char attached_install_status[
        REVLINK_AP_INSTALL_STATUS_CAPACITY * 2U + 3U
    ];
    char selected_family_code[16U];
    char selected_family_name[64U];
    char attached_family_code[16U];
    char attached_family_name[64U];
    char escaped_ssid[sizeof(sidecar.ssid) * 2U + 3U];
    char escaped_connected_ssid[
        sizeof(network.connected_ssid) * 2U + 3U
    ];
    char escaped_hostname[sizeof(sidecar.hostname) * 2U + 3U];
    char escaped_device_id[sizeof(sidecar.device_id) * 2U + 3U];
    char escaped_hardware_mac[sizeof(sidecar.hardware_mac) * 2U + 3U];
    char escaped_upload_name[
        REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY * 2U + 3U
    ];
    char escaped_upload_destination[
        REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY * 2U + 3U
    ];
    char escaped_upload_target[
        REVLINK_AP_PART_NUMBER_CAPACITY * 2U + 3U
    ];
    const esp_app_desc_t *app_description = esp_app_get_description();
    char escaped_build_version[
        sizeof(app_description->version) * 2U + 3U
    ];
    char escaped_build_date[
        sizeof(app_description->date) * 2U + 3U
    ];
    char escaped_build_time[
        sizeof(app_description->time) * 2U + 3U
    ];
    if (
        !json_escape(storage->device.serial, serial, sizeof(serial))
        || !json_escape(
            storage->device.part_number,
            part_number,
            sizeof(part_number)
        )
        || !json_escape(
            storage->device.firmware,
            firmware,
            sizeof(firmware)
        )
        || !json_escape(storage->device.vehicle, vehicle, sizeof(vehicle))
        || !json_escape(
            storage->device.install_status,
            install_status,
            sizeof(install_status)
        )
        || !json_escape(
            attached_identity.serial,
            attached_serial,
            sizeof(attached_serial)
        )
        || !json_escape(
            attached_identity.part_number,
            attached_part_number,
            sizeof(attached_part_number)
        )
        || !json_escape(
            attached_identity.firmware,
            attached_firmware,
            sizeof(attached_firmware)
        )
        || !json_escape(
            attached_identity.vehicle,
            attached_vehicle,
            sizeof(attached_vehicle)
        )
        || !json_escape(
            attached_identity.install_status,
            attached_install_status,
            sizeof(attached_install_status)
        )
        || !json_escape(
            selected_catalog_supported
                ? selected_catalog.family_code : "",
            selected_family_code,
            sizeof(selected_family_code)
        )
        || !json_escape(
            selected_catalog_supported
                ? selected_catalog.family_name : "",
            selected_family_name,
            sizeof(selected_family_name)
        )
        || !json_escape(
            attached_catalog_supported
                ? attached_catalog.family_code : "",
            attached_family_code,
            sizeof(attached_family_code)
        )
        || !json_escape(
            attached_catalog_supported
                ? attached_catalog.family_name : "",
            attached_family_name,
            sizeof(attached_family_name)
        )
        || !json_escape(sidecar.ssid, escaped_ssid, sizeof(escaped_ssid))
        || !json_escape(
            network.connected_ssid,
            escaped_connected_ssid,
            sizeof(escaped_connected_ssid)
        )
        || !json_escape(
            sidecar.hostname,
            escaped_hostname,
            sizeof(escaped_hostname)
        )
        || !json_escape(
            sidecar.device_id,
            escaped_device_id,
            sizeof(escaped_device_id)
        )
        || !json_escape(
            sidecar.hardware_mac,
            escaped_hardware_mac,
            sizeof(escaped_hardware_mac)
        )
        || !json_escape(
            upload.name,
            escaped_upload_name,
            sizeof(escaped_upload_name)
        )
        || !json_escape(
            upload.destination,
            escaped_upload_destination,
            sizeof(escaped_upload_destination)
        )
        || !json_escape(
            upload.target_part_number,
            escaped_upload_target,
            sizeof(escaped_upload_target)
        )
        || !json_escape(
            app_description->version,
            escaped_build_version,
            sizeof(escaped_build_version)
        )
        || !json_escape(
            app_description->date,
            escaped_build_date,
            sizeof(escaped_build_date)
        )
        || !json_escape(
            app_description->time,
            escaped_build_time,
            sizeof(escaped_build_time)
        )
    ) {
        free(storage);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Status encoding failed\"}"
        );
    }

    char *response = malloc(6100U);
    if (response == NULL) {
        free(storage);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Status allocation failed\"}"
        );
    }
    const revlink_device_snapshot_t *device = &control.snapshot.device;
    const revlink_sync_snapshot_t *sync = &control.snapshot.sync;
    const bool sync_active =
        sync->state == REVLINK_SYNC_QUEUED
        || sync->state == REVLINK_SYNC_RUNNING
        || sync->state == REVLINK_SYNC_CANCELLING;
    const int length = snprintf(
        response,
        6100U,
        "{"
        "\"build\":{\"version\":%s,\"date\":%s,\"time\":%s},"
        "\"accessPortCatalog\":{\"revision\":\"%s\",\"partCount\":%u},"
        "\"identity\":{\"ssid\":%s,\"hostname\":%s,"
        "\"deviceId\":%s,\"hardwareMac\":%s,\"collisionIndex\":%u},"
        "\"network\":{\"state\":\"%s\",\"ssid\":%s,\"clients\":%u,"
        "\"transferActive\":%s,\"credentialsPersistent\":%s},"
        "\"device\":{\"state\":\"%s\",\"attached\":%s,\"available\":%s,"
        "\"vendorId\":%u,\"productId\":%u,\"highSpeed\":%s,"
        "\"maxPacket\":%u,\"platformError\":%d,\"eligibleCount\":%u,"
        "\"topologyRevision\":%" PRIu32 ","
        "\"conflictRecoveryRequired\":%s},"
        "\"accessPort\":{\"known\":%s,\"serial\":%s,\"partNumber\":%s,"
        "\"firmware\":%s,\"vehicle\":%s,\"installStatus\":%s,"
        "\"catalogSupported\":%s,\"familyCode\":%s,\"familyName\":%s,"
        "\"readOnlyFileSyncSupported\":%s},"
        "\"attachedAccessPort\":{\"known\":%s,\"serial\":%s,"
        "\"partNumber\":%s,\"firmware\":%s,\"vehicle\":%s,"
        "\"installStatus\":%s,\"catalogSupported\":%s,"
        "\"familyCode\":%s,\"familyName\":%s,"
        "\"readOnlyFileSyncSupported\":%s},"
        "\"sync\":{\"state\":\"%s\",\"active\":%s,\"autoSync\":%s,"
        "\"candidates\":%u,\"downloaded\":%u,\"skipped\":%u,"
        "\"bytes\":%" PRIu32 ",\"pending\":%u,\"platformError\":%d,"
        "\"closeRecoveryAttempt\":%s,\"dataPhaseCompleted\":%s,"
        "\"sessionCloseSent\":%s,\"sessionCloseAcknowledged\":%s},"
        "\"storage\":{\"mounted\":%s,\"sessionSelected\":%s,"
        "\"totalBytes\":%" PRIu64 ",\"freeBytes\":%" PRIu64 ","
        "\"totalFiles\":%u,\"listedFiles\":%u},"
        "\"safety\":{\"writesCompiled\":%s,\"deletesCompiled\":%s,"
        "\"shutdownRequested\":%s,\"writeConsent\":%s,"
        "\"writeRecoveryRequired\":%s},"
        "\"mapUpload\":{\"state\":\"%s\",\"kind\":\"%s\","
        "\"staged\":%s,\"name\":%s,"
        "\"destination\":%s,\"size\":%" PRIu32 ",\"platformError\":%d,"
        "\"autoApply\":%s,\"pinned\":%s,\"targetPartNumber\":%s}"
        "}",
        escaped_build_version,
        escaped_build_date,
        escaped_build_time,
        REVLINK_ACCESSPORT_CATALOG_REVISION,
        (unsigned int)revlink_accessport_catalog_count(),
        escaped_ssid,
        escaped_hostname,
        escaped_device_id,
        escaped_hardware_mac,
        (unsigned int)sidecar.collision_index,
        revlink_network_state_name(network.coordinator.state),
        escaped_connected_ssid,
        (unsigned int)network.radio.hotspot_client_count,
        network.coordinator.transfer_active ? "true" : "false",
        network.station_credentials_persistent ? "true" : "false",
        revlink_device_state_name(device->state),
        device->state != REVLINK_DEVICE_WAITING
            && device->state != REVLINK_DEVICE_STOPPED ? "true" : "false",
        device->state == REVLINK_DEVICE_AVAILABLE ? "true" : "false",
        (unsigned int)device->identity.vendor_id,
        (unsigned int)device->identity.product_id,
        device->identity.high_speed ? "true" : "false",
        (unsigned int)device->identity.bulk_max_packet_size,
        device->last_platform_error,
        (unsigned int)device->eligible_device_count,
        device->topology_revision,
        device->conflict_recovery_required ? "true" : "false",
        storage->namespace_known ? "true" : "false",
        serial,
        part_number,
        firmware,
        vehicle,
        install_status,
        selected_catalog_supported ? "true" : "false",
        selected_family_code,
        selected_family_name,
        selected_catalog_supported
                && selected_catalog.read_only_file_sync_supported
            ? "true" : "false",
        attached_identity_known ? "true" : "false",
        attached_serial,
        attached_part_number,
        attached_firmware,
        attached_vehicle,
        attached_install_status,
        attached_catalog_supported ? "true" : "false",
        attached_family_code,
        attached_family_name,
        attached_catalog_supported
                && attached_catalog.read_only_file_sync_supported
            ? "true" : "false",
        revlink_sync_state_name(sync->state),
        sync_active ? "true" : "false",
        control.snapshot.sync_policy.auto_sync_on_attach
            ? "true" : "false",
        (unsigned int)sync->candidates,
        (unsigned int)sync->downloaded,
        (unsigned int)sync->skipped,
        sync->downloaded_bytes,
        (unsigned int)sync->pending,
        sync->last_platform_error,
        sync->close_recovery_attempt ? "true" : "false",
        sync->data_phase_completed ? "true" : "false",
        sync->session_close_sent ? "true" : "false",
        sync->session_close_acknowledged ? "true" : "false",
        storage->mounted ? "true" : "false",
        storage->session_selected ? "true" : "false",
        storage->total_bytes,
        storage->free_bytes,
        (unsigned int)storage->total_files,
        (unsigned int)storage->listed_files,
        control.snapshot.writes_compiled ? "true" : "false",
        control.snapshot.deletes_compiled ? "true" : "false",
        control.snapshot.shutdown_requested ? "true" : "false",
        upload.consent_enabled ? "true" : "false",
        upload.recovery_required ? "true" : "false",
        upload_state_name(upload.state),
        upload_kind_name(upload.kind),
        upload.staged ? "true" : "false",
        escaped_upload_name,
        escaped_upload_destination,
        upload.size,
        upload.platform_error,
        upload.auto_apply_enabled ? "true" : "false",
        upload.target_serial[0] != '\0' ? "true" : "false",
        escaped_upload_target
    );
    free(storage);
    if (length <= 0 || length >= 6100) {
        free(response);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Status response exceeded its bound\"}"
        );
    }
    const esp_err_t status = send_json(request, HTTPD_200, response);
    free(response);
    return status;
}

static esp_err_t portal_devices_handler(httpd_req_t *request)
{
    revlink_sd_cached_devices_snapshot_t *snapshot =
        calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) {
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Device catalog allocation failed\"}"
        );
    }
    const esp_err_t snapshot_status =
        revlink_sd_cached_devices_snapshot(snapshot);
    if (snapshot_status != ESP_OK) {
        free(snapshot);
        return send_json(
            request,
            "503 Service Unavailable",
            "{\"error\":\"Cached device catalog is unavailable\"}"
        );
    }

    const size_t capacity =
        64U + snapshot->count * 1400U;
    char *response = malloc(capacity);
    if (response == NULL) {
        free(snapshot);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Device catalog response allocation failed\"}"
        );
    }
    size_t used = (size_t)snprintf(
        response,
        capacity,
        "{\"devices\":["
    );
    bool failed = used >= capacity;
    for (size_t index = 0U; index < snapshot->count && !failed; ++index) {
        const revlink_sd_cached_device_t *device =
            &snapshot->devices[index];
        char serial[REVLINK_AP_SERIAL_CAPACITY * 2U + 3U];
        char part[REVLINK_AP_PART_NUMBER_CAPACITY * 2U + 3U];
        char firmware[REVLINK_AP_FIRMWARE_CAPACITY * 2U + 3U];
        char vehicle[REVLINK_AP_VEHICLE_CAPACITY * 2U + 3U];
        revlink_accessport_catalog_entry_t catalog = {0};
        const bool catalog_supported =
            revlink_accessport_catalog_lookup(
                device->identity.part_number,
                &catalog
            );
        char family_code[16U];
        char family_name[64U];
        if (
            !json_escape(device->identity.serial, serial, sizeof(serial))
            || !json_escape(
                device->identity.part_number,
                part,
                sizeof(part)
            )
            || !json_escape(
                device->identity.firmware,
                firmware,
                sizeof(firmware)
            )
            || !json_escape(
                device->identity.vehicle,
                vehicle,
                sizeof(vehicle)
            )
            || !json_escape(
                catalog_supported ? catalog.family_code : "",
                family_code,
                sizeof(family_code)
            )
            || !json_escape(
                catalog_supported ? catalog.family_name : "",
                family_name,
                sizeof(family_name)
            )
        ) {
            failed = true;
            break;
        }
        const int count = snprintf(
            &response[used],
            capacity - used,
            "%s{\"key\":\"%s\",\"selected\":%s,\"serial\":%s,"
            "\"partNumber\":%s,\"firmware\":%s,\"vehicle\":%s,"
            "\"catalogSupported\":%s,\"familyCode\":%s,"
            "\"familyName\":%s,\"readOnlyFileSyncSupported\":%s}",
            index == 0U ? "" : ",",
            device->key,
            device->selected ? "true" : "false",
            serial,
            part,
            firmware,
            vehicle,
            catalog_supported ? "true" : "false",
            family_code,
            family_name,
            catalog_supported && catalog.read_only_file_sync_supported
                ? "true" : "false"
        );
        if (count <= 0 || (size_t)count >= capacity - used) {
            failed = true;
            break;
        }
        used += (size_t)count;
    }
    if (!failed) {
        const int count = snprintf(
            &response[used],
            capacity - used,
            "]}"
        );
        failed = count <= 0 || (size_t)count >= capacity - used;
    }
    free(snapshot);
    if (failed) {
        free(response);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Device catalog exceeded its response bound\"}"
        );
    }
    const esp_err_t status = send_json(request, HTTPD_200, response);
    free(response);
    return status;
}

static esp_err_t portal_device_select_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (
        request->content_len <= 4
        || request->content_len >= (int)PORTAL_ACTION_BODY_LIMIT
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid cached device selection\"}"
        );
    }
    char body[PORTAL_ACTION_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            &body[received],
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete cached device selection\"}"
            );
        }
        received += (size_t)count;
    }
    static const char prefix[] = "key=";
    if (
        received != (size_t)request->content_len
        || memcmp(body, prefix, sizeof(prefix) - 1U) != 0
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Cached device key is missing\"}"
        );
    }

    const revlink_sync_snapshot_t sync =
        revlink_runtime_sync_snapshot();
    const revlink_control_request_t control_request = {
        .command = REVLINK_CONTROL_GET_STATUS,
    };
    revlink_control_response_t control = {0};
    if (
        sync.state == REVLINK_SYNC_QUEUED
        || sync.state == REVLINK_SYNC_RUNNING
        || sync.state == REVLINK_SYNC_CANCELLING
        || revlink_runtime_control_execute(
               &control_request,
               &control
           ) != REVLINK_CONTROL_OK
        || control.snapshot.device.state == REVLINK_DEVICE_SESSION_ACTIVE
        || control.snapshot.device.state == REVLINK_DEVICE_INSPECTING
    ) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Wait for the current device operation to finish\"}"
        );
    }

    const esp_err_t status = revlink_sd_select_cached_device(
        body + sizeof(prefix) - 1U
    );
    if (status != ESP_OK) {
        return send_json(
            request,
            status == ESP_ERR_NOT_FOUND
                ? "404 Not Found"
                : (status == ESP_ERR_INVALID_ARG
                    ? HTTPD_400
                    : "409 Conflict"),
            "{\"error\":\"Unable to select the cached device\"}"
        );
    }
    return send_json(request, HTTPD_200, "{\"ok\":true}");
}

static esp_err_t portal_files_handler(httpd_req_t *request)
{
    revlink_sd_portal_snapshot_t *storage = calloc(1U, sizeof(*storage));
    if (storage == NULL) {
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Inventory allocation failed\"}"
        );
    }
    if (revlink_sd_portal_snapshot(storage) != ESP_OK) {
        free(storage);
        return send_json(
            request,
            "503 Service Unavailable",
            "{\"error\":\"Inventory is unavailable\"}"
        );
    }

    revlink_sync_annotations_t *annotations =
        calloc(1U, sizeof(*annotations));
    if (annotations == NULL) {
        free(storage);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Notes allocation failed\"}"
        );
    }
    if (revlink_sd_annotations_snapshot(annotations) != ESP_OK) {
        revlink_sync_annotations_init(annotations);
    }

    typedef struct {
        char escaped_path[REVLINK_SD_PORTAL_PATH_CAPACITY * 2U + 3U];
        char escaped_note[REVLINK_SYNC_NOTE_CAPACITY * 2U + 3U];
        char digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U + 1U];
        char map_digest[
            REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U + 3U
        ];
        char entry[
            REVLINK_SD_PORTAL_PATH_CAPACITY * 2U + 3U
            + REVLINK_SYNC_NOTE_CAPACITY * 2U + 3U
            + REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 4U + 4U
            + 192U
        ];
    } portal_file_json_scratch_t;
    portal_file_json_scratch_t *scratch =
        calloc(1U, sizeof(*scratch));
    if (scratch == NULL) {
        free(annotations);
        free(storage);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Inventory response allocation failed\"}"
        );
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Connection", "close");
    char prefix[96];
    const int prefix_length = snprintf(
        prefix,
        sizeof(prefix),
        "{\"total\":%u,\"shown\":%u,\"files\":[",
        (unsigned int)storage->total_files,
        (unsigned int)storage->listed_files
    );
    esp_err_t status =
        prefix_length > 0 && (size_t)prefix_length < sizeof(prefix)
        ? httpd_resp_send_chunk(request, prefix, (ssize_t)prefix_length)
        : ESP_FAIL;
    for (
        size_t index = 0U;
        status == ESP_OK && index < storage->listed_files;
        ++index
    ) {
        const revlink_sync_annotation_t *annotation =
            revlink_sync_annotations_find(
                annotations,
                storage->files[index].sha256
            );
        if (!json_escape(
                storage->files[index].path,
                scratch->escaped_path,
                sizeof(scratch->escaped_path)
            )
            || !json_escape(
                annotation != NULL ? annotation->note : "",
                scratch->escaped_note,
                sizeof(scratch->escaped_note)
            )) {
            status = ESP_FAIL;
            break;
        }
        static const char hex[] = "0123456789abcdef";
        for (
            size_t byte = 0U;
            byte < REVLINK_SYNC_ANNOTATION_SHA256_BYTES;
            ++byte
        ) {
            scratch->digest[byte * 2U] =
                hex[storage->files[index].sha256[byte] >> 4U];
            scratch->digest[byte * 2U + 1U] =
                hex[storage->files[index].sha256[byte] & 0x0fU];
        }
        scratch->digest[sizeof(scratch->digest) - 1U] = '\0';
        memcpy(scratch->map_digest, "null", 5U);
        if (annotation != NULL && annotation->has_map_sha256) {
            scratch->map_digest[0] = '"';
            for (
                size_t byte = 0U;
                byte < REVLINK_SYNC_ANNOTATION_SHA256_BYTES;
                ++byte
            ) {
                scratch->map_digest[byte * 2U + 1U] =
                    hex[annotation->map_sha256[byte] >> 4U];
                scratch->map_digest[byte * 2U + 2U] =
                    hex[annotation->map_sha256[byte] & 0x0fU];
            }
            scratch->map_digest[
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U + 1U
            ] = '"';
            scratch->map_digest[
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U + 2U
            ] = '\0';
        }
        const int entry_length = snprintf(
            scratch->entry,
            sizeof(scratch->entry),
            "%s{\"path\":%s,\"kind\":\"%s\",\"size\":%" PRIu32
            ",\"deviceTimeRaw\":%" PRIu32
            ",\"initialSyncUtc\":%" PRIu64
            ",\"sha256\":\"%s\",\"note\":%s,\"noteUpdatedAt\":%" PRIu64
            ",\"mapDigest\":%s}",
            index == 0U ? "" : ",",
            scratch->escaped_path,
            storage->files[index].kind == REVLINK_SD_FILE_MAP
                ? "map"
                : storage->files[index].kind
                    == REVLINK_SD_FILE_STARTUP_SCREEN
                    ? "startup"
                    : storage->files[index].kind
                        == REVLINK_SD_FILE_SCREENSHOT
                        ? "screenshot" : "datalog",
            storage->files[index].size,
            storage->files[index].device_time_raw,
            storage->files[index].initial_sync_utc,
            scratch->digest,
            scratch->escaped_note,
            annotation != NULL ? annotation->updated_at_utc : 0U,
            scratch->map_digest
        );
        if (
            entry_length <= 0
            || (size_t)entry_length >= sizeof(scratch->entry)
        ) {
            status = ESP_FAIL;
            break;
        }
        status = httpd_resp_send_chunk(
            request,
            scratch->entry,
            (ssize_t)entry_length
        );
    }
    free(scratch);
    free(annotations);
    free(storage);
    if (status != ESP_OK) {
        httpd_resp_send_chunk(request, NULL, 0);
        return status;
    }
    status = httpd_resp_send_chunk(request, "]}", 2);
    if (status == ESP_OK) {
        status = httpd_resp_send_chunk(request, NULL, 0);
    }
    return status;
}

static void safe_download_name(
    const char *path,
    bool strip_gzip,
    char *output,
    size_t capacity
)
{
    const char *name = path != NULL ? strrchr(path, '/') : NULL;
    name = name != NULL ? name + 1U : path;
    if (name == NULL || name[0] == '\0') {
        name = "revlink-file";
    }
    size_t used = 0U;
    while (name[used] != '\0' && used + 1U < capacity) {
        const unsigned char value = (unsigned char)name[used];
        output[used] =
            (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9')
                || value == '.' || value == '_' || value == '-'
            ? (char)value : '_';
        ++used;
    }
    output[used] = '\0';
    if (strip_gzip && used > 3U
        && strcmp(output + used - 3U, ".gz") == 0) {
        output[used - 3U] = '\0';
    }
}

static esp_err_t portal_file_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (
        httpd_req_get_url_query_len(request) == 0U
        || httpd_req_get_url_query_len(request) >= PORTAL_FILE_QUERY_LIMIT
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"File digest is missing\"}"
        );
    }
    char query[PORTAL_FILE_QUERY_LIMIT] = {0};
    char digest_text[
        REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U + 1U
    ] = {0};
    if (
        httpd_req_get_url_query_str(
            request,
            query,
            sizeof(query)
        ) != ESP_OK
        || httpd_query_key_value(
               query,
               "digest",
               digest_text,
               sizeof(digest_text)
           ) != ESP_OK
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"File digest is invalid\"}"
        );
    }
    uint8_t digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    if (!decode_digest(digest_text, strlen(digest_text), digest)) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"File digest is invalid\"}"
        );
    }

    revlink_sd_cached_reader_t *reader = NULL;
    revlink_sd_cached_file_info_t info = {0};
    const esp_err_t open_status =
        revlink_sd_cached_reader_open(digest, &reader, &info);
    if (open_status != ESP_OK) {
        return send_json(
            request,
            open_status == ESP_ERR_NOT_FOUND
                ? "404 Not Found" : "409 Conflict",
            "{\"error\":\"Cached file is unavailable\"}"
        );
    }

    char filename[REVLINK_SD_PORTAL_PATH_CAPACITY];
    safe_download_name(
        info.path,
        info.gzip_encoded,
        filename,
        sizeof(filename)
    );
    char disposition[REVLINK_SD_PORTAL_PATH_CAPACITY + 32U];
    const int disposition_length = snprintf(
        disposition,
        sizeof(disposition),
        "inline; filename=\"%s\"",
        filename
    );
    if (
        disposition_length <= 0
        || (size_t)disposition_length >= sizeof(disposition)
    ) {
        revlink_sd_cached_reader_close(reader);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"File response name is invalid\"}"
        );
    }

    httpd_resp_set_type(
        request,
        info.kind == REVLINK_SD_FILE_SCREENSHOT
            ? (strstr(info.path, ".bmp") != NULL
                ? "image/bmp" : "image/png")
            : (info.kind == REVLINK_SD_FILE_MAP
                || info.kind == REVLINK_SD_FILE_STARTUP_SCREEN)
                ? "application/octet-stream"
                : "text/csv; charset=utf-8"
    );
    httpd_resp_set_hdr(request, "Cache-Control", "private, no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Disposition", disposition);
    httpd_resp_set_hdr(request, "Connection", "close");
    if (info.gzip_encoded) {
        httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    }

    uint8_t *buffer = malloc(PORTAL_FILE_STREAM_BYTES);
    if (buffer == NULL) {
        revlink_sd_cached_reader_close(reader);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"File stream allocation failed\"}"
        );
    }
    esp_err_t status = ESP_OK;
    while (status == ESP_OK) {
        size_t count = 0U;
        status = revlink_sd_cached_reader_read(
            reader,
            buffer,
            PORTAL_FILE_STREAM_BYTES,
            &count
        );
        if (status != ESP_OK || count == 0U) {
            break;
        }
        status = httpd_resp_send_chunk(
            request,
            (const char *)buffer,
            (ssize_t)count
        );
    }
    free(buffer);
    revlink_sd_cached_reader_close(reader);
    if (status == ESP_OK) {
        status = httpd_resp_send_chunk(request, NULL, 0);
    }
    return status;
}

static esp_err_t portal_note_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (request->content_len <= 0
        || request->content_len >= (int)PORTAL_NOTE_BODY_LIMIT) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid note request\"}"
        );
    }
    const revlink_control_request_t control_request = {
        .command = REVLINK_CONTROL_GET_STATUS,
    };
    revlink_control_response_t control = {0};
    if (revlink_runtime_control_execute(&control_request, &control)
            != REVLINK_CONTROL_OK
        || control.snapshot.sync.state == REVLINK_SYNC_QUEUED
        || control.snapshot.sync.state == REVLINK_SYNC_RUNNING
        || control.snapshot.sync.state == REVLINK_SYNC_CANCELLING) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Wait for synchronization to finish before saving notes\"}"
        );
    }

    char *body = calloc(1U, PORTAL_NOTE_BODY_LIMIT);
    char *note = calloc(1U, REVLINK_SYNC_NOTE_CAPACITY);
    if (body == NULL || note == NULL) {
        free(body);
        free(note);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Note request allocation failed\"}"
        );
    }
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            &body[received],
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            free(body);
            free(note);
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete note request\"}"
            );
        }
        received += (size_t)count;
    }
    static const char digest_prefix[] = "digest=";
    static const char note_marker[] = "&note=";
    if (received <= sizeof(digest_prefix) - 1U
        || memcmp(body, digest_prefix, sizeof(digest_prefix) - 1U) != 0) {
        free(body);
        free(note);
        return send_json(request, HTTPD_400, "{\"error\":\"Digest is missing\"}");
    }
    char *marker = strstr(body, note_marker);
    if (marker == NULL) {
        free(body);
        free(note);
        return send_json(request, HTTPD_400, "{\"error\":\"Note is missing\"}");
    }
    const char *digest_text = body + sizeof(digest_prefix) - 1U;
    const size_t digest_length = (size_t)(marker - digest_text);
    uint8_t digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    size_t note_length = 0U;
    if (!decode_digest(digest_text, digest_length, digest)
        || !form_decode(
               marker + sizeof(note_marker) - 1U,
               received
                   - (size_t)(marker - body)
                   - (sizeof(note_marker) - 1U),
               note,
               REVLINK_SYNC_NOTE_CAPACITY,
               &note_length
           )) {
        free(body);
        free(note);
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Note or file version is invalid\"}"
        );
    }
    const esp_err_t status =
        revlink_sd_annotation_set(digest, note, note_length);
    free(body);
    free(note);
    if (status != ESP_OK) {
        return send_json(
            request,
            status == ESP_ERR_NOT_FOUND ? "404 Not Found" : HTTPD_500,
            "{\"error\":\"Unable to save the note\"}"
        );
    }
    return send_json(request, HTTPD_200, "{\"ok\":true}");
}

static esp_err_t portal_log_map_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (
        request->content_len <= 0
        || request->content_len >= (int)PORTAL_LOG_MAP_BODY_LIMIT
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid map tag request\"}"
        );
    }
    const revlink_control_request_t control_request = {
        .command = REVLINK_CONTROL_GET_STATUS,
    };
    revlink_control_response_t control = {0};
    if (
        revlink_runtime_control_execute(&control_request, &control)
            != REVLINK_CONTROL_OK
        || control.snapshot.sync.state == REVLINK_SYNC_QUEUED
        || control.snapshot.sync.state == REVLINK_SYNC_RUNNING
        || control.snapshot.sync.state == REVLINK_SYNC_CANCELLING
    ) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Wait for synchronization to finish before tagging logs\"}"
        );
    }

    char body[PORTAL_LOG_MAP_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            &body[received],
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete map tag request\"}"
            );
        }
        received += (size_t)count;
    }
    static const char digest_prefix[] = "digest=";
    static const char map_marker[] = "&mapDigest=";
    if (
        received <= sizeof(digest_prefix) - 1U
        || memcmp(body, digest_prefix, sizeof(digest_prefix) - 1U) != 0
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Datalog version is missing\"}"
        );
    }
    char *marker = strstr(body, map_marker);
    if (marker == NULL) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Map version is missing\"}"
        );
    }
    const char *log_text = body + sizeof(digest_prefix) - 1U;
    const size_t log_length = (size_t)(marker - log_text);
    const char *map_text = marker + sizeof(map_marker) - 1U;
    const size_t map_length =
        received - (size_t)(map_text - body);
    uint8_t log_digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    uint8_t map_digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    if (
        !decode_digest(log_text, log_length, log_digest)
        || (map_length > 0U
            && !decode_digest(map_text, map_length, map_digest))
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Datalog or map version is invalid\"}"
        );
    }
    const esp_err_t status = revlink_sd_annotation_set_map(
        log_digest,
        map_length > 0U ? map_digest : NULL
    );
    if (status != ESP_OK) {
        return send_json(
            request,
            status == ESP_ERR_NOT_FOUND ? "404 Not Found" : HTTPD_500,
            "{\"error\":\"Unable to save the map tag\"}"
        );
    }
    return send_json(request, HTTPD_200, "{\"ok\":true}");
}

static bool read_required_header(
    httpd_req_t *request,
    const char *header,
    char *value,
    size_t capacity
)
{
    const size_t length = httpd_req_get_hdr_value_len(request, header);
    return length > 0U && length < capacity
        && httpd_req_get_hdr_value_str(
               request,
               header,
               value,
               capacity
           ) == ESP_OK;
}

static esp_err_t portal_startup_profiles_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    revlink_sd_startup_profiles_snapshot_t snapshot;
    const esp_err_t snapshot_status =
        revlink_sd_startup_profiles_snapshot(&snapshot);
    if (snapshot_status != ESP_OK) {
        return send_json(
            request,
            snapshot_status == ESP_ERR_INVALID_ARG
                ? "409 Conflict" : HTTPD_500,
            "{\"error\":\"Startup image library is unavailable\"}"
        );
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t status = httpd_resp_send_chunk(request, "{\"profiles\":[", 13);
    for (size_t index = 0U;
         status == ESP_OK && index < snapshot.count;
         ++index) {
        char escaped_name[
            REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY * 2U + 3U
        ];
        if (!json_escape(
                snapshot.profiles[index].name,
                escaped_name,
                sizeof(escaped_name)
            )) {
            status = ESP_FAIL;
            break;
        }
        char entry[
            REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY * 2U + 128U
        ];
        const int length = snprintf(
            entry,
            sizeof(entry),
            "%s{\"id\":\"%s\",\"name\":%s,\"size\":%" PRIu32 "}",
            index == 0U ? "" : ",",
            snapshot.profiles[index].id,
            escaped_name,
            snapshot.profiles[index].size
        );
        if (length <= 0 || (size_t)length >= sizeof(entry)) {
            status = ESP_FAIL;
            break;
        }
        status = httpd_resp_send_chunk(request, entry, length);
    }
    if (status == ESP_OK) {
        status = httpd_resp_send_chunk(request, "]}", 2);
    }
    if (status == ESP_OK) {
        status = httpd_resp_send_chunk(request, NULL, 0);
    }
    return status;
}

static esp_err_t portal_startup_profile_save_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (request->content_len != (int)REVLINK_AP_STARTUP_SCREEN_BYTES) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Startup image must be exactly 240 by 320 pixels\"}"
        );
    }
    char name[REVLINK_SD_STARTUP_PROFILE_NAME_CAPACITY] = {0};
    if (!read_required_header(
            request,
            "X-RevLink-Profile-Name",
            name,
            sizeof(name)
        )) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Startup image profile name is required\"}"
        );
    }
    esp_err_t status = revlink_sd_startup_profile_begin(
        name,
        REVLINK_AP_STARTUP_SCREEN_BYTES
    );
    if (status != ESP_OK) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Unable to begin startup image profile\"}"
        );
    }
    uint8_t *buffer = malloc(PORTAL_FILE_STREAM_BYTES);
    if (buffer == NULL) {
        revlink_sd_startup_profile_abort();
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Startup image allocation failed\"}"
        );
    }
    size_t received = 0U;
    while (received < REVLINK_AP_STARTUP_SCREEN_BYTES) {
        const size_t wanted =
            REVLINK_AP_STARTUP_SCREEN_BYTES - received
                < PORTAL_FILE_STREAM_BYTES
            ? REVLINK_AP_STARTUP_SCREEN_BYTES - received
            : PORTAL_FILE_STREAM_BYTES;
        const int count = httpd_req_recv(
            request,
            (char *)buffer,
            wanted
        );
        if (count <= 0
            || revlink_sd_startup_profile_write(
                   buffer,
                   (size_t)count
               ) != ESP_OK) {
            free(buffer);
            revlink_sd_startup_profile_abort();
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Startup image upload was incomplete\"}"
            );
        }
        received += (size_t)count;
    }
    free(buffer);
    char id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY];
    status = revlink_sd_startup_profile_commit(id, sizeof(id));
    if (status != ESP_OK) {
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Unable to save startup image profile\"}"
        );
    }
    char response[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY + 32U];
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"id\":\"%s\"}",
        id
    );
    return length > 0 && (size_t)length < sizeof(response)
        ? send_json(request, HTTPD_200, response)
        : send_json(
              request,
              HTTPD_500,
              "{\"error\":\"Startup profile response failed\"}"
          );
}

static esp_err_t portal_startup_profile_file_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (httpd_req_get_url_query_len(request) == 0U
        || httpd_req_get_url_query_len(request) >= PORTAL_FILE_QUERY_LIMIT) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Startup image profile is missing\"}"
        );
    }
    char query[PORTAL_FILE_QUERY_LIMIT] = {0};
    char id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Startup image profile is invalid\"}"
        );
    }
    revlink_sd_cached_reader_t *reader = NULL;
    revlink_sd_startup_profile_t profile = {0};
    const esp_err_t open_status =
        revlink_sd_startup_profile_reader_open(id, &reader, &profile);
    if (open_status != ESP_OK) {
        return send_json(
            request,
            open_status == ESP_ERR_NOT_FOUND
                ? "404 Not Found" : "409 Conflict",
            "{\"error\":\"Startup image profile is unavailable\"}"
        );
    }
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "private, no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Connection", "close");
    uint8_t *buffer = malloc(PORTAL_FILE_STREAM_BYTES);
    if (buffer == NULL) {
        revlink_sd_cached_reader_close(reader);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Startup image stream allocation failed\"}"
        );
    }
    esp_err_t status = ESP_OK;
    while (status == ESP_OK) {
        size_t count = 0U;
        status = revlink_sd_cached_reader_read(
            reader,
            buffer,
            PORTAL_FILE_STREAM_BYTES,
            &count
        );
        if (status != ESP_OK || count == 0U) break;
        status = httpd_resp_send_chunk(
            request,
            (const char *)buffer,
            (ssize_t)count
        );
    }
    free(buffer);
    revlink_sd_cached_reader_close(reader);
    if (status == ESP_OK) {
        status = httpd_resp_send_chunk(request, NULL, 0);
    }
    return status;
}

static esp_err_t portal_startup_apply_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (request->content_len <= 0
        || request->content_len >= (int)PORTAL_STARTUP_APPLY_BODY_LIMIT) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Startup image selection is invalid\"}"
        );
    }
    char body[PORTAL_STARTUP_APPLY_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            body + received,
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Startup image selection was incomplete\"}"
            );
        }
        received += (size_t)count;
    }
    char id[REVLINK_SD_STARTUP_PROFILE_ID_CAPACITY] = {0};
    char confirmed[8] = {0};
    if (httpd_query_key_value(body, "id", id, sizeof(id)) != ESP_OK
        || httpd_query_key_value(
               body,
               "confirmed",
               confirmed,
               sizeof(confirmed)
           ) != ESP_OK
        || strcmp(confirmed, "true") != 0) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Startup image replacement must be confirmed\"}"
        );
    }
    bool known = false;
    revlink_ap_device_info_t identity = {0};
    if (revlink_runtime_connected_accessport_snapshot(
            &known,
            &identity
        ) != ESP_OK || !known
        || !revlink_sd_selected_device_matches(&identity)) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Attach and select this AccessPort first\"}"
        );
    }
    const revlink_sync_snapshot_t sync =
        revlink_runtime_sync_snapshot();
    if (sync.state == REVLINK_SYNC_QUEUED
        || sync.state == REVLINK_SYNC_RUNNING
        || sync.state == REVLINK_SYNC_CANCELLING) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Wait for synchronization to finish\"}"
        );
    }
    revlink_sd_cached_reader_t *reader = NULL;
    revlink_sd_startup_profile_t profile = {0};
    esp_err_t status =
        revlink_sd_startup_profile_reader_open(id, &reader, &profile);
    if (status != ESP_OK) {
        return send_json(
            request,
            status == ESP_ERR_NOT_FOUND ? "404 Not Found" : "409 Conflict",
            "{\"error\":\"Startup image profile is unavailable\"}"
        );
    }
    status = revlink_map_upload_stage_begin(
        "startup_screen.fb",
        "images/startup_screen.fb",
        REVLINK_AP_STARTUP_SCREEN_BYTES
    );
    /* This flow requires the device already attached, so pin to it. */
    if (status == ESP_OK) {
        status = revlink_map_upload_stage_set_target(
            identity.part_number,
            identity.serial
        );
    }
    uint8_t *buffer =
        status == ESP_OK ? malloc(PORTAL_FILE_STREAM_BYTES) : NULL;
    if (status == ESP_OK && buffer == NULL) status = ESP_ERR_NO_MEM;
    while (status == ESP_OK) {
        size_t count = 0U;
        status = revlink_sd_cached_reader_read(
            reader,
            buffer,
            PORTAL_FILE_STREAM_BYTES,
            &count
        );
        if (status != ESP_OK || count == 0U) break;
        status = revlink_map_upload_stage_write(buffer, count);
    }
    free(buffer);
    revlink_sd_cached_reader_close(reader);
    if (status == ESP_OK) {
        status = revlink_map_upload_stage_commit();
    }
    if (status == ESP_OK) {
        status = revlink_map_upload_request(&identity);
    }
    if (status != ESP_OK) {
        revlink_map_upload_stage_abort();
        return send_json(
            request,
            status == ESP_ERR_NOT_SUPPORTED
                ? "403 Forbidden" : "409 Conflict",
            "{\"error\":\"Startup image transfer was refused by a safety gate\"}"
        );
    }
    return send_json(
        request,
        "202 Accepted",
        "{\"ok\":true,\"queued\":true}"
    );
}

static esp_err_t portal_write_consent_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (request->content_len <= 0
        || request->content_len >= (int)PORTAL_ACTION_BODY_LIMIT) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid write-consent request\"}"
        );
    }
    char body[PORTAL_ACTION_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            body + received,
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete write-consent request\"}"
            );
        }
        received += (size_t)count;
    }
    const bool enabled =
        received == sizeof("enabled=true") - 1U
        && memcmp(body, "enabled=true", received) == 0;
    const bool disabled =
        received == sizeof("enabled=false") - 1U
        && memcmp(body, "enabled=false", received) == 0;
    if (!enabled && !disabled) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Write consent must be true or false\"}"
        );
    }
    const esp_err_t status =
        revlink_runtime_set_write_consent(enabled);
    return status == ESP_OK
        ? send_json(request, HTTPD_200, "{\"ok\":true}")
        : send_json(
              request,
              status == ESP_ERR_NOT_SUPPORTED
                  ? "501 Not Implemented" : "409 Conflict",
              "{\"error\":\"Unable to change write consent\"}"
          );
}

/*
 * The AccessPort a staged map belongs to is the one whose dataset is
 * currently selected in the portal. Staging happens with no device attached,
 * so the selected cached dataset is the only expression of intent available.
 */
static bool portal_selected_device_identity(
    revlink_ap_device_info_t *identity
)
{
    revlink_sd_cached_devices_snapshot_t *snapshot =
        calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) return false;

    bool found = false;
    if (revlink_sd_cached_devices_snapshot(snapshot) == ESP_OK) {
        for (size_t index = 0U; index < snapshot->count; ++index) {
            if (!snapshot->devices[index].selected) continue;
            *identity = snapshot->devices[index].identity;
            found = identity->serial[0] != '\0'
                && identity->part_number[0] != '\0';
            break;
        }
    }
    free(snapshot);
    return found;
}

/*
 * The AccessPort refuses a destination it already holds, and it refuses it at
 * the readiness step — after a transfer has been started and the write service
 * has entered its running state. The owner then sees a generic failure for
 * something that was never going to work.
 *
 * The cached inventory mirrors the device after a sync, so it is a good
 * pre-flight. It is not the authority: if it cannot be read, say nothing and
 * let the device decide, exactly as before.
 */
static bool portal_cached_destination_digest(
    const char *destination,
    uint8_t digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
)
{
    revlink_sd_portal_snapshot_t *snapshot = calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) return false;
    bool present = false;
    if (revlink_sd_portal_snapshot(snapshot) == ESP_OK) {
        for (size_t index = 0U; index < snapshot->listed_files; ++index) {
            if (strcmp(snapshot->files[index].path, destination) == 0) {
                memcpy(
                    digest,
                    snapshot->files[index].sha256,
                    REVLINK_SYNC_ANNOTATION_SHA256_BYTES
                );
                present = true;
                break;
            }
        }
    }
    free(snapshot);
    return present;
}

static esp_err_t portal_map_stage_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (request->content_len <= 0
        || (uint32_t)request->content_len > REVLINK_AP_MAX_CHUNK_PAYLOAD) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Map file size is invalid\"}"
        );
    }
    char name[REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY] = {0};
    char destination[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY] = {0};
    if (!read_required_header(
            request,
            "X-RevLink-Map-Name",
            name,
            sizeof(name)
        )) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"A map file name is required\"}"
        );
    }
    const int destination_length = snprintf(
        destination,
        sizeof(destination),
        "maps/%s",
        name
    );
    if (destination_length <= 5
        || (size_t)destination_length >= sizeof(destination)) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Use a .ptm file name without a folder\"}"
        );
    }
    esp_err_t status = revlink_map_upload_stage_begin(
        name,
        destination,
        (uint32_t)request->content_len
    );
    if (status != ESP_OK) {
        return send_json(
            request,
            status == ESP_ERR_NOT_SUPPORTED
                ? "501 Not Implemented" : "409 Conflict",
            status == ESP_ERR_INVALID_ARG
                ? "{\"error\":\"Use a .ptm file name without a folder or path traversal\"}"
                : "{\"error\":\"Another map operation is already in progress\"}"
        );
    }
    /*
     * Pin the payload to the selected dataset's AccessPort before any bytes
     * are accepted. An unpinned staged map can still be applied by hand with
     * the device in front of you, but it will never be written automatically.
     */
    revlink_ap_device_info_t target = {0};
    const bool pinned = portal_selected_device_identity(&target)
        && revlink_map_upload_stage_set_target(
               target.part_number,
               target.serial
           ) == ESP_OK;

    uint8_t *buffer = malloc(PORTAL_MAP_STREAM_BYTES);
    if (buffer == NULL) {
        revlink_map_upload_stage_abort();
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Map staging allocation failed\"}"
        );
    }
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const size_t wanted =
            (size_t)request->content_len - received < PORTAL_MAP_STREAM_BYTES
            ? (size_t)request->content_len - received
            : PORTAL_MAP_STREAM_BYTES;
        const int count = httpd_req_recv(
            request,
            (char *)buffer,
            wanted
        );
        if (count <= 0
            || revlink_map_upload_stage_write(
                   buffer,
                   (size_t)count
               ) != ESP_OK) {
            free(buffer);
            revlink_map_upload_stage_abort();
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Map staging was incomplete\"}"
            );
        }
        received += (size_t)count;
    }
    free(buffer);
    status = revlink_map_upload_stage_commit();
    if (status != ESP_OK) {
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Unable to commit staged map\"}"
        );
    }
    /*
     * The AccessPort never overwrites a map, and refuses a name it already
     * holds at the readiness step — after a transfer has started. Decide here
     * instead, now that the payload is committed and its digest is known.
     *
     * Same name and same bytes is not a conflict, it is a no-op: the device
     * already has exactly this file. Say so and drop the payload rather than
     * leaving something staged that would fail. Same name with different bytes
     * is a real conflict and is refused.
     *
     * The cached inventory mirrors the device after a sync, so this is a
     * pre-flight rather than the authority. If it cannot be read the payload
     * stays staged and the device decides, as it did before.
     */
    revlink_map_upload_snapshot_t staged = {0};
    uint8_t cached_digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES] = {0};
    if (revlink_map_upload_snapshot(&staged) == ESP_OK
        && portal_cached_destination_digest(destination, cached_digest)) {
        const bool identical = memcmp(
            staged.sha256,
            cached_digest,
            sizeof(cached_digest)
        ) == 0;
        (void)revlink_map_upload_discard();
        return identical
            ? send_json(
                  request,
                  HTTPD_200,
                  "{\"ok\":true,\"staged\":false,\"alreadyPresent\":true}"
              )
            : send_json(
                  request,
                  "409 Conflict",
                  "{\"error\":\"This AccessPort already has a different map "
                  "with that name. Existing maps are never overwritten "
                  "\u2014 rename the file and save it again.\"}"
              );
    }

    char body[192];
    (void)snprintf(
        body,
        sizeof(body),
        "{\"ok\":true,\"staged\":true,\"pinned\":%s,\"target\":\"%s\"}",
        pinned ? "true" : "false",
        pinned ? target.part_number : ""
    );
    return send_json(request, HTTPD_200, body);
}

static esp_err_t portal_map_auto_apply_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    char body[32] = {0};
    if (request->content_len <= 0
        || (size_t)request->content_len >= sizeof(body)) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Auto-apply request is invalid\"}"
        );
    }
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            body + received,
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete auto-apply request\"}"
            );
        }
        received += (size_t)count;
    }
    const bool enabled =
        received == sizeof("enabled=true") - 1U
        && memcmp(body, "enabled=true", received) == 0;
    const bool disabled =
        received == sizeof("enabled=false") - 1U
        && memcmp(body, "enabled=false", received) == 0;
    if (!enabled && !disabled) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Auto-apply must be true or false\"}"
        );
    }
    const esp_err_t status = revlink_runtime_set_map_auto_apply(enabled);
    return status == ESP_OK
        ? send_json(request, HTTPD_200, "{\"ok\":true}")
        : send_json(
              request,
              status == ESP_ERR_NOT_SUPPORTED
                  ? "501 Not Implemented" : "409 Conflict",
              "{\"error\":\"Unable to change staged-map auto-apply\"}"
          );
}

static esp_err_t portal_map_discard_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    const esp_err_t status = revlink_map_upload_discard();
    return status == ESP_OK
        ? send_json(request, HTTPD_200, "{\"ok\":true}")
        : send_json(
              request,
              status == ESP_ERR_NOT_SUPPORTED
                  ? "501 Not Implemented" : "409 Conflict",
              "{\"error\":\"A transfer is in progress\"}"
          );
}

static esp_err_t portal_map_apply_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    bool known = false;
    revlink_ap_device_info_t identity = {0};
    if (revlink_runtime_connected_accessport_snapshot(
            &known,
            &identity
        ) != ESP_OK || !known) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Connect and identify one AccessPort first\"}"
        );
    }
    if (!revlink_sd_selected_device_matches(&identity)) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Select the attached AccessPort dataset first\"}"
        );
    }
    const revlink_sync_snapshot_t sync =
        revlink_runtime_sync_snapshot();
    if (sync.state == REVLINK_SYNC_QUEUED
        || sync.state == REVLINK_SYNC_RUNNING
        || sync.state == REVLINK_SYNC_CANCELLING) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Wait for synchronization to finish\"}"
        );
    }
    const esp_err_t status = revlink_map_upload_request(&identity);
    return status == ESP_OK
        ? send_json(request, "202 Accepted", "{\"ok\":true,\"queued\":true}")
        : send_json(
              request,
              status == ESP_ERR_NOT_SUPPORTED
                  ? "403 Forbidden" : "409 Conflict",
              "{\"error\":\"Map upload was refused by a safety gate\"}"
          );
}

static esp_err_t execute_control(
    httpd_req_t *request,
    revlink_control_command_t command,
    bool enabled
)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    const revlink_control_request_t control_request = {
        .command = command,
        .enabled = enabled,
    };
    revlink_control_response_t response = {0};
    const revlink_control_status_t status =
        revlink_runtime_control_execute(&control_request, &response);
    char json[128];
    const int length = snprintf(
        json,
        sizeof(json),
        "{\"ok\":%s,\"status\":\"%s\"}",
        status == REVLINK_CONTROL_OK ? "true" : "false",
        revlink_control_status_name(status)
    );
    if (length <= 0 || (size_t)length >= sizeof(json)) {
        return ESP_FAIL;
    }
    return send_json(request, control_http_status(status), json);
}

static esp_err_t portal_sync_handler(httpd_req_t *request)
{
    return execute_control(
        request,
        REVLINK_CONTROL_REQUEST_SYNC,
        false
    );
}

static esp_err_t portal_cancel_handler(httpd_req_t *request)
{
    return execute_control(
        request,
        REVLINK_CONTROL_CANCEL_SYNC,
        false
    );
}

static esp_err_t portal_time_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (
        request->content_len <= 0
        || request->content_len >= (int)PORTAL_TIME_BODY_LIMIT
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid time observation\"}"
        );
    }
    char body[PORTAL_TIME_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            &body[received],
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete time observation\"}"
            );
        }
        received += (size_t)count;
    }
    static const char prefix[] = "utc=";
    if (received <= sizeof(prefix) - 1U
        || memcmp(body, prefix, sizeof(prefix) - 1U) != 0) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Time observation is malformed\"}"
        );
    }
    char *end = NULL;
    const char *value = body + sizeof(prefix) - 1U;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || end != body + received
        || parsed < PORTAL_TIME_MINIMUM_UTC
        || parsed > PORTAL_TIME_MAXIMUM_UTC) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Time observation is outside the accepted range\"}"
        );
    }
    if (portal_time_observer == NULL
        || !portal_time_observer(
               portal_time_context,
               (uint64_t)parsed
           )) {
        return send_json(
            request,
            "409 Conflict",
            "{\"ok\":false,\"status\":\"trusted-time-already-available\"}"
        );
    }
    return send_json(
        request,
        HTTPD_200,
        "{\"ok\":true,\"status\":\"client-time-accepted\"}"
    );
}

static esp_err_t portal_auto_sync_handler(httpd_req_t *request)
{
    if (
        request->content_len <= 0
        || request->content_len >= (int)PORTAL_ACTION_BODY_LIMIT
    ) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid auto-sync request\"}"
        );
    }
    char body[PORTAL_ACTION_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            &body[received],
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete auto-sync request\"}"
            );
        }
        received += (size_t)count;
    }
    const bool enabled =
        received == sizeof("enabled=true") - 1U
        && memcmp(body, "enabled=true", received) == 0;
    const bool disabled =
        received == sizeof("enabled=false") - 1U
        && memcmp(body, "enabled=false", received) == 0;
    if (!enabled && !disabled) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Auto-sync must be true or false\"}"
        );
    }
    return execute_control(
        request,
        REVLINK_CONTROL_SET_AUTO_SYNC,
        enabled
    );
}

static esp_err_t portal_backup_chunk(
    void *context,
    const uint8_t *data,
    size_t length
)
{
    httpd_req_t *request = context;
    return httpd_resp_send_chunk(
        request,
        (const char *)data,
        (ssize_t)length
    );
}

static esp_err_t portal_backup_export_handler(httpd_req_t *request)
{
    bool mounted = false;
    bool session_selected = false;
    if (revlink_sd_portal_io_status(&mounted, &session_selected) != ESP_OK
        || !mounted) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"The microSD card is unavailable\"}"
        );
    }
    if (session_selected) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Disconnect the AccessPort before creating a backup\"}"
        );
    }
    httpd_resp_set_type(request, "application/x-revlink-backup");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(
        request,
        "Content-Disposition",
        "attachment; filename=\"revlink-sidecar-backup.revlink-backup\""
    );
    revlink_backup_summary_t summary = {0};
    const esp_err_t status = revlink_backup_export(
        portal_backup_chunk,
        request,
        &summary
    );
    if (status != ESP_OK) {
        httpd_resp_send_chunk(request, NULL, 0);
        return status;
    }
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t portal_backup_preview_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Portal request header is missing\"}"
        );
    }
    if (request->content_len <= 0) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Choose a RevLink backup file\"}"
        );
    }
    esp_err_t status =
        revlink_backup_stage_begin((uint64_t)request->content_len);
    if (status != ESP_OK) {
        return send_json(
            request,
            "409 Conflict",
            status == ESP_ERR_INVALID_STATE
                ? "{\"error\":\"Disconnect the AccessPort before restoring\"}"
                : "{\"error\":\"The backup cannot be staged on this card\"}"
        );
    }
    uint8_t buffer[PORTAL_FILE_STREAM_BYTES];
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const size_t requested =
            (size_t)request->content_len - received < sizeof(buffer)
                ? (size_t)request->content_len - received : sizeof(buffer);
        const int count = httpd_req_recv(request, (char *)buffer, requested);
        if (count <= 0
            || revlink_backup_stage_write(buffer, (size_t)count) != ESP_OK) {
            revlink_backup_stage_abort();
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"The backup upload was interrupted\"}"
            );
        }
        received += (size_t)count;
    }
    revlink_backup_summary_t summary = {0};
    status = revlink_backup_stage_commit(&summary);
    if (status != ESP_OK) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"This is not a valid, intact RevLink backup\"}"
        );
    }
    char response[360U];
    const int response_length = snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"token\":\"%s\",\"files\":%" PRIu32
        ",\"devices\":%" PRIu32 ",\"dataBytes\":%" PRIu64
        ",\"archiveBytes\":%" PRIu64 ",\"createdUtc\":%" PRIu64 "}",
        summary.token,
        summary.file_count,
        summary.device_count,
        summary.data_bytes,
        summary.archive_bytes,
        summary.created_utc
    );
    if (response_length <= 0
        || (size_t)response_length >= sizeof(response)) {
        revlink_backup_stage_abort();
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Backup preview could not be prepared\"}"
        );
    }
    return send_json(request, HTTPD_200, response);
}

static esp_err_t portal_backup_restore_handler(httpd_req_t *request)
{
    if (!portal_header_is_valid(request)
        || request->content_len <= 6
        || request->content_len >= (int)PORTAL_BACKUP_RESTORE_BODY_LIMIT) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid restore confirmation\"}"
        );
    }
    char body[PORTAL_BACKUP_RESTORE_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(
            request,
            body + received,
            (size_t)request->content_len - received
        );
        if (count <= 0) {
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Restore confirmation was interrupted\"}"
            );
        }
        received += (size_t)count;
    }
    static const char prefix[] = "token=";
    if (received != sizeof(prefix) - 1U + 64U
        || memcmp(body, prefix, sizeof(prefix) - 1U) != 0) {
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Restore token is invalid\"}"
        );
    }
    revlink_backup_restore_result_t result = {0};
    const esp_err_t status = revlink_backup_restore_merge(
        body + sizeof(prefix) - 1U,
        &result
    );
    if (status != ESP_OK) {
        return send_json(
            request,
            "409 Conflict",
            status == ESP_ERR_INVALID_STATE
                ? "{\"error\":\"Disconnect the AccessPort before restoring\"}"
                : "{\"error\":\"Restore could not be completed safely\"}"
        );
    }
    char response[240U];
    const int response_length = snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"restored\":%" PRIu32 ",\"identical\":%" PRIu32
        ",\"conflicts\":%" PRIu32 ",\"restoredBytes\":%" PRIu64 "}",
        result.restored_files,
        result.identical_files,
        result.conflicting_files,
        result.restored_bytes
    );
    if (response_length <= 0
        || (size_t)response_length >= sizeof(response)) {
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Restore result could not be reported\"}"
        );
    }
    return send_json(request, HTTPD_200, response);
}

esp_err_t revlink_portal_page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_hdr(
        request,
        "Content-Security-Policy",
        "default-src 'self'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' blob:; object-src 'none'; frame-ancestors 'none'"
    );
    return httpd_resp_send(
        request,
        (const char *)portal_index_html_start,
        (ssize_t)(portal_index_html_end - portal_index_html_start)
    );
}

static esp_err_t send_embedded_asset(
    httpd_req_t *request,
    const char *content_type,
    const uint8_t *start,
    const uint8_t *end
)
{
    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(
        request,
        (const char *)start,
        (ssize_t)(end - start)
    );
}

static esp_err_t portal_accessport_render_handler(httpd_req_t *request)
{
    return send_embedded_asset(
        request,
        "image/webp",
        portal_assets_accessport_render_webp_start,
        portal_assets_accessport_render_webp_end
    );
}

static esp_err_t portal_wordmark_handler(httpd_req_t *request)
{
    return send_embedded_asset(
        request,
        "image/png",
        portal_assets_revlink_wordmark_png_start,
        portal_assets_revlink_wordmark_png_end
    );
}

static esp_err_t register_uri(
    httpd_handle_t server,
    const char *uri,
    httpd_method_t method,
    esp_err_t (*handler)(httpd_req_t *)
)
{
    const httpd_uri_t route = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &route);
}

esp_err_t revlink_portal_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const struct {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {
        {
            "/assets/accessport-render.webp",
            HTTP_GET,
            portal_accessport_render_handler
        },
        {
            "/assets/revlink-wordmark.png",
            HTTP_GET,
            portal_wordmark_handler
        },
        {"/api/portal/status", HTTP_GET, portal_status_handler},
        {"/api/portal/devices", HTTP_GET, portal_devices_handler},
        {
            "/api/portal/device/select",
            HTTP_POST,
            portal_device_select_handler
        },
        {"/api/portal/files", HTTP_GET, portal_files_handler},
        {"/api/portal/file", HTTP_GET, portal_file_handler},
        {"/api/portal/backup", HTTP_GET, portal_backup_export_handler},
        {
            "/api/portal/backup/preview",
            HTTP_POST,
            portal_backup_preview_handler
        },
        {
            "/api/portal/backup/restore",
            HTTP_POST,
            portal_backup_restore_handler
        },
        {"/api/portal/notes", HTTP_POST, portal_note_handler},
        {"/api/portal/log-map", HTTP_POST, portal_log_map_handler},
        {"/api/portal/time", HTTP_POST, portal_time_handler},
        {"/api/portal/sync", HTTP_POST, portal_sync_handler},
        {"/api/portal/sync/cancel", HTTP_POST, portal_cancel_handler},
        {"/api/portal/auto-sync", HTTP_POST, portal_auto_sync_handler},
        {
            "/api/portal/writes",
            HTTP_POST,
            portal_write_consent_handler
        },
        {
            "/api/portal/maps/stage",
            HTTP_POST,
            portal_map_stage_handler
        },
        {
            "/api/portal/maps/discard",
            HTTP_POST,
            portal_map_discard_handler
        },
        {
            "/api/portal/maps/apply",
            HTTP_POST,
            portal_map_apply_handler
        },
        {
            "/api/portal/maps/auto-apply",
            HTTP_POST,
            portal_map_auto_apply_handler
        },
        {
            "/api/portal/startup/profiles",
            HTTP_GET,
            portal_startup_profiles_handler
        },
        {
            "/api/portal/startup/profiles",
            HTTP_POST,
            portal_startup_profile_save_handler
        },
        {
            "/api/portal/startup/profile",
            HTTP_GET,
            portal_startup_profile_file_handler
        },
        {
            "/api/portal/startup/apply",
            HTTP_POST,
            portal_startup_apply_handler
        },
    };
    for (size_t index = 0U; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        const esp_err_t status = register_uri(
            server,
            routes[index].uri,
            routes[index].method,
            routes[index].handler
        );
        if (status != ESP_OK) {
            return status;
        }
    }
    return ESP_OK;
}
