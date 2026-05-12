// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "../core/sink_app_internal.h"
#include "../ecast_crypto.h"

static const uint8_t s_broadcast_code[16] = ECAST_BROADCAST_CODE_BYTES;
static bool s_ecast_psn_valid = false;
static uint32_t s_last_ecast_psn = 0;
static uint32_t s_prev_unique_audio_rx_us = 0;

static void format_room_code(uint32_t room_code, char *out, size_t out_len)
{
    uint16_t building = 0;
    uint16_t room = 0;
    uint8_t suffix = 0;
    decode_room_code(room_code, &building, &room, &suffix);

    if (suffix == 0) {
        snprintf(out, out_len, "%03u-%03u", building, room);
    } else if (suffix <= 26) {
        snprintf(out, out_len, "%03u-%03u%c", building, room, (char)('A' + suffix - 1));
    } else {
        snprintf(out, out_len, "%03u-%03u?", building, room);
    }
}

void wifi_init_espnow(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));
    {
        const uint8_t proto_bits = WIFI_PROTOCOL_11B
                                 | WIFI_PROTOCOL_11G
                                 | WIFI_PROTOCOL_11N;
        esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, proto_bits);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA set_protocol(11B|11G|11N) failed: %s", esp_err_to_name(err));
        }
        err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA set_bandwidth(HT20) failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "WiFi PHY: 11B|11G|11N HT20, modem-sleep=NONE, txpwr=84 qdBm");
    }

    ESP_ERROR_CHECK(esp_now_init());
}

static void set_peer_rate_mcs1(const uint8_t mac[6]) {
    esp_now_rate_config_t rcfg = {
        .phymode = WIFI_PHY_MODE_HT20,
        .rate = WIFI_PHY_RATE_MCS1_LGI,
        .ersu = false,
        .dcm = false,
    };
    esp_now_set_peer_rate_config(mac, &rcfg);
}

void add_peer_unicast(const uint8_t mac[6], uint8_t channel) {
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    (void)channel;
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;

    esp_err_t err = esp_now_add_peer(&p);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        set_peer_rate_mcs1(mac);
    }
}

void sync_selected_peer(void) {
    if (!selected) return;
    esp_wifi_set_channel(sel_channel, WIFI_SECOND_CHAN_NONE);
    if (selected_is_ecast) return;
    if (esp_now_is_peer_exist(sel_mac)) {
        esp_now_del_peer(sel_mac);
    }
    add_peer_unicast(sel_mac, sel_channel);
}

static void update_ecast_clock(uint32_t source_now_us, uint32_t local_now_us) {
    int32_t raw_offset = (int32_t)(local_now_us - source_now_us);
    if (clock_sync_count == 0) {
        clock_offset_us = raw_offset;
        capture_clock_offset_us = raw_offset;
    } else {
        int32_t err = raw_offset - clock_offset_us;
        int32_t corr = err / 16;
        if (corr > 1500) corr = 1500;
        if (corr < -1500) corr = -1500;
        clock_offset_us += corr;
        capture_clock_offset_us = clock_offset_us;
    }
    clock_sync_count++;
}

static void handle_ecast_beacon(const esp_now_recv_info_t *ri, const uint8_t *data, int len) {
    if (len < ECAST_BEACON_FRAME_SIZE) return;

    ecast_hdr_t h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != ECAST_MAGIC ||
        ECAST_VER_OF(h.ver_type) != ECAST_VERSION ||
        ECAST_TYPE_OF(h.ver_type) != ECAST_TYPE_BEACON ||
        h.payload_len != (uint16_t)(sizeof(ecast_beacon_payload_t) + ECAST_BEACON_TAG_LEN)) {
        return;
    }

    const uint8_t *body = data;
    size_t body_len = sizeof(ecast_hdr_t) + sizeof(ecast_beacon_payload_t);
    const uint8_t *tag = data + body_len;
    if (!ecast_beacon_verify(s_broadcast_code, body, body_len, tag)) return;

    ecast_beacon_payload_t b;
    memcpy(&b, data + sizeof(ecast_hdr_t), sizeof(b));
    if (b.channels != CHANNELS ||
        b.sample_rate_hz != SAMPLE_RATE_HZ ||
        b.frame_us != LC3_FRAME_US ||
        b.lc3_bytes_per_ch != LC3_BYTES_PER_CH) {
        return;
    }

    uint8_t session_key[16] = {0};
    bool key_valid = true;
    if (b.is_encrypted) {
        key_valid = ecast_derive_session_key(s_broadcast_code, b.key_diversifier, session_key);
        if (!key_valid) return;
    }

    uint32_t local_now_us = (uint32_t)esp_timer_get_time();
    int32_t beacon_clock_offset_us = (int32_t)(local_now_us - h.pres_time_us);
    int8_t rssi = ri->rx_ctrl ? ri->rx_ctrl->rssi : 0;
    add_or_update_ecast_room(ri->src_addr, &b, rssi, session_key, key_valid,
                             beacon_clock_offset_us);

    if (selected && selected_is_ecast && sel_stream_id == b.stream_id_full) {
        selected_pres_delay_us = b.pres_delay_us;
        update_ecast_clock(h.pres_time_us, local_now_us);
    }
}

