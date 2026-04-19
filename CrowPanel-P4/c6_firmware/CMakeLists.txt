# CrowPanel P4 – C6 Coprocessor Firmware
#
# ESP-Hosted slave + ESP-NOW audio sink.
# Extends the stock ESP-Hosted slave with custom ESP-NOW handling.
#
cmake_minimum_required(VERSION 3.16)

set(HOSTED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../factory_project/managed_components/espressif__esp_hosted")
set(HOSTED_SLAVE_DIR "${HOSTED_DIR}/slave")

# Add the wrap flag for esp_wifi_init (required by ESP-Hosted slave)
add_compile_options(-Wl,--wrap=esp_wifi_init)

# Read version from hosted slave header
file(READ "${HOSTED_SLAVE_DIR}/main/esp_hosted_coprocessor_fw_ver.h" HEADER_CONTENTS)
set(PROJECT_VERSION_REGEX "#define[ \t]+PROJECT_VERSION_MAJOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_MINOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_PATCH_1[ \t]+([0-9]+)")
string(REGEX MATCH "${PROJECT_VERSION_REGEX}" PROJECT_VERSION_MATCH "${HEADER_CONTENTS}")
if(PROJECT_VERSION_MATCH)
    set(PROJECT_VER "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    message(STATUS "ESP-Hosted slave version: ${PROJECT_VER}")
endif()

# Bump version above hosted base to force OTA update when C6 code changes
set(PROJECT_VER "3.1.16")
message(STATUS "C6 firmware version (overridden): ${PROJECT_VER}")

# No extra component directories needed - C6 forwards raw LC3 without decoding

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXCLUDE_COMPONENTS lwip esp_netif)

set(COMPONENTS esp_timer esptool_py bootloader main nvs_flash esp_rom esp_wifi protocomm driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio esp_pm)

project(crowpanel_c6_firmware)
idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)
