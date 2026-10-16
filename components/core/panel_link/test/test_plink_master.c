/*
 * test_plink_master.c - the master's link manager (components/core/panel_link plink_master.h,
 * docs/features/11), driven over the pretend network (plink_test_net.c) by scripted keypads built
 * from the real pairing, handshake, session and message codecs, with the real engine (alarm_sim)
 * behind it as the platform would wire it: port.run runs the command on the engine and answers
 * at once from inside port.run, or later; every engine call is followed by its events, its
 * sounds and a new snapshot.
 *
 * Proves: pairing end to end and the device number it takes; the HELLO checks (mac1, replays,
 * the same HELLO again while pending, the X25519 budget); pending and live sessions; the REKEY
 * and NOT_PAIRED hints; STATUS on change, repeated, coalesced and every 10 s; zones, the
 * configuration's pages and events (before the STATUS that carries them); sounds by partition;
 * commands (the home partition, the result cache, one at a time with panic apart, the rate);
 * supervision both ways, paused while the network is down, and a provisional keypad dropped;
 * removal; the clone note; the address moving only on authenticated frames; TIME; and, after
 * every test, no AES-CCM key and nonce used twice on either side.
 *
 * Requirements: LINK-001, LINK-003, LINK-004, LINK-005, LINK-006, LINK-007, LINK-014, LINK-015,
 * LINK-016, LINK-017, LINK-018, LINK-021, LINK-022, LINK-023, LINK-027.
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "panel_link/plink_keypad.h" /* the keypad's timers, for the scripted keypad */
#include "panel_link/plink_master.h"
#include "plink_test_net.h"
#include "plink_test_port.h"
#include "sim.h"
#include "unity.h"

#define TYPES      0x30u
#define K_LOG      2048u
#define K_EVS      512u
#define K_RESULTS  64u
#define KP_MAX     3u
#define HB_MS      10000u /* a keypad's heartbeat (LINK-006) */

enum { RUN_NOW, RUN_LATER, RUN_REFUSE };

static const uint8_t IP_M[4] = {192, 0, 2, 1};
static const uint8_t IP_ALT[4] = {192, 0, 2, 21};
static const uint8_t IP_X[4] = {198, 51, 100, 66};
static const plink_caps_t CAP = {ALARM_MAX_PARTITIONS, ALARM_MAX_ZONES, 32u, 16u};

/* ---- a scripted keypad ----------------------------------------------------------------------- */

typedef struct {
    uint8_t type;
    uint64_t at;
    uint32_t sid;
    uint32_t a; /* STATUS: last_event_seq; EVENTS: the newest seq; RESULT: req; REKEY: sid;
                 * ZONES: first | count << 16; NOT_PAIRED: the header's net id */
} k_log_t;

typedef struct {
    int node, alt, via;
    plink_test_port_t tp;
    plink_crypto_t c;
    plink_pair_k_t pk;
    bool paired;
    plink_keypad_rec_t rec;
    plink_hs_k_t hs;
    plink_session_t live, old; /* old: still accepted while the new one is tentative */
    bool auto_hello;           /* a HELLO once paired */
    bool hb_on_welcome;        /* a HEARTBEAT as soon as a WELCOME verifies */
    bool auto_hb;              /* a HEARTBEAT every 10 s */
    bool auto_rekey;           /* a HELLO on a REKEY for its live sid */
    uint64_t hb_due;
    uint64_t status_at;        /* the last STATUS (0 none) */
    k_log_t log[K_LOG];
    size_t n_log;
    unsigned count[TYPES];
    size_t last_len[TYPES];
    uint8_t last[TYPES][PLINK_FRAME_MAX];
    unsigned open_fail, welcome_fail;
    plink_status_t status;
    uint32_t ev_seq[K_EVS];
    size_t n_ev;
    uint8_t ev_flags;          /* of the last EVENTS */
    uint32_t ev_max;           /* the newest event received */
    bool ev_based;             /* this session's first STATUS came */
    bool ev_order_bad;         /* a STATUS carried a seq whose EVENTS hadn't come */
    plink_result_t results[K_RESULTS];
    size_t n_results;
    uint8_t st_first[256];     /* each STATUS frame's first partition record (0 none) */
    size_t n_st;
} fk_t;

/* ---- the rig: the master, its port, the engine and the network ------------------------------- */

typedef struct {
    uint32_t ticket;
    uint8_t device;
    plink_cmd_t cmd;
    bool answered;
} run_t;

typedef struct {
    plink_net_t net;
    sim_t sim;
    plink_test_port_t tpm;
    plink_crypto_t cm;
    plink_master_port_t port;
    plink_master_t m;
    int mnode, xnode, alt_node;
    fk_t kp[KP_MAX];
    unsigned nkp;
    unsigned x_count[TYPES];
    /* the master's port */
    plink_master_rec_t stored;
    unsigned n_store;
    bool store_fail;
    uint32_t taken;
    uint32_t held;
    unsigned n_held, held_order;
    struct {
        uint8_t device;
        bool up;
        uint64_t at;
        unsigned order;
    } reach[64];
    unsigned n_reach;
    unsigned order;
    unsigned checkins[PLINK_DEVICE_LAST + 1u];
    struct {
        plink_note_t note;
        uint8_t device;
    } notes[64];
    unsigned n_notes;
    int run_mode;
    bool hold;                 /* RUN_LATER: the test answers itself (answer()) */
    run_t runs[128];
    unsigned n_runs;
    bool in_run;
    unsigned sent_in_run;      /* frames sent from inside port.run: must stay 0 */
    size_t mtu;                /* 0: PLINK_FRAME_MAX */
    /* the platform's glue */
    bool snap_first;           /* hand the snapshot before the events (the other order) */
    uint32_t snap_seq;
    unsigned ev_fwd, snd_fwd;
    uint8_t report;
    alarm_snapshot_t snap;
    alarm_config_t cfg_scratch;
    uint8_t cfg_blob[ALARM_CONFIG_BLOB_MAX];
    size_t cfg_len;
    uint32_t cfg_crc;
} rig_t;

static rig_t R;
static plink_out_t KOUT;
static uint8_t KBUF[PLINK_FRAME_MAX], KPT[PLINK_FRAME_MAX], KIN[PLINK_FRAME_MAX];

#define NOW (R.net.now_ms)

static const plink_addr_t *maddr(void)
{
    return plink_net_addr(&R.net, R.mnode);
}

/* ---- the engine and the platform's glue ------------------------------------------------------ */

static void sync_engine(uint64_t now)
{
    if (now > R.sim.now) {
        R.sim.now = now;
    }
    alarm_engine_tick(&R.sim.eng, R.sim.now);
}

/* What the platform does after every engine call: the events and sounds it made, then a new
 * snapshot (its seq rises every time, change or not). */
static void pump(uint64_t now)
{
    if (!R.snap_first) {
        while (R.ev_fwd < R.sim.n_events) {
            plink_master_event(&R.m, now, &R.sim.events[R.ev_fwd++]);
        }
    }
    while (R.snd_fwd < R.sim.n_sounds) {
        const sim_sound_t *s = &R.sim.sounds[R.snd_fwd++];
        plink_master_sound(&R.m, now, s->partition, (uint8_t)s->sound, s->zone);
    }
    alarm_engine_snapshot(&R.sim.eng, &R.snap);
    R.snap.seq = ++R.snap_seq;
    plink_master_snapshot(&R.m, now, &R.snap, (int64_t)(1790000000000ull + now), R.report);
    if (R.snap_first) {
        while (R.ev_fwd < R.sim.n_events) {
            plink_master_event(&R.m, now, &R.sim.events[R.ev_fwd++]);
        }
    }
}

/* The engine at the network's clock, before a test drives it directly. */
static void at_now(void)
{
    sync_engine(NOW);
}

static plink_result_t exec(uint8_t device, const plink_cmd_t *c)
{
    plink_result_t r;
    memset(&r, 0, sizeof r);
    r.req = c->req;
    const alarm_source_t src = {ALARM_SRC_KEYPAD, device};
    sync_engine(NOW);
    if (c->op == PLINK_OP_COMMAND) {
        alarm_cmd_result_t cr;
        memset(&cr, 0, sizeof cr);
        r.result = (uint8_t)alarm_engine_command(&R.sim.eng, &c->cmd, &src, R.sim.now, &cr);
        r.user = cr.user;
        r.not_ready_zone = cr.not_ready_zone;
        r.may_disarm = cr.may_disarm ? 1u : 0u;
    } else if (c->op == PLINK_OP_AUTH) {
        uint8_t user = 0;
        r.result = (uint8_t)alarm_engine_authenticate(&R.sim.eng, &src, c->cmd.code,
                                                      c->cmd.partition, R.sim.now, &user);
        r.user = user;
    } else {
        r.result = (uint8_t)ALARM_ERR_UNAVAILABLE;
    }
    return r;
}

/* Run i answered now (outside port.run). */
static void answer(unsigned i)
{
    TEST_ASSERT_TRUE(i < R.n_runs);
    TEST_ASSERT_FALSE(R.runs[i].answered);
    R.runs[i].answered = true;
    const plink_result_t r = exec(R.runs[i].device, &R.runs[i].cmd);
    plink_master_result(&R.m, NOW, R.runs[i].ticket, &r);
}

static bool p_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (R.in_run) {
        R.sent_in_run++;
    }
    return plink_net_send(&R.net, R.mnode, to, frame, len);
}

static size_t p_mtu(void *ctx, uint8_t transport)
{
    (void)ctx;
    (void)transport;
    return R.mtu;
}

static bool p_store(void *ctx, const plink_master_rec_t *rec)
{
    (void)ctx;
    if (R.store_fail) {
        return false;
    }
    R.stored = *rec;
    R.n_store++;
    return true;
}

static bool p_run(void *ctx, uint32_t ticket, uint8_t device, const plink_cmd_t *cmd)
{
    (void)ctx;
    TEST_ASSERT_FALSE_MESSAGE(R.in_run, "port.run called from inside port.run");
    TEST_ASSERT_TRUE(R.n_runs < sizeof R.runs / sizeof R.runs[0]);
    run_t *r = &R.runs[R.n_runs++];
    r->ticket = ticket;
    r->device = device;
    r->cmd = *cmd;
    r->answered = false;
    if (R.run_mode == RUN_REFUSE) {
        r->answered = true;
        return false;
    }
    if (R.run_mode == RUN_NOW) {
        /* The answer from inside port.run: the master must hold it until run is back. */
        R.in_run = true;
        r->answered = true;
        const plink_result_t res = exec(device, cmd);
        plink_master_result(&R.m, NOW, ticket, &res);
        R.in_run = false;
    }
    return true;
}

static void p_reachable(void *ctx, uint8_t device, bool up)
{
    (void)ctx;
    TEST_ASSERT_TRUE(R.n_reach < sizeof R.reach / sizeof R.reach[0]);
    R.reach[R.n_reach].device = device;
    R.reach[R.n_reach].up = up;
    R.reach[R.n_reach].at = NOW;
    R.reach[R.n_reach].order = ++R.order;
    R.n_reach++;
}

static void p_checkin(void *ctx, uint8_t device)
{
    (void)ctx;
    if (device <= PLINK_DEVICE_LAST) {
        R.checkins[device]++;
    }
}

static uint32_t p_taken(void *ctx)
{
    (void)ctx;
    return R.taken;
}

static void p_held(void *ctx, uint32_t held)
{
    (void)ctx;
    R.held = held;
    R.n_held++;
    R.held_order = ++R.order;
}

static uint32_t p_lockout(void *ctx, uint8_t device, uint64_t now)
{
    (void)ctx;
    const alarm_source_t src = {ALARM_SRC_KEYPAD, device};
    return (uint32_t)alarm_engine_lockout_left(&R.sim.eng, &src, now);
}

/* The newest events after after_seq, oldest first, at most max (the engine's own record). */
static size_t p_events_after(void *ctx, uint32_t after, alarm_event_t *out, size_t max, bool *gap)
{
    (void)ctx;
    size_t first = 0;
    while (first < R.sim.n_events && R.sim.events[first].seq <= after) {
        first++;
    }
    const size_t avail = R.sim.n_events - first;
    const size_t k = avail < max ? avail : max;
    *gap = k < avail;
    for (size_t i = 0; i < k; i++) {
        out[i] = R.sim.events[R.sim.n_events - k + i];
    }
    return k;
}

static void p_note(void *ctx, plink_note_t n, uint8_t device)
{
    (void)ctx;
    TEST_ASSERT_TRUE(R.n_notes < sizeof R.notes / sizeof R.notes[0]);
    R.notes[R.n_notes].note = n;
    R.notes[R.n_notes].device = device;
    R.n_notes++;
}

static unsigned notes(plink_note_t n, uint8_t device)
{
    unsigned c = 0;
    for (unsigned i = 0; i < R.n_notes; i++) {
        c += R.notes[i].note == n && R.notes[i].device == device;
    }
    return c;
}

