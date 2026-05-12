/*
 * ESP32 WROVER Room Audio Source — Auracast-style ESP-NOW broadcast
 *
 * Pipeline:  PCM1808 ADC → PCM ring → LC3 encode → AES → ESP-NOW broadcast
 *
 * Architecture (modeled after the stable esp-now-audio-source):
 *   Core 0: WiFi stack, beacon_task, handshake_task, pcm1808_reader_task
 *   Core 1: audio_encode_task (7.5 ms cadence), audio_tx_task (1.875 ms cadence)
 *
 * Key design decisions:
 *   - CONFIG_FREERTOS_HZ=1000 → 1 tick = 1 ms.  The 1.875 ms TX cadence
 *     delays for whole ticks, then busy-waits only the final short residual.
 *   - PCM ring (pointer queue) decouples I2S capture jitter from the
 *     cadence-critical emitter.
 *   - The TX task sends one ESP-NOW packet every 1.875 ms using the
 *     interleaved resend pattern: newest copy0/copy1, previous copy2,
 *     two-frame-old copy3.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lc3.h"
#include "mbedtls/aes.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

extern int esp_clk_cpu_freq(void);

/* ── Protocol constants (must match C6 bridge + sink) ──────────────── */
#define ESPNOW_CHANNEL       1
#define ROOM_ID_LEN          8
#define PROTO_MAGIC          0x44554152u
#define PROTO_VERSION        1
#define MSG_BEACON           1
#define MSG_HELLO            2
#define MSG_ACCEPT           3
#define MSG_AUDIO            4
#define HANDSHAKE_QUEUE      8

/* ── Audio / codec parameters ──────────────────────────────────────── */
#define LC3_DT_US            7500
#define AUDIO_RATE_HZ        48000
#define AUDIO_CHANNELS       2
#define AUDIO_BITS_PER_SAMPLE 24
#define LC3_BYTES_PER_CH     72
#define LC3_FRAME_BYTES      (LC3_BYTES_PER_CH * AUDIO_CHANNELS)
#define AUDIO_FRAME_SAMPLES  360
#define LC3_BITRATE_KBPS     154
#define MAX_FRAME_BYTES      400
#define ESPNOW_AUDIO_MAX_PAYLOAD LC3_FRAME_BYTES

_Static_assert((AUDIO_FRAME_SAMPLES * 1000000) == (AUDIO_RATE_HZ * LC3_DT_US),
               "PCM frame samples must match sample rate and LC3 frame time");

/* ── Auracast-style redundancy ─────────────────────────────────────── */
#define RTN                  4          /* copies per audio frame */
#define SUB_INTERVAL_US      1875       /* 7.5 ms / 4 copies */
#define TX_RING_FRAMES       16
#define ESPNOW_MAX_IN_FLIGHT 6
#define PACE_SPIN_US         250
#define PACE_ONE_TICK_US     1000
#define PACE_DELAY_GUARD_US  (PACE_ONE_TICK_US + PACE_SPIN_US)
#define ENC_TIMING_WINDOW_LOG2 8
#define ENC_TIMING_WINDOW    (1u << ENC_TIMING_WINDOW_LOG2)

_Static_assert((TX_RING_FRAMES & (TX_RING_FRAMES - 1)) == 0,
               "TX ring size must remain a power of two");

/* ── PCM1808 I2S pin config ────────────────────────────────────────── */
#define PIN_I2S_LRCK         GPIO_NUM_18
#define PIN_I2S_DIN          GPIO_NUM_19
#define PIN_I2S_BCK          GPIO_NUM_21
#define PIN_I2S_MCLK         GPIO_NUM_0

/* ── PCM ring between I2S reader and emitter ───────────────────────── */
#define NUM_PCM_BLOCKS       12

/* ── Logging tag ───────────────────────────────────────────────────── */
static const char *TAG = "room_source";

/* ── Broadcast address ─────────────────────────────────────────────── */
static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── Master key (shared with C6 and sink for handshake auth) ───────── */
static const uint8_t MASTER_KEY[32] = {
    0x93, 0x11, 0x75, 0x32, 0xd8, 0x4f, 0x26, 0x09,
    0xa5, 0x6b, 0xcd, 0x18, 0x74, 0xee, 0x20, 0x91,
    0x4c, 0x65, 0x2b, 0xaf, 0x8e, 0x39, 0x50, 0x7d,
    0x16, 0xb0, 0xf4, 0x43, 0x9a, 0xc7, 0x0e, 0x58,
};

/* ── Wire structs (packed, match C6 bridge exactly) ────────────────── */
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
    uint8_t  payload[ESPNOW_AUDIO_MAX_PAYLOAD];
} audio_msg_t;

