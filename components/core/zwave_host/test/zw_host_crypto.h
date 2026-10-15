/*
 * zw_host_crypto.h - the Z-Wave crypto port on a PC (docs/features/36 README "Components",
 * ZW-008): AES-128 encryption (FIPS-197) and X25519 (RFC 7748 §5), written from the standards
 * for this project, and a deterministic, seedable random generator, so the host tests - and
 * later the PC simulator - run zwave_host's portable S2/S0 code as the panel will. The
 * firmware's port is PSA (mbedTLS 4), milestone Z8.
 *
 * Not for a panel: the random numbers are SplitMix64 (predictable on purpose, so a test repeats
 * itself) and the AES looks bytes up in a table (not constant-time). The X25519 ladder swaps in
 * constant time.
 *
 * Fault tests can make one call of the port fail (zw_host_crypto_fail_at): that call fills its
 * output with junk and returns false, as a failing PSA call may.
 */
#ifndef ZW_HOST_CRYPTO_H
#define ZW_HOST_CRYPTO_H

#include <stdint.h>

#include "zwave_host/zw_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_HOST_NEVER UINT32_MAX /* fail_at: no call fails */

typedef struct {
    uint8_t sbox[256]; /* FIPS-197 §5.1.1, computed by zw_host_crypto_init */
    uint64_t rng;      /* SplitMix64 state */
    uint32_t calls;    /* port calls (all three primitives) since init or the last fail_at */
    uint32_t fail_at;  /* the call, counted from 0, that fails; ZW_HOST_NEVER for none */
} zw_host_crypto_t;

/* Ready to use, random numbers from `seed`, no call failing. */
void zw_host_crypto_init(zw_host_crypto_t *hc, uint64_t seed);

/* The port over hc (hc must outlive it). */
zw_crypto_port_t zw_host_crypto_port(zw_host_crypto_t *hc);

/* Restarts the call count and makes call number `call` (from 0) fail; ZW_HOST_NEVER disarms. */
void zw_host_crypto_fail_at(zw_host_crypto_t *hc, uint32_t call);

#ifdef __cplusplus
}
#endif

#endif /* ZW_HOST_CRYPTO_H */
