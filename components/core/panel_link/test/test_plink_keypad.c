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
 * Smart Home and settings (docs/features/11 "Smart Home and Home Assistant on keypads"): the
 * scripted master builds its device list and records from a registry with plink_home.h, as the
 * master's platform does. It proves HOME offered only with a mirror, and nothing of either
 * capability without its bit or on frames under 250 B; the list pulled as the configuration is,
 * installed whole, a list that fails remembered, records carried over by ref at an install; the
 * records in frame order per device and only under the installed list, caught up while their CRC
 * differs (250 ms, then 1 s doubling to 30 s); device data stale after 30 s, kept fresh only by
 * data under the installed list (a list that fails leaves the old one offline); device actions
 * refused at once, resent every 500 ms and given up at 3 s, their answers and what the device
 * shows, in 2 slots of their own; list and state requests held while a panic is sent; the
 * settings report's timing, the HEARTBEAT's settings CRC, and a PREFS_SET applied once.
 *
 * Updates through the link (docs/features/11 "Updates through the link", "The keypad"): the
 * scripted master sends FW_ORDERs and serves FW_REQs from a test image; a scripted service takes
 * the orders and writes the relay's bytes. It proves nothing of it without PLINK_CAP_FW or on
 * frames under 250 B; FW_INFO's timing; every order answered at once and run once, a stale one
 * dropped, the refusals; the relay's pull - its requests (offset, page, count, the 16 KB window,
 * the resends), the pages it takes, the writer fed in order and paced by its flash, the pauses
 * and their reasons, the stall and the pause's time-out, a run going on across sessions and a
 * master restart, another image starting over; the budget, which a time-out at the gate (armed)
 * doesn't count; the INSTALL hold, CANCEL, a READY image's 24 h and removal; FW_STATE's timing;
 * PROBE, FETCH and the last section; the master's version kept as its orders name it (only the
 * paired master's, in frame order) and gone with the master, the cap following the pairing and a
 * stored version restored only on its own pairing's network; and the
 * STATUS flags read for the gate and for fire and CO.
 *
 * Requirements: LINK-002, LINK-003, LINK-006, LINK-008, LINK-014, LINK-015, LINK-016, LINK-017,
 * LINK-018, LINK-021, LINK-023, LINK-025, LINK-026, LINK-043, LINK-044, LINK-045, LINK-046,
 * LINK-047, LINK-048, LINK-049, LINK-050, LINK-051, LINK-052; updates: LINK-029, LINK-030,
 * LINK-031, LINK-032, LINK-033, LINK-034, LINK-035, LINK-036, LINK-037, LINK-039
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_snapshot.h"
#include "alarm_proto/proto_prefs.h"
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
#define FM_PREFS           64u
#define EV_PER_FRAME       32u
#define HOME_CAPS          (PLINK_CAP_HOME | PLINK_CAP_PREFS)
#define DEV_SUMMARY_MS     10000u /* the master's summary, 10 s after its last DEV_STATE */
#define FM_FW              1024u  /* FW_REQs, FW_STATEs and FW_INFOs the master keeps */
#define IMG_N              70001u /* the test image: not a whole number of pages */
#define IMG_PAGE           1024u  /* the orders' page (UDP, docs/features/11 "Per transport") */
#define IMG_COUNT          16u    /* ... and count: 16 KB a request */

static const uint8_t IP_M[4] = {192u, 0u, 2u, 10u};
static const uint8_t IP_K[4] = {192u, 0u, 2u, 20u};
static const uint8_t IP_X[4] = {198u, 51u, 100u, 7u};

static const plink_caps_t CAP = {ALARM_MAX_PARTITIONS, ALARM_MAX_ZONES, ALARM_MAX_USERS,
                                 ALARM_MAX_DEVICES};

/* ---- the scripted master ------------------------------------------------------------------ */

/* A PREFS the master got. */
typedef struct {
    uint64_t at;
    uint32_t req;
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    uint8_t voice;
    uint32_t accepts;
    uint8_t refused_count;
    uint8_t refused[2u * PLINK_PREFS_REFUSED_MAX];
    uint8_t fields_len;
    uint8_t fields[PLINK_PREFS_BODY_MAX];
} fm_prefs_t;

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
    plink_heartbeat_t hb; /* the newest HEARTBEAT, decoded */
    /* Smart Home and settings: how it behaves */
    uint16_t caps;       /* offered in its WELCOME */
    bool dev_on_session; /* a DEV_STATE summary at each new session, */
    bool dev_auto;       /* ... and 10 s after the last DEV_STATE */
    bool serve_dev_list, serve_dev_state;
    uint8_t dev_answer; /* a DEV_ACT is answered with result dev_answer - 1 (0: never) */
    uint8_t dev_why;
    uint64_t dev_summary_due;
    /* ... its registry, and the list and records made from it */
    hc_device_t rows[HC_MAX_DEVICES];
    size_t n_rows;
    uint8_t dev_blob[PLINK_DEV_BLOB_MAX];
    size_t dev_blob_len;
    uint32_t list_crc;
    plink_dev_recs_t recs;
    /* ... and what it saw */
    plink_dev_list_req_t list_seen[FM_SEEN];
    uint64_t list_at[FM_SEEN];
    size_t n_list;
    plink_dev_state_req_t sreq_seen[FM_SEEN];
    uint64_t sreq_at[FM_SEEN];
    size_t n_sreq;
    plink_dev_act_t act_seen[FM_SEEN];
    uint64_t act_at[FM_SEEN];
    size_t act_len[FM_SEEN];
    size_t n_act;
    fm_prefs_t prefs_seen[FM_PREFS];
    size_t n_prefs;
    /* updates through the link: its STATUS flags and how it serves FW_REQ, ... */
    bool updating, hold;
    bool serve_fw;         /* FW_REQ answered from IMG */
    uint16_t fw_page_cap;  /* its pages at most this (0: the request's) */
    unsigned fw_lose_from; /* the pages of a request from this one on are lost (0: none) */
    unsigned fw_lose_one;  /* ... only this one (1-based; 0: none) */
    uint32_t next_order;
    /* ... and what it saw */
    plink_fw_req_t fw_req[FM_FW];
    uint64_t fw_req_at[FM_FW];
    uint32_t fw_req_wrote[FM_FW]; /* the writer's bytes when it came */
    size_t n_fw_req;
    plink_fw_state_t fw_st[FM_FW];
    uint64_t fw_st_at[FM_FW];
    size_t n_fw_st;
    plink_fw_info_t fw_info;
    uint64_t fw_info_at[FM_FW];
    size_t n_fw_info;
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
static uint16_t KCAPS;          /* what the keypad offers (setUp: none) */
static bool KHOME;              /* a mirror attached */
static size_t KMTU;             /* its largest frame (0: PLINK_FRAME_MAX) */
static plink_dev_mirror_t KMIR; /* the mirror's memory */
static bool KNOWRITER;          /* its port has no relay writer */

static bool k_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return plink_net_send(&NET, KN, to, frame, len);
}

static size_t k_mtu(void *ctx, uint8_t transport)
{
    (void)ctx;
    (void)transport;
    return KMTU != 0u ? KMTU : PLINK_FRAME_MAX;
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

/* The keypad's service for updates (scripted): the orders it took, and the relay's writer. */
#define SV_ORDERS 64u
static struct {
    bool on;                  /* takes its orders after every call into the keypad */
    bool go;                  /* a new RELAY's writer opens at once (RECEIVING) */
    plink_fw_order_t taken[SV_ORDERS];
    size_t n_taken;
    uint32_t run;             /* the RELAY it writes */
    uint8_t img[IMG_N];
    uint32_t got;             /* bytes the writer took, in order */
    bool out_of_order;        /* a write not at the first byte it lacked */
    unsigned writes;
    size_t max_write;
    uint32_t take_max;        /* each write takes at most this (0: all) */
    uint32_t busy_ms;         /* ... and keeps the flash busy this long */
    uint64_t write_at[4096];
    size_t n_write_at;
} SV;

static uint32_t SV_GOT(void)
{
    return SV.got;
}

static size_t k_fw_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len,
                         uint32_t *busy_ms)
{
    (void)ctx;
    if (offset != SV.got) {
        SV.out_of_order = true;
        return 0;
    }
    size_t took = len;
    if (SV.take_max != 0u && took > SV.take_max) {
        took = SV.take_max;
    }
    TEST_ASSERT_TRUE(offset + took <= IMG_N);
    memcpy(&SV.img[offset], data, took);
    SV.got = offset + (uint32_t)took;
    SV.writes++;
    SV.max_write = len > SV.max_write ? len : SV.max_write;
    if (SV.n_write_at < sizeof SV.write_at / sizeof SV.write_at[0]) {
        SV.write_at[SV.n_write_at++] = NET.now_ms;
    }
    *busy_ms = SV.busy_ms;
    return took;
}

/* The service's turn: every order waiting is taken; a new RELAY restarts its writer and, with
 * go, opens it at once. */
static void sv_poll(uint64_t now)
{
    plink_fw_order_t o;
    while (plink_keypad_take_fw_order(&K, &o)) {
        if (SV.n_taken < SV_ORDERS) {
            SV.taken[SV.n_taken++] = o;
        }
        if (o.op == PLINK_FW_OP_RELAY) {
            SV.run = o.order;
            SV.got = 0;
            memset(SV.img, 0, sizeof SV.img);
            if (SV.go) {
                plink_keypad_fw_report(&K, now, o.order, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK);
            }
        }
    }
}

static void k_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t n)
{
    (void)ctx;
    plink_keypad_input(&K, now, from, f, n);
    if (SV.on) {
        sv_poll(now);
    }
}

static void k_tick(void *ctx, uint64_t now)
{
    (void)ctx;
    plink_keypad_tick(&K, now);
    if (SV.on) {
        sv_poll(now);
    }
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
                                    PLINK_REPORT_ONLINE, M.blob_crc, (uint32_t)M.blob_len,
                                    CAP.zones,           M.updating, M.hold};
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

/* ---- the master's Smart Home and settings ------------------------------------------------- */

/* A row of its registry. */
static hc_device_t *fm_row(const char *ref, uint8_t kind, uint8_t source, const char *state,
                           bool online)
{
    TEST_ASSERT_TRUE(M.n_rows < HC_MAX_DEVICES);
    hc_device_t *d = &M.rows[M.n_rows++];
    memset(d, 0, sizeof *d);
    d->info.kind = kind;
    d->info.source = source;
    snprintf(d->info.ref, sizeof d->info.ref, "%s", ref);
    snprintf(d->info.name, sizeof d->info.name, "%s", ref);
    snprintf(d->info.area, sizeof d->info.area, "Ground floor");
    snprintf(d->info.state, sizeof d->info.state, "%s", state);
    d->info.online = online;
    return d;
}

/* The list and the records of its registry now, as plink_dev builds them. */
static void fm_dev_build(void)
{
    TEST_ASSERT_TRUE(plink_home_list_build(M.rows, M.n_rows, M.dev_blob, sizeof M.dev_blob,
                                           &M.dev_blob_len, &M.list_crc));
    plink_home_recs_build(M.rows, M.n_rows, &M.recs);
}

static uint64_t bits_from(uint64_t mask, unsigned from)
{
    return from >= 64u ? 0u : mask >> from;
}

/* One DEV_STATE payload: the records of mask from *from on that fit (none: a summary). */
static size_t fm_dev_state_payload(uint64_t mask, unsigned *from, uint8_t *p, size_t cap)
{
    static uint8_t items[PLINK_PAYLOAD_MAX];
    size_t used = 0;
    uint8_t count = 0;
    unsigned i = *from;
    for (; i < M.recs.count; i++) {
        const size_t k = M.recs.len[i];
        if ((mask & ((uint64_t)1u << i)) == 0u || k == 0u) {
            continue;
        }
        if (used + 1u + k > cap - PLINK_DEV_STATE_HEAD) {
            break;
        }
        items[used] = (uint8_t)i;
        memcpy(&items[used + 1u], M.recs.rec[i], k);
        used += 1u + k;
        count++;
    }
    *from = i;
    const plink_dev_state_t s = {M.list_crc, plink_home_states_crc(&M.recs), count, items, used};
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_dev_state(&s, p, cap, &n));
    return n;
}

/* The records of mask, in as many DEV_STATE as they need (0: a summary). */
static void fm_dev_state(uint64_t mask)
{
    unsigned from = 0;
    do {
        uint8_t p[PLINK_PAYLOAD_MAX];
        const size_t n = fm_dev_state_payload(mask, &from, p, sizeof p);
        fm_send_sealed(PLINK_T_DEV_STATE, p, n);
    } while (from < M.recs.count && bits_from(mask, from) != 0u);
    M.dev_summary_due = NET.now_ms + DEV_SUMMARY_MS;
}

/* One DEV_STATE frame (sealed now, not sent) with the records of mask. */
static size_t fm_dev_state_frame(uint64_t mask, uint8_t *f)
{
    unsigned from = 0;
    uint8_t p[PLINK_PAYLOAD_MAX];
    const size_t n = fm_dev_state_payload(mask, &from, p, sizeof p);
    return fm_seal(PLINK_T_DEV_STATE, p, n, f);
}

/* One DEV_STATE frame (sealed now, not sent) under the master's list, naming states, carrying
 * record section sec (its length byte first) for device index. */
static size_t fm_dev_item_frame(uint8_t index, const uint8_t *sec, size_t len, uint32_t states,
                                uint8_t *f)
{
    uint8_t items[1u + 256u];
    items[0] = index;
    memcpy(&items[1], sec, len);
    const plink_dev_state_t s = {M.list_crc, states, 1u, items, 1u + len};
    uint8_t p[PLINK_PAYLOAD_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_dev_state(&s, p, sizeof p, &n));
    return fm_seal(PLINK_T_DEV_STATE, p, n, f);
}

/* A DEV_ACK with the record of the device named ref as it is now (none for NULL). */
static void fm_dev_ack(uint32_t req, uint8_t result, uint8_t why, const char *ref)
{
    plink_dev_ack_t a;
    memset(&a, 0, sizeof a);
    a.req = req;
    a.result = result;
    a.why = why;
    uint8_t index = 0;
    if (ref != NULL && plink_home_list_find(M.dev_blob, M.dev_blob_len, ref, &index) &&
        plink_home_rec(&M.recs, index, &a.rec)) {
        a.has_rec = true;
        a.list_crc = M.list_crc;
        a.index = index;
    }
    uint8_t p[PLINK_DEV_ACK_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_dev_ack(&a, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_DEV_ACK, p, n);
}

static void fm_dev_list_req(uint64_t now, const uint8_t *pt, size_t n)
{
    plink_dev_list_req_t q;
    if (!plink_dec_dev_list_req(pt, n, &q)) {
        return;
    }
    if (M.n_list < FM_SEEN) {
        M.list_seen[M.n_list] = q;
        M.list_at[M.n_list++] = now;
    }
    if (!M.serve_dev_list || M.dev_blob_len == 0u) {
        return;
    }
    /* As the master: another list's request gets the current one from its start. */
    const uint32_t off = q.cfg_crc == M.list_crc && q.offset < M.dev_blob_len ? q.offset : 0u;
    size_t len = M.dev_blob_len - off;
    if (q.max != 0u && len > q.max) {
        len = q.max;
    }
    if (len > PLINK_PAYLOAD_MAX - PLINK_CFG_PAGE_HDR) {
        len = PLINK_PAYLOAD_MAX - PLINK_CFG_PAGE_HDR;
    }
    const plink_dev_list_page_t pg = {M.list_crc, (uint32_t)M.dev_blob_len, off, &M.dev_blob[off],
                                      (uint16_t)len};
    uint8_t p[PLINK_PAYLOAD_MAX];
    size_t pl = 0;
    TEST_ASSERT_TRUE(plink_enc_dev_list_page(&pg, p, sizeof p, &pl));
    fm_send_sealed(PLINK_T_DEV_LIST_PAGE, p, pl);
}

static void fm_dev_state_req(uint64_t now, const uint8_t *pt, size_t n)
{
    plink_dev_state_req_t q;
    if (!plink_dec_dev_state_req(pt, n, &q)) {
        return;
    }
    if (M.n_sreq < FM_SEEN) {
        M.sreq_seen[M.n_sreq] = q;
        M.sreq_at[M.n_sreq++] = now;
    }
    if (!M.serve_dev_state) {
        return;
    }
    uint64_t mask = 0;
    const unsigned last = q.count == 0u ? M.recs.count : (unsigned)q.first + q.count;
    for (unsigned i = q.first; i < last && i < M.recs.count; i++) {
        mask |= (uint64_t)1u << i;
    }
    fm_dev_state(q.list_crc == M.list_crc ? mask : 0u);
}

static void fm_dev_act(uint64_t now, const uint8_t *pt, size_t n)
{
    plink_dev_act_t a;
    if (!plink_dec_dev_act(pt, n, &a)) {
        return;
    }
    if (M.n_act < FM_SEEN) {
        M.act_seen[M.n_act] = a;
        M.act_len[M.n_act] = n;
        M.act_at[M.n_act++] = now;
    }
    if (M.dev_answer != 0u) {
        fm_dev_ack(a.req, (uint8_t)(M.dev_answer - 1u), M.dev_why, a.ref);
    }
    plink_wipe(&a.code, sizeof a.code);
}

static void fm_prefs(uint64_t now, const uint8_t *pt, size_t n)
{
    plink_prefs_t r;
    if (!plink_dec_prefs(pt, n, &r) || M.n_prefs >= FM_PREFS) {
        return;
    }
    fm_prefs_t *s = &M.prefs_seen[M.n_prefs++];
    memset(s, 0, sizeof *s);
    s->at = now;
    s->req = r.req;
    memcpy(s->panel_id, r.panel_id, sizeof s->panel_id);
    s->voice = r.voice;
    s->accepts = r.accepts;
    s->refused_count = r.refused_count;
    TEST_ASSERT_TRUE(r.refused_count <= PLINK_PREFS_REFUSED_MAX);
    if (r.refused_count > 0u) {
        memcpy(s->refused, r.refused, 2u * r.refused_count);
    }
    s->fields_len = (uint8_t)r.fields_len;
    if (r.fields_len > 0u) {
        memcpy(s->fields, r.fields, r.fields_len);
    }
}

static void fm_prefs_req(uint32_t have_crc)
{
    const plink_prefs_req_t r = {have_crc};
    uint8_t p[16];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_prefs_req(&r, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_PREFS_REQ, p, n);
}

static size_t fm_prefs_set_payload(uint32_t req, uint8_t origin, const uint8_t *body, size_t len,
                                   uint8_t *p, size_t cap)
{
    plink_prefs_set_t s;
    memset(&s, 0, sizeof s);
    s.req = req;
    s.origin = origin;
    s.fields = body;
    s.fields_len = len;
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&s, p, cap, &n));
    return n;
}

static void fm_prefs_set(uint32_t req, uint8_t origin, const uint8_t *body, size_t len)
{
    uint8_t p[PLINK_PREFS_SET_MAX];
    const size_t n = fm_prefs_set_payload(req, origin, body, len, p, sizeof p);
    fm_send_sealed(PLINK_T_PREFS_SET, p, n);
}

/* ---- the master's updates ------------------------------------------------------------------ */

static uint8_t IMG[IMG_N];
static uint8_t IMG_SHA[PLINK_SHA256_LEN];
static uint8_t IMG2_SHA[PLINK_SHA256_LEN]; /* another image of the same size */

/* One FW_PAGE of the test image (as sent, or with a field changed by the test). */
static size_t fm_fw_page_payload(const uint8_t *id8, uint32_t total, uint32_t off, uint16_t len,
                                 uint8_t *p, size_t cap)
{
    const plink_fw_page_t pg = {{0}, total, off, &IMG[off], len};
    plink_fw_page_t m = pg;
    memcpy(m.id8, id8, PLINK_ID8_LEN);
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_fw_page(&m, p, cap, &n));
    return n;
}

static void fm_fw_page(const uint8_t *id8, uint32_t off, uint16_t len)
{
    uint8_t p[PLINK_PAYLOAD_MAX];
    const size_t n = fm_fw_page_payload(id8, IMG_N, off, len, p, sizeof p);
    fm_send_sealed(PLINK_T_FW_PAGE, p, n);
}

static uint32_t SV_GOT(void);

/* A FW_REQ: count pages of min(the request's page, fw_page_cap) from its offset, as the master
 * serves them (docs/features/11 "The master"). */
static void fm_fw_req(uint64_t now, const uint8_t *pt, size_t n)
{
    plink_fw_req_t q;
    TEST_ASSERT_TRUE(n <= PLINK_PAYLOAD_SMALL);
    if (!plink_dec_fw_req(pt, n, &q)) {
        return;
    }
    if (M.n_fw_req < FM_FW) {
        M.fw_req[M.n_fw_req] = q;
        M.fw_req_wrote[M.n_fw_req] = SV_GOT();
        M.fw_req_at[M.n_fw_req++] = now;
    }
    if (!M.serve_fw || (memcmp(q.id8, IMG_SHA, PLINK_ID8_LEN) != 0 &&
                        memcmp(q.id8, IMG2_SHA, PLINK_ID8_LEN) != 0)) {
        return;
    }
    uint16_t page = q.page;
    if (M.fw_page_cap != 0u && page > M.fw_page_cap) {
        page = M.fw_page_cap;
    }
    uint32_t off = q.offset;
    for (unsigned i = 0; i < q.count && off < IMG_N; i++) {
        const uint32_t len = IMG_N - off < page ? IMG_N - off : page;
        if ((M.fw_lose_from == 0u || i + 1u < M.fw_lose_from) && M.fw_lose_one != i + 1u) {
            fm_fw_page(q.id8, off, (uint16_t)len);
        }
        off += len;
    }
}

