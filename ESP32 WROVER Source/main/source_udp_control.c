#include "source_udp_control.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "source_wifi.h"

#define CTRL_NS "udp_ctrl"
#define CTRL_KEY_SOURCE_ID "source_id"
#define CTRL_KEY_CLIENTS "clients"
#define CTRL_PAIR_PIN_NONE ""
#define CTRL_PAIR_DEFAULT_MS 60000
#define CTRL_ACTIVE_TIMEOUT_MS 7000
#define CTRL_DISCOVERY_LOG_MS 15000
#define CTRL_AUDIO_TARGET_LOG_MS 5000

#define ENDPOINT_ARGS(addr) \
    (uint8_t)(ntohl((addr).sin_addr.s_addr) >> 24), \
    (uint8_t)(ntohl((addr).sin_addr.s_addr) >> 16), \
    (uint8_t)(ntohl((addr).sin_addr.s_addr) >> 8), \
    (uint8_t)(ntohl((addr).sin_addr.s_addr)), \
    (unsigned)ntohs((addr).sin_port)

typedef struct {
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    char client_name[RAUD_CLIENT_NAME_LEN];
    uint8_t client_auth_key[RAUD_KEY_LEN];
    uint8_t enabled;
    uint32_t added_ms;
    uint32_t last_seen_ms;
} stored_client_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    stored_client_t clients[SOURCE_UDP_MAX_AUTH_CLIENTS];
} client_store_t;

typedef struct {
    bool used;
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    uint32_t addr;
    uint16_t port;
    uint32_t session_id;
    uint32_t last_seen_ms;
} active_client_t;

static const char *TAG = "udp_control";
static SemaphoreHandle_t s_lock;
static uint8_t s_source_id[RAUD_SOURCE_ID_LEN];
static char s_room_code[RAUD_ROOM_CODE_LEN] = "A10-0001";
static char s_room_name[RAUD_ROOM_NAME_LEN] = "Room Source";
static client_store_t s_store;
static active_client_t s_active[SOURCE_UDP_MAX_AUTH_CLIENTS];
static char s_pair_pin[7] = CTRL_PAIR_PIN_NONE;
static int64_t s_pair_expires_us;
static volatile uint32_t s_advertisements_sent;
static volatile uint32_t s_pair_accepts;
static volatile uint32_t s_pair_rejects;
static volatile uint32_t s_join_accepts;
static volatile uint32_t s_join_rejects;
static volatile uint32_t s_auth_failures;
static uint32_t s_last_discovery_log_ms;
static uint32_t s_last_audio_target_log_ms;

static bool ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void random_bytes(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = esp_random();
        size_t n = (len - i < 4) ? len - i : 4;
        memcpy(out + i, &r, n);
    }
}

static esp_err_t cmac16(const uint8_t key[RAUD_KEY_LEN], const void *data, size_t len,
                        uint8_t out[RAUD_TAG_LEN])
{
    const mbedtls_cipher_info_t *ci = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_ECB);
    if (!ci) {
        return ESP_FAIL;
    }
    return mbedtls_cipher_cmac(ci, key, 256, data, len, out) == 0 ? ESP_OK : ESP_FAIL;
}

static bool tag_equal(const uint8_t a[RAUD_TAG_LEN], const uint8_t b[RAUD_TAG_LEN])
{
    uint8_t diff = 0;
    for (int i = 0; i < RAUD_TAG_LEN; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static void room_pin_from_code(const char *room_code, char out[7])
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < RAUD_ROOM_CODE_LEN && room_code && room_code[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)room_code[i];
        if (isspace(c)) {
            continue;
        }
        c = (unsigned char)toupper(c);
        hash ^= c;
        hash *= 16777619u;
    }
    snprintf(out, 7, "%06" PRIu32, hash % 1000000u);
}

static void refresh_pair_pin_locked(void)
{
    room_pin_from_code(s_room_code, s_pair_pin);
    s_pair_expires_us = INT64_MAX;
}

