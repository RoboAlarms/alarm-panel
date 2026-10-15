/*
 * test_zwz_replay.c - recorded exchanges with the ZAC93 as host tests (docs/features/36 README
 * "Test and acceptance"; the fixtures and how a bench recording becomes one: traces/README.md).
 *
 * A zwtrace recording (zwtrace.h) - panelctl's `zwave check` on the bench, or the fake ZAC93's
 * through the recorder (zwtrace_record.h) - is replayed into the panel's Z-Wave as zwave_service
 * runs it (zwz_panel, with the real zone source and hub) by zwtrace_replay.h: the recording plays
 * the module, the stack must make the recorded writes (callback IDs follow the stack's), and the
 * installer's actions come where the recording shows the writes they cause (the radio restarted
 * from the pages, the add and remove windows opened and stopped). Then the tests read what the
 * pages would show. The replay's stack starts as a fresh panel - nothing stored, no region chosen,
 * no reset line - on another clock than the recording's.
 *
 * Requirements: ZW-004 (the start-up exchange, again after the module restarted by itself, through
 * line noise), ZW-005 (the add window: listening, stopped), ZW-016 (the remove window: listening,
 * stopped) (docs/features/36).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_zac93.h"
#include "unity.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zwave.h"
#include "zw_host_crypto.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_frame.h"
#include "zwave_zones/zwz_panel.h"
#include "zwtrace.h"
#include "zwtrace_record.h"
#include "zwtrace_replay.h"

#define T0        1000u  /* the recorded panel's clock at its start */
#define REPLAY_T0 70000u /* the replay's: another one, on purpose */
#define SLACK_MS  10u    /* the installer acts this long after the recorded write was due */
#define GRACE_MS  40000u /* longer than any wait of the stack's own before a write (SendData's
                          * 30 s callback) */
#define FAKE_HOME 0xC0FFEE01u /* the fake ZAC93's home ID (fake_identity_default) */
#define SCRUBBED_HOME 0xCAFE0001u /* the one tools/zwtrace_scrub.py writes in its place */

static zwt_trace_t TRACE;
static char TEXT[256u * 1024u]; /* a recording being made */

/* ---- the world ------------------------------------------------------------------------------- */

/* What the pages showed of the operations. */
typedef struct {
    bool add_listened, remove_listened;
    uint32_t add_left, remove_left; /* the countdown when it was first seen listening */
    uint8_t add_result, remove_result;
} seen_t;

typedef struct {
    zwz_panel_t p;
    zw_host_crypto_t hc;
    zw_crypto_port_t crypto;
    zsrc_zw_t src;
    zone_hub_t hub;
    bool replaying;
    /* the fake module, its wire and the recorder */
    fake_zac93_t zac;
    fake_wire_t wire;
    zwt_rec_t rec;
    bool recording;
    /* the recorded module */
    zwt_replay_t replay;
    /* what the owner keeps (NVS zwave) */
    zwz_panel_setup_t setup;
    uint8_t nodes[16384];
    size_t nodes_len;
    zwz_checkin_t checkins[ZWZ_PANEL_CHECKINS];
    size_t checkin_n;
    unsigned key_writes;
    /* what it logged, what the pages showed */
    zwz_log_t log[32];
    size_t log_n;
    seen_t seen;
} world_t;

static world_t W;

static uint64_t now(void)
{
    return W.replaying ? W.replay.now : W.wire.now;
}

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (W.replaying) {
        return zwt_replay_write(&W.replay, bytes, len);
    }
    if (W.recording) {
        zwt_rec_panel(&W.rec, W.wire.now, bytes, len);
    }
    return fake_wire_write(&W.wire, bytes, len);
}

/* The fake wire's bytes as they go: the module's reach the recorder (the panel's writes were
 * recorded at the port, the link's answers the recorder makes itself). */
static void tap(void *ctx, bool to_module, uint64_t at, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (!to_module && W.recording) {
        zwt_rec_module(&W.rec, at, bytes, len);
    }
}

static bool port_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    return zone_hub_get(&W.hub, zone, out);
}

static void hub_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    (void)zone;
    (void)ev;
}

static void hub_changed(void *ctx)
{
    (void)ctx;
}

static void hub_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    (void)kind;
    (void)on;
}

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t v, uint64_t t)
{
    (void)ctx;
    zone_hub_input(&W.hub, a, state, v, t);
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t t)
{
    (void)ctx;
    zone_hub_event(&W.hub, a, ev, t);
}

static void sink_reachable(void *ctx, const zsrc_address_t *a, bool ok, uint64_t t)
{
    (void)ctx;
    zone_hub_reachable(&W.hub, a, ok, t);
}

static void sink_link(void *ctx, uint8_t kind, bool up, uint64_t t)
{
    (void)ctx;
    zone_hub_link(&W.hub, kind, up, t);
}

static void n_control(void *ctx, uint64_t t, uint8_t b)
{
    (void)ctx;
    zwz_panel_on_control(&W.p, t, b);
}

static void n_frame(void *ctx, uint64_t t, const zw_frame_t *f)
{
    (void)ctx;
    zwz_panel_on_frame(&W.p, t, f);
}

static void n_text(void *ctx, uint64_t t, const uint8_t *b, size_t n)
{
    (void)ctx;
    zwz_panel_on_text(&W.p, t, b, n);
}

static void n_poll(void *ctx, uint64_t t)
{
    (void)ctx;
    zwz_panel_poll(&W.p, t);
    zone_hub_tick(&W.hub, t);
}

static uint64_t n_deadline(void *ctx)
{
    (void)ctx;
    const uint64_t t = zwz_panel_next_deadline(&W.p);
    const uint64_t h = zone_hub_next_deadline(&W.hub, now());
    return h < t ? h : t;
}

/* What zwave_service does after unlocking: store what changed, log what happened. */
static void store(void)
{
    uint8_t buf[sizeof W.nodes];
    uint16_t gen = 0u;
    size_t n = zwz_panel_take_nodes(&W.p, buf, sizeof buf, &gen);
    if (n > 0u) {
        memcpy(W.nodes, buf, n);
        W.nodes_len = n;
        W.setup.generation = gen;
        zwz_panel_stored(&W.p, ZWZ_DIRTY_NODES); /* as zwz_store_run (ZW-014) */
    }
    n = zwz_panel_take_keys(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_TRUE(zwz_keys_decode(&W.setup.keys, buf, n));
        W.key_writes++;
    }
    n = zwz_panel_take_cfg(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_TRUE(zwz_cfg_decode(&W.setup.cfg, buf, n));
    }
    n = zwz_panel_take_checkins(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_TRUE(zwz_checkins_decode(W.checkins, ZWZ_PANEL_CHECKINS, &W.checkin_n, buf, n));
    }
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_panel_dirty(&W.p));
    zwz_log_t l;
    while (zwz_panel_take_log(&W.p, &l)) {
        TEST_ASSERT_TRUE(W.log_n < sizeof W.log / sizeof W.log[0]);
        W.log[W.log_n++] = l;
    }
}

/* The add and remove pages, as the installer would see them. */
static void observe(void)
{
    ui_zw_progress_t g;
    zwz_panel_progress(&W.p, now(), &g);
    const bool add = g.op == UI_ZW_OP_ADD;
    if (!add && g.op != UI_ZW_OP_REMOVE) {
        return;
    }
    bool *listened = add ? &W.seen.add_listened : &W.seen.remove_listened;
    if (g.step == UI_ZW_STEP_LISTENING && !*listened) {
        *listened = true;
        *(add ? &W.seen.add_left : &W.seen.remove_left) = g.seconds_left;
    } else if (g.step == UI_ZW_STEP_DONE) {
        *(add ? &W.seen.add_result : &W.seen.remove_result) = g.result;
    }
}

