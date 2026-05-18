/*
 * Assistive Listening sink for CrowPanel P4.
 *
 * C6 handles ESP-NOW room discovery/authentication and receives duplicated
 * ESP-NOW SBC frames from the source. C6 forwards verified audio frames to P4
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
#include "oi_codec_sbc.h"
#include "espnow_protocol.h"
#include "espnow_sink.h"

/* Defined in components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c.
 * Returns the running count of SDIO RX bytes that were CMD53'd off the C6
 * but then dropped on the host because the double-buf hand-off slot was
 * full. Each drop = one or more audio packets lost between SDIO bus and
 * the from_slave_queue dispatch — a fix at the host driver level (priority
 * bump for sdio_rx_buf) should keep this at 0. */
extern uint32_t hosted_get_sdio_double_buf_drops(void);

static const char *TAG = "assist_sink";

#define MAX_FRAME_BYTES      ESPNOW_AUDIO_MAX_FRAME_BYTES
#define AUDIO_RATE_HZ        48000
#define LC3_DT_US            ESPNOW_LC3_FRAME_US
#define AUDIO_FRAME_SAMPLES  ESPNOW_SAMPLES_PER_FRAME
#define SBC_FRAMES_PER_PACKET 3
#define SBC_SAMPLES_PER_FRAME 128
#define SBC_FRAME_BYTES      83
#define FRAME_PAYLOAD_BYTES  ESPNOW_LC3_FRAME_BYTES
#define SOURCE_TIMEOUT_MS    10000
#define ROOM_SCAN_STALE_MS   15000

/* Jitter buffer geometry, sized for minimum latency. The strategy is
 * to fix the SDIO stall sources at the root (C6 mempool, pserial
 * priority, P4 SDIO threads pinned to core 0, etc.) so we don't have
 * to absorb large jitter spikes here. Each frame is 8 ms, so MIN=6
 * gives a 48 ms running buffer — close to the user's 50–60 ms total
 * latency goal. MAX=9 gives the adaptive ramp a small amount of room
 * (up to 72 ms) for sustained stress, but the steady state should
 * stay at MIN. If the glitch log shows max_late>5 frames we still
 * have a real transport problem and the right answer is to fix it,
 * not to deepen this buffer. */
#define JB_SIZE              32
#define JB_MASK              (JB_SIZE - 1)
/* Dynamic-latency profile. The jitter buffer depth (and PLL servo
 * target) is a runtime closed-loop controller. Previous iteration drove
 * it from end-to-end `spread` (max-min of packet age over the window),
 * but that over-counts harmless EARLY arrivals — a single packet that
 * landed 50 ms ahead of the average widens `spread` by 50 ms but costs
 * the buffer nothing. The old controller would then needlessly grow to
 * the ceiling and lock 60+ ms of latency in for 20 s.
 *
 * Current signal: `s_max_late_age` — the worst-case number of frames a
 * dropped packet was BEHIND `play_seq` when it finally arrived. That is
 * precisely the depth-deficit the buffer would have needed to catch
 * that packet. We grow by exactly that much (+ safety), capped to one
 * step per window so a single outlier can't catapult us to the
 * ceiling. We separately distinguish:
 *
 *   max_late > 0  : late arrivals → grow (buffer would have helped)
 *   PLC > 0 but max_late == 0 : genuine air/SDIO loss → DO NOT grow
 *       (depth cannot recover a packet that never arrived). Still
 *       allow shrink, because keeping latency high doesn't help.
 *   PLC == 0      : everything clean → shrink after STABLE_WINDOWS
 *
 * Grow step capped at +2 frames/window; shrink step at -1/window. Both
 * asymmetries are intentional: grow only as much as evidence demands;
 * shrink slowly so we don't yo-yo around the ideal depth.
 *
 * Latency outcomes (incl. ~22 ms of fixed source+encode+air+SDIO+I2S):
 *   clean air,         max_late=0   → prebuf=2 → ~38 ms total ⚡
 *   1 frame late/win,  max_late=1   → prebuf=3-4 → ~46-54 ms
 *   3 frames late/win, max_late=3   → prebuf=5-6 → ~62-70 ms
 *   only genuine air loss (max_late=0, plc>0) → adapter does nothing
 *       — buffer cannot help, keep latency low so the inevitable PLC
 *       at least passes through fast.
 *
 * The PLL target tracks prebuffer_target 1:1 so the buffer level
 * settles at the chosen depth instead of fighting the resampler.
 * Deadband stays at ±1 frame regardless of depth — ±8 ms is smaller
 * than any realistic stream skew at 48 kHz. */
/* The old adapter used max_late_snap as its ONLY signal — it grew when
 * packets had already been dropped. By that point the damage was done.
 * The new adapter uses the per-window `spread` (the max-min of packet
 * age over the 5 s window) as a *predictive* jitter estimate: if the
 * link delivered a 56 ms jitter window in the last 5 s, the buffer
 * needs ≥ 7 frames RIGHT NOW to absorb the next similar burst — even
 * if no packet was actually dropped this window.
 *
 * Combined with a "leaky maximum" tracker (instant grow, slow decay)
 * this gives:
 *   - jitter spike → buffer grows immediately the next window
 *   - link stays quiet → buffer leaks back down by 1 frame / 5 s
 *   - sustained low jitter → settles at FLOOR with no oscillation
 *
 * FLOOR is now 3 (24 ms) instead of 2 (16 ms). At 2 frames the buffer
 * cannot absorb even a single jitter event without dropping packets as
 * near-late; 3 frames gives us a tiny safety margin even in the
 * absolute best case. The user explicitly wants 0% loss, so trading
 * 8 ms of latency for that guarantee is the right call. */
#define JB_PREFILL_FLOOR       3   /* 24 ms — absolute minimum (1 frame jitter tolerance) */
#define JB_PREFILL_INIT        5   /* 40 ms — initial seed before first measurement */
#define JB_PREFILL_MAX         8   /* 64 ms — ceiling for sustained-jitter expansion */
#define JB_LATENCY_SAFETY      1   /* frames added above observed jitter */
#define JB_GROW_STEP_CAP       4   /* max frames to grow in one window — fast reaction */
#define JB_SHRINK_LEAK_FRAMES  1   /* tracked-max decay per 5 s when link is quiet */
#define AUX_DMA_FRAME_SAMPLES 64
#define AUX_VOLUME_BOOST_PCT 267  /* +8.5 dB at 100% output volume */
#define PLC_FADE_SAMPLES     96
#define PLC_RESYNC_FRAMES    64
#define JB_PLL_DEADBAND      1
/* Legacy: kept as a no-op fallback for any code still referencing the
 * old name. Real target is jb->pll_target_level (runtime). */
#define JB_LATENCY_TARGET    JB_PREFILL_INIT
#define JB_TRIM_START_LEVEL  (JB_SIZE - 4)
#define JB_TRIM_MIN_INTERVAL_MS 500
#define PLL_PPM_PER_PACKET   30
#define PLL_MAX_PPM          200
#define PLL_ACC_SCALE        1000000
#define OUTPUT_MAX_SAMPLES   (AUDIO_FRAME_SAMPLES + 1)
#define AUDIO_STATS_LOG_ENABLE 0
/* Single-frame PLC threshold — surface even one late packet so we
 * know if the transport ever stalls. */
#define PLC_LOG_THRESHOLD    1
#define PLAYBACK_TASK_STACK  24576
#define AUDIO_RX_TASK_STACK  6144
#define AUDIO_RX_QUEUE_DEPTH 64

_Static_assert(ESPNOW_LC3_FRAME_US == 8000, "P4 ESP-NOW SBC packet duration must be 8 ms");
_Static_assert(ESPNOW_SAMPLE_RATE_HZ == 48000, "P4 ESP-NOW sample rate must be 48 kHz");
_Static_assert(ESPNOW_CHANNELS == 2, "P4 ESP-NOW audio must be stereo");
_Static_assert(ESPNOW_BITS_PER_SAMPLE == 24, "P4 decoded PCM must be 24-bit");
_Static_assert(SBC_FRAMES_PER_PACKET * SBC_SAMPLES_PER_FRAME == ESPNOW_SAMPLES_PER_FRAME,
               "P4 SBC packet sample count mismatch");
_Static_assert(SBC_FRAMES_PER_PACKET * SBC_FRAME_BYTES == ESPNOW_LC3_FRAME_BYTES,
               "P4 ESP-NOW SBC payload size mismatch");
