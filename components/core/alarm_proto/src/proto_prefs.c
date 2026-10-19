/*
 * proto_prefs.c - a panel's Screen and sound settings as fields, on the panel link and in the
 * Home Assistant link's JSON (proto_prefs.h).
 */
#include "alarm_proto/proto_prefs.h"

#include <string.h>

#include "alarm/alarm_codec.h"

#define LOOK   PROTO_PREFS_FL_LOOK
#define SWITCH PROTO_PREFS_FL_SWITCH
#define DELAY  PROTO_PREFS_FL_DELAY

#define DELAY_MIN_MS 10000u    /* 10 s */
#define DELAY_MAX_MS 86400000u /* 24 h */

typedef struct {
    proto_prefs_field_t f;
    uint16_t at; /* its member's offset in proto_prefs_values_t */
} field_t;

/* 0, or a compile error when a member's width isn't its field's wire size. */
#define WIDTH_IS(member, size)                                                                    \
    (0u * sizeof(char[sizeof(((proto_prefs_values_t *)0)->member) == (size) ? 1 : -1]))
#define FIELD(id, member, size, lo, hi, flags, key)                                               \
    {{(id), (size), (flags), (key), (lo), (hi)},                                                  \
     (uint16_t)(offsetof(proto_prefs_values_t, member) + WIDTH_IS(member, size))}

/* The contract (docs/features/11 "Settings fields"): index id - 1. Ranges as panel_prefs.c and
 * the Panel Manager's (web_config.c) take them. */
static const field_t FIELDS[PROTO_PREFS_F_LAST] = {
    FIELD(PROTO_PREFS_F_BRIGHTNESS, brightness, 1u, 10u, 100u, LOOK, "brightness"),
    FIELD(PROTO_PREFS_F_SAVER_BRIGHTNESS, saver_brightness, 1u, 0u, 50u, LOOK, "saver_level"),
    FIELD(PROTO_PREFS_F_SAVER_AFTER, saver_after_ms, 4u, DELAY_MIN_MS, DELAY_MAX_MS, DELAY,
          "saver_after"),
    FIELD(PROTO_PREFS_F_OFF_AFTER, off_after_ms, 4u, DELAY_MIN_MS, DELAY_MAX_MS, DELAY,
          "off_after"),
    FIELD(PROTO_PREFS_F_NIGHT_ON, night_on, 1u, 0u, 1u, SWITCH | LOOK, "night"),
    FIELD(PROTO_PREFS_F_NIGHT_START, night_start, 2u, 0u, 1439u, LOOK, "night_start"),
    FIELD(PROTO_PREFS_F_NIGHT_END, night_end, 2u, 0u, 1439u, LOOK, "night_end"),
    FIELD(PROTO_PREFS_F_NIGHT_BRIGHTNESS, night_brightness, 1u, 10u, 100u, LOOK, "night_level"),
    FIELD(PROTO_PREFS_F_NIGHT_OFF_AFTER, night_off_after_ms, 4u, DELAY_MIN_MS, DELAY_MAX_MS,
          DELAY, "night_off"),
    FIELD(PROTO_PREFS_F_CLOCK_MODE, clock_mode, 1u, 0u, 2u, 0u, "clock"),
    FIELD(PROTO_PREFS_F_APPEARANCE, appearance, 1u, 0u, 2u, LOOK, "appearance"),
    FIELD(PROTO_PREFS_F_HIGH_CONTRAST, high_contrast, 1u, 0u, 1u, SWITCH | LOOK, "contrast"),
    FIELD(PROTO_PREFS_F_VOLUME, volume, 1u, 0u, 100u, 0u, "beeps"),
    FIELD(PROTO_PREFS_F_CHIME_VOLUME, chime_volume, 1u, 0u, 100u, 0u, "chime_vol"),
    FIELD(PROTO_PREFS_F_VOICE_VOLUME, voice_volume, 1u, 0u, 100u, 0u, "voice_vol"),
    FIELD(PROTO_PREFS_F_KEY_CLICK, key_click, 1u, 0u, 1u, SWITCH, "key_click"),
    FIELD(PROTO_PREFS_F_VOICE_ENABLED, voice_enabled, 1u, 0u, 1u, SWITCH, "voice"),
    FIELD(PROTO_PREFS_F_VOICE_STATUS, voice_status, 1u, 0u, 1u, SWITCH, "voice_stat"),
    FIELD(PROTO_PREFS_F_VOICE_CHIME, voice_chime, 1u, 0u, 1u, SWITCH, "voice_chime"),
};

