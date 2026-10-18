/*
 * plink_frame.c - the panel link's v1 frame (see include/panel_link/plink_frame.h;
 * docs/features/11 "Frame format (v1)"; LINK-003, LINK-005, LINK-017): the 17-byte header and its
 * checks, the CCM nonce, plain and sealed frames, the RFC 4303 replay window, peer addresses.
 */
#include "panel_link/plink_frame.h"

#include <string.h>

#define WINDOW_BITS 64u /* plink_window_t.bits */

/* Header offsets (docs/features/11 "Frame format (v1)"). */
#define OFF_MAGIC   0u
#define OFF_VERSION 2u
#define OFF_TYPE    3u
#define OFF_FLAGS   4u
#define OFF_NET_ID  5u
#define OFF_SENDER  7u
#define OFF_SID     9u
#define OFF_SEQ     13u

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- message types ------------------------------------------------------------------------ */

bool plink_type_known(uint8_t type)
{
    switch (type) {
    case PLINK_T_PAIR_REQ:
    case PLINK_T_PAIR_COMMIT:
    case PLINK_T_PAIR_NONCE:
    case PLINK_T_PAIR_REVEAL:
    case PLINK_T_PAIR_CONFIRM:
    case PLINK_T_PAIR_DONE:
    case PLINK_T_PAIR_FAIL:
    case PLINK_T_HELLO:
    case PLINK_T_WELCOME:
    case PLINK_T_REKEY:
    case PLINK_T_NOT_PAIRED:
    case PLINK_T_BEACON:
    case PLINK_T_STATUS:
    case PLINK_T_ZONES:
    case PLINK_T_ZONES_REQ:
    case PLINK_T_CFG_REQ:
    case PLINK_T_CFG_PAGE:
    case PLINK_T_EV_REQ:
    case PLINK_T_EVENTS:
    case PLINK_T_CMD:
    case PLINK_T_RESULT:
    case PLINK_T_SOUND:
    case PLINK_T_HEARTBEAT:
    case PLINK_T_TIME:
    case PLINK_T_BYE:
    case PLINK_T_FW_INFO:
    case PLINK_T_FW_ORDER:
    case PLINK_T_FW_STATE:
    case PLINK_T_FW_REQ:
    case PLINK_T_FW_PAGE:
    case PLINK_T_DEV_LIST_REQ:
    case PLINK_T_DEV_LIST_PAGE:
    case PLINK_T_DEV_STATE:
    case PLINK_T_DEV_STATE_REQ:
    case PLINK_T_DEV_ACT:
    case PLINK_T_DEV_ACK:
    case PLINK_T_WIFI_REQ:
    case PLINK_T_PREFS:
    case PLINK_T_PREFS_REQ:
    case PLINK_T_WIFI:
    case PLINK_T_PREFS_SET: return true;
    default: return false;
    }
}

bool plink_type_sealed(uint8_t type)
{
    return type >= 0x20u;
}

bool plink_type_from_master(uint8_t type)
{
    switch (type) {
    case PLINK_T_PAIR_COMMIT:
    case PLINK_T_PAIR_REVEAL:
    case PLINK_T_PAIR_DONE:
    case PLINK_T_PAIR_FAIL:
    case PLINK_T_WELCOME:
    case PLINK_T_REKEY:
    case PLINK_T_NOT_PAIRED:
    case PLINK_T_BEACON:
    case PLINK_T_STATUS:
    case PLINK_T_ZONES:
    case PLINK_T_CFG_PAGE:
    case PLINK_T_EVENTS:
    case PLINK_T_RESULT:
    case PLINK_T_SOUND:
    case PLINK_T_TIME:
    case PLINK_T_BYE:
    case PLINK_T_FW_ORDER:
    case PLINK_T_FW_PAGE:
    case PLINK_T_DEV_LIST_PAGE:
    case PLINK_T_DEV_STATE:
    case PLINK_T_DEV_ACK:
    case PLINK_T_PREFS_REQ:
    case PLINK_T_WIFI:
    case PLINK_T_PREFS_SET: return true;
    default: return false;
    }
}

/* ---- the header --------------------------------------------------------------------------- */

/* The rules a header must keep in a frame of len bytes (plink_hdr_read lists them). The writers
 * use them too, so nothing is ever sent that a receiver would drop. */
static bool hdr_ok(const plink_hdr_t *h, size_t len)
{
    const bool sealed = (h->flags & PLINK_FLAG_SEALED) != 0u;
    const bool from_master = (h->flags & PLINK_FLAG_FROM_MASTER) != 0u;
    if (len < PLINK_HDR_LEN || len > PLINK_FRAME_MAX || !plink_type_known(h->type) ||
        (h->flags & ~PLINK_FLAGS_KNOWN) != 0u || sealed != plink_type_sealed(h->type) ||
        from_master != (h->sender == PLINK_SENDER_MASTER)) {
        return false;
    }
    /* PAIR_FAIL travels either way; every other type one way only. */
    if (h->type != PLINK_T_PAIR_FAIL && from_master != plink_type_from_master(h->type)) {
        return false;
    }
    if (sealed) {
        return len >= PLINK_SEALED_OVERHEAD && h->sid != 0u && h->seq != 0u;
    }
    return h->sid == 0u && h->seq == 0u;
}

