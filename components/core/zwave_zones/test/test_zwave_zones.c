/*
 * test_zwave_zones.c - Z-Wave devices as zones end to end (docs/features/36, milestone Z6): the
 * panel's network (zw_net) against zwave_host's fake ZAC93 and simulated devices with their own
 * Security 2, the bridge, the zone source and the real hub, whose zone events are recorded, on a
 * fake clock. The owner here is what the simulator and zwave_service are: it keeps the node cache,
 * the keys and the bindings across a restart, grants what devices ask for, types PINs, and passes
 * zw_net's news to the bridge.
 *
 * Requirements: ZW-004 (restarting the radio leaves the zones be), ZW-010 (no lifeline, no
 * inputs; the quirks table's "no Supervision"), ZW-012 (a quiet listening device's pings are its
 * check-ins; a confirmed check-in's grace), ZW-013 (reports to zone events, Supervision answered
 * with the request's encapsulation), ZW-014 (addresses: generations, a new network), ZW-015 (trust
 * per device) (docs/features/36); ZSRC-007, ZSRC-015 (docs/features/22).
 */
#include <string.h>

#include "fake_zac93.h"
#include "fake_zdev.h"
#include "unity.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zwave.h"
#include "zw_host_crypto.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_zones/zwave_zones.h"

#define MIN_MS 60000u
#define HOUR_MS 3600000u

/* ---- the world ------------------------------------------------------------------------------- */

typedef struct {
    uint16_t zone;
    alarm_zone_event_t ev;
    uint64_t at;
} zev_t;

typedef struct {
    zw_net_t net;
    fake_zac93_t zac;
    fake_wire_t wire;
    zw_host_crypto_t hc;
    zw_crypto_port_t crypto;
    zw_net_config_t cfg;
    uint8_t nodes[8192]; /* what the owner keeps: the node cache, the bindings */
    size_t nodes_len;
    uint8_t bind[ZONE_HUB_BLOB_MAX];
    size_t bind_len;
    zw_incl_event_t incl[32];
    size_t incl_n;
    zw_net_event_t ev[128];
    size_t ev_n;
    uint16_t pin;
    uint8_t grant;
    zwz_t bridge;
    zsrc_zw_t src;
    zone_hub_t hub;
    zev_t zev[512];
    size_t zev_n;
    unsigned trouble_on, trouble_off;
    uint32_t other_done; /* send_done of tags not the bridge's */
} world_t;

static world_t W;

static void o_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    zwz_link_up(&W.bridge, facts, W.wire.now);
}

static void o_link_down(void *ctx, const zw_link_down_t *why)
{
    (void)ctx;
    (void)why;
    zwz_link_down(&W.bridge, W.wire.now);
}

static void o_restarted(void *ctx, uint8_t reason)
{
    (void)ctx;
    (void)reason;
    zwz_link_down(&W.bridge, W.wire.now);
}

static void o_received(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    zwz_received(&W.bridge, rx, W.wire.now);
}

static void o_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    if (!zwz_send_done(&W.bridge, tag, r)) {
        W.other_done++;
    }
}

static void o_incl(void *ctx, const zw_incl_event_t *ev)
{
    (void)ctx;
    if (W.incl_n < sizeof(W.incl) / sizeof(W.incl[0])) {
        W.incl[W.incl_n] = *ev;
        W.incl[W.incl_n].nif = NULL;
        W.incl_n++;
    }
    zw_incl_t *in = zw_net_incl(&W.net);
    if (ev->kind == ZW_INCL_EV_GRANT) {
        TEST_ASSERT_TRUE(zw_incl_grant(in, W.wire.now, W.grant != 0u ? W.grant : ev->requested));
    } else if (ev->kind == ZW_INCL_EV_PIN) {
        TEST_ASSERT_TRUE(zw_incl_pin(in, W.wire.now, W.pin));
    }
}

static void o_event(void *ctx, const zw_net_event_t *ev)
{
    (void)ctx;
    if (W.ev_n < sizeof(W.ev) / sizeof(W.ev[0])) {
        W.ev[W.ev_n++] = *ev;
    }
    zwz_event(&W.bridge, ev, W.wire.now);
}

static void o_store_nodes(void *ctx, const zw_node_cache_t *nodes, uint16_t generation)
{
    (void)ctx;
    W.nodes_len = zw_node_cache_encode(nodes, W.nodes, sizeof(W.nodes));
    TEST_ASSERT_NOT_EQUAL(0u, W.nodes_len);
    W.cfg.generation = generation;
}

static void o_store_keys(void *ctx, uint32_t home_id, const zw_s2_keys_t *keys)
{
    (void)ctx;
    W.cfg.keys = *keys;
    W.cfg.keys_bound = true;
    W.cfg.keys_home_id = home_id;
}

static bool o_quirk(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out)
{
    (void)ctx;
    return zwz_quirk(&W.bridge, rec, out);
}

static bool o_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&W.wire, bytes, len);
}

/* The hub's port: zone events for the engine, the source's own trouble (sensor_radio). */
static void hub_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    TEST_ASSERT_TRUE(W.zev_n < sizeof(W.zev) / sizeof(W.zev[0]));
    W.zev[W.zev_n].zone = zone;
    W.zev[W.zev_n].ev = ev;
    W.zev[W.zev_n].at = W.wire.now;
    W.zev_n++;
}

static void hub_changed(void *ctx)
{
    (void)ctx;
    const zone_hub_result_t r = zone_hub_encode(&W.hub, W.bind, sizeof(W.bind), &W.bind_len);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, r);
}

static void hub_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZWAVE, kind);
    if (on) {
        W.trouble_on++;
    } else {
        W.trouble_off++;
    }
}

/* The zone source's sink, straight into the hub (zone_service adds its lock). */
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

/* The wire's view of the panel: zw_net, the bridge and the hub, polled together. */
static void n_control(void *ctx, uint64_t now, uint8_t b)
{
    (void)ctx;
    zw_net_on_control(&W.net, now, b);
}

static void n_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    (void)ctx;
    zw_net_on_frame(&W.net, now, f);
}

static void n_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    (void)ctx;
    zw_net_on_text(&W.net, now, b, n);
}

static void n_poll(void *ctx, uint64_t now)
{
    (void)ctx;
    zw_net_poll(&W.net, now);
    zwz_poll(&W.bridge, now);
    zone_hub_tick(&W.hub, now);
}

static uint64_t n_deadline(void *ctx)
{
    (void)ctx;
    uint64_t t = zw_net_next_deadline(&W.net);
    const uint64_t b = zwz_next_deadline(&W.bridge);
    const uint64_t h = zone_hub_next_deadline(&W.hub, W.wire.now);
    t = b < t ? b : t;
    return h < t ? h : t;
}

static void run(uint64_t t)
{
    fake_wire_run_until(&W.wire, t);
    TEST_ASSERT_FALSE_MESSAGE(W.wire.stuck, "the wire did not settle");
}

static void run_for(uint64_t ms)
{
    run(W.wire.now + ms);
}

/* The panel (re)starts: zw_net, the bridge, the source and the hub afresh, with what the owner
 * stored. */
static void boot(void)
{
    static const zw_net_port_t port = {
        .write = o_write,
        .link_up = o_link_up,
        .link_down = o_link_down,
        .restarted = o_restarted,
        .received = o_received,
        .send_done = o_send_done,
        .incl_event = o_incl,
        .event = o_event,
        .store_nodes = o_store_nodes,
        .store_keys = o_store_keys,
        .quirk = o_quirk,
    };
    static const fake_host_t fh = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
    static const zone_hub_port_t hport = {NULL, hub_event, hub_changed, hub_trouble};
    static const zsrc_sink_t sink = {NULL, sink_input, sink_event, sink_reachable, sink_link, NULL};
    static const zw_cc_panel_identity_t id = {0u, 0u, 1u, 5u, 0xFFFFu, 0xFFFFu, 0xFFFFu,
                                              0x00u, 0x00u, 0x0500u, 0x0500u, false,
                                              0u, 0u, 0u, 0u, 0u, 0u};
    const uint64_t t = W.wire.now;
    zw_net_init(&W.net, &port, &W.cfg, &W.crypto);
    if (W.nodes_len > 0u) {
        TEST_ASSERT_TRUE(zw_net_load_nodes(&W.net, W.nodes, W.nodes_len));
    }
    zsrc_zw_init(&W.src, &sink);
    zone_hub_init(&W.hub, &hport);
    if (W.bind_len > 0u) {
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&W.hub, W.bind, W.bind_len));
    }
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&W.hub, zsrc_zw_ops(), &W.src, t));
    zone_hub_start(&W.hub, t);
    zwz_init(&W.bridge, &W.net, &W.src, &id);
    fake_wire_init(&W.wire, &W.zac, &fh, t);
    zw_net_start(&W.net, t);
    run(t + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&W.net)));
}

void setUp(void)
{
    memset(&W, 0, sizeof(W));
    W.cfg.interview = true;
    zw_host_crypto_init(&W.hc, 0x7A6F6E65u);
    W.crypto = zw_host_crypto_port(&W.hc);
    fake_zac93_init(&W.zac);
    boot();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.zac.overflow, "the fake module ran out of room");
}

/* ---- helpers ------------------------------------------------------------------------------- */

static unsigned count(uint16_t zone, alarm_zone_event_t ev)
{
    unsigned n = 0u;
    for (size_t i = 0; i < W.zev_n; i++) {
        n += W.zev[i].zone == zone && W.zev[i].ev == ev;
    }
    return n;
}

static uint64_t last_at(uint16_t zone, alarm_zone_event_t ev)
{
    for (size_t i = W.zev_n; i > 0u; i--) {
        if (W.zev[i - 1u].zone == zone && W.zev[i - 1u].ev == ev) {
            return W.zev[i - 1u].at;
        }
    }
    return 0u;
}

#define EXPECT(z_, e_, n_) TEST_ASSERT_EQUAL_UINT_MESSAGE((n_), count((z_), (e_)), #e_)

/* When zw_health last heard from a device (network.md §8's clock). */
static uint64_t last_heard(uint16_t node)
{
    const zw_health_t *h = zw_net_health(&W.net);
    for (size_t i = 0; i < h->node_count; i++) {
        if (h->nodes[i].node_id == node) {
            return h->nodes[i].last_heard_ms;
        }
    }
    TEST_FAIL_MESSAGE("not supervised");
    return 0u;
}

/* A device that answers its interview and really sleeps when it is a sleeper. */
static void device(fake_zdev_t *d, uint64_t seed, bool sleeps)
{
    fake_zdev_init(d, seed);
    d->m.answers = true;
    d->m.sleeps = sleeps;
}

/* "Add a device": the node it got, 0 when it did not join. */
static uint16_t include(fake_zdev_t *d, uint8_t grant)
{
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, d));
    W.pin = d->pin;
    W.grant = grant;
    const size_t before = W.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&W.net, W.wire.now, true, 2u, 0u));
    run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&W.zac, W.wire.now, d));
    for (unsigned i = 0; i < 400u; i++) {
        run_for(100u);
        for (size_t k = before; k < W.incl_n; k++) {
            if (W.incl[k].kind == ZW_INCL_EV_DONE) {
                return W.incl[k].result == ZW_INCL_ADDED ? W.incl[k].node : 0u;
            }
        }
    }
    TEST_FAIL_MESSAGE("the inclusion did not end");
    return 0u;
}

/* Runs until the zone source offers the device's inputs. */
static const zsrc_zw_device_t *until_ready(uint16_t node)
{
    for (unsigned i = 0; i < 600u; i++) {
        const zsrc_zw_device_t *d = zsrc_zw_find(&W.src, node);
        if (d != NULL && d->ready) {
            return d;
        }
        run_for(100u);
    }
    TEST_FAIL_MESSAGE("the device never offered its inputs");
    return NULL;
}

/* The candidate for an input of a node, or NULL. */
static const zsrc_candidate_t *candidate(uint16_t node, uint8_t ep, uint8_t slot)
{
    static zsrc_candidate_t c[128];
    const size_t n = zone_hub_candidates(&W.hub, ZSRC_ZWAVE, NULL, 0u, c, 128u);
    for (size_t i = 0; i < n; i++) {
        uint32_t home = 0u;
        uint16_t gen = 0u;
        uint16_t nd = 0u;
        uint8_t e = 0u;
        uint8_t s = 0u;
        if (zsrc_zw_address_parse(&c[i].addr, &home, &gen, &nd, &e, &s) && nd == node &&
            e == ep && s == slot) {
            return &c[i];
        }
    }
    return NULL;
}

static void bind(uint16_t zone, const zsrc_candidate_t *c)
{
    TEST_ASSERT_NOT_NULL(c);
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = c->addr;
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, (alarm_device_class_t)c->device_class);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&W.hub, zone, &b, W.wire.now));
}

static void say(fake_zdev_t *d, const uint8_t *cmd, size_t len)
{
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(d, W.wire.now, ZW_SEC_AUTO, cmd, len));
    run_for(1000u);
}

static void notification(fake_zdev_t *d, uint8_t ep, uint8_t type, uint8_t event)
{
    const uint8_t n[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, type, event, 0x00};
    if (ep == 0u) {
        say(d, n, sizeof(n));
        return;
    }
    uint8_t mc[24];
    const size_t k = zw_cc_wrap_multi_channel(mc, sizeof(mc), ep, 0u, false, n, sizeof(n));
    say(d, mc, k);
}

static zone_hub_info_t info(uint16_t zone)
{
    zone_hub_info_t i;
    TEST_ASSERT_TRUE(zone_hub_info(&W.hub, zone, &i));
    return i;
}

/* A mains leak sensor without Security 2 (its commands in the clear). */
static void plain_listener(fake_zdev_t *d, uint64_t seed)
{
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71};
    device(d, seed, false);
    fake_zdev_listening(d);
    fake_zdev_no_s2(d);
    d->m.secure_n = 0u;
    memcpy(d->nif, NIF, sizeof(NIF));
    d->nif_len = sizeof(NIF);
    d->m.notif_types[0] = 0x05u;
    d->m.notif_n = 1u;
}

/* Where these bytes are in the first SendData from the module's log entry `from` on that carries
 * them (a command in the clear), or NULL; `index`: that entry. */
static const uint8_t *sent_at(size_t from, const uint8_t *bytes, size_t n, size_t *index)
{
    for (size_t i = from; i < W.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&W.zac, i);
        if (h == NULL || h->func != ZW_FUNC_SEND_DATA) {
            continue;
        }
        const size_t len =
            h->payload_len < sizeof(h->payload) ? h->payload_len : sizeof(h->payload);
        for (size_t k = 0; k + n <= len; k++) {
            if (memcmp(&h->payload[k], bytes, n) == 0) {
                if (index != NULL) {
                    *index = i;
                }
                return &h->payload[k];
            }
        }
    }
    return NULL;
}

static size_t sent(size_t from, const uint8_t *bytes, size_t n)
{
    size_t i = SIZE_MAX;
    (void)sent_at(from, bytes, n, &i);
    return i;
}

/* ---- the bridge's outbox (network.md section 7) ---------------------------------------------- */

/* A Get of this device's fresh state is out in the bridge's outbox: handed to zw_net, its end not
 * yet heard (for a sleeper: held in its wake-up queue). */
static bool get_held(uint16_t node)
{
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        const zwz_out_t *o = &W.bridge.out[i];
        if (o->used && o->refresh && o->out && o->node == node) {
            return true;
        }
    }
    return false;
}

/* An answer to this device is out: handed to zw_net, its end not yet heard. */
static bool reply_out(uint16_t node)
{
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        const zwz_out_t *o = &W.bridge.out[i];
        if (o->used && !o->refresh && o->out && o->node == node) {
            return true;
        }
    }
    return false;
}

static size_t gets_held(void)
{
    size_t n = 0u;
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        n += W.bridge.out[i].used && W.bridge.out[i].refresh && W.bridge.out[i].out ? 1u : 0u;
    }
    return n;
}

