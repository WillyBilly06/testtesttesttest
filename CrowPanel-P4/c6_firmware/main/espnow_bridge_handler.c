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
 *      init ESP-NOW, start listening.
 *   3. espnow_recv_cb runs continuously — every valid beacon is stored
 *      locally AND forwarded to the P4 as ROOM_FOUND immediately.
 *   4. CMD_SCAN: scan common AP channels, clear room list, wait
 *      SCAN_LISTEN_MS, then send SCAN_DONE with rooms found during that window.
 *   5. CMD_JOIN: send authenticated HELLO to the Source, wait for ACCEPT.
 *   6. On ACCEPT: decrypt audio key, forward AUDIO_KEY + AUDIO_CONFIG to P4.
 *   7. Forward validated encrypted SBC ESP-NOW audio packets to the P4.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
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
#include "sdio_slave_api.h"

static const char *TAG = "room_c6";

/* ───── Constants ───── */

#define ESPNOW_DEFAULT_CHANNEL 1
#define ESPNOW_SCAN_CHANNELS 3
#define MAX_ROOMS            ESPNOW_MAX_ROOMS
#define ROOM_ID_LEN          8
#define PROTO_MAGIC          0x44554152u
#define PROTO_VERSION        1
#define MSG_BEACON           1
#define MSG_HELLO            2
#define MSG_ACCEPT           3
#define MSG_AUDIO            4
#define JOIN_TIMEOUT_MS      2000
#define AUDIO_DEDUPE_SLOTS   64
#define AUDIO_FORWARD_QUEUE_DEPTH 24
#define MAX_WIFI_TX_POWER_QDBM 84
#define ESPNOW_RANGE_PHY_MODE WIFI_PHY_MODE_11B
#define ESPNOW_RANGE_PHY_RATE WIFI_PHY_RATE_5M_L

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
    uint16_t frame_bytes;
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
    uint16_t frame_bytes;
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
    uint16_t frame_bytes;
    uint8_t  channels;
    uint8_t  payload[ESPNOW_AUDIO_MAX_FRAME_BYTES];
} audio_msg_t;

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
    uint16_t frame_bytes;
    uint8_t  channels;
} audio_hdr_t;

_Static_assert(ESPNOW_LC3_FRAME_US == 8000, "C6 ESP-NOW SBC packet duration must be 8 ms");
_Static_assert(ESPNOW_SAMPLE_RATE_HZ == 48000, "C6 ESP-NOW sample rate must be 48 kHz");
_Static_assert(ESPNOW_CHANNELS == 2, "C6 ESP-NOW audio must be stereo");
_Static_assert(ESPNOW_SAMPLES_PER_FRAME == 384, "C6 ESP-NOW SBC payload must represent 384 samples");
_Static_assert(ESPNOW_LC3_FRAME_BYTES == 249, "C6 ESP-NOW SBC payload must be 249 bytes");
_Static_assert(ESPNOW_AUDIO_COPY_DEFAULT == 4, "C6 ESP-NOW audio must use four copies");
_Static_assert(sizeof(audio_hdr_t) == offsetof(audio_msg_t, payload),
               "C6 audio header must match audio_msg_t payload offset");
_Static_assert(sizeof(((audio_msg_t *)0)->payload) == ESPNOW_LC3_FRAME_BYTES,
               "C6 audio_msg_t payload must carry one SBC audio packet");

/* ───── Room list ───── */

