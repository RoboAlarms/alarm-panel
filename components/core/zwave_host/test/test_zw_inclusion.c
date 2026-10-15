/*
 * test_zw_inclusion.c - adding a device end to end (docs/features/36 network.md §4, §4.1,
 * security.md §5): zw_net (the host, the security manager, the inclusion) against the fake
 * ZAC93 and a simulated door sensor with a real joining-side S2, on a fake clock. Then the sensor
 * at work: its reports at its highest class, a clear "door closed" dropped, resyncs after either
 * side restarts, lost, repeated and crossed frames.
 *
 * Requirements: ZW-005 (one device per 60 s window, the protocol's steps, timeouts and refusals),
 * ZW-006 (the key exchange with consent and PIN, the timers, every failure insecure and said so,
 * downgrades detected), ZW-007 and ZW-009 end to end, ZW-015 (a device added without security has
 * no class, whatever its NIF lists), ZW-023 (a truncated command dropped and counted), ZW-037 (a
 * frame the radio did not deliver sent again, 3 transmissions in all, the key exchange's too),
 * ZW-004 with ZW-005 (the radio restarting - by itself or by serial-api.md §7's soft reset - ends
 * an add or remove window at once; after the device joined, its key exchange carries on).
 */
#include <string.h>

#include "fake_zac93.h"
#include "fake_zdev.h"
#include "unity.h"
#include "zw_host_crypto.h"
#include "zwave_host/zw_net.h"
#include "zwave_proto/zw_cc.h"

/* The documented times (network.md §4, security.md §5-6), as literals: a library constant that
 * drifts from them fails the tests. */
#define TA_MS          10000u
#define TAI_MS        240000u
#define WINDOW_MS      60000u
#define FOUND_MS       60000u
#define NONCE_WAIT_MS   1000u

static zw_net_t net;
static fake_zac93_t zac;
static fake_wire_t wire;
static fake_zdev_t dev;
static zw_host_crypto_t panel_hc;
static zw_crypto_port_t panel_crypto;
static zw_net_config_t cfg;

/* ---- The owner ---------------------------------------------------------------------------- */

static struct {
    unsigned ups;
    zw_incl_event_t ev[24];
    uint8_t nif[24][24];
    size_t n;
    zw_s2_rx_t rx[16];
    uint8_t rx_cmd[16][48];
    size_t rx_n;
    zw_s2_event_t s2ev[64];
    size_t s2ev_n;
    uint32_t done_tag[8];
    zw_s2_result_t done_r[8];
    size_t done_n;
    uint64_t done_at; /* the inclusion's end */
    unsigned rm_done; /* "Remove a device" ended: how often, how last, when */
    zw_remove_result_t rm_result;
    uint64_t rm_at;
} heard;

/* nodes the owner knows: registered at every link up */
static struct {
    uint16_t node;
    uint8_t granted;
} known[4];
static size_t known_n;
static uint16_t cache_has; /* a node ID the node cache already holds */

static int grant_with;     /* -1: no answer; 0: what was requested; else these keys */
static int pin_with;       /* -1: no answer; else this PIN */
static bool cancel_at_grant;
static bool lose_kex_set;  /* the panel's KEX Set never reaches the sensor the first time */

/* What the owner keeps across a panel restart (NVS zwave/keys and zwave/nodes). */
static uint8_t stored_nodes[4096];
static size_t stored_nodes_len;

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&wire, bytes, len);
}

static void on_store_nodes(void *ctx, const zw_node_cache_t *nodes, uint16_t generation)
{
    (void)ctx;
    stored_nodes_len = zw_node_cache_encode(nodes, stored_nodes, sizeof stored_nodes);
    TEST_ASSERT_NOT_EQUAL(0u, stored_nodes_len);
    cfg.generation = generation;
}

static void on_store_keys(void *ctx, uint32_t home_id, const zw_s2_keys_t *keys)
{
    (void)ctx;
    cfg.keys = *keys;
    cfg.keys_bound = true;
    cfg.keys_home_id = home_id;
}

static void on_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    (void)facts;
    heard.ups++;
    static const uint8_t NIF[] = {0x5E, 0x55, 0x9F, 0x6C};
    for (size_t i = 0; i < known_n; i++) {
        TEST_ASSERT_TRUE(zw_net_add_node(&net, known[i].node, known[i].granted, NIF, sizeof NIF));
    }
}

static void on_received(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    TEST_ASSERT_TRUE(heard.rx_n < 16u && rx->len <= 48u);
    heard.rx[heard.rx_n] = *rx;
    memcpy(heard.rx_cmd[heard.rx_n], rx->command, rx->len);
    heard.rx[heard.rx_n].command = heard.rx_cmd[heard.rx_n];
    heard.rx_n++;
}

static void on_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    if (heard.done_n < 8u) {
        heard.done_tag[heard.done_n] = tag;
        heard.done_r[heard.done_n] = r;
        heard.done_n++;
    }
}

static void on_incl(void *ctx, const zw_incl_event_t *ev)
{
    (void)ctx;
    TEST_ASSERT_TRUE(heard.n < 24u);
    heard.ev[heard.n] = *ev;
    if (ev->nif != NULL && ev->nif_len <= 24u) {
        memcpy(heard.nif[heard.n], ev->nif, ev->nif_len);
        heard.ev[heard.n].nif = heard.nif[heard.n];
    }
    heard.n++;
    if (ev->kind == ZW_INCL_EV_DONE) {
        heard.done_at = wire.now;
    }
    zw_incl_t *in = zw_net_incl(&net);
    if (ev->kind == ZW_INCL_EV_GRANT) {
        if (lose_kex_set) {
            dev.lose_in = 1u; /* the grant sends the KEX Set at once */
        }
        if (cancel_at_grant) {
            zw_incl_cancel(in, wire.now);
        } else if (grant_with >= 0) {
            const uint8_t keys = grant_with == 0 ? ev->requested : (uint8_t)grant_with;
            TEST_ASSERT_TRUE(zw_incl_grant(in, wire.now, keys));
        }
    } else if (ev->kind == ZW_INCL_EV_PIN && pin_with >= 0) {
        TEST_ASSERT_TRUE(zw_incl_pin(in, wire.now, (uint16_t)pin_with));
    }
}

static bool on_known(void *ctx, uint16_t node)
{
    (void)ctx;
    return node == cache_has;
}

static void on_s2_event(void *ctx, uint16_t node, zw_s2_event_t ev)
{
    (void)ctx;
    (void)node;
    if (heard.s2ev_n < 64u) {
        heard.s2ev[heard.s2ev_n++] = ev;
    }
}

static void on_net(void *ctx, const zw_net_event_t *ev)
{
    (void)ctx;
    if (ev->kind == ZW_NET_EV_REMOVE && ev->remove.kind == ZW_REMOVE_EV_DONE) {
        heard.rm_done++;
        heard.rm_result = ev->remove.result;
        heard.rm_at = wire.now;
    }
}

/* The wire's view of the panel. */
static void n_control(void *ctx, uint64_t now, uint8_t b)
{
    zw_net_on_control(ctx, now, b);
}

static void n_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    zw_net_on_frame(ctx, now, f);
}

static void n_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    zw_net_on_text(ctx, now, b, n);
}

static void n_poll(void *ctx, uint64_t now)
{
    zw_net_poll(ctx, now);
}

static uint64_t n_deadline(void *ctx)
{
    return zw_net_next_deadline(ctx);
}

static void run(uint64_t t)
{
    fake_wire_run_until(&wire, t);
}

static void run_for(uint64_t ms)
{
    run(wire.now + ms);
}

/* The panel (re)started: a fresh zw_net over the same module. */
static void panel_boot(void)
{
    static const zw_net_port_t port = {
        .ctx = NULL,
        .write = port_write,
        .reset_line = NULL,
        .link_up = on_link_up,
        .received = on_received,
        .send_done = on_send_done,
        .incl_event = on_incl,
        .known = on_known,
        .s2_event = on_s2_event,
        .event = on_net,
        .store_nodes = on_store_nodes,
        .store_keys = on_store_keys,
    };
    const fake_host_t fh = {&net, n_control, n_frame, n_text, n_poll, n_deadline};
    const uint64_t t = wire.now;
    zw_net_init(&net, &port, &cfg, &panel_crypto);
    if (stored_nodes_len > 0u) {
        TEST_ASSERT_TRUE(zw_net_load_nodes(&net, stored_nodes, stored_nodes_len));
    }
    fake_wire_init(&wire, &zac, &fh, t);
    zw_net_start(&net, t);
    run(t + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&net)));
}

