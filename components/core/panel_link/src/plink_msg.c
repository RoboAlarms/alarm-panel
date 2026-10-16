/*
 * plink_msg.c - the panel link's message payloads (plink_msg.h; docs/features/11 "Messages",
 * "STATUS", "Zone record", "Event record", "Commands"; LINK-005, LINK-017, LINK-018).
 *
 * Every layout here is the table of docs/features/11 field by field. Encoders write through a
 * plink_w_t and check it once; decoders zero the message, read through a plink_r_t and keep
 * nothing when it ran short. A section is read with a padding reader, so a section a newer
 * firmware lengthened is read as far as this one knows, and an older, shorter one reads its
 * missing fields as 0 (LINK-017).
 */
#include "panel_link/plink_msg.h"

#include <string.h>

#include "alarm_proto/proto_link.h" /* proto_sha256 */
#include "panel_link/plink_wire.h"

#define CMD_OP_LEN     9u /* CMD's op section in v1, after its length byte */
#define EVENTS_HDR_LEN 3u /* EVENTS: flags, count, rec_len */

/* ---- shared helpers ----------------------------------------------------------------------- */

/* The end of every encoder: the length when everything fit. */
static bool enc_done(const plink_w_t *w, size_t *len)
{
    if (len != NULL) {
        *len = w->ok ? w->len : 0u;
    }
    return w->ok;
}

/* The end of every decoder: nothing half-decoded is left behind. */
static bool dec_done(const plink_r_t *r, void *m, size_t size)
{
    if (!r->ok) {
        memset(m, 0, size);
    }
    return r->ok;
}

/* A reader over everything but a MAC-ending message's last 16 bytes (its fields; whatever a
 * newer version appended before the MAC is skipped). false: too short for a MAC. */
static bool r_before_mac(plink_r_t *r, const uint8_t *p, size_t len)
{
    plink_r_init(r, p, len >= PLINK_MAC_LEN ? len - PLINK_MAC_LEN : 0u);
    return p != NULL && len >= PLINK_MAC_LEN;
}

static int8_t to_i8(uint8_t v)
{
    return v < 0x80u ? (int8_t)v : (int8_t)((int)v - 256);
}

static void w_caps(plink_w_t *w, const plink_caps_t *c)
{
    const size_t at = plink_w_section(w);
    plink_w_u8(w, c->partitions);
    plink_w_u16(w, c->zones);
    plink_w_u8(w, c->users);
    plink_w_u8(w, c->devices);
    plink_w_section_end(w, at);
}

static void r_caps(plink_r_t *r, plink_caps_t *c)
{
    plink_r_t s = plink_r_section(r);
    c->partitions = plink_r_u8(&s);
    c->zones = plink_r_u16(&s);
    c->users = plink_r_u8(&s);
    c->devices = plink_r_u8(&s);
}

/* ---- pairing ------------------------------------------------------------------------------ */

bool plink_enc_pair_req(const plink_pair_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->attempt);
    plink_w_u8(&w, m->proto);
    plink_w_u16(&w, m->caps);
    plink_w_u8(&w, m->kind);
    w_caps(&w, &m->cap);
    plink_w_bytes(&w, m->pk, PLINK_X25519_LEN);
    plink_w_str(&w, m->name, PLINK_NAME_MAX);
    plink_w_str(&w, m->fw, PLINK_FW_MAX);
    return enc_done(&w, len);
}

bool plink_dec_pair_req(const uint8_t *p, size_t len, plink_pair_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->attempt = plink_r_u32(&r);
    m->proto = plink_r_u8(&r);
    m->caps = plink_r_u16(&r);
    m->kind = plink_r_u8(&r);
    r_caps(&r, &m->cap);
    plink_r_bytes(&r, m->pk, PLINK_X25519_LEN);
    plink_r_str(&r, m->name, sizeof(m->name));
    plink_r_str(&r, m->fw, sizeof(m->fw));
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_pair_commit(const plink_pair_commit_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->attempt);
    plink_w_u8(&w, m->proto);
    plink_w_u16(&w, m->caps);
    plink_w_bytes(&w, m->pk, PLINK_X25519_LEN);
    plink_w_bytes(&w, m->commit, PLINK_SHA256_LEN);
    plink_w_str(&w, m->name, PLINK_NAME_MAX);
    plink_w_str(&w, m->panel_id, PLINK_PANEL_ID_LEN);
    return enc_done(&w, len);
}

