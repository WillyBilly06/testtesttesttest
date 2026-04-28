/*
 * ESP-NOW Audio Sink - P4 Host Side (Rewritten)
 *
 * Communicates with C6 coprocessor via ESP-Hosted custom data channel (SDIO).
 * C6 forwards raw LC3 packets. P4 decodes LC3 -> PCM and plays via I2S codec.
 *
 * Features:
 * - LC3 decoding (liblc3) on P4
 * - Packet Loss Concealment (PLC) for missing frames
 * - Sequence tracking with gap detection
 * - Pre-buffering before playback start
 */

#include <string.h>
#include <stddef.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_hosted.h"
#include "bsp_board_extra.h"
#include "lc3.h"
#include "espnow_protocol.h"
#include "espnow_sink.h"
#include "ecast_protocol.h"
#include "ecast_crypto.h"

static const char *TAG = "espnow_sink";

/* ── LC3 constants ─────────────────────────────────────────── */
#define LC3_FRAME_US       ESPNOW_LC3_FRAME_US         /* 7500 */
#define SAMPLE_RATE_HZ     ESPNOW_SAMPLE_RATE_HZ       /* 48000 */
#define CHANNELS           ESPNOW_CHANNELS              /* 2 */
#define LC3_BYTES_PER_CH   ESPNOW_LC3_BYTES_PER_CH      /* 72 */
#define SAMPLES_PER_FRAME  ESPNOW_SAMPLES_PER_FRAME     /* 360 */

#define PLC_MAX_GAP_FILL       1   /* max PLC frames inserted for lost_before gap (1 frame = 7.5ms repair, inaudible) */
#define PLC_MAX_TIMEOUT       80   /* max consecutive timeout-PLC frames (~600ms) */
#define SILENCE_MAX         2000   /* silence frames before full reset (~15s)      */
/* Latency budget (target: end-to-end 40-50 ms with RTN=2 redundancy):
 *   Source capture+encode+queue+TX  ~  8 ms
 *   RTN=2 second-copy window        ~  5 ms
 *   Over-air at 24 Mbps             ~  1-3 ms
 *   C6 RX callback + SDIO forward   ~  5-8 ms
 *   P4 TARGET_LAG playout wait      ~ 25 ms (absorbs jitter + retry bursts)
 *   I2S DMA output                  ~  5 ms
 *   ─────────────────────────────────────────
 *   Total                           ~ 45 ms ✓
 *
 * PREBUFFER_FRAMES is the startup-only prebuffer. Steady-state latency is
 * governed by TARGET_LAG_US (the playout scheduler below). Keep prebuffer
 * small so resync after a drop doesn't add headstart latency. */
#define PREBUFFER_FRAMES       3   /* ~22 ms startup prebuffer */
#define DRIFT_QUEUE_LOW        1   /* only pad if queue about to empty */
#define DRIFT_QUEUE_HIGH       8   /* trim only if queue >8 (~60 ms) — ~40 ms of burst headroom */
#define RX_TIMEOUT_MS         20   /* > 7.5 ms frame + jitter margin */

/* Playout scheduler: all sinks using the same TARGET_LAG_US play the same
 * sample at the same wall-clock time (modulo RF jitter), giving inter-receiver
 * sync within ±5 ms. 25 ms target = ~8 ms typical RF+SDIO path + ~17 ms
 * jitter/burst-loss headroom. Combined with ~5 ms I2S DMA, end-to-end
 * latency lands at ~40-45 ms. Raise to 35 ms if RF environment is noisy. */
#define TARGET_LAG_US      25000
#define PHASE_EARLY_MIN_US   200   /* ignore <200 µs early (spin-wait overhead) */
#define PHASE_EARLY_MAX_US 10000   /* cap single delay to 10 ms to preserve WDT feeding */

/* ── Internal state ──────────────────────────────────────── */
static bool                     s_initialized   = false;
static espnow_state_t           s_state         = ESPNOW_STATE_NOT_INIT;
static bool                     s_c6_wifi_ok    = false;
static bool                     s_c6_espnow_ok  = false;

static espnow_room_info_t       s_rooms[ESPNOW_SINK_MAX_ROOMS];
static int                      s_room_count    = 0;
static SemaphoreHandle_t        s_rooms_mutex   = NULL;

static int                      s_selected_room = -1;
static bool                     s_force_leave_before_scan = false;

/* Statistics (updated from C6 EVT_STATS) */
static volatile uint32_t        s_packets_rx    = 0;
static volatile uint32_t        s_packets_lost  = 0;
static char                     s_c6_fw_version[32] = "unknown";
static bool                     s_c6_fw_valid = false;

/* Callbacks */
static espnow_sink_callbacks_t  s_cbs           = {0};

/* Audio playback task */
static TaskHandle_t             s_playback_task = NULL;
static QueueHandle_t            s_audio_queue   = NULL;
static bool                     s_codec_ready   = false;

/* LC3 decoder state */
static lc3_decoder_mem_48k_t    s_lc3_dec_mem[CHANNELS];
static lc3_decoder_t            s_lc3_dec[CHANNELS];

/* SDIO health */
static volatile uint32_t        s_last_evt_ms   = 0;
#define SDIO_ALIVE_TIMEOUT_MS   5000

/* ── Auto-scan / auto-connect state machine ─────────────────
 *
 * When enabled, a dedicated task owns the complete connect lifecycle:
 *   IDLE/NOT_INIT  → start scan
 *   SCAN_DONE 0    → rescan after a short pause
 *   SCAN_DONE >0   → pick highest-RSSI room and JOIN
 *   CONNECTED      → idle watcher, react to disconnect/timeout events
 *   TIMEOUT/DROP   → try to rejoin the same room up to N times × 500 ms,
 *                    then fall back to scanning a fresh set of rooms
 *
 * The task signals itself via `s_autoscan_signal`; event callbacks
 * (scan_done, error, state_change) just give the signal to wake it.
 * The UI never sends CMD_INIT / CMD_DEINIT / CMD_SCAN directly — it only
 * flips the autoscan flag on/off. This eliminates the scan-deinit-rescan
 * thrashing that prevented the P4 from ever completing a 600 ms scan. */
#define AUTOSCAN_RESCAN_DELAY_MS     1500   /* idle after empty scan before retry */
#define AUTOSCAN_RECONNECT_DELAY_MS   500   /* between drop-retry attempts */
#define AUTOSCAN_MAX_RECONNECT         10   /* drop → retry this many times, then give up */
#define AUTOSCAN_JOIN_TIMEOUT_MS     2000   /* wait up to this for JOIN to complete */
#define AUTOSCAN_SCAN_TIMEOUT_MS     1500   /* SCAN_DONE event deadline */