void setUp(void)
{
    memset(&heard, 0, sizeof heard);
    known_n = 0u;
    cache_has = 0u;
    grant_with = 0;
    pin_with = -1;
    cancel_at_grant = false;
    lose_kex_set = false;
    stored_nodes_len = 0u;
    memset(&cfg, 0, sizeof cfg);
    memset(cfg.keys.s2_unauth, 0x11, 16);
    memset(cfg.keys.s2_auth, 0x22, 16);
    memset(cfg.keys.s2_access, 0x33, 16);
    memset(cfg.keys.s0, 0x44, 16);
    memset(cfg.keys.lr_auth, 0x55, 16);
    memset(cfg.keys.lr_access, 0x66, 16);
    zw_host_crypto_init(&panel_hc, 0x70616E656Cu);
    panel_crypto = zw_host_crypto_port(&panel_hc);
    fake_zac93_init(&zac);
    fake_zdev_init(&dev, 0x5E41u);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&zac, &dev));
    memset(&wire, 0, sizeof wire);
    panel_boot();
    pin_with = dev.pin;
}

void tearDown(void)
{
    TEST_ASSERT_FALSE_MESSAGE(wire.stuck, "the wire did not settle");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, zac.overflow, "the fake module ran out of room");
}

/* ---- Helpers ------------------------------------------------------------------------------ */

static const zw_incl_event_t *find(zw_incl_event_kind_t kind)
{
    for (size_t i = heard.n; i > 0u; i--) {
        if (heard.ev[i - 1u].kind == kind) {
            return &heard.ev[i - 1u];
        }
    }
    return NULL;
}

static const zw_incl_event_t *done_event(void)
{
    const zw_incl_event_t *e = find(ZW_INCL_EV_DONE);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the inclusion did not end");
    return e;
}

static unsigned count_heard(uint8_t func, int b0, int b1)
{
    unsigned n = 0;
    for (size_t i = 0; i < zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&zac, i);
        if (h != NULL && h->func == func &&
            (b0 < 0 || (h->payload_len > 0 && h->payload[0] == b0)) &&
            (b1 < 0 || (h->payload_len > 1 && h->payload[1] == b1))) {
            n++;
        }
    }
    return n;
}

/* SendData frames to `node` whose command starts `cc cmd` (an unencrypted one). */
static unsigned sends_of(uint8_t node, uint8_t cc, uint8_t cmd)
{
    unsigned n = 0;
    for (size_t i = 0; i < zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&zac, i);
        if (h != NULL && h->func == 0x13 && h->payload_len > 3 && h->payload[0] == node &&
            h->payload[2] == cc && h->payload[3] == cmd) {
            n++;
        }
    }
    return n;
}

/* AddNode stops the panel sent (mode C5). */
static unsigned stops(void)
{
    return count_heard(0x4A, 0xC5, -1);
}

/* "Add a device", and the sensor's button pressed at once. */
static void start_and_press(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0u, 0u));
    run_for(100u);
    TEST_ASSERT_NOT_NULL(find(ZW_INCL_EV_LISTENING));
    TEST_ASSERT_TRUE(fake_zac93_press(&zac, wire.now, &dev));
}

static const zw_incl_event_t *join(void)
{
    start_and_press();
    run_for(20000u);
    return done_event();
}

static unsigned count_s2(zw_s2_event_t ev)
{
    unsigned n = 0;
    for (size_t i = 0; i < heard.s2ev_n; i++) {
        n += heard.s2ev[i] == ev ? 1u : 0u;
    }
    return n;
}

static size_t door(uint8_t *out, uint8_t event)
{
    zw_cc_notification_report_t r;
    memset(&r, 0, sizeof r);
    r.status = ZW_NOTIFICATION_STATUS_ON;
    r.notification_type = ZW_NOTIFICATION_TYPE_ACCESS_CONTROL;
    r.event = event;
    return zw_cc_notification_report_build(out, 16u, &r);
}

static void expect_secure(const zw_incl_event_t *e, zw_incl_security_t sec, uint8_t granted)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, e->result);
    TEST_ASSERT_EQUAL_INT(sec, e->security);
    TEST_ASSERT_EQUAL_HEX8(granted, e->granted);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_S2_OK, e->failure);
    TEST_ASSERT_EQUAL_HEX8(granted, zw_s2_granted(zw_net_s2(&net), e->node));
    TEST_ASSERT_EQUAL_UINT16(0, zw_net_s2(&net)->temp_node); /* the temporary key deleted */
    run_for(2000u);
    TEST_ASSERT_EQUAL_INT(FAKE_ZDEV_SECURE, dev.status);
    TEST_ASSERT_EQUAL_HEX8(granted, dev.have);
}

/* An S2 failure: in the network without security, said so; no class, no temporary key. */
static void expect_insecure(zw_incl_security_t sec, zw_incl_s2_failure_t failure, uint8_t detail)
{
    const zw_incl_event_t *e = done_event();
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, e->result);
    TEST_ASSERT_EQUAL_INT(sec, e->security);
    TEST_ASSERT_EQUAL_INT(failure, e->failure);
    TEST_ASSERT_EQUAL_HEX8(detail, e->failure_detail);
    TEST_ASSERT_EQUAL_HEX8(0, e->granted);
    TEST_ASSERT_EQUAL_HEX8(0, zw_s2_granted(zw_net_s2(&net), e->node));
    TEST_ASSERT_EQUAL_UINT16(0, zw_net_s2(&net)->temp_node);
    TEST_ASSERT_TRUE(zw_s2_has_node(zw_net_s2(&net), e->node)); /* it stays in the network */
}

/* ---- S2 joins (ZW-006) --------------------------------------------------------------------- */

/* ZW-005, ZW-006: a sensor asking for Access Control, Authenticated and Unauthenticated joins
 * with all three: the window, found, added, the installer's consent, the PIN typed from its
 * label (groups 2-8 shown), the key exchange; then its report counts at Access Control and a
 * "door closed" in the clear is dropped (ZW-009). */
static void test_join_s2_access_control(void)
{
    const zw_incl_event_t *e = join();
    TEST_ASSERT_EQUAL_INT(ZW_INCL_EV_LISTENING, heard.ev[0].kind);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_EV_FOUND, heard.ev[1].kind);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_EV_ADDING, heard.ev[2].kind);
    TEST_ASSERT_EQUAL_UINT16(2, heard.ev[2].node);
    TEST_ASSERT_EQUAL_UINT(dev.nif_len, heard.ev[2].nif_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(dev.nif, heard.ev[2].nif, dev.nif_len);
    const zw_incl_event_t *g = find(ZW_INCL_EV_GRANT);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_HEX8(0x07, g->requested);
    const zw_incl_event_t *p = find(ZW_INCL_EV_PIN);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING_LEN("00000-", p->dsk, 6);          /* the PIN is not sent */
    TEST_ASSERT_EQUAL_STRING(&dev.dsk[5], &p->dsk[5]);          /* groups 2-8 as on its label */
    expect_secure(e, ZW_INCL_SEC_S2_ACCESS, 0x07);
    TEST_ASSERT_EQUAL_STRING(dev.dsk, e->dsk);                  /* the whole DSK, PIN included */
    TEST_ASSERT_FALSE(e->route_failed);
    TEST_ASSERT_EQUAL_UINT(1, count_heard(0x51, 2, -1));       /* the SUC return route */
    TEST_ASSERT_EQUAL_UINT(2, stops());                         /* C5 with Done, C5 without */
    /* at work: the door opens (Access Control), closes in the clear (dropped) */
    uint8_t cmd[16];
    size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, heard.rx[0].security);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cmd, heard.rx_cmd[0], n);
    n = door(cmd, 0x17);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_NONE, cmd, n));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_EQUAL_UINT(1, count_s2(ZW_S2_EV_WRONG_LEVEL));
}

/* ZW-006: Authenticated (with the PIN). */
static void test_join_s2_authenticated(void)
{
    dev.kex.keys = ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH;
    expect_secure(join(), ZW_INCL_SEC_S2_AUTH, 0x03);
    TEST_ASSERT_NOT_NULL(find(ZW_INCL_EV_PIN));
}

/* ZW-006: Unauthenticated: no PIN asked for; "Secure (no PIN)". */
static void test_join_s2_unauthenticated(void)
{
    dev.kex.keys = ZW_S2_KEY_UNAUTH;
    expect_secure(join(), ZW_INCL_SEC_S2_UNAUTH, 0x01);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_PIN));
}

/* ZW-006: the installer grants less than asked for - Unauthenticated only: no PIN, and exactly
 * that (nothing is granted that was not chosen). */
static void test_installer_grants_less(void)
{
    grant_with = ZW_S2_KEY_UNAUTH;
    expect_secure(join(), ZW_INCL_SEC_S2_UNAUTH, 0x01);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_PIN));
    TEST_ASSERT_FALSE(zw_incl_grant(zw_net_incl(&net), wire.now, 0x01)); /* not now */
}

