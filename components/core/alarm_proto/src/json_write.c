/*
 * json_write.c - JSON writer into a fixed buffer (see proto_json.h).
 */
#include <string.h>

#include "alarm_proto/proto_json.h"

static uint16_t level_bit(uint8_t depth)
{
    return (uint16_t)(1u << depth);
}

static void put(pj_writer_t *w, const char *s, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (n >= w->cap - w->len) { /* keep room for the NUL */
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void put_char(pj_writer_t *w, char c)
{
    put(w, &c, 1);
}

void pj_init(pj_writer_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (buf == NULL || cap == 0u) {
        w->overflow = true;
        w->cap = 0;
    } else {
        buf[0] = '\0';
    }
}

/* Comma before a value or key, unless it follows a key or starts its level. */
static void separate(pj_writer_t *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if ((w->has_items & level_bit(w->depth)) != 0u) {
        if (w->depth == 0u) {
            w->overflow = true; /* a second top-level value */
            return;
        }
        put_char(w, ',');
    }
    w->has_items = (uint16_t)(w->has_items | level_bit(w->depth));
}

static void open_level(pj_writer_t *w, char c)
{
    separate(w);
    put_char(w, c);
    if (w->depth + 1u >= 16u) {
        w->overflow = true;
        return;
    }
    w->depth++;
    w->has_items = (uint16_t)(w->has_items & (uint16_t)~level_bit(w->depth));
}

static void close_level(pj_writer_t *w, char c)
{
    if (w->depth == 0u || w->after_key) {
        w->overflow = true;
        return;
    }
    w->depth--;
    put_char(w, c);
}

void pj_obj(pj_writer_t *w)
{
    open_level(w, '{');
}

void pj_obj_end(pj_writer_t *w)
{
    close_level(w, '}');
}

void pj_arr(pj_writer_t *w)
{
    open_level(w, '[');
}

void pj_arr_end(pj_writer_t *w)
{
    close_level(w, ']');
}

/* Length of the valid UTF-8 sequence at p, 0 when it isn't one (overlong, surrogate, cut). */
static size_t utf8_len(const unsigned char *p)
{
    const unsigned char c = p[0];
    size_t n;
    uint32_t cp;
    uint32_t min;
    if (c >= 0xC2u && c <= 0xDFu) {
        n = 2;
        cp = c & 0x1Fu;
        min = 0x80u;
    } else if (c >= 0xE0u && c <= 0xEFu) {
        n = 3;
        cp = c & 0x0Fu;
        min = 0x800u;
    } else if (c >= 0xF0u && c <= 0xF4u) {
        n = 4;
        cp = c & 0x07u;
        min = 0x10000u;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            return 0; /* also stops at the terminating NUL */
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return 0;
    }
    return n;
}

static void put_string(pj_writer_t *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    put_char(w, '"');
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0u && !w->overflow) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            const char esc[2] = {'\\', (char)c};
            put(w, esc, 2);
            p++;
        } else if (c < 0x20u) {
            switch (c) {
            case '\n': put(w, "\\n", 2); break;
            case '\r': put(w, "\\r", 2); break;
            case '\t': put(w, "\\t", 2); break;
            case '\b': put(w, "\\b", 2); break;
            case '\f': put(w, "\\f", 2); break;
            default: {
                const char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15u]};
                put(w, esc, 6);
                break;
            }
            }
            p++;
        } else if (c < 0x80u) {
            put_char(w, (char)c);
            p++;
        } else {
            const size_t n = utf8_len(p);
            if (n == 0u) {
                put(w, "\xEF\xBF\xBD", 3); /* U+FFFD for a broken byte */
                p++;
            } else {
                put(w, (const char *)p, n);
                p += n;
            }
        }
    }
    put_char(w, '"');
}

void pj_key(pj_writer_t *w, const char *key)
{
    if (w->after_key || w->depth == 0u) {
        w->overflow = true;
        return;
    }
    separate(w);
    put_string(w, key);
    put_char(w, ':');
    w->after_key = true;
}

void pj_str(pj_writer_t *w, const char *s)
{
    if (s == NULL) {
        pj_null(w);
        return;
    }
    separate(w);
    put_string(w, s);
}

static void put_u64(pj_writer_t *w, uint64_t v)
{
    char tmp[20];
    size_t n = 0;
    do {
        tmp[sizeof(tmp) - 1u - n] = (char)('0' + (int)(v % 10u));
        v /= 10u;
        n++;
    } while (v != 0u);
    put(w, tmp + sizeof(tmp) - n, n);
}

void pj_int(pj_writer_t *w, int64_t v)
{
    separate(w);
    if (v < 0) {
        put_char(w, '-');
        put_u64(w, (uint64_t)(-(v + 1)) + 1u);
    } else {
        put_u64(w, (uint64_t)v);
    }
}

void pj_uint(pj_writer_t *w, uint64_t v)
{
    separate(w);
    put_u64(w, v);
}

void pj_bool(pj_writer_t *w, bool v)
{
    separate(w);
    if (v) {
        put(w, "true", 4);
    } else {
        put(w, "false", 5);
    }
}

void pj_null(pj_writer_t *w)
{
    separate(w);
    put(w, "null", 4);
}

void pj_kstr(pj_writer_t *w, const char *key, const char *s)
{
    pj_key(w, key);
    pj_str(w, s);
}

void pj_kint(pj_writer_t *w, const char *key, int64_t v)
{
    pj_key(w, key);
    pj_int(w, v);
}

void pj_kuint(pj_writer_t *w, const char *key, uint64_t v)
{
    pj_key(w, key);
    pj_uint(w, v);
}

void pj_kbool(pj_writer_t *w, const char *key, bool v)
{
    pj_key(w, key);
    pj_bool(w, v);
}

void pj_knull(pj_writer_t *w, const char *key)
{
    pj_key(w, key);
    pj_null(w);
}

void pj_kobj(pj_writer_t *w, const char *key)
{
    pj_key(w, key);
    pj_obj(w);
}

void pj_karr(pj_writer_t *w, const char *key)
{
    pj_key(w, key);
    pj_arr(w);
}

void pj_knum(pj_writer_t *w, const char *key, uint32_t v)
{
    pj_key(w, key);
    if (v == 0u) {
        pj_null(w);
    } else {
        pj_uint(w, v);
    }
}

size_t pj_finish(pj_writer_t *w)
{
    if (w->cap == 0u) {
        return 0;
    }
    w->buf[w->len] = '\0';
    if (w->overflow || w->depth != 0u || w->after_key || w->len == 0u) {
        return 0;
    }
    return w->len;
}
