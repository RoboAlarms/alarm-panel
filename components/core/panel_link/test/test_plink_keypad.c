/*
 * test_plink_keypad.c - a keypad's link manager (plink_keypad.h, docs/features/11) against a
 * scripted master over the pretend network (plink_test_net.c) on a fake clock. The master here is
 * not plink_master.c: it is built from the real pieces - pairing (plink_pair_m_t), the handshake
 * answer (plink_hs_answer), sessions (plink_session_*), the message codecs, STATUS from snapshots
 * of the real engine (alarm_sim, plink_status_build) and the keypads' configuration copy
 * (plink_cfg_for_keypads) - with switches for every misbehaviour a test needs.
 *
 * It proves: pairing to the stored record and a handshake at once; the handshake and the
 * tentative session (the old one read until the new one is heard; the first HEARTBEAT lost);
 * the link's states and their timers (CONNECTING until a full sync, LOST at 30 s, back no sooner
 * than 3 s, no-code maintenance at 10 min); LINK-026's ordering of status, partition, zone and
 * time items; the configuration pull through loss, a change mid-transfer and a blob it can't
 * read; the zones asked for when their CRC differs; the events backfilled and the ring's rules;
 * commands (resent, given up at 3 s, refused off-line, request ids unique across a restart) and
 * panic for 60 s through an outage, run once; BYE; NOT_PAIRED hints; REKEY and the handshake's
 * back-off; a new handshake at seq 2^30; the master's address moved only by authenticated frames;
 * and, on both sides' crypto ports, no AES-CCM key and nonce used twice. The HEARTBEAT and CMD
 * payloads are checked against bytes written by hand from the document's tables.
 *
 * Requirements: LINK-002, LINK-003, LINK-006, LINK-008, LINK-014, LINK-015, LINK-016, LINK-017,
 * LINK-018, LINK-021, LINK-023, LINK-025, LINK-026
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_keypad.h"
#include "plink_test_net.h"
#include "plink_test_port.h"
#include "sim.h"
#include "unity.h"

#define NET_ID             0x5A17u
#define DEV                3u
#define HOME               1u
#define ZONES              100u /* the master's zones: a configuration of several pages */
#define PAGE               (PLINK_FRAME_MAX - PLINK_SEALED_OVERHEAD - PLINK_CFG_PAGE_HDR)
#define FM_STATUS_EVERY_MS 10000u
#define HS_PANIC_TEST_MS   2000u /* docs/features/11 "Sessions": 2 s at most while a panic waits */
#define FM_CACHE           16u
#define FM_SEEN            256u
#define EV_PER_FRAME       32u

static const uint8_t IP_M[4] = {192u, 0u, 2u, 10u};
static const uint8_t IP_K[4] = {192u, 0u, 2u, 20u};
static const uint8_t IP_X[4] = {198u, 51u, 100u, 7u};

static const plink_caps_t CAP = {ALARM_MAX_PARTITIONS, ALARM_MAX_ZONES, ALARM_MAX_USERS,
                                 ALARM_MAX_DEVICES};

/* ---- the scripted master ------------------------------------------------------------------ */

typedef struct {
    plink_test_port_t tp;
    plink_crypto_t c;
    bool paired;
    uint8_t k_pair[PLINK_KEY_LEN];
    plink_session_t live, pending;
    uint8_t welcome[128];
    size_t welcome_len;
    uint8_t hello[128];
    size_t hello_len;
    bool have_kp;
    plink_addr_t kp;
    /* how it behaves */
    bool answer_hello, not_paired, rekey, serve_cfg, serve_events, serve_zones, serve_cmds;
    bool auto_status;
    bool bad_blob; /* its keypads' copy is one keypads can't read */
    uint64_t status_due;
    void (*edit)(plink_status_t *st);
    void (*on_page)(void); /* after each configuration page served */
    uint32_t ev_limit;     /* its log holds only events up to this seq (0: all) */
    /* what keypads get */
    uint8_t blob[ALARM_CONFIG_BLOB_MAX];
    size_t blob_len;
    uint32_t blob_crc;
    int64_t epoch_ms;
    uint32_t pushed; /* events pushed up to this seq */
    /* what it saw */
    unsigned hellos, hbs, cfg_reqs, ev_reqs, zones_reqs, cmd_frames, runs, cached, promotions;
    unsigned unknown_sid, pages;
    uint64_t promoted_at;
    plink_ev_req_t ev_req;
    plink_zones_req_t zones_req;
    uint32_t cmd_req[FM_SEEN];
    size_t n_cmd;
    plink_cfg_req_t cfg_seen[FM_SEEN];
    uint64_t cfg_at[FM_SEEN];
    size_t n_cfg;
    struct {
        uint32_t req;
        uint8_t digest[PLINK_SHA256_LEN];
        plink_result_t res;
    } cache[FM_CACHE];
    size_t n_cache;
    uint8_t cmd_pt[64];
    size_t cmd_pt_len;
    uint8_t hb_pt[64];
    size_t hb_pt_len;
    /* pairing */
    plink_pair_m_t pm;
    bool pairing;
    plink_out_t out;
} fm_t;

static sim_t S;
static alarm_config_t SCRATCH;
static alarm_snapshot_t SNAP;
static plink_status_t ST;
static alarm_event_t EVTMP[SIM_MAX_RECORDS];
static fm_t M;

/* ---- the keypad --------------------------------------------------------------------------- */

static plink_keypad_t K;
static plink_test_port_t TPK;
static plink_crypto_t CK;
static plink_net_t NET;
static int KN, MN;
static uint8_t KBUF[ALARM_CONFIG_BLOB_MAX];
static alarm_config_t KCFG;
static plink_keypad_rec_t REC;
static struct {
    bool fail;
    bool have;
    plink_keypad_rec_t rec;
    unsigned writes, erases;
} KS;
static unsigned RESOLVES;

static bool k_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return plink_net_send(&NET, KN, to, frame, len);
}

static bool k_store(void *ctx, const plink_keypad_rec_t *rec)
{
    (void)ctx;
    if (KS.fail) {
        return false;
    }
    if (rec != NULL) {
        KS.rec = *rec;
        KS.have = true;
        KS.writes++;
    } else {
        memset(&KS.rec, 0, sizeof KS.rec);
        KS.have = false;
        KS.erases++;
    }
    return true;
}

static void k_resolve(void *ctx)
{
    (void)ctx;
    RESOLVES++;
}

static void k_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t n)
{
    (void)ctx;
    plink_keypad_input(&K, now, from, f, n);
}

static void k_tick(void *ctx, uint64_t now)
{
    (void)ctx;
    plink_keypad_tick(&K, now);
}

static uint64_t k_next(void *ctx)
{
    (void)ctx;
    return plink_keypad_next(&K);
}

/* ---- the master's pieces ------------------------------------------------------------------ */

static void fm_blob(void)
{
    size_t len = 0;
    uint32_t crc = 0;
    TEST_ASSERT_EQUAL_INT(
        ALARM_OK, plink_cfg_for_keypads(&S.cfg, &SCRATCH, M.blob, sizeof M.blob, &len, &crc));
    if (M.bad_blob) {
        M.blob[5] = ALARM_CONFIG_BLOB_COMPAT + 1u; /* a newer compat than this build reads */
        crc = alarm_crc32(0u, M.blob, len);
    }
    M.blob_len = len;
    M.blob_crc = crc;
}

static void fm_plain_net(const plink_addr_t *to, uint8_t type, uint16_t net_id,
                         const uint8_t *payload, size_t len)
{
    uint8_t f[PLINK_FRAME_MAX];
    size_t n = 0;
    const plink_hdr_t h = {type, PLINK_FLAG_FROM_MASTER, net_id, PLINK_SENDER_MASTER, 0u, 0u};
    if (len > 0u) {
        memcpy(&f[PLINK_HDR_LEN], payload, len);
    }
    TEST_ASSERT_TRUE(plink_frame_plain(&h, &f[PLINK_HDR_LEN], len, f, sizeof f, &n));
    plink_net_send(&NET, MN, to, f, n);
}

static void fm_rekey(uint32_t sid, uint16_t net_id)
{
    const plink_rekey_t r = {sid};
    uint8_t p[8];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_rekey(&r, p, sizeof p, &n));
    fm_plain_net(plink_net_addr(&NET, KN), PLINK_T_REKEY, net_id, p, n);
}

/* A sealed frame of the master's live session into out (not sent). */
static size_t fm_seal(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out)
{
    size_t n = 0;
    TEST_ASSERT_TRUE(M.live.valid);
    TEST_ASSERT_TRUE(plink_session_seal(&M.c, &M.live, type, NET_ID, PLINK_SENDER_MASTER, true,
                                        payload, len, out, PLINK_FRAME_MAX, &n));
    return n;
}

static void fm_send_sealed(uint8_t type, const uint8_t *payload, size_t len)
{
    if (!M.live.valid || !M.have_kp) {
        return;
    }
    uint8_t f[PLINK_FRAME_MAX];
    const size_t n = fm_seal(type, payload, len, f);
    plink_net_send(&NET, MN, &M.kp, f, n);
}

/* The STATUS of the engine's snapshot now (with the test's edit). */
static size_t fm_status_payload(uint8_t *p, size_t cap)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    const plink_status_extra_t x = {M.epoch_ms,          HOME,       0u,
                                    PLINK_REPORT_ONLINE, M.blob_crc, (uint32_t)M.blob_len};
    plink_status_build(&SNAP, SNAP.taken_ms, &x, &ST);
    if (M.edit != NULL) {
        M.edit(&ST);
    }
    size_t n = 0;
    uint8_t written = 0;
    TEST_ASSERT_TRUE(plink_enc_status(&ST, 0u, p, cap, &n, &written));
    return n;
}

static void fm_status(void)
{
    uint8_t p[PLINK_PAYLOAD_MAX];
    const size_t n = fm_status_payload(p, sizeof p);
    fm_send_sealed(PLINK_T_STATUS, p, n);
}

static void fm_time(int64_t epoch_ms, const char *tz)
{
    plink_time_t t;
    memset(&t, 0, sizeof t);
    t.epoch_ms = epoch_ms;
    snprintf(t.tz, sizeof t.tz, "%s", tz);
    uint8_t p[96];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_time(&t, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_TIME, p, n);
}

/* The engine's events after `after` (its log as limited by ev_limit), the newest max. */
static size_t fm_events_after(uint32_t after, alarm_event_t *out, size_t max, bool *gap)
{
    size_t n = 0;
    for (unsigned i = 0; i < S.n_events; i++) {
        const alarm_event_t *e = &S.events[i];
        if (e->seq > after && (M.ev_limit == 0u || e->seq <= M.ev_limit)) {
            EVTMP[n++] = *e;
        }
    }
    const size_t k = n < max ? n : max;
    *gap = k < n;
    memcpy(out, &EVTMP[n - k], k * sizeof *out);
    return k;
}

/* Events as EVENTS frames of at most 32, oldest first. */
static void fm_send_events(const alarm_event_t *ev, size_t n, bool gap)
{
    size_t at = 0;
    do {
        const size_t chunk = n - at < EV_PER_FRAME ? n - at : EV_PER_FRAME;
        uint8_t p[PLINK_PAYLOAD_MAX];
        size_t len = 0, written = 0;
        TEST_ASSERT_TRUE(plink_enc_events(&ev[at], chunk, gap && at == 0u ? PLINK_EVF_GAP : 0u, p,
                                          sizeof p, &len, &written));
        TEST_ASSERT_EQUAL_size_t(chunk, written);
        fm_send_sealed(PLINK_T_EVENTS, p, len);
        at += chunk;
    } while (at < n);
}

/* Each new event pushed before the STATUS that carries its seq. */
static void fm_push_events(void)
{
    static alarm_event_t ev[SIM_MAX_RECORDS];
    bool gap = false;
    const size_t n = fm_events_after(M.pushed, ev, SIM_MAX_RECORDS, &gap);
    if (n > 0u) {
        fm_send_events(ev, n, false);
        M.pushed = ev[n - 1u].seq;
    }
}

static void fm_result(const plink_result_t *res)
{
    uint8_t p[32];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_result(res, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_RESULT, p, n);
}

