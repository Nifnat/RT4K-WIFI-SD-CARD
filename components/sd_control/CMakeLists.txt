idf_component_register(
    SRCS "sd_control.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_common log fatfs vfs board_pins esp_timer
    PRIV_REQUIRES spi_flash sdmmc esp_driver_sdspi esp_driver_sdmmc
)
