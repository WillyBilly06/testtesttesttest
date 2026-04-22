/*
 * Shared ESP-NOW SDIO Protocol between P4 (host) and C6 (coprocessor)
 *
 * Communication uses ESP-Hosted custom peer data transfer API:
 *   esp_hosted_send_custom_data(msg_id, data, len)
 *   esp_hosted_register_custom_callback(msg_id, callback)
 *
 * All message IDs are uint32_t. Data is the raw struct payload.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*──────────────────────────────────────────────────────────────
 * Message IDs  (P4 → C6 = 0x100..0x1FF, C6 → P4 = 0x200..0x2FF)
 *────────────────────────────────────────────────────────────*/

/* P4 → C6 commands */
#define ESPNOW_MSG_CMD_INIT          0x100  /* Request ESP-NOW init on C6 */
#define ESPNOW_MSG_CMD_DEINIT        0x101  /* Request ESP-NOW deinit */
#define ESPNOW_MSG_CMD_SCAN          0x102  /* Start room scan */
#define ESPNOW_MSG_CMD_STOP_SCAN     0x103  /* Stop room scan */
#define ESPNOW_MSG_CMD_JOIN          0x104  /* Join a room (payload: espnow_cmd_join_t) */
#define ESPNOW_MSG_CMD_LEAVE         0x105  /* Leave current room */
#define ESPNOW_MSG_CMD_GET_STATUS    0x106  /* Request current status */
#define ESPNOW_MSG_CMD_GET_FW_VER    0x107  /* Request C6 app firmware version */

/* C6 → P4 events/data */
#define ESPNOW_MSG_EVT_STATUS        0x200  /* Status report (espnow_evt_status_t) */
#define ESPNOW_MSG_EVT_ROOM_FOUND    0x201  /* Room discovered during scan (espnow_evt_room_t) */
#define ESPNOW_MSG_EVT_SCAN_DONE     0x202  /* Scan complete (espnow_evt_scan_done_t) */
#define ESPNOW_MSG_EVT_JOINED        0x203  /* Successfully joined room (espnow_evt_joined_t) */
#define ESPNOW_MSG_EVT_LEFT          0x204  /* Left room */
#define ESPNOW_MSG_EVT_AUDIO         0x205  /* Raw LC3 audio data (espnow_evt_audio_raw_t) */
#define ESPNOW_MSG_EVT_STATS         0x206  /* Playback statistics (espnow_evt_stats_t) */
#define ESPNOW_MSG_EVT_ERROR         0x207  /* Error notification (espnow_evt_error_t) */
#define ESPNOW_MSG_EVT_FW_VER        0x208  /* Firmware version response (espnow_evt_fw_ver_t) */

/* ─── EspCastBR (Auracast-style) message IDs ──────────────────────── */
/* These carry RAW on-air ECast frames from C6 to P4 *after* authentication
 * (beacons) / deduplication (audio). P4 owns decryption + playout. */
#define ESPNOW_MSG_EVT_ECAST_BEACON   0x210  /* 84 B: ecast_hdr + beacon_payload + tag */
#define ESPNOW_MSG_EVT_ECAST_AUDIO    0x211  /* 165 B: ecast_hdr + ciphertext + MIC   */
/* Kept for future multi-channel scanning; MVP uses fixed channel 6. */
#define ESPNOW_MSG_CMD_ECAST_SET_CH   0x110  /* P4 → C6: set wifi channel, 1 B */
#define ESPNOW_MSG_EVT_ECAST_RSSI     0x212  /* C6 RSSI/rx stats (espnow_evt_stats_t) */

/*──────────────────────────────────────────────────────────────
 * ESP-NOW air protocol constants (must match source firmware)
 *────────────────────────────────────────────────────────────*/
#define ESPNOW_PROTO_MAGIC       0xA5
#define ESPNOW_SAMPLE_RATE_HZ    48000
#define ESPNOW_CHANNELS          2
#define ESPNOW_LC3_FRAME_US      7500
#define ESPNOW_LC3_BYTES_PER_CH  72
#define ESPNOW_SAMPLES_PER_FRAME ((ESPNOW_SAMPLE_RATE_HZ * ESPNOW_LC3_FRAME_US) / 1000000)  /* 360 */

