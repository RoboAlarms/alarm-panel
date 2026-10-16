/*
 * plink_test_port.h - the panel link's crypto port for host tests: zwave_host's PC primitives
 * (AES from FIPS-197, X25519 from RFC 7748, seeded SplitMix64 random numbers) behind a
 * plink_crypto_t, marked insecure, and a watch on AES-CCM: every CCM operation starts by
 * encrypting its counter block A_0 = flags || nonce || 0, so a (key, A_0) pair seen twice on one
 * side means a nonce was used twice with one key (LINK-003, docs/features/11 "Sessions"). Each
 * side of a test has its own port, so a frame sealed by one side and opened by the other is not
 * a repeat; a side never opens a frame twice (the replay window comes first).
 */
#ifndef PLINK_TEST_PORT_H
#define PLINK_TEST_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"
#include "zw_host_crypto.h"

#define PLINK_TEST_CTR_SEEN 4096u

typedef struct {
    zw_host_crypto_t hc;
    zw_crypto_port_t zw;
    uint32_t seen[PLINK_TEST_CTR_SEEN]; /* FNV-1a of key || A_0 */
    uint32_t seen_count;
    bool reused;      /* a (key, A_0) came twice */
    uint32_t aes_calls;
} plink_test_port_t;

/* Ready, random numbers from seed. */
void plink_test_port_init(plink_test_port_t *t, uint64_t seed);
/* The port over t (insecure = true). */
plink_crypto_t plink_test_port(plink_test_port_t *t);
/* Makes the n-th primitive call from now fail (zw_host_crypto_fail_at). */
void plink_test_port_fail_at(plink_test_port_t *t, uint32_t call);

#endif /* PLINK_TEST_PORT_H */
