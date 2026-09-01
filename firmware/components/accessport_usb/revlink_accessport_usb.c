#include "revlink_accessport_usb.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "revlink_accessport_catalog.h"
#include "revlink_accessport_protocol.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#if CONFIG_REVLINK_USB_ROOT_PORT_POWER
#define REVLINK_USB_ROOT_PORT_UNPOWERED false
#define REVLINK_USB_ROOT_PORT_POWER_TEXT "enabled"
#else
#define REVLINK_USB_ROOT_PORT_UNPOWERED true
#define REVLINK_USB_ROOT_PORT_POWER_TEXT "disabled"
#endif

#define REVLINK_USB_HOST_TASK_STACK_SIZE 4096
#define REVLINK_USB_CLIENT_TASK_STACK_SIZE 6144
#define REVLINK_USB_DESCRIPTOR_TASK_STACK_SIZE 6144
#define REVLINK_USB_TRANSACTION_TASK_STACK_SIZE 12288
#define REVLINK_USB_HOST_TASK_PRIORITY 2
#define REVLINK_USB_CLIENT_TASK_PRIORITY 3
#define REVLINK_USB_DESCRIPTOR_TASK_PRIORITY 2
#define REVLINK_USB_MAX_DEVICE_ADDRESS 127
#define REVLINK_USB_SCAN_QUEUE_DEPTH 4
#define REVLINK_ACCESSPORT_VID 0x1a84
#define REVLINK_ACCESSPORT_PID 0x0121
#define REVLINK_ACCESSPORT_INTERFACE 0
#define REVLINK_ACCESSPORT_BULK_OUT 0x03
#define REVLINK_ACCESSPORT_BULK_IN 0x82
#define REVLINK_ACCESSPORT_BULK_PACKET_BYTES 512U
#define REVLINK_ACCESSPORT_TRANSACTION_READY_DELAY_US 5000000LL

#if CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE \
    || CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_USB_SESSION_CLOSE_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC \
    || CONFIG_REVLINK_ALLOW_DEVICE_WRITES
#define REVLINK_ROOT_LIST_REQUEST_CAPACITY 64U
#define REVLINK_ROOT_LIST_RESPONSE_CAPACITY 8192U
#define REVLINK_ROOT_LIST_OUT_DEADLINE_MS 3000U
#define REVLINK_ROOT_LIST_IN_DEADLINE_MS 5000U
#endif

#if CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_USB_SESSION_CLOSE_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC \
    || CONFIG_REVLINK_ALLOW_DEVICE_WRITES
#define REVLINK_DOWNLOAD_REQUEST_CAPACITY 512U
#define REVLINK_DOWNLOAD_TEMP_PATH_CAPACITY 192U
#define REVLINK_DOWNLOAD_TRANSFER_CAPACITY 16384U
#define REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY 32768U
#define REVLINK_DOWNLOAD_LISTING_ENTRY_CAPACITY 128U
#define REVLINK_DOWNLOAD_NAME_CAPACITY 128U
#define REVLINK_DOWNLOAD_PATH_CAPACITY 256U
#define REVLINK_DOWNLOAD_MAX_FILE_BYTES (8U * 1024U * 1024U)
#define REVLINK_DOWNLOAD_OUT_DEADLINE_MS 3000U
#define REVLINK_DOWNLOAD_IN_DEADLINE_MS 5000U
#define REVLINK_DISCONNECT_IN_DEADLINE_MS 1500U
#define REVLINK_POLITE_REENUMERATION_WINDOW_US 3000000LL
#define REVLINK_DOWNLOAD_MAX_TRANSFER_OVERHEAD 8192U
#define REVLINK_INCREMENTAL_MAX_DOWNLOADS 4U
#define REVLINK_INCREMENTAL_MAX_SESSION_BYTES (16U * 1024U * 1024U)
#endif

#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
#define REVLINK_UPLOAD_ACK_CAPACITY 64U
#define REVLINK_UPLOAD_SOURCE_READ_BYTES 4096U
#define REVLINK_UPLOAD_TOTAL_DEADLINE_US 120000000LL
#define REVLINK_UPLOAD_READY_ACK 0x07U
#define REVLINK_UPLOAD_COMPLETE_ACK 0x23U
#endif

static const char *TAG = "revlink_usb";

typedef enum {
    DEVICE_ACTION_NONE = 0,
    DEVICE_ACTION_OPEN,
    DEVICE_ACTION_CLOSE,
} device_action_t;

typedef enum {
    DESCRIPTOR_WORK_SCAN = 0,
    DESCRIPTOR_WORK_IDENTITY,
    DESCRIPTOR_WORK_SYNC,
    DESCRIPTOR_WORK_CLOSE_RECOVERY,
    DESCRIPTOR_WORK_MAP_UPLOAD,
} descriptor_work_kind_t;

typedef enum {
    CONTROL_REQUEST_IDENTITY = 0,
    CONTROL_REQUEST_SYNC,
    CONTROL_REQUEST_CLOSE_RECOVERY,
    CONTROL_CANCEL_SYNC,
    CONTROL_REQUEST_MAP_UPLOAD,
} control_request_kind_t;

typedef struct {
    control_request_kind_t kind;
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    revlink_accessport_map_upload_request_t upload;
#endif
} usb_control_request_t;

typedef struct {
    uint8_t address;
    usb_device_handle_t handle;
    device_action_t action;
    bool scan_in_progress;
    bool is_accessport;
    bool eligible_accessport;
    bool acceptance_attempted;
    bool polite_disconnect_sent;
    bool software_reenumeration;
    int64_t transaction_ready_after_us;
    uint32_t attachment_generation;
    revlink_device_identity_t identity;
} enumerated_device_t;

typedef struct {
    descriptor_work_kind_t kind;
    enumerated_device_t *device;
    usb_device_handle_t pinned_handle;
    uint32_t attachment_generation;
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    revlink_accessport_map_upload_request_t upload;
#endif
} descriptor_work_t;

typedef struct {
    revlink_accessport_usb_observer_t observer;
    void *observer_context;
} accessport_usb_monitor_t;

typedef struct {
    usb_host_client_handle_t client;
    QueueHandle_t scan_requests;
    QueueHandle_t scan_results;
    QueueHandle_t transaction_requests;
    QueueHandle_t transaction_results;
    enumerated_device_t devices[REVLINK_USB_MAX_DEVICE_ADDRESS + 1];
    accessport_usb_monitor_t *monitor;
    uint32_t next_attachment_generation;
    uint32_t next_topology_revision;
    bool conflict_latched;
    bool expect_polite_accessport_reenumeration;
    int64_t polite_reenumeration_deadline_us;
    bool physical_detach_recovery_pending;
    int64_t physical_detach_recovery_deadline_us;
} enumeration_client_t;

typedef struct {
    TaskHandle_t startup_task;
    accessport_usb_monitor_t *monitor;
} host_task_startup_t;

static size_t eligible_accessport_count(
    const enumeration_client_t *state
)
{
    size_t count = 0U;
    for (size_t i = 1U; i <= REVLINK_USB_MAX_DEVICE_ADDRESS; ++i) {
        const enumerated_device_t *device = &state->devices[i];
        if (device->handle != NULL
            && device->eligible_accessport
            && device->action != DEVICE_ACTION_CLOSE) {
            ++count;
        }
    }
    return count;
}

static uint8_t bounded_eligible_count(const enumeration_client_t *state)
{
    const size_t count = eligible_accessport_count(state);
    return count > UINT8_MAX ? UINT8_MAX : (uint8_t)count;
}

static uint32_t advance_topology_revision(enumeration_client_t *state)
{
    ++state->next_topology_revision;
    if (state->next_topology_revision == 0U) {
        ++state->next_topology_revision;
    }
    return state->next_topology_revision;
}

#define REVLINK_PHYSICAL_DETACH_RECOVERY_DELAY_MS 1200U
#define REVLINK_ROOT_PORT_RECOVERY_OFF_MS 150U

static void recover_root_port_after_physical_detach(
    enumeration_client_t *state
)
{
    if (state == NULL || !state->physical_detach_recovery_pending
        || esp_timer_get_time()
            < state->physical_detach_recovery_deadline_us) {
        return;
    }

    state->physical_detach_recovery_pending = false;
    state->physical_detach_recovery_deadline_us = 0;
    if (eligible_accessport_count(state) != 0U) {
        return;
    }

    ESP_LOGW(
        TAG,
        "no AccessPort enumerated after the settle delay; cycling the "
        "logical USB root port once for startup/swap recovery"
    );
    const esp_err_t stop_status = usb_host_lib_set_root_port_power(false);
    if (stop_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "device-swap root-port stop failed: %s",
            esp_err_to_name(stop_status)
        );
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(REVLINK_ROOT_PORT_RECOVERY_OFF_MS));
    const esp_err_t start_status = usb_host_lib_set_root_port_power(true);
    if (start_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "device-swap root-port restart failed: %s",
            esp_err_to_name(start_status)
        );
        return;
    }
    ESP_LOGI(TAG, "device-swap logical USB root-port recovery completed");
}

static void wait_for_accessport_transaction_readiness(
    enumerated_device_t *device
)
{
    if (device == NULL || device->transaction_ready_after_us == 0) {
        return;
    }

    const int64_t remaining_us =
        device->transaction_ready_after_us - esp_timer_get_time();
    device->transaction_ready_after_us = 0;
    if (remaining_us <= 0) {
        return;
    }

    const uint32_t remaining_ms =
        (uint32_t)((remaining_us + 999LL) / 1000LL);
    ESP_LOGI(
        TAG,
        "AccessPort protocol readiness gate: waiting %" PRIu32
        " ms after USB enumeration",
        remaining_ms
    );
    vTaskDelay(pdMS_TO_TICKS(remaining_ms));
}

static accessport_usb_monitor_t monitor;
static bool monitor_started;
static revlink_accessport_download_sink_t configured_download_sink;
static bool download_sink_configured;
static revlink_accessport_sync_observer_config_t configured_sync_observer;
static revlink_accessport_identity_observer_config_t
    configured_identity_observer;
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
static revlink_accessport_upload_source_t configured_upload_source;
static bool upload_source_configured;
static atomic_bool write_recovery_required = ATOMIC_VAR_INIT(false);
#endif
#if CONFIG_REVLINK_RUNTIME_SYNC
static QueueHandle_t control_requests;
static atomic_bool sync_cancel_requested;
#endif
#if CONFIG_REVLINK_USB_CLOSE_RECOVERY_ACCEPTANCE
static atomic_bool close_fault_injected = ATOMIC_VAR_INIT(false);
#endif

#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
static void publish_sync_event(const revlink_sync_event_t *event)
{
    if (configured_sync_observer.observer != NULL) {
        configured_sync_observer.observer(
            configured_sync_observer.context,
            event
        );
    }
}
#endif

#if CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
static bool sync_cancelled(void)
{
#if CONFIG_REVLINK_RUNTIME_SYNC
    return atomic_load(&sync_cancel_requested);
#else
    return false;
#endif
}
#endif

static void publish_event(
    accessport_usb_monitor_t *target,
    const revlink_device_event_t *event
)
{
    if (target != NULL && target->observer != NULL) {
        target->observer(target->observer_context, event);
    }
}

static void publish_multiple_accessports(enumeration_client_t *state)
{
    const uint8_t count = bounded_eligible_count(state);
    if (count < 2U) {
        return;
    }
    state->conflict_latched = true;
#if CONFIG_REVLINK_RUNTIME_SYNC
    atomic_store(&sync_cancel_requested, true);
#endif
    const revlink_device_event_t event = {
        .kind = REVLINK_DEVICE_EVENT_MULTIPLE_DETECTED,
        .eligible_device_count = count,
        .topology_revision = state->next_topology_revision,
    };
    ESP_LOGE(
        TAG,
        "MULTIPLE ACCESSPORTS DETECTED: eligible=%u; all device actions "
        "blocked until a complete zero-device reattachment",
        (unsigned int)count
    );
    publish_event(state->monitor, &event);
}

static const char *speed_name(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return "low";
    case USB_SPEED_FULL:
        return "full";
    case USB_SPEED_HIGH:
        return "high";
    default:
        return "unknown";
    }
}

static const char *transfer_type_name(usb_transfer_type_t type)
{
    switch (type) {
    case USB_TRANSFER_TYPE_CTRL:
        return "control";
    case USB_TRANSFER_TYPE_ISOCHRONOUS:
        return "isochronous";
    case USB_TRANSFER_TYPE_BULK:
        return "bulk";
    case USB_TRANSFER_TYPE_INTR:
        return "interrupt";
    default:
        return "unknown";
    }
}

