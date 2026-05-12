/*
 * Assistive Listening sink for CrowPanel P4.
 *
 * C6 handles ESP-NOW room discovery/authentication and receives duplicated
 * ESP-NOW LC3 frames from the source. C6 forwards verified audio frames to P4
 * over SDIO; P4 decodes and plays them through the selected I2S output.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/aes.h"
#include "bsp_board_extra.h"
#include "lc3.h"
#include "espnow_protocol.h"
#include "espnow_sink.h"

static const char *TAG = "assist_sink";

#define MAX_FRAME_BYTES      ESPNOW_AUDIO_MAX_FRAME_BYTES
#define AUDIO_RATE_HZ        48000
#define LC3_DT_US            7500
#define AUDIO_FRAME_SAMPLES  360
#define CODEC_FRAME_BYTES    ESPNOW_LC3_BYTES_PER_CH
#define FRAME_PAYLOAD_BYTES  ESPNOW_LC3_FRAME_BYTES
#define AUDIO_RAW_QUEUE_LEN  64
#define SOURCE_TIMEOUT_MS    10000
#define ROOM_SCAN_STALE_MS   4000

#define JB_SIZE              32
#define JB_MASK              (JB_SIZE - 1)
#define JB_PREFILL           3
#define AUX_DMA_FRAME_SAMPLES 120
#define AUX_VOLUME_BOOST_PCT 100

typedef struct {
    int32_t pcm_l[JB_SIZE][AUDIO_FRAME_SAMPLES];
    int32_t pcm_r[JB_SIZE][AUDIO_FRAME_SAMPLES];
    uint32_t write_idx;
    uint32_t read_idx;
    int      started;
    SemaphoreHandle_t mutex;
} jitter_buffer_t;

static bool s_initialized;
static espnow_state_t s_state = ESPNOW_STATE_NOT_INIT;
static bool s_c6_wifi_ok;
static bool s_c6_espnow_ok;
static uint32_t s_last_evt_ms;
static char s_c6_fw_version[32] = "unknown";
static bool s_c6_fw_valid;

static espnow_room_info_t s_rooms[ESPNOW_SINK_MAX_ROOMS];
static int s_room_count;
static SemaphoreHandle_t s_rooms_mutex;
static int s_selected_room = -1;

static espnow_sink_callbacks_t s_cbs;
static jitter_buffer_t s_jb;
static TaskHandle_t s_playback_task;

static i2s_chan_handle_t s_i2s_tx;
static uint8_t s_audio_key[ESPNOW_AUDIO_KEY_LEN];
static uint8_t s_frame_bytes;
static uint8_t s_channels;
static uint8_t s_audio_copy_count;
static volatile bool s_audio_cfg_valid;
static volatile int s_output_volume = 100;

static QueueHandle_t s_audio_raw_q;
static TaskHandle_t s_audio_decode_task;
static volatile bool s_audio_rx_active;
static uint32_t s_audio_stream_id;

static volatile uint32_t s_packets_rx;
static volatile uint32_t s_packets_lost;
static volatile uint32_t s_last_audio_ms;
static volatile uint32_t s_last_room_found_ms;

static volatile bool s_autoscan_enabled;
static volatile bool s_autoscan_autojoin;
static TaskHandle_t s_autoscan_task;
static SemaphoreHandle_t s_autoscan_signal;
static volatile bool s_autoscan_scan_done;
static uint8_t s_autoscan_target_mac[6];
static uint32_t s_autoscan_target_code;
static bool s_autoscan_target_valid;

__attribute__((unused))
static uint32_t rd24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void update_evt_time(void)
{
    s_last_evt_ms = now_ms();
}

static inline int32_t clamp_s24(int64_t sample)
{
    const int64_t max_s24 = 8388607;
    const int64_t min_s24 = -8388608;
    if (sample > max_s24) {
        return (int32_t)max_s24;
    }
    if (sample < min_s24) {
        return (int32_t)min_s24;
    }
    return (int32_t)sample;
}

static inline int32_t scale_pcm_s24(int32_t sample, int volume, int boost_pct)
{
    return clamp_s24(((int64_t)sample * volume * boost_pct) / 10000);
}

static inline int16_t s24_to_s16(int32_t sample)
{
    sample = clamp_s24(sample);
    return (int16_t)(sample >> 8);
}

void espnow_sink_set_output_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    s_output_volume = volume;
}

void espnow_sink_flush_aux_output(void)
{
    if (s_i2s_tx == NULL) {
        return;
    }

    static int32_t silence[AUDIO_FRAME_SAMPLES * 2] = {0};
    size_t written = 0;
    for (int i = 0; i < 12; ++i) {
        (void)i2s_channel_write(s_i2s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
    }
    (void)i2s_channel_disable(s_i2s_tx);
    (void)i2s_channel_enable(s_i2s_tx);
    for (int i = 0; i < 2; ++i) {
        (void)i2s_channel_write(s_i2s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
    }
}

static void stop_audio_rx(void);
static void start_audio_rx(void);

static esp_err_t aes_ctr_crypt(const uint8_t key[32], const uint8_t nonce[16],
                               const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context ctx;
    uint8_t nc[16];
    uint8_t stream[16] = {0};
    size_t offset = 0;
    memcpy(nc, nonce, sizeof(nc));
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ctr(&ctx, len, &offset, nc, stream, in, out);
    }
    mbedtls_aes_free(&ctx);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

static void make_audio_nonce(uint32_t stream_id, uint32_t seq, uint8_t nonce[16])
{
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

static esp_err_t send_cmd(uint32_t msg_id, const void *payload, size_t len)
{
    esp_err_t ret = esp_hosted_send_custom_data(msg_id, (const uint8_t *)payload, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_cmd 0x%lx failed: %s", (unsigned long)msg_id, esp_err_to_name(ret));
    }
    return ret;
}

static void autoscan_wake(void)
{
    if (s_autoscan_signal) {
        xSemaphoreGive(s_autoscan_signal);
    }
}

static void notify_state(espnow_state_t st)
{
    s_state = st;
    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(st);
    }
    autoscan_wake();
}

/* ───── Direct I2S output — bypass BSP, identical to standalone ───── */