/* A CMD: answered from the cache for a repeat (the same req and digest), otherwise run on the
 * engine as {ALARM_SRC_KEYPAD, DEV}. */
static void fm_cmd(const uint8_t *pt, size_t n)
{
    M.cmd_frames++;
    M.cmd_pt_len = n < sizeof M.cmd_pt ? n : sizeof M.cmd_pt;
    memcpy(M.cmd_pt, pt, M.cmd_pt_len);
    plink_cmd_t cmd;
    if (!plink_dec_cmd(pt, n, &cmd)) {
        return;
    }
    if (M.n_cmd < FM_SEEN) {
        M.cmd_req[M.n_cmd++] = cmd.req;
    }
    if (!M.serve_cmds) {
        return;
    }
    uint8_t dg[PLINK_SHA256_LEN];
    plink_cmd_digest(&cmd, dg);
    for (size_t i = 0; i < M.n_cache; i++) {
        if (M.cache[i].req == cmd.req && memcmp(M.cache[i].digest, dg, sizeof dg) == 0) {
            M.cached++;
            fm_result(&M.cache[i].res);
            return;
        }
    }
    plink_result_t res;
    memset(&res, 0, sizeof res);
    res.req = cmd.req;
    res.result = ALARM_OK;
    if (cmd.op == PLINK_OP_COMMAND) {
        S.keypad.kind = ALARM_SRC_KEYPAD;
        S.keypad.id = DEV;
        res.result = (uint8_t)sim_cmd(&S, &cmd.cmd);
        res.user = S.last.user;
        res.not_ready_zone = S.last.not_ready_zone;
        res.may_disarm = S.last.may_disarm ? 1u : 0u;
    }
    M.runs++;
    const size_t slot = M.n_cache < FM_CACHE ? M.n_cache++ : (size_t)(M.runs % FM_CACHE);
    M.cache[slot].req = cmd.req;
    memcpy(M.cache[slot].digest, dg, sizeof dg);
    M.cache[slot].res = res;
    plink_wipe(&cmd, sizeof cmd);
    fm_result(&res);
}

static void fm_cfg_req(uint64_t now, const uint8_t *pt, size_t n)
{
    plink_cfg_req_t q;
    if (!plink_dec_cfg_req(pt, n, &q)) {
        return;
    }
    M.cfg_reqs++;
    if (M.n_cfg < FM_SEEN) {
        M.cfg_seen[M.n_cfg] = q;
        M.cfg_at[M.n_cfg++] = now;
    }
    if (!M.serve_cfg || q.cfg_crc != M.blob_crc || q.offset >= M.blob_len) {
        return;
    }
    size_t len = M.blob_len - q.offset;
    if (len > q.max) {
        len = q.max;
    }
    if (len > PLINK_PAYLOAD_MAX - PLINK_CFG_PAGE_HDR) {
        len = PLINK_PAYLOAD_MAX - PLINK_CFG_PAGE_HDR;
    }
    const plink_cfg_page_t pg = {M.blob_crc, (uint32_t)M.blob_len, q.offset, &M.blob[q.offset],
                                 (uint16_t)len};
    uint8_t p[PLINK_PAYLOAD_MAX];
    size_t pl = 0;
    TEST_ASSERT_TRUE(plink_enc_cfg_page(&pg, p, sizeof p, &pl));
    fm_send_sealed(PLINK_T_CFG_PAGE, p, pl);
    M.pages++;
    if (M.on_page != NULL) {
        M.on_page();
    }
}

static void fm_zones(uint16_t first, uint16_t count)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    const uint32_t crc = plink_zones_crc(SNAP.zone, SNAP.zones_used);
    uint16_t at = 0;
    while (at < count && first + at <= SNAP.zones_used) {
        uint8_t p[PLINK_PAYLOAD_MAX];
        size_t len = 0;
        uint16_t written = 0;
        const uint16_t z = (uint16_t)(first + at);
        TEST_ASSERT_TRUE(plink_enc_zones(crc, z, &SNAP.zone[z - 1u], (uint16_t)(count - at), p,
                                         sizeof p, &len, &written));
        fm_send_sealed(PLINK_T_ZONES, p, len);
        at = (uint16_t)(at + written);
    }
}

static void fm_hello(const plink_addr_t *from, const plink_hdr_t *h, const uint8_t *f, size_t len)
{
    M.hellos++;
    if (M.not_paired) {
        fm_plain_net(from, PLINK_T_NOT_PAIRED, NET_ID, NULL, 0u);
        return;
    }
    if (!M.paired || !M.answer_hello || h->net_id != NET_ID || h->sender != DEV) {
        return;
    }
    if (M.pending.valid && len == M.hello_len && memcmp(f, M.hello, len) == 0) {
        plink_net_send(&NET, MN, from, M.welcome, M.welcome_len);
        return;
    }
    plink_session_t pend;
    plink_hello_t hello;
    if (!plink_hs_answer(&M.c, M.k_pair, h, f, len, &pend, &hello, M.welcome, sizeof M.welcome,
                         &M.welcome_len)) {
        return;
    }
    plink_session_wipe(&M.pending);
    M.pending = pend;
    plink_session_wipe(&pend);
    memcpy(M.hello, f, len);
    M.hello_len = len;
    plink_net_send(&NET, MN, from, M.welcome, M.welcome_len);
}

static void fm_sealed(uint64_t now, const plink_addr_t *from, const plink_hdr_t *h,
                      const uint8_t *f, size_t len)
{
    plink_session_t *s = NULL;
    bool promote = false;
    if (M.live.valid && h->sid == M.live.sid) {
        s = &M.live;
    } else if (M.pending.valid && h->sid == M.pending.sid) {
        s = &M.pending;
        promote = true;
    }
    if (s == NULL) {
        M.unknown_sid++;
        if (M.rekey) {
            const plink_rekey_t r = {h->sid};
            uint8_t p[8];
            size_t n = 0;
            TEST_ASSERT_TRUE(plink_enc_rekey(&r, p, sizeof p, &n));
            fm_plain_net(from, PLINK_T_REKEY, NET_ID, p, n);
        }
        return;
    }
    uint8_t pt[PLINK_FRAME_MAX];
    size_t n = 0;
    if (h->net_id != NET_ID || h->sender != DEV ||
        !plink_session_open(&M.c, s, h, f, len, pt, sizeof pt, &n)) {
        return;
    }
    M.kp = *from;
    M.have_kp = true;
    if (promote) {
        plink_session_wipe(&M.live);
        M.live = M.pending;
        memset(&M.pending, 0, sizeof M.pending);
        M.promotions++;
        M.promoted_at = now;
        fm_status();
        fm_time(M.epoch_ms, "CET-1CEST,M3.5.0,M10.5.0/3");
        M.status_due = now + FM_STATUS_EVERY_MS;
    }
    switch (h->type) {
    case PLINK_T_HEARTBEAT:
        M.hbs++;
        M.hb_pt_len = n < sizeof M.hb_pt ? n : sizeof M.hb_pt;
        memcpy(M.hb_pt, pt, M.hb_pt_len);
        break;
    case PLINK_T_CFG_REQ: fm_cfg_req(now, pt, n); break;
    case PLINK_T_EV_REQ:
        M.ev_reqs++;
        if (plink_dec_ev_req(pt, n, &M.ev_req) && M.serve_events) {
            static alarm_event_t ev[SIM_MAX_RECORDS];
            bool gap = false;
            const size_t k = fm_events_after(M.ev_req.after_seq, ev, M.ev_req.max, &gap);
            fm_send_events(ev, k, gap);
        }
        break;
    case PLINK_T_ZONES_REQ:
        M.zones_reqs++;
        if (plink_dec_zones_req(pt, n, &M.zones_req) && M.serve_zones) {
            fm_zones(M.zones_req.first, M.zones_req.count);
        }
        break;
    case PLINK_T_CMD: fm_cmd(pt, n); break;
    default: break;
    }
}

static void fm_out(void)
{
    if (M.out.len > 0u) {
        plink_net_send(&NET, MN, &M.out.to, M.out.buf, M.out.len);
    }
    M.out.len = 0u;
}

static void fm_pair_take(uint64_t now)
{
    plink_pair_new_t nk;
    if (!plink_pm_take(&M.pm, &nk)) {
        return;
    }
    memcpy(M.k_pair, nk.k_pair, sizeof M.k_pair);
    M.paired = true;
    plink_pm_stored(&M.pm, now, true, PLINK_FAIL_NONE, DEV, HOME, &M.out);
    fm_out();
}

static void fm_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t n)
{
    (void)ctx;
    plink_hdr_t h;
    if (!plink_hdr_read(f, n, &h) || (h.flags & PLINK_FLAG_FROM_MASTER) != 0u) {
        return;
    }
    if (h.type >= PLINK_T_PAIR_REQ && h.type <= PLINK_T_PAIR_FAIL) {
        if (M.pairing) {
            plink_pm_input(&M.pm, now, from, &h, f, n, &M.out);
            fm_out();
            fm_pair_take(now);
        }
    } else if (h.type == PLINK_T_HELLO) {
        fm_hello(from, &h, f, n);
    } else if ((h.flags & PLINK_FLAG_SEALED) != 0u) {
        fm_sealed(now, from, &h, f, n);
    }
}

static void fm_tick(void *ctx, uint64_t now)
{
    (void)ctx;
    if (M.pairing) {
        plink_pm_tick(&M.pm, now, &M.out);
        fm_out();
        fm_pair_take(now);
    }
    if (M.auto_status && M.live.valid && M.have_kp && now >= M.status_due) {
        fm_status();
        M.status_due = now + FM_STATUS_EVERY_MS;
    }
}

static uint64_t fm_next(void *ctx)
{
    (void)ctx;
    uint64_t t = UINT64_MAX;
    if (M.pairing) {
        t = plink_pm_next(&M.pm);
    }
    if (M.auto_status && M.live.valid && M.have_kp && M.status_due < t) {
        t = M.status_due;
    }
    return t;
}

/* The master restarted: its sessions are gone (the keypad's frames now meet an unknown sid). */
static void fm_restart(void)
{
    plink_session_wipe(&M.live);
    plink_session_wipe(&M.pending);
    M.have_kp = false;
}

/* ---- set-up ------------------------------------------------------------------------------- */

static void master_config(alarm_config_t *cfg)
{
    sim_config_basic(cfg);
    for (uint16_t z = 21u; z <= ZONES; z++) {
        sim_set_zone(cfg, z, ALARM_ZT_PERIMETER, 1u);
    }
    for (uint16_t z = 1u; z <= ZONES; z++) {
        snprintf(cfg->zones[z - 1u].name, ALARM_NAME_LEN, "Zone %u, north garden window", z);
    }
}

void setUp(void)
{
    static alarm_config_t cfg;
    plink_net_init(&NET, 0x11EEu);
    plink_test_port_init(&TPK, 0xC0DEu);
    CK = plink_test_port(&TPK);
    memset(&M, 0, sizeof M);
    plink_test_port_init(&M.tp, 0xBEEFu);
    M.c = plink_test_port(&M.tp);
    M.answer_hello = M.rekey = M.serve_cfg = M.serve_events = M.serve_zones = M.serve_cmds = true;
    M.auto_status = true;
    M.epoch_ms = 1790000000000LL;
    master_config(&cfg);
    sim_start(&S, &cfg, NULL);
    fm_blob();
    for (size_t i = 0; i < sizeof M.k_pair; i++) {
        M.k_pair[i] = (uint8_t)(0x40u + i);
    }
    M.paired = true;
    MN = plink_net_node(&NET, IP_M, &M, fm_input, fm_tick, fm_next);
    KN = plink_net_node(&NET, IP_K, &K, k_input, k_tick, k_next);
    memset(&KS, 0, sizeof KS);
    memset(&K, 0, sizeof K);
    memset(&KCFG, 0, sizeof KCFG);
    RESOLVES = 0;
}

void tearDown(void)
{
    /* LINK-003: no AES-CCM key and nonce was ever used twice, on either side. */
    TEST_ASSERT_FALSE_MESSAGE(TPK.reused, "the keypad used a key and nonce twice");
    TEST_ASSERT_FALSE_MESSAGE(M.tp.reused, "the master used a key and nonce twice");
}

