/*
 * test_zwz_panel.c - the panel's Z-Wave as the firmware's zwave_service runs it (docs/features/36
 * Z8): zwz_panel against zwave_host's fake ZAC93 and simulated devices, with the real zone source
 * and hub, on a fake clock. The owner here is what zwave_service is: it answers the pages'
 * questions through requests (as the installer would), keeps what zwz_panel_dirty() says is to
 * be stored across a restart of the panel, and records what zwz_panel_take_log() hands out.
 *
 * Requirements: ZW-005 (adding needs installer mode), ZW-011 (a device's own check-in: taken, not
 * taken, set again after a restart), ZW-016 (removal: CHECK, logged), ZW-017 (keys and the node
 * cache stored, and back after a restart), ZW-018 (region, reset), ZW-019 (the pages' contract:
 * ui_backend_t's zw_* answers), ZW-022 (the logged events), ZW-029 (a device that never
 * finished joining, was reset or is dead: its setup stopped, retried, and its removal as failed
 * with every outcome), ZW-033 (a device's software update: started only when allowed and possible,
 * a sleeper's at its wake-up and held awake, each outcome, logged, and its setup read again)
 * (docs/features/36), ZW-034/035 (what devices switch, and a garage door, end to end).
 */
#include <string.h>

#include "fake_zac93.h"
#include "fake_zdev.h"
#include "unity.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zwave.h"
#include "zw_host_crypto.h"
#include "zwave_zones/zwz_panel.h"
#include "zwave_zones/zw_feedback.h"
#include "zwave_proto/zw_cc_encap.h"
#include "zwave_proto/zw_cc_supervision.h"
#include "zwave_proto/zw_dsk.h"
#include "zwave_proto/zw_gbl.h"

#define MIN_MS 60000u

/* ---- the world ------------------------------------------------------------------------------- */

typedef struct {
    zwz_panel_t p;
    fake_zac93_t zac;
    fake_wire_t wire;
    zw_host_crypto_t hc;
    zw_crypto_port_t crypto;
    zsrc_zw_t src;
    zone_hub_t hub;
    /* what the owner keeps (NVS zwave) */
    zwz_panel_setup_t setup;
    uint8_t nodes[16384];
    size_t nodes_len;
    unsigned node_writes; /* node cache blobs stored (BL-152: none during a re-read) */
    zwz_checkin_t checkins[ZWZ_PANEL_CHECKINS];
    size_t checkin_n;
    unsigned key_writes, cfg_writes;
    uint8_t bind[ZONE_HUB_BLOB_MAX]; /* NVS zones/bind */
    size_t bind_len;
    /* what it logged */
    zwz_log_t log[32];
    size_t log_n;
    unsigned zone_events;
    zwf_t feedback;
    bool feedback_enabled;
    zwkc_t keyctl;
    uint8_t garage[ZWO_GARAGE_BLOB]; /* NVS zwave/garage */
    size_t garage_len;
    bool radio_trouble;      /* the hub's sensor_radio trouble */
    bool claim_on;
    zw_s2_rx_t claimed;      /* what the owner copied under "the lock" */
    uint8_t claimed_cmd[64];
    unsigned claims;
    unsigned engine_calls;
    alarm_cmd_t engine_cmd;  /* what reached the engine's port (its code checked, then erased) */
    bool disarmed;           /* every partition disarmed (SmartStart listens only then) */
    bool clock_set;          /* the panel's clock is set: W.local is its local time */
    zwz_local_time_t local;
    uint8_t prov[ZW_PROV_BLOB_MAX]; /* NVS zwave/prov */
    size_t prov_len;
    unsigned kp_calls;       /* ZW-038: port.keypad_reachable, its calls and the last one */
    uint8_t kp_device;
    bool kp_reachable;
    uint8_t refuse_nodes;    /* ZW-014: NVS refuses the node cache (REFUSE_*) */
    unsigned refused_writes;
} world_t;

/* Where a refused write of the node cache stops (zwz_store.h's order: gen, then nodes). */
enum { REFUSE_NONE = 0, REFUSE_GEN, REFUSE_CACHE };

static world_t W;

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&W.wire, bytes, len);
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
    W.zone_events++;
}

static void hub_changed(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&W.hub, W.bind, sizeof W.bind, &W.bind_len));
}

static void hub_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    (void)kind;
    W.radio_trouble = on;
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

static void sink_link_now(void *ctx, uint8_t kind, uint64_t t)
{
    (void)ctx;
    zone_hub_link_now(&W.hub, kind, t); /* as zone_service.c's sink */
}

static void n_control(void *ctx, uint64_t now, uint8_t b)
{
    (void)ctx;
    zwz_panel_on_control(&W.p, now, b);
}

static void n_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    (void)ctx;
    zwz_panel_on_frame(&W.p, now, f);
}

static void n_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    (void)ctx;
    zwz_panel_on_text(&W.p, now, b, n);
}

static void n_poll(void *ctx, uint64_t now)
{
    (void)ctx;
    zwz_panel_poll(&W.p, now);
    if (W.feedback_enabled) { zwf_poll(&W.feedback, &W.p.net, now, NULL, NULL); }
    zone_hub_tick(&W.hub, now);
}

static uint64_t n_deadline(void *ctx)
{
    (void)ctx;
    const uint64_t t = zwz_panel_next_deadline(&W.p);
    const uint64_t h = zone_hub_next_deadline(&W.hub, W.wire.now);
    return h < t ? h : t;
}

/* What the service does after unlocking: store what changed, log what happened. */
static void store(void)
{
    uint8_t buf[sizeof W.nodes];
    uint16_t gen = 0u;
    size_t n = zwz_panel_take_nodes(&W.p, buf, sizeof buf, &gen);
    if (n > 0u && W.refuse_nodes != REFUSE_NONE) {
        /* ZW-014: NVS refuses, as zwz_store_run orders it: the counter first, then the cache */
        if (W.refuse_nodes == REFUSE_CACHE) {
            W.setup.generation = gen;
        }
        W.refused_writes++;
        zwz_panel_redo(&W.p, ZWZ_DIRTY_NODES);
    } else if (n > 0u) {
        memcpy(W.nodes, buf, n);
        W.nodes_len = n;
        W.setup.generation = gen;
        W.node_writes++;
        zwz_panel_stored(&W.p, ZWZ_DIRTY_NODES); /* as zwz_store_run */
    }
    n = zwz_panel_take_keys(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_EQUAL_UINT(ZWZ_KEYS_BLOB, n);
        TEST_ASSERT_TRUE(zwz_keys_decode(&W.setup.keys, buf, n));
        W.key_writes++;
    }
    n = zwz_panel_take_cfg(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_TRUE(zwz_cfg_decode(&W.setup.cfg, buf, n));
        W.cfg_writes++;
    }
    n = zwz_panel_take_checkins(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_TRUE(zwz_checkins_decode(W.checkins, ZWZ_PANEL_CHECKINS, &W.checkin_n, buf, n));
    }
    n = zwz_panel_take_prov(&W.p, buf, sizeof buf);
    if (n > 0u) {
        memcpy(W.prov, buf, n);
        W.prov_len = n;
    }
    n = zwz_panel_take_garage(&W.p, buf, sizeof buf);
    if (n > 0u) {
        TEST_ASSERT_EQUAL_UINT(ZWO_GARAGE_BLOB, n);
        memcpy(W.garage, buf, n);
        W.garage_len = n;
    }
    TEST_ASSERT_EQUAL_UINT8(W.refuse_nodes != REFUSE_NONE ? (zwz_panel_dirty(&W.p) & ZWZ_DIRTY_NODES)
                                                          : 0u,
                            zwz_panel_dirty(&W.p));
    zwz_log_t l;
    while (zwz_panel_take_log(&W.p, &l)) {
        TEST_ASSERT_TRUE(W.log_n < sizeof W.log / sizeof W.log[0]);
        W.log[W.log_n++] = l;
    }
}

static void run(uint64_t t)
{
    fake_wire_run_until(&W.wire, t);
    TEST_ASSERT_FALSE_MESSAGE(W.wire.stuck, "the wire did not settle");
    store();
}

static void run_for(uint64_t ms)
{
    run(W.wire.now + ms);
}

/* The panel (re)starts with what the owner stored. */
static bool owner_command(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    if (!W.claim_on || !zwkc_claims(&W.keyctl, &W.p.net, rx, NULL) || rx->len > 64u) {
        return false;
    }
    W.claims++;
    W.claimed = *rx;
    memcpy(W.claimed_cmd, rx->command, rx->len);
    W.claimed.command = W.claimed_cmd;
    return true;
}

static bool engine_port(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *src,
                        zwk_reply_t *reply)
{
    (void)ctx;
    (void)src;
    W.engine_calls++;
    W.engine_cmd = *cmd;
    reply->result = ALARM_OK;
    return true;
}

static void owner_send_done(void *ctx, uint32_t tag, zw_s2_result_t result)
{
    (void)ctx;
    (void)zwf_done(&W.feedback, tag, result, W.wire.now);
}

/* ZW-035: where a garage's tilt sensor says the door is, as zwave_service answers it. */
static zwo_position_t port_position(void *ctx, uint16_t zone)
{
    (void)ctx;
    zone_hub_info_t in;
    if (!zone_hub_info(&W.hub, zone, &in) || !in.known || in.unreachable || in.check ||
        in.link_down) {
        return ZWO_POS_UNKNOWN;
    }
    return in.active ? ZWO_POS_OPEN : ZWO_POS_CLOSED;
}

static bool port_disarmed(void *ctx)
{
    (void)ctx;
    return W.disarmed;
}

static bool port_local_time(void *ctx, zwz_local_time_t *out)
{
    (void)ctx;
    *out = W.local;
    return W.clock_set;
}

static void port_keypad_reachable(void *ctx, uint8_t device, bool reachable)
{
    (void)ctx;
    W.kp_calls++;
    W.kp_device = device;
    W.kp_reachable = reachable;
}

static void boot(void)
{
    static const fake_host_t fh = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
    static const zone_hub_port_t hport = {NULL, hub_event, hub_changed, hub_trouble};
    static const zsrc_sink_t sink = {NULL,      sink_input, sink_event, sink_reachable,
                                     sink_link, sink_link_now};
    static const zwz_panel_port_t port = {.write = port_write, .binding = port_binding,
                                         .send_done = owner_send_done,
                                         .command = owner_command,
                                         .zone_position = port_position,
                                         .disarmed = port_disarmed,
                                         .local_time = port_local_time,
                                         .keypad_reachable = port_keypad_reachable};
    const uint64_t t = W.wire.now;
    zsrc_zw_init(&W.src, &sink);
    zone_hub_init(&W.hub, &hport);
    if (W.bind_len > 0u) {
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&W.hub, W.bind, W.bind_len));
    }
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&W.hub, zsrc_zw_ops(), &W.src, t));
    zone_hub_start(&W.hub, t);
    zwz_panel_init(&W.p, &port, &W.setup, W.checkins, W.checkin_n, &W.src, &W.crypto);
    if (W.nodes_len > 0u) {
        TEST_ASSERT_TRUE(zwz_panel_load_nodes(&W.p, W.nodes, W.nodes_len));
    }
    if (W.garage_len > 0u) {
        TEST_ASSERT_TRUE(zwz_panel_load_garage(&W.p, W.garage, W.garage_len));
    }
    if (W.prov_len > 0u) {
        TEST_ASSERT_TRUE(zwz_panel_load_prov(&W.p, W.prov, W.prov_len));
    }
    fake_wire_init(&W.wire, &W.zac, &fh, t);
    zwz_panel_start(&W.p, t);
    run(t + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&W.p.net.host));
}

void setUp(void)
{
    memset(&W, 0, sizeof W);
    zw_host_crypto_init(&W.hc, 0x50414E45u);
    W.crypto = zw_host_crypto_port(&W.hc);
    fake_zac93_init(&W.zac);
    W.setup.identity.firmware_major = 1u;
    W.setup.jam = true; /* as the firmware */
    W.setup.identity.manufacturer_id = 0xFFFFu;
    W.setup.identity.product_type = 0xFFFFu;
    W.setup.identity.product_id = 0xFFFFu;
    boot();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.zac.overflow, "the fake module ran out of room");
}

/* ---- helpers --------------------------------------------------------------------------------- */

static ui_zw_req_result_t ask(uint8_t kind, uint16_t node, bool allowed)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.user = 1u;
    r.node = node;
    r.generation = zwz_panel_generation(&W.p, node);
    const ui_zw_req_result_t res = zwz_panel_request(&W.p, W.wire.now, &r, allowed);
    run_for(0u);
    return res;
}

static ui_zw_progress_t progress(void)
{
    ui_zw_progress_t g;
    zwz_panel_progress(&W.p, W.wire.now, &g);
    return g;
}

static void device(fake_zdev_t *d, uint64_t seed, bool sleeps)
{
    fake_zdev_init(d, seed);
    d->m.answers = true;
    d->m.sleeps = sleeps;
}

/* "Add a device" as the pages do it: the request, the device's button, the installer's answers
 * (grant what it asks, type its PIN) until the pages say it is done; the result. */
static uint8_t include(fake_zdev_t *d, uint8_t grant)
{
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, d));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_ADD, progress().op);
    run_for(100u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_LISTENING, progress().step);
    TEST_ASSERT_TRUE(progress().seconds_left > 55u && progress().seconds_left <= 60u);
    TEST_ASSERT_TRUE(fake_zac93_press(&W.zac, W.wire.now, d));
    for (unsigned i = 0; i < 1200u; i++) {
        run_for(100u);
        const ui_zw_progress_t g = progress();
        if (g.step == UI_ZW_STEP_ASK_ALLOW) {
            ui_zw_request_t r;
            memset(&r, 0, sizeof r);
            r.kind = UI_ZW_REQ_ALLOW;
            r.keys = grant != 0u ? grant : g.requested;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
        } else if (g.step == UI_ZW_STEP_ASK_PIN) {
            TEST_ASSERT_EQUAL_STRING(d->dsk + 5, g.dsk + 5); /* the rest of its key; the PIN is
                                                               * what the installer types */
            ui_zw_request_t r;
            memset(&r, 0, sizeof r);
            r.kind = UI_ZW_REQ_PIN;
            r.pin = d->pin;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
        } else if (g.step == UI_ZW_STEP_DONE) {
            return g.result;
        }
    }
    TEST_FAIL_MESSAGE("the add did not end");
    return UI_ZW_RES_NONE;
}

static const zwz_log_t *logged(uint8_t type)
{
    for (size_t i = W.log_n; i > 0u; i--) {
        if (W.log[i - 1u].type == type) {
            return &W.log[i - 1u];
        }
    }
    return NULL;
}

static size_t logged_n(uint8_t type)
{
    size_t n = 0u;
    for (size_t i = 0; i < W.log_n; i++) {
        n += W.log[i].type == type;
    }
    return n;
}

/* Binds the device's input to a zone, as the zone editor does. */
static void bind(uint16_t zone, uint16_t node, uint8_t slot)
{
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, zwz_panel_generation(&W.p, node),
                                      &page));
    for (size_t i = 0; i < page.input_count; i++) {
        if (page.inputs[i].slot == slot) {
            zone_hub_binding_t b;
            memset(&b, 0, sizeof b);
            b.in[ZSRC_ROLE_MAIN].addr = page.inputs[i].addr;
            zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map,
                                  (alarm_device_class_t)page.inputs[i].device_class);
            TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&W.hub, zone, &b, W.wire.now));
            return;
        }
    }
    TEST_FAIL_MESSAGE("no such input");
}

/* A keypad-like device: woken by the panel's call (FLiRS), battery powered, Security 2. */
static void flirs_device(fake_zdev_t *d, uint64_t seed)
{
    device(d, seed, false);
    fake_zdev_listening(d);
    d->m.listening = false;
    d->m.flirs = true;
    d->m.secure_ccs[d->m.secure_n++] = ZW_CC_BATTERY; /* back: fake_zdev_listening took it */
    d->m.versions[d->m.versions_n++] = (fake_cc_version_t){ZW_CC_BATTERY, 1};
}

static const ui_zw_device_t *row_of(uint16_t node)
{
    static ui_zw_device_t list[8];
    const size_t n = zwz_panel_devices(&W.p, W.wire.now, list, 8u);
    for (size_t i = 0; i < n; i++) {
        if (list[i].node == node) {
            return &list[i];
        }
    }
    return NULL;
}

/* Runs a second at a time until the device's row is in `state` (or `ms` is over). */
static bool wait_state(uint16_t node, uint8_t state, uint64_t ms)
{
    for (uint64_t t = 0; t <= ms; t += 1000u) {
        const ui_zw_device_t *d = row_of(node);
        if (d != NULL && d->state == state) {
            return true;
        }
        run_for(1000u);
    }
    return false;
}

static ui_zw_req_result_t ask_pin(uint32_t pin)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_PIN;
    r.pin = pin;
    const ui_zw_req_result_t res = zwz_panel_request(&W.p, W.wire.now, &r, true);
    run_for(0u);
    return res;
}

/* The pages' add, up to its PIN question (the device asks for Authenticated or better). */
static void add_to_pin(fake_zdev_t *d)
{
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, d));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ADD, 0u, true));
    run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&W.zac, W.wire.now, d));
    for (unsigned i = 0; i < 600u && progress().step != UI_ZW_STEP_ASK_PIN; i++) {
        run_for(100u);
        if (progress().step == UI_ZW_STEP_ASK_ALLOW) {
            ui_zw_request_t r;
            memset(&r, 0, sizeof r);
            r.kind = UI_ZW_REQ_ALLOW;
            r.keys = ZW_S2_KEY_AUTH;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_ASK_PIN, progress().step);
}

static uint8_t add_end(void)
{
    for (unsigned i = 0; i < 3000u && progress().step != UI_ZW_STEP_DONE; i++) {
        run_for(100u);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    return progress().result;
}

/* ---- the tests ------------------------------------------------------------------------------- */

/* ZW-017: at the first start the network's keys are made and handed out once, bound to the
 * module's home ID; the pages see a working radio without devices. */
static void test_first_start(void)
{
    TEST_ASSERT_EQUAL_UINT(1u, W.key_writes);
    TEST_ASSERT_TRUE(W.setup.keys.bound);
    TEST_ASSERT_EQUAL_HEX32(W.zac.id.home_id, W.setup.keys.home_id);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_EQUAL_UINT16(0u, st.devices);
    TEST_ASSERT_EQUAL_UINT32(ZW_NET_CHECK_IN_S, st.check_in_s);
    TEST_ASSERT_EQUAL_HEX32(W.zac.id.home_id, st.home_id);
    TEST_ASSERT_TRUE(st.region_known);
    TEST_ASSERT_TRUE(st.region_count > 0u);
    ui_zw_device_t list[4];
    TEST_ASSERT_EQUAL_UINT(0u, zwz_panel_devices(&W.p, W.wire.now, list, 4u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    TEST_ASSERT_EQUAL_UINT(0u, W.log_n);
}

/* ZW-005, ZW-019, ZW-022: a secure door sensor added through the pages' requests - refused
 * outside installer mode; the steps as the pages follow them (listening with its countdown, the
 * security question, its PIN, reading what it can do); added, logged, listed, with its door
 * contact on its page; the ended operation dismissed. */
static void test_add_secure_device(void)
{
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_ADD, 0u, false));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    static fake_zdev_t door;
    device(&door, 0xD00Du, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, ZW_S2_KEY_AUTH));
    const ui_zw_progress_t g = progress();
    const uint16_t node = g.node;
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_EQUAL_UINT16(zwz_panel_generation(&W.p, node), g.generation);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_S2_AUTH, g.security);
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor", g.model);
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_ADDED);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(node, l->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_JOIN_S2_AUTH, l->arg2);
    TEST_ASSERT_EQUAL_UINT8(1u, l->user);
    TEST_ASSERT_EQUAL_UINT16(0u, l->zone);

    ui_zw_device_t list[4];
    TEST_ASSERT_EQUAL_UINT(1u, zwz_panel_devices(&W.p, W.wire.now, list, 4u));
    TEST_ASSERT_EQUAL_UINT16(node, list[0].node);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_READY, list[0].state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_S2_AUTH, list[0].security);
    TEST_ASSERT_FALSE(list[0].listening);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, list[0].device_class);
    TEST_ASSERT_EQUAL_UINT8(0u, list[0].zone_count);
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, g.generation, &page));
    TEST_ASSERT_EQUAL_UINT32(ZW_NET_CHECK_IN_S, page.check_in_s);
    TEST_ASSERT_EQUAL_UINT32(0u, page.check_in_next_s);
    TEST_ASSERT_TRUE(page.input_count >= 1u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_SLOT_CONTACT, page.inputs[0].slot);
    TEST_ASSERT_EQUAL_STRING("door contact", page.inputs[0].words);
    TEST_ASSERT_FALSE(zwz_panel_device(&W.p, W.wire.now, node,
                                       (uint16_t)((unsigned)g.generation + 1u), &page));
    bind(3u, node, ZSRC_ZW_SLOT_CONTACT);
    TEST_ASSERT_EQUAL_UINT(1u, zwz_panel_devices(&W.p, W.wire.now, list, 4u));
    TEST_ASSERT_EQUAL_UINT8(1u, list[0].zone_count);
    TEST_ASSERT_EQUAL_UINT16(3u, list[0].zones[0]);
    TEST_ASSERT_EQUAL_UINT16(3u, zwz_panel_zone_of(&W.p, node, g.generation));

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    TEST_ASSERT_NOT_EQUAL(0u, W.nodes_len); /* the node cache was stored */
}

/* ZW-022: a device without Security 2 joins not encrypted - an ordinary add for the pages (its
 * words say so), "added without encryption" in the log. */
static void test_add_plain_device(void)
{
    static fake_zdev_t pir;
    device(&pir, 0xB00Du, false);
    fake_zdev_listening(&pir);
    fake_zdev_no_s2(&pir);
    pir.m.secure_n = 0u;
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71};
    memcpy(pir.nif, NIF, sizeof NIF);
    pir.nif_len = sizeof NIF;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&pir, 0u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_NONE, progress().security);
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_ADDED_INSECURE);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_JOIN_NOT_SECURE, l->arg2);
    TEST_ASSERT_NULL(logged(ALARM_EV_ZWAVE_ADDED));
    ui_zw_device_t list[2];
    TEST_ASSERT_EQUAL_UINT(1u, zwz_panel_devices(&W.p, W.wire.now, list, 2u));
    TEST_ASSERT_TRUE(list[0].listening);
    TEST_ASSERT_FALSE(list[0].flirs);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_POWER_MAINS, list[0].power); /* listening, no Battery CC */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_NONE, list[0].security); /* its list: no Security 2 */

    /* "Check it now" on a listening device: it answers; then it goes quiet: no answer */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_PING, list[0].node, false));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_PING, progress().op);
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ANSWERED, progress().result);
    pir.lose_in = 1000u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_PING, list[0].node, false));
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_NO_ANSWER, progress().result);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask(UI_ZW_REQ_PING, 99u, false)); /* no such device */
}

/* ZW-016, ZW-022: "Remove a device" - its zone keeps its binding; the pages' result names the
 * model and the zone that now needs a sensor; the log names the zone. */
static void test_remove_device(void)
{
    static fake_zdev_t door;
    device(&door, 0xD00Eu, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    bind(2u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(5000u);
    W.log_n = 0u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_REMOVE, 0u, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE, 0u, true));
    run_for(200u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_REMOVE, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_LISTENING, progress().step);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&W.zac, W.wire.now, &door));
    run_for(3000u);
    const ui_zw_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, g.result);
    TEST_ASSERT_EQUAL_UINT16(node, g.node);
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor", g.model);
    TEST_ASSERT_EQUAL_UINT8(1u, g.zone_count);
    TEST_ASSERT_EQUAL_UINT16(2u, g.zones[0]);
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_REMOVED);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT16(2u, l->zone);
    TEST_ASSERT_EQUAL_UINT8(node, l->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_REMOVED_EXCLUDED, l->arg2);
    TEST_ASSERT_NULL(logged(ALARM_EV_ZWAVE_LEFT));
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&W.hub, 2u, &b)); /* the zone keeps it: CHECK "sensor removed" */
    ui_zw_device_t list[2];
    TEST_ASSERT_EQUAL_UINT(0u, zwz_panel_devices(&W.p, W.wire.now, list, 2u));
}

/* ---- SmartStart (network.md §4.2; ZW-024) ----------------------------------------------------- */

static ui_zw_prov_t prov_entry(void)
{
    ui_zw_prov_t list[UI_ZW_PROV_MAX];
    TEST_ASSERT_EQUAL_UINT(1u, zwz_panel_prov(&W.p, list, UI_ZW_PROV_MAX));
    return list[0];
}

/* ZW-024: a SmartStart entry carries its model's name from the table that names devices: a known
 * product gets it, an unknown one or a code with no product gets none */
