/*
 * json_read.c - strict, bounded JSON reader (see proto_json.h).
 */
#include <string.h>

#include "alarm_proto/proto_json.h"

static uint16_t level_bit(uint8_t depth)
{
    return (uint16_t)(1u << depth);
}

static void fail(pj_reader_t *r)
{
    r->error = true;
    r->pending = false;
    r->p = r->end;
}

static void skip_ws(pj_reader_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

void pj_read_init(pj_reader_t *r, const char *json, size_t len)
{
    memset(r, 0, sizeof(*r));
    if (json == NULL) {
        r->error = true;
        return;
    }
    r->p = json;
    r->end = json + len;
}

pj_type_t pj_peek(pj_reader_t *r)
{
    if (r->error) {
        return PJ_END;
    }
    skip_ws(r);
    if (r->p >= r->end) {
        return PJ_END;
    }
    switch (*r->p) {
    case '{': return PJ_OBJECT;
    case '[': return PJ_ARRAY;
    case '"': return PJ_STRING;
    case 't':
    case 'f': return PJ_BOOL;
    case 'n': return PJ_NULL;
    default: return (*r->p == '-' || is_digit(*r->p)) ? PJ_NUMBER : PJ_END;
    }
}

/* ---- scalars ----------------------------------------------------------------------------- */

static bool hex4(pj_reader_t *r, uint32_t *out)
{
    if (r->end - r->p < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = r->p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A') + 10u;
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    r->p += 4;
    *out = v;
    return true;
}

typedef struct {
    char *out;
    size_t cap;
    size_t n;
    bool fits;
} sink_t;

static void emit(sink_t *s, unsigned char c)
{
    if (s->out != NULL) {
        if (s->n + 1u < s->cap) {
            s->out[s->n] = (char)c;
        } else {
            s->fits = false;
        }
    }
    s->n++;
}

static void emit_utf8(sink_t *s, uint32_t cp)
{
    if (cp < 0x80u) {
        emit(s, (unsigned char)cp);
    } else if (cp < 0x800u) {
        emit(s, (unsigned char)(0xC0u | (cp >> 6)));
        emit(s, (unsigned char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        emit(s, (unsigned char)(0xE0u | (cp >> 12)));
        emit(s, (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)));
        emit(s, (unsigned char)(0x80u | (cp & 0x3Fu)));
    } else {
        emit(s, (unsigned char)(0xF0u | (cp >> 18)));
        emit(s, (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu)));
        emit(s, (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)));
        emit(s, (unsigned char)(0x80u | (cp & 0x3Fu)));
    }
}

/* The string at r->p, decoded into out (may be NULL). Returns false on a syntax error (which
 * sets r->error); *fits is false when it didn't fit or held \u0000 (out is then ""). */
static bool scan_string(pj_reader_t *r, char *out, size_t cap, bool *fits)
{
    sink_t s = {out, cap, 0, true};
    if (r->p >= r->end || *r->p != '"') {
        fail(r);
        return false;
    }
    r->p++;
    for (;;) {
        if (r->p >= r->end) {
            fail(r);
            return false;
        }
        unsigned char c = (unsigned char)*r->p++;
        if (c == '"') {
            break;
        }
        if (c < 0x20u) {
            fail(r); /* raw control characters must be escaped */
            return false;
        }
        if (c != '\\') {
            emit(&s, c);
            continue;
        }
        if (r->p >= r->end) {
            fail(r);
            return false;
        }
        c = (unsigned char)*r->p++;
        uint32_t cp;
        switch (c) {
        case '"':
        case '\\':
        case '/': emit(&s, c); break;
        case 'b': emit(&s, '\b'); break;
        case 'f': emit(&s, '\f'); break;
        case 'n': emit(&s, '\n'); break;
        case 'r': emit(&s, '\r'); break;
        case 't': emit(&s, '\t'); break;
        case 'u':
            if (!hex4(r, &cp)) {
                fail(r);
                return false;
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                uint32_t lo;
                if (r->end - r->p < 2 || r->p[0] != '\\' || r->p[1] != 'u') {
                    fail(r);
                    return false;
                }
                r->p += 2;
                if (!hex4(r, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                    fail(r);
                    return false;
                }
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                fail(r); /* a lone low surrogate */
                return false;
            }
            if (cp == 0u) {
                s.fits = false; /* would cut the C string short */
            }
            emit_utf8(&s, cp);
            break;
        default:
            fail(r);
            return false;
        }
    }
    if (out != NULL && cap > 0u) {
        out[s.fits ? s.n : 0u] = '\0';
    }
    *fits = s.fits;
    return true;
}

/* The number at r->p (JSON grammar). *value is set and *ok true for an integer in int64. */
static bool scan_number(pj_reader_t *r, int64_t *value, bool *ok)
{
    const char *p = r->p;
    const char *end = r->end;
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        p++;
    }
    if (p >= end || !is_digit(*p)) {
        fail(r);
        return false;
    }
    uint64_t mag = 0;
    bool in_range = true;
    if (*p == '0') {
        p++;
        if (p < end && is_digit(*p)) {
            fail(r); /* no leading zeros */
            return false;
        }
    } else {
        while (p < end && is_digit(*p)) {
            const uint64_t d = (uint64_t)(*p - '0');
            if (mag > (UINT64_MAX - d) / 10u) {
                in_range = false;
            } else {
                mag = mag * 10u + d;
            }
            p++;
        }
    }
    bool integral = true;
    if (p < end && *p == '.') {
        integral = false;
        p++;
        if (p >= end || !is_digit(*p)) {
            fail(r);
            return false;
        }
        while (p < end && is_digit(*p)) {
            p++;
        }
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        integral = false;
        p++;
        if (p < end && (*p == '+' || *p == '-')) {
            p++;
        }
        if (p >= end || !is_digit(*p)) {
            fail(r);
            return false;
        }
        while (p < end && is_digit(*p)) {
            p++;
        }
    }
    r->p = p;
    *ok = false;
    if (integral && in_range) {
        if (neg && mag <= (uint64_t)INT64_MAX + 1u) {
            *value = (mag == (uint64_t)INT64_MAX + 1u) ? INT64_MIN : -(int64_t)mag;
            *ok = true;
        } else if (!neg && mag <= (uint64_t)INT64_MAX) {
            *value = (int64_t)mag;
            *ok = true;
        }
    }
    return true;
}

static bool scan_literal(pj_reader_t *r, const char *lit)
{
    const size_t n = strlen(lit);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, lit, n) != 0) {
        fail(r);
        return false;
    }
    r->p += n;
    return true;
}