/* network.md §4 step 7: a device without Security 2 is added without it ("Not encrypted"); no
 * KEX Get goes. */
static void test_join_without_s2(void)
{
    fake_zdev_no_s2(&dev);
    const zw_incl_event_t *e = join();
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, e->result);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_SEC_NONE, e->security);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_S2_OK, e->failure);
    TEST_ASSERT_EQUAL_UINT(0, count_heard(0x13, 2, -1)); /* nothing sent to it */
}

/* ---- network.md §4 (ZW-005) ----------------------------------------------------------------- */

/* ZW-005: not in installer mode with every partition disarmed: refused, nothing sent; one
 * operation at a time. */
static void test_refusals_before_start(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_NOT_ALLOWED, zw_net_add_device(&net, wire.now, false, 0, 0));
    run_for(100u);
    TEST_ASSERT_EQUAL_UINT(0, count_heard(0x4A, -1, -1));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0, 0));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BUSY, zw_net_add_device(&net, wire.now, true, 0, 0));
}

/* ZW-005: a module that skips Ready: its first report (NodeFound) ends the start and counts -
 * the device joins all the same. */
static void test_module_skips_ready(void)
{
    zac.skip_ready = true;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0u, 0u));
    run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&zac, wire.now, &dev));
    run_for(20000u);
    TEST_ASSERT_NOT_NULL(find(ZW_INCL_EV_LISTENING));
    TEST_ASSERT_NOT_NULL(find(ZW_INCL_EV_FOUND)); /* the first report, taken */
    expect_secure(done_event(), ZW_INCL_SEC_S2_ACCESS, 0x07);
}

/* ZW-005: no Ready within 10 s: "The Z-Wave radio did not start listening". */
static void test_radio_does_not_listen(void)
{
    zac.fault.mute_func = 0x4A;
    zac.fault.mute_count = 1u;
    const uint64_t t0 = wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0, 0));
    run(t0 + 9999u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    run(t0 + 10100u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_NOT_LISTENING, done_event()->result);
    TEST_ASSERT_EQUAL_UINT(1, stops());
}

/* ZW-005: the 60 s window ends with nothing found; "Keep listening" gives 60 s again. */
static void test_window_and_keep_listening(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0, 0));
    run_for(100u);
    const uint64_t closes = find(ZW_INCL_EV_LISTENING)->deadline;
    run(closes - 10000u);
    zw_incl_keep_listening(zw_net_incl(&net), wire.now);
    const uint64_t again = wire.now + WINDOW_MS;
    run(closes + 1000u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    run(again - 1u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    run(again + 100u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_NO_DEVICE, done_event()->result);
    TEST_ASSERT_EQUAL_UINT(1, stops());
    TEST_ASSERT_FALSE(fake_zac93_press(&zac, wire.now, &dev)); /* the module no longer listens */
}

/* ZW-005: the installer stops listening. */
static void test_installer_stops(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0, 0));
    run_for(100u);
    zw_incl_stop(zw_net_incl(&net), wire.now);
    run_for(100u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STOPPED, done_event()->result);
    TEST_ASSERT_EQUAL_UINT(1, stops());
}

/* ZW-005, network.md §4 step 3: a controller is refused - stopped at once. */
static void test_controller_refused(void)
{
    dev.controller = true;
    start_and_press();
    run_for(5000u);
    const zw_incl_event_t *e = done_event();
    TEST_ASSERT_EQUAL_INT(ZW_INCL_CONTROLLER, e->result);
    TEST_ASSERT_EQUAL_UINT16(2, e->node);
    TEST_ASSERT_EQUAL_UINT(1, stops());
    TEST_ASSERT_EQUAL_UINT(0, count_heard(0x51, -1, -1));
}

/* ZW-005: status Failed: "The device did not finish joining". */
static void test_status_failed(void)
{
    zac.join_fail_after = ZW_ADD_NODE_STATUS_NODE_FOUND;
    start_and_press();
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_FAILED, done_event()->result);
}

/* ZW-005: the protocol stalls - no AddingEndNode within 60 s of NodeFound; no ProtocolDone within
 * 76 s + 217 ms per listening node + 3.5 s per FLiRS node. */
static void test_protocol_timeouts(void)
{
    zac.join_stall_after = ZW_ADD_NODE_STATUS_NODE_FOUND;
    start_and_press();
    const uint64_t found = wire.now + zac.join_step_ms;
    run(found + FOUND_MS - 1u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    run(found + FOUND_MS + 100u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_TIMEOUT, done_event()->result);
    /* again, stalling after AddingEndNode, with 2 listening and 1 FLiRS node in the network */
    memset(&heard, 0, sizeof heard);
    zac.join_stall_after = ZW_ADD_NODE_STATUS_ADDING_END_NODE;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 2u, 1u));
    run_for(100u);
    fake_zdev_init(&dev, 0x5E42u);
    TEST_ASSERT_TRUE(fake_zac93_press(&zac, wire.now, &dev));
    const uint64_t adding = wire.now + 2u * zac.join_step_ms;
    const uint64_t limit = 76000u + 2u * 217u + 3500u;
    run(adding + limit - 1u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    run(adding + limit + 100u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_TIMEOUT, done_event()->result);
}

/* ZW-005: node IDs refused after Done - one already in the cache, the broadcast ID. */
static void test_node_id_refusals(void)
{
    cache_has = 5u;
    zac.join_node_id = 5u;
    start_and_press();
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ALREADY_ADDED, done_event()->result);
    memset(&heard, 0, sizeof heard);
    zac.join_node_id = 0xFFu;
    fake_zdev_init(&dev, 0x5E43u);
    start_and_press();
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BAD_NODE_ID, done_event()->result);
}

/* network.md §4 step 6: a SUC return route that fails does not stop the device joining. */
static void test_route_failure_still_joins(void)
{
    zac.route_fails = true;
    const zw_incl_event_t *e = join();
    TEST_ASSERT_TRUE(e->route_failed);
    expect_secure(e, ZW_INCL_SEC_S2_ACCESS, 0x07);
}

/* ---- The key exchange's failures (ZW-006) -------------------------------------------------- */

/* ZW-006, §5 step 8: the wrong PIN - the device's echo fails to decrypt twice on an exchanged
 * nonce: KEX Fail "cancel", "Wrong PIN", no class. */
static void test_wrong_pin(void)
{
    pin_with = dev.pin ^ 0x0101;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_WRONG_PIN, 0);
    TEST_ASSERT_EQUAL_INT(FAKE_ZDEV_FAILED, dev.status);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEX_FAIL_CANCEL, dev.fail_got);
}

/* Runs in 1 ms steps until the inclusion waits in `st`: the time it got there. */
static uint64_t run_until_state(zw_incl_state_t st)
{
    const uint64_t limit = wire.now + 30000u;
    while (zw_incl_state(zw_net_incl(&net)) != st && wire.now < limit) {
        run(wire.now + 1u);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(st, zw_incl_state(zw_net_incl(&net)), "never got there");
    return wire.now;
}

/* ZW-006: every timer, to the millisecond from where its wait began - the device silent at each
 * step (TA1-TA5; the echo under TAI2), the installer never deciding (TAI1) or never typing the
 * PIN (TAI2). */
static void test_each_timer(void)
{
    static const struct {
        uint8_t silent_at;
        int grant;
        int pin;
        zw_incl_state_t waits;
        uint32_t ms;
        uint8_t detail;
    } CASES[] = {
        {FAKE_KEX_REPORT, 0, 1, ZW_INCL_KEX_REPORT, TA_MS, 1u},
        {FAKE_KEX_PUBKEY, 0, 1, ZW_INCL_PUBKEY, TA_MS, 2u},
        {FAKE_KEX_ECHO, 0, 1, ZW_INCL_ECHO, TAI_MS, ZW_INCL_TAI2},
        {FAKE_KEX_NETKEY_GET, 0, 1, ZW_INCL_NETKEY_GET, TA_MS, 3u},
        {FAKE_KEX_VERIFY, 0, 1, ZW_INCL_VERIFY, TA_MS, 4u},
        {FAKE_KEX_TRANSFER_END, 0, 1, ZW_INCL_TRANSFER_END, TA_MS, 5u},
        {FAKE_KEX_NONE, -1, 1, ZW_INCL_GRANT, TAI_MS, ZW_INCL_TAI1},
        {FAKE_KEX_NONE, 0, -1, ZW_INCL_PIN, TAI_MS, ZW_INCL_TAI2},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        tearDown();
        setUp();
        dev.silent_at = CASES[i].silent_at;
        grant_with = CASES[i].grant;
        pin_with = CASES[i].pin < 0 ? -1 : (int)dev.pin;
        start_and_press();
        const uint64_t t = run_until_state(CASES[i].waits);
        run(t + CASES[i].ms - 1u);
        TEST_ASSERT_NULL_MESSAGE(find(ZW_INCL_EV_DONE), "too early");
        run(t + CASES[i].ms);
        expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, CASES[i].detail);
        run_for(1000u); /* the panel ends it with a KEX Fail the device hears */
        TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEX_FAIL_CANCEL, dev.fail_got);
    }
}

