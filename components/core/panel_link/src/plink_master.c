/*
 * plink_master.c - the master's side of the panel link (see include/panel_link/plink_master.h;
 * docs/features/11; LINK-001, -006, -007, -014, -015, -016, -018, -021, -027).
 *
 * One plink_peer_t per device number (peer[d - 1]). A frame is checked and routed: PAIR_* to the
 * pairing state machine (plink_pair.c), HELLO to the handshake (plink_session.c), a sealed frame
 * to its keypad's live or pending session and then by type. Everything sent to a keypad goes to
 * the address of its last authenticated frame (LINK-021); a WELCOME goes to its HELLO's source
 * without moving it.
 *
 * m->pt (payloads) and m->out (frames) serve one send at a time: a handler decodes what it needs
 * from m->pt before it sends anything. port.run may call plink_master_result before it returns;
 * the answer is then kept and sent once port.run is back, so the port is never called from inside
 * one of its own calls.
 */
#include "panel_link/plink_master.h"

#include <string.h>

#include "panel_link/plink_keypad.h" /* how long a keypad resends a command */
#include "panel_link/plink_wire.h"

#define CLONE_MS           15000u /* a handshake from a new address this soon after the live one */
#define HINT_GAP_MS        1000u  /* NOT_PAIRED (all keypads) and REKEY (each keypad) */
#define CMD_BURST          5u     /* LINK-027: about 2 commands a second, a burst of 5 */
#define CMD_EVERY_MS       500u
#define REQ_BURST          32u    /* CFG_REQ, ZONES_REQ, EV_REQ: room for a whole configuration */
#define REQ_EVERY_MS       50u
#define BYE_COUNT          3u     /* LINK-014: three times over 2 s */
#define BYE_GAP_MS         1000u
#define BYE_REMOVED        1u
#define STORE_RETRY_MS     5000u
#define CLOCK_STEP_MS      2000u  /* a wall clock that moved further than this was set */
#define COUNTDOWN_SLACK_MS 500u   /* two snapshots whose countdowns end this close agree */
#define EVENTS_HDR         3u     /* EVENTS: flags, count, rec_len */
#define X25519_WINDOW_MS   1000u
#define HS_BURST           2u     /* one keypad's fresh handshakes: a burst of 2, then 1 a second, */
#define HS_EVERY_MS        1000u  /* so one keypad's keys can't use up the budget of all of them */

/* ---- small helpers --------------------------------------------------------------------------- */

static uint64_t min64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static uint64_t max64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
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

/* The wall clock at now from a reading `epoch` taken at `since` (0 stays 0: not set). */
static int64_t epoch_at(int64_t epoch, uint64_t since, uint64_t now)
{
    return epoch != 0 ? epoch + (int64_t)(now > since ? now - since : 0u) : 0;
}

/* A token bucket: burst tokens, one more every every_ms. false: none left (drop the request). */
static bool bucket_take(uint8_t *tokens, uint64_t *at, uint64_t now, uint8_t burst,
                        uint32_t every_ms)
{
    if (now > *at) {
        const uint64_t add = (now - *at) / every_ms;
        if ((uint64_t)*tokens + add >= burst) {
            *tokens = burst;
            *at = now;
        } else if (add > 0u) {
            *tokens = (uint8_t)(*tokens + add);
            *at += add * every_ms;
        }
    }
    if (*tokens == 0u) {
        return false;
    }
    (*tokens)--;
    return true;
}

static uint8_t dev_of(const plink_master_t *m, const plink_peer_t *p)
{
    return (uint8_t)((size_t)(p - m->peer) + PLINK_DEVICE_FIRST);
}

/* A paired keypad by number (not one being removed). */
static plink_peer_t *peer_of(plink_master_t *m, uint32_t device)
{
    if (device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST) {
        return NULL;
    }
    plink_peer_t *p = &m->peer[device - PLINK_DEVICE_FIRST];
    return p->used ? p : NULL;
}

/* A keypad the master talks to now: a live session and an address to send to. */
static bool is_live(const plink_peer_t *p)
{
    return p->used && p->live.valid && p->have_addr;
}

static bool is_provisional(const plink_peer_t *p)
{
    return (p->rec.flags & PLINK_PEER_PROVISIONAL) != 0u;
}

static void note(const plink_master_t *m, plink_note_t n, uint8_t device)
{
    if (m->port.note != NULL) {
        m->port.note(m->port.ctx, n, device);
    }
}

static void reachable(const plink_master_t *m, uint8_t device, bool yes)
{
    if (m->port.reachable != NULL) {
        m->port.reachable(m->port.ctx, device, yes);
    }
}

static void held_changed(const plink_master_t *m)
{
    if (m->port.held_changed != NULL) {
        m->port.held_changed(m->port.ctx, plink_master_held(m));
    }
}

/* The largest frame towards an address (the transport's, at most PLINK_FRAME_MAX). */
static size_t frame_cap(const plink_master_t *m, const plink_addr_t *to)
{
    size_t cap = PLINK_FRAME_MAX;
    if (m->port.mtu != NULL) {
        const size_t mtu = m->port.mtu(m->port.ctx, to->tp);
        if (mtu > 0u && mtu < cap) {
            cap = mtu;
        }
    }
    return cap;
}

/* The largest sealed payload towards keypad p. */
static size_t pt_cap(const plink_master_t *m, const plink_peer_t *p)
{
    const size_t cap = frame_cap(m, &p->addr);
    return cap > PLINK_SEALED_OVERHEAD ? cap - PLINK_SEALED_OVERHEAD : 0u;
}

/* ---- sending --------------------------------------------------------------------------------- */

static void send_frame(const plink_master_t *m, const plink_addr_t *to, const uint8_t *frame,
                       size_t len)
{
    if (len > 0u && m->port.send != NULL) {
        (void)m->port.send(m->port.ctx, to, frame, len);
    }
}

/* The pairing's answer, when it has one. */
static void send_pout(plink_master_t *m)
{
    if (m->pout.len > 0u) {
        send_frame(m, &m->pout.to, m->pout.buf, m->pout.len);
        m->pout.len = 0u;
    }
}

/* A plain hint (REKEY, NOT_PAIRED) whose payload of n bytes lies at m->out + PLINK_HDR_LEN. */
static void send_plain(plink_master_t *m, const plink_addr_t *to, uint8_t type, uint16_t net_id,
                       size_t n)
{
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.flags = PLINK_FLAG_FROM_MASTER;
    h.net_id = net_id;
    h.sender = PLINK_SENDER_MASTER;
    size_t len = 0u;
    if (plink_frame_plain(&h, &m->out[PLINK_HDR_LEN], n, m->out, sizeof m->out, &len)) {
        send_frame(m, to, m->out, len);
    }
}

/* The payload in m->pt (n bytes) sealed under p's live session and sent to its address. The seq
 * is spent even when sealing fails, so no nonce is ever tried twice (LINK-015). */
static bool send_sealed(plink_master_t *m, plink_peer_t *p, uint8_t type, size_t n)
{
    size_t len = 0u;
    if (!p->live.valid || !p->have_addr ||
        !plink_session_seal(m->c, &p->live, type, m->rec.net_id, PLINK_SENDER_MASTER, true, m->pt,
                            n, m->out, frame_cap(m, &p->addr), &len)) {
        return false;
    }
    send_frame(m, &p->addr, m->out, len);
    return true;
}

/* ---- the stored record ----------------------------------------------------------------------- */

/* The record built from the keypads (by number) and written now. The copy of the keys it took is
 * wiped afterwards: they live in the peers. */
static bool store_rec(plink_master_t *m)
{
    plink_master_rec_t *r = &m->rec;
    r->count = 0u;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (m->peer[i].used) {
            r->peer[r->count++] = m->peer[i].rec;
        }
    }
    const bool ok = m->port.store != NULL && m->port.store(m->port.ctx, r);
    plink_wipe(r->peer, sizeof r->peer);
    r->count = 0u;
    return ok;
}

/* After a change that must reach NVS: a failure is noted once and retried at tick. */
static bool store_or_retry(plink_master_t *m, uint64_t now)
{
    if (store_rec(m)) {
        m->store_due = false;
        return true;
    }
    if (!m->store_due) {
        note(m, PLINK_NOTE_STORE_FAILED, 0u);
    }
    m->store_due = true;
    m->store_at = now + STORE_RETRY_MS;
    return false;
}

/* ---- the keypads ----------------------------------------------------------------------------- */

/* A slot taken by a keypad from now: heard now (a grace of one supervision period), full rate
 * buckets, a provisional one's 10 minutes. */