typedef struct {
    beacon_msg_t beacon;
    int64_t      last_seen_us;
    int8_t       rssi;
    uint8_t      wifi_channel;
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
static uint8_t      s_selected_channel = ESPNOW_DEFAULT_CHANNEL;
static uint8_t      s_current_channel = ESPNOW_DEFAULT_CHANNEL;
static uint8_t      s_selected_sink_nonce[16];
static uint8_t      s_audio_key[ESPNOW_AUDIO_KEY_LEN];
static int8_t       s_last_rssi;
static uint32_t     s_sdio_errors;
static volatile uint32_t s_espnow_rx_total;
static volatile uint32_t s_espnow_beacons;
static volatile uint32_t s_espnow_audio;
static volatile uint32_t s_audio_late_or_dup;
static volatile uint32_t s_audio_crc_fail;
static volatile uint32_t s_audio_header_drop;
static volatile uint32_t s_audio_forward_fail;
static volatile uint32_t s_audio_copy_hist[ESPNOW_AUDIO_COPY_DEFAULT];
/* Worst-case esp_hosted_send_custom_data() return time in the current
 * stats reporting window.
 *
 * IMPORTANT: this is NOT the wall-clock time the data spends on the
 * SDIO bus. esp_hosted_send_custom_data is asynchronous — it mallocs,
 * memcpys, and pushes to an internal req_queue, then returns. The
 * actual SDIO transfer is performed later by pserial_task (and the
 * SDIO peripheral itself) at CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY.
 *
 * What this metric DOES catch:
 *   - heap_caps_malloc stalls inside the send path (if mempool is off)
 *   - the req_queue filling up (xQueueSend on req_queue uses
 *     portMAX_DELAY, so it blocks the caller when pserial_task is
 *     starved and can't drain). A growing max here means pserial_task
 *     is being preempted long enough to back up.
 *
 * Updated by sdio_send() without a lock; stats task reads-then-zeroes
 * it once per emit. A torn read just produces a slightly stale max,
 * which is fine for telemetry.
 */
static volatile uint32_t s_sdio_enq_max_us;
static uint32_t s_forwarded_seq[AUDIO_DEDUPE_SLOTS];
static bool s_forwarded_seq_valid[AUDIO_DEDUPE_SLOTS];
static const uint8_t s_scan_channels[ESPNOW_SCAN_CHANNELS] = {1, 6, 11};
static TaskHandle_t s_audio_forward_task;
static QueueHandle_t s_audio_forward_q;
static StaticQueue_t s_audio_forward_q_struct;
static uint8_t s_audio_forward_q_storage[AUDIO_FORWARD_QUEUE_DEPTH * sizeof(espnow_evt_audio_raw_t)];

/* Forward declaration */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);

static void apply_peer_rate_config(const uint8_t peer_addr[ESP_NOW_ETH_ALEN], const char *label)
{
    esp_now_rate_config_t rcfg = {
        .phymode = ESPNOW_RANGE_PHY_MODE,
        .rate    = ESPNOW_RANGE_PHY_RATE,
        .ersu    = false,
        .dcm     = false,
    };
    esp_err_t err = esp_now_set_peer_rate_config(peer_addr, &rcfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set peer rate %s: %s", label ? label : "peer", esp_err_to_name(err));
    }
}

/* ───── SDIO helpers ───── */

/* sdio_send + audio_forward_task_fn live in IRAM so the audio forward
 * path keeps running even when the rest of the C6 has its instruction
 * cache disabled (NVS commits, OTA flash writes, app-CPU flash erase,
 * etc.). During a cache disable any code that lives in flash stalls
 * for tens of milliseconds — and that single event is enough to drop
 * a whole burst of audio packets and show up as a 30-50 ms `spread`
 * window on the P4 glitch log. Putting these two functions in IRAM
 * costs ~600 B of IRAM and removes that entire failure mode. */
static IRAM_ATTR esp_err_t sdio_send(uint32_t msg_id, const void *data, size_t len)
{
    /* Time the call so the P4 can see how long the enqueue path took.
     * See the comment on s_sdio_enq_max_us — this is the time INCLUDING
     * mallocs and req_queue blocking, NOT the on-wire SDIO time. */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_hosted_send_custom_data(msg_id, (const uint8_t *)data, len);
    uint32_t dt_us = (uint32_t)(esp_timer_get_time() - t0);
    if (dt_us > s_sdio_enq_max_us) s_sdio_enq_max_us = dt_us;
    if (err != ESP_OK) s_sdio_errors++;
    return err;
}

