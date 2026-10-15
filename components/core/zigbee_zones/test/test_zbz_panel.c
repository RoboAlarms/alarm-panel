/*
 * test_zbz_panel.c - zbz_panel against a scripted radio (docs/features/20 devices.md §4-§9,
 * testing.md §5-§8; ZB-010..ZB-018, ZB-021, ZB-027): each device's setup (a plain IAS contact, a
 * sleepy device waiting for its wake, a router, a device that is not a sensor, a failure that
 * stops with its step, retries), every operation and every refusal, the two permission levels,
 * the NVS reserve, the pages' views, what is stored (the cfg frozen fixture, damaged blobs) and
 * the log.
 *
 * The radio here records each hook call; the test answers them as a device would, from the
 * signature rows (signatures/zb_signatures.h) or by hand. The sirens, keypads and fobs (ZB-023,
 * ZB-024) and the Basic split read (ZB-012) run at the end against the fake radio's real frames
 * (testing.md §2) and the real engine (alarm_sim).
 */
#include <stdio.h>
#include <string.h>

#include "cfg_fixture_v1.h"
#include "signatures/zb_signatures.h"
#include "sim.h"
#include "unity.h"
#include "zbz_fake_radio.h"
#include "zigbee_zones/zbz_models.h"
#include "zigbee_zones/zbz_panel.h"

/* ---- the scripted radio ------------------------------------------------------------------------ */

enum {
    C_PERMIT = 1, C_CIE, C_ENROLL, C_READ1, C_REMOVE, C_SCAN, C_NODE, C_POWER, C_ACTIVE, C_SIMPLE,
    C_READ, C_BIND, C_REPORT, C_LEAVE, C_FORGET, C_IDENTIFY, C_TESTMODE, C_ZCL, C_IC, C_CODES_ONLY,
    C_CHANNEL, C_RESET, C_SURVEY, C_KINDS
};

typedef struct {
    uint8_t kind;
    uint64_t ieee;
    uint8_t ep;
    uint16_t cluster;
    uint16_t attr0;
    uint32_t tag;
    uint32_t value; /* seconds, a zone id, a channel, a code's length, an on/off */
    bool status_attr; /* a read that asks for IAS Zone's zone status (0x0002) */
    bool answered;
} call_t;

#define CALLS_MAX 2048u
static call_t g_calls[CALLS_MAX];
static size_t g_ncalls;
static bool g_refuse[C_KINDS]; /* the hook answers false: not queued */
static uint8_t g_fail_kind;    /* the scripted device answers this kind with g_fail_result ... */
static uint8_t g_fail_result;
static int g_fail_left;        /* ... this many times (-1: always) */
static uint8_t g_silent_kind;  /* ... or never answers it (no report, no DONE) */

static uint64_t g_now;
static zsrc_zb_t g_src;
static zbz_panel_t g_panel;
static zsrc_zb_radio_t g_radio;
static zsrc_sink_t g_sink;
static zbz_panel_port_t g_port;

static call_t *record(uint8_t kind, uint64_t ieee, uint8_t ep, uint16_t cluster, uint32_t tag,
                      uint32_t value)
{
    TEST_ASSERT_TRUE(g_ncalls < CALLS_MAX);
    call_t *c = &g_calls[g_ncalls++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->ieee = ieee;
    c->ep = ep;
    c->cluster = cluster;
    c->tag = tag;
    c->value = value;
    return c;
}

static bool r_permit(void *ctx, uint16_t s)
{
    (void)ctx;
    record(C_PERMIT, 0u, 0u, 0u, 0u, s);
    return !g_refuse[C_PERMIT];
}
static bool r_cie(void *ctx, uint64_t ieee, uint8_t ep)
{
    (void)ctx;
    record(C_CIE, ieee, ep, ZSRC_ZB_ZCL_IAS_ZONE, 0u, 0u);
    return !g_refuse[C_CIE];
}
static bool r_enroll(void *ctx, uint64_t ieee, uint8_t ep, uint8_t zone_id)
{
    (void)ctx;
    record(C_ENROLL, ieee, ep, ZSRC_ZB_ZCL_IAS_ZONE, 0u, zone_id)->answered = true;
    return !g_refuse[C_ENROLL];
}
static bool r_read1(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cl, uint16_t attr)
{
    (void)ctx;
    record(C_READ1, ieee, ep, cl, 0u, attr)->answered = true;
    return true;
}
static bool r_remove(void *ctx, uint64_t ieee)
{
    (void)ctx;
    record(C_REMOVE, ieee, 0u, 0u, 0u, 0u)->answered = true;
    return true;
}
static bool r_scan(void *ctx)
{
    (void)ctx;
    return true; /* the source's energy scans: not followed here */
}
static bool r_node(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    (void)nwk;
    record(C_NODE, ieee, 0u, 0u, tag, 0u);
    return !g_refuse[C_NODE];
}
static bool r_power(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    (void)nwk;
    record(C_POWER, ieee, 0u, 0u, tag, 0u);
    return !g_refuse[C_POWER];
}
static bool r_active(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    (void)nwk;
    record(C_ACTIVE, ieee, 0u, 0u, tag, 0u);
    return !g_refuse[C_ACTIVE];
}
static bool r_simple(void *ctx, uint64_t ieee, uint16_t nwk, uint8_t ep, uint32_t tag)
{
    (void)ctx;
    (void)nwk;
    record(C_SIMPLE, ieee, ep, 0u, tag, 0u);
    return !g_refuse[C_SIMPLE];
}
static bool r_read(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cl, const uint16_t *attrs,
                   uint8_t count, uint32_t tag)
{
    (void)ctx;
    call_t *c = record(C_READ, ieee, ep, cl, tag, count);
    c->attr0 = count > 0u ? attrs[0] : 0u;
    for (uint8_t k = 0; k < count; k++) {
        c->status_attr = c->status_attr || (cl == ZSRC_ZB_ZCL_IAS_ZONE && attrs[k] == 0x0002u);
    }
    return !g_refuse[C_READ];
}
static bool r_bind(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cl, uint32_t tag)
{
    (void)ctx;
    record(C_BIND, ieee, ep, cl, tag, 0u);
    return !g_refuse[C_BIND];
}
static bool r_report(void *ctx, uint64_t ieee, uint8_t ep, const zsrc_zb_reporting_t *r,
                     uint32_t tag)
{
    (void)ctx;
    record(C_REPORT, ieee, ep, r->cluster, tag, r->max_s)->attr0 = r->attr;
    return !g_refuse[C_REPORT];
}
static bool r_leave(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    (void)nwk;
    record(C_LEAVE, ieee, 0u, 0u, tag, 0u);
    return !g_refuse[C_LEAVE];
}
static bool r_forget(void *ctx, uint64_t ieee)
{
    (void)ctx;
    record(C_FORGET, ieee, 0u, 0u, 0u, 0u)->answered = true;
    return true;
}
static bool r_identify(void *ctx, uint64_t ieee, uint8_t ep, uint16_t s, uint32_t tag)
{
    (void)ctx;
    record(C_IDENTIFY, ieee, ep, ZSRC_ZB_ZCL_IDENTIFY, tag, s);
    return !g_refuse[C_IDENTIFY];
}
static bool r_testmode(void *ctx, uint64_t ieee, uint8_t ep, uint8_t s, uint32_t tag)
{
    (void)ctx;
    record(C_TESTMODE, ieee, ep, ZSRC_ZB_ZCL_IAS_ZONE, tag, s)->answered = true;
    return true;
}
static bool r_zcl(void *ctx, uint64_t ieee, uint8_t ep, const zsrc_zb_zcl_t *cmd, uint32_t tag)
{
    (void)ctx;
    record(C_ZCL, ieee, ep, cmd->cluster, tag, cmd->command)->answered = true;
    return true;
}
static bool r_ic(void *ctx, uint64_t ieee, const uint8_t *code, uint8_t len)
{
    (void)ctx;
    (void)code;
    record(C_IC, ieee, 0u, 0u, 0u, len)->answered = true;
    return !g_refuse[C_IC];
}
static bool r_codes_only(void *ctx, bool on)
{
    (void)ctx;
    record(C_CODES_ONLY, 0u, 0u, 0u, 0u, on ? 1u : 0u)->answered = true;
    return true;
}
static bool r_channel(void *ctx, uint8_t ch, uint32_t tag)
{
    (void)ctx;
    record(C_CHANNEL, 0u, 0u, 0u, tag, ch);
    return !g_refuse[C_CHANNEL];
}
static bool r_reset(void *ctx, uint32_t tag)
{
    (void)ctx;
    record(C_RESET, 0u, 0u, 0u, tag, 0u);
    return true;
}
static bool r_survey(void *ctx, uint32_t tag)
{
    (void)ctx;
    record(C_SURVEY, 0u, 0u, 0u, tag, 0u);
    return true;
}

static const zsrc_zb_radio_t RADIO = {
    .ctx = NULL,
    .permit_join = r_permit,
    .ias_cie_write = r_cie,
    .ias_enroll_response = r_enroll,
    .read_attribute = r_read1,
    .remove_device = r_remove,
    .energy_scan = r_scan,
    .node_desc_req = r_node,
    .power_desc_req = r_power,
    .active_ep_req = r_active,
    .simple_desc_req = r_simple,
    .read_attributes = r_read,
    .bind = r_bind,
    .configure_reporting = r_report,
    .leave = r_leave,
    .forget = r_forget,
    .identify = r_identify,
    .ias_test_mode = r_testmode,
    .zcl_command = r_zcl,
    .install_code = r_ic,
    .codes_only = r_codes_only,
    .set_channel = r_channel,
    .reset_network = r_reset,
    .channel_survey = r_survey,
};

/* ---- the sink and the owner's port ------------------------------------------------------------- */

static void s_input(void *ctx, const zsrc_address_t *a, const char *st, int32_t v, uint64_t now)
{
    (void)ctx;
    (void)a;
    (void)st;
    (void)v;
    (void)now;
}
static void s_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    (void)a;
    (void)ev;
    (void)now;
}
static void s_reach(void *ctx, const zsrc_address_t *a, bool ok, uint64_t now)
{
    (void)ctx;
    (void)a;
    (void)ok;
    (void)now;
}
static void s_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    (void)kind;
    (void)up;
    (void)now;
}

static zone_hub_binding_t g_bind[ALARM_MAX_ZONES + 1];
static bool g_bound[ALARM_MAX_ZONES + 1];
static bool g_rebind_refused;
static uint16_t g_rebind_refuse_zone; /* the hub refuses this zone only (its partition armed) */
static zone_bind_gate_t g_gate;       /* ZB21/RF32: the binding gate the owner's hooks drive */
static bool g_rebind_gated;           /* every rebind must find the gate claimed (CHANGING) */
static unsigned g_rebinds;
static bool g_replace_waits;          /* fresh(): port.replace_waits */
static bool g_no_enroll_request;      /* the sensor never asks to enroll: it waits for a response */
static uint32_t g_taken;
static unsigned g_changed;
#define WALL 1790000000u

static bool p_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    if (zone == 0u || zone > ALARM_MAX_ZONES || !g_bound[zone]) {
        return false;
    }
    *out = g_bind[zone];
    return true;
}
static bool p_rebind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    g_rebinds++;
    if (g_rebind_gated) {
        TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_CHANGING, g_gate.state); /* RF32: under a claim */
    }
    if (g_rebind_refused || (g_rebind_refuse_zone != 0u && zone == g_rebind_refuse_zone)) {
        return false;
    }
    g_bind[zone] = *b;
    g_bound[zone] = true;
    return true;
}
static uint32_t p_wall(void *ctx)
{
    (void)ctx;
    return WALL;
}
static void p_changed(void *ctx)
{
    (void)ctx;
    g_changed++;
}
static uint32_t p_taken(void *ctx)
{
    (void)ctx;
    return g_taken;
}

static void bind_zone(uint16_t zone, uint64_t ieee, uint8_t ep)
{
    memset(&g_bind[zone], 0, sizeof(g_bind[zone]));
    g_bind[zone].in[ZSRC_ROLE_MAIN].addr = zsrc_zb_address(ieee, ep);
    g_bound[zone] = true;
}

/* ---- the store --------------------------------------------------------------------------------- */

typedef struct {
    bool used;
    char key[ZBZ_KEY_LEN];
    uint8_t buf[640];
    size_t len;
} kv_t;
static kv_t g_kv[ZBZ_DEV_MAX + 8];

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
    if (e->len > cap) {
        return e->len;
    }
    memcpy(buf, e->buf, e->len);
    return e->len;
}
static const zbz_store_port_t STORE = {.ctx = NULL, .read = st_read};

/* The owner's write-back: every take, into the store. */
static void write_back(void)
{
    char key[ZBZ_KEY_LEN];
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    size_t len = 0u;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&g_panel, key, buf, sizeof(buf), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            kv_put(key, buf, len);
        } else {
            kv_erase(key);
        }
    }
    len = zbz_panel_take_codes(&g_panel, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBZ_KEY_CODES, buf, len);
    }
    len = zbz_panel_take_cfg(&g_panel, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBZ_CFG_KEY, buf, len);
    }
    len = zbz_panel_take_wd(&g_panel, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBW_KEY, buf, len);
    }
    len = zbz_panel_take_ace(&g_panel, buf, sizeof(buf));
    if (len > 0u) {
        kv_put(ZBA_KEY, buf, len);
    }
}

/* ---- set-up and reports ------------------------------------------------------------------------ */

static void radio_state(uint8_t state, uint8_t restarts)
{
    zbz_radio_info_t info;
    memset(&info, 0, sizeof(info));
    info.state = state;
    info.restarts = restarts;
    snprintf(info.version, sizeof(info.version), "%s", "1.2.3");
    zbz_panel_radio(&g_panel, g_now, &info);
}

static void feed(zsrc_zb_report_t *r)
{
    zbz_panel_on_report(&g_panel, g_now, r);
}

static zsrc_zb_report_t rep(uint8_t kind, uint64_t ieee, uint32_t tag)
{
    zsrc_zb_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.ieee = ieee;
    r.tag = tag;
    return r;
}

static void network(uint8_t channel)
{
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_NETWORK, 0u, 0u);
    r.u.network.channel = channel;
    r.u.network.pan_id = 0x1A2Bu;
    r.u.network.ext_pan_id = 0x02ABCD00000000FFull;
    r.u.network.panel_ieee = 0x02ABCD0000000100ull;
    feed(&r);
}

static void bring_up(void)
{
    zsrc_zb_link(&g_src, true, g_now);
    radio_state(UI_ZB_RADIO_WORKING, 0u);
    network(11u);
}

static void fresh(const zsrc_zb_radio_t *radio)
{
    memset(g_calls, 0, sizeof(g_calls));
    g_ncalls = 0u;
    memset(g_refuse, 0, sizeof(g_refuse));
    memset(g_bind, 0, sizeof(g_bind));
    memset(g_bound, 0, sizeof(g_bound));
    g_rebind_refused = false;
    g_rebind_refuse_zone = 0u;
    g_rebind_gated = false;
    g_rebinds = 0u;
    zone_bind_gate_init(&g_gate);
    g_no_enroll_request = false;
    g_fail_kind = 0u;
    g_silent_kind = 0u;
    g_taken = 0u;
    g_changed = 0u;
    g_now = 1000u;
    g_radio = *radio;
    g_sink = (zsrc_sink_t){.ctx = NULL, .input = s_input, .event = s_event, .reachable = s_reach,
                           .link = s_link};
    g_port = (zbz_panel_port_t){.ctx = NULL, .binding = p_binding, .rebind = p_rebind,
                                .wall_clock = p_wall, .changed = p_changed,
                                .devices_taken = p_taken, .replace_waits = g_replace_waits};
    zsrc_zb_init(&g_src, &g_radio, &g_sink);
    zbz_panel_init(&g_panel, &g_port, &g_src, &g_radio, NULL, NULL);
}

void setUp(void)
{
    memset(g_kv, 0, sizeof(g_kv)); /* the store outlives fresh(): a restart keeps it */
    fresh(&RADIO);
    bring_up();
}

void tearDown(void) {}

static void join(uint64_t ieee, uint16_t nwk, uint8_t how, bool rejoin)
{
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_JOINED, ieee, 0u);
    r.nwk = nwk;
    r.u.joined.join = how;
    r.u.joined.rejoin = rejoin;
    feed(&r);
}

/* A fresh join told from its announce, its own key exchange maybe still running (B021). */
static void join_key_pending(uint64_t ieee, uint16_t nwk)
{
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_JOINED, ieee, 0u);
    r.nwk = nwk;
    r.u.joined.join = ZSRC_ZB_JOIN_STANDARD_KEY;
    r.u.joined.key_pending = true;
    feed(&r);
}

static void done(uint64_t ieee, uint32_t tag, uint8_t result)
{
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_DONE, ieee, tag);
    r.u.done.result = result;
    feed(&r);
}

static void frame(uint64_t ieee, uint8_t lqi, int8_t rssi)
{
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_LINK, ieee, 0u);
    r.u.link.lqi = lqi;
    r.u.link.rssi = rssi;
    feed(&r);
}

static void advance(uint64_t ms)
{
    g_now += ms;
    zbz_panel_poll(&g_panel, g_now);
}

static size_t count(uint8_t kind)
{
    size_t n = 0u;
    for (size_t i = 0; i < g_ncalls; i++) {
        n += g_calls[i].kind == kind ? 1u : 0u;
    }
    return n;
}

static call_t *last(uint8_t kind)
{
    for (size_t i = g_ncalls; i > 0u; i--) {
        if (g_calls[i - 1u].kind == kind) {
            return &g_calls[i - 1u];
        }
    }
    return NULL;
}

static ui_zb_req_result_t ask(uint8_t kind, uint16_t id, bool allowed)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = 1u;
    req.id = id;
    return zbz_panel_request(&g_panel, g_now, &req, allowed);
}

static ui_zb_progress_t progress(void)
{
    ui_zb_progress_t g;
    zbz_panel_progress(&g_panel, g_now, &g);
    return g;
}

static const zbz_device_t *dev(uint64_t ieee)
{
    return zbz_reg_find(&g_panel.reg, ieee);
}

static const zbsig_t *sig(const char *file)
{
    for (size_t i = 0; i < sizeof(ZB_SIGNATURES) / sizeof(ZB_SIGNATURES[0]); i++) {
        if (strcmp(ZB_SIGNATURES[i].file, file) == 0) {
            return &ZB_SIGNATURES[i];
        }
    }
    TEST_FAIL_MESSAGE(file);
    return NULL;
}

/* ---- a device answering from its signature row -------------------------------------------------- */


static bool setup_kind(uint8_t k)
{
    return k == C_NODE || k == C_POWER || k == C_ACTIVE || k == C_SIMPLE || k == C_READ ||
           k == C_BIND || k == C_REPORT || k == C_CIE || k == C_ENROLL;
}

static const zbsig_ep_t *sig_ep(const zbsig_t *s, uint8_t ep)
{
    for (uint8_t k = 0; k < s->ep_count; k++) {
        if (s->ep[k].endpoint == ep) {
            return &s->ep[k];
        }
    }
    return NULL;
}

static void answer(const zbsig_t *s, call_t *c)
{
    c->answered = true;
    if (c->kind == g_silent_kind) {
        return;
    }
    if (c->kind == g_fail_kind && g_fail_left != 0) {
        if (g_fail_left > 0) {
            g_fail_left--;
        }
        done(s->ieee, c->tag, g_fail_result);
        return;
    }
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_NONE, s->ieee, c->tag);
    r.nwk = s->nwk;
    switch (c->kind) {
    case C_NODE:
        r.kind = ZSRC_ZB_REP_NODE_DESC;
        r.u.node = s->node;
        feed(&r);
        break;
    case C_POWER:
        r.kind = ZSRC_ZB_REP_POWER_DESC;
        r.u.power.current_source = s->power_source == 1u ? 0x01u : 0x04u;
        feed(&r);
        break;
    case C_ACTIVE:
        r.kind = ZSRC_ZB_REP_ACTIVE_EP;
        r.u.active.count = s->ep_count;
        for (uint8_t k = 0; k < s->ep_count; k++) {
            r.u.active.ep[k] = s->ep[k].endpoint;
        }
        feed(&r);
        break;
    case C_SIMPLE: {
        const zbsig_ep_t *e = sig_ep(s, c->ep);
        TEST_ASSERT_NOT_NULL(e);
        r.kind = ZSRC_ZB_REP_SIMPLE_DESC;
        r.endpoint = c->ep;
        r.u.simple.endpoint = e->endpoint;
        r.u.simple.profile = e->profile;
        r.u.simple.device_id = e->device_id;
        r.u.simple.in_count = e->in_count;
        r.u.simple.out_count = e->out_count;
        memcpy(r.u.simple.in, e->in, sizeof(r.u.simple.in));
        memcpy(r.u.simple.out, e->out, sizeof(r.u.simple.out));
        feed(&r);
        break;
    }
    case C_READ:
        if (c->cluster == ZSRC_ZB_ZCL_BASIC) {
            r.kind = ZSRC_ZB_REP_BASIC;
            r.endpoint = c->ep;
            r.u.basic.have = ZSRC_ZB_BASIC_MANUFACTURER | ZSRC_ZB_BASIC_MODEL |
                             ZSRC_ZB_BASIC_POWER | ZSRC_ZB_BASIC_SW_BUILD;
            snprintf(r.u.basic.manufacturer, sizeof(r.u.basic.manufacturer), "%s",
                     s->manufacturer);
            snprintf(r.u.basic.model, sizeof(r.u.basic.model), "%s", s->model);
            snprintf(r.u.basic.sw_build, sizeof(r.u.basic.sw_build), "%s", "2.1.0");
            r.u.basic.power_source = s->power_source;
            feed(&r);
        } else if (c->cluster == ZSRC_ZB_ZCL_IAS_ZONE) {
            /* The zone type read back: the radio side describes the endpoint again with it
             * (radio_port.c's attribute(), EV_DEVICE), where the source keeps a slot. */
            const zbsig_ep_t *e = sig_ep(s, c->ep);
            const zsrc_zb_device_t *slot = zsrc_zb_device(&g_src, s->ieee, c->ep);
            if (e != NULL && slot != NULL) {
                zsrc_zb_desc_t desc;
                memset(&desc, 0, sizeof(desc));
                desc.ieee = slot->ieee;
                desc.nwk = slot->nwk;
                desc.endpoint = slot->endpoint;
                desc.clusters = slot->clusters;
                desc.zone_type = e->zone_type;
                desc.router = slot->router;
                memcpy(desc.manufacturer, slot->manufacturer, sizeof(desc.manufacturer));
                memcpy(desc.model, slot->model, sizeof(desc.model));
                desc.join = slot->join;
                TEST_ASSERT_TRUE(zsrc_zb_device_seen(&g_src, &desc, g_now));
            }
            /* A zone status read back goes to the source as a status heard (radio_port.c's
             * attribute(), EV_IAS -> zsrc_zb_ias_status), as on the panel. */
            if (c->status_attr) {
                zsrc_zb_ias_status(&g_src, s->ieee, c->ep, 0u, g_now);
            }
        }
        break;
    case C_CIE: {
        /* A sensor asks to enroll once it holds the CIE address (as the radio side feeds it). */
        const zbsig_ep_t *e = sig_ep(s, c->ep);
        if (s->input_count > 0u && s->enroll == ZBSIG_ENROLL_STANDARD && e != NULL &&
            !g_no_enroll_request) {
            zsrc_zb_ias_enroll_request(&g_src, s->ieee, c->ep, e->zone_type, g_now);
            frame(s->ieee, 0u, 0);
        }
        return;
    }
    case C_ENROLL:
        return;
    default:
        break;
    }
    done(s->ieee, c->tag, ZSRC_ZB_DONE_OK);
}

