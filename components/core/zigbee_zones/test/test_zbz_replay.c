/*
 * test_zbz_replay.c - the replays (docs/features/20 testing.md §7, §8; ZB-003, ZB-005, ZB-011,
 * ZB-012, ZB-013, ZB-014, ZB-021, ZB-022, ZB-023, ZB-024): short scripts, each one device's story,
 * run through the whole panel side as the firmware wires it - zbz_panel, zone_hub's zsrc_zigbee and
 * a real zone_hub, over the fake radio's real ZCL and ZDO bytes - with virtual time. Each asserts
 * what the installer sees (the pages' views and the progress), what the zones get (the hub's zone
 * events), what is stored (the take calls, read back) and what is logged.
 *
 * The scripts are made by hand from the device signatures and the fake radio's fault knobs until
 * bench captures exist (`zigbee trace`, ZB-M6): then each capture becomes one more script here.
 * The clean, lossy and sleepy profiles of §7 are test_clean_join_to_ready,
 * test_lossy_profile_retries_then_stops and test_sleepy_profile_waits_for_its_wake.
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "unity.h"
#include "zbz_fake_radio.h"
#include "zigbee_zones/zbz_panel.h"
#include "zone_hub/zone_hub.h"

/* ---- the panel side, wired as zigbee_service wires it -------------------------------------------- */

static zbf_radio_t F;
static zsrc_zb_t Z;
static zsrc_zb_radio_t H;
static zbz_panel_t P;
static zone_hub_t HUB;
static sim_t SIM;
static bool g_engine;   /* the zones' events go on to the real engine (alarm_sim) */
static uint64_t g_now;

/* What the zones got. */
typedef struct {
    uint16_t zone;
    uint8_t ev;
    uint64_t at;
} zev_t;
#define ZEV_MAX 1024u
static zev_t g_zev[ZEV_MAX];
static size_t g_nzev;
static int g_src_trouble; /* the hub's source trouble: -1 never told, 0 off, 1 on */

/* What was logged. */
#define LOG_MAX 64u
static zbz_log_t g_log[LOG_MAX];
static size_t g_nlog;

/* The owner's side: the alarm task's queue, the bell trouble, the keypads' Default Responses. */
#define FQ_MAX 8u
static zba_post_t FQ[FQ_MAX];
static unsigned FQN;
static int g_bell_trouble;
static uint32_t g_taken;

static void hub_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    if (g_nzev < ZEV_MAX) {
        g_zev[g_nzev++] = (zev_t){zone, (uint8_t)ev, g_now};
    }
    if (g_engine) {
        sim_zev(&SIM, zone, ev);
    }
}

static void hub_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    if (kind == (uint8_t)ZSRC_ZIGBEE) {
        g_src_trouble = on ? 1 : 0;
    }
}

static void s_input(void *ctx, const zsrc_address_t *a, const char *st, int32_t v, uint64_t now)
{
    (void)ctx;
    (void)zone_hub_input(&HUB, a, st, v, now);
}
static void s_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    (void)zone_hub_event(&HUB, a, ev, now);
}
static void s_reach(void *ctx, const zsrc_address_t *a, bool ok, uint64_t now)
{
    (void)ctx;
    (void)zone_hub_reachable(&HUB, a, ok, now);
}
static void s_link(void *ctx, uint8_t kind, bool up_now, uint64_t now)
{
    (void)ctx;
    zone_hub_link(&HUB, kind, up_now, now);
}
static void s_link_now(void *ctx, uint8_t kind, uint64_t now)
{
    (void)ctx;
    zone_hub_link_now(&HUB, kind, now);
}

static void f_report(void *ctx, const zsrc_zb_report_t *r, uint64_t now)
{
    (void)ctx;
    zbz_panel_on_report(&P, now, r);
}

static bool p_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    return zone_hub_get(&HUB, zone, out);
}
static bool p_rebind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    return zone_hub_bind(&HUB, zone, b, g_now) == ZONE_HUB_OK;
}
static uint32_t p_wall(void *ctx)
{
    (void)ctx;
    return 1790000000u;
}
static uint32_t p_taken(void *ctx)
{
    (void)ctx;
    return g_taken;
}
static bool p_post(void *ctx, const zba_post_t *post)
{
    (void)ctx;
    if (FQN >= FQ_MAX) {
        return false;
    }
    FQ[FQN++] = *post;
    return true;
}
static void w_bell(void *ctx, bool active)
{
    (void)ctx;
    g_bell_trouble = active ? 1 : 0;
}
static bool a_default_rsp(void *ctx, uint64_t ieee, uint8_t ep, uint8_t tsn, uint8_t command,
                          uint8_t status, uint32_t tag)
{
    (void)ctx;
    (void)ieee;
    (void)ep;
    (void)tsn;
    (void)command;
    (void)status;
    (void)tag;
    return true;
}

/* ---- the store ----------------------------------------------------------------------------------- */

typedef struct {
    bool used;
    char key[ZBZ_KEY_LEN];
    uint8_t buf[ZBZ_RECORD_MAX];
    size_t len;
} kv_t;
static kv_t g_kv[ZBZ_DEV_MAX + 8u];

static kv_t *kv_find(const char *key)
{
    for (size_t i = 0; i < sizeof(g_kv) / sizeof(g_kv[0]); i++) {
        if (g_kv[i].used && strcmp(g_kv[i].key, key) == 0) {
            return &g_kv[i];
        }
    }
    return NULL;
}
static void kv_put(const char *key, const uint8_t *buf, size_t len)
{
    kv_t *e = kv_find(key);
    for (size_t i = 0; e == NULL && i < sizeof(g_kv) / sizeof(g_kv[0]); i++) {
        if (!g_kv[i].used) {
            e = &g_kv[i];
        }
    }
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(len <= sizeof(e->buf));
    e->used = true;
    snprintf(e->key, sizeof(e->key), "%s", key);
    memcpy(e->buf, buf, len);
    e->len = len;
}
static void kv_erase(const char *key)
{
    kv_t *e = kv_find(key);
    if (e != NULL) {
        e->used = false;
    }
}
static size_t st_read(void *ctx, const char *key, uint8_t *buf, size_t cap)
{
    (void)ctx;
    const kv_t *e = kv_find(key);
    if (e == NULL) {
        return 0u;
    }
    if (e->len <= cap) {
        memcpy(buf, e->buf, e->len);
    }
    return e->len;
}
static const zbz_store_port_t STORE = {.ctx = NULL, .read = st_read};

/* The owner's write-back after unlocking: every take, into the store. */
static void write_back(void)
{
    char key[ZBZ_KEY_LEN];
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    size_t len = 0u;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&P, key, buf, sizeof(buf), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            kv_put(key, buf, len);
        } else {
            kv_erase(key);
        }
    }
    len = zbz_panel_take_codes(&P, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBZ_KEY_CODES, buf, len);
    }
    len = zbz_panel_take_cfg(&P, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBZ_CFG_KEY, buf, len);
    }
    len = zbz_panel_take_wd(&P, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBW_KEY, buf, len);
    }
    len = zbz_panel_take_ace(&P, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBA_KEY, buf, len);
    }
}

/* ---- set-up, time ---------------------------------------------------------------------------------- */

static void radio(uint8_t state)
{
    zbz_radio_info_t info;
    memset(&info, 0, sizeof(info));
    info.state = state;
    zbz_panel_radio(&P, g_now, &info);
}

static const zbz_panel_port_t PORT = {.ctx = NULL,
                                      .binding = p_binding,
                                      .rebind = p_rebind,
                                      .wall_clock = p_wall,
                                      .changed = NULL,
                                      .devices_taken = p_taken,
                                      .ace_post = p_post};

/* The panel's start-up: the hub, the radio side and zbz_panel; `engine`: zone events reach the real
 * engine, configured as `cfg` (NULL: sim_config_basic). The store is kept (a restart reads it). */
static void start_with(bool engine, const alarm_config_t *cfg)
{
    g_engine = engine;
    if (engine) {
        alarm_config_t c;
        if (cfg != NULL) {
            c = *cfg;
        } else {
            sim_config_basic(&c);
        }
        sim_start(&SIM, &c, NULL);
    }
    g_nzev = 0u;
    g_src_trouble = -1;
    g_nlog = 0u;
    FQN = 0u;
    g_bell_trouble = -1;
    g_taken = 1u << 0; /* the panel's own keypad 1 */
    const zone_hub_port_t hp = {
        .ctx = NULL, .zone_event = hub_event, .changed = NULL, .source_trouble = hub_trouble};
    zone_hub_init(&HUB, &hp);
    static const zbf_sink_t sink = {.ctx = NULL, .report = f_report, .attr = NULL};
    zbf_init(&F, &Z, &sink, g_now);
    H = zbf_hooks(&F);
    const zsrc_sink_t zs = {.ctx = NULL,
                            .input = s_input,
                            .event = s_event,
                            .reachable = s_reach,
                            .link = s_link,
                            .link_now = s_link_now};
    zsrc_zb_init(&Z, &H, &zs);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&HUB, zsrc_zb_ops(), &Z, g_now));
    zone_hub_start(&HUB, g_now);
    const zbw_port_t wd = {.ctx = NULL, .send = NULL, .bell_trouble = w_bell, .checkin = NULL};
    const zba_port_t ace = {.ctx = NULL, .send = NULL, .default_rsp = a_default_rsp};
    zbz_panel_init(&P, &PORT, &Z, &H, &wd, &ace);
    (void)zbz_panel_load(&P, &STORE);
    zbf_up(&F);
    radio(UI_ZB_RADIO_WORKING);
}

static void start(bool engine)
{
    start_with(engine, NULL);
}

void setUp(void)
{
    memset(g_kv, 0, sizeof(g_kv));
    g_now = 1000u;
    start(false);
}

void tearDown(void) {}