/* ZW-006, security.md §5 "An empty node information": a node information that lists nothing
 * does not say "no Security 2". KEX Get goes anyway: a device with S2 joins securely; one that
 * never answers ends at TA1 as NO_INFO (not confirmed), never as an ordinary device without
 * security (ZW_INCL_SEC_NONE). A list without 9F still means no S2 (no KEX Get at all). */
static void test_empty_node_information(void)
{
    dev.nif_len = 0u;
    const zw_incl_event_t *e = join();
    TEST_ASSERT_EQUAL_UINT(0u, heard.ev[2].nif_len);
    expect_secure(e, ZW_INCL_SEC_S2_ACCESS, 0x07);

    tearDown();
    setUp();
    dev.nif_len = 0u;
    dev.silent_at = FAKE_KEX_REPORT; /* it answers nothing of the key exchange */
    start_and_press();
    const uint64_t t = run_until_state(ZW_INCL_KEX_REPORT);
    run(t + TA_MS - 1u);
    TEST_ASSERT_NULL_MESSAGE(find(ZW_INCL_EV_DONE), "too early");
    run(t + TA_MS);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_NO_INFO, 1u);

    tearDown();
    setUp();
    fake_zdev_no_s2(&dev); /* a real list, without 9F */
    e = join();
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, e->result);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_SEC_NONE, e->security);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_S2_OK, e->failure);
}

/* ZW-006: the device's KEX Fail, at each of its steps, ends it (insecure, its type said). */
static void test_kex_fail_from_device(void)
{
    static const uint8_t STEPS[] = {FAKE_KEX_REPORT, FAKE_KEX_PUBKEY, FAKE_KEX_ECHO,
                                    FAKE_KEX_NETKEY_GET, FAKE_KEX_VERIFY, FAKE_KEX_TRANSFER_END};
    static const uint8_t TYPES[] = {ZW_S2_KEX_FAIL_KEY, ZW_S2_KEX_FAIL_SCHEME,
                                    ZW_S2_KEX_FAIL_DECRYPT, ZW_S2_KEX_FAIL_CANCEL,
                                    ZW_S2_KEX_FAIL_KEY_VERIFY, ZW_S2_KEX_FAIL_KEY_REPORT};
    for (size_t i = 0; i < sizeof STEPS; i++) {
        tearDown();
        setUp();
        dev.fail_at = STEPS[i];
        dev.fail_type = TYPES[i];
        start_and_press();
        run_for(20000u);
        expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_DEVICE_FAIL, TYPES[i]);
    }
}

/* ZW-006, §5 step 1: the panel refuses a KEX Report with the echo set, another scheme or curve,
 * a CSA request, or no key it has - KEX Fail with the matching type. */
static void test_kex_report_refused(void)
{
    static const struct {
        uint8_t flags, schemes, curves, keys, fail;
    } CASES[] = {
        {0x01, 0x02, 0x01, 0x07, ZW_S2_KEX_FAIL_CANCEL}, /* echo */
        {0x00, 0x06, 0x01, 0x07, ZW_S2_KEX_FAIL_SCHEME}, /* not exactly scheme 1 */
        {0x00, 0x02, 0x03, 0x07, ZW_S2_KEX_FAIL_CURVES}, /* not exactly Curve25519 */
        {0x02, 0x02, 0x01, 0x07, ZW_S2_KEX_FAIL_CANCEL}, /* CSA requested */
        {0x00, 0x02, 0x01, 0x00, ZW_S2_KEX_FAIL_KEY},    /* nothing the panel has */
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        tearDown();
        setUp();
        dev.kex.flags = CASES[i].flags;
        dev.kex.schemes = CASES[i].schemes;
        dev.kex.curves = CASES[i].curves;
        dev.kex.keys = CASES[i].keys;
        start_and_press();
        run_for(20000u);
        expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_REFUSED, CASES[i].fail);
        TEST_ASSERT_EQUAL_HEX8(CASES[i].fail, dev.fail_got);
        TEST_ASSERT_NULL(find(ZW_INCL_EV_GRANT));
    }
}

/* ZW-006: nothing is granted that was not requested (nor nothing at all); a valid grant goes. */
static void test_grant_refusals(void)
{
    grant_with = -1;
    dev.kex.keys = ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH;
    start_and_press();
    (void)run_until_state(ZW_INCL_GRANT);
    zw_incl_t *in = zw_net_incl(&net);
    TEST_ASSERT_FALSE(zw_incl_grant(in, wire.now, ZW_S2_KEY_ACCESS));
    TEST_ASSERT_FALSE(zw_incl_grant(in, wire.now, ZW_S2_KEY_S0 | ZW_S2_KEY_AUTH));
    TEST_ASSERT_FALSE(zw_incl_grant(in, wire.now, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_GRANT, zw_incl_state(in));
    TEST_ASSERT_TRUE(zw_incl_grant(in, wire.now, ZW_S2_KEY_AUTH));
    run_for(20000u);
    expect_secure(done_event(), ZW_INCL_SEC_S2_AUTH, 0x02);
}

/* ZW-006: TAI2 runs from the panel's Public Key Report, through the PIN and the echo: a PIN typed
 * late does not start it again. */
static void test_tai2_from_public_key(void)
{
    pin_with = -1;
    dev.silent_at = FAKE_KEX_ECHO;
    start_and_press();
    const uint64_t t = run_until_state(ZW_INCL_PIN);
    run(t + 100000u);
    TEST_ASSERT_TRUE(zw_incl_pin(zw_net_incl(&net), wire.now, dev.pin));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ECHO, zw_incl_state(zw_net_incl(&net)));
    run(t + TAI_MS - 1u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    run(t + TAI_MS);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, ZW_INCL_TAI2);
}

/* ZW-006: a PIN typed late (30 s): the device's echoes meanwhile cannot be decrypted (no
 * temporary key yet) and are dropped without a nonce, and do not count as a wrong PIN; its next
 * repeat after the PIN gets through. */
static void test_late_pin_joins(void)
{
    pin_with = -1;
    start_and_press();
    const uint64_t t = run_until_state(ZW_INCL_PIN);
    run(t + 30000u);
    TEST_ASSERT_TRUE(dev.echoes >= 3u); /* again about every 10 s */
    TEST_ASSERT_TRUE(zw_incl_pin(zw_net_incl(&net), wire.now, dev.pin));
    run_for(20000u);
    expect_secure(done_event(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    TEST_ASSERT_TRUE(count_s2(ZW_S2_EV_NO_KEY) >= 3u);
}

/* ZW-006, §5 step 8: one echo that fails to decrypt (damaged on the way) is not a wrong PIN - a
 * Nonce Report, the echo again, and the device joins; only a second failure is. */
static void test_one_damaged_echo_is_not_a_wrong_pin(void)
{
    dev.damage_first_echo = true;
    const zw_incl_event_t *e = join();
    expect_secure(e, ZW_INCL_SEC_S2_ACCESS, 0x07);
    TEST_ASSERT_EQUAL_UINT(1, count_s2(ZW_S2_EV_DECRYPT_FAILED_SPAN));
}

/* ZW-006, §5 step 10: a Network Key Get for a key not granted (fail 08); a Network Key Verify
 * under another key than the one just sent (fail 09). */
static void test_wrong_key_requests(void)
{
    grant_with = ZW_S2_KEY_UNAUTH;
    dev.ask_key = ZW_S2_KEY_ACCESS;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_REFUSED, ZW_S2_KEX_FAIL_KEY_GET);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEX_FAIL_KEY_GET, dev.fail_got);

    tearDown();
    setUp();
    dev.verify_with_temp = true;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_REFUSED, ZW_S2_KEX_FAIL_KEY_VERIFY);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEX_FAIL_KEY_VERIFY, dev.fail_got);
}

/* ZW-006: the installer cancels at the grant: KEX Fail "cancel", added without encryption. */
static void test_installer_cancels(void)
{
    cancel_at_grant = true;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_CANCELLED, 0);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEX_FAIL_CANCEL, dev.fail_got);
}

/* ZW-006: downgrades are detected - a device listing Security 2 but asking for S0 only; its KEX
 * Report changed on the way (the device refuses the panel's echo); the panel's KEX Set changed
 * on the way (the device's echo differs). "Security was downgraded", no class. */
