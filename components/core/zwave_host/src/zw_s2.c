/*
 * zw_s2.c - the S2 security manager (security.md §6-9, command-classes.md §1). See zw_s2.h.
 */
#include "zwave_host/zw_s2.h"

#include <string.h>

#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"

/* A peer's SPAN (§6), short names. */
#define SPAN_NONE   ZW_S2_SPAN_NONE
#define SPAN_LOCAL  ZW_S2_SPAN_LOCAL_EI
#define SPAN_REMOTE ZW_S2_SPAN_REMOTE_EI
#define SPAN_READY  ZW_S2_SPAN_READY

/* A send's progress. */
enum {
    S_FREE = 0,
    S_QUEUED,     /* waiting for an earlier send to the same node */
    S_NONCE_GET,  /* its Nonce Get handed to the radio */
    S_WAIT_NONCE, /* the Nonce Get delivered: the Nonce Report is awaited */
    S_TX,         /* the frame handed to the radio */
    S_GRACE,      /* delivered: a Nonce Report now means it was not decoded (§6) */
    S_JAM_WAIT    /* the module was jammed: the frame goes again at the deadline (jam_state) */
};

/* Key slots. */
enum { K_UNAUTH = 0, K_AUTH, K_ACCESS, K_S0, K_LR_AUTH, K_LR_ACCESS, K_COUNT };

/* Tags on the frames handed to port.send: a marker, what it is, the stamp, the index. */
#define TAG_MARK         0x50000000u
#define TAG_MARK_MASK    0xF0000000u
#define TAG_NONCE_REPORT 1u
#define TAG_SEND         2u

/* §9: what the panel supports securely - never Security, Security 2 or Transport Service, and
 * none of it in the panel's node information (serial-api.md §6.2: 5E 9F 6C 56 8F). */
static const uint8_t PANEL_SECURE_CCS[] = {0x86u, 0x72u, 0x85u, 0x59u, 0x8Eu, 0x5Au};

/* S0 encapsulation (security.md §10), recognised to be dropped (phase 3). */
#define S0_ENCAP        0x81u
#define S0_ENCAP_NONCE  0xC1u
#define S0_SCHEME_GET   0x04u
#define S0_SCHEME_REPORT 0x05u
#define S0_NONCE_GET    0x40u
#define S0_NONCE_REPORT 0x80u

/* ---- Small helpers ------------------------------------------------------------------------- */

static uint32_t make_tag(unsigned kind, uint16_t stamp, size_t idx)
{
    return TAG_MARK | (uint32_t)kind << 24 | (uint32_t)stamp << 8 | (uint32_t)(idx & 0xFFu);
}

static uint16_t next_stamp(zw_s2_t *s2)
{
    s2->tx_gen = (uint16_t)((unsigned)s2->tx_gen + 1u);
    return s2->tx_gen;
}

static bool bit_test(const uint8_t map[32], uint8_t cc)
{
    return (((unsigned)map[cc >> 3] >> ((unsigned)cc & 7u)) & 1u) != 0u;
}

static void bits_add_list(uint8_t map[32], const uint8_t *ccs, size_t n)
{
    if (ccs == NULL || n == 0u) {
        return;
    }
    zw_cc_iter_t it;
    uint16_t cc = 0u;
    bool controlled = false;
    zw_cc_iter_init(&it, ccs, n);
    while (zw_cc_iter_next(&it, &cc, &controlled)) {
        if (!controlled && cc <= 0xFFu) { /* supported, one-byte identifiers (ours all are) */
            map[cc >> 3] = (uint8_t)((unsigned)map[cc >> 3] | 1u << ((unsigned)cc & 7u));
        }
    }
}

static void bits_from_list(uint8_t map[32], const uint8_t *ccs, size_t n)
{
    memset(map, 0, 32u);
    bits_add_list(map, ccs, n);
}

static uint8_t class_bit(zw_sec_class_t cls)
{
    switch (cls) {
    case ZW_SEC_S2_UNAUTH: return ZW_S2_KEY_UNAUTH;
    case ZW_SEC_S2_AUTH: return ZW_S2_KEY_AUTH;
    case ZW_SEC_S2_ACCESS: return ZW_S2_KEY_ACCESS;
    case ZW_SEC_S0: return ZW_S2_KEY_S0;
    default: return 0u;
    }
}

static zw_sec_class_t highest_of(uint8_t granted)
{
    if ((granted & ZW_S2_KEY_ACCESS) != 0u) {
        return ZW_SEC_S2_ACCESS;
    }
    if ((granted & ZW_S2_KEY_AUTH) != 0u) {
        return ZW_SEC_S2_AUTH;
    }
    if ((granted & ZW_S2_KEY_UNAUTH) != 0u) {
        return ZW_SEC_S2_UNAUTH;
    }
    return ((unsigned)granted & ZW_S2_KEY_S0) != 0u ? ZW_SEC_S0 : ZW_SEC_NONE;
}

static bool is_s2_class(uint8_t cls)
{
    return cls == ZW_SEC_S2_UNAUTH || cls == ZW_SEC_S2_AUTH || cls == ZW_SEC_S2_ACCESS;
}

static void event(zw_s2_t *s2, uint16_t node, zw_s2_event_t ev)
{
    if (s2->port.event != NULL) {
        s2->port.event(s2->port.ctx, node, ev);
    }
}

static zw_s2_peer_t *find_peer(zw_s2_t *s2, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_S2_PEERS; i++) {
        if (s2->peers[i].node == node) {
            return &s2->peers[i];
        }
    }
    return NULL;
}

static const zw_s2_peer_t *find_peer_c(const zw_s2_t *s2, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_S2_PEERS; i++) {
        if (s2->peers[i].node == node) {
            return &s2->peers[i];
        }
    }
    return NULL;
}

/* The key slot of a class for a node: Long Range nodes have their own Authenticated and Access
 * Control keys and no Unauthenticated one (§1). -1: none. */
static int key_slot(const zw_s2_t *s2, uint16_t node, uint8_t key_bit)
{
    const bool lr = node >= 256u || s2->own_node >= 256u;
    switch (key_bit) {
    case ZW_S2_KEY_UNAUTH: return lr ? -1 : K_UNAUTH;
    case ZW_S2_KEY_AUTH: return lr ? K_LR_AUTH : K_AUTH;
    case ZW_S2_KEY_ACCESS: return lr ? K_LR_ACCESS : K_ACCESS;
    case ZW_S2_KEY_S0: return K_S0;
    default: return -1;
    }
}

/* The CCM key and personalization string of a class for a node: the temporary key for the node
 * being bootstrapped, else its network key's. */
static bool class_keys(const zw_s2_t *s2, uint16_t node, uint8_t cls, const uint8_t **key_ccm,
                       const uint8_t **pstring)
{
    if (cls == ZW_SEC_TEMP) {
        if (s2->temp_node == 0u || s2->temp_node != node) {
            return false;
        }
        *key_ccm = s2->temp_key_ccm;
        *pstring = s2->temp_pstring;
        return true;
    }
    if (!is_s2_class(cls)) {
        return false;
    }
    const int k = key_slot(s2, node, class_bit((zw_sec_class_t)cls));
    if (k < 0 || !s2->keys[k].present) {
        return false;
    }
    *key_ccm = s2->keys[k].key_ccm;
    *pstring = s2->keys[k].pstring;
    return true;
}

/* Whether the node may use the class: granted (or its temporary key) and the key present. */
static bool class_usable(const zw_s2_t *s2, const zw_s2_peer_t *p, uint8_t cls)
{
    const uint8_t *k = NULL;
    const uint8_t *ps = NULL;
    if (cls == ZW_SEC_TEMP) {
        return class_keys(s2, p->node, cls, &k, &ps);
    }
    return is_s2_class(cls) && (p->granted & class_bit((zw_sec_class_t)cls)) != 0u &&
           class_keys(s2, p->node, cls, &k, &ps);
}

/* §8, a node whose classes are not all known (Z-Wave JS hasSecurityClass(cls) !== false): a class
 * it may hold - granted, or not decided yet - whose key the panel holds. Tried when decrypting
 * (Security2CC.ts decryptSinglecast), and usable for a send at an explicit class (the interview's
 * probe, Security2CC.interview: getSupportedCommands(secClass)). */
static bool class_possible(const zw_s2_t *s2, const zw_s2_peer_t *p, uint8_t cls)
{
    const uint8_t *k = NULL;
    const uint8_t *ps = NULL;
    if (class_usable(s2, p, cls)) {
        return true;
    }
    return p->classes_unknown && is_s2_class(cls) &&
           (p->refused & class_bit((zw_sec_class_t)cls)) == 0u &&
           class_keys(s2, p->node, cls, &k, &ps);
}

static void set_span(zw_s2_peer_t *p, uint8_t state)
{
    p->span_state = state;
    zw_wipe(&p->span, sizeof p->span);
    p->crossed_valid = false;
    zw_wipe(p->crossed_nonce, sizeof p->crossed_nonce);
}

/* §8, receiving (Z-Wave JS Driver.ts isSecurityLevelTooLow): whether a command's CC is one the
 * node lists in its Security Commands Supported Report - what decides for a node whose highest
 * class is S0 (the innermost command decides; Security 0 and Security 2 commands never need it).
 * A node whose highest class is S2 needs its highest class for every command whatever it lists:
 * level_ok_s2. */
