/*
 * proto_sha256.h - SHA-256 (FIPS 180-4) over a message given in pieces, for HMAC and HKDF and for
 * transcripts that are never copied into one buffer (components/core/panel_link, docs/features/11).
 * The one-shot proto_sha256() (proto_link.h) runs on the same code.
 *
 * Portable C11: no heap, no globals. The context holds what was hashed so far: one that hashed a
 * secret (an HMAC key, a shared secret) is the caller's to wipe.
 */
#ifndef PROTO_SHA256_H
#define PROTO_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SHA256_LEN   32u
#define PROTO_SHA256_BLOCK 64u

typedef struct {
    uint32_t h[8];
    uint64_t total; /* message bytes so far */
    uint8_t buf[64];
    size_t buf_len;
} proto_sha256_ctx_t;

void proto_sha256_init(proto_sha256_ctx_t *s);
/* Adds len bytes (p may be NULL when len is 0). */
void proto_sha256_update(proto_sha256_ctx_t *s, const void *p, size_t len);
/* The digest of everything added since init; the context must be initialised again to reuse. */
void proto_sha256_final(proto_sha256_ctx_t *s, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_SHA256_H */