/* An order, sealed and sent with the next order id; returns the id. */
static uint32_t fm_order(plink_fw_order_t *o)
{
    o->order = M.next_order++;
    uint8_t p[PLINK_FW_ORDER_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_fw_order(o, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_FW_ORDER, p, n);
    return o->order;
}

/* The same order again (a resend: the same id). */
static void fm_order_again(const plink_fw_order_t *o)
{
    uint8_t p[PLINK_FW_ORDER_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_fw_order(o, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_FW_ORDER, p, n);
}

static plink_fw_order_t order_of(uint8_t op, const char *version)
{
    plink_fw_order_t o;
    memset(&o, 0, sizeof o);
    o.op = op;
    o.kind = PLINK_FW_KIND_APP;
    snprintf(o.version, sizeof o.version, "%s", version);
    snprintf(o.server, sizeof o.server, "https://updates.example.org");
    snprintf(o.channel, sizeof o.channel, "stable");
    return o;
}

/* A RELAY of the test image (sha: IMG_SHA or IMG2_SHA). */
static plink_fw_order_t relay_of(const uint8_t *sha, uint16_t gap_ms)
{
    plink_fw_order_t o = order_of(PLINK_FW_OP_RELAY, "1.3.0");
    o.size = IMG_N;
    memcpy(o.sha256, sha, sizeof o.sha256);
    memset(o.key, 0x6B, sizeof o.key);
    o.page = IMG_PAGE;
    o.count = IMG_COUNT;
    o.gap_ms = gap_ms;
    return o;
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
    if (!plink_hs_answer(&M.c, M.k_pair, h, f, len, M.caps, &pend, &hello, M.welcome,
                         sizeof M.welcome, &M.welcome_len)) {
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
        if (M.dev_on_session && M.list_crc != 0u) {
            fm_dev_state(0u); /* names its list */
        }
    }
    switch (h->type) {
    case PLINK_T_HEARTBEAT:
        M.hbs++;
        M.hb_pt_len = n < sizeof M.hb_pt ? n : sizeof M.hb_pt;
        memcpy(M.hb_pt, pt, M.hb_pt_len);
        TEST_ASSERT_TRUE(plink_dec_heartbeat(pt, n, &M.hb));
        break;
    case PLINK_T_DEV_LIST_REQ: fm_dev_list_req(now, pt, n); break;
    case PLINK_T_DEV_STATE_REQ: fm_dev_state_req(now, pt, n); break;
    case PLINK_T_DEV_ACT: fm_dev_act(now, pt, n); break;
    case PLINK_T_PREFS: fm_prefs(now, pt, n); break;
    case PLINK_T_FW_INFO:
        TEST_ASSERT_TRUE(plink_dec_fw_info(pt, n, &M.fw_info));
        TEST_ASSERT_TRUE(n <= PLINK_PAYLOAD_SMALL); /* LINK-037 */
        if (M.n_fw_info < FM_FW) {
            M.fw_info_at[M.n_fw_info++] = now;
        }
        break;
    case PLINK_T_FW_STATE:
        TEST_ASSERT_TRUE(n <= PLINK_PAYLOAD_SMALL);
        if (M.n_fw_st < FM_FW) {
            TEST_ASSERT_TRUE(plink_dec_fw_state(pt, n, &M.fw_st[M.n_fw_st]));
            M.fw_st_at[M.n_fw_st++] = now;
        }
        break;
    case PLINK_T_FW_REQ: fm_fw_req(now, pt, n); break;
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
    if (M.dev_auto && M.list_crc != 0u && M.live.valid && M.have_kp && now >= M.dev_summary_due) {
        fm_dev_state(0u);
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
    if (M.dev_auto && M.list_crc != 0u && M.live.valid && M.have_kp && M.dev_summary_due < t) {
        t = M.dev_summary_due;
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
    KCAPS = 0u;
    KHOME = false;
    KMTU = 0u;
    KNOWRITER = false;
    memset(&SV, 0, sizeof SV);
    M.next_order = 0x7E570001u;
    for (size_t i = 0; i < IMG_N; i++) {
        IMG[i] = (uint8_t)((i * 2654435761u) >> 13);
    }
    const plink_iov_t img = {IMG, IMG_N};
    plink_sha256(&img, 1, IMG_SHA);
    memcpy(IMG2_SHA, IMG_SHA, sizeof IMG2_SHA);
    IMG2_SHA[0] ^= 0xFFu;
}

void tearDown(void)
{
    /* LINK-003: no AES-CCM key and nonce was ever used twice, on either side. */
    TEST_ASSERT_FALSE_MESSAGE(TPK.reused, "the keypad used a key and nonce twice");
    TEST_ASSERT_FALSE_MESSAGE(M.tp.reused, "the master used a key and nonce twice");
}

/* A keypad, paired or not; a paired one is told the master is at `at` (a broadcast when bcast). */
static void kp_init_at(bool paired, const plink_addr_t *at, bool bcast)
{
    memset(&REC, 0, sizeof REC);
    REC.net_id = NET_ID;
    REC.device = DEV;
    REC.home = HOME;
    memcpy(REC.k_pair, M.k_pair, sizeof REC.k_pair);
    snprintf(REC.master_name, sizeof REC.master_name, "Harbor House");
    snprintf(REC.panel_id, sizeof REC.panel_id, "A1B2C3");
    const plink_keypad_port_t port = {.ctx = NULL,
                                      .send = k_send,
                                      .mtu = k_mtu,
                                      .store = k_store,
                                      .resolve = k_resolve,
                                      .fw_write = KNOWRITER ? NULL : k_fw_write};
    plink_keypad_init(&K, &CK, &port, paired ? &REC : NULL, KBUF, sizeof KBUF, &KCFG,
                      PLINK_KIND_PANEL, &CAP, KCAPS, "Hall keypad", "1.2.3", NET.now_ms);
    if (KHOME) {
        plink_keypad_home_attach(&K, &KMIR);
    }
    if (paired) {
        plink_keypad_master_addr(&K, NET.now_ms, at, bcast);
    }
}

static void kp_init(bool paired)
{
    kp_init_at(paired, plink_net_addr(&NET, MN), false);
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
    plink_keypad_pair_start(&K, NET.now_ms, plink_net_addr(&NET, MN), false, NET_ID);
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
    plink_pm_init(&M.pm, &M.c, 0u);
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

/* LINK-025: a keypad that is not paired - never paired, or removed at the master - opens the
 * no-code pages at once: no master could check a code, and it protects nothing. Paired, it
 * doesn't. */
static void test_maintenance_when_not_paired(void)
{
    kp_init(false);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, info().state);
    TEST_ASSERT_TRUE(info().maintenance);
    kp_online();
    TEST_ASSERT_FALSE(info().maintenance);
    uint8_t p[4];
    size_t n = 0;
    const plink_bye_t bye = {1u};
    TEST_ASSERT_TRUE(plink_enc_bye(&bye, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_BYE, p, n);
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_REMOVED, info().unpaired_why);
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
static uint64_t FRAME_MONO; /* status_frame's master engine time (0: the snapshot's) */

static size_t status_frame(uint32_t troubles, const plink_part_t *recs, uint8_t count, uint8_t *f)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    const plink_status_extra_t x = {M.epoch_ms,          HOME,       0u,
                                    PLINK_REPORT_ONLINE, M.blob_crc, (uint32_t)M.blob_len,
                                    CAP.zones,           M.updating, M.hold};
    plink_status_build(&SNAP, SNAP.taken_ms, &x, &ST);
    ST.sys_troubles = troubles;
    if (FRAME_MONO != 0u) {
        ST.mono_ms = FRAME_MONO;
    }
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

/* LINK-028: the master's engine time comes only from the newest system section: an older frame
 * whose partition record is still taken never sets it back (a keypad's voice dates events by it). */
static void test_master_engine_time_never_steps_back(void)
{
    kp_online();
    M.auto_status = false;
    plink_part_t r;
    static uint8_t fa[PLINK_FRAME_MAX], fb[PLINK_FRAME_MAX];
    part_rec(&r, 1u, ALARM_STATE_EXIT_DELAY);
    FRAME_MONO = 40000u;
    const size_t na = status_frame(0x04u, &r, 1u, fa); /* partition 1, sealed first */
    part_rec(&r, 2u, ALARM_STATE_ARMED);
    FRAME_MONO = 50000u;
    const size_t nb = status_frame(0x08u, &r, 1u, fb); /* the newest system section */
    FRAME_MONO = 0u;
    deliver(fb, nb);
    TEST_ASSERT_EQUAL_UINT64(50000u, plink_keypad_mono_ms(&K, NET.now_ms));
    deliver(fa, na);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, plink_keypad_snapshot(&K)->part[0].state);
    TEST_ASSERT_EQUAL_UINT64(50000u, plink_keypad_mono_ms(&K, NET.now_ms));
    NET.now_ms += 1500u;
    TEST_ASSERT_EQUAL_UINT64(51500u, plink_keypad_mono_ms(&K, NET.now_ms));
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
    return K.cfg_pull.pulling && K.view.have_status;
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

static bool lost(void *ctx)
{
    (void)ctx;
    return info().state == PLINK_LINK_LOST;
}

/* LINK-017, LINK-008: a configuration this keypad couldn't read says "update this keypad" only
 * while the master's latest status names it. Another configuration that installs, or the master
 * going back to the installed one (its release rolled back, a configuration shrunk again), ends
 * it for good: a link lost days later is only a lost link, not a reason to update. */
static void test_config_bad_forgotten_when_another_installs(void)
{
    /* Unreadable first, then a readable one installs. */
    M.bad_blob = true;
    fm_blob();
    const uint32_t bad = M.blob_crc;
    kp_init(true);
    plink_net_run(&NET, 5000);
    TEST_ASSERT_EQUAL_HEX32(bad, info().cfg_bad_crc);
    M.bad_blob = false;
    fm_blob();
    fm_status();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, online, NULL));
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc);
    TEST_ASSERT_EQUAL_HEX32(0u, K.cfg_pull.bad_crc);
    NET.cut_from = MN;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_LOST_MS + 2000u, lost, NULL));
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc);
    NET.cut_from = -1;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_REHANDSHAKE_MS + 10000u, online, NULL));

    /* Readable and installed, then an unreadable one, then the installed one named again. */
    const uint32_t good = M.blob_crc;
    M.bad_blob = true;
    snprintf(S.cfg.zones[0].name, ALARM_NAME_LEN, "Front door, new name");
    fm_blob();
    fm_status();
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_HEX32(M.blob_crc, info().cfg_bad_crc); /* the master's: update */
    M.bad_blob = false;
    snprintf(S.cfg.zones[0].name, ALARM_NAME_LEN, "Zone 1, north garden window");
    fm_blob();
    TEST_ASSERT_EQUAL_HEX32(good, M.blob_crc);
    fm_status();
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc);
    TEST_ASSERT_EQUAL_HEX32(0u, K.cfg_pull.bad_crc);
    NET.cut_from = MN;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_LOST_MS + 2000u, lost, NULL));
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc);
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

/* docs/features/11 "Event record": a keypad whose new session's first STATUS was lost, and that
 * kept only the events pushed before the next one (whose last_event_seq is then no newer than
 * them, so nothing is pulled for being behind), holds fewer than 50 with older ones at the
 * master: after that STATUS it asks for the newest 50 again and ends with them. A ring that is
 * complete from event 1 isn't asked for again. */
static void test_short_ring_backfilled(void)
{
    arm_disarm(40);
    TEST_ASSERT_TRUE(S.n_events > 60u);
    M.pushed = S.events[S.n_events - 1u].seq;
    NET.lose_type = PLINK_T_STATUS;
    NET.lose_type_count = 1u; /* the new session's first */
    kp_init(true);
    plink_net_run(&NET, 500);
    TEST_ASSERT_TRUE(K.live.valid);
    TEST_ASSERT_FALSE(K.view.have_status);
    arm_disarm(1);
    fm_push_events();
    plink_net_run(&NET, 10);
    TEST_ASSERT_TRUE(K.ev_count > 0u && K.ev_count < PLINK_EVENTS_KEPT);
    TEST_ASSERT_EQUAL_UINT(0u, M.ev_reqs);
    const uint32_t newest = S.events[S.n_events - 1u].seq;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, FM_STATUS_EVERY_MS + 2000u, events_full, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, M.ev_reqs);
    TEST_ASSERT_EQUAL_UINT32(newest - PLINK_EVENTS_KEPT, M.ev_req.after_seq);
    for (size_t i = 0; i < PLINK_EVENTS_KEPT; i++) {
        TEST_ASSERT_EQUAL_UINT32(S.events[S.n_events - PLINK_EVENTS_KEPT + i].seq,
                                 K.events[i].seq);
    }
    plink_net_run(&NET, 3u * FM_STATUS_EVERY_MS);
    TEST_ASSERT_EQUAL_UINT(1u, M.ev_reqs); /* full now: not asked again */
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

/* LINK-023, LINK-018: the same panic keyed again while it is still being sent is the same
 * request, and its caller waits for it again: the first caller let it go (the UI's 3 s), yet the
 * answer doesn't free the slot before the second reads it - DONE, run once - and the slot goes
 * when that caller lets go. */
static void test_panic_keyed_again_is_waited_for_again(void)
{
    kp_online();
    M.serve_cmds = false; /* no answer yet */
    const plink_cmd_t p = panic_cmd();
    const uint32_t req = plink_keypad_command(&K, NET.now_ms, &p);
    TEST_ASSERT_NOT_EQUAL(0u, req);
    plink_net_run(&NET, 3000);
    plink_keypad_command_release(&K, req);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, plink_keypad_command_state(&K, req, NULL));
    TEST_ASSERT_EQUAL_UINT32(req, plink_keypad_command(&K, NET.now_ms, &p)); /* keyed again */
    M.serve_cmds = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, panic_done, NULL));
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, plink_keypad_command_state(&K, req, &res));
    TEST_ASSERT_EQUAL_UINT32(req, res.req);
    TEST_ASSERT_EQUAL_UINT(1u, M.runs);
    plink_keypad_command_release(&K, req);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NONE, plink_keypad_command_state(&K, req, NULL));
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
    plink_keypad_master_addr(&K, NET.now_ms, &NET.node[MN].addr, false); /* discovery found it */
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

/* LINK-015, docs/features/11 "Sessions": a new handshake starts 15 s after the last authenticated
 * STATUS, though other sealed frames of the session keep coming (a TIME every 2 s here): a session
 * whose STATUS doesn't get through is renewed, and the new session brings a STATUS at once, so
 * the keypad never comes near LOST. The 15 s count from the WELCOME too: no second handshake
 * follows while the new session's STATUS flows. */
static void test_handshake_after_15_s_without_status(void)
{
    kp_online();
    M.auto_status = false;
    fm_status();
    plink_net_run(&NET, 10);
    const uint64_t last = K.status_ms;
    const unsigned hellos = M.hellos;
    const uint32_t old_sid = K.live.sid;
    while (NET.now_ms + 2000u < last + PLINK_REHANDSHAKE_MS) {
        plink_net_run(&NET, 2000);
        fm_time(M.epoch_ms, "UTC0");
    }
    run_to(last + PLINK_REHANDSHAKE_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(hellos, M.hellos);
    TEST_ASSERT_TRUE(K.time_frame_seq != 0u); /* the TIME frames were taken */
    run_to(last + PLINK_REHANDSHAKE_MS + 2u * NET.delay_ms);
    TEST_ASSERT_EQUAL_UINT(hellos + 1u, M.hellos);
    /* The new session: promoted on its first HEARTBEAT, its STATUS at once. */
    plink_net_run(&NET, 1000);
    TEST_ASSERT_NOT_EQUAL(old_sid, K.live.sid);
    TEST_ASSERT_EQUAL_UINT32(M.live.sid, K.live.sid);
    TEST_ASSERT_TRUE(K.status_ms > last + PLINK_REHANDSHAKE_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    M.auto_status = true;
    M.status_due = NET.now_ms + FM_STATUS_EVERY_MS;
    plink_net_run(&NET, 3u * PLINK_REHANDSHAKE_MS);
    TEST_ASSERT_EQUAL_UINT(hellos + 1u, M.hellos);
}

static bool have_session(void *ctx)
{
    (void)ctx;
    return K.live.valid && !K.hs.waiting;
}

/* LINK-015, LINK-021 (docs/features/11 "Sessions"): a WELCOME is worked out (an X25519 before its
 * mac2 can be checked) only when it comes from where the HELLO went - the genuine answer from
 * another address is not even worked out - and for at most PLINK_WELCOME_CHECKS per HELLO: a
 * flood of forged ones from the master's address costs 8 X25519 and then nothing, even the
 * genuine one waits; the next HELLO opens the count again and its answer is taken. */
static void test_welcome_only_from_its_hello_and_counted(void)
{
    NET.lose_type = PLINK_T_WELCOME; /* the master's answers are kept from the keypad */
    kp_init(true);
    plink_net_run(&NET, 5);
    TEST_ASSERT_TRUE(K.hs.waiting);
    static uint8_t w[PLINK_FRAME_MAX], f[PLINK_FRAME_MAX];
    size_t wl = 0;
    TEST_ASSERT_TRUE(plink_net_last(&NET, MN, PLINK_T_WELCOME, w, sizeof w, &wl));
    const plink_addr_t other = plink_addr_udp(IP_X, PLINK_UDP_PORT);
    const uint32_t calls = TPK.hc.calls;
    plink_keypad_input(&K, NET.now_ms, &other, w, wl);
    TEST_ASSERT_EQUAL_UINT32(calls, TPK.hc.calls);
    TEST_ASSERT_TRUE(K.hs.waiting);
    TEST_ASSERT_FALSE(K.live.valid);
    memcpy(f, w, wl);
    f[wl - 1u] ^= 0x01u; /* mac2 */
    for (unsigned i = 0; i < 3u * PLINK_WELCOME_CHECKS; i++) {
        deliver(f, wl);
    }
    TEST_ASSERT_EQUAL_UINT32(calls + PLINK_WELCOME_CHECKS, TPK.hc.calls);
    deliver(w, wl);
    TEST_ASSERT_EQUAL_UINT32(calls + PLINK_WELCOME_CHECKS, TPK.hc.calls);
    TEST_ASSERT_TRUE(K.hs.waiting);
    TEST_ASSERT_FALSE(K.live.valid);
    NET.lose_type = 0u;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, have_session, NULL));
    TEST_ASSERT_EQUAL_UINT(2u, M.hellos);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
}

/* LINK-021, LINK-015: on a /25 whose mDNS is blocked the keypad says HELLO to its subnet's
 * broadcast, x.y.z.127, which the service names as one (its bytes don't say so): the master's
 * WELCOME comes from the master's own address and is taken, the master's address is learnt from
 * it, and the link comes up. */
static void test_hello_by_subnet_broadcast(void)
{
    static const uint8_t ip_b[4] = {192u, 0u, 2u, 127u};
    memcpy(NET.subnet_bcast, ip_b, sizeof ip_b);
    const plink_addr_t b = plink_addr_udp(ip_b, (uint16_t)PLINK_UDP_PORT);
    kp_init_at(true, &b, true);
    TEST_ASSERT_TRUE_MESSAGE(plink_net_run_until(&NET, 5000, online, NULL), "not online");
    TEST_ASSERT_TRUE(hello_sent_to((void *)&b));
    TEST_ASSERT_EQUAL_UINT(1u, M.hellos);
    TEST_ASSERT_TRUE(master_is(plink_net_addr(&NET, MN)));
}

/* LINK-021: the same subnet broadcast named as a unicast address (the bug this guards against,
 * the other way round): a WELCOME from anywhere but where the HELLO went is not worked out. */
static void test_hello_to_unnamed_broadcast_takes_no_other_welcome(void)
{
    static const uint8_t ip_b[4] = {192u, 0u, 2u, 127u};
    memcpy(NET.subnet_bcast, ip_b, sizeof ip_b);
    const plink_addr_t b = plink_addr_udp(ip_b, (uint16_t)PLINK_UDP_PORT);
    NET.lose_type = PLINK_T_WELCOME; /* the master's answers are kept from the keypad */
    kp_init_at(true, &b, false);
    plink_net_run(&NET, 5);
    TEST_ASSERT_EQUAL_UINT(1u, M.hellos); /* the master heard it and answered */
    TEST_ASSERT_TRUE(K.hs.waiting);
    static uint8_t w[PLINK_FRAME_MAX];
    size_t wl = 0;
    TEST_ASSERT_TRUE(plink_net_last(&NET, MN, PLINK_T_WELCOME, w, sizeof w, &wl));
    const uint32_t calls = TPK.hc.calls;
    plink_keypad_input(&K, NET.now_ms, plink_net_addr(&NET, MN), w, wl);
    TEST_ASSERT_EQUAL_UINT32(calls, TPK.hc.calls); /* not worked out */
    TEST_ASSERT_TRUE(K.hs.waiting);
    TEST_ASSERT_FALSE(K.live.valid);
}

/* LINK-021, LINK-015: a master whose own (unicast) address ends in .255 - a host on a /23 - is
 * no broadcast: its WELCOME is worked out only from that address, as any master's. */
static void test_master_ending_255_keeps_welcome_check(void)
{
    static const uint8_t ip_255[4] = {198u, 51u, 100u, 255u};
    NET.node[MN].addr = plink_addr_udp(ip_255, (uint16_t)PLINK_UDP_PORT);
    NET.lose_type = PLINK_T_WELCOME; /* the master's answers are kept from the keypad */
    kp_init_at(true, &NET.node[MN].addr, false);
    plink_net_run(&NET, 5);
    TEST_ASSERT_TRUE(K.hs.waiting);
    TEST_ASSERT_FALSE(K.hs_bcast);
    static uint8_t w[PLINK_FRAME_MAX];
    size_t wl = 0;
    TEST_ASSERT_TRUE(plink_net_last(&NET, MN, PLINK_T_WELCOME, w, sizeof w, &wl));
    const plink_addr_t other = plink_addr_udp(IP_X, PLINK_UDP_PORT);
    const uint32_t calls = TPK.hc.calls;
    plink_keypad_input(&K, NET.now_ms, &other, w, wl);
    TEST_ASSERT_EQUAL_UINT32(calls, TPK.hc.calls); /* not even worked out */
    TEST_ASSERT_TRUE(K.hs.waiting);
    plink_keypad_input(&K, NET.now_ms, &NET.node[MN].addr, w, wl); /* worked out, and taken */
    TEST_ASSERT_TRUE(TPK.hc.calls > calls);
    TEST_ASSERT_TRUE(K.live.valid);
    NET.lose_type = 0u;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    TEST_ASSERT_TRUE(master_is(&NET.node[MN].addr));
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
    plink_keypad_master_addr(&K, NET.now_ms, &x, false); /* discovery, while the session is up */
    TEST_ASSERT_TRUE(master_is(m));
    TEST_ASSERT_TRUE(plink_addr_equal(&x, &K.disc));
    fl = fm_seal(PLINK_T_STATUS, p, pl, f);
    plink_keypad_input(&K, NET.now_ms, &x, f, fl); /* genuine, from a new address */
    TEST_ASSERT_TRUE(master_is(&x));
    plink_keypad_master_addr(&K, NET.now_ms, m, false);
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

/* ---- Smart Home and settings (LINK-043..052) ---------------------------------------------- */

/* The master's devices: five of Home Assistant's, a Z-Wave switch, an offline light. */
enum { R_HALL, R_LOCK, R_PORCH, R_GARAGE, R_CLIMATE, R_ZWAVE, R_ATTIC, R_COUNT };

static void home_rows(void)
{
    fm_row("light.hall", DEV_LIGHT, DSRC_HA, "on", true);
    fm_row("lock.front", DEV_LOCK, DSRC_HA, "locked", true);
    fm_row("switch.porch", DEV_SWITCH, DSRC_HA, "off", true);
    fm_row("cover.garage", DEV_COVER, DSRC_HA, "closed", true);
    fm_row("climate.living", DEV_CLIMATE, DSRC_HA, "heat", true);
    fm_row("switch.zw_7", DEV_SWITCH, DSRC_ZWAVE, "off", true);
    fm_row("light.attic", DEV_LIGHT, DSRC_HA, "unavailable", false);
}

/* Both offer HOME and PREFS, the keypad with a mirror; the master serves the rows above. */
static void home_rig(void)
{
    M.caps = KCAPS = HOME_CAPS;
    KHOME = true;
    M.dev_on_session = M.dev_auto = M.serve_dev_list = M.serve_dev_state = true;
    home_rows();
    fm_dev_build();
}

static plink_dev_info_t dev_info(void)
{
    plink_dev_info_t d;
    plink_keypad_dev_info(&K, NET.now_ms, &d);
    return d;
}

/* The keypad's mirror is the master's: its list installed, its records' CRC the master's. */
static bool home_synced(void *ctx)
{
    (void)ctx;
    return M.list_crc != 0u && KMIR.list_crc == M.list_crc &&
           plink_home_states_crc(&KMIR.recs) == plink_home_states_crc(&M.recs);
}

static bool list_installed(void *ctx)
{
    (void)ctx;
    return KMIR.list_crc == M.list_crc;
}

static bool stale(void *ctx)
{
    (void)ctx;
    return !dev_info().fresh;
}

static bool no_panic(void *ctx)
{
    (void)ctx;
    return !plink_keypad_panic_pending(&K);
}

static bool new_session(void *ctx)
{
    (void)ctx;
    return M.promotions >= 2u && K.items_sid == K.live.sid && !K.tentative;
}

static void kp_home_online(void)
{
    kp_online();
    TEST_ASSERT_TRUE_MESSAGE(plink_net_run_until(&NET, 5000, home_synced, NULL), "no mirror");
}

/* The keypad's record of row i. */
static plink_dev_rec_t krec(uint8_t i)
{
    plink_dev_rec_t r;
    memset(&r, 0, sizeof r);
    TEST_ASSERT_TRUE(plink_keypad_dev_rec(&K, i, &r));
    return r;
}

static plink_dev_cat_t krow(uint8_t i)
{
    plink_dev_cat_t c;
    bool usable = false;
    TEST_ASSERT_TRUE(plink_keypad_dev_row(&K, i, &c, &usable));
    TEST_ASSERT_TRUE(usable);
    return c;
}

/* Row i's state word at the master: its records change, its list doesn't. */
static void fm_state(unsigned i, const char *state)
{
    snprintf(M.rows[i].info.state, sizeof M.rows[i].info.state, "%s", state);
    fm_dev_build();
}

/* An action on the keypad, answered as expect at once; its ticket. */
static uint32_t act(const char *ref, uint8_t action, int32_t value, const char *code,
                    uint8_t expect)
{
    uint32_t t = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_UINT8(expect,
                            plink_keypad_dev_act(&K, NET.now_ms, ref, action, value, 0u, code, &t));
    TEST_ASSERT_TRUE(expect == PLINK_DEV_OK ? t != 0u : t == 0u);
    return t;
}

static uint8_t act_mode(const char *ref, uint8_t mode)
{
    uint32_t t = 0;
    return plink_keypad_dev_act(&K, NET.now_ms, ref, ACT_SET_MODE, 0, mode, NULL, &t);
}

static plink_cmd_state_t act_state(uint32_t t, plink_dev_answer_t *a)
{
    return plink_keypad_dev_act_state(&K, t, a);
}

static const plink_kact_t *kact_of(uint32_t req)
{
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        if (K.acts[i].used && K.acts[i].req == req) {
            return &K.acts[i];
        }
    }
    TEST_FAIL_MESSAGE("no such action");
    return NULL;
}

