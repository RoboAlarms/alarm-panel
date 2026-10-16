/*
 * plink_pair.c - pairing a keypad with its master (see include/panel_link/plink_pair.h;
 * docs/features/11 "Pairing"; LINK-004, LINK-016): the 6-digit code, the commitment, the keys and
 * the MACs that end a plain frame, then the master's and the keypad's state machines.
 *
 * Each side keeps the payloads it took for each step (the *_raw fields, a whole frame's payload
 * each, so a newer firmware's longer one is taken: LINK-017): a repeat that is the same byte for
 * byte gets the stored answer again, anything else for the same attempt ends it, so no value is
 * ever set twice (LINK-016); the transcript hashes the two first payloads exactly as they
 * travelled. Whenever an attempt ends, its secrets - the X25519 private key and shared secret, the
 * nonces, K_pair, K_conf, the code - are wiped.
 *
 * Random numbers are drawn in a fixed order, which the golden vectors rely on
 * (test/vectors_plink_pair.py): the keypad's attempt id (4 bytes, little-endian) then its private
 * key (32) when it sends PAIR_REQ, n_k (16) when it takes PAIR_COMMIT; the master's private key
 * (32) then n_m (16) when it takes PAIR_REQ.
 *
 * Every call that may answer takes a plink_out_t, which must not be NULL (the call does nothing
 * then); out->len is 0 when there is nothing to send.
 */
#include "panel_link/plink_pair.h"

#include <string.h>

#include "panel_link/plink_wire.h"

#define PAYLOAD_CAP PLINK_PAIR_PAYLOAD_MAX

/* The labels of docs/features/11 "Pairing", hashed without their NUL. */
static const char L_COMMIT[] = "plink v1 commit";
static const char L_SAS[] = "plink v1 sas";
static const char L_TRANSCRIPT[] = "plink v1 transcript";
static const char L_PAIR[] = "plink v1 pair";
static const char L_KEY[] = "plink v1 key";
static const char L_CONF[] = "plink v1 conf";

#define LABEL(s) {(s), sizeof(s) - 1u}

/* ---- small helpers ------------------------------------------------------------------------ */

