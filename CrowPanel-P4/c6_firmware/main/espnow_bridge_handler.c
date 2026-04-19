/*
 * C6 ESP-NOW Sink Implementation (Rewritten)
 *
 * Runs entirely on the C6 coprocessor. Handles:
 * - WiFi + ESP-NOW initialization (fixed channel 6)
 * - Room beacon scanning (channel 6 only)
 * - Room joining / leaving
 * - Forwarding raw LC3 audio packets to P4 via SDIO (NO decoding on C6)
 * - Sending room/stats/error events to P4 via SDIO
 *
 * The P4 host sends commands (INIT, SCAN, JOIN, LEAVE, etc.)
 * and receives events (STATUS, ROOM_FOUND, AUDIO_RAW, etc.)
 * P4 is responsible for LC3 decoding and I2S playback.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "slave_control.h"
#include "esp_app_desc.h"
#include "esp_event.h"

#include "espnow_protocol.h"
#include "espnow_sink_c6.h"

static const char *TAG = "espnow_c6";

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * ESP-NOW air protocol structures (must match source firmware)
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t type;
    uint32_t room_code;
} air_msg_hdr_t;

typedef struct __attribute__((packed)) {
    air_msg_hdr_t h;
    uint8_t  wifi_channel;
    uint8_t  channels;
    uint16_t sample_rate_hz;
    uint16_t frame_us;
    uint8_t  bytes_per_ch;
    uint32_t stream_id;
} air_beacon_msg_t;

typedef struct __attribute__((packed)) {
    air_msg_hdr_t h;
    uint32_t stream_id;
} air_join_msg_t;

typedef struct __attribute__((packed)) {
    air_msg_hdr_t h;
    uint32_t stream_id;
} air_leave_msg_t;

typedef struct __attribute__((packed)) {
    air_msg_hdr_t h;
    uint32_t stream_id;
    uint16_t last_seq_rx;
} air_keepalive_msg_t;

typedef struct __attribute__((packed)) {
    air_msg_hdr_t h;
    uint16_t seq;
    uint16_t payload_len;
    uint32_t src_t_us;
    uint8_t  flags;
    uint32_t capture_us;
    uint8_t  payload[ESPNOW_LC3_BYTES_PER_CH * ESPNOW_CHANNELS];
} air_audio_msg_t;

/* Message codes */
#define AIR_MSG_BEACON    0x01
#define AIR_MSG_JOIN      0x02
#define AIR_MSG_LEAVE     0x03
#define AIR_MSG_KEEPALIVE 0x04
#define AIR_MSG_AUDIO     0x10

/* Fixed channel - both source and sink operate on channel 6 */
#define ESPNOW_FIXED_CHANNEL  6

#ifndef WIFI_PHY_RATE_54M
#define WIFI_PHY_RATE_54M WIFI_PHY_RATE_24M
#endif

#define ESPNOW_LINK_RATE_PRIMARY   WIFI_PHY_RATE_54M
#define ESPNOW_LINK_RATE_FALLBACK  WIFI_PHY_RATE_24M
#define KEEPALIVE_INTERVAL_MS      500
#define KEEPALIVE_STATS_TICKS      10

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * State
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static espnow_state_t s_state = ESPNOW_STATE_NOT_INIT;
static bool s_wifi_initialized = false;
static bool s_espnow_initialized = false;

/* Room scanning */
static espnow_evt_room_t s_rooms[ESPNOW_MAX_ROOMS];
static int s_room_count = 0;
static SemaphoreHandle_t s_rooms_mutex = NULL;

/* Selected room */
static uint8_t  s_sel_mac[6] = {0};
static uint8_t  s_sel_channel = ESPNOW_FIXED_CHANNEL;
static uint32_t s_sel_room_code = 0;
static uint32_t s_sel_stream_id = 0;

/* Statistics */
static volatile uint32_t s_packets_rx = 0;
static volatile uint32_t s_packets_lost_air = 0;
static volatile uint32_t s_packets_dropped_c6q = 0;
static volatile uint32_t s_packets_dup = 0;   /* TX-redundancy duplicates suppressed */
static volatile uint16_t s_last_seq = 0;
static volatile bool s_seq_valid = false;
static volatile int8_t s_rssi_last = 0;
static volatile uint8_t s_sdio_send_errors = 0;

