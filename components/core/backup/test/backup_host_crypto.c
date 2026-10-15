/*
 * backup_host_crypto.c - the backup file's crypto port on a PC (see backup_host_crypto.h).
 */
#include "backup_host_crypto.h"

#include <string.h>

#include "alarm_proto/proto_link.h" /* proto_sha256 */

/* ---- HMAC-SHA256 (RFC 2104) and PBKDF2 (RFC 8018 §5.2) ------------------------------------- */

#define SHA_BLOCK 64u
#define SHA_LEN   32u
#define MSG_MAX   64u /* the longest message PBKDF2 hands HMAC: salt + 4, or one U */

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                        uint8_t out[32])
{
    uint8_t k[SHA_BLOCK] = {0};
    if (key_len > SHA_BLOCK) {
        proto_sha256(key, key_len, k);
    } else if (key_len > 0u) {
        memcpy(k, key, key_len);
    }
    uint8_t inner[SHA_BLOCK + MSG_MAX];
    for (size_t i = 0; i < SHA_BLOCK; i++) {
        inner[i] = (uint8_t)(k[i] ^ 0x36u);
    }
    memcpy(inner + SHA_BLOCK, msg, msg_len);
    uint8_t ih[SHA_LEN];
    proto_sha256(inner, SHA_BLOCK + msg_len, ih);
    uint8_t outer[SHA_BLOCK + SHA_LEN];
    for (size_t i = 0; i < SHA_BLOCK; i++) {
        outer[i] = (uint8_t)(k[i] ^ 0x5Cu);
    }
    memcpy(outer + SHA_BLOCK, ih, SHA_LEN);
    proto_sha256(outer, sizeof(outer), out);
}

void bhc_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                       uint32_t iterations, uint8_t *out, size_t out_len)
{
    uint32_t block = 1;
    while (out_len > 0u) {
        uint8_t msg[MSG_MAX];
        memcpy(msg, salt, salt_len);
        msg[salt_len] = (uint8_t)(block >> 24);
        msg[salt_len + 1u] = (uint8_t)(block >> 16);
        msg[salt_len + 2u] = (uint8_t)(block >> 8);
        msg[salt_len + 3u] = (uint8_t)block;
        uint8_t u[SHA_LEN];
        uint8_t t[SHA_LEN];
        hmac_sha256(pw, pw_len, msg, salt_len + 4u, u);
        memcpy(t, u, SHA_LEN);
        for (uint32_t i = 1; i < iterations; i++) {
            uint8_t next[SHA_LEN];
            hmac_sha256(pw, pw_len, u, SHA_LEN, next);
            memcpy(u, next, SHA_LEN);
            for (size_t j = 0; j < SHA_LEN; j++) {
                t[j] ^= u[j];
            }
        }
        const size_t n = out_len < SHA_LEN ? out_len : SHA_LEN;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
        block++;
    }
}

/* ---- AES-256 encryption (FIPS-197) --------------------------------------------------------- */

static uint8_t aes_xtime(uint8_t a)
{
    return (uint8_t)(((unsigned)a << 1) ^ (((unsigned)a >> 7) * 0x1Bu));
}

static uint8_t aes_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b != 0u) {
        if (b & 1u) {
            p = (uint8_t)(p ^ a);
        }
        a = aes_xtime(a);
        b = (uint8_t)(b >> 1);
    }
    return p;
}

/* SubBytes' table (FIPS-197 §5.1.1), computed: the inverse in GF(2^8), then the affine map. */
static void aes_make_sbox(uint8_t sbox[256])
{
    for (unsigned x = 0; x < 256u; x++) {
        unsigned inv = 0u;
        for (unsigned y = 1; x != 0u && y < 256u; y++) {
            if (aes_mul((uint8_t)x, (uint8_t)y) == 1u) {
                inv = y;
                break;
            }
        }
        unsigned s = inv;
        for (unsigned r = 1; r <= 4u; r++) {
            s ^= ((inv << r) | (inv >> (8u - r))) & 0xFFu;
        }
        sbox[x] = (uint8_t)(s ^ 0x63u);
    }
}

/* KeyExpansion for Nk = 8, Nr = 14 (FIPS-197 §5.2): 60 words; RotWord + SubWord + Rcon when
 * i mod 8 = 0, SubWord alone when i mod 8 = 4. */
