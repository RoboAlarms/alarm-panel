/* i18n_utf8.c - UTF-8 helpers (I18N-010). */
#include <string.h>

#include "i18n/i18n.h"
#include "i18n_internal.h"

static bool cont(const unsigned char *s, size_t avail, size_t k, unsigned lo, unsigned hi)
{
    return k < avail && (unsigned)s[k] >= lo && (unsigned)s[k] <= hi;
}

size_t i18n_utf8_seq(const unsigned char *s, size_t avail)
{
    if (avail == 0u) {
        return 0u;
    }
    unsigned b = (unsigned)s[0];
    if (b < 0x80u) {
        return 1u;
    }
    if (b >= 0xC2u && b <= 0xDFu) {
        return cont(s, avail, 1u, 0x80u, 0xBFu) ? 2u : 0u;
    }
    if (b >= 0xE0u && b <= 0xEFu) {
        unsigned lo = (b == 0xE0u) ? 0xA0u : 0x80u;
        unsigned hi = (b == 0xEDu) ? 0x9Fu : 0xBFu;
        return (cont(s, avail, 1u, lo, hi) && cont(s, avail, 2u, 0x80u, 0xBFu)) ? 3u : 0u;
    }
    if (b >= 0xF0u && b <= 0xF4u) {
        unsigned lo = (b == 0xF0u) ? 0x90u : 0x80u;
        unsigned hi = (b == 0xF4u) ? 0x8Fu : 0xBFu;
        return (cont(s, avail, 1u, lo, hi) && cont(s, avail, 2u, 0x80u, 0xBFu) &&
                cont(s, avail, 3u, 0x80u, 0xBFu))
                   ? 4u
                   : 0u;
    }
    return 0u;
}

/* One character's length at a NUL-terminated string: an invalid byte is one. */
static size_t char_len(const unsigned char *s)
{
    size_t l = i18n_utf8_seq(s, 4u);
    return l ? l : 1u;
}

size_t i18n_utf8_fit(const char *s, size_t max_bytes)
{
    if (s == NULL) {
        return 0u;
    }
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0u;
    while (i < max_bytes && p[i] != 0u) {
        size_t l = char_len(p + i);
        if (i + l > max_bytes) {
            break;
        }
        i += l;
    }
    return i;
}

size_t i18n_utf8_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0u) {
        return 0u;
    }
    if (src == NULL) {
        src = "";
    }
    size_t n = i18n_utf8_fit(src, dst_len - 1u);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

size_t i18n_utf8_chars(const char *s)
{
    if (s == NULL) {
        return 0u;
    }
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0u;
    size_t i = 0u;
    while (p[i] != 0u) {
        i += char_len(p + i);
        n++;
    }
    return n;
}

/* The code point at s (a bad byte is itself, as U+DC80..U+DCFF would never be), and its length. */
static uint32_t decode(const unsigned char *s, size_t avail, size_t *len)
{
    const size_t l = i18n_utf8_seq(s, avail);
    if (l == 0u) {
        *len = 1u;
        return 0xDC00u | (uint32_t)s[0];
    }
    *len = l;
    if (l == 1u) {
        return s[0];
    }
    uint32_t c = (uint32_t)s[0] & (0x7Fu >> l);
    for (size_t k = 1; k < l; k++) {
        c = (c << 6) | ((uint32_t)s[k] & 0x3Fu);
    }
    return c;
}

/* Simple case folding, capital to small, for the scripts the panel's fonts carry. */
static uint32_t fold(uint32_t c)
{
    if ((c >= 0x41u && c <= 0x5Au) || (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)) {
        return c + 0x20u; /* ASCII; Latin-1 but for the multiplication sign */
    }
    if (c < 0x100u) {
        return c;
    }
    if (c <= 0x17Fu) { /* Latin Extended-A: mostly capital, small pairs */
        if (c == 0x130u) {
            return 0x69u; /* dotted capital I: i */
        }
        if (c == 0x178u) {
            return 0xFFu; /* Y with diaeresis: its small letter is in Latin-1 */
        }
        if (c == 0x17Fu) {
            return 0x73u; /* long s */
        }
        if ((c <= 0x137u && c % 2u == 0u) || (c >= 0x14Au && c <= 0x177u && c % 2u == 0u) ||
            (((c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17Eu)) && c % 2u == 1u)) {
            return c + 1u;
        }
        return c;
    }
    if (c >= 0x391u && c <= 0x3A9u && c != 0x3A2u) {
        return c + 0x20u; /* Greek */
    }
    if (c >= 0x410u && c <= 0x42Fu) {
        return c + 0x20u; /* Cyrillic */
    }
    if (c >= 0x400u && c <= 0x40Fu) {
        return c + 0x50u;
    }
    return c;
}

bool i18n_utf8_equal_fold(const char *a, size_t a_len, const char *b, size_t b_len)
{
    const unsigned char *p = (const unsigned char *)(a != NULL ? a : "");
    const unsigned char *q = (const unsigned char *)(b != NULL ? b : "");
    size_t i = 0u;
    size_t j = 0u;
    while (i < a_len && j < b_len && p[i] != 0u && q[j] != 0u) {
        size_t li;
        size_t lj;
        const uint32_t x = decode(p + i, a_len - i, &li);
        const uint32_t y = decode(q + j, b_len - j, &lj);
        if (fold(x) != fold(y)) {
            return false;
        }
        i += li;
        j += lj;
    }
    const bool a_done = i >= a_len || p[i] == 0u;
    const bool b_done = j >= b_len || q[j] == 0u;
    return a_done && b_done;
}

bool i18n_utf8_contains_fold(const char *haystack, const char *needle)
{
    const unsigned char *h = (const unsigned char *)(haystack != NULL ? haystack : "");
    const unsigned char *n = (const unsigned char *)(needle != NULL ? needle : "");
    if (n[0] == 0u) {
        return true;
    }
    for (size_t at = 0u; h[at] != 0u; at += char_len(h + at)) {
        size_t i = at;
        size_t j = 0u;
        while (h[i] != 0u && n[j] != 0u) {
            size_t li;
            size_t lj;
            const uint32_t x = decode(h + i, 4u, &li);
            const uint32_t y = decode(n + j, 4u, &lj);
            if (fold(x) != fold(y)) {
                break;
            }
            i += li;
            j += lj;
        }
        if (n[j] == 0u) {
            return true;
        }
    }
    return false;
}

bool i18n_utf8_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0u;
    while (i < len) {
        size_t l = i18n_utf8_seq(p + i, len - i);
        if (l == 0u) {
            return false;
        }
        i += l;
    }
    return true;
}

bool i18n_tag_valid(const char *tag)
{
    if (tag == NULL) {
        return false;
    }
    size_t n = 0u;
    for (; tag[n] != '\0'; n++) {
        char c = tag[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok || n >= 15u) {
            return false;
        }
    }
    return n >= 2u;
}
