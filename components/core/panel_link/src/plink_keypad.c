/*
 * plink_keypad.c - a keypad's side of the panel link (see include/panel_link/plink_keypad.h;
 * docs/features/11 "Pairing", "Sessions", "Timers", "Roles on a P4 panel"; LINK-002, -006, -008,
 * -014, -015, -016, -017, -018, -023, -025, -026).
 *
 * One task owns the keypad: frames come in through plink_keypad_input, time through
 * plink_keypad_tick, and whatever has to go out goes through the port at once. The pieces:
 *   - pairing (plink_pair_k_t), only while a person pairs; the record it makes is stored and a
 *     handshake starts at once;
 *   - the handshake (plink_hs_*): at start-up, after 15 s without an authenticated STATUS (a
 *     frame of another kind getting through doesn't keep a session whose STATUS doesn't; a new
 *     session brings one at once), on a REKEY for the live session, at seq 2^30 and every 24 h.
 *     An unanswered HELLO is followed by a fresh one after 1, 2, 4 ... 30 s, jittered (at most
 *     2 s while a panic waits). A WELCOME makes the new session tentative: the old one is still
 *     opened until a frame of the new one arrives, and HEARTBEATs go every 500 ms until then (the
 *     master promotes the new session on the first of them);
 *   - what the master says, in frame order per item (LINK-026): a STATUS's system section, each
 *     partition record, each zone record and the time are each taken only from a frame newer
 *     than the one they last came in; a new session starts the count again;
 *   - what the keypad pulls: the configuration and the device list page by page (one plink_pull_t
 *     each: 2 requests out, each retried after 300 ms doubling to 2 s), the zones when their CRC
 *     differs, the events past the newest kept;
 *   - commands, with a request id counted from a random base, resent every 500 ms for 3 s, a
 *     panic every 1 s for 60 s whatever the link's state (LINK-018, LINK-023);
 *   - Smart Home and settings (LINK-043..052), each only in a session holding its capability bit
 *     and on a transport whose frames are 250 B or more: the mirror of the master's devices, its
 *     records in frame order per device and caught up while their CRC differs, device actions in
 *     2 slots of their own, the settings report, and each PREFS_SET applied once.
 *
 * No code or key is kept longer than it is needed: a command's or a device action's code is
 * wiped once it is answered or given up (an action's after its last copy), and every buffer that
 * held one in clear is wiped after sealing.
 */
#include "panel_link/plink_keypad.h"

#include <string.h>

#include "alarm/alarm_codec.h"         /* alarm_crc32 */
#include "alarm_proto/proto_prefs.h"   /* prefs_crc */
#include "home_control/home_control.h" /* what an action needs: hc_action_valid, hc_needs_code */
#include "panel_link/plink_wire.h"

#define NEVER UINT64_MAX

/* The handshake: the first wait for a WELCOME, doubled after every unanswered HELLO up to
 * PLINK_BACKOFF_MAX_MS, each wait drawn from its last quarter; while a panic waits, 2 s. */
#define HS_FIRST_MS 1000u
#define HS_PANIC_MS 2000u

#define HB_JITTER_MS      1000u   /* LINK-006: a heartbeat every 10 s +- 1 s */
#define CFG_DELAY_MS      500u    /* a pull starts 0..500 ms after a new cfg_crc is seen */
#define ZONES_WAIT_MS     250u    /* a ZONES the master pushed may trail its STATUS */
#define ZONES_GAP_MS      1000u   /* ZONES_REQ at most this often, ... */
#define ZONES_GAP_MAX_MS  30000u  /* ... the gap doubling while the CRC still differs */
#define NOT_PAIRED_RUN_MS 120000u /* NOT_PAIRED hints further apart start a new run */
#define STORE_RETRY_MS    5000u
#define OWN_PAYLOAD_MAX   225u /* the keypad's own payloads (HEARTBEAT, CMD, the requests, FW_INFO,
                                * FW_STATE, DEV_ACT, PREFS): PLINK_PAYLOAD_SMALL */
#define EV_FILL_TRIES     4u  /* EV_REQs for one hole in the events before it is let be */

enum { STORE_NONE = 0, STORE_WRITE = 1, STORE_ERASE = 2 };
enum { PREFS_IN_NONE = 0, PREFS_IN_WAITING = 1, PREFS_IN_TAKEN = 2 };

/* Every PREFS the keypad sends fits its own payloads: the answer section (5 B), the info section
 * (13 B), the refusals section (2 B, 2 per refusal) and the fields section (1 B and the body). */
_Static_assert(5u + 13u + 2u + 2u * PLINK_PREFS_REFUSED_MAX + 1u + PLINK_PREFS_OWN_MAX <=
                   OWN_PAYLOAD_MAX,
               "a keypad's PREFS fits its payloads");
_Static_assert(PLINK_DEV_ACT_LEN <= OWN_PAYLOAD_MAX, "DEV_ACT fits a keypad's payloads");
_Static_assert(PLINK_DEV_BLOB_MAX <= UINT16_MAX, "a row's offset fits plink_dev_mirror_t.off");

/* ---- small helpers ------------------------------------------------------------------------ */

static uint64_t earliest(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0u;
    while (s != NULL && n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* 32 random bits for jitter and the request base. A failed draw gives 0: nothing secret rides
 * on these (the firmware's start-up self-check keeps the link off when the generator fails). */
static uint32_t rnd32(const plink_keypad_t *k)
{
    uint8_t b[4] = {0u, 0u, 0u, 0u};
    if (!plink_random(k->c, b, sizeof b)) {
        return 0u;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* The largest frame towards the master. */
static size_t frame_cap(const plink_keypad_t *k)
{
    size_t m = PLINK_FRAME_MAX;
    if (k->port.mtu != NULL) {
        m = k->port.mtu(k->port.ctx, k->master.tp);
    }
    return m < PLINK_FRAME_MAX ? m : PLINK_FRAME_MAX;
}

/* The blob bytes one CFG_PAGE (or DEV_LIST_PAGE, the same layout) can carry on this transport
 * (0: none). */
static uint16_t page_size(const plink_keypad_t *k)
{
    const size_t m = frame_cap(k);
    const size_t over = PLINK_SEALED_OVERHEAD + PLINK_CFG_PAGE_HDR;
    return m > over ? (uint16_t)(m - over) : 0u;
}

/* Whether a capability's frames go to the master now (LINK-043): a live session holding the bit,
 * on a transport whose frames are 250 B or more - asked at every send, so a failover is safe -
 * and Smart Home's only with a mirror. */
static bool cap_ok(const plink_keypad_t *k, uint16_t bit)
{
    return k->live.valid && (k->live.caps & bit) != 0u && frame_cap(k) >= PLINK_FRAME_ESPNOW &&
           (bit != PLINK_CAP_HOME || k->home != NULL);
}

static void set_state(plink_keypad_t *k, uint64_t now, uint8_t st)
{
    if (k->state == st) {
        return;
    }
    k->state = st;
    k->state_since_ms = now;
    if (st == PLINK_LINK_LOST) {
        k->lost_since_ms = now;
    }
}

/* The snapshot changed: the UI's counter rises, and the snapshot carries it (UI: snap.seq ==
 * snapshot_seq()). */
static void bump_seq(plink_keypad_t *k)
{
    k->seq++;
    k->view.snap.seq = k->seq;
    k->view.snap.config_seq = k->config_seq;
}

/* Writes the record (STORE_WRITE) or erases it (STORE_ERASE); a failure is tried again every
 * 5 s from the tick. */
static bool store_now(plink_keypad_t *k, uint64_t now, uint8_t what)
{
    bool ok = true;
    if (k->port.store != NULL) {
        ok = k->port.store(k->port.ctx, what == STORE_WRITE ? &k->rec : NULL);
    }
    k->store_due = ok ? (uint8_t)STORE_NONE : what;
    k->store_at_ms = now + STORE_RETRY_MS;
    return ok;
}

/* One datagram; UDP doesn't tell whether it arrived, and the protocol recovers either way. */
static void send_to(plink_keypad_t *k, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    if (k->port.send != NULL && len > 0u) {
        (void)k->port.send(k->port.ctx, to, frame, len);
    }
}

/* What pairing left in k->out. */
static void send_out(plink_keypad_t *k)
{
    if (k->out.len > 0u) {
        send_to(k, &k->out.to, k->out.buf, k->out.len);
    }
    k->out.len = 0u;
}

/* ---- the handshake ------------------------------------------------------------------------ */

/* A handshake as soon as at (one under way keeps its own schedule). */
static void hs_soon(plink_keypad_t *k, uint64_t at)
{
    if (!k->hs.waiting && at < k->hs_due_ms) {
        k->hs_due_ms = at;
    }
}

/* A sealed frame of the live session to the master. Its seq is spent even when sealing fails, so
 * no nonce is tried twice (plink_session_seal); at 2^30 a new handshake starts (LINK-015). */
static bool seal_send(plink_keypad_t *k, uint64_t now, uint8_t type, const uint8_t *payload,
                      size_t len)
{
    if (!k->paired || !k->live.valid || !k->have_master) {
        return false;
    }
    size_t n = 0u;
    const bool ok = plink_session_seal(k->c, &k->live, type, k->rec.net_id, k->rec.device, false,
                                       payload, len, k->out.buf, frame_cap(k), &n);
    if (k->live.tx_seq >= PLINK_SEQ_REKEY) {
        hs_soon(k, now);
    }
    if (ok) {
        send_to(k, &k->master, k->out.buf, n);
    }
    k->out.len = 0u;
    return ok;
}

static bool panic_waits(const plink_keypad_t *k)
{
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        const plink_kcmd_t *c = &k->cmds[i];
        if (c->used && c->panic && c->state == PLINK_CMD_PENDING) {
            return true;
        }
    }
    return false;
}

/* A fresh HELLO (new e_k and n_k: never reused) to where discovery found the master. The next
 * one goes if no WELCOME has verified by then. */
static void hs_send(plink_keypad_t *k, uint64_t now)
{
    if (k->hs.waiting && k->port.resolve != NULL) {
        /* The last HELLO went unanswered: the platform looks for the master again. */
        k->port.resolve(k->port.ctx);
    }
    const plink_addr_t *to = k->have_disc ? &k->disc : (k->have_master ? &k->master : NULL);
    if (to == NULL) {
        k->hs_due_ms = NEVER; /* plink_keypad_master_addr brings it back */
        return;
    }
    const uint64_t base = k->hs_backoff_ms;
    uint64_t wait = base - base / 4u + rnd32(k) % (base / 4u + 1u);
    if (panic_waits(k) && wait > HS_PANIC_MS) {
        wait = HS_PANIC_MS;
    }
    k->hs_backoff_ms = base * 2u < PLINK_BACKOFF_MAX_MS ? base * 2u : PLINK_BACKOFF_MAX_MS;
    k->hs_due_ms = now + wait;
    if (plink_hs_hello(k->c, k->rec.k_pair, k->rec.net_id, k->rec.device, &k->cap, k->caps,
                       &k->hs)) {
        k->hs_to = *to;
        k->hs_bcast = to == &k->disc ? k->disc_bcast : plink_addr_is_broadcast(to);
        k->hs_checks = 0u;
        send_to(k, to, k->hs.hello, k->hs.hello_len);
    }
}

static void hs_tick(plink_keypad_t *k, uint64_t now)
{
    if ((k->have_disc || k->have_master) && !k->hs.waiting && k->hs_due_ms == NEVER) {
        if (now >= k->status_heard_ms + PLINK_REHANDSHAKE_MS) {
            hs_soon(k, now); /* 15 s without an authenticated STATUS */
        }
        if (k->live.valid && now >= k->session_started_ms + PLINK_SESSION_MAX_MS) {
            hs_soon(k, now); /* every 24 h */
        }
        if (k->live.valid && k->live.tx_seq >= PLINK_SEQ_REKEY) {
            hs_soon(k, now);
        }
    }
    if (now >= k->hs_due_ms) {
        hs_send(k, now);
    }
}

/* ---- heartbeats --------------------------------------------------------------------------- */

static void hb_schedule(plink_keypad_t *k, uint64_t now)
{
    k->hb_due_ms = now + (k->tentative ? (uint64_t)PLINK_TENTATIVE_HB_MS
                                       : (uint64_t)(PLINK_HEARTBEAT_MS - HB_JITTER_MS) +
                                             rnd32(k) % (2u * HB_JITTER_MS + 1u));
}

static void hb_send(plink_keypad_t *k, uint64_t now)
{
    plink_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    hb.flags = k->power_flags;
    hb.rssi = k->rssi;
    const uint64_t up = now > k->started_ms ? (now - k->started_ms) / 1000u : 0u;
    hb.uptime_s = up > UINT32_MAX ? UINT32_MAX : (uint32_t)up;
    hb.status_age_s = PLINK_STATUS_AGE_NONE;
    if (k->view.have_status) {
        const uint64_t age = now > k->status_ms ? (now - k->status_ms) / 1000u : 0u;
        hb.status_age_s =
            age >= PLINK_STATUS_AGE_NONE ? (uint16_t)(PLINK_STATUS_AGE_NONE - 1u) : (uint16_t)age;
    }
    memcpy(hb.fw, k->fw, sizeof hb.fw);
    /* LINK-050: the settings' CRC, only in a session with PREFS (a v1 master's heartbeat stays
     * as it was), once there is a report to name. */
    if (k->prefs.have && cap_ok(k, PLINK_CAP_PREFS)) {
        hb.has_prefs = true;
        hb.prefs_crc = k->prefs.crc;
    }
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_heartbeat(&hb, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_HEARTBEAT, p, n);
    }
    hb_schedule(k, now);
}

/* ---- commands (LINK-018, LINK-023) -------------------------------------------------------- */

static bool is_panic(const plink_cmd_t *cmd)
{
    return cmd->op == PLINK_OP_COMMAND && cmd->cmd.type == ALARM_CMD_PANIC;
}

static void cmd_free(plink_keypad_t *k, size_t i)
{
    plink_wipe(&k->cmds[i], sizeof k->cmds[i]);
    k->cmd_released = (uint8_t)(k->cmd_released & ~(1u << (unsigned)i));
}

/* Answered (res) or given up: the codes go now; the slot goes too once its caller let it go. */
static void cmd_end(plink_keypad_t *k, size_t i, uint8_t state, const plink_result_t *res)
{
    plink_kcmd_t *c = &k->cmds[i];
    c->state = state;
    if (res != NULL) {
        c->res = *res;
    }
    plink_wipe(c->cmd.cmd.code, sizeof c->cmd.cmd.code);
    plink_wipe(c->cmd.new_code, sizeof c->cmd.new_code);
    if ((k->cmd_released & (1u << (unsigned)i)) != 0u) {
        cmd_free(k, i);
    }
}

static void cmd_send(plink_keypad_t *k, uint64_t now, const plink_kcmd_t *c)
{
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_cmd(&c->cmd, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_CMD, p, n);
    }
    plink_wipe(p, sizeof p); /* the codes in clear */
}

/* Every command due goes again with the same req: every 500 ms for 3 s, a panic every 1 s for
 * 60 s from keying, whatever the link's state (a panic without a session waits for one). */
static void cmds_tick(plink_keypad_t *k, uint64_t now)
{
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        plink_kcmd_t *c = &k->cmds[i];
        if (!c->used || c->state != PLINK_CMD_PENDING || now < c->next_ms) {
            continue;
        }
        const uint64_t keep = c->panic ? PLINK_PANIC_KEEP_MS : PLINK_CMD_GIVE_UP_MS;
        const uint64_t every = c->panic ? PLINK_PANIC_RETRY_MS : PLINK_CMD_RETRY_MS;
        if (now >= c->started_ms + keep) {
            cmd_end(k, i, PLINK_CMD_NO_ANSWER, NULL);
            continue;
        }
        cmd_send(k, now, c);
        c->next_ms += every;
        if (c->next_ms <= now) {
            c->next_ms = now + every; /* a late tick: no burst to catch up */
        }
        c->next_ms = earliest(c->next_ms, c->started_ms + keep);
    }
}

/* A slot for a new command: a panic takes slot 0 first, any other command one of the rest, so a
 * panic always finds one. With none free, the oldest finished one whose caller never let it go
 * is taken. -1: none. */
static int cmd_slot(plink_keypad_t *k, bool panic)
{
    const size_t first = panic ? 0u : 1u;
    for (size_t i = first; i < PLINK_CMDS_MAX; i++) {
        if (!k->cmds[i].used) {
            return (int)i;
        }
    }
    int best = -1;
    for (size_t i = first; i < PLINK_CMDS_MAX; i++) {
        const plink_kcmd_t *c = &k->cmds[i];
        if (c->state != PLINK_CMD_PENDING &&
            (best < 0 || c->started_ms < k->cmds[(size_t)best].started_ms)) {
            best = (int)i;
        }
    }
    if (best >= 0) {
        cmd_free(k, (size_t)best);
    }
    return best;
}

/* ---- pulls: the configuration and the device list ----------------------------------------- */

static void pull_stop(plink_pull_t *pl)
{
    pl->pulling = false;
    pl->ahead_off = pl->ahead_len = 0u;
    memset(pl->req, 0, sizeof pl->req);
}

/* Pull blob crc (total bytes; 0: told by its first page) unless it is the one being pulled or one
 * that failed before; whatever else was being pulled is dropped. A blob this keypad can't hold is
 * remembered as failed. Not all keypads at once: the first requests go 0-500 ms on. */
static void pull_start(plink_keypad_t *k, plink_pull_t *pl, uint64_t now, uint32_t crc,
                       uint32_t total)
{
    if (pl->pulling && crc == pl->crc) {
        return;
    }
    pull_stop(pl);
    if (pl->bad_crc != 0u && crc == pl->bad_crc) {
        return;
    }
    if (pl->buf == NULL || total > pl->cap) {
        pl->bad_crc = crc; /* more than this keypad can hold */
        return;
    }
    pl->pulling = true;
    pl->crc = crc;
    pl->total = total;
    pl->have = 0u;
    pl->next_ms = now + rnd32(k) % (CFG_DELAY_MS + 1u);
}

/* One request: CFG_REQ, or DEV_LIST_REQ (CFG_REQ's layout and codec). */
static void pull_request(plink_keypad_t *k, const plink_pull_t *pl, uint64_t now, uint32_t off,
                         uint16_t max)
{
    const plink_cfg_req_t q = {pl->crc, off, max};
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_cfg_req(&q, p, sizeof p, &n)) {
        (void)seal_send(k, now, pl->type, p, n);
    }
}