static uint8_t overlay(uint8_t row, uint8_t *action)
{
    return plink_keypad_dev_overlay(&K, row, action);
}

/* A fields body: the brightness and the Beeps volume. */
static size_t prefs_body(uint8_t brightness, uint8_t volume, uint8_t *out)
{
    proto_prefs_values_t v;
    memset(&v, 0, sizeof v);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE,
                          proto_prefs_put(&v, PROTO_PREFS_F_BRIGHTNESS, brightness));
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&v, PROTO_PREFS_F_VOLUME, volume));
    const size_t n = proto_prefs_encode(&v, PROTO_PREFS_KNOWN, out, PROTO_PREFS_ENC_MAX);
    TEST_ASSERT_TRUE(n > 0u);
    return n;
}

static bool report(const uint8_t *body, size_t len)
{
    return plink_keypad_prefs_report(&K, NET.now_ms, "a1b2c3", PLINK_VOICE_READY, PROTO_PREFS_KNOWN,
                                     body, len);
}

/* LINK-043: a keypad offers HOME only with a mirror attached (a build without home control, or
 * without its memory, never does; PAIR_REQ's caps follow), PREFS whenever its caller offers it,
 * and the session shares only what both sides offer. Without a mirror there is no Smart Home: an
 * action is UNAVAILABLE and no list is pulled. */
static void test_home_offered_only_with_a_mirror(void)
{
    M.caps = KCAPS = HOME_CAPS;
    M.dev_on_session = M.dev_auto = M.serve_dev_list = M.serve_dev_state = true;
    home_rows();
    fm_dev_build();
    kp_online();
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_PREFS, info().caps);
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_PREFS, K.caps);
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_PREFS, K.pair.caps);
    TEST_ASSERT_FALSE(dev_info().attached);
    TEST_ASSERT_FALSE(dev_info().shared);
    act("light.hall", ACT_ON, -1, NULL, PLINK_DEV_UNAVAILABLE);
    plink_net_run(&NET, 2000);
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_DEV_LIST_REQ));
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_DEV_ACT));

    /* With one: both bits shared, and the master's list mirrored. */
    fm_restart();
    KHOME = true;
    kp_home_online();
    TEST_ASSERT_EQUAL_HEX16(HOME_CAPS, info().caps);
    TEST_ASSERT_EQUAL_HEX16(HOME_CAPS, K.pair.caps);
    TEST_ASSERT_TRUE(dev_info().attached);
    TEST_ASSERT_TRUE(dev_info().shared);

    /* A master offering neither: nothing shared. */
    fm_restart();
    M.caps = 0u;
    kp_online();
    TEST_ASSERT_EQUAL_HEX16(0u, info().caps);
    TEST_ASSERT_TRUE(dev_info().attached);
    TEST_ASSERT_FALSE(dev_info().shared);
}

/* LINK-043: Smart Home's and the settings' frames go only while the master's transport carries
 * frames of 250 B or more, asked at every send: on a smaller one the keypad sends no list or
 * state request and no PREFS, refuses an action at once and leaves out the HEARTBEAT section; once
 * its frames are 250 B again they all go. */
static void test_home_needs_frames_of_250(void)
{
    home_rig();
    KMTU = PLINK_FRAME_ESPNOW - 1u;
    kp_init(true);
    uint8_t body[PROTO_PREFS_ENC_MAX];
    const size_t bl = prefs_body(80u, 30u, body);
    TEST_ASSERT_TRUE(report(body, bl));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 20000, online, NULL));
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 2000u);
    TEST_ASSERT_EQUAL_HEX16(HOME_CAPS, info().caps);
    TEST_ASSERT_TRUE(M.hbs >= 2u);
    TEST_ASSERT_FALSE(M.hb.has_prefs);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_list);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_sreq);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_prefs);
    TEST_ASSERT_NOT_EQUAL(0u, dev_info().heard_ms); /* what the master sends is still heard */
    TEST_ASSERT_EQUAL_HEX32(0u, dev_info().list_crc);
    act("light.hall", ACT_ON, -1, NULL, PLINK_DEV_UNREACHABLE);
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_DEV_ACT));

    KMTU = PLINK_FRAME_ESPNOW;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, home_synced, NULL));
    TEST_ASSERT_TRUE(M.n_list > 0u);
    TEST_ASSERT_TRUE(M.n_prefs >= 1u);
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_TRUE(M.hb.has_prefs);
    M.dev_answer = 1u + PLINK_DEV_OK;
    const uint32_t t = act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t, NULL));
}

/* LINK-044, docs/features/11 "The device list": the list is pulled as the configuration is - 0 to
 * 500 ms after the DEV_STATE naming it, its first page alone (its length comes with it), each
 * request retried after 300 ms doubling to 2 s - then two pages out at a time; 64 devices
 * install whole, every row the master's. */
static void test_dev_list_pull_requests(void)
{
    home_rig();
    M.n_rows = 0;
    for (unsigned i = 0; i < HC_MAX_DEVICES; i++) {
        char ref[40];
        snprintf(ref, sizeof ref, "light.room_%02u_ceiling", i);
        hc_device_t *d = fm_row(ref, DEV_LIGHT, DSRC_HA, "on", true);
        snprintf(d->info.name, sizeof d->info.name, "Room %02u ceiling light by the north window",
                 i);
        snprintf(d->info.area, sizeof d->info.area, "Upstairs, the north wing over the garage");
    }
    fm_dev_build();
    TEST_ASSERT_TRUE(M.dev_blob_len > 3u * PAGE);
    M.serve_dev_list = false;
    kp_online();
    plink_net_run(&NET, 7000);
    TEST_ASSERT_TRUE(M.n_list >= 6u);
    TEST_ASSERT_TRUE(M.list_at[0] >= M.promoted_at + 2u &&
                     M.list_at[0] <= M.promoted_at + 2u + 500u + 2u);
    static const uint32_t gaps[] = {300u, 600u, 1200u, 2000u, 2000u};
    for (size_t i = 0; i < 6u; i++) {
        TEST_ASSERT_EQUAL_HEX32(M.list_crc, M.list_seen[i].cfg_crc);
        TEST_ASSERT_EQUAL_UINT32(0u, M.list_seen[i].offset);
        TEST_ASSERT_EQUAL_UINT16(PAGE, M.list_seen[i].max);
        if (i > 0u) {
            TEST_ASSERT_EQUAL_UINT64(gaps[i - 1u], M.list_at[i] - M.list_at[i - 1u]);
        }
    }
    TEST_ASSERT_EQUAL_HEX32(0u, dev_info().list_crc);

    const size_t from = M.n_list;
    M.serve_dev_list = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, home_synced, NULL));
    TEST_ASSERT_TRUE(M.n_list >= from + 3u);
    TEST_ASSERT_EQUAL_UINT32(0u, M.list_seen[from].offset);
    TEST_ASSERT_EQUAL_UINT32(PAGE, M.list_seen[from + 1u].offset);
    TEST_ASSERT_EQUAL_UINT32(2u * PAGE, M.list_seen[from + 2u].offset);
    TEST_ASSERT_EQUAL_UINT64(M.list_at[from + 1u], M.list_at[from + 2u]); /* two out at once */
    TEST_ASSERT_EQUAL_UINT8(HC_MAX_DEVICES, dev_info().count);
    for (uint8_t i = 0; i < HC_MAX_DEVICES; i++) {
        const plink_dev_cat_t c = krow(i);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.ref, c.ref);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.name, c.name);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.area, c.area);
    }
}

/* LINK-044, LINK-045: once the list is whole it is installed - every row the master's, in its
 * order - and every record under it is asked for at once (DEV_STATE_REQ list_crc, 0, 0); the
 * records are kept as they came, so the keypad's states_crc is the master's. */
static void test_dev_list_installed_with_its_records(void)
{
    home_rig();
    kp_home_online();
    const plink_dev_info_t d = dev_info();
    TEST_ASSERT_EQUAL_HEX32(M.list_crc, d.list_crc);
    TEST_ASSERT_EQUAL_UINT8(R_COUNT, d.count);
    TEST_ASSERT_TRUE(d.fresh);
    TEST_ASSERT_TRUE(d.shared);
    for (uint8_t i = 0; i < R_COUNT; i++) {
        const plink_dev_cat_t c = krow(i);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.ref, c.ref);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.name, c.name);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.area, c.area);
        TEST_ASSERT_EQUAL_UINT8(M.rows[i].info.kind, c.kind);
        TEST_ASSERT_EQUAL_UINT8(M.rows[i].info.source, c.source);
        const plink_dev_rec_t r = krec(i);
        TEST_ASSERT_EQUAL_STRING(M.rows[i].info.state, r.word);
        TEST_ASSERT_EQUAL(M.rows[i].info.online, (r.flags & PLINK_DEVF_ONLINE) != 0u);
        TEST_ASSERT_EQUAL_UINT8(M.recs.len[i], KMIR.recs.len[i]);
        TEST_ASSERT_EQUAL_MEMORY(M.recs.rec[i], KMIR.recs.rec[i], M.recs.len[i]);
    }
    TEST_ASSERT_FALSE(plink_keypad_dev_row(&K, R_COUNT, NULL, NULL));
    TEST_ASSERT_TRUE(M.n_sreq >= 1u);
    TEST_ASSERT_EQUAL_HEX32(M.list_crc, M.sreq_seen[0].list_crc);
    TEST_ASSERT_EQUAL_UINT8(0u, M.sreq_seen[0].first);
    TEST_ASSERT_EQUAL_UINT8(0u, M.sreq_seen[0].count);
    TEST_ASSERT_TRUE(M.sreq_at[0] <= M.list_at[M.n_list - 1u] + 10u); /* at the install */
}

/* LINK-044: a list this firmware can't read - a newer master's compat, or bytes that don't match
 * the CRC named - is pulled once, remembered by its CRC and never pulled again; the list
 * installed stays, and the keypad asks for no update (devices aren't alarm-critical). */
static void test_dev_list_bad_blob_remembered(void)
{
    home_rig();
    M.dev_blob[0] = PLINK_DEV_CAT_COMPAT + 1u;
    M.list_crc = plink_home_list_crc(M.dev_blob, M.dev_blob_len);
    kp_online();
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_HEX32(M.list_crc, K.dev_pull.bad_crc);
    TEST_ASSERT_EQUAL_HEX32(0u, dev_info().list_crc);
    const size_t reqs = M.n_list;
    TEST_ASSERT_TRUE(reqs >= 1u);
    plink_net_run(&NET, 3u * DEV_SUMMARY_MS); /* three summaries naming it */
    TEST_ASSERT_EQUAL_size_t(reqs, M.n_list);
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);

    fm_dev_build();
    fm_dev_state(0u);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, home_synced, NULL));
    const uint32_t good = M.list_crc;
    snprintf(M.rows[R_HALL].info.name, sizeof M.rows[R_HALL].info.name, "Hall ceiling");
    fm_dev_build();
    M.list_crc ^= 0x00010000u;
    const uint32_t lying = M.list_crc;
    const size_t reqs2 = M.n_list;
    fm_dev_state(0u);
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_HEX32(lying, K.dev_pull.bad_crc);
    TEST_ASSERT_EQUAL_HEX32(good, dev_info().list_crc);
    TEST_ASSERT_EQUAL_STRING("light.hall", krow(R_HALL).name);
    const size_t reqs3 = M.n_list;
    TEST_ASSERT_TRUE(reqs3 > reqs2);
    plink_net_run(&NET, 2u * DEV_SUMMARY_MS);
    TEST_ASSERT_EQUAL_size_t(reqs3, M.n_list);
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc);
}

/* LINK-044: a new list leaves the old one installed until it is whole; at its install each row
 * keeps the last record of the old row with the same ref (a rename never blinks; a new row has
 * none until its own comes), LINK-026's count starts again, and every record is asked for. */
static void test_dev_list_change_carries_records_by_ref(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    const uint32_t a = M.list_crc;
    const plink_dev_rec_t hall = krec(R_HALL);
    /* At the master: the lock renamed and opened, a fan listed first. */
    snprintf(M.rows[R_LOCK].info.name, sizeof M.rows[R_LOCK].info.name, "Front door lock");
    snprintf(M.rows[R_LOCK].info.state, sizeof M.rows[R_LOCK].info.state, "unlocked");
    memmove(&M.rows[1], &M.rows[0], M.n_rows * sizeof M.rows[0]);
    const size_t n = M.n_rows + 1u;
    M.n_rows = 0;
    fm_row("fan.bedroom", DEV_FAN, DSRC_HA, "on", true);
    M.n_rows = n;
    fm_dev_build();
    const uint32_t b = M.list_crc;
    TEST_ASSERT_NOT_EQUAL(a, b);
    M.serve_dev_list = M.serve_dev_state = false;
    const uint32_t lseq = dev_info().list_seq;
    fm_dev_state(0u);
    plink_net_run(&NET, 1000);
    TEST_ASSERT_TRUE(K.dev_pull.pulling);
    TEST_ASSERT_EQUAL_HEX32(b, K.dev_pull.crc);
    TEST_ASSERT_EQUAL_HEX32(a, dev_info().list_crc); /* the old one meanwhile, whole */
    TEST_ASSERT_EQUAL_UINT8(R_COUNT, dev_info().count);
    TEST_ASSERT_EQUAL_STRING("lock.front", krow(R_LOCK).name);
    TEST_ASSERT_EQUAL_STRING("locked", krec(R_LOCK).word);

    M.serve_dev_list = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, list_installed, NULL));
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT32(lseq + 1u, dev_info().list_seq);
    TEST_ASSERT_EQUAL_UINT8(R_COUNT + 1u, dev_info().count);
    TEST_ASSERT_EQUAL_STRING("fan.bedroom", krow(0).ref);
    plink_dev_rec_t r;
    TEST_ASSERT_FALSE(plink_keypad_dev_rec(&K, 0, &r)); /* new: none yet */
    TEST_ASSERT_EQUAL_STRING("light.hall", krow(1).ref);
    TEST_ASSERT_EQUAL_STRING(hall.word, krec(1).word);
    TEST_ASSERT_EQUAL_STRING("Front door lock", krow(2).name);
    TEST_ASSERT_EQUAL_STRING("locked", krec(2).word); /* carried by ref until its own comes */
    TEST_ASSERT_EACH_EQUAL_UINT32(0u, KMIR.frame_seq, PLINK_DEV_MAX);
    TEST_ASSERT_EQUAL_HEX32(b, M.sreq_seen[M.n_sreq - 1u].list_crc);
    TEST_ASSERT_EQUAL_UINT8(0u, M.sreq_seen[M.n_sreq - 1u].count);

    M.serve_dev_state = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, home_synced, NULL));
    TEST_ASSERT_EQUAL_STRING("unlocked", krec(2).word);
    TEST_ASSERT_EQUAL_STRING("on", krec(0).word);

    /* A rename alone: every record carried matches, and every one is still asked for. */
    snprintf(M.rows[1].info.name, sizeof M.rows[1].info.name, "Hall ceiling");
    fm_dev_build();
    const size_t s0 = M.n_sreq;
    fm_dev_state(0u);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, list_installed, NULL));
    TEST_ASSERT_TRUE(home_synced(NULL));
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_size_t(s0 + 1u, M.n_sreq);
    TEST_ASSERT_EQUAL_HEX32(M.list_crc, M.sreq_seen[s0].list_crc);
    TEST_ASSERT_EQUAL_STRING("Hall ceiling", krow(1).name);
}

/* LINK-026, LINK-045: a record applies only from a newer frame of the session than the last one
 * applied for that device - each device by its own count - and a new session starts the count
 * again. */
static void test_dev_records_in_frame_order(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    M.serve_dev_state = false;
    static uint8_t fa[PLINK_FRAME_MAX], fb[PLINK_FRAME_MAX];
    fm_state(R_HALL, "off");
    fm_state(R_LOCK, "unlocked");
    const size_t na = fm_dev_state_frame((1u << R_HALL) | (1u << R_LOCK), fa);
    fm_state(R_HALL, "dimmed");
    const size_t nb = fm_dev_state_frame(1u << R_HALL, fb);
    const uint32_t s0 = dev_info().seq;
    deliver(fb, nb);
    TEST_ASSERT_EQUAL_STRING("dimmed", krec(R_HALL).word);
    deliver(fa, na);
    TEST_ASSERT_EQUAL_STRING("dimmed", krec(R_HALL).word);   /* older for the light: skipped */
    TEST_ASSERT_EQUAL_STRING("unlocked", krec(R_LOCK).word); /* newer for the lock: taken */
    TEST_ASSERT_EQUAL_UINT32(s0 + 2u, dev_info().seq);

    fm_rekey(K.live.sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, new_session, NULL));
    fm_state(R_HALL, "off");
    const size_t nc = fm_dev_state_frame(1u << R_HALL, fa);
    deliver(fa, nc);
    TEST_ASSERT_EQUAL_STRING("off", krec(R_HALL).word);
    TEST_ASSERT_TRUE(KMIR.frame_seq[R_HALL] < 10u);
}

/* LINK-045, LINK-040: a record is kept as it came, fields a newer master appended included, so
 * the keypad's states_crc is the master's and nothing is asked, while the fields it knows read as
 * before. A record longer than any it keeps whole is kept as the fields it reads: its CRC then
 * differs, and the keypad asks. */
static void test_dev_records_kept_as_they_came(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    M.serve_dev_state = false;
    plink_dev_rec_t r;
    TEST_ASSERT_TRUE(plink_home_rec(&M.recs, R_PORCH, &r));
    snprintf(r.word, sizeof r.word, "on");
    uint8_t sec[64];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_dev_rec(&r, sec, sizeof sec, &n));
    sec[n] = 0xA1u; /* a newer master's three bytes, inside the section */
    sec[n + 1u] = 0xB2u;
    sec[n + 2u] = 0xC3u;
    sec[0] = (uint8_t)(sec[0] + 3u);
    n += 3u;
    TEST_ASSERT_TRUE(n <= PLINK_DEV_REC_MAX);
    TEST_ASSERT_TRUE(plink_home_rec_put(&M.recs, R_PORCH, sec, n));
    const size_t s0 = M.n_sreq;
    static uint8_t f[PLINK_FRAME_MAX];
    size_t fl = fm_dev_item_frame(R_PORCH, sec, n, plink_home_states_crc(&M.recs), f);
    deliver(f, fl);
    TEST_ASSERT_EQUAL_STRING("on", krec(R_PORCH).word);
    TEST_ASSERT_EQUAL_UINT8(n, KMIR.recs.len[R_PORCH]);
    TEST_ASSERT_EQUAL_MEMORY(sec, KMIR.recs.rec[R_PORCH], n);
    TEST_ASSERT_TRUE(home_synced(NULL));
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_size_t(s0, M.n_sreq);

    snprintf(r.word, sizeof r.word, "unavailable_for_a_while"); /* 23: the longest */
    TEST_ASSERT_TRUE(plink_enc_dev_rec(&r, sec, sizeof sec, &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_REC_MAX, n);
    memset(&sec[n], 0x5A, 6u);
    sec[0] = (uint8_t)(sec[0] + 6u);
    n += 6u;
    uint32_t crc = 0u; /* the master's states_crc with that record */
    for (unsigned i = 0; i < M.recs.count; i++) {
        const uint8_t idx = (uint8_t)i;
        crc = alarm_crc32(crc, &idx, 1u);
        crc = i == R_PORCH ? alarm_crc32(crc, sec, n)
                           : alarm_crc32(crc, M.recs.rec[i], M.recs.len[i]);
    }
    fl = fm_dev_item_frame(R_PORCH, sec, n, crc, f);
    deliver(f, fl);
    TEST_ASSERT_EQUAL_STRING("unavailable_for_a_while", krec(R_PORCH).word);
    plink_net_run(&NET, 300);
    TEST_ASSERT_EQUAL_size_t(s0 + 1u, M.n_sreq);
}

/* LINK-045, LINK-046, LINK-047: a record applies only under the installed list - a DEV_STATE or
 * DEV_ACK naming another list changes no record (an index means another device there) and is no
 * device data for the rows shown, which stay offline - and the DEV_STATE naming the new list
 * starts its pull; once it installs, its records make them fresh again. */
static void test_dev_records_only_under_installed_list(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    M.serve_dev_list = M.serve_dev_state = false;
    snprintf(M.rows[R_HALL].info.name, sizeof M.rows[R_HALL].info.name, "Hall ceiling");
    fm_state(R_HALL, "off"); /* another list */
    plink_net_run(&NET, PLINK_DEV_STALE_MS + 1000u);
    TEST_ASSERT_FALSE(dev_info().fresh);
    const uint64_t last = dev_info().heard_ms;
    fm_dev_state(1u << R_HALL);
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_STRING("on", krec(R_HALL).word);
    TEST_ASSERT_FALSE(dev_info().fresh);
    fm_dev_ack(0x1234u, PLINK_DEV_OK, 0u, "light.hall");
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_STRING("on", krec(R_HALL).word);
    TEST_ASSERT_FALSE(dev_info().fresh);
    TEST_ASSERT_EQUAL_UINT64(last, dev_info().heard_ms); /* "Offline since" the old list's data */
    TEST_ASSERT_TRUE(K.dev_pull.pulling);
    TEST_ASSERT_EQUAL_HEX32(M.list_crc, K.dev_pull.crc);

    M.serve_dev_list = M.serve_dev_state = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, home_synced, NULL));
    TEST_ASSERT_EQUAL_STRING("off", krec(R_HALL).word);
    TEST_ASSERT_TRUE(dev_info().fresh);
}

/* LINK-044, LINK-046: a list the keypad can't install - here a newer master's compat - is
 * remembered and never pulled again, and the old list stays; its summaries keep coming every 10 s,
 * but they name that list, so they are no device data for the old list's rows: 30 s after the
 * last data under the old list every row reads offline since then, and a tap sends nothing,
 * rather than frozen states shown as live. A list that installs brings them back. */
