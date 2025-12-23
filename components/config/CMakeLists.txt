idf_component_register(
    SRCS
        "gateway_config.c"
        "nvs_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