/* Keeps two requests out: the first gap, and what follows it (or the page in past it); while the
 * blob's length isn't known yet, its first page alone. A request no longer wanted is dropped; one
 * due goes again, its wait doubled up to 2 s. */
static void pull_pump(plink_keypad_t *k, plink_pull_t *pl, uint64_t now)
{
    const uint16_t page = page_size(k);
    if (!pl->pulling || !k->live.valid || page == 0u || now < pl->next_ms) {
        return;
    }
    uint32_t off[PLINK_PULL_OUTSTANDING];
    uint16_t max[PLINK_PULL_OUTSTANDING];
    size_t n = 0u;
    if (pl->total == 0u) {
        off[n] = 0u;
        max[n] = page;
        n++;
    } else {
        const uint32_t gap_end = pl->ahead_len != 0u ? pl->ahead_off : pl->total;
        uint32_t next = pl->have;
        if (pl->have < gap_end) {
            const uint32_t room = gap_end - pl->have;
            off[n] = pl->have;
            max[n] = room < page ? (uint16_t)room : page;
            next = pl->have + max[n];
            n++;
        }
        if (pl->ahead_len != 0u) {
            next = pl->ahead_off + pl->ahead_len;
        }
        if (n < PLINK_PULL_OUTSTANDING && next < pl->total) {
            const uint32_t room = pl->total - next;
            off[n] = next;
            max[n] = room < page ? (uint16_t)room : page;
            n++;
        }
    }
    for (size_t r = 0; r < PLINK_PULL_OUTSTANDING; r++) {
        plink_kpull_t *q = &pl->req[r];
        bool wanted = false;
        for (size_t j = 0; j < n; j++) {
            wanted = wanted || (q->used && q->off == off[j]);
        }
        if (q->used && !wanted) {
            memset(q, 0, sizeof *q);
        }
    }
    for (size_t j = 0; j < n; j++) {
        plink_kpull_t *q = NULL;
        for (size_t r = 0; r < PLINK_PULL_OUTSTANDING && q == NULL; r++) {
            if (pl->req[r].used && pl->req[r].off == off[j]) {
                q = &pl->req[r];
            }
        }
        for (size_t r = 0; r < PLINK_PULL_OUTSTANDING && q == NULL; r++) {
            if (!pl->req[r].used) {
                q = &pl->req[r];
                q->used = true;
                q->off = off[j];
                q->retry_ms = PLINK_PULL_RETRY_MS;
                q->due_ms = now;
            }
        }
        if (q == NULL || now < q->due_ms) {
            continue;
        }
        q->max = max[j];
        pull_request(k, pl, now, q->off, q->max);
        q->due_ms = now + q->retry_ms;
        q->retry_ms =
            q->retry_ms * 2u < PLINK_PULL_RETRY_MAX_MS ? q->retry_ms * 2u : PLINK_PULL_RETRY_MAX_MS;
    }
}

/* A page of the blob being pulled: true once the blob is whole (the caller checks it, installs it
 * and stops the pull); otherwise the caller asks for the next pages at once. Another blob's page
 * is dropped (the next STATUS or DEV_STATE says which one is current). */
static bool pull_take(plink_pull_t *pl, const plink_cfg_page_t *pg)
{
    if (!pl->pulling || pg->cfg_crc != pl->crc || pg->len == 0u) {
        return false;
    }
    if (pl->total == 0u) {
        /* The device list: its length comes with its first page. */
        if (pg->total > pl->cap) {
            pull_stop(pl);
            pl->bad_crc = pl->crc; /* more than this keypad can hold */
            return false;
        }
        pl->total = pg->total;
    }
    if (pg->total != pl->total) {
        return false;
    }
    const uint32_t off = pg->offset;
    const uint32_t end = pg->offset + pg->len; /* <= total <= cap (the decoder checked) */
    const uint32_t ahead_end = pl->ahead_off + pl->ahead_len;
    if (off <= pl->have) {
        if (end > pl->have) {
            memcpy(&pl->buf[pl->have], &pg->data[pl->have - off], end - pl->have);
            pl->have = end;
        }
    } else if (pl->ahead_len == 0u) {
        memcpy(&pl->buf[off], pg->data, pg->len);
        pl->ahead_off = off;
        pl->ahead_len = pg->len;
    } else if (off <= ahead_end && end > ahead_end) {
        memcpy(&pl->buf[ahead_end], &pg->data[ahead_end - off], end - ahead_end);
        pl->ahead_len = end - pl->ahead_off;
    } else if (off < pl->ahead_off && end >= pl->ahead_off) {
        memcpy(&pl->buf[off], pg->data, pl->ahead_off - off);
        pl->ahead_len += pl->ahead_off - off;
        pl->ahead_off = off;
    } /* anything else is asked for again */
    if (pl->ahead_len != 0u && pl->ahead_off <= pl->have) {
        const uint32_t a_end = pl->ahead_off + pl->ahead_len;
        if (a_end > pl->have) {
            pl->have = a_end;
        }
        pl->ahead_off = pl->ahead_len = 0u;
    }
    return pl->have >= pl->total;
}

/* When a pull's next request is due (NEVER: none). */
static uint64_t pull_next(const plink_keypad_t *k, const plink_pull_t *pl)
{
    if (!pl->pulling || page_size(k) == 0u) {
        return NEVER;
    }
    uint64_t c = NEVER;
    for (size_t r = 0; r < PLINK_PULL_OUTSTANDING; r++) {
        if (pl->req[r].used) {
            c = earliest(c, pl->req[r].due_ms);
        }
    }
    return c != NEVER ? c : pl->next_ms;
}

/* ---- the configuration (docs/features/11 "Configuration") --------------------------------- */

/* After a STATUS: pull the configuration it names unless it is the one installed, the one being
 * pulled, or one that failed before. The master back on the installed one (a release rolled
 * back, a configuration shrunk again): the one that failed is forgotten. */
static void cfg_check(plink_keypad_t *k, uint64_t now)
{
    plink_pull_t *pl = &k->cfg_pull;
    const uint32_t crc = k->view.cfg_crc;
    const uint32_t len = k->view.cfg_len;
    if (len != 0u && k->have_cfg && crc == k->cfg_installed_crc) {
        pl->bad_crc = 0u;
    }
    if (len == 0u || (k->have_cfg && crc == k->cfg_installed_crc)) {
        pull_stop(pl);
        return;
    }
    pull_start(k, pl, now, crc, len);
}

static void cfg_pump(plink_keypad_t *k, uint64_t now)
{
    pull_pump(k, &k->cfg_pull, now);
}

/* The whole blob is in: its CRC checked, then decoded leniently into the caller's configuration
 * (LINK-017, LINK-019). One that fails is remembered and not pulled again; the keypad says it
 * needs an update (plink_keypad_info cfg_bad_crc) until another one installs. */
static void cfg_finish(plink_keypad_t *k)
{
    plink_pull_t *pl = &k->cfg_pull;
    const uint32_t crc = pl->crc;
    const uint32_t total = pl->total;
    pull_stop(pl);
    if (alarm_crc32(0u, pl->buf, total) != crc) {
        pl->bad_crc = crc;
        return;
    }
    if (plink_cfg_decode(pl->buf, total, k->cfg) != ALARM_OK) {
        /* The decode may have started on the copy: it holds nothing usable now. What the
         * platform took at the last config_seq stays on the screen. */
        pl->bad_crc = crc;
        k->have_cfg = false;
        k->cfg_installed_crc = 0u;
        return;
    }
    k->have_cfg = true;
    k->cfg_installed_crc = crc;
    pl->bad_crc = 0u; /* the master's configuration is read: no update is needed for it */
    k->config_seq++;
    k->view.snap.config_seq = k->config_seq;
}

static void on_cfg_page(plink_keypad_t *k, uint64_t now, const uint8_t *p, size_t n)
{
    plink_cfg_page_t pg;
    if (!plink_dec_cfg_page(p, n, &pg)) {
        return;
    }
    if (pull_take(&k->cfg_pull, &pg)) {
        cfg_finish(k);
        return;
    }
    cfg_pump(k, now); /* the next pages at once */
}

