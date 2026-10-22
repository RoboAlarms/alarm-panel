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

#include "alarm_proto/proto_link.h"  /* proto_sha256 */
#include "alarm_proto/proto_prefs.h" /* the settings contract the PREFS layouts carry */
#include "panel_link/plink_frame.h"
#include "panel_link/plink_wire.h"

#define CMD_OP_LEN     9u /* CMD's op section in v1, after its length byte */
#define EVENTS_HDR_LEN 3u /* EVENTS: flags, count, rec_len */
#define DEV_ACT_OP_LEN 7u /* DEV_ACT's act section in v1, after its length byte */

/* The longest payloads, field by field from docs/features/11 (a string is its length byte and
 * its bytes, a section its length byte and its fields), and LINK-043: each fits a 250-byte frame
 * (FW_PAGE and DEV_LIST_PAGE fit their peer's frame instead). */
#define STR(max) (1u + (max))
_Static_assert(PLINK_FW_INFO_MAX == 1u + 1u + STR(PLINK_FW_DEVICE_MAX) + STR(PLINK_FW_BOARD_MAX) +
                                        STR(PLINK_FW_CHIP_MAX) + 2u + STR(PLINK_FW_PROJECT_MAX) +
                                        STR(PLINK_FW_VERSION_MAX) + 4u + 2u * PLINK_KEY8_LEN +
                                        STR(PLINK_FW_CP_MAX),
               "FW_INFO's layout");
_Static_assert(PLINK_FW_ORDER_MAX == (1u + 4u + 1u + 1u + STR(PLINK_FW_VERSION_MAX) + 4u +
                                      PLINK_SHA256_LEN + PLINK_FW_KEY_LEN) +
                                         (1u + STR(PLINK_FW_SERVER_MAX) +
                                          STR(PLINK_FW_CHANNEL_MAX)) +
                                         (1u + 2u + 1u + 2u),
               "FW_ORDER's layout");
_Static_assert(PLINK_FW_STATE_MAX == (1u + 4u + 4u * 1u + 4u + 4u + PLINK_ID8_LEN +
                                      STR(PLINK_FW_VERSION_MAX)) +
                                         (1u + 2u + STR(PLINK_FW_VERSION_MAX) + 1u) +
                                         (1u + 2u + 4u + STR(PLINK_FW_VERSION_MAX) +
                                          PLINK_KEY8_LEN + 1u),
               "FW_STATE's layout");
_Static_assert(PLINK_FW_REQ_LEN == 1u + PLINK_ID8_LEN + 4u + 2u + 1u, "FW_REQ's layout");
_Static_assert(PLINK_FW_PAGE_HDR == PLINK_ID8_LEN + 4u + 4u + 2u, "FW_PAGE's layout");
_Static_assert(PLINK_DEV_REC_MAX == 1u + 6u + 4u + 4u + 4u + 1u + STR(PLINK_DEV_WORD_MAX),
               "the record section's layout");
_Static_assert(PLINK_DEV_ITEM_MAX == 1u + PLINK_DEV_REC_MAX, "a DEV_STATE record");
_Static_assert(PLINK_DEV_CAT_MAX ==
                   1u + 2u + STR(PLINK_REF_MAX) + STR(PLINK_NAME_MAX) + STR(PLINK_AREA_MAX),
               "the catalog record section's layout");
_Static_assert(PLINK_DEV_STATE_HEAD == 1u + 4u + 4u + 1u, "DEV_STATE's head section");
_Static_assert(PLINK_DEV_STATE_REQ_LEN == 4u + 1u + 1u, "DEV_STATE_REQ's layout");
_Static_assert(PLINK_DEV_ACT_LEN ==
                   4u + 1u + DEV_ACT_OP_LEN + STR(PLINK_REF_MAX) + STR(ALARM_CODE_MAX_LEN),
               "DEV_ACT's layout");
_Static_assert(PLINK_DEV_ACK_MAX == 4u + 1u + 1u + 1u + 4u + 1u + PLINK_DEV_REC_MAX,
               "DEV_ACK's layout");
/* PREFS and PREFS_SET at their longest carry every field proto_prefs.h knows (its fields body,
 * count first) and, in PREFS, a refusal for each. */
_Static_assert(PLINK_PREFS_MAX == (1u + 4u) + (1u + STR(PLINK_PANEL_ID_LEN) + 1u + 4u) +
                                      (1u + 1u + 2u * PROTO_PREFS_F_LAST) +
                                      (1u + PROTO_PREFS_ENC_MAX),
               "PREFS' layout");
_Static_assert(PLINK_PREFS_SET_MAX == 4u + 1u + (1u + PROTO_PREFS_ENC_MAX), "PREFS_SET's layout");
/* The settings why on the link are proto_prefs.h's numbers. */
_Static_assert((int)PLINK_PREFS_NOT_ALLOWED == (int)PROTO_PREFS_WHY_NOT_ALLOWED &&
                   (int)PLINK_PREFS_RANGE == (int)PROTO_PREFS_WHY_RANGE &&
                   (int)PLINK_PREFS_IN_USE == (int)PROTO_PREFS_WHY_IN_USE &&
                   (int)PLINK_PREFS_HELD == (int)PROTO_PREFS_WHY_HELD &&
                   (int)PLINK_PREFS_VOICE_NOT_READY == (int)PROTO_PREFS_WHY_VOICE_NOT_READY &&
                   (int)PLINK_PREFS_BAD == (int)PROTO_PREFS_WHY_BAD &&
                   (int)PLINK_PREFS_BUSY == (int)PROTO_PREFS_WHY_BUSY &&
                   (unsigned)PLINK_PREFS_BUSY == PROTO_PREFS_WHY_LAST,
               "the settings why are proto_prefs.h's");
