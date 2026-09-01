#include "revlink_usb_link.h"

#include "esp_log.h"
#include "revlink_accessport_usb.h"

static const char *TAG = "revlink_usb_link";

revlink_usb_link_capabilities_t revlink_usb_link_capabilities(void)
{
    return (revlink_usb_link_capabilities_t){
        .physical_data_isolation = false,
        .physical_vbus_isolation = false,
    };
}

esp_err_t revlink_usb_link_disconnect(void)
{
    const esp_err_t status =
        revlink_accessport_usb_set_root_port_enabled(false);
    if (status == ESP_OK) {
        ESP_LOGW(
            TAG,
            "USB host stopped logically; this development board cannot "
            "physically isolate D+/D- or VBUS"
        );
    }
    return status;
}
