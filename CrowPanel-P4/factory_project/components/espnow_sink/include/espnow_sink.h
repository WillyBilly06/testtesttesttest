/*
 * Assistive Listening Sink for CrowPanel P4
 *
 * P4-side component that communicates with C6 coprocessor via ESP-Hosted
 * custom data channel (SDIO). The C6 owns ESP-NOW room discovery and
 * authentication and receives ESP-NOW audio; the P4 decodes LC3 audio from
 * C6-forwarded SDIO events and plays it.
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

/* Room information. The first fields are used by the existing Settings UI;
 * the remaining fields are retained for ABI/source compatibility with older
 * EspCast builds and are zeroed by the room-verification path.
 */
typedef struct {
    uint8_t  mac[6];
    uint8_t  wifi_channel;
    uint32_t room_code;
    uint32_t stream_id;
    int8_t   rssi;
    bool     valid;

    /* EspCastBR extension fields (zeroed for legacy beacons) */
    char     name[24];              /* human-readable room name */
    uint64_t broadcast_id;
    uint8_t  enc_iv[8];
    uint8_t  key_diversifier[8];
    uint8_t  session_key[16];
    bool     is_encrypted;
    bool     session_key_valid;
    uint16_t pres_delay_us;
    int32_t  clock_offset_us;
    uint32_t clock_sync_count;
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
 * @brief Optional decoded-audio callback
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
 * @brief Tell C6 to initialize ESP-NOW room verification
 * @return ESP_OK if command sent successfully
 */
esp_err_t espnow_sink_enable(void);

/**
 * @brief Enable or disable the autoscan/autoconnect state machine.
 *
 * When enabled, a dedicated task owns the lifecycle: it sends CMD_INIT to
 * the C6 if needed, then keeps scanning so the room list stays fresh. It does
 * not auto-join unless autojoin is explicitly enabled. A user-selected room is
 * automatically retried for the source-timeout window if audio drops.
 *
 * @param enable  true to start the state machine, false to stop it.
 */
void espnow_sink_set_autoscan(bool enable);

/**
 * @brief Query whether autoscan is currently enabled.
 */
bool espnow_sink_get_autoscan(void);

/**
 * @brief Enable or disable auto-join behaviour inside the autoscan task.
 *
 * When auto-join is true, the autoscan task will join the highest-RSSI room it
 * discovers. The Settings UI keeps this false so the user picks a room. When false, it keeps the room list
 * fresh but never joins on its own — the user must call
 * espnow_sink_join_room() explicitly. Drop-recovery (rejoining the same
 * cached target after an audio drop) still works regardless of this
 * flag, so a user-initiated join is auto-recovered if it drops.
 */
void espnow_sink_set_autoscan_autojoin(bool autojoin);

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
 * @brief Set software output volume for direct assistive playback.
 *
 * This controls the P4-side PCM path used by the AUX DAC and mirrors the UI
 * volume when SPEAKER is selected.
 */
void espnow_sink_set_output_volume(int volume);

/**
 * @brief Write a short silence burst to the direct AUX I2S path.
 *
 * Useful before routing playback to SPEAKER so the PCM5102A line is not left
 * holding the last active samples.
 */
void espnow_sink_flush_aux_output(void);

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
 * @brief Get the room code of the currently connected room.
 * @return room code, or 0 if not connected.
 */
uint32_t espnow_sink_get_room_code(void);

/**
 * @brief Get the currently connected room information.
 * @param room Output room info.
 * @return true if connected room info was copied.
 */
bool espnow_sink_get_connected_room(espnow_room_info_t *room);

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