static bool inspect_config_descriptor(
    const usb_config_desc_t *config_desc,
    unsigned int descriptor_number,
    bool is_accessport,
    uint16_t *bulk_max_packet_size
)
{
    const uint8_t *bytes = (const uint8_t *)config_desc;
    const size_t total_length = config_desc->wTotalLength;
    size_t offset = 0;
    bool saw_interface_zero = false;
    bool saw_bulk_out = false;
    bool saw_bulk_in = false;
    uint16_t bulk_out_max_packet = 0;
    uint16_t bulk_in_max_packet = 0;
    uint8_t current_interface = UINT8_MAX;

    ESP_LOGI(
        TAG,
        "configuration descriptor=%u value=%u interfaces=%u total_length=%u "
        "max_power=%u mA",
        descriptor_number,
        config_desc->bConfigurationValue,
        config_desc->bNumInterfaces,
        (unsigned int)total_length,
        (unsigned int)config_desc->bMaxPower * 2U
    );

    while (offset + USB_STANDARD_DESC_SIZE <= total_length) {
        const usb_standard_desc_t *standard =
            (const usb_standard_desc_t *)(bytes + offset);
        if (standard->bLength < USB_STANDARD_DESC_SIZE
            || offset + standard->bLength > total_length) {
            ESP_LOGW(
                TAG,
                "invalid descriptor at offset=%u length=%u",
                (unsigned int)offset,
                standard->bLength
            );
            break;
        }

        if (standard->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE
            && standard->bLength >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *interface_desc =
                (const usb_intf_desc_t *)standard;
            ESP_LOGI(
                TAG,
                "interface number=%u alternate=%u endpoints=%u class=0x%02x "
                "subclass=0x%02x protocol=0x%02x",
                interface_desc->bInterfaceNumber,
                interface_desc->bAlternateSetting,
                interface_desc->bNumEndpoints,
                interface_desc->bInterfaceClass,
                interface_desc->bInterfaceSubClass,
                interface_desc->bInterfaceProtocol
            );
            if (interface_desc->bInterfaceNumber
                == REVLINK_ACCESSPORT_INTERFACE) {
                saw_interface_zero = true;
            }
            current_interface = interface_desc->bInterfaceNumber;
        } else if (
            standard->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT
            && standard->bLength >= USB_EP_DESC_SIZE
        ) {
            const usb_ep_desc_t *endpoint_desc =
                (const usb_ep_desc_t *)standard;
            const usb_transfer_type_t transfer_type =
                USB_EP_DESC_GET_XFERTYPE(endpoint_desc);
            ESP_LOGI(
                TAG,
                "endpoint address=0x%02x type=%s max_packet=%u interval=%u",
                endpoint_desc->bEndpointAddress,
                transfer_type_name(transfer_type),
                USB_EP_DESC_GET_MPS(endpoint_desc),
                endpoint_desc->bInterval
            );
            if (current_interface == REVLINK_ACCESSPORT_INTERFACE
                && transfer_type == USB_TRANSFER_TYPE_BULK
                && endpoint_desc->bEndpointAddress
                    == REVLINK_ACCESSPORT_BULK_OUT) {
                saw_bulk_out = true;
                bulk_out_max_packet = USB_EP_DESC_GET_MPS(endpoint_desc);
            }
            if (current_interface == REVLINK_ACCESSPORT_INTERFACE
                && transfer_type == USB_TRANSFER_TYPE_BULK
                && endpoint_desc->bEndpointAddress
                    == REVLINK_ACCESSPORT_BULK_IN) {
                saw_bulk_in = true;
                bulk_in_max_packet = USB_EP_DESC_GET_MPS(endpoint_desc);
            }
        }

        offset += standard->bLength;
    }

    if (is_accessport) {
        if (saw_interface_zero && saw_bulk_out && saw_bulk_in) {
            ESP_LOGI(
                TAG,
                "ACCESSPORT CONFIG MATCH: value=%u interface=0 "
                "bulk_out=0x03 bulk_in=0x82",
                config_desc->bConfigurationValue
            );
        } else {
            ESP_LOGW(
                TAG,
                "ACCESSPORT ENUMERATION INCOMPLETE: interface0=%s "
                "bulk_out_0x03=%s bulk_in_0x82=%s",
                saw_interface_zero ? "yes" : "no",
                saw_bulk_out ? "yes" : "no",
                saw_bulk_in ? "yes" : "no"
            );
        }
    }

    if (saw_bulk_out && saw_bulk_in && bulk_max_packet_size != NULL) {
        *bulk_max_packet_size =
            bulk_out_max_packet < bulk_in_max_packet
            ? bulk_out_max_packet
            : bulk_in_max_packet;
    }

    return saw_interface_zero && saw_bulk_out && saw_bulk_in;
}

#if CONFIG_REVLINK_USB_INTERFACE_CLAIM_ACCEPTANCE
static void run_interface_claim_acceptance(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    accessport_usb_monitor_t *event_monitor,
    const revlink_device_identity_t *identity
)
{
    ESP_LOGI(
        TAG,
        "ACCESSPORT INTERFACE ACCEPTANCE START: interface=0 alternate=0 "
        "bulk_transfers=0"
    );

    esp_err_t err = usb_host_interface_claim(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE,
        0
    );
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ACCESSPORT INTERFACE CLAIM FAILED: %s",
            esp_err_to_name(err)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "ACCESSPORT INTERFACE CLAIM PASSED: interface=0 endpoint_pipes=4 "
        "bulk_transfers=0"
    );
    const revlink_device_event_t opened_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_OPENED,
        .identity = *identity,
    };
    publish_event(event_monitor, &opened_event);

    err = usb_host_interface_release(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE
    );
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ACCESSPORT INTERFACE RELEASE FAILED: %s",
            esp_err_to_name(err)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "ACCESSPORT INTERFACE RELEASE PASSED: interface=0 bulk_transfers=0"
    );
    const revlink_device_event_t closed_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED,
        .identity = *identity,
    };
    publish_event(event_monitor, &closed_event);
    ESP_LOGI(TAG, "ACCESSPORT INTERFACE LIFECYCLE ACCEPTANCE PASSED");
}
#endif

#if CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE \
    || CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC \
    || CONFIG_REVLINK_ALLOW_DEVICE_WRITES
typedef struct {
    SemaphoreHandle_t completion;
    usb_transfer_status_t status;
    int actual_num_bytes;
} bounded_transfer_wait_t;

static void bounded_transfer_complete(usb_transfer_t *transfer)
{
    bounded_transfer_wait_t *wait =
        (bounded_transfer_wait_t *)transfer->context;
    wait->status = transfer->status;
    wait->actual_num_bytes = transfer->actual_num_bytes;
    xSemaphoreGive(wait->completion);
}

static bool wait_for_bounded_transfer(
    usb_device_handle_t device,
    uint8_t endpoint,
    bounded_transfer_wait_t *wait,
    uint32_t deadline_ms
)
{
    if (xSemaphoreTake(wait->completion, pdMS_TO_TICKS(deadline_ms)) == pdTRUE) {
        return true;
    }

    ESP_LOGE(
        TAG,
        "READ-ONLY ACCEPTANCE DEADLINE: endpoint=0x%02x deadline_ms=%u; "
        "halting and canceling without retry",
        endpoint,
        (unsigned int)deadline_ms
    );
    const esp_err_t halt_status = usb_host_endpoint_halt(device, endpoint);
    const esp_err_t flush_status = halt_status == ESP_OK
        ? usb_host_endpoint_flush(device, endpoint)
        : halt_status;
    ESP_LOGW(
        TAG,
        "READ-ONLY ACCEPTANCE CANCEL: halt=%s flush=%s",
        esp_err_to_name(halt_status),
        esp_err_to_name(flush_status)
    );

    /*
     * A submitted transfer cannot be freed while in flight and its callback
     * owns this wait context. After the wire deadline, wait only for the host
     * stack's cancellation completion; no further device transaction is sent.
     */
    xSemaphoreTake(wait->completion, portMAX_DELAY);
    if (halt_status == ESP_OK) {
        const esp_err_t clear_status =
            usb_host_endpoint_clear(device, endpoint);
        ESP_LOGW(
            TAG,
            "READ-ONLY ACCEPTANCE CANCEL COMPLETE: clear=%s",
            esp_err_to_name(clear_status)
        );
    }
    return false;
}

static bool submit_and_wait(
    usb_transfer_t *transfer,
    uint32_t deadline_ms
)
{
    bounded_transfer_wait_t *wait =
        (bounded_transfer_wait_t *)transfer->context;
    wait->status = USB_TRANSFER_STATUS_ERROR;
    wait->actual_num_bytes = 0;

    const esp_err_t submit_status = usb_host_transfer_submit(transfer);
    if (submit_status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ACCEPTANCE SUBMIT FAILED: endpoint=0x%02x error=%s",
            transfer->bEndpointAddress,
            esp_err_to_name(submit_status)
        );
        return false;
    }
    return wait_for_bounded_transfer(
        transfer->device_handle,
        transfer->bEndpointAddress,
        wait,
        deadline_ms
    );
}

#endif

#if CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE
static void run_root_list_acceptance(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    accessport_usb_monitor_t *event_monitor,
    const revlink_device_identity_t *identity
)
{
    ESP_LOGW(
        TAG,
        "READ-ONLY ROOT LIST ACCEPTANCE START: one request, no retries, "
        "response_limit=%u, writes=disabled",
        REVLINK_ROOT_LIST_RESPONSE_CAPACITY
    );

    uint8_t request[REVLINK_ROOT_LIST_REQUEST_CAPACITY] = {0};
    size_t request_length = 0;
    const revlink_ap_status_t build_status = revlink_ap_build_list(
        NULL,
        0U,
        request,
        sizeof(request),
        &request_length
    );
    if (build_status != REVLINK_AP_OK) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: request build=%s",
            revlink_ap_status_name(build_status)
        );
        return;
    }

    esp_err_t status = usb_host_interface_claim(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE,
        0
    );
    if (status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: interface claim=%s",
            esp_err_to_name(status)
        );
        return;
    }
    const revlink_device_event_t opened_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_OPENED,
        .identity = *identity,
    };
    publish_event(event_monitor, &opened_event);

    SemaphoreHandle_t completion = xSemaphoreCreateBinary();
    usb_transfer_t *out_transfer = NULL;
    usb_transfer_t *in_transfer = NULL;
    bounded_transfer_wait_t wait = {
        .completion = completion,
    };
    bool passed = false;

    if (completion == NULL
        || usb_host_transfer_alloc(
            request_length,
            0,
            &out_transfer
        ) != ESP_OK
        || usb_host_transfer_alloc(
            REVLINK_ROOT_LIST_RESPONSE_CAPACITY,
            0,
            &in_transfer
        ) != ESP_OK) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: transfer allocation"
        );
        goto cleanup;
    }

    memcpy(out_transfer->data_buffer, request, request_length);
    out_transfer->num_bytes = (int)request_length;
    out_transfer->device_handle = device;
    out_transfer->bEndpointAddress = REVLINK_ACCESSPORT_BULK_OUT;
    out_transfer->callback = bounded_transfer_complete;
    out_transfer->context = &wait;
    if (!submit_and_wait(
            out_transfer,
            REVLINK_ROOT_LIST_OUT_DEADLINE_MS
        )
        || wait.status != USB_TRANSFER_STATUS_COMPLETED
        || wait.actual_num_bytes != (int)request_length) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: OUT status=%d bytes=%d/%u",
            wait.status,
            wait.actual_num_bytes,
            (unsigned int)request_length
        );
        goto cleanup;
    }
    ESP_LOGI(
        TAG,
        "READ-ONLY ROOT LIST OUT PASSED: bytes=%u endpoint=0x03",
        (unsigned int)request_length
    );

    in_transfer->num_bytes = REVLINK_ROOT_LIST_RESPONSE_CAPACITY;
    in_transfer->device_handle = device;
    in_transfer->bEndpointAddress = REVLINK_ACCESSPORT_BULK_IN;
    in_transfer->callback = bounded_transfer_complete;
    in_transfer->context = &wait;
    if (!submit_and_wait(
            in_transfer,
            REVLINK_ROOT_LIST_IN_DEADLINE_MS
        )
        || wait.status != USB_TRANSFER_STATUS_COMPLETED
        || wait.actual_num_bytes <= 0) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: IN status=%d bytes=%d",
            wait.status,
            wait.actual_num_bytes
        );
        goto cleanup;
    }

    const size_t response_length = (size_t)wait.actual_num_bytes;
    revlink_ap_record_view_t response = {0};
    const revlink_ap_status_t parse_status = revlink_ap_parse_record(
        in_transfer->data_buffer,
        response_length,
        &response
    );
    if (parse_status != REVLINK_AP_OK
        || response.opcode != REVLINK_AP_OPCODE_RESPONSE) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: response parse=%s "
            "opcode=0x%04x bytes=%u",
            revlink_ap_status_name(parse_status),
            response.opcode,
            (unsigned int)response_length
        );
        goto cleanup;
    }

    size_t entry_count = 0;
    const revlink_ap_status_t listing_status =
        revlink_ap_parse_listing_payload(
            response.payload,
            response.payload_length,
            NULL,
            0U,
            &entry_count
        );
    if (listing_status != REVLINK_AP_OK
        && listing_status != REVLINK_AP_BUFFER_TOO_SMALL) {
        ESP_LOGE(
            TAG,
            "READ-ONLY ROOT LIST ACCEPTANCE FAILED: listing parse=%s",
            revlink_ap_status_name(listing_status)
        );
        goto cleanup;
    }

    passed = true;
    ESP_LOGI(
        TAG,
        "READ-ONLY ROOT LIST ACCEPTANCE PASSED: response_bytes=%u "
        "entries=%u checksum=valid opcode=0x1601",
        (unsigned int)response_length,
        (unsigned int)entry_count
    );

cleanup:
    if (in_transfer != NULL) {
        usb_host_transfer_free(in_transfer);
    }
    if (out_transfer != NULL) {
        usb_host_transfer_free(out_transfer);
    }
    if (completion != NULL) {
        vSemaphoreDelete(completion);
    }
    status = usb_host_interface_release(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE
    );
    ESP_LOGI(
        TAG,
        "READ-ONLY ROOT LIST SESSION CLOSED: interface_release=%s result=%s",
        esp_err_to_name(status),
        passed ? "passed" : "failed"
    );
    const revlink_device_event_t closed_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED,
        .identity = *identity,
    };
    publish_event(event_monitor, &closed_event);
}
#endif

#if CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC \
    || CONFIG_REVLINK_ALLOW_DEVICE_WRITES
typedef struct {
    const uint8_t *directory;
    size_t directory_length;
    const char *label;
    const uint8_t *extension;
    size_t extension_length;
    const uint8_t *alternate_extension;
    size_t alternate_extension_length;
} revlink_read_collection_t;

