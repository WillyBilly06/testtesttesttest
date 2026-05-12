/*
 * C6 ESP-NOW bridge for WROVER ESP-NOW audio source discovery.
 *
 * The C6 runs its own Wi-Fi + ESP-NOW stack independently. It does NOT rely
 * on the P4 host to initialise Wi-Fi through ESP-Hosted RPC — that path
 * causes channel conflicts that silently kill ESP-NOW reception.
 *
 * Flow:
 *   1. On boot, register SDIO handlers and a Wi-Fi event listener.
 *   2. On WIFI_EVENT_STA_START (fired by ESP-Hosted after transport up),
 *      force channel 1, init ESP-NOW, start listening.
 *   3. espnow_recv_cb runs continuously — every valid beacon is stored
 *      locally AND forwarded to the P4 as ROOM_FOUND immediately.
 *   4. CMD_SCAN: force channel, clear room list, wait SCAN_LISTEN_MS,
 *      then send SCAN_DONE with the count of rooms found during that window.
 *   5. CMD_JOIN: send authenticated HELLO to the Source, wait for ACCEPT.
 *   6. On ACCEPT: decrypt audio key, forward AUDIO_KEY + AUDIO_CONFIG to P4.
 *   7. Forward validated encrypted LC3 ESP-NOW audio packets to the P4.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/aes.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/sha256.h"

#include "esp_hosted_peer_data.h"
#include "espnow_protocol.h"
#include "espnow_sink_c6.h"

static const char *TAG = "room_c6";

/* ───── Constants ───── */

#define ESPNOW_CHANNEL      1
#define MAX_ROOMS            ESPNOW_MAX_ROOMS
#define ROOM_ID_LEN          8
#define PROTO_MAGIC          0x44554152u
#define PROTO_VERSION        1
#define MSG_BEACON           1
#define MSG_HELLO            2
#define MSG_ACCEPT           3
#define MSG_AUDIO            4
#define JOIN_TIMEOUT_MS      2000

/* ───── Shared secret (must match Source protocol.h MASTER_KEY) ───── */

static const uint8_t MASTER_KEY[32] = {
    0x93, 0x11, 0x75, 0x32, 0xd8, 0x4f, 0x26, 0x09,
    0xa5, 0x6b, 0xcd, 0x18, 0x74, 0xee, 0x20, 0x91,
    0x4c, 0x65, 0x2b, 0xaf, 0x8e, 0x39, 0x50, 0x7d,
    0x16, 0xb0, 0xf4, 0x43, 0x9a, 0xc7, 0x0e, 0x58,
};

/* ───── Wire structs (must match Source protocol.h exactly) ───── */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    char     room[9];
    uint8_t  source_mac[6];
    uint16_t lc3_dt_us;
    uint16_t sample_rate_hz;
    uint16_t bitrate_kbps;
    uint8_t  frame_bytes;
    uint8_t  flags;
    uint32_t stream_id;
} beacon_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    char     room[9];
    uint8_t  sink_nonce[16];
    uint8_t  auth[16];
} hello_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    char     room[9];
    uint8_t  sink_nonce[16];
    uint8_t  source_nonce[16];
    uint8_t  key_nonce[16];
    uint8_t  encrypted_audio_key[32];
    uint32_t stream_id;
    uint8_t  frame_bytes;
    uint8_t  auth[16];
} accept_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  room_hash;
    uint8_t  flags;
    uint32_t stream_id;
    uint32_t seq;
    uint32_t capture_us;
    uint32_t payload_crc32;
    uint8_t  copy_idx;
    uint8_t  copy_count;
    uint8_t  frame_bytes;
    uint8_t  channels;
    uint8_t  payload[ESPNOW_AUDIO_MAX_FRAME_BYTES];
} audio_msg_t;

