/*
 * zw_aes_modes.c - AES-CMAC, AES-CCM, AES-OFB and S0's CBC-MAC over the port's one-block
 * encryption (docs/features/36 security.md §2; see zw_crypto.h).
 */
#include "zw_crypto_internal.h"

#define ZW_CCM_FLAGS_ADATA 0x40u
#define ZW_CCM_FLAGS_M (((ZW_S2_TAG_LEN - 2u) / 2u) << 3) /* M' = (M - 2) / 2 in bits 5..3 */
#define ZW_CCM_FLAGS_L (2u - 1u)                           /* L' = L - 1 in bits 2..0 */

/* ---- AES-CMAC (RFC 4493) ---------------------------------------------------------------- */

/* out = in << 1, and XOR 0x87 into the last byte when in's top bit was set (RFC 4493 §2.3),
 * with a mask instead of a branch: L comes from the key. */
static void zw_cmac_double(const uint8_t in[16], uint8_t out[16])
{
    const unsigned carry = (unsigned)in[0] >> 7;
    for (size_t i = 0; i < 15u; i++) {
        out[i] = (uint8_t)(((unsigned)in[i] << 1) | ((unsigned)in[i + 1u] >> 7));
    }
    out[15] = (uint8_t)(((unsigned)in[15] << 1) ^ (0x87u & (0u - carry)));
}

bool zw_aes_cmac(const zw_crypto_port_t *port, const uint8_t key[16], const uint8_t *msg,
                 size_t len, uint8_t mac[16])
{
    static const uint8_t zero[16] = {0};
    uint8_t l[16] = {0}, k1[16] = {0}, k2[16] = {0}, x[16] = {0}, b[16] = {0};
    bool ok = zw_has_aes(port) && key != NULL && mac != NULL && (msg != NULL || len == 0u);

    /* Subkeys: L = E(K, 0), K1 = double(L), K2 = double(K1). */
    if (ok) {
        ok = zw_aes_block(port, key, zero, l);
    }
    if (ok) {
        zw_cmac_double(l, k1);
        zw_cmac_double(k1, k2);
        /* n blocks, at least one; every block but the last goes through CBC as it is, from
         * X = 0. */
        const size_t n = len == 0u ? 1u : (len + 15u) / 16u;
        for (size_t i = 0; ok && i + 1u < n; i++) {
            ok = zw_cbc_step(port, key, x, &msg[16u * i]);
        }
        if (ok) {
            /* The last block: complete (and M not empty) -> XOR K1; otherwise pad with
             * 80 00.. -> XOR K2. The length is public, so these branches leak nothing. */
            const size_t rest = len - 16u * (n - 1u);
            const uint8_t *k = rest == 16u ? k1 : k2;
            memset(b, 0, sizeof b);
            if (rest > 0u) {
                memcpy(b, &msg[16u * (n - 1u)], rest);
            }
            if (rest < 16u) {
                b[rest] = 0x80u;
            }
            for (size_t i = 0; i < 16u; i++) {
                b[i] = (uint8_t)(b[i] ^ k[i]);
            }
            ok = zw_cbc_step(port, key, x, b);
        }
    }
    if (ok) {
        memcpy(mac, x, 16);
    } else {
        zw_wipe_out(mac, 16);
    }
    zw_wipe(l, sizeof l);
    zw_wipe(k1, sizeof k1);
    zw_wipe(k2, sizeof k2);
    zw_wipe(x, sizeof x);
    zw_wipe(b, sizeof b);
    return ok;
}

/* ---- AES-CCM (RFC 3610, L = 2, M = 8) ----------------------------------------------------- */

static bool zw_ccm_args(const zw_crypto_port_t *port, const uint8_t *key, const uint8_t *nonce,
                        const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                        const uint8_t *out, const uint8_t *tag)
{
    return zw_has_aes(port) && key != NULL && nonce != NULL && tag != NULL &&
           (aad != NULL || aad_len == 0u) && aad_len <= ZW_CCM_AAD_MAX &&
           ((in != NULL && out != NULL) || len == 0u) && len <= ZW_CCM_TEXT_MAX;
}

/* CCM's two passes in one stream (RFC 3610 §2.2-2.3): the CTR encryption of `in` into `out`
 * with A_i = L' | nonce | i (i >= 1), and the CBC-MAC of B_0 = flags | nonce | l(m), the encoded
 * associated data and the plaintext (`in` when encrypting, `out` when decrypting), each part
 * zero-padded to a whole block. u = the first 8 bytes of the MAC XOR E(K, A_0). Block by block,
 * each input byte read before its output byte is written, so out == in works. */