_Static_assert(PLINK_FW_INFO_MAX <= PLINK_PAYLOAD_SMALL, "FW_INFO fits a 250-byte frame");
_Static_assert(PLINK_FW_ORDER_MAX <= PLINK_PAYLOAD_SMALL, "FW_ORDER fits a 250-byte frame");
_Static_assert(PLINK_FW_STATE_MAX <= PLINK_PAYLOAD_SMALL, "FW_STATE fits a 250-byte frame");
_Static_assert(PLINK_FW_REQ_LEN <= PLINK_PAYLOAD_SMALL, "FW_REQ fits a 250-byte frame");
_Static_assert(PLINK_DEV_STATE_HEAD + 4u * PLINK_DEV_ITEM_MAX <= PLINK_PAYLOAD_SMALL,
               "DEV_STATE: four records at their longest fit a 250-byte frame");
_Static_assert(PLINK_DEV_STATE_REQ_LEN <= PLINK_PAYLOAD_SMALL, "DEV_STATE_REQ fits");
_Static_assert(PLINK_DEV_ACT_LEN <= PLINK_PAYLOAD_SMALL, "DEV_ACT fits a 250-byte frame");
_Static_assert(PLINK_DEV_ACK_MAX <= PLINK_PAYLOAD_SMALL, "DEV_ACK fits a 250-byte frame");
_Static_assert(PLINK_PREFS_MAX <= PLINK_PAYLOAD_SMALL, "PREFS fits a 250-byte frame");
_Static_assert(PLINK_PREFS_SET_MAX <= PLINK_PAYLOAD_SMALL, "PREFS_SET fits a 250-byte frame");
_Static_assert(8u + STR(PLINK_FW_MAX) + 5u <= PLINK_PAYLOAD_SMALL, "HEARTBEAT with its section");
#undef STR

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
    plink_w_u64(w, m->mono_ms);
    if (m->selected != 0u || m->switch_allowed || m->return_never || m->return_s != 0u) {
        plink_w_u8(w, m->selected);
        if (m->switch_allowed || m->return_never || m->return_s != 0u) {
            plink_w_u8(w, (uint8_t)((m->switch_allowed ? 1u : 0u) | (m->return_never ? 2u : 0u)));
            plink_w_u16(w, m->return_s);
        }
    }
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
    m->mono_ms = plink_r_u64(&s);
    m->selected = plink_r_u8(&s);
    const uint8_t policy = plink_r_u8(&s);
    m->switch_allowed = (policy & 1u) != 0u;
    m->return_never = (policy & 2u) != 0u;
    m->return_s = plink_r_u16(&s);
    if (policy > 3u || m->return_s > 3600u) {
        m->switch_allowed = false;
        m->return_never = false;
        m->return_s = 0u;
    }
    if (m->selected > PLINK_PARTS_MAX) {
        m->selected = 0u;
    }
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
    if (m->has_prefs) {
        const size_t at = plink_w_section(&w);
        plink_w_u32(&w, m->prefs_crc);
        plink_w_section_end(&w, at);
    }
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
    /* Only when bytes remain: a section read from an exhausted reader would refuse v1's
     * heartbeat, which ends at fw. A section too short for prefs_crc carries none (an empty one
     * marks it absent before a later section). */
    if (plink_r_left(&r) > 0u) {
        plink_r_t s = plink_r_section(&r);
        if (plink_r_left(&s) >= 4u) {
            m->prefs_crc = plink_r_u32(&s);
            m->has_prefs = true;
        }
    }
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

/* ---- updates through the link (docs/features/11 "Updates through the link") --------------- */

plink_fw_why_class_t plink_fw_why_class(uint8_t why)
{
    if (why == 0u) {
        return PLINK_FWC_OK;
    }
    if (why < 10u) {
        return PLINK_FWC_NOT_REACHED;
    }
    if (why < 20u) {
        return PLINK_FWC_SERVER;
    }
    if (why < 30u) {
        return PLINK_FWC_TRANSFER;
    }
    if (why < 50u) {
        return PLINK_FWC_REFUSED;
    }
    if (why < 60u) {
        return PLINK_FWC_WAITING;
    }
    if (why < 70u) {
        return PLINK_FWC_STOPPED;
    }
    return PLINK_FWC_UNKNOWN;
}

bool plink_enc_fw_info(const plink_fw_info_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    const size_t at = plink_w_section(&w);
    plink_w_u8(&w, m->can);
    plink_w_str(&w, m->device, PLINK_FW_DEVICE_MAX);
    plink_w_str(&w, m->board_rev, PLINK_FW_BOARD_MAX);
    plink_w_str(&w, m->chip_rev, PLINK_FW_CHIP_MAX);
    plink_w_u16(&w, m->chip_rev_full);
    plink_w_str(&w, m->project, PLINK_FW_PROJECT_MAX);
    plink_w_str(&w, m->version, PLINK_FW_VERSION_MAX);
    plink_w_u32(&w, m->slot);
    plink_w_bytes(&w, m->own_key8, PLINK_KEY8_LEN);
    plink_w_bytes(&w, m->accepted_key8, PLINK_KEY8_LEN);
    plink_w_str(&w, m->cp_version, PLINK_FW_CP_MAX);
    plink_w_section_end(&w, at);
    return enc_done(&w, len);
}