bool plink_dec_pair_commit(const uint8_t *p, size_t len, plink_pair_commit_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->attempt = plink_r_u32(&r);
    m->proto = plink_r_u8(&r);
    m->caps = plink_r_u16(&r);
    plink_r_bytes(&r, m->pk, PLINK_X25519_LEN);
    plink_r_bytes(&r, m->commit, PLINK_SHA256_LEN);
    plink_r_str(&r, m->name, sizeof(m->name));
    plink_r_str(&r, m->panel_id, sizeof(m->panel_id));
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_pair_nonce(const plink_pair_nonce_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->attempt);
    plink_w_bytes(&w, m->n, PLINK_NONCE_LEN);
    return enc_done(&w, len);
}

bool plink_dec_pair_nonce(const uint8_t *p, size_t len, plink_pair_nonce_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->attempt = plink_r_u32(&r);
    plink_r_bytes(&r, m->n, PLINK_NONCE_LEN);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_pair_confirm(const plink_pair_confirm_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->attempt);
    plink_w_bytes(&w, m->mac, PLINK_MAC_LEN);
    return enc_done(&w, len);
}

bool plink_dec_pair_confirm(const uint8_t *p, size_t len, plink_pair_confirm_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    if (!r_before_mac(&r, p, len)) {
        return false;
    }
    m->attempt = plink_r_u32(&r);
    if (r.ok) {
        memcpy(m->mac, p + len - PLINK_MAC_LEN, PLINK_MAC_LEN);
    }
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_pair_done(const plink_pair_done_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->attempt);
    plink_w_u8(&w, m->device);
    plink_w_u8(&w, m->home);
    plink_w_bytes(&w, m->mac, PLINK_MAC_LEN);
    return enc_done(&w, len);
}

bool plink_dec_pair_done(const uint8_t *p, size_t len, plink_pair_done_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    if (!r_before_mac(&r, p, len)) {
        return false;
    }
    m->attempt = plink_r_u32(&r);
    m->device = plink_r_u8(&r);
    m->home = plink_r_u8(&r);
    if (r.ok) {
        memcpy(m->mac, p + len - PLINK_MAC_LEN, PLINK_MAC_LEN);
    }
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_pair_fail(const plink_pair_fail_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->attempt);
    plink_w_u8(&w, m->reason);
    return enc_done(&w, len);
}

bool plink_dec_pair_fail(const uint8_t *p, size_t len, plink_pair_fail_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->attempt = plink_r_u32(&r);
    m->reason = plink_r_u8(&r);
    return dec_done(&r, m, sizeof(*m));
}

/* ---- handshake ---------------------------------------------------------------------------- */

bool plink_enc_hello(const plink_hello_t *m, bool hello, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u8(&w, m->proto);
    plink_w_u16(&w, m->caps);
    if (hello) {
        w_caps(&w, &m->cap);
    }
    plink_w_bytes(&w, m->n, PLINK_NONCE_LEN);
    plink_w_bytes(&w, m->e, PLINK_X25519_LEN);
    plink_w_bytes(&w, m->mac, PLINK_MAC_LEN);
    return enc_done(&w, len);
}

bool plink_dec_hello(const uint8_t *p, size_t len, bool hello, plink_hello_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    if (!r_before_mac(&r, p, len)) {
        return false;
    }
    m->proto = plink_r_u8(&r);
    m->caps = plink_r_u16(&r);
    if (hello) {
        r_caps(&r, &m->cap);
    }
    plink_r_bytes(&r, m->n, PLINK_NONCE_LEN);
    plink_r_bytes(&r, m->e, PLINK_X25519_LEN);
    if (r.ok) {
        memcpy(m->mac, p + len - PLINK_MAC_LEN, PLINK_MAC_LEN);
    }
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_rekey(const plink_rekey_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->sid);
    return enc_done(&w, len);
}

bool plink_dec_rekey(const uint8_t *p, size_t len, plink_rekey_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->sid = plink_r_u32(&r);
    return dec_done(&r, m, sizeof(*m));
}

/* ---- STATUS ------------------------------------------------------------------------------- */