/* A door contact's open, inside a Supervision Get of `session`. */
static size_t supervised_open(uint8_t *out, size_t cap, uint8_t session)
{
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    return zw_cc_wrap_supervision(out, cap, session, false, OPEN, sizeof(OPEN));
}

/* The device says this; runs the wire in 1 ms steps for `ms`: true when, at some step, its
 * answer was out while a Get of its fresh state was still held (the answer did not wait for it). */
static bool say_and_watch(fake_zdev_t *d, const uint8_t *cmd, size_t len, uint64_t ms)
{
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(d, W.wire.now, ZW_SEC_AUTO, cmd, len));
    bool both = false;
    for (uint64_t t = 0u; t < ms; t++) {
        run_for(1u);
        both = both || (reply_out(d->node) && get_held(d->node));
    }
    return both;
}

/* A battery-less sleeping door without Security 2: one Notification type, so one Get to ask. */
static void plain_sleeper(fake_zdev_t *d, uint64_t seed)
{
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x84};
    device(d, seed, true);
    fake_zdev_no_s2(d);
    d->m.secure_n = 0u;
    memcpy(d->nif, NIF, sizeof(NIF));
    d->nif_len = sizeof(NIF);
    d->m.notif_types[0] = 0x06u;
    d->m.notif_n = 1u;
}

/* ---- the tests ------------------------------------------------------------------------------ */

/* ZW-013, ZW-015, the Z6 "Done when": a secure S2 door sensor joins, is interviewed and offered
 * as a door contact named after its model; bound to a zone, its open and close, tamper and low
 * battery reach the zone; it is trusted for fire (Security 2, checking in); when it stops
 * checking in, network.md §8 says it is unreachable T + 10 min after its last frame and the zone
 * shows CHECK after its grace, another T + 10 min. */
static void test_secure_door_sensor(void)
{
    static fake_zdev_t door;
    device(&door, 0xD00Au, true);
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    const zsrc_zw_device_t *d = until_ready(node);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_SEC_S2_AUTH, d->security);
    TEST_ASSERT_EQUAL_UINT32(3600u, d->check_in_s);
    TEST_ASSERT_FALSE(d->listening);
    const zsrc_candidate_t *c = candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor", c->name);
    const uint32_t home = zw_host_facts(zw_net_host(&W.net))->home_id; /* the network's */
    TEST_ASSERT_EQUAL_HEX32(home, (uint32_t)(c->addr.id >> 32));
    TEST_ASSERT_EQUAL_STRING("door contact", c->detail);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, c->device_class);
    bind(1u, c);
    const zone_hub_info_t i = info(1u);
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor · door contact", i.address);
    TEST_ASSERT_TRUE((i.caps & ZSRC_CAP_AUTH) != 0u);
    TEST_ASSERT_TRUE(zsrc_trusted_for(i.caps, ALARM_ZT_FIRE));
    /* open, closed, tamper, low battery */
    notification(&door, 0u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    notification(&door, 0u, 0x06u, 0x17u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    notification(&door, 0u, 0x07u, 0x03u);
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    notification(&door, 0u, 0x07u, 0x00u);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    static const uint8_t LOW[] = {0x80, 0x03, 0x0A};
    say(&door, LOW, sizeof(LOW));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    /* it stops checking in: unreachable T + 10 min after its last frame, CHECK after its grace
     * (T + 10 min more, network.md §8) - to the millisecond */
    door.m.auto_wake = false;
    const uint64_t heard = last_heard(node);
    run(heard + 4200u * 1000u - 1u);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    run(heard + 4200u * 1000u);
    TEST_ASSERT_TRUE(info(1u).unreachable);
    run(heard + 2u * 4200u * 1000u - 1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    run(heard + 2u * 4200u * 1000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    TEST_ASSERT_EQUAL_UINT64(heard + 2u * 4200u * 1000u, last_at(1u, ALARM_ZEV_UNREACHABLE));
    /* it wakes again: REACHABLE */
    door.m.auto_wake = true;
    fake_zdev_wake(&door, W.wire.now);
    run_for(3000u);
    EXPECT(1u, ALARM_ZEV_REACHABLE, 1u);
    TEST_ASSERT_FALSE(info(1u).check);
}

/* ZW-012, network.md §7-8: a door sensor's own check-in lengthened from 1 h to 12 h - its zone's
 * grace stays 1 h + 10 min until the device confirms it at its wake-up, then follows at once (not
 * only at the next network check or interview), and its inputs are not asked again for it. When
 * it then stops checking in it is unreachable 12 h + 10 min after its last frame, and its zone is
 * not in CHECK 1 h + 10 min (and a minute) later - only after 12 h + 10 min, to the millisecond. */
static void test_confirmed_check_in_moves_the_grace(void)
{
    static fake_zdev_t door;
    device(&door, 0xD00Bu, true);
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    const zsrc_zw_device_t *d = until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    notification(&door, 0u, 0x06u, 0x17u); /* closed */
    run_for(MIN_MS);                       /* asleep */
    door.m.auto_wake = false;              /* it wakes when the test says */
    TEST_ASSERT_EQUAL_UINT32(3600u, d->check_in_s);
    const zsrc_address_t a = candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT)->addr;
    TEST_ASSERT_EQUAL_UINT32(3600u + 600u, zsrc_zw_ops()->grace_of(&W.src, &a));
    const uint32_t gets = W.bridge.stats.gets;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&W.net, W.wire.now, true, node, 12u * 3600u));
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT32(3600u, d->check_in_s); /* not before the device says so */
    const size_t events = W.ev_n;
    fake_zdev_wake(&door, W.wire.now);
    run_for(3000u);
    bool took = false;
    for (size_t i = events; i < W.ev_n; i++) {
        took = took || (W.ev[i].kind == ZW_NET_EV_CHECK_IN && W.ev[i].took);
    }
    TEST_ASSERT_TRUE(took);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u + 600u, zsrc_zw_ops()->grace_of(&W.src, &a));
    TEST_ASSERT_EQUAL_UINT32(gets, W.bridge.stats.gets); /* nothing asked again */
    /* it stops checking in */
    const uint64_t heard = last_heard(node);
    const uint64_t t = (12u * 3600u + 600u) * 1000u;
    run(heard + t - 1u);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    run(heard + t);
    TEST_ASSERT_TRUE(info(1u).unreachable);
    run(heard + t + (3600u + 600u) * 1000u + MIN_MS); /* the old grace and a minute */
    TEST_ASSERT_FALSE(info(1u).check);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    run(heard + 2u * t - 1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    run(heard + 2u * t);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    TEST_ASSERT_TRUE(info(1u).check);
}

/* A secure door sensor, bound to zone 1, asleep: it wakes only when the test says. Its last frame
 * (network.md §8's clock) in *heard. */
static uint16_t sleeping_door(fake_zdev_t *door, uint64_t seed, const zsrc_zw_device_t **d,
                              zsrc_address_t *a, uint64_t *heard)
{
    device(door, seed, true);
    const uint16_t node = include(door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    *d = until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    notification(door, 0u, 0x06u, 0x17u); /* closed */
    run_for(MIN_MS);                      /* asleep */
    door->m.auto_wake = false;
    *a = candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT)->addr;
    *heard = last_heard(node);
    return node;
}

/* The CHECK_IN events (took) for a node since event `from`, the last one's interval in *s. */
static unsigned checkins_took(size_t from, uint16_t node, uint32_t *s)
{
    unsigned n = 0u;
    for (size_t i = from; i < W.ev_n; i++) {
        if (W.ev[i].kind == ZW_NET_EV_CHECK_IN && W.ev[i].node == node && W.ev[i].took) {
            n++;
            *s = W.ev[i].check_in_s;
        }
    }
    return n;
}

/* ZW-009, as Z-Wave JS Driver.ts shouldDiscardCC (`node.getEndpoint(i) ?? node`): a device
 * whose root supports Multilevel Sensor, with one end point whose list (without it) is recorded.
 * A Multilevel Sensor Report from an end point the record does not hold - past the record's end
 * points (5), or one never described (2) - is judged by the device's own list: it reaches the
 * zones. From end point 1, whose list lacks the CC, it is discarded. */
static void test_meter_sensor_support_falls_back_to_the_node(void)
{
    static fake_zdev_t multi;
    device(&multi, 0xC0E1u, false);
    fake_zdev_listening(&multi);
    multi.m.versions[multi.m.versions_n].cc = 0x60u;
    multi.m.versions[multi.m.versions_n].version = 4u;
    multi.m.versions_n++;
    multi.m.versions[multi.m.versions_n].cc = 0x31u;
    multi.m.versions[multi.m.versions_n].version = 5u;
    multi.m.versions_n++;
    multi.m.secure_ccs[multi.m.secure_n++] = 0x60u;
    multi.m.secure_ccs[multi.m.secure_n++] = 0x31u;
    multi.m.notif_n = 0u;
    multi.m.ep_n = 1u;
    fake_ep_t *ep = &multi.m.eps[0];
    ep->generic = 0x07u;
    ep->specific = 0x01u;
    ep->ccs[0] = 0x5Eu;
    ep->cc_n = 1u;
    ep->secure_ccs[0] = 0x71u;
    ep->secure_n = 1u;
    ep->notif_types[0] = 0x06u;
    ep->notif_n = 1u;
    const uint16_t node = include(&multi, 0u);
    until_ready(node);
    run_for(5000u);
    const zw_node_record_t *rec = zw_node_find_const(&W.net.nodes, node);
    TEST_ASSERT_TRUE(rec->endpoints[1].has_class);
    TEST_ASSERT_FALSE(rec->endpoints[2].has_class);
    static const uint8_t AIR[] = {0x31, 0x05, 0x01, 0x22, 0x00, 0xE6};
    uint8_t mc[24];
    const uint8_t eps[3] = {5u, 2u, 1u};
    for (size_t i = 0; i < 3u; i++) {
        const uint32_t unsupported = zw_net_stats(&W.net)->unsupported_cc;
        const uint32_t reports = W.bridge.stats.reports;
        const size_t k = zw_cc_wrap_multi_channel(mc, sizeof(mc), eps[i], 0u, false, AIR,
                                                  sizeof(AIR));
        say(&multi, mc, k);
        if (eps[i] == 1u) {
            TEST_ASSERT_EQUAL_UINT32(unsupported + 1u, zw_net_stats(&W.net)->unsupported_cc);
            TEST_ASSERT_EQUAL_UINT32(reports, W.bridge.stats.reports);
        } else {
            TEST_ASSERT_EQUAL_UINT32(unsupported, zw_net_stats(&W.net)->unsupported_cc);
            TEST_ASSERT_EQUAL_UINT32(reports + 1u, W.bridge.stats.reports);
        }
    }
}

/* ZW-009, the panel's storage-limit fallback (security.md "Where the panel still differs": the
 * record holds 12 CCs per end point): an end point whose recorded list is full and lacks
 * Multilevel Sensor is judged by the device's own list, which has it - its report reaches the zones
 * (Z-Wave JS would judge by the end point's full list). */
static void test_meter_sensor_full_endpoint_list_falls_back(void)
{
    static fake_zdev_t multi;
    device(&multi, 0xC0E2u, false);
    fake_zdev_listening(&multi);
    multi.m.versions[multi.m.versions_n].cc = 0x60u;
    multi.m.versions[multi.m.versions_n].version = 4u;
    multi.m.versions_n++;
    multi.m.versions[multi.m.versions_n].cc = 0x31u;
    multi.m.versions[multi.m.versions_n].version = 5u;
    multi.m.versions_n++;
    multi.m.secure_ccs[multi.m.secure_n++] = 0x60u;
    multi.m.secure_ccs[multi.m.secure_n++] = 0x31u;
    multi.m.notif_n = 0u;
    multi.m.ep_n = 1u;
    fake_ep_t *ep = &multi.m.eps[0];
    ep->generic = 0x07u;
    ep->specific = 0x01u;
    static const uint8_t TWELVE[12] = {0x5E, 0x71, 0x30, 0x80, 0x84, 0x85, 0x86, 0x70,
                                       0x72, 0x73, 0x59, 0x8E};
    memcpy(ep->ccs, TWELVE, sizeof(TWELVE));
    ep->cc_n = 12u;
    ep->secure_ccs[0] = 0x71u;
    ep->secure_n = 1u;
    ep->notif_types[0] = 0x06u;
    ep->notif_n = 1u;
    const uint16_t node = include(&multi, 0u);
    until_ready(node);
    run_for(5000u);
    const zw_node_record_t *rec = zw_node_find_const(&W.net.nodes, node);
    TEST_ASSERT_TRUE(rec->endpoints[1].has_class);
    TEST_ASSERT_EQUAL_UINT(ZW_NODE_ENDPOINT_CC_MAX, rec->endpoints[1].cc_count);
    static const uint8_t AIR[] = {0x31, 0x05, 0x01, 0x22, 0x00, 0xE6};
    uint8_t mc[24];
    const uint32_t unsupported = zw_net_stats(&W.net)->unsupported_cc;
    const uint32_t reports = W.bridge.stats.reports;
    const size_t k = zw_cc_wrap_multi_channel(mc, sizeof(mc), 1u, 0u, false, AIR, sizeof(AIR));
    say(&multi, mc, k);
    TEST_ASSERT_EQUAL_UINT32(unsupported, zw_net_stats(&W.net)->unsupported_cc);
    TEST_ASSERT_EQUAL_UINT32(reports + 1u, W.bridge.stats.reports);
}

/* ZW-042, ZW-009: a leak sensor without keys announces a leak by broadcast - handled like a
 * singlecast report (Z-Wave JS: a report is not a Get, and a node without a class needs no
 * encryption): its zone faults. */
static void test_keyless_broadcast_report_reaches_zone(void)
{
    static fake_zdev_t leak;
    plain_listener(&leak, 0xC0E2u);
    const uint16_t node = include(&leak, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_WATER));
    run_for(5000u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    static const uint8_t WET[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x02, 0x00};
    const uint32_t dropped = zw_net_stats(&W.net)->not_singlecast;
    fake_zac93_from_node_as(&W.zac, W.wire.now, node, 0x04u, WET, sizeof(WET));
    run_for(1000u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(dropped, zw_net_stats(&W.net)->not_singlecast);
}

/* SendData requests the module got since its log entry `from`. */
static unsigned send_datas_from(size_t from)
{
    unsigned k = 0u;
    for (size_t i = from; i < W.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&W.zac, i);
        if (h != NULL && h->type == ZW_TYPE_REQUEST && h->func == ZW_FUNC_SEND_DATA) {
            k++;
        }
    }
    return k;
}

/* ZW-009, network.md §8 (Z-Wave JS: every accepted frame reaches handleRequest, where markAsAlive
 * is): from an S2 door sensor, a KEX Get and a Security 2 Commands Supported Get in the clear are
 * accepted - each is heard from it, a check-in - while a Notification Report in the clear (a wrong
 * level) is not. ZW-011, network.md §7 (Node.ts markAsAwake: a Get other than the two Nonce Gets):
 * with a command held for the sleeper, a Security 0 Nonce Get from it sends nothing, and its
 * Commands Supported Get is a wake-up - the held command goes. ZW-009 (Z-Wave JS: Nonce Gets are
 * the driver's, never the application's): the Security 0 Nonce Get is heard from it, but is no
 * report - the zones never see it. */