static uint64_t earliest(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

/* Time passes: the air, the panel's deadlines, the hub and the source, the engine - each at its own
 * time. */
static void run(uint64_t ms)
{
    const uint64_t end = g_now + ms;
    for (unsigned guard = 0; guard < 3000000u; guard++) {
        zone_hub_tick(&HUB, g_now);
        zbz_panel_poll(&P, g_now);
        if (g_now >= end) {
            return;
        }
        uint64_t t = zbf_next_due(&F);
        t = earliest(t, zbz_panel_next_deadline(&P));
        t = earliest(t, zone_hub_next_deadline(&HUB, g_now));
        t = t <= g_now ? g_now + 1u : t;
        t = t < end ? t : end;
        if (g_engine) {
            sim_advance(&SIM, (alarm_ms_t)(t - g_now));
        }
        g_now = t;
        zbf_advance(&F, g_now);
    }
    TEST_FAIL_MESSAGE("time stood still");
}

/* ---- what the installer sees -------------------------------------------------------------------- */

static ui_zb_req_result_t ask(uint8_t kind, uint16_t id)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = 1u;
    req.id = id;
    return zbz_panel_request(&P, g_now, &req, true);
}

static ui_zb_progress_t progress(void)
{
    ui_zb_progress_t g;
    zbz_panel_progress(&P, g_now, &g);
    return g;
}

static ui_zb_device_page_t page(uint16_t id)
{
    ui_zb_device_page_t pg;
    TEST_ASSERT_TRUE_MESSAGE(zbz_panel_device(&P, g_now, id, &pg), "no such device");
    return pg;
}

static ui_zb_status_t status(void)
{
    ui_zb_status_t s;
    zbz_panel_status(&P, g_now, &s);
    return s;
}

static const zbz_device_t *rec(uint64_t ieee)
{
    return zbz_reg_find(&P.reg, ieee);
}

static uint16_t id_of(const zbf_dev_t *d)
{
    const zbz_device_t *r = rec(d->ieee);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "not in the registry");
    return r->id;
}

/* ---- what is logged ------------------------------------------------------------------------------ */

static void drain_log(void)
{
    zbz_log_t e;
    while (zbz_panel_take_log(&P, &e)) {
        if (g_nlog < LOG_MAX) {
            g_log[g_nlog++] = e;
        }
    }
}

static unsigned logged(uint8_t type)
{
    drain_log();
    unsigned n = 0u;
    for (size_t i = 0; i < g_nlog; i++) {
        n += g_log[i].type == type ? 1u : 0u;
    }
    return n;
}

static const zbz_log_t *last_logged(uint8_t type)
{
    drain_log();
    for (size_t i = g_nlog; i > 0u; i--) {
        if (g_log[i - 1u].type == type) {
            return &g_log[i - 1u];
        }
    }
    return NULL;
}

/* ---- what the zones get ------------------------------------------------------------------------- */

static unsigned zone_events(uint16_t zone, uint8_t ev)
{
    unsigned n = 0u;
    for (size_t i = 0; i < g_nzev; i++) {
        n += (g_zev[i].zone == zone && g_zev[i].ev == ev) ? 1u : 0u;
    }
    return n;
}

static const zev_t *first_zone_event(uint16_t zone, uint8_t ev)
{
    for (size_t i = 0; i < g_nzev; i++) {
        if (g_zev[i].zone == zone && g_zev[i].ev == ev) {
            return &g_zev[i];
        }
    }
    return NULL;
}

static void bind(uint16_t zone, uint64_t ieee, uint8_t ep)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_zb_address(ieee, ep);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&HUB, zone, &b, g_now));
}

/* ---- what went over the air ---------------------------------------------------------------------- */

/* A cluster's own command for tx_count (a global one is its plain number). */
#define CS(cmd) ((uint16_t)(0x100u | (cmd)))

/* A mark in the fake's frame log: the count of every frame logged so far. The log keeps only the
 * newest ZBF_LOG_MAX frames, so an index into it taken once it is full never grows; a mark doesn't. */
static size_t mark(void)
{
    return (size_t)F.log_total;
}

/* The log index of the first frame after mark `m`; fails when the log has already dropped one of
 * those frames (the counts would silently come out short). */
static size_t log_from(size_t m)
{
    const size_t first = (size_t)F.log_total - zbf_log_count(&F);
    TEST_ASSERT_TRUE_MESSAGE(m >= first, "the fake's frame log dropped frames after the mark");
    return m - first;
}

/* Is `f` the panel's ZCL frame of `cluster` with `command` (CS() for a cluster command)? */
static bool zcl_is(const zbf_frame_t *f, uint16_t cluster, uint16_t command)
{
    if (f->rx || f->kind != ZBF_FRAME_ZCL || f->cluster != cluster || f->len < 3u) {
        return false;
    }
    const size_t at = (f->bytes[0] & ZBZ_ZCL_FC_MANUFACTURER) != 0u ? 4u : 2u;
    const uint8_t type = (uint8_t)(f->bytes[0] & ZBZ_ZCL_FC_TYPE_MASK);
    const uint8_t want = (command & 0x100u) != 0u ? ZBZ_ZCL_FC_CLUSTER : ZBZ_ZCL_FC_GLOBAL;
    return f->len > at && type == want && f->bytes[at] == (uint8_t)(command & 0xFFu);
}

/* The panel's frames to `ieee` of `kind` and `cluster` (ZCL: that command when `command` isn't
 * 0xFFFF; CS() for a cluster command) since mark `from`. */
static unsigned tx_count(uint64_t ieee, uint8_t kind, uint16_t cluster, uint16_t command,
                         size_t from)
{
    unsigned n = 0u;
    for (size_t i = log_from(from); i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->rx || f->ieee != ieee || f->kind != kind || f->cluster != cluster) {
            continue;
        }
        if (kind == (uint8_t)ZBF_FRAME_ZCL && command != 0xFFFFu && !zcl_is(f, cluster, command)) {
            continue;
        }
        n++;
    }
    return n;
}

/* ---- devices ------------------------------------------------------------------------------------- */

/* A signature's device made a mains-powered router (it listens all the time): the lossy profile's
 * device, whose steps time out instead of waiting for a wake window. */
static void make_router(zbf_dev_t *d)
{
    d->node.logical_type = ZSRC_ZB_LOGICAL_ROUTER;
    d->node.mac_caps = (uint8_t)(ZSRC_ZB_MAC_FFD | ZSRC_ZB_MAC_MAINS | ZSRC_ZB_MAC_RX_ON_IDLE |
                                 ZSRC_ZB_MAC_ALLOC_ADDR);
    d->power_source = 1u;
    d->battery_pct = ZSRC_ZB_BATTERY_UNKNOWN;
    zbf_set_sleepy(d, false, ZBF_WAKE_MS);
}

/* The add page: open the window, the device joins. */
static void open_and_join(zbf_dev_t *d)
{
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    run(100u);
    TEST_ASSERT_TRUE(zbf_join(&F, d));
}

/* Its setup to its end, woken (a check-in, as its button would) whenever it waits for that. */
static void until_set_up(zbf_dev_t *d)
{
    run(100u); /* its join or rejoin reaches the panel */
    for (int guard = 0; guard < 2000; guard++) {
        const zbz_device_t *r = rec(d->ieee);
        if (r != NULL && r->setup != ZBZ_SETUP_RUNNING && r->setup != ZBZ_SETUP_WAKE &&
            r->setup != ZBZ_SETUP_NEW) {
            return;
        }
        if (r != NULL && r->setup == ZBZ_SETUP_WAKE) {
            (void)zbf_check_in(&F, d);
        }
        run(1000u);
    }
    TEST_FAIL_MESSAGE("its setup never ended");
}

/* The window closed and the page dismissed. */
static void close_add(void)
{
    if (progress().step != UI_ZB_STEP_DONE) {
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u));
    }
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
}

/* Added and set up, the add page closed. */
static zbf_dev_t *added(const char *name, bool router)
{
    zbf_dev_t *d = zbf_add(&F, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, name);
    if (router) {
        make_router(d);
    }
    open_and_join(d);
    until_set_up(d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZBZ_SETUP_DONE, rec(d->ieee)->setup, name);
    close_add();
    return d;
}

static void every_step(zbf_dev_t *d, const zbf_fault_t *f)
{
    for (uint8_t s = (uint8_t)ZSRC_ZB_STEP_NODE_DESC; s < (uint8_t)ZSRC_ZB_STEP_DONE; s++) {
        zbf_fault(d, s, f);
    }
}

/* ---- the engine's news, for sirens and keypads ------------------------------------------------- */

static void news(void)
{
    const alarm_partition_status_t st = sim_status(&SIM, 1);
    zba_view_t v;
    memset(&v, 0, sizeof(v));
    v.state = (uint8_t)st.state;
    v.level = (uint8_t)st.level;
    v.ready = st.ready;
    v.delay_s = (uint16_t)((st.delay_remaining_ms + 999u) / 1000u);
    v.at = g_now;
    zbz_panel_partition(&P, g_now, 1u, &v);
    zbz_panel_output(&P, g_now, 1u, ALARM_OUT_BELL, sim_output(&SIM, ALARM_OUT_BELL, 1u));
}

static alarm_result_t alarm_task(void)
{
    alarm_result_t last = ALARM_OK;
    for (unsigned i = 0; i < FQN; i++) {
        zba_post_t *q = &FQ[i];
        alarm_cmd_result_t res;
        memset(&res, 0, sizeof(res));
        last = q->user != 0u
                   ? alarm_engine_command_as(&SIM.eng, &q->cmd, &q->source, q->user, SIM.now, &res)
                   : alarm_engine_command(&SIM.eng, &q->cmd, &q->source, SIM.now, &res);
        memset(q->cmd.code, 0, sizeof(q->cmd.code));
        TEST_ASSERT_TRUE(zbz_panel_ace_result(&P, g_now, q->ticket, last, &res));
    }
    FQN = 0u;
    return last;
}

/* ==== §7 the three profiles ======================================================================== */

/* ZB-012, ZB-021 (testing.md §7 "clean"): a Sonoff contact joins during the installer's window with
 * the standard key and is set up to Ready; the add page follows it row by row and ends ADDED when
 * the window closes; the record says how it joined, is stored and reads back; its zone trips. */