static bool cc_listed_secure(const zw_s2_peer_t *p, uint8_t cc)
{
    return cc != ZW_S2_CC && cc != (uint8_t)ZW_CC_SECURITY_0 && p->secure_known &&
           bit_test(p->secure, cc);
}

/* §8, sending (Z-Wave JS Driver.ts isCCSecure): whether a command of CC `cc` goes encrypted at the
 * node's highest class. A node with no class: never. S2: Basic and every CC the node supports
 * (its NIF or its Commands Supported Report) - a CC it does not support goes in the clear, like
 * Z-Wave JS's "avoids silly things like S2-encapsulated pings". S0: Basic and the CCs its secure
 * list names. Security 2's, Security 0's and Transport Service's own commands are never secured
 * here: their class is the caller's (Security2CC.requiresEncapsulation). Multi Channel of an S2
 * node: its end points are reached inside the node's security (zw_s2_class_for_endpoint). */
static bool cc_secure(const zw_s2_t *s2, const zw_s2_peer_t *p, uint8_t cc)
{
    const unsigned keys = (unsigned)p->granted;
    if (cc == ZW_S2_CC || cc == (uint8_t)ZW_CC_SECURITY_0 ||
        cc == (uint8_t)ZW_CC_TRANSPORT_SERVICE) {
        return false;
    }
    if (!s2->cfg.panel) {
        return (keys & ZW_S2_KEYS_S2) != 0u; /* a simulated device: its reports go secure */
    }
    if ((keys & ZW_S2_KEYS_S2) != 0u) {
        return cc == (uint8_t)ZW_CC_BASIC || cc == (uint8_t)ZW_CC_MULTI_CHANNEL ||
               bit_test(p->nif, cc) || (p->secure_known && bit_test(p->secure, cc));
    }
    if ((keys & ZW_S2_KEY_S0) != 0u) {
        return cc == (uint8_t)ZW_CC_BASIC || cc_listed_secure(p, cc);
    }
    return false;
}

/* ---- Sending -------------------------------------------------------------------------------- */

static zw_s2_send_t *active_send(zw_s2_t *s2, uint16_t node)
{
    for (size_t i = 0; i < ZW_S2_SENDS; i++) {
        const uint8_t st = s2->sends[i].state;
        if (s2->sends[i].node == node && st != S_FREE && st != S_QUEUED) {
            return &s2->sends[i];
        }
    }
    return NULL;
}

static bool node_has_queue(const zw_s2_t *s2, uint16_t node)
{
    for (size_t i = 0; i < ZW_S2_SENDS; i++) {
        if (s2->sends[i].node == node && s2->sends[i].state != S_FREE) {
            return true;
        }
    }
    return false;
}

static void start_slot(zw_s2_t *s2, zw_s2_send_t *sl);

/* Ends a send: its slot freed first (send_done may send again), the next one to the node
 * started. */
static void finish(zw_s2_t *s2, zw_s2_send_t *sl, zw_s2_result_t result)
{
    const uint16_t node = sl->node;
    const uint32_t tag = sl->tag;
    const bool internal = sl->internal;
    zw_wipe(sl->plain, sizeof sl->plain);
    zw_wipe(sl->frame, sizeof sl->frame);
    sl->state = S_FREE;
    sl->len = 0u;
    sl->frame_len = 0u;
    if (!internal && s2->port.send_done != NULL) {
        s2->port.send_done(s2->port.ctx, tag, result);
    }
    if (active_send(s2, node) != NULL) {
        return;
    }
    zw_s2_send_t *next = NULL;
    for (size_t i = 0; i < ZW_S2_SENDS; i++) {
        zw_s2_send_t *q = &s2->sends[i];
        if (q->state == S_QUEUED && q->node == node && (next == NULL || q->order < next->order)) {
            next = q;
        }
    }
    if (next != NULL) {
        start_slot(s2, next);
    }
}

static size_t max_frame(const zw_s2_t *s2)
{
    return s2->cfg.max_frame != 0u ? (unsigned)s2->cfg.max_frame : ZW_S2_MAX_FRAME_DEFAULT;
}

/* Encrypts the first `plain_len` bytes of the slot's command (0: a frame of the SPAN extension
 * alone) into s2->tx (§7): the SPAN's next nonce, or - with the peer's receiver EI - a new SPAN
 * and its extension. */
static zw_s2_result_t encapsulate(zw_s2_t *s2, zw_s2_peer_t *p, zw_s2_send_t *sl,
                                  size_t plain_len, size_t *n)
{
    const uint8_t *key_ccm = NULL;
    const uint8_t *pstring = NULL;
    if (!class_keys(s2, p->node, sl->cls, &key_ccm, &pstring)) {
        return ZW_S2_NO_KEY;
    }
    uint8_t sender_ei[ZW_S2_EI_LEN];
    const uint8_t *ext = NULL;
    if (p->span_state != SPAN_READY || p->span_class != sl->cls) {
        zw_ctr_drbg_t d;
        if (p->span_state != SPAN_REMOTE ||
            !zw_ctr_drbg_generate(&s2->crypto, &s2->rng, sender_ei, sizeof sender_ei) ||
            !zw_s2_span_instantiate(&s2->crypto, &d, sender_ei, p->remote_ei, pstring)) {
            zw_wipe(sender_ei, sizeof sender_ei);
            return ZW_S2_CRYPTO;
        }
        set_span(p, SPAN_READY);
        p->span = d;
        p->span_class = sl->cls;
        zw_wipe(&d, sizeof d);
        ext = sender_ei;
    }
    uint8_t nonce[ZW_S2_NONCE_LEN];
    zw_s2_result_t r = ZW_S2_CRYPTO;
    const size_t hdr = zw_s2_encap_header_build(s2->tx, sizeof s2->tx, p->own_seq, ext);
    const size_t total = hdr + plain_len + ZW_S2_TAG_BYTES;
    if (hdr > 0u && total <= sizeof s2->tx && zw_s2_span_next_nonce(&s2->crypto, &p->span, nonce)) {
        p->own_seq = (uint8_t)((unsigned)p->own_seq + 1u);
        const size_t aad_len = zw_s2_aad_build(s2->aad, sizeof s2->aad, s2->own_node, p->node,
                                               s2->home_id, (uint16_t)total, &s2->tx[2],
                                               hdr - 2u);
        if (aad_len > 0u &&
            zw_aes_ccm_encrypt(&s2->crypto, key_ccm, nonce, s2->aad, aad_len,
                               plain_len > 0u ? sl->plain : NULL, plain_len, &s2->tx[hdr],
                               &s2->tx[hdr + plain_len])) {
            /* §6 crossed frames: this nonce stays good for the peer's crossing frame a while */
            p->crossed_valid = true;
            p->crossed_class = sl->cls;
            p->crossed_until = s2->now + ZW_S2_CROSSED_MS;
            memcpy(p->crossed_nonce, nonce, sizeof nonce);
            p->sent_last = true;
            *n = total;
            r = ZW_S2_OK;
        }
    }
    zw_wipe(sender_ei, sizeof sender_ei);
    zw_wipe(nonce, sizeof nonce);
    return r;
}

/* Hands the slot's frame out (sl->frame, in `state`) to the radio, under a new stamp: a callback
 * of an earlier transmission of it is then ignored. */
static void send_frame(zw_s2_t *s2, zw_s2_send_t *sl, uint8_t state)
{
    sl->state = state;
    sl->gen = next_stamp(s2);
    const uint32_t tag = make_tag(TAG_SEND, sl->gen, (size_t)(sl - s2->sends));
    if (!s2->port.send(s2->port.ctx, sl->node, sl->frame, sl->frame_len, ZW_TX_OPT_DEFAULT, tag)) {
        finish(s2, sl, ZW_S2_FAILED);
    }
}

/* Hands the slot's frame to the radio: plain, or encrypted. */
static void transmit(zw_s2_t *s2, zw_s2_send_t *sl)
{
    zw_s2_peer_t *p = find_peer(s2, sl->node);
    if (p == NULL) {
        finish(s2, sl, ZW_S2_CANCELLED);
        return;
    }
    size_t n = 0u;
    sl->span_only = false;
    if (sl->cls == ZW_SEC_NONE) {
        memcpy(s2->tx, sl->plain, sl->len);
        n = sl->len;
    } else {
        const bool new_span = p->span_state != SPAN_READY || p->span_class != sl->cls;
        sl->span_only = new_span && ZW_S2_ENCAP_HEADER + ZW_S2_EXT_SPAN_LEN + sl->len +
                                            ZW_S2_TAG_BYTES > max_frame(s2);
        const zw_s2_result_t r = encapsulate(s2, p, sl, sl->span_only ? 0u : sl->len, &n);
        if (r != ZW_S2_OK) {
            finish(s2, sl, r);
            return;
        }
    }
    memcpy(sl->frame, s2->tx, n);
    sl->frame_len = (uint8_t)n;
    sl->jams = 0u; /* a new frame: its own count */
    send_frame(s2, sl, S_TX);
}

/* Starts a send: at once when it needs no nonce, else with a Nonce Get (§6 "before sending"). */
static void start_slot(zw_s2_t *s2, zw_s2_send_t *sl)
{
    zw_s2_peer_t *p = find_peer(s2, sl->node);
    if (p == NULL) {
        finish(s2, sl, ZW_S2_CANCELLED);
        return;
    }
    if (sl->cls == ZW_SEC_NONE || p->span_state == SPAN_REMOTE ||
        (p->span_state == SPAN_READY && p->span_class == sl->cls)) {
        transmit(s2, sl);
        return;
    }
    const size_t n = zw_s2_nonce_get_build(sl->frame, sizeof sl->frame, p->own_seq);
    p->own_seq = (uint8_t)((unsigned)p->own_seq + 1u);
    sl->frame_len = (uint8_t)n;
    sl->jams = 0u;
    sl->started = s2->now;
    send_frame(s2, sl, S_NONCE_GET);
}