static void test_kex_and_commands_supported_get_heard(void)
{
    static fake_zdev_t door;
    const zsrc_zw_device_t *d = NULL;
    zsrc_address_t a;
    uint64_t heard = 0u;
    const uint16_t node = sleeping_door(&door, 0xD0E3u, &d, &a, &heard);
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    static const uint8_t KEX_GET[] = {0x9F, 0x04};
    static const uint8_t CSG[] = {0x9F, 0x0D};
    run_for(MIN_MS);
    fake_zac93_from_node_as(&W.zac, W.wire.now, node, 0x00u, OPEN, sizeof(OPEN));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT64(heard, last_heard(node));
    const uint32_t reports = W.bridge.stats.reports;
    fake_zac93_from_node_as(&W.zac, W.wire.now, node, 0x00u, KEX_GET, sizeof(KEX_GET));
    run_for(1000u);
    const uint64_t after_kex = last_heard(node);
    TEST_ASSERT_TRUE(after_kex > heard);
    TEST_ASSERT_EQUAL_UINT32(reports, W.bridge.stats.reports); /* it does not reach the zones */
    run_for(MIN_MS);
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&W.net, W.wire.now, node, ZW_SEC_AUTO,
                                                BATTERY_GET, sizeof(BATTERY_GET), 0x42u));
    run_for(1000u);
    const size_t log = W.zac.heard;
    door.awake_until = W.wire.now + 20000u; /* it listens from here on */
    static const uint8_t S0_NONCE_GET[] = {0x98, 0x40};
    const uint32_t before_nonce = W.bridge.stats.reports;
    const uint64_t heard_before_nonce = last_heard(node);
    fake_zac93_from_node_as(&W.zac, W.wire.now, node, 0x00u, S0_NONCE_GET, sizeof(S0_NONCE_GET));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(0u, send_datas_from(log)); /* no wake-up: the Battery Get waits */
    TEST_ASSERT_EQUAL_UINT32(before_nonce, W.bridge.stats.reports); /* no report */
    TEST_ASSERT_TRUE(last_heard(node) > heard_before_nonce);        /* heard */
    fake_zac93_from_node_as(&W.zac, W.wire.now, node, 0x00u, CSG, sizeof(CSG));
    run_for(1000u);
    TEST_ASSERT_TRUE(send_datas_from(log) >= 1u); /* a wake-up: it went */
    TEST_ASSERT_TRUE(last_heard(node) > after_kex); /* (its Battery Report answers then) */
    TEST_ASSERT_FALSE(info(1u).active);
}

/* ZW-012, network.md §7-8: a door sensor that silently keeps 4 h (its interval changed behind the
 * panel's back; the record says 1 h). Its late wake-up (more than its interval + 5 min) has its
 * interval read again, and the zone follows at once - not at the next restart: grace 4 h + 10 min,
 * no CHECK 1 h + 10 min (twice, and a minute) after it, unreachable only at 4 h + 10 min and CHECK
 * 4 h + 10 min later, to the millisecond. Its inputs are not asked again for it. Late again, its
 * interval is read again and found unchanged: no second event, the grace stays 4 h + 10 min. */
static void test_reread_interval_moves_the_grace(void)
{
    static fake_zdev_t door;
    const zsrc_zw_device_t *d = NULL;
    zsrc_address_t a;
    uint64_t heard = 0u;
    const uint16_t node = sleeping_door(&door, 0xD00Cu, &d, &a, &heard);
    TEST_ASSERT_EQUAL_UINT32(3600u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(3600u + 600u, zsrc_zw_ops()->grace_of(&W.src, &a));
    door.m.wake_interval = 4u * 3600u; /* what it really does */
    const uint32_t gets = W.bridge.stats.gets;
    const size_t events = W.ev_n;
    run(heard + HOUR_MS + 6u * MIN_MS); /* late, not unreachable yet */
    TEST_ASSERT_FALSE(info(1u).unreachable);
    fake_zdev_wake(&door, W.wire.now);
    run_for(3000u);
    uint32_t s = 0u;
    TEST_ASSERT_EQUAL_UINT(1u, checkins_took(events, node, &s));
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, s);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u,
                             zw_node_find_const(&W.net.nodes, node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u + 600u, zsrc_zw_ops()->grace_of(&W.src, &a));
    TEST_ASSERT_EQUAL_UINT32(gets, W.bridge.stats.gets); /* nothing asked again */
    const zone_hub_info_t i = info(1u);
    TEST_ASSERT_TRUE((i.caps & ZSRC_CAP_SUPERVISED) != 0u);
    TEST_ASSERT_TRUE((i.caps & ZSRC_CAP_LIFE_SAFETY) != 0u);
    /* it says nothing more: the old interval's unreachable and CHECK times pass quietly */
    heard = last_heard(node);
    const uint64_t t = (4u * 3600u + 600u) * 1000u;
    run(heard + 2u * (3600u + 600u) * 1000u + MIN_MS);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    TEST_ASSERT_FALSE(info(1u).check);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    run(heard + t - 1u);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    run(heard + t);
    TEST_ASSERT_TRUE(info(1u).unreachable);
    run(heard + 2u * t - 1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    run(heard + 2u * t);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    TEST_ASSERT_TRUE(info(1u).check);
    /* woken by hand, late again (well past 4 h + 5 min): its interval is read again, still 4 h -
     * nothing changed, so no second event and the grace stays */
    const unsigned asked = fake_zdev_asked(&door, ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_GET);
    fake_zdev_wake(&door, W.wire.now);
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(asked + 1u,
                           fake_zdev_asked(&door, ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_GET));
    TEST_ASSERT_EQUAL_UINT(1u, checkins_took(events, node, &s));
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u + 600u, zsrc_zw_ops()->grace_of(&W.src, &a));
}

/* ZW-015 ("interval 0 = not supervised"), network.md §8: a door sensor whose interval, read again
 * after a late wake-up, is 0 - it now wakes only by hand. Its zone no longer claims supervision:
 * no SUPERVISED, no LIFE_SAFETY (not trusted for fire), no grace; and no CHECK ever for its
 * silence, two days on. At 0 the panel does not read its interval again at its wake-ups (no
 * Wake Up Interval Get, no second event): only a re-interview brings it back. */
static void test_reread_interval_zero_not_supervised(void)
{
    static fake_zdev_t door;
    const zsrc_zw_device_t *d = NULL;
    zsrc_address_t a;
    uint64_t heard = 0u;
    const uint16_t node = sleeping_door(&door, 0xD00Du, &d, &a, &heard);
    TEST_ASSERT_TRUE((info(1u).caps & ZSRC_CAP_LIFE_SAFETY) != 0u);
    TEST_ASSERT_TRUE(zsrc_trusted_for(info(1u).caps, ALARM_ZT_FIRE));
    door.m.wake_interval = 0u;
    const size_t events = W.ev_n;
    run(heard + HOUR_MS + 6u * MIN_MS);
    fake_zdev_wake(&door, W.wire.now);
    run_for(3000u);
    uint32_t s = 1u;
    TEST_ASSERT_EQUAL_UINT(1u, checkins_took(events, node, &s));
    TEST_ASSERT_EQUAL_UINT32(0u, s);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_node_find_const(&W.net.nodes, node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(ZW_HEALTH_INTERVAL_NEVER,
                             zw_health_interval(zw_net_health(&W.net), node));
    TEST_ASSERT_EQUAL_UINT32(0u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_GRACE_NEVER, zsrc_zw_ops()->grace_of(&W.src, &a));
    const zone_hub_info_t i = info(1u);
    TEST_ASSERT_TRUE((i.caps & ZSRC_CAP_SUPERVISED) == 0u);
    TEST_ASSERT_TRUE((i.caps & ZSRC_CAP_LIFE_SAFETY) == 0u);
    TEST_ASSERT_FALSE(zsrc_trusted_for(i.caps, ALARM_ZT_FIRE));
    TEST_ASSERT_TRUE((i.caps & ZSRC_CAP_AUTH) != 0u); /* still encrypted */
    for (unsigned h = 0u; h < 4u; h++) {
        run_for(12u * HOUR_MS); /* two days, in steps the wire's guard allows */
    }
    TEST_ASSERT_FALSE(info(1u).unreachable);
    TEST_ASSERT_FALSE(info(1u).check);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    /* woken by hand two days on: at 0 its interval is not read again, nothing changes */
    const unsigned asked = fake_zdev_asked(&door, ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_GET);
    fake_zdev_wake(&door, W.wire.now);
    run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(asked,
                           fake_zdev_asked(&door, ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_GET));
    TEST_ASSERT_EQUAL_UINT(1u, checkins_took(events, node, &s));
    TEST_ASSERT_EQUAL_UINT32(0u, d->check_in_s);
    TEST_ASSERT_FALSE(info(1u).check);
}

/* ZW-015: an insecure motion sensor is a zone too - not authenticated, so not trusted for life
 * safety; its motion and idle reach the zone. */
static void test_insecure_motion_sensor(void)
{
    static fake_zdev_t pir;
    device(&pir, 0xB00Bu, true);
    fake_zdev_no_s2(&pir);
    pir.m.secure_n = 0u;
    pir.m.product_type = 0x0301u; /* a Zooz ZSE18 */
    pir.m.product_id = 0x0012u;
    pir.m.notif_types[0] = 0x07u;
    pir.m.notif_n = 1u;
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x80, 0x84};
    memcpy(pir.nif, NIF, sizeof(NIF)); /* a lifeline to be had without Security 2 */
    pir.nif_len = sizeof(NIF);
    const uint16_t node = include(&pir, 0u);
    const zsrc_zw_device_t *d = until_ready(node);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_SEC_NONE, d->security);
    const zsrc_candidate_t *c = candidate(node, 0u, ZSRC_ZW_SLOT_MOTION);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE18 motion sensor", c->name);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_MOTION, c->device_class);
    bind(1u, c);
    const zone_hub_info_t i = info(1u);
    TEST_ASSERT_EQUAL_UINT32(0u, i.caps & ZSRC_CAP_AUTH);
    TEST_ASSERT_FALSE(zsrc_trusted_for(i.caps, ALARM_ZT_FIRE));
    TEST_ASSERT_TRUE(zsrc_trusted_for(i.caps, ALARM_ZT_INTERIOR_FOLLOWER));
    notification(&pir, 0u, 0x07u, 0x08u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    const uint8_t idle[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x00, 0x01, 0x08};
    say(&pir, idle, sizeof(idle));
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
}

/* ZW-043, zones.md §8, command-classes.md "v1 alarm": a First Alert ZCOMBO as it really joins -
 * classic, Notification v1, so its interview asks no notification types - still finishes, is
 * offered smoke and CO through its quirk's alarm map, and its 4-byte V1 alarm (type 01, level FF
 * then 00) decoded from the wire reaches the bound zone as smoke detected, then idle. */
static void test_v1_smoke_alarm_by_quirk(void)
{
    static fake_zdev_t zc;
    device(&zc, 0x2C0Bu, false);
    fake_zdev_listening(&zc);
    fake_zdev_no_s2(&zc);
    zc.m.secure_n = 0u;
    zc.m.manufacturer = 0x0138u;
    zc.m.product_type = 0x0001u;
    zc.m.product_id = 0x0002u;
    zc.m.notif_n = 0u;
    for (size_t i = 0; i < zc.m.versions_n; i++) {
        if (zc.m.versions[i].cc == 0x71u) {
            zc.m.versions[i].version = 1u; /* Notification (Alarm) v1 */
        }
    }
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x80, 0x84};
    memcpy(zc.nif, NIF, sizeof(NIF));
    zc.nif_len = sizeof(NIF);
    const uint16_t node = include(&zc, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    const zsrc_zw_device_t *d = until_ready(node);
    TEST_ASSERT_NOT_NULL(d);
    const zsrc_candidate_t *smoke = candidate(node, 0u, ZSRC_ZW_SLOT_SMOKE);
    TEST_ASSERT_NOT_NULL(smoke);
    TEST_ASSERT_EQUAL_STRING("First Alert ZCOMBO smoke and CO alarm", smoke->name);
    const zsrc_candidate_t *co = candidate(node, 0u, ZSRC_ZW_SLOT_CO);
    TEST_ASSERT_NOT_NULL(co);
    TEST_ASSERT_EQUAL_STRING("First Alert ZCOMBO smoke and CO alarm", co->name);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_SMOKE));
    run_for(5000u);
    static const uint8_t ALARM[] = {0x71, 0x05, 0x01, 0xFF};
    say(&zc, ALARM, sizeof(ALARM));
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    static const uint8_t IDLE[] = {0x71, 0x05, 0x01, 0x00};
    say(&zc, IDLE, sizeof(IDLE));
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
}

/* ZW-013, ZW-011, network.md section 7: a battery sleeper whose last level is a day old is asked
 * for it once, at its next wake-up (not again at the one after); the level it sends clears a Low
 * that an earlier report set; a device without the Battery CC is never asked. */
static void test_battery_reread_daily(void)
{
    static fake_zdev_t door;
    device(&door, 0xB47u, true);
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    (void)until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(2u * HOUR_MS);
    static const uint8_t LOW[] = {0x80, 0x03, 0x0A};
    say(&door, LOW, sizeof(LOW));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    door.m.battery = 80u;
    unsigned asked = fake_zdev_asked(&door, 0x80, 0x02);
    run_for(23u * HOUR_MS); /* the report was 23 h ago: not yet */
    TEST_ASSERT_EQUAL_UINT(asked, fake_zdev_asked(&door, 0x80, 0x02));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    run_for(3u * HOUR_MS); /* a day old: asked once, when it wakes */
    TEST_ASSERT_EQUAL_UINT(asked + 1u, fake_zdev_asked(&door, 0x80, 0x02));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u); /* its 80 % cleared Low */
    asked++;
    run_for(20u * HOUR_MS); /* its wake-ups since: nothing asked */
    TEST_ASSERT_EQUAL_UINT(asked, fake_zdev_asked(&door, 0x80, 0x02));

    static fake_zdev_t plain;
    device(&plain, 0xB48u, false);
    fake_zdev_listening(&plain); /* a mains device: no Battery CC */
    const uint16_t pn = include(&plain, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, pn);
    run_for(HOUR_MS);
    asked = fake_zdev_asked(&plain, 0x80, 0x02);
    run_for(50u * HOUR_MS);
    TEST_ASSERT_EQUAL_UINT(asked, fake_zdev_asked(&plain, 0x80, 0x02));
}

/* ZW-013, ZW-024 (BL-085), zones.md §3-4: a mains smoke sensor that reports only through Alarm
 * Sensor (no Notification, no Binary Sensor) offers smoke, CO and heat inputs - its types from the
 * interview, through the node cache, to the zone source - and nothing else; its Alarm Sensor
 * Reports reach the bound zone. */
static void test_alarm_sensor_only_device(void)
{
    static fake_zdev_t smoke;
    device(&smoke, 0x5A0Cu, false);
    fake_zdev_listening(&smoke);
    fake_zdev_no_s2(&smoke);
    smoke.m.secure_n = 0u;
    smoke.m.product_type = 0x0F0Fu; /* a model no quirk row names */
    smoke.m.product_id = 0x0F0Fu;
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x9C};
    memcpy(smoke.nif, NIF, sizeof(NIF));
    smoke.nif_len = sizeof(NIF);
    smoke.m.notif_n = 0u;
    smoke.m.binary_n = 0u;
    static const uint8_t TYPES[] = {ZW_ALARM_SENSOR_TYPE_SMOKE, ZW_ALARM_SENSOR_TYPE_CO,
                                    ZW_ALARM_SENSOR_TYPE_HEAT};
    memcpy(smoke.m.alarm_types, TYPES, sizeof(TYPES));
    smoke.m.alarm_n = sizeof(TYPES);
    const uint16_t node = include(&smoke, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    const zsrc_zw_device_t *d = until_ready(node);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_SMOKE));
    TEST_ASSERT_NOT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_CO));
    TEST_ASSERT_NOT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_HEAT));
    TEST_ASSERT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    TEST_ASSERT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_WATER));
    TEST_ASSERT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_GENERAL)); /* Basic is not its input */
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_SMOKE));
    run_for(5000u);
    static const uint8_t ALARM[] = {0x9C, 0x02, 0x00, ZW_ALARM_SENSOR_TYPE_SMOKE, 0xFF, 0x00, 0x00};
    say(&smoke, ALARM, sizeof(ALARM));
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    static const uint8_t IDLE[] = {0x9C, 0x02, 0x00, ZW_ALARM_SENSOR_TYPE_SMOKE, 0x00, 0x00, 0x00};
    say(&smoke, IDLE, sizeof(IDLE));
    EXPECT(1u, ALARM_ZEV_RESTORE, 2u); /* idle again: its first state came from the watch's Get */
}