static void test_clean_join_to_ready(void)
{
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_ADD, g.op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_LEARN_S, g.seconds_left);
    run(10000u);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_LEARN_S - 10u, progress().seconds_left);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_ZB_LEARN_S - 10u, status().learn_s);

    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    TEST_ASSERT_TRUE(zbf_join(&F, d));
    run(15u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(1u, g.joined_count);
    TEST_ASSERT_TRUE(g.joined[0].step == UI_ZB_STEP_JOINING || g.joined[0].step == UI_ZB_STEP_READING);
    run(5000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[0].step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.joined[0].result);
    TEST_ASSERT_EQUAL_STRING("Door sensor", g.joined[0].model);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step); /* the window stays open */
    run((uint64_t)ZSRC_ZB_LEARN_S * 1000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.result);

    const uint16_t id = id_of(d);
    const ui_zb_device_page_t pg = page(id);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, pg.dev.state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SENSOR, pg.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_JOIN_STANDARD_KEY, pg.dev.join);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_POWER_BATTERY, pg.dev.power);
    TEST_ASSERT_EQUAL_UINT8(pg.dev.setup_total, pg.dev.setup_done);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, pg.dev.device_class);
    TEST_ASSERT_EQUAL_STRING("SONOFF", pg.manufacturer);
    TEST_ASSERT_EQUAL_STRING("SNZB-04PR2", pg.model_id);
    TEST_ASSERT_TRUE(pg.sleepy);
    TEST_ASSERT_EQUAL_UINT32(3600u, pg.check_in_s); /* Poll Control read in its setup */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_DONE, pg.setup_step);
    TEST_ASSERT_EQUAL_UINT8(1u, pg.input_count);
    TEST_ASSERT_EQUAL_UINT16(0u, pg.inputs[0].zone);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_GOOD, pg.dev.signal);
    /* the device itself: its CIE address is the panel's, and it is enrolled with the source's id */
    const zbf_ep_t *e = zbf_ep(d, 1u);
    TEST_ASSERT_EQUAL_UINT64(F.panel_ieee, e->cie);
    TEST_ASSERT_TRUE(e->enrolled);
    TEST_ASSERT_EQUAL_UINT8(zsrc_zb_device(&Z, d->ieee, 1u)->zone_id, e->zone_id);

    /* logged once, as the installer's, with how it joined */
    TEST_ASSERT_EQUAL_UINT(1u, logged(ZBZ_EV_ADDED));
    const zbz_log_t *l = last_logged(ZBZ_EV_ADDED);
    TEST_ASSERT_EQUAL_UINT16(id, l->device);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, l->arg);
    TEST_ASSERT_EQUAL_UINT8(1u, l->user);

    /* its zone trips */
    bind(3u, d->ieee, 1u);
    TEST_ASSERT_TRUE(zbf_trip(&F, d, true));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(3u, ALARM_ZEV_FAULT));
    TEST_ASSERT_TRUE(zbf_trip(&F, d, false));
    run(100u);
    TEST_ASSERT_TRUE(zone_events(3u, ALARM_ZEV_RESTORE) >= 1u);

    /* stored, and after a restart the same device, Ready, joined with the standard key */
    write_back();
    const uint64_t ieee = d->ieee;
    start(false);
    const zbz_device_t *r = rec(ieee);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT16(id, r->id);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, r->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, r->role);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, r->join);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_ZB_ZONE_TYPE_CONTACT, r->ep[0].zone_type);
    run(10u);
    TEST_ASSERT_NOT_NULL(zsrc_zb_device(&Z, ieee, 1u)); /* described from its record */
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(id).dev.state);
}

/* ZB-012 (testing.md §7 "lossy"): each setup step of a listening device dropped once, then twice:
 * it retries and finishes; dropped ZBZ_SETUP_TRIES + 1 times at a step the setup can't do without,
 * it STOPS there - the page says where, the record keeps it across a restart - and "Set it up
 * again" finishes it from that step. */
static void test_lossy_profile_retries_then_stops(void)
{
    for (uint8_t drops = 1u; drops <= 2u; drops++) {
        memset(g_kv, 0, sizeof(g_kv));
        start(false);
        zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
        make_router(d);
        const zbf_fault_t f = {.drop = drops};
        every_step(d, &f);
        const size_t from = mark();
        open_and_join(d);
        until_set_up(d);
        TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
        /* the node descriptor was asked drops + 1 times */
        TEST_ASSERT_EQUAL_UINT(drops + 1u,
                               tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_NODE_DESC, 0xFFFFu, from));
        run((uint64_t)ZSRC_ZB_LEARN_S * 1000u);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, progress().result);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(id_of(d)).dev.state);
    }

    memset(g_kv, 0, sizeof(g_kv));
    start(false);
    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    make_router(d);
    const zbf_fault_t lost = {.drop = (uint8_t)(ZBZ_SETUP_TRIES + 1u)};
    zbf_fault(d, ZSRC_ZB_STEP_ACTIVE_EP, &lost);
    open_and_join(d);
    run(5u * ZBZ_STEP_MS);
    const uint16_t id = id_of(d);
    ui_zb_device_page_t pg = page(id);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_SETUP_STOPPED, pg.dev.state);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_ACTIVE_EP, pg.setup_step);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_ACTIVE_EP - 1u, pg.dev.setup_done);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_SETUP_NOT_FINISHED, g.joined[0].result);
    /* its stop is stored: after a restart it still says where it stopped */
    write_back();
    start(false);
    d = zbf_add(&F, "sonoff_snzb_04pr2"); /* the same device, still in the network */
    make_router(d);
    d->joined = true;
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, rec(d->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_ACTIVE_EP, rec(d->ieee)->setup_step);
    /* "Set it up again": from that step, to Ready */
    const size_t from = mark();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, id));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_SETUP, progress().op);
    run(2000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.result);
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_NODE_DESC, 0xFFFFu, from));
    TEST_ASSERT_TRUE(tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_ACTIVE_EP, 0xFFFFu, from) >= 1u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(id).dev.state);
}

/* ZB-003, ZB-012 (testing.md §7 "lossy": enrollment): a sensor that never asks to enroll and never
 * takes the Enroll Response is sent it ZSRC_ZB_ENROLL_TRIES times by its setup, never less than
 * ZSRC_ZB_ENROLL_GAP_MS apart, and the source chases it after; the setup still ends Ready, and its
 * status reports still reach its zone (fail-safe). */
static void test_lossy_enrollment_gives_up_and_reports_still_work(void)
{
    zbf_dev_t *d = zbf_add(&F, "third_reality_3rds17bz");
    TEST_ASSERT_NOT_NULL(d);
    make_router(d);
    d->auto_enroll = false;
    const zbf_fault_t mute = {.timeout = true};
    zbf_fault(d, ZSRC_ZB_STEP_ENROLL, &mute);
    const size_t from = mark();
    open_and_join(d);
    until_set_up(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
    /* the setup's ZSRC_ZB_ENROLL_TRIES, then the source's own patient enrollment goes on */
    TEST_ASSERT_TRUE(tx_count(d->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                              CS(ZBZ_IAS_ENROLL_RESPONSE), from) >= ZSRC_ZB_ENROLL_TRIES);
    uint64_t prev = 0u;
    for (size_t i = log_from(from); i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->ieee == d->ieee && zcl_is(f, ZSRC_ZB_ZCL_IAS_ZONE, CS(ZBZ_IAS_ENROLL_RESPONSE))) {
            TEST_ASSERT_TRUE(prev == 0u || f->at - prev >= ZSRC_ZB_ENROLL_GAP_MS);
            prev = f->at;
        }
    }
    TEST_ASSERT_FALSE(zbf_ep(d, 1u)->enrolled);
    close_add();
    bind(5u, d->ieee, 1u);
    TEST_ASSERT_TRUE(zbf_trip(&F, d, true));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(5u, ALARM_ZEV_FAULT));
}

/* ZB-005, ZB-012 (testing.md §7 "sleepy", §8): a sleepy device that listens only briefly after its
 * own frames: its steps land only in its wake windows; while it sleeps the page shows WAKE (the
 * row's wake box, the device "Press its button"); each Poll Control check-in releases the next step;
 * left asleep it STOPS after ZBZ_SETUP_MAX_MS, and "Set it up again" with its button finishes. */
static void test_sleepy_profile_waits_for_its_wake(void)
{
    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    zbf_set_sleepy(d, true, 15u); /* its window closes before the panel's next request lands */
    open_and_join(d);
    run(ZBF_TIMEOUT_MS + 1000u);
    const uint16_t id = id_of(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_WAKE, rec(d->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_WAKE_NEEDED, page(id).dev.state);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_TRUE(g.joined[0].asleep);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_READING, g.joined[0].step);
    /* nothing is sent while it sleeps */
    const size_t quiet = mark();
    run(60000u);
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_NODE_DESC, 0xFFFFu, quiet) +
                                   tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_ACTIVE_EP, 0xFFFFu, quiet));
    /* Its button pressed every 5 s: each press wakes it for 15 ms, too short for the request it
     * prompts, which waits at its parent (ZBF_HOLD_MS) and is handed over at the next press - one
     * step a press. The panel asks for no fast polling (devices.md §5): with presses further apart
     * than ZBF_HOLD_MS it would never get there, and STOPS (below). */
    unsigned wakes = 0u;
    uint8_t step = rec(d->ieee)->setup_step;
    unsigned moved = 0u;
    for (int guard = 0; guard < 300 && rec(d->ieee)->setup != ZBZ_SETUP_DONE; guard++) {
        TEST_ASSERT_TRUE(zbf_check_in(&F, d));
        wakes++;
        run(5000u);
        moved += rec(d->ieee)->setup_step != step ? 1u : 0u;
        step = rec(d->ieee)->setup_step;
    }
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
    TEST_ASSERT_TRUE(moved >= 5u);
    TEST_ASSERT_TRUE(wakes >= moved);
    TEST_ASSERT_FALSE(progress().joined[0].asleep);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(id).dev.state);
    close_add();

    /* another, never woken: STOPPED after ZBZ_SETUP_MAX_MS, then set up again with its button */
    zbf_dev_t *s = zbf_add(&F, "centralite_3300");
    zbf_set_sleepy(s, true, 15u);
    open_and_join(s);
    const uint64_t joined = g_now;
    run(ZBZ_SETUP_MAX_MS - 2000u);
    TEST_ASSERT_TRUE(rec(s->ieee)->setup == ZBZ_SETUP_WAKE || rec(s->ieee)->setup == ZBZ_SETUP_RUNNING);
    run(4000u);
    TEST_ASSERT_TRUE(g_now - joined >= ZBZ_SETUP_MAX_MS);
    const uint16_t sid = id_of(s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, rec(s->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_SETUP_STOPPED, page(sid).dev.state);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, sid));
    zbf_set_sleepy(s, true, ZBF_WAKE_MS); /* the installer holds its button */
    (void)zbf_check_in(&F, s);
    until_set_up(s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(s->ieee)->setup);
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, progress().result);
}

