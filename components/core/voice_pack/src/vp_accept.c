// vp_accept.c - the panel's acceptance of a voice pack and its immutable identity (vp_accept.h).
#include "voice_pack/vp_accept.h"

#include <string.h>

vp_accept_t vp_accept(const vp_pack_t *pack, bool release)
{
    if (strcmp(pack->locale, VP_ACCEPT_LOCALE) != 0) {
        return VP_ACCEPT_LOCALE_OTHER;
    }
    if (strcmp(pack->voice_id, VP_ACCEPT_VOICE_ID) != 0) {
        return VP_ACCEPT_VOICE_OTHER;
    }
    if (release && vp_is_draft(pack)) {
        return VP_ACCEPT_DRAFT_RELEASE;
    }
    return VP_ACCEPT_OK;
}

const char *vp_accept_name(vp_accept_t a)
{
    static const char *const names[] = {"ok", "locale", "voice", "draft"};
    return (unsigned)a < sizeof(names) / sizeof(names[0]) ? names[a] : "?";
}

/* Copies a NUL-terminated string into a zero-padded field (truncating; vp_open() already bounds it). */
static void put_str(char *dst, size_t size, const char *src)
{
    memset(dst, 0, size);
    for (size_t i = 0u; i + 1u < size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
}

void vp_identity_of(const vp_pack_t *pack, const uint8_t sha256[VP_DIGEST_SIZE], vp_identity_t *out)
{
    memset(out, 0, sizeof(*out));
    put_str(out->locale, sizeof(out->locale), pack->locale);
    put_str(out->voice_id, sizeof(out->voice_id), pack->voice_id);
    out->revision = pack->pack_revision;
    memcpy(out->sha256, sha256, VP_DIGEST_SIZE);
}

static size_t field_len(const char *s, size_t size)
{
    size_t n = 0u;
    while (n < size && s[n] != '\0') {
        n++;
    }
    return n;
}

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void vp_identity_key(const vp_identity_t *id, char out[VP_ID_KEY_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t h = 2166136261u;
    h = fnv1a(h, (const uint8_t *)id->locale, field_len(id->locale, sizeof(id->locale)));
    h = fnv1a(h, (const uint8_t *)"", 1u);
    h = fnv1a(h, (const uint8_t *)id->voice_id, field_len(id->voice_id, sizeof(id->voice_id)));
    h = fnv1a(h, (const uint8_t *)"", 1u);
    const uint8_t rev[4] = {(uint8_t)(id->revision & 0xFFu), (uint8_t)((id->revision >> 8) & 0xFFu),
                            (uint8_t)((id->revision >> 16) & 0xFFu), (uint8_t)((id->revision >> 24) & 0xFFu)};
    h = fnv1a(h, rev, sizeof(rev));
    out[0] = 'v';
    for (unsigned i = 0u; i < 8u; i++) {
        out[1u + i] = hex[(h >> (28u - 4u * i)) & 0xFu];
    }
    out[9] = '\0';
}

void vp_identity_encode(const vp_identity_t *id, uint8_t out[VP_ID_RECORD_SIZE])
{
    memset(out, 0, VP_ID_RECORD_SIZE);
    out[0] = (uint8_t)VP_ID_RECORD_FORMAT;
    out[4] = (uint8_t)(id->revision & 0xFFu);
    out[5] = (uint8_t)((id->revision >> 8) & 0xFFu);
    out[6] = (uint8_t)((id->revision >> 16) & 0xFFu);
    out[7] = (uint8_t)((id->revision >> 24) & 0xFFu);
    put_str((char *)&out[8], VP_LOCALE_SIZE, id->locale);
    put_str((char *)&out[24], VP_VOICE_ID_SIZE, id->voice_id);
    memcpy(&out[56], id->sha256, VP_DIGEST_SIZE);
}

/* A field holds a non-empty NUL-terminated string followed only by zeros. */
static bool field_ok(const uint8_t *p, size_t size)
{
    size_t n = 0u;
    while (n < size && p[n] != 0u) {
        n++;
    }
    if (n == 0u || n == size) {
        return false;
    }
    for (size_t i = n; i < size; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

bool vp_identity_decode(const uint8_t *rec, size_t len, vp_identity_t *out)
{
    if (rec == NULL || len != VP_ID_RECORD_SIZE || rec[0] != (uint8_t)VP_ID_RECORD_FORMAT || rec[1] != 0u ||
        rec[2] != 0u || rec[3] != 0u || !field_ok(&rec[8], VP_LOCALE_SIZE) ||
        !field_ok(&rec[24], VP_VOICE_ID_SIZE)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->revision = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8) | ((uint32_t)rec[6] << 16) | ((uint32_t)rec[7] << 24);
    memcpy(out->locale, &rec[8], VP_LOCALE_SIZE);
    memcpy(out->voice_id, &rec[24], VP_VOICE_ID_SIZE);
    memcpy(out->sha256, &rec[56], VP_DIGEST_SIZE);
    return true;
}

vp_id_verdict_t vp_identity_judge(const vp_identity_t *stored, bool stored_ok, const vp_identity_t *cand)
{
    if (stored == NULL) {
        return VP_ID_NEW;
    }
    if (!stored_ok || stored->revision != cand->revision ||
        memcmp(stored->locale, cand->locale, sizeof(stored->locale)) != 0 ||
        memcmp(stored->voice_id, cand->voice_id, sizeof(stored->voice_id)) != 0) {
        return VP_ID_COLLISION;
    }
    return memcmp(stored->sha256, cand->sha256, VP_DIGEST_SIZE) == 0 ? VP_ID_SAME : VP_ID_OTHER_AUDIO;
}

const char *vp_id_verdict_name(vp_id_verdict_t v)
{
    static const char *const names[] = {"new", "same", "other_audio", "collision"};
    return (unsigned)v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}