static void test_dev_list_that_fails_reads_offline(void)
{
    home_rig();
    kp_home_online();
    const uint32_t good = M.list_crc;
    plink_net_run(&NET, 100);
    const uint64_t last = dev_info().heard_ms;
    TEST_ASSERT_TRUE(dev_info().fresh);
    snprintf(M.rows[R_HALL].info.name, sizeof M.rows[R_HALL].info.name, "Hall ceiling");
    fm_dev_build();
    M.dev_blob[0] = PLINK_DEV_CAT_COMPAT + 1u;
    M.list_crc = plink_home_list_crc(M.dev_blob, M.dev_blob_len);
    const uint32_t newer = M.list_crc;
    const size_t states0 = plink_net_count(&NET, MN, PLINK_T_DEV_STATE);
    fm_dev_state(0u); /* names it at once; then a summary every 10 s (dev_auto) */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_DEV_STALE_MS + 1000u, stale, NULL));
    TEST_ASSERT_EQUAL_UINT64(last + PLINK_DEV_STALE_MS, NET.now_ms);
    TEST_ASSERT_TRUE(plink_net_count(&NET, MN, PLINK_T_DEV_STATE) >= states0 + 3u);
    TEST_ASSERT_EQUAL_HEX32(newer, K.dev_pull.bad_crc);
    TEST_ASSERT_EQUAL_HEX32(good, dev_info().list_crc); /* the old list stays */
    TEST_ASSERT_EQUAL_UINT8(R_COUNT, dev_info().count);
    TEST_ASSERT_EQUAL_UINT64(last, dev_info().heard_ms);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    TEST_ASSERT_EQUAL_HEX32(0u, info().cfg_bad_crc); /* no update asked for */
    const size_t acts = plink_net_count(&NET, KN, PLINK_T_DEV_ACT);
    act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_UNREACHABLE);
    plink_net_run(&NET, 20000);
    TEST_ASSERT_FALSE(dev_info().fresh);
    TEST_ASSERT_EQUAL_size_t(acts, plink_net_count(&NET, KN, PLINK_T_DEV_ACT));

    fm_dev_build(); /* the master's list readable again */
    fm_dev_state(0u);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, home_synced, NULL)); /* records carried */
    TEST_ASSERT_FALSE(dev_info().fresh);
    plink_net_run(&NET, 100); /* the records it asked for at the install */
    TEST_ASSERT_TRUE(dev_info().fresh);
    TEST_ASSERT_TRUE(dev_info().heard_ms > last + PLINK_DEV_STALE_MS);
    TEST_ASSERT_EQUAL_STRING("Hall ceiling", krow(R_HALL).name);
}

/* LINK-045: while the records' CRC differs from the master's under the installed list, the
 * keypad asks for every record 250 ms on, then at least 1 s apart, doubling to 30 s; records
 * that come within the 250 ms (a full set takes several frames) settle it with nothing asked; a
 * match ends it, and the next mismatch starts at 250 ms and 1 s again. */
static void test_dev_states_catch_up(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    M.serve_dev_state = false;
    size_t from = M.n_sreq;
    fm_state(R_PORCH, "on");
    fm_dev_state(0u); /* its head first: the CRCs differ */
    plink_net_run(&NET, 100);
    fm_dev_state(1u << R_PORCH);
    plink_net_run(&NET, 2000);
    TEST_ASSERT_EQUAL_size_t(from, M.n_sreq);
    TEST_ASSERT_TRUE(home_synced(NULL));

    fm_state(R_PORCH, "off");
    const uint64_t t0 = NET.now_ms;
    fm_dev_state(0u); /* the record lost */
    plink_net_run(&NET, 65000);
    TEST_ASSERT_TRUE(M.n_sreq >= from + 7u);
    TEST_ASSERT_EQUAL_UINT64(t0 + 2u + PLINK_DEV_REQ_WAIT_MS + 2u, M.sreq_at[from]);
    static const uint32_t gaps[] = {1000u, 2000u, 4000u, 8000u, 16000u, 30000u};
    for (size_t i = 0; i < 6u; i++) {
        TEST_ASSERT_EQUAL_HEX32(M.list_crc, M.sreq_seen[from + i].list_crc);
        TEST_ASSERT_EQUAL_UINT8(0u, M.sreq_seen[from + i].first);
        TEST_ASSERT_EQUAL_UINT8(0u, M.sreq_seen[from + i].count);
        TEST_ASSERT_EQUAL_UINT64(gaps[i], M.sreq_at[from + i + 1u] - M.sreq_at[from + i]);
    }

    fm_dev_state(1u << R_PORCH);
    plink_net_run(&NET, 100);
    TEST_ASSERT_TRUE(home_synced(NULL));
    TEST_ASSERT_FALSE(K.dev_want);
    plink_net_run(&NET, 2000);
    from = M.n_sreq;
    fm_state(R_PORCH, "on");
    const uint64_t t1 = NET.now_ms;
    fm_dev_state(0u);
    plink_net_run(&NET, 1500);
    TEST_ASSERT_EQUAL_size_t(from + 2u, M.n_sreq);
    TEST_ASSERT_EQUAL_UINT64(t1 + 2u + PLINK_DEV_REQ_WAIT_MS + 2u, M.sreq_at[from]);
    TEST_ASSERT_EQUAL_UINT64(PLINK_DEV_REQ_GAP_MS, M.sreq_at[from + 1u] - M.sreq_at[from]);
}

/* LINK-046: device data counts as fresh for 30 s from the last DEV_STATE (a summary too) or
 * DEV_ACK record under the installed list - each summary moves heard_ms, but not seq, as nothing
 * shown changes; past that, or while the link isn't ONLINE, every row reads offline since the
 * last device data, and an action sends nothing. */
static void test_dev_freshness_30_s(void)
{
    home_rig();
    kp_home_online();
    TEST_ASSERT_TRUE(dev_info().fresh);
    const plink_dev_info_t d0 = dev_info();
    plink_net_run(&NET, 35000); /* a summary every 10 s */
    TEST_ASSERT_TRUE(dev_info().fresh);
    TEST_ASSERT_TRUE(dev_info().heard_ms >= d0.heard_ms + 3u * DEV_SUMMARY_MS);
    TEST_ASSERT_EQUAL_UINT32(d0.seq, dev_info().seq);
    M.dev_auto = false;
    plink_net_run(&NET, 100); /* nothing on its way */
    const uint64_t last = dev_info().heard_ms;
    const uint32_t seq = dev_info().seq;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_DEV_STALE_MS + 1000u, stale, NULL));
    TEST_ASSERT_EQUAL_UINT64(last + PLINK_DEV_STALE_MS, NET.now_ms);
    TEST_ASSERT_EQUAL_UINT64(last, dev_info().heard_ms);
    TEST_ASSERT_EQUAL_UINT32(seq + 1u, dev_info().seq); /* the platform redraws */
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, info().state);
    act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_UNREACHABLE);
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_DEV_ACT));

    fm_dev_ack(0x54u, PLINK_DEV_OK, 0u, NULL); /* an answer with no record: no device data */
    plink_net_run(&NET, 10);
    TEST_ASSERT_FALSE(dev_info().fresh);
    fm_dev_ack(0x55u, PLINK_DEV_OK, 0u, "light.hall"); /* a DEV_ACK's record is device data too */
    plink_net_run(&NET, 10);
    TEST_ASSERT_TRUE(dev_info().fresh);

    /* Not ONLINE: offline, however recent the device data. */
    M.dev_auto = true;
    M.dev_summary_due = NET.now_ms;
    NET.lose_type = PLINK_T_STATUS;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_LOST_MS + 12000u, lost, NULL));
    TEST_ASSERT_TRUE(NET.now_ms < dev_info().heard_ms + PLINK_DEV_STALE_MS);
    TEST_ASSERT_FALSE(dev_info().fresh);
    act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_UNREACHABLE);
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_DEV_ACT));
}

/* LINK-046..048, docs/features/11 "Device actions": an action that can't go is answered at once
 * and sends nothing - not ONLINE, or a row offline: UNREACHABLE; a ref the list doesn't hold, a
 * Z-Wave row (until BL-169), an action its kind doesn't take, a mode that isn't one bit, a code
 * that isn't digits: REFUSED; an unlock or an opening without a code: NEED_CODE (the pad opens
 * with no round trip; the master judges again); an action on a device whose last one is still
 * being sent: BUSY. Lock, close and the modes never ask for a code. */
static void test_dev_act_refused_at_once(void)
{
    home_rig();
    kp_init(true);
    act("light.hall", ACT_ON, -1, NULL, PLINK_DEV_UNREACHABLE); /* not ONLINE yet */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, home_synced, NULL));
    TEST_ASSERT_TRUE(online(NULL));
    M.dev_answer = 0u;
    act("light.attic", ACT_ON, -1, NULL, PLINK_DEV_UNREACHABLE);
    act("light.nowhere", ACT_ON, -1, NULL, PLINK_DEV_REFUSED);
    act("light.hal", ACT_ON, -1, NULL, PLINK_DEV_REFUSED);
    act("switch.zw_7", ACT_ON, -1, NULL, PLINK_DEV_REFUSED);
    act("light.hall", ACT_UNLOCK, 0, "1234", PLINK_DEV_REFUSED);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_REFUSED, act_mode("climate.living", 0x03u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_REFUSED, act_mode("climate.living", 0x10u));
    act("lock.front", ACT_UNLOCK, 0, NULL, PLINK_DEV_NEED_CODE);
    act("lock.front", ACT_UNLOCK, 0, "", PLINK_DEV_NEED_CODE);
    act("cover.garage", ACT_OPEN, 0, NULL, PLINK_DEV_NEED_CODE);
    act("cover.garage", ACT_SET_LEVEL, 40, NULL, PLINK_DEV_NEED_CODE);
    act("lock.front", ACT_UNLOCK, 0, "12a4", PLINK_DEV_REFUSED);
    act("lock.front", ACT_UNLOCK, 0, "12345678901", PLINK_DEV_REFUSED);
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_DEV_ACT));

    act("cover.garage", ACT_CLOSE, 0, NULL, PLINK_DEV_OK);
    act("cover.garage", ACT_STOP, 0, NULL, PLINK_DEV_BUSY);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OK, act_mode("climate.living", PLINK_DEV_MODE_HEAT));
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_size_t(2u, M.n_act);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_MODE_HEAT, M.act_seen[1].mode);
    TEST_ASSERT_EQUAL_UINT8(0u, M.act_seen[0].mode);
}

/* LINK-047: an action goes at once, then again every 500 ms with the same req - every copy
 * PLINK_DEV_ACT_LEN bytes - until 3 s have passed, never after; its code is wiped after its last
 * copy. Given up, it reads NO_ANSWER; once its caller stopped waiting, its device shows it pending
 * while it is sent, then failed and unsure ("Not confirmed"), until a later record. */
static void test_dev_act_resent_then_given_up(void)
{
    home_rig();
    kp_home_online();
    M.dev_answer = 0u;
    NET.log_count = 0;
    const uint64_t t0 = NET.now_ms;
    const uint32_t t = act("lock.front", ACT_UNLOCK, 0, "1234", PLINK_DEV_OK);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, act_state(t, NULL));
    uint8_t action = 0;
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, overlay(R_LOCK, &action));
    plink_keypad_dev_act_release(&K, t);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_PENDING, overlay(R_LOCK, &action));
    TEST_ASSERT_EQUAL_UINT8(ACT_UNLOCK, action);
    run_to(t0 + 2400u);
    TEST_ASSERT_FALSE(all_zero(kact_of(t)->act.code, sizeof kact_of(t)->act.code));
    run_to(t0 + 2600u);
    TEST_ASSERT_TRUE(all_zero(kact_of(t)->act.code, sizeof kact_of(t)->act.code));
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, act_state(t, NULL));
    run_to(t0 + 6000u);
    uint64_t at[16];
    const size_t n = sent_times(KN, PLINK_T_DEV_ACT, at, 16);
    TEST_ASSERT_EQUAL_size_t(6u, n);
    TEST_ASSERT_EQUAL_size_t(6u, M.n_act);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT64(t0 + i * PLINK_DEV_ACT_RETRY_MS, at[i]);
        TEST_ASSERT_EQUAL_UINT32(t, M.act_seen[i].req);
        TEST_ASSERT_EQUAL_size_t(PLINK_DEV_ACT_LEN, M.act_len[i]);
        TEST_ASSERT_EQUAL_STRING("lock.front", M.act_seen[i].ref);
        TEST_ASSERT_EQUAL_STRING("1234", M.act_seen[i].code);
        TEST_ASSERT_EQUAL_UINT8(ACT_UNLOCK, M.act_seen[i].action);
    }
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NO_ANSWER, act_state(t, NULL));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_UNSURE, overlay(R_LOCK, &action));
    TEST_ASSERT_EQUAL_UINT8(ACT_UNLOCK, action);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, overlay(R_HALL, NULL));
    fm_dev_state(1u << R_LOCK);
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, overlay(R_LOCK, NULL));
}

/* LINK-047: the answer is the DEV_ACK with the action's req: its record - the device as it is now -
 * goes into the mirror at once, the code is wiped, and the caller reads the result (for a
 * refused code, why). An answer after its caller stopped waiting: OK shows the record, a refusal
 * shows it failed - definite, even after the give-up - until a later record; the copies of that
 * answer don't end it. An unknown result reads REFUSED. */
static void test_dev_act_answered(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    M.dev_answer = 1u + PLINK_DEV_OK;
    fm_state(R_HALL, "off");
    const uint32_t t1 = act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_OK);
    plink_net_run(&NET, 20);
    plink_dev_answer_t ans;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t1, &ans));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OK, ans.result);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, ans.overlay);
    TEST_ASSERT_EQUAL_STRING("off", krec(R_HALL).word);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_act);

    M.dev_answer = 1u + PLINK_DEV_CODE_REFUSED;
    M.dev_why = ALARM_ERR_LOCKED_OUT;
    const uint32_t t2 = act("lock.front", ACT_UNLOCK, 0, "9999", PLINK_DEV_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t2, &ans));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_CODE_REFUSED, ans.result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_LOCKED_OUT, ans.why);
    TEST_ASSERT_TRUE(all_zero(kact_of(t2)->act.code, sizeof kact_of(t2)->act.code));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, overlay(R_LOCK, NULL)); /* the caller saw it */

    M.dev_answer = 0u;
    const uint32_t t3 = act("switch.porch", ACT_ON, 0, NULL, PLINK_DEV_OK);
    plink_keypad_dev_act_release(&K, t3);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_PENDING, overlay(R_PORCH, NULL));
    fm_dev_ack(t3, PLINK_DEV_REFUSED, 0u, "switch.porch");
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t3, &ans));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_REFUSED, ans.result);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_FAILED, overlay(R_PORCH, NULL));
    fm_dev_ack(t3, PLINK_DEV_REFUSED, 0u, "switch.porch"); /* another copy's answer */
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_FAILED, overlay(R_PORCH, NULL));
    fm_dev_state(1u << R_PORCH);
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, overlay(R_PORCH, NULL));

    const uint32_t t4 = act("switch.porch", ACT_OFF, 0, NULL, PLINK_DEV_OK);
    plink_keypad_dev_act_release(&K, t4);
    plink_net_run(&NET, PLINK_DEV_ACT_GIVE_UP_MS + 100u);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NO_ANSWER, act_state(t4, NULL));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_UNSURE, overlay(R_PORCH, NULL));
    fm_dev_ack(t4, PLINK_DEV_OK, 0u, "switch.porch"); /* late */
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t4, &ans));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OK, ans.result);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OVL_NONE, overlay(R_PORCH, NULL));

    M.dev_answer = 1u + 9u;
    const uint32_t t5 = act("light.hall", ACT_ON, -1, NULL, PLINK_DEV_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t5, &ans));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_REFUSED, ans.result);
}

/* Row i as the keypad's screen gets it. */
static plink_dev_view_t view(uint8_t i)
{
    plink_dev_view_t v;
    TEST_ASSERT_TRUE(plink_keypad_dev_view(&K, NET.now_ms, i, &v));
    TEST_ASSERT_TRUE(v.usable);
    return v;
}

/* LINK-046, LINK-047, docs/features/11 "Device actions" step 7: the screen shows a row as the
 * master's record says - its fields, words, values and flags, an action under way whoever started
 * it - with this keypad's own action over it once its caller stopped waiting: pending while it is
 * sent, failed for a late refusal, failed and unsure once given up ("Not confirmed"), until a later
 * record. Once the device data is stale every row reads offline since the last device data; a row
 * the master already said is offline keeps its own time. */
static void test_dev_view_for_the_screen(void)
{
    home_rig();
    M.rows[R_CLIMATE].info.values.flags = DEV_VAL_SETPOINT;
    M.rows[R_CLIMATE].info.values.setpoint = 2150;
    M.rows[R_CLIMATE].info.values.hvac_action = DEV_HVAC_HEATING;
    M.rows[R_ATTIC].info.values.since = 1790000000u;
    fm_dev_build();
    plink_dev_view_t v;
    TEST_ASSERT_FALSE(plink_keypad_dev_view(&K, NET.now_ms, 0u, &v)); /* no list yet */
    kp_home_online();
    v = view(R_CLIMATE);
    TEST_ASSERT_TRUE(v.have_rec && v.online);
    TEST_ASSERT_FALSE(v.pending || v.failed || v.unsure || v.since_last_data);
    TEST_ASSERT_EQUAL_STRING("climate.living", v.cat.ref);
    TEST_ASSERT_EQUAL_STRING("Ground floor", v.cat.area);
    TEST_ASSERT_EQUAL_UINT8(DEV_CLIMATE, v.cat.kind);
    TEST_ASSERT_EQUAL_UINT8(DSRC_HA, v.cat.source);
    TEST_ASSERT_EQUAL_STRING("heat", v.word);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_SETPOINT, v.values.flags);
    TEST_ASSERT_EQUAL_INT32(2150, v.values.setpoint);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_HEATING, v.values.hvac_action);
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, v.action);
    v = view(R_ATTIC);
    TEST_ASSERT_FALSE(v.online || v.since_last_data);
    TEST_ASSERT_EQUAL_UINT32(1790000000u, v.values.since);
    TEST_ASSERT_FALSE(plink_keypad_dev_view(&K, NET.now_ms, R_COUNT, &v));

    /* The master's record of an action under way, whoever started it */
    M.rows[R_HALL].pending = 5u;
    M.rows[R_HALL].action = ACT_OFF;
    fm_dev_build();
    fm_dev_state(1u << R_HALL);
    plink_net_run(&NET, 10);
    v = view(R_HALL);
    TEST_ASSERT_TRUE(v.pending && v.online);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, v.action);
    M.rows[R_HALL].pending = 0u;
    fm_dev_build();
    fm_dev_state(1u << R_HALL);
    plink_net_run(&NET, 10);
    TEST_ASSERT_FALSE(view(R_HALL).pending);

    /* This keypad's own: nothing over the record while its caller waits; pending once released;
     * a late refusal failed (definite) until a later record */
    M.dev_answer = 0u;
    const uint32_t t1 = act("switch.porch", ACT_ON, 0, NULL, PLINK_DEV_OK);
    v = view(R_PORCH);
    TEST_ASSERT_FALSE(v.pending || v.failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, v.action);
    plink_keypad_dev_act_release(&K, t1);
    v = view(R_PORCH);
    TEST_ASSERT_TRUE(v.pending && v.online);
    TEST_ASSERT_FALSE(v.failed || v.unsure);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, v.action);
    fm_dev_ack(t1, PLINK_DEV_REFUSED, 0u, "switch.porch");
    plink_net_run(&NET, 10);
    v = view(R_PORCH);
    TEST_ASSERT_TRUE(v.failed && v.online);
    TEST_ASSERT_FALSE(v.pending || v.unsure);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, v.action);
    fm_dev_state(1u << R_PORCH);
    plink_net_run(&NET, 10);
    v = view(R_PORCH);
    TEST_ASSERT_FALSE(v.pending || v.failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, v.action);

    /* Given up: failed, and whether it happened isn't known */
    const uint32_t t2 = act("switch.porch", ACT_OFF, 0, NULL, PLINK_DEV_OK);
    plink_keypad_dev_act_release(&K, t2);
    plink_net_run(&NET, PLINK_DEV_ACT_GIVE_UP_MS + 100u);
    v = view(R_PORCH);
    TEST_ASSERT_TRUE(v.failed && v.unsure && v.online);
    TEST_ASSERT_FALSE(v.pending);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, v.action);

    /* Stale: every row offline since the last device data, the attic since its own time */
    M.dev_auto = false;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, PLINK_DEV_STALE_MS + 1000u, stale, NULL));
    v = view(R_HALL);
    TEST_ASSERT_TRUE(v.have_rec && !v.online && v.since_last_data);
    v = view(R_PORCH);
    TEST_ASSERT_TRUE(!v.online && v.since_last_data && v.failed && v.unsure);
    v = view(R_ATTIC);
    TEST_ASSERT_FALSE(v.online || v.since_last_data);
}

/* LINK-049: device actions have 2 slots of their own: with both being sent a third is BUSY while
 * a command still goes, and with every command slot taken an action still goes; their request
 * ids never meet. */
static void test_dev_act_slots_apart_from_commands(void)
{
    home_rig();
    kp_home_online();
    M.dev_answer = 0u;
    M.serve_cmds = false;
    uint32_t ids[8];
    ids[0] = act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_OK);
    ids[1] = act("switch.porch", ACT_ON, 0, NULL, PLINK_DEV_OK);
    act("cover.garage", ACT_CLOSE, 0, NULL, PLINK_DEV_BUSY);
    const unsigned frames = M.cmd_frames;
    const plink_cmd_t c = arm_cmd("1234");
    ids[2] = plink_keypad_command(&K, NET.now_ms, &c);
    TEST_ASSERT_NOT_EQUAL(0u, ids[2]);
    plink_net_run(&NET, 10);
    TEST_ASSERT_TRUE(M.cmd_frames > frames);

    plink_net_run(&NET, PLINK_DEV_ACT_GIVE_UP_MS + 100u);
    for (size_t i = 3; i < 6u; i++) {
        ids[i] = plink_keypad_command(&K, NET.now_ms, &c);
        TEST_ASSERT_NOT_EQUAL(0u, ids[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_command(&K, NET.now_ms, &c)); /* all taken */
    const size_t sent = M.n_act;
    ids[6] = act("cover.garage", ACT_CLOSE, 0, NULL, PLINK_DEV_OK);
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_size_t(sent + 1u, M.n_act);
    for (size_t i = 0; i < 7u; i++) {
        for (size_t j = i + 1u; j < 7u; j++) {
            TEST_ASSERT_NOT_EQUAL(ids[i], ids[j]);
        }
    }
}

/* LINK-049: list and state requests wait while a panic is being sent and go once it is answered;
 * device actions aren't held. */
static void test_dev_requests_wait_for_panic(void)
{
    home_rig();
    kp_home_online();
    M.dev_auto = false;
    M.serve_cmds = false;
    const plink_cmd_t p = panic_cmd();
    TEST_ASSERT_NOT_EQUAL(0u, plink_keypad_command(&K, NET.now_ms, &p));
    TEST_ASSERT_TRUE(plink_keypad_panic_pending(&K));
    const size_t s0 = M.n_sreq;
    fm_state(R_PORCH, "on");
    fm_dev_state(0u);
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_size_t(s0, M.n_sreq);
    M.dev_answer = 1u + PLINK_DEV_OK;
    const uint32_t t = act("light.hall", ACT_OFF, 0, NULL, PLINK_DEV_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, act_state(t, NULL));
    M.serve_cmds = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 1500, no_panic, NULL));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 1000, home_synced, NULL));
    TEST_ASSERT_TRUE(M.n_sreq > s0);

    M.serve_cmds = false;
    TEST_ASSERT_NOT_EQUAL(0u, plink_keypad_command(&K, NET.now_ms, &p));
    const size_t l0 = M.n_list;
    snprintf(M.rows[R_HALL].info.name, sizeof M.rows[R_HALL].info.name, "Hall ceiling");
    fm_dev_build();
    fm_dev_state(0u);
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_size_t(l0, M.n_list);
    M.serve_cmds = true;
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, list_installed, NULL));
    TEST_ASSERT_TRUE(M.n_list > l0);
}