static void kp_init(bool paired)
{
    memset(&REC, 0, sizeof REC);
    REC.net_id = NET_ID;
    REC.device = DEV;
    REC.home = HOME;
    memcpy(REC.k_pair, M.k_pair, sizeof REC.k_pair);
    snprintf(REC.master_name, sizeof REC.master_name, "Harbor House");
    snprintf(REC.panel_id, sizeof REC.panel_id, "A1B2C3");
    const plink_keypad_port_t port = {
        .ctx = NULL, .send = k_send, .mtu = NULL, .store = k_store, .resolve = k_resolve};
    plink_keypad_init(&K, &CK, &port, paired ? &REC : NULL, KBUF, sizeof KBUF, &KCFG,
                      PLINK_KIND_PANEL, &CAP, "Hall keypad", "1.2.3", NET.now_ms);
    if (paired) {
        plink_keypad_master_addr(&K, NET.now_ms, plink_net_addr(&NET, MN));
    }
}

static plink_keypad_info_t info(void)
{
    plink_keypad_info_t i;
    plink_keypad_info(&K, NET.now_ms, &i);
    return i;
}

static bool master_is(const plink_addr_t *a)
{
    const plink_keypad_info_t i = info();
    return plink_addr_equal(a, &i.master);
}

static bool online(void *ctx)
{
    (void)ctx;
    return info().state == PLINK_LINK_ONLINE;
}

static bool have_status(void *ctx)
{
    (void)ctx;
    return K.view.have_status;
}

static void kp_online(void)
{
    kp_init(true);
    TEST_ASSERT_TRUE_MESSAGE(plink_net_run_until(&NET, 5000, online, NULL), "not online");
}

static void run_to(uint64_t t)
{
    if (t > NET.now_ms) {
        plink_net_run(&NET, t - NET.now_ms);
    }
}

static void deliver(const uint8_t *f, size_t n)
{
    plink_keypad_input(&K, NET.now_ms, plink_net_addr(&NET, MN), f, n);
}

/* The times of the datagrams of one type a node sent (lost or not), into at; how many. */
static size_t sent_times(int from, uint8_t type, uint64_t *at, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < NET.log_count && n < max; i++) {
        if (NET.log[i].from == from && NET.log[i].type == type) {
            at[n++] = NET.log[i].at_ms;
        }
    }
    return n;
}

static plink_cmd_t arm_cmd(const char *code)
{
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = PLINK_OP_COMMAND;
    c.cmd.type = ALARM_CMD_ARM;
    c.cmd.partition = 1u;
    c.cmd.level = ALARM_LEVEL_AWAY;
    snprintf(c.cmd.code, sizeof c.cmd.code, "%s", code);
    return c;
}

static plink_cmd_t panic_cmd(void)
{
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = PLINK_OP_COMMAND;
    c.cmd.type = ALARM_CMD_PANIC;
    c.cmd.partition = 1u;
    c.cmd.panic = ALARM_PANIC_POLICE;
    return c;
}

static const plink_kcmd_t *slot_of(uint32_t req)
{
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        if (K.cmds[i].used && K.cmds[i].req == req) {
            return &K.cmds[i];
        }
    }
    return NULL;
}

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* ---- pairing ------------------------------------------------------------------------------ */

static bool both_compare(void *ctx)
{
    (void)ctx;
    plink_pair_info_t a, b;
    plink_keypad_pair_info(&K, NET.now_ms, &a);
    plink_pm_info(&M.pm, NET.now_ms, &b);
    return a.state == PLINK_PS_COMPARE && b.state == PLINK_PS_COMPARE;
}

static bool stored_twice(void *ctx)
{
    (void)ctx;
    return KS.writes >= 2u;
}

static void pair_once(void)
{
    plink_pm_open(&M.pm, NET.now_ms);
    M.pairing = true;
    plink_keypad_pair_start(&K, NET.now_ms, plink_net_addr(&NET, MN), NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, both_compare, NULL));
    plink_pair_info_t a, b;
    plink_keypad_pair_info(&K, NET.now_ms, &a);
    plink_pm_info(&M.pm, NET.now_ms, &b);
    TEST_ASSERT_EQUAL_STRING(b.code, a.code);
    TEST_ASSERT_EQUAL_UINT(6u, strlen(a.code));
    plink_pm_decide(&M.pm, NET.now_ms, true, &M.out);
    fm_out();
    plink_keypad_pair_confirm(&K, NET.now_ms, true);
}

/* LINK-016, LINK-004: a keypad pairs only when a person starts it; nothing is stored before both
 * confirmations; the record PAIR_DONE gives is stored and the first HELLO goes at once; a keypad
 * that pairs again drops what the earlier pairing gave it (its event ring included). */
static void test_pairing_stores_the_record(void)
{
    M.paired = false;
    memset(M.k_pair, 0, sizeof M.k_pair);
    plink_pm_init(&M.pm, &M.c);
    plink_pm_identity(&M.pm, NET_ID, "Harbor House", "A1B2C3", ZONES);
    kp_init(false);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_NEVER, info().unpaired_why);
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_size_t(0u, NET.log_count); /* nothing until a person pairs */
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_keypad_next(&K));

    pair_once();
    TEST_ASSERT_EQUAL_UINT(0u, KS.writes); /* not before both MACs verified */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, KS.writes);
    TEST_ASSERT_TRUE(KS.have);
    TEST_ASSERT_EQUAL_HEX16(NET_ID, KS.rec.net_id);
    TEST_ASSERT_EQUAL_UINT8(DEV, KS.rec.device);
    TEST_ASSERT_EQUAL_UINT8(HOME, KS.rec.home);
    TEST_ASSERT_EQUAL_MEMORY(M.k_pair, KS.rec.k_pair, PLINK_KEY_LEN);
    TEST_ASSERT_EQUAL_STRING("Harbor House", KS.rec.master_name);
    TEST_ASSERT_EQUAL_STRING("A1B2C3", KS.rec.panel_id);
    /* The HELLO followed the PAIR_DONE at once. */
    uint64_t done[4], hello[4];
    TEST_ASSERT_EQUAL_size_t(1u, sent_times(MN, PLINK_T_PAIR_DONE, done, 4));
    TEST_ASSERT_TRUE(sent_times(KN, PLINK_T_HELLO, hello, 4) >= 1u);
    TEST_ASSERT_UINT64_WITHIN(NET.delay_ms, done[0] + NET.delay_ms, hello[0]);
    const plink_keypad_info_t i = info();
    TEST_ASSERT_EQUAL_UINT8(DEV, i.device);
    TEST_ASSERT_EQUAL_UINT8(HOME, i.home);
    TEST_ASSERT_EQUAL_STRING("Harbor House", i.master_name);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&NET, MN), &i.master));
    TEST_ASSERT_TRUE(K.ev_count > 0u); /* the events were backfilled */

    /* Pair again: the link stays up until the new pairing completes, then everything of the
     * earlier one goes - the event ring first of all - and the new keys connect. */
    uint8_t old_key[PLINK_KEY_LEN];
    memcpy(old_key, M.k_pair, sizeof old_key);
    pair_once();
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, stored_twice, NULL));
    TEST_ASSERT_EQUAL_size_t(0u, K.ev_count);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_CONNECTING, info().state);
    TEST_ASSERT_FALSE(memcmp(old_key, KS.rec.k_pair, PLINK_KEY_LEN) == 0);
    TEST_ASSERT_EQUAL_MEMORY(M.k_pair, KS.rec.k_pair, PLINK_KEY_LEN);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
}

/* ---- the handshake ------------------------------------------------------------------------ */

/* LINK-015: a handshake at start-up; the new session is tentative and HEARTBEATs go every 500 ms
 * until the master is heard on it, so with the first HEARTBEAT lost the STATUS still comes within
 * 1 s of the WELCOME. */
static void test_handshake_first_heartbeat_lost(void)
{
    NET.lose_type = PLINK_T_HEARTBEAT;
    NET.lose_type_count = 1u;
    kp_init(true);
    plink_net_run(&NET, 5);
    TEST_ASSERT_EQUAL_UINT(1u, M.hellos);
    TEST_ASSERT_TRUE(K.tentative);
    TEST_ASSERT_TRUE(K.live.valid);
    TEST_ASSERT_FALSE(K.old.valid);
    uint64_t welcome[2];
    TEST_ASSERT_EQUAL_size_t(1u, sent_times(MN, PLINK_T_WELCOME, welcome, 2));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 1000, have_status, NULL));
    TEST_ASSERT_TRUE(NET.now_ms - welcome[0] < 1000u);
    TEST_ASSERT_FALSE(K.tentative);
    TEST_ASSERT_EQUAL_UINT(1u, M.promotions);
    uint64_t hb[4];
    TEST_ASSERT_TRUE(sent_times(KN, PLINK_T_HEARTBEAT, hb, 4) >= 2u);
    TEST_ASSERT_EQUAL_UINT64(PLINK_TENTATIVE_HB_MS, hb[1] - hb[0]);
    TEST_ASSERT_TRUE(NET.log[0].type == PLINK_T_HELLO && NET.log[0].from == KN);
}

/* LINK-015: during a new handshake the old session is still read until a frame of the new one
 * arrives; a forged or stale WELCOME never replaces the live session; once the master is heard on
 * the new session the old one is gone and its frames are refused. */
static void test_old_session_until_new_heard(void)
{
    kp_online();
    M.auto_status = false;
    const uint32_t old_sid = K.live.sid;
    NET.lose_type = PLINK_T_HEARTBEAT; /* the master can't promote the new session yet */
    uint8_t stale[PLINK_FRAME_MAX];
    size_t stale_len = 0;
    TEST_ASSERT_TRUE(plink_net_last(&NET, MN, PLINK_T_WELCOME, stale, sizeof stale, &stale_len));

    /* A REKEY hint: a HELLO, answered; the WELCOME is held on the way. A forged copy of it and
     * the answer to an earlier HELLO leave the live session as it is. */
    fm_rekey(old_sid, NET_ID);
    plink_net_run(&NET, 2u * NET.delay_ms);
    TEST_ASSERT_TRUE(K.hs.waiting);
    TEST_ASSERT_EQUAL_UINT(2u, M.hellos);
    NET.hold = true;
    uint8_t forged[PLINK_FRAME_MAX];
    size_t forged_len = 0;
    TEST_ASSERT_TRUE(plink_net_last(&NET, MN, PLINK_T_WELCOME, forged, sizeof forged, &forged_len));
    TEST_ASSERT_FALSE(forged_len == stale_len && memcmp(forged, stale, stale_len) == 0);
    forged[forged_len - 1u] ^= 0x01u; /* mac2 */
    deliver(forged, forged_len);
    deliver(stale, stale_len);
    TEST_ASSERT_EQUAL_UINT32(old_sid, K.live.sid);
    TEST_ASSERT_FALSE(K.tentative);
    TEST_ASSERT_TRUE(K.hs.waiting);
    plink_net_release(&NET);
    plink_net_run(&NET, 5);
    TEST_ASSERT_TRUE(K.tentative);
    TEST_ASSERT_EQUAL_UINT32(old_sid, K.old.sid);
    TEST_ASSERT_NOT_EQUAL(old_sid, K.live.sid);

    /* The master still uses the old session: its STATUS is taken. */
    const uint32_t seq0 = plink_keypad_seq(&K);
    fm_status();
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT32(seq0 + 1u, plink_keypad_seq(&K));
    /* HEARTBEATs every 500 ms while tentative. */
    const size_t hb0 = plink_net_count(&NET, KN, PLINK_T_HEARTBEAT);
    plink_net_run(&NET, 2000);
    TEST_ASSERT_EQUAL_size_t(hb0 + 4u, plink_net_count(&NET, KN, PLINK_T_HEARTBEAT));

    /* They get through: the master promotes the new session, the old one goes. */
    plink_session_t old_m = M.live;
    NET.lose_type = 0u;
    plink_net_run(&NET, 600);
    TEST_ASSERT_FALSE(K.tentative);
    TEST_ASSERT_FALSE(K.old.valid);
    TEST_ASSERT_EQUAL_UINT32(M.live.sid, K.live.sid);
    TEST_ASSERT_EQUAL_UINT(2u, M.promotions);
    /* A frame of the old session now is refused; heartbeats are back to every 10 s. */
    uint8_t p[PLINK_PAYLOAD_MAX], f[PLINK_FRAME_MAX];
    const size_t pl = fm_status_payload(p, sizeof p);
    size_t fl = 0;
    TEST_ASSERT_TRUE(plink_session_seal(&M.c, &old_m, PLINK_T_STATUS, NET_ID, PLINK_SENDER_MASTER,
                                        true, p, pl, f, sizeof f, &fl));
    const uint32_t seq1 = plink_keypad_seq(&K);
    deliver(f, fl);
    TEST_ASSERT_EQUAL_UINT32(seq1, plink_keypad_seq(&K));
    plink_session_wipe(&old_m);
    const size_t hb1 = plink_net_count(&NET, KN, PLINK_T_HEARTBEAT);
    plink_net_run(&NET, 8000);
    TEST_ASSERT_EQUAL_size_t(hb1, plink_net_count(&NET, KN, PLINK_T_HEARTBEAT));
}

