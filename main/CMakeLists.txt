idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        sx1276
        lora_gateway
        network
        config
        nvs_flash
        esp_wifi
        esp_eth
        driver
)
