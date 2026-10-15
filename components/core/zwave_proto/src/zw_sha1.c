/*
 * zw_sha1.c - SHA-1 (see zw_sha1.h), FIPS 180-4 §6.1.
 */
#include "zwave_proto/zw_sha1.h"

#include <string.h>

static uint32_t rol(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static void block(zw_sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (unsigned t = 0u; t < 16u; t++) {
        w[t] = ((uint32_t)p[t * 4u] << 24) | ((uint32_t)p[t * 4u + 1u] << 16) |
               ((uint32_t)p[t * 4u + 2u] << 8) | (uint32_t)p[t * 4u + 3u];
    }
    for (unsigned t = 16u; t < 80u; t++) {
        w[t] = rol(w[t - 3u] ^ w[t - 8u] ^ w[t - 14u] ^ w[t - 16u], 1u);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (unsigned t = 0u; t < 80u; t++) {
        uint32_t f, k;
        if (t < 20u) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (t < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t tmp = rol(a, 5u) + f + e + k + w[t];
        e = d;
        d = c;
        c = rol(b, 30u);
        b = a;
        a = tmp;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void zw_sha1_init(zw_sha1_t *s)
{
    memset(s, 0, sizeof *s);
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
}

void zw_sha1_update(zw_sha1_t *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bytes += len;
    while (len > 0u) {
        size_t take = 64u - s->used;
        if (take > len) {
            take = len;
        }
        memcpy(&s->block[s->used], p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64u) {
            block(s, s->block);
            s->used = 0u;
        }
    }
}

void zw_sha1_final(zw_sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->bytes * 8u;
    const uint8_t one = 0x80u;
    const uint8_t zero = 0u;
    zw_sha1_update(s, &one, 1u);
    while (s->used != 56u) {
        zw_sha1_update(s, &zero, 1u);
    }
    uint8_t len[8];
    for (unsigned i = 0u; i < 8u; i++) {
        len[i] = (uint8_t)(bits >> (56u - i * 8u));
    }
    zw_sha1_update(s, len, sizeof len);
    for (unsigned i = 0u; i < 5u; i++) {
        out[i * 4u] = (uint8_t)(s->h[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(s->h[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(s->h[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)s->h[i];
    }
}

void zw_sha1(const void *data, size_t len, uint8_t out[20])
{
    zw_sha1_t s;
    zw_sha1_init(&s);
    zw_sha1_update(&s, data, len);
    zw_sha1_final(&s, out);
}