static bool safe_collection_entry(
    const revlink_ap_listing_entry_t *entry,
    const revlink_read_collection_t *collection
)
{
    if (entry == NULL || entry->is_directory || entry->size == 0U
        || entry->size > REVLINK_DOWNLOAD_MAX_FILE_BYTES
        || entry->name == NULL || entry->name_length == 0U
        || entry->name_length >= REVLINK_DOWNLOAD_NAME_CAPACITY
        || entry->path == NULL || collection == NULL
        || entry->path_length
            != collection->directory_length + 1U + entry->name_length
        || memcmp(
               entry->path,
               collection->directory,
               collection->directory_length
           ) != 0
        || entry->path[collection->directory_length] != '/'
        || memcmp(
               entry->path + collection->directory_length + 1U,
               entry->name,
               entry->name_length
           ) != 0) {
        return false;
    }

    for (size_t index = 0; index < entry->name_length; ++index) {
        const uint8_t value = entry->name[index];
        const bool allowed =
            (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9')
            || value == '.' || value == '_' || value == '-'
            || value == ' ' || value == '(' || value == ')';
        if (!allowed) {
            return false;
        }
    }

    const bool primary_matches =
        entry->name_length > collection->extension_length
        && memcmp(
               entry->name + entry->name_length
                   - collection->extension_length,
               collection->extension,
               collection->extension_length
           ) == 0;
    const bool alternate_matches =
        collection->alternate_extension != NULL
        && entry->name_length > collection->alternate_extension_length
        && memcmp(
               entry->name + entry->name_length
                   - collection->alternate_extension_length,
               collection->alternate_extension,
               collection->alternate_extension_length
           ) == 0;
    return primary_matches || alternate_matches;
}

static bool entry_precedes(
    const revlink_ap_listing_entry_t *candidate,
    const revlink_ap_listing_entry_t *selected
)
{
    if (selected == NULL || candidate->size < selected->size) {
        return true;
    }
    if (candidate->size > selected->size) {
        return false;
    }
    const size_t common =
        candidate->path_length < selected->path_length
        ? candidate->path_length
        : selected->path_length;
    const int order = memcmp(candidate->path, selected->path, common);
    return order < 0
        || (order == 0 && candidate->path_length < selected->path_length);
}

static bool acceptance_out(
    usb_transfer_t *transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    const uint8_t *request,
    size_t request_length
)
{
    memcpy(transfer->data_buffer, request, request_length);
    transfer->num_bytes = (int)request_length;
    transfer->device_handle = device;
    transfer->bEndpointAddress = REVLINK_ACCESSPORT_BULK_OUT;
    transfer->callback = bounded_transfer_complete;
    transfer->context = wait;
    return submit_and_wait(transfer, REVLINK_DOWNLOAD_OUT_DEADLINE_MS)
        && wait->status == USB_TRANSFER_STATUS_COMPLETED
        && wait->actual_num_bytes == (int)request_length;
}

static bool acceptance_in(
    usb_transfer_t *transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait
)
{
    /*
     * Allow a bounded multi-packet bulk transfer. Directory listings that
     * exceed one 512-byte high-speed packet can be delivered continuously by
     * the AccessPort; submitting only one packet causes the host controller
     * to report USB_TRANSFER_STATUS_OVERFLOW before any bytes are published.
     * The listing and download decoders still enforce their independent
     * record and session limits across repeated 16 KiB reads.
     */
    transfer->num_bytes = REVLINK_DOWNLOAD_TRANSFER_CAPACITY;
    transfer->device_handle = device;
    transfer->bEndpointAddress = REVLINK_ACCESSPORT_BULK_IN;
    transfer->callback = bounded_transfer_complete;
    transfer->context = wait;
    return submit_and_wait(transfer, REVLINK_DOWNLOAD_IN_DEADLINE_MS)
        && wait->status == USB_TRANSFER_STATUS_COMPLETED
        && wait->actual_num_bytes > 0;
}

static bool send_polite_disconnect(
    usb_transfer_t *out_transfer,
    usb_transfer_t *in_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    bool *acknowledged
)
{
    uint8_t request[REVLINK_AP_DISCONNECT_REQUEST_SIZE] = {0};
    size_t request_length = 0U;
    if (out_transfer == NULL || wait == NULL || acknowledged == NULL
        || revlink_ap_disconnect_request(
               request,
               sizeof(request),
               &request_length
           ) != REVLINK_AP_OK
        || !acceptance_out(
               out_transfer,
               device,
               wait,
               request,
               request_length
           )) {
        return false;
    }

    *acknowledged = false;
    if (in_transfer != NULL) {
        in_transfer->num_bytes = REVLINK_ACCESSPORT_BULK_PACKET_BYTES;
        in_transfer->device_handle = device;
        in_transfer->bEndpointAddress = REVLINK_ACCESSPORT_BULK_IN;
        in_transfer->callback = bounded_transfer_complete;
        in_transfer->context = wait;
        if (submit_and_wait(
                in_transfer,
                REVLINK_DISCONNECT_IN_DEADLINE_MS
            )
            && wait->status == USB_TRANSFER_STATUS_COMPLETED
            && wait->actual_num_bytes > 0) {
            *acknowledged = revlink_ap_is_disconnect_ack(
                in_transfer->data_buffer,
                (size_t)wait->actual_num_bytes
            );
        }
    }

    ESP_LOGI(
        TAG,
        "POLITE SESSION CLOSE: subtype=0x05 out=sent ack_0x35=%s; "
        "software re-enumeration expected",
        *acknowledged ? "yes" : "not-observed"
    );
    return true;
}

#if CONFIG_REVLINK_USB_SESSION_CLOSE_ACCEPTANCE
static void run_session_close_acceptance(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    accessport_usb_monitor_t *event_monitor,
    const revlink_device_identity_t *identity,
    bool *polite_disconnect_sent
)
{
    ESP_LOGW(
        TAG,
        "POLITE SESSION-CLOSE ACCEPTANCE START: one subtype=0x05, "
        "no file operations, AccessPort writes=disabled"
    );

    esp_err_t status = usb_host_interface_claim(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE,
        0
    );
    if (status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "POLITE SESSION-CLOSE ACCEPTANCE FAILED: interface claim=%s",
            esp_err_to_name(status)
        );
        return;
    }

    const revlink_device_event_t opened_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_OPENED,
        .identity = *identity,
    };
    publish_event(event_monitor, &opened_event);

    SemaphoreHandle_t completion = xSemaphoreCreateBinary();
    usb_transfer_t *out_transfer = NULL;
    usb_transfer_t *in_transfer = NULL;
    bounded_transfer_wait_t wait = {
        .completion = completion,
    };
    bool acknowledged = false;
    bool sent = false;

    if (completion == NULL
        || usb_host_transfer_alloc(
               REVLINK_AP_DISCONNECT_REQUEST_SIZE,
               0,
               &out_transfer
           ) != ESP_OK
        || usb_host_transfer_alloc(
               REVLINK_ACCESSPORT_BULK_PACKET_BYTES,
               0,
               &in_transfer
           ) != ESP_OK) {
        ESP_LOGE(
            TAG,
            "POLITE SESSION-CLOSE ACCEPTANCE FAILED: transfer allocation"
        );
        goto cleanup;
    }

    sent = send_polite_disconnect(
        out_transfer,
        in_transfer,
        device,
        &wait,
        &acknowledged
    );
    if (polite_disconnect_sent != NULL) {
        *polite_disconnect_sent = sent;
    }
    ESP_LOGI(
        TAG,
        "POLITE SESSION-CLOSE ACCEPTANCE %s: out=%s ack_0x35=%s",
        sent ? "PASSED" : "FAILED",
        sent ? "sent" : "failed",
        acknowledged ? "yes" : "not-observed"
    );

cleanup:
    if (in_transfer != NULL) {
        usb_host_transfer_free(in_transfer);
    }
    if (out_transfer != NULL) {
        usb_host_transfer_free(out_transfer);
    }
    if (completion != NULL) {
        vSemaphoreDelete(completion);
    }
    status = usb_host_interface_release(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE
    );
    ESP_LOGI(
        TAG,
        "POLITE SESSION-CLOSE ACCEPTANCE CLEANUP: interface_release=%s",
        esp_err_to_name(status)
    );
    const revlink_device_event_t closed_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED,
        .identity = *identity,
    };
    publish_event(event_monitor, &closed_event);
}
#endif

static bool read_archive_record(
    usb_transfer_t *in_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    uint8_t *record,
    size_t record_capacity,
    size_t *record_length
)
{
    if (record == NULL || record_length == NULL
        || record_capacity < REVLINK_AP_PREFIX_SIZE
            + REVLINK_AP_CHECKSUM_SIZE) {
        return false;
    }
    size_t received_total = 0U;
    size_t target_length = 0U;
    const size_t maximum_reads =
        record_capacity / REVLINK_ACCESSPORT_BULK_PACKET_BYTES + 2U;
    for (size_t index = 0U;
         received_total < target_length || target_length == 0U;
         ++index) {
        if (index >= maximum_reads) {
            return false;
        }
        if (sync_cancelled()) {
            return false;
        }
        if (!acceptance_in(in_transfer, device, wait)) {
            return false;
        }
        const size_t received = (size_t)wait->actual_num_bytes;
        if (received > record_capacity - received_total) {
            return false;
        }
        memcpy(
            record + received_total,
            in_transfer->data_buffer,
            received
        );
        received_total += received;
        if (target_length == 0U && received_total >= 5U) {
            if (record[0] != 0x02U || record[1] != 0x00U
                || record[2] != 0x00U) {
                return false;
            }
            target_length =
                (((size_t)record[3] << 8U) | (size_t)record[4]) + 7U;
            if (target_length > record_capacity
                || target_length < REVLINK_AP_PREFIX_SIZE
                    + REVLINK_AP_CHECKSUM_SIZE) {
                return false;
            }
        }
        if (target_length != 0U && received_total >= target_length) {
            break;
        }
    }
    if (target_length == 0U || received_total != target_length) {
        return false;
    }
    *record_length = received_total;
    return true;
}

static bool read_true_device_identity(
    usb_transfer_t *out_transfer,
    usb_transfer_t *in_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    revlink_ap_device_info_t *identity
)
{
    uint8_t request[REVLINK_DOWNLOAD_REQUEST_CAPACITY];
    uint8_t *response = malloc(REVLINK_DOWNLOAD_TRANSFER_CAPACITY);
    if (identity == NULL || response == NULL) {
        free(response);
        return false;
    }
    memset(identity, 0, sizeof(*identity));
    for (size_t index = 0U;
         index < REVLINK_AP_IDENTITY_HANDSHAKE_COUNT;
         ++index) {
        if (sync_cancelled()) {
            free(response);
            return false;
        }
        size_t request_length = 0U;
        revlink_ap_status_t protocol_status =
            revlink_ap_identity_handshake_request(
                index,
                request,
                sizeof(request),
                &request_length
            );
        if (protocol_status != REVLINK_AP_OK
            || !acceptance_out(
                   out_transfer,
                   device,
                   wait,
                   request,
                   request_length
               )) {
            ESP_LOGE(
                TAG,
                "IDENTITY FAILED: mini=%u OUT protocol=%s usb_status=%d",
                (unsigned int)index,
                revlink_ap_status_name(protocol_status),
                wait->status
            );
            free(response);
            return false;
        }
        ESP_LOGI(
            TAG,
            "IDENTITY OUT COMPLETE: mini=%u bytes=%u endpoint=0x03",
            (unsigned int)index,
            (unsigned int)request_length
        );
        ESP_LOG_BUFFER_HEX_LEVEL(
            TAG,
            request,
            request_length,
            ESP_LOG_INFO
        );
        size_t response_length = 0U;
        if (!read_archive_record(
                in_transfer,
                device,
                wait,
                response,
                REVLINK_DOWNLOAD_TRANSFER_CAPACITY,
                &response_length
            )) {
            ESP_LOGE(
                TAG,
                "IDENTITY FAILED: mini=%u incomplete response",
                (unsigned int)index
            );
            free(response);
            return false;
        }
        revlink_ap_record_view_t record = {0};
        protocol_status = revlink_ap_parse_record(
            response,
            response_length,
            &record
        );
        if (protocol_status != REVLINK_AP_OK
            || record.opcode != REVLINK_AP_OPCODE_RESPONSE) {
            ESP_LOGE(
                TAG,
                "IDENTITY FAILED: mini=%u record=%s opcode=0x%04x",
                (unsigned int)index,
                revlink_ap_status_name(protocol_status),
                record.opcode
            );
            free(response);
            return false;
        }
        if (index == REVLINK_AP_IDENTITY_RESPONSE_INDEX) {
            protocol_status = revlink_ap_parse_device_identity_payload(
                record.payload,
                record.payload_length,
                identity
            );
            if (protocol_status != REVLINK_AP_OK) {
                ESP_LOGE(
                    TAG,
                    "IDENTITY FAILED: payload=%s",
                    revlink_ap_status_name(protocol_status)
                );
                free(response);
                return false;
            }
        }
    }
    free(response);
    if (identity->serial[0] == '\0') {
        ESP_LOGE(TAG, "IDENTITY FAILED: authoritative serial is empty");
        return false;
    }
    ESP_LOGI(
        TAG,
        "TRUE DEVICE IDENTITY PASSED: part=%s serial=%s firmware=%s",
        identity->part_number,
        identity->serial,
        identity->firmware
    );
    return true;
}

static bool download_file_entry(
    usb_transfer_t *out_transfer,
    usb_transfer_t *in_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    const revlink_ap_listing_entry_t *entry
)
{
    uint8_t request[REVLINK_DOWNLOAD_REQUEST_CAPACITY];
    size_t request_length = 0U;
    bool sink_started = false;
    revlink_ap_download_decoder_t *decoder = NULL;
    revlink_ap_status_t protocol_status = REVLINK_AP_OK;

    if (sync_cancelled()) {
        return false;
    }

    esp_err_t status = configured_download_sink.begin(
        configured_download_sink.context,
        entry->name,
        entry->name_length,
        entry->path,
        entry->path_length,
        entry->device_time_raw,
        entry->size
    );
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: cache begin=%s", esp_err_to_name(status));
        return false;
    }
    sink_started = true;

    protocol_status = revlink_ap_build_download(
        entry->name,
        entry->name_length,
        entry->path,
        entry->path_length,
        request,
        sizeof(request),
        &request_length
    );
    if (protocol_status != REVLINK_AP_OK
        || !acceptance_out(
               out_transfer,
               device,
               wait,
               request,
               request_length
           )) {
        ESP_LOGE(
            TAG,
            "DOWNLOAD FAILED: 0x1620 protocol=%s usb_status=%d",
            revlink_ap_status_name(protocol_status),
            wait->status
        );
        goto cleanup;
    }

    uint8_t temporary_path[REVLINK_DOWNLOAD_TEMP_PATH_CAPACITY];
    static const uint8_t temporary_prefix[] = "/tmp/revlink/";
    if (sizeof(temporary_prefix) - 1U + entry->name_length
        > sizeof(temporary_path)) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: temporary path exceeds bound");
        goto cleanup;
    }
    memcpy(
        temporary_path,
        temporary_prefix,
        sizeof(temporary_prefix) - 1U
    );
    memcpy(
        temporary_path + sizeof(temporary_prefix) - 1U,
        entry->name,
        entry->name_length
    );
    const size_t temporary_path_length =
        sizeof(temporary_prefix) - 1U + entry->name_length;
    protocol_status = revlink_ap_build_temp_notice(
        temporary_path,
        temporary_path_length,
        request,
        sizeof(request),
        &request_length
    );
    if (protocol_status != REVLINK_AP_OK
        || !acceptance_out(
               out_transfer,
               device,
               wait,
               request,
               request_length
           )) {
        ESP_LOGE(
            TAG,
            "DOWNLOAD FAILED: 0x1621 protocol=%s usb_status=%d",
            revlink_ap_status_name(protocol_status),
            wait->status
        );
        goto cleanup;
    }

    decoder = calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: decoder allocation");
        goto cleanup;
    }
    revlink_ap_download_decoder_init(decoder, entry->size);
    const size_t maximum_reads =
        (entry->size + REVLINK_DOWNLOAD_MAX_TRANSFER_OVERHEAD
            + REVLINK_ACCESSPORT_BULK_PACKET_BYTES - 1U)
        / REVLINK_ACCESSPORT_BULK_PACKET_BYTES + 2U;
    for (size_t read_index = 0U; read_index < maximum_reads; ++read_index) {
        if (sync_cancelled()) {
            ESP_LOGW(TAG, "DOWNLOAD CANCELLED: stopping between payload reads");
            goto cleanup;
        }
        if (!acceptance_in(in_transfer, device, wait)) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: payload IN read=%u status=%d bytes=%d",
                (unsigned int)(read_index + 1U),
                wait->status,
                wait->actual_num_bytes
            );
            goto cleanup;
        }
        protocol_status = revlink_ap_download_decoder_feed(
            decoder,
            in_transfer->data_buffer,
            (size_t)wait->actual_num_bytes,
            configured_download_sink.write,
            configured_download_sink.context
        );
        if (protocol_status != REVLINK_AP_OK) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: streamed decode=%s read=%u",
                revlink_ap_status_name(protocol_status),
                (unsigned int)(read_index + 1U)
            );
            goto cleanup;
        }
        if (revlink_ap_download_decoder_complete(decoder)) {
            break;
        }
    }
    if (!revlink_ap_download_decoder_complete(decoder)) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: bounded read count exhausted");
        goto cleanup;
    }

    status = configured_download_sink.commit(
        configured_download_sink.context
    );
    sink_started = false;
    if (status != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DOWNLOAD FAILED: atomic cache commit=%s",
            esp_err_to_name(status)
        );
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "READ-ONLY FILE PASSED: name=%.*s bytes=%" PRIu32
        " trailing_bytes=%u protocol=valid cache=atomic",
        (int)entry->name_length,
        (const char *)entry->name,
        entry->size,
        (unsigned int)decoder->trailing_bytes
    );
    free(decoder);
    return true;

