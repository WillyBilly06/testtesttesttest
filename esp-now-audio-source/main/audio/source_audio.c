// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "../core/source_app_internal.h"
#include "../ecast_source.h"
#include "../ecast_protocol.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

/* ── PCM1808 I2S-in pin map (ESP32 acts as I2S master) ──────────────
 *
 *   PCM1808 pin   ESP32 GPIO    Role
 *   SCK / SCKI    GPIO  0       Master clock out (12.288 MHz = 256×Fs)
 *   BCK           GPIO 27       Bit  clock  out (64×Fs  = 3.072 MHz)
 *   LRCK          GPIO 25       Word/LR clock out (Fs   =   48 kHz)
 *   DOUT          GPIO 26       Serial data in  (24-bit MSB-aligned in 32-bit slot)
 *
 * APLL is used for the MCLK domain so the 256×Fs clock is jitter-free.
 * PCM1808's FMT pin must be strapped for Philips/I2S mode (default).
 */
#define PIN_I2S_MCLK   GPIO_NUM_0
#define PIN_I2S_BCLK   GPIO_NUM_27
#define PIN_I2S_LRCK   GPIO_NUM_25
#define PIN_I2S_DIN    GPIO_NUM_26

/* One DMA descriptor = one 5 ms LC3plus frame worth of stereo samples.
 * Four descriptors keep 20 ms of buffering, which absorbs RTOS jitter
 * between i2s_channel_read() calls without overflowing. */
#define I2S_DMA_DESC_NUM    4
#define I2S_DMA_FRAME_NUM   SAMPLES_PER_FRAME   /* 240 stereo frames */

static i2s_chan_handle_t s_i2s_rx = NULL;

const char *espnow_rate_name(wifi_phy_rate_t rate) {
    switch (rate) {
        case WIFI_PHY_RATE_6M:        return "6M";
        case WIFI_PHY_RATE_12M:       return "12M";
        case WIFI_PHY_RATE_24M:       return "24M";
        case WIFI_PHY_RATE_MCS0_LGI:  return "MCS0-LGI";
        case WIFI_PHY_RATE_MCS1_LGI:  return "MCS1-LGI";
        case WIFI_PHY_RATE_MCS2_LGI:  return "MCS2-LGI";
        case WIFI_PHY_RATE_MCS0_SGI:  return "MCS0-SGI";
        case WIFI_PHY_RATE_MCS1_SGI:  return "MCS1-SGI";
        case WIFI_PHY_RATE_MCS2_SGI:  return "MCS2-SGI";
        default: return "other";
    }
}

wifi_phy_rate_t espnow_rate_step_down(wifi_phy_rate_t rate) {
#if ESPNOW_HAS_RATE_54M
    if (rate == WIFI_PHY_RATE_54M) return WIFI_PHY_RATE_24M;
#endif
    if (rate == WIFI_PHY_RATE_24M) return WIFI_PHY_RATE_12M;
    if (rate == WIFI_PHY_RATE_12M) return WIFI_PHY_RATE_6M;
    return WIFI_PHY_RATE_6M;
}

wifi_phy_rate_t espnow_rate_step_up(wifi_phy_rate_t rate) {
#if ESPNOW_HAS_RATE_54M
    if (rate == WIFI_PHY_RATE_24M) return WIFI_PHY_RATE_54M;
#endif
    if (rate == WIFI_PHY_RATE_6M) return WIFI_PHY_RATE_12M;
    if (rate == WIFI_PHY_RATE_12M) return WIFI_PHY_RATE_24M;
#if ESPNOW_HAS_RATE_54M
    return WIFI_PHY_RATE_54M;
#else
    return WIFI_PHY_RATE_24M;
#endif
}