/* Audio timeout tracking */
#define AUDIO_RX_TIMEOUT_US        2500000   /* tolerate short RF stalls before timeout handling */
#define BEACON_RX_TIMEOUT_US       8000000   /* beacon may be sparse during congestion */
static volatile uint32_t s_last_audio_rx_us = 0;
static volatile uint32_t s_last_beacon_rx_us = 0;

/* WiFi restart recovery */
static volatile bool s_espnow_needs_reinit = false;

/* Tasks */
static TaskHandle_t s_keepalive_task = NULL;
static TaskHandle_t s_scan_task = NULL;
static TaskHandle_t s_forward_task = NULL;

/* Audio forward queue: ESP-NOW recv -> forward task -> SDIO to P4
 * We forward raw LC3 packets without decoding. */
#define AUDIO_QUEUE_DEPTH  128

typedef struct {
    uint16_t seq;
    uint16_t lost_before;   /* Number of lost packets before this one */
    uint32_t capture_us;    /* Source capture timestamp for P4 clock sync */
    uint8_t  payload[ESPNOW_LC3_BYTES_PER_CH * ESPNOW_CHANNELS];  /* 144 bytes raw LC3 */
} raw_audio_item_t;

static QueueHandle_t s_audio_raw_queue = NULL;

static void set_peer_rate_high_with_fallback(const uint8_t peer_mac[6])
{
    esp_now_rate_config_t rcfg = {
        .phymode = WIFI_PHY_MODE_11G,
        .rate    = ESPNOW_LINK_RATE_PRIMARY,
        .ersu    = false,
        .dcm     = false,
    };

    esp_err_t ret = esp_now_set_peer_rate_config(peer_mac, &rcfg);
    if (ret != ESP_OK) {
        rcfg.rate = ESPNOW_LINK_RATE_FALLBACK;
        ret = esp_now_set_peer_rate_config(peer_mac, &rcfg);
        ESP_LOGW(TAG, "Primary peer rate set failed, fallback applied: %s", esp_err_to_name(ret));
    }
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Helpers: send events to P4
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static void send_status_to_host(void)
{
    espnow_evt_status_t evt = {
        .state = (uint8_t)s_state,
        .wifi_init = s_wifi_initialized ? 1 : 0,
        .espnow_init = s_espnow_initialized ? 1 : 0,
    };
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_STATUS,
                                (const uint8_t *)&evt, sizeof(evt));
}

static void send_error_to_host(esp_err_t err, const char *msg)
{
    espnow_evt_error_t evt = {0};
    evt.error_code = err;
    if (msg) {
        strncpy(evt.message, msg, sizeof(evt.message) - 1);
    }
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_ERROR,
                                (const uint8_t *)&evt, sizeof(evt));
}

static void send_room_to_host(const espnow_evt_room_t *room)
{
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_ROOM_FOUND,
                                (const uint8_t *)room, sizeof(*room));
}