static void put_le16(uint8_t p[2], uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Whole seconds left until `until` (rounded up), 0 once it has passed. */
static uint32_t secs_left(uint64_t now, uint64_t until)
{
    return now < until ? (uint32_t)((until - now + 999u) / 1000u) : 0u;
}

/* strlen, at most max. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0u;
    while (s != NULL && n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static bool same_bytes(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len)
{
    return a_len == b_len && (a_len == 0u || memcmp(a, b, a_len) == 0);
}

/* ---- the code, the commitment, the keys ---------------------------------------------------- */

void plink_pair_code(const uint8_t pk_k[PLINK_X25519_LEN], const uint8_t pk_m[PLINK_X25519_LEN],
                     const uint8_t n_k[PLINK_NONCE_LEN], const uint8_t n_m[PLINK_NONCE_LEN],
                     char code[PLINK_CODE_LEN])
{
    if (code == NULL) {
        return;
    }
    code[0] = '\0';
    if (pk_k == NULL || pk_m == NULL || n_k == NULL || n_m == NULL) {
        return;
    }
    /* be32(SHA-256("plink v1 sas" || pk_k || pk_m || n_k || n_m)[0..3]) mod 10^6, six digits. */
    const plink_iov_t parts[] = {
        LABEL(L_SAS),          {pk_k, PLINK_X25519_LEN}, {pk_m, PLINK_X25519_LEN},
        {n_k, PLINK_NONCE_LEN}, {n_m, PLINK_NONCE_LEN},
    };
    uint8_t h[PLINK_SHA256_LEN];
    plink_sha256(parts, sizeof parts / sizeof parts[0], h);
    uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) |
                 (uint32_t)h[3];
    v %= 1000000u;
    for (size_t i = PLINK_CODE_LEN - 1u; i-- > 0u;) {
        code[i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    code[PLINK_CODE_LEN - 1u] = '\0';
    plink_wipe(h, sizeof h);
}

void plink_pair_commitment(const uint8_t pk_m[PLINK_X25519_LEN],
                           const uint8_t pk_k[PLINK_X25519_LEN],
                           const uint8_t n_m[PLINK_NONCE_LEN], uint8_t out[PLINK_SHA256_LEN])
{
    if (out == NULL) {
        return;
    }
    if (pk_m == NULL || pk_k == NULL || n_m == NULL) {
        memset(out, 0, PLINK_SHA256_LEN);
        return;
    }
    /* SHA-256("plink v1 commit" || pk_m || pk_k || n_m): binds the master to n_m before n_k. */
    const plink_iov_t parts[] = {
        LABEL(L_COMMIT), {pk_m, PLINK_X25519_LEN}, {pk_k, PLINK_X25519_LEN},
        {n_m, PLINK_NONCE_LEN},
    };
    plink_sha256(parts, sizeof parts / sizeof parts[0], out);
}

bool plink_pair_keys(const uint8_t dh[PLINK_X25519_LEN], const uint8_t *req_payload,
                     size_t req_len, const uint8_t *commit_payload, size_t commit_len,
                     uint16_t net_id, const uint8_t n_k[PLINK_NONCE_LEN],
                     const uint8_t n_m[PLINK_NONCE_LEN], uint8_t k_pair[PLINK_KEY_LEN],
                     uint8_t k_conf[PLINK_KEY_LEN])
{
    static const uint8_t zero[PLINK_X25519_LEN] = {0};
    const bool ok = dh != NULL && (req_payload != NULL || req_len == 0u) &&
                    (commit_payload != NULL || commit_len == 0u) && req_len <= 0xFFFFu &&
                    commit_len <= 0xFFFFu && n_k != NULL && n_m != NULL && k_pair != NULL &&
                    k_conf != NULL && !plink_ct_equal(dh, zero, sizeof zero);
    if (!ok) {
        plink_wipe(k_pair, k_pair != NULL ? PLINK_KEY_LEN : 0u);
        plink_wipe(k_conf, k_conf != NULL ? PLINK_KEY_LEN : 0u);
        return false;
    }
    /* TH = SHA-256("plink v1 transcript" || PAIR_REQ payload || PAIR_COMMIT payload || net_id ||
     * n_k || n_m), every element after the label prefixed with its length as u16 (little-endian,
     * as everything on the link), so no two transcripts hash the same bytes. */
    uint8_t l_req[2], l_commit[2], l_net[2], net[2], l_nonce[2];
    put_le16(l_req, (uint16_t)req_len);
    put_le16(l_commit, (uint16_t)commit_len);
    put_le16(l_net, (uint16_t)sizeof net);
    put_le16(net, net_id);
    put_le16(l_nonce, (uint16_t)PLINK_NONCE_LEN);
    const plink_iov_t th_parts[] = {
        LABEL(L_TRANSCRIPT),   {l_req, 2u},  {req_payload, req_len}, {l_commit, 2u},
        {commit_payload, commit_len},        {l_net, 2u},            {net, 2u},
        {l_nonce, 2u},         {n_k, PLINK_NONCE_LEN},                {l_nonce, 2u},
        {n_m, PLINK_NONCE_LEN},
    };
    uint8_t th[PLINK_SHA256_LEN], prk[PLINK_SHA256_LEN];
    plink_sha256(th_parts, sizeof th_parts / sizeof th_parts[0], th);
    /* PRK = HKDF-Extract("plink v1 pair", DH); K_pair and K_conf expand it with the transcript. */
    const plink_iov_t ikm = {dh, PLINK_X25519_LEN};
    plink_hkdf_extract((const uint8_t *)L_PAIR, sizeof L_PAIR - 1u, &ikm, 1u, prk);
    const plink_iov_t key_info[] = {LABEL(L_KEY), {th, sizeof th}};
    const plink_iov_t conf_info[] = {LABEL(L_CONF), {th, sizeof th}};
    const bool done = plink_hkdf_expand(prk, key_info, 2u, k_pair, PLINK_KEY_LEN) &&
                      plink_hkdf_expand(prk, conf_info, 2u, k_conf, PLINK_KEY_LEN);
    if (!done) {
        plink_wipe(k_pair, PLINK_KEY_LEN);
        plink_wipe(k_conf, PLINK_KEY_LEN);
    }
    plink_wipe(prk, sizeof prk);
    plink_wipe(th, sizeof th);
    return done;
}

/* HMAC(key, frame up to its last 16 bytes), all 32 bytes. */
static bool frame_mac(const uint8_t key[PLINK_KEY_LEN], const uint8_t *frame, size_t len,
                      uint8_t mac[PLINK_SHA256_LEN])
{
    if (key == NULL || frame == NULL || len < PLINK_MAC_LEN) {
        return false;
    }
    const plink_iov_t part = {frame, len - PLINK_MAC_LEN};
    plink_hmac(key, PLINK_KEY_LEN, &part, 1u, mac);
    return true;
}

void plink_mac_fill(const uint8_t key[PLINK_KEY_LEN], uint8_t *frame, size_t len)
{
    uint8_t mac[PLINK_SHA256_LEN];
    if (frame_mac(key, frame, len, mac)) {
        memcpy(&frame[len - PLINK_MAC_LEN], mac, PLINK_MAC_LEN);
    }
    plink_wipe(mac, sizeof mac);
}

bool plink_mac_check(const uint8_t key[PLINK_KEY_LEN], const uint8_t *frame, size_t len)
{
    uint8_t mac[PLINK_SHA256_LEN];
    const bool ok = frame_mac(key, frame, len, mac) &&
                    plink_ct_equal(mac, &frame[len - PLINK_MAC_LEN], PLINK_MAC_LEN);
    plink_wipe(mac, sizeof mac);
    return ok;
}

/* ---- frames ------------------------------------------------------------------------------- */

/* out->buf holds a payload of len bytes at PLINK_HDR_LEN: the plain header goes before it and the
 * frame to `to`. The master's frames carry sender 0 and FROM_MASTER, a keypad's sender 0xFFFF
 * (it has no number yet). */
static bool finish(plink_out_t *out, const plink_addr_t *to, uint8_t type, uint16_t net_id,
                   bool from_master, size_t len)
{
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.flags = from_master ? PLINK_FLAG_FROM_MASTER : 0u;
    h.net_id = net_id;
    h.sender = from_master ? PLINK_SENDER_MASTER : PLINK_SENDER_UNPAIRED;
    if (!plink_frame_plain(&h, &out->buf[PLINK_HDR_LEN], len, out->buf, sizeof out->buf,
                           &out->len)) {
        out->len = 0u;
        return false;
    }
    out->to = *to;
    return true;
}

static void send_fail(plink_out_t *out, const plink_addr_t *to, uint16_t net_id, bool from_master,
                      uint32_t attempt, uint8_t reason)
{
    if (out == NULL) {
        return;
    }
    const plink_pair_fail_t f = {attempt, reason};
    size_t len = 0u;
    if (plink_enc_pair_fail(&f, &out->buf[PLINK_HDR_LEN], PAYLOAD_CAP, &len)) {
        (void)finish(out, to, PLINK_T_PAIR_FAIL, net_id, from_master, len);
    }
}

/* PAIR_NONCE (keypad: n_k) and PAIR_REVEAL (master: n_m) share a payload. */
static bool send_nonce(plink_out_t *out, const plink_addr_t *to, uint8_t type, uint16_t net_id,
                       bool from_master, uint32_t attempt, const uint8_t n[PLINK_NONCE_LEN])
{
    plink_pair_nonce_t m;
    m.attempt = attempt;
    memcpy(m.n, n, PLINK_NONCE_LEN);
    size_t len = 0u;
    const bool ok = plink_enc_pair_nonce(&m, &out->buf[PLINK_HDR_LEN], PAYLOAD_CAP, &len) &&
                    finish(out, to, type, net_id, from_master, len);
    plink_wipe(&m, sizeof m);
    return ok;
}

/* ---- the master's side -------------------------------------------------------------------- */

/* An attempt is under way: from its PAIR_COMMIT until PAIR_DONE or a failure. */
static bool m_busy(const plink_pair_m_t *pm)
{
    return pm->state == PLINK_PS_WORKING || pm->state == PLINK_PS_COMPARE ||
           pm->state == PLINK_PS_WAITING;
}

/* The attempt's secrets and the values it took. What is shown about it (the keypad's name, its
 * address) stays. */
static void m_wipe(plink_pair_m_t *pm)
{
    plink_wipe(pm->sk, sizeof pm->sk);
    plink_wipe(pm->dh, sizeof pm->dh);
    plink_wipe(pm->pk_m, sizeof pm->pk_m);
    plink_wipe(pm->n_m, sizeof pm->n_m);
    plink_wipe(pm->pk_k, sizeof pm->pk_k);
    plink_wipe(pm->n_k, sizeof pm->n_k);
    plink_wipe(pm->k_pair, sizeof pm->k_pair);
    plink_wipe(pm->k_conf, sizeof pm->k_conf);
    plink_wipe(pm->code, sizeof pm->code);
    plink_wipe(pm->req_raw, sizeof pm->req_raw);
    plink_wipe(pm->commit_raw, sizeof pm->commit_raw);
    plink_wipe(pm->nonce_raw, sizeof pm->nonce_raw);
    plink_wipe(pm->confirm_raw, sizeof pm->confirm_raw);
    pm->req_len = pm->commit_len = pm->nonce_len = pm->confirm_len = 0u;
    pm->allowed = pm->confirmed = pm->revealed = false;
}

/* Ends the attempt under way (or a request refused before it became one) with reason, told to
 * its keypad when tell. An attempt that revealed n_m stays counted; the window closes after the
 * third, or once its time is up. */
static void m_end(plink_pair_m_t *pm, uint64_t now, uint8_t reason, bool tell, plink_out_t *out)
{
    if (tell) {
        send_fail(out, &pm->peer, pm->net_id, true, pm->attempt, reason);
    }
    m_wipe(pm);
    pm->ended = true;
    pm->ended_attempt = pm->attempt;
    pm->ended_fail = reason;
    pm->state = PLINK_PS_FAILED;
    pm->fail = reason;
    pm->next_attempt_at = now + PLINK_PAIR_GAP_MS;
    if (pm->attempts >= PLINK_PAIR_ATTEMPTS) {
        pm->window = false;
        pm->fail = PLINK_FAIL_LIMIT;
    } else if (now >= pm->window_until) {
        pm->window = false;
    }
}

/* A message of the attempt that ended, from its keypad: its PAIR_FAIL again (whose first copy may
 * have been lost); nothing after a PAIR_DONE. */
static void m_ended(const plink_pair_m_t *pm, const plink_addr_t *from, plink_out_t *out)
{
    if (pm->ended_fail != PLINK_FAIL_NONE) {
        send_fail(out, from, pm->net_id, true, pm->ended_attempt, pm->ended_fail);
    }
}

static void m_send_commit(const plink_pair_m_t *pm, plink_out_t *out)
{
    memcpy(&out->buf[PLINK_HDR_LEN], pm->commit_raw, pm->commit_len);
    (void)finish(out, &pm->peer, PLINK_T_PAIR_COMMIT, pm->net_id, true, pm->commit_len);
}

/* Two keypads at once: the attempt under way ends (its keypad is told), or the newcomer is, and
 * the window closes. */
static void m_overlap(plink_pair_m_t *pm, uint64_t now, const plink_addr_t *from,
                      uint32_t attempt, plink_out_t *out)
{
    if (m_busy(pm)) {
        m_end(pm, now, PLINK_FAIL_OVERLAP, true, out);
    } else {
        send_fail(out, from, pm->net_id, true, attempt, PLINK_FAIL_OVERLAP);
    }
    pm->window = false;
    pm->state = PLINK_PS_FAILED;
    pm->fail = PLINK_FAIL_OVERLAP;
}

/* A new attempt from the window's keypad: checked, then its key pair, n_m and the commitment;
 * PAIR_COMMIT answers. A request refused here was never an attempt and is not counted. */
static void m_start(plink_pair_m_t *pm, uint64_t now, const plink_addr_t *from,
                    const plink_pair_req_t *req, const uint8_t *p, size_t n, plink_out_t *out)
{
    m_wipe(pm);
    pm->have_peer = true;
    pm->peer = *from;
    pm->attempt = req->attempt;
    pm->req = *req;
    pm->ended = false;
    uint8_t refuse = PLINK_FAIL_NONE;
    if (req->proto == 0u) {
        refuse = PLINK_FAIL_VERSION; /* no version in common */
    } else if (req->cap.zones < pm->zones_used) {
        refuse = PLINK_FAIL_CAPACITY;
    } else {
        memcpy(pm->req_raw, p, n);
        pm->req_len = n;
        memcpy(pm->pk_k, req->pk, PLINK_X25519_LEN);
        if (!plink_x25519_keypair(pm->c, pm->sk, pm->pk_m) ||
            !plink_random(pm->c, pm->n_m, PLINK_NONCE_LEN)) {
            refuse = PLINK_FAIL_BUSY; /* the port failed: try again */
        } else if (!plink_x25519_shared(pm->c, pm->sk, pm->pk_k, pm->dh)) {
            refuse = PLINK_FAIL_MISMATCH; /* a key of small order */
        }
    }
    if (refuse == PLINK_FAIL_NONE) {
        plink_pair_commit_t cm;
        memset(&cm, 0, sizeof cm);
        cm.attempt = pm->attempt;
        cm.proto = PLINK_PROTO;
        cm.caps = PLINK_CAPS;
        memcpy(cm.pk, pm->pk_m, PLINK_X25519_LEN);
        plink_pair_commitment(pm->pk_m, pm->pk_k, pm->n_m, cm.commit);
        memcpy(cm.name, pm->name, sizeof cm.name);
        memcpy(cm.panel_id, pm->panel_id, sizeof cm.panel_id);
        if (!plink_enc_pair_commit(&cm, pm->commit_raw, sizeof pm->commit_raw, &pm->commit_len)) {
            refuse = PLINK_FAIL_BUSY;
        }
    }
    if (refuse != PLINK_FAIL_NONE) {
        m_end(pm, now, refuse, true, out);
        return;
    }
    pm->state = PLINK_PS_WORKING;
    pm->step_until = now + PLINK_PAIR_STEP_MS;
    m_send_commit(pm, out);
}

static void m_req(plink_pair_m_t *pm, uint64_t now, const plink_addr_t *from,
                  const plink_hdr_t *h, const uint8_t *p, size_t n, plink_out_t *out)
{
    plink_pair_req_t req;
    if ((h->net_id != 0u && h->net_id != pm->net_id) || !plink_dec_pair_req(p, n, &req)) {
        return; /* for another master, or not a request */
    }
    const bool from_peer = pm->have_peer && plink_addr_equal(from, &pm->peer);
    if (m_busy(pm) && from_peer && req.attempt == pm->attempt) {
        /* The attempt under way: a repeat gets its PAIR_COMMIT again, a changed one ends it. */
        if (same_bytes(p, n, pm->req_raw, pm->req_len)) {
            m_send_commit(pm, out);
        } else {
            m_end(pm, now, PLINK_FAIL_INTERRUPTED, true, out);
        }
        return;
    }
    if (pm->ended && from_peer && req.attempt == pm->ended_attempt) {
        m_ended(pm, from, out);
        return;
    }
    if (!pm->window) {
        send_fail(out, from, pm->net_id, true, req.attempt, PLINK_FAIL_CLOSED);
        return;
    }
    if (pm->have_peer && !from_peer) {
        m_overlap(pm, now, from, req.attempt, out);
        return;
    }
    if (m_busy(pm)) {
        /* Another attempt from the same keypad: it gave up on the one under way. */
        m_end(pm, now, PLINK_FAIL_INTERRUPTED, true, out);
        return;
    }
    if (now < pm->next_attempt_at) {
        return; /* 3 s between attempts: the keypad's resend comes back */
    }
    m_start(pm, now, from, &req, p, n, out);
}

static void m_nonce(plink_pair_m_t *pm, uint64_t now, const plink_addr_t *from,
                    const plink_hdr_t *h, const uint8_t *p, size_t n, plink_out_t *out)
{
    plink_pair_nonce_t msg;
    if (h->net_id != pm->net_id || !pm->have_peer || !plink_addr_equal(from, &pm->peer) ||
        !plink_dec_pair_nonce(p, n, &msg)) {
        return;
    }
    if (!m_busy(pm) || msg.attempt != pm->attempt) {
        if (pm->ended && msg.attempt == pm->ended_attempt) {
            m_ended(pm, from, out);
        }
        return;
    }
    if (pm->revealed) {
        /* n_k was taken: the same PAIR_NONCE gets PAIR_REVEAL again, another one ends it. */
        if (same_bytes(p, n, pm->nonce_raw, pm->nonce_len)) {
            (void)send_nonce(out, &pm->peer, PLINK_T_PAIR_REVEAL, pm->net_id, true, pm->attempt,
                             pm->n_m);
        } else {
            m_end(pm, now, PLINK_FAIL_INTERRUPTED, true, out);
        }
        return;
    }
    memcpy(pm->nonce_raw, p, n);
    pm->nonce_len = n;
    memcpy(pm->n_k, msg.n, PLINK_NONCE_LEN);
    /* Everything is known now: the keys and the code are computed once, before n_m goes. */
    if (!plink_pair_keys(pm->dh, pm->req_raw, pm->req_len, pm->commit_raw, pm->commit_len,
                         pm->net_id, pm->n_k, pm->n_m, pm->k_pair, pm->k_conf)) {
        m_end(pm, now, PLINK_FAIL_BUSY, true, out);
        return;
    }
    plink_pair_code(pm->pk_k, pm->pk_m, pm->n_k, pm->n_m, pm->code);
    /* LINK-016: the attempt counts from here, however it ends. */
    pm->revealed = true;
    pm->attempts = (uint8_t)(pm->attempts + 1u);
    pm->state = PLINK_PS_COMPARE;
    pm->step_until = now + PLINK_PAIR_COMPARE_MS;
    (void)send_nonce(out, &pm->peer, PLINK_T_PAIR_REVEAL, pm->net_id, true, pm->attempt, pm->n_m);
}

static void m_confirm(plink_pair_m_t *pm, uint64_t now, const plink_addr_t *from,
                      const plink_hdr_t *h, const uint8_t *frame, size_t len, plink_out_t *out)
{
    const uint8_t *p = &frame[PLINK_HDR_LEN];
    const size_t n = len - PLINK_HDR_LEN;
    plink_pair_confirm_t msg;
    if (h->net_id != pm->net_id || !plink_dec_pair_confirm(p, n, &msg)) {
        return;
    }
    /* PAIR_DONE was lost: the same PAIR_CONFIRM gets it again for 60 s, window or not. */
    if (pm->done_len > 0u && now < pm->done_until && plink_addr_equal(from, &pm->done_peer) &&
        same_bytes(p, n, pm->done_confirm, pm->done_confirm_len)) {
        memcpy(out->buf, pm->done_frame, pm->done_len);
        out->len = pm->done_len;
        out->to = *from;
        return;
    }
    if (!pm->have_peer || !plink_addr_equal(from, &pm->peer)) {
        return;
    }
    if (!m_busy(pm) || msg.attempt != pm->attempt) {
        if (pm->ended && msg.attempt == pm->ended_attempt) {
            m_ended(pm, from, out);
        }
        return;
    }
    if (!pm->revealed) {
        return; /* before the code was shown: no keypad of ours sent it */
    }
    if (pm->confirmed) {
        /* The same again waits for its answer, PAIR_DONE, after the installer; another ends it. */
        if (!same_bytes(p, n, pm->confirm_raw, pm->confirm_len)) {
            m_end(pm, now, PLINK_FAIL_INTERRUPTED, true, out);
        }
        return;
    }
    if (!plink_mac_check(pm->k_conf, frame, len)) {
        m_end(pm, now, PLINK_FAIL_MISMATCH, true, out);
        return;
    }
    memcpy(pm->confirm_raw, p, n);
    pm->confirm_len = n;
    pm->confirmed = true;
}

static void m_fail(plink_pair_m_t *pm, uint64_t now, const plink_addr_t *from,
                   const plink_hdr_t *h, const uint8_t *p, size_t n)
{
    plink_pair_fail_t msg;
    if ((h->net_id != 0u && h->net_id != pm->net_id) || !plink_dec_pair_fail(p, n, &msg)) {
        return;
    }
    /* Honoured only for the attempt under way, from its keypad. */
    if (!m_busy(pm) || msg.attempt != pm->attempt || !plink_addr_equal(from, &pm->peer)) {
        return;
    }
    m_end(pm, now, msg.reason != PLINK_FAIL_NONE ? msg.reason : PLINK_FAIL_CANCELLED, false, NULL);
}

void plink_pm_init(plink_pair_m_t *pm, const plink_crypto_t *c)
{
    if (pm == NULL) {
        return;
    }
    plink_wipe(pm, sizeof *pm);
    pm->c = c;
    pm->state = PLINK_PS_IDLE;
}

void plink_pm_identity(plink_pair_m_t *pm, uint16_t net_id, const char *name,
                       const char panel_id[PLINK_PANEL_ID_LEN + 1], uint16_t zones_used)
{
    if (pm == NULL) {
        return;
    }
    pm->net_id = net_id;
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), pm->name, sizeof pm->name);
    plink_clean_text(panel_id, bounded_len(panel_id, PLINK_PANEL_ID_LEN), pm->panel_id,
                     sizeof pm->panel_id);
    pm->zones_used = zones_used;
}