/* ---- structure --------------------------------------------------------------------------- */

static void skip_value(pj_reader_t *r, unsigned depth);

static void skip_container(pj_reader_t *r, unsigned depth, bool object)
{
    if (depth >= PJ_MAX_DEPTH) {
        fail(r);
        return;
    }
    const char close = object ? '}' : ']';
    r->p++;
    skip_ws(r);
    if (r->p < r->end && *r->p == close) {
        r->p++;
        return;
    }
    for (;;) {
        if (object) {
            bool fits;
            skip_ws(r);
            if (!scan_string(r, NULL, 0, &fits)) {
                return;
            }
            skip_ws(r);
            if (r->p >= r->end || *r->p != ':') {
                fail(r);
                return;
            }
            r->p++;
        }
        skip_value(r, depth + 1u);
        if (r->error) {
            return;
        }
        skip_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        if (r->p < r->end && *r->p == close) {
            r->p++;
            return;
        }
        fail(r);
        return;
    }
}

static void skip_value(pj_reader_t *r, unsigned depth)
{
    bool ok;
    bool fits;
    int64_t v;
    switch (pj_peek(r)) {
    case PJ_OBJECT: skip_container(r, depth, true); break;
    case PJ_ARRAY: skip_container(r, depth, false); break;
    case PJ_STRING: scan_string(r, NULL, 0, &fits); break;
    case PJ_NUMBER: scan_number(r, &v, &ok); break;
    case PJ_BOOL: scan_literal(r, *r->p == 't' ? "true" : "false"); break;
    case PJ_NULL: scan_literal(r, "null"); break;
    default: fail(r); break;
    }
}