static volatile bool            s_autoscan_enabled     = false;
static TaskHandle_t             s_autoscan_task        = NULL;
static SemaphoreHandle_t        s_autoscan_signal      = NULL;
static volatile bool            s_autoscan_scan_done   = false;
static volatile bool            s_autoscan_audio_drop  = false;
static uint8_t                  s_autoscan_target_mac[6] = {0};
static uint32_t                 s_autoscan_target_code  = 0;

/* Forward decl so event callbacks (defined earlier in the file) can signal
 * the autoscan task without reordering the whole file. */
static void autoscan_wake(void);

/* ── LC3 decoder helpers ─────────────────────────────────── */

static void lc3_init_decoder(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        s_lc3_dec[ch] = lc3_setup_decoder(LC3_FRAME_US, SAMPLE_RATE_HZ, 0,
                                           &s_lc3_dec_mem[ch]);
        if (!s_lc3_dec[ch]) {
            ESP_LOGE(TAG, "lc3_setup_decoder failed ch=%d", ch);
            abort();
        }
    }
    ESP_LOGI(TAG, "LC3 decoder ready (%dus, %dHz, %dB/ch)",
             LC3_FRAME_US, SAMPLE_RATE_HZ, LC3_BYTES_PER_CH);
}

static bool lc3_decode_stereo(const uint8_t *payload, int32_t *pcm_s24_interleaved)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        const uint8_t *in = payload + ch * LC3_BYTES_PER_CH;
        int rc = lc3_decode(s_lc3_dec[ch], in, LC3_BYTES_PER_CH,
                            LC3_PCM_FORMAT_S24, pcm_s24_interleaved + ch, CHANNELS);
        if (rc < 0) return false;
    }
    return true;
}

static bool lc3_plc_stereo(int32_t *pcm_s24_interleaved)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        int rc = lc3_decode(s_lc3_dec[ch], NULL, 0,
                            LC3_PCM_FORMAT_S24, pcm_s24_interleaved + ch, CHANNELS);
        if (rc < 0) return false;
    }
    return true;
}

/* Convert LC3's 24-bit PCM (low 24 bits of int32) to 32-bit MSB-aligned slot
 * the codec expects when configured as 32-bit. Saturate to 24-bit range first. */
static inline int32_t sat24(int32_t v)
{
    if (v >  8388607)  return  8388607;
    if (v < -8388608)  return -8388608;
    return v;
}

static void pcm24_to_i2s32(const int32_t *pcm_s24, int32_t *i2s_out)
{
    const int N = SAMPLES_PER_FRAME * CHANNELS;
    for (int i = 0; i < N; i++) {
        i2s_out[i] = sat24(pcm_s24[i]) << 8;
    }
}

/* ── Codec setup ─────────────────────────────────────────── */

static void ensure_codec_ready(void)
{
    if (!s_codec_ready) {
        /* 32-bit slot carrying 24-bit MSB-aligned audio. Matches standalone sink. */
        bsp_extra_codec_set_fs(SAMPLE_RATE_HZ, 32, I2S_SLOT_MODE_STEREO);
        s_codec_ready = true;
        ESP_LOGI(TAG, "Codec configured for %d Hz 24-bit-in-32-slot stereo", SAMPLE_RATE_HZ);
    }
}

/* ── Playback task ───────────────────────────────────────── */