static void run(uint64_t t)
{
    if (W.replaying) {
        zwt_replay_run_until(&W.replay, t);
        TEST_ASSERT_FALSE_MESSAGE(W.replay.stuck, "the replay's wire did not settle");
    } else {
        fake_wire_run_until(&W.wire, t);
        TEST_ASSERT_FALSE_MESSAGE(W.wire.stuck, "the wire did not settle");
    }
    store();
    observe();
}

static void run_for(uint64_t ms)
{
    run(now() + ms);
}

static void world_reset(bool replaying)
{
    memset(&W, 0, sizeof W);
    W.replaying = replaying;
    zw_host_crypto_init(&W.hc, 0x50414E45u);
    W.crypto = zw_host_crypto_port(&W.hc);
    fake_zac93_init(&W.zac);
    W.setup.identity.firmware_major = 1u;
    W.setup.identity.manufacturer_id = 0xFFFFu;
    W.setup.identity.product_type = 0xFFFFu;
    W.setup.identity.product_id = 0xFFFFu;
    /* the recordings predate jamming detection (network.md §9): its reading every 5 s would be
     * writes they never saw, so they replay with it off (setup.jam stays false) */
}

/* The panel (re)starts at t with what the owner stored (replaying: the replay's clock is at t). */
static void boot(uint64_t t)
{
    static const zone_hub_port_t hport = {NULL, hub_event, hub_changed, hub_trouble};
    static const zsrc_sink_t sink = {NULL, sink_input, sink_event, sink_reachable, sink_link, NULL};
    static const zwz_panel_port_t port = {.write = port_write, .binding = port_binding};
    zsrc_zw_init(&W.src, &sink);
    zone_hub_init(&W.hub, &hport);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&W.hub, zsrc_zw_ops(), &W.src, t));
    zone_hub_start(&W.hub, t);
    zwz_panel_init(&W.p, &port, &W.setup, W.checkins, W.checkin_n, &W.src, &W.crypto);
    if (W.nodes_len > 0u) {
        TEST_ASSERT_TRUE(zwz_panel_load_nodes(&W.p, W.nodes, W.nodes_len));
    }
    if (!W.replaying) {
        static const fake_host_t fh = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
        fake_wire_init(&W.wire, &W.zac, &fh, t);
        W.wire.tap.bytes = tap;
    }
    zwz_panel_start(&W.p, t);
    run(t);
}

/* A request from the pages, as the installer makes it (installer mode, disarmed). */
static ui_zw_req_result_t ask(uint8_t kind)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.user = 1u;
    const ui_zw_req_result_t res = zwz_panel_request(&W.p, now(), &r, true);
    run(now());
    return res;
}

static void dismiss_if_done(void)
{
    ui_zw_progress_t g;
    zwz_panel_progress(&W.p, now(), &g);
    if (g.step == UI_ZW_STEP_DONE) {
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS));
    }
}

static ui_zw_status_t status(void)
{
    ui_zw_status_t st;
    zwz_panel_status(&W.p, now(), &st);
    return st;
}

static size_t logged(uint8_t type)
{
    size_t n = 0u;
    for (size_t i = 0; i < W.log_n; i++) {
        n += W.log[i].type == type;
    }
    return n;
}

void setUp(void)
{
    world_reset(false);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.zac.overflow, "the fake module ran out of room");
}

/* ---- recordings of the fake ZAC93 ------------------------------------------------------------ */

typedef enum {
    SC_STARTUP = 0,   /* the panel's first start against a factory module */
    SC_LINE_NOISE,    /* the same through noise: junk bytes, a frame cut off, a damaged answer */
    SC_RADIO_RESTART, /* started, then the module restarts by itself (its watchdog) */
    SC_CHECK_WINDOWS, /* `zwave check` with no device: restart, add window, remove window */
    SC_COUNT
} scenario_t;

/* The fixtures made from each (scrubbed first, traces/README.md). */
static const char *const SC_FILE[SC_COUNT] = {"startup.zwtrace", "startup_line_noise.zwtrace",
                                              "radio_restart.zwtrace", "check_windows.zwtrace"};

static void start_recording(void)
{
    char line[96];
    const fake_identity_t *id = &W.zac.id;
    zwt_rec_init(&W.rec, TEXT, sizeof TEXT);
    zwt_rec_comment(&W.rec, "synthetic, recorded by test_zwz_replay from the fake ZAC93");
    const int n = snprintf(line, sizeof line, "module firmware %u.%02u sdk %u.%u.%u chip %u",
                           (unsigned)id->firmware[0], (unsigned)id->firmware[1],
                           (unsigned)id->protocol[0], (unsigned)id->protocol[1],
                           (unsigned)id->protocol[2], (unsigned)id->chip_type);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof line);
    zwt_rec_comment(&W.rec, line);
    W.recording = true;
}

/* A window of `zwave check` (panelctl): open it, 3 s of listening, stop it, dismiss it. */
static void check_window(uint8_t kind)
{
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(kind));
    run_for(1000u);
    run_for(3000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP));
    run_for(1000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS));
}

/* A scenario against the fake ZAC93, recorded into TEXT (its length); what the pages showed stays
 * in W. A recording starts where panelctl's would: at the panel's start, or - `zwave check` - at
 * the check's first step. */
static size_t record(scenario_t sc)
{
    world_reset(false);
    if (sc != SC_CHECK_WINDOWS) {
        start_recording();
    }
    if (sc == SC_LINE_NOISE) {
        static const uint8_t junk[] = {0xFE, 0xFF, 0x00};
        static const uint8_t cut[] = {0x01, 0x10, 0x01, 0x15, 0x5A, 0x2D}; /* goes quiet */
        TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&W.zac, T0 + 100u, junk, sizeof junk));
        TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&W.zac, T0 + 200u, cut, sizeof cut));
    }
    boot(T0);
    if (sc == SC_LINE_NOISE) {
        run(T0 + 499u);
        W.zac.fault.corrupt_in = 11; /* the answer to GetSerialApiCapabilities: its ACK and 10
                                      * bytes pass, the 11th is damaged */
    }
    run(T0 + 2000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&W.p.net.host));
    if (sc == SC_RADIO_RESTART) {
        TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, now() + 1500u, 0x03u));
        run_for(10000u);
    } else if (sc == SC_CHECK_WINDOWS) {
        start_recording();
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_RESTART));
        run_for(8000u);
        dismiss_if_done();
        check_window(UI_ZW_REQ_ADD);
        check_window(UI_ZW_REQ_REMOVE);
    }
    run_for(1000u);
    zwt_rec_expire(&W.rec, now() + 1000u);
    W.recording = false;
    TEST_ASSERT_FALSE_MESSAGE(W.rec.full, "the recording did not fit");
    return W.rec.len;
}

/* ---- replaying ------------------------------------------------------------------------------- */

/* What the installer did, as the recording shows it: the write the recording expects next is one
 * the stack makes only when asked - the radio restarted from the pages (`zwave check` step b: the
 * start-up's lone NAK), an add or remove window opened or stopped. */