/* Queues a send (the manager's own ones included). */
static zw_s2_result_t queue_send(zw_s2_t *s2, uint16_t node, zw_sec_class_t cls,
                                 const uint8_t *cmd, size_t len, uint32_t tag, bool internal)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (!s2->started || p == NULL) {
        return ZW_S2_UNKNOWN_NODE;
    }
    if (cmd == NULL || len == 0u) {
        return ZW_S2_INVALID;
    }
    if (cls == ZW_SEC_AUTO) {
        cls = cc_secure(s2, p, cmd[0]) ? highest_of(p->granted) : ZW_SEC_NONE;
    }
    if (cls != ZW_SEC_NONE && !class_possible(s2, p, (uint8_t)cls)) {
        return ZW_S2_NO_KEY;
    }
    /* a new SPAN's extension may go ahead in a frame of its own: the command need only fit the
     * module's frame without it */
    const size_t need = cls == ZW_SEC_NONE ? len : ZW_S2_ENCAP_HEADER + len + ZW_S2_TAG_BYTES;
    if (len > ZW_S2_PLAIN_MAX || need > max_frame(s2)) {
        return ZW_S2_TOO_LONG;
    }
    zw_s2_send_t *sl = NULL;
    for (size_t i = 0; i < ZW_S2_SENDS && sl == NULL; i++) {
        if (s2->sends[i].state == S_FREE) {
            sl = &s2->sends[i];
        }
    }
    if (sl == NULL) {
        return ZW_S2_BUSY;
    }
    const bool wait = node_has_queue(s2, node);
    sl->state = S_QUEUED;
    sl->cls = (uint8_t)cls;
    sl->retried = false;
    sl->tries = 0u;
    sl->jams = 0u;
    sl->jam_state = S_FREE;
    sl->jam_rebuild = false;
    sl->frame_len = 0u;
    sl->internal = internal;
    sl->reply = zw_s2_command_is_reply(cmd, len);
    sl->node = node;
    sl->tag = tag;
    sl->order = s2->next_order++;
    sl->deadline = 0u;
    sl->len = len;
    memcpy(sl->plain, cmd, len);
    if (!wait) {
        start_slot(s2, sl);
    }
    return ZW_S2_OK;
}

/* ---- Nonces (§6) --------------------------------------------------------------------------- */

/* A Nonce Report with a fresh receiver EI - unless one to this node still waits to go. */
static void send_nonce_report(zw_s2_t *s2, zw_s2_peer_t *p)
{
    if (p->nonce_report_pending) {
        return; /* no floods */
    }
    uint8_t rei[ZW_S2_EI_LEN];
    if (!zw_ctr_drbg_generate(&s2->crypto, &s2->rng, rei, sizeof rei)) {
        return;
    }
    set_span(p, SPAN_LOCAL);
    memcpy(p->local_ei, rei, sizeof rei);
    p->local_ei_valid = true;
    p->has_last_rx = false; /* a forgotten SPAN forgets the peer's last sequence number */
    const zw_s2_nonce_report_t r = {p->own_seq, ZW_S2_NONCE_SOS, rei};
    uint8_t f[4u + ZW_S2_EI_LEN];
    const size_t n = zw_s2_nonce_report_build(f, sizeof f, &r);
    p->own_seq = (uint8_t)((unsigned)p->own_seq + 1u);
    p->nonce_report_pending = true;
    p->nonce_report_gen = next_stamp(s2);
    p->stats.nonce_reports++;
    const uint32_t tag = make_tag(TAG_NONCE_REPORT, p->nonce_report_gen, (size_t)(p - s2->peers));
    if (!s2->port.send(s2->port.ctx, p->node, f, n, ZW_TX_OPT_NONCE_REPORT, tag)) {
        p->nonce_report_pending = false;
    }
    zw_wipe(rei, sizeof rei);
    zw_wipe(f, sizeof f);
}

/* The peer's last sequence number again: a duplicate, dropped silently. */
static bool duplicate(zw_s2_t *s2, zw_s2_peer_t *p, uint8_t seq)
{
    if (p->has_last_rx && p->last_rx_seq == seq) {
        p->stats.duplicates++;
        event(s2, p->node, ZW_S2_EV_DUPLICATE);
        return true;
    }
    return false;
}

static void note_seq(zw_s2_peer_t *p, uint8_t seq)
{
    p->last_rx_seq = seq;
    p->has_last_rx = true;
}

static void on_nonce_get(zw_s2_t *s2, zw_s2_peer_t *p, const uint8_t *cmd, size_t len)
{
    uint8_t seq = 0u;
    if (!zw_s2_nonce_get_parse(cmd, len, &seq)) {
        p->stats.malformed++;
        event(s2, p->node, ZW_S2_EV_MALFORMED);
        return;
    }
    if (duplicate(s2, p, seq)) {
        return;
    }
    note_seq(p, seq);
    event(s2, p->node, ZW_S2_EV_NONCE);
    send_nonce_report(s2, p);
}

static void on_nonce_report(zw_s2_t *s2, zw_s2_peer_t *p, const uint8_t *cmd, size_t len)
{
    zw_s2_nonce_report_t r;
    if (!zw_s2_nonce_report_parse(cmd, len, &r)) {
        p->stats.malformed++;
        event(s2, p->node, ZW_S2_EV_MALFORMED);
        return;
    }
    if (duplicate(s2, p, r.seq)) {
        return;
    }
    note_seq(p, r.seq);
    event(s2, p->node, ZW_S2_EV_NONCE);
    if ((r.flags & ZW_S2_NONCE_SOS) == 0u) {
        return; /* multicast out of sync only: the panel runs no multicast */
    }
    set_span(p, SPAN_REMOTE);
    memcpy(p->remote_ei, r.receiver_ei, ZW_S2_EI_LEN);
    zw_s2_send_t *sl = active_send(s2, p->node);
    if (sl == NULL) {
        return;
    }
    if (sl->state == S_JAM_WAIT) {
        /* the frame kept is on the old SPAN (or a Nonce Get no longer needed): at the deadline
         * it is built again, on the nonce this report brings (§6.12 serial-api.md) */
        sl->jam_rebuild = true;
        return;
    }
    if (sl->state == S_NONCE_GET || sl->state == S_WAIT_NONCE) {
        transmit(s2, sl); /* the nonce it waited for */
    } else if (sl->cls != ZW_SEC_NONE) {
        /* S_TX or S_GRACE: the node could not decrypt it - once more with a new SPAN, then give
         * up (§6 "after sending") */
        if (sl->retried) {
            finish(s2, sl, ZW_S2_NOT_DECODED);
        } else {
            sl->retried = true;
            sl->tries = 0u; /* the node is there: its own count for the frame sent again */
            p->stats.resends++;
            transmit(s2, sl);
        }
    }
}

/* ---- Receiving: the layers (command-classes.md §1) and §8 ---------------------------------- */

typedef enum { W_OK = 0, W_MALFORMED, W_UNSUPPORTED } walk_result_t;

typedef struct {
    zw_s2_t *s2;
    zw_s2_peer_t *p;
    bool deliver;     /* the second pass: hand the commands over */
    bool secure;      /* a command in it is of a CC the node lists as secure (S0: §8) */
    bool group;       /* it came broadcast or multicast */
    bool get;         /* ...and a command in it is a Get (ZW-042: discarded) */
} walk_t;

/* ZW-042 (Z-Wave JS Driver.ts shouldDiscardCC: a command whose class name ends in "Get", received
 * broadcast or multicast, is discarded; Node.ts: a received Get other than the two Nonce Gets marks
 * a sleeper awake). Z-Wave JS decides by the TypeScript class's name; this table mirrors that rule:
 * all 147 commands of Z-Wave JS master (2026-10-03) whose class name ends in "Get", extracted by
 * script from packages/cc/src/cc/ (each ...CC.ts), as CC << 8 | command, sorted. Not included: one
 * manufacturer-proprietary CC. So Multilevel Sensor's GetSupportedSensor (31 01) and
 * GetSupportedScale (31 03) are not Gets here, as there. */