static void init_direct_i2s(void)
{
    if (s_i2s_tx) return;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 2;
    chan_cfg.dma_frame_num = AUX_DMA_FRAME_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_4,
            .ws   = GPIO_NUM_2,
            .dout = GPIO_NUM_3,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
    ESP_LOGI(TAG, "Direct I2S init: 48kHz 32-bit stereo (LRCK=2 DOUT=3 BCK=4)");
}

/* ───── Jitter buffer (ring buffer + mutex) — identical to standalone sink ───── */

static void jb_init(jitter_buffer_t *jb)
{
    memset(jb, 0, sizeof(*jb));
    jb->mutex = xSemaphoreCreateMutex();
}

static int jb_level(jitter_buffer_t *jb)
{
    return (int)(jb->write_idx - jb->read_idx);
}

static void jb_push(jitter_buffer_t *jb, const int32_t *pcm_l, const int32_t *pcm_r)
{
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    if (jb_level(jb) >= JB_SIZE) {
        jb->read_idx++;
    }
    uint32_t idx = jb->write_idx & JB_MASK;
    memcpy(jb->pcm_l[idx], pcm_l, AUDIO_FRAME_SAMPLES * sizeof(int32_t));
    memcpy(jb->pcm_r[idx], pcm_r, AUDIO_FRAME_SAMPLES * sizeof(int32_t));
    jb->write_idx++;
    xSemaphoreGive(jb->mutex);
}

static int jb_pop(jitter_buffer_t *jb, int32_t *pcm_l, int32_t *pcm_r)
{
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    if (jb_level(jb) <= 0) {
        xSemaphoreGive(jb->mutex);
        return -1;
    }
    uint32_t idx = jb->read_idx & JB_MASK;
    memcpy(pcm_l, jb->pcm_l[idx], AUDIO_FRAME_SAMPLES * sizeof(int32_t));
    memcpy(pcm_r, jb->pcm_r[idx], AUDIO_FRAME_SAMPLES * sizeof(int32_t));
    jb->read_idx++;
    xSemaphoreGive(jb->mutex);
    return 0;
}

static void jb_reset(jitter_buffer_t *jb)
{
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    jb->write_idx = 0;
    jb->read_idx = 0;
    jb->started = 0;
    xSemaphoreGive(jb->mutex);
}

/* ───── Playback task — direct I2S, identical to standalone sink ───── */

