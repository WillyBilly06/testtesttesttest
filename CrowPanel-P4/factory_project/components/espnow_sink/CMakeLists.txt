idf_component_register(
    SRCS "espnow_sink.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_netif bsp_extra freertos espressif__esp_codec_dev espressif__esp_hosted sbc_codec mbedtls esp_driver_i2s esp_driver_gpio driver
)