typedef struct {
    uint8_t  mac[ESP_NOW_ETH_ALEN];
    hello_msg_t hello;
} hello_event_t;

/* PCM block passed by pointer through the ring */
typedef struct {
    int32_t pcm[AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS];
} pcm_block_t;

typedef struct {
    bool     valid;
    uint32_t seq;
    uint32_t capture_us;
    uint32_t payload_crc32;
    uint8_t  payload[ESPNOW_AUDIO_MAX_PAYLOAD];
} tx_frame_t;

typedef struct {
    uint8_t age;
    uint8_t copy;
} tx_sched_t;

/* ── Module state ──────────────────────────────────────────────────── */
static QueueHandle_t       s_hello_queue;
static QueueHandle_t       s_peer_cleanup_q;
static QueueHandle_t       s_pcm_free_q;
static QueueHandle_t       s_pcm_full_q;
static SemaphoreHandle_t   s_tx_tokens;
static portMUX_TYPE        s_tx_ring_lock = portMUX_INITIALIZER_UNLOCKED;

static pcm_block_t         s_pcm_pool[NUM_PCM_BLOCKS];
static tx_frame_t          s_tx_ring[TX_RING_FRAMES];
static i2s_chan_handle_t   s_i2s_rx;

static uint8_t  s_source_mac[6];
static uint8_t  s_audio_key[32];
static uint8_t  s_room_hash;
static uint32_t s_stream_id;
static int      s_frame_bytes;
static int      s_per_ch;
static volatile uint32_t s_latest_seq;
static volatile bool     s_have_latest_seq;

/* Stats (volatile — written from ISR/callback context) */
static volatile uint32_t s_tx_queued;
static volatile uint32_t s_tx_queue_fail;
static volatile uint32_t s_tx_cb_ok;
static volatile uint32_t s_tx_cb_fail;
static volatile uint32_t s_tx_token_drop;
static volatile uint32_t s_late_encode_frames;
static volatile uint32_t s_late_tx_ticks;
static volatile uint32_t s_pcm_underruns;

static const tx_sched_t s_tx_sched[RTN] = {
    {0, 0},
    {0, 1},
    {1, 2},
    {2, 3},
};

#if defined(CONFIG_ROOM_AUDIO_PHY_RATE_MCS1)
#define AUDIO_PHY_MODE WIFI_PHY_MODE_HT20
#define AUDIO_PHY_RATE WIFI_PHY_RATE_MCS1_LGI
#define AUDIO_PHY_NAME "MCS1-HT20-LGI"
#elif defined(CONFIG_ROOM_AUDIO_PHY_RATE_MCS2)
#define AUDIO_PHY_MODE WIFI_PHY_MODE_HT20
#define AUDIO_PHY_RATE WIFI_PHY_RATE_MCS2_LGI
#define AUDIO_PHY_NAME "MCS2-HT20-LGI"
#elif defined(CONFIG_ROOM_AUDIO_PHY_RATE_OFDM6)
#define AUDIO_PHY_MODE WIFI_PHY_MODE_11G
#define AUDIO_PHY_RATE WIFI_PHY_RATE_6M
#define AUDIO_PHY_NAME "OFDM-6M"
#elif defined(CONFIG_ROOM_AUDIO_PHY_RATE_OFDM12)
#define AUDIO_PHY_MODE WIFI_PHY_MODE_11G
#define AUDIO_PHY_RATE WIFI_PHY_RATE_12M
#define AUDIO_PHY_NAME "OFDM-12M"
#else
#define AUDIO_PHY_MODE WIFI_PHY_MODE_HT20
#define AUDIO_PHY_RATE WIFI_PHY_RATE_MCS3_LGI
#define AUDIO_PHY_NAME "MCS3-HT20-LGI"
#endif

static void random_bytes(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = esp_random();
        size_t n = (len - i < 4) ? len - i : 4;
        memcpy(out + i, &r, n);
    }
}

static bool room_id_valid(const char *r) {
    if (!r || strlen(r) != 8) return false;
    for (int i = 0; i < 8; i++) {
        if (i == 3) { if (r[i] != '-') return false; }
        else        { if (!isalnum((unsigned char)r[i])) return false; }
    }
    return true;
}

static void derive_room_hash(const char *room, uint8_t *hash) {
    uint8_t dig[32], in[32] = {0};
    snprintf((char *)in, sizeof(in), "espnow:%s", room);
    mbedtls_sha256(in, strlen((char *)in), dig, 0);
    *hash = dig[0] ^ dig[1] ^ dig[2] ^ dig[3];
}