static call_t *next_unanswered(uint64_t ieee)
{
    for (size_t i = 0; i < g_ncalls; i++) {
        if (!g_calls[i].answered && setup_kind(g_calls[i].kind) && g_calls[i].ieee == ieee) {
            return &g_calls[i];
        }
    }
    return NULL;
}

/* The device answers everything until its setup is over (time passes when nothing is due). */
static void run_setup(const zbsig_t *s)
{
    for (int guard = 0; guard < 4000; guard++) {
        const zbz_device_t *d = dev(s->ieee);
        if (d == NULL || (d->setup != ZBZ_SETUP_RUNNING && d->setup != ZBZ_SETUP_WAKE)) {
            return;
        }
        call_t *c = next_unanswered(s->ieee);
        if (c != NULL) {
            answer(s, c);
        } else {
            advance(1000u);
        }
    }
    TEST_FAIL_MESSAGE("the setup never ended");
}

/* ADD, the device joins and is set up, the window is stopped and the result dismissed. */
static uint16_t add_device(const zbsig_t *s)
{
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(s);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, true));
    const zbz_device_t *d = dev(s->ieee);
    TEST_ASSERT_NOT_NULL(d);
    return d->id;
}

static bool take(zbz_log_t *out)
{
    return zbz_panel_take_log(&g_panel, out);
}

static void drain_log(void)
{
    zbz_log_t l;
    while (take(&l)) {
    }
}

/* ==== the setup (devices.md §5; ZB-012) ======================================================== */

/* ZB-012, ZB-021, ZB-002: a plain IAS contact joins during Add with the standard key, and its
 * setup runs every step in order - node descriptor, endpoints, its one descriptor, Basic (its
 * power source known, so no power descriptor), IAS Zone, the CIE address, its enrollment, its
 * battery and check-in bound, battery reporting, its check-in interval - and ends READY, ADDED. */
static void test_setup_plain_ias_contact(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_LEARN_S, last(C_PERMIT)->value);
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(1u, g.joined_count);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_READING, g.joined[0].step);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, dev(s->ieee)->setup);
    /* the source holds its own enrollment while the setup runs */
    TEST_ASSERT_EQUAL_UINT64(s->ieee, g_src.setup[0]);

    const size_t first = g_ncalls - 1u;
    TEST_ASSERT_EQUAL_UINT8(C_NODE, g_calls[first].kind);
    run_setup(s);

    /* the calls the setup made, in order */
    static const struct {
        uint8_t kind;
        uint16_t cluster;
    } WANT[] = {
        {C_NODE, 0u}, {C_ACTIVE, 0u}, {C_SIMPLE, 0u},
        /* Basic in three reads: the manufacturer, the model, the rest (devices.md §5) */
        {C_READ, ZSRC_ZB_ZCL_BASIC}, {C_READ, ZSRC_ZB_ZCL_BASIC}, {C_READ, ZSRC_ZB_ZCL_BASIC},
        {C_READ, ZSRC_ZB_ZCL_IAS_ZONE}, {C_CIE, ZSRC_ZB_ZCL_IAS_ZONE},
        {C_ENROLL, ZSRC_ZB_ZCL_IAS_ZONE}, {C_ENROLL, ZSRC_ZB_ZCL_IAS_ZONE},
        {C_BIND, ZSRC_ZB_ZCL_POWER_CFG}, {C_BIND, ZSRC_ZB_ZCL_POLL_CTRL},
        {C_REPORT, ZSRC_ZB_ZCL_POWER_CFG}, {C_READ, ZSRC_ZB_ZCL_POLL_CTRL},
    };
    size_t k = 0u;
    for (size_t i = first; i < g_ncalls; i++) {
        if (g_calls[i].ieee != s->ieee) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(k < sizeof(WANT) / sizeof(WANT[0]), "an extra call");
        TEST_ASSERT_EQUAL_UINT8(WANT[k].kind, g_calls[i].kind);
        if (WANT[k].cluster != 0u) {
            TEST_ASSERT_EQUAL_HEX16(WANT[k].cluster, g_calls[i].cluster);
        }
        /* ZB-012: the panel's tags never carry the source's bit */
        TEST_ASSERT_TRUE(g_calls[i].tag == 0u || (g_calls[i].tag & ZBZ_TAG_MASK) == ZBZ_TAG);
        TEST_ASSERT_EQUAL_UINT32(0u, g_calls[i].tag & ZSRC_ZB_TAG_SOURCE);
        k++;
    }
    TEST_ASSERT_EQUAL_size_t(sizeof(WANT) / sizeof(WANT[0]), k);
    TEST_ASSERT_EQUAL_UINT32(0x0021u, last(C_REPORT)->attr0); /* battery percentage */
    for (size_t i = first; i < g_ncalls; i++) {
        TEST_ASSERT_FALSE_MESSAGE(g_calls[i].status_attr, "the setup reads no zone status");
    }
    /* the Enroll Response carries the zone id the source gave its endpoint */
    const zsrc_zb_device_t *slot = zsrc_zb_device(&g_src, s->ieee, 1u);
    TEST_ASSERT_NOT_NULL(slot);
    TEST_ASSERT_EQUAL_UINT32(slot->zone_id, last(C_ENROLL)->value);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_ENROLL_DONE, slot->enroll);
    TEST_ASSERT_EQUAL_UINT64(0u, g_src.setup[0]); /* the setup is over for the source */

    const zbz_device_t *d = dev(s->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_DONE, d->setup_step);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, d->role);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, d->join);
    TEST_ASSERT_EQUAL_UINT32(WALL, d->joined_at);
    TEST_ASSERT_EQUAL_STRING("SONOFF", d->manufacturer);
    TEST_ASSERT_EQUAL_UINT16(4742u, d->manufacturer_code);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, d->ep[0].zone_type);

    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step); /* the window stays open */
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[0].step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.joined[0].result);
    TEST_ASSERT_EQUAL_STRING("Door sensor", g.joined[0].model);
    /* the window ends: the operation with it, ADDED */
    advance(ZSRC_ZB_LEARN_S * 1000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.result);
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);

    /* ZB-019 (the log's record of it), ZB-021: how it joined */
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_ADDED, l.type);
    TEST_ASSERT_EQUAL_UINT16(d->id, l.device);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, l.arg);
    TEST_ASSERT_EQUAL_UINT8(1u, l.user);
    TEST_ASSERT_FALSE(take(&l));
}

/* ZB-012, ZB-003, ZB-007: a sensor that never asks to enroll and waits for its Enroll Response
 * before it sends Zone Status Change Notifications still gets one, even when a status of it was
 * heard during the setup (a zone status read back, which the source takes as working): only the
 * NO_ENROLL quirk or its own Enroll Request answered ends the ENROLL step early. The unasked
 * responses go every ZSRC_ZB_ENROLL_GAP_MS, at most ZSRC_ZB_ENROLL_TRIES, then the setup goes
 * on and the source chases it. */
static void test_setup_enrolls_a_sensor_heard_before(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    g_no_enroll_request = true;
    const size_t before = count(C_ENROLL);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    for (int guard = 0; guard < 200 && dev(s->ieee)->setup_step < ZSRC_ZB_STEP_CIE_WRITE;
         guard++) {
        call_t *c = next_unanswered(s->ieee);
        TEST_ASSERT_NOT_NULL(c);
        answer(s, c);
    }
    /* what the radio side does with a read zone status: the source has a status heard */
    zsrc_zb_ias_status(&g_src, s->ieee, 1u, 0u, g_now);
    const zsrc_zb_device_t *slot = zsrc_zb_device(&g_src, s->ieee, 1u);
    TEST_ASSERT_NOT_NULL(slot);
    TEST_ASSERT_TRUE(slot->status_known);
    run_setup(s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, dev(s->ieee)->setup);
    TEST_ASSERT_EQUAL_size_t(before + ZSRC_ZB_ENROLL_TRIES, count(C_ENROLL));
    TEST_ASSERT_EQUAL_UINT32(slot->zone_id, last(C_ENROLL)->value);

    /* the NO_ENROLL quirk alone skips it (a model zb_quirks.c lists with it) */
    zbsig_t nq = *sig("third_reality_3rds17bz.json");
    nq.ieee = 0x02ABCD0000000077ull;
    nq.manufacturer = "eWeLink";
    nq.model = "DS01";
    const size_t before_q = count(C_ENROLL);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, progress().step); /* the same window */
    join(nq.ieee, nq.nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(&nq);
    const zsrc_zb_device_t *ns = zsrc_zb_device(&g_src, nq.ieee, 1u);
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_NOT_EQUAL(0u, ns->quirks & ZSRC_ZB_QUIRK_NO_ENROLL);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, dev(nq.ieee)->setup);
    TEST_ASSERT_EQUAL_size_t(before_q, count(C_ENROLL));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_ENROLL_DONE, ns->enroll);
}

/* ZB-012, ZB-005: a sleepy device that stops answering mid-setup waits for its wake window - the
 * page says WAKE ("Press its button to wake it"), nothing is sent until its next frame (a Poll
 * Control check-in), then the step goes again; after ZBZ_WAKE_WAIT_MS without a frame the step
 * is tried anyway; after ZBZ_SETUP_MAX_MS the setup STOPS. */
static void test_setup_sleepy_waits_for_its_wake(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    answer(s, last(C_NODE));                 /* now known as a sleepy end device */
    answer(s, last(C_ACTIVE));
    call_t *simple = last(C_SIMPLE);
    simple->answered = true;
    done(s->ieee, simple->tag, ZSRC_ZB_DONE_TIMEOUT); /* asleep */
    const uint16_t id = dev(s->ieee)->id;
    TEST_ASSERT_TRUE(g_panel.live[0].asleep);
    ui_zb_device_t rows[4];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_devices(&g_panel, g_now, rows, 4u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_WAKE_NEEDED, rows[0].state);
    TEST_ASSERT_TRUE(progress().joined[0].asleep);
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_TRUE(page.sleepy);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_SIMPLE_DESC, page.setup_step);

    const size_t sent = count(C_SIMPLE);
    advance(60000u);
    TEST_ASSERT_EQUAL_size_t(sent, count(C_SIMPLE)); /* nothing while it sleeps */
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_CHECK_IN, s->ieee, 0u);
    feed(&r);                                        /* it woke: the step at once */
    TEST_ASSERT_EQUAL_size_t(sent + 1u, count(C_SIMPLE));
    TEST_ASSERT_FALSE(g_panel.live[0].asleep);
    TEST_ASSERT_FALSE(progress().joined[0].asleep);

    /* asleep again; no frame for ZBZ_WAKE_WAIT_MS: tried anyway */
    simple = last(C_SIMPLE);
    simple->answered = true;
    done(s->ieee, simple->tag, ZSRC_ZB_DONE_TIMEOUT);
    advance(ZBZ_WAKE_WAIT_MS - 1000u);
    TEST_ASSERT_EQUAL_size_t(sent + 1u, count(C_SIMPLE));
    advance(1000u);
    TEST_ASSERT_EQUAL_size_t(sent + 2u, count(C_SIMPLE));

    /* and it never wakes: ZBZ_SETUP_MAX_MS after the setup began, it STOPS at its step */
    for (int i = 0; i < 6 && dev(s->ieee)->setup != ZBZ_SETUP_STOPPED; i++) {
        call_t *c = last(C_SIMPLE);
        if (!c->answered) {
            c->answered = true;
            done(s->ieee, c->tag, ZSRC_ZB_DONE_TIMEOUT);
        }
        advance(ZBZ_WAKE_WAIT_MS);
    }
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, dev(s->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_SIMPLE_DESC, dev(s->ieee)->setup_step);
    TEST_ASSERT_EQUAL_UINT64(0u, g_src.setup[0]);
}

/* ZB-012: a router (a mains siren) that doesn't answer gets ZBZ_STEP_MS and ZBZ_SETUP_TRIES
 * tries per step - a request with no DONE at all times out by the panel's own deadline, a
 * refusal counts as a try - then the setup STOPS at that step ("Set it up again" goes on from
 * there, and finishes: a siren is NOT_A_SENSOR, its own page instead). */
static void test_setup_router_retries_then_stops(void)
{
    const zbsig_t *s = sig("tuya_ts0216.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    answer(s, last(C_NODE));
    TEST_ASSERT_TRUE(zbz_dev_router(dev(s->ieee)));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_ACTIVE));
    last(C_ACTIVE)->answered = true;
    advance(ZBZ_STEP_MS - 1u);
    TEST_ASSERT_EQUAL_size_t(1u, count(C_ACTIVE));
    advance(1u); /* no DONE in ZBZ_STEP_MS: a try, and again */
    TEST_ASSERT_EQUAL_size_t(2u, count(C_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(1u, g_panel.live[0].tries);
    last(C_ACTIVE)->answered = true;
    done(s->ieee, last(C_ACTIVE)->tag, ZSRC_ZB_DONE_REFUSED);
    TEST_ASSERT_EQUAL_size_t(3u, count(C_ACTIVE));
    last(C_ACTIVE)->answered = true;
    done(s->ieee, last(C_ACTIVE)->tag, ZSRC_ZB_DONE_TIMEOUT);
    TEST_ASSERT_EQUAL_size_t(3u, count(C_ACTIVE)); /* ZBZ_SETUP_TRIES: given up */
    const zbz_device_t *d = dev(s->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_ACTIVE_EP, d->setup_step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_SETUP_NOT_FINISHED, progress().joined[0].result);
    ui_zb_device_t rows[2];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_devices(&g_panel, g_now, rows, 2u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_SETUP_STOPPED, rows[0].state);
    TEST_ASSERT_EQUAL_UINT8(1u, rows[0].setup_done); /* NODE_DESC of SETUP_STEPS */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_DONE - 1u, rows[0].setup_total);

    /* "Set it up again": from ACTIVE_EP, not from the start */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    const size_t nodes = count(C_NODE);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, d->id, true));
    TEST_ASSERT_EQUAL_size_t(nodes, count(C_NODE));
    TEST_ASSERT_EQUAL_size_t(4u, count(C_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_SETUP, progress().op);
    run_setup(s);
    d = dev(s->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SIREN, d->role);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NOT_A_SENSOR, g.result);
    /* a siren is no zone input: the source keeps no slot for it */
    TEST_ASSERT_NULL(zsrc_zb_device(&g_src, s->ieee, 1u));
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, d->id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_AVAILABLE, page.siren.fit);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_NONE, page.keypad.fit);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SIREN, page.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_POWER_MAINS, page.dev.power);
}

/* ZB-012: a device with nothing the panel uses (a manufacturer cluster only) ends NOT_USABLE,
 * "Not a sensor", decided by its clusters, not its brand. */
static void test_setup_not_a_sensor(void)
{
    const zbsig_t *s = sig("tuya_ts0601_siren.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(s);
    const zbz_device_t *d = dev(s->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_NOT_USABLE, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_REPEATER, d->role);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NOT_A_SENSOR, progress().joined[0].result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_STOPPED, g.result); /* Stop says what it did */
    ui_zb_device_t rows[2];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_devices(&g_panel, g_now, rows, 2u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_NOT_USABLE, rows[0].state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_REPEATER, rows[0].kind);
    TEST_ASSERT_TRUE(rows[0].router);
}

/* ZB-012: a failure that stops the setup keeps its reason - the step it stopped at - and a
 * non-success other than UNSUPPORTED counts as a try; UNSUPPORTED is not a failure. */
static void test_setup_failure_stops_with_its_step(void)
{
    zbsig_t s = *sig("sonoff_snzb_02d.json");
    s.ieee = 0x02ABCD0000000077ull;
    s.node.logical_type = ZSRC_ZB_LOGICAL_ROUTER; /* a listening temperature sensor */
    s.node.mac_caps = 0x8Eu;
    s.power_source = 1u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s.ieee, s.nwk, ZSRC_ZB_JOIN_INSTALL_CODE, false);
    g_fail_kind = C_REPORT;
    g_fail_result = ZSRC_ZB_DONE_UNSUPPORTED;
    g_fail_left = -1;
    g_silent_kind = 0u;
    /* REPORTING unsupported everywhere: not a failure; BIND refused: three tries, then STOPPED */
    for (int guard = 0; guard < 200; guard++) {
        call_t *c = next_unanswered(s.ieee);
        if (c == NULL) {
            break;
        }
        if (c->kind == C_BIND) {
            c->answered = true;
            done(s.ieee, c->tag, ZSRC_ZB_DONE_REFUSED);
        } else {
            answer(&s, c);
        }
    }
    g_fail_kind = 0u;
    const zbz_device_t *d = dev(s.ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_BIND, d->setup_step);
    TEST_ASSERT_EQUAL_size_t(ZBZ_SETUP_TRIES, count(C_BIND));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_SETUP_NOT_FINISHED, progress().joined[0].result);
    /* its inputs were described once its descriptors and Basic were known */
    TEST_ASSERT_NOT_NULL(zsrc_zb_device(&g_src, s.ieee, 1u));

    /* again: binds answer, reporting is UNSUPPORTED - not a failure, the setup finishes */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, d->id, true));
    g_fail_kind = C_REPORT;
    g_fail_result = ZSRC_ZB_DONE_UNSUPPORTED;
    g_fail_left = -1;
    run_setup(&s);
    g_fail_kind = 0u;
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, dev(s.ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, progress().result);
    TEST_ASSERT_TRUE(count(C_REPORT) >= 2u); /* battery and temperature, each once */
}

/* ZB-012: optional steps are given up, not fatal - a node descriptor that never comes (some
 * Aqara devices) and a Basic that keeps failing still end in a working sensor; a request the
 * radio can't even queue is tried again after ZBZ_STEP_MS. */
static void test_setup_retries_and_optional_steps(void)
{
    zbsig_t s = *sig("third_reality_3rds17bz.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    g_refuse[C_NODE] = true;
    join(s.ieee, s.nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    TEST_ASSERT_EQUAL_size_t(1u, count(C_NODE));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_ACTIVE));
    advance(ZBZ_STEP_MS);
    TEST_ASSERT_EQUAL_size_t(2u, count(C_NODE)); /* not queued: again after a while */
    advance(ZBZ_STEP_MS);
    TEST_ASSERT_EQUAL_size_t(3u, count(C_NODE));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_ACTIVE)); /* given up as optional: the next step */
    g_refuse[C_NODE] = false;
    for (size_t i = 0; i < g_ncalls; i++) {
        if (g_calls[i].kind == C_NODE) {
            g_calls[i].answered = true;
        }
    }
    g_fail_kind = C_READ;
    g_fail_result = ZSRC_ZB_DONE_TIMEOUT;
    g_fail_left = (int)ZBZ_SETUP_TRIES; /* Basic three times: given up */
    run_setup(&s);
    g_fail_kind = 0u;
    const zbz_device_t *d = dev(s.ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, d->role);
    TEST_ASSERT_EQUAL_STRING("", d->manufacturer); /* Basic never answered */
    TEST_ASSERT_TRUE(count(C_POWER) >= 1u);         /* so the power descriptor was asked */
}

/* ZB-012 (testing.md §2's converter): each signature row, set up through the scripted device,
 * ends with the role, router and sleepy facts its row expects, and exactly its inputs described
 * to the source - their clusters, zone type and class. */
static void test_setup_of_every_signature(void)
{
    const size_t n = sizeof(ZB_SIGNATURES) / sizeof(ZB_SIGNATURES[0]);
    for (size_t i = 0; i < n; i++) {
        const zbsig_t *s = &ZB_SIGNATURES[i];
        fresh(&RADIO);
        bring_up();
        g_fail_kind = 0u;
        g_silent_kind = 0u;
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
        join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
        run_setup(s);
        const zbz_device_t *d = dev(s->ieee);
        TEST_ASSERT_NOT_NULL_MESSAGE(d, s->file);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->role, d->role, s->file);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->role, zbz_role_of(d), s->file);
        TEST_ASSERT_EQUAL_MESSAGE(s->router, zbz_dev_router(d), s->file);
        TEST_ASSERT_EQUAL_MESSAGE(s->sleepy, zbz_dev_sleepy(d), s->file);
        TEST_ASSERT_TRUE_MESSAGE(d->setup == ZBZ_SETUP_DONE || d->setup == ZBZ_SETUP_NOT_USABLE,
                                 s->file);
        for (uint8_t k = 0; k < s->input_count; k++) {
            const zsrc_zb_device_t *slot = zsrc_zb_device(&g_src, s->ieee, s->input[k].endpoint);
            TEST_ASSERT_NOT_NULL_MESSAGE(slot, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(s->input[k].clusters, slot->clusters, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(s->input[k].zone_type, slot->zone_type, s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->input[k].device_class, slot->device_class,
                                            s->file);
        }
        for (uint8_t e = 0; e < s->ep_count; e++) {
            bool is_input = false;
            for (uint8_t k = 0; k < s->input_count; k++) {
                is_input = is_input || s->input[k].endpoint == s->ep[e].endpoint;
            }
            if (!is_input) {
                TEST_ASSERT_NULL_MESSAGE(zsrc_zb_device(&g_src, s->ieee, s->ep[e].endpoint),
                                         s->file);
            }
        }
        const ui_zb_joined_t *row = &progress().joined[0];
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->role == ZBSIG_ROLE_SENSOR ? UI_ZB_RES_ADDED
                                                                     : UI_ZB_RES_NOT_A_SENSOR,
                                        row->result, s->file);
    }
}

/* ==== operations (devices.md §6; ZB-002, ZB-013..ZB-017) ======================================== */