/* LINK-050: with PREFS, the keypad's settings report goes at each session's first STATUS, after a
 * change at most once a second (the newest values then), and on PREFS_REQ, never otherwise; it
 * carries the panel id, the voice, the fields accepted and their values, req 0. A body that isn't
 * one is refused. */
static void test_prefs_report_timing(void)
{
    M.caps = KCAPS = PLINK_CAP_PREFS;
    uint8_t b1[PROTO_PREFS_ENC_MAX], b2[PROTO_PREFS_ENC_MAX], b3[PROTO_PREFS_ENC_MAX],
        b4[PROTO_PREFS_ENC_MAX];
    const size_t n1 = prefs_body(80u, 30u, b1);
    kp_init(true);
    TEST_ASSERT_TRUE(report(b1, n1));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_prefs);
    const fm_prefs_t *p = &M.prefs_seen[0];
    TEST_ASSERT_TRUE(p->at >= M.promoted_at && p->at <= M.promoted_at + 10u);
    TEST_ASSERT_EQUAL_UINT32(0u, p->req);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", p->panel_id);
    TEST_ASSERT_EQUAL_UINT8(PLINK_VOICE_READY, p->voice);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_KNOWN, p->accepts);
    TEST_ASSERT_EQUAL_UINT8(0u, p->refused_count);
    TEST_ASSERT_EQUAL_UINT8(n1, p->fields_len);
    TEST_ASSERT_EQUAL_MEMORY(b1, p->fields, n1);

    TEST_ASSERT_TRUE(report(b1, n1)); /* the same again: nothing */
    plink_net_run(&NET, 25000);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_prefs);

    const size_t n2 = prefs_body(60u, 30u, b2);
    const uint64_t t1 = NET.now_ms;
    TEST_ASSERT_TRUE(report(b2, n2));
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_size_t(2u, M.n_prefs);
    TEST_ASSERT_EQUAL_UINT64(t1 + 2u, M.prefs_seen[1].at);
    TEST_ASSERT_EQUAL_MEMORY(b2, M.prefs_seen[1].fields, n2);
    plink_net_run(&NET, 190);
    const size_t n3 = prefs_body(50u, 30u, b3);
    TEST_ASSERT_TRUE(report(b3, n3));
    plink_net_run(&NET, 300);
    const size_t n4 = prefs_body(40u, 20u, b4);
    TEST_ASSERT_TRUE(report(b4, n4));
    plink_net_run(&NET, 2000);
    TEST_ASSERT_EQUAL_size_t(3u, M.n_prefs);
    TEST_ASSERT_EQUAL_UINT64(t1 + PLINK_PREFS_GAP_MS + 2u, M.prefs_seen[2].at);
    TEST_ASSERT_EQUAL_MEMORY(b4, M.prefs_seen[2].fields, n4);

    fm_prefs_req(proto_prefs_crc(b4, n4)); /* whatever CRC it names */
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(4u, M.n_prefs);
    TEST_ASSERT_EQUAL_UINT32(0u, M.prefs_seen[3].req);

    fm_rekey(K.live.sid, NET_ID); /* within the second: it waits for it */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, new_session, NULL));
    plink_net_run(&NET, PLINK_PREFS_GAP_MS + 100u);
    TEST_ASSERT_EQUAL_size_t(5u, M.n_prefs);
    TEST_ASSERT_EQUAL_UINT64(M.prefs_seen[3].at + PLINK_PREFS_GAP_MS, M.prefs_seen[4].at);
    TEST_ASSERT_TRUE(M.prefs_seen[4].at >= M.promoted_at);

    const uint8_t bad[3] = {2u, PROTO_PREFS_F_BRIGHTNESS, 1u}; /* a TLV running past the body */
    TEST_ASSERT_FALSE(report(bad, sizeof bad));
    plink_net_run(&NET, 2000);
    TEST_ASSERT_EQUAL_size_t(5u, M.n_prefs);
}

/* LINK-050: a HEARTBEAT carries the settings' CRC (proto_prefs_crc of the report's fields) only in
 * a session with PREFS and once there is a report, following each change; with a master that
 * doesn't share PREFS it stays v1's. */
static void test_heartbeat_prefs_section(void)
{
    uint8_t b1[PROTO_PREFS_ENC_MAX], b2[PROTO_PREFS_ENC_MAX];
    const size_t n1 = prefs_body(80u, 30u, b1);
    const size_t n2 = prefs_body(55u, 30u, b2);
    KCAPS = PLINK_CAP_PREFS;
    kp_init(true);
    TEST_ASSERT_TRUE(report(b1, n1));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_TRUE(M.hbs >= 2u);
    TEST_ASSERT_FALSE(M.hb.has_prefs);
    uint8_t v1[64];
    size_t v1_len = 0;
    TEST_ASSERT_TRUE(plink_enc_heartbeat(&M.hb, v1, sizeof v1, &v1_len));
    TEST_ASSERT_EQUAL_size_t(v1_len, M.hb_pt_len);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_prefs);

    fm_restart();
    M.caps = PLINK_CAP_PREFS;
    kp_online(); /* no report yet */
    const unsigned h0 = M.hbs;
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_TRUE(M.hbs > h0);
    TEST_ASSERT_FALSE(M.hb.has_prefs);
    TEST_ASSERT_TRUE(report(b1, n1));
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_TRUE(M.hb.has_prefs);
    TEST_ASSERT_EQUAL_HEX32(proto_prefs_crc(b1, n1), M.hb.prefs_crc);
    TEST_ASSERT_TRUE(report(b2, n2));
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_EQUAL_HEX32(proto_prefs_crc(b2, n2), M.hb.prefs_crc);
    TEST_ASSERT_NOT_EQUAL(proto_prefs_crc(b1, n1), proto_prefs_crc(b2, n2));
}

/* LINK-051, LINK-052: a PREFS_SET is given to the platform once; its repeats are ignored until it
 * is answered, then answered again from that answer (kept 30 s), never given again; another one
 * waits while one is being applied. The answer is a PREFS with its req, the refusals and the
 * values now stored. Only the master's frames count - one in the keypad's direction is dropped -
 * and never a req of 0. */
static void test_prefs_set_applied_once(void)
{
    M.caps = KCAPS = PLINK_CAP_PREFS;
    uint8_t b1[PROTO_PREFS_ENC_MAX], set[PROTO_PREFS_ENC_MAX], now_b[PROTO_PREFS_ENC_MAX];
    const size_t n1 = prefs_body(80u, 30u, b1);
    const size_t ns = prefs_body(40u, 0u, set);
    const size_t nn = prefs_body(40u, 30u, now_b);
    kp_init(true);
    TEST_ASSERT_TRUE(report(b1, n1));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_net_run(&NET, 1500);
    const size_t p0 = M.n_prefs;
    plink_prefs_change_t ch;
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));

    fm_prefs_set(77u, PLINK_ORIGIN_HA, set, ns);
    plink_net_run(&NET, 10);
    TEST_ASSERT_TRUE(plink_keypad_take_prefs_set(&K, &ch));
    TEST_ASSERT_EQUAL_UINT32(77u, ch.req);
    TEST_ASSERT_EQUAL_UINT8(PLINK_ORIGIN_HA, ch.origin);
    TEST_ASSERT_EQUAL_UINT8(ns, ch.len);
    TEST_ASSERT_EQUAL_MEMORY(set, ch.fields, ns);
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));
    fm_prefs_set(77u, PLINK_ORIGIN_HA, set, ns);      /* its resend */
    fm_prefs_set(78u, PLINK_ORIGIN_CONSOLE, set, ns); /* another, meanwhile */
    plink_net_run(&NET, 10);
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));
    TEST_ASSERT_EQUAL_size_t(p0, M.n_prefs);

    const uint8_t refused[2] = {PROTO_PREFS_F_VOLUME, PLINK_PREFS_HELD};
    TEST_ASSERT_TRUE(plink_keypad_prefs_answer(&K, NET.now_ms, 77u, refused, 1u, now_b, nn));
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_size_t(p0 + 1u, M.n_prefs);
    const fm_prefs_t *a = &M.prefs_seen[p0];
    TEST_ASSERT_EQUAL_UINT32(77u, a->req);
    TEST_ASSERT_EQUAL_UINT8(1u, a->refused_count);
    TEST_ASSERT_EQUAL_MEMORY(refused, a->refused, 2u);
    TEST_ASSERT_EQUAL_UINT8(nn, a->fields_len);
    TEST_ASSERT_EQUAL_MEMORY(now_b, a->fields, nn);

    plink_net_run(&NET, 1000);
    fm_prefs_set(77u, PLINK_ORIGIN_HA, set, ns); /* the answer lost: resent */
    plink_net_run(&NET, 10);
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));
    TEST_ASSERT_EQUAL_size_t(p0 + 2u, M.n_prefs);
    a = &M.prefs_seen[p0 + 1u];
    TEST_ASSERT_EQUAL_UINT32(77u, a->req);
    TEST_ASSERT_EQUAL_UINT8(1u, a->refused_count);
    TEST_ASSERT_EQUAL_MEMORY(refused, a->refused, 2u);
    TEST_ASSERT_EQUAL_MEMORY(now_b, a->fields, nn);

    fm_prefs_set(78u, PLINK_ORIGIN_CONSOLE, set, ns);
    plink_net_run(&NET, 10);
    TEST_ASSERT_TRUE(plink_keypad_take_prefs_set(&K, &ch));
    TEST_ASSERT_EQUAL_UINT32(78u, ch.req);
    TEST_ASSERT_EQUAL_UINT8(PLINK_ORIGIN_CONSOLE, ch.origin);
    TEST_ASSERT_TRUE(plink_keypad_prefs_answer(&K, NET.now_ms, 78u, NULL, 0u, NULL, 0u));
    plink_net_run(&NET, 10);
    a = &M.prefs_seen[M.n_prefs - 1u];
    TEST_ASSERT_EQUAL_UINT32(78u, a->req);
    TEST_ASSERT_EQUAL_UINT8(0u, a->refused_count);
    TEST_ASSERT_EQUAL_MEMORY(now_b, a->fields, nn); /* the values stored since */

    /* A PREFS_SET whose header says a keypad sent it: dropped (the header's flags at byte 4,
     * its sender at bytes 7-8; docs/features/11 "Frame format"). */
    uint8_t pay[PLINK_PREFS_SET_MAX];
    static uint8_t f[PLINK_FRAME_MAX];
    size_t pn = fm_prefs_set_payload(79u, PLINK_ORIGIN_HA, set, ns, pay, sizeof pay);
    size_t fl = fm_seal(PLINK_T_PREFS_SET, pay, pn, f);
    f[4] = PLINK_FLAG_SEALED;
    f[7] = (uint8_t)DEV;
    f[8] = 0u;
    deliver(f, fl);
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));
    pn = fm_prefs_set_payload(1u, PLINK_ORIGIN_HA, set, ns, pay, sizeof pay);
    memset(pay, 0, 4u); /* req 0 */
    fl = fm_seal(PLINK_T_PREFS_SET, pay, pn, f);
    deliver(f, fl);
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));
}

/* LINK-043: a keypad drops what comes of a capability its session doesn't share: a master that
 * doesn't share HOME gets no request and its device data counts for nothing; one that doesn't
 * share PREFS gets no PREFS and no HEARTBEAT section, and its PREFS_SET is never given. */
static void test_home_frames_need_their_capability(void)
{
    home_rig();
    M.caps = PLINK_CAP_PREFS;
    kp_online();
    fm_dev_state(0u);
    fm_dev_state(1u << R_HALL);
    fm_dev_ack(5u, PLINK_DEV_OK, 0u, "light.hall");
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_list);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_sreq);
    TEST_ASSERT_EQUAL_UINT64(0u, dev_info().heard_ms);
    TEST_ASSERT_FALSE(dev_info().fresh);
    TEST_ASSERT_EQUAL_HEX32(0u, dev_info().list_crc);

    fm_restart();
    M.caps = PLINK_CAP_HOME;
    uint8_t b1[PROTO_PREFS_ENC_MAX];
    const size_t n1 = prefs_body(80u, 30u, b1);
    kp_init(true);
    TEST_ASSERT_TRUE(report(b1, n1));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, home_synced, NULL));
    fm_prefs_req(0u);
    fm_prefs_set(9u, PLINK_ORIGIN_HA, b1, n1);
    plink_net_run(&NET, PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_prefs);
    plink_prefs_change_t ch;
    TEST_ASSERT_FALSE(plink_keypad_take_prefs_set(&K, &ch));
    TEST_ASSERT_TRUE(M.hbs > 0u);
    TEST_ASSERT_FALSE(M.hb.has_prefs);
}

/* LINK-043, docs/features/11 "Versions": a keypad that mirrored its master's list drops it at the
 * first frame of a session without HOME (a master that serves no devices now, a v1 one): no rows,
 * so Smart Home hides rather than every tile reading offline until the keypad restarts; nothing is
 * asked and a tap is refused at once. The session before it kept the list, and once a session
 * shares HOME again the list is pulled and installed again. */
static bool session_after(void *ctx)
{
    const unsigned *promoted = ctx;
    return M.promotions > *promoted && K.items_sid == K.live.sid && !K.tentative;
}

static void test_dev_list_dropped_without_home(void)
{
    home_rig();
    kp_home_online();
    plink_dev_info_t d = dev_info();
    TEST_ASSERT_EQUAL_UINT8(R_COUNT, d.count);
    const uint32_t list_seq = d.list_seq;

    /* A new session with HOME keeps it. */
    unsigned promoted = M.promotions;
    fm_rekey(K.live.sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, session_after, &promoted));
    d = dev_info();
    TEST_ASSERT_TRUE(d.shared);
    TEST_ASSERT_EQUAL_HEX32(M.list_crc, d.list_crc);
    TEST_ASSERT_EQUAL_UINT8(R_COUNT, d.count);
    TEST_ASSERT_EQUAL_UINT32(list_seq, d.list_seq);

    /* One without it drops it. */
    plink_net_run(&NET, PLINK_REKEY_GAP_MS);
    M.caps = PLINK_CAP_PREFS;
    promoted = M.promotions;
    fm_rekey(K.live.sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 2000, session_after, &promoted));
    plink_net_run(&NET, 3000);
    TEST_ASSERT_TRUE(online(NULL));
    d = dev_info();
    TEST_ASSERT_TRUE(d.attached);
    TEST_ASSERT_FALSE(d.shared);
    TEST_ASSERT_FALSE(d.fresh);
    TEST_ASSERT_EQUAL_HEX32(0u, d.list_crc);
    TEST_ASSERT_EQUAL_UINT8(0u, d.count);
    TEST_ASSERT_EQUAL_UINT64(0u, d.heard_ms);
    TEST_ASSERT_NOT_EQUAL(list_seq, d.list_seq);
    plink_dev_view_t v;
    TEST_ASSERT_FALSE(plink_keypad_dev_view(&K, NET.now_ms, 0u, &v));
    TEST_ASSERT_FALSE(plink_keypad_dev_row(&K, 0u, NULL, NULL));
    const size_t lists = M.n_list, sreqs = M.n_sreq;
    act("light.hall", ACT_ON, -1, NULL, PLINK_DEV_UNREACHABLE);
    plink_net_run(&NET, 5000);
    TEST_ASSERT_EQUAL_size_t(lists, M.n_list);
    TEST_ASSERT_EQUAL_size_t(sreqs, M.n_sreq);
    TEST_ASSERT_EQUAL_UINT8(0u, dev_info().count);

    /* HOME again: pulled and installed again. */
    M.caps = HOME_CAPS;
    TEST_ASSERT_EQUAL_HEX32(0u, KMIR.list_crc);
    fm_rekey(K.live.sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, home_synced, NULL));
    d = dev_info();
    TEST_ASSERT_TRUE(d.shared);
    TEST_ASSERT_EQUAL_UINT8(R_COUNT, d.count);
    TEST_ASSERT_TRUE(M.n_list > lists);
}

/* ---- updates through the link (LINK-029..037, LINK-039) ----------------------------------- */

/* The keypad's facts as its service gives them. */
static plink_fw_info_t kinfo(const char *version)
{
    plink_fw_info_t i;
    memset(&i, 0, sizeof i);
    i.can = PLINK_FWI_FETCH | PLINK_FWI_RELAY | PLINK_FWI_KEY_ACCEPT;
    snprintf(i.device, sizeof i.device, "crowpanel_p4");
    snprintf(i.board_rev, sizeof i.board_rev, "v1.1");
    snprintf(i.chip_rev, sizeof i.chip_rev, "v1");
    i.chip_rev_full = 100u;
    snprintf(i.project, sizeof i.project, "roboalarms");
    snprintf(i.version, sizeof i.version, "%s", version);
    i.slot = 4718592u;
    memset(i.own_key8, 0x11, sizeof i.own_key8);
    snprintf(i.cp_version, sizeof i.cp_version, "2.12.3");
    return i;
}

/* Both offer FW; the keypad runs 1.2.3, its service takes every order and opens its writer at
 * once; online, its facts sent. */
static void kp_fw_online(void)
{
    M.caps = KCAPS = PLINK_CAP_FW;
    SV.on = SV.go = true;
    M.serve_fw = true;
    kp_init(true);
    const plink_fw_info_t i = kinfo("1.2.3");
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    TEST_ASSERT_TRUE_MESSAGE(plink_net_run_until(&NET, 5000, online, NULL), "not online");
    plink_net_run(&NET, 50);
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_FW, info().caps);
}

/* The newest FW_STATE the master got. */
static const plink_fw_state_t *last_st(void)
{
    TEST_ASSERT_TRUE(M.n_fw_st > 0u);
    return &M.fw_st[M.n_fw_st - 1u];
}

/* What FW_STATE says now. */
static plink_fw_state_t kst(void)
{
    plink_fw_state_t s;
    plink_keypad_fw_status(&K, NET.now_ms, &s);
    return s;
}

static bool verifying(void *ctx)
{
    (void)ctx;
    return kst().state == PLINK_FWS_VERIFYING;
}

/* The newest order the service took. */
static const plink_fw_order_t *sv_last(void)
{
    TEST_ASSERT_TRUE(SV.n_taken > 0u);
    return &SV.taken[SV.n_taken - 1u];
}

/* An order sent; its answer is the next FW_STATE, echoing it with why (a relay under way may
 * have sent more since). */
static uint32_t expect_answer(plink_fw_order_t *o, uint8_t why)
{
    size_t n = M.n_fw_st;
    const uint32_t id = fm_order(o);
    plink_net_run(&NET, 20);
    while (n < M.n_fw_st && M.fw_st[n].order != id) {
        n++; /* one already on its way */
    }
    TEST_ASSERT_TRUE_MESSAGE(n < M.n_fw_st, "not answered");
    TEST_ASSERT_EQUAL_UINT8(o->op, M.fw_st[n].op);
    TEST_ASSERT_EQUAL_UINT8(why, M.fw_st[n].why);
    return id;
}

/* Paused now, for why: the flag and the reason, in what the keypad says and what it sent. */
static void expect_paused(uint8_t why)
{
    const plink_fw_state_t s = kst();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, s.state);
    TEST_ASSERT_EQUAL_HEX8(PLINK_FWF_PAUSED, s.flags);
    TEST_ASSERT_EQUAL_UINT8(why, s.why);
    TEST_ASSERT_EQUAL_HEX8(PLINK_FWF_PAUSED, last_st()->flags);
    TEST_ASSERT_EQUAL_UINT8(why, last_st()->why);
}

static void expect_running(void)
{
    const plink_fw_state_t s = kst();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, s.state);
    TEST_ASSERT_EQUAL_HEX8(0u, s.flags);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, s.why);
    TEST_ASSERT_EQUAL_HEX8(0u, last_st()->flags);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, last_st()->why);
}

/* While paused, nothing is asked for or written for ms; afterwards it runs on. */
static void expect_held(uint64_t ms)
{
    const size_t reqs = M.n_fw_req;
    const uint32_t got = SV.got;
    plink_net_run(&NET, ms);
    TEST_ASSERT_EQUAL_size_t(reqs, M.n_fw_req);
    TEST_ASSERT_EQUAL_UINT32(got, SV.got);
}

static void expect_moves(void)
{
    const uint32_t got = SV.got;
    plink_net_run(&NET, 1500);
    TEST_ASSERT_TRUE(SV.got > got);
}

/* The relay went through whole: the writer got the image, every byte once and in order. */
static void expect_image(void)
{
    TEST_ASSERT_FALSE(SV.out_of_order);
    TEST_ASSERT_EQUAL_UINT32(IMG_N, SV.got);
    TEST_ASSERT_EQUAL_MEMORY(IMG, SV.img, IMG_N);
    TEST_ASSERT_TRUE(SV.max_write <= PLINK_FW_WRITE_MAX);
}

/* LINK-037, LINK-043, LINK-029: a keypad that offers FW to a master that doesn't, or on frames
 * under 250 B, sends no FW_INFO nor FW_STATE and takes no order. */
static void test_fw_nothing_without_the_bit(void)
{
    KCAPS = PLINK_CAP_FW; /* the master offers nothing */
    SV.on = SV.go = true;
    M.serve_fw = true;
    kp_online();
    plink_fw_info_t i = kinfo("1.2.3");
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    TEST_ASSERT_EQUAL_HEX16(0u, info().caps);
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    fm_order(&o);
    o = order_of(PLINK_FW_OP_INFO, "");
    fm_order(&o);
    plink_net_run(&NET, 12000);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_info);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_st);
    TEST_ASSERT_EQUAL_size_t(0u, SV.n_taken);

    /* Both offer it, on frames of 249 B */
    M.caps = PLINK_CAP_FW;
    KMTU = PLINK_FRAME_ESPNOW - 1u;
    kp_init(true);
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 20000, online, NULL));
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_FW, info().caps);
    o = relay_of(IMG_SHA, 0u);
    fm_order(&o);
    o = order_of(PLINK_FW_OP_INFO, "");
    fm_order(&o);
    plink_net_run(&NET, 12000);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_info);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_st);
    TEST_ASSERT_EQUAL_size_t(0u, SV.n_taken);
    TEST_ASSERT_EQUAL_size_t(0u, plink_net_count(&NET, KN, PLINK_T_FW_REQ));

    /* 250 B: the facts go, as they were wanted since the session's first STATUS */
    KMTU = PLINK_FRAME_ESPNOW;
    plink_net_run(&NET, 12000);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_fw_info);
}

/* LINK-029: FW_INFO at each session's first STATUS (facts given before it wait for it), after a
 * change at most once every 2 s (the newest), and on INFO - each order answered with FW_STATE at
 * once; never on its own otherwise. */