cleanup:
    if (sink_started) {
        configured_download_sink.abort(configured_download_sink.context);
    }
    free(decoder);
    return false;
}

#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
static void upload_observe(
    const revlink_accessport_map_upload_request_t *request,
    revlink_accessport_upload_state_t state,
    esp_err_t platform_error,
    bool recovery_required
)
{
    if (configured_upload_source.observe == NULL) {
        return;
    }
    const revlink_accessport_upload_event_t event = {
        .state = state,
        .request = *request,
        .recovery_required = recovery_required,
        .platform_error = platform_error,
    };
    configured_upload_source.observe(
        configured_upload_source.context,
        &event
    );
}

static bool read_upload_ack(
    usb_transfer_t *in_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    uint8_t expected_subtype
)
{
    if (!acceptance_in(in_transfer, device, wait)) {
        return false;
    }
    return revlink_ap_is_plain_ack(
        in_transfer->data_buffer,
        (size_t)wait->actual_num_bytes,
        expected_subtype
    );
}

static bool map_destination_absent(
    usb_transfer_t *out_transfer,
    usb_transfer_t *in_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    const char *path
)
{
    static const uint8_t map_directory[] = "maps";
    uint8_t request[REVLINK_DOWNLOAD_REQUEST_CAPACITY];
    size_t request_length = 0U;
    if (revlink_ap_build_list(
            map_directory,
            sizeof(map_directory) - 1U,
            request,
            sizeof(request),
            &request_length
        ) != REVLINK_AP_OK
        || !acceptance_out(
               out_transfer,
               device,
               wait,
               request,
               request_length
           )) {
        return false;
    }

    uint8_t *response = malloc(REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY);
    if (response == NULL) {
        return false;
    }
    size_t response_length = 0U;
    bool absent = false;
    if (!read_archive_record(
            in_transfer,
            device,
            wait,
            response,
            REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY,
            &response_length
        )) {
        free(response);
        return false;
    }
    revlink_ap_record_view_t record = {0};
    size_t entry_count = 0U;
    revlink_ap_status_t protocol_status = revlink_ap_parse_record(
        response,
        response_length,
        &record
    );
    if (protocol_status != REVLINK_AP_OK
        || record.opcode != REVLINK_AP_OPCODE_RESPONSE) {
        free(response);
        return false;
    }
    protocol_status = revlink_ap_parse_listing_payload(
        record.payload,
        record.payload_length,
        NULL,
        0U,
        &entry_count
    );
    if ((protocol_status != REVLINK_AP_OK
            && protocol_status != REVLINK_AP_BUFFER_TOO_SMALL)
        || entry_count > REVLINK_DOWNLOAD_LISTING_ENTRY_CAPACITY) {
        free(response);
        return false;
    }
    revlink_ap_listing_entry_t *entries =
        entry_count > 0U ? calloc(entry_count, sizeof(*entries)) : NULL;
    if (entry_count > 0U && entries == NULL) {
        free(response);
        return false;
    }
    if (entry_count > 0U
        && revlink_ap_parse_listing_payload(
               record.payload,
               record.payload_length,
               entries,
               entry_count,
               &entry_count
           ) != REVLINK_AP_OK) {
        free(entries);
        free(response);
        return false;
    }

    absent = true;
    const size_t path_length = strlen(path);
    for (size_t index = 0U; index < entry_count; ++index) {
        bool same_path = entries[index].path_length == path_length;
        for (size_t offset = 0U; same_path && offset < path_length; ++offset) {
            uint8_t device_value = entries[index].path[offset];
            uint8_t requested_value = (uint8_t)path[offset];
            if (device_value >= 'A' && device_value <= 'Z') {
                device_value =
                    (uint8_t)(device_value + ('a' - 'A'));
            }
            if (requested_value >= 'A' && requested_value <= 'Z') {
                requested_value =
                    (uint8_t)(requested_value + ('a' - 'A'));
            }
            same_path = device_value == requested_value;
        }
        if (same_path) {
            absent = false;
            break;
        }
    }
    free(entries);
    free(response);
    return absent;
}

static bool calculate_upload_chunk_crc(
    const revlink_accessport_map_upload_request_t *request,
    const uint8_t prefix[REVLINK_AP_CHUNK_PREFIX_SIZE],
    uint32_t *crc
)
{
    uint8_t buffer[REVLINK_UPLOAD_SOURCE_READ_BYTES];
    uint32_t received = 0U;
    revlink_ap_jamcrc_t state;
    revlink_ap_jamcrc_init(&state);
    revlink_ap_jamcrc_update(&state, prefix, REVLINK_AP_CHUNK_PREFIX_SIZE);
    while (received < request->size) {
        size_t count = 0U;
        const size_t wanted =
            request->size - received < sizeof(buffer)
            ? request->size - received
            : sizeof(buffer);
        if (configured_upload_source.read(
                configured_upload_source.context,
                buffer,
                wanted,
                &count
            ) != ESP_OK
            || count == 0U
            || count > wanted) {
            return false;
        }
        revlink_ap_jamcrc_update(&state, buffer, count);
        received += (uint32_t)count;
    }
    size_t extra = 0U;
    if (configured_upload_source.read(
            configured_upload_source.context,
            buffer,
            1U,
            &extra
        ) != ESP_OK
        || extra != 0U) {
        return false;
    }
    *crc = revlink_ap_jamcrc_finish_zeroed_trailer(&state);
    return configured_upload_source.rewind(
        configured_upload_source.context
    ) == ESP_OK;
}

static bool stream_upload_chunk(
    usb_transfer_t *out_transfer,
    usb_device_handle_t device,
    bounded_transfer_wait_t *wait,
    const revlink_accessport_map_upload_request_t *request,
    const uint8_t prefix[REVLINK_AP_CHUNK_PREFIX_SIZE],
    uint32_t crc
)
{
    uint8_t packet[REVLINK_ACCESSPORT_BULK_PACKET_BYTES];
    uint8_t trailer[REVLINK_AP_CHECKSUM_SIZE] = {
        (uint8_t)(crc >> 24U),
        (uint8_t)(crc >> 16U),
        (uint8_t)(crc >> 8U),
        (uint8_t)crc,
    };
    size_t prefix_offset = 0U;
    uint32_t payload_remaining = request->size;
    size_t trailer_offset = 0U;
    const int64_t deadline =
        esp_timer_get_time() + REVLINK_UPLOAD_TOTAL_DEADLINE_US;

    while (prefix_offset < REVLINK_AP_CHUNK_PREFIX_SIZE
        || payload_remaining > 0U
        || trailer_offset < sizeof(trailer)) {
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        size_t used = 0U;
        if (prefix_offset < REVLINK_AP_CHUNK_PREFIX_SIZE) {
            const size_t count =
                REVLINK_AP_CHUNK_PREFIX_SIZE - prefix_offset;
            memcpy(packet, prefix + prefix_offset, count);
            prefix_offset += count;
            used += count;
        }
        while (used < sizeof(packet) && payload_remaining > 0U) {
            size_t count = 0U;
            const size_t wanted =
                payload_remaining < sizeof(packet) - used
                ? payload_remaining
                : sizeof(packet) - used;
            if (configured_upload_source.read(
                    configured_upload_source.context,
                    packet + used,
                    wanted,
                    &count
                ) != ESP_OK
                || count == 0U
                || count > wanted) {
                return false;
            }
            used += count;
            payload_remaining -= (uint32_t)count;
        }
        if (payload_remaining == 0U
            && used < sizeof(packet)
            && trailer_offset < sizeof(trailer)) {
            const size_t count =
                sizeof(trailer) - trailer_offset < sizeof(packet) - used
                ? sizeof(trailer) - trailer_offset
                : sizeof(packet) - used;
            memcpy(packet + used, trailer + trailer_offset, count);
            trailer_offset += count;
            used += count;
        }
        if (!acceptance_out(
                out_transfer,
                device,
                wait,
                packet,
                used
            )) {
            return false;
        }
    }
    return true;
}

