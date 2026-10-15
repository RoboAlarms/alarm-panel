/* i18n_format.c - the {n} formatter (I18N-003, I18N-008, I18N-010). */
#include <string.h>

#include "i18n/i18n.h"
#include "i18n_internal.h"

i18n_arg_t i18n_arg_str(const char *s)
{
    i18n_arg_t a;
    memset(&a, 0, sizeof(a));
    a.kind = I18N_ARG_STR;
    a.v.s = s;
    return a;
}

i18n_arg_t i18n_arg_int(int64_t i)
{
    i18n_arg_t a;
    memset(&a, 0, sizeof(a));
    a.kind = I18N_ARG_INT;
    a.v.i = i;
    return a;
}

i18n_arg_t i18n_arg_uint(uint64_t u)
{
    i18n_arg_t a;
    memset(&a, 0, sizeof(a));
    a.kind = I18N_ARG_UINT;
    a.v.u = u;
    return a;
}

typedef struct {
    char *out;
    size_t cap; /* bytes available for text (len - 1) */
    size_t pos;
    bool full;
} sink_t;

/* Append a whole-character prefix of s[0..n) (s NUL-terminated at n or beyond); the first
 * chunk that does not fit ends the output. */
static void put(sink_t *k, const char *s, size_t n)
{
    if (k->full) {
        return;
    }
    size_t room = k->cap - k->pos;
    if (n > room) {
        /* Cut on a character: only whole characters of the chunk are copied. */
        const unsigned char *p = (const unsigned char *)s;
        size_t i = 0u;
        while (i < n && p[i] != 0u) {
            size_t l = i18n_utf8_seq(p + i, 4u);
            if (l == 0u) {
                l = 1u;
            }
            if (i + l > room) {
                break;
            }
            i += l;
        }
        memcpy(k->out + k->pos, s, i);
        k->pos += i;
        k->full = true;
        return;
    }
    memcpy(k->out + k->pos, s, n);
    k->pos += n;
}

static void put_number(sink_t *k, bool neg, uint64_t mag)
{
    char digits[24];
    size_t n = 0u;
    do {
        digits[n++] = (char)('0' + (unsigned)(mag % 10u));
        mag /= 10u;
    } while (mag != 0u);
    char text[26];
    size_t t = 0u;
    if (neg) {
        text[t++] = '-';
    }
    while (n > 0u) {
        text[t++] = digits[--n];
    }
    put(k, text, t);
}

size_t i18n_format(char *out, size_t len, const char *pattern, const i18n_arg_t *args, size_t nargs)
{
    if (out == NULL || len == 0u) {
        return 0u;
    }
    sink_t k = {out, len - 1u, 0u, false};
    const unsigned char *p = (const unsigned char *)(pattern ? pattern : "");
    while (*p != 0u && !k.full) {
        if (p[0] == '{' && p[1] == '{') {
            put(&k, "{", 1u);
            p += 2;
        } else if (p[0] == '}' && p[1] == '}') {
            put(&k, "}", 1u);
            p += 2;
        } else if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') {
            size_t idx = (size_t)(p[1] - (unsigned char)'0');
            if (args != NULL && idx < nargs) {
                const i18n_arg_t *a = &args[idx];
                if (a->kind == I18N_ARG_STR) {
                    if (a->v.s != NULL) {
                        put(&k, a->v.s, strlen(a->v.s));
                    }
                } else if (a->kind == I18N_ARG_INT) {
                    int64_t v = a->v.i;
                    put_number(&k, v < 0, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v);
                } else {
                    put_number(&k, false, a->v.u);
                }
            } else {
                put(&k, (const char *)p, 3u);
            }
            p += 3;
        } else {
            size_t l = i18n_utf8_seq(p, 4u);
            if (l == 0u) {
                l = 1u;
            }
            put(&k, (const char *)p, l);
            p += l;
        }
    }
    out[k.pos] = '\0';
    return k.pos;
}

/* How many times the formatter would put argument 0 into pattern: the same reading as i18n_format
 * ("{{" and "}}" are literal braces, "{0}" a placeholder). */
static size_t count_arg0(const char *pattern)
{
    size_t n = 0u;
    const unsigned char *p = (const unsigned char *)(pattern ? pattern : "");
    while (*p != 0u) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            p += 2;
        } else if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') {
            n += p[1] == '0' ? 1u : 0u;
            p += 3;
        } else {
            size_t l = i18n_utf8_seq(p, 4u);
            p += l == 0u ? 1u : l;
        }
    }
    return n;
}

size_t i18n_format_fit(char *out, size_t len, const char *pattern, const char *name)
{
    if (out == NULL || len == 0u) {
        return 0u;
    }
    if (name == NULL) {
        name = "";
    }
    const size_t uses = count_arg0(pattern);
    const i18n_arg_t empty = i18n_arg_str("");
    /* the pattern's own words: the sentence with an empty name */
    const size_t words = i18n_format(out, len, pattern, &empty, 1u);
    const size_t room = uses == 0u || words >= len - 1u ? 0u : (len - 1u - words) / uses;
    const size_t whole = strlen(name);
    char cut[256];
    const char *fitted = name;
    if (uses > 0u && (whole > room || whole >= sizeof(cut))) {
        const size_t limit = room < sizeof(cut) - 1u ? room : sizeof(cut) - 1u;
        size_t n = 0u;
        if (limit > 3u) {
            n = i18n_utf8_fit(name, limit - 3u);
            while (n > 0u && name[n - 1u] == ' ') {
                n--;
            }
        }
        memcpy(cut, name, n);
        if (limit >= 3u) {
            memcpy(cut + n, "\xE2\x80\xA6", 3u); /* "…" */
            n += 3u;
        }
        cut[n] = '\0';
        fitted = cut;
    }
    const i18n_arg_t arg = i18n_arg_str(fitted);
    return i18n_format(out, len, pattern, &arg, 1u);
}