/* ---- zones and events --------------------------------------------------------------------- */

/* After a STATUS or ZONES: the zones are asked for when their CRC differs from the master's,
 * first after 250 ms (a pushed ZONES may trail its STATUS), then at most every 1 s, doubling. */
static void zones_check(plink_keypad_t *k, uint64_t now)
{
    if (plink_view_zones_crc(&k->view) == k->view.zones_crc) {
        k->zones_want = false;
        k->zones_gap_ms = ZONES_GAP_MS;
        return;
    }
    if (!k->zones_want) {
        k->zones_want = true;
        if (k->zones_next_ms < now + ZONES_WAIT_MS) {
            k->zones_next_ms = now + ZONES_WAIT_MS;
        }
    }
}

static void zones_pump(plink_keypad_t *k, uint64_t now)
{
    if (!k->zones_want || !k->live.valid || now < k->zones_next_ms) {
        return;
    }
    const uint16_t used = k->view.snap.zones_used;
    if (used == 0u || plink_view_zones_crc(&k->view) == k->view.zones_crc) {
        k->zones_want = false;
        k->zones_gap_ms = ZONES_GAP_MS;
        return;
    }
    const plink_zones_req_t q = {1u, used};
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_zones_req(&q, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_ZONES_REQ, p, n);
    }
    k->zones_next_ms = now + k->zones_gap_ms;
    k->zones_gap_ms =
        k->zones_gap_ms * 2u < ZONES_GAP_MAX_MS ? k->zones_gap_ms * 2u : ZONES_GAP_MAX_MS;
}

static uint32_t ev_newest(const plink_keypad_t *k)
{
    return k->ev_count > 0u ? k->events[k->ev_count - 1u].seq : 0u;
}

static void ev_drop(plink_keypad_t *k)
{
    memset(k->events, 0, sizeof k->events);
    k->ev_count = 0u;
    k->ev_frame_seq = 0u;
    k->ev_fill = 0u;
    k->ev_back = false;
    k->ev_back_tries = 0u;
}

/* A ring short of what it keeps whose oldest isn't the master's first: older events may be
 * there to fetch (a first answer lost, or only a pushed event kept before any pull). */
static bool ev_short(const plink_keypad_t *k)
{
    return k->ev_count > 0u && k->ev_count < PLINK_EVENTS_KEPT && k->events[0].seq > 1u;
}

/* A pull is due now (its waits start again at 300 ms) unless one is under way. */
static void ev_pull(plink_keypad_t *k, uint64_t now)
{
    if (!k->ev_want) {
        k->ev_want = true;
        k->ev_retry_ms = PLINK_PULL_RETRY_MS;
        k->ev_next_ms = now;
    }
}

/* After a STATUS (frame fs): its last_event_seq below the newest kept means the master's log
 * started again - but only a STATUS newer than the frame that brought that event says so; one
 * above it is pulled: EV_REQ after the newest kept (an empty ring: the last 50). */
static void ev_check(plink_keypad_t *k, uint64_t now, uint32_t fs)
{
    const uint32_t last = k->view.snap.last_event_seq;
    if (k->ev_count > 0u && last < ev_newest(k) && fs > k->ev_frame_seq) {
        ev_drop(k);
    }
    if (last > ev_newest(k)) {
        k->ev_back = false;
        ev_pull(k, now);
    } else if (k->ev_fill == 0u && !k->ev_back) {
        k->ev_want = false;
        /* Short: the newest 50 once more (a few times at most for one ring; a backfill under
         * way ends with its answer). */
        if (ev_short(k) && k->ev_back_tries < EV_FILL_TRIES) {
            k->ev_back_tries++;
            k->ev_back = true;
            ev_pull(k, now);
        }
    }
}

static void ev_pump(plink_keypad_t *k, uint64_t now)
{
    if (!k->ev_want || !k->live.valid || now < k->ev_next_ms) {
        return;
    }
    if (k->ev_fill != 0u && ++k->ev_fill_tries > EV_FILL_TRIES) {
        /* The master's answers never brought it: let the hole be. */
        k->ev_fill = 0u;
        if (ev_newest(k) >= k->view.snap.last_event_seq) {
            k->ev_want = false;
            return;
        }
    }
    const uint32_t newest = ev_newest(k);
    uint32_t after = newest;
    if (k->ev_fill != 0u) {
        after = k->ev_fill;
    } else if (k->ev_back) {
        after = newest > PLINK_EVENTS_KEPT ? newest - PLINK_EVENTS_KEPT : 0u;
    }
    const plink_ev_req_t q = {after, (uint8_t)PLINK_EVENTS_KEPT};
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_ev_req(&q, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_EV_REQ, p, n);
    }
    k->ev_next_ms = now + k->ev_retry_ms;
    k->ev_retry_ms = k->ev_retry_ms * 2u < PLINK_PULL_RETRY_MAX_MS ? k->ev_retry_ms * 2u
                                                                   : PLINK_PULL_RETRY_MAX_MS;
}

/* Puts e in the ring in seq order (once); a full ring drops its oldest, and an event older than
 * everything a full ring holds is not kept. true when it went in. */
static bool ev_insert(plink_keypad_t *k, const alarm_event_t *e)
{
    size_t i = k->ev_count; /* how many kept events are older than e */
    while (i > 0u && k->events[i - 1u].seq > e->seq) {
        i--;
    }
    if (i > 0u && k->events[i - 1u].seq == e->seq) {
        return false;
    }
    if (k->ev_count < PLINK_EVENTS_KEPT) {
        memmove(&k->events[i + 1u], &k->events[i], (k->ev_count - i) * sizeof k->events[0]);
        k->events[i] = *e;
        k->ev_count++;
        return true;
    }
    if (i == 0u) {
        return false;
    }
    memmove(&k->events[0], &k->events[1], (i - 1u) * sizeof k->events[0]);
    k->events[i - 1u] = *e;
    return true;
}

/* Events in, oldest first. The engine numbers its events one by one, so events past the newest
 * kept that don't start right after it, in a frame that doesn't say older ones were left out,
 * mean one pushed before them was lost (or is still on its way): they are kept, and the events
 * after the newest kept before them are pulled once more, until one comes that fills the hole, an
 * answer says it can't, or EV_FILL_TRIES requests went unanswered. */
static void on_events(plink_keypad_t *k, uint64_t now, uint32_t fs, const uint8_t *p, size_t n)
{
    size_t got = 0u;
    uint8_t flags = 0u;
    if (!plink_dec_events(p, n, k->scr.ev, PLINK_EVENTS_KEPT, &got, &flags)) {
        return;
    }
    const uint32_t before = ev_newest(k);
    bool hole = false;
    for (size_t i = 0; i < got; i++) {
        if (k->scr.ev[i].seq > before) {
            hole =
                k->ev_count > 0u && k->scr.ev[i].seq > before + 1u && (flags & PLINK_EVF_GAP) == 0u;
            break;
        }
    }
    for (size_t i = 0; i < got; i++) {
        if (k->ev_fill != 0u &&
            (k->scr.ev[i].seq == k->ev_fill + 1u || (flags & PLINK_EVF_GAP) != 0u)) {
            k->ev_fill = 0u; /* the hole is filled, or the master says it can't be */
        }
        (void)ev_insert(k, &k->scr.ev[i]);
    }
    if (ev_newest(k) != before) {
        k->ev_frame_seq = fs;
    }
    if (hole && k->ev_fill == 0u) {
        k->ev_fill = before;
        k->ev_fill_tries = 0u;
        ev_pull(k, now);
    }
    if (k->ev_fill == 0u && ev_newest(k) >= k->view.snap.last_event_seq) {
        k->ev_want = false;
        k->ev_back = false;
    }
}

/* ---- Smart Home: the device list and its records (LINK-044..046) -------------------------- */

static bool dev_fresh_at(const plink_keypad_t *k, uint64_t now)
{
    return k->dev_heard_ms != 0u && now < k->dev_heard_ms + PLINK_DEV_STALE_MS;
}

/* A DEV_STATE (a summary too) or a DEV_ACK's record naming list list_crc: fresh for 30 s
 * (LINK-046) - only under the installed list, or any while none is installed (no row to keep
 * fresh then). Data under a list the keypad can't install (a newer compat, one too big, a CRC
 * that lies: remembered, never pulled again) refreshes none of the old list's rows, so they read
 * offline 30 s after their own last data instead of frozen states shown as live. */
static void dev_heard(plink_keypad_t *k, uint64_t now, uint32_t list_crc)
{
    if (k->home->list_crc != 0u && list_crc != k->home->list_crc) {
        return;
    }
    k->dev_heard_ms = now != 0u ? now : 1u;
    if (!k->dev_fresh) {
        k->dev_fresh = true;
        k->dev_seq++;
    }
}

/* Row i of the installed list (false: past its last row, or a row this firmware can't use). */
static bool dev_cat(const plink_dev_mirror_t *h, unsigned i, plink_dev_cat_t *cat)
{
    if (i >= h->recs.count || h->off[i] < PLINK_DEV_BLOB_HDR || h->off[i] >= h->blob_len) {
        return false;
    }
    return plink_dec_dev_cat(&h->blob[h->off[i]], h->blob_len - h->off[i], cat, NULL);
}

/* The installed row whose ref is ref, byte for byte (by its key first). */
static bool dev_find(const plink_dev_mirror_t *h, const char *ref, uint8_t *index,
                     plink_dev_cat_t *cat)
{
    const uint32_t key = plink_home_ref_key(ref);
    for (unsigned i = 0; i < h->recs.count; i++) {
        if (h->keys[i] == key && dev_cat(h, i, cat) && strcmp(cat->ref, ref) == 0) {
            *index = (uint8_t)i;
            return true;
        }
    }
    return false;
}

/* Device index's record section sec (its length byte first) from frame fs: taken only from a
 * newer frame of the session than the last one for that device (LINK-026), kept as it came so
 * the states_crc is the master's. A section longer than this firmware keeps (a newer master's)
 * is kept as the fields it reads: its CRC then differs, and the keypad keeps asking at most every
 * 30 s. A later record ends what a finished action showed on that device (not one of the copies
 * of its own answer, req ack). */
static void dev_apply(plink_keypad_t *k, uint32_t fs, uint8_t index, const uint8_t *sec, size_t len,
                      uint32_t ack)
{
    plink_dev_mirror_t *h = k->home;
    if (index >= h->recs.count || fs <= h->frame_seq[index]) {
        return;
    }
    if (!plink_home_rec_put(&h->recs, index, sec, len)) {
        plink_dev_rec_t r;
        uint8_t mine[PLINK_DEV_REC_MAX];
        size_t used = 0u, n = 0u;
        if (!plink_dec_dev_rec(sec, len, &r, &used) || used != len ||
            !plink_enc_dev_rec(&r, mine, sizeof mine, &n) ||
            !plink_home_rec_put(&h->recs, index, mine, n)) {
            return;
        }
    }
    h->frame_seq[index] = fs;
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        plink_kact_t *a = &k->acts[i];
        if (a->used && a->key == h->keys[index] && a->state != PLINK_CMD_PENDING &&
            (ack == 0u || a->req != ack)) {
            a->overlay = PLINK_DEV_OVL_NONE;
        }
    }
    k->dev_seq++;
}

/* The list the master names: pulled unless it is the one installed, the one being pulled, or one
 * that failed before - that one is never pulled again and the old list stays; no update is asked
 * for, as devices aren't alarm-critical (LINK-044). */
static void dev_list_check(plink_keypad_t *k, uint64_t now)
{
    const uint32_t crc = k->dev_named_crc;
    if (crc == 0u || crc == k->home->list_crc) {
        pull_stop(&k->dev_pull);
        return;
    }
    pull_start(k, &k->dev_pull, now, crc, 0u);
}

/* While the records' CRC under the installed list differs from the master's, DEV_STATE_REQ goes
 * 250 ms on (a full set takes several frames), then at least 1 s apart, doubling to 30 s
 * (LINK-045). Once they match, the next mismatch starts at 250 ms again - and at least 1 s after
 * the last request. */
static void dev_states_check(plink_keypad_t *k, uint64_t now)
{
    const plink_dev_mirror_t *h = k->home;
    if (h->list_crc == 0u || k->dev_named_crc != h->list_crc ||
        plink_home_states_crc(&h->recs) == k->dev_named_states) {
        k->dev_want = false;
        k->dev_gap_ms = PLINK_DEV_REQ_GAP_MS;
        k->dev_next_ms = k->dev_req_ms != 0u ? k->dev_req_ms + PLINK_DEV_REQ_GAP_MS : 0u;
        return;
    }
    if (!k->dev_want) {
        k->dev_want = true;
        if (k->dev_next_ms < now + PLINK_DEV_REQ_WAIT_MS) {
            k->dev_next_ms = now + PLINK_DEV_REQ_WAIT_MS;
        }
    }
}