/* ZB-002: Add opens the 3-minute window, counts it down once a second, Keep listening gives the
 * whole window again, and a window that runs out with nobody is NOTHING_JOINED and closes. */
static void test_add_window_and_countdown(void)
{
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_ADD, g.op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step);
    TEST_ASSERT_EQUAL_UINT32(180u, g.seconds_left);
    g_now += 400u;
    TEST_ASSERT_EQUAL_UINT32(180u, progress().seconds_left); /* not before the second is up */
    g_now += 600u;
    TEST_ASSERT_EQUAL_UINT32(179u, progress().seconds_left);
    ui_zb_status_t st;
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_EQUAL_UINT16(179u, st.learn_s);
    TEST_ASSERT_TRUE(g_src.learning);
    advance(100000u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_KEEP_LISTENING, 0u, true));
    TEST_ASSERT_EQUAL_UINT32(180u, progress().seconds_left);
    TEST_ASSERT_EQUAL_UINT64(g_now + 180000u, zbz_panel_next_deadline(&g_panel));
    advance(179999u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, progress().step);
    advance(1u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NOTHING_JOINED, g.result);
    TEST_ASSERT_EQUAL_UINT32(0u, g.seconds_left);
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);
    TEST_ASSERT_FALSE(g_src.learning);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_KEEP_LISTENING, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, false));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_NONE, progress().op);
}

/* ZB-015, ZB-021: Add with an install code checks the code (length, CRC, address), gives it to
 * the trust center, admits only that device for the window, and forgets the code once its device
 * joined with it; a scanned QR text works the same. */
static void test_add_with_install_code(void)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_ADD_CODE;
    req.user = 1u;
    req.ieee = 0x02ABCD0000000001ull;
    snprintf(req.code, sizeof(req.code), "%s", "83FE D340");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_BAD_CODE, zbz_panel_request(&g_panel, g_now, &req, true));
    snprintf(req.code, sizeof(req.code), "%s", "83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B6");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_CHECKSUM, zbz_panel_request(&g_panel, g_now, &req, true));
    req.ieee = UINT64_MAX;
    snprintf(req.code, sizeof(req.code), "%s", "83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_BAD_ADDRESS, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_IC));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_NONE, progress().op);

    req.ieee = 0u; /* a scanned label */
    snprintf(req.code, sizeof(req.code), "%s",
             "Z:02ABCD0000000001$I:83FED3407A939723A5C639B26916D505C3B5");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD0000000001ull, last(C_IC)->ieee);
    TEST_ASSERT_EQUAL_UINT32(18u, last(C_IC)->value);
    TEST_ASSERT_EQUAL_UINT32(1u, last(C_CODES_ONLY)->value);
    ui_zb_code_t codes[4];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_codes(&g_panel, codes, 4u));
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD0000000001ull, codes[0].ieee);
    TEST_ASSERT_EQUAL_UINT32(WALL, codes[0].added_at);
    TEST_ASSERT_TRUE((zbz_panel_dirty(&g_panel) & ZBZ_DIRTY_CODES) != 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_ADD_CODE, progress().op);

    zbsig_t s = *sig("sonoff_snzb_04pr2.json");
    s.ieee = 0x02ABCD0000000001ull;
    join(s.ieee, s.nwk, ZSRC_ZB_JOIN_INSTALL_CODE, false);
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_CODES_ONLY)->value); /* the window is over */
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_codes(&g_panel, codes, 4u)); /* used: forgotten */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, dev(s.ieee)->join);
    TEST_ASSERT_NOT_EQUAL(UI_ZB_STEP_DONE, progress().step);
    run_setup(&s);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.result);
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_ADDED, l.type);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, l.arg);
}

/* ZB-015: codes kept for later (CODE_ADD / CODE_FORGET), given to the trust center at once and
 * again each time the radio comes up; FULL past ZBZ_CODES_MAX; NOT_FOUND for no such code. */
static void test_install_codes_waiting(void)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CODE_ADD;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_IC));
    for (uint64_t k = 3u; k < 3u + ZBZ_CODES_MAX - 1u; k++) {
        req.ieee = 0x02ABCD0000000000ull + k;
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    }
    req.ieee = 0x02ABCD00000000F0ull;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, zbz_panel_request(&g_panel, g_now, &req, true));
    ui_zb_status_t st;
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_EQUAL_UINT16(ZBZ_CODES_MAX, st.codes_waiting);

    req.kind = UI_ZB_REQ_CODE_FORGET;
    req.ieee = 0x02ABCD0000000002ull;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_IC)->value); /* taken back */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_FOUND, zbz_panel_request(&g_panel, g_now, &req, true));

    /* the radio restarts: every waiting code is given again */
    const size_t before = count(C_IC);
    radio_state(UI_ZB_RADIO_STARTING, 0u);
    radio_state(UI_ZB_RADIO_WORKING, 0u);
    TEST_ASSERT_EQUAL_size_t(before + ZBZ_CODES_MAX - 1u, count(C_IC));
}

/* ZB-013: Remove asks the device to leave; its answer removes it - its zones listed, its slots
 * and record gone, the log says REMOVED with its first zone. */
static void test_remove_answered(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(s);
    bind_zone(3u, s->ieee, 1u);
    drain_log();
    TEST_ASSERT_EQUAL_UINT16(id, zbz_panel_device_of_zone(&g_panel, 3u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REMOVE, id, true));
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, g.step);
    TEST_ASSERT_EQUAL_UINT16(id, g.id);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT16(3u, g.zones[0]);
    TEST_ASSERT_EQUAL_STRING("Door sensor", g.model);
    done(s->ieee, last(C_LEAVE)->tag, ZSRC_ZB_DONE_OK);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_REMOVED, g.result);
    TEST_ASSERT_EQUAL_UINT16(3u, g.zones[0]);
    TEST_ASSERT_NULL(dev(s->ieee));
    TEST_ASSERT_NULL(zsrc_zb_device(&g_src, s->ieee, 1u));
    ui_zb_device_page_t page;
    TEST_ASSERT_FALSE(zbz_panel_device(&g_panel, g_now, id, &page)); /* the page goes back */
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_REMOVED, l.type);
    TEST_ASSERT_EQUAL_UINT16(3u, l.zone);
    TEST_ASSERT_EQUAL_UINT16(id, l.device);
    TEST_ASSERT_EQUAL_UINT8(1u, l.user);
}

/* ZB-010, ZB-013 (devices.md section 4; the review of wave 6): a forgotten device's record is
 * erased; when that erase fails, the owner's redo with the registry's bit (it doesn't say which
 * key) erases it again - before the fix only used slots were redone, the erase was lost and the
 * device came back from the store after a restart. */
static void test_redo_erases_a_dropped_record_again(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    write_back();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REMOVE, id, true));
    advance(ZBZ_LEAVE_MS);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_FORGET, id, true));
    TEST_ASSERT_NULL(dev(s->ieee));
    char key[ZBZ_KEY_LEN];
    char erased[ZBZ_KEY_LEN];
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    size_t len = 0u;
    TEST_ASSERT_EQUAL(ZBZ_TAKE_ERASE, zbz_panel_take_registry(&g_panel, key, buf, sizeof(buf), &len));
    memcpy(erased, key, sizeof(erased));
    TEST_ASSERT_NOT_NULL(kv_find(erased)); /* the store still has it */
    /* the erase failed: asked for again */
    zbz_panel_redo(&g_panel, ZBZ_DIRTY_REGISTRY);
    bool again = false;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&g_panel, key, buf, sizeof(buf), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_ERASE) {
            again = again || strcmp(key, erased) == 0;
            kv_erase(key);
        } else {
            kv_put(key, buf, len);
        }
    }
    TEST_ASSERT_TRUE(again);
    TEST_ASSERT_NULL(kv_find(erased));
}

/* ZB-010 (devices.md section 4; the review of wave 6): a sensor added while the zone source has no
 * endpoint slot left ends its add row FULL, not ADDED - no zone could use it - instead of the
 * source's refusal being dropped silently. */
static void test_a_sensor_the_source_has_no_room_for_is_full(void)
{
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_desc_t d;
        memset(&d, 0, sizeof(d));
        d.ieee = 0x02ABCD0000F00000ull + i;
        d.endpoint = 1u;
        d.clusters = ZSRC_ZB_CL_ON_OFF;
        d.zone_type = ZSRC_ZB_ZONE_TYPE_NONE;
        TEST_ASSERT_TRUE(zsrc_zb_device_seen(&g_src, &d, g_now));
    }
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(s);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(1u, g.joined_count);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[0].step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FULL, g.joined[0].result);
    TEST_ASSERT_NULL(zsrc_zb_device(&g_src, s->ieee, 1u));
}

/* ZB-019: the log says where its user asked - a Forget from the Panel Manager names it, one from
 * the touchscreen names the touchscreen, and what the panel does by itself has no user (BL-151). */
static void test_log_names_where_it_was_asked(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    drain_log();
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_FORGET;
    req.user = 1u;
    req.via = UI_ZB_VIA_WEB;
    req.id = id;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_FORGOTTEN, l.type);
    TEST_ASSERT_EQUAL_UINT8(1u, l.user);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_VIA_WEB, l.via);

    const uint16_t again = add_device(s); /* its ADDED: asked on the touchscreen (ask()) */
    zbz_log_t added;
    bool seen = false;
    while (take(&added)) {
        if (added.type == (uint8_t)ZBZ_EV_ADDED && added.device == again) {
            seen = true;
            TEST_ASSERT_EQUAL_UINT8(1u, added.user);
            TEST_ASSERT_EQUAL_UINT8(UI_ZB_VIA_PANEL, added.via);
        }
    }
    TEST_ASSERT_TRUE(seen);

    /* An operation the Panel Manager started keeps its source to its end (REMOVE: REMOVED). */
    req.kind = UI_ZB_REQ_REMOVE;
    req.id = again;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_VIA_WEB, g_panel.op_via);
}

/* ZB-013: a device that doesn't answer its leave is NO_ANSWER after ZBZ_LEAVE_MS ("Forget it
 * anyway?"); Forget then removes it without its answer - FORGOTTEN, the radio told to forget it. */
static void test_remove_no_answer_then_forget(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    drain_log();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REMOVE, id, true));
    advance(ZBZ_LEAVE_MS - 1u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, progress().step);
    advance(1u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, progress().result);
    TEST_ASSERT_NOT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_FORGET, id, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FORGOTTEN, progress().result);
    TEST_ASSERT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL_UINT64(s->ieee, last(C_FORGET)->ieee);
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_FORGOTTEN, l.type);
    TEST_ASSERT_EQUAL_UINT16(id, l.device);
    /* its traffic doesn't bring it back; only a new join does */
    zsrc_zb_ias_status(&g_src, s->ieee, 1u, ZSRC_ZB_IAS_ALARM1, g_now);
    TEST_ASSERT_NULL(zsrc_zb_device(&g_src, s->ieee, 1u));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, true); /* a rejoin of a forgotten device */
    TEST_ASSERT_NULL(dev(s->ieee));
}

/* ZB-013: Replace listens like Add; the first device set up with an input of the old one's class
 * takes each of its zones (the hub's rebind), then the old one is forgotten: REPLACED. */
static void test_replace(void)
{
    const zbsig_t *old = sig("sonoff_snzb_04pr2.json");
    const zbsig_t *repl = sig("third_reality_3rds17bz.json");
    const zbsig_t *other = sig("sonoff_snzb_02d.json"); /* temperature: not its class */
    const uint16_t old_id = add_device(old);
    bind_zone(3u, old->ieee, 1u);
    drain_log();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_FOUND, ask(UI_ZB_REQ_REPLACE, 999u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_REPLACE, progress().op);
    TEST_ASSERT_EQUAL_UINT16(old_id, progress().old_id);
    join(other->ieee, other->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(other);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, progress().step); /* not its replacement */
    TEST_ASSERT_NOT_NULL(dev(other->ieee));                        /* but added all the same */
    join(repl->ieee, repl->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(repl);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_REPLACED, g.result);
    TEST_ASSERT_EQUAL_UINT16(dev(repl->ieee)->id, g.id);
    TEST_ASSERT_EQUAL_UINT16(old_id, g.old_id);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT16(3u, g.zones[0]);
    TEST_ASSERT_EQUAL_UINT64(repl->ieee, g_bind[3].in[ZSRC_ROLE_MAIN].addr.id);
    TEST_ASSERT_NULL(dev(old->ieee));
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);
    zbz_log_t l;
    bool replaced = false;
    while (take(&l)) {
        if (l.type == ZBZ_EV_REPLACED) {
            replaced = true;
            TEST_ASSERT_EQUAL_UINT16(dev(repl->ieee)->id, l.device);
            TEST_ASSERT_EQUAL_UINT16(3u, l.zone);
        }
    }
    TEST_ASSERT_TRUE(replaced);

    /* the hub refuses (armed): FAILED, and the old device stays */
    const uint16_t keep = dev(repl->ieee)->id;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, true));
    g_rebind_refused = true;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, keep, true));
    zbsig_t again = *old;
    again.ieee = 0x02ABCD0000000099ull;
    join(again.ieee, again.nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(&again);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
    TEST_ASSERT_NOT_NULL(zbz_reg_cget(&g_panel.reg, keep));
}

/* ZB-013: Replace moves every zone or none - the hub refusing a later zone (its partition armed)
 * puts back the ones already moved, so the old device's zones never end up split between two
 * sensors: FAILED, the old device kept, and every zone still bound to it as before. */
static void test_replace_refused_moves_nothing(void)
{
    const zbsig_t *old = sig("sonoff_snzb_04pr2.json");
    const zbsig_t *repl = sig("third_reality_3rds17bz.json");
    const uint16_t old_id = add_device(old);
    bind_zone(3u, old->ieee, 1u);
    bind_zone(5u, old->ieee, 1u);
    g_bind[5].in[ZSRC_ROLE_TAMPER].addr = zsrc_zb_address(old->ieee, 1u);
    const zone_hub_binding_t keep3 = g_bind[3], keep5 = g_bind[5];
    g_rebind_refuse_zone = 5u;
    drain_log();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id, true));
    join(repl->ieee, repl->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(repl);
    const ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, g.result);
    TEST_ASSERT_EQUAL_UINT8(0u, g.zone_count);
    TEST_ASSERT_NOT_NULL(dev(old->ieee));
    TEST_ASSERT_EQUAL_UINT16(old_id, dev(old->ieee)->id);
    TEST_ASSERT_EQUAL_MEMORY(&keep3, &g_bind[3], sizeof(keep3)); /* moved, then put back */
    TEST_ASSERT_EQUAL_MEMORY(&keep5, &g_bind[5], sizeof(keep5)); /* refused: untouched */
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);
    zbz_log_t l;
    while (take(&l)) {
        TEST_ASSERT_NOT_EQUAL(ZBZ_EV_REPLACED, l.type);
    }
}

/* ZB-014: Check it now - a listening device is read (Basic's ZCL version) and ANSWERED or
 * NO_ANSWER; a sleepy one waits for its next frame. It doesn't read `allowed`. */
static void test_ping(void)
{
    const zbsig_t *router = sig("tuya_ts0216.json");
    const uint16_t rid = add_device(router);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, rid, false));
    call_t *c = last(C_READ);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_BASIC, c->cluster);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, c->attr0);
    TEST_ASSERT_EQUAL_UINT32(1u, c->value);
    done(router->ieee, c->tag, ZSRC_ZB_DONE_OK);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ANSWERED, progress().result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, rid, false));
    advance(ZBZ_PING_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, progress().result);

    const zbsig_t *sleepy = sig("sonoff_snzb_04pr2.json");
    const uint16_t sid = add_device(sleepy);
    const size_t reads = count(C_READ);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, sid, false));
    TEST_ASSERT_EQUAL_size_t(reads, count(C_READ)); /* nothing sent: it is asleep */
    advance(60000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, progress().step);
    frame(sleepy->ieee, 120u, -70);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ANSWERED, progress().result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_PING, sid, false));
    advance(ZBZ_WAKE_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, progress().result);
}

/* ZB-014: Identify blinks the device's own light for ZBZ_IDENTIFY_S (never the panel's screen):
 * DONE when it answers, FAILED when not; a device without Identify is refused. */
static void test_identify(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(s);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_IDENTIFY, id, false));
    TEST_ASSERT_EQUAL_UINT32(ZBZ_IDENTIFY_S, last(C_IDENTIFY)->value);
    done(s->ieee, last(C_IDENTIFY)->tag, ZSRC_ZB_DONE_OK);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_IDENTIFY, id, false));
    done(s->ieee, last(C_IDENTIFY)->tag, ZSRC_ZB_DONE_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
    const uint16_t plain = add_device(sig("third_reality_3rds17bz.json")); /* no Identify */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_IDENTIFY, plain, false));
}

/* ZB-016: the survey (not while devices may join) and the channel move: 11/15/20/25 only, stored
 * as the installer's choice (cfg), logged with the channel. */
static void test_survey_and_channel(void)
{
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SURVEY, 0u, false));
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_SURVEY, 0u, 0u);
    r.u.survey.known = 0x0Fu;
    r.u.survey.dbm[0] = -60;
    r.u.survey.dbm[1] = -85;
    r.u.survey.dbm[2] = -70;
    r.u.survey.dbm[3] = -90;
    feed(&r);
    done(0u, last(C_SURVEY)->tag, ZSRC_ZB_DONE_OK);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    ui_zb_status_t st;
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_EQUAL_UINT8(0x0Fu, st.survey_known);
    TEST_ASSERT_EQUAL_INT8(-85, st.survey_dbm[1]);
    TEST_ASSERT_EQUAL_UINT8(25u, st.quietest);
    TEST_ASSERT_EQUAL_UINT8(11u, st.channel);
    TEST_ASSERT_EQUAL_UINT16(0x1A2Bu, st.pan_id);
    TEST_ASSERT_EQUAL_STRING("1.2.3", st.radio_version);
    /* not while the window is open, the zone editor's included */
    TEST_ASSERT_TRUE(zsrc_zb_ops()->learn(&g_src, true, g_now));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_BUSY, ask(UI_ZB_REQ_SURVEY, 0u, false));
    TEST_ASSERT_TRUE(zsrc_zb_ops()->learn(&g_src, false, g_now));

    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CHANNEL;
    req.user = 1u;
    req.channel = 12u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, zbz_panel_request(&g_panel, g_now, &req, true));
    req.channel = 25u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_UINT32(25u, last(C_CHANNEL)->value);
    TEST_ASSERT_EQUAL_UINT8(0u, zbz_panel_dirty(&g_panel) & ZBZ_DIRTY_CFG);
    done(0u, last(C_CHANNEL)->tag, ZSRC_ZB_DONE_OK);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_size_t(4u, zbz_panel_take_cfg(&g_panel, buf, sizeof(buf)));
    const uint8_t want[] = {0x01, 0x01, 25u, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, 4u);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_take_cfg(&g_panel, buf, sizeof(buf)));
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_CHANNEL, l.type);
    TEST_ASSERT_EQUAL_UINT8(25u, l.arg);
    /* a move that times out fails and stores nothing */
    req.channel = 15u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    advance(ZBZ_CHANNEL_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
    TEST_ASSERT_EQUAL_UINT8(25u, g_panel.cfg.channel);
}

/* ZB-017: the network reset (its installer code checked by the caller) empties the registry,
 * keeps the waiting codes, the zones' devices go, and it is logged; the code's answer maps to
 * WRONG_CODE or LOCKED_OUT. */
static void test_network_reset(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    (void)add_device(s);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CODE_ADD;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    drain_log();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_RESET, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, progress().step);
    done(0u, last(C_RESET)->tag, ZSRC_ZB_DONE_OK);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_reg_count(&g_panel.reg));
    TEST_ASSERT_NULL(zsrc_zb_device(&g_src, s->ieee, 1u));
    ui_zb_code_t codes[2];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_codes(&g_panel, codes, 2u));
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_NETWORK_RESET, l.type);
    TEST_ASSERT_EQUAL_UINT8(1u, l.user);
    /* a reset that never finishes fails */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_RESET, 0u, true));
    advance(ZBZ_RESET_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);

    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_reset_code_result(ALARM_OK, ALARM_AUTH_INSTALLER));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_WRONG_CODE, zbz_reset_code_result(ALARM_OK, ALARM_AUTH_MASTER));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_WRONG_CODE, zbz_reset_code_result(ALARM_OK, ALARM_AUTH_STANDARD));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_WRONG_CODE,
                      zbz_reset_code_result(ALARM_ERR_INVALID_CODE, ALARM_AUTH_INSTALLER));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_WRONG_CODE,
                      zbz_reset_code_result(ALARM_ERR_CODE_REQUIRED, ALARM_AUTH_NONE));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_LOCKED_OUT,
                      zbz_reset_code_result(ALARM_ERR_LOCKED_OUT, ALARM_AUTH_INSTALLER));
}

/* ZB-018: Restart the radio ends when it works again (logged "asked"); a restart by itself and
 * the guard's pause are logged with their own arg; Try again only while paused. */
static void test_restart_and_retry(void)
{
    drain_log();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_RETRY, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_RESTART, 0u, true));
    radio_state(UI_ZB_RADIO_WORKING, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, progress().step); /* not down yet */
    radio_state(UI_ZB_RADIO_STARTING, 0u);
    radio_state(UI_ZB_RADIO_WORKING, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_RADIO_RESTARTED, l.type);
    TEST_ASSERT_EQUAL_UINT8(0u, l.arg);
    TEST_ASSERT_EQUAL_UINT8(1u, l.user);
    radio_state(UI_ZB_RADIO_STARTING, 1u);
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(1u, l.arg); /* by itself */
    radio_state(UI_ZB_RADIO_PAUSED, 2u);
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(1u, l.arg);
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(2u, l.arg); /* the guard paused it */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_RETRY, 0u, true));
    advance(ZBZ_RESET_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
}

/* ZB-013, ZB-027: a device that leaves by itself is logged LEFT (user 0) and keeps its record; one
 * that leaves during its setup is JOIN_FAILED on the add page; a stranger turned away is logged
 * once an hour per address. */