static void playback_task_fn(void *arg)
{
    (void)arg;
    espnow_evt_audio_raw_t pkt;

    /* Decode buffers (24-bit audio in low 24 bits of int32) + MSB-aligned I2S out */
    static int32_t pcm_s24[SAMPLES_PER_FRAME * CHANNELS];
    static int32_t i2s_out[SAMPLES_PER_FRAME * CHANNELS];

    bool started = false;
    uint16_t expected_seq = 0;
    bool expected_valid = false;
    uint32_t consecutive_plc = 0;
    uint32_t stat_decoded = 0;
    uint32_t stat_plc = 0;
    uint32_t stat_drift_trim = 0;
    uint32_t stat_drift_hold = 0;
    uint32_t log_time = 0;

    /* Playout scheduler: phase-align playback to (capture_us + offset + TARGET_LAG).
     * All sinks using the same TARGET_LAG_US converge to identical wall-clock
     * playout times — inter-receiver sync within ±5 ms. */
    int32_t  clock_offset_us = 0;
    uint32_t clock_sync_count = 0;
    int32_t  playout_err_smooth_us = 0;

    lc3_init_decoder();

    ESP_LOGI(TAG, "Playback task started (LC3 decode on P4)");

    while (1) {
        if (s_state != ESPNOW_STATE_CONNECTED) {
            /* Not connected: reset state */
            if (started) {
                ESP_LOGI(TAG, "Disconnected, resetting playback state");
            }
            started = false;
            expected_valid = false;
            consecutive_plc = 0;
            if (s_audio_queue) {
                xQueueReset(s_audio_queue);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Pre-buffer before starting playback */
        if (!started) {
            if (s_audio_queue && uxQueueMessagesWaiting(s_audio_queue) < PREBUFFER_FRAMES) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            ensure_codec_ready();
            started = true;
            consecutive_plc = 0;
            stat_decoded = 0;
            stat_plc = 0;
            stat_drift_trim = 0;
            stat_drift_hold = 0;
            ESP_LOGI(TAG, "Playback start (prebuffer=%d)", PREBUFFER_FRAMES);
        }

        bool have_pkt = false;
        if (xQueueReceive(s_audio_queue, &pkt, pdMS_TO_TICKS(RX_TIMEOUT_MS)) == pdTRUE) {
            have_pkt = true;
        }

        bool ok = false;

        if (have_pkt) {
            uint16_t seq = pkt.seq;
            uint16_t lost = pkt.lost_before;

            UBaseType_t qfill = s_audio_queue ? uxQueueMessagesWaiting(s_audio_queue) : 0;

            /* If source runs slightly faster, trim one queued frame to keep latency bounded. */
            if (qfill > DRIFT_QUEUE_HIGH) {
                espnow_evt_audio_raw_t newer_pkt;
                if (xQueueReceive(s_audio_queue, &newer_pkt, 0) == pdTRUE) {
                    pkt = newer_pkt;
                    seq = pkt.seq;
                    lost = pkt.lost_before;
                    stat_drift_trim++;
                    qfill--;
                }
            }

            if (!expected_valid) {
                expected_seq = seq;
                expected_valid = true;
            }

            /* Generate PLC frames for gaps (capped to keep latency low) */
            if (lost > 0 && lost < PLC_MAX_GAP_FILL + 1) {
                for (uint16_t i = 0; i < lost && i < PLC_MAX_GAP_FILL; i++) {
                    lc3_plc_stereo(pcm_s24);
                    pcm24_to_i2s32(pcm_s24, i2s_out);
                    size_t written = 0;
                    bsp_extra_i2s_write(i2s_out,
                                        SAMPLES_PER_FRAME * CHANNELS * sizeof(int32_t),
                                        &written, portMAX_DELAY);
                    stat_plc++;
                }
            }

            /* If source runs slightly slower, pad with one PLC frame WITHOUT consuming this
             * packet, so the producer has time to refill the queue. Re-inject pkt at head.
             * Previous logic consumed + padded which drained the queue 2:1 and caused
             * continuous robotic PLC output. */
            if (qfill < DRIFT_QUEUE_LOW) {
                if (lc3_plc_stereo(pcm_s24)) {
                    pcm24_to_i2s32(pcm_s24, i2s_out);
                    size_t written = 0;
                    bsp_extra_i2s_write(i2s_out,
                                        SAMPLES_PER_FRAME * CHANNELS * sizeof(int32_t),
                                        &written, portMAX_DELAY);
                    stat_plc++;
                    stat_drift_hold++;
                    consecutive_plc++;
                }
                /* Put packet back at the head; process it next iteration when queue has filled */
                if (s_audio_queue && xQueueSendToFront(s_audio_queue, &pkt, 0) != pdTRUE) {
                    /* Queue somehow full already: decode now rather than dropping */
                    ok = lc3_decode_stereo(pkt.payload, pcm_s24);
                    expected_seq = seq + 1;
                }
                continue;
            }

            /* Decode the actual packet */
            ok = lc3_decode_stereo(pkt.payload, pcm_s24);
            expected_seq = seq + 1;
        } else {
            /* No packet arrived in time */
            if (consecutive_plc < PLC_MAX_TIMEOUT) {
                /* LC3 PLC: gradually fades to silence naturally */
                ok = lc3_plc_stereo(pcm_s24);
            } else if (consecutive_plc < SILENCE_MAX) {
                /* PLC exhausted: feed explicit silence to keep I2S flowing */
                memset(pcm_s24, 0, SAMPLES_PER_FRAME * CHANNELS * sizeof(int32_t));
                ok = true;
            }
        }

        if (ok) {
            if (have_pkt) {
                stat_decoded++;
                consecutive_plc = 0;
            } else {
                stat_plc++;
                consecutive_plc++;
            }

            /* Pacing model: the I2S DMA clock is authoritative.
             *
             * i2s_channel_write() blocks until DMA has room for the next
             * SAMPLES_PER_FRAME samples, which it consumes at the hardware
             * sample rate. This naturally paces the consumer at 7.5 ms/frame
             * to match the source's capture rate. The PREBUFFER_FRAMES deep
             * queue absorbs RF/SDIO jitter; DRIFT_QUEUE_HIGH/LOW handle
             * slow clock drift between source and sink crystals.
             *
             * We previously ran an additional playout scheduler that delayed
             * each write to a wall-clock `due_time = capture + offset + lag`.
             * That fought the I2S clock: per-frame sleep + I2S blocking write
             * pushed consumer period to ~17 ms while producer stayed at 7.5 ms,
             * and the EMA absorbed the drift back into `clock_offset`,
             * creating a self-reinforcing 45 ms lag and DMA underruns that
             * manifested as robotic, pitched, noisy output.
             *
             * Clock offset is still tracked (diagnostic only) to observe
             * source↔sink clock drift over time without affecting pacing. */
            if (have_pkt) {
                uint32_t now_us = (uint32_t)esp_timer_get_time();
                int32_t  raw_offset = (int32_t)(now_us - pkt.capture_us);

                if (clock_sync_count == 0) {
                    clock_offset_us = raw_offset;
                } else {
                    /* EMA alpha = 1/128 via arithmetic right shift — no division. */
                    int32_t err = raw_offset - clock_offset_us;
                    int32_t corr = err >> 7;
                    if (corr >  300) corr =  300;
                    if (corr < -300) corr = -300;
                    clock_offset_us += corr;
                }
                clock_sync_count++;

                /* Diagnostic: current instantaneous phase error vs the
                 * tracked offset. No action taken — pacing is I2S-driven. */
                int32_t inst_err = raw_offset - clock_offset_us;
                playout_err_smooth_us += (inst_err - playout_err_smooth_us) >> 3;
            }

            pcm24_to_i2s32(pcm_s24, i2s_out);

            size_t written = 0;
            bsp_extra_i2s_write(i2s_out,
                                SAMPLES_PER_FRAME * CHANNELS * sizeof(int32_t),
                                &written, portMAX_DELAY);

            if (s_cbs.on_audio && have_pkt) {
                /* Callback still delivers 24-bit-in-int32 samples. Clients should
                 * be aware this changed from int16_t — update callback signature if used. */
                s_cbs.on_audio((const int16_t *)pcm_s24, SAMPLES_PER_FRAME, pkt.seq);
            }
        } else {
            consecutive_plc++;

            /* Only reset after very long outage (>1.5s of no packets) */
            if (consecutive_plc >= SILENCE_MAX && started) {
                ESP_LOGW(TAG, "Audio outage (%lu frames), resetting decoder",
                         (unsigned long)consecutive_plc);
                lc3_init_decoder();
                started = false;
                expected_valid = false;
                consecutive_plc = 0;
                if (s_audio_queue) {
                    xQueueReset(s_audio_queue);
                }
                continue;
            }
        }

        /* Periodic stats log */
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - log_time >= 10000) {
            log_time = now;
            ESP_LOGI(TAG, "Playback: decoded=%lu plc=%lu dr_trim=%lu dr_hold=%lu cplc=%lu q=%u clk_off=%ldus ph_err=%ldus",
                     (unsigned long)stat_decoded, (unsigned long)stat_plc,
                     (unsigned long)stat_drift_trim, (unsigned long)stat_drift_hold,
                     (unsigned long)consecutive_plc,
                     (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                     (long)clock_offset_us, (long)playout_err_smooth_us);
        }
    }
}

/* ── SDIO event handlers ─────────────────────────────────── */

static inline void update_evt_time(void) {
    s_last_evt_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void on_evt_status(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_status_t)) return;
    const espnow_evt_status_t *evt = (const espnow_evt_status_t *)data;

    s_c6_wifi_ok   = evt->wifi_init;
    s_c6_espnow_ok = evt->espnow_init;
    espnow_state_t new_state = (espnow_state_t)evt->state;

    ESP_LOGI(TAG, "C6 status: state=%d wifi=%d espnow=%d",
             evt->state, evt->wifi_init, evt->espnow_init);

    if (new_state != s_state) {
        s_state = new_state;
        if (s_cbs.on_state_change) {
            s_cbs.on_state_change(new_state);
        }
    }
}

static void on_evt_room_found(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_room_t)) return;
    const espnow_evt_room_t *r = (const espnow_evt_room_t *)data;

    if (!s_rooms_mutex) return;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);

    int idx = -1;
    for (int i = 0; i < s_room_count; i++) {
        if (s_rooms[i].stream_id == r->stream_id) {
            idx = i;
            break;
        }
    }

    if (idx < 0 && s_room_count < ESPNOW_SINK_MAX_ROOMS) {
        idx = s_room_count++;
    }

    if (idx >= 0) {
        espnow_room_info_t *room = &s_rooms[idx];
        memcpy(room->mac, r->mac, 6);
        room->wifi_channel = r->wifi_channel;
        room->room_code    = r->room_code;
        room->stream_id    = r->stream_id;
        room->rssi         = r->rssi;
        room->valid        = true;

        ESP_LOGI(TAG, "Room found: code=0x%08" PRIX32 " ch=%d stream=%lu rssi=%d",
                 r->room_code, r->wifi_channel,
                 (unsigned long)r->stream_id, r->rssi);
    }

    xSemaphoreGive(s_rooms_mutex);

    if (idx >= 0 && s_cbs.on_room_found) {
        s_cbs.on_room_found(&s_rooms[idx]);
    }
}

