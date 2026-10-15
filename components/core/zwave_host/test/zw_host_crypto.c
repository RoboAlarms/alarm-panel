/*
 * zw_host_crypto.c - the Z-Wave crypto port on a PC (see zw_host_crypto.h): AES-128 encryption
 * from FIPS-197 §5.1-5.2, X25519 from RFC 7748 §5, SplitMix64 for random numbers.
 */
#include "zw_host_crypto.h"

#include <string.h>

/* ---- AES-128 encryption (FIPS-197) ------------------------------------------------------ */

/* Multiplication by x in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1 (FIPS-197 §4.2.1). */
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

/* SubBytes' table (FIPS-197 §5.1.1), computed rather than typed in: the multiplicative inverse
 * in GF(2^8) (0 maps to 0), then the affine transformation
 * b ^ rotl(b, 1) ^ rotl(b, 2) ^ rotl(b, 3) ^ rotl(b, 4) ^ 0x63. */
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

/* KeyExpansion for Nk = 4 (FIPS-197 §5.2): 44 words, RotWord + SubWord + Rcon every fourth. */
static void aes_expand_key(const uint8_t sbox[256], const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rcon = 0x01u;
    memcpy(rk, key, 16);
    for (unsigned i = 16u; i < 176u; i += 4u) {
        uint8_t t[4] = {rk[i - 4u], rk[i - 3u], rk[i - 2u], rk[i - 1u]};
        if (i % 16u == 0u) {
            const uint8_t first = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = aes_xtime(rcon);
        }
        for (unsigned j = 0; j < 4u; j++) {
            rk[i + j] = (uint8_t)(rk[i + j - 16u] ^ t[j]);
        }
    }
}

/* Cipher (FIPS-197 §5.1): the state is column-major, s[r][c] = st[r + 4c]. */
static void aes128_encrypt(const uint8_t sbox[256], const uint8_t key[16], const uint8_t in[16],
                           uint8_t out[16])
{
    uint8_t rk[176], st[16], t[16];
    aes_expand_key(sbox, key, rk);
    for (unsigned i = 0; i < 16u; i++) {
        st[i] = (uint8_t)(in[i] ^ rk[i]);
    }
    for (unsigned round = 1; round <= 10u; round++) {
        /* SubBytes and ShiftRows: row r moves left by r, s'[r][c] = s[r][(c + r) mod 4]. */
        for (unsigned c = 0; c < 4u; c++) {
            for (unsigned r = 0; r < 4u; r++) {
                t[r + 4u * c] = sbox[st[r + 4u * ((c + r) % 4u)]];
            }
        }
        if (round < 10u) { /* MixColumns (§5.1.3), not in the last round */
            for (unsigned c = 0; c < 4u; c++) {
                const uint8_t a0 = t[4u * c], a1 = t[4u * c + 1u];
                const uint8_t a2 = t[4u * c + 2u], a3 = t[4u * c + 3u];
                st[4u * c] = (uint8_t)(aes_xtime(a0) ^ aes_xtime(a1) ^ a1 ^ a2 ^ a3);
                st[4u * c + 1u] = (uint8_t)(a0 ^ aes_xtime(a1) ^ aes_xtime(a2) ^ a2 ^ a3);
                st[4u * c + 2u] = (uint8_t)(a0 ^ a1 ^ aes_xtime(a2) ^ aes_xtime(a3) ^ a3);
                st[4u * c + 3u] = (uint8_t)(aes_xtime(a0) ^ a0 ^ a1 ^ a2 ^ aes_xtime(a3));
            }
        } else {
            memcpy(st, t, 16);
        }
        for (unsigned i = 0; i < 16u; i++) { /* AddRoundKey */
            st[i] = (uint8_t)(st[i] ^ rk[16u * round + i]);
        }
    }
    memcpy(out, st, 16);
    zw_wipe(rk, sizeof rk);
    zw_wipe(st, sizeof st);
    zw_wipe(t, sizeof t);
}

/* ---- X25519 (RFC 7748 §5) ---------------------------------------------------------------- */

/* A field element mod p = 2^255 - 19: sixteen signed 16-bit limbs, value = sum f[i] 2^(16 i).
 * Limbs may leave [0, 2^16) between carries; every bound below stays far inside int64_t. */
