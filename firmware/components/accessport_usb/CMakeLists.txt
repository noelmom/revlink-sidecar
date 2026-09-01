idf_component_register(
    SRCS "revlink_accessport_usb.c"
    INCLUDE_DIRS "include"
    REQUIRES
        accessport_catalog
        accessport_protocol
        revlink_core
        revlink_sync
        espressif__usb
        esp_timer
)