/* ── Crypto: handshake auth/key-wrap and AES-CTR audio payloads ────── */
static esp_err_t aes_ctr_crypt(const uint8_t key[32], const uint8_t nonce[16],
                               const uint8_t *in, uint8_t *out, size_t len) {
    mbedtls_aes_context ctx;
    uint8_t nc[16], stream[16] = {0};
    size_t offset = 0;
    memcpy(nc, nonce, 16);
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0) ret = mbedtls_aes_crypt_ctr(&ctx, len, &offset, nc, stream, in, out);
    mbedtls_aes_free(&ctx);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t aes_cmac_16(const uint8_t *data, size_t len, uint8_t out[16]) {
    const mbedtls_cipher_info_t *ci = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_ECB);
    if (!ci) return ESP_FAIL;
    return mbedtls_cipher_cmac(ci, MASTER_KEY, 256, data, len, out) == 0 ? ESP_OK : ESP_FAIL;
}

static bool verify_hello(const hello_msg_t *msg) {
    uint8_t expected[16];
    hello_msg_t tmp = *msg;
    memset(tmp.auth, 0, sizeof(tmp.auth));
    if (aes_cmac_16((const uint8_t *)&tmp, sizeof(tmp), expected) != ESP_OK) return false;
    return memcmp(expected, msg->auth, 16) == 0;
}

static void make_audio_nonce(uint32_t stream_id, uint32_t seq, uint8_t nonce[16]) {
    memset(nonce, 0, 16);
    memcpy(&nonce[0], "LC3A", 4);
    nonce[4] = (uint8_t)stream_id;
    nonce[5] = (uint8_t)(stream_id >> 8);
    nonce[6] = (uint8_t)(stream_id >> 16);
    nonce[7] = (uint8_t)(stream_id >> 24);
    nonce[8] = (uint8_t)seq;
    nonce[9] = (uint8_t)(seq >> 8);
    nonce[10] = (uint8_t)(seq >> 16);
    nonce[11] = (uint8_t)(seq >> 24);
}

static uint32_t crc32_audio(const uint8_t *data, size_t len) {
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

/* ── ESP-NOW send callback ─────────────────────────────────────────── */
static void espnow_send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_tx_cb_ok++;
    } else {
        s_tx_cb_fail++;
    }
    if (s_tx_tokens) {
        xSemaphoreGive(s_tx_tokens);
    }

    if (info && info->des_addr && s_peer_cleanup_q &&
        memcmp(info->des_addr, BCAST, ESP_NOW_ETH_ALEN) != 0) {
        uint8_t mac[ESP_NOW_ETH_ALEN];
        memcpy(mac, info->des_addr, sizeof(mac));
        (void)xQueueSend(s_peer_cleanup_q, mac, 0);
    }
}

/* ── Precise pacing (from original stable source) ──────────────────── *
 *
 * CONFIG_FREERTOS_HZ = 1000 → 1 tick = 1 ms.
 * Strategy: vTaskDelay for whole ticks when comfortably before deadline,
 * then busy-wait only the final short residual.
 * This keeps IDLE tasks fed while hitting sub-ms precision. */
static inline void wait_until_us(int64_t target_us) {
    int64_t remain;
    while ((remain = target_us - esp_timer_get_time()) > PACE_DELAY_GUARD_US) {
        vTaskDelay(1);
    }
    while (esp_timer_get_time() < target_us) {
        /* Spin only near the deadline to preserve sub-ms packet spacing. */
    }
}

static void publish_tx_frame(uint32_t seq, uint32_t capture_us, const uint8_t *payload)
{
    uint32_t crc = crc32_audio(payload, s_frame_bytes);
    tx_frame_t *f = &s_tx_ring[seq & (TX_RING_FRAMES - 1)];

    portENTER_CRITICAL(&s_tx_ring_lock);
    f->valid = false;
    f->seq = seq;
    f->capture_us = capture_us;
    memcpy(f->payload, payload, s_frame_bytes);
    f->payload_crc32 = crc;
    f->valid = true;
    s_latest_seq = seq;
    s_have_latest_seq = true;
    portEXIT_CRITICAL(&s_tx_ring_lock);
}

static bool copy_tx_frame(uint32_t seq, tx_frame_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_tx_ring_lock);
    const tx_frame_t *f = &s_tx_ring[seq & (TX_RING_FRAMES - 1)];
    if (f->valid && f->seq == seq) {
        *out = *f;
        ok = true;
    }
    portEXIT_CRITICAL(&s_tx_ring_lock);
    return ok;
}