static void aes_expand_key(const uint8_t sbox[256], const uint8_t key[32], uint8_t rk[240])
{
    uint8_t rcon = 0x01u;
    memcpy(rk, key, 32);
    for (unsigned i = 8u; i < 60u; i++) {
        uint8_t t[4] = {rk[4u * i - 4u], rk[4u * i - 3u], rk[4u * i - 2u], rk[4u * i - 1u]};
        if (i % 8u == 0u) {
            const uint8_t first = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = aes_xtime(rcon);
        } else if (i % 8u == 4u) {
            for (unsigned k = 0; k < 4u; k++) {
                t[k] = sbox[t[k]];
            }
        }
        for (unsigned k = 0; k < 4u; k++) {
            rk[4u * i + k] = (uint8_t)(rk[4u * (i - 8u) + k] ^ t[k]);
        }
    }
}

void bhc_aes256_encrypt(const backup_host_crypto_t *hc, const uint8_t key[32], const uint8_t in[16],
                        uint8_t out[16])
{
    uint8_t rk[240];
    aes_expand_key(hc->sbox, key, rk);
    uint8_t st[16];
    for (unsigned i = 0; i < 16u; i++) {
        st[i] = (uint8_t)(in[i] ^ rk[i]);
    }
    for (unsigned round = 1; round <= 14u; round++) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4u; c++) { /* SubBytes + ShiftRows */
            for (unsigned r = 0; r < 4u; r++) {
                t[r + 4u * c] = hc->sbox[st[r + 4u * ((c + r) % 4u)]];
            }
        }
        if (round != 14u) { /* MixColumns */
            for (unsigned c = 0; c < 4u; c++) {
                const uint8_t a0 = t[4u * c], a1 = t[4u * c + 1u], a2 = t[4u * c + 2u],
                              a3 = t[4u * c + 3u];
                st[4u * c] = (uint8_t)(aes_xtime(a0) ^ aes_xtime(a1) ^ a1 ^ a2 ^ a3);
                st[4u * c + 1u] = (uint8_t)(a0 ^ aes_xtime(a1) ^ aes_xtime(a2) ^ a2 ^ a3);
                st[4u * c + 2u] = (uint8_t)(a0 ^ a1 ^ aes_xtime(a2) ^ aes_xtime(a3) ^ a3);
                st[4u * c + 3u] = (uint8_t)(aes_xtime(a0) ^ a0 ^ a1 ^ a2 ^ aes_xtime(a3));
            }
        } else {
            memcpy(st, t, 16);
        }
        for (unsigned i = 0; i < 16u; i++) {
            st[i] ^= rk[16u * round + i];
        }
    }
    memcpy(out, st, 16);
}

/* ---- GCM (NIST SP 800-38D) ----------------------------------------------------------------- */

/* X = X * Y in GF(2^128) (§6.3, Algorithm 1), bit 0 the leftmost. */
static void gf_mul(uint8_t x[16], const uint8_t y[16])
{
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, y, 16);
    for (unsigned i = 0; i < 128u; i++) {
        if (((unsigned)x[i / 8u] >> (7u - i % 8u)) & 1u) {
            for (unsigned k = 0; k < 16u; k++) {
                z[k] ^= v[k];
            }
        }
        const unsigned lsb = (unsigned)v[15] & 1u;
        for (unsigned k = 15u; k > 0u; k--) {
            v[k] = (uint8_t)(((unsigned)v[k] >> 1) | ((unsigned)v[k - 1u] << 7));
        }
        v[0] = (uint8_t)((unsigned)v[0] >> 1);
        if (lsb) {
            v[0] = (uint8_t)((unsigned)v[0] ^ 0xE1u);
        }
    }
    memcpy(x, z, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len)
{
    while (len > 0u) {
        const size_t n = len < 16u ? len : 16u;
        for (size_t i = 0; i < n; i++) {
            y[i] ^= data[i];
        }
        gf_mul(y, h);
        data += n;
        len -= n;
    }
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (56u - 8u * i));
    }
}