static void test_left_and_refused(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    bind_zone(5u, s->ieee, 1u);
    drain_log();
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_LEFT, s->ieee, 0u);
    feed(&r);
    zbz_log_t l;
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_LEFT, l.type);
    TEST_ASSERT_EQUAL_UINT16(5u, l.zone);
    TEST_ASSERT_EQUAL_UINT16(id, l.device);
    TEST_ASSERT_EQUAL_UINT8(0u, l.user);
    TEST_ASSERT_NOT_NULL(dev(s->ieee));

    const zbsig_t *t = sig("sonoff_snzb_04pr2.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(t->ieee, t->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    r = rep(ZSRC_ZB_REP_LEFT, t->ieee, 0u);
    feed(&r);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_JOIN_FAILED, progress().joined[0].result);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, dev(t->ieee)->setup);
    drain_log();

    r = rep(ZSRC_ZB_REP_JOIN_REFUSED, 0x02ABCD00000000AAull, 0u);
    r.u.refused.why = 1u;
    feed(&r);
    g_now += ZBZ_REFUSED_LOG_MS - 1u;
    feed(&r);
    TEST_ASSERT_TRUE(take(&l));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_JOIN_REFUSED, l.type);
    TEST_ASSERT_EQUAL_UINT8(1u, l.arg);
    TEST_ASSERT_FALSE(take(&l)); /* once an hour */
    g_now += 1u;
    feed(&r);
    TEST_ASSERT_TRUE(take(&l));
}

/* ZB-012 / B021: a device that has just joined may run its own key exchange, polling for the
 * trust center's answers; the setup's first request waits ZBZ_KEY_WAIT_MS for it, or less if the
 * trust center saw the exchange end (AUTHORIZED). A device that leaves within the wait (its
 * exchange failed) is JOIN_FAILED, and waited for again when it joins again. A join the stack
 * authorized first, or a rejoin, doesn't wait. */
static void test_setup_waits_for_the_key_exchange(void)
{
    const zbsig_t *s = sig("sonoff_snzb_02d.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join_key_pending(s->ieee, s->nwk);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, dev(s->ieee)->setup);
    TEST_ASSERT_EQUAL_size_t(0u, count(C_NODE));
    TEST_ASSERT_TRUE(zbz_panel_next_deadline(&g_panel) <= g_now + ZBZ_KEY_WAIT_MS);
    advance(ZBZ_KEY_WAIT_MS - 1000u);
    TEST_ASSERT_EQUAL_size_t(0u, count(C_NODE));
    advance(1000u);
    TEST_ASSERT_EQUAL_size_t(1u, count(C_NODE)); /* no word from the stack: it goes on */
    run_setup(s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, dev(s->ieee)->setup);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, true));

    /* the trust center saw the exchange end: at once */
    const zbsig_t *c = sig("sonoff_snzb_04pr2.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join_key_pending(c->ieee, c->nwk);
    const size_t nodes = count(C_NODE);
    advance(3000u);
    TEST_ASSERT_EQUAL_size_t(nodes, count(C_NODE));
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_AUTHORIZED, c->ieee, 0u);
    r.nwk = c->nwk;
    feed(&r);
    TEST_ASSERT_EQUAL_size_t(nodes + 1u, count(C_NODE));
    TEST_ASSERT_EQUAL_UINT64(c->ieee, last(C_NODE)->ieee);
    feed(&r); /* told twice: nothing more */
    TEST_ASSERT_EQUAL_size_t(nodes + 1u, count(C_NODE));
}

/* B021, the bench's loop: the device leaves 15.7 s after its announce, its key exchange failed,
 * and joins again with a new address, over and over. The panel never sends it anything in that
 * time, says JOIN_FAILED, and waits again on each new join. */
static void test_a_failed_key_exchange_is_left_alone(void)
{
    const zbsig_t *k = sig("third_reality_3rds17bz.json");
    (void)add_device(k);
    const size_t before = count(C_NODE);
    const zbsig_t *s = sig("sonoff_snzb_02d.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    for (unsigned round = 0; round < 3u; round++) {
        join_key_pending(s->ieee, (uint16_t)(0x5A10u + round));
        advance(15660u);
        zsrc_zb_report_t r = rep(ZSRC_ZB_REP_LEFT, s->ieee, 0u);
        feed(&r);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_JOIN_FAILED, progress().joined[0].result);
        TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, dev(s->ieee)->setup);
        advance(4000u);
    }
    TEST_ASSERT_EQUAL_size_t(before, count(C_NODE));
    /* a join the stack authorized before the announce: no wait */
    const size_t nodes = count(C_NODE);
    join(k->ieee, k->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false); /* it was reset, and joined again */
    TEST_ASSERT_EQUAL_size_t(nodes + 1u, count(C_NODE));
}

/* ZB-002, ZB-021: a known device joining again - a rejoin keeps its record and its setup; a
 * fresh join (it was reset) is ALREADY_ADDED on the add page, its join recorded again, and it is
 * set up again from the start. */
static void test_known_device_joins_again(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    drain_log();
    const size_t nodes = count(C_NODE);
    join(s->ieee, 0x2B2Bu, ZSRC_ZB_JOIN_STANDARD_KEY, true); /* a rejoin, a new short address */
    TEST_ASSERT_EQUAL_size_t(nodes, count(C_NODE));
    TEST_ASSERT_EQUAL_UINT16(0x2B2Bu, dev(s->ieee)->nwk);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, dev(s->ieee)->setup);

    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_INSTALL_CODE, false);
    TEST_ASSERT_EQUAL_UINT16(id, dev(s->ieee)->id); /* the same record */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, dev(s->ieee)->join);
    TEST_ASSERT_EQUAL_size_t(nodes + 1u, count(C_NODE));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, dev(s->ieee)->setup);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ALREADY_ADDED, g.joined[0].result);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[0].step);
    run_setup(s);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ALREADY_ADDED, progress().joined[0].result); /* sticky */
    zbz_log_t l;
    TEST_ASSERT_FALSE(take(&l)); /* not ADDED again */
}

/* ==== refusals and permission levels (devices.md §1, §6) ======================================== */

/* ZB-002, ZB-013, ZB-016, ZB-017: every request but PING, IDENTIFY, SURVEY and DISMISS needs
 * `allowed` (installer mode, every partition disarmed) and changes nothing without it; those four
 * don't read it. */
static void test_permission_levels(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(s);
    static const uint8_t NEED[] = {
        UI_ZB_REQ_ADD, UI_ZB_REQ_ADD_CODE, UI_ZB_REQ_KEEP_LISTENING, UI_ZB_REQ_STOP,
        UI_ZB_REQ_REMOVE, UI_ZB_REQ_FORGET, UI_ZB_REQ_REPLACE, UI_ZB_REQ_SETUP, UI_ZB_REQ_RENAME,
        UI_ZB_REQ_CODE_ADD, UI_ZB_REQ_CODE_FORGET, UI_ZB_REQ_CHANNEL, UI_ZB_REQ_RESET,
        UI_ZB_REQ_RESTART, UI_ZB_REQ_RETRY, UI_ZB_REQ_SIREN, UI_ZB_REQ_SIREN_TEST,
        UI_ZB_REQ_KEYPAD, UI_ZB_REQ_FOB,
    };
    const size_t calls = g_ncalls;
    for (size_t k = 0; k < sizeof(NEED); k++) {
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_NOT_ALLOWED, ask(NEED[k], id, false), "needs allowed");
    }
    TEST_ASSERT_EQUAL_size_t(calls, g_ncalls); /* nothing reached the radio */
    TEST_ASSERT_NOT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_NONE, progress().op);
    static const uint8_t FREE[] = {UI_ZB_REQ_PING, UI_ZB_REQ_IDENTIFY, UI_ZB_REQ_SURVEY,
                                   UI_ZB_REQ_DISMISS};
    for (size_t k = 0; k < sizeof(FREE); k++) {
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_OK, ask(FREE[k], id, false), "doesn't read allowed");
        advance(ZBZ_WAKE_WAIT_MS); /* whatever it started ends by its deadline */
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, false));
    }
}

/* ZB-002, ZB-013, ZB-014, ZB-016, ZB-017, ZB-018: the refusals - one operation at a time (BUSY),
 * nothing on air without the radio (NO_RADIO; Forget alone goes on), no such device or code
 * (NOT_FOUND), a value out of range or a request out of place (INVALID). */
static void test_refusals(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    ui_zb_request_t req;

    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    static const uint8_t BUSY[] = {UI_ZB_REQ_ADD, UI_ZB_REQ_ADD_CODE, UI_ZB_REQ_REPLACE,
                                   UI_ZB_REQ_REMOVE, UI_ZB_REQ_FORGET, UI_ZB_REQ_PING,
                                   UI_ZB_REQ_SETUP, UI_ZB_REQ_SURVEY, UI_ZB_REQ_RESET,
                                   UI_ZB_REQ_RESTART};
    for (size_t k = 0; k < sizeof(BUSY); k++) {
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_BUSY, ask(BUSY[k], id, true), "one at a time");
    }
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CHANNEL;
    req.channel = 15u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_BUSY, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_DISMISS, 0u, true));
    TEST_ASSERT_NOT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_KEEP_LISTENING, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(99u, id, true));

    static const uint8_t BY_ID[] = {UI_ZB_REQ_REMOVE, UI_ZB_REQ_FORGET, UI_ZB_REQ_REPLACE,
                                    UI_ZB_REQ_PING, UI_ZB_REQ_IDENTIFY, UI_ZB_REQ_SETUP,
                                    UI_ZB_REQ_RENAME, UI_ZB_REQ_SIREN, UI_ZB_REQ_SIREN_TEST,
                                    UI_ZB_REQ_KEYPAD, UI_ZB_REQ_FOB};
    for (size_t k = 0; k < sizeof(BY_ID); k++) {
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_NOT_FOUND, ask(BY_ID[k], 999u, true), "no such id");
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_NOT_FOUND, ask(BY_ID[k], 0u, true), "id 0 is none");
    }
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CODE_FORGET;
    req.ieee = 0x02ABCD00000000EEull;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_FOUND, zbz_panel_request(&g_panel, g_now, &req, true));

    /* the siren, keypad and fob settings on a sensor: not that kind of device */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_SIREN, id, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_SIREN_TEST, id, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_KEYPAD, id, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_FOB, id, true));

    /* the radio gone: nothing goes on air; Forget still removes the record */
    radio_state(UI_ZB_RADIO_NOT_ANSWERING, 0u);
    static const uint8_t AIR[] = {UI_ZB_REQ_ADD, UI_ZB_REQ_REPLACE, UI_ZB_REQ_REMOVE,
                                  UI_ZB_REQ_PING, UI_ZB_REQ_IDENTIFY, UI_ZB_REQ_SETUP,
                                  UI_ZB_REQ_SURVEY, UI_ZB_REQ_RESET};
    const size_t calls = g_ncalls;
    for (size_t k = 0; k < sizeof(AIR); k++) {
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_NO_RADIO, ask(AIR[k], id, true), "no radio");
    }
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NO_RADIO, zbz_panel_request(&g_panel, g_now, &(ui_zb_request_t){
                                                                .kind = UI_ZB_REQ_CHANNEL,
                                                                .channel = 15u},
                                                            true));
    TEST_ASSERT_EQUAL_size_t(calls, g_ncalls);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_RETRY, 0u, true)); /* not paused */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_FORGET, id, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FORGOTTEN, progress().result);
    TEST_ASSERT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_FORGET)); /* the radio is told once it can be */
}

/* ZB-002, ZB-013, ZB-016: a radio without a hook answers UNSUPPORTED for what needs it, and a
 * leave without the leave hook falls back to removing the device. */
static void test_unsupported_hooks(void)
{
    zsrc_zb_radio_t bare = RADIO;
    bare.leave = NULL;
    bare.install_code = NULL;
    bare.set_channel = NULL;
    bare.channel_survey = NULL;
    bare.reset_network = NULL;
    bare.identify = NULL;
    fresh(&bare);
    bring_up();
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(s);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, ask(UI_ZB_REQ_REMOVE, id, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, ask(UI_ZB_REQ_IDENTIFY, id, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, ask(UI_ZB_REQ_SURVEY, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, ask(UI_ZB_REQ_RESET, 0u, true));
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CHANNEL;
    req.channel = 20u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, zbz_panel_request(&g_panel, g_now, &req, true));
    req.kind = UI_ZB_REQ_ADD_CODE;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, zbz_panel_request(&g_panel, g_now, &req, true));
    /* a code kept for later is kept, and given to the radio when it can take one */
    req.kind = UI_ZB_REQ_CODE_ADD;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_IC));

    /* no permit-join hook: no window */
    zsrc_zb_radio_t closed = RADIO;
    closed.permit_join = NULL;
    fresh(&closed);
    bring_up();
    TEST_ASSERT_EQUAL(UI_ZB_REQ_UNSUPPORTED, ask(UI_ZB_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_NONE, progress().op);
}

/* ZB-010 (devices.md §4): under ZBZ_NVS_ADD_MIN free entries nothing new is added - Add, Add with
 * a code, Replace and a code kept for later are FULL, the status says so - and a device that
 * joins anyway is asked to leave (FULL); what is there still changes; the reserve is lifted once
 * the owner says there is room again. */
static void test_nvs_reserve(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(s);
    ui_zb_status_t st;
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_FALSE(st.storage_full); /* not told: no limit */

    zbz_panel_nvs_free(&g_panel, ZBZ_NVS_ADD_MIN - 1u);
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_TRUE(st.storage_full);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, ask(UI_ZB_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, ask(UI_ZB_REQ_REPLACE, id, true));
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_ADD_CODE;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, zbz_panel_request(&g_panel, g_now, &req, true));
    req.kind = UI_ZB_REQ_CODE_ADD;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_IC));
    /* changing what is there still writes */
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_RENAME;
    req.id = id;
    snprintf(req.name, sizeof(req.name), "%s", "Back door");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_TRUE((zbz_panel_dirty(&g_panel) & ZBZ_DIRTY_REGISTRY) != 0u);

    /* exactly the minimum: room */
    zbz_panel_nvs_free(&g_panel, ZBZ_NVS_ADD_MIN);
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_FALSE(st.storage_full);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    /* the reserve is reached while the window is open: the next device is asked to leave */
    zbz_panel_nvs_free(&g_panel, ZBZ_NVS_ADD_MIN - 1u);
    const uint64_t stranger = 0x02ABCD00000000C1ull;
    join(stranger, 0x3C3Cu, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    TEST_ASSERT_NULL(dev(stranger));
    TEST_ASSERT_EQUAL_UINT64(stranger, last(C_LEAVE)->ieee);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FULL, g.result);
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);
    zbz_panel_nvs_free(&g_panel, ZBZ_NVS_NOT_TOLD);
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_FALSE(st.storage_full);
}

/* ZB-010: the registry holds ZBZ_DEV_MAX devices; Add is FULL beyond, and a device that joins
 * when the last record was taken during the window is asked to leave (FULL). */
static void test_registry_full(void)
{
    for (uint64_t k = 0; k < ZBZ_DEV_MAX - 1u; k++) {
        TEST_ASSERT_NOT_NULL(zbz_reg_add(&g_panel.reg, 0x02ABCD0000001000ull + k, 0x4000u));
    }
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(0x02ABCD0000002000ull, 0x5000u, ZSRC_ZB_JOIN_STANDARD_KEY, false); /* the 64th */
    TEST_ASSERT_NOT_NULL(dev(0x02ABCD0000002000ull));
    join(0x02ABCD0000002001ull, 0x5001u, ZSRC_ZB_JOIN_STANDARD_KEY, false); /* no room */
    TEST_ASSERT_NULL(dev(0x02ABCD0000002001ull));
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD0000002001ull, last(C_LEAVE)->ieee);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FULL, progress().result);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, ask(UI_ZB_REQ_ADD, 0u, true));
    /* no window open: a stranger that gets in anyway is asked to leave too (no op to end) */
    join(0x02ABCD0000002002ull, 0x5002u, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD0000002002ull, last(C_LEAVE)->ieee);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_NONE, progress().op);
}

/* ZB-024 (alarm-devices.md §4): a keypad is given an engine device number nobody holds - never
 * the request's - keeps it while assigned and frees it when cleared or removed; when the
 * panel-link keypads and the installer's device table hold them all, the page says
 * NO_DEVICE_SLOT and assigning it is FULL. ZB-023: a siren's assignment and its refusals. */
static void test_keypad_device_numbers(void)
{
    const zbsig_t *s = sig("centralite_3405l.json");
    const uint16_t id = add_device(s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_KEYPAD, dev(s->ieee)->role);
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_AVAILABLE, page.keypad.fit);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_KEYPAD, page.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(0u, page.keypad.partition);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_KEYPAD;
    req.id = id;
    req.keypad.partition = 1u;
    req.keypad.show_state = true;
    req.keypad.device = 7u; /* ignored: the number is the panel's to give */
    g_taken = 1u << 0; /* a panel-link keypad holds 1 */
    const size_t before = count(C_ZCL);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_HEX32(1u << 1, zbz_panel_devices_held(&g_panel));
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(2u, page.keypad.device);
    TEST_ASSERT_EQUAL_UINT8(1u, page.keypad.partition);
    TEST_ASSERT_TRUE(page.keypad.live);
    /* greeted at once: its partition's state (Panel Status Changed), under the keypads' tag */
    TEST_ASSERT_EQUAL_size_t(before + 1u, count(C_ZCL));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ACE, last(C_ZCL)->cluster);
    TEST_ASSERT_EQUAL_HEX32(ZBA_TAG, last(C_ZCL)->tag & ZBA_TAG_MASK);
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_ZCL)->tag & ZSRC_ZB_TAG_SOURCE);
    /* its settings again: the same number, even with every other one taken */
    for (unsigned n = ZBA_DEVICE_FIRST; n <= (unsigned)ZBA_DEVICE_LAST; n++) {
        g_taken |= 1u << (n - 1u);
    }
    g_taken &= ~(1u << 1); /* the panel's own keypad holds 2 */
    req.keypad.partition = 2u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(2u, page.keypad.device);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_AVAILABLE, page.keypad.fit);
    /* a partition past the panel's */
    req.keypad.partition = (uint8_t)(ALARM_MAX_PARTITIONS + 1u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, zbz_panel_request(&g_panel, g_now, &req, true));
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    TEST_ASSERT_TRUE(zbz_panel_take_ace(&g_panel, buf, sizeof(buf)) > 0u);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_take_ace(&g_panel, buf, sizeof(buf))); /* taken */

    /* taken off its partition: its number is free again, and then none is */
    req.keypad.partition = 0u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_HEX32(0u, zbz_panel_devices_held(&g_panel));
    g_taken |= 1u << 1;
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_NO_DEVICE_SLOT, page.keypad.fit);
    req.keypad.partition = 1u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_FULL, zbz_panel_request(&g_panel, g_now, &req, true));
    req.keypad.partition = 0u; /* taking it off a partition needs no number */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    g_taken &= ~(1u << (ZBA_DEVICE_LAST - 1u)); /* the last one free again */
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_AVAILABLE, page.keypad.fit);
    req.keypad.partition = 1u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_HEX32(1u << (ZBA_DEVICE_LAST - 1u), zbz_panel_devices_held(&g_panel));

    /* the partition's news reaches it only when something it shows changed (alarm-devices.md
     * §3): the first view, a change of readiness - not the same view again, nor its time alone */
    zba_view_t pv;
    memset(&pv, 0, sizeof(pv));
    pv.state = (uint8_t)ALARM_STATE_DISARMED;
    pv.ready = true;
    pv.at = g_now;
    size_t sent = count(C_ZCL);
    zbz_panel_partition(&g_panel, g_now, 1u, &pv);
    TEST_ASSERT_EQUAL_size_t(sent + 1u, count(C_ZCL));
    pv.at = g_now + 1000u;
    zbz_panel_partition(&g_panel, g_now + 1000u, 1u, &pv);
    zbz_panel_partition(&g_panel, g_now + 1000u, 1u, &pv);
    TEST_ASSERT_EQUAL_size_t(sent + 1u, count(C_ZCL));
    pv.ready = false;
    zbz_panel_partition(&g_panel, g_now + 2000u, 1u, &pv);
    TEST_ASSERT_EQUAL_size_t(sent + 2u, count(C_ZCL));
    zbz_panel_partition(&g_panel, g_now + 2000u, 2u, &pv); /* another partition's: not its */
    TEST_ASSERT_EQUAL_size_t(sent + 2u, count(C_ZCL));

    /* removed (its answer never came, then forgotten): its number goes with it */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REMOVE, id, true));
    advance(ZBZ_LEAVE_MS);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_FORGET, id, true));
    TEST_ASSERT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL_HEX32(0u, zbz_panel_devices_held(&g_panel));
    TEST_ASSERT_TRUE(zbz_panel_take_ace(&g_panel, buf, sizeof(buf)) > 0u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u, true));

    /* ZB-023: a siren - no partition clears it; its test refused until it is the panel's */
    const uint16_t siren = add_device(sig("tuya_ts0216.json"));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SIREN, siren, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_SIREN_TEST, siren, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_KEYPAD, siren, true));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, ask(UI_ZB_REQ_FOB, siren, true));
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_take_wd(&g_panel, buf, sizeof(buf))); /* unchanged */
    /* the engine's news with nothing in it changes nothing */
    zbz_panel_partition(&g_panel, g_now, 1u, NULL);
    zba_view_t v;
    memset(&v, 0, sizeof(v));
    zbz_panel_partition(&g_panel, g_now, 0u, &v);
    zbz_panel_partition(&g_panel, g_now, (uint8_t)(ALARM_MAX_PARTITIONS + 1u), &v);
    TEST_ASSERT_FALSE(zbz_panel_ace_result(&g_panel, g_now, 1u, ALARM_OK, NULL));
}

/* ==== the pages' views (devices.md §7) ========================================================== */

/* ZB-010, ZB-014: the device list - the model table's name or the installer's, unreachable first
 * then by name regardless of case, the zones each feeds, its kind, power and signal; and the
 * signal buckets' edges. */