/* Every record of the installed list ("from first to the last"). */
static void dev_state_req(plink_keypad_t *k, uint64_t now)
{
    const plink_dev_state_req_t q = {k->home->list_crc, 0u, 0u};
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_dev_state_req(&q, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_DEV_STATE_REQ, p, n);
    }
    k->dev_req_ms = now != 0u ? now : 1u;
    k->dev_next_ms = now + k->dev_gap_ms;
    k->dev_gap_ms = k->dev_gap_ms * 2u < PLINK_DEV_REQ_GAP_MAX_MS ? k->dev_gap_ms * 2u
                                                                  : PLINK_DEV_REQ_GAP_MAX_MS;
}

/* List and state requests go only with HOME on this transport, and wait while a panic is being
 * sent (docs/features/11 "Alarm traffic always wins"). */
static bool dev_requests_go(const plink_keypad_t *k)
{
    return k->home != NULL && cap_ok(k, PLINK_CAP_HOME) && !panic_waits(k);
}

static void dev_req_pump(plink_keypad_t *k, uint64_t now)
{
    if (!k->dev_want || !dev_requests_go(k) || now < k->dev_next_ms) {
        return;
    }
    dev_states_check(k, now); /* what came since may have settled it */
    if (k->dev_want) {
        dev_state_req(k, now);
    }
}

static void dev_pull_pump(plink_keypad_t *k, uint64_t now)
{
    if (dev_requests_go(k)) {
        pull_pump(k, &k->dev_pull, now);
    }
}

/* The whole list is in: its CRC and its rows checked (a newer compat is refused, never guessed
 * at), then installed in place of the old one. Each new row keeps the last record of the old row
 * with the same ref until its own comes, so a rename never blinks; LINK-026's count starts again;
 * then every record under the new list is asked for at once. One that fails is remembered and the
 * old list stays (LINK-044). */
static void dev_finish(plink_keypad_t *k, uint64_t now)
{
    plink_pull_t *pl = &k->dev_pull;
    plink_dev_mirror_t *h = k->home;
    const uint32_t crc = pl->crc;
    const uint32_t total = pl->total;
    pull_stop(pl);
    uint8_t count = 0u;
    if (plink_home_list_crc(pl->buf, total) != crc ||
        !plink_home_list_check(pl->buf, total, &count)) {
        pl->bad_crc = crc;
        return;
    }
    plink_home_list_keys(pl->buf, total, h->in_keys);
    plink_home_carry(h->keys, &h->recs, h->in_keys, count, &h->carry);
    memcpy(h->blob, pl->buf, total);
    h->blob_len = total;
    h->list_crc = crc;
    memcpy(h->keys, h->in_keys, sizeof h->keys);
    h->recs = h->carry;
    memset(h->frame_seq, 0, sizeof h->frame_seq);
    memset(h->off, 0, sizeof h->off);
    plink_home_walk_t w = {0u, 0u};
    for (;;) {
        const size_t at = w.pos < PLINK_DEV_BLOB_HDR ? PLINK_DEV_BLOB_HDR : w.pos;
        uint8_t i = 0u;
        if (!plink_home_list_next(h->blob, total, &w, &i, NULL, NULL)) {
            break;
        }
        h->off[i] = (uint16_t)at;
    }
    k->dev_list_seq++;
    k->dev_seq++;
    k->dev_want = true;
    k->dev_gap_ms = PLINK_DEV_REQ_GAP_MS;
    k->dev_next_ms = now;
    if (dev_requests_go(k)) {
        dev_state_req(k, now);
    }
}

static void on_dev_list_page(plink_keypad_t *k, uint64_t now, const uint8_t *p, size_t n)
{
    plink_dev_list_page_t pg;
    if (!plink_dec_dev_list_page(p, n, &pg)) {
        return;
    }
    if (pull_take(&k->dev_pull, &pg)) {
        dev_finish(k, now);
        return;
    }
    dev_pull_pump(k, now); /* the next pages at once */
}

/* A DEV_STATE (frame fs): device data, fresh, and its records applied, under the installed list;
 * its head - the master's list and states_crc - taken from a newer frame than the last taken. */
static void on_dev_state(plink_keypad_t *k, uint64_t now, uint32_t fs, const uint8_t *p, size_t n)
{
    plink_dev_state_t s;
    if (!plink_dec_dev_state(p, n, &s) || s.list_crc == 0u) {
        return;
    }
    dev_heard(k, now, s.list_crc);
    if (s.list_crc == k->home->list_crc) {
        size_t pos = 0u;
        uint8_t index = 0u;
        const uint8_t *item = NULL;
        size_t item_len = 0u;
        while (plink_dev_state_next(&s, &pos, &index, NULL, &item, &item_len)) {
            dev_apply(k, fs, index, item + 1, item_len - 1u, 0u);
        }
    }
    if (fs > k->dev_head_seq) {
        k->dev_head_seq = fs;
        k->dev_named_crc = s.list_crc;
        k->dev_named_states = s.states_crc;
        dev_list_check(k, now);
        dev_pull_pump(k, now);
    }
    dev_states_check(k, now);
}

/* ---- Smart Home: device actions (LINK-047) ------------------------------------------------ */

static void act_free(plink_kact_t *a)
{
    plink_wipe(a, sizeof *a);
}

static void act_send(plink_keypad_t *k, uint64_t now, const plink_kact_t *a)
{
    if (!cap_ok(k, PLINK_CAP_HOME)) {
        return; /* not on this transport: the copy is skipped, the schedule runs on */
    }
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_dev_act(&a->act, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_DEV_ACT, p, n);
    }
    plink_wipe(p, sizeof p); /* the code in clear */
}

/* Each action due goes again with the same req every 500 ms; at 3 s it is given up and never
 * sent again, and its code goes with its last copy. */
static void acts_tick(plink_keypad_t *k, uint64_t now)
{
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        plink_kact_t *a = &k->acts[i];
        if (!a->used || a->state != PLINK_CMD_PENDING || now < a->next_ms) {
            continue;
        }
        const uint64_t end = a->started_ms + PLINK_DEV_ACT_GIVE_UP_MS;
        if (now >= end) {
            a->state = PLINK_CMD_NO_ANSWER;
            plink_wipe(a->act.code, sizeof a->act.code);
            if (a->released) {
                a->overlay = PLINK_DEV_OVL_UNSURE;
            }
            k->dev_seq++;
            continue;
        }
        act_send(k, now, a);
        a->next_ms += PLINK_DEV_ACT_RETRY_MS;
        if (a->next_ms <= now) {
            a->next_ms = now + PLINK_DEV_ACT_RETRY_MS; /* a late tick: no burst to catch up */
        }
        a->next_ms = earliest(a->next_ms, end);
        if (a->next_ms >= end) {
            plink_wipe(a->act.code, sizeof a->act.code); /* that was its last copy */
        }
    }
}

/* A slot for an action on device key: a finished one on the same device first (the next action
 * on a device ends what the last one showed), then a free one, then the oldest finished one
 * showing nothing, then the oldest finished one. -1: every slot still sends. */
static int act_slot(plink_keypad_t *k, uint32_t key)
{
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        plink_kact_t *a = &k->acts[i];
        if (a->used && a->key == key && a->state != PLINK_CMD_PENDING) {
            act_free(a);
            return (int)i;
        }
    }
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        if (!k->acts[i].used) {
            return (int)i;
        }
    }
    int best = -1;
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        const plink_kact_t *a = &k->acts[i];
        if (a->state == PLINK_CMD_PENDING) {
            continue;
        }
        if (best < 0) {
            best = (int)i;
            continue;
        }
        const plink_kact_t *b = &k->acts[(size_t)best];
        const bool a_quiet = a->overlay == PLINK_DEV_OVL_NONE;
        const bool b_quiet = b->overlay == PLINK_DEV_OVL_NONE;
        if ((a_quiet && !b_quiet) || (a_quiet == b_quiet && a->started_ms < b->started_ms)) {
            best = (int)i;
        }
    }
    if (best >= 0) {
        act_free(&k->acts[(size_t)best]);
    }
    return best;
}

static plink_kact_t *act_of(plink_keypad_t *k, uint32_t req)
{
    for (size_t i = 0; req != 0u && i < PLINK_DEV_ACTS_MAX; i++) {
        if (k->acts[i].used && k->acts[i].req == req) {
            return &k->acts[i];
        }
    }
    return NULL;
}

/* What a finished action shows once its caller stopped waiting. */
static uint8_t act_overlay(const plink_kact_t *a)
{
    if (a->state == PLINK_CMD_PENDING) {
        return PLINK_DEV_OVL_PENDING;
    }
    if (a->state == PLINK_CMD_NO_ANSWER) {
        return PLINK_DEV_OVL_UNSURE;
    }
    return a->result == PLINK_DEV_OK ? (uint8_t)PLINK_DEV_OVL_NONE : (uint8_t)PLINK_DEV_OVL_FAILED;
}

/* A DEV_ACK (frame fs): its record - the device as it is now - device data, fresh, and applied
 * under the installed list, as a DEV_STATE's (the CRCs are compared at the next DEV_STATE, which
 * names the master's states after it; one with no record refreshes nothing); then the answer to
 * the action with its req, even one given up (a late refusal is definite). */
static void on_dev_ack(plink_keypad_t *k, uint64_t now, uint32_t fs, const uint8_t *p, size_t n)
{
    plink_dev_ack_t a;
    if (!plink_dec_dev_ack(p, n, &a)) {
        return;
    }
    dev_heard(k, now, a.has_rec ? a.list_crc : 0u);
    if (a.has_rec && a.list_crc == k->home->list_crc && k->home->list_crc != 0u) {
        dev_apply(k, fs, a.index, a.rec_raw, a.rec_raw_len, a.req);
    }
    plink_kact_t *c = act_of(k, a.req);
    if (c == NULL || c->state == PLINK_CMD_DONE) {
        return;
    }
    c->state = PLINK_CMD_DONE;
    c->result = a.result <= PLINK_DEV_UNAVAILABLE ? a.result : (uint8_t)PLINK_DEV_REFUSED;
    c->why = a.why;
    plink_wipe(c->act.code, sizeof c->act.code);
    c->overlay = c->released ? act_overlay(c) : (uint8_t)PLINK_DEV_OVL_NONE;
    k->dev_seq++;
}

/* ---- settings (LINK-050..052) ------------------------------------------------------------- */

/* A PREFS: the report as it is now, answering req (0: unasked for) with its refusals - only in a
 * live session with PREFS, on frames of 250 B or more (LINK-043). Every PREFS carries the whole
 * report, so one waiting to go is no longer needed. */
static void prefs_send(plink_keypad_t *k, uint64_t now, uint32_t req, const uint8_t *refused,
                       uint8_t count)
{
    if (!cap_ok(k, PLINK_CAP_PREFS)) {
        return;
    }
    plink_kprefs_t *s = &k->prefs;
    plink_prefs_t m;
    memset(&m, 0, sizeof m);
    m.req = req;
    memcpy(m.panel_id, s->panel_id, sizeof m.panel_id);
    m.voice = s->voice;
    m.accepts = s->accepts;
    m.refused_count = count;
    m.refused = count > 0u ? refused : NULL;
    m.fields = s->len > 0u ? s->fields : NULL;
    m.fields_len = s->len;
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_prefs(&m, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_PREFS, p, n);
    }
    s->due = false;
    s->next_ms = now + PLINK_PREFS_GAP_MS;
}

/* Whether an unasked-for PREFS may go: a report to send, a session with PREFS whose first STATUS
 * came (that STATUS asks for one anyway: a change before it waits for it, so a new session gets
 * one report, not two), and a second since the last one. */
static bool prefs_ready(const plink_keypad_t *k)
{
    const plink_kprefs_t *s = &k->prefs;
    return s->due && s->have && cap_ok(k, PLINK_CAP_PREFS) && s->first_sid == k->live.sid;
}

static void prefs_pump(plink_keypad_t *k, uint64_t now)
{
    if (prefs_ready(k) && now >= k->prefs.next_ms) {
        prefs_send(k, now, 0u, NULL, 0u);
    }
}

/* The report's values: a fields body framed as both codecs want it, within what the keypad's
 * PREFS carries. true when it changed. */
static bool prefs_store(plink_kprefs_t *s, const char *panel_id, uint8_t voice, uint32_t accepts,
                        const uint8_t *fields, size_t len, bool *ok)
{
    *ok = false;
    if (len > PLINK_PREFS_OWN_MAX || (fields == NULL && len > 0u)) {
        return false;
    }
    plink_prefs_t probe;
    memset(&probe, 0, sizeof probe);
    probe.fields = fields;
    probe.fields_len = len;
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (!plink_enc_prefs(&probe, p, sizeof p, &n)) {
        return false;
    }
    *ok = true;
    char id[PLINK_PANEL_ID_LEN + 1];
    memset(id, 0, sizeof id);
    const size_t idn = bounded_len(panel_id, PLINK_PANEL_ID_LEN);
    if (idn > 0u) {
        memcpy(id, panel_id, idn);
    }
    const bool same = s->have && memcmp(id, s->panel_id, sizeof id) == 0 && voice == s->voice &&
                      accepts == s->accepts && len == s->len &&
                      (len == 0u || memcmp(fields, s->fields, len) == 0);
    if (same) {
        return false;
    }
    s->have = true;
    memcpy(s->panel_id, id, sizeof id);
    s->voice = voice;
    s->accepts = accepts;
    s->len = (uint8_t)len;
    memset(s->fields, 0, sizeof s->fields);
    if (len > 0u) {
        memcpy(s->fields, fields, len);
    }
    s->crc = proto_prefs_crc(s->fields, s->len);
    return true;
}