static void playback_task_fn(void *arg)
{
    (void)arg;
    static int32_t pcm_l[AUDIO_FRAME_SAMPLES];
    static int32_t pcm_r[AUDIO_FRAME_SAMPLES];
    static int32_t i2s_buf[AUDIO_FRAME_SAMPLES * 2];

    ESP_LOGI(TAG, "Playback (core %d): prefill=%d", xPortGetCoreID(), JB_PREFILL);
    while (jb_level(&s_jb) < JB_PREFILL) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_jb.started = 1;
    ESP_LOGI(TAG, "Playback started!");

    while (1) {
        if (jb_pop(&s_jb, pcm_l, pcm_r) != 0) {
            memset(pcm_l, 0, sizeof(pcm_l));
            memset(pcm_r, 0, sizeof(pcm_r));
        }

        size_t written;
        if (s_output_volume >= 100 && AUX_VOLUME_BOOST_PCT == 100) {
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                i2s_buf[i << 1]       = pcm_l[i] << 8;
                i2s_buf[(i << 1) | 1] = pcm_r[i] << 8;
            }
        } else {
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                int32_t left = scale_pcm_s24(pcm_l[i], s_output_volume, AUX_VOLUME_BOOST_PCT);
                int32_t right = scale_pcm_s24(pcm_r[i], s_output_volume, AUX_VOLUME_BOOST_PCT);
                i2s_buf[i << 1]       = left << 8;
                i2s_buf[(i << 1) | 1] = right << 8;
            }
        }
        i2s_channel_write(s_i2s_tx, i2s_buf,
                          AUDIO_FRAME_SAMPLES * sizeof(int32_t) * 2,
                          &written, portMAX_DELAY);
    }
}

/* ───── ESP-NOW audio decode task: decrypt → dedupe/gap-fill → LC3 decode → PCM queue ───── */

static void audio_decode_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ESP-NOW audio decode task started (core %d)", xPortGetCoreID());

    while (!s_audio_cfg_valid) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    unsigned enc_sz = lc3_encoder_size(LC3_DT_US, AUDIO_RATE_HZ);
    unsigned dec_sz = lc3_decoder_size(LC3_DT_US, AUDIO_RATE_HZ);
    (void)enc_sz;

    void *dec_l_mem = heap_caps_aligned_alloc(8, dec_sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    void *dec_r_mem = heap_caps_aligned_alloc(8, dec_sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    lc3_decoder_t dec_l = lc3_setup_decoder(LC3_DT_US, AUDIO_RATE_HZ, 0, dec_l_mem);
    lc3_decoder_t dec_r = lc3_setup_decoder(LC3_DT_US, AUDIO_RATE_HZ, 0, dec_r_mem);

    int32_t *pcm_l = calloc(AUDIO_FRAME_SAMPLES, sizeof(int32_t));
    int32_t *pcm_r = calloc(AUDIO_FRAME_SAMPLES, sizeof(int32_t));
    uint8_t  decrypted[MAX_FRAME_BYTES];

    if (!dec_l || !dec_r || !pcm_l || !pcm_r) {
        ESP_LOGE(TAG, "LC3 decoder setup failed");
        free(dec_l_mem); free(dec_r_mem); free(pcm_l); free(pcm_r);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "LC3 decoder: dt=%uus samples=%d frame=%u (dec_mem=%u)",
             LC3_DT_US, AUDIO_FRAME_SAMPLES, FRAME_PAYLOAD_BYTES, dec_sz);

    espnow_evt_audio_raw_t pkt;
    uint32_t expected_seq = 0;
    bool have_expected = false;

    while (s_audio_rx_active) {
        if (xQueueReceive(s_audio_raw_q, &pkt, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        s_last_audio_ms = now_ms();

        if (pkt.stream_id != s_audio_stream_id ||
            pkt.frame_bytes != FRAME_PAYLOAD_BYTES ||
            pkt.channels != ESPNOW_CHANNELS ||
            pkt.lc3_dt_us != LC3_DT_US ||
            pkt.sample_rate_hz != AUDIO_RATE_HZ) {
            continue;
        }

        if (!have_expected) {
            expected_seq = pkt.seq;
            have_expected = true;
        }

        if ((int32_t)(pkt.seq - expected_seq) < 0) {
            s_packets_lost++;
            continue;
        }

        while (expected_seq != pkt.seq) {
            (void)lc3_decode(dec_l, NULL, 0, LC3_PCM_FORMAT_S24, pcm_l, 1);
            (void)lc3_decode(dec_r, NULL, 0, LC3_PCM_FORMAT_S24, pcm_r, 1);
            jb_push(&s_jb, pcm_l, pcm_r);
            s_packets_lost++;
            expected_seq++;
        }

        uint8_t nonce[16];
        make_audio_nonce(pkt.stream_id, pkt.seq, nonce);
        if (aes_ctr_crypt(s_audio_key, nonce, pkt.payload, decrypted, FRAME_PAYLOAD_BYTES) != ESP_OK) {
            s_packets_lost++;
            continue;
        }

        int ret_l = lc3_decode(dec_l, decrypted, CODEC_FRAME_BYTES, LC3_PCM_FORMAT_S24, pcm_l, 1);
        int ret_r = lc3_decode(dec_r, decrypted + CODEC_FRAME_BYTES, CODEC_FRAME_BYTES, LC3_PCM_FORMAT_S24, pcm_r, 1);

        if (ret_l < 0 || ret_r < 0) {
            s_packets_lost++;
            continue;
        }

        jb_push(&s_jb, pcm_l, pcm_r);
        s_packets_rx++;
        expected_seq++;
    }

    free(pcm_l); free(pcm_r); free(dec_l_mem); free(dec_r_mem);
    s_audio_decode_task = NULL;
    vTaskDelete(NULL);
}

static int find_or_create_room(const espnow_evt_room_t *r)
{
    int idx = -1;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; i++) {
        if (s_rooms[i].stream_id == r->stream_id || memcmp(s_rooms[i].mac, r->mac, 6) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0 && s_room_count < ESPNOW_SINK_MAX_ROOMS) {
        idx = s_room_count++;
        memset(&s_rooms[idx], 0, sizeof(s_rooms[idx]));
    }
    if (idx >= 0) {
        espnow_room_info_t *room = &s_rooms[idx];
        memcpy(room->mac, r->mac, 6);
        room->wifi_channel = r->wifi_channel;
        room->room_code = r->room_code;
        room->stream_id = r->stream_id;
        room->rssi = r->rssi;
        room->valid = true;
        memset(room->name, 0, sizeof(room->name));
        memcpy(room->name, r->name, sizeof(r->name));
        s_last_room_found_ms = now_ms();
    }
    xSemaphoreGive(s_rooms_mutex);
    return idx;
}

static void on_evt_status(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_status_t)) {
        return;
    }

    const espnow_evt_status_t *evt = (const espnow_evt_status_t *)data;
    s_c6_wifi_ok = evt->wifi_init != 0;
    s_c6_espnow_ok = evt->espnow_init != 0;
    notify_state((espnow_state_t)evt->state);
}

static void on_evt_room_found(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_room_t)) {
        return;
    }

    const espnow_evt_room_t *room_evt = (const espnow_evt_room_t *)data;
    int idx = find_or_create_room(room_evt);
    if (idx >= 0 && s_cbs.on_room_found) {
        s_cbs.on_room_found(&s_rooms[idx]);
    }
}