static void test_device_list_sort_and_words(void)
{
    const zbsig_t *door = sig("sonoff_snzb_04pr2.json");
    const zbsig_t *temp = sig("sonoff_snzb_02d.json");
    const zbsig_t *siren = sig("tuya_ts0216.json");
    const uint16_t door_id = add_device(door);
    const uint16_t temp_id = add_device(temp);
    const uint16_t siren_id = add_device(siren);
    bind_zone(3u, door->ieee, 1u);
    bind_zone(7u, temp->ieee, 1u);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_RENAME;
    req.id = door_id;
    snprintf(req.name, sizeof(req.name), "%s", "back door");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    frame(door->ieee, 200u, -50);
    frame(temp->ieee, 90u, -80);
    frame(siren->ieee, 20u, -95);

    ui_zb_device_t rows[8];
    TEST_ASSERT_EQUAL_size_t(3u, zbz_panel_devices(&g_panel, g_now, rows, 8u));
    TEST_ASSERT_EQUAL_UINT16(door_id, rows[0].id); /* "back door" */
    TEST_ASSERT_EQUAL_UINT16(siren_id, rows[1].id); /* "Siren" */
    TEST_ASSERT_EQUAL_UINT16(temp_id, rows[2].id);  /* "Temperature sensor" */
    TEST_ASSERT_EQUAL_STRING("back door", rows[0].name);
    TEST_ASSERT_EQUAL_STRING("Door sensor", rows[0].model);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, rows[0].state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SENSOR, rows[0].kind);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_POWER_BATTERY, rows[0].power);
    TEST_ASSERT_FALSE(rows[0].router);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_GOOD, rows[0].signal);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, rows[0].device_class);
    TEST_ASSERT_EQUAL_UINT8(1u, rows[0].zone_count);
    TEST_ASSERT_EQUAL_UINT16(3u, rows[0].zones[0]);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, rows[0].join);
    TEST_ASSERT_EQUAL_UINT8(rows[0].setup_total, rows[0].setup_done);
    TEST_ASSERT_EQUAL_UINT32(0u, rows[0].heard_s);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_FAIR, rows[2].signal);
    TEST_ASSERT_EQUAL_UINT16(7u, rows[2].zones[0]);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_WEAK, rows[1].signal);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SIREN, rows[1].kind);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_POWER_MAINS, rows[1].power);
    TEST_ASSERT_TRUE(rows[1].router);
    TEST_ASSERT_EQUAL_UINT8(0u, rows[1].zone_count);

    /* the siren (no zone input) silent past a router's window: unreachable, listed first */
    g_now += (uint64_t)ZSRC_ZB_ROUTER_SUPERVISION_S * 1000u + 1000u;
    TEST_ASSERT_EQUAL_size_t(3u, zbz_panel_devices(&g_panel, g_now, rows, 8u));
    TEST_ASSERT_EQUAL_UINT16(siren_id, rows[0].id);
    TEST_ASSERT_TRUE(rows[0].unreachable);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_ROUTER_SUPERVISION_S + 1u, rows[0].heard_s);
    ui_zb_status_t st;
    zbz_panel_status(&g_panel, g_now, &st);
    TEST_ASSERT_EQUAL_UINT16(3u, st.devices);
    TEST_ASSERT_EQUAL_UINT16(1u, st.not_heard);
    /* a list cut short keeps the first rows of the sorted list, wherever their slots are */
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_devices(&g_panel, g_now, rows, 1u));
    TEST_ASSERT_EQUAL_UINT16(siren_id, rows[0].id);
    TEST_ASSERT_EQUAL_size_t(2u, zbz_panel_devices(&g_panel, g_now, rows, 2u));
    TEST_ASSERT_EQUAL_UINT16(siren_id, rows[0].id);
    TEST_ASSERT_EQUAL_UINT16(door_id, rows[1].id);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_devices(&g_panel, g_now, rows, 0u));

    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_UNKNOWN, zbz_signal_of(0u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_WEAK, zbz_signal_of(1u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_WEAK, zbz_signal_of(ZBZ_LQI_FAIR - 1u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_FAIR, zbz_signal_of(ZBZ_LQI_FAIR));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_FAIR, zbz_signal_of(ZBZ_LQI_GOOD - 1u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_GOOD, zbz_signal_of(ZBZ_LQI_GOOD));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_GOOD, zbz_signal_of(255u));
}

/* ZB-014, ZB-003, ZB-004: a device's page - what it reported, its words from the model table, its
 * inputs with their class and state in the source's words and the zone each feeds; two inputs
 * are told apart by endpoint; the zone page's "Zigbee device". */
static void test_device_page(void)
{
    const zbsig_t *door = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(door);
    bind_zone(3u, door->ieee, 1u);
    frame(door->ieee, 160u, -60);
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT64(door->ieee, page.ieee);
    TEST_ASSERT_EQUAL_UINT16(door->nwk, page.nwk);
    TEST_ASSERT_EQUAL_STRING("SONOFF", page.manufacturer);
    TEST_ASSERT_EQUAL_STRING("SNZB-04PR2", page.model_id);
    TEST_ASSERT_EQUAL_STRING("2.1.0", page.sw_build);
    TEST_ASSERT_EQUAL_UINT8(160u, page.lqi);
    TEST_ASSERT_EQUAL_INT8(-60, page.rssi);
    TEST_ASSERT_EQUAL_UINT32(WALL, page.joined_at);
    TEST_ASSERT_TRUE(page.sleepy);
    TEST_ASSERT_TRUE(page.can_identify);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_DONE, page.setup_step);
    TEST_ASSERT_EQUAL_STRING("Hold its button for 5 seconds, until its light blinks",
                             page.add_words);
    TEST_ASSERT_EQUAL_STRING("", page.wake_words);
    TEST_ASSERT_EQUAL_UINT8(1u, page.dev.zone_count);
    TEST_ASSERT_EQUAL_UINT8(1u, page.input_count);
    TEST_ASSERT_EQUAL_STRING("door contact", page.inputs[0].words);
    TEST_ASSERT_EQUAL_UINT8(1u, page.inputs[0].endpoint);
    TEST_ASSERT_EQUAL_UINT16(3u, page.inputs[0].zone);
    TEST_ASSERT_EQUAL_STRING("", page.inputs[0].state); /* nothing said yet */
    zsrc_zb_ias_status(&g_src, door->ieee, 1u, ZSRC_ZB_IAS_ALARM1, g_now);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_STRING("open", page.inputs[0].state);
    zsrc_zb_ias_status(&g_src, door->ieee, 1u, 0u, g_now);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_STRING("closed", page.inputs[0].state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_NONE, page.siren.fit);
    TEST_ASSERT_EQUAL_UINT16(id, zbz_panel_device_of_zone(&g_panel, 3u));
    TEST_ASSERT_EQUAL_UINT16(0u, zbz_panel_device_of_zone(&g_panel, 4u));
    TEST_ASSERT_EQUAL_UINT16(0u, zbz_panel_device_of_zone(&g_panel, 0u));
    TEST_ASSERT_FALSE(zbz_panel_device(&g_panel, g_now, 0u, &page));
    TEST_ASSERT_FALSE(zbz_panel_device(&g_panel, g_now, 999u, &page));

    const zbsig_t *temp = sig("sonoff_snzb_02d.json");
    const uint16_t tid = add_device(temp);
    zsrc_zb_attr_value(&g_src, temp->ieee, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 2150, g_now);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, tid, &page));
    TEST_ASSERT_EQUAL_STRING("temperature", page.inputs[0].words);
    TEST_ASSERT_EQUAL_STRING("21.50", page.inputs[0].state);
    zsrc_zb_attr_value(&g_src, temp->ieee, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, -5, g_now);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, tid, &page));
    TEST_ASSERT_EQUAL_STRING("-0.05", page.inputs[0].state);

    /* two contacts in one device: each input says its endpoint */
    zbsig_t two = *sig("third_reality_3rds17bz.json");
    two.ieee = 0x02ABCD0000000042ull;
    two.ep_count = 2u;
    two.ep[1] = two.ep[0];
    two.ep[1].endpoint = 2u;
    const uint16_t did = add_device(&two);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, did, &page));
    TEST_ASSERT_EQUAL_UINT8(2u, page.input_count);
    TEST_ASSERT_EQUAL_STRING("endpoint 1 \xC2\xB7 door contact", page.inputs[0].words);
    TEST_ASSERT_EQUAL_STRING("endpoint 2 \xC2\xB7 door contact", page.inputs[1].words);
    TEST_ASSERT_FALSE(page.can_identify);
}

/* The source's words for a sensor (its zone picker's list, zsrc_zigbee.c state_words). */
static void source_state(uint64_t ieee, uint8_t ep, char *out, size_t len)
{
    zsrc_candidate_t c[16];
    const size_t n = zsrc_zb_ops()->candidates(&g_src, "", 0u, c, 16u);
    out[0] = '\0';
    for (size_t k = 0; k < n; k++) {
        if (c[k].addr.id == ieee && c[k].addr.input == ep) {
            snprintf(out, len, "%s", c[k].state);
            return;
        }
    }
    TEST_FAIL_MESSAGE("not in the source's list");
}

/* ZB-014, ZB-003, ZB-007: a device page's input state says what the zone list says - the same
 * alarm mask (either alarm bit, or Alarm2 alone for the ALARM2 quirk) over every status word;
 * with the INVERT quirk the page follows how the zone reads the device (inverted). */
static void test_device_page_words_match_the_source(void)
{
    const zbsig_t *door = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(door);
    zsrc_zb_device_t *slot = (zsrc_zb_device_t *)zsrc_zb_device(&g_src, door->ieee, 1u);
    TEST_ASSERT_NOT_NULL(slot);
    static const uint16_t QUIRKS[] = {0u, ZSRC_ZB_QUIRK_ALARM2, ZSRC_ZB_QUIRK_BOTH_ALARMS,
                                      ZSRC_ZB_QUIRK_INVERT,
                                      (uint16_t)(ZSRC_ZB_QUIRK_ALARM2 | ZSRC_ZB_QUIRK_INVERT)};
    static const uint16_t WORDS[] = {0u, ZSRC_ZB_IAS_ALARM1, ZSRC_ZB_IAS_ALARM2,
                                     (uint16_t)(ZSRC_ZB_IAS_ALARM1 | ZSRC_ZB_IAS_ALARM2)};
    ui_zb_device_page_t page;
    char words[32];
    for (size_t q = 0; q < sizeof(QUIRKS) / sizeof(QUIRKS[0]); q++) {
        slot->quirks = QUIRKS[q];
        for (size_t w = 0; w < sizeof(WORDS) / sizeof(WORDS[0]); w++) {
            zsrc_zb_ias_status(&g_src, door->ieee, 1u, WORDS[w], g_now);
            TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
            source_state(door->ieee, 1u, words, sizeof(words));
            TEST_ASSERT_EQUAL_STRING(words, page.inputs[0].state);
        }
    }
    slot->quirks = ZSRC_ZB_QUIRK_ALARM2;
    zsrc_zb_ias_status(&g_src, door->ieee, 1u, ZSRC_ZB_IAS_ALARM1, g_now);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_STRING("closed", page.inputs[0].state);
    slot->quirks = ZSRC_ZB_QUIRK_INVERT;
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_STRING("closed", page.inputs[0].state); /* the alarm bit, inverted */
    zsrc_zb_ias_status(&g_src, door->ieee, 1u, 0u, g_now);
    TEST_ASSERT_TRUE(zbz_panel_device(&g_panel, g_now, id, &page));
    TEST_ASSERT_EQUAL_STRING("open", page.inputs[0].state);
}

/* ==== what is stored (devices.md §4) ============================================================ */

/* ZB-010, ZB-015, ZB-016: what changed is handed out once, a failed write is asked for again
 * (redo), and a panel that starts from the store has every device, waiting code and setting
 * back - its devices described to the source again, enrolled, before they speak. */
static void test_store_take_redo_and_load(void)
{
    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    const uint16_t id = add_device(s);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CODE_ADD;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CHANNEL;
    req.channel = 11u; /* the channel it is on: kept as the installer's choice at once */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, progress().result);
    TEST_ASSERT_EQUAL_size_t(0u, count(C_CHANNEL));
    const uint8_t want_dirty = ZBZ_DIRTY_REGISTRY | ZBZ_DIRTY_CODES | ZBZ_DIRTY_CFG;
    TEST_ASSERT_EQUAL_HEX8(want_dirty, zbz_panel_dirty(&g_panel));
    uint8_t small[3];
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_take_cfg(&g_panel, small, sizeof(small)));
    TEST_ASSERT_TRUE((zbz_panel_dirty(&g_panel) & ZBZ_DIRTY_CFG) != 0u); /* the bit stays */
    write_back();
    TEST_ASSERT_EQUAL_HEX8(0u, zbz_panel_dirty(&g_panel));
    TEST_ASSERT_NOT_NULL(kv_find("meta"));
    TEST_ASSERT_NOT_NULL(kv_find("d00"));
    TEST_ASSERT_NOT_NULL(kv_find(ZBZ_KEY_CODES));
    TEST_ASSERT_NOT_NULL(kv_find(ZBZ_CFG_KEY));

    zbz_panel_redo(&g_panel, ZBZ_DIRTY_REGISTRY | ZBZ_DIRTY_CODES | ZBZ_DIRTY_CFG);
    TEST_ASSERT_EQUAL_HEX8(want_dirty, zbz_panel_dirty(&g_panel));
    write_back();
    TEST_ASSERT_EQUAL_HEX8(0u, zbz_panel_dirty(&g_panel));

    /* the panel starts again: the store, then the radio, then the link */
    fresh(&RADIO);
    TEST_ASSERT_EQUAL(ZBZ_REG_OK, zbz_panel_load(&g_panel, &STORE));
    const zbz_device_t *d = dev(s->ieee);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT16(id, d->id);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, d->role);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, d->ep[0].zone_type);
    TEST_ASSERT_EQUAL_UINT8(1u, (uint8_t)g_panel.codes.count);
    TEST_ASSERT_TRUE(g_panel.cfg.channel_set);
    TEST_ASSERT_EQUAL_UINT8(11u, g_panel.cfg.channel);
    TEST_ASSERT_EQUAL_HEX8(0u, zbz_panel_dirty(&g_panel)); /* loading writes nothing */
    radio_state(UI_ZB_RADIO_WORKING, 0u);
    TEST_ASSERT_EQUAL_size_t(1u, count(C_IC));            /* the waiting code, given again */
    TEST_ASSERT_NULL(zsrc_zb_device(&g_src, s->ieee, 1u)); /* the link isn't up yet */
    TEST_ASSERT_NOT_EQUAL(0u, zbz_panel_next_deadline(&g_panel));
    zsrc_zb_link(&g_src, true, g_now);
    TEST_ASSERT_EQUAL_UINT64(0u, zbz_panel_next_deadline(&g_panel)); /* owed: at once */
    zbz_panel_poll(&g_panel, g_now);
    const zsrc_zb_device_t *slot = zsrc_zb_device(&g_src, s->ieee, 1u);
    TEST_ASSERT_NOT_NULL(slot);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, slot->zone_type);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, slot->device_class);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_ENROLL_DONE, slot->enroll);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zbz_panel_next_deadline(&g_panel));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_NODE)); /* not set up again */
}

/* ZB-010 (devices.md §4): each stored part stands alone - damaged codes are empty, a damaged or
 * later or oversized cfg is the default, a damaged record is dropped - and the rest loads. */
static void test_load_damaged_parts(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    (void)add_device(s);
    const zbsig_t *t = sig("sonoff_snzb_04pr2.json");
    (void)add_device(t);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CODE_ADD;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request(&g_panel, g_now, &req, true));
    write_back();
    kv_put(ZBZ_CFG_KEY, CFG_FIXTURE_V1_CHANNEL_20, sizeof(CFG_FIXTURE_V1_CHANNEL_20));

    /* damaged codes: empty; the registry and cfg load */
    static const uint8_t junk[] = {0xFFu, 0x00u, 0x13u};
    kv_put(ZBZ_KEY_CODES, junk, sizeof(junk));
    fresh(&RADIO);
    TEST_ASSERT_EQUAL(ZBZ_REG_OK, zbz_panel_load(&g_panel, &STORE));
    TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)g_panel.codes.count);
    TEST_ASSERT_EQUAL_size_t(2u, zbz_reg_count(&g_panel.reg));
    TEST_ASSERT_EQUAL_UINT8(20u, g_panel.cfg.channel);

    /* cfg: a channel that isn't one, a later compat, compat 0, too short, too long to read:
     * each the default; a longer blob at this compat reads as far as known */
    static const uint8_t not_a_channel[] = {0x01u, 0x01u, 0x05u, 0x00u};
    static const uint8_t later[] = {0x02u, 0x01u, 0x14u, 0x00u};
    static const uint8_t zero[] = {0x00u, 0x01u, 0x14u, 0x00u};
    static const uint8_t short_blob[] = {0x01u, 0x01u, 0x14u};
    static const uint8_t longer[] = {0x01u, 0x01u, 0x19u, 0x00u, 0xAAu, 0xBBu};
    static const struct {
        const uint8_t *b;
        size_t n;
        bool set;
        uint8_t ch;
    } CASES[] = {
        {not_a_channel, sizeof(not_a_channel), false, 0u}, {later, sizeof(later), false, 0u},
        {zero, sizeof(zero), false, 0u}, {short_blob, sizeof(short_blob), false, 0u},
        {longer, sizeof(longer), true, 25u},
    };
    for (size_t k = 0; k < sizeof(CASES) / sizeof(CASES[0]); k++) {
        kv_put(ZBZ_CFG_KEY, CASES[k].b, CASES[k].n);
        fresh(&RADIO);
        (void)zbz_panel_load(&g_panel, &STORE);
        TEST_ASSERT_EQUAL(CASES[k].set, g_panel.cfg.channel_set);
        TEST_ASSERT_EQUAL_UINT8(CASES[k].ch, g_panel.cfg.channel);
        TEST_ASSERT_EQUAL_size_t(2u, zbz_reg_count(&g_panel.reg));
    }
    uint8_t huge[600];
    memset(huge, 0x01, sizeof(huge));
    huge[2] = 20u;
    kv_put(ZBZ_CFG_KEY, huge, sizeof(huge));
    fresh(&RADIO);
    (void)zbz_panel_load(&g_panel, &STORE);
    TEST_ASSERT_FALSE(g_panel.cfg.channel_set);

    /* a damaged record (cut short): that device is dropped, the other loads */
    kv_put(ZBZ_CFG_KEY, CFG_FIXTURE_V1_DEFAULT, sizeof(CFG_FIXTURE_V1_DEFAULT));
    kv_t *rec = kv_find("d00");
    TEST_ASSERT_NOT_NULL(rec);
    rec->len -= 3u;
    fresh(&RADIO);
    TEST_ASSERT_EQUAL(ZBZ_REG_DAMAGED, zbz_panel_load(&g_panel, &STORE));
    TEST_ASSERT_EQUAL_size_t(1u, zbz_reg_count(&g_panel.reg));
    TEST_ASSERT_NOT_NULL(dev(t->ieee));
    TEST_ASSERT_FALSE(g_panel.cfg.channel_set);

    /* nothing stored, or no store: a new panel */
    memset(g_kv, 0, sizeof(g_kv));
    fresh(&RADIO);
    TEST_ASSERT_EQUAL(ZBZ_REG_EMPTY, zbz_panel_load(&g_panel, &STORE));
    TEST_ASSERT_EQUAL_size_t(0u, zbz_reg_count(&g_panel.reg));
    (void)zbz_panel_load(&g_panel, NULL);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_reg_count(&g_panel.reg));
}

/* ZB-016: the cfg blob's frozen fixtures (made by hand, cfg_fixture_v1.h) read back as written,
 * and the encoder writes them byte for byte; a buffer too small takes nothing. */
static void test_cfg_frozen_fixture(void)
{
    zbz_cfg_t c;
    TEST_ASSERT_TRUE(zbz_cfg_decode(&c, CFG_FIXTURE_V1_CHANNEL_20,
                                    sizeof(CFG_FIXTURE_V1_CHANNEL_20)));
    TEST_ASSERT_TRUE(c.channel_set);
    TEST_ASSERT_EQUAL_UINT8(20u, c.channel);
    uint8_t out[8];
    TEST_ASSERT_EQUAL_size_t(ZBZ_CFG_BLOB, zbz_cfg_encode(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CFG_FIXTURE_V1_CHANNEL_20, out, ZBZ_CFG_BLOB);

    TEST_ASSERT_TRUE(zbz_cfg_decode(&c, CFG_FIXTURE_V1_DEFAULT, sizeof(CFG_FIXTURE_V1_DEFAULT)));
    TEST_ASSERT_FALSE(c.channel_set);
    TEST_ASSERT_EQUAL_size_t(ZBZ_CFG_BLOB, zbz_cfg_encode(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CFG_FIXTURE_V1_DEFAULT, out, ZBZ_CFG_BLOB);

    TEST_ASSERT_EQUAL_size_t(0u, zbz_cfg_encode(&c, out, ZBZ_CFG_BLOB - 1u));
    TEST_ASSERT_FALSE(zbz_cfg_decode(&c, NULL, 4u));
    TEST_ASSERT_FALSE(zbz_cfg_decode(NULL, CFG_FIXTURE_V1_DEFAULT, 4u));

    /* and through the store, as a panel starts */
    kv_put(ZBZ_CFG_KEY, CFG_FIXTURE_V1_CHANNEL_20, sizeof(CFG_FIXTURE_V1_CHANNEL_20));
    fresh(&RADIO);
    (void)zbz_panel_load(&g_panel, &STORE);
    TEST_ASSERT_TRUE(g_panel.cfg.channel_set);
    TEST_ASSERT_EQUAL_UINT8(20u, g_panel.cfg.channel);
}

/* ==== the log, the radio and the walk test ====================================================== */

/* ZB-019 (devices.md §11): events wait in order, oldest first, at most ZBZ_LOG_MAX; more are
 * counted lost, never overwrite the waiting ones. Strangers turned away: each new address once
 * an hour, ZBZ_REFUSED_MAX remembered (the oldest gives way). */
static void test_log_order_and_overflow(void)
{
    for (uint8_t k = 0; k < ZBZ_LOG_MAX + 2u; k++) {
        zsrc_zb_report_t r = rep(ZSRC_ZB_REP_JOIN_REFUSED, 0x02ABCD0000003000ull + k, 0u);
        r.u.refused.why = k;
        g_now += 10u;
        feed(&r);
    }
    TEST_ASSERT_EQUAL_UINT32(2u, g_panel.log_lost);
    zbz_log_t l;
    for (uint8_t k = 0; k < ZBZ_LOG_MAX; k++) {
        TEST_ASSERT_TRUE(take(&l));
        TEST_ASSERT_EQUAL_UINT8(ZBZ_EV_JOIN_REFUSED, l.type);
        TEST_ASSERT_EQUAL_UINT8(k, l.arg);
        TEST_ASSERT_EQUAL_UINT8(0u, l.user);
    }
    TEST_ASSERT_FALSE(take(&l));
    TEST_ASSERT_FALSE(zbz_panel_take_log(&g_panel, NULL));
    /* the first address was given way to: it is logged again within the hour */
    zsrc_zb_report_t r = rep(ZSRC_ZB_REP_JOIN_REFUSED, 0x02ABCD0000003000ull, 0u);
    feed(&r);
    TEST_ASSERT_TRUE(take(&l));
    /* the newest is remembered: not again within the hour */
    r.ieee = 0x02ABCD0000003000ull + ZBZ_LOG_MAX + 1u;
    feed(&r);
    TEST_ASSERT_FALSE(take(&l));
}

/* ZB-018, ZB-012: the radio going away mid-way - an open window fails and closes, a setup waits
 * (nothing is counted as a try) and goes on when the radio is back; a setup left STOPPED stays
 * stopped. */
static void test_radio_lost_and_back(void)
{
    const zbsig_t *stopped = sig("tuya_ts0216.json");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u, true));
    join(stopped->ieee, stopped->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    g_fail_kind = C_NODE; /* optional: given up after three */
    g_fail_result = ZSRC_ZB_DONE_REFUSED;
    g_fail_left = -1;
    answer(stopped, last(C_NODE));
    answer(stopped, last(C_NODE));
    answer(stopped, last(C_NODE));
    g_fail_kind = C_ACTIVE;
    answer(stopped, last(C_ACTIVE));
    answer(stopped, last(C_ACTIVE));
    answer(stopped, last(C_ACTIVE));
    g_fail_kind = 0u;
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, dev(stopped->ieee)->setup);

    const zbsig_t *s = sig("sonoff_snzb_04pr2.json");
    join(s->ieee, s->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    const size_t nodes = count(C_NODE);
    radio_state(UI_ZB_RADIO_NOT_ANSWERING, 0u);
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, g.result);
    TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value);
    advance(ZBZ_STEP_MS * 5u);
    TEST_ASSERT_EQUAL_size_t(nodes, count(C_NODE)); /* nothing sent without the radio */
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, dev(s->ieee)->setup);
    TEST_ASSERT_EQUAL_UINT8(0u, g_panel.live[1].tries);
    radio_state(UI_ZB_RADIO_WORKING, 0u);
    TEST_ASSERT_EQUAL_size_t(nodes + 1u, count(C_NODE)); /* the step again */
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, dev(stopped->ieee)->setup);
    run_setup(s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, dev(s->ieee)->setup);
    /* a DONE that says the radio went away is no try either */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, dev(s->ieee)->id, true));
    done(s->ieee, last(C_NODE)->tag, ZSRC_ZB_DONE_NO_RADIO);
    last(C_NODE)->answered = true;
    TEST_ASSERT_EQUAL_UINT8(0u, g_panel.live[1].tries);
    /* ... and while the radio still says WORKING, the step goes again after a while */
    const size_t again = count(C_NODE);
    advance(ZBZ_STEP_MS);
    TEST_ASSERT_EQUAL_size_t(again + 1u, count(C_NODE));
    TEST_ASSERT_EQUAL_UINT8(0u, g_panel.live[1].tries);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_RUNNING, dev(s->ieee)->setup);
}