static void peer_start(plink_peer_t *p, uint64_t now)
{
    p->used = true;
    p->heard_ms = now;
    p->cmd_tokens = CMD_BURST;
    p->cmd_tokens_at = now;
    p->requests = REQ_BURST;
    p->requests_at = now;
    p->hs_tokens = HS_BURST;
    p->hs_tokens_at = now;
    p->provisional_until = is_provisional(p) ? now + PLINK_PROVISIONAL_MS : 0u;
}

/* The slot's keys, sessions and everything else gone. */
static void peer_clear(plink_peer_t *p)
{
    plink_wipe(p, sizeof *p);
}

/* The pending handshake and its cached WELCOME discarded. */
static void pending_drop(plink_peer_t *p)
{
    plink_session_wipe(&p->pending);
    plink_wipe(p->welcome, sizeof p->welcome);
    plink_wipe(p->pending_hello, sizeof p->pending_hello);
    plink_wipe(p->pending_nk, sizeof p->pending_nk);
    p->welcome_len = 0u;
    p->pending_hello_len = 0u;
    p->pending_until = 0u;
}

/* Has this keypad's HELLO nonce been seen in the live session or the last 4 handshakes? */
static bool nk_seen(const plink_peer_t *p, const uint8_t n[PLINK_NONCE_LEN])
{
    bool seen = p->live.valid && memcmp(p->live_nk, n, PLINK_NONCE_LEN) == 0;
    for (size_t i = 0; i < p->seen_count && i < PLINK_HELLO_SEEN; i++) {
        seen = seen || memcmp(p->seen_nk[i], n, PLINK_NONCE_LEN) == 0;
    }
    return seen;
}

static void nk_remember(plink_peer_t *p, const uint8_t n[PLINK_NONCE_LEN])
{
    memcpy(p->seen_nk[p->seen_next % PLINK_HELLO_SEEN], n, PLINK_NONCE_LEN);
    p->seen_next = (uint8_t)((p->seen_next + 1u) % PLINK_HELLO_SEEN);
    if (p->seen_count < PLINK_HELLO_SEEN) {
        p->seen_count++;
    }
}

/* Numbers 1..8 nobody holds: neither the link (a keypad, or one still saying BYE) nor the Zigbee
 * keypads and the device table's entries (port.devices_taken). The lowest, 0 none (LINK-001). */
static uint8_t free_number(const plink_master_t *m)
{
    uint32_t taken = plink_master_held(m);
    if (m->port.devices_taken != NULL) {
        taken |= m->port.devices_taken(m->port.ctx);
    }
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        if ((taken & (1u << (d - 1u))) == 0u) {
            return d;
        }
    }
    return 0u;
}

/* ---- supervision (LINK-007) ------------------------------------------------------------------ */

static uint64_t period_ms(const plink_peer_t *p)
{
    return (uint64_t)p->rec.supervision_s * 1000u;
}

/* Level-based: a supervised keypad is down when nothing authenticated came from it for its
 * period, or it has reported a status older than 30 s for that long. Raising pauses while the
 * master's own network is down and counts again from when it came back; a keypad that is down
 * comes back up only when both directions are fresh again. */
static void supervise(plink_master_t *m, plink_peer_t *p, uint64_t now)
{
    if (!p->used || is_provisional(p)) {
        return;
    }
    const uint64_t per = period_ms(p);
    bool down;
    if (per == 0u) {
        down = false; /* not supervised */
    } else if (p->down) {
        down = now - p->heard_ms >= per || p->stale_since_ms != 0u;
    } else if (!m->net_up) {
        return; /* paused: the network trouble covers it */
    } else {
        const uint64_t heard = max64(p->heard_ms, m->net_up_since_ms);
        down = now >= heard + per;
        if (p->stale_since_ms != 0u) {
            down = down || now >= max64(p->stale_since_ms, m->net_up_since_ms) + per;
        }
    }
    if (down != p->down) {
        p->down = down;
        reachable(m, dev_of(m, p), !down);
    }
}

/* When supervision may next raise p's trouble (UINT64_MAX: not before something changes). */
static uint64_t supervise_next(const plink_master_t *m, const plink_peer_t *p)
{
    const uint64_t per = period_ms(p);
    if (!p->used || is_provisional(p) || per == 0u || p->down || !m->net_up) {
        return UINT64_MAX;
    }
    uint64_t t = max64(p->heard_ms, m->net_up_since_ms) + per;
    if (p->stale_since_ms != 0u) {
        t = min64(t, max64(p->stale_since_ms, m->net_up_since_ms) + per);
    }
    return t;
}

/* ---- status, zones, events, time (LINK-006) -------------------------------------------------- */

static bool about_same(uint32_t a, uint32_t b)
{
    return (a > b ? a - b : b - a) <= COUNTDOWN_SLACK_MS;
}

static bool part_same(const plink_part_t *a, const plink_part_t *b)
{
    return a->number == b->number && a->state == b->state && a->level == b->level &&
           a->arm_flags == b->arm_flags && a->armed_by == b->armed_by &&
           a->arm_src_kind == b->arm_src_kind && a->arm_src_id == b->arm_src_id &&
           a->ready_levels == b->ready_levels && a->flags == b->flags &&
           a->sounder == b->sounder && a->bell == b->bell &&
           about_same(a->delay_left_ms, b->delay_left_ms) && a->memory == b->memory &&
           a->faults == b->faults && a->bypassed == b->bypassed && a->alarms == b->alarms &&
           a->troubles == b->troubles && about_same(a->walk_left_ms, b->walk_left_ms) &&
           about_same(a->walk_age_ms, b->walk_age_ms) && a->walk_id == b->walk_id &&
           a->walk_seq == b->walk_seq && a->walk_tested == b->walk_tested &&
           a->walk_total == b->walk_total && a->walk_flags == b->walk_flags &&
           a->walk_started_by == b->walk_started_by && a->walk_ended_by == b->walk_ended_by &&
           a->walk_end_how == b->walk_end_how && a->walk_src_kind == b->walk_src_kind &&
           a->walk_src_id == b->walk_src_id;
}

/* Two STATUS built at the same moment say the same: every field but the countdowns equal, and
 * the countdowns ending within COUNTDOWN_SLACK_MS (a snapshot taken later counts the same
 * deadline from a later taken_ms). */
static bool status_same(const plink_status_t *a, const plink_status_t *b)
{
    if (a->partitions != b->partitions || a->sys_troubles != b->sys_troubles ||
        a->sys_restored != b->sys_restored || a->dev_troubles != b->dev_troubles ||
        a->default_codes != b->default_codes || a->flags != b->flags ||
        !about_same(a->recovery_left_ms, b->recovery_left_ms) || a->zones_used != b->zones_used ||
        a->last_event_seq != b->last_event_seq || a->zones_crc != b->zones_crc ||
        a->count != b->count) {
        return false;
    }
    for (size_t i = 0; i < a->count && i < PLINK_PARTS_MAX; i++) {
        if (!part_same(&a->part[i], &b->part[i])) {
            return false;
        }
    }
    return true;
}

static bool zone_same(const alarm_zone_status_t *a, const alarm_zone_status_t *b)
{
    return a->flags == b->flags && a->trouble == b->trouble && a->walk == b->walk &&
           a->walk_first_s == b->walk_first_s && a->walk_trips == b->walk_trips;
}

static uint16_t zones_used(const alarm_snapshot_t *s)
{
    return s->zones_used <= ALARM_MAX_ZONES ? s->zones_used : (uint16_t)ALARM_MAX_ZONES;
}

/* ev[0..n-1], oldest first, to p in as many EVENTS as it takes; flags (the gap) on the first. */
static void send_events(plink_master_t *m, plink_peer_t *p, const alarm_event_t *ev, size_t n,
                        uint8_t flags)
{
    const size_t cap = pt_cap(m, p);
    size_t fit = cap > EVENTS_HDR ? (cap - EVENTS_HDR) / PLINK_EVENT_REC_LEN : 0u;
    if (fit > 255u) {
        fit = 255u;
    }
    if (fit == 0u || n == 0u) {
        return;
    }
    size_t done = 0u;
    do {
        const size_t k = n - done < fit ? n - done : fit;
        size_t len = 0u, written = 0u;
        if (!plink_enc_events(&ev[done], k, flags, m->pt, cap, &len, &written) ||
            !send_sealed(m, p, PLINK_T_EVENTS, len)) {
            return;
        }
        done += k;
        flags = 0u;
    } while (done < n);
}

/* Events the keypad hasn't been sent yet, up to the status's last_event_seq, so the STATUS that
 * carries a seq never goes before its event (the platform normally hands each one in first). */
