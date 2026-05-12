idf_component_register(
    SRCS
        "main_adpcm.c"
        "sink_app.c"
        "core/sink_core.c"
        "net/sink_net.c"
        "audio/sink_audio.c"
        "ecast_crypto.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_timer driver nvs_flash liblc3 mbedtls
)
