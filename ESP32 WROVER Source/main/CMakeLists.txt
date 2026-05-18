idf_component_register(
    SRCS "source_main.c" "source_config.c" "source_wifi.c" "source_web.cpp" "cp_logo_png.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server esp_netif nvs_flash esp_timer esp_app_format driver mbedtls lwip sbc_codec
)

# Force -O3 + loop unrolling on the audio hot path AND the Web UI.
# The default project optimisation is -O2 (CONFIG_COMPILER_OPTIMIZATION_PERF);
# IDF won't let you set -O3 globally because some IDF code mis-compiles
# at -O3, but applying it per-file is safe.
#
# source_main.c: tightens per-frame encoder loop (PCM shift, gain, CRC,
# AES nonce build, packet copy).
#
# source_web.cpp: HTTP request handlers run on core 0 (httpd task) and
# every millisecond they hold core 0 is a millisecond espnow_send_cb is
# delayed, which directly widens the sink-side audio spread. Most of the
# handler runtime is snprintf + httpd_resp_sendstr_chunk loops; -O3 +
# unrolling cuts those significantly.
#
# The SBC encoder itself is already -O3 in components/sbc_codec.
set_source_files_properties(
    source_main.c
    source_web.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-funroll-loops;-ftree-vectorize"
)