static void events_catch_up(plink_master_t *m, plink_peer_t *p)
{
    const uint32_t newest = m->snap.last_event_seq;
    if (newest <= p->ev_sent_seq) {
        return; /* sent already (perhaps before the snapshot that carries it) */
    }
    if (m->port.events_after != NULL) {
        bool gap = false;
        size_t n = m->port.events_after(m->port.ctx, p->ev_sent_seq, m->ev,
                                        sizeof m->ev / sizeof m->ev[0], &gap);
        if (n > sizeof m->ev / sizeof m->ev[0]) {
            n = sizeof m->ev / sizeof m->ev[0];
        }
        if (n > 0u) {
            send_events(m, p, m->ev, n, gap ? PLINK_EVF_GAP : 0u);
        }
    }
    p->ev_sent_seq = newest;
}

/* The zones keypad p can hold: its HELLO's capacity (0: not known, all), at most zones_used. */
static uint16_t zones_for(const plink_master_t *m, const plink_peer_t *p)
{
    const uint16_t used = zones_used(&m->snap);
    const uint16_t cap = p->live.peer_cap.zones;
    return cap != 0u && cap < used ? cap : used;
}

/* ZONES for zones first..last of the snapshot (clipped to the zones p can hold), in as many
 * frames as the transport needs; their zones_crc is the one p's STATUS carries. */
static void send_zones(plink_master_t *m, plink_peer_t *p, uint16_t first, uint16_t last)
{
    const uint16_t used = zones_for(m, p);
    const uint32_t crc = plink_zones_crc(m->snap.zone, used);
    if (first == 0u) {
        first = 1u;
    }
    if (last > used) {
        last = used;
    }
    const size_t cap = pt_cap(m, p);
    while (first <= last) {
        size_t len = 0u;
        uint16_t written = 0u;
        if (!plink_enc_zones(crc, first, &m->snap.zone[first - 1u],
                             (uint16_t)(last - first + 1u), m->pt, cap, &len, &written) ||
            written == 0u || !send_sealed(m, p, PLINK_T_ZONES, len)) {
            return;
        }
        first = (uint16_t)(first + written);
    }
}

/* What a STATUS carries besides the snapshot, for keypad p at now. */
static void status_extra(const plink_master_t *m, const plink_peer_t *p, uint64_t now,
                         plink_status_extra_t *x)
{
    memset(x, 0, sizeof *x);
    x->epoch_ms = epoch_at(m->epoch_ms, m->epoch_at_ms, now);
    x->home = p->rec.home;
    if (m->port.lockout_left != NULL) {
        x->lockout_left_ms = m->port.lockout_left(m->port.ctx, dev_of(m, p), now);
    }
    x->report = m->report;
    if (m->cfg != NULL) {
        x->cfg_crc = m->cfg_crc;
        x->cfg_len = (uint32_t)m->cfg_len;
    }
    x->zones_cap = p->live.peer_cap.zones; /* zones_crc over the zones it can hold */
}

/* p's STATUS now: first the events it carries and the zones that changed, then the status in
 * as many frames as the transport needs; the repeats of a change are planned from here. */
static void send_status(plink_master_t *m, plink_peer_t *p, uint64_t now)
{
    if (!m->have_snap || !is_live(p)) {
        return;
    }
    events_catch_up(m, p);
    if (p->zones_lo != 0u) {
        send_zones(m, p, p->zones_lo, p->zones_hi);
        p->zones_lo = p->zones_hi = 0u;
    }
    plink_status_extra_t x;
    status_extra(m, p, now, &x);
    plink_status_build(&m->snap, now, &x, &m->st_a);
    const size_t cap = pt_cap(m, p);
    uint8_t first = 0u, written = 0u;
    do {
        size_t len = 0u;
        if (!plink_enc_status(&m->st_a, first, m->pt, cap, &len, &written) ||
            !send_sealed(m, p, PLINK_T_STATUS, len)) {
            break;
        }
        first = (uint8_t)(first + written);
    } while (written > 0u && first < m->st_a.count);
    p->last_status_ms = now;
    p->status_due_ms = now + PLINK_STATUS_EVERY_MS;
    p->sent_zones_crc = m->zones_crc;
    if (p->change_ms != 0u) {
        p->change_ms = 0u;
        p->repeat1_ms = now + PLINK_STATUS_REPEAT1_MS;
        p->repeat2_ms = now + PLINK_STATUS_REPEAT2_MS;
    } else if (p->repeat1_ms != 0u && p->repeat1_ms <= now) {
        p->repeat1_ms = 0u;
    } else if (p->repeat2_ms != 0u && p->repeat2_ms <= now) {
        p->repeat2_ms = 0u;
    }
}

/* When p's next STATUS is due: a change, its repeats or the periodic one, no sooner than
 * PLINK_STATUS_MIN_GAP_MS after the last (LINK-006). */
static uint64_t status_next(const plink_peer_t *p)
{
    uint64_t t = p->status_due_ms;
    if (p->change_ms != 0u) {
        t = min64(t, p->change_ms);
    }
    if (p->repeat1_ms != 0u) {
        t = min64(t, p->repeat1_ms);
    }
    if (p->repeat2_ms != 0u) {
        t = min64(t, p->repeat2_ms);
    }
    if (p->last_status_ms != 0u) {
        t = max64(t, p->last_status_ms + PLINK_STATUS_MIN_GAP_MS);
    }
    return t;
}

static void status_changed(plink_peer_t *p, uint64_t now)
{
    if (p->change_ms == 0u) {
        p->change_ms = now != 0u ? now : 1u;
    }
}

static void status_service(plink_master_t *m, plink_peer_t *p, uint64_t now)
{
    if (m->have_snap && is_live(p) && now >= status_next(p)) {
        send_status(m, p, now);
    }
}

/* TIME to p (the wall clock now and the time zone); the next one in 10 min. */
static void send_time(plink_master_t *m, plink_peer_t *p, uint64_t now)
{
    p->time_due_ms = now + PLINK_TIME_EVERY_MS;
    plink_time_t t;
    memset(&t, 0, sizeof t);
    t.epoch_ms = m->time.epoch_ms != 0 ? epoch_at(m->time.epoch_ms, m->time_at_ms, now)
                                       : epoch_at(m->epoch_ms, m->epoch_at_ms, now);
    memcpy(t.tz, m->time.tz, sizeof t.tz);
    size_t len = 0u;
    if ((t.epoch_ms != 0 || t.tz[0] != '\0') && plink_enc_time(&t, m->pt, pt_cap(m, p), &len)) {
        (void)send_sealed(m, p, PLINK_T_TIME, len);
    }
}

/* ---- commands (LINK-018, LINK-027) ----------------------------------------------------------- */

#define CACHE_NONE PLINK_RESULTS_PER_KEYPAD

static bool cache_used(const plink_cached_t *e)
{
    return e->last_ms != 0u;
}

/* Entry i forgotten, and the slot of a command it still held. */
static void cache_clear(plink_peer_t *p, size_t i)
{
    plink_cached_t *e = &p->cache[i];
    if (e->pending && e->ticket != 0u && e->ticket == p->cmd_ticket) {
        p->cmd_running = false;
        p->cmd_ticket = 0u;
    }
    if (e->pending && e->ticket != 0u && e->ticket == p->panic_ticket) {
        p->panic_running = false;
        p->panic_ticket = 0u;
    }
    memset(e, 0, sizeof *e);
    p->result_due = (uint8_t)(p->result_due & ~(1u << i));
    p->cache_panic = (uint8_t)(p->cache_panic & ~(1u << i));
}

/* Answers kept 90 s after the last copy of their request; a command whose answer never came is
 * let go as long after its last copy (no keypad still resends it by then). */
static void cache_expire(plink_peer_t *p, uint64_t now)
{
    for (size_t i = 0; i < PLINK_RESULTS_PER_KEYPAD; i++) {
        if (cache_used(&p->cache[i]) && now >= p->cache[i].last_ms + PLINK_RESULT_KEEP_MS) {
            cache_clear(p, i);
        }
    }
}

static size_t cache_find(const plink_peer_t *p, uint32_t req)
{
    for (size_t i = 0; i < PLINK_RESULTS_PER_KEYPAD; i++) {
        if (cache_used(&p->cache[i]) && p->cache[i].req == req) {
            return i;
        }
    }
    return CACHE_NONE;
}

/* A slot for a new command: a free one, else the answer its keypad stopped resending the longest
 * ago. A command is resent for 3 s and a panic for 60 s (plink_keypad.h): an answer that may
 * still be asked for is never given up for another, so nothing runs twice (LINK-018).
 * CACHE_NONE: none now (the new command's own resend comes back). */