static const field_t *find(unsigned id)
{
    return id >= 1u && id <= PROTO_PREFS_F_LAST ? &FIELDS[id - 1u] : NULL;
}

static bool in_range(const proto_prefs_field_t *f, uint32_t x)
{
    return ((f->flags & DELAY) != 0u && x == 0u) || (x >= f->min && x <= f->max);
}

static uint32_t load(const proto_prefs_values_t *v, const field_t *t)
{
    const unsigned char *p = (const unsigned char *)v + t->at;
    if ((t->f.flags & SWITCH) != 0u) {
        bool b;
        memcpy(&b, p, sizeof b);
        return b ? 1u : 0u;
    }
    switch (t->f.size) {
    case 2u: {
        uint16_t x;
        memcpy(&x, p, sizeof x);
        return x;
    }
    case 4u: {
        uint32_t x;
        memcpy(&x, p, sizeof x);
        return x;
    }
    default:
        return *p;
    }
}

static void store(proto_prefs_values_t *v, const field_t *t, uint32_t x)
{
    unsigned char *p = (unsigned char *)v + t->at;
    if ((t->f.flags & SWITCH) != 0u) {
        const bool b = x != 0u;
        memcpy(p, &b, sizeof b);
        return;
    }
    switch (t->f.size) {
    case 2u: {
        const uint16_t y = (uint16_t)x;
        memcpy(p, &y, sizeof y);
        break;
    }
    case 4u:
        memcpy(p, &x, sizeof x);
        break;
    default:
        *p = (unsigned char)x;
        break;
    }
}

/* ---- the table ---------------------------------------------------------------------------- */

const proto_prefs_field_t *proto_prefs_field(unsigned id)
{
    const field_t *t = find(id);
    return t != NULL ? &t->f : NULL;
}

uint8_t proto_prefs_id(const char *key)
{
    for (unsigned i = 0; key != NULL && i < PROTO_PREFS_F_LAST; i++) {
        if (strcmp(FIELDS[i].f.key, key) == 0) {
            return FIELDS[i].f.id;
        }
    }
    return 0u;
}

proto_prefs_why_t proto_prefs_check(unsigned id, uint32_t value)
{
    const field_t *t = find(id);
    if (t == NULL) {
        return PROTO_PREFS_WHY_NOT_ALLOWED;
    }
    return in_range(&t->f, value) ? PROTO_PREFS_WHY_NONE : PROTO_PREFS_WHY_RANGE;
}

proto_prefs_why_t proto_prefs_put(proto_prefs_values_t *v, unsigned id, uint32_t value)
{
    const proto_prefs_why_t why = proto_prefs_check(id, value);
    if (why == PROTO_PREFS_WHY_NONE) {
        store(v, find(id), value);
        v->have |= PROTO_PREFS_BIT(id);
    }
    return why;
}

bool proto_prefs_get(const proto_prefs_values_t *v, unsigned id, uint32_t *value)
{
    const field_t *t = find(id);
    if (t == NULL || (v->have & PROTO_PREFS_BIT(id)) == 0u) {
        return false;
    }
    *value = load(v, t);
    return true;
}

proto_prefs_why_t proto_prefs_why_read(uint8_t wire)
{
    return wire >= 1u && wire <= PROTO_PREFS_WHY_LAST ? (proto_prefs_why_t)wire
                                                       : PROTO_PREFS_WHY_NOT_ALLOWED;
}

const char *proto_prefs_why_word(uint8_t why)
{
    /* BAD has no word in "refused" (docs/features/23 protocol.md): settings_set's parser makes
     * the whole request bad instead, and a keypad's BAD (a TLV the master sent malformed, which
     * proto_prefs_encode never writes) reads as not allowed. */
    static const char *const WORD[PROTO_PREFS_WHY_LAST + 1u] = {
        "not_allowed", "not_allowed",     "out_of_range", "in_use",
        "held",        "voice_not_ready", "not_allowed",  "busy",
    };
    return WORD[proto_prefs_why_read(why)];
}

/* ---- taking fields, for both decoders ----------------------------------------------------- */