static void on_evt_scan_done(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_scan_done_t)) {
        return;
    }

    const espnow_evt_scan_done_t *scan_done = (const espnow_evt_scan_done_t *)data;
    s_autoscan_scan_done = true;
    notify_state(ESPNOW_STATE_IDLE);
    if (s_cbs.on_scan_done) {
        s_cbs.on_scan_done(scan_done->room_count);
    }
}

static void on_evt_joined(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_joined_t)) {
        return;
    }

    const espnow_evt_joined_t *joined = (const espnow_evt_joined_t *)data;
    ESP_LOGI(TAG, "Joined room code=0x%08" PRIX32 " stream=%" PRIu32,
             joined->room_code, joined->stream_id);
    s_packets_rx = 0;
    s_packets_lost = 0;
    s_last_audio_ms = now_ms();
    s_audio_cfg_valid = false;
    s_audio_stream_id = joined->stream_id;
    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
    notify_state(ESPNOW_STATE_CONNECTED);
}

static void on_evt_left(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)data;
    (void)len;
    (void)ctx;
    update_evt_time();
    stop_audio_rx();
    s_selected_room = -1;
    s_audio_cfg_valid = false;
    jb_reset(&s_jb);
    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
    notify_state(ESPNOW_STATE_IDLE);
}

static void on_evt_stats(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_stats_t)) {
        return;
    }

    const espnow_evt_stats_t *stats = (const espnow_evt_stats_t *)data;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; ++i) {
        s_rooms[i].rssi = stats->rssi_last;
    }
    xSemaphoreGive(s_rooms_mutex);
}

static void on_evt_error(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_error_t)) {
        return;
    }

    const espnow_evt_error_t *err = (const espnow_evt_error_t *)data;
    ESP_LOGE(TAG, "C6 error %ld: %.64s", (long)err->error_code, err->message);
    notify_state(ESPNOW_STATE_IDLE);
}

static void on_evt_fw_ver(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_fw_ver_t)) {
        return;
    }

    const espnow_evt_fw_ver_t *ver = (const espnow_evt_fw_ver_t *)data;
    memset(s_c6_fw_version, 0, sizeof(s_c6_fw_version));
    memcpy(s_c6_fw_version, ver->version, sizeof(ver->version));
    s_c6_fw_version[sizeof(s_c6_fw_version) - 1] = '\0';
    s_c6_fw_valid = s_c6_fw_version[0] != '\0';
    ESP_LOGI(TAG, "C6 firmware: version='%s' project='%.32s'",
             s_c6_fw_valid ? s_c6_fw_version : "unknown", ver->project);
}

