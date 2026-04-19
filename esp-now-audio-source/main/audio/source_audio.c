// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "../core/source_app_internal.h"
#include "minimp3.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"

/* ── MP3 streaming read buffer ──────────────────────────────────────── */
#define MP3_BUF_SIZE   (32 * 1024)  /* 32KB read-ahead buffer */
static uint8_t                *mp3_buf      = NULL;
static size_t                  mp3_buf_len  = 0;   /* valid bytes in buffer */
static size_t                  mp3_flash_pos = 0;  /* current read offset in partition */
static size_t                  mp3_flash_size = 0; /* actual MP3 data size */
static const esp_partition_t  *mp3_partition = NULL;

const char *espnow_rate_name(wifi_phy_rate_t rate) {
    switch (rate) {
        case WIFI_PHY_RATE_6M: return "6M";
        case WIFI_PHY_RATE_12M: return "12M";
        case WIFI_PHY_RATE_24M: return "24M";
#if ESPNOW_HAS_RATE_54M
        case WIFI_PHY_RATE_54M: return "54M";
#endif
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

void beacon_task(void *arg) {
    (void)arg;
    beacon_msg_t b = {0};
    b.h.magic = PROTO_MAGIC;
    b.h.type = MSG_BEACON;
    b.h.room_code = ROOM_CODE;
    b.wifi_channel = wifi_channel;
    b.channels = CHANNELS;
    b.sample_rate_hz = SAMPLE_RATE_HZ;
    b.frame_us = LC3_FRAME_US;
    b.bytes_per_ch = LC3_BYTES_PER_CH;
    b.stream_id = stream_id;

    add_peer_if_needed(BROADCAST_MAC);

    while (1) {
        b.wifi_channel = wifi_channel;
        esp_now_send(BROADCAST_MAC, (const uint8_t *)&b, sizeof(b));
        vTaskDelay(pdMS_TO_TICKS(BEACON_PERIOD_MS));
    }
}

void audio_capture_encode_task(void *arg) {
    (void)arg;

    /* ── Find "music" partition and read size header ────────────────── */
    mp3_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "music");
    if (!mp3_partition) {
        ESP_LOGE(TAG, "Music partition not found!");
        vTaskDelete(NULL);
        return;
    }

    /* First 4 bytes = little-endian uint32 file size, then MP3 data follows */
    uint32_t stored_size = 0;
    esp_partition_read(mp3_partition, 0, &stored_size, 4);
    if (stored_size == 0 || stored_size > mp3_partition->size - 4) {
        ESP_LOGE(TAG, "Invalid MP3 size header: %u (partition=%u)", 
                 (unsigned)stored_size, (unsigned)mp3_partition->size);
        vTaskDelete(NULL);
        return;
    }
    mp3_flash_size = stored_size;
    ESP_LOGI(TAG, "Music partition: %u bytes of MP3 data", (unsigned)mp3_flash_size);

    /* Allocate read buffer */
    mp3_buf = heap_caps_malloc(MP3_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "Failed to allocate MP3 read buffer");
        vTaskDelete(NULL);
        return;
    }

    /* Initial fill (MP3 data starts at offset 4 in partition) */
    mp3_flash_pos = 0;
    size_t to_read = (mp3_flash_size < MP3_BUF_SIZE) ? mp3_flash_size : MP3_BUF_SIZE;
    esp_partition_read(mp3_partition, 4, mp3_buf, to_read);
    mp3_buf_len = to_read;
    mp3_flash_pos = to_read;

    /* ── MP3 decoder state ─────────────────────────────────────────────── */
    static mp3dec_t mp3dec;
    mp3dec_init(&mp3dec);

    size_t         mp3_buf_pos = 0;    /* current decode position within mp3_buf */

    /* Decode buffer – minimp3 outputs up to 1152*2 int16 samples per frame */
    static int16_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int            mp3_pcm_avail   = 0;   /* samples remaining in mp3_pcm (per channel) */
    int            mp3_pcm_pos     = 0;   /* current read position in mp3_pcm (in samples) */
    int            mp3_channels    = 2;
    int            mp3_sample_rate = 48000;

    /* Output buffers for one LC3 frame */
    static int32_t pcm_s24[SAMPLES_PER_FRAME * CHANNELS];  /* 360 * 2 */

    lc3_init_encoder();

    /* Helper: refill mp3_buf – shift unconsumed data to front, read more from flash */
    #define MP3_BUF_REFILL() do { \
        if (mp3_buf_pos > 0) { \
            size_t remain = mp3_buf_len - mp3_buf_pos; \
            if (remain > 0) memmove(mp3_buf, mp3_buf + mp3_buf_pos, remain); \
            mp3_buf_len = remain; \
            mp3_buf_pos = 0; \
        } \
        size_t space = MP3_BUF_SIZE - mp3_buf_len; \
        size_t avail = mp3_flash_size - mp3_flash_pos; \
        size_t rd = (space < avail) ? space : avail; \
        if (rd > 0) { \
            esp_partition_read(mp3_partition, 4 + mp3_flash_pos, mp3_buf + mp3_buf_len, rd); \
            mp3_buf_len += rd; \
            mp3_flash_pos += rd; \
        } \
    } while(0)

    /* Helper: get next MP3 decoded frame */
    #define MP3_DECODE_NEXT() do { \
        mp3dec_frame_info_t info; \
        int samples = 0; \
        while (samples == 0) { \
            /* If buffer is running low, refill */ \
            if (mp3_buf_len - mp3_buf_pos < 4096) { \
                MP3_BUF_REFILL(); \
            } \
            /* Check if we've exhausted all data */ \
            if (mp3_buf_pos >= mp3_buf_len) { \
                /* Loop: restart from beginning */ \
                mp3_flash_pos = 0; \
                mp3_buf_pos = 0; \
                mp3_buf_len = 0; \
                MP3_BUF_REFILL(); \
                mp3dec_init(&mp3dec); \
                ESP_LOGI(TAG, "MP3 looping"); \
            } \
            int remain = (int)(mp3_buf_len - mp3_buf_pos); \
            samples = mp3dec_decode_frame(&mp3dec, mp3_buf + mp3_buf_pos, remain, mp3_pcm, &info); \
            mp3_buf_pos += info.frame_bytes; \
            if (info.frame_bytes == 0) { \
                mp3_buf_pos += 1; \
            } \
            if (samples > 0) { \
                mp3_channels = info.channels; \
                mp3_sample_rate = info.hz; \
            } \
        } \
        mp3_pcm_avail = samples; \
    } while(0)

    /* Prime the first MP3 frame */
    MP3_DECODE_NEXT();
    mp3_pcm_pos = 0;
    ESP_LOGI(TAG, "MP3: %d Hz, %d ch", mp3_sample_rate, mp3_channels);
    if (mp3_sample_rate != SAMPLE_RATE_HZ) {
        ESP_LOGW(TAG, "MP3 sample rate %d != %d, audio will be pitched!", mp3_sample_rate, SAMPLE_RATE_HZ);
    }

    /* Real-time pacing: we produce one LC3 frame every LC3_FRAME_US microseconds */
    int64_t next_frame_time = esp_timer_get_time() + LC3_FRAME_US;

    while (1) {
        /* ── Fill pcm_s24[SAMPLES_PER_FRAME * 2] from decoded MP3 ── */
        for (int out_i = 0; out_i < SAMPLES_PER_FRAME; out_i++) {
            /* Need more decoded samples? */
            if (mp3_pcm_pos >= mp3_pcm_avail) {
                MP3_DECODE_NEXT();
                mp3_pcm_pos = 0;
            }

            for (int ch = 0; ch < CHANNELS; ch++) {
                int16_t s;
                if (mp3_channels == 1) {
                    s = mp3_pcm[mp3_pcm_pos];
                } else {
                    s = mp3_pcm[mp3_pcm_pos * 2 + ch];
                }
                /* int16 → S24: shift left by 8 */
                pcm_s24[out_i * CHANNELS + ch] = (int32_t)s << 8;
            }
            mp3_pcm_pos++;
        }

        uint32_t t_us = (uint32_t)esp_timer_get_time();

        /* UDP still honours active-client gating to avoid pointless queueing.
         * ESP-NOW path always broadcasts regardless of join state — any sink on
         * the channel receives the stream with zero handshake, which is also a
         * prerequisite for tight inter-receiver sync. */
        bool have_udp = false;
        for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
            if (udp_clients[i].in_use) { have_udp = true; break; }
        }

        audio_msg_t m = {0};
        m.h.magic = PROTO_MAGIC;
        m.h.type = MSG_AUDIO;
        m.h.room_code = ROOM_CODE;
        m.seq = seq_num++;
        m.payload_len = LC3_BYTES_PER_CH * CHANNELS;
        m.src_t_us = stream_id;
        m.flags = 0;
        m.capture_us = t_us - LC3_FRAME_US;

        if (lc3_encode_stereo_s24(pcm_s24, m.payload)) {
            /* ESP-NOW broadcast path: always enqueue */
            if (audio_q) {
                if (xQueueSend(audio_q, &m, 0) != pdTRUE) {
                    audio_msg_t drop;
                    (void)xQueueReceive(audio_q, &drop, 0);
                    (void)xQueueSend(audio_q, &m, 0);
                }
            }
            /* UDP path: only when a client is registered */
            if (udp_audio_q && have_udp) {
                if (xQueueSend(udp_audio_q, &m, 0) != pdTRUE) {
                    audio_msg_t drop;
                    (void)xQueueReceive(udp_audio_q, &drop, 0);
                    (void)xQueueSend(udp_audio_q, &m, 0);
                }
            }
        }

        /* ── Real-time pacing ──────────────────────────────────────────── */
        int64_t now = esp_timer_get_time();
        int64_t wait_us = next_frame_time - now;
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        }
        next_frame_time += LC3_FRAME_US;
        /* If we fell behind, reset to avoid burst */
        if (next_frame_time < esp_timer_get_time() - LC3_FRAME_US * 4) {
            next_frame_time = esp_timer_get_time() + LC3_FRAME_US;
        }
    }
}

void audio_send_task(void *arg) {
    (void)arg;

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
         *    well inside the 7.5 ms budget.
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
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
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
