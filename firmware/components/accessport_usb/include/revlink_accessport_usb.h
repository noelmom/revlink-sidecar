#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "revlink_accessport_protocol.h"
#include "revlink_device_service.h"
#include "revlink_sync_coordinator.h"

#define REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY 128U
#define REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY 256U
#define REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES 32U

/**
 * Receive normalized AccessPort lifecycle events from the ESP-IDF USB host
 * adapter. Consumers must not depend on ESP-IDF descriptor structures.
 */
typedef void (*revlink_accessport_usb_observer_t)(
    void *context,
    const revlink_device_event_t *event
);

typedef struct {
    void *context;
    esp_err_t (*select_device)(
        void *context,
        const revlink_ap_device_info_t *identity
    );
    void (*release_device)(void *context);
    bool (*is_current)(
        void *context,
        const uint8_t *path,
        size_t path_length,
        uint32_t device_time_raw,
        uint32_t expected_size
    );
    esp_err_t (*begin)(
        void *context,
        const uint8_t *name,
        size_t name_length,
        const uint8_t *path,
        size_t path_length,
        uint32_t device_time_raw,
        uint32_t expected_size
    );
    bool (*write)(
        void *context,
        const uint8_t *data,
        size_t length
    );
    esp_err_t (*commit)(void *context);
    void (*abort)(void *context);
} revlink_accessport_download_sink_t;

typedef void (*revlink_accessport_sync_observer_t)(
    void *context,
    const revlink_sync_event_t *event
);

typedef struct {
    void *context;
    revlink_accessport_sync_observer_t observer;
} revlink_accessport_sync_observer_config_t;

typedef void (*revlink_accessport_identity_observer_t)(
    void *context,
    const revlink_ap_device_info_t *identity
);

typedef struct {
    void *context;
    revlink_accessport_identity_observer_t observer;
} revlink_accessport_identity_observer_config_t;

typedef enum {
    REVLINK_ACCESSPORT_UPLOAD_IDLE = 0,
    REVLINK_ACCESSPORT_UPLOAD_RUNNING,
    REVLINK_ACCESSPORT_UPLOAD_VERIFIED,
    REVLINK_ACCESSPORT_UPLOAD_FAILED,
} revlink_accessport_upload_state_t;