static void on_evt_scan_done(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_scan_done_t)) return;
    const espnow_evt_scan_done_t *sd = (const espnow_evt_scan_done_t *)data;

    s_state = ESPNOW_STATE_IDLE;
    s_autoscan_scan_done = true;
    ESP_LOGI(TAG, "Scan done, %d rooms found", sd->room_count);

    if (s_cbs.on_scan_done) {
        s_cbs.on_scan_done(sd->room_count);
    }
    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_IDLE);
    }
    autoscan_wake();
}

static void on_evt_joined(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_joined_t)) return;
    const espnow_evt_joined_t *j = (const espnow_evt_joined_t *)data;

    s_state = ESPNOW_STATE_CONNECTED;
    s_force_leave_before_scan = false;
    s_packets_rx   = 0;
    s_packets_lost = 0;

    ESP_LOGI(TAG, "Joined room code=0x%08" PRIX32 " ch=%d stream=%lu",
             j->room_code, j->wifi_channel, (unsigned long)j->stream_id);

    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_CONNECTED);
    }
    autoscan_wake();
}

static void on_evt_left(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    (void)data; (void)len;

    s_state         = ESPNOW_STATE_IDLE;
    s_selected_room = -1;
    s_force_leave_before_scan = false;

    ESP_LOGI(TAG, "Left room");

    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_IDLE);
    }
    autoscan_wake();
}

static void on_evt_audio(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_audio_raw_t)) return;

    s_packets_rx++;

    /* Queue raw LC3 packet for decode in playback task.
     * On overflow: drop oldest (stale) and keep newest to bound real-time latency,
     * instead of silently losing the fresh packet. */
    if (s_audio_queue) {
        if (xQueueSend(s_audio_queue, data, 0) != pdTRUE) {
            espnow_evt_audio_raw_t stale;
            if (xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) {
                s_packets_lost++;
            }
            if (xQueueSend(s_audio_queue, data, 0) != pdTRUE) {
                s_packets_lost++;
            }
        }
    }
}

static void on_evt_stats(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_stats_t)) return;
    const espnow_evt_stats_t *st = (const espnow_evt_stats_t *)data;

    /* C6 now packs TX counters into plc_frames: low 16 bits = tx_ok,
     * high 16 bits = tx_fail. This gives visibility into ESP-NOW silent
     * TX failures (e.g. MAC-level ACK missing). */
    uint16_t tx_ok   = (uint16_t)(st->plc_frames & 0xFFFF);
    uint16_t tx_fail = (uint16_t)((st->plc_frames >> 16) & 0xFFFF);
    ESP_LOGI(TAG, "C6 stats: rx=%lu lost=%lu rssi=%d ch=%d sdio_err=%d "
             "tx_ok=%u tx_fail=%u",
             (unsigned long)st->packets_rx, (unsigned long)st->packets_lost,
             st->rssi_last, st->wifi_channel, st->sdio_send_errors,
             (unsigned)tx_ok, (unsigned)tx_fail);
}

static void on_evt_error(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_error_t)) return;
    const espnow_evt_error_t *e = (const espnow_evt_error_t *)data;

    ESP_LOGE(TAG, "C6 error %ld: %.64s", (long)e->error_code, e->message);

    if (e->error_code == ESP_ERR_TIMEOUT) {
        /* Audio timeout means room stream dropped; recover to IDLE so scan can restart cleanly. */
        s_state = ESPNOW_STATE_IDLE;
        s_autoscan_audio_drop = true;
        /* Keep selected room marker so start_scan can force a C6 LEAVE before SCAN. */
        s_force_leave_before_scan = true;
        if (s_audio_queue) {
            xQueueReset(s_audio_queue);
        }
        if (s_cbs.on_state_change) {
            s_cbs.on_state_change(ESPNOW_STATE_IDLE);
        }
        autoscan_wake();
        return;
    }

    s_state = ESPNOW_STATE_ERROR;
    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_ERROR);
    }
    autoscan_wake();
}

static void on_evt_fw_ver(uint32_t msg_id, const uint8_t *data, size_t len)
{
    update_evt_time();
    if (len < sizeof(espnow_evt_fw_ver_t)) return;
    const espnow_evt_fw_ver_t *ver = (const espnow_evt_fw_ver_t *)data;

    memset(s_c6_fw_version, 0, sizeof(s_c6_fw_version));
    memcpy(s_c6_fw_version, ver->version, sizeof(ver->version));
    s_c6_fw_version[sizeof(s_c6_fw_version) - 1] = '\0';
    s_c6_fw_valid = (s_c6_fw_version[0] != '\0');

    ESP_LOGI(TAG, "C6 firmware: version='%s' project='%.32s'",
             s_c6_fw_valid ? s_c6_fw_version : "unknown", ver->project);
}

/* ── EspCastBR handlers ──────────────────────────────────── */
/* The C6 forwards two things now:
 *   - ECAST_BEACON: already authenticated (CMAC-verified on C6). We parse,
 *     derive the AES-CCM session key, and populate the room list.
 *   - ECAST_AUDIO : already deduplicated (C6 drops RTN retries). We
 *     decrypt with the active room's session key, then push the plaintext
 *     LC3 into the existing audio queue so the unchanged playback task
 *     decodes + outputs it.
 *
 * Because RTN=3 copies of every frame are scheduled on the source and the
 * C6 forwards whichever copy arrives first, any single-copy RF loss is
 * transparent to the P4 — we never see gaps we would have PLC'd for.
 */

static uint8_t  s_broadcast_code[16] = ECAST_BROADCAST_CODE_BYTES;
static volatile uint16_t s_active_stream_id16 = 0;
static volatile bool     s_active_stream_valid = false;