static unsigned edges(uint8_t device, bool up)
{
    unsigned c = 0;
    for (unsigned i = 0; i < R.n_reach; i++) {
        c += R.reach[i].device == device && R.reach[i].up == up;
    }
    return c;
}

/* ---- the master as a network node ------------------------------------------------------------ */

static void m_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t len)
{
    (void)ctx;
    plink_master_input(&R.m, now, from, f, len);
}

static void m_tick(void *ctx, uint64_t now)
{
    (void)ctx;
    sync_engine(now);
    pump(now);
    plink_master_tick(&R.m, now);
}

static uint64_t m_next(void *ctx)
{
    (void)ctx;
    const uint64_t t = plink_master_next(&R.m);
    const uint64_t e = alarm_engine_next_deadline(&R.sim.eng);
    return e < t ? e : t;
}

static void x_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t len)
{
    (void)ctx;
    (void)now;
    (void)from;
    if (len > 3u && f[3] < TYPES) {
        R.x_count[f[3]]++;
    }
}

/* ---- the scripted keypad's behaviour --------------------------------------------------------- */

static void k_send(fk_t *k, const uint8_t *f, size_t len)
{
    (void)plink_net_send(&R.net, k->via, maddr(), f, len);
}

static void k_send_out(fk_t *k)
{
    if (KOUT.len > 0u) {
        (void)plink_net_send(&R.net, k->via, &KOUT.to, KOUT.buf, KOUT.len);
        KOUT.len = 0u;
    }
}

/* A payload sealed under s from this keypad and sent to the master. */
static bool k_seal_with(fk_t *k, plink_session_t *s, uint8_t type, const uint8_t *p, size_t n)
{
    size_t len = 0;
    if (!plink_session_seal(&k->c, s, type, k->rec.net_id, k->rec.device, false, p, n, KBUF,
                            sizeof KBUF, &len)) {
        return false;
    }
    k_send(k, KBUF, len);
    return true;
}

static bool k_seal(fk_t *k, uint8_t type, const uint8_t *p, size_t n)
{
    return k_seal_with(k, &k->live, type, p, n);
}

static void k_hello(fk_t *k)
{
    TEST_ASSERT_TRUE(plink_hs_hello(&k->c, k->rec.k_pair, k->rec.net_id, k->rec.device, &CAP,
                                    &k->hs));
    k_send(k, k->hs.hello, k->hs.hello_len);
}

/* What its heartbeats report: the age of its last STATUS, 0xFFFF before the first. */
static uint16_t k_age(const fk_t *k)
{
    if (k->status_at == 0u) {
        return PLINK_STATUS_AGE_NONE;
    }
    const uint64_t s = (NOW - k->status_at) / 1000u;
    return s < 0xFFFEu ? (uint16_t)s : 0xFFFEu;
}

static void k_heartbeat_with(fk_t *k, plink_session_t *s)
{
    plink_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    hb.rssi = -55;
    hb.uptime_s = (uint32_t)(NOW / 1000u);
    hb.status_age_s = k_age(k);
    strcpy(hb.fw, "1.0.0-test");
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_heartbeat(&hb, KPT, sizeof KPT, &n));
    (void)k_seal_with(k, s, PLINK_T_HEARTBEAT, KPT, n);
    k->hb_due = NOW + HB_MS;
}

static void k_heartbeat(fk_t *k)
{
    k_heartbeat_with(k, &k->live);
}

static void k_take(fk_t *k)
{
    plink_keypad_rec_t rec;
    if (plink_pk_take(&k->pk, &rec)) {
        k->rec = rec;
        k->paired = true;
        if (k->auto_hello) {
            k_hello(k);
        }
    }
}

static void k_record(fk_t *k, const plink_hdr_t *h, const uint8_t *p, size_t n, uint32_t a)
{
    if (k->n_log < K_LOG) {
        k_log_t *l = &k->log[k->n_log++];
        l->type = h->type;
        l->at = NOW;
        l->sid = h->sid;
        l->a = a;
    }
    if (h->type < TYPES) {
        k->count[h->type]++;
        memcpy(k->last[h->type], p, n);
        k->last_len[h->type] = n;
    }
}

static void k_welcome(fk_t *k, const plink_hdr_t *h, const uint8_t *f, size_t len)
{
    plink_session_t s;
    if (!k->hs.waiting || !plink_hs_finish(&k->c, k->rec.k_pair, &k->hs, h, f, len, &s)) {
        k->welcome_fail++;
        return;
    }
    plink_session_wipe(&k->old);
    if (k->live.valid) {
        k->old = k->live;
    }
    k->live = s;
    plink_session_wipe(&s);
    k->ev_based = false;
    if (k->hb_on_welcome) {
        k_heartbeat(k);
    }
}

/* A sealed frame from the master, opened and recorded; STATUS, EVENTS, RESULT decoded. */
static void k_sealed(fk_t *k, uint64_t now, const plink_hdr_t *h, const uint8_t *f, size_t len)
{
    plink_session_t *s = NULL;
    if (k->live.valid && h->sid == k->live.sid) {
        s = &k->live;
    } else if (k->old.valid && h->sid == k->old.sid) {
        s = &k->old;
    }
    size_t n = 0;
    if (s == NULL || !plink_session_open(&k->c, s, h, f, len, KIN, sizeof KIN, &n)) {
        k->open_fail++;
        return;
    }
    if (s == &k->live && k->old.valid) {
        plink_session_wipe(&k->old); /* the new session proved itself */
    }
    uint32_t a = 0;
    if (h->type == PLINK_T_STATUS) {
        TEST_ASSERT_TRUE(plink_dec_status(KIN, n, &k->status));
        a = k->status.last_event_seq;
        k->status_at = now;
        if (k->n_st < sizeof k->st_first) {
            k->st_first[k->n_st++] = k->status.count > 0u ? k->status.part[0].number : 0u;
        }
        /* Every event a STATUS carries came before it (once this session's first came). */
        if (!k->ev_based) {
            k->ev_based = true;
            if (k->ev_max < a) {
                k->ev_max = a;
            }
        } else if (a > k->ev_max) {
            k->ev_order_bad = true;
        }
    } else if (h->type == PLINK_T_EVENTS) {
        alarm_event_t ev[64];
        size_t cnt = 0;
        TEST_ASSERT_TRUE(plink_dec_events(KIN, n, ev, 64u, &cnt, &k->ev_flags));
        for (size_t i = 0; i < cnt && k->n_ev < K_EVS; i++) {
            k->ev_seq[k->n_ev++] = ev[i].seq;
            if (ev[i].seq > k->ev_max) {
                k->ev_max = ev[i].seq;
            }
            a = ev[i].seq;
        }
    } else if (h->type == PLINK_T_RESULT) {
        TEST_ASSERT_TRUE(k->n_results < K_RESULTS);
        TEST_ASSERT_TRUE(plink_dec_result(KIN, n, &k->results[k->n_results]));
        a = k->results[k->n_results++].req;
    } else if (h->type == PLINK_T_ZONES) {
        plink_zones_t z;
        TEST_ASSERT_TRUE(plink_dec_zones(KIN, n, &z));
        a = (uint32_t)z.first | ((uint32_t)z.count << 16);
    }
    k_record(k, h, KIN, n, a);
}

static void k_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t len)
{
    fk_t *k = ctx;
    plink_hdr_t h;
    if (!plink_hdr_read(f, len, &h)) {
        return;
    }
    if (plink_type_sealed(h.type)) {
        k_sealed(k, now, &h, f, len);
        return;
    }
    const uint8_t *p = &f[PLINK_HDR_LEN];
    const size_t n = len - PLINK_HDR_LEN;
    uint32_t a = 0;
    if (h.type == PLINK_T_REKEY) {
        plink_rekey_t r;
        TEST_ASSERT_TRUE(plink_dec_rekey(p, n, &r));
        a = r.sid;
    } else if (h.type == PLINK_T_NOT_PAIRED) {
        a = h.net_id;
    }
    k_record(k, &h, p, n, a);
    if (h.type == PLINK_T_WELCOME) {
        k_welcome(k, &h, f, len);
    } else if (h.type == PLINK_T_REKEY) {
        if (k->auto_rekey && k->live.valid && a == k->live.sid) {
            k_hello(k);
        }
    } else if (h.type <= PLINK_T_PAIR_FAIL) {
        plink_pk_input(&k->pk, now, from, &h, f, len, &KOUT);
        k_send_out(k);
        k_take(k);
    }
}

static void k_tick(void *ctx, uint64_t now)
{
    fk_t *k = ctx;
    plink_pk_tick(&k->pk, now, &KOUT);
    k_send_out(k);
    if (k->auto_hb && k->live.valid && now >= k->hb_due) {
        k_heartbeat(k);
    }
}

static uint64_t k_next(void *ctx)
{
    const fk_t *k = ctx;
    uint64_t t = plink_pk_next(&k->pk);
    if (k->auto_hb && k->live.valid && k->hb_due < t) {
        t = k->hb_due;
    }
    return t;
}

static fk_t *kp_add(void)
{
    TEST_ASSERT_TRUE(R.nkp < KP_MAX);
    fk_t *k = &R.kp[R.nkp];
    const uint8_t ip[4] = {192, 0, 2, (uint8_t)(11u + R.nkp)};
    char name[24];
    strcpy(name, "Hall keypad 1");
    name[12] = (char)('1' + R.nkp);
    R.nkp++;
    plink_test_port_init(&k->tp, 0x4B000000u + R.nkp);
    k->c = plink_test_port(&k->tp);
    plink_pk_init(&k->pk, &k->c, PLINK_KIND_PANEL, &CAP, name, "1.0.0");
    k->node = plink_net_node(&R.net, ip, k, k_input, k_tick, k_next);
    k->via = k->node;
    k->alt = -1;
    k->auto_hello = true;
    k->hb_on_welcome = true;
    k->auto_hb = true;
    return k;
}

/* A second address for keypad k (the same keys: a keypad that moved, or a clone). */
static void kp_alt(fk_t *k)
{
    k->alt = plink_net_node(&R.net, IP_ALT, k, k_input, k_tick, k_next);
}

static plink_peer_t *mpeer(const fk_t *k)
{
    TEST_ASSERT_TRUE(k->rec.device >= 1u && k->rec.device <= PLINK_DEVICE_LAST);
    return &R.m.peer[k->rec.device - 1u];
}

static void k_cmd(fk_t *k, uint32_t req, uint8_t op, alarm_cmd_type_t type, uint8_t partition,
                  const char *code)
{
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.req = req;
    c.op = op;
    c.cmd.type = type;
    c.cmd.partition = partition;
    c.cmd.level = ALARM_LEVEL_AWAY;
    c.cmd.panic = ALARM_PANIC_MEDICAL;
    strcpy(c.cmd.code, code);
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_cmd(&c, KPT, sizeof KPT, &n));
    TEST_ASSERT_TRUE(k_seal(k, PLINK_T_CMD, KPT, n));
}

static void k_request(fk_t *k, uint8_t type, const uint8_t *p, size_t n)
{
    TEST_ASSERT_TRUE(k_seal(k, type, p, n));
}

static void k_cfg_req(fk_t *k, uint32_t crc, uint32_t offset, uint16_t max)
{
    const plink_cfg_req_t r = {crc, offset, max};
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_cfg_req(&r, KPT, sizeof KPT, &n));
    k_request(k, PLINK_T_CFG_REQ, KPT, n);
}

static void k_ev_req(fk_t *k, uint32_t after, uint8_t max)
{
    const plink_ev_req_t r = {after, max};
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_ev_req(&r, KPT, sizeof KPT, &n));
    k_request(k, PLINK_T_EV_REQ, KPT, n);
}

static void k_zones_req(fk_t *k, uint16_t first, uint16_t count)
{
    const plink_zones_req_t r = {first, count};
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_zones_req(&r, KPT, sizeof KPT, &n));
    k_request(k, PLINK_T_ZONES_REQ, KPT, n);
}

static const plink_result_t *k_result(const fk_t *k, uint32_t req)
{
    for (size_t i = k->n_results; i-- > 0;) {
        if (k->results[i].req == req) {
            return &k->results[i];
        }
    }
    return NULL;
}

static unsigned k_results(const fk_t *k, uint32_t req)
{
    unsigned c = 0;
    for (size_t i = 0; i < k->n_results; i++) {
        c += k->results[i].req == req;
    }
    return c;
}

/* ---- running --------------------------------------------------------------------------------- */

static void run(uint64_t ms)
{
    plink_net_run(&R.net, ms);
}

static bool c_compare(void *ctx)
{
    const fk_t *k = ctx;
    plink_pair_info_t a, b;
    plink_pk_info(&k->pk, NOW, &a);
    plink_master_pair_info(&R.m, NOW, &b);
    return a.state == PLINK_PS_COMPARE && b.state == PLINK_PS_COMPARE;
}

static bool c_paired(void *ctx)
{
    return ((const fk_t *)ctx)->paired;
}

