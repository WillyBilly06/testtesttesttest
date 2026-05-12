// MY_NOTE: I split this file out so it's easier for me to tune and debug quickly.
#pragma once

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_http_server.h"


#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

#include "lc3.h"

extern const char *TAG;

#define SAMPLE_RATE_HZ          48000
#define CHANNELS               2
#define LC3_FRAME_US           5000
#define LC3_BYTES_PER_CH       48
#define SAMPLES_PER_FRAME      ((SAMPLE_RATE_HZ * LC3_FRAME_US) / 1000000)

#define PCM24_MAX              8388607
#define PCM24_MIN             -8388608

/* PCM1808 I2S-in pins are defined locally in audio/source_audio.c:
 *   MCLK=GPIO0  BCLK=GPIO27  LRCK=GPIO25  DIN=GPIO26  (ESP32 = master) */

#define ROOM_BUILDING          52    /* 000..999 */
#define ROOM_NUMBER            127   /* 000..999 */
#define ROOM_SUFFIX            0     /* 0 = none, 1..26 = A..Z */
#define PACK_ROOM_CODE(b, r, s) \
    ((((uint32_t)(b) & 0x3FFU) << 15) | (((uint32_t)(r) & 0x3FFU) << 5) | ((uint32_t)(s) & 0x1FU))
#define ROOM_CODE              PACK_ROOM_CODE(ROOM_BUILDING, ROOM_NUMBER, ROOM_SUFFIX)
#define WIFI_CHANNEL_DEFAULT   6
#define WIFI_TX_POWER_QDBM     84
#define BEACON_PERIOD_MS       400
#define MAX_SINKS              32   /* Lifted from 10: all-app-layer crypto removes ESP-NOW 17-peer hw limit */

#define PROV_AP_SSID           "ALS_SETUP"
#define PROV_AP_PASSWORD       ""
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_MAX_RETRY         20
#define WIFI_RECONNECT_BASE_MS 250
#define WIFI_RECONNECT_MAX_MS  5000
#define WIFI_CRED_NAMESPACE    "wifi_cfg"
#define WIFI_CRED_KEY_SSID     "ssid"
#define WIFI_CRED_KEY_PASS     "pass"

#define SOURCE_AP_SSID         "ALS_TX"
#define SOURCE_AP_PASSWORD     "openals123"

#ifndef CONFIG_SOFTAP_SSID
#define CONFIG_SOFTAP_SSID SOURCE_AP_SSID
#endif

#ifndef CONFIG_SOFTAP_PASSWORD
#define CONFIG_SOFTAP_PASSWORD SOURCE_AP_PASSWORD
#endif

#ifndef CONFIG_SOURCE_AP_IP_C
#define CONFIG_SOURCE_AP_IP_C 50
#endif

#ifndef CONFIG_AUDIO_QUEUE_LEN
#define CONFIG_AUDIO_QUEUE_LEN 12
#endif

#ifndef CONFIG_AUDIO_TX_TOKENS
/* 48 tokens (vs 16) absorbs transient espnow_send_cb stalls that otherwise
 * caused long audio dropouts. At 200 pps the pool only ever has <2
 * in-flight on a healthy link; 48 gives ~240 ms of headroom before any
 * backpressure kicks in. */
#define CONFIG_AUDIO_TX_TOKENS 48
#endif

#ifndef CONFIG_AUDIO_TASK_PRIO_CAP_ENC
#define CONFIG_AUDIO_TASK_PRIO_CAP_ENC 7
#endif

#ifndef CONFIG_AUDIO_TASK_PRIO_SEND
#define CONFIG_AUDIO_TASK_PRIO_SEND 8
#endif

#define SOURCE_AP_IP_A         192
#define SOURCE_AP_IP_B         168
#define SOURCE_AP_IP_C         CONFIG_SOURCE_AP_IP_C
#define SOURCE_AP_IP_D         1

#define AUDIO_QUEUE_LEN         CONFIG_AUDIO_QUEUE_LEN
#define AUDIO_TX_TOKENS         CONFIG_AUDIO_TX_TOKENS
/* 10 ms wait lets legacy unicast ride out short WiFi callback stalls.
 * The ECast broadcast path is separately paced and non-blocking. */
#define AUDIO_TX_TOKEN_WAIT_MS  10

/* Audio TX uses ESP-NOW UNICAST fanout (one esp_now_send per joined sink).
 * Unicast gets automatic 802.11 ACK + MAC-level retry (4-7 retries) for free,
 * so app-layer duplication is not needed. Keep this at 1 unless you want to
 * layer extra belts-and-braces redundancy on top of MAC retries. */
#define AUDIO_TX_REDUNDANCY     1
#define AUDIO_TX_DUP_GAP_US     2500

#define TASK_PRIO_CAP_ENC       CONFIG_AUDIO_TASK_PRIO_CAP_ENC
#define TASK_PRIO_SEND          CONFIG_AUDIO_TASK_PRIO_SEND
#define TASK_PRIO_UDP_SEND      4
#define UDP_AUDIO_QUEUE_LEN     2