static bool act(const zwt_entry_t *e)
{
    const uint8_t *b = zwt_bytes(W.replay.trace, e);
    if (e->kind == ZWT_CTX && b[0] == ZW_NAK) {
        return zw_host_link(&W.p.net.host) == ZW_LINK_UP && ask(UI_ZW_REQ_RESTART) == UI_ZW_REQ_OK;
    }
    if (e->kind != ZWT_TX || !zwt_frame_ok(b, e->len) || b[2] != ZW_TYPE_REQUEST || e->len < 7u ||
        (b[3] != ZW_FUNC_ADD_NODE_TO_NETWORK && b[3] != ZW_FUNC_REMOVE_NODE_FROM_NETWORK)) {
        return false;
    }
    const uint8_t type = (uint8_t)(b[4] & 0x0Fu);
    if (type == ZW_ADD_NODE_TYPE_STOP) {
        return ask(UI_ZW_REQ_STOP) == UI_ZW_REQ_OK;
    }
    if (type == ZW_ADD_NODE_TYPE_ANY) {
        dismiss_if_done();
        return ask(b[3] == ZW_FUNC_ADD_NODE_TO_NETWORK ? UI_ZW_REQ_ADD : UI_ZW_REQ_REMOVE) ==
               UI_ZW_REQ_OK;
    }
    return false;
}

/* One segment: the module's entries at their times, the stack's writes as it makes them, the
 * installer's actions where it does not. A write that is neither made in time nor an action of
 * the installer's may still come from a timer of the stack's own (the recorded panel was a little
 * quicker): it gets until GRACE_MS after it was due, then it is reported as never made. */
static void drive(void)
{
    const zwt_entry_t *overdue = NULL;
    uint64_t overdue_at = 0u;
    for (unsigned guard = 0; guard < 100000u && W.replay.mismatches == 0u; guard++) {
        uint64_t due = 0u;
        uint64_t ahead = 0u;
        const zwt_entry_t *next = zwt_replay_owed(&W.replay, &due);
        if (next == NULL) {
            const uint64_t last = zwt_replay_last_due(&W.replay);
            run(last > now() ? last : now());
            return;
        }
        if (zwt_replay_module_ahead(&W.replay, &ahead)) {
            run(ahead > now() ? ahead : now()); /* what the module said before it, first */
            continue;
        }
        run((due > now() ? due : now()) + SLACK_MS);
        if (zwt_replay_owed(&W.replay, NULL) != next || W.replay.mismatches > 0u) {
            continue; /* made by the stack itself */
        }
        if (act(next)) {
            if (zwt_replay_owed(&W.replay, NULL) == next && W.replay.mismatches == 0u) {
                zwt_replay_fail(&W.replay, "the installer's action did not make the recorded write",
                                next);
                return;
            }
            continue;
        }
        if (overdue != next) {
            overdue = next;
            overdue_at = now();
        }
        const uint64_t d = n_deadline(NULL);
        if (d != UINT64_MAX && d > now() && d <= overdue_at + GRACE_MS) {
            run(d); /* the stack's next timer: it may be the one */
            continue;
        }
        zwt_replay_fail(&W.replay, "the stack never made the recorded write", next);
        return;
    }
}

/* The recording into a fresh panel, segment by segment (the panel restarting where it did). */
static void replay(const zwt_trace_t *t, bool from_startup)
{
    static const zwt_host_t host = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
    world_reset(true);
    uint64_t start = REPLAY_T0;
    zwt_replay_init(&W.replay, t, &host, start);
    if (from_startup) {
        TEST_ASSERT_TRUE_MESSAGE(zwt_replay_skip_to_startup(&W.replay),
                                 "no start-up in the recording (record with `panelctl zwave check`)");
    }
    boot(start);
    for (;;) {
        drive();
        if (W.replay.mismatches > 0u || !zwt_replay_at_restart(&W.replay)) {
            return;
        }
        start = now() + 1000u;
        TEST_ASSERT_TRUE(zwt_replay_next_segment(&W.replay, start));
        boot(start); /* the panel restarted, with what it had stored */
    }
}

static void assert_played(const char *what)
{
    static char why[ZWT_MISMATCH_LEN + 128u];
    const int n = snprintf(why, sizeof why, "%s: %s", what, W.replay.mismatch);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.replay.mismatches, why);
    TEST_ASSERT_TRUE_MESSAGE(zwt_replay_done(&W.replay), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.replay.control_diffs, what);
}

static void load(const char *dir, const char *name)
{
    char path[512];
    const int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_load(&TRACE, path), TRACE.error);
}

