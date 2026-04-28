/*
 * EspCastBR — Auracast-style broadcast audio over ESP-NOW
 *
 * This header is shared verbatim between:
 *   - esp-now-audio-source      (ESP32 broadcaster)
 *   - CrowPanel-P4 C6 firmware  (listener-side RF bridge)
 *   - CrowPanel-P4 P4 firmware  (audio decode/playout)
 *
 * Design (see project docs for full rationale):
 *   - All frames are ESP-NOW broadcast (no unicast peer mgmt, no ACK).
 *   - Reliability via scheduled redundancy: each audio frame is transmitted
 *     RTN times at SUB_INTERVAL_US spacing — BIS-style retransmission
 *     without feedback.
 *   - Presentation time is carried in every frame: sinks schedule playout
 *     at source_capture_us + PRES_DELAY_US (wall-clock on source's clock,
 *     converted to sink clock via EMA-tracked offset from beacons).
 *   - Encryption: AES-CCM with a session key derived from a user-shared
 *     16-byte Broadcast_Code, following the Auracast GSKD/GSK derivation
 *     pattern. Header is authenticated (AAD); payload is encrypted.
 *   - Discovery: unencrypted beacons carry the room metadata (name,
 *     stream_id, codec, IVs, channel) — anyone can see the room is there;
 *     only key holders can decode audio.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ─── Codec parameters ─────────────────────────────────────────────── */
#define ECAST_SAMPLE_RATE_HZ       48000
#define ECAST_CHANNELS             2
#define ECAST_LC3_FRAME_US         7500
#define ECAST_LC3_BYTES_PER_CH     72        /* 72 kbps per channel */
#define ECAST_SAMPLES_PER_FRAME    360       /* 7.5 ms × 48 kHz */
#define ECAST_AUDIO_BYTES          (ECAST_LC3_BYTES_PER_CH * ECAST_CHANNELS)

/* ─── Timing / redundancy ──────────────────────────────────────────── */
/* Number of copies of each audio frame transmitted (BIS RTN equivalent).
 * Spaced SUB_INTERVAL_US apart → redundancy window = (RTN-1)*SUB_INTERVAL_US.
 * Raise RTN for worse RF environments; airtime scales linearly.
 *
 * RTN=2 with 5 ms spacing matches Auracast BIG_Sync_Delay for Standard Quality
 * and gives ~5 ms redundancy window — survives single-packet bursts at 2.4 GHz
 * without wasting airtime. Bump to 3 in hostile RF; keep at 2 for low-latency. */
#define ECAST_RTN                  2
#define ECAST_SUB_INTERVAL_US      5000      /* 5 ms between copies */

/* Presentation delay: the sink plays every frame at
 *   local_time = source_capture_us + PRES_DELAY_US + clock_offset
 * All RTN copies of the frame must arrive before this deadline.
 * Must be ≥ (RTN-1)*SUB_INTERVAL_US + margin for RF + sink pipeline.
 *
 * At RTN=2 with 5 ms sub-interval the minimum is ~5 ms. 25 ms gives a 20 ms
 * margin for WiFi retry bursts and C6→P4 SDIO hop while keeping source-side
 * scheduling latency low. Total end-to-end target: 40-50 ms. */
#define ECAST_PRES_DELAY_US        25000     /* 25 ms */

/* Beacon emission period on the source */
#define ECAST_BEACON_PERIOD_MS     100

/* ─── Wire format ──────────────────────────────────────────────────── */
#define ECAST_MAGIC                0xACU     /* "AuraCast"-inspired */
#define ECAST_VERSION              1U
#define ECAST_MIC_LEN              4         /* AES-CCM tag, 32-bit */
#define ECAST_NONCE_LEN            13        /* AES-CCM L=2 → N=13 */

/* ver_type: top 4 bits = version, bottom 4 = frame type */
#define ECAST_TYPE_BEACON          0x1
#define ECAST_TYPE_AUDIO           0x2

#define ECAST_VER_TYPE(type)       ((uint8_t)(((ECAST_VERSION) << 4) | (type)))
#define ECAST_VER_OF(vt)           ((uint8_t)(((vt) >> 4) & 0x0FU))
#define ECAST_TYPE_OF(vt)          ((uint8_t)((vt) & 0x0FU))

/* Public frame header (authenticated, not encrypted).
 * Put first in every on-air frame so C6 can dedupe by psn BEFORE decrypt. */
typedef struct __attribute__((packed)) {
    uint8_t  magic;          /* ECAST_MAGIC */
    uint8_t  ver_type;       /* ECAST_VER_TYPE(type) */
    uint16_t stream_id16;    /* low 16 bits of stream_id_full (for fast dedupe) */
    uint32_t psn;            /* packet sequence number (also CCM nonce input) */
    uint32_t pres_time_us;   /* source monotonic time when sink plays this frame
                              * (for beacons: source's current monotonic time) */
    uint8_t  copy_idx;       /* 0..RTN-1 */
    uint8_t  rtn;            /* total copies being sent */
    uint16_t payload_len;    /* bytes of payload following (incl. MIC if encrypted) */
} ecast_hdr_t;

_Static_assert(sizeof(ecast_hdr_t) == 16, "ecast_hdr_t must be 16 bytes");