#define UDP_PACE_DIV_NORMAL     2
#define UDP_PACE_DIV_STRONG     4

/* ── ESP-NOW PHY: 802.11n HT20, fixed MCS1 LGI ────────────────────────
 *
 * MCS1 HT20 long-GI = 13 Mbps PHY rate. Compared to legacy 24 Mbps OFDM:
 *   - ~3 dB more sensitivity → roughly 1.4× free-space range.
 *   - Same 20 MHz channel width, so no co-existence change with the
 *     STA's upstream AP.
 *   - With RTN=3 + AES-CCM payload (117 B on-air), each copy uses ~170 us
 *     of air time, and the 3-copy burst fits inside the 5 ms LC3plus slot.
 *
 * The whole-band rate fall-back logic that lived in apply_espnow_rate_all_peers
 * is intentionally a no-op now — we want a *fixed* PHY for predictable
 * range/latency. */
#define ESPNOW_PHY_MODE         WIFI_PHY_MODE_HT20
#define ESPNOW_PHY_RATE         WIFI_PHY_RATE_MCS1_LGI
#define ESPNOW_PHY_RATE_MAX     WIFI_PHY_RATE_MCS1_LGI

/* Legacy alias kept so any pre-existing rate-step code keeps compiling.
 * On a fixed-rate config every step "stays" at MCS1 LGI. */
#ifndef WIFI_PHY_RATE_54M
#define WIFI_PHY_RATE_54M WIFI_PHY_RATE_MCS1_LGI
#endif
#define ESPNOW_HAS_RATE_54M     0

#define UDP_PORT               5000
#define MAX_UDP_CLIENTS        10
#define UDP_CLIENT_TIMEOUT_US  5000000UL

#define SINK_SEND_FAIL_MAX_STREAK  40

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
    uint8_t mac[6];
    bool in_use;
    uint32_t last_seen_us;
    uint8_t send_fail_streak;
} sink_peer_t;

typedef struct {
    struct sockaddr_in addr;
    bool in_use;
    uint32_t last_seen_us;
} udp_client_t;

extern lc3_encoder_mem_48k_t lc3_enc_mem[CHANNELS];
extern lc3_encoder_t lc3_enc[CHANNELS];
extern uint32_t stream_id;
extern uint16_t seq_num;
extern sink_peer_t sinks[MAX_SINKS];
extern QueueHandle_t audio_q;
extern QueueHandle_t udp_audio_q;
extern SemaphoreHandle_t tx_tokens;
extern const uint8_t BROADCAST_MAC[6];
extern uint8_t wifi_channel;
extern EventGroupHandle_t wifi_ev;
extern int wifi_retry;
extern esp_netif_t *wifi_sta_netif;
extern esp_netif_t *wifi_ap_netif;
extern bool napt_enabled;
extern bool streaming_mode_active;
extern volatile uint32_t sta_disconnect_count;
extern volatile uint32_t sta_reconnect_count;
extern volatile uint32_t sta_last_drop_us;
extern volatile uint32_t sta_last_recover_ms;
extern TimerHandle_t sta_reconnect_timer;
extern uint32_t sta_reconnect_backoff_ms;
extern wifi_phy_rate_t espnow_rate_current;
extern volatile uint32_t espnow_send_ok;
extern volatile uint32_t espnow_send_fail;
extern volatile uint32_t espnow_token_drop;
extern volatile bool radio_congested;
extern volatile bool provision_done;
extern udp_client_t udp_clients[MAX_UDP_CLIENTS];
extern int udp_sock;
extern volatile uint32_t stat_src_drop_espnow_q;
extern volatile uint32_t stat_src_drop_udp_q;
extern volatile uint32_t stat_udp_paced_drop;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

int find_sink_slot_by_mac(const uint8_t mac[6]);
int alloc_sink_slot(const uint8_t mac[6]);
void remove_sink_slot(const uint8_t mac[6]);
int count_active_sinks(void);
int count_active_udp_clients(void);
int32_t clamp_pcm24(int32_t v);
int32_t abs_i32(int32_t v);

void load_or_start_provisioning(char *sta_ssid, size_t ssid_len, char *sta_pass, size_t pass_len);
void wifi_start_apsta(const char *ssid, const char *password);
void start_provisioning_portal(void);
void wifi_init_espnow(void);
void add_peer_if_needed(const uint8_t mac[6]);
void ensure_napt_enabled(void);

const char *espnow_rate_name(wifi_phy_rate_t rate);
wifi_phy_rate_t espnow_rate_step_down(wifi_phy_rate_t rate);
wifi_phy_rate_t espnow_rate_step_up(wifi_phy_rate_t rate);
void apply_espnow_rate_all_peers(wifi_phy_rate_t rate);

void espnow_send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status);
void espnow_recv_cb(const esp_now_recv_info_t *ri, const uint8_t *data, int len);

void beacon_task(void *arg);
void audio_capture_encode_task(void *arg);
void audio_emit_task(void *arg);
void audio_send_task(void *arg);
void udp_audio_send_task(void *arg);
void udp_server_task(void *arg);
void source_stats_task(void *arg);

void source_core_main(void);