/* ct = GCTR(inc32(J0), in), and the tag over aad and ct. */
static void gcm_core(const backup_host_crypto_t *hc, const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                     uint8_t *out, bool encrypting, uint8_t tag[16])
{
    uint8_t h[16] = {0};
    bhc_aes256_encrypt(hc, key, h, h);
    uint8_t j0[16];
    memcpy(j0, nonce, 12);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
    uint8_t y[16] = {0};
    ghash_update(y, h, aad, aad_len);
    uint8_t ctr[16];
    memcpy(ctr, j0, 16);
    for (size_t off = 0; off < len; off += 16u) {
        for (int i = 15; i >= 12; i--) { /* inc32 */
            if (++ctr[i] != 0u) {
                break;
            }
        }
        uint8_t ks[16];
        bhc_aes256_encrypt(hc, key, ctr, ks);
        const size_t n = len - off < 16u ? len - off : 16u;
        uint8_t block[16] = {0};
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = (uint8_t)(in[off + i] ^ ks[i]);
            block[i] = encrypting ? c : in[off + i];
            out[off + i] = c;
        }
        for (size_t i = 0; i < 16u; i++) {
            y[i] ^= block[i];
        }
        gf_mul(y, h);
    }
    uint8_t lens[16];
    put_be64(lens, (uint64_t)aad_len * 8u);
    put_be64(lens + 8, (uint64_t)len * 8u);
    ghash_update(y, h, lens, 16u);
    uint8_t ek[16];
    bhc_aes256_encrypt(hc, key, j0, ek);
    for (unsigned i = 0; i < 16u; i++) {
        tag[i] = (uint8_t)(y[i] ^ ek[i]);
    }
}

void bhc_gcm_encrypt(const backup_host_crypto_t *hc, const uint8_t key[32],
                     const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16])
{
    gcm_core(hc, key, nonce, aad, aad_len, pt, len, ct, true, tag);
}

/* ---- the port -------------------------------------------------------------------------------- */

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static bool port_pbkdf2(void *ctx, const uint8_t *password, size_t password_len,
                        const uint8_t *salt, size_t salt_len, uint32_t iterations, uint8_t *out,
                        size_t out_len)
{
    backup_host_crypto_t *hc = ctx;
    hc->pbkdf2_calls++;
    if (hc->fail == BHC_FAIL_PBKDF2 || salt_len + 4u > MSG_MAX || iterations == 0u) {
        memset(out, 0xA5, out_len);
        return false;
    }
    bhc_pbkdf2_sha256(password, password_len, salt, salt_len, iterations, out, out_len);
    return true;
}

static bool port_encrypt(void *ctx, const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t len,
                         uint8_t *ct, uint8_t tag[16])
{
    backup_host_crypto_t *hc = ctx;
    if (hc->fail == BHC_FAIL_ENCRYPT) {
        return false;
    }
    gcm_core(hc, key, nonce, aad, aad_len, pt, len, ct, true, tag);
    return true;
}

static bool port_decrypt(void *ctx, const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t len,
                         const uint8_t tag[16], uint8_t *pt)
{
    backup_host_crypto_t *hc = ctx;
    uint8_t want[16];
    gcm_core(hc, key, nonce, aad, aad_len, ct, len, pt, false, want);
    uint8_t diff = 0;
    for (unsigned i = 0; i < 16u; i++) {
        diff |= (uint8_t)(want[i] ^ tag[i]);
    }
    if (diff != 0u) {
        memset(pt, 0, len);
        return false;
    }
    return true;
}

static bool port_random(void *ctx, uint8_t *buf, size_t len)
{
    backup_host_crypto_t *hc = ctx;
    if (hc->fail == BHC_FAIL_RANDOM) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)splitmix64(&hc->rng);
    }
    return true;
}

void backup_host_crypto_init(backup_host_crypto_t *hc, uint64_t seed)
{
    memset(hc, 0, sizeof(*hc));
    aes_make_sbox(hc->sbox);
    hc->rng = seed;
}

backup_crypto_t backup_host_crypto_port(backup_host_crypto_t *hc)
{
    const backup_crypto_t c = {
        .ctx = hc,
        .pbkdf2_sha256 = port_pbkdf2,
        .gcm_encrypt = port_encrypt,
        .gcm_decrypt = port_decrypt,
        .random = port_random,
    };
    return c;
}
