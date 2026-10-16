/*
 * sha256.c - SHA-256 (FIPS 180-4) for the link's pairing code and commitments (proto_link.h) and,
 * in pieces, for the panel link's HMAC, HKDF and transcripts (proto_sha256.h). Plain C11, no
 * heap; tested against the standard vectors in components/core/alarm_proto/test/test_link.c, and
 * in pieces in components/core/panel_link/test/test_plink_crypto.c.
 */
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(proto_sha256_ctx_t *s, const uint8_t p[64])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0];
    uint32_t b = s->h[1];
    uint32_t c = s->h[2];
    uint32_t d = s->h[3];
    uint32_t e = s->h[4];
    uint32_t f = s->h[5];
    uint32_t g = s->h[6];
    uint32_t h = s->h[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void proto_sha256_init(proto_sha256_ctx_t *s)
{
    static const uint32_t H0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(s->h, H0, sizeof(s->h));
    s->total = 0;
    s->buf_len = 0;
}

void proto_sha256_update(proto_sha256_ctx_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    if (len == 0u) {
        return; /* data may be NULL */
    }
    s->total += len;
    if (s->buf_len != 0u) {
        size_t take = 64u - s->buf_len;
        if (take > len) {
            take = len;
        }
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        len -= take;
        if (s->buf_len == 64u) {
            sha256_block(s, s->buf);
            s->buf_len = 0;
        }
    }
    while (len >= 64u) {
        sha256_block(s, p);
        p += 64;
        len -= 64;
    }
    if (len != 0u) {
        memcpy(s->buf, p, len);
        s->buf_len = len;
    }
}

void proto_sha256_final(proto_sha256_ctx_t *s, uint8_t out[32])
{
    uint64_t bits = s->total * 8u;
    static const uint8_t pad = 0x80;
    proto_sha256_update(s, &pad, 1);
    static const uint8_t zero[64] = {0};
    while (s->buf_len != 56u) {
        proto_sha256_update(s, zero, s->buf_len < 56u ? 56u - s->buf_len : 64u - s->buf_len);
    }
    uint8_t len_be[8];
    for (size_t i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    proto_sha256_update(s, len_be, 8);
    for (size_t i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(s->h[i]);
    }
}

void proto_sha256(const void *data, size_t len, uint8_t out[32])
{
    proto_sha256_ctx_t s;
    proto_sha256_init(&s);
    proto_sha256_update(&s, data, len);
    proto_sha256_final(&s, out);
}
