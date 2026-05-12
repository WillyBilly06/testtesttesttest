// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "../core/sink_app_internal.h"

static void lc3_init_decoder(void) {
    for (int ch = 0; ch < CHANNELS; ch++) {
        lc3_dec[ch] = lc3_setup_decoder(LC3_FRAME_US, SAMPLE_RATE_HZ, 0, &lc3_dec_mem[ch]);
        if (!lc3_dec[ch]) {
            ESP_LOGE(TAG, "lc3_setup_decoder failed ch=%d", ch);
            abort();
        }
    }
    ESP_LOGI(TAG, "LC3 decoder ready (%dus, %dHz, %dB/ch)", LC3_FRAME_US, SAMPLE_RATE_HZ, LC3_BYTES_PER_CH);
}

static bool lc3_decode_stereo(const uint8_t *payload, int32_t *pcm_s24_interleaved) {
    for (int ch = 0; ch < CHANNELS; ch++) {
        const uint8_t *in = payload + ch * LC3_BYTES_PER_CH;
        int rc = lc3_decode(lc3_dec[ch], in, LC3_BYTES_PER_CH,
                            LC3_PCM_FORMAT_S24, pcm_s24_interleaved + ch, CHANNELS);
        if (rc < 0) return false;
    }
    return true;
}

static bool lc3_plc_stereo(int32_t *pcm_s24_interleaved) {
    for (int ch = 0; ch < CHANNELS; ch++) {
        int rc = lc3_decode(lc3_dec[ch], NULL, 0,
                            LC3_PCM_FORMAT_S24, pcm_s24_interleaved + ch, CHANNELS);
        if (rc < 0) return false;
    }
    return true;
}

static inline int32_t sat24(int32_t v) {
    if (v >  8388607) return  8388607;
    if (v < -8388608) return -8388608;
    return v;
}

static void wait_audio_phase_delay(int32_t delay_us) {
    if (delay_us <= 0) return;
    if (delay_us > PHASE_DELAY_MAX_US) delay_us = PHASE_DELAY_MAX_US;

    while (delay_us > 2000) {
        int32_t sleep_us = delay_us - 1000;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)));
        delay_us = delay_us - sleep_us;
    }
    if (delay_us > 0) {
        esp_rom_delay_us((uint32_t)delay_us);
    }
}

static void pcm24_to_i2s32(const int32_t *pcm_s24, int32_t *i2s_out) {
    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
        int base = i * CHANNELS;
        i2s_out[base + 0] = sat24(pcm_s24[base + 1]) << 8;
        i2s_out[base + 1] = sat24(pcm_s24[base + 0]) << 8;
    }
}

static void i2s_init_tx(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = 120;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE_HZ,
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    ESP_LOGI(TAG, "I2S TX ready (PCM5102A) @ %d Hz", SAMPLE_RATE_HZ);
}