void apply_espnow_rate_all_peers(wifi_phy_rate_t rate) {
    esp_now_rate_config_t rcfg = {
        .phymode = ESPNOW_PHY_MODE,
        .rate = rate,
        .ersu = false,
        .dcm = false,
    };
    (void)esp_now_set_peer_rate_config(BROADCAST_MAC, &rcfg);

    for (int i = 0; i < MAX_SINKS; i++) {
        if (!sinks[i].in_use) continue;
        if (!esp_now_is_peer_exist(sinks[i].mac)) continue;
        (void)esp_now_set_peer_rate_config(sinks[i].mac, &rcfg);
    }

    espnow_rate_current = rate;
    ESP_LOGW(TAG, "ESP-NOW PHY rate adjusted to %s", espnow_rate_name(rate));
}

static void lc3_init_encoder(void) {
    for (int ch = 0; ch < CHANNELS; ch++) {
        lc3_enc[ch] = lc3_setup_encoder(LC3_FRAME_US, SAMPLE_RATE_HZ, 0, &lc3_enc_mem[ch]);
        if (!lc3_enc[ch]) {
            ESP_LOGE(TAG, "lc3_setup_encoder failed ch=%d", ch);
            abort();
        }
        lc3_encoder_disable_ltpf(lc3_enc[ch]);
    }
    ESP_LOGI(TAG, "LC3 encoder ready (%dus, %dHz, %dB/ch)", LC3_FRAME_US, SAMPLE_RATE_HZ, LC3_BYTES_PER_CH);
}

static bool lc3_encode_stereo_s24(const int32_t *pcm_interleaved_s24, uint8_t *out_payload) {
    for (int ch = 0; ch < CHANNELS; ch++) {
        uint8_t *out = out_payload + ch * LC3_BYTES_PER_CH;
        int rc = lc3_encode(lc3_enc[ch], LC3_PCM_FORMAT_S24,
                            pcm_interleaved_s24 + ch, CHANNELS,
                            LC3_BYTES_PER_CH, out);
        if (rc != 0) return false;
    }
    return true;
}

/* ── PCM ring shared between capture (producer) and emitter (consumer) ──
 *
 * The capture side is allowed to block on i2s_channel_read() — it only
 * ever stalls itself, never the audio cadence. The emitter pulls a
 * pre-captured PCM block per 5 ms slot, so its only inline work is one
 * LC3plus encode + ecast publish (about 1 ms), leaving roughly 4 ms of slack.
 *
 * Pool of NUM_PCM_BLOCKS pre-allocated S24 frames cycled between two SPSC
 * queues:
 *   - s_pcm_free_q: empty blocks the decoder may fill
 *   - s_pcm_full_q: filled blocks ready for the emitter
 */
#define NUM_PCM_BLOCKS 10  /* 10 x 5 ms = 50 ms decoded headroom */

typedef struct {
    int32_t pcm_s24[SAMPLES_PER_FRAME * CHANNELS];
} pcm_block_t;

static pcm_block_t   s_pcm_pool[NUM_PCM_BLOCKS];
static QueueHandle_t s_pcm_free_q;
static QueueHandle_t s_pcm_full_q;
static volatile uint32_t s_pcm_underruns = 0;

/* Precise pacing helper.
 *
 * Strategy: yield the CPU only as long as we are safely far from the
 * deadline (>= 2 ms), using a shift-based us→tick approximation that
 * deliberately *under*-sleeps (>>10 ≈ ÷1024 vs. true ÷1000 → ~2.4 % short).
 * Then busy-wait the residual via esp_rom_delay_us() so we *never*
 * overshoot the deadline by a tick boundary.
 *
 * Tick rate is 1000 Hz on this build (1 ms per tick), so:
 *     ticks ≈ (us_remaining - 2000) >> 10
 * is conservative and the final arithmetic uses only a subtract + shift —
 * no division anywhere on the audio path. */
static inline void wait_until_us(int64_t target_us) {
    int64_t now = esp_timer_get_time();
    int64_t remain = target_us - now;
    if (remain > 2000) {
        uint32_t safe_us = (uint32_t)(remain - 2000);
        TickType_t ticks = (TickType_t)(safe_us >> 10);
        if (ticks > 0) vTaskDelay(ticks);
    }
    /* Busy-wait the residual — guaranteed ≤ ~2 ms by construction. */
    while (1) {
        now = esp_timer_get_time();
        remain = target_us - now;
        if (remain <= 0) return;
        esp_rom_delay_us((uint32_t)remain);
    }
}