_Static_assert(ESPNOW_LC3_FRAME_US == 7500, "C6 ESP-NOW LC3 frame duration must be 7.5 ms");
_Static_assert(ESPNOW_SAMPLE_RATE_HZ == 48000, "C6 ESP-NOW sample rate must be 48 kHz");
_Static_assert(ESPNOW_CHANNELS == 2, "C6 ESP-NOW audio must be stereo");
_Static_assert(ESPNOW_LC3_BYTES_PER_CH == 72, "C6 ESP-NOW LC3 channel payload must be 72 bytes");
_Static_assert(ESPNOW_LC3_FRAME_BYTES == 144, "C6 ESP-NOW stereo payload must be 144 bytes");
_Static_assert(ESPNOW_AUDIO_COPY_DEFAULT == 4, "C6 ESP-NOW audio must use four copies");
_Static_assert(sizeof(((audio_msg_t *)0)->payload) == ESPNOW_LC3_FRAME_BYTES,
               "C6 audio_msg_t payload must carry one full LC3 stereo frame");

/* ───── Room list ───── */

typedef struct {
    beacon_msg_t beacon;
    int64_t      last_seen_us;
    int8_t       rssi;
    uint32_t     room_code;
    uint32_t     stream_id;
} room_entry_t;

static room_entry_t  s_rooms[MAX_ROOMS];
static portMUX_TYPE  s_room_lock = portMUX_INITIALIZER_UNLOCKED;

/* ───── State ───── */

static volatile espnow_state_t s_state        = ESPNOW_STATE_NOT_INIT;
static volatile bool           s_espnow_inited = false;
static volatile bool           s_waiting_accept = false;
static TaskHandle_t            s_scan_done_task  = NULL;
static TaskHandle_t            s_join_timeout_task = NULL;

static beacon_msg_t s_selected_beacon;
static uint32_t     s_selected_room_code;
static uint32_t     s_selected_stream_id;
static uint8_t      s_selected_room_hash;
static uint8_t      s_selected_sink_nonce[16];
static uint8_t      s_audio_key[ESPNOW_AUDIO_KEY_LEN];
static int8_t       s_last_rssi;
static uint32_t     s_sdio_errors;
static volatile uint32_t s_espnow_rx_total;
static volatile uint32_t s_espnow_beacons;
static volatile uint32_t s_espnow_audio;
static volatile uint32_t s_audio_late_or_dup;
static uint32_t s_last_forwarded_seq;
static bool s_have_forwarded_seq;

/* Forward declaration */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);

/* ───── SDIO helpers ───── */

static void sdio_send(uint32_t msg_id, const void *data, size_t len)
{
    esp_err_t err = esp_hosted_send_custom_data(msg_id, (const uint8_t *)data, len);
    if (err != ESP_OK) s_sdio_errors++;
}

/* ───── Crypto helpers ───── */

static void random_bytes(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = esp_random();
        size_t n = ((len - i) < 4) ? (len - i) : 4;
        memcpy(out + i, &r, n);
    }
}

static esp_err_t aes_ctr_crypt(const uint8_t key[32], const uint8_t nonce[16],
                               const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context ctx;
    uint8_t nc[16], stream[16] = {0};
    size_t offset = 0;
    memcpy(nc, nonce, 16);
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0)
        ret = mbedtls_aes_crypt_ctr(&ctx, len, &offset, nc, stream, in, out);
    mbedtls_aes_free(&ctx);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t aes_cmac_16(const uint8_t *data, size_t len, uint8_t out[16])
{
    const mbedtls_cipher_info_t *info =
        mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_ECB);
    if (!info) return ESP_FAIL;
    return (mbedtls_cipher_cmac(info, MASTER_KEY, 256, data, len, out) == 0)
           ? ESP_OK : ESP_FAIL;
}

static uint32_t crc32_audio(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool verify_accept(const accept_msg_t *msg)
{
    accept_msg_t tmp = *msg;
    uint8_t expected[16];
    memset(tmp.auth, 0, sizeof(tmp.auth));
    if (aes_cmac_16((const uint8_t *)&tmp, sizeof(tmp), expected) != ESP_OK)
        return false;
    return memcmp(expected, msg->auth, 16) == 0;
}

/* ───── Room helpers ───── */

static void copy_room_name(char out[ESPNOW_ROOM_NAME_LEN], const char in[9])
{
    memset(out, 0, ESPNOW_ROOM_NAME_LEN);
    for (int i = 0; i < ESPNOW_ROOM_NAME_LEN - 1 && i < 9 && in[i]; i++)
        out[i] = in[i];
}

static uint32_t hash_room_id(const beacon_msg_t *b)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) { h ^= (uint8_t)b->room[i]; h *= 16777619u; }
    for (int i = 0; i < 6; i++) { h ^= b->source_mac[i];     h *= 16777619u; }
    return h ? h : 1u;
}

