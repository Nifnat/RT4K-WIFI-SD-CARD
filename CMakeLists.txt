cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

set(PROJECT_VER "1.1.2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rt4k-wifi-sd-card)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WEB_ASSETS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(WEB_ASSETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/web_assets")
set(WEB_ASSETS_STAMP "${WEB_ASSETS_DIR}/.prepared")

file(GLOB_RECURSE WEB_ASSET_INPUTS CONFIGURE_DEPENDS "${WEB_ASSETS_SRC_DIR}/*")

add_custom_command(
	OUTPUT "${WEB_ASSETS_STAMP}"
	COMMAND ${CMAKE_COMMAND} -E remove_directory "${WEB_ASSETS_DIR}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${WEB_ASSETS_DIR}"
	COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_web_assets.py" "${WEB_ASSETS_SRC_DIR}" "${WEB_ASSETS_DIR}"
	COMMAND ${CMAKE_COMMAND} -E touch "${WEB_ASSETS_STAMP}"
	DEPENDS ${WEB_ASSET_INPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_web_assets.py"
	COMMENT "Preparing compressed web assets"
	VERBATIM
)

add_custom_target(prepare_web_assets DEPENDS "${WEB_ASSETS_STAMP}")

spiffs_create_partition_image(storage "${WEB_ASSETS_DIR}" FLASH_IN_PROJECT DEPENDS prepare_web_assets)