static const uint16_t GETS[147] = {
        0x2002U, 0x2502U, 0x2602U, 0x2606U, 0x2C02U, 0x2D02U, 0x3001U, 0x3002U, 0x3104U, 0x3201U,
        0x3203U, 0x3301U, 0x3303U, 0x4002U, 0x4004U, 0x4201U, 0x4202U, 0x4205U, 0x4302U, 0x4304U,
        0x4309U, 0x4402U, 0x4404U, 0x4502U, 0x4602U, 0x4604U, 0x4607U, 0x4702U, 0x4C01U, 0x4C03U,
        0x4E04U, 0x4E07U, 0x4E09U, 0x4E0BU, 0x4E0EU, 0x5901U, 0x5903U, 0x5905U, 0x5B01U, 0x5B05U,
        0x5E01U, 0x6004U, 0x6007U, 0x6009U, 0x600EU, 0x6202U, 0x6205U, 0x6207U, 0x6302U, 0x6304U,
        0x6306U, 0x6309U, 0x630CU, 0x630FU, 0x6311U, 0x6402U, 0x6404U, 0x6406U, 0x6408U, 0x6602U,
        0x6604U, 0x6607U, 0x6A01U, 0x6A03U, 0x6B01U, 0x6B03U, 0x6B06U, 0x6B08U, 0x6B0BU, 0x6B0FU,
        0x6C01U, 0x6D02U, 0x6D04U, 0x6E01U, 0x6F02U, 0x6F04U, 0x6F07U, 0x7005U, 0x7008U, 0x700AU,
        0x700CU, 0x700EU, 0x7101U, 0x7104U, 0x7107U, 0x7204U, 0x7206U, 0x7302U, 0x7305U, 0x7502U,
        0x7504U, 0x7507U, 0x750AU, 0x7602U, 0x7702U, 0x7705U, 0x7901U, 0x7903U, 0x7906U, 0x7909U,
        0x7A01U, 0x7A03U, 0x7A05U, 0x7A0AU, 0x8002U, 0x8004U, 0x8105U, 0x8301U, 0x8303U, 0x8306U,
        0x830BU, 0x8314U, 0x8316U, 0x8318U, 0x831BU, 0x831DU, 0x8320U, 0x8405U, 0x8409U, 0x8502U,
        0x8505U, 0x850BU, 0x8611U, 0x8613U, 0x8615U, 0x8617U, 0x8702U, 0x8704U, 0x8706U, 0x8902U,
        0x8A01U, 0x8A03U, 0x8A06U, 0x8B02U, 0x8E02U, 0x8E05U, 0x9002U, 0x9802U, 0x9804U, 0x9840U,
        0x98C1U, 0x9C01U, 0x9C03U, 0x9F01U, 0x9F04U, 0x9F09U, 0x9F0DU
};

