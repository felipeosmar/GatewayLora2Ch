idf_component_register(
    SRCS
        "lora_gateway.c"
        "packet_forwarder.c"
        "channel_manager.c"
    INCLUDE_DIRS "include" "."
    REQUIRES sx1276 network config esp_timer lwip json
)
