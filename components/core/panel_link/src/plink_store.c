/*
 * plink_store.c - see include/panel_link/plink_store.h (docs/features/11 "Pairing"; LINK-014,
 * LINK-017, LINK-022).
 *
 * Layout (little-endian), frozen by test/plink_store_fixture_v1.h:
 *   header   'P' 'L' | kind u8 ('M', 'K') | format u8 (1) | compat u8 (1) | payload_len u16 |
 *            crc32 u32 (alarm_crc32 over the payload)
 *   master   net_id u16 | count u8 | count x (len u8 | entry), then bytes a later format appends
 *   entry    device u8 | home u8 | kind u8 | flags u8 | supervision_s u16 | k_pair[32] |
 *            paired_epoch_ms i64 | name (len u8 + UTF-8), then fields a later format appends
 *   keypad   net_id u16 | device u8 | home u8 | k_pair[32] | master_name (len u8 + UTF-8) |
 *            panel_id (len u8 + chars), then fields a later format appends
 *
 * A reader takes the fields it knows and skips the rest (an entry by its length, the record's
 * tail by the payload's), so later formats only append and keep compat 1; compat goes up only
 * when an older reader would misread the record, and that reader then refuses it.
 */
#include "panel_link/plink_store.h"

#include <string.h>

#include "alarm/alarm_codec.h"

#define HDR_LEN      11u
#define STORE_FORMAT 1u    /* what this firmware writes */
#define STORE_COMPAT 1u    /* the newest compat this firmware reads */
#define KIND_MASTER  0x4Du /* 'M' */
#define KIND_KEYPAD  0x4Bu /* 'K' */

/* A master entry's fields in format 1, up to the name's length byte. */
#define PEER_V1_LEN (1u + 1u + 1u + 1u + 2u + PLINK_KEY_LEN + 8u + 1u)

/* The records hold pairing keys: a copy on the stack is wiped so the compiler can't drop it. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n-- > 0u) {
        *b++ = 0u;
    }
}

/* ---- writing ------------------------------------------------------------------------------- */

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
    if (n > 0u) {
        memcpy(w->buf + w->len, data, n);
        w->len += n;
    }
}

static void put_u8(writer_t *w, uint8_t v)
{
    put(w, &v, 1u);
}

static void put_le(writer_t *w, uint64_t v, unsigned width)
{
    uint8_t b[8];
    for (unsigned i = 0; i < width; i++) {
        b[i] = (uint8_t)(v >> (8u * i));
    }
    put(w, b, width);
}

/* A string: its length byte, then at most max bytes, cut where a UTF-8 character starts. */
static void put_str(writer_t *w, const char *s, size_t max)
{
    size_t n = 0u;
    while (n < max && s[n] != '\0') {
        n++;
    }
    if (s[n] != '\0') {
        while (n > 0u && ((unsigned)(uint8_t)s[n] & 0xC0u) == 0x80u) {
            n--;
        }
    }
    put_u8(w, (uint8_t)n);
    put(w, s, n);
}