/* The pages show what the recording's module said about itself. */
static void assert_pages_match_module(const char *what)
{
    zwt_module_t m;
    zwt_module(&TRACE, &m);
    const ui_zw_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UI_ZW_RADIO_WORKING, st.radio, what);
    TEST_ASSERT_TRUE_MESSAGE(m.has_caps && m.has_init && m.has_sdk && m.has_region && m.has_home,
                             what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(m.firmware_major, st.firmware_major, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(m.firmware_minor, st.firmware_minor, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(m.sdk_major, st.sdk_major, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(m.sdk_minor, st.sdk_minor, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(m.sdk_patch, st.sdk_patch, what);
    TEST_ASSERT_TRUE_MESSAGE(st.region_known, what);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(m.region, st.region, what);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(m.home_id, st.home_id, what);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(m.chip_type, zw_host_facts(&W.p.net.host)->chip_type, what);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(m.node_id, zw_host_facts(&W.p.net.host)->node_id, what);
}

/* The first entry of a kind at or after `from` whose frame has this FUNC (0: any bytes). */
static size_t find(const zwt_trace_t *t, size_t from, uint8_t kind, uint8_t func)
{
    for (size_t i = from; i < t->n; i++) {
        const uint8_t *b = zwt_bytes(t, &t->e[i]);
        if (t->e[i].kind == kind && (func == 0u || (t->e[i].len >= 4u && b[3] == func))) {
            return i;
        }
    }
    return t->n;
}

/* ---- the reader ------------------------------------------------------------------------------ */

/* The format as panelctl writes it: comments anywhere, blank lines and Windows line ends,
 * `lost`, `# panel restarted`, every direction, a damaged frame taken as it came. */
static void test_the_reader_takes_a_recording(void)
{
    static const char text[] =
        "zwtrace 1\r\n"
        "# panelctl, RoboAlarms 1.2.3\r\n"
        "# module firmware 1.50 sdk 7.22.1 chip 8\n"
        "\n"
        "1 100 ctx 15\n"
        "2 600 tx 01030007FB\n"
        "3 601 crx 06\n"
        "4 603 rx 0104010528d6\n" /* the checksum is wrong: damaged, as it came */
        "5 603 ctx 15\n"
        "6 610 text feff00\n"
        "lost 3\n"
        "# panel restarted\n"
        "1 20 ctx 15\n"
        ZWT_SCRUBBED_MARK ": home IDs replaced\n";
    TEST_ASSERT_EQUAL_INT(ZWT_OK, zwt_parse(&TRACE, text, sizeof text - 1u));
    TEST_ASSERT_EQUAL_UINT(9u, TRACE.n);
    TEST_ASSERT_EQUAL_UINT32(3u, TRACE.lost);
    TEST_ASSERT_EQUAL_UINT32(1u, TRACE.restarts);
    TEST_ASSERT_TRUE(TRACE.scrubbed);
    static const uint8_t kinds[] = {ZWT_CTX, ZWT_TX,   ZWT_CRX,     ZWT_RX, ZWT_CTX,
                                    ZWT_TEXT, ZWT_LOST, ZWT_RESTART, ZWT_CTX};
    for (size_t i = 0; i < TRACE.n; i++) {
        TEST_ASSERT_EQUAL_UINT8(kinds[i], TRACE.e[i].kind);
    }
    const zwt_entry_t *tx = &TRACE.e[1];
    TEST_ASSERT_EQUAL_UINT32(2u, tx->seq);
    TEST_ASSERT_EQUAL_UINT64(600u, tx->t);
    TEST_ASSERT_EQUAL_UINT32(6u, tx->line);
    static const uint8_t caps[] = {0x01, 0x03, 0x00, 0x07, 0xFB};
    TEST_ASSERT_EQUAL_UINT16(sizeof caps, tx->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(caps, zwt_bytes(&TRACE, tx), sizeof caps);
    TEST_ASSERT_TRUE(zwt_frame_ok(zwt_bytes(&TRACE, tx), tx->len));
    TEST_ASSERT_FALSE(zwt_frame_ok(zwt_bytes(&TRACE, &TRACE.e[3]), TRACE.e[3].len));
    TEST_ASSERT_EQUAL_UINT32(3u, TRACE.e[6].lost);
    TEST_ASSERT_EQUAL_UINT64(20u, TRACE.e[8].t); /* the restarted panel's clock */

    char hex[11];
    TEST_ASSERT_EQUAL_UINT(10u, zwt_hex(caps, sizeof caps, hex, sizeof hex));
    TEST_ASSERT_EQUAL_STRING("01030007fb", hex);
    TEST_ASSERT_EQUAL_UINT(7u, zwt_hex(caps, sizeof caps, hex, 8u)); /* no room: cut, "..." */
    TEST_ASSERT_EQUAL_STRING("0103...", hex);
}

/* A file that is not a zwtrace 1 recording, or a line it cannot read, is refused with the line
 * and the reason. */
static void test_the_reader_refuses_what_it_cannot_read(void)
{
    static const struct {
        const char *text;
        zwt_result_t result;
        uint32_t line;
    } CASES[] = {
        {"", ZWT_ERR_HEADER, 1u},
        {"zwtrace 2\n1 0 tx 01030007fb\n", ZWT_ERR_HEADER, 1u},
        {"ZWTRACE 1\n", ZWT_ERR_HEADER, 1u},
        {"# zwtrace 1\n", ZWT_ERR_HEADER, 1u},
        {"zwtrace 1\n1 0 xx 06\n", ZWT_ERR_LINE, 2u},
        {"zwtrace 1\n1 0 tx 0103000\n", ZWT_ERR_LINE, 2u},  /* half a byte */
        {"zwtrace 1\n1 0 tx 01030g07\n", ZWT_ERR_LINE, 2u}, /* not hex */
        {"zwtrace 1\n1 0 tx\n", ZWT_ERR_LINE, 2u},
        {"zwtrace 1\n1 0 tx 06 06\n", ZWT_ERR_LINE, 2u},
        {"zwtrace 1\n1 x tx 06\n", ZWT_ERR_LINE, 2u},
        {"zwtrace 1\n# fine\n1 0 crx 0606\n", ZWT_ERR_LINE, 3u},
        {"zwtrace 1\nlost\n", ZWT_ERR_LINE, 2u},
        {"zwtrace 1\nlost many\n", ZWT_ERR_LINE, 2u},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(CASES[i].result,
                                      zwt_parse(&TRACE, CASES[i].text, strlen(CASES[i].text)),
                                      CASES[i].text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(CASES[i].line, TRACE.error_line, CASES[i].text);
        TEST_ASSERT_TRUE(TRACE.error[0] != '\0');
    }
    TEST_ASSERT_EQUAL_INT(ZWT_ERR_FILE, zwt_load(&TRACE, ZWZ_TRACE_DIR "/no-such-recording.zwtrace"));
}

/* ---- the recorder ---------------------------------------------------------------------------- */

/* The recorder writes what zw_link would: the start-up's NAK, the junk as text, the frame the byte
 * timeout cut off, the damaged answer NAKed and the good one sent again, each frame with the
 * link's answer after it. */
static void test_the_recorder_writes_what_the_link_saw(void)
{
    const size_t n = record(SC_LINE_NOISE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_parse(&TRACE, TEXT, n), TRACE.error);
    const zwt_entry_t *e = TRACE.e;
    TEST_ASSERT_EQUAL_UINT8(ZWT_CTX, e[0].kind);
    TEST_ASSERT_EQUAL_HEX8(ZW_NAK, zwt_bytes(&TRACE, &e[0])[0]);
    TEST_ASSERT_EQUAL_UINT64(T0, e[0].t);
    TEST_ASSERT_EQUAL_UINT8(ZWT_TEXT, e[1].kind);
    TEST_ASSERT_EQUAL_UINT64(T0 + 100u, e[1].t);
    TEST_ASSERT_EQUAL_UINT16(3u, e[1].len);
    const size_t caps = find(&TRACE, 0u, ZWT_TX, ZW_FUNC_GET_SERIAL_API_CAPABILITIES);
    TEST_ASSERT_TRUE(caps < TRACE.n);
    TEST_ASSERT_EQUAL_UINT64(T0 + 500u, e[caps].t);
    /* the cut-off frame goes in when the next byte shows it waited too long, then that ACK */
    TEST_ASSERT_EQUAL_UINT8(ZWT_RX, e[caps + 1u].kind);
    TEST_ASSERT_EQUAL_UINT16(6u, e[caps + 1u].len);
    TEST_ASSERT_EQUAL_UINT8(ZWT_CRX, e[caps + 2u].kind);
    /* the damaged answer, NAKed; the good one, ACKed */
    TEST_ASSERT_EQUAL_UINT8(ZWT_RX, e[caps + 3u].kind);
    TEST_ASSERT_FALSE(zwt_frame_ok(zwt_bytes(&TRACE, &e[caps + 3u]), e[caps + 3u].len));
    TEST_ASSERT_EQUAL_UINT8(ZWT_CTX, e[caps + 4u].kind);
    TEST_ASSERT_EQUAL_HEX8(ZW_NAK, zwt_bytes(&TRACE, &e[caps + 4u])[0]);
    TEST_ASSERT_EQUAL_UINT8(ZWT_RX, e[caps + 5u].kind);
    TEST_ASSERT_TRUE(zwt_frame_ok(zwt_bytes(&TRACE, &e[caps + 5u]), e[caps + 5u].len));
    TEST_ASSERT_EQUAL_HEX8(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, zwt_bytes(&TRACE, &e[caps + 5u])[3]);
    TEST_ASSERT_EQUAL_UINT8(ZWT_CTX, e[caps + 6u].kind);
    TEST_ASSERT_EQUAL_HEX8(ZW_ACK, zwt_bytes(&TRACE, &e[caps + 6u])[0]);
    /* sequence numbers from 1, one each */
    for (size_t i = 0; i < TRACE.n; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1u, e[i].seq);
    }
}

/* ---- recorded and replayed: the same outcome --------------------------------------------------- */

/* Records a scenario from the fake, replays the recording into a fresh panel: every write made as
 * recorded, and the pages show what they showed while it was recorded. */
static void round_trip(scenario_t sc)
{
    const size_t n = record(sc);
    const ui_zw_status_t before = status();
    const seen_t seen = W.seen;
    const size_t restarts = logged(ALARM_EV_ZWAVE_RADIO_RESTARTED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_parse(&TRACE, TEXT, n), TRACE.error);
    replay(&TRACE, false);
    assert_played(SC_FILE[sc]);
    const ui_zw_status_t after = status();
    TEST_ASSERT_EQUAL_UINT8(before.radio, after.radio);
    TEST_ASSERT_EQUAL_UINT8(before.firmware_major, after.firmware_major);
    TEST_ASSERT_EQUAL_UINT8(before.firmware_minor, after.firmware_minor);
    TEST_ASSERT_EQUAL_UINT8(before.sdk_major, after.sdk_major);
    TEST_ASSERT_EQUAL_UINT8(before.sdk_minor, after.sdk_minor);
    TEST_ASSERT_EQUAL_UINT8(before.sdk_patch, after.sdk_patch);
    TEST_ASSERT_EQUAL_UINT8(before.region, after.region);
    TEST_ASSERT_EQUAL_UINT8(before.region_count, after.region_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before.regions, after.regions, before.region_count);
    TEST_ASSERT_EQUAL_UINT16(before.devices, after.devices);
    TEST_ASSERT_EQUAL_HEX32(FAKE_HOME, after.home_id); /* not scrubbed: the fake's own */
    TEST_ASSERT_EQUAL(seen.add_listened, W.seen.add_listened);
    TEST_ASSERT_EQUAL_UINT8(seen.add_result, W.seen.add_result);
    TEST_ASSERT_EQUAL(seen.remove_listened, W.seen.remove_listened);
    TEST_ASSERT_EQUAL_UINT8(seen.remove_result, W.seen.remove_result);
    TEST_ASSERT_EQUAL_UINT(restarts, logged(ALARM_EV_ZWAVE_RADIO_RESTARTED));
}

/* ZW-004 */
static void test_round_trip_startup(void)
{
    round_trip(SC_STARTUP);
}

/* ZW-004 */
static void test_round_trip_line_noise(void)
{
    round_trip(SC_LINE_NOISE);
}

/* ZW-004 */
static void test_round_trip_radio_restart(void)
{
    round_trip(SC_RADIO_RESTART);
}

/* ZW-005, ZW-016 */
static void test_round_trip_check_windows(void)
{
    round_trip(SC_CHECK_WINDOWS);
}

/* ---- the fixtures ---------------------------------------------------------------------------- */

/* ZW-004: the panel's first start against a factory ZAC93, from the recording: the pages show the
 * radio working with the module's facts, no devices; the network's keys made once and bound to
 * the recorded home ID - the scrubbed one, which the replay takes like any other. */
static void test_fixture_startup(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    TEST_ASSERT_TRUE(TRACE.scrubbed);
    replay(&TRACE, false);
    assert_played(SC_FILE[SC_STARTUP]);
    const ui_zw_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_EQUAL_UINT8(1u, st.firmware_major);
    TEST_ASSERT_EQUAL_UINT8(50u, st.firmware_minor);
    TEST_ASSERT_EQUAL_UINT8(7u, st.sdk_major);
    TEST_ASSERT_EQUAL_UINT8(22u, st.sdk_minor);
    TEST_ASSERT_EQUAL_UINT8(1u, st.sdk_patch);
    TEST_ASSERT_FALSE(st.update_advised);
    TEST_ASSERT_TRUE(st.region_known);
    TEST_ASSERT_EQUAL_HEX8(0x01u, st.region); /* USA */
    TEST_ASSERT_EQUAL_UINT8(2u, st.region_count);
    TEST_ASSERT_EQUAL_HEX8(0x09u, st.regions[1]); /* USA Long Range on offer */
    TEST_ASSERT_EQUAL_UINT16(0u, st.devices);
    TEST_ASSERT_EQUAL_HEX32(SCRUBBED_HOME, st.home_id);
    const zw_module_facts_t *f = zw_host_facts(&W.p.net.host);
    TEST_ASSERT_EQUAL_HEX8(0x08u, f->chip_type);
    TEST_ASSERT_EQUAL_STRING("Z-Wave 7.22", f->library);
    TEST_ASSERT_EQUAL_UINT16(1u, f->node_id);
    TEST_ASSERT_EQUAL_UINT16(1u, f->suc_node_id); /* it named itself SUC */
    TEST_ASSERT_TRUE(f->tx_status_report);
    TEST_ASSERT_EQUAL_UINT(1u, W.key_writes);
    TEST_ASSERT_TRUE(W.setup.keys.bound);
    TEST_ASSERT_EQUAL_HEX32(SCRUBBED_HOME, W.setup.keys.home_id);
    TEST_ASSERT_EQUAL_UINT(0u, W.log_n);
    TEST_ASSERT_EQUAL_UINT32(0u, W.replay.remapped); /* its callback IDs were the stack's */
}

/* ZW-004: the start-up through a noisy line - junk the parser discards (text), a frame the byte
 * timeout cut off, an answer damaged on the way (NAKed, sent again): the stack sees only the
 * junk and the good answer, and comes up as without the noise. */
static void test_fixture_line_noise(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_LINE_NOISE]);
    replay(&TRACE, false);
    assert_played(SC_FILE[SC_LINE_NOISE]);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.rx.timeouts);     /* the cut-off frame, dropped */
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.rx.bad_checksum); /* the damaged answer, NAKed */
    TEST_ASSERT_EQUAL_UINT32(3u, W.replay.rx.discarded);    /* the junk */
    TEST_ASSERT_EQUAL_UINT32(3u, zw_host_stats(&W.p.net.host)->text_bytes);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&W.p.net.host)->attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_host_stats(&W.p.net.host)->step_retries);
}