void pj_skip(pj_reader_t *r)
{
    r->pending = false;
    skip_value(r, r->depth);
}

static bool enter(pj_reader_t *r, pj_type_t type)
{
    if (pj_peek(r) != type || r->depth >= PJ_MAX_DEPTH) {
        fail(r);
        return false;
    }
    r->p++;
    r->pending = false;
    r->depth++;
    r->started = (uint16_t)(r->started & (uint16_t)~level_bit(r->depth));
    if (type == PJ_ARRAY) {
        r->is_array = (uint16_t)(r->is_array | level_bit(r->depth));
    } else {
        r->is_array = (uint16_t)(r->is_array & (uint16_t)~level_bit(r->depth));
    }
    return true;
}

bool pj_enter_obj(pj_reader_t *r)
{
    return enter(r, PJ_OBJECT);
}

bool pj_enter_arr(pj_reader_t *r)
{
    return enter(r, PJ_ARRAY);
}

/* Moves to the next member / element of the current level: false at its end (level left). */
static bool next_item(pj_reader_t *r, bool array)
{
    if (r->error || r->depth == 0u || (((r->is_array & level_bit(r->depth)) != 0u) != array)) {
        fail(r);
        return false;
    }
    if (r->pending) {
        pj_skip(r);
        if (r->error) {
            return false;
        }
    }
    const char close = array ? ']' : '}';
    skip_ws(r);
    if (r->p >= r->end) {
        fail(r);
        return false;
    }
    if (*r->p == close) {
        r->p++;
        r->depth--;
        return false;
    }
    if ((r->started & level_bit(r->depth)) != 0u) {
        if (*r->p != ',') {
            fail(r);
            return false;
        }
        r->p++;
        skip_ws(r);
    }
    r->started = (uint16_t)(r->started | level_bit(r->depth));
    return true;
}

bool pj_next_member(pj_reader_t *r, char *key, size_t key_cap)
{
    if (key != NULL && key_cap > 0u) {
        key[0] = '\0';
    }
    if (!next_item(r, false)) {
        return false;
    }
    bool fits;
    if (!scan_string(r, key, key_cap, &fits)) {
        return false;
    }
    skip_ws(r);
    if (r->p >= r->end || *r->p != ':') {
        fail(r);
        return false;
    }
    r->p++;
    r->pending = true;
    return true;
}

bool pj_next_element(pj_reader_t *r)
{
    if (!next_item(r, true)) {
        return false;
    }
    r->pending = true;
    return true;
}

bool pj_get_str(pj_reader_t *r, char *out, size_t cap)
{
    if (out != NULL && cap > 0u) {
        out[0] = '\0';
    }
    if (pj_peek(r) != PJ_STRING) {
        pj_skip(r);
        return false;
    }
    r->pending = false;
    bool fits;
    return scan_string(r, out, cap, &fits) && fits;
}

bool pj_get_int(pj_reader_t *r, int64_t *out)
{
    if (pj_peek(r) != PJ_NUMBER) {
        pj_skip(r);
        return false;
    }
    r->pending = false;
    bool ok = false;
    int64_t v = 0;
    if (!scan_number(r, &v, &ok) || !ok) {
        return false;
    }
    *out = v;
    return true;
}

bool pj_get_bool(pj_reader_t *r, bool *out)
{
    if (pj_peek(r) != PJ_BOOL) {
        pj_skip(r);
        return false;
    }
    r->pending = false;
    const bool v = *r->p == 't';
    if (!scan_literal(r, v ? "true" : "false")) {
        return false;
    }
    *out = v;
    return true;
}

bool pj_get_null(pj_reader_t *r)
{
    if (pj_peek(r) != PJ_NULL) {
        pj_skip(r);
        return false;
    }
    r->pending = false;
    return scan_literal(r, "null");
}

bool pj_finished(pj_reader_t *r)
{
    if (r->error || r->depth != 0u || r->pending) {
        return false;
    }
    skip_ws(r);
    return r->p == r->end;
}
