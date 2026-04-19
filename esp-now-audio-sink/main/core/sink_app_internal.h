// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#pragma once

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "lc3.h"

extern const char *TAG;

#define SAMPLE_RATE_HZ          48000
#define CHANNELS               2
#define LC3_FRAME_US           7500
#define LC3_BYTES_PER_CH       72
#define SAMPLES_PER_FRAME      ((SAMPLE_RATE_HZ * LC3_FRAME_US) / 1000000)

#define I2S_BYTES_PER_SAMPLE   4
#define I2S_FRAME_BYTES        (SAMPLES_PER_FRAME * CHANNELS * I2S_BYTES_PER_SAMPLE)

#define PIN_BCLK               GPIO_NUM_27
#define PIN_WS                 GPIO_NUM_25
#define PIN_DOUT               GPIO_NUM_26

extern const uint8_t SCAN_CHANNELS[];
#define SCAN_CHANNEL_COUNT     3
#define SCAN_LISTEN_MS         300
#define MAX_ROOMS              16

#define RX_SLOTS               18
#define PREBUFFER_FRAMES       2
#define TARGET_FILL            2
#define KEEPALIVE_MS           1000

#define TARGET_FILL_MIN        2
#define TARGET_FILL_MAX        3
#define PACKET_WAIT_MS_MIN     2
#define PACKET_WAIT_MS_MAX     3
#define PHASE_DELAY_MIN_US     1200
#define PHASE_DELAY_MAX_US     2000
#define BURST_GUARD_SECONDS    1
#define BURST_JITTER_US        9000
#define BURST_PHASE_ERR_US     12000
#define PLC_MAX_CONSECUTIVE    3

#define AUDIO_RX_TIMEOUT_US    1000000
#define REJOIN_BACKOFF_MS      250

#define PROTO_MAGIC            0xA5

typedef enum {
    MSG_BEACON     = 0x01,
    MSG_JOIN       = 0x02,
    MSG_LEAVE      = 0x03,
    MSG_KEEPALIVE  = 0x04,
    MSG_AUDIO      = 0x10,
} msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t type;
    uint32_t room_code;
} msg_hdr_t;

typedef struct __attribute__((packed)) {
    msg_hdr_t h;
    uint8_t  wifi_channel;
    uint8_t  channels;
    uint16_t sample_rate_hz;
    uint16_t frame_us;
    uint8_t  bytes_per_ch;
    uint32_t stream_id;
} beacon_msg_t;

typedef struct __attribute__((packed)) {
    msg_hdr_t h;
    uint32_t stream_id;
} join_msg_t;

typedef struct __attribute__((packed)) {
    msg_hdr_t h;
    uint32_t stream_id;
} leave_msg_t;

typedef struct __attribute__((packed)) {
    msg_hdr_t h;
    uint32_t stream_id;
    uint16_t last_seq_rx;
} keepalive_msg_t;

typedef struct __attribute__((packed)) {
    msg_hdr_t h;
    uint16_t seq;
    uint16_t payload_len;
    uint32_t src_t_us;
    uint8_t  flags;
    uint32_t capture_us;
    uint8_t  payload[LC3_BYTES_PER_CH * CHANNELS];
} audio_msg_t;

typedef struct {
    bool     active;
    uint32_t room_code;
    uint8_t  channel;
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t stream_id;
} room_info_t;

typedef struct {
    audio_msg_t msg;
    int         len;
} rx_slot_t;

extern room_info_t rooms[MAX_ROOMS];
extern volatile int room_count;
extern volatile bool discovery_mode;

extern uint8_t  sel_mac[6];
extern uint32_t sel_room;
extern uint8_t  sel_channel;
extern uint32_t sel_stream_id;
extern volatile bool selected;

extern rx_slot_t rx_slots[RX_SLOTS];
extern QueueHandle_t free_q;
extern QueueHandle_t full_q;

extern volatile uint16_t last_seq_rx;
extern volatile uint32_t last_audio_rx_us;

extern volatile uint32_t stat_decoded;
extern volatile uint32_t stat_plc;
extern volatile uint32_t stat_dropped;
extern volatile uint8_t adaptive_target_fill;
extern volatile uint8_t adaptive_packet_wait_ms;

extern volatile int32_t  latency_raw_us;
extern volatile int32_t  latency_smooth_us;
extern volatile int32_t  clock_offset_us;
extern volatile int32_t  capture_clock_offset_us;
extern volatile uint32_t clock_sync_count;
extern volatile int32_t  playout_err_us;
extern volatile int32_t  rx_jitter_us;
extern volatile uint8_t  burst_guard_s;

extern volatile bool request_stream_reset;
extern volatile bool request_rejoin;
extern volatile uint32_t request_rejoin_at_us;

extern lc3_decoder_mem_48k_t lc3_dec_mem[CHANNELS];
extern lc3_decoder_t lc3_dec[CHANNELS];
extern i2s_chan_handle_t i2s_tx;

#define UART_NUM              UART_NUM_0
#define UART_BUF_SIZE         256

void add_or_update_room(const uint8_t mac[6], const beacon_msg_t *b, int8_t rssi);
int uart_read_line(char *buf, int max_len);
void flush_full_queue(void);
int32_t iabs32(int32_t v);

void wifi_init_espnow(void);
void add_peer_unicast(const uint8_t mac[6], uint8_t channel);
void sync_selected_peer(void);
void espnow_recv_cb(const esp_now_recv_info_t *ri, const uint8_t *data, int len);
void scan_for_rooms(void);
void room_select_task(void *arg);
void keepalive_task(void *arg);

void playback_task(void *arg);
void stats_task(void *arg);

static inline void decode_room_code(uint32_t room_code, uint16_t *building, uint16_t *room, uint8_t *suffix)
{
    if (building) {
        *building = (uint16_t)((room_code >> 15) & 0x3FFU);
    }
    if (room) {
        *room = (uint16_t)((room_code >> 5) & 0x3FFU);
    }
    if (suffix) {
        *suffix = (uint8_t)(room_code & 0x1FU);
    }
}

void sink_core_main(void);