/* ZW-013, ZW-024 (BL-130), zones.md §3 "Fresh state": watched, an Alarm Sensor device is asked at
 * once for each type its Supported Report listed (smoke, CO, heat: a Get each, naming the type),
 * and the answer reaches the zone - a smoke alarm that went on before the watch is seen. A device
 * whose Alarm Sensor types are not known (it never answered the question) is asked no Alarm Sensor
 * Get: every one names a type, and it offers no Alarm Sensor input. */
static void test_alarm_sensor_fresh_state(void)
{
    static fake_zdev_t smoke;
    device(&smoke, 0x5A0Du, false);
    fake_zdev_listening(&smoke);
    fake_zdev_no_s2(&smoke);
    smoke.m.secure_n = 0u;
    smoke.m.product_type = 0x0F0Fu; /* a model no quirk row names */
    smoke.m.product_id = 0x0F0Fu;
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x9C};
    memcpy(smoke.nif, NIF, sizeof(NIF));
    smoke.nif_len = sizeof(NIF);
    smoke.m.notif_n = 0u;
    smoke.m.binary_n = 0u;
    static const uint8_t TYPES[] = {ZW_ALARM_SENSOR_TYPE_SMOKE, ZW_ALARM_SENSOR_TYPE_CO,
                                    ZW_ALARM_SENSOR_TYPE_HEAT};
    memcpy(smoke.m.alarm_types, TYPES, sizeof(TYPES));
    smoke.m.alarm_n = sizeof(TYPES);
    const uint16_t node = include(&smoke, 0u);
    until_ready(node);
    run_for(5000u);
    smoke.m.alarm_active = 1u << ZW_ALARM_SENSOR_TYPE_SMOKE; /* smoke, while nobody watched */
    const unsigned gets = fake_zdev_asked(&smoke, 0x9C, ZW_CMD_ALARM_SENSOR_GET);
    const size_t from = W.zac.heard;
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_SMOKE));
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(gets + 3u, fake_zdev_asked(&smoke, 0x9C, ZW_CMD_ALARM_SENSOR_GET));
    static const uint8_t SMOKE_GET[] = {0x9C, 0x01, ZW_ALARM_SENSOR_TYPE_SMOKE};
    static const uint8_t CO_GET[] = {0x9C, 0x01, ZW_ALARM_SENSOR_TYPE_CO};
    static const uint8_t HEAT_GET[] = {0x9C, 0x01, ZW_ALARM_SENSOR_TYPE_HEAT};
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, sent(from, SMOKE_GET, sizeof(SMOKE_GET)));
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, sent(from, CO_GET, sizeof(CO_GET)));
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, sent(from, HEAT_GET, sizeof(HEAT_GET)));
    TEST_ASSERT_TRUE(info(1u).known);
    TEST_ASSERT_TRUE(info(1u).active);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);

    /* no types known: Notification asked, no Alarm Sensor Get */
    static fake_zdev_t mute;
    device(&mute, 0x5A0Eu, false);
    fake_zdev_listening(&mute);
    fake_zdev_no_s2(&mute);
    mute.m.secure_n = 0u;
    static const uint8_t MNIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x9C};
    memcpy(mute.nif, MNIF, sizeof(MNIF));
    mute.nif_len = sizeof(MNIF);
    mute.m.notif_types[0] = 0x06u; /* access control */
    mute.m.notif_n = 1u;
    mute.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    mute.m.alarm_n = 1u;
    mute.m.mute_cc = 0x9Cu;
    mute.m.mute_cmd = ZW_CMD_ALARM_SENSOR_SUPPORTED_GET;
    mute.m.mute_n = FAKE_MUTE_ALWAYS;
    const uint16_t mn = include(&mute, 0u);
    until_ready(mn);
    run_for(5000u);
    TEST_ASSERT_NULL(candidate(mn, 0u, ZSRC_ZW_SLOT_SMOKE));
    const unsigned notif_gets = fake_zdev_asked(&mute, 0x71, 0x04);
    bind(2u, candidate(mn, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(notif_gets + 1u, fake_zdev_asked(&mute, 0x71, 0x04));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&mute, 0x9C, ZW_CMD_ALARM_SENSOR_GET));
}

/* A mains multi-sensor with two end points (a contact, a leak), S2 Access Control: each end point
 * its own input and zone; asked for its state at once when watched (zones.md §3 "Fresh state");
 * its root's tamper reaches both zones; its Supervision Gets answered at once with their class and
 * end point (command-classes.md §4.14), and a question to the panel answered (§5). Then the radio
 * is pulled: one trouble for the source after its grace, no zone CHECK (ZSRC-015); back, every
 * input speaks again. */
static void test_two_endpoints_and_the_radio(void)
{
    static fake_zdev_t multi;
    device(&multi, 0xC0DEu, false);
    fake_zdev_listening(&multi);
    multi.m.versions[multi.m.versions_n].cc = 0x60u;
    multi.m.versions[multi.m.versions_n].version = 4u;
    multi.m.versions_n++;
    multi.m.secure_ccs[multi.m.secure_n++] = 0x60u;
    multi.m.notif_n = 0u;
    multi.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &multi.m.eps[e];
        ep->generic = 0x07u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x71u;
        ep->secure_n = 1u;
        ep->notif_types[0] = e == 0u ? 0x06u : 0x05u;
        ep->notif_n = 1u;
    }
    const uint16_t node = include(&multi, 0u);
    until_ready(node);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_SEC_S2_ACCESS, zsrc_zw_find(&W.src, node)->security);
    const zsrc_candidate_t *contact = candidate(node, 1u, ZSRC_ZW_SLOT_CONTACT);
    const zsrc_candidate_t *leak = candidate(node, 2u, ZSRC_ZW_SLOT_WATER);
    TEST_ASSERT_NOT_NULL(contact);
    TEST_ASSERT_NOT_NULL(leak);
    TEST_ASSERT_EQUAL_STRING("input 2 · water", leak->detail);
    TEST_ASSERT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    const unsigned gets = fake_zdev_asked(&multi, 0x71, 0x04);
    bind(1u, contact);
    bind(2u, leak);
    run_for(5000u);
    /* asked at once, each end point (its interview's answers had said dry already) */
    TEST_ASSERT_EQUAL_UINT(gets + 2u, fake_zdev_asked(&multi, 0x71, 0x04));
    TEST_ASSERT_TRUE(info(2u).known);
    TEST_ASSERT_FALSE(info(2u).active);
    notification(&multi, 1u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    notification(&multi, 2u, 0x05u, 0x02u);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    notification(&multi, 0u, 0x07u, 0x03u); /* the root's cover */
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
    /* a supervised report from end point 2: answered FF, at its class, to end point 2 */
    const uint8_t dry[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x00, 0x00};
    uint8_t mc[24];
    uint8_t spv[40];
    const size_t k = zw_cc_wrap_multi_channel(mc, sizeof(mc), 2u, 0u, false, dry, sizeof(dry));
    const size_t s = zw_cc_wrap_supervision(spv, sizeof(spv), 0x15u, false, mc, k);
    multi.got_n = 0u;
    const unsigned reports = fake_zdev_asked(&multi, 0x6C, 0x02);
    say(&multi, spv, s);
    EXPECT(2u, ALARM_ZEV_RESTORE, 2u); /* dry again (its first state came from the Get) */
    TEST_ASSERT_EQUAL_UINT(reports + 1u, fake_zdev_asked(&multi, 0x6C, 0x02));
    TEST_ASSERT_TRUE(multi.got_n >= 1u);
    static const uint8_t WANT[] = {0x6C, 0x02, 0x15, 0xFF, 0x00};
    TEST_ASSERT_EQUAL_UINT(sizeof(WANT), multi.got_len[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(WANT, multi.got[0], sizeof(WANT));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, multi.got_sec[0]);
    /* ZW-009 (Z-Wave JS Driver.ts shouldDiscardCC): a Meter Report from a device without Meter
     * is discarded before anything - not even its Supervision Get is answered */
    static const uint8_t METER[] = {0x32, 0x02, 0x21, 0x44, 0x00, 0x00, 0x01, 0x2C};
    size_t s2n = zw_cc_wrap_supervision(spv, sizeof(spv), 0x17u, false, METER, sizeof(METER));
    const uint32_t unsupported = zw_net_stats(&W.net)->unsupported_cc;
    multi.got_n = 0u;
    say(&multi, spv, s2n);
    TEST_ASSERT_EQUAL_UINT(0u, multi.got_n);
    TEST_ASSERT_EQUAL_UINT32(unsupported + 1u, zw_net_stats(&W.net)->unsupported_cc);
    /* ...and so is its Multilevel Sensor Report (air temperature): no zone hears it */
    static const uint8_t AIR[] = {0x31, 0x05, 0x01, 0x22, 0x00, 0xE6};
    const size_t reports_before = W.bridge.stats.reports;
    say(&multi, AIR, sizeof(AIR));
    TEST_ASSERT_EQUAL_UINT32(unsupported + 2u, zw_net_stats(&W.net)->unsupported_cc);
    TEST_ASSERT_EQUAL_UINT(reports_before, W.bridge.stats.reports);
    /* ZW-009 (Z-Wave JS Node.ts hands each Multi Command entry to handleCommand, so shouldDiscardCC
     * never sees inside it): the same report inside a Multi Command is not checked for support -
     * each entry delivered, nothing counted */
    static const uint8_t LOW_BATTERY[] = {0x80, 0x03, 0xFF};
    const uint8_t *const entries[2] = {AIR, LOW_BATTERY};
    const size_t entry_lens[2] = {sizeof(AIR), sizeof(LOW_BATTERY)};
    uint8_t mcmd[32];
    const size_t mn = zw_cc_wrap_multi_command(mcmd, sizeof(mcmd), entries, entry_lens, 2u);
    TEST_ASSERT_TRUE(mn > 0u);
    say(&multi, mcmd, mn);
    TEST_ASSERT_EQUAL_UINT32(unsupported + 2u, zw_net_stats(&W.net)->unsupported_cc);
    TEST_ASSERT_EQUAL_UINT(reports_before + 2u, W.bridge.stats.reports);
    /* a command the zones don't use (a Node Naming Report), supervised: "no support" (00) */
    static const uint8_t NAME[] = {0x77, 0x03, 0x00, 0x41};
    s2n = zw_cc_wrap_supervision(spv, sizeof(spv), 0x16u, false, NAME, sizeof(NAME));
    multi.got_n = 0u;
    say(&multi, spv, s2n);
    static const uint8_t NO_SUPPORT[] = {0x6C, 0x02, 0x16, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(NO_SUPPORT, multi.got[0], sizeof(NO_SUPPORT));
    /* it asks the panel's version (§5): answered */
    static const uint8_t VERSION_GET[] = {0x86, 0x11};
    const unsigned versions = fake_zdev_asked(&multi, 0x86, 0x12);
    say(&multi, VERSION_GET, sizeof(VERSION_GET));
    TEST_ASSERT_EQUAL_UINT(versions + 1u, fake_zdev_asked(&multi, 0x86, 0x12));
    TEST_ASSERT_TRUE(W.bridge.stats.answers >= 1u);
    notification(&multi, 2u, 0x05u, 0x02u); /* wet when the radio goes */
    TEST_ASSERT_TRUE(info(2u).active);
    /* the radio is pulled: the link goes down at the next command (a quiet device's ping) */
    W.zev_n = 0u;
    W.zac.fault.dead = true;
    run_for(30u * MIN_MS);
    TEST_ASSERT_NOT_EQUAL(ZW_LINK_UP, zw_host_link(zw_net_host(&W.net)));
    TEST_ASSERT_FALSE(W.src.link_up);
    run_for(4200u * 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.trouble_on); /* one sensor_radio trouble */
    run_for(2u * HOUR_MS);
    TEST_ASSERT_EQUAL_UINT(1u, W.trouble_on);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 0u);
    /* plugged in again: the link comes up, the device is asked again and speaks */
    W.zac.fault.dead = false;
    run_for(10u * MIN_MS);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&W.net)));
    TEST_ASSERT_TRUE(W.src.link_up);
    TEST_ASSERT_FALSE(info(2u).active); /* asked at link up: end point 2 says dry */
    notification(&multi, 1u, 0x06u, 0x17u);
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.trouble_off); /* both inputs heard again */
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
}

/* zones.md §3 "Fresh state" for a sleeper after a panel restart: its bindings and node cache come
 * back; its state is asked for at its next wake-up, not before (it sleeps). ZW-014: an address
 * with the generation the device had. The quirks table reaches the interview (zones.md §8). */
static void test_restart_and_fresh_state(void)
{
    static const zsrc_zw_quirk_t ROWS[] = {
        {.manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE001u,
         .name = "Test door", .config_count = 1u, .config = {{7u, 1u, 3}}},
    };
    zsrc_zw_set_quirks(&W.src, ROWS, 1u);
    static fake_zdev_t door;
    device(&door, 0xD00Bu, true);
    const uint16_t node = include(&door, 0u);
    until_ready(node);
    TEST_ASSERT_EQUAL_UINT(1u, door.m.config_n); /* the row's parameter, set by the interview */
    TEST_ASSERT_EQUAL_UINT8(7u, door.m.config[0].parameter);
    TEST_ASSERT_EQUAL_INT32(3, door.m.config[0].value);
    const zsrc_candidate_t *c = candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT);
    TEST_ASSERT_EQUAL_STRING("Test door", c->name);
    const uint16_t gen = zsrc_zw_find(&W.src, node)->generation;
    bind(1u, c);
    notification(&door, 0u, 0x06u, 0x17u);
    run_for(2000u); /* back to sleep */
    TEST_ASSERT_FALSE(fake_zdev_awake(&door, W.wire.now));
    /* the panel restarts */
    const unsigned gets = fake_zdev_asked(&door, 0x71, 0x04);
    boot();
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&W.hub, 1u, &b));
    TEST_ASSERT_TRUE(zsrc_address_equal(&b.in[ZSRC_ROLE_MAIN].addr, &c->addr));
    TEST_ASSERT_EQUAL_UINT16(gen, zsrc_zw_find(&W.src, node)->generation);
    TEST_ASSERT_TRUE(W.src.link_up);
    run_for(10u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(gets, fake_zdev_asked(&door, 0x71, 0x04)); /* asleep: not yet */
    TEST_ASSERT_TRUE(info(1u).unreachable); /* nothing heard since the restart */
    fake_zdev_wake(&door, W.wire.now); /* its check-in, or its button */
    run_for(10000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&door, 0x71, 0x04) > gets); /* asked at its wake-up */
    TEST_ASSERT_FALSE(info(1u).unreachable);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    /* the module restarts: zw_health's clocks start again, which is no word from the sleeper -
     * its zone waits for it (ZSRC-015) */
    run_for(MIN_MS);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&W.zac, W.wire.now + 10u, 0x00u));
    run_for(10u * MIN_MS);
    TEST_ASSERT_TRUE(W.src.link_up);
    TEST_ASSERT_TRUE(info(1u).unreachable);
}

/* ZW-014 through the network: the device is removed ("Remove a device"): its zone keeps its
 * binding and shows CHECK at once; the next device gets the same node ID with a new generation
 * and never feeds that zone. */