/* ZB-022: the walk test goes to the source (zsrc_zb_walk_test), on and off. */
static void test_walk_test(void)
{
    (void)add_device(sig("sonoff_snzb_04pr2.json"));
    zbz_panel_walk_test(&g_panel, g_now, true);
    TEST_ASSERT_TRUE(g_src.walk_test);
    zbz_panel_walk_test(&g_panel, g_now, false);
    TEST_ASSERT_FALSE(g_src.walk_test);
}

/* ZB-012: the panel's own requests carry its tag ('Z' and a number, never 0, never the source's
 * bit) - every tagged request the tests above made through this harness included. */
static void test_tags_are_the_panels(void)
{
    (void)add_device(sig("tuya_ts0216.json"));
    (void)add_device(sig("sonoff_snzb_02d.json"));
    size_t tagged = 0u;
    for (size_t i = 0; i < g_ncalls; i++) {
        if (g_calls[i].tag == 0u) {
            continue;
        }
        tagged++;
        TEST_ASSERT_EQUAL_HEX32(ZBZ_TAG, g_calls[i].tag & ZBZ_TAG_MASK);
        TEST_ASSERT_EQUAL_HEX32(0u, g_calls[i].tag & ZSRC_ZB_TAG_SOURCE);
        TEST_ASSERT_NOT_EQUAL(0u, g_calls[i].tag & 0xFFFFu);
    }
    TEST_ASSERT_TRUE(tagged > 10u);
    g_panel.seq = 0xFFFFu; /* the number wraps past 0 */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SURVEY, 0u, true));
    TEST_ASSERT_EQUAL_HEX32(ZBZ_TAG | 1u, last(C_SURVEY)->tag);
}

/* ==== against the fake radio and the real engine (alarm-devices.md §2-§4, testing.md §4, §9) ==== */

/* zbz_panel wired as the firmware wires it: the fake radio's hooks, its reports into
 * zbz_panel_on_report, and the owner - the test - as the alarm task: each keypad's post runs in the
 * real engine (alarm_sim) and its result comes back by ticket; the engine's outputs and partition
 * state go to zbz_panel_output and zbz_panel_partition. */
static zbf_radio_t F;
static zsrc_zb_t FZ;
static zsrc_zb_radio_t FH;
static zbz_panel_t FP;
static sim_t SIM;
#define FQ_MAX 8u
static zba_post_t FQ[FQ_MAX];
static unsigned FQN;
static bool g_post_refused;
static int g_bell_trouble;
static unsigned g_bell_calls;
typedef struct {
    uint64_t ieee;
    uint8_t ep, tsn, command, status;
} f_drsp_t;
static f_drsp_t g_drsp[16];
static unsigned g_ndrsp;

static void f_report(void *ctx, const zsrc_zb_report_t *r, uint64_t now)
{
    (void)ctx;
    zbz_panel_on_report(&FP, now, r);
}

/* The owner's queue to the alarm task. */
static bool f_post(void *ctx, const zba_post_t *post)
{
    (void)ctx;
    if (g_post_refused || FQN >= FQ_MAX) {
        return false;
    }
    FQ[FQN++] = *post;
    return true;
}

static void f_bell(void *ctx, bool active)
{
    (void)ctx;
    g_bell_trouble = active ? 1 : 0;
    g_bell_calls++;
}

static bool f_default_rsp(void *ctx, uint64_t ieee, uint8_t ep, uint8_t tsn, uint8_t command,
                          uint8_t status, uint32_t tag)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_HEX32(ZBA_TAG, tag & ZBA_TAG_MASK);
    TEST_ASSERT_TRUE(g_ndrsp < sizeof(g_drsp) / sizeof(g_drsp[0]));
    g_drsp[g_ndrsp++] = (f_drsp_t){ieee, ep, tsn, command, status};
    return true;
}

static void f_up(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_start(&SIM, &cfg, NULL);
    FQN = 0u;
    g_post_refused = false;
    g_bell_trouble = -1;
    g_bell_calls = 0u;
    g_ndrsp = 0u;
    static const zbf_sink_t sink = {.ctx = NULL, .report = f_report, .attr = NULL};
    zbf_init(&F, &FZ, &sink, g_now);
    FH = zbf_hooks(&F);
    zsrc_zb_init(&FZ, &FH, &g_sink);
    g_port.ace_post = f_post;
    const zbw_port_t wd = {.ctx = NULL, .send = NULL, .bell_trouble = f_bell, .checkin = NULL};
    const zba_port_t ace = {.ctx = NULL, .send = NULL, .default_rsp = f_default_rsp};
    zbz_panel_init(&FP, &g_port, &FZ, &FH, &wd, &ace);
    zbf_up(&F);
    zbz_radio_info_t info;
    memset(&info, 0, sizeof(info));
    info.state = UI_ZB_RADIO_WORKING;
    zbz_panel_radio(&FP, g_now, &info);
}

/* Time passes: the air and the panel's deadlines, each at its own time. */
static void frun(uint64_t ms)
{
    const uint64_t end = g_now + ms;
    for (unsigned guard = 0; guard < 400000u; guard++) {
        zbz_panel_poll(&FP, g_now);
        if (g_now >= end) {
            return;
        }
        uint64_t t = zbf_next_due(&F);
        const uint64_t d = zbz_panel_next_deadline(&FP);
        t = d < t ? d : t;
        t = t <= g_now ? g_now + 1u : t;
        g_now = t < end ? t : end;
        zbf_advance(&F, g_now);
    }
    TEST_FAIL_MESSAGE("time stood still");
}

static ui_zb_req_result_t fask(const ui_zb_request_t *req)
{
    return zbz_panel_request(&FP, g_now, req, true);
}

static const zbz_device_t *fdev(uint64_t ieee)
{
    return zbz_reg_find(&FP.reg, ieee);
}

/* What a device added next is made to do before it joins (its fake knobs); NULL nothing. */
static void (*g_prep)(zbf_dev_t *d);

/* ADD, it joins and is set up (woken when it waits for its wake), the window closed: its setup
 * DONE, or (f_add_ended) however it ended. */
static zbf_dev_t *f_add_ended(const char *name, uint16_t *id, bool done);

static zbf_dev_t *f_add(const char *name, uint16_t *id)
{
    return f_add_ended(name, id, true);
}

static zbf_dev_t *f_add_ended(const char *name, uint16_t *id, bool done)
{
    zbf_dev_t *d = zbf_add(&F, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, name);
    if (g_prep != NULL) {
        g_prep(d);
        g_prep = NULL;
    }
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_ADD;
    req.user = 1u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    frun(100u);
    TEST_ASSERT_TRUE_MESSAGE(zbf_join(&F, d), name);
    for (int guard = 0; guard < 600; guard++) {
        const zbz_device_t *r = fdev(d->ieee);
        if (r != NULL && r->setup != ZBZ_SETUP_RUNNING && r->setup != ZBZ_SETUP_WAKE) {
            break;
        }
        if (r != NULL && r->setup == ZBZ_SETUP_WAKE) {
            (void)zbf_check_in(&F, d);
        }
        frun(1000u);
    }
    const zbz_device_t *r = fdev(d->ieee);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    if (done) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZBZ_SETUP_DONE, r->setup, name);
    }
    req.kind = UI_ZB_REQ_STOP;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    req.kind = UI_ZB_REQ_DISMISS;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    *id = r->id;
    return d;
}

/* The engine's news, as the owner forwards it: partition 1's state, then its bell. */
static void f_news(void)
{
    const alarm_partition_status_t st = sim_status(&SIM, 1);
    zba_view_t v;
    memset(&v, 0, sizeof(v));
    v.state = (uint8_t)st.state;
    v.level = (uint8_t)st.level;
    v.ready = st.ready;
    v.delay_s = (uint16_t)((st.delay_remaining_ms + 999u) / 1000u);
    v.at = g_now;
    zbz_panel_partition(&FP, g_now, 1u, &v);
    zbz_panel_output(&FP, g_now, 1u, ALARM_OUT_BELL, sim_output(&SIM, ALARM_OUT_BELL, 1u));
}

/* The alarm task: every queued post through the engine (a keypad with its code, a fob as its
 * user), the code wiped, the result back by ticket. The last result. */
static alarm_result_t f_task(void)
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
        TEST_ASSERT_TRUE(zbz_panel_ace_result(&FP, g_now, q->ticket, last, &res));
    }
    FQN = 0u;
    return last;
}

/* A keypad's or fob's IAS ACE command, as the device sends it. */
static void f_send(zbf_dev_t *d, uint8_t ep, uint8_t command, const uint8_t *payload, uint8_t len,
                   bool want_response)
{
    zsrc_zb_zcl_t c;
    memset(&c, 0, sizeof(c));
    c.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    c.command = command;
    c.tsn = ZSRC_ZB_TSN_NEW; /* its next sequence number */
    c.want_response = want_response;
    c.len = len;
    if (len > 0u) {
        memcpy(c.payload, payload, len);
    }
    TEST_ASSERT_TRUE(zbf_send(&F, d, ep, &c));
    frun(100u);
}

static void f_arm(zbf_dev_t *d, uint8_t ep, uint8_t mode, const char *code)
{
    uint8_t p[ZSRC_ZB_ZCL_PAYLOAD_MAX];
    const size_t n = code != NULL ? strlen(code) : 0u;
    p[0] = mode;
    p[1] = (uint8_t)n;
    if (n > 0u) {
        memcpy(&p[2], code, n);
    }
    p[2u + n] = 0u; /* zone id */
    f_send(d, ep, ZBA_CMD_ARM, p, (uint8_t)(3u + n), false);
}

/* The device's own last ZCL frame's sequence number. */
static uint8_t f_last_tsn(uint64_t ieee)
{
    for (size_t i = zbf_log_count(&F); i > 0u; i--) {
        const zbf_frame_t *f = zbf_log_at(&F, i - 1u);
        if (f->rx && f->ieee == ieee && f->kind == ZBF_FRAME_ZCL && f->len >= 3u) {
            return f->bytes[1];
        }
    }
    TEST_FAIL_MESSAGE("no frame");
    return 0u;
}

/* The Arm Response the keypad got last: its note. */
static uint8_t f_arm_note(zbf_dev_t *d, uint8_t ep)
{
    frun(100u);
    const zbf_ep_t *e = zbf_ep(d, ep);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ACE, e->last_cmd.cluster);
    TEST_ASSERT_TRUE(e->last_cmd.server_to_client);
    TEST_ASSERT_EQUAL_HEX8(ZBA_RSP_ARM, e->last_cmd.command);
    return e->last_cmd.payload[0];
}

/* The first siren's layout of the Start Warning byte (its model's). */
static uint8_t wd_layout(void)
{
    return (uint8_t)((FP.wd.siren[0].a.model_flags & ZBZ_MODEL_WD_REVERSED) != 0u
                         ? ZBW_LAYOUT_REVERSED
                         : ZBW_LAYOUT_SPEC);
}

/* Whether `needle` is anywhere in the panel's state (a code must not be). */
static bool state_holds_code(const char *needle)
{
    const uint8_t *b = (const uint8_t *)&FP;
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= sizeof(FP); i++) {
        if (memcmp(b + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* ZB-023 (alarm-devices.md §2): a siren follows the bell - Start Warning when the engine's bell
 * sounds, Stop when it is silenced; "Test it" sounds it, its result reaches the page's last_test,
 * and is refused while its partition is armed; silent past its supervision it is the bell
 * trouble, cleared by its next frame; its settings are stored and load back. */
static void test_siren_follows_the_bell(void)
{
    f_up();
    uint16_t id = 0u;
    zbf_dev_t *sd = f_add("frient_sirzb_111", &id);
    zbf_ep_t *wd = zbf_ep(sd, 43u);
    TEST_ASSERT_NOT_NULL(wd);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = id;
    req.siren.partitions = 1u << 0;
    req.siren.burglary = true;
    req.siren.fire = true;
    req.siren.volume = ZBW_LEVEL_HIGH;
    req.siren.supervision_min = 60u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    f_news(); /* start-up: disarmed, the bell off */
    frun(1000u);
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SIREN, page.dev.kind);
    TEST_ASSERT_TRUE(page.siren.live);
    TEST_ASSERT_EQUAL_UINT8(1u, page.siren.partitions);
    TEST_ASSERT_EQUAL_UINT8(0u, page.siren.device);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NONE, page.siren.last_test);
    ui_zb_device_t rows[4];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_devices(&FP, g_now, rows, 4u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SIREN, rows[0].kind);

    /* "Test it": a short sound, followed to its end */
    const uint32_t warnings = wd->warnings;
    req.kind = UI_ZB_REQ_SIREN_TEST;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    ui_zb_progress_t g;
    zbz_panel_progress(&FP, g_now, &g);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_SIREN_TEST, g.op);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_BUSY, fask(&req));
    frun(500u);
    zbz_panel_progress(&FP, g_now, &g);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, g.result);
    TEST_ASSERT_EQUAL_UINT32(warnings + 1u, wd->warnings);
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_DONE, page.siren.last_test);
    req.kind = UI_ZB_REQ_DISMISS;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    /* a test it never confirms: NO_ANSWER */
    const zbf_fault_t mute = {.timeout = true};
    zbf_fault(sd, ZSRC_ZB_STEP_NONE, &mute);
    req.kind = UI_ZB_REQ_SIREN_TEST;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    frun(ZBZ_SIREN_TEST_MS + 1000u);
    zbz_panel_progress(&FP, g_now, &g);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, g.result);
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_NO_ANSWER, page.siren.last_test);
    zbf_fault(sd, ZSRC_ZB_STEP_NONE, NULL);
    req.kind = UI_ZB_REQ_DISMISS;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    frun(ZBW_CONFIRM_MS * 4u); /* its retries end */

    /* armed: no test */
    sim_arm_away_and_leave(&SIM, CODE_USER);
    f_news();
    frun(100u);
    req.kind = UI_ZB_REQ_SIREN_TEST;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_ALLOWED, fask(&req));

    /* a burglary: the bell sounds, the siren with it */
    const uint32_t before = wd->warnings;
    sim_fault(&SIM, Z_WINDOW);
    TEST_ASSERT_EQUAL(ALARM_PAT_STEADY, sim_output(&SIM, ALARM_OUT_BELL, 1u));
    f_news();
    frun(500u);
    TEST_ASSERT_TRUE(wd->warnings > before);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_WD, wd->last_cmd.cluster);
    TEST_ASSERT_EQUAL_HEX8(ZBW_CMD_START_WARNING, wd->last_cmd.command);
    zbw_warning_t w;
    zbw_warning_info_decode(wd->last_cmd.payload[0], wd_layout(), &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    /* silenced by the code: Stop */
    TEST_ASSERT_EQUAL(ALARM_OK, sim_disarm(&SIM, 1u, CODE_USER));
    TEST_ASSERT_EQUAL(ALARM_PAT_OFF, sim_output(&SIM, ALARM_OUT_BELL, 1u));
    f_news();
    frun(500u);
    TEST_ASSERT_EQUAL_HEX8(ZBW_CMD_START_WARNING, wd->last_cmd.command);
    zbw_warning_info_decode(wd->last_cmd.payload[0], wd_layout(), &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);

    /* silent for its supervision time - dead: it answers not even the panel's reads (ZB-023) -
     * the bell trouble, through the owner's port */
    TEST_ASSERT_TRUE(g_bell_trouble != 1);
    const zbf_fault_t dead = {.timeout = true};
    zbf_fault(sd, ZSRC_ZB_STEP_NONE, &dead);
    frun(60u * 60u * 1000u + 1000u);
    TEST_ASSERT_EQUAL_INT(1, g_bell_trouble);
    zbf_fault(sd, ZSRC_ZB_STEP_NONE, NULL);
    TEST_ASSERT_TRUE(zbf_check_in(&F, sd)); /* a frame from it */
    frun(1000u);
    TEST_ASSERT_EQUAL_INT(0, g_bell_trouble);

    /* stored, and loaded back with what isn't stored refilled */
    uint8_t blob[ZBZ_CODES_BLOB_MAX];
    const size_t n = zbz_panel_take_wd(&FP, blob, sizeof(blob));
    TEST_ASSERT_TRUE(n > 0u && n <= ZBW_BLOB_MAX);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_take_wd(&FP, blob, sizeof(blob)));
    const zbw_assign_t was = FP.wd.siren[0].a;
    memset(g_kv, 0, sizeof(g_kv));
    char key[ZBZ_KEY_LEN];
    size_t len = 0u;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&FP, key, blob, sizeof(blob), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            kv_put(key, blob, len);
        }
    }
    FP.wd.dirty = true;
    kv_put(ZBW_KEY, blob, zbz_panel_take_wd(&FP, blob, sizeof(blob)));
    zbz_panel_init(&FP, &g_port, &FZ, &FH, NULL, NULL);
    TEST_ASSERT_EQUAL(ZBZ_REG_OK, zbz_panel_load(&FP, &STORE));
    const zbw_siren_t *s = zbw_find(&FP.wd, id);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT64(was.ieee, s->a.ieee);
    TEST_ASSERT_EQUAL_UINT8(was.endpoint, s->a.endpoint);
    TEST_ASSERT_EQUAL_UINT8(was.partitions, s->a.partitions);
    TEST_ASSERT_EQUAL_UINT16(60u, s->a.supervision_min);
    TEST_ASSERT_EQUAL_UINT16(was.model_flags, s->a.model_flags);
    TEST_ASSERT_EQUAL_UINT16(was.max_duration_s, s->a.max_duration_s);
    TEST_ASSERT_EQUAL(was.sleepy, s->a.sleepy);
    TEST_ASSERT_FALSE(FP.wd.dirty); /* loading writes nothing */
    /* a damaged value loads no siren and isn't written over */
    blob[0] = 0u;
    kv_put(ZBW_KEY, blob, 2u);
    TEST_ASSERT_EQUAL(ZBZ_REG_OK, zbz_panel_load(&FP, &STORE));
    TEST_ASSERT_NULL(zbw_find(&FP.wd, id));
    TEST_ASSERT_EQUAL_size_t(0u, zbz_panel_take_wd(&FP, blob, sizeof(blob)));
}

/* ---- ZB03: the siren's MaxDuration, read at its setup ------------------------------------------- */

static bool g_wd_set;
static uint16_t g_wd_s;
static uint8_t g_wd_type;
static zbf_fault_t g_wd_fault;

static void prep_wd(zbf_dev_t *d)
{
    d->wd_max_set = g_wd_set;
    d->wd_max_s = g_wd_s;
    d->wd_max_type = g_wd_type;
    zbf_fault(d, ZSRC_ZB_STEP_POLL_CTRL, &g_wd_fault); /* the step its read belongs to */
}

/* The panel's Read Attributes to `ieee`'s IAS WD in the log (global frame, command 0x00). */
static unsigned wd_reads(uint64_t ieee)
{
    unsigned n = 0u;
    for (size_t i = 0; i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->rx || f->ieee != ieee || f->kind != ZBF_FRAME_ZCL ||
            f->cluster != ZSRC_ZB_ZCL_IAS_WD || f->len < 3u || (f->bytes[0] & 0x03u) != 0u) {
            continue;
        }
        const size_t cmd = (f->bytes[0] & 0x04u) != 0u ? 4u : 2u;
        n += (f->len > cmd && f->bytes[cmd] == ZBZ_ZCL_READ_ATTR) ? 1u : 0u;
    }
    return n;
}

/* The siren `id` on partition 1, burglary and fire. */
static void siren_on(uint16_t id)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = id;
    req.siren.partitions = 1u << 0;
    req.siren.burglary = true;
    req.siren.fire = true;
    req.siren.volume = ZBW_LEVEL_HIGH;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
}

/* The Start Warning's duration field (Warning Info | duration u16 | duty | strobe level). */
static uint16_t warned_s(const zbf_ep_t *e)
{
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_WD, e->last_cmd.cluster);
    TEST_ASSERT_EQUAL_HEX8(ZBW_CMD_START_WARNING, e->last_cmd.command);
    return (uint16_t)((unsigned)e->last_cmd.payload[1] | ((unsigned)e->last_cmd.payload[2] << 8));
}