static esp_err_t aes_ctr_crypt(const uint8_t key[RAUD_KEY_LEN],
                               const uint8_t nonce[RAUD_NONCE_LEN],
                               const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context ctx;
    uint8_t nc[16], stream[16] = {0};
    size_t offset = 0;
    memcpy(nc, nonce, 16);
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ctr(&ctx, len, &offset, nc, stream, in, out);
    }
    mbedtls_aes_free(&ctx);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static void derive_pin_key(const char pin[7], uint8_t out[RAUD_KEY_LEN])
{
    mbedtls_sha256_context ctx;
    static const char domain[] = "room-audio-pin-v1";
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)domain, strlen(domain));
    mbedtls_sha256_update(&ctx, s_source_id, sizeof(s_source_id));
    mbedtls_sha256_update(&ctx, (const uint8_t *)pin, 6);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static esp_err_t save_clients_locked(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CTRL_NS, NVS_READWRITE, &nvs), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(nvs, CTRL_KEY_CLIENTS, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t load_state(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CTRL_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(s_source_id);
    err = nvs_get_blob(nvs, CTRL_KEY_SOURCE_ID, s_source_id, &len);
    if (err != ESP_OK || len != sizeof(s_source_id)) {
        random_bytes(s_source_id, sizeof(s_source_id));
        err = nvs_set_blob(nvs, CTRL_KEY_SOURCE_ID, s_source_id, sizeof(s_source_id));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        if (err != ESP_OK) {
            goto done;
        }
    }

    memset(&s_store, 0, sizeof(s_store));
    len = sizeof(s_store);
    if (nvs_get_blob(nvs, CTRL_KEY_CLIENTS, &s_store, &len) != ESP_OK ||
        len != sizeof(s_store) || s_store.version != 1) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.version = 1;
    }

done:
    nvs_close(nvs);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static bool source_matches(const uint8_t source_id[RAUD_SOURCE_ID_LEN], const char room_code[RAUD_ROOM_CODE_LEN])
{
    return memcmp(source_id, s_source_id, RAUD_SOURCE_ID_LEN) == 0 &&
           strncmp(room_code, s_room_code, RAUD_ROOM_CODE_LEN) == 0;
}

static stored_client_t *find_client_locked(const uint8_t client_id[RAUD_CLIENT_ID_LEN])
{
    for (uint32_t i = 0; i < s_store.count && i < SOURCE_UDP_MAX_AUTH_CLIENTS; ++i) {
        if (memcmp(s_store.clients[i].client_id, client_id, RAUD_CLIENT_ID_LEN) == 0) {
            return &s_store.clients[i];
        }
    }
    return NULL;
}

static void mark_active_locked(const uint8_t client_id[RAUD_CLIENT_ID_LEN], uint32_t addr,
                               uint16_t port, uint32_t session_id)
{
    active_client_t *slot = NULL;
    for (int i = 0; i < SOURCE_UDP_MAX_AUTH_CLIENTS; ++i) {
        if (s_active[i].used && memcmp(s_active[i].client_id, client_id, RAUD_CLIENT_ID_LEN) == 0) {
            slot = &s_active[i];
            break;
        }
        if (!s_active[i].used && !slot) {
            slot = &s_active[i];
        }
    }
    if (!slot) {
        slot = &s_active[0];
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->client_id, client_id, RAUD_CLIENT_ID_LEN);
    slot->addr = addr;
    slot->port = port;
    slot->session_id = session_id;
    slot->last_seen_ms = now_ms();
}

static esp_err_t send_reject(int sock, const struct sockaddr_in *dest, socklen_t dest_len, uint8_t type,
                             const uint8_t source_id[RAUD_SOURCE_ID_LEN],
                             const char room_code[RAUD_ROOM_CODE_LEN],
                             const uint8_t client_id[RAUD_CLIENT_ID_LEN])
{
    raud_client_control_t reject = {0};
    reject.hdr.magic = RAUD_MAGIC;
    reject.hdr.version = RAUD_VERSION;
    reject.hdr.msg_type = type;
    reject.hdr.length = sizeof(reject);
    if (source_id) memcpy(reject.source_id, source_id, RAUD_SOURCE_ID_LEN);
    if (room_code) memcpy(reject.room_code, room_code, RAUD_ROOM_CODE_LEN);
    if (client_id) memcpy(reject.client_id, client_id, RAUD_CLIENT_ID_LEN);
    (void)sendto(sock, &reject, sizeof(reject), 0, (const struct sockaddr *)dest, dest_len);
    return ESP_OK;
}

static void handle_pair_request(int sock, const raud_pair_request_t *req,
                                const struct sockaddr_in *from, socklen_t from_len)
{
    ESP_LOGI(TAG, "PAIR_REQUEST from %u.%u.%u.%u:%u client=%02X%02X%02X%02X name=%.*s",
             ENDPOINT_ARGS((*from)), req->client_id[0], req->client_id[1],
             req->client_id[2], req->client_id[3], RAUD_CLIENT_NAME_LEN - 1,
             req->client_name);

    if (req->hdr.length != sizeof(*req) || !source_matches(req->source_id, req->room_code)) {
        s_pair_rejects++;
        return;
    }

    char pin[7] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    refresh_pair_pin_locked();
    memcpy(pin, s_pair_pin, sizeof(pin));
    xSemaphoreGive(s_lock);

    uint8_t pin_key[RAUD_KEY_LEN];
    uint8_t expected[RAUD_TAG_LEN];
    derive_pin_key(pin, pin_key);
    raud_pair_request_t tmp = *req;
    memset(tmp.pin_proof, 0, sizeof(tmp.pin_proof));
    if (cmac16(pin_key, &tmp, sizeof(tmp), expected) != ESP_OK ||
        !tag_equal(expected, req->pin_proof)) {
        s_pair_rejects++;
        s_auth_failures++;
        ESP_LOGW(TAG, "PAIR_REJECT sent to %u.%u.%u.%u:%u reason=pin_auth",
                 ENDPOINT_ARGS((*from)));
        (void)send_reject(sock, from, from_len, RAUD_MSG_PAIR_REJECT, req->source_id, req->room_code, req->client_id);
        return;
    }

    raud_pair_accept_t resp = {0};
    resp.hdr.magic = RAUD_MAGIC;
    resp.hdr.version = RAUD_VERSION;
    resp.hdr.msg_type = RAUD_MSG_PAIR_ACCEPT;
    resp.hdr.length = sizeof(resp);
    memcpy(resp.source_id, s_source_id, sizeof(resp.source_id));
    memcpy(resp.room_code, s_room_code, sizeof(resp.room_code));
    memcpy(resp.client_id, req->client_id, sizeof(resp.client_id));
    memcpy(resp.client_nonce, req->client_nonce, sizeof(resp.client_nonce));
    random_bytes(resp.source_nonce, sizeof(resp.source_nonce));
    random_bytes(resp.key_wrap_nonce, sizeof(resp.key_wrap_nonce));

    uint8_t client_key[RAUD_KEY_LEN];
    random_bytes(client_key, sizeof(client_key));
    if (aes_ctr_crypt(pin_key, resp.key_wrap_nonce, client_key,
                      resp.encrypted_client_auth_key, sizeof(client_key)) != ESP_OK) {
        s_pair_rejects++;
        return;
    }

    raud_pair_accept_t auth_copy = resp;
    memset(auth_copy.auth_tag, 0, sizeof(auth_copy.auth_tag));
    if (cmac16(pin_key, &auth_copy, sizeof(auth_copy), resp.auth_tag) != ESP_OK) {
        s_pair_rejects++;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_client_t *client = find_client_locked(req->client_id);
    if (!client && s_store.count < SOURCE_UDP_MAX_AUTH_CLIENTS) {
        client = &s_store.clients[s_store.count++];
    }
    if (client) {
        memset(client, 0, sizeof(*client));
        memcpy(client->client_id, req->client_id, RAUD_CLIENT_ID_LEN);
        snprintf(client->client_name, sizeof(client->client_name), "%.*s",
                 RAUD_CLIENT_NAME_LEN - 1, req->client_name);
        memcpy(client->client_auth_key, client_key, RAUD_KEY_LEN);
        client->enabled = 1;
        client->added_ms = now_ms();
        client->last_seen_ms = client->added_ms;
        (void)save_clients_locked();
    }
    xSemaphoreGive(s_lock);

    if (!client) {
        s_pair_rejects++;
        ESP_LOGW(TAG, "PAIR_REJECT sent to %u.%u.%u.%u:%u reason=client_table_full",
                 ENDPOINT_ARGS((*from)));
        (void)send_reject(sock, from, from_len, RAUD_MSG_PAIR_REJECT, req->source_id, req->room_code, req->client_id);
        return;
    }

    (void)sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr *)from, from_len);
    ESP_LOGI(TAG, "PAIR_ACCEPT sent to %u.%u.%u.%u:%u",
             ENDPOINT_ARGS((*from)));
    s_pair_accepts++;
}

