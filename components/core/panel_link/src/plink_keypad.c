/*
 * plink_keypad.c - a keypad's side of the panel link (see include/panel_link/plink_keypad.h;
 * docs/features/11 "Pairing", "Sessions", "Timers", "Roles on a P4 panel"; LINK-002, -006, -008,
 * -014, -015, -016, -017, -018, -023, -025, -026).
 *
 * One task owns the keypad: frames come in through plink_keypad_input, time through
 * plink_keypad_tick, and whatever has to go out goes through the port at once. The pieces:
 *   - pairing (plink_pair_k_t), only while a person pairs; the record it makes is stored and a
 *     handshake starts at once;
 *   - the handshake (plink_hs_*): at start-up, after 15 s without an authenticated frame, on a
 *     REKEY for the live session, at seq 2^30 and every 24 h. An unanswered HELLO is followed by
 *     a fresh one after 1, 2, 4 ... 30 s, jittered (at most 2 s while a panic waits). A WELCOME
 *     makes the new session tentative: the old one is still opened until a frame of the new one
 *     arrives, and HEARTBEATs go every 500 ms until then (the master promotes the new session on
 *     the first of them);
 *   - what the master says, in frame order per item (LINK-026): a STATUS's system section, each
 *     partition record, each zone record and the time are each taken only from a frame newer
 *     than the one they last came in; a new session starts the count again;
 *   - what the keypad pulls: the configuration page by page (2 requests out, each retried after
 *     300 ms doubling to 2 s), the zones when their CRC differs, the events past the newest kept;
 *   - commands, with a request id counted from a random base, resent every 500 ms for 3 s, a
 *     panic every 1 s for 60 s whatever the link's state (LINK-018, LINK-023).
 *
 * No code or key is kept longer than it is needed: a command's codes are wiped once it is
 * answered or given up, and every buffer that held them in clear is wiped after sealing.
 */
#include "panel_link/plink_keypad.h"

#include <string.h>

#include "alarm/alarm_codec.h" /* alarm_crc32 */
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
#define OWN_PAYLOAD_MAX   64u /* the keypad's own payloads: HEARTBEAT, CMD, the requests */

enum { STORE_NONE = 0, STORE_WRITE = 1, STORE_ERASE = 2 };

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

/* The configuration bytes one CFG_PAGE can carry on this transport (0: none). */
static uint16_t page_size(const plink_keypad_t *k)
{
    const size_t m = frame_cap(k);
    const size_t over = PLINK_SEALED_OVERHEAD + PLINK_CFG_PAGE_HDR;
    return m > over ? (uint16_t)(m - over) : 0u;
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
    if (plink_hs_hello(k->c, k->rec.k_pair, k->rec.net_id, k->rec.device, &k->cap, &k->hs)) {
        send_to(k, to, k->hs.hello, k->hs.hello_len);
    }
}