static size_t cache_slot(const plink_peer_t *p, uint64_t now)
{
    size_t best = CACHE_NONE;
    for (size_t i = 0; i < PLINK_RESULTS_PER_KEYPAD; i++) {
        const plink_cached_t *e = &p->cache[i];
        if (!cache_used(e)) {
            return i;
        }
        const uint8_t bit = (uint8_t)(1u << i);
        const uint64_t resent =
            (p->cache_panic & bit) != 0u ? PLINK_PANIC_KEEP_MS : PLINK_CMD_GIVE_UP_MS;
        if (!e->pending && (p->result_due & bit) == 0u && now >= e->last_ms + resent &&
            (best == CACHE_NONE || e->last_ms < p->cache[best].last_ms)) {
            best = i;
        }
    }
    return best;
}

static void send_result(plink_master_t *m, plink_peer_t *p, const plink_result_t *r)
{
    size_t len = 0u;
    if (plink_enc_result(r, m->pt, pt_cap(m, p), &len)) {
        (void)send_sealed(m, p, PLINK_T_RESULT, len);
    }
}

/* The answers that came back while port.run was running. */
static void results_flush(plink_master_t *m)
{
    for (size_t k = 0; k < PLINK_KEYPADS_MAX; k++) {
        plink_peer_t *p = &m->peer[k];
        for (size_t i = 0; p->used && p->result_due != 0u && i < PLINK_RESULTS_PER_KEYPAD; i++) {
            if ((p->result_due & (1u << i)) != 0u) {
                p->result_due = (uint8_t)(p->result_due & ~(1u << i));
                if (!p->cache[i].pending && cache_used(&p->cache[i])) {
                    send_result(m, p, &p->cache[i].res);
                }
            }
        }
        p->result_due = 0u;
    }
}

/* A command's checks before anything runs (LINK-017, LINK-027): an op this master doesn't
 * serve is UNAVAILABLE, an unknown command BAD_ARG, a partition other than the keypad's home
 * NOT_AUTHORIZED. */
static alarm_result_t cmd_check(const plink_peer_t *p, const plink_cmd_t *c)
{
    if (c->op != PLINK_OP_COMMAND && c->op != PLINK_OP_AUTH && c->op != PLINK_OP_CHANGE_CODE) {
        return ALARM_ERR_UNAVAILABLE;
    }
    if (c->op == PLINK_OP_COMMAND && (unsigned)c->cmd.type > (unsigned)ALARM_CMD_CODE_CHECK) {
        return ALARM_ERR_BAD_ARG;
    }
    if (c->cmd.partition != p->rec.home) {
        return ALARM_ERR_NOT_AUTHORIZED;
    }
    return ALARM_OK;
}

static void answer_now(plink_master_t *m, plink_peer_t *p, uint32_t req, alarm_result_t r)
{
    plink_result_t res;
    memset(&res, 0, sizeof res);
    res.req = req;
    res.result = (uint8_t)r;
    send_result(m, p, &res);
}

/* A new command handed to port.run with a fresh ticket in cache slot i, pending until the answer
 * comes back (perhaps from inside port.run: then it is sent once port.run is back). */
static void cmd_run(plink_master_t *m, plink_peer_t *p, uint64_t now, size_t i,
                    const plink_cmd_t *cmd, const uint8_t digest[PLINK_SHA256_LEN], bool panic)
{
    cache_clear(p, i);
    uint32_t ticket = ++m->next_ticket;
    if (ticket == 0u) {
        ticket = ++m->next_ticket;
    }
    plink_cached_t *e = &p->cache[i];
    e->req = cmd->req;
    memcpy(e->digest, digest, PLINK_SHA256_LEN);
    e->pending = true;
    e->ticket = ticket;
    e->last_ms = now;
    if (panic) {
        p->cache_panic = (uint8_t)(p->cache_panic | (1u << i));
        p->panic_running = true;
        p->panic_ticket = ticket;
    } else {
        p->cmd_running = true;
        p->cmd_ticket = ticket;
    }
    m->in_run = true;
    const bool ok = m->port.run(m->port.ctx, ticket, dev_of(m, p), cmd);
    m->in_run = false;
    if (!ok && e->pending && e->ticket == ticket) {
        cache_clear(p, i); /* no room now: forgotten, so the keypad's resend runs it */
    }
    results_flush(m);
}

/* A command from p's live session: a copy of one already taken is answered from the cache (or
 * waits for its answer); a new one is checked, held to one at a time (panic in a slot of its
 * own) and to about 2 a second, then run. */
static void cmd_take(plink_master_t *m, plink_peer_t *p, uint64_t now, const plink_cmd_t *cmd)
{
    uint8_t digest[PLINK_SHA256_LEN];
    plink_cmd_digest(cmd, digest);
    const bool panic = cmd->op == PLINK_OP_COMMAND && cmd->cmd.type == ALARM_CMD_PANIC;
    cache_expire(p, now);
    const size_t found = cache_find(p, cmd->req);
    if (found != CACHE_NONE) {
        plink_cached_t *e = &p->cache[found];
        if (memcmp(e->digest, digest, sizeof digest) == 0) {
            /* LINK-018: a command runs once; its copies get its answer, or wait for it. */
            e->last_ms = now;
            if (!e->pending) {
                send_result(m, p, &e->res);
            }
            return;
        }
        if (e->pending) {
            return; /* the same req for another command: once the first is answered */
        }
        cache_clear(p, found); /* the same req with another command is a new one */
    }
    const alarm_result_t why = cmd_check(p, cmd);
    if (why != ALARM_OK) {
        answer_now(m, p, cmd->req, why);
        return;
    }
    /* LINK-027: over these limits the request is dropped here, before anything reaches the
     * alarm task; the keypad's resend comes back. */
    const size_t slot = cache_slot(p, now);
    if (m->port.run == NULL || slot == CACHE_NONE ||
        (panic ? p->panic_running
               : p->cmd_running || !bucket_take(&p->cmd_tokens, &p->cmd_tokens_at, now,
                                                CMD_BURST, CMD_EVERY_MS))) {
        return;
    }
    cmd_run(m, p, now, slot, cmd, digest, panic);
}

/* A CMD: decoded, taken, and every copy of its codes wiped (LINK-018). */
static void on_cmd(plink_master_t *m, plink_peer_t *p, uint64_t now, size_t n)
{
    plink_cmd_t cmd;
    const bool ok = plink_dec_cmd(m->pt, n, &cmd);
    plink_wipe(m->pt, n);
    if (ok) {
        cmd_take(m, p, now, &cmd);
    }
    plink_wipe(&cmd, sizeof cmd);
}

void plink_master_result(plink_master_t *m, uint64_t now_ms, uint32_t ticket,
                         const plink_result_t *res)
{
    if (m == NULL || res == NULL || ticket == 0u) {
        return;
    }
    for (size_t k = 0; k < PLINK_KEYPADS_MAX; k++) {
        plink_peer_t *p = &m->peer[k];
        if (p->cmd_ticket == ticket) {
            p->cmd_running = false;
            p->cmd_ticket = 0u;
        }
        if (p->panic_ticket == ticket) {
            p->panic_running = false;
            p->panic_ticket = 0u;
        }
        for (size_t i = 0; p->used && i < PLINK_RESULTS_PER_KEYPAD; i++) {
            plink_cached_t *e = &p->cache[i];
            if (!cache_used(e) || !e->pending || e->ticket != ticket) {
                continue;
            }
            e->res = *res;
            e->res.req = e->req;
            e->pending = false;
            e->ticket = 0u;
            e->last_ms = max64(e->last_ms, now_ms);
            if (m->in_run) {
                p->result_due = (uint8_t)(p->result_due | (1u << i)); /* sent once run is back */
            } else {
                send_result(m, p, &e->res);
            }
            return;
        }
    }
}

/* ---- requests: configuration, zones, events -------------------------------------------------- */

/* A CFG_REQ: a page of the keypads' configuration copy. A request for another blob (an older
 * cfg_crc) gets the current one from offset 0, so the keypad starts again. */