static void run_map_upload(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    accessport_usb_monitor_t *event_monitor,
    const revlink_device_identity_t *identity,
    bool *polite_disconnect_sent,
    const revlink_accessport_map_upload_request_t *request
)
{
    esp_err_t final_error = ESP_FAIL;
    bool source_open = false;
    bool interface_claimed = false;
    bool namespace_selected = false;
    bool write_phase_started = false;
    bool disconnect_sent = false;
    bool disconnect_acknowledged = false;
    SemaphoreHandle_t completion = NULL;
    usb_transfer_t *out_transfer = NULL;
    usb_transfer_t *in_transfer = NULL;
    bounded_transfer_wait_t wait = {0};
    revlink_ap_device_info_t true_identity = {0};
    uint8_t chunk_prefix[REVLINK_AP_CHUNK_PREFIX_SIZE] = {0};
    uint32_t chunk_crc = 0U;

    upload_observe(
        request,
        REVLINK_ACCESSPORT_UPLOAD_RUNNING,
        ESP_OK,
        false
    );
    if (!upload_source_configured
        || configured_upload_source.open == NULL
        || configured_upload_source.read == NULL
        || configured_upload_source.rewind == NULL
        || configured_upload_source.close == NULL
        || configured_upload_source.cached_file_matches == NULL
        || atomic_load(&write_recovery_required)) {
        final_error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    const size_t name_length = strnlen(
        request->name,
        sizeof(request->name)
    );
    const size_t path_length = strnlen(
        request->path,
        sizeof(request->path)
    );
    revlink_ap_upload_kind_t kind = REVLINK_AP_UPLOAD_MAP;
    if (name_length == 0U || name_length >= sizeof(request->name)
        || path_length == 0U || path_length >= sizeof(request->path)
        || revlink_ap_validate_upload_target(
               (const uint8_t *)request->path,
               path_length,
               request->size,
               &kind
           ) != REVLINK_AP_OK) {
        final_error = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (configured_upload_source.open(
            configured_upload_source.context,
            request
        ) != ESP_OK) {
        final_error = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    source_open = true;
    if (revlink_ap_build_chunk_prefix(
            request->size,
            REVLINK_AP_UPLOAD_CHUNK_FLAG,
            chunk_prefix
        ) != REVLINK_AP_OK
        || !calculate_upload_chunk_crc(request, chunk_prefix, &chunk_crc)) {
        final_error = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    final_error = usb_host_interface_claim(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE,
        0
    );
    if (final_error != ESP_OK) {
        goto cleanup;
    }
    interface_claimed = true;
    const revlink_device_event_t opened_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_OPENED,
        .identity = *identity,
    };
    publish_event(event_monitor, &opened_event);

    completion = xSemaphoreCreateBinary();
    wait.completion = completion;
    if (completion == NULL
        || usb_host_transfer_alloc(
               REVLINK_DOWNLOAD_REQUEST_CAPACITY,
               0,
               &out_transfer
           ) != ESP_OK
        || usb_host_transfer_alloc(
               REVLINK_DOWNLOAD_TRANSFER_CAPACITY,
               0,
               &in_transfer
           ) != ESP_OK) {
        final_error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (!read_true_device_identity(
            out_transfer,
            in_transfer,
            device,
            &wait,
            &true_identity
        )
        || strcmp(
               true_identity.part_number,
               request->expected_part_number
           ) != 0
        || strcmp(true_identity.serial, request->expected_serial) != 0) {
        final_error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    revlink_accessport_catalog_entry_t catalog = {0};
    if (!revlink_accessport_catalog_lookup(
            true_identity.part_number,
            &catalog
        )
        || !catalog.read_only_file_sync_supported) {
        final_error = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    if (configured_download_sink.select_device(
            configured_download_sink.context,
            &true_identity
        ) != ESP_OK) {
        final_error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    namespace_selected = true;
    if (kind == REVLINK_AP_UPLOAD_MAP
        && !map_destination_absent(
               out_transfer,
               in_transfer,
               device,
               &wait,
               request->path
           )) {
        final_error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    uint8_t upload_request[REVLINK_DOWNLOAD_REQUEST_CAPACITY];
    size_t upload_request_length = 0U;
    if (revlink_ap_build_upload(
            (const uint8_t *)request->name,
            name_length,
            (const uint8_t *)request->path,
            path_length,
            request->modification_time,
            request->size,
            upload_request,
            sizeof(upload_request),
            &upload_request_length
        ) != REVLINK_AP_OK
        || revlink_ap_validate_record(
               upload_request,
               upload_request_length
           ) != REVLINK_AP_OK
        || !acceptance_out(
               out_transfer,
               device,
               &wait,
               upload_request,
               upload_request_length
           )
        || !read_upload_ack(
               in_transfer,
               device,
               &wait,
               REVLINK_UPLOAD_READY_ACK
           )) {
        final_error = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    write_phase_started = true;
    if (!stream_upload_chunk(
            out_transfer,
            device,
            &wait,
            request,
            chunk_prefix,
            chunk_crc
        )
        || !read_upload_ack(
               in_transfer,
               device,
               &wait,
               REVLINK_UPLOAD_COMPLETE_ACK
           )) {
        final_error = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    const revlink_ap_listing_entry_t readback_entry = {
        .name = (const uint8_t *)request->name,
        .name_length = name_length,
        .device_time_raw = request->modification_time,
        .size = request->size,
        .is_directory = false,
        .path = (const uint8_t *)request->path,
        .path_length = path_length,
    };
    if (!download_file_entry(
            out_transfer,
            in_transfer,
            device,
            &wait,
            &readback_entry
        )) {
        final_error = ESP_FAIL;
        goto cleanup;
    }
    if (!configured_upload_source.cached_file_matches(
            configured_upload_source.context,
            request->path,
            request->size,
            request->source_sha256
        )) {
        final_error = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }
    final_error = ESP_OK;

cleanup:
    if (write_phase_started && final_error != ESP_OK) {
        atomic_store(&write_recovery_required, true);
    }
    if (out_transfer != NULL && interface_claimed) {
        disconnect_sent = send_polite_disconnect(
            out_transfer,
            in_transfer,
            device,
            &wait,
            &disconnect_acknowledged
        );
    }
    if (polite_disconnect_sent != NULL) {
        *polite_disconnect_sent = disconnect_sent;
    }
    if (in_transfer != NULL) {
        usb_host_transfer_free(in_transfer);
    }
    if (out_transfer != NULL) {
        usb_host_transfer_free(out_transfer);
    }
    if (completion != NULL) {
        vSemaphoreDelete(completion);
    }
    if (interface_claimed) {
        const esp_err_t release_status = usb_host_interface_release(
            client,
            device,
            REVLINK_ACCESSPORT_INTERFACE
        );
        if (final_error == ESP_OK && release_status != ESP_OK) {
            final_error = release_status;
        }
        const revlink_device_event_t closed_event = {
            .kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED,
            .identity = *identity,
        };
        publish_event(event_monitor, &closed_event);
    }
    if (namespace_selected) {
        configured_download_sink.release_device(
            configured_download_sink.context
        );
    }
    if (source_open) {
        configured_upload_source.close(
            configured_upload_source.context
        );
    }
    const bool recovery_required =
        atomic_load(&write_recovery_required);
    upload_observe(
        request,
        final_error == ESP_OK
            ? REVLINK_ACCESSPORT_UPLOAD_VERIFIED
            : REVLINK_ACCESSPORT_UPLOAD_FAILED,
        final_error,
        recovery_required
    );
    ESP_LOGI(
        TAG,
        "MAP WRITE %s: part=%s path=%s bytes=%" PRIu32
        " ready/completion/readback=%s recovery_required=%s",
        final_error == ESP_OK ? "VERIFIED" : "FAILED",
        request->expected_part_number,
        request->path,
        request->size,
        final_error == ESP_OK ? "passed" : "failed",
        recovery_required ? "yes" : "no"
    );
}
#endif

static void run_download_acceptance(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    accessport_usb_monitor_t *event_monitor,
    const revlink_device_identity_t *identity,
    bool *polite_disconnect_sent,
    bool report_runtime,
    bool close_recovery_attempt,
    bool identity_only
)
{
    if (!download_sink_configured
        || configured_download_sink.select_device == NULL
        || configured_download_sink.release_device == NULL) {
        ESP_LOGE(TAG, "READ-ONLY DOWNLOAD ACCEPTANCE FAILED: no cache sink");
        if (report_runtime) {
            const revlink_sync_event_t failed = {
                .kind = REVLINK_SYNC_EVENT_FAILED,
                .close_recovery_attempt = close_recovery_attempt,
                .platform_error = ESP_ERR_INVALID_STATE,
            };
            publish_sync_event(&failed);
        }
        return;
    }
#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
    if (configured_download_sink.is_current == NULL) {
        ESP_LOGE(
            TAG,
            "INCREMENTAL SYNC ACCEPTANCE FAILED: cache query unavailable"
        );
        if (report_runtime) {
            const revlink_sync_event_t failed = {
                .kind = REVLINK_SYNC_EVENT_FAILED,
                .close_recovery_attempt = close_recovery_attempt,
                .platform_error = ESP_ERR_INVALID_STATE,
            };
            publish_sync_event(&failed);
        }
        return;
    }
#endif

    if (report_runtime) {
        const revlink_sync_event_t started = {
            .kind = REVLINK_SYNC_EVENT_STARTED,
            .close_recovery_attempt = close_recovery_attempt,
        };
        publish_sync_event(&started);
    }

    ESP_LOGW(
        TAG,
        "READ-ONLY %s START: file_limit=%u, retries=0, "
        "AccessPort writes=disabled",
        identity_only
            ? "IDENTITY"
            :
#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
            "INCREMENTAL SYNC ACCEPTANCE",
#else
            "DOWNLOAD ACCEPTANCE",
#endif
        REVLINK_DOWNLOAD_MAX_FILE_BYTES
    );

    esp_err_t status = usb_host_interface_claim(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE,
        0
    );
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: interface claim=%s", esp_err_to_name(status));
        if (report_runtime) {
            const revlink_sync_event_t failed = {
                .kind = REVLINK_SYNC_EVENT_FAILED,
                .close_recovery_attempt = close_recovery_attempt,
                .platform_error = status,
            };
            publish_sync_event(&failed);
        }
        return;
    }
    const revlink_device_event_t opened_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_OPENED,
        .identity = *identity,
    };
    publish_event(event_monitor, &opened_event);

    SemaphoreHandle_t completion = xSemaphoreCreateBinary();
    usb_transfer_t *out_transfer = NULL;
    usb_transfer_t *in_transfer = NULL;
    revlink_ap_listing_entry_t *entries = NULL;
    const revlink_ap_listing_entry_t **candidates = NULL;
    uint8_t *listing_bytes = NULL;
    bounded_transfer_wait_t wait = {
        .completion = completion,
    };
    bool passed = false;
    bool cancelled = false;
    bool disconnect_acknowledged = false;
    bool disconnect_sent = false;
    size_t candidate_count = 0U;
    size_t downloaded = 0U;
    size_t skipped = 0U;
    uint32_t downloaded_bytes = 0U;

    if (completion == NULL
        || usb_host_transfer_alloc(
               REVLINK_DOWNLOAD_REQUEST_CAPACITY,
               0,
               &out_transfer
           ) != ESP_OK
        || usb_host_transfer_alloc(
               REVLINK_DOWNLOAD_TRANSFER_CAPACITY,
               0,
               &in_transfer
           ) != ESP_OK) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: bounded transfer allocation");
        goto cleanup;
    }
    revlink_ap_device_info_t true_identity = {0};
    if (sync_cancelled()) {
        cancelled = true;
        goto cleanup;
    }
    const bool identity_probe_succeeded = read_true_device_identity(
            out_transfer,
            in_transfer,
            device,
            &wait,
            &true_identity
        );
    if (
        identity_probe_succeeded
        && configured_identity_observer.observer != NULL
    ) {
        configured_identity_observer.observer(
            configured_identity_observer.context,
            &true_identity
        );
    }
    if (close_recovery_attempt) {
        ESP_LOGI(
            TAG,
            "CLOSE RECOVERY: initialized identity probe=%s; proceeding "
            "directly to acknowledged session close",
            identity_probe_succeeded ? "passed" : "stale-session-timeout"
        );
        goto cleanup;
    }
    if (!identity_probe_succeeded) {
        ESP_LOGE(
            TAG,
            "DOWNLOAD FAILED: authoritative device identity unavailable"
        );
        goto cleanup;
    }
    revlink_accessport_catalog_entry_t catalog_entry = {0};
    if (!revlink_accessport_catalog_lookup(
            true_identity.part_number,
            &catalog_entry
        )
        || !catalog_entry.read_only_file_sync_supported) {
        ESP_LOGE(
            TAG,
            "DEVICE SUPPORT BLOCKED: authoritative part=%s is not in "
            "catalog=%s; identity is visible but file operations are "
            "disabled",
            true_identity.part_number,
            REVLINK_ACCESSPORT_CATALOG_REVISION
        );
        goto cleanup;
    }
    ESP_LOGI(
        TAG,
        "SUPPORTED DEVICE IDENTIFIED: part=%s family=%s catalog=%s",
        true_identity.part_number,
        catalog_entry.family_name,
        REVLINK_ACCESSPORT_CATALOG_REVISION
    );
    if (configured_download_sink.select_device(
            configured_download_sink.context,
            &true_identity
        ) != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DOWNLOAD FAILED: authoritative device namespace unavailable"
        );
        goto cleanup;
    }
    if (identity_only) {
        passed = true;
        ESP_LOGI(
            TAG,
            "READ-ONLY IDENTITY PASSED: part=%s serial=%s firmware=%s",
            true_identity.part_number,
            true_identity.serial,
            true_identity.firmware
        );
        goto cleanup;
    }
    listing_bytes = malloc(REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY);
    if (listing_bytes == NULL) {
        ESP_LOGE(TAG, "DOWNLOAD FAILED: bounded listing buffer allocation");
        goto cleanup;
    }

    static const uint8_t datalog_directory[] = "datalog";
    static const uint8_t map_directory[] = "maps";
    static const uint8_t image_directory[] = "images";
    static const uint8_t screenshot_directory[] = "screenshots";
    static const uint8_t csv_extension[] = ".csv";
    static const uint8_t gzip_extension[] = ".csv.gz";
    static const uint8_t map_extension[] = ".ptm";
    static const uint8_t framebuffer_extension[] = ".fb";
    static const uint8_t png_extension[] = ".png";
    static const uint8_t bitmap_extension[] = ".bmp";
    static const revlink_read_collection_t collections[] = {
        {
            .directory = datalog_directory,
            .directory_length = sizeof(datalog_directory) - 1U,
            .label = "datalog",
            .extension = csv_extension,
            .extension_length = sizeof(csv_extension) - 1U,
            .alternate_extension = gzip_extension,
            .alternate_extension_length = sizeof(gzip_extension) - 1U,
        },
        {
            .directory = map_directory,
            .directory_length = sizeof(map_directory) - 1U,
            .label = "map",
            .extension = map_extension,
            .extension_length = sizeof(map_extension) - 1U,
        },
        {
            .directory = image_directory,
            .directory_length = sizeof(image_directory) - 1U,
            .label = "startup image",
            .extension = framebuffer_extension,
            .extension_length = sizeof(framebuffer_extension) - 1U,
        },
        {
            .directory = screenshot_directory,
            .directory_length = sizeof(screenshot_directory) - 1U,
            .label = "screenshot",
            .extension = png_extension,
            .extension_length = sizeof(png_extension) - 1U,
            .alternate_extension = bitmap_extension,
            .alternate_extension_length = sizeof(bitmap_extension) - 1U,
        },
    };
    uint8_t request[REVLINK_DOWNLOAD_REQUEST_CAPACITY];
    const size_t maximum_listing_reads =
        REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY
            / REVLINK_ACCESSPORT_BULK_PACKET_BYTES + 2U;
    for (
        size_t collection_index = 0U;
        collection_index < sizeof(collections) / sizeof(collections[0]);
        ++collection_index
    ) {
        const revlink_read_collection_t *collection =
            &collections[collection_index];
        size_t request_length = 0U;
        revlink_ap_status_t protocol_status = revlink_ap_build_list(
            collection->directory,
            collection->directory_length,
            request,
            sizeof(request),
            &request_length
        );
        if (protocol_status != REVLINK_AP_OK
            || !acceptance_out(
                   out_transfer,
                   device,
                   &wait,
                   request,
                   request_length
               )) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: %s listing OUT protocol=%s usb_status=%d",
                collection->label,
                revlink_ap_status_name(protocol_status),
                wait.status
            );
            goto cleanup;
        }

        size_t listing_length = 0U;
        size_t listing_target_length = 0U;
        for (
            size_t read_index = 0U;
            read_index < maximum_listing_reads;
            ++read_index
        ) {
            if (sync_cancelled()) {
                cancelled = true;
                goto cleanup;
            }
            if (!acceptance_in(in_transfer, device, &wait)) {
                ESP_LOGE(
                    TAG,
                    "DOWNLOAD FAILED: %s listing IN read=%u status=%d bytes=%d",
                    collection->label,
                    (unsigned int)(read_index + 1U),
                    wait.status,
                    wait.actual_num_bytes
                );
                goto cleanup;
            }
            const size_t received = (size_t)wait.actual_num_bytes;
            if (received
                > REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY
                    - listing_length) {
                ESP_LOGE(TAG, "DOWNLOAD FAILED: listing exceeded byte limit");
                goto cleanup;
            }
            memcpy(
                &listing_bytes[listing_length],
                in_transfer->data_buffer,
                received
            );
            listing_length += received;
            if (listing_target_length == 0U && listing_length >= 5U) {
                if (listing_bytes[0] != 0x02U
                    || listing_bytes[1] != 0x00U
                    || listing_bytes[2] != 0x00U) {
                    ESP_LOGE(
                        TAG,
                        "DOWNLOAD FAILED: listing prefix=%02x%02x%02x bytes=%u",
                        listing_bytes[0],
                        listing_bytes[1],
                        listing_bytes[2],
                        (unsigned int)listing_length
                    );
                    goto cleanup;
                }
                listing_target_length =
                    ((size_t)listing_bytes[3] << 8U)
                    | (size_t)listing_bytes[4];
                listing_target_length += 7U;
                if (listing_target_length
                        > REVLINK_DOWNLOAD_LISTING_RESPONSE_CAPACITY
                    || listing_target_length
                        < 7U + REVLINK_AP_CHECKSUM_SIZE) {
                    ESP_LOGE(
                        TAG,
                        "DOWNLOAD FAILED: declared listing length=%u",
                        (unsigned int)listing_target_length
                    );
                    goto cleanup;
                }
            }
            if (listing_target_length != 0U
                && listing_length >= listing_target_length) {
                break;
            }
        }
        if (listing_target_length == 0U
            || listing_length != listing_target_length) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: incomplete/extra %s listing bytes=%u declared=%u",
                collection->label,
                (unsigned int)listing_length,
                (unsigned int)listing_target_length
            );
            goto cleanup;
        }

        uint16_t plain_error_code = 0U;
        if (revlink_ap_plain_error_code(
                listing_bytes,
                listing_length,
                &plain_error_code
            )) {
            if (plain_error_code == 38U) {
                ESP_LOGI(
                    TAG,
                    "READ-ONLY %s collection is unavailable on this device",
                    collection->label
                );
                continue;
            }
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: %s listing rejected by device code=%u",
                collection->label,
                (unsigned int)plain_error_code
            );
            goto cleanup;
        }

        revlink_ap_record_view_t listing_record = {0};
        protocol_status = revlink_ap_parse_record(
            listing_bytes,
            listing_length,
            &listing_record
        );
        if (protocol_status != REVLINK_AP_OK
            || listing_record.opcode != REVLINK_AP_OPCODE_RESPONSE) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: %s listing record=%s opcode=0x%04x",
                collection->label,
                revlink_ap_status_name(protocol_status),
                listing_record.opcode
            );
            goto cleanup;
        }

        size_t entry_count = 0U;
        protocol_status = revlink_ap_parse_listing_payload(
            listing_record.payload,
            listing_record.payload_length,
            NULL,
            0U,
            &entry_count
        );
        if ((protocol_status != REVLINK_AP_OK
                && protocol_status != REVLINK_AP_BUFFER_TOO_SMALL)
            || entry_count > REVLINK_DOWNLOAD_LISTING_ENTRY_CAPACITY) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: %s listing count=%u parse=%s",
                collection->label,
                (unsigned int)entry_count,
                revlink_ap_status_name(protocol_status)
            );
            goto cleanup;
        }
        if (entry_count == 0U) {
            ESP_LOGI(
                TAG,
                "READ-ONLY %s collection is empty",
                collection->label
            );
            continue;
        }

        entries = calloc(entry_count, sizeof(*entries));
        if (entries == NULL) {
            ESP_LOGE(TAG, "DOWNLOAD FAILED: listing entry allocation");
            goto cleanup;
        }
        protocol_status = revlink_ap_parse_listing_payload(
            listing_record.payload,
            listing_record.payload_length,
            entries,
            entry_count,
            &entry_count
        );
        if (protocol_status != REVLINK_AP_OK) {
            ESP_LOGE(
                TAG,
                "DOWNLOAD FAILED: strict %s listing parse=%s",
                collection->label,
                revlink_ap_status_name(protocol_status)
            );
            goto cleanup;
        }

        candidates = calloc(entry_count, sizeof(*candidates));
        if (candidates == NULL) {
            ESP_LOGE(TAG, "DOWNLOAD FAILED: candidate allocation");
            goto cleanup;
        }
        size_t collection_candidate_count = 0U;
        for (size_t index = 0; index < entry_count; ++index) {
            if (safe_collection_entry(&entries[index], collection)) {
                candidates[collection_candidate_count++] = &entries[index];
            }
        }
        candidate_count += collection_candidate_count;
        for (size_t index = 1U;
             index < collection_candidate_count;
             ++index) {
            const revlink_ap_listing_entry_t *moving = candidates[index];
            size_t position = index;
            while (position > 0U
                && entry_precedes(moving, candidates[position - 1U])) {
                candidates[position] = candidates[position - 1U];
                --position;
            }
            candidates[position] = moving;
        }

        ESP_LOGI(
            TAG,
            "READ-ONLY %s collection: entries=%u safe_files=%u",
            collection->label,
            (unsigned int)entry_count,
            (unsigned int)collection_candidate_count
        );
        if (report_runtime) {
            const revlink_sync_event_t progress = {
                .kind = REVLINK_SYNC_EVENT_PROGRESS,
                .candidates = candidate_count,
                .downloaded = downloaded,
                .skipped = skipped,
                .downloaded_bytes = downloaded_bytes,
                .pending = candidate_count - downloaded - skipped,
            };
            publish_sync_event(&progress);
        }
        for (size_t index = 0U;
             index < collection_candidate_count;
             ++index) {
            const revlink_ap_listing_entry_t *candidate = candidates[index];
            if (sync_cancelled()) {
                cancelled = true;
                goto cleanup;
            }
#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
            if (configured_download_sink.is_current(
                    configured_download_sink.context,
                    candidate->path,
                    candidate->path_length,
                    candidate->device_time_raw,
                    candidate->size
                )) {
                ++skipped;
                if (report_runtime) {
                    const revlink_sync_event_t progress = {
                        .kind = REVLINK_SYNC_EVENT_PROGRESS,
                        .candidates = candidate_count,
                        .downloaded = downloaded,
                        .skipped = skipped,
                        .downloaded_bytes = downloaded_bytes,
                        .pending = candidate_count - downloaded - skipped,
                    };
                    publish_sync_event(&progress);
                }
                continue;
            }
            if (downloaded >= REVLINK_INCREMENTAL_MAX_DOWNLOADS
                || candidate->size
                    > REVLINK_INCREMENTAL_MAX_SESSION_BYTES
                        - downloaded_bytes) {
                break;
            }
#else
            if (downloaded >= 1U) {
                break;
            }
#endif
            ESP_LOGI(
                TAG,
                "Deterministic %s selected: name=%.*s bytes=%" PRIu32
                " device_time=%" PRIu32,
                collection->label,
                (int)candidate->name_length,
                (const char *)candidate->name,
                candidate->size,
                candidate->device_time_raw
            );
            if (!download_file_entry(
                    out_transfer,
                    in_transfer,
                    device,
                    &wait,
                    candidate
                )) {
                cancelled = sync_cancelled();
                goto cleanup;
            }
            ++downloaded;
            downloaded_bytes += candidate->size;
            if (report_runtime) {
                const revlink_sync_event_t progress = {
                    .kind = REVLINK_SYNC_EVENT_PROGRESS,
                    .candidates = candidate_count,
                    .downloaded = downloaded,
                    .skipped = skipped,
                    .downloaded_bytes = downloaded_bytes,
                    .pending = candidate_count - downloaded - skipped,
                };
                publish_sync_event(&progress);
            }
        }

        free(candidates);
        candidates = NULL;
        free(entries);
        entries = NULL;
    }

    passed = true;
    ESP_LOGI(
        TAG,
        "READ-ONLY %s PASSED: candidates=%u downloaded=%u skipped=%u "
        "bytes=%" PRIu32 " pending=%u",
#if CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_RUNTIME_SYNC
        "INCREMENTAL SYNC",
#else
        "DOWNLOAD ACCEPTANCE",
#endif
        (unsigned int)candidate_count,
        (unsigned int)downloaded,
        (unsigned int)skipped,
        downloaded_bytes,
        (unsigned int)(candidate_count - downloaded - skipped)
    );