void plink_pm_open(plink_pair_m_t *pm, uint64_t now_ms)
{
    if (pm == NULL || pm->net_id == 0u) {
        return; /* no network to pair into yet */
    }
    /* A fresh window: an attempt still under way is dropped (its keypad times out). */
    m_wipe(pm);
    memset(&pm->req, 0, sizeof pm->req);
    pm->window = true;
    pm->window_until = now_ms + PLINK_PAIR_WINDOW_MS;
    pm->next_attempt_at = now_ms;
    pm->attempts = 0u;
    pm->have_peer = false;
    pm->ended = false;
    pm->state = PLINK_PS_OPEN;
    pm->fail = PLINK_FAIL_NONE;
    pm->device = 0u;
    pm->home = 0u;
}

void plink_pm_cancel(plink_pair_m_t *pm, uint64_t now_ms, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pm == NULL) {
        return;
    }
    if (m_busy(pm)) {
        m_end(pm, now_ms, PLINK_FAIL_CANCELLED, true, out);
    } else {
        m_wipe(pm);
    }
    pm->window = false;
    pm->state = PLINK_PS_IDLE;
    pm->fail = PLINK_FAIL_NONE;
}

void plink_pm_input(plink_pair_m_t *pm, uint64_t now_ms, const plink_addr_t *from,
                    const plink_hdr_t *h, const uint8_t *frame, size_t len, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pm == NULL || from == NULL || h == NULL || frame == NULL || len < PLINK_HDR_LEN ||
        len > PLINK_FRAME_MAX || pm->net_id == 0u) {
        return; /* (a frame plink_hdr_read would have dropped: every payload fits a *_raw) */
    }
    /* A keypad pairs as sender 0xFFFF (docs/features/11 "Pairing" 9), whatever it was before. */
    if (h->sender != PLINK_SENDER_UNPAIRED || (h->flags & PLINK_FLAG_FROM_MASTER) != 0u) {
        return;
    }
    const uint8_t *p = &frame[PLINK_HDR_LEN];
    const size_t n = len - PLINK_HDR_LEN;
    switch (h->type) {
    case PLINK_T_PAIR_REQ: m_req(pm, now_ms, from, h, p, n, out); break;
    case PLINK_T_PAIR_NONCE: m_nonce(pm, now_ms, from, h, p, n, out); break;
    case PLINK_T_PAIR_CONFIRM: m_confirm(pm, now_ms, from, h, frame, len, out); break;
    case PLINK_T_PAIR_FAIL: m_fail(pm, now_ms, from, h, p, n); break;
    default: break;
    }
}

