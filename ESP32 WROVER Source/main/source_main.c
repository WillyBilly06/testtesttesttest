/*
 * ESP32 WROVER Room Audio Source — Auracast-style ESP-NOW broadcast
 *
 * Pipeline:  PCM1808 ADC → PCM ring → SBC encode → AES → ESP-NOW broadcast
 *
 * Architecture:
 *   Core 0: WiFi stack, room_ctrl, low-priority Web UI
 *   Core 1: pcm1808_reader_task, audio_encode_task, UDP TX, audio_tx_task
 *
 * Key design decisions:
 *   - SBC is hardcoded to 48 kHz, joint stereo, 16 blocks, 8 subbands, bitpool 53.
 *   - Each ESP-NOW audio packet carries 3 SBC frames: 384 samples / 8 ms.
 *   - RTN=4 copies, one packet every 2000 us.
 *   - TX uses interleaved resend schedule:
 *       phase 0: latest seq, copy0
 *       phase 1: latest seq, copy1
 *       phase 2: latest seq - 1, copy2
 *       phase 3: latest seq - 2, copy3
 *   - Three SBC frames are encoded per audio packet on Core 1.
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
 #include "esp_heap_caps.h"
 #include "esp_log.h"
 #include "esp_mac.h"
 #include "esp_now.h"
 #include "esp_random.h"
 #include "esp_timer.h"
 #include "esp_wifi.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/queue.h"
 #include "freertos/semphr.h"
 #include "freertos/task.h"
#include "sbc_encoder.h"
 #include "mbedtls/aes.h"
 #include "mbedtls/cipher.h"
 #include "mbedtls/cmac.h"
 #include "mbedtls/sha256.h"
 #include "nvs_flash.h"
#include "source_audio_stats.h"
#include "source_config.h"
#include "source_udp_control.h"
#include "source_udp_stream.h"
#include "source_web.h"
#include "source_wifi.h"

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
#define LC3_DT_US             8000
 #define AUDIO_RATE_HZ         48000
 #define AUDIO_CHANNELS        2
 #define AUDIO_BITS_PER_SAMPLE 24
#define SBC_FRAMES_PER_PACKET 3
#define SBC_SAMPLES_PER_FRAME 128
#define SBC_FRAME_BYTES       119
#define SBC_PAYLOAD_BYTES     (SBC_FRAMES_PER_PACKET * SBC_FRAME_BYTES)
#define SBC_BITRATE_KBPS      357
#define AUDIO_FRAME_SAMPLES   (SBC_FRAMES_PER_PACKET * SBC_SAMPLES_PER_FRAME)
#define LC3_BYTES_PER_CH      SBC_FRAME_BYTES
#define LC3_FRAME_BYTES       SBC_PAYLOAD_BYTES
#define LC3_BITRATE_KBPS      SBC_BITRATE_KBPS
#define MAX_FRAME_BYTES       1470
 #define ESPNOW_AUDIO_MAX_PAYLOAD LC3_FRAME_BYTES

 _Static_assert((AUDIO_FRAME_SAMPLES * 1000000) == (AUDIO_RATE_HZ * LC3_DT_US),
               "PCM frame samples must match sample rate and SBC packet time");

 /* ── Auracast-style redundancy ─────────────────────────────────────── */
 #define RTN                  4
#define SUB_INTERVAL_US       2000
 #define TX_RING_FRAMES       16
#define ESPNOW_MAX_IN_FLIGHT 8
 #define PACE_SPIN_US         250
 #define MAX_WIFI_TX_POWER_QDBM 84
 #define SOURCE_GAIN_DB       5
 #define SOURCE_GAIN_Q8       455  /* +5 dB = 1.778x */

 _Static_assert((TX_RING_FRAMES & (TX_RING_FRAMES - 1)) == 0,
                "TX ring size must remain a power of two");

 /* ── PCM1808 I2S pin config ────────────────────────────────────────── */
 #define PIN_I2S_LRCK         GPIO_NUM_18
 #define PIN_I2S_DIN          GPIO_NUM_19
 #define PIN_I2S_BCK          GPIO_NUM_21
 #define PIN_I2S_MCLK         GPIO_NUM_0

 /* ── PCM ring between I2S reader and emitter ───────────────────────── */
 #define NUM_PCM_BLOCKS       6
 #define PCM_START_PREFILL_BLOCKS 1
 #define PCM_RX_DMA_FRAME_SAMPLES 64

 static const char *TAG = "room_source";
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
     uint8_t  payload[ESPNOW_AUDIO_MAX_PAYLOAD];
 } audio_msg_t;

 typedef struct {
     uint8_t  mac[ESP_NOW_ETH_ALEN];
     hello_msg_t hello;
 } hello_event_t;

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
 static QueueSetHandle_t    s_control_queue_set;
 static QueueHandle_t       s_pcm_free_q;
 static QueueHandle_t       s_pcm_full_q;
 static SemaphoreHandle_t   s_tx_tokens;
