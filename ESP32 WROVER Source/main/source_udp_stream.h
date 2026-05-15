#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOURCE_UDP_MULTICAST_ADDR "239.10.10.10"
#define SOURCE_UDP_QUEUE_DEPTH 8
#define SOURCE_UDP_MAX_PAYLOAD 400

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t room_hash;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t seq;
    uint32_t capture_us;
    uint32_t payload_crc32;
    uint8_t copy_idx;
    uint8_t copy_count;
    uint16_t frame_bytes;
    uint8_t channels;
    uint8_t payload[SOURCE_UDP_MAX_PAYLOAD];
} source_udp_audio_packet_t;

typedef struct {
    uint32_t packets_sent;
    uint32_t queue_level;
    uint32_t queue_drops;
    uint32_t send_errors;
} source_udp_stats_t;

esp_err_t source_udp_stream_start(void);
void source_udp_stream_set_port(uint16_t port);
void source_udp_stream_submit(const source_udp_audio_packet_t *pkt, size_t payload_len);
void source_udp_stream_get_stats(source_udp_stats_t *out);

#ifdef __cplusplus
}
#endif
