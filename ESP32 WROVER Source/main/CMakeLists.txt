file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/bt/host/bluedroid/external/liblc3" LC3_ROOT)
file(GLOB LC3_SRCS "${LC3_ROOT}/src/*.c")

idf_component_register(
    SRCS "source_main.c" ${LC3_SRCS}
    INCLUDE_DIRS "." "${LC3_ROOT}/include" "${LC3_ROOT}/src"
    REQUIRES esp_wifi nvs_flash esp_timer driver mbedtls
)

set_source_files_properties(${LC3_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-error")