bool plink_dec_fw_info(const uint8_t *p, size_t len, plink_fw_info_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    m->can = plink_r_u8(&s);
    plink_r_str(&s, m->device, sizeof(m->device));
    plink_r_str(&s, m->board_rev, sizeof(m->board_rev));
    plink_r_str(&s, m->chip_rev, sizeof(m->chip_rev));
    m->chip_rev_full = plink_r_u16(&s);
    plink_r_str(&s, m->project, sizeof(m->project));
    plink_r_str(&s, m->version, sizeof(m->version));
    m->slot = plink_r_u32(&s);
    plink_r_bytes(&s, m->own_key8, PLINK_KEY8_LEN);
    plink_r_bytes(&s, m->accepted_key8, PLINK_KEY8_LEN);
    plink_r_str(&s, m->cp_version, sizeof(m->cp_version));
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_fw_order(const plink_fw_order_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || m->order == 0u) {
        w.ok = false;
        return enc_done(&w, len);
    }
    size_t at = plink_w_section(&w);
    plink_w_u32(&w, m->order);
    plink_w_u8(&w, m->op);
    plink_w_u8(&w, m->kind);
    plink_w_str(&w, m->version, PLINK_FW_VERSION_MAX);
    plink_w_u32(&w, m->size);
    plink_w_bytes(&w, m->sha256, PLINK_SHA256_LEN);
    plink_w_bytes(&w, m->key, PLINK_FW_KEY_LEN);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_str(&w, m->server, PLINK_FW_SERVER_MAX);
    plink_w_str(&w, m->channel, PLINK_FW_CHANNEL_MAX);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_u16(&w, m->page);
    plink_w_u8(&w, m->count);
    plink_w_u16(&w, m->gap_ms);
    plink_w_section_end(&w, at);
    return enc_done(&w, len);
}