static void send_raw_audio_to_host(const raw_audio_item_t *item)
{
    /* Build raw audio event and send to P4 â€” P4 will decode LC3 */
    static espnow_evt_audio_raw_t audio_evt;
    audio_evt.seq = item->seq;
    audio_evt.lost_before = item->lost_before;
    audio_evt.capture_us = item->capture_us;
    memcpy(audio_evt.payload, item->payload, sizeof(audio_evt.payload));

    esp_err_t ret = esp_hosted_send_custom_data(ESPNOW_MSG_EVT_AUDIO,
                                (const uint8_t *)&audio_evt, sizeof(audio_evt));
    if (ret != ESP_OK && s_sdio_send_errors < 255) {
        s_sdio_send_errors++;
    }
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Forward task: reads raw LC3 from queue, sends to P4 via SDIO
 * Runs on its own task so WiFi callback returns immediately.
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static void forward_task_fn(void *arg)
{
    (void)arg;
    raw_audio_item_t item;

    ESP_LOGI(TAG, "Audio forward task started (raw LC3 â†’ P4)");

    while (1) {
        if (xQueueReceive(s_audio_raw_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }

        /* Forward the raw LC3 packet to P4 for decoding */
        send_raw_audio_to_host(&item);
    }
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * ESP-NOW receive callback (runs in WiFi task context)
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (!info || !data || len < (int)sizeof(air_msg_hdr_t)) return;

    air_msg_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != ESPNOW_PROTO_MAGIC) return;

    /* â”€â”€ Beacon â”€â”€ */
    if (hdr.type == AIR_MSG_BEACON && len >= (int)sizeof(air_beacon_msg_t)) {
        air_beacon_msg_t beacon;
        memcpy(&beacon, data, sizeof(beacon));

        /* Validate codec parameters */
        if (beacon.channels != ESPNOW_CHANNELS ||
            beacon.sample_rate_hz != ESPNOW_SAMPLE_RATE_HZ ||
            beacon.frame_us != ESPNOW_LC3_FRAME_US ||
            beacon.bytes_per_ch != ESPNOW_LC3_BYTES_PER_CH) {
            return;
        }

        if (s_state == ESPNOW_STATE_SCANNING) {
            int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;

            if (xSemaphoreTake(s_rooms_mutex, 0) == pdTRUE) {
                bool found = false;
                for (int i = 0; i < s_room_count; i++) {
                    if (s_rooms[i].stream_id == beacon.stream_id &&
                        memcmp(s_rooms[i].mac, info->src_addr, 6) == 0) {
                        s_rooms[i].rssi = rssi;
                        s_rooms[i].wifi_channel = beacon.wifi_channel;
                        found = true;
                        break;
                    }
                }
                if (!found && s_room_count < ESPNOW_MAX_ROOMS) {
                    espnow_evt_room_t *r = &s_rooms[s_room_count++];
                    memcpy(r->mac, info->src_addr, 6);
                    r->wifi_channel = beacon.wifi_channel;
                    r->room_code = beacon.h.room_code;
                    r->stream_id = beacon.stream_id;
                    r->rssi = rssi;

                    ESP_LOGI(TAG, "Room: code=0x%08lX ch=%d rssi=%d stream=%lu",
                             (unsigned long)r->room_code, r->wifi_channel, rssi,
                             (unsigned long)r->stream_id);

                    send_room_to_host(r);
                }
                xSemaphoreGive(s_rooms_mutex);
            }
        } else if (s_state == ESPNOW_STATE_CONNECTED &&
                   memcmp(info->src_addr, s_sel_mac, 6) == 0 &&
                   beacon.h.room_code == s_sel_room_code) {
            s_last_beacon_rx_us = (uint32_t)esp_timer_get_time();
            /* Track stream ID changes */
            if (s_sel_stream_id != beacon.stream_id) {
                s_sel_stream_id = beacon.stream_id;
                s_seq_valid = false;
            }
        }
        return;
    }

    /* â”€â”€ Audio â”€â”€ */
    if (hdr.type == AIR_MSG_AUDIO && len >= (int)sizeof(air_audio_msg_t)) {
        if (s_state != ESPNOW_STATE_CONNECTED) return;
        if (memcmp(info->src_addr, s_sel_mac, 6) != 0) return;

        air_audio_msg_t audio;
        memcpy(&audio, data, sizeof(audio));

        s_rssi_last = info->rx_ctrl ? info->rx_ctrl->rssi : 0;

        /* Track sequence for loss estimation AND deduplicate TX-redundancy copies.
         * Source sends each frame AUDIO_TX_REDUNDANCY times; we keep the first copy
         * to reach us and drop all subsequent copies carrying the same (or older)
         * seq. Wrap-around-safe via signed 16-bit delta arithmetic. */
        uint16_t lost_before = 0;
        if (s_seq_valid) {
            int16_t delta = (int16_t)((uint16_t)(audio.seq - s_last_seq));
            if (delta <= 0) {
                /* Duplicate (second/third copy of a frame we already forwarded)
                 * or a stale reorder. Drop silently — don't bill SDIO bandwidth. */
                s_packets_dup++;
                return;
            }
            if (delta > 1) {
                lost_before = (uint16_t)(delta - 1);
                s_packets_lost_air += lost_before;
            }
        }
        s_last_seq = audio.seq;
        s_seq_valid = true;
        s_packets_rx++;
        s_last_audio_rx_us = (uint32_t)esp_timer_get_time();

        if (s_packets_rx == 1) {
            ESP_LOGI(TAG, "First audio packet received: seq=%d rssi=%d",
                     audio.seq, s_rssi_last);
        }

        /* Enqueue raw LC3 for forward task (no decode â€” just pass through) */
        if (s_audio_raw_queue) {
            raw_audio_item_t item;
            item.seq = audio.seq;
            item.lost_before = lost_before;
            item.capture_us = audio.capture_us;
            memcpy(item.payload, audio.payload, sizeof(item.payload));

            if (xQueueSend(s_audio_raw_queue, &item, 0) != pdTRUE) {
                /* Queue full: drop oldest and keep newest frame to reduce real-time lag. */
                raw_audio_item_t old_item;
                if (xQueueReceive(s_audio_raw_queue, &old_item, 0) == pdTRUE) {
                    s_packets_dropped_c6q++;
                }
                if (xQueueSend(s_audio_raw_queue, &item, 0) != pdTRUE) {
                    s_packets_dropped_c6q++;
                }
            }
        }
        return;
    }
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * WiFi + ESP-NOW init/deinit
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static esp_err_t do_espnow_init(void)
{
    if (s_espnow_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing ESP-NOW on STA interface (fixed channel %d)...",
             ESPNOW_FIXED_CHANNEL);

    uint8_t sta_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    ESP_LOGI(TAG, "C6 STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             sta_mac[0], sta_mac[1], sta_mac[2],
             sta_mac[3], sta_mac[4], sta_mac[5]);

    /* Set TX power to max */
    esp_wifi_set_max_tx_power(84);

    /* Set fixed channel 6 */
    esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);

    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_ESPNOW_INTERNAL) {
            ESP_LOGW(TAG, "esp_now_init returned INTERNAL (0x%x) â€” proceeding", ret);
        } else {
            ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    esp_now_register_recv_cb(espnow_recv_cb);

    /* Disable power save for low latency */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_wifi_initialized = true;
    s_espnow_initialized = true;
    s_espnow_needs_reinit = false;

    /* Create audio forward queue + task */
    if (!s_audio_raw_queue) {
        s_audio_raw_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(raw_audio_item_t));
        if (!s_audio_raw_queue) {
            ESP_LOGE(TAG, "Failed to create audio forward queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_forward_task) {
        xTaskCreate(forward_task_fn, "audio_fwd", 4096, NULL, 8, &s_forward_task);
    }

    s_state = ESPNOW_STATE_IDLE;
    ESP_LOGI(TAG, "ESP-NOW initialized on channel %d", ESPNOW_FIXED_CHANNEL);
    send_status_to_host();

    return ESP_OK;
}

static void do_espnow_deinit(void)
{
    if (!s_espnow_initialized) return;

    if (s_keepalive_task) {
        vTaskDelete(s_keepalive_task);
        s_keepalive_task = NULL;
    }

    if (s_forward_task) {
        vTaskDelete(s_forward_task);
        s_forward_task = NULL;
    }
    if (s_audio_raw_queue) {
        vQueueDelete(s_audio_raw_queue);
        s_audio_raw_queue = NULL;
    }

    esp_now_unregister_recv_cb();
    esp_now_deinit();

    s_espnow_initialized = false;
    s_espnow_needs_reinit = false;
    s_state = ESPNOW_STATE_NOT_INIT;
    send_status_to_host();
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Room scanning (fixed channel 6 only â€” no multi-channel sweep)
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static void scan_task_fn(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_rooms_mutex, portMAX_DELAY);
    memset(s_rooms, 0, sizeof(s_rooms));
    s_room_count = 0;
    xSemaphoreGive(s_rooms_mutex);

    s_state = ESPNOW_STATE_SCANNING;
    send_status_to_host();

    /* Fixed channel 6 - just listen for beacons */
    esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);

    /* 2 passes, 300ms each - total 600ms listening on channel 6 */
    for (int pass = 0; pass < 2; pass++) {
        if (s_state != ESPNOW_STATE_SCANNING) break;
        ESP_LOGI(TAG, "Scanning ch %d (pass %d)", ESPNOW_FIXED_CHANNEL, pass + 1);
        vTaskDelay(pdMS_TO_TICKS(ESPNOW_SCAN_LISTEN_MS));
    }

    s_state = ESPNOW_STATE_IDLE;

    espnow_evt_scan_done_t done = { .room_count = (uint8_t)s_room_count };
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_SCAN_DONE,
                                (const uint8_t *)&done, sizeof(done));
    send_status_to_host();

    ESP_LOGI(TAG, "Scan done, %d rooms on ch %d", s_room_count, ESPNOW_FIXED_CHANNEL);
    s_scan_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t do_scan(void)
{
    if (!s_espnow_initialized) {
        send_error_to_host(ESP_ERR_INVALID_STATE, "ESP-NOW not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == ESPNOW_STATE_CONNECTED) {
        send_error_to_host(ESP_ERR_INVALID_STATE, "Leave room before scanning");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_task) {
        send_error_to_host(ESP_ERR_INVALID_STATE, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    xTaskCreate(scan_task_fn, "espnow_scan", 4096, NULL, 5, &s_scan_task);
    return ESP_OK;
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Room join / leave
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/

static void sync_selected_peer(void)
{
    if (s_state != ESPNOW_STATE_CONNECTED) return;

    /* Always enforce channel 6 */
    esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_is_peer_exist(s_sel_mac)) {
        esp_now_del_peer(s_sel_mac);
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_sel_mac, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    set_peer_rate_high_with_fallback(s_sel_mac);
}

/* WiFi event handler: detect channel changes AND WiFi restarts from
 * ESP-Hosted STA operations */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_STOP) {
        if (s_espnow_initialized) {
            ESP_LOGW(TAG, "WiFi STOPPED by host â€” ESP-NOW destroyed, will re-init on restart");
            s_espnow_initialized = false;
            s_wifi_initialized = false;
            s_espnow_needs_reinit = true;
        }
        return;
    }

    if (event_id == WIFI_EVENT_STA_START) {
        if (s_espnow_needs_reinit) {
            ESP_LOGW(TAG, "WiFi RESTARTED â€” re-initializing ESP-NOW...");
            s_espnow_needs_reinit = false;

            vTaskDelay(pdMS_TO_TICKS(100));

            esp_wifi_set_max_tx_power(84);
            esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);

            esp_err_t ret = esp_now_init();
            if (ret == ESP_OK || ret == ESP_ERR_ESPNOW_INTERNAL) {
                esp_now_register_recv_cb(espnow_recv_cb);
                esp_wifi_set_ps(WIFI_PS_NONE);
                s_espnow_initialized = true;
                s_wifi_initialized = true;
                ESP_LOGI(TAG, "ESP-NOW re-initialized after WiFi restart (ch %d)",
                         ESPNOW_FIXED_CHANNEL);

                if (s_state == ESPNOW_STATE_CONNECTED) {
                    ESP_LOGI(TAG, "Restoring room connection on ch %d", ESPNOW_FIXED_CHANNEL);
                    sync_selected_peer();

                    air_join_msg_t rj = {0};
                    rj.h.magic = ESPNOW_PROTO_MAGIC;
                    rj.h.type = AIR_MSG_JOIN;
                    rj.h.room_code = s_sel_room_code;
                    rj.stream_id = s_sel_stream_id;
                    esp_now_send(s_sel_mac, (uint8_t *)&rj, sizeof(rj));
                }
                send_status_to_host();
            } else {
                ESP_LOGE(TAG, "ESP-NOW re-init FAILED: %s", esp_err_to_name(ret));
                send_error_to_host(ret, "ESP-NOW re-init failed after WiFi restart");
            }
        }
        return;
    }

    /* Channel drift protection â€” always restore to channel 6 */
    if (s_state != ESPNOW_STATE_CONNECTED) return;

    if (event_id == WIFI_EVENT_SCAN_DONE ||
        event_id == WIFI_EVENT_STA_DISCONNECTED ||
        event_id == WIFI_EVENT_STA_CONNECTED) {
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);
        if (ch != ESPNOW_FIXED_CHANNEL) {
            ESP_LOGW(TAG, "WiFi event %ld changed channel %d->%d, restoring to %d",
                     (long)event_id, ch, ESPNOW_FIXED_CHANNEL, ESPNOW_FIXED_CHANNEL);
            sync_selected_peer();
        }
    }
}

static void send_stats_to_host(uint8_t wifi_ch)
{
    uint32_t total_lost = s_packets_lost_air + s_packets_dropped_c6q;
    espnow_evt_stats_t stats = {
        .packets_rx = s_packets_rx,
        .packets_lost = total_lost,
        .plc_frames = s_packets_dropped_c6q,  /* Reused field for C6 queue drops */
        .rssi_last = s_rssi_last,
        .wifi_channel = wifi_ch,
        .sdio_send_errors = s_sdio_send_errors,
    };
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_STATS,
                                (const uint8_t *)&stats, sizeof(stats));
}

