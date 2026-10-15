/*
 * zw_ctr_drbg.c - CTR_DRBG with AES-128 and no derivation function, the S2 nonce generator
 * (docs/features/36 security.md §2; NIST SP 800-90A §10.2.1 without reseeding or additional
 * input; see zw_crypto.h).
 *
 * Both calls work on a copy of the state and store it only when every port call succeeded, so a
 * failure never leaves a half-updated generator behind (a SPAN that repeated a nonce would reuse
 * CCM's keystream).
 */
#include "zw_crypto_internal.h"

/* V = V + 1 as a 128-bit big-endian number: always 16 steps, no branch on V. */
static void zw_drbg_increment(uint8_t v[16])
{
    unsigned carry = 1u;
    for (size_t i = 16u; i-- > 0u;) {
        const unsigned t = (unsigned)v[i] + carry;
        v[i] = (uint8_t)t;
        carry = t >> 8;
    }
}

/* Update(data): V+1, T1 = E(Key, V), V+1, T2 = E(Key, V); Key | V = (T1 | T2) XOR data. */
static bool zw_drbg_update(const zw_crypto_port_t *port, zw_ctr_drbg_t *w, const uint8_t data[32])
{
    uint8_t t[32] = {0};
    zw_drbg_increment(w->v);
    bool ok = zw_aes_block(port, w->key, w->v, &t[0]);
    if (ok) {
        zw_drbg_increment(w->v);
        ok = zw_aes_block(port, w->key, w->v, &t[16]);
    }
    if (ok) {
        for (size_t i = 0; i < 32u; i++) {
            t[i] = (uint8_t)(t[i] ^ data[i]);
        }
        memcpy(w->key, &t[0], 16);
        memcpy(w->v, &t[16], 16);
    }
    zw_wipe(t, sizeof t);
    return ok;
}

bool zw_ctr_drbg_instantiate(const zw_crypto_port_t *port, zw_ctr_drbg_t *d,
                             const uint8_t entropy[32], const uint8_t personalization[32])
{
    zw_ctr_drbg_t w = {{0}, {0}}; /* Key = 0, V = 0 */
    uint8_t seed[32] = {0};
    bool ok = zw_has_aes(port) && d != NULL && entropy != NULL;
    if (ok) {
        for (size_t i = 0; i < 32u; i++) {
            const unsigned p = personalization != NULL ? (unsigned)personalization[i] : 0u;
            seed[i] = (uint8_t)((unsigned)entropy[i] ^ p);
        }
        ok = zw_drbg_update(port, &w, seed);
    }
    if (ok) {
        *d = w;
    }
    zw_wipe(&w, sizeof w);
    zw_wipe(seed, sizeof seed);
    return ok;
}

bool zw_ctr_drbg_generate(const zw_crypto_port_t *port, zw_ctr_drbg_t *d, uint8_t *out, size_t n)
{
    static const uint8_t no_data[32] = {0};
    zw_ctr_drbg_t w = {{0}, {0}};
    uint8_t block[16] = {0};
    bool ok = zw_has_aes(port) && d != NULL && out != NULL && n > 0u &&
              n <= ZW_CTR_DRBG_MAX_REQUEST;
    if (ok) {
        w = *d;
        for (size_t off = 0; ok && off < n; off += 16u) {
            zw_drbg_increment(w.v);
            ok = zw_aes_block(port, w.key, w.v, block);
            if (ok) {
                memcpy(&out[off], block, n - off < 16u ? n - off : 16u);
            }
        }
        if (ok) {
            ok = zw_drbg_update(port, &w, no_data);
        }
    }
    if (ok) {
        *d = w;
    } else {
        zw_wipe_out(out, n);
    }
    zw_wipe(&w, sizeof w);
    zw_wipe(block, sizeof block);
    return ok;
}
