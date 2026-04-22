// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "source_app_internal.h"
#include "../ecast_source.h"
#include "../ecast_protocol.h"

const char *TAG = "SRC";

lc3_encoder_mem_48k_t lc3_enc_mem[CHANNELS];
lc3_encoder_t lc3_enc[CHANNELS];
uint32_t stream_id = 0;
uint16_t seq_num = 0;
sink_peer_t sinks[MAX_SINKS];
QueueHandle_t audio_q;
QueueHandle_t udp_audio_q;
SemaphoreHandle_t tx_tokens;

const uint8_t BROADCAST_MAC[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
uint8_t wifi_channel = WIFI_CHANNEL_DEFAULT;

EventGroupHandle_t wifi_ev = NULL;
int wifi_retry = 0;
esp_netif_t *wifi_sta_netif = NULL;
esp_netif_t *wifi_ap_netif = NULL;
bool napt_enabled = false;
bool streaming_mode_active = false;
volatile uint32_t sta_disconnect_count = 0;
volatile uint32_t sta_reconnect_count = 0;
volatile uint32_t sta_last_drop_us = 0;
volatile uint32_t sta_last_recover_ms = 0;
TimerHandle_t sta_reconnect_timer = NULL;
uint32_t sta_reconnect_backoff_ms = WIFI_RECONNECT_BASE_MS;
wifi_phy_rate_t espnow_rate_current = ESPNOW_PHY_RATE;
volatile uint32_t espnow_send_ok = 0;
volatile uint32_t espnow_send_fail = 0;
volatile uint32_t espnow_token_drop = 0;
volatile bool radio_congested = false;
volatile bool provision_done = false;

udp_client_t udp_clients[MAX_UDP_CLIENTS];
int udp_sock = -1;
volatile uint32_t stat_src_drop_espnow_q = 0;
volatile uint32_t stat_src_drop_udp_q = 0;
volatile uint32_t stat_udp_paced_drop = 0;

int32_t clamp_pcm24(int32_t v) {
    if (v > PCM24_MAX) return PCM24_MAX;
    if (v < PCM24_MIN) return PCM24_MIN;
    return v;
}

int32_t abs_i32(int32_t v) {
    return (v < 0) ? -v : v;
}

int find_sink_slot_by_mac(const uint8_t mac[6]) {
    for (int i = 0; i < MAX_SINKS; i++) {
        if (sinks[i].in_use && memcmp(sinks[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

int alloc_sink_slot(const uint8_t mac[6]) {
    int existing = find_sink_slot_by_mac(mac);
    if (existing >= 0) return existing;

    for (int i = 0; i < MAX_SINKS; i++) {
        if (!sinks[i].in_use) {
            sinks[i].in_use = true;
            memcpy(sinks[i].mac, mac, 6);
            sinks[i].last_seen_us = (uint32_t)esp_timer_get_time();
            sinks[i].send_fail_streak = 0;
            return i;
        }
    }
    return -1;
}

void remove_sink_slot(const uint8_t mac[6]) {
    int idx = find_sink_slot_by_mac(mac);
    if (idx >= 0) {
        sinks[idx].in_use = false;
        memset(sinks[idx].mac, 0, 6);
        sinks[idx].last_seen_us = 0;
        sinks[idx].send_fail_streak = 0;
    }
}

int count_active_sinks(void) {
    int n = 0;
    for (int i = 0; i < MAX_SINKS; i++) {
        if (sinks[i].in_use) n++;
    }
    return n;
}

int count_active_udp_clients(void) {
    int n = 0;
    for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
        if (udp_clients[i].in_use) n++;
    }
    return n;
}

void espnow_send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        espnow_send_ok++;
    } else {
        espnow_send_fail++;
    }

    if (info && info->des_addr) {
        int idx = find_sink_slot_by_mac(info->des_addr);
        if (idx >= 0) {
            if (status == ESP_NOW_SEND_SUCCESS) {
                sinks[idx].send_fail_streak = 0;
            } else if (sinks[idx].send_fail_streak < 255) {
                sinks[idx].send_fail_streak++;
                if (sinks[idx].send_fail_streak >= SINK_SEND_FAIL_MAX_STREAK) {
                    ESP_LOGW(TAG,
                             "Removing sink after send failures (%u): %02X:%02X:%02X:%02X:%02X:%02X",
                             (unsigned)sinks[idx].send_fail_streak,
                             sinks[idx].mac[0], sinks[idx].mac[1], sinks[idx].mac[2],
                             sinks[idx].mac[3], sinks[idx].mac[4], sinks[idx].mac[5]);
                    remove_sink_slot(sinks[idx].mac);
                }
            }
        }
    }

    if (tx_tokens) xSemaphoreGive(tx_tokens);
}

void espnow_recv_cb(const esp_now_recv_info_t *ri, const uint8_t *data, int len) {
    if (!ri || !data || len < (int)sizeof(msg_hdr_t)) return;

    msg_hdr_t h;
    memcpy(&h, data, sizeof(h));

    const uint8_t *src = ri->src_addr;
    ESP_LOGI(TAG, "RX from %02X:%02X:%02X:%02X:%02X:%02X len=%d magic=0x%02X type=0x%02X room=0x%08lX",
             src[0],src[1],src[2],src[3],src[4],src[5], len, h.magic, h.type, (unsigned long)h.room_code);

    if (h.magic != PROTO_MAGIC) return;
    if (h.room_code != ROOM_CODE) return;

    if (h.type == MSG_JOIN && len >= (int)sizeof(join_msg_t)) {
        join_msg_t jm;
        memcpy(&jm, data, sizeof(jm));
        ESP_LOGI(TAG, "JOIN request: stream_id=%u (mine=%u)", (unsigned)jm.stream_id, (unsigned)stream_id);
        if (jm.stream_id != stream_id) return;

        int slot = alloc_sink_slot(src);
        if (slot < 0) return;
        sinks[slot].send_fail_streak = 0;

        add_peer_if_needed(src);
        ESP_LOGI(TAG, "JOIN %02X:%02X:%02X:%02X:%02X:%02X",
                 src[0],src[1],src[2],src[3],src[4],src[5]);
        return;
    }

    if (h.type == MSG_LEAVE && len >= (int)sizeof(leave_msg_t)) {
        leave_msg_t lm;
        memcpy(&lm, data, sizeof(lm));
        if (lm.stream_id != stream_id) return;

        ESP_LOGI(TAG, "LEAVE %02X:%02X:%02X:%02X:%02X:%02X",
                 src[0],src[1],src[2],src[3],src[4],src[5]);

        if (esp_now_is_peer_exist(src)) esp_now_del_peer(src);
        remove_sink_slot(src);
        return;
    }

    if (h.type == MSG_KEEPALIVE && len >= (int)sizeof(keepalive_msg_t)) {
        keepalive_msg_t ka;
        memcpy(&ka, data, sizeof(ka));
        if (ka.stream_id != stream_id) return;

        int idx = find_sink_slot_by_mac(src);
        if (idx < 0) {
            /* Implicit JOIN recovery — register peer on valid keepalive */
            idx = alloc_sink_slot(src);
            if (idx < 0) return;
            add_peer_if_needed(src);
            ESP_LOGI(TAG, "JOIN (via keepalive) %02X:%02X:%02X:%02X:%02X:%02X",
                     src[0],src[1],src[2],src[3],src[4],src[5]);
        }
        sinks[idx].last_seen_us = (uint32_t)esp_timer_get_time();
        sinks[idx].send_fail_streak = 0;
        return;
    }
}

void udp_server_task(void *arg) {
    (void)arg;

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed");
        vTaskDelete(NULL);
        return;
    }

    if (bind(udp_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed");
        close(udp_sock);
        udp_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP listening on port %d", UDP_PORT);

    uint8_t rx_buf[64];
    struct sockaddr_in cli;
    socklen_t cli_len;

    while (1) {
        cli_len = sizeof(cli);
        int n = recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0,
                         (struct sockaddr *)&cli, &cli_len);
        if (n < 0) continue;

        uint32_t now = (uint32_t)esp_timer_get_time();

        bool found = false;
        for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
            if (udp_clients[i].in_use &&
                udp_clients[i].addr.sin_addr.s_addr == cli.sin_addr.s_addr &&
                udp_clients[i].addr.sin_port == cli.sin_port) {
                udp_clients[i].last_seen_us = now;
                found = true;
                break;
            }
        }

        if (!found) {
            for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
                if (!udp_clients[i].in_use) {
                    udp_clients[i].in_use = true;
                    udp_clients[i].addr = cli;
                    udp_clients[i].last_seen_us = now;
                    uint8_t *ip = (uint8_t *)&cli.sin_addr.s_addr;
                    ESP_LOGI(TAG, "UDP client: %d.%d.%d.%d:%d",
                             ip[0], ip[1], ip[2], ip[3], ntohs(cli.sin_port));
                    break;
                }
            }
        }
    }
}

void source_core_main(void) {
    ESP_LOGI(TAG, "ESP-NOW LC3 SOURCE (room=0x%08lX)", (unsigned long)ROOM_CODE);
    esp_log_level_set("wifi", ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    stream_id = esp_random();

    char sta_ssid[33] = {0};
    char sta_pass[65] = {0};

    load_or_start_provisioning(sta_ssid, sizeof(sta_ssid), sta_pass, sizeof(sta_pass));
    if (sta_ssid[0] == 0) {
        return;
    }

    ESP_LOGI(TAG, "Starting APSTA source, upstream SSID: %s", sta_ssid);
    wifi_start_apsta(sta_ssid, sta_pass);
    streaming_mode_active = true;

    wifi_init_espnow();

    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    /* Legacy queues still created for UDP path + any stale references.
     * ECast owns its own internal queue; audio_q is unused on ECast path. */
    audio_q     = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_msg_t));
    udp_audio_q = xQueueCreate(UDP_AUDIO_QUEUE_LEN, sizeof(audio_msg_t));
    tx_tokens   = xSemaphoreCreateCounting(AUDIO_TX_TOKENS, AUDIO_TX_TOKENS);

    /* ── Start the new Auracast-style broadcaster ─────────────────── */
    const uint8_t bcode[16] = ECAST_BROADCAST_CODE_BYTES;
    /* Compose a human name from the room code: "Room 52-127" */
    uint16_t bld = (ROOM_CODE >> 15) & 0x3FFU;
    uint16_t rm  = (ROOM_CODE >>  5) & 0x3FFU;
    char name[24];
    snprintf(name, sizeof(name), "Room %u-%u", (unsigned)bld, (unsigned)rm);

    /* 64-bit broadcast_id = room_code in low 32, stream_id in high 32.
     * Stable per-room, distinct per-stream (for multiple sources on one room). */
    uint64_t bid = ((uint64_t)stream_id << 32) | (uint64_t)ROOM_CODE;

    bool ok = ecast_source_init(bid, stream_id, name, bcode,
                                /*enable_encryption=*/true);
    if (!ok) {
        ESP_LOGE(TAG, "ecast_source_init failed — broadcasts will not start");
    }

    /* Capture task: produces LC3 frames → ecast_source_publish_audio().
     * Legacy beacon_task / audio_send_task are stubs that exit immediately. */
    xTaskCreatePinnedToCore(audio_capture_encode_task, "cap_enc", 32768, NULL, TASK_PRIO_CAP_ENC, NULL, 1);
    xTaskCreatePinnedToCore(udp_audio_send_task, "udp_send", 4096, NULL, TASK_PRIO_UDP_SEND, NULL, 0);
    xTaskCreatePinnedToCore(udp_server_task, "udp_srv", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(source_stats_task, "src_stats", 3072, NULL, 1, NULL, 1);
}
