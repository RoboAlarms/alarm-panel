/*
 * zw_sha1.h - SHA-1 (FIPS 180-4), only for the SmartStart QR code's checksum (docs/features/36,
 * network.md §4.2): the first two bytes of SHA-1 over the code's digits. Nothing secret is
 * hashed with it and it proves nothing about security; it tells a mistyped or damaged code
 * from a good one. Portable C11, caller-owned state.
 */
#ifndef ZW_SHA1_H
#define ZW_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[5];
    uint64_t bytes;     /* hashed so far */
    uint8_t block[64];
    size_t used;        /* bytes waiting in `block` */
} zw_sha1_t;

void zw_sha1_init(zw_sha1_t *s);
void zw_sha1_update(zw_sha1_t *s, const void *data, size_t len);
void zw_sha1_final(zw_sha1_t *s, uint8_t out[20]);
/* The three together. */
void zw_sha1(const void *data, size_t len, uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif /* ZW_SHA1_H */
