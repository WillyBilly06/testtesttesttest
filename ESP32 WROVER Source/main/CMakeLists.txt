idf_component_register(
    SRCS "source_main.c" "source_config.c" "source_wifi.c" "source_web.cpp" "source_udp_stream.c" "source_udp_control.c" "cp_logo_png.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server esp_netif nvs_flash esp_timer esp_app_format driver mbedtls lwip sbc_codec
)