bool plink_dec_fw_order(const uint8_t *p, size_t len, plink_fw_order_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    m->order = plink_r_u32(&s);
    m->op = plink_r_u8(&s); /* an unknown op or kind is the keypad's to refuse */
    m->kind = plink_r_u8(&s);
    plink_r_str(&s, m->version, sizeof(m->version));
    m->size = plink_r_u32(&s);
    plink_r_bytes(&s, m->sha256, PLINK_SHA256_LEN);
    plink_r_bytes(&s, m->key, PLINK_FW_KEY_LEN);
    s = plink_r_section(&r);
    plink_r_str(&s, m->server, sizeof(m->server));
    plink_r_str(&s, m->channel, sizeof(m->channel));
    s = plink_r_section(&r);
    m->page = plink_r_u16(&s);
    m->count = plink_r_u8(&s);
    m->gap_ms = plink_r_u16(&s);
    if (r.ok && m->order == 0u) {
        r.ok = false; /* no order is 0 */
    }
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_fw_state(const plink_fw_state_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    size_t at = plink_w_section(&w);
    plink_w_u32(&w, m->order);
    plink_w_u8(&w, m->op);
    plink_w_u8(&w, m->state);
    plink_w_u8(&w, m->flags);
    plink_w_u8(&w, m->why);
    plink_w_u32(&w, m->bytes);
    plink_w_u32(&w, m->size);
    plink_w_bytes(&w, m->id8, PLINK_ID8_LEN);
    plink_w_str(&w, m->target, PLINK_FW_VERSION_MAX);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_u8(&w, m->probation);
    plink_w_u8(&w, m->last);
    plink_w_str(&w, m->last_version, PLINK_FW_VERSION_MAX);
    plink_w_u8(&w, m->failed_mask);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_u8(&w, m->asked);
    plink_w_u8(&w, m->probe_why);
    plink_w_u32(&w, m->age_s);
    plink_w_str(&w, m->offered, PLINK_FW_VERSION_MAX);
    plink_w_bytes(&w, m->offered_key8, PLINK_KEY8_LEN);
    plink_w_u8(&w, m->key_trusted);
    plink_w_section_end(&w, at);
    return enc_done(&w, len);
}

bool plink_dec_fw_state(const uint8_t *p, size_t len, plink_fw_state_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    m->order = plink_r_u32(&s);
    m->op = plink_r_u8(&s);
    m->state = plink_r_u8(&s);
    m->flags = plink_r_u8(&s);
    m->why = plink_r_u8(&s);
    m->bytes = plink_r_u32(&s);
    m->size = plink_r_u32(&s);
    plink_r_bytes(&s, m->id8, PLINK_ID8_LEN);
    plink_r_str(&s, m->target, sizeof(m->target));
    s = plink_r_section(&r);
    m->probation = plink_r_u8(&s);
    m->last = plink_r_u8(&s);
    plink_r_str(&s, m->last_version, sizeof(m->last_version));
    m->failed_mask = plink_r_u8(&s);
    s = plink_r_section(&r);
    m->asked = plink_r_u8(&s);
    m->probe_why = plink_r_u8(&s);
    m->age_s = plink_r_u32(&s);
    plink_r_str(&s, m->offered, sizeof(m->offered));
    plink_r_bytes(&s, m->offered_key8, PLINK_KEY8_LEN);
    m->key_trusted = plink_r_u8(&s);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_fw_req(const plink_fw_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    const size_t at = plink_w_section(&w);
    plink_w_bytes(&w, m->id8, PLINK_ID8_LEN);
    plink_w_u32(&w, m->offset);
    plink_w_u16(&w, m->page);
    plink_w_u8(&w, m->count);
    plink_w_section_end(&w, at);
    return enc_done(&w, len);
}

bool plink_dec_fw_req(const uint8_t *p, size_t len, plink_fw_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    plink_r_bytes(&s, m->id8, PLINK_ID8_LEN);
    m->offset = plink_r_u32(&s);
    m->page = plink_r_u16(&s);
    m->count = plink_r_u8(&s);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_fw_page(const plink_fw_page_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || (m->data == NULL && m->len > 0u) ||
        (uint64_t)m->offset + m->len > m->total) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_bytes(&w, m->id8, PLINK_ID8_LEN);
    plink_w_u32(&w, m->total);
    plink_w_u32(&w, m->offset);
    plink_w_u16(&w, m->len);
    plink_w_bytes(&w, m->data, m->len);
    return enc_done(&w, len);
}

bool plink_dec_fw_page(const uint8_t *p, size_t len, plink_fw_page_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_bytes(&r, m->id8, PLINK_ID8_LEN);
    m->total = plink_r_u32(&r);
    m->offset = plink_r_u32(&r);
    m->len = plink_r_u16(&r);
    m->data = plink_r_take(&r, m->len);
    if (r.ok && (uint64_t)m->offset + m->len > m->total) {
        r.ok = false; /* a page outside the image */
    }
    return dec_done(&r, m, sizeof(*m));
}

/* ---- Smart Home on keypads (docs/features/11 "Smart Home and Home Assistant on keypads") --- */

static int32_t to_i32(uint32_t v)
{
    /* Two's complement back without an implementation-defined conversion. */
    return v <= (uint32_t)INT32_MAX ? (int32_t)v : -(int32_t)(~v) - 1;
}

/* Whether plink_clean_text keeps all n bytes at p: valid UTF-8, no control, DEL or bidi control
 * (a NUL included). */
static bool bytes_clean(const uint8_t *p, size_t n)
{
    char kept[PLINK_REF_MAX + 1];
    if (n > PLINK_REF_MAX) {
        return false;
    }
    plink_clean_text((const char *)p, n, kept, sizeof(kept));
    return strlen(kept) == n;
}

/* A ref's length when it ends within PLINK_REF_MAX bytes and is clean; -1 otherwise: cut or
 * cleaned, a ref could name another device. */
static int ref_len(const char *ref)
{
    size_t n = 0;
    while (n <= PLINK_REF_MAX && ref[n] != '\0') {
        n++;
    }
    return n <= PLINK_REF_MAX && bytes_clean((const uint8_t *)ref, n) ? (int)n : -1;
}

/* A section taken whole, its length byte first, even from a section's reader (which would read
 * a section cut by its own end as zeros): *n its bytes. NULL and r not ok when it isn't whole. */
static const uint8_t *r_whole_section(plink_r_t *r, size_t *n)
{
    *n = 0u;
    if (plink_r_left(r) == 0u) {
        r->ok = false;
        return NULL;
    }
    const size_t body = r->p[r->pos];
    const uint8_t *at = plink_r_take(r, 1u + body);
    if (at != NULL) {
        *n = 1u + body;
    }
    return at;
}

/* The record section (docs/features/11 "Record section"). */
static void w_dev_rec(plink_w_t *w, const plink_dev_rec_t *m)
{
    const size_t at = plink_w_section(w);
    plink_w_u8(w, m->flags);
    plink_w_u8(w, m->action);
    plink_w_u8(w, m->vflags);
    plink_w_u8(w, m->level);
    plink_w_u8(w, m->modes);
    plink_w_u8(w, m->hvac_action);
    plink_w_u32(w, (uint32_t)m->setpoint);
    plink_w_u32(w, (uint32_t)m->temp);
    plink_w_u32(w, m->since);
    plink_w_u8(w, m->caps);
    plink_w_str(w, m->word, PLINK_DEV_WORD_MAX);
    plink_w_section_end(w, at);
}

static void r_dev_rec(plink_r_t *s, plink_dev_rec_t *m)
{
    m->flags = plink_r_u8(s);
    m->action = plink_r_u8(s); /* a value this firmware doesn't know stays as is */
    m->vflags = plink_r_u8(s);
    m->level = plink_r_u8(s);
    m->modes = plink_r_u8(s);
    m->hvac_action = plink_r_u8(s);
    m->setpoint = to_i32(plink_r_u32(s));
    m->temp = to_i32(plink_r_u32(s));
    m->since = plink_r_u32(s);
    m->caps = plink_r_u8(s);
    plink_r_str(s, m->word, sizeof(m->word));
}

bool plink_enc_dev_rec(const plink_dev_rec_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    w_dev_rec(&w, m);
    return enc_done(&w, len);
}

bool plink_dec_dev_rec(const uint8_t *p, size_t len, plink_dev_rec_t *m, size_t *used)
{
    if (used != NULL) {
        *used = 0u;
    }
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    if (!r.ok) {
        return false;
    }
    r_dev_rec(&s, m);
    if (used != NULL) {
        *used = r.pos;
    }
    return true;
}

bool plink_enc_dev_item(uint8_t index, const plink_dev_rec_t *m, uint8_t *out, size_t cap,
                        size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u8(&w, index);
    w_dev_rec(&w, m);
    return enc_done(&w, len);
}

/* A ref as a catalog carries it (a plain string), checked as ref_len checks one. A ref cut by
 * its section's end is not clean either. */
static bool r_ref_str(plink_r_t *r, char out[PLINK_REF_MAX + 1])
{
    out[0] = '\0';
    const size_t n = plink_r_u8(r);
    const size_t left = plink_r_left(r);
    if (n > left && !r->pad) {
        r->ok = false;
    }
    const uint8_t *raw = plink_r_take(r, n < left ? n : left);
    if (!r->ok || n > left || !bytes_clean(raw, n)) {
        return false;
    }
    if (n > 0u) {
        memcpy(out, raw, n);
    }
    out[n] = '\0';
    return true;
}

bool plink_enc_dev_cat(const plink_dev_cat_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || ref_len(m->ref) < 0) {
        w.ok = false;
        return enc_done(&w, len);
    }
    const size_t at = plink_w_section(&w);
    plink_w_u8(&w, m->kind);
    plink_w_u8(&w, m->source);
    plink_w_str(&w, m->ref, PLINK_REF_MAX);
    plink_w_str(&w, m->name, PLINK_NAME_MAX);
    plink_w_str(&w, m->area, PLINK_AREA_MAX);
    plink_w_section_end(&w, at);
    return enc_done(&w, len);
}

bool plink_dec_dev_cat(const uint8_t *p, size_t len, plink_dev_cat_t *m, size_t *used)
{
    if (used != NULL) {
        *used = 0u;
    }
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    if (!r.ok) {
        return false;
    }
    if (used != NULL) {
        *used = r.pos; /* whole: a list can step over a device it can't use */
    }
    m->kind = plink_r_u8(&s); /* a kind or a source this firmware doesn't know stays as is */
    m->source = plink_r_u8(&s);
    const bool ref_ok = r_ref_str(&s, m->ref);
    plink_r_str(&s, m->name, sizeof(m->name));
    plink_r_str(&s, m->area, sizeof(m->area));
    if (!ref_ok) {
        memset(m, 0, sizeof(*m));
        return false;
    }
    return true;
}

bool plink_enc_dev_list_req(const plink_dev_list_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    return plink_enc_cfg_req(m, out, cap, len);
}

bool plink_dec_dev_list_req(const uint8_t *p, size_t len, plink_dev_list_req_t *m)
{
    return plink_dec_cfg_req(p, len, m);
}

bool plink_enc_dev_list_page(const plink_dev_list_page_t *m, uint8_t *out, size_t cap,
                             size_t *len)
{
    return plink_enc_cfg_page(m, out, cap, len);
}

bool plink_dec_dev_list_page(const uint8_t *p, size_t len, plink_dev_list_page_t *m)
{
    return plink_dec_cfg_page(p, len, m);
}

/* count records (index u8, record section) from r; false (r not ok) when one isn't whole. */
static bool r_dev_items(plink_r_t *r, uint8_t count)
{
    for (unsigned i = 0; i < count && r->ok; i++) {
        size_t n = 0u;
        (void)plink_r_u8(r);
        (void)r_whole_section(r, &n);
    }
    return r->ok;
}

bool plink_enc_dev_state(const plink_dev_state_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    bool items_ok = m != NULL && (m->items != NULL || m->items_len == 0u);
    if (items_ok) {
        /* Exactly count whole records: nothing goes that a keypad would refuse. */
        plink_r_t r;
        plink_r_init(&r, m->items, m->items_len);
        items_ok = r_dev_items(&r, m->count) && plink_r_left(&r) == 0u;
    }
    if (!items_ok) {
        w.ok = false;
        return enc_done(&w, len);
    }
    const size_t at = plink_w_section(&w);
    plink_w_u32(&w, m->list_crc);
    plink_w_u32(&w, m->states_crc);
    plink_w_u8(&w, m->count);
    plink_w_section_end(&w, at);
    plink_w_bytes(&w, m->items, m->items_len);
    return enc_done(&w, len);
}

bool plink_dec_dev_state(const uint8_t *p, size_t len, plink_dev_state_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    m->list_crc = plink_r_u32(&s);
    m->states_crc = plink_r_u32(&s);
    m->count = plink_r_u8(&s);
    const size_t start = r.pos;
    if (r_dev_items(&r, m->count)) {
        m->items = p + start; /* bytes after the count records are a newer master's: skipped */
        m->items_len = r.pos - start;
    }
    return dec_done(&r, m, sizeof(*m));
}

bool plink_dev_state_next(const plink_dev_state_t *m, size_t *pos, uint8_t *index,
                          plink_dev_rec_t *rec, const uint8_t **item, size_t *item_len)
{
    if (m == NULL || pos == NULL || m->items == NULL || *pos >= m->items_len) {
        return false;
    }
    plink_r_t r;
    plink_r_init(&r, m->items + *pos, m->items_len - *pos);
    const uint8_t idx = plink_r_u8(&r);
    size_t n = 0u;
    const uint8_t *sec = r_whole_section(&r, &n);
    if (!r.ok) {
        return false;
    }
    if (index != NULL) {
        *index = idx;
    }
    if (rec != NULL) {
        (void)plink_dec_dev_rec(sec, n, rec, NULL);
    }
    if (item != NULL) {
        *item = m->items + *pos;
    }
    if (item_len != NULL) {
        *item_len = 1u + n;
    }
    *pos += 1u + n;
    return true;
}

bool plink_enc_dev_state_req(const plink_dev_state_req_t *m, uint8_t *out, size_t cap,
                             size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->list_crc);
    plink_w_u8(&w, m->first);
    plink_w_u8(&w, m->count);
    return enc_done(&w, len);
}