static void keepalive_task_fn(void *arg)
{
    (void)arg;
    int heartbeat_counter = 0;
    uint8_t timeout_streak = 0;

    while (s_state == ESPNOW_STATE_CONNECTED) {
        if (!s_espnow_initialized) {
            ESP_LOGW(TAG, "ESP-NOW down (WiFi restart?), waiting for re-init...");
            send_stats_to_host(0);
            vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_INTERVAL_MS));
            heartbeat_counter = 0;
            continue;
        }

        /* Always enforce channel 6 */
        esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);

        uint8_t primary = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&primary, &second);
        if (primary != ESPNOW_FIXED_CHANNEL) {
            ESP_LOGW(TAG, "Channel drifted: current=%d expected=%d, re-syncing",
                     primary, ESPNOW_FIXED_CHANNEL);
            sync_selected_peer();
            esp_wifi_get_channel(&primary, &second);
        }

        air_keepalive_msg_t ka = {0};
        ka.h.magic = ESPNOW_PROTO_MAGIC;
        ka.h.type = AIR_MSG_KEEPALIVE;
        ka.h.room_code = s_sel_room_code;
        ka.stream_id = s_sel_stream_id;
        ka.last_seq_rx = s_last_seq;

        esp_err_t ret = esp_now_send(s_sel_mac, (uint8_t *)&ka, sizeof(ka));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Keepalive send failed: %s, re-syncing",
                     esp_err_to_name(ret));
            sync_selected_peer();
            esp_now_send(s_sel_mac, (uint8_t *)&ka, sizeof(ka));
        }

        /* Audio timeout detection + auto-rejoin */
        uint32_t now_us = (uint32_t)esp_timer_get_time();
        bool audio_stale = false;
        bool beacon_stale = true;
        bool timed_out = false;

        if (s_last_audio_rx_us != 0) {
            audio_stale = ((now_us - s_last_audio_rx_us) > AUDIO_RX_TIMEOUT_US);
        }
        if (s_last_beacon_rx_us != 0) {
            beacon_stale = ((now_us - s_last_beacon_rx_us) > BEACON_RX_TIMEOUT_US);
        }
        timed_out = audio_stale && beacon_stale;

        if (timed_out) {
            timeout_streak++;
            if ((timeout_streak == 1) || ((timeout_streak % 8) == 0)) {
                ESP_LOGW(TAG, "Audio timeout (%u): re-syncing + rejoin", timeout_streak);
            }
            sync_selected_peer();

            air_join_msg_t rj = {0};
            rj.h.magic = ESPNOW_PROTO_MAGIC;
            rj.h.type = AIR_MSG_JOIN;
            rj.h.room_code = s_sel_room_code;
            rj.stream_id = s_sel_stream_id;
            esp_now_send(s_sel_mac, (uint8_t *)&rj, sizeof(rj));

            s_last_audio_rx_us = now_us;
        } else {
            timeout_streak = 0;
        }

        /* Send stats every ~5 seconds. */
        if (++heartbeat_counter >= KEEPALIVE_STATS_TICKS) {
            heartbeat_counter = 0;
            send_stats_to_host(primary);
        }

        vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_INTERVAL_MS));
    }
    s_keepalive_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t do_join(const espnow_cmd_join_t *cmd)
{
    if (!s_espnow_initialized) {
        send_error_to_host(ESP_ERR_INVALID_STATE, "ESP-NOW not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Joining room 0x%08lX ch=%d stream=%lu",
             (unsigned long)cmd->room_code, ESPNOW_FIXED_CHANNEL,
             (unsigned long)cmd->stream_id);

    /* Always use fixed channel 6 regardless of what P4 requested */
    esp_err_t ch_ret = esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (ch_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi channel %d: %s",
                 ESPNOW_FIXED_CHANNEL, esp_err_to_name(ch_ret));
        send_error_to_host(ch_ret, "Set channel failed");
        return ch_ret;
    }

    if (!esp_now_is_peer_exist(cmd->mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, cmd->mac, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t ret = esp_now_add_peer(&peer);
        if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
            send_error_to_host(ret, "Failed to add peer");
            return ret;
        }
    } else {
        sync_selected_peer();
    }

    /* Set highest practical peer rate first, then fallback if unsupported. */
    set_peer_rate_high_with_fallback(cmd->mac);

    memcpy(s_sel_mac, cmd->mac, 6);
    s_sel_channel = ESPNOW_FIXED_CHANNEL;
    s_sel_room_code = cmd->room_code;
    s_sel_stream_id = cmd->stream_id;
    s_seq_valid = false;
    s_packets_rx = 0;
    s_packets_lost_air = 0;
    s_packets_dropped_c6q = 0;
    s_last_audio_rx_us = 0;
    s_last_beacon_rx_us = 0;
    s_sdio_send_errors = 0;

    air_join_msg_t join = {0};
    join.h.magic = ESPNOW_PROTO_MAGIC;
    join.h.type = AIR_MSG_JOIN;
    join.h.room_code = cmd->room_code;
    join.stream_id = cmd->stream_id;
    esp_err_t send_ret = esp_now_send(cmd->mac, (uint8_t *)&join, sizeof(join));
    if (send_ret != ESP_OK) {
        ESP_LOGE(TAG, "JOIN send failed: %s", esp_err_to_name(send_ret));
        send_error_to_host(send_ret, "JOIN send failed");
        return send_ret;
    }

    s_state = ESPNOW_STATE_CONNECTED;

    if (s_keepalive_task) {
        vTaskDelete(s_keepalive_task);
    }
    xTaskCreate(keepalive_task_fn, "keepalive", 4096, NULL, 3, &s_keepalive_task);

    espnow_evt_joined_t evt = {0};
    memcpy(evt.mac, cmd->mac, 6);
    evt.wifi_channel = ESPNOW_FIXED_CHANNEL;
    evt.room_code = cmd->room_code;
    evt.stream_id = cmd->stream_id;
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_JOINED,
                                (const uint8_t *)&evt, sizeof(evt));
    send_status_to_host();

    return ESP_OK;
}