/* ---- the link's state --------------------------------------------------------------------- */

/* LINK-008: CONNECTING, steady, until a session, a STATUS and the configuration it names are all
 * in; commands other than panic are refused meanwhile. */
static void test_connecting_until_full_sync(void)
{
    M.serve_cfg = false;
    kp_init(true);
    plink_net_run(&NET, 10000);
    plink_keypad_info_t i = info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_CONNECTING, i.state);
    TEST_ASSERT_EQUAL_UINT32(10u, i.since_s);
    TEST_ASSERT_TRUE(K.live.valid);
    TEST_ASSERT_TRUE(K.view.have_status);
    TEST_ASSERT_NULL(plink_keypad_config(&K));
    TEST_ASSERT_TRUE(i.status_age_s < 10u);
    const plink_cmd_t c = arm_cmd("1111");
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_command(&K, NET.now_ms, &c));
    M.serve_cfg = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_PULL_RETRY_MAX_MS + 100u, online, NULL));
    TEST_ASSERT_NOT_NULL(plink_keypad_config(&K));
    TEST_ASSERT_EQUAL_UINT32(1u, plink_keypad_config_seq(&K));
    TEST_ASSERT_EQUAL_UINT32(M.blob_crc, K.cfg_installed_crc);
    TEST_ASSERT_EQUAL_UINT32(0u, info().since_s);
}

/* LINK-008: LOST 30 s after the last authenticated STATUS, refusing commands; back to ONLINE only
 * after a status since and no sooner than 3 s after the loss. */
static void test_lost_at_30s_back_after_3s(void)
{
    kp_online();
    NET.cut_from = MN; /* nothing from the master arrives */
    const uint64_t last = K.status_ms;
    run_to(last + PLINK_LOST_MS - 1u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    run_to(last + PLINK_LOST_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_LOST, info().state);
    TEST_ASSERT_EQUAL_UINT32(0u, info().since_s);
    TEST_ASSERT_EQUAL_UINT32(30u, info().status_age_s);
    const plink_cmd_t c = arm_cmd("1111");
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_command(&K, NET.now_ms, &c));

    /* Nothing new from the master: still LOST after 3 s, whatever it held before. */
    const uint64_t lost = NET.now_ms;
    run_to(lost + PLINK_LOST_MIN_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_LOST, info().state);
    /* A status comes: back at once, the 3 s being over. */
    NET.cut_from = -1;
    fm_status();
    plink_net_run(&NET, 2u * NET.delay_ms);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);

    /* Lost again, and the master back at once: the keypad waits out its 3 s. */
    plink_net_run(&NET, 2000);
    NET.cut_from = MN;
    const uint64_t last2 = K.status_ms;
    run_to(last2 + PLINK_LOST_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_LOST, info().state);
    const uint64_t lost2 = NET.now_ms;
    NET.cut_from = -1;
    fm_status();
    run_to(lost2 + PLINK_LOST_MIN_MS - 1u);
    TEST_ASSERT_TRUE(K.status_ms >= lost2);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_LOST, info().state);
    run_to(lost2 + PLINK_LOST_MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
}

/* LINK-025: no-code maintenance after 10 min without system communication - from the last status,
 * or from start-up when no master ever answered - and not a moment before. */
static void test_maintenance_after_10_min(void)
{
    kp_online();
    NET.cut_from = MN;
    NET.cut_to = MN;
    const uint64_t last = K.status_ms;
    run_to(last + PLINK_MAINTENANCE_MS - 1u);
    TEST_ASSERT_FALSE(info().maintenance);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_LOST, info().state);
    run_to(last + PLINK_MAINTENANCE_MS);
    TEST_ASSERT_TRUE(info().maintenance);
    NET.cut_from = NET.cut_to = -1;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_BACKOFF_MAX_MS + 5000u, online, NULL));
    TEST_ASSERT_FALSE(info().maintenance);

    /* From start-up with no master answering. */
    NET.cut_from = NET.cut_to = MN;
    const uint64_t start = NET.now_ms;
    kp_init(true);
    run_to(start + PLINK_MAINTENANCE_MS - 1u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_CONNECTING, info().state);
    TEST_ASSERT_FALSE(info().maintenance);
    run_to(start + PLINK_MAINTENANCE_MS);
    TEST_ASSERT_TRUE(info().maintenance);
}

/* ---- LINK-026: frame order per item ------------------------------------------------------- */

static void part_rec(plink_part_t *r, uint8_t number, uint8_t state)
{
    memset(r, 0, sizeof *r);
    r->number = number;
    r->state = state;
    r->level = ALARM_LEVEL_AWAY;
    r->flags = PLINK_PF_READY;
}

/* A STATUS frame (sealed now, not sent) from the engine's snapshot with these system troubles,
 * partitions 1 and 2 enabled, and the records given. */
static size_t status_frame(uint32_t troubles, const plink_part_t *recs, uint8_t count, uint8_t *f)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    const plink_status_extra_t x = {M.epoch_ms,          HOME,       0u,
                                    PLINK_REPORT_ONLINE, M.blob_crc, (uint32_t)M.blob_len};
    plink_status_build(&SNAP, SNAP.taken_ms, &x, &ST);
    ST.sys_troubles = troubles;
    ST.partitions = 0x03u;
    ST.count = count;
    memcpy(ST.part, recs, count * sizeof recs[0]);
    uint8_t p[PLINK_PAYLOAD_MAX];
    size_t n = 0;
    uint8_t w = 0;
    TEST_ASSERT_TRUE(plink_enc_status(&ST, 0u, p, sizeof p, &n, &w));
    return fm_seal(PLINK_T_STATUS, p, n, f);
}

static size_t zones_frame(uint16_t first, const uint16_t *flags, uint16_t count, uint8_t *f)
{
    alarm_zone_status_t z[4];
    memset(z, 0, sizeof z);
    for (uint16_t i = 0; i < count; i++) {
        z[i].flags = flags[i];
    }
    uint8_t p[64];
    size_t n = 0;
    uint16_t w = 0;
    TEST_ASSERT_TRUE(plink_enc_zones(0u, first, z, count, p, sizeof p, &n, &w));
    return fm_seal(PLINK_T_ZONES, p, n, f);
}

static size_t time_frame(int64_t epoch, uint8_t *f)
{
    plink_time_t t;
    memset(&t, 0, sizeof t);
    t.epoch_ms = epoch;
    snprintf(t.tz, sizeof t.tz, "UTC0");
    uint8_t p[96];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_time(&t, p, sizeof p, &n));
    return fm_seal(PLINK_T_TIME, p, n, f);
}

static void edit_troubles(plink_status_t *st)
{
    st->sys_troubles = 0x10u;
}

static bool new_session_status(void *ctx)
{
    (void)ctx;
    return M.promotions >= 2u && K.items_sid == K.live.sid && !K.tentative &&
           K.view.snap.sys_troubles == 0x10u;
}

static bool promoted_twice(void *ctx)
{
    (void)ctx;
    return M.promotions >= 2u && K.view.snap.taken_ms >= M.promoted_at;
}

/* LINK-026: a status block, partition record, zone record or time is applied only from a frame
 * newer than the one it was last applied from; a new session starts the count again. */
static void test_reordered_frames(void)
{
    kp_online();
    M.auto_status = false;
    plink_part_t r[2];
    static uint8_t fa[PLINK_FRAME_MAX], fb[PLINK_FRAME_MAX], fc[PLINK_FRAME_MAX],
        fd[PLINK_FRAME_MAX];

    /* The system section: the older frame, arriving last, changes nothing. */
    part_rec(&r[0], 1u, ALARM_STATE_EXIT_DELAY);
    part_rec(&r[1], 2u, ALARM_STATE_DISARMED);
    const size_t na = status_frame(0x01u, r, 2u, fa);
    const size_t nb = status_frame(0x02u, r, 2u, fb);
    const uint32_t seq0 = plink_keypad_seq(&K);
    deliver(fb, nb);
    TEST_ASSERT_EQUAL_HEX32(0x02u, plink_keypad_snapshot(&K)->sys_troubles);
    deliver(fa, na);
    TEST_ASSERT_EQUAL_HEX32(0x02u, plink_keypad_snapshot(&K)->sys_troubles);
    TEST_ASSERT_EQUAL_UINT32(seq0 + 1u, plink_keypad_seq(&K));
    TEST_ASSERT_EQUAL_UINT32(plink_keypad_seq(&K), plink_keypad_snapshot(&K)->seq);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, plink_keypad_snapshot(&K)->part[0].state);

    /* A STATUS in pieces: each partition record by its own order. c1 and c2 carry partition 1
     * (c2 newer), d the newest system section and partition 2. Delivered d, c2, c1. */
    part_rec(&r[0], 1u, ALARM_STATE_ARMED);
    const size_t nc1 = status_frame(0x04u, &r[0], 1u, fa);
    part_rec(&r[0], 1u, ALARM_STATE_DISARMED);
    const size_t nc2 = status_frame(0x04u, &r[0], 1u, fb);
    part_rec(&r[1], 2u, ALARM_STATE_ARMED);
    const size_t nd = status_frame(0x08u, &r[1], 1u, fc);
    deliver(fc, nd);
    TEST_ASSERT_EQUAL_HEX32(0x08u, plink_keypad_snapshot(&K)->sys_troubles);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_ARMED, plink_keypad_snapshot(&K)->part[1].state);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, plink_keypad_snapshot(&K)->part[0].state);
    deliver(fb, nc2); /* older system section skipped, newer partition 1 record taken */
    TEST_ASSERT_EQUAL_HEX32(0x08u, plink_keypad_snapshot(&K)->sys_troubles);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, plink_keypad_snapshot(&K)->part[0].state);
    const uint32_t seq1 = plink_keypad_seq(&K);
    deliver(fa, nc1); /* older for both: nothing */
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, plink_keypad_snapshot(&K)->part[0].state);
    TEST_ASSERT_EQUAL_HEX32(0x08u, plink_keypad_snapshot(&K)->sys_troubles);
    TEST_ASSERT_EQUAL_UINT32(seq1, plink_keypad_seq(&K));

    /* Zones: z1 carries zones 3 and 4, z2 (newer) zone 3. Delivered z2, z1: zone 3 from z2, zone
     * 4 from z1. */
    const uint16_t f1[2] = {0x0001u, 0x0001u}, f2[1] = {0x0002u};
    const size_t nz1 = zones_frame(3u, f1, 2u, fa);
    const size_t nz2 = zones_frame(3u, f2, 1u, fb);
    deliver(fb, nz2);
    deliver(fa, nz1);
    TEST_ASSERT_EQUAL_HEX16(0x0002u, plink_keypad_snapshot(&K)->zone[2].flags);
    TEST_ASSERT_EQUAL_HEX16(0x0001u, plink_keypad_snapshot(&K)->zone[3].flags);

    /* Time: the newer first; the older and an unset clock are ignored. */
    plink_time_t t;
    (void)plink_keypad_take_time(&K, &t);
    const size_t nt1 = time_frame(1790000001000LL, fa);
    const size_t nt2 = time_frame(1790000002000LL, fb);
    const size_t nt3 = time_frame(0, fd);
    deliver(fb, nt2);
    TEST_ASSERT_TRUE(plink_keypad_take_time(&K, &t));
    TEST_ASSERT_EQUAL_INT64(1790000002000LL, t.epoch_ms);
    deliver(fa, nt1);
    deliver(fd, nt3);
    TEST_ASSERT_FALSE(plink_keypad_take_time(&K, &t));

    /* A new session counts from 1 again, and its frames are taken. */
    M.edit = edit_troubles;
    fm_rekey(K.live.sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, new_session_status, NULL));
    TEST_ASSERT_EQUAL_UINT(2u, M.promotions);
    TEST_ASSERT_TRUE(K.status_frame_seq < 5u);
    TEST_ASSERT_EQUAL_HEX32(0x10u, plink_keypad_snapshot(&K)->sys_troubles);
}