static void test_fw_info_timing(void)
{
    M.caps = KCAPS = PLINK_CAP_FW;
    kp_init(true);
    const plink_fw_info_t i = kinfo("1.2.3");
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_fw_info);
    TEST_ASSERT_TRUE(M.fw_info_at[0] >= M.promoted_at && M.fw_info_at[0] <= M.promoted_at + 10u);
    TEST_ASSERT_EQUAL_STRING("crowpanel_p4", M.fw_info.device);
    TEST_ASSERT_EQUAL_STRING("v1.1", M.fw_info.board_rev);
    TEST_ASSERT_EQUAL_STRING("1.2.3", M.fw_info.version);
    TEST_ASSERT_EQUAL_STRING("2.12.3", M.fw_info.cp_version);
    TEST_ASSERT_EQUAL_UINT32(4718592u, M.fw_info.slot);
    TEST_ASSERT_EQUAL_HEX8(PLINK_FWI_FETCH | PLINK_FWI_RELAY | PLINK_FWI_KEY_ACCEPT,
                           M.fw_info.can);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_st); /* no order, no run */

    /* The same facts: nothing; a change at once, the next 2 s after it, with the newest */
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    plink_net_run(&NET, 12000);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_fw_info);
    plink_fw_info_t j = i;
    snprintf(j.cp_version, sizeof j.cp_version, "2.13.0");
    plink_keypad_fw_info(&K, NET.now_ms, &j);
    plink_net_run(&NET, 10);
    TEST_ASSERT_EQUAL_size_t(2u, M.n_fw_info);
    TEST_ASSERT_EQUAL_STRING("2.13.0", M.fw_info.cp_version);
    j.chip_rev_full = 101u;
    plink_keypad_fw_info(&K, NET.now_ms, &j);
    j.chip_rev_full = 102u;
    plink_keypad_fw_info(&K, NET.now_ms, &j);
    plink_net_run(&NET, 1500);
    TEST_ASSERT_EQUAL_size_t(2u, M.n_fw_info);
    plink_net_run(&NET, 600);
    TEST_ASSERT_EQUAL_size_t(3u, M.n_fw_info);
    TEST_ASSERT_EQUAL_UINT64(M.fw_info_at[1] + PLINK_FW_INFO_GAP_MS, M.fw_info_at[2]);
    TEST_ASSERT_EQUAL_UINT16(102u, M.fw_info.chip_rev_full);

    /* INFO: FW_STATE at once, and FW_INFO */
    plink_net_run(&NET, 3000);
    plink_fw_order_t o = order_of(PLINK_FW_OP_INFO, "");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_IDLE, last_st()->state);
    TEST_ASSERT_EQUAL_size_t(4u, M.n_fw_info);

    /* A new session (the master restarted): FW_INFO at its first STATUS */
    fm_restart();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 20000, new_session, NULL));
    plink_net_run(&NET, 100);
    TEST_ASSERT_EQUAL_size_t(5u, M.n_fw_info);
    TEST_ASSERT_TRUE(M.fw_info_at[4] >= M.promoted_at);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_fw_st);
}

/* LINK-029, LINK-033: FW_INFO's can says what the keypad can do now: no relay without a writer
 * (and a RELAY refused), and a configuration it can't read while the master's newest status names
 * one - a keypad in the needs-update state, which never gets ONLINE, still sends it. */
static void test_fw_info_can(void)
{
    M.caps = KCAPS = PLINK_CAP_FW;
    KNOWRITER = true;
    M.bad_blob = true;
    fm_blob();
    kp_init(true);
    const plink_fw_info_t i = kinfo("1.2.3");
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    plink_net_run(&NET, 15000);
    TEST_ASSERT_NOT_EQUAL(0u, info().cfg_bad_crc);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_CONNECTING, info().state);
    TEST_ASSERT_TRUE(M.n_fw_info >= 2u);
    TEST_ASSERT_EQUAL_HEX8(PLINK_FWI_FETCH | PLINK_FWI_KEY_ACCEPT | PLINK_FWI_CFG_UNREADABLE,
                           M.fw_info.can);
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_BAD_ORDER);

    /* The master's configuration readable again: the bit goes */
    M.bad_blob = false;
    fm_blob();
    fm_status();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_net_run(&NET, 11000);
    TEST_ASSERT_EQUAL_HEX8(PLINK_FWI_FETCH | PLINK_FWI_KEY_ACCEPT, M.fw_info.can);
}

/* LINK-029: every order is answered with FW_STATE at once, echoing it; one seen before is answered
 * again and never runs twice; PROBE goes to the service and its answer to the probe section, its
 * age counted. LINK-026: a new order from an older frame of the session than the newest taken is
 * dropped, so an INSTALL never overtakes the CANCEL sent after it; dropped, it isn't remembered. */
static void test_fw_orders_answered_once(void)
{
    kp_fw_online();
    plink_fw_order_t o = order_of(PLINK_FW_OP_PROBE, "1.3.0");
    const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_PROBING, last_st()->state);
    TEST_ASSERT_EQUAL_UINT8(0u, last_st()->asked);
    TEST_ASSERT_EQUAL_size_t(1u, SV.n_taken);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sv_last()->op);
    TEST_ASSERT_EQUAL_HEX32(id, sv_last()->order);
    TEST_ASSERT_EQUAL_STRING("1.3.0", sv_last()->version);
    TEST_ASSERT_EQUAL_STRING("https://updates.example.org", sv_last()->server);
    TEST_ASSERT_EQUAL_STRING("stable", sv_last()->channel);

    fm_order_again(&o);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(2u, M.n_fw_st);
    TEST_ASSERT_EQUAL_HEX32(id, last_st()->order);
    TEST_ASSERT_EQUAL_size_t(1u, SV.n_taken);

    const uint8_t key8[PLINK_KEY8_LEN] = {0xC0, 0xFF, 0xEE, 0x01, 0x02, 0x03, 0x04, 0x05};
    plink_keypad_fw_probe(&K, NET.now_ms, PLINK_FW_WHY_OTHER_VERSION, "1.4.0", key8, true);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(3u, M.n_fw_st);
    const plink_fw_state_t *s = last_st();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_IDLE, s->state);
    TEST_ASSERT_EQUAL_HEX32(id, s->order);
    TEST_ASSERT_EQUAL_UINT8(1u, s->asked);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OTHER_VERSION, s->probe_why);
    TEST_ASSERT_EQUAL_STRING("1.4.0", s->offered);
    TEST_ASSERT_EQUAL_MEMORY(key8, s->offered_key8, sizeof key8);
    TEST_ASSERT_EQUAL_UINT8(1u, s->key_trusted);
    TEST_ASSERT_EQUAL_UINT32(0u, s->age_s);
    /* Idle: no FW_STATE of its own; the answer's age counts on */
    plink_net_run(&NET, 25000);
    TEST_ASSERT_EQUAL_size_t(3u, M.n_fw_st);
    o = order_of(PLINK_FW_OP_INFO, "");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(last_st()->age_s >= 25u && last_st()->age_s <= 26u);

    /* Sealed in this order, arriving in the other */
    plink_fw_order_t a = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    a.order = M.next_order++;
    plink_fw_order_t c = order_of(PLINK_FW_OP_CANCEL, "");
    c.order = M.next_order++;
    uint8_t pa[PLINK_FW_ORDER_MAX], pc[PLINK_FW_ORDER_MAX], fa[PLINK_FRAME_MAX],
        fc[PLINK_FRAME_MAX];
    size_t la = 0, lc = 0;
    TEST_ASSERT_TRUE(plink_enc_fw_order(&a, pa, sizeof pa, &la));
    TEST_ASSERT_TRUE(plink_enc_fw_order(&c, pc, sizeof pc, &lc));
    const size_t na = fm_seal(PLINK_T_FW_ORDER, pa, la, fa);
    const size_t nc = fm_seal(PLINK_T_FW_ORDER, pc, lc, fc);
    const size_t taken = SV.n_taken, sts = M.n_fw_st;
    deliver(fc, nc);
    deliver(fa, na);
    sv_poll(NET.now_ms);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(taken + 1u, SV.n_taken);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sv_last()->op);
    TEST_ASSERT_EQUAL_size_t(sts + 1u, M.n_fw_st);
    TEST_ASSERT_EQUAL_HEX32(c.order, last_st()->order);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    fm_order_again(&a);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_HEX32(a.order, last_st()->order);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sv_last()->op);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
}

/* LINK-029: orders wait for the service in a queue of 4; one that finds it full isn't taken nor
 * answered (the master's resend brings it back), and nothing is lost or run twice. */
static void test_fw_queue_full(void)
{
    kp_fw_online();
    SV.on = false;
    plink_fw_order_t o[5];
    uint32_t id[5];
    for (size_t i = 0; i < 5u; i++) {
        o[i] = order_of(i % 2u == 0u ? PLINK_FW_OP_PROBE : PLINK_FW_OP_INFO, "1.3.0");
        id[i] = fm_order(&o[i]);
    }
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(4u, M.n_fw_st);
    TEST_ASSERT_EQUAL_HEX32(id[3], last_st()->order);
    plink_fw_order_t got;
    TEST_ASSERT_TRUE(plink_keypad_take_fw_order(&K, &got));
    TEST_ASSERT_EQUAL_HEX32(id[0], got.order);
    fm_order_again(&o[4]);
    fm_order_again(&o[0]);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(6u, M.n_fw_st);
    TEST_ASSERT_EQUAL_HEX32(id[0], last_st()->order); /* the repeat's answer */
    for (size_t i = 1; i < 5u; i++) {
        TEST_ASSERT_TRUE(plink_keypad_take_fw_order(&K, &got));
        TEST_ASSERT_EQUAL_HEX32(id[i], got.order);
    }
    TEST_ASSERT_FALSE(plink_keypad_take_fw_order(&K, &got));
}

/* LINK-029, LINK-031, LINK-033, LINK-035: refused orders - an unknown op or kind, a RELAY without
 * a size within the slot, a SHA-256, a page or a count, an order without a version, a FETCH or
 * RELAY not newer than the version running - answered with the refusal, nothing handed on or
 * asked for; asked again, the same refusal. */
static void test_fw_order_refusals(void)
{
    kp_fw_online();
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    o.op = 7u;
    expect_answer(&o, PLINK_FW_WHY_UNKNOWN_OP);
    o.op = 0u;
    expect_answer(&o, PLINK_FW_WHY_UNKNOWN_OP);
    o = relay_of(IMG_SHA, 0u);
    o.kind = PLINK_FW_KIND_C6;
    expect_answer(&o, PLINK_FW_WHY_UNKNOWN_KIND);
    o = order_of(PLINK_FW_OP_INFO, "");
    o.kind = 9u;
    expect_answer(&o, PLINK_FW_WHY_UNKNOWN_KIND);
    o = order_of(PLINK_FW_OP_PROBE, "");
    expect_answer(&o, PLINK_FW_WHY_BAD_ORDER);
    o = relay_of(IMG_SHA, 0u);
    o.size = 4718592u + 1u;
    expect_answer(&o, PLINK_FW_WHY_TOO_BIG);
    o.size = 0u;
    expect_answer(&o, PLINK_FW_WHY_BAD_ORDER);
    o = relay_of(IMG_SHA, 0u);
    memset(o.sha256, 0, sizeof o.sha256);
    expect_answer(&o, PLINK_FW_WHY_BAD_ORDER);
    o = relay_of(IMG_SHA, 0u);
    o.page = 0u;
    expect_answer(&o, PLINK_FW_WHY_BAD_ORDER);
    o = relay_of(IMG_SHA, 0u);
    o.count = 0u;
    expect_answer(&o, PLINK_FW_WHY_BAD_ORDER);
    o = relay_of(IMG_SHA, 0u);
    snprintf(o.version, sizeof o.version, "1.2.3");
    expect_answer(&o, PLINK_FW_WHY_NOT_NEWER);
    o = order_of(PLINK_FW_OP_FETCH, "1.1.9");
    expect_answer(&o, PLINK_FW_WHY_NOT_NEWER);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_IDLE, last_st()->state);
    fm_order_again(&o);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NOT_NEWER, last_st()->why);
    TEST_ASSERT_EQUAL_size_t(0u, SV.n_taken);
    plink_net_run(&NET, 3000);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_req);
    /* An order taken: its answer says OK */
    o = order_of(PLINK_FW_OP_INFO, "");
    expect_answer(&o, PLINK_FW_WHY_OK);
}

/* LINK-032, LINK-033: the relay. Nothing is asked for until the service's writer is open; then
 * FW_REQs by id8 from the first byte missing, pages and counts within the order's, never past
 * 16 KB ahead of the writer, and the writer gets the image in order, at most 4 KB at a time, every
 * byte once; its last byte ends the transfer (VERIFYING), the service checks it (READY), and
 * nothing more is asked for. */
static void test_fw_relay_in_order(void)
{
    kp_fw_online();
    SV.go = false;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_size_t(1u, SV.n_taken);
    const plink_fw_order_t *t = sv_last();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, t->op);
    TEST_ASSERT_EQUAL_HEX32(id, t->order);
    TEST_ASSERT_EQUAL_UINT32(IMG_N, t->size);
    TEST_ASSERT_EQUAL_MEMORY(IMG_SHA, t->sha256, sizeof IMG_SHA);
    TEST_ASSERT_EQUAL_HEX8(0x6Bu, t->key[0]);
    const plink_fw_state_t *s = last_st();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, s->state);
    TEST_ASSERT_EQUAL_UINT32(0u, s->bytes);
    TEST_ASSERT_EQUAL_UINT32(IMG_N, s->size);
    TEST_ASSERT_EQUAL_MEMORY(IMG_SHA, s->id8, PLINK_ID8_LEN);
    TEST_ASSERT_EQUAL_STRING("1.3.0", s->target);
    plink_net_run(&NET, 2000);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_req);

    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 30000, verifying, NULL));
    expect_image();
    TEST_ASSERT_TRUE(M.n_fw_req >= 5u);
    TEST_ASSERT_EQUAL_UINT32(0u, M.fw_req[0].offset);
    TEST_ASSERT_EQUAL_UINT16(IMG_PAGE, M.fw_req[0].page);
    TEST_ASSERT_EQUAL_UINT8(IMG_COUNT, M.fw_req[0].count);
    for (size_t i = 0; i < M.n_fw_req; i++) {
        const plink_fw_req_t *q = &M.fw_req[i];
        const uint32_t end = q->offset + (uint32_t)q->page * q->count;
        TEST_ASSERT_EQUAL_MEMORY(IMG_SHA, q->id8, PLINK_ID8_LEN);
        TEST_ASSERT_TRUE(q->page >= 1u && q->page <= IMG_PAGE);
        TEST_ASSERT_TRUE(q->count >= 1u && q->count <= IMG_COUNT);
        TEST_ASSERT_TRUE(end <= IMG_N);
        TEST_ASSERT_TRUE(end <= M.fw_req_wrote[i] + PLINK_FW_WINDOW);
        TEST_ASSERT_TRUE(q->offset >= M.fw_req_wrote[i]);
    }
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_VERIFYING, last_st()->state);
    TEST_ASSERT_EQUAL_UINT32(IMG_N, last_st()->bytes);
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, last_st()->state);
    expect_held(5000);
}

/* LINK-032: through loss, duplicates, damage and reordering the writer still gets the image in
 * order, every byte once, the missing bytes asked for again. */
static void test_fw_relay_through_loss(void)
{
    kp_fw_online();
    SV.go = false;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
    TPK.only_dir = PLINK_DIR_KEYPAD; /* a damaged frame may be opened twice: harmless */
    M.tp.only_dir = PLINK_DIR_MASTER;
    NET.loss_pm = 150u;
    NET.dup_pm = 100u;
    NET.corrupt_pm = 20u;
    NET.jitter_ms = 30u;
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 300000, verifying, NULL));
    expect_image();
    TEST_ASSERT_TRUE(M.n_fw_req > IMG_N / (IMG_PAGE * IMG_COUNT) + 2u);
}

/* LINK-032, LINK-035: an unanswered FW_REQ is sent again after 300 ms, doubling to 2 s; one
 * answered in part, 300 ms after its last page, for the bytes still missing: from the first byte
 * missing up to the first run of pages already in past it. 60 s without a new byte (not paused):
 * FAILED, STALLED - the service is told, nothing more is asked for. */
static void test_fw_resends_and_stall(void)
{
    kp_fw_online();
    SV.take_max = 1024u; /* a slow writer: the window fills */
    SV.busy_ms = 100u;
    M.serve_fw = false;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 4500);
    uint64_t at[16];
    const size_t n = sent_times(KN, PLINK_T_FW_REQ, at, 16);
    TEST_ASSERT_TRUE(n >= 5u);
    TEST_ASSERT_EQUAL_UINT64(300u, at[1] - at[0]);
    TEST_ASSERT_EQUAL_UINT64(600u, at[2] - at[1]);
    TEST_ASSERT_EQUAL_UINT64(1200u, at[3] - at[2]);
    TEST_ASSERT_EQUAL_UINT64(2000u, at[4] - at[3]);
    for (size_t i = 0; i < M.n_fw_req; i++) {
        TEST_ASSERT_EQUAL_UINT32(0u, M.fw_req[i].offset);
        TEST_ASSERT_EQUAL_UINT8(IMG_COUNT, M.fw_req[i].count);
    }

    /* The third page of the next answer is lost: the resend asks for it alone, 300 ms after the
     * last page came */
    M.serve_fw = true;
    M.fw_lose_one = 3u;
    const size_t r0 = M.n_fw_req;
    plink_net_run(&NET, 1700);
    TEST_ASSERT_EQUAL_size_t(r0 + 1u, M.n_fw_req);
    TEST_ASSERT_EQUAL_UINT32(0u, M.fw_req[r0].offset);
    TEST_ASSERT_EQUAL_UINT32(2u * IMG_PAGE, SV.got);
    TEST_ASSERT_EQUAL_UINT8(1u, K.upd.isl_n);
    M.fw_lose_one = 0u;
    plink_net_run(&NET, 320);
    TEST_ASSERT_TRUE(M.n_fw_req >= r0 + 2u);
    TEST_ASSERT_EQUAL_UINT32(2u * IMG_PAGE, M.fw_req[r0 + 1u].offset);
    TEST_ASSERT_EQUAL_UINT16(IMG_PAGE, M.fw_req[r0 + 1u].page);
    TEST_ASSERT_EQUAL_UINT8(1u, M.fw_req[r0 + 1u].count);
    TEST_ASSERT_EQUAL_UINT64(M.fw_req_at[r0] + 2u * NET.delay_ms + PLINK_PULL_RETRY_MS,
                             M.fw_req_at[r0 + 1u]);
    plink_net_run(&NET, 50);
    TEST_ASSERT_TRUE(K.upd.have >= IMG_COUNT * IMG_PAGE);
    TEST_ASSERT_FALSE(SV.out_of_order);

    /* Then nothing comes */
    M.serve_fw = false;
    plink_net_run(&NET, 50);
    const uint64_t last = NET.now_ms;
    plink_net_run(&NET, PLINK_FW_STALL_MS - 1000u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, kst().state);
    plink_net_run(&NET, 1500);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, kst().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_STALLED, last_st()->why);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, last_st()->state);
    TEST_ASSERT_TRUE(M.fw_st_at[M.n_fw_st - 1u] <= last + PLINK_FW_STALL_MS + 5u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sv_last()->op);
    TEST_ASSERT_EQUAL_HEX32(0u, sv_last()->order); /* the core's own */
    expect_held(10000);
}

/* LINK-032: a FW_PAGE is taken only for the relay under way (its id8 and total), inside what was
 * asked for and past what the writer took; a page ahead waits in the window until the gap before
 * it fills; a repeat writes nothing again. */
static void test_fw_page_checks(void)
{
    kp_fw_online();
    M.serve_fw = false;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(1u, M.n_fw_req); /* [0, 16 KB) asked for */
    uint8_t p[PLINK_PAYLOAD_MAX];
    size_t n = fm_fw_page_payload(IMG2_SHA, IMG_N, 0u, IMG_PAGE, p, sizeof p);
    fm_send_sealed(PLINK_T_FW_PAGE, p, n);
    n = fm_fw_page_payload(IMG_SHA, IMG_N + 1u, 0u, IMG_PAGE, p, sizeof p);
    fm_send_sealed(PLINK_T_FW_PAGE, p, n);
    fm_fw_page(IMG_SHA, PLINK_FW_WINDOW, IMG_PAGE);                    /* not asked for */
    fm_fw_page(IMG_SHA, PLINK_FW_WINDOW - IMG_PAGE / 2u, IMG_PAGE);    /* half of it not */
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT32(0u, SV.got);
    TEST_ASSERT_EQUAL_UINT32(0u, K.upd.have);
    TEST_ASSERT_EQUAL_UINT8(0u, K.upd.isl_n);

    fm_fw_page(IMG_SHA, IMG_PAGE, IMG_PAGE); /* ahead of a gap: kept, not written */
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT32(0u, SV.got);
    TEST_ASSERT_EQUAL_UINT8(1u, K.upd.isl_n);
    fm_fw_page(IMG_SHA, 0u, IMG_PAGE);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT32(2u * IMG_PAGE, SV.got);
    TEST_ASSERT_EQUAL_UINT8(0u, K.upd.isl_n);
    const unsigned writes = SV.writes;
    fm_fw_page(IMG_SHA, IMG_PAGE, IMG_PAGE); /* again, and below the writer */
    fm_fw_page(IMG_SHA, 0u, IMG_PAGE);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT32(2u * IMG_PAGE, SV.got);
    TEST_ASSERT_EQUAL_UINT(writes, SV.writes);
    TEST_ASSERT_FALSE(SV.out_of_order);
    TEST_ASSERT_EQUAL_MEMORY(IMG, SV.img, 2u * IMG_PAGE);

    /* After a CANCEL nothing is taken */
    o = order_of(PLINK_FW_OP_CANCEL, "");
    expect_answer(&o, PLINK_FW_WHY_CANCELLED);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_STOPPED, last_st()->state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_CANCELLED, last_st()->why);
    fm_fw_page(IMG_SHA, 2u * IMG_PAGE, IMG_PAGE);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT32(2u * IMG_PAGE, SV.got);
}

/* LINK-032: the flash paces the pull - the next write and the next FW_REQ no sooner than the last
 * write kept it busy, so it is busy at most half the time - and FW_REQs keep the order's gap;
 * FW_STATE for the bytes at most once a second. */
static void test_fw_writer_paces_the_pull(void)
{
    kp_fw_online();
    SV.take_max = 1500u;
    SV.busy_ms = 100u;
    plink_fw_order_t o = relay_of(IMG_SHA, 120u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 60000, verifying, NULL));
    expect_image();
    for (size_t i = 1; i < SV.n_write_at; i++) {
        TEST_ASSERT_TRUE(SV.write_at[i] >= SV.write_at[i - 1u] + SV.busy_ms);
    }
    uint64_t req[256];
    const size_t nr = sent_times(KN, PLINK_T_FW_REQ, req, 256);
    TEST_ASSERT_TRUE(nr >= 5u);
    for (size_t i = 1; i < nr; i++) {
        TEST_ASSERT_TRUE(req[i] >= req[i - 1u] + 120u);
    }
    for (size_t i = 0; i < nr; i++) {
        for (size_t w = 0; w < SV.n_write_at && SV.write_at[w] <= req[i]; w++) {
            TEST_ASSERT_TRUE(SV.write_at[w] + SV.busy_ms <= req[i]);
        }
    }
    size_t moving = 0;
    for (size_t i = 1; i < M.n_fw_st; i++) {
        if (M.fw_st[i].state == PLINK_FWS_RECEIVING &&
            M.fw_st[i - 1u].state == PLINK_FWS_RECEIVING) {
            TEST_ASSERT_TRUE(M.fw_st_at[i] >= M.fw_st_at[i - 1u] + PLINK_FW_STATE_GAP_MS);
            TEST_ASSERT_TRUE(M.fw_st[i].bytes > M.fw_st[i - 1u].bytes);
            moving++;
        }
    }
    TEST_ASSERT_TRUE(moving >= 3u);
}

static void edit_fire(plink_status_t *st);