static IRAM_ATTR void audio_forward_task_fn(void *arg)
{
    (void)arg;
    espnow_evt_audio_raw_t evt;

    while (1) {
        if (xQueueReceive(s_audio_forward_q, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (s_state != ESPNOW_STATE_CONNECTED ||
            evt.stream_id != s_selected_stream_id) {
            continue;
        }

        /* Stamp the C6's clock just before we hand the packet to the
         * SDIO/RPC pipeline. The P4 uses (p4_now - c6_send_us) spread
         * within a 5 s window to isolate C6→P4 transport jitter from
         * source-side / air-side jitter. */
        evt.c6_send_us = (uint32_t)esp_timer_get_time();

        if (sdio_send(ESPNOW_MSG_EVT_AUDIO, &evt, sizeof(evt)) != ESP_OK) {
            s_audio_forward_fail++;
        }
    }
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
    evt.wifi_channel = entry->wifi_channel;
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

static void audio_dedupe_reset(void)
{
    memset(s_forwarded_seq_valid, 0, sizeof(s_forwarded_seq_valid));
    memset((void *)s_audio_copy_hist, 0, sizeof(s_audio_copy_hist));
    s_audio_late_or_dup = 0;
    s_audio_crc_fail = 0;
    s_audio_header_drop = 0;
    s_audio_forward_fail = 0;
}

static bool audio_already_forwarded(uint32_t seq)
{
    uint32_t slot = seq & (AUDIO_DEDUPE_SLOTS - 1);
    return s_forwarded_seq_valid[slot] && s_forwarded_seq[slot] == seq;
}

static void audio_mark_forwarded(uint32_t seq)
{
    uint32_t slot = seq & (AUDIO_DEDUPE_SLOTS - 1);
    s_forwarded_seq[slot] = seq;
    s_forwarded_seq_valid[slot] = true;
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

/* Lightweight channel hop: keeps ESP-NOW state, broadcast peer registration
 * and the RX callback all alive — we only retune the radio. Saves ~30-80 ms
 * of `esp_now_deinit` + `esp_now_init` + `esp_now_register_recv_cb` +
 * `esp_now_add_peer` overhead per channel switch, which is critical during
 * scan: with 1200 ms slices and full reinit eating ~50 ms each, we were
 * effectively listening for only ~1150 ms per channel. With set_channel-
 * only we get the full 1200 ms of pure RX time per slice. */
static esp_err_t espnow_set_channel_only(uint8_t channel)
{
    if (channel == 0) channel = ESPNOW_DEFAULT_CHANNEL;
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel(%u): %s", channel, esp_err_to_name(err));
        return err;
    }
    s_current_channel = channel;
    return ESP_OK;
}

static esp_err_t espnow_full_init(uint8_t channel)
{
    espnow_full_deinit();

    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set_ps: %s", esp_err_to_name(err));

    err = esp_wifi_set_max_tx_power(MAX_WIFI_TX_POWER_QDBM);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set_max_tx_power(%d): %s", MAX_WIFI_TX_POWER_QDBM, esp_err_to_name(err));

    err = esp_wifi_set_protocol(WIFI_IF_STA,
                                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set_protocol: %s", esp_err_to_name(err));

    if (channel == 0) {
        channel = ESPNOW_DEFAULT_CHANNEL;
    }
    err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel(%u): %s", channel, esp_err_to_name(err));
        return err;
    }
    s_current_channel = channel;

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
    bcast.channel = channel;
    bcast.ifidx   = WIFI_IF_STA;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);

    s_espnow_inited = true;

    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "ESP-NOW ready on channel %u (actual=%u)", channel, ch);
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
    s_rooms[slot].wifi_channel = s_current_channel;
    s_rooms[slot].room_code    = hash_room_id(beacon);
    s_rooms[slot].stream_id    = stream_id_from_beacon(beacon);
    snapshot = s_rooms[slot];
    portEXIT_CRITICAL(&s_room_lock);

    ESP_LOGI(TAG, "Room found: %.8s ch=%u rssi=%d mac=" MACSTR " dt=%u fb=%u",
             beacon->room, s_current_channel, rssi, MAC2STR(beacon->source_mac),
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
        if (s_state == ESPNOW_STATE_CONNECTED &&
            memcmp(beacon.source_mac, s_selected_beacon.source_mac, 6) == 0 &&
            strncmp(beacon.room, s_selected_beacon.room, ROOM_ID_LEN) == 0) {
            return;
        }
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
            ESP_LOGW(TAG, "ACCEPT stream changed got=%" PRIu32 " expected=%" PRIu32 "; using new stream",
                     accept.stream_id, s_selected_stream_id);
            s_selected_stream_id = accept.stream_id;
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
        if (s_audio_forward_q) {
            xQueueReset(s_audio_forward_q);
        }

        espnow_evt_joined_t joined = {0};
        memcpy(joined.mac, s_selected_beacon.source_mac, 6);
        joined.wifi_channel = s_selected_channel;
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

        audio_dedupe_reset();
        ESP_LOGI(TAG, "ACCEPT OK: room=%.8s ESP-NOW SBC key installed", accept.room);
        send_status(s_state);
    } else if (data[5] == MSG_AUDIO && info &&
               len == (int)(offsetof(audio_msg_t, payload) + ESPNOW_LC3_FRAME_BYTES)) {
        if (s_state != ESPNOW_STATE_CONNECTED) return;
        if (memcmp(info->src_addr, s_selected_beacon.source_mac, 6) != 0) return;

        audio_hdr_t audio;
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
            s_audio_header_drop++;
            return;
        }
        if (audio_already_forwarded(audio.seq)) {
            s_audio_late_or_dup++;
            return;
        }

        const uint8_t *payload = data + offsetof(audio_msg_t, payload);
        if (crc32_audio(payload, ESPNOW_LC3_FRAME_BYTES) != audio.payload_crc32) {
            s_audio_crc_fail++;
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
        memcpy(evt.payload, payload, ESPNOW_LC3_FRAME_BYTES);
        if (s_audio_forward_q &&
            xQueueSendToBack(s_audio_forward_q, &evt, 0) == pdTRUE) {
            audio_mark_forwarded(audio.seq);
            s_audio_copy_hist[audio.copy_idx]++;
        } else {
            s_audio_forward_fail++;
        }
        s_espnow_audio++;
    }
}

/* ───── Scan done timer task ───── */

static void scan_done_task_fn(void *arg)
{
    (void)arg;
    const uint32_t slice_ms = ESPNOW_SCAN_LISTEN_MS / ESPNOW_SCAN_CHANNELS;
    /* First slice: full ESP-NOW (re)init so we're guaranteed to have a
     * registered RX callback and broadcast peer. Subsequent slices just
     * retune the radio — see espnow_set_channel_only() comment. */
    for (int i = 0; i < ESPNOW_SCAN_CHANNELS && s_state == ESPNOW_STATE_SCANNING; ++i) {
        uint8_t channel = s_scan_channels[i];
        esp_err_t err = (i == 0) ? espnow_full_init(channel)
                                 : espnow_set_channel_only(channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan channel %u %s failed: %s", channel,
                     (i == 0) ? "init" : "hop", esp_err_to_name(err));
            /* Recovery: if a bare hop fails for some reason, fall back to
             * a full reinit so the next slice can still listen. */
            if (i > 0 && espnow_full_init(channel) != ESP_OK) {
                continue;
            }
        }
        ESP_LOGI(TAG, "Scanning for rooms on channel %u (%" PRIu32 " ms)...",
                 channel, slice_ms);
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
    }
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
    esp_err_t err = espnow_full_init(ESPNOW_DEFAULT_CHANNEL);
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

    portENTER_CRITICAL(&s_room_lock);
    memset(s_rooms, 0, sizeof(s_rooms));
    portEXIT_CRITICAL(&s_room_lock);

    s_state = ESPNOW_STATE_SCANNING;
    send_status(s_state);
    ESP_LOGI(TAG, "Scanning for rooms across channels 1/6/11 (%d ms)...", ESPNOW_SCAN_LISTEN_MS);

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
        if (cmd->wifi_channel == 0 || cmd->name[0] == '\0') {
            send_error(ESP_ERR_NOT_FOUND, "Room no longer visible");
            return;
        }

        memset(&room, 0, sizeof(room));
        room.beacon.magic = PROTO_MAGIC;
        room.beacon.version = PROTO_VERSION;
        room.beacon.type = MSG_BEACON;
        copy_room_name(room.beacon.room, cmd->name);
        memcpy(room.beacon.source_mac, cmd->mac, ESP_NOW_ETH_ALEN);
        room.beacon.lc3_dt_us = ESPNOW_LC3_FRAME_US;
        room.beacon.sample_rate_hz = ESPNOW_SAMPLE_RATE_HZ;
        room.beacon.bitrate_kbps = 0;
        room.beacon.frame_bytes = ESPNOW_LC3_FRAME_BYTES;
        room.beacon.flags = 0x01;
        room.beacon.stream_id = cmd->stream_id;
        room.last_seen_us = esp_timer_get_time();
        room.wifi_channel = cmd->wifi_channel;
        room.room_code = cmd->room_code;
        room.stream_id = cmd->stream_id;
        found = true;
        ESP_LOGW(TAG, "Joining from P4 room snapshot ch=%u room=%.8s stream=%" PRIu32,
                 room.wifi_channel, room.beacon.room, room.stream_id);
    }

    if (espnow_full_init(room.wifi_channel) != ESP_OK) {
        send_error(ESP_ERR_INVALID_STATE, "ESP-NOW reinit failed for room channel");
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
        peer.channel = room.wifi_channel;
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    apply_peer_rate_config(room.beacon.source_mac, "source");

    s_selected_beacon    = room.beacon;
    s_selected_room_code = room.room_code;
    s_selected_stream_id = room.stream_id;
    s_selected_channel   = room.wifi_channel;
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
    if (s_audio_forward_q) {
        xQueueReset(s_audio_forward_q);
    }
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
        espnow_full_init(ESPNOW_DEFAULT_CHANNEL);
        if (s_state == ESPNOW_STATE_NOT_INIT)
            s_state = ESPNOW_STATE_IDLE;
    } else if (id == WIFI_EVENT_STA_STOP || id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "Wi-Fi STA stopped — ESP-NOW invalidated");
        s_espnow_inited = false;
        s_state = ESPNOW_STATE_NOT_INIT;
    }
}