/* ==== §7 the edge cases ============================================================================ */

/* ZB-012, ZB-013: a device that leaves in the middle of its setup: the row says it didn't join, the
 * setup stops, its leaving is logged; when it joins again its setup starts over and finishes. */
static void test_device_leaves_mid_setup(void)
{
    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    make_router(d);
    const zbf_fault_t slow = {.delay_ms = 2000u};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, &slow);
    open_and_join(d);
    run(5000u);
    const uint16_t id = id_of(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, rec(d->ieee)->setup);
    zbf_leave(&F, d, false);
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, rec(d->ieee)->setup);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[0].step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_JOIN_FAILED, g.joined[0].result);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step);
    TEST_ASSERT_EQUAL_UINT(1u, logged(ZBZ_EV_LEFT));
    TEST_ASSERT_EQUAL_UINT16(id, last_logged(ZBZ_EV_LEFT)->device);
    run(30000u); /* its late answers change nothing */
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, rec(d->ieee)->setup);
    /* it comes back (a fresh join): set up again, the same record */
    zbf_fault(d, ZSRC_ZB_STEP_NONE, NULL);
    TEST_ASSERT_TRUE(zbf_join(&F, d));
    until_set_up(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT16(id, id_of(d));
}

/* ZB-012 (testing.md §7): a rejoin with a new short address - the record takes the address and
 * keeps everything else (no new setup, nothing logged), and what the panel asks next goes to the
 * new address: "Set it up again" finishes. */
static void test_rejoin_with_a_new_short_address(void)
{
    zbf_dev_t *d = added("sonoff_snzb_04pr2", true);
    const uint16_t id = id_of(d);
    const unsigned adds = logged(ZBZ_EV_ADDED);
    const size_t from = mark();
    TEST_ASSERT_TRUE(zbf_rejoin(&F, d, 0x7A11u));
    run(1000u);
    TEST_ASSERT_EQUAL_HEX16(0x7A11u, rec(d->ieee)->nwk);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_NODE_DESC, 0xFFFFu, from));
    TEST_ASSERT_EQUAL_UINT(adds, logged(ZBZ_EV_ADDED));
    TEST_ASSERT_EQUAL_HEX16(0x7A11u, page(id).nwk);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, id));
    /* enrolled already, it sends no Enroll Request: the unasked responses take their tries */
    run((uint64_t)ZSRC_ZB_ENROLL_TRIES * ZSRC_ZB_ENROLL_GAP_MS + 5000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, progress().result);
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_NODE_DESC, 0xFFFFu, from));
    for (size_t i = log_from(from); i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        TEST_ASSERT_TRUE(f->rx || f->ieee != d->ieee || f->nwk == 0x7A11u);
    }
    write_back();
    zbz_device_t back;
    const kv_t *kv = NULL;
    for (size_t i = 0; i < sizeof(g_kv) / sizeof(g_kv[0]) && kv == NULL; i++) {
        if (g_kv[i].used && g_kv[i].key[0] == 'd' &&
            zbz_dev_decode(&back, g_kv[i].buf, g_kv[i].len) == ZBZ_REG_OK && back.id == id) {
            kv = &g_kv[i];
        }
    }
    TEST_ASSERT_NOT_NULL(kv);
    TEST_ASSERT_EQUAL_HEX16(0x7A11u, back.nwk);
}

/* ZB-007, ZB-012 (testing.md §7): a device that never answers Basic - the step is given up after
 * ZBZ_SETUP_TRIES, its power comes from the power descriptor, and it is Ready by its clusters; and
 * a siren whose Basic gives no manufacturer is named, and handled, by its model's row. */
static void test_basic_never_answers_and_a_model_row_names_it(void)
{
    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    make_router(d);
    const zbf_fault_t mute = {.timeout = true};
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &mute);
    const size_t from = mark();
    open_and_join(d);
    until_set_up(d);
    const zbz_device_t *r = rec(d->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, r->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, r->role);
    TEST_ASSERT_EQUAL_STRING("", r->manufacturer);
    TEST_ASSERT_EQUAL_UINT(ZBZ_SETUP_TRIES,
                           tx_count(d->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_READ_ATTR, from));
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_POWER_DESC, 0xFFFFu, from));
    const ui_zb_device_page_t pg = page(r->id);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, pg.dev.state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_POWER_MAINS, pg.dev.power);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SENSOR, pg.dev.kind);
    close_add();

    zbf_dev_t *s = zbf_add(&F, "tuya_ts0216");
    TEST_ASSERT_NOT_NULL(s);
    memset(s->manufacturer, 0, sizeof(s->manufacturer));
    open_and_join(s);
    until_set_up(s);
    const zbz_device_t *sr = rec(s->ieee);
    TEST_ASSERT_EQUAL_STRING("", sr->manufacturer);
    TEST_ASSERT_EQUAL_STRING("TS0216", sr->model);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SIREN, sr->role);
    TEST_ASSERT_EQUAL_STRING("Siren", page(sr->id).dev.model);
    close_add();
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = sr->id;
    req.siren.partitions = 1u;
    req.siren.burglary = true;
    req.siren.fire = true;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&P, g_now, &req, true));
    const zbw_siren_t *w = zbw_find(&P.wd, sr->id);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE((w->a.model_flags & ZBZ_MODEL_WD_ONE_TONE) != 0u);
}

/* ZB-003, ZB-012 (testing.md §7): several endpoints - Basic is read on the endpoint that has it,
 * wherever it sits in the list ([2, 1] and [4, 1]); the IAS endpoint is the zone's; an endpoint
 * listed without a simple descriptor is dropped; a vendor profile's endpoint is kept last. */
static void test_several_endpoints(void)
{
    static const uint8_t numbers[2] = {2u, 4u};
    for (size_t v = 0; v < 2u; v++) {
        memset(g_kv, 0, sizeof(g_kv));
        start(false);
        zbf_dev_t *d = zbf_add(&F, "ikea_parasoll");
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_EQUAL_UINT8(2u, d->ep_count);
        /* the IAS endpoint first in its list, numbered 2 or 4; endpoint 1 after it */
        zbf_ep_t ias = d->ep[0].sd.endpoint == 2u ? d->ep[0] : d->ep[1];
        zbf_ep_t other = d->ep[0].sd.endpoint == 2u ? d->ep[1] : d->ep[0];
        ias.sd.endpoint = numbers[v];
        d->ep[0] = ias;
        d->ep[1] = other;
        d->listed_only_ep = 3u;
        const size_t from = mark();
        open_and_join(d);
        until_set_up(d);
        const zbz_device_t *r = rec(d->ieee);
        TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, r->setup);
        /* every Basic read went to the endpoint with Basic */
        for (size_t i = log_from(from); i < zbf_log_count(&F); i++) {
            const zbf_frame_t *f = zbf_log_at(&F, i);
            if (!f->rx && f->ieee == d->ieee && f->kind == ZBF_FRAME_ZCL &&
                f->cluster == ZSRC_ZB_ZCL_BASIC) {
                TEST_ASSERT_EQUAL_UINT8(numbers[v], f->endpoint);
            }
        }
        TEST_ASSERT_EQUAL_STRING("IKEA of Sweden", r->manufacturer);
        /* endpoint 3 was asked about and dropped */
        TEST_ASSERT_TRUE(tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_SIMPLE_DESC, 0xFFFFu, from) >= 3u);
        bool has3 = false, has_ias = false;
        for (uint8_t k = 0; k < r->ep_count; k++) {
            has3 = has3 || r->ep[k].endpoint == 3u;
            has_ias = has_ias || (r->ep[k].endpoint == numbers[v] &&
                                  r->ep[k].zone_type == ZSRC_ZB_ZONE_TYPE_CONTACT);
        }
        TEST_ASSERT_FALSE(has3);
        TEST_ASSERT_TRUE(has_ias);
        const ui_zb_device_page_t pg = page(r->id);
        TEST_ASSERT_EQUAL_UINT8(1u, pg.input_count);
        TEST_ASSERT_EQUAL_UINT8(numbers[v], pg.inputs[0].endpoint);
        TEST_ASSERT_TRUE(zbf_ep(d, numbers[v])->enrolled);
        close_add();
        bind(2u, d->ieee, numbers[v]);
        TEST_ASSERT_TRUE(zbf_trip(&F, d, true));
        run(100u);
        TEST_ASSERT_EQUAL_UINT(1u, zone_events(2u, ALARM_ZEV_FAULT));
    }

    /* a motion sensor with a second endpoint on a vendor profile: kept, last, never an input */
    zbf_dev_t *m = added("centralite_3326l", false);
    const zbz_device_t *r = rec(m->ieee);
    TEST_ASSERT_EQUAL_UINT8(2u, r->ep_count);
    TEST_ASSERT_EQUAL_UINT8(1u, r->ep[0].endpoint);
    TEST_ASSERT_EQUAL_HEX16(0x0104u, r->ep[0].profile);
    TEST_ASSERT_EQUAL_UINT8(2u, r->ep[1].endpoint);
    TEST_ASSERT_TRUE(r->ep[1].profile != 0x0104u);
    TEST_ASSERT_EQUAL_UINT8(1u, page(r->id).input_count);
}

/* ZB-012, ZB-014, ZB-023: a Default Response with a failure status - Identify answered FAILURE ends
 * FAILED; a siren's "Test it" answered FAILURE ends FAILED on the page and in its last test. */
static void test_default_response_failure(void)
{
    zbf_dev_t *d = added("sonoff_snzb_04pr2", true);
    const uint16_t id = id_of(d);
    const zbf_fault_t fail = {.status = ZBZ_ZCL_FAILURE};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, &fail);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_IDENTIFY, id));
    run(500u);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_IDENTIFY, g.op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, g.result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    zbf_fault(d, ZSRC_ZB_STEP_NONE, NULL);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_IDENTIFY, id));
    run(500u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));

    start(true);
    zbf_dev_t *s = added("frient_sirzb_111", false);
    const uint16_t sid = id_of(s);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = sid;
    req.siren.partitions = 1u;
    req.siren.burglary = true;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&P, g_now, &req, true));
    news();
    run(1000u);
    zbf_fault(s, ZSRC_ZB_STEP_NONE, &fail);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SIREN_TEST, sid));
    run(1000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_SIREN_TEST, g.op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, g.result);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, page(sid).siren.last_test);
}