static void do_leave(void)
{
    if (s_state != ESPNOW_STATE_CONNECTED) return;

    air_leave_msg_t leave = {0};
    leave.h.magic = ESPNOW_PROTO_MAGIC;
    leave.h.type = AIR_MSG_LEAVE;
    leave.h.room_code = s_sel_room_code;
    leave.stream_id = s_sel_stream_id;
    esp_now_send(s_sel_mac, (uint8_t *)&leave, sizeof(leave));

    esp_now_del_peer(s_sel_mac);

    if (s_keepalive_task) {
        vTaskDelete(s_keepalive_task);
        s_keepalive_task = NULL;
    }

    s_state = ESPNOW_STATE_IDLE;

    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_LEFT, NULL, 0);
    send_status_to_host();

    ESP_LOGI(TAG, "Left room");
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * SDIO command handlers (called from P4 via ESP-Hosted custom data)
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static void on_cmd_init(uint32_t msg_id, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "CMD: INIT");
    esp_err_t ret = do_espnow_init();
    if (ret != ESP_OK) {
        send_error_to_host(ret, "ESP-NOW init failed");
    }
}

static void on_cmd_deinit(uint32_t msg_id, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "CMD: DEINIT");
    do_espnow_deinit();
}

static void on_cmd_scan(uint32_t msg_id, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "CMD: SCAN");
    do_scan();
}