static void test_smartstart_model_name(void)
{
    static fake_zdev_t door;
    run_for(2000u);
    device(&door, 0x55AAu, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &door));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_ADDED,
                          zwz_panel_prov_register(&W.p, W.wire.now, door.dsk, "Front door", true));
    TEST_ASSERT_EQUAL_STRING("", prov_entry().model); /* a typed key: no product */
    W.p.prov.e[0].manufacturer = 0x027Au;
    W.p.prov.e[0].product_type = 0x7000u;
    W.p.prov.e[0].product_id = 0xE001u;
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor", prov_entry().model);
    W.p.prov.e[0].product_id = 0x1234u; /* not in the table */
    TEST_ASSERT_EQUAL_STRING("", prov_entry().model);
}

/* ZW-024: a sensor registered by its key joins by itself once powered - only while nothing is
 * armed - is logged as added by the panel, survives a restart of the panel; removed by hand its
 * code goes with it unless kept; a key of a device already added reads as joined; the network
 * reset empties the list */
static void test_smartstart_on_the_panel(void)
{
    static fake_zdev_t door, plain;
    device(&door, 0x55AAu, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &door));

    /* what the pages may register */
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_NOT_ALLOWED,
                          zwz_panel_prov_register(&W.p, W.wire.now, door.dsk, "Front door", false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_BAD_CODE,
                          zwz_panel_prov_register(&W.p, W.wire.now, "12345", NULL, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_CHECKSUM,
                          zwz_panel_prov_register(&W.p, W.wire.now,
                                                  "90013278300351525354554142434445313233343521"
                                                  "2223242500100435301537022065520001000000300578",
                                                  NULL, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_ADDED,
                          zwz_panel_prov_register(&W.p, W.wire.now, door.dsk, "Front door", true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_UPDATED,
                          zwz_panel_prov_register(&W.p, W.wire.now, door.dsk, NULL, true));
    run_for(500u);
    TEST_ASSERT_TRUE(W.prov_len > 0u); /* stored */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_WAITING, prov_entry().state);
    TEST_ASSERT_EQUAL_STRING("Front door", prov_entry().name);

    /* armed: the radio doesn't listen for it */
    W.disarmed = false;
    run_for(2000u);
    TEST_ASSERT_FALSE(W.zac.smart_listen);
    W.disarmed = true;
    run_for(2000u);
    TEST_ASSERT_TRUE(W.zac.smart_listen);

    /* powered: it joins, secure, nobody asked; logged as the panel's doing */
    W.log_n = 0u;
    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&W.zac, W.wire.now, &door));
    for (unsigned i = 0; i < 1200u && prov_entry().state != UI_ZW_PROV_JOINED; i++) {
        run_for(100u);
    }
    const ui_zw_prov_t e = prov_entry();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_JOINED, e.state);
    TEST_ASSERT_NOT_EQUAL(0u, e.node);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op); /* no page follows it */
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_ADDED);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(0u, l->user);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_JOIN_S2_ACCESS, l->arg2);
    run_for(30000u); /* its setup */
    TEST_ASSERT_FALSE(W.zac.smart_listen); /* nothing waits */

    /* the panel restarts: the list comes back as it was */
    boot();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_JOINED, prov_entry().state);
    TEST_ASSERT_EQUAL_UINT16(e.node, prov_entry().node);

    /* removed by hand: its code is forgotten with it; the page may keep it */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE, 0u, true));
    run_for(200u);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&W.zac, W.wire.now, &door));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, progress().result);
    TEST_ASSERT_TRUE(progress().prov_forgot);
    ui_zw_prov_t none[2];
    TEST_ASSERT_EQUAL_UINT(0u, zwz_panel_prov(&W.p, none, 2u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_UPDATED,
                          zwz_panel_prov_change(&W.p, W.wire.now, NULL, UI_ZW_PROV_KEEP, 0u, true));
    TEST_ASSERT_FALSE(progress().prov_forgot);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_WAITING, prov_entry().state); /* it joins again */
    TEST_ASSERT_EQUAL_STRING("Front door", prov_entry().name);
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_NOT_FOUND,
                          zwz_panel_prov_change(&W.p, W.wire.now, NULL, UI_ZW_PROV_KEEP, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* its keys: a subset of what it may have, never none */
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_UPDATED,
                          zwz_panel_prov_change(&W.p, W.wire.now, door.pub, UI_ZW_PROV_GRANT,
                                                UI_ZW_KEY_S2_UNAUTH, true));
    TEST_ASSERT_EQUAL_HEX8(UI_ZW_KEY_S2_UNAUTH, prov_entry().grant);
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_BAD_CODE,
                          zwz_panel_prov_change(&W.p, W.wire.now, door.pub, UI_ZW_PROV_GRANT, 0u,
                                                true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_UPDATED,
                          zwz_panel_prov_change(&W.p, W.wire.now, door.pub, UI_ZW_PROV_OFF_OP, 0u,
                                                true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_OFF, prov_entry().state);
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_UPDATED,
                          zwz_panel_prov_change(&W.p, W.wire.now, door.pub, UI_ZW_PROV_FORGET, 0u,
                                                true));
    TEST_ASSERT_EQUAL_UINT(0u, zwz_panel_prov(&W.p, none, 2u));

    /* a key of a device added the usual way: registered as joined */
    device(&plain, 0x5A5Bu, false);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&plain, 0u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_JOINED_ALREADY,
                          zwz_panel_prov_register(&W.p, W.wire.now, plain.dsk, NULL, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_JOINED, prov_entry().state);

    /* the network reset empties it */
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_RESET;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    run_for(15000u);
    TEST_ASSERT_EQUAL_UINT(0u, zwz_panel_prov(&W.p, none, 2u));
}

/* The last command a device got with this CC and command; NULL: none. */
static const uint8_t *got_cmd(const fake_zdev_t *d, uint8_t cc, uint8_t cmd, size_t *len)
{
    for (size_t i = d->got_n; i > 0u; i--) {
        if (d->got_len[i - 1u] >= 2u && d->got[i - 1u][0] == cc && d->got[i - 1u][1] == cmd) {
            *len = d->got_len[i - 1u];
            return d->got[i - 1u];
        }
    }
    return NULL;
}

/* ZW-024 (network.md §9): a device's own Home Security "RF jamming detected" shows on its page
 * and is logged - once an hour at most; its idle and other notifications are not */
static void test_device_jamming_report(void)
{
    static fake_zdev_t lamp;
    device(&lamp, 0x7A11u, false);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&lamp, 0u));
    run_for(30000u);
    const uint16_t node = progress().node;
    const uint16_t gen = zwz_panel_generation(&W.p, node);
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(UI_ZW_NEVER, page.jam_report_s);
    static const uint8_t JAM[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x0C, 0x00};
    static const uint8_t IDLE[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x00, 0x01, 0x0C};
    const size_t before = logged_n(ALARM_EV_ZWAVE_JAMMING);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&lamp, W.wire.now, ZW_SEC_AUTO, IDLE, sizeof IDLE));
    run_for(2000u);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(UI_ZW_NEVER, page.jam_report_s);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&lamp, W.wire.now, ZW_SEC_AUTO, JAM, sizeof JAM));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, logged_n(ALARM_EV_ZWAVE_JAMMING));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)node, logged(ALARM_EV_ZWAVE_JAMMING)->arg);
    run_for(5u * 60000u);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_TRUE(page.jam_report_s >= 300u && page.jam_report_s < 310u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&lamp, W.wire.now, ZW_SEC_AUTO, JAM, sizeof JAM));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, logged_n(ALARM_EV_ZWAVE_JAMMING)); /* not twice an hour */
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_TRUE(page.jam_report_s < 5u);
    run_for(61u * 60000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&lamp, W.wire.now, ZW_SEC_AUTO, JAM, sizeof JAM));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(before + 2u, logged_n(ALARM_EV_ZWAVE_JAMMING));
}

/* ZW-024 (command-classes.md §5): a device asking the time (a keypad, a lock) gets the panel's
 * local time as it is then - encrypted as it asked - or "clock not set" when the panel's clock
 * isn't set; never an old or made-up time. */
static void test_time_answers(void)
{
    static fake_zdev_t keypad;
    device(&keypad, 0x7133u, false);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, 0u));
    run_for(30000u);
    static const uint8_t TIME_GET[] = {0x8A, 0x01};
    static const uint8_t DATE_GET[] = {0x8A, 0x03};

    W.clock_set = false;
    keypad.got_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, TIME_GET, 2u));
    run_for(2000u);
    size_t n = 0u;
    const uint8_t *r = got_cmd(&keypad, 0x8A, 0x02, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "a Time Get went unanswered");
    TEST_ASSERT_EQUAL_UINT(5u, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, r[2] & 0x80); /* clock not set */

    W.clock_set = true;
    W.local = (zwz_local_time_t){.year = 2026u, .month = 9u, .day = 27u, .hour = 14u, .minute = 5u,
                                 .second = 42u};
    keypad.got_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, TIME_GET, 2u));
    run_for(2000u);
    r = got_cmd(&keypad, 0x8A, 0x02, &n);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX8(14u, r[2]); /* set, 14 h */
    TEST_ASSERT_EQUAL_UINT8(5u, r[3]);
    TEST_ASSERT_EQUAL_UINT8(42u, r[4]);
    TEST_ASSERT_TRUE(keypad.got_sec[keypad.got_n - 1u] != ZW_SEC_NONE); /* as secure as it asked */

    W.local.hour = 15u; /* read when asked: never a copy from before */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, DATE_GET, 2u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now + 1u, ZW_SEC_AUTO, TIME_GET,
                                                     2u));
    run_for(3000u);
    r = got_cmd(&keypad, 0x8A, 0x04, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "a Date Get went unanswered");
    TEST_ASSERT_EQUAL_UINT(6u, n);
    TEST_ASSERT_EQUAL_UINT16(2026u, (uint16_t)((unsigned)r[2] << 8 | (unsigned)r[3]));
    TEST_ASSERT_EQUAL_UINT8(9u, r[4]);
    TEST_ASSERT_EQUAL_UINT8(27u, r[5]);
    r = got_cmd(&keypad, 0x8A, 0x02, &n);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX8(15u, r[2]);
}

/* ZW-024: "Blink its light" - a device with Indicator v3 is told to blink its node identify
 * indicator 3 times, 0.8 s apart, 0.6 s on (its own light: the panel's screen never blinks), at
 * its secure class; the page hears whether it answered. A device without it, or one that sleeps,
 * isn't offered it. */
static void test_identify(void)
{
    static fake_zdev_t keypad, plain;
    device(&keypad, 0x1DE7u, false);
    fake_zdev_listening(&keypad);
    keypad.m.listening = false;
    keypad.m.flirs = true;
    keypad.m.secure_ccs[keypad.m.secure_n++] = ZW_CC_INDICATOR;
    keypad.m.versions[keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_INDICATOR, 3};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(20000u);
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, zwz_panel_generation(&W.p, node), &page));
    TEST_ASSERT_TRUE(page.identify);

    keypad.got_n = 0u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_IDENTIFY, node, false)); /* anyone may */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask(UI_ZW_REQ_IDENTIFY, node, false));
    run_for(3000u);
    size_t n = 0u;
    const uint8_t *r = got_cmd(&keypad, 0x87, 0x01, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "the device was not told to blink");
    const uint8_t want[] = {0x87, 0x01, 0x00, 0x03, 0x50, 0x03, 0x08, 0x50, 0x04, 0x03, 0x50, 0x05, 0x06};
    TEST_ASSERT_EQUAL_UINT(sizeof want, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, r, sizeof want);
    TEST_ASSERT_TRUE(keypad.got_sec[keypad.got_n - 1u] != ZW_SEC_NONE);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_IDENTIFY, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ANSWERED, progress().result);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* gone quiet: it doesn't answer, and the page says so */
    keypad.silent = true;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_IDENTIFY, node, false));
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_NO_ANSWER, progress().result);
    keypad.silent = false;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* a sensor without Indicator: not offered, refused */
    device(&plain, 0x1DE8u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&plain, 0u));
    const uint16_t other = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, other, zwz_panel_generation(&W.p, other), &page));
    TEST_ASSERT_FALSE(page.identify);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_UNSUPPORTED, ask(UI_ZW_REQ_IDENTIFY, other, false));
}

/* ZW-022: a device reset with its own button leaves the network: logged with its zone. */
static void test_device_reset_locally(void)
{
    static fake_zdev_t door;
    device(&door, 0xD00Fu, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    bind(4u, node, ZSRC_ZW_SLOT_CONTACT);
    W.log_n = 0u;
    fake_zdev_reset_locally(&door, W.wire.now);
    run_for(MIN_MS);
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_LEFT);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT16(4u, l->zone);
    TEST_ASSERT_EQUAL_UINT8(node, l->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_LEFT_RESET_LOCALLY, l->arg2);
    TEST_ASSERT_EQUAL_UINT(1u, logged_n(ALARM_EV_ZWAVE_LEFT));
    TEST_ASSERT_NULL(logged(ALARM_EV_ZWAVE_REMOVED)); /* its removal as failed follows, quietly */
}

/* ZW-011, network.md §7: new devices' check-in, and one device's own - kept, sent at its next
 * wake-up, "changes when it next wakes up" on its page until it has it; listening devices have
 * none. */
static void test_check_in(void)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_CHECK_IN;
    r.seconds = 4u * 3600u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, zwz_panel_request(&W.p, W.wire.now, &r, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    run_for(0u);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, W.setup.cfg.check_in_s);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, st.check_in_s);
    r.seconds = 60u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, zwz_panel_request(&W.p, W.wire.now, &r, true));

    static fake_zdev_t door;
    device(&door, 0xD010u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, page.check_in_s); /* new devices': 4 h */
    run_for(MIN_MS); /* asleep */
    TEST_ASSERT_FALSE(fake_zdev_awake(&door, W.wire.now));
    r.node = node;
    r.generation = gen;
    r.seconds = 12u * 3600u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.checkin_n);
    TEST_ASSERT_EQUAL_UINT16(node, W.checkins[0].node);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, W.checkins[0].seconds);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, page.check_in_s);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, page.check_in_next_s);
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, door.m.wake_interval);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, page.check_in_s);
    TEST_ASSERT_EQUAL_UINT32(0u, page.check_in_next_s);

    /* kept across a restart of the panel: its re-interview keeps it */
    boot();
    TEST_ASSERT_EQUAL_UINT(1u, W.p.checkin_count);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, W.p.checkins[0].seconds);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, W.p.net.cfg.check_in_s);
    /* a device that listens has no check-in to set */
    r.node = 77u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, zwz_panel_request(&W.p, W.wire.now, &r, true));
}

/* A device's check-in on its way in the network's memory (zw_net), or NULL. */
static const zw_net_checkin_t *on_its_way(uint16_t node)
{
    for (size_t i = 0; i < ZW_NET_CHECK_INS; i++) {
        if (W.p.net.checkins[i].node == node) {
            return &W.p.net.checkins[i];
        }
    }
    return NULL;
}

static ui_zw_req_result_t ask_check_in(uint16_t node, uint16_t generation, uint32_t seconds,
                                       bool allowed)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_CHECK_IN;
    r.user = 1u;
    r.node = node;
    r.generation = generation;
    r.seconds = seconds;
    return zwz_panel_request(&W.p, W.wire.now, &r, allowed);
}

/* ZW-011, network.md §7: a device that answers its check-in's Set "fail" keeps the interval it
 * had - the choice is dropped (and stored so), its page shows the interval it has with no change
 * to come, nothing is logged, its zones keep their grace. The refusals as the pages hear them: no
 * radio while it restarts, another device's generation, more than 24 h, not allowed. */
static void test_check_in_not_taken(void)
{
    static fake_zdev_t door;
    device(&door, 0xD013u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    run_for(MIN_MS); /* asleep */
    door.m.supervision_fail = true; /* its Supervision Reports say "fail" from now on */
    const size_t logs = W.log_n;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_check_in(node, gen, 12u * 3600u, true));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.checkin_n);
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, page.check_in_next_s);
    const uint32_t supervised = door.supervised;
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT32(supervised + 1u, door.supervised); /* the Set, answered "fail" */
    TEST_ASSERT_NULL(on_its_way(node));
    TEST_ASSERT_EQUAL_UINT(0u, W.checkin_n);
    TEST_ASSERT_EQUAL_UINT(0u, W.p.checkin_count);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(ZW_NET_CHECK_IN_S, page.check_in_s);
    TEST_ASSERT_EQUAL_UINT32(0u, page.check_in_next_s);
    TEST_ASSERT_EQUAL_UINT(logs, W.log_n);
    TEST_ASSERT_EQUAL_UINT32(ZW_NET_CHECK_IN_S, zsrc_zw_find(&W.src, node)->check_in_s);

    /* refused: nothing stored, nothing on its way */
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_RESTART;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NO_RADIO, ask_check_in(node, gen, 4u * 3600u, true));
    run_for(8000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID,
                          ask_check_in(node, (uint16_t)((unsigned)gen + 1u), 4u * 3600u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_check_in(node, gen, 25u * 3600u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask_check_in(node, gen, 4u * 3600u, false));
    run_for(1000u);
    TEST_ASSERT_NULL(on_its_way(node));
    TEST_ASSERT_EQUAL_UINT(0u, W.checkin_n);
}

/* ZW-011, network.md §7: a check-in chosen on a device's page but not confirmed when the panel
 * restarts lived in the network's memory only - at the first check after the restart it is set
 * again from what was stored, the page still says it is to come, and the device takes it at its
 * next wake-up (its zones' grace with it). Once it has it, a restart sets nothing. */
static void test_check_in_after_restart(void)
{
    static fake_zdev_t door;
    device(&door, 0xD014u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    run_for(MIN_MS); /* asleep */
    door.m.auto_wake = false; /* it wakes when the test says */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_check_in(node, gen, 12u * 3600u, true));
    run_for(1000u);
    TEST_ASSERT_NOT_NULL(on_its_way(node));
    TEST_ASSERT_EQUAL_UINT(1u, W.checkin_n);
    const unsigned sets = fake_zdev_asked(&door, 0x84, 0x04);

    boot(); /* before it wakes: what zw_net carried is gone */
    const zw_net_checkin_t *c = on_its_way(node);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, c->seconds);
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(ZW_NET_CHECK_IN_S, page.check_in_s);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, page.check_in_next_s);
    TEST_ASSERT_EQUAL_UINT32(ZW_NET_CHECK_IN_S, zsrc_zw_find(&W.src, node)->check_in_s);
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&door, 0x84, 0x04)); /* one Set */
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, door.m.wake_interval);
    TEST_ASSERT_NULL(on_its_way(node));
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, page.check_in_s);
    TEST_ASSERT_EQUAL_UINT32(0u, page.check_in_next_s);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, zsrc_zw_find(&W.src, node)->check_in_s);
    TEST_ASSERT_EQUAL_UINT(1u, W.checkin_n); /* kept for its later interviews */

    boot(); /* it has it: nothing to set again */
    TEST_ASSERT_NULL(on_its_way(node));
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, zsrc_zw_find(&W.src, node)->check_in_s);
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&door, 0x84, 0x04));
}

/* ZW-018: the region (one the radio offers; stored, the radio restarts), "Restart the radio" and
 * the network reset (its devices go quietly: one "network reset" in the log, for who asked; new
 * keys) - each followed by the pages until the radio works again. A refused request leaves the
 * page's operation as it was. */
static void test_region_restart_and_reset(void)
{
    static fake_zdev_t door;
    device(&door, 0xD011u, false);
    fake_zdev_listening(&door);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_REGION;
    r.region = 0x03u; /* Hong Kong: not this radio's */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_UNSUPPORTED, zwz_panel_request(&W.p, W.wire.now, &r, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_ADD, progress().op); /* what the page had */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, progress().result);
    r.region = 0x09u;
    /* ZW-045: the module offers a region on another band too (Europe), read at the start-up after
     * the change */
    W.zac.id.regions[W.zac.id.region_count++] = 0x00u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, zwz_panel_request(&W.p, W.wire.now, &r, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_REGION, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_WORKING, progress().step);
    run_for(8000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_DONE, progress().result);
    TEST_ASSERT_TRUE(W.setup.cfg.region_set);
    TEST_ASSERT_EQUAL_UINT8(0x09u, W.setup.cfg.region);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(0x09u, st.region);
    /* ZW-045: back to United States from its Long Range region keeps the one classic device;
     * Europe, another band, stops it; the page reads it region by region */
    TEST_ASSERT_EQUAL_UINT16(1u, st.devices);
    TEST_ASSERT_EQUAL_UINT16(0u, st.devices_lr);
    TEST_ASSERT_EQUAL_UINT8(3u, st.region_count);
    bool saw_eu = false;
    for (size_t i = 0; i < (size_t)st.region_count; i++) {
        if (st.regions[i] == 0x00u) {
            saw_eu = true;
            TEST_ASSERT_EQUAL_UINT8(UI_ZW_REGION_BAND_CHANGE, st.region_change[i]);
            TEST_ASSERT_EQUAL_UINT16(st.devices, st.region_stop[i]);
            continue;
        }
        TEST_ASSERT_TRUE(st.regions[i] == 0x01u || st.regions[i] == 0x09u);
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_REGION_SAME_BAND, st.region_change[i]);
        TEST_ASSERT_EQUAL_UINT16(0u, st.region_stop[i]);
    }
    TEST_ASSERT_TRUE(saw_eu);

    const uint32_t resets = W.zac.by_func[ZW_FUNC_SOFT_RESET];
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_RESTART;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    TEST_ASSERT_FALSE(W.src.link_up); /* until the network is checked again */
    run_for(8000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_RESTART, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_DONE, progress().result);
    TEST_ASSERT_EQUAL_UINT32(resets + 1u, W.zac.by_func[ZW_FUNC_SOFT_RESET]);
    TEST_ASSERT_TRUE(W.src.link_up);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT16(1u, st.devices);

    const zwz_keys_t keys = W.setup.keys;
    W.log_n = 0u;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_RESET;
    r.user = 2u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, zwz_panel_request(&W.p, W.wire.now, &r, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    run_for(8000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_RESET, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_DONE, progress().result);
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_NETWORK_RESET);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(2u, l->user);
    TEST_ASSERT_NULL(logged(ALARM_EV_ZWAVE_LEFT));
    TEST_ASSERT_EQUAL_UINT(1u, W.log_n);
    TEST_ASSERT_FALSE(memcmp(&keys.keys, &W.setup.keys.keys, sizeof keys.keys) == 0);
    TEST_ASSERT_EQUAL_HEX32(W.zac.id.home_id, W.setup.keys.home_id);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_EQUAL_UINT16(0u, st.devices);
}

/* ZW-017, ZW-022: after a panel restart the devices, keys and inclusion counter come back from
 * what was stored; a module that restarts by itself (a watchdog) is logged, a reset through its
 * line is not; a pulled module shows "Not answering". */
static void test_restarts_and_the_radio(void)
{
    static fake_zdev_t door;
    device(&door, 0xD012u, false);
    fake_zdev_listening(&door);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    const zwz_keys_t keys = W.setup.keys;
    boot();
    TEST_ASSERT_EQUAL_UINT16(gen, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_TRUE(memcmp(&keys, &W.setup.keys, sizeof keys) == 0);
    TEST_ASSERT_EQUAL_UINT(1u, W.key_writes); /* the first start's alone */
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT16(1u, st.devices);

    W.log_n = 0u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x03u)); /* its watchdog */
    run_for(10000u);
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_RADIO_RESTARTED);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(0x03u, l->arg);
    TEST_ASSERT_EQUAL_UINT8(1u, l->arg2); /* ZW-041: the one restart it stands for */
    W.log_n = 0u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x00u)); /* its reset line */
    run_for(10000u);
    zwz_panel_expect_restart(&W.p, W.wire.now, 5000u);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x09u)); /* expected */
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(0u, W.log_n);

    W.zac.fault.dead = true;
    run_for(40u * MIN_MS);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_NOT_ANSWERING, st.radio);
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_ADD;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NO_RADIO, zwz_panel_request(&W.p, W.wire.now, &r, true));
}

/* ZW-005: the module restarting while the panel listens ends the add or the remove with its own
 * result, not "failed" or a window that runs out. */
static void test_radio_restart_ends_the_windows(void)
{
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ADD, 0u, true));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_LISTENING, progress().step);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x03u));
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_RADIO_RESTARTED, progress().result);

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE, 0u, true));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_LISTENING, progress().step);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x03u));
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_RADIO_RESTARTED, progress().result);
}

