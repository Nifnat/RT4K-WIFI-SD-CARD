cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rt4k-wifi-sd-card)

# Generate SPIFFS image from frontend data directory
spiffs_create_partition_image(storage data FLASH_IN_PROJECT)