static void derive_room_hash(const char *room, uint8_t *hash)
{
    uint8_t dig[32], in[32] = {0};
    snprintf((char *)in, sizeof(in), "espnow:%.8s", room);
    mbedtls_sha256(in, strlen((char *)in), dig, 0);
    *hash = dig[0] ^ dig[1] ^ dig[2] ^ dig[3];
}

static uint32_t stream_id_from_beacon(const beacon_msg_t *b)
{
    if (b->stream_id != 0) {
        return b->stream_id;
    }
    uint32_t h = hash_room_id(b) ^ 0x9e3779b9u;
    return h ? h : 1u;
}

static uint8_t room_count_locked(void)
{
    uint8_t c = 0;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (s_rooms[i].last_seen_us) c++;
    return c;
}

/* ───── SDIO event senders ───── */

static void send_status(espnow_state_t state)
{
    espnow_evt_status_t evt = {
        .state = (uint8_t)state,
        .wifi_init = 1,
        .espnow_init = s_espnow_inited ? 1 : 0,
        .reserved = 0,
    };
    sdio_send(ESPNOW_MSG_EVT_STATUS, &evt, sizeof(evt));
}

static void send_error(esp_err_t code, const char *msg)
{
    espnow_evt_error_t evt = { .error_code = (int32_t)code };
    snprintf(evt.message, sizeof(evt.message), "%s", msg ? msg : "error");
    sdio_send(ESPNOW_MSG_EVT_ERROR, &evt, sizeof(evt));
}

static void send_room_found(const room_entry_t *entry)
{
    espnow_evt_room_t evt = {0};
    memcpy(evt.mac, entry->beacon.source_mac, sizeof(evt.mac));
    evt.wifi_channel = ESPNOW_CHANNEL;
    evt.room_code    = entry->room_code;
    evt.stream_id    = entry->stream_id;
    evt.rssi         = entry->rssi;
    copy_room_name(evt.name, entry->beacon.room);
    evt.frame_bytes  = entry->beacon.frame_bytes;
    sdio_send(ESPNOW_MSG_EVT_ROOM_FOUND, &evt, sizeof(evt));
}

static void send_scan_done(void)
{
    espnow_evt_scan_done_t evt = {0};
    portENTER_CRITICAL(&s_room_lock);
    evt.room_count = room_count_locked();
    portEXIT_CRITICAL(&s_room_lock);
    sdio_send(ESPNOW_MSG_EVT_SCAN_DONE, &evt, sizeof(evt));
}

/* ───── ESP-NOW init / deinit ─────
 *
 * Key insight: the P4 host's Wi-Fi RPCs (scan, connect) internally restart
 * the C6's STA, which silently destroys ESP-NOW state even though our
 * s_espnow_inited flag is still true.  To be robust we do a full
 * deinit → init cycle on every scan and every join.
 */

static void espnow_full_deinit(void)
{
    if (s_espnow_inited) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        s_espnow_inited = false;
    }
}

static esp_err_t espnow_full_init(void)
{
    espnow_full_deinit();

    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set_ps: %s", esp_err_to_name(err));

    err = esp_wifi_set_protocol(WIFI_IF_STA,
                                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set_protocol: %s", esp_err_to_name(err));

    err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel(%d): %s", ESPNOW_CHANNEL, esp_err_to_name(err));
        return err;
    }

    err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_INTERNAL) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return err;
    }

    esp_now_set_pmk(MASTER_KEY);

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_recv_cb: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    esp_now_peer_info_t bcast = {0};
    memset(bcast.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    bcast.channel = ESPNOW_CHANNEL;
    bcast.ifidx   = WIFI_IF_STA;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);

    s_espnow_inited = true;

    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "ESP-NOW ready on channel %u (actual=%u)", ESPNOW_CHANNEL, ch);
    return ESP_OK;
}

/* ───── Beacon processing ───── */

static bool beacon_supported(const beacon_msg_t *b)
{
    return b->magic == PROTO_MAGIC &&
           b->version == PROTO_VERSION &&
           b->type == MSG_BEACON &&
           b->lc3_dt_us == ESPNOW_LC3_FRAME_US &&
           b->sample_rate_hz == ESPNOW_SAMPLE_RATE_HZ &&
           b->frame_bytes == ESPNOW_LC3_FRAME_BYTES &&
           (b->flags & 0x01);
}