/* A PREFS_SET (LINK-051): a repeat of the last one answered gets that answer again (kept 30 s); a
 * repeat of the one waiting or being applied is ignored; another one waits while one is being
 * applied (the master's resend brings it back), else it is the next to take. Never applied
 * twice. */
static void on_prefs_set(plink_keypad_t *k, uint64_t now, const uint8_t *p, size_t n)
{
    plink_prefs_set_t m;
    if (!plink_dec_prefs_set(p, n, &m) || m.fields_len > PLINK_PREFS_BODY_MAX) {
        return;
    }
    plink_kprefs_t *s = &k->prefs;
    if (s->ans_req != 0u && m.req == s->ans_req && now < s->ans_ms + PLINK_PREFS_KEEP_MS) {
        prefs_send(k, now, s->ans_req, s->ans_refused, s->ans_count);
        return;
    }
    if (s->in_state != PREFS_IN_NONE && (m.req == s->in.req || s->in_state == PREFS_IN_TAKEN)) {
        return;
    }
    plink_wipe(&s->in, sizeof s->in);
    s->in.req = m.req;
    s->in.origin = m.origin;
    s->in.len = (uint8_t)m.fields_len;
    if (m.fields_len > 0u) {
        memcpy(s->in.fields, m.fields, m.fields_len);
    }
    s->in_state = PREFS_IN_WAITING;
}

/* ---- the master's status (LINK-008, LINK-026) --------------------------------------------- */

/* Every item's frame order starts again in a new session. */
static void items_reset(plink_keypad_t *k, uint32_t sid)
{
    k->items_sid = sid;
    k->status_frame_seq = 0u;
    memset(k->part_frame_seq, 0, sizeof k->part_frame_seq);
    memset(k->zone_frame_seq, 0, sizeof k->zone_frame_seq);
    k->time_frame_seq = 0u;
    k->ev_frame_seq = 0u;
    k->dev_head_seq = 0u;
    if (k->home != NULL) {
        memset(k->home->frame_seq, 0, sizeof k->home->frame_seq);
    }
}

/* The system section as the keypad holds it at now: an older frame's partition records are
 * applied under it, so nothing of that frame's own (older) system section comes back. */
static void system_now(const plink_keypad_t *k, uint64_t now, plink_status_t *st)
{
    const plink_view_t *v = &k->view;
    const alarm_snapshot_t *s = &v->snap;
    const uint64_t gone = now > s->taken_ms ? now - s->taken_ms : 0u;
    st->epoch_ms = v->epoch_ms;
    if (v->epoch_ms != 0 && now > v->epoch_at_ms) {
        st->epoch_ms = v->epoch_ms + (int64_t)(now - v->epoch_at_ms);
    }
    st->partitions = s->partitions;
    st->home = v->home;
    st->sys_troubles = s->sys_troubles;
    st->sys_restored = s->sys_troubles_restored;
    st->dev_troubles = s->dev_troubles;
    st->default_codes = s->default_codes;
    st->flags = (uint8_t)((s->program ? PLINK_SF_PROGRAM : 0u) |
                          (s->exit_sounds ? PLINK_SF_EXIT_SOUNDS : 0u));
    st->recovery_left_ms = s->recovery_left_ms > gone ? (uint32_t)(s->recovery_left_ms - gone) : 0u;
    st->lockout_left_ms = 0u;
    if (s->local_lockout_until > now) {
        const uint64_t left = s->local_lockout_until - now;
        st->lockout_left_ms = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
    }
    st->report = v->report;
    st->zones_used = s->zones_used;
    st->last_event_seq = s->last_event_seq;
    st->cfg_crc = v->cfg_crc;
    st->cfg_len = v->cfg_len;
    st->zones_crc = v->zones_crc;
    st->mono_ms = 0u; /* the master's engine time carried on, never the older frame's */
    if (v->mono_ms != 0u) {
        st->mono_ms = v->mono_ms + (now > v->mono_at_ms ? now - v->mono_at_ms : 0u);
    }
}

/* The master moved this keypad to another home partition: kept in its record. */
static void home_check(plink_keypad_t *k, uint64_t now, uint8_t home)
{
    if (home >= 1u && home <= PLINK_PARTS_MAX && home != k->rec.home) {
        k->rec.home = home;
        (void)store_now(k, now, STORE_WRITE);
    }
}

static void on_status(plink_keypad_t *k, uint64_t now, uint32_t fs, const uint8_t *p, size_t n)
{
    plink_status_t *st = &k->scr.st;
    if (!plink_dec_status(p, n, st)) {
        return;
    }
    /* LINK-026: the system section only from a newer frame than the last one applied, and each
     * partition record only from a newer frame than its own last (a small transport's STATUS
     * comes in pieces, each with the system section and some records). */
    const bool system = fs > k->status_frame_seq;
    uint8_t keep = 0u;
    for (uint8_t i = 0; i < st->count; i++) {
        const uint8_t num = st->part[i].number;
        if (num >= 1u && num <= ALARM_MAX_PARTITIONS && fs > k->part_frame_seq[num - 1u]) {
            st->part[keep++] = st->part[i];
        }
    }
    st->count = keep;
    if (!system) {
        if (keep == 0u || !k->view.have_status) {
            return;
        }
        system_now(k, now, st);
    }
    plink_view_apply_status(&k->view, st, now);
    for (uint8_t i = 0; i < keep; i++) {
        k->part_frame_seq[st->part[i].number - 1u] = fs;
    }
    bump_seq(k);
    if (!system) {
        return;
    }
    k->status_frame_seq = fs;
    k->status_ms = now;
    k->silent_since_ms = now;
    home_check(k, now, st->home);
    cfg_check(k, now);
    zones_check(k, now);
    ev_check(k, now, fs);
    cfg_pump(k, now);
    ev_pump(k, now);
    /* LINK-050: a session's first STATUS asks for the settings report. */
    if (k->prefs.first_sid != k->items_sid) {
        k->prefs.first_sid = k->items_sid;
        k->prefs.due = true;
        prefs_pump(k, now);
    }
}

static void on_zones(plink_keypad_t *k, uint64_t now, uint32_t fs, const uint8_t *p, size_t n)
{
    plink_zones_t z;
    if (!plink_dec_zones(p, n, &z)) {
        return;
    }
    /* LINK-026 per zone: runs of records newer than what each zone holds go to the view. */
    bool any = false;
    uint32_t i = 0u;
    while (i < z.count) {
        uint32_t j = i;
        while (j < z.count) {
            const uint32_t zone = (uint32_t)z.first + j;
            if (zone < 1u || zone > ALARM_MAX_ZONES || fs <= k->zone_frame_seq[zone - 1u]) {
                break;
            }
            k->zone_frame_seq[zone - 1u] = fs;
            j++;
        }
        if (j > i) {
            plink_zones_t run = z;
            run.first = (uint16_t)(z.first + i);
            run.count = (uint16_t)(j - i);
            run.recs = z.recs + (size_t)i * z.rec_len;
            plink_view_apply_zones(&k->view, &run);
            any = true;
            i = j;
        } else {
            i++;
        }
    }
    if (any) {
        bump_seq(k);
        if (plink_view_zones_crc(&k->view) == k->view.zones_crc) {
            k->zones_want = false;
            k->zones_gap_ms = ZONES_GAP_MS;
        }
    }
    (void)now;
}

static void on_time(plink_keypad_t *k, uint64_t now, uint32_t fs, const uint8_t *p, size_t n)
{
    plink_time_t t;
    if (!plink_dec_time(p, n, &t) || fs <= k->time_frame_seq) {
        return;
    }
    k->time_frame_seq = fs;
    if (t.epoch_ms == 0) {
        return; /* the master's clock isn't set */
    }
    k->time = t;
    k->time_at_ms = now;
    k->time_changed = true;
}

static void on_result(plink_keypad_t *k, const uint8_t *p, size_t n)
{
    plink_result_t r;
    if (!plink_dec_result(p, n, &r) || r.req == 0u) {
        return;
    }
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        const plink_kcmd_t *c = &k->cmds[i];
        if (c->used && c->req == r.req && c->state == PLINK_CMD_PENDING) {
            cmd_end(k, i, PLINK_CMD_DONE, &r);
            return;
        }
    }
}

static void on_sound(plink_keypad_t *k, const uint8_t *p, size_t n)
{
    plink_sound_t s;
    if (!plink_dec_sound(p, n, &s)) {
        return;
    }
    const uint8_t cap = (uint8_t)(sizeof k->sounds / sizeof k->sounds[0]);
    if (k->sound_count == cap) {
        k->sound_head = (uint8_t)((k->sound_head + 1u) % cap); /* the oldest goes */
        k->sound_count--;
    }
    k->sounds[(k->sound_head + k->sound_count) % cap] = s;
    k->sound_count++;
}

/* ---- the link's state (LINK-008, LINK-025) ------------------------------------------------ */

/* Everything the keypad needs to show the master as it is: a session, a status, and the
 * configuration that status names. */
static bool synced(const plink_keypad_t *k)
{
    return k->live.valid && k->view.have_status && k->have_cfg &&
           k->cfg_installed_crc == k->view.cfg_crc;
}

static void update_state(plink_keypad_t *k, uint64_t now)
{
    if (!k->paired) {
        set_state(k, now, PLINK_LINK_NOT_PAIRED);
        return;
    }
    switch (k->state) {
    case PLINK_LINK_CONNECTING:
        if (synced(k) && now < k->status_ms + PLINK_LOST_MS) {
            set_state(k, now, PLINK_LINK_ONLINE);
        }
        break;
    case PLINK_LINK_ONLINE:
        if (now >= k->status_ms + PLINK_LOST_MS) {
            set_state(k, now, PLINK_LINK_LOST);
        }
        break;
    case PLINK_LINK_LOST:
        /* Back only with a status since the loss and no sooner than 3 s after it, so the
         * screen never flips back and forth (UI-020). */
        if (synced(k) && k->status_ms >= k->lost_since_ms &&
            now >= k->lost_since_ms + PLINK_LOST_MIN_MS) {
            set_state(k, now, PLINK_LINK_ONLINE);
        }
        break;
    default: set_state(k, now, PLINK_LINK_CONNECTING); break;
    }
}

/* The master's device list and what came under it (LINK-044..046): gone, the pull with it, so
 * Smart Home hides until a list installs again. The actions keep their slots and run their course
 * (their copies go only in a session with HOME). */
static void dev_list_drop(plink_keypad_t *k)
{
    pull_stop(&k->dev_pull);
    k->dev_pull.bad_crc = 0u;
    if (k->home != NULL) {
        plink_dev_mirror_t *h = k->home;
        h->list_crc = 0u;
        h->blob_len = 0u;
        memset(h->keys, 0, sizeof h->keys);
        memset(h->off, 0, sizeof h->off);
        memset(&h->recs, 0, sizeof h->recs);
        memset(h->frame_seq, 0, sizeof h->frame_seq);
        k->dev_list_seq++;
    }
    k->dev_named_crc = k->dev_named_states = 0u;
    k->dev_want = false;
    k->dev_next_ms = 0u;
    k->dev_gap_ms = PLINK_DEV_REQ_GAP_MS;
    k->dev_req_ms = 0u;
    k->dev_heard_ms = 0u;
    k->dev_fresh = false;
    k->dev_seq++;
}

/* What a master said goes: sessions, commands, status, configuration, events, sounds, time. */
static void forget_data(plink_keypad_t *k, uint64_t now)
{
    plink_session_wipe(&k->live);
    plink_session_wipe(&k->old);
    plink_hs_k_wipe(&k->hs);
    k->tentative = false;
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        cmd_free(k, i);
    }
    plink_view_init(&k->view);
    bump_seq(k);
    ev_drop(k);
    k->ev_want = false;
    pull_stop(&k->cfg_pull);
    k->have_cfg = false;
    k->cfg_installed_crc = 0u;
    k->cfg_pull.bad_crc = 0u;
    k->zones_want = false;
    k->zones_next_ms = 0u;
    k->zones_gap_ms = ZONES_GAP_MS;
    items_reset(k, 0u);
    memset(k->sounds, 0, sizeof k->sounds);
    k->sound_head = k->sound_count = 0u;
    memset(&k->time, 0, sizeof k->time);
    k->time_changed = false;
    k->status_ms = 0u;
    k->status_heard_ms = now;
    k->silent_since_ms = now;
    k->not_paired_since_ms = k->not_paired_last_ms = 0u;
    k->hs_due_ms = NEVER;
    k->hs_backoff_ms = HS_FIRST_MS;
    k->last_rekey_ms = NEVER;
    k->hb_due_ms = NEVER;
    k->session_started_ms = 0u;
    /* Its devices and their actions, and what it was asked to change; the keypad's own settings
     * report stays. */
    dev_list_drop(k);
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        act_free(&k->acts[i]);
    }
    plink_kprefs_t *s = &k->prefs;
    s->due = false;
    s->next_ms = 0u;
    s->first_sid = 0u;
    s->in_state = PREFS_IN_NONE;
    plink_wipe(&s->in, sizeof s->in);
    s->ans_req = 0u;
    s->ans_ms = 0u;
    s->ans_count = 0u;
    memset(s->ans_refused, 0, sizeof s->ans_refused);
}

