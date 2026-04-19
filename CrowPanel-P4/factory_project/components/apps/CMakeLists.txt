set(APP_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/calculator
    ${CMAKE_CURRENT_LIST_DIR}/setting
)

set(APPS_C_SRCS)
set(APPS_CPP_SRCS)

foreach(APP_SOURCE_DIR IN LISTS APP_SOURCE_DIRS)
    file(GLOB_RECURSE APP_DIR_C_SRCS ${APP_SOURCE_DIR}/*.c)
    file(GLOB_RECURSE APP_DIR_CPP_SRCS ${APP_SOURCE_DIR}/*.cpp)
    list(APPEND APPS_C_SRCS ${APP_DIR_C_SRCS})
    list(APPEND APPS_CPP_SRCS ${APP_DIR_CPP_SRCS})
endforeach()

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS .
    REQUIRES lvgl__lvgl esp_event esp_wifi nvs_flash esp_driver_jpeg esp_mm esp-brookesia bsp_extra esp32_p4_function_ev_board esp_video pedestrian_detect human_face_detect espressif__esp_lcd_touch_gt911 espnow_sink esp_app_format)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-format
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wno-format
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
    -DLV_LVGL_H_INCLUDE_SIMPLE
)