static void add_or_update_room(const beacon_msg_t *beacon, int8_t rssi)
{
    if (!beacon_supported(beacon)) {
        ESP_LOGW(TAG, "Rejected beacon: magic=0x%08lx ver=%u type=%u dt=%u rate=%u fb=%u",
                 (unsigned long)beacon->magic, beacon->version, beacon->type,
                 beacon->lc3_dt_us, beacon->sample_rate_hz, beacon->frame_bytes);
        return;
    }

    room_entry_t snapshot = {0};
    int slot = -1, empty = -1, oldest_i = 0;
    int64_t oldest = INT64_MAX, now = esp_timer_get_time();

    portENTER_CRITICAL(&s_room_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (s_rooms[i].last_seen_us == 0 && empty < 0) empty = i;
        if (s_rooms[i].last_seen_us != 0 &&
            memcmp(s_rooms[i].beacon.source_mac, beacon->source_mac, 6) == 0 &&
            strncmp(s_rooms[i].beacon.room, beacon->room, ROOM_ID_LEN) == 0) {
            slot = i;
            break;
        }
        if (s_rooms[i].last_seen_us < oldest) { oldest = s_rooms[i].last_seen_us; oldest_i = i; }
    }
    if (slot < 0) slot = (empty >= 0) ? empty : oldest_i;

    s_rooms[slot].beacon      = *beacon;
    s_rooms[slot].last_seen_us = now;
    s_rooms[slot].rssi         = rssi;
    s_rooms[slot].room_code    = hash_room_id(beacon);
    s_rooms[slot].stream_id    = stream_id_from_beacon(beacon);
    snapshot = s_rooms[slot];
    portEXIT_CRITICAL(&s_room_lock);

    ESP_LOGI(TAG, "Room found: %.8s rssi=%d mac=" MACSTR " dt=%u fb=%u",
             beacon->room, rssi, MAC2STR(beacon->source_mac),
             beacon->lc3_dt_us, beacon->frame_bytes);
    send_room_found(&snapshot);
}