/* The legacy beacon_task is replaced by ecast_source's built-in beacon
 * emitter (see ecast_source.c). Stub retained so any stray xTaskCreate
 * reference still compiles cleanly; it just exits. */
void beacon_task(void *arg) {
    (void)arg;
    vTaskDelete(NULL);
}

/* ── PCM1808 I2S bring-up ───────────────────────────────────────────
 *
 * Configures I2S0 in master RX mode with Philips framing, 32-bit slot
 * carrying the PCM1808's 24-bit MSB-aligned sample. MCLK (256×Fs) is
 * driven by the APLL so jitter stays under the PCM1808's spec window.
 *
 * Returns ESP_OK on success; callers should bail out if the channel
 * couldn't be opened because the capture task has nothing to do without it.
 */
static esp_err_t pcm1808_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear    = false;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* APLL + 256×Fs MCLK — required for clean PCM1808 operation. */
    std_cfg.clk_cfg.clk_src       = I2S_CLK_SRC_APLL;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
        return err;
    }

    ESP_LOGI(TAG, "PCM1808 I2S RX up: MCLK=GPIO%d BCLK=GPIO%d LRCK=GPIO%d DIN=GPIO%d "
                  "fs=%d 24b/32b Philips APLL",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DIN,
             SAMPLE_RATE_HZ);
    return ESP_OK;
}

