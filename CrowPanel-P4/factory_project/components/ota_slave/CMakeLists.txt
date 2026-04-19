idf_component_register(
    SRCS "ota_slave.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition freertos nvs_flash espressif__esp_hosted espnow_sink
)
