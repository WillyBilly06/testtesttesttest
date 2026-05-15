#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "source_room_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOURCE_UDP_MAX_AUTH_CLIENTS 4

typedef struct {
    uint32_t stream_id;
    uint32_t session_id;
    uint32_t latest_seq;
    uint16_t frame_bytes;
    uint8_t channels;
    uint8_t key[RAUD_KEY_LEN];
} source_udp_session_snapshot_t;

typedef struct {
    uint32_t addr;
    uint16_t port;
} source_udp_audio_dest_t;

typedef struct {
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    char client_name[RAUD_CLIENT_NAME_LEN];
    bool enabled;
    uint32_t added_ms;
    uint32_t last_seen_ms;
} source_authorized_client_view_t;

typedef struct {
    uint32_t advertisements_sent;
    uint32_t pair_accepts;
    uint32_t pair_rejects;
    uint32_t join_accepts;
    uint32_t join_rejects;
    uint32_t auth_failures;
    uint32_t active_clients;
} source_udp_control_stats_t;

esp_err_t source_udp_control_start(void);
void source_udp_control_set_room_metadata(const char *room_code, const char *room_name);
void source_udp_control_get_source_id(uint8_t out[RAUD_SOURCE_ID_LEN]);
int source_udp_control_get_audio_dests(source_udp_audio_dest_t *out, int max_dests);

esp_err_t source_udp_control_enable_pairing(uint32_t duration_ms);
void source_udp_control_disable_pairing(void);
bool source_udp_control_get_pairing_pin(char out[7], uint32_t *remaining_ms);
int source_udp_control_get_authorized_clients(source_authorized_client_view_t *out, int max_clients);
esp_err_t source_udp_control_revoke_client(const uint8_t client_id[RAUD_CLIENT_ID_LEN]);
void source_udp_control_get_stats(source_udp_control_stats_t *out);

/* Implemented by source_main.c so control/auth never reaches into audio internals. */
esp_err_t source_audio_get_udp_session(source_udp_session_snapshot_t *out);

#ifdef __cplusplus
}
#endif