static bool get_selected_ecast_crypto(uint16_t stream_id16, uint8_t enc_iv[8],
                                      uint8_t session_key[16], bool *encrypted,
                                      bool *key_valid, uint16_t *pres_delay_us) {
    if (!selected || !selected_is_ecast ||
        stream_id16 != (uint16_t)(sel_stream_id & 0xFFFFU)) {
        return false;
    }

    for (int i = 0; i < room_count; i++) {
        if (!rooms[i].active || !rooms[i].ecast) continue;
        if (rooms[i].stream_id != sel_stream_id) continue;
        if ((uint16_t)(rooms[i].stream_id & 0xFFFFU) != stream_id16) continue;
        *encrypted = rooms[i].is_encrypted;
        *key_valid = rooms[i].session_key_valid;
        *pres_delay_us = rooms[i].pres_delay_us ? rooms[i].pres_delay_us : ECAST_PRES_DELAY_US;
        memcpy(enc_iv, rooms[i].enc_iv, 8);
        memcpy(session_key, rooms[i].session_key, 16);
        return true;
    }
    return false;
}

static void handle_ecast_audio(const esp_now_recv_info_t *ri, const uint8_t *data, int len) {
    (void)ri;
    if (len < ECAST_AUDIO_FRAME_SIZE) return;

    ecast_hdr_t h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != ECAST_MAGIC ||
        ECAST_VER_OF(h.ver_type) != ECAST_VERSION ||
        ECAST_TYPE_OF(h.ver_type) != ECAST_TYPE_AUDIO ||
        h.payload_len != (uint16_t)(sizeof(ecast_audio_plain_t) + ECAST_MIC_LEN)) {
        return;
    }

    if (s_ecast_psn_valid) {
        int32_t psn_delta = (int32_t)(h.psn - s_last_ecast_psn);
        if (psn_delta <= 0) return;
    }

    uint8_t enc_iv[8] = {0};
    uint8_t session_key[16] = {0};
    bool encrypted = false;
    bool key_valid = false;
    uint16_t pres_delay_us = ECAST_PRES_DELAY_US;
    if (!get_selected_ecast_crypto(h.stream_id16, enc_iv, session_key,
                                   &encrypted, &key_valid, &pres_delay_us)) {
        return;
    }

    const uint8_t *cipher_and_mic = data + sizeof(ecast_hdr_t);
    size_t ct_and_mic_len = sizeof(ecast_audio_plain_t) + ECAST_MIC_LEN;
    ecast_audio_plain_t plain;

    if (encrypted) {
        if (!key_valid) return;
        uint8_t nonce[ECAST_NONCE_LEN];
        ecast_make_nonce(nonce, enc_iv, h.psn, h.copy_idx);
        if (!ecast_ccm_decrypt(session_key, nonce,
                               (const uint8_t *)&h, sizeof(h),
                               cipher_and_mic, ct_and_mic_len,
                               (uint8_t *)&plain)) {
            return;
        }
    } else {
        memcpy(&plain, cipher_and_mic, sizeof(plain));
    }

    s_last_ecast_psn = h.psn;
    s_ecast_psn_valid = true;

    audio_msg_t m = {0};
    m.h.magic = PROTO_MAGIC;
    m.h.type = MSG_AUDIO;
    m.h.room_code = sel_room;
    m.seq = (uint16_t)(h.psn & 0xFFFFU);
    m.payload_len = LC3_BYTES_PER_CH * CHANNELS;
    m.src_t_us = sel_stream_id;
    m.flags = AUDIO_FLAG_ECAST;
    m.capture_us = h.pres_time_us - pres_delay_us;
    memcpy(m.payload, plain.lc3, ECAST_AUDIO_BYTES);

    int mlen = (int)(offsetof(audio_msg_t, payload) + m.payload_len);
    if (!queue_rx_audio(&m, mlen)) return;

    uint32_t now_us = (uint32_t)esp_timer_get_time();
    last_audio_rx_us = now_us;

    if (s_prev_unique_audio_rx_us != 0) {
        int32_t ia_us = (int32_t)(now_us - s_prev_unique_audio_rx_us);
        int32_t dev_us = iabs32(ia_us - (int32_t)LC3_FRAME_US);
        if (rx_jitter_us == 0) {
            rx_jitter_us = dev_us;
        } else {
            rx_jitter_us += (dev_us - rx_jitter_us) / 8;
        }
        if (dev_us >= BURST_JITTER_US) {
            burst_guard_s = BURST_GUARD_SECONDS;
        }
    }
    s_prev_unique_audio_rx_us = now_us;
}