typedef struct {
    char name[REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY];
    char path[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    char expected_part_number[REVLINK_AP_PART_NUMBER_CAPACITY];
    char expected_serial[REVLINK_AP_SERIAL_CAPACITY];
    uint32_t modification_time;
    uint32_t size;
    uint8_t source_sha256[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES];
} revlink_accessport_map_upload_request_t;

#if CONFIG_REVLINK_ALLOW_DEVICE_DELETES
typedef enum {
    REVLINK_ACCESSPORT_DELETE_IDLE = 0,
    REVLINK_ACCESSPORT_DELETE_RUNNING,
    REVLINK_ACCESSPORT_DELETE_REMOVED,
    REVLINK_ACCESSPORT_DELETE_FAILED,
} revlink_accessport_delete_state_t;

typedef struct {
    char name[REVLINK_ACCESSPORT_UPLOAD_NAME_CAPACITY];
    char path[REVLINK_ACCESSPORT_UPLOAD_PATH_CAPACITY];
    /*
     * Pinned exactly as a write is. A delete is irreversible, so the device it
     * lands on is checked against both fields immediately before the request
     * goes out, not merely at the time the owner asked.
     */
    char expected_part_number[REVLINK_AP_PART_NUMBER_CAPACITY];
    char expected_serial[REVLINK_AP_SERIAL_CAPACITY];
} revlink_accessport_delete_request_t;

typedef struct {
    revlink_accessport_delete_state_t state;
    revlink_accessport_delete_request_t request;
    bool recovery_required;
    esp_err_t platform_error;
} revlink_accessport_delete_event_t;

typedef void (*revlink_accessport_delete_observer_t)(
    void *context,
    const revlink_accessport_delete_event_t *event
);

typedef struct {
    void *context;
    revlink_accessport_delete_observer_t observe;
} revlink_accessport_delete_sink_t;

/*
 * Register the observer that receives every delete outcome. Without it the
 * transport still refuses to act: a delete nobody can account for is not one
 * this product performs.
 */
esp_err_t revlink_accessport_usb_configure_delete_sink(
    const revlink_accessport_delete_sink_t *sink
);

/*
 * Queue one delete. The transport verifies the pinned identity, confirms the
 * target is present, sends 0x1625, requires the "15" acknowledgement, and
 * re-lists to confirm the entry is gone before reporting success.
 */
esp_err_t revlink_accessport_usb_request_delete(
    const revlink_accessport_delete_request_t *request
);
#endif

typedef struct {
    revlink_accessport_upload_state_t state;
    revlink_accessport_map_upload_request_t request;
    uint8_t readback_sha256[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES];
    bool recovery_required;
    esp_err_t platform_error;
} revlink_accessport_upload_event_t;

typedef struct {
    void *context;
    esp_err_t (*open)(
        void *context,
        const revlink_accessport_map_upload_request_t *request
    );
    esp_err_t (*read)(
        void *context,
        uint8_t *buffer,
        size_t capacity,
        size_t *count
    );
    esp_err_t (*rewind)(void *context);
    void (*close)(void *context);
    bool (*cached_file_matches)(
        void *context,
        const char *path,
        uint32_t size,
        const uint8_t sha256[REVLINK_ACCESSPORT_UPLOAD_SHA256_BYTES]
    );
    void (*observe)(
        void *context,
        const revlink_accessport_upload_event_t *event
    );
} revlink_accessport_upload_source_t;

/**
 * Configure the destination for bounded read-only synchronization.
 * The transport owns no filesystem policy.
 *
 * This must be called before revlink_accessport_usb_start().
 */
esp_err_t revlink_accessport_usb_configure_download_sink(
    const revlink_accessport_download_sink_t *sink
);

/**
 * Configure application-level progress reporting for read-only sync.
 * This must be called before revlink_accessport_usb_start().
 */
esp_err_t revlink_accessport_usb_configure_sync_observer(
    const revlink_accessport_sync_observer_config_t *config
);

/**
 * Configure authoritative AccessPort identity reporting.
 * This must be called before revlink_accessport_usb_start().
 */
esp_err_t revlink_accessport_usb_configure_identity_observer(
    const revlink_accessport_identity_observer_config_t *config
);

/*
 * Configure a microSD-backed upload source and verification observer.
 * The USB adapter never opens paths or owns filesystem policy.
 */
esp_err_t revlink_accessport_usb_configure_upload_source(
    const revlink_accessport_upload_source_t *source
);

/**
 * Start the read-only AccessPort USB monitor.
 *
 * The adapter enumerates the external hub and attached devices, reads only
 * standard USB descriptors, and reports normalized lifecycle events.
 * When the explicit interface-lifecycle acceptance gate is enabled, it also
 * claims and releases interface 0 without submitting a bulk transaction.
 */
esp_err_t revlink_accessport_usb_start(
    revlink_accessport_usb_observer_t observer,
    void *observer_context
);

/**
 * Queue a bounded incremental read-only sync for the accepted AccessPort.
 */
esp_err_t revlink_accessport_usb_request_sync(void);

/**
 * Queue the bounded, read-only identity handshake without listing files.
 */
esp_err_t revlink_accessport_usb_request_identity(void);

/*
 * Queue one non-retrying, identity-pinned map upload. The build-time write
 * capability and explicit runtime consent are enforced by the caller; the
 * transport independently revalidates path, identity, topology, protocol
 * acknowledgements, and read-back bytes.
 */
esp_err_t revlink_accessport_usb_request_map_upload(
    const revlink_accessport_map_upload_request_t *request
);

/**
 * Report whether a failed partial write requires a complete physical detach
 * before another write can be queued.
 */
bool revlink_accessport_usb_write_recovery_required(void);

/**
 * Queue one bounded session-close recovery attempt.
 *
 * The adapter probes the normal initialized session path, sends opcode 0x05,
 * requires acknowledgement 0x35, and never lists or transfers user files.
 */
esp_err_t revlink_accessport_usb_request_close_recovery(void);

/**
 * Request cooperative cancellation of a queued or running sync.
 */
esp_err_t revlink_accessport_usb_cancel_sync(void);

/**
 * Change the ESP-IDF logical root-port state.
 *
 * This disconnects downstream devices in the host stack but does not imply
 * that a board-specific external VBUS or data switch has been isolated.
 */
esp_err_t revlink_accessport_usb_set_root_port_enabled(bool enabled);