void audio_capture_encode_task(void *arg) {
    (void)arg;

    /* Bring the PCM1808 link up before LC3 so the encoder starts on a
     * real audio frame rather than whatever garbage the DMA ring had at
     * boot. */
    if (pcm1808_i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "PCM1808 init failed, capture task exiting");
        vTaskDelete(NULL);
        return;
    }

    lc3_init_encoder();

    /* Lazily create the PCM exchange queues + seed the free pool. The
     * emitter task creates these in its own init path too; whichever runs
     * first wins. */
    if (!s_pcm_free_q) {
        s_pcm_free_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
        s_pcm_full_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
        for (int i = 0; i < NUM_PCM_BLOCKS; i++) {
            pcm_block_t *p = &s_pcm_pool[i];
            xQueueSend(s_pcm_free_q, &p, 0);
        }
    }

    const size_t frame_bytes = SAMPLES_PER_FRAME * CHANNELS * sizeof(int32_t);

    /* Discard the first I2S frame — the PCM1808 needs a few ms of SCKI
     * before its modulator settles, and the initial DMA descriptor can
     * contain pre-enable noise. The throwaway buffer is `static` so it
     * doesn't blow ~2.8 KB of task stack just for one read. */
    {
        static int32_t junk[SAMPLES_PER_FRAME * CHANNELS];
        size_t got = 0;
        (void)i2s_channel_read(s_i2s_rx, junk, sizeof(junk), &got, pdMS_TO_TICKS(200));
    }

    while (1) {
        /* Acquire an empty block. Producer-side blocking is fine — it only
         * stalls the decoder, never the emitter (cadence) path. */
        pcm_block_t *blk = NULL;
        if (xQueueReceive(s_pcm_free_q, &blk, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* ── Capture SAMPLES_PER_FRAME stereo sample-pairs from PCM1808 ──
         *
         * i2s_channel_read is the natural rate limiter: it blocks until
         * the DMA ring has the requested number of bytes, so this loop
         * runs at exactly one 5 ms period per iteration as long as the
         * PCM1808 keeps feeding samples. The `blk->pcm_s24[]` buffer is
         * sized for a full stereo frame, so we fill it in one read.
         */
        size_t bytes_read = 0;
        esp_err_t rerr = i2s_channel_read(s_i2s_rx, blk->pcm_s24,
                                          frame_bytes, &bytes_read,
                                          pdMS_TO_TICKS(50));
        if (rerr != ESP_OK || bytes_read != frame_bytes) {
            ESP_LOGW(TAG, "i2s read: err=%s got=%u exp=%u",
                     esp_err_to_name(rerr),
                     (unsigned)bytes_read, (unsigned)frame_bytes);
            /* Fill the shortfall with zeros so the emitter always sees a
             * full frame; keep cadence intact even on transient RX hiccups. */
            if (bytes_read < frame_bytes) {
                memset((uint8_t *)blk->pcm_s24 + bytes_read, 0,
                       frame_bytes - bytes_read);
            }
        }

        /* PCM1808 delivers 24-bit samples MSB-aligned in a 32-bit slot:
         * raw_i32 = (int32_t)(s24 << 8). LC3's LC3_PCM_FORMAT_S24 expects
         * the 24-bit value sign-extended in the low 24 bits, so we
         * arithmetic-shift right by 8 in place. Single pass, no copy.
         *
         * While we're walking the buffer, track peak |sample| per channel
         * so we can periodically verify the PCM1808 is actually capturing
         * signal vs. just clocking out zeros. If peak stays at 0, it's a
         * hardware/wiring issue (FMT/MD0/MD1 strapping, SCKI wrong pin,
         * no audio input, etc.) rather than an encoder bug. */
        int32_t *p = blk->pcm_s24;
        const int N = SAMPLES_PER_FRAME * CHANNELS;
        static int32_t  peak_l = 0, peak_r = 0;
        static uint32_t diag_frames = 0;
        static uint32_t last_diag_ms = 0;
        for (int i = 0; i < N; i += 2) {
            int32_t l = p[i]     >> 8;
            int32_t r = p[i + 1] >> 8;
            p[i]     = l;
            p[i + 1] = r;
            int32_t al = l < 0 ? -l : l;
            int32_t ar = r < 0 ? -r : r;
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
        }
        diag_frames++;
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_diag_ms >= 2000) {
            /* 24-bit full-scale = 8388607 ≈ 0 dBFS. Convert to dBFS so
             * "silence vs something" is obvious at a glance. */
            ESP_LOGI(TAG, "PCM1808 capture: frames=%lu peak_L=%ld peak_R=%ld (0dBFS=%d)",
                     (unsigned long)diag_frames,
                     (long)peak_l, (long)peak_r, PCM24_MAX);
            peak_l = 0;
            peak_r = 0;
            diag_frames = 0;
            last_diag_ms = now_ms;
        }

        /* Publish PCM block to the emitter — never drop, never block the
         * cadence side. */
        if (xQueueSend(s_pcm_full_q, &blk, portMAX_DELAY) != pdTRUE) {
            /* Defensive: return block to free pool so we don't leak. */
            xQueueSend(s_pcm_free_q, &blk, 0);
        }
    }
}

/* ── LC3 emitter task ─────────────────────────────────────────────────
 *
 * Hot path; pinned to core 1 at high priority. The only work it does
 * inline with the cadence is:
 *   1. xQueueReceive (non-blocking) of a pre-decoded PCM block
 *   2. lc3_encode_stereo_s24() — ~1 ms for both channels
 *   3. ecast_source_publish_audio() — pure queue send, <50 µs
 *   4. UDP publish (non-blocking) when a UDP client is active
 *   5. wait_until_us() — yield + sub-ms busy-wait, no division
 *
 * On underrun (decoder briefly behind), the previous PCM block is
 * re-encoded so cadence is never broken. */