typedef struct {
    proto_prefs_values_t *values;
    proto_prefs_mask_t *refused;
    uint8_t *why;
    proto_prefs_mask_t accept;
    proto_prefs_mask_t seen;
} taker_t;

/* A known field with its value x, or refused with why (BAD from the reader). Each field is
 * refused alone; one named twice is refused BAD whatever its copies said. */
static void take(taker_t *k, const field_t *t, proto_prefs_why_t why, uint32_t x)
{
    const proto_prefs_mask_t bit = PROTO_PREFS_BIT(t->f.id);
    if ((k->seen & bit) != 0u) {
        why = PROTO_PREFS_WHY_BAD;
    } else if (why == PROTO_PREFS_WHY_NONE && (k->accept & bit) == 0u) {
        why = PROTO_PREFS_WHY_NOT_ALLOWED;
    } else if (why == PROTO_PREFS_WHY_NONE && !in_range(&t->f, x)) {
        why = PROTO_PREFS_WHY_RANGE;
    }
    k->seen |= bit;
    if (why != PROTO_PREFS_WHY_NONE) {
        store(k->values, t, 0u);
        k->values->have &= ~bit;
        *k->refused |= bit;
        k->why[t->f.id - 1u] = (uint8_t)why;
        return;
    }
    store(k->values, t, x);
    k->values->have |= bit;
}

/* ---- the panel link (TLV) ----------------------------------------------------------------- */

size_t proto_prefs_encode(const proto_prefs_values_t *v, proto_prefs_mask_t mask, uint8_t *out,
                          size_t cap)
{
    const proto_prefs_mask_t m = v->have & mask & PROTO_PREFS_KNOWN;
    if (cap < 1u) {
        return 0u;
    }
    size_t o = 1u;
    uint8_t n = 0u;
    for (unsigned i = 0; i < PROTO_PREFS_F_LAST; i++) {
        const field_t *t = &FIELDS[i];
        if ((m & PROTO_PREFS_BIT(t->f.id)) == 0u) {
            continue;
        }
        if (cap - o < 2u + (size_t)t->f.size) {
            return 0u;
        }
        const uint32_t x = load(v, t);
        out[o++] = t->f.id;
        out[o++] = t->f.size;
        for (unsigned b = 0; b < t->f.size; b++) {
            out[o++] = (uint8_t)((x >> (8u * b)) & 0xFFu);
        }
        n++;
    }
    out[0] = n;
    return o;
}

bool proto_prefs_decode(const uint8_t *in, size_t len, proto_prefs_mask_t accept,
                        proto_prefs_decoded_t *out)
{
    memset(out, 0, sizeof *out);
    taker_t k = {&out->values, &out->refused, out->why, accept, 0u};
    const unsigned count = len > 0u ? in[0] : 0u;
    size_t o = 1u;
    for (unsigned i = 0; i < count; i++) {
        if (len - o < 2u || len - o - 2u < (size_t)in[o + 1u]) {
            memset(out, 0, sizeof *out);
            return false; /* a TLV runs past the section */
        }
        const unsigned id = in[o];
        const unsigned vlen = in[o + 1u];
        const uint8_t *val = in + o + 2u;
        o += 2u + vlen;
        const field_t *t = find(id);
        if (t == NULL) {
            bool listed = false;
            for (unsigned u = 0; u < out->unknown_n && u < PROTO_PREFS_UNKNOWN_MAX; u++) {
                listed = listed || out->unknown[u] == id;
            }
            if (!listed && out->unknown_n < UINT8_MAX) {
                if (out->unknown_n < PROTO_PREFS_UNKNOWN_MAX) {
                    out->unknown[out->unknown_n] = (uint8_t)id;
                }
                out->unknown_n++;
            }
            continue;
        }
        if (vlen != t->f.size) {
            take(&k, t, PROTO_PREFS_WHY_BAD, 0u);
            continue;
        }
        uint32_t x = 0u;
        for (unsigned b = 0; b < vlen; b++) {
            x |= (uint32_t)(unsigned)val[b] << (8u * b);
        }
        take(&k, t, PROTO_PREFS_WHY_NONE, x);
    }
    return true;
}

uint32_t proto_prefs_crc(const uint8_t *fields, size_t len)
{
    return alarm_crc32(0u, fields, len);
}