bool plink_dec_dev_state_req(const uint8_t *p, size_t len, plink_dev_state_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->list_crc = plink_r_u32(&r);
    m->first = plink_r_u8(&r);
    m->count = plink_r_u8(&r);
    return dec_done(&r, m, sizeof(*m));
}

/* The act section: what the action is, never the code. */
static void w_dev_act_op(plink_w_t *w, const plink_dev_act_t *m)
{
    const size_t at = plink_w_section(w);
    plink_w_u8(w, m->action);
    plink_w_u32(w, (uint32_t)m->value);
    plink_w_u8(w, m->mode);
    plink_w_u8(w, m->flags);
    plink_w_section_end(w, at);
}

/* A ref in DEV_ACT: its length, then its bytes zero-padded to PLINK_REF_MAX, so every DEV_ACT
 * has the same size whatever the device (LINK-047). */
static void w_ref_padded(plink_w_t *w, const char *ref, int n)
{
    plink_w_u8(w, (uint8_t)n);
    plink_w_bytes(w, ref, (size_t)n);
    for (int i = n; i < (int)PLINK_REF_MAX; i++) {
        plink_w_u8(w, 0);
    }
}

/* A padded ref from the wire into out: false when it is longer than PLINK_REF_MAX or not clean.
 * The padding is not read. */
static bool r_ref_padded(plink_r_t *r, char out[PLINK_REF_MAX + 1])
{
    const size_t n = plink_r_u8(r);
    const uint8_t *raw = plink_r_take(r, PLINK_REF_MAX);
    if (!r->ok || n > PLINK_REF_MAX || !bytes_clean(raw, n)) {
        return false;
    }
    if (n > 0u) {
        memcpy(out, raw, n);
    }
    out[n] = '\0';
    return true;
}

bool plink_enc_dev_act(const plink_dev_act_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    const int ref_n = m != NULL ? ref_len(m->ref) : -1;
    const int code_n = m != NULL ? code_len(m->code) : -1;
    if (ref_n < 0 || code_n < 0) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->req);
    w_dev_act_op(&w, m);
    w_ref_padded(&w, m->ref, ref_n);
    w_code(&w, m->code, code_n);
    return enc_done(&w, len);
}

