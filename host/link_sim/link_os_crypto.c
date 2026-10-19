/*
 * link_os_crypto.c - see link_os_crypto.h.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#include "link_os_crypto.h"

#include <string.h>

#if !defined(_WIN32)
static bool urandom(uint8_t *p, size_t len)
{
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    size_t got = 0;
    while (got < len) {
        const ssize_t n = read(fd, p + got, len - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            return false;
        }
        got += (size_t)n;
    }
    close(fd);
    return true;
}
#endif

bool link_os_random(void *buf, size_t len)
{
    uint8_t *p = buf;
#if defined(_WIN32)
    while (len > 0u) {
        const ULONG n = len > 0x10000u ? 0x10000u : (ULONG)len;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
#elif defined(__linux__)
    size_t got = 0;
    while (got < len) {
        const ssize_t n = getrandom(p + got, len - got, 0u);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == ENOSYS) {
            return urandom(p + got, len - got); /* a kernel older than 3.17 */
        }
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
#else
    return urandom(p, len);
#endif
}

/* zwave_host's PC primitives count their calls to fail one on purpose in tests (never set
 * here); keeping the count at 0 means a long run can never reach that call. */
static bool os_aes(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    link_os_crypto_t *oc = ctx;
    oc->hc.calls = 0u;
    return oc->zw.aes128_encrypt(oc->zw.ctx, key, in, out);
}

static bool os_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    link_os_crypto_t *oc = ctx;
    oc->hc.calls = 0u;
    return oc->zw.x25519(oc->zw.ctx, out, scalar, point);
}

static bool os_random(void *ctx, uint8_t *buf, size_t len)
{
    link_os_crypto_t *oc = ctx;
    if (!link_os_random(buf, len)) {
        memset(buf, 0, len);
        oc->random_failures++;
        return false;
    }
    return true;
}

bool link_os_crypto_init(link_os_crypto_t *oc)
{
    memset(oc, 0, sizeof(*oc));
    zw_host_crypto_init(&oc->hc, 0u);
    oc->zw = zw_host_crypto_port(&oc->hc);
    uint8_t probe[16];
    const bool ok = link_os_random(probe, sizeof(probe));
    memset(probe, 0, sizeof(probe));
    return ok;
}

plink_crypto_t link_os_crypto(link_os_crypto_t *oc)
{
    const plink_crypto_t c = {
        .ctx = oc, .aes128_encrypt = os_aes, .x25519 = os_x25519, .random = os_random,
        .insecure = false};
    return c;
}