/* ---- the configuration -------------------------------------------------------------------- */

static bool pulling(void *ctx)
{
    (void)ctx;
    return K.cfg_pulling && K.view.have_status;
}

/* LINK-005, docs/features/11 "Configuration": the pull starts 0-500 ms after a new cfg_crc, keeps
 * two requests out and retries each after 300 ms doubling to 2 s. */
static void test_config_pull_requests(void)
{
    M.serve_cfg = false;
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, pulling, NULL));
    const uint64_t seen_at = NET.now_ms;
    plink_net_run(&NET, 7000);
    TEST_ASSERT_TRUE(M.n_cfg >= 10u);
    TEST_ASSERT_TRUE(M.cfg_at[0] >= seen_at && M.cfg_at[0] <= seen_at + 500u + NET.delay_ms);
    TEST_ASSERT_TRUE(M.blob_len > 4u * PAGE);
    static const uint32_t gaps[] = {0u, 300u, 600u, 1200u, 2000u, 2000u};
    TEST_ASSERT_TRUE(M.n_cfg >= 2u * (sizeof gaps / sizeof gaps[0]));
    uint64_t t = M.cfg_at[0];
    for (size_t i = 0; i + 1u < M.n_cfg; i += 2u) {
        /* Two out - the first gap and the page after it - and both resent together. */
        const plink_cfg_req_t *a = &M.cfg_seen[i], *b = &M.cfg_seen[i + 1u];
        TEST_ASSERT_EQUAL_HEX32(M.blob_crc, a->cfg_crc);
        TEST_ASSERT_EQUAL_HEX32(M.blob_crc, b->cfg_crc);
        TEST_ASSERT_EQUAL_UINT32(PAGE, a->offset + b->offset);
        TEST_ASSERT_TRUE(a->offset == 0u || b->offset == 0u);
        TEST_ASSERT_EQUAL_UINT16(PAGE, a->max);
        TEST_ASSERT_EQUAL_UINT16(PAGE, b->max);
        TEST_ASSERT_EQUAL_UINT64(M.cfg_at[i], M.cfg_at[i + 1u]);
        if (i / 2u < sizeof gaps / sizeof gaps[0]) {
            t += gaps[i / 2u];
            TEST_ASSERT_EQUAL_UINT64(t, M.cfg_at[i]);
        }
    }
}

/* LINK-017, LINK-019: the configuration arrives whole through loss, repeats, reordering and
 * corruption, its CRC checked, decoded and installed once. */
static void test_config_pull_through_loss(void)
{
    NET.loss_pm = 250u;
    NET.dup_pm = 100u;
    NET.corrupt_pm = 30u;
    NET.jitter_ms = 40u;
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 120000, online, NULL));
    TEST_ASSERT_EQUAL_UINT32(M.blob_crc, K.cfg_installed_crc);
    TEST_ASSERT_EQUAL_UINT32(1u, plink_keypad_config_seq(&K));
    const alarm_config_t *cfg = plink_keypad_config(&K);
    TEST_ASSERT_NOT_NULL(cfg);
    for (unsigned z = 0; z < ZONES; z++) {
        TEST_ASSERT_EQUAL_STRING(S.cfg.zones[z].name, cfg->zones[z].name);
        TEST_ASSERT_EQUAL_INT(S.cfg.zones[z].type, cfg->zones[z].type);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, info().cfg_bad_crc);
}

static void rename_zone_5(void)
{
    if (M.pages == 2u) {
        snprintf(S.cfg.zones[4].name, ALARM_NAME_LEN, "Family room bay window");
        fm_blob();
        fm_status();
    }
}

/* docs/features/11 "Configuration": a new cfg_crc mid-transfer restarts the pull on it; only the
 * whole new configuration is installed (config_seq rises once). */
static void test_config_changes_mid_transfer(void)
{
    const uint32_t first_crc = M.blob_crc;
    M.on_page = rename_zone_5;
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 10000, online, NULL));
    TEST_ASSERT_NOT_EQUAL(first_crc, M.blob_crc);
    TEST_ASSERT_EQUAL_UINT32(M.blob_crc, K.cfg_installed_crc);
    TEST_ASSERT_EQUAL_UINT32(1u, plink_keypad_config_seq(&K));
    TEST_ASSERT_EQUAL_STRING("Family room bay window", plink_keypad_config(&K)->zones[4].name);
    bool restarted = false;
    for (size_t i = 0; i < M.n_cfg; i++) {
        restarted =
            restarted || (M.cfg_seen[i].cfg_crc == M.blob_crc && M.cfg_seen[i].offset == 0u);
    }
    TEST_ASSERT_TRUE(restarted);
}

/* LINK-017: a blob this firmware can't read is remembered and never pulled again; the keypad says
 * so and stays CONNECTING; a readable one later is pulled at once. Once a configuration is in, a
 * later unreadable one doesn't take the link down. */
static void test_config_bad_blob(void)
{
    M.bad_blob = true;
    fm_blob();
    const uint32_t bad = M.blob_crc;
    kp_init(true);
    plink_net_run(&NET, 10000);
    TEST_ASSERT_EQUAL_HEX32(bad, info().cfg_bad_crc);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_CONNECTING, info().state);
    TEST_ASSERT_NULL(plink_keypad_config(&K));
    const unsigned reqs = M.cfg_reqs;
    plink_net_run(&NET, 60000); /* six STATUS still naming it */
    TEST_ASSERT_EQUAL_UINT(reqs, M.cfg_reqs);

    /* A master whose blob doesn't match the CRC it names: not installed, not pulled again. */
    M.bad_blob = false;
    fm_blob();
    M.blob_crc ^= 0x00010000u;
    const uint32_t lying = M.blob_crc;
    fm_status();
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_HEX32(lying, info().cfg_bad_crc);
    TEST_ASSERT_NULL(plink_keypad_config(&K));
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_config_seq(&K));
    const unsigned reqs2 = M.cfg_reqs;
    plink_net_run(&NET, 20000);
    TEST_ASSERT_EQUAL_UINT(reqs2, M.cfg_reqs);

    fm_blob();
    fm_status();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, online, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, plink_keypad_config_seq(&K));

    M.bad_blob = true;
    snprintf(S.cfg.zones[0].name, ALARM_NAME_LEN, "Front door, new name");
    fm_blob();
    fm_status();
    plink_net_run(&NET, 5000);
    TEST_ASSERT_EQUAL_HEX32(M.blob_crc, info().cfg_bad_crc);
    TEST_ASSERT_EQUAL_UINT32(1u, plink_keypad_config_seq(&K));
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
}

/* ---- zones and events --------------------------------------------------------------------- */

/* docs/features/11 "Zone record": the keypad asks for the zones when its CRC differs from the
 * master's after a STATUS, rate-limited (1 s, doubling) while it still differs. */
static void test_zones_requested_on_crc_mismatch(void)
{
    kp_online();
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_HEX32(K.view.zones_crc, plink_view_zones_crc(&K.view));
    const unsigned z0 = M.zones_reqs;
    sim_fault(&S, Z_WINDOW); /* no ZONES pushed: only the next STATUS says the CRC moved */
    fm_status();
    plink_net_run(&NET, 400);
    TEST_ASSERT_EQUAL_UINT(z0 + 1u, M.zones_reqs);
    TEST_ASSERT_EQUAL_UINT16(1u, M.zones_req.first);
    TEST_ASSERT_EQUAL_UINT16(ZONES, M.zones_req.count);
    TEST_ASSERT_EQUAL_HEX16(sim_zstatus(&S, Z_WINDOW).flags,
                            plink_keypad_snapshot(&K)->zone[Z_WINDOW - 1u].flags);
    TEST_ASSERT_EQUAL_HEX32(K.view.zones_crc, plink_view_zones_crc(&K.view));
    plink_net_run(&NET, 20000);
    TEST_ASSERT_EQUAL_UINT(z0 + 1u, M.zones_reqs);

    /* The master doesn't answer: asked again 1 s later, then 2 s, 4 s ... */
    M.serve_zones = false;
    sim_restore(&S, Z_WINDOW);
    fm_status();
    const uint64_t t0 = NET.now_ms;
    M.auto_status = false;
    plink_net_run(&NET, 16000);
    uint64_t at[16];
    const size_t n = sent_times(KN, PLINK_T_ZONES_REQ, at, 16);
    TEST_ASSERT_TRUE(n >= z0 + 5u);
    const size_t k = z0 + 1u; /* the first of this round */
    TEST_ASSERT_EQUAL_UINT64(t0 + 250u + NET.delay_ms, at[k]);
    TEST_ASSERT_EQUAL_UINT64(1000u, at[k + 1u] - at[k]);
    TEST_ASSERT_EQUAL_UINT64(2000u, at[k + 2u] - at[k + 1u]);
    TEST_ASSERT_EQUAL_UINT64(4000u, at[k + 3u] - at[k + 2u]);
}

static void arm_disarm(unsigned times)
{
    for (unsigned i = 0; i < times; i++) {
        EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
        EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
        sim_advance(&S, 1000u);
    }
}

static bool events_full(void *ctx)
{
    (void)ctx;
    return K.ev_count == PLINK_EVENTS_KEPT && !K.ev_want;
}

static void edit_log_restarted(plink_status_t *st)
{
    st->last_event_seq = 5u;
}

/* docs/features/11 "Event record": an empty ring asks for the last 50; a pushed event is taken
 * without a request; one that was lost is asked for after the newest kept; 50 are kept; the ring
 * is dropped when the master's last_event_seq goes below its newest. */