static void hs_tick(plink_keypad_t *k, uint64_t now)
{
    if ((k->have_disc || k->have_master) && !k->hs.waiting && k->hs_due_ms == NEVER) {
        if (now >= k->heard_ms + PLINK_REHANDSHAKE_MS) {
            hs_soon(k, now); /* 15 s without an authenticated frame */
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

/* ---- the configuration (docs/features/11 "Configuration") --------------------------------- */

static void cfg_stop(plink_keypad_t *k)
{
    k->cfg_pulling = false;
    k->cfg_ahead_off = k->cfg_ahead_len = 0u;
    memset(k->cfg_req, 0, sizeof k->cfg_req);
}

/* After a STATUS: pull the configuration it names unless it is the one installed, the one being
 * pulled, or one that failed before. */
static void cfg_check(plink_keypad_t *k, uint64_t now)
{
    const uint32_t crc = k->view.cfg_crc;
    const uint32_t len = k->view.cfg_len;
    if (len == 0u || (k->have_cfg && crc == k->cfg_installed_crc) ||
        (k->cfg_bad_crc != 0u && crc == k->cfg_bad_crc)) {
        cfg_stop(k);
        return;
    }
    if (k->cfg_pulling && crc == k->cfg_pull_crc) {
        return;
    }
    /* Another configuration: whatever was being pulled is dropped. */
    cfg_stop(k);
    if (k->cfg_buf == NULL || k->cfg == NULL || len > k->cfg_cap) {
        k->cfg_bad_crc = crc; /* more than this keypad can hold */
        return;
    }
    k->cfg_pulling = true;
    k->cfg_pull_crc = crc;
    k->cfg_total = len;
    k->cfg_have = 0u;
    /* Not all keypads at once. */
    k->cfg_next_ms = now + rnd32(k) % (CFG_DELAY_MS + 1u);
}

static void cfg_request(plink_keypad_t *k, uint64_t now, uint32_t off, uint16_t max)
{
    const plink_cfg_req_t q = {k->cfg_pull_crc, off, max};
    uint8_t p[OWN_PAYLOAD_MAX];
    size_t n = 0u;
    if (plink_enc_cfg_req(&q, p, sizeof p, &n)) {
        (void)seal_send(k, now, PLINK_T_CFG_REQ, p, n);
    }
}

/* Keeps two requests out: the first gap, and what follows it (or the page in past it). A request
 * no longer wanted is dropped; one due goes again, its wait doubled up to 2 s. */
static void cfg_pump(plink_keypad_t *k, uint64_t now)
{
    const uint16_t page = page_size(k);
    if (!k->cfg_pulling || !k->live.valid || page == 0u || now < k->cfg_next_ms) {
        return;
    }
    uint32_t off[PLINK_PULL_OUTSTANDING];
    uint16_t max[PLINK_PULL_OUTSTANDING];
    size_t n = 0u;
    const uint32_t gap_end = k->cfg_ahead_len != 0u ? k->cfg_ahead_off : k->cfg_total;
    uint32_t next = k->cfg_have;
    if (k->cfg_have < gap_end) {
        const uint32_t room = gap_end - k->cfg_have;
        off[n] = k->cfg_have;
        max[n] = room < page ? (uint16_t)room : page;
        next = k->cfg_have + max[n];
        n++;
    }
    if (k->cfg_ahead_len != 0u) {
        next = k->cfg_ahead_off + k->cfg_ahead_len;
    }
    if (n < PLINK_PULL_OUTSTANDING && next < k->cfg_total) {
        const uint32_t room = k->cfg_total - next;
        off[n] = next;
        max[n] = room < page ? (uint16_t)room : page;
        n++;
    }
    for (size_t r = 0; r < PLINK_PULL_OUTSTANDING; r++) {
        plink_kpull_t *q = &k->cfg_req[r];
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
            if (k->cfg_req[r].used && k->cfg_req[r].off == off[j]) {
                q = &k->cfg_req[r];
            }
        }
        for (size_t r = 0; r < PLINK_PULL_OUTSTANDING && q == NULL; r++) {
            if (!k->cfg_req[r].used) {
                q = &k->cfg_req[r];
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
        cfg_request(k, now, q->off, q->max);
        q->due_ms = now + q->retry_ms;
        q->retry_ms =
            q->retry_ms * 2u < PLINK_PULL_RETRY_MAX_MS ? q->retry_ms * 2u : PLINK_PULL_RETRY_MAX_MS;
    }
}

/* The whole blob is in: its CRC checked, then decoded leniently into the caller's configuration
 * (LINK-017, LINK-019). One that fails is remembered and not pulled again; the keypad says it
 * needs an update (plink_keypad_info cfg_bad_crc). */
static void cfg_finish(plink_keypad_t *k)
{
    const uint32_t crc = k->cfg_pull_crc;
    const uint32_t total = k->cfg_total;
    cfg_stop(k);
    if (alarm_crc32(0u, k->cfg_buf, total) != crc) {
        k->cfg_bad_crc = crc;
        return;
    }
    if (plink_cfg_decode(k->cfg_buf, total, k->cfg) != ALARM_OK) {
        /* The decode may have started on the copy: it holds nothing usable now. What the
         * platform took at the last config_seq stays on the screen. */
        k->cfg_bad_crc = crc;
        k->have_cfg = false;
        k->cfg_installed_crc = 0u;
        return;
    }
    k->have_cfg = true;
    k->cfg_installed_crc = crc;
    k->config_seq++;
    k->view.snap.config_seq = k->config_seq;
}

static void on_cfg_page(plink_keypad_t *k, uint64_t now, const uint8_t *p, size_t n)
{
    plink_cfg_page_t pg;
    if (!plink_dec_cfg_page(p, n, &pg) || !k->cfg_pulling || pg.cfg_crc != k->cfg_pull_crc ||
        pg.total != k->cfg_total || pg.len == 0u) {
        return; /* another configuration's page: the next STATUS says which one is current */
    }
    const uint32_t off = pg.offset;
    const uint32_t end = pg.offset + pg.len; /* <= total <= cfg_cap (the decoder checked) */
    const uint32_t ahead_end = k->cfg_ahead_off + k->cfg_ahead_len;
    if (off <= k->cfg_have) {
        if (end > k->cfg_have) {
            memcpy(&k->cfg_buf[k->cfg_have], &pg.data[k->cfg_have - off], end - k->cfg_have);
            k->cfg_have = end;
        }
    } else if (k->cfg_ahead_len == 0u) {
        memcpy(&k->cfg_buf[off], pg.data, pg.len);
        k->cfg_ahead_off = off;
        k->cfg_ahead_len = pg.len;
    } else if (off <= ahead_end && end > ahead_end) {
        memcpy(&k->cfg_buf[ahead_end], &pg.data[ahead_end - off], end - ahead_end);
        k->cfg_ahead_len = end - k->cfg_ahead_off;
    } else if (off < k->cfg_ahead_off && end >= k->cfg_ahead_off) {
        memcpy(&k->cfg_buf[off], pg.data, k->cfg_ahead_off - off);
        k->cfg_ahead_len += k->cfg_ahead_off - off;
        k->cfg_ahead_off = off;
    } /* anything else is asked for again */
    if (k->cfg_ahead_len != 0u && k->cfg_ahead_off <= k->cfg_have) {
        const uint32_t a_end = k->cfg_ahead_off + k->cfg_ahead_len;
        if (a_end > k->cfg_have) {
            k->cfg_have = a_end;
        }
        k->cfg_ahead_off = k->cfg_ahead_len = 0u;
    }
    if (k->cfg_have >= k->cfg_total) {
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
        if (!k->ev_want) {
            k->ev_want = true;
            k->ev_retry_ms = PLINK_PULL_RETRY_MS;
            k->ev_next_ms = now;
        }
    } else {
        k->ev_want = false;
    }
}

static void ev_pump(plink_keypad_t *k, uint64_t now)
{
    if (!k->ev_want || !k->live.valid || now < k->ev_next_ms) {
        return;
    }
    const plink_ev_req_t q = {ev_newest(k), (uint8_t)PLINK_EVENTS_KEPT};
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

static void on_events(plink_keypad_t *k, uint32_t fs, const uint8_t *p, size_t n)
{
    size_t got = 0u;
    uint8_t flags = 0u;
    if (!plink_dec_events(p, n, k->scr.ev, PLINK_EVENTS_KEPT, &got, &flags)) {
        return;
    }
    const uint32_t before = ev_newest(k);
    for (size_t i = 0; i < got; i++) {
        (void)ev_insert(k, &k->scr.ev[i]);
    }
    if (ev_newest(k) != before) {
        k->ev_frame_seq = fs;
    }
    if (ev_newest(k) >= k->view.snap.last_event_seq) {
        k->ev_want = false;
    }
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
    cfg_stop(k);
    k->have_cfg = false;
    k->cfg_installed_crc = 0u;
    k->cfg_bad_crc = 0u;
    k->zones_want = false;
    k->zones_next_ms = 0u;
    k->zones_gap_ms = ZONES_GAP_MS;
    items_reset(k, 0u);
    memset(k->sounds, 0, sizeof k->sounds);
    k->sound_head = k->sound_count = 0u;
    memset(&k->time, 0, sizeof k->time);
    k->time_changed = false;
    k->status_ms = 0u;
    k->heard_ms = now;
    k->silent_since_ms = now;
    k->not_paired_since_ms = k->not_paired_last_ms = 0u;
    k->hs_due_ms = NEVER;
    k->hs_backoff_ms = HS_FIRST_MS;
    k->last_rekey_ms = NEVER;
    k->hb_due_ms = NEVER;
    k->session_started_ms = 0u;
}

/* Not paired any more: removed at the master (a BYE) or forgotten here. */
static bool drop_master(plink_keypad_t *k, uint64_t now, uint8_t why)
{
    forget_data(k, now);
    plink_wipe(&k->rec, sizeof k->rec);
    k->paired = false;
    k->unpaired_why = why;
    k->have_master = k->have_disc = false;
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
    plink_session_t s;
    if (!k->paired || !k->hs.waiting ||
        !plink_hs_finish(k->c, k->rec.k_pair, &k->hs, h, frame, len, &s)) {
        return; /* only the answer to the latest HELLO */
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
    k->heard_ms = now;
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
    k->heard_ms = now;
    k->not_paired_since_ms = k->not_paired_last_ms = 0u;
    if (s == &k->live && k->tentative) {
        /* The master has the new session: the old one, and its frames, are done. */
        k->tentative = false;
        plink_session_wipe(&k->old);
        hb_schedule(k, now);
    }
    if (s->sid != k->items_sid) {
        items_reset(k, s->sid);
    }
    const uint32_t fs = h->seq;
    switch (h->type) {
    case PLINK_T_STATUS: on_status(k, now, fs, k->pt, n); break;
    case PLINK_T_ZONES: on_zones(k, now, fs, k->pt, n); break;
    case PLINK_T_CFG_PAGE: on_cfg_page(k, now, k->pt, n); break;
    case PLINK_T_EVENTS: on_events(k, fs, k->pt, n); break;
    case PLINK_T_RESULT: on_result(k, k->pt, n); break;
    case PLINK_T_SOUND: on_sound(k, k->pt, n); break;
    case PLINK_T_TIME: on_time(k, now, fs, k->pt, n); break;
    case PLINK_T_BYE: (void)drop_master(k, now, PLINK_UNPAIRED_REMOVED); break; /* LINK-014 */
    default: break;
    }
    plink_wipe(k->pt, n);
}

/* ---- the API ------------------------------------------------------------------------------ */

void plink_keypad_init(plink_keypad_t *k, const plink_crypto_t *c, const plink_keypad_port_t *port,
                       const plink_keypad_rec_t *rec, uint8_t *cfg_buf, size_t cfg_cap,
                       alarm_config_t *cfg, uint8_t kind, const plink_caps_t *cap, const char *name,
                       const char *fw, uint64_t now_ms)
{
    if (k == NULL) {
        return;
    }
    plink_wipe(k, sizeof *k);
    k->c = c;
    if (port != NULL) {
        k->port = *port;
    }
    k->cfg_buf = cfg_buf;
    k->cfg_cap = cfg_buf != NULL ? cfg_cap : 0u;
    k->cfg = cfg;
    k->kind = kind;
    if (cap != NULL) {
        k->cap = *cap;
    }
    plink_clean_text(name, bounded_len(name, PLINK_NAME_MAX), k->name, sizeof k->name);
    plink_clean_text(fw, bounded_len(fw, PLINK_FW_MAX), k->fw, sizeof k->fw);
    plink_pk_init(&k->pair, c, kind, cap, name, fw);
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

void plink_keypad_master_addr(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *addr)
{
    if (k == NULL || addr == NULL) {
        return;
    }
    const bool changed = !k->have_disc || !plink_addr_equal(&k->disc, addr);
    k->disc = *addr;
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
        t = earliest(t, k->heard_ms + PLINK_REHANDSHAKE_MS);
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
        if (k->cfg_pulling && page_size(k) != 0u) {
            uint64_t c = NEVER;
            for (size_t r = 0; r < PLINK_PULL_OUTSTANDING; r++) {
                if (k->cfg_req[r].used) {
                    c = earliest(c, k->cfg_req[r].due_ms);
                }
            }
            t = earliest(t, c != NEVER ? c : k->cfg_next_ms);
        }
        if (k->zones_want) {
            t = earliest(t, k->zones_next_ms);
        }
        if (k->ev_want) {
            t = earliest(t, k->ev_next_ms);
        }
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
                             uint16_t net_id)
{
    if (k == NULL || master == NULL) {
        return;
    }
    k->pairing = true;
    plink_pk_start(&k->pair, now_ms, master, net_id, &k->out);
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
     * of connecting when none came). */
    out->maintenance = k->paired && k->state != PLINK_LINK_ONLINE &&
                       now_ms >= k->silent_since_ms + PLINK_MAINTENANCE_MS;
    if (k->paired) {
        out->device = k->rec.device;
        out->home = k->rec.home;
        memcpy(out->master_name, k->rec.master_name, sizeof out->master_name);
    }
    if (k->have_master) {
        out->master = k->master;
    }
    out->cfg_bad_crc = k->cfg_bad_crc;
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
        /* The same panic keyed again while it is still being sent is the same request. */
        for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
            const plink_kcmd_t *c = &k->cmds[i];
            if (c->used && c->panic && c->state == PLINK_CMD_PENDING &&
                c->cmd.cmd.partition == cmd->cmd.partition && c->cmd.cmd.panic == cmd->cmd.panic) {
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
