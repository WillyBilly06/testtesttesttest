// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "../core/source_app_internal.h"
#include "../ecast_source.h"

static void sta_reconnect_timer_cb(TimerHandle_t tmr) {
    (void)tmr;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect attempt failed to start: %d", err);
    }
}

/* Scan all 2.4 GHz channels (1-13) and pick the least-congested non-overlapping
 * channel (1, 6, or 11). Weights each channel by:
 *   - count of APs seen on that channel
 *   - RSSI-weighted "busyness" (stronger APs = more interference)
 *
 * Only non-overlapping channels are considered because ESP-NOW at 24 Mbps uses
 * 20 MHz bandwidth which overlaps ±2 adjacent channels. Choosing ch3 with a
 * loud ch1 AP nearby is worse than just using ch1.
 *
 * Must be called AFTER esp_wifi_start() (scan needs the radio active). Takes
 * ~1 second (100 ms active scan × 13 channels minus early termination).
 * Returns 1, 6, or 11. Falls back to WIFI_CHANNEL_DEFAULT on failure. */
static uint8_t pick_best_channel(void) {
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0,            /* 0 = all channels */
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 100,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);   /* block until done */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel scan failed: %s", esp_err_to_name(err));
        return WIFI_CHANNEL_DEFAULT;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGI(TAG, "Channel scan: empty spectrum, defaulting to ch 1");
        return 1;
    }

    wifi_ap_record_t *aps = (wifi_ap_record_t *)heap_caps_malloc(
        (size_t)n * sizeof(wifi_ap_record_t), MALLOC_CAP_DEFAULT);
    if (!aps) {
        ESP_LOGW(TAG, "Channel scan: out of memory");
        return WIFI_CHANNEL_DEFAULT;
    }

    esp_wifi_scan_get_ap_records(&n, aps);

    /* Per-channel badness score. rssi_weight adds more for stronger signals. */
    uint16_t ap_count[14]   = {0};   /* idx 1..13 valid */
    int32_t  badness[14]    = {0};

    for (uint16_t i = 0; i < n; i++) {
        uint8_t ch = aps[i].primary;
        if (ch < 1 || ch > 13) continue;
        ap_count[ch]++;
        /* rssi ranges roughly -100 (weak) to -20 (loud). Convert to 0..80. */
        int32_t loudness = (int32_t)aps[i].rssi + 100;   /* 0..80 */
        if (loudness < 0) loudness = 0;
        badness[ch] += loudness;
    }
    free(aps);

    /* Score each non-overlapping channel by itself + its neighbours within
     * ±2 (adjacent channels cause co-channel interference). */
    const uint8_t candidates[3] = {1, 6, 11};
    uint8_t best = WIFI_CHANNEL_DEFAULT;
    int32_t best_score = INT32_MAX;

    for (int i = 0; i < 3; i++) {
        uint8_t c = candidates[i];
        int32_t score = 0;
        for (int8_t off = -2; off <= 2; off++) {
            int8_t nc = (int8_t)c + off;
            if (nc < 1 || nc > 13) continue;
            /* Adjacent channel contributes less (weight by distance). */
            uint8_t w = (off == 0) ? 4 : ((off == -1 || off == 1) ? 2 : 1);
            score += (int32_t)ap_count[nc] * w * 4;
            score += badness[nc] * w;
        }
        ESP_LOGI(TAG, "Channel-scan: ch%02u score=%ld (%u APs on-channel)",
                 (unsigned)c, (long)score, (unsigned)ap_count[c]);
        if (score < best_score) {
            best_score = score;
            best = c;
        }
    }

    ESP_LOGI(TAG, "Channel-scan: picked ch %u (best of 1/6/11)", (unsigned)best);
    return best;
}

static void schedule_sta_reconnect(uint32_t delay_ms) {
    if (!sta_reconnect_timer) {
        return;
    }
    TickType_t ticks = pdMS_TO_TICKS(delay_ms == 0 ? 1 : delay_ms);
    xTimerStop(sta_reconnect_timer, 0);
    xTimerChangePeriod(sta_reconnect_timer, ticks, 0);
    xTimerStart(sta_reconnect_timer, 0);
}

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len) {
    size_t di = 0;
    for (size_t si = 0; si < src_len && di + 1 < dst_len; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
            continue;
        }
        if (c == '%' && (si + 2) < src_len) {
            char h1 = src[si + 1];
            char h2 = src[si + 2];
            int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') :
                     (h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) :
                     (h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) : -1;
            int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') :
                     (h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) :
                     (h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) : -1;
            if (v1 >= 0 && v2 >= 0) {
                dst[di++] = (char)((v1 << 4) | v2);
                si += 2;
                continue;
            }
        }
        dst[di++] = c;
    }
    dst[di] = 0;
}

