/* AES-CCM + AES-CMAC helpers — built on mbedTLS */

#include "ecast_crypto.h"
#include <string.h>
#include "mbedtls/ccm.h"
#include "mbedtls/cmac.h"
#include "mbedtls/aes.h"
#include "esp_log.h"

static const char *TAG = "ecast_crypto";

bool ecast_derive_session_key(const uint8_t broadcast_code[16],
                              const uint8_t key_diversifier[8],
                              uint8_t       session_key_out[16])
{
    /* input = key_diversifier (8) || SALT (8) = 16 bytes */
    uint8_t input[16];
    const uint8_t salt[ECAST_SALT_LEN] = ECAST_SALT_BYTES;
    memcpy(input,     key_diversifier, 8);
    memcpy(input + 8, salt,            8);

    const mbedtls_cipher_info_t *ci =
        mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (!ci) {
        ESP_LOGE(TAG, "cipher_info_from_type failed");
        return false;
    }

    mbedtls_cipher_context_t ctx;
    mbedtls_cipher_init(&ctx);
    int rc = mbedtls_cipher_setup(&ctx, ci);
    if (rc != 0) {
        ESP_LOGE(TAG, "cipher_setup rc=%d", rc);
        mbedtls_cipher_free(&ctx);
        return false;
    }

    rc = mbedtls_cipher_cmac_starts(&ctx, broadcast_code, 128);
    if (rc != 0) { mbedtls_cipher_free(&ctx); return false; }
    rc = mbedtls_cipher_cmac_update(&ctx, input, sizeof(input));
    if (rc != 0) { mbedtls_cipher_free(&ctx); return false; }
    rc = mbedtls_cipher_cmac_finish(&ctx, session_key_out);
    mbedtls_cipher_free(&ctx);

    return rc == 0;
}

bool ecast_ccm_encrypt(const uint8_t  session_key[16],
                       const uint8_t  nonce[ECAST_NONCE_LEN],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *plaintext, size_t plaintext_len,
                       uint8_t       *ciphertext_and_mic_out)
{
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, session_key, 128);
    if (rc != 0) { mbedtls_ccm_free(&ctx); return false; }

    rc = mbedtls_ccm_encrypt_and_tag(
        &ctx,
        plaintext_len,
        nonce, ECAST_NONCE_LEN,
        aad,   aad_len,
        plaintext,
        ciphertext_and_mic_out,
        ciphertext_and_mic_out + plaintext_len, ECAST_MIC_LEN);

    mbedtls_ccm_free(&ctx);
    return rc == 0;
}

bool ecast_ccm_decrypt(const uint8_t  session_key[16],
                       const uint8_t  nonce[ECAST_NONCE_LEN],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *ciphertext_and_mic, size_t ct_and_mic_len,
                       uint8_t       *plaintext_out)
{
    if (ct_and_mic_len < ECAST_MIC_LEN) return false;
    size_t ct_len = ct_and_mic_len - ECAST_MIC_LEN;

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, session_key, 128);
    if (rc != 0) { mbedtls_ccm_free(&ctx); return false; }

    rc = mbedtls_ccm_auth_decrypt(
        &ctx,
        ct_len,
        nonce, ECAST_NONCE_LEN,
        aad,   aad_len,
        ciphertext_and_mic,
        plaintext_out,
        ciphertext_and_mic + ct_len, ECAST_MIC_LEN);

    mbedtls_ccm_free(&ctx);
    return rc == 0;
}

/* Compute CMAC(broadcast_code, body || BEACON_SALT) and return the
 * truncated-to-4-bytes tag. */
static bool compute_beacon_tag(const uint8_t broadcast_code[16],
                               const uint8_t *body, size_t body_len,
                               uint8_t       tag_out[ECAST_BEACON_TAG_LEN])
{
    const uint8_t salt[ECAST_SALT_LEN] = ECAST_SALT_BEACON_BYTES;

    const mbedtls_cipher_info_t *ci =
        mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (!ci) return false;

    mbedtls_cipher_context_t ctx;
    mbedtls_cipher_init(&ctx);
    int rc = mbedtls_cipher_setup(&ctx, ci);
    if (rc != 0) { mbedtls_cipher_free(&ctx); return false; }

    uint8_t full_tag[16];
    rc = mbedtls_cipher_cmac_starts(&ctx, broadcast_code, 128);
    if (rc != 0) { mbedtls_cipher_free(&ctx); return false; }
    rc = mbedtls_cipher_cmac_update(&ctx, body, body_len);
    if (rc != 0) { mbedtls_cipher_free(&ctx); return false; }
    rc = mbedtls_cipher_cmac_update(&ctx, salt, sizeof(salt));
    if (rc != 0) { mbedtls_cipher_free(&ctx); return false; }
    rc = mbedtls_cipher_cmac_finish(&ctx, full_tag);
    mbedtls_cipher_free(&ctx);
    if (rc != 0) return false;

    for (int i = 0; i < ECAST_BEACON_TAG_LEN; i++) tag_out[i] = full_tag[i];
    return true;
}

bool ecast_beacon_sign(const uint8_t  broadcast_code[16],
                       const uint8_t *body, size_t body_len,
                       uint8_t        tag_out[ECAST_BEACON_TAG_LEN])
{
    return compute_beacon_tag(broadcast_code, body, body_len, tag_out);
}

bool ecast_beacon_verify(const uint8_t  broadcast_code[16],
                         const uint8_t *body, size_t body_len,
                         const uint8_t  tag[ECAST_BEACON_TAG_LEN])
{
    uint8_t expected[ECAST_BEACON_TAG_LEN];
    if (!compute_beacon_tag(broadcast_code, body, body_len, expected)) return false;

    /* constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < ECAST_BEACON_TAG_LEN; i++) diff |= tag[i] ^ expected[i];
    return diff == 0;
}