void espnow_recv_cb(const esp_now_recv_info_t *ri, const uint8_t *data, int len) {
    if (!ri || !data || len < (int)sizeof(msg_hdr_t)) return;

    if (data[0] == ECAST_MAGIC) {
        if (len >= (int)sizeof(ecast_hdr_t)) {
            uint8_t type = ECAST_TYPE_OF(data[1]);
            if (type == ECAST_TYPE_BEACON) {
                handle_ecast_beacon(ri, data, len);
            } else if (type == ECAST_TYPE_AUDIO) {
                handle_ecast_audio(ri, data, len);
            }
        }
        return;
    }

    msg_hdr_t h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != PROTO_MAGIC) return;

    if (h.type == MSG_BEACON && len >= (int)sizeof(beacon_msg_t)) {
        beacon_msg_t b;
        memcpy(&b, data, sizeof(b));
        if (b.h.magic != PROTO_MAGIC) return;
        if (b.channels != CHANNELS || b.sample_rate_hz != SAMPLE_RATE_HZ ||
            b.frame_us != LC3_FRAME_US || b.bytes_per_ch != LC3_BYTES_PER_CH) {
            return;
        }

        int8_t rssi = ri->rx_ctrl ? ri->rx_ctrl->rssi : 0;

        if (discovery_mode) {
            add_or_update_room(ri->src_addr, &b, rssi);
        } else if (selected &&
                   memcmp(ri->src_addr, sel_mac, 6) == 0 &&
                   b.h.room_code == sel_room) {
            if (sel_stream_id != b.stream_id) {
                sel_stream_id = b.stream_id;
                request_stream_reset = true;
                request_rejoin = true;
                request_rejoin_at_us = (uint32_t)esp_timer_get_time();
            }
        }
        return;
    }

    if (!selected) return;
    if (memcmp(ri->src_addr, sel_mac, 6) != 0) return;

    if (h.type != MSG_AUDIO) return;
    if (len < (int)offsetof(audio_msg_t, payload)) return;

    audio_msg_t am;
    memcpy(&am, data, len > (int)sizeof(am) ? (int)sizeof(am) : len);
    if (!queue_rx_audio(&am, len)) return;

    last_audio_rx_us = (uint32_t)esp_timer_get_time();

    static uint32_t prev_audio_rx_us = 0;
    if (prev_audio_rx_us != 0) {
        int32_t ia_us = (int32_t)(last_audio_rx_us - prev_audio_rx_us);
        int32_t dev_us = iabs32(ia_us - (int32_t)LC3_FRAME_US);
        if (rx_jitter_us == 0) {
            rx_jitter_us = dev_us;
        } else {
            rx_jitter_us += (dev_us - rx_jitter_us) / 8;
        }
        if (dev_us >= BURST_JITTER_US) {
            burst_guard_s = BURST_GUARD_SECONDS;
        }
    }
    prev_audio_rx_us = last_audio_rx_us;

    int32_t raw_ow = (int32_t)(last_audio_rx_us - am.capture_us);
    if (clock_sync_count == 0) {
        clock_offset_us = raw_ow;
        capture_clock_offset_us = raw_ow;
    } else {
        int32_t err = raw_ow - clock_offset_us;
        if (iabs32(playout_err_us) < 4000 && rx_jitter_us < 5000) {
            int32_t corr = err / 128;
            if (corr > 300) corr = 300;
            if (corr < -300) corr = -300;
            clock_offset_us += corr;
        }

        int32_t meas_err = raw_ow - capture_clock_offset_us;
        int32_t meas_corr = meas_err / 16;
        if (meas_corr > 2000) meas_corr = 2000;
        if (meas_corr < -2000) meas_corr = -2000;
        capture_clock_offset_us += meas_corr;
    }
    clock_sync_count++;
}