static int find_or_create_room_by_stream(uint32_t stream_id, const uint8_t *mac_or_null)
{
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_room_count; i++) {
        if (s_rooms[i].stream_id == stream_id) { idx = i; break; }
    }
    if (idx < 0 && s_room_count < ESPNOW_SINK_MAX_ROOMS) {
        idx = s_room_count++;
        memset(&s_rooms[idx], 0, sizeof(s_rooms[idx]));
        if (mac_or_null) memcpy(s_rooms[idx].mac, mac_or_null, 6);
    }
    xSemaphoreGive(s_rooms_mutex);
    return idx;
}

static void on_evt_ecast_beacon(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id;
    update_evt_time();
    if (len < ECAST_BEACON_FRAME_SIZE) return;

    const ecast_hdr_t *h = (const ecast_hdr_t *)data;
    if (h->magic != ECAST_MAGIC) return;
    if (ECAST_TYPE_OF(h->ver_type) != ECAST_TYPE_BEACON) return;

    const ecast_beacon_payload_t *b =
        (const ecast_beacon_payload_t *)(data + sizeof(ecast_hdr_t));

    int idx = find_or_create_room_by_stream(b->stream_id_full, NULL);
    if (idx < 0) return;

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    espnow_room_info_t *r = &s_rooms[idx];
    r->wifi_channel  = b->wifi_channel;
    r->room_code     = (uint32_t)(b->broadcast_id & 0xFFFFFFFFU);
    r->stream_id     = b->stream_id_full;
    r->rssi          = 0;   /* no per-frame RSSI in envelope yet; update via EVT_ECAST_RSSI */
    r->valid         = true;
    r->broadcast_id  = b->broadcast_id;
    r->pres_delay_us = b->pres_delay_us;
    r->is_encrypted  = (b->is_encrypted != 0);
    memcpy(r->enc_iv,          b->enc_iv,          8);
    memcpy(r->key_diversifier, b->key_diversifier, 8);
    memset(r->name, 0, sizeof(r->name));
    memcpy(r->name, b->name, sizeof(b->name));
    r->name[sizeof(r->name) - 1] = '\0';

    /* Derive session key once per distinct (stream_id, key_diversifier). */
    if (r->is_encrypted && !r->session_key_valid) {
        if (ecast_derive_session_key(s_broadcast_code, r->key_diversifier, r->session_key)) {
            r->session_key_valid = true;
            ESP_LOGI(TAG, "ECast room '%s' stream=%08lX: session key derived",
                     r->name, (unsigned long)r->stream_id);
        } else {
            ESP_LOGW(TAG, "ECast room %08lX: session key derivation failed",
                     (unsigned long)r->stream_id);
        }
    }
    xSemaphoreGive(s_rooms_mutex);

    /* First authenticated beacon auto-selects the stream (MVP behaviour:
     * one source in range → just play it). For multi-source UI selection
     * the UI calls espnow_sink_join_room() which calls espnow_sink_select_ecast_stream(). */
    if (!s_active_stream_valid) {
        s_active_stream_id16 = (uint16_t)(b->stream_id_full & 0xFFFFU);
        s_active_stream_valid = true;
        s_state = ESPNOW_STATE_CONNECTED;
        if (s_cbs.on_state_change) s_cbs.on_state_change(ESPNOW_STATE_CONNECTED);
        ESP_LOGI(TAG, "ECast: auto-selected stream=%08lX ('%s')",
                 (unsigned long)b->stream_id_full, s_rooms[idx].name);
    }

    if (s_cbs.on_room_found) s_cbs.on_room_found(&s_rooms[idx]);
}

static void on_evt_ecast_audio(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id;
    update_evt_time();
    if (len < ECAST_AUDIO_FRAME_SIZE) return;

    const ecast_hdr_t *h = (const ecast_hdr_t *)data;
    if (h->magic != ECAST_MAGIC) return;
    if (ECAST_TYPE_OF(h->ver_type) != ECAST_TYPE_AUDIO) return;

    /* Only decrypt for the active stream. */
    if (!s_active_stream_valid) return;
    if (h->stream_id16 != s_active_stream_id16) return;

    /* Look up the room by low-16 of stream_id (matches source's id16 stamp). */
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_room_count; i++) {
        if ((uint16_t)(s_rooms[i].stream_id & 0xFFFFU) == h->stream_id16 &&
            s_rooms[i].valid) { idx = i; break; }
    }
    uint8_t session_key[16];
    uint8_t enc_iv[8];
    bool    encrypted = false;
    bool    key_valid = false;
    if (idx >= 0) {
        encrypted = s_rooms[idx].is_encrypted;
        key_valid = s_rooms[idx].session_key_valid;
        memcpy(session_key, s_rooms[idx].session_key, 16);
        memcpy(enc_iv,      s_rooms[idx].enc_iv,      8);
    }
    xSemaphoreGive(s_rooms_mutex);

    if (idx < 0) return;   /* no beacon yet — can't decrypt */

    const uint8_t *cipher_and_mic = data + sizeof(ecast_hdr_t);
    size_t ct_and_mic_len = sizeof(ecast_audio_plain_t) + ECAST_MIC_LEN;

    ecast_audio_plain_t plain;

    if (encrypted) {
        if (!key_valid) return;
        uint8_t nonce[ECAST_NONCE_LEN];
        ecast_make_nonce(nonce, enc_iv, h->psn, h->copy_idx);
        if (!ecast_ccm_decrypt(session_key, nonce,
                               (const uint8_t *)h, sizeof(ecast_hdr_t),
                               cipher_and_mic, ct_and_mic_len,
                               (uint8_t *)&plain)) {
            /* Auth failure — drop silently (forged frame or key mismatch). */
            return;
        }
    } else {
        /* Unencrypted: layout is plaintext + zeroed MIC. */
        memcpy(&plain, cipher_and_mic, sizeof(plain));
    }

    s_packets_rx++;

    /* Pack into legacy audio_raw_t so the unchanged playback task can
     * consume it. capture_us = presentation time minus the advertised
     * delay so the existing clock_offset EMA still converges. */
    espnow_evt_audio_raw_t pkt;
    pkt.seq         = (uint16_t)(h->psn & 0xFFFFU);
    pkt.lost_before = 0;
    pkt.capture_us  = h->pres_time_us
                      - (idx >= 0 ? s_rooms[idx].pres_delay_us : ECAST_PRES_DELAY_US);
    memcpy(pkt.payload, plain.lc3, ECAST_AUDIO_BYTES);

    if (s_audio_queue) {
        if (xQueueSend(s_audio_queue, &pkt, 0) != pdTRUE) {
            espnow_evt_audio_raw_t stale;
            if (xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) s_packets_lost++;
            if (xQueueSend(s_audio_queue, &pkt, 0) != pdTRUE)      s_packets_lost++;
        }
    }
}

