/* AES-CCM helpers for EspCastBR */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ecast_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Derive a 128-bit session key from the shared Broadcast_Code using
 * AES-CMAC(Broadcast_Code, key_diversifier || SALT).
 *
 *   @param broadcast_code   16-byte user-shared secret
 *   @param key_diversifier  8 bytes from the beacon (random per stream)
 *   @param session_key_out  16 bytes — derived key for CCM encrypt/decrypt
 *   @return true on success
 */
bool ecast_derive_session_key(const uint8_t broadcast_code[16],
                              const uint8_t key_diversifier[8],
                              uint8_t       session_key_out[16]);

/* AES-CCM encrypt the audio payload.
 *
 *   nonce    = ecast_make_nonce(enc_iv, psn, copy_idx)
 *   AAD      = full ecast_hdr_t (16 B)
 *   plaintext = ecast_audio_plain_t
 *   ciphertext = plaintext bytes (written over plaintext in-place-capable) + 4 B MIC
 *
 *   Returns true on success. Out buffer must be ≥ plaintext_len + ECAST_MIC_LEN.
 */
bool ecast_ccm_encrypt(const uint8_t  session_key[16],
                       const uint8_t  nonce[ECAST_NONCE_LEN],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *plaintext, size_t plaintext_len,
                       uint8_t       *ciphertext_and_mic_out);

/* AES-CCM decrypt + verify.
 *
 *   Returns true iff MIC is valid. Plaintext written to plaintext_out
 *   (which may alias ciphertext_and_mic). plaintext_out must be
 *   ≥ (ciphertext_and_mic_len - ECAST_MIC_LEN).
 */
bool ecast_ccm_decrypt(const uint8_t  session_key[16],
                       const uint8_t  nonce[ECAST_NONCE_LEN],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *ciphertext_and_mic, size_t ct_and_mic_len,
                       uint8_t       *plaintext_out);

/* Compute a beacon-authentication tag:
 *   full_cmac = AES-CMAC(broadcast_code, body || BEACON_SALT)
 *   tag       = first ECAST_BEACON_TAG_LEN bytes of full_cmac
 *
 * "body" is the beacon header + beacon payload (everything before the tag).
 * Returns true on success. */
bool ecast_beacon_sign(const uint8_t  broadcast_code[16],
                       const uint8_t *body, size_t body_len,
                       uint8_t       tag_out[ECAST_BEACON_TAG_LEN]);

/* Verify a beacon tag; returns true iff tag matches. */
bool ecast_beacon_verify(const uint8_t  broadcast_code[16],
                         const uint8_t *body, size_t body_len,
                         const uint8_t  tag[ECAST_BEACON_TAG_LEN]);

#ifdef __cplusplus
}
#endif
