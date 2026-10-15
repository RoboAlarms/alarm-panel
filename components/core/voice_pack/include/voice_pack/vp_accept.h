/*
 * vp_accept.h - what the panel accepts of a pack vp_open_signed() has read (docs/features/08
 * voice-first-build.md D08, D09, D12; AUD-024, AUD-027), and the pack's immutable identity.
 *
 * Acceptance: the first build speaks one voice, en-US "qwen3-designed_female"; a release build
 * refuses a DRAFT pack, a dev build takes it. The boot load and the install use the same check,
 * before any write or activation.
 *
 * Identity: locale, voice ID and revision name a pack; the SHA-256 of its unpadded payload
 * (payload_length bytes) is what that name means, for good: a known identity with another digest
 * is refused, so approved spoken names never change their audio silently. The panel keeps one
 * record per identity (88 bytes, below) under a short key made from the identity; a record found
 * under the key that names another identity is a collision and is refused too.
 *
 * The record is a stored format (NVS): a record one firmware writes must read on later ones.
 * Never move a field; a format change takes a new format byte and a new frozen test vector.
 *
 * Plain C11, portable (components/core rules): no heap, no state.
 */
#ifndef VOICE_PACK_VP_ACCEPT_H
#define VOICE_PACK_VP_ACCEPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_pack/voice_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one voice of the first build (D08). */
#define VP_ACCEPT_LOCALE "en-US"
#define VP_ACCEPT_VOICE_ID "qwen3-designed_female"

typedef enum {
    VP_ACCEPT_OK = 0,
    VP_ACCEPT_LOCALE_OTHER,  /* not en-US */
    VP_ACCEPT_VOICE_OTHER,   /* not qwen3-designed_female */
    VP_ACCEPT_DRAFT_RELEASE, /* a DRAFT pack on a release build */
} vp_accept_t;

/* Judges an accepted pack (vp_open_signed() returned VP_OK). `release`: the build is a release. */
vp_accept_t vp_accept(const vp_pack_t *pack, bool release);

/* "ok" "locale" "voice" "draft" */
const char *vp_accept_name(vp_accept_t a);

/* ---- Identity records ---- */
#define VP_DIGEST_SIZE 32u
#define VP_ID_RECORD_FORMAT 1u
#define VP_ID_RECORD_SIZE 88u /* format u8, 3 zero, revision u32 LE, locale[16], voice_id[32], sha256[32] */
#define VP_ID_KEY_SIZE 10u    /* "v" + 8 lowercase hex digits + NUL: an NVS key (at most 15) */

typedef struct {
    char locale[VP_LOCALE_SIZE];     /* NUL-terminated, zero-padded */
    char voice_id[VP_VOICE_ID_SIZE]; /* NUL-terminated, zero-padded */
    uint32_t revision;
    uint8_t sha256[VP_DIGEST_SIZE];  /* of the unpadded payload (payload_length bytes) */
} vp_identity_t;

/* The identity of an accepted pack, with the digest of its payload (computed by the caller). */
void vp_identity_of(const vp_pack_t *pack, const uint8_t sha256[VP_DIGEST_SIZE], vp_identity_t *out);

/* The record's key: "v" and the FNV-1a 32 of locale, NUL, voice ID, NUL, revision (u32 LE), in
 * lowercase hex. Different identities can share a key: vp_identity_judge() says so. */
void vp_identity_key(const vp_identity_t *id, char out[VP_ID_KEY_SIZE]);

/* The record's bytes. */
void vp_identity_encode(const vp_identity_t *id, uint8_t out[VP_ID_RECORD_SIZE]);

/* Reads a record; false when its size, format byte, zero bytes or strings are wrong. */
bool vp_identity_decode(const uint8_t *rec, size_t len, vp_identity_t *out);

typedef enum {
    VP_ID_NEW = 0,     /* no record under the key: reserve one */
    VP_ID_SAME,        /* the same identity and digest: a reinstall, nothing to write */
    VP_ID_OTHER_AUDIO, /* the same identity with another digest: refuse */
    VP_ID_COLLISION,   /* the key holds another identity (or an unreadable record): refuse */
} vp_id_verdict_t;

/* Judges a candidate against what the key holds: `stored` NULL when there is no record,
 * `stored_ok` false when there is one that does not decode. */
vp_id_verdict_t vp_identity_judge(const vp_identity_t *stored, bool stored_ok, const vp_identity_t *cand);

/* "new" "same" "other_audio" "collision" */
const char *vp_id_verdict_name(vp_id_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_PACK_VP_ACCEPT_H */