void plink_pm_tick(plink_pair_m_t *pm, uint64_t now_ms, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pm == NULL) {
        return;
    }
    if (pm->done_len > 0u && now_ms >= pm->done_until) {
        plink_wipe(pm->done_frame, sizeof pm->done_frame);
        plink_wipe(pm->done_confirm, sizeof pm->done_confirm);
        pm->done_len = pm->done_confirm_len = 0u;
    }
    if (m_busy(pm)) {
        /* 30 s per step, 2 min to compare; an attempt under way outlives its window. */
        if (now_ms >= pm->step_until) {
            m_end(pm, now_ms, PLINK_FAIL_TIMEOUT, true, out);
        }
        return;
    }
    if (pm->window && now_ms >= pm->window_until) {
        pm->window = false;
        if (pm->state == PLINK_PS_OPEN) {
            pm->state = PLINK_PS_FAILED; /* nobody came: a failed attempt keeps its own reason */
            pm->fail = PLINK_FAIL_TIMEOUT;
        }
    }
}

void plink_pm_decide(plink_pair_m_t *pm, uint64_t now_ms, bool allow, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pm == NULL || pm->state != PLINK_PS_COMPARE) {
        return;
    }
    if (!allow) {
        m_end(pm, now_ms, PLINK_FAIL_REFUSED, true, out);
        return;
    }
    pm->allowed = true;
    pm->state = PLINK_PS_WAITING;
}