bool zw_s2_command_is_get(const uint8_t *cmd, size_t len)
{
    if (cmd == NULL || len < 2u) {
        return false;
    }
    const uint16_t key = (uint16_t)(((unsigned)cmd[0] << 8) | (unsigned)cmd[1]);
    size_t lo = 0u;
    size_t hi = sizeof GETS / sizeof GETS[0];
    while (lo < hi) { /* binary search */
        const size_t mid = lo + (hi - lo) / 2u;
        if (GETS[mid] == key) {
            return true;
        }
        if (GETS[mid] < key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return false;
}

static bool command_is_get(const uint8_t *cmd, size_t len)
{
    return zw_s2_command_is_get(cmd, len);
}

static void answer_commands_supported(zw_s2_t *s2, zw_s2_peer_t *p, zw_sec_class_t cls)
{
    const zw_sec_class_t top = highest_of(p->granted);
    size_t n = 0u;
    if (cls == top && is_s2_class((uint8_t)cls)) {
        n = sizeof PANEL_SECURE_CCS;
    } else if (!is_s2_class((uint8_t)cls)) {
        return; /* unencrypted, S0 or the temporary key: no answer (§9) */
    }
    uint8_t f[2u + sizeof PANEL_SECURE_CCS];
    const size_t len = zw_s2_commands_supported_report_build(f, sizeof f, PANEL_SECURE_CCS, n);
    (void)queue_send(s2, p->node, cls, f, len, 0u, true);
}

/* One command at the end of the layers. */
static void hand_over(walk_t *w, const zw_s2_rx_t *rx)
{
    zw_s2_t *s2 = w->s2;
    zw_s2_peer_t *p = w->p;
    if (p->node != rx->node) {
        return; /* forgotten by an earlier callback */
    }
    const uint8_t *c = rx->command;
    const bool bare = !rx->multi_channel && !rx->supervised && !rx->multi_command;
    if (s2->cfg.panel && rx->len >= 2u && c[0] == ZW_S2_CC) {
        if (c[1] == ZW_S2_COMMANDS_SUPPORTED_GET) {
            /* accepted (Z-Wave JS: it reaches handleRequest, where markAsAlive is): heard */
            event(s2, p->node, ZW_S2_EV_ANSWERED);
            if (bare && p->node == rx->node) {
                answer_commands_supported(s2, p, rx->security);
            }
            return;
        }
        /* A Commands Supported Report sets nothing here: only the interview's first non-empty
         * answer is the secure list (Z-Wave JS Security2CC.ts interview; zw_s2_set_secure_ccs
         * from zw_iv_steps.c take_secure_list) - an unasked or empty one changes nothing */
    }
    s2->port.received(s2->port.ctx, rx);
}

static walk_result_t walk(walk_t *w, zw_s2_rx_t ctx, uint8_t seen, const uint8_t *cmd, size_t len)
{
    zw_cc_unwrap_t u;
    if (cmd == NULL || len == 0u || !zw_cc_unwrap(cmd, len, &u)) {
        return W_MALFORMED;
    }
    switch (u.kind) {
    case ZW_CC_UNWRAP_TRANSPORT_SERVICE:
        /* not reassembled (v1, a recorded gap: security.md §8): nothing in it reaches anyone.
         * Transport Service sits outside security: Z-Wave JS joins the segments first
         * (Driver.ts handleTransportServiceCommand) and only then judges the joined command with
         * isSecurityLevelTooLow and shouldDiscardCC - so reassembly, if it ever comes, goes
         * before deliver's level check, and a segment is never a wrong level (deliver) */
        return W_UNSUPPORTED;
    case ZW_CC_UNWRAP_SECURITY_2:
        /* only the outermost layer is security, and nonces are never wrapped: an encapsulation
         * or a nonce command in here drops the frame; other S2 commands are commands */
        if (len < 2u || cmd[1] == ZW_S2_MESSAGE_ENCAPSULATION || cmd[1] == ZW_S2_NONCE_GET ||
            cmd[1] == ZW_S2_NONCE_REPORT) {
            return W_MALFORMED;
        }
        break;
    case ZW_CC_UNWRAP_SECURITY_0:
        if (len < 2u) {
            return W_MALFORMED;
        }
        if (cmd[1] == S0_ENCAP || cmd[1] == S0_ENCAP_NONCE) {
            /* S0 encapsulation: unsupported outermost (phase 3), a forgery anywhere else */
            return seen == 0u && ctx.security == ZW_SEC_NONE ? W_UNSUPPORTED : W_MALFORMED;
        }
        break;
    case ZW_CC_UNWRAP_NONE:
        break;
    default: {
        const uint8_t bit = (uint8_t)(1u << (unsigned)u.kind);
        if ((seen & bit) != 0u) {
            return W_MALFORMED; /* a layer appears at most once */
        }
        seen = (uint8_t)(seen | bit);
        if (u.kind == ZW_CC_UNWRAP_SUPERVISION) {
            ctx.supervised = true;
            ctx.supervision_session = u.supervision_session_id;
            ctx.supervision_updates = u.supervision_status_updates_wanted;
        } else if (u.kind == ZW_CC_UNWRAP_MULTI_CHANNEL) {
            ctx.multi_channel = true;
            ctx.source_endpoint = u.source_endpoint;
            ctx.destination_endpoint = u.destination_endpoint;
        } else if (u.kind == ZW_CC_UNWRAP_CRC16) {
            ctx.crc16 = true;
        } else if (u.kind == ZW_CC_UNWRAP_MULTI_COMMAND) {
            ctx.multi_command = true;
            zw_cc_multi_command_iter_t it;
            const uint8_t *c = NULL;
            size_t n = 0u;
            if (!zw_cc_multi_command_iter_init(&it, u.inner, u.inner_len)) {
                return W_MALFORMED;
            }
            while (zw_cc_multi_command_iter_next(&it, &c, &n)) {
                const walk_result_t r = walk(w, ctx, seen, c, n);
                if (r != W_OK) {
                    return r;
                }
            }
            return it.remaining == 0u ? W_OK : W_MALFORMED; /* a truncated list */
        }
        return walk(w, ctx, seen, u.inner, u.inner_len);
    }
    }
    /* a command: the innermost decides (Driver.ts isSecurityLevelTooLow) */
    if (cc_listed_secure(w->p, cmd[0])) {
        w->secure = true;
    }
    if (w->group && !ctx.multi_command && command_is_get(cmd, len)) {
        /* shouldDiscardCC looks through the encapsulating layers (Supervision, Multi Channel,
         * CRC-16) but not inside Multi Command, a multi-encapsulating one: a Get in a Multi
         * Command received broadcast or multicast is delivered, as Z-Wave JS does */
        w->get = true;
    }
    if (w->deliver) {
        ctx.command = cmd;
        ctx.len = len;
        hand_over(w, &ctx);
    }
    return W_OK;
}

/* ZW-009, §8 (Z-Wave JS Driver.ts isSecurityLevelTooLow, isCCConsideredSecure) for a node whose
 * highest class `top` is an S2 class: every command must come encrypted at that class. In the
 * clear only a bare Security 2 command (after any CRC-16 layer: "CRC16 belongs outside of Security
 * encapsulation") and Security 0's Nonce Get, Nonce Report, Scheme Get and Scheme Report
 * ("Cannot be sent encapsulated": always accepted; they carry no state); below its class only
 * Security 2 Commands Supported Get and Report, and a singlecast follow-up of a multicast (its
 * MGRP extension: "Multicast commands are always accepted"). */
static bool level_ok_s2(zw_sec_class_t cls, zw_sec_class_t top, bool mgrp, const uint8_t *cmd,
                        size_t len)
{
    if (cls == ZW_SEC_NONE) {
        zw_cc_unwrap_t u;
        while (len >= 2u && cmd[0] == (uint8_t)ZW_CC_CRC16_ENCAPSULATION &&
               cmd[1] == ZW_CMD_CRC16_ENCAPSULATE) {
            if (!zw_cc_unwrap(cmd, len, &u) || u.kind != ZW_CC_UNWRAP_CRC16) {
                /* a CRC-16 frame that does not parse (its checksum fails): a decode error in
                 * Z-Wave JS (CommandClass.parse), dropped before any security check - walk()
                 * counts it malformed */
                return true;
            }
            cmd = u.inner;
            len = u.inner_len;
        }
        if (len >= 2u && cmd[0] == (uint8_t)ZW_CC_SECURITY_0) {
            return cmd[1] == S0_NONCE_GET || cmd[1] == S0_NONCE_REPORT ||
                   cmd[1] == S0_SCHEME_GET || cmd[1] == S0_SCHEME_REPORT;
        }
        return len >= 2u && cmd[0] == ZW_S2_CC;
    }
    if (cls == top || mgrp) {
        return true;
    }
    return len >= 2u && cmd[0] == ZW_S2_CC &&
           (cmd[1] == ZW_S2_COMMANDS_SUPPORTED_GET || cmd[1] == ZW_S2_COMMANDS_SUPPORTED_REPORT);
}

static void wrong_level(zw_s2_t *s2, zw_s2_peer_t *p)
{
    p->stats.wrong_level++;
    event(s2, p->node, ZW_S2_EV_WRONG_LEVEL);
}

/* Everything in a command (decrypted, or received in the clear) that §1 and §8 let through: the
 * level first, as Z-Wave JS does - after a Transport Service segment, which Z-Wave JS joins before
 * judging (not reassembled here: counted unsupported, never a wrong level; its Segment Request,
 * Complete and Wait are judged). A node being bootstrapped (its temporary key installed) is not
 * judged on its level: Z-Wave JS's class for it is Temporary or not yet known. `group`: it came
 * broadcast or multicast - then a Get in it is discarded (false), after the level check
 * (shouldDiscardCC). */
static bool deliver(zw_s2_t *s2, zw_s2_peer_t *p, zw_sec_class_t cls, bool mgrp, bool crc16,
                    bool group, const uint8_t *cmd, size_t len)
{
    walk_t w = {s2, p, false, false, group, false};
    bool judged_ts = false;
    zw_s2_rx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.node = p->node;
    ctx.security = cls;
    ctx.crc16 = crc16;
    const zw_sec_class_t top = highest_of(p->granted);
    const bool judged = s2->cfg.panel && s2->temp_node != p->node && cls != ZW_SEC_TEMP;
    if (len >= 2u && cmd[0] == (uint8_t)ZW_CC_TRANSPORT_SERVICE &&
        (((unsigned)cmd[1] & 0xF8u) == 0xC0u || ((unsigned)cmd[1] & 0xF8u) == 0xE0u)) {
        /* a Transport Service segment (First or Subsequent Segment), outermost: not reassembled
         * (walk), so not supported - never a wrong level, as Z-Wave JS judges only the joined
         * command (TransportServiceCC.ts isTransportServiceEncapsulation). Its Segment Request,
         * Complete and Wait are judged on their level like any command (Driver.ts
         * isSecurityLevelTooLow) */
        judged_ts = true;
    }
    if (judged && !judged_ts && is_s2_class((uint8_t)top) &&
        !level_ok_s2(cls, top, mgrp, cmd, len)) {
        wrong_level(s2, p);
        return true;
    }
    const walk_result_t r = walk(&w, ctx, 0u, cmd, len);
    if (r != W_OK) {
        if (r == W_UNSUPPORTED) {
            p->stats.unsupported++;
        } else {
            p->stats.malformed++;
        }
        event(s2, p->node, r == W_UNSUPPORTED ? ZW_S2_EV_UNSUPPORTED : ZW_S2_EV_MALFORMED);
        return true;
    }
    /* S0 highest: a CC it lists as secure only under S0 - which the panel never decrypts (phase
     * 3) - while an unlisted CC is taken in the clear, as Z-Wave JS does */
    if (judged && top == ZW_SEC_S0 && w.secure && cls != ZW_SEC_S0) {
        wrong_level(s2, p);
        return true;
    }
    if (w.get) {
        return false;
    }
    w.deliver = true;
    (void)walk(&w, ctx, 0u, cmd, len);
    return true;
}

/* ---- Receiving: decryption (§6, §7) --------------------------------------------------------- */

static bool try_nonce(zw_s2_t *s2, const uint8_t *key_ccm, const uint8_t nonce[ZW_S2_NONCE_LEN],
                      const zw_s2_encap_t *e, size_t aad_len)
{
    return zw_aes_ccm_decrypt(&s2->crypto, key_ccm, nonce, s2->aad, aad_len, e->ciphertext,
                              e->ciphertext_len, e->tag, s2->plain);
}

/* A frame with a SPAN extension on the panel's receiver EI: the SPAN it starts, tried with each
 * class the node may use - the temporary key first while it is bootstrapped, then the class
 * being exchanged; else its classes, highest first. The class that decrypts it, or NONE. */
static zw_sec_class_t decrypt_new_span(zw_s2_t *s2, zw_s2_peer_t *p, const zw_s2_encap_t *e,
                                       size_t aad_len)
{
    uint8_t order[3];
    size_t n = 0u;
    if (s2->temp_node == p->node) {
        order[n++] = ZW_SEC_TEMP;
        if (is_s2_class((uint8_t)s2->exchange_class)) {
            order[n++] = (uint8_t)s2->exchange_class;
        }
    } else {
        static const uint8_t TOP_DOWN[3] = {ZW_SEC_S2_ACCESS, ZW_SEC_S2_AUTH, ZW_SEC_S2_UNAUTH};
        for (size_t i = 0; i < 3u; i++) {
            if (class_possible(s2, p, TOP_DOWN[i])) {
                order[n++] = TOP_DOWN[i];
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        const uint8_t *key_ccm = NULL;
        const uint8_t *pstring = NULL;
        zw_ctr_drbg_t d;
        uint8_t nonce[ZW_S2_NONCE_LEN];
        bool ok = class_keys(s2, p->node, order[i], &key_ccm, &pstring) &&
                  zw_s2_span_instantiate(&s2->crypto, &d, e->found.sender_ei, p->local_ei,
                                         pstring) &&
                  zw_s2_span_next_nonce(&s2->crypto, &d, nonce) &&
                  try_nonce(s2, key_ccm, nonce, e, aad_len);
        if (ok) {
            set_span(p, SPAN_READY);
            p->span = d;
            p->span_class = order[i];
            p->local_ei_valid = false; /* used */
            if (p->classes_unknown && is_s2_class(order[i])) {
                /* ZW-009, a class learnt by decrypting (Z-Wave JS Security2CC.ts
                 * decryptSinglecast: setSecurityClass(secClass, true) for the class that
                 * decrypted, the others left undecided and tried again on the next SPAN): this
                 * class is granted; its highest granted class (highest_of, as
                 * getHighestSecurityClass) rises when a higher one decrypts later */
                const uint8_t bit = class_bit((zw_sec_class_t)order[i]);
                const bool learnt = (p->granted & bit) == 0u;
                p->granted = (uint8_t)(p->granted | bit);
                p->refused = (uint8_t)(p->refused & ~bit);
                if (learnt) {
                    /* stored at once, as Z-Wave JS's cache-backed securityClasses: the owner
                     * adds it to the record (zw_net.c s_event) */
                    event(s2, p->node, ZW_S2_EV_CLASS_LEARNT);
                }
            }
        }
        zw_wipe(&d, sizeof d);
        zw_wipe(nonce, sizeof nonce);
        if (ok) {
            return (zw_sec_class_t)order[i];
        }
    }
    return ZW_SEC_NONE;
}

/* A frame on the running SPAN: the panel's last sending nonce when the frames crossed, then the
 * SPAN's next nonces, ZW_S2_NONCE_ATTEMPTS of them (one for a multicast follow-up) - lost frames
 * advance the peer's generator. The generator moves only when one authenticates. `after_ours`:
 * it took the nonce right after the panel's own frame, and its sequence number follows the
 * peer's last one (no frame of it missed) - so the peer's generator moved past the panel's
 * frame's nonce, which only decrypting that frame does: the peer decoded it. */
static zw_sec_class_t decrypt_on_span(zw_s2_t *s2, zw_s2_peer_t *p, const zw_s2_encap_t *e,
                                      size_t aad_len, bool *after_ours)
{
    const uint8_t cls = p->span_class;
    const uint8_t *key_ccm = NULL;
    const uint8_t *pstring = NULL;
    *after_ours = false;
    if (!class_keys(s2, p->node, cls, &key_ccm, &pstring)) {
        return ZW_SEC_NONE;
    }
    const bool next_seq = p->has_last_rx && e->seq == (uint8_t)((unsigned)p->last_rx_seq + 1u);
    if (p->crossed_valid && p->crossed_class == cls && s2->now < p->crossed_until && next_seq &&
        try_nonce(s2, key_ccm, p->crossed_nonce, e, aad_len)) {
        p->crossed_valid = false; /* once */
        zw_wipe(p->crossed_nonce, sizeof p->crossed_nonce);
        return (zw_sec_class_t)cls;
    }
    zw_ctr_drbg_t d = p->span;
    const unsigned tries = e->found.mgrp ? 1u : ZW_S2_NONCE_ATTEMPTS;
    zw_sec_class_t won = ZW_SEC_NONE;
    for (unsigned i = 0; i < tries && won == ZW_SEC_NONE; i++) {
        uint8_t nonce[ZW_S2_NONCE_LEN];
        if (!zw_s2_span_next_nonce(&s2->crypto, &d, nonce)) {
            break;
        }
        if (try_nonce(s2, key_ccm, nonce, e, aad_len)) {
            p->span = d;
            won = (zw_sec_class_t)cls;
            *after_ours = i == 0u && next_seq && p->sent_last;
        }
        zw_wipe(nonce, sizeof nonce);
    }
    zw_wipe(&d, sizeof d);
    return won;
}

static bool peer_has_key(const zw_s2_t *s2, const zw_s2_peer_t *p)
{
    const uint8_t *k = NULL;
    const uint8_t *ps = NULL;
    (void)k;
    (void)ps;
    /* a node whose classes are not all known: any class it may hold (class_possible) */
    return s2->temp_node == p->node || class_possible(s2, p, ZW_SEC_S2_UNAUTH) ||
           class_possible(s2, p, ZW_SEC_S2_AUTH) || class_possible(s2, p, ZW_SEC_S2_ACCESS);
}

/* §6 after sending: the node decoded the frame delivered (decrypt_on_span's `after_ours`), so no
 * Nonce Report can come for it - its send ends now, not ZW_S2_RESEND_MS after delivery, and the
 * node's next one goes at once (a contact holds its next report until it hears the Supervision
 * Report to the last). After the node's command is handed over, so what it asks (a firmware
 * fragment again) is known before the next send goes; unless that send ended meanwhile. */
static void end_decoded(zw_s2_t *s2, zw_s2_send_t *sl, uint16_t gen)
{
    if (sl != NULL && sl->state == S_GRACE && sl->gen == gen) {
        finish(s2, sl, ZW_S2_OK);
    }
}

static void on_encap(zw_s2_t *s2, zw_s2_peer_t *p, const uint8_t *cmd, size_t len, bool crc16)
{
    zw_s2_encap_t e;
    if (len > ZW_S2_RX_MAX || !zw_s2_encap_parse(cmd, len, &e)) {
        p->stats.malformed++;
        event(s2, p->node, ZW_S2_EV_MALFORMED);
        return;
    }
    if (!peer_has_key(s2, p)) {
        event(s2, p->node, ZW_S2_EV_NO_KEY); /* §7: no class and no temporary key */
        return;
    }
    if (duplicate(s2, p, e.seq)) {
        return;
    }
    const size_t aad_len = zw_s2_aad_build(s2->aad, sizeof s2->aad, p->node, s2->own_node,
                                           s2->home_id, (uint16_t)len, &cmd[2], 2u + e.ext_len);
    zw_sec_class_t cls = ZW_SEC_NONE;
    const bool exchanged = e.found.span && p->local_ei_valid;
    bool after_ours = false;
    if (aad_len > 0u) {
        if (exchanged) {
            cls = decrypt_new_span(s2, p, &e, aad_len);
        } else if (!e.found.span && p->span_state == SPAN_READY) {
            cls = decrypt_on_span(s2, p, &e, aad_len, &after_ours);
        }
    }
    if (cls == ZW_SEC_NONE) {
        p->stats.decrypt_failures++;
        event(s2, p->node, exchanged ? ZW_S2_EV_DECRYPT_FAILED_SPAN : ZW_S2_EV_DECRYPT_FAILED);
        send_nonce_report(s2, p); /* so the peer sends it again with a new SPAN */
        return;
    }
    p->stats.decrypted++;
    note_seq(p, e.seq);
    p->sent_last = false;
    zw_s2_send_t *sl = active_send(s2, p->node);
    zw_s2_send_t *decoded = NULL;
    uint16_t decoded_gen = 0u;
    if (sl != NULL && sl->state == S_JAM_WAIT && sl->cls != ZW_SEC_NONE) {
        /* the node's frame moved the shared SPAN on, past the nonce of the frame kept: at the
         * deadline it is built again on the next one, not resent (§6.12 serial-api.md) */
        sl->jam_rebuild = true;
    } else if (sl != NULL && sl->state == S_GRACE && after_ours) {
        decoded = sl; /* ended once the node's command is handed over (end_decoded) */
        decoded_gen = sl->gen;
    }
    size_t used = 0u;
    zw_s2_ext_t inner;
    if ((e.flags & ZW_S2_ENCAP_EXT_ENC) != 0u &&
        !zw_s2_ext_parse(s2->plain, e.ciphertext_len, true, &inner, &used)) {
        p->stats.malformed++;
        event(s2, p->node, ZW_S2_EV_MALFORMED);
        zw_wipe(s2->plain, e.ciphertext_len);
        end_decoded(s2, decoded, decoded_gen);
        return;
    }
    if (e.ciphertext_len > used) { /* nothing more when it only carried extensions */
        (void)deliver(s2, p, cls, e.found.mgrp, crc16, false, &s2->plain[used],
                      e.ciphertext_len - used);
    }
    zw_wipe(s2->plain, e.ciphertext_len);
    end_decoded(s2, decoded, decoded_gen);
}

/* ---- API ------------------------------------------------------------------------------------- */

void zw_s2_init(zw_s2_t *s2, const zw_crypto_port_t *crypto, const zw_s2_port_t *port,
                const zw_s2_config_t *cfg)
{
    memset(s2, 0, sizeof *s2);
    s2->crypto = *crypto;
    s2->port = *port;
    if (cfg != NULL) {
        s2->cfg = *cfg;
    }
    s2->exchange_class = ZW_SEC_NONE;
}

void zw_s2_set_attempts(zw_s2_t *s2, zw_s2_attempts_fn attempts)
{
    s2->attempts = attempts;
}

void zw_s2_set_jammed(zw_s2_t *s2, zw_s2_jammed_fn jammed)
{
    s2->jammed = jammed;
}

static void wipe_state(zw_s2_t *s2)
{
    zw_wipe(s2->keys, sizeof s2->keys);
    zw_wipe(s2->peers, sizeof s2->peers);
    zw_wipe(s2->sends, sizeof s2->sends);
    zw_wipe(s2->temp_key_ccm, sizeof s2->temp_key_ccm);
    zw_wipe(s2->temp_pstring, sizeof s2->temp_pstring);
    zw_wipe(&s2->rng, sizeof s2->rng);
    s2->temp_node = 0u;
    s2->exchange_class = ZW_SEC_NONE;
}

/* Expands a network key into its slot (§3); KeyMPAN is not used (no multicast). */
static bool set_slot(zw_s2_t *s2, int k, const uint8_t key[16])
{
    uint8_t mpan[16];
    zw_s2_key_t *slot = &s2->keys[k];
    slot->present = zw_s2_network_key_expand(&s2->crypto, key, slot->key_ccm, slot->pstring, mpan);
    zw_wipe(mpan, sizeof mpan);
    if (slot->present) {
        memcpy(slot->netkey, key, 16u);
    }
    return slot->present;
}

bool zw_s2_set_key(zw_s2_t *s2, uint8_t key_bit, const uint8_t key[16])
{
    const int k = key_slot(s2, s2->own_node, key_bit);
    return k >= 0 && key != NULL && set_slot(s2, k, key);
}

bool zw_s2_start(zw_s2_t *s2, uint32_t home_id, uint16_t own_node, const zw_s2_keys_t *keys)
{
    wipe_state(s2);
    s2->started = false;
    s2->home_id = home_id;
    s2->own_node = own_node;
    uint8_t entropy[32];
    bool ok = s2->crypto.random(s2->crypto.ctx, entropy, sizeof entropy) &&
              zw_ctr_drbg_instantiate(&s2->crypto, &s2->rng, entropy, NULL);
    zw_wipe(entropy, sizeof entropy);
    if (ok && keys != NULL) {
        ok = set_slot(s2, K_UNAUTH, keys->s2_unauth) && set_slot(s2, K_AUTH, keys->s2_auth) &&
             set_slot(s2, K_ACCESS, keys->s2_access) && set_slot(s2, K_S0, keys->s0) &&
             set_slot(s2, K_LR_AUTH, keys->lr_auth) && set_slot(s2, K_LR_ACCESS, keys->lr_access);
    }
    if (!ok) {
        wipe_state(s2);
        return false;
    }
    s2->started = true;
    return true;
}

void zw_s2_stop(zw_s2_t *s2)
{
    wipe_state(s2);
    s2->started = false;
    s2->home_id = 0u;
    s2->own_node = 0u;
}

const uint8_t *zw_s2_network_key(const zw_s2_t *s2, uint16_t node, uint8_t key_bit)
{
    const int k = key_slot(s2, node, key_bit);
    return k >= 0 && s2->keys[k].present ? s2->keys[k].netkey : NULL;
}

bool zw_s2_add_node(zw_s2_t *s2, uint16_t node, uint8_t granted, const uint8_t *nif_ccs,
                    size_t nif_len)
{
    if (!s2->started || node == 0u || node == s2->own_node) {
        return false;
    }
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        for (size_t i = 0; i < ZW_S2_PEERS && p == NULL; i++) {
            if (s2->peers[i].node == 0u) {
                p = &s2->peers[i];
            }
        }
        if (p == NULL) {
            return false;
        }
        memset(p, 0, sizeof *p);
        p->node = node;
        /* §6: the panel's sequence numbers start at random */
        if (!zw_ctr_drbg_generate(&s2->crypto, &s2->rng, &p->own_seq, 1u)) {
            p->own_seq = 0u;
        }
    }
    p->granted = granted;
    p->refused = 0u;
    bits_from_list(p->nif, nif_ccs, nif_len);
    return true;
}

bool zw_s2_add_nif_ccs(zw_s2_t *s2, uint16_t node, const uint8_t *nif_ccs, size_t nif_len)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        return false;
    }
    bits_add_list(p->nif, nif_ccs, nif_len); /* added, never removed (ZW-009) */
    return true;
}

bool zw_s2_set_nif_ccs(zw_s2_t *s2, uint16_t node, const uint8_t *nif_ccs, size_t nif_len)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        return false;
    }
    bits_from_list(p->nif, nif_ccs, nif_len); /* replaced: granted and refused stay (BL-152) */
    return true;
}

