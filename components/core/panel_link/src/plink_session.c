/*
 * plink_session.c - the handshake at every connection and the sessions it makes (see
 * include/panel_link/plink_session.h; docs/features/11 "Sessions"; LINK-003, LINK-015).
 *
 *   K_hs   = HKDF-Expand(K_pair, "plink v1 hello", 32)
 *   HELLO  = header || proto, caps, capacities, n_k, e_k || mac1 = HMAC(K_hs, all before)[0..15]
 *   TH_s   = SHA-256(HELLO frame || WELCOME frame up to mac2)
 *   PRK_s  = HKDF-Extract(salt n_k || n_m, ikm K_pair || X25519(e, E_peer))
 *   HKDF-Expand(PRK_s, "plink v1 session" || TH_s, 68) = K_m2k[16] || K_k2m[16] || K_fin[32] ||
 *            sid[4] (little-endian; 0 becomes 1)
 *   WELCOME = header (sid 0) || proto, caps, n_m, e_m || mac2 = HMAC(K_fin, all before)[0..15]
 *
 * Every key and secret on the stack is wiped before returning. Random numbers are drawn in a
 * fixed order, which the golden vectors rely on (test/vectors_plink_session.py): the ephemeral
 * private key (32 bytes), then the nonce (16), on both sides.
 */
#include "panel_link/plink_session.h"

#include <string.h>

#include "panel_link/plink_pair.h" /* plink_mac_fill, plink_mac_check */

static const char L_HELLO[] = "plink v1 hello";
static const char L_SESSION[] = "plink v1 session";

#define OKM_LEN (2u * PLINK_AES_LEN + PLINK_KEY_LEN + 4u) /* 68 */

/* What a handshake derives. Holds keys: wiped by its user. */
typedef struct {
    uint8_t m2k[PLINK_AES_LEN];
    uint8_t k2m[PLINK_AES_LEN];
    uint8_t fin[PLINK_KEY_LEN];
    uint32_t sid;
} hs_keys_t;

static uint8_t lower(uint8_t a, uint8_t b)
{
    return a < b ? a : b;
}

/* K_hs from the pairing key (used as HKDF's PRK). */
static bool hello_key(const uint8_t k_pair[PLINK_KEY_LEN], uint8_t k_hs[PLINK_KEY_LEN])
{
    const plink_iov_t info = {L_HELLO, sizeof L_HELLO - 1u};
    return plink_hkdf_expand(k_pair, &info, 1u, k_hs, PLINK_KEY_LEN);
}

/* The session's keys and id: hello is the whole HELLO frame, welcome the WELCOME frame up to
 * mac2 (welcome_len bytes). */
static bool derive(const uint8_t k_pair[PLINK_KEY_LEN], const uint8_t n_k[PLINK_NONCE_LEN],
                   const uint8_t n_m[PLINK_NONCE_LEN], const uint8_t dh[PLINK_X25519_LEN],
                   const uint8_t *hello, size_t hello_len, const uint8_t *welcome,
                   size_t welcome_len, hs_keys_t *k)
{
    uint8_t th[PLINK_SHA256_LEN], prk[PLINK_SHA256_LEN], okm[OKM_LEN], salt[2u * PLINK_NONCE_LEN];
    const plink_iov_t transcript[] = {{hello, hello_len}, {welcome, welcome_len}};
    plink_sha256(transcript, 2u, th);
    memcpy(salt, n_k, PLINK_NONCE_LEN);
    memcpy(&salt[PLINK_NONCE_LEN], n_m, PLINK_NONCE_LEN);
    const plink_iov_t ikm[] = {{k_pair, PLINK_KEY_LEN}, {dh, PLINK_X25519_LEN}};
    plink_hkdf_extract(salt, sizeof salt, ikm, 2u, prk);
    const plink_iov_t info[] = {{L_SESSION, sizeof L_SESSION - 1u}, {th, sizeof th}};
    const bool ok = plink_hkdf_expand(prk, info, 2u, okm, sizeof okm);
    if (ok) {
        memcpy(k->m2k, okm, PLINK_AES_LEN);
        memcpy(k->k2m, &okm[PLINK_AES_LEN], PLINK_AES_LEN);
        memcpy(k->fin, &okm[2u * PLINK_AES_LEN], PLINK_KEY_LEN);
        const uint8_t *s = &okm[2u * PLINK_AES_LEN + PLINK_KEY_LEN];
        k->sid = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) |
                 ((uint32_t)s[3] << 24);
        if (k->sid == 0u) {
            k->sid = 1u; /* 0 means a plain frame */
        }
    }
    plink_wipe(okm, sizeof okm);
    plink_wipe(prk, sizeof prk);
    plink_wipe(salt, sizeof salt);
    plink_wipe(th, sizeof th);
    return ok;
}