static void on_evt_ecast_rssi(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id;
    update_evt_time();
    if (len < sizeof(espnow_evt_stats_t)) return;
    const espnow_evt_stats_t *st = (const espnow_evt_stats_t *)data;
    ESP_LOGI(TAG, "ECast C6: rx=%lu dupe=%lu beacons_ok=%lu rssi=%d ch=%d sdio_err=%u",
             (unsigned long)st->packets_rx, (unsigned long)st->packets_lost,
             (unsigned long)st->plc_frames, st->rssi_last, st->wifi_channel,
             (unsigned)st->sdio_send_errors);

    /* Also refresh RSSI on all rooms so the UI shows live signal strength. */
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; i++) s_rooms[i].rssi = st->rssi_last;
    xSemaphoreGive(s_rooms_mutex);
}

/* ── Register all SDIO event callbacks ───────────────────── */

static esp_err_t register_event_handlers(void)
{
    struct { uint32_t id; void (*cb)(uint32_t, const uint8_t *, size_t); } handlers[] = {
        { ESPNOW_MSG_EVT_STATUS,        on_evt_status     },
        { ESPNOW_MSG_EVT_ROOM_FOUND,    on_evt_room_found },
        { ESPNOW_MSG_EVT_SCAN_DONE,     on_evt_scan_done  },
        { ESPNOW_MSG_EVT_JOINED,        on_evt_joined     },
        { ESPNOW_MSG_EVT_LEFT,          on_evt_left       },
        { ESPNOW_MSG_EVT_AUDIO,         on_evt_audio      },
        { ESPNOW_MSG_EVT_STATS,         on_evt_stats      },
        { ESPNOW_MSG_EVT_ERROR,         on_evt_error      },
        { ESPNOW_MSG_EVT_FW_VER,        on_evt_fw_ver     },
        /* ECast channel */
        { ESPNOW_MSG_EVT_ECAST_BEACON,  on_evt_ecast_beacon },
        { ESPNOW_MSG_EVT_ECAST_AUDIO,   on_evt_ecast_audio  },
        { ESPNOW_MSG_EVT_ECAST_RSSI,    on_evt_ecast_rssi   },
    };

    for (int i = 0; i < (int)(sizeof(handlers) / sizeof(handlers[0])); i++) {
        esp_err_t ret = esp_hosted_register_custom_callback(handlers[i].id, handlers[i].cb);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register handler 0x%lx: %s",
                     (unsigned long)handlers[i].id, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "SDIO event handlers registered");
    return ESP_OK;
}

/* ── Send command to C6 ──────────────────────────────────── */

static esp_err_t send_cmd(uint32_t msg_id, const void *payload, size_t len)
{
    esp_err_t ret = esp_hosted_send_custom_data(msg_id, (const uint8_t *)payload, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_cmd 0x%lx failed: %s",
                 (unsigned long)msg_id, esp_err_to_name(ret));
    }
    return ret;
}

/* ── Public API ──────────────────────────────────────────── */

esp_err_t espnow_sink_init(const espnow_sink_callbacks_t *cbs)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing ESP-NOW sink (LC3 decode on P4)");

    if (cbs) {
        s_cbs = *cbs;
    }

    s_rooms_mutex = xSemaphoreCreateMutex();
    if (!s_rooms_mutex) return ESP_ERR_NO_MEM;

    /* Audio queue: holds raw LC3 packets (~152 bytes each).
     * Capacity 48 (~360 ms worst-case). DRIFT_QUEUE_HIGH=32 trims long before
     * overflow; the extra headroom above DRIFT_QUEUE_HIGH guarantees that
     * even bursty RF delivery (802.11 retry pile-ups) can be queued safely
     * before the drift-trim path gets a chance to react.
     *
     * Total RAM: 48 * 152 B ≈ 7.3 KB — negligible on the P4. */
    s_audio_queue = xQueueCreate(48, sizeof(espnow_evt_audio_raw_t));
    if (!s_audio_queue) return ESP_ERR_NO_MEM;

    esp_err_t ret = register_event_handlers();
    if (ret != ESP_OK) return ret;

    ensure_codec_ready();

    /* Playback task — needs bigger stack for LC3 decode.
     * Pinned to CPU1. LVGL task is pinned to CPU0 (see main.cpp), so audio
     * decode + blocking I2S writes never contend with UI paint. system services
     * (app_main, esp_timer) are on CPU0 too; floater tasks the scheduler can
     * spread across either. This gives UI buttery-smooth refresh and keeps
     * audio glitch-free even during heavy LVGL redraws. */
    xTaskCreatePinnedToCore(playback_task_fn, "espnow_play", 16384, NULL, 7,
                            &s_playback_task, 1);

    s_state       = ESPNOW_STATE_NOT_INIT;
    s_initialized = true;
    s_last_evt_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_c6_fw_valid = false;
    snprintf(s_c6_fw_version, sizeof(s_c6_fw_version), "%s", "unknown");

    ESP_LOGI(TAG, "ESP-NOW sink ready (LC3 decode on P4, waiting for enable)");
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
    if (!s_initialized) return;

    espnow_sink_leave_room();
    espnow_sink_disable();

    if (s_playback_task) {
        vTaskDelete(s_playback_task);
        s_playback_task = NULL;
    }

    uint32_t ids[] = {
        ESPNOW_MSG_EVT_STATUS, ESPNOW_MSG_EVT_ROOM_FOUND, ESPNOW_MSG_EVT_SCAN_DONE,
        ESPNOW_MSG_EVT_JOINED, ESPNOW_MSG_EVT_LEFT, ESPNOW_MSG_EVT_AUDIO,
        ESPNOW_MSG_EVT_STATS, ESPNOW_MSG_EVT_ERROR, ESPNOW_MSG_EVT_FW_VER,
    };
    for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
        esp_hosted_register_custom_callback(ids[i], NULL);
    }

    if (s_audio_queue)  { vQueueDelete(s_audio_queue);      s_audio_queue   = NULL; }
    if (s_rooms_mutex)  { vSemaphoreDelete(s_rooms_mutex);  s_rooms_mutex   = NULL; }

    s_initialized = false;
    s_state       = ESPNOW_STATE_NOT_INIT;
}

esp_err_t espnow_sink_enable(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Sending CMD_INIT to C6");
    return send_cmd(ESPNOW_MSG_CMD_INIT, NULL, 0);
}

esp_err_t espnow_sink_disable(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Sending CMD_DEINIT to C6");
    return send_cmd(ESPNOW_MSG_CMD_DEINIT, NULL, 0);
}

/* ── Auto-scan task ───────────────────────────────────────
 *
 * Owns the complete scan → join → monitor → reconnect lifecycle.
 * Event callbacks (on_evt_scan_done, on_evt_error, on_evt_left) wake this
 * task via s_autoscan_signal; the task itself reads the s_state / flags it
 * accumulated and decides what to do next. All C6 command sequencing
 * happens here, nowhere else. */
