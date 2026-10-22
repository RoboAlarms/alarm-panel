/*
 * ur_codec.c - ui_remote's records (ur_internal.h) and the tables of the structs the calls carry.
 */
#include <string.h>

#include "ur_internal.h"

/* ---- strings ---- */

void ur_put_str(plink_w_t *w, const char *s)
{
    const size_t n = s != NULL ? strlen(s) : 0u;
    plink_w_u16(w, (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n));
    plink_w_bytes(w, s, n > 0xFFFFu ? 0xFFFFu : n);
}

void ur_get_str(plink_r_t *r, char *out, size_t cap, bool clean)
{
    if (cap == 0u) {
        return;
    }
    memset(out, 0, cap);
    size_t n = plink_r_u16(r);
    const size_t left = plink_r_left(r);
    if (n > left) {
        if (!r->pad) {
            r->ok = false;
            return;
        }
        n = left;
    }
    const uint8_t *s = plink_r_take(r, n);
    if (s == NULL) {
        return;
    }
    if (clean) {
        plink_clean_text((const char *)s, n, out, cap);
        return;
    }
    /* a code or a password: as typed, cut to the field (never on a NUL inside it) */
    const size_t k = n < cap - 1u ? n : cap - 1u;
    memcpy(out, s, k);
    out[strnlen(out, k)] = '\0';
}

/* ---- records ---- */

static void put_int(plink_w_t *w, const uint8_t *p, size_t size)
{
    uint64_t v = 0;
    switch (size) {
    case 1: v = *p; break;
    case 2: { uint16_t x; memcpy(&x, p, 2); v = x; break; }
    case 4: { uint32_t x; memcpy(&x, p, 4); v = x; break; }
    case 8: memcpy(&v, p, 8); break;
    default: w->ok = false; return;
    }
    for (size_t i = 0; i < size; i++) {
        plink_w_u8(w, (uint8_t)(v >> (8u * i)));
    }
}