/* ZW-004: the module restarts by itself (its watchdog, SerialAPIStarted reason 03) while the link
 * is up: steps 13-19 again, the radio working after, and "restarted by itself" logged once. */
static void test_fixture_radio_restart(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_RADIO_RESTART]);
    zwt_module_t m;
    zwt_module(&TRACE, &m);
    TEST_ASSERT_EQUAL_UINT(2u, m.started); /* step 12's soft reset, then its watchdog */
    replay(&TRACE, false);
    assert_played(SC_FILE[SC_RADIO_RESTART]);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&W.p.net.host)->restarts);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&W.p.net.host)->link_ups);
    TEST_ASSERT_EQUAL_UINT(1u, W.log_n);
    TEST_ASSERT_EQUAL_UINT8(ALARM_EV_ZWAVE_RADIO_RESTARTED, W.log[0].type);
    TEST_ASSERT_EQUAL_HEX8(0x03u, W.log[0].arg);
}

/* ZW-005, ZW-016: `zwave check` with no device, as panelctl records it from its first step: the
 * radio restarted from the pages (a whole start-up), then the add window - listening with its
 * countdown - stopped, and the remove window the same. The recording's callback IDs were a
 * running panel's; the replay's fresh stack numbers its own and the module's reports follow. */
static void test_fixture_check_windows(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_CHECK_WINDOWS]);
    replay(&TRACE, false);
    assert_played(SC_FILE[SC_CHECK_WINDOWS]);
    TEST_ASSERT_TRUE(W.seen.add_listened);
    TEST_ASSERT_TRUE(W.seen.add_left > 55u && W.seen.add_left <= 60u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_STOPPED, W.seen.add_result);
    TEST_ASSERT_TRUE(W.seen.remove_listened);
    TEST_ASSERT_TRUE(W.seen.remove_left > 55u && W.seen.remove_left <= 60u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_STOPPED, W.seen.remove_result);
    /* IDs may now coincide: own-node SetSUC no longer consumes a callback ID. The
     * shifted-ID test below proves remapping independently of that startup detail. */
    const ui_zw_status_t st = status();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_EQUAL_UINT16(0u, st.devices);
    TEST_ASSERT_EQUAL_UINT(0u, W.log_n);
}