static void test_removed_and_node_id_reused(void)
{
    static fake_zdev_t a;
    static fake_zdev_t b2;
    device(&a, 0xAAAAu, true); /* a sleeper: its state asked for at its next wake-up */
    const uint16_t node = include(&a, 0u);
    until_ready(node);
    run_for(5000u);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&W.net, W.wire.now, true));
    run_for(200u);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&W.zac, W.wire.now, &a));
    run_for(3000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    zone_hub_binding_t bb;
    TEST_ASSERT_TRUE(zone_hub_get(&W.hub, 1u, &bb));
    device(&b2, 0xBBBBu, false);
    fake_zdev_listening(&b2);
    W.zac.join_node_id = node; /* the module gives the node ID out again */
    const uint16_t again = include(&b2, 0u);
    W.zac.join_node_id = 0u;
    TEST_ASSERT_EQUAL_UINT16(node, again);
    until_ready(again);
    TEST_ASSERT_TRUE(zsrc_zw_find(&W.src, again)->generation > 1u);
    notification(&b2, 0u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_TRUE(info(1u).check);
    /* nothing of the removed device's is waiting for the new one: its question is answered */
    static const uint8_t VERSION_GET[] = {0x86, 0x11};
    const unsigned answered = fake_zdev_asked(&b2, 0x86, 0x12);
    say(&b2, VERSION_GET, sizeof(VERSION_GET));
    TEST_ASSERT_EQUAL_UINT(answered + 1u, fake_zdev_asked(&b2, 0x86, 0x12));
}

static void add_cc(zw_node_record_t *rec, uint8_t cc, uint8_t version)
{
    rec->ccs[rec->cc_count].cc = cc;
    rec->ccs[rec->cc_count].version = version;
    rec->ccs[rec->cc_count].supported = true;
    rec->cc_count++;
}

/* The node cache's record as the zone source's facts (zones.md §2's "interview shows"): identity,
 * security, check-in, listening, the inputs verdict per end point, notification types with their
 * events, Binary Sensor types, air temperature, Basic. */
static void test_describe_from_the_node_cache(void)
{
    zw_node_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.node_id = 9u;
    rec.generation = 4u;
    rec.has_protocol = true;
    rec.has_identity = true;
    rec.manufacturer_id = 0x027Au;
    rec.product_type = 0x7000u;
    rec.product_id = 0xE001u;
    rec.security_classes = ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH;
    rec.has_wake_up = true;
    rec.wake_up_interval_seconds = 14400u;
    rec.has_zwave_plus = true;
    rec.installer_icon = 0x0C07u;
    rec.has_lifeline = true;
    rec.lifeline_set = true;
    rec.lifeline_group = 1u;
    rec.interview_step = ZW_IV_STEP_DONE;
    add_cc(&rec, 0x71u, 8u);
    add_cc(&rec, 0x30u, 2u);
    add_cc(&rec, 0x31u, 5u);
    zw_node_endpoint_t *e0 = &rec.endpoints[0];
    e0->used = true;
    e0->notification_count = 2u;
    e0->notifications[0].notification_type = 0x06u;
    e0->notifications[0].event_mask[2] = 0xC0u; /* events 16, 17 */
    e0->notifications[0].event_mask_len = 3u;
    e0->notifications[1].notification_type = 0x07u;
    e0->notifications[1].event_mask[0] = 0x08u; /* event 03 */
    e0->notifications[1].event_mask_len = 1u;
    e0->has_binary_sensor_mask = true;
    e0->binary_sensor_mask[1] = 0x14u; /* types 0A and 0C */
    e0->binary_sensor_mask_len = 2u;
    e0->has_multilevel_mask = true;
    e0->multilevel_mask[0] = 0x01u; /* air temperature */
    e0->multilevel_mask_len = 1u;
    zw_node_endpoint_t *e1 = &rec.endpoints[1];
    e1->used = true;
    e1->has_class = true;
    e1->ccs[0] = 0x71u;
    e1->ccs[1] = 0x20u;
    e1->cc_count = 2u;
    e1->notification_count = 1u;
    e1->notifications[0].notification_type = 0x05u;
    e1->notifications[0].event_mask[0] = 0x06u; /* events 01, 02 */
    e1->notifications[0].event_mask_len = 1u;
    rec.endpoints[3].used = true; /* 3 of them, the second unknown */
    rec.endpoints[3].has_push_pull = true;
    rec.endpoints[3].push_pull_mode = ZW_NODE_MODE_PULL;
    zsrc_zw_desc_t d;
    zwz_describe(&rec, &d);
    TEST_ASSERT_EQUAL_UINT16(9u, d.node);
    TEST_ASSERT_EQUAL_UINT16(4u, d.generation);
    TEST_ASSERT_TRUE(d.has_identity);
    TEST_ASSERT_EQUAL_HEX16(0x027Au, d.manufacturer);
    TEST_ASSERT_EQUAL_HEX16(0x7000u, d.product_type);
    TEST_ASSERT_EQUAL_HEX16(0xE001u, d.product_id);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_SEC_S2_AUTH, d.security);
    TEST_ASSERT_FALSE(d.listening);
    TEST_ASSERT_EQUAL_UINT32(14400u, d.check_in_s);
    TEST_ASSERT_EQUAL_HEX16(0x0C07u, d.installer_icon);
    TEST_ASSERT_EQUAL_UINT8(3u, d.endpoint_count);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_INPUTS_OFFERED, d.ep[0].inputs);
    TEST_ASSERT_EQUAL_UINT8(2u, d.ep[0].notif_count);
    TEST_ASSERT_EQUAL_UINT8(0x06u, d.ep[0].notif[0].type);
    TEST_ASSERT_EQUAL_HEX32((1u << 0x16) | (1u << 0x17), d.ep[0].notif[0].events);
    TEST_ASSERT_EQUAL_HEX32(1u << 0x03, d.ep[0].notif[1].events);
    TEST_ASSERT_TRUE(d.ep[0].binary);
    TEST_ASSERT_EQUAL_HEX32((1u << 0x0A) | (1u << 0x0C), d.ep[0].binary_types);
    TEST_ASSERT_TRUE(d.ep[0].air_temperature);
    TEST_ASSERT_FALSE(d.ep[0].basic);
    TEST_ASSERT_EQUAL_HEX16(0u, d.ep[0].alarm_types); /* not asked (a record from before BL-085) */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_INPUTS_OFFERED, d.ep[1].inputs);
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 2), d.ep[1].notif[0].events);
    TEST_ASSERT_TRUE(d.ep[1].basic);
    TEST_ASSERT_FALSE(d.ep[1].binary);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_INPUTS_NOT_READY, d.ep[2].inputs);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_INPUTS_PULL, d.ep[3].inputs);
    /* the other classes; FLiRS listens; no Wake Up, no check-in; Multilevel Sensor before v5 */
    static const struct {
        uint8_t classes, security;
    } SEC[] = {{ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH, ZSRC_ZW_SEC_S2_ACCESS},
               {ZW_S2_KEY_UNAUTH, ZSRC_ZW_SEC_S2_UNAUTH},
               {ZW_S2_KEY_S0, ZSRC_ZW_SEC_S0},
               {0x00u, ZSRC_ZW_SEC_NONE},
               {ZW_NODE_SECURITY_UNKNOWN, ZSRC_ZW_SEC_UNKNOWN}};
    for (size_t i = 0; i < sizeof(SEC) / sizeof(SEC[0]); i++) {
        rec.security_classes = SEC[i].classes;
        zwz_describe(&rec, &d);
        TEST_ASSERT_EQUAL_UINT8(SEC[i].security, d.security);
    }
    rec.is_flirs = true;
    rec.has_wake_up = false;
    rec.has_zwave_plus = false;
    e0->has_multilevel_mask = false;
    memset(e0->multilevel_mask, 0, sizeof(e0->multilevel_mask));
    e0->multilevel_mask_len = 0u;
    zwz_describe(&rec, &d);
    TEST_ASSERT_TRUE(d.listening);
    TEST_ASSERT_EQUAL_UINT32(0u, d.check_in_s);
    TEST_ASSERT_EQUAL_HEX16(0u, d.installer_icon);
    TEST_ASSERT_TRUE(d.ep[0].air_temperature); /* it can't say: offered */
    e0->has_multilevel_mask = true;
    e0->multilevel_mask[0] = 0x10u; /* humidity only */
    zwz_describe(&rec, &d);
    TEST_ASSERT_FALSE(d.ep[0].air_temperature);
    e0->multilevel_mask[0] = 0x01u;
    e0->multilevel_mask_len = 1u;
    rec.interview_step = ZW_IV_STEP_BATTERY; /* still interviewing */
    zwz_describe(&rec, &d);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_INPUTS_NOT_READY, d.ep[0].inputs);
    rec.endpoints[3].used = false; /* one end point */
    zwz_describe(&rec, &d);
    TEST_ASSERT_EQUAL_UINT8(1u, d.endpoint_count);
    /* BL-085: Alarm Sensor's types, bit t = type t (smoke 1, CO 2, heat 4; general 0 too) */
    e0->has_alarm_sensor_mask = true;
    e0->alarm_sensor_mask[0] = 0x17u;
    e0->alarm_sensor_mask_len = 1u;
    zwz_describe(&rec, &d);
    TEST_ASSERT_EQUAL_HEX16(0x0017u, d.ep[0].alarm_types);
    e0->alarm_sensor_mask[2] = 0x01u; /* type 16: past what the source holds */
    e0->alarm_sensor_mask_len = 3u;
    zwz_describe(&rec, &d);
    TEST_ASSERT_EQUAL_HEX16(0x0017u, d.ep[0].alarm_types);
}

/* A sleeping device with two end points: both inputs' states are asked at its next wake-up, one
 * question after the other - two queued at once would be one kind to zw_net (a Multi Channel
 * wrapper to the device) and the second would replace the first. */
static void test_sleeping_end_points_asked_one_at_a_time(void)
{
    static fake_zdev_t two;
    device(&two, 0x7E57u, true);
    two.m.versions[two.m.versions_n].cc = 0x60u;
    two.m.versions[two.m.versions_n].version = 4u;
    two.m.versions_n++;
    two.m.secure_ccs[two.m.secure_n++] = 0x60u;
    two.m.notif_n = 0u;
    two.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &two.m.eps[e];
        ep->generic = 0x07u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x71u;
        ep->secure_n = 1u;
        ep->notif_types[0] = e == 0u ? 0x06u : 0x05u;
        ep->notif_n = 1u;
    }
    const uint16_t node = include(&two, 0u);
    until_ready(node);
    run_for(5000u); /* back to sleep */
    TEST_ASSERT_FALSE(fake_zdev_awake(&two, W.wire.now));
    const unsigned gets = fake_zdev_asked(&two, 0x71, 0x04);
    bind(1u, candidate(node, 1u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, candidate(node, 2u, ZSRC_ZW_SLOT_WATER));
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT(gets, fake_zdev_asked(&two, 0x71, 0x04)); /* asleep */
    /* one frame from it (Multi Command): two supervised reports from end point 1 - two answers
     * to one end point at once; all Multi Channel commands are one kind to zw_net's queue, where
     * the newer would replace the older */
    const unsigned reports = fake_zdev_asked(&two, 0x6C, 0x02);
    static const uint8_t SHUT[] = {0x20, 0x03, 0x00};
    uint8_t mc[16];
    const size_t k = zw_cc_wrap_multi_channel(mc, sizeof(mc), 1u, 0u, false, SHUT, sizeof(SHUT));
    uint8_t spv[2][24];
    const uint8_t *cmds[2];
    size_t lens[2];
    for (size_t i = 0; i < 2u; i++) {
        const uint8_t session = (uint8_t)(0x21u + i);
        lens[i] = zw_cc_wrap_supervision(spv[i], sizeof(spv[i]), session, false, mc, k);
        cmds[i] = spv[i];
    }
    uint8_t both[64];
    const size_t n = zw_cc_wrap_multi_command(both, sizeof(both), cmds, lens, 2u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&two, W.wire.now, ZW_SEC_AUTO, both, n));
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(reports + 2u, fake_zdev_asked(&two, 0x6C, 0x02));
    /* B026: a report it supervised is no wake-up - answered at once, its Gets wait */
    TEST_ASSERT_EQUAL_UINT(gets, fake_zdev_asked(&two, 0x71, 0x04));
    fake_zdev_wake(&two, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(gets + 2u, fake_zdev_asked(&two, 0x71, 0x04));
    TEST_ASSERT_TRUE(info(2u).known); /* its answer: dry */
}

/* A report the device supervises, as battery sensors send their state (command-classes.md §4.14). */
static void supervised(fake_zdev_t *d, uint8_t session, uint8_t type, uint8_t event)
{
    const uint8_t n[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, type, event, 0x00};
    uint8_t sv[24];
    const size_t k = zw_cc_wrap_supervision(sv, sizeof(sv), session, false, n, sizeof(n));
    say(d, sv, k);
}

/* B026, ZW-011 (network.md §7 "Waking"): a sleeper's supervised report is no wake-up. Its
 * Supervision Report goes at once and the device stays asleep - no No More Information follows,
 * as Z-Wave JS answers on its Immediate queue without marking the node awake. On the bench a
 * contact that sleeps once answered never ACKed that No More Information, whose encryption took
 * the shared SPAN's next nonce: the contact's next report failed to decrypt, and its open or
 * closed came late or not at all. A wake-up still ends with No More Information. */
static void test_supervised_report_is_no_wake_up(void)
{
    static fake_zdev_t door;
    device(&door, 0xB026u, true);
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(MIN_MS); /* asleep */
    TEST_ASSERT_FALSE(fake_zdev_awake(&door, W.wire.now));
    const uint32_t nmi = zw_net_stats(&W.net)->no_more_info;
    const uint32_t heard_nmi = door.nmi_heard;
    const uint32_t answers = door.spv_reports;
    unsigned faults = count(1u, ALARM_ZEV_FAULT);
    unsigned restores = count(1u, ALARM_ZEV_RESTORE);
    /* it listens 2 s after it speaks (the default): it would hear anything that followed */
    for (uint8_t k = 0u; k < 4u; k++) {
        supervised(&door, (uint8_t)(0x10u + k), 0x06u, (k & 1u) != 0u ? 0x17u : 0x16u);
        run_for(2000u);
    }
    EXPECT(1u, ALARM_ZEV_FAULT, faults + 2u);
    EXPECT(1u, ALARM_ZEV_RESTORE, restores + 2u);
    TEST_ASSERT_EQUAL_UINT32(answers + 4u, door.spv_reports); /* each answered */
    TEST_ASSERT_EQUAL_UINT32(nmi, zw_net_stats(&W.net)->no_more_info);
    TEST_ASSERT_EQUAL_UINT32(heard_nmi, door.nmi_heard);
    /* as the bench contact: asleep 100 ms after it speaks - each report decrypts the first time */
    door.m.awake_extend_ms = 100u;
    const uint32_t failed = zw_s2_node_stats(&W.net.s2, node)->decrypt_failures;
    faults = count(1u, ALARM_ZEV_FAULT);
    restores = count(1u, ALARM_ZEV_RESTORE);
    for (uint8_t k = 0u; k < 6u; k++) {
        supervised(&door, (uint8_t)(0x20u + k), 0x06u, (k & 1u) != 0u ? 0x17u : 0x16u);
        run_for(2000u);
    }
    EXPECT(1u, ALARM_ZEV_FAULT, faults + 3u);
    EXPECT(1u, ALARM_ZEV_RESTORE, restores + 3u);
    TEST_ASSERT_EQUAL_UINT32(failed, zw_s2_node_stats(&W.net.s2, node)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT32(nmi, zw_net_stats(&W.net)->no_more_info);
    /* its wake-up still ends with No More Information */
    door.m.awake_extend_ms = 2000u;
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, zw_net_stats(&W.net)->no_more_info);
    TEST_ASSERT_EQUAL_UINT32(heard_nmi + 1u, door.nmi_heard);
}

/* The bench contact's way: a command inside a Supervision Get, and the next one as soon as it
 * hears the answer. True when the answer came within 200 ms. */
static bool supervised_answered(fake_zdev_t *d, uint8_t session, const uint8_t *cmd, size_t n)
{
    const uint32_t answers = d->spv_reports;
    uint8_t sv[24];
    const size_t len = zw_cc_wrap_supervision(sv, sizeof(sv), session, false, cmd, n);
    const uint64_t asked = W.wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(d, asked, ZW_SEC_AUTO, sv, len));
    for (unsigned t = 0u; t < 20u && d->spv_reports == answers; t++) {
        run_for(10u);
    }
    return d->spv_reports == answers + 1u && d->spv_at - asked < 200u;
}