/* ZB-012, ZB-018 (testing.md §7): the radio unplugged in the middle of a setup and of an operation -
 * the add page's operation ends FAILED, the remove too; nothing more is sent; every request is
 * NO_RADIO; the zones get no CHECK each (the source's link is one trouble); back, the setup goes on
 * to Ready and the remove works. */
static void test_radio_unplugged_mid_operation_and_back(void)
{
    zbf_dev_t *a = added("third_reality_3rds17bz", true);
    bind(4u, a->ieee, 1u);
    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    make_router(d);
    const zbf_fault_t slow = {.delay_ms = 1500u};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, &slow);
    open_and_join(d);
    run(4000u);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, rec(d->ieee)->setup);
    const size_t zev_before = g_nzev;

    zbf_unplug(&F, true);
    radio(UI_ZB_RADIO_NOT_ANSWERING);
    run(100u);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, g.result);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RADIO_NOT_ANSWERING, status().radio);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NO_RADIO, ask(UI_ZB_REQ_ADD, 0u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NO_RADIO, ask(UI_ZB_REQ_PING, id_of(a)));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NO_RADIO, ask(UI_ZB_REQ_REMOVE, id_of(a)));
    const size_t sent = mark();
    run(60000u);
    for (size_t i = log_from(sent); i < zbf_log_count(&F); i++) {
        TEST_ASSERT_TRUE_MESSAGE(zbf_log_at(&F, i)->rx, "sent while the radio was out");
    }
    for (size_t i = zev_before; i < g_nzev; i++) {
        TEST_ASSERT_TRUE(g_zev[i].ev != ALARM_ZEV_UNREACHABLE);
    }
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, rec(d->ieee)->setup);

    zbf_unplug(&F, false);
    radio(UI_ZB_RADIO_WORKING);
    until_set_up(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(id_of(d)).dev.state);

    /* An operation in flight when it goes: FAILED, not NO_ANSWER - its request ends NO_RADIO
     * before the panel hears the radio is out (found by this replay: a ping said "didn't
     * answer", and a remove offered "Forget it anyway?" for a device never asked to leave). */
    static const uint8_t in_flight[] = {UI_ZB_REQ_PING, UI_ZB_REQ_REMOVE};
    for (size_t k = 0; k < sizeof(in_flight); k++) {
        zbf_fault(d, ZSRC_ZB_STEP_NONE, &slow);
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(in_flight[k], id_of(d)));
        run(5u); /* before it reaches the device */
        zbf_unplug(&F, true);
        run(10u);
        radio(UI_ZB_RADIO_NOT_ANSWERING);
        run(100u);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, progress().step);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
        TEST_ASSERT_NOT_NULL(rec(d->ieee)); /* still recorded, still in the network */
        TEST_ASSERT_TRUE(d->joined);
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
        zbf_unplug(&F, false);
        radio(UI_ZB_RADIO_WORKING);
        run(100u);
    }
    zbf_fault(d, ZSRC_ZB_STEP_NONE, NULL);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, id_of(d)));
    run(500u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ANSWERED, progress().result);
    /* the zone kept its sensor: it still trips */
    TEST_ASSERT_TRUE(zbf_trip(&F, a, true));
    run(100u);
    TEST_ASSERT_TRUE(zone_events(4u, ALARM_ZEV_FAULT) >= 1u);
}

/* ZB-013 (testing.md §7): Replace - the new sensor takes the old one's zone (the hub's binding
 * moves), the old record goes, both are logged as one REPLACED; the new one trips the zone, the old
 * one no longer does. */
static void test_replace_moves_the_zone(void)
{
    zbf_dev_t *old_dev = added("sonoff_snzb_04pr2", false);
    const uint16_t old_id = id_of(old_dev);
    bind(3u, old_dev->ieee, 1u);
    zbf_dev_t *nd = zbf_add(&F, "third_reality_3rds17bz");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_REPLACE, progress().op);
    TEST_ASSERT_EQUAL_UINT16(old_id, progress().old_id);
    run(100u);
    TEST_ASSERT_TRUE(zbf_join(&F, nd));
    until_set_up(nd);
    run(1000u);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_REPLACED, g.result);
    TEST_ASSERT_EQUAL_UINT16(old_id, g.old_id);
    TEST_ASSERT_EQUAL_UINT16(id_of(nd), g.id);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT16(3u, g.zones[0]);
    TEST_ASSERT_NULL(rec(old_dev->ieee));
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&HUB, 3u, &b));
    const zsrc_address_t want = zsrc_zb_address(nd->ieee, 1u);
    TEST_ASSERT_TRUE(zsrc_address_equal(&want, &b.in[ZSRC_ROLE_MAIN].addr));
    TEST_ASSERT_EQUAL_UINT(1u, logged(ZBZ_EV_REPLACED));
    TEST_ASSERT_EQUAL_UINT16(3u, last_logged(ZBZ_EV_REPLACED)->zone);
    TEST_ASSERT_EQUAL_UINT16(3u, zbz_panel_device_of_zone(&P, 3u) == id_of(nd) ? 3u : 0u);
    const size_t before = g_nzev;
    TEST_ASSERT_TRUE(zbf_trip(&F, nd, true));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(3u, ALARM_ZEV_FAULT));
    TEST_ASSERT_TRUE(zbf_trip(&F, nd, false));
    run(100u);
    const size_t after_new = g_nzev;
    (void)zbf_trip(&F, old_dev, true); /* still in the air, no longer anyone's */
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(3u, ALARM_ZEV_FAULT));
    TEST_ASSERT_TRUE(after_new > before);
}

/* ZB-013 (testing.md §7): remove without an answer, then forget - "Asked it to leave" ends NO_ANSWER
 * with its zone listed; Forget removes it (logged with the zone), the zone loses its sensor and shows
 * CHECK soon after, and the record is erased from the store. */
static void test_remove_without_an_answer_then_forget(void)
{
    zbf_dev_t *d = added("sonoff_snzb_04pr2", true);
    const uint16_t id = id_of(d);
    bind(6u, d->ieee, 1u);
    write_back();
    const zbf_fault_t mute = {.timeout = true};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, &mute);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REMOVE, id));
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, g.step);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT16(6u, g.zones[0]);
    run(ZBZ_LEAVE_MS + 1000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, g.result);
    TEST_ASSERT_NOT_NULL(rec(d->ieee)); /* still there until forgotten */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_FORGET, id));
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FORGOTTEN, g.result);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT16(6u, g.zones[0]);
    TEST_ASSERT_NULL(rec(d->ieee));
    TEST_ASSERT_EQUAL_UINT(1u, logged(ZBZ_EV_FORGOTTEN));
    TEST_ASSERT_EQUAL_UINT16(6u, last_logged(ZBZ_EV_FORGOTTEN)->zone);
    TEST_ASSERT_EQUAL_UINT16(id, last_logged(ZBZ_EV_FORGOTTEN)->device);
    run(((uint64_t)ZSRC_ZB_GONE_GRACE_S + 2u) * 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(6u, ALARM_ZEV_UNREACHABLE));
    (void)zbf_trip(&F, d, true); /* its traffic no longer feeds the zone */
    run(100u);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(6u, ALARM_ZEV_FAULT));
    write_back();
    for (size_t i = 0; i < sizeof(g_kv) / sizeof(g_kv[0]); i++) {
        zbz_device_t back;
        if (g_kv[i].used && g_kv[i].key[0] == 'd') {
            TEST_ASSERT_FALSE(zbz_dev_decode(&back, g_kv[i].buf, g_kv[i].len) == ZBZ_REG_OK &&
                              back.ieee == d->ieee);
        }
    }
    TEST_ASSERT_EQUAL_UINT16(0u, status().devices);
}

/* ZB-022 (testing.md §7): the walk test - a bound sensor is asked into IAS test mode for
 * ZSRC_ZB_TEST_MODE_LEN seconds (the frame's one byte of seconds), again before it runs out, and its
 * trips reach the zone as tests; a sensor that refuses test mode is not asked again, and one that
 * ignores it (no answer, no TEST bit), and their trips stay faults (fail-safe); after the walk test
 * each is asked back to normal and trips are faults. */
static void test_walk_test_mode(void)
{
    zbf_dev_t *a = added("sonoff_snzb_04pr2", true);
    zbf_dev_t *b = added("third_reality_3rds17bz", true);
    bind(3u, a->ieee, 1u);
    bind(4u, b->ieee, 1u);
    const zbf_fault_t refuse = {.status = ZBZ_ZCL_UNSUP_COMMAND};
    zbf_fault(b, ZSRC_ZB_STEP_NONE, &refuse);
    zbf_dev_t *c = added("centralite_3300", true);
    bind(5u, c->ieee, 1u);
    const zbf_fault_t ignore = {.timeout = true};
    zbf_fault(c, ZSRC_ZB_STEP_NONE, &ignore);
    const size_t from = mark();
    zbz_panel_walk_test(&P, g_now, true);
    uint8_t covered[ALARM_ZONE_BITMAP_BYTES];
    memset(covered, 0, sizeof(covered));
    covered[0] = (uint8_t)((1u << 2) | (1u << 3) | (1u << 4)); /* zones 3, 4 and 5 */
    zbz_panel_walk_scope(&P, g_now, covered);
    run(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(a->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                                        CS(ZBZ_IAS_TEST_MODE), from));
    const zbf_frame_t *t = NULL;
    for (size_t i = log_from(from); i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->ieee == a->ieee && zcl_is(f, ZSRC_ZB_ZCL_IAS_ZONE, CS(ZBZ_IAS_TEST_MODE))) {
            t = f;
        }
    }
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_TEST_MODE_LEN, t->bytes[3]); /* seconds */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_TEST_ON, zsrc_zb_device(&Z, a->ieee, 1u)->test);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_TEST_REFUSED, zsrc_zb_device(&Z, b->ieee, 1u)->test);
    TEST_ASSERT_TRUE(zbf_trip(&F, a, true));
    TEST_ASSERT_TRUE(zbf_trip(&F, b, true));
    run(ZBF_TIMEOUT_MS + 1000u); /* the ignoring one's request has timed out */
    TEST_ASSERT_TRUE(zsrc_zb_device(&Z, c->ieee, 1u)->test != ZSRC_ZB_TEST_ON);
    TEST_ASSERT_TRUE(zbf_trip(&F, c, true));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(3u, ALARM_ZEV_FAULT));
    TEST_ASSERT_TRUE(zone_events(3u, ALARM_ZEV_TEST) >= 1u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(4u, ALARM_ZEV_FAULT));
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(4u, ALARM_ZEV_TEST));
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(5u, ALARM_ZEV_FAULT));
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(5u, ALARM_ZEV_TEST));
    (void)zbf_trip(&F, a, false);
    (void)zbf_trip(&F, b, false);
    (void)zbf_trip(&F, c, false);
    /* asked again before its minute runs out; the refusing one never */
    const size_t again = mark();
    run(ZSRC_ZB_TEST_REFRESH_MS + 1000u);
    TEST_ASSERT_TRUE(tx_count(a->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE, CS(ZBZ_IAS_TEST_MODE),
                              again) >= 1u);
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(b->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                                        CS(ZBZ_IAS_TEST_MODE), again));
    /* over: back to normal, trips are faults */
    const size_t off = mark();
    zbz_panel_walk_test(&P, g_now, false);
    run(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(a->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                                        CS(ZBZ_IAS_NORMAL_MODE), off));
    TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)(zbf_ep(a, 1u)->test_until != 0u));
    TEST_ASSERT_TRUE(zbf_trip(&F, a, true));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(3u, ALARM_ZEV_FAULT));
}