uint32_t proto_prefs_values_crc(const proto_prefs_values_t *v)
{
    uint8_t buf[PROTO_PREFS_ENC_MAX];
    const size_t n = proto_prefs_encode(v, PROTO_PREFS_KNOWN, buf, sizeof buf);
    return proto_prefs_crc(buf, n);
}

/* ---- the Home Assistant link (JSON) ------------------------------------------------------- */

void proto_prefs_json_values(pj_writer_t *w, const proto_prefs_values_t *v)
{
    pj_obj(w);
    for (unsigned i = 0; i < PROTO_PREFS_F_LAST; i++) {
        const field_t *t = &FIELDS[i];
        if ((v->have & PROTO_PREFS_BIT(t->f.id)) == 0u) {
            continue;
        }
        const uint32_t x = load(v, t);
        if ((t->f.flags & SWITCH) != 0u) {
            pj_kbool(w, t->f.key, x != 0u);
        } else if ((t->f.flags & DELAY) != 0u) {
            pj_kuint(w, t->f.key, x / 1000u);
        } else {
            pj_kuint(w, t->f.key, x);
        }
    }
    pj_obj_end(w);
}

void proto_prefs_json_keys(pj_writer_t *w, proto_prefs_mask_t mask)
{
    pj_arr(w);
    for (unsigned i = 0; i < PROTO_PREFS_F_LAST; i++) {
        if ((mask & PROTO_PREFS_BIT(FIELDS[i].f.id)) != 0u) {
            pj_str(w, FIELDS[i].f.key);
        }
    }
    pj_arr_end(w);
}

/* A JSON value for field t: its wire value, or why it can't be one. */
static proto_prefs_why_t json_value(pj_reader_t *r, const field_t *t, uint32_t *x)
{
    if ((t->f.flags & SWITCH) != 0u) {
        bool b;
        if (!pj_get_bool(r, &b)) {
            return PROTO_PREFS_WHY_BAD;
        }
        *x = b ? 1u : 0u;
        return PROTO_PREFS_WHY_NONE;
    }
    int64_t i;
    if (!pj_get_int(r, &i)) {
        return PROTO_PREFS_WHY_BAD; /* another type, a fraction, beyond int64 */
    }
    const int64_t top = (t->f.flags & DELAY) != 0u ? (int64_t)(DELAY_MAX_MS / 1000u)
                                                    : (int64_t)UINT32_MAX;
    if (i < 0 || i > top) {
        return PROTO_PREFS_WHY_RANGE;
    }
    *x = (t->f.flags & DELAY) != 0u ? (uint32_t)i * 1000u : (uint32_t)i;
    return PROTO_PREFS_WHY_NONE;
}

bool proto_prefs_json_parse(pj_reader_t *r, proto_prefs_mask_t accept, proto_prefs_parsed_t *out)
{
    memset(out, 0, sizeof *out);
    if (pj_peek(r) != PJ_OBJECT) {
        pj_skip(r); /* another type is no syntax error: the caller reads on */
        return false;
    }
    if (!pj_enter_obj(r)) {
        return false;
    }
    taker_t k = {&out->values, &out->refused, out->why, accept, 0u};
    char key[PROTO_PREFS_KEY_MAX + 1u];
    bool bad = false; /* the whole request: read on, then refuse it all */
    while (pj_next_member(r, key, sizeof key)) {
        if (key[0] == '\0') {
            bad = true; /* "" or longer than any settings key */
            continue;
        }
        const field_t *t = find(proto_prefs_id(key));
        if (t == NULL) {
            bool listed = false;
            for (unsigned u = 0; u < out->unknown_n && u < PROTO_PREFS_UNKNOWN_MAX; u++) {
                listed = listed || strcmp(out->unknown[u], key) == 0;
            }
            if (!listed && out->unknown_n < UINT8_MAX) {
                if (out->unknown_n < PROTO_PREFS_UNKNOWN_MAX) {
                    memcpy(out->unknown[out->unknown_n], key, strlen(key) + 1u);
                }
                out->unknown_n++;
            }
            continue;
        }
        uint32_t x = 0u;
        const proto_prefs_why_t why = json_value(r, t, &x);
        if (why == PROTO_PREFS_WHY_BAD || (k.seen & PROTO_PREFS_BIT(t->f.id)) != 0u) {
            bad = true; /* another JSON type, or named twice */
            continue;
        }
        take(&k, t, why, x);
    }
    if (r->error || bad) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}