/* The keypad's newest session is the master's live one. */
static bool c_live(void *ctx)
{
    const fk_t *k = ctx;
    const plink_peer_t *p = &R.m.peer[k->rec.device - 1u];
    return k->paired && k->live.valid && p->used && p->live.valid && p->live.sid == k->live.sid;
}

static unsigned m_sent(uint8_t type)
{
    return (unsigned)plink_net_count(&R.net, R.mnode, type);
}

/* Frames of a type the master sent to node `to`. */
static unsigned m_sent_to(int to, uint8_t type)
{
    unsigned c = 0;
    for (size_t i = 0; i < R.net.log_count; i++) {
        const plink_net_seen_t *s = &R.net.log[i];
        c += s->from == R.mnode && s->to == to && s->type == type;
    }
    return c;
}

/* Opens pairing at the master and has keypad k start: both reach the codes. */
static void pair_start(fk_t *k)
{
    TEST_ASSERT_TRUE(plink_master_pair_open(&R.m, NOW));
    plink_pk_start(&k->pk, NOW, maddr(), plink_master_net_id(&R.m), &KOUT);
    k_send_out(k);
    TEST_ASSERT_TRUE(plink_net_run_until(&R.net, 5000, c_compare, k));
}

/* Pairs keypad k end to end; with auto_hello its first session is live afterwards. */
static void pair(fk_t *k)
{
    pair_start(k);
    plink_pair_info_t a, b;
    plink_pk_info(&k->pk, NOW, &a);
    plink_master_pair_info(&R.m, NOW, &b);
    /* LINK-004: both screens show the same 6-digit code */
    TEST_ASSERT_EQUAL_UINT(6u, strlen(a.code));
    TEST_ASSERT_EQUAL_STRING(a.code, b.code);
    plink_pk_confirm(&k->pk, NOW, true, &KOUT);
    k_send_out(k);
    plink_master_pair_decide(&R.m, NOW, true);
    TEST_ASSERT_TRUE(plink_net_run_until(&R.net, 10000, c_paired, k));
    if (k->auto_hello) {
        TEST_ASSERT_TRUE(plink_net_run_until(&R.net, 5000, c_live, k));
    }
}

static bool mem_has(const void *hay, size_t hay_len, const void *needle, size_t n)
{
    const uint8_t *h = hay;
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(&h[i], needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- set-up ---------------------------------------------------------------------------------- */

void setUp(void)
{
    memset(&R, 0, sizeof R);
    plink_net_init(&R.net, 0xC0FFEEu);
    sim_start(&R.sim, NULL, NULL);
    R.net.now_ms = R.sim.now; /* one clock: the engine's */
    plink_test_port_init(&R.tpm, 0x3A57u);
    R.cm = plink_test_port(&R.tpm);
    R.port.ctx = NULL;
    R.port.send = p_send;
    R.port.mtu = p_mtu;
    R.port.store = p_store;
    R.port.run = p_run;
    R.port.reachable = p_reachable;
    R.port.checkin = p_checkin;
    R.port.devices_taken = p_taken;
    R.port.held_changed = p_held;
    R.port.lockout_left = p_lockout;
    R.port.events_after = p_events_after;
    R.port.note = p_note;
    plink_master_init(&R.m, &R.cm, &R.port, NULL, "Harbor House", "a1b2c3", NOW);
    R.mnode = plink_net_node(&R.net, IP_M, NULL, m_input, m_tick, m_next);
    R.xnode = plink_net_node(&R.net, IP_X, NULL, x_input, NULL, NULL);
    R.run_mode = RUN_NOW;
    R.report = PLINK_REPORT_ONLINE;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, plink_cfg_for_keypads(&R.sim.cfg, &R.cfg_scratch, R.cfg_blob,
                                                          sizeof R.cfg_blob, &R.cfg_len,
                                                          &R.cfg_crc));
    plink_master_config(&R.m, R.cfg_blob, R.cfg_len, R.cfg_crc);
    pump(NOW);
}

/* The engine started again with cfg (before anything ran), its configuration copy handed in. */
static void restart_engine(const alarm_config_t *cfg)
{
    sim_start(&R.sim, cfg, NULL);
    TEST_ASSERT_EQUAL_UINT64(R.sim.now, NOW);
    R.ev_fwd = R.snd_fwd = 0u;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, plink_cfg_for_keypads(&R.sim.cfg, &R.cfg_scratch, R.cfg_blob,
                                                          sizeof R.cfg_blob, &R.cfg_len,
                                                          &R.cfg_crc));
    plink_master_config(&R.m, R.cfg_blob, R.cfg_len, R.cfg_crc);
    pump(NOW);
}

void tearDown(void)
{
    /* LINK-003, LINK-015: no AES-CCM key and nonce was ever used twice, on either side. */
    TEST_ASSERT_FALSE_MESSAGE(R.tpm.reused, "the master used a key and nonce twice");
    for (unsigned i = 0; i < R.nkp; i++) {
        TEST_ASSERT_FALSE_MESSAGE(R.kp[i].tp.reused, "a keypad used a key and nonce twice");
    }
    /* Nothing was ever sent from inside port.run. */
    TEST_ASSERT_EQUAL_UINT(0u, R.sent_in_run);
}

/* ---- pairing --------------------------------------------------------------------------------- */

/* LINK-001: a new keypad takes the lowest number that neither the link nor the Zigbee keypads
 * nor the device table hold; LINK-016: it is stored provisional, then a keypad like any other
 * after its first handshake. LINK-004: the codes match on both screens. */
static void test_pairing_takes_the_lowest_free_number(void)
{
    R.taken = 0x03u; /* devices 1 and 2: Zigbee keypads */
    fk_t *a = kp_add();
    a->auto_hello = false;
    TEST_ASSERT_FALSE(plink_master_in_use(&R.m));
    TEST_ASSERT_TRUE(plink_master_pair_open(&R.m, NOW));
    /* The network's id is chosen and stored at the first opening. */
    TEST_ASSERT_NOT_EQUAL(0u, plink_master_net_id(&R.m));
    TEST_ASSERT_EQUAL_UINT(1u, R.n_store);
    TEST_ASSERT_EQUAL_UINT16(plink_master_net_id(&R.m), R.stored.net_id);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);
    TEST_ASSERT_TRUE(plink_master_pairing(&R.m));
    TEST_ASSERT_TRUE(plink_master_in_use(&R.m));
    pair(a);
    TEST_ASSERT_EQUAL_UINT8(3u, a->rec.device);
    TEST_ASSERT_EQUAL_UINT16(plink_master_net_id(&R.m), a->rec.net_id);
    TEST_ASSERT_EQUAL_UINT8(1u, a->rec.home);
    TEST_ASSERT_EQUAL_STRING("Harbor House", a->rec.master_name);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", a->rec.panel_id);
    TEST_ASSERT_EQUAL_UINT8(1u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT8(3u, R.stored.peer[0].device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_KIND_PANEL, R.stored.peer[0].kind);
    TEST_ASSERT_EQUAL_STRING("Hall keypad 1", R.stored.peer[0].name);
    TEST_ASSERT_TRUE((R.stored.peer[0].flags & PLINK_PEER_PROVISIONAL) != 0u);
    TEST_ASSERT_EQUAL_UINT16(PLINK_SUPERVISION_DEFAULT_S, R.stored.peer[0].supervision_s);
    TEST_ASSERT_EQUAL_MEMORY(a->rec.k_pair, R.stored.peer[0].k_pair, PLINK_KEY_LEN);
    TEST_ASSERT_EQUAL_UINT32(0x04u, plink_master_held(&R.m));
    TEST_ASSERT_EQUAL_UINT32(0x04u, R.held);
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_PAIRED, 3u));
    TEST_ASSERT_FALSE(plink_master_pairing(&R.m));
    plink_peer_info_t info[PLINK_KEYPADS_MAX];
    TEST_ASSERT_EQUAL_size_t(1u, plink_master_peers(&R.m, NOW, info, PLINK_KEYPADS_MAX));
    TEST_ASSERT_TRUE(info[0].provisional);
    TEST_ASSERT_FALSE(info[0].online);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, info[0].heard_ago_s);

    /* Its first handshake: supervised from now on, and stored so. */
    k_hello(a);
    TEST_ASSERT_TRUE(plink_net_run_until(&R.net, 5000, c_live, a));
    TEST_ASSERT_EQUAL_UINT8(1u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.peer[0].flags & PLINK_PEER_PROVISIONAL);
    TEST_ASSERT_EQUAL_size_t(1u, plink_master_peers(&R.m, NOW, info, PLINK_KEYPADS_MAX));
    TEST_ASSERT_FALSE(info[0].provisional);
    TEST_ASSERT_TRUE(info[0].online);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, a->node), &info[0].addr));

    /* The next keypad skips a number the device table took meanwhile. */
    R.taken = 0x0Bu; /* 1, 2 and 4 */
    fk_t *b = kp_add();
    pair(b);
    TEST_ASSERT_EQUAL_UINT8(5u, b->rec.device);
    TEST_ASSERT_EQUAL_UINT32(0x14u, R.held);
    TEST_ASSERT_EQUAL_UINT8(2u, R.stored.count);
}

/* LINK-001: no pairing without a free number; the net id is stored before it is used; a
 * keypad whose number vanished while it compared gets PAIR_FAIL FULL and nothing is stored. */
static void test_pairing_needs_a_number_and_a_stored_net_id(void)
{
    R.taken = 0xFFu;
    TEST_ASSERT_FALSE(plink_master_pair_open(&R.m, NOW));
    TEST_ASSERT_FALSE(plink_master_pairing(&R.m));
    TEST_ASSERT_EQUAL_UINT16(0u, plink_master_net_id(&R.m));
    R.taken = 0u;
    R.store_fail = true;
    TEST_ASSERT_FALSE(plink_master_pair_open(&R.m, NOW));
    TEST_ASSERT_EQUAL_UINT16(0u, plink_master_net_id(&R.m));
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_STORE_FAILED, 0u));
    R.store_fail = false;

    fk_t *k = kp_add();
    pair_start(k);
    R.taken = 0xFFu; /* the Zigbee side took every number */
    plink_pk_confirm(&k->pk, NOW, true, &KOUT);
    k_send_out(k);
    plink_master_pair_decide(&R.m, NOW, true);
    run(2000);
    plink_pair_info_t ki;
    plink_pk_info(&k->pk, NOW, &ki);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, ki.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_FULL, ki.fail);
    TEST_ASSERT_FALSE(k->paired);
    TEST_ASSERT_EQUAL_UINT32(0u, plink_master_held(&R.m));
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);

    /* A record that can't be written: PAIR_FAIL STORE, nothing kept. */
    R.taken = 0u;
    pair_start(k);
    R.store_fail = true;
    plink_pk_confirm(&k->pk, NOW, true, &KOUT);
    k_send_out(k);
    plink_master_pair_decide(&R.m, NOW, true);
    run(2000);
    plink_pk_info(&k->pk, NOW, &ki);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_STORE, ki.fail);
    TEST_ASSERT_EQUAL_UINT32(0u, plink_master_held(&R.m));
    TEST_ASSERT_FALSE(R.m.peer[0].used);
}

/* ---- the handshake --------------------------------------------------------------------------- */

static unsigned welcomes(void)
{
    return m_sent(PLINK_T_WELCOME);
}

/* LINK-015: a HELLO whose mac1 isn't the keypad's gets no WELCOME (no X25519 spent) but a
 * NOT_PAIRED hint, and the live session goes on. */
static void test_hello_with_a_bad_mac_is_not_answered(void)
{
    fk_t *k = kp_add();
    pair(k);
    const unsigned w0 = welcomes();
    const uint32_t sid = mpeer(k)->live.sid;
    uint8_t wrong[PLINK_KEY_LEN];
    memcpy(wrong, k->rec.k_pair, sizeof wrong);
    wrong[0] ^= 0x01u;
    plink_hs_k_t hs;
    TEST_ASSERT_TRUE(plink_hs_hello(&k->c, wrong, k->rec.net_id, k->rec.device, &CAP, &hs));
    k_send(k, hs.hello, hs.hello_len);
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0, welcomes());
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_NOT_PAIRED]);
    TEST_ASSERT_FALSE(mpeer(k)->pending.valid);
    TEST_ASSERT_EQUAL_UINT32(sid, mpeer(k)->live.sid);
    const unsigned st = k->count[PLINK_T_STATUS];
    run(11000);
    TEST_ASSERT_TRUE(k->count[PLINK_T_STATUS] > st);
}

/* LINK-015: a HELLO replayed after its session went live is dropped (its n_k was seen), from
 * the keypad's address or another; the live session is untouched. */
