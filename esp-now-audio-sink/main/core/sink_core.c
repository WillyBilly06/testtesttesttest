// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#include "sink_app_internal.h"

const char *TAG = "SINK";

const uint8_t SCAN_CHANNELS[] = {1, 6, 11};

room_info_t rooms[MAX_ROOMS];
volatile int room_count = 0;
volatile bool discovery_mode = false;

uint8_t  sel_mac[6] = {0};
uint32_t sel_room = 0;
uint8_t  sel_channel = 0;
uint32_t sel_stream_id = 0;
volatile bool selected = false;

rx_slot_t rx_slots[RX_SLOTS];
QueueHandle_t free_q;
QueueHandle_t full_q;

volatile uint16_t last_seq_rx = 0;
volatile uint32_t last_audio_rx_us = 0;

volatile uint32_t stat_decoded = 0;
volatile uint32_t stat_plc = 0;
volatile uint32_t stat_dropped = 0;
volatile uint8_t adaptive_target_fill = TARGET_FILL;
volatile uint8_t adaptive_packet_wait_ms = PACKET_WAIT_MS_MIN;

volatile int32_t  latency_raw_us = 0;
volatile int32_t  latency_smooth_us = 0;
volatile int32_t  clock_offset_us = 0;
volatile int32_t  capture_clock_offset_us = 0;
volatile uint32_t clock_sync_count = 0;
volatile int32_t  playout_err_us = 0;
volatile int32_t  rx_jitter_us = 0;
volatile uint8_t  burst_guard_s = 0;

volatile bool request_stream_reset = false;
volatile bool request_rejoin = false;
volatile uint32_t request_rejoin_at_us = 0;

lc3_decoder_mem_48k_t lc3_dec_mem[CHANNELS];
lc3_decoder_t lc3_dec[CHANNELS];
i2s_chan_handle_t i2s_tx = NULL;

void add_or_update_room(const uint8_t mac[6], const beacon_msg_t *b, int8_t rssi) {
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].active &&
            rooms[i].room_code == b->h.room_code &&
            memcmp(rooms[i].mac, mac, 6) == 0 &&
            rooms[i].stream_id == b->stream_id) {
            rooms[i].rssi = rssi;
            rooms[i].channel = b->wifi_channel;
            return;
        }
    }
    if (room_count >= MAX_ROOMS) return;

    room_info_t *r = &rooms[room_count++];
    r->active = true;
    r->room_code = b->h.room_code;
    r->channel = b->wifi_channel;
    memcpy(r->mac, mac, 6);
    r->rssi = rssi;
    r->stream_id = b->stream_id;
}

int uart_read_line(char *buf, int max_len) {
    int pos = 0;
    while (pos < max_len - 1) {
        uint8_t ch;
        int n = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) break;
            continue;
        }
        buf[pos++] = (char)ch;
    }
    buf[pos] = 0;
    return pos;
}

void flush_full_queue(void) {
    uint8_t idx;
    while (xQueueReceive(full_q, &idx, 0) == pdTRUE) {
        xQueueSend(free_q, &idx, 0);
    }
}

int32_t iabs32(int32_t v) {
    return (v < 0) ? -v : v;
}

void sink_core_main(void) {
    ESP_LOGI(TAG, "ESP-NOW LC3 SINK");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_espnow();
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    free_q = xQueueCreate(RX_SLOTS, sizeof(uint8_t));
    full_q = xQueueCreate(RX_SLOTS, sizeof(uint8_t));
    for (uint8_t i = 0; i < RX_SLOTS; i++) {
        xQueueSend(free_q, &i, 0);
    }

    xTaskCreatePinnedToCore(playback_task, "playback", 8192, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(room_select_task, "room_sel", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(keepalive_task, "keepalive", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, NULL, 1, NULL, 1);
}
