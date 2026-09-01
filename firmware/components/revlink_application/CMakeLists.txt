idf_component_register(
    SRCS "revlink_application.c"
    INCLUDE_DIRS "include"
    REQUIRES accessport_protocol revlink_core revlink_sync
)