cleanup:
    /*
     * A cancellation can arrive while a bounded USB transaction is waiting.
     * Those helpers intentionally return false so the worker can unwind, but
     * the local flag may not have been set by the phase-specific call site.
     * Snapshot the shared request before closing the session so a clean
     * cancellation is reported as cancelled instead of as a transport failure.
     */
    cancelled = cancelled || sync_cancelled();
#if CONFIG_REVLINK_USB_CLOSE_RECOVERY_ACCEPTANCE
    const bool inject_unclean_close =
        report_runtime
        && passed
        && !close_recovery_attempt
        && !atomic_exchange(&close_fault_injected, true);
    if (inject_unclean_close) {
        ESP_LOGW(
            TAG,
            "CLOSE-RECOVERY ACCEPTANCE: deliberately omitting the first "
            "0x05 close after a completed read-only data phase"
        );
    } else
#endif
    {
        disconnect_sent = send_polite_disconnect(
            out_transfer,
            in_transfer,
            device,
            &wait,
            &disconnect_acknowledged
        );
    }
    if (polite_disconnect_sent != NULL) {
        *polite_disconnect_sent = disconnect_sent;
    }
    free(candidates);
    free(entries);
    free(listing_bytes);
    if (in_transfer != NULL) {
        usb_host_transfer_free(in_transfer);
    }
    if (out_transfer != NULL) {
        usb_host_transfer_free(out_transfer);
    }
    if (completion != NULL) {
        vSemaphoreDelete(completion);
    }
    status = usb_host_interface_release(
        client,
        device,
        REVLINK_ACCESSPORT_INTERFACE
    );
    const bool session_closed_cleanly =
        disconnect_sent
        && disconnect_acknowledged
        && status == ESP_OK;
    ESP_LOGI(
        TAG,
        "READ-ONLY DOWNLOAD SESSION CLOSED: interface_release=%s "
        "close_sent=%s ack_0x35=%s result=%s",
        esp_err_to_name(status),
        disconnect_sent ? "yes" : "no",
        disconnect_acknowledged ? "yes" : "no",
        (passed || close_recovery_attempt) && session_closed_cleanly
            ? "passed" : "failed"
    );
    const revlink_device_event_t closed_event = {
        .kind = REVLINK_DEVICE_EVENT_SESSION_CLOSED,
        .identity = *identity,
    };
    publish_event(event_monitor, &closed_event);
    configured_download_sink.release_device(
        configured_download_sink.context
    );
    if (report_runtime) {
        const bool completed =
            (passed || close_recovery_attempt) && session_closed_cleanly;
        const bool cancelled_cleanly = cancelled && session_closed_cleanly;
        const revlink_sync_event_t finished = {
            .kind = cancelled_cleanly
                ? REVLINK_SYNC_EVENT_CANCELLED
                : (completed
                    ? REVLINK_SYNC_EVENT_COMPLETED
                    : REVLINK_SYNC_EVENT_FAILED),
            .candidates = candidate_count,
            .downloaded = downloaded,
            .skipped = skipped,
            .downloaded_bytes = downloaded_bytes,
            .pending = candidate_count >= downloaded + skipped
                ? candidate_count - downloaded - skipped
                : 0U,
            .close_recovery_attempt = close_recovery_attempt,
            .data_phase_completed = passed,
            .session_close_sent = disconnect_sent,
            .session_close_acknowledged = disconnect_acknowledged,
            .platform_error = completed || cancelled_cleanly
                ? ESP_OK
                : (status != ESP_OK
                    ? status
                    : (disconnect_sent && !disconnect_acknowledged
                        ? ESP_ERR_INVALID_RESPONSE
                        : ESP_FAIL)),
        };
        publish_sync_event(&finished);
    }
}
#endif