_Static_assert(ESPNOW_AUDIO_COPY_DEFAULT == 4, "P4 ESP-NOW audio must use four copies");
_Static_assert((JB_SIZE & (JB_SIZE - 1)) == 0, "P4 jitter ring size must be a power of two");

typedef struct {
    bool     valid;
    uint32_t stream_id;
    uint32_t seq;
    uint32_t capture_us;
    uint32_t payload_crc32;
    uint8_t  copy_idx;
    uint8_t  copy_count;
    uint16_t frame_bytes;
    uint8_t  channels;
    uint8_t  payload[FRAME_PAYLOAD_BYTES];
} jitter_slot_t;

typedef struct {
    jitter_slot_t slots[JB_SIZE];
    uint32_t play_seq;
    uint32_t highest_seq;
    bool     have_play_seq;
    bool     have_highest_seq;
    bool     started;
    /* Both are adjusted at runtime by the 5 s spread-driven adapter.
     * prebuffer_target: how many frames must be queued before we start
     *   draining after a (re)start. Determines the join latency.
     * pll_target_level: where the resampler PLL aims to keep the
     *   steady-state level. Tracks prebuffer_target 1:1 so the buffer
     *   doesn't oscillate between the prefill threshold and the PLL
     *   set-point. Lives in the struct (not a global / not a #define)
     *   so both fields move atomically under s_jb.mutex. */
    uint8_t  prebuffer_target;
    uint8_t  pll_target_level;
    SemaphoreHandle_t mutex;
} jitter_buffer_t;

static bool s_initialized;
static espnow_state_t s_state = ESPNOW_STATE_NOT_INIT;
static bool s_c6_wifi_ok;
static bool s_c6_espnow_ok;
static uint32_t s_last_evt_ms;
static char s_c6_fw_version[32] = "unknown";
static bool s_c6_fw_valid;

/* Latest stats snapshot from the C6 bridge. Updated in on_evt_stats
 * (called on the SDIO event task), read by the playback task when it
 * emits its 5 s glitch log so we can compare "what we received from
 * the C6" against "what we played", localising loss to either the
 * air/source path or the C6→SDIO→P4 path. */
static portMUX_TYPE       s_c6_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static espnow_evt_stats_t s_c6_stats;
static volatile bool      s_c6_stats_valid;

static espnow_room_info_t s_rooms[ESPNOW_SINK_MAX_ROOMS];
static uint32_t s_room_seen_ms[ESPNOW_SINK_MAX_ROOMS];
static int s_room_count;
static SemaphoreHandle_t s_rooms_mutex;
static int s_selected_room = -1;

static espnow_sink_callbacks_t s_cbs;
static jitter_buffer_t s_jb;
static TaskHandle_t s_playback_task;
static TaskHandle_t s_audio_rx_task;
static QueueHandle_t s_audio_rx_q;
static StaticQueue_t s_audio_rx_q_struct;
static uint8_t s_audio_rx_q_storage[AUDIO_RX_QUEUE_DEPTH * sizeof(espnow_evt_audio_raw_t)];

static i2s_chan_handle_t s_i2s_tx;
static uint8_t s_audio_key[ESPNOW_AUDIO_KEY_LEN];
static uint16_t s_frame_bytes;
static uint8_t s_channels;
static uint8_t s_audio_copy_count;
static volatile bool s_audio_cfg_valid;
static volatile int s_output_volume = 100;

static volatile bool s_audio_rx_active;
static uint32_t s_audio_stream_id;

static volatile uint32_t s_packets_rx;
static volatile uint32_t s_valid_packets;
static volatile uint32_t s_packets_lost;
/* Splits of the old s_duplicate_drops counter so we can tell two very
 * different failure modes apart in the glitch log:
 *
 *   s_near_late_drops : packet arrived after play_seq had already moved past
 *                       it, but only by a frame or two — typically a redundant
 *                       copy of a frame whose copy-0 was lost on air. The C6
 *                       did the right thing forwarding it; we just couldn't
 *                       wait any longer. Fix is "deepen prebuffer" or "accept
 *                       slightly stale frames".
 *   s_slot_dup_drops  : jitter slot was already filled with the exact same
 *                       (stream_id, seq). That means the C6 forwarded the
 *                       same seq twice — a C6-side dedupe failure. Fix is
 *                       on the C6 (widen dedupe ring, fix wrap/race).
 *   s_max_late_age    : worst-case "frames behind play_seq" for any packet
 *                       dropped in the current 5 s reporting window. Tells
 *                       us in concrete numbers whether the current adaptive
 *                       prebuf depth is sized correctly. If max_late stays
 *                       well below prebuf we have headroom; if it equals or
 *                       exceeds prebuf the adapter will grow next window.
 *                       Reset by the stats task each window.
 */
static volatile uint32_t s_near_late_drops;
static volatile uint32_t s_slot_dup_drops;
static volatile uint32_t s_late_drops;
static volatile uint32_t s_max_late_age;
/* End-to-end "source.capture → P4.jb_insert" age telemetry. The source
 * stamps capture_us in each packet using its own esp_timer; the P4
 * subtracts that from its own esp_timer when the packet lands in the
 * jitter buffer. The two clocks are NOT synced so the absolute value of
 * the age has an unknown constant offset baked in — but the SPREAD
 * (max − min) is meaningful: it directly measures the end-to-end
 * transport jitter (encoder → ESP-NOW air → C6 dedupe → SDIO → P4
 * audio_rx_task → jb_insert). If max−min is, say, 50 ms then any
 * jitter buffer shallower than 50 ms WILL drop packets, no matter
 * what. Reset by the stats task each window. */
static volatile int64_t  s_age_min_us = INT64_MAX;
static volatile int64_t  s_age_max_us = INT64_MIN;
/* Same idea as s_age_{min,max}_us above, but using the C6's local clock
 * stamped into evt.c6_send_us right before audio_forward_task calls
 * sdio_send. The two clocks are not synced so the constant offset is
 * unknown — but max−min of (p4_now − c6_send_us) across all packets in
 * a window IS meaningful: it equals the C6→P4 transport jitter ONLY
 * (RPC req_queue wait + IDF SDIO TX queue wait + on-wire CMD53 +
 * P4 sdio_read → rpc_rx → audio_rx_task). If this matches `spread`,
 * the stall is entirely between C6 and P4; if it's much smaller than
 * `spread`, the stall is on the source or in the air. */
static volatile int64_t  s_c6_transit_min_us = INT64_MAX;
static volatile int64_t  s_c6_transit_max_us = INT64_MIN;
static volatile uint32_t s_wrong_stream_drops;
static volatile uint32_t s_crc_failures;
static volatile uint32_t s_plc_frames;
static volatile uint32_t s_jitter_overflow;
static volatile uint32_t s_audio_queue_drops;
static volatile uint32_t s_rebuffer_events;
static volatile uint32_t s_latency_trims;
static volatile uint32_t s_last_latency_trim_ms;
static volatile uint32_t s_pll_drop_samples;
static volatile uint32_t s_pll_insert_samples;
static volatile int s_pll_ppm;
static volatile uint32_t s_copy_hist[ESPNOW_AUDIO_COPY_DEFAULT];
static volatile uint32_t s_last_audio_ms;
static volatile uint32_t s_last_room_found_ms;

static volatile bool s_autoscan_enabled;
static volatile bool s_autoscan_autojoin;
static TaskHandle_t s_autoscan_task;
static SemaphoreHandle_t s_autoscan_signal;
static volatile bool s_autoscan_scan_done;
static uint8_t s_autoscan_target_mac[6];
static uint32_t s_autoscan_target_code;
static uint32_t s_autoscan_target_stream;
static bool s_autoscan_target_valid;
static volatile bool s_autoscan_force_rescan;

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