/* ───── ESP-NOW receive callback ───── */

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    s_espnow_rx_total++;

    if (info && info->rx_ctrl)
        s_last_rssi = info->rx_ctrl->rssi;

    if (!data || len < 6) {
        ESP_LOGW(TAG, "ESP-NOW RX: too short len=%d", len);
        return;
    }

    uint32_t magic = 0;
    memcpy(&magic, data, 4);

    if (magic != PROTO_MAGIC || data[4] != PROTO_VERSION)
        return;

    if (data[5] == MSG_BEACON && len == (int)sizeof(beacon_msg_t)) {
        ESP_LOGD(TAG, "ESP-NOW beacon #%lu rssi=%d",
                 (unsigned long)s_espnow_rx_total, s_last_rssi);
        s_espnow_beacons++;
        beacon_msg_t beacon;
        memcpy(&beacon, data, sizeof(beacon));
        add_or_update_room(&beacon, s_last_rssi);
    } else if (data[5] == MSG_ACCEPT && len == (int)sizeof(accept_msg_t) && info) {
        accept_msg_t accept;
        memcpy(&accept, data, sizeof(accept));
        /* handle_accept inline */
        if (!s_waiting_accept) return;
        if (memcmp(info->src_addr, s_selected_beacon.source_mac, 6) != 0) return;
        if (strncmp(accept.room, s_selected_beacon.room, ROOM_ID_LEN) != 0) return;
        if (memcmp(accept.sink_nonce, s_selected_sink_nonce, sizeof(s_selected_sink_nonce)) != 0) {
            ESP_LOGW(TAG, "ACCEPT nonce mismatch");
            return;
        }
        if (!verify_accept(&accept)) {
            ESP_LOGW(TAG, "ACCEPT bad CMAC");
            return;
        }
        if (accept.stream_id == 0) {
            ESP_LOGW(TAG, "ACCEPT missing stream_id");
            return;
        }
        if (accept.stream_id != s_selected_stream_id) {
            ESP_LOGW(TAG, "ACCEPT stream mismatch got=%" PRIu32 " expected=%" PRIu32,
                     accept.stream_id, s_selected_stream_id);
            return;
        }
        if (accept.frame_bytes != ESPNOW_LC3_FRAME_BYTES) {
            ESP_LOGW(TAG, "ACCEPT frame size mismatch got=%u expected=%u",
                     accept.frame_bytes, ESPNOW_LC3_FRAME_BYTES);
            return;
        }
        if (aes_ctr_crypt(MASTER_KEY, accept.key_nonce,
                          accept.encrypted_audio_key, s_audio_key, 32) != ESP_OK) {
            send_error(ESP_FAIL, "Key decrypt failed");
            return;
        }

        s_waiting_accept = false;
        s_state = ESPNOW_STATE_CONNECTED;
        s_selected_stream_id = accept.stream_id;

        espnow_evt_joined_t joined = {0};
        memcpy(joined.mac, s_selected_beacon.source_mac, 6);
        joined.wifi_channel = ESPNOW_CHANNEL;
        joined.room_code = s_selected_room_code;
        joined.stream_id = s_selected_stream_id;
        sdio_send(ESPNOW_MSG_EVT_JOINED, &joined, sizeof(joined));

        espnow_evt_audio_config_t cfg = {0};
        cfg.audio_copy_count = ESPNOW_AUDIO_COPY_DEFAULT;
        cfg.frame_bytes = ESPNOW_LC3_FRAME_BYTES;
        cfg.channels = ESPNOW_CHANNELS;
        cfg.lc3_dt_us = ESPNOW_LC3_FRAME_US;
        cfg.sample_rate_hz = ESPNOW_SAMPLE_RATE_HZ;
        sdio_send(ESPNOW_MSG_EVT_AUDIO_CONFIG, &cfg, sizeof(cfg));

        espnow_evt_audio_key_t audio_key = {0};
        memcpy(audio_key.audio_key, s_audio_key, ESPNOW_AUDIO_KEY_LEN);
        audio_key.frame_bytes = ESPNOW_LC3_FRAME_BYTES;
        audio_key.channels = ESPNOW_CHANNELS;
        audio_key.lc3_dt_us = ESPNOW_LC3_FRAME_US;
        audio_key.sample_rate_hz = ESPNOW_SAMPLE_RATE_HZ;
        sdio_send(ESPNOW_MSG_EVT_AUDIO_KEY, &audio_key, sizeof(audio_key));

        s_have_forwarded_seq = false;
        ESP_LOGI(TAG, "ACCEPT OK: room=%.8s ESP-NOW LC3 key installed", accept.room);
        send_status(s_state);
    } else if (data[5] == MSG_AUDIO && info &&
               len == (int)(offsetof(audio_msg_t, payload) + ESPNOW_LC3_FRAME_BYTES)) {
        if (s_state != ESPNOW_STATE_CONNECTED) return;
        if (memcmp(info->src_addr, s_selected_beacon.source_mac, 6) != 0) return;

        audio_msg_t audio;
        memcpy(&audio, data, sizeof(audio));
        if (audio.magic != PROTO_MAGIC ||
            audio.version != PROTO_VERSION ||
            audio.type != MSG_AUDIO ||
            audio.frame_bytes != ESPNOW_LC3_FRAME_BYTES ||
            audio.channels != ESPNOW_CHANNELS ||
            audio.room_hash != s_selected_room_hash ||
            audio.stream_id != s_selected_stream_id ||
            audio.copy_count != ESPNOW_AUDIO_COPY_DEFAULT ||
            audio.copy_idx >= ESPNOW_AUDIO_COPY_DEFAULT) {
            return;
        }
        if (crc32_audio(audio.payload, ESPNOW_LC3_FRAME_BYTES) != audio.payload_crc32) {
            return;
        }
        espnow_evt_audio_raw_t evt = {0};
        evt.seq = audio.seq;
        evt.capture_us = audio.capture_us;
        evt.stream_id = audio.stream_id;
        evt.payload_crc32 = audio.payload_crc32;
        evt.copy_idx = audio.copy_idx;
        evt.copy_count = audio.copy_count;
        evt.frame_bytes = ESPNOW_LC3_FRAME_BYTES;
        evt.channels = ESPNOW_CHANNELS;
        evt.lc3_dt_us = ESPNOW_LC3_FRAME_US;
        evt.sample_rate_hz = ESPNOW_SAMPLE_RATE_HZ;
        memcpy(evt.payload, audio.payload, ESPNOW_LC3_FRAME_BYTES);
        sdio_send(ESPNOW_MSG_EVT_AUDIO, &evt, sizeof(evt));
        s_last_forwarded_seq = audio.seq;
        s_have_forwarded_seq = true;
        s_espnow_audio++;
    }
}

