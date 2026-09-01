#ifndef REVLINK_USB_CONTROL_H
#define REVLINK_USB_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REVLINK_USB_CONTROL_OK = 0,
    REVLINK_USB_CONTROL_INVALID_ARGUMENT,
    REVLINK_USB_CONTROL_INVALID_STATE,
    REVLINK_USB_CONTROL_DRIVER_ERROR,
} revlink_usb_control_status_t;

typedef enum {
    REVLINK_USB_LINK_UNINITIALIZED = 0,
    REVLINK_USB_LINK_ISOLATED,
    REVLINK_USB_LINK_ATTACHING,
    REVLINK_USB_LINK_ATTACHED,
    REVLINK_USB_LINK_ISOLATING,
    REVLINK_USB_LINK_FAULT,
} revlink_usb_control_state_t;

typedef enum {
    REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK = 0,
    REVLINK_USB_VBUS_EXTERNALLY_POWERED,
} revlink_usb_vbus_mode_t;

typedef bool (*revlink_usb_control_set_output_t)(
    void *context,
    bool enabled
);

typedef void (*revlink_usb_control_delay_t)(
    void *context,
    uint32_t delay_ms
);

typedef struct {
    void *driver_context;
    revlink_usb_control_set_output_t set_data_connected;
    revlink_usb_control_set_output_t set_vbus_enabled;
    revlink_usb_control_delay_t delay;
    uint32_t vbus_settle_ms;
    uint32_t data_settle_ms;
    uint32_t isolate_settle_ms;
} revlink_usb_control_config_t;

typedef struct {
    revlink_usb_control_state_t state;
    bool data_connected;
    bool vbus_enabled;
    revlink_usb_vbus_mode_t vbus_mode;
} revlink_usb_control_snapshot_t;

typedef struct {
    revlink_usb_control_config_t config;
    revlink_usb_control_snapshot_t snapshot;
    bool initialized;
} revlink_usb_control_t;

revlink_usb_control_status_t revlink_usb_control_init(
    revlink_usb_control_t *control,
    const revlink_usb_control_config_t *config
);

revlink_usb_control_status_t revlink_usb_control_attach(
    revlink_usb_control_t *control,
    revlink_usb_vbus_mode_t vbus_mode
);

revlink_usb_control_status_t revlink_usb_control_isolate(
    revlink_usb_control_t *control
);

revlink_usb_control_status_t revlink_usb_control_force_isolate(
    revlink_usb_control_t *control
);

revlink_usb_control_snapshot_t revlink_usb_control_snapshot(
    const revlink_usb_control_t *control
);

const char *revlink_usb_control_state_name(
    revlink_usb_control_state_t state
);

#ifdef __cplusplus
}
#endif

#endif