bool zw_s2_set_classes_unknown(zw_s2_t *s2, uint16_t node, bool unknown)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        return false;
    }
    p->classes_unknown = unknown;
    p->refused = 0u; /* nothing refused yet */
    return true;
}

bool zw_s2_set_class_result(zw_s2_t *s2, uint16_t node, uint8_t key_bit, bool granted)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL || (key_bit & ZW_S2_KEYS_S2) == 0u) {
        return false;
    }
    const uint8_t bit = (uint8_t)(key_bit & ZW_S2_KEYS_S2);
    if (granted) {
        p->granted = (uint8_t)(p->granted | bit);
        p->refused = (uint8_t)(p->refused & ~bit);
    } else {
        p->granted = (uint8_t)(p->granted & ~bit);
        p->refused = (uint8_t)(p->refused | bit);
        if (is_s2_class(p->span_class) && !class_possible(s2, p, p->span_class)) {
            set_span(p, SPAN_NONE);
        }
    }
    return true;
}

uint8_t zw_s2_refused(const zw_s2_t *s2, uint16_t node)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL ? (unsigned)p->refused : 0u;
}

bool zw_s2_classes_unknown(const zw_s2_t *s2, uint16_t node)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL && p->classes_unknown;
}

void zw_s2_remove_node(zw_s2_t *s2, uint16_t node)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        return;
    }
    if (s2->temp_node == node) {
        zw_s2_clear_temp_key(s2);
    }
    zw_wipe(p, sizeof *p); /* its node is 0 now: its sends end as cancelled */
    for (size_t i = 0; i < ZW_S2_SENDS; i++) {
        if (s2->sends[i].state != S_FREE && s2->sends[i].node == node) {
            finish(s2, &s2->sends[i], ZW_S2_CANCELLED);
        }
    }
}

