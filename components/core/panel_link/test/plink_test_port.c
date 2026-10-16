/*
 * plink_test_port.c - see plink_test_port.h.
 */
#include "plink_test_port.h"

#include <string.h>

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* A CCM counter block with L = 2 (RFC 3610 §2.3): flags 0x01, the 13-byte nonce, counter 0. */
static bool is_a0(const uint8_t in[16])
{
    return in[0] == 0x01u && in[14] == 0u && in[15] == 0u;
}

static bool t_aes(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    plink_test_port_t *t = ctx;
    t->aes_calls++;
    if (is_a0(in)) {
        const uint32_t h = fnv1a(fnv1a(2166136261u, key, 16), in, 16);
        for (uint32_t i = 0; i < t->seen_count; i++) {
            if (t->seen[i] == h) {
                t->reused = true;
            }
        }
        if (t->seen_count < PLINK_TEST_CTR_SEEN) {
            t->seen[t->seen_count++] = h;
        }
    }
    return t->zw.aes128_encrypt(t->zw.ctx, key, in, out);
}

static bool t_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    plink_test_port_t *t = ctx;
    return t->zw.x25519(t->zw.ctx, out, scalar, point);
}

static bool t_random(void *ctx, uint8_t *buf, size_t len)
{
    plink_test_port_t *t = ctx;
    return t->zw.random(t->zw.ctx, buf, len);
}

void plink_test_port_init(plink_test_port_t *t, uint64_t seed)
{
    memset(t, 0, sizeof(*t));
    zw_host_crypto_init(&t->hc, seed);
    t->zw = zw_host_crypto_port(&t->hc);
}

plink_crypto_t plink_test_port(plink_test_port_t *t)
{
    const plink_crypto_t c = {
        .ctx = t, .aes128_encrypt = t_aes, .x25519 = t_x25519, .random = t_random,
        .insecure = true};
    return c;
}

void plink_test_port_fail_at(plink_test_port_t *t, uint32_t call)
{
    zw_host_crypto_fail_at(&t->hc, call);
}