static void get_int(plink_r_t *r, uint8_t *p, size_t size)
{
    uint64_t v = 0;
    for (size_t i = 0; i < size; i++) {
        v |= (uint64_t)plink_r_u8(r) << (8u * i);
    }
    switch (size) {
    case 1: *p = (uint8_t)v; break;
    case 2: { const uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { const uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    case 8: memcpy(p, &v, 8); break;
    default: r->ok = false; break;
    }
}

void ur_put(plink_w_t *w, const ur_table_t *t, const void *obj)
{
    const size_t at = w->len;
    plink_w_u16(w, 0); /* the record's length, set below */
    const uint8_t *base = obj;
    for (size_t i = 0; i < t->n; i++) {
        const ur_field_t *f = &t->f[i];
        for (size_t e = 0; e < f->count; e++) {
            const uint8_t *p = base + f->off + e * f->size;
            switch (f->kind) {
            case UR_F_INT: put_int(w, p, f->size); break;
            case UR_F_BOOL: plink_w_u8(w, *(const bool *)(const void *)p ? 1u : 0u); break;
            case UR_F_STR:
            case UR_F_SECRET: {
                const size_t n = strnlen((const char *)p, f->size);
                plink_w_u16(w, (uint16_t)n);
                plink_w_bytes(w, p, n);
                break;
            }
            case UR_F_REC: ur_put(w, f->sub, p); break;
            default: w->ok = false; break;
            }
        }
    }
    const size_t n = w->len - at - 2u;
    if (!w->ok || n > 0xFFFFu) {
        w->ok = false;
        return;
    }
    w->p[at] = (uint8_t)n;
    w->p[at + 1u] = (uint8_t)(n >> 8);
}

void ur_get(plink_r_t *r, const ur_table_t *t, void *obj)
{
    memset(obj, 0, t->size);
    const size_t n = plink_r_u16(r);
    const uint8_t *at = plink_r_take(r, n);
    if (at == NULL) {
        r->ok = false;
        return;
    }
    /* the record's own reader: a field it lacks (a sender older than this table) reads zero, and
     * fields it has past this table (a newer sender) are skipped with the rest of the record */
    plink_r_t rr;
    plink_r_init(&rr, at, n);
    rr.pad = true;
    uint8_t *base = obj;
    for (size_t i = 0; i < t->n && rr.ok; i++) {
        const ur_field_t *f = &t->f[i];
        for (size_t e = 0; e < f->count; e++) {
            uint8_t *p = base + f->off + e * f->size;
            switch (f->kind) {
            case UR_F_INT: get_int(&rr, p, f->size); break;
            case UR_F_BOOL: *(bool *)(void *)p = plink_r_u8(&rr) != 0u; break;
            case UR_F_STR: ur_get_str(&rr, (char *)p, f->size, true); break;
            case UR_F_SECRET: ur_get_str(&rr, (char *)p, f->size, false); break;
            case UR_F_REC:
                if (plink_r_left(&rr) > 0u) {
                    ur_get(&rr, f->sub, p);
                }
                break;
            default: rr.ok = false; break;
            }
        }
    }
    if (!rr.ok) {
        r->ok = false;
    }
}

void ur_put_list(plink_w_t *w, const ur_table_t *t, const void *objs, size_t count)
{
    plink_w_u16(w, (uint16_t)count);
    for (size_t i = 0; i < count; i++) {
        ur_put(w, t, (const uint8_t *)objs + i * t->size);
    }
}

size_t ur_get_list(plink_r_t *r, const ur_table_t *t, void *objs, size_t max)
{
    const size_t count = plink_r_u16(r);
    size_t got = 0;
    for (size_t i = 0; i < count && r->ok; i++) {
        if (got < max) {
            ur_get(r, t, (uint8_t *)objs + got * t->size);
            got++;
        } else { /* more than the caller holds: skip the rest */
            const size_t n = plink_r_u16(r);
            (void)plink_r_take(r, n);
        }
    }
    return r->ok ? got : 0u;
}

/* ---- the tables (append only: the frozen fixture holds each one's bytes) ---- */

static const ur_field_t f_address[] = {
    UR_INT(zsrc_address_t, kind),
    UR_INT(zsrc_address_t, input),
    UR_INT(zsrc_address_t, id),
    UR_STR(zsrc_address_t, ref),
};
static const ur_table_t t_address = UR_TABLE(zsrc_address_t, f_address);

static const ur_field_t f_mapping[] = {
    UR_INT(zsrc_mapping_t, mode),
    UR_BOOL(zsrc_mapping_t, invert),
    UR_INT(zsrc_mapping_t, threshold_x100),
    UR_INT(zsrc_mapping_t, hysteresis_x100),
    UR_STR(zsrc_mapping_t, states),
    UR_INT(zsrc_mapping_t, grace_s),
    UR_INT(zsrc_mapping_t, response_ms),
    UR_INT(zsrc_mapping_t, hold_s),
};
static const ur_table_t t_mapping = UR_TABLE(zsrc_mapping_t, f_mapping);

static const ur_field_t f_input[] = {
    UR_REC(zsrc_input_t, addr, t_address),
    UR_REC(zsrc_input_t, map, t_mapping),
};
static const ur_table_t t_input = UR_TABLE(zsrc_input_t, f_input);

static const ur_field_t f_binding[] = {
    UR_RECS(zone_hub_binding_t, in, t_input),
};
const ur_table_t ur_t_binding = UR_TABLE(zone_hub_binding_t, f_binding);

static const ur_field_t f_info[] = {
    UR_REC(zone_hub_info_t, binding, ur_t_binding),
    UR_BOOL(zone_hub_info_t, lost),
    UR_BOOL(zone_hub_info_t, source_present),
    UR_INT(zone_hub_info_t, caps),
    UR_BOOL(zone_hub_info_t, link_down),
    UR_BOOL(zone_hub_info_t, unreachable),
    UR_BOOL(zone_hub_info_t, check),
    UR_BOOL(zone_hub_info_t, known),
    UR_BOOL(zone_hub_info_t, active),
    UR_BOOL(zone_hub_info_t, has_value),
    UR_INT(zone_hub_info_t, value_x100),
    UR_INT(zone_hub_info_t, heard_at),
    UR_STR(zone_hub_info_t, address),
};
const ur_table_t ur_t_info = UR_TABLE(zone_hub_info_t, f_info);

static const ur_field_t f_candidate[] = {
    UR_REC(zsrc_candidate_t, addr, t_address),
    UR_STR(zsrc_candidate_t, name),
    UR_STR(zsrc_candidate_t, detail),
    UR_STR(zsrc_candidate_t, state),
    UR_INT(zsrc_candidate_t, device_class),
    UR_BOOL(zsrc_candidate_t, changed),
    UR_BOOL(zsrc_candidate_t, best),
    UR_BOOL(zsrc_candidate_t, unusable),
    UR_INT(zsrc_candidate_t, zone),
    UR_BOOL(zsrc_candidate_t, eol),
};
const ur_table_t ur_t_candidate = UR_TABLE(zsrc_candidate_t, f_candidate);

static const ur_field_t f_source[] = {
    UR_INT(ui_zone_source_t, kind),
    UR_INT(ui_zone_source_t, caps),
    UR_BOOL(ui_zone_source_t, usable),
};
const ur_table_t ur_t_source = UR_TABLE(ui_zone_source_t, f_source);

size_t ui_remote_enc_binding(const zone_hub_binding_t *b, uint8_t *out, size_t cap)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    ur_put(&w, &ur_t_binding, b);
    return w.ok ? w.len : 0u;
}

bool ui_remote_dec_binding(const uint8_t *p, size_t len, zone_hub_binding_t *b)
{
    plink_r_t r;
    plink_r_init(&r, p, len);
    ur_get(&r, &ur_t_binding, b);
    return r.ok;
}