static void log_open_device(
    enumeration_client_t *state,
    enumerated_device_t *device
)
{
    usb_device_info_t info = {0};
    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;

    esp_err_t err = usb_host_device_info(device->handle, &info);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "device address=%u info failed: %s",
            device->address,
            esp_err_to_name(err)
        );
        return;
    }

    err = usb_host_get_device_descriptor(device->handle, &device_desc);
    if (err != ESP_OK || device_desc == NULL) {
        ESP_LOGE(
            TAG,
            "device address=%u descriptor failed: %s",
            device->address,
            esp_err_to_name(err)
        );
        return;
    }

    const bool is_accessport =
        device_desc->idVendor == REVLINK_ACCESSPORT_VID
        && device_desc->idProduct == REVLINK_ACCESSPORT_PID;
    device->is_accessport = is_accessport;

    ESP_LOGI(
        TAG,
        "device address=%u speed=%s parent_port=%s configuration=%u "
        "vid=0x%04x pid=0x%04x class=0x%02x max_packet0=%u configs=%u%s",
        device->address,
        speed_name(info.speed),
        info.parent.dev_hdl == NULL ? "root" : "external-hub",
        info.bConfigurationValue,
        device_desc->idVendor,
        device_desc->idProduct,
        device_desc->bDeviceClass,
        device_desc->bMaxPacketSize0,
        device_desc->bNumConfigurations,
        is_accessport ? " ACCESSPORT" : ""
    );

    if (info.str_desc_manufacturer != NULL) {
        ESP_LOGI(TAG, "manufacturer string:");
        usb_print_string_descriptor(info.str_desc_manufacturer);
    }
    if (info.str_desc_product != NULL) {
        ESP_LOGI(TAG, "product string:");
        usb_print_string_descriptor(info.str_desc_product);
    }
    if (info.str_desc_serial_num != NULL) {
        ESP_LOGI(TAG, "serial string:");
        usb_print_string_descriptor(info.str_desc_serial_num);
    }

    if (!is_accessport) {
        err = usb_host_get_active_config_descriptor(device->handle, &config_desc);
        if (err != ESP_OK || config_desc == NULL) {
            ESP_LOGE(
                TAG,
                "device address=%u active configuration failed: %s",
                device->address,
                esp_err_to_name(err)
            );
            return;
        }
        inspect_config_descriptor(config_desc, 1, false, NULL);
        return;
    }

    device->identity = (revlink_device_identity_t){
        .vendor_id = device_desc->idVendor,
        .product_id = device_desc->idProduct,
        .address = device->address,
        .configuration_count = device_desc->bNumConfigurations,
        .interface_number = REVLINK_ACCESSPORT_INTERFACE,
        .bulk_out_endpoint = REVLINK_ACCESSPORT_BULK_OUT,
        .bulk_in_endpoint = REVLINK_ACCESSPORT_BULK_IN,
        .attachment_generation = device->attachment_generation,
        .high_speed = info.speed == USB_SPEED_HIGH,
    };
    bool software_reenumeration = false;
    if (state->expect_polite_accessport_reenumeration) {
        software_reenumeration =
            esp_timer_get_time() <= state->polite_reenumeration_deadline_us;
        ESP_LOGI(
            TAG,
            "AccessPort attach classified as %s",
            software_reenumeration
                ? "polite software re-enumeration"
                : "new physical attachment (polite window expired)"
        );
        state->expect_polite_accessport_reenumeration = false;
        state->polite_reenumeration_deadline_us = 0;
    }
    device->software_reenumeration = software_reenumeration;

    unsigned int matching_configurations = 0;
    ESP_LOGI(
        TAG,
        "AccessPort has %u configurations; validating active configuration=%u",
        device_desc->bNumConfigurations,
        info.bConfigurationValue
    );
    const usb_config_desc_t *active_config = NULL;
    err = usb_host_get_active_config_descriptor(
        device->handle,
        &active_config
    );
    if (err == ESP_OK && active_config != NULL) {
        uint16_t bulk_max_packet_size = 0;
        if (
            inspect_config_descriptor(
                active_config,
                info.bConfigurationValue,
                true,
                &bulk_max_packet_size
            )
        ) {
            ++matching_configurations;
            device->identity.bulk_max_packet_size = bulk_max_packet_size;
        }
    } else {
        ESP_LOGE(
            TAG,
            "active configuration validation failed: %s",
            esp_err_to_name(err)
        );
    }

    ESP_LOGI(
        TAG,
        "ACCESSPORT ACTIVE CONFIGURATION VALIDATION COMPLETE: matches=%u",
        matching_configurations
    );
    device->identity.matching_configuration_count =
        (uint8_t)matching_configurations;

    if (matching_configurations == 1U) {
        device->eligible_accessport = true;
        const uint32_t topology_revision =
            advance_topology_revision(state);
        const uint8_t eligible_count = bounded_eligible_count(state);
        if (eligible_count >= 2U || state->conflict_latched) {
            publish_multiple_accessports(state);
            return;
        }
        const revlink_device_event_t attached_event = {
            .kind = REVLINK_DEVICE_EVENT_ATTACHED,
            .identity = device->identity,
            .eligible_device_count = 1U,
            .topology_revision = topology_revision,
            .software_reenumeration = software_reenumeration,
        };
        publish_event(state->monitor, &attached_event);
        const revlink_device_event_t accepted_event = {
            .kind = REVLINK_DEVICE_EVENT_ACCEPTED,
            .identity = device->identity,
            .eligible_device_count = 1U,
            .topology_revision = topology_revision,
            .software_reenumeration = software_reenumeration,
        };
        publish_event(state->monitor, &accepted_event);
#if CONFIG_REVLINK_USB_INTERFACE_CLAIM_ACCEPTANCE \
    || CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE \
    || CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_USB_SESSION_CLOSE_ACCEPTANCE
        if (device->software_reenumeration) {
            ESP_LOGI(
                TAG,
                "Attachment-scoped acceptance transaction skipped after "
                "polite software re-enumeration"
            );
            return;
        }
        if (device->acceptance_attempted) {
            ESP_LOGW(
                TAG,
                "Attachment-scoped transaction skipped: already attempted"
            );
            return;
        }
        device->acceptance_attempted = true;
#endif
#if CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE \
    || CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE \
    || CONFIG_REVLINK_USB_SESSION_CLOSE_ACCEPTANCE
        wait_for_accessport_transaction_readiness(device);
#endif
#if CONFIG_REVLINK_USB_INTERFACE_CLAIM_ACCEPTANCE
        run_interface_claim_acceptance(
            state->client,
            device->handle,
            state->monitor,
            &device->identity
        );
#endif
#if CONFIG_REVLINK_USB_SESSION_CLOSE_ACCEPTANCE
        run_session_close_acceptance(
            state->client,
            device->handle,
            state->monitor,
            &device->identity,
            &device->polite_disconnect_sent
        );
#endif
#if CONFIG_REVLINK_USB_ROOT_LIST_ACCEPTANCE
        run_root_list_acceptance(
            state->client,
            device->handle,
            state->monitor,
            &device->identity
        );
#endif
#if CONFIG_REVLINK_USB_DOWNLOAD_ACCEPTANCE \
    || CONFIG_REVLINK_USB_INCREMENTAL_SYNC_ACCEPTANCE
        run_download_acceptance(
            state->client,
            device->handle,
            state->monitor,
            &device->identity,
            &device->polite_disconnect_sent,
            false,
            false,
            false
        );
#endif
    } else {
        ESP_LOGE(
            TAG,
            "ACCESSPORT INTERFACE ACCEPTANCE SKIPPED: descriptor matches=%u "
            "expected=1 active configuration (device advertises %u total)",
            matching_configurations,
            device_desc->bNumConfigurations
        );
        if (eligible_accessport_count(state) == 0U
            && !state->conflict_latched) {
            const revlink_device_event_t attached_event = {
                .kind = REVLINK_DEVICE_EVENT_ATTACHED,
                .identity = device->identity,
                .software_reenumeration = software_reenumeration,
            };
            publish_event(state->monitor, &attached_event);
            const revlink_device_event_t failure_event = {
                .kind = REVLINK_DEVICE_EVENT_FAILURE,
                .identity = device->identity,
                .platform_error = ESP_ERR_INVALID_RESPONSE,
            };
            publish_event(state->monitor, &failure_event);
        }
    }
}

static void usb_descriptor_task(void *arg)
{
    enumeration_client_t *state = (enumeration_client_t *)arg;

    while (true) {
        descriptor_work_t work = {0};
        if (
            xQueueReceive(
                state->scan_requests,
                &work,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            continue;
        }

        log_open_device(state, work.device);
        if (
            xQueueSend(
                state->scan_results,
                &work,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            ESP_LOGE(TAG, "failed to report descriptor scan completion");
        }
    }
}

static void usb_transaction_task(void *arg)
{
    enumeration_client_t *state = (enumeration_client_t *)arg;

    while (true) {
        descriptor_work_t work = {0};
        if (
            xQueueReceive(
                state->transaction_requests,
                &work,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            continue;
        }

        if (work.device->handle != work.pinned_handle
            || work.device->attachment_generation
                != work.attachment_generation
            || !work.device->eligible_accessport
            || state->conflict_latched
            || eligible_accessport_count(state) != 1U) {
            ESP_LOGE(
                TAG,
                "refusing stale or conflicted USB transaction: "
                "generation=%" PRIu32,
                work.attachment_generation
            );
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            if (work.kind == DESCRIPTOR_WORK_MAP_UPLOAD) {
                upload_observe(
                    &work.upload,
                    REVLINK_ACCESSPORT_UPLOAD_FAILED,
                    ESP_ERR_INVALID_STATE,
                    atomic_load(&write_recovery_required)
                );
            } else
#endif
#if CONFIG_REVLINK_RUNTIME_SYNC
            if (work.kind != DESCRIPTOR_WORK_IDENTITY) {
                const revlink_sync_event_t failed = {
                    .kind = REVLINK_SYNC_EVENT_FAILED,
                    .close_recovery_attempt =
                        work.kind == DESCRIPTOR_WORK_CLOSE_RECOVERY,
                    .platform_error = ESP_ERR_INVALID_STATE,
                };
                publish_sync_event(&failed);
            }
#endif
        } else {
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            if (work.kind == DESCRIPTOR_WORK_MAP_UPLOAD) {
                wait_for_accessport_transaction_readiness(work.device);
                run_map_upload(
                    state->client,
                    work.pinned_handle,
                    state->monitor,
                    &work.device->identity,
                    &work.device->polite_disconnect_sent,
                    &work.upload
                );
            } else
#endif
#if CONFIG_REVLINK_RUNTIME_SYNC
            {
            wait_for_accessport_transaction_readiness(work.device);
            run_download_acceptance(
                state->client,
                work.pinned_handle,
                state->monitor,
                &work.device->identity,
                &work.device->polite_disconnect_sent,
                work.kind != DESCRIPTOR_WORK_IDENTITY,
                work.kind == DESCRIPTOR_WORK_CLOSE_RECOVERY,
                work.kind == DESCRIPTOR_WORK_IDENTITY
            );
            }
#endif
        }
        if (
            xQueueSend(
                state->transaction_results,
                &work,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            ESP_LOGE(TAG, "failed to report transaction completion");
        }
    }
}

static void collect_scan_results(enumeration_client_t *state)
{
    descriptor_work_t work = {0};
    while (
        xQueueReceive(
            state->scan_results,
            &work,
            0
        ) == pdTRUE
    ) {
        if (work.device->attachment_generation
            == work.attachment_generation) {
            work.device->scan_in_progress = false;
        }
    }
    while (
        xQueueReceive(
            state->transaction_results,
            &work,
            0
        ) == pdTRUE
    ) {
        if (work.device->attachment_generation
            == work.attachment_generation) {
            work.device->scan_in_progress = false;
        }
    }
}

static void process_control_requests(enumeration_client_t *state)
{
#if CONFIG_REVLINK_RUNTIME_SYNC
    usb_control_request_t request = {
        .kind = CONTROL_REQUEST_IDENTITY,
    };
    while (xQueueReceive(control_requests, &request, 0) == pdTRUE) {
        if (request.kind == CONTROL_CANCEL_SYNC) {
            atomic_store(&sync_cancel_requested, true);
            continue;
        }

        enumerated_device_t *selected = NULL;
        const size_t eligible_count = eligible_accessport_count(state);
        if (state->conflict_latched || eligible_count != 1U) {
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            if (request.kind == CONTROL_REQUEST_MAP_UPLOAD) {
                upload_observe(
                    &request.upload,
                    REVLINK_ACCESSPORT_UPLOAD_FAILED,
                    ESP_ERR_INVALID_STATE,
                    atomic_load(&write_recovery_required)
                );
            } else
#endif
            if (request.kind != CONTROL_REQUEST_IDENTITY) {
                const revlink_sync_event_t failed = {
                    .kind = REVLINK_SYNC_EVENT_FAILED,
                    .close_recovery_attempt =
                        request.kind == CONTROL_REQUEST_CLOSE_RECOVERY,
                    .platform_error = ESP_ERR_INVALID_STATE,
                };
                publish_sync_event(&failed);
            }
            continue;
        }
        for (size_t i = 1; i <= REVLINK_USB_MAX_DEVICE_ADDRESS; ++i) {
            enumerated_device_t *device = &state->devices[i];
            if (device->handle != NULL && device->eligible_accessport
                && !device->scan_in_progress
                && device->action != DEVICE_ACTION_CLOSE
                && device->identity.configuration_count != 0U
                && device->identity.matching_configuration_count == 1U) {
                selected = device;
                break;
            }
        }
        if (selected == NULL) {
            bool accepted_device_busy = false;
            if (request.kind == CONTROL_REQUEST_CLOSE_RECOVERY
                || request.kind == CONTROL_REQUEST_SYNC
                || request.kind == CONTROL_REQUEST_MAP_UPLOAD) {
                for (
                    size_t i = 1;
                    i <= REVLINK_USB_MAX_DEVICE_ADDRESS;
                    ++i
                ) {
                    const enumerated_device_t *device = &state->devices[i];
                    if (device->handle != NULL && device->is_accessport
                        && device->scan_in_progress
                        && device->action != DEVICE_ACTION_CLOSE) {
                        accepted_device_busy = true;
                        break;
                    }
                }
            }
            if (accepted_device_busy) {
                ESP_LOGI(
                    TAG,
                    "deferring %s until the current transaction releases "
                    "the accepted device",
                    request.kind == CONTROL_REQUEST_CLOSE_RECOVERY
                        ? "close recovery"
                        : request.kind == CONTROL_REQUEST_MAP_UPLOAD
                            ? "map upload" : "sync continuation"
                );
                if (xQueueSend(control_requests, &request, 0) != pdTRUE) {
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
                    if (request.kind == CONTROL_REQUEST_MAP_UPLOAD) {
                        upload_observe(
                            &request.upload,
                            REVLINK_ACCESSPORT_UPLOAD_FAILED,
                            ESP_ERR_TIMEOUT,
                            atomic_load(&write_recovery_required)
                        );
                    } else
#endif
                    {
                    const revlink_sync_event_t failed = {
                        .kind = REVLINK_SYNC_EVENT_FAILED,
                        .close_recovery_attempt =
                            request.kind == CONTROL_REQUEST_CLOSE_RECOVERY,
                        .platform_error = ESP_ERR_TIMEOUT,
                    };
                    publish_sync_event(&failed);
                    }
                }
                break;
            }
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            if (request.kind == CONTROL_REQUEST_MAP_UPLOAD) {
                upload_observe(
                    &request.upload,
                    REVLINK_ACCESSPORT_UPLOAD_FAILED,
                    ESP_ERR_INVALID_STATE,
                    atomic_load(&write_recovery_required)
                );
            } else
#endif
            if (request.kind != CONTROL_REQUEST_IDENTITY) {
                const revlink_sync_event_t failed = {
                    .kind = REVLINK_SYNC_EVENT_FAILED,
                    .close_recovery_attempt =
                        request.kind == CONTROL_REQUEST_CLOSE_RECOVERY,
                    .platform_error = ESP_ERR_INVALID_STATE,
                };
                publish_sync_event(&failed);
            }
            continue;
        }

        if (request.kind != CONTROL_REQUEST_MAP_UPLOAD) {
            atomic_store(&sync_cancel_requested, false);
        }
        selected->scan_in_progress = true;
        const descriptor_work_t work = {
            .kind = request.kind == CONTROL_REQUEST_MAP_UPLOAD
                ? DESCRIPTOR_WORK_MAP_UPLOAD
                : (request.kind == CONTROL_REQUEST_CLOSE_RECOVERY
                ? DESCRIPTOR_WORK_CLOSE_RECOVERY
                : (request.kind == CONTROL_REQUEST_IDENTITY
                    ? DESCRIPTOR_WORK_IDENTITY
                    : DESCRIPTOR_WORK_SYNC)),
            .device = selected,
            .pinned_handle = selected->handle,
            .attachment_generation = selected->attachment_generation,
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            .upload = request.upload,
#endif
        };
        if (xQueueSend(
                state->transaction_requests,
                &work,
                pdMS_TO_TICKS(100)
            ) != pdTRUE) {
            selected->scan_in_progress = false;
            if (request.kind == CONTROL_REQUEST_MAP_UPLOAD) {
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
                upload_observe(
                    &request.upload,
                    REVLINK_ACCESSPORT_UPLOAD_FAILED,
                    ESP_ERR_TIMEOUT,
                    atomic_load(&write_recovery_required)
                );
#endif
            } else if (request.kind != CONTROL_REQUEST_IDENTITY) {
                const revlink_sync_event_t failed = {
                    .kind = REVLINK_SYNC_EVENT_FAILED,
                    .close_recovery_attempt =
                        request.kind == CONTROL_REQUEST_CLOSE_RECOVERY,
                    .platform_error = ESP_ERR_TIMEOUT,
                };
                publish_sync_event(&failed);
            }
        }
    }
#else
    (void)state;
#endif
}

static void client_event_callback(
    const usb_host_client_event_msg_t *event,
    void *callback_arg
)
{
    enumeration_client_t *state = (enumeration_client_t *)callback_arg;

    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        const uint8_t address = event->new_dev.address;
        if (address > REVLINK_USB_MAX_DEVICE_ADDRESS) {
            ESP_LOGE(TAG, "invalid USB address=%u", address);
            return;
        }
        enumerated_device_t *device = &state->devices[address];
        memset(device, 0, sizeof(*device));
        device->address = address;
        ++state->next_attachment_generation;
        if (state->next_attachment_generation == 0U) {
            ++state->next_attachment_generation;
        }
        device->attachment_generation =
            state->next_attachment_generation;
        device->action = DEVICE_ACTION_OPEN;
        state->physical_detach_recovery_pending = false;
        state->physical_detach_recovery_deadline_us = 0;
        return;
    }

    if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (size_t i = 1; i <= REVLINK_USB_MAX_DEVICE_ADDRESS; ++i) {
            if (state->devices[i].handle == event->dev_gone.dev_hdl) {
                state->devices[i].action = DEVICE_ACTION_CLOSE;
                return;
            }
        }
        ESP_LOGW(TAG, "disconnect event for an unknown device handle");
        return;
    }

    ESP_LOGW(TAG, "unhandled USB client event=%d", event->event);
}

static void inspect_pending_devices(enumeration_client_t *state)
{
    for (size_t i = 1; i <= REVLINK_USB_MAX_DEVICE_ADDRESS; ++i) {
        enumerated_device_t *device = &state->devices[i];
        const device_action_t action = device->action;

        if (action == DEVICE_ACTION_OPEN) {
            device->action = DEVICE_ACTION_NONE;
            esp_err_t err = usb_host_device_open(
                state->client,
                device->address,
                &device->handle
            );
            if (err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "open device address=%u failed: %s",
                    device->address,
                    esp_err_to_name(err)
                );
                device->handle = NULL;
                continue;
            }
            device->scan_in_progress = true;
            device->transaction_ready_after_us =
                esp_timer_get_time()
                + REVLINK_ACCESSPORT_TRANSACTION_READY_DELAY_US;
            const descriptor_work_t work = {
                .kind = DESCRIPTOR_WORK_SCAN,
                .device = device,
                .pinned_handle = device->handle,
                .attachment_generation = device->attachment_generation,
            };
            if (
                xQueueSend(
                    state->scan_requests,
                    &work,
                    pdMS_TO_TICKS(100)
                ) != pdTRUE
            ) {
                ESP_LOGE(
                    TAG,
                    "queue descriptor scan address=%u failed",
                    device->address
                );
                device->scan_in_progress = false;
            }
        } else if (
            action == DEVICE_ACTION_CLOSE
            && device->handle != NULL
            && !device->scan_in_progress
        ) {
            device->action = DEVICE_ACTION_NONE;
            device->eligible_accessport = false;
            const esp_err_t err = usb_host_device_close(
                state->client,
                device->handle
            );
            ESP_LOGI(
                TAG,
                "device address=%u disconnected close=%s",
                device->address,
                esp_err_to_name(err)
            );
            if (device->is_accessport) {
                const uint32_t topology_revision =
                    advance_topology_revision(state);
                if (device->polite_disconnect_sent) {
                    state->expect_polite_accessport_reenumeration = true;
                    state->polite_reenumeration_deadline_us =
                        esp_timer_get_time()
                        + REVLINK_POLITE_REENUMERATION_WINDOW_US;
                }
                const revlink_device_event_t detached_event = {
                    .kind = REVLINK_DEVICE_EVENT_DETACHED,
                    .identity = device->identity,
                    .eligible_device_count =
                        bounded_eligible_count(state),
                    .topology_revision = topology_revision,
                    .software_reenumeration =
                        device->polite_disconnect_sent,
                };
                publish_event(state->monitor, &detached_event);
                if (!device->polite_disconnect_sent
                    && eligible_accessport_count(state) == 0U) {
                    state->physical_detach_recovery_pending = true;
                    state->physical_detach_recovery_deadline_us =
                        esp_timer_get_time()
                        + (int64_t)
                            REVLINK_PHYSICAL_DETACH_RECOVERY_DELAY_MS
                            * 1000LL;
                }
            }
            if (state->conflict_latched
                && eligible_accessport_count(state) == 0U) {
                state->conflict_latched = false;
                ESP_LOGW(
                    TAG,
                    "multiple-device conflict cleared after complete "
                    "zero-device detach; automatic sync remains disarmed"
                );
            }
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
            if (!device->polite_disconnect_sent
                && eligible_accessport_count(state) == 0U
                && atomic_exchange(
                       &write_recovery_required,
                       false
                   )) {
                ESP_LOGI(
                    TAG,
                    "map-write recovery latch cleared after complete "
                    "AccessPort detach"
                );
            }
#endif
            memset(device, 0, sizeof(*device));
        }
    }
}

static void usb_client_task(void *arg)
{
    enumeration_client_t *state = calloc(1, sizeof(*state));
    ESP_ERROR_CHECK(state != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    state->monitor = (accessport_usb_monitor_t *)arg;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 16,
        .async = {
            .client_event_callback = client_event_callback,
            .callback_arg = state,
        },
    };

    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &state->client));
    /*
     * Do not schedule detach recovery merely because the monitor started
     * without an AccessPort. Some supported P4 boards expose an onboard USB
     * hub whose own enumeration can still be settling at this point. Cycling
     * the logical root port during that release window violates the hub
     * driver's lifecycle and can assert. A real, previously identified
     * AccessPort detach schedules the bounded recovery below.
     */
    state->physical_detach_recovery_pending = false;
    state->physical_detach_recovery_deadline_us = 0;
    state->scan_requests = xQueueCreate(
        REVLINK_USB_SCAN_QUEUE_DEPTH,
        sizeof(descriptor_work_t)
    );
    state->scan_results = xQueueCreate(
        REVLINK_USB_SCAN_QUEUE_DEPTH,
        sizeof(descriptor_work_t)
    );
    state->transaction_requests = xQueueCreate(
        REVLINK_USB_SCAN_QUEUE_DEPTH,
        sizeof(descriptor_work_t)
    );
    state->transaction_results = xQueueCreate(
        REVLINK_USB_SCAN_QUEUE_DEPTH,
        sizeof(descriptor_work_t)
    );
    ESP_ERROR_CHECK(
        state->scan_requests != NULL && state->scan_results != NULL
            && state->transaction_requests != NULL
            && state->transaction_results != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM
    );

    const BaseType_t descriptor_created = xTaskCreatePinnedToCore(
        usb_descriptor_task,
        "revlink_usb_desc",
        REVLINK_USB_DESCRIPTOR_TASK_STACK_SIZE,
        state,
        REVLINK_USB_DESCRIPTOR_TASK_PRIORITY,
        NULL,
        0
    );
    ESP_ERROR_CHECK(
        descriptor_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM
    );
    const BaseType_t transaction_created = xTaskCreatePinnedToCore(
        usb_transaction_task,
        "revlink_usb_xfer",
        REVLINK_USB_TRANSACTION_TASK_STACK_SIZE,
        state,
        REVLINK_USB_DESCRIPTOR_TASK_PRIORITY,
        NULL,
        0
    );
    ESP_ERROR_CHECK(
        transaction_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM
    );
    ESP_LOGI(TAG, "descriptor-only USB client registered");

    while (true) {
        const esp_err_t err = usb_host_client_handle_events(
            state->client,
            pdMS_TO_TICKS(100)
        );
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "client event handling failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        collect_scan_results(state);
        process_control_requests(state);
        inspect_pending_devices(state);
        recover_root_port_after_physical_detach(state);
    }
}