static void fill_plc_frame(int16_t *dst, const int16_t *last, bool have_last, uint8_t plc_run)
{
    if (!have_last) {
        memset(dst, 0, AUDIO_FRAME_SAMPLES * 2 * sizeof(int16_t));
        return;
    }

    int32_t gain_q15;
    if (plc_run == 0) {
        gain_q15 = 32113;  /* -0.2 dB */
    } else if (plc_run == 1) {
        gain_q15 = 31130;  /* -0.5 dB */
    } else if (plc_run == 2) {
        gain_q15 = 29491;  /* -0.9 dB */
    } else if (plc_run == 3) {
        gain_q15 = 27853;  /* -1.4 dB */
    } else if (plc_run == 4) {
        gain_q15 = 24576;  /* -2.5 dB */
    } else if (plc_run == 5) {
        gain_q15 = 20480;  /* -4.1 dB */
    } else if (plc_run == 6) {
        gain_q15 = 16384;  /* -6 dB */
    } else if (plc_run == 7) {
        gain_q15 = 12288;  /* -8.5 dB */
    } else {
        gain_q15 = 8192;   /* -12 dB for longer loss bursts */
    }

    for (int i = 0; i < AUDIO_FRAME_SAMPLES * 2; ++i) {
        dst[i] = (int16_t)(((int32_t)last[i] * gain_q15) >> 15);
    }

    int fade = PLC_FADE_SAMPLES;
    if (fade > AUDIO_FRAME_SAMPLES) {
        fade = AUDIO_FRAME_SAMPLES;
    }
    for (int i = 0; i < fade; ++i) {
        int32_t a = fade - i;
        int32_t b = i;
        for (int ch = 0; ch < 2; ++ch) {
            int idx = (i << 1) | ch;
            int16_t prev = last[((AUDIO_FRAME_SAMPLES - 1) << 1) | ch];
            dst[idx] = (int16_t)(((int32_t)prev * a + (int32_t)dst[idx] * b) / fade);
        }
    }
}

static void smooth_recovery_frame(int16_t *pcm, const int16_t *last, uint8_t plc_run)
{
    int fade = PLC_FADE_SAMPLES;
    if (fade > AUDIO_FRAME_SAMPLES) {
        fade = AUDIO_FRAME_SAMPLES;
    }

    int32_t gain_q15;
    if (plc_run <= 1) {
        gain_q15 = 31130;
    } else if (plc_run <= 3) {
        gain_q15 = 29491;
    } else if (plc_run <= 5) {
        gain_q15 = 24576;
    } else if (plc_run <= 7) {
        gain_q15 = 16384;
    } else {
        gain_q15 = 8192;
    }
    for (int i = 0; i < fade; ++i) {
        int32_t a = fade - i;
        int32_t b = i;
        for (int ch = 0; ch < 2; ++ch) {
            int idx = (i << 1) | ch;
            int16_t plc = (int16_t)(((int32_t)last[((AUDIO_FRAME_SAMPLES - 1) << 1) | ch] * gain_q15) >> 15);
            pcm[idx] = (int16_t)(((int32_t)plc * a + (int32_t)pcm[idx] * b) / fade);
        }
    }
}

static inline void render_i2s_sample(int32_t *out, int out_idx, int16_t left_s16, int16_t right_s16)
{
    if (s_output_volume >= 100 && AUX_VOLUME_BOOST_PCT == 100) {
        out[out_idx << 1]       = (int32_t)left_s16 << 16;
        out[(out_idx << 1) | 1] = (int32_t)right_s16 << 16;
    } else {
        int32_t left = (int32_t)left_s16 << 8;
        int32_t right = (int32_t)right_s16 << 8;
        left = scale_pcm_s24(left, s_output_volume, AUX_VOLUME_BOOST_PCT);
        right = scale_pcm_s24(right, s_output_volume, AUX_VOLUME_BOOST_PCT);
        out[out_idx << 1]       = left << 8;
        out[(out_idx << 1) | 1] = right << 8;
    }
}

