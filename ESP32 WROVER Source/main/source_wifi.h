#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t source_wifi_start(uint8_t espnow_channel);
esp_err_t source_wifi_get_sta_mac(uint8_t mac[6]);
bool source_wifi_is_sta_connected(void);
uint32_t source_wifi_sta_ip(void);
uint8_t source_wifi_current_channel(void);
const char *source_wifi_status_text(void);
void source_wifi_set_audio_streaming(bool streaming);
esp_err_t source_wifi_save_credentials_if_channel_ok(const char *ssid, const char *pass,
                                                     char *err_buf, size_t err_buf_len);

#ifdef __cplusplus
}
#endif