static void w_status_system(plink_w_t *w, const plink_status_t *m)
{
    const size_t at = plink_w_section(w);
    plink_w_i64(w, m->epoch_ms);
    plink_w_u8(w, m->partitions);
    plink_w_u8(w, m->home);
    plink_w_u32(w, m->sys_troubles);
    plink_w_u32(w, m->sys_restored);
    plink_w_u32(w, m->dev_troubles);
    plink_w_u8(w, m->default_codes);
    plink_w_u8(w, m->flags);
    plink_w_u32(w, m->recovery_left_ms);
    plink_w_u32(w, m->lockout_left_ms);
    plink_w_u8(w, m->report);
    plink_w_u16(w, m->zones_used);
    plink_w_u32(w, m->last_event_seq);
    plink_w_u32(w, m->cfg_crc);
    plink_w_u32(w, m->cfg_len);
    plink_w_u32(w, m->zones_crc);
    plink_w_section_end(w, at);
}

static void r_status_system(plink_r_t *r, plink_status_t *m)
{
    plink_r_t s = plink_r_section(r);
    m->epoch_ms = plink_r_i64(&s);
    m->partitions = plink_r_u8(&s);
    m->home = plink_r_u8(&s);
    m->sys_troubles = plink_r_u32(&s);
    m->sys_restored = plink_r_u32(&s);
    m->dev_troubles = plink_r_u32(&s);
    m->default_codes = plink_r_u8(&s);
    m->flags = plink_r_u8(&s);
    m->recovery_left_ms = plink_r_u32(&s);
    m->lockout_left_ms = plink_r_u32(&s);
    m->report = plink_r_u8(&s);
    m->zones_used = plink_r_u16(&s);
    m->last_event_seq = plink_r_u32(&s);
    m->cfg_crc = plink_r_u32(&s);
    m->cfg_len = plink_r_u32(&s);
    m->zones_crc = plink_r_u32(&s);
}

/* A partition record: its number, then its fields as a section (docs/features/11 "STATUS"). */
static void w_status_part(plink_w_t *w, const plink_part_t *p)
{
    plink_w_u8(w, p->number);
    const size_t at = plink_w_section(w);
    plink_w_u8(w, p->state);
    plink_w_u8(w, p->level);
    plink_w_u8(w, p->arm_flags);
    plink_w_u8(w, p->armed_by);
    plink_w_u8(w, p->arm_src_kind);
    plink_w_u16(w, p->arm_src_id);
    plink_w_u8(w, p->ready_levels);
    plink_w_u8(w, p->flags);
    plink_w_u8(w, p->sounder);
    plink_w_u8(w, p->bell);
    plink_w_u32(w, p->delay_left_ms);
    plink_w_u16(w, p->memory);
    plink_w_u16(w, p->faults);
    plink_w_u16(w, p->bypassed);
    plink_w_u16(w, p->alarms);
    plink_w_u16(w, p->troubles);
    plink_w_u32(w, p->walk_left_ms);
    plink_w_u32(w, p->walk_age_ms);
    plink_w_u32(w, p->walk_id);
    plink_w_u32(w, p->walk_seq);
    plink_w_u16(w, p->walk_tested);
    plink_w_u16(w, p->walk_total);
    plink_w_u8(w, p->walk_flags);
    plink_w_u8(w, p->walk_started_by);
    plink_w_u8(w, p->walk_ended_by);
    plink_w_u8(w, p->walk_end_how);
    plink_w_u8(w, p->walk_src_kind);
    plink_w_u16(w, p->walk_src_id);
    plink_w_section_end(w, at);
}

static void r_status_part(plink_r_t *s, plink_part_t *p)
{
    p->state = plink_r_u8(s);
    p->level = plink_r_u8(s);
    p->arm_flags = plink_r_u8(s);
    p->armed_by = plink_r_u8(s);
    p->arm_src_kind = plink_r_u8(s);
    p->arm_src_id = plink_r_u16(s);
    p->ready_levels = plink_r_u8(s);
    p->flags = plink_r_u8(s);
    p->sounder = plink_r_u8(s);
    p->bell = plink_r_u8(s);
    p->delay_left_ms = plink_r_u32(s);
    p->memory = plink_r_u16(s);
    p->faults = plink_r_u16(s);
    p->bypassed = plink_r_u16(s);
    p->alarms = plink_r_u16(s);
    p->troubles = plink_r_u16(s);
    p->walk_left_ms = plink_r_u32(s);
    p->walk_age_ms = plink_r_u32(s);
    p->walk_id = plink_r_u32(s);
    p->walk_seq = plink_r_u32(s);
    p->walk_tested = plink_r_u16(s);
    p->walk_total = plink_r_u16(s);
    p->walk_flags = plink_r_u8(s);
    p->walk_started_by = plink_r_u8(s);
    p->walk_ended_by = plink_r_u8(s);
    p->walk_end_how = plink_r_u8(s);
    p->walk_src_kind = plink_r_u8(s);
    p->walk_src_id = plink_r_u16(s);
}

