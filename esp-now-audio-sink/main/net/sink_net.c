// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "../core/sink_app_internal.h"

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

    ESP_ERROR_CHECK(esp_now_init());
}

static void set_peer_rate_6m(const uint8_t mac[6]) {
    esp_now_rate_config_t rcfg = {
        .phymode = WIFI_PHY_MODE_11G,
        .rate = WIFI_PHY_RATE_6M,
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
        set_peer_rate_6m(mac);
    }
}

void sync_selected_peer(void) {
    if (!selected) return;
    esp_wifi_set_channel(sel_channel, WIFI_SECOND_CHAN_NONE);
    if (esp_now_is_peer_exist(sel_mac)) {
        esp_now_del_peer(sel_mac);
    }
    add_peer_unicast(sel_mac, sel_channel);
}

void espnow_recv_cb(const esp_now_recv_info_t *ri, const uint8_t *data, int len) {
    if (!ri || !data || len < (int)sizeof(msg_hdr_t)) return;

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

    uint8_t idx;
    if (xQueueReceive(free_q, &idx, 0) != pdTRUE) {
        return;
    }

    if (len > (int)sizeof(audio_msg_t)) len = sizeof(audio_msg_t);
    memcpy(&rx_slots[idx].msg, data, len);
    rx_slots[idx].len = len;

    xQueueSend(full_q, &idx, 0);
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

    {
        audio_msg_t am;
        memcpy(&am, data, len > (int)sizeof(am) ? (int)sizeof(am) : len);
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
            ESP_LOGI(TAG, " [%d] room=%s ch=%d rssi=%d stream=0x%08" PRIX32 "  %02X:%02X:%02X:%02X:%02X:%02X",
                     i+1, room_text, r->channel, r->rssi, r->stream_id,
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

            esp_wifi_set_channel(sel_channel, WIFI_SECOND_CHAN_NONE);
            add_peer_unicast(sel_mac, sel_channel);

            join_msg_t jm = {0};
            jm.h.magic = PROTO_MAGIC;
            jm.h.type = MSG_JOIN;
            jm.h.room_code = sel_room;
            jm.stream_id = sel_stream_id;

            esp_err_t jerr = esp_now_send(sel_mac, (const uint8_t *)&jm, sizeof(jm));
            if (jerr != ESP_OK) {
                ESP_LOGW(TAG, "JOIN send failed (%d), retrying with peer re-sync", (int)jerr);
                sync_selected_peer();
                esp_now_send(sel_mac, (const uint8_t *)&jm, sizeof(jm));
            }
            last_audio_rx_us = (uint32_t)esp_timer_get_time();

            selected = true;

            char room_text[16];
            format_room_code(sel_room, room_text, sizeof(room_text));
            ESP_LOGI(TAG, "\nSelected room=%s ch=%d stream=0x%08" PRIX32,
                     room_text, sel_channel, sel_stream_id);
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
                    esp_now_send(sel_mac, (const uint8_t *)&lm, sizeof(lm));
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