bool plink_dec_dev_act(const uint8_t *p, size_t len, plink_dev_act_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    plink_r_t s = plink_r_section(&r);
    m->action = plink_r_u8(&s); /* the master judges the action, the mode and the flags */
    m->value = to_i32(plink_r_u32(&s));
    m->mode = plink_r_u8(&s);
    m->flags = plink_r_u8(&s);
    if (!r_ref_padded(&r, m->ref) || !r_code(&r, m->code)) {
        r.ok = false;
    }
    return dec_done(&r, m, sizeof(*m)); /* a refused action leaves no code behind */
}

void plink_dev_act_digest(const plink_dev_act_t *m, uint8_t out[PLINK_SHA256_LEN])
{
    uint8_t buf[1u + DEV_ACT_OP_LEN + 1u + PLINK_REF_MAX]; /* never the code */
    plink_w_t w;
    plink_w_init(&w, buf, sizeof(buf));
    if (m != NULL) {
        const int ref_n = ref_len(m->ref);
        if (ref_n >= 0) {
            w_dev_act_op(&w, m);
            w_ref_padded(&w, m->ref, ref_n);
        } else {
            w.ok = false;
        }
    }
    proto_sha256(buf, w.ok ? w.len : 0u, out);
}

bool plink_enc_dev_ack(const plink_dev_ack_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->req);
    plink_w_u8(&w, m->result);
    plink_w_u8(&w, m->why);
    if (m->has_rec) {
        const size_t at = plink_w_section(&w);
        plink_w_u32(&w, m->list_crc);
        plink_w_u8(&w, m->index);
        w_dev_rec(&w, &m->rec);
        plink_w_section_end(&w, at);
    }
    return enc_done(&w, len);
}

bool plink_dec_dev_ack(const uint8_t *p, size_t len, plink_dev_ack_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->result = plink_r_u8(&r); /* an unknown result is the keypad's to read as REFUSED */
    m->why = plink_r_u8(&r);
    if (plink_r_left(&r) > 0u) {
        plink_r_t s = plink_r_section(&r);
        if (plink_r_left(&s) > 0u) { /* empty: no record, whatever follows */
            m->list_crc = plink_r_u32(&s);
            m->index = plink_r_u8(&s);
            size_t n = 0u;
            const uint8_t *raw = r_whole_section(&s, &n);
            if (!s.ok) {
                r.ok = false; /* the section is there, but not its whole record */
            } else {
                m->has_rec = true;
                m->rec_raw = raw;
                m->rec_raw_len = n;
                (void)plink_dec_dev_rec(raw, n, &m->rec, NULL);
            }
        }
    }
    return dec_done(&r, m, sizeof(*m));
}

/* ---- a keypad's settings ------------------------------------------------------------------ */

/* Whether a fields body (len bytes) is framed as proto_prefs_decode reads it: empty, or its count
 * and that many TLVs (id u8, len u8, len bytes) within it; bytes after them are a later
 * version's. */
static bool fields_framed(const uint8_t *body, size_t len)
{
    if (len == 0u) {
        return true;
    }
    if (body == NULL) {
        return false;
    }
    plink_r_t r;
    plink_r_init(&r, body + 1, len - 1u);
    for (unsigned i = 0; i < body[0] && r.ok; i++) {
        const uint8_t *h = plink_r_take(&r, 2u);
        if (h != NULL) {
            (void)plink_r_take(&r, (size_t)h[1]);
        }
    }
    return r.ok;
}

/* A fields section: its body as given. */
static void w_fields(plink_w_t *w, const uint8_t *body, size_t len)
{
    const size_t at = plink_w_section(w);
    plink_w_bytes(w, body, len);
    plink_w_section_end(w, at);
}

/* A fields section from r: its whole body, as sent (the TLVs and whatever a later version
 * appended after them, all in prefs_crc). r not ok when the section is missing or a TLV runs
 * past it. */
static void r_fields(plink_r_t *r, uint8_t *count, const uint8_t **body, size_t *len)
{
    const plink_r_t s = plink_r_section(r);
    if (!s.ok || !fields_framed(s.p, s.len)) {
        r->ok = false;
        return;
    }
    *body = s.p; /* NULL for an empty section */
    *len = s.len;
    *count = s.len > 0u ? s.p[0] : 0u;
}

bool plink_enc_prefs(const plink_prefs_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || (m->refused == NULL && m->refused_count > 0u) ||
        !fields_framed(m->fields, m->fields_len)) {
        w.ok = false;
        return enc_done(&w, len);
    }
    size_t at = plink_w_section(&w);
    plink_w_u32(&w, m->req);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_str(&w, m->panel_id, PLINK_PANEL_ID_LEN);
    plink_w_u8(&w, m->voice);
    plink_w_u32(&w, m->accepts);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_u8(&w, m->refused_count);
    plink_w_bytes(&w, m->refused, 2u * (size_t)m->refused_count);
    plink_w_section_end(&w, at);
    w_fields(&w, m->fields, m->fields_len);
    return enc_done(&w, len);
}

bool plink_dec_prefs(const uint8_t *p, size_t len, plink_prefs_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t s = plink_r_section(&r);
    m->req = plink_r_u32(&s);
    s = plink_r_section(&r);
    plink_r_str(&s, m->panel_id, sizeof(m->panel_id));
    m->voice = plink_r_u8(&s);
    m->accepts = plink_r_u32(&s);
    s = plink_r_section(&r);
    m->refused_count = plink_r_u8(&s);
    m->refused = plink_r_take(&s, 2u * (size_t)m->refused_count); /* never padded */
    if (!s.ok) {
        r.ok = false;
    }
    r_fields(&r, &m->field_count, &m->fields, &m->fields_len);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_prefs_req(const plink_prefs_req_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->have_crc);
    return enc_done(&w, len);
}

bool plink_dec_prefs_req(const uint8_t *p, size_t len, plink_prefs_req_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->have_crc = plink_r_u32(&r);
    return dec_done(&r, m, sizeof(*m));
}