/* Every recording in traces/ (a bench recording copied in joins them): scrubbed, nothing lost, it
 * plays out from its first start-up with every write as recorded, and the pages show what its
 * module said about itself. */
static void test_every_recording_replays(void)
{
    char list[] = ZWZ_TRACE_FILES;
    unsigned count = 0u;
    for (char *name = strtok(list, ","); name != NULL; name = strtok(NULL, ",")) {
        load(ZWZ_TRACE_DIR, name);
        TEST_ASSERT_TRUE_MESSAGE(TRACE.scrubbed, "not scrubbed: run tools/zwtrace_scrub.py on it");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, TRACE.lost, "entries lost: record it again");
        replay(&TRACE, true);
        assert_played(name);
        assert_pages_match_module(name);
        count++;
    }
    TEST_ASSERT_TRUE(count >= (unsigned)SC_COUNT);
}

/* The replay does not depend on the home ID: the fake's own recording (C0FFEE01) and the scrubbed
 * fixture (CAFE0001) play out alike, each panel's keys bound to its own. */
static void test_scrubbed_and_raw_recordings_replay_alike(void)
{
    const size_t n = record(SC_STARTUP);
    TEST_ASSERT_EQUAL_INT(ZWT_OK, zwt_parse(&TRACE, TEXT, n));
    replay(&TRACE, false);
    assert_played("raw");
    const ui_zw_status_t raw = status();
    TEST_ASSERT_EQUAL_HEX32(FAKE_HOME, raw.home_id);
    TEST_ASSERT_EQUAL_HEX32(FAKE_HOME, W.setup.keys.home_id);
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    replay(&TRACE, false);
    assert_played("scrubbed");
    const ui_zw_status_t scrubbed = status();
    TEST_ASSERT_EQUAL_HEX32(SCRUBBED_HOME, scrubbed.home_id);
    TEST_ASSERT_EQUAL_HEX32(SCRUBBED_HOME, W.setup.keys.home_id);
    TEST_ASSERT_EQUAL_UINT8(raw.radio, scrubbed.radio);
    TEST_ASSERT_EQUAL_UINT8(raw.firmware_minor, scrubbed.firmware_minor);
    TEST_ASSERT_EQUAL_UINT8(raw.region, scrubbed.region);
}

/* ---- divergence is reported ------------------------------------------------------------------ */

/* A recording whose SetApplicationNodeInformation lists Security 0 (98) too - as a panel built with
 * S0 would send it - against this stack, which does not: the replay reports the write with both
 * frames in hex and stops; nothing after it is played. */
static void test_a_mismatch_is_reported(void)
{
    load(ZWZ_TRACE_DIR "/bad", "startup_node_info_differs.zwtrace");
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.mismatches);
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "tx #"));
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "the stack wrote\n  010c0003010100055e9f6c568f81"));
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "where the recording has\n  010d0003010100065e9f6c568f98"));
    TEST_ASSERT_FALSE(zwt_replay_done(&W.replay));
    /* stopped there: the stack's retransmissions are not matched, the module says nothing more */
    const uint32_t delivered = W.replay.delivered;
    run_for(5000u);
    TEST_ASSERT_TRUE(W.replay.writes_after > 0u);
    TEST_ASSERT_EQUAL_UINT32(delivered, W.replay.delivered);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.mismatches);
    TEST_ASSERT_NOT_EQUAL(ZW_LINK_UP, zw_host_link(&W.p.net.host));
}

/* The replay works with the stack's callback IDs: the recording's moved on by 40 (a panel that ran
 * a while) - add/remove requests and their callbacks - still plays out, the callback carrying the
 * stack's ID. */
static void test_callback_ids_follow_the_stack(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_CHECK_WINDOWS]);
    unsigned moved = 0u;
    for (size_t i = 0; i < TRACE.n; i++) {
        zwt_entry_t *e = &TRACE.e[i];
        uint8_t *b = &TRACE.bytes[e->off];
        size_t off = 0u;
        if (!zwt_frame_ok(b, e->len) || b[2] != ZW_TYPE_REQUEST) {
            continue;
        }
        if (e->kind == ZWT_TX &&
            zw_api_callback_id_offset(b[3], &b[4], (size_t)e->len - 5u, &off) &&
            b[4u + off] != 0u) {
            b[4u + off] = (uint8_t)((unsigned)b[4u + off] + 0x40u);
        } else if (e->kind == ZWT_RX &&
                   (b[3] == ZW_FUNC_ADD_NODE_TO_NETWORK || b[3] == ZW_FUNC_REMOVE_NODE_FROM_NETWORK)) {
            b[4] = (uint8_t)((unsigned)b[4] + 0x40u);
        } else {
            continue;
        }
        b[(size_t)e->len - 1u] = zw_frame_checksum(&b[1], b[1]);
        moved++;
    }
    TEST_ASSERT_TRUE(moved >= 4u);
    replay(&TRACE, true);
    assert_played("callback IDs moved");
    TEST_ASSERT_TRUE(W.replay.remapped >= 2u);
    TEST_ASSERT_EQUAL_UINT16(1u, zw_host_facts(&W.p.net.host)->suc_node_id);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);
}

/* Either way round: a recording that ends before the stack does (its next write is "after the
 * recording's last"), and one with a write the stack never makes (GetBackgroundRSSI, phase 2). */
static void test_a_divergence_is_reported_either_way(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    TRACE.n = find(&TRACE, 0u, ZWT_TX, ZW_FUNC_SET_SERIAL_API_TIMEOUTS); /* cut before step 18 */
    TEST_ASSERT_TRUE(TRACE.n > 0u);
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.mismatches);
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "after the recording's last write"));
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "01050006a00f53"));

    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    static const uint8_t rssi[] = {0x01, 0x03, 0x00, 0x3B, 0xC7};
    TEST_ASSERT_TRUE(TRACE.n < ZWT_ENTRIES_MAX && TRACE.used + sizeof rssi <= ZWT_BYTES_MAX);
    zwt_entry_t *last = &TRACE.e[TRACE.n - 1u];
    zwt_entry_t *e = &TRACE.e[TRACE.n++];
    *e = *last;
    e->kind = ZWT_TX;
    e->seq = last->seq + 1u;
    e->t = last->t + 1000u;
    e->off = (uint32_t)TRACE.used;
    e->len = sizeof rssi;
    memcpy(&TRACE.bytes[TRACE.used], rssi, sizeof rssi);
    TRACE.used += sizeof rssi;
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.mismatches);
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "the stack never made the recorded write"));
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "0103003bc7"));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio); /* everything before it played */

    /* a damaged fixture: a frame the panel ACKed whose checksum no longer fits */
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    const size_t v = find(&TRACE, 0u, ZWT_RX, ZW_FUNC_GET_CONTROLLER_VERSION);
    TEST_ASSERT_TRUE(v < TRACE.n);
    TRACE.bytes[TRACE.e[v].off + 6u] ^= 0x20u; /* "Z-Wave" becomes "Z-wave" */
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.mismatches);
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "the recording ACKed this frame"));
}