/* Not paired any more: removed at the master (a BYE) or forgotten here. */
static bool drop_master(plink_keypad_t *k, uint64_t now, uint8_t why)
{
    forget_data(k, now);
    plink_wipe(&k->rec, sizeof k->rec);
    k->paired = false;
    k->unpaired_why = why;
    k->have_master = k->have_disc = false;
    k->disc_bcast = false;
    memset(&k->master, 0, sizeof k->master);
    memset(&k->disc, 0, sizeof k->disc);
    set_state(k, now, PLINK_LINK_NOT_PAIRED);
    return store_now(k, now, STORE_ERASE);
}

/* Pairing finished: the new master replaces any earlier one, its record is stored, and the
 * first handshake goes at once. */
static void pair_take(plink_keypad_t *k, uint64_t now)
{
    plink_keypad_rec_t r;
    if (!plink_pk_take(&k->pair, &r)) {
        return;
    }
    const plink_addr_t at = k->pair.master;
    forget_data(k, now); /* the event ring of an earlier pairing too */
    k->rec = r;
    plink_wipe(&r, sizeof r);
    k->paired = true;
    k->pairing = false;
    k->unpaired_why = PLINK_UNPAIRED_NEVER;
    k->master = k->disc = at;
    k->have_master = k->have_disc = true;
    k->disc_bcast = false; /* the master that answered, never a broadcast */
    (void)store_now(k, now, STORE_WRITE);
    k->state = PLINK_LINK_CONNECTING;
    k->state_since_ms = now;
    k->hs_due_ms = now;
    hs_send(k, now);
}

/* ---- frames from the master --------------------------------------------------------------- */

static void on_welcome(plink_keypad_t *k, uint64_t now, const plink_addr_t *from,
                       const plink_hdr_t *h, const uint8_t *frame, size_t len)
{
    /* Only the answer to the latest HELLO. Its mac2 can be checked only after an X25519, so a
     * device on the LAN could keep this task busy with WELCOMEs: they are taken only from where
     * the HELLO went (unless that was a broadcast: then the master answers from its own
     * address), and only so many per HELLO (the master's answer comes within a round trip; the
     * next HELLO opens the count again). */
    plink_hello_t w;
    if (!k->paired || !k->hs.waiting || h->net_id != k->rec.net_id ||
        (!k->hs_bcast && !plink_addr_equal(from, &k->hs_to)) ||
        k->hs_checks >= PLINK_WELCOME_CHECKS ||
        !plink_dec_hello(&frame[PLINK_HDR_LEN], len - PLINK_HDR_LEN, false, &w) ||
        w.proto == 0u) {
        return;
    }
    k->hs_checks++;
    plink_session_t s;
    if (!plink_hs_finish(k->c, k->rec.k_pair, &k->hs, h, frame, len, &s)) {
        return;
    }
    /* The session the master uses stays readable until it shows it has the new one. While one
     * is already tentative, the master still uses the one before it: that one stays. */
    if (k->tentative) {
        plink_session_wipe(&k->live);
    } else {
        plink_session_wipe(&k->old);
        k->old = k->live;
    }
    k->live = s;
    plink_session_wipe(&s);
    k->tentative = true;
    k->master = *from; /* the WELCOME is authenticated: the master may move (LINK-021) */
    k->have_master = true;
    k->status_heard_ms = now; /* the master's STATUS comes once it has the new session */
    k->session_started_ms = now;
    k->hs_due_ms = NEVER;
    k->hs_backoff_ms = HS_FIRST_MS;
    hb_send(k, now); /* the master promotes the session on its first frame */
}

static void on_rekey(plink_keypad_t *k, uint64_t now, const plink_hdr_t *h, const uint8_t *frame,
                     size_t len)
{
    plink_rekey_t r;
    if (!k->paired || h->net_id != k->rec.net_id || !k->live.valid || k->hs.waiting ||
        !plink_dec_rekey(&frame[PLINK_HDR_LEN], len - PLINK_HDR_LEN, &r) || r.sid != k->live.sid) {
        return;
    }
    /* A plain hint anyone could send: at most every 5 s, and the live session stays until a
     * WELCOME verifies. */
    if (k->last_rekey_ms != NEVER && now < k->last_rekey_ms + PLINK_REKEY_GAP_MS) {
        return;
    }
    k->last_rekey_ms = now;
    hs_soon(k, now);
    hs_send(k, now);
}

/* A plain hint, so it only changes what the keypad says, and only once it has kept coming for
 * 5 min; the keys stay until a new pairing completes. */
static void on_not_paired(plink_keypad_t *k, uint64_t now, const plink_addr_t *from)
{
    const bool ours = (k->have_disc && plink_addr_equal(from, &k->disc)) ||
                      (k->have_master && plink_addr_equal(from, &k->master));
    if (!k->paired || !ours) {
        return;
    }
    if (k->not_paired_since_ms == 0u || now > k->not_paired_last_ms + NOT_PAIRED_RUN_MS) {
        k->not_paired_since_ms = now != 0u ? now : 1u;
    }
    k->not_paired_last_ms = now;
}

static void on_sealed(plink_keypad_t *k, uint64_t now, const plink_addr_t *from,
                      const plink_hdr_t *h, const uint8_t *frame, size_t len)
{
    if (!k->paired || h->net_id != k->rec.net_id) {
        return;
    }
    plink_session_t *s = NULL;
    if (k->live.valid && h->sid == k->live.sid) {
        s = &k->live;
    } else if (k->tentative && k->old.valid && h->sid == k->old.sid) {
        s = &k->old;
    }
    size_t n = 0u;
    if (s == NULL || !plink_session_open(k->c, s, h, frame, len, k->pt, sizeof k->pt, &n)) {
        return;
    }
    k->master = *from; /* authenticated, tag and replay checked (LINK-021) */
    k->have_master = true;
    if (h->type == PLINK_T_STATUS) {
        k->status_heard_ms = now; /* (one LINK-026 won't apply still says the link works) */
    }
    k->not_paired_since_ms = k->not_paired_last_ms = 0u;
    if (s == &k->live && k->tentative) {
        /* The master has the new session: the old one, and its frames, are done. */
        k->tentative = false;
        plink_session_wipe(&k->old);
        hb_schedule(k, now);
    }
    if (s->sid != k->items_sid) {
        items_reset(k, s->sid);
        /* LINK-043 ("Versions"): a session without HOME - a master that serves no devices now,
         * such as a v1 one - drops the list, so Smart Home hides instead of every tile reading
         * offline until the keypad restarts. */
        if (k->home != NULL && (s->caps & PLINK_CAP_HOME) == 0u &&
            (k->home->list_crc != 0u || k->dev_pull.pulling || k->dev_named_crc != 0u ||
             k->dev_heard_ms != 0u)) {
            dev_list_drop(k);
        }
    }
    const uint32_t fs = h->seq;
    /* LINK-043: a capability's frames only from a session holding its bit (Smart Home's only
     * with a mirror); LINK-052: their direction was checked by plink_hdr_read. */
    const bool home = k->home != NULL && (s->caps & PLINK_CAP_HOME) != 0u;
    const bool prefs = (s->caps & PLINK_CAP_PREFS) != 0u;
    switch (h->type) {
    case PLINK_T_STATUS: on_status(k, now, fs, k->pt, n); break;
    case PLINK_T_ZONES: on_zones(k, now, fs, k->pt, n); break;
    case PLINK_T_CFG_PAGE: on_cfg_page(k, now, k->pt, n); break;
    case PLINK_T_EVENTS: on_events(k, now, fs, k->pt, n); break;
    case PLINK_T_RESULT: on_result(k, k->pt, n); break;
    case PLINK_T_SOUND: on_sound(k, k->pt, n); break;
    case PLINK_T_TIME: on_time(k, now, fs, k->pt, n); break;
    case PLINK_T_BYE: (void)drop_master(k, now, PLINK_UNPAIRED_REMOVED); break; /* LINK-014 */
    case PLINK_T_DEV_LIST_PAGE:
        if (home) {
            on_dev_list_page(k, now, k->pt, n);
        }
        break;
    case PLINK_T_DEV_STATE:
        if (home) {
            on_dev_state(k, now, fs, k->pt, n);
        }
        break;
    case PLINK_T_DEV_ACK:
        if (home) {
            on_dev_ack(k, now, fs, k->pt, n);
        }
        break;
    case PLINK_T_PREFS_REQ:
        if (prefs) {
            k->prefs.due = true; /* whatever CRC the master holds: the report goes */
            prefs_pump(k, now);
        }
        break;
    case PLINK_T_PREFS_SET:
        if (prefs) {
            on_prefs_set(k, now, k->pt, n);
        }
        break;
    default: break;
    }
    plink_wipe(k->pt, n);
}

/* ---- the API ------------------------------------------------------------------------------ */

void plink_keypad_init(plink_keypad_t *k, const plink_crypto_t *c, const plink_keypad_port_t *port,
                       const plink_keypad_rec_t *rec, uint8_t *cfg_buf, size_t cfg_cap,
                       alarm_config_t *cfg, uint8_t kind, const plink_caps_t *cap, uint16_t caps,
                       const char *name, const char *fw, uint64_t now_ms)
{
    if (k == NULL) {
        return;
    }
    plink_wipe(k, sizeof *k);
    k->c = c;
    if (port != NULL) {
        k->port = *port;
    }
    k->cfg_pull.buf = cfg_buf;
    k->cfg_pull.cap = cfg_buf != NULL && cfg != NULL ? cfg_cap : 0u;
    k->cfg_pull.type = PLINK_T_CFG_REQ;
    k->cfg = cfg;
    k->dev_pull.type = PLINK_T_DEV_LIST_REQ; /* its memory comes with a mirror */
    k->kind = kind;
    if (cap != NULL) {
        k->cap = *cap;
    }
    /* LINK-043: HOME only once a mirror is attached (plink_keypad_home_attach). */
    k->caps_asked = (uint16_t)(caps & PLINK_CAPS_KNOWN);
    k->caps = (uint16_t)(k->caps_asked & ~PLINK_CAP_HOME);
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), k->name, sizeof k->name);
    plink_clean_text(fw, bounded_len(fw, PLINK_FW_MAX), k->fw, sizeof k->fw);
    plink_pk_init(&k->pair, c, kind, cap, k->caps, name, fw);
    k->started_ms = now_ms;
    forget_data(k, now_ms);
    k->seq = 0u;
    k->view.snap.seq = 0u;
    /* LINK-018: the request ids start from a random base, so a keypad restarted within the
     * master's 90 s cache never meets its own earlier ids. */
    k->next_req = rnd32(k);
    if (k->next_req == 0u) {
        k->next_req = 1u;
    }
    k->state = PLINK_LINK_NOT_PAIRED;
    k->state_since_ms = now_ms;
    if (rec != NULL && rec->net_id != 0u && rec->device >= PLINK_DEVICE_FIRST &&
        rec->device <= PLINK_DEVICE_LAST) {
        k->rec = *rec;
        k->paired = true;
        k->state = PLINK_LINK_CONNECTING;
        k->hs_due_ms = now_ms; /* a handshake at start-up */
    }
}

void plink_keypad_master_addr(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *addr,
                              bool broadcast)
{
    if (k == NULL || addr == NULL) {
        return;
    }
    const bool bcast = broadcast || plink_addr_is_broadcast(addr);
    const bool changed =
        !k->have_disc || !plink_addr_equal(&k->disc, addr) || k->disc_bcast != bcast;
    k->disc = *addr;
    k->disc_bcast = bcast;
    k->have_disc = true;
    if (!k->have_master) {
        k->master = *addr;
        k->have_master = true;
    }
    /* A new place to look while a handshake waits, or the link isn't up: try it now. Sealed
     * frames still go where the last authenticated frame came from. */
    if (k->paired && changed && (k->hs.waiting || k->state != PLINK_LINK_ONLINE)) {
        k->hs_due_ms = earliest(k->hs_due_ms, now_ms);
    }
}

void plink_keypad_input(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *from,
                        const uint8_t *frame, size_t len)
{
    plink_hdr_t h;
    if (k == NULL || from == NULL || !plink_hdr_read(frame, len, &h) ||
        (h.flags & PLINK_FLAG_FROM_MASTER) == 0u) {
        return; /* a keypad hears only its master */
    }
    switch (h.type) {
    case PLINK_T_PAIR_COMMIT:
    case PLINK_T_PAIR_REVEAL:
    case PLINK_T_PAIR_DONE:
    case PLINK_T_PAIR_FAIL:
        /* LINK-016: pairing traffic counts only while a person pairs this keypad. */
        if (k->pairing) {
            plink_pk_input(&k->pair, now_ms, from, &h, frame, len, &k->out);
            send_out(k);
            pair_take(k, now_ms);
        }
        break;
    case PLINK_T_WELCOME: on_welcome(k, now_ms, from, &h, frame, len); break;
    case PLINK_T_REKEY: on_rekey(k, now_ms, &h, frame, len); break;
    case PLINK_T_NOT_PAIRED: on_not_paired(k, now_ms, from); break;
    default:
        if ((h.flags & PLINK_FLAG_SEALED) != 0u) {
            on_sealed(k, now_ms, from, &h, frame, len);
        }
        break; /* BEACON: ESP-NOW's */
    }
    update_state(k, now_ms);
}