/* ZB-010 (testing.md §6, §7): the NVS reserve - one entry under ZBZ_NVS_ADD_MIN: the status says
 * storage_full, ADD is FULL, a device that joins in a window opened before is asked to leave
 * (RES_FULL) and is not recorded, a rename still goes through; at ZBZ_NVS_ADD_MIN it adds again. */
static void test_nvs_reserve_full(void)
{
    zbf_dev_t *a = added("sonoff_snzb_04pr2", true);
    const uint16_t aid = id_of(a);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    zbz_panel_nvs_free(&P, ZBZ_NVS_ADD_MIN - 1u);
    TEST_ASSERT_TRUE(status().storage_full);
    zbf_dev_t *d = zbf_add(&F, "third_reality_3rds17bz");
    const size_t from = mark();
    TEST_ASSERT_TRUE(zbf_join(&F, d));
    run(1000u);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FULL, g.result);
    TEST_ASSERT_NULL(rec(d->ieee));
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(d->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_LEAVE, 0xFFFFu, from));
    TEST_ASSERT_FALSE(d->joined); /* it took the leave */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, ask(UI_ZB_REQ_ADD, 0u));
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_RENAME;
    req.user = 1u;
    req.id = aid;
    snprintf(req.name, sizeof(req.name), "%s", "Back door");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&P, g_now, &req, true));
    TEST_ASSERT_EQUAL_STRING("Back door", page(aid).dev.name);
    zbz_panel_nvs_free(&P, ZBZ_NVS_ADD_MIN);
    TEST_ASSERT_FALSE(status().storage_full);
    TEST_ASSERT_TRUE(zbf_join(&F, d) == false); /* the window is shut */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    run(100u);
    TEST_ASSERT_TRUE(zbf_join(&F, d));
    until_set_up(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
}

/* ZB-023 (testing.md §4, §7): a siren follows an alarm end to end - a Zigbee contact bound to the
 * window zone trips while the partition is armed away, the hub tells the real engine, the engine's
 * bell sounds, and the siren gets a burglar Start Warning; disarmed, a Stop. */
static void test_siren_follows_an_alarm(void)
{
    start(true);
    zbf_dev_t *c = added("sonoff_snzb_04pr2", false);
    zbf_dev_t *s = added("frient_sirzb_111", false);
    bind(Z_WINDOW, c->ieee, 1u);
    run(100u);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = id_of(s);
    req.siren.partitions = 1u;
    req.siren.burglary = true;
    req.siren.fire = true;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&P, g_now, &req, true));
    news();
    run(1000u);
    sim_arm_away_and_leave(&SIM, CODE_USER);
    news();
    run(1000u);
    const zbf_ep_t *wd = zbf_ep(s, 43u);
    const uint32_t before = wd->warnings;
    TEST_ASSERT_TRUE(zbf_trip(&F, c, true));
    run(200u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(Z_WINDOW, ALARM_ZEV_FAULT));
    TEST_ASSERT_EQUAL(ALARM_PAT_STEADY, sim_output(&SIM, ALARM_OUT_BELL, 1u));
    news();
    run(500u);
    TEST_ASSERT_TRUE(wd->warnings > before);
    zbw_warning_t w;
    zbw_warning_info_decode(wd->last_cmd.payload[0], ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    TEST_ASSERT_EQUAL(ALARM_OK, sim_disarm(&SIM, 1u, CODE_USER));
    news();
    run(500u);
    zbw_warning_info_decode(wd->last_cmd.payload[0], ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
    TEST_ASSERT_TRUE(g_bell_trouble != 1);
}

/* ZB-024 (testing.md §9): a keypad arms the real engine - its Arm with Alex's code goes to the alarm
 * task, the Arm Response says all armed, the exit delay is shown on it with its seconds, and its
 * disarm ends it. */
static void test_keypad_arms_the_panel(void)
{
    start(true);
    zbf_dev_t *k = added("frient_kepzb_110", false);
    const uint8_t KEP = 44u;
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_KEYPAD;
    req.user = 1u;
    req.id = id_of(k);
    req.keypad.partition = 1u;
    req.keypad.show_state = true;
    (void)zbf_check_in(&F, k);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&P, g_now, &req, true));
    news();
    run(200u);
    TEST_ASSERT_EQUAL_UINT8(2u, page(id_of(k)).keypad.device); /* 1 is the panel's own */
    zsrc_zb_zcl_t c;
    memset(&c, 0, sizeof(c));
    c.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    c.command = ZBA_CMD_ARM;
    c.tsn = ZSRC_ZB_TSN_NEW;
    c.payload[0] = ZBA_ARM_ALL;
    c.payload[1] = (uint8_t)strlen(CODE_USER);
    memcpy(&c.payload[2], CODE_USER, strlen(CODE_USER));
    c.payload[2u + strlen(CODE_USER)] = 0u;
    c.len = (uint8_t)(3u + strlen(CODE_USER));
    TEST_ASSERT_TRUE(zbf_send(&F, k, KEP, &c));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, FQN);
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_task());
    run(100u);
    const zbf_ep_t *e = zbf_ep(k, KEP);
    TEST_ASSERT_EQUAL_HEX8(ZBA_RSP_ARM, e->last_cmd.command);
    TEST_ASSERT_EQUAL_HEX8(ZBA_NOTE_ALL_ARMED, e->last_cmd.payload[0]);
    news();
    run(200u);
    EXPECT_STATE(&SIM, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_HEX8(ZBA_RSP_PANEL_STATUS_CHANGED, e->last_cmd.command);
    TEST_ASSERT_EQUAL_HEX8(ZBA_PS_EXIT_DELAY, e->last_cmd.payload[0]);
    TEST_ASSERT_TRUE(e->last_cmd.payload[1] > 0u); /* its seconds */
    c.payload[0] = ZBA_ARM_DISARM;
    c.tsn = ZSRC_ZB_TSN_NEW;
    TEST_ASSERT_TRUE(zbf_send(&F, k, KEP, &c));
    run(100u);
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_task());
    run(100u);
    news();
    EXPECT_STATE(&SIM, 1, ALARM_STATE_DISARMED);
}

/* ZB-012 (testing.md §7): no node descriptor (Xiaomi's way) - the step is optional, the setup goes
 * on, and router or sleepy is decided by its announce's MAC capabilities. */
static void test_no_node_descriptor(void)
{
    zbf_dev_t *d = zbf_add(&F, "aqara_magnet_agl02");
    const zbf_fault_t none = {.status = ZBF_ZDP_NOT_SUPPORTED};
    zbf_fault(d, ZSRC_ZB_STEP_NODE_DESC, &none);
    open_and_join(d);
    until_set_up(d);
    const zbz_device_t *r = rec(d->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, r->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_NODE_UNKNOWN, r->node_type);
    TEST_ASSERT_TRUE(zbz_dev_sleepy(r));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(r->id).dev.state);
}

/* ZB-003, ZB-012 (testing.md §7): IAS Zone already enrolled (a sensor moved from another panel),
 * status without an Enroll Request, and a duplicate Enroll Request after success - each ends with a
 * working zone and no second setup. */
static void test_enrollment_edge_cases(void)
{
    /* already enrolled, with another panel's zone id */
    zbf_dev_t *a = zbf_add(&F, "sonoff_snzb_04pr2");
    make_router(a);
    zbf_ep(a, 1u)->enrolled = true;
    zbf_ep(a, 1u)->zone_id = 0x33u;
    open_and_join(a);
    until_set_up(a);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(a->ieee)->setup);
    close_add();
    bind(3u, a->ieee, 1u);
    TEST_ASSERT_TRUE(zbf_trip(&F, a, true));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(3u, ALARM_ZEV_FAULT));

    /* never asks to enroll: the unasked Enroll Response enrolls it */
    zbf_dev_t *b = zbf_add(&F, "third_reality_3rds17bz");
    make_router(b);
    b->auto_enroll = false;
    open_and_join(b);
    until_set_up(b);
    TEST_ASSERT_TRUE(zbf_ep(b, 1u)->enrolled);
    TEST_ASSERT_EQUAL_UINT8(zsrc_zb_device(&Z, b->ieee, 1u)->zone_id, zbf_ep(b, 1u)->zone_id);
    close_add();
    bind(4u, b->ieee, 1u);
    TEST_ASSERT_TRUE(zbf_ias_status(&F, b, 1u, ZSRC_ZB_IAS_ALARM1));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(4u, ALARM_ZEV_FAULT));
    (void)zbf_ias_status(&F, b, 1u, 0u);
    run(100u);

    /* a duplicate Enroll Request after success: answered with the same zone id, nothing else */
    const uint8_t zone_id = zbf_ep(b, 1u)->zone_id;
    const unsigned adds = logged(ZBZ_EV_ADDED);
    const size_t from = mark();
    zsrc_zb_zcl_t req;
    memset(&req, 0, sizeof(req));
    req.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    req.command = ZBZ_IAS_ENROLL_REQUEST;
    req.server_to_client = true;
    req.tsn = ZSRC_ZB_TSN_NEW;
    req.payload[0] = (uint8_t)(ZSRC_ZB_ZONE_TYPE_CONTACT & 0xFFu);
    req.payload[1] = 0u;
    req.payload[2] = (uint8_t)(b->node.manufacturer_code & 0xFFu);
    req.payload[3] = (uint8_t)(b->node.manufacturer_code >> 8);
    req.len = 4u;
    TEST_ASSERT_TRUE(zbf_send(&F, b, 1u, &req));
    run(ZSRC_ZB_ENROLL_GAP_MS * 2u);
    TEST_ASSERT_TRUE(zbf_ep(b, 1u)->enrolled);
    TEST_ASSERT_EQUAL_UINT8(zone_id, zbf_ep(b, 1u)->zone_id);
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(b->ieee, ZBF_FRAME_ZDO, ZBF_ZDO_NODE_DESC, 0xFFFFu, from));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(b->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT(adds, logged(ZBZ_EV_ADDED));
    TEST_ASSERT_TRUE(zbf_ias_status(&F, b, 1u, ZSRC_ZB_IAS_ALARM1));
    run(100u);
    TEST_ASSERT_EQUAL_UINT(2u, zone_events(4u, ALARM_ZEV_FAULT));
}