/* The stored formats: round trips, a later (longer) value read as far as known, damage refused. */
static void test_stored_formats(void)
{
    uint8_t buf[ZWZ_CHECKINS_BLOB + 8u];
    const zwz_cfg_t cfg = {true, 0x09u, 14400u, true, 0, false, 0, false, 0};
    TEST_ASSERT_EQUAL_UINT(ZWZ_CFG_BLOB, zwz_cfg_encode(&cfg, buf, sizeof buf));
    static const uint8_t CFG[] = {0x01, 0x03, 0x09, 0x00, 0x40, 0x38, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CFG, buf, sizeof CFG);
    {
        /* ZW-024: the jamming threshold in the byte older firmware writes 00 (the default) */
        const zwz_cfg_t jam = {false, 0u, 0u, false, -70, false, 0, false, 0};
        uint8_t j[ZWZ_CFG_BLOB];
        TEST_ASSERT_EQUAL_UINT(ZWZ_CFG_BLOB, zwz_cfg_encode(&jam, j, sizeof j));
        TEST_ASSERT_EQUAL_HEX8(0xBA, j[3]);
        zwz_cfg_t back;
        TEST_ASSERT_TRUE(zwz_cfg_decode(&back, j, sizeof j));
        TEST_ASSERT_EQUAL_INT8(-70, back.jam_dbm);
        j[3] = 0x7Fu; /* nonsense: the default */
        TEST_ASSERT_TRUE(zwz_cfg_decode(&back, j, sizeof j));
        TEST_ASSERT_EQUAL_INT8(0, back.jam_dbm);
    }
    zwz_cfg_t back;
    buf[8] = 0xAAu; /* a later format's field */
    TEST_ASSERT_TRUE(zwz_cfg_decode(&back, buf, 9u));
    TEST_ASSERT_TRUE(back.region_set && back.s0_allowed);
    TEST_ASSERT_EQUAL_UINT8(0x09u, back.region);
    TEST_ASSERT_EQUAL_UINT32(14400u, back.check_in_s);
    TEST_ASSERT_FALSE(back.power_cap_set || back.lr_cap_set); /* a blob before the caps */
    TEST_ASSERT_FALSE(zwz_cfg_decode(&back, buf, 7u));
    buf[0] = 0u;
    TEST_ASSERT_FALSE(zwz_cfg_decode(&back, buf, 8u));
    TEST_ASSERT_EQUAL_UINT(0u, zwz_cfg_encode(&cfg, buf, 7u));
    {
        /* ZW-025: the power caps after the older fields (tenths of a dBm, little-endian) */
        zwz_cfg_t pw;
        memset(&pw, 0, sizeof pw);
        pw.power_cap_set = true;
        pw.power_cap_dbm10 = -15;
        pw.lr_cap_set = true;
        pw.lr_cap_dbm10 = 200;
        uint8_t b[ZWZ_CFG_BLOB];
        TEST_ASSERT_EQUAL_UINT(ZWZ_CFG_BLOB, zwz_cfg_encode(&pw, b, sizeof b));
        static const uint8_t TAIL[] = {0x03, 0xF1, 0xFF, 0xC8, 0x00};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(TAIL, &b[8], sizeof TAIL);
        TEST_ASSERT_TRUE(zwz_cfg_decode(&back, b, sizeof b));
        TEST_ASSERT_TRUE(back.power_cap_set && back.lr_cap_set);
        TEST_ASSERT_EQUAL_INT16(-15, back.power_cap_dbm10);
        TEST_ASSERT_EQUAL_INT16(200, back.lr_cap_dbm10);
    }

    zwz_keys_t k;
    memset(&k, 0, sizeof k);
    k.bound = true;
    k.home_id = 0xC0FFEE01u;
    for (unsigned i = 0; i < 16u; i++) {
        k.keys.s2_unauth[i] = (uint8_t)i;
        k.keys.lr_access[i] = (uint8_t)(0xF0u + i);
    }
    TEST_ASSERT_EQUAL_UINT(ZWZ_KEYS_BLOB, zwz_keys_encode(&k, buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[4]); /* home ID, little-endian */
    TEST_ASSERT_EQUAL_HEX8(0xC0u, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, buf[ZWZ_KEYS_BLOB - 1u]);
    zwz_keys_t kb;
    TEST_ASSERT_TRUE(zwz_keys_decode(&kb, buf, ZWZ_KEYS_BLOB));
    TEST_ASSERT_TRUE(memcmp(&k, &kb, sizeof k) == 0);
    TEST_ASSERT_FALSE(zwz_keys_decode(&kb, buf, ZWZ_KEYS_BLOB - 1u));
    TEST_ASSERT_FALSE(kb.bound); /* refused: nothing kept */

    zwz_checkin_t c[3] = {{7u, 2u, 14400u}, {9u, 5u, 3600u}, {0u, 0u, 0u}};
    const size_t n = zwz_checkins_encode(c, 2u, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT(18u, n);
    zwz_checkin_t cb[2];
    size_t got = 0u;
    TEST_ASSERT_TRUE(zwz_checkins_decode(cb, 2u, &got, buf, n));
    TEST_ASSERT_EQUAL_UINT(2u, got);
    TEST_ASSERT_EQUAL_UINT16(9u, cb[1].node);
    TEST_ASSERT_EQUAL_UINT16(5u, cb[1].generation);
    TEST_ASSERT_EQUAL_UINT32(3600u, cb[1].seconds);
    TEST_ASSERT_TRUE(zwz_checkins_decode(cb, 1u, &got, buf, n)); /* room for one: one */
    TEST_ASSERT_EQUAL_UINT(1u, got);
    TEST_ASSERT_FALSE(zwz_checkins_decode(cb, 2u, &got, buf, n - 1u)); /* cut short */
    TEST_ASSERT_EQUAL_UINT(0u, got);
    TEST_ASSERT_EQUAL_UINT(0u, zwz_checkins_encode(c, 2u, buf, 17u));
}

/* ZW-028: real inclusion/interview/S2/Serial API and completion dispatch to a simulated
 * FLiRS output device; feedback commands reach its decrypted receive buffer. */
static void test_feedback_over_secure_radio(void)
{
    static fake_zdev_t keypad;
    device(&keypad, 0xFEEDu, false);
    fake_zdev_listening(&keypad);
    keypad.m.listening = false; keypad.m.flirs = true;
    keypad.m.manufacturer = 0x0346; keypad.m.product_type = 0x0101; keypad.m.product_id = 0x0301;
    keypad.m.secure_ccs[keypad.m.secure_n++] = ZW_CC_INDICATOR;
    keypad.m.versions[keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_INDICATOR, 3};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, ZW_S2_KEY_ACCESS));
    zwf_init(&W.feedback);
    TEST_ASSERT_TRUE(zwf_assign(&W.feedback, 0, &W.p.net, progress().node, 1));
    zwf_view_t view = {.partitions = 1, .taken_ms = W.wire.now, .part = {{2, 0}}};
    zwf_snapshot(&W.feedback, &view);
    keypad.got_n = 0;
    W.feedback_enabled = true;
    run_for(2000u);
    TEST_ASSERT_FALSE(W.feedback.target[0].inflight);
    TEST_ASSERT_TRUE(W.feedback.target[0].synced);
    TEST_ASSERT_EQUAL_UINT(1, keypad.got_n);
    const uint8_t expected[] = {0x87, 1, 0, 1, 2, 1, 99};
    TEST_ASSERT_EQUAL_MEMORY(expected, keypad.got[0], sizeof(expected));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, keypad.got_sec[0]);
    run_for(2000u); TEST_ASSERT_EQUAL_UINT(1, keypad.got_n);
    view.taken_ms = W.wire.now; view.part[0].indicator = 11;
    zwf_snapshot(&W.feedback, &view);
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(2, keypad.got_n);
    TEST_ASSERT_EQUAL_UINT8(11, keypad.got[1][4]);
    /* Removed/reused identity must never receive the previous assignment's commands. */
    zw_node_record_t *n = zw_node_find(&W.p.net.nodes, progress().node);
    TEST_ASSERT_NOT_NULL(n);
    n->generation++;
    view.part[0].indicator = 2; view.taken_ms = W.wire.now;
    zwf_snapshot(&W.feedback, &view); run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(2, keypad.got_n);
}

/* ZW-006, ZW-029: a device whose node information came empty is not taken for one without
 * Security 2 - KEX Get goes anyway, and a keypad that has it joins securely (it had joined
 * unencrypted before the fix). It answers when called but runs on batteries: its page says so,
 * never "Mains powered". */
static void test_empty_node_information_still_secured(void)
{
    static fake_zdev_t keypad;
    flirs_device(&keypad, 0x4B45u);
    keypad.nif_len = 0u;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, ZW_S2_KEY_ACCESS));
    const ui_zw_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_S2_ACCESS, g.security);
    const ui_zw_device_t *d = row_of(g.node);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_S2_ACCESS, d->security);
    TEST_ASSERT_TRUE(d->listening);
    TEST_ASSERT_TRUE(d->flirs);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_POWER_BATTERY, d->power);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_READY, d->state);
}

/* ZW-029 (the partial keypad seen on a real panel): a device that joins with an empty node
 * information and then hears nothing (reset, or out of range). Its key exchange is not
 * confirmed (never "Not encrypted"); its interview gives up without "asleep" (it is FLiRS); the
 * device is SETUP_STOPPED, "Stop and finish later" says SETUP_PAUSED (never "Added"), "Try its
 * setup again" runs it again, and the removal as failed takes it out of the radio's own list too
 * (no ghost). */
static void test_partial_join_setup_stops_and_is_cleaned_up(void)
{
    static fake_zdev_t stray;
    flirs_device(&stray, 0x5747u);
    stray.nif_len = 0u;
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &stray));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ADD, 0u, true));
    run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&W.zac, W.wire.now, &stray));
    stray.silent = true; /* it joined, and hears nothing more */
    run_for(15000u);     /* TA1: its KEX Get unanswered */
    ui_zw_progress_t g = progress();
    const uint16_t node = g.node;
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_READING, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_S2_NO_INFO, g.s2_failure);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_UNCONFIRMED, g.security);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_UNCONFIRMED, row_of(node)->security);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_POWER_UNKNOWN, row_of(node)->power); /* nothing read yet */
    for (unsigned i = 0; i < 3600u && !progress().stalled; i++) {
        run_for(1000u);
    }
    g = progress();
    TEST_ASSERT_TRUE(g.stalled);
    TEST_ASSERT_FALSE(g.asleep); /* FLiRS: it never sleeps; it didn't answer */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_READING, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_SETUP_STOPPED, row_of(node)->state);
    TEST_ASSERT_TRUE(row_of(node)->setup_done < row_of(node)->setup_total);

    /* "Stop and finish later": its setup is not finished - never "Added" */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_SETUP_PAUSED, progress().result);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* "Try its setup again": installer only; it runs (SETTING_UP), then stops again */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_SETUP, node, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask(UI_ZW_REQ_SETUP, 99u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_SETTING_UP, row_of(node)->state);
    TEST_ASSERT_TRUE(wait_state(node, UI_ZW_DEV_SETUP_STOPPED, 3600000u));

    /* the page offers the removal as failed without waiting for it to be unreachable */
    TEST_ASSERT_FALSE(row_of(node)->unreachable);
    W.log_n = 0u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_REMOVE_FAILED, node, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, node, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_REMOVE_FAILED, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_WORKING, progress().step);
    run_for(20000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, g.result);
    TEST_ASSERT_NULL(row_of(node));
    TEST_ASSERT_FALSE(zw_node_list_test(W.zac.id.node_list, node)); /* no ghost in the radio */
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_REMOVED);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_REMOVED_FAILED, l->arg2);
}

/* ZW-016, ZW-029: every way a removal as failed ends, each in its own words - and nothing leaves
 * the panel's list unless it left the radio's: one that answers is kept (STILL_ANSWERS); pings
 * unanswered but the radio doesn't count it failed (REFUSED); the radio silent on
 * RemoveFailedNode (TIMED_OUT); then removed. */
static void test_remove_failed_outcomes(void)
{
    static fake_zdev_t keypad;
    flirs_device(&keypad, 0x4B46u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, ZW_S2_KEY_ACCESS));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, node, true));
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_STILL_ANSWERS, progress().result);
    TEST_ASSERT_NOT_NULL(row_of(node));
    TEST_ASSERT_TRUE(zw_node_list_test(W.zac.id.node_list, node));

    keypad.silent = true; /* factory reset: it hears nothing */
    W.zac.failed_off = true;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, node, true));
    run_for(20000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REFUSED, progress().result);
    TEST_ASSERT_NOT_NULL(row_of(node));
    TEST_ASSERT_TRUE(zw_node_list_test(W.zac.id.node_list, node));

    W.zac.failed_off = false;
    W.zac.fault.mute_func = (uint8_t)ZW_FUNC_REMOVE_FAILED_NODE;
    W.zac.fault.mute_count = 1u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, node, true));
    run_for(60000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_TIMED_OUT, progress().result);
    TEST_ASSERT_NOT_NULL(row_of(node)); /* not known to be gone: kept */
    TEST_ASSERT_TRUE(zw_node_list_test(W.zac.id.node_list, node));

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, node, true));
    run_for(20000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, progress().result);
    TEST_ASSERT_NULL(row_of(node));
    TEST_ASSERT_FALSE(zw_node_list_test(W.zac.id.node_list, node));
}

/* ZW-006: the PIN is the DSK's first group, 16 bits written as 5 digits: 65536 and 99999 are
 * refused before any conversion (the question stays open), 65535 and 00000 are taken. */
static void test_pin_range(void)
{
    static fake_zdev_t a, b;
    device(&a, 0x9101u, true);
    add_to_pin(&a);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_pin(65536u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_pin(99999u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_pin(100000u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_ASK_PIN, progress().step);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_pin(65535u));
    TEST_ASSERT_EQUAL_UINT8(a.pin == 65535u ? UI_ZW_RES_ADDED : UI_ZW_RES_ADDED_INSECURE,
                            add_end());
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    device(&b, 0x9102u, true);
    add_to_pin(&b);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_pin(0u)); /* "00000" */
    TEST_ASSERT_EQUAL_UINT8(b.pin == 0u ? UI_ZW_RES_ADDED : UI_ZW_RES_ADDED_INSECURE, add_end());
    if (b.pin != 0u) {
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_S2_WRONG_PIN, progress().s2_failure);
    }
}

/* ZW-028: the device page's "Sounds and lights": a keypad with the profile and S2 Access is
 * available, off until assigned, then on its partition and sent; one granted only S2
 * Unauthenticated needs pairing again; a sensor has none. */
static void test_feedback_on_the_device_page(void)
{
    zwf_init(&W.feedback);
    zwz_panel_set_feedback(&W.p, &W.feedback);
    static fake_zdev_t keypad, weak, door;
    flirs_device(&keypad, 0xFEE1u);
    keypad.m.manufacturer = 0x0346; keypad.m.product_type = 0x0101; keypad.m.product_id = 0x0301;
    keypad.m.secure_ccs[keypad.m.secure_n++] = ZW_CC_INDICATOR;
    keypad.m.versions[keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_INDICATOR, 3};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, ZW_S2_KEY_ACCESS));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FB_AVAILABLE, page.feedback.fit);
    TEST_ASSERT_EQUAL_UINT8(0u, page.feedback.partition);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_UNSUPPORTED, ask(UI_ZW_REQ_FEEDBACK, node, true)); /* owner's */
    const size_t slot = zwf_slot_for(&W.feedback, node);
    TEST_ASSERT_TRUE(zwf_assign(&W.feedback, slot, &W.p.net, node, 1u));
    zwf_view_t view = {.partitions = 1, .taken_ms = W.wire.now, .part = {{2, 0}}};
    zwf_snapshot(&W.feedback, &view);
    W.feedback_enabled = true;
    run_for(2000u);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT8(1u, page.feedback.partition);
    TEST_ASSERT_TRUE(page.feedback.live);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_POWER_BATTERY, page.dev.power);

    flirs_device(&weak, 0xFEE2u);
    weak.m.manufacturer = 0x0346; weak.m.product_type = 0x0101; weak.m.product_id = 0x0301;
    weak.m.secure_ccs[weak.m.secure_n++] = ZW_CC_INDICATOR;
    weak.m.versions[weak.m.versions_n++] = (fake_cc_version_t){ZW_CC_INDICATOR, 3};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&weak, ZW_S2_KEY_UNAUTH));
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, progress().node, progress().generation,
                                      &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FB_NOT_SECURE, page.feedback.fit);

    device(&door, 0xFEE3u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, progress().node, progress().generation,
                                      &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FB_NONE, page.feedback.fit);
}

/* ZW-031: an assigned keypad's Entry Control, sent over S2 by a simulated keypad, reaches the
 * owner's claim (copied, not run) and, run later, the engine's port with the typed code and its
 * partition; an unassigned keypad's is not claimed; the device page shows the assignment. */
static void test_keypad_control_end_to_end(void)
{
    static const zwk_port_t kport = {.command = engine_port};
    zwkc_init(&W.keyctl, &kport);
    zwz_panel_set_keyctl(&W.p, &W.keyctl);
    W.claim_on = true;
    static fake_zdev_t keypad, other;
    flirs_device(&keypad, 0xC0DEu);
    keypad.m.secure_ccs[keypad.m.secure_n++] = ZW_CC_ENTRY_CONTROL;
    keypad.m.versions[keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_ENTRY_CONTROL, 1};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, ZW_S2_KEY_ACCESS));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FB_AVAILABLE, page.control.fit);
    TEST_ASSERT_EQUAL_UINT8(0u, page.control.partition);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_UNSUPPORTED, ask(UI_ZW_REQ_CONTROL, node, true)); /* owner's */
    flirs_device(&other, 0xC0DFu);
    other.m.secure_ccs[other.m.secure_n++] = ZW_CC_ENTRY_CONTROL;
    other.m.versions[other.m.versions_n++] = (fake_cc_version_t){ZW_CC_ENTRY_CONTROL, 1};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&other, ZW_S2_KEY_ACCESS));

    const zw_cc_entry_notification_t n = {1u, ZW_ENTRY_DATA_ASCII, ZW_ENTRY_EVENT_ARM_AWAY,
                                          (const uint8_t *)"1234", 4u};
    uint8_t cmd[40];
    const size_t len = zw_cc_entry_notification_build(cmd, sizeof cmd, &n);
    /* not assigned yet: nobody claims it */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, cmd, len));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(0u, W.claims);

    TEST_ASSERT_TRUE(zwkc_assign(&W.keyctl, 0, &W.p.net, node, 1, 0));
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT8(1u, page.control.partition);
    TEST_ASSERT_TRUE(page.control.live);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, cmd, len));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.claims);
    TEST_ASSERT_EQUAL_UINT(0u, W.engine_calls); /* claimed, not run under the lock */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, W.claimed.security);
    zwk_reply_t reply;
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, zwkc_run(&W.keyctl, 0, W.p.net.keys_home_id, gen,
                                                &W.claimed, W.wire.now, &reply));
    TEST_ASSERT_EQUAL_UINT(1u, W.engine_calls);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, W.engine_cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, W.engine_cmd.level);
    TEST_ASSERT_EQUAL_UINT8(1u, W.engine_cmd.partition);
    TEST_ASSERT_EQUAL_STRING("1234", W.engine_cmd.code);
    /* the other keypad (not assigned) is not claimed */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&other, W.wire.now, ZW_SEC_AUTO, cmd, len));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.claims);
}

/* A secure keypad, included; with `partition` its control slot `slot` is assigned (ZW-031). */
static uint16_t supervised_keypad(fake_zdev_t *keypad, uint64_t seed, size_t slot,
                                  uint8_t partition, uint8_t panic_mask)
{
    static const zwk_port_t kport = {.command = engine_port};
    zwkc_init(&W.keyctl, &kport);
    zwz_panel_set_keyctl(&W.p, &W.keyctl);
    flirs_device(keypad, seed);
    keypad->m.secure_ccs[keypad->m.secure_n++] = ZW_CC_ENTRY_CONTROL;
    keypad->m.versions[keypad->m.versions_n++] = (fake_cc_version_t){ZW_CC_ENTRY_CONTROL, 1};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(keypad, ZW_S2_KEY_ACCESS));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    if (partition != 0u) {
        TEST_ASSERT_TRUE(zwkc_assign(&W.keyctl, slot, &W.p.net, node, partition, panic_mask));
    }
    run_for(2000u);
    return node;
}

/* ZW-038, CORE-TRBL-008: a keypad allowed to arm and disarm that stops answering is said
 * unreachable once, as its slot's engine device (ZWKC_DEVICE); heard again, reachable once. */
static void test_keypad_supervised_unreachable(void)
{
    static fake_zdev_t keypad;
    const uint16_t node = supervised_keypad(&keypad, 0xC1DEu, 2u, 1u,
                                            (uint8_t)(1u << ALARM_PANIC_FIRE));
    TEST_ASSERT_EQUAL_UINT(0u, W.kp_calls); /* reachable from the start: nothing said */
    keypad.silent = true;
    run_for(45u * MIN_MS);
    TEST_ASSERT_TRUE(row_of(node)->unreachable);
    TEST_ASSERT_EQUAL_UINT(1u, W.kp_calls);
    TEST_ASSERT_EQUAL_UINT8(ZWKC_DEVICE(2u), W.kp_device);
    TEST_ASSERT_EQUAL_UINT8(11u, W.kp_device); /* 9-16: the Troubles list's "Keypad 11" */
    TEST_ASSERT_FALSE(W.kp_reachable);
    run_for(60u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(1u, W.kp_calls); /* said once */

    keypad.silent = false;
    const uint8_t battery[] = {ZW_CC_BATTERY, 0x03, 0x5A};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, battery,
                                                     sizeof battery));
    run_for(3000u);
    TEST_ASSERT_FALSE(row_of(node)->unreachable);
    TEST_ASSERT_EQUAL_UINT(2u, W.kp_calls);
    TEST_ASSERT_EQUAL_UINT8(ZWKC_DEVICE(2u), W.kp_device);
    TEST_ASSERT_TRUE(W.kp_reachable);
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT(2u, W.kp_calls);
}

/* ZW-038: a radio that stops answering and comes back while a supervised keypad is still
 * unreachable says nothing more - no false "heard again", no second trouble. */
static void test_keypad_supervision_survives_radio_restart(void)
{
    static fake_zdev_t keypad;
    const uint16_t node = supervised_keypad(&keypad, 0xC1E2u, 3u, 1u, 0u);
    keypad.silent = true;
    run_for(45u * MIN_MS);
    TEST_ASSERT_TRUE(row_of(node)->unreachable);
    TEST_ASSERT_EQUAL_UINT(1u, W.kp_calls);
    TEST_ASSERT_FALSE(W.kp_reachable);
    const uint32_t downs = zw_host_stats(&W.p.net.host)->link_downs;
    W.zac.fault.dead = true;
    run_for(3u * MIN_MS);
    TEST_ASSERT_TRUE(zw_host_stats(&W.p.net.host)->link_downs > downs);
    W.zac.fault.dead = false;
    run_for(10u * MIN_MS);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_EQUAL_UINT(1u, W.kp_calls);
    TEST_ASSERT_FALSE(W.kp_reachable);
    TEST_ASSERT_EQUAL_UINT8(ZWKC_DEVICE(3u), W.kp_device);
}

/* ZW-038: a keypad that may not arm, disarm or call for help (its control slot off) isn't
 * supervised: it going quiet says nothing (only its row on the Z-Wave page). */
static void test_keypad_unassigned_not_supervised(void)
{
    static fake_zdev_t keypad;
    const uint16_t node = supervised_keypad(&keypad, 0xC1DFu, 0u, 0u, 0u);
    keypad.silent = true;
    run_for(45u * MIN_MS);
    TEST_ASSERT_TRUE(row_of(node)->unreachable);
    TEST_ASSERT_EQUAL_UINT(0u, W.kp_calls);
}

/* ZW-038: a slot turned off while its keypad is unreachable restores the trouble it raised, and
 * nothing more is said of that keypad; a removed keypad's trouble is restored as well. */
static void test_keypad_supervision_restored_when_off_or_removed(void)
{
    static fake_zdev_t keypad, second;
    const uint16_t node = supervised_keypad(&keypad, 0xC1E0u, 0u, 1u, 0u);
    keypad.silent = true;
    run_for(45u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(1u, W.kp_calls);
    TEST_ASSERT_FALSE(W.kp_reachable);
    TEST_ASSERT_TRUE(zwkc_assign(&W.keyctl, 0u, &W.p.net, node, 0u, 0u)); /* control off */
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(2u, W.kp_calls);
    TEST_ASSERT_EQUAL_UINT8(ZWKC_DEVICE(0u), W.kp_device);
    TEST_ASSERT_TRUE(W.kp_reachable);
    run_for(60u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(2u, W.kp_calls);

    /* another keypad, in slot 1, unreachable and then removed as failed */
    flirs_device(&second, 0xC1E1u);
    second.m.secure_ccs[second.m.secure_n++] = ZW_CC_ENTRY_CONTROL;
    second.m.versions[second.m.versions_n++] = (fake_cc_version_t){ZW_CC_ENTRY_CONTROL, 1};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&second, ZW_S2_KEY_ACCESS));
    const uint16_t other = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_TRUE(zwkc_assign(&W.keyctl, 1u, &W.p.net, other, 1u, 0u));
    second.silent = true;
    run_for(45u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(3u, W.kp_calls);
    TEST_ASSERT_EQUAL_UINT8(ZWKC_DEVICE(1u), W.kp_device);
    TEST_ASSERT_FALSE(W.kp_reachable);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, other, true));
    for (unsigned i = 0; i < 120u && progress().step != UI_ZW_STEP_DONE; i++) {
        run_for(1000u);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, progress().result);
    TEST_ASSERT_EQUAL_UINT(4u, W.kp_calls);
    TEST_ASSERT_EQUAL_UINT8(ZWKC_DEVICE(1u), W.kp_device);
    TEST_ASSERT_TRUE(W.kp_reachable);
}

/* ZW-032, serial-api.md §7 (B015 UX-002): a module pulled out while nothing talks is found by
 * the idle probe - the page says the radio isn't answering and the hub raises its one radio
 * trouble - and it comes back by itself once plugged in again. A sensor in a zone makes the
 * hub's trouble count. */