/* Puts entries into the loaded trace at `at` (they take its later entries' places). */
static void insert(size_t at, const zwt_entry_t *entries, size_t n)
{
    TEST_ASSERT_TRUE(TRACE.n + n <= ZWT_ENTRIES_MAX);
    memmove(&TRACE.e[at + n], &TRACE.e[at], (TRACE.n - at) * sizeof TRACE.e[0]);
    memcpy(&TRACE.e[at], entries, n * sizeof entries[0]);
    TRACE.n += n;
}

/* The module's entries keep their recorded gaps: before the first write, after the recording's
 * start (a SerialAPIStarted the reset line brought, then bootloader-less text); after a write,
 * after the moment the stack made it (the module's ACK 5 ms, its answer 50 ms after the stack's
 * GetSerialApiCapabilities, which comes 500 ms after its NAK here too). */
static void test_the_module_keeps_its_recorded_timing(void)
{
    static const char text[] =
        "zwtrace 1\n"
        "1 1000 rx 010f000a0000010201055e9f6c568f0089\n"
        "2 1000 ctx 06\n"
        "3 1300 text 4142\n"
        "4 1400 ctx 15\n"
        "5 1900 tx 01030007fb\n"
        "6 1905 crx 06\n"
        "7 1950 rx 012b01070132027a00040611fe87348040200014e30739800700008000000000800000000000000f0000000066\n"
        "8 1950 ctx 06\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_parse(&TRACE, text, sizeof text - 1u), TRACE.error);
    static const zwt_host_t host = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
    world_reset(true);
    zwt_replay_init(&W.replay, &TRACE, &host, REPLAY_T0);
    boot(REPLAY_T0); /* its NAK at once: the recording's comes 400 ms after its start */
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.delivered);
    run(REPLAY_T0 + 299u);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.delivered);
    run(REPLAY_T0 + 300u);
    TEST_ASSERT_EQUAL_UINT32(2u, W.replay.delivered);
    run(REPLAY_T0 + 504u);
    TEST_ASSERT_EQUAL_UINT32(2u, W.replay.matched); /* the NAK, and 07 at 500 */
    TEST_ASSERT_EQUAL_UINT32(2u, W.replay.delivered);
    run(REPLAY_T0 + 505u);
    TEST_ASSERT_EQUAL_UINT32(3u, W.replay.delivered);
    run(REPLAY_T0 + 549u);
    TEST_ASSERT_EQUAL_UINT32(3u, W.replay.delivered);
    run(REPLAY_T0 + 550u);
    TEST_ASSERT_EQUAL_UINT32(4u, W.replay.delivered);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&W.p.net.host)->text_bytes);
    /* the recording ends here; the stack's next command is past it */
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.mismatches);
    TEST_ASSERT_NOT_NULL(strstr(W.replay.mismatch, "after the recording's last write, the stack "
                                                   "wrote\n  01030002fe"));
}

/* A file's text into TEXT at `at`; its length. */
static size_t read_text(const char *name, size_t at)
{
    char path[512];
    const int w = snprintf(path, sizeof path, "%s/%s", ZWZ_TRACE_DIR, name);
    TEST_ASSERT_TRUE(w > 0 && (size_t)w < sizeof path);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    const size_t n = fread(&TEXT[at], 1u, sizeof TEXT - at - 1u, f);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    TEXT[at + n] = '\0';
    return n;
}

/* The panel rebooted while it recorded (panelctl writes "# panel restarted" where the numbers go
 * back to 1): the replay restarts its stack there with what it had stored, and plays on - the
 * second start-up finds the network the stored keys belong to and makes no new ones. */
static void test_a_panel_restart_in_the_recording(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    replay(&TRACE, false);
    assert_played("once");
    const uint32_t writes = W.replay.matched;
    const uint32_t entries = W.replay.delivered;

    size_t n = read_text(SC_FILE[SC_STARTUP], 0u);
    static const char mark[] = "# panel restarted\n";
    TEST_ASSERT_TRUE(n + sizeof mark < sizeof TEXT / 2u);
    memcpy(&TEXT[n], mark, sizeof mark - 1u);
    n += sizeof mark - 1u;
    const size_t second = n;
    n += read_text(SC_FILE[SC_STARTUP], n); /* the same recording again after the reboot... */
    TEXT[second] = '#';                     /* ...its "zwtrace 1" line made a comment */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_parse(&TRACE, TEXT, n), TRACE.error);
    TEST_ASSERT_EQUAL_UINT32(1u, TRACE.restarts);
    replay(&TRACE, false);
    assert_played("restarted");
    TEST_ASSERT_FALSE(zwt_replay_at_restart(&W.replay)); /* the last segment played */
    TEST_ASSERT_EQUAL_UINT32(2u * writes, W.replay.matched); /* both start-ups, all of them */
    TEST_ASSERT_EQUAL_UINT32(2u * entries, W.replay.delivered);
    TEST_ASSERT_EQUAL_UINT(1u, W.key_writes);
    TEST_ASSERT_EQUAL_HEX32(SCRUBBED_HOME, W.setup.keys.home_id);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&W.p.net.host)->link_ups); /* the new start's */
}

/* A recording that begins in the middle of the panel's life (`zwave trace` started any time) is
 * played from its first start-up: what came before - here the end of an earlier exchange - is
 * passed over. Without a start-up in it, the replay says so. */
static void test_a_recording_is_played_from_its_first_startup(void)
{
    size_t n = read_text(SC_FILE[SC_STARTUP], 0u);
    char *first = strstr(TEXT, "\n1 1000 ctx 15\n");
    TEST_ASSERT_NOT_NULL(first);
    static const char before[] = "\n7 400 tx 01030020dc\n8 401 crx 06\n"
                                 "9 402 rx 01080120cafe000101e2\n10 402 ctx 06";
    const size_t at = (size_t)(first - TEXT);
    TEST_ASSERT_TRUE(n + sizeof before < sizeof TEXT);
    memmove(&TEXT[at + sizeof before - 1u], &TEXT[at], n - at + 1u);
    memcpy(&TEXT[at], before, sizeof before - 1u);
    n += sizeof before - 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_parse(&TRACE, TEXT, n), TRACE.error);
    TEST_ASSERT_EQUAL_UINT8(ZWT_TX, TRACE.e[0].kind);
    replay(&TRACE, true);
    assert_played("from its start-up");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);

    static const char none[] = "zwtrace 1\n7 400 tx 01030020dc\n8 401 crx 06\n";
    TEST_ASSERT_EQUAL_INT(ZWT_OK, zwt_parse(&TRACE, none, sizeof none - 1u));
    static const zwt_host_t host = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
    zwt_replay_init(&W.replay, &TRACE, &host, 0u);
    TEST_ASSERT_FALSE(zwt_replay_skip_to_startup(&W.replay));
}

/* A host that only notes what it is handed: the callback ID of every frame. */
static struct {
    uint8_t ids[8];
    size_t n;
} HANDED;

static void stub_control(void *ctx, uint64_t t, uint8_t b)
{
    (void)ctx;
    (void)t;
    (void)b;
}

static void stub_frame(void *ctx, uint64_t t, const zw_frame_t *f)
{
    (void)ctx;
    (void)t;
    if (f->payload_len > 0u && HANDED.n < sizeof HANDED.ids) {
        HANDED.ids[HANDED.n++] = f->payload[0];
    }
}

static void stub_poll(void *ctx, uint64_t t)
{
    (void)ctx;
    (void)t;
}

static uint64_t stub_deadline(void *ctx)
{
    (void)ctx;
    return UINT64_MAX;
}

/* Only a callback ID the stack was seen to use for its own is changed: of two Ready reports the
 * one with the recorded request's ID reaches the stack with the stack's, the one with an ID no
 * recorded request had (77) as it was recorded. */