static void test_hello_replayed_after_promotion_is_dropped(void)
{
    fk_t *k = kp_add();
    pair(k);
    uint8_t hello[160];
    size_t hl = 0;
    TEST_ASSERT_TRUE(plink_net_last(&R.net, k->node, PLINK_T_HELLO, hello, sizeof hello, &hl));
    const unsigned w0 = welcomes();
    const uint32_t sid = mpeer(k)->live.sid;
    run(1100); /* the X25519 budget is not what stops it */
    plink_net_inject(&R.net, k->node, maddr(), hello, hl);
    plink_net_inject(&R.net, R.xnode, maddr(), hello, hl);
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0, welcomes());
    TEST_ASSERT_EQUAL_UINT(0u, m_sent(PLINK_T_NOT_PAIRED));
    TEST_ASSERT_FALSE(mpeer(k)->pending.valid);
    TEST_ASSERT_EQUAL_UINT32(sid, mpeer(k)->live.sid);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->node), &mpeer(k)->addr));
}

/* LINK-015: the same HELLO, byte for byte, while its handshake is pending gets the same WELCOME
 * (its first copy was lost); no new key is made. */
static void test_hello_repeated_while_pending_gets_the_same_welcome(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(1100);
    k->hb_on_welcome = false;
    R.net.lose_type = PLINK_T_WELCOME;
    R.net.lose_type_count = 1u;
    const unsigned w0 = welcomes();
    k_hello(k);
    run(100);
    TEST_ASSERT_TRUE(k->hs.waiting); /* lost */
    TEST_ASSERT_TRUE(mpeer(k)->pending.valid);
    uint8_t w1[160], w2[160];
    size_t l1 = 0, l2 = 0;
    TEST_ASSERT_TRUE(plink_net_last(&R.net, R.mnode, PLINK_T_WELCOME, w1, sizeof w1, &l1));
    const uint32_t pending = mpeer(k)->pending.sid;
    k_send(k, k->hs.hello, k->hs.hello_len);
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0 + 2u, welcomes());
    TEST_ASSERT_TRUE(plink_net_last(&R.net, R.mnode, PLINK_T_WELCOME, w2, sizeof w2, &l2));
    TEST_ASSERT_EQUAL_size_t(l1, l2);
    TEST_ASSERT_EQUAL_MEMORY(w1, w2, l1);
    TEST_ASSERT_EQUAL_UINT32(pending, mpeer(k)->pending.sid);
    TEST_ASSERT_FALSE(k->hs.waiting);
    TEST_ASSERT_EQUAL_UINT32(pending, k->live.sid);
}

/* n fresh HELLOs from keypad k at once. */
static void fresh_hellos(fk_t *k, int n)
{
    for (int i = 0; i < n; i++) {
        plink_hs_k_t hs;
        TEST_ASSERT_TRUE(plink_hs_hello(&k->c, k->rec.k_pair, k->rec.net_id, k->rec.device, &CAP,
                                        &hs));
        k_send(k, hs.hello, hs.hello_len);
    }
}

/* LINK-015: at most about 4 handshakes a second over all keypads; LINK-027: one keypad's fresh
 * HELLOs get a burst of 2 and then 1 a second, so a device with one keypad's keys can't use up
 * the budget of the others (and a keypad refused for the others' sake keeps its share). */
static void test_x25519_budget(void)
{
    fk_t *k[KP_MAX];
    for (unsigned j = 0; j < KP_MAX; j++) {
        k[j] = kp_add();
        pair(k[j]);
        k[j]->hb_on_welcome = false;
    }
    run(2100);
    /* One keypad: 2 of 6, then 1 a second. */
    unsigned w0 = welcomes();
    fresh_hellos(k[0], 6);
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0 + 2u, welcomes());
    run(1000);
    fresh_hellos(k[0], 3);
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0 + 3u, welcomes());
    /* Every keypad at once: 4 of 6 in a second, whoever sends them. */
    run(2100);
    w0 = welcomes();
    for (unsigned j = 0; j < KP_MAX; j++) {
        fresh_hellos(k[j], 2);
    }
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0 + PLINK_X25519_PER_S, welcomes());
    /* The last keypad was refused for the budget, not for its own share: both of its next ones
     * go once the second is over. */
    run(1000);
    fresh_hellos(k[KP_MAX - 1u], 2);
    run(100);
    TEST_ASSERT_EQUAL_UINT(w0 + PLINK_X25519_PER_S + 2u, welcomes());
}

/* LINK-015: the live session stays until a sealed frame proves the pending one; promotion sends
 * STATUS and TIME at once under the new session, the STATUS again at +250 ms and +1 s as a
 * change's (LINK-006: one lost copy must not leave the keypad without a status for 10 s); the old
 * session's frames then get a REKEY; a pending session never proved expires after 20 s. */
static void test_pending_session_and_promotion(void)
{
    plink_master_time(&R.m, NOW, 1790000000000LL, "UTC0");
    fk_t *k = kp_add();
    pair(k);
    run(2000);
    plink_peer_t *p = mpeer(k);
    const uint32_t l1 = p->live.sid;
    k->hb_on_welcome = false;
    k_hello(k);
    run(50);
    const uint32_t l2 = k->live.sid;
    TEST_ASSERT_NOT_EQUAL(l1, l2);
    TEST_ASSERT_EQUAL_UINT32(l1, k->old.sid);
    TEST_ASSERT_EQUAL_UINT32(l1, p->live.sid);
    TEST_ASSERT_TRUE(p->pending.valid);
    TEST_ASSERT_EQUAL_UINT32(l2, p->pending.sid);
    plink_session_t s1 = k->old; /* a copy of the old session, to use after the keypad drops it */
    /* The old session still works: a frame under it moves nothing. */
    k_heartbeat_with(k, &k->old);
    s1.tx_seq = k->old.tx_seq;
    run(50);
    TEST_ASSERT_EQUAL_UINT32(l1, p->live.sid);
    TEST_ASSERT_TRUE(p->pending.valid);
    /* The first frame of the new one promotes it. */
    const unsigned st = k->count[PLINK_T_STATUS], tm = k->count[PLINK_T_TIME];
    k_heartbeat(k);
    run(50);
    TEST_ASSERT_EQUAL_UINT32(l2, p->live.sid);
    TEST_ASSERT_FALSE(p->pending.valid);
    TEST_ASSERT_EQUAL_UINT(st + 1u, k->count[PLINK_T_STATUS]);
    TEST_ASSERT_EQUAL_UINT(tm + 1u, k->count[PLINK_T_TIME]);
    TEST_ASSERT_EQUAL_UINT32(l2, k->log[k->n_log - 1u].sid);
    const uint64_t promoted = k->log[k->n_log - 1u].at;
    run(PLINK_STATUS_REPEAT2_MS);
    TEST_ASSERT_EQUAL_UINT(st + 3u, k->count[PLINK_T_STATUS]);
    TEST_ASSERT_UINT64_WITHIN(5u, promoted + PLINK_STATUS_REPEAT1_MS, k->log[k->n_log - 2u].at);
    TEST_ASSERT_UINT64_WITHIN(5u, promoted + PLINK_STATUS_REPEAT2_MS, k->log[k->n_log - 1u].at);
    TEST_ASSERT_EQUAL_UINT8(PLINK_T_STATUS, k->log[k->n_log - 1u].type);
    /* The old session is gone: its frames get a hint to start a handshake. */
    k->auto_hb = false;
    k_heartbeat_with(k, &s1);
    run(50);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_REKEY]);
    TEST_ASSERT_EQUAL_UINT32(l1, k->log[k->n_log - 1u].a);
    /* A pending session that never proves itself expires after 20 s. */
    k_hello(k);
    run(50);
    const uint32_t l3 = k->live.sid;
    TEST_ASSERT_EQUAL_UINT32(l3, p->pending.sid);
    run(PLINK_PENDING_MS + 100u);
    TEST_ASSERT_FALSE(p->pending.valid);
    TEST_ASSERT_EQUAL_UINT32(l2, p->live.sid);
    k_heartbeat(k); /* under l3, which the master no longer knows */
    run(50);
    TEST_ASSERT_EQUAL_UINT(2u, k->count[PLINK_T_REKEY]);
    TEST_ASSERT_EQUAL_UINT32(l2, p->live.sid);
    plink_session_wipe(&s1);
}

/* LINK-015: a sealed frame of a session the master doesn't know, from a known keypad, gets a
 * REKEY to its source, at most once a second; an unknown keypad's gets nothing. */
static void test_rekey_hint(void)
{
    fk_t *k = kp_add();
    pair(k);
    k->auto_hb = false;
    plink_session_t fake;
    memset(&fake, 0, sizeof fake);
    fake.valid = true;
    fake.sid = 0x5EED1234u;
    memset(fake.k_tx, 0x77, sizeof fake.k_tx);
    const uint8_t pl[4] = {0};
    TEST_ASSERT_TRUE(k_seal_with(k, &fake, PLINK_T_HEARTBEAT, pl, sizeof pl));
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_REKEY]);
    TEST_ASSERT_EQUAL_UINT32(0x5EED1234u, k->log[k->n_log - 1u].a);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(k_seal_with(k, &fake, PLINK_T_HEARTBEAT, pl, sizeof pl));
        run(200);
    }
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_REKEY]);
    run(500);
    TEST_ASSERT_TRUE(k_seal_with(k, &fake, PLINK_T_HEARTBEAT, pl, sizeof pl));
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, k->count[PLINK_T_REKEY]);
    /* A device the master doesn't have: nothing. */
    const unsigned r0 = m_sent(PLINK_T_REKEY);
    const uint8_t dev = k->rec.device;
    k->rec.device = 6u;
    TEST_ASSERT_TRUE(k_seal_with(k, &fake, PLINK_T_HEARTBEAT, pl, sizeof pl));
    k->rec.device = dev;
    run(1100);
    TEST_ASSERT_EQUAL_UINT(r0, m_sent(PLINK_T_REKEY));
    plink_session_wipe(&fake);
}

/* LINK-015, LINK-022: a master restarted from its stored record knows its keypads but no
 * session: a keypad's next frame gets a REKEY and a new handshake brings it back. */
static void test_master_restart_and_rekey(void)
{
    fk_t *k = kp_add();
    k->auto_rekey = true;
    pair(k);
    run(3000);
    const plink_master_rec_t rec = R.stored;
    plink_master_init(&R.m, &R.cm, &R.port, &rec, "Harbor House", "a1b2c3", NOW);
    plink_master_config(&R.m, R.cfg_blob, R.cfg_len, R.cfg_crc);
    TEST_ASSERT_EQUAL_UINT16(rec.net_id, plink_master_net_id(&R.m));
    TEST_ASSERT_EQUAL_UINT32(1u << (k->rec.device - 1u), plink_master_held(&R.m));
    const unsigned st = k->count[PLINK_T_STATUS];
    run(12000);
    TEST_ASSERT_TRUE(k->count[PLINK_T_REKEY] >= 1u);
    TEST_ASSERT_TRUE(c_live(k));
    TEST_ASSERT_TRUE(k->count[PLINK_T_STATUS] > st);
    TEST_ASSERT_EQUAL_UINT(0u, edges(k->rec.device, false));
}

/* LINK-015: a HELLO the master can't place gets NOT_PAIRED, at most once a second whoever
 * sends them; another network's HELLO is told with its own net id. */
static void test_not_paired_hint(void)
{
    fk_t *k = kp_add();
    pair(k);
    uint8_t key[PLINK_KEY_LEN];
    memset(key, 0x42, sizeof key);
    plink_hs_k_t hs;
    TEST_ASSERT_TRUE(plink_hs_hello(&k->c, key, k->rec.net_id, 6u, &CAP, &hs));
    k_send(k, hs.hello, hs.hello_len);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_NOT_PAIRED]);
    k_send(k, hs.hello, hs.hello_len);
    run(500);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_NOT_PAIRED]);
    run(600);
    const uint16_t other = (uint16_t)(k->rec.net_id ^ 0x5555u);
    TEST_ASSERT_TRUE(plink_hs_hello(&k->c, key, other, 6u, &CAP, &hs));
    k_send(k, hs.hello, hs.hello_len);
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, k->count[PLINK_T_NOT_PAIRED]);
    TEST_ASSERT_EQUAL_UINT32(other, k->log[k->n_log - 1u].a);
}

/* ---- status (LINK-006) ----------------------------------------------------------------------- */

/* Times at which keypad k received a STATUS since t. */
static size_t status_times(const fk_t *k, uint64_t since, uint64_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < k->n_log && n < max; i++) {
        if (k->log[i].type == PLINK_T_STATUS && k->log[i].at >= since) {
            out[n++] = k->log[i].at;
        }
    }
    return n;
}

/* LINK-006: with nothing changing, a STATUS every 10 s - however often the platform hands in a
 * snapshot (here after every engine tick, with a new seq each time). */
static void test_status_every_10_s(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(1500);
    const uint64_t t0 = NOW;
    run(35000);
    uint64_t t[16];
    const size_t n = status_times(k, t0, t, 16);
    TEST_ASSERT_EQUAL_size_t(3u, n);
    TEST_ASSERT_UINT64_WITHIN(1u, PLINK_STATUS_EVERY_MS, t[1] - t[0]);
    TEST_ASSERT_UINT64_WITHIN(1u, PLINK_STATUS_EVERY_MS, t[2] - t[1]);
}