/* ZW-011 (B026's bench follow-up): during a wake-up, each supervised report is answered at once.
 * The bench contact supervises its Wake Up Notification and its reports, and holds each until it
 * hears the Supervision Report to the last. Its next frame, on the SPAN's next nonce, shows it
 * decoded that answer (security.md §6), so the next answer need not wait out the 500 ms watch for
 * a Nonce Report: on the bench each one did, and a change showed up to half a second late. */
static void test_awake_reports_answered_at_once(void)
{
    static fake_zdev_t door;
    device(&door, 0xB027u, true);
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(MIN_MS); /* asleep */
    const uint32_t nmi = zw_net_stats(&W.net)->no_more_info;
    const unsigned faults = count(1u, ALARM_ZEV_FAULT);
    const unsigned restores = count(1u, ALARM_ZEV_RESTORE);
    door.awake_until = W.wire.now + 10000u; /* it wakes, until No More Information */
    static const uint8_t WUN[] = {0x84, 0x07};
    TEST_ASSERT_TRUE(supervised_answered(&door, 0x2Fu, WUN, sizeof(WUN)));
    for (uint8_t k = 0u; k < 6u; k++) {
        const uint8_t event = (uint8_t)((k & 1u) != 0u ? 0x17u : 0x16u);
        const uint8_t n[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06u, event, 0x00};
        TEST_ASSERT_TRUE_MESSAGE(supervised_answered(&door, (uint8_t)(0x30u + k), n, sizeof(n)),
                                 "the answer waited");
    }
    run_for(10000u);
    EXPECT(1u, ALARM_ZEV_FAULT, faults + 3u);
    EXPECT(1u, ALARM_ZEV_RESTORE, restores + 3u);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_s2_node_stats(&W.net.s2, node)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, zw_net_stats(&W.net)->no_more_info); /* its wake-up ends */
    TEST_ASSERT_EQUAL_UINT32(0u, door.heard_asleep);
}

/* ZW-010's "no lifeline, no inputs" through the bridge: a device without Association has none
 * to offer, however many notification types it lists. */
static void test_no_lifeline_offers_nothing(void)
{
    static fake_zdev_t mute;
    device(&mute, 0x0FF5u, false);
    fake_zdev_listening(&mute);
    fake_zdev_no_s2(&mute);
    mute.m.secure_n = 0u;
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x71};
    memcpy(mute.nif, NIF, sizeof(NIF));
    mute.nif_len = sizeof(NIF);
    const uint16_t node = include(&mute, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    run_for(30000u);
    size_t ready = 0u;
    for (size_t i = 0; i < W.ev_n; i++) {
        ready += W.ev[i].kind == ZW_NET_EV_NODE_READY && W.ev[i].node == node;
    }
    TEST_ASSERT_EQUAL_UINT(1u, ready); /* interviewed */
    const zsrc_zw_device_t *d = zsrc_zw_find(&W.src, node);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(d->ready);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZW_INPUTS_NO_LIFELINE, d->inputs[0]);
    TEST_ASSERT_NULL(candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
}

/* A device without Security 2, so the module's log shows its commands in the clear: a mains
 * multi-sensor - home security, a door/window and a motion Binary Sensor, air temperature and a
 * backup battery at its root, a contact and a leak on two end points. Fresh state asks what each
 * watched end point has (notification types, binary types, air temperature, then the battery), an
 * end point's inside Multi Channel; the answers and its reports decode to its inputs (typed Binary
 * Sensor, v2 Battery's replace flag, Multilevel Sensor at any precision and in Fahrenheit, an idle
 * with its parameter); Hail asks again; a supervised report from an end point is answered to that
 * end point; the panel's Version Report has the module's protocol version. And a device that
 * speaks only Basic: Set and Report feed its general input (command-classes.md §3-§5, zones.md
 * §3, §5). ZW-013. */
static void test_every_input_asked_and_decoded(void)
{
    static fake_zdev_t ms;
    device(&ms, 0x3510u, false);
    fake_zdev_listening(&ms);
    fake_zdev_no_s2(&ms);
    ms.m.secure_n = 0u;
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x30, 0x31, 0x80, 0x60};
    memcpy(ms.nif, NIF, sizeof(NIF));
    ms.nif_len = sizeof(NIF);
    static const fake_cc_version_t MORE[] = {{0x30, 2}, {0x31, 11}, {0x80, 2}, {0x60, 4}};
    for (size_t i = 0; i < sizeof(MORE) / sizeof(MORE[0]); i++) {
        ms.m.versions[ms.m.versions_n++] = MORE[i];
    }
    ms.m.notif_types[0] = 0x07u;
    ms.m.notif_n = 1u;
    ms.m.binary_types[0] = 0x0Au;
    ms.m.binary_types[1] = 0x0Cu;
    ms.m.binary_n = 2u;
    ms.m.temperature = true;
    ms.m.temp_c10 = 215;
    ms.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &ms.m.eps[e];
        ep->generic = 0x07u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->ccs[1] = 0x71u;
        ep->cc_n = 2u;
        ep->notif_types[0] = e == 0u ? 0x06u : 0x05u;
        ep->notif_n = 1u;
    }
    const uint16_t node = include(&ms, 0u);
    until_ready(node);
    run_for(5000u);
    /* its root's inputs watched: the root is asked, each of its CCs, not its end points */
    const unsigned notif_gets = fake_zdev_asked(&ms, 0x71, 0x04);
    const unsigned binary_gets = fake_zdev_asked(&ms, 0x30, 0x02);
    const unsigned ml_gets = fake_zdev_asked(&ms, 0x31, 0x04);
    const unsigned battery_gets = fake_zdev_asked(&ms, 0x80, 0x02);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, candidate(node, 0u, ZSRC_ZW_SLOT_TEMPERATURE));
    bind(3u, candidate(node, 0u, ZSRC_ZW_SLOT_MOTION));
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(notif_gets + 1u, fake_zdev_asked(&ms, 0x71, 0x04));
    TEST_ASSERT_EQUAL_UINT(binary_gets + 2u, fake_zdev_asked(&ms, 0x30, 0x02));
    TEST_ASSERT_EQUAL_UINT(ml_gets + 1u, fake_zdev_asked(&ms, 0x31, 0x04));
    TEST_ASSERT_EQUAL_UINT(battery_gets + 1u, fake_zdev_asked(&ms, 0x80, 0x02));
    TEST_ASSERT_EQUAL_INT32(2150, info(2u).value_x100); /* its answer: 21.5 at precision 1 */
    /* an end point's input: its Notification Get inside Multi Channel, to that end point */
    size_t from = W.zac.heard;
    bind(4u, candidate(node, 1u, ZSRC_ZW_SLOT_CONTACT));
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(notif_gets + 2u, fake_zdev_asked(&ms, 0x71, 0x04));
    static const uint8_t EP1_GET[] = {0x60, 0x0D, 0x00, 0x01, 0x71, 0x04};
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, sent(from, EP1_GET, sizeof(EP1_GET)));
    /* Binary Sensor Reports by type */
    static const uint8_t DOOR_OPEN[] = {0x30, 0x03, 0xFF, 0x0A};
    static const uint8_t DOOR_SHUT[] = {0x30, 0x03, 0x00, 0x0A};
    static const uint8_t MOVING[] = {0x30, 0x03, 0xFF, 0x0C};
    static const uint8_t STILL[] = {0x30, 0x03, 0x00, 0x0C};
    say(&ms, DOOR_OPEN, sizeof(DOOR_OPEN));
    TEST_ASSERT_TRUE(info(1u).active);
    say(&ms, DOOR_SHUT, sizeof(DOOR_SHUT));
    TEST_ASSERT_FALSE(info(1u).active);
    say(&ms, MOVING, sizeof(MOVING));
    TEST_ASSERT_TRUE(info(3u).active);
    say(&ms, STILL, sizeof(STILL));
    TEST_ASSERT_FALSE(info(3u).active);
    /* Multilevel Sensor at precision 3, then in Fahrenheit */
    uint8_t ml[12];
    const zw_cc_multilevel_sensor_report_t c3 = {0x01u, 3u, 0u, 4u, 20500};
    say(&ms, ml, zw_cc_multilevel_sensor_report_build(ml, sizeof(ml), &c3));
    TEST_ASSERT_EQUAL_INT32(2050, info(2u).value_x100);
    const zw_cc_multilevel_sensor_report_t f1 = {0x01u, 1u, 1u, 2u, 707}; /* 70.7 °F */
    say(&ms, ml, zw_cc_multilevel_sensor_report_build(ml, sizeof(ml), &f1));
    TEST_ASSERT_EQUAL_INT32(2150, info(2u).value_x100);
    /* a v2 Battery Report's "replace soon" is low battery at any level */
    uint8_t bat[8];
    zw_cc_battery_report_t br;
    memset(&br, 0, sizeof(br));
    br.level = 80u;
    br.has_v2 = true;
    br.replace_or_recharge = 1u;
    say(&ms, bat, zw_cc_battery_report_build(bat, sizeof(bat), &br));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    br.replace_or_recharge = 0u;
    say(&ms, bat, zw_cc_battery_report_build(bat, sizeof(bat), &br));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    /* the cover, motion, and motion's idle naming it: the cover stays */
    notification(&ms, 0u, 0x07u, 0x03u);
    EXPECT(3u, ALARM_ZEV_TAMPER, 1u);
    notification(&ms, 0u, 0x07u, 0x08u);
    TEST_ASSERT_TRUE(info(3u).active);
    static const uint8_t MOTION_IDLE[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF,
                                          0x07, 0x00, 0x01, 0x08};
    say(&ms, MOTION_IDLE, sizeof(MOTION_IDLE));
    TEST_ASSERT_FALSE(info(3u).active);
    EXPECT(3u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    /* Hail: "ask me" */
    const unsigned hailed = fake_zdev_asked(&ms, 0x71, 0x04);
    static const uint8_t HAIL[] = {0x82, 0x01};
    say(&ms, HAIL, sizeof(HAIL));
    run_for(5000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&ms, 0x71, 0x04) > hailed);
    /* a supervised report from end point 2: its Supervision Report goes to end point 2 */
    from = W.zac.heard;
    static const uint8_t DRY[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x00, 0x00};
    uint8_t mc[24];
    uint8_t spv[40];
    const size_t k = zw_cc_wrap_multi_channel(mc, sizeof(mc), 2u, 0u, false, DRY, sizeof(DRY));
    say(&ms, spv, zw_cc_wrap_supervision(spv, sizeof(spv), 0x2Au, false, mc, k));
    static const uint8_t TO_EP2[] = {0x60, 0x0D, 0x00, 0x02, 0x6C, 0x02, 0x2A, 0xFF, 0x00};
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, sent(from, TO_EP2, sizeof(TO_EP2)));
    /* it asks the panel's version: the module's protocol (7.22), however the panel was set up */
    from = W.zac.heard;
    static const uint8_t VERSION_GET[] = {0x86, 0x11};
    say(&ms, VERSION_GET, sizeof(VERSION_GET));
    static const uint8_t VERSION_REPORT[] = {0x86, 0x12};
    const uint8_t *v = sent_at(from, VERSION_REPORT, sizeof(VERSION_REPORT), NULL);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT8(7u, v[3]);
    TEST_ASSERT_EQUAL_UINT8(22u, v[4]);
    /* a device that speaks only Basic */
    static fake_zdev_t bd;
    device(&bd, 0xBA51u, false);
    fake_zdev_listening(&bd);
    fake_zdev_no_s2(&bd);
    bd.m.secure_n = 0u;
    bd.m.notif_n = 0u;
    static const uint8_t BNIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x20};
    memcpy(bd.nif, BNIF, sizeof(BNIF));
    bd.nif_len = sizeof(BNIF);
    bd.m.versions[bd.m.versions_n].cc = 0x20u;
    bd.m.versions[bd.m.versions_n].version = 2u;
    bd.m.versions_n++;
    const uint16_t bn = include(&bd, 0u);
    until_ready(bn);
    bind(5u, candidate(bn, 0u, ZSRC_ZW_SLOT_GENERAL));
    static const uint8_t SET_ON[] = {0x20, 0x01, 0xFF};
    static const uint8_t REPORT_OFF[] = {0x20, 0x03, 0x00};
    static const uint8_t REPORT_ON[] = {0x20, 0x03, 0xFF};
    static const uint8_t SET_OFF[] = {0x20, 0x01, 0x00};
    say(&bd, SET_ON, sizeof(SET_ON));
    TEST_ASSERT_TRUE(info(5u).active);
    say(&bd, REPORT_OFF, sizeof(REPORT_OFF));
    TEST_ASSERT_FALSE(info(5u).active);
    say(&bd, REPORT_ON, sizeof(REPORT_ON));
    TEST_ASSERT_TRUE(info(5u).active);
    say(&bd, SET_OFF, sizeof(SET_OFF));
    TEST_ASSERT_FALSE(info(5u).active);
}

/* The security manager busy - its four sends taken by the owner's own commands: the bridge's Get
 * waits ZWZ_RETRY_MS and goes then, an answer queued meanwhile for the same device goes first,
 * and the ends of the owner's sends are the owner's. Then the device stays quiet for hours:
 * zw_health pings a quiet listening device, and each ACK is its check-in (network.md §8) - no
 * CHECK. ZW-012, ZW-013. */
static void test_busy_answers_first_and_quiet_devices(void)
{
    static fake_zdev_t a;
    static fake_zdev_t b;
    plain_listener(&a, 0xA0A0u);
    plain_listener(&b, 0xB0B0u);
    const uint16_t na = include(&a, 0u);
    const uint16_t nb = include(&b, 0u);
    until_ready(na);
    until_ready(nb);
    run_for(5000u);
    static const uint8_t VERSION_GET[] = {0x86, 0x11};
    for (uint32_t i = 0; i < 4u; i++) {
        const zw_s2_result_t r = zw_net_send(&W.net, W.wire.now, na, ZW_SEC_NONE, VERSION_GET,
                                             sizeof(VERSION_GET), 0x100u + i);
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, r);
    }
    const size_t from = W.zac.heard;
    const unsigned gets = fake_zdev_asked(&b, 0x71, 0x04);
    const uint32_t busy = W.bridge.stats.busy;
    bind(1u, candidate(nb, 0u, ZSRC_ZW_SLOT_WATER));
    TEST_ASSERT_TRUE(zwz_next_deadline(&W.bridge) <= W.wire.now); /* work: at once */
    run_for(1u);
    TEST_ASSERT_TRUE(W.bridge.stats.busy > busy); /* the Get waits */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&b, W.wire.now, ZW_SEC_NONE, VERSION_GET,
                                                     sizeof(VERSION_GET)));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(gets + 1u, fake_zdev_asked(&b, 0x71, 0x04));
    static const uint8_t VERSION_REPORT[] = {0x86, 0x12};
    static const uint8_t NOTIFICATION_GET[] = {0x71, 0x04};
    const size_t answer = sent(from, VERSION_REPORT, sizeof(VERSION_REPORT));
    const size_t get = sent(from, NOTIFICATION_GET, sizeof(NOTIFICATION_GET));
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, answer);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, get);
    TEST_ASSERT_TRUE(answer < get); /* the answer first */
    TEST_ASSERT_EQUAL_UINT32(4u, W.other_done);
    /* quiet for hours - with the table of whom zw_health heard full of devices long gone (they
     * make room) */
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        W.bridge.heard[i].node = (uint16_t)(900u + i);
        W.bridge.heard[i].at = 1u;
    }
    W.zev_n = 0u;
    const uint32_t heard = W.bridge.stats.heard;
    run_for(3u * HOUR_MS);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    TEST_ASSERT_TRUE(W.bridge.stats.heard >= heard + 6u);
    TEST_ASSERT_TRUE(count(1u, ALARM_ZEV_CHECKIN) >= 6u);
    TEST_ASSERT_EQUAL_UINT64(last_heard(nb), last_at(1u, ALARM_ZEV_CHECKIN)); /* at once */
}

/* network.md §2 through the bridge: a replaced (or reset) module is another network - the old
 * bindings match none of its devices and show CHECK at once, and nothing is offered; a module of
 * another system is no network at all - the source stays down: one trouble after its grace, no
 * zone CHECK (ZW-014, ZSRC-015). */