/* LINK-034 (decided 2026-10-17: downloading goes on while armed): the relay pauses - FW_STATE at
 * once with PAUSED and why, nothing asked for or written - only while the view shows an exit or an
 * entry time or an alarm sounding (NOT_QUIET; fire by the STATUS flag too), a panic is still being
 * sent (PANIC_PENDING), the service's gate says no (a kept tone), or no STATUS came for 15 s
 * (NO_STATUS: exactly then); it goes on when that ends. Never for the master's UPDATING or HOLD,
 * nor while merely armed - though the INSTALL gate says no then. */
static void test_fw_relay_pauses(void)
{
    kp_fw_online();
    SV.take_max = 512u;
    SV.busy_ms = 200u;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 2000);
    TEST_ASSERT_TRUE(SV.got > 0u && SV.got < IMG_N);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, plink_keypad_fw_hold(&K, NET.now_ms));

    /* The master's own update and its HOLD: no pause */
    M.updating = M.hold = true;
    fm_status();
    plink_net_run(&NET, 20);
    expect_running();
    expect_moves();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NOT_QUIET, plink_keypad_fw_hold(&K, NET.now_ms));
    M.updating = M.hold = false;
    fm_status();
    plink_net_run(&NET, 20);

    /* Arming: its exit time pauses it; armed, it goes on (no restart, though) */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    fm_status();
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_NOT_QUIET);
    expect_held(2000);
    sim_advance(&S, 61000u);
    fm_status();
    plink_net_run(&NET, 20);
    expect_running();
    expect_moves();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NOT_QUIET, plink_keypad_fw_hold(&K, NET.now_ms));
    /* An entry time */
    sim_fault(&S, Z_FRONT);
    fm_status();
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_NOT_QUIET);
    expect_held(2000);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    sim_restore(&S, Z_FRONT);
    fm_status();
    plink_net_run(&NET, 20);
    expect_running();
    /* An alarm sounding: fire, by the STATUS flag */
    M.edit = edit_fire;
    fm_status();
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_NOT_QUIET);
    expect_held(2000);
    M.edit = NULL;
    fm_status();
    plink_net_run(&NET, 20);
    expect_running();

    M.serve_cmds = false;
    const plink_cmd_t pc = panic_cmd();
    const uint32_t req = plink_keypad_command(&K, NET.now_ms, &pc);
    TEST_ASSERT_NOT_EQUAL(0u, req);
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_PANIC_PENDING);
    expect_held(2000);
    const plink_result_t res = {req, ALARM_OK, 0u, 0u, 0u};
    fm_result(&res);
    plink_net_run(&NET, 20);
    expect_running();

    plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_KEPT_TONE);
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_KEPT_TONE);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, plink_keypad_fw_hold(&K, NET.now_ms));
    expect_held(2000);
    plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    expect_running();

    NET.lose_type = PLINK_T_STATUS;
    fm_status();
    const uint64_t heard = K.status_ms;
    plink_net_run(&NET, PLINK_FW_FRESH_MS + 1000u);
    expect_paused(PLINK_FW_WHY_NO_STATUS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NO_STATUS, plink_keypad_fw_hold(&K, NET.now_ms));
    size_t i = M.n_fw_st;
    while (i > 0u && M.fw_st[i - 1u].why == PLINK_FW_WHY_NO_STATUS) {
        i--;
    }
    TEST_ASSERT_TRUE(M.fw_st_at[i] >= heard + PLINK_FW_FRESH_MS &&
                     M.fw_st_at[i] <= heard + PLINK_FW_FRESH_MS + 5u);
    expect_held(3000);
    NET.lose_type = 0u;
    fm_status();
    plink_net_run(&NET, 20);
    expect_running();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 120000, verifying, NULL));
    expect_image();
}

/* LINK-035: paused time isn't a stall - a pause longer than 60 s goes on - but 30 min paused
 * fails the relay (TIMEOUT): the service is told, and nothing more is asked for. */
static void test_fw_pause_times_out(void)
{
    kp_fw_online();
    SV.take_max = 512u;
    SV.busy_ms = 200u;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 2000);
    plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_KEPT_TONE);
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_KEPT_TONE);
    plink_net_run(&NET, 2u * PLINK_FW_STALL_MS);
    expect_paused(PLINK_FW_WHY_KEPT_TONE);
    plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    expect_running();
    expect_moves();

    plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_KEPT_TONE);
    plink_net_run(&NET, 20);
    expect_paused(PLINK_FW_WHY_KEPT_TONE);
    plink_net_run(&NET, PLINK_FW_PAUSE_MAX_MS - 1000u);
    expect_paused(PLINK_FW_WHY_KEPT_TONE);
    plink_net_run(&NET, 1500);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, last_st()->state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_TIMEOUT, last_st()->why);
    TEST_ASSERT_EQUAL_HEX8(0u, last_st()->flags);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sv_last()->op);
    TEST_ASSERT_EQUAL_HEX32(0u, sv_last()->order);
    plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_OK);
    expect_held(5000);
}

/* LINK-035: a relay that times out paused while its gate says no - a kept tone, three times
 * running - isn't a failed run, so a fourth RELAY is taken; one that times out without a STATUS
 * (the link gone) is: with two more failed runs, the next is refused BUDGET. */
static void test_fw_timeout_at_the_gate_isnt_counted(void)
{
    kp_fw_online();
    SV.take_max = 512u;
    SV.busy_ms = 200u;
    for (unsigned i = 0; i < 3u; i++) {
        plink_fw_order_t o = relay_of(IMG_SHA, 0u);
        expect_answer(&o, PLINK_FW_WHY_OK);
        plink_net_run(&NET, 1000);
        plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_KEPT_TONE);
        plink_net_run(&NET, 20);
        expect_paused(PLINK_FW_WHY_KEPT_TONE);
        plink_net_run(&NET, PLINK_FW_PAUSE_MAX_MS + 500u);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, last_st()->state);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_TIMEOUT, last_st()->why);
        plink_keypad_fw_gate(&K, NET.now_ms, PLINK_FW_WHY_OK);
        plink_net_run(&NET, 20);
    }
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 1000);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, kst().state);

    /* No STATUS: 15 s on it pauses, 30 min on it fails - a failed run. */
    NET.lose_type = PLINK_T_STATUS;
    plink_net_run(&NET, PLINK_FW_FRESH_MS + PLINK_FW_PAUSE_MAX_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, kst().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_TIMEOUT, kst().why);
    NET.lose_type = 0u;
    fm_status();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 60000, online, NULL));
    SV.go = false;
    for (unsigned i = 0; i < 2u; i++) {
        o = relay_of(IMG_SHA, 0u);
        const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
        plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_FAILED, PLINK_FW_WHY_BAD_SHA);
        plink_net_run(&NET, 20);
    }
    o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_BUDGET);
}

/* LINK-032: the relay goes on across a new session and a master restart, by the image's id8: a
 * RELAY with a new order id for the same image continues the run (answered, not handed to the
 * service again), and no byte is written twice. */
static void test_fw_relay_across_sessions(void)
{
    kp_fw_online();
    SV.take_max = 512u;
    SV.busy_ms = 200u;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 1500);
    const uint32_t got = SV.got;
    TEST_ASSERT_TRUE(got > 0u);

    const uint32_t sid = K.live.sid;
    fm_rekey(sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, new_session, NULL));
    TEST_ASSERT_NOT_EQUAL(sid, K.live.sid);
    expect_moves();

    fm_restart();
    M.next_order = 0x13570001u; /* its new random base */
    plink_net_run(&NET, 12000);
    TEST_ASSERT_TRUE(M.promotions >= 3u);
    TEST_ASSERT_TRUE(SV.got < IMG_N);
    o = relay_of(IMG_SHA, 0u);
    const uint32_t id2 = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_HEX32(id2, last_st()->order);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, last_st()->state);
    TEST_ASSERT_EQUAL_size_t(1u, SV.n_taken);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 120000, verifying, NULL));
    expect_image();
}

/* LINK-032, LINK-035: a RELAY of another image ends the run and starts over from its first byte,
 * the service told by the new order; a FETCH ends a relay (and a READY image of another version),
 * and its pull stops. */
static void test_fw_another_image_starts_over(void)
{
    kp_fw_online();
    SV.take_max = 1024u;
    SV.busy_ms = 100u;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 1500);
    TEST_ASSERT_TRUE(SV.got > 0u && SV.got < IMG_N);

    plink_fw_order_t b = relay_of(IMG2_SHA, 0u);
    const size_t r0 = M.n_fw_req;
    const uint32_t id2 = expect_answer(&b, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_size_t(2u, SV.n_taken);
    TEST_ASSERT_EQUAL_HEX32(id2, SV.run);
    TEST_ASSERT_EQUAL_MEMORY(IMG2_SHA, kst().id8, PLINK_ID8_LEN);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 60000, verifying, NULL));
    expect_image();
    TEST_ASSERT_TRUE(M.n_fw_req > r0);
    TEST_ASSERT_EQUAL_UINT32(0u, M.fw_req[r0].offset);
    for (size_t i = r0; i < M.n_fw_req; i++) {
        TEST_ASSERT_EQUAL_MEMORY(IMG2_SHA, M.fw_req[i].id8, PLINK_ID8_LEN);
    }
    plink_keypad_fw_report(&K, NET.now_ms, id2, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    plink_fw_order_t in = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    expect_answer(&in, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));

    /* A FETCH of 1.4.0: the READY image of 1.3.0 goes, and its INSTALL */
    plink_fw_order_t f = order_of(PLINK_FW_OP_FETCH, "1.4.0");
    const uint32_t idf = expect_answer(&f, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FETCHING, last_st()->state);
    TEST_ASSERT_EQUAL_STRING("1.4.0", last_st()->target);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sv_last()->op);

    /* A RELAY ends the fetch; a FETCH ends that relay mid-way: its pull stops */
    o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 1000);
    TEST_ASSERT_TRUE(SV.got > 0u && SV.got < IMG_N);
    plink_keypad_fw_report(&K, NET.now_ms, idf, PLINK_FWS_FAILED, PLINK_FW_WHY_BROKE_OFF);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, kst().state); /* about the fetch: ignored */
    f = order_of(PLINK_FW_OP_FETCH, "1.4.0");
    expect_answer(&f, PLINK_FW_WHY_OK);
    expect_held(5000);
}

/* LINK-035: after 3 failed runs in 24 h - the service's or the core's own (a stall) - a new FETCH
 * or RELAY is refused BUDGET, a PROBE still taken; 24 h on, taken again. A FETCH whose check never
 * reached the server, or that the server answered, wrote nothing: no failed run (the master tries
 * again in an hour). */
static void test_fw_budget(void)
{
    kp_fw_online();
    SV.go = false;
    for (unsigned i = 0; i < 3u; i++) {
        plink_fw_order_t f = order_of(PLINK_FW_OP_FETCH, "1.3.0");
        const uint32_t idf = expect_answer(&f, PLINK_FW_WHY_OK);
        plink_keypad_fw_report(&K, NET.now_ms, idf, PLINK_FWS_FAILED,
                               i == 2u ? PLINK_FW_WHY_UNKNOWN_DEVICE : PLINK_FW_WHY_NO_CONNECT);
        plink_net_run(&NET, 20);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, last_st()->state);
    }
    for (unsigned i = 0; i < 2u; i++) {
        plink_fw_order_t o = relay_of(IMG_SHA, 0u);
        const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
        plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_FAILED, PLINK_FW_WHY_BAD_SHA);
        plink_net_run(&NET, 20);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, last_st()->state);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_BAD_SHA, last_st()->why);
    }
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, PLINK_FW_STALL_MS + 100u); /* its writer never opens */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_STALLED, last_st()->why);
    const size_t taken = SV.n_taken;
    o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_BUDGET);
    o = order_of(PLINK_FW_OP_FETCH, "1.3.0");
    expect_answer(&o, PLINK_FW_WHY_BUDGET);
    TEST_ASSERT_EQUAL_size_t(taken, SV.n_taken);
    o = order_of(PLINK_FW_OP_PROBE, "1.3.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_size_t(taken + 1u, SV.n_taken);

    NET.now_ms += PLINK_FW_BUDGET_MS; /* a day on (the link comes back after the jump) */
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 60000, online, NULL));
    o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_RECEIVING, last_st()->state);
}

/* LINK-034, LINK-035: the INSTALL hold - an INSTALL records the version it names, for the gate;
 * while READY, FW_STATE every 10 s; once its restart has started a CANCEL, a FETCH or a RELAY is
 * refused BUSY; the restart put off (OTA-028: READY again) keeps the hold, for its next try - but
 * after a CANCEL refused BUSY the run ends as that CANCEL said (STOPPED, CANCELLED): the hold goes
 * and the service is told to discard the image, so the keypad never restarts later on an INSTALL
 * its master gave up; a CANCEL discards a READY image and its hold (STOPPED, CANCELLED), and
 * STOPPED sends nothing of its own; a READY image without an INSTALL is discarded after 24 h,
 * which isn't a failed run. */
static void test_fw_install_hold(void)
{
    kp_fw_online();
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 30000, verifying, NULL));
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, last_st()->state);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    size_t n = M.n_fw_st;
    const uint64_t from = M.fw_st_at[n - 1u];
    plink_net_run(&NET, 25000);
    TEST_ASSERT_EQUAL_size_t(n + 2u, M.n_fw_st);
    TEST_ASSERT_EQUAL_UINT64(from + PLINK_FW_STATE_EVERY_MS, M.fw_st_at[n]);

    plink_fw_order_t in = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    const uint32_t idi = expect_answer(&in, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, last_st()->state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sv_last()->op);
    TEST_ASSERT_EQUAL_HEX32(idi, sv_last()->order);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.1"));
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3"));
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, NULL));
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_NOT_QUIET);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NOT_QUIET, last_st()->why);
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_INSTALLING, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_INSTALLING, last_st()->state);
    plink_fw_order_t f = order_of(PLINK_FW_OP_FETCH, "1.4.0");
    expect_answer(&f, PLINK_FW_WHY_BUSY);
    plink_fw_order_t r = relay_of(IMG2_SHA, 0u);
    expect_answer(&r, PLINK_FW_WHY_BUSY);
    /* The gate's second ask said no (put off): READY again, the hold kept for its next try */
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_NOT_QUIET);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    /* Its restart again; a CANCEL then is refused BUSY (a FETCH and a RELAY after it too), and
     * the restart goes on holding the INSTALL - until it is put off: the run ends as that CANCEL
     * said, the hold with it, and the service is told to discard the image (the core's own). */
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_INSTALLING, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_INSTALLING, last_st()->state);
    plink_fw_order_t c = order_of(PLINK_FW_OP_CANCEL, "");
    expect_answer(&c, PLINK_FW_WHY_BUSY);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_INSTALLING, last_st()->state);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    f = order_of(PLINK_FW_OP_FETCH, "1.4.0");
    expect_answer(&f, PLINK_FW_WHY_BUSY);
    r = relay_of(IMG2_SHA, 0u);
    expect_answer(&r, PLINK_FW_WHY_BUSY);
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_NOT_QUIET);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_STOPPED, last_st()->state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_CANCELLED, last_st()->why);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sv_last()->op);
    TEST_ASSERT_EQUAL_HEX32(0u, sv_last()->order);
    for (size_t i = 0; i < PLINK_FW_BUDGET; i++) {
        TEST_ASSERT_EQUAL_UINT64(0u, K.upd.fail_ms[i]); /* not a failed run */
    }

    /* A CANCEL of a READY image */
    o = relay_of(IMG2_SHA, 0u);
    const uint32_t idr = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 30000, verifying, NULL));
    plink_keypad_fw_report(&K, NET.now_ms, idr, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    in = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    expect_answer(&in, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    c = order_of(PLINK_FW_OP_CANCEL, "");
    const uint32_t idc = expect_answer(&c, PLINK_FW_WHY_CANCELLED);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_STOPPED, last_st()->state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_CANCELLED, last_st()->why);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    TEST_ASSERT_EQUAL_HEX32(idc, sv_last()->order);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sv_last()->op);
    n = M.n_fw_st;
    plink_net_run(&NET, 25000);
    TEST_ASSERT_EQUAL_size_t(n, M.n_fw_st);

    /* READY for 24 h without an INSTALL */
    o = relay_of(IMG2_SHA, 0u);
    const uint32_t id2 = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 30000, verifying, NULL));
    plink_keypad_fw_report(&K, NET.now_ms, id2, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    const uint64_t ready = NET.now_ms;
    plink_keypad_tick(&K, ready + PLINK_FW_READY_KEEP_MS - 1u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, K.upd.state);
    TEST_ASSERT_TRUE(plink_keypad_next(&K) <= ready + PLINK_FW_READY_KEEP_MS);
    plink_keypad_tick(&K, ready + PLINK_FW_READY_KEEP_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FAILED, K.upd.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_TIMEOUT, K.upd.why);
    sv_poll(ready + PLINK_FW_READY_KEEP_MS);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sv_last()->op);
    TEST_ASSERT_EQUAL_HEX32(0u, sv_last()->order);
    for (size_t i = 0; i < PLINK_FW_BUDGET; i++) {
        TEST_ASSERT_EQUAL_UINT64(0u, K.upd.fail_ms[i]); /* not a failed run */
    }
}

/* LINK-034: the INSTALL hold is held only in the session it came in and while no order but INFO
 * came after it. An INFO leaves it (INFO only names the master's version), and so does the
 * master's UPDATING (keypads restart with their master); a PROBE taken after it voids it, and so
 * does a new session (a master that restarted, on its old version or a new one, never finds the
 * keypad restarting on an INSTALL of before); the READY image stays. A CANCEL refused BUSY - its
 * restart has started - leaves it. */
static void test_fw_install_hold_ends(void)
{
    kp_fw_online();
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 30000, verifying, NULL));
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    plink_fw_order_t in = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    expect_answer(&in, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    /* An INFO after it: still held. */
    plink_fw_order_t info = order_of(PLINK_FW_OP_INFO, "1.3.0");
    expect_answer(&info, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    /* The master's own update (UPDATING): still held. */
    M.updating = true;
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    M.updating = false;
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    /* A PROBE after it. */
    plink_fw_order_t pr = order_of(PLINK_FW_OP_PROBE, "1.3.0");
    expect_answer(&pr, PLINK_FW_WHY_OK);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, kst().state);
    in = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    expect_answer(&in, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    /* A new session. */
    const uint32_t sid = K.live.sid;
    fm_rekey(sid, NET_ID);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, new_session, NULL));
    plink_net_run(&NET, 100);
    TEST_ASSERT_FALSE(plink_keypad_fw_install_for(&K, "1.3.0"));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, kst().state);
    /* An INSTALL of this session, and a CANCEL refused BUSY once its restart started. */
    in = order_of(PLINK_FW_OP_INSTALL, "1.3.0");
    expect_answer(&in, PLINK_FW_WHY_OK);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_INSTALLING, PLINK_FW_WHY_OK);
    plink_fw_order_t c = order_of(PLINK_FW_OP_CANCEL, "");
    expect_answer(&c, PLINK_FW_WHY_BUSY);
    TEST_ASSERT_TRUE(plink_keypad_fw_install_for(&K, "1.3.0"));
}

/* LINK-029, LINK-031: FETCH goes to the service, whose progress goes as FW_STATE at most once a
 * second (bytes, size, the image's id8), each change of state or why at once; a report about
 * another run is ignored; the last section (probation, then the result) goes when it changes. */
static void test_fw_fetch_and_last(void)
{
    kp_fw_online();
    plink_fw_order_t o = order_of(PLINK_FW_OP_FETCH, "1.3.0");
    const uint32_t id = expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FETCHING, last_st()->state);
    TEST_ASSERT_EQUAL_STRING("1.3.0", last_st()->target);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sv_last()->op);
    TEST_ASSERT_EQUAL_STRING("https://updates.example.org", sv_last()->server);
    const uint8_t id8[PLINK_ID8_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
    const size_t n = M.n_fw_st;
    for (uint32_t k = 1; k <= 30u; k++) {
        plink_keypad_fw_progress(&K, NET.now_ms, id, k * 1000u, 30000u, id8);
        plink_net_run(&NET, 100);
    }
    TEST_ASSERT_TRUE(M.n_fw_st - n >= 2u && M.n_fw_st - n <= 4u);
    for (size_t i = n + 1u; i < M.n_fw_st; i++) {
        TEST_ASSERT_TRUE(M.fw_st_at[i] >= M.fw_st_at[i - 1u] + PLINK_FW_STATE_GAP_MS);
    }
    TEST_ASSERT_EQUAL_UINT32(30000u, last_st()->size);
    TEST_ASSERT_EQUAL_MEMORY(id8, last_st()->id8, sizeof id8);
    plink_keypad_fw_progress(&K, NET.now_ms, id + 1u, 1u, 2u, NULL); /* not this run */
    TEST_ASSERT_EQUAL_UINT32(30000u, kst().bytes);
    plink_keypad_fw_report(&K, NET.now_ms, id + 1u, PLINK_FWS_FAILED, PLINK_FW_WHY_BROKE_OFF);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FETCHING, kst().state);
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_KEY_WAIT, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_FETCHING, kst().state); /* no person's step: ignored */
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_VERIFYING, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_VERIFYING, last_st()->state);
    plink_keypad_fw_report(&K, NET.now_ms, id, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_READY, last_st()->state);

    /* After the restart: probation, then the result */
    plink_keypad_fw_last(&K, NET.now_ms, 1u, PLINK_FW_LAST_NONE, "", 0u);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(1u, last_st()->probation);
    plink_keypad_fw_last(&K, NET.now_ms, 0u, PLINK_FW_LAST_ROLLED_BACK, "1.3.0", 0x24u);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(0u, last_st()->probation);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_LAST_ROLLED_BACK, last_st()->last);
    TEST_ASSERT_EQUAL_STRING("1.3.0", last_st()->last_version);
    TEST_ASSERT_EQUAL_HEX8(0x24u, last_st()->failed_mask);
    const size_t m = M.n_fw_st;
    plink_keypad_fw_last(&K, NET.now_ms, 0u, PLINK_FW_LAST_ROLLED_BACK, "1.3.0", 0x24u);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(m, M.n_fw_st);
}

/* LINK-035: a keypad removed at the master (BYE) ends its run and drops the orders waiting: the
 * service gets only the core's CANCEL, to discard what it holds; nothing more is asked for. */
static void test_fw_removed_discards(void)
{
    kp_fw_online();
    SV.take_max = 512u;
    SV.busy_ms = 200u;
    plink_fw_order_t o = relay_of(IMG_SHA, 0u);
    expect_answer(&o, PLINK_FW_WHY_OK);
    plink_net_run(&NET, 1500);
    SV.on = false;
    o = order_of(PLINK_FW_OP_INFO, "");
    expect_answer(&o, PLINK_FW_WHY_OK);
    const plink_bye_t bye = {1u};
    uint8_t p[4];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_bye(&bye, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_BYE, p, n);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, info().state);
    plink_fw_order_t got;
    TEST_ASSERT_TRUE(plink_keypad_take_fw_order(&K, &got));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, got.op);
    TEST_ASSERT_EQUAL_HEX32(0u, got.order);
    TEST_ASSERT_FALSE(plink_keypad_take_fw_order(&K, &got));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWS_IDLE, kst().state);
    TEST_ASSERT_EQUAL_HEX32(0u, kst().order);
    expect_held(5000);
}

/* The master's version as the keypad keeps it now, and its seq. */
static void expect_mver(const char *v, uint32_t seq)
{
    uint32_t s = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_STRING(v, plink_keypad_master_version(&K, &s));
    TEST_ASSERT_EQUAL_UINT32(seq, s);
}