static TaskHandle_t        s_audio_tx_task;
 static portMUX_TYPE        s_tx_ring_lock = portMUX_INITIALIZER_UNLOCKED;

 static pcm_block_t         s_pcm_pool[NUM_PCM_BLOCKS];
 static tx_frame_t          s_tx_ring[TX_RING_FRAMES];
 static i2s_chan_handle_t   s_i2s_rx;

 static uint8_t  s_source_mac[6];
 static uint8_t  s_audio_key[32];
static char     s_room_id[ROOM_ID_LEN + 1];
 static uint8_t  s_room_hash;
 static uint32_t s_stream_id;
 static int      s_frame_bytes;
 static int      s_per_ch;
static volatile int s_input_gain_q8 = SOURCE_GAIN_Q8;

 static volatile uint32_t s_latest_seq;
 static volatile bool     s_have_latest_seq;
static volatile bool     s_audio_tx_enabled;
static portMUX_TYPE      s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;

 /* Stats */
 static volatile uint32_t s_tx_queued;
 static volatile uint32_t s_tx_queue_fail;
 static volatile uint32_t s_tx_cb_ok;
 static volatile uint32_t s_tx_cb_fail;
 static volatile uint32_t s_tx_token_drop;
static volatile uint32_t s_espnow_queue_drops;
static volatile uint32_t s_espnow_fragment_drops;
static volatile uint32_t s_alloc_failures;
 static volatile uint32_t s_late_encode_frames;
 static volatile uint32_t s_late_tx_ticks;
 static volatile uint32_t s_pcm_underruns;
 static volatile uint32_t s_source_gain_clips;

 static const tx_sched_t s_tx_sched[RTN] = {
     {0, 0},
     {0, 1},
     {1, 2},
     {2, 3},
 };

void source_audio_get_stats(source_audio_stats_t *out)
{
    if (!out) {
        return;
    }

    uint32_t latest = 0;
    bool have_latest = false;
    portENTER_CRITICAL(&s_tx_ring_lock);
    have_latest = s_have_latest_seq;
    latest = s_latest_seq;
    portEXIT_CRITICAL(&s_tx_ring_lock);

    uint32_t ring_level = 0;
    if (have_latest) {
        ring_level = (latest + 1u) < TX_RING_FRAMES ? (latest + 1u) : TX_RING_FRAMES;
    }

    uint32_t in_flight = 0;
    if (s_tx_tokens) {
        UBaseType_t available = uxSemaphoreGetCount(s_tx_tokens);
        in_flight = available < ESPNOW_MAX_IN_FLIGHT ? (ESPNOW_MAX_IN_FLIGHT - available) : 0;
    }

    out->espnow_fragments_sent = s_tx_cb_ok;
    out->espnow_frames_sent = s_tx_cb_ok / RTN;
    out->espnow_queue_level = ring_level + in_flight;
    out->espnow_queue_drops = s_espnow_queue_drops;
    out->espnow_fragment_drops = s_espnow_fragment_drops;
    out->espnow_send_errors = s_tx_queue_fail + s_tx_cb_fail;
    out->audio_scheduler_deadline_misses = s_late_encode_frames + s_late_tx_ticks;
    out->allocation_failures = s_alloc_failures;
}

esp_err_t source_audio_get_udp_session(source_udp_session_snapshot_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_runtime_lock);
    out->stream_id = s_stream_id;
    out->session_id = s_stream_id;
    out->frame_bytes = (uint16_t)s_frame_bytes;
    out->channels = AUDIO_CHANNELS;
    memcpy(out->key, s_audio_key, sizeof(out->key));
    portEXIT_CRITICAL(&s_runtime_lock);

    portENTER_CRITICAL(&s_tx_ring_lock);
    out->latest_seq = s_latest_seq;
    portEXIT_CRITICAL(&s_tx_ring_lock);
    return ESP_OK;
}

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

 static void random_bytes(uint8_t *out, size_t len)
 {
     for (size_t i = 0; i < len; i += 4) {
         uint32_t r = esp_random();
         size_t n = (len - i < 4) ? len - i : 4;
         memcpy(out + i, &r, n);
     }
 }

 static void derive_room_hash(const char *room, uint8_t *hash)
 {
     uint8_t dig[32], in[32] = {0};
     snprintf((char *)in, sizeof(in), "espnow:%s", room);
     mbedtls_sha256(in, strlen((char *)in), dig, 0);
     *hash = dig[0] ^ dig[1] ^ dig[2] ^ dig[3];
 }