bool zw_s2_has_node(const zw_s2_t *s2, uint16_t node)
{
    return find_peer_c(s2, node) != NULL;
}

bool zw_s2_set_granted(zw_s2_t *s2, uint16_t node, uint8_t granted)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        return false;
    }
    p->granted = granted;
    p->refused = 0u;
    p->classes_unknown = false; /* its classes are decided now */
    if (is_s2_class(p->span_class) && !class_usable(s2, p, p->span_class)) {
        set_span(p, SPAN_NONE);
    }
    return true;
}

uint8_t zw_s2_granted(const zw_s2_t *s2, uint16_t node)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL ? (unsigned)p->granted : 0u;
}

zw_sec_class_t zw_s2_highest(const zw_s2_t *s2, uint16_t node)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL ? highest_of(p->granted) : ZW_SEC_NONE;
}

zw_sec_class_t zw_s2_class_for(const zw_s2_t *s2, uint16_t node, uint8_t cc)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL && cc_secure(s2, p, cc) ? highest_of(p->granted) : ZW_SEC_NONE;
}

zw_sec_class_t zw_s2_class_for_endpoint(const zw_s2_t *s2, uint16_t node, uint8_t endpoint,
                                        uint8_t cc)
{
    return zw_s2_class_for(s2, node, endpoint != 0u ? (uint8_t)ZW_CC_MULTI_CHANNEL : cc);
}

bool zw_s2_set_secure_ccs(zw_s2_t *s2, uint16_t node, const uint8_t *ccs, size_t n)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        return false;
    }
    bits_from_list(p->secure, ccs, n);
    p->secure_known = true;
    return true;
}

const zw_s2_node_stats_t *zw_s2_node_stats(const zw_s2_t *s2, uint16_t node)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL ? &p->stats : NULL;
}

zw_s2_span_state_t zw_s2_span_state(const zw_s2_t *s2, uint16_t node)
{
    const zw_s2_peer_t *p = find_peer_c(s2, node);
    return p != NULL ? (zw_s2_span_state_t)p->span_state : ZW_S2_SPAN_NONE;
}

bool zw_s2_set_temp_key(zw_s2_t *s2, uint16_t node, const uint8_t key_ccm[16],
                        const uint8_t pstring[32])
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL || key_ccm == NULL || pstring == NULL) {
        return false;
    }
    if (s2->temp_node != 0u && s2->temp_node != node) {
        zw_s2_clear_temp_key(s2);
    }
    s2->temp_node = node;
    s2->exchange_class = ZW_SEC_NONE;
    memcpy(s2->temp_key_ccm, key_ccm, sizeof s2->temp_key_ccm);
    memcpy(s2->temp_pstring, pstring, sizeof s2->temp_pstring);
    set_span(p, SPAN_NONE);
    p->local_ei_valid = false;
    return true;
}

void zw_s2_set_exchange_class(zw_s2_t *s2, zw_sec_class_t cls)
{
    s2->exchange_class = is_s2_class((uint8_t)cls) ? cls : ZW_SEC_NONE;
}

void zw_s2_clear_temp_key(zw_s2_t *s2)
{
    zw_s2_peer_t *p = find_peer(s2, s2->temp_node);
    if (p != NULL && p->span_class == ZW_SEC_TEMP) {
        set_span(p, SPAN_NONE);
    }
    zw_wipe(s2->temp_key_ccm, sizeof s2->temp_key_ccm);
    zw_wipe(s2->temp_pstring, sizeof s2->temp_pstring);
    s2->temp_node = 0u;
    s2->exchange_class = ZW_SEC_NONE;
}

void zw_s2_forget_span(zw_s2_t *s2, uint16_t node)
{
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p != NULL) {
        set_span(p, SPAN_NONE);
        p->local_ei_valid = false;
    }
}

zw_s2_result_t zw_s2_send(zw_s2_t *s2, uint64_t now, uint16_t node, zw_sec_class_t cls,
                          const uint8_t *cmd, size_t len, uint32_t tag)
{
    s2->now = now;
    return queue_send(s2, node, cls, cmd, len, tag, false);
}

/* A frame that waited out a jam goes again: as it was, or built again after an SOS Nonce Report
 * (start_slot: the frame itself on the new nonce). Its jam count stays (the owner's to judge). */
static void jam_resend(zw_s2_t *s2, zw_s2_send_t *sl)
{
    if (sl->jam_rebuild) {
        const uint8_t jams = sl->jams;
        sl->jam_rebuild = false;
        start_slot(s2, sl);
        if (sl->state == S_NONCE_GET || sl->state == S_TX) {
            sl->jams = jams;
        }
        return;
    }
    if (sl->jam_state == S_NONCE_GET) {
        sl->started = s2->now; /* the round trip is timed from this transmission */
    }
    send_frame(s2, sl, sl->jam_state);
}

/* serial-api.md §6.12: a frame of the send the radio did not deliver - sent again as a new frame
 * (a new sequence number; encrypted, the SPAN's next nonce), up to the owner's count of
 * transmissions; else the send fails. Only the radio's own failures go again: an unknown end
 * fails it as before (a jammed module is jammed()'s). */
static void not_delivered(zw_s2_t *s2, zw_s2_send_t *sl, zw_s2_tx_t status)
{
    sl->tries = (uint8_t)((unsigned)sl->tries + 1u);
    const bool radio = status == ZW_S2_TX_NO_ACK || status == ZW_S2_TX_FAIL ||
                       status == ZW_S2_TX_NO_ROUTE;
    unsigned max = 1u;
    if (radio && s2->attempts != NULL) {
        max = s2->attempts(s2->port.ctx, sl->node, sl->tag);
        max = max < 1u ? 1u : max > ZW_S2_TX_ATTEMPTS ? ZW_S2_TX_ATTEMPTS : max;
    }
    zw_s2_peer_t *p = find_peer(s2, sl->node);
    if (!radio || p == NULL || (unsigned)sl->tries >= max) {
        finish(s2, sl, ZW_S2_FAILED);
        return;
    }
    p->stats.retries++;
    if (sl->state == S_NONCE_GET) {
        start_slot(s2, sl); /* a new Nonce Get - or the frame itself, if a nonce came meanwhile */
    } else {
        transmit(s2, sl);
    }
}

/* serial-api.md §6.12: the module could not send the frame at all - not a delivery attempt. The
 * owner decides: the same bytes again after ZW_S2_JAM_WAIT_MS or at once (the node never heard
 * them, so the same sequence number and nonce stay right), or the send fails. */