static void test_an_unknown_callback_id_stays_as_recorded(void)
{
    static const char text[] = "zwtrace 1\n"
                               "1 0 tx 0105004ac10273\n"   /* AddNode(any), callback 02 */
                               "2 0 crx 06\n"
                               "3 5 rx 0107004a02010000b1\n" /* Ready, 02 */
                               "4 5 ctx 06\n"
                               "5 6 rx 0107004a77010000c4\n" /* Ready, 77 */
                               "6 6 ctx 06\n";
    static const zwt_host_t host = {NULL, stub_control, stub_frame, NULL, stub_poll, stub_deadline};
    static const uint8_t start[] = {0x01, 0x05, 0x00, 0x4A, 0xC1, 0x09, 0x78}; /* callback 09 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ZWT_OK, zwt_parse(&TRACE, text, sizeof text - 1u), TRACE.error);
    TEST_ASSERT_TRUE(zwt_frame_ok(start, sizeof start));
    TEST_ASSERT_TRUE(zwt_frame_ok(zwt_bytes(&TRACE, &TRACE.e[4]), TRACE.e[4].len));
    memset(&HANDED, 0, sizeof HANDED);
    world_reset(true);
    zwt_replay_init(&W.replay, &TRACE, &host, 100u);
    TEST_ASSERT_TRUE(zwt_replay_write(&W.replay, start, sizeof start));
    zwt_replay_run_until(&W.replay, 200u);
    TEST_ASSERT_TRUE(zwt_replay_done(&W.replay));
    TEST_ASSERT_EQUAL_UINT(2u, HANDED.n);
    TEST_ASSERT_EQUAL_HEX8(0x09u, HANDED.ids[0]);
    TEST_ASSERT_EQUAL_HEX8(0x77u, HANDED.ids[1]);
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.remapped);
}

/* The stack's own control bytes are checked loosely: a different one (the recording's start-up
 * NAK made a CAN), a missing one (the NAK dropped from the recording) or an extra one recorded
 * before a frame is counted in control_diffs, and the recording still plays out. */
static void test_control_bytes_are_checked_loosely(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    TEST_ASSERT_EQUAL_UINT8(ZWT_CTX, TRACE.e[0].kind);
    TRACE.bytes[TRACE.e[0].off] = ZW_CAN;
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(0u, W.replay.mismatches);
    TEST_ASSERT_TRUE(zwt_replay_done(&W.replay));
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.control_diffs);

    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    memmove(&TRACE.e[0], &TRACE.e[1], (TRACE.n - 1u) * sizeof TRACE.e[0]);
    TRACE.n--;
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(0u, W.replay.mismatches);
    TEST_ASSERT_TRUE(zwt_replay_done(&W.replay));
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.control_diffs);

    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    const size_t caps = find(&TRACE, 0u, ZWT_TX, ZW_FUNC_GET_SERIAL_API_CAPABILITIES);
    TEST_ASSERT_TRUE(caps < TRACE.n && TRACE.e[caps - 1u].kind == ZWT_CTX);
    const zwt_entry_t extra = TRACE.e[0]; /* a lone NAK before the first command */
    insert(caps, &extra, 1u);
    replay(&TRACE, false);
    TEST_ASSERT_EQUAL_UINT32(0u, W.replay.mismatches);
    TEST_ASSERT_TRUE(zwt_replay_done(&W.replay));
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.control_diffs);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);
}

/* A good frame the panel NAKed - its zwave task had no room (zw_link.c), so the module sent it
 * again - is NAKed here too and not handed on: the stack hears the answer once, as the panel did. */
static void test_a_frame_the_panel_refused_is_not_handed_on(void)
{
    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    replay(&TRACE, false);
    assert_played("as recorded");
    const uint32_t unclaimed = zw_host_transport_stats(&W.p.net.host)->unclaimed;
    TEST_ASSERT_EQUAL_UINT32(0u, W.replay.honored_naks);

    load(ZWZ_TRACE_DIR, SC_FILE[SC_STARTUP]);
    const size_t v = find(&TRACE, 0u, ZWT_RX, ZW_FUNC_GET_CONTROLLER_VERSION);
    TEST_ASSERT_TRUE(v + 1u < TRACE.n && TRACE.e[v + 1u].kind == ZWT_CTX);
    zwt_entry_t nak = TRACE.e[v + 1u]; /* the answer NAKed... */
    nak.off = (uint32_t)TRACE.used;
    TRACE.bytes[TRACE.used++] = ZW_NAK;
    const zwt_entry_t again[2] = {nak, TRACE.e[v]}; /* ...and sent again */
    insert(v + 1u, again, 2u);
    replay(&TRACE, false);
    assert_played("NAKed once");
    TEST_ASSERT_EQUAL_UINT32(1u, W.replay.honored_naks);
    TEST_ASSERT_EQUAL_UINT32(unclaimed, zw_host_transport_stats(&W.p.net.host)->unclaimed);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, status().radio);
}

/* ---- fixtures on request --------------------------------------------------------------------- */

/* ZWZ_RECORD_DIR=<folder> build/host/test_zwz_replay writes the scenarios' raw recordings there,
 * to be scrubbed into traces/ (traces/README.md, "The fake's recordings"). */
static void test_record_the_fixtures_when_asked(void)
{
    const char *dir = getenv("ZWZ_RECORD_DIR");
    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    for (int sc = 0; sc < (int)SC_COUNT; sc++) {
        const size_t n = record((scenario_t)sc);
        char path[512];
        const int w = snprintf(path, sizeof path, "%s/%s", dir, SC_FILE[sc]);
        TEST_ASSERT_TRUE(w > 0 && (size_t)w < sizeof path);
        FILE *f = fopen(path, "wb");
        TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
        TEST_ASSERT_EQUAL_UINT(n, fwrite(TEXT, 1u, n, f));
        TEST_ASSERT_EQUAL_INT(0, fclose(f));
        printf("recorded %s\n", path);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_reader_takes_a_recording);
    RUN_TEST(test_the_reader_refuses_what_it_cannot_read);
    RUN_TEST(test_the_recorder_writes_what_the_link_saw);
    RUN_TEST(test_round_trip_startup);
    RUN_TEST(test_round_trip_line_noise);
    RUN_TEST(test_round_trip_radio_restart);
    RUN_TEST(test_round_trip_check_windows);
    RUN_TEST(test_fixture_startup);
    RUN_TEST(test_fixture_line_noise);
    RUN_TEST(test_fixture_radio_restart);
    RUN_TEST(test_fixture_check_windows);
    RUN_TEST(test_every_recording_replays);
    RUN_TEST(test_scrubbed_and_raw_recordings_replay_alike);
    RUN_TEST(test_a_mismatch_is_reported);
    RUN_TEST(test_callback_ids_follow_the_stack);
    RUN_TEST(test_a_divergence_is_reported_either_way);
    RUN_TEST(test_the_module_keeps_its_recorded_timing);
    RUN_TEST(test_a_panel_restart_in_the_recording);
    RUN_TEST(test_a_recording_is_played_from_its_first_startup);
    RUN_TEST(test_an_unknown_callback_id_stays_as_recorded);
    RUN_TEST(test_control_bytes_are_checked_loosely);
    RUN_TEST(test_a_frame_the_panel_refused_is_not_handed_on);
    RUN_TEST(test_record_the_fixtures_when_asked);
    return UNITY_END();
}