static void get_runtime_audio(uint8_t *room_hash, uint32_t *stream_id, uint8_t audio_key[32])
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (room_hash) *room_hash = s_room_hash;
    if (stream_id) *stream_id = s_stream_id;
    if (audio_key) memcpy(audio_key, s_audio_key, sizeof(s_audio_key));
    portEXIT_CRITICAL(&s_runtime_lock);
}

static void get_runtime_room(char room[ROOM_ID_LEN + 1], uint32_t *stream_id, uint8_t audio_key[32])
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (room) memcpy(room, s_room_id, ROOM_ID_LEN + 1);
    if (stream_id) *stream_id = s_stream_id;
    if (audio_key) memcpy(audio_key, s_audio_key, sizeof(s_audio_key));
    portEXIT_CRITICAL(&s_runtime_lock);
}

static void apply_runtime_config(void *ctx)
{
    (void)ctx;
    source_config_t cfg;
    source_config_get(&cfg);

    uint8_t hash = 0;
    derive_room_hash(cfg.room_id, &hash);
    uint8_t new_key[32];
    random_bytes(new_key, sizeof(new_key));
    uint32_t new_stream = esp_random();
    if (new_stream == 0) new_stream = 1;

    portENTER_CRITICAL(&s_runtime_lock);
    bool room_changed = strncmp(s_room_id, cfg.room_id, ROOM_ID_LEN) != 0;
    if (room_changed || s_room_id[0] == '\0') {
        memcpy(s_room_id, cfg.room_id, ROOM_ID_LEN + 1);
        s_room_hash = hash;
        memcpy(s_audio_key, new_key, sizeof(s_audio_key));
        s_stream_id = new_stream;
        s_have_latest_seq = false;
        s_audio_tx_enabled = false;
    }
    s_input_gain_q8 = source_config_gain_q8_from_db_x10(cfg.gain_db_x10);
    portEXIT_CRITICAL(&s_runtime_lock);
    source_udp_stream_set_port(cfg.udp_port);
    source_udp_control_set_room_metadata(cfg.room_id, cfg.room_id);
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
     if (ret == 0) ret = mbedtls_aes_crypt_ctr(&ctx, len, &offset, nc, stream, in, out);
     mbedtls_aes_free(&ctx);
     return ret == 0 ? ESP_OK : ESP_FAIL;
 }

 static esp_err_t aes_cmac_16(const uint8_t *data, size_t len, uint8_t out[16])
 {
     const mbedtls_cipher_info_t *ci = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_ECB);
     if (!ci) return ESP_FAIL;
     return mbedtls_cipher_cmac(ci, MASTER_KEY, 256, data, len, out) == 0 ? ESP_OK : ESP_FAIL;
 }

 static bool verify_hello(const hello_msg_t *msg)
 {
     uint8_t expected[16];
     hello_msg_t tmp = *msg;
     memset(tmp.auth, 0, sizeof(tmp.auth));
     if (aes_cmac_16((const uint8_t *)&tmp, sizeof(tmp), expected) != ESP_OK) return false;
     return memcmp(expected, msg->auth, 16) == 0;
 }

 static void make_audio_nonce(uint32_t stream_id, uint32_t seq, uint8_t nonce[16])
 {
     memset(nonce, 0, 16);
    memcpy(&nonce[0], "SBCA", 4);
     nonce[4] = (uint8_t)stream_id;
     nonce[5] = (uint8_t)(stream_id >> 8);
     nonce[6] = (uint8_t)(stream_id >> 16);
     nonce[7] = (uint8_t)(stream_id >> 24);
     nonce[8]  = (uint8_t)seq;
     nonce[9]  = (uint8_t)(seq >> 8);
     nonce[10] = (uint8_t)(seq >> 16);
     nonce[11] = (uint8_t)(seq >> 24);
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

 static void espnow_send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
 {
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

 static inline void wait_until_us(int64_t target_us)
 {
     int64_t remain;
    while ((remain = target_us - esp_timer_get_time()) > PACE_SPIN_US) {
         vTaskDelay(1);
     }
     while (esp_timer_get_time() < target_us) {
         ;
     }
 }

 static inline void wait_until_encode_us(int64_t target_us)
 {
     int64_t remain;
     while ((remain = target_us - esp_timer_get_time()) > PACE_SPIN_US) {
         vTaskDelay(1);
     }
     while (esp_timer_get_time() < target_us) {
         ;
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

    uint8_t room_hash = 0;
    uint32_t stream_id = 0;
    get_runtime_audio(&room_hash, &stream_id, NULL);
    if (!source_wifi_is_sta_connected()) {
        return;
    }

    source_udp_audio_packet_t udp = {0};
    udp.magic = PROTO_MAGIC;
    udp.version = PROTO_VERSION;
    udp.type = MSG_AUDIO;
    udp.room_hash = room_hash;
    udp.flags = 0;
    udp.stream_id = stream_id;
    udp.seq = seq;
    udp.capture_us = capture_us;
    udp.payload_crc32 = crc;
    udp.copy_idx = 0;
    udp.copy_count = 1;
    udp.frame_bytes = (uint16_t)s_frame_bytes;
    udp.channels = AUDIO_CHANNELS;
    memcpy(udp.payload, payload, s_frame_bytes);
    source_udp_stream_submit(&udp, s_frame_bytes);
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
     if (ok) *seq = s_latest_seq;
     portEXIT_CRITICAL(&s_tx_ring_lock);
     return ok;
 }

 static void send_one_audio_packet(const tx_frame_t *frame, uint8_t copy_idx)
 {
    uint8_t room_hash = 0;
    uint32_t stream_id = 0;
    get_runtime_audio(&room_hash, &stream_id, NULL);

     audio_msg_t pkt = {0};
     pkt.magic          = PROTO_MAGIC;
     pkt.version        = PROTO_VERSION;
     pkt.type           = MSG_AUDIO;
    pkt.room_hash      = room_hash;
     pkt.flags          = 0;
    pkt.stream_id      = stream_id;
     pkt.seq            = frame->seq;
     pkt.capture_us     = frame->capture_us;
     pkt.copy_count     = RTN;
    pkt.frame_bytes    = (uint16_t)s_frame_bytes;
     pkt.channels       = AUDIO_CHANNELS;
     pkt.payload_crc32  = frame->payload_crc32;
     pkt.copy_idx       = copy_idx;
     memcpy(pkt.payload, frame->payload, s_frame_bytes);

     if (xSemaphoreTake(s_tx_tokens, 0) != pdTRUE) {
         s_tx_token_drop++;
         s_espnow_fragment_drops++;
         return;
     }

     const size_t send_len = offsetof(audio_msg_t, payload) + (size_t)s_frame_bytes;
     esp_err_t err = esp_now_send(BCAST, (const uint8_t *)&pkt, send_len);
     if (err == ESP_OK) {
         s_tx_queued++;
     } else {
         s_tx_queue_fail++;
         s_espnow_fragment_drops++;
         xSemaphoreGive(s_tx_tokens);
     }
 }

 static void pcm1808_i2s_init(void)
 {
     if (s_i2s_rx) return;

     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
     chan_cfg.dma_desc_num  = 3;
     chan_cfg.dma_frame_num = PCM_RX_DMA_FRAME_SAMPLES;
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

 static void wifi_espnow_init(void)
 {
    ESP_ERROR_CHECK(source_wifi_start(ESPNOW_CHANNEL));
    ESP_ERROR_CHECK(source_wifi_get_sta_mac(s_source_mac));

     ESP_ERROR_CHECK(esp_now_init());
     ESP_ERROR_CHECK(esp_now_set_pmk(MASTER_KEY));

     esp_now_peer_info_t peer = {0};
     memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0;
     peer.ifidx = WIFI_IF_STA;
     peer.encrypt = false;
     ESP_ERROR_CHECK(esp_now_add_peer(&peer));

     esp_now_rate_config_t rcfg = {
         .phymode = AUDIO_PHY_MODE,
         .rate    = AUDIO_PHY_RATE,
         .ersu    = false,
         .dcm     = false,
     };
     esp_err_t re = esp_now_set_peer_rate_config(BCAST, &rcfg);
     if (re != ESP_OK) {
         ESP_LOGW(TAG, "PHY rate config: %s", esp_err_to_name(re));
     }

    ESP_LOGI(TAG, "WiFi ch %u, ESP-NOW ready, PHY=%s",
             source_wifi_current_channel(), AUDIO_PHY_NAME);
 }

 static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
 {
     if (!info || !data || len < 6) return;
     uint32_t magic = 0;
     memcpy(&magic, data, 4);
     if (magic != PROTO_MAGIC || data[4] != PROTO_VERSION ||
         data[5] != MSG_HELLO || len != sizeof(hello_msg_t)) {
         return;
     }
     hello_event_t ev = {0};
     memcpy(ev.mac, info->src_addr, 6);
     memcpy(&ev.hello, data, sizeof(ev.hello));
     if (s_hello_queue) xQueueSend(s_hello_queue, &ev, 0);
 }

static void send_beacon_once(const beacon_msg_t *b)
 {
    if (xSemaphoreTake(s_tx_tokens, 0) == pdTRUE) {
        esp_err_t err = esp_now_send(BCAST, (const uint8_t *)b, sizeof(*b));
        if (err != ESP_OK) {
            xSemaphoreGive(s_tx_tokens);
         }
     }
 }

 static void send_accept(const hello_event_t *ev)
 {
    char room[ROOM_ID_LEN + 1];
    uint32_t stream_id = 0;
    uint8_t audio_key[32];
    get_runtime_room(room, &stream_id, audio_key);
    if (strncmp(ev->hello.room, room, ROOM_ID_LEN) != 0) return;
     if (!verify_hello(&ev->hello)) {
         ESP_LOGW(TAG, "Rejected sink " MACSTR, MAC2STR(ev->mac));
         return;
     }

     accept_msg_t a = {0};
     a.magic   = PROTO_MAGIC;
     a.version = PROTO_VERSION;
     a.type    = MSG_ACCEPT;
    snprintf(a.room, sizeof(a.room), "%s", room);
     memcpy(a.sink_nonce, ev->hello.sink_nonce, sizeof(a.sink_nonce));
     random_bytes(a.source_nonce, 16);
     random_bytes(a.key_nonce, 16);
    aes_ctr_crypt(MASTER_KEY, a.key_nonce, audio_key,
                  a.encrypted_audio_key, sizeof(audio_key));
    a.stream_id = stream_id;
    a.frame_bytes = (uint16_t)s_frame_bytes;

     accept_msg_t auth_copy = a;
     memset(auth_copy.auth, 0, 16);
     aes_cmac_16((const uint8_t *)&auth_copy, sizeof(auth_copy), a.auth);

     esp_now_peer_info_t p = {0};
     memcpy(p.peer_addr, ev->mac, 6);
    p.channel = 0;
     p.ifidx   = WIFI_IF_STA;
     if (!esp_now_is_peer_exist(ev->mac)) esp_now_add_peer(&p);

     esp_err_t err = ESP_ERR_TIMEOUT;
     if (xSemaphoreTake(s_tx_tokens, 0) == pdTRUE) {
         err = esp_now_send(ev->mac, (const uint8_t *)&a, sizeof(a));
         if (err != ESP_OK) {
             xSemaphoreGive(s_tx_tokens);
        } else {
            s_audio_tx_enabled = true;
         }
     }
     ESP_LOGI(TAG, "Authenticated sink " MACSTR ", accept=%s",
              MAC2STR(ev->mac), esp_err_to_name(err));
     if (err != ESP_OK) {
         esp_now_del_peer(ev->mac);
     }
 }

static void control_task(void *arg)
 {
     (void)arg;
    const TickType_t beacon_period = pdMS_TO_TICKS(1000);
    TickType_t next_beacon = xTaskGetTickCount();
    hello_event_t ev;
     uint8_t mac[ESP_NOW_ETH_ALEN];

     while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_beacon) >= 0) {
            char room[ROOM_ID_LEN + 1];
            uint32_t stream_id = 0;
            get_runtime_room(room, &stream_id, NULL);

            beacon_msg_t b = {0};
            b.magic       = PROTO_MAGIC;
            b.version     = PROTO_VERSION;
            b.type        = MSG_BEACON;
            snprintf(b.room, sizeof(b.room), "%s", room);
            memcpy(b.source_mac, s_source_mac, 6);
            b.lc3_dt_us      = LC3_DT_US;
            b.sample_rate_hz = AUDIO_RATE_HZ;
            b.bitrate_kbps   = LC3_BITRATE_KBPS;
            b.frame_bytes    = (uint16_t)s_frame_bytes;
            b.flags          = 0x01;
            b.stream_id      = stream_id;
            send_beacon_once(&b);
            next_beacon += beacon_period;
            if ((int32_t)(now - next_beacon) >= 0) {
                next_beacon = now + beacon_period;
            }
         }

        now = xTaskGetTickCount();
        TickType_t wait_ticks = (TickType_t)((next_beacon > now) ? (next_beacon - now) : 0);
        QueueSetMemberHandle_t ready = xQueueSelectFromSet(s_control_queue_set, wait_ticks);
        if (ready == s_hello_queue) {
            if (xQueueReceive(s_hello_queue, &ev, 0) == pdTRUE) {
                send_accept(&ev);
            }
        } else if (ready == s_peer_cleanup_q) {
            if (xQueueReceive(s_peer_cleanup_q, mac, 0) == pdTRUE) {
                esp_now_del_peer(mac);
            }
         }
     }
 }

 static void pcm1808_reader_task(void *arg)
 {
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

 static inline int16_t pcm_s24_to_s16(int32_t sample)
 {
    int64_t boosted = (int64_t)sample * s_input_gain_q8;
     boosted += (boosted >= 0) ? 128 : -128;
     boosted >>= 8;

     if (boosted > 8388607) {
         boosted = 8388607;
         s_source_gain_clips++;
     } else if (boosted < -8388608) {
         boosted = -8388608;
         s_source_gain_clips++;
     }
     return (int16_t)((int32_t)boosted >> 8);
 }

 static bool take_latest_pcm_block(pcm_block_t *out)
 {
     pcm_block_t *blk = NULL;
     pcm_block_t *latest = NULL;

     while (xQueueReceive(s_pcm_full_q, &blk, 0) == pdTRUE && blk) {
         if (latest) {
             xQueueSend(s_pcm_free_q, &latest, 0);
         }
         latest = blk;
     }

     if (!latest) {
         return false;
     }

     memcpy(out, latest, sizeof(*out));
     xQueueSend(s_pcm_free_q, &latest, 0);
     return true;
 }

 static void audio_encode_task(void *arg)
 {
     (void)arg;

     const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
     SBC_ENC_PARAMS *enc = heap_caps_calloc(1, sizeof(SBC_ENC_PARAMS), caps);
     uint8_t *sbc_buf = heap_caps_calloc(1, s_frame_bytes, caps);
     uint8_t *encrypted = heap_caps_calloc(1, s_frame_bytes, caps);

     if (!enc || !sbc_buf || !encrypted) {
         s_alloc_failures++;
         ESP_LOGE(TAG, "SBC hot buffer allocation failed enc=%p sbc=%p encrypted=%p internal_free=%u",
                  enc, sbc_buf, encrypted,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
         vTaskDelete(NULL);
         return;
     }

     enc->sbc_mode = SBC_MODE_STD;
     enc->s16SamplingFreq = SBC_sf48000;
     enc->s16ChannelMode = SBC_JOINT_STEREO;
     enc->s16NumOfSubBands = SUB_BANDS_8;
     enc->s16NumOfBlocks = SBC_BLOCK_3;
     enc->s16AllocationMethod = SBC_LOUDNESS;
     enc->u16BitRate = SBC_BITRATE_KBPS;
     SBC_Encoder_Init(enc);

     if (enc->s16BitPool != 53) {
         ESP_LOGW(TAG, "SBC bitpool=%d expected=53 for %d kbps", enc->s16BitPool, SBC_BITRATE_KBPS);
     }

     static pcm_block_t last_pcm;
     memset(&last_pcm, 0, sizeof(last_pcm));

     while (uxQueueMessagesWaiting(s_pcm_full_q) < PCM_START_PREFILL_BLOCKS) {
         vTaskDelay(pdMS_TO_TICKS(1));
     }

     if (!take_latest_pcm_block(&last_pcm)) {
         ESP_LOGW(TAG, "No initial PCM block available; starting with silence");
     }

    uint32_t initial_stream_id = 0;
    get_runtime_audio(NULL, &initial_stream_id, NULL);
    ESP_LOGI(TAG, "Stereo SBC encode: %d Hz, joint stereo, blocks=16 subbands=8 bitpool=%d, %d frames/pkt, %d B total stream=%" PRIu32,
              AUDIO_RATE_HZ, enc->s16BitPool, SBC_FRAMES_PER_PACKET, s_frame_bytes,
             initial_stream_id);

    uint32_t seq = 0;
     int64_t frame_due_us = esp_timer_get_time();
     uint32_t enc_fail_log_div = 0;

     while (1) {
         wait_until_encode_us(frame_due_us);
         int64_t frame_start_us = esp_timer_get_time();

         if (frame_start_us > frame_due_us + 500) {
             s_late_encode_frames++;
         }

         if (!take_latest_pcm_block(&last_pcm)) {
             s_pcm_underruns++;
         }

         size_t out_len = 0;
         bool enc_ok = true;

         for (int f = 0; f < SBC_FRAMES_PER_PACKET; ++f) {
             const int sample_base = f * SBC_SAMPLES_PER_FRAME;
             for (int i = 0; i < SBC_SAMPLES_PER_FRAME; ++i) {
                 const int src = (sample_base + i) << 1;
                 enc->as16PcmBuffer[i << 1] = pcm_s24_to_s16(last_pcm.pcm[src]);
                 enc->as16PcmBuffer[(i << 1) | 1] = pcm_s24_to_s16(last_pcm.pcm[src | 1]);
             }

             enc->pu8Packet = sbc_buf + out_len;
             enc->u8NumPacketToEncode = 1;
             SBC_Encoder(enc);

             if (enc->u16PacketLength != SBC_FRAME_BYTES || out_len + enc->u16PacketLength > (size_t)s_frame_bytes) {
                 enc_ok = false;
                 break;
             }
             out_len += enc->u16PacketLength;
         }

         if (!enc_ok || out_len != (size_t)s_frame_bytes) {
             if ((enc_fail_log_div++ % 100) == 0) {
                 ESP_LOGW(TAG,
                          "SBC enc fail out=%u expected=%d last_frame=%u seq=%" PRIu32,
                          (unsigned)out_len, s_frame_bytes, enc->u16PacketLength, seq);
             }

             frame_due_us += LC3_DT_US;
             int64_t now_fail = esp_timer_get_time();
             if (frame_due_us < now_fail - (int64_t)(LC3_DT_US * 4)) {
                 s_late_encode_frames++;
                 frame_due_us = now_fail + LC3_DT_US;
             }
             continue;
         }

        uint32_t stream_id = 0;
        uint8_t audio_key[32];
        get_runtime_audio(NULL, &stream_id, audio_key);

        uint8_t nonce[16];
        make_audio_nonce(stream_id, seq, nonce);

        if (aes_ctr_crypt(audio_key, nonce, sbc_buf, encrypted, s_frame_bytes) != ESP_OK) {
             ESP_LOGW(TAG, "Audio AES encrypt failed");
             frame_due_us += LC3_DT_US;
             continue;
         }

         publish_tx_frame(seq, (uint32_t)frame_start_us, encrypted);

         seq++;

         int64_t now = esp_timer_get_time();
         frame_due_us += LC3_DT_US;
         if (frame_due_us < now - (int64_t)(LC3_DT_US * 4)) {
             s_late_encode_frames++;
             frame_due_us = now + LC3_DT_US;
         }

     }
 }

static void tx_timer_cb(void *arg)
{
    TaskHandle_t task = (TaskHandle_t)arg;
    if (task) {
        xTaskNotifyGive(task);
    }
}

 static void audio_tx_task(void *arg)
 {
     (void)arg;
     s_audio_tx_task = xTaskGetCurrentTaskHandle();

     esp_timer_handle_t tx_timer = NULL;
     const esp_timer_create_args_t timer_args = {
         .callback = tx_timer_cb,
         .arg = s_audio_tx_task,
         .name = "tx_slot",
     };
     ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tx_timer));
     ESP_ERROR_CHECK(esp_timer_start_periodic(tx_timer, SUB_INTERVAL_US));

     int64_t due = esp_timer_get_time() + SUB_INTERVAL_US;
     uint32_t tick = 0;

     ESP_LOGI(TAG, "ESP-NOW TX scheduler: RTN=%d sub=%dus timer-driven", RTN, SUB_INTERVAL_US);
     while (1) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t now = esp_timer_get_time();

        if (!s_audio_tx_enabled) {
            due = now + SUB_INTERVAL_US;
            tick = 0;
            continue;
        }

        if (now > due + 1000) {
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
                 } else {
                     s_espnow_queue_drops++;
                 }
             }
         }

         tick++;
         due += SUB_INTERVAL_US;
         if (due < now - (int64_t)(SUB_INTERVAL_US * 4)) {
             s_late_tx_ticks++;
             due = now + SUB_INTERVAL_US;
         }
     }
 }

 void app_main(void)
 {
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("source_udp", ESP_LOG_INFO);
    esp_log_level_set("udp_control", ESP_LOG_INFO);
    esp_log_level_set("source_web", ESP_LOG_WARN);
    /* INFO so STA / setup AP IP and Web UI URL (ESP_LOGI in source_wifi.c) are visible */
    esp_log_level_set("source_wifi", ESP_LOG_INFO);

    if (!source_config_room_id_valid(CONFIG_ROOM_AUDIO_ROOM_ID)) {
         ESP_LOGE(TAG, "Invalid room '%s' (expected XXX-XXXX)", CONFIG_ROOM_AUDIO_ROOM_ID);
         return;
     }
    ESP_ERROR_CHECK(source_config_init(CONFIG_ROOM_AUDIO_ROOM_ID));
    source_config_set_apply_callback(apply_runtime_config, NULL);

     s_per_ch = LC3_BYTES_PER_CH;
     s_frame_bytes = LC3_FRAME_BYTES;
     if (s_per_ch <= 0 || s_frame_bytes > MAX_FRAME_BYTES ||
         s_frame_bytes > ESPNOW_AUDIO_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Bad SBC payload size %d", s_frame_bytes);
         return;
     }

    apply_runtime_config(NULL);

     s_pcm_free_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
     s_pcm_full_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
     for (int i = 0; i < NUM_PCM_BLOCKS; i++) {
         pcm_block_t *p = &s_pcm_pool[i];
         xQueueSend(s_pcm_free_q, &p, 0);
     }

     s_tx_tokens = xSemaphoreCreateCounting(ESPNOW_MAX_IN_FLIGHT, ESPNOW_MAX_IN_FLIGHT);
     s_hello_queue = xQueueCreate(HANDSHAKE_QUEUE, sizeof(hello_event_t));
     s_peer_cleanup_q = xQueueCreate(HANDSHAKE_QUEUE, ESP_NOW_ETH_ALEN);
    s_control_queue_set = xQueueCreateSet(HANDSHAKE_QUEUE * 2);

    if (!s_pcm_free_q || !s_pcm_full_q || !s_tx_tokens ||
        !s_hello_queue || !s_peer_cleanup_q || !s_control_queue_set) {
         ESP_LOGE(TAG, "Queue/semaphore creation failed");
         return;
     }
    if (xQueueAddToSet(s_hello_queue, s_control_queue_set) != pdPASS ||
        xQueueAddToSet(s_peer_cleanup_q, s_control_queue_set) != pdPASS) {
        ESP_LOGE(TAG, "Control queue set setup failed");
        return;
    }

     wifi_espnow_init();
     ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
     ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_ERROR_CHECK(source_udp_control_start());
    ESP_ERROR_CHECK(source_udp_stream_start());
    source_wifi_set_audio_streaming(true);
    ESP_ERROR_CHECK(source_web_start());

    char room[ROOM_ID_LEN + 1];
    uint32_t stream_id = 0;
    get_runtime_room(room, &stream_id, NULL);
     ESP_LOGI(TAG, "CPU clock: %" PRIu32 " Hz config=%u MHz",
              (uint32_t)esp_clk_cpu_freq(), (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "Room %s on Wi-Fi ch %u", room, source_wifi_current_channel());
    ESP_LOGI(TAG, "Stereo SBC: %d Hz, %d us, %d-bit capture, %d SBC frames, %d B/pkt, "
              "RTN=%d sub=%dus stream=%" PRIu32 " phy=%s tick_hz=%u",
              AUDIO_RATE_HZ, LC3_DT_US, AUDIO_BITS_PER_SAMPLE,
             SBC_FRAMES_PER_PACKET, s_frame_bytes, RTN, SUB_INTERVAL_US,
             stream_id, AUDIO_PHY_NAME, (unsigned)configTICK_RATE_HZ);
     ESP_LOGI(TAG, "Audio payload integrity: CRC32 only (non-security); "
                   "session key is delivered by CMAC-authenticated ACCEPT");

    xTaskCreatePinnedToCore(control_task, "room_ctrl", 6144, NULL, 3, NULL, 0);
     xTaskCreatePinnedToCore(pcm1808_reader_task, "pcm1808_rx", 8192, NULL, 23, NULL, 1);
    xTaskCreatePinnedToCore(audio_tx_task, "espnow_tx", 6144, NULL, 20, NULL, 1);
     xTaskCreatePinnedToCore(audio_encode_task, "sbc_enc", 16384, NULL, 22, NULL, 1);
 }