/* LINK-006: a change goes at once, then again at +250 ms and +1 s; then every 10 s. */
static void test_status_on_change_is_repeated(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(2000);
    const uint64_t t0 = NOW;
    at_now();
    sim_fault(&R.sim, Z_WINDOW);
    run(1500);
    uint64_t t[16];
    const size_t n = status_times(k, t0, t, 16);
    TEST_ASSERT_EQUAL_size_t(3u, n);
    TEST_ASSERT_TRUE(t[0] - t0 <= 60u);
    TEST_ASSERT_EQUAL_UINT64(PLINK_STATUS_REPEAT1_MS, t[1] - t[0]);
    TEST_ASSERT_EQUAL_UINT64(PLINK_STATUS_REPEAT2_MS, t[2] - t[0]);
    /* What it says: partition 1 is not ready now. */
    TEST_ASSERT_EQUAL_UINT8(1u, k->status.count);
    TEST_ASSERT_EQUAL_UINT8(0u, k->status.part[0].flags & PLINK_PF_READY);
    TEST_ASSERT_EQUAL_UINT8(1u, k->status.home);
    TEST_ASSERT_EQUAL_UINT8(PLINK_REPORT_ONLINE, k->status.report);
    TEST_ASSERT_EQUAL_UINT32(R.cfg_crc, k->status.cfg_crc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)R.cfg_len, k->status.cfg_len);
    run(10000);
    TEST_ASSERT_EQUAL_size_t(4u, status_times(k, t0, t, 16));
    TEST_ASSERT_UINT64_WITHIN(1u, PLINK_STATUS_EVERY_MS, t[3] - t[2]);
}

/* LINK-006: changes in a burst are coalesced: never more than one STATUS per 100 ms to a keypad,
 * and the last one says how things ended. */
static void test_status_changes_are_coalesced(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(2000);
    const size_t log0 = R.net.log_count;
    for (int i = 0; i < 12; i++) {
        at_now();
        if (i % 2 == 0) {
            sim_fault(&R.sim, Z_WINDOW);
        } else {
            sim_restore(&R.sim, Z_WINDOW);
        }
        run(15);
    }
    run(3000);
    uint64_t last = 0;
    unsigned sent = 0;
    for (size_t i = log0; i < R.net.log_count; i++) {
        const plink_net_seen_t *s = &R.net.log[i];
        if (s->from == R.mnode && s->to == k->node && s->type == PLINK_T_STATUS) {
            if (sent > 0u) {
                TEST_ASSERT_TRUE_MESSAGE(s->at_ms - last >= PLINK_STATUS_MIN_GAP_MS,
                                         "two STATUS within 100 ms");
            }
            last = s->at_ms;
            sent++;
        }
    }
    TEST_ASSERT_TRUE(sent >= 3u && sent <= 6u);
    TEST_ASSERT_TRUE((k->status.part[0].flags & PLINK_PF_READY) != 0u);
}

/* LINK-005: the zones that changed go, sealed, ahead of the STATUS that carries their CRC; a
 * ZONES_REQ is answered with the records and the current CRC. */
static void test_zones_pushed_and_requested(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(2000);
    const size_t log0 = k->n_log;
    at_now();
    sim_fault(&R.sim, Z_FAMILY);
    run(100);
    size_t zi = SIZE_MAX, si = SIZE_MAX;
    for (size_t i = log0; i < k->n_log; i++) {
        if (k->log[i].type == PLINK_T_ZONES && zi == SIZE_MAX) {
            zi = i;
        }
        if (k->log[i].type == PLINK_T_STATUS && si == SIZE_MAX) {
            si = i;
        }
    }
    TEST_ASSERT_TRUE(zi != SIZE_MAX && si != SIZE_MAX);
    TEST_ASSERT_TRUE(zi < si);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)Z_FAMILY | (1u << 16), k->log[zi].a);
    plink_zones_t z;
    TEST_ASSERT_TRUE(plink_dec_zones(k->last[PLINK_T_ZONES], k->last_len[PLINK_T_ZONES], &z));
    TEST_ASSERT_EQUAL_UINT32(k->status.zones_crc, z.zones_crc);
    alarm_zone_status_t zs;
    plink_zones_rec(&z, 0, &zs);
    TEST_ASSERT_EQUAL_UINT16(sim_zstatus(&R.sim, Z_FAMILY).flags, zs.flags);
    TEST_ASSERT_TRUE((zs.flags & ALARM_ZS_FAULT) != 0u);
    /* Asked for: zones 1..20 in one frame on UDP. */
    k_zones_req(k, 1u, 20u);
    run(20);
    TEST_ASSERT_TRUE(plink_dec_zones(k->last[PLINK_T_ZONES], k->last_len[PLINK_T_ZONES], &z));
    TEST_ASSERT_EQUAL_UINT16(1u, z.first);
    TEST_ASSERT_EQUAL_UINT16(20u, z.count);
    alarm_snapshot_t snap;
    alarm_engine_snapshot(&R.sim.eng, &snap);
    TEST_ASSERT_EQUAL_UINT32(plink_zones_crc(snap.zone, snap.zones_used), z.zones_crc);
    plink_zones_rec(&z, Z_FAMILY - 1u, &zs);
    TEST_ASSERT_TRUE((zs.flags & ALARM_ZS_FAULT) != 0u);
}

/* LINK-005: on a transport with small frames every answer is paged to fit: zones, the
 * configuration, events; STATUS still fits. */
static void test_small_frames_are_paged(void)
{
    R.mtu = 160u;
    fk_t *k = kp_add();
    pair(k);
    run(1000);
    const size_t log0 = R.net.log_count;
    k_zones_req(k, 1u, 20u);
    run(50);
    TEST_ASSERT_EQUAL_UINT(2u, k->count[PLINK_T_ZONES]); /* 18 zones, then 2 */
    plink_zones_t z;
    TEST_ASSERT_TRUE(plink_dec_zones(k->last[PLINK_T_ZONES], k->last_len[PLINK_T_ZONES], &z));
    TEST_ASSERT_EQUAL_UINT16(19u, z.first);
    TEST_ASSERT_EQUAL_UINT16(2u, z.count);
    k_cfg_req(k, R.cfg_crc, 0u, 0u);
    run(20);
    plink_cfg_page_t pg;
    TEST_ASSERT_TRUE(plink_dec_cfg_page(k->last[PLINK_T_CFG_PAGE], k->last_len[PLINK_T_CFG_PAGE],
                                        &pg));
    TEST_ASSERT_EQUAL_UINT16(160u - PLINK_SEALED_OVERHEAD - PLINK_CFG_PAGE_HDR, pg.len);
    for (size_t i = log0; i < R.net.log_count; i++) {
        if (R.net.log[i].from == R.mnode) {
            TEST_ASSERT_TRUE(R.net.log[i].len <= 160u);
        }
    }
    TEST_ASSERT_TRUE(k->status_at != 0u); /* STATUS came */
}

/* LINK-005: a STATUS too big for the transport's frame goes as several, each with the system
 * section and the partition records that fit, in order. */
static void test_status_split_over_small_frames(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    for (unsigned p = 1u; p < ALARM_MAX_PARTITIONS; p++) {
        cfg.partitions[p].enabled = true;
    }
    restart_engine(&cfg);
    R.mtu = 160u; /* the system section and one record */
    fk_t *k = kp_add();
    pair(k);
    run(50);
    TEST_ASSERT_EQUAL_size_t(ALARM_MAX_PARTITIONS, k->n_st);
    for (size_t i = 0; i < k->n_st; i++) {
        TEST_ASSERT_EQUAL_UINT8(i + 1u, k->st_first[i]);
    }
    TEST_ASSERT_EQUAL_UINT8((1u << ALARM_MAX_PARTITIONS) - 1u, k->status.partitions);
    for (size_t i = 0; i < R.net.log_count; i++) {
        if (R.net.log[i].from == R.mnode) {
            TEST_ASSERT_TRUE(R.net.log[i].len <= 160u);
        }
    }
}

/* LINK-005: the configuration copy is paged out: pulled page by page it is the blob, with its
 * CRC; a request for another blob, or past its end, starts again from offset 0. */
static void test_configuration_pages(void)
{
    fk_t *k = kp_add();
    pair(k);
    static uint8_t got[ALARM_CONFIG_BLOB_MAX];
    uint32_t off = 0;
    plink_cfg_page_t pg;
    int pages = 0;
    do {
        k_cfg_req(k, R.cfg_crc, off, 0u);
        run(20);
        TEST_ASSERT_TRUE(plink_dec_cfg_page(k->last[PLINK_T_CFG_PAGE],
                                            k->last_len[PLINK_T_CFG_PAGE], &pg));
        TEST_ASSERT_EQUAL_UINT32(R.cfg_crc, pg.cfg_crc);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)R.cfg_len, pg.total);
        TEST_ASSERT_EQUAL_UINT32(off, pg.offset);
        TEST_ASSERT_TRUE(pg.len > 0u);
        memcpy(&got[off], pg.data, pg.len);
        off += pg.len;
        pages++;
    } while (off < pg.total);
    TEST_ASSERT_EQUAL_MEMORY(R.cfg_blob, got, R.cfg_len);
    TEST_ASSERT_EQUAL_UINT32(R.cfg_crc, alarm_crc32(0u, got, R.cfg_len));
    TEST_ASSERT_TRUE(pages >= 1);
    k_cfg_req(k, R.cfg_crc ^ 1u, 7u, 0u);
    run(20);
    TEST_ASSERT_TRUE(plink_dec_cfg_page(k->last[PLINK_T_CFG_PAGE], k->last_len[PLINK_T_CFG_PAGE],
                                        &pg));
    TEST_ASSERT_EQUAL_UINT32(R.cfg_crc, pg.cfg_crc);
    TEST_ASSERT_EQUAL_UINT32(0u, pg.offset);
    k_cfg_req(k, R.cfg_crc, (uint32_t)R.cfg_len + 10u, 0u);
    run(20);
    TEST_ASSERT_TRUE(plink_dec_cfg_page(k->last[PLINK_T_CFG_PAGE], k->last_len[PLINK_T_CFG_PAGE],
                                        &pg));
    TEST_ASSERT_EQUAL_UINT32(0u, pg.offset);
    k_cfg_req(k, R.cfg_crc, 0u, 100u);
    run(20);
    TEST_ASSERT_TRUE(plink_dec_cfg_page(k->last[PLINK_T_CFG_PAGE], k->last_len[PLINK_T_CFG_PAGE],
                                        &pg));
    TEST_ASSERT_EQUAL_UINT16(R.cfg_len < 100u ? R.cfg_len : 100u, pg.len);
    /* A new configuration: its CRC goes out in a STATUS. */
    R.cfg_blob[R.cfg_len - 1u] ^= 0x01u;
    const uint32_t crc2 = alarm_crc32(0u, R.cfg_blob, R.cfg_len);
    plink_master_config(&R.m, R.cfg_blob, R.cfg_len, crc2);
    run(100);
    TEST_ASSERT_EQUAL_UINT32(crc2, k->status.cfg_crc);
}

/* LINK-027 (and docs/features/11 "Configuration"): requests for configuration, zones and events
 * over their limit are dropped in the link task. */
static void test_requests_are_rate_limited(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(3000);
    const unsigned e0 = k->count[PLINK_T_EVENTS];
    for (int i = 0; i < 40; i++) {
        k_ev_req(k, 0u, 1u);
    }
    run(20);
    TEST_ASSERT_EQUAL_UINT(e0 + 32u, k->count[PLINK_T_EVENTS]);
    run(1000);
    k_ev_req(k, 0u, 1u);
    run(20);
    TEST_ASSERT_EQUAL_UINT(e0 + 33u, k->count[PLINK_T_EVENTS]);
}

/* ---- events ---------------------------------------------------------------------------------- */

/* LINK-005, LINK-006: each new event is pushed, sealed, before the STATUS that carries its seq -
 * also when the platform hands the snapshot in first; none twice. */
