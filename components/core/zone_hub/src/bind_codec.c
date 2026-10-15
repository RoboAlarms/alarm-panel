/*
 * bind_codec.c - the binding table as a versioned, self-describing blob (see zone_hub.h), and
 * the store of items this firmware doesn't understand.
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "hub_internal.h"

#define HDR_LEN 16u

/* Group 0x01 (inputs). External contract: never reuse or renumber a field. */
enum {
    F_SELECT = 0x00, /* zone u16 | role u8 */
    F_KIND = 0x01,
    F_INPUT = 0x02,
    F_ID = 0x03,
    F_REF = 0x04,
    F_MODE = 0x05,
    F_INVERT = 0x06,
    F_THRESHOLD = 0x07,
    F_HYSTERESIS = 0x08,
    F_STATES = 0x09,
    F_GRACE = 0x0A,
    F_RESPONSE = 0x0B,
    F_HOLD = 0x0C, /* u16 seconds, RF sensors (docs/features/25); older readers keep it unread */
};

/* ---- kept items -------------------------------------------------------------------------------- */

bool zh_keep_add(zone_hub_t *h, uint16_t zone, uint8_t role, const uint8_t *item, size_t len)
{
    if (len > (size_t)ZONE_HUB_KEEP_BYTES - h->keep_len ||
        ZH_KEEP_HDR > (size_t)ZONE_HUB_KEEP_BYTES - h->keep_len - len) {
        return false;
    }
    uint8_t *p = h->keep + h->keep_len;
    p[0] = (uint8_t)zone;
    p[1] = (uint8_t)(zone >> 8);
    p[2] = role;
    memcpy(p + ZH_KEEP_HDR, item, len);
    h->keep_len = (uint16_t)(h->keep_len + ZH_KEEP_HDR + len);
    return true;
}

static size_t record_len(const uint8_t *rec)
{
    return ZH_KEEP_HDR + ZH_ITEM_HDR + rec[ZH_KEEP_HDR + 2u];
}

static uint16_t record_zone(const uint8_t *rec)
{
    return (uint16_t)(rec[0] | (rec[1] << 8));
}

void zh_keep_drop(zone_hub_t *h, uint16_t zone, unsigned role)
{
    size_t at = 0;
    while (at < h->keep_len) {
        uint8_t *rec = h->keep + at;
        const size_t n = record_len(rec);
        if (zone != 0u && record_zone(rec) == zone && (role == ZH_ANY_ROLE || rec[2] == role)) {
            memmove(rec, rec + n, h->keep_len - at - n);
            h->keep_len = (uint16_t)(h->keep_len - n);
        } else {
            at += n;
        }
    }
}

/* ---- encoding ---------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} writer_t;

static void put(writer_t *w, const void *data, size_t n)
{
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void put_item(writer_t *w, uint8_t field, const void *value, size_t len)
{
    const uint8_t head[ZH_ITEM_HDR] = {field, ZH_GRP_INPUT, (uint8_t)len};
    put(w, head, sizeof(head));
    put(w, value, len);
}

static void put_le(writer_t *w, uint8_t field, uint64_t v, unsigned width)
{
    uint8_t b[8];
    for (unsigned i = 0; i < width; i++) {
        b[i] = (uint8_t)(v >> (8u * i));
    }
    put_item(w, field, b, width);
}

static void put_str(writer_t *w, uint8_t field, const char *s, size_t size)
{
    const char *end = memchr(s, '\0', size);
    const size_t n = end != NULL ? (size_t)(end - s) : size - 1u;
    if (n > 0u) {
        put_item(w, field, s, n);
    }
}

static void put_input(writer_t *w, const zone_hub_input_t *in)
{
    const uint8_t sel[3] = {(uint8_t)in->zone, (uint8_t)(in->zone >> 8), in->role};
    put_item(w, F_SELECT, sel, sizeof(sel));
    const zsrc_address_t *a = &in->in.addr;
    const zsrc_mapping_t *m = &in->in.map;
    put_le(w, F_KIND, a->kind, 1u); /* never zero */
    if (a->input != 0u) {
        put_le(w, F_INPUT, a->input, 1u);
    }
    if (a->id != 0u) {
        put_le(w, F_ID, a->id, 8u);
    }
    put_str(w, F_REF, a->ref, sizeof(a->ref));
    if (m->mode != 0u) {
        put_le(w, F_MODE, m->mode, 1u);
    }
    if (m->invert) {
        put_le(w, F_INVERT, 1u, 1u);
    }
    if (m->threshold_x100 != 0) {
        put_le(w, F_THRESHOLD, (uint32_t)m->threshold_x100, 4u);
    }
    if (m->hysteresis_x100 != 0) {
        put_le(w, F_HYSTERESIS, (uint32_t)m->hysteresis_x100, 4u);
    }
    put_str(w, F_STATES, m->states, sizeof(m->states));
    if (m->grace_s != 0u) {
        put_le(w, F_GRACE, m->grace_s, 2u);
    }
    if (m->response_ms != 0u) {
        put_le(w, F_RESPONSE, m->response_ms, 2u);
    }
    if (m->hold_s != 0u) {
        put_le(w, F_HOLD, m->hold_s, 2u);
    }
}