static void test_downgrades_detected(void)
{
    dev.kex.keys = ZW_S2_KEY_S0;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_DOWNGRADED, ZW_INCL_S2_TAMPERED, ZW_S2_KEX_FAIL_KEY);

    tearDown();
    setUp();
    dev.mitm_report_keys = ZW_S2_KEY_UNAUTH; /* asked for all three; the panel sees one */
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_DOWNGRADED, ZW_INCL_S2_TAMPERED, ZW_S2_KEX_FAIL_AUTH);
    TEST_ASSERT_EQUAL_INT(FAKE_ZDEV_FAILED, dev.status);

    tearDown();
    setUp();
    dev.mitm_set_keys = ZW_S2_KEY_UNAUTH; /* granted all three; the device sees one */
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_DOWNGRADED, ZW_INCL_S2_TAMPERED, ZW_S2_KEX_FAIL_AUTH);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEX_FAIL_AUTH, dev.fail_got);

    tearDown();
    setUp();
    dev.echo_flags_or = 0x04u; /* an echo with a reserved bit set is not the KEX Set sent */
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_DOWNGRADED, ZW_INCL_S2_TAMPERED, ZW_S2_KEX_FAIL_AUTH);
}

/* ZW-006, §5 steps 10-11: a Network Key Get in the clear (fail 07, wrong security level); a last
 * Transfer End without "key request complete" (fail 06). */
static void test_bad_kex_messages_refused(void)
{
    dev.netkey_get_plain = true;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_REFUSED, ZW_S2_KEX_FAIL_AUTH);

    tearDown();
    setUp();
    dev.te_flags_set = true;
    dev.te_flags = 0x00u;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_REFUSED, ZW_S2_KEX_FAIL_CANCEL);
}

/* ZW-006: the link goes down in the middle of a key exchange (here while the installer decides):
 * the device is in the network, without security, and the inclusion says so. */
static void test_link_down_during_key_exchange(void)
{
    grant_with = -1;
    start_and_press();
    (void)run_until_state(ZW_INCL_GRANT);
    zac.fault.dead = true;
    /* the owner's next request finds the module gone: recovery, then link down */
    static const uint8_t NOTHING[1] = {0};
    zw_request_t r;
    memset(&r, 0, sizeof r);
    r.func = 0x20; /* GetControllerId */
    r.payload = NOTHING;
    r.payload_len = 0u;
    r.expect = ZW_EXPECT_RESPONSE;
    r.prio = ZW_PRIO_NODE;
    r.tag = 5u;
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, zw_host_submit(zw_net_host(&net), &r));
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(zw_net_host(&net)));
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0);
}

/* ---- The radio restarts mid-window (network.md §4, §10) ------------------------------------- */

static unsigned count_done(void)
{
    unsigned n = 0;
    for (size_t i = 0; i < heard.n; i++) {
        n += heard.ev[i].kind == ZW_INCL_EV_DONE ? 1u : 0u;
    }
    return n;
}

/* ZW-004, ZW-005: the module restarts by itself (an unasked SerialAPIStarted, serial-api.md §2)
 * while the add window is open: its window closed with it, so the inclusion ends at once -
 * RADIO_RESTARTED, not "nothing joined" 60 s later; the device's button does nothing; the stop
 * goes after the restart; the operation is freed: a new window opens and the device joins. */
static void test_restart_ends_add_window(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0u, 0u));
    run_for(100u);
    TEST_ASSERT_NOT_NULL(find(ZW_INCL_EV_LISTENING));
    const unsigned ups = heard.ups;
    const uint32_t restarts = zw_host_stats(zw_net_host(&net))->restarts;
    const uint64_t at = wire.now + 10u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, at, 0x03u));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT32(restarts + 1u, zw_host_stats(zw_net_host(&net))->restarts);
    TEST_ASSERT_EQUAL_UINT(ups + 1u, heard.ups);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_RADIO_RESTARTED, done_event()->result);
    TEST_ASSERT_TRUE(heard.done_at < at + 1000u); /* at once */
    TEST_ASSERT_EQUAL_INT(ZW_INCL_IDLE, zw_incl_state(zw_net_incl(&net)));
    TEST_ASSERT_EQUAL_UINT(1, stops());
    TEST_ASSERT_FALSE(fake_zac93_press(&zac, wire.now, &dev)); /* the module no longer listens */
    run(at + WINDOW_MS + 5000u);
    TEST_ASSERT_EQUAL_UINT(1, count_done()); /* no second end when the old window would close */
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
}

/* ZW-004, ZW-005: serial-api.md §7's soft reset mid-window (a module that stopped answering, a
 * jammed radio's reset: zw_host_recover) restarts the module without a restarted() - the same
 * end, at once, and not counted as the radio restarting by itself. */
static void test_recovery_ends_add_window(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0u, 0u));
    run_for(100u);
    const uint32_t restarts = zw_host_stats(zw_net_host(&net))->restarts;
    const uint32_t module_restarts = zac.restarts;
    const uint64_t at = wire.now;
    TEST_ASSERT_TRUE(zw_host_recover(zw_net_host(&net), wire.now));
    run_for(2000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&net)));
    TEST_ASSERT_EQUAL_UINT32(module_restarts + 1u, zac.restarts);
    TEST_ASSERT_EQUAL_UINT32(restarts, zw_host_stats(zw_net_host(&net))->restarts);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_RADIO_RESTARTED, done_event()->result);
    TEST_ASSERT_TRUE(heard.done_at < at + 1000u);
    TEST_ASSERT_EQUAL_UINT(1, stops());
    TEST_ASSERT_FALSE(fake_zac93_press(&zac, wire.now, &dev));
    run(at + WINDOW_MS + 5000u);
    TEST_ASSERT_EQUAL_UINT(1, count_done());
    /* the remove window too */
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&net, wire.now, true));
    run_for(100u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_LISTENING, zw_remove_state(zw_net_rm(&net)));
    const uint64_t at2 = wire.now;
    TEST_ASSERT_TRUE(zw_host_recover(zw_net_host(&net), wire.now));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rm_done);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_RADIO_RESTARTED, heard.rm_result);
    TEST_ASSERT_TRUE(heard.rm_at < at2 + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_IDLE, zw_remove_state(zw_net_rm(&net)));
}

/* ZW-004, ZW-005: "Remove a device" while the module restarts by itself: RADIO_RESTARTED at
 * once, the module no longer removes, and the operation is free for the next one. */
static void test_restart_ends_remove_window(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&net, wire.now, true));
    run_for(100u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_LISTENING, zw_remove_state(zw_net_rm(&net)));
    const uint64_t at = wire.now + 10u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, at, 0x03u));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rm_done);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_RADIO_RESTARTED, heard.rm_result);
    TEST_ASSERT_TRUE(heard.rm_at < at + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_IDLE, zw_remove_state(zw_net_rm(&net)));
    TEST_ASSERT_FALSE(fake_zac93_press_remove(&zac, wire.now, &dev));
    run(at + WINDOW_MS + 5000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rm_done);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0u, 0u));
}

/* ZW-005, ZW-006: a restart after the device joined - it is in the module, so nothing ends: the
 * installer's grant still counts and the key exchange finishes securely. */
static void test_restart_after_join_continues(void)
{
    grant_with = -1;
    start_and_press();
    (void)run_until_state(ZW_INCL_GRANT);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, wire.now + 10u, 0x03u));
    run_for(2000u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_DONE));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_GRANT, zw_incl_state(zw_net_incl(&net)));
    const zw_incl_event_t *g = find(ZW_INCL_EV_GRANT);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_TRUE(zw_incl_grant(zw_net_incl(&net), wire.now, g->requested));
    run_for(20000u);
    expect_secure(done_event(), ZW_INCL_SEC_S2_ACCESS, 0x07);
}

/* ZW-004, ZW-005: a restart that ends the AddNode or RemoveNode still waiting for its first report
 * (ZW_TX_MODULE_RESTARTED) is the same end - RADIO_RESTARTED, not "did not start listening". */
static void test_restart_before_ready(void)
{
    zac.skip_ready = true; /* the first report never comes before the restart */
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&net, wire.now, true, 0u, 0u));
    const uint64_t at = wire.now + 50u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, at, 0x03u));
    run_for(2000u);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_LISTENING));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_RADIO_RESTARTED, done_event()->result);
    TEST_ASSERT_TRUE(heard.done_at < at + 1000u);
    TEST_ASSERT_EQUAL_UINT(1, count_done());
    zac.skip_remove_ready = true;
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&net, wire.now, true));
    const uint64_t at2 = wire.now + 50u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, at2, 0x03u));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rm_done);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_RADIO_RESTARTED, heard.rm_result);
    TEST_ASSERT_TRUE(heard.rm_at < at2 + 1000u);
}