static void test_pulled_radio_found_when_idle(void)
{
    static fake_zdev_t door;
    device(&door, 0xD0E1u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    bind(5u, progress().node, ZSRC_ZW_SLOT_CONTACT);
    run_for(60000u);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_FALSE(W.radio_trouble);
    const uint32_t downs = zw_host_stats(&W.p.net.host)->link_downs;
    W.zac.fault.dead = true;
    run_for(3u * MIN_MS);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_NOT_ANSWERING, st.radio);
    /* noticed with nothing else going on: by the idle probe, or - with jamming watched
     * (network.md §9) - by the background noise reading every 5 s, which leaves the probe nothing
     * to do; either way its link went down */
    TEST_ASSERT_TRUE(zw_host_stats(&W.p.net.host)->link_downs > downs);
    TEST_ASSERT_TRUE(W.radio_trouble);
    /* B015 UX-026: while it stays unplugged the headline stays too - the panel's own retries
     * never show as "starting" */
    for (unsigned i = 0; i < 60u; i++) {
        run_for(5000u);
        zwz_panel_status(&W.p, W.wire.now, &st);
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_NOT_ANSWERING, st.radio);
    }
    W.zac.fault.dead = false;
    run_for(10u * MIN_MS);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    /* ZSRC-015: the trouble stays until its sleeping sensor has spoken again (or shows CHECK) */
    TEST_ASSERT_TRUE(W.radio_trouble);
    fake_zdev_wake(&door, W.wire.now);
    run_for(5000u);
    TEST_ASSERT_FALSE(W.radio_trouble);
}

/* ZW-032, serial-api.md §7: the same pulled radio with jamming not watched (no background noise
 * reading every 5 s): the idle probe alone finds it, within the 3 minutes */
static void test_pulled_radio_found_by_idle_probe(void)
{
    W.setup.jam = false;
    boot();
    static fake_zdev_t door;
    device(&door, 0xD0E2u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    bind(5u, progress().node, ZSRC_ZW_SLOT_CONTACT);
    run_for(60000u);
    TEST_ASSERT_FALSE(W.radio_trouble);
    const uint32_t probes = zw_host_stats(&W.p.net.host)->probes;
    W.zac.fault.dead = true;
    run_for(3u * MIN_MS);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(zw_host_stats(&W.p.net.host)->probes > probes);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_NOT_ANSWERING, st.radio);
    TEST_ASSERT_TRUE(W.radio_trouble);
}

/* ZW-024 (network.md §9): every channel loud for 30 s - the pages say "Interference" and the zone
 * source's link goes down (one sensor_radio trouble, as for a radio that stops answering); quiet
 * again, it clears after 30 s and the trouble with the sensor's next word. Frames that never get
 * the air jam too. The threshold is stored. */
/* ZW-025 (network.md §12 power): the first read is the stored cap; a change above it is refused,
 * one below it takes (after the soft reset), back up to it is allowed, and the cap survives a
 * restart of the panel without a new read replacing it */
static void test_power_cap(void)
{
    static const uint8_t subs[] = {ZW_SETUP_GET_POWERLEVEL_16BIT, ZW_SETUP_SET_POWERLEVEL_16BIT,
                                   ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER,
                                   ZW_SETUP_SET_LONG_RANGE_MAXIMUM_TX_POWER};
    for (size_t i = 0; i < sizeof subs; i++) {
        W.zac.id.setup_supported[subs[i] / 8u] =
            (uint8_t)(W.zac.id.setup_supported[subs[i] / 8u] | 1u << (subs[i] % 8u));
    }
    boot();
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zwz_panel_set_power(&W.p, W.wire.now, true, false, -20));
    TEST_ASSERT_TRUE(zwz_panel_read_power(&W.p, W.wire.now));
    run_for(2000u);
    TEST_ASSERT_TRUE(W.setup.cfg.power_cap_set && W.setup.cfg.lr_cap_set);
    TEST_ASSERT_EQUAL_INT16(0, W.setup.cfg.power_cap_dbm10);
    TEST_ASSERT_EQUAL_INT16(140, W.setup.cfg.lr_cap_dbm10);
    TEST_ASSERT_EQUAL_INT(ZW_NET_TOO_HIGH, zwz_panel_set_power(&W.p, W.wire.now, true, false, 10));
    TEST_ASSERT_EQUAL_INT(ZW_NET_TOO_HIGH, zwz_panel_set_power(&W.p, W.wire.now, true, true, 200));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zwz_panel_set_power(&W.p, W.wire.now, true, false, -20));
    run_for(15000u);
    TEST_ASSERT_EQUAL_INT16(-20, W.zac.power_dbm10);
    TEST_ASSERT_EQUAL_INT16(-20, zw_net_power(&W.p.net)->dbm10); /* read again after it */
    TEST_ASSERT_EQUAL_INT16(0, W.setup.cfg.power_cap_dbm10);     /* the cap stays */
    boot();
    TEST_ASSERT_TRUE(W.setup.cfg.power_cap_set);
    TEST_ASSERT_TRUE(zwz_panel_read_power(&W.p, W.wire.now));
    run_for(2000u);
    TEST_ASSERT_EQUAL_INT16(0, W.setup.cfg.power_cap_dbm10);
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zwz_panel_set_power(&W.p, W.wire.now, true, false, 0));
    run_for(15000u);
    TEST_ASSERT_EQUAL_INT16(0, W.zac.power_dbm10);
}

static void gbl_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8 & 0xFFu);
    p[2] = (uint8_t)(v >> 16 & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

/* A small whole .gbl: header, 600 program bytes, the end tag with its CRC. */
static size_t radio_gbl(uint8_t *out)
{
    size_t at = 0u;
    gbl_u32(&out[at], ZW_GBL_TAG_HEADER);
    gbl_u32(&out[at + 4u], 8u);
    gbl_u32(&out[at + 8u], 0x03000000u);
    gbl_u32(&out[at + 12u], 0u);
    at += 16u;
    gbl_u32(&out[at], 0xF40A0AF4u);
    gbl_u32(&out[at + 4u], 604u);
    memset(&out[at + 8u], 0x5A, 604u);
    at += 612u;
    gbl_u32(&out[at], ZW_GBL_TAG_END);
    gbl_u32(&out[at + 4u], 4u);
    gbl_u32(&out[at + 8u], zw_crc32(0u, out, at + 8u));
    return at + 12u;
}

/* ZW-025 (network.md §14, CORE-ARM-013's owner): the radio's own firmware - arming held from its
 * start until the radio is back with its network, the log saying who started it and how it
 * ended, the devices still there; a failed one holds arming ZWZ_PANEL_OTW_HOLD_MS at most */
static void test_radio_update(void)
{
    static fake_zdev_t door;
    device(&door, 0x0DA7u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    run_for(30000u);
    static uint8_t image[700];
    const size_t n = radio_gbl(image);
    W.zac.next_firmware[0] = 2u;
    W.zac.next_firmware[1] = 30u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, zwz_panel_radio_update(&W.p, W.wire.now, image, n, false, 1u));
    image[100] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, zwz_panel_radio_update(&W.p, W.wire.now, image, n, true, 1u));
    image[100] ^= 0x01u;
    TEST_ASSERT_FALSE(zwz_panel_arming_held(&W.p, W.wire.now));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_radio_update(&W.p, W.wire.now, image, n, true, 1u));
    TEST_ASSERT_TRUE(zwz_panel_arming_held(&W.p, W.wire.now));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_RADIO_UPDATE, progress().op);
    run_for(50u); /* the log is taken after each run, as the service does */
    TEST_ASSERT_NOT_NULL(logged(ALARM_EV_ZWAVE_SOFTWARE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_RADIO_STARTED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    TEST_ASSERT_EQUAL_UINT8(1u, logged(ALARM_EV_ZWAVE_SOFTWARE)->user);
    run_for(20000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_UPDATED, progress().result);
    TEST_ASSERT_EQUAL_UINT16(progress().fw_total, progress().fw_done);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_RADIO_UPDATED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    run_for(20000u);
    TEST_ASSERT_FALSE(zwz_panel_arming_held(&W.p, W.wire.now)); /* back, with its network */
    TEST_ASSERT_EQUAL_UINT8(2u, zw_host_facts(&W.p.net.host)->firmware_major);
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(&W.p.net.nodes)); /* its device kept */

    /* the bootloader refuses it: failed, and arming held no longer than 15 minutes */
    W.zac.fault.otw_cancel_block = 2u;
    W.zac.fault.otw_cancel_error = 0x18u;
    W.zac.fault.otw_bad_image = false;
    W.zac.fault.otw_no_upload = false;
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_DISMISS;
    (void)zwz_panel_request(&W.p, W.wire.now, &r, true);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_radio_update(&W.p, W.wire.now, image, n, true, 1u));
    W.zac.fault.otw_cancel_times = 0u;
    W.zac.fault.bootloader_sticky = true; /* and the old firmware doesn't come back either */
    run_for(20000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_FAILED, progress().result);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_REFUSED, progress().fw_why);
    TEST_ASSERT_EQUAL_HEX8(0x18u, progress().fw_error);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_RADIO_FAILED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    TEST_ASSERT_TRUE(zwz_panel_arming_held(&W.p, W.wire.now));
    run_for(ZWZ_PANEL_OTW_HOLD_MS);
    TEST_ASSERT_FALSE(zwz_panel_arming_held(&W.p, W.wire.now));
}

static void test_jamming(void)
{
    static fake_zdev_t door;
    device(&door, 0x7A77u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    bind(6u, progress().node, ZSRC_ZW_SLOT_CONTACT);
    run_for(60000u);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_FALSE(st.jammed);
    TEST_ASSERT_TRUE(st.noise_known);
    TEST_ASSERT_EQUAL_INT8(-95, st.noise_dbm);
    TEST_ASSERT_TRUE(W.zac.bg_rssi_asks >= 10u); /* every 5 s */

    W.zac.bg_rssi[0] = -48;
    W.zac.bg_rssi[1] = -52;
    W.zac.bg_rssi[2] = -95; /* one quiet channel: ordinary traffic on the others */
    run_for(60000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_FALSE(st.jammed);
    W.zac.bg_rssi[2] = -50;
    run_for(20000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_FALSE(st.jammed); /* not 30 s yet */
    run_for(15000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(st.jammed);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio); /* the radio itself is fine */
    run_for(3u * MIN_MS);
    TEST_ASSERT_TRUE(W.radio_trouble);

    W.zac.bg_rssi[0] = W.zac.bg_rssi[1] = W.zac.bg_rssi[2] = -95;
    run_for(20000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(st.jammed); /* 30 s of quiet first */
    run_for(15000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_FALSE(st.jammed);
    fake_zdev_wake(&door, W.wire.now); /* ZSRC-015: its next word ends the trouble */
    run_for(5000u);
    TEST_ASSERT_FALSE(W.radio_trouble);

    /* frames that never get the air */
    static fake_zdev_t lamp;
    device(&lamp, 0x7A78u, false);
    fake_zdev_listening(&lamp); /* mains powered: "Check it now" reaches it at once */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&lamp, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(20000u);
    W.zac.air_blocked = true;
    for (int i = 0; i < 3; i++) {
        (void)ask(UI_ZW_REQ_PING, node, false);
        run_for(3000u);
        (void)ask(UI_ZW_REQ_DISMISS, 0u, true);
    }
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(st.jammed);
    W.zac.air_blocked = false;
    run_for(40000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_FALSE(st.jammed);

    /* its threshold, stored with the settings */
    TEST_ASSERT_FALSE(zwz_panel_set_jam(&W.p, W.wire.now, -10));
    TEST_ASSERT_TRUE(zwz_panel_set_jam(&W.p, W.wire.now, -70));
    run_for(100u);
    TEST_ASSERT_EQUAL_INT8(-70, W.setup.cfg.jam_dbm);
    W.zac.bg_rssi[0] = W.zac.bg_rssi[1] = W.zac.bg_rssi[2] = -65; /* loud for -70 */
    run_for(40000u);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(st.jammed);
}

/* The background noise on every channel, until the panel says jammed (or not). */
static void air(int8_t dbm, bool jammed)
{
    W.zac.bg_rssi[0] = W.zac.bg_rssi[1] = W.zac.bg_rssi[2] = dbm;
    ui_zw_status_t st;
    for (unsigned i = 0; i < 90u; i++) {
        run_for(1000u);
        zwz_panel_status(&W.p, W.wire.now, &st);
        if (st.jammed == jammed) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("the jam did not change");
}

/* ZW-024, ZW-013 (network.md §9): while the radio is jammed what still gets through counts - a
 * door that opens is told to the hub, a supervised report is answered at once (the device would
 * never send it again) - the sensor_radio trouble is raised at the jam's start (the detector's
 * 30 s, not the link's 90 s grace), and when the air clears a sleeper keeps its last state while
 * its state is asked afresh. */
static void test_jam_reports_flow(void)
{
    static fake_zdev_t door;
    device(&door, 0x7A79u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    bind(6u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(60000u);
    static const uint8_t CLOSED[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x17, 0x00};
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK,
                          fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, CLOSED, sizeof CLOSED));
    run_for(2000u);
    zone_hub_info_t in;
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_TRUE(in.known);
    TEST_ASSERT_FALSE(in.active);
    TEST_ASSERT_FALSE(W.radio_trouble);

    /* (c) ZW-024: the trouble when the jam is found, not 90 s later */
    air(-50, true);
    TEST_ASSERT_TRUE_MESSAGE(W.radio_trouble, "sensor_radio waited for the link's grace");
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_TRUE(in.link_down);

    /* (a) ZW-024, ZW-013: the door opens during the jam - the hub hears it */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK,
                          fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(2000u);
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_TRUE_MESSAGE(in.active, "a door that opened during a jam was dropped");

    /* (b) ZW-013: a supervised report is processed and answered without waiting for the air */
    uint8_t spv[16];
    const size_t n = zw_cc_wrap_supervision(spv, sizeof spv, 0x21u, false, CLOSED, sizeof CLOSED);
    TEST_ASSERT_TRUE(n > 0u);
    const uint32_t reports = door.spv_reports;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, spv, n));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reports + 1u, door.spv_reports,
                                     "the Supervision Report waited for the jam to end");
    TEST_ASSERT_EQUAL_UINT8(0x21u, door.spv_session);
    TEST_ASSERT_EQUAL_HEX8(ZW_SUPERVISION_STATUS_SUCCESS, door.spv_status);
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_FALSE(in.active);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(st.jammed); /* all of it during the jam */

    /* (d) ZW-024: the air clears - its last state (open) is kept, its state asked afresh */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK,
                          fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(2000u);
    const uint32_t gets = W.p.bridge.stats.gets;
    const unsigned asked = fake_zdev_asked(&door, 0x71u, 0x04u);
    air(-95, false);
    run_for(1000u);
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_FALSE(in.link_down);
    TEST_ASSERT_TRUE(in.active);
    char w[24];
    zsrc_zw_state_words(zsrc_zw_find(&W.src, node), 0u, ZSRC_ZW_SLOT_CONTACT, W.wire.now, w,
                        sizeof w);
    TEST_ASSERT_EQUAL_STRING("Open", w);
    TEST_ASSERT_TRUE_MESSAGE(W.p.bridge.stats.gets > gets, "no fresh-state Get after the jam");
    fake_zdev_wake(&door, W.wire.now); /* the sleeper's Get goes when it wakes */
    run_for(5000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&door, 0x71u, 0x04u) > asked);
    TEST_ASSERT_FALSE(W.radio_trouble);
}

/* ZW-041: a module stuck in a watchdog loop (a restart every 30 s for 2 h) is logged at its first
 * restart, then once an hour at most: every restart is counted in the events' arg2, none twice,
 * none lost - the ones held back when the restarts stop are logged once the hour is up. A
 * restart the panel caused is still not logged nor counted. */
static void test_restart_loop_logged_once_an_hour(void)
{
    W.log_n = 0u;
    const uint32_t before = W.zac.restarts;
    for (unsigned i = 0; i < 240u; i++) {
        TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x03u));
        run_for(30000u);
        if (i == 0u) {
            TEST_ASSERT_EQUAL_UINT(1u, W.log_n); /* the first at once */
            TEST_ASSERT_EQUAL_UINT8(1u, W.log[0].arg2);
        }
    }
    const uint32_t restarts = W.zac.restarts - before;
    TEST_ASSERT_EQUAL_UINT32(240u, restarts);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&W.p.net.host)); /* it came back each time */
    size_t events = logged_n(ALARM_EV_ZWAVE_RADIO_RESTARTED);
    TEST_ASSERT_TRUE(events >= 2u && events <= 3u);
    TEST_ASSERT_EQUAL_UINT8(1u, W.log[0].arg2);
    TEST_ASSERT_EQUAL_UINT8(0x03u, W.log[0].arg);
    unsigned counted = 0u;
    for (size_t i = 0; i < W.log_n; i++) {
        TEST_ASSERT_EQUAL_UINT8(ALARM_EV_ZWAVE_RADIO_RESTARTED, W.log[i].type);
        TEST_ASSERT_EQUAL_UINT16(0u, W.log[i].zone);
        TEST_ASSERT_TRUE(W.log[i].arg2 >= 1u);
        counted += (unsigned)W.log[i].arg2;
    }
    TEST_ASSERT_TRUE(counted < restarts); /* some still held: the hour isn't up */
    TEST_ASSERT_TRUE(zwz_panel_next_deadline(&W.p) <= W.p.restart_logged_at + ZWZ_PANEL_RESTART_LOG_MS);
    run_for(60u * MIN_MS); /* quiet now: what was held is logged once the hour is up */
    events = logged_n(ALARM_EV_ZWAVE_RADIO_RESTARTED);
    TEST_ASSERT_TRUE(events <= 3u);
    counted = 0u;
    for (size_t i = 0; i < W.log_n; i++) {
        counted += (unsigned)W.log[i].arg2;
    }
    TEST_ASSERT_EQUAL_UINT(restarts, counted);
    TEST_ASSERT_EQUAL_UINT8(0u, W.p.restarts_held);
    TEST_ASSERT_EQUAL_UINT32(0u, W.p.log_lost);

    /* the hour after the last logged one is up: the next restart is logged at once, by itself */
    W.log_n = 0u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x08u));
    run_for(30000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.log_n);
    TEST_ASSERT_EQUAL_UINT8(0x08u, W.log[0].arg);
    TEST_ASSERT_EQUAL_UINT8(1u, W.log[0].arg2);
    /* within the hour: one the panel caused is neither logged nor held */
    zwz_panel_expect_restart(&W.p, W.wire.now, 5000u);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x09u));
    run_for(30000u);
    TEST_ASSERT_EQUAL_UINT8(0u, W.p.restarts_held);
    TEST_ASSERT_EQUAL_UINT(1u, W.log_n);
}

/* ZW-041: a network check whose module list misses 12 cached devices logs all 12 "left" (more
 * than the owner takes in one go: the rest wait for its next turn); past the queue's room each
 * event is counted in log_lost, never overwriting the waiting ones - except the restarts, which
 * are held (counted) until the queue has room, so a batch is never lost. */
static void test_many_devices_left_at_once(void)
{
    static fake_zdev_t devs[12];
    for (uint16_t n = 0; n < 12u; n++) {
        device(&devs[n], 0xC0DE00u + n, false);
        fake_zdev_listening(&devs[n]);
        TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &devs[n]));
        fake_zac93_join(&W.zac, &devs[n], (uint16_t)(5u + n));
    }
    boot(); /* the panel learns them from the module's list */
    run_for(5u * MIN_MS);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT16(12u, st.devices);
    TEST_ASSERT_TRUE(W.nodes_len > 0u);
    for (uint16_t n = 0; n < 12u; n++) {
        zw_node_list_set(W.zac.id.node_list, (uint16_t)(5u + n), false);
    }
    W.log_n = 0u;
    /* the panel restarts: the network check finds all 12 gone in one call, before the owner
     * takes any */
    boot();
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(12u, logged_n(ALARM_EV_ZWAVE_LEFT));
    for (size_t i = 0; i < W.log_n; i++) {
        if (W.log[i].type == ALARM_EV_ZWAVE_LEFT) {
            TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_LEFT_MISSING, W.log[i].arg2);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0u, W.p.log_lost);
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT16(0u, st.devices);

    /* full: the waiting ones are kept; a restart is held, not lost (the review's batch case) */
    for (size_t i = 0; i < ZWZ_PANEL_LOG_QUEUE; i++) {
        W.p.log[i] = (zwz_log_t){.type = (uint8_t)ALARM_EV_ZWAVE_LEFT, .arg = 1u};
    }
    W.p.log_n = ZWZ_PANEL_LOG_QUEUE;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x03u));
    fake_wire_run_until(&W.wire, W.wire.now + 30000u);
    TEST_ASSERT_EQUAL_UINT(ZWZ_PANEL_LOG_QUEUE, W.p.log_n);
    TEST_ASSERT_EQUAL_UINT32(0u, W.p.log_lost);
    TEST_ASSERT_EQUAL_UINT8(1u, W.p.restarts_held);
    TEST_ASSERT_FALSE(W.p.restart_logged);
    TEST_ASSERT_FALSE(W.wire.stuck); /* no deadline of "now" while the queue stays full */
    TEST_ASSERT_TRUE(ZWZ_PANEL_LOG_QUEUE >= ZW_NODES_MAX + ZWZ_PANEL_LOG_MAX);
    /* any other event that finds the queue full is counted in log_lost: a network reset */
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_RESET;
    r.user = 2u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    fake_wire_run_until(&W.wire, W.wire.now + 8000u);
    TEST_ASSERT_FALSE(W.wire.stuck);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_DONE, progress().result);
    TEST_ASSERT_EQUAL_UINT(ZWZ_PANEL_LOG_QUEUE, W.p.log_n);
    TEST_ASSERT_EQUAL_UINT32(1u, W.p.log_lost);
    TEST_ASSERT_EQUAL_UINT8(1u, W.p.restarts_held); /* the reset's own restart isn't counted */
    /* room again: the held restart is logged, by itself, with its count */
    W.p.log_n = 0u;
    W.log_n = 0u;
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, logged_n(ALARM_EV_ZWAVE_RADIO_RESTARTED));
    const zwz_log_t *l = logged(ALARM_EV_ZWAVE_RADIO_RESTARTED);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(0x03u, l->arg);
    TEST_ASSERT_EQUAL_UINT8(1u, l->arg2);
    TEST_ASSERT_EQUAL_UINT8(0u, W.p.restarts_held);
    TEST_ASSERT_TRUE(W.p.restart_logged);
}

/* ZW-024, ZSRC-015 (network.md §9): the radio restarts by itself during a jam - zw_net keeps the
 * jam, so the zone source does too: the hub keeps the link down after the restart (one
 * sensor_radio trouble, never a CHECK per zone) until the air clears. */
static void test_jam_survives_a_module_restart(void)
{
    static fake_zdev_t door;
    device(&door, 0x7A7Au, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    bind(6u, progress().node, ZSRC_ZW_SLOT_CONTACT);
    run_for(60000u);
    air(-50, true);
    TEST_ASSERT_TRUE(W.radio_trouble);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x03u)); /* its watchdog */
    run_for(30000u);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_TRUE(st.jammed);
    TEST_ASSERT_TRUE(W.src.link_up);
    zone_hub_info_t in;
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_TRUE_MESSAGE(in.link_down, "the hub heard the link up while still jammed");
    TEST_ASSERT_TRUE(W.radio_trouble);
    run_for(3u * MIN_MS); /* past the link's grace: still the one trouble */
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_TRUE(in.link_down);
    TEST_ASSERT_TRUE(W.radio_trouble);
    air(-95, false);
    run_for(1000u);
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_FALSE(in.link_down);
}

/* ZW-024, ZW-032, ZSRC-015 (network.md §9): the radio stops answering during a jam - a real link
 * down resets zw_net's detector without an event, so the source forgets the jam too: once the
 * module answers again over quiet air, the hub hears the link up instead of staying down. */
static void test_jam_then_a_real_link_down(void)
{
    static fake_zdev_t door;
    device(&door, 0x7A7Bu, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    bind(6u, progress().node, ZSRC_ZW_SLOT_CONTACT);
    run_for(60000u);
    air(-50, true);
    TEST_ASSERT_TRUE(W.radio_trouble);
    const uint32_t downs = zw_host_stats(&W.p.net.host)->link_downs;
    W.zac.fault.dead = true;
    for (unsigned i = 0; i < 180u && zw_host_stats(&W.p.net.host)->link_downs == downs; i++) {
        run_for(1000u);
    }
    TEST_ASSERT_TRUE(zw_host_stats(&W.p.net.host)->link_downs > downs);
    TEST_ASSERT_FALSE(W.src.jammed); /* nothing known of the air without the radio */
    W.zac.bg_rssi[0] = W.zac.bg_rssi[1] = W.zac.bg_rssi[2] = -95;
    W.zac.fault.dead = false;
    run_for(10u * MIN_MS);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RADIO_WORKING, st.radio);
    TEST_ASSERT_FALSE(st.jammed);
    TEST_ASSERT_TRUE(W.src.link_up);
    zone_hub_info_t in;
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, 6u, &in));
    TEST_ASSERT_FALSE_MESSAGE(in.link_down, "the hub is stuck with the link down");
}