static void test_events_go_before_their_status(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(2000);
    const size_t ev0 = k->n_ev;
    at_now();
    EXPECT_RESULT(ALARM_OK, sim_arm(&R.sim, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    run(2000);
    TEST_ASSERT_TRUE(k->n_ev > ev0);
    TEST_ASSERT_FALSE(k->ev_order_bad);
    TEST_ASSERT_EQUAL_UINT32(R.sim.eng.seq, k->ev_max);
    TEST_ASSERT_EQUAL_UINT32(R.sim.eng.seq, k->status.last_event_seq);

    R.snap_first = true;
    const size_t ev1 = k->n_ev;
    at_now();
    EXPECT_RESULT(ALARM_OK, sim_disarm(&R.sim, 1, CODE_USER));
    run(2000);
    TEST_ASSERT_TRUE(k->n_ev > ev1);
    TEST_ASSERT_FALSE(k->ev_order_bad);
    TEST_ASSERT_EQUAL_UINT32(R.sim.eng.seq, k->ev_max);
    for (size_t i = ev0 + 1u; i < k->n_ev; i++) {
        TEST_ASSERT_TRUE_MESSAGE(k->ev_seq[i] > k->ev_seq[i - 1u],
                                 "an event twice or out of order");
    }
}

/* LINK-005: EV_REQ gets the newest `max` events after after_seq that one frame holds, oldest
 * first, with the gap flag when older ones were left out. */
static void test_event_requests(void)
{
    fk_t *k = kp_add();
    pair(k);
    for (int i = 0; i < 20; i++) {
        at_now();
        (void)sim_arm(&R.sim, 1, CODE_USER, ALARM_LEVEL_STAY, 0);
        run(50);
        at_now();
        (void)sim_disarm(&R.sim, 1, CODE_USER);
        run(50);
    }
    run(500);
    const uint32_t newest = R.sim.eng.seq;
    TEST_ASSERT_TRUE(newest > 40u);
    const size_t n0 = k->n_ev;
    k_ev_req(k, 0u, 50u);
    run(20);
    TEST_ASSERT_EQUAL_size_t(n0 + 32u, k->n_ev);
    TEST_ASSERT_TRUE((k->ev_flags & PLINK_EVF_GAP) != 0u);
    TEST_ASSERT_EQUAL_UINT32(newest, k->ev_seq[k->n_ev - 1u]);
    TEST_ASSERT_EQUAL_UINT32(newest - 31u, k->ev_seq[n0]);
    k_ev_req(k, newest - 3u, 50u);
    run(20);
    TEST_ASSERT_EQUAL_size_t(n0 + 35u, k->n_ev);
    TEST_ASSERT_EQUAL_UINT8(0u, k->ev_flags & PLINK_EVF_GAP);
    TEST_ASSERT_EQUAL_UINT32(newest - 2u, k->ev_seq[n0 + 32u]);
    const unsigned e = k->count[PLINK_T_EVENTS];
    k_ev_req(k, newest, 50u);
    run(20);
    TEST_ASSERT_EQUAL_UINT(e + 1u, k->count[PLINK_T_EVENTS]); /* answered: nothing newer */
    TEST_ASSERT_EQUAL_size_t(n0 + 35u, k->n_ev);
}

/* ---- sounds ---------------------------------------------------------------------------------- */

/* LINK-002 (docs/features/11 "Roles", Sounds): a sound goes to the keypads whose home it
 * concerns (partition 0: all); fire and CO to every keypad. A keypad learns a new home from its
 * STATUS. */
static void test_sounds_by_home_partition(void)
{
    fk_t *a = kp_add(), *b = kp_add();
    pair(a);
    pair(b);
    TEST_ASSERT_TRUE(plink_master_set_home(&R.m, NOW, b->rec.device, 2u));
    run(200);
    TEST_ASSERT_EQUAL_UINT8(2u, b->status.home);
    TEST_ASSERT_EQUAL_UINT8(2u, R.stored.peer[1].home);
    unsigned sa = a->count[PLINK_T_SOUND], sb = b->count[PLINK_T_SOUND];
    plink_master_sound(&R.m, NOW, 1u, ALARM_SOUND_CHIME, 3u);
    run(10);
    TEST_ASSERT_EQUAL_UINT(++sa, a->count[PLINK_T_SOUND]);
    TEST_ASSERT_EQUAL_UINT(sb, b->count[PLINK_T_SOUND]);
    plink_master_sound(&R.m, NOW, 2u, ALARM_SOUND_ENTRY, 0u);
    run(10);
    TEST_ASSERT_EQUAL_UINT(sa, a->count[PLINK_T_SOUND]);
    TEST_ASSERT_EQUAL_UINT(++sb, b->count[PLINK_T_SOUND]);
    plink_master_sound(&R.m, NOW, 0u, ALARM_SOUND_ACK, 0u);
    run(10);
    TEST_ASSERT_EQUAL_UINT(++sa, a->count[PLINK_T_SOUND]);
    TEST_ASSERT_EQUAL_UINT(++sb, b->count[PLINK_T_SOUND]);
    plink_master_sound(&R.m, NOW, 2u, ALARM_SOUND_FIRE, Z_SMOKE);
    run(10);
    TEST_ASSERT_EQUAL_UINT(++sa, a->count[PLINK_T_SOUND]);
    TEST_ASSERT_EQUAL_UINT(++sb, b->count[PLINK_T_SOUND]);
    plink_master_sound(&R.m, NOW, 1u, ALARM_SOUND_CO, Z_CO);
    run(10);
    TEST_ASSERT_EQUAL_UINT(++sa, a->count[PLINK_T_SOUND]);
    TEST_ASSERT_EQUAL_UINT(++sb, b->count[PLINK_T_SOUND]);
    plink_sound_t s;
    TEST_ASSERT_TRUE(plink_dec_sound(b->last[PLINK_T_SOUND], b->last_len[PLINK_T_SOUND], &s));
    TEST_ASSERT_EQUAL_UINT8(1u, s.partition);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_CO, s.sound);
    TEST_ASSERT_EQUAL_UINT16(Z_CO, s.zone);
}

/* ---- commands (LINK-018, LINK-027) ----------------------------------------------------------- */

/* LINK-002, LINK-018: a keypad's command runs once at the master as {KEYPAD, n}, n from the
 * session; the answer, given from inside port.run, goes once port.run is back. */
static void test_command_runs_as_the_keypad(void)
{
    fk_t *k = kp_add();
    pair(k);
    k_cmd(k, 1001u, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, CODE_USER);
    run(50);
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT8(k->rec.device, R.runs[0].device);
    const plink_result_t *r = k_result(k, 1001u);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(ALARM_OK, r->result);
    TEST_ASSERT_EQUAL_UINT8(3u, r->user);
    EXPECT_STATE(&R.sim, 1, ALARM_STATE_EXIT_DELAY);
    const alarm_event_t *ev = sim_last_event(&R.sim, ALARM_EV_ARMED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(k->rec.device, ev->source.id);
    /* The STATUS shows the exit delay counting down. */
    run(300);
    TEST_ASSERT_EQUAL_UINT8(ALARM_STATE_EXIT_DELAY, k->status.part[0].state);
    TEST_ASSERT_TRUE(k->status.part[0].delay_left_ms > 55000u);
}

/* LINK-027: only the keypad's home partition (NOT_AUTHORIZED otherwise); LINK-017: an op this
 * master doesn't serve is UNAVAILABLE; a command it doesn't know BAD_ARG. None reaches port.run. */
static void test_command_checks(void)
{
    fk_t *k = kp_add();
    pair(k);
    k_cmd(k, 2001u, PLINK_OP_COMMAND, ALARM_CMD_ARM, 2u, CODE_USER);
    k_cmd(k, 2002u, PLINK_OP_AUTH, ALARM_CMD_ARM, 0u, CODE_USER);
    k_cmd(k, 2003u, 9u, ALARM_CMD_ARM, 1u, CODE_USER);
    k_cmd(k, 2004u, PLINK_OP_COMMAND, (alarm_cmd_type_t)77, 1u, CODE_USER);
    run(50);
    TEST_ASSERT_EQUAL_UINT(0u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_NOT_AUTHORIZED, k_result(k, 2001u)->result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_NOT_AUTHORIZED, k_result(k, 2002u)->result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_UNAVAILABLE, k_result(k, 2003u)->result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_BAD_ARG, k_result(k, 2004u)->result);
    EXPECT_STATE(&R.sim, 1, ALARM_STATE_DISARMED);
}

/* LINK-018: a copy while the command runs waits for its answer; a copy after is answered from
 * the cache, for 90 s after the last copy; then the same req runs as a new command; the same req
 * with another command is a new one. Nothing runs twice. */
static void test_command_result_cache(void)
{
    fk_t *k = kp_add();
    pair(k);
    R.run_mode = RUN_LATER;
    k_cmd(k, 3001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(50);
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(0u, k_results(k, 3001u));
    k_cmd(k, 3001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER); /* a retry: waits */
    run(500);
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(0u, k_results(k, 3001u));
    answer(0);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 3001u));
    TEST_ASSERT_EQUAL_UINT8(ALARM_OK, k_result(k, 3001u)->result);
    TEST_ASSERT_EQUAL_UINT8(3u, k_result(k, 3001u)->user);
    /* Copies after the answer: from the cache, as long as one comes within 90 s of the last. */
    run(30000);
    k_cmd(k, 3001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, k_results(k, 3001u));
    run(89000);
    k_cmd(k, 3001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(3u, k_results(k, 3001u));
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    /* 90 s without a copy: forgotten; the same req then runs as a new command. */
    run(PLINK_RESULT_KEEP_MS + 100u);
    k_cmd(k, 3001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, R.n_runs);
    answer(1);
    run(10);
    TEST_ASSERT_EQUAL_UINT(4u, k_results(k, 3001u));
    /* The same req with another command: a new one. */
    k_cmd(k, 3001u, PLINK_OP_COMMAND, ALARM_CMD_CHIME, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(3u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT8(PLINK_OP_COMMAND, R.runs[2].cmd.op);
    answer(2);
    run(10);
    TEST_ASSERT_EQUAL_UINT(5u, k_results(k, 3001u));
}

/* LINK-027: one command at a time, panic in a slot of its own (LINK-023: a panic goes while a
 * command runs); a second of either kind is dropped until the first is answered. */
static void test_one_command_at_a_time_and_the_panic_slot(void)
{
    fk_t *k = kp_add();
    pair(k);
    R.run_mode = RUN_LATER;
    k_cmd(k, 4001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    k_cmd(k, 4002u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    k_cmd(k, 4003u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT32(4003u, R.runs[1].cmd.req);
    k_cmd(k, 4004u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, R.n_runs);
    answer(0);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 4001u));
    TEST_ASSERT_EQUAL_UINT(0u, k_results(k, 4002u));
    k_cmd(k, 4002u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER); /* its retry */
    run(10);
    TEST_ASSERT_EQUAL_UINT(3u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT32(4002u, R.runs[2].cmd.req);
    answer(1);
    answer(2);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 4003u));
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 4002u));
    /* LINK-023: the panic's resends with the same req get the same answer, never a second run. */
    for (int i = 0; i < 5; i++) {
        k_cmd(k, 4003u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
        run(PLINK_PANIC_RETRY_MS);
    }
    TEST_ASSERT_EQUAL_UINT(3u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(6u, k_results(k, 4003u));
}

/* LINK-027: new commands at most about 2 a second with a burst of 5; the rest are dropped
 * before port.run. A panic is not held by that rate. */
static void test_command_rate_limit(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(3000);
    for (uint32_t i = 0; i < 8u; i++) {
        k_cmd(k, 5000u + i, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    }
    run(10);
    TEST_ASSERT_EQUAL_UINT(5u, R.n_runs);
    TEST_ASSERT_EQUAL_size_t(5u, k->n_results);
    k_cmd(k, 5100u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
    run(10);
    TEST_ASSERT_EQUAL_UINT(6u, R.n_runs);
    run(PLINK_CMD_RETRY_MS);
    k_cmd(k, 5005u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    k_cmd(k, 5006u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(7u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 5005u));
    TEST_ASSERT_EQUAL_UINT(0u, k_results(k, 5006u));
}

/* LINK-018: an answer its keypad may still ask for (a command is resent for 3 s, a panic for
 * 60 s) is never given up for a new command: with every slot holding one, the new command waits
 * for its resend; a lost answer's resend is still answered from the cache, never run again. */
static void test_cache_keeps_answers_still_wanted(void)
{
    fk_t *k = kp_add();
    pair(k);
    run(3000);
    R.net.lose_type = PLINK_T_RESULT;
    R.net.lose_type_count = 1u; /* the first answer is lost */
    const uint64_t t0 = NOW;
    for (uint32_t i = 0; i < PLINK_RESULTS_PER_KEYPAD; i++) {
        if (i >= 5u) {
            run(PLINK_CMD_RETRY_MS); /* the rate's burst is 5, then one per 500 ms */
        }
        k_cmd(k, 9100u + i, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    }
    run(10);
    TEST_ASSERT_EQUAL_UINT(PLINK_RESULTS_PER_KEYPAD, R.n_runs);
    TEST_ASSERT_TRUE(NOW - t0 < PLINK_CMD_GIVE_UP_MS);
    TEST_ASSERT_EQUAL_UINT(0u, k_results(k, 9100u));
    /* A panic (not held by the rate) finds every slot holding an answer still wanted: it waits. */
    k_cmd(k, 9200u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
    run(10);
    TEST_ASSERT_EQUAL_UINT(PLINK_RESULTS_PER_KEYPAD, R.n_runs);
    /* The lost answer's resend: from the cache. */
    k_cmd(k, 9100u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 9100u));
    TEST_ASSERT_EQUAL_UINT(PLINK_RESULTS_PER_KEYPAD, R.n_runs);
    /* 3 s on, nobody resends those any more: the panic's resend takes a slot and runs once. */
    run(PLINK_CMD_GIVE_UP_MS);
    k_cmd(k, 9200u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
    run(10);
    TEST_ASSERT_EQUAL_UINT(PLINK_RESULTS_PER_KEYPAD + 1u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 9200u));
    k_cmd(k, 9200u, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, "");
    run(10);
    TEST_ASSERT_EQUAL_UINT(PLINK_RESULTS_PER_KEYPAD + 1u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(2u, k_results(k, 9200u));
}

/* LINK-018: port.run with no room: the command is forgotten, so its retry runs it (once). */
static void test_command_refused_by_the_platform_runs_on_retry(void)
{
    fk_t *k = kp_add();
    pair(k);
    R.run_mode = RUN_REFUSE;
    k_cmd(k, 6001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(0u, k_results(k, 6001u));
    R.run_mode = RUN_NOW;
    run(PLINK_CMD_RETRY_MS);
    k_cmd(k, 6001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(k, 6001u));
}

/* LINK-018: codes are never kept: after a command, no copy of its code is left in the master. */
static void test_codes_are_not_kept(void)
{
    fk_t *k = kp_add();
    pair(k);
    static const char code[] = "86429753";
    const alarm_source_t local = {ALARM_SRC_KEYPAD, ALARM_KEYPAD_LOCAL};
    at_now();
    TEST_ASSERT_EQUAL_INT(ALARM_OK, alarm_engine_user_set(&R.sim.eng, &local, CODE_MASTER, 8u,
                                                          code, ALARM_AUTH_STANDARD, 0x01u, "Pat",
                                                          R.sim.now));
    k_cmd(k, 7001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, code);
    run(50);
    TEST_ASSERT_EQUAL_UINT8(ALARM_OK, k_result(k, 7001u)->result);
    TEST_ASSERT_EQUAL_UINT8(8u, k_result(k, 7001u)->user);
    TEST_ASSERT_FALSE(mem_has(&R.m, sizeof R.m, code, strlen(code)));
}

/* ---- supervision (LINK-007) ------------------------------------------------------------------ */

/* LINK-007: a keypad not heard for its supervision time is down, told once; it is up again once
 * both directions are fresh. */
static void test_supervision_not_heard(void)
{
    fk_t *k = kp_add();
    pair(k);
    const uint8_t d = k->rec.device;
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, NOW, d, 60u));
    TEST_ASSERT_EQUAL_UINT16(60u, R.stored.peer[0].supervision_s);
    run(5000);
    const uint64_t heard = mpeer(k)->heard_ms;
    R.net.node[k->node].down = true;
    run(heard + 59000u - NOW);
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, false));
    run(2000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    TEST_ASSERT_UINT64_WITHIN(60u, heard + 60000u, R.reach[R.n_reach - 1u].at);
    plink_peer_info_t info;
    TEST_ASSERT_EQUAL_size_t(1u, plink_master_peers(&R.m, NOW, &info, 1u));
    TEST_ASSERT_TRUE(info.down);
    TEST_ASSERT_FALSE(info.online);
    run(60000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false)); /* level-based: told once */
    R.net.node[k->node].down = false;
    run(25000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, true));
    TEST_ASSERT_FALSE(mpeer(k)->down);
    TEST_ASSERT_TRUE(R.checkins[d] > 0u);
}

/* LINK-007: a keypad that is heard but gets no status (it reports one older than 30 s) is down
 * after its supervision time too; up again when status flows. */
static void test_supervision_stale_status(void)
{
    fk_t *k = kp_add();
    pair(k);
    const uint8_t d = k->rec.device;
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, NOW, d, 60u));
    run(3000);
    const uint64_t cut = NOW;
    R.net.cut_to = k->node;
    run(80000);
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, false));
    run(cut + 115000u - NOW);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    TEST_ASSERT_TRUE(mpeer(k)->hb.status_age_s > PLINK_STATUS_STALE_S);
    R.net.cut_to = -1;
    run(25000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, true));
}