/* ZW-005: a restart between ProtocolDone and Done ends the inclusion, "radio restarted", as Z-Wave
 * JS's performInclusion ends when finishInclusion gets no Done: no node added, no key exchange.
 * The module may still hold the device; the reconcile at the next link up takes it in. */
static void test_restart_after_protocol_done_ends(void)
{
    start_and_press();
    (void)run_until_state(ZW_INCL_FINISHING);
    run_for(5u); /* the stop is in the module: its Done comes 20 ms after it arrived */
    TEST_ASSERT_EQUAL_INT(ZW_INCL_FINISHING, zw_incl_state(zw_net_incl(&net)));
    const uint32_t module_restarts = zac.restarts;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, wire.now + 1u, 0x03u));
    run_for(20000u);
        TEST_ASSERT_EQUAL_UINT32(module_restarts + 1u, zac.restarts);
    TEST_ASSERT_NULL(find(ZW_INCL_EV_JOINED));
    TEST_ASSERT_NULL(find(ZW_INCL_EV_GRANT));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_RADIO_RESTARTED, done_event()->result);
    TEST_ASSERT_EQUAL_UINT(1, count_done());
}

/* ---- At work (ZW-007, ZW-009) ---------------------------------------------------------------- */

/* The sensor's Commands Supported Report lists Battery: a Battery Get to it goes encrypted
 * (ZW-009, Z-Wave JS Driver.ts isCCSecure - a CC the node does not support goes in the clear). */
static void supports_battery(void)
{
    static const uint8_t ITS[] = {0x71, 0x80};
    TEST_ASSERT_TRUE(zw_s2_set_secure_ccs(zw_net_s2(&net), 2u, ITS, sizeof ITS));
}

/* ZW-009, ZW-042: commands from a node the panel does not know or from another network (receive
 * status bits 6-7) are dropped before anyone sees them; a known node's broadcast and multicast
 * ones pass the same security check as singlecast (Z-Wave JS isSecurityLevelTooLow): from the S2
 * sensor in the clear they are a wrong level, and so is its NIF's CC singlecast. */
static void test_frames_dropped_by_net(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    static const uint8_t PLUS_INFO[] = {0x5E, 0x02, 0x02, 0x00, 0x00, 0x0C, 0x07, 0x0C, 0x07};
    const struct {
        uint8_t status;
        uint16_t node;
    } CASES[] = {{0x00, 9u}, {0x80, 2u}, {0x40, 2u}, {0x04, 2u}, {0x08, 2u}};
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const zw_api_app_cmd_t c = {false, CASES[i].status, 0u, CASES[i].node, PLUS_INFO,
                                    sizeof PLUS_INFO, NULL, 0u, false, 0};
        uint8_t p[32];
        const size_t n = zw_api_app_cmd_build(p, sizeof p, ZW_NID_8, &c);
        TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, wire.now + 1u, 0x00, 0x04, p, n));
        run_for(100u);
    }
    TEST_ASSERT_EQUAL_UINT(0, heard.rx_n);
    TEST_ASSERT_EQUAL_UINT(1, count_s2(ZW_S2_EV_UNKNOWN_NODE));
    TEST_ASSERT_EQUAL_UINT32(2, zw_net_stats(&net)->foreign);
    TEST_ASSERT_EQUAL_UINT32(0, zw_net_stats(&net)->not_singlecast);
    TEST_ASSERT_EQUAL_UINT(2, count_s2(ZW_S2_EV_WRONG_LEVEL));
    /* the same from the sensor itself, singlecast: its NIF's CC in the clear is dropped too */
    const zw_api_app_cmd_t ok = {false, 0x00, 0u, 2u, PLUS_INFO, sizeof PLUS_INFO, NULL, 0u,
                                 false, 0};
    uint8_t p[32];
    const size_t n = zw_api_app_cmd_build(p, sizeof p, ZW_NID_8, &ok);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, wire.now + 1u, 0x00, 0x04, p, n));
    run_for(100u);
    TEST_ASSERT_EQUAL_UINT(0, heard.rx_n);
    TEST_ASSERT_EQUAL_UINT(3, count_s2(ZW_S2_EV_WRONG_LEVEL));
}

/* ZW-007: the panel restarts (a fresh zw_net: its SPANs were in RAM): the sensor's next report
 * fails to decrypt, the panel's Nonce Report brings it again on a new SPAN - delivered once. */
static void test_panel_restart_resync(void)
{
    const zw_incl_event_t *e = join();
    expect_secure(e, ZW_INCL_SEC_S2_ACCESS, 0x07);
    uint8_t cmd[16];
    size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u); /* a SPAN in use */
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    known[0].node = 2u;
    known[0].granted = 0x07u;
    known_n = 1u;
    panel_boot();
    n = door(cmd, 0x17);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(2, heard.rx_n);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, heard.rx[1].security);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cmd, heard.rx_cmd[1], n);
    TEST_ASSERT_EQUAL_UINT(1, count_s2(ZW_S2_EV_DECRYPT_FAILED));
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&dev.s2, FAKE_ZDEV_PANEL)->resends);
}

/* ZW-007: the sensor restarts and forgets its SPAN: the panel's command is not decoded, the
 * sensor's Nonce Report makes the panel send it once more. */
static void test_device_restart(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    supports_battery();
    uint8_t cmd[16];
    const size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u); /* a SPAN in use */
    fake_zdev_restart(&dev);
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    const size_t before = dev.got_n;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 0x77u));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, dev.got_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(BATTERY_GET, dev.got[before], 2);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, dev.got_sec[before]);
    TEST_ASSERT_EQUAL_UINT(1, heard.done_n);
    TEST_ASSERT_EQUAL_UINT32(0x77u, heard.done_tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, heard.done_r[0]);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(zw_net_s2(&net), 2u)->resends);
}

/* ZW-007: the module restarts by itself: the link comes up again on the same network, and the
 * SPANs and nodes stay - the sensor's next report needs no resync. */
static void test_module_restart_keeps_spans(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    uint8_t cmd[16];
    size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u);
    const unsigned ups = heard.ups;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, wire.now + 10u, 0x03u));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(ups + 1u, heard.ups);
    n = door(cmd, 0x17);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(2, heard.rx_n);
    TEST_ASSERT_EQUAL_UINT(0, count_s2(ZW_S2_EV_DECRYPT_FAILED));
}

/* ZW-007, ZW-037: a frame the sensor does not hear goes again (serial-api.md §6.12), each time a
 * new frame - a Nonce Get, or the command on the SPAN's next nonce: two lost are bridged by the
 * third transmission, which it takes once; three lost fail the send (the module's callback says
 * NoAck each time). A Nonce Report that never comes fails the
 * send 1 s plus the round trip after its Nonce Get went. */
static void test_undelivered_and_no_nonce(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    supports_battery();
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    const zw_s2_node_stats_t *st = zw_s2_node_stats(zw_net_s2(&net), 2u);
    unsigned frames = count_heard(0x13, 2, -1);
    const size_t got = dev.got_n;
    dev.lose_in = 2u; /* its first two Nonce Gets (the SPAN is still the temporary key's) */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 1u));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.done_n);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, heard.done_r[0]);
    TEST_ASSERT_EQUAL_UINT(frames + 4u, count_heard(0x13, 2, -1)); /* 3 Nonce Gets, the command */
    TEST_ASSERT_EQUAL_UINT(2, st->retries);
    TEST_ASSERT_EQUAL_UINT(got + 1u, dev.got_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(BATTERY_GET, dev.got[got], 2);

    frames = count_heard(0x13, 2, -1);
    dev.lose_in = 2u; /* the command itself, twice */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 2u));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(2, heard.done_n);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, heard.done_r[1]);
    TEST_ASSERT_EQUAL_UINT(frames + 3u, count_heard(0x13, 2, -1));
    TEST_ASSERT_EQUAL_UINT(4, st->retries);
    TEST_ASSERT_EQUAL_UINT(got + 2u, dev.got_n); /* once, on the SPAN's third nonce from here */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, dev.got_sec[got + 1u]);
    TEST_ASSERT_EQUAL_UINT(0, st->resends);      /* no resync needed */

    frames = count_heard(0x13, 2, -1);
    dev.lose_in = 3u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 3u));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(3, heard.done_n);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, heard.done_r[2]);
    TEST_ASSERT_EQUAL_UINT(frames + 3u, count_heard(0x13, 2, -1)); /* 3 transmissions, no more */
    TEST_ASSERT_EQUAL_UINT(6, st->retries);
    TEST_ASSERT_EQUAL_UINT(got + 2u, dev.got_n);

    dev.lose_out = 1u; /* its Nonce Report */
    zw_s2_forget_span(zw_net_s2(&net), 2u); /* the next send asks for a nonce */
    const uint64_t t0 = wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 4u));
    /* the Nonce Get's callback at +10 ms, then 1000 ms and the 10 ms round trip */
    run(t0 + 10u + NONCE_WAIT_MS + 10u - 1u);
    TEST_ASSERT_EQUAL_UINT(3, heard.done_n);
    run(t0 + 10u + NONCE_WAIT_MS + 10u);
    TEST_ASSERT_EQUAL_UINT(4, heard.done_n);
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_NONCE, heard.done_r[3]);
}