static void test_replaced_and_foreign_modules(void)
{
    static fake_zdev_t door;
    plain_listener(&door, 0xD0D0u);
    const uint16_t node = include(&door, 0u);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_WATER));
    notification(&door, 0u, 0x05u, 0x02u);
    /* a new module in its place */
    fake_zac93_init(&W.zac);
    W.zac.id.home_id = 0xC0FFEE02u;
    W.zev_n = 0u;
    boot();
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_REPLACED, zw_net_radio(&W.net));
    TEST_ASSERT_TRUE(W.src.link_up);
    zsrc_candidate_t c[4];
    TEST_ASSERT_EQUAL_UINT(0u, zone_hub_candidates(&W.hub, ZSRC_ZWAVE, NULL, 0u, c, 4u));
    run_for(3000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    /* another system's module, with a device of its own */
    fake_zac93_init(&W.zac);
    W.zac.id.home_id = 0x0BADF00Du;
    static fake_zdev_t theirs;
    plain_listener(&theirs, 0x7E1Fu);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, &theirs));
    fake_zac93_join(&W.zac, &theirs, 7u);
    W.zev_n = 0u;
    W.trouble_on = 0u;
    boot();
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_FOREIGN, zw_net_radio(&W.net));
    TEST_ASSERT_FALSE(W.src.link_up);
    run_for(4200u * 1000u + 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, W.trouble_on);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
}

/* ZW-004 through the bridge, screens.md §8 "Restart the radio": the soft reset and the start-up
 * again. The zone source's link comes back with the check, every input gets its grace to speak
 * again (ZSRC-015), and each does - the mains leak sensor at once (its state asked, its ping), the
 * sleeping door at its next check-in. Over the hours after it no zone shows CHECK and the source
 * raises no trouble; the devices keep their generations. */
static void test_restart_the_radio(void)
{
    static fake_zdev_t door;
    static fake_zdev_t leak;
    device(&door, 0x5EA1u, true);
    const uint16_t dn = include(&door, 0u);
    until_ready(dn);
    plain_listener(&leak, 0x1EA4u);
    const uint16_t ln = include(&leak, 0u);
    until_ready(ln);
    run_for(5000u);
    bind(1u, candidate(dn, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, candidate(ln, 0u, ZSRC_ZW_SLOT_WATER));
    run_for(5000u);
    const uint16_t gen = zsrc_zw_find(&W.src, dn)->generation;
    const uint32_t soft = W.zac.by_func[ZW_FUNC_SOFT_RESET];
    W.zev_n = 0u;
    W.trouble_on = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&W.net, W.wire.now, true));
    run_for(5000u);
    TEST_ASSERT_EQUAL_UINT32(soft + 1u, W.zac.by_func[ZW_FUNC_SOFT_RESET]);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&W.net)));
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, zw_net_radio(&W.net));
    TEST_ASSERT_TRUE(W.src.link_up);
    TEST_ASSERT_TRUE(info(1u).unreachable);  /* the sleeper: its grace to speak again */
    TEST_ASSERT_FALSE(info(2u).unreachable); /* the mains sensor answered at once */
    TEST_ASSERT_FALSE(info(1u).check);
    TEST_ASSERT_FALSE(info(2u).check);
    run_for(3u * HOUR_MS);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 0u);
    TEST_ASSERT_FALSE(info(1u).check);
    TEST_ASSERT_FALSE(info(2u).check);
    TEST_ASSERT_FALSE(info(1u).unreachable); /* the door checked in since */
    TEST_ASSERT_FALSE(info(2u).unreachable);
    TEST_ASSERT_EQUAL_UINT(0u, W.trouble_on);
    TEST_ASSERT_EQUAL_UINT16(gen, zsrc_zw_find(&W.src, dn)->generation);
    TEST_ASSERT_NOT_NULL(candidate(dn, 0u, ZSRC_ZW_SLOT_CONTACT));
}

/* Battery Report v2 "disconnected" (command-classes.md §4.6) reaches the zone through the
 * bridge's decode(), treated like "replace now": LOW_BATTERY while set, restored only by a later
 * report without it at 30 % or up (Z-Wave JS cc/BatteryCC.test.ts's "disconnected" case; zones.md
 * §3, ZW-013). */
static void test_battery_disconnected_reaches_zone(void)
{
    static fake_zdev_t d;
    device(&d, 0xD15Cu, false);
    fake_zdev_listening(&d);
    const uint16_t node = include(&d, ZW_S2_KEY_AUTH);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    uint8_t bat[8];
    zw_cc_battery_report_t br;
    memset(&br, 0, sizeof(br));
    br.level = 90u; /* otherwise healthy */
    br.has_v2 = true;
    br.disconnected = true;
    say(&d, bat, zw_cc_battery_report_build(bat, sizeof(bat), &br));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    br.disconnected = false;
    say(&d, bat, zw_cc_battery_report_build(bat, sizeof(bat), &br));
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
}

/* Supervision answers (command-classes.md §4.14, zwz_received): a supervised Notification Report
 * whose event parameters cannot be interpreted further still gets Success once the report itself
 * was applied - the panel does not need to understand every byte to have honestly received the
 * notification (Z-Wave JS cc-specific/notificationSupervisionUnparsableEventParams.test.ts).
 * ZW-013. */
static void test_supervised_notification_unparsable_event_succeeds(void)
{
    static fake_zdev_t d;
    device(&d, 0x50C1u, false);
    fake_zdev_listening(&d);
    const uint16_t node = include(&d, ZW_S2_KEY_AUTH);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(5000u); /* let the "fresh state" ask this bind triggers land before its own got[0] */
    /* Home Security 09 "product moved": recognised and acted on (it tampers or pulses the impact
     * input), but its event parameter (here an arbitrary, extended-CC-like byte) is never itself
     * interpreted - decode() still succeeds. */
    uint8_t param = 0xABu;
    zw_cc_notification_report_t nr;
    memset(&nr, 0, sizeof(nr));
    nr.status = 0xFFu;
    nr.notification_type = 0x07u;
    nr.event = 0x09u;
    nr.event_params = &param;
    nr.event_params_len = 1u;
    uint8_t buf[24];
    const size_t n = zw_cc_notification_report_build(buf, sizeof(buf), &nr);
    TEST_ASSERT_NOT_EQUAL(0u, n);
    uint8_t spv[40];
    const size_t k = zw_cc_wrap_supervision(spv, sizeof(spv), 0x05u, false, buf, n);
    /* the device is S2-secured, so the reply on the wire is encrypted: check what it decrypts to,
     * not the raw wire bytes (as the two-end-point test does for its own Supervision Report). */
    d.got_n = 0u;
    say(&d, spv, k);
    TEST_ASSERT_TRUE(d.got_n >= 1u);
    static const uint8_t SUCCESS[] = {0x6C, 0x02, 0x05, 0xFF, 0x00};
    TEST_ASSERT_EQUAL_UINT(sizeof(SUCCESS), d.got_len[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(SUCCESS, d.got[0], sizeof(SUCCESS));
}

/* A genuinely undecodable Notification Report (its flags byte declares more event-parameter and
 * sequence-number bytes than the frame actually carries) gets No Support, not Success (Z-Wave JS
 * cc-specific/notificationSupervisionUnparsableEventParams.test.ts's other half). ZW-013. */
static void test_supervised_notification_malformed_gets_no_support(void)
{
    static fake_zdev_t d;
    device(&d, 0x50C2u, false);
    fake_zdev_listening(&d);
    const uint16_t node = include(&d, ZW_S2_KEY_AUTH);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(5000u); /* let the "fresh state" ask this bind triggers land before its own got[0] */
    /* flags 0x81: bit 7 (a sequence number follows) set, length bits = 1 declared parameter byte -
     * the frame ends right there, one byte short of even the parameter, let alone the sequence
     * number: zw_cc_notification_report_parse must refuse it. */
    static const uint8_t MALFORMED[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x08, 0x81};
    uint8_t spv[24];
    const size_t k =
        zw_cc_wrap_supervision(spv, sizeof(spv), 0x06u, false, MALFORMED, sizeof(MALFORMED));
    d.got_n = 0u;
    say(&d, spv, k);
    TEST_ASSERT_TRUE(d.got_n >= 1u);
    static const uint8_t NO_SUPPORT[] = {0x6C, 0x02, 0x06, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT(sizeof(NO_SUPPORT), d.got_len[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(NO_SUPPORT, d.got[0], sizeof(NO_SUPPORT));
}

/* A maximum-length (31-byte, the 5-bit field's ceiling) Notification event parameter crashes
 * nothing, and the next frame from the same device is still processed normally afterward (Z-Wave
 * JS cc-specific/notificationOversizedValueParamNoCrash.test.ts). ZW-013, ZW-023. */
static void test_supervised_notification_oversized_param_no_crash(void)
{
    /* unencrypted: a 9 + 31 byte report wrapped in S2 could exceed the module's own frame limit,
     * which is not what this test is about - command-classes.md's parser bound is the point. */
    static fake_zdev_t d;
    plain_listener(&d, 0x50C3u);
    const uint16_t node = include(&d, 0u);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_WATER));
    uint8_t params[31];
    for (size_t i = 0; i < sizeof(params); i++) {
        params[i] = (uint8_t)(0x10u + i);
    }
    zw_cc_notification_report_t nr;
    memset(&nr, 0, sizeof(nr));
    nr.status = 0xFFu;
    nr.notification_type = 0x05u;
    nr.event = 0x00u; /* idle, carrying the oversized parameter */
    nr.event_params = params;
    nr.event_params_len = sizeof(params);
    uint8_t buf[48];
    const size_t n = zw_cc_notification_report_build(buf, sizeof(buf), &nr);
    TEST_ASSERT_NOT_EQUAL(0u, n);
    const uint32_t reports = W.bridge.stats.reports;
    say(&d, buf, n); /* must not crash */
    TEST_ASSERT_TRUE(W.bridge.stats.reports > reports); /* processed (a MAC retry may double it) */
    /* the next frame is still processed normally */
    notification(&d, 0u, 0x05u, 0x01u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
}

/* Multi Command (command-classes.md §4.17): one frame bundling a Notification Report and a
 * Battery Report for two different inputs of the same device - both reach the zone source (Z-Wave
 * JS compliance/handleMultiCommandPayload.test.ts). ZW-013. */
static void test_multi_command_battery_and_notification(void)
{
    static fake_zdev_t d;
    device(&d, 0x5A11u, false);
    fake_zdev_listening(&d);
    const uint16_t node = include(&d, ZW_S2_KEY_AUTH);
    until_ready(node);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    static const uint8_t DOOR_OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    static const uint8_t LOW_BATTERY[] = {0x80, 0x03, 0x0A};
    const uint8_t *cmds[2] = {DOOR_OPEN, LOW_BATTERY};
    const size_t lens[2] = {sizeof(DOOR_OPEN), sizeof(LOW_BATTERY)};
    uint8_t both[32];
    const size_t n = zw_cc_wrap_multi_command(both, sizeof(both), cmds, lens, 2u);
    say(&d, both, n);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);       /* the door notification reached the zone */
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u); /* the battery report reached it too */
}

/* Unknown CC (command-classes.md §2): logged once per node and CC, so a chatty unsupported device
 * cannot flood whatever logs this later - the bridge's own stats are the observable. ZW-013. */
static void test_unknown_cc_counted_once_per_node_and_cc(void)
{
    static fake_zdev_t a;
    static fake_zdev_t b;
    device(&a, 0x0AAAu, false);
    fake_zdev_listening(&a);
    device(&b, 0x0BBBu, false);
    fake_zdev_listening(&b);
    const uint16_t na = include(&a, 0u);
    const uint16_t nb = include(&b, 0u);
    until_ready(na);
    until_ready(nb);
    const uint32_t before = W.bridge.stats.unknown;
    static const uint8_t COLOR_GET[] = {0x33, 0x03}; /* not one of ours */
    say(&a, COLOR_GET, sizeof(COLOR_GET));
    TEST_ASSERT_EQUAL_UINT32(before + 1u, W.bridge.stats.unknown);
    say(&a, COLOR_GET, sizeof(COLOR_GET)); /* the same CC again: not counted twice */
    TEST_ASSERT_EQUAL_UINT32(before + 1u, W.bridge.stats.unknown);
    /* a different unknown CC from the same node: counted separately */
    static const uint8_t DOOR_LOCK_GET[] = {0x62, 0x02}; /* not one of ours either */
    say(&a, DOOR_LOCK_GET, sizeof(DOOR_LOCK_GET));
    TEST_ASSERT_EQUAL_UINT32(before + 2u, W.bridge.stats.unknown);
    /* the same first CC, a different node: counted again */
    say(&b, COLOR_GET, sizeof(COLOR_GET));
    TEST_ASSERT_EQUAL_UINT32(before + 3u, W.bridge.stats.unknown);
}

/* command-classes.md §5: a broadcast- or multicast-addressed Get must not be answered as if
 * singlecast - an unwanted extra reply on a shared channel (Z-Wave JS driver/
 * receiveApplicationCommandHandlerBridge.test.ts, second case). zw_net already refuses anything
 * but a singlecast receive status before it ever reaches the security manager (serial-api.md
 * §6.11's "not_singlecast", ZW-009), so the bridge's answer path is never even asked - proved end
 * to end here, since that is the observable that matters from the bridge's side. ZW-013. */
static void test_broadcast_get_not_answered(void)
{
    static fake_zdev_t d;
    plain_listener(&d, 0xB4CAu);
    const uint16_t node = include(&d, 0u);
    until_ready(node);
    run_for(5000u);
    static const uint8_t VERSION_GET[] = {0x86, 0x11};
    const uint32_t reports = W.bridge.stats.reports;
    const uint32_t answers = W.bridge.stats.answers;
    const zw_api_app_cmd_t bc = {false, 0x04u, 0u, node, VERSION_GET, sizeof(VERSION_GET),
                                NULL, 0u, false, 0};
    uint8_t p[32];
    const size_t n = zw_api_app_cmd_build(p, sizeof(p), ZW_NID_8, &bc);
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&W.zac, W.wire.now + 1u, ZW_TYPE_REQUEST,
                                        ZW_FUNC_APPLICATION_COMMAND_HANDLER, p, n));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT32(reports, W.bridge.stats.reports);
    TEST_ASSERT_EQUAL_UINT32(answers, W.bridge.stats.answers);
    /* the same Get, singlecast: answered normally (the command itself is fine) */
    say(&d, VERSION_GET, sizeof(VERSION_GET));
    TEST_ASSERT_EQUAL_UINT32(answers + 1u, W.bridge.stats.answers);
}

/* zones.md §8's no_supervision through the bridge: a model whose row says its firmware answers
 * supervised commands "fail" (an unknown firmware counts as below the row's - the node cache keeps
 * none) is interviewed without Supervision, so its Sets are taken and its interview ends. */
static void test_quirk_without_supervision(void)
{
    static const zsrc_zw_quirk_t ROWS[] = {
        {.manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE001u,
         .name = "Old door", .no_supervision_before = 0x0200u},
    };
    zsrc_zw_set_quirks(&W.src, ROWS, 1u);
    static fake_zdev_t door;
    device(&door, 0xD00Cu, true);
    door.m.supervision_fail = true; /* what its firmware answers */
    const uint16_t node = include(&door, 0u);
    until_ready(node);
    TEST_ASSERT_EQUAL_UINT32(0u, door.supervised);
    TEST_ASSERT_EQUAL_UINT32(3600u, door.m.wake_interval);
    TEST_ASSERT_EQUAL_UINT8(1u, door.m.wake_node);
    TEST_ASSERT_EQUAL_STRING("Old door", candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT)->name);
}

/* ZW-013, ZW-011, network.md section 7 "the bridge's outbox": a sleeping S2 door watched while
 * it sleeps has its fresh-state Get held for its next wake-up. When it then sends a supervised
 * open, the Supervision Report goes to zw_net at once - out while the held Get has not completed
 * (zw_net sends it straight away, in the reply class, and the device stays asleep: B026) -
 * instead of waiting in the bridge for the Get's end; the open reaches the zone, and the Get goes
 * at the device's wake-up. */