typedef int64_t fe[16];

static void fe_set(fe o, int64_t v)
{
    memset(o, 0, sizeof(fe));
    o[0] = v;
}

static void fe_copy(fe o, const fe a)
{
    memcpy(o, a, sizeof(fe));
}

static void fe_add(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++) {
        o[i] = a[i] + b[i];
    }
}

static void fe_sub(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++) {
        o[i] = a[i] - b[i];
    }
}

/* One carry pass: each limb to [0, 2^16) with the rest moved up; what leaves the top limb
 * comes back into the bottom times 38 (2^256 = 2 * 19 mod p). floor(x / 2^16) without shifting
 * a negative number (implementation-defined) or shifting one left (undefined): bias by 2^48,
 * which every limb stays well below in magnitude (at most about 2^44 after a product). */
static void fe_carry(fe o)
{
    for (int i = 0; i < 16; i++) {
        const int64_t c = ((o[i] + ((int64_t)1 << 48)) >> 16) - ((int64_t)1 << 32);
        o[i] -= c * 65536;
        if (i < 15) {
            o[i + 1] += c;
        } else {
            o[0] += 38 * c;
        }
    }
}

/* o = a * b. Inputs: carried limbs, or sums/differences of two (|limb| < 2^17 + 76), so each
 * product term is below 2^35 and a column of 16 of them, folded with 38x, below 2^44. */
static void fe_mul(fe o, const fe a, const fe b)
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; i++) {
        o[i] = t[i];
    }
    fe_carry(o);
    fe_carry(o);
}

/* o = z^(p - 2) = 1 / z (0 for 0). p - 2 = 2^255 - 21: bits 254..0 set except bits 4 and 2. */
static void fe_invert(fe o, const fe z)
{
    fe c;
    fe_copy(c, z);
    for (int bit = 253; bit >= 0; bit--) {
        fe_mul(c, c, c);
        if (bit != 4 && bit != 2) {
            fe_mul(c, c, z);
        }
    }
    fe_copy(o, c);
}