bool plink_enc_prefs_set(const plink_prefs_set_t *m, uint8_t *out, size_t cap, size_t *len)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    if (m == NULL || m->req == 0u || !fields_framed(m->fields, m->fields_len)) {
        w.ok = false;
        return enc_done(&w, len);
    }
    plink_w_u32(&w, m->req);
    plink_w_u8(&w, m->origin);
    w_fields(&w, m->fields, m->fields_len);
    return enc_done(&w, len);
}

bool plink_dec_prefs_set(const uint8_t *p, size_t len, plink_prefs_set_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->origin = plink_r_u8(&r);
    r_fields(&r, &m->field_count, &m->fields, &m->fields_len);
    if (r.ok && m->req == 0u) {
        r.ok = false; /* no request is 0 */
    }
    return dec_done(&r, m, sizeof(*m));
}

/* LINK-055: a keypad's wired terminals. */
bool plink_enc_io_state(const plink_io_state_t *m, uint8_t *out, size_t cap, size_t *len)
{
    if (m == NULL || out == NULL || len == NULL || m->count > PLINK_IO_MAX) {
        return false;
    }
    const size_t need = 2u + (size_t)m->count * PLINK_IO_REC_LEN;
    if (cap < need) {
        return false;
    }
    out[0] = m->count;
    out[1] = (uint8_t)PLINK_IO_REC_LEN;
    for (size_t i = 0; i < m->count; i++) {
        uint8_t *r = out + 2u + i * PLINK_IO_REC_LEN;
        r[0] = m->rec[i].gpio;
        r[1] = m->rec[i].flags;
        r[2] = (uint8_t)(m->rec[i].changes & 0xFFu);
        r[3] = (uint8_t)(m->rec[i].changes >> 8);
    }
    *len = need;
    return true;
}

bool plink_dec_io_state(const uint8_t *p, size_t len, plink_io_state_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    if (p == NULL || len < 2u) {
        return false;
    }
    const size_t count = p[0], rec_len = p[1];
    if (count > PLINK_IO_MAX || rec_len < PLINK_IO_REC_LEN || len < 2u + count * rec_len) {
        return false;
    }
    m->count = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *r = p + 2u + i * rec_len;
        m->rec[i].gpio = r[0];
        m->rec[i].flags = r[1];
        m->rec[i].changes = (uint16_t)((unsigned)r[2] | ((unsigned)r[3] << 8));
    }
    return true;
}

/* LINK-056: an automation's arming warnings. */
bool plink_enc_auto_warn(const plink_auto_warn_t *m, uint8_t *out, size_t cap, size_t *len)
{
    if (m == NULL || out == NULL || len == NULL || m->count > PLINK_AUTO_MAX) {
        return false;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    plink_w_u8(&w, m->count);
    plink_w_u8(&w, (uint8_t)PLINK_AUTO_REC_LEN);
    for (size_t i = 0; i < m->count; i++) {
        const plink_auto_rec_t *r = &m->rec[i];
        plink_w_u16(&w, r->uid);
        plink_w_u8(&w, r->partition);
        plink_w_u8(&w, r->level);
        plink_w_u8(&w, r->state);
        plink_w_u8(&w, r->warn_min);
        plink_w_u8(&w, r->bypass);
        plink_w_u8(&w, r->put_off_by);
        plink_w_u32(&w, (uint32_t)((uint64_t)r->arm_at_epoch_ms & 0xFFFFFFFFu));
        plink_w_u32(&w, (uint32_t)((uint64_t)r->arm_at_epoch_ms >> 32));
        plink_w_u32(&w, r->arm_in_ms);
        plink_w_u32(&w, (uint32_t)r->until_day);
        plink_w_u16(&w, r->until_minute);
    }
    return enc_done(&w, len);
}

bool plink_dec_auto_warn(const uint8_t *p, size_t len, plink_auto_warn_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    if (p == NULL || len < 2u) {
        return false;
    }
    const size_t count = p[0], rec_len = p[1];
    if (count > PLINK_AUTO_MAX || rec_len < PLINK_AUTO_REC_LEN || len < 2u + count * rec_len) {
        return false;
    }
    m->count = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        plink_r_t r;
        plink_r_init(&r, p + 2u + i * rec_len, PLINK_AUTO_REC_LEN);
        plink_auto_rec_t *a = &m->rec[i];
        a->uid = plink_r_u16(&r);
        a->partition = plink_r_u8(&r);
        a->level = plink_r_u8(&r);
        a->state = plink_r_u8(&r);
        a->warn_min = plink_r_u8(&r);
        a->bypass = plink_r_u8(&r);
        a->put_off_by = plink_r_u8(&r);
        const uint32_t lo = plink_r_u32(&r), hi = plink_r_u32(&r);
        a->arm_at_epoch_ms = (int64_t)(((uint64_t)hi << 32) | lo);
        a->arm_in_ms = plink_r_u32(&r);
        a->until_day = (int32_t)plink_r_u32(&r);
        a->until_minute = plink_r_u16(&r);
        if (!r.ok) {
            memset(m, 0, sizeof(*m));
            return false;
        }
    }
    return true;
}

/* LINK-058: the remote backend's four messages. Data is copied in by the encoders and pointed
 * at by the decoders. */
