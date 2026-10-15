/*
 * zw_s2_kdf.c - S2's key derivations (CKDF, docs/features/36 security.md §3) and the SPAN
 * nonce generator built on them (§6 "Establishing a SPAN"); see zw_crypto.h.
 */
#include "zw_crypto_internal.h"

#define ZW_CKDF_PAD 15u /* the repeated constant byte of each expansion step */

/* The expansion both network keys and temporary keys use: T1 = CMAC(K, c x15 | 01),
 * Ti = CMAC(K, Ti-1 | c x15 | i) for i = 2..count, into t (count * 16 bytes). */
static bool zw_ckdf_expand(const zw_crypto_port_t *port, const uint8_t key[16], uint8_t c,
                           uint8_t *t, size_t count)
{
    uint8_t m[16u + ZW_CKDF_PAD + 1u] = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        size_t len = 0u;
        if (i > 0u) {
            memcpy(m, &t[16u * (i - 1u)], 16);
            len = 16u;
        }
        memset(&m[len], c, ZW_CKDF_PAD);
        len += ZW_CKDF_PAD;
        m[len++] = (uint8_t)(i + 1u);
        ok = zw_aes_cmac(port, key, m, len, &t[16u * i]);
    }
    zw_wipe(m, sizeof m);
    return ok;
}

bool zw_s2_network_key_expand(const zw_crypto_port_t *port, const uint8_t pnk[16],
                              uint8_t key_ccm[16], uint8_t pstring[32], uint8_t key_mpan[16])
{
    uint8_t t[4u * 16u] = {0};
    bool ok = zw_has_aes(port) && pnk != NULL && key_ccm != NULL && pstring != NULL &&
              key_mpan != NULL && zw_ckdf_expand(port, pnk, 0x55u, t, 4u);
    if (ok) {
        memcpy(key_ccm, &t[0], 16);   /* T1 */
        memcpy(pstring, &t[16], 32);  /* T2 | T3 */
        memcpy(key_mpan, &t[48], 16); /* T4 */
    } else {
        zw_wipe_out(key_ccm, 16);
        zw_wipe_out(pstring, 32);
        zw_wipe_out(key_mpan, 16);
    }
    zw_wipe(t, sizeof t);
    return ok;
}

bool zw_s2_temp_key_extract(const zw_crypto_port_t *port, const uint8_t shared[32],
                            const uint8_t pub_including[32], const uint8_t pub_joining[32],
                            uint8_t prk[16])
{
    uint8_t c33[16], m[96] = {0};
    bool ok = shared != NULL && pub_including != NULL && pub_joining != NULL;
    if (ok) {
        memset(c33, 0x33, sizeof c33);
        memcpy(&m[0], shared, 32);
        memcpy(&m[32], pub_including, 32);
        memcpy(&m[64], pub_joining, 32);
        ok = zw_aes_cmac(port, c33, m, sizeof m, prk); /* zeroes prk when it fails */
    } else {
        zw_wipe_out(prk, 16);
    }
    zw_wipe(m, sizeof m);
    return ok;
}

bool zw_s2_temp_key_expand(const zw_crypto_port_t *port, const uint8_t prk[16],
                           uint8_t key_ccm[16], uint8_t pstring[32])
{
    uint8_t t[3u * 16u] = {0};
    bool ok = zw_has_aes(port) && prk != NULL && key_ccm != NULL && pstring != NULL &&
              zw_ckdf_expand(port, prk, 0x88u, t, 3u);
    if (ok) {
        memcpy(key_ccm, &t[0], 16);  /* T1 */
        memcpy(pstring, &t[16], 32); /* T2 | T3 */
    } else {
        zw_wipe_out(key_ccm, 16);
        zw_wipe_out(pstring, 32);
    }
    zw_wipe(t, sizeof t);
    return ok;
}

bool zw_s2_nonce_prk(const zw_crypto_port_t *port, const uint8_t sender_ei[16],
                     const uint8_t receiver_ei[16], uint8_t prk[16])
{
    uint8_t c26[16], m[32] = {0};
    bool ok = sender_ei != NULL && receiver_ei != NULL;
    if (ok) {
        memset(c26, 0x26, sizeof c26);
        memcpy(&m[0], sender_ei, 16);
        memcpy(&m[16], receiver_ei, 16);
        ok = zw_aes_cmac(port, c26, m, sizeof m, prk); /* zeroes prk when it fails */
    } else {
        zw_wipe_out(prk, 16);
    }
    zw_wipe(m, sizeof m);
    return ok;
}

bool zw_s2_mei(const zw_crypto_port_t *port, const uint8_t nonce_prk[16], uint8_t mei[32])
{
    uint8_t m[32] = {0}, t[32] = {0};
    bool ok = zw_has_aes(port) && nonce_prk != NULL && mei != NULL;
    if (ok) { /* T1 = CMAC(NoncePRK, EI | 00 | EI | 01), EI = 88 x15 */
        memset(&m[0], 0x88, ZW_CKDF_PAD);
        m[15] = 0x00u;
        memset(&m[16], 0x88, ZW_CKDF_PAD);
        m[31] = 0x01u;
        ok = zw_aes_cmac(port, nonce_prk, m, sizeof m, &t[0]);
    }
    if (ok) { /* T2 = CMAC(NoncePRK, T1 | EI | 02) */
        memcpy(&m[0], &t[0], 16);
        memset(&m[16], 0x88, ZW_CKDF_PAD);
        m[31] = 0x02u;
        ok = zw_aes_cmac(port, nonce_prk, m, sizeof m, &t[16]);
    }
    if (ok) {
        memcpy(mei, t, sizeof t);
    } else {
        zw_wipe_out(mei, ZW_S2_MEI_LEN);
    }
    zw_wipe(m, sizeof m);
    zw_wipe(t, sizeof t);
    return ok;
}

bool zw_s2_span_instantiate(const zw_crypto_port_t *port, zw_ctr_drbg_t *drbg,
                            const uint8_t sender_ei[16], const uint8_t receiver_ei[16],
                            const uint8_t pstring[32])
{
    uint8_t prk[16] = {0}, mei[32] = {0};
    const bool ok = drbg != NULL && pstring != NULL &&
                    zw_s2_nonce_prk(port, sender_ei, receiver_ei, prk) &&
                    zw_s2_mei(port, prk, mei) &&
                    zw_ctr_drbg_instantiate(port, drbg, mei, pstring);
    zw_wipe(prk, sizeof prk);
    zw_wipe(mei, sizeof mei);
    return ok;
}

bool zw_s2_span_next_nonce(const zw_crypto_port_t *port, zw_ctr_drbg_t *drbg, uint8_t nonce[13])
{
    uint8_t block[16] = {0};
    /* No output buffer: refuse before drawing, so no nonce is lost. */
    const bool ok = nonce != NULL && zw_ctr_drbg_generate(port, drbg, block, sizeof block);
    if (ok) {
        memcpy(nonce, block, ZW_S2_NONCE_LEN);
    } else {
        zw_wipe_out(nonce, ZW_S2_NONCE_LEN);
    }
    zw_wipe(block, sizeof block);
    return ok;
}
