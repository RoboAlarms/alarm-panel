/*
 * plink_psa.c - the panel link's crypto port on PSA (ESP-IDF 6.1's mbedTLS 4 is PSA only;
 * ADR-002): the three primitives components/core/panel_link builds on (plink_crypto.h). HMAC,
 * HKDF, AES-CCM and the key schedule stay the portable, host-proved code. The same approach as
 * Z-Wave's port (zwave_service/src/zw_psa.c), with its own state: only the link task calls in.
 *
 *   AES-128, one block   PSA_ALG_ECB_NO_PADDING (the P4's AES peripheral does the work)
 *   X25519               psa_raw_key_agreement(PSA_ALG_ECDH) with the scalar as a Montgomery-255
 *                        key pair; the all-zero result is refused by plink_x25519_shared
 *   random               psa_generate_random (the hardware RNG)
 *
 * The last AES key stays imported: a frame's CCM runs many blocks under one key. Keys never reach
 * the log; plink_crypto_check proves the port at every start (plink_service_start).
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "plink";

static struct {
    bool ready;
    bool aes_valid;
    uint8_t aes_key[16];
    psa_key_id_t aes_id;
} P;

static void forget_aes(void)
{
    if (P.aes_valid) {
        (void)psa_destroy_key(P.aes_id);
        P.aes_valid = false;
    }
    plink_wipe(P.aes_key, sizeof P.aes_key);
}

static bool aes128(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    (void)ctx;
    if (!P.aes_valid || !plink_ct_equal(P.aes_key, key, 16u)) {
        forget_aes();
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
        psa_set_key_algorithm(&a, PSA_ALG_ECB_NO_PADDING);
        psa_set_key_type(&a, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&a, 128u);
        if (psa_import_key(&a, key, 16u, &P.aes_id) != PSA_SUCCESS) {
            return false;
        }
        memcpy(P.aes_key, key, 16u);
        P.aes_valid = true;
    }
    size_t n = 0u;
    return psa_cipher_encrypt(P.aes_id, PSA_ALG_ECB_NO_PADDING, in, 16u, out, 16u, &n) ==
               PSA_SUCCESS &&
           n == 16u;
}

static bool x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    (void)ctx;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255u);
    psa_key_id_t id = 0;
    if (psa_import_key(&a, scalar, 32u, &id) != PSA_SUCCESS) {
        return false;
    }
    size_t n = 0u;
    const psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, id, point, 32u, out, 32u, &n);
    (void)psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 32u;
}

static bool random_bytes(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    return psa_generate_random(buf, len) == PSA_SUCCESS;
}

bool plink_psa_port(plink_crypto_t *out)
{
    if (!P.ready) {
        const psa_status_t st = psa_crypto_init(); /* idempotent (PSA) */
        if (st != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
            return false;
        }
        P.ready = true;
    }
    memset(out, 0, sizeof(*out));
    out->ctx = NULL;
    out->aes128_encrypt = aes128;
    out->x25519 = x25519;
    out->random = random_bytes;
    out->insecure = false; /* the hardware RNG: peers anywhere on the network */
    return true;
}

#endif /* CONFIG_ALARM_LINK */
