/*
 * backup_psa.c - the backup file's crypto port on PSA (ESP-IDF 6.1's mbedTLS 4; backup.h):
 *
 *   PBKDF2-HMAC-SHA256   psa_key_derivation with PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256), the
 *                        password as PSA_KEY_DERIVATION_INPUT_PASSWORD bytes
 *   AES-256-GCM          psa_aead_encrypt / _decrypt (PSA_ALG_GCM, 16-byte tag): the tag is the
 *                        last 16 bytes of PSA's output, so ct and tag must be adjacent, as the
 *                        backup component always passes them; otherwise a bounce buffer
 *   random               psa_generate_random (the hardware RNG)
 *
 * Each key is imported for one call and destroyed after it. Keys never reach the log.
 */
#include "backup_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

static bool ready(void)
{
    return psa_crypto_init() == PSA_SUCCESS; /* idempotent */
}

static bool pbkdf2(void *ctx, const uint8_t *password, size_t password_len, const uint8_t *salt,
                   size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
    (void)ctx;
    if (!ready()) {
        return false;
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    bool ok = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)) ==
                  PSA_SUCCESS &&
              psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations) ==
                  PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
                                             salt_len) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, password,
                                             password_len) == PSA_SUCCESS &&
              psa_key_derivation_output_bytes(&op, out, out_len) == PSA_SUCCESS;
    (void)psa_key_derivation_abort(&op);
    if (!ok) {
        mbedtls_platform_zeroize(out, out_len);
    }
    return ok;
}

static bool import(const uint8_t key[32], psa_key_id_t *id)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256u);
    return ready() && psa_import_key(&a, key, 32u, id) == PSA_SUCCESS;
}

static bool gcm_encrypt(void *ctx, const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t len,
                        uint8_t *ct, uint8_t tag[16])
{
    (void)ctx;
    psa_key_id_t id = 0;
    if (!import(key, &id)) {
        return false;
    }
    const bool adjacent = tag == ct + len;
    uint8_t *out = adjacent ? ct : heap_caps_malloc(len + 16u, MALLOC_CAP_SPIRAM);
    size_t n = 0;
    bool ok = out != NULL &&
              psa_aead_encrypt(id, PSA_ALG_GCM, nonce, 12u, aad, aad_len, pt, len, out,
                               len + 16u, &n) == PSA_SUCCESS &&
              n == len + 16u;
    (void)psa_destroy_key(id);
    if (ok && !adjacent) {
        memcpy(ct, out, len);
        memcpy(tag, out + len, 16u);
    }
    if (!adjacent) {
        free(out);
    }
    return ok;
}

static bool gcm_decrypt(void *ctx, const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t len,
                        const uint8_t tag[16], uint8_t *pt)
{
    (void)ctx;
    psa_key_id_t id = 0;
    if (!import(key, &id)) {
        return false;
    }
    const bool adjacent = tag == ct + len;
    uint8_t *in = adjacent ? (uint8_t *)ct : heap_caps_malloc(len + 16u, MALLOC_CAP_SPIRAM);
    if (in != NULL && !adjacent) {
        memcpy(in, ct, len);
        memcpy(in + len, tag, 16u);
    }
    size_t n = 0;
    const bool ok = in != NULL &&
                    psa_aead_decrypt(id, PSA_ALG_GCM, nonce, 12u, aad, aad_len, in, len + 16u,
                                     pt, len, &n) == PSA_SUCCESS &&
                    n == len;
    (void)psa_destroy_key(id);
    if (!adjacent) {
        free(in);
    }
    if (!ok) {
        memset(pt, 0, len);
    }
    return ok;
}

static bool rng(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    return ready() && psa_generate_random(buf, len) == PSA_SUCCESS;
}

const backup_crypto_t *backup_psa_port(void)
{
    static const backup_crypto_t port = {
        .ctx = NULL,
        .pbkdf2_sha256 = pbkdf2,
        .gcm_encrypt = gcm_encrypt,
        .gcm_decrypt = gcm_decrypt,
        .random = rng,
    };
    return &port;
}

/* ---- the known answers (the same the host tests check the portable side's port against) ---- */

static bool hex_equal(const uint8_t *got, const char *want, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2u; k++) {
            const char c = want[2u * i + k];
            v = v * 16u + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        if (got[i] != (uint8_t)v) {
            return false;
        }
    }
    return true;
}

static void from_hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2u; k++) {
            const char c = s[2u * i + k];
            v = v * 16u + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
}

bool backup_psa_known_answers(void)
{
    const backup_crypto_t *c = backup_psa_port();
    /* RFC 7914 §11: PBKDF2-HMAC-SHA256("passwd", "salt", 1, 64). */
    uint8_t dk[64];
    if (!c->pbkdf2_sha256(NULL, (const uint8_t *)"passwd", 6u, (const uint8_t *)"salt", 4u, 1u,
                          dk, sizeof(dk)) ||
        !hex_equal(dk,
                   "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                   "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
                   64u)) {
        return false;
    }
    /* The GCM specification's test case 16 (AES-256). */
    uint8_t key[32];
    uint8_t iv[12];
    uint8_t pt[60];
    uint8_t aad[20];
    uint8_t out[76];
    from_hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", key, 32u);
    from_hex("cafebabefacedbaddecaf888", iv, 12u);
    from_hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
             "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
             pt, 60u);
    from_hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20u);
    if (!c->gcm_encrypt(NULL, key, iv, aad, 20u, pt, 60u, out, out + 60) ||
        !hex_equal(out,
                   "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                   "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
                   "76fc6ece0f4e1768cddf8853bb2d551b",
                   76u)) {
        return false;
    }
    uint8_t back[60];
    if (!c->gcm_decrypt(NULL, key, iv, aad, 20u, out, 60u, out + 60, back) ||
        memcmp(back, pt, 60u) != 0) {
        return false;
    }
    out[3] ^= 1u; /* a changed ciphertext must be refused */
    return !c->gcm_decrypt(NULL, key, iv, aad, 20u, out, 60u, out + 60, back);
}