void playback_task(void *arg) {
    (void)arg;

    lc3_init_decoder();
    i2s_init_tx();

    static int32_t pcm_s24[SAMPLES_PER_FRAME * CHANNELS];
    static int32_t i2s_out[SAMPLES_PER_FRAME * CHANNELS];

    bool started = false;
    uint16_t expected_seq = 0;
    bool expected_valid = false;

    bool have_held = false;
    audio_msg_t held = {0};
    int held_len = 0;

    uint32_t last_reset_log_us = 0;
    uint32_t consecutive_plc = 0;
    bool i2s_tx_enabled = true;
    #define DRIFT_RESYNC_THRESHOLD  30

    while (1) {
        if (!selected) {
            if (i2s_tx_enabled) {
                i2s_channel_disable(i2s_tx);
                i2s_tx_enabled = false;
                ESP_LOGW(TAG, "Room inactive: I2S disabled and audio buffers cleared");
            }
            started = false;
            expected_valid = false;
            have_held = false;
            consecutive_plc = 0;
            latency_raw_us = 0;
            latency_smooth_us = 0;
            playout_err_us = 0;
            capture_clock_offset_us = 0;
            clock_sync_count = 0;
            selected_is_ecast = false;
            selected_pres_delay_us = ECAST_PRES_DELAY_US;
            memset(&held, 0, sizeof(held));
            memset(pcm_s24, 0, sizeof(pcm_s24));
            memset(i2s_out, 0, sizeof(i2s_out));
            flush_full_queue();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (request_stream_reset) {
            request_stream_reset = false;
            request_rejoin = true;

            flush_full_queue();
            lc3_init_decoder();
            started = false;
            expected_valid = false;
            have_held = false;
            latency_raw_us = 0;
            latency_smooth_us = 0;
            playout_err_us = 0;
            capture_clock_offset_us = 0;
            clock_sync_count = 0;

            uint32_t now = (uint32_t)esp_timer_get_time();
            if (now - last_reset_log_us > 200000) {
                last_reset_log_us = now;
                ESP_LOGW(TAG, "Stream reset -> decoder/buffer reset, rejoin requested (stream=0x%08" PRIX32 ")", sel_stream_id);
            }
        }

        if (!started) {
            if (uxQueueMessagesWaiting(full_q) < PREBUFFER_FRAMES) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            if (!i2s_tx_enabled) {
                ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
                i2s_tx_enabled = true;
            }
            started = true;
            ESP_LOGI(TAG, "Playback start (prebuffer=%d)", PREBUFFER_FRAMES);
        }

        /* The receive callback bounds latency by dropping the oldest queued
         * frame only when the queue is completely full. Playback consumes in
         * order here so normal jitter is absorbed instead of churned. */

        bool have_pkt = false;
        audio_msg_t m = {0};
        int mlen = 0;

        if (have_held) {
            m = held;
            mlen = held_len;
            have_held = false;
            have_pkt = true;
        } else {
            uint8_t idx;
            if (xQueueReceive(full_q, &idx, pdMS_TO_TICKS(adaptive_packet_wait_ms)) == pdTRUE) {
                m = rx_slots[idx].msg;
                mlen = rx_slots[idx].len;
                xQueueSend(free_q, &idx, 0);
                have_pkt = true;
            }
        }

        bool ok = false;

        bool pkt_ecast = have_pkt && ((m.flags & AUDIO_FLAG_ECAST) != 0);

        if (have_pkt && m.h.magic == PROTO_MAGIC && m.h.type == MSG_AUDIO && m.h.room_code == sel_room) {
            uint32_t pkt_stream_id = m.src_t_us;
            if (pkt_stream_id != sel_stream_id) {
                sel_stream_id = pkt_stream_id;
                request_stream_reset = true;
                request_rejoin = true;
                ok = lc3_plc_stereo(pcm_s24);
            } else {
                uint16_t seq = m.seq;
                last_seq_rx = seq;

                if (!expected_valid) {
                    expected_seq = seq;
                    expected_valid = true;
                }

                uint16_t diff = (uint16_t)(seq - expected_seq);
                if (diff == 0) {
                    if (m.payload_len == LC3_BYTES_PER_CH * CHANNELS &&
                        mlen >= (int)(offsetof(audio_msg_t, payload) + m.payload_len)) {
                        ok = lc3_decode_stereo(m.payload, pcm_s24);
                    }
                    expected_seq++;

                } else if (diff < 0x8000) {
                    if (diff <= 2) {
                        uint8_t qfill_now = (uint8_t)uxQueueMessagesWaiting(full_q);
                        bool late_recover =
                            (playout_err_us > (LC3_FRAME_US + 2000)) ||
                            (qfill_now > (uint8_t)(adaptive_target_fill + 1));

                        if (late_recover &&
                            m.payload_len == LC3_BYTES_PER_CH * CHANNELS &&
                            mlen >= (int)(offsetof(audio_msg_t, payload) + m.payload_len)) {
                            ok = lc3_decode_stereo(m.payload, pcm_s24);
                            expected_seq = seq + 1;
                        } else {
                            held = m;
                            held_len = mlen;
                            have_held = true;
                            ok = lc3_plc_stereo(pcm_s24);
                            expected_seq++;
                        }
                    } else {
                        ESP_LOGW(TAG, "Seq jump: expected=%u got=%u (gap=%u), skipping forward",
                                 expected_seq, seq, diff);
                        if (m.payload_len == LC3_BYTES_PER_CH * CHANNELS &&
                            mlen >= (int)(offsetof(audio_msg_t, payload) + m.payload_len)) {
                            lc3_init_decoder();
                            ok = lc3_decode_stereo(m.payload, pcm_s24);
                        }
                        expected_seq = seq + 1;
                    }
                } else {
                    if (consecutive_plc < PLC_MAX_CONSECUTIVE) {
                        ok = lc3_plc_stereo(pcm_s24);
                    } else {
                        ok = false;
                    }
                }
            }
        } else {
            if (consecutive_plc < PLC_MAX_CONSECUTIVE) {
                ok = lc3_plc_stereo(pcm_s24);
            } else {
                ok = false;
            }
        }

        if (ok && have_pkt) {
            stat_decoded++;
            consecutive_plc = 0;
        } else {
            stat_plc++;
            consecutive_plc++;

            if (consecutive_plc >= DRIFT_RESYNC_THRESHOLD && started) {
                ESP_LOGW(TAG, "PLC spiral (%lu frames), re-syncing playback",
                         (unsigned long)consecutive_plc);
                flush_full_queue();
                lc3_init_decoder();
                started = false;
                expected_valid = false;
                have_held = false;
                consecutive_plc = 0;
                latency_raw_us = 0;
                latency_smooth_us = 0;
                playout_err_us = 0;
                capture_clock_offset_us = 0;
                clock_sync_count = 0;
                continue;
            }
        }

        if (!ok || !have_pkt) {
            if (!ok) memset(pcm_s24, 0, sizeof(pcm_s24));
        }

        if (ok && have_pkt) {
            int32_t now_us = (int32_t)esp_timer_get_time();
            int32_t target_delay_us = pkt_ecast
                ? (int32_t)selected_pres_delay_us
                : (int32_t)adaptive_target_fill * LC3_FRAME_US;
            int32_t due_us = (int32_t)m.capture_us + clock_offset_us + target_delay_us;
            int32_t err_us = now_us - due_us;

            if (playout_err_us == 0) {
                playout_err_us = err_us;
            } else {
                playout_err_us += (err_us - playout_err_us) / 8;
            }

            int32_t pkt_clock_offset_us = pkt_ecast ? clock_offset_us : capture_clock_offset_us;
            int32_t lat_now_us = now_us - ((int32_t)m.capture_us + pkt_clock_offset_us);
            if (lat_now_us < 0) lat_now_us = 0;
            if (lat_now_us > 500000) lat_now_us = 500000;
            latency_raw_us = lat_now_us;
            if (latency_smooth_us == 0) {
                latency_smooth_us = lat_now_us;
            } else {
                latency_smooth_us += (lat_now_us - latency_smooth_us) / 8;
            }

            if (err_us < -PHASE_DELAY_MIN_US) {
                int32_t delay_us = -err_us;
                wait_audio_phase_delay(delay_us);
            }

            if (iabs32(err_us) >= BURST_PHASE_ERR_US) {
                burst_guard_s = BURST_GUARD_SECONDS;
            }
        }

        pcm24_to_i2s32(pcm_s24, i2s_out);

        size_t written = 0;
        (void)i2s_channel_write(i2s_tx, i2s_out, I2S_FRAME_BYTES, &written, portMAX_DELAY);
    }
}

void stats_task(void *arg) {
    (void)arg;
    uint32_t prev_dec = 0, prev_plc = 0, prev_drop = 0;
    uint8_t stuck_high_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t d = stat_decoded, p = stat_plc, dr = stat_dropped;
        uint32_t dd = d - prev_dec, dp = p - prev_plc, ddr = dr - prev_drop;
        prev_dec = d; prev_plc = p; prev_drop = dr;
        uint8_t qfill = (uint8_t)uxQueueMessagesWaiting(full_q);

        uint8_t new_target = adaptive_target_fill;
        uint8_t new_wait = adaptive_packet_wait_ms;
        int32_t phase_abs_us = iabs32(playout_err_us);

        bool burst_mode = (burst_guard_s > 0) ||
                  (rx_jitter_us >= BURST_JITTER_US);

        if (burst_mode) {
            new_target = TARGET_FILL_MAX;
            new_wait = PACKET_WAIT_MS_MAX;
            if (burst_guard_s > 0) burst_guard_s--;
        }

        if (!burst_mode && dp >= 4) {
            if (new_target < TARGET_FILL_MAX) new_target++;
            if (new_wait < PACKET_WAIT_MS_MAX) new_wait += 2;
        } else if (!burst_mode && dp == 0 && qfill >= (uint8_t)(new_target + 1)) {
            if (new_target > TARGET_FILL_MIN) new_target--;
            if (new_wait > PACKET_WAIT_MS_MIN) new_wait--;
        }

        if (playout_err_us > 8000 && qfill > (uint8_t)(TARGET_FILL_MIN + 1)) {
            new_target = TARGET_FILL_MIN;
            new_wait = PACKET_WAIT_MS_MIN;
            int32_t corr = (playout_err_us / 2) + 2000;
            if (corr > 9000) corr = 9000;
            clock_offset_us += corr;
        }

        if (!burst_mode && dp > 0 && playout_err_us > (LC3_FRAME_US / 2)) {
            int32_t fast_corr = (int32_t)dp * LC3_FRAME_US;
            if (fast_corr > (LC3_FRAME_US + 4000)) fast_corr = (LC3_FRAME_US + 4000);
            if (fast_corr > playout_err_us) fast_corr = playout_err_us;
            clock_offset_us += fast_corr;
        }

        if (qfill > new_target && playout_err_us > 2000) {
            int32_t backlog = (int32_t)qfill - (int32_t)new_target;
            int32_t corr = (playout_err_us / 3) + (backlog * 1800);
            if (corr > 9000) corr = 9000;
            clock_offset_us += corr;
        }

        if (playout_err_us > 22000 && qfill >= (uint8_t)(TARGET_FILL_MIN + 3) && dp == 0) {
            if (stuck_high_s < 255) stuck_high_s++;
        } else {
            stuck_high_s = 0;
        }

        if (stuck_high_s >= 3) {
            request_stream_reset = true;
            request_rejoin = true;
            request_rejoin_at_us = (uint32_t)esp_timer_get_time();
            stuck_high_s = 0;
            ESP_LOGW(TAG, "Latency stuck high: forcing stream re-sync");
        }

        if (!burst_mode && dp == 0 && ddr == 0) {
            if (playout_err_us > 3000) {
                int32_t corr = playout_err_us / 6;
                if (corr > 1500) corr = 1500;
                clock_offset_us += corr;
            } else if (playout_err_us < -3000) {
                int32_t corr = playout_err_us / 10;
                if (corr < -1000) corr = -1000;
                clock_offset_us += corr;
            }

            if (qfill <= new_target && new_target > TARGET_FILL_MIN) {
                new_target--;
            }
            if (qfill <= new_target && new_wait > PACKET_WAIT_MS_MIN) {
                new_wait--;
            }
        }

        adaptive_target_fill = new_target;
        adaptive_packet_wait_ms = new_wait;

        int32_t lat_raw_ms  = latency_raw_us / 1000;
        int32_t lat_avg_ms  = latency_smooth_us / 1000;
        int32_t clk_off_ms  = clock_offset_us / 1000;
        int32_t phase_err_ms = playout_err_us / 1000;
        int32_t rx_jit_ms = rx_jitter_us / 1000;

        ESP_LOGI(TAG, "lat=%ldms avg=%ldms clk=%ldms ph=%ldms rj=%ldms bg=%u | q=%u tgt=%u wait=%ums dec=%lu plc=%lu drop=%lu",
            (long)lat_raw_ms, (long)lat_avg_ms, (long)clk_off_ms, (long)phase_err_ms, (long)rx_jit_ms, (unsigned)burst_guard_s,
                 qfill,
                 (unsigned)adaptive_target_fill,
                 (unsigned)adaptive_packet_wait_ms,
                 (unsigned long)dd, (unsigned long)dp, (unsigned long)ddr);
    }
}