/* Kept records of one input (zone, role), or with 'rest' every record that belongs to no input
 * this firmware knows. */
static void put_kept(writer_t *w, const zone_hub_t *h, uint16_t zone, uint8_t role, bool rest)
{
    size_t at = 0;
    while (at < h->keep_len) {
        const uint8_t *rec = h->keep + at;
        const size_t n = record_len(rec);
        const uint16_t rz = record_zone(rec);
        const bool own = rz != 0u && rec[2] < ZSRC_ROLE_COUNT && zh_find_role(h, rz, rec[2]) >= 0;
        if (rest ? !own : (rz == zone && rec[2] == role)) {
            put(w, rec + ZH_KEEP_HDR, n - ZH_KEEP_HDR);
        }
        at += n;
    }
}

static void wr_le(uint8_t *p, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

zone_hub_result_t zone_hub_encode(const zone_hub_t *h, uint8_t *buf, size_t cap, size_t *len_out)
{
    if (h == NULL || buf == NULL || cap < HDR_LEN) {
        return ZONE_HUB_ERR_ARG;
    }
    writer_t w = {buf, cap, HDR_LEN, false};
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) { /* zone and role order: stable bytes */
        for (uint8_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
            const int i = zh_find_role(h, z, r);
            if (i >= 0) {
                put_input(&w, &h->inputs[i]);
                put_kept(&w, h, z, r, false);
            }
        }
    }
    put_kept(&w, h, 0u, 0u, true);
    if (w.overflow) {
        return ZONE_HUB_ERR_ARG;
    }
    const size_t payload = w.len - HDR_LEN;
    memcpy(buf, "ZBND", 4);
    buf[4] = ZONE_HUB_BLOB_FORMAT;
    buf[5] = 1u; /* readers of format 1 understand everything written here */
    wr_le(buf + 6, HDR_LEN, 2u);
    wr_le(buf + 8, (uint32_t)payload, 4u);
    wr_le(buf + 12, alarm_crc32(0u, buf + HDR_LEN, payload), 4u);
    if (len_out != NULL) {
        *len_out = w.len;
    }
    return ZONE_HUB_OK;
}

/* ---- decoding ---------------------------------------------------------------------------------- */

static uint64_t rd_le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static bool get_str(char *dst, size_t size, const uint8_t *val, uint8_t len)
{
    if (len >= size || memchr(val, '\0', len) != NULL) {
        return false;
    }
    memset(dst, 0, size);
    memcpy(dst, val, len);
    return true;
}

/* 1 applied, 0 a field this firmware doesn't know, -1 a bad value. */
static int apply(zone_hub_input_t *in, uint8_t field, const uint8_t *val, uint8_t len)
{
    zsrc_address_t *a = &in->in.addr;
    zsrc_mapping_t *m = &in->in.map;
    const uint64_t v = (len >= 1u && len <= 8u) ? rd_le(val, len) : UINT64_MAX;
    switch (field) {
    case F_KIND:
        if (len != 1u || v == 0u) {
            return -1;
        }
        a->kind = (uint8_t)v;
        return 1;
    case F_INPUT:
        if (len != 1u) {
            return -1;
        }
        a->input = (uint8_t)v;
        return 1;
    case F_ID:
        if (len < 1u || len > 8u) {
            return -1;
        }
        a->id = v;
        return 1;
    case F_REF: return get_str(a->ref, sizeof(a->ref), val, len) ? 1 : -1;
    case F_MODE:
        if (len != 1u || v >= ZSRC_MAP_COUNT) {
            return -1;
        }
        m->mode = (uint8_t)v;
        return 1;
    case F_INVERT:
        if (len != 1u || v > 1u) {
            return -1;
        }
        m->invert = v != 0u;
        return 1;
    case F_THRESHOLD:
    case F_HYSTERESIS: {
        if (len != 4u) {
            return -1;
        }
        const uint32_t u = (uint32_t)v;
        int32_t s;
        memcpy(&s, &u, sizeof(s));
        if (field == F_THRESHOLD) {
            m->threshold_x100 = s;
        } else if (s >= 0) {
            m->hysteresis_x100 = s;
        } else {
            return -1;
        }
        return 1;
    }
    case F_STATES: return get_str(m->states, sizeof(m->states), val, len) ? 1 : -1;
    case F_GRACE:
    case F_RESPONSE:
    case F_HOLD:
        if (len < 1u || len > 2u) {
            return -1;
        }
        if (field == F_GRACE) {
            m->grace_s = (uint16_t)v;
        } else if (field == F_RESPONSE) {
            m->response_ms = (uint16_t)v;
        } else {
            m->hold_s = (uint16_t)v;
        }
        return 1;
    default: return 0;
    }
}

