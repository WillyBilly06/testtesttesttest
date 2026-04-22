/* EspCastBR source-side broadcaster — public API
 *
 * Call ecast_source_init() once after ESP-NOW is up, then feed encoded
 * LC3 frames from the capture task via ecast_source_publish_audio().
 *
 * The module owns:
 *   - the beacon emitter task
 *   - the RTN TX scheduler task
 *   - the internal queue carrying ready-to-send audio frames
 *   - AES-CCM session key state
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ecast_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure + start the broadcaster. Safe to call exactly once after
 * esp_now_init(). Returns true on success. */
bool ecast_source_init(uint64_t broadcast_id,
                       uint32_t stream_id_full,
                       const char *room_name,
                       const uint8_t broadcast_code[16],
                       bool enable_encryption);

/* Feed one LC3-encoded stereo frame from the capture task.
 *   lc3_payload : ECAST_AUDIO_BYTES bytes (144 B stereo LC3)
 *   capture_us  : source monotonic-clock time the frame was captured
 *
 * The call is non-blocking; the frame is enqueued for the scheduler
 * which emits RTN copies at SUB_INTERVAL_US spacing. */
void ecast_source_publish_audio(const uint8_t *lc3_payload,
                                uint32_t       capture_us);

/* Refresh the channel advertised in beacons (call when STA changes channel). */
void ecast_source_set_channel(uint8_t channel);

/* Runtime counters (for stats task) */
extern volatile uint32_t ecast_tx_ok;     /* successful copy TXs */
extern volatile uint32_t ecast_tx_fail;   /* failed copy TXs */
extern volatile uint32_t ecast_tx_frames; /* distinct audio frames published */
extern volatile uint32_t ecast_tx_dropped;/* frames dropped due to full queue */

#ifdef __cplusplus
}
#endif