static void autoscan_wake(void)
{
    if (s_autoscan_signal) xSemaphoreGive(s_autoscan_signal);
}

/* Pick the room with highest RSSI from the current s_rooms[] table.
 * Returns -1 if no rooms available. Caller must hold no lock. */
static int autoscan_pick_best_room(uint8_t out_mac[6], uint32_t *out_code)
{
    if (!s_rooms_mutex) return -1;
    int best = -1;
    int8_t best_rssi = -127;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; i++) {
        if (!s_rooms[i].valid) continue;
        if (s_rooms[i].rssi > best_rssi) {
            best_rssi = s_rooms[i].rssi;
            best = i;
        }
    }
    if (best >= 0 && out_mac)  memcpy(out_mac,  s_rooms[best].mac, 6);
    if (best >= 0 && out_code) *out_code = s_rooms[best].room_code;
    xSemaphoreGive(s_rooms_mutex);
    return best;
}

static void autoscan_task_fn(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Autoscan task started");

    /* Bring C6 up once at task entry. Every path below keeps it up; only a
     * set_autoscan(false) or task exit tears it down. */
    if (s_c6_espnow_ok == false) {
        espnow_sink_enable();
        /* Give the C6 a moment to enter IDLE. */
        for (int i = 0; i < 20 && s_c6_espnow_ok == false; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    int  reconnect_attempts = 0;

    while (s_autoscan_enabled) {
        espnow_state_t st = s_state;

        switch (st) {
        case ESPNOW_STATE_NOT_INIT:
            /* C6 somehow went down (SDIO reset or similar). Re-enable. */
            ESP_LOGW(TAG, "Autoscan: C6 in NOT_INIT, re-enabling");
            espnow_sink_enable();
            vTaskDelay(pdMS_TO_TICKS(300));
            break;

        case ESPNOW_STATE_IDLE:
            /* IDLE behaviour depends on whether we have a cached target
             * (i.e. the user previously connected to a specific source):
             *
             *   - No cached target: continuously scan so the UI room list
             *     stays fresh for the user to pick from.
             *   - Cached target: we were connected and got dropped — try
             *     to rejoin that same room (the retry block after this
             *     switch handles that). */
            s_autoscan_scan_done = false;
            ESP_LOGI(TAG, "Autoscan: starting scan");
            if (espnow_sink_start_scan() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            }

            /* Wait up to SCAN_TIMEOUT for the C6's SCAN_DONE event. */
            for (uint32_t waited = 0;
                 waited < AUTOSCAN_SCAN_TIMEOUT_MS && s_autoscan_enabled;
                 waited += 50) {
                if (s_autoscan_scan_done) break;
                xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(50));
            }

            if (!s_autoscan_enabled) break;

            /* Auto-join policy:
             *   - If a cached target exists (we were connected and dropped),
             *     the post-switch retry block handles rejoin.
             *   - Else, if any rooms are visible in this scan, auto-join the
             *     one with the strongest RSSI. This preserves the "tap to
             *     connect" UX (the UI also calls espnow_sink_join_room on
             *     tap, which just overrides the cache to whatever the user
             *     picked) while giving zero-touch auto-connect behaviour
             *     when there's only a single source or the user hasn't
             *     picked yet.
             *   - Else no rooms: pause briefly and rescan. */
            bool has_cache =
                (s_autoscan_target_mac[0] | s_autoscan_target_mac[1] |
                 s_autoscan_target_mac[2] | s_autoscan_target_mac[3] |
                 s_autoscan_target_mac[4] | s_autoscan_target_mac[5]) != 0;
            if (!has_cache) {
                uint8_t  mac[6];
                uint32_t code;
                int best = autoscan_pick_best_room(mac, &code);
                if (best >= 0) {
                    ESP_LOGI(TAG, "Autoscan: auto-join strongest room idx=%d "
                             "code=0x%08" PRIX32, best, code);
                    memcpy(s_autoscan_target_mac, mac, 6);
                    s_autoscan_target_code = code;
                    reconnect_attempts = 0;
                    espnow_sink_join_room(best);
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(AUTOSCAN_RESCAN_DELAY_MS));
            break;

        case ESPNOW_STATE_SCANNING:
            /* Already scanning (either we kicked it off just above, or the
             * UI kicked it). Wait briefly for scan_done. */
            xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(200));
            break;

        case ESPNOW_STATE_JOINING:
            /* Wait for JOIN result. Short, bounded. */
            xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(AUTOSCAN_JOIN_TIMEOUT_MS));
            /* If still JOINING after timeout, treat as a failed attempt. */
            if (s_state == ESPNOW_STATE_JOINING) {
                ESP_LOGW(TAG, "Autoscan: JOIN timed out, will retry");
                send_cmd(ESPNOW_MSG_CMD_LEAVE, NULL, 0);
                s_state = ESPNOW_STATE_IDLE;
                vTaskDelay(pdMS_TO_TICKS(AUTOSCAN_RECONNECT_DELAY_MS));
                reconnect_attempts++;
                if (reconnect_attempts >= AUTOSCAN_MAX_RECONNECT) {
                    ESP_LOGW(TAG, "Autoscan: giving up on target, rescanning");
                    memset(s_autoscan_target_mac, 0, 6);
                    reconnect_attempts = 0;
                    s_force_leave_before_scan = true;
                }
            }
            break;

        case ESPNOW_STATE_CONNECTED:
            /* Steady-state: sleep, wake on signal (drop/leave/timeout). */
            s_autoscan_audio_drop = false;
            reconnect_attempts = 0;
            xSemaphoreTake(s_autoscan_signal, pdMS_TO_TICKS(1000));

            if (s_autoscan_audio_drop && s_state != ESPNOW_STATE_CONNECTED) {
                /* Audio stopped arriving - C6 already dropped us back to
                 * IDLE via on_evt_error. Attempt rejoin of the same room. */
                ESP_LOGW(TAG, "Autoscan: audio drop detected");
            }
            break;

        case ESPNOW_STATE_ERROR:
            ESP_LOGW(TAG, "Autoscan: C6 in ERROR, reinitialising");
            espnow_sink_disable();
            vTaskDelay(pdMS_TO_TICKS(300));
            espnow_sink_enable();
            vTaskDelay(pdMS_TO_TICKS(300));
            break;
        }

        /* Retry loop after a drop. Only runs if we have a cached target MAC
         * (meaning we were connected or mid-join) and the state is IDLE. */
        if (s_autoscan_enabled &&
            s_state == ESPNOW_STATE_IDLE &&
            s_autoscan_target_mac[0] | s_autoscan_target_mac[1] |
            s_autoscan_target_mac[2] | s_autoscan_target_mac[3] |
            s_autoscan_target_mac[4] | s_autoscan_target_mac[5]) {

            if (reconnect_attempts < AUTOSCAN_MAX_RECONNECT) {
                /* Find the cached room in the current list by MAC. */
                int idx = -1;
                xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
                for (int i = 0; i < s_room_count; i++) {
                    if (memcmp(s_rooms[i].mac, s_autoscan_target_mac, 6) == 0) {
                        idx = i; break;
                    }
                }
                xSemaphoreGive(s_rooms_mutex);

                if (idx >= 0) {
                    reconnect_attempts++;
                    ESP_LOGI(TAG, "Autoscan: reconnect attempt %d/%d to cached room",
                             reconnect_attempts, AUTOSCAN_MAX_RECONNECT);
                    espnow_sink_join_room(idx);
                } else {
                    /* Cached MAC not in current scan list — need a fresh
                     * scan first to re-discover it. Fall through to IDLE
                     * case next iteration. */
                    ESP_LOGI(TAG, "Autoscan: cached room not visible, rescanning");
                }
                vTaskDelay(pdMS_TO_TICKS(AUTOSCAN_RECONNECT_DELAY_MS));
            } else {
                ESP_LOGW(TAG, "Autoscan: %d reconnects failed, abandoning target",
                         AUTOSCAN_MAX_RECONNECT);
                memset(s_autoscan_target_mac, 0, 6);
                s_autoscan_target_code = 0;
                reconnect_attempts = 0;
                s_force_leave_before_scan = true;
            }
        }
    }

    ESP_LOGI(TAG, "Autoscan task exiting");
    s_autoscan_task = NULL;
    vTaskDelete(NULL);
}