/* ---- sessions ----------------------------------------------------------------------------- */

void plink_session_wipe(plink_session_t *s)
{
    plink_wipe(s, s != NULL ? sizeof *s : 0u);
}

bool plink_session_seal(const plink_crypto_t *c, plink_session_t *s, uint8_t type,
                        uint16_t net_id, uint16_t sender, bool from_master,
                        const uint8_t *payload, size_t len, uint8_t *out, size_t cap,
                        size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0u;
    }
    /* LINK-015: no seq at or above 2^31 is ever sent; the keypad rekeys long before. */
    if (s == NULL || !s->valid || out_len == NULL || s->tx_seq >= PLINK_SEQ_LIMIT - 1u) {
        return false;
    }
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.flags = (uint8_t)(PLINK_FLAG_SEALED | (from_master ? PLINK_FLAG_FROM_MASTER : 0u));
    h.net_id = net_id;
    h.sender = sender;
    h.sid = s->sid;
    h.seq = s->tx_seq + 1u;
    /* The seq is spent whatever happens next, so no nonce is ever tried twice with this key. */
    s->tx_seq = h.seq;
    return plink_frame_seal(c, s->k_tx, &h, payload, len, out, cap, out_len);
}

bool plink_session_open(const plink_crypto_t *c, plink_session_t *s, const plink_hdr_t *h,
                        const uint8_t *frame, size_t len, uint8_t *pt, size_t cap,
                        size_t *pt_len)
{
    if (pt_len != NULL) {
        *pt_len = 0u;
    }
    if (s == NULL || !s->valid || h == NULL || pt_len == NULL || h->sid != s->sid ||
        h->seq >= PLINK_SEQ_LIMIT) {
        return false;
    }
    /* LINK-003: the window first (a replay costs nothing), the tag next, and only a frame whose
     * tag verified moves the window. */
    if (!plink_window_check(&s->rx, h->seq) ||
        !plink_frame_open(c, s->k_rx, h, frame, len, pt, cap, pt_len)) {
        return false;
    }
    plink_window_commit(&s->rx, h->seq);
    return true;
}

/* ---- the keypad's handshake --------------------------------------------------------------- */