/* ZW-037, ZW-006: a KEX Set the sensor does not hear goes again at once (serial-api.md §6.12:
 * the key exchange's frames get the 3 transmissions too) - the inclusion does not wait out the
 * 10 s TA timer and end "Added without encryption": it ends at the class asked for, well within
 * TA of the loss. */
static void test_lost_kex_set_sent_again(void)
{
    lose_kex_set = true;
    start_and_press();
    run_for(20000u);
    const zw_incl_event_t *g = find(ZW_INCL_EV_GRANT);
    TEST_ASSERT_NOT_NULL(g);
    expect_secure(done_event(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    TEST_ASSERT_EQUAL_UINT(2, sends_of(2u, ZW_S2_CC, ZW_S2_KEX_SET)); /* lost, then heard */
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(zw_net_s2(&net), 2u)->retries);
    TEST_ASSERT_EQUAL_UINT(0, count_s2(ZW_S2_EV_DECRYPT_FAILED));
}

/* ZW-037, ZW-006: the first Network Key Report (under the temporary key) never reaches the
 * sensor: it goes again on the temporary SPAN's next nonce, the sensor takes it and verifies, and
 * the exchange ends with every key granted - within TA of the loss, not after it. */
static void test_lost_network_key_report_sent_again(void)
{
    start_and_press();
    const uint64_t limit = wire.now + 20000u;
    while (dev.step != FAKE_KEX_VERIFY && wire.now < limit) {
        run_for(1u); /* until the sensor has asked for its first key */
    }
    TEST_ASSERT_EQUAL_INT(FAKE_KEX_VERIFY, dev.step);
    TEST_ASSERT_EQUAL_HEX8(0x00, dev.have);
    const uint64_t lost_at = wire.now;
    dev.lose_in = 1u; /* the panel's next frame: the Network Key Report */
    run_for(20000u);
    expect_secure(done_event(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    TEST_ASSERT_TRUE(heard.done_at - lost_at < TA_MS);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(zw_net_s2(&net), 2u)->retries);
}

/* ZW-037: a No Operation goes once - its owner counts its own attempts. "Remove a failed device"
 * pings a sensor that hears nothing 3 times 2 s apart (network.md §10): 3 SendData frames, not
 * 3 each. */
static void test_pings_not_sent_again(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    const unsigned before = count_heard(0x13, 2, 1); /* SendData to node 2, a 1-byte command */
    dev.lose_in = 1000u;
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_failed(&net, wire.now, true, 2u));
    run_for(3u * 2000u + 500u);
    TEST_ASSERT_EQUAL_UINT(before + 3u, count_heard(0x13, 2, 1));
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(zw_net_s2(&net), 2u)->retries);
    run_for(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_IDLE, zw_remove_state(zw_net_rm(&net)));
}

/* ZW-007, ZW-037: a frame sent again after a Nonce Report ends with its own callbacks - the first
 * one's, coming late (the sensor answered before the module did), is not taken for it: the resend
 * is lost 3 times (sent again after each NoAck, serial-api.md §6.12), and the send fails. */
static void test_stale_send_end_ignored(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    supports_battery();
    uint8_t cmd[16];
    const size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u); /* a SPAN in use */
    fake_zdev_restart(&dev);
    zac.radio_ms = 40u; /* heard at 20 ms, its callback at 40 */
    zac.reply_ms = 5u;  /* the sensor's Nonce Report at 25 ms */
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    const uint64_t t0 = wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 0x79u));
    run(t0 + 21u); /* the first frame heard (and not decoded) */
    dev.lose_in = 3u;
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.done_n);
    TEST_ASSERT_EQUAL_UINT32(0x79u, heard.done_tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, heard.done_r[0]);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(zw_net_s2(&net), 2u)->resends);
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_node_stats(zw_net_s2(&net), 2u)->retries);
}

/* ZW-007: nonce reports go in the reply class, ahead of node commands already queued. */
static void test_nonce_reports_overtake(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    fake_rule_t slow;
    memset(&slow, 0, sizeof slow);
    slow.func = 0x60; /* RequestNodeInfo, 3 s to answer */
    slow.respond = true;
    slow.response.len = 1;
    slow.response.bytes[0] = 0x01;
    slow.response.after_ms = 3000u;
    TEST_ASSERT_TRUE(fake_zac93_on(&zac, &slow));
    static const uint8_t NODE9[1] = {9};
    for (uint32_t i = 0; i < 2u; i++) {
        zw_request_t r;
        memset(&r, 0, sizeof r);
        r.func = 0x60;
        r.payload = NODE9;
        r.payload_len = 1u;
        r.expect = ZW_EXPECT_RESPONSE;
        r.prio = ZW_PRIO_NODE;
        r.tag = 20u + i;
        TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, zw_host_submit(zw_net_host(&net), &r));
    }
    run_for(100u); /* the first one in flight, the second queued */
    uint8_t cmd[16];
    const size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(8000u);
    size_t first = 0, second = 0, report = 0;
    unsigned sixties = 0;
    for (size_t i = 0; i < zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&zac, i);
        if (h == NULL) {
            continue;
        }
        if (h->func == 0x60) {
            ++sixties;
            first = sixties == 1u ? i : first;
            second = sixties == 2u ? i : second;
        }
        /* the Nonce Report answering this report's Nonce Get (not one of the join's) */
        if (sixties > 0u && report == 0u && h->func == 0x13 && h->payload_len > 3u &&
            h->payload[2] == 0x9F && h->payload[3] == ZW_S2_NONCE_REPORT) {
            report = i;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(first > 0u && second > first && report > first, "not all heard");
    TEST_ASSERT_TRUE_MESSAGE(report < second, "the nonce report waited behind a node command");
}

/* ZW-007: 4 lost reports are bridged by the 5 nonce attempts; a repeated frame is dropped. */
static void test_lost_and_repeated_frames(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    uint8_t cmd[16];
    const size_t n = door(cmd, 0x16);
    dev.lose_out = 4u;
    for (unsigned i = 0; i < 5u; i++) {
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
        run_for(1000u);
    }
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(zw_net_s2(&net), 2u)->decrypt_failures);
    dev.repeat_out = 1u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(2, heard.rx_n);
    TEST_ASSERT_EQUAL_UINT(1, count_s2(ZW_S2_EV_DUPLICATE));
}

/* ZW-007: the sensor's report crosses the panel's command on the same SPAN position: both are
 * taken, without a resync. */
static void test_crossed_frames(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    /* a SPAN in use both ways first */
    uint8_t cmd[16];
    const size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(1000u);
    const uint32_t reports = zw_s2_node_stats(zw_net_s2(&net), 2u)->nonce_reports;
    dev.cross_next = true;
    dev.cross_len = door(dev.cross, 0x17);
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    const size_t before = dev.got_n;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&net, wire.now, 2u, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 0x78u));
    run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, dev.got_n);
    TEST_ASSERT_EQUAL_UINT(2, heard.rx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(dev.cross, heard.rx_cmd[1], dev.cross_len);
    TEST_ASSERT_EQUAL_UINT32(reports, zw_s2_node_stats(zw_net_s2(&net), 2u)->nonce_reports);
    TEST_ASSERT_EQUAL_UINT(0, count_s2(ZW_S2_EV_DECRYPT_FAILED));
}

/* ---- Scenarios from Z-Wave JS's test suite (hardening) ------------------------------------- */

/* The panel's Nonce Reports the module heard since frame `from` (SendData carrying 9F 02), and the
 * transmit options of the last one. */
static unsigned nonce_reports_since(size_t from, uint8_t *tx_options)
{
    unsigned n = 0;
    for (size_t i = from; i < zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&zac, i);
        if (h != NULL && h->func == 0x13 && h->payload_len > 3u && h->payload[2] == 0x9F &&
            h->payload[3] == ZW_S2_NONCE_REPORT) {
            n++;
            const size_t at = 2u + (size_t)h->payload[1]; /* node, length, command, options */
            if (tx_options != NULL && at < h->payload_len) {
                *tx_options = h->payload[at];
            }
        }
    }
    return n;
}