void espnow_sink_set_autoscan(bool enable)
{
    if (!s_initialized) return;

    if (enable) {
        if (s_autoscan_task) return;  /* already running */
        if (!s_autoscan_signal) {
            s_autoscan_signal = xSemaphoreCreateBinary();
        }
        s_autoscan_enabled = true;
        xTaskCreatePinnedToCore(autoscan_task_fn, "sink_autoscan",
                                4096, NULL, 4, &s_autoscan_task, 0);
    } else {
        s_autoscan_enabled = false;
        autoscan_wake();  /* kick it so it notices and exits */
        /* Task deletes itself; we don't join synchronously. */
    }
}

bool espnow_sink_get_autoscan(void)
{
    return s_autoscan_enabled;
}

esp_err_t espnow_sink_start_scan(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (s_state == ESPNOW_STATE_CONNECTED || s_state == ESPNOW_STATE_JOINING || s_selected_room >= 0 || s_force_leave_before_scan) {
        /* Best-effort leave first to avoid C6 "Leave room before scanning" state conflicts. */
        if (s_state == ESPNOW_STATE_CONNECTED) {
            espnow_sink_leave_room();
        } else {
            send_cmd(ESPNOW_MSG_CMD_LEAVE, NULL, 0);
            s_state = ESPNOW_STATE_IDLE;
            s_selected_room = -1;
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        s_force_leave_before_scan = false;
    }

    ESP_LOGI(TAG, "Starting room scan via C6");

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    memset(s_rooms, 0, sizeof(s_rooms));
    s_room_count = 0;
    xSemaphoreGive(s_rooms_mutex);

    s_state = ESPNOW_STATE_SCANNING;
    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_SCANNING);
    }

    return send_cmd(ESPNOW_MSG_CMD_SCAN, NULL, 0);
}

void espnow_sink_stop_scan(void)
{
    if (!s_initialized) return;
    send_cmd(ESPNOW_MSG_CMD_STOP_SCAN, NULL, 0);
}

int espnow_sink_get_rooms(espnow_room_info_t *rooms, int max_rooms)
{
    if (!s_rooms_mutex || !rooms) return 0;

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    int count = (s_room_count < max_rooms) ? s_room_count : max_rooms;
    memcpy(rooms, s_rooms, count * sizeof(espnow_room_info_t));
    xSemaphoreGive(s_rooms_mutex);

    return count;
}

esp_err_t espnow_sink_join_room(int room_index)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);

    if (room_index < 0 || room_index >= s_room_count) {
        xSemaphoreGive(s_rooms_mutex);
        ESP_LOGE(TAG, "Invalid room index %d (count=%d)", room_index, s_room_count);
        return ESP_ERR_INVALID_ARG;
    }

    espnow_room_info_t *room = &s_rooms[room_index];

    espnow_cmd_join_t cmd;
    memcpy(cmd.mac, room->mac, 6);
    cmd.wifi_channel = room->wifi_channel;
    cmd.room_code    = room->room_code;
    cmd.stream_id    = room->stream_id;

    xSemaphoreGive(s_rooms_mutex);

    ESP_LOGI(TAG, "Joining room code=0x%08" PRIX32 " ch=%d stream=%lu",
             cmd.room_code, cmd.wifi_channel, (unsigned long)cmd.stream_id);

    /* Cache for autoscan auto-reconnect. Also reset retry counter so a
     * fresh user-initiated join doesn't inherit a stale attempt count. */
    memcpy(s_autoscan_target_mac, cmd.mac, 6);
    s_autoscan_target_code = cmd.room_code;

    s_selected_room = room_index;
    s_state = ESPNOW_STATE_JOINING;
    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_JOINING);
    }

    return send_cmd(ESPNOW_MSG_CMD_JOIN, &cmd, sizeof(cmd));
}

bool espnow_sink_leave_room(void)
{
    if (!s_initialized || s_state != ESPNOW_STATE_CONNECTED) return true;

    ESP_LOGI(TAG, "Leaving room");

    s_state         = ESPNOW_STATE_IDLE;
    s_selected_room = -1;
    /* Explicit user-initiated leave: do NOT let autoscan auto-reconnect. */
    memset(s_autoscan_target_mac, 0, 6);
    s_autoscan_target_code = 0;

    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t evt_age = now - s_last_evt_ms;
    bool c6_alive = (evt_age < SDIO_ALIVE_TIMEOUT_MS);

    if (c6_alive) {
        esp_err_t ret = send_cmd(ESPNOW_MSG_CMD_LEAVE, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "LEAVE command failed: %s", esp_err_to_name(ret));
            c6_alive = false;
        }
    } else {
        ESP_LOGW(TAG, "Skipping LEAVE: C6 unresponsive (no event for %lums)",
                 (unsigned long)evt_age);
    }

    if (s_audio_queue) {
        xQueueReset(s_audio_queue);
    }

    if (s_cbs.on_state_change) {
        s_cbs.on_state_change(ESPNOW_STATE_IDLE);
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
    if (packets_rx)   *packets_rx   = s_packets_rx;
    if (packets_lost) *packets_lost = s_packets_lost;
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
    if ((out == NULL) || (out_len == 0)) {
        return false;
    }

    snprintf(out, out_len, "%s", s_c6_fw_version);
    return s_c6_fw_valid;
}