void scan_for_rooms(void) {
    memset(rooms, 0, sizeof(rooms));
    room_count = 0;

    discovery_mode = true;
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < SCAN_CHANNEL_COUNT; i++) {
            esp_wifi_set_channel(SCAN_CHANNELS[i], WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(SCAN_LISTEN_MS));
        }
    }
    discovery_mode = false;
}

void room_select_task(void *arg) {
    (void)arg;
    char input[32];

    uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);

    while (1) {
        selected = false;

        ESP_LOGI(TAG, "\nScanning for rooms...");
        scan_for_rooms();

        if (room_count == 0) {
            ESP_LOGW(TAG, "No rooms found. Retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Rooms found:");
        for (int i = 0; i < room_count; i++) {
            room_info_t *r = &rooms[i];
            char room_text[16];
            format_room_code(r->room_code, room_text, sizeof(room_text));
            ESP_LOGI(TAG, " [%d] room=%s%s ch=%d rssi=%d stream=0x%08" PRIX32 " %s %02X:%02X:%02X:%02X:%02X:%02X",
                     i+1, room_text, r->ecast ? " ECast" : "", r->channel, r->rssi, r->stream_id,
                     r->ecast ? r->name : "",
                     r->mac[0],r->mac[1],r->mac[2],r->mac[3],r->mac[4],r->mac[5]);
        }
        ESP_LOGI(TAG, "Enter room number (or 'r' to rescan):");

        while (1) {
            int n = uart_read_line(input, sizeof(input));
            if (n <= 0) continue;

            if (input[0] == 'r' || input[0] == 'R') break;

            int sel = atoi(input);
            if (sel < 1 || sel > room_count) {
                ESP_LOGW(TAG, "Invalid selection.");
                continue;
            }

            room_info_t *r = &rooms[sel - 1];
            memcpy(sel_mac, r->mac, 6);
            sel_room = r->room_code;
            sel_channel = r->channel;
            sel_stream_id = r->stream_id;
            selected_is_ecast = r->ecast;
            selected_pres_delay_us = (r->ecast && r->pres_delay_us)
                ? r->pres_delay_us
                : ECAST_PRES_DELAY_US;
            s_ecast_psn_valid = false;
            s_prev_unique_audio_rx_us = 0;
            if (r->ecast && r->clock_sync_count > 0) {
                clock_offset_us = r->clock_offset_us;
                capture_clock_offset_us = r->clock_offset_us;
                clock_sync_count = r->clock_sync_count;
            } else {
                clock_sync_count = 0;
            }

            esp_wifi_set_channel(sel_channel, WIFI_SECOND_CHAN_NONE);
            if (!selected_is_ecast) {
                add_peer_unicast(sel_mac, sel_channel);
            }

            join_msg_t jm = {0};
            jm.h.magic = PROTO_MAGIC;
            jm.h.type = MSG_JOIN;
            jm.h.room_code = sel_room;
            jm.stream_id = sel_stream_id;

            if (!selected_is_ecast) {
                esp_err_t jerr = esp_now_send(sel_mac, (const uint8_t *)&jm, sizeof(jm));
                if (jerr != ESP_OK) {
                    ESP_LOGW(TAG, "JOIN send failed (%d), retrying with peer re-sync", (int)jerr);
                    sync_selected_peer();
                    esp_now_send(sel_mac, (const uint8_t *)&jm, sizeof(jm));
                }
            }
            last_audio_rx_us = (uint32_t)esp_timer_get_time();

            selected = true;

            char room_text[16];
            format_room_code(sel_room, room_text, sizeof(room_text));
            ESP_LOGI(TAG, "\nSelected room=%s ch=%d stream=0x%08" PRIX32 " mode=%s target=%ums",
                     room_text, sel_channel, sel_stream_id,
                     selected_is_ecast ? "ECast" : "legacy",
                     (unsigned)(selected_pres_delay_us / 1000U));
            ESP_LOGI(TAG, "Press 'q' to leave room.");

            while (1) {
                if (!selected) {
                    break;
                }
                n = uart_read_line(input, sizeof(input));
                if (n > 0 && (input[0] == 'q' || input[0] == 'Q')) {
                    leave_msg_t lm = {0};
                    lm.h.magic = PROTO_MAGIC;
                    lm.h.type = MSG_LEAVE;
                    lm.h.room_code = sel_room;
                    lm.stream_id = sel_stream_id;
                    if (!selected_is_ecast) {
                        esp_now_send(sel_mac, (const uint8_t *)&lm, sizeof(lm));
                    }
                    selected_is_ecast = false;
                    selected = false;
                    break;
                }
            }
            break;
        }
    }
}