void audio_emit_task(void *arg) {
    (void)arg;

    if (!s_pcm_free_q) {
        s_pcm_free_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
        s_pcm_full_q = xQueueCreate(NUM_PCM_BLOCKS, sizeof(pcm_block_t *));
        /* Decoder seeds the free pool, but if the emitter raced ahead we
         * still want a valid (silent) starting block. */
    }

    /* Local persistent buffer used when the queue underruns. Initialised
     * to silence; subsequent iterations replace it with the most recently
     * received PCM. */
    static pcm_block_t last_pcm;
    memset(&last_pcm, 0, sizeof(last_pcm));

    /* Wait until the decoder has produced at least one block before we
     * start the cadence — avoids sending pure silence at startup. */
    {
        pcm_block_t *first = NULL;
        if (xQueueReceive(s_pcm_full_q, &first, pdMS_TO_TICKS(2000)) == pdTRUE && first) {
            memcpy(&last_pcm, first, sizeof(last_pcm));
            xQueueSend(s_pcm_free_q, &first, 0);
        }
    }

    int64_t deadline_us = esp_timer_get_time() + LC3_FRAME_US;

    while (1) {
        /* Non-blocking pull. On underrun reuse last_pcm — keeps cadence. */
        pcm_block_t *blk = NULL;
        if (xQueueReceive(s_pcm_full_q, &blk, 0) == pdTRUE && blk) {
            memcpy(&last_pcm, blk, sizeof(last_pcm));
            xQueueSend(s_pcm_free_q, &blk, 0);
        } else {
            s_pcm_underruns++;
        }

        /* capture timestamp = scheduled deadline minus one frame period */
        uint32_t capture_us = (uint32_t)(deadline_us - LC3_FRAME_US);

        bool have_udp = false;
        for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
            if (udp_clients[i].in_use) { have_udp = true; break; }
        }

        uint8_t lc3[ECAST_AUDIO_BYTES];
        if (lc3_encode_stereo_s24(last_pcm.pcm_s24, lc3)) {
            ecast_source_publish_audio(lc3, capture_us);

            if (udp_audio_q && have_udp) {
                audio_msg_t m = {0};
                m.h.magic = PROTO_MAGIC;
                m.h.type = MSG_AUDIO;
                m.h.room_code = ROOM_CODE;
                m.seq = seq_num++;
                m.payload_len = LC3_BYTES_PER_CH * CHANNELS;
                m.src_t_us = stream_id;
                m.flags = 0;
                m.capture_us = capture_us;
                memcpy(m.payload, lc3, ECAST_AUDIO_BYTES);
                if (xQueueSend(udp_audio_q, &m, 0) != pdTRUE) {
                    audio_msg_t drop;
                    (void)xQueueReceive(udp_audio_q, &drop, 0);
                    (void)xQueueSend(udp_audio_q, &m, 0);
                }
            }
        }

        /* Advance deadline by exactly one frame period. */
        deadline_us += LC3_FRAME_US;

        /* If we fell badly behind (RTOS hiccup, etc.), resync to avoid a
         * burst. LC3_FRAME_US << 2 == 4 frame periods == 20 ms cap. */
        int64_t now = esp_timer_get_time();
        if (deadline_us < now - (int64_t)(LC3_FRAME_US << 2)) {
            deadline_us = now + LC3_FRAME_US;
        }
        wait_until_us(deadline_us);
    }
}

/* The legacy audio_send_task (unicast fanout) is fully replaced by
 * ecast_source's internal RTN scheduler. Stub exits immediately so any
 * task-create reference still links.
 *
 * Preserved below is the original code (now unreachable) for reference
 * in case we need to diff against the old unicast path during debugging. */