static void test_events_backfill_and_ring(void)
{
    arm_disarm(40);
    TEST_ASSERT_TRUE(S.n_events > 60u);
    const uint32_t last = S.events[S.n_events - 1u].seq;
    M.pushed = last;
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, events_full, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(0u, M.ev_req.after_seq);
    TEST_ASSERT_EQUAL_UINT8(PLINK_EVENTS_KEPT, M.ev_req.max);
    alarm_event_t ev[PLINK_EVENTS_KEPT];
    TEST_ASSERT_EQUAL_size_t(PLINK_EVENTS_KEPT, plink_keypad_events(&K, ev, PLINK_EVENTS_KEPT));
    TEST_ASSERT_EQUAL_UINT32(last, ev[0].seq); /* newest first */
    for (size_t i = 1; i < PLINK_EVENTS_KEPT; i++) {
        TEST_ASSERT_TRUE(ev[i].seq < ev[i - 1u].seq);
    }
    TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - PLINK_EVENTS_KEPT].seq,
                             ev[PLINK_EVENTS_KEPT - 1u].seq);

    /* Pushed before its STATUS: no request. */
    arm_disarm(1);
    fm_push_events();
    fm_status();
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_UINT(1u, M.ev_reqs);
    TEST_ASSERT_EQUAL_size_t(PLINK_EVENTS_KEPT, K.ev_count);
    TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - 1u].seq, K.events[K.ev_count - 1u].seq);

    /* The push lost: asked for after the newest kept. */
    const uint32_t before = K.events[K.ev_count - 1u].seq;
    arm_disarm(1);
    NET.lose_type = PLINK_T_EVENTS;
    NET.lose_type_count = 1u;
    fm_push_events();
    fm_status();
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_UINT(2u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(before, M.ev_req.after_seq);
    TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - 1u].seq, K.events[K.ev_count - 1u].seq);
    TEST_ASSERT_EQUAL_size_t(PLINK_EVENTS_KEPT, K.ev_count);

    /* LINK-026: a STATUS sealed before the EVENTS that brought a newer event, arriving after
     * them, reads a last_event_seq below the newest kept - and the ring stays. */
    M.auto_status = false;
    uint8_t sp[PLINK_PAYLOAD_MAX], sf[PLINK_FRAME_MAX];
    const size_t sl = fm_seal(PLINK_T_STATUS, sp, fm_status_payload(sp, sizeof sp), sf);
    NET.hold = true;
    arm_disarm(1);
    fm_push_events();
    plink_net_release(&NET);
    plink_net_run(&NET, 10);
    const uint32_t pushed_newest = K.events[K.ev_count - 1u].seq;
    TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - 1u].seq, pushed_newest);
    deliver(sf, sl);
    TEST_ASSERT_EQUAL_size_t(PLINK_EVENTS_KEPT, K.ev_count);
    TEST_ASSERT_EQUAL_UINT32(pushed_newest, K.events[K.ev_count - 1u].seq);
    fm_status();
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_UINT(2u, M.ev_reqs);
    M.auto_status = true;

    /* An EVENTS frame repeated, and old events: kept once, the ring still in order. */
    fm_send_events(&S.events[S.n_events - 5u], 5u, false);
    fm_send_events(&S.events[0], 3u, false);
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_size_t(PLINK_EVENTS_KEPT, plink_keypad_events(&K, ev, PLINK_EVENTS_KEPT));
    for (size_t i = 1; i < PLINK_EVENTS_KEPT; i++) {
        TEST_ASSERT_TRUE(ev[i].seq < ev[i - 1u].seq);
    }

    /* The master's log started again: last_event_seq below the newest kept. */
    size_t early = 0;
    uint32_t early_newest = 0;
    for (unsigned i = 0; i < S.n_events; i++) {
        if (S.events[i].seq <= 5u) {
            early++;
            early_newest = S.events[i].seq;
        }
    }
    TEST_ASSERT_TRUE(early > 0u);
    M.ev_limit = 5u;
    M.edit = edit_log_restarted;
    fm_status();
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_UINT(3u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(0u, M.ev_req.after_seq);
    TEST_ASSERT_EQUAL_size_t(early, plink_keypad_events(&K, ev, PLINK_EVENTS_KEPT));
    TEST_ASSERT_EQUAL_UINT32(early_newest, ev[0].seq);
}

static bool events_all(void *ctx)
{
    (void)ctx;
    return K.ev_count == S.n_events && !K.ev_want;
}

/* docs/features/11 "Event record" (EV_REQ fills gaps): a pushed event that doesn't follow on from
 * the newest kept - the push before it was lost - is kept, and what lies between is pulled after
 * the newest kept before it, so the ring has no hole; a frame that says older events were left
 * out is taken as it is; a hole the master's answers never fill is asked for 4 times, then let
 * be - never pulled for good. */
static void test_events_hole_filled(void)
{
    arm_disarm(3);
    M.pushed = S.events[S.n_events - 1u].seq;
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, events_all, NULL));
    const unsigned reqs = M.ev_reqs;
    uint32_t before = K.events[K.ev_count - 1u].seq;
    /* Two new events; only the second one's push arrives. */
    arm_disarm(1);
    TEST_ASSERT_EQUAL_UINT32(before + 2u, S.events[S.n_events - 1u].seq);
    fm_send_events(&S.events[S.n_events - 1u], 1u, false);
    M.pushed = S.events[S.n_events - 1u].seq;
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_UINT(reqs + 1u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(before, M.ev_req.after_seq);
    TEST_ASSERT_EQUAL_size_t(S.n_events, K.ev_count);
    for (size_t i = 0; i < K.ev_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(S.events[i].seq, K.events[i].seq);
    }
    TEST_ASSERT_FALSE(K.ev_want);

    /* A frame that says older events were left out: taken as it is, nothing pulled. */
    arm_disarm(1);
    fm_send_events(&S.events[S.n_events - 1u], 1u, true);
    M.pushed = S.events[S.n_events - 1u].seq;
    plink_net_run(&NET, 11000);
    TEST_ASSERT_EQUAL_UINT(reqs + 1u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - 1u].seq, K.events[K.ev_count - 1u].seq);
    TEST_ASSERT_FALSE(K.ev_want);

    /* A hole the master never answers for: asked for 4 times, then let be. */
    M.serve_events = false;
    before = K.events[K.ev_count - 1u].seq;
    arm_disarm(1);
    fm_send_events(&S.events[S.n_events - 1u], 1u, false);
    M.pushed = S.events[S.n_events - 1u].seq;
    plink_net_run(&NET, 30000);
    TEST_ASSERT_EQUAL_UINT(reqs + 5u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(before, M.ev_req.after_seq);
    TEST_ASSERT_FALSE(K.ev_want);
    plink_net_run(&NET, 60000);
    TEST_ASSERT_EQUAL_UINT(reqs + 5u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - 1u].seq, K.events[K.ev_count - 1u].seq);
}

/* ---- commands ----------------------------------------------------------------------------- */

static bool cmd_done(void *ctx)
{
    return plink_keypad_command_state(&K, *(const uint32_t *)ctx, NULL) == PLINK_CMD_DONE;
}

/* LINK-018, LINK-002: a command goes with a request id and its codes padded; the master's answer
 * comes back; a lost copy is resent after 500 ms with the same req and runs once; a lost answer
 * is answered again from the master's cache; the codes are wiped once it is answered. */
static void test_command_and_retries(void)
{
    kp_online();
    plink_cmd_t c = arm_cmd("1111");
    uint32_t req = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_NOT_EQUAL(0u, req);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 100, cmd_done, &req));
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, plink_keypad_command_state(&K, req, &res));
    TEST_ASSERT_EQUAL_UINT32(req, res.req);
    TEST_ASSERT_EQUAL_UINT8(ALARM_OK, res.result);
    TEST_ASSERT_EQUAL_UINT8(3u, res.user); /* user 3's code, as the engine found it */
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_TRUE(all_zero(slot_of(req)->cmd.cmd.code, sizeof c.cmd.code));
    /* The CMD's payload, byte for byte from docs/features/11 "Messages": req u32, op u8, the op
     * section (length 9: type, partition, level, arm_flags, zone u16, panic, walk_op,
     * walk_flags), then each code as a length byte and 10 bytes zero-padded. */
    const uint8_t want[] = {(uint8_t)req,
                            (uint8_t)(req >> 8),
                            (uint8_t)(req >> 16),
                            (uint8_t)(req >> 24),
                            PLINK_OP_COMMAND,
                            9u,
                            ALARM_CMD_ARM,
                            1u,
                            ALARM_LEVEL_AWAY,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            4u,
                            '1',
                            '1',
                            '1',
                            '1',
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u,
                            0u};
    TEST_ASSERT_EQUAL_size_t(sizeof want, M.cmd_pt_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, M.cmd_pt, sizeof want);
    plink_keypad_command_release(&K, req);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NONE, plink_keypad_command_state(&K, req, NULL));

    /* The first copy lost. */
    c = arm_cmd("1111");
    c.cmd.type = ALARM_CMD_DISARM;
    NET.lose_type = PLINK_T_CMD;
    NET.lose_type_count = 1u;
    const unsigned runs = M.runs;
    const uint64_t t0 = NET.now_ms;
    req = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 1000, cmd_done, &req));
    TEST_ASSERT_EQUAL_UINT(runs + 1u, M.runs);
    TEST_ASSERT_EQUAL_UINT32(req, M.cmd_req[M.n_cmd - 1u]);
    uint64_t at[8];
    const size_t n = sent_times(KN, PLINK_T_CMD, at, 8);
    TEST_ASSERT_EQUAL_UINT64(t0, at[n - 2u]);
    TEST_ASSERT_EQUAL_UINT64(t0 + PLINK_CMD_RETRY_MS, at[n - 1u]);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* The answer lost: the retry is answered from the cache, not run again. */
    c = arm_cmd("1111");
    NET.lose_type = PLINK_T_RESULT;
    NET.lose_type_count = 1u;
    req = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 1000, cmd_done, &req));
    TEST_ASSERT_EQUAL_UINT(runs + 2u, M.runs);
    TEST_ASSERT_EQUAL_UINT(1u, M.cached);

    /* Released while still out: the slot frees once it's answered. */
    sim_disarm(&S, 1, CODE_USER);
    c.cmd.type = ALARM_CMD_CHIME;
    M.serve_cmds = false;
    req = plink_keypad_command(&K, NET.now_ms, &c);
    plink_keypad_command_release(&K, req);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, plink_keypad_command_state(&K, req, NULL));
    M.serve_cmds = true;
    plink_net_run(&NET, 600);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NONE, plink_keypad_command_state(&K, req, NULL));
    TEST_ASSERT_NULL(slot_of(req));

    /* A code the frame can't carry is refused at once. */
    c = arm_cmd("12a4");
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_command(&K, NET.now_ms, &c));
}

/* LINK-018: no answer within 3 s: given up, after a copy every 500 ms, the codes wiped. */
static void test_command_gives_up_at_3s(void)
{
    kp_online();
    M.serve_cmds = false;
    const plink_cmd_t c = arm_cmd("1111");
    const uint64_t t0 = NET.now_ms;
    const size_t before = plink_net_count(&NET, KN, PLINK_T_CMD);
    const uint32_t req = plink_keypad_command(&K, NET.now_ms, &c);
    run_to(t0 + PLINK_CMD_GIVE_UP_MS - 1u);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, plink_keypad_command_state(&K, req, NULL));
    TEST_ASSERT_FALSE(all_zero(slot_of(req)->cmd.cmd.code, sizeof c.cmd.code));
    run_to(t0 + PLINK_CMD_GIVE_UP_MS);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NO_ANSWER, plink_keypad_command_state(&K, req, NULL));
    TEST_ASSERT_TRUE(all_zero(slot_of(req)->cmd.cmd.code, sizeof c.cmd.code));
    TEST_ASSERT_EQUAL_size_t(before + 6u, plink_net_count(&NET, KN, PLINK_T_CMD));
    plink_net_run(&NET, 5000);
    TEST_ASSERT_EQUAL_size_t(before + 6u, plink_net_count(&NET, KN, PLINK_T_CMD));
    /* Unreleased answers don't block new commands: the oldest finished slot is reused. */
    for (unsigned i = 0; i < 2u * PLINK_CMDS_MAX; i++) {
        TEST_ASSERT_NOT_EQUAL(0u, plink_keypad_command(&K, NET.now_ms, &c));
        plink_net_run(&NET, PLINK_CMD_GIVE_UP_MS);
    }
}

/* LINK-018: request ids count from a random base drawn at each start, so a keypad restarted
 * within the master's 90 s cache runs its new command as new (never answered from the cache). */
static void test_request_ids_unique_across_restart(void)
{
    kp_online();
    const plink_cmd_t c = arm_cmd("1111");
    uint32_t r1 = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 100, cmd_done, &r1));
    uint32_t r1b = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_EQUAL_UINT32(r1 + 1u, r1b);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 100, cmd_done, &r1b));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    plink_net_run(&NET, 10000);
    kp_online(); /* restarted (its port's generator moves on, as a real one does) */
    uint32_t r2 = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_NOT_EQUAL(0u, r2);
    TEST_ASSERT_NOT_EQUAL(r1, r2);
    TEST_ASSERT_NOT_EQUAL(r1b, r2);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 100, cmd_done, &r2));
    TEST_ASSERT_EQUAL_UINT(3u, M.runs);
    TEST_ASSERT_EQUAL_UINT(0u, M.cached);
}

static bool panic_done(void *ctx)
{
    (void)ctx;
    return !plink_keypad_panic_pending(&K);
}

/* LINK-023: a panic keyed during a link outage is accepted whatever the link's state, resent
 * every 1 s with the same req, and runs once when the master hears it; a second key-press is the
 * same request; the handshake retries come within 2 s while it waits. */
