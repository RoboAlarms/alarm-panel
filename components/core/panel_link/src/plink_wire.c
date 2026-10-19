/*
 * plink_wire.c - little-endian byte writing and reading for the panel link (plink_wire.h,
 * docs/features/11 "Messages"; LINK-017): writers and readers that stop at the first overrun,
 * sections a newer peer may lengthen, and names cleaned before anything shows them.
 */
#include "panel_link/plink_wire.h"

#include <string.h>

#define SECTION_MAX 255u /* a section's or a string's length is one byte */

/* ---- writing ------------------------------------------------------------------------------ */

void plink_w_init(plink_w_t *w, uint8_t *buf, size_t cap)
{
    w->p = buf;
    w->cap = buf != NULL ? cap : 0u;
    w->len = 0;
    w->ok = true;
}

/* n bytes of room at the end, or NULL (and ok = false from now on) when they don't fit. */
static uint8_t *w_room(plink_w_t *w, size_t n)
{
    if (!w->ok || n > w->cap - w->len) {
        w->ok = false;
        return NULL;
    }
    uint8_t *at = w->p + w->len;
    w->len += n;
    return at;
}

/* v as n little-endian bytes. */
static void w_le(plink_w_t *w, uint64_t v, size_t n)
{
    uint8_t *at = w_room(w, n);
    if (at == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        at[i] = (uint8_t)(v >> (8u * i));
    }
}

void plink_w_u8(plink_w_t *w, uint8_t v)
{
    w_le(w, v, 1);
}

void plink_w_u16(plink_w_t *w, uint16_t v)
{
    w_le(w, v, 2);
}

void plink_w_u32(plink_w_t *w, uint32_t v)
{
    w_le(w, v, 4);
}

void plink_w_u64(plink_w_t *w, uint64_t v)
{
    w_le(w, v, 8);
}

void plink_w_i64(plink_w_t *w, int64_t v)
{
    w_le(w, (uint64_t)v, 8); /* two's complement on the wire */
}

void plink_w_bytes(plink_w_t *w, const void *p, size_t len)
{
    if (len == 0u) {
        return;
    }
    uint8_t *at = w_room(w, len);
    if (at != NULL) {
        memcpy(at, p, len);
    }
}

void plink_w_str(plink_w_t *w, const char *s, size_t max)
{
    if (max > SECTION_MAX) {
        max = SECTION_MAX;
    }
    size_t n = 0;
    if (s != NULL) {
        while (n < max && s[n] != '\0') {
            n++;
        }
        /* Longer than max: never leave half a character, so back off to the start of the one
         * the cut falls in (its continuation bytes are 10xxxxxx). */
        if (n == max && s[n] != '\0') {
            while (n > 0u && ((unsigned)(unsigned char)s[n] & 0xC0u) == 0x80u) {
                n--;
            }
        }
    }
    plink_w_u8(w, (uint8_t)n);
    plink_w_bytes(w, s, n);
}

size_t plink_w_section(plink_w_t *w)
{
    const size_t at = w->len;
    plink_w_u8(w, 0); /* the length, filled by plink_w_section_end */
    return at;
}

void plink_w_section_end(plink_w_t *w, size_t at)
{
    if (!w->ok) {
        return;
    }
    if (at >= w->len || w->len - at - 1u > SECTION_MAX) {
        w->ok = false;
        return;
    }
    w->p[at] = (uint8_t)(w->len - at - 1u);
}

/* ---- reading ------------------------------------------------------------------------------ */

void plink_r_init(plink_r_t *r, const uint8_t *buf, size_t len)
{
    r->p = buf;
    r->len = buf != NULL ? len : 0u;
    r->pos = 0;
    r->ok = true;
    r->pad = false;
}

size_t plink_r_left(const plink_r_t *r)
{
    return r->ok ? r->len - r->pos : 0u;
}

/* The next n bytes into out. Short: a section's reader (pad) gives zeros past its end and stays
 * ok; any other reader gives zeros and is not ok from now on. */
static void r_get(plink_r_t *r, uint8_t *out, size_t n)
{
    const size_t left = plink_r_left(r);
    if (r->ok && n <= left) {
        if (n > 0u) {
            memcpy(out, r->p + r->pos, n);
            r->pos += n;
        }
        return;
    }
    if (r->ok && r->pad) {
        if (left > 0u) {
            memcpy(out, r->p + r->pos, left);
        }
        memset(out + left, 0, n - left);
        r->pos = r->len;
        return;
    }
    r->ok = false;
    if (n > 0u) {
        memset(out, 0, n);
    }
}

static uint64_t r_le(plink_r_t *r, size_t n)
{
    uint8_t b[8];
    r_get(r, b, n);
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)(unsigned)b[i] << (8u * i);
    }
    return v;
}

uint8_t plink_r_u8(plink_r_t *r)
{
    return (uint8_t)r_le(r, 1);
}

uint16_t plink_r_u16(plink_r_t *r)
{
    return (uint16_t)r_le(r, 2);
}