static void handle_join_hello(int sock, const raud_join_hello_t *req,
                              const struct sockaddr_in *from, socklen_t from_len)
{
    ESP_LOGI(TAG, "JOIN_HELLO from %u.%u.%u.%u:%u audio_port=%u client=%02X%02X%02X%02X",
             ENDPOINT_ARGS((*from)), (unsigned)req->audio_port, req->client_id[0],
             req->client_id[1], req->client_id[2], req->client_id[3]);

    if (req->hdr.length != sizeof(*req) || !source_matches(req->source_id, req->room_code) ||
        req->requested_codec != RAUD_CODEC_SBC || req->audio_port == 0) {
        s_join_rejects++;
        return;
    }

    uint8_t client_key[RAUD_KEY_LEN] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_client_t *client = find_client_locked(req->client_id);
    bool authorized = client && client->enabled;
    if (authorized) {
        memcpy(client_key, client->client_auth_key, sizeof(client_key));
        client->last_seen_ms = now_ms();
        (void)save_clients_locked();
    }
    xSemaphoreGive(s_lock);

    if (!authorized) {
        s_join_rejects++;
        ESP_LOGW(TAG, "JOIN_REJECT sent to %u.%u.%u.%u:%u reason=unknown_client",
                 ENDPOINT_ARGS((*from)));
        (void)send_reject(sock, from, from_len, RAUD_MSG_JOIN_REJECT, req->source_id, req->room_code, req->client_id);
        return;
    }

    uint8_t expected[RAUD_TAG_LEN];
    raud_join_hello_t auth_copy = *req;
    memset(auth_copy.auth_tag, 0, sizeof(auth_copy.auth_tag));
    if (cmac16(client_key, &auth_copy, sizeof(auth_copy), expected) != ESP_OK ||
        !tag_equal(expected, req->auth_tag)) {
        s_join_rejects++;
        s_auth_failures++;
        ESP_LOGW(TAG, "JOIN_REJECT sent to %u.%u.%u.%u:%u reason=auth",
                 ENDPOINT_ARGS((*from)));
        (void)send_reject(sock, from, from_len, RAUD_MSG_JOIN_REJECT, req->source_id, req->room_code, req->client_id);
        return;
    }

    source_udp_session_snapshot_t session;
    if (source_audio_get_udp_session(&session) != ESP_OK) {
        s_join_rejects++;
        return;
    }

    raud_join_accept_t resp = {0};
    resp.hdr.magic = RAUD_MAGIC;
    resp.hdr.version = RAUD_VERSION;
    resp.hdr.msg_type = RAUD_MSG_JOIN_ACCEPT;
    resp.hdr.length = sizeof(resp);
    memcpy(resp.source_id, s_source_id, sizeof(resp.source_id));
    memcpy(resp.room_code, s_room_code, sizeof(resp.room_code));
    resp.stream_id = session.stream_id;
    resp.session_id = session.session_id;
    memcpy(resp.client_id, req->client_id, sizeof(resp.client_id));
    memcpy(resp.client_nonce, req->client_nonce, sizeof(resp.client_nonce));
    random_bytes(resp.source_nonce, sizeof(resp.source_nonce));
    random_bytes(resp.key_wrap_nonce, sizeof(resp.key_wrap_nonce));
    (void)aes_ctr_crypt(client_key, resp.key_wrap_nonce, session.key,
                        resp.encrypted_audio_session_key, sizeof(resp.encrypted_audio_session_key));
    resp.audio_key_len = RAUD_KEY_LEN;
    resp.codec = RAUD_CODEC_SBC;
    resp.sample_rate_hz = 48000;
    resp.channels = session.channels;
    resp.bit_depth = 24;
    resp.frame_us = 8000;
    resp.frame_bytes = session.frame_bytes;
    resp.audio_start_sequence = session.latest_seq;

    raud_join_accept_t tag_copy = resp;
    memset(tag_copy.auth_tag, 0, sizeof(tag_copy.auth_tag));
    (void)cmac16(client_key, &tag_copy, sizeof(tag_copy), resp.auth_tag);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mark_active_locked(req->client_id, from->sin_addr.s_addr, req->audio_port, session.session_id);
    xSemaphoreGive(s_lock);

    (void)sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr *)from, from_len);
    ESP_LOGI(TAG, "JOIN_ACCEPT sent to %u.%u.%u.%u:%u", ENDPOINT_ARGS((*from)));
    ESP_LOGI(TAG, "UDP audio target %u.%u.%u.%u:%u active",
             (uint8_t)(ntohl(from->sin_addr.s_addr) >> 24),
             (uint8_t)(ntohl(from->sin_addr.s_addr) >> 16),
             (uint8_t)(ntohl(from->sin_addr.s_addr) >> 8),
             (uint8_t)ntohl(from->sin_addr.s_addr),
             (unsigned)req->audio_port);
    s_join_accepts++;
}

