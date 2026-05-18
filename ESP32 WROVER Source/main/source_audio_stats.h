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
    /* Non-fatal radio backpressure: send completion CB was briefly
     * delayed and the in-flight token semaphore ran dry. RTN=4
     * tolerates this — every event here just means one copy of one
     * frame was skipped; the other 3 still ship. */
    uint32_t tx_token_backpressure;
    /* PCM block could not be obtained when the I2S reader needed one
     * (encoder is far behind) or the encoder had to skip a cycle. */
    uint32_t pcm_underruns;
} source_audio_stats_t;

void source_audio_get_stats(source_audio_stats_t *out);

#ifdef __cplusplus
}
#endif