static void on_cmd_stop_scan(uint32_t msg_id, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "CMD: STOP_SCAN");
    if (s_state == ESPNOW_STATE_SCANNING) {
        s_state = ESPNOW_STATE_IDLE;
    }
}

static void on_cmd_join(uint32_t msg_id, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "CMD: JOIN");
    if (len < sizeof(espnow_cmd_join_t)) {
        send_error_to_host(ESP_ERR_INVALID_ARG, "JOIN payload too small");
        return;
    }
    espnow_cmd_join_t cmd;
    memcpy(&cmd, data, sizeof(cmd));
    do_join(&cmd);
}

static void on_cmd_leave(uint32_t msg_id, const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "CMD: LEAVE");
    do_leave();
}

static void on_cmd_get_status(uint32_t msg_id, const uint8_t *data, size_t len)
{
    send_status_to_host();

    if (s_state == ESPNOW_STATE_CONNECTED) {
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);
        send_stats_to_host(ch);
    }
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Deferred firmware version response
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
static espnow_evt_fw_ver_t s_pending_fw_ver;
static esp_timer_handle_t  s_fw_ver_timer;

static void fw_ver_timer_cb(void *arg)
{
    esp_hosted_send_custom_data(ESPNOW_MSG_EVT_FW_VER,
                                (const uint8_t *)&s_pending_fw_ver,
                                sizeof(s_pending_fw_ver));
}

