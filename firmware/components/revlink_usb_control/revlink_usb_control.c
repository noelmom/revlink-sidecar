#include "revlink_usb_control.h"

#include <string.h>

static bool valid_mode(revlink_usb_vbus_mode_t mode)
{
    return mode == REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK
        || mode == REVLINK_USB_VBUS_EXTERNALLY_POWERED;
}

static void delay_if_requested(
    const revlink_usb_control_t *control,
    uint32_t delay_ms
)
{
    if (delay_ms != 0U) {
        control->config.delay(
            control->config.driver_context,
            delay_ms
        );
    }
}

static void enter_fault(revlink_usb_control_t *control)
{
    control->snapshot.state = REVLINK_USB_LINK_FAULT;
}

revlink_usb_control_status_t revlink_usb_control_force_isolate(
    revlink_usb_control_t *control
)
{
    if (control == NULL || !control->initialized) {
        return REVLINK_USB_CONTROL_INVALID_STATE;
    }

    control->snapshot.state = REVLINK_USB_LINK_ISOLATING;

    const bool data_disabled =
        control->config.set_data_connected(
            control->config.driver_context,
            false
        );
    if (data_disabled) {
        control->snapshot.data_connected = false;
    }

    delay_if_requested(control, control->config.isolate_settle_ms);

    const bool vbus_disabled =
        control->config.set_vbus_enabled(
            control->config.driver_context,
            false
        );
    if (vbus_disabled) {
        control->snapshot.vbus_enabled = false;
    }

    if (!data_disabled || !vbus_disabled) {
        enter_fault(control);
        return REVLINK_USB_CONTROL_DRIVER_ERROR;
    }

    control->snapshot.state = REVLINK_USB_LINK_ISOLATED;
    control->snapshot.vbus_mode =
        REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK;
    return REVLINK_USB_CONTROL_OK;
}

revlink_usb_control_status_t revlink_usb_control_init(
    revlink_usb_control_t *control,
    const revlink_usb_control_config_t *config
)
{
    if (control == NULL || config == NULL
        || config->set_data_connected == NULL
        || config->set_vbus_enabled == NULL
        || config->delay == NULL) {
        return REVLINK_USB_CONTROL_INVALID_ARGUMENT;
    }

    memset(control, 0, sizeof(*control));
    control->config = *config;
    control->initialized = true;
    control->snapshot.state = REVLINK_USB_LINK_UNINITIALIZED;

    return revlink_usb_control_force_isolate(control);
}

revlink_usb_control_status_t revlink_usb_control_attach(
    revlink_usb_control_t *control,
    revlink_usb_vbus_mode_t vbus_mode
)
{
    if (control == NULL || !control->initialized) {
        return REVLINK_USB_CONTROL_INVALID_STATE;
    }
    if (!valid_mode(vbus_mode)) {
        return REVLINK_USB_CONTROL_INVALID_ARGUMENT;
    }
    if (control->snapshot.state != REVLINK_USB_LINK_ISOLATED) {
        return REVLINK_USB_CONTROL_INVALID_STATE;
    }

    control->snapshot.state = REVLINK_USB_LINK_ATTACHING;
    control->snapshot.vbus_mode = vbus_mode;

    if (vbus_mode == REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK) {
        if (!control->config.set_vbus_enabled(
                control->config.driver_context,
                true
            )) {
            enter_fault(control);
            (void)revlink_usb_control_force_isolate(control);
            enter_fault(control);
            return REVLINK_USB_CONTROL_DRIVER_ERROR;
        }
        control->snapshot.vbus_enabled = true;
        delay_if_requested(control, control->config.vbus_settle_ms);
    } else {
        if (!control->config.set_vbus_enabled(
                control->config.driver_context,
                false
            )) {
            enter_fault(control);
            (void)revlink_usb_control_force_isolate(control);
            enter_fault(control);
            return REVLINK_USB_CONTROL_DRIVER_ERROR;
        }
        control->snapshot.vbus_enabled = false;
    }

    if (!control->config.set_data_connected(
            control->config.driver_context,
            true
        )) {
        enter_fault(control);
        (void)revlink_usb_control_force_isolate(control);
        enter_fault(control);
        return REVLINK_USB_CONTROL_DRIVER_ERROR;
    }
    control->snapshot.data_connected = true;
    delay_if_requested(control, control->config.data_settle_ms);

    control->snapshot.state = REVLINK_USB_LINK_ATTACHED;
    return REVLINK_USB_CONTROL_OK;
}

revlink_usb_control_status_t revlink_usb_control_isolate(
    revlink_usb_control_t *control
)
{
    if (control == NULL || !control->initialized) {
        return REVLINK_USB_CONTROL_INVALID_STATE;
    }
    if (control->snapshot.state == REVLINK_USB_LINK_ISOLATED) {
        return REVLINK_USB_CONTROL_OK;
    }
    if (control->snapshot.state != REVLINK_USB_LINK_ATTACHED) {
        return REVLINK_USB_CONTROL_INVALID_STATE;
    }
    return revlink_usb_control_force_isolate(control);
}

revlink_usb_control_snapshot_t revlink_usb_control_snapshot(
    const revlink_usb_control_t *control
)
{
    return control != NULL && control->initialized
        ? control->snapshot
        : (revlink_usb_control_snapshot_t){
            .state = REVLINK_USB_LINK_UNINITIALIZED,
        };
}

const char *revlink_usb_control_state_name(
    revlink_usb_control_state_t state
)
{
    switch (state) {
    case REVLINK_USB_LINK_UNINITIALIZED:
        return "uninitialized";
    case REVLINK_USB_LINK_ISOLATED:
        return "isolated";
    case REVLINK_USB_LINK_ATTACHING:
        return "attaching";
    case REVLINK_USB_LINK_ATTACHED:
        return "attached";
    case REVLINK_USB_LINK_ISOLATING:
        return "isolating";
    case REVLINK_USB_LINK_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}