static void on_cfg_req(plink_master_t *m, plink_peer_t *p, size_t n)
{
    plink_cfg_req_t r;
    if (!plink_dec_cfg_req(m->pt, n, &r) || m->cfg == NULL || m->cfg_len == 0u ||
        m->cfg_len > UINT32_MAX) {
        return;
    }
    const uint32_t total = (uint32_t)m->cfg_len;
    uint32_t offset = r.offset;
    if (r.cfg_crc != m->cfg_crc || offset > total) {
        offset = 0u;
    }
    const size_t cap = pt_cap(m, p);
    size_t len = total - offset;
    const size_t room = cap > PLINK_CFG_PAGE_HDR ? cap - PLINK_CFG_PAGE_HDR : 0u;
    if (len > room) {
        len = room;
    }
    if (r.max != 0u && len > r.max) {
        len = r.max;
    }
    plink_cfg_page_t pg;
    memset(&pg, 0, sizeof pg);
    pg.cfg_crc = m->cfg_crc;
    pg.total = total;
    pg.offset = offset;
    pg.data = m->cfg + offset;
    pg.len = (uint16_t)(len > UINT16_MAX ? UINT16_MAX : len);
    size_t plen = 0u;
    if (plink_enc_cfg_page(&pg, m->pt, cap, &plen)) {
        (void)send_sealed(m, p, PLINK_T_CFG_PAGE, plen);
    }
}

static void on_zones_req(plink_master_t *m, plink_peer_t *p, size_t n)
{
    plink_zones_req_t r;
    if (!plink_dec_zones_req(m->pt, n, &r) || !m->have_snap || r.count == 0u) {
        return;
    }
    const uint16_t first = r.first != 0u ? r.first : 1u;
    const uint32_t last = (uint32_t)first + r.count - 1u;
    send_zones(m, p, first, (uint16_t)(last > UINT16_MAX ? UINT16_MAX : last));
}

/* An EV_REQ: the newest `max` events after after_seq, as many as one frame holds (the keypad
 * asks again for the rest). */
static void on_ev_req(plink_master_t *m, plink_peer_t *p, size_t n)
{
    plink_ev_req_t r;
    if (!plink_dec_ev_req(m->pt, n, &r) || r.max == 0u) {
        return;
    }
    const size_t cap = pt_cap(m, p);
    size_t k = cap > EVENTS_HDR ? (cap - EVENTS_HDR) / PLINK_EVENT_REC_LEN : 0u;
    if (k > sizeof m->ev / sizeof m->ev[0]) {
        k = sizeof m->ev / sizeof m->ev[0];
    }
    if (k > r.max) {
        k = r.max;
    }
    bool gap = false;
    size_t got = 0u;
    if (m->port.events_after != NULL && k > 0u) {
        got = m->port.events_after(m->port.ctx, r.after_seq, m->ev, k, &gap);
        if (got > k) {
            got = k;
        }
    }
    size_t len = 0u, written = 0u;
    if (plink_enc_events(m->ev, got, gap ? PLINK_EVF_GAP : 0u, m->pt, cap, &len, &written)) {
        (void)send_sealed(m, p, PLINK_T_EVENTS, len);
    }
}

static void on_heartbeat(plink_master_t *m, plink_peer_t *p, uint64_t now, size_t n)
{
    plink_heartbeat_t hb;
    if (!plink_dec_heartbeat(m->pt, n, &hb)) {
        return;
    }
    p->hb = hb;
    p->status_age_s = hb.status_age_s;
    /* No status yet (0xFFFF) or one older than 30 s: the master's direction isn't fresh. */
    if (hb.status_age_s > PLINK_STATUS_STALE_S) {
        if (p->stale_since_ms == 0u) {
            p->stale_since_ms = now;
        }
    } else {
        p->stale_since_ms = 0u;
    }
    /* A check-in says both directions work: the engine's check-in clears a device trouble, so a
     * keypad that is down, or reports a stale status (LINK-007's second condition), isn't checked
     * in - its trouble clears through supervise once both directions are fresh again. */
    if (m->port.checkin != NULL && !p->down && p->stale_since_ms == 0u) {
        m->port.checkin(m->port.ctx, dev_of(m, p));
    }
}

/* ---- sessions (LINK-015) --------------------------------------------------------------------- */

/* The pending session proved itself with a sealed frame from `from`: it becomes the live one
 * (the old one wiped) and the keypad's address moves there. STATUS and TIME follow once the frame
 * is handled. */
static void promote(plink_master_t *m, plink_peer_t *p, uint64_t now, const plink_addr_t *from)
{
    const uint8_t device = dev_of(m, p);
    if (p->have_addr && !plink_addr_equal(from, &p->addr) && p->live.valid &&
        p->live_heard_ms != 0u && now - p->live_heard_ms < CLONE_MS) {
        note(m, PLINK_NOTE_CLONE, device); /* a second device using keypad n's keys? */
    }
    plink_session_wipe(&p->live);
    p->live = p->pending;
    memcpy(p->live_nk, p->pending_nk, PLINK_NONCE_LEN);
    pending_drop(p);
    p->addr = *from;
    p->have_addr = true;
    p->ev_sent_seq = m->have_snap ? m->snap.last_event_seq : 0u;
    p->zones_lo = p->zones_hi = 0u;
    p->change_ms = p->repeat1_ms = p->repeat2_ms = 0u;
    p->last_status_ms = 0u;
    p->rekey_at = 0u;
    if (is_provisional(p)) {
        /* Its first handshake: supervised from now on, and kept across a restart. */
        p->rec.flags = (uint8_t)(p->rec.flags & ~PLINK_PEER_PROVISIONAL);
        p->provisional_until = 0u;
        (void)store_or_retry(m, now);
    }
}

/* A sealed frame of a session this master doesn't know: a hint to start a handshake, to its
 * source, at most once a second per keypad. */
static void send_rekey(plink_master_t *m, plink_peer_t *p, uint64_t now, const plink_addr_t *to,
                       uint32_t sid)
{
    if (p->rekey_at != 0u && now < p->rekey_at + HINT_GAP_MS) {
        return;
    }
    p->rekey_at = now;
    const plink_rekey_t r = {sid};
    size_t n = 0u;
    if (plink_enc_rekey(&r, &m->out[PLINK_HDR_LEN], sizeof m->out - PLINK_HDR_LEN, &n)) {
        send_plain(m, to, PLINK_T_REKEY, m->rec.net_id, n);
    }
}

/* A HELLO the master can't place: a hint, at most once a second whoever sends them. */
static void send_not_paired(plink_master_t *m, uint64_t now, const plink_addr_t *to,
                            uint16_t net_id)
{
    if (m->not_paired_at != 0u && now < m->not_paired_at + HINT_GAP_MS) {
        return;
    }
    m->not_paired_at = now;
    send_plain(m, to, PLINK_T_NOT_PAIRED, net_id, 0u);
}

/* At most about 4 handshakes a second over all keypads (each costs two X25519). */
static bool x25519_budget(plink_master_t *m, uint64_t now)
{
    if (m->x25519_count == 0u || now >= m->x25519_window_ms + X25519_WINDOW_MS) {
        m->x25519_window_ms = now;
        m->x25519_count = 0u;
    }
    if (m->x25519_count >= PLINK_X25519_PER_S) {
        return false;
    }
    m->x25519_count++;
    return true;
}

static void on_hello(plink_master_t *m, uint64_t now, const plink_addr_t *from,
                     const plink_hdr_t *h, const uint8_t *frame, size_t len)
{
    plink_peer_t *p = NULL;
    if (m->rec.net_id != 0u && h->net_id == m->rec.net_id) {
        p = peer_of(m, h->sender);
    }
    /* mac1 first: a HELLO not made with this keypad's key costs no X25519. */
    if (p == NULL || !plink_hs_hello_valid(p->rec.k_pair, frame, len)) {
        send_not_paired(m, now, from, h->net_id);
        return;
    }
    plink_hello_t hello;
    if (!plink_dec_hello(&frame[PLINK_HDR_LEN], len - PLINK_HDR_LEN, true, &hello)) {
        return;
    }
    /* The same HELLO again while its handshake is pending: its WELCOME again (it was lost). */
    if (p->pending.valid && now < p->pending_until && p->pending_hello_len == len &&
        memcmp(p->pending_hello, frame, len) == 0) {
        send_frame(m, from, p->welcome, p->welcome_len);
        return;
    }
    /* A replayed HELLO never reaches X25519, and never touches the live session. One keypad's
     * fresh HELLOs - a device with its keys flooding them - get only its own share of the
     * budget, so the other keypads' handshakes still go through (LINK-027). */
    if (nk_seen(p, hello.n) ||
        !bucket_take(&p->hs_tokens, &p->hs_tokens_at, now, HS_BURST, HS_EVERY_MS)) {
        return;
    }
    if (!x25519_budget(m, now)) {
        p->hs_tokens++; /* refused for the others' sake, not its own: its token goes back */
        return;
    }
    plink_session_t s;
    plink_hello_t in;
    size_t wl = 0u;
    if (!plink_hs_answer(m->c, p->rec.k_pair, h, frame, len, &s, &in, m->out, sizeof m->out,
                         &wl) ||
        wl > sizeof p->welcome || (p->live.valid && s.sid == p->live.sid)) {
        plink_session_wipe(&s);
        return;
    }
    /* The new pending session replaces an older one; the live one stays until this proves
     * itself with a sealed frame. */
    pending_drop(p);
    p->pending = s;
    plink_session_wipe(&s);
    memcpy(p->pending_nk, hello.n, PLINK_NONCE_LEN);
    nk_remember(p, hello.n);
    memcpy(p->welcome, m->out, wl);
    p->welcome_len = wl;
    if (len <= sizeof p->pending_hello) {
        memcpy(p->pending_hello, frame, len);
        p->pending_hello_len = len;
    }
    p->pending_until = now + PLINK_PENDING_MS;
    send_frame(m, from, p->welcome, wl);
}

