/*
 * sbc_decoder_native — thin C wrapper over google/libsbc.
 *
 * Exports two cdecl entry points consumed by the C# `SbcDecoder` class:
 *
 *   int sbc_decoder_probe(void);
 *       Returns 0. Used by the managed side only to verify the DLL loads
 *       and the entry point is reachable.
 *
 *   int sbc_decode_frame(const uint8_t *input, int input_len,
 *                        int16_t *output, int max_samples);
 *       Decodes one or more concatenated SBC frames found in `input` into
 *       interleaved 16-bit L/R PCM in `output`. Returns the total number
 *       of int16 samples written (== blocks * subbands * channels per frame
 *       summed over all decoded frames), or -1 on error.
 *
 * Built as a Windows DLL with cdecl exports; the C# DllImport attributes
 * load it as `sbc_decoder_native.dll`.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sbc.h"

#if defined(_WIN32)
  #define SBCN_EXPORT __declspec(dllexport)
#else
  #define SBCN_EXPORT __attribute__((visibility("default")))
#endif

/*
 * A single shared decoder context is kept inside the wrapper. The managed
 * caller decodes packets serially from a single audio thread, so this is
 * safe and matches how the encoder side maintains one context per stream.
 *
 * The libsbc decoder state must persist across frames because the synthesis
 * filter keeps a ring buffer of past subband samples; resetting between
 * every UDP packet would introduce audible filter clicks at packet
 * boundaries.
 */
static sbc_t g_sbc;
static int   g_initialized = 0;

static void ensure_initialized(void)
{
    if (!g_initialized) {
        sbc_reset(&g_sbc);
        g_initialized = 1;
    }
}

SBCN_EXPORT int sbc_decoder_probe(void)
{
    ensure_initialized();
    return 0;
}

SBCN_EXPORT int sbc_decode_frame(const uint8_t *input, int input_len,
                                 int16_t *output, int max_samples)
{
    if (!input || input_len <= 0 || !output || max_samples <= 0) {
        return -1;
    }
    ensure_initialized();

    int total_samples = 0;
    int offset        = 0;

    while (offset < input_len) {
        int remaining = input_len - offset;
        if (remaining < SBC_PROBE_SIZE) {
            break;
        }

        struct sbc_frame frame;
        if (sbc_probe(input + offset, &frame) < 0) {
            /* Junk byte at offset — try to resync by skipping it. */
            offset += 1;
            continue;
        }

        unsigned frame_size = sbc_get_frame_size(&frame);
        if (frame_size == 0 || (int)frame_size > remaining) {
            break;
        }

        int nchannels = (frame.mode == SBC_MODE_MONO) ? 1 : 2;
        int samples_per_channel = frame.nblocks * frame.nsubbands;
        int frame_samples = samples_per_channel * nchannels;

        if (total_samples + frame_samples > max_samples) {
            /* Output buffer too small for the next frame — return what we
             * have so the caller can size up next time. */
            break;
        }

        /* libsbc writes L and R into separate buffers with a configurable
         * pitch (stride). Decode directly into the interleaved output by
         * pointing L and R at adjacent slots with stride == nchannels. */
        int16_t *base = output + total_samples;
        int16_t *pcml = base;
        int16_t *pcmr = (nchannels == 2) ? base + 1 : NULL;
        int      pitchl = nchannels;
        int      pitchr = nchannels;

        if (sbc_decode(&g_sbc, input + offset, frame_size, &frame,
                       pcml, pitchl, pcmr, pitchr) < 0) {
            /* Corrupt frame — skip past it and keep going so a single
             * lost packet does not stall the audio stream. */
            offset += (int)frame_size;
            continue;
        }

        total_samples += frame_samples;
        offset        += (int)frame_size;
    }

    return total_samples;
}

SBCN_EXPORT void sbc_decoder_reset(void)
{
    sbc_reset(&g_sbc);
    g_initialized = 1;
}