/* ZB-012, ZB-021 (testing.md §7): an announce from an address the panel never admitted makes no
 * record and logs nothing (only a trust-center join does); the sequence numbers wrap from 0xFF to
 * 0x00 in the middle of a setup without losing an answer. */
static void test_unknown_announce_and_sequence_wrap(void)
{
    zbf_dev_t *u = zbf_add(&F, "sonoff_snzb_02d");
    u->joined = true; /* in the network through some other way, never through the panel's window */
    TEST_ASSERT_TRUE(zbf_check_in(&F, u));
    run(1000u);
    TEST_ASSERT_NULL(rec(u->ieee));
    TEST_ASSERT_EQUAL_UINT16(0u, status().devices);
    TEST_ASSERT_EQUAL_UINT(0u, logged(ZBZ_EV_ADDED));

    zbf_set_tsn(&F, 0xFEu, 0xFEu);
    zbf_dev_t *d = zbf_add(&F, "sonoff_snzb_04pr2");
    make_router(d);
    d->tsn = 0xFDu;
    const size_t from = mark();
    open_and_join(d);
    until_set_up(d);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, rec(d->ieee)->setup);
    bool zcl_ff = false, zcl_00 = false, zdo_ff = false, zdo_00 = false;
    for (size_t i = log_from(from); i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->rx || f->ieee != d->ieee || f->len < 3u) {
            continue;
        }
        if (f->kind == ZBF_FRAME_ZCL) {
            zcl_ff = zcl_ff || f->bytes[1] == 0xFFu;
            zcl_00 = zcl_00 || f->bytes[1] == 0x00u;
        } else if (f->kind == ZBF_FRAME_ZDO) {
            zdo_ff = zdo_ff || f->bytes[0] == 0xFFu;
            zdo_00 = zdo_00 || f->bytes[0] == 0x00u;
        }
    }
    TEST_ASSERT_TRUE(zcl_ff && zcl_00);
    TEST_ASSERT_TRUE(zdo_ff && zdo_00);
}

/* ==== §8 supervision and sleepy devices ============================================================ */

/* When the panel last heard `d` (its last frame on the air to the panel). */
static uint64_t last_heard(const zbf_dev_t *d)
{
    uint64_t at = 0u;
    for (size_t i = 0; i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->rx && f->ieee == d->ieee && f->at > at) {
            at = f->at;
        }
    }
    TEST_ASSERT_TRUE(at != 0u);
    return at;
}

/* ZB-022 (docs/features/40 WALK-007; the review of wave 6, its "must"): the walk test belongs to a
 * partition and to the zone types it covers. Partition 1 is walk-tested while partition 2 is armed
 * away: only partition 1's window sensor is asked into IAS test mode, and its trip marks the zone
 * tested; partition 2's sensor trips a real burglary alarm, and partition 1's smoke detector -
 * fire stays live - a real fire alarm. Before the fix the walk test asked every sensor into test
 * mode and turned both alarms into "tests". */
static void test_walk_test_scope_by_partition_and_zone_type(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 21u, ALARM_ZT_PERIMETER, 2u);
    start_with(true, &cfg);
    zbf_dev_t *a = added("sonoff_snzb_04pr2", true);
    zbf_dev_t *b = added("third_reality_3rds17bz", true);
    zbf_dev_t *c = added("heiman_smoke_em", true);
    bind(Z_WINDOW, a->ieee, 1u);
    bind(21u, b->ieee, 1u);
    bind(Z_SMOKE, c->ieee, 1u);
    run(1000u);
    TEST_ASSERT_EQUAL(ALARM_OK, sim_arm(&SIM, 2u, CODE_MASTER, ALARM_LEVEL_AWAY, 0u));
    run(61000u);
    TEST_ASSERT_EQUAL(ALARM_STATE_ARMED, sim_status(&SIM, 2u).state);
    TEST_ASSERT_EQUAL(ALARM_OK, sim_simple(&SIM, ALARM_CMD_WALK_TEST, 1u, CODE_MASTER));
    TEST_ASSERT_TRUE(sim_status(&SIM, 1u).walk_test);

    /* What the owner does on the engine's news: the covered zones, then the scope. */
    alarm_snapshot_t snap;
    alarm_engine_snapshot(&SIM.eng, &snap);
    uint8_t covered[ALARM_ZONE_BITMAP_BYTES];
    zbz_walk_covered(&SIM.cfg, &snap, covered);
    TEST_ASSERT_TRUE((covered[(Z_WINDOW - 1u) / 8u] & (1u << ((Z_WINDOW - 1u) % 8u))) != 0u);
    TEST_ASSERT_FALSE((covered[(21u - 1u) / 8u] & (1u << ((21u - 1u) % 8u))) != 0u);
    TEST_ASSERT_FALSE((covered[(Z_SMOKE - 1u) / 8u] & (1u << ((Z_SMOKE - 1u) % 8u))) != 0u);
    const size_t from = mark();
    zbz_panel_walk_test(&P, g_now, true);
    zbz_panel_walk_scope(&P, g_now, covered);
    run(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(a->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                                        CS(ZBZ_IAS_TEST_MODE), from));
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(b->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                                        CS(ZBZ_IAS_TEST_MODE), from));
    TEST_ASSERT_EQUAL_UINT(0u, tx_count(c->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_IAS_ZONE,
                                        CS(ZBZ_IAS_TEST_MODE), from));

    /* Partition 1's window: its sensor's own test marks it, no alarm. */
    TEST_ASSERT_TRUE(zbf_trip(&F, a, true));
    run(200u);
    TEST_ASSERT_TRUE(zone_events(Z_WINDOW, ALARM_ZEV_TEST) >= 1u);
    TEST_ASSERT_TRUE(sim_zstatus(&SIM, Z_WINDOW).walk != 0u);
    TEST_ASSERT_FALSE((sim_zstatus(&SIM, Z_WINDOW).flags & ALARM_ZS_ALARM) != 0u);
    /* Partition 2, armed away: a burglar. */
    TEST_ASSERT_TRUE(zbf_trip(&F, b, true));
    run(200u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(21u, ALARM_ZEV_FAULT));
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(21u, ALARM_ZEV_TEST));
    TEST_ASSERT_TRUE(sim_alarm_logged(&SIM, 21u, ALARM_CAUSE_BURGLARY));
    /* Partition 1's smoke detector during its walk test: a fire. */
    TEST_ASSERT_TRUE(zbf_trip(&F, c, true));
    run(200u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(Z_SMOKE, ALARM_ZEV_FAULT));
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(Z_SMOKE, ALARM_ZEV_TEST));
    TEST_ASSERT_TRUE(sim_alarm_logged(&SIM, Z_SMOKE, ALARM_CAUSE_FIRE));
}

/* ZB-023 (alarm-devices.md section 4; the review of wave 6): a mains siren that sends nothing by
 * itself is read by the panel each third of its silence limit, so in six quiet hours it is never
 * the bell trouble; dead (it answers nothing), it is, at its limit. */
static void test_a_quiet_mains_siren_is_no_trouble(void)
{
    zbf_dev_t *s = added("frient_sirzb_111", true);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = id_of(s);
    req.siren.partitions = 1u;
    req.siren.burglary = true;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&P, g_now, &req, true));
    run(1000u);
    for (unsigned h = 0; h < 6u; h++) {
        const uint64_t pinged = P.wd_ping_at[0];
        run(60u * 60u * 1000u);
        TEST_ASSERT_TRUE(g_bell_trouble != 1);
        TEST_ASSERT_TRUE(P.wd_ping_at[0] > pinged); /* read at least once in the hour */
    }
    /* The read itself, around when it is due (the frame log keeps only the newest frames). */
    const uint64_t since = P.wd.siren[0].heard_at > P.wd_ping_at[0] ? P.wd.siren[0].heard_at
                                                                     : P.wd_ping_at[0];
    const uint64_t due = since + (uint64_t)ZBW_SILENT_ROUTER_S * 1000u / 3u;
    TEST_ASSERT_TRUE(due > g_now + 100000u);
    run(due - g_now - 100000u);
    const size_t from = mark();
    run(200000u);
    TEST_ASSERT_EQUAL_UINT(1u, tx_count(s->ieee, ZBF_FRAME_ZCL, ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_READ_ATTR,
                                        from));
    const zbf_fault_t dead = {.timeout = true};
    zbf_fault(s, ZSRC_ZB_STEP_NONE, &dead);
    run((uint64_t)ZBW_SILENT_ROUTER_S * 1000u + 60000u);
    TEST_ASSERT_EQUAL_INT(1, g_bell_trouble);
}

/* ZB-013 (devices.md section 6; the review of wave 6): a network reset forgets every device, and
 * each zone one fed shows CHECK within seconds - not only the newest eight's, the others an hour
 * later. */