static void test_panic_through_outage(void)
{
    kp_online();
    NET.cut_from = KN;
    NET.cut_to = KN;
    plink_net_run(&NET, 31000);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_LOST, info().state);
    const plink_cmd_t p = panic_cmd();
    const uint64_t t0 = NET.now_ms;
    const uint32_t req = plink_keypad_command(&K, NET.now_ms, &p);
    TEST_ASSERT_NOT_EQUAL(0u, req);
    TEST_ASSERT_EQUAL_UINT32(req, plink_keypad_command(&K, NET.now_ms, &p));
    TEST_ASSERT_TRUE(plink_keypad_panic_pending(&K));
    plink_keypad_command_release(&K, req); /* the UI stops waiting; the panic goes on */
    const size_t c0 = plink_net_count(&NET, KN, PLINK_T_CMD);
    const size_t h0 = plink_net_count(&NET, KN, PLINK_T_HELLO);
    plink_net_run(&NET, 20000);
    TEST_ASSERT_EQUAL_size_t(c0 + 20u, plink_net_count(&NET, KN, PLINK_T_CMD));
    uint64_t at[64];
    const size_t nh = sent_times(KN, PLINK_T_HELLO, at, 64);
    TEST_ASSERT_TRUE(nh >= h0 + 9u);
    for (size_t i = h0 + 1u; i < nh; i++) {
        TEST_ASSERT_TRUE(at[i] - at[i - 1u] <= 2000u);
    }
    TEST_ASSERT_EQUAL_UINT(0u, M.runs);

    NET.cut_from = NET.cut_to = -1;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, panic_done, NULL));
    TEST_ASSERT_TRUE(NET.now_ms < t0 + PLINK_PANIC_KEEP_MS);
    TEST_ASSERT_EQUAL_UINT(1u, M.runs);
    plink_net_run(&NET, 5000);
    TEST_ASSERT_EQUAL_UINT(1u, M.runs);
    for (size_t i = 0; i < M.n_cmd; i++) {
        TEST_ASSERT_EQUAL_UINT32(req, M.cmd_req[i]);
    }
    TEST_ASSERT_TRUE(sim_count_events(&S, ALARM_EV_ALARM) >= 1u);
    TEST_ASSERT_NULL(slot_of(req)); /* released earlier: freed once answered */
}

/* LINK-023: a panic nobody answers is given up 60 s after keying, after a copy every 1 s. */
static void test_panic_given_up_after_60s(void)
{
    kp_online();
    M.serve_cmds = false;
    const plink_cmd_t p = panic_cmd();
    const uint64_t t0 = NET.now_ms;
    const size_t c0 = plink_net_count(&NET, KN, PLINK_T_CMD);
    const uint32_t req = plink_keypad_command(&K, NET.now_ms, &p);
    run_to(t0 + PLINK_PANIC_KEEP_MS - 1u);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, plink_keypad_command_state(&K, req, NULL));
    run_to(t0 + PLINK_PANIC_KEEP_MS);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NO_ANSWER, plink_keypad_command_state(&K, req, NULL));
    TEST_ASSERT_FALSE(plink_keypad_panic_pending(&K));
    TEST_ASSERT_EQUAL_size_t(c0 + 60u, plink_net_count(&NET, KN, PLINK_T_CMD));
}

/* ---- removal and hints -------------------------------------------------------------------- */

/* LINK-014: a sealed BYE erases the record and the keypad says it was removed; nothing more is
 * sent. A forged BYE (bad tag) changes nothing; an erase that fails is tried again. */
static void test_bye_removes(void)
{
    kp_online();
    uint8_t p[4], f[PLINK_FRAME_MAX];
    size_t n = 0;
    const plink_bye_t bye = {1u};
    TEST_ASSERT_TRUE(plink_enc_bye(&bye, p, sizeof p, &n));
    const size_t fl = fm_seal(PLINK_T_BYE, p, n, f);
    f[fl - 1u] ^= 0x80u;
    deliver(f, fl);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    TEST_ASSERT_EQUAL_UINT(0u, KS.erases);

    KS.fail = true;
    fm_send_sealed(PLINK_T_BYE, p, n);
    plink_net_run(&NET, 10);
    plink_keypad_info_t i = info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, i.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_REMOVED, i.unpaired_why);
    TEST_ASSERT_EQUAL_UINT8(0u, i.device);
    TEST_ASSERT_FALSE(K.live.valid);
    TEST_ASSERT_TRUE(all_zero(&K.rec, sizeof K.rec));
    KS.fail = false;
    plink_net_run(&NET, 5000);
    TEST_ASSERT_EQUAL_UINT(1u, KS.erases);
    TEST_ASSERT_FALSE(KS.have);
    const plink_cmd_t c = panic_cmd();
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_command(&K, NET.now_ms, &c));
    const size_t sent = NET.log_count;
    size_t from_k = 0;
    plink_net_run(&NET, 60000);
    for (size_t j = sent; j < NET.log_count; j++) {
        from_k += NET.log[j].from == KN ? 1u : 0u;
    }
    TEST_ASSERT_EQUAL_size_t(0u, from_k);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_keypad_next(&K));
}

/* docs/features/11 "Sessions": NOT_PAIRED hints change only what the keypad says, once they have
 * kept coming for 5 min; the keys stay. The keypad looks for the master again before every retry
 * after the first. */
static void test_not_paired_hint_after_5_min(void)
{
    M.not_paired = true;
    kp_init(true);
    const uint64_t t0 = NET.now_ms;
    /* A hint from another address counts for nothing. */
    uint8_t f[PLINK_HDR_LEN];
    size_t fl = 0;
    const plink_hdr_t h = {
        PLINK_T_NOT_PAIRED, PLINK_FLAG_FROM_MASTER, NET_ID, PLINK_SENDER_MASTER, 0u, 0u};
    TEST_ASSERT_TRUE(plink_frame_plain(&h, &f[PLINK_HDR_LEN], 0u, f, sizeof f, &fl));
    const plink_addr_t x = plink_addr_udp(IP_X, (uint16_t)PLINK_UDP_PORT);
    plink_keypad_input(&K, NET.now_ms, &x, f, fl);
    TEST_ASSERT_EQUAL_UINT64(0u, K.not_paired_since_ms);
    run_to(t0 + PLINK_NOT_PAIRED_HINT_MS - 1u);
    TEST_ASSERT_FALSE(info().not_paired_hint);
    run_to(t0 + PLINK_NOT_PAIRED_HINT_MS + PLINK_BACKOFF_MAX_MS + 1000u);
    TEST_ASSERT_TRUE(info().not_paired_hint);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_CONNECTING, info().state);
    TEST_ASSERT_EQUAL_UINT(0u, KS.erases);
    TEST_ASSERT_EQUAL_UINT(0u, KS.writes);
    TEST_ASSERT_EQUAL_MEMORY(REC.k_pair, K.rec.k_pair, PLINK_KEY_LEN);
    TEST_ASSERT_EQUAL_UINT(M.hellos - 1u, RESOLVES);
    /* A hint from elsewhere counts for nothing; the master taking the keypad again clears it. */
    M.not_paired = false;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_BACKOFF_MAX_MS + 1000u, online, NULL));
    TEST_ASSERT_FALSE(info().not_paired_hint);
}

/* ---- REKEY and the handshake's back-off --------------------------------------------------- */

/* docs/features/11 "Sessions": the master restarted - its sessions gone - answers the keypad's
 * next frame with REKEY; the keypad's new handshake brings it back within 15 s without the link
 * ever showing LOST. A panic keyed meanwhile goes on the new session. */
static void test_rekey_after_master_restart(void)
{
    kp_online();
    const uint64_t online_since = K.state_since_ms;
    const uint32_t old_sid = K.live.sid;
    fm_restart();
    const uint64_t t0 = NET.now_ms;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_REHANDSHAKE_MS, promoted_twice, NULL));
    TEST_ASSERT_TRUE(M.promoted_at - t0 <= PLINK_REHANDSHAKE_MS);
    TEST_ASSERT_NOT_EQUAL(old_sid, K.live.sid);
    TEST_ASSERT_EQUAL_UINT32(M.live.sid, K.live.sid);
    TEST_ASSERT_TRUE(M.unknown_sid >= 1u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    TEST_ASSERT_EQUAL_UINT64(online_since, K.state_since_ms);

    /* Again, with a panic keyed right after the restart (and after the 5 s between REKEYs). */
    plink_net_run(&NET, PLINK_REKEY_GAP_MS);
    fm_restart();
    const plink_cmd_t p = panic_cmd();
    const uint32_t req = plink_keypad_command(&K, NET.now_ms, &p);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, panic_done, NULL));
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, plink_keypad_command_state(&K, req, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, M.runs);
}

/* docs/features/11 "Sessions": a REKEY hint is taken for the live session only, from the
 * keypad's network, at most every 5 s. */
static void test_rekey_hints_limited(void)
{
    kp_online();
    const size_t h0 = plink_net_count(&NET, KN, PLINK_T_HELLO);
    fm_rekey(K.live.sid ^ 1u, NET_ID);      /* another session */
    fm_rekey(K.live.sid, NET_ID ^ 0x0100u); /* another network */
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_size_t(h0, plink_net_count(&NET, KN, PLINK_T_HELLO));
    for (unsigned i = 0; i < 10u; i++) {
        fm_rekey(K.live.sid, NET_ID);
        plink_net_run(&NET, 1000);
    }
    TEST_ASSERT_EQUAL_size_t(h0 + 2u, plink_net_count(&NET, KN, PLINK_T_HELLO));
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
}

/* docs/features/11 "Sessions": unanswered HELLOs back off 1, 2, 4 ... 30 s with jitter (each wait
 * in the last quarter of its step), at most 2 s while a panic waits; every HELLO is fresh. */
static void test_handshake_backoff(void)
{
    M.answer_hello = false;
    kp_init(true);
    plink_net_run(&NET, 150000);
    uint64_t at[64];
    const size_t n = sent_times(KN, PLINK_T_HELLO, at, 64);
    TEST_ASSERT_TRUE(n >= 9u);
    uint64_t step = 1000u;
    for (size_t i = 1; i < n; i++) {
        const uint64_t gap = at[i] - at[i - 1u];
        TEST_ASSERT_TRUE_MESSAGE(gap >= step - step / 4u && gap <= step, "back-off step");
        step = step * 2u < PLINK_BACKOFF_MAX_MS ? step * 2u : PLINK_BACKOFF_MAX_MS;
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)n - 1u, RESOLVES);
    uint8_t h1[PLINK_FRAME_MAX], h2[PLINK_FRAME_MAX];
    size_t l1 = 0, l2 = 0;
    TEST_ASSERT_TRUE(plink_net_last(&NET, KN, PLINK_T_HELLO, h1, sizeof h1, &l1));
    plink_net_run(&NET, PLINK_BACKOFF_MAX_MS);
    TEST_ASSERT_TRUE(plink_net_last(&NET, KN, PLINK_T_HELLO, h2, sizeof h2, &l2));
    TEST_ASSERT_FALSE(l1 == l2 && memcmp(h1, h2, l1) == 0); /* new e_k and n_k */

    /* A panic waits: the next HELLO within 2 s, and every one after it. */
    const plink_cmd_t p = panic_cmd();
    const uint64_t t0 = NET.now_ms;
    const size_t before = sent_times(KN, PLINK_T_HELLO, at, 64);
    TEST_ASSERT_NOT_EQUAL(0u, plink_keypad_command(&K, NET.now_ms, &p));
    plink_net_run(&NET, 10000);
    const size_t m = sent_times(KN, PLINK_T_HELLO, at, 64);
    TEST_ASSERT_TRUE(m >= before + 5u);
    TEST_ASSERT_TRUE(at[before] <= t0 + HS_PANIC_TEST_MS);
    for (size_t i = before + 1u; i < m; i++) {
        TEST_ASSERT_TRUE(at[i] - at[i - 1u] <= HS_PANIC_TEST_MS);
    }
    /* The master answers: the waiting panic goes on the new session. */
    M.answer_hello = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, panic_done, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, M.runs);

    /* A WELCOME that verified starts the back-off again from 1 s. */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_net_run(&NET, PLINK_REKEY_GAP_MS);
    M.answer_hello = false;
    const size_t r0 = sent_times(KN, PLINK_T_HELLO, at, 64);
    fm_restart(); /* the next HEARTBEAT meets REKEY */
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 5000u);
    const size_t k = sent_times(KN, PLINK_T_HELLO, at, 64);
    TEST_ASSERT_TRUE(k >= r0 + 3u);
    TEST_ASSERT_TRUE(at[r0 + 1u] - at[r0] >= 750u && at[r0 + 1u] - at[r0] <= 1000u);
    TEST_ASSERT_TRUE(at[r0 + 2u] - at[r0 + 1u] >= 1500u && at[r0 + 2u] - at[r0 + 1u] <= 2000u);
}