bool plink_enc_rb_call(const plink_rb_call_t *m, uint8_t *out, size_t cap, size_t *len)
{
    if (m == NULL || out == NULL || len == NULL || m->req == 0u || (m->len > 0u && m->data == NULL) ||
        (uint64_t)m->offset + m->len > m->total) {
        return false;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    plink_w_u32(&w, m->req);
    plink_w_u16(&w, m->call);
    plink_w_u32(&w, m->offset);
    plink_w_u32(&w, m->total);
    plink_w_u16(&w, m->len);
    plink_w_bytes(&w, m->data, m->len);
    return enc_done(&w, len);
}

bool plink_dec_rb_call(const uint8_t *p, size_t len, plink_rb_call_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->call = plink_r_u16(&r);
    m->offset = plink_r_u32(&r);
    m->total = plink_r_u32(&r);
    m->len = plink_r_u16(&r);
    m->data = plink_r_take(&r, m->len);
    if (!r.ok || m->req == 0u || (uint64_t)m->offset + m->len > m->total) {
        memset(m, 0, sizeof(*m));
        return false;
    }
    return true;
}

bool plink_enc_rb_answer(const plink_rb_answer_t *m, uint8_t *out, size_t cap, size_t *len)
{
    if (m == NULL || out == NULL || len == NULL || (m->len > 0u && m->data == NULL) ||
        (uint64_t)m->offset + m->len > m->total) {
        return false;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    plink_w_u32(&w, m->req);
    plink_w_u8(&w, m->status);
    plink_w_u32(&w, m->total);
    plink_w_u32(&w, m->offset);
    plink_w_u16(&w, m->len);
    plink_w_bytes(&w, m->data, m->len);
    return enc_done(&w, len);
}

bool plink_dec_rb_answer(const uint8_t *p, size_t len, plink_rb_answer_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->status = plink_r_u8(&r);
    m->total = plink_r_u32(&r);
    m->offset = plink_r_u32(&r);
    m->len = plink_r_u16(&r);
    m->data = plink_r_take(&r, m->len);
    if (!r.ok || (uint64_t)m->offset + m->len > m->total) {
        memset(m, 0, sizeof(*m));
        return false;
    }
    return true;
}

bool plink_enc_rb_more(const plink_rb_more_t *m, uint8_t *out, size_t cap, size_t *len)
{
    if (m == NULL || out == NULL || len == NULL) {
        return false;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    plink_w_u32(&w, m->req);
    plink_w_u32(&w, m->offset);
    plink_w_u16(&w, m->max);
    return enc_done(&w, len);
}

bool plink_dec_rb_more(const uint8_t *p, size_t len, plink_rb_more_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->req = plink_r_u32(&r);
    m->offset = plink_r_u32(&r);
    m->max = plink_r_u16(&r);
    return r.ok;
}

bool plink_enc_rb_grant(const plink_rb_grant_t *m, uint8_t *out, size_t cap, size_t *len)
{
    if (m == NULL || out == NULL || len == NULL) {
        return false;
    }
    plink_w_t w;
    plink_w_init(&w, out, cap);
    size_t at = plink_w_section(&w);
    plink_w_u8(&w, m->user);
    plink_w_u8(&w, m->authority);
    plink_w_u8(&w, m->partitions);
    plink_w_u8(&w, m->installer ? 1u : 0u);
    plink_w_u16(&w, m->idle_left_s);
    plink_w_section_end(&w, at);
    at = plink_w_section(&w);
    plink_w_u8(&w, m->holder_kind);
    plink_w_u16(&w, m->holder_id);
    plink_w_section_end(&w, at);
    plink_w_u32(&w, m->areas);
    return enc_done(&w, len);
}

bool plink_dec_rb_grant(const uint8_t *p, size_t len, plink_rb_grant_t *m)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    plink_r_t g = plink_r_section(&r);
    m->user = plink_r_u8(&g);
    m->authority = plink_r_u8(&g);
    m->partitions = plink_r_u8(&g);
    m->installer = plink_r_u8(&g) != 0u;
    m->idle_left_s = plink_r_u16(&g);
    plink_r_t h = plink_r_section(&r);
    m->holder_kind = plink_r_u8(&h);
    m->holder_id = plink_r_u16(&h);
    m->areas = plink_r_u32(&r);
    if (!r.ok || !g.ok || !h.ok) {
        memset(m, 0, sizeof(*m));
        return false;
    }
    return true;
}

/* LINK-053: bounded secrets, never accepted without the outstanding request. */
#define WIFI_SSID_AT PLINK_WIFI_REQ_LEN                         /* 4 */
#define WIFI_PASS_AT (PLINK_WIFI_REQ_LEN + PLINK_WIFI_SSID_LEN) /* 37 */
bool plink_enc_wifi(const plink_wifi_t *wifi, uint32_t req, uint8_t *out, size_t cap)
{
    if (!wifi || !out || cap < PLINK_WIFI_LEN || !req ||
        !memchr(wifi->ssid, 0, PLINK_WIFI_SSID_LEN) ||
        !memchr(wifi->password, 0, PLINK_WIFI_PASS_LEN)) return false;
    memset(out, 0, PLINK_WIFI_LEN);
    for (unsigned i = 0; i < PLINK_WIFI_REQ_LEN; ++i) out[i] = (uint8_t)(req >> (8u * i));
    memcpy(out + WIFI_SSID_AT, wifi->ssid, strlen(wifi->ssid));
    if (wifi->ssid[0]) memcpy(out + WIFI_PASS_AT, wifi->password, strlen(wifi->password));
    return true;
}
bool plink_dec_wifi(const uint8_t *p, size_t n, uint32_t req, plink_wifi_t *out)
{
    if (!out) return false;
    plink_wipe(out, sizeof *out);
    if (!p || n < PLINK_WIFI_LEN || !req || !memchr(p + WIFI_SSID_AT, 0, PLINK_WIFI_SSID_LEN) ||
        !memchr(p + WIFI_PASS_AT, 0, PLINK_WIFI_PASS_LEN)) return false;
    uint32_t id = 0;
    for (unsigned i = 0; i < PLINK_WIFI_REQ_LEN; ++i) id |= (uint32_t)p[i] << (8u * i);
    if (id != req) return false;
    memcpy(out->ssid, p + WIFI_SSID_AT, PLINK_WIFI_SSID_LEN);
    if (out->ssid[0]) memcpy(out->password, p + WIFI_PASS_AT, PLINK_WIFI_PASS_LEN);
    return true;
}