/* ZW-024 (network.md §11): "Rebuild routes" from the pages - the whole network in the page's
 * numbers (a sleeper waiting), the installer stopping it; one device's; installer only. */
static void test_rebuild_routes(void)
{
    static fake_zdev_t lamp, plug, door;
    device(&lamp, 0x4071u, false);
    fake_zdev_listening(&lamp);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&lamp, 0u));
    const uint16_t a = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    device(&plug, 0x4072u, false);
    fake_zdev_listening(&plug);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&plug, 0u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(30000u);

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_ROUTES, 0u, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ROUTES, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_ROUTES, progress().op);
    TEST_ASSERT_EQUAL_UINT16(2u, progress().routes_total);
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT16(2u, st.routes_total);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask(UI_ZW_REQ_ADD, 0u, true));
    run_for(10000u);
    ui_zw_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_DONE, g.result);
    TEST_ASSERT_EQUAL_UINT16(2u, g.routes_done);
    TEST_ASSERT_EQUAL_UINT16(0u, g.routes_failed);
    TEST_ASSERT_EQUAL_UINT16(0u, g.generation); /* the whole network's */
    zwz_panel_status(&W.p, W.wire.now, &st);
    TEST_ASSERT_EQUAL_UINT16(0u, st.routes_total); /* not running any more */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* one device's */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ROUTES, a, true));
    run_for(5000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_DONE, g.result);
    TEST_ASSERT_EQUAL_UINT16(a, g.node);
    TEST_ASSERT_EQUAL_UINT16(zwz_panel_generation(&W.p, a), g.generation);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* a sleeper waits; the installer stops it */
    device(&door, 0x4073u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(60000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ROUTES, 0u, true));
    run_for(10000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_WORKING, g.step);
    TEST_ASSERT_EQUAL_UINT16(1u, g.routes_waiting);
    /* BL-152 ZW-024: the sleeper is named while it waits, and as skipped once it is stopped */
    TEST_ASSERT_EQUAL_UINT8(1u, g.routes_list_n);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_ROUTE_WAITING, g.routes_list[0].what);
    const uint16_t waiter = g.routes_list[0].node;
    TEST_ASSERT_TRUE(waiter != 0u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP, 0u, true));
    g = progress();
    TEST_ASSERT_EQUAL_UINT16(1u, g.routes_skipped);
    TEST_ASSERT_EQUAL_UINT8(1u, g.routes_list_n);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_ROUTE_SKIPPED, g.routes_list[0].what);
    TEST_ASSERT_EQUAL_UINT16(waiter, g.routes_list[0].node);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
    TEST_ASSERT_TRUE(g.routes_stopped);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_STOPPED, g.result);
}

/* Runs until the operation's end (its result), at most `ms`. */
static ui_zw_progress_t until_done(uint64_t ms)
{
    ui_zw_progress_t g = progress();
    for (uint64_t t = 0u; t < ms && g.step != UI_ZW_STEP_DONE; t += 100u) {
        run_for(100u);
        g = progress();
    }
    return g;
}

/* ZW-024 (network.md §10): "Replace it" - a new device in a broken one's place, followed as an add
 * (`replacing`): its security question and PIN, then its zone - bound to the old device - hears
 * the new one without being bound again. ZW-029: the old device is checked first (pings, then
 * IsFailedNode) - one that answers a ping is "Not replaced: the old device answered"
 * (STILL_ANSWERS), and ReplaceFailedNode never goes. */
static void test_replace_on_the_panel(void)
{
    static fake_zdev_t old, fresh;
    device(&old, 0x0ED0u, false);
    fake_zdev_listening(&old);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&old, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(7u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(20000u);

    /* ZW-029: it still answers - not replaced, the radio never asked */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REPLACE, node, true));
    TEST_ASSERT_TRUE(progress().replacing);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_STARTING, progress().step); /* while it is checked */
    ui_zw_progress_t kept = until_done(20000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, kept.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_STILL_ANSWERS, kept.result);
    TEST_ASSERT_EQUAL_UINT32(0u, W.zac.by_func[ZW_FUNC_REPLACE_FAILED_NODE]);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    old.silent = true; /* broken: the check's unanswered pings make the radio list it */
    device(&fresh, 0x0ED1u, false);
    fake_zdev_listening(&fresh);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &fresh));

    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_REPLACE;
    r.user = 1u;
    r.node = node;
    r.generation = gen;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, zwz_panel_request(&W.p, W.wire.now, &r, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_ADD, progress().op);
    TEST_ASSERT_TRUE(progress().replacing);
    for (unsigned i = 0; i < 150u && progress().step == UI_ZW_STEP_STARTING; i++) {
        run_for(100u); /* the check: 3 pings 2 s apart, then IsFailedNode */
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_LISTENING, progress().step);
    TEST_ASSERT_TRUE(fake_zac93_press_replace(&W.zac, W.wire.now, &fresh));
    ui_zw_progress_t g = progress();
    for (unsigned i = 0; i < 1200u && g.step != UI_ZW_STEP_DONE; i++) {
        run_for(100u);
        g = progress();
        ui_zw_request_t a;
        memset(&a, 0, sizeof a);
        if (g.step == UI_ZW_STEP_ASK_ALLOW) {
            a.kind = UI_ZW_REQ_ALLOW;
            a.keys = g.requested;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &a, true));
        } else if (g.step == UI_ZW_STEP_ASK_PIN) {
            a.kind = UI_ZW_REQ_PIN;
            a.pin = fresh.pin;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &a, true));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, g.result);
    TEST_ASSERT_TRUE(g.replacing);
    TEST_ASSERT_EQUAL_UINT16(node, g.node);
    TEST_ASSERT_EQUAL_UINT16(gen, g.generation); /* its zones' addresses are unchanged */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SEC_S2_ACCESS, g.security);
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&W.hub, 7u, &b)); /* never bound again */
    run_for(20000u);
    const unsigned before = W.zone_events;
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK,
                          fake_zdev_report(&fresh, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(3000u);
    TEST_ASSERT_TRUE_MESSAGE(W.zone_events > before, "the zone did not hear the new device");
}

/* ZW-024, ZW-029: the radio's refusals after the check, each with its own words - listed as failed
 * (IsFailedNode yes) but ReplaceFailedNode refused with bit 08: "doesn't count the old device as
 * failed yet" (REFUSED); another refusal bit: it could not be done (FAILED). Nothing replaced. */
static void test_replace_refused_on_the_panel(void)
{
    static fake_zdev_t old;
    device(&old, 0x0ED2u, false);
    fake_zdev_listening(&old);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&old, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(20000u);
    old.silent = true;
    static const struct {
        uint8_t why;
        uint8_t want;
    } CASES[] = {{0x08u, UI_ZW_RES_REFUSED}, {0x20u, UI_ZW_RES_FAILED}};
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        fake_rule_t rule;
        memset(&rule, 0, sizeof rule);
        rule.func = (uint8_t)ZW_FUNC_REPLACE_FAILED_NODE;
        rule.respond = true;
        rule.response.len = 1u;
        rule.response.bytes[0] = CASES[i].why;
        TEST_ASSERT_TRUE(fake_zac93_on(&W.zac, &rule));
        const uint32_t checks = W.zac.by_func[ZW_FUNC_IS_FAILED_NODE];
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REPLACE, node, true));
        const ui_zw_progress_t g = until_done(20000u);
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
        TEST_ASSERT_TRUE(g.replacing);
        TEST_ASSERT_EQUAL_UINT8(CASES[i].want, g.result);
        TEST_ASSERT_EQUAL_UINT32(1u, W.zac.by_func[ZW_FUNC_IS_FAILED_NODE] - checks);
        TEST_ASSERT_TRUE(zw_node_list_test(W.zac.failed, node)); /* the radio listed it */
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    }
    TEST_ASSERT_EQUAL_UINT16(gen, zwz_panel_generation(&W.p, node)); /* still there */
}

/* ---- ZW-039: the NVS budget (docs/features/20 devices.md "The NVS budget") ------------------- */

/* ZW-039: NVS "zwave" at its largest, entry by entry - the figure devices.md gives beside zbpanel's
 * 686 (test_zbz_contract.c). A stored value that grows changes this, and the budget with it. */
_Static_assert(ZWZ_NVS_ENTRIES(ZWZ_KEYS_BLOB) == 6u, "keys: index, header, 4 entries");
_Static_assert(ZWZ_NODES_BLOB_MAX == 52224u, "the node cache at its fullest (network.md §2)");
_Static_assert(ZWZ_NVS_ENTRIES(ZWZ_NODES_BLOB_MAX) == 1647u, "nodes: index, 14 headers, 1,632");
_Static_assert(ZWZ_NVS_ENTRIES(ZWZ_CFG_BLOB) == 3u, "cfg");
_Static_assert(ZWZ_NVS_ENTRIES(ZWZ_CHECKINS_BLOB) == 19u, "chk");
_Static_assert(ZWZ_NVS_ENTRIES(ZWO_GARAGE_BLOB) == 3u, "garage");
_Static_assert(ZWZ_NVS_ENTRIES(ZW_PROV_BLOB_MAX) == 59u, "prov");
_Static_assert(ZWZ_NVS_ENTRIES(ZWF_BLOB_SIZE) == 5u && ZWZ_NVS_ENTRIES(ZWKC_BLOB_SIZE) == 6u,
               "keypad_fb and keypad_ctl");
_Static_assert(ZWZ_NVS_WORST == 1753u, "zwave at its largest: devices.md's budget");
_Static_assert(ZWZ_NVS_NODE_GROWTH == 27u, "one more record: 25 entries, a header, the rounding");
/* ZW-039: the reserve covers the configuration's rewrite at its largest and a record's growth;
 * adding stops before it. */
_Static_assert(ZWZ_NVS_RESERVE >= ALARM_CONFIG_BLOB_MAX / ZWZ_NVS_ENTRY_BYTES + 25u,
               "the configuration can always be written again");
_Static_assert(ZWZ_NVS_ADD_MIN > ZWZ_NVS_RESERVE, "adding stops before the reserve");
/* ZW-039: the result is appended; the ones before keep their values (ui_backend.h). */
_Static_assert((int)UI_ZW_REQ_LOW_BATTERY == 6 && (int)UI_ZW_REQ_FULL == 7, "FULL appended");

static ui_zw_req_result_t ask_replace(uint16_t node, uint16_t gen, bool allowed)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_REPLACE;
    r.user = 1u;
    r.node = node;
    r.generation = gen;
    const ui_zw_req_result_t res = zwz_panel_request(&W.p, W.wire.now, &r, allowed);
    run_for(0u);
    return res;
}

static bool status_storage_full(void)
{
    ui_zw_status_t st;
    zwz_panel_status(&W.p, W.wire.now, &st);
    return st.storage_full;
}

/* ZW-039: below the reserve Add and Replace answer FULL and start nothing, and the status says the
 * storage is full; at it they start; not told (the PC, before the service reads NVS) there is no
 * limit. */
static void test_nvs_reserve_add_and_replace(void)
{
    /* not told: no limit */
    TEST_ASSERT_EQUAL_UINT32(ZWZ_NVS_NOT_TOLD, W.p.nvs_free);
    TEST_ASSERT_FALSE(zwz_panel_storage_full(&W.p));
    TEST_ASSERT_FALSE(status_storage_full());
    TEST_ASSERT_EQUAL_UINT32(ZWZ_NVS_ADD_MIN, zwz_panel_nvs_need(&W.p)); /* a small network */

    static fake_zdev_t old, fresh;
    device(&old, 0x0E39u, false);
    fake_zdev_listening(&old);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&old, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(20000u);

    /* one entry short: refused, nothing listens, nothing is followed */
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN - 1u);
    TEST_ASSERT_TRUE(zwz_panel_storage_full(&W.p));
    TEST_ASSERT_TRUE(status_storage_full());
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_ADD, 0u, false)); /* who first */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    old.silent = true; /* broken: Replace would start */
    zw_node_list_set(W.zac.failed, node, true);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask_replace(node, gen, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask_replace(node, gen, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_replace(node, (uint16_t)(gen + 1u), true));
    /* nearly empty NVS: still refused, and changing what is there still works */
    zwz_panel_nvs_free(&W.p, 0u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_check_in(0u, 0u, 7200u, true));

    /* at the reserve: they start */
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN);
    TEST_ASSERT_FALSE(zwz_panel_storage_full(&W.p));
    TEST_ASSERT_FALSE(status_storage_full());
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_ADD, progress().op);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP, 0u, true));
    run_for(3000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    device(&fresh, 0x0E3Au, false);
    fake_zdev_listening(&fresh);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &fresh));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_replace(node, gen, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_ADD, progress().op);
    TEST_ASSERT_TRUE(progress().replacing);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP, 0u, true));
    run_for(3000u);
}

/* ZW-039: a large node cache needs room for its own rewrite with one more device at its largest -
 * more than the reserve once the cache is bigger than the configuration. */
static void test_nvs_need_grows_with_the_cache(void)
{
    const uint8_t had = W.p.net.nodes.node_count;
    W.p.net.nodes.node_count = (uint8_t)(ZW_NODES_MAX - 1u); /* the count alone is read */
    const uint32_t need = zwz_panel_nvs_need(&W.p);
    W.p.net.nodes.node_count = had;
    /* the figures devices.md documents, worked out by hand, so a wrong formula can't match itself:
     * 63 devices -> 27 + the cache of 64 at their largest (52,224 bytes: 1 + 14 + 1,632) = 1,674 */
    TEST_ASSERT_EQUAL_UINT32(1674u, need);
    TEST_ASSERT_EQUAL_UINT32(1083u, ZWZ_NVS_ADD_MIN);
    TEST_ASSERT_EQUAL_UINT32(1056u, ZWZ_NVS_RESERVE);
    /* the boundary: up to 39 devices the need is ZWZ_NVS_ADD_MIN (39 -> 27 + 1,042 = 1,069);
     * from 40 the cache's rewrite passes it (27 + 1,067 = 1,094) */
    static const struct { uint8_t count; uint32_t need; } EDGE[] = {
        {0u, 1083u}, {38u, 1083u}, {39u, 1083u}, {40u, 1094u}};
    for (size_t i = 0; i < sizeof EDGE / sizeof EDGE[0]; i++) {
        W.p.net.nodes.node_count = EDGE[i].count;
        const uint32_t got = zwz_panel_nvs_need(&W.p);
        W.p.net.nodes.node_count = had;
        TEST_ASSERT_EQUAL_UINT32(EDGE[i].need, got);
    }
    TEST_ASSERT_EQUAL_UINT32(ZWZ_NVS_ADD_MIN, zwz_panel_nvs_need(&W.p));
}

/* ZW-039: below the reserve the owner's list answers no SmartStart request (n_smart_find's guard,
 * proven directly: in the firmware a join frame can be handled before the next poll stops the
 * listening), the radio stops listening, and a request on its way is not answered; with room
 * again it listens and the device joins. */
static void test_nvs_reserve_smartstart(void)
{
    static fake_zdev_t door;
    device(&door, 0x5E39u, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &door));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_ADDED,
                          zwz_panel_prov_register(&W.p, W.wire.now, door.dsk, "Back door", true));
    W.disarmed = true;
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN);
    run_for(2000u);
    TEST_ASSERT_TRUE(W.zac.smart_listen);

    /* the guard itself: short, the registered device's NWI home ID finds nothing; told one more
     * entry, it is found again (so the guard, not the list, refused it) */
    uint8_t nwi[4], auth[4];
    zw_dsk_home_ids(door.pub, nwi, auth);
    zw_incl_smartstart_t ss;
    memset(&ss, 0, sizeof ss);
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN - 1u);
    TEST_ASSERT_FALSE(W.p.net.port.smartstart_find(W.p.net.port.ctx, nwi, false, &ss));
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN);
    TEST_ASSERT_TRUE(W.p.net.port.smartstart_find(W.p.net.port.ctx, nwi, false, &ss));

    /* short, with the request already on its way: the next poll stops the listening first, and
     * the request is not answered */
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN - 1u);
    W.log_n = 0u;
    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&W.zac, W.wire.now, &door));
    run_for(20000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_WAITING, prov_entry().state);
    TEST_ASSERT_NULL(logged(ALARM_EV_ZWAVE_ADDED));
    TEST_ASSERT_FALSE(W.zac.smart_listen); /* and no longer listening */
    TEST_ASSERT_FALSE(fake_zac93_smartstart_ask(&W.zac, W.wire.now, &door));

    /* room again: it listens, and the device joins */
    zwz_panel_nvs_free(&W.p, ZWZ_NVS_ADD_MIN + 100u);
    run_for(2000u);
    TEST_ASSERT_TRUE(W.zac.smart_listen);
    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&W.zac, W.wire.now, &door));
    for (unsigned i = 0; i < 1200u && prov_entry().state != UI_ZW_PROV_JOINED; i++) {
        run_for(100u);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_PROV_JOINED, prov_entry().state);
    TEST_ASSERT_NOT_NULL(logged(ALARM_EV_ZWAVE_ADDED));
}

/* ---- a node ID's identity (ZW-014; zones.md §1: ZW31, ZW32) ------------------------------------ */

/* The generation in a zone's main address (zones.md §1: home << 32 | generation << 16 | node). */
static uint16_t bound_generation(uint16_t zone)
{
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&W.hub, zone, &b));
    return (uint16_t)((b.in[ZSRC_ROLE_MAIN].addr.id >> 16) & 0xFFFFu);
}

/* A door sensor added and bound to zone 2; its node ID. */
static uint16_t bound_door(fake_zdev_t *d, uint64_t seed)
{
    device(d, seed, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(d, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(2u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(5000u);
    return node;
}

static void remove_by_hand(fake_zdev_t *d)
{
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE, 0u, true));
    run_for(200u);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&W.zac, W.wire.now, d));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, progress().result);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
}

/* ZW-014 (ZW31): Remove, then Add on the removed device's node ID, with a restart between - the
 * node cache's writes refused at `where` (the counter, or the cache after it). While the cache
 * without the removed device is not stored, Add answers FULL - also after a restart, which reads
 * the old cache, forgets the removed device again and holds on; so does Remove. Once a
 * write succeeds the new device joins on that node ID with a new generation and keeps it across a
 * restart; the removed device's zone stays bound to the old generation, fed by nothing. */
static void identity_after_refused_removal(uint8_t where)
{
    static fake_zdev_t old, fresh;
    const uint16_t node = bound_door(&old, 0x1D01u);
    const uint16_t gen = zwz_panel_generation(&W.p, node);
    W.refuse_nodes = where;
    remove_by_hand(&old);
    TEST_ASSERT_TRUE(W.refused_writes > 0u);
    TEST_ASSERT_TRUE(zwz_panel_identity_held(&W.p));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_ADD, 0u, false)); /* who first */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_REMOVE, 0u, true)); /* a failed write */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);

    /* the panel restarts on the old cache: the device is forgotten again, still held */
    boot();
    TEST_ASSERT_NULL(row_of(node));
    TEST_ASSERT_TRUE(zwz_panel_identity_held(&W.p));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));

    /* NVS works again: held until a write succeeds (the one after this request) */
    W.refuse_nodes = REFUSE_NONE;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&W.p));
    device(&fresh, 0x1D02u, true);
    fresh.m.product_id = 0x7777u; /* another model */
    W.zac.next_node = node;       /* the module gives the removed device's node ID */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&fresh, 0u));
    TEST_ASSERT_EQUAL_UINT16(node, progress().node);
    const uint16_t fresh_gen = progress().generation;
    TEST_ASSERT_TRUE(fresh_gen > gen);
    TEST_ASSERT_EQUAL_UINT16(0u, zwz_panel_zone_of(&W.p, node, fresh_gen));
    boot();
    TEST_ASSERT_EQUAL_UINT16(fresh_gen, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(0u, zwz_panel_zone_of(&W.p, node, fresh_gen));
    TEST_ASSERT_EQUAL_UINT16(gen, bound_generation(2u)); /* the old zone waits for its device */
}

static void test_identity_refused_counter_write(void)
{
    identity_after_refused_removal(REFUSE_GEN);
}

static void test_identity_refused_cache_write(void)
{
    identity_after_refused_removal(REFUSE_CACHE);
}

/* ZW-014 (ZW31): the removal stored, the new device's writes refused at `where`, then a restart:
 * the stored cache lacks the node ID, so the new device is found as new - a generation above the
 * removed device's, never its (the zone of the removed one stays CHECK). While the writes fail,
 * Add answers FULL and SmartStart finds no entry; after a write succeeds both work again. */
static void identity_after_refused_add(uint8_t where)
{
    static fake_zdev_t old, fresh, later;
    const uint16_t node = bound_door(&old, 0x1D11u);
    const uint16_t gen = zwz_panel_generation(&W.p, node);
    remove_by_hand(&old);
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&W.p));
    device(&later, 0x1D13u, false); /* a SmartStart device waiting */
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &later));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_ADDED,
                          zwz_panel_prov_register(&W.p, W.wire.now, later.dsk, "Back door", true));
    uint8_t nwi[4], auth[4];
    zw_dsk_home_ids(later.pub, nwi, auth);
    zw_incl_smartstart_t ss;
    memset(&ss, 0, sizeof ss);
    TEST_ASSERT_TRUE(W.p.net.port.smartstart_find(W.p.net.port.ctx, nwi, false, &ss));

    W.refuse_nodes = where;
    device(&fresh, 0x1D12u, true);
    W.zac.next_node = node;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&fresh, 0u));
    TEST_ASSERT_EQUAL_UINT16(node, progress().node);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_TRUE(zwz_panel_identity_held(&W.p));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_FALSE(W.p.net.port.smartstart_find(W.p.net.port.ctx, nwi, false, &ss));

    boot();
    const uint16_t now_gen = zwz_panel_generation(&W.p, node);
    TEST_ASSERT_TRUE(now_gen > gen);
    TEST_ASSERT_EQUAL_UINT16(0u, zwz_panel_zone_of(&W.p, node, now_gen));
    TEST_ASSERT_EQUAL_UINT16(gen, bound_generation(2u));

    W.refuse_nodes = REFUSE_NONE;
    run_for(1000u);
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&W.p));
    TEST_ASSERT_TRUE(W.p.net.port.smartstart_find(W.p.net.port.ctx, nwi, false, &ss));
    boot();
    TEST_ASSERT_EQUAL_UINT16(now_gen, zwz_panel_generation(&W.p, node)); /* stored now */
}

static void test_identity_add_refused_counter_write(void)
{
    identity_after_refused_add(REFUSE_GEN);
}

static void test_identity_add_refused_cache_write(void)
{
    identity_after_refused_add(REFUSE_CACHE);
}

/* ZW-014 (ZW31): the removal's write only due, not refused (before the owner's next pass): Add
 * answers BUSY, and starts once the pass stored it. */
static void test_identity_waits_for_due_write(void)
{
    static fake_zdev_t old;
    (void)bound_door(&old, 0x1D21u);
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = UI_ZW_REQ_REMOVE;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    run_for(200u);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&W.zac, W.wire.now, &old));
    fake_wire_run_until(&W.wire, W.wire.now + 3000u); /* no store pass yet */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, progress().result);
    TEST_ASSERT_TRUE(zwz_panel_identity_held(&W.p));
    r.kind = UI_ZW_REQ_DISMISS;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &r, true));
    r.kind = UI_ZW_REQ_ADD;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, zwz_panel_request(&W.p, W.wire.now, &r, true));
    store();
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&W.p));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP, 0u, true));
    run_for(3000u);
}

/* ZW-014 (ZW32): the inclusion counter never wraps. Stored at 65534, the next device gets 65535;
 * then Add answers FULL and SmartStart finds nothing - also after a restart with 65535 stored -
 * while "Replace a failed device", which keeps the generation, still starts. */