static void on_evt_audio_config(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_audio_config_t)) {
        return;
    }

    const espnow_evt_audio_config_t *cfg = (const espnow_evt_audio_config_t *)data;
    uint8_t channels = cfg->channels;
    if (channels == 0) channels = 1;
    if (cfg->lc3_dt_us != LC3_DT_US ||
        cfg->sample_rate_hz != AUDIO_RATE_HZ ||
        cfg->frame_bytes != FRAME_PAYLOAD_BYTES || channels != ESPNOW_CHANNELS) {
        ESP_LOGE(TAG, "Unsupported ESP-NOW audio config dt=%u rate=%u frame=%u ch=%u",
                 cfg->lc3_dt_us, cfg->sample_rate_hz, cfg->frame_bytes, channels);
        return;
    }

    s_audio_copy_count = cfg->audio_copy_count ? cfg->audio_copy_count : ESPNOW_AUDIO_COPY_DEFAULT;
    s_frame_bytes = cfg->frame_bytes;
    s_channels = channels;
    s_audio_cfg_valid = true;
    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
    ESP_LOGI(TAG, "ESP-NOW audio config from C6: frame=%u channels=%u copies=%u",
             cfg->frame_bytes, channels, s_audio_copy_count);
}

static void on_evt_audio(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    if (!data || len < sizeof(espnow_evt_audio_raw_t) || !s_audio_cfg_valid || !s_audio_raw_q) {
        return;
    }

    const espnow_evt_audio_raw_t *pkt = (const espnow_evt_audio_raw_t *)data;
    if (pkt->frame_bytes != FRAME_PAYLOAD_BYTES ||
        pkt->channels != ESPNOW_CHANNELS ||
        pkt->copy_count != ESPNOW_AUDIO_COPY_DEFAULT ||
        pkt->copy_idx >= pkt->copy_count) {
        return;
    }

    if (xQueueSend(s_audio_raw_q, pkt, 0) != pdTRUE) {
        espnow_evt_audio_raw_t drop;
        (void)xQueueReceive(s_audio_raw_q, &drop, 0);
        (void)xQueueSend(s_audio_raw_q, pkt, 0);
        s_packets_lost++;
    }
}

/* ───── ESP-NOW audio receive (triggered after AUDIO_KEY from C6) ───── */

static void start_audio_rx(void)
{
    if (s_audio_decode_task) return;

    if (!s_audio_raw_q) {
        s_audio_raw_q = xQueueCreate(AUDIO_RAW_QUEUE_LEN, sizeof(espnow_evt_audio_raw_t));
    } else {
        xQueueReset(s_audio_raw_q);
    }

    s_audio_rx_active = true;
    xTaskCreatePinnedToCore(audio_decode_task_fn, "espnow_dec", 20480, NULL, 23,
                            &s_audio_decode_task, 1);
    ESP_LOGI(TAG, "ESP-NOW audio RX started: frame=%u ch=%u copies=%u",
             s_frame_bytes, s_channels, s_audio_copy_count);
}

