/*
 * ESP-NOW Audio Sink for CrowPanel P4
 *
 * P4-side component that communicates with C6 coprocessor via ESP-Hosted
 * custom data channel (SDIO). All ESP-NOW/WiFi runs on C6.
 * P4 sends commands, receives events + decoded PCM audio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espnow_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Room information (same fields as espnow_evt_room_t but with extra metadata) */
typedef struct {
    uint8_t mac[6];
    uint8_t wifi_channel;
    uint32_t room_code;
    uint32_t stream_id;
    int8_t rssi;
    bool valid;
} espnow_room_info_t;

static inline void espnow_decode_room_code(uint32_t room_code, uint16_t *building, uint16_t *room, uint8_t *suffix)
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

#define ESPNOW_SINK_MAX_ROOMS ESPNOW_MAX_ROOMS

/**
 * @brief Callback for audio data received from C6
 * @param pcm  Interleaved 16-bit stereo PCM samples
 * @param samples  Number of samples per channel
 * @param seq  Sequence number
 */
typedef void (*espnow_sink_audio_cb_t)(const int16_t *pcm, int samples, uint16_t seq);

/**
 * @brief Callback for state changes
 * @param state  Current state
 */
typedef void (*espnow_sink_state_cb_t)(espnow_state_t state);

/**
 * @brief Callback for room discovery during scan
 * @param room  Room information
 */
typedef void (*espnow_sink_room_cb_t)(const espnow_room_info_t *room);

/**
 * @brief Callback for scan complete
 * @param room_count  Number of rooms found
 */
typedef void (*espnow_sink_scan_done_cb_t)(int room_count);

/**
 * @brief Callbacks configuration
 */
typedef struct {
    espnow_sink_audio_cb_t on_audio;         /* PCM audio received */
    espnow_sink_state_cb_t on_state_change;  /* State changed */
    espnow_sink_room_cb_t on_room_found;     /* Room discovered */
    espnow_sink_scan_done_cb_t on_scan_done; /* Scan complete */
} espnow_sink_callbacks_t;

/**
 * @brief Initialize ESP-NOW sink (registers SDIO event handlers)
 * @param cbs  Optional callbacks (can be NULL, set later with espnow_sink_set_callbacks)
 * @return ESP_OK on success
 */
esp_err_t espnow_sink_init(const espnow_sink_callbacks_t *cbs);

/**
 * @brief Set/update callbacks
 */
void espnow_sink_set_callbacks(const espnow_sink_callbacks_t *cbs);

/**
 * @brief Deinitialize ESP-NOW sink
 */
void espnow_sink_deinit(void);

/**
 * @brief Tell C6 to initialize ESP-NOW + WiFi
 * @return ESP_OK if command sent successfully
 */
esp_err_t espnow_sink_enable(void);

/**
 * @brief Enable or disable the autoscan/autoconnect state machine.
 *
 * When enabled, a dedicated task owns the lifecycle: it sends CMD_INIT to
 * the C6 if needed, then loops scan → auto-join highest-RSSI room →
 * monitor → on disconnect reconnect up to 10 × 500 ms → on giveup rescan.
 * UI code must not call espnow_sink_enable/disable/start_scan/join_room
 * while autoscan is on; it should only call this function.
 *
 * @param enable  true to start the state machine, false to stop it.
 */
void espnow_sink_set_autoscan(bool enable);

/**
 * @brief Query whether autoscan is currently enabled.
 */
bool espnow_sink_get_autoscan(void);

/**
 * @brief Tell C6 to deinitialize ESP-NOW
 * @return ESP_OK if command sent successfully
 */
esp_err_t espnow_sink_disable(void);

/**
 * @brief Start scanning for ESP-NOW audio sources (rooms)
 * @return ESP_OK on success
 */
esp_err_t espnow_sink_start_scan(void);

/**
 * @brief Stop scanning for rooms
 */
void espnow_sink_stop_scan(void);

/**
 * @brief Get list of discovered rooms
 * @param rooms Output array of room info
 * @param max_rooms Maximum number of rooms to return
 * @return Number of rooms found
 */
int espnow_sink_get_rooms(espnow_room_info_t *rooms, int max_rooms);

/**
 * @brief Join a specific room (start receiving audio)
 * @param room_index Index of the room from get_rooms()
 * @return ESP_OK on success
 */
esp_err_t espnow_sink_join_room(int room_index);

/**
 * @brief Leave the current room (stop receiving audio)
 * @return true if C6 was responsive (LEAVE command sent), false if C6 appears dead (SDIO stale)
 */
bool espnow_sink_leave_room(void);

/**
 * @brief Check if currently connected to a room
 * @return true if connected
 */
bool espnow_sink_is_connected(void);

/**
 * @brief Get current state
 */
espnow_state_t espnow_sink_get_state(void);

/**
 * @brief Check if C6 ESP-NOW is initialized
 */
bool espnow_sink_is_c6_ready(void);

/**
 * @brief Get current playback status
 * @param packets_rx Output: packets received (can be NULL)
 * @param packets_lost Output: packets lost (can be NULL)
 */
void espnow_sink_get_stats(uint32_t *packets_rx, uint32_t *packets_lost);

/**
 * @brief Request status update from C6
 */
void espnow_sink_request_status(void);

/**
 * @brief Request firmware version from C6.
 */
void espnow_sink_request_fw_version(void);

/**
 * @brief Get last firmware version reported by C6.
 * @param out Output buffer.
 * @param out_len Output buffer length.
 * @return true if a valid version has been received from C6.
 */
bool espnow_sink_get_c6_fw_version(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