static void usb_host_task(void *arg)
{
    const host_task_startup_t startup = *(host_task_startup_t *)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = REVLINK_USB_ROOT_PORT_UNPOWERED,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = BIT0,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
#if CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGI(TAG, "USB peripheral 0 selected: ESP32-P4 high-speed OTG 2.0");
#endif
    ESP_LOGI(
        TAG,
        "USB host installed; root-port power=%s; external hub enabled",
        REVLINK_USB_ROOT_PORT_POWER_TEXT
    );
    const revlink_device_event_t started_event = {
        .kind = REVLINK_DEVICE_EVENT_MONITOR_STARTED,
    };
    publish_event(startup.monitor, &started_event);
    xTaskNotifyGive(startup.startup_task);

    while (true) {
        uint32_t event_flags = 0;
        const esp_err_t err = usb_host_lib_handle_events(
            portMAX_DELAY,
            &event_flags
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "host event handling failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t revlink_accessport_usb_start(
    revlink_accessport_usb_observer_t observer,
    void *observer_context
)
{
    if (monitor_started) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_REVLINK_RUNTIME_SYNC
    if (!download_sink_configured
        || configured_download_sink.is_current == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    control_requests = xQueueCreate(
        REVLINK_USB_SCAN_QUEUE_DEPTH,
        sizeof(usb_control_request_t)
    );
    if (control_requests == NULL) {
        return ESP_ERR_NO_MEM;
    }
#endif
    monitor = (accessport_usb_monitor_t){
        .observer = observer,
        .observer_context = observer_context,
    };
    monitor_started = true;

    TaskHandle_t host_task = NULL;
    host_task_startup_t startup = {
        .startup_task = xTaskGetCurrentTaskHandle(),
        .monitor = &monitor,
    };
    const BaseType_t host_created = xTaskCreatePinnedToCore(
        usb_host_task,
        "revlink_usb_host",
        REVLINK_USB_HOST_TASK_STACK_SIZE,
        &startup,
        REVLINK_USB_HOST_TASK_PRIORITY,
        &host_task,
        0
    );
    if (host_created != pdPASS) {
#if CONFIG_REVLINK_RUNTIME_SYNC
        vQueueDelete(control_requests);
        control_requests = NULL;
#endif
        monitor_started = false;
        return ESP_ERR_NO_MEM;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000)) == 0) {
        ESP_LOGE(TAG, "timed out waiting for USB host installation");
#if CONFIG_REVLINK_RUNTIME_SYNC
        vQueueDelete(control_requests);
        control_requests = NULL;
#endif
        monitor_started = false;
        return ESP_ERR_TIMEOUT;
    }

    const BaseType_t client_created = xTaskCreatePinnedToCore(
        usb_client_task,
        "revlink_usb_client",
        REVLINK_USB_CLIENT_TASK_STACK_SIZE,
        &monitor,
        REVLINK_USB_CLIENT_TASK_PRIORITY,
        NULL,
        0
    );
    if (client_created != pdPASS) {
        vTaskDelete(host_task);
#if CONFIG_REVLINK_RUNTIME_SYNC
        vQueueDelete(control_requests);
        control_requests = NULL;
#endif
        monitor_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "read-only USB enumeration probe started");
    return ESP_OK;
}

esp_err_t revlink_accessport_usb_configure_download_sink(
    const revlink_accessport_download_sink_t *sink
)
{
    if (monitor_started || sink == NULL || sink->select_device == NULL
        || sink->release_device == NULL || sink->begin == NULL
        || sink->write == NULL || sink->commit == NULL
        || sink->abort == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    configured_download_sink = *sink;
    download_sink_configured = true;
    return ESP_OK;
}

esp_err_t revlink_accessport_usb_configure_sync_observer(
    const revlink_accessport_sync_observer_config_t *config
)
{
    if (monitor_started || config == NULL || config->observer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    configured_sync_observer = *config;
    return ESP_OK;
}

esp_err_t revlink_accessport_usb_configure_identity_observer(
    const revlink_accessport_identity_observer_config_t *config
)
{
    if (monitor_started || config == NULL || config->observer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    configured_identity_observer = *config;
    return ESP_OK;
}

esp_err_t revlink_accessport_usb_configure_upload_source(
    const revlink_accessport_upload_source_t *source
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    if (monitor_started || source == NULL || source->open == NULL
        || source->read == NULL || source->rewind == NULL
        || source->close == NULL
        || source->cached_file_matches == NULL
        || source->observe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    configured_upload_source = *source;
    upload_source_configured = true;
    return ESP_OK;
#else
    (void)source;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_accessport_usb_request_identity(void)
{
#if CONFIG_REVLINK_RUNTIME_SYNC
    if (!monitor_started || control_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_control_request_t request = {
        .kind = CONTROL_REQUEST_IDENTITY,
    };
    return xQueueSend(control_requests, &request, 0) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_accessport_usb_request_map_upload(
    const revlink_accessport_map_upload_request_t *request
)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES && CONFIG_REVLINK_RUNTIME_SYNC
    if (!monitor_started || control_requests == NULL
        || !upload_source_configured || request == NULL
        || atomic_load(&write_recovery_required)) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_control_request_t control = {
        .kind = CONTROL_REQUEST_MAP_UPLOAD,
        .upload = *request,
    };
    return xQueueSend(control_requests, &control, 0) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
#else
    (void)request;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool revlink_accessport_usb_write_recovery_required(void)
{
#if CONFIG_REVLINK_ALLOW_DEVICE_WRITES
    return atomic_load(&write_recovery_required);
#else
    return false;
#endif
}

esp_err_t revlink_accessport_usb_request_sync(void)
{
#if CONFIG_REVLINK_RUNTIME_SYNC
    if (!monitor_started || control_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_control_request_t request = {
        .kind = CONTROL_REQUEST_SYNC,
    };
    return xQueueSend(control_requests, &request, 0) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_accessport_usb_request_close_recovery(void)
{
#if CONFIG_REVLINK_RUNTIME_SYNC
    if (!monitor_started || control_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_control_request_t request = {
        .kind = CONTROL_REQUEST_CLOSE_RECOVERY,
    };
    return xQueueSend(control_requests, &request, 0) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_accessport_usb_cancel_sync(void)
{
#if CONFIG_REVLINK_RUNTIME_SYNC
    if (!monitor_started || control_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_control_request_t request = {
        .kind = CONTROL_CANCEL_SYNC,
    };
    return xQueueSend(control_requests, &request, 0) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t revlink_accessport_usb_set_root_port_enabled(bool enabled)
{
    if (!monitor_started) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t status = usb_host_lib_set_root_port_power(enabled);
    if (status == ESP_OK) {
        ESP_LOGI(
            TAG,
            "logical USB root port %s",
            enabled ? "enabled" : "disabled"
        );
    } else {
        ESP_LOGE(
            TAG,
            "logical USB root-port change to %s failed: %s",
            enabled ? "enabled" : "disabled",
            esp_err_to_name(status)
        );
    }
    return status;
}