static void test_generations_used_up(void)
{
    W.setup.generation = 65534u; /* as stored */
    boot();
    static fake_zdev_t old, fresh, later;
    device(&old, 0x6E01u, false);
    fake_zdev_listening(&old);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&old, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_UINT16(65535u, progress().generation);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(3u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(20000u);
    TEST_ASSERT_FALSE(zw_net_generations_left(&W.p.net));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_ADD, 0u, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
    device(&later, 0x6E03u, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &later));
    TEST_ASSERT_EQUAL_INT(UI_ZW_PROV_ADDED,
                          zwz_panel_prov_register(&W.p, W.wire.now, later.dsk, NULL, true));
    uint8_t nwi[4], auth[4];
    zw_dsk_home_ids(later.pub, nwi, auth);
    zw_incl_smartstart_t ss;
    memset(&ss, 0, sizeof ss);
    TEST_ASSERT_FALSE(W.p.net.port.smartstart_find(W.p.net.port.ctx, nwi, false, &ss));

    boot(); /* 65535 stored */
    TEST_ASSERT_EQUAL_UINT16(65535u, W.setup.generation);
    TEST_ASSERT_EQUAL_UINT16(65535u, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_ADD, 0u, true));

    old.silent = true; /* broken: Replace keeps its generation, so it may start */
    zw_node_list_set(W.zac.failed, node, true);
    device(&fresh, 0x6E02u, false);
    fake_zdev_listening(&fresh);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &fresh));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_replace(node, 65535u, true));
    TEST_ASSERT_TRUE(progress().replacing);
    /* network.md §10 (Z-Wave JS keeps the node): the new device joins in its place and keeps
     * generation 65535 and its zone - also across a restart */
    for (unsigned i = 0; i < 150u && progress().step == UI_ZW_STEP_STARTING; i++) {
        run_for(100u); /* the check: 3 pings 2 s apart, then IsFailedNode */
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_LISTENING, progress().step);
    TEST_ASSERT_TRUE(fake_zac93_press_replace(&W.zac, W.wire.now, &fresh));
    ui_zw_progress_t g = progress();
    for (unsigned i = 0; i < 1200u && g.step != UI_ZW_STEP_DONE; i++) {
        run_for(100u);
        g = progress();
        ui_zw_request_t a;
        memset(&a, 0, sizeof a);
        if (g.step == UI_ZW_STEP_ASK_ALLOW) {
            a.kind = UI_ZW_REQ_ALLOW;
            a.keys = g.requested;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &a, true));
        } else if (g.step == UI_ZW_STEP_ASK_PIN) {
            a.kind = UI_ZW_REQ_PIN;
            a.pin = fresh.pin;
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_request(&W.p, W.wire.now, &a, true));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, g.result);
    TEST_ASSERT_EQUAL_UINT16(node, g.node);
    TEST_ASSERT_EQUAL_UINT16(65535u, g.generation);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_EQUAL_UINT16(65535u, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(3u, zwz_panel_zone_of(&W.p, node, 65535u));
    TEST_ASSERT_EQUAL_UINT16(65535u, bound_generation(3u));
    run_for(20000u);
    boot();
    TEST_ASSERT_EQUAL_UINT16(65535u, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(3u, zwz_panel_zone_of(&W.p, node, 65535u));
    TEST_ASSERT_FALSE(zw_net_generations_left(&W.p.net));
}

/* ZW-014 (ZW31, review 2): A removed and stored; B added on A's node ID at the next generation,
 * the counter's write refused (storage still has the old counter and a cache without B); a zone
 * bound to B. While that write fails, removing B is refused (FULL) - by hand, as failed, and by
 * itself: a device reset locally stays in the module and the cache as gone. Otherwise a restart
 * would read neither B nor its generation, and the next device on that node ID would get B's
 * generation and feed B's zone. A restart finds B again with its own generation (its zone stays
 * B's); once a write succeeds B's removal goes on, and a new device on its node ID gets a newer
 * generation that feeds no zone - also after a restart. */
static void test_identity_failed_write_holds_removal(void)
{
    static fake_zdev_t a, b, c;
    const uint16_t node = bound_door(&a, 0x1D31u);
    remove_by_hand(&a);
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&W.p));
    W.refuse_nodes = REFUSE_GEN;
    W.zac.next_node = node;
    TEST_ASSERT_EQUAL_UINT16(node, bound_door(&b, 0x1D32u)); /* zone 2 now B's */
    const uint16_t gen_b = zwz_panel_generation(&W.p, node);
    TEST_ASSERT_EQUAL_UINT16(gen_b, bound_generation(2u));
    TEST_ASSERT_TRUE(W.refused_writes > 0u);
    TEST_ASSERT_NOT_EQUAL(gen_b, W.setup.generation); /* the counter as stored is older */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_REMOVE, 0u, false)); /* who first */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_REMOVE, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_REMOVE_FAILED, node, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);

    /* a restart: B is found again - with its own generation, so its zone is still its own */
    boot();
    TEST_ASSERT_EQUAL_UINT16(gen_b, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(2u, zwz_panel_zone_of(&W.p, node, gen_b));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_FULL, ask(UI_ZW_REQ_REMOVE, 0u, true));

    /* B resets itself: gone, but not removed from the module while the write fails */
    const uint32_t removals = W.zac.by_func[ZW_FUNC_REMOVE_FAILED_NODE];
    fake_zdev_reset_locally(&b, W.wire.now);
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT32(removals, W.zac.by_func[ZW_FUNC_REMOVE_FAILED_NODE]);
    TEST_ASSERT_TRUE(zw_node_list_test(W.zac.id.node_list, node));
    TEST_ASSERT_NOT_NULL(zw_node_find_const(&W.p.net.nodes, node));

    /* storage works again: the write succeeds, then B's removal goes on */
    W.refuse_nodes = REFUSE_NONE;
    run_for(0u); /* the owner's next pass stores it */
    TEST_ASSERT_TRUE(zw_node_list_test(W.zac.id.node_list, node));
    run_for(MIN_MS);
    TEST_ASSERT_FALSE(zw_node_list_test(W.zac.id.node_list, node));
    TEST_ASSERT_NULL(zw_node_find_const(&W.p.net.nodes, node));
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&W.p));
    TEST_ASSERT_EQUAL_UINT16(gen_b, W.setup.generation); /* stored with the cache */

    device(&c, 0x1D33u, true);
    c.m.product_id = 0x7777u; /* another model */
    W.zac.next_node = node;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&c, 0u));
    TEST_ASSERT_EQUAL_UINT16(node, progress().node);
    const uint16_t gen_c = progress().generation;
    TEST_ASSERT_TRUE(gen_c > gen_b);
    TEST_ASSERT_EQUAL_UINT16(0u, zwz_panel_zone_of(&W.p, node, gen_c));
    boot();
    TEST_ASSERT_EQUAL_UINT16(gen_c, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(0u, zwz_panel_zone_of(&W.p, node, gen_c));
    TEST_ASSERT_EQUAL_UINT16(gen_b, bound_generation(2u)); /* B's zone waits for B */
}

/* ---- ZW-033: a device's software update ------------------------------------------------------- */

static uint8_t IMAGE[300];

static void fw_device(fake_zdev_t *d, uint64_t seed, bool sleeps)
{
    device(d, seed, sleeps);
    if (!sleeps) {
        fake_zdev_listening(d);
    }
    d->m.secure_ccs[d->m.secure_n++] = ZW_CC_FIRMWARE_UPDATE;
    d->m.versions[d->m.versions_n++] = (fake_cc_version_t){ZW_CC_FIRMWARE_UPDATE, 5};
    d->fw.on = true;
    d->fw.firmware_id = 0x0042u;
    d->fw.wait_s = 3u;
    for (size_t i = 0; i < sizeof IMAGE; i++) {
        IMAGE[i] = (uint8_t)(i * 13u + 1u);
    }
}

static ui_zw_req_result_t fw_start(uint16_t node, bool allowed)
{
    const ui_zw_req_result_t r = zwz_panel_fw_start(&W.p, W.wire.now, node,
                                                    zwz_panel_generation(&W.p, node), 0u, IMAGE,
                                                    sizeof IMAGE, allowed, 1u);
    run_for(0u);
    return r;
}

static ui_zw_progress_t fw_end(uint64_t ms)
{
    for (uint64_t t = 0; t < ms && progress().step != UI_ZW_STEP_DONE; t += 100u) {
        run_for(100u);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_FIRMWARE, progress().op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    return progress();
}

/* ZW-033: a sleeping device's update - its page offers it (its versions known), only with
 * installer mode; it waits for the device's wake-up (the page says so) and keeps it awake;
 * nothing else runs on the network meanwhile; the image arrives whole and checked; logged; the
 * device let sleep; its setup read again after its restart. */
static void test_firmware_update_sleeping_device(void)
{
    static fake_zdev_t door;
    fw_device(&door, 0xF1A1u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    run_for(40000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&door, W.wire.now));
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, zwz_panel_generation(&W.p, node),
                                      &page));
    TEST_ASSERT_TRUE(page.fw.known);
    TEST_ASSERT_EQUAL_UINT8(1u, page.fw.major); /* the fake's Version Report: 1.10 */
    TEST_ASSERT_EQUAL_UINT8(10u, page.fw.minor);
    TEST_ASSERT_TRUE(page.fw.updatable);

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, fw_start(node, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID,
                          zwz_panel_fw_start(&W.p, W.wire.now, node, 99u, 0u, IMAGE, sizeof IMAGE,
                                             true, 1u)); /* not this device's generation */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID,
                          zwz_panel_fw_start(&W.p, W.wire.now, node,
                                             zwz_panel_generation(&W.p, node), 1u, IMAGE,
                                             sizeof IMAGE, true, 1u)); /* no such target */
    TEST_ASSERT_EQUAL_UINT(0u, logged_n(ALARM_EV_ZWAVE_SOFTWARE));

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_FIRMWARE, progress().op);
    run_for(2000u);
    TEST_ASSERT_TRUE(progress().fw_wake); /* "Wake it up now" */
    TEST_ASSERT_EQUAL_UINT16(0u, progress().fw_total);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask(UI_ZW_REQ_ADD, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, fw_start(node, true));
    TEST_ASSERT_EQUAL_UINT(1u, logged_n(ALARM_EV_ZWAVE_SOFTWARE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_STARTED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    TEST_ASSERT_EQUAL_UINT8(1u, logged(ALARM_EV_ZWAVE_SOFTWARE)->user);

    const unsigned version_gets = fake_zdev_asked(&door, 0x86u, 0x11u);
    fake_zdev_wake(&door, W.wire.now);
    for (unsigned k = 0; k < 50u && progress().fw_wake; k++) {
        run_for(100u); /* its Wake Up Notification, then the queued question */
    }
    TEST_ASSERT_FALSE(progress().fw_wake);
    const ui_zw_progress_t g = fw_end(10u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_UPDATED, g.result);
    TEST_ASSERT_EQUAL_UINT16(3u, g.fw_wait_s);
    TEST_ASSERT_TRUE(g.fw_total > 1u);
    TEST_ASSERT_EQUAL_UINT16(g.fw_total, g.fw_done);
    TEST_ASSERT_EQUAL_UINT32(1u, door.fw.updates);
    TEST_ASSERT_EQUAL_UINT(sizeof IMAGE, door.fw.len);
    TEST_ASSERT_EQUAL_MEMORY(IMAGE, door.fw.image, sizeof IMAGE);
    TEST_ASSERT_EQUAL_UINT(2u, logged_n(ALARM_EV_ZWAVE_SOFTWARE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_UPDATED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    TEST_ASSERT_EQUAL_UINT16(0u, W.p.net.hold_awake); /* it may sleep again */
    TEST_ASSERT_EQUAL_UINT32(0u, door.heard_asleep);
    /* after its restart (3 s + 5 s, Z-Wave JS's margin) its setup is read again without waiting
     * for a wake-up (BL-152, refreshInfo({ waitForWakeup: false })): it holds its scratch record
     * now, and its questions wait in its wake-up queue */
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT16(0u, W.p.fw_reinterview);
    TEST_ASSERT_TRUE_MESSAGE(zw_iv_asking(&W.p.net.iv, node), "it waited for a wake-up");
    fake_zdev_wake(&door, W.wire.now);
    run_for(60000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&door, 0x86u, 0x11u) > version_gets);
    TEST_ASSERT_TRUE(wait_state(node, UI_ZW_DEV_READY, 60000u));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op);
}

/* ZW-033: every way it ends without the new software, each told apart - the device refuses
 * (battery, identity), finds the image damaged, stops asking; the installer stops it; the radio
 * goes; a sleeper nobody wakes. A fragment lost on the way is asked for again and the update
 * still succeeds. Nothing ends up held awake. */
static void test_firmware_update_outcomes(void)
{
    static fake_zdev_t sw;
    fw_device(&sw, 0xF1A2u, false);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&sw, 0u));
    const uint16_t node = progress().node;
    run_for(5000u);

    sw.fw.refuse_set = true;
    sw.fw.refuse = ZW_FWU_REQ_BATTERY_LOW;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    ui_zw_progress_t g = fw_end(MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_FAILED, g.result);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_LOW_BATTERY, g.fw_why);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_FAILED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    sw.fw.refuse = ZW_FWU_REQ_WRONG_HARDWARE;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true)); /* a new start after the end */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_REFUSED, fw_end(MIN_MS).fw_why);
    sw.fw.refuse_set = false;

    /* the device's own manufacturer isn't what the panel knows it as */
    sw.m.manufacturer = (uint16_t)(sw.m.manufacturer + 1u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_WRONG_DEVICE, fw_end(MIN_MS).fw_why);
    sw.m.manufacturer = (uint16_t)(sw.m.manufacturer - 1u);

    /* a fragment lost: asked again, and it still takes it whole */
    sw.fw.drop_at = 5u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    g = fw_end(5u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_UPDATED, g.result);
    TEST_ASSERT_EQUAL_MEMORY(IMAGE, sw.fw.image, sizeof IMAGE);
    TEST_ASSERT_EQUAL_UINT32(0u, sw.fw.bad);
    TEST_ASSERT_EQUAL_UINT32(0u, sw.fw.unexpected); /* the lost one asked again, nothing out of turn */
    run_for(20000u); /* its setup read again */

    /* it stops asking half way */
    sw.fw.quiet_after = 6u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    g = fw_end(5u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_NO_ANSWER, g.fw_why);
    TEST_ASSERT_TRUE(g.fw_done >= 6u && g.fw_done < g.fw_total);
    sw.fw.quiet_after = 0u;

    /* the installer stops it */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    run_for(50u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_STOP, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_STOPPED, progress().fw_why);
    run_for(5000u);

    /* its image found damaged (the panel sends a checksum that isn't the image's) */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    W.p.fw.checksum = (uint16_t)(W.p.fw.checksum ^ 0x5555u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_BAD_TRANSFER, fw_end(5u * MIN_MS).fw_why);

    /* the radio goes away */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    run_for(20u);
    W.zac.fault.dead = true;
    g = fw_end(5u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_FAILED, g.result);
    TEST_ASSERT_TRUE(g.fw_why == UI_ZW_FWF_NO_RADIO || g.fw_why == UI_ZW_FWF_SEND_FAILED ||
                     g.fw_why == UI_ZW_FWF_NO_ANSWER);
    W.zac.fault.dead = false;
    run_for(3u * MIN_MS);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true)); /* and works again after */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_UPDATED, fw_end(5u * MIN_MS).result);
    TEST_ASSERT_EQUAL_UINT16(0u, W.p.net.hold_awake);
    run_for(20000u);

    /* reset by hand half way: the update ends, logged as failed; the device is gone */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    run_for(300u);
    fake_zdev_reset_locally(&sw, W.wire.now);
    g = fw_end(MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_FAILED, g.result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_SW_FAILED, logged(ALARM_EV_ZWAVE_SOFTWARE)->arg2);
    run_for(MIN_MS); /* its removal follows (it said it was reset) */
    TEST_ASSERT_EQUAL_UINT16(0u, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(0u, W.p.net.hold_awake);
    TEST_ASSERT_FALSE(zw_fwupd_running(&W.p.fw));
}

/* ZW-033: a sleeping device nobody wakes: it ends after ZWZ_FW_WAKE_WAIT_MS, "not woken", and
 * its queued question is not left to surprise it at its next wake-up as an update. */
static void test_firmware_update_not_woken(void)
{
    static fake_zdev_t door;
    fw_device(&door, 0xF1A3u, true);
    door.m.auto_wake = false;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    run_for(40000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
    const ui_zw_progress_t g = fw_end(ZWZ_FW_WAKE_WAIT_MS + MIN_MS);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FWF_NOT_WOKEN, g.fw_why);
    TEST_ASSERT_EQUAL_UINT16(0u, W.p.net.hold_awake);
    /* woken later: whatever it hears, it takes no image and the panel asks nothing of it */
    fake_zdev_wake(&door, W.wire.now);
    run_for(30000u);
    TEST_ASSERT_FALSE(door.fw.active);
    TEST_ASSERT_EQUAL_UINT32(0u, door.fw.updates);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
}

/* ---- ZW-034, ZW-035: what devices switch -------------------------------------------------------- */

/* A mains, S2 device that switches: a light switch (Binary Switch at the device), a dimmer, or a
 * three-relay device (Binary Switch at end points 1-3, its root mirroring them). */
static void switch_device(fake_zdev_t *d, uint64_t seed, uint8_t cc, uint8_t relays)
{
    device(d, seed, false);
    fake_zdev_listening(d);
    d->m.notif_n = 0u;
    d->m.sw_report = true;
    d->m.secure_ccs[d->m.secure_n++] = cc;
    d->m.versions[d->m.versions_n++] = (fake_cc_version_t){cc, 1};
    if (relays > 0u) {
        d->m.secure_ccs[d->m.secure_n++] = 0x60u;
        d->m.versions[d->m.versions_n++] = (fake_cc_version_t){0x60u, 4};
        d->m.ep_n = relays;
        for (uint8_t e = 0u; e < relays; e++) {
            fake_ep_t *ep = &d->m.eps[e];
            ep->generic = 0x10u; /* binary switch */
            ep->specific = 0x01u;
            ep->ccs[0] = 0x5Eu;
            ep->cc_n = 1u;
            ep->secure_ccs[0] = 0x25u;
            ep->secure_n = 1u;
        }
    }
}

static ui_zw_device_page_t page_of(uint16_t node)
{
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, zwz_panel_generation(&W.p, node),
                                      &page));
    return page;
}

static ui_zw_req_result_t ask_output(uint8_t kind, uint16_t node, uint8_t ep, int16_t level,
                                     uint16_t zone, bool allowed)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.user = 1u;
    r.node = node;
    r.generation = zwz_panel_generation(&W.p, node);
    r.endpoint = ep;
    r.level = level;
    r.zone = zone;
    const ui_zw_req_result_t res = zwz_panel_request(&W.p, W.wire.now, &r, allowed);
    run_for(0u);
    return res;
}

/* ZW-034: a light switch and a dimmer - found, switched and confirmed, the same state when
 * someone presses the switch itself, a stale command refused, one the device never confirms
 * said so. */
static void test_switch_and_dimmer(void)
{
    static fake_zdev_t sw, dim;
    switch_device(&sw, 0x5B01u, 0x25u, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&sw, 0u));
    const uint16_t sn = progress().node;
    switch_device(&dim, 0x5B02u, 0x26u, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&dim, 0u));
    const uint16_t dn = progress().node;
    run_for(3000u);
    ui_zw_device_page_t page = page_of(sn);
    TEST_ASSERT_EQUAL_UINT8(1u, page.output_count);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_SWITCH, page.outputs[0].kind);
    TEST_ASSERT_EQUAL_INT8(0, page.outputs[0].level); /* asked where it is when it was ready */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_DIMMER, page_of(dn).outputs[0].kind);

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, sn, 0u, 100, 0u, false));
    TEST_ASSERT_TRUE(page_of(sn).outputs[0].busy);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask_output(UI_ZW_REQ_OUTPUT, sn, 0u, 0, 0u, false));
    run_for(3000u);
    page = page_of(sn);
    TEST_ASSERT_EQUAL_INT8(100, page.outputs[0].level);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_DONE, page.outputs[0].last);
    TEST_ASSERT_FALSE(page.outputs[0].busy);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, sw.m.sw[0]);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_output(UI_ZW_REQ_OUTPUT, sn, 0u, 50, 0u, false));

    /* pressed at the switch: the panel's state follows */
    fake_zdev_switch_press(&sw, W.wire.now, 0u, 0x25u, 0x00u);
    run_for(1000u);
    TEST_ASSERT_EQUAL_INT8(0, page_of(sn).outputs[0].level);

    /* the dimmer to 40 %, then full */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, dn, 0u, 40, 0u, false));
    run_for(3000u);
    TEST_ASSERT_EQUAL_INT8(40, page_of(dn).outputs[0].level);
    TEST_ASSERT_EQUAL_HEX8(40u, dim.m.sw[0]);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, dn, 0u, 100, 0u, false));
    run_for(3000u);
    TEST_ASSERT_EQUAL_INT8(100, page_of(dn).outputs[0].level);

    /* a command from long ago (a replayed or delayed one) switches nothing */
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_STALE,
                          zwz_panel_output_set(&W.p, W.wire.now, sn, zwz_panel_generation(&W.p, sn),
                                               0u, 100, W.wire.now - 60000u));
    run_for(2000u);
    TEST_ASSERT_EQUAL_HEX8(0x00u, sw.m.sw[0]);

    /* ZW-036: Home Assistant's view - one entity an output, keyed by network, device,
     * generation and end point; its commands switch the same output, a stale one nothing */
    proto_zw_device_t ha[8];
    size_t hn = zwz_panel_ha_devices(&W.p, W.wire.now, ha, 8u);
    TEST_ASSERT_EQUAL_UINT(2u, hn);
    char skey[PROTO_ZW_KEY_LEN];
    TEST_ASSERT_TRUE(proto_zw_key(W.p.net.host.facts.home_id, sn, zwz_panel_generation(&W.p, sn),
                                  0u, skey));
    size_t si = strcmp(ha[0].key, skey) == 0 ? 0u : 1u;
    TEST_ASSERT_EQUAL_STRING(skey, ha[si].key);
    TEST_ASSERT_EQUAL_UINT8(PROTO_ZW_SWITCH, ha[si].kind);
    TEST_ASSERT_EQUAL_UINT8(PROTO_ZW_LIGHT, ha[1u - si].kind);
    TEST_ASSERT_EQUAL_INT8(100, ha[1u - si].level);
    TEST_ASSERT_TRUE(ha[si].online);
    TEST_ASSERT_TRUE(zwz_panel_ha_command(&W.p, W.wire.now, skey, 100, W.wire.now));
    run_for(3000u);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, sw.m.sw[0]);
    hn = zwz_panel_ha_devices(&W.p, W.wire.now, ha, 8u);
    TEST_ASSERT_EQUAL_INT8(100, ha[si].level);
    TEST_ASSERT_FALSE(zwz_panel_ha_command(&W.p, W.wire.now, skey, 0, W.wire.now - 60000u));
    TEST_ASSERT_FALSE(zwz_panel_ha_command(&W.p, W.wire.now, "zw_00000000_9_9_9", 0,
                                           W.wire.now));
    run_for(2000u);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, sw.m.sw[0]);
    TEST_ASSERT_TRUE(zwz_panel_ha_command(&W.p, W.wire.now, skey, 0, W.wire.now));
    run_for(3000u);

    /* a switch that takes nothing: no answer, its level as it last said */
    sw.m.sw_ignore = true;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, sn, 0u, 100, 0u, false));
    run_for(ZWO_SET_MS + 1000u);
    page = page_of(sn);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_NO_ANSWER, page.outputs[0].last);
    TEST_ASSERT_EQUAL_INT8(0, page.outputs[0].level);

    /* PL02 (HCTL-012, AUT-006): a posted command's caller hears its end, not its start - with
     * its own request id, once: no answer is a timeout, a confirmed one done */
    const uint16_t sg = zwz_panel_generation(&W.p, sn);
    size_t slot = 99u;
    uint32_t req = 0u;
    zwo_result_t res = ZWO_OK;
    TEST_ASSERT_EQUAL_INT(-1, zwz_panel_output_wait(&W.p, sn, sg, 0u, 41u)); /* none under way */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwz_panel_output_set(&W.p, W.wire.now, sn, sg, 0u, 100,
                                                       W.wire.now));
    const int w41 = zwz_panel_output_wait(&W.p, sn, sg, 0u, 41u);
    TEST_ASSERT_TRUE(w41 >= 0);
    TEST_ASSERT_EQUAL_INT(-1, zwz_panel_output_wait(&W.p, sn, sg, 0u, 40u)); /* one caller */
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res)); /* started only */
    run_for(ZWO_SET_MS / 2u);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    run_for(ZWO_SET_MS);
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT((size_t)w41, slot);
    TEST_ASSERT_EQUAL_UINT32(41u, req);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_TIMEOUT, res);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res)); /* once */
    /* the next command: confirmed, and nothing older ends it */
    sw.m.sw_ignore = false;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwz_panel_output_set(&W.p, W.wire.now, sn, sg, 0u, 100,
                                                       W.wire.now));
    TEST_ASSERT_TRUE(zwz_panel_output_wait(&W.p, sn, sg, 0u, 42u) >= 0);
    run_for(3000u);
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT32(42u, req);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, res);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, sw.m.sw[0]);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    /* a device that hears nothing (no ACK): a failure, never "done" */
    sw.lose_in = 1000u;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwz_panel_output_set(&W.p, W.wire.now, sn, sg, 0u, 0,
                                                       W.wire.now));
    TEST_ASSERT_TRUE(zwz_panel_output_wait(&W.p, sn, sg, 0u, 43u) >= 0);
    run_for(ZWO_SET_MS + 2000u);
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT32(43u, req);
    TEST_ASSERT_TRUE(res == ZWO_ERR_SEND || res == ZWO_ERR_TIMEOUT);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    sw.lose_in = 0u;
}

/* ZW-034, ZW-035: a three-relay device and a tilt sensor make a garage door - its relays through
 * their end points; the garage set only in installer mode and kept across a restart; opening
 * only in installer mode, closing by anyone; never a press without a known position; the door
 * watched to its goal; logged; the relay never switched on its own; removal drops the garage. */