void plink_hdr_write(const plink_hdr_t *h, uint8_t out[PLINK_HDR_LEN])
{
    out[OFF_MAGIC] = PLINK_MAGIC0;
    out[OFF_MAGIC + 1u] = PLINK_MAGIC1;
    out[OFF_VERSION] = PLINK_VERSION;
    out[OFF_TYPE] = h->type;
    out[OFF_FLAGS] = h->flags;
    put_u16(&out[OFF_NET_ID], h->net_id);
    put_u16(&out[OFF_SENDER], h->sender);
    put_u32(&out[OFF_SID], h->sid);
    put_u32(&out[OFF_SEQ], h->seq);
}

bool plink_hdr_read(const uint8_t *frame, size_t len, plink_hdr_t *out)
{
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    bool ok = frame != NULL && len >= PLINK_HDR_LEN && frame[OFF_MAGIC] == PLINK_MAGIC0 &&
              frame[OFF_MAGIC + 1u] == PLINK_MAGIC1 && frame[OFF_VERSION] == PLINK_VERSION;
    if (ok) {
        h.type = frame[OFF_TYPE];
        h.flags = frame[OFF_FLAGS];
        h.net_id = get_u16(&frame[OFF_NET_ID]);
        h.sender = get_u16(&frame[OFF_SENDER]);
        h.sid = get_u32(&frame[OFF_SID]);
        h.seq = get_u32(&frame[OFF_SEQ]);
        ok = hdr_ok(&h, len);
    }
    if (out != NULL) {
        if (!ok) {
            memset(&h, 0, sizeof h);
        }
        *out = h;
    }
    return ok;
}

void plink_nonce(const plink_hdr_t *h, uint8_t out[PLINK_CCM_NONCE_LEN])
{
    out[0] = (h->flags & PLINK_FLAG_FROM_MASTER) != 0u ? PLINK_DIR_MASTER : PLINK_DIR_KEYPAD;
    put_u32(&out[1], h->sid);
    put_u16(&out[5], h->sender);
    put_u16(&out[7], h->net_id);
    put_u32(&out[9], h->seq);
}

/* ---- plain and sealed frames -------------------------------------------------------------- */

bool plink_frame_plain(const plink_hdr_t *h, const uint8_t *payload, size_t len, uint8_t *out,
                       size_t cap, size_t *out_len)
{
    if (out_len == NULL) {
        return false;
    }
    *out_len = 0u;
    if (h == NULL || out == NULL || (payload == NULL && len != 0u) ||
        len > PLINK_FRAME_MAX - PLINK_HDR_LEN || PLINK_HDR_LEN + len > cap) {
        return false;
    }
    const size_t total = PLINK_HDR_LEN + len;
    if (plink_type_sealed(h->type) || !hdr_ok(h, total)) {
        return false;
    }
    /* The payload first: it may already lie in out (encoded after the header's room). */
    if (len > 0u) {
        memmove(&out[PLINK_HDR_LEN], payload, len);
    }
    plink_hdr_write(h, out);
    *out_len = total;
    return true;
}

bool plink_frame_seal(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                      const plink_hdr_t *h, const uint8_t *payload, size_t len, uint8_t *out,
                      size_t cap, size_t *out_len)
{
    if (out_len == NULL) {
        return false;
    }
    *out_len = 0u;
    if (c == NULL || key == NULL || h == NULL || out == NULL || (payload == NULL && len != 0u) ||
        len > PLINK_PAYLOAD_MAX || PLINK_SEALED_OVERHEAD + len > cap) {
        return false;
    }
    const size_t total = PLINK_SEALED_OVERHEAD + len;
    if (!plink_type_sealed(h->type) || !hdr_ok(h, total)) {
        return false;
    }
    /* Header || CCM(payload) || tag, the header the associated data; encrypted where it lies. */
    uint8_t *body = &out[PLINK_HDR_LEN];
    if (len > 0u) {
        memmove(body, payload, len);
    }
    plink_hdr_write(h, out);
    uint8_t nonce[PLINK_CCM_NONCE_LEN];
    plink_nonce(h, nonce);
    if (!plink_ccm_seal(c, key, nonce, out, PLINK_HDR_LEN, body, len, body, &body[len])) {
        plink_wipe(out, total); /* no plaintext left behind */
        return false;
    }
    *out_len = total;
    return true;
}