uint32_t plink_r_u32(plink_r_t *r)
{
    return (uint32_t)r_le(r, 4);
}

uint64_t plink_r_u64(plink_r_t *r)
{
    return r_le(r, 8);
}

int64_t plink_r_i64(plink_r_t *r)
{
    const uint64_t u = r_le(r, 8);
    /* Two's complement back without an implementation-defined conversion. */
    if (u <= (uint64_t)INT64_MAX) {
        return (int64_t)u;
    }
    return -(int64_t)(~u) - 1;
}

void plink_r_bytes(plink_r_t *r, void *out, size_t len)
{
    r_get(r, out, len);
}

const uint8_t *plink_r_take(plink_r_t *r, size_t len)
{
    if (!r->ok) {
        return NULL;
    }
    if (len > r->len - r->pos) {
        r->ok = false;
        return NULL;
    }
    const uint8_t *at = r->p != NULL ? r->p + r->pos : NULL;
    r->pos += len;
    return at;
}

void plink_r_str(plink_r_t *r, char *out, size_t cap)
{
    size_t n = plink_r_u8(r);
    const size_t left = plink_r_left(r);
    if (n > left) {
        if (!r->pad) {
            r->ok = false;
        }
        n = left; /* a section's reader: what is there; the zeros past it are dropped anyway */
    }
    const uint8_t *s = plink_r_take(r, n);
    plink_clean_text((const char *)s, s != NULL ? n : 0u, out, cap);
}

plink_r_t plink_r_section(plink_r_t *r)
{
    plink_r_t s;
    plink_r_init(&s, NULL, 0);
    s.pad = true;
    size_t n = plink_r_u8(r);
    if (!r->ok) {
        s.ok = false;
        return s;
    }
    const size_t left = plink_r_left(r);
    if (n > left) {
        if (!r->pad) {
            r->ok = false;
            s.ok = false;
            return s;
        }
        n = left; /* inside a short section: the rest reads as zeros */
    }
    if (n > 0u) {
        s.p = r->p + r->pos;
        s.len = n;
        r->pos += n;
    }
    return s;
}

/* ---- text safe to show -------------------------------------------------------------------- */

/* One UTF-8 character at s (left bytes): its length and code point, or 0 when the bytes are not
 * a well-formed character (Unicode 15 Table 3-7: no overlong forms, no surrogates, nothing past
 * U+10FFFF, no sequence cut short). */
static size_t utf8_char(const uint8_t *s, size_t left, uint32_t *cp)
{
    const unsigned b0 = s[0];
    size_t n;
    unsigned lo = 0x80u;
    unsigned hi = 0xBFu;
    uint32_t v;
    if (b0 < 0x80u) {
        *cp = b0;
        return 1;
    }
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        n = 2;
        v = b0 & 0x1Fu;
    } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
        n = 3;
        v = b0 & 0x0Fu;
        if (b0 == 0xE0u) {
            lo = 0xA0u; /* no overlong forms */
        } else if (b0 == 0xEDu) {
            hi = 0x9Fu; /* no surrogates */
        }
    } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
        n = 4;
        v = b0 & 0x07u;
        if (b0 == 0xF0u) {
            lo = 0x90u; /* no overlong forms */
        } else if (b0 == 0xF4u) {
            hi = 0x8Fu; /* nothing past U+10FFFF */
        }
    } else {
        return 0; /* a continuation byte, C0, C1 or F5..FF */
    }
    if (left < n) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        const unsigned b = s[i];
        if (b < lo || b > hi) {
            return 0;
        }
        lo = 0x80u;
        hi = 0xBFu;
        v = (v << 6) | (b & 0x3Fu);
    }
    *cp = v;
    return n;
}

/* Shown on a screen: no C0 or C1 control, no DEL, no bidi control (Unicode's Bidi_Control set:
 * ALM U+061C, LRM/RLM U+200E/F, the embeddings and overrides U+202A..E, the isolates
 * U+2066..9), so a name from the network can't hide or reorder the words around it. */
static bool utf8_shown(uint32_t cp)
{
    if (cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu)) {
        return false;
    }
    if (cp == 0x061Cu || cp == 0x200Eu || cp == 0x200Fu) {
        return false;
    }
    if ((cp >= 0x202Au && cp <= 0x202Eu) || (cp >= 0x2066u && cp <= 0x2069u)) {
        return false;
    }
    return true;
}

void plink_clean_text(const char *in, size_t in_len, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return;
    }
    const uint8_t *s = (const uint8_t *)in;
    size_t o = 0;
    size_t i = 0;
    while (s != NULL && i < in_len) {
        uint32_t cp = 0;
        const size_t n = utf8_char(s + i, in_len - i, &cp);
        if (n == 0u) {
            i++; /* not UTF-8: drop the byte and look again at the next */
            continue;
        }
        if (utf8_shown(cp)) {
            if (n > cap - 1u - o) {
                break; /* full: cut before a character that doesn't fit whole */
            }
            memcpy(out + o, s + i, n);
            o += n;
        }
        i += n;
    }
    out[o] = '\0';
}
