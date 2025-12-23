idf_component_register(
    SRCS
        "network_manager.c"
        "wifi_handler.c"
        "ethernet_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_eth esp_netif esp_event driver lwip config
)