void plink_keypad_tick(plink_keypad_t *k, uint64_t now_ms)
{
    if (k == NULL) {
        return;
    }
    if (k->pairing) {
        plink_pk_tick(&k->pair, now_ms, &k->out);
        send_out(k);
    }
    if (k->store_due != STORE_NONE && now_ms >= k->store_at_ms) {
        (void)store_now(k, now_ms, k->store_due);
    }
    if (!k->paired) {
        return;
    }
    hs_tick(k, now_ms);
    if (k->live.valid && now_ms >= k->hb_due_ms) {
        hb_send(k, now_ms);
    }
    cmds_tick(k, now_ms);
    cfg_pump(k, now_ms);
    zones_pump(k, now_ms);
    ev_pump(k, now_ms);
    if (k->home != NULL) {
        acts_tick(k, now_ms);
        if (k->dev_fresh && !dev_fresh_at(k, now_ms)) {
            k->dev_fresh = false; /* LINK-046: every row now reads offline */
            k->dev_seq++;
        }
        dev_pull_pump(k, now_ms);
        dev_req_pump(k, now_ms);
    }
    prefs_pump(k, now_ms);
    update_state(k, now_ms);
}

uint64_t plink_keypad_next(const plink_keypad_t *k)
{
    if (k == NULL) {
        return NEVER;
    }
    uint64_t t = NEVER;
    if (k->pairing) {
        t = earliest(t, plink_pk_next(&k->pair));
    }
    if (k->store_due != STORE_NONE) {
        t = earliest(t, k->store_at_ms);
    }
    if (!k->paired) {
        return t;
    }
    t = earliest(t, k->hs_due_ms);
    if ((k->have_disc || k->have_master) && !k->hs.waiting && k->hs_due_ms == NEVER) {
        t = earliest(t, k->status_heard_ms + PLINK_REHANDSHAKE_MS);
        if (k->live.valid) {
            t = earliest(t, k->session_started_ms + PLINK_SESSION_MAX_MS);
        }
    }
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        if (k->cmds[i].used && k->cmds[i].state == PLINK_CMD_PENDING) {
            t = earliest(t, k->cmds[i].next_ms);
        }
    }
    if (k->live.valid) {
        t = earliest(t, k->hb_due_ms);
        t = earliest(t, pull_next(k, &k->cfg_pull));
        if (k->zones_want) {
            t = earliest(t, k->zones_next_ms);
        }
        if (k->ev_want) {
            t = earliest(t, k->ev_next_ms);
        }
    }
    if (k->home != NULL) {
        for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
            if (k->acts[i].used && k->acts[i].state == PLINK_CMD_PENDING) {
                t = earliest(t, k->acts[i].next_ms);
            }
        }
        if (k->dev_fresh) {
            t = earliest(t, k->dev_heard_ms + PLINK_DEV_STALE_MS);
        }
        if (dev_requests_go(k)) { /* paused: they go again at the tick that ends the pause */
            t = earliest(t, pull_next(k, &k->dev_pull));
            if (k->dev_want) {
                t = earliest(t, k->dev_next_ms);
            }
        }
    }
    if (prefs_ready(k)) {
        t = earliest(t, k->prefs.next_ms);
    }
    if (k->state == PLINK_LINK_ONLINE) {
        t = earliest(t, k->status_ms + PLINK_LOST_MS);
    } else if (k->state == PLINK_LINK_LOST && synced(k) && k->status_ms >= k->lost_since_ms) {
        t = earliest(t, k->lost_since_ms + PLINK_LOST_MIN_MS);
    }
    return t;
}

void plink_keypad_addr_changed(plink_keypad_t *k, uint64_t now_ms)
{
    /* The master learns the new address from an authenticated frame (LINK-021). */
    if (k != NULL && k->paired && k->live.valid) {
        hb_send(k, now_ms);
    }
}

void plink_keypad_set_power(plink_keypad_t *k, uint8_t flags, int8_t rssi)
{
    if (k != NULL) {
        k->power_flags = flags;
        k->rssi = rssi;
    }
}

/* ---- pairing ------------------------------------------------------------------------------ */

void plink_keypad_pair_start(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *master,
                             bool broadcast, uint16_t net_id)
{
    if (k == NULL || master == NULL) {
        return;
    }
    k->pairing = true;
    plink_pk_start(&k->pair, now_ms, master, broadcast, net_id, &k->out);
    send_out(k);
}

void plink_keypad_pair_retry(plink_keypad_t *k, uint64_t now_ms)
{
    if (k == NULL || !k->pairing) {
        return;
    }
    plink_pk_retry(&k->pair, now_ms, &k->out);
    send_out(k);
}

void plink_keypad_pair_confirm(plink_keypad_t *k, uint64_t now_ms, bool match)
{
    if (k == NULL || !k->pairing) {
        return;
    }
    plink_pk_confirm(&k->pair, now_ms, match, &k->out);
    send_out(k);
}

void plink_keypad_pair_cancel(plink_keypad_t *k, uint64_t now_ms)
{
    if (k == NULL) {
        return;
    }
    plink_pk_cancel(&k->pair, now_ms, &k->out);
    send_out(k);
    k->pairing = false;
}

void plink_keypad_pair_info(const plink_keypad_t *k, uint64_t now_ms, plink_pair_info_t *out)
{
    plink_pk_info(k != NULL ? &k->pair : NULL, now_ms, out);
}

/* ---- what the UI shows -------------------------------------------------------------------- */

void plink_keypad_info(const plink_keypad_t *k, uint64_t now_ms, plink_keypad_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->status_age_s = UINT32_MAX;
    if (k == NULL) {
        return;
    }
    out->state = k->state;
    out->unpaired_why = k->paired ? (uint8_t)PLINK_UNPAIRED_NEVER : k->unpaired_why;
    out->not_paired_hint =
        k->paired && k->not_paired_since_ms != 0u &&
        k->not_paired_last_ms >= k->not_paired_since_ms + PLINK_NOT_PAIRED_HINT_MS;
    const uint64_t in_state =
        now_ms > k->state_since_ms ? (now_ms - k->state_since_ms) / 1000u : 0u;
    out->since_s = in_state > UINT32_MAX ? UINT32_MAX : (uint32_t)in_state;
    if (k->view.have_status) {
        const uint64_t age = now_ms > k->status_ms ? (now_ms - k->status_ms) / 1000u : 0u;
        out->status_age_s = age >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)age;
    }
    /* LINK-025: 10 min without system communication, from the last status (or from the start
     * of connecting when none came); and at once when not paired - removed at the master or
     * never paired, no master can check a code and there is nothing to protect. */
    out->maintenance = !k->paired || (k->state != PLINK_LINK_ONLINE &&
                                      now_ms >= k->silent_since_ms + PLINK_MAINTENANCE_MS);
    if (k->paired) {
        out->device = k->rec.device;
        out->home = k->rec.home;
        memcpy(out->master_name, k->rec.master_name, sizeof out->master_name);
    }
    if (k->have_master) {
        out->master = k->master;
    }
    out->caps = k->live.valid ? k->live.caps : 0u;
    /* Only while the latest status still names it: the master may have moved on (a newer
     * configuration being pulled is no reason to update). */
    const bool named = k->view.have_status && k->view.cfg_crc == k->cfg_pull.bad_crc;
    out->cfg_bad_crc = named ? k->cfg_pull.bad_crc : 0u;
}

const alarm_snapshot_t *plink_keypad_snapshot(const plink_keypad_t *k)
{
    return k != NULL ? &k->view.snap : NULL;
}

uint32_t plink_keypad_seq(const plink_keypad_t *k)
{
    return k != NULL ? k->seq : 0u;
}

uint32_t plink_keypad_config_seq(const plink_keypad_t *k)
{
    return k != NULL ? k->config_seq : 0u;
}

const alarm_config_t *plink_keypad_config(const plink_keypad_t *k)
{
    return k != NULL && k->have_cfg ? k->cfg : NULL;
}

size_t plink_keypad_events(const plink_keypad_t *k, alarm_event_t *out, size_t max)
{
    if (k == NULL || out == NULL) {
        return 0u;
    }
    const size_t n = k->ev_count < max ? k->ev_count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = k->events[k->ev_count - 1u - i];
    }
    return n;
}

bool plink_keypad_take_sound(plink_keypad_t *k, plink_sound_t *out)
{
    if (k == NULL || out == NULL || k->sound_count == 0u) {
        return false;
    }
    const uint8_t cap = (uint8_t)(sizeof k->sounds / sizeof k->sounds[0]);
    *out = k->sounds[k->sound_head];
    k->sound_head = (uint8_t)((k->sound_head + 1u) % cap);
    k->sound_count--;
    return true;
}

bool plink_keypad_take_time(plink_keypad_t *k, plink_time_t *out)
{
    if (k == NULL || out == NULL || !k->time_changed) {
        return false;
    }
    *out = k->time;
    k->time_changed = false;
    return true;
}

int64_t plink_keypad_epoch_ms(const plink_keypad_t *k, uint64_t now_ms)
{
    if (k == NULL) {
        return 0;
    }
    if (k->view.have_status && k->view.epoch_ms != 0) {
        const uint64_t gone = now_ms > k->view.epoch_at_ms ? now_ms - k->view.epoch_at_ms : 0u;
        return k->view.epoch_ms + (int64_t)gone;
    }
    if (k->time.epoch_ms != 0) {
        const uint64_t gone = now_ms > k->time_at_ms ? now_ms - k->time_at_ms : 0u;
        return k->time.epoch_ms + (int64_t)gone;
    }
    return 0;
}

uint64_t plink_keypad_mono_ms(const plink_keypad_t *k, uint64_t now_ms)
{
    if (k == NULL || !k->view.have_status || k->view.mono_ms == 0u) {
        return 0u;
    }
    const uint64_t gone = now_ms > k->view.mono_at_ms ? now_ms - k->view.mono_at_ms : 0u;
    return k->view.mono_ms + gone;
}

uint8_t plink_keypad_report(const plink_keypad_t *k)
{
    return k != NULL ? k->view.report : (uint8_t)PLINK_REPORT_NONE;
}

/* ---- commands ----------------------------------------------------------------------------- */

uint32_t plink_keypad_command(plink_keypad_t *k, uint64_t now_ms, const plink_cmd_t *cmd)
{
    if (k == NULL || cmd == NULL || !k->paired ||
        (cmd->op != PLINK_OP_COMMAND && cmd->op != PLINK_OP_AUTH &&
         cmd->op != PLINK_OP_CHANGE_CODE)) {
        return 0u;
    }
    const bool panic = is_panic(cmd);
    if (!panic && k->state != PLINK_LINK_ONLINE) {
        return 0u; /* LINK-008: only a panic waits for the link (LINK-023) */
    }
    if (panic) {
        /* The same panic keyed again while it is still being sent is the same request, and its
         * caller waits for it again: a release from an earlier wait is undone, or the answer
         * would free the slot before the caller reads it. */
        for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
            const plink_kcmd_t *c = &k->cmds[i];
            if (c->used && c->panic && c->state == PLINK_CMD_PENDING &&
                c->cmd.cmd.partition == cmd->cmd.partition && c->cmd.cmd.panic == cmd->cmd.panic) {
                k->cmd_released = (uint8_t)(k->cmd_released & ~(1u << (unsigned)i));
                return c->req;
            }
        }
    }
    /* A code the frame can't carry is refused now, not at every retry. */
    plink_cmd_t probe = *cmd;
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    const bool fits = plink_enc_cmd(&probe, p, sizeof p, &n);
    plink_wipe(p, sizeof p);
    plink_wipe(&probe, sizeof probe);
    const int slot = fits ? cmd_slot(k, panic) : -1;
    if (slot < 0) {
        return 0u;
    }
    uint32_t req = k->next_req++;
    if (req == 0u) {
        req = k->next_req++; /* 0 means none */
    }
    plink_kcmd_t *c = &k->cmds[(size_t)slot];
    c->used = true;
    c->req = req;
    c->cmd = *cmd;
    c->cmd.req = req;
    c->started_ms = now_ms;
    c->next_ms = now_ms;
    c->panic = panic;
    c->state = PLINK_CMD_PENDING;
    if (panic && k->hs.waiting && k->hs_due_ms > now_ms + HS_PANIC_MS) {
        k->hs_due_ms = now_ms + HS_PANIC_MS; /* a handshake under way retries within 2 s */
    }
    cmds_tick(k, now_ms); /* the first copy goes now */
    return req;
}

plink_cmd_state_t plink_keypad_command_state(const plink_keypad_t *k, uint32_t req,
                                             plink_result_t *res)
{
    if (k == NULL || req == 0u) {
        return PLINK_CMD_NONE;
    }
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        const plink_kcmd_t *c = &k->cmds[i];
        if (c->used && c->req == req) {
            if (c->state == PLINK_CMD_DONE && res != NULL) {
                *res = c->res;
            }
            return (plink_cmd_state_t)c->state;
        }
    }
    return PLINK_CMD_NONE;
}