bool plink_enc_status(const plink_status_t *m, uint8_t first_part, uint8_t *out, size_t cap,
                      size_t *len, uint8_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || m->count > PLINK_PARTS_MAX || first_part > m->count) {
        w.ok = false;
        return enc_done(&w, len);
    }
    w_status_system(&w, m);
    uint8_t n = 0;
    for (uint8_t i = first_part; w.ok && i < m->count; i++) {
        const size_t before = w.len;
        w_status_part(&w, &m->part[i]);
        if (!w.ok) {
            /* This record didn't fit: the frame ends before it, the next frame starts with it. */
            w.len = before;
            w.ok = n > 0u;
            break;
        }
        n++;
    }
    if (w.ok && written != NULL) {
        *written = n;
    }
    return enc_done(&w, len);
}

bool plink_dec_status(const uint8_t *p, size_t len, plink_status_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    r_status_system(&r, m);
    unsigned seen = 0; /* bit n-1: partition n already taken */
    while (r.ok && plink_r_left(&r) > 0u) {
        const uint8_t number = plink_r_u8(&r);
        plink_r_t s = plink_r_section(&r);
        if (!r.ok) {
            break; /* a record cut short: the frame is damaged */
        }
        /* A number this firmware can't hold, or one already given, is skipped. */
        if (number == 0u || number > PLINK_PARTS_MAX || (seen & (1u << (number - 1u))) != 0u) {
            continue;
        }
        seen |= 1u << (number - 1u);
        plink_part_t *pt = &m->part[m->count++];
        pt->number = number;
        r_status_part(&s, pt);
    }
    return dec_done(&r, m, sizeof(*m));
}

/* ---- ZONES -------------------------------------------------------------------------------- */

/* Zone record (docs/features/11, 7 bytes in v1). */
static void w_zone_rec(plink_w_t *w, const alarm_zone_status_t *z)
{
    plink_w_u16(w, z->flags);
    plink_w_u8(w, z->trouble);
    plink_w_u8(w, z->walk);
    plink_w_u16(w, z->walk_first_s);
    plink_w_u8(w, z->walk_trips);
}

/* Zones first..first+count-1 exist: 1-based, within u16. */
static bool zone_range_ok(uint16_t first, uint16_t count)
{
    return count == 0u || (first != 0u && (uint32_t)first + count - 1u <= 0xFFFFu);
}

bool plink_enc_zones(uint32_t zones_crc, uint16_t first, const alarm_zone_status_t *z,
                     uint16_t count, uint8_t *out, size_t cap, size_t *len, uint16_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if ((z == NULL && count > 0u) || !zone_range_ok(first, count)) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, zones_crc);
    plink_w_u16(&w, first);
    const size_t count_at = w.len;
    plink_w_u16(&w, 0); /* the count, once known */
    plink_w_u8(&w, PLINK_ZONE_REC_LEN);
    uint16_t n = 0;
    while (w.ok && n < count) {
        const size_t before = w.len;
        w_zone_rec(&w, &z[n]);
        if (!w.ok) {
            w.len = before;
            w.ok = n > 0u;
            break;
        }
        n++;
    }
    if (w.ok) {
        w.p[count_at] = (uint8_t)n;
        w.p[count_at + 1u] = (uint8_t)(n >> 8);
        if (written != NULL) {
            *written = n;
        }
    }
    return enc_done(&w, len);
}