/* LINK-007: supervision pauses while the master's own network is down and resumes a period after
 * it is back. */
static void test_supervision_paused_while_the_network_is_down(void)
{
    fk_t *k = kp_add();
    pair(k);
    const uint8_t d = k->rec.device;
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, NOW, d, 60u));
    plink_master_net(&R.m, NOW, false);
    R.net.node[k->node].down = true;
    run(300000);
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, false));
    plink_master_net(&R.m, NOW, true);
    const uint64_t up = NOW;
    run(59000);
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, false));
    run(2000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    TEST_ASSERT_UINT64_WITHIN(60u, up + 60000u, R.reach[R.n_reach - 1u].at);
}

/* LINK-007: a keypad set to "not supervised" never raises a trouble; a supervision time is one
 * of 1, 5 or 15 minutes; setting none clears a raised one; a setting that can't be stored is not
 * taken. */
static void test_supervision_setting(void)
{
    fk_t *k = kp_add();
    pair(k);
    const uint8_t d = k->rec.device;
    TEST_ASSERT_FALSE(plink_master_set_supervision(&R.m, NOW, d, 61u));
    TEST_ASSERT_FALSE(plink_master_set_supervision(&R.m, NOW, 7u, 60u));
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, NOW, d, 60u));
    R.net.node[k->node].down = true;
    run(70000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, NOW, d, 0u));
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, true));
    run(600000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    /* A setting that can't be stored is not taken. */
    R.store_fail = true;
    TEST_ASSERT_FALSE(plink_master_set_supervision(&R.m, NOW, d, 900u));
    TEST_ASSERT_EQUAL_UINT16(0u, mpeer(k)->rec.supervision_s);
    TEST_ASSERT_FALSE(plink_master_rename(&R.m, d, "Back door"));
    TEST_ASSERT_EQUAL_STRING("Hall keypad 1", mpeer(k)->rec.name);
    R.store_fail = false;
    TEST_ASSERT_TRUE(plink_master_rename(&R.m, d, "Back door"));
    TEST_ASSERT_EQUAL_STRING("Back door", R.stored.peer[0].name);
}

/* LINK-016: a provisional keypad is not supervised and is dropped 10 min after pairing without
 * a handshake: its trouble cleared before its number is freed. */
static void test_provisional_keypad_dropped(void)
{
    fk_t *k = kp_add();
    k->auto_hello = false;
    pair(k);
    const uint8_t d = k->rec.device;
    TEST_ASSERT_EQUAL_UINT8(1u, d);
    run(PLINK_PROVISIONAL_MS - 30000u);
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, false));
    TEST_ASSERT_TRUE(R.m.peer[0].used);
    run(31000);
    TEST_ASSERT_FALSE(R.m.peer[0].used);
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_PROVISIONAL_DROPPED, d));
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, true));
    TEST_ASSERT_TRUE(R.reach[R.n_reach - 1u].order < R.held_order);
    TEST_ASSERT_EQUAL_UINT32(0u, R.held);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);
    /* Its HELLO now gets NOT_PAIRED; the number goes to the next keypad. */
    k_hello(k);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_NOT_PAIRED]);
    fk_t *b = kp_add();
    pair(b);
    TEST_ASSERT_EQUAL_UINT8(1u, b->rec.device);
}

/* ---- removal (LINK-014) ---------------------------------------------------------------------- */

/* LINK-014: removing a keypad clears its trouble, writes the record without it and wipes its
 * key, sends a sealed BYE three times over 2 s, then its frames are refused. */
static void test_removal(void)
{
    fk_t *k = kp_add();
    pair(k);
    const uint8_t d = k->rec.device;
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, NOW, d, 60u));
    R.net.node[k->node].down = true;
    run(70000);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    R.net.node[k->node].down = false;
    const uint64_t t0 = NOW;
    const size_t log0 = k->n_log;
    TEST_ASSERT_TRUE(plink_master_remove(&R.m, NOW, d));
    /* Its trouble cleared first, its record written without it, its key gone from RAM. */
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, true));
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_REMOVED, d));
    TEST_ASSERT_FALSE(mem_has(&R.m, sizeof R.m, k->rec.k_pair, PLINK_KEY_LEN));
    TEST_ASSERT_FALSE(plink_master_in_use(&R.m));
    TEST_ASSERT_EQUAL_size_t(0u, plink_master_peers(&R.m, NOW, NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32(1u << (d - 1u), plink_master_held(&R.m)); /* until its BYEs went */
    run(3000);
    /* Three sealed BYEs, a second apart. */
    uint64_t at[4];
    size_t n = 0;
    for (size_t i = log0; i < k->n_log; i++) {
        if (k->log[i].type == PLINK_T_BYE && n < 4u) {
            at[n++] = k->log[i].at;
        }
    }
    TEST_ASSERT_EQUAL_size_t(3u, n);
    TEST_ASSERT_TRUE(at[0] - t0 <= 5u);
    TEST_ASSERT_EQUAL_UINT64(1000u, at[1] - at[0]);
    TEST_ASSERT_EQUAL_UINT64(1000u, at[2] - at[1]);
    plink_bye_t bye;
    TEST_ASSERT_TRUE(plink_dec_bye(k->last[PLINK_T_BYE], k->last_len[PLINK_T_BYE], &bye));
    TEST_ASSERT_EQUAL_UINT8(1u, bye.reason);
    TEST_ASSERT_EQUAL_UINT32(0u, plink_master_held(&R.m));
    TEST_ASSERT_EQUAL_UINT32(0u, R.held);
    TEST_ASSERT_FALSE(mem_has(&R.m, sizeof R.m, k->live.k_rx, PLINK_AES_LEN)); /* session gone */
    /* Then its frames are refused: no answer of any kind. */
    const size_t sent0 = R.net.log_count;
    k_heartbeat(k);
    k_cmd(k, 8001u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(15000);
    for (size_t i = sent0; i < R.net.log_count; i++) {
        TEST_ASSERT_FALSE(R.net.log[i].from == R.mnode && R.net.log[i].to == k->node);
    }
    TEST_ASSERT_EQUAL_UINT(0u, R.n_runs);
    k_hello(k);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_NOT_PAIRED]);
    TEST_ASSERT_FALSE(plink_master_remove(&R.m, NOW, d));
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
}

/* LINK-014: a removal whose record can't be written: false, the keypad refused anyway, and the
 * record written at a later tick. */
static void test_removal_store_retried(void)
{
    fk_t *k = kp_add();
    pair(k);
    const uint8_t d = k->rec.device;
    R.store_fail = true;
    TEST_ASSERT_FALSE(plink_master_remove(&R.m, NOW, d));
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_STORE_FAILED, 0u));
    TEST_ASSERT_EQUAL_UINT8(1u, R.stored.count);
    TEST_ASSERT_TRUE(plink_master_next(&R.m) <= NOW + 5000u);
    run(3000);
    TEST_ASSERT_EQUAL_UINT(3u, k->count[PLINK_T_BYE]);
    R.store_fail = false;
    run(3000);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_STORE_FAILED, 0u));
    k_hello(k);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_NOT_PAIRED]);
}

/* LINK-018: the answer to a command of a keypad removed meanwhile goes nowhere; its number's next
 * keypad never sees it. */