static int render_i2s_frame(const int16_t *pcm, int32_t *out, int slip)
{
    int out_samples = AUDIO_FRAME_SAMPLES;
    if (slip > 0) {
        out_samples = AUDIO_FRAME_SAMPLES - 1;
    } else if (slip < 0) {
        out_samples = AUDIO_FRAME_SAMPLES + 1;
    }

    if (out_samples == 1) {
        render_i2s_sample(out, 0, pcm[0], pcm[1]);
        return out_samples;
    }

    const int32_t step_q16 = ((AUDIO_FRAME_SAMPLES - 1) << 16) / (out_samples - 1);
    for (int i = 0; i < out_samples; ++i) {
        int32_t pos_q16 = i * step_q16;
        int idx = pos_q16 >> 16;
        int frac = pos_q16 & 0xFFFF;
        if (idx >= AUDIO_FRAME_SAMPLES - 1) {
            idx = AUDIO_FRAME_SAMPLES - 1;
            frac = 0;
        }

        int next = (idx < AUDIO_FRAME_SAMPLES - 1) ? idx + 1 : idx;
        int32_t left = ((int32_t)pcm[idx << 1] * (65536 - frac) +
                        (int32_t)pcm[next << 1] * frac) >> 16;
        int32_t right = ((int32_t)pcm[(idx << 1) | 1] * (65536 - frac) +
                         (int32_t)pcm[(next << 1) | 1] * frac) >> 16;
        render_i2s_sample(out, i, left, right);
    }
    return out_samples;
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
    memcpy(&nonce[0], "SBCA", 4);
    nonce[4] = (uint8_t)stream_id;
    nonce[5] = (uint8_t)(stream_id >> 8);
    nonce[6] = (uint8_t)(stream_id >> 16);
    nonce[7] = (uint8_t)(stream_id >> 24);
    nonce[8] = (uint8_t)seq;
    nonce[9] = (uint8_t)(seq >> 8);
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

static void clear_rooms_locked(void)
{
    memset(s_rooms, 0, sizeof(s_rooms));
    memset(s_room_seen_ms, 0, sizeof(s_room_seen_ms));
    s_room_count = 0;
    s_selected_room = -1;
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
    jb->prebuffer_target = JB_PREFILL_INIT;
    jb->pll_target_level = JB_PREFILL_INIT;
    jb->mutex = xSemaphoreCreateMutex();
}

static bool jb_insert_packet(jitter_buffer_t *jb, const espnow_evt_audio_raw_t *pkt)
{
    bool inserted = false;
    /* Record end-to-end transport age BEFORE any drop check. spread now
     * reflects the jitter of EVERY audio packet that crossed the
     * source→C6→SDIO→P4 link this window, including the ones we end up
     * rejecting as late or duplicate. If we only counted accepted
     * packets the late drops (which by definition are the WORST stalls)
     * would be hidden from the diagnostic. */
    int64_t now_us = esp_timer_get_time();
    if (pkt->capture_us != 0) {
        int64_t age = now_us - (int64_t)pkt->capture_us;
        if (age < s_age_min_us) s_age_min_us = age;
        if (age > s_age_max_us) s_age_max_us = age;
    }
    /* C6→P4 transport-only jitter. c6_send_us is the C6's local clock at
     * the moment audio_forward_task handed the packet to sdio_send.
     * Subtracting from p4_now gives a value with an unknown constant
     * offset (clocks not synced), but max−min over a window equals
     * exactly the C6→P4 transit jitter. Zero means the packet came from
     * an old C6 firmware that doesn't stamp this field — treat as
     * absent and skip. */
    if (pkt->c6_send_us != 0) {
        int64_t transit = now_us - (int64_t)(uint32_t)pkt->c6_send_us;
        if (transit < s_c6_transit_min_us) s_c6_transit_min_us = transit;
        if (transit > s_c6_transit_max_us) s_c6_transit_max_us = transit;
    }

    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    if (!jb->have_play_seq) {
        jb->play_seq = pkt->seq;
        jb->have_play_seq = true;
    }

    if ((int32_t)(pkt->seq - jb->play_seq) < 0) {
        uint32_t age = jb->play_seq - pkt->seq;
        if (age > s_max_late_age) s_max_late_age = age;
        if (pkt->copy_idx > 0 || age <= ESPNOW_AUDIO_COPY_DEFAULT) {
            s_near_late_drops++;
        } else {
            s_late_drops++;
        }
        goto out;
    }
    if ((uint32_t)(pkt->seq - jb->play_seq) >= JB_SIZE) {
        s_jitter_overflow++;
        goto out;
    }

    jitter_slot_t *slot = &jb->slots[pkt->seq & JB_MASK];
    if (slot->valid && slot->stream_id == pkt->stream_id && slot->seq == pkt->seq) {
        s_slot_dup_drops++;
        goto out;
    }

    if (slot->valid) {
        s_jitter_overflow++;
        goto out;
    }

    slot->valid = true;
    slot->stream_id = pkt->stream_id;
    slot->seq = pkt->seq;
    slot->capture_us = pkt->capture_us;
    slot->payload_crc32 = pkt->payload_crc32;
    slot->copy_idx = pkt->copy_idx;
    slot->copy_count = pkt->copy_count;
    slot->frame_bytes = pkt->frame_bytes;
    slot->channels = pkt->channels;
    memcpy(slot->payload, pkt->payload, FRAME_PAYLOAD_BYTES);

    if (!jb->have_highest_seq || (int32_t)(pkt->seq - jb->highest_seq) > 0) {
        jb->highest_seq = pkt->seq;
        jb->have_highest_seq = true;
    }
    if (pkt->copy_idx < ESPNOW_AUDIO_COPY_DEFAULT) {
        s_copy_hist[pkt->copy_idx]++;
    }
    inserted = true;

out:
    xSemaphoreGive(jb->mutex);
    return inserted;
}

static bool jb_ready_to_play(jitter_buffer_t *jb)
{
    bool ready = false;
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    if (jb->started) {
        ready = true;
    } else if (jb->have_play_seq && jb->have_highest_seq &&
               (int32_t)(jb->highest_seq - jb->play_seq + 1) >= jb->prebuffer_target) {
        jb->started = true;
        ready = true;
    }
    xSemaphoreGive(jb->mutex);
    return ready;
}

static bool jb_take_play_packet(jitter_buffer_t *jb, espnow_evt_audio_raw_t *pkt)
{
    bool found = false;
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    if (!jb->have_play_seq) {
        goto out;
    }

    jitter_slot_t *slot = &jb->slots[jb->play_seq & JB_MASK];
    if (slot->valid && slot->stream_id == s_audio_stream_id && slot->seq == jb->play_seq) {
        pkt->stream_id = slot->stream_id;
        pkt->seq = slot->seq;
        pkt->capture_us = slot->capture_us;
        pkt->payload_crc32 = slot->payload_crc32;
        pkt->copy_idx = slot->copy_idx;
        pkt->copy_count = slot->copy_count;
        pkt->frame_bytes = slot->frame_bytes;
        pkt->channels = slot->channels;
        pkt->lc3_dt_us = LC3_DT_US;
        pkt->sample_rate_hz = AUDIO_RATE_HZ;
        memcpy(pkt->payload, slot->payload, FRAME_PAYLOAD_BYTES);
        slot->valid = false;
        found = true;
    }
    jb->play_seq++;

out:
    xSemaphoreGive(jb->mutex);
    return found;
}

static void jb_drop_stale_slots_locked(jitter_buffer_t *jb)
{
    for (int i = 0; i < JB_SIZE; ++i) {
        jitter_slot_t *slot = &jb->slots[i];
        if (slot->valid && (int32_t)(slot->seq - jb->play_seq) < 0) {
            slot->valid = false;
        }
    }
}

static int jb_valid_level_locked(const jitter_buffer_t *jb)
{
    int level = 0;
    if (!jb->have_play_seq) {
        return 0;
    }

    for (int i = 0; i < JB_SIZE; ++i) {
        const jitter_slot_t *slot = &jb->slots[i];
        if (slot->valid &&
            slot->stream_id == s_audio_stream_id &&
            (int32_t)(slot->seq - jb->play_seq) >= 0 &&
            (uint32_t)(slot->seq - jb->play_seq) < JB_SIZE) {
            level++;
        }
    }
    return level;
}

static void jb_trim_latency(jitter_buffer_t *jb)
{
    uint32_t now = now_ms();
    if (s_last_latency_trim_ms != 0 &&
        (uint32_t)(now - s_last_latency_trim_ms) < JB_TRIM_MIN_INTERVAL_MS) {
        return;
    }

    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    int level = jb_valid_level_locked(jb);
    if (level >= JB_TRIM_START_LEVEL && jb->have_highest_seq) {
        uint32_t target = jb->play_seq + 1;
        if ((int32_t)(jb->highest_seq - target) >= 0) {
            s_latency_trims++;
            jb->play_seq = target;
            s_last_latency_trim_ms = now;
            jb_drop_stale_slots_locked(jb);
        }
    }
    xSemaphoreGive(jb->mutex);
}

static void jb_resync_to_live_edge(jitter_buffer_t *jb)
{
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    if (jb->have_highest_seq) {
        uint8_t depth = jb->prebuffer_target ? jb->prebuffer_target : JB_PREFILL_INIT;
        uint32_t target = (jb->highest_seq >= (uint32_t)(depth - 1))
                        ? (jb->highest_seq - depth + 1)
                        : 0;

        for (uint32_t seq = target; (int32_t)(jb->highest_seq - seq) >= 0; ++seq) {
            jitter_slot_t *slot = &jb->slots[seq & JB_MASK];
            if (slot->valid && slot->stream_id == s_audio_stream_id && slot->seq == seq) {
                target = seq;
                break;
            }
        }

        if (!jb->have_play_seq || (int32_t)(target - jb->play_seq) > 0) {
            jb->play_seq = target;
            jb->have_play_seq = true;
        }
        jb_drop_stale_slots_locked(jb);
    }
    jb->started = false;
    s_rebuffer_events++;
    xSemaphoreGive(jb->mutex);
}

static void jb_reset(jitter_buffer_t *jb)
{
    xSemaphoreTake(jb->mutex, portMAX_DELAY);
    memset(jb->slots, 0, sizeof(jb->slots));
    jb->play_seq = 0;
    jb->highest_seq = 0;
    jb->have_play_seq = false;
    jb->have_highest_seq = false;
    jb->started = false;
    /* Reset to the initial seed depth, not the previous adapter state —
     * a stream change implies the air conditions may also have shifted
     * (e.g. user moved between rooms) so we re-learn from scratch. */
    jb->prebuffer_target = JB_PREFILL_INIT;
    jb->pll_target_level = JB_PREFILL_INIT;
    s_last_latency_trim_ms = 0;
    xSemaphoreGive(jb->mutex);
}

static void process_audio_packet(const espnow_evt_audio_raw_t *pkt)
{
    if (!pkt || !s_audio_cfg_valid || !s_audio_rx_active) {
        return;
    }
    if (pkt->stream_id != s_audio_stream_id) {
        s_wrong_stream_drops++;
        return;
    }
    if (pkt->frame_bytes != FRAME_PAYLOAD_BYTES ||
        pkt->channels != ESPNOW_CHANNELS ||
        pkt->copy_count != ESPNOW_AUDIO_COPY_DEFAULT ||
        pkt->copy_idx >= ESPNOW_AUDIO_COPY_DEFAULT ||
        pkt->lc3_dt_us != LC3_DT_US ||
        pkt->sample_rate_hz != AUDIO_RATE_HZ) {
        return;
    }

    if (crc32_audio(pkt->payload, FRAME_PAYLOAD_BYTES) != pkt->payload_crc32) {
        s_crc_failures++;
        return;
    }

    if (jb_insert_packet(&s_jb, pkt)) {
        s_valid_packets++;
        s_last_audio_ms = now_ms();
    }
}

static void audio_rx_task_fn(void *arg)
{
    (void)arg;
    espnow_evt_audio_raw_t pkt;
    ESP_LOGI(TAG, "Audio RX task pinned to core %d", xPortGetCoreID());
    while (1) {
        if (xQueueReceive(s_audio_rx_q, &pkt, portMAX_DELAY) == pdTRUE) {
            process_audio_packet(&pkt);
        }
    }
}

/* ───── Playback task — direct I2S, identical to standalone sink ───── */

static void playback_task_fn(void *arg)
{
    (void)arg;
    static int16_t pcm_s16[AUDIO_FRAME_SAMPLES * 2];
    static int16_t last_pcm_s16[AUDIO_FRAME_SAMPLES * 2];
    static int32_t i2s_buf[OUTPUT_MAX_SAMPLES * 2];
    static uint8_t decrypted[MAX_FRAME_BYTES];
    espnow_evt_audio_raw_t pkt;

    OI_CODEC_SBC_DECODER_CONTEXT *dec = heap_caps_calloc(
        1, sizeof(OI_CODEC_SBC_DECODER_CONTEXT), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    OI_UINT32 *dec_data = heap_caps_calloc(
        CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS), sizeof(OI_UINT32),
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!dec || !dec_data) {
        ESP_LOGE(TAG, "SBC decoder allocation failed dec=%p data=%p", dec, dec_data);
        vTaskDelete(NULL);
        return;
    }

    OI_STATUS dec_status = OI_CODEC_SBC_DecoderReset(
        dec, dec_data,
        CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS) * sizeof(OI_UINT32),
        2, 2, false, false);
    if (dec_status != OI_OK) {
        ESP_LOGE(TAG, "SBC decoder reset failed status=%d", dec_status);
        vTaskDelete(NULL);
        return;
    }

    uint32_t next_stats_ms = now_ms() + 5000;
    uint32_t last_plc_frames = 0;
    uint32_t last_late_drops = 0;
    uint32_t last_queue_drops = 0;
    uint32_t last_jb_overflow = 0;
    uint32_t last_rebuffers = 0;
    uint32_t last_near_late_drops = 0;
    uint32_t last_slot_dup_drops = 0;
    uint32_t last_wrong_stream_drops = 0;
    uint32_t last_crc_failures = 0;
    /* C6 stats baseline for delta diffing in the 5 s glitch log. */
    uint32_t last_p4_rx_from_c6 = 0;
    uint32_t last_c6_packets_rx = 0;
    uint32_t last_c6_forward_fail = 0;
    uint32_t last_c6_header_drops = 0;
    uint32_t last_c6_crc_fails = 0;
    uint32_t last_c6_late_or_dup = 0;
    uint32_t last_c6_sdio_errors = 0;
    uint32_t last_c6_sdio_tx_drop = 0;
    uint32_t last_sdio_dbuf_drops = 0;
    bool     last_c6_stats_seeded = false;
    /* Leaky-max tracker for the dynamic latency adapter (see comment at
     * JB_PREFILL_FLOOR for the design). Each window we bump this up to
     * the observed jitter in frames, or decay it by JB_SHRINK_LEAK_FRAMES
     * if the observed jitter is lower. The buffer target is then
     * tracked_max_frames + JB_LATENCY_SAFETY, clamped to [FLOOR, MAX]. */
    int tracked_max_frames = 0;
    uint8_t plc_run = 0;
    bool have_last_pcm = false;
    int64_t pll_accum = 0;

    ESP_LOGI(TAG, "Playback (core %d): SBC jitter ring prefill_init=%d range=[%d,%d] frame=%dB samples=%d",
             xPortGetCoreID(), JB_PREFILL_INIT, JB_PREFILL_FLOOR, JB_PREFILL_MAX,
             FRAME_PAYLOAD_BYTES, AUDIO_FRAME_SAMPLES);

    while (1) {
        bool have_audio = false;
        bool use_plc = false;

        if (s_audio_rx_active && s_audio_cfg_valid) {
            jb_trim_latency(&s_jb);
        }

        if (s_audio_rx_active && s_audio_cfg_valid && jb_ready_to_play(&s_jb)) {
            if (jb_take_play_packet(&s_jb, &pkt)) {
                uint8_t nonce[16];
                make_audio_nonce(pkt.stream_id, pkt.seq, nonce);
                if (aes_ctr_crypt(s_audio_key, nonce, pkt.payload,
                                  decrypted, FRAME_PAYLOAD_BYTES) == ESP_OK) {
                    const OI_BYTE *frame_data = decrypted;
                    OI_UINT32 frame_bytes = FRAME_PAYLOAD_BYTES;
                    uint32_t sample_offset = 0;
                    bool decode_ok = true;

                    for (int f = 0; f < SBC_FRAMES_PER_PACKET; ++f) {
                        OI_UINT32 pcm_bytes = (AUDIO_FRAME_SAMPLES * 2 - sample_offset * 2) * sizeof(int16_t);
                        dec_status = OI_CODEC_SBC_DecodeFrame(
                            dec, &frame_data, &frame_bytes,
                            &pcm_s16[sample_offset * 2], &pcm_bytes);
                        if (dec_status != OI_OK) {
                            decode_ok = false;
                            break;
                        }
                        sample_offset += pcm_bytes / (sizeof(int16_t) * 2);
                    }

                    if (decode_ok && sample_offset == AUDIO_FRAME_SAMPLES && frame_bytes == 0) {
                        have_audio = true;
                        s_packets_rx++;
                        if (plc_run > 0 && have_last_pcm) {
                            smooth_recovery_frame(pcm_s16, last_pcm_s16, plc_run);
                        }
                        memcpy(last_pcm_s16, pcm_s16, sizeof(last_pcm_s16));
                        have_last_pcm = true;
                        plc_run = 0;
                    } else {
                        use_plc = true;
                    }
                } else {
                    use_plc = true;
                }
            } else {
                use_plc = true;
            }
        }

        if (use_plc) {
            if (plc_run >= PLC_RESYNC_FRAMES) {
                jb_resync_to_live_edge(&s_jb);
            }
            fill_plc_frame(pcm_s16, last_pcm_s16, have_last_pcm, plc_run);
            if (plc_run < UINT8_MAX) {
                plc_run++;
            }
            s_packets_lost++;
            s_plc_frames++;
            have_audio = true;
        }

        if (!have_audio) {
            memset(pcm_s16, 0, sizeof(pcm_s16));
            plc_run = 0;
        }

        int slip = 0;
        if (have_audio) {
            int level_now = 0;
            int pll_target = JB_PREFILL_INIT;
            xSemaphoreTake(s_jb.mutex, portMAX_DELAY);
            level_now = jb_valid_level_locked(&s_jb);
            pll_target = s_jb.pll_target_level;
            xSemaphoreGive(s_jb.mutex);

            int error = level_now - pll_target;
            int ppm = 0;
            /* Wider deadband (±JB_PLL_DEADBAND) and a much smaller
             * per-step ppm keeps the resampler idle for normal
             * level fluctuations and only nudges it gently when the
             * buffer is genuinely drifting. */
            if (error > JB_PLL_DEADBAND) {
                ppm = (error - JB_PLL_DEADBAND) * PLL_PPM_PER_PACKET;
            } else if (error < -JB_PLL_DEADBAND) {
                ppm = (error + JB_PLL_DEADBAND) * PLL_PPM_PER_PACKET;
            }
            if (ppm > PLL_MAX_PPM) {
                ppm = PLL_MAX_PPM;
            } else if (ppm < -PLL_MAX_PPM) {
                ppm = -PLL_MAX_PPM;
            }

            s_pll_ppm = ppm;
            pll_accum += (int64_t)ppm * AUDIO_FRAME_SAMPLES;
            if (pll_accum >= PLL_ACC_SCALE) {
                slip = 1;
                pll_accum -= PLL_ACC_SCALE;
                s_pll_drop_samples++;
            } else if (pll_accum <= -PLL_ACC_SCALE) {
                slip = -1;
                pll_accum += PLL_ACC_SCALE;
                s_pll_insert_samples++;
            }
        }

        int output_samples = render_i2s_frame(pcm_s16, i2s_buf, slip);
        size_t written;
        i2s_channel_write(s_i2s_tx, i2s_buf,
                          output_samples * sizeof(int32_t) * 2,
                          &written, portMAX_DELAY);

        uint32_t now = now_ms();
        if (now >= next_stats_ms) {
            uint32_t play_seq = 0;
            uint8_t prebuffer = 0;
            uint8_t pll_target_snap = 0;
            int level = 0;
            uint32_t plc_delta = s_plc_frames - last_plc_frames;

            /* ── Dynamic-latency adapter ──
             * Snapshot every signal the controller needs AND reset it
             * here, BEFORE the conditional log block, so the reset
             * happens every window — otherwise these stats silently
             * accumulate across windows and starve the adapter.
             *
             * The PRIMARY control signal is `max_late_snap`: the worst
             * number of frames any dropped packet was behind play_seq.
             * That's exactly the depth-deficit the buffer needed to
             * catch the latest packet. `spread` is kept only for the
             * diagnostic log — see the constants comment above for why
             * spread is the wrong signal to drive growth from. */
            uint32_t max_late_snap = s_max_late_age;
            s_max_late_age = 0;

            int64_t age_min_snap = s_age_min_us;
            int64_t age_max_snap = s_age_max_us;
            s_age_min_us = INT64_MAX;
            s_age_max_us = INT64_MIN;
            int64_t spread_us = 0;
            bool spread_valid = false;
            if (age_min_snap != INT64_MAX && age_max_snap != INT64_MIN &&
                age_max_snap > age_min_snap) {
                spread_us = age_max_snap - age_min_snap;
                spread_valid = true;
            }

            int64_t c6_min_snap = s_c6_transit_min_us;
            int64_t c6_max_snap = s_c6_transit_max_us;
            s_c6_transit_min_us = INT64_MAX;
            s_c6_transit_max_us = INT64_MIN;

            xSemaphoreTake(s_jb.mutex, portMAX_DELAY);
            int cur = s_jb.prebuffer_target;
            int target = cur;
            const char *reason = NULL;

            /* === New adapter: leaky-max of observed jitter ============
             *
             * Step 1: Convert this window's observations into a single
             * "observed jitter in frames" number.
             *
             * The primary signal is `spread` — the span between the
             * earliest and latest packet age seen in the last 5 s.
             * This is a PREDICTIVE measure: even if zero packets were
             * actually dropped this window, a 56 ms spread tells us the
             * link CAN deliver bursts that wide, and the next one might
             * not be so kindly aligned with the play clock.
             *
             * If max_late_snap > 0 we ALSO got direct evidence that the
             * buffer was too small — a packet arrived behind play_seq.
             * In that case we use (cur + max_late) as the lower bound,
             * because that's literally the depth we needed to catch it.
             *
             * The MAX of these two is used: spread tells us "this much
             * jitter exists", max_late tells us "this much jitter
             * actually beat the buffer". Taking the max ensures we
             * respond to both. */
            int spread_frames = 0;
            if (spread_valid && spread_us > 0) {
                /* Ceil division: 1 ns of spread above N frames bumps to N+1. */
                spread_frames = (int)((spread_us + 7999) / 8000);
            }
            int late_evidence = (max_late_snap > 0)
                                ? cur + (int)max_late_snap
                                : 0;
            int observed = (spread_frames > late_evidence)
                           ? spread_frames
                           : late_evidence;

            /* Step 2: Leaky maximum tracker.
             *   - observed >  tracked  → bump UP instantly (grow fast)
             *   - observed <= tracked  → decay tracked by LEAK_FRAMES
             *
             * This makes the buffer react to spikes IMMEDIATELY while
             * decaying back down over many quiet windows — exactly the
             * asymmetry we want: cheap to grow when needed, expensive
             * to shrink so a single brief burst doesn't get us caught
             * out by the next one. */
            if (observed > tracked_max_frames) {
                tracked_max_frames = observed;
            } else if (tracked_max_frames > 0) {
                tracked_max_frames -= JB_SHRINK_LEAK_FRAMES;
                if (tracked_max_frames < 0) tracked_max_frames = 0;
            }

            /* Step 3: Compute desired depth and clamp. */
            int desired = tracked_max_frames + JB_LATENCY_SAFETY;
            if (desired < JB_PREFILL_FLOOR) desired = JB_PREFILL_FLOOR;
            if (desired > JB_PREFILL_MAX)   desired = JB_PREFILL_MAX;

            /* Step 4: Step current toward desired with grow/shrink caps.
             * Growth: up to JB_GROW_STEP_CAP frames per window (we want
             * to catch the NEXT burst, not wait for several).
             * Shrink: at most 1 frame per window (slow surrender of
             * safety margin so a quiet window followed by a noisy one
             * doesn't strand us back at the floor). */
            if (desired > cur) {
                int step = desired - cur;
                if (step > JB_GROW_STEP_CAP) step = JB_GROW_STEP_CAP;
                target = cur + step;
                reason = (max_late_snap > 0) ? "late" : "spread";
            } else if (desired < cur) {
                target = cur - 1;
                reason = (plc_delta > 0) ? "shrink-genloss" : "shrink-clean";
            }

            bool target_changed = (target != cur);
            if (target_changed) {
                s_jb.prebuffer_target = (uint8_t)target;
                s_jb.pll_target_level = (uint8_t)target;
            }
            play_seq = s_jb.play_seq;
            prebuffer = s_jb.prebuffer_target;
            pll_target_snap = s_jb.pll_target_level;
            level = jb_valid_level_locked(&s_jb);
            xSemaphoreGive(s_jb.mutex);

            float age_spread_ms = spread_valid ? (float)spread_us / 1000.0f : 0.0f;
            float c6_spread_ms = 0.0f;
            if (c6_min_snap != INT64_MAX && c6_max_snap != INT64_MIN) {
                c6_spread_ms = (float)(c6_max_snap - c6_min_snap) / 1000.0f;
            }
            if (target_changed) {
                /* One line per actual depth change (≤ 1 per 5 s). The
                 * `reason` tag tells you why: "late" = depth bump in
                 * response to late-arrival evidence; "shrink-clean" =
                 * zero PLC for STABLE_WINDOWS so giving back 1 frame;
                 * "shrink-genloss" = PLC from packets that never
                 * arrived (buffer can't help, reclaim latency). */
                ESP_LOGI(TAG,
                         "jb-latency %s [%s]: %d → %d frames (%d → %d ms)"
                         " | max_late=%uf spread=%.1f ms plc=%" PRIu32,
                         (target > cur) ? "grow" : "shrink",
                         reason ? reason : "?",
                         cur, target, cur * 8, target * 8,
                         (unsigned)max_late_snap, age_spread_ms, plc_delta);
            }
#if AUDIO_STATS_LOG_ENABLE
            ESP_LOGI(TAG,
                     "audio stats rx=%" PRIu32 " lost=%" PRIu32 " plc=%" PRIu32
                     " dup=%" PRIu32 " late=%" PRIu32 " wrong=%" PRIu32
                     " crc=%" PRIu32 " qdrop=%" PRIu32 " jb_ovf=%" PRIu32
                     " rebuf=%" PRIu32 " trim=%" PRIu32 " pll=%d"
                     " slip_d=%" PRIu32 " slip_i=%" PRIu32 " jb_level=%d"
                     " prebuf=%u play_seq=%" PRIu32,
                     s_packets_rx, s_packets_lost, s_plc_frames,
                     (s_near_late_drops + s_slot_dup_drops), s_late_drops, s_wrong_stream_drops,
                     s_crc_failures, s_audio_queue_drops, s_jitter_overflow,
                     s_rebuffer_events, s_latency_trims, s_pll_ppm,
                     s_pll_drop_samples, s_pll_insert_samples, level, prebuffer, play_seq);
#endif
            /* Surface only the events that actually corrupt audio so
             * we can correlate them with source-side bursts without
             * spamming the log every 5 s when everything's healthy.
             *
             * When a glitch hits, also dump the matching C6 bridge
             * counters so the line tells us in one place:
             *   - p4_rx: packets the P4 actually received from the C6
             *   - c6_rx: packets the C6 received over the air
             *     → if c6_rx ≈ p4_rx ≈ expected (≈625 in 5 s) but plc>0,
             *       loss happened inside the P4 (decode / jitter buffer)
             *     → if c6_rx ≈ expected but p4_rx ≪ c6_rx, loss happened
             *       on SDIO (look at c6_fwd_fail / sdio_err)
             *     → if c6_rx ≪ expected, loss happened over the air
             *       (source-side stall, interference, range)
             */
            espnow_evt_stats_t c6 = {0};
            bool c6_valid = false;
            portENTER_CRITICAL(&s_c6_stats_lock);
            if (s_c6_stats_valid) {
                c6 = s_c6_stats;
                c6_valid = true;
            }
            portEXIT_CRITICAL(&s_c6_stats_lock);

            uint32_t p4_rx_now = s_packets_rx;
            uint32_t p4_rx_delta = p4_rx_now - last_p4_rx_from_c6;
            uint32_t c6_rx_delta = 0, c6_fwd_fail_delta = 0, c6_hdr_drop_delta = 0;
            uint32_t c6_crc_fail_delta = 0, c6_dup_delta = 0, c6_sdio_err_delta = 0;
            uint32_t c6_tx_drop_delta = 0;
            if (c6_valid) {
                if (!last_c6_stats_seeded) {
                    last_c6_packets_rx   = c6.packets_rx;
                    last_c6_forward_fail = c6.forward_fail;
                    last_c6_header_drops = c6.header_drops;
                    last_c6_crc_fails    = c6.crc_fails;
                    last_c6_late_or_dup  = c6.late_or_dup;
                    last_c6_sdio_errors  = c6.sdio_send_errors;
                    last_c6_sdio_tx_drop = c6.sdio_tx_silent_drops;
                    last_c6_stats_seeded = true;
                }
                c6_rx_delta       = c6.packets_rx      - last_c6_packets_rx;
                c6_fwd_fail_delta = c6.forward_fail    - last_c6_forward_fail;
                c6_hdr_drop_delta = c6.header_drops    - last_c6_header_drops;
                c6_crc_fail_delta = c6.crc_fails       - last_c6_crc_fails;
                c6_dup_delta      = c6.late_or_dup     - last_c6_late_or_dup;
                c6_sdio_err_delta = (uint32_t)c6.sdio_send_errors - last_c6_sdio_errors;
                c6_tx_drop_delta  = c6.sdio_tx_silent_drops - last_c6_sdio_tx_drop;
            }

            uint32_t qd_delta = s_audio_queue_drops - last_queue_drops;
            uint32_t jbo_delta = s_jitter_overflow - last_jb_overflow;
            uint32_t reb_delta = s_rebuffer_events - last_rebuffers;
            uint32_t late_delta = s_late_drops - last_late_drops;
            uint32_t near_late_delta = s_near_late_drops - last_near_late_drops;
            uint32_t slot_dup_delta  = s_slot_dup_drops  - last_slot_dup_drops;
            /* Keep "dup" as the sum of the two so existing log parsers /
             * dashboards still read the same total, but break it down
             * inline so the user can tell which path triggered. */
            uint32_t dup_delta = near_late_delta + slot_dup_delta;
            uint32_t wrong_delta = s_wrong_stream_drops - last_wrong_stream_drops;
            uint32_t crc_delta = s_crc_failures - last_crc_failures;
            uint32_t sdio_dbuf_now = hosted_get_sdio_double_buf_drops();
            uint32_t sdio_dbuf_delta = sdio_dbuf_now - last_sdio_dbuf_drops;
            if (plc_delta >= PLC_LOG_THRESHOLD || qd_delta > 0 ||
                jbo_delta > 0 || reb_delta > 0 ||
                c6_fwd_fail_delta > 0 || c6_sdio_err_delta > 0 ||
                c6_tx_drop_delta > 0 ||
                late_delta > 0 || crc_delta > 0 ||
                sdio_dbuf_delta > 0) {
                /* max_late_snap / spread / c6spread / age_spread_ms /
                 * c6_spread_ms were all snapshotted (and reset) by the
                 * dynamic-latency adapter above so the controller always
                 * has fresh data — we just reuse the values here for the
                 * glitch log.
                 *
                 * Reading the post-fix log: if `c6_tx_drop` is non-zero
                 * the SDIO slave on the C6 side silently lost packets
                 * (mempool exhaustion / send_queue fail / interface
                 * inactive). If c6_tx_drop=0 but p4_rx<c6_rx, then the
                 * loss is on the P4 host RX side — either the SDIO
                 * double-buffer (sdio_dbuf), the serial_ll RX queue,
                 * or the RPC dispatch. With all the buffer-size and
                 * propagation fixes in this iteration, we expect both
                 * c6_tx_drop and (c6_rx − p4_rx) to be 0. */
                ESP_LOGW(TAG,
                         "audio glitch 5s: plc=%" PRIu32 " (%.1f ms) qdrop=%" PRIu32
                         " jb_ovf=%" PRIu32 " rebuf=%" PRIu32
                         " late=%" PRIu32 " dup=%" PRIu32 "(nl=%" PRIu32 ",slot=%" PRIu32 ")"
                         " wrong=%" PRIu32 " crc=%" PRIu32 " max_late=%" PRIu32 "f"
                         " spread=%.1f ms c6spread=%.1f ms sdio_dbuf=%" PRIu32
                         " jb_lvl=%d prebuf=%u pll_tgt=%u pll_ppm=%d"
                         " | p4_rx=%" PRIu32 " c6_rx=%" PRIu32
                         " c6_fwd_fail=%" PRIu32 " c6_hdr_drop=%" PRIu32
                         " c6_crc_fail=%" PRIu32 " c6_dup=%" PRIu32
                         " sdio_err=%" PRIu32 " c6_tx_drop=%" PRIu32
                         " enq_max_us=%" PRIu32
                         " rssi=%d ch=%u",
                         plc_delta, plc_delta * 8.0f, qd_delta, jbo_delta, reb_delta,
                         late_delta, dup_delta, near_late_delta, slot_dup_delta,
                         wrong_delta, crc_delta, max_late_snap,
                         age_spread_ms, c6_spread_ms, sdio_dbuf_delta,
                         level, prebuffer, pll_target_snap, s_pll_ppm,
                         p4_rx_delta, c6_rx_delta,
                         c6_fwd_fail_delta, c6_hdr_drop_delta,
                         c6_crc_fail_delta, c6_dup_delta,
                         c6_sdio_err_delta, c6_tx_drop_delta,
                         c6_valid ? c6.sdio_max_us : 0u,
                         c6_valid ? (int)c6.rssi_last : 0,
                         c6_valid ? (unsigned)c6.wifi_channel : 0);
            }
            last_sdio_dbuf_drops = sdio_dbuf_now;
            last_plc_frames = s_plc_frames;
            last_late_drops = s_late_drops;
            last_queue_drops = s_audio_queue_drops;
            last_jb_overflow = s_jitter_overflow;
            last_rebuffers = s_rebuffer_events;
            last_near_late_drops = s_near_late_drops;
            last_slot_dup_drops  = s_slot_dup_drops;
            last_wrong_stream_drops = s_wrong_stream_drops;
            last_crc_failures = s_crc_failures;
            last_p4_rx_from_c6 = p4_rx_now;
            if (c6_valid) {
                last_c6_packets_rx   = c6.packets_rx;
                last_c6_forward_fail = c6.forward_fail;
                last_c6_header_drops = c6.header_drops;
                last_c6_crc_fails    = c6.crc_fails;
                last_c6_late_or_dup  = c6.late_or_dup;
                last_c6_sdio_errors  = c6.sdio_send_errors;
                last_c6_sdio_tx_drop = c6.sdio_tx_silent_drops;
            }
            next_stats_ms = now + 5000;
        }
    }
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

static int find_or_create_room(const espnow_evt_room_t *evt)
{
    int idx = -1;
    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);

    for (int i = 0; i < s_room_count; ++i) {
        if (memcmp(s_rooms[i].mac, evt->mac, sizeof(evt->mac)) == 0 ||
            (s_rooms[i].room_code == evt->room_code &&
             s_rooms[i].stream_id == evt->stream_id)) {
            idx = i;
            break;
        }
    }

    if (idx < 0 && s_room_count < ESPNOW_SINK_MAX_ROOMS) {
        idx = s_room_count++;
    }

    if (idx >= 0) {
        espnow_room_info_t *room = &s_rooms[idx];
        memset(room, 0, sizeof(*room));
        memcpy(room->mac, evt->mac, sizeof(room->mac));
        room->wifi_channel = evt->wifi_channel;
        room->room_code = evt->room_code;
        room->stream_id = evt->stream_id;
        room->rssi = evt->rssi;
        room->valid = true;
        memcpy(room->name, evt->name, ESPNOW_ROOM_NAME_LEN);
        room->name[sizeof(room->name) - 1] = '\0';
        s_last_room_found_ms = now_ms();
        s_room_seen_ms[idx] = s_last_room_found_ms;
    }

    xSemaphoreGive(s_rooms_mutex);
    return idx;
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
    s_valid_packets = 0;
    s_packets_lost = 0;
    s_near_late_drops = 0;
    s_slot_dup_drops  = 0;
    s_late_drops = 0;
    s_max_late_age = 0;
    s_age_min_us = INT64_MAX;
    s_age_max_us = INT64_MIN;
    s_c6_transit_min_us = INT64_MAX;
    s_c6_transit_max_us = INT64_MIN;
    s_wrong_stream_drops = 0;
    s_crc_failures = 0;
    s_plc_frames = 0;
    s_jitter_overflow = 0;
    s_audio_queue_drops = 0;
    s_rebuffer_events = 0;
    s_latency_trims = 0;
    s_pll_drop_samples = 0;
    s_pll_insert_samples = 0;
    s_pll_ppm = 0;
    memset((void *)s_copy_hist, 0, sizeof(s_copy_hist));
    s_last_audio_ms = now_ms();
    s_audio_cfg_valid = false;
    s_audio_stream_id = joined->stream_id;
    s_autoscan_target_stream = joined->stream_id;
    s_autoscan_force_rescan = false;
    if (s_audio_rx_q) {
        xQueueReset(s_audio_rx_q);
    }
    jb_reset(&s_jb);
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

    portENTER_CRITICAL(&s_c6_stats_lock);
    s_c6_stats = *stats;
    s_c6_stats_valid = true;
    portEXIT_CRITICAL(&s_c6_stats_lock);
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
    if (err->error_code == ESP_ERR_TIMEOUT || err->error_code == ESP_ERR_NOT_FOUND) {
        stop_audio_rx();
        s_audio_cfg_valid = false;
        s_selected_room = -1;
        s_autoscan_force_rescan = true;
        if (err->error_code == ESP_ERR_NOT_FOUND && s_rooms_mutex) {
            xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
            clear_rooms_locked();
            xSemaphoreGive(s_rooms_mutex);
        }
    }
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

    if (cfg->audio_copy_count != 0 && cfg->audio_copy_count != ESPNOW_AUDIO_COPY_DEFAULT) {
        ESP_LOGE(TAG, "Unsupported ESP-NOW copy count %u", cfg->audio_copy_count);
        return;
    }

    s_audio_copy_count = ESPNOW_AUDIO_COPY_DEFAULT;
    s_frame_bytes = cfg->frame_bytes;
    s_channels = channels;
    s_audio_cfg_valid = true;
    jb_reset(&s_jb);
    ESP_LOGI(TAG, "ESP-NOW audio config from C6: frame=%u channels=%u copies=%u",
             cfg->frame_bytes, channels, s_audio_copy_count);
}

