#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOURCE_ROOM_ID_LEN 8
#define SOURCE_WIFI_SSID_MAX 32
#define SOURCE_WIFI_PASS_MAX 64
#define SOURCE_DEFAULT_UDP_PORT 5004

typedef struct {
    char room_id[SOURCE_ROOM_ID_LEN + 1];
    int gain_db_x10;
    uint16_t udp_port;
    char wifi_ssid[SOURCE_WIFI_SSID_MAX + 1];
    char wifi_pass[SOURCE_WIFI_PASS_MAX + 1];
    bool password_set;
    bool locked;
} source_config_t;

typedef void (*source_config_apply_cb_t)(void *ctx);

esp_err_t source_config_init(const char *default_room_id);
void source_config_set_apply_callback(source_config_apply_cb_t cb, void *ctx);
void source_config_get(source_config_t *out);
bool source_config_room_id_valid(const char *room_id);
int source_config_gain_q8_from_db_x10(int gain_db_x10);

bool source_config_is_locked(void);
bool source_config_has_wifi(void);
esp_err_t source_config_set_room_id(const char *room_id);
esp_err_t source_config_set_gain_db_x10(int gain_db_x10);
esp_err_t source_config_set_udp_port(uint16_t udp_port);
esp_err_t source_config_set_wifi(const char *ssid, const char *pass);
esp_err_t source_config_set_password(const char *password);
esp_err_t source_config_unlock(const char *password);
void source_config_lock(void);

#ifdef __cplusplus
}
#endif