bool plink_pm_take(const plink_pair_m_t *pm, plink_pair_new_t *out)
{
    if (pm == NULL || pm->state != PLINK_PS_WAITING || !pm->allowed || !pm->confirmed) {
        return false;
    }
    if (out != NULL) {
        memset(out, 0, sizeof *out);
        memcpy(out->k_pair, pm->k_pair, PLINK_KEY_LEN);
        out->kind = pm->req.kind;
        out->cap = pm->req.cap;
        memcpy(out->name, pm->req.name, sizeof out->name);
    }
    return true;
}

void plink_pm_stored(plink_pair_m_t *pm, uint64_t now_ms, bool ok, uint8_t fail, uint8_t device,
                     uint8_t home, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (!plink_pm_take(pm, NULL)) {
        return;
    }
    if (ok && (device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST)) {
        ok = false;
        fail = PLINK_FAIL_STORE;
    }
    plink_pair_done_t d;
    memset(&d, 0, sizeof d);
    d.attempt = pm->attempt;
    d.device = device;
    d.home = home;
    size_t n = 0u;
    if (ok && (!plink_enc_pair_done(&d, &out->buf[PLINK_HDR_LEN], PAYLOAD_CAP, &n) ||
               !finish(out, &pm->peer, PLINK_T_PAIR_DONE, pm->net_id, true, n) ||
               out->len > sizeof pm->done_frame)) {
        ok = false;
        fail = PLINK_FAIL_STORE;
    }
    if (!ok) {
        m_end(pm, now_ms, fail != PLINK_FAIL_NONE ? fail : PLINK_FAIL_STORE, true, out);
        pm->window = false;
        return;
    }
    /* mac_m covers the frame up to it, the number and the home partition included; the frame is
     * kept 60 s for a PAIR_CONFIRM whose PAIR_DONE was lost. */
    plink_mac_fill(pm->k_conf, out->buf, out->len);
    memcpy(pm->done_frame, out->buf, out->len);
    pm->done_len = out->len;
    pm->done_until = now_ms + PLINK_PAIR_DONE_KEEP_MS;
    pm->done_peer = pm->peer;
    memcpy(pm->done_confirm, pm->confirm_raw, pm->confirm_len);
    pm->done_confirm_len = pm->confirm_len;
    pm->device = device;
    pm->home = home;
    m_wipe(pm);
    pm->ended = true;
    pm->ended_attempt = pm->attempt;
    pm->ended_fail = PLINK_FAIL_NONE;
    pm->window = false;
    pm->state = PLINK_PS_DONE;
    pm->fail = PLINK_FAIL_NONE;
}

void plink_pm_info(const plink_pair_m_t *pm, uint64_t now_ms, plink_pair_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (pm == NULL) {
        return;
    }
    out->state = pm->state;
    out->fail = pm->state == PLINK_PS_FAILED ? pm->fail : (uint8_t)PLINK_FAIL_NONE;
    out->attempts = pm->attempts;
    out->left_s = pm->window ? secs_left(now_ms, pm->window_until) : 0u;
    if (pm->state == PLINK_PS_COMPARE || pm->state == PLINK_PS_WAITING) {
        memcpy(out->code, pm->code, sizeof out->code);
    }
    if (pm->have_peer) {
        memcpy(out->peer_name, pm->req.name, sizeof out->peer_name);
        out->peer = pm->peer;
    }
    out->device = pm->state == PLINK_PS_DONE ? pm->device : 0u;
}

uint64_t plink_pm_next(const plink_pair_m_t *pm)
{
    if (pm == NULL) {
        return UINT64_MAX;
    }
    uint64_t t = UINT64_MAX;
    if (m_busy(pm)) {
        t = pm->step_until;
    } else if (pm->window) {
        t = pm->window_until;
    }
    if (pm->done_len > 0u && pm->done_until < t) {
        t = pm->done_until;
    }
    return t;
}

/* ---- the keypad's side -------------------------------------------------------------------- */

static bool k_busy(const plink_pair_k_t *pk)
{
    return pk->state == PLINK_PS_OPEN || pk->state == PLINK_PS_WORKING ||
           pk->state == PLINK_PS_COMPARE || pk->state == PLINK_PS_WAITING;
}