void audio_send_task(void *arg) {
    (void)arg;
    vTaskDelete(NULL);
    return;

    /* ── UNREACHABLE LEGACY BODY ──────────────────────────────────── */
    audio_msg_t m;
    while (1) {
        if (xQueueReceive(audio_q, &m, portMAX_DELAY) != pdTRUE) continue;

        /* Drain any older packets in the queue, keep only the newest to bound
         * real-time lag. */
        uint32_t dropped_local = 0;
        audio_msg_t newer;
        while (xQueueReceive(audio_q, &newer, 0) == pdTRUE) {
            m = newer;
            dropped_local++;
        }
        stat_src_drop_espnow_q += dropped_local;

        int send_len = (int)(offsetof(audio_msg_t, payload) + m.payload_len);

        /* Per-sink ESP-NOW UNICAST fanout.
         *
         * Advantages vs broadcast:
         *  - 802.11 MAC-level ACK + automatic retries (4-7 retries) per sink
         *    give inherent reliability without app-layer duplication.
         *  - Rate negotiation and power-level adapt per peer.
         *  - No on-air overhead for sinks that aren't connected.
         *
         * Trade-offs (mitigated elsewhere):
         *  - Inter-sink sync: serialised sends + variable retry latency add
         *    up to ~a few ms of skew. The sink's playout scheduler
         *    (TARGET_LAG_US = 45 ms) absorbs this and re-aligns all sinks
         *    to the source's capture_us wall clock, so sync is preserved
         *    as long as each sink receives the packet before its due time.
         *  - Scaling: airtime grows linearly with sink count. At 6 Mbps
         *    + 150 B packet ≈ 200 µs per send; 10 sinks = 2 ms per frame;
         *    well inside the 5 ms budget.
         *
         * If no sinks are joined we still take the token and fall through
         * (no actual send) — keeps the queue draining cleanly. */
        int sink_count = 0;
        for (int i = 0; i < MAX_SINKS; i++) {
            if (!sinks[i].in_use) continue;
            sink_count++;

            if (xSemaphoreTake(tx_tokens, pdMS_TO_TICKS(AUDIO_TX_TOKEN_WAIT_MS)) != pdTRUE) {
                espnow_token_drop++;
                break;  /* TX pipeline stalled; skip remaining sinks this frame */
            }

            esp_err_t err = esp_now_send(sinks[i].mac, (const uint8_t *)&m, send_len);
            if (err != ESP_OK) {
                /* send_cb won't fire on immediate-fail — release token ourselves. */
                xSemaphoreGive(tx_tokens);
            }
        }
        (void)sink_count;  /* reserved for future telemetry */
    }
}

void udp_audio_send_task(void *arg) {
    (void)arg;

    audio_msg_t m;
    uint32_t pace_count = 0;
    while (1) {
        if (xQueueReceive(udp_audio_q, &m, portMAX_DELAY) != pdTRUE) continue;

        bool esp_active = count_active_sinks() > 0;
        if (esp_active && radio_congested) {
            UBaseType_t aq = audio_q ? uxQueueMessagesWaiting(audio_q) : 0;
            uint32_t pace_div = (aq > 0) ? UDP_PACE_DIV_STRONG : UDP_PACE_DIV_NORMAL;
            pace_count++;
            if ((pace_count % pace_div) != 0U) {
                stat_udp_paced_drop++;
                continue;
            }
        }

        uint32_t dropped_local = 0;
        audio_msg_t newer;
        while (xQueueReceive(udp_audio_q, &newer, 0) == pdTRUE) {
            m = newer;
            dropped_local++;
        }
        stat_src_drop_udp_q += dropped_local;

        int send_len = (int)(offsetof(audio_msg_t, payload) + m.payload_len);

        if (udp_sock >= 0) {
            uint32_t udp_now = (uint32_t)esp_timer_get_time();
            for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
                if (!udp_clients[i].in_use) continue;
                if ((udp_now - udp_clients[i].last_seen_us) > UDP_CLIENT_TIMEOUT_US) {
                    udp_clients[i].in_use = false;
                    continue;
                }
                sendto(udp_sock, &m, send_len, MSG_DONTWAIT,
                       (struct sockaddr *)&udp_clients[i].addr,
                       sizeof(udp_clients[i].addr));
            }
        }
    }
}