/* ZW-007: a Nonce Report the sensor never hears does not wedge it. Mirrors Z-Wave JS's
 * nodeAsleepBlockNonceReport.test.ts (their node used S0; this one S2). The sensor restarted, so
 * its next report asks for a nonce first; then it goes quiet before the panel's Nonce Report
 * reaches it - the module's callback says NoAck. The panel sent it once (security.md §6: one
 * attempt, transmit options 05) and sends nothing more on its own; the sensor's report fails on its
 * side, unheard. At its next report the sensor asks again: that Nonce Get is answered at once, and
 * the report is taken at its class, without a resync. */
static void test_undelivered_nonce_report_does_not_wedge(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    fake_zdev_restart(&dev);
    const size_t from = zac.heard;
    dev.lose_in = 1u; /* the panel's next frame, the Nonce Report, never reaches the sensor */
    uint8_t cmd[16];
    size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(5000u);
    uint8_t options = 0u;
    TEST_ASSERT_EQUAL_UINT(1, nonce_reports_since(from, &options));
    TEST_ASSERT_EQUAL_HEX8(ZW_TX_OPT_NONCE_REPORT, options);
    TEST_ASSERT_EQUAL_UINT(0, heard.rx_n);

    n = door(cmd, 0x17);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_AUTO, cmd, n));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(2, nonce_reports_since(from, NULL));
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, heard.rx[0].security);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cmd, heard.rx_cmd[0], n);
    TEST_ASSERT_EQUAL_UINT(0, count_s2(ZW_S2_EV_DECRYPT_FAILED));
}

/* ZW-015 at the library level, ZW-009: a device whose node information lists Security 2 but that
 * joins without security (here the installer said no at the grant) holds no class at all - never a
 * partial one. Mirrors Z-Wave JS's highestSecurityClass.test.ts ("An insecurely-included node that
 * supports Security S2 has security class None", their issue #6098). Its granted classes are none,
 * its highest is none, its stored record says so (00, not FF "not yet known") with no DSK; §8
 * chooses "unencrypted" for every CC to it, and a named class is refused. What it sends: a report
 * in the clear counts as unencrypted; an S2 frame - even under this network's own key, which such a
 * device could only have taken from somewhere - is dropped, and nothing of it reaches the owner. */
static void test_insecure_join_of_s2_device_has_no_class(void)
{
    cancel_at_grant = true;
    start_and_press();
    run_for(20000u);
    expect_insecure(ZW_INCL_SEC_FAILED, ZW_INCL_S2_CANCELLED, 0);
    const uint16_t node = done_event()->node;
    zw_s2_t *s2 = zw_net_s2(&net);
    TEST_ASSERT_EQUAL_HEX8(0x00u, zw_s2_granted(s2, node));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_highest(s2, node));
    static const uint8_t CCS[] = {0x71, 0x80, 0x84, 0x86, 0x5E, 0x6C, 0x60, 0x9F};
    for (size_t i = 0; i < sizeof CCS; i++) {
        TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, CCS[i]));
    }
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_KEY, zw_net_send(&net, wire.now, node, ZW_SEC_S2_UNAUTH,
                                                    BATTERY_GET, sizeof BATTERY_GET, 1u));
    run_for(100u);
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, stored_nodes, stored_nodes_len));
    const zw_node_record_t *rec = zw_node_find_const(&back, node);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_HEX8(0x00u, rec->security_classes);
    TEST_ASSERT_FALSE(rec->has_dsk);

    uint8_t cmd[16];
    size_t n = door(cmd, 0x16);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_NONE, cmd, n));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, heard.rx[0].security);

    TEST_ASSERT_TRUE(zw_s2_set_key(&dev.s2, ZW_S2_KEY_ACCESS, cfg.keys.s2_access));
    TEST_ASSERT_TRUE(zw_s2_set_granted(&dev.s2, FAKE_ZDEV_PANEL, ZW_S2_KEY_ACCESS));
    n = door(cmd, 0x17);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, wire.now, ZW_SEC_S2_ACCESS, cmd, n));
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_TRUE(count_s2(ZW_S2_EV_NO_KEY) >= 1u);
    TEST_ASSERT_EQUAL_HEX8(0x00u, zw_s2_granted(s2, node));
}

/* ZW-023, ZW-009: Z-Wave JS's invalidPayloadLog.test.ts frame 010800040021043003e5 reaching the
 * panel - its checksum right, its command length running past its end: zw_link ACKs it (the module
 * must not send it again), zw_net counts it as malformed and hands nothing on; the link is untouched
 * and the sensor's next frame is taken as usual. */
static void test_truncated_command_counted(void)
{
    expect_secure(join(), ZW_INCL_SEC_S2_ACCESS, 0x07);
    static const uint8_t RAW[] = {0x01, 0x08, 0x00, 0x04, 0x00, 0x21, 0x04, 0x30, 0x03, 0xE5};
    const uint32_t acks = wire.acks;
    TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&zac, wire.now + 1u, RAW, sizeof RAW));
    run_for(100u);
    TEST_ASSERT_EQUAL_UINT32(acks + 1u, wire.acks);
    TEST_ASSERT_EQUAL_UINT32(1, zw_net_stats(&net)->malformed);
    TEST_ASSERT_EQUAL_UINT(0, heard.rx_n);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&net)));
    /* a whole one after it is taken: a bare Security 2 command, which an S2 node may send in the
     * clear (ZW-009, Z-Wave JS isSecurityLevelTooLow) */
    static const uint8_t CSR[] = {0x9F, 0x0E, 0x71};
    const zw_api_app_cmd_t ok = {false, 0x00, 0u, 2u, CSR, sizeof CSR, NULL, 0u, false, 0};
    uint8_t p[32];
    const size_t n = zw_api_app_cmd_build(p, sizeof p, ZW_NID_8, &ok);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, wire.now + 1u, 0x00, 0x04, p, n));
    run_for(100u);
    TEST_ASSERT_EQUAL_UINT(1, heard.rx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CSR, heard.rx_cmd[0], sizeof CSR);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_join_s2_access_control);
    RUN_TEST(test_join_s2_authenticated);
    RUN_TEST(test_join_s2_unauthenticated);
    RUN_TEST(test_installer_grants_less);
    RUN_TEST(test_empty_node_information);
    RUN_TEST(test_join_without_s2);
    RUN_TEST(test_refusals_before_start);
    RUN_TEST(test_module_skips_ready);
    RUN_TEST(test_radio_does_not_listen);
    RUN_TEST(test_window_and_keep_listening);
    RUN_TEST(test_installer_stops);
    RUN_TEST(test_controller_refused);
    RUN_TEST(test_status_failed);
    RUN_TEST(test_protocol_timeouts);
    RUN_TEST(test_node_id_refusals);
    RUN_TEST(test_route_failure_still_joins);
    RUN_TEST(test_wrong_pin);
    RUN_TEST(test_each_timer);
    RUN_TEST(test_kex_fail_from_device);
    RUN_TEST(test_kex_report_refused);
    RUN_TEST(test_grant_refusals);
    RUN_TEST(test_tai2_from_public_key);
    RUN_TEST(test_late_pin_joins);
    RUN_TEST(test_one_damaged_echo_is_not_a_wrong_pin);
    RUN_TEST(test_wrong_key_requests);
    RUN_TEST(test_installer_cancels);
    RUN_TEST(test_downgrades_detected);
    RUN_TEST(test_bad_kex_messages_refused);
    RUN_TEST(test_link_down_during_key_exchange);
    RUN_TEST(test_restart_ends_add_window);
    RUN_TEST(test_recovery_ends_add_window);
    RUN_TEST(test_restart_ends_remove_window);
    RUN_TEST(test_restart_after_join_continues);
    RUN_TEST(test_restart_before_ready);
    RUN_TEST(test_restart_after_protocol_done_ends);
    RUN_TEST(test_frames_dropped_by_net);
    RUN_TEST(test_panel_restart_resync);
    RUN_TEST(test_module_restart_keeps_spans);
    RUN_TEST(test_device_restart);
    RUN_TEST(test_undelivered_and_no_nonce);
    RUN_TEST(test_lost_kex_set_sent_again);
    RUN_TEST(test_lost_network_key_report_sent_again);
    RUN_TEST(test_pings_not_sent_again);
    RUN_TEST(test_stale_send_end_ignored);
    RUN_TEST(test_nonce_reports_overtake);
    RUN_TEST(test_lost_and_repeated_frames);
    RUN_TEST(test_crossed_frames);
    RUN_TEST(test_undelivered_nonce_report_does_not_wedge);
    RUN_TEST(test_insecure_join_of_s2_device_has_no_class);
    RUN_TEST(test_truncated_command_counted);
    return UNITY_END();
}