static void handle_client_control(const raud_client_control_t *req, uint8_t msg_type)
{
    if (req->hdr.length != sizeof(*req) || !source_matches(req->source_id, req->room_code)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_client_t *client = find_client_locked(req->client_id);
    if (!client || !client->enabled) {
        xSemaphoreGive(s_lock);
        return;
    }
    uint8_t expected[RAUD_TAG_LEN];
    raud_client_control_t copy = *req;
    memset(copy.auth_tag, 0, sizeof(copy.auth_tag));
    bool ok = cmac16(client->client_auth_key, &copy, sizeof(copy), expected) == ESP_OK &&
              tag_equal(expected, req->auth_tag);
    if (ok) {
        client->last_seen_ms = now_ms();
        for (int i = 0; i < SOURCE_UDP_MAX_AUTH_CLIENTS; ++i) {
            if (s_active[i].used &&
                memcmp(s_active[i].client_id, req->client_id, RAUD_CLIENT_ID_LEN) == 0) {
                if (msg_type == RAUD_MSG_LEAVE) {
                    memset(&s_active[i], 0, sizeof(s_active[i]));
                } else {
                    s_active[i].last_seen_ms = client->last_seen_ms;
                }
            }
        }
        (void)save_clients_locked();
    } else {
        s_auth_failures++;
    }
    xSemaphoreGive(s_lock);
}

static void discovery_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(RAUD_DISCOVERY_PORT);
    dest.sin_addr.s_addr = inet_addr("255.255.255.255");

    while (1) {
        if (source_wifi_is_sta_connected()) {
            raud_room_advertise_t adv = {0};
            adv.hdr.magic = RAUD_MAGIC;
            adv.hdr.version = RAUD_VERSION;
            adv.hdr.msg_type = RAUD_MSG_ROOM_ADVERTISE;
            adv.hdr.length = sizeof(adv);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(adv.source_id, s_source_id, sizeof(adv.source_id));
            memcpy(adv.room_code, s_room_code, sizeof(adv.room_code));
            snprintf(adv.room_name, sizeof(adv.room_name), "%s", s_room_name);
            refresh_pair_pin_locked();
            adv.pairing_available = 1;
            xSemaphoreGive(s_lock);
            source_udp_session_snapshot_t session = {0};
            (void)source_audio_get_udp_session(&session);
            adv.stream_id = session.stream_id;
            adv.source_ip = source_wifi_sta_ip();
            adv.control_port = RAUD_CONTROL_PORT;
            adv.sample_rate_hz = 48000;
            adv.frame_us = 8000;
            adv.frame_bytes = session.frame_bytes;
            adv.codec = RAUD_CODEC_SBC;
            adv.channels = session.channels;
            adv.bit_depth = 24;
            adv.auth_required = 1;
            const esp_app_desc_t *desc = esp_app_get_description();
            snprintf(adv.firmware_version, sizeof(adv.firmware_version), "%.15s", desc ? desc->version : "unknown");
            snprintf(adv.device_model, sizeof(adv.device_model), "ESP32-WROVER");
            if (sendto(sock, &adv, sizeof(adv), 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
                s_advertisements_sent++;
                uint32_t now = now_ms();
                if (now - s_last_discovery_log_ms >= CTRL_DISCOVERY_LOG_MS) {
                    s_last_discovery_log_ms = now;
                    ESP_LOGI(TAG, "ROOM_ADVERTISE ip=%u.%u.%u.%u control=%u",
                             (uint8_t)(ntohl(adv.source_ip) >> 24),
                             (uint8_t)(ntohl(adv.source_ip) >> 16),
                             (uint8_t)(ntohl(adv.source_ip) >> 8),
                             (uint8_t)ntohl(adv.source_ip),
                             (unsigned)RAUD_CONTROL_PORT);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void control_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(RAUD_CONTROL_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "control bind failed port=%u errno=%d", (unsigned)RAUD_CONTROL_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP control listening on 0.0.0.0:%u", (unsigned)RAUD_CONTROL_PORT);

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < (int)sizeof(raud_msg_hdr_t)) {
            continue;
        }
        raud_msg_hdr_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != RAUD_MAGIC || hdr.version != RAUD_VERSION ||
            hdr.length != len || hdr.length > sizeof(buf)) {
            continue;
        }
        switch (hdr.msg_type) {
        case RAUD_MSG_PAIR_REQUEST:
            if (len == sizeof(raud_pair_request_t)) handle_pair_request(sock, (const raud_pair_request_t *)buf, &from, from_len);
            break;
        case RAUD_MSG_JOIN_HELLO:
            if (len == sizeof(raud_join_hello_t)) handle_join_hello(sock, (const raud_join_hello_t *)buf, &from, from_len);
            break;
        case RAUD_MSG_HEARTBEAT:
        case RAUD_MSG_LEAVE:
        case RAUD_MSG_IDENTIFY:
            if (len == sizeof(raud_client_control_t)) handle_client_control((const raud_client_control_t *)buf, hdr.msg_type);
            break;
        default:
            break;
        }
    }
}

esp_err_t source_udp_control_start(void)
{
    ESP_RETURN_ON_ERROR(load_state(), TAG, "load");
    BaseType_t d = xTaskCreatePinnedToCore(discovery_task, "room_disc", 4096, NULL, 2, NULL, 0);
    BaseType_t c = xTaskCreatePinnedToCore(control_task, "udp_ctrl", 6144, NULL, 3, NULL, 0);
    return (d == pdPASS && c == pdPASS) ? ESP_OK : ESP_FAIL;
}

void source_udp_control_set_room_metadata(const char *room_code, const char *room_name)
{
    if (!ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_room_code, sizeof(s_room_code), "%s", room_code ? room_code : "A10-0001");
    snprintf(s_room_name, sizeof(s_room_name), "%s", room_name && room_name[0] ? room_name : s_room_code);
    refresh_pair_pin_locked();
    xSemaphoreGive(s_lock);
}

void source_udp_control_get_source_id(uint8_t out[RAUD_SOURCE_ID_LEN])
{
    if (!out) return;
    if (!ensure_lock()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_source_id, RAUD_SOURCE_ID_LEN);
    xSemaphoreGive(s_lock);
}

int source_udp_control_get_audio_dests(source_udp_audio_dest_t *out, int max_dests)
{
    if (!out || max_dests <= 0 || !s_lock) return 0;
    int count = 0;
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SOURCE_UDP_MAX_AUTH_CLIENTS && count < max_dests; ++i) {
        if (!s_active[i].used) continue;
        if (now - s_active[i].last_seen_ms > CTRL_ACTIVE_TIMEOUT_MS) {
            memset(&s_active[i], 0, sizeof(s_active[i]));
            continue;
        }
        out[count].addr = s_active[i].addr;
        out[count].port = s_active[i].port;
        count++;
    }
    xSemaphoreGive(s_lock);
    if (count > 0 && now - s_last_audio_target_log_ms >= CTRL_AUDIO_TARGET_LOG_MS) {
        s_last_audio_target_log_ms = now;
        ESP_LOGI(TAG, "active UDP clients count=%d", count);
    }
    return count;
}

esp_err_t source_udp_control_enable_pairing(uint32_t duration_ms)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    (void)duration_ms;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    refresh_pair_pin_locked();
    ESP_LOGI(TAG, "Pairing always on for room %s with PIN %s", s_room_code, s_pair_pin);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void source_udp_control_disable_pairing(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    refresh_pair_pin_locked();
    xSemaphoreGive(s_lock);
}

bool source_udp_control_get_pairing_pin(char out[7], uint32_t *remaining_ms)
{
    if (!out || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    refresh_pair_pin_locked();
    memcpy(out, s_pair_pin, 7);
    if (remaining_ms) *remaining_ms = UINT32_MAX;
    xSemaphoreGive(s_lock);
    return true;
}

int source_udp_control_get_authorized_clients(source_authorized_client_view_t *out, int max_clients)
{
    if (!out || max_clients <= 0 || !s_lock) return 0;
    int count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < s_store.count && i < SOURCE_UDP_MAX_AUTH_CLIENTS && count < max_clients; ++i) {
        memcpy(out[count].client_id, s_store.clients[i].client_id, RAUD_CLIENT_ID_LEN);
        snprintf(out[count].client_name, sizeof(out[count].client_name), "%s", s_store.clients[i].client_name);
        out[count].enabled = s_store.clients[i].enabled != 0;
        out[count].added_ms = s_store.clients[i].added_ms;
        out[count].last_seen_ms = s_store.clients[i].last_seen_ms;
        count++;
    }
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t source_udp_control_revoke_client(const uint8_t client_id[RAUD_CLIENT_ID_LEN])
{
    if (!client_id || !s_lock) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_client_t *client = find_client_locked(client_id);
    if (client) {
        client->enabled = 0;
        for (int i = 0; i < SOURCE_UDP_MAX_AUTH_CLIENTS; ++i) {
            if (s_active[i].used && memcmp(s_active[i].client_id, client_id, RAUD_CLIENT_ID_LEN) == 0) {
                memset(&s_active[i], 0, sizeof(s_active[i]));
            }
        }
        ret = save_clients_locked();
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void source_udp_control_get_stats(source_udp_control_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->advertisements_sent = s_advertisements_sent;
    out->pair_accepts = s_pair_accepts;
    out->pair_rejects = s_pair_rejects;
    out->join_accepts = s_join_accepts;
    out->join_rejects = s_join_rejects;
    out->auth_failures = s_auth_failures;
    source_udp_audio_dest_t dests[SOURCE_UDP_MAX_AUTH_CLIENTS];
    out->active_clients = (uint32_t)source_udp_control_get_audio_dests(dests, SOURCE_UDP_MAX_AUTH_CLIENTS);
}