/* The attempt's secrets and the values it took; the master's name (shown) stays. */
static void k_wipe(plink_pair_k_t *pk)
{
    plink_wipe(pk->sk, sizeof pk->sk);
    plink_wipe(pk->dh, sizeof pk->dh);
    plink_wipe(pk->pk_k, sizeof pk->pk_k);
    plink_wipe(pk->n_k, sizeof pk->n_k);
    plink_wipe(pk->pk_m, sizeof pk->pk_m);
    plink_wipe(pk->n_m, sizeof pk->n_m);
    plink_wipe(pk->commit, sizeof pk->commit);
    plink_wipe(pk->k_pair, sizeof pk->k_pair);
    plink_wipe(pk->k_conf, sizeof pk->k_conf);
    plink_wipe(pk->code, sizeof pk->code);
    plink_wipe(pk->req_raw, sizeof pk->req_raw);
    plink_wipe(pk->commit_raw, sizeof pk->commit_raw);
    plink_wipe(pk->reveal_raw, sizeof pk->reveal_raw);
    plink_wipe(pk->last, pk->last_len);
    pk->req_len = pk->commit_len = pk->reveal_len = pk->last_len = 0u;
    pk->nonce_sent = pk->revealed = pk->confirmed = false;
}

/* The frame in out becomes the one resent until answered: after 1 s, doubling to 4 s. */
static void k_keep(plink_pair_k_t *pk, uint64_t now, const plink_out_t *out)
{
    memcpy(pk->last, out->buf, out->len);
    pk->last_len = out->len;
    pk->resend_ms = PLINK_PAIR_RESEND_MS;
    pk->resend_at = now + PLINK_PAIR_RESEND_MS;
}

/* Ends the attempt with reason, told to the master when tell. One that sent n_k stays counted. */
static void k_end(plink_pair_k_t *pk, uint64_t now, uint8_t reason, bool tell, plink_out_t *out)
{
    if (tell) {
        send_fail(out, &pk->master, pk->net_id, false, pk->attempt, reason);
    }
    k_wipe(pk);
    pk->state = PLINK_PS_FAILED;
    pk->fail = reason;
    pk->next_attempt_at = now + PLINK_PAIR_GAP_MS;
}

/* A new attempt: a fresh attempt id and key pair, PAIR_REQ to the master (held until 3 s after
 * the last attempt ended). */
static void k_attempt(plink_pair_k_t *pk, uint64_t now, plink_out_t *out)
{
    k_wipe(pk);
    memset(&pk->commit_msg, 0, sizeof pk->commit_msg);
    plink_wipe(&pk->result, sizeof pk->result);
    pk->have_result = false;
    pk->device = 0u;
    pk->net_id = pk->net_hint; /* a net id learned from a COMMIT never outlives its attempt */
    pk->fail = PLINK_FAIL_NONE;
    if (pk->by_broadcast) {
        /* Asked by broadcast: so is every attempt, and its first PAIR_COMMIT names its master. */
        pk->master = pk->asked;
        pk->found = false;
        pk->heard_fail = PLINK_FAIL_NONE;
    }
    uint8_t a[4];
    bool ok = plink_random(pk->c, a, sizeof a) && plink_x25519_keypair(pk->c, pk->sk, pk->pk_k);
    if (ok) {
        pk->attempt = (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) |
                      ((uint32_t)a[3] << 24);
        if (pk->attempt == 0u) {
            pk->attempt = 1u;
        }
        plink_pair_req_t req;
        memset(&req, 0, sizeof req);
        req.attempt = pk->attempt;
        req.proto = PLINK_PROTO;
        req.caps = PLINK_CAPS;
        req.kind = pk->kind;
        req.cap = pk->cap;
        memcpy(req.pk, pk->pk_k, PLINK_X25519_LEN);
        memcpy(req.name, pk->name, sizeof req.name);
        memcpy(req.fw, pk->fw, sizeof req.fw);
        ok = plink_enc_pair_req(&req, pk->req_raw, sizeof pk->req_raw, &pk->req_len);
    }
    if (ok) {
        memcpy(&out->buf[PLINK_HDR_LEN], pk->req_raw, pk->req_len);
        ok = finish(out, &pk->master, PLINK_T_PAIR_REQ, pk->net_id, false, pk->req_len);
    }
    if (!ok) {
        k_end(pk, now, PLINK_FAIL_CANCELLED, false, out); /* the port failed: nothing went */
        out->len = 0u;
        return;
    }
    pk->state = PLINK_PS_OPEN;
    k_keep(pk, now, out);
    if (now < pk->next_attempt_at) {
        out->len = 0u;
        pk->resend_ms = 0u; /* the first send, then 1 s ... */
        pk->resend_at = pk->next_attempt_at;
    }
    pk->step_until = (now < pk->next_attempt_at ? pk->next_attempt_at : now) + PLINK_PAIR_STEP_MS;
}

static void k_commit(plink_pair_k_t *pk, uint64_t now, const plink_addr_t *from,
                     const plink_hdr_t *h, const uint8_t *p, size_t n, plink_out_t *out)
{
    plink_pair_commit_t msg;
    if (!plink_dec_pair_commit(p, n, &msg) || msg.attempt != pk->attempt || h->net_id == 0u ||
        (pk->net_hint != 0u && h->net_id != pk->net_hint)) {
        return; /* not for this attempt, or another network's master */
    }
    if (pk->by_broadcast && !pk->found) {
        /* Asked by broadcast (LINK-021): the first master to answer this attempt is its master,
         * fixed here, before n_k goes; from now on everything must come from that address. */
        pk->master = *from;
        pk->found = true;
    } else if (!plink_addr_equal(from, &pk->master)) {
        /* A second master answered: nothing more goes to either. Asked by broadcast, the
         * PAIR_FAIL goes as the request did - by broadcast, for any network - so both masters
         * end the attempt. */
        if (pk->by_broadcast) {
            k_end(pk, now, PLINK_FAIL_OVERLAP, false, out);
            send_fail(out, &pk->asked, pk->net_hint, false, pk->attempt, PLINK_FAIL_OVERLAP);
        } else {
            k_end(pk, now, PLINK_FAIL_OVERLAP, true, out);
        }
        return;
    }
    if (pk->commit_len > 0u) {
        /* pk_m and the commitment were taken: the same PAIR_COMMIT gets PAIR_NONCE again,
         * another one ends the attempt. */
        if (same_bytes(p, n, pk->commit_raw, pk->commit_len) && h->net_id == pk->net_id) {
            (void)send_nonce(out, &pk->master, PLINK_T_PAIR_NONCE, pk->net_id, false, pk->attempt,
                             pk->n_k);
        } else {
            k_end(pk, now, PLINK_FAIL_INTERRUPTED, true, out);
        }
        return;
    }
    if (msg.proto == 0u) {
        k_end(pk, now, PLINK_FAIL_VERSION, true, out); /* no version in common */
        return;
    }
    memcpy(pk->commit_raw, p, n);
    pk->commit_len = n;
    pk->commit_msg = msg;
    memcpy(pk->pk_m, msg.pk, PLINK_X25519_LEN);
    memcpy(pk->commit, msg.commit, PLINK_SHA256_LEN);
    pk->net_id = h->net_id;
    if (!plink_x25519_shared(pk->c, pk->sk, pk->pk_m, pk->dh)) {
        k_end(pk, now, PLINK_FAIL_MISMATCH, true, out); /* a key of small order */
        return;
    }
    if (!plink_random(pk->c, pk->n_k, PLINK_NONCE_LEN)) {
        k_end(pk, now, PLINK_FAIL_CANCELLED, true, out);
        return;
    }
    /* LINK-016: n_k goes only now that the master is bound to n_m; the attempt counts from here. */
    pk->nonce_sent = true;
    pk->attempts = (uint8_t)(pk->attempts + 1u);
    pk->state = PLINK_PS_WORKING;
    pk->step_until = now + PLINK_PAIR_STEP_MS;
    if (send_nonce(out, &pk->master, PLINK_T_PAIR_NONCE, pk->net_id, false, pk->attempt,
                   pk->n_k)) {
        k_keep(pk, now, out);
    }
}

