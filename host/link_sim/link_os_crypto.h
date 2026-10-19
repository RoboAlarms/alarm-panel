/*
 * link_os_crypto.h - the panel link's crypto port on a PC for host/link_sim (docs/features/11
 * "Design", LINK-013): AES-128 and X25519 from zwave_host's PC primitives
 * (components/core/zwave_host/test/zw_host_crypto.c, written from FIPS-197 and RFC 7748) and
 * random numbers from the operating system (BCryptGenRandom on Windows, getrandom or /dev/urandom
 * elsewhere), so insecure is false and the tools talk to panels on the LAN.
 *
 * For a PC tool on a bench, not a hardened implementation: the AES looks bytes up in a table (not
 * constant-time). A panel uses PSA (components/services/plink_service).
 */
#ifndef LINK_OS_CRYPTO_H
#define LINK_OS_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"
#include "zw_host_crypto.h"

typedef struct {
    zw_host_crypto_t hc; /* the AES and X25519 primitives (its seeded generator is never used) */
    zw_crypto_port_t zw;
    uint32_t random_failures;
} link_os_crypto_t;

/* Ready; false when the OS has no random numbers to give (the port then fails every draw). */
bool link_os_crypto_init(link_os_crypto_t *oc);
/* The port over oc (insecure = false). */
plink_crypto_t link_os_crypto(link_os_crypto_t *oc);
/* len bytes from the OS's cryptographically secure generator. */
bool link_os_random(void *buf, size_t len);

#endif /* LINK_OS_CRYPTO_H */