bool plink_hs_hello(const plink_crypto_t *c, const uint8_t k_pair[PLINK_KEY_LEN], uint16_t net_id,
                    uint16_t device, const plink_caps_t *cap, uint16_t caps, plink_hs_k_t *hs)
{
    if (hs == NULL) {
        return false;
    }
    /* Whatever an earlier HELLO left goes first: e_k and n_k are never reused. */
    plink_hs_k_wipe(hs);
    if (c == NULL || k_pair == NULL || cap == NULL || net_id == 0u ||
        device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST) {
        return false;
    }
    plink_hello_t m;
    memset(&m, 0, sizeof m);
    m.proto = PLINK_PROTO;
    m.caps = (uint16_t)(caps & PLINK_CAPS_KNOWN);
    m.cap = *cap;
    uint8_t k_hs[PLINK_KEY_LEN];
    size_t plen = 0u;
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = PLINK_T_HELLO;
    h.net_id = net_id;
    h.sender = device;
    bool ok = plink_x25519_keypair(c, hs->e, m.e) && plink_random(c, hs->n_k, PLINK_NONCE_LEN);
    if (ok) {
        memcpy(m.n, hs->n_k, PLINK_NONCE_LEN);
        ok = plink_enc_hello(&m, true, &hs->hello[PLINK_HDR_LEN],
                             sizeof hs->hello - PLINK_HDR_LEN, &plen) &&
             plink_frame_plain(&h, &hs->hello[PLINK_HDR_LEN], plen, hs->hello, sizeof hs->hello,
                               &hs->hello_len) &&
             hello_key(k_pair, k_hs);
    }
    if (ok) {
        plink_mac_fill(k_hs, hs->hello, hs->hello_len); /* mac1 */
        hs->caps = m.caps;
        hs->waiting = true;
    } else {
        plink_hs_k_wipe(hs);
    }
    plink_wipe(k_hs, sizeof k_hs);
    plink_wipe(&m, sizeof m);
    return ok;
}

bool plink_hs_finish(const plink_crypto_t *c, const uint8_t k_pair[PLINK_KEY_LEN],
                     plink_hs_k_t *hs, const plink_hdr_t *h, const uint8_t *frame, size_t len,
                     plink_session_t *out)
{
    /* Only while the answer to the latest HELLO is awaited, and only for that HELLO's network:
     * a replayed WELCOME, or one made for an earlier HELLO, fails the transcript. */
    if (c == NULL || k_pair == NULL || hs == NULL || h == NULL || frame == NULL || out == NULL ||
        !hs->waiting || h->type != PLINK_T_WELCOME || len < PLINK_HDR_LEN + PLINK_MAC_LEN ||
        hs->hello_len < PLINK_HDR_LEN ||
        h->net_id != (uint16_t)((unsigned)hs->hello[5] | ((unsigned)hs->hello[6] << 8))) {
        return false;
    }
    plink_hello_t w;
    if (!plink_dec_hello(&frame[PLINK_HDR_LEN], len - PLINK_HDR_LEN, false, &w) || w.proto == 0u) {
        return false;
    }
    uint8_t dh[PLINK_X25519_LEN];
    hs_keys_t k;
    memset(&k, 0, sizeof k);
    const bool ok = plink_x25519_shared(c, hs->e, w.e, dh) &&
                    derive(k_pair, hs->n_k, w.n, dh, hs->hello, hs->hello_len, frame,
                           len - PLINK_MAC_LEN, &k) &&
                    plink_mac_check(k.fin, frame, len);
    if (ok) {
        plink_session_t s;
        memset(&s, 0, sizeof s);
        s.valid = true;
        s.sid = k.sid;
        memcpy(s.k_tx, k.k2m, PLINK_AES_LEN);
        memcpy(s.k_rx, k.m2k, PLINK_AES_LEN);
        s.proto = lower(PLINK_PROTO, w.proto);
        s.caps = (uint16_t)(hs->caps & PLINK_CAPS_KNOWN & w.caps);
        *out = s;
        plink_wipe(&s, sizeof s);
        plink_wipe(hs->e, sizeof hs->e);
        plink_wipe(hs->n_k, sizeof hs->n_k);
        hs->waiting = false;
    }
    plink_wipe(dh, sizeof dh);
    plink_wipe(&k, sizeof k);
    return ok;
}

void plink_hs_k_wipe(plink_hs_k_t *hs)
{
    plink_wipe(hs, hs != NULL ? sizeof *hs : 0u);
}

/* ---- the master's answer ------------------------------------------------------------------ */