bool plink_frame_open(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                      const plink_hdr_t *h, const uint8_t *frame, size_t len, uint8_t *pt,
                      size_t cap, size_t *pt_len)
{
    if (pt_len != NULL) {
        *pt_len = 0u;
    }
    bool ok = c != NULL && key != NULL && h != NULL && frame != NULL && pt_len != NULL &&
              plink_type_sealed(h->type) && hdr_ok(h, len);
    const size_t n = ok ? len - PLINK_SEALED_OVERHEAD : 0u;
    ok = ok && n <= cap && (pt != NULL || n == 0u);
    if (ok) {
        uint8_t nonce[PLINK_CCM_NONCE_LEN];
        plink_nonce(h, nonce);
        ok = plink_ccm_open(c, key, nonce, frame, PLINK_HDR_LEN, &frame[PLINK_HDR_LEN], n,
                            &frame[PLINK_HDR_LEN + n], pt);
    }
    if (ok) {
        *pt_len = n;
    } else {
        plink_wipe(pt, cap);
    }
    return ok;
}

/* ---- the replay window (RFC 4303 §3.4.3) -------------------------------------------------- */

bool plink_window_check(const plink_window_t *w, uint32_t seq)
{
    if (w == NULL || seq == 0u) {
        return false;
    }
    if (seq > w->top) {
        return true; /* ahead of everything seen (a zeroed window: anything from 1) */
    }
    const uint32_t back = w->top - seq;
    return back < WINDOW_BITS && (w->bits & ((uint64_t)1u << back)) == 0u;
}

void plink_window_commit(plink_window_t *w, uint32_t seq)
{
    if (w == NULL || seq == 0u) {
        return;
    }
    if (seq > w->top) {
        /* The window slides: bit 0 is the new top, older bits move up, the oldest fall off. */
        const uint32_t ahead = seq - w->top;
        w->bits = ahead >= WINDOW_BITS ? (uint64_t)1u : (w->bits << ahead) | (uint64_t)1u;
        w->top = seq;
    } else {
        const uint32_t back = w->top - seq;
        if (back < WINDOW_BITS) {
            w->bits |= (uint64_t)1u << back;
        }
    }
}

/* ---- addresses ---------------------------------------------------------------------------- */

plink_addr_t plink_addr_udp(const uint8_t ip[4], uint16_t port)
{
    plink_addr_t a;
    memset(&a, 0, sizeof a);
    a.tp = PLINK_TP_UDP;
    a.len = 6u;
    if (ip != NULL) {
        memcpy(a.a, ip, 4u);
    }
    a.a[4] = (uint8_t)(port >> 8);
    a.a[5] = (uint8_t)port;
    return a;
}

bool plink_addr_equal(const plink_addr_t *a, const plink_addr_t *b)
{
    return a != NULL && b != NULL && a->tp == b->tp && a->len == b->len &&
           a->len <= PLINK_ADDR_MAX && memcmp(a->a, b->a, a->len) == 0;
}

bool plink_addr_is_broadcast(const plink_addr_t *a)
{
    /* Only 255.255.255.255: a subnet's broadcast is named by its caller (plink_frame.h). */
    return a != NULL && a->tp == PLINK_TP_UDP && a->len == 6u && a->a[0] == 255u &&
           a->a[1] == 255u && a->a[2] == 255u && a->a[3] == 255u;
}

/* Appends v in decimal at out[at]; returns the new end. */
static size_t put_dec(char *out, size_t at, unsigned v)
{
    char digits[10];
    size_t n = 0u;
    do {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        out[at++] = digits[--n];
    }
    return at;
}

static size_t put_hex(char *out, size_t at, uint8_t b)
{
    static const char HEX[] = "0123456789abcdef";
    out[at++] = HEX[b >> 4];
    out[at++] = HEX[b & 0x0Fu];
    return at;
}

void plink_addr_text(const plink_addr_t *a, char out[24])
{
    if (out == NULL) {
        return;
    }
    size_t at = 0u;
    if (a != NULL && a->tp == PLINK_TP_UDP && a->len == 6u) {
        /* "255.255.255.255:65535" is 21 characters at most. */
        for (size_t i = 0; i < 4u; i++) {
            at = put_dec(out, at, a->a[i]);
            out[at++] = i < 3u ? '.' : ':';
        }
        at = put_dec(out, at, ((unsigned)a->a[4] << 8) | (unsigned)a->a[5]);
    } else if (a != NULL && a->tp == PLINK_TP_ESPNOW && a->len == 6u) {
        for (size_t i = 0; i < 6u; i++) {
            if (i > 0u) {
                out[at++] = ':';
            }
            at = put_hex(out, at, a->a[i]);
        }
    } else if (a != NULL && a->tp == PLINK_TP_RF && a->len > 0u && a->len <= PLINK_ADDR_MAX) {
        /* Reserved: "rf:" and the first 10 bytes in hex (23 characters at most). */
        out[at++] = 'r';
        out[at++] = 'f';
        out[at++] = ':';
        for (size_t i = 0; i < a->len && i < 10u; i++) {
            at = put_hex(out, at, a->a[i]);
        }
    } else {
        out[at++] = '-';
    }
    out[at] = '\0';
}
