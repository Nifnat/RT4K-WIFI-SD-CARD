idf_component_register(
    SRCS "rt4k_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_common log sd_control
)