static void k_reveal(plink_pair_k_t *pk, uint64_t now, const plink_addr_t *from,
                     const plink_hdr_t *h, const uint8_t *p, size_t n, plink_out_t *out)
{
    plink_pair_nonce_t msg;
    if (!plink_dec_pair_nonce(p, n, &msg) || msg.attempt != pk->attempt ||
        !plink_addr_equal(from, &pk->master) || h->net_id != pk->net_id || !pk->nonce_sent) {
        return;
    }
    if (pk->revealed) {
        /* The same PAIR_REVEAL gets PAIR_CONFIRM again once the person said yes; another one
         * ends the attempt. */
        if (!same_bytes(p, n, pk->reveal_raw, pk->reveal_len)) {
            k_end(pk, now, PLINK_FAIL_INTERRUPTED, true, out);
        } else if (pk->confirmed && pk->last_len > 0u) {
            memcpy(out->buf, pk->last, pk->last_len);
            out->len = pk->last_len;
            out->to = pk->master;
        }
        return;
    }
    /* n_m must be the one the master committed to before it saw n_k. */
    uint8_t c[PLINK_SHA256_LEN];
    plink_pair_commitment(pk->pk_m, pk->pk_k, msg.n, c);
    const bool kept = plink_ct_equal(c, pk->commit, sizeof c);
    plink_wipe(c, sizeof c);
    if (!kept) {
        k_end(pk, now, PLINK_FAIL_MISMATCH, true, out);
        return;
    }
    memcpy(pk->reveal_raw, p, n);
    pk->reveal_len = n;
    memcpy(pk->n_m, msg.n, PLINK_NONCE_LEN);
    if (!plink_pair_keys(pk->dh, pk->req_raw, pk->req_len, pk->commit_raw, pk->commit_len,
                         pk->net_id, pk->n_k, pk->n_m, pk->k_pair, pk->k_conf)) {
        k_end(pk, now, PLINK_FAIL_CANCELLED, true, out);
        return;
    }
    plink_pair_code(pk->pk_k, pk->pk_m, pk->n_k, pk->n_m, pk->code);
    pk->revealed = true;
    pk->state = PLINK_PS_COMPARE;
    pk->step_until = now + PLINK_PAIR_COMPARE_MS;
    plink_wipe(pk->last, pk->last_len); /* PAIR_NONCE was answered: nothing to resend */
    pk->last_len = 0u;
}

static void k_done(plink_pair_k_t *pk, uint64_t now, const plink_addr_t *from,
                   const plink_hdr_t *h, const uint8_t *frame, size_t len, plink_out_t *out)
{
    plink_pair_done_t msg;
    if (pk->state != PLINK_PS_WAITING ||
        !plink_dec_pair_done(&frame[PLINK_HDR_LEN], len - PLINK_HDR_LEN, &msg) ||
        msg.attempt != pk->attempt || !plink_addr_equal(from, &pk->master) ||
        h->net_id != pk->net_id) {
        return;
    }
    if (!plink_mac_check(pk->k_conf, frame, len) || msg.device < PLINK_DEVICE_FIRST ||
        msg.device > PLINK_DEVICE_LAST || msg.home < 1u || msg.home > PLINK_PARTS_MAX) {
        k_end(pk, now, PLINK_FAIL_MISMATCH, true, out);
        return;
    }
    plink_keypad_rec_t *r = &pk->result;
    memset(r, 0, sizeof *r);
    r->net_id = pk->net_id;
    r->device = msg.device;
    r->home = msg.home;
    memcpy(r->k_pair, pk->k_pair, PLINK_KEY_LEN);
    memcpy(r->master_name, pk->commit_msg.name, sizeof r->master_name);
    memcpy(r->panel_id, pk->commit_msg.panel_id, sizeof r->panel_id);
    pk->have_result = true;
    pk->device = msg.device;
    k_wipe(pk);
    pk->state = PLINK_PS_DONE;
    pk->fail = PLINK_FAIL_NONE;
}

static void k_fail(plink_pair_k_t *pk, uint64_t now, const plink_addr_t *from,
                   const plink_hdr_t *h, const uint8_t *p, size_t n)
{
    plink_pair_fail_t msg;
    if (!plink_dec_pair_fail(p, n, &msg) || msg.attempt != pk->attempt) {
        return;
    }
    if (pk->by_broadcast && !pk->found) {
        /* Asked by broadcast, no master yet: one panel's refusal doesn't speak for another that
         * may still answer. It only names the reason if nobody does. */
        if (msg.reason != PLINK_FAIL_NONE) {
            pk->heard_fail = msg.reason;
        }
        return;
    }
    /* Honoured only for the attempt under way, from its master. */
    if (!plink_addr_equal(from, &pk->master) || (pk->net_id != 0u && h->net_id != pk->net_id)) {
        return;
    }
    k_end(pk, now, msg.reason != PLINK_FAIL_NONE ? msg.reason : PLINK_FAIL_CANCELLED, false, NULL);
}

void plink_pk_init(plink_pair_k_t *pk, const plink_crypto_t *c, uint8_t kind,
                   const plink_caps_t *cap, const char *name, const char *fw)
{
    if (pk == NULL) {
        return;
    }
    plink_wipe(pk, sizeof *pk);
    pk->c = c;
    pk->kind = kind;
    if (cap != NULL) {
        pk->cap = *cap;
    }
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), pk->name, sizeof pk->name);
    plink_clean_text(fw, bounded_len(fw, PLINK_FW_MAX), pk->fw, sizeof pk->fw);
    pk->state = PLINK_PS_IDLE;
}