bool plink_dec_zones(const uint8_t *p, size_t len, plink_zones_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->zones_crc = plink_r_u32(&r);
    m->first = plink_r_u16(&r);
    m->count = plink_r_u16(&r);
    m->rec_len = plink_r_u8(&r);
    if (r.ok && !zone_range_ok(m->first, m->count)) {
        r.ok = false; /* zone 0, or past the last zone number */
    }
    /* count records of rec_len bytes each; plink_zones_rec reads each one (a longer record's
     * extra bytes skipped, a shorter one's missing fields 0). */
    m->recs = plink_r_take(&r, (size_t)m->count * m->rec_len);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_zones_req(const plink_zones_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u16(&w, m->first);
    plink_w_u16(&w, m->count);
    return enc_done(&w, len);
}

bool plink_dec_zones_req(const uint8_t *p, size_t len, plink_zones_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->first = plink_r_u16(&r);
    m->count = plink_r_u16(&r);
    return dec_done(&r, m, sizeof(*m));
}

/* ---- configuration ------------------------------------------------------------------------ */

bool plink_enc_cfg_req(const plink_cfg_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->cfg_crc);
    plink_w_u32(&w, m->offset);
    plink_w_u16(&w, m->max);
    return enc_done(&w, len);
}

bool plink_dec_cfg_req(const uint8_t *p, size_t len, plink_cfg_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->cfg_crc = plink_r_u32(&r);
    m->offset = plink_r_u32(&r);
    m->max = plink_r_u16(&r);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_cfg_page(const plink_cfg_page_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || (m->data == NULL && m->len > 0u) ||
        (uint64_t)m->offset + m->len > m->total) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->cfg_crc);
    plink_w_u32(&w, m->total);
    plink_w_u32(&w, m->offset);
    plink_w_u16(&w, m->len);
    plink_w_bytes(&w, m->data, m->len);
    return enc_done(&w, len);
}

bool plink_dec_cfg_page(const uint8_t *p, size_t len, plink_cfg_page_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->cfg_crc = plink_r_u32(&r);
    m->total = plink_r_u32(&r);
    m->offset = plink_r_u32(&r);
    m->len = plink_r_u16(&r);
    m->data = plink_r_take(&r, m->len);
    if (r.ok && (uint64_t)m->offset + m->len > m->total) {
        r.ok = false; /* a page outside the blob */
    }
    return dec_done(&r, m, sizeof(*m));
}

/* ---- events ------------------------------------------------------------------------------- */

bool plink_enc_ev_req(const plink_ev_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->after_seq);
    plink_w_u8(&w, m->max);
    return enc_done(&w, len);
}

bool plink_dec_ev_req(const uint8_t *p, size_t len, plink_ev_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->after_seq = plink_r_u32(&r);
    m->max = plink_r_u8(&r);
    return dec_done(&r, m, sizeof(*m));
}

/* Event record (docs/features/11, 32 bytes in v1). */
static void w_event_rec(plink_w_t *w, const alarm_event_t *e)
{
    plink_w_u32(w, e->seq);
    plink_w_u64(w, e->mono_ms);
    plink_w_i64(w, e->epoch_ms);
    plink_w_u16(w, (uint16_t)e->type);
    plink_w_u8(w, e->partition);
    plink_w_u16(w, e->zone);
    plink_w_u8(w, e->user);
    plink_w_u8(w, e->source.kind);
    plink_w_u16(w, e->source.id);
    plink_w_u8(w, e->arg);
    plink_w_u8(w, e->arg2);
    plink_w_u8(w, e->flags);
}

static void r_event_rec(plink_r_t *r, alarm_event_t *e)
{
    memset(e, 0, sizeof(*e));
    e->seq = plink_r_u32(r);
    e->mono_ms = plink_r_u64(r);
    e->epoch_ms = plink_r_i64(r);
    e->type = (alarm_event_type_t)plink_r_u16(r); /* a type this build doesn't know stays as is */
    e->partition = plink_r_u8(r);
    e->zone = plink_r_u16(r);
    e->user = plink_r_u8(r);
    e->source.kind = plink_r_u8(r);
    e->source.id = plink_r_u16(r);
    e->arg = plink_r_u8(r);
    e->arg2 = plink_r_u8(r);
    e->flags = plink_r_u8(r);
}

bool plink_enc_events(const alarm_event_t *ev, size_t n, uint8_t flags, uint8_t *out, size_t cap,
                      size_t *len, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (ev == NULL && n > 0u) {
        w.ok = false;
        return enc_done(&w, len);
    }
    /* The newest that fit (and a count fits a byte); the older ones are left out. */
    size_t fit = cap >= EVENTS_HDR_LEN ? (cap - EVENTS_HDR_LEN) / PLINK_EVENT_REC_LEN : 0u;
    if (fit > 255u) {
        fit = 255u;
    }
    const size_t k = n < fit ? n : fit;
    if (k < n) {
        flags = (uint8_t)(flags | PLINK_EVF_GAP);
    }
    if (n > 0u && k == 0u) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u8(&w, flags);
    plink_w_u8(&w, (uint8_t)k);
    plink_w_u8(&w, PLINK_EVENT_REC_LEN);
    for (size_t i = n - k; i < n; i++) {
        w_event_rec(&w, &ev[i]);
    }
    if (w.ok && written != NULL) {
        *written = k;
    }
    return enc_done(&w, len);
}