/* ZB-023 (ZB03; alarm-devices.md §2 "MaxDuration read at its setup"): the setup reads the IAS WD
 * server's MaxDuration (0x0502 / 0x0000) once, byte for byte through the fake radio; the device
 * keeps it, the assignment takes it, and a burglary's Start Warning asks for it and is asked
 * again before it runs out - the default (240), a shorter one (60), a valid 0, and every answer
 * that isn't a value (0xFFFF, another type, a refused read, no answer) as "not read": 240. */
static void test_siren_max_duration_from_setup(void)
{
    static const struct {
        bool set;
        uint16_t s;
        uint8_t type;
        uint8_t status;   /* the read refused with this */
        bool timeout;     /* the read never answered */
        bool read;
        uint16_t read_s;
        uint16_t asked;
        uint32_t lead_ms;
    } rows[] = {
        {false, 0, 0, 0, false, true, 240, 240, 51000u},
        {true, 60, 0, 0, false, true, 60, 60, 30000u},
        {true, 0, 0, 0, false, true, 0, ZBW_MAX_DURATION_ZERO_S, 10000u},
        {true, 0xFFFF, 0, 0, false, false, 0, 240, 51000u},
        {true, 60, ZBZ_ZCL_TYPE_U8, 0, false, false, 0, 240, 51000u},
        {false, 0, 0, ZBZ_ZCL_UNSUP_ATTRIBUTE, false, false, 0, 240, 51000u},
        {false, 0, 0, 0, true, false, 0, 240, 51000u},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        f_up();
        g_wd_set = rows[i].set;
        g_wd_s = rows[i].s;
        g_wd_type = rows[i].type;
        memset(&g_wd_fault, 0, sizeof(g_wd_fault));
        g_wd_fault.status = rows[i].status;
        g_wd_fault.timeout = rows[i].timeout;
        g_prep = prep_wd;
        uint16_t id = 0u;
        zbf_dev_t *sd = f_add("frient_sirzb_111", &id); /* setup DONE: the read is optional */
        zbf_fault(sd, ZSRC_ZB_STEP_POLL_CTRL, NULL);
        zbf_ep_t *ep = zbf_ep(sd, 43u);
        TEST_ASSERT_NOT_NULL(ep);
        if (rows[i].timeout) {
            TEST_ASSERT_TRUE(wd_reads(sd->ieee) >= 1u);
        } else {
            TEST_ASSERT_EQUAL_UINT(1u, wd_reads(sd->ieee));
        }
        const zbz_device_t *d = fdev(sd->ieee);
        TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SIREN, d->role);
        TEST_ASSERT_EQUAL(rows[i].read, d->wd_max_read);
        TEST_ASSERT_EQUAL_UINT16(rows[i].read_s, d->wd_max_s);
        siren_on(id);
        const zbw_siren_t *s = zbw_find(&FP.wd, id);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQUAL(rows[i].read, s->a.max_read);
        TEST_ASSERT_EQUAL_UINT16(rows[i].read_s, s->a.max_read_s);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, zbw_warn_seconds(&s->a));
        /* A burglary: the warning asked for, then its refresh just before it runs out. */
        f_news();
        frun(1000u);
        sim_arm_away_and_leave(&SIM, CODE_USER);
        f_news();
        frun(100u);
        sim_fault(&SIM, Z_WINDOW);
        const uint64_t t0 = g_now;
        f_news();
        frun(500u);
        const uint32_t first = ep->warnings;
        TEST_ASSERT_TRUE(first > 0u);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, warned_s(ep));
        const uint64_t due = t0 + (uint64_t)rows[i].asked * 1000u - rows[i].lead_ms;
        frun(due - 300u - g_now);
        TEST_ASSERT_EQUAL_UINT32(first, ep->warnings);
        frun(600u);
        TEST_ASSERT_EQUAL_UINT32(first + 1u, ep->warnings);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, warned_s(ep));
        TEST_ASSERT_EQUAL(ALARM_OK, sim_disarm(&SIM, 1u, CODE_USER));
        f_news();
        frun(500u);
        zbw_warning_t w;
        zbw_warning_info_decode(ep->last_cmd.payload[0], wd_layout(), &w);
        TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
    }
}

/* A REP_ZCL from `ieee`'s endpoint `ep`: the frame `b` as the radio hands it on (decoded as a
 * global frame when it decodes, else carried as it is: command 0x01, server to client). */
static void wd_answer(uint64_t ieee, uint8_t ep, const uint8_t *b, size_t n)
{
    zsrc_zb_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZB_REP_ZCL;
    r.ieee = ieee;
    r.endpoint = ep;
    if (zbz_zcl_decode_global(b, n, ZSRC_ZB_ZCL_IAS_WD, &r.u.zcl) != ZBZ_ZCL_OK) {
        r.u.zcl.cluster = ZSRC_ZB_ZCL_IAS_WD;
        r.u.zcl.command = ZBZ_ZCL_READ_ATTR_RSP;
        r.u.zcl.server_to_client = true;
        r.u.zcl.len = (uint8_t)(n - 3u);
        memcpy(r.u.zcl.payload, &b[3], n - 3u);
    }
    zbz_panel_on_report(&FP, g_now, &r);
}

/* ZB-023 (ZB03): a damaged or invalid answer (cut short, 0xFFFF, from an endpoint without IAS WD)
 * leaves what the setup read; a valid one replaces it, for the assigned siren's next warning
 * too; the value is stored with the device (field 21) and is there after a restart, for the
 * assignment loaded back. */
static void test_siren_max_duration_kept(void)
{
    f_up();
    g_wd_set = true;
    g_wd_s = 60u;
    g_wd_type = 0u;
    memset(&g_wd_fault, 0, sizeof(g_wd_fault));
    g_prep = prep_wd;
    uint16_t id = 0u;
    zbf_dev_t *sd = f_add("frient_sirzb_111", &id);
    siren_on(id);
    TEST_ASSERT_EQUAL_UINT16(60u, zbw_find(&FP.wd, id)->a.max_read_s);
    static const uint8_t cut[] = {0x18, 0x60, 0x01, 0x00, 0x00, 0x00, 0x21, 0x5A};
    static const uint8_t bad[] = {0x18, 0x61, 0x01, 0x00, 0x00, 0x00, 0x21, 0xFF, 0xFF};
    static const uint8_t ok90[] = {0x18, 0x62, 0x01, 0x00, 0x00, 0x00, 0x21, 0x5A, 0x00};
    wd_answer(sd->ieee, 43u, cut, sizeof(cut));
    wd_answer(sd->ieee, 43u, bad, sizeof(bad));
    wd_answer(sd->ieee, 7u, ok90, sizeof(ok90)); /* no such endpoint */
    TEST_ASSERT_TRUE(fdev(sd->ieee)->wd_max_read);
    TEST_ASSERT_EQUAL_UINT16(60u, fdev(sd->ieee)->wd_max_s);
    TEST_ASSERT_EQUAL_UINT16(60u, zbw_find(&FP.wd, id)->a.max_read_s);
    wd_answer(sd->ieee, 43u, ok90, sizeof(ok90));
    TEST_ASSERT_EQUAL_UINT16(90u, fdev(sd->ieee)->wd_max_s);
    TEST_ASSERT_EQUAL_UINT16(90u, zbw_find(&FP.wd, id)->a.max_read_s);
    TEST_ASSERT_EQUAL_UINT16(90u, zbw_warn_seconds(&zbw_find(&FP.wd, id)->a));

    /* Stored, and back after a restart (setup isn't repeated). */
    uint8_t blob[ZBZ_RECORD_MAX];
    memset(g_kv, 0, sizeof(g_kv));
    char key[ZBZ_KEY_LEN];
    size_t len = 0u;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&FP, key, blob, sizeof(blob), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            kv_put(key, blob, len);
        }
    }
    FP.wd.dirty = true;
    kv_put(ZBW_KEY, blob, zbz_panel_take_wd(&FP, blob, sizeof(blob)));
    zbz_panel_init(&FP, &g_port, &FZ, &FH, NULL, NULL);
    TEST_ASSERT_EQUAL(ZBZ_REG_OK, zbz_panel_load(&FP, &STORE));
    TEST_ASSERT_TRUE(fdev(sd->ieee)->wd_max_read);
    TEST_ASSERT_EQUAL_UINT16(90u, fdev(sd->ieee)->wd_max_s);
    const zbw_siren_t *s = zbw_find(&FP.wd, id);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->a.max_read);
    TEST_ASSERT_EQUAL_UINT16(90u, s->a.max_read_s);
}

/* ZB-023 (ZB01; alarm-devices.md §2 "smoke detectors with a built-in sounder (Heiman
 * SmokeSensor-EM)"): a smoke detector with an IAS WD server stays a sensor - its role, its smoke
 * input, its row and page - and takes a siren assignment and its test on that endpoint; the bell
 * sounds it. A device without an IAS WD server, a manufacturer-cluster siren (0xEF00) and a
 * keypad are still refused; a standalone siren is unchanged (test_siren_follows_the_bell). */
static void test_smoke_sounder_takes_a_siren(void)
{
    f_up();
    uint16_t id = 0u;
    zbf_dev_t *sd = f_add("heiman_smoke_em", &id);
    const zbz_device_t *d = fdev(sd->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, d->role);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
    TEST_ASSERT_TRUE(d->wd_max_read); /* its sounder's MaxDuration was read too (ZB03) */
    TEST_ASSERT_EQUAL_UINT16(240u, d->wd_max_s);
    const zsrc_zb_device_t *in = zsrc_zb_device(&FZ, sd->ieee, 1u);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_EQUAL_HEX16(0x0028u, in->zone_type);
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, id, &page));
    const uint8_t inputs = page.input_count;
    TEST_ASSERT_TRUE(inputs > 0u);

    siren_on(id);
    const zbw_siren_t *s = zbw_find(&FP.wd, id);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT8(1u, s->a.endpoint);
    TEST_ASSERT_TRUE(s->a.sleepy);
    /* Still a sensor: role, input, row and page as before. */
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, fdev(sd->ieee)->role);
    TEST_ASSERT_NOT_NULL(zsrc_zb_device(&FZ, sd->ieee, 1u));
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, id, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SENSOR, page.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(inputs, page.input_count);
    ui_zb_device_t rows[4];
    TEST_ASSERT_EQUAL_size_t(1u, zbz_panel_devices(&FP, g_now, rows, 4u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SENSOR, rows[0].kind);

    /* Its test, disarmed. */
    zbf_ep_t *ep = zbf_ep(sd, 1u);
    TEST_ASSERT_NOT_NULL(ep);
    f_news();
    frun(1000u);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN_TEST;
    req.user = 1u;
    req.id = id;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    (void)zbf_check_in(&F, sd); /* a sleepy device takes it in its wake window */
    frun(ZBZ_SIREN_TEST_MS + 1000u);
    req.kind = UI_ZB_REQ_DISMISS;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    const uint32_t tested = ep->warnings;
    TEST_ASSERT_TRUE(tested > 0u);
    frun(ZBW_CONFIRM_MS * 4u);

    /* A burglary: the bell sounds it. */
    sim_arm_away_and_leave(&SIM, CODE_USER);
    f_news();
    frun(100u);
    sim_fault(&SIM, Z_WINDOW);
    f_news();
    (void)zbf_check_in(&F, sd);
    frun(2000u);
    TEST_ASSERT_TRUE(ep->warnings > tested);
    TEST_ASSERT_EQUAL_UINT16(240u, warned_s(ep));
    zbw_warning_t w;
    zbw_warning_info_decode(ep->last_cmd.payload[0], ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    TEST_ASSERT_EQUAL(ALARM_OK, sim_disarm(&SIM, 1u, CODE_USER));
    f_news();
    frun(500u);

    /* Refused: no IAS WD server, a manufacturer-cluster siren, a keypad. */
    static const char *const others[] = {"centralite_3300", "tuya_ts0601_siren", "frient_kepzb_110"};
    for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++) {
        uint16_t oid = 0u;
        const zbf_dev_t *od = f_add_ended(others[k], &oid, false);
        TEST_ASSERT_NOT_EQUAL(ZBZ_SETUP_RUNNING, fdev(od->ieee)->setup);
        TEST_ASSERT_NOT_EQUAL(ZBZ_ROLE_SIREN, fdev(od->ieee)->role);
        memset(&req, 0, sizeof(req));
        req.user = 1u;
        req.id = oid;
        req.kind = UI_ZB_REQ_SIREN;
        req.siren.partitions = 1u;
        req.siren.burglary = true;
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_INVALID, fask(&req), others[k]);
        req.kind = UI_ZB_REQ_SIREN_TEST;
        TEST_ASSERT_EQUAL_MESSAGE(UI_ZB_REQ_INVALID, fask(&req), others[k]);
        TEST_ASSERT_NULL(zbw_find(&FP.wd, oid));
    }
}

/* ZB-024 (alarm-devices.md §3-§4): a keypad arms and disarms the real engine with its code, through
 * the owner's queue to the alarm task; a wrong code counts toward that keypad's own lockout; a fob
 * arms as its user; a panic gets through; a post the owner couldn't queue is forgotten (the
 * retry runs); refusals carry the request's own sequence number; the assignments store and load
 * back. */
static void test_keypad_and_fob_against_the_engine(void)
{
    f_up();
    g_taken = 1u << 0; /* the panel's own keypad 1 */
    uint16_t kid = 0u;
    zbf_dev_t *kd = f_add("frient_kepzb_110", &kid);
    const uint8_t KEP = 44u;
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_KEYPAD;
    req.user = 1u;
    req.id = kid;
    req.keypad.partition = 1u;
    req.keypad.show_state = true;
    /* ZB-024: a keypad's buttons offer Off or their one audible alarm - no silent panic */
    req.keypad.panic_mask = (uint8_t)((1u << ALARM_PANIC_POLICE) | (1u << ALARM_PANIC_SILENT));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, fask(&req));
    TEST_ASSERT_EQUAL_HEX32(0u, zbz_panel_devices_held(&FP));
    req.keypad.panic_mask = (uint8_t)(1u << ALARM_PANIC_POLICE);
    (void)zbf_check_in(&F, kd); /* awake for its greeting */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    TEST_ASSERT_EQUAL_HEX32(1u << 1, zbz_panel_devices_held(&FP));
    f_news();
    frun(200u);
    ui_zb_device_page_t page;
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, kid, &page));
    TEST_ASSERT_EQUAL_UINT8(2u, page.keypad.device);
    TEST_ASSERT_TRUE(page.keypad.live);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_KEYPAD, page.dev.kind);

    /* arm, then disarm, with Alex's code */
    f_arm(kd, KEP, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_UINT(1u, FQN);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, FQ[0].source.kind);
    TEST_ASSERT_EQUAL_UINT16(2u, FQ[0].source.id);
    TEST_ASSERT_FALSE(state_holds_code(CODE_USER)); /* nothing of it stays in the panel */
    TEST_ASSERT_EQUAL(ALARM_OK, f_task());
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, f_arm_note(kd, KEP));
    f_news();
    EXPECT_STATE(&SIM, 1, ALARM_STATE_EXIT_DELAY);
    f_arm(kd, KEP, ZBA_ARM_DISARM, CODE_USER);
    TEST_ASSERT_EQUAL(ALARM_OK, f_task());
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, f_arm_note(kd, KEP));
    f_news();
    EXPECT_STATE(&SIM, 1, ALARM_STATE_DISARMED);

    /* the owner's queue full: forgotten, so the keypad's retry runs (not a duplicate) */
    g_post_refused = true;
    f_arm(kd, KEP, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_UINT(0u, FQN);
    g_post_refused = false;
    TEST_ASSERT_TRUE(zbf_repeat(&F, kd));
    frun(100u);
    TEST_ASSERT_EQUAL_UINT(1u, FQN);
    TEST_ASSERT_EQUAL(ALARM_OK, f_task());
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, f_arm_note(kd, KEP));
    f_news();
    /* answered, then sent again: the first answer again, not run twice */
    const unsigned armed = sim_count_events(&SIM, ALARM_EV_ARMED);
    frun(200u); /* its Panel Status Changed delivered */
    const uint32_t answers = zbf_ep(kd, KEP)->commands;
    TEST_ASSERT_TRUE(zbf_repeat(&F, kd));
    frun(200u);
    TEST_ASSERT_EQUAL_UINT(0u, FQN);
    TEST_ASSERT_EQUAL_UINT(armed, sim_count_events(&SIM, ALARM_EV_ARMED));
    TEST_ASSERT_EQUAL_UINT32(answers + 1u, zbf_ep(kd, KEP)->commands);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, f_arm_note(kd, KEP));
    f_arm(kd, KEP, ZBA_ARM_DISARM, CODE_USER);
    TEST_ASSERT_EQUAL(ALARM_OK, f_task());
    f_news();

    /* five wrong codes lock that keypad out; the panel's own keypad 1 still works */
    for (unsigned i = 0; i < 5u; i++) {
        f_arm(kd, KEP, ZBA_ARM_ALL, "0000");
        TEST_ASSERT_EQUAL(ALARM_ERR_INVALID_CODE, f_task());
        TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, f_arm_note(kd, KEP));
    }
    f_arm(kd, KEP, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL(ALARM_ERR_LOCKED_OUT, f_task());
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, f_arm_note(kd, KEP));
    EXPECT_EVENTS(&SIM, ALARM_EV_LOCKOUT, 1);
    EXPECT_STATE(&SIM, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL(ALARM_OK, sim_arm(&SIM, 1u, CODE_USER, ALARM_LEVEL_STAY, 0u));
    TEST_ASSERT_EQUAL(ALARM_OK, sim_disarm(&SIM, 1u, CODE_USER));
    f_news();

    /* a panic gets through, and its Default Response carries the keypad's own sequence number */
    f_send(kd, KEP, ZBA_CMD_PANIC, NULL, 0u, true);
    const uint8_t tsn = f_last_tsn(kd->ieee);
    TEST_ASSERT_EQUAL_UINT(1u, FQN);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, FQ[0].cmd.type);
    TEST_ASSERT_EQUAL(ALARM_OK, f_task());
    TEST_ASSERT_TRUE(sim_alarm_logged(&SIM, 0, ALARM_CAUSE_PANIC_POLICE));
    TEST_ASSERT_TRUE(g_ndrsp > 0u);
    TEST_ASSERT_EQUAL_UINT64(kd->ieee, g_drsp[g_ndrsp - 1u].ieee);
    TEST_ASSERT_EQUAL_HEX8(tsn, g_drsp[g_ndrsp - 1u].tsn);
    TEST_ASSERT_EQUAL_HEX8(ZBA_CMD_PANIC, g_drsp[g_ndrsp - 1u].command);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, g_drsp[g_ndrsp - 1u].status);
    /* a refused one (medical isn't allowed) too: at once, nothing posted */
    f_send(kd, KEP, ZBA_CMD_EMERGENCY, NULL, 0u, true);
    TEST_ASSERT_EQUAL_UINT(0u, FQN);
    TEST_ASSERT_EQUAL_HEX8(f_last_tsn(kd->ieee), g_drsp[g_ndrsp - 1u].tsn);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_NOT_AUTHORIZED, g_drsp[g_ndrsp - 1u].status);
    TEST_ASSERT_EQUAL(ALARM_OK, sim_disarm(&SIM, 1u, CODE_MASTER));
    f_news();

    /* a fob arms as Alex (user 3); one fob per user */
    uint16_t fid = 0u;
    zbf_dev_t *fd = f_add("heiman_hs1rc", &fid);
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_FOB;
    req.user = 1u;
    req.id = fid;
    req.fob.user = 3u;
    req.fob.partition = 1u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, fask(&req));
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, fid, &page));
    TEST_ASSERT_EQUAL_UINT8(3u, page.fob.user);
    TEST_ASSERT_TRUE(page.fob.live);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_KEYFOB, page.dev.kind);
    req.fob.user = (uint8_t)(ALARM_MAX_USERS + 1u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_INVALID, fask(&req));
    f_arm(fd, 1u, ZBA_ARM_ALL, NULL);
    TEST_ASSERT_EQUAL_UINT(1u, FQN);
    TEST_ASSERT_EQUAL_UINT8(3u, FQ[0].user);
    TEST_ASSERT_EQUAL(ALARM_OK, f_task());
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, f_arm_note(fd, 1u));
    f_news();
    EXPECT_STATE(&SIM, 1, ALARM_STATE_EXIT_DELAY);
    const alarm_event_t *ev = sim_last_event(&SIM, ALARM_EV_ARMED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(3u, ev->user);

    /* stored, and loaded back: the keypad keeps its number, the fob its user */
    uint8_t blob[ZBZ_CODES_BLOB_MAX];
    memset(g_kv, 0, sizeof(g_kv));
    char key[ZBZ_KEY_LEN];
    size_t len = 0u;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&FP, key, blob, sizeof(blob), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            kv_put(key, blob, len);
        }
    }
    len = zbz_panel_take_ace(&FP, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0u && len <= ZBA_BLOB_MAX);
    kv_put(ZBA_KEY, blob, len);
    const uint16_t flags = FP.ace.keypad[0].model_flags;
    zbz_panel_init(&FP, &g_port, &FZ, &FH, NULL, NULL);
    TEST_ASSERT_EQUAL(ZBZ_REG_OK, zbz_panel_load(&FP, &STORE));
    TEST_ASSERT_EQUAL_HEX32(1u << 1, zbz_panel_devices_held(&FP));
    TEST_ASSERT_EQUAL_UINT16(flags, FP.ace.keypad[0].model_flags);
    TEST_ASSERT_TRUE(zbz_panel_device(&FP, g_now, fid, &page));
    TEST_ASSERT_EQUAL_UINT8(3u, page.fob.user);
    TEST_ASSERT_FALSE(FP.ace.dirty);
    g_port.ace_post = NULL;
}

/* ZB-012 (devices.md §5): Basic is read in three - the manufacturer, the model, the rest - so
 * IKEA's PARASOLL, whose manufacturer and model with the others need 51 bytes of a 48-byte
 * answer, is named in full; every answer fits. */
static void test_basic_read_split(void)
{
    f_up();
    uint16_t id = 0u;
    zbf_dev_t *d = f_add("ikea_parasoll", &id);
    const zbz_device_t *r = fdev(d->ieee);
    TEST_ASSERT_EQUAL_STRING("IKEA of Sweden", r->manufacturer);
    TEST_ASSERT_EQUAL_STRING("PARASOLL Door/Window Sensor", r->model);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, r->role);
    unsigned reads = 0u;
    for (size_t i = 0; i < zbf_log_count(&F); i++) {
        const zbf_frame_t *f = zbf_log_at(&F, i);
        if (f->ieee != d->ieee || f->kind != ZBF_FRAME_ZCL || f->cluster != ZSRC_ZB_ZCL_BASIC) {
            continue;
        }
        if (!f->rx) {
            reads++;
        } else {
            TEST_ASSERT_TRUE(f->len <= 3u + ZSRC_ZB_ZCL_PAYLOAD_MAX);
        }
    }
    TEST_ASSERT_EQUAL_UINT(3u, reads);
}

