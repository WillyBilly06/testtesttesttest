idf_component_register(
    SRCS "espnow_sink.c" "ecast_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_netif bsp_extra freertos espressif__esp_codec_dev espressif__esp_hosted liblc3 mbedtls
)