static bool get_latest_seq(uint32_t *seq)
{
    bool ok;
    portENTER_CRITICAL(&s_tx_ring_lock);
    ok = s_have_latest_seq;
    if (ok) {
        *seq = s_latest_seq;
    }
    portEXIT_CRITICAL(&s_tx_ring_lock);
    return ok;
}

static void send_one_audio_packet(const tx_frame_t *frame, uint8_t copy_idx)
{
    audio_msg_t pkt = {0};
    pkt.magic          = PROTO_MAGIC;
    pkt.version        = PROTO_VERSION;
    pkt.type           = MSG_AUDIO;
    pkt.room_hash      = s_room_hash;
    pkt.flags          = 0;
    pkt.stream_id      = s_stream_id;
    pkt.seq            = frame->seq;
    pkt.capture_us     = frame->capture_us;
    pkt.copy_count     = RTN;
    pkt.frame_bytes    = (uint8_t)s_frame_bytes;
    pkt.channels       = AUDIO_CHANNELS;
    /* CRC32 is for accidental corruption only; it is not an authentication tag. */
    pkt.payload_crc32  = frame->payload_crc32;
    pkt.copy_idx       = copy_idx;
    memcpy(pkt.payload, frame->payload, s_frame_bytes);

    if (xSemaphoreTake(s_tx_tokens, 0) != pdTRUE) {
        s_tx_token_drop++;
        return;
    }

    const size_t send_len = offsetof(audio_msg_t, payload) + (size_t)s_frame_bytes;
    esp_err_t err = esp_now_send(BCAST, (const uint8_t *)&pkt, send_len);
    if (err == ESP_OK) {
        s_tx_queued++;
    } else {
        s_tx_queue_fail++;
        xSemaphoreGive(s_tx_tokens);
    }
}

/* ── PCM1808 I2S capture ───────────────────────────────────────────── */
static void pcm1808_i2s_init(void) {
    if (s_i2s_rx) return;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                           I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = 120;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_LRCK,
            .din  = PIN_I2S_DIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx));

    ESP_LOGI(TAG, "PCM1808 I2S RX: %dHz 32-bit stereo (LRCK=%d DIN=%d BCK=%d MCLK=%d)",
             AUDIO_RATE_HZ, PIN_I2S_LRCK, PIN_I2S_DIN, PIN_I2S_BCK, PIN_I2S_MCLK);
}

/* ── WiFi + ESP-NOW init ───────────────────────────────────────────── */
static void wifi_espnow_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Enable 802.11n HT20 for better range at same bandwidth */
    (void)esp_wifi_set_protocol(WIFI_IF_STA,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    (void)esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_source_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk(MASTER_KEY));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* Four copies per 7.5 ms frame needs airtime headroom. Keep this
     * Kconfig-selectable so range can be tested without changing code. */
    esp_now_rate_config_t rcfg = {
        .phymode = AUDIO_PHY_MODE,
        .rate    = AUDIO_PHY_RATE,
        .ersu    = false,
        .dcm     = false,
    };
    esp_err_t re = esp_now_set_peer_rate_config(BCAST, &rcfg);
    if (re != ESP_OK)
        ESP_LOGW(TAG, "PHY rate config: %s", esp_err_to_name(re));

    ESP_LOGI(TAG, "WiFi ch %d, ESP-NOW ready, PHY=%s", ESPNOW_CHANNEL, AUDIO_PHY_NAME);
}

/* ── ESP-NOW receive callback (handshake only) ─────────────────────── */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len) {
    if (!info || !data || len < 6) return;
    uint32_t magic = 0;
    memcpy(&magic, data, 4);
    if (magic != PROTO_MAGIC || data[4] != PROTO_VERSION ||
        data[5] != MSG_HELLO || len != sizeof(hello_msg_t))
        return;
    hello_event_t ev = {0};
    memcpy(ev.mac, info->src_addr, 6);
    memcpy(&ev.hello, data, sizeof(ev.hello));
    if (s_hello_queue) xQueueSend(s_hello_queue, &ev, 0);
}