void source_stats_task(void *arg) {
    (void)arg;
    extern volatile uint32_t ecast_tx_frames;
    extern volatile uint32_t ecast_tx_ok;
    extern volatile uint32_t ecast_tx_fail;
    extern volatile uint32_t ecast_tx_dropped;
    static uint32_t prev_ecast_frames = 0;
    static uint32_t prev_underruns = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* ECast emit-rate visibility. Two-second window, expected = 400
         * unique frames ((1 / 5 ms) * 2 s = 400). Anything <390 means
         * the cadence is slipping and the audio will drift. */
        uint32_t ef_now = ecast_tx_frames;
        uint32_t ef_delta = ef_now - prev_ecast_frames;
        prev_ecast_frames = ef_now;
        uint32_t un_now = s_pcm_underruns;
        uint32_t un_delta = un_now - prev_underruns;
        prev_underruns = un_now;
        ESP_LOGI(TAG, "ECast emit: frames/2s=%lu (target=400) underruns=%lu tx_ok=%lu tx_fail=%lu tx_drop=%lu",
                 (unsigned long)ef_delta,
                 (unsigned long)un_delta,
                 (unsigned long)ecast_tx_ok,
                 (unsigned long)ecast_tx_fail,
                 (unsigned long)ecast_tx_dropped);

        EventBits_t bits = wifi_ev ? xEventGroupGetBits(wifi_ev) : 0;
        bool sta_up = (bits & WIFI_CONNECTED_BIT) != 0;
        if (sta_up && !napt_enabled) {
            ensure_napt_enabled();
        }

        int sinks_n = count_active_sinks();
        int udp_n = count_active_udp_clients();
        UBaseType_t qfill = audio_q ? uxQueueMessagesWaiting(audio_q) : 0;
        UBaseType_t uqfill = udp_audio_q ? uxQueueMessagesWaiting(udp_audio_q) : 0;
        uint32_t drop_es = stat_src_drop_espnow_q;
        uint32_t drop_ud = stat_src_drop_udp_q;
        uint32_t drop_paced = stat_udp_paced_drop;
        uint32_t sta_disc = sta_disconnect_count;
        uint32_t sta_rec = sta_reconnect_count;
        uint32_t sta_gap = sta_last_recover_ms;

        static uint32_t prev_ok = 0, prev_fail = 0, prev_tok = 0;
        uint32_t ok_now = espnow_send_ok;
        uint32_t fail_now = espnow_send_fail;
        uint32_t tok_now = espnow_token_drop;
        uint32_t d_ok = ok_now - prev_ok;
        uint32_t d_fail = fail_now - prev_fail;
        uint32_t d_tok = tok_now - prev_tok;
        prev_ok = ok_now;
        prev_fail = fail_now;
        prev_tok = tok_now;

        uint32_t attempts = d_ok + d_fail;
        uint32_t fail_pct = (attempts > 0) ? (d_fail * 100U / attempts) : 0;
        radio_congested = (d_tok > 0) || (attempts >= 40 && fail_pct >= 4);
        /* PHY rate is locked at compile-time (ESPNOW_PHY_RATE).
         * Adaptive stepping removed — rate bouncing between 12M/24M
         * caused burst packet losses that triggered PLC spirals on sinks. */

        ESP_LOGI(TAG, "alive ch=%u seq=%u sinks=%d udp=%d q=%u uq=%u dq_es=%lu dq_udp=%lu dq_up=%lu cong=%u sta=%u napt=%u disc=%lu rec=%lu gap=%lums rate=%s fail=%u%% tok=%lu",
                 (unsigned)wifi_channel,
                 (unsigned)seq_num,
                 sinks_n,
                 udp_n,
                 (unsigned)qfill,
                 (unsigned)uqfill,
                 (unsigned long)drop_es,
                 (unsigned long)drop_ud,
                 (unsigned long)drop_paced,
                 radio_congested ? 1U : 0U,
                 sta_up ? 1U : 0U,
                 napt_enabled ? 1U : 0U,
                 (unsigned long)sta_disc,
                 (unsigned long)sta_rec,
                 (unsigned long)sta_gap,
                 espnow_rate_name(espnow_rate_current),
                 (unsigned)fail_pct,
                 (unsigned long)d_tok);
    }
}
