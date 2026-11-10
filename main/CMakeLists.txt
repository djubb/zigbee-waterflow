idf_component_register(
    SRCS
    "main.c"
    "zigbee.c"
    "zigbee_task.c"
    "flow_task.c"
    INCLUDE_DIRS "."
    REQUIRES "nvs_flash" "driver"
)