bool plink_dec_events(const uint8_t *p, size_t len, alarm_event_t *out, size_t max, size_t *n,
                      uint8_t *flags)
{
    if (n != NULL) {
        *n = 0;
    }
    if (flags != NULL) {
        *flags = 0;
    }
    if (out == NULL && max > 0u) {
        return false;
    }
    plink_r_t r;
    plink_r_init(&r, p, len);
    uint8_t f = plink_r_u8(&r);
    const size_t count = plink_r_u8(&r);
    const size_t rec_len = plink_r_u8(&r);
    const uint8_t *recs = plink_r_take(&r, count * rec_len);
    if (!r.ok) {
        return false;
    }
    /* Oldest first: when there are more than max, the newest max are kept. */
    const size_t k = count < max ? count : max;
    if (k < count) {
        f = (uint8_t)(f | PLINK_EVF_GAP);
    }
    for (size_t i = 0; i < k; i++) {
        plink_r_t rr;
        plink_r_init(&rr, rec_len > 0u ? recs + (count - k + i) * rec_len : NULL, rec_len);
        rr.pad = true; /* a record shorter than v1's reads its missing fields as 0 */
        r_event_rec(&rr, &out[i]);
    }
    if (n != NULL) {
        *n = k;
    }
    if (flags != NULL) {
        *flags = f;
    }
    return true;
}

/* ---- commands ----------------------------------------------------------------------------- */

/* The op section: the alarm_cmd_t's fields except the code, the same for every op (AUTH and
 * CHANGE_CODE read the partition only). */
static void w_cmd_op(plink_w_t *w, const alarm_cmd_t *c)
{
    const size_t at = plink_w_section(w);
    plink_w_u8(w, (uint8_t)c->type);
    plink_w_u8(w, c->partition);
    plink_w_u8(w, (uint8_t)c->level);
    plink_w_u8(w, c->arm_flags);
    plink_w_u16(w, c->zone);
    plink_w_u8(w, (uint8_t)c->panic);
    plink_w_u8(w, c->walk_op);
    plink_w_u8(w, c->walk_flags);
    plink_w_section_end(w, at);
}

static void r_cmd_op(plink_r_t *r, alarm_cmd_t *c)
{
    plink_r_t s = plink_r_section(r);
    c->type = (alarm_cmd_type_t)plink_r_u8(&s);
    c->partition = plink_r_u8(&s);
    c->level = (alarm_arm_level_t)plink_r_u8(&s);
    c->arm_flags = plink_r_u8(&s);
    c->zone = plink_r_u16(&s);
    c->panic = (alarm_panic_t)plink_r_u8(&s);
    c->walk_op = plink_r_u8(&s);
    c->walk_flags = plink_r_u8(&s);
}

/* A code's length when it is digits only and NUL-terminated within ALARM_CODE_MAX_LEN; -1
 * otherwise. */
static int code_len(const char *code)
{
    for (int i = 0; i <= ALARM_CODE_MAX_LEN; i++) {
        if (code[i] == '\0') {
            return i;
        }
        if (code[i] < '0' || code[i] > '9') {
            return -1;
        }
    }
    return -1;
}

/* A code on the wire: its length, then its digits zero-padded to ALARM_CODE_MAX_LEN, so every CMD
 * has the same size whatever the code (docs/features/11 "Commands"). */
static void w_code(plink_w_t *w, const char *code, int n)
{
    plink_w_u8(w, (uint8_t)n);
    plink_w_bytes(w, code, (size_t)n);
    for (int i = n; i < ALARM_CODE_MAX_LEN; i++) {
        plink_w_u8(w, 0);
    }
}

/* A code from the wire into out (ALARM_CODE_MAX_LEN + 1 bytes): false when it is longer than
 * ALARM_CODE_MAX_LEN or not digits only. The padding is not read. */