static zone_hub_result_t fail(zone_hub_t *h)
{
    memset(h->inputs, 0, sizeof(h->inputs));
    h->keep_len = 0u;
    h->keep_lost = 0u;
    return ZONE_HUB_ERR_ARG;
}

zone_hub_result_t zone_hub_decode(zone_hub_t *h, const uint8_t *buf, size_t len)
{
    memset(h->inputs, 0, sizeof(h->inputs));
    h->keep_len = 0u;
    h->keep_lost = 0u;
    if (buf == NULL || len < HDR_LEN || memcmp(buf, "ZBND", 4) != 0 ||
        buf[5] > ZONE_HUB_BLOB_COMPAT) {
        return ZONE_HUB_ERR_ARG;
    }
    const uint64_t hdr = rd_le(buf + 6, 2u);
    const uint64_t payload = rd_le(buf + 8, 4u);
    if (hdr < HDR_LEN || hdr > len || payload > len - hdr ||
        alarm_crc32(0u, buf + hdr, (size_t)payload) != (uint32_t)rd_le(buf + 12, 4u)) {
        return ZONE_HUB_ERR_ARG;
    }
    const uint8_t *p = buf + hdr;
    const uint8_t *end = p + payload;
    zone_hub_input_t *cur = NULL;
    bool raw = false; /* the current entry is kept as it is (unknown role, zone beyond us) */
    uint16_t raw_zone = 0u;
    uint8_t raw_role = 0u;
    while (p < end) {
        if (end - p < (ptrdiff_t)ZH_ITEM_HDR) {
            return fail(h);
        }
        const uint8_t field = p[0];
        const uint8_t group = p[1];
        const uint8_t vlen = p[2];
        const uint8_t *val = p + ZH_ITEM_HDR;
        if ((size_t)(end - val) < vlen) {
            return fail(h);
        }
        const uint8_t *item = p;
        const size_t item_len = ZH_ITEM_HDR + (size_t)vlen;
        p = val + vlen;
        bool keep = false;
        uint16_t kz = 0u;
        uint8_t kr = 0u;
        if (group != ZH_GRP_INPUT) {
            keep = true; /* a group from a newer format */
        } else if (field == F_SELECT) {
            if (vlen != 3u) {
                return fail(h);
            }
            const uint16_t zone = (uint16_t)rd_le(val, 2u);
            const uint8_t role = val[2];
            if (zone == 0u) {
                return fail(h);
            }
            cur = NULL;
            raw = zone > ALARM_MAX_ZONES || role >= ZSRC_ROLE_COUNT;
            if (!raw) {
                if (zh_find_role(h, zone, role) >= 0) {
                    return fail(h); /* the same input twice */
                }
                const int slot = zh_find_role(h, 0u, 0u);
                if (slot < 0) {
                    raw = true; /* more inputs than this build holds: keep the rest as they are */
                } else {
                    cur = &h->inputs[slot];
                    cur->zone = zone;
                    cur->role = role;
                }
            }
            if (raw) {
                raw_zone = zone;
                raw_role = role;
                keep = true;
                kz = zone;
                kr = role;
            }
        } else if (raw) {
            keep = true;
            kz = raw_zone;
            kr = raw_role;
        } else if (cur == NULL) {
            return fail(h); /* a field before any input */
        } else {
            const int r = apply(cur, field, val, vlen);
            if (r < 0) {
                return fail(h);
            }
            if (r == 0) {
                keep = true; /* a field from a newer format */
                kz = cur->zone;
                kr = cur->role;
            }
        }
        if (keep && !zh_keep_add(h, kz, kr, item, item_len)) {
            /* No room: the zones still work; the platform logs it and leaves the stored blob
             * alone until the installer changes a binding. */
            const size_t lost = (size_t)h->keep_lost + item_len;
            h->keep_lost = (uint16_t)(lost > UINT16_MAX ? UINT16_MAX : lost);
        }
    }
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        const zone_hub_input_t *in = &h->inputs[i];
        if (in->zone == 0u) {
            continue;
        }
        if (in->in.addr.kind == ZSRC_NONE) {
            return fail(h); /* every input names its source */
        }
        for (int j = 0; j < i; j++) {
            if (h->inputs[j].zone != 0u && zsrc_address_equal(&h->inputs[j].in.addr, &in->in.addr)) {
                return fail(h); /* one sensor feeds one zone */
            }
        }
    }
    return ZONE_HUB_OK;
}