bool plink_hs_hello_valid(const uint8_t k_pair[PLINK_KEY_LEN], const uint8_t *frame, size_t len)
{
    if (k_pair == NULL || frame == NULL || len < PLINK_HDR_LEN + PLINK_MAC_LEN) {
        return false;
    }
    uint8_t k_hs[PLINK_KEY_LEN];
    const bool ok = hello_key(k_pair, k_hs) && plink_mac_check(k_hs, frame, len);
    plink_wipe(k_hs, sizeof k_hs);
    return ok;
}

bool plink_hs_answer(const plink_crypto_t *c, const uint8_t k_pair[PLINK_KEY_LEN],
                     const plink_hdr_t *h, const uint8_t *frame, size_t len, uint16_t caps,
                     plink_session_t *pending, plink_hello_t *hello, uint8_t *welcome,
                     size_t cap, size_t *welcome_len)
{
    if (welcome_len != NULL) {
        *welcome_len = 0u;
    }
    if (c == NULL || k_pair == NULL || h == NULL || frame == NULL || pending == NULL ||
        hello == NULL || welcome == NULL || welcome_len == NULL || cap < PLINK_HDR_LEN ||
        h->type != PLINK_T_HELLO) {
        return false;
    }
    /* mac1 first: a HELLO not made with this keypad's key costs no X25519. */
    plink_hello_t in;
    if (!plink_hs_hello_valid(k_pair, frame, len) ||
        !plink_dec_hello(&frame[PLINK_HDR_LEN], len - PLINK_HDR_LEN, true, &in) ||
        in.proto == 0u) {
        return false;
    }
    plink_hello_t m;
    memset(&m, 0, sizeof m);
    m.proto = PLINK_PROTO;
    m.caps = (uint16_t)(caps & PLINK_CAPS_KNOWN);
    uint8_t e[PLINK_X25519_LEN], dh[PLINK_X25519_LEN];
    hs_keys_t k;
    memset(&k, 0, sizeof k);
    plink_hdr_t wh;
    memset(&wh, 0, sizeof wh);
    wh.type = PLINK_T_WELCOME;
    wh.flags = PLINK_FLAG_FROM_MASTER;
    wh.net_id = h->net_id;
    wh.sender = PLINK_SENDER_MASTER; /* sid 0: the session isn't one until a frame proves it */
    size_t plen = 0u, wl = 0u;
    const bool ok =
        plink_x25519_keypair(c, e, m.e) && plink_random(c, m.n, PLINK_NONCE_LEN) &&
        plink_x25519_shared(c, e, in.e, dh) &&
        plink_enc_hello(&m, false, &welcome[PLINK_HDR_LEN], cap - PLINK_HDR_LEN, &plen) &&
        plink_frame_plain(&wh, &welcome[PLINK_HDR_LEN], plen, welcome, cap, &wl) &&
        wl >= PLINK_HDR_LEN + PLINK_MAC_LEN &&
        derive(k_pair, in.n, m.n, dh, frame, len, welcome, wl - PLINK_MAC_LEN, &k);
    if (ok) {
        plink_mac_fill(k.fin, welcome, wl); /* mac2 */
        plink_session_t s;
        memset(&s, 0, sizeof s);
        s.valid = true;
        s.sid = k.sid;
        memcpy(s.k_tx, k.m2k, PLINK_AES_LEN);
        memcpy(s.k_rx, k.k2m, PLINK_AES_LEN);
        s.proto = lower(PLINK_PROTO, in.proto);
        s.caps = (uint16_t)(m.caps & in.caps); /* an unknown bit is never common */
        s.peer_cap = in.cap;
        *pending = s;
        plink_wipe(&s, sizeof s);
        *hello = in;
        *welcome_len = wl;
    } else {
        plink_wipe(welcome, cap);
    }
    plink_wipe(e, sizeof e);
    plink_wipe(dh, sizeof dh);
    plink_wipe(&k, sizeof k);
    plink_wipe(&m, sizeof m);
    return ok;
}