#define ESPNOW_MAX_ROOMS         16
#define ESPNOW_SCAN_LISTEN_MS    300

/*──────────────────────────────────────────────────────────────
 * State enum
 *────────────────────────────────────────────────────────────*/
typedef enum {
    ESPNOW_STATE_NOT_INIT = 0,
    ESPNOW_STATE_IDLE,          /* Initialized, not scanning or connected */
    ESPNOW_STATE_SCANNING,      /* Currently scanning for rooms */
    ESPNOW_STATE_JOINING,       /* Connecting to a room */
    ESPNOW_STATE_CONNECTED,     /* Receiving audio from a room */
    ESPNOW_STATE_ERROR,
} espnow_state_t;

/*──────────────────────────────────────────────────────────────
 * P4 → C6 command payloads
 *────────────────────────────────────────────────────────────*/

/* ESPNOW_MSG_CMD_JOIN */
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint8_t  wifi_channel;
    uint32_t room_code;
    uint32_t stream_id;
} espnow_cmd_join_t;

/*──────────────────────────────────────────────────────────────
 * C6 → P4 event payloads
 *────────────────────────────────────────────────────────────*/

/* ESPNOW_MSG_EVT_STATUS */
typedef struct __attribute__((packed)) {
    uint8_t  state;             /* espnow_state_t */
    uint8_t  wifi_init;         /* 1 if WiFi initialized */
    uint8_t  espnow_init;       /* 1 if ESP-NOW initialized */
    uint8_t  reserved;
} espnow_evt_status_t;

/* ESPNOW_MSG_EVT_ROOM_FOUND */
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint8_t  wifi_channel;
    uint32_t room_code;
    uint32_t stream_id;
    int8_t   rssi;
    uint8_t  reserved[3];
} espnow_evt_room_t;

/* ESPNOW_MSG_EVT_SCAN_DONE */
typedef struct __attribute__((packed)) {
    uint8_t  room_count;
    uint8_t  reserved[3];
} espnow_evt_scan_done_t;

/* ESPNOW_MSG_EVT_JOINED */
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint8_t  wifi_channel;
    uint32_t room_code;
    uint32_t stream_id;
} espnow_evt_joined_t;

/* ESPNOW_MSG_EVT_AUDIO - Raw LC3 payload forwarded from C6 (P4 decodes) */
typedef struct __attribute__((packed)) {
    uint16_t seq;               /* Sequence number from source */
    uint16_t lost_before;       /* Number of lost packets before this one (for PLC) */
    uint32_t capture_us;        /* Source capture timestamp for clock sync */
    uint8_t  payload[ESPNOW_LC3_BYTES_PER_CH * ESPNOW_CHANNELS];  /* 144 bytes raw LC3 */
} espnow_evt_audio_raw_t;

/* ESPNOW_MSG_EVT_STATS */
typedef struct __attribute__((packed)) {
    uint32_t packets_rx;
    uint32_t packets_lost;
    uint32_t plc_frames;
    int8_t   rssi_last;
    uint8_t  wifi_channel;      /* Current WiFi channel on C6 */
    uint8_t  sdio_send_errors;  /* Count of failed esp_hosted_send_custom_data calls (saturates at 255) */
    uint8_t  reserved;
} espnow_evt_stats_t;

/* ESPNOW_MSG_EVT_ERROR */
typedef struct __attribute__((packed)) {
    int32_t  error_code;        /* esp_err_t */
    char     message[64];       /* Human-readable error text */
} espnow_evt_error_t;

/* ESPNOW_MSG_EVT_FW_VER */
typedef struct __attribute__((packed)) {
    char     version[32];       /* esp_app_desc_t.version from C6 firmware */
    char     project[32];       /* esp_app_desc_t.project_name */
} espnow_evt_fw_ver_t;

#ifdef __cplusplus
}
#endif