static void test_multi_relay_garage(void)
{
    static fake_zdev_t relay, tilt;
    switch_device(&relay, 0x5B03u, 0x25u, 3u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&relay, 0u));
    const uint16_t rn = progress().node;
    device(&tilt, 0x5B04u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&tilt, 0u));
    const uint16_t tn = progress().node;
    bind(12u, tn, ZSRC_ZW_SLOT_CONTACT);
    run_for(3000u);
    ui_zw_device_page_t page = page_of(rn);
    TEST_ASSERT_EQUAL_UINT8(3u, page.output_count);
    for (uint8_t i = 0u; i < 3u; i++) {
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_RELAY, page.outputs[i].kind);
        TEST_ASSERT_EQUAL_UINT8(i + 1u, page.outputs[i].endpoint);
    }
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, rn, 2u, 100, 0u, false));
    run_for(3000u);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, relay.m.sw[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, relay.m.sw[1]);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, rn, 2u, 0, 0u, false));
    run_for(3000u);

    /* relay 1 opens the garage; its tilt sensor is zone 12 */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED,
                          ask_output(UI_ZW_REQ_GARAGE, rn, 1u, 0, 12u, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE, rn, 1u, 0, 12u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask_output(UI_ZW_REQ_OUTPUT, rn, 1u, 100, 0u, true));
    page = page_of(rn);
    TEST_ASSERT_TRUE(page.outputs[0].garage);
    TEST_ASSERT_EQUAL_UINT16(12u, page.outputs[0].garage_zone);
    /* the tilt sensor has said nothing yet: no press */
    TEST_ASSERT_EQUAL_STRING("unknown", page.outputs[0].garage_state);
    const uint32_t sets = relay.m.sw_sets;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 100, 0u, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_NO_POSITION, page_of(rn).outputs[0].last);
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT32(sets, relay.m.sw_sets);

    /* closed, by its report */
    const uint8_t closed[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x17, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&tilt, W.wire.now, ZW_SEC_AUTO, closed,
                                                     sizeof closed));
    run_for(1000u);
    TEST_ASSERT_EQUAL_STRING("closed", page_of(rn).outputs[0].garage_state);
    /* opening takes installer mode; closing when closed presses nothing */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED,
                          ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 100, 0u, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 0, 0u, false));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_ALREADY, page_of(rn).outputs[0].last);
    TEST_ASSERT_EQUAL_UINT32(sets, relay.m.sw_sets);

    const size_t logs = logged_n(ALARM_EV_ZWAVE_GARAGE);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 100, 0u, true));
    /* PL02: its caller waits for the door, not the press */
    const uint16_t rg = zwz_panel_generation(&W.p, rn);
    TEST_ASSERT_TRUE(zwz_panel_output_wait(&W.p, rn, rg, 1u, 51u) >= 0);
    /* PL03: no clearing it while it moves - its relay's release would go with it */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask_output(UI_ZW_REQ_GARAGE, rn, 1u, 0, 0u, true));
    TEST_ASSERT_TRUE(page_of(rn).outputs[0].garage);
    /* the sensor's word only: closed, pressed to open */
    TEST_ASSERT_EQUAL_STRING("closed", page_of(rn).outputs[0].garage_state);
    TEST_ASSERT_EQUAL_UINT8(1u, page_of(rn).outputs[0].garage_goal);
    run_for(3000u); /* pressed and released */
    TEST_ASSERT_EQUAL_UINT32(sets + 2u, relay.m.sw_sets);
    TEST_ASSERT_EQUAL_HEX8(0x00u, relay.m.sw[1]);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask_output(UI_ZW_REQ_GARAGE, rn, 1u, 0, 0u, true));
    size_t slot = 99u;
    uint32_t req = 0u;
    zwo_result_t res = ZWO_ERR_UNKNOWN;
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res)); /* travelling */
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_GARAGE_OPEN_ASKED, logged(ALARM_EV_ZWAVE_GARAGE)->arg2);
    TEST_ASSERT_EQUAL_UINT8(1u, logged(ALARM_EV_ZWAVE_GARAGE)->user);
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK,
                          fake_zdev_report(&tilt, W.wire.now, ZW_SEC_AUTO, open, sizeof open));
    run_for(3000u);
    page = page_of(rn);
    TEST_ASSERT_EQUAL_STRING("open", page.outputs[0].garage_state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_DONE, page.outputs[0].last);
    TEST_ASSERT_EQUAL_UINT(logs + 2u, logged_n(ALARM_EV_ZWAVE_GARAGE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_GARAGE_OPENED, logged(ALARM_EV_ZWAVE_GARAGE)->arg2);
    TEST_ASSERT_EQUAL_UINT16(12u, logged(ALARM_EV_ZWAVE_GARAGE)->zone);
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT32(51u, req);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, res);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));

    /* it got there: closing may go at once (B015 UX-044); a press during that move is held
     * back, said on the page - not a busy radio */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 0, 0u, false));
    TEST_ASSERT_EQUAL_UINT8(2u, page_of(rn).outputs[0].garage_goal);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 0, 0u, false));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OUT_STILL_MOVING, page_of(rn).outputs[0].last);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&tilt, W.wire.now, ZW_SEC_AUTO, closed,
                                                     sizeof closed));
    run_for(3000u);
    TEST_ASSERT_EQUAL_STRING("closed", page_of(rn).outputs[0].garage_state);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_GARAGE_CLOSED, logged(ALARM_EV_ZWAVE_GARAGE)->arg2);

    /* PL02 (HCTL-012, AUT-009): a door that never gets there ends its caller's wait - once, as
     * NO_MOVE ("failed": pressed, and it didn't get there), logged failed after its "asked" */
    size_t asked = logged_n(ALARM_EV_ZWAVE_GARAGE);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 100, 0u, true));
    TEST_ASSERT_TRUE(zwz_panel_output_wait(&W.p, rn, rg, 1u, 52u) >= 0);
    run_for(3000u); /* pressed and released; its sensor never says open */
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    run_for(ZWO_TRAVEL_S_DEF * 1000u);
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT32(52u, req);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_NO_MOVE, res);
    TEST_ASSERT_EQUAL_STRING("failed", zwo_end_word(res));
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT(asked + 2u, logged_n(ALARM_EV_ZWAVE_GARAGE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_GARAGE_FAILED, logged(ALARM_EV_ZWAVE_GARAGE)->arg2);

    /* PL01 (review 2): a press whose on is lost on the air is released and ends unknown - its
     * "asked" entry followed by a failed one, never left without an end */
    asked = logged_n(ALARM_EV_ZWAVE_GARAGE);
    relay.lose_in = ZW_S2_TX_ATTEMPTS; /* the on: every transmission */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 100, 0u, true));
    TEST_ASSERT_TRUE(zwz_panel_output_wait(&W.p, rn, rg, 1u, 53u) >= 0);
    run_for(ZWO_SET_MS + 5000u);
    relay.lose_in = 0u;
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT32(53u, req);
    TEST_ASSERT_TRUE(res == ZWO_ERR_SEND || res == ZWO_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("timeout", zwo_end_word(res));
    TEST_ASSERT_EQUAL_UINT(asked + 2u, logged_n(ALARM_EV_ZWAVE_GARAGE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_GARAGE_FAILED, logged(ALARM_EV_ZWAVE_GARAGE)->arg2);
    TEST_ASSERT_EQUAL_HEX8(0x00u, relay.m.sw[1]);
    run_for(ZWO_TRAVEL_S_DEF * 1000u); /* it may have moved: a new press waits out its travel */

    /* ZW-036: Home Assistant sees a garage door (not relay 1), and two relays; it may close the
     * door, never open it */
    proto_zw_device_t ha[8];
    const size_t hn = zwz_panel_ha_devices(&W.p, W.wire.now, ha, 8u);
    TEST_ASSERT_EQUAL_UINT(3u, hn);
    size_t gi = hn;
    for (size_t i = 0; i < hn; i++) {
        if (ha[i].kind == PROTO_ZW_GARAGE) {
            gi = i;
        }
    }
    TEST_ASSERT_TRUE(gi < hn);
    TEST_ASSERT_EQUAL_STRING("Garage door", ha[gi].name);
    TEST_ASSERT_EQUAL_STRING("closed", ha[gi].garage);
    TEST_ASSERT_FALSE(zwz_panel_ha_command(&W.p, W.wire.now, ha[gi].key, 100, W.wire.now));

    /* the panel restarts: the garage is still there */
    boot();
    run_for(5000u);
    page = page_of(rn);
    TEST_ASSERT_TRUE(page.outputs[0].garage);
    TEST_ASSERT_EQUAL_UINT16(12u, page.outputs[0].garage_zone);

    /* PL02 (HCTL-012): its device dropped mid-move (what a removal does, zwo_forget): the
     * caller's wait ends once, unknown - nothing it can still follow */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&tilt, W.wire.now, ZW_SEC_AUTO, closed,
                                                     sizeof closed));
    run_for(1000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_GARAGE_MOVE, rn, 1u, 100, 0u, true));
    TEST_ASSERT_TRUE(zwz_panel_output_wait(&W.p, rn, rg, 1u, 54u) >= 0);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    const size_t forgot = logged_n(ALARM_EV_ZWAVE_GARAGE);
    zwo_forget(&W.p.outputs, rn);
    TEST_ASSERT_TRUE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT32(54u, req);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_UNKNOWN, res);
    TEST_ASSERT_EQUAL_STRING("timeout", zwo_end_word(res));
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res));
    TEST_ASSERT_EQUAL_UINT(forgot, logged_n(ALARM_EV_ZWAVE_GARAGE)); /* no zone left to log */
    run_for(3000u);
    TEST_ASSERT_FALSE(zwz_panel_output_ended(&W.p, &slot, &req, &res)); /* late answers: none */

    /* the relay device removed as failed: its garage goes (and is stored gone) */
    relay.silent = true;
    run_for(2u * MIN_MS);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_REMOVE_FAILED, rn, true));
    for (unsigned i = 0; i < 120u && progress().step != UI_ZW_STEP_DONE; i++) {
        run_for(1000u);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_REMOVED, progress().result);
    TEST_ASSERT_FALSE(W.p.outputs.garage[0].used);
    /* ZW-036: its entities are no longer listed (the MQTT client retracts what isn't) */
    TEST_ASSERT_EQUAL_UINT(0u, zwz_panel_ha_devices(&W.p, W.wire.now, ha, 8u));
    boot();
    run_for(2000u);
    TEST_ASSERT_FALSE(W.p.outputs.garage[0].used);
}

/* ZW-040: a device's signal on its row, from the RSSI of the frames it sends and of its ACKs.
 * UNKNOWN until measured; Good / Fair / Weak by the margin over the noise (the fake air's
 * -95 dBm) or, with none read, by dBm: the thresholds in zwz_panel.c. A frame from another
 * network or a broadcast tells nothing about this node's link. */
void zw_net_signal_forget(zw_net_t *net, uint16_t node); /* zw_net_internal.h: what a gone node does */
static void hear(fake_zdev_t *d, int8_t rssi, uint8_t status)
{
    static const uint8_t BASIC_REPORT[] = {0x20, 0x03, 0xFF};
    W.zac.rx_rssi = rssi;
    W.zac.rx_status = status;
    fake_zac93_from_device(&W.zac, W.wire.now, d, BASIC_REPORT, sizeof BASIC_REPORT);
    run_for(500u);
    W.zac.rx_status = 0u;
}

static void test_signal_from_received_frames(void)
{
    W.zac.rx_rssi = ZW_RSSI_NOT_AVAILABLE; /* nothing measured while it joins */
    W.zac.ack_rssi = ZW_RSSI_NOT_AVAILABLE;
    static fake_zdev_t lamp;
    device(&lamp, 0x5C01u, false);
    fake_zdev_listening(&lamp);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&lamp, 0u));
    run_for(30000u);
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_UNKNOWN, row_of(node)->signal);

    /* another network's frame and a broadcast: ignored, however strong */
    hear(&lamp, -40, 0x40u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_UNKNOWN, row_of(node)->signal);
    hear(&lamp, -40, 0x04u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_UNKNOWN, row_of(node)->signal);
    /* a reading that is not one (no signal) ... */
    hear(&lamp, ZW_RSSI_NO_SIGNAL, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_UNKNOWN, row_of(node)->signal);

    hear(&lamp, -50, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_GOOD, row_of(node)->signal);
    hear(&lamp, -82, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_FAIR, row_of(node)->signal);
    hear(&lamp, -98, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_WEAK, row_of(node)->signal);
    zw_net_signal_t s;
    TEST_ASSERT_TRUE(zw_net_node_signal(&W.p.net, node, &s));
    TEST_ASSERT_TRUE(s.has_rx);
    TEST_ASSERT_EQUAL_INT8(-98, s.rx_rssi);
    TEST_ASSERT_FALSE(s.has_tx);

    /* a removed device's readings go with it */
    zw_net_signal_forget(&W.p.net, node);
    TEST_ASSERT_FALSE(zw_net_node_signal(&W.p.net, node, &s));
}

static void test_signal_from_the_ack(void)
{
    W.zac.rx_rssi = ZW_RSSI_NOT_AVAILABLE;
    W.zac.ack_rssi = ZW_RSSI_NOT_AVAILABLE;
    static fake_zdev_t sw;
    switch_device(&sw, 0x5C02u, 0x25u, 0u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&sw, 0u));
    run_for(5000u);
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_UNKNOWN, row_of(node)->signal);

    /* no frame heard since: the ACK of the panel's command gives it, with its route */
    W.zac.ack_rssi = -52;
    W.zac.ack_repeaters = 1u;
    W.zac.ack_repeater_ids[0] = 9u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, node, 0u, 100, 0u, false));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_GOOD, row_of(node)->signal);
    zw_net_signal_t s;
    TEST_ASSERT_TRUE(zw_net_node_signal(&W.p.net, node, &s));
    TEST_ASSERT_TRUE(s.has_tx);
    TEST_ASSERT_FALSE(s.has_rx);
    TEST_ASSERT_EQUAL_UINT8(1u, s.tx_repeaters);
    TEST_ASSERT_EQUAL_UINT8(9u, s.tx_repeater_ids[0]);

    /* the newer reading wins: a weaker ACK now */
    W.zac.ack_rssi = -99;
    W.zac.ack_repeaters = 0u;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask_output(UI_ZW_REQ_OUTPUT, node, 0u, 0, 0u, false));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_SIGNAL_WEAK, row_of(node)->signal);
}

/* The zone's state while its device is read again: bound, heard, not CHECK. */
static void zone_steady(uint16_t zone, bool active)
{
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, zone, &info));
    TEST_ASSERT_FALSE(info.lost);
    TEST_ASSERT_TRUE(info.source_present);
    TEST_ASSERT_FALSE(info.check);
    TEST_ASSERT_FALSE(info.unreachable);
    TEST_ASSERT_TRUE(info.known);
    TEST_ASSERT_EQUAL(active, info.active);
}

/* ZW-010, ZW-029 (BL-152): "Set it up again" on a device whose setup is done reads it again from
 * step 1, through the same path as after a software update: installer mode only, refused while
 * another operation runs; the same node and generation; its zone
 * keeps its binding and its state, and never shows CHECK; a sleeper's waits for its wake-up. */
static void test_setup_again(void)
{
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    static fake_zdev_t door, sleepy;
    device(&door, 0x5E7Au, false);
    fake_zdev_listening(&door);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&door, 0u));
    const uint16_t node = progress().node;
    const uint16_t gen = zwz_panel_generation(&W.p, node);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(5u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(2000u);
    zone_steady(5u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_READY, row_of(node)->state);

    /* refused: not in installer mode, an unknown device, while another operation runs */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NOT_ALLOWED, ask(UI_ZW_REQ_SETUP, node, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, ask(UI_ZW_REQ_SETUP, 99u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_PING, node, false));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask(UI_ZW_REQ_SETUP, node, true));
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_ROUTES, 0u, true));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, ask(UI_ZW_REQ_SETUP, node, true));
    for (unsigned k = 0; k < 600u && progress().step != UI_ZW_STEP_DONE; k++) {
        run_for(1000u);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));

    /* read again: from step 1, the zone untouched all along */
    const unsigned version_gets = fake_zdev_asked(&door, 0x86u, 0x11u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_SETTING_UP, row_of(node)->state);
    bool ready = false;
    for (unsigned k = 0; k < 120u && !ready; k++) {
        run_for(500u);
        zone_steady(5u, true);
        ready = row_of(node)->state == UI_ZW_DEV_READY;
    }
    TEST_ASSERT_TRUE_MESSAGE(ready, "its reading did not end");
    TEST_ASSERT_TRUE(fake_zdev_asked(&door, 0x86u, 0x11u) > version_gets);
    TEST_ASSERT_EQUAL_UINT16(gen, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(5u, zwz_panel_zone_of(&W.p, node, gen));
    run_for(5000u);
    zone_steady(5u, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_OP_NONE, progress().op); /* no page's operation: the device page shows it */

    /* a sleeper's: asked, it waits for the device's next wake-up */
    device(&sleepy, 0x5E7Bu, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&sleepy, 0u));
    const uint16_t other = progress().node;
    const uint16_t other_gen = zwz_panel_generation(&W.p, other);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(6u, other, ZSRC_ZW_SLOT_CONTACT);
    run_for(40000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&sleepy, W.wire.now));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&sleepy, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(2000u);
    zone_steady(6u, true);
    const unsigned sleepy_gets = fake_zdev_asked(&sleepy, 0x86u, 0x11u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, other, true));
    for (unsigned k = 0; k < 60u; k++) {
        run_for(1000u);
        zone_steady(6u, true);
    }
    TEST_ASSERT_EQUAL_UINT(sleepy_gets, fake_zdev_asked(&sleepy, 0x86u, 0x11u)); /* not yet */
    TEST_ASSERT_NOT_EQUAL(UI_ZW_DEV_READY, row_of(other)->state);
    fake_zdev_wake(&sleepy, W.wire.now);
    ready = false;
    for (unsigned k = 0; k < 120u && !ready; k++) {
        run_for(500u);
        zone_steady(6u, true);
        ready = row_of(other)->state == UI_ZW_DEV_READY;
    }
    TEST_ASSERT_TRUE_MESSAGE(ready, "the sleeper's reading did not end at its wake-up");
    TEST_ASSERT_TRUE(fake_zdev_asked(&sleepy, 0x86u, 0x11u) > sleepy_gets);
    TEST_ASSERT_EQUAL_UINT16(other_gen, zwz_panel_generation(&W.p, other));
    TEST_ASSERT_EQUAL_UINT16(6u, zwz_panel_zone_of(&W.p, other, other_gen));
    zone_steady(5u, true);
}

/* ZW-028, ZW-031, BL-152: a keypad keeps arming, disarming and its sounds and lights WHILE it is
 * read again (Z-Wave JS's refreshInfo: the device keeps working) - its Entry Control is claimed,
 * the panel's state reaches it - and after the reading as before. */
static void test_setup_again_keypad_works_during_the_reading(void)
{
    static const zwk_port_t kport = {.command = engine_port};
    zwkc_init(&W.keyctl, &kport);
    zwz_panel_set_keyctl(&W.p, &W.keyctl);
    W.claim_on = true;
    zwf_init(&W.feedback);
    zwz_panel_set_feedback(&W.p, &W.feedback);
    static fake_zdev_t keypad;
    flirs_device(&keypad, 0xFEE5u);
    keypad.m.manufacturer = 0x0346; keypad.m.product_type = 0x0101; keypad.m.product_id = 0x0301;
    keypad.m.secure_ccs[keypad.m.secure_n++] = ZW_CC_INDICATOR;
    keypad.m.versions[keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_INDICATOR, 3};
    keypad.m.secure_ccs[keypad.m.secure_n++] = ZW_CC_ENTRY_CONTROL;
    keypad.m.versions[keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_ENTRY_CONTROL, 1};
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&keypad, ZW_S2_KEY_ACCESS));
    const uint16_t node = progress().node;
    const uint16_t gen = progress().generation;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    const size_t slot = zwf_slot_for(&W.feedback, node);
    TEST_ASSERT_TRUE(zwf_assign(&W.feedback, slot, &W.p.net, node, 1u));
    TEST_ASSERT_TRUE(zwkc_assign(&W.keyctl, 0, &W.p.net, node, 1, 0));
    zwf_view_t view = {.partitions = 1, .taken_ms = W.wire.now, .part = {{2, 0}}};
    zwf_snapshot(&W.feedback, &view);
    W.feedback_enabled = true;
    run_for(2000u);
    ui_zw_device_page_t page;
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_TRUE(page.feedback.live);
    TEST_ASSERT_TRUE(page.control.live);

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    keypad.m.answers = false; /* it keeps the reading going: questions unanswered, its own
                               * commands still sent */
    run_for(500u);
    TEST_ASSERT_TRUE(zw_iv_running(&W.p.net.iv, node));
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_TRUE_MESSAGE(page.feedback.live, "its sounds and lights stopped during the reading");
    TEST_ASSERT_TRUE_MESSAGE(page.control.live, "its arming stopped during the reading");
    /* it arms: claimed and run as before */
    const zw_cc_entry_notification_t n = {1u, ZW_ENTRY_DATA_ASCII, ZW_ENTRY_EVENT_ARM_AWAY,
                                          (const uint8_t *)"1234", 4u};
    uint8_t cmd[40];
    const size_t len = zw_cc_entry_notification_build(cmd, sizeof cmd, &n);
    const unsigned claims = W.claims;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&keypad, W.wire.now, ZW_SEC_AUTO, cmd, len));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(claims + 1u, W.claims, "its arming was not claimed during the reading");
    zwk_reply_t reply;
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, zwkc_run(&W.keyctl, 0, W.p.net.keys_home_id, gen,
                                                &W.claimed, W.wire.now, &reply));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, W.engine_cmd.type);
    /* the panel's new state reaches it */
    keypad.got_n = 0u;
    view.taken_ms = W.wire.now;
    view.part[0].indicator = (uint8_t)(view.part[0].indicator + 1u);
    zwf_snapshot(&W.feedback, &view);
    bool heard = false;
    for (unsigned k = 0; k < 20u && !heard; k++) {
        run_for(500u);
        size_t m = 0u;
        heard = got_cmd(&keypad, 0x87, 0x01, &m) != NULL;
    }
    TEST_ASSERT_TRUE_MESSAGE(heard, "the keypad was not told the panel's state during its reading");
    TEST_ASSERT_TRUE(zw_iv_running(&W.p.net.iv, node));

    /* it answers again: the reading ends READY, and the panel's state still reaches it */
    keypad.m.answers = true;
    bool ready = false;
    for (unsigned k = 0; k < 1200u && !ready; k++) {
        run_for(500u);
        if (!zw_iv_running(&W.p.net.iv, node) && row_of(node)->state != UI_ZW_DEV_READY) {
            TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true)); /* it stopped */
        }
        ready = !zw_iv_running(&W.p.net.iv, node) && row_of(node)->state == UI_ZW_DEV_READY;
    }
    TEST_ASSERT_TRUE_MESSAGE(ready, "its reading did not end");
    keypad.got_n = 0u;
    view.taken_ms = W.wire.now;
    view.part[0].indicator = (uint8_t)(view.part[0].indicator + 1u);
    zwf_snapshot(&W.feedback, &view);
    heard = false;
    for (unsigned k = 0; k < 20u && !heard; k++) {
        run_for(500u);
        size_t m = 0u;
        heard = got_cmd(&keypad, 0x87, 0x01, &m) != NULL;
    }
    TEST_ASSERT_TRUE_MESSAGE(heard, "the keypad was not told the panel's state after its reading");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_READY, row_of(node)->state);
    run_for(2000u);
    TEST_ASSERT_TRUE(zwz_panel_device(&W.p, W.wire.now, node, gen, &page));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FB_AVAILABLE, page.feedback.fit);
    TEST_ASSERT_EQUAL_UINT8(1u, page.feedback.partition);
    TEST_ASSERT_TRUE(page.feedback.live);
    TEST_ASSERT_TRUE(page.control.live);
}