void keepalive_task(void *arg) {
    (void)arg;
    uint32_t last_rejoin_ms = 0;
    uint8_t timeout_streak = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_MS));
        if (!selected) continue;

        if (selected_is_ecast) {
            uint32_t now_us = (uint32_t)esp_timer_get_time();
            bool timed_out = (last_audio_rx_us != 0) && ((now_us - last_audio_rx_us) > AUDIO_RX_TIMEOUT_US);
            if (timed_out) {
                selected = false;
                selected_is_ecast = false;
                request_stream_reset = true;
                request_rejoin = false;
                ESP_LOGW(TAG, "ECast audio timeout: returning to room scan");
            }
            continue;
        }

        keepalive_msg_t ka = {0};
        ka.h.magic = PROTO_MAGIC;
        ka.h.type = MSG_KEEPALIVE;
        ka.h.room_code = sel_room;
        ka.stream_id = sel_stream_id;
        ka.last_seq_rx = last_seq_rx;
        esp_err_t ka_err = esp_now_send(sel_mac, (const uint8_t *)&ka, sizeof(ka));
        if (ka_err != ESP_OK) {
            ESP_LOGW(TAG, "KEEPALIVE send failed (%d), re-syncing peer/channel", (int)ka_err);
            sync_selected_peer();
            esp_now_send(sel_mac, (const uint8_t *)&ka, sizeof(ka));
        }

        uint32_t now_us = (uint32_t)esp_timer_get_time();
        bool timed_out = (last_audio_rx_us != 0) && ((now_us - last_audio_rx_us) > AUDIO_RX_TIMEOUT_US);

        if (timed_out) {
            timeout_streak++;
            request_rejoin = true;
            request_rejoin_at_us = now_us;

            ESP_LOGW(TAG, "Audio timeout: no RX (%u), trying peer/channel re-sync + rejoin",
                     (unsigned)timeout_streak);
            sync_selected_peer();

            join_msg_t rj = {0};
            rj.h.magic = PROTO_MAGIC;
            rj.h.type = MSG_JOIN;
            rj.h.room_code = sel_room;
            rj.stream_id = sel_stream_id;
            esp_now_send(sel_mac, (const uint8_t *)&rj, sizeof(rj));

            if (timeout_streak >= 6) {
                selected = false;
                request_rejoin = false;
                request_rejoin_at_us = 0;
                timeout_streak = 0;
                ESP_LOGW(TAG, "Audio timeout: room disconnected after retries");
            }
            continue;
        }

        timeout_streak = 0;

        uint32_t now_ms = now_us / 1000;
        if (request_rejoin && (now_ms - last_rejoin_ms) >= REJOIN_BACKOFF_MS) {
            join_msg_t jm = {0};
            jm.h.magic = PROTO_MAGIC;
            jm.h.type = MSG_JOIN;
            jm.h.room_code = sel_room;
            jm.stream_id = sel_stream_id;
            esp_err_t jerr = esp_now_send(sel_mac, (const uint8_t *)&jm, sizeof(jm));
            if (jerr != ESP_OK) {
                ESP_LOGW(TAG, "REJOIN send failed (%d), re-syncing peer/channel", (int)jerr);
                sync_selected_peer();
                esp_now_send(sel_mac, (const uint8_t *)&jm, sizeof(jm));
            }
            last_rejoin_ms = now_ms;
        }
    }
}
