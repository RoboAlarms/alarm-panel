/*
 * zw_crypto_internal.h - helpers the crypto sources share (private to zwave_host).
 */
#ifndef ZW_CRYPTO_INTERNAL_H
#define ZW_CRYPTO_INTERNAL_H

#include <string.h>

#include "zwave_host/zw_crypto.h"

/* The port can encrypt a block. */
static inline bool zw_has_aes(const zw_crypto_port_t *port)
{
    return port != NULL && port->aes128_encrypt != NULL;
}

/* out = E(key, in) through the port; out never overlaps key or in (the port's contract). */
static inline bool zw_aes_block(const zw_crypto_port_t *port, const uint8_t key[16],
                                const uint8_t in[16], uint8_t out[16])
{
    return port->aes128_encrypt(port->ctx, key, in, out);
}

/* One CBC step: x = E(key, x XOR b). */
static inline bool zw_cbc_step(const zw_crypto_port_t *port, const uint8_t key[16], uint8_t x[16],
                               const uint8_t b[16])
{
    uint8_t t[16];
    for (size_t i = 0; i < 16u; i++) {
        t[i] = (uint8_t)(x[i] ^ b[i]);
    }
    const bool ok = zw_aes_block(port, key, t, x);
    zw_wipe(t, sizeof t);
    return ok;
}

/* Zeroes an output a failed call was given, when there is one. */
static inline void zw_wipe_out(void *p, size_t n)
{
    if (p != NULL) {
        zw_wipe(p, n);
    }
}

#endif /* ZW_CRYPTO_INTERNAL_H */