void plink_pk_start(plink_pair_k_t *pk, uint64_t now_ms, const plink_addr_t *master,
                    bool broadcast, uint16_t net_id, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pk == NULL || master == NULL) {
        return;
    }
    /* A new pairing of up to 3 attempts. One under way is dropped: its PAIR_REQ's successor ends
     * it at the master. */
    pk->master = *master;
    pk->asked = *master;
    pk->by_broadcast = broadcast || plink_addr_is_broadcast(master);
    pk->found = false;
    pk->heard_fail = PLINK_FAIL_NONE;
    pk->net_hint = net_id;
    pk->attempts = 0u;
    pk->next_attempt_at = now_ms;
    k_attempt(pk, now_ms, out);
}

void plink_pk_retry(plink_pair_k_t *pk, uint64_t now_ms, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pk == NULL || pk->state != PLINK_PS_FAILED) {
        return;
    }
    if (pk->attempts >= PLINK_PAIR_ATTEMPTS) {
        pk->fail = PLINK_FAIL_LIMIT;
        return;
    }
    k_attempt(pk, now_ms, out);
}

void plink_pk_input(plink_pair_k_t *pk, uint64_t now_ms, const plink_addr_t *from,
                    const plink_hdr_t *h, const uint8_t *frame, size_t len, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    /* Pairing traffic counts only while a person is pairing (docs/features/11 "Pairing" 9). */
    if (pk == NULL || from == NULL || h == NULL || frame == NULL || len < PLINK_HDR_LEN ||
        len > PLINK_FRAME_MAX || !k_busy(pk) || h->sender != PLINK_SENDER_MASTER ||
        (h->flags & PLINK_FLAG_FROM_MASTER) == 0u) {
        return;
    }
    const uint8_t *p = &frame[PLINK_HDR_LEN];
    const size_t n = len - PLINK_HDR_LEN;
    switch (h->type) {
    case PLINK_T_PAIR_COMMIT: k_commit(pk, now_ms, from, h, p, n, out); break;
    case PLINK_T_PAIR_REVEAL: k_reveal(pk, now_ms, from, h, p, n, out); break;
    case PLINK_T_PAIR_DONE: k_done(pk, now_ms, from, h, frame, len, out); break;
    case PLINK_T_PAIR_FAIL: k_fail(pk, now_ms, from, h, p, n); break;
    default: break;
    }
}

void plink_pk_tick(plink_pair_k_t *pk, uint64_t now_ms, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pk == NULL || !k_busy(pk)) {
        return;
    }
    if (now_ms >= pk->step_until) {
        const uint8_t heard = pk->by_broadcast && !pk->found ? pk->heard_fail : PLINK_FAIL_NONE;
        k_end(pk, now_ms, PLINK_FAIL_TIMEOUT, true, out);
        if (heard != PLINK_FAIL_NONE) {
            pk->fail = heard; /* no master answered but this one, which said why not */
        }
        return;
    }
    if (pk->last_len > 0u && now_ms >= pk->resend_at) {
        memcpy(out->buf, pk->last, pk->last_len);
        out->len = pk->last_len;
        out->to = pk->master;
        if (pk->resend_ms == 0u) {
            pk->resend_ms = PLINK_PAIR_RESEND_MS; /* that was the first send */
        } else if (pk->resend_ms >= PLINK_PAIR_RESEND_MAX_MS / 2u) {
            pk->resend_ms = PLINK_PAIR_RESEND_MAX_MS;
        } else {
            pk->resend_ms *= 2u;
        }
        pk->resend_at = now_ms + pk->resend_ms;
    }
}

void plink_pk_confirm(plink_pair_k_t *pk, uint64_t now_ms, bool match, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pk == NULL || pk->state != PLINK_PS_COMPARE) {
        return;
    }
    if (!match) {
        k_end(pk, now_ms, PLINK_FAIL_REFUSED, true, out);
        return;
    }
    /* mac_k = HMAC(K_conf, the frame up to it)[0..15]. */
    plink_pair_confirm_t cf;
    memset(&cf, 0, sizeof cf);
    cf.attempt = pk->attempt;
    size_t n = 0u;
    if (!plink_enc_pair_confirm(&cf, &out->buf[PLINK_HDR_LEN], PAYLOAD_CAP, &n) ||
        !finish(out, &pk->master, PLINK_T_PAIR_CONFIRM, pk->net_id, false, n)) {
        k_end(pk, now_ms, PLINK_FAIL_CANCELLED, true, out);
        return;
    }
    plink_mac_fill(pk->k_conf, out->buf, out->len);
    k_keep(pk, now_ms, out);
    pk->confirmed = true;
    pk->state = PLINK_PS_WAITING;
}

void plink_pk_cancel(plink_pair_k_t *pk, uint64_t now_ms, plink_out_t *out)
{
    if (out == NULL) {
        return;
    }
    out->len = 0u;
    if (pk == NULL || (pk->state == PLINK_PS_DONE && pk->have_result)) {
        return; /* paired: the manager takes the master next */
    }
    if (k_busy(pk)) {
        k_end(pk, now_ms, PLINK_FAIL_CANCELLED, true, out);
    } else {
        k_wipe(pk);
    }
    pk->state = PLINK_PS_IDLE;
    pk->fail = PLINK_FAIL_NONE;
}

bool plink_pk_take(plink_pair_k_t *pk, plink_keypad_rec_t *out)
{
    if (pk == NULL || out == NULL || !pk->have_result) {
        return false;
    }
    *out = pk->result;
    plink_wipe(&pk->result, sizeof pk->result);
    pk->have_result = false;
    return true;
}

void plink_pk_info(const plink_pair_k_t *pk, uint64_t now_ms, plink_pair_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (pk == NULL) {
        return;
    }
    out->state = pk->state;
    out->fail = pk->state == PLINK_PS_FAILED ? pk->fail : (uint8_t)PLINK_FAIL_NONE;
    out->attempts = pk->attempts;
    out->left_s = k_busy(pk) ? secs_left(now_ms, pk->step_until) : 0u;
    if (pk->state == PLINK_PS_COMPARE || pk->state == PLINK_PS_WAITING) {
        memcpy(out->code, pk->code, sizeof out->code);
    }
    memcpy(out->peer_name, pk->commit_msg.name, sizeof out->peer_name);
    out->peer = pk->master;
    out->device = pk->state == PLINK_PS_DONE ? pk->device : 0u;
}

uint64_t plink_pk_next(const plink_pair_k_t *pk)
{
    if (pk == NULL || !k_busy(pk)) {
        return UINT64_MAX;
    }
    uint64_t t = pk->step_until;
    if (pk->last_len > 0u && pk->resend_at < t) {
        t = pk->resend_at;
    }
    return t;
}