static void test_answer_after_removal(void)
{
    fk_t *k = kp_add();
    pair(k);
    R.run_mode = RUN_LATER;
    k_cmd(k, 8101u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(1u, R.n_runs);
    TEST_ASSERT_TRUE(plink_master_remove(&R.m, NOW, k->rec.device));
    run(3000);
    fk_t *b = kp_add();
    pair(b);
    TEST_ASSERT_EQUAL_UINT8(k->rec.device, b->rec.device);
    answer(0);
    run(50);
    TEST_ASSERT_EQUAL_size_t(0u, k->n_results);
    TEST_ASSERT_EQUAL_size_t(0u, b->n_results);
    /* The new keypad's own commands run as usual. */
    R.run_mode = RUN_NOW;
    k_cmd(b, 8101u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(1u, k_results(b, 8101u));
}

/* LINK-016: the installer cancels at the master: the keypad is told, nothing is stored. */
static void test_pairing_cancelled_at_the_master(void)
{
    fk_t *k = kp_add();
    pair_start(k);
    plink_master_pair_cancel(&R.m, NOW);
    run(100);
    plink_pair_info_t ki;
    plink_pk_info(&k->pk, NOW, &ki);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, ki.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CANCELLED, ki.fail);
    TEST_ASSERT_FALSE(plink_master_pairing(&R.m));
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT32(0u, plink_master_held(&R.m));
    TEST_ASSERT_FALSE(plink_master_set_home(&R.m, NOW, 1u, 1u)); /* no such keypad */
}

/* LINK-003, LINK-017, LINK-021: random bytes and damaged, truncated or replayed copies of genuine
 * frames, from another address: nothing runs, nothing is raised or noted, the keypad's session
 * and address stay, and it goes on getting its status. */
static void test_hostile_input(void)
{
    fk_t *k = kp_add();
    pair(k);
    R.tpm.only_dir = PLINK_DIR_MASTER; /* forged frames are opened, perhaps twice: watch seals */
    k_cmd(k, 8201u, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
    run(1000);
    const unsigned runs0 = R.n_runs, reach0 = R.n_reach, notes0 = R.n_notes;
    const uint32_t sid = mpeer(k)->live.sid;
    static const uint8_t types[] = {PLINK_T_HELLO, PLINK_T_HEARTBEAT, PLINK_T_CMD,
                                    PLINK_T_PAIR_REQ, PLINK_T_CFG_REQ};
    uint8_t good[5][PLINK_FRAME_MAX];
    size_t good_len[5];
    for (size_t i = 0; i < 5u; i++) {
        good_len[i] = 0;
        if (!plink_net_last(&R.net, k->node, types[i], good[i], sizeof good[i], &good_len[i])) {
            good_len[i] = 0;
        }
    }
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    uint8_t f[PLINK_FRAME_MAX + 8u];
    for (int it = 0; it < 20000; it++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        const unsigned pick = (unsigned)(rng % 4u);
        const size_t g = (size_t)((rng >> 8) % 5u);
        size_t len;
        if (pick == 0u || good_len[g] == 0u) {
            len = (size_t)((rng >> 16) % 300u);
            for (size_t i = 0; i < len; i++) {
                f[i] = (uint8_t)(rng >> ((i % 7u) * 8u)) ^ (uint8_t)(i * 31u);
            }
            if (len > 4u && (rng & 0x100u) != 0u) {
                f[0] = PLINK_MAGIC0; /* a real header now and then */
                f[1] = PLINK_MAGIC1;
                f[2] = PLINK_VERSION;
                f[3] = types[g];
            }
        } else {
            len = good_len[g];
            memcpy(f, good[g], len);
            if (pick == 1u) {
                f[(rng >> 20) % len] ^= (uint8_t)(1u + (rng >> 32) % 255u);
                f[(rng >> 40) % len] ^= (uint8_t)(rng >> 48);
            } else if (pick == 2u) {
                len = (size_t)((rng >> 24) % len); /* cut short */
            } /* pick 3: replayed as is */
        }
        plink_master_input(&R.m, NOW, plink_net_addr(&R.net, R.xnode), f, len);
        if (it % 50 == 0) {
            run(1);
        }
    }
    TEST_ASSERT_EQUAL_UINT(runs0, R.n_runs);
    TEST_ASSERT_EQUAL_UINT(reach0, R.n_reach);
    TEST_ASSERT_EQUAL_UINT(notes0, R.n_notes);
    TEST_ASSERT_EQUAL_UINT32(sid, mpeer(k)->live.sid);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->node), &mpeer(k)->addr));
    TEST_ASSERT_EQUAL_UINT32(1u << (k->rec.device - 1u), plink_master_held(&R.m));
    const unsigned st = k->count[PLINK_T_STATUS];
    run(11000);
    TEST_ASSERT_TRUE(k->count[PLINK_T_STATUS] > st);
    TEST_ASSERT_EQUAL_UINT(0u, m_sent_to(R.xnode, PLINK_T_STATUS));
}

/* ---- addresses (LINK-021) and the clone note ------------------------------------------------- */

/* LINK-015, LINK-021 (docs/features/11 "Security notes"): a handshake completed from a new
 * address while the live session was heard in the last 15 s is noted; after a longer silence it
 * is a keypad that moved. */
static void test_clone_note(void)
{
    fk_t *k = kp_add();
    kp_alt(k);
    pair(k);
    const uint8_t d = k->rec.device;
    run(3000);
    k->via = k->alt;
    k_hello(k);
    run(100);
    TEST_ASSERT_TRUE(c_live(k));
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_CLONE, d));
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->alt), &mpeer(k)->addr));
    k->auto_hb = false;
    run(16000);
    k->via = k->node;
    k_hello(k);
    run(100);
    TEST_ASSERT_TRUE(c_live(k));
    TEST_ASSERT_EQUAL_UINT(1u, notes(PLINK_NOTE_CLONE, d));
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->node), &mpeer(k)->addr));
}

/* LINK-021: a keypad's address moves only on a sealed frame that passed the tag and the replay
 * window: a replayed frame or a forged one from elsewhere moves nothing. */
static void test_address_moves_only_on_authenticated_frames(void)
{
    fk_t *k = kp_add();
    kp_alt(k);
    pair(k);
    run(1000);
    uint8_t f[PLINK_FRAME_MAX];
    size_t fl = 0;
    TEST_ASSERT_TRUE(plink_net_last(&R.net, k->node, PLINK_T_HEARTBEAT, f, sizeof f, &fl));
    plink_net_inject(&R.net, R.xnode, maddr(), f, fl); /* a replay from elsewhere */
    /* The same frame with a seq far ahead, then a damaged tag under yet another seq: both fail
     * the tag. (Each forgery takes its own seq: a failed tag doesn't move the window, so a second
     * forgery under the same seq would be opened again with the same nonce, which the test
     * port counts as a reuse.) */
    f[13] = 0x00u;
    f[14] = 0x10u;
    f[15] = 0x00u;
    f[16] = 0x00u;
    plink_net_inject(&R.net, R.xnode, maddr(), f, fl);
    f[14] = 0x20u;
    f[fl - 1u] ^= 0x01u;
    plink_net_inject(&R.net, R.xnode, maddr(), f, fl);
    run(20);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->node), &mpeer(k)->addr));
    run(11000);
    TEST_ASSERT_EQUAL_UINT(0u, m_sent_to(R.xnode, PLINK_T_STATUS));
    TEST_ASSERT_EQUAL_UINT(0u, R.x_count[PLINK_T_STATUS] + R.x_count[PLINK_T_REKEY]);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->node), &mpeer(k)->addr));
    const unsigned to_alt = m_sent_to(k->alt, PLINK_T_STATUS);
    TEST_ASSERT_EQUAL_UINT(0u, to_alt);
    /* A genuine frame from the new address moves it. */
    k->via = k->alt;
    k_heartbeat(k);
    run(11000);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, k->alt), &mpeer(k)->addr));
    TEST_ASSERT_TRUE(m_sent_to(k->alt, PLINK_T_STATUS) >= 1u);
}

/* ---- time ------------------------------------------------------------------------------------ */

/* LINK-006, LINK-015 (docs/features/11 "Timers"): TIME at each new session, every 10 min, and at
 * once on a clock step or a new time zone. */
static void test_time(void)
{
    static const char tz[] = "CET-1CEST,M3.5.0,M10.5.0/3";
    const int64_t epoch = 1790000000000LL;
    const uint64_t set = NOW;
    plink_master_time(&R.m, NOW, epoch, tz);
    fk_t *k = kp_add();
    pair(k);
    run(50);
    TEST_ASSERT_EQUAL_UINT(1u, k->count[PLINK_T_TIME]);
    plink_time_t t;
    TEST_ASSERT_TRUE(plink_dec_time(k->last[PLINK_T_TIME], k->last_len[PLINK_T_TIME], &t));
    TEST_ASSERT_EQUAL_STRING(tz, t.tz);
    TEST_ASSERT_TRUE(t.epoch_ms >= epoch && t.epoch_ms <= epoch + (int64_t)(NOW - set));
    run(PLINK_TIME_EVERY_MS + 100u);
    TEST_ASSERT_EQUAL_UINT(2u, k->count[PLINK_T_TIME]);
    const int64_t now_epoch = epoch + (int64_t)(NOW - set);
    plink_master_time(&R.m, NOW, now_epoch + 500, tz); /* as expected: nothing */
    run(10);
    TEST_ASSERT_EQUAL_UINT(2u, k->count[PLINK_T_TIME]);
    plink_master_time(&R.m, NOW, now_epoch + 60000, tz); /* a clock step */
    run(10);
    TEST_ASSERT_EQUAL_UINT(3u, k->count[PLINK_T_TIME]);
    plink_master_time(&R.m, NOW, now_epoch + 60000, "UTC0");
    run(10);
    TEST_ASSERT_EQUAL_UINT(4u, k->count[PLINK_T_TIME]);
    TEST_ASSERT_TRUE(plink_dec_time(k->last[PLINK_T_TIME], k->last_len[PLINK_T_TIME], &t));
    TEST_ASSERT_EQUAL_STRING("UTC0", t.tz);
}

/* ---- a rough network ------------------------------------------------------------------------- */

/* LINK-003, LINK-006, LINK-007, LINK-018: two keypads over a network that loses, repeats,
 * reorders and damages frames for 15 minutes, the alarm busy meanwhile: status keeps flowing,
 * nothing is raised under a 5-minute supervision, every command runs once at most, and no side
 * seals two frames under one key and nonce. */
static void test_rough_network(void)
{
    fk_t *a = kp_add(), *b = kp_add();
    a->auto_rekey = b->auto_rekey = true;
    pair(a);
    pair(b);
    /* A damaged frame fails its tag and leaves the window as it was, so the same nonce may be
     * opened again (a duplicate, or a later frame whose seq the damage hit): watch the seals. */
    R.tpm.only_dir = PLINK_DIR_MASTER;
    a->tp.only_dir = b->tp.only_dir = PLINK_DIR_KEYPAD;
    R.net.loss_pm = 50u;
    R.net.dup_pm = 50u;
    R.net.corrupt_pm = 20u;
    R.net.jitter_ms = 40u;
    for (int i = 0; i < 90; i++) {
        at_now();
        if (i % 3 == 0) {
            sim_fault(&R.sim, Z_WINDOW);
        } else if (i % 3 == 1) {
            sim_restore(&R.sim, Z_WINDOW);
        }
        k_cmd(i % 2 ? a : b, 9000u + (uint32_t)i, PLINK_OP_AUTH, ALARM_CMD_ARM, 1u, CODE_USER);
        run(10000);
    }
    R.net.loss_pm = R.net.dup_pm = R.net.corrupt_pm = 0u;
    run(15000);
    TEST_ASSERT_EQUAL_UINT(0u, edges(a->rec.device, false));
    TEST_ASSERT_EQUAL_UINT(0u, edges(b->rec.device, false));
    TEST_ASSERT_TRUE(a->status_at + 11000u >= NOW);
    TEST_ASSERT_TRUE(b->status_at + 11000u >= NOW);
    TEST_ASSERT_TRUE(R.n_runs <= 90u);
    for (unsigned i = 0; i < R.n_runs; i++) {
        for (unsigned j = i + 1u; j < R.n_runs; j++) {
            TEST_ASSERT_FALSE(R.runs[i].cmd.req == R.runs[j].cmd.req &&
                              R.runs[i].device == R.runs[j].device);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pairing_takes_the_lowest_free_number);
    RUN_TEST(test_pairing_needs_a_number_and_a_stored_net_id);
    RUN_TEST(test_hello_with_a_bad_mac_is_not_answered);
    RUN_TEST(test_hello_replayed_after_promotion_is_dropped);
    RUN_TEST(test_hello_repeated_while_pending_gets_the_same_welcome);
    RUN_TEST(test_x25519_budget);
    RUN_TEST(test_pending_session_and_promotion);
    RUN_TEST(test_rekey_hint);
    RUN_TEST(test_master_restart_and_rekey);
    RUN_TEST(test_not_paired_hint);
    RUN_TEST(test_status_every_10_s);
    RUN_TEST(test_status_on_change_is_repeated);
    RUN_TEST(test_status_changes_are_coalesced);
    RUN_TEST(test_zones_pushed_and_requested);
    RUN_TEST(test_small_frames_are_paged);
    RUN_TEST(test_status_split_over_small_frames);
    RUN_TEST(test_configuration_pages);
    RUN_TEST(test_requests_are_rate_limited);
    RUN_TEST(test_events_go_before_their_status);
    RUN_TEST(test_event_requests);
    RUN_TEST(test_sounds_by_home_partition);
    RUN_TEST(test_command_runs_as_the_keypad);
    RUN_TEST(test_command_checks);
    RUN_TEST(test_command_result_cache);
    RUN_TEST(test_one_command_at_a_time_and_the_panic_slot);
    RUN_TEST(test_command_rate_limit);
    RUN_TEST(test_cache_keeps_answers_still_wanted);
    RUN_TEST(test_command_refused_by_the_platform_runs_on_retry);
    RUN_TEST(test_codes_are_not_kept);
    RUN_TEST(test_supervision_not_heard);
    RUN_TEST(test_supervision_stale_status);
    RUN_TEST(test_supervision_paused_while_the_network_is_down);
    RUN_TEST(test_supervision_setting);
    RUN_TEST(test_provisional_keypad_dropped);
    RUN_TEST(test_removal);
    RUN_TEST(test_removal_store_retried);
    RUN_TEST(test_answer_after_removal);
    RUN_TEST(test_pairing_cancelled_at_the_master);
    RUN_TEST(test_hostile_input);
    RUN_TEST(test_clone_note);
    RUN_TEST(test_address_moves_only_on_authenticated_frames);
    RUN_TEST(test_time);
    RUN_TEST(test_rough_network);
    return UNITY_END();
}
