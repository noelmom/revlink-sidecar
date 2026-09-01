idf_component_register(
    SRCS "revlink_status_model.c"
    INCLUDE_DIRS "include"
    REQUIRES revlink_core revlink_network revlink_sync
)