static bool get_form_field(const char *body, const char *key, char *out, size_t out_len) {
    char needle[24];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t enc_len = end ? (size_t)(end - p) : strlen(p);
    url_decode(out, out_len, p, enc_len);
    return true;
}

static bool load_wifi_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_CRED_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    esp_err_t e1 = nvs_get_str(nvs, WIFI_CRED_KEY_SSID, ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(nvs, WIFI_CRED_KEY_PASS, pass, &pass_len);
    nvs_close(nvs);
    return (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != 0);
}

static bool save_wifi_credentials(const char *ssid, const char *pass) {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_CRED_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    if (nvs_set_str(nvs, WIFI_CRED_KEY_SSID, ssid) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    if (nvs_set_str(nvs, WIFI_CRED_KEY_PASS, pass) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    bool ok = (nvs_commit(nvs) == ESP_OK);
    nvs_close(nvs);
    return ok;
}

static void log_wifi_mac(wifi_interface_t ifx, const char *label) {
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(ifx, mac) == ESP_OK) {
        ESP_LOGI(TAG, "%s MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 label,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void ensure_napt_enabled(void) {
#if CONFIG_LWIP_IPV4_NAPT
    if (napt_enabled || !wifi_ap_netif) return;
    esp_err_t err = esp_netif_napt_enable(wifi_ap_netif);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        napt_enabled = true;
        ESP_LOGI(TAG, "NAPT enabled on SoftAP");
    } else {
        ESP_LOGW(TAG, "NAPT enable failed: %d", err);
    }
#else
    ESP_LOGW(TAG, "NAPT disabled in sdkconfig (CONFIG_LWIP_IPV4_NAPT=n)");
#endif
}

static void refresh_softap_dns_from_sta(void) {
    if (!wifi_sta_netif || !wifi_ap_netif) return;

    esp_netif_dns_info_t dns_main = {0};
    if (esp_netif_get_dns_info(wifi_sta_netif, ESP_NETIF_DNS_MAIN, &dns_main) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read STA DNS");
        return;
    }

    if (!IP_IS_V4(&dns_main.ip) || ip_2_ip4(&dns_main.ip)->addr == 0) {
        ESP_LOGW(TAG, "STA DNS is empty, skipping SoftAP DNS update");
        return;
    }

    esp_err_t err = esp_netif_set_dns_info(wifi_ap_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP DNS set failed: %d", err);
        return;
    }

    ESP_LOGI(TAG, "SoftAP DNS set to " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));

    err = esp_netif_dhcps_stop(wifi_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "SoftAP DHCP stop failed: %d", err);
    }
    err = esp_netif_dhcps_start(wifi_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "SoftAP DHCP start failed: %d", err);
    } else {
        ESP_LOGI(TAG, "SoftAP DHCP restarted to refresh DNS lease options");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "AP client joined: %02X:%02X:%02X:%02X:%02X:%02X aid=%d",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "AP client left: %02X:%02X:%02X:%02X:%02X:%02X aid=%d",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        sta_reconnect_backoff_ms = WIFI_RECONNECT_BASE_MS;
        schedule_sta_reconnect(0);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)event_data;
        sta_disconnect_count++;
        sta_last_drop_us = (uint32_t)esp_timer_get_time();
        ESP_LOGW(TAG, "STA disconnected (reason=%u)", (unsigned)e->reason);

        /* While streaming, suppress STA reconnect entirely.
         * esp_wifi_connect() triggers an internal scan that hops channels and
         * causes massive ESP-NOW packet loss (the root cause of audio drops).
         *
         * Previously this required count_active_sinks() > 0 as an extra guard,
         * but that failed whenever sinks[] was stale (fresh stream_id after
         * source reflash, brief RF hiccup preventing keepalive, etc.), putting
         * us in a self-reinforcing reconnect storm that produced 500-4000 ms
         * audio dropouts. Streaming > chasing an unreachable upstream AP. */
        if (streaming_mode_active) {
            ESP_LOGW(TAG, "STA reconnect SUPPRESSED (streaming mode, sinks=%d)",
                     count_active_sinks());
            return;
        }

        uint32_t reconnect_delay_ms = 200;
        if (streaming_mode_active) {
            reconnect_delay_ms = sta_reconnect_backoff_ms;
            if (sta_reconnect_backoff_ms < WIFI_RECONNECT_MAX_MS) {
                uint32_t next = sta_reconnect_backoff_ms << 1;
                sta_reconnect_backoff_ms = (next > WIFI_RECONNECT_MAX_MS) ? WIFI_RECONNECT_MAX_MS : next;
            }
        }

        if (wifi_retry < WIFI_MAX_RETRY) {
            wifi_retry++;
            schedule_sta_reconnect(reconnect_delay_ms);
            ESP_LOGW(TAG, "STA reconnect scheduled in %ums (retry=%d)",
                     (unsigned)reconnect_delay_ms, wifi_retry);
        } else {
            wifi_retry = 0;
            ESP_LOGW(TAG, "STA reconnect limit reached, retry loop continues");
            schedule_sta_reconnect(WIFI_RECONNECT_MAX_MS);
            xEventGroupSetBits(wifi_ev, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        wifi_retry = 0;
        sta_reconnect_backoff_ms = WIFI_RECONNECT_BASE_MS;
        if (sta_reconnect_timer) {
            xTimerStop(sta_reconnect_timer, 0);
        }
        sta_reconnect_count++;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&evt->ip_info.ip));

        if (sta_last_drop_us != 0) {
            uint32_t now_us = (uint32_t)esp_timer_get_time();
            sta_last_recover_ms = (now_us - sta_last_drop_us) / 1000U;
            ESP_LOGW(TAG, "STA recovered in %ums", (unsigned)sta_last_recover_ms);
            sta_last_drop_us = 0;
        }

        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        uint8_t primary = WIFI_CHANNEL_DEFAULT;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
            wifi_channel = primary;
            ESP_LOGI(TAG, "APSTA channel synced to %u", (unsigned)wifi_channel);
            ecast_source_set_channel(wifi_channel);
        }

        refresh_softap_dns_from_sta();
        ensure_napt_enabled();
        xEventGroupSetBits(wifi_ev, WIFI_CONNECTED_BIT);
    }
}