static void jammed(zw_s2_t *s2, zw_s2_send_t *sl)
{
    sl->jams = (uint8_t)((unsigned)sl->jams + 1u);
    const zw_s2_jam_t j = s2->jammed != NULL && sl->jams != 0u
                              ? s2->jammed(s2->port.ctx, sl->node, sl->tag, sl->jams)
                              : ZW_S2_JAM_FAIL;
    if (j == ZW_S2_JAM_FAIL) {
        finish(s2, sl, ZW_S2_FAILED);
        return;
    }
    sl->jam_state = sl->state;
    sl->jam_rebuild = false;
    sl->state = S_JAM_WAIT;
    sl->deadline = j == ZW_S2_JAM_NOW ? s2->now : s2->now + ZW_S2_JAM_WAIT_MS;
    if (j == ZW_S2_JAM_NOW) {
        jam_resend(s2, sl);
    }
}

void zw_s2_sent(zw_s2_t *s2, uint64_t now, uint32_t tag, bool delivered)
{
    zw_s2_sent_tx(s2, now, tag, delivered ? ZW_S2_TX_OK : ZW_S2_TX_NO_ACK);
}

void zw_s2_sent_tx(zw_s2_t *s2, uint64_t now, uint32_t tag, zw_s2_tx_t status)
{
    s2->now = now;
    if ((tag & TAG_MARK_MASK) != TAG_MARK) {
        return;
    }
    const unsigned kind = (unsigned)(tag >> 24) & 0xFu;
    const uint16_t stamp = (uint16_t)(tag >> 8);
    const size_t idx = tag & 0xFFu;
    if (kind == TAG_NONCE_REPORT && idx < ZW_S2_PEERS) {
        zw_s2_peer_t *p = &s2->peers[idx];
        if (p->node != 0u && p->nonce_report_pending && p->nonce_report_gen == stamp) {
            p->nonce_report_pending = false; /* only one attempt, whatever came of it */
        }
        return;
    }
    if (kind != TAG_SEND || idx >= ZW_S2_SENDS) {
        return;
    }
    zw_s2_send_t *sl = &s2->sends[idx];
    if (sl->gen != stamp || (sl->state != S_NONCE_GET && sl->state != S_TX)) {
        return; /* an older frame of it, sent again since */
    }
    if (status == ZW_S2_TX_JAMMED) {
        jammed(s2, sl);
        return;
    }
    if (status != ZW_S2_TX_OK) {
        not_delivered(s2, sl, status);
        return;
    }
    sl->tries = 0u; /* delivered: the next frame of it has its own transmissions */
    sl->jams = 0u;
    if (sl->state == S_TX && sl->span_only) {
        transmit(s2, sl); /* the SPAN is there: now the command, on its next nonce */
    } else if (sl->state == S_NONCE_GET) {
        sl->state = S_WAIT_NONCE;
        sl->deadline = now + ZW_S2_NONCE_WAIT_MS + (now - sl->started);
    } else if (sl->cls == ZW_SEC_NONE) {
        finish(s2, sl, ZW_S2_OK);
    } else {
        sl->state = S_GRACE;
        sl->deadline = now + ZW_S2_RESEND_MS;
    }
}

bool zw_s2_command_is_reply(const uint8_t *cmd, size_t len)
{
    zw_cc_unwrap_t u;
    if (cmd != NULL && len >= 2u && cmd[0] == (uint8_t)ZW_CC_MULTI_CHANNEL &&
        zw_cc_unwrap(cmd, len, &u) && u.kind == ZW_CC_UNWRAP_MULTI_CHANNEL) {
        cmd = u.inner;
        len = u.inner_len;
    }
    if (cmd == NULL || len < 2u) {
        return false;
    }
    /* command-classes.md §5: the Report each of the panel's answers to a device's Get is */
    static const uint8_t ANSWERS[][2] = {
        {(uint8_t)ZW_CC_SUPERVISION, ZW_CMD_SUPERVISION_REPORT},
        {(uint8_t)ZW_CC_VERSION, 0x12u},                      /* Version Report */
        {(uint8_t)ZW_CC_VERSION, 0x14u},                      /* Version Command Class Report */
        {(uint8_t)ZW_CC_MANUFACTURER_SPECIFIC, 0x05u},
        {(uint8_t)ZW_CC_ZWAVE_PLUS_INFO, 0x02u},
        {(uint8_t)ZW_CC_ASSOCIATION, 0x03u},                  /* Report */
        {(uint8_t)ZW_CC_ASSOCIATION, 0x06u},                  /* Supported Groupings Report */
        {(uint8_t)ZW_CC_ASSOCIATION, 0x0Cu},                  /* Specific Group Report */
        {(uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, 0x03u},
        {(uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, 0x06u},
        {(uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, 0x02u}, /* Name Report */
        {(uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, 0x04u}, /* Info Report */
        {(uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, 0x06u}, /* Command List Report */
        {(uint8_t)ZW_CC_TIME, 0x02u},                         /* Time Report */
        {(uint8_t)ZW_CC_TIME, 0x04u},                         /* Date Report */
    };
    for (size_t i = 0; i < sizeof ANSWERS / sizeof ANSWERS[0]; i++) {
        if (cmd[0] == ANSWERS[i][0] && cmd[1] == ANSWERS[i][1]) {
            return true;
        }
    }
    return false;
}

bool zw_s2_frame_is_reply(const zw_s2_t *s2, uint32_t tag)
{
    if ((tag & TAG_MARK_MASK) != TAG_MARK) {
        return false;
    }
    const unsigned kind = (unsigned)(tag >> 24) & 0xFu;
    const uint16_t stamp = (uint16_t)(tag >> 8);
    const size_t idx = tag & 0xFFu;
    if (kind == TAG_NONCE_REPORT) {
        return true;
    }
    return kind == TAG_SEND && idx < ZW_S2_SENDS && s2->sends[idx].state != S_FREE &&
           s2->sends[idx].gen == stamp && s2->sends[idx].reply;
}

void zw_s2_receive(zw_s2_t *s2, uint64_t now, uint16_t node, const uint8_t *cmd, size_t len)
{
    s2->now = now;
    if (!s2->started || cmd == NULL || len == 0u) {
        return;
    }
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL) {
        s2->dropped_unknown++;
        event(s2, node, ZW_S2_EV_UNKNOWN_NODE);
        return;
    }
    /* CRC-16 belongs outside Security 2 (Driver.ts isSecurityLevelTooLow unwraps it): a Security 2
     * frame inside one is taken as that frame */
    bool crc16 = false;
    zw_cc_unwrap_t u;
    if (cmd[0] == (uint8_t)ZW_CC_CRC16_ENCAPSULATION && zw_cc_unwrap(cmd, len, &u) &&
        u.kind == ZW_CC_UNWRAP_CRC16 && u.inner_len >= 2u && u.inner[0] == ZW_S2_CC &&
        (u.inner[1] == ZW_S2_NONCE_GET || u.inner[1] == ZW_S2_NONCE_REPORT ||
         u.inner[1] == ZW_S2_MESSAGE_ENCAPSULATION)) {
        cmd = u.inner;
        len = u.inner_len;
        crc16 = true;
    }
    if (cmd[0] == ZW_S2_CC && len >= 2u) {
        switch (cmd[1]) {
        case ZW_S2_NONCE_GET:
            on_nonce_get(s2, p, cmd, len);
            return;
        case ZW_S2_NONCE_REPORT:
            on_nonce_report(s2, p, cmd, len);
            return;
        case ZW_S2_MESSAGE_ENCAPSULATION:
            on_encap(s2, p, cmd, len, crc16);
            return;
        default:
            break; /* the key exchange's unencrypted commands: the rules below */
        }
    }
    (void)deliver(s2, p, ZW_SEC_NONE, false, false, false, cmd, len);
}

bool zw_s2_receive_group(zw_s2_t *s2, uint64_t now, uint16_t node, const uint8_t *cmd, size_t len)
{
    s2->now = now;
    if (!s2->started || cmd == NULL || len == 0u) {
        return false;
    }
    zw_s2_peer_t *p = find_peer(s2, node);
    if (p == NULL || (cmd[0] == ZW_S2_CC && len >= 2u &&
                      (cmd[1] == ZW_S2_NONCE_GET || cmd[1] == ZW_S2_NONCE_REPORT ||
                       cmd[1] == ZW_S2_MESSAGE_ENCAPSULATION))) {
        return false; /* S2 multicast (MPAN) is not received (v1); nonces are singlecast */
    }
    return deliver(s2, p, ZW_SEC_NONE, false, false, true, cmd, len);
}

void zw_s2_poll(zw_s2_t *s2, uint64_t now)
{
    s2->now = now;
    for (size_t i = 0; i < ZW_S2_SENDS; i++) {
        zw_s2_send_t *sl = &s2->sends[i];
        if (sl->state == S_WAIT_NONCE && now >= sl->deadline) {
            finish(s2, sl, ZW_S2_NO_NONCE);
        } else if (sl->state == S_GRACE && now >= sl->deadline) {
            finish(s2, sl, ZW_S2_OK);
        } else if (sl->state == S_JAM_WAIT && now >= sl->deadline) {
            jam_resend(s2, sl);
        }
    }
}

uint64_t zw_s2_next_deadline(const zw_s2_t *s2)
{
    uint64_t t = UINT64_MAX;
    for (size_t i = 0; i < ZW_S2_SENDS; i++) {
        const zw_s2_send_t *sl = &s2->sends[i];
        if ((sl->state == S_WAIT_NONCE || sl->state == S_GRACE || sl->state == S_JAM_WAIT) &&
            sl->deadline < t) {
            t = sl->deadline;
        }
    }
    return t;
}
