/*
 * zw_crypto_util.c - constant-time comparison and wiping (see zw_crypto.h, "Hygiene").
 */
#include "zwave_host/zw_crypto.h"

bool zw_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    /* Volatile reads and no early exit: every byte is read whatever the data says. */
    const volatile uint8_t *va = a;
    const volatile uint8_t *vb = b;
    unsigned diff = 0u;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)va[i] ^ (unsigned)vb[i];
    }
    return diff == 0u;
}

void zw_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    for (size_t i = 0; i < n; i++) {
        v[i] = 0u;
    }
}