/* A listening door on zone `zone`, open. */
static uint16_t door_on_zone(fake_zdev_t *door, uint64_t seed, uint16_t zone)
{
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    device(door, seed, false);
    fake_zdev_listening(door);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(door, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(zone, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(door, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(2000u);
    zone_steady(zone, true);
    return node;
}

static void door_says(fake_zdev_t *door, bool open)
{
    const uint8_t r[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, open ? 0x16 : 0x17, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(door, W.wire.now, ZW_SEC_AUTO, r, sizeof r));
    run_for(1000u);
}

/* The zone source offers the device's contact (its zone is watched, supervised). */
static void zone_watched(uint16_t node)
{
    const zsrc_zw_device_t *d = zsrc_zw_find(&W.src, node);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZSRC_ZW_INPUTS_OFFERED, d->inputs[0], "its inputs are no longer offered");
    TEST_ASSERT_TRUE_MESSAGE((d->offered[0] & (1u << ZSRC_ZW_SLOT_CONTACT)) != 0u,
                             "its zone is no longer supervised");
}

/* ZW-010, ZW-029, BL-152: while a finished device is read again it keeps working on the facts it
 * had (Z-Wave JS's refreshInfo): a report reaches its zone; a link up (each network check) leaves
 * its zone READY and supervised, never CHECK; a reading that stops leaves it working on its old
 * facts, the page saying the re-read stopped (SETUP_STOPPED with all steps done); set up again,
 * it ends READY. */
static void test_setup_again_keeps_the_device_working(void)
{
    static fake_zdev_t door;
    const uint16_t node = door_on_zone(&door, 0x5E7Cu, 5u);
    zone_watched(node);

    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    door.m.answers = false; /* the reading goes on: its questions wait */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_SETTING_UP, row_of(node)->state);
    TEST_ASSERT_EQUAL_UINT8(row_of(node)->setup_total, row_of(node)->setup_done); /* a re-read */
    /* a report mid-reading reaches the zone */
    door_says(&door, false);
    zone_steady(5u, false);
    TEST_ASSERT_TRUE(zw_iv_running(&W.p.net.iv, node));

    /* the radio goes and comes back (a network check): the zone stays READY and supervised */
    door.m.answers = true;
    const uint32_t downs = zw_host_stats(&W.p.net.host)->link_downs;
    W.zac.fault.dead = true;
    run_for(3u * MIN_MS);
    TEST_ASSERT_TRUE(zw_host_stats(&W.p.net.host)->link_downs > downs);
    door.m.answers = false;
    W.zac.fault.dead = false;
    for (unsigned k = 0; k < 600u && zw_host_link(&W.p.net.host) != ZW_LINK_UP; k++) {
        run_for(1000u);
    }
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&W.p.net.host));
    run_for(5000u);
    TEST_ASSERT_TRUE(zw_iv_running(&W.p.net.iv, node));
    zone_steady(5u, false);
    zone_watched(node);
    door_says(&door, true);
    zone_steady(5u, true);

    /* it answers nothing more: the reading stops, the device works on as before */
    for (unsigned k = 0; k < 1800u && zw_iv_running(&W.p.net.iv, node); k++) {
        run_for(1000u);
        zone_steady(5u, true);
    }
    TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, node));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UI_ZW_DEV_SETUP_STOPPED, row_of(node)->state,
                                    "the page does not say the re-read stopped");
    TEST_ASSERT_EQUAL_UINT8(row_of(node)->setup_total, row_of(node)->setup_done);
    door.m.answers = true;
    door_says(&door, false);
    zone_steady(5u, false);
    zone_watched(node);

    /* set up again: it ends READY, and the stopped re-read is forgotten */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    bool ready = false;
    for (unsigned k = 0; k < 240u && !ready; k++) {
        run_for(500u);
        zone_steady(5u, false);
        ready = !zw_iv_running(&W.p.net.iv, node) && row_of(node)->state == UI_ZW_DEV_READY;
    }
    TEST_ASSERT_TRUE_MESSAGE(ready, "its reading did not end");
}

/* ZW-017, ZW-029, BL-152: a panel restart in the middle of a re-read drops the reading (it lives
 * in RAM only): the stored record keeps its finished setup, the device is READY on its old facts
 * and its zone READY, hearing it. */
static void test_setup_again_panel_restart(void)
{
    static fake_zdev_t door;
    const uint16_t node = door_on_zone(&door, 0x5E7Du, 5u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    door.m.answers = false;
    run_for(3000u); /* some of its steps answered, stored */
    TEST_ASSERT_TRUE(zw_iv_running(&W.p.net.iv, node));
    boot();
    door.m.answers = true;
    run_for(5000u);
    TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, node));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_READY, row_of(node)->state);
    TEST_ASSERT_EQUAL_UINT8(row_of(node)->setup_total, row_of(node)->setup_done);
    zone_watched(node); /* its zone's state is heard afresh after a restart: */
    door_says(&door, false);
    zone_steady(5u, false);
}

/* ZW-029, BL-152: a sleeper's re-read waits for its wake-up as a calm "read again at its next
 * wake-up": WAKE_NEEDED with all its steps done (the pages' sign of a re-read), not a setup that
 * needs it woken; its zone keeps working meanwhile. */
static void test_setup_again_sleeper_waits_calmly(void)
{
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    static fake_zdev_t sleepy;
    device(&sleepy, 0x5E7Eu, true);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&sleepy, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    bind(6u, node, ZSRC_ZW_SLOT_CONTACT);
    run_for(40000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    run_for(2000u);
    const ui_zw_device_t *r = row_of(node);
    /* pinned: the calm wake-up state, which ui_zwave.c words as "Read again at its next wake-up" */
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_WAKE_NEEDED, r->state);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r->setup_total, r->setup_done, "not shown as a re-read");
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&sleepy, W.wire.now, ZW_SEC_AUTO, OPEN, sizeof OPEN));
    run_for(2000u);
    zone_steady(6u, true);
}

/* BL-152: end point `ep` of a door reports its contact (Multi Channel encapsulated). */
static void endpoint_door_says(fake_zdev_t *d, uint8_t ep, bool open)
{
    const uint8_t report[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06,
                              (uint8_t)(open ? 0x16u : 0x17u), 0x00};
    uint8_t mc[24];
    const size_t n = zw_cc_wrap_multi_channel(mc, sizeof mc, ep, 0u, false, report, sizeof report);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(d, W.wire.now, ZW_SEC_AUTO, mc, n));
    run_for(2000u);
}

/* BL-152: a listening door with two Multi Channel end points, each its own contact (Notification
 * access control); its end point `zone_ep`'s contact on zone `zone`, open. */
static uint16_t two_endpoint_door(fake_zdev_t *d, uint64_t seed, uint16_t zone, uint8_t zone_ep)
{
    device(d, seed, false);
    fake_zdev_listening(d);
    d->m.secure_ccs[d->m.secure_n++] = 0x60u;
    d->m.versions[d->m.versions_n++] = (fake_cc_version_t){0x60u, 4};
    d->m.ep_n = 2u;
    for (uint8_t e = 0u; e < 2u; e++) {
        fake_ep_t *ep = &d->m.eps[e];
        ep->generic = 0x07u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x71u;
        ep->secure_n = 1u;
        ep->notif_types[0] = 0x06u;
        ep->notif_n = 1u;
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(d, 0u));
    const uint16_t node = progress().node;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
    const ui_zw_device_page_t page = page_of(node);
    bool bound = false;
    for (size_t i = 0; i < page.input_count && !bound; i++) {
        if (page.inputs[i].slot == ZSRC_ZW_SLOT_CONTACT && page.inputs[i].endpoint == zone_ep) {
            zone_hub_binding_t b;
            memset(&b, 0, sizeof b);
            b.in[ZSRC_ROLE_MAIN].addr = page.inputs[i].addr;
            zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map,
                                  (alarm_device_class_t)page.inputs[i].device_class);
            TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&W.hub, zone, &b, W.wire.now));
            bound = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bound, "no contact at that end point");
    run_for(5000u);
    endpoint_door_says(d, zone_ep, true);
    zone_steady(zone, true);
    return node;
}

/* Its end point's contact offered by the zone source (its zone is watched, supervised). */
static void endpoint_watched(uint16_t node, uint8_t ep)
{
    const zsrc_zw_device_t *d = zsrc_zw_find(&W.src, node);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZSRC_ZW_INPUTS_OFFERED, d->inputs[ep],
                                    "its end point's inputs are no longer offered");
    TEST_ASSERT_TRUE_MESSAGE((d->offered[ep] & (1u << ZSRC_ZW_SLOT_CONTACT)) != 0u,
                             "its end point's zone is no longer supervised");
}

/* ZW-010, ZW-013, ZW-017, ZW-029 (BL-152, the reviewers' proof): a re-read of a two-end-point
 * door whose zone is on end point 2 stalls at step 11 (its Notification Supported Get goes
 * unanswered): the node cache is not stored while it reads, the device row says the re-read
 * stopped, and its end point keeps its facts - after a panel restart too: end point 2's contact
 * still offered, its zone READY, and never CHECK over the next 75 minutes. */
static void test_setup_again_stall_keeps_endpoint_facts(void)
{
    static fake_zdev_t door;
    const uint16_t node = two_endpoint_door(&door, 0x5E80u, 7u, 2u);
    endpoint_watched(node, 2u);
    const zw_node_record_t *rec = zw_node_find_const(&W.p.net.nodes, node);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[2].notification_count);
    const unsigned writes = W.node_writes;
    uint8_t before[sizeof W.nodes];
    const size_t before_len = W.nodes_len;
    memcpy(before, W.nodes, before_len);

    door.m.mute_cc = 0x71u; /* Notification Supported Get: step 11 never ends */
    door.m.mute_cmd = 0x07u;
    door.m.mute_n = FAKE_MUTE_ALWAYS;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    bool past_endpoints = false;
    for (unsigned k = 0; k < 1800u && zw_iv_running(&W.p.net.iv, node); k++) {
        run_for(1000u);
        past_endpoints = past_endpoints ||
                         zw_iv_step(&W.p.net.iv, node) >= ZW_IV_STEP_NOTIFICATION;
        zone_steady(7u, true);
        endpoint_watched(node, 2u);
    }
    TEST_ASSERT_TRUE_MESSAGE(past_endpoints, "the re-read never reached step 11");
    TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, node));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UI_ZW_DEV_SETUP_STOPPED, row_of(node)->state,
                                    "the page does not say the re-read stopped");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(writes, W.node_writes, "stored while it was read again");
    TEST_ASSERT_EQUAL_UINT(before_len, W.nodes_len);
    TEST_ASSERT_EQUAL_MEMORY(before, W.nodes, before_len);
    rec = zw_node_find_const(&W.p.net.nodes, node);
    TEST_ASSERT_TRUE(rec->endpoints[2].used);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[2].notification_count);

    /* the panel restarts: the old, complete facts, the zone READY; the re-read is forgotten */
    door.m.mute_n = 0u;
    boot();
    run_for(5000u);
    TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, node));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_DEV_READY, row_of(node)->state);
    endpoint_watched(node, 2u);
    endpoint_door_says(&door, 2u, false);
    zone_steady(7u, false);
    for (unsigned k = 0; k < 75u; k++) {
        run_for(MIN_MS);
        zone_steady(7u, false);
    }
    endpoint_watched(node, 2u);
}

/* ZW-010, ZW-013, ZW-029 (BL-152): a re-read that stalls inside step 8 (its Multi Channel
 * Capability Get goes unanswered) changes nothing the zones use: through a link down and up (a
 * network check), end point 2's contact stays offered and its zone READY; nothing is stored. */
static void test_setup_again_stall_in_endpoints_then_link_up(void)
{
    static fake_zdev_t door;
    const uint16_t node = two_endpoint_door(&door, 0x5E81u, 7u, 2u);
    const unsigned writes = W.node_writes;
    door.m.mute_cc = 0x60u; /* Multi Channel Capability Get */
    door.m.mute_cmd = 0x09u;
    door.m.mute_n = FAKE_MUTE_ALWAYS;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    for (unsigned k = 0; k < 120u && zw_iv_step(&W.p.net.iv, node) != ZW_IV_STEP_ENDPOINTS; k++) {
        run_for(250u);
    }
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_ENDPOINTS, zw_iv_step(&W.p.net.iv, node));
    run_for(3000u); /* its end point report came: the old record would have lost them here */
    TEST_ASSERT_TRUE(zw_iv_refreshing(&W.p.net.iv, node));
    endpoint_watched(node, 2u);

    W.zac.fault.dead = true;
    run_for(3u * MIN_MS);
    W.zac.fault.dead = false;
    for (unsigned k = 0; k < 600u && zw_host_link(&W.p.net.host) != ZW_LINK_UP; k++) {
        run_for(1000u);
    }
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&W.p.net.host));
    run_for(5000u);
    endpoint_watched(node, 2u);
    zone_steady(7u, true);
    for (unsigned k = 0; k < 1800u && zw_iv_running(&W.p.net.iv, node); k++) {
        run_for(1000u);
    }
    TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, node));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(writes, W.node_writes, "stored while it was read again");
    endpoint_watched(node, 2u);
    zone_steady(7u, true);
    const zw_node_record_t *rec = zw_node_find_const(&W.p.net.nodes, node);
    TEST_ASSERT_TRUE(rec->endpoints[2].used);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 2u));
}

/* ZW-010, ZW-029 (BL-152): what the re-read does not ask is kept across it - the device's zone
 * binding and generation, its security classes and DSK - and the console's `zwave interview <n>`
 * (zwz_panel_interview, the core call it reaches) or "Set it up again" while a re-read runs starts
 * it again from step 1, as Z-Wave JS's refreshInfo restarts a running interview. */
static void test_setup_again_keeps_what_it_does_not_read(void)
{
    static fake_zdev_t door;
    const uint16_t node = two_endpoint_door(&door, 0x5E82u, 7u, 2u);
    const uint16_t gen = zwz_panel_generation(&W.p, node);
    const zw_node_record_t old = *zw_node_find_const(&W.p.net.nodes, node);
    TEST_ASSERT_NOT_EQUAL(0u, old.security_classes);
    const unsigned writes = W.node_writes;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true));
    for (unsigned k = 0; k < 200u && zw_iv_step(&W.p.net.iv, node) < ZW_IV_STEP_IDENTITY; k++) {
        run_for(50u);
    }
    const uint8_t step = zw_iv_step(&W.p.net.iv, node);
    TEST_ASSERT_TRUE(step >= ZW_IV_STEP_IDENTITY);
    const uint32_t started = zw_iv_stats(&W.p.net.iv)->started;
    TEST_ASSERT_TRUE(zwz_panel_interview(&W.p, W.wire.now, node)); /* restarted, as Z-Wave JS */
    TEST_ASSERT_TRUE(zw_iv_step(&W.p.net.iv, node) < ZW_IV_STEP_IDENTITY); /* step 1 again */
    TEST_ASSERT_EQUAL_UINT32(started + 1u, zw_iv_stats(&W.p.net.iv)->started);
    for (unsigned k = 0; k < 200u && zw_iv_step(&W.p.net.iv, node) < ZW_IV_STEP_IDENTITY; k++) {
        run_for(50u);
    }
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, node, true)); /* the same */
    TEST_ASSERT_TRUE(zw_iv_step(&W.p.net.iv, node) < ZW_IV_STEP_IDENTITY);
    TEST_ASSERT_EQUAL_UINT32(started + 2u, zw_iv_stats(&W.p.net.iv)->started);
    bool ready = false;
    for (unsigned k = 0; k < 240u && !ready; k++) {
        run_for(500u);
        zone_steady(7u, true);
        ready = !zw_iv_running(&W.p.net.iv, node) && row_of(node)->state == UI_ZW_DEV_READY;
    }
    TEST_ASSERT_TRUE_MESSAGE(ready, "its reading did not end");
    TEST_ASSERT_EQUAL_UINT(writes + 1u, W.node_writes); /* stored once, at READY */
    const zw_node_record_t *rec = zw_node_find_const(&W.p.net.nodes, node);
    TEST_ASSERT_EQUAL_UINT16(gen, zwz_panel_generation(&W.p, node));
    TEST_ASSERT_EQUAL_UINT16(7u, zwz_panel_zone_of(&W.p, node, gen));
    TEST_ASSERT_EQUAL_UINT8(old.security_classes, rec->security_classes);
    TEST_ASSERT_EQUAL(old.has_dsk, rec->has_dsk);
    TEST_ASSERT_EQUAL_MEMORY(old.dsk, rec->dsk, sizeof old.dsk);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, rec->interview_step);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[2].notification_count);
    endpoint_watched(node, 2u);
}

/* ZW-033 (BL-152): after a successful update its setup is read again when Z-Wave JS's
 * onNodeFirmwareUpdated would: the time the device said its restart takes, plus Z-Wave JS's margin
 * (getConservativeWaitTimeAfterFirmwareUpdate: none said 5 s, under 20 s +5, under 60 s +10, else
 * +30) - not before, and then at once. */
static void test_update_reread_after_conservative_wait(void)
{
    static fake_zdev_t sw;
    fw_device(&sw, 0xF1A6u, false);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(&sw, 0u));
    const uint16_t node = progress().node;
    run_for(5000u);
    static const struct {
        uint16_t said_s;
        uint32_t ms;
    } cases[] = {{0u, 5000u}, {10u, 15000u}, {30u, 40000u}, {100u, 130000u}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sw.fw.wait_s = cases[i].said_s;
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, fw_start(node, true));
        const ui_zw_progress_t g = fw_end(5u * MIN_MS);
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_FW_UPDATED, g.result);
        TEST_ASSERT_EQUAL_UINT16(node, W.p.fw_reinterview);
        /* it ended within the last 100 ms step of fw_end */
        const uint64_t due = W.wire.now + cases[i].ms;
        TEST_ASSERT_TRUE_MESSAGE(W.p.fw_reinterview_at <= due && W.p.fw_reinterview_at + 100u > due,
                                 "not Z-Wave JS's wait");
        run_for((uint32_t)(W.p.fw_reinterview_at - W.wire.now - 200u));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(node, W.p.fw_reinterview, "read again too early");
        TEST_ASSERT_FALSE(zw_iv_refreshing(&W.p.net.iv, node));
        run_for(400u);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, W.p.fw_reinterview, "not read again when due");
        for (unsigned k = 0; k < 120u && zw_iv_running(&W.p.net.iv, node); k++) {
            run_for(500u);
        }
        TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, node));
        TEST_ASSERT_FALSE(zw_iv_reread_stopped(&W.p.net.iv, node));
    }
}

/* ZW-029, ZW-033 (BL-152): the re-read a software update asks for is never dropped, as Z-Wave JS
 * always re-interviews after such an update: refused while both scratch records are in use by
 * other devices' re-reads, it is tried again, and runs once one is free. */
static void test_update_reread_waits_for_a_scratch_record(void)
{
    static fake_zdev_t a, b, c;
    fake_zdev_t *d[3] = {&a, &b, &c};
    uint16_t n[3];
    for (size_t i = 0; i < 3u; i++) {
        device(d[i], 0x5E90u + i, false);
        fake_zdev_listening(d[i]);
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(d[i], 0u));
        n[i] = progress().node;
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
        run_for(5000u);
    }
    for (size_t i = 0; i < 2u; i++) {
        d[i]->m.mute_cc = 0x72u; /* who it is: these readings stop */
        d[i]->m.mute_n = FAKE_MUTE_ALWAYS;
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_SETUP, n[i], true));
    }
    /* its update is done; its re-read is due now */
    W.p.fw_reinterview = n[2];
    W.p.fw_reinterview_gen = zwz_panel_generation(&W.p, n[2]);
    W.p.fw_reinterview_at = W.wire.now;
    run_for(1000u);
    TEST_ASSERT_FALSE(zw_iv_refreshing(&W.p.net.iv, n[2])); /* busy */
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(n[2], W.p.fw_reinterview, "its re-read was dropped");
    bool started = false;
    for (unsigned k = 0; k < 600u && !started; k++) {
        run_for(1000u);
        started = zw_iv_refreshing(&W.p.net.iv, n[2]);
    }
    TEST_ASSERT_TRUE_MESSAGE(started, "its re-read never ran");
    TEST_ASSERT_TRUE(zw_iv_reread_stopped(&W.p.net.iv, n[0]));
    TEST_ASSERT_EQUAL_UINT16(0u, W.p.fw_reinterview);
    for (unsigned k = 0; k < 120u && zw_iv_running(&W.p.net.iv, n[2]); k++) {
        run_for(500u);
    }
    TEST_ASSERT_FALSE(zw_iv_running(&W.p.net.iv, n[2]));
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(&W.p.net.iv, n[2]));
    TEST_ASSERT_EQUAL_INT(UI_ZW_DEV_READY, row_of(n[2])->state);
}

/* ZW-029 (BL-152): `zwave interview <n>` says why it was refused (zwz_panel_interview_result,
 * the core call the console reaches): busy for a known device whose re-read would start while both
 * scratch records are in use, not that device for one the panel does not know, and no radio while
 * the panel's Z-Wave is not started. */
static void test_interview_refusal_reasons(void)
{
    static fake_zdev_t a, b, c;
    fake_zdev_t *d[3] = {&a, &b, &c};
    uint16_t n[3];
    for (size_t i = 0; i < 3u; i++) {
        device(d[i], 0x5EA0u + i, false);
        fake_zdev_listening(d[i]);
        TEST_ASSERT_EQUAL_UINT8(UI_ZW_RES_ADDED, include(d[i], 0u));
        n[i] = progress().node;
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, ask(UI_ZW_REQ_DISMISS, 0u, true));
        run_for(5000u);
    }
    for (size_t i = 0; i < 2u; i++) {
        d[i]->m.mute_cc = 0x72u; /* who it is: these readings hold their scratch records */
        d[i]->m.mute_n = FAKE_MUTE_ALWAYS;
        TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_interview_result(&W.p, W.wire.now, n[i]));
    }
    run_for(1000u);
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_BUSY, zwz_panel_interview_result(&W.p, W.wire.now, n[2]));
    TEST_ASSERT_FALSE(zwz_panel_interview(&W.p, W.wire.now, n[2]));
    TEST_ASSERT_FALSE(zw_iv_refreshing(&W.p.net.iv, n[2]));
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_INVALID, zwz_panel_interview_result(&W.p, W.wire.now, 200u));
    W.p.started = false;
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_NO_RADIO,
                          zwz_panel_interview_result(&W.p, W.wire.now, n[2]));
    W.p.started = true;
    /* a running re-read asked again starts from step 1 (Z-Wave JS's refreshInfo): ok */
    TEST_ASSERT_EQUAL_INT(UI_ZW_REQ_OK, zwz_panel_interview_result(&W.p, W.wire.now, n[0]));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_start);
    RUN_TEST(test_signal_from_received_frames);
    RUN_TEST(test_signal_from_the_ack);
    RUN_TEST(test_add_secure_device);
    RUN_TEST(test_add_plain_device);
    RUN_TEST(test_remove_device);
    RUN_TEST(test_device_reset_locally);
    RUN_TEST(test_check_in);
    RUN_TEST(test_check_in_not_taken);
    RUN_TEST(test_check_in_after_restart);
    RUN_TEST(test_region_restart_and_reset);
    RUN_TEST(test_restarts_and_the_radio);
    RUN_TEST(test_radio_restart_ends_the_windows);
    RUN_TEST(test_restart_loop_logged_once_an_hour);
    RUN_TEST(test_many_devices_left_at_once);
    RUN_TEST(test_stored_formats);
    RUN_TEST(test_power_cap);
    RUN_TEST(test_radio_update);
    RUN_TEST(test_feedback_over_secure_radio);
    RUN_TEST(test_empty_node_information_still_secured);
    RUN_TEST(test_partial_join_setup_stops_and_is_cleaned_up);
    RUN_TEST(test_remove_failed_outcomes);
    RUN_TEST(test_pin_range);
    RUN_TEST(test_feedback_on_the_device_page);
    RUN_TEST(test_keypad_control_end_to_end);
    RUN_TEST(test_keypad_supervised_unreachable);
    RUN_TEST(test_keypad_supervision_survives_radio_restart);
    RUN_TEST(test_keypad_unassigned_not_supervised);
    RUN_TEST(test_keypad_supervision_restored_when_off_or_removed);
    RUN_TEST(test_pulled_radio_found_when_idle);
    RUN_TEST(test_pulled_radio_found_by_idle_probe);
    RUN_TEST(test_smartstart_model_name);
    RUN_TEST(test_smartstart_on_the_panel);
    RUN_TEST(test_time_answers);
    RUN_TEST(test_device_jamming_report);
    RUN_TEST(test_identify);
    RUN_TEST(test_setup_again);
    RUN_TEST(test_setup_again_keypad_works_during_the_reading);
    RUN_TEST(test_setup_again_keeps_the_device_working);
    RUN_TEST(test_setup_again_panel_restart);
    RUN_TEST(test_setup_again_sleeper_waits_calmly);
    RUN_TEST(test_setup_again_stall_keeps_endpoint_facts);
    RUN_TEST(test_setup_again_stall_in_endpoints_then_link_up);
    RUN_TEST(test_setup_again_keeps_what_it_does_not_read);
    RUN_TEST(test_update_reread_after_conservative_wait);
    RUN_TEST(test_update_reread_waits_for_a_scratch_record);
    RUN_TEST(test_interview_refusal_reasons);
    RUN_TEST(test_jamming);
    RUN_TEST(test_jam_reports_flow);
    RUN_TEST(test_jam_survives_a_module_restart);
    RUN_TEST(test_jam_then_a_real_link_down);
    RUN_TEST(test_rebuild_routes);
    RUN_TEST(test_replace_on_the_panel);
    RUN_TEST(test_replace_refused_on_the_panel);
    RUN_TEST(test_firmware_update_sleeping_device);
    RUN_TEST(test_firmware_update_outcomes);
    RUN_TEST(test_firmware_update_not_woken);
    RUN_TEST(test_switch_and_dimmer);
    RUN_TEST(test_multi_relay_garage);
    RUN_TEST(test_nvs_reserve_add_and_replace);
    RUN_TEST(test_nvs_need_grows_with_the_cache);
    RUN_TEST(test_nvs_reserve_smartstart);
    RUN_TEST(test_identity_refused_counter_write);
    RUN_TEST(test_identity_refused_cache_write);
    RUN_TEST(test_identity_add_refused_counter_write);
    RUN_TEST(test_identity_add_refused_cache_write);
    RUN_TEST(test_identity_waits_for_due_write);
    RUN_TEST(test_generations_used_up);
    RUN_TEST(test_identity_failed_write_holds_removal);
    return UNITY_END();
}