/* ==== the binding gate around requests and the replacement (ZB21, RF32) ======================== */

/* The owner's hooks (zbz_gate_port_t) as zigbee_service gives them, over the real gate
 * (zone_hub/zone_bind_gate.h) and the real engine: every engine step bracketed as alarm_service
 * brackets it, and a household arm injected at each boundary - before the request runs (armed
 * since the page's snapshot), right after the reservation, inside an engine step that overlaps
 * the claim, and while the claimed change runs. */
typedef enum {
    GA_NONE,
    GA_BEFORE,      /* between the page's snapshot and the zigbee task (before the reservation) */
    GA_RESERVED,    /* after the reservation, before the claim (at the first lock) */
    GA_IN_STEP,     /* an engine step that arms overlaps the claim */
    GA_CHANGING,    /* while the claimed change runs (the `after` hook) */
} gate_arm_at_t;

static sim_t GS;
static gate_arm_at_t g_arm_at;
static bool g_armed_tried;
static alarm_result_t g_arm_result;
static bool g_stepping;     /* an engine step is running (GA_IN_STEP) */
static int g_lock_depth;
static unsigned g_reserves, g_claims, g_ends, g_pauses, g_afters;
static bool g_other_change; /* another changer (zone_service) holds the gate */

static void gs_step_begin(void)
{
    const bool hold = zone_bind_gate_step_begin(&g_gate);
    alarm_engine_hold_arming(&GS.eng, hold);
}

static void gs_step_end(void)
{
    zone_bind_gate_step_end(&g_gate, zone_bind_gate_all_disarmed(&GS.eng));
}

static alarm_result_t gs_household_arm(void)
{
    gs_step_begin();
    const alarm_result_t r = sim_arm(&GS, 1u, CODE_USER, ALARM_LEVEL_AWAY, 0u);
    gs_step_end();
    return r;
}

static zone_bind_reserve_t g_reserve(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_INT(0, g_lock_depth); /* waits for the alarm task: never under a lock */
    g_reserves++;
    gs_step_begin();
    const zone_bind_reserve_t r =
        zone_bind_gate_try(&g_gate, zone_bind_gate_all_disarmed(&GS.eng), GS.now);
    gs_step_end();
    return r;
}

static zone_bind_claim_t g_claim(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_INT(1, g_lock_depth);
    g_claims++;
    if (g_arm_at == GA_IN_STEP && !g_armed_tried) {
        g_armed_tried = true;
        gs_step_begin(); /* the step runs on the alarm task as the claim is asked */
        g_stepping = true;
    }
    return zone_bind_gate_claim(&g_gate, GS.now);
}

static void g_end(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_INT(1, g_lock_depth); /* before the lock goes: the owner's unlock stores */
    g_ends++;
    zone_bind_gate_end(&g_gate);
}

static void g_lock(void *ctx)
{
    (void)ctx;
    g_lock_depth++;
    if (g_arm_at == GA_RESERVED && !g_armed_tried && g_gate.state == ZONE_BIND_GATE_RESERVED) {
        g_armed_tried = true;
        g_arm_result = gs_household_arm(); /* the alarm task doesn't wait for the zone lock */
    }
}

static void g_unlock(void *ctx)
{
    (void)ctx;
    g_lock_depth--;
}

static void g_pause(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_INT(0, g_lock_depth); /* never a wait under the zone lock */
    g_pauses++;
    GS.now += 1u;
    if (g_stepping) {
        g_stepping = false;
        g_arm_result = sim_arm(&GS, 1u, CODE_USER, ALARM_LEVEL_AWAY, 0u); /* the step arms */
        gs_step_end();
    }
}

static void g_after(void *ctx, const ui_zb_request_t *req, ui_zb_req_result_t r)
{
    (void)ctx;
    (void)req;
    (void)r;
    g_afters++;
    if (g_arm_at == GA_CHANGING && !g_armed_tried) {
        g_armed_tried = true;
        g_arm_result = gs_household_arm();
    }
}

static const zbz_gate_port_t GATE = {.ctx = NULL, .reserve = g_reserve, .claim = g_claim,
                                     .end = g_end, .lock = g_lock, .unlock = g_unlock,
                                     .pause = g_pause, .after = g_after};

static void gate_up(gate_arm_at_t at)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_start(&GS, &cfg, NULL);
    zone_bind_gate_init(&g_gate);
    g_arm_at = at;
    g_armed_tried = false;
    g_arm_result = ALARM_OK;
    g_stepping = false;
    g_lock_depth = 0;
    g_reserves = g_claims = g_ends = g_pauses = g_afters = 0u;
    g_other_change = false;
}

/* The page's request, its `allowed` a snapshot taken on the page's task (installer mode, every
 * partition disarmed); the arm, when asked GA_BEFORE, comes after it. */
static ui_zb_req_result_t gated(uint8_t kind, uint16_t id, uint8_t channel)
{
    const bool allowed = zone_bind_gate_all_disarmed(&GS.eng);
    if (g_arm_at == GA_BEFORE) {
        g_armed_tried = true;
        g_arm_result = gs_household_arm();
    }
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = 1u;
    req.id = id;
    req.channel = channel;
    const ui_zb_req_result_t r = zbz_panel_request_gated(&g_panel, g_now, &req, allowed, &GATE);
    TEST_ASSERT_EQUAL_INT(0, g_lock_depth);
    return r;
}

static void assert_gate_left_idle(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, g_gate.state);
    gs_step_begin();
    TEST_ASSERT_FALSE(alarm_engine_arming_held(&GS.eng)); /* nothing holds arming afterwards */
    gs_step_end();
}

/* ZB21, ZB-002, ZB-016: a channel change asked with every partition disarmed reaches the radio
 * under a claimed gate, and the gate is idle again after it. */
static void test_gate_disarmed_channel_goes_through(void)
{
    gate_up(GA_NONE);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, gated(UI_ZB_REQ_CHANNEL, 0u, 25u));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_CHANNEL));
    TEST_ASSERT_EQUAL_UINT(1u, g_reserves);
    TEST_ASSERT_EQUAL_UINT(1u, g_claims);
    TEST_ASSERT_EQUAL_UINT(1u, g_ends);
    assert_gate_left_idle();
}

/* ZB21, ZB-016, ZB-017, ZB-013, ZB-018: armed between the page's snapshot and the zigbee task -
 * CHANNEL, RESET, REMOVE and RESTART are NOT_ALLOWED and the radio is never called. */
static void test_gate_armed_after_the_snapshot_calls_no_radio(void)
{
    const zbsig_t *s = sig("third_reality_3rds17bz.json");
    const uint16_t id = add_device(s);
    const size_t leaves = count(C_LEAVE);
    static const uint8_t KINDS[] = {UI_ZB_REQ_CHANNEL, UI_ZB_REQ_RESET, UI_ZB_REQ_REMOVE,
                                    UI_ZB_REQ_RESTART};
    for (size_t k = 0; k < sizeof(KINDS); k++) {
        gate_up(GA_BEFORE);
        TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_ALLOWED, gated(KINDS[k], id, 25u));
        TEST_ASSERT_EQUAL_INT(ALARM_OK, g_arm_result); /* the arm itself went ahead */
        TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, sim_status(&GS, 1u).state);
        TEST_ASSERT_EQUAL_UINT(0u, g_claims);
        TEST_ASSERT_EQUAL_UINT(0u, g_ends); /* nothing was reserved */
        TEST_ASSERT_NOT_EQUAL(UI_ZB_OP_RESTART, progress().op);
    }
    TEST_ASSERT_EQUAL_size_t(0u, count(C_CHANNEL));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_RESET));
    TEST_ASSERT_EQUAL_size_t(leaves, count(C_LEAVE));
    TEST_ASSERT_NOT_NULL(dev(s->ieee));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, g_gate.state);
}

/* ZB21: an arm after the reservation, before the claim, goes ahead (a reservation never holds
 * arming) and the request is refused without the radio; likewise an arm in an engine step that
 * overlaps the claim (RETRY, the lock let go for the pause, then refused). */
static void test_gate_arm_before_the_claim_wins(void)
{
    static const gate_arm_at_t AT[] = {GA_RESERVED, GA_IN_STEP};
    for (size_t a = 0; a < sizeof(AT) / sizeof(AT[0]); a++) {
        for (int kind = 0; kind < 2; kind++) {
            gate_up(AT[a]);
            const uint8_t k = kind == 0 ? (uint8_t)UI_ZB_REQ_CHANNEL : (uint8_t)UI_ZB_REQ_RESET;
            TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_ALLOWED, gated(k, 0u, 25u));
            TEST_ASSERT_TRUE(g_armed_tried);
            TEST_ASSERT_EQUAL_INT(ALARM_OK, g_arm_result);
            TEST_ASSERT_EQUAL_UINT(1u, g_ends); /* the refused claim is ended */
            TEST_ASSERT_EQUAL_UINT(AT[a] == GA_IN_STEP ? 1u : 0u, g_pauses);
            TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, g_gate.state);
        }
    }
    TEST_ASSERT_EQUAL_size_t(0u, count(C_CHANNEL));
    TEST_ASSERT_EQUAL_size_t(0u, count(C_RESET));
}

/* ZB21, CORE-ARM-013: an arm while the claimed request runs is held - the change finishes first -
 * and arming goes again as soon as the claim ends. */
static void test_gate_arm_during_the_change_is_held(void)
{
    gate_up(GA_CHANGING);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, gated(UI_ZB_REQ_RESET, 0u, 0u));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_RESET));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_HELD, g_arm_result);
    assert_gate_left_idle();
    TEST_ASSERT_EQUAL_INT(ALARM_OK, gs_household_arm());
}

/* ZB21: a gate another change holds (a zone binding from the zone editor) is asked again
 * ZBZ_GATE_TRIES times, then the answer is BUSY - not "not allowed" - with nothing done, and the
 * other change's reservation is left alone. */
static void test_gate_busy_is_busy_and_bounded(void)
{
    gate_up(GA_NONE);
    TEST_ASSERT_TRUE(zone_bind_gate_take(&g_gate, true, GS.now)); /* the other changer's */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_BUSY, gated(UI_ZB_REQ_CHANNEL, 0u, 25u));
    TEST_ASSERT_EQUAL_UINT(ZBZ_GATE_TRIES, g_reserves);
    TEST_ASSERT_EQUAL_UINT(ZBZ_GATE_TRIES - 1u, g_pauses);
    TEST_ASSERT_EQUAL_UINT(0u, g_claims);
    TEST_ASSERT_EQUAL_UINT(0u, g_ends);
    TEST_ASSERT_EQUAL_UINT(0u, g_afters);
    TEST_ASSERT_EQUAL_size_t(0u, count(C_CHANNEL));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_RESERVED, g_gate.state);
    /* freed between two tries: the second one goes through */
    zone_bind_gate_end(&g_gate);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, gated(UI_ZB_REQ_CHANNEL, 0u, 25u));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_CHANNEL));
}

/* ZB21, ZB-016: requests that don't read `allowed` (SURVEY, PING) and requests refused without
 * it never reserve; a request without `allowed` is NOT_ALLOWED as before. */
static void test_gate_only_for_allowed_requests(void)
{
    gate_up(GA_NONE);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SURVEY;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, zbz_panel_request_gated(&g_panel, g_now, &req, false, &GATE));
    TEST_ASSERT_EQUAL_size_t(1u, count(C_SURVEY));
    req.kind = UI_ZB_REQ_CHANNEL;
    req.channel = 25u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_ALLOWED,
                      zbz_panel_request_gated(&g_panel, g_now, &req, false, &GATE));
    TEST_ASSERT_EQUAL_UINT(0u, g_reserves);
    TEST_ASSERT_EQUAL_UINT(2u, g_afters);
    TEST_ASSERT_EQUAL_INT(0, g_lock_depth);
    TEST_ASSERT_TRUE(zbz_panel_needs_allowed(UI_ZB_REQ_RESET));
    TEST_ASSERT_TRUE(zbz_panel_needs_allowed(UI_ZB_REQ_RESTART));
    TEST_ASSERT_FALSE(zbz_panel_needs_allowed(UI_ZB_REQ_SURVEY));
    TEST_ASSERT_FALSE(zbz_panel_needs_allowed(UI_ZB_REQ_DISMISS));
}

/* The panel as the firmware runs it: a replacement's rebinds wait for the gate. */
static void replace_waits(void)
{
    g_replace_waits = true;
    fresh(&RADIO);
    g_replace_waits = false;
    bring_up();
}

/* RF32, ZB-013: with port.replace_waits, the new device found moves nothing by itself; the gated
 * pass moves its zones under one claim, every rebind finding the gate CHANGING. */
static void test_replace_waits_for_the_gate(void)
{
    replace_waits();
    gate_up(GA_NONE);
    const zbsig_t *old = sig("sonoff_snzb_04pr2.json");
    const zbsig_t *repl = sig("third_reality_3rds17bz.json");
    const uint16_t old_id = add_device(old);
    bind_zone(3u, old->ieee, 1u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id, true));
    zbz_panel_replace_gated(&g_panel, g_now, &GATE); /* nothing found yet: no reservation */
    TEST_ASSERT_EQUAL_UINT(0u, g_reserves);
    join(repl->ieee, repl->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(repl);
    TEST_ASSERT_TRUE(zbz_panel_replace_waiting(&g_panel));
    TEST_ASSERT_EQUAL_UINT(0u, g_rebinds);
    TEST_ASSERT_EQUAL_UINT64(old->ieee, g_bind[3].in[ZSRC_ROLE_MAIN].addr.id);
    TEST_ASSERT_NOT_EQUAL(UI_ZB_STEP_DONE, progress().step); /* still listening */
    g_rebind_gated = true;
    zbz_panel_replace_gated(&g_panel, g_now, &GATE);
    TEST_ASSERT_FALSE(zbz_panel_replace_waiting(&g_panel));
    const ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_REPLACED, g.result);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT64(repl->ieee, g_bind[3].in[ZSRC_ROLE_MAIN].addr.id);
    TEST_ASSERT_NULL(dev(old->ieee));
    TEST_ASSERT_EQUAL_UINT(1u, g_reserves);
    TEST_ASSERT_EQUAL_UINT(1u, g_claims);
    TEST_ASSERT_EQUAL_UINT(1u, g_ends);
    assert_gate_left_idle();
}

/* RF32, ZB-013: armed after the replacement was found - before the reservation, or between the
 * reservation and the claim - FAILED, no rebind, the old device keeps its zones and the new one
 * stays added. */
static void test_replace_armed_meanwhile_moves_nothing(void)
{
    static const gate_arm_at_t AT[] = {GA_BEFORE, GA_RESERVED, GA_IN_STEP};
    const zbsig_t *old = sig("sonoff_snzb_04pr2.json");
    for (size_t a = 0; a < sizeof(AT) / sizeof(AT[0]); a++) {
        replace_waits();
        gate_up(AT[a]);
        zbsig_t repl = *sig("third_reality_3rds17bz.json");
        const uint16_t old_id = add_device(old);
        bind_zone(3u, old->ieee, 1u);
        const zone_hub_binding_t keep3 = g_bind[3];
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id, true));
        join(repl.ieee, repl.nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
        run_setup(&repl);
        TEST_ASSERT_TRUE(zbz_panel_replace_waiting(&g_panel));
        if (AT[a] == GA_BEFORE) {
            g_armed_tried = true;
            g_arm_result = gs_household_arm();
        }
        zbz_panel_replace_gated(&g_panel, g_now, &GATE);
        TEST_ASSERT_TRUE(g_armed_tried);
        TEST_ASSERT_EQUAL_INT(ALARM_OK, g_arm_result);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
        TEST_ASSERT_EQUAL_UINT(0u, g_rebinds);
        TEST_ASSERT_EQUAL_MEMORY(&keep3, &g_bind[3], sizeof(keep3));
        TEST_ASSERT_NOT_NULL(dev(old->ieee));
        TEST_ASSERT_NOT_NULL(dev(repl.ieee));
        TEST_ASSERT_FALSE(zbz_panel_replace_waiting(&g_panel));
        TEST_ASSERT_EQUAL_UINT(AT[a] == GA_BEFORE ? 0u : 1u, g_ends);
        TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, g_gate.state);
        TEST_ASSERT_EQUAL_UINT32(0u, last(C_PERMIT)->value); /* the window closed */
    }
}

/* RF32, ZB-013: the hub refusing a later zone puts back the ones already moved under the same
 * claim - the put-back is never refused by the gate - and the claim ends after it. */
static void test_replace_rollback_under_one_claim(void)
{
    replace_waits();
    gate_up(GA_NONE);
    const zbsig_t *old = sig("sonoff_snzb_04pr2.json");
    const zbsig_t *repl = sig("third_reality_3rds17bz.json");
    const uint16_t old_id = add_device(old);
    bind_zone(3u, old->ieee, 1u);
    bind_zone(5u, old->ieee, 1u);
    const zone_hub_binding_t keep3 = g_bind[3], keep5 = g_bind[5];
    g_rebind_refuse_zone = 5u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id, true));
    join(repl->ieee, repl->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(repl);
    g_rebind_gated = true;
    zbz_panel_replace_gated(&g_panel, g_now, &GATE);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_FAILED, progress().result);
    TEST_ASSERT_EQUAL_UINT(3u, g_rebinds); /* 3 moved, 5 refused, 3 put back */
    TEST_ASSERT_EQUAL_MEMORY(&keep3, &g_bind[3], sizeof(keep3));
    TEST_ASSERT_EQUAL_MEMORY(&keep5, &g_bind[5], sizeof(keep5));
    TEST_ASSERT_NOT_NULL(dev(old->ieee));
    TEST_ASSERT_EQUAL_UINT(1u, g_claims);
    TEST_ASSERT_EQUAL_UINT(1u, g_ends);
    assert_gate_left_idle();
}

/* RF32: a replacement stopped while it waited reserves nothing and moves nothing. */
static void test_replace_stopped_while_waiting(void)
{
    replace_waits();
    gate_up(GA_NONE);
    const zbsig_t *old = sig("sonoff_snzb_04pr2.json");
    const zbsig_t *repl = sig("third_reality_3rds17bz.json");
    const uint16_t old_id = add_device(old);
    bind_zone(3u, old->ieee, 1u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REPLACE, old_id, true));
    join(repl->ieee, repl->nwk, ZSRC_ZB_JOIN_STANDARD_KEY, false);
    run_setup(repl);
    TEST_ASSERT_TRUE(zbz_panel_replace_waiting(&g_panel));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u, true));
    TEST_ASSERT_FALSE(zbz_panel_replace_waiting(&g_panel));
    zbz_panel_replace_gated(&g_panel, g_now, &GATE);
    TEST_ASSERT_EQUAL_UINT(0u, g_reserves);
    TEST_ASSERT_EQUAL_UINT(0u, g_rebinds);
    TEST_ASSERT_EQUAL_UINT64(old->ieee, g_bind[3].in[ZSRC_ROLE_MAIN].addr.id);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_STOPPED, progress().result);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_setup_plain_ias_contact);
    RUN_TEST(test_setup_enrolls_a_sensor_heard_before);
    RUN_TEST(test_setup_sleepy_waits_for_its_wake);
    RUN_TEST(test_setup_router_retries_then_stops);
    RUN_TEST(test_setup_not_a_sensor);
    RUN_TEST(test_setup_failure_stops_with_its_step);
    RUN_TEST(test_setup_retries_and_optional_steps);
    RUN_TEST(test_setup_of_every_signature);
    RUN_TEST(test_add_window_and_countdown);
    RUN_TEST(test_add_with_install_code);
    RUN_TEST(test_install_codes_waiting);
    RUN_TEST(test_remove_answered);
    RUN_TEST(test_remove_no_answer_then_forget);
    RUN_TEST(test_log_names_where_it_was_asked);
    RUN_TEST(test_replace);
    RUN_TEST(test_replace_refused_moves_nothing);
    RUN_TEST(test_ping);
    RUN_TEST(test_identify);
    RUN_TEST(test_survey_and_channel);
    RUN_TEST(test_network_reset);
    RUN_TEST(test_redo_erases_a_dropped_record_again);
    RUN_TEST(test_a_sensor_the_source_has_no_room_for_is_full);
    RUN_TEST(test_restart_and_retry);
    RUN_TEST(test_left_and_refused);
    RUN_TEST(test_known_device_joins_again);
    RUN_TEST(test_setup_waits_for_the_key_exchange);
    RUN_TEST(test_a_failed_key_exchange_is_left_alone);
    RUN_TEST(test_permission_levels);
    RUN_TEST(test_refusals);
    RUN_TEST(test_unsupported_hooks);
    RUN_TEST(test_nvs_reserve);
    RUN_TEST(test_registry_full);
    RUN_TEST(test_keypad_device_numbers);
    RUN_TEST(test_device_list_sort_and_words);
    RUN_TEST(test_device_page);
    RUN_TEST(test_device_page_words_match_the_source);
    RUN_TEST(test_store_take_redo_and_load);
    RUN_TEST(test_load_damaged_parts);
    RUN_TEST(test_cfg_frozen_fixture);
    RUN_TEST(test_log_order_and_overflow);
    RUN_TEST(test_radio_lost_and_back);
    RUN_TEST(test_walk_test);
    RUN_TEST(test_tags_are_the_panels);
    RUN_TEST(test_siren_follows_the_bell);
    RUN_TEST(test_siren_max_duration_from_setup);
    RUN_TEST(test_siren_max_duration_kept);
    RUN_TEST(test_smoke_sounder_takes_a_siren);
    RUN_TEST(test_keypad_and_fob_against_the_engine);
    RUN_TEST(test_basic_read_split);
    RUN_TEST(test_gate_disarmed_channel_goes_through);
    RUN_TEST(test_gate_armed_after_the_snapshot_calls_no_radio);
    RUN_TEST(test_gate_arm_before_the_claim_wins);
    RUN_TEST(test_gate_arm_during_the_change_is_held);
    RUN_TEST(test_gate_busy_is_busy_and_bounded);
    RUN_TEST(test_gate_only_for_allowed_requests);
    RUN_TEST(test_replace_waits_for_the_gate);
    RUN_TEST(test_replace_armed_meanwhile_moves_nothing);
    RUN_TEST(test_replace_rollback_under_one_claim);
    RUN_TEST(test_replace_stopped_while_waiting);
    return UNITY_END();
}