/* Beacon payload — plaintext so receivers can discover rooms.
 * Fixed size so the whole on-air frame is fixed length. */
typedef struct __attribute__((packed)) {
    uint64_t broadcast_id;          /* full 64-bit room id */
    uint32_t stream_id_full;        /* full stream_id (anti-collision) */
    uint16_t pres_delay_us;         /* 0..65535 µs (we use 20000) */
    uint16_t frame_us;              /* 7500 */
    uint16_t sample_rate_hz;        /* 48000 */
    uint8_t  channels;              /* 2 */
    uint8_t  lc3_bytes_per_ch;      /* 72 */
    uint8_t  wifi_channel;          /* channel the source is on right now */
    uint8_t  rtn;                   /* RTN currently in use */
    uint8_t  sub_interval_us_x100;  /* sub interval / 100µs (50 = 5 ms) */
    uint8_t  is_encrypted;          /* 0 or 1 */
    uint8_t  enc_iv[8];             /* nonce prefix (random per stream boot) */
    uint8_t  key_diversifier[8];    /* GSKD-equivalent (random per stream boot) */
    char     name[24];              /* human-readable UTF-8 room name, NUL-padded */
} ecast_beacon_payload_t;

_Static_assert(sizeof(ecast_beacon_payload_t) == 64,
               "beacon payload must be exactly 64 B");

/* Audio payload, PLAINTEXT form (pre-encryption) */
typedef struct __attribute__((packed)) {
    uint8_t  flags;                               /* reserved, must be 0 */
    uint8_t  lc3[ECAST_AUDIO_BYTES];              /* 144 bytes stereo LC3 */
} ecast_audio_plain_t;

_Static_assert(sizeof(ecast_audio_plain_t) == 1 + ECAST_AUDIO_BYTES,
               "audio plaintext must be 145 B");

/* Total on-air frame sizes
 *  - Beacon: header + payload + 4 B CMAC tag (auth, not encryption)
 *  - Audio:  header + 145 B plaintext + 4 B CCM MIC (authenticated encryption)
 */
#define ECAST_BEACON_FRAME_SIZE   (sizeof(ecast_hdr_t) + sizeof(ecast_beacon_payload_t) + ECAST_BEACON_TAG_LEN)  /* 84 B */
#define ECAST_AUDIO_FRAME_SIZE    (sizeof(ecast_hdr_t) + sizeof(ecast_audio_plain_t) + ECAST_MIC_LEN)             /* 165 B */

/* ─── Crypto constants ─────────────────────────────────────────────── */
/* AES-CMAC salt used to derive per-stream session key:
 *   SK = AES-CMAC(Broadcast_Code, key_diversifier || SALT)       (16 B)
 * where Broadcast_Code is the user-shared 16-byte secret. */
#define ECAST_SALT_LEN             8
#define ECAST_SALT_BYTES           { 'E','C','a','s','t','S','K','1' }

/* Beacon authentication salt — domain separation from session key derivation.
 * Beacons carry an AES-CMAC(Broadcast_Code, beacon_body || SALT_BEACON)
 * trailer so only sinks sharing the secret will accept the room. */
#define ECAST_SALT_BEACON_BYTES    { 'E','C','a','s','t','B','c','1' }
#define ECAST_BEACON_TAG_LEN       4    /* truncated CMAC, matches CCM MIC size */

/* Shared Broadcast_Code — mandatory. Supplied by ecast_secret.h which is
 * gitignored. Without it the build fails on purpose: no one can compile a
 * compatible receiver without access to your secret. Copy
 * ecast_secret.h.example to ecast_secret.h and fill in 16 random bytes. */
#include "ecast_secret.h"

#ifndef ECAST_BROADCAST_CODE_BYTES
#  error "ECAST_BROADCAST_CODE_BYTES not defined. Copy ecast_secret.h.example to ecast_secret.h and set a 16-byte private key."
#endif

/* CCM AAD = full ecast_hdr_t (integrity-protect all header fields).
 * CCM plaintext = audio plaintext struct (145 B).
 * CCM ciphertext length = plaintext length.
 * CCM MIC = 4 B, appended after ciphertext in the on-air frame. */

/* ─── Nonce construction (unique per-packet) ────────────────────────
 *   nonce[13] = enc_iv[8] || psn[4] || copy_idx[1]
 * Different copies of the same frame (same psn) use different nonces,
 * which is required by CCM security. Receiver reconstructs nonce from
 * the on-air header. */
static inline void ecast_make_nonce(uint8_t nonce_out[ECAST_NONCE_LEN],
                                    const uint8_t enc_iv[8],
                                    uint32_t psn,
                                    uint8_t  copy_idx)
{
    for (int i = 0; i < 8; i++) nonce_out[i] = enc_iv[i];
    nonce_out[8]  = (uint8_t)(psn         & 0xFF);
    nonce_out[9]  = (uint8_t)((psn >>  8) & 0xFF);
    nonce_out[10] = (uint8_t)((psn >> 16) & 0xFF);
    nonce_out[11] = (uint8_t)((psn >> 24) & 0xFF);
    nonce_out[12] = copy_idx;
}

/* Max frame size we'll ever put on the air — used to size receive buffers */
#define ECAST_MAX_FRAME_SIZE       250        /* ESP-NOW payload hard limit */