static bool r_code(plink_r_t *r, char out[ALARM_CODE_MAX_LEN + 1])
{
    const size_t n = plink_r_u8(r);
    const uint8_t *raw = plink_r_take(r, ALARM_CODE_MAX_LEN);
    if (!r->ok || n > ALARM_CODE_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (raw[i] < (uint8_t)'0' || raw[i] > (uint8_t)'9') {
            return false;
        }
        out[i] = (char)raw[i];
    }
    out[n] = '\0';
    return true;
}

bool plink_enc_cmd(const plink_cmd_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    const int code_n = m != NULL ? code_len(m->cmd.code) : -1;
    const int new_n = m != NULL ? code_len(m->new_code) : -1;
    if (code_n < 0 || new_n < 0) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->req);
    plink_w_u8(&w, m->op);
    w_cmd_op(&w, &m->cmd);
    w_code(&w, m->cmd.code, code_n);
    w_code(&w, m->new_code, new_n);
    return enc_done(&w, len);
}

bool plink_dec_cmd(const uint8_t *p, size_t len, plink_cmd_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->op = plink_r_u8(&r); /* an op this firmware doesn't know is the master's to answer */
    r_cmd_op(&r, &m->cmd);
    if (!r_code(&r, m->cmd.code) || !r_code(&r, m->new_code)) {
        r.ok = false;
    }
    return dec_done(&r, m, sizeof(*m)); /* a refused command leaves no code behind */
}

void plink_cmd_digest(const plink_cmd_t *m, uint8_t out[PLINK_SHA256_LEN])
{
    uint8_t buf[2u + CMD_OP_LEN]; /* op, the section's length, its fields: never a code */
    plink_w_t w;
    plink_w_init(&w, buf, sizeof(buf));
    if (m != NULL) {
        plink_w_u8(&w, m->op);
        w_cmd_op(&w, &m->cmd);
    }
    proto_sha256(buf, w.ok ? w.len : 0u, out);
}

/* ---- answers and the rest ----------------------------------------------------------------- */

bool plink_enc_result(const plink_result_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->req);
    plink_w_u8(&w, m->result);
    plink_w_u8(&w, m->user);
    plink_w_u16(&w, m->not_ready_zone);
    plink_w_u8(&w, m->may_disarm);
    return enc_done(&w, len);
}

bool plink_dec_result(const uint8_t *p, size_t len, plink_result_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->result = plink_r_u8(&r);
    m->user = plink_r_u8(&r);
    m->not_ready_zone = plink_r_u16(&r);
    m->may_disarm = plink_r_u8(&r);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_sound(const plink_sound_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u8(&w, m->partition);
    plink_w_u8(&w, m->sound);
    plink_w_u16(&w, m->zone);
    return enc_done(&w, len);
}

bool plink_dec_sound(const uint8_t *p, size_t len, plink_sound_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->partition = plink_r_u8(&r);
    m->sound = plink_r_u8(&r);
    m->zone = plink_r_u16(&r);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_heartbeat(const plink_heartbeat_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u8(&w, m->flags);
    plink_w_u8(&w, (uint8_t)m->rssi);
    plink_w_u32(&w, m->uptime_s);
    plink_w_u16(&w, m->status_age_s);
    plink_w_str(&w, m->fw, PLINK_FW_MAX);
    return enc_done(&w, len);
}

bool plink_dec_heartbeat(const uint8_t *p, size_t len, plink_heartbeat_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->flags = plink_r_u8(&r);
    m->rssi = to_i8(plink_r_u8(&r));
    m->uptime_s = plink_r_u32(&r);
    m->status_age_s = plink_r_u16(&r);
    plink_r_str(&r, m->fw, sizeof(m->fw));
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_time(const plink_time_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_i64(&w, m->epoch_ms);
    plink_w_str(&w, m->tz, PLINK_TZ_MAX);
    return enc_done(&w, len);
}

bool plink_dec_time(const uint8_t *p, size_t len, plink_time_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->epoch_ms = plink_r_i64(&r);
    plink_r_str(&r, m->tz, sizeof(m->tz));
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_bye(const plink_bye_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u8(&w, m->reason);
    return enc_done(&w, len);
}

bool plink_dec_bye(const uint8_t *p, size_t len, plink_bye_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->reason = plink_r_u8(&r);
    return dec_done(&r, m, sizeof(*m));
}