static void on_cmd_get_fw_ver(uint32_t msg_id, const uint8_t *data, size_t len)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    memset(&s_pending_fw_ver, 0, sizeof(s_pending_fw_ver));
    strncpy(s_pending_fw_ver.version, desc->version, sizeof(s_pending_fw_ver.version) - 1);
    strncpy(s_pending_fw_ver.project, desc->project_name, sizeof(s_pending_fw_ver.project) - 1);
    ESP_LOGI(TAG, "FW version query: project='%s' version='%s'",
             s_pending_fw_ver.project, s_pending_fw_ver.version);

    if (s_fw_ver_timer) {
        esp_timer_start_once(s_fw_ver_timer, 20 * 1000);
    } else {
        ESP_LOGE(TAG, "FW version timer not created!");
    }
}

/* â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Public init
 * â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€*/
esp_err_t espnow_sink_c6_init(void)
{
    s_rooms_mutex = xSemaphoreCreateMutex();
    if (!s_rooms_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    esp_err_t ret;
    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_INIT, on_cmd_init);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register INIT handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_DEINIT, on_cmd_deinit);
    if (ret != ESP_OK) return ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_SCAN, on_cmd_scan);
    if (ret != ESP_OK) return ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_STOP_SCAN, on_cmd_stop_scan);
    if (ret != ESP_OK) return ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_JOIN, on_cmd_join);
    if (ret != ESP_OK) return ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_LEAVE, on_cmd_leave);
    if (ret != ESP_OK) return ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_GET_STATUS, on_cmd_get_status);
    if (ret != ESP_OK) return ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_GET_FW_VER, on_cmd_get_fw_ver);
    if (ret != ESP_OK) return ret;

    const esp_timer_create_args_t fw_ver_timer_args = {
        .callback = fw_ver_timer_cb,
        .name = "fw_ver_reply",
    };
    ret = esp_timer_create(&fw_ver_timer_args, &s_fw_ver_timer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create FW version timer: %s", esp_err_to_name(ret));
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);

    ESP_LOGI(TAG, "ESP-NOW sink bridge handlers registered (fixed ch %d, raw LC3 forwarding)",
             ESPNOW_FIXED_CHANNEL);
    ESP_LOGI(TAG, "Waiting for INIT command from P4...");

    return ESP_OK;
}