/* ───── Scan done timer task ───── */

static void scan_done_task_fn(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(ESPNOW_SCAN_LISTEN_MS));
    if (s_state == ESPNOW_STATE_SCANNING) {
        s_state = ESPNOW_STATE_IDLE;
        send_scan_done();
        send_status(s_state);
    }
    s_scan_done_task = NULL;
    vTaskDelete(NULL);
}

/* ───── Join timeout task ───── */

static void join_timeout_task_fn(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(JOIN_TIMEOUT_MS));
    if (s_waiting_accept) {
        s_waiting_accept = false;
        s_state = ESPNOW_STATE_IDLE;
        send_error(ESP_ERR_TIMEOUT, "Room auth timed out");
        send_status(s_state);
    }
    s_join_timeout_task = NULL;
    vTaskDelete(NULL);
}

/* ───── SDIO command handlers ───── */

static void cmd_fw_ver_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;
    espnow_evt_fw_ver_t fw = {0};
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) {
        snprintf(fw.version, sizeof(fw.version), "%s", d->version);
        snprintf(fw.project, sizeof(fw.project), "%s", d->project_name);
    }
    sdio_send(ESPNOW_MSG_EVT_FW_VER, &fw, sizeof(fw));
}

static void cmd_get_status_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;
    send_status(s_state);
}

static void cmd_init_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;
    ESP_LOGI(TAG, "CMD_INIT: full ESP-NOW reinit");
    esp_err_t err = espnow_full_init();
    if (err == ESP_OK) {
        s_state = ESPNOW_STATE_IDLE;
    } else {
        send_error(err, "ESP-NOW init failed");
    }
    send_status(s_state);
}

static void cmd_deinit_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;
    s_waiting_accept = false;
    espnow_full_deinit();
    s_state = ESPNOW_STATE_NOT_INIT;
    send_status(s_state);
}

static void cmd_scan_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;

    /* Full reinit to guarantee clean ESP-NOW state and correct channel */
    esp_err_t err = espnow_full_init();
    if (err != ESP_OK) {
        send_error(ESP_ERR_INVALID_STATE, "ESP-NOW reinit failed for scan");
        return;
    }

    portENTER_CRITICAL(&s_room_lock);
    memset(s_rooms, 0, sizeof(s_rooms));
    portEXIT_CRITICAL(&s_room_lock);

    s_state = ESPNOW_STATE_SCANNING;
    send_status(s_state);
    ESP_LOGI(TAG, "Scanning for rooms (%d ms)...", ESPNOW_SCAN_LISTEN_MS);

    if (s_scan_done_task == NULL)
        xTaskCreate(scan_done_task_fn, "scan_done", 3072, NULL, 3, &s_scan_done_task);
}

static void cmd_stop_scan_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;
    if (s_state == ESPNOW_STATE_SCANNING) {
        s_state = ESPNOW_STATE_IDLE;
        send_scan_done();
        send_status(s_state);
    }
}