static void stop_audio_rx(void)
{
    s_audio_rx_active = false;

    for (int i = 0; i < 40 && s_audio_decode_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (s_audio_decode_task) {
        ESP_LOGW(TAG, "Force-deleting audio decode task");
        vTaskDelete(s_audio_decode_task);
        s_audio_decode_task = NULL;
    }

    jb_reset(&s_jb);

    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
}

static void on_evt_audio_key(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    update_evt_time();
    if (!data || len < sizeof(espnow_evt_audio_key_t)) {
        return;
    }

    const espnow_evt_audio_key_t *key = (const espnow_evt_audio_key_t *)data;
    memcpy(s_audio_key, key->audio_key, ESPNOW_AUDIO_KEY_LEN);
    s_frame_bytes = key->frame_bytes;
    s_channels = key->channels ? key->channels : ESPNOW_CHANNELS;
    s_audio_cfg_valid = true;

    ESP_LOGI(TAG, "Received ESP-NOW audio key: frame=%u ch_count=%u dt=%u",
             s_frame_bytes, s_channels, LC3_DT_US);

    stop_audio_rx();
    start_audio_rx();
}

static esp_err_t register_event_handlers(void)
{
    struct handler_entry {
        uint32_t id;
        void (*cb)(uint32_t, const uint8_t *, size_t, void *);
    } handlers[] = {
        { ESPNOW_MSG_EVT_STATUS,       on_evt_status },
        { ESPNOW_MSG_EVT_ROOM_FOUND,   on_evt_room_found },
        { ESPNOW_MSG_EVT_SCAN_DONE,    on_evt_scan_done },
        { ESPNOW_MSG_EVT_JOINED,       on_evt_joined },
        { ESPNOW_MSG_EVT_LEFT,         on_evt_left },
        { ESPNOW_MSG_EVT_STATS,        on_evt_stats },
        { ESPNOW_MSG_EVT_ERROR,        on_evt_error },
        { ESPNOW_MSG_EVT_FW_VER,       on_evt_fw_ver },
        { ESPNOW_MSG_EVT_AUDIO,        on_evt_audio },
        { ESPNOW_MSG_EVT_AUDIO_CONFIG, on_evt_audio_config },
        { ESPNOW_MSG_EVT_AUDIO_KEY,    on_evt_audio_key },
    };

    for (int i = 0; i < (int)(sizeof(handlers) / sizeof(handlers[0])); ++i) {
        esp_err_t ret = esp_hosted_register_custom_callback(handlers[i].id, handlers[i].cb, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register handler 0x%lx failed: %s",
                     (unsigned long)handlers[i].id, esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

static int autoscan_pick_best_room(void)
{
    int best = -1;
    int8_t best_rssi = -127;

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; ++i) {
        if (s_rooms[i].valid && s_rooms[i].rssi > best_rssi) {
            best = i;
            best_rssi = s_rooms[i].rssi;
        }
    }
    xSemaphoreGive(s_rooms_mutex);

    return best;
}

static int find_target_room(void)
{
    int idx = -1;

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; ++i) {
        if (!s_rooms[i].valid) {
            continue;
        }
        if ((memcmp(s_rooms[i].mac, s_autoscan_target_mac, 6) == 0) ||
            (s_rooms[i].room_code == s_autoscan_target_code)) {
            idx = i;
            break;
        }
    }
    xSemaphoreGive(s_rooms_mutex);

    return idx;
}

static void autoscan_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Autoscan task started (autojoin=%d)", s_autoscan_autojoin ? 1 : 0);
    espnow_sink_enable();

    int reconnect_attempts = 0;
    const int reconnect_max = SOURCE_TIMEOUT_MS / 500;

    while (s_autoscan_enabled) {
        espnow_state_t state = s_state;
        if (state == ESPNOW_STATE_NOT_INIT) {
            espnow_sink_enable();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (state == ESPNOW_STATE_CONNECTED) {
            xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(500));
            uint32_t last_audio_ms = s_last_audio_ms;
            if (last_audio_ms != 0 && (now_ms() - last_audio_ms) >= SOURCE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Source timeout: no ESP-NOW audio for %u ms; reconnecting",
                         SOURCE_TIMEOUT_MS);
                s_audio_cfg_valid = false;
                jb_reset(&s_jb);
                if (s_audio_raw_q) {
                    xQueueReset(s_audio_raw_q);
                }
                notify_state(ESPNOW_STATE_IDLE);
            }
            reconnect_attempts = 0;
            continue;
        }

        if (state == ESPNOW_STATE_IDLE) {
            if (s_autoscan_target_valid) {
                int target = find_target_room();
                if (target >= 0 && reconnect_attempts < reconnect_max) {
                    reconnect_attempts++;
                    ESP_LOGI(TAG, "Reconnect attempt %d/%d to selected room",
                             reconnect_attempts, reconnect_max);
                    espnow_sink_join_room(target);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                if (reconnect_attempts >= reconnect_max) {
                    ESP_LOGW(TAG, "Reconnect window exhausted; returning to manual room selection");
                    s_autoscan_target_valid = false;
                    memset(s_autoscan_target_mac, 0, sizeof(s_autoscan_target_mac));
                    s_autoscan_target_code = 0;
                    reconnect_attempts = 0;
                }
            }

            s_autoscan_scan_done = false;
            espnow_sink_start_scan();
            for (uint32_t waited = 0;
                 waited < ESPNOW_SCAN_LISTEN_MS + 500 && s_autoscan_enabled;
                 waited += 100) {
                if (s_autoscan_scan_done) {
                    break;
                }
                xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(100));
            }

            if (!s_autoscan_target_valid && s_autoscan_autojoin) {
                int best = autoscan_pick_best_room();
                if (best >= 0) {
                    ESP_LOGI(TAG, "Auto-joining strongest room because autojoin is enabled");
                    espnow_sink_join_room(best);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(300));
    }

    ESP_LOGI(TAG, "Autoscan task exiting");
    s_autoscan_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t espnow_sink_init(const espnow_sink_callbacks_t *cbs)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (cbs) {
        s_cbs = *cbs;
    }

    s_rooms_mutex = xSemaphoreCreateMutex();
    s_autoscan_signal = xSemaphoreCreateBinary();
    jb_init(&s_jb);
    if (!s_rooms_mutex || !s_autoscan_signal || !s_jb.mutex) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(register_event_handlers(), TAG, "register SDIO handlers");
    init_direct_i2s();

    /* Keep AUX I2S above decode so the output clock never waits on app work. */
    xTaskCreatePinnedToCore(playback_task_fn, "playback", 4096, NULL, 24, &s_playback_task, 1);

    s_state = ESPNOW_STATE_NOT_INIT;
    s_last_evt_ms = now_ms();
    s_c6_fw_valid = false;
    snprintf(s_c6_fw_version, sizeof(s_c6_fw_version), "%s", "unknown");
    s_initialized = true;
    s_audio_cfg_valid = false;
    s_audio_copy_count = ESPNOW_AUDIO_COPY_DEFAULT;
    s_autoscan_autojoin = false;
    ESP_LOGI(TAG, "Assistive sink ready: C6 verifies rooms, P4 receives ESP-NOW audio via SDIO");
    return ESP_OK;
}

void espnow_sink_set_callbacks(const espnow_sink_callbacks_t *cbs)
{
    if (cbs) {
        s_cbs = *cbs;
    } else {
        memset(&s_cbs, 0, sizeof(s_cbs));
    }
}

void espnow_sink_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    stop_audio_rx();
    espnow_sink_set_autoscan(false);
    espnow_sink_leave_room();
    espnow_sink_disable();

    if (s_playback_task) {
        vTaskDelete(s_playback_task);
        s_playback_task = NULL;
    }

    uint32_t ids[] = {
        ESPNOW_MSG_EVT_STATUS, ESPNOW_MSG_EVT_ROOM_FOUND, ESPNOW_MSG_EVT_SCAN_DONE,
        ESPNOW_MSG_EVT_JOINED, ESPNOW_MSG_EVT_LEFT, ESPNOW_MSG_EVT_STATS,
        ESPNOW_MSG_EVT_ERROR, ESPNOW_MSG_EVT_FW_VER, ESPNOW_MSG_EVT_AUDIO,
        ESPNOW_MSG_EVT_AUDIO_CONFIG, ESPNOW_MSG_EVT_AUDIO_KEY,
    };
    for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); ++i) {
        esp_hosted_register_custom_callback(ids[i], NULL, NULL);
    }

    jb_reset(&s_jb);
    if (s_audio_raw_q) {
        vQueueDelete(s_audio_raw_q);
        s_audio_raw_q = NULL;
    }
    if (s_rooms_mutex) {
        vSemaphoreDelete(s_rooms_mutex);
        s_rooms_mutex = NULL;
    }
    if (s_autoscan_signal) {
        vSemaphoreDelete(s_autoscan_signal);
        s_autoscan_signal = NULL;
    }

    s_initialized = false;
    s_state = ESPNOW_STATE_NOT_INIT;
}

