#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t espnow_fragments_sent;
    uint32_t espnow_frames_sent;
    uint32_t espnow_queue_level;
    uint32_t espnow_queue_drops;
    uint32_t espnow_fragment_drops;
    uint32_t espnow_send_errors;
    uint32_t audio_scheduler_deadline_misses;
    uint32_t allocation_failures;
} source_audio_stats_t;

void source_audio_get_stats(source_audio_stats_t *out);

#ifdef __cplusplus
}
#endif