static bool zw_ccm_run(const zw_crypto_port_t *port, const uint8_t key[16],
                       const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                       const uint8_t *in, size_t len, uint8_t *out, bool decrypt, uint8_t u[8])
{
    uint8_t x[16] = {0}; /* the CBC-MAC chain */
    uint8_t b[16] = {0}; /* the block the MAC takes next */
    uint8_t a[16] = {0}; /* the counter block A_i */
    uint8_t s[16] = {0}; /* E(K, A_i) */

    b[0] = (uint8_t)((aad_len > 0u ? ZW_CCM_FLAGS_ADATA : 0u) | ZW_CCM_FLAGS_M | ZW_CCM_FLAGS_L);
    memcpy(&b[1], nonce, 13);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    bool ok = zw_aes_block(port, key, b, x);

    /* The associated data: l(a) on two bytes (0 < l(a) < 0xFF00), a, zero padding. */
    if (ok && aad_len > 0u) {
        memset(b, 0, sizeof b);
        b[0] = (uint8_t)(aad_len >> 8);
        b[1] = (uint8_t)aad_len;
        size_t pos = 2u;
        for (size_t i = 0; ok && i < aad_len; i++) {
            b[pos++] = aad[i];
            if (pos == 16u || i + 1u == aad_len) {
                ok = zw_cbc_step(port, key, x, b);
                memset(b, 0, sizeof b);
                pos = 0u;
            }
        }
    }

    /* The message: CTR and CBC-MAC over the same blocks. */
    a[0] = (uint8_t)ZW_CCM_FLAGS_L;
    memcpy(&a[1], nonce, 13);
    for (size_t off = 0, ctr = 1u; ok && off < len; off += 16u, ctr++) {
        const size_t n = len - off < 16u ? len - off : 16u;
        a[14] = (uint8_t)(ctr >> 8);
        a[15] = (uint8_t)ctr;
        ok = zw_aes_block(port, key, a, s);
        if (!ok) {
            break;
        }
        memset(b, 0, sizeof b);
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = in[off + i];
            const uint8_t p = (uint8_t)(c ^ s[i]);
            out[off + i] = p;
            b[i] = (uint8_t)(decrypt ? p : c); /* the MAC is over the plaintext */
        }
        ok = zw_cbc_step(port, key, x, b);
    }

    /* The tag: T XOR the first bytes of S_0 = E(K, A_0). */
    if (ok) {
        a[14] = 0u;
        a[15] = 0u;
        ok = zw_aes_block(port, key, a, s);
    }
    if (ok) {
        for (size_t i = 0; i < ZW_S2_TAG_LEN; i++) {
            u[i] = (uint8_t)(x[i] ^ s[i]);
        }
    }
    zw_wipe(x, sizeof x);
    zw_wipe(b, sizeof b);
    zw_wipe(a, sizeof a);
    zw_wipe(s, sizeof s);
    return ok;
}

bool zw_aes_ccm_encrypt(const zw_crypto_port_t *port, const uint8_t key[16],
                        const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[8])
{
    uint8_t u[ZW_S2_TAG_LEN] = {0};
    bool ok = zw_ccm_args(port, key, nonce, aad, aad_len, pt, len, ct, tag) &&
              zw_ccm_run(port, key, nonce, aad, aad_len, pt, len, ct, false, u);
    if (ok) {
        memcpy(tag, u, sizeof u);
    } else {
        /* Nothing half-encrypted (or, in place, still plain) is left to be sent. */
        zw_wipe_out(ct, len);
        zw_wipe_out(tag, ZW_S2_TAG_LEN);
    }
    zw_wipe(u, sizeof u);
    return ok;
}

bool zw_aes_ccm_decrypt(const zw_crypto_port_t *port, const uint8_t key[16],
                        const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, size_t len, const uint8_t tag[8], uint8_t *pt)
{
    uint8_t want[ZW_S2_TAG_LEN] = {0}, got[ZW_S2_TAG_LEN] = {0};
    bool ok = zw_ccm_args(port, key, nonce, aad, aad_len, ct, len, pt, tag);
    if (ok) {
        memcpy(want, tag, sizeof want); /* before pt is written: the tag may follow ct */
        ok = zw_ccm_run(port, key, nonce, aad, aad_len, ct, len, pt, true, got);
        /* ok is public (the port failed or not), so the && leaks nothing. */
        ok = ok && zw_ct_equal(got, want, sizeof got);
    }
    if (!ok) {
        zw_wipe_out(pt, len);
    }
    zw_wipe(want, sizeof want);
    zw_wipe(got, sizeof got);
    return ok;
}

/* ---- AES-OFB and S0's CBC-MAC ------------------------------------------------------------- */

bool zw_aes_ofb(const zw_crypto_port_t *port, const uint8_t key[16], const uint8_t iv[16],
                const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t s[16] = {0}, t[16] = {0};
    bool ok = zw_has_aes(port) && key != NULL && iv != NULL &&
              ((in != NULL && out != NULL) || len == 0u);
    if (ok) {
        memcpy(s, iv, sizeof s);
    }
    for (size_t off = 0; ok && off < len; off += 16u) {
        ok = zw_aes_block(port, key, s, t); /* S_i = E(K, S_i-1), S_0 = IV */
        if (!ok) {
            break;
        }
        memcpy(s, t, sizeof s);
        const size_t n = len - off < 16u ? len - off : 16u;
        for (size_t i = 0; i < n; i++) {
            out[off + i] = (uint8_t)(in[off + i] ^ s[i]);
        }
    }
    if (!ok) {
        zw_wipe_out(out, len);
    }
    zw_wipe(s, sizeof s);
    zw_wipe(t, sizeof t);
    return ok;
}

bool zw_s0_cbc_mac(const zw_crypto_port_t *port, const uint8_t key[16], const uint8_t *data,
                   size_t len, uint8_t mac[8])
{
    uint8_t x[16] = {0}; /* the chain, from the zero IV */
    uint8_t b[16] = {0};
    bool ok = zw_has_aes(port) && key != NULL && data != NULL && len > 0u && mac != NULL;
    for (size_t off = 0; ok && off < len; off += 16u) {
        const size_t n = len - off < 16u ? len - off : 16u;
        memset(b, 0, sizeof b); /* zero padding of the last block */
        memcpy(b, &data[off], n);
        ok = zw_cbc_step(port, key, x, b);
    }
    if (ok) {
        memcpy(mac, x, ZW_S0_MAC_LEN);
    } else {
        zw_wipe_out(mac, ZW_S0_MAC_LEN);
    }
    zw_wipe(x, sizeof x);
    zw_wipe(b, sizeof b);
    return ok;
}
