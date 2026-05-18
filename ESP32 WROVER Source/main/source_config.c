#include "source_config.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define CFG_NS "src_cfg"
#define CFG_KEY_ROOM "room"
#define CFG_KEY_GAIN "gain_x10"
#define CFG_KEY_SSID "ssid"
#define CFG_KEY_PASS "pass"
#define CFG_KEY_PWSET "pw_set"
#define CFG_KEY_PWHASH "pw_hash"

static const char *TAG = "source_cfg";

static source_config_t s_cfg;
static uint8_t s_password_hash[32];
static SemaphoreHandle_t s_mutex;
static source_config_apply_cb_t s_apply_cb;
static void *s_apply_ctx;

static void hash_password(const char *password, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    static const char domain[] = "room-source-ui-password-v1:";
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)domain, strlen(domain));
    if (password) {
        mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    }
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void notify_apply(void)
{
    if (s_apply_cb) {
        s_apply_cb(s_apply_ctx);
    }
}

static esp_err_t save_string(nvs_handle_t nvs, const char *key, const char *value)
{
    return nvs_set_str(nvs, key, value ? value : "");
}

static void load_string(nvs_handle_t nvs, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(nvs, key, dst, &len);
    if (err != ESP_OK) {
        dst[0] = '\0';
    }
}

bool source_config_room_id_valid(const char *room_id)
{
    if (!room_id || strlen(room_id) != SOURCE_ROOM_ID_LEN) {
        return false;
    }
    for (int i = 0; i < SOURCE_ROOM_ID_LEN; ++i) {
        if (i == 3) {
            if (room_id[i] != '-') {
                return false;
            }
        } else if (!isalnum((unsigned char)room_id[i])) {
            return false;
        }
    }
    return true;
}

int source_config_gain_q8_from_db_x10(int gain_db_x10)
{
    if (gain_db_x10 < -240) {
        gain_db_x10 = -240;
    } else if (gain_db_x10 > 240) {
        gain_db_x10 = 240;
    }
    double linear = pow(10.0, ((double)gain_db_x10 / 10.0) / 20.0);
    return (int)(linear * 256.0 + 0.5);
}

esp_err_t source_config_init(const char *default_room_id)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(s_password_hash, 0, sizeof(s_password_hash));
    snprintf(s_cfg.room_id, sizeof(s_cfg.room_id), "%s",
             source_config_room_id_valid(default_room_id) ? default_room_id : "A10-0001");
    s_cfg.gain_db_x10 = 50;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char room[SOURCE_ROOM_ID_LEN + 1] = {0};
    load_string(nvs, CFG_KEY_ROOM, room, sizeof(room));
    if (source_config_room_id_valid(room)) {
        memcpy(s_cfg.room_id, room, sizeof(s_cfg.room_id));
    }

    int32_t gain = 0;
    if (nvs_get_i32(nvs, CFG_KEY_GAIN, &gain) == ESP_OK) {
        s_cfg.gain_db_x10 = gain;
    }

    load_string(nvs, CFG_KEY_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_string(nvs, CFG_KEY_PASS, s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));

    uint8_t pw_set = 0;
    if (nvs_get_u8(nvs, CFG_KEY_PWSET, &pw_set) == ESP_OK && pw_set != 0) {
        size_t hash_len = sizeof(s_password_hash);
        if (nvs_get_blob(nvs, CFG_KEY_PWHASH, s_password_hash, &hash_len) == ESP_OK &&
            hash_len == sizeof(s_password_hash)) {
            s_cfg.password_set = true;
            s_cfg.locked = true;
        }
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded config room=%s gain=%.1fdB wifi=%s lock=%d",
             s_cfg.room_id, s_cfg.gain_db_x10 / 10.0,
             s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "(none)", s_cfg.locked ? 1 : 0);
    return ESP_OK;
}

void source_config_set_apply_callback(source_config_apply_cb_t cb, void *ctx)
{
    s_apply_cb = cb;
    s_apply_ctx = ctx;
}

void source_config_get(source_config_t *out)
{
    if (!out || !s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

bool source_config_is_locked(void)
{
    bool locked = true;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        locked = s_cfg.locked;
        xSemaphoreGive(s_mutex);
    }
    return locked;
}

bool source_config_has_wifi(void)
{
    bool has_wifi = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        has_wifi = s_cfg.wifi_ssid[0] != '\0';
        xSemaphoreGive(s_mutex);
    }
    return has_wifi;
}

esp_err_t source_config_set_room_id(const char *room_id)
{
    if (!source_config_room_id_valid(room_id) || source_config_is_locked()) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = strncmp(s_cfg.room_id, room_id, sizeof(s_cfg.room_id)) != 0;
    xSemaphoreGive(s_mutex);
    if (!changed) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = save_string(nvs, CFG_KEY_ROOM, room_id);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_cfg.room_id, sizeof(s_cfg.room_id), "%s", room_id);
        xSemaphoreGive(s_mutex);
        notify_apply();
    }
    return err;
}

esp_err_t source_config_set_gain_db_x10(int gain_db_x10)
{
    if (source_config_is_locked() || gain_db_x10 < -240 || gain_db_x10 > 240) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_cfg.gain_db_x10 != gain_db_x10;
    xSemaphoreGive(s_mutex);
    if (!changed) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(nvs, CFG_KEY_GAIN, gain_db_x10);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_cfg.gain_db_x10 = gain_db_x10;
        xSemaphoreGive(s_mutex);
        notify_apply();
    }
    return err;
}

esp_err_t source_config_set_wifi(const char *ssid, const char *pass)
{
    if (source_config_is_locked() || !ssid || ssid[0] == '\0' ||
        strlen(ssid) > SOURCE_WIFI_SSID_MAX ||
        (pass && strlen(pass) > SOURCE_WIFI_PASS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *new_pass = pass ? pass : "";
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = strncmp(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid)) != 0 ||
                   strncmp(s_cfg.wifi_pass, new_pass, sizeof(s_cfg.wifi_pass)) != 0;
    xSemaphoreGive(s_mutex);
    if (!changed) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = save_string(nvs, CFG_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = save_string(nvs, CFG_KEY_PASS, new_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", ssid);
        snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%s", new_pass);
        xSemaphoreGive(s_mutex);
        notify_apply();
    }
    return err;
}

esp_err_t source_config_set_password(const char *password)
{
    if (!password || strlen(password) < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t hash[32];
    hash_password(password, hash);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = !s_cfg.password_set ||
                   memcmp(s_password_hash, hash, sizeof(s_password_hash)) != 0;
    xSemaphoreGive(s_mutex);
    if (!changed) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, CFG_KEY_PWSET, 1);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, CFG_KEY_PWHASH, hash, sizeof(hash));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(s_password_hash, hash, sizeof(s_password_hash));
        s_cfg.password_set = true;
        s_cfg.locked = true;
        xSemaphoreGive(s_mutex);
    }
    return err;
}

esp_err_t source_config_unlock(const char *password)
{
    if (!password) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[32];
    hash_password(password, hash);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_cfg.password_set && memcmp(hash, s_password_hash, sizeof(hash)) == 0;
    if (ok) {
        s_cfg.locked = false;
    }
    xSemaphoreGive(s_mutex);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void source_config_lock(void)
{
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_cfg.password_set) {
        s_cfg.locked = true;
    }
    xSemaphoreGive(s_mutex);
}