esp_err_t espnow_sink_enable(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_cmd(ESPNOW_MSG_CMD_INIT, NULL, 0);
}

esp_err_t espnow_sink_disable(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_c6_wifi_ok = false;
    s_c6_espnow_ok = false;
    s_audio_cfg_valid = false;
    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
    return send_cmd(ESPNOW_MSG_CMD_DEINIT, NULL, 0);
}

void espnow_sink_set_autoscan(bool enable)
{
    if (!s_initialized) {
        return;
    }
    if (enable) {
        if (s_autoscan_task) {
            return;
        }
        s_autoscan_enabled = true;
        xTaskCreatePinnedToCore(autoscan_task_fn, "sink_autoscan", 4096, NULL, 4, &s_autoscan_task, 0);
    } else {
        s_autoscan_enabled = false;
        autoscan_wake();
    }
}

bool espnow_sink_get_autoscan(void)
{
    return s_autoscan_enabled;
}

void espnow_sink_set_autoscan_autojoin(bool autojoin)
{
    s_autoscan_autojoin = autojoin;
    autoscan_wake();
}

esp_err_t espnow_sink_start_scan(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == ESPNOW_STATE_CONNECTED || s_state == ESPNOW_STATE_JOINING) {
        send_cmd(ESPNOW_MSG_CMD_LEAVE, NULL, 0);
        s_audio_cfg_valid = false;
        s_selected_room = -1;
        jb_reset(&s_jb);
        if (s_audio_raw_q) {
            xQueueReset(s_audio_raw_q);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_autoscan_scan_done = false;
    notify_state(ESPNOW_STATE_SCANNING);
    return send_cmd(ESPNOW_MSG_CMD_SCAN, NULL, 0);
}

void espnow_sink_stop_scan(void)
{
    if (!s_initialized) {
        return;
    }
    send_cmd(ESPNOW_MSG_CMD_STOP_SCAN, NULL, 0);
}

int espnow_sink_get_rooms(espnow_room_info_t *rooms, int max_rooms)
{
    if (!s_rooms_mutex || !rooms || max_rooms <= 0) {
        return 0;
    }

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    if (s_room_count > 0 && s_last_room_found_ms != 0 &&
        (now_ms() - s_last_room_found_ms) > ROOM_SCAN_STALE_MS) {
        memset(s_rooms, 0, sizeof(s_rooms));
        s_room_count = 0;
    }
    int count = (s_room_count < max_rooms) ? s_room_count : max_rooms;
    memcpy(rooms, s_rooms, count * sizeof(espnow_room_info_t));
    xSemaphoreGive(s_rooms_mutex);
    return count;
}

esp_err_t espnow_sink_join_room(int room_index)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    espnow_cmd_join_t cmd = {0};
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    if (room_index < 0 || room_index >= s_room_count || !s_rooms[room_index].valid) {
        xSemaphoreGive(s_rooms_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if ((s_state == ESPNOW_STATE_CONNECTED || s_state == ESPNOW_STATE_JOINING) &&
        room_index == s_selected_room) {
        xSemaphoreGive(s_rooms_mutex);
        return ESP_OK;
    }

    espnow_room_info_t *room = &s_rooms[room_index];
    memcpy(cmd.mac, room->mac, 6);
    cmd.wifi_channel = room->wifi_channel;
    cmd.room_code = room->room_code;
    cmd.stream_id = room->stream_id;
    s_selected_room = room_index;
    memcpy(s_autoscan_target_mac, room->mac, 6);
    s_autoscan_target_code = room->room_code;
    s_autoscan_target_valid = true;
    xSemaphoreGive(s_rooms_mutex);

    s_audio_cfg_valid = false;
    s_last_audio_ms = now_ms();
    jb_reset(&s_jb);
    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
    notify_state(ESPNOW_STATE_JOINING);
    ESP_LOGI(TAG, "Requesting verified join room=0x%08" PRIX32 " stream=%" PRIu32,
             cmd.room_code, cmd.stream_id);
    return send_cmd(ESPNOW_MSG_CMD_JOIN, &cmd, sizeof(cmd));
}

bool espnow_sink_leave_room(void)
{
    if (!s_initialized) {
        return true;
    }

    stop_audio_rx();
    s_selected_room = -1;
    s_autoscan_target_valid = false;
    memset(s_autoscan_target_mac, 0, sizeof(s_autoscan_target_mac));
    s_autoscan_target_code = 0;
    s_audio_cfg_valid = false;
    jb_reset(&s_jb);
    if (s_audio_raw_q) {
        xQueueReset(s_audio_raw_q);
    }
    notify_state(ESPNOW_STATE_IDLE);

    bool c6_alive = (now_ms() - s_last_evt_ms) < 5000;
    if (c6_alive) {
        esp_err_t ret = send_cmd(ESPNOW_MSG_CMD_LEAVE, NULL, 0);
        if (ret != ESP_OK) {
            c6_alive = false;
        }
    }
    return c6_alive;
}

bool espnow_sink_is_connected(void)
{
    return s_state == ESPNOW_STATE_CONNECTED;
}

espnow_state_t espnow_sink_get_state(void)
{
    return s_state;
}

bool espnow_sink_is_c6_ready(void)
{
    return s_c6_wifi_ok && s_c6_espnow_ok;
}

void espnow_sink_get_stats(uint32_t *packets_rx, uint32_t *packets_lost)
{
    if (packets_rx) {
        *packets_rx = s_packets_rx;
    }
    if (packets_lost) {
        *packets_lost = s_packets_lost;
    }
}

uint32_t espnow_sink_get_room_code(void)
{
    if (s_state != ESPNOW_STATE_CONNECTED || s_selected_room < 0) {
        return 0;
    }
    uint32_t code = 0;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    if (s_selected_room >= 0 && s_selected_room < s_room_count) {
        code = s_rooms[s_selected_room].room_code;
    }
    xSemaphoreGive(s_rooms_mutex);
    return code;
}

bool espnow_sink_get_connected_room(espnow_room_info_t *room)
{
    if (!room || s_state != ESPNOW_STATE_CONNECTED || s_selected_room < 0) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    if (s_selected_room >= 0 && s_selected_room < s_room_count) {
        *room = s_rooms[s_selected_room];
        ok = room->valid;
    }
    xSemaphoreGive(s_rooms_mutex);
    return ok;
}

void espnow_sink_request_status(void)
{
    send_cmd(ESPNOW_MSG_CMD_GET_STATUS, NULL, 0);
}

void espnow_sink_request_fw_version(void)
{
    send_cmd(ESPNOW_MSG_CMD_GET_FW_VER, NULL, 0);
}

bool espnow_sink_get_c6_fw_version(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }

    snprintf(out, out_len, "%s", s_c6_fw_version);
    return s_c6_fw_valid;
}