void plink_keypad_command_release(plink_keypad_t *k, uint32_t req)
{
    if (k == NULL || req == 0u) {
        return;
    }
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        const plink_kcmd_t *c = &k->cmds[i];
        if (c->used && c->req == req) {
            if (c->state == PLINK_CMD_PENDING) {
                k->cmd_released = (uint8_t)(k->cmd_released | (1u << (unsigned)i));
            } else {
                cmd_free(k, i);
            }
            return;
        }
    }
}

bool plink_keypad_panic_pending(const plink_keypad_t *k)
{
    return k != NULL && panic_waits(k);
}

bool plink_keypad_forget(plink_keypad_t *k, uint64_t now_ms)
{
    if (k == NULL) {
        return false;
    }
    return drop_master(k, now_ms, PLINK_UNPAIRED_NEVER);
}

/* ---- Smart Home --------------------------------------------------------------------------- */

void plink_keypad_home_attach(plink_keypad_t *k, plink_dev_mirror_t *mem)
{
    if (k == NULL) {
        return;
    }
    k->home = mem;
    if (mem != NULL) {
        memset(mem, 0, sizeof *mem);
    }
    pull_stop(&k->dev_pull);
    k->dev_pull.buf = mem != NULL ? mem->in : NULL;
    k->dev_pull.cap = mem != NULL ? sizeof mem->in : 0u;
    k->dev_pull.bad_crc = 0u;
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        act_free(&k->acts[i]);
    }
    k->dev_list_seq++;
    k->dev_seq++;
    /* LINK-043: HOME is offered (in HELLO, and PAIR_REQ's caps) only with a mirror. */
    k->caps = (uint16_t)(mem != NULL ? k->caps_asked : (k->caps_asked & ~PLINK_CAP_HOME));
    k->pair.caps = k->caps;
}

void plink_keypad_dev_info(const plink_keypad_t *k, uint64_t now_ms, plink_dev_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (k == NULL) {
        return;
    }
    out->attached = k->home != NULL;
    out->shared = out->attached && k->live.valid && (k->live.caps & PLINK_CAP_HOME) != 0u;
    if (k->home != NULL) {
        out->list_crc = k->home->list_crc;
        out->count = k->home->list_crc != 0u ? k->home->recs.count : 0u;
    }
    out->list_seq = k->dev_list_seq;
    out->seq = k->dev_seq;
    out->fresh = out->attached && k->state == PLINK_LINK_ONLINE && dev_fresh_at(k, now_ms);
    out->heard_ms = k->dev_heard_ms;
}

bool plink_keypad_dev_row(const plink_keypad_t *k, uint8_t index, plink_dev_cat_t *cat,
                          bool *usable)
{
    plink_dev_cat_t c;
    memset(&c, 0, sizeof c);
    if (usable != NULL) {
        *usable = false;
    }
    if (cat != NULL) {
        *cat = c;
    }
    if (k == NULL || k->home == NULL || k->home->list_crc == 0u || index >= k->home->recs.count) {
        return false;
    }
    const bool ok = dev_cat(k->home, index, &c);
    if (!ok) {
        memset(&c, 0, sizeof c);
    }
    if (cat != NULL) {
        *cat = c;
    }
    if (usable != NULL) {
        *usable = ok;
    }
    return true;
}

bool plink_keypad_dev_rec(const plink_keypad_t *k, uint8_t index, plink_dev_rec_t *rec)
{
    if (k == NULL || k->home == NULL || k->home->list_crc == 0u) {
        return false;
    }
    return plink_home_rec(&k->home->recs, index, rec);
}

uint8_t plink_keypad_dev_overlay(const plink_keypad_t *k, uint8_t index, uint8_t *action)
{
    if (action != NULL) {
        *action = ACT_NONE;
    }
    if (k == NULL || k->home == NULL || index >= k->home->recs.count ||
        k->home->keys[index] == 0u) {
        return PLINK_DEV_OVL_NONE;
    }
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        const plink_kact_t *a = &k->acts[i];
        if (a->used && a->key == k->home->keys[index] && a->overlay != PLINK_DEV_OVL_NONE) {
            if (action != NULL) {
                *action = a->act.action;
            }
            return a->overlay;
        }
    }
    return PLINK_DEV_OVL_NONE;
}

bool plink_keypad_dev_view(const plink_keypad_t *k, uint64_t now_ms, uint8_t index,
                           plink_dev_view_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->action = ACT_NONE;
    plink_home_values_of(NULL, &out->values);
    if (k == NULL || k->home == NULL || k->home->list_crc == 0u || index >= k->home->recs.count) {
        return false;
    }
    out->usable = dev_cat(k->home, index, &out->cat);
    if (!out->usable) {
        memset(&out->cat, 0, sizeof out->cat);
        return true;
    }
    plink_dev_rec_t rec;
    out->have_rec = plink_home_rec(&k->home->recs, index, &rec);
    if (out->have_rec) {
        memcpy(out->word, rec.word, sizeof out->word);
        plink_home_values_of(&rec, &out->values);
        out->online = (rec.flags & PLINK_DEVF_ONLINE) != 0u;
        out->pending = (rec.flags & PLINK_DEVF_PENDING) != 0u;
        out->failed = (rec.flags & PLINK_DEVF_FAILED) != 0u;
        out->unsure = (rec.flags & PLINK_DEVF_UNSURE) != 0u;
        out->action = rec.action;
    }
    /* This keypad's own action, once its caller stopped waiting, until a later record or the next
     * action on the device (the overlay is gone then). */
    uint8_t action = ACT_NONE;
    const uint8_t ovl = plink_keypad_dev_overlay(k, index, &action);
    if (ovl != PLINK_DEV_OVL_NONE) {
        out->pending = ovl == PLINK_DEV_OVL_PENDING;
        out->failed = !out->pending;
        out->unsure = ovl == PLINK_DEV_OVL_UNSURE;
        out->action = action;
    }
    /* LINK-046: stale device data, or a link that isn't ONLINE, shows every row offline since the
     * last device data; a row the master already said is offline keeps its own time. */
    if (k->state != PLINK_LINK_ONLINE || !dev_fresh_at(k, now_ms)) {
        out->since_last_data = out->online || out->values.since == 0u;
        out->online = false;
    }
    return true;
}

/* ACT_SET_MODE's mode: exactly one of the four the wire knows. */
static bool mode_ok(uint8_t mode)
{
    return mode == PLINK_DEV_MODE_OFF || mode == PLINK_DEV_MODE_HEAT ||
           mode == PLINK_DEV_MODE_COOL || mode == PLINK_DEV_MODE_AUTO;
}

uint8_t plink_keypad_dev_act(plink_keypad_t *k, uint64_t now_ms, const char *ref, uint8_t action,
                             int32_t value, uint8_t mode, const char *code, uint32_t *ticket)
{
    if (ticket != NULL) {
        *ticket = 0u;
    }
    if (k == NULL || k->home == NULL || ref == NULL) {
        return PLINK_DEV_UNAVAILABLE;
    }
    /* LINK-046: nothing goes while the device data can't be trusted. */
    if (!k->paired || k->state != PLINK_LINK_ONLINE || !cap_ok(k, PLINK_CAP_HOME) ||
        !dev_fresh_at(k, now_ms)) {
        return PLINK_DEV_UNREACHABLE;
    }
    const plink_dev_mirror_t *h = k->home;
    uint8_t index = 0u;
    plink_dev_cat_t cat;
    memset(&cat, 0, sizeof cat);
    if (h->list_crc == 0u || !dev_find(h, ref, &index, &cat)) {
        return PLINK_DEV_REFUSED;
    }
    plink_dev_rec_t rec;
    if (!plink_home_rec(&h->recs, index, &rec) || (rec.flags & PLINK_DEVF_ONLINE) == 0u) {
        return PLINK_DEV_UNREACHABLE;
    }
    /* The master judges again; these are refused here so the screen answers at once (and a
     * Z-Wave row only until BL-169). */
    if (cat.source == DSRC_ZWAVE || !hc_action_valid((dev_kind_t)cat.kind, (dev_action_t)action) ||
        (action == ACT_SET_MODE && !mode_ok(mode))) {
        return PLINK_DEV_REFUSED;
    }
    const bool coded = code != NULL && code[0] != '\0';
    if (!coded && hc_needs_code((dev_kind_t)cat.kind, (dev_action_t)action, value)) {
        return PLINK_DEV_NEED_CODE;
    }
    const uint32_t key = h->keys[index];
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        const plink_kact_t *a = &k->acts[i];
        if (a->used && a->key == key && a->state == PLINK_CMD_PENDING) {
            return PLINK_DEV_BUSY; /* one action on a device at a time, as hc_act's */
        }
    }
    plink_dev_act_t act;
    memset(&act, 0, sizeof act);
    act.action = action;
    act.value = value;
    act.mode = action == ACT_SET_MODE ? mode : 0u;
    memcpy(act.ref, cat.ref, sizeof act.ref);
    if (coded) {
        const size_t cn = bounded_len(code, sizeof act.code);
        if (cn >= sizeof act.code) {
            plink_wipe(&act, sizeof act);
            return PLINK_DEV_REFUSED; /* longer than any code */
        }
        memcpy(act.code, code, cn);
    }
    /* A code the frame can't carry is refused now, not at every resend. */
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    const bool fits = plink_enc_dev_act(&act, p, sizeof p, &n);
    plink_wipe(p, sizeof p);
    const int slot = fits ? act_slot(k, key) : -1;
    if (slot < 0) {
        plink_wipe(&act, sizeof act);
        return fits ? (uint8_t)PLINK_DEV_BUSY : (uint8_t)PLINK_DEV_REFUSED;
    }
    uint32_t req = k->next_req++;
    if (req == 0u) {
        req = k->next_req++; /* 0 means none */
    }
    plink_kact_t *a = &k->acts[(size_t)slot];
    a->used = true;
    a->req = req;
    a->act = act;
    a->act.req = req;
    plink_wipe(&act, sizeof act);
    a->key = key;
    a->started_ms = now_ms;
    a->next_ms = now_ms;
    a->state = PLINK_CMD_PENDING;
    a->overlay = PLINK_DEV_OVL_NONE;
    k->dev_seq++;
    acts_tick(k, now_ms); /* the first copy goes now */
    if (ticket != NULL) {
        *ticket = req;
    }
    return PLINK_DEV_OK;
}

plink_cmd_state_t plink_keypad_dev_act_state(const plink_keypad_t *k, uint32_t ticket,
                                             plink_dev_answer_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (k == NULL || ticket == 0u) {
        return PLINK_CMD_NONE;
    }
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        const plink_kact_t *a = &k->acts[i];
        if (a->used && a->req == ticket) {
            if (out != NULL) {
                out->result = a->result;
                out->why = a->why;
                out->overlay = a->overlay;
            }
            return (plink_cmd_state_t)a->state;
        }
    }
    return PLINK_CMD_NONE;
}

void plink_keypad_dev_act_release(plink_keypad_t *k, uint32_t ticket)
{
    plink_kact_t *a = k != NULL ? act_of(k, ticket) : NULL;
    if (a == NULL || a->released) {
        return;
    }
    a->released = true;
    a->overlay = act_overlay(a);
    k->dev_seq++;
}

/* ---- settings ----------------------------------------------------------------------------- */

bool plink_keypad_prefs_report(plink_keypad_t *k, uint64_t now_ms, const char *panel_id,
                               uint8_t voice, uint32_t accepts, const uint8_t *fields, size_t len)
{
    if (k == NULL) {
        return false;
    }
    bool ok = false;
    if (prefs_store(&k->prefs, panel_id, voice, accepts, fields, len, &ok)) {
        k->prefs.due = true; /* LINK-050: after a change, at most once a second */
        prefs_pump(k, now_ms);
    }
    return ok;
}

bool plink_keypad_take_prefs_set(plink_keypad_t *k, plink_prefs_change_t *out)
{
    if (k == NULL || out == NULL || k->prefs.in_state != PREFS_IN_WAITING) {
        return false;
    }
    *out = k->prefs.in;
    k->prefs.in_state = PREFS_IN_TAKEN;
    return true;
}

bool plink_keypad_prefs_answer(plink_keypad_t *k, uint64_t now_ms, uint32_t req,
                               const uint8_t *refused, uint8_t refused_count, const uint8_t *fields,
                               size_t len)
{
    if (k == NULL || req == 0u || (refused == NULL && refused_count > 0u)) {
        return false;
    }
    plink_kprefs_t *s = &k->prefs;
    bool ok = true;
    if (fields != NULL || len > 0u) {
        (void)prefs_store(s, s->panel_id, s->voice, s->accepts, fields, len, &ok);
    }
    /* Kept 30 s: a repeat of req is answered from it, never applied again (LINK-051). */
    s->ans_req = req;
    s->ans_ms = now_ms;
    s->ans_count =
        refused_count < PLINK_PREFS_REFUSED_MAX ? refused_count : (uint8_t)PLINK_PREFS_REFUSED_MAX;
    memset(s->ans_refused, 0, sizeof s->ans_refused);
    if (s->ans_count > 0u) {
        memcpy(s->ans_refused, refused, 2u * (size_t)s->ans_count);
    }
    if (s->in_state != PREFS_IN_NONE && s->in.req == req) {
        s->in_state = PREFS_IN_NONE;
        plink_wipe(&s->in, sizeof s->in);
    }
    prefs_send(k, now_ms, req, s->ans_refused, s->ans_count); /* (a repeat gets it later) */
    return ok;
}
