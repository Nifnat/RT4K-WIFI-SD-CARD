idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_common log spiffs sd_control network rt4k_config
    PRIV_REQUIRES vfs fatfs
)