static void on_evt_audio(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    if (!data || len < sizeof(espnow_evt_audio_raw_t) ||
        !s_audio_cfg_valid || !s_audio_rx_active || !s_audio_rx_q) {
        return;
    }

    espnow_evt_audio_raw_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (xQueueSendToBack(s_audio_rx_q, &pkt, 0) != pdTRUE) {
        s_audio_queue_drops++;
    }
}

/* ───── ESP-NOW audio receive (triggered after AUDIO_KEY from C6) ───── */

static void start_audio_rx(void)
{
    if (s_audio_rx_q) {
        xQueueReset(s_audio_rx_q);
    }
    jb_reset(&s_jb);
    s_audio_rx_active = true;
    ESP_LOGI(TAG, "ESP-NOW audio RX started: frame=%u ch=%u copies=%u prebuffer=%u",
             s_frame_bytes, s_channels, s_audio_copy_count, s_jb.prebuffer_target);
}

static void stop_audio_rx(void)
{
    s_audio_rx_active = false;
    if (s_audio_rx_q) {
        xQueueReset(s_audio_rx_q);
    }
    jb_reset(&s_jb);
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
    uint8_t channels = key->channels ? key->channels : ESPNOW_CHANNELS;
    if (key->frame_bytes != FRAME_PAYLOAD_BYTES ||
        channels != ESPNOW_CHANNELS ||
        key->lc3_dt_us != LC3_DT_US ||
        key->sample_rate_hz != AUDIO_RATE_HZ) {
        ESP_LOGE(TAG, "Rejected ESP-NOW audio key config frame=%u ch=%u dt=%u rate=%u",
                 key->frame_bytes, channels, key->lc3_dt_us, key->sample_rate_hz);
        s_audio_cfg_valid = false;
        stop_audio_rx();
        return;
    }

    memcpy(s_audio_key, key->audio_key, ESPNOW_AUDIO_KEY_LEN);
    s_frame_bytes = key->frame_bytes;
    s_channels = channels;
    s_audio_copy_count = ESPNOW_AUDIO_COPY_DEFAULT;
    s_audio_cfg_valid = true;

    ESP_LOGI(TAG, "Received ESP-NOW audio key: frame=%u ch=%u dt=%u rate=%u",
             s_frame_bytes, s_channels, key->lc3_dt_us, key->sample_rate_hz);

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
    uint32_t best_seen = 0;
    uint32_t now = now_ms();

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    for (int i = 0; i < s_room_count; ++i) {
        if (!s_rooms[i].valid) {
            continue;
        }
        if (s_room_seen_ms[i] != 0 && (now - s_room_seen_ms[i]) > ROOM_SCAN_STALE_MS) {
            continue;
        }
        if ((memcmp(s_rooms[i].mac, s_autoscan_target_mac, 6) == 0) ||
            (s_rooms[i].room_code == s_autoscan_target_code)) {
            if (idx < 0 || s_rooms[i].stream_id == s_autoscan_target_stream ||
                s_room_seen_ms[i] > best_seen) {
                idx = i;
                best_seen = s_room_seen_ms[i];
            }
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
                stop_audio_rx();
                jb_reset(&s_jb);
                s_selected_room = -1;
                s_autoscan_force_rescan = true;
                notify_state(ESPNOW_STATE_IDLE);
            }
            reconnect_attempts = 0;
            continue;
        }

        if (state == ESPNOW_STATE_IDLE) {
            if (s_autoscan_target_valid && !s_autoscan_force_rescan) {
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
                    s_autoscan_target_stream = 0;
                    reconnect_attempts = 0;
                }
            }

            s_autoscan_force_rescan = false;
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
    s_audio_rx_q = xQueueCreateStatic(AUDIO_RX_QUEUE_DEPTH,
                                      sizeof(espnow_evt_audio_raw_t),
                                      s_audio_rx_q_storage,
                                      &s_audio_rx_q_struct);
    jb_init(&s_jb);
    if (!s_rooms_mutex || !s_autoscan_signal || !s_audio_rx_q || !s_jb.mutex) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(register_event_handlers(), TAG, "register SDIO handlers");
    init_direct_i2s();

    /* Drain SDIO audio first so bursts become jitter-buffer depth, not queue drops. */
    xTaskCreatePinnedToCore(playback_task_fn, "playback", PLAYBACK_TASK_STACK, NULL, 23,
                            &s_playback_task, 1);
    /* Pinned to core 0 to colocate with the ESP-Hosted SDIO threads
     * (which are also pinned to core 0 in main.cpp after esp_hosted_init).
     * Keeping the whole SDIO RX → audio_rx_q → jb_insert pipeline on a
     * single core eliminates the cross-core notify latency and makes the
     * arrival time of each audio packet deterministic relative to the
     * SDIO read that produced it. playback_task stays on core 1 with its
     * own decode + I2S workload so the two halves of the pipeline run in
     * parallel on separate cores. */
    xTaskCreatePinnedToCore(audio_rx_task_fn, "audio_rx", AUDIO_RX_TASK_STACK, NULL, 24,
                            &s_audio_rx_task, 0);

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
    if (s_audio_rx_task) {
        vTaskDelete(s_audio_rx_task);
        s_audio_rx_task = NULL;
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
    jb_reset(&s_jb);
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
        clear_rooms_locked();
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

    uint32_t seen_ms = s_room_seen_ms[room_index];
    if (seen_ms != 0 && (now_ms() - seen_ms) > ROOM_SCAN_STALE_MS) {
        xSemaphoreGive(s_rooms_mutex);
        s_autoscan_force_rescan = true;
        autoscan_wake();
        return ESP_ERR_INVALID_STATE;
    }

    espnow_room_info_t *room = &s_rooms[room_index];
    memcpy(cmd.mac, room->mac, 6);
    cmd.wifi_channel = room->wifi_channel;
    cmd.room_code = room->room_code;
    cmd.stream_id = room->stream_id;
    memcpy(cmd.name, room->name, sizeof(cmd.name));
    s_selected_room = room_index;
    memcpy(s_autoscan_target_mac, room->mac, 6);
    s_autoscan_target_code = room->room_code;
    s_autoscan_target_stream = room->stream_id;
    s_autoscan_target_valid = true;
    s_autoscan_force_rescan = false;
    xSemaphoreGive(s_rooms_mutex);

    s_audio_cfg_valid = false;
    s_last_audio_ms = now_ms();
    jb_reset(&s_jb);
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
    s_autoscan_target_stream = 0;
    s_autoscan_force_rescan = false;
    s_audio_cfg_valid = false;
    jb_reset(&s_jb);
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