void wifi_start_apsta(const char *ssid, const char *password) {
    napt_enabled = false;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", err);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", err);
        return;
    }

    wifi_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (!wifi_sta_netif || !wifi_ap_netif) {
        ESP_LOGE(TAG, "Failed to create APSTA netifs");
        return;
    }

    wifi_ev = xEventGroupCreate();
    if (!wifi_ev) return;

    if (!sta_reconnect_timer) {
        sta_reconnect_timer = xTimerCreate("sta_reconn",
                                           pdMS_TO_TICKS(WIFI_RECONNECT_BASE_MS),
                                           pdFALSE,
                                           NULL,
                                           sta_reconnect_timer_cb);
        if (!sta_reconnect_timer) {
            ESP_LOGE(TAG, "Failed to create STA reconnect timer");
            return;
        }
    }

    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_t got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, CONFIG_SOFTAP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, CONFIG_SOFTAP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(CONFIG_SOFTAP_SSID);
    ap_cfg.ap.channel = WIFI_CHANNEL_DEFAULT;
    ap_cfg.ap.max_connection = 8;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode = strlen(CONFIG_SOFTAP_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    esp_err_t dhcp_err = esp_netif_dhcps_stop(wifi_ap_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "SoftAP DHCP stop failed: %d", dhcp_err);
    }

    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, SOURCE_AP_IP_A, SOURCE_AP_IP_B, SOURCE_AP_IP_C, SOURCE_AP_IP_D);
    IP4_ADDR(&ap_ip.gw, SOURCE_AP_IP_A, SOURCE_AP_IP_B, SOURCE_AP_IP_C, SOURCE_AP_IP_D);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(wifi_ap_netif, &ap_ip));

    dhcp_err = esp_netif_dhcps_start(wifi_ap_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "SoftAP DHCP start failed: %d", dhcp_err);
    }
    ESP_LOGI(TAG, "SoftAP subnet: %d.%d.%d.%d/24",
             SOURCE_AP_IP_A, SOURCE_AP_IP_B, SOURCE_AP_IP_C, SOURCE_AP_IP_D);

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;   /* Fast scan: find AP quickly, less radio disruption */
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.listen_interval = 1;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    xEventGroupClearBits(wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));

    /* Auto channel selection: scan the band and move the AP (and therefore
     * ESP-NOW) to the quietest non-overlapping channel. Runs on every boot
     * because the RF environment changes throughout the day. */
    uint8_t best_ch = pick_best_channel();
    if (best_ch >= 1 && best_ch <= 13 && best_ch != WIFI_CHANNEL_DEFAULT) {
        /* Update AP config so future ap_start() cycles also land on the right
         * channel, and directly move the radio now. */
        ap_cfg.ap.channel = best_ch;
        esp_err_t ccfg = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ccfg != ESP_OK) {
            ESP_LOGW(TAG, "AP config rewrite failed: %s", esp_err_to_name(ccfg));
        }
        esp_err_t cset = esp_wifi_set_channel(best_ch, WIFI_SECOND_CHAN_NONE);
        if (cset == ESP_OK) {
            wifi_channel = best_ch;
            ecast_source_set_channel(wifi_channel);
            ESP_LOGI(TAG, "Auto-selected channel %u for ESP-NOW", (unsigned)best_ch);
        } else {
            ESP_LOGW(TAG, "esp_wifi_set_channel(%u) failed: %s (keeping default %u)",
                     (unsigned)best_ch, esp_err_to_name(cset),
                     (unsigned)WIFI_CHANNEL_DEFAULT);
        }
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_ev,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        uint8_t primary = WIFI_CHANNEL_DEFAULT;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
            wifi_channel = primary;
            ecast_source_set_channel(wifi_channel);
        }
        ensure_napt_enabled();
        ESP_LOGI(TAG, "APSTA ready: ap_ssid=%s ch=%u", CONFIG_SOFTAP_SSID, (unsigned)wifi_channel);
        log_wifi_mac(WIFI_IF_AP, "Transmitter AP");
        log_wifi_mac(WIFI_IF_STA, "Transmitter STA");
    } else {
        wifi_channel = WIFI_CHANNEL_DEFAULT;
        ESP_LOGW(TAG, "STA not connected yet. AP is still up (ssid=%s), continuing without upstream internet", CONFIG_SOFTAP_SSID);
        log_wifi_mac(WIFI_IF_AP, "Transmitter AP");
    }
}