/* A sealed frame from a keypad: opened under its live session or its pending one (which it then
 * proves), its source becomes the keypad's address, then the frame is handled. */
static void on_sealed(plink_master_t *m, uint64_t now, const plink_addr_t *from,
                      const plink_hdr_t *h, const uint8_t *frame, size_t len)
{
    if (m->rec.net_id == 0u || h->net_id != m->rec.net_id) {
        return;
    }
    plink_peer_t *p = peer_of(m, h->sender);
    if (p == NULL) {
        return; /* not paired, or removed: refused */
    }
    plink_session_t *s;
    bool promoting = false;
    if (p->live.valid && h->sid == p->live.sid) {
        s = &p->live;
    } else if (p->pending.valid && h->sid == p->pending.sid && now < p->pending_until) {
        s = &p->pending;
        promoting = true;
    } else {
        send_rekey(m, p, now, from, h->sid);
        return;
    }
    size_t n = 0u;
    if (!plink_session_open(m->c, s, h, frame, len, m->pt, sizeof m->pt, &n)) {
        return; /* a replay, a forgery or damage: nothing moves (LINK-003, LINK-021) */
    }
    if (promoting) {
        promote(m, p, now, from);
    } else if (!p->have_addr || !plink_addr_equal(from, &p->addr)) {
        p->addr = *from; /* LINK-021: only an authenticated frame moves the address */
        p->have_addr = true;
        p->addr_flips++;
    }
    p->heard_ms = now;
    p->live_heard_ms = now;
    switch (h->type) {
    case PLINK_T_HEARTBEAT: on_heartbeat(m, p, now, n); break;
    case PLINK_T_CMD: on_cmd(m, p, now, n); break;
    case PLINK_T_CFG_REQ:
    case PLINK_T_ZONES_REQ:
    case PLINK_T_EV_REQ:
        /* Requests over their limit are dropped: the keypad's retry comes back. */
        if (bucket_take(&p->requests, &p->requests_at, now, REQ_BURST, REQ_EVERY_MS)) {
            if (h->type == PLINK_T_CFG_REQ) {
                on_cfg_req(m, p, n);
            } else if (h->type == PLINK_T_ZONES_REQ) {
                on_zones_req(m, p, n);
            } else {
                on_ev_req(m, p, n);
            }
        }
        break;
    default: break; /* LINK-017: nothing else is the master's to take */
    }
    plink_wipe(m->pt, n);
    if (promoting && p->used) {
        /* A new session: STATUS and TIME at once, the STATUS repeated as a change's is (LINK-006):
         * the keypad has dropped the old session, and until one of them arrives it has no status
         * (CONNECTING at start-up, or nearer to LOST). */
        status_changed(p, now);
        send_status(m, p, now);
        send_time(m, p, now);
    }
    supervise(m, p, now);
}

/* ---- pairing (LINK-001, LINK-004, LINK-016) -------------------------------------------------- */

static void pair_identity(plink_master_t *m)
{
    plink_pm_identity(&m->pair, m->rec.net_id, m->name, m->panel_id,
                      m->have_snap ? zones_used(&m->snap) : 0u);
}

/* Both confirmations verified and the installer allowed it: the keypad takes the lowest free
 * number, is stored as provisional, and PAIR_DONE goes (or PAIR_FAIL when no number is free or
 * the record can't be written). */
static void pair_take(plink_master_t *m, uint64_t now)
{
    plink_pair_new_t nw;
    if (!plink_pm_take(&m->pair, &nw)) {
        return;
    }
    const uint8_t d = free_number(m);
    if (d == 0u) {
        plink_pm_stored(&m->pair, now, false, PLINK_FAIL_FULL, 0u, 0u, &m->pout);
        send_pout(m);
        plink_wipe(&nw, sizeof nw);
        return;
    }
    uint8_t home = 1u;
    while (m->have_snap && home < ALARM_MAX_PARTITIONS &&
           (m->snap.partitions & (1u << (home - 1u))) == 0u) {
        home++;
    }
    if (m->have_snap && (m->snap.partitions & (1u << (home - 1u))) == 0u) {
        home = 1u; /* no partition enabled: the first */
    }
    plink_peer_t *p = &m->peer[d - 1u];
    peer_clear(p);
    p->rec.device = d;
    p->rec.home = home;
    p->rec.kind = nw.kind;
    p->rec.flags = PLINK_PEER_PROVISIONAL;
    p->rec.supervision_s = PLINK_SUPERVISION_DEFAULT_S;
    memcpy(p->rec.k_pair, nw.k_pair, PLINK_KEY_LEN);
    p->rec.paired_epoch_ms = epoch_at(m->epoch_ms, m->epoch_at_ms, now);
    /* No name: a new keypad is "Keypad N" until renamed on the alarm panel (decided 2026-10-15).
     * The name it gave itself was shown only while pairing, next to the code. */
    memset(p->rec.name, 0, sizeof p->rec.name);
    plink_wipe(&nw, sizeof nw);
    peer_start(p, now);
    if (!store_rec(m)) {
        peer_clear(p);
        note(m, PLINK_NOTE_STORE_FAILED, d);
        plink_pm_stored(&m->pair, now, false, PLINK_FAIL_STORE, 0u, 0u, &m->pout);
        send_pout(m);
        return;
    }
    plink_pm_stored(&m->pair, now, true, PLINK_FAIL_NONE, d, home, &m->pout);
    send_pout(m);
    note(m, PLINK_NOTE_PAIRED, d);
    held_changed(m);
}

bool plink_master_pair_open(plink_master_t *m, uint64_t now_ms)
{
    if (m == NULL || free_number(m) == 0u) {
        return false;
    }
    if (m->rec.net_id == 0u) {
        /* The first pairing chooses the network's id and stores it before anything uses it. */
        uint16_t id = 0u;
        for (int tries = 0; tries < 4 && id == 0u; tries++) {
            uint8_t b[2];
            if (!plink_random(m->c, b, sizeof b)) {
                return false;
            }
            id = (uint16_t)((unsigned)b[0] | ((unsigned)b[1] << 8));
        }
        if (id == 0u) {
            return false;
        }
        m->rec.net_id = id;
        if (!store_rec(m)) {
            m->rec.net_id = 0u;
            note(m, PLINK_NOTE_STORE_FAILED, 0u);
            return false;
        }
    }
    pair_identity(m);
    plink_pm_open(&m->pair, now_ms);
    return true;
}

void plink_master_pair_cancel(plink_master_t *m, uint64_t now_ms)
{
    if (m == NULL) {
        return;
    }
    plink_pm_cancel(&m->pair, now_ms, &m->pout);
    send_pout(m);
}

void plink_master_pair_decide(plink_master_t *m, uint64_t now_ms, bool allow)
{
    if (m == NULL) {
        return;
    }
    plink_pm_decide(&m->pair, now_ms, allow, &m->pout);
    send_pout(m);
    pair_take(m, now_ms);
}

