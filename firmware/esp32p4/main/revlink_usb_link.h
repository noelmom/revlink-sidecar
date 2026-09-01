#ifndef REVLINK_USB_LINK_H
#define REVLINK_USB_LINK_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool physical_data_isolation;
    bool physical_vbus_isolation;
} revlink_usb_link_capabilities_t;

revlink_usb_link_capabilities_t revlink_usb_link_capabilities(void);
esp_err_t revlink_usb_link_disconnect(void);

#endif