/* Swaps a and b when swap is 1, leaves them when 0, the same work either way. */
static void fe_cswap(fe a, fe b, int64_t swap)
{
    const int64_t mask = -swap;
    for (int i = 0; i < 16; i++) {
        const int64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

/* decodeUCoordinate (RFC 7748 §5): little-endian, the top bit masked off. */
static void fe_unpack(fe o, const uint8_t in[32])
{
    for (int i = 0; i < 16; i++) {
        o[i] = (int64_t)in[2 * i] + (int64_t)in[2 * i + 1] * 256;
    }
    o[15] &= 0x7FFF;
}

/* encodeUCoordinate: the value reduced to [0, p), little-endian. After the carries every limb
 * is in [0, 2^16), so the value is below 2^256 < 3p: subtract p twice where it fits. */
static void fe_pack(uint8_t out[32], const fe a)
{
    fe t, m;
    fe_copy(t, a);
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);
    for (int pass = 0; pass < 2; pass++) {
        int64_t borrow = 0;
        for (int i = 0; i < 16; i++) {
            const int64_t p = i == 0 ? 0xFFED : i == 15 ? 0x7FFF : 0xFFFF;
            const int64_t d = t[i] - p - borrow;
            borrow = (int64_t)((uint64_t)d >> 63); /* d is negative: borrow from the next */
            m[i] = d + borrow * 65536;
        }
        fe_cswap(t, m, 1 - borrow); /* no borrow out: t >= p, keep t - p */
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = (uint8_t)(t[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
    zw_wipe(t, sizeof t);
    zw_wipe(m, sizeof m);
}

/* X25519(k, u), RFC 7748 §5: decodeScalar25519, then the Montgomery ladder over bits 254..0
 * with a24 = 121665, x_2 * z_2^(p - 2) at the end. */
static void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    static const fe a24 = {0xDB41, 1}; /* 121665 = 0x1DB41 */
    uint8_t k[32];
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t;
    int64_t swap = 0;

    memcpy(k, scalar, 32);
    k[0] &= 248u;
    k[31] &= 127u;
    k[31] |= 64u;
    fe_unpack(x1, point);
    fe_set(x2, 1);
    fe_set(z2, 0);
    fe_copy(x3, x1);
    fe_set(z3, 1);
    for (int pos = 254; pos >= 0; pos--) {
        const int64_t bit = (k[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(a, x2, z2);   /* A = x_2 + z_2 */
        fe_mul(aa, a, a);    /* AA = A^2 */
        fe_sub(b, x2, z2);   /* B = x_2 - z_2 */
        fe_mul(bb, b, b);    /* BB = B^2 */
        fe_sub(e, aa, bb);   /* E = AA - BB */
        fe_add(c, x3, z3);   /* C = x_3 + z_3 */
        fe_sub(d, x3, z3);   /* D = x_3 - z_3 */
        fe_mul(da, d, a);    /* DA = D * A */
        fe_mul(cb, c, b);    /* CB = C * B */
        fe_add(t, da, cb);
        fe_mul(x3, t, t);    /* x_3 = (DA + CB)^2 */
        fe_sub(t, da, cb);
        fe_mul(t, t, t);
        fe_mul(z3, x1, t);   /* z_3 = x_1 * (DA - CB)^2 */
        fe_mul(x2, aa, bb);  /* x_2 = AA * BB */
        fe_mul(t, a24, e);
        fe_add(t, aa, t);
        fe_mul(z2, e, t);    /* z_2 = E * (AA + a24 * E) */
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_pack(out, x2);

    zw_wipe(k, sizeof k);
    zw_wipe(x2, sizeof x2);
    zw_wipe(z2, sizeof z2);
    zw_wipe(x3, sizeof x3);
    zw_wipe(z3, sizeof z3);
    zw_wipe(a, sizeof a);
    zw_wipe(aa, sizeof aa);
    zw_wipe(b, sizeof b);
    zw_wipe(bb, sizeof bb);
    zw_wipe(e, sizeof e);
    zw_wipe(c, sizeof c);
    zw_wipe(d, sizeof d);
    zw_wipe(da, sizeof da);
    zw_wipe(cb, sizeof cb);
    zw_wipe(t, sizeof t);
}

/* ---- Random numbers: SplitMix64 (deterministic, for tests only) -------------------------- */

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15u);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

/* ---- The port ----------------------------------------------------------------------------- */

/* Counts the call; true when this is the one that fails. */
static bool call_fails(zw_host_crypto_t *hc)
{
    const bool fail = hc->calls == hc->fail_at;
    hc->calls++;
    return fail;
}

static bool port_aes128_encrypt(void *ctx, const uint8_t key[16], const uint8_t in[16],
                                uint8_t out[16])
{
    zw_host_crypto_t *hc = ctx;
    if (call_fails(hc)) {
        memset(out, 0xA5, 16);
        return false;
    }
    aes128_encrypt(hc->sbox, key, in, out);
    return true;
}

static bool port_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32])
{
    zw_host_crypto_t *hc = ctx;
    if (call_fails(hc)) {
        memset(out, 0xA5, 32);
        return false;
    }
    x25519(out, scalar, point);
    return true;
}

static bool port_random(void *ctx, uint8_t *buf, size_t len)
{
    zw_host_crypto_t *hc = ctx;
    if (call_fails(hc)) {
        memset(buf, 0xA5, len);
        return false;
    }
    for (size_t i = 0; i < len; i += 8u) {
        uint64_t r = splitmix64(&hc->rng);
        for (size_t j = i; j < len && j < i + 8u; j++) {
            buf[j] = (uint8_t)r;
            r >>= 8;
        }
    }
    return true;
}

void zw_host_crypto_init(zw_host_crypto_t *hc, uint64_t seed)
{
    aes_make_sbox(hc->sbox);
    hc->rng = seed;
    hc->calls = 0u;
    hc->fail_at = ZW_HOST_NEVER;
}

zw_crypto_port_t zw_host_crypto_port(zw_host_crypto_t *hc)
{
    const zw_crypto_port_t port = {
        .ctx = hc,
        .aes128_encrypt = port_aes128_encrypt,
        .x25519 = port_x25519,
        .random = port_random,
    };
    return port;
}

void zw_host_crypto_fail_at(zw_host_crypto_t *hc, uint32_t call)
{
    hc->calls = 0u;
    hc->fail_at = call;
}