static void test_held_get_does_not_hold_the_answer(void)
{
    static fake_zdev_t door;
    device(&door, 0x5E1Du, true);
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    run_for(5000u); /* back to sleep */
    TEST_ASSERT_FALSE(fake_zdev_awake(&door, W.wire.now));
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(MIN_MS);
    TEST_ASSERT_TRUE(get_held(node)); /* asleep: its Get waits for it */
    const unsigned reports = fake_zdev_asked(&door, 0x6C, 0x02);
    const unsigned gets = fake_zdev_asked(&door, 0x71, 0x04);
    uint8_t spv[24];
    const size_t n = supervised_open(spv, sizeof(spv), 0x31u);
    TEST_ASSERT_TRUE_MESSAGE(say_and_watch(&door, spv, n, 3000u),
                             "the Supervision Report waited for the held Get");
    TEST_ASSERT_EQUAL_UINT(reports + 1u, fake_zdev_asked(&door, 0x6C, 0x02));
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    /* B026: a report it supervised is no wake-up - its Get waits for one */
    TEST_ASSERT_EQUAL_UINT(gets, fake_zdev_asked(&door, 0x71, 0x04));
    TEST_ASSERT_TRUE(get_held(node));
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&door, 0x71, 0x04) > gets);
    TEST_ASSERT_FALSE(get_held(node));
    TEST_ASSERT_EQUAL_UINT32(0u, W.bridge.stats.replies_lost);
}

/* ZW-013, network.md section 7: a Get the device did not hear (no ACK) goes back in its wake-up
 * queue with no end told, so the bridge holds it until the device's next wake-up - and still the
 * device's supervised report is answered at once, not after that Get. */
static void test_noack_get_does_not_block_the_answer(void)
{
    static fake_zdev_t door;
    plain_sleeper(&door, 0x0ACBu);
    const uint16_t node = include(&door, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    run_for(5000u);
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT));
    run_for(MIN_MS);
    TEST_ASSERT_TRUE(get_held(node));
    const unsigned gets = fake_zdev_asked(&door, 0x71, 0x04);
    door.lose_in = 1u; /* its Get, at the wake-up, is lost on the air */
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(gets, fake_zdev_asked(&door, 0x71, 0x04));
    TEST_ASSERT_TRUE(get_held(node)); /* no end told: held for the next wake-up */
    run_for(MIN_MS);
    TEST_ASSERT_FALSE(fake_zdev_awake(&door, W.wire.now));
    const unsigned reports = fake_zdev_asked(&door, 0x6C, 0x02);
    uint8_t spv[24];
    const size_t n = supervised_open(spv, sizeof(spv), 0x32u);
    TEST_ASSERT_TRUE_MESSAGE(say_and_watch(&door, spv, n, 3000u),
                             "the Supervision Report waited for the unheard Get");
    TEST_ASSERT_EQUAL_UINT(reports + 1u, fake_zdev_asked(&door, 0x6C, 0x02));
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT(gets, fake_zdev_asked(&door, 0x71, 0x04)); /* B026: no wake-up */
    TEST_ASSERT_TRUE(get_held(node));
    fake_zdev_wake(&door, W.wire.now);
    run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(gets + 1u, fake_zdev_asked(&door, 0x71, 0x04)); /* it went again */
    TEST_ASSERT_FALSE(get_held(node));
    TEST_ASSERT_EQUAL_UINT32(0u, W.bridge.stats.replies_lost);
}

/* Which of the ghosts' SendData frames the module heard since `from`: Supervision Reports and
 * Notification Gets, by node (first..first+n-1). */
static size_t scan_ghosts(size_t from, uint16_t first, size_t n, bool *sr, bool *get)
{
    TEST_ASSERT_TRUE_MESSAGE(W.zac.heard - from <= 64u, "the module's log wrapped");
    for (size_t i = from; i < W.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&W.zac, i);
        if (h == NULL || h->func != ZW_FUNC_SEND_DATA || h->payload_len < 4u ||
            (size_t)h->payload[0] < (size_t)first || (size_t)h->payload[0] >= (size_t)first + n) {
            continue;
        }
        const size_t k = (size_t)h->payload[0] - (size_t)first;
        sr[k] = sr[k] || (h->payload[2] == 0x6Cu && h->payload[3] == 0x02u);
        get[k] = get[k] || (h->payload[2] == 0x71u && h->payload[3] == 0x04u);
    }
    return W.zac.heard;
}

/* ZW-013, ZW-011, network.md section 7 "the bridge's outbox": 40 watched sleepers (more than the
 * old 32-slot outbox) after the panel starts - every one gets its fresh-state Get held (a Get
 * slot for each device; a Get without room is proved not skipped by
 * test_get_without_room_is_asked_later); each one's supervised report is answered at once while
 * its Get stays held (a report is no wake-up, B026), and each Get goes when its device wakes. The
 * 40 are copies of a real door's record (the fake module holds 12 devices): the module lists
 * them and takes every SendData as heard. */
static void test_forty_sleepers_answered_and_asked(void)
{
    enum { GHOSTS = 40, FIRST = 20 };
    static fake_zdev_t door;
    plain_sleeper(&door, 0x40D0u);
    const uint16_t node = include(&door, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    static zw_node_cache_t cache;
    cache = *zw_net_nodes(&W.net);
    const zw_node_record_t tmpl = *zw_node_find_const(&cache, node);
    for (uint16_t i = 0u; i < GHOSTS; i++) {
        const uint16_t id = (uint16_t)(FIRST + i);
        zw_node_record_t *r = zw_node_add(&cache, id);
        TEST_ASSERT_NOT_NULL(r);
        *r = tmpl;
        r->node_id = id;
        zw_node_list_set(W.zac.id.node_list, id, true);
    }
    W.nodes_len = zw_node_cache_encode(&cache, W.nodes, sizeof(W.nodes));
    TEST_ASSERT_NOT_EQUAL(0u, W.nodes_len);
    fake_rule_t heard;
    memset(&heard, 0, sizeof(heard));
    heard.func = ZW_FUNC_SEND_DATA;
    heard.respond = true;
    heard.response.len = 1u;
    heard.response.bytes[0] = 0x01u;
    heard.callbacks = 1u;
    heard.callback[0].len = 1u;
    heard.callback[0].bytes[0] = 0x00u; /* transmit OK */
    heard.callback[0].after_ms = 20u;
    TEST_ASSERT_TRUE(fake_zac93_on(&W.zac, &heard));
    boot(); /* the panel restarts with them */
    run_for(5000u);
    for (uint16_t i = 0u; i < GHOSTS; i++) {
        bind((uint16_t)(1u + i), candidate((uint16_t)(FIRST + i), 0u, ZSRC_ZW_SLOT_CONTACT));
    }
    const uint32_t dropped = W.bridge.stats.dropped;
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT(GHOSTS, gets_held()); /* every one held for its wake-up */
    TEST_ASSERT_EQUAL_UINT32(dropped, W.bridge.stats.dropped);
    static bool sr[GHOSTS];
    static bool get[GHOSTS];
    memset(sr, 0, sizeof(sr));
    memset(get, 0, sizeof(get));
    static fake_zdev_t ghost;
    size_t from = W.zac.heard;
    for (uint16_t i = 0u; i < GHOSTS; i++) {
        ghost.node = (uint16_t)(FIRST + i);
        uint8_t spv[24];
        const size_t n = supervised_open(spv, sizeof(spv), (uint8_t)(0x10u + i));
        fake_zac93_from_device(&W.zac, W.wire.now, &ghost, spv, n);
        run_for(100u);
        from = scan_ghosts(from, FIRST, GHOSTS, sr, get);
    }
    run_for(2000u);
    from = scan_ghosts(from, FIRST, GHOSTS, sr, get);
    for (size_t i = 0; i < GHOSTS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(sr[i], "a supervised report went unanswered");
        TEST_ASSERT_FALSE_MESSAGE(get[i], "a Get went to a device asleep");
        EXPECT((uint16_t)(1u + i), ALARM_ZEV_FAULT, 1u);
    }
    TEST_ASSERT_EQUAL_UINT(GHOSTS, gets_held());
    static const uint8_t WOKE[] = {0x84, 0x07}; /* Wake Up Notification */
    for (uint16_t i = 0u; i < GHOSTS; i++) {
        ghost.node = (uint16_t)(FIRST + i);
        fake_zac93_from_device(&W.zac, W.wire.now, &ghost, WOKE, sizeof(WOKE));
        run_for(100u);
        from = scan_ghosts(from, FIRST, GHOSTS, sr, get);
    }
    run_for(2000u);
    (void)scan_ghosts(from, FIRST, GHOSTS, sr, get);
    for (size_t i = 0; i < GHOSTS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(get[i], "a Get was skipped");
    }
    TEST_ASSERT_EQUAL_UINT(0u, gets_held());
    TEST_ASSERT_EQUAL_UINT32(0u, W.bridge.stats.replies_lost);
    TEST_ASSERT_EQUAL_UINT32(dropped, W.bridge.stats.dropped);
}

/* ZW-013, network.md section 7: answers beyond the outbox's room are counted (replies_lost, and
 * in dropped), never lost silently - nor one zw_net refuses. */
static void test_answers_without_room_are_counted(void)
{
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    zw_s2_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.security = ZW_SEC_NONE;
    rx.supervised = true;
    rx.command = OPEN;
    rx.len = sizeof(OPEN);
    const uint32_t dropped = W.bridge.stats.dropped;
    for (size_t i = 0; i <= ZWZ_OUTBOX; i++) {
        rx.node = (uint16_t)(100u + i); /* nodes the network does not have */
        rx.supervision_session = (uint8_t)(i & 0x3Fu);
        zwz_received(&W.bridge, &rx, W.wire.now);
    }
    TEST_ASSERT_EQUAL_UINT32(1u, W.bridge.stats.replies_lost); /* the one without room */
    TEST_ASSERT_EQUAL_UINT32(dropped + 1u, W.bridge.stats.dropped);
    run_for(1000u); /* zw_net refuses the rest: unknown nodes */
    TEST_ASSERT_EQUAL_UINT32(ZWZ_OUTBOX + 1u, W.bridge.stats.replies_lost);
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        TEST_ASSERT_FALSE(W.bridge.out[i].used);
    }
}

/* This device's fresh-state refresh in the bridge (NULL: none). */
static const zwz_refresh_t *refresh_of(uint16_t node)
{
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (W.bridge.refresh[i].node == node) {
            return &W.bridge.refresh[i];
        }
    }
    return NULL;
}

/* ZW-013, network.md section 7 "the bridge's outbox": a Get that finds the outbox full (here,
 * every slot an answer owed) is not skipped - its refresh stays at that Get, which is asked at a
 * later poll, once there is room; it is not counted as dropped, and the answers are not held
 * back by it (the bridge's retry for a busy security manager stays clear). */
static void test_get_without_room_is_asked_later(void)
{
    static const uint8_t OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    static fake_zdev_t door;
    plain_sleeper(&door, 0x0F11u);
    const uint16_t node = include(&door, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    until_ready(node);
    run_for(5000u);
    TEST_ASSERT_NULL(refresh_of(node)); /* not watched: nothing to ask */
    const unsigned asked = fake_zdev_asked(&door, 0x71, 0x04);
    /* the outbox full of answers owed, no poll in between */
    zw_s2_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.security = ZW_SEC_NONE;
    rx.supervised = true;
    rx.command = OPEN;
    rx.len = sizeof(OPEN);
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        rx.node = (uint16_t)(100u + i); /* nodes the network does not have */
        rx.supervision_session = (uint8_t)(i & 0x3Fu);
        zwz_received(&W.bridge, &rx, W.wire.now);
    }
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        TEST_ASSERT_TRUE(W.bridge.out[i].used);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, W.bridge.stats.replies_lost); /* each one found room */
    const uint32_t gets = W.bridge.stats.gets;
    const uint32_t dropped = W.bridge.stats.dropped;
    bind(1u, candidate(node, 0u, ZSRC_ZW_SLOT_CONTACT)); /* watched: the source wants its state */
    zwz_poll(&W.bridge, W.wire.now); /* its Get finds no room (the Gets go before the answers) */
    const zwz_refresh_t *f = refresh_of(node);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT8(0u, f->next); /* still at its first Get: not skipped */
    TEST_ASSERT_EQUAL_UINT32(gets, W.bridge.stats.gets);
    /* only the answers zw_net refused are dropped: the Get waits, it is not dropped */
    TEST_ASSERT_EQUAL_UINT32(dropped + W.bridge.stats.replies_lost, W.bridge.stats.dropped);
    TEST_ASSERT_EQUAL_UINT64(0u, W.bridge.retry_at);           /* the answers not held back */
    TEST_ASSERT_TRUE(W.bridge.get_retry_at > W.wire.now);
    run_for(1000u); /* zw_net refuses the answers (unknown nodes): their slots free up */
    TEST_ASSERT_EQUAL_UINT32(ZWZ_OUTBOX, W.bridge.stats.replies_lost);
    TEST_ASSERT_EQUAL_UINT32(gets + 1u, W.bridge.stats.gets);
    TEST_ASSERT_TRUE_MESSAGE(get_held(node) || fake_zdev_asked(&door, 0x71, 0x04) > asked,
                             "the Get that found no room was never asked");
    TEST_ASSERT_EQUAL_UINT64(0u, W.bridge.get_retry_at);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_secure_door_sensor);
    RUN_TEST(test_confirmed_check_in_moves_the_grace);
    RUN_TEST(test_reread_interval_moves_the_grace);
    RUN_TEST(test_reread_interval_zero_not_supervised);
    RUN_TEST(test_insecure_motion_sensor);
    RUN_TEST(test_v1_smoke_alarm_by_quirk);
    RUN_TEST(test_battery_reread_daily);
    RUN_TEST(test_alarm_sensor_only_device);
    RUN_TEST(test_alarm_sensor_fresh_state);
    RUN_TEST(test_two_endpoints_and_the_radio);
    RUN_TEST(test_meter_sensor_support_falls_back_to_the_node);
    RUN_TEST(test_meter_sensor_full_endpoint_list_falls_back);
    RUN_TEST(test_keyless_broadcast_report_reaches_zone);
    RUN_TEST(test_kex_and_commands_supported_get_heard);
    RUN_TEST(test_restart_and_fresh_state);
    RUN_TEST(test_removed_and_node_id_reused);
    RUN_TEST(test_describe_from_the_node_cache);
    RUN_TEST(test_sleeping_end_points_asked_one_at_a_time);
    RUN_TEST(test_supervised_report_is_no_wake_up);
    RUN_TEST(test_awake_reports_answered_at_once);
    RUN_TEST(test_no_lifeline_offers_nothing);
    RUN_TEST(test_every_input_asked_and_decoded);
    RUN_TEST(test_busy_answers_first_and_quiet_devices);
    RUN_TEST(test_replaced_and_foreign_modules);
    RUN_TEST(test_restart_the_radio);
    RUN_TEST(test_battery_disconnected_reaches_zone);
    RUN_TEST(test_supervised_notification_unparsable_event_succeeds);
    RUN_TEST(test_supervised_notification_malformed_gets_no_support);
    RUN_TEST(test_supervised_notification_oversized_param_no_crash);
    RUN_TEST(test_multi_command_battery_and_notification);
    RUN_TEST(test_unknown_cc_counted_once_per_node_and_cc);
    RUN_TEST(test_broadcast_get_not_answered);
    RUN_TEST(test_quirk_without_supervision);
    RUN_TEST(test_held_get_does_not_hold_the_answer);
    RUN_TEST(test_noack_get_does_not_block_the_answer);
    RUN_TEST(test_forty_sleepers_answered_and_asked);
    RUN_TEST(test_answers_without_room_are_counted);
    RUN_TEST(test_get_without_room_is_asked_later);
    return UNITY_END();
}