/* ── Beacon task (Core 0, low priority) ────────────────────────────── */
static void beacon_task(void *arg) {
    (void)arg;
    beacon_msg_t b = {0};
    b.magic       = PROTO_MAGIC;
    b.version     = PROTO_VERSION;
    b.type        = MSG_BEACON;
    snprintf(b.room, sizeof(b.room), "%s", CONFIG_ROOM_AUDIO_ROOM_ID);
    memcpy(b.source_mac, s_source_mac, 6);
    b.lc3_dt_us      = LC3_DT_US;
    b.sample_rate_hz = AUDIO_RATE_HZ;
    b.bitrate_kbps   = LC3_BITRATE_KBPS;
    b.frame_bytes    = (uint8_t)s_frame_bytes;
    b.flags          = 0x01;  /* stereo */
    b.stream_id      = s_stream_id;

    while (1) {
        if (xSemaphoreTake(s_tx_tokens, 0) == pdTRUE) {
            esp_err_t err = esp_now_send(BCAST, (const uint8_t *)&b, sizeof(b));
            if (err != ESP_OK) {
                xSemaphoreGive(s_tx_tokens);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── Handshake task (Core 0) ───────────────────────────────────────── */
static void send_accept(const hello_event_t *ev) {
    if (strncmp(ev->hello.room, CONFIG_ROOM_AUDIO_ROOM_ID, ROOM_ID_LEN) != 0) return;
    if (!verify_hello(&ev->hello)) {
        ESP_LOGW(TAG, "Rejected sink " MACSTR, MAC2STR(ev->mac));
        return;
    }
    accept_msg_t a = {0};
    a.magic   = PROTO_MAGIC;
    a.version = PROTO_VERSION;
    a.type    = MSG_ACCEPT;
    snprintf(a.room, sizeof(a.room), "%s", CONFIG_ROOM_AUDIO_ROOM_ID);
    memcpy(a.sink_nonce, ev->hello.sink_nonce, sizeof(a.sink_nonce));
    random_bytes(a.source_nonce, 16);
    random_bytes(a.key_nonce, 16);
    aes_ctr_crypt(MASTER_KEY, a.key_nonce, s_audio_key,
                  a.encrypted_audio_key, sizeof(s_audio_key));
    a.stream_id = s_stream_id;
    a.frame_bytes = (uint8_t)s_frame_bytes;

    accept_msg_t auth_copy = a;
    memset(auth_copy.auth, 0, 16);
    aes_cmac_16((const uint8_t *)&auth_copy, sizeof(auth_copy), a.auth);

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, ev->mac, 6);
    p.channel = ESPNOW_CHANNEL;
    p.ifidx   = WIFI_IF_STA;
    if (!esp_now_is_peer_exist(ev->mac)) esp_now_add_peer(&p);

    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_tx_tokens, 0) == pdTRUE) {
        err = esp_now_send(ev->mac, (const uint8_t *)&a, sizeof(a));
        if (err != ESP_OK) {
            xSemaphoreGive(s_tx_tokens);
        }
    }
    ESP_LOGI(TAG, "Authenticated sink " MACSTR ", accept=%s",
             MAC2STR(ev->mac), esp_err_to_name(err));
    if (err != ESP_OK) {
        esp_now_del_peer(ev->mac);
    }
}

static void peer_cleanup_task(void *arg) {
    (void)arg;
    uint8_t mac[ESP_NOW_ETH_ALEN];
    while (1) {
        if (xQueueReceive(s_peer_cleanup_q, mac, portMAX_DELAY) == pdTRUE) {
            esp_now_del_peer(mac);
        }
    }
}

static void handshake_task(void *arg) {
    (void)arg;
    hello_event_t ev;
    while (1) {
        if (xQueueReceive(s_hello_queue, &ev, portMAX_DELAY) == pdTRUE)
            send_accept(&ev);
    }
}

/* ── PCM1808 reader task (Core 0, moderate priority) ───────────────── *
 *
 * Reads 48 kHz stereo audio from PCM1808 over I2S and fills the PCM ring.
 * Queue operations are non-blocking; when the app falls behind, old captured
 * audio is replaced with the newest block instead of delaying the TX path. */
static void pcm1808_reader_task(void *arg) {
    (void)arg;
    static int32_t i2s_buf[AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS];
    pcm1808_i2s_init();

    while (1) {
        pcm_block_t *blk = NULL;
        if (xQueueReceive(s_pcm_free_q, &blk, 0) != pdTRUE) {
            (void)xQueueReceive(s_pcm_full_q, &blk, 0);
        }
        if (blk == NULL) {
            taskYIELD();
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_i2s_rx, i2s_buf, sizeof(i2s_buf),
                                         &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read != sizeof(i2s_buf)) {
            ESP_LOGW(TAG, "PCM1808 I2S read failed: %s bytes=%u",
                     esp_err_to_name(ret), (unsigned)bytes_read);
            memset(blk->pcm, 0, sizeof(blk->pcm));
        } else {
            for (int i = 0; i < AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS; ++i) {
                blk->pcm[i] = i2s_buf[i] >> 8;
            }
        }
        if (xQueueSend(s_pcm_full_q, &blk, 0) != pdTRUE) {
            pcm_block_t *old = NULL;
            if (xQueueReceive(s_pcm_full_q, &old, 0) == pdTRUE && old != NULL) {
                (void)xQueueSend(s_pcm_free_q, &old, 0);
            }
            (void)xQueueSend(s_pcm_full_q, &blk, 0);
        }
    }
}

/* ── Audio encode task (Core 1) ────────────────────────────────────── *
 *
 * Every 7.5 ms: pull a PCM block, LC3-encode, AES encrypt, publish to the
 * TX ring. This task never waits on ESP-NOW. */
static void audio_encode_task(void *arg) {
    (void)arg;

    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t enc_size = lc3_encoder_size(LC3_DT_US, AUDIO_RATE_HZ);
    void *enc_l_mem = heap_caps_calloc(1, enc_size, caps);
    void *enc_r_mem = heap_caps_calloc(1, enc_size, caps);
    uint8_t *lc3_buf = heap_caps_calloc(1, s_frame_bytes, caps);
    uint8_t *encrypted = heap_caps_calloc(1, s_frame_bytes, caps);
    if (!enc_l_mem || !enc_r_mem || !lc3_buf || !encrypted) {
        ESP_LOGE(TAG, "LC3 hot buffer allocation failed internal_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vTaskDelete(NULL);
        return;
    }

    lc3_encoder_t enc_l = lc3_setup_encoder(LC3_DT_US, AUDIO_RATE_HZ, 0, enc_l_mem);
    lc3_encoder_t enc_r = lc3_setup_encoder(LC3_DT_US, AUDIO_RATE_HZ, 0, enc_r_mem);
    if (!enc_l || !enc_r) {
        ESP_LOGE(TAG, "LC3 encoder init failed internal_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vTaskDelete(NULL);
        return;
    }

    static pcm_block_t last_pcm;
    memset(&last_pcm, 0, sizeof(last_pcm));

    /* Wait for PCM1808 reader to fill some blocks before starting cadence */
    while (uxQueueMessagesWaiting(s_pcm_full_q) < 4)
        vTaskDelay(pdMS_TO_TICKS(10));

    /* Prime with first block */
    {
        pcm_block_t *first = NULL;
        if (xQueueReceive(s_pcm_full_q, &first, pdMS_TO_TICKS(2000)) == pdTRUE && first) {
            memcpy(&last_pcm, first, sizeof(last_pcm));
            xQueueSend(s_pcm_free_q, &first, 0);
        }
    }

    ESP_LOGI(TAG, "Stereo LC3 encode: %d Hz, %d ch, %d B/ch, "
             "%d B total stream=%" PRIu32,
             AUDIO_RATE_HZ, AUDIO_CHANNELS,
             s_per_ch, s_frame_bytes, s_stream_id);

    uint32_t seq = 0;
    uint32_t frame_count = 0;
    uint32_t last_frame_count = 0;
    uint32_t last_tx_queued = s_tx_queued;
    int64_t frame_due_us = esp_timer_get_time();
    int64_t next_stats = esp_timer_get_time() + 5000000;
    uint32_t timing_count = 0;
    int64_t enc_l_sum = 0, enc_r_sum = 0, aes_sum = 0, pub_sum = 0, total_sum = 0;
    int64_t enc_l_max = 0, enc_r_max = 0, aes_max = 0, pub_max = 0, total_max = 0;

    while (1) {
        wait_until_us(frame_due_us);
        int64_t frame_start_us = esp_timer_get_time();
        if (frame_start_us > frame_due_us + 500) {
            s_late_encode_frames++;
        }

        /* Never block the cadence task waiting for PCM. Missing input reuses
         * the previous block, which is less audible than slowing the stream. */
        pcm_block_t *blk = NULL;
        if (xQueueReceive(s_pcm_full_q, &blk, 0) == pdTRUE && blk) {
            memcpy(&last_pcm, blk, sizeof(last_pcm));
            xQueueSend(s_pcm_free_q, &blk, 0);
        } else {
            s_pcm_underruns++;
        }

        /* LC3 encode both channels sequentially on core 1. */
        int64_t t0 = esp_timer_get_time();
        int rl = lc3_encode(enc_l, LC3_PCM_FORMAT_S24,
                            last_pcm.pcm, AUDIO_CHANNELS,
                            s_per_ch, lc3_buf);
        int64_t t1 = esp_timer_get_time();
        int rr = lc3_encode(enc_r, LC3_PCM_FORMAT_S24,
                            last_pcm.pcm + 1, AUDIO_CHANNELS,
                            s_per_ch, lc3_buf + s_per_ch);
        int64_t t2 = esp_timer_get_time();
        if (rl != 0 || rr != 0) {
            ESP_LOGW(TAG, "LC3 enc fail L=%d R=%d", rl, rr);
            frame_due_us += LC3_DT_US;
            continue;
        }

        uint8_t nonce[16];
        make_audio_nonce(s_stream_id, seq, nonce);
        if (aes_ctr_crypt(s_audio_key, nonce, lc3_buf, encrypted, s_frame_bytes) != ESP_OK) {
            ESP_LOGW(TAG, "Audio AES encrypt failed");
            frame_due_us += LC3_DT_US;
            continue;
        }
        int64_t t3 = esp_timer_get_time();

        publish_tx_frame(seq, (uint32_t)frame_start_us, encrypted);
        int64_t t4 = esp_timer_get_time();

        int64_t enc_l_us = t1 - t0;
        int64_t enc_r_us = t2 - t1;
        int64_t aes_us = t3 - t2;
        int64_t pub_us = t4 - t3;
        int64_t total_us = t4 - t0;
        enc_l_sum += enc_l_us;
        enc_r_sum += enc_r_us;
        aes_sum += aes_us;
        pub_sum += pub_us;
        total_sum += total_us;
        if (enc_l_us > enc_l_max) enc_l_max = enc_l_us;
        if (enc_r_us > enc_r_max) enc_r_max = enc_r_us;
        if (aes_us > aes_max) aes_max = aes_us;
        if (pub_us > pub_max) pub_max = pub_us;
        if (total_us > total_max) total_max = total_us;
        timing_count++;
        if (timing_count == ENC_TIMING_WINDOW) {
            ESP_LOGI(TAG,
                     "ENC timing avg/max us: L=%" PRId64 "/%" PRId64
                     " R=%" PRId64 "/%" PRId64 " AES=%" PRId64 "/%" PRId64
                     " PUB=%" PRId64 "/%" PRId64 " TOTAL=%" PRId64 "/%" PRId64,
                     enc_l_sum >> ENC_TIMING_WINDOW_LOG2, enc_l_max,
                     enc_r_sum >> ENC_TIMING_WINDOW_LOG2, enc_r_max,
                     aes_sum >> ENC_TIMING_WINDOW_LOG2, aes_max,
                     pub_sum >> ENC_TIMING_WINDOW_LOG2, pub_max,
                     total_sum >> ENC_TIMING_WINDOW_LOG2, total_max);
            timing_count = 0;
            enc_l_sum = enc_r_sum = aes_sum = pub_sum = total_sum = 0;
            enc_l_max = enc_r_max = aes_max = pub_max = total_max = 0;
        }

        frame_count++;
        seq++;

        int64_t now = esp_timer_get_time();
        frame_due_us += LC3_DT_US;
        if (frame_due_us < now - (int64_t)(LC3_DT_US * 4)) {
            s_late_encode_frames++;
            frame_due_us = now + LC3_DT_US;
        }

        /* Stats every 5 seconds */
        if (now >= next_stats) {
            uint32_t frame_delta = frame_count - last_frame_count;
            uint32_t tx_delta = s_tx_queued - last_tx_queued;
            uint32_t enc_fps_x10 = frame_delta << 1;
            uint32_t tx_pkt_s_x10 = tx_delta << 1;
            ESP_LOGI(TAG, "TX: frames=%" PRIu32 " queued=%" PRIu32 " qfail=%" PRIu32
                     " cb_ok=%" PRIu32 " cb_fail=%" PRIu32 " token_drop=%" PRIu32
                     " late_enc=%" PRIu32 " late_tx=%" PRIu32 " underrun=%" PRIu32
                     " pcm_q=%u enc_fps_x10=%" PRIu32 " tx_pkt_s_x10=%" PRIu32,
                     frame_count, s_tx_queued, s_tx_queue_fail, s_tx_cb_ok,
                     s_tx_cb_fail, s_tx_token_drop, s_late_encode_frames,
                     s_late_tx_ticks, s_pcm_underruns,
                     (unsigned)uxQueueMessagesWaiting(s_pcm_full_q),
                     enc_fps_x10, tx_pkt_s_x10);
            last_frame_count = frame_count;
            last_tx_queued = s_tx_queued;
            next_stats = now + 5000000;
        }
    }
}

static void audio_tx_task(void *arg)
{
    (void)arg;
    int64_t due = esp_timer_get_time() + SUB_INTERVAL_US;
    uint32_t tick = 0;

    ESP_LOGI(TAG, "ESP-NOW TX scheduler: RTN=%d sub=%dus", RTN, SUB_INTERVAL_US);
    while (1) {
        wait_until_us(due);
        int64_t now = esp_timer_get_time();
        if (now > due + 500) {
            s_late_tx_ticks++;
        }

        uint32_t latest;
        if (get_latest_seq(&latest)) {
            tx_sched_t sched = s_tx_sched[tick & (RTN - 1)];
            if (latest >= sched.age) {
                uint32_t seq = latest - sched.age;
                tx_frame_t frame;
                if (copy_tx_frame(seq, &frame)) {
                    send_one_audio_packet(&frame, sched.copy);
                }
            }
        }

        tick++;
        due += SUB_INTERVAL_US;
        now = esp_timer_get_time();
        if (due < now - (int64_t)(SUB_INTERVAL_US * 4)) {
            s_late_tx_ticks++;
            due = now + SUB_INTERVAL_US;
        }
    }
}

/* ── app_main ──────────────────────────────────────────────────────── */
void app_main(void) {
    if (!room_id_valid(CONFIG_ROOM_AUDIO_ROOM_ID)) {
        ESP_LOGE(TAG, "Invalid room '%s' (expected XXX-XXXX)", CONFIG_ROOM_AUDIO_ROOM_ID);
        return;
    }

    s_per_ch = LC3_BYTES_PER_CH;
    s_frame_bytes = LC3_FRAME_BYTES;
    if (s_per_ch <= 0 || s_frame_bytes > MAX_FRAME_BYTES ||
        s_frame_bytes > ESPNOW_AUDIO_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Bad LC3 frame size %d", s_frame_bytes);
        return;
    }

    derive_room_hash(CONFIG_ROOM_AUDIO_ROOM_ID, &s_room_hash);
    random_bytes(s_audio_key, sizeof(s_audio_key));
    s_stream_id = esp_random();
    if (s_stream_id == 0) {
        s_stream_id = 1;
    }

    /* Create PCM ring */
    s_pcm_free_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
    s_pcm_full_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
    for (int i = 0; i < NUM_PCM_BLOCKS; i++) {
        pcm_block_t *p = &s_pcm_pool[i];
        xQueueSend(s_pcm_free_q, &p, 0);
    }

    s_tx_tokens = xSemaphoreCreateCounting(ESPNOW_MAX_IN_FLIGHT, ESPNOW_MAX_IN_FLIGHT);
    s_hello_queue = xQueueCreate(HANDSHAKE_QUEUE, sizeof(hello_event_t));
    s_peer_cleanup_q = xQueueCreate(HANDSHAKE_QUEUE, ESP_NOW_ETH_ALEN);
    if (!s_pcm_free_q || !s_pcm_full_q || !s_tx_tokens ||
        !s_hello_queue || !s_peer_cleanup_q) {
        ESP_LOGE(TAG, "Queue/semaphore creation failed");
        return;
    }

    wifi_espnow_init();
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    ESP_LOGI(TAG, "CPU clock: %" PRIu32 " Hz config=%u MHz",
             (uint32_t)esp_clk_cpu_freq(), (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "Room %s on Wi-Fi ch %d", CONFIG_ROOM_AUDIO_ROOM_ID, ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "Stereo LC3: %d Hz, %d us, %d-bit, %d+%d=%d B/frame, "
             "RTN=%d sub=%dus stream=%" PRIu32 " phy=%s tick_hz=%u",
             AUDIO_RATE_HZ, LC3_DT_US, AUDIO_BITS_PER_SAMPLE,
             s_per_ch, s_per_ch, s_frame_bytes, RTN, SUB_INTERVAL_US,
             s_stream_id, AUDIO_PHY_NAME, (unsigned)configTICK_RATE_HZ);
    ESP_LOGI(TAG, "Audio payload integrity: CRC32 only (non-security); "
                  "session key is delivered by CMAC-authenticated ACCEPT");

    /* Core 0 tasks (WiFi lives here; PCM capture blocks only on I2S DMA) */
    xTaskCreatePinnedToCore(beacon_task,    "room_beacon", 4096, NULL, 4,  NULL, 0);
    xTaskCreatePinnedToCore(peer_cleanup_task, "peer_clean", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(handshake_task, "room_auth",   6144, NULL, 5,  NULL, 0);
    xTaskCreatePinnedToCore(pcm1808_reader_task, "pcm1808_rx", 8192, NULL, 18, NULL, 0);

    /* Core 1 tasks — TX keeps packet cadence while encode timing is measured. */
    xTaskCreatePinnedToCore(audio_tx_task,     "espnow_tx",  6144,  NULL, 23, NULL, 1);
    xTaskCreatePinnedToCore(audio_encode_task, "lc3_enc",    16384, NULL, 22, NULL, 1);
}