static esp_err_t prov_root_get(httpd_req_t *req) {
    const char *html =
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>OpenALS Wi-Fi Setup</title></head><body>"
        "<h2>Connect OpenALS Source</h2>"
        "<form method='POST' action='/save'>"
        "SSID:<br><input name='ssid' maxlength='32'><br><br>"
        "Password:<br><input name='password' maxlength='64' type='password'><br><br>"
        "<button type='submit'>Save</button></form></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t prov_save_post(httpd_req_t *req) {
    char body[192] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!get_form_field(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    (void)get_form_field(body, "password", pass, sizeof(pass));

    if (ssid[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    if (pass[0] != 0 && strlen(pass) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password min 8 chars");
        return ESP_FAIL;
    }
    if (!save_wifi_credentials(ssid, pass)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    provision_done = true;
    httpd_resp_sendstr(req, "Saved. Device will reboot and connect.");
    return ESP_OK;
}

void start_provisioning_portal(void) {
    if (streaming_mode_active) {
        ESP_LOGW(TAG, "Provisioning request ignored while streaming is active");
        return;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", err);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", err);
        return;
    }
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, PROV_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, PROV_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(PROV_AP_SSID);
    ap_cfg.ap.channel = WIFI_CHANNEL_DEFAULT;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &server_cfg));

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = prov_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = prov_save_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);

    ESP_LOGW(TAG, "Provisioning AP started: SSID=%s, open http://192.168.4.1", PROV_AP_SSID);

    provision_done = false;
    while (!provision_done) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    httpd_stop(server);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void wifi_init_espnow(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));
    /* Use the Kconfig-selected rate. 6 Mbps is the default for max range
     * (~12 dB more link margin than 24 Mbps; required for clear audio
     * at 100 ft especially through walls). N=2 TX redundancy keeps
     * effective channel utilisation under 6% even at 6 Mbps. */
    espnow_rate_current = ESPNOW_PHY_RATE;

    esp_now_rate_config_t rcfg = {
        .phymode = ESPNOW_PHY_MODE,
        .rate = espnow_rate_current,
        .ersu = false,
        .dcm = false,
    };
    esp_now_set_peer_rate_config(BROADCAST_MAC, &rcfg);

    ESP_LOGI(TAG, "APSTA active on channel %d + ESP-NOW ready (rate=%s)",
             wifi_channel, espnow_rate_name(espnow_rate_current));
}

static void set_peer_rate(const uint8_t mac[6]) {
    esp_now_rate_config_t rcfg = {
        .phymode = ESPNOW_PHY_MODE,
        .rate = espnow_rate_current,
        .ersu = false,
        .dcm = false,
    };
    esp_now_set_peer_rate_config(mac, &rcfg);
}

void add_peer_if_needed(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_AP;
    p.encrypt = false;

    esp_err_t err = esp_now_add_peer(&p);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        set_peer_rate(mac);
    } else {
        ESP_LOGW(TAG, "esp_now_add_peer failed %d", err);
    }
}

void load_or_start_provisioning(char *sta_ssid, size_t ssid_len, char *sta_pass, size_t pass_len) {
    if (!load_wifi_credentials(sta_ssid, ssid_len, sta_pass, pass_len)) {
        ESP_LOGW(TAG, "No source Wi-Fi configured yet, starting setup portal");
        start_provisioning_portal();
    }
}
