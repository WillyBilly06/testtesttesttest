# Main component for C6 firmware
# Merges ESP-Hosted slave sources with our ESP-NOW sink code

set(HOSTED_SLAVE_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/../../factory_project/managed_components/espressif__esp_hosted/slave/main")

if(IS_DIRECTORY "${HOSTED_SLAVE_MAIN}/common")
    set(common_dir "${HOSTED_SLAVE_MAIN}/common")
else()
    set(common_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../factory_project/managed_components/espressif__esp_hosted/common")
endif()

# ── ESP-Hosted slave sources ──
set(COMPONENT_SRCS
    "${common_dir}/proto/esp_hosted_rpc.pb-c.c"
    "${common_dir}/utils/esp_hosted_cli.c"
    "${HOSTED_SLAVE_MAIN}/protocomm_pserial.c"
    "${HOSTED_SLAVE_MAIN}/esp_hosted_coprocessor.c"
    "${HOSTED_SLAVE_MAIN}/mempool.c"
    "${HOSTED_SLAVE_MAIN}/stats.c"
    "${HOSTED_SLAVE_MAIN}/mempool_ll.c"
    "${HOSTED_SLAVE_MAIN}/host_power_save.c"
    "${HOSTED_SLAVE_MAIN}/slave_light_sleep.c"
    "${HOSTED_SLAVE_MAIN}/nw_split_router.c"
    "${HOSTED_SLAVE_MAIN}/slave_transport_gpio_pin_guard.c"
    "${HOSTED_SLAVE_MAIN}/slave_gpio_expander.c"
    "${HOSTED_SLAVE_MAIN}/slave_ext_coex.c"
    "${HOSTED_SLAVE_MAIN}/slave_control.c"
)

if(CONFIG_SOC_BT_SUPPORTED)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_bt.c")
endif()

if(CONFIG_ESP_HOSTED_CP_WIFI)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_wifi_std.c")
endif()

if(CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_wifi_enterprise.c")
endif()

if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_network_split.c")
endif()

if(CONFIG_SOC_BT_SUPPORTED)
    if(CONFIG_IDF_TARGET_ESP32)
        list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_bt_uart_esp32.c")
    elseif(CONFIG_IDF_TARGET_ESP32C3 OR CONFIG_IDF_TARGET_ESP32S3)
        list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_bt_uart_esp32c3_s3.c")
    else()
        list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/slave_bt_uart_esp32xx.c")
    endif()
endif()

if(CONFIG_ESP_SDIO_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/sdio_slave_api.c")
elseif(CONFIG_ESP_SPI_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/spi_slave_api.c")
elseif(CONFIG_ESP_SPI_HD_MODE)
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/spi_hd_slave_api.c")
else()
    list(APPEND COMPONENT_SRCS "${HOSTED_SLAVE_MAIN}/uart_slave_api.c")
endif()

# ── Our ESP-NOW sink code ──
list(APPEND COMPONENT_SRCS
    "app_main.c"
    "espnow_bridge_handler.c"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "${HOSTED_SLAVE_MAIN}"
    "${common_dir}"
    "${common_dir}/log"
    "${common_dir}/proto"
    "${common_dir}/rpc"
    "${common_dir}/transport"
    "${common_dir}/utils"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../factory_project/components/espnow_sink/include"
)

register_component()

# protocomm private headers (required by hosted slave)
idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")

# Wrap esp_wifi_init (required by hosted slave)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")