static bool hello_sent_to(void *ctx)
{
    const plink_addr_t *a = ctx;
    for (size_t i = NET.recent_next > PLINK_NET_RECENT ? NET.recent_next - PLINK_NET_RECENT : 0u;
         i < NET.recent_next; i++) {
        const plink_net_dgram_t *d = &NET.recent[i % PLINK_NET_RECENT];
        if (d->from == KN && d->len > 3u && d->b[3] == PLINK_T_HELLO &&
            plink_addr_equal(&d->to, a)) {
            return true;
        }
    }
    return false;
}

/* LINK-021, docs/features/11 "Sessions": the router restarted - the master moved and lost its
 * sessions. Its keypad, hearing nothing for 15 s, sends HELLOs where it last found the master and
 * asks the platform to look again; discovery's new address gets a HELLO at once, and the WELCOME
 * from there moves the master. */
static void test_master_moves(void)
{
    kp_online();
    fm_status(); /* the last word from the old address: the clocks below count from it */
    plink_net_run(&NET, 10);
    const plink_addr_t old_addr = *plink_net_addr(&NET, MN);
    static const uint8_t ip_new[4] = {192u, 0u, 2u, 77u};
    NET.node[MN].addr = plink_addr_udp(ip_new, (uint16_t)PLINK_UDP_PORT);
    fm_restart();
    const uint64_t t0 = NET.now_ms;
    plink_net_run(&NET, PLINK_REHANDSHAKE_MS + 5000u);
    TEST_ASSERT_TRUE(hello_sent_to((void *)&old_addr));
    TEST_ASSERT_TRUE(RESOLVES >= 1u);
    TEST_ASSERT_EQUAL_UINT(1u, M.promotions);
    TEST_ASSERT_TRUE(master_is(&old_addr));
    plink_keypad_master_addr(&K, NET.now_ms, &NET.node[MN].addr); /* discovery found it */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 10, hello_sent_to, &NET.node[MN].addr));
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_UINT(2u, M.promotions);
    TEST_ASSERT_TRUE(master_is(&NET.node[MN].addr));
    TEST_ASSERT_TRUE(NET.now_ms - t0 < PLINK_LOST_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
}

/* LINK-015: at seq 2^30 the keypad starts a new handshake while its session still works, and
 * moves to the new session long before 2^31. */
static void test_rekey_at_seq_2_30(void)
{
    kp_online();
    const uint32_t old_sid = K.live.sid;
    const size_t h0 = plink_net_count(&NET, KN, PLINK_T_HELLO);
    K.live.tx_seq = PLINK_SEQ_REKEY - 2u;
    plink_keypad_addr_changed(&K, NET.now_ms); /* a HEARTBEAT with seq 2^30 - 1 */
    plink_net_run(&NET, 5);
    TEST_ASSERT_EQUAL_size_t(h0, plink_net_count(&NET, KN, PLINK_T_HELLO));
    plink_keypad_addr_changed(&K, NET.now_ms); /* seq 2^30 */
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_size_t(h0 + 1u, plink_net_count(&NET, KN, PLINK_T_HELLO));
    TEST_ASSERT_NOT_EQUAL(old_sid, K.live.sid);
    TEST_ASSERT_TRUE(K.live.tx_seq < 100u);
    TEST_ASSERT_EQUAL_UINT32(M.live.sid, K.live.sid);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
}

/* ---- addresses, heartbeats and the rest --------------------------------------------------- */

/* LINK-021: the master's address moves only on an authenticated frame - not on a forged or
 * replayed one, nor on discovery while a session is up; a new address of the keypad's own is
 * told with a HEARTBEAT at once. LINK-006: the HEARTBEAT's payload is the document's. */
static void test_addresses_and_heartbeat(void)
{
    kp_online();
    const plink_addr_t x = plink_addr_udp(IP_X, (uint16_t)PLINK_UDP_PORT);
    const plink_addr_t *m = plink_net_addr(&NET, MN);
    M.auto_status = false;
    uint8_t p[PLINK_PAYLOAD_MAX], f[PLINK_FRAME_MAX];
    size_t pl = fm_status_payload(p, sizeof p);
    size_t fl = fm_seal(PLINK_T_STATUS, p, pl, f);
    deliver(f, fl);
    plink_keypad_input(&K, NET.now_ms, &x, f, fl); /* replayed from elsewhere */
    TEST_ASSERT_TRUE(master_is(m));
    fl = fm_seal(PLINK_T_STATUS, p, pl, f);
    f[PLINK_HDR_LEN + 3u] ^= 0x10u; /* forged: the tag fails */
    plink_keypad_input(&K, NET.now_ms, &x, f, fl);
    TEST_ASSERT_TRUE(master_is(m));
    plink_keypad_master_addr(&K, NET.now_ms, &x); /* discovery, while the session is up */
    TEST_ASSERT_TRUE(master_is(m));
    TEST_ASSERT_TRUE(plink_addr_equal(&x, &K.disc));
    fl = fm_seal(PLINK_T_STATUS, p, pl, f);
    plink_keypad_input(&K, NET.now_ms, &x, f, fl); /* genuine, from a new address */
    TEST_ASSERT_TRUE(master_is(&x));
    plink_keypad_master_addr(&K, NET.now_ms, m);
    fl = fm_seal(PLINK_T_STATUS, p, pl, f);
    deliver(f, fl);
    TEST_ASSERT_TRUE(master_is(m));

    plink_keypad_set_power(&K, PLINK_HB_MAINS_LOST | PLINK_HB_BATTERY_THERE, -61);
    const unsigned hb0 = M.hbs;
    const uint64_t sent = NET.now_ms;
    plink_keypad_addr_changed(&K, NET.now_ms);
    plink_net_run(&NET, NET.delay_ms);
    TEST_ASSERT_EQUAL_UINT(hb0 + 1u, M.hbs);
    const uint32_t up = (uint32_t)((sent - K.started_ms) / 1000u);
    const uint16_t age = (uint16_t)((sent - K.status_ms) / 1000u);
    /* flags u8, rssi i8, uptime_s u32, status_age_s u16, fw (length byte + UTF-8). */
    const uint8_t want[] = {0x05u,
                            (uint8_t)(int8_t)-61,
                            (uint8_t)up,
                            (uint8_t)(up >> 8),
                            (uint8_t)(up >> 16),
                            (uint8_t)(up >> 24),
                            (uint8_t)age,
                            (uint8_t)(age >> 8),
                            5u,
                            '1',
                            '.',
                            '2',
                            '.',
                            '3'};
    TEST_ASSERT_EQUAL_size_t(sizeof want, M.hb_pt_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, M.hb_pt, sizeof want);

    /* Heartbeats every 10 s +- 1 s. */
    M.auto_status = true;
    plink_net_run(&NET, 60000);
    uint64_t at[64];
    const size_t n = sent_times(KN, PLINK_T_HEARTBEAT, at, 64);
    TEST_ASSERT_TRUE(n >= 6u);
    for (size_t i = n - 5u; i < n; i++) {
        const uint64_t gap = at[i] - at[i - 1u];
        TEST_ASSERT_TRUE(gap >= PLINK_HEARTBEAT_MS - 1000u && gap <= PLINK_HEARTBEAT_MS + 1000u);
    }
}

static void edit_home_2(plink_status_t *st)
{
    st->home = 2u;
}

/* docs/features/11 "Roles": sounds queue up to 4 (the oldest dropped); the master's time is
 * taken; its epoch and reporting state are read from the status; a new home partition is kept in
 * the record. */
static void test_sounds_time_and_home(void)
{
    kp_online();
    plink_time_t t;
    TEST_ASSERT_TRUE(plink_keypad_take_time(&K, &t));
    TEST_ASSERT_EQUAL_INT64(M.epoch_ms, t.epoch_ms);
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", t.tz);
    TEST_ASSERT_FALSE(plink_keypad_take_time(&K, &t));
    const int64_t e = plink_keypad_epoch_ms(&K, NET.now_ms + 1500u);
    TEST_ASSERT_EQUAL_INT64(M.epoch_ms + (int64_t)(NET.now_ms + 1500u - K.view.epoch_at_ms), e);
    TEST_ASSERT_EQUAL_UINT8(PLINK_REPORT_ONLINE, plink_keypad_report(&K));

    for (uint8_t i = 0; i < 5u; i++) {
        const plink_sound_t s = {1u, (uint8_t)(ALARM_SOUND_CHIME + 0u), (uint16_t)(10u + i)};
        uint8_t p[8];
        size_t n = 0;
        TEST_ASSERT_TRUE(plink_enc_sound(&s, p, sizeof p, &n));
        fm_send_sealed(PLINK_T_SOUND, p, n);
        plink_net_run(&NET, 1); /* one after the other (the net reorders same-time datagrams) */
    }
    plink_net_run(&NET, 10);
    plink_sound_t s;
    for (uint16_t i = 1; i < 5u; i++) {
        TEST_ASSERT_TRUE(plink_keypad_take_sound(&K, &s));
        TEST_ASSERT_EQUAL_UINT16(10u + i, s.zone);
    }
    TEST_ASSERT_FALSE(plink_keypad_take_sound(&K, &s));

    M.edit = edit_home_2;
    const unsigned w = KS.writes;
    fm_status();
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT(w + 1u, KS.writes);
    TEST_ASSERT_EQUAL_UINT8(2u, KS.rec.home);
    TEST_ASSERT_EQUAL_UINT8(2u, info().home);
    fm_status();
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT(w + 1u, KS.writes); /* written once */
}

/* LINK-014's keypad side: forgetting the master (stand-alone, or before pairing again) erases the
 * record and wipes the session; nothing more is sent. */
static void test_forget(void)
{
    kp_online();
    TEST_ASSERT_TRUE(plink_keypad_forget(&K, NET.now_ms));
    TEST_ASSERT_EQUAL_UINT(1u, KS.erases);
    plink_keypad_info_t i = info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, i.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_NEVER, i.unpaired_why);
    TEST_ASSERT_FALSE(K.live.valid);
    TEST_ASSERT_NULL(plink_keypad_config(&K));
    const size_t sent = NET.log_count;
    plink_net_run(&NET, 30000);
    for (size_t j = sent; j < NET.log_count; j++) {
        TEST_ASSERT_NOT_EQUAL(KN, NET.log[j].from);
    }
    KS.fail = true;
    TEST_ASSERT_FALSE(plink_keypad_forget(&K, NET.now_ms));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pairing_stores_the_record);
    RUN_TEST(test_handshake_first_heartbeat_lost);
    RUN_TEST(test_old_session_until_new_heard);
    RUN_TEST(test_connecting_until_full_sync);
    RUN_TEST(test_lost_at_30s_back_after_3s);
    RUN_TEST(test_maintenance_after_10_min);
    RUN_TEST(test_reordered_frames);
    RUN_TEST(test_config_pull_requests);
    RUN_TEST(test_config_pull_through_loss);
    RUN_TEST(test_config_changes_mid_transfer);
    RUN_TEST(test_config_bad_blob);
    RUN_TEST(test_zones_requested_on_crc_mismatch);
    RUN_TEST(test_events_backfill_and_ring);
    RUN_TEST(test_events_hole_filled);
    RUN_TEST(test_command_and_retries);
    RUN_TEST(test_command_gives_up_at_3s);
    RUN_TEST(test_request_ids_unique_across_restart);
    RUN_TEST(test_panic_through_outage);
    RUN_TEST(test_panic_given_up_after_60s);
    RUN_TEST(test_bye_removes);
    RUN_TEST(test_not_paired_hint_after_5_min);
    RUN_TEST(test_rekey_after_master_restart);
    RUN_TEST(test_rekey_hints_limited);
    RUN_TEST(test_handshake_backoff);
    RUN_TEST(test_master_moves);
    RUN_TEST(test_rekey_at_seq_2_30);
    RUN_TEST(test_addresses_and_heartbeat);
    RUN_TEST(test_sounds_time_and_home);
    RUN_TEST(test_forget);
    return UNITY_END();
}