static void test_network_reset_makes_every_zone_check(void)
{
    static const char *const names[] = {"sonoff_snzb_04pr2",  "third_reality_3rds17bz",
                                        "centralite_3300",    "heiman_smoke_em",
                                        "aqara_magnet_agl02", "aqara_motion_agl04",
                                        "ikea_parasoll",      "centralite_3326l",
                                        "sonoff_snzb_02d",    "tuya_ts0216"};
    const size_t n = sizeof(names) / sizeof(names[0]);
    zbf_dev_t *d[sizeof(names) / sizeof(names[0])];
    for (size_t i = 0; i < n; i++) {
        d[i] = added(names[i], true);
        const zbz_device_t *r = rec(d[i]->ieee);
        TEST_ASSERT_NOT_NULL(r);
        bind((uint16_t)(30u + i), d[i]->ieee, r->ep[0].endpoint);
    }
    run(1000u);
    g_nzev = 0u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_RESET, 0u));
    run(5000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    run(((uint64_t)ZSRC_ZB_GONE_GRACE_S + 2u) * 1000u);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, zone_events((uint16_t)(30u + i), ALARM_ZEV_UNREACHABLE),
                                       names[i]);
    }
}

/* ZB-005 (testing.md §8): an Aqara contact that speaks once an hour never goes CHECK under the
 * 2 h + 1 h rule; silent, its zone shows CHECK 2 h + 1 h after its last frame and not a minute
 * before; its next frame clears it. */
static void test_hourly_check_in_never_check(void)
{
    zbf_dev_t *d = added("aqara_magnet_agl02", false);
    bind(7u, d->ieee, 1u);
    const zsrc_zb_device_t *s = zsrc_zb_device(&Z, d->ieee, 1u);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_SUPERVISION_S, zsrc_zb_supervision_s(&Z, s));
    zbf_set_check_in(&F, d, 3600u);
    run(10u * 3600u * 1000u);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(7u, ALARM_ZEV_UNREACHABLE));
    TEST_ASSERT_FALSE(page(id_of(d)).dev.unreachable);
    /* silent from now */
    zbf_set_check_in(&F, d, 0u);
    (void)zbf_check_in(&F, d);
    run(100u);
    const uint64_t last = last_heard(d);
    const uint64_t limit = ((uint64_t)ZSRC_ZB_SUPERVISION_S + ZSRC_ZB_GRACE_S) * 1000u;
    run(last + limit - 60000u - g_now);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(7u, ALARM_ZEV_UNREACHABLE));
    run(120u * 1000u);
    const zev_t *e = first_zone_event(7u, ALARM_ZEV_UNREACHABLE);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->at - last >= limit);
    TEST_ASSERT_TRUE(page(id_of(d)).dev.unreachable);
    /* its next frame clears it */
    (void)zbf_check_in(&F, d);
    run(100u);
    TEST_ASSERT_EQUAL_UINT(1u, zone_events(7u, ALARM_ZEV_REACHABLE));
    TEST_ASSERT_FALSE(page(id_of(d)).dev.unreachable);
}

/* ZB-005 (testing.md §8): two mains-powered sensors that speak every 10 minutes; one goes silent
 * and its zone shows CHECK 30 min + 1 h after its last frame, not a minute before; the other never
 * does. */
static void test_mains_device_that_stops_answering(void)
{
    zbf_dev_t *a = added("sonoff_snzb_04pr2", true);
    zbf_dev_t *b = added("third_reality_3rds17bz", true);
    bind(3u, a->ieee, 1u);
    bind(4u, b->ieee, 1u);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_ROUTER_SUPERVISION_S,
                             zsrc_zb_supervision_s(&Z, zsrc_zb_device(&Z, a->ieee, 1u)));
    zbf_set_check_in(&F, a, 600u);
    zbf_set_check_in(&F, b, 600u);
    run(3u * 3600u * 1000u);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(3u, ALARM_ZEV_UNREACHABLE));
    zbf_set_check_in(&F, a, 0u); /* powered off */
    const uint64_t last = last_heard(a);
    const uint64_t limit = ((uint64_t)ZSRC_ZB_ROUTER_SUPERVISION_S + ZSRC_ZB_GRACE_S) * 1000u;
    run(last + limit - 60000u - g_now);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(3u, ALARM_ZEV_UNREACHABLE));
    run(120u * 1000u);
    const zev_t *e = first_zone_event(3u, ALARM_ZEV_UNREACHABLE);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->at - last >= limit);
    TEST_ASSERT_TRUE(page(id_of(a)).dev.unreachable);
    run(3u * 3600u * 1000u);
    TEST_ASSERT_EQUAL_UINT(0u, zone_events(4u, ALARM_ZEV_UNREACHABLE));
    TEST_ASSERT_FALSE(page(id_of(b)).dev.unreachable);
}

/* ZB-005, ZB-014 (testing.md §8): "Check it now" on a sleepy device waits for its next frame (a Poll
 * Control check-in answers it at once); with none it ends NO_ANSWER after ZBZ_WAKE_WAIT_MS. */
static void test_check_it_now_waits_for_a_sleepy_device(void)
{
    zbf_dev_t *d = added("sonoff_snzb_04pr2", false);
    const uint16_t id = id_of(d);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, id));
    run(60000u);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, g.step);
    TEST_ASSERT_TRUE(zbf_check_in(&F, d));
    run(100u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ANSWERED, g.result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, id));
    run(ZBZ_WAKE_WAIT_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, progress().result);
}

/* ZB-005 (testing.md §8): unchanged reports every 20 s are check-ins - the zone hears one at most
 * once a minute and never goes CHECK over four hours. */
static void test_unchanged_reports_count_as_check_ins(void)
{
    zbf_dev_t *d = added("sonoff_snzb_04pr2", false);
    bind(3u, d->ieee, 1u);
    run(1000u);
    const size_t from = g_nzev;
    const uint64_t t0 = g_now;
    for (unsigned i = 0; i < 4u * 180u; i++) {
        TEST_ASSERT_TRUE(zbf_ias_status(&F, d, 1u, 0u));
        run(20000u);
    }
    unsigned checkins = 0u;
    uint64_t prev = 0u;
    for (size_t i = from; i < g_nzev; i++) {
        TEST_ASSERT_TRUE(g_zev[i].ev != ALARM_ZEV_UNREACHABLE);
        if (g_zev[i].zone == 3u && g_zev[i].ev == ALARM_ZEV_CHECKIN) {
            TEST_ASSERT_TRUE(prev == 0u || g_zev[i].at - prev >= ZSRC_ZB_CHECKIN_EVERY_MS);
            prev = g_zev[i].at;
            checkins++;
        }
    }
    TEST_ASSERT_TRUE(checkins >= 4u * 60u - 2u);
    TEST_ASSERT_TRUE(checkins <= (unsigned)((g_now - t0) / ZSRC_ZB_CHECKIN_EVERY_MS) + 1u);
}

/* ZB-011 (testing.md §8): link quality - the buckets at their edges as the page shows them, and a
 * frame whose LQI and RSSI are 0 ("not measured") leaves the last measured values. */
static void test_link_quality_buckets_and_not_measured(void)
{
    zbf_dev_t *d = added("sonoff_snzb_04pr2", true);
    const uint16_t id = id_of(d);
    static const struct {
        uint8_t lqi;
        uint8_t signal;
    } edges[] = {
        {ZBZ_LQI_GOOD, UI_ZB_SIGNAL_GOOD},
        {ZBZ_LQI_GOOD - 1u, UI_ZB_SIGNAL_FAIR},
        {ZBZ_LQI_FAIR, UI_ZB_SIGNAL_FAIR},
        {ZBZ_LQI_FAIR - 1u, UI_ZB_SIGNAL_WEAK},
        {1u, UI_ZB_SIGNAL_WEAK},
        {255u, UI_ZB_SIGNAL_GOOD},
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        zbf_set_link(d, edges[i].lqi, -60);
        TEST_ASSERT_TRUE(zbf_check_in(&F, d));
        run(100u);
        const ui_zb_device_page_t pg = page(id);
        TEST_ASSERT_EQUAL_UINT8(edges[i].lqi, pg.lqi);
        TEST_ASSERT_EQUAL_UINT8(edges[i].signal, pg.dev.signal);
        TEST_ASSERT_EQUAL_UINT8(edges[i].signal, zbz_signal_of(edges[i].lqi));
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_UNKNOWN, zbz_signal_of(0u));
    zbf_set_link(d, 0u, 0);
    TEST_ASSERT_TRUE(zbf_check_in(&F, d));
    run(100u);
    const ui_zb_device_page_t pg = page(id);
    TEST_ASSERT_EQUAL_UINT8(255u, pg.lqi);
    TEST_ASSERT_EQUAL_INT8(-60, pg.rssi);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_GOOD, pg.dev.signal);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clean_join_to_ready);
    RUN_TEST(test_lossy_profile_retries_then_stops);
    RUN_TEST(test_lossy_enrollment_gives_up_and_reports_still_work);
    RUN_TEST(test_sleepy_profile_waits_for_its_wake);
    RUN_TEST(test_device_leaves_mid_setup);
    RUN_TEST(test_rejoin_with_a_new_short_address);
    RUN_TEST(test_basic_never_answers_and_a_model_row_names_it);
    RUN_TEST(test_several_endpoints);
    RUN_TEST(test_default_response_failure);
    RUN_TEST(test_radio_unplugged_mid_operation_and_back);
    RUN_TEST(test_replace_moves_the_zone);
    RUN_TEST(test_remove_without_an_answer_then_forget);
    RUN_TEST(test_walk_test_mode);
    RUN_TEST(test_walk_test_scope_by_partition_and_zone_type);
    RUN_TEST(test_a_quiet_mains_siren_is_no_trouble);
    RUN_TEST(test_network_reset_makes_every_zone_check);
    RUN_TEST(test_nvs_reserve_full);
    RUN_TEST(test_siren_follows_an_alarm);
    RUN_TEST(test_keypad_arms_the_panel);
    RUN_TEST(test_no_node_descriptor);
    RUN_TEST(test_enrollment_edge_cases);
    RUN_TEST(test_unknown_announce_and_sequence_wrap);
    RUN_TEST(test_hourly_check_in_never_check);
    RUN_TEST(test_mains_device_that_stops_answering);
    RUN_TEST(test_check_it_now_waits_for_a_sleepy_device);
    RUN_TEST(test_unchanged_reports_count_as_check_ins);
    RUN_TEST(test_link_quality_buckets_and_not_measured);
    return UNITY_END();
}