void plink_master_pair_info(const plink_master_t *m, uint64_t now_ms, plink_pair_info_t *out)
{
    if (out == NULL) {
        return;
    }
    if (m == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    plink_pm_info(&m->pair, now_ms, out);
}

bool plink_master_pairing(const plink_master_t *m)
{
    if (m == NULL) {
        return false;
    }
    const uint8_t st = m->pair.state;
    return m->pair.window || st == PLINK_PS_WORKING || st == PLINK_PS_COMPARE ||
           st == PLINK_PS_WAITING;
}

/* ---- the frame entry point and timers -------------------------------------------------------- */

void plink_master_init(plink_master_t *m, const plink_crypto_t *c, const plink_master_port_t *port,
                       const plink_master_rec_t *rec, const char *name,
                       const char panel_id[PLINK_PANEL_ID_LEN + 1], uint64_t now_ms)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof *m);
    m->c = c;
    if (port != NULL) {
        m->port = *port;
    }
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), m->name, sizeof m->name);
    plink_clean_text(panel_id, bounded_len(panel_id, PLINK_PANEL_ID_LEN), m->panel_id,
                     sizeof m->panel_id);
    m->net_up = true;
    m->net_up_since_ms = now_ms;
    plink_pm_init(&m->pair, c);
    if (rec != NULL && rec->net_id != 0u) {
        m->rec.net_id = rec->net_id;
        for (size_t i = 0; i < rec->count && i < PLINK_KEYPADS_MAX; i++) {
            const plink_peer_rec_t *r = &rec->peer[i];
            if (r->device < PLINK_DEVICE_FIRST || r->device > PLINK_DEVICE_LAST ||
                m->peer[r->device - 1u].used) {
                continue;
            }
            plink_peer_t *p = &m->peer[r->device - 1u];
            p->rec = *r;
            if (p->rec.home < 1u || p->rec.home > ALARM_MAX_PARTITIONS) {
                p->rec.home = 1u;
            }
            peer_start(p, now_ms); /* heard now: the engine's grace at start-up */
        }
    }
    pair_identity(m);
}

void plink_master_input(plink_master_t *m, uint64_t now_ms, const plink_addr_t *from,
                        const uint8_t *frame, size_t len)
{
    plink_hdr_t h;
    if (m == NULL || from == NULL || !plink_hdr_read(frame, len, &h) ||
        (h.flags & PLINK_FLAG_FROM_MASTER) != 0u) {
        return; /* not a frame of ours, or one from a master */
    }
    switch (h.type) {
    case PLINK_T_PAIR_REQ:
    case PLINK_T_PAIR_NONCE:
    case PLINK_T_PAIR_CONFIRM:
    case PLINK_T_PAIR_FAIL:
        plink_pm_input(&m->pair, now_ms, from, &h, frame, len, &m->pout);
        send_pout(m);
        pair_take(m, now_ms);
        break;
    case PLINK_T_HELLO: on_hello(m, now_ms, from, &h, frame, len); break;
    default:
        if ((h.flags & PLINK_FLAG_SEALED) != 0u) {
            on_sealed(m, now_ms, from, &h, frame, len);
        }
        break;
    }
}

/* A removed keypad's BYEs: three, a second apart; then its session goes and its number is free. */
static void bye_service(plink_master_t *m, plink_peer_t *p, uint64_t now)
{
    if (p->bye_left > 0u && now >= p->bye_at) {
        plink_bye_t b;
        b.reason = BYE_REMOVED;
        size_t n = 0u;
        if (plink_enc_bye(&b, m->pt, pt_cap(m, p), &n)) {
            (void)send_sealed(m, p, PLINK_T_BYE, n);
        }
        p->bye_left--;
        p->bye_at = now + BYE_GAP_MS;
    }
    if (p->bye_left == 0u) {
        peer_clear(p);
        held_changed(m);
    }
}

/* A provisional keypad that never came: its trouble cleared (it never had one), its number
 * freed. */
static void provisional_drop(plink_master_t *m, plink_peer_t *p, uint64_t now)
{
    const uint8_t d = dev_of(m, p);
    reachable(m, d, true);
    peer_clear(p);
    note(m, PLINK_NOTE_PROVISIONAL_DROPPED, d);
    (void)store_or_retry(m, now);
    held_changed(m);
}

void plink_master_tick(plink_master_t *m, uint64_t now_ms)
{
    if (m == NULL) {
        return;
    }
    plink_pm_tick(&m->pair, now_ms, &m->pout);
    send_pout(m);
    if (m->store_due && now_ms >= m->store_at) {
        (void)store_or_retry(m, now_ms);
    }
    results_flush(m);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_peer_t *p = &m->peer[i];
        if (p->removing) {
            bye_service(m, p, now_ms);
            continue;
        }
        if (!p->used) {
            continue;
        }
        if (p->pending.valid && now_ms >= p->pending_until) {
            pending_drop(p);
        }
        if (is_provisional(p) && now_ms >= p->provisional_until) {
            provisional_drop(m, p, now_ms);
            continue;
        }
        cache_expire(p, now_ms);
        status_service(m, p, now_ms);
        if (is_live(p) && now_ms >= p->time_due_ms) {
            send_time(m, p, now_ms);
        }
        supervise(m, p, now_ms);
    }
}

uint64_t plink_master_next(const plink_master_t *m)
{
    if (m == NULL) {
        return UINT64_MAX;
    }
    uint64_t t = plink_pm_next(&m->pair);
    if (m->store_due) {
        t = min64(t, m->store_at);
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_peer_t *p = &m->peer[i];
        if (p->removing) {
            t = min64(t, p->bye_left > 0u ? p->bye_at : 0u);
            continue;
        }
        if (!p->used) {
            continue;
        }
        if (p->result_due != 0u) {
            t = 0u;
        }
        if (p->pending.valid) {
            t = min64(t, p->pending_until);
        }
        if (is_provisional(p)) {
            t = min64(t, p->provisional_until);
        }
        for (size_t k = 0; k < PLINK_RESULTS_PER_KEYPAD; k++) {
            if (cache_used(&p->cache[k])) {
                t = min64(t, p->cache[k].last_ms + PLINK_RESULT_KEEP_MS);
            }
        }
        if (is_live(p)) {
            if (m->have_snap) {
                t = min64(t, status_next(p));
            }
            t = min64(t, p->time_due_ms);
        }
        t = min64(t, supervise_next(m, p));
    }
    return t;
}

/* ---- what the platform hands in -------------------------------------------------------------- */

void plink_master_snapshot(plink_master_t *m, uint64_t now_ms, const alarm_snapshot_t *snap,
                           int64_t epoch_ms, uint8_t report)
{
    if (m == NULL || snap == NULL) {
        return;
    }
    const bool first = !m->have_snap;
    bool changed = first || report != m->report;
    uint16_t lo = 0u, hi = 0u;
    if (!first) {
        /* Both built at now: a countdown only reads differently when its deadline moved. The
         * snapshot's seq can't tell: it rises after every engine call. */
        plink_status_extra_t x;
        memset(&x, 0, sizeof x);
        plink_status_build(&m->snap, now_ms, &x, &m->st_a);
        plink_status_build(snap, now_ms, &x, &m->st_b);
        changed = changed || !status_same(&m->st_a, &m->st_b);
        const uint16_t a = zones_used(&m->snap), b = zones_used(snap);
        for (uint16_t z = 1u; z <= (a > b ? a : b); z++) {
            if (!zone_same(&m->snap.zone[z - 1u], &snap->zone[z - 1u])) {
                lo = lo == 0u ? z : lo;
                hi = z;
            }
        }
    }
    const bool zones_moved = first || zones_used(&m->snap) != zones_used(snap);
    m->snap = *snap;
    m->have_snap = true;
    m->epoch_ms = epoch_ms;
    m->epoch_at_ms = now_ms;
    m->report = report;
    m->zones_crc = plink_zones_crc(m->snap.zone, zones_used(&m->snap));
    if (zones_moved) {
        pair_identity(m); /* a keypad that can't hold the zones is refused at pairing */
    }
    if (!changed) {
        return;
    }
    m->snap_changed_ms = now_ms;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_peer_t *p = &m->peer[i];
        if (!is_live(p)) {
            continue;
        }
        if (first) {
            p->ev_sent_seq = m->snap.last_event_seq; /* the keypad asks for what it lacks */
        }
        if (lo != 0u) {
            p->zones_lo = p->zones_lo == 0u || lo < p->zones_lo ? lo : p->zones_lo;
            p->zones_hi = hi > p->zones_hi ? hi : p->zones_hi;
        }
        status_changed(p, now_ms);
        status_service(m, p, now_ms);
    }
}

void plink_master_config(plink_master_t *m, const uint8_t *blob, size_t len, uint32_t crc)
{
    if (m == NULL) {
        return;
    }
    const uint8_t *cfg = len > 0u ? blob : NULL;
    const size_t cfg_len = cfg != NULL ? len : 0u;
    const bool changed =
        crc != m->cfg_crc || cfg_len != m->cfg_len || (cfg == NULL) != (m->cfg == NULL);
    m->cfg = cfg;
    m->cfg_len = cfg_len;
    m->cfg_crc = crc;
    if (!changed) {
        return;
    }
    /* No clock here: the STATUS carrying the new cfg_crc goes at the next tick. */
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (is_live(&m->peer[i])) {
            status_changed(&m->peer[i], 1u);
        }
    }
}

