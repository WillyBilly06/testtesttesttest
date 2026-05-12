/*
 * Shared SDIO protocol between the P4 host and C6 coprocessor.
 *
 * C6 owns ESP-NOW room discovery/authentication and ESP-NOW audio receive.
 * P4 decodes LC3 audio and plays it over I2S.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* P4 -> C6 commands */
#define ESPNOW_MSG_CMD_INIT          0x100
#define ESPNOW_MSG_CMD_DEINIT        0x101
#define ESPNOW_MSG_CMD_SCAN          0x102
#define ESPNOW_MSG_CMD_STOP_SCAN     0x103
#define ESPNOW_MSG_CMD_JOIN          0x104
#define ESPNOW_MSG_CMD_LEAVE         0x105
#define ESPNOW_MSG_CMD_GET_STATUS    0x106
#define ESPNOW_MSG_CMD_GET_FW_VER    0x107

/* C6 -> P4 events */
#define ESPNOW_MSG_EVT_STATUS        0x200
#define ESPNOW_MSG_EVT_ROOM_FOUND    0x201
#define ESPNOW_MSG_EVT_SCAN_DONE     0x202
#define ESPNOW_MSG_EVT_JOINED        0x203
#define ESPNOW_MSG_EVT_LEFT          0x204
#define ESPNOW_MSG_EVT_AUDIO         0x205
#define ESPNOW_MSG_EVT_STATS         0x206
#define ESPNOW_MSG_EVT_ERROR         0x207
#define ESPNOW_MSG_EVT_FW_VER        0x208
#define ESPNOW_MSG_EVT_AUDIO_CONFIG  0x209  /* payload: espnow_evt_audio_config_t */
#define ESPNOW_MSG_EVT_AUDIO_KEY     0x20A  /* payload: espnow_evt_audio_key_t */

#define ESPNOW_SAMPLE_RATE_HZ        48000
#define ESPNOW_CHANNELS              2
#define ESPNOW_BITS_PER_SAMPLE       24
#define ESPNOW_LC3_FRAME_US          7500
#define ESPNOW_LC3_BYTES_PER_CH      72
#define ESPNOW_LC3_FRAME_BYTES       (ESPNOW_LC3_BYTES_PER_CH * ESPNOW_CHANNELS)
#define ESPNOW_SAMPLES_PER_FRAME     ((ESPNOW_SAMPLE_RATE_HZ * ESPNOW_LC3_FRAME_US) / 1000000)

#define ESPNOW_MAX_ROOMS             16
#define ESPNOW_SCAN_LISTEN_MS        1500
#define ESPNOW_ROOM_NAME_LEN         9
#define ESPNOW_AUDIO_KEY_LEN         32
#define ESPNOW_AUDIO_MAX_FRAME_BYTES ESPNOW_LC3_FRAME_BYTES
#define ESPNOW_AUDIO_COPY_DEFAULT    4

typedef enum {
    ESPNOW_STATE_NOT_INIT = 0,
    ESPNOW_STATE_IDLE,
    ESPNOW_STATE_SCANNING,
    ESPNOW_STATE_JOINING,
    ESPNOW_STATE_CONNECTED,
    ESPNOW_STATE_ERROR,
} espnow_state_t;

/* ESPNOW_MSG_CMD_JOIN */
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint8_t  wifi_channel;
    uint32_t room_code;
    uint32_t stream_id;
} espnow_cmd_join_t;

/* ESPNOW_MSG_EVT_STATUS */
typedef struct __attribute__((packed)) {
    uint8_t  state;
    uint8_t  wifi_init;
    uint8_t  espnow_init;
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
    char     name[ESPNOW_ROOM_NAME_LEN];
    uint8_t  frame_bytes;
    uint8_t  reserved2[7];
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

/* ESPNOW_MSG_EVT_AUDIO */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t capture_us;
    uint32_t stream_id;
    uint32_t payload_crc32;
    uint8_t  copy_idx;
    uint8_t  copy_count;
    uint8_t  frame_bytes;
    uint8_t  channels;
    uint16_t lc3_dt_us;
    uint16_t sample_rate_hz;
    uint8_t  payload[ESPNOW_AUDIO_MAX_FRAME_BYTES];
} espnow_evt_audio_raw_t;

/* ESPNOW_MSG_EVT_STATS */
typedef struct __attribute__((packed)) {
    uint32_t packets_rx;
    uint32_t packets_lost;
    uint32_t plc_frames;
    int8_t   rssi_last;
    uint8_t  wifi_channel;
    uint8_t  sdio_send_errors;
    uint8_t  reserved;
} espnow_evt_stats_t;

/* ESPNOW_MSG_EVT_ERROR */
typedef struct __attribute__((packed)) {
    int32_t  error_code;
    char     message[64];
} espnow_evt_error_t;

/* ESPNOW_MSG_EVT_FW_VER */
typedef struct __attribute__((packed)) {
    char     version[32];
    char     project[32];
} espnow_evt_fw_ver_t;

/* ESPNOW_MSG_EVT_AUDIO_CONFIG */
typedef struct __attribute__((packed)) {
    uint8_t  audio_copy_count;
    uint8_t  frame_bytes;
    uint8_t  channels;
    uint8_t  reserved;
    uint16_t lc3_dt_us;
    uint16_t sample_rate_hz;
} espnow_evt_audio_config_t;

/* ESPNOW_MSG_EVT_AUDIO_KEY — C6 sends the AES audio key to P4 after ACCEPT. */
typedef struct __attribute__((packed)) {
    uint8_t  audio_key[ESPNOW_AUDIO_KEY_LEN];
    uint8_t  frame_bytes;
    uint8_t  channels;
    uint16_t lc3_dt_us;
    uint16_t sample_rate_hz;
} espnow_evt_audio_key_t;


#ifdef __cplusplus
}
#endif
