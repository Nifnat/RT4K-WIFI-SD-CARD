idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        spiffs
        rt4k_config
        sd_control
        network
        web_server
)