/* An order sealed in the master's live session (with the next order id), not sent. */
static size_t fm_order_frame(plink_fw_order_t *o, uint8_t *f)
{
    o->order = M.next_order++;
    uint8_t p[PLINK_FW_ORDER_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_fw_order(o, p, sizeof p, &n));
    return fm_seal(PLINK_T_FW_ORDER, p, n, f);
}

/* LINK-029 (Decisions, keypad updates, 5): the keypad keeps the version its master runs - the one
 * it may not pass - as INFO names it, and only INFO: PROBE, FETCH, INSTALL and RELAY name the
 * master's target, which may be ahead of what it runs, and change nothing; nor does an INFO with no
 * version (the master on its self-test), or of a kind the keypad doesn't know. A newer version
 * replaces it (the master was updated), an older one too (it went back, or was put back by cable:
 * the cap follows the master). Only the paired master's sealed frames count, in frame order: a
 * forged one, one sealed with another master's keys, one of another network, an unsealed one, a
 * repeat of an earlier INFO and one from an older frame never set it. The seq moves only on a
 * change. */
static void test_fw_master_version_from_info(void)
{
    kp_fw_online();
    expect_mver("", 0u);
    TEST_ASSERT_NOT_NULL(plink_keypad_master_version(&K, NULL));
    TEST_ASSERT_EQUAL_STRING("", plink_keypad_master_version(NULL, NULL));

    /* Not the paired master's: nothing taken, nothing answered */
    plink_fw_order_t o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    uint8_t p[PLINK_FW_ORDER_MAX], f[PLINK_FRAME_MAX];
    size_t fl = fm_order_frame(&o, f);
    f[fl - 1u] ^= 0x80u; /* forged: the tag fails */
    deliver(f, fl);
    size_t n = 0;
    o.order = M.next_order++;
    TEST_ASSERT_TRUE(plink_enc_fw_order(&o, p, sizeof p, &n));
    plink_session_t other = M.live; /* another master's keys, under the same session id */
    memset(other.k_tx, 0x5A, sizeof other.k_tx);
    other.tx_seq += 100000u; /* (never a nonce the keypad opens again with the master's frames) */
    TEST_ASSERT_TRUE(plink_session_seal(&M.c, &other, PLINK_T_FW_ORDER, NET_ID,
                                        PLINK_SENDER_MASTER, true, p, n, f, sizeof f, &fl));
    deliver(f, fl);
    TEST_ASSERT_TRUE(plink_session_seal(&M.c, &other, PLINK_T_FW_ORDER, NET_ID ^ 0x0100u,
                                        PLINK_SENDER_MASTER, true, p, n, f, sizeof f, &fl));
    deliver(f, fl); /* ... of another network */
    const plink_hdr_t plain = {PLINK_T_FW_ORDER,    PLINK_FLAG_FROM_MASTER, NET_ID,
                               PLINK_SENDER_MASTER, K.live.sid,             other.tx_seq + 100u};
    plink_hdr_write(&plain, f);
    memcpy(&f[PLINK_HDR_LEN], p, n);
    deliver(f, PLINK_HDR_LEN + n); /* unsealed */
    plink_session_wipe(&other);
    sv_poll(NET.now_ms);
    plink_net_run(&NET, 20);
    expect_mver("", 0u);
    TEST_ASSERT_EQUAL_size_t(0u, M.n_fw_st);
    TEST_ASSERT_EQUAL_size_t(0u, SV.n_taken);

    /* INFO with no version (the master on its self-test): nothing */
    o = order_of(PLINK_FW_OP_INFO, "");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("", 0u);
    /* INFO: the master's version */
    o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.4.0", 1u);
    /* The other orders name the master's target: nothing */
    o = order_of(PLINK_FW_OP_PROBE, "1.4.2");
    expect_answer(&o, PLINK_FW_WHY_OK);
    o = order_of(PLINK_FW_OP_FETCH, "1.4.3");
    expect_answer(&o, PLINK_FW_WHY_OK);
    o = order_of(PLINK_FW_OP_INSTALL, "1.4.4");
    expect_answer(&o, PLINK_FW_WHY_OK);
    o = relay_of(IMG2_SHA, 0u);
    snprintf(o.version, sizeof o.version, "1.4.5");
    expect_answer(&o, PLINK_FW_WHY_OK);
    o = order_of(PLINK_FW_OP_CANCEL, "");
    expect_answer(&o, PLINK_FW_WHY_CANCELLED);
    expect_mver("1.4.0", 1u);
    /* Orders of an op or kind it doesn't know: refused, and naming nothing */
    o = order_of(7u, "9.9.9");
    expect_answer(&o, PLINK_FW_WHY_UNKNOWN_OP);
    o = order_of(PLINK_FW_OP_INFO, "2.13.0");
    o.kind = PLINK_FW_KIND_C6;
    expect_answer(&o, PLINK_FW_WHY_UNKNOWN_KIND);
    expect_mver("1.4.0", 1u);
    /* A newer version: the master was updated */
    o = order_of(PLINK_FW_OP_INFO, "1.5.0");
    expect_answer(&o, PLINK_FW_WHY_CANCELLED); /* (taken: the why is the run's, cancelled) */
    const plink_fw_order_t newer = o;
    expect_mver("1.5.0", 2u);
    /* An older one: the master went back, and the cap follows it */
    o = order_of(PLINK_FW_OP_INFO, "1.4.1");
    expect_answer(&o, PLINK_FW_WHY_CANCELLED);
    expect_mver("1.4.1", 3u);
    /* A repeat of the INFO that named 1.5.0: answered again, nothing taken */
    const size_t sts = M.n_fw_st;
    fm_order_again(&newer);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_size_t(sts + 1u, M.n_fw_st);
    TEST_ASSERT_EQUAL_HEX32(newer.order, last_st()->order);
    expect_mver("1.4.1", 3u);
    /* Sealed in this order, arriving in the other: the newer frame's version stays */
    plink_fw_order_t a = order_of(PLINK_FW_OP_INFO, "1.6.0");
    plink_fw_order_t b = order_of(PLINK_FW_OP_INFO, "1.7.0");
    uint8_t fa[PLINK_FRAME_MAX], fb[PLINK_FRAME_MAX];
    const size_t na = fm_order_frame(&a, fa);
    const size_t nb = fm_order_frame(&b, fb);
    deliver(fb, nb);
    deliver(fa, na);
    sv_poll(NET.now_ms);
    plink_net_run(&NET, 20);
    expect_mver("1.7.0", 4u);
}

/* LINK-034 (decided 2026-10-17: on trial with the master): after an install its master ordered,
 * the keypad's trial passes on an INFO naming its version or a newer one, fails on an older one
 * (the master went back) and fails with none within 10 min - judged once; only INFO counts (an
 * order naming its version doesn't); without one started there is none. */
static void test_fw_trial(void)
{
    /* Started at start-up, passed by its master's INFO in its first session */
    M.caps = KCAPS = PLINK_CAP_FW;
    SV.on = SV.go = true;
    kp_init(true);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_NONE, plink_keypad_fw_trial_state(&K, NET.now_ms));
    plink_keypad_fw_trial(&K, NET.now_ms, "1.4.0");
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_WAIT, plink_keypad_fw_trial_state(&K, NET.now_ms));
    TEST_ASSERT_TRUE(plink_keypad_next(&K) <= NET.now_ms + PLINK_FW_TRIAL_MS);
    const plink_fw_info_t i = kinfo("1.2.3");
    plink_keypad_fw_info(&K, NET.now_ms, &i);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    plink_fw_order_t o = order_of(PLINK_FW_OP_FETCH, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_WAIT, plink_keypad_fw_trial_state(&K, NET.now_ms));
    o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_PASS, plink_keypad_fw_trial_state(&K, NET.now_ms));
    o = order_of(PLINK_FW_OP_INFO, "1.3.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_PASS, plink_keypad_fw_trial_state(&K, NET.now_ms));
    /* An older one: failed */
    kp_fw_online();
    plink_keypad_fw_trial(&K, NET.now_ms, "1.4.0");
    o = order_of(PLINK_FW_OP_INFO, "1.3.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_FAIL, plink_keypad_fw_trial_state(&K, NET.now_ms));
    /* A newer one: passed */
    kp_fw_online();
    plink_keypad_fw_trial(&K, NET.now_ms, "1.4.0");
    o = order_of(PLINK_FW_OP_INFO, "1.4.1");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_PASS, plink_keypad_fw_trial_state(&K, NET.now_ms));
    /* None within 10 min: failed, and an INFO after that changes nothing */
    kp_fw_online();
    plink_keypad_fw_trial(&K, NET.now_ms, "1.4.0");
    const uint64_t from = NET.now_ms;
    plink_net_run(&NET, PLINK_FW_TRIAL_MS - 1000u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_WAIT, plink_keypad_fw_trial_state(&K, NET.now_ms));
    plink_net_run(&NET, from + PLINK_FW_TRIAL_MS - NET.now_ms);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_FAIL, plink_keypad_fw_trial_state(&K, NET.now_ms));
    o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWT_FAIL, plink_keypad_fw_trial_state(&K, NET.now_ms));
}

/* A session other than sid, its first frame heard. */
static bool session_since(void *ctx)
{
    const uint32_t *sid = ctx;
    return K.live.valid && K.live.sid != *sid && K.items_sid == K.live.sid && !K.tentative;
}

static bool stored_since(void *ctx)
{
    return KS.writes > *(const unsigned *)ctx;
}

/* Online in a new session (the master restarted with caps). */
static void fm_restart_with(uint16_t caps)
{
    const uint32_t sid = K.live.sid;
    M.caps = caps;
    fm_restart();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 20000, session_since, (void *)&sid));
}

/* LINK-029: the master's version goes with the master, and each clearing moves the seq once: at
 * removal (BYE, or forgotten at the keypad), at a new pairing - a frame of the earlier pairing's
 * session never brings it back - and in a session without FW (a master put back to firmware from
 * before keypad updates, which names none: the keypad then offers no Install while paired). The
 * service's stored version, restored at start-up, holds before the first INFO without moving the
 * seq, only while paired and only before one is heard. */
static void test_fw_master_version_goes_with_the_master(void)
{
    kp_fw_online();
    plink_fw_order_t o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.4.0", 1u);

    /* A session without FW: none any more; with FW again, INFO names it again */
    fm_restart_with(0u);
    expect_mver("", 2u);
    fm_restart_with(0u);
    expect_mver("", 2u);
    fm_restart_with(PLINK_CAP_FW);
    expect_mver("", 2u);
    o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.4.0", 3u);

    /* Restarted: the stored version back before the first INFO, the seq unmoved; the same one
     * named again changes nothing */
    kp_init(true);
    expect_mver("", 0u);
    plink_keypad_master_version_restore(&K, "1.4.0", NET_ID);
    expect_mver("1.4.0", 0u);
    plink_keypad_master_version_restore(&K, "1.3.0", NET_ID); /* once only */
    expect_mver("1.4.0", 0u);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    o = order_of(PLINK_FW_OP_INFO, "1.4.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.4.0", 0u);
    o = order_of(PLINK_FW_OP_INFO, "1.5.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.5.0", 1u);
    plink_keypad_master_version_restore(&K, "1.4.0", NET_ID); /* one is heard: ignored */
    expect_mver("1.5.0", 1u);

    /* Removed at the master (BYE) */
    const plink_bye_t bye = {1u};
    uint8_t p[PLINK_FW_ORDER_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_bye(&bye, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_BYE, p, n);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, info().state);
    expect_mver("", 2u);
    /* Not paired: nothing is restored */
    kp_init(false);
    plink_keypad_master_version_restore(&K, "1.5.0", NET_ID);
    expect_mver("", 0u);

    /* Forgotten at the keypad */
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    o = order_of(PLINK_FW_OP_INFO, "1.5.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.5.0", 1u);
    TEST_ASSERT_TRUE(plink_keypad_forget(&K, NET.now_ms));
    expect_mver("", 2u);

    /* Paired again, to another master: none until the new one names it; a frame of the earlier
     * pairing's session, sealed before and arriving after, doesn't bring the old one back */
    kp_init(true);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    o = order_of(PLINK_FW_OP_INFO, "1.5.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.5.0", 1u);
    uint8_t f[PLINK_FRAME_MAX];
    o = order_of(PLINK_FW_OP_INFO, "1.9.0");
    const size_t fl = fm_order_frame(&o, f);
    plink_pm_init(&M.pm, &M.c, 0u);
    plink_pm_identity(&M.pm, NET_ID, "Harbor House", "A1B2C3", ZONES);
    const unsigned w0 = KS.writes;
    pair_once();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 3000, stored_since, (void *)&w0));
    expect_mver("", 2u);
    deliver(f, fl);
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    deliver(f, fl);
    expect_mver("", 2u);
    o = order_of(PLINK_FW_OP_INFO, "1.6.0");
    expect_answer(&o, PLINK_FW_WHY_OK);
    expect_mver("1.6.0", 3u);
}

/* LINK-029 (Decisions, keypad updates, 5): the cap the service applies follows the pairing - none
 * unpaired (a stand-alone panel has no master), nothing past "" once paired until its master names
 * a version, then that version - and the seq moves at each pairing and each removal even while no
 * version is known, so a service that applies the cap at every move, and once right after the
 * restore, never misses one. A version stored is restored only on its own pairing's network (one
 * of an earlier pairing, left by a power cut before the service erased it, is not). */
static void test_fw_version_cap_follows_the_pairing(void)
{
    const char *v = NULL;
    uint16_t net = 0xFFFFu;
    TEST_ASSERT_FALSE(plink_keypad_version_cap(NULL, &v, &net));
    TEST_ASSERT_EQUAL_STRING("", v);
    TEST_ASSERT_EQUAL_HEX16(0u, net);
    /* Never paired: no cap, nothing restored */
    M.paired = false;
    memset(M.k_pair, 0, sizeof M.k_pair);
    M.caps = KCAPS = PLINK_CAP_FW;
    plink_pm_init(&M.pm, &M.c, 0u);
    plink_pm_identity(&M.pm, NET_ID, "Harbor House", "A1B2C3", ZONES);
    kp_init(false);
    TEST_ASSERT_FALSE(plink_keypad_version_cap(&K, &v, &net));
    TEST_ASSERT_EQUAL_STRING("", v);
    plink_keypad_master_version_restore(&K, "1.4.0", NET_ID);
    expect_mver("", 0u);
    /* Paired, no version named yet: the seq moves, and nothing passes */
    pair_once();
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    expect_mver("", 1u);
    TEST_ASSERT_TRUE(plink_keypad_version_cap(&K, &v, &net));
    TEST_ASSERT_EQUAL_STRING("", v);
    TEST_ASSERT_EQUAL_HEX16(NET_ID, net);
    TEST_ASSERT_TRUE(plink_keypad_version_cap(&K, NULL, NULL));
    /* Removed at the master (BYE) while none is known: the seq moves, no cap */
    const plink_bye_t bye = {1u};
    uint8_t p[PLINK_FW_ORDER_MAX];
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_bye(&bye, p, sizeof p, &n));
    fm_send_sealed(PLINK_T_BYE, p, n);
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, info().state);
    expect_mver("", 2u);
    TEST_ASSERT_FALSE(plink_keypad_version_cap(&K, &v, &net));
    TEST_ASSERT_EQUAL_STRING("", v);
    TEST_ASSERT_EQUAL_HEX16(0u, net);
    /* Forgotten while not paired: nothing changes */
    (void)plink_keypad_forget(&K, NET.now_ms);
    expect_mver("", 2u);

    /* Paired at start-up: a version stored under another network isn't restored; its own is,
     * and is the cap from the first moment */
    kp_init(true);
    TEST_ASSERT_TRUE(plink_keypad_version_cap(&K, &v, NULL));
    TEST_ASSERT_EQUAL_STRING("", v);
    plink_keypad_master_version_restore(&K, "1.4.0", (uint16_t)(NET_ID ^ 0x0100u));
    expect_mver("", 0u);
    plink_keypad_master_version_restore(&K, "1.4.0", NET_ID);
    expect_mver("1.4.0", 0u);
    TEST_ASSERT_TRUE(plink_keypad_version_cap(&K, &v, &net));
    TEST_ASSERT_EQUAL_STRING("1.4.0", v);
    TEST_ASSERT_EQUAL_HEX16(NET_ID, net);
}

static void edit_fire(plink_status_t *st)
{
    st->flags |= PLINK_SF_FIRE;
}

/* LINK-030, LINK-034, LINK-039: the link's part of the INSTALL gate - NO_STATUS without a session
 * or a STATUS under 15 s old, NOT_QUIET (the master's HOLD, or its partition armed),
 * PANIC_PENDING; never the master's UPDATING (keypads restart with their master) - and the STATUS
 * flags as sent: fire from the master's FIRE sounds on the keypad. */
static void test_fw_gate_and_flags(void)
{
    M.caps = KCAPS = PLINK_CAP_FW;
    kp_init(true);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NO_STATUS, plink_keypad_fw_hold(&K, NET.now_ms));
    TEST_ASSERT_EQUAL_HEX8(0u, plink_keypad_master_flags(&K));
    TEST_ASSERT_TRUE(plink_net_run_until(&NET, 5000, online, NULL));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, plink_keypad_fw_hold(&K, NET.now_ms));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK,
                            plink_keypad_fw_hold(&K, K.status_ms + PLINK_FW_FRESH_MS - 1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NO_STATUS,
                            plink_keypad_fw_hold(&K, K.status_ms + PLINK_FW_FRESH_MS));
    M.updating = true;
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, plink_keypad_fw_hold(&K, NET.now_ms));
    M.hold = true;
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NOT_QUIET, plink_keypad_fw_hold(&K, NET.now_ms));
    TEST_ASSERT_EQUAL_HEX8(PLINK_SF_UPDATING | PLINK_SF_HOLD,
                           plink_keypad_master_flags(&K) & (PLINK_SF_UPDATING | PLINK_SF_HOLD));
    M.updating = M.hold = false;
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_NOT_QUIET, plink_keypad_fw_hold(&K, NET.now_ms));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, plink_keypad_fw_hold(&K, NET.now_ms));
    M.serve_cmds = false;
    const plink_cmd_t pc = panic_cmd();
    TEST_ASSERT_NOT_EQUAL(0u, plink_keypad_command(&K, NET.now_ms, &pc));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_PANIC_PENDING, plink_keypad_fw_hold(&K, NET.now_ms));
    plink_net_run(&NET, PLINK_PANIC_KEEP_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_OK, plink_keypad_fw_hold(&K, NET.now_ms));

    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_NONE, plink_keypad_continuous(&K));
    M.edit = edit_fire;
    fm_status();
    plink_net_run(&NET, 20);
    TEST_ASSERT_EQUAL_HEX8(PLINK_SF_FIRE, plink_keypad_master_flags(&K) & PLINK_SF_FIRE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_FIRE, plink_keypad_continuous(&K));
    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_NONE, K.view.snap.part[0].sounder); /* not its own */
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
    RUN_TEST(test_maintenance_when_not_paired);
    RUN_TEST(test_reordered_frames);
    RUN_TEST(test_master_engine_time_never_steps_back);
    RUN_TEST(test_config_pull_requests);
    RUN_TEST(test_config_pull_through_loss);
    RUN_TEST(test_config_changes_mid_transfer);
    RUN_TEST(test_config_bad_blob);
    RUN_TEST(test_config_bad_forgotten_when_another_installs);
    RUN_TEST(test_zones_requested_on_crc_mismatch);
    RUN_TEST(test_events_backfill_and_ring);
    RUN_TEST(test_events_hole_filled);
    RUN_TEST(test_short_ring_backfilled);
    RUN_TEST(test_command_and_retries);
    RUN_TEST(test_command_gives_up_at_3s);
    RUN_TEST(test_request_ids_unique_across_restart);
    RUN_TEST(test_panic_through_outage);
    RUN_TEST(test_panic_given_up_after_60s);
    RUN_TEST(test_panic_keyed_again_is_waited_for_again);
    RUN_TEST(test_bye_removes);
    RUN_TEST(test_not_paired_hint_after_5_min);
    RUN_TEST(test_rekey_after_master_restart);
    RUN_TEST(test_rekey_hints_limited);
    RUN_TEST(test_handshake_backoff);
    RUN_TEST(test_master_moves);
    RUN_TEST(test_rekey_at_seq_2_30);
    RUN_TEST(test_handshake_after_15_s_without_status);
    RUN_TEST(test_welcome_only_from_its_hello_and_counted);
    RUN_TEST(test_hello_by_subnet_broadcast);
    RUN_TEST(test_hello_to_unnamed_broadcast_takes_no_other_welcome);
    RUN_TEST(test_master_ending_255_keeps_welcome_check);
    RUN_TEST(test_addresses_and_heartbeat);
    RUN_TEST(test_sounds_time_and_home);
    RUN_TEST(test_forget);
    RUN_TEST(test_home_offered_only_with_a_mirror);
    RUN_TEST(test_home_needs_frames_of_250);
    RUN_TEST(test_home_frames_need_their_capability);
    RUN_TEST(test_dev_list_pull_requests);
    RUN_TEST(test_dev_list_installed_with_its_records);
    RUN_TEST(test_dev_list_bad_blob_remembered);
    RUN_TEST(test_dev_list_that_fails_reads_offline);
    RUN_TEST(test_dev_list_change_carries_records_by_ref);
    RUN_TEST(test_dev_list_dropped_without_home);
    RUN_TEST(test_dev_records_in_frame_order);
    RUN_TEST(test_dev_records_only_under_installed_list);
    RUN_TEST(test_dev_records_kept_as_they_came);
    RUN_TEST(test_dev_states_catch_up);
    RUN_TEST(test_dev_freshness_30_s);
    RUN_TEST(test_dev_act_refused_at_once);
    RUN_TEST(test_dev_act_resent_then_given_up);
    RUN_TEST(test_dev_act_answered);
    RUN_TEST(test_dev_view_for_the_screen);
    RUN_TEST(test_dev_act_slots_apart_from_commands);
    RUN_TEST(test_dev_requests_wait_for_panic);
    RUN_TEST(test_prefs_report_timing);
    RUN_TEST(test_heartbeat_prefs_section);
    RUN_TEST(test_prefs_set_applied_once);
    RUN_TEST(test_fw_nothing_without_the_bit);
    RUN_TEST(test_fw_info_timing);
    RUN_TEST(test_fw_info_can);
    RUN_TEST(test_fw_orders_answered_once);
    RUN_TEST(test_fw_queue_full);
    RUN_TEST(test_fw_order_refusals);
    RUN_TEST(test_fw_relay_in_order);
    RUN_TEST(test_fw_relay_through_loss);
    RUN_TEST(test_fw_resends_and_stall);
    RUN_TEST(test_fw_page_checks);
    RUN_TEST(test_fw_writer_paces_the_pull);
    RUN_TEST(test_fw_relay_pauses);
    RUN_TEST(test_fw_pause_times_out);
    RUN_TEST(test_fw_timeout_at_the_gate_isnt_counted);
    RUN_TEST(test_fw_relay_across_sessions);
    RUN_TEST(test_fw_another_image_starts_over);
    RUN_TEST(test_fw_budget);
    RUN_TEST(test_fw_install_hold);
    RUN_TEST(test_fw_install_hold_ends);
    RUN_TEST(test_fw_fetch_and_last);
    RUN_TEST(test_fw_removed_discards);
    RUN_TEST(test_fw_master_version_from_info);
    RUN_TEST(test_fw_trial);
    RUN_TEST(test_fw_master_version_goes_with_the_master);
    RUN_TEST(test_fw_version_cap_follows_the_pairing);
    RUN_TEST(test_fw_gate_and_flags);
    return UNITY_END();
}