static void wr_le(uint8_t *p, uint32_t v, unsigned width)
{
    for (unsigned i = 0; i < width; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static bool begin(writer_t *w, uint8_t *out, size_t cap)
{
    w->buf = out;
    w->cap = cap;
    w->len = HDR_LEN;
    w->overflow = out == NULL || cap < HDR_LEN;
    return !w->overflow;
}

/* The header over what was written; false (and the buffer wiped: it held keys) when it didn't
 * fit. */
static bool finish(writer_t *w, uint8_t kind, size_t *len)
{
    const size_t payload = w->len - HDR_LEN;
    if (w->overflow || payload > UINT16_MAX) {
        if (w->buf != NULL) {
            wipe(w->buf, w->cap);
        }
        return false;
    }
    w->buf[0] = 'P';
    w->buf[1] = 'L';
    w->buf[2] = kind;
    w->buf[3] = STORE_FORMAT;
    w->buf[4] = STORE_COMPAT;
    wr_le(w->buf + 5, (uint32_t)payload, 2u);
    wr_le(w->buf + 7, alarm_crc32(0u, w->buf + HDR_LEN, payload), 4u);
    if (len != NULL) {
        *len = w->len;
    }
    return true;
}

bool plink_store_master_encode(const plink_master_rec_t *rec, uint8_t *out, size_t cap, size_t *len)
{
    writer_t w;
    if (rec == NULL || rec->count > PLINK_KEYPADS_MAX || !begin(&w, out, cap)) {
        return false;
    }
    put_le(&w, rec->net_id, 2u);
    put_u8(&w, rec->count);
    for (unsigned i = 0; i < rec->count; i++) {
        const plink_peer_rec_t *p = &rec->peer[i];
        const size_t at = w.len;
        put_u8(&w, 0u); /* the entry's length, filled in below */
        put_u8(&w, p->device);
        put_u8(&w, p->home);
        put_u8(&w, p->kind);
        put_u8(&w, p->flags);
        put_le(&w, p->supervision_s, 2u);
        put(&w, p->k_pair, sizeof(p->k_pair));
        put_le(&w, (uint64_t)p->paired_epoch_ms, 8u);
        put_str(&w, p->name, PLINK_NAME_MAX);
        if (p->switch_allowed || p->return_never || p->return_s != 0u) {
            put_u8(&w, (uint8_t)((p->switch_allowed ? 1u : 0u) | (p->return_never ? 2u : 0u)));
            put_le(&w, p->return_s, 2u);
        }
        if (!w.overflow) {
            out[at] = (uint8_t)(w.len - at - 1u);
        }
    }
    return finish(&w, KIND_MASTER, len);
}

bool plink_store_keypad_encode(const plink_keypad_rec_t *rec, uint8_t *out, size_t cap, size_t *len)
{
    writer_t w;
    if (rec == NULL || !begin(&w, out, cap)) {
        return false;
    }
    put_le(&w, rec->net_id, 2u);
    put_u8(&w, rec->device);
    put_u8(&w, rec->home);
    put(&w, rec->k_pair, sizeof(rec->k_pair));
    put_str(&w, rec->master_name, PLINK_NAME_MAX);
    put_str(&w, rec->panel_id, PLINK_PANEL_ID_LEN);
    return finish(&w, KIND_KEYPAD, len);
}

/* ---- reading ------------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    bool ok; /* false once a read ran past the end: it then reads zeros */
} reader_t;

static void take(reader_t *r, void *out, size_t n)
{
    if (!r->ok || n > r->len - r->pos) {
        r->ok = false;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->p + r->pos, n);
    r->pos += n;
}

static uint8_t get_u8(reader_t *r)
{
    uint8_t v;
    take(r, &v, 1u);
    return v;
}

static uint64_t get_le(reader_t *r, unsigned width)
{
    uint8_t b[8];
    take(r, b, width);
    uint64_t v = 0u;
    for (unsigned i = 0; i < width; i++) {
        v |= (uint64_t)b[i] << (8u * i);
    }
    return v;
}

/* A string into out (cap bytes, NUL-terminated): one longer than out holds is cut where a UTF-8
 * character starts. ok = false when its bytes run past the end. */
static void get_str(reader_t *r, char *out, size_t cap)
{
    memset(out, 0, cap);
    const uint8_t n = get_u8(r);
    if (!r->ok || n > r->len - r->pos) {
        r->ok = false;
        return;
    }
    const uint8_t *s = r->p + r->pos;
    size_t keep = n;
    if (keep > cap - 1u) {
        keep = cap - 1u;
        while (keep > 0u && ((unsigned)s[keep] & 0xC0u) == 0x80u) {
            keep--;
        }
    }
    memcpy(out, s, keep);
    r->pos += n;
}

/* Checks the header (magic, kind, a compat this firmware reads) and the CRC; r then reads the
 * payload. Any format with compat 1 reads: a later one only appended. */
static bool open_record(const uint8_t *buf, size_t len, uint8_t kind, reader_t *r)
{
    if (buf == NULL || len < HDR_LEN || buf[0] != 'P' || buf[1] != 'L' || buf[2] != kind ||
        buf[4] > STORE_COMPAT) {
        return false;
    }
    const size_t payload = (size_t)buf[5] | ((size_t)buf[6] << 8);
    const uint32_t crc = (uint32_t)buf[7] | ((uint32_t)buf[8] << 8) | ((uint32_t)buf[9] << 16) |
                         ((uint32_t)buf[10] << 24);
    if (payload > len - HDR_LEN || alarm_crc32(0u, buf + HDR_LEN, payload) != crc) {
        return false;
    }
    r->p = buf + HDR_LEN;
    r->len = payload;
    r->pos = 0u;
    r->ok = true;
    return true;
}

/* One master entry of len bytes: false when it is damaged (shorter than format 1's fields, or
 * its name runs past its end) or its number is outside 1..8. */
static bool peer_decode(const uint8_t *p, size_t len, plink_peer_rec_t *peer)
{
    reader_t r = {p, len, 0u, len >= PEER_V1_LEN};
    peer->device = get_u8(&r);
    peer->home = get_u8(&r);
    peer->kind = get_u8(&r);
    peer->flags = get_u8(&r);
    peer->supervision_s = (uint16_t)get_le(&r, 2u);
    take(&r, peer->k_pair, sizeof(peer->k_pair));
    peer->paired_epoch_ms = (int64_t)get_le(&r, 8u);
    get_str(&r, peer->name, sizeof(peer->name));
    if (r.ok && r.len - r.pos >= 3u) {
        const uint8_t policy = get_u8(&r);
        peer->switch_allowed = (policy & 1u) != 0u;
        peer->return_never = (policy & 2u) != 0u;
        peer->return_s = (uint16_t)get_le(&r, 2u);
        if (peer->return_s > 3600u) peer->return_s = 30u;
    }
    return r.ok && peer->device >= PLINK_DEVICE_FIRST && peer->device <= PLINK_DEVICE_LAST;
}

static bool has_device(const plink_master_rec_t *rec, uint8_t device)
{
    for (unsigned i = 0; i < rec->count; i++) {
        if (rec->peer[i].device == device) {
            return true;
        }
    }
    return false;
}

bool plink_store_master_decode(const uint8_t *buf, size_t len, plink_master_rec_t *rec)
{
    if (rec == NULL) {
        return false;
    }
    wipe(rec, sizeof(*rec));
    reader_t r;
    if (!open_record(buf, len, KIND_MASTER, &r)) {
        return false;
    }
    rec->net_id = (uint16_t)get_le(&r, 2u);
    const uint8_t count = get_u8(&r);
    if (!r.ok) {
        wipe(rec, sizeof(*rec));
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        const uint8_t n = get_u8(&r);
        if (!r.ok || n > r.len - r.pos) {
            break; /* the entries end early: the rest can't be found */
        }
        plink_peer_rec_t peer;
        memset(&peer, 0, sizeof(peer));
        /* A damaged entry, a number outside 1..8 or one already taken is dropped; the others
         * still load, so one bad keypad doesn't unpair them all. */
        if (peer_decode(r.p + r.pos, n, &peer) && !has_device(rec, peer.device) &&
            rec->count < PLINK_KEYPADS_MAX) {
            rec->peer[rec->count++] = peer;
        }
        wipe(&peer, sizeof(peer));
        r.pos += n;
    }
    return true;
}

bool plink_store_keypad_decode(const uint8_t *buf, size_t len, plink_keypad_rec_t *rec)
{
    if (rec == NULL) {
        return false;
    }
    wipe(rec, sizeof(*rec));
    reader_t r;
    if (!open_record(buf, len, KIND_KEYPAD, &r)) {
        return false;
    }
    rec->net_id = (uint16_t)get_le(&r, 2u);
    rec->device = get_u8(&r);
    rec->home = get_u8(&r);
    take(&r, rec->k_pair, sizeof(rec->k_pair));
    get_str(&r, rec->master_name, sizeof(rec->master_name));
    get_str(&r, rec->panel_id, sizeof(rec->panel_id));
    /* A keypad record without its fields, a network or a number in 1..8 is no master. */
    if (!r.ok || rec->net_id == 0u || rec->device < PLINK_DEVICE_FIRST ||
        rec->device > PLINK_DEVICE_LAST) {
        wipe(rec, sizeof(*rec));
        return false;
    }
    return true;
}