void plink_master_event(plink_master_t *m, uint64_t now_ms, const alarm_event_t *ev)
{
    (void)now_ms;
    if (m == NULL || ev == NULL) {
        return;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_peer_t *p = &m->peer[i];
        if (is_live(p) && ev->seq > p->ev_sent_seq) {
            send_events(m, p, ev, 1u, 0u);
            p->ev_sent_seq = ev->seq;
        }
    }
}

void plink_master_sound(plink_master_t *m, uint64_t now_ms, uint8_t partition, uint8_t sound,
                        uint16_t zone)
{
    (void)now_ms;
    if (m == NULL) {
        return;
    }
    /* Fire and CO sound on every keypad, the rest on the keypads of the partition concerned. */
    const bool everyone =
        partition == 0u || sound == (uint8_t)ALARM_SOUND_FIRE || sound == (uint8_t)ALARM_SOUND_CO;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_peer_t *p = &m->peer[i];
        if (!is_live(p) || (!everyone && p->rec.home != partition)) {
            continue;
        }
        const plink_sound_t s = {partition, sound, zone};
        size_t len = 0u;
        if (plink_enc_sound(&s, m->pt, pt_cap(m, p), &len)) {
            (void)send_sealed(m, p, PLINK_T_SOUND, len);
        }
    }
}

void plink_master_time(plink_master_t *m, uint64_t now_ms, int64_t epoch_ms, const char *tz)
{
    if (m == NULL) {
        return;
    }
    char clean[PLINK_TZ_MAX + 1];
    plink_clean_text(tz, bounded_len(tz, PLINK_TZ_MAX), clean, sizeof clean);
    /* A change: another time zone, a clock set or lost, or one that stepped. */
    const int64_t expect = epoch_at(m->time.epoch_ms, m->time_at_ms, now_ms);
    const int64_t step = epoch_ms > expect ? epoch_ms - expect : expect - epoch_ms;
    const bool changed = strcmp(clean, m->time.tz) != 0 || (epoch_ms == 0) != (expect == 0) ||
                         step > (int64_t)CLOCK_STEP_MS;
    m->time.epoch_ms = epoch_ms;
    memcpy(m->time.tz, clean, sizeof m->time.tz);
    m->time_at_ms = now_ms;
    if (!changed) {
        return;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (is_live(&m->peer[i])) {
            send_time(m, &m->peer[i], now_ms);
        }
    }
}

void plink_master_name(plink_master_t *m, const char *name)
{
    if (m == NULL) {
        return;
    }
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), m->name, sizeof m->name);
    pair_identity(m);
}

void plink_master_net(plink_master_t *m, uint64_t now_ms, bool up)
{
    if (m == NULL || up == m->net_up) {
        return;
    }
    m->net_up = up;
    if (up) {
        m->net_up_since_ms = now_ms; /* supervision counts again from here */
    }
}

/* ---- the keypads ----------------------------------------------------------------------------- */

size_t plink_master_peers(const plink_master_t *m, uint64_t now_ms, plink_peer_info_t *out,
                          size_t max)
{
    size_t n = 0u;
    for (size_t i = 0; m != NULL && i < PLINK_KEYPADS_MAX; i++) {
        const plink_peer_t *p = &m->peer[i];
        if (!p->used) {
            continue;
        }
        if (out == NULL || n >= max) {
            n++; /* counted, not filled */
            continue;
        }
        plink_peer_info_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->device = p->rec.device;
        memcpy(o->name, p->rec.name, sizeof o->name);
        o->home = p->rec.home;
        o->kind = p->rec.kind;
        o->supervision_s = p->rec.supervision_s;
        o->provisional = is_provisional(p);
        const uint64_t per = p->rec.supervision_s != 0u
                                 ? period_ms(p)
                                 : (uint64_t)PLINK_SUPERVISION_DEFAULT_S * 1000u;
        o->online = is_live(p) && p->live_heard_ms != 0u && now_ms - p->live_heard_ms < per;
        o->down = p->down;
        o->heard_ago_s = UINT32_MAX; /* never, since this master started */
        if (p->live_heard_ms != 0u) {
            const uint64_t ago = (now_ms - p->live_heard_ms) / 1000u;
            o->heard_ago_s = ago < UINT32_MAX ? (uint32_t)ago : UINT32_MAX - 1u;
        }
        o->addr = p->addr;
        o->hb = p->hb;
    }
    return n;
}

bool plink_master_remove(plink_master_t *m, uint64_t now_ms, uint8_t device)
{
    plink_peer_t *p = m != NULL ? peer_of(m, device) : NULL;
    if (p == NULL) {
        return false;
    }
    /* LINK-014: its trouble cleared first; then the keys go from RAM (the pending session, the
     * pairing key, the commands it sent) and the record is written without it. Only the live
     * session stays, to seal the BYEs, and goes after the third. */
    reachable(m, device, true);
    p->down = false;
    pending_drop(p);
    plink_wipe(&p->rec, sizeof p->rec);
    plink_wipe(p->cache, sizeof p->cache);
    plink_wipe(p->seen_nk, sizeof p->seen_nk);
    plink_wipe(p->live_nk, sizeof p->live_nk);
    p->result_due = 0u;
    p->cache_panic = 0u;
    p->cmd_running = p->panic_running = false;
    p->cmd_ticket = p->panic_ticket = 0u;
    p->used = false;
    const bool bye = p->live.valid && p->have_addr;
    note(m, PLINK_NOTE_REMOVED, device);
    const bool stored = store_or_retry(m, now_ms);
    if (bye) {
        p->removing = true;
        p->bye_left = BYE_COUNT;
        p->bye_at = now_ms;
        bye_service(m, p, now_ms);
    } else {
        peer_clear(p);
        held_changed(m);
    }
    return stored;
}

/* A setter's change stored, or taken back when it couldn't be. */
static bool store_change(plink_master_t *m, plink_peer_t *p, const plink_peer_rec_t *before)
{
    if (store_rec(m)) {
        return true;
    }
    p->rec = *before;
    note(m, PLINK_NOTE_STORE_FAILED, p->rec.device);
    return false;
}

bool plink_master_rename(plink_master_t *m, uint8_t device, const char *name)
{
    plink_peer_t *p = m != NULL ? peer_of(m, device) : NULL;
    if (p == NULL || name == NULL) {
        return false;
    }
    plink_peer_rec_t before = p->rec;
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), p->rec.name, sizeof p->rec.name);
    const bool ok = store_change(m, p, &before);
    plink_wipe(&before, sizeof before);
    return ok;
}

bool plink_master_set_home(plink_master_t *m, uint64_t now_ms, uint8_t device, uint8_t home)
{
    plink_peer_t *p = m != NULL ? peer_of(m, device) : NULL;
    if (p == NULL || home < 1u || home > ALARM_MAX_PARTITIONS || home > PLINK_PARTS_MAX) {
        return false;
    }
    if (home == p->rec.home) {
        return true;
    }
    plink_peer_rec_t before = p->rec;
    p->rec.home = home;
    const bool ok = store_change(m, p, &before);
    plink_wipe(&before, sizeof before);
    if (ok) {
        status_changed(p, now_ms); /* the keypad learns its new home from its STATUS */
        status_service(m, p, now_ms);
    }
    return ok;
}

bool plink_master_set_supervision(plink_master_t *m, uint64_t now_ms, uint8_t device,
                                  uint16_t seconds)
{
    plink_peer_t *p = m != NULL ? peer_of(m, device) : NULL;
    /* LINK-007: 1, 5 or 15 minutes, or not supervised (0). */
    if (p == NULL || (seconds != 0u && seconds != 60u && seconds != 300u && seconds != 900u)) {
        return false;
    }
    plink_peer_rec_t before = p->rec;
    p->rec.supervision_s = seconds;
    const bool ok = store_change(m, p, &before);
    plink_wipe(&before, sizeof before);
    if (ok) {
        supervise(m, p, now_ms);
    }
    return ok;
}

uint32_t plink_master_held(const plink_master_t *m)
{
    uint32_t held = 0u;
    for (size_t i = 0; m != NULL && i < PLINK_KEYPADS_MAX; i++) {
        if (m->peer[i].used || m->peer[i].removing) {
            held |= 1u << i;
        }
    }
    return held;
}

bool plink_master_in_use(const plink_master_t *m)
{
    if (m == NULL) {
        return false;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (m->peer[i].used) {
            return true;
        }
    }
    return plink_master_pairing(m);
}

uint16_t plink_master_net_id(const plink_master_t *m)
{
    return m != NULL ? m->rec.net_id : 0u;
}