/* ───── Stats task ───── *
 *
 * While CONNECTED we emit the full counter snapshot every 1 s so the P4
 * can diff it against its own packet-RX counter every 5 s and surface
 * "I had a glitch, here's where it came from" in one line. The wire cost
 * is ~48 B/s over SDIO which is negligible compared with audio traffic.
 * Outside CONNECTED we fall back to the original 2 s cadence so room
 * scan UI feedback isn't delayed.
 */
static void stats_task_fn(void *arg)
{
    (void)arg;
    while (1) {
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);

        espnow_evt_stats_t st = {
            .packets_rx       = s_espnow_audio,
            .packets_lost     = s_audio_late_or_dup,  /* backcompat */
            .plc_frames       = 0,
            .rssi_last        = s_last_rssi,
            .wifi_channel     = ch,
            .sdio_send_errors = (uint8_t)(s_sdio_errors > 255 ? 255 : s_sdio_errors),
            .reserved         = 0,
            .forward_fail     = s_audio_forward_fail,
            .header_drops     = s_audio_header_drop,
            .crc_fails        = s_audio_crc_fail,
            .late_or_dup      = s_audio_late_or_dup,
        };
        for (int i = 0; i < 4; ++i) {
            st.copy_hist[i] = s_audio_copy_hist[i];
        }
        /* Snapshot-and-reset the enqueue-time max for this window. Doing it
         * BEFORE the stats sdio_send() means the value reported reflects the
         * peak between the previous emit and now, not including the stats
         * send itself (which would dominate the reading). */
        st.sdio_max_us = s_sdio_enq_max_us;
        s_sdio_enq_max_us = 0;
        /* Cumulative slave SDIO TX silent-drop counter. Surfaced as
         * `c6_tx_drop` in the P4 glitch log so we can finally tell
         * whether p4_rx < c6_rx is happening because the C6 TX path
         * is silently dropping (mempool exhaustion, send_queue fail)
         * vs. happening further downstream on the P4 host RX side. */
        st.sdio_tx_silent_drops = sdio_slave_get_tx_silent_drops();
        sdio_send(ESPNOW_MSG_EVT_STATS, &st, sizeof(st));

        if (s_state == ESPNOW_STATE_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
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

    s_audio_forward_q = xQueueCreateStatic(AUDIO_FORWARD_QUEUE_DEPTH,
                                           sizeof(espnow_evt_audio_raw_t),
                                           s_audio_forward_q_storage,
                                           &s_audio_forward_q_struct);
    if (!s_audio_forward_q) {
        ESP_LOGE(TAG, "Audio forward queue creation failed");
        return ESP_ERR_NO_MEM;
    }
    /* audio_fwd at prio 23 — same as pserial_task. Both are the audio
     * hot path and should be the highest-priority work the C6 does
     * outside of Wi-Fi MAC ISR/task. FreeRTOS round-robins same-prio
     * tasks, which lets audio_fwd hand off a packet immediately when
     * pserial drains its req_queue, instead of having to wait for
     * pserial to block on something at >22 (the old setup).
     *
     * Also: audio_forward_task_fn and sdio_send are now IRAM-resident
     * so this pipeline keeps running through any flash cache disable
     * event (NVS commit, OTA write, etc.). */
    xTaskCreate(audio_forward_task_fn, "audio_fwd", 4096, NULL, 23, &s_audio_forward_task);

    xTaskCreate(stats_task_fn, "room_stats", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Room bridge registered; waiting for Wi-Fi STA start");
    return ESP_OK;
}
