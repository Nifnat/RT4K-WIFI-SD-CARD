idf_component_register(
    SRCS "network.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_common log rt4k_config mdns
)