static void cmd_join_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)ctx;
    if (!data || len < sizeof(espnow_cmd_join_t)) return;

    /* Reinit ESP-NOW to ensure channel is correct */
    if (espnow_full_init() != ESP_OK) {
        send_error(ESP_ERR_INVALID_STATE, "ESP-NOW reinit failed for join");
        return;
    }

    const espnow_cmd_join_t *cmd = (const espnow_cmd_join_t *)data;
    room_entry_t room = {0};
    bool found = false;

    portENTER_CRITICAL(&s_room_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (s_rooms[i].last_seen_us == 0) continue;
        if (memcmp(s_rooms[i].beacon.source_mac, cmd->mac, 6) == 0 ||
            s_rooms[i].stream_id == cmd->stream_id ||
            s_rooms[i].room_code == cmd->room_code) {
            room = s_rooms[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_room_lock);

    if (!found) {
        send_error(ESP_ERR_NOT_FOUND, "Room no longer visible");
        return;
    }

    /* Build and send HELLO */
    hello_msg_t hello = {0};
    hello.magic   = PROTO_MAGIC;
    hello.version = PROTO_VERSION;
    hello.type    = MSG_HELLO;
    copy_room_name(hello.room, room.beacon.room);
    random_bytes(hello.sink_nonce, sizeof(hello.sink_nonce));
    if (aes_cmac_16((const uint8_t *)&hello, sizeof(hello), hello.auth) != ESP_OK) {
        send_error(ESP_FAIL, "HELLO CMAC failed");
        return;
    }

    if (!esp_now_is_peer_exist(room.beacon.source_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, room.beacon.source_mac, ESP_NOW_ETH_ALEN);
        peer.channel = ESPNOW_CHANNEL;
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    s_selected_beacon    = room.beacon;
    s_selected_room_code = room.room_code;
    s_selected_stream_id = room.stream_id;
    derive_room_hash(room.beacon.room, &s_selected_room_hash);
    memcpy(s_selected_sink_nonce, hello.sink_nonce, sizeof(s_selected_sink_nonce));
    s_waiting_accept     = true;
    s_state = ESPNOW_STATE_JOINING;
    send_status(s_state);

    esp_err_t err = esp_now_send(room.beacon.source_mac, (const uint8_t *)&hello, sizeof(hello));
    ESP_LOGI(TAG, "HELLO → %.8s " MACSTR ": %s",
             room.beacon.room, MAC2STR(room.beacon.source_mac), esp_err_to_name(err));

    if (err != ESP_OK) {
        s_waiting_accept = false;
        s_state = ESPNOW_STATE_IDLE;
        send_error(err, "HELLO send failed");
        send_status(s_state);
    } else if (s_join_timeout_task == NULL) {
        xTaskCreate(join_timeout_task_fn, "join_to", 3072, NULL, 3, &s_join_timeout_task);
    }
}

static void cmd_leave_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)data; (void)len; (void)ctx;
    s_waiting_accept = false;
    s_state = s_espnow_inited ? ESPNOW_STATE_IDLE : ESPNOW_STATE_NOT_INIT;
    sdio_send(ESPNOW_MSG_EVT_LEFT, NULL, 0);
    send_status(s_state);
}

/* ───── Wi-Fi event handler ───── */

static void wifi_evt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started — init ESP-NOW");
        espnow_full_init();
        if (s_state == ESPNOW_STATE_NOT_INIT)
            s_state = ESPNOW_STATE_IDLE;
    } else if (id == WIFI_EVENT_STA_STOP || id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "Wi-Fi STA stopped — ESP-NOW invalidated");
        s_espnow_inited = false;
        s_state = ESPNOW_STATE_NOT_INIT;
    }
}

/* ───── Stats task ───── */

static void stats_task_fn(void *arg)
{
    (void)arg;
    while (1) {
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);

        espnow_evt_stats_t st = {
            .packets_rx = s_espnow_audio,
            .packets_lost = s_audio_late_or_dup,
            .plc_frames = 0,
            .rssi_last = s_last_rssi,
            .wifi_channel = ch,
            .sdio_send_errors = (uint8_t)(s_sdio_errors > 255 ? 255 : s_sdio_errors),
            .reserved = 0,
        };
        sdio_send(ESPNOW_MSG_EVT_STATS, &st, sizeof(st));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ───── Public init ───── */

esp_err_t espnow_sink_c6_init(void)
{
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_GET_FW_VER,  cmd_fw_ver_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_GET_STATUS,  cmd_get_status_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_INIT,        cmd_init_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_DEINIT,      cmd_deinit_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_SCAN,        cmd_scan_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_STOP_SCAN,   cmd_stop_scan_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_JOIN,        cmd_join_cb, NULL);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_LEAVE,       cmd_leave_cb, NULL);

    esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt_handler, NULL);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "WIFI_EVENT register failed: %s", esp_err_to_name(err));

    xTaskCreate(stats_task_fn, "room_stats", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Room bridge registered; waiting for Wi-Fi STA start");
    return ESP_OK;
}
