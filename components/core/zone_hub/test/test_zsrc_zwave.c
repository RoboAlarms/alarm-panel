/*
 * test_zsrc_zwave.c - Z-Wave devices as zones (components/core/zone_hub/src/zsrc_zwave.c,
 * docs/features/36 zones.md): addresses and generations, the slots a device offers, every row of
 * zones.md §3's table, momentary holds, check-ins and reachability, trust per device, fresh
 * state, the source's link and the quirks table - driven through the real hub, with decoded facts
 * and reports as components/core/zwave_zones hands them over (no Z-Wave stack here: that is the
 * point of keeping the source portable).
 *
 * Requirements: ZW-013 (reports to zone events), ZW-014 (addresses), ZW-015 (trust per device),
 * ZW-043 (V1 alarms and Alarm Sensor water/general) (docs/features/36); ZSRC-005, ZSRC-007, ZSRC-010, ZSRC-015 (docs/features/22).
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zwave.h"

/* ---- harness ------------------------------------------------------------------------------- */

#define HOME 0xC0FFEE01u
#define MIN_MS 60000u

typedef struct {
    uint16_t zone;
    alarm_zone_event_t ev;
} rec_t;

static zone_hub_t H;
static zsrc_zw_t Z;
static uint64_t NOW;
static rec_t R[512];
static unsigned NR;
static unsigned TROUBLE_ON, TROUBLE_OFF;
static unsigned LINK_NOW; /* sink.link_now calls */

static void port_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    TEST_ASSERT_TRUE(NR < sizeof(R) / sizeof(R[0]));
    R[NR].zone = zone;
    R[NR].ev = ev;
    NR++;
}

static void port_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZWAVE, kind);
    if (on) {
        TROUBLE_ON++;
    } else {
        TROUBLE_OFF++;
    }
}

static unsigned count(uint16_t zone, alarm_zone_event_t ev)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NR; i++) {
        n += R[i].zone == zone && R[i].ev == ev;
    }
    return n;
}

static void clear(void)
{
    NR = 0;
    TROUBLE_ON = 0;
    TROUBLE_OFF = 0;
}

#define EXPECT(z_, e_, n_) TEST_ASSERT_EQUAL_UINT_MESSAGE((n_), count((z_), (e_)), #e_)
#define EXPECT_LAST(z_, e_)                             \
    do {                                                \
        TEST_ASSERT_TRUE(NR > 0u);                      \
        TEST_ASSERT_EQUAL_UINT16((z_), R[NR - 1].zone); \
        TEST_ASSERT_EQUAL_INT((e_), R[NR - 1].ev);      \
    } while (0)

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                       uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H, a, state, value, now);
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    zone_hub_event(&H, a, ev, now);
}

static void sink_reachable(void *ctx, const zsrc_address_t *a, bool ok, uint64_t now)
{
    (void)ctx;
    zone_hub_reachable(&H, a, ok, now);
}

static void sink_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    zone_hub_link(&H, kind, up, now);
}

static void sink_link_now(void *ctx, uint8_t kind, uint64_t now)
{
    (void)ctx;
    LINK_NOW++;
    zone_hub_link_now(&H, kind, now);
}

static void advance(uint64_t ms)
{
    const uint64_t end = NOW + ms;
    while (NOW < end) {
        const uint64_t next = zone_hub_next_deadline(&H, NOW);
        NOW = next > NOW && next < end ? next : end;
        zone_hub_tick(&H, NOW);
    }
}

void setUp(void)
{
    NOW = 1000000u;
    clear();
    static const zone_hub_port_t port = {NULL, port_event, NULL, port_trouble};
    zone_hub_init(&H, &port);
    LINK_NOW = 0u;
    const zsrc_sink_t sink = {NULL, sink_input, sink_event, sink_reachable, sink_link,
                              sink_link_now};
    zsrc_zw_init(&Z, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_zw_ops(), &Z, NOW));
    zone_hub_start(&H, NOW);
    zsrc_zw_network(&Z, HOME);
}

void tearDown(void)
{
}

/* ---- devices, as zwave_zones describes them -------------------------------------------------- */

#define EV(e) (1u << (e))

static zsrc_zw_desc_t base_desc(uint16_t node, uint16_t gen, bool sleeps)
{
    zsrc_zw_desc_t d;
    memset(&d, 0, sizeof(d));
    d.node = node;
    d.generation = gen;
    d.security = ZSRC_ZW_SEC_S2_AUTH;
    d.listening = !sleeps;
    d.check_in_s = sleeps ? 3600u : 0u;
    d.ep[0].inputs = ZSRC_ZW_INPUTS_OFFERED;
    return d;
}

static void add_notif(zsrc_zw_desc_t *d, uint8_t ep, uint8_t type, uint32_t events)
{
    zsrc_zw_endpoint_desc_t *e = &d->ep[ep];
    e->notif[e->notif_count].type = type;
    e->notif[e->notif_count].events = events;
    e->notif_count++;
}

/* A Zooz ZSE41: door open/closed, cover tamper, battery; S2 Authenticated, hourly check-in. */
static zsrc_zw_desc_t door(uint16_t node, uint16_t gen)
{
    zsrc_zw_desc_t d = base_desc(node, gen, true);
    d.has_identity = true;
    d.manufacturer = 0x027Au;
    d.product_type = 0x7000u;
    d.product_id = 0xE001u;
    add_notif(&d, 0, 0x06u, EV(0x16) | EV(0x17));
    add_notif(&d, 0, 0x07u, EV(0x03));
    return d;
}

/* A motion sensor without a cover tamper event (so "moved" is its impact input). */
static zsrc_zw_desc_t pir(uint16_t node, uint16_t gen)
{
    zsrc_zw_desc_t d = base_desc(node, gen, true);
    d.security = ZSRC_ZW_SEC_S2_UNAUTH;
    add_notif(&d, 0, 0x07u, EV(0x07) | EV(0x08) | EV(0x09) | EV(0x05));
    return d;
}

/* A combination smoke / CO / heat listener with maintenance and system events, mains powered. */
static zsrc_zw_desc_t smoke_co(uint16_t node, uint16_t gen)
{
    zsrc_zw_desc_t d = base_desc(node, gen, false);
    add_notif(&d, 0, 0x01u, EV(1) | EV(2) | EV(3) | EV(4) | EV(5) | EV(6) | EV(7) | EV(8));
    add_notif(&d, 0, 0x02u, EV(1) | EV(2) | EV(3) | EV(4) | EV(5) | EV(6));
    add_notif(&d, 0, 0x04u, EV(1) | EV(2) | EV(5) | EV(6) | EV(7) | EV(8));
    add_notif(&d, 0, 0x09u, EV(1) | EV(2) | EV(3) | EV(4) | EV(5) | EV(6));
    add_notif(&d, 0, 0x08u, EV(2) | EV(3) | EV(0x0A) | EV(0x0B) | EV(0x0E) | EV(0x0F) | EV(0x10));
    return d;
}

static void add(const zsrc_zw_desc_t *d)
{
    TEST_ASSERT_TRUE(zsrc_zw_device(&Z, d, NOW));
}

static void up(void)
{
    zsrc_zw_link(&Z, true, NOW);
}

static zsrc_address_t addr(uint16_t node, uint16_t gen, uint8_t ep, uint8_t slot)
{
    return zsrc_zwave_address(HOME, gen, node, ep, slot);
}

/* An address to pass by pointer, several in one expression. */
static const zsrc_address_t *A(uint16_t node, uint16_t gen, uint8_t ep, uint8_t slot)
{
    static zsrc_address_t buf[4];
    static unsigned k;
    zsrc_address_t *p = &buf[k++ % 4u];
    *p = addr(node, gen, ep, slot);
    return p;
}

static void bind_m(uint16_t zone, zsrc_address_t a, const zsrc_mapping_t *m)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = a;
    b.in[ZSRC_ROLE_MAIN].map = *m;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
}

static void bind(uint16_t zone, zsrc_address_t a)
{
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_GENERIC);
    bind_m(zone, a, &m);
}

static void report(uint16_t node, const zsrc_zw_report_t *r)
{
    zsrc_zw_report(&Z, node, r, NOW);
}

static void notif_p(uint16_t node, uint8_t ep, uint8_t type, uint8_t event, int param)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_NOTIFICATION;
    r.endpoint = ep;
    r.notif_status = 0xFFu;
    r.notif_type = type;
    r.notif_event = event;
    if (param >= 0) {
        r.notif_param_len = 1u;
        r.notif_params[0] = (uint8_t)param;
    }
    report(node, &r);
}

static void notif(uint16_t node, uint8_t type, uint8_t event)
{
    notif_p(node, 0u, type, event, -1);
}

static void wake(uint16_t node)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_WAKE_UP;
    report(node, &r);
}

static void frame(uint16_t node)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_FRAME;
    report(node, &r);
}

static void binary(uint16_t node, uint8_t ep, int type, bool on)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_BINARY;
    r.endpoint = ep;
    r.binary_has_type = type >= 0;
    r.binary_type = type >= 0 ? (uint8_t)type : 0u;
    r.binary_on = on;
    report(node, &r);
}

static void battery(uint16_t node, uint8_t level, uint8_t replace)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_BATTERY;
    r.battery_level = level;
    r.battery_replace = replace;
    report(node, &r);
}

static void temperature(uint16_t node, uint8_t ep, uint8_t type, uint8_t scale, int32_t v)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_MULTILEVEL;
    r.endpoint = ep;
    r.ml_type = type;
    r.ml_scale = scale;
    r.ml_value_x100 = v;
    report(node, &r);
}

static void alarm_sensor(uint16_t node, uint8_t type, uint8_t state)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_ALARM_SENSOR;
    r.alarm_type = type;
    r.alarm_state = state;
    report(node, &r);
}

static void basic(uint16_t node, uint8_t ep, uint8_t value)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_BASIC;
    r.endpoint = ep;
    r.basic_value = value;
    report(node, &r);
}

static uint32_t offered(uint16_t node, uint8_t ep)
{
    const zsrc_zw_device_t *d = zsrc_zw_find(&Z, node);
    TEST_ASSERT_NOT_NULL(d);
    return d->offered[ep];
}

#define S(slot) (1u << (slot))

static zone_hub_info_t info(uint16_t zone)
{
    zone_hub_info_t i;
    TEST_ASSERT_TRUE(zone_hub_info(&H, zone, &i));
    return i;
}

/* ---- addresses (ZW-014) -------------------------------------------------------------------- */

/* ZW-014, zones.md §1: kind 7; id = home ID << 32 | generation << 16 | node ID; input =
 * endpoint (bits 7..5) | slot (bits 4..0); the text part empty. */
static void test_address_layout(void)
{
    TEST_ASSERT_EQUAL_UINT8(7u, ZSRC_ZWAVE);
    const zsrc_address_t a = zsrc_zwave_address(0x12345678u, 0x0203u, 0x0045u, 2u, 7u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZWAVE, a.kind);
    TEST_ASSERT_EQUAL_HEX64(0x1234567802030045ull, a.id);
    TEST_ASSERT_EQUAL_HEX8(0x47u, a.input);
    TEST_ASSERT_EQUAL_STRING("", a.ref);
    uint32_t home = 0u;
    uint16_t gen = 0u;
    uint16_t node = 0u;
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    TEST_ASSERT_TRUE(zsrc_zw_address_parse(&a, &home, &gen, &node, &ep, &slot));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, home);
    TEST_ASSERT_EQUAL_UINT16(0x0203u, gen);
    TEST_ASSERT_EQUAL_UINT16(0x45u, node);
    TEST_ASSERT_EQUAL_UINT8(2u, ep);
    TEST_ASSERT_EQUAL_UINT8(7u, slot);
    /* not a Z-Wave input: another kind, no slot, no node or generation, a text part */
    zsrc_address_t bad = a;
    bad.kind = ZSRC_ZIGBEE;
    TEST_ASSERT_FALSE(zsrc_zw_address_parse(&bad, &home, &gen, &node, &ep, &slot));
    bad = zsrc_zwave_address(HOME, 1u, 5u, 0u, 0u);
    TEST_ASSERT_FALSE(zsrc_zw_address_parse(&bad, &home, &gen, &node, &ep, &slot));
    bad = zsrc_zwave_address(HOME, 1u, 0u, 0u, 1u);
    TEST_ASSERT_FALSE(zsrc_zw_address_parse(&bad, &home, &gen, &node, &ep, &slot));
    bad = zsrc_zwave_address(HOME, 0u, 5u, 0u, 1u);
    TEST_ASSERT_FALSE(zsrc_zw_address_parse(&bad, &home, &gen, &node, &ep, &slot));
    bad = zsrc_zwave_address(HOME, 1u, 5u, 0u, 1u);
    bad.ref[0] = 'x';
    TEST_ASSERT_FALSE(zsrc_zw_address_parse(&bad, &home, &gen, &node, &ep, &slot));
    /* the watch refuses what is not an address of this source */
    TEST_ASSERT_FALSE(zsrc_zw_ops()->watch(&Z, &bad, NULL, NOW));
}

/* ZW-014: a node ID given out again (a new generation) never feeds the old binding - that zone
 * shows CHECK at once ("sensor removed"); the new device is a new address; a replaced device
 * (network.md §10: the same generation) feeds its zones again. */
static void test_generations(void)
{
    zsrc_zw_desc_t none;
    memset(&none, 0, sizeof(none));
    TEST_ASSERT_FALSE(zsrc_zw_device(&Z, &none, NOW)); /* no node ID, no generation */
    none.node = 5u;
    TEST_ASSERT_FALSE(zsrc_zw_device(&Z, &none, NOW));
    const zsrc_zw_desc_t d1 = door(5u, 1u);
    add(&d1);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(5u, 0x06u, 0x17u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    /* it fails and is replaced (network.md §10): the same generation feeds its zone again */
    clear();
    zsrc_zw_device_gone(&Z, 5u, 1u, NOW);
    advance(999u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    advance(2u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u); /* ZSRC_ZW_GONE_GRACE_S: "sensor removed" */
    char words[64];
    const zsrc_address_t old = addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT);
    zsrc_zw_ops()->describe(&Z, &old, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Z-Wave device 5 · door contact (removed)", words);
    TEST_ASSERT_FALSE(zsrc_zw_ops()->valid(&Z, &old));
    add(&d1);
    EXPECT(1u, ALARM_ZEV_REACHABLE, 1u);
    notif(5u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_FALSE(info(1u).check);
    /* it is removed, and another device gets node ID 5 again: generation 2 */
    zsrc_zw_device_gone(&Z, 5u, 1u, NOW);
    advance(1001u);
    const zsrc_zw_desc_t d2 = door(5u, 2u);
    add(&d2);
    clear();
    notif(5u, 0x06u, 0x17u);
    notif(5u, 0x06u, 0x16u);
    TEST_ASSERT_EQUAL_UINT(0u, NR); /* never the old zone */
    TEST_ASSERT_TRUE(info(1u).check);
    const zsrc_address_t now_addr = addr(5u, 2u, 0u, ZSRC_ZW_SLOT_CONTACT);
    TEST_ASSERT_TRUE(zsrc_zw_ops()->valid(&Z, &now_addr));
    TEST_ASSERT_FALSE(zsrc_zw_ops()->valid(&Z, &old));
    zsrc_zw_ops()->describe(&Z, &(zsrc_address_t){0}, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Z-Wave (not an input)", words);
    /* a newer generation taking the node ID of a device still there: that one is gone */
    bind(2u, now_addr);
    clear();
    const zsrc_zw_desc_t d3 = door(5u, 3u);
    add(&d3);
    advance(1001u);
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 1u);
}

/* ZW-014, zones.md §1: a reset or replaced module (another home ID) - no old binding matches:
 * CHECK at once when the network comes up, and no device of the new network feeds it. */
static void test_new_network(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(5u, 0x06u, 0x17u);
    zsrc_zw_link(&Z, false, NOW);
    zsrc_zw_network(&Z, 0xC0FFEE02u);
    TEST_ASSERT_NULL(zsrc_zw_find(&Z, 5u)); /* none of the old devices is in it */
    add(&d);                                /* node 5, generation 1, of the new network */
    clear();
    up();
    notif(5u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    advance(1001u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    char words[64];
    zsrc_zw_ops()->describe(&Z, A(5u, 1u, 0u, 1u), words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Z-Wave device 5 · door contact (another network)", words);
}

/* ---- what a device offers (zones.md §2) ---------------------------------------------------- */

/* zones.md §2: a slot is offered when the interview shows the device can report it; tamper and
 * battery are not slots; nothing from pull-mode, lifeline-less, interviewing or controller
 * devices; endpoints beyond the device's. */
static void test_slots_offered(void)
{
    zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT), offered(5u, 0u)); /* the cover: no slot */
    const zsrc_zw_desc_t p = pir(6u, 1u);
    add(&p);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_MOTION) | S(ZSRC_ZW_SLOT_GLASS_BREAK) |
                                S(ZSRC_ZW_SLOT_IMPACT),
                            offered(6u, 0u)); /* no cover: "moved" is its impact input */
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_SMOKE) | S(ZSRC_ZW_SLOT_CO) | S(ZSRC_ZW_SLOT_HEAT) |
                                S(ZSRC_ZW_SLOT_FREEZE) | S(ZSRC_ZW_SLOT_MAINS),
                            offered(7u, 0u));
    /* every notification type's inputs */
    zsrc_zw_desc_t all = base_desc(8u, 1u, true);
    add_notif(&all, 0, 0x05u, EV(1) | EV(2));                 /* water */
    add_notif(&all, 0, 0x07u, EV(1) | EV(2) | EV(0x0A));      /* intrusion, impact */
    add_notif(&all, 0, 0x0Au, EV(1) | EV(2) | EV(3) | EV(4)); /* emergency */
    add_notif(&all, 0, 0x10u, EV(3));                         /* freeze */
    add_notif(&all, 0, 0x12u, EV(1) | EV(2) | EV(3) | EV(4)); /* gas */
    add(&all);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_WATER) | S(ZSRC_ZW_SLOT_INTRUSION) |
                                S(ZSRC_ZW_SLOT_IMPACT) | S(ZSRC_ZW_SLOT_EMERGENCY) |
                                S(ZSRC_ZW_SLOT_FREEZE) | S(ZSRC_ZW_SLOT_GAS),
                            offered(8u, 0u));
    /* Binary Sensor types, a v1 Binary Sensor, air temperature, Alarm Sensor */
    zsrc_zw_desc_t b = base_desc(9u, 1u, true);
    b.ep[0].binary = true;
    b.ep[0].binary_types = (1u << 0x0A) | (1u << 0x0B) | (1u << 0x0C) | (1u << 0x0D) |
                           (1u << 0x06) | (1u << 0x08) | (1u << 0x04);
    b.ep[0].air_temperature = true;
    b.ep[0].alarm_types = (1u << 0x01) | (1u << 0x02) | (1u << 0x04) | (1u << 0x05);
    add(&b);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT) | S(ZSRC_ZW_SLOT_TILT) |
                                S(ZSRC_ZW_SLOT_MOTION) | S(ZSRC_ZW_SLOT_GLASS_BREAK) |
                                S(ZSRC_ZW_SLOT_WATER) | S(ZSRC_ZW_SLOT_TEMPERATURE) |
                                S(ZSRC_ZW_SLOT_SMOKE) | S(ZSRC_ZW_SLOT_CO) | S(ZSRC_ZW_SLOT_HEAT),
                            offered(9u, 0u)); /* type 08 is its tamper; CO2 and water alarm: no */
    zsrc_zw_desc_t v1 = base_desc(10u, 1u, true);
    v1.ep[0].binary = true;
    add(&v1);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_GENERAL), offered(10u, 0u));
    /* a notification type the device could not list events for (v2): all its inputs */
    zsrc_zw_desc_t v2 = base_desc(11u, 1u, true);
    add_notif(&v2, 0, 0x06u, 0u);
    add(&v2);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT), offered(11u, 0u));
    /* nothing from devices that do not report by themselves, are interviewing, or control */
    static const uint8_t NONE[] = {ZSRC_ZW_INPUTS_NOT_READY, ZSRC_ZW_INPUTS_NO_LIFELINE,
                                   ZSRC_ZW_INPUTS_PULL, ZSRC_ZW_INPUTS_NOT_USABLE};
    for (size_t i = 0; i < sizeof(NONE); i++) {
        d.ep[0].inputs = NONE[i];
        d.node = (uint16_t)(20u + i);
        add(&d);
        TEST_ASSERT_EQUAL_HEX32(0u, offered(d.node, 0u));
        TEST_ASSERT_FALSE(zsrc_zw_find(&Z, d.node)->ready);
    }
    /* end points: each its own inputs; one beyond the device's count offers nothing */
    zsrc_zw_desc_t mc = base_desc(12u, 1u, false);
    mc.endpoint_count = 2u;
    mc.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&mc, 1, 0x06u, EV(0x16) | EV(0x17));
    mc.ep[2].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&mc, 2, 0x05u, EV(1) | EV(2));
    mc.ep[3].inputs = ZSRC_ZW_INPUTS_OFFERED; /* not one of its two */
    add_notif(&mc, 3, 0x05u, EV(1) | EV(2));
    add(&mc);
    TEST_ASSERT_EQUAL_HEX32(0u, offered(12u, 0u));
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT), offered(12u, 1u));
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_WATER), offered(12u, 2u));
    TEST_ASSERT_EQUAL_HEX32(0u, offered(12u, 3u));
}

/* zones.md §2 candidates(): one per offered input of every ready device, with the device's name,
 * the input and its state in words, the class it suggests; the device added while the picker was
 * open first ("best"), then the inputs that changed; describe() for the zone page. */
static void test_candidates_and_words(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    zsrc_zw_desc_t mc = base_desc(12u, 1u, false);
    mc.endpoint_count = 2u;
    mc.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&mc, 1, 0x06u, EV(0x16) | EV(0x17));
    mc.ep[2].inputs = ZSRC_ZW_INPUTS_OFFERED;
    mc.ep[2].air_temperature = true;
    add(&mc);
    zsrc_zw_desc_t busy = door(13u, 1u);
    busy.ep[0].inputs = ZSRC_ZW_INPUTS_NOT_READY;
    add(&busy);
    up();
    zsrc_candidate_t c[8];
    size_t n = zone_hub_candidates(&H, ZSRC_ZWAVE, NULL, 0u, c, 8u);
    TEST_ASSERT_EQUAL_UINT(3u, n); /* the door; the multi-sensor's contact and temperature */
    TEST_ASSERT_TRUE(zsrc_address_equal(&c[0].addr, A(5u, 1u, 0u, 1u)));
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor", c[0].name);
    TEST_ASSERT_EQUAL_STRING("door contact", c[0].detail);
    TEST_ASSERT_EQUAL_STRING("", c[0].state); /* nothing heard yet */
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, c[0].device_class);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device 12", c[1].name);
    TEST_ASSERT_EQUAL_STRING("input 1 · door contact", c[1].detail);
    TEST_ASSERT_EQUAL_STRING("input 2 · temperature", c[2].detail);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_TEMPERATURE, c[2].device_class);
    /* the picker opens: the multi-sensor's contact opens, a new device joins */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZWAVE, true, NOW));
    notif_p(12u, 1u, 0x06u, 0x16u, -1);
    temperature(12u, 2u, 0x01u, 0u, 2150);
    zsrc_zw_desc_t fresh = pir(14u, 1u);
    fresh.has_identity = true;
    fresh.manufacturer = 0x1234u;
    fresh.product_type = 0x0001u;
    fresh.product_id = 0x00ABu;
    add(&fresh);
    n = zone_hub_candidates(&H, ZSRC_ZWAVE, NULL, 0u, c, 8u);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    TEST_ASSERT_EQUAL_STRING("Z-Wave 1234:0001:00AB", c[0].name); /* added: first, best */
    TEST_ASSERT_TRUE(c[0].best);
    TEST_ASSERT_TRUE(c[0].changed);
    TEST_ASSERT_EQUAL_STRING("input 1 · door contact", c[3].detail); /* then what changed */
    TEST_ASSERT_TRUE(c[3].changed);
    TEST_ASSERT_FALSE(c[3].best);
    TEST_ASSERT_EQUAL_STRING("Open", c[3].state);
    TEST_ASSERT_EQUAL_STRING("door contact", c[4].detail);
    TEST_ASSERT_EQUAL_STRING("21.5 °C", c[5].state); /* a value is not a change of state */
    TEST_ASSERT_FALSE(c[5].changed);
    /* the words for each state */
    notif(5u, 0x06u, 0x17u);
    char w[24];
    const zsrc_zw_device_t *dev = zsrc_zw_find(&Z, 5u);
    zsrc_zw_state_words(dev, 0u, ZSRC_ZW_SLOT_CONTACT, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("Closed", w);
    temperature(12u, 2u, 0x01u, 0u, -349);
    zsrc_zw_state_words(zsrc_zw_find(&Z, 12u), 2u, ZSRC_ZW_SLOT_TEMPERATURE, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("-3.5 °C", w);
    temperature(12u, 2u, 0x01u, 0u, -40);
    zsrc_zw_state_words(zsrc_zw_find(&Z, 12u), 2u, ZSRC_ZW_SLOT_TEMPERATURE, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("-0.4 °C", w);
    notif(14u, 0x07u, 0x05u);
    zsrc_zw_state_words(zsrc_zw_find(&Z, 14u), 0u, ZSRC_ZW_SLOT_GLASS_BREAK, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("Glass break", w);
    advance(31000u);
    zsrc_zw_state_words(zsrc_zw_find(&Z, 14u), 0u, ZSRC_ZW_SLOT_GLASS_BREAK, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("Clear", w);
    /* describe() for the zone page */
    char words[64];
    zsrc_zw_ops()->describe(&Z, A(5u, 1u, 0u, 1u), words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Zooz ZSE41 open/close sensor · door contact", words);
    zsrc_zw_ops()->describe(&Z, A(12u, 1u, 2u, 14u), words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Z-Wave device 12 · input 2 · temperature", words);
    zsrc_zw_ops()->describe(&Z, A(40u, 1u, 0u, 3u), words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Z-Wave device 40 · motion (not in the network)", words);
    /* the picker closes: nothing is new or changed the next time */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZWAVE, false, NOW));
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZWAVE, true, NOW));
    n = zone_hub_candidates(&H, ZSRC_ZWAVE, NULL, 0u, c, 8u);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_FALSE(c[i].changed || c[i].best);
    }
    /* no picker without the radio */
    zsrc_zw_link(&Z, false, NOW);
    TEST_ASSERT_FALSE(zone_hub_learn(&H, ZSRC_ZWAVE, true, NOW));
}

/* ---- reports -> zone events (zones.md §3; ZW-013) ------------------------------------------- */

/* A state event makes its input active (door 16 open, 17 closed); an idle with a parameter ends
 * that event's variable, one without ends every state of the type (the door has no idle); the
 * status 00 / FE says nothing. */
static void test_notification_states_and_idle(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    const zsrc_zw_desc_t p = pir(6u, 1u);
    add(&p);
    zsrc_zw_desc_t sec = base_desc(8u, 1u, false);
    add_notif(&sec, 0, 0x07u, EV(1) | EV(2) | EV(7) | EV(8) | EV(3));
    add(&sec);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    bind(3u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_INTRUSION));
    bind(4u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    clear();
    notif(5u, 0x06u, 0x16u);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    notif(5u, 0x06u, 0x17u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    notif(5u, 0x06u, 0x16u);
    notif(5u, 0x06u, 0x00u); /* Access Control idle: the door has none */
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_TRUE(info(1u).active);
    /* motion, then its idle with the motion event as parameter */
    notif(6u, 0x07u, 0x08u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    notif_p(6u, 0u, 0x07u, 0x00u, 0x08);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    /* an idle naming the other variable ends only that one */
    notif(8u, 0x07u, 0x02u);
    notif(8u, 0x07u, 0x07u);
    EXPECT(3u, ALARM_ZEV_FAULT, 1u);
    EXPECT(4u, ALARM_ZEV_FAULT, 1u);
    notif_p(8u, 0u, 0x07u, 0x00u, 0x07);
    EXPECT(4u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(3u).active);
    /* an idle without a parameter: every state of the type */
    notif(8u, 0x07u, 0x07u);
    notif(8u, 0x07u, 0x00u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(4u, ALARM_ZEV_RESTORE, 2u);
    /* reports switched off, nothing pending, a v1 alarm: nothing */
    clear();
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_NOTIFICATION;
    r.notif_type = 0x07u;
    r.notif_event = 0x07u;
    r.notif_status = 0x00u;
    report(8u, &r);
    r.notif_status = 0xFEu;
    report(8u, &r);
    r.notif_status = 0xFFu;
    r.notif_v1 = true;
    report(8u, &r);
    EXPECT(4u, ALARM_ZEV_FAULT, 0u);
    /* motion's idle leaves the cover's tamper (another variable of the type) as it is */
    notif(8u, 0x07u, 0x03u);
    notif(8u, 0x07u, 0x08u);
    notif_p(8u, 0u, 0x07u, 0x00u, 0x08);
    EXPECT(4u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(4u, ALARM_ZEV_TAMPER, 1u);
    EXPECT(4u, ALARM_ZEV_TAMPER_RESTORE, 0u);
}

/* The zone's last open or closed event: what it shows now. */
static alarm_zone_event_t last_state(uint16_t zone)
{
    alarm_zone_event_t ev = ALARM_ZEV_CHECKIN;
    for (unsigned i = 0; i < NR; i++) {
        if (R[i].zone == zone && (R[i].ev == ALARM_ZEV_FAULT || R[i].ev == ALARM_ZEV_RESTORE)) {
            ev = R[i].ev;
        }
    }
    return ev;
}

/* ZW-013 / B020: a bouncing magnet. A door sensor sends open, closed, open, closed in the same
 * instant; every report is a change told in order, and the zone ends where the last one says. A
 * report sent twice (a Supervision retry whose answer was lost, a repeated lifeline report) is no
 * new change: the zone stays where it is. The same for a Binary Sensor contact. */
static void test_a_bounce_ends_on_the_last_report(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    zsrc_zw_desc_t b = base_desc(10u, 1u, true);
    b.ep[0].binary = true;
    b.ep[0].binary_types = 1u << 0x0A;
    add(&b);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(10u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    const uint8_t door_ev[5] = {0x16u, 0x17u, 0x16u, 0x17u, 0x16u};
    for (unsigned i = 0; i < 5u; i++) {
        notif(5u, 0x06u, door_ev[i]);
    }
    EXPECT(1u, ALARM_ZEV_FAULT, 3u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 2u);
    TEST_ASSERT_EQUAL(ALARM_ZEV_FAULT, last_state(1u));
    notif(5u, 0x06u, 0x17u);
    notif(5u, 0x06u, 0x17u); /* sent again */
    TEST_ASSERT_EQUAL(ALARM_ZEV_RESTORE, last_state(1u));
    TEST_ASSERT_FALSE(info(1u).active);
    notif(5u, 0x06u, 0x16u);
    notif(5u, 0x06u, 0x16u);
    TEST_ASSERT_EQUAL(ALARM_ZEV_FAULT, last_state(1u));
    TEST_ASSERT_TRUE(info(1u).active);

    for (unsigned i = 0; i < 4u; i++) {
        binary(10u, 0u, 0x0A, (i % 2u) == 0u);
    }
    TEST_ASSERT_EQUAL(ALARM_ZEV_RESTORE, last_state(2u));
    binary(10u, 0u, 0x0A, true);
    TEST_ASSERT_EQUAL(ALARM_ZEV_FAULT, last_state(2u));
    TEST_ASSERT_TRUE(info(2u).active);
}

/* Tamper (Home Security 03, System 06, Binary Sensor 08) reaches every watched input of the
 * device, and so does low battery; its idle restores. */
static void test_tamper_and_battery_reach_every_input(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, true);
    d.endpoint_count = 2u;
    add_notif(&d, 0, 0x07u, EV(3));
    add_notif(&d, 0, 0x09u, EV(6));
    d.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&d, 1, 0x06u, EV(0x16) | EV(0x17));
    d.ep[2].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&d, 2, 0x05u, EV(1) | EV(2));
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 1u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(9u, 1u, 2u, ZSRC_ZW_SLOT_WATER));
    clear();
    notif(9u, 0x07u, 0x03u);
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
    notif(9u, 0x09u, 0x06u); /* a second cause: still one tamper */
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    notif_p(9u, 0u, 0x07u, 0x00u, 0x03);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u); /* the other cause holds it */
    notif_p(9u, 0u, 0x09u, 0x00u, 0x06);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    binary(9u, 0u, 0x08, true);
    EXPECT(2u, ALARM_ZEV_TAMPER, 2u);
    binary(9u, 0u, 0x08, false);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 2u);
    /* Home Security idle without a parameter restores its tamper too */
    notif(9u, 0x07u, 0x03u);
    notif(9u, 0x07u, 0x00u);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 3u);
    /* low battery, the same way */
    clear();
    battery(9u, 10u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY, 1u);
    battery(9u, 90u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    /* a zone bound later hears what is already true */
    notif(9u, 0x07u, 0x03u);
    battery(9u, 5u, 0u);
    zone_hub_unbind(&H, 2u, NOW);
    clear();
    bind(2u, addr(9u, 1u, 2u, ZSRC_ZW_SLOT_WATER));
    EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY, 1u);
}

/* zones.md §3: Home Security 09 (moved) is a tamper on a device with a cover, restored after 30 s;
 * without a cover it is the impact input, momentary. */
static void test_product_moved(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u); /* has a cover (07 03) */
    add(&d);
    const zsrc_zw_desc_t p = pir(6u, 1u);  /* none */
    add(&p);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_IMPACT));
    clear();
    notif(5u, 0x07u, 0x09u);
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    advance(29999u);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    advance(1u);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    notif(6u, 0x07u, 0x09u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    advance(30000u);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    /* a cover of System (09 06) is a cover too */
    zsrc_zw_desc_t sys = base_desc(8u, 1u, true);
    add_notif(&sys, 0, 0x06u, EV(0x16) | EV(0x17));
    add_notif(&sys, 0, 0x09u, EV(0x06));
    add_notif(&sys, 0, 0x07u, EV(0x09));
    add(&sys);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT), offered(8u, 0u));
    bind(3u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(8u, 0x07u, 0x09u);
    EXPECT(3u, ALARM_ZEV_TAMPER, 1u);
}

static void scene(uint16_t node, uint8_t seq, uint8_t attr, uint8_t number)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof r);
    r.kind = ZSRC_ZW_RPT_CENTRAL_SCENE;
    r.scene_seq = seq;
    r.scene_attr = attr;
    r.scene_number = number;
    report(node, &r);
}

/* ZW-024 (zones.md §4 slot 13, command-classes.md §4.21): a key fob's scene is its emergency
 * button only when its quirk says so - offered at the device itself; pressed, held or pressed
 * again count, its release doesn't; a repeated sequence number is the same press sent again */
static void test_central_scene_by_quirk(void)
{
    static const zsrc_zw_quirk_t ROWS[] = {{.manufacturer = 0x0102u, .product_type = 0x0304u,
                                            .product_id = 0x0506u, .name = "Test fob",
                                            .panic_scenes = 1u << 2}};
    zsrc_zw_set_quirks(&Z, ROWS, 1u);
    zsrc_zw_desc_t fob = base_desc(8u, 1u, true);
    fob.has_identity = true;
    fob.manufacturer = 0x0102u;
    fob.product_type = 0x0304u;
    fob.product_id = 0x0506u;
    add(&fob);
    zsrc_zw_desc_t other = base_desc(9u, 1u, true); /* no quirk: scenes feed nothing */
    add(&other);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_EMERGENCY), offered(8u, 0u));
    TEST_ASSERT_EQUAL_HEX32(0u, offered(9u, 0u));
    up();
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_PANIC_BUTTON);
    m.hold_s = 5u;
    bind_m(2u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_EMERGENCY), &m);
    clear();
    scene(8u, 1u, 0u, 1u); /* another scene of it: only a sign of life */
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    scene(8u, 2u, 0u, 2u); /* its panic scene, pressed once */
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    const unsigned restores = count(2u, ALARM_ZEV_RESTORE);
    advance(5000u);
    EXPECT(2u, ALARM_ZEV_RESTORE, restores + 1u); /* its hold over */
    scene(8u, 2u, 0u, 2u); /* the same press sent again */
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    scene(8u, 3u, 1u, 2u); /* released */
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    scene(8u, 4u, 2u, 2u); /* held */
    EXPECT(2u, ALARM_ZEV_FAULT, 2u);
    advance(5000u);
    scene(8u, 5u, 4u, 2u); /* pressed three times */
    EXPECT(2u, ALARM_ZEV_FAULT, 3u);
    zsrc_zw_set_quirks(&Z, NULL, 0u);
}

/* zones.md §3: momentary inputs (glass break, impact, emergency) are on for the binding's hold,
 * 30 s by default, restarted by a repeat. */
static void test_momentary_holds(void)
{
    const zsrc_zw_desc_t p = pir(6u, 1u);
    add(&p);
    zsrc_zw_desc_t e = base_desc(7u, 1u, true);
    add_notif(&e, 0, 0x0Au, EV(1) | EV(2) | EV(3) | EV(4));
    add(&e);
    up();
    bind(1u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_GLASS_BREAK));
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_PANIC_BUTTON);
    m.hold_s = 5u;
    bind_m(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_EMERGENCY), &m);
    clear();
    notif(6u, 0x07u, 0x06u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT64(NOW + 30000u, zsrc_zw_ops()->next_tick(&Z, NOW)); /* its end */
    advance(20000u);
    notif(6u, 0x07u, 0x05u); /* again: 30 s from now */
    advance(29999u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    advance(1u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    for (uint8_t ev = 1u; ev <= 4u; ev++) {
        notif(7u, 0x0Au, ev);
        EXPECT(2u, ALARM_ZEV_FAULT, ev);
        advance(5000u); /* the binding's hold */
        EXPECT(2u, ALARM_ZEV_RESTORE, ev);
    }
}

/* command-classes.md §4.5: CO's periodic inspection (07) and Heat's dust and periodic inspection
 * (0A, 0B) are maintenance too - TROUBLE on the life-safety inputs, ended by that type's idle. */
static void test_life_safety_maintenance_events(void)
{
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(1u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    clear();
    notif(7u, 0x02u, 0x07u);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 1u);
    notif(7u, 0x02u, 0x00u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    notif(7u, 0x04u, 0x0Au);
    EXPECT(3u, ALARM_ZEV_TROUBLE, 2u);
    notif(7u, 0x04u, 0x00u);
    EXPECT(3u, ALARM_ZEV_TROUBLE_RESTORE, 2u);
    notif(7u, 0x04u, 0x0Bu);
    EXPECT(3u, ALARM_ZEV_TROUBLE, 3u);
    notif(7u, 0x04u, 0x00u);
    EXPECT(3u, ALARM_ZEV_TROUBLE_RESTORE, 3u);
}

/* ZW-013: Heat 03/04 (rapid temperature rise, a rate-of-rise detector's main trigger) are events
 * (Z-Wave JS): the heat input is on for the binding's hold, 30 s by default, restarted by a
 * repeat, and a device whose Heat mask has only 03 or 04 offers the heat input. */
static void test_heat_rapid_rise(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, false);
    add_notif(&d, 0, 0x04u, EV(3) | EV(4));
    add(&d);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_HEAT), offered(9u, 0u));
    zsrc_zw_desc_t only4 = base_desc(10u, 1u, false);
    add_notif(&only4, 0, 0x04u, EV(4));
    add(&only4);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_HEAT), offered(10u, 0u));
    up();
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_HEAT);
    m.hold_s = 5u;
    bind_m(2u, addr(10u, 1u, 0u, ZSRC_ZW_SLOT_HEAT), &m);
    clear();
    notif(9u, 0x04u, 0x03u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT64(NOW + 30000u, zsrc_zw_ops()->next_tick(&Z, NOW)); /* its end */
    char words[24];
    zsrc_zw_state_words(zsrc_zw_find(&Z, 9u), 0u, ZSRC_ZW_SLOT_HEAT, NOW, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Heat", words);
    advance(10000u);
    notif(9u, 0x04u, 0x04u); /* again: 30 s from now */
    advance(29999u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    advance(1u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u); /* its hold over, though it never sent a heat state */
    zsrc_zw_state_words(zsrc_zw_find(&Z, 9u), 0u, ZSRC_ZW_SLOT_HEAT, NOW, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Clear", words); /* no state known, but the hub heard "off" */
    notif_p(9u, 0u, 0x04u, 0x00u, 0x03); /* an idle of the event: nothing to end */
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    notif(10u, 0x04u, 0x04u); /* the binding's own hold */
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    advance(5000u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    /* a heat state that starts during the pulse outlasts it */
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    clear();
    notif(7u, 0x04u, 0x03u); /* not in its mask, but its heat input is offered (01/02) */
    EXPECT(3u, ALARM_ZEV_FAULT, 1u);
    notif(7u, 0x04u, 0x02u); /* overheat */
    advance(30000u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 0u);
    notif(7u, 0x04u, 0x00u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 1u);
}

/* ZW-013: CO 03's parameter: 02 (test failed) is TROUBLE on the life-safety inputs, not "tested";
 * 01 (test OK) ends it and is "tested". CO 03 has no idle, so no idle ends the trouble. */
static void test_co_test_failed(void)
{
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    clear();
    advance(MIN_MS);
    const uint64_t before = zsrc_zw_find(&Z, 7u)->tested_at;
    notif_p(7u, 0u, 0x02u, 0x03u, 0x02);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_EQUAL_UINT64(before, zsrc_zw_find(&Z, 7u)->tested_at); /* failed: not "tested" */
    notif_p(7u, 0u, 0x02u, 0x00u, 0x03); /* its idle (it has none) */
    notif(7u, 0x02u, 0x00u);             /* the type's idle */
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    advance(1000u);
    notif_p(7u, 0u, 0x02u, 0x03u, 0x01); /* test OK */
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 7u)->tested_at);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 1u);
    advance(1000u);
    notif(7u, 0x02u, 0x03u); /* no parameter: a test, no trouble */
    EXPECT(2u, ALARM_ZEV_TROUBLE, 1u);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 7u)->tested_at);
    /* a CO 03 without a parameter after a failed test is a test: it ends the trouble */
    advance(1000u);
    notif_p(7u, 0u, 0x02u, 0x03u, 0x02);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 2u);
    advance(1000u);
    notif(7u, 0x02u, 0x03u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 2u);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 7u)->tested_at);
    /* a parameter that is neither 01 nor 02 (Z-Wave JS knows no other): it neither ends the
     * trouble nor is "tested" */
    advance(1000u);
    notif_p(7u, 0u, 0x02u, 0x03u, 0x02);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 3u);
    const uint64_t failed_at = zsrc_zw_find(&Z, 7u)->tested_at;
    advance(1000u);
    notif_p(7u, 0u, 0x02u, 0x03u, 0x05);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 2u);
    TEST_ASSERT_EQUAL_UINT64(failed_at, zsrc_zw_find(&Z, 7u)->tested_at);
    notif_p(7u, 0u, 0x02u, 0x03u, 0x01);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 3u);
}

/* ZW-013: each maintenance variable (Z-Wave JS) has its own trouble: one ending leaves another. */
static void test_maintenance_variables_apart(void)
{
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(1u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    /* Smoke: maintenance {04,05}, periodic inspection {07}, dust {08} */
    notif(7u, 0x01u, 0x05u); /* end of life */
    notif(7u, 0x01u, 0x08u); /* dust */
    notif_p(7u, 0u, 0x01u, 0x00u, 0x08);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u); /* end of life is still active */
    notif(7u, 0x01u, 0x07u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x04); /* ZW22: 04 and 05 are one variable, holding 05 */
    notif_p(7u, 0u, 0x01u, 0x00u, 0x07);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u); /* the idle of 04 was stale: 05 is still active */
    notif(7u, 0x01u, 0x07u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x05);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u); /* inspection due is still active */
    notif_p(7u, 0u, 0x01u, 0x00u, 0x07);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    /* CO: maintenance {04,05}, periodic inspection {07}; and its failed test */
    clear();
    notif(7u, 0x02u, 0x04u);
    notif(7u, 0x02u, 0x07u);
    notif_p(7u, 0u, 0x02u, 0x03u, 0x02);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x04);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x07);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u); /* the failed test is still active */
    notif_p(7u, 0u, 0x02u, 0x03u, 0x01);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    clear(); /* without a failed test: inspection {07} outlasts maintenance {04,05} */
    notif(7u, 0x02u, 0x04u);
    notif(7u, 0x02u, 0x07u);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x04);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x07);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    clear(); /* {04,05} are one variable: 05 replaces 04, and the idle of 05 ends it */
    notif(7u, 0x02u, 0x04u);
    notif(7u, 0x02u, 0x05u);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x05);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    /* Heat: maintenance {08}, dust {0A}, periodic inspection {0B} */
    clear();
    notif(7u, 0x04u, 0x08u);
    notif(7u, 0x04u, 0x0Au);
    notif(7u, 0x04u, 0x0Bu);
    notif_p(7u, 0u, 0x04u, 0x00u, 0x0A);
    notif_p(7u, 0u, 0x04u, 0x00u, 0x0B);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(7u, 0u, 0x04u, 0x00u, 0x08);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    clear(); /* dust {0A} and inspection {0B} apart */
    notif(7u, 0x04u, 0x0Au);
    notif(7u, 0x04u, 0x0Bu);
    notif_p(7u, 0u, 0x04u, 0x00u, 0x0A);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(7u, 0u, 0x04u, 0x00u, 0x0B);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    /* System: hardware {01,03}, software {02,04} */
    clear();
    notif(7u, 0x09u, 0x01u);
    notif(7u, 0x09u, 0x02u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x01);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u); /* the software failure is still active */
    notif_p(7u, 0u, 0x09u, 0x00u, 0x04); /* ZW22: the variable holds 02, not 04 */
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x02);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
}

/* Life-safety devices: their inputs (smoke 01/02, CO 01/02, overheat 01/02, underheat 05/06 as
 * freeze), maintenance and system failures as TROUBLE on the smoke, CO, heat and gas inputs
 * (never on another), their idle; tests are check-ins, not faults; alarm silenced is nothing;
 * a heartbeat is a check-in; mains lost and back. */
static void test_life_safety_rows(void)
{
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(1u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    bind(4u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_FREEZE));
    bind(5u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_MAINS));
    clear();
    notif(7u, 0x01u, 0x02u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x02);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    notif(7u, 0x02u, 0x01u);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    notif(7u, 0x04u, 0x02u);
    EXPECT(3u, ALARM_ZEV_FAULT, 1u);
    EXPECT(4u, ALARM_ZEV_RESTORE, 1u); /* the same variable: so not underheat */
    notif(7u, 0x04u, 0x06u); /* underheat: overheat ends, freeze starts */
    EXPECT(3u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(4u, ALARM_ZEV_FAULT, 1u);
    notif_p(7u, 0u, 0x04u, 0x00u, 0x06);
    EXPECT(4u, ALARM_ZEV_RESTORE, 2u);
    /* maintenance: trouble on the life-safety inputs only */
    clear();
    for (uint8_t ev = 0x04u; ev <= 0x08u; ev++) {
        if (ev == 0x06u) {
            continue;
        }
        notif(7u, 0x01u, ev);
        notif_p(7u, 0u, 0x01u, 0x00u, ev);
    }
    EXPECT(1u, ALARM_ZEV_TROUBLE, 4u);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 4u);
    EXPECT(3u, ALARM_ZEV_TROUBLE, 4u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 4u);
    EXPECT(4u, ALARM_ZEV_TROUBLE, 0u); /* freeze is not a life-safety input */
    EXPECT(5u, ALARM_ZEV_TROUBLE, 0u);
    clear();
    notif(7u, 0x02u, 0x05u);
    notif(7u, 0x04u, 0x08u);
    notif(7u, 0x09u, 0x01u);
    notif(7u, 0x09u, 0x03u);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 1u); /* four causes, one trouble */
    /* zones bound meanwhile: a life-safety input hears it, another does not */
    zone_hub_unbind(&H, 3u, NOW);
    zone_hub_unbind(&H, 4u, NOW);
    const unsigned heat = count(3u, ALARM_ZEV_TROUBLE);
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    bind(4u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_FREEZE));
    EXPECT(3u, ALARM_ZEV_TROUBLE, heat + 1u);
    EXPECT(4u, ALARM_ZEV_TROUBLE, 0u);
    notif(7u, 0x02u, 0x00u);           /* CO idle: its maintenance ends */
    notif(7u, 0x04u, 0x00u);
    /* ZW-013: System 01 and 03 are one variable (hardware failure), 02 and 04 another (software),
     * as in the spec and Z-Wave JS. This test used to raise 01 and 04 and idle 02 then 03, which
     * passed with the wrong grouping (01/02 hardware, 03/04 software) too. The steps after the
     * idle of 03 prove the grouping: 02 then 04 (one variable: 04 replaces 02), then the idle of
     * 01 must not end it and the idle of 04 must - it would leave 02 if they were apart. An idle
     * names the event its variable holds (ZW22): the idle of 02 is stale once 04 holds it. */
    notif_p(7u, 0u, 0x09u, 0x00u, 0x02); /* software: not raised */
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x03);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    notif(7u, 0x09u, 0x02u); /* a software failure alone */
    notif(7u, 0x09u, 0x04u);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 2u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x01); /* hardware's idle: not software's */
    notif_p(7u, 0u, 0x09u, 0x00u, 0x02); /* stale */
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x04);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 2u);
    /* tests: a check-in and "tested", never a fault; silenced: nothing */
    clear();
    advance(MIN_MS);
    notif(7u, 0x01u, 0x03u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 7u)->tested_at);
    advance(1000u);
    notif(7u, 0x02u, 0x03u);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 7u)->tested_at); /* CO's test */
    advance(1000u);
    notif(7u, 0x04u, 0x07u);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 7u)->tested_at); /* heat's */
    notif(7u, 0x01u, 0x06u);
    notif(7u, 0x02u, 0x06u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u); /* at most once a minute */
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    advance(MIN_MS);
    notif(7u, 0x09u, 0x05u); /* heartbeat */
    EXPECT(1u, ALARM_ZEV_CHECKIN, 2u);
    /* mains lost and back (it has no idle) */
    clear();
    notif(7u, 0x08u, 0x02u);
    EXPECT_LAST(5u, ALARM_ZEV_FAULT);
    notif(7u, 0x08u, 0x00u);
    EXPECT(5u, ALARM_ZEV_RESTORE, 0u);
    notif(7u, 0x08u, 0x03u);
    EXPECT_LAST(5u, ALARM_ZEV_RESTORE);
}

/* Water, freeze (Weather 03), gas (two variables, one input), emergency; Power Management's
 * battery events are LOW_BATTERY until their idle. */
static void test_other_rows(void)
{
    zsrc_zw_desc_t d = base_desc(8u, 1u, true);
    add_notif(&d, 0, 0x05u, EV(1) | EV(2));
    add_notif(&d, 0, 0x10u, EV(3));
    add_notif(&d, 0, 0x12u, EV(1) | EV(2) | EV(3) | EV(4) | EV(5) | EV(6));
    add_notif(&d, 0, 0x08u, EV(0x0A) | EV(0x0B) | EV(0x0E) | EV(0x0F) | EV(0x10));
    add_notif(&d, 0, 0x07u, EV(0x04) | EV(0x0B));
    add(&d);
    up();
    bind(1u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_WATER));
    bind(2u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_FREEZE));
    bind(3u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_GAS));
    clear();
    notif(8u, 0x05u, 0x02u);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    notif(8u, 0x05u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    notif(8u, 0x10u, 0x03u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    notif_p(8u, 0u, 0x10u, 0x00u, 0x03);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    notif(8u, 0x12u, 0x01u); /* combustible */
    notif(8u, 0x12u, 0x03u); /* and toxic */
    EXPECT(3u, ALARM_ZEV_FAULT, 1u);
    notif_p(8u, 0u, 0x12u, 0x00u, 0x01);
    EXPECT(3u, ALARM_ZEV_RESTORE, 0u); /* toxic still */
    notif_p(8u, 0u, 0x12u, 0x00u, 0x04); /* ZW22: toxic holds 03, not 04 */
    EXPECT(3u, ALARM_ZEV_RESTORE, 0u);
    notif_p(8u, 0u, 0x12u, 0x00u, 0x03);
    EXPECT(3u, ALARM_ZEV_RESTORE, 1u);
    notif(8u, 0x12u, 0x03u); /* toxic {03,04} are one variable: 04 replaces 03 ... */
    notif(8u, 0x12u, 0x04u);
    notif_p(8u, 0u, 0x12u, 0x00u, 0x04); /* ... and the idle of 04 ends it */
    EXPECT(3u, ALARM_ZEV_FAULT, 2u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 2u);
    notif(8u, 0x12u, 0x06u); /* its maintenance: a gas input is life safety */
    EXPECT(3u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 0u);
    /* Power Management's battery variables */
    static const uint8_t BATTERY_EVENTS[] = {0x0Au, 0x0Bu, 0x0Eu, 0x0Fu, 0x10u};
    for (size_t i = 0; i < sizeof(BATTERY_EVENTS); i++) {
        clear();
        notif(8u, 0x08u, BATTERY_EVENTS[i]);
        EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
        EXPECT(3u, ALARM_ZEV_LOW_BATTERY, 1u);
        notif_p(8u, 0u, 0x08u, 0x00u, BATTERY_EVENTS[i]);
        EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    }
    /* invalid code, magnetic interference, an event no row names: nothing for a zone */
    clear();
    notif(8u, 0x07u, 0x04u);
    notif(8u, 0x07u, 0x0Bu);
    notif(8u, 0x05u, 0x0Au);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    EXPECT(1u, ALARM_ZEV_TAMPER, 0u);
}

/* Binary Sensor Report: FF active, 00 inactive, per type; a v1 report without a type is its one
 * type, else the general input. */
static void test_binary_sensor_rows(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, true);
    d.ep[0].binary = true;
    d.ep[0].binary_types = (1u << 0x0C) | (1u << 0x0D) | (1u << 0x06);
    add(&d);
    zsrc_zw_desc_t one = base_desc(10u, 1u, true);
    one.ep[0].binary = true;
    one.ep[0].binary_types = 1u << 0x0A;
    add(&one);
    zsrc_zw_desc_t v1 = base_desc(11u, 1u, true);
    v1.ep[0].binary = true;
    add(&v1);
    up();
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    bind(2u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_GLASS_BREAK));
    bind(3u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_WATER));
    bind(4u, addr(10u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(5u, addr(11u, 1u, 0u, ZSRC_ZW_SLOT_GENERAL));
    clear();
    binary(9u, 0u, 0x0C, true);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    binary(9u, 0u, 0x0C, false);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    binary(9u, 0u, 0x0D, true); /* glass break as a state (zones.md §2) */
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    advance(40000u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    binary(9u, 0u, 0x0D, false);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    binary(9u, 0u, 0x06, true);
    EXPECT_LAST(3u, ALARM_ZEV_FAULT);
    binary(10u, 0u, -1, true); /* v1: its one type, door/window */
    EXPECT_LAST(4u, ALARM_ZEV_FAULT);
    binary(11u, 0u, -1, true); /* v1, no type: the general input */
    EXPECT_LAST(5u, ALARM_ZEV_FAULT);
    binary(11u, 0u, 0x04, true); /* CO2: not an input */
    EXPECT(5u, ALARM_ZEV_FAULT, 1u);
    EXPECT(5u, ALARM_ZEV_RESTORE, 0u);
}

/* zones.md §3: Battery FF, below 20 % or v2 "replace soon/now" is low; 30 % and up without the
 * flags restores; between, nothing changes; an invalid level is dropped. */
static void test_battery_thresholds(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    battery(5u, 20u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 0u);
    battery(5u, 19u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    battery(5u, 29u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u); /* the hysteresis */
    battery(5u, 30u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    battery(5u, 0xFFu, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 2u);
    TEST_ASSERT_EQUAL_UINT8(0u, zsrc_zw_find(&Z, 5u)->battery_pct);
    battery(5u, 80u, 0u);
    battery(5u, 80u, 1u); /* replace soon */
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 3u);
    battery(5u, 80u, 0u);
    battery(5u, 0x65u, 0u); /* invalid: dropped */
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 3u);
    TEST_ASSERT_EQUAL_UINT8(80u, zsrc_zw_find(&Z, 5u)->battery_pct);
}

/* Battery Report v2 "disconnected" (flags2 bit 0, zw_cc_battery_report_t.disconnected) is treated
 * like "replace now": LOW_BATTERY while set, restored only by a later report without it at 30 %
 * or up (Z-Wave JS cc/BatteryCC.test.ts's "disconnected" case; zones.md §3, ZW-013). */
static void test_battery_disconnected(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_BATTERY;
    r.battery_level = 90u; /* otherwise healthy */
    r.battery_disconnected = true;
    report(5u, &r);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    r.battery_disconnected = false;
    report(5u, &r);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
}

/* Binary Sensor Report type FF ("Any"): mapped to the endpoint's one supported type when it has
 * exactly one; with several (or none of ours) it is ambiguous - dropped, counted (Z-Wave JS
 * compat/binarySensorReportAnyUseFirstSupported.test.ts; zones.md §2, ZW-013). */
static void test_binary_sensor_any_type(void)
{
    zsrc_zw_desc_t one = base_desc(9u, 1u, true);
    one.ep[0].binary = true;
    one.ep[0].binary_types = 1u << 0x0Cu; /* motion: its only supported type */
    add(&one);
    zsrc_zw_desc_t many = base_desc(10u, 1u, true);
    many.ep[0].binary = true;
    many.ep[0].binary_types = (1u << 0x0Au) | (1u << 0x0Cu); /* door and motion: ambiguous */
    add(&many);
    up();
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    bind(2u, addr(10u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    binary(9u, 0u, 0xFFu, true); /* "Any": this endpoint's one type is motion */
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    binary(9u, 0u, 0xFFu, false);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    TEST_ASSERT_EQUAL_UINT32(0u, zsrc_zw_find(&Z, 9u)->binary_any_ambiguous);
    binary(10u, 0u, 0xFFu, true); /* several supported types: ambiguous, dropped and counted */
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, zsrc_zw_find(&Z, 10u)->binary_any_ambiguous);
    binary(10u, 0u, 0xFFu, true);
    TEST_ASSERT_EQUAL_UINT32(2u, zsrc_zw_find(&Z, 10u)->binary_any_ambiguous);
}

/* Access Control 16's tilt parameter (00 regular, 01 tilted): a tilted-open window is open for a
 * zone, the same as fully open - the parameter never changes the result (resolving zones.md §2's
 * old "(+ tilt parameter)" note; Z-Wave JS notificationEnums.test.ts's door/tilt grouping). */
static void test_access_control_tilt_parameter(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    notif_p(5u, 0u, 0x06u, 0x16u, 0x01); /* tilted open */
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    TEST_ASSERT_TRUE(info(1u).active);
    notif_p(5u, 0u, 0x06u, 0x17u, -1); /* closed */
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    notif_p(5u, 0u, 0x06u, 0x16u, 0x00); /* regular open: the same result */
    TEST_ASSERT_TRUE(info(1u).active);
}

/* Notification status FE ("no pending notification", answering a Get) never idles an active
 * state: any status but FF (on) does nothing at all, so an idle-shaped event under FE is not
 * honoured either - a deliberately conservative choice, named explicitly so it is not "fixed"
 * into auto-idling later (Z-Wave JS cc-specific/notificationIdleOnUnknownEvent.test.ts; zones.md
 * §3, ZW-013). */
static void test_notification_status_fe_never_idles(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, false);
    add_notif(&d, 0, 0x01u, EV(1) | EV(2));
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    notif(9u, 0x01u, 0x01u); /* smoke detected: active */
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_NOTIFICATION;
    r.notif_status = 0xFEu; /* an idle-shaped event, but the status is not FF */
    r.notif_type = 0x01u;
    r.notif_event = 0x00u;
    report(9u, &r);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u); /* still active */
}

/* Multi Channel: a report addressed to an endpoint for a slot that endpoint does not offer is
 * dropped - even when a different endpoint of the same device offers that slot, it is never
 * credited there (Z-Wave JS cc-specific/discardUnsupportedReports.test.ts). A root (unencapsulated)
 * report reaches endpoint 0 alone; with nothing offered there it is simply the device's check-in,
 * never moved to another endpoint (Z-Wave JS Node.handleCommand.test.ts's "identical endpoints"
 * root-to-endpoint-1 compat flag is out of scope by design, docs/features/36 README). ZW-013. */
static void test_multi_channel_endpoint_isolation(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, false);
    d.endpoint_count = 2u;
    d.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&d, 1, 0x06u, EV(0x16) | EV(0x17));
    d.ep[2].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&d, 2, 0x05u, EV(1) | EV(2));
    add(&d);
    TEST_ASSERT_EQUAL_HEX32(0u, offered(9u, 0u));
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT), offered(9u, 1u));
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_WATER), offered(9u, 2u));
    up();
    bind(1u, addr(9u, 1u, 1u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(9u, 1u, 2u, ZSRC_ZW_SLOT_WATER));
    clear();
    /* a water event addressed to endpoint 1, which does not offer water (only endpoint 2 does) */
    notif_p(9u, 1u, 0x05u, 0x01u, -1);
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    /* endpoint 2's own water event: its own zone, still */
    notif_p(9u, 2u, 0x05u, 0x01u, -1);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    /* a root report: endpoint 0 offers nothing, so it is only a check-in for both zones */
    clear();
    advance(MIN_MS);
    notif(9u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u);
    EXPECT(2u, ALARM_ZEV_CHECKIN, 1u);
}

/* Multilevel air temperature feeds the temperature input (hundredths of °C, Fahrenheit
 * converted); the zone's BELOW mapping decides; other types are not inputs. */
static void test_temperature(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, true);
    d.ep[0].air_temperature = true;
    add(&d);
    up();
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_TEMPERATURE); /* below 5 °C */
    bind_m(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_TEMPERATURE), &m);
    clear();
    temperature(9u, 0u, 0x01u, 0u, 2150);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    TEST_ASSERT_EQUAL_INT32(2150, info(1u).value_x100);
    temperature(9u, 0u, 0x01u, 1u, 3920); /* 39.2 °F = 4 °C */
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    TEST_ASSERT_EQUAL_INT32(400, info(1u).value_x100);
    temperature(9u, 0u, 0x05u, 0u, 9000); /* humidity */
    temperature(9u, 0u, 0x01u, 2u, 9000); /* a scale that is not a temperature's */
    TEST_ASSERT_EQUAL_INT32(400, info(1u).value_x100);
}

/* Alarm Sensor (legacy): its type's input, state 00 inactive, anything else active. */
/* Faults and restores told (a dropped report is still a check-in). */
static unsigned changes(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NR; i++) {
        n += R[i].ev == ALARM_ZEV_FAULT || R[i].ev == ALARM_ZEV_RESTORE;
    }
    return n;
}

static void test_alarm_sensor_rows(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, false);
    d.ep[0].alarm_types = (1u << 0x01) | (1u << 0x02) | (1u << 0x04);
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    bind(3u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    clear();
    alarm_sensor(9u, 0x01u, 0xFFu);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    alarm_sensor(9u, 0x02u, 0x40u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    alarm_sensor(9u, 0x04u, 0x01u);
    EXPECT_LAST(3u, ALARM_ZEV_FAULT);
    alarm_sensor(9u, 0x01u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    const unsigned before = changes();
    alarm_sensor(9u, 0x05u, 0xFFu); /* water: a type it did not list feeds nothing offered */
    alarm_sensor(9u, 0x03u, 0xFFu); /* 03 (burglar): not one of zones.md §2's */
    TEST_ASSERT_EQUAL_UINT(before, changes());
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
}

/* ZW-043, command-classes.md §4.22: Alarm Sensor type 05 (water) feeds the water input and 00
 * (general purpose) the general input, as 01, 02 and 04 feed theirs. */
static void test_alarm_sensor_water_and_general(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, false);
    d.ep[0].alarm_types = (1u << 0x00) | (1u << 0x05);
    add(&d);
    up();
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_WATER) | S(ZSRC_ZW_SLOT_GENERAL), offered(9u, 0u));
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_WATER));
    bind(2u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_GENERAL));
    clear();
    alarm_sensor(9u, 0x05u, 0xFFu);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    EXPECT(2u, ALARM_ZEV_FAULT, 0u);
    alarm_sensor(9u, 0x00u, 0x01u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    alarm_sensor(9u, 0x05u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    alarm_sensor(9u, 0x00u, 0x00u);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
}

/* zones.md §5: a device that offers anything from Notification, Binary or Alarm Sensor ignores
 * Basic; one with nothing better and Basic offers the general input fed by it - also when it
 * only shows it by sending Basic; a quirk can name the slot. */
static void test_basic(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    zsrc_zw_desc_t b = base_desc(9u, 1u, true);
    b.ep[0].basic = true;
    add(&b);
    zsrc_zw_desc_t quiet = base_desc(10u, 1u, true); /* lists nothing: sends Basic anyway */
    add(&quiet);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_GENERAL), offered(9u, 0u));
    TEST_ASSERT_EQUAL_HEX32(0u, offered(10u, 0u));
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_GENERAL));
    clear();
    basic(5u, 0u, 0xFFu); /* the door: one event is not counted twice */
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    basic(9u, 0u, 0xFFu);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    basic(9u, 0u, 0x00u);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    zsrc_zw_desc_t bin = base_desc(12u, 1u, true); /* Binary Sensor v1 feeds its general input */
    bin.ep[0].binary = true;
    bin.ep[0].basic = true;
    add(&bin);
    bind(5u, addr(12u, 1u, 0u, ZSRC_ZW_SLOT_GENERAL));
    binary(12u, 0u, -1, false);
    basic(12u, 0u, 0xFFu); /* its Basic Set of the same event: not counted again */
    EXPECT(5u, ALARM_ZEV_FAULT, 0u);
    basic(10u, 0u, 0x63u);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_GENERAL), offered(10u, 0u));
    bind(3u, addr(10u, 1u, 0u, ZSRC_ZW_SLOT_GENERAL));
    EXPECT_LAST(3u, ALARM_ZEV_FAULT); /* its state is known already */
    /* a quirk: a water sensor that speaks only Basic */
    static const zsrc_zw_quirk_t ROWS[] = {{.manufacturer = 0x0101u, .product_type = 0x0202u,
                                            .product_id = 0x0303u, .name = "Leak",
                                            .basic_slot = ZSRC_ZW_SLOT_WATER},
                                           {.manufacturer = 0x0101u, .product_type = 0x0202u,
                                            .product_id = 0x0404u, .name = "Door and leak",
                                            .basic_slot = ZSRC_ZW_SLOT_WATER}};
    zsrc_zw_set_quirks(&Z, ROWS, 2u);
    zsrc_zw_desc_t leak = base_desc(11u, 1u, true);
    leak.has_identity = true;
    leak.manufacturer = 0x0101u;
    leak.product_type = 0x0202u;
    leak.product_id = 0x0303u;
    leak.ep[0].basic = true;
    add(&leak);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_WATER), offered(11u, 0u));
    bind(4u, addr(11u, 1u, 0u, ZSRC_ZW_SLOT_WATER));
    basic(11u, 0u, 0xFFu);
    EXPECT_LAST(4u, ALARM_ZEV_FAULT);
    /* the quirk's slot is fed by Basic beside the device's own inputs */
    zsrc_zw_desc_t both = door(13u, 1u);
    both.manufacturer = 0x0101u;
    both.product_type = 0x0202u;
    both.product_id = 0x0404u;
    both.ep[0].basic = true;
    add(&both);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT) | S(ZSRC_ZW_SLOT_WATER), offered(13u, 0u));
    bind(6u, addr(13u, 1u, 0u, ZSRC_ZW_SLOT_WATER));
    basic(13u, 0u, 0xFFu);
    EXPECT_LAST(6u, ALARM_ZEV_FAULT);
}

/* A report for an endpoint the device does not have, or an input it did not offer, feeds no
 * zone; the device is still alive. */
static void test_unoffered_is_ignored(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    TEST_ASSERT_FALSE(zsrc_zw_ops()->valid(&Z, A(5u, 1u, 0u, 3u)));
    TEST_ASSERT_FALSE(zsrc_zw_ops()->valid(&Z, A(5u, 1u, 1u, 1u)));
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = addr(5u, 1u, 0u, ZSRC_ZW_SLOT_MOTION);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, 2u, &b, NOW));
    clear();
    advance(MIN_MS);
    notif(5u, 0x07u, 0x08u); /* motion from a door sensor: not one of its inputs */
    notif_p(5u, 2u, 0x06u, 0x16u, -1); /* an endpoint it lacks */
    notif_p(5u, 7u, 0x06u, 0x16u, -1); /* one beyond any device's */
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u); /* alive, once a minute */
}

/* ---- check-ins, reachability, grace (network.md §8; ZW-012 via the hub) ---------------------- */

/* A Wake Up Notification, a heartbeat or any frame that changes nothing is a check-in for every
 * input of the device, at most once a minute. */
static void test_checkins(void)
{
    zsrc_zw_desc_t d = door(5u, 1u);
    d.endpoint_count = 1u;
    d.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&d, 1, 0x05u, EV(1) | EV(2));
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(5u, 1u, 1u, ZSRC_ZW_SLOT_WATER));
    clear();
    wake(5u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u);
    EXPECT(2u, ALARM_ZEV_CHECKIN, 1u);
    advance(59999u);
    wake(5u);
    frame(5u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u);
    advance(1u);
    frame(5u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 2u);
    EXPECT(2u, ALARM_ZEV_CHECKIN, 2u);
    /* an input bound meanwhile hears the device's next frame, however soon */
    zone_hub_unbind(&H, 2u, NOW);
    bind(2u, addr(5u, 1u, 1u, ZSRC_ZW_SLOT_WATER));
    frame(5u);
    EXPECT(2u, ALARM_ZEV_CHECKIN, 3u);
    /* a report that feeds an input is not a check-in of its own (the hub counts it) */
    advance(MIN_MS);
    clear();
    notif(5u, 0x06u, 0x16u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 0u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
}

/* network.md §8 through the hub: UNREACHABLE after the device's own grace - a sleeper's check-in
 * + 10 min, a listening device's 30 min - never for a sleeper that never checks in; REACHABLE
 * again at its next frame. A sleeper that confirmed a longer check-in of its own (network.md §7,
 * ZW-012) has the new grace at once, and its inputs are not asked again for it. */
static void test_reachability_and_grace(void)
{
    zsrc_zw_desc_t sleeper = door(5u, 1u);
    sleeper.check_in_s = 4u * 3600u;
    add(&sleeper);
    zsrc_zw_desc_t mains = smoke_co(7u, 1u);
    add(&mains);
    zsrc_zw_desc_t never = door(9u, 1u);
    never.check_in_s = 0u;
    add(&never);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(3u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(5u, 0x06u, 0x17u);
    notif(7u, 0x01u, 0x00u);
    notif(9u, 0x06u, 0x17u);
    clear();
    zsrc_zw_reachable(&Z, 5u, false, NOW);
    zsrc_zw_reachable(&Z, 7u, false, NOW);
    zsrc_zw_reachable(&Z, 9u, false, NOW);
    advance(1800u * 1000u - 1u);
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 0u);
    advance(1u);
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 1u); /* listening: 30 min */
    advance((4u * 3600u + 600u) * 1000u - 1800u * 1000u - 1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    advance(1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u); /* sleeper: T + 10 min */
    advance(48u * 3600u * 1000u);
    EXPECT(3u, ALARM_ZEV_UNREACHABLE, 0u); /* never checks in: never CHECK for silence */
    frame(9u); /* it speaks: reachable, and a check-in (nothing else changed) */
    EXPECT(3u, ALARM_ZEV_CHECKIN, 1u);
    /* back */
    zsrc_zw_reachable(&Z, 7u, true, NOW);
    EXPECT(2u, ALARM_ZEV_REACHABLE, 1u);
    wake(5u); /* its next frame, before zw_health's word */
    EXPECT(1u, ALARM_ZEV_REACHABLE, 1u);
    /* the grace the binding names wins */
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_DOOR);
    m.grace_s = 10u;
    bind_m(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT), &m);
    frame(5u); /* a check-in */
    clear();
    zsrc_zw_reachable(&Z, 5u, false, NOW);
    advance(10000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
    frame(5u); /* within the minute of its check-in: no check-in, still its end of unreachability */
    EXPECT(1u, ALARM_ZEV_CHECKIN, 0u);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    /* a sleeper confirms a check-in of 12 h instead of 1 h: its grace follows, nothing is asked */
    const zsrc_zw_desc_t later = door(11u, 1u);
    add(&later);
    bind(4u, addr(11u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(11u, 0x06u, 0x17u);
    uint16_t node = 0u;
    uint8_t eps = 0u;
    while (zsrc_zw_take_refresh(&Z, &node, &eps)) {
    }
    TEST_ASSERT_TRUE(zsrc_zw_check_in(&Z, 11u, 1u, 12u * 3600u));
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, zsrc_zw_find(&Z, 11u)->check_in_s);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_FALSE(zsrc_zw_check_in(&Z, 11u, 2u, 3600u)); /* another device's generation */
    TEST_ASSERT_FALSE(zsrc_zw_check_in(&Z, 7u, 1u, 3600u));  /* listening */
    TEST_ASSERT_FALSE(zsrc_zw_check_in(&Z, 7u, 1u, 0u));     /* listening, 0 too */
    TEST_ASSERT_FALSE(zsrc_zw_check_in(&Z, 99u, 1u, 3600u)); /* no such device */
    TEST_ASSERT_EQUAL_UINT32(12u * 3600u, zsrc_zw_find(&Z, 11u)->check_in_s);
    clear();
    zsrc_zw_reachable(&Z, 11u, false, NOW);
    advance((3600u + 600u) * 1000u + MIN_MS);
    EXPECT(4u, ALARM_ZEV_UNREACHABLE, 0u); /* the old grace and a minute: not CHECK */
    advance((43200u - 3600u) * 1000u - MIN_MS - 1u);
    EXPECT(4u, ALARM_ZEV_UNREACHABLE, 0u);
    advance(1u);
    EXPECT(4u, ALARM_ZEV_UNREACHABLE, 1u); /* 12 h + 10 min */
    zsrc_zw_device_gone(&Z, 11u, 1u, NOW);
    TEST_ASSERT_FALSE(zsrc_zw_check_in(&Z, 11u, 1u, 3600u)); /* gone */
}

/* ZW-015 ("interval 0 = not supervised"), ZW-012, network.md §8: a sleeper's interval read again
 * as 0 (it now wakes only by hand) is taken. Its zone no longer claims supervision - no SUPERVISED,
 * no LIFE_SAFETY, not trusted for fire, grace never - and a CHECK its silence raised clears at once
 * (the source vouches for it, as for any sleeper that never checks in); none comes again, two days
 * on. An interval again: supervised again, its grace back. */
static void test_check_in_zero_not_supervised(void)
{
    const zone_source_ops_t *ops = zsrc_zw_ops();
    const zsrc_zw_desc_t d = door(11u, 1u);
    add(&d);
    up();
    bind(1u, addr(11u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(11u, 0x06u, 0x17u);
    const zsrc_address_t a = addr(11u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT);
    TEST_ASSERT_EQUAL_UINT32(3600u, zsrc_zw_find(&Z, 11u)->check_in_s);
    TEST_ASSERT_TRUE((info(1u).caps & ZSRC_CAP_LIFE_SAFETY) != 0u);
    TEST_ASSERT_TRUE(zsrc_trusted_for(info(1u).caps, ALARM_ZT_FIRE));
    clear();
    zsrc_zw_reachable(&Z, 11u, false, NOW);
    advance((3600u + 600u) * 1000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u); /* CHECK after its grace */
    TEST_ASSERT_TRUE(info(1u).unreachable);
    TEST_ASSERT_TRUE(zsrc_zw_check_in(&Z, 11u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, zsrc_zw_find(&Z, 11u)->check_in_s);
    const uint32_t caps = ops->caps_of(&Z, &a);
    TEST_ASSERT_EQUAL_HEX32(0u, caps & (ZSRC_CAP_SUPERVISED | ZSRC_CAP_LIFE_SAFETY));
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_AUTH) != 0u); /* still encrypted */
    TEST_ASSERT_EQUAL_HEX32(caps, info(1u).caps);    /* what the zone page reads */
    TEST_ASSERT_FALSE(zsrc_trusted_for(info(1u).caps, ALARM_ZT_FIRE));
    TEST_ASSERT_EQUAL_UINT32(ZSRC_GRACE_NEVER, ops->grace_of(&Z, &a));
    EXPECT(1u, ALARM_ZEV_REACHABLE, 1u); /* vouched for */
    TEST_ASSERT_FALSE(info(1u).unreachable);
    TEST_ASSERT_TRUE(zsrc_zw_check_in(&Z, 11u, 1u, 0u)); /* again: nothing more */
    EXPECT(1u, ALARM_ZEV_REACHABLE, 1u);
    advance(48u * 3600u * 1000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u); /* never CHECK for its silence */
    TEST_ASSERT_FALSE(info(1u).check);
    /* it checks in again: supervised again */
    TEST_ASSERT_TRUE(zsrc_zw_check_in(&Z, 11u, 1u, 3600u));
    TEST_ASSERT_TRUE((ops->caps_of(&Z, &a) & ZSRC_CAP_SUPERVISED) != 0u);
    TEST_ASSERT_TRUE((ops->caps_of(&Z, &a) & ZSRC_CAP_LIFE_SAFETY) != 0u);
    TEST_ASSERT_EQUAL_UINT32(3600u + 600u, ops->grace_of(&Z, &a));
}

/* ---- trust per device (zones.md §6; ZW-015) -------------------------------------------------- */

/* ZW-015: Security 2 and supervised = authenticated and life-safety; S0 = authenticated; none =
 * neither; a sleeper that never checks in is not supervised (so no life safety either); the zone
 * page reads it from zone_hub_info(). */
static void test_trust_per_device(void)
{
    const zone_source_ops_t *ops = zsrc_zw_ops();
    const uint32_t base = ZSRC_CAP_LEARN | ZSRC_CAP_SUPERVISED | ZSRC_CAP_TAMPER |
                          ZSRC_CAP_BATTERY | ZSRC_CAP_OFFLINE_OK;
    TEST_ASSERT_EQUAL_HEX32(base, ops->caps);
    static const struct {
        uint8_t security;
        bool listening;
        uint32_t check_in;
        uint32_t caps;
    } ROWS[] = {
        {ZSRC_ZW_SEC_S2_ACCESS, false, 3600u, ZSRC_CAP_AUTH | ZSRC_CAP_LIFE_SAFETY},
        {ZSRC_ZW_SEC_S2_AUTH, false, 3600u, ZSRC_CAP_AUTH | ZSRC_CAP_LIFE_SAFETY},
        {ZSRC_ZW_SEC_S2_UNAUTH, false, 3600u, ZSRC_CAP_AUTH | ZSRC_CAP_LIFE_SAFETY},
        {ZSRC_ZW_SEC_S2_UNAUTH, true, 0u, ZSRC_CAP_AUTH | ZSRC_CAP_LIFE_SAFETY},
        {ZSRC_ZW_SEC_S0, false, 3600u, ZSRC_CAP_AUTH},
        {ZSRC_ZW_SEC_NONE, false, 3600u, 0u},
        {ZSRC_ZW_SEC_UNKNOWN, true, 0u, 0u},
        {ZSRC_ZW_SEC_S2_AUTH, false, 0u, ZSRC_CAP_AUTH}, /* never checks in */
    };
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        zsrc_zw_desc_t d = door((uint16_t)(5u + i), 1u);
        d.security = ROWS[i].security;
        d.listening = ROWS[i].listening;
        d.check_in_s = ROWS[i].check_in;
        add(&d);
        const zsrc_address_t a = addr((uint16_t)(5u + i), 1u, 0u, ZSRC_ZW_SLOT_CONTACT);
        uint32_t want = base | ROWS[i].caps;
        if (!ROWS[i].listening && ROWS[i].check_in == 0u) {
            want &= ~ZSRC_CAP_SUPERVISED;
        }
        char msg[16];
        snprintf(msg, sizeof(msg), "row %u", (unsigned)i);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(want, ops->caps_of(&Z, &a), msg);
    }
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(10u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    TEST_ASSERT_TRUE(zsrc_trusted_for(info(1u).caps, ALARM_ZT_FIRE));
    TEST_ASSERT_FALSE(zsrc_trusted_for(info(2u).caps, ALARM_ZT_FIRE)); /* not encrypted */
    TEST_ASSERT_EQUAL_HEX32(0u, info(2u).caps & ZSRC_CAP_AUTH);
    /* an address of no device of the network: the source's own */
    TEST_ASSERT_EQUAL_HEX32(base, ops->caps_of(&Z, A(99u, 1u, 0u, 1u)));
}

/* ---- the link, fresh state (zones.md §3; ZSRC-015) ------------------------------------------- */

/* The source is usable only with its network up; down is one trouble for the source after its
 * grace, never a CHECK per zone; with it back every input has its device's grace to report again,
 * and a device that never checks in is vouched for (it can't be asked). */
static void test_link_trouble(void)
{
    const zone_source_ops_t *ops = zsrc_zw_ops();
    TEST_ASSERT_FALSE(ops->usable(&Z));
    zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    zsrc_zw_desc_t never = door(9u, 1u);
    never.check_in_s = 0u;
    add(&never);
    up();
    TEST_ASSERT_TRUE(ops->usable(&Z));
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(5u, 0x06u, 0x17u);
    notif(9u, 0x06u, 0x17u);
    clear();
    zsrc_zw_link(&Z, false, NOW); /* the radio stops answering */
    TEST_ASSERT_FALSE(ops->usable(&Z));
    /* ZW-032: the radio's own trouble after ZSRC_ZW_LINK_GRACE_S, not the sleepers' grace (it
     * was 70 min: a pulled module went unsaid that long) */
    advance(ZSRC_ZW_LINK_GRACE_S * 1000u - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_ON);
    advance(1u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON);
    advance(24u * 3600u * 1000u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 0u);
    /* back: the sleeper has its T + 10 min to speak, the one that never checks in is vouched */
    up();
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_OFF);
    TEST_ASSERT_TRUE(info(1u).unreachable);
    TEST_ASSERT_FALSE(info(2u).unreachable);
    wake(5u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    /* another break: this time the sleeper stays silent - its CHECK after T + 10 min */
    zsrc_zw_link(&Z, false, NOW);
    up();
    advance(4200u * 1000u - 1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u);
    advance(1u);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 1u);
}

/* zones.md §3 "Fresh state": when the link comes up, and when an input starts to be watched and
 * its state is not known, the device is asked for it (each request handed out once); until it
 * answers the input has no state. */
static void test_fresh_state(void)
{
    zsrc_zw_desc_t d = door(5u, 1u);
    d.endpoint_count = 1u;
    d.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add_notif(&d, 1, 0x05u, EV(1) | EV(2));
    add(&d);
    const zsrc_zw_desc_t other = door(6u, 1u);
    add(&other);
    uint16_t node = 0u;
    uint8_t eps = 0u;
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* not before the link */
    up();
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* nothing watched */
    bind(1u, addr(5u, 1u, 1u, ZSRC_ZW_SLOT_WATER));
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_UINT16(5u, node);
    TEST_ASSERT_EQUAL_HEX8(0x02u, eps);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* once */
    TEST_ASSERT_FALSE(info(1u).known);
    notif_p(5u, 1u, 0x05u, 0x00u, -1); /* its answer: dry */
    TEST_ASSERT_TRUE(info(1u).known);
    TEST_ASSERT_FALSE(info(1u).active);
    /* a new input whose state is known: told at once, and its device asked all the same */
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_HEX8(0x01u, eps);
    notif(5u, 0x06u, 0x16u);
    zone_hub_unbind(&H, 2u, NOW);
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    TEST_ASSERT_TRUE(info(2u).active);
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_HEX8(0x01u, eps);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps));
    /* the link again: everything watched is asked afresh, what was known is forgotten */
    zsrc_zw_link(&Z, false, NOW);
    up();
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_UINT16(5u, node);
    TEST_ASSERT_EQUAL_HEX8(0x03u, eps);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* device 6: nothing watched */
    char w[24];
    zsrc_zw_state_words(zsrc_zw_find(&Z, 5u), 0u, ZSRC_ZW_SLOT_CONTACT, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("", w);
}

/* ZW-024, ZW-013 (network.md §9): a jam is not a link down for the source. The hub hears the link
 * down at once (sink.link_now: the jam detector already waited, no ZSRC_ZW_LINK_GRACE_S), but the
 * source keeps its link: a door that opens during the jam is told. When the jam ends the hub
 * hears the link up, what the devices said is kept, and every watched device is asked afresh. */
static void test_jam_keeps_reports(void)
{
    const zone_source_ops_t *ops = zsrc_zw_ops();
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    zsrc_zw_desc_t never = door(9u, 1u);
    never.check_in_s = 0u;
    add(&never);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(5u, 0x06u, 0x17u);
    notif(9u, 0x06u, 0x17u);
    uint16_t node = 0u;
    uint8_t eps = 0u;
    while (zsrc_zw_take_refresh(&Z, &node, &eps)) {
    }
    clear();
    zsrc_zw_jammed(&Z, true, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, LINK_NOW);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON); /* at once, not after ZSRC_ZW_LINK_GRACE_S */
    TEST_ASSERT_TRUE(info(1u).link_down);
    TEST_ASSERT_TRUE(Z.link_up);
    TEST_ASSERT_TRUE(ops->usable(&Z));
    zsrc_zw_jammed(&Z, true, NOW); /* told once */
    TEST_ASSERT_EQUAL_UINT(1u, LINK_NOW);
    /* reports flow: the door opens during the jam */
    notif(5u, 0x06u, 0x16u);
    TEST_ASSERT_TRUE(info(1u).known);
    TEST_ASSERT_TRUE(info(1u).active);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* nothing asked while jammed */
    advance(30u * MIN_MS);
    EXPECT(1u, ALARM_ZEV_UNREACHABLE, 0u); /* one trouble for the source, never a CHECK each */
    EXPECT(2u, ALARM_ZEV_UNREACHABLE, 0u);
    wake(5u); /* it checks in just before the air clears: its check-in limiter starts */
    /* the air clears: the hub hears the link up; the source forgot nothing and asks afresh */
    zsrc_zw_jammed(&Z, false, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, LINK_NOW);
    TEST_ASSERT_FALSE(info(1u).link_down);
    TEST_ASSERT_TRUE(info(1u).active);
    char w[24];
    zsrc_zw_state_words(zsrc_zw_find(&Z, 5u), 0u, ZSRC_ZW_SLOT_CONTACT, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("Open", w);
    TEST_ASSERT_TRUE(info(1u).unreachable); /* ZSRC-015: until it speaks again */
    TEST_ASSERT_FALSE(info(2u).unreachable); /* the one that never checks in is vouched for */
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_UINT16(5u, node);
    TEST_ASSERT_EQUAL_HEX8(0x01u, eps);
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_UINT16(9u, node);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_OFF);
    wake(5u); /* the same minute: passed on all the same - the jam's end reset its limiter */
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    zsrc_zw_jammed(&Z, false, NOW); /* told once */
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF);
}

/* ZW-024, ZW-032, ZSRC-015: a real link down (the module gone) keeps its rule during a jam -
 * reports are not taken, and the link-up that follows forgets what was known; a jam that ends
 * while the link is down tells the hub nothing, and a link that comes up while jammed stays down
 * for the hub until the air clears. */
static void test_jam_and_a_real_link_down(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    notif(5u, 0x06u, 0x16u);
    uint16_t node = 0u;
    uint8_t eps = 0u;
    while (zsrc_zw_take_refresh(&Z, &node, &eps)) {
    }
    clear();
    zsrc_zw_jammed(&Z, true, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, LINK_NOW);
    zsrc_zw_link(&Z, false, NOW); /* the module is gone */
    TEST_ASSERT_FALSE(Z.link_up);
    notif(5u, 0x06u, 0x17u); /* not taken without the link */
    TEST_ASSERT_TRUE(info(1u).active);
    zsrc_zw_jammed(&Z, false, NOW); /* zw_net forgets the air without its radio */
    TEST_ASSERT_TRUE(info(1u).link_down);
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps));
    up();
    TEST_ASSERT_FALSE(info(1u).link_down);
    char w[24];
    zsrc_zw_state_words(zsrc_zw_find(&Z, 5u), 0u, ZSRC_ZW_SLOT_CONTACT, NOW, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("", w); /* forgotten, as after any break */
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps));
    TEST_ASSERT_EQUAL_UINT16(5u, node);
    /* up while jammed: the hub keeps the link down, at once, until the air clears */
    zsrc_zw_link(&Z, false, NOW);
    zsrc_zw_jammed(&Z, true, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, LINK_NOW); /* the hub already hears the link down */
    up();
    TEST_ASSERT_EQUAL_UINT(2u, LINK_NOW);
    TEST_ASSERT_TRUE(info(1u).link_down);
    notif(5u, 0x06u, 0x16u); /* taken: the link is up */
    TEST_ASSERT_TRUE(info(1u).active);
    zsrc_zw_jammed(&Z, false, NOW);
    TEST_ASSERT_FALSE(info(1u).link_down);
}

/* A watch for a device the source doesn't know is kept (a binding from before the network came
 * up, a device of an older firmware's): CHECK after the grace until it reports (ZSRC-010). */
static void test_unknown_device_watched(void)
{
    const zsrc_address_t a = addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT);
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = a;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, 1u, &b, NOW)); /* not valid */
    /* as a stored binding is, at the hub's start before the network is up: watched all the same */
    TEST_ASSERT_TRUE(zsrc_zw_ops()->watch(&Z, &a, NULL, NOW));
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    uint16_t node = 0u;
    uint8_t eps = 0u;
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps));
    up();
    TEST_ASSERT_TRUE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* its state is asked for */
    TEST_ASSERT_EQUAL_UINT16(5u, node);
    zsrc_zw_ops()->unwatch(&Z, &a);
    zsrc_zw_link(&Z, false, NOW);
    up();
    TEST_ASSERT_FALSE(zsrc_zw_take_refresh(&Z, &node, &eps)); /* not watched any more */
}

/* The source's limits (zones.md §7): ZSRC_ZW_DEV_MAX devices - a device that left makes room -
 * and ZSRC_ZW_WATCH_MAX inputs. */
static void test_limits(void)
{
    for (uint16_t n = 1u; n <= ZSRC_ZW_DEV_MAX; n++) {
        const zsrc_zw_desc_t d = door(n, 1u);
        add(&d);
    }
    const zsrc_zw_desc_t more = door(200u, 1u);
    TEST_ASSERT_FALSE(zsrc_zw_device(&Z, &more, NOW));
    zsrc_zw_device_gone(&Z, 7u, 1u, NOW);
    TEST_ASSERT_TRUE(zsrc_zw_device(&Z, &more, NOW));
    TEST_ASSERT_NULL(zsrc_zw_find(&Z, 7u));
    for (uint16_t i = 0u; i < ZSRC_ZW_WATCH_MAX; i++) {
        const zsrc_address_t a = addr((uint16_t)(300u + i), 1u, 0u, 1u);
        TEST_ASSERT_TRUE(zsrc_zw_ops()->watch(&Z, &a, NULL, NOW));
    }
    const zsrc_address_t a = addr(299u, 1u, 0u, 1u);
    TEST_ASSERT_FALSE(zsrc_zw_ops()->watch(&Z, &a, NULL, NOW));
    const zsrc_address_t again = addr(300u, 1u, 0u, 1u);
    TEST_ASSERT_TRUE(zsrc_zw_ops()->watch(&Z, &again, NULL, NOW)); /* one it has: a new mapping */
}

/* ---- the quirks table (zones.md §8) ---------------------------------------------------------- */

/* zones.md §8: the starter rows by manufacturer / type / ID; the model name and class; the ZSE43's
 * tilt fed by its door events and its shock as the impact input; supervision off for old
 * firmware; rows a bench adds come first; hide and idle_after. */
static void test_quirks(void)
{
    size_t n = 0u;
    const zsrc_zw_quirk_t *t = zsrc_zw_quirk_table(&n);
    TEST_ASSERT_EQUAL_UINT(12u, n);
    static const struct {
        uint16_t type, id;
        const char *name;
        uint8_t cls;
    } STARTER[] = {
        {0x7000u, 0xE001u, "Zooz ZSE41 open/close sensor", ALARM_DEVCLASS_DOOR},
        {0x7000u, 0xE002u, "Zooz ZSE42 water leak sensor", ALARM_DEVCLASS_WATER},
        {0x7000u, 0xE003u, "Zooz ZSE43 tilt and shock sensor", ALARM_DEVCLASS_GARAGE_DOOR},
        {0x7000u, 0xE004u, "Zooz ZSE44 temperature sensor", ALARM_DEVCLASS_TEMPERATURE},
        {0x0301u, 0x0012u, "Zooz ZSE18 motion sensor", ALARM_DEVCLASS_MOTION},
        {0x0201u, 0x0006u, "Zooz ZSE11 Q sensor", ALARM_DEVCLASS_MOTION},
        {0x0001u, 0x0005u, "Zooz ZSE29 outdoor motion sensor", ALARM_DEVCLASS_MOTION},
        {0x0004u, 0x0006u, "Zooz ZSE70 outdoor motion sensor", ALARM_DEVCLASS_MOTION},
    };
    for (size_t i = 0; i < sizeof(STARTER) / sizeof(STARTER[0]); i++) {
        const zsrc_zw_quirk_t *q = zsrc_zw_quirk_for(0x027Au, STARTER[i].type, STARTER[i].id);
        TEST_ASSERT_EQUAL_PTR(&t[i], q);
        TEST_ASSERT_EQUAL_STRING(STARTER[i].name, q->name);
        TEST_ASSERT_EQUAL_UINT8(STARTER[i].cls, q->device_class);
    }
    TEST_ASSERT_NULL(zsrc_zw_quirk_for(0x027Bu, 0x7000u, 0xE001u));
    /* ZW-013: the Ecolink DWZWAVE25 never sends idle (Z-Wave JS forceNotificationIdleReset) */
    TEST_ASSERT_EQUAL_PTR(&t[8], zsrc_zw_quirk_for(0x014Au, 0x0004u, 0x0002u));
    TEST_ASSERT_EQUAL_STRING("Ecolink DWZWAVE25 door/window sensor", t[8].name);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, t[8].device_class);
    TEST_ASSERT_EQUAL_UINT16(300u, t[8].idle_after_s);
    for (size_t i = 0; i < 8u; i++) {
        TEST_ASSERT_EQUAL_UINT16(0u, t[i].idle_after_s);
    }
    TEST_ASSERT_EQUAL_STRING("Click the Z-Wave button 4 times quickly", t[0].wake_words);
    /* ZW-043: First Alert ZCOMBO (two product IDs) and ZSMOKE read through an alarm map */
    TEST_ASSERT_EQUAL_PTR(&t[9], zsrc_zw_quirk_for(0x0138u, 0x0001u, 0x0002u));
    TEST_ASSERT_EQUAL_PTR(&t[10], zsrc_zw_quirk_for(0x0138u, 0x000Cu, 0x0001u));
    TEST_ASSERT_EQUAL_PTR(&t[11], zsrc_zw_quirk_for(0x0138u, 0x0001u, 0x0001u));
    TEST_ASSERT_EQUAL_STRING("First Alert ZSMOKE smoke alarm", t[11].name);
    TEST_ASSERT_EQUAL_UINT8(0x0Du, t[11].alarm_map[4].alarm_type);
    TEST_ASSERT_EQUAL_UINT8(0u, t[11].alarm_map[5].alarm_type); /* no CO rows */
    TEST_ASSERT_EQUAL_UINT8(0x02u, t[9].alarm_map[5].alarm_type);
    TEST_ASSERT_EQUAL_UINT8(0u, t[9].alarm_map[7].alarm_type);
    for (size_t i = 0; i < 9u; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, t[i].alarm_map[0].alarm_type);
    }
    /* supervision: ZSE11 before 2.0, ZSE29 before 2.2; unknown firmware counts as old */
    TEST_ASSERT_TRUE(zsrc_zw_quirk_no_supervision(&t[5], true, 0x0109u));
    TEST_ASSERT_FALSE(zsrc_zw_quirk_no_supervision(&t[5], true, 0x0200u));
    TEST_ASSERT_TRUE(zsrc_zw_quirk_no_supervision(&t[6], false, 0xFFFFu));
    TEST_ASSERT_FALSE(zsrc_zw_quirk_no_supervision(&t[6], true, 0x0202u));
    TEST_ASSERT_FALSE(zsrc_zw_quirk_no_supervision(&t[0], false, 0u));
    TEST_ASSERT_FALSE(zsrc_zw_quirk_no_supervision(NULL, false, 0u));
    /* the ZSE43: its door events are its tilt, "moved" its shock even with a cover */
    zsrc_zw_desc_t tilt = door(5u, 1u);
    tilt.product_id = 0xE003u;
    tilt.ep[0].notif[1].events |= EV(0x09); /* its shock; it has a cover too */
    add(&tilt);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_TILT) | S(ZSRC_ZW_SLOT_IMPACT), offered(5u, 0u));
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_TILT));
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_IMPACT));
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_GARAGE_DOOR,
                            zsrc_zw_slot_class(zsrc_zw_find(&Z, 5u), ZSRC_ZW_SLOT_TILT));
    clear();
    notif(5u, 0x06u, 0x16u);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    notif(5u, 0x07u, 0x09u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    EXPECT(1u, ALARM_ZEV_TAMPER, 0u);
    /* a bench's rows first: hide an input, a window's class, motion that never ends */
    static const zsrc_zw_quirk_t ROWS[] = {
        {.manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE001u,
         .name = "Window contact", .device_class = ALARM_DEVCLASS_WINDOW},
        {.manufacturer = 0x0202u, .product_type = ZSRC_ZW_QUIRK_ANY,
         .product_id = ZSRC_ZW_QUIRK_ANY, .name = "Endless PIR", .idle_after_s = 120u,
         .hide = {S(ZSRC_ZW_SLOT_GLASS_BREAK) | S(ZSRC_ZW_SLOT_IMPACT)}},
        {.manufacturer = 0x0303u, .product_type = 0x0001u, .product_id = 0x0001u,
         .name = "Remote", .offer = {0u, S(ZSRC_ZW_SLOT_EMERGENCY)}},
    };
    zsrc_zw_set_quirks(&Z, ROWS, 3u);
    zsrc_zw_desc_t remote = base_desc(8u, 1u, true); /* its button end point lists nothing */
    remote.has_identity = true;
    remote.manufacturer = 0x0303u;
    remote.product_type = 0x0001u;
    remote.product_id = 0x0001u;
    remote.endpoint_count = 1u;
    remote.ep[1].inputs = ZSRC_ZW_INPUTS_OFFERED;
    add(&remote);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_EMERGENCY), offered(8u, 1u));
    const zsrc_zw_desc_t window = door(6u, 1u);
    add(&window);
    char words[64];
    zsrc_zw_ops()->describe(&Z, A(6u, 1u, 0u, 1u), words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Window contact · window contact", words);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_WINDOW,
                            zsrc_zw_slot_class(zsrc_zw_find(&Z, 6u), ZSRC_ZW_SLOT_CONTACT));
    zsrc_zw_desc_t endless = pir(7u, 1u);
    endless.has_identity = true;
    endless.manufacturer = 0x0202u;
    endless.product_type = 0x0001u;
    endless.product_id = 0x0001u;
    add(&endless);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_MOTION), offered(7u, 0u));
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    clear();
    notif(7u, 0x07u, 0x08u);
    advance(60000u);
    notif(7u, 0x07u, 0x08u); /* again: its 120 s start over */
    advance(119999u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 0u);
    advance(1u);
    EXPECT_LAST(3u, ALARM_ZEV_RESTORE);
    zsrc_zw_set_quirks(&Z, NULL, 0u);
    TEST_ASSERT_EQUAL_PTR(&t[0], zsrc_zw_quirk_lookup(&Z, 0x027Au, 0x7000u, 0xE001u));
}

/* An Ecolink DWZWAVE25 (zw_quirks.c's row): door open/closed, intrusion and cover tamper, sent
 * and never idled. Another product ID has no row. */
static zsrc_zw_desc_t ecolink(uint16_t node, uint16_t gen, uint16_t product_id)
{
    zsrc_zw_desc_t d = base_desc(node, gen, true);
    d.has_identity = true;
    d.manufacturer = 0x014Au;
    d.product_type = 0x0004u;
    d.product_id = product_id;
    add_notif(&d, 0, 0x06u, EV(0x16) | EV(0x17));
    add_notif(&d, 0, 0x07u, EV(0x02) | EV(0x03));
    return d;
}

/* ZW-013, zones.md §8 idle_after_s: the quirk ends intrusion and cover tamper 300 s after each
 * was last set, each on its own timer, and the zone restores; a repeat starts the timer again,
 * the device's own idle stops it; a door's open/closed (06 16/17) has no idle state and is never
 * ended by it. */
static void test_idle_reset_any_variable(void)
{
    const zsrc_zw_desc_t d = ecolink(5u, 1u, 0x0002u);
    add(&d);
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_CONTACT) | S(ZSRC_ZW_SLOT_INTRUSION), offered(5u, 0u));
    const zsrc_zw_quirk_t *q = zsrc_zw_find(&Z, 5u)->quirk;
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_UINT16(300u, q->idle_after_s);
    TEST_ASSERT_EQUAL_STRING("Pull the plastic tab from the back", q->add_words);
    TEST_ASSERT_EQUAL_STRING("Open the top cover", q->wake_words);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_INTRUSION));
    clear();
    notif(5u, 0x07u, 0x02u); /* intrusion at t */
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    advance(100000u);
    notif(5u, 0x07u, 0x03u); /* cover tamper at t + 100 s */
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
    advance(199999u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(2u).active);
    advance(1u); /* t + 300 s: intrusion ends, the tamper has its own timer */
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_FALSE(info(2u).active);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    advance(99999u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    advance(1u); /* t + 400 s: the tamper ends on every input of the device */
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    /* a repeat starts its 300 s again */
    clear();
    notif(5u, 0x07u, 0x02u);
    advance(200000u);
    notif(5u, 0x07u, 0x02u);
    advance(299999u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    advance(1u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    /* the device's own idle ends it, and the timer has nothing left to say */
    clear();
    notif(5u, 0x07u, 0x02u);
    notif_p(5u, 0u, 0x07u, 0x00u, 0x02);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    const unsigned told = NR;
    advance(600000u);
    TEST_ASSERT_EQUAL_UINT(told, NR);
    /* a door left open stays open: it has no idle state */
    clear();
    notif(5u, 0x06u, 0x16u);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    advance(3600000u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    notif(5u, 0x06u, 0x17u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    /* ZW-013, ZSRC-015: a link break forgets the intrusion and its timer - the stale timer must
     * not answer for a state the device hasn't spoken of since - but the cover tamper is a
     * condition the hub keeps across the break, and its timer still ends it at t + 300 s */
    clear();
    notif(5u, 0x07u, 0x02u); /* intrusion at t */
    notif(5u, 0x07u, 0x03u); /* cover tamper at t */
    EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
    advance(60000u);
    zsrc_zw_link(&Z, false, NOW);
    advance(30000u);
    up();
    const unsigned restores = count(2u, ALARM_ZEV_RESTORE);
    advance(209999u); /* t + 299.999 s */
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    advance(1u); /* t + 300 s */
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    advance(3600000u);
    EXPECT(2u, ALARM_ZEV_RESTORE, restores);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
}

/* ZW-013: without the quirk (another Ecolink product ID, no row) intrusion and cover tamper stay
 * set until the device says otherwise. */
static void test_idle_reset_needs_the_quirk(void)
{
    const zsrc_zw_desc_t d = ecolink(5u, 1u, 0x0001u);
    add(&d);
    TEST_ASSERT_NULL(zsrc_zw_find(&Z, 5u)->quirk);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_INTRUSION));
    clear();
    notif(5u, 0x07u, 0x02u);
    notif(5u, 0x07u, 0x03u);
    advance(24u * 3600000u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(2u).active);
    notif_p(5u, 0u, 0x07u, 0x00u, 0x03);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
}

/* ZW-013: motion keeps its own timer - Notification's or Binary Sensor's, re-armed by its last
 * motion report, told once; a quirk with idle_after_s also ends the device's other variables
 * (smoke, trouble), and with more than ZSRC_ZW_IDLE_RESET_MAX set at once the one set longest
 * ago ends early rather than staying latched. */
static void test_idle_reset_motion_and_limits(void)
{
    static const zsrc_zw_quirk_t ROWS[] = {
        {.manufacturer = 0x0202u, .product_type = ZSRC_ZW_QUIRK_ANY,
         .product_id = ZSRC_ZW_QUIRK_ANY, .name = "Endless", .idle_after_s = 120u},
    };
    zsrc_zw_set_quirks(&Z, ROWS, 1u);
    zsrc_zw_desc_t p = pir(6u, 1u);
    p.has_identity = true;
    p.manufacturer = 0x0202u;
    p.ep[0].binary = true;
    p.ep[0].binary_types = 1u << 0x0Cu;
    add(&p);
    zsrc_zw_desc_t s = smoke_co(7u, 1u);
    s.has_identity = true;
    s.manufacturer = 0x0202u;
    add(&s);
    up();
    bind(1u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    clear();
    notif(6u, 0x07u, 0x08u);
    advance(60000u);
    binary(6u, 0u, 0x0C, true); /* Binary Sensor motion re-arms the same timer */
    advance(119999u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    advance(1u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    advance(600000u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u); /* told once */
    /* smoke ends too; a maintenance trouble, on its own timer */
    clear();
    notif(7u, 0x01u, 0x02u);
    advance(30000u);
    notif(7u, 0x01u, 0x04u);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 1u);
    advance(90000u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    advance(30000u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    /* nine variables at once: the first set (smoke) ends when the ninth is set */
    clear();
    notif(7u, 0x01u, 0x02u);
    advance(10000u);
    static const uint8_t MORE[][2] = {
        {0x01u, 0x04u}, {0x01u, 0x07u}, {0x01u, 0x08u}, {0x02u, 0x01u},
        {0x02u, 0x04u}, {0x02u, 0x07u}, {0x09u, 0x01u}, {0x09u, 0x02u},
    };
    for (size_t i = 0; i < sizeof(MORE) / sizeof(MORE[0]); i++) {
        EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
        notif(7u, MORE[i][0], MORE[i][1]);
    }
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(3u, ALARM_ZEV_FAULT, 1u);
    advance(119999u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 0u);
    advance(1u);
    EXPECT(3u, ALARM_ZEV_RESTORE, 1u);
    zsrc_zw_set_quirks(&Z, NULL, 0u);
}

/* The picker (zones.md §2): a momentary input that fires while it is open counts as changed; an
 * intrusion input's class is its device's (a contact's door, a motion sensor's), else generic. */
static void test_picker_pulses_and_classes(void)
{
    const zsrc_zw_desc_t p = pir(6u, 1u);
    add(&p);
    up();
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZWAVE, true, NOW));
    notif(6u, 0x07u, 0x05u);
    zsrc_candidate_t c[8];
    const size_t n = zone_hub_candidates(&H, ZSRC_ZWAVE, NULL, 0u, c, 8u);
    TEST_ASSERT_EQUAL_UINT(3u, n); /* motion, glass break, impact */
    TEST_ASSERT_TRUE(zsrc_address_equal(&c[0].addr, A(6u, 1u, 0u, ZSRC_ZW_SLOT_GLASS_BREAK)));
    TEST_ASSERT_TRUE(c[0].changed);
    TEST_ASSERT_FALSE(c[1].changed);
    zsrc_zw_desc_t contact = base_desc(7u, 1u, true);
    add_notif(&contact, 0, 0x07u, EV(1) | EV(2));
    add_notif(&contact, 0, 0x06u, EV(0x16) | EV(0x17));
    add(&contact);
    zsrc_zw_desc_t motion = base_desc(8u, 1u, true);
    add_notif(&motion, 0, 0x07u, EV(2) | EV(8));
    add(&motion);
    zsrc_zw_desc_t alone = base_desc(9u, 1u, true);
    add_notif(&alone, 0, 0x07u, EV(2));
    add(&alone);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR,
                            zsrc_zw_slot_class(zsrc_zw_find(&Z, 7u), ZSRC_ZW_SLOT_INTRUSION));
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_MOTION,
                            zsrc_zw_slot_class(zsrc_zw_find(&Z, 8u), ZSRC_ZW_SLOT_INTRUSION));
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_GENERIC,
                            zsrc_zw_slot_class(zsrc_zw_find(&Z, 9u), ZSRC_ZW_SLOT_INTRUSION));
}

/* ZSRC-015 with a momentary input: a hold whose end falls while the link is down is not told
 * then, nor after the break before the device speaks (its input is unreachable, as every one);
 * the device's next frame is its check-in, and says the input is idle. */
static void test_hold_across_a_break(void)
{
    const zsrc_zw_desc_t p = pir(6u, 1u);
    add(&p);
    up();
    bind(1u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_GLASS_BREAK));
    clear();
    notif(6u, 0x07u, 0x05u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    advance(10000u);
    zsrc_zw_link(&Z, false, NOW);
    advance(10000u);
    up();
    advance(20000u); /* the hold's end passes */
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).unreachable);
    TEST_ASSERT_TRUE(info(1u).active);
    frame(6u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    TEST_ASSERT_FALSE(info(1u).active);
    /* a hold the break cut short: old news, ended by the device's first frame after it */
    notif(6u, 0x07u, 0x05u);
    EXPECT(1u, ALARM_ZEV_FAULT, 2u);
    advance(5000u);
    zsrc_zw_link(&Z, false, NOW);
    up();
    advance(2000u);
    frame(6u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 2u);
}

/* ZW-013, ZSRC-015 with heat's rapid rise: a hold whose end falls while the link is down ends at
 * the device's first frame after the break, as a momentary input's does, though a rate-of-rise
 * detector (Heat 03/04 only) never reports a heat state; the next rise is a new alarm. */
static void test_heat_hold_across_a_break(void)
{
    zsrc_zw_desc_t d = base_desc(9u, 1u, false);
    add_notif(&d, 0, 0x04u, EV(3) | EV(4));
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    clear();
    notif(9u, 0x04u, 0x03u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    advance(10000u);
    zsrc_zw_link(&Z, false, NOW);
    advance(10000u);
    up();
    advance(20000u); /* the hold's end passes */
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    frame(9u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_FALSE(info(1u).active);
    TEST_ASSERT_FALSE(info(1u).unreachable);
    advance(MIN_MS);
    notif(9u, 0x04u, 0x04u); /* the next rise is heard */
    EXPECT(1u, ALARM_ZEV_FAULT, 2u);
    /* a hold the break cut short: ended by the first frame after it */
    advance(5000u);
    zsrc_zw_link(&Z, false, NOW);
    up();
    advance(2000u);
    frame(9u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 2u);
    /* an overheat reported since the break outlasts the cut hold */
    advance(MIN_MS);
    notif(9u, 0x04u, 0x03u);
    EXPECT(1u, ALARM_ZEV_FAULT, 3u);
    advance(5000u);
    zsrc_zw_link(&Z, false, NOW);
    up();
    notif(9u, 0x04u, 0x02u);
    advance(MIN_MS);
    frame(9u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 2u);
    TEST_ASSERT_TRUE(info(1u).active);
}

/* A condition that changes while the link is down - "moved" ends its 30 s - is not lost: the hub
 * keeps conditions across a break, and hears the change when the device next speaks. */
static void test_condition_across_a_break(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    notif(5u, 0x07u, 0x09u); /* moved, with a cover: tamper for 30 s */
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    advance(10000u);
    zsrc_zw_link(&Z, false, NOW);
    advance(30000u);
    up();
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u); /* not before the device speaks */
    wake(5u);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
}

/* ---- V1 Alarm Type and Level (ZW-043) --------------------------------------------------------- */

/* A Notification Report as a v1 device sends it: only the V1 Alarm Type and Level. */
static void v1(uint16_t node, uint8_t alarm_type, uint8_t alarm_level)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_NOTIFICATION;
    r.notif_v1 = true;
    r.notif_v1_type = alarm_type;
    r.notif_v1_level = alarm_level;
    report(node, &r);
}

/* A v2+ report carrying V1 bytes besides its own notification type and event (0: none). */
static void v1_long(uint16_t node, uint8_t alarm_type, uint8_t alarm_level, uint8_t type,
                    uint8_t event)
{
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_NOTIFICATION;
    r.notif_v1_type = alarm_type;
    r.notif_v1_level = alarm_level;
    r.notif_status = 0xFFu;
    r.notif_type = type;
    r.notif_event = event;
    report(node, &r);
}

/* A First Alert smoke alarm as the interview finds a Notification v1 device: its IDs, no types. */
static zsrc_zw_desc_t first_alert(uint16_t node, uint16_t gen, uint16_t type, uint16_t id)
{
    zsrc_zw_desc_t d = base_desc(node, gen, true);
    d.has_identity = true;
    d.manufacturer = 0x0138u;
    d.product_type = type;
    d.product_id = id;
    return d;
}

/* ZW-043, zones.md §8: a ZCOMBO's V1 alarms read through its quirk's alarm map (Z-Wave JS
 * zcombo.json): smoke and CO offered with no notification types, 01/02 FF set them and 00 ends
 * them, 0C FF is the smoke test, 0D FF idles smoke; an unmapped alarm type, alarm type 0 and a
 * level no row names are dropped. Both product IDs. */
static void test_v1_alarm_map_zcombo(void)
{
    const zsrc_zw_desc_t a = first_alert(5u, 1u, 0x0001u, 0x0002u);
    const zsrc_zw_desc_t b = first_alert(6u, 1u, 0x000Cu, 0x0001u);
    add(&a);
    add(&b);
    up();
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_SMOKE) | S(ZSRC_ZW_SLOT_CO), offered(5u, 0u));
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_SMOKE) | S(ZSRC_ZW_SLOT_CO), offered(6u, 0u));
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    bind(3u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    clear();
    v1(5u, 0x01u, 0xFFu);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    v1(5u, 0x01u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    v1(5u, 0x02u, 0xFFu);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    v1(5u, 0x02u, 0x00u);
    EXPECT_LAST(2u, ALARM_ZEV_RESTORE);
    v1(6u, 0x02u, 0xFFu); /* the other product ID */
    EXPECT_LAST(3u, ALARM_ZEV_FAULT);
    /* the test button: the smoke test event (a check-in, "tested"), no alarm */
    advance(MIN_MS);
    clear();
    v1(5u, 0x0Cu, 0xFFu);
    TEST_ASSERT_EQUAL_UINT64(NOW, zsrc_zw_find(&Z, 5u)->tested_at);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    EXPECT(1u, ALARM_ZEV_CHECKIN, 1u);
    v1(5u, 0x0Cu, 0x00u);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
    /* 0D FF: smoke idles */
    v1(5u, 0x01u, 0xFFu);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    v1(5u, 0x0Du, 0xFFu);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    /* nothing the map names: dropped */
    clear();
    v1(5u, 0x15u, 0x01u);
    v1(5u, 0x00u, 0xFFu);
    v1(5u, 0x01u, 0x40u);
    v1(5u, 0x02u, 0x01u);
    TEST_ASSERT_EQUAL_UINT(0u, changes());
    /* the map reads a v2 report that names no notification type too */
    v1_long(5u, 0x01u, 0xFFu, 0x00u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    /* ... but not one with status FE ("no pending notification"): its V1 bytes 01 00 don't
     * idle smoke (ZW-043; an FE answer never idles, as test_notification_status_fe_never_idles) */
    clear();
    {
        zsrc_zw_report_t r;
        memset(&r, 0, sizeof(r));
        r.kind = ZSRC_ZW_RPT_NOTIFICATION;
        r.notif_v1_type = 0x01u;
        r.notif_v1_level = 0x00u;
        r.notif_status = 0xFEu;
        report(5u, &r);
        r.notif_v1_level = 0xFFu; /* nor raise it */
        report(5u, &r);
    }
    TEST_ASSERT_EQUAL_UINT(0u, changes());
    /* ... nor one that names its own: read as that (here an unknown one, dropped) */
    clear();
    v1_long(5u, 0x02u, 0xFFu, 0x0Eu, 0x01u);
    TEST_ASSERT_EQUAL_UINT(0u, changes());
}

/* ZW-043: a ZSMOKE (Z-Wave JS zsmoke.json) offers smoke but no CO, and its map has no CO rows. */
static void test_v1_alarm_map_zsmoke(void)
{
    const zsrc_zw_desc_t a = first_alert(5u, 1u, 0x0001u, 0x0001u);
    add(&a);
    up();
    TEST_ASSERT_EQUAL_HEX32(S(ZSRC_ZW_SLOT_SMOKE), offered(5u, 0u));
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    v1(5u, 0x02u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(0u, changes());
    v1(5u, 0x01u, 0xFFu);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    v1(5u, 0x01u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
}

/* ZW-043, Z-Wave JS GH#1034: a v2+ device whose Supported Reports list Smoke 02 and CO 02 reads
 * V1 bytes (01, 02) as smoke detected and (02, 02) as CO detected - as a v1 report or a v2 one
 * naming no type; V1 bytes naming a type or an event it did not list, a level 0, or a type whose
 * events it could not say, are dropped; a report naming its own type is read as that. */
static void test_v1_alarm_supported_types(void)
{
    zsrc_zw_desc_t d = base_desc(5u, 1u, false);
    add_notif(&d, 0, 0x01u, EV(2) | EV(3));
    add_notif(&d, 0, 0x02u, EV(2));
    add_notif(&d, 0, 0x07u, 0u); /* its events unknown: "all" offered, none read from V1 bytes */
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    bind(3u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    clear();
    v1(5u, 0x01u, 0x02u);
    EXPECT_LAST(1u, ALARM_ZEV_FAULT);
    notif(5u, 0x01u, 0x00u);
    EXPECT_LAST(1u, ALARM_ZEV_RESTORE);
    v1_long(5u, 0x02u, 0x02u, 0x00u, 0x00u);
    EXPECT_LAST(2u, ALARM_ZEV_FAULT);
    clear();
    v1(5u, 0x01u, 0x01u); /* Smoke 01: not listed */
    v1(5u, 0x04u, 0x02u); /* Heat: not a supported type */
    v1(5u, 0x01u, 0x00u); /* level 0: no event */
    v1(5u, 0x07u, 0x08u); /* Home Security: its events unknown */
    TEST_ASSERT_EQUAL_UINT(0u, changes());
    v1_long(5u, 0x01u, 0x02u, 0x07u, 0x08u); /* its own type: motion, not smoke */
    EXPECT_LAST(3u, ALARM_ZEV_FAULT);
    EXPECT(1u, ALARM_ZEV_FAULT, 0u);
}

/* ZW-043: a device with neither a map nor listed events drops V1 alarms as before. */
static void test_v1_alarm_dropped_without_a_map(void)
{
    zsrc_zw_desc_t d = base_desc(5u, 1u, false);
    d.ep[0].binary = true;
    d.ep[0].binary_types = 1u << 0x02;
    add(&d);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    v1(5u, 0x01u, 0xFFu);
    v1(5u, 0x01u, 0x02u);
    v1_long(5u, 0x01u, 0x02u, 0x00u, 0x00u);
    TEST_ASSERT_EQUAL_UINT(0u, changes());
    const zsrc_zw_desc_t w = door(6u, 1u); /* a quirk without a map */
    add(&w);
    bind(2u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    clear();
    v1(6u, 0x06u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(0u, changes());
}

/* ---- an idle names the event it ends (ZW22); conditions are kept per endpoint (ZW21) --------- */

/* A device with the same smoke detector at endpoints 1 and 2 (and nothing at the device itself);
 * each endpoint also reports cover tamper, Power Management battery and smoke maintenance. */
static zsrc_zw_desc_t two_smokes(uint16_t node, uint16_t gen)
{
    zsrc_zw_desc_t d = base_desc(node, gen, false);
    d.endpoint_count = 2u;
    for (uint8_t e = 1u; e <= 2u; e++) {
        d.ep[e].inputs = ZSRC_ZW_INPUTS_OFFERED;
        add_notif(&d, e, 0x01u, EV(1) | EV(2) | EV(4) | EV(5));
        add_notif(&d, e, 0x07u, EV(3));
        add_notif(&d, e, 0x08u, EV(0x0A) | EV(0x0B));
        d.ep[e].binary = true;
        d.ep[e].binary_types = (1u << 0x02) | (1u << 0x08);
    }
    return d;
}

/* ZW22, ZW-013, ZD-4 (Z-Wave JS's Notification idle): Smoke 01 then 02 - one variable, now
 * holding 02. An idle naming 01 is stale: the smoke stays active and nothing is restored. The idle
 * naming 02 ends it with exactly one restore; the same idle again tells nothing new. 01 -> 02 ->
 * idle(02) restores once; an idle without a parameter still ends whatever the variable holds. */
static void test_stale_idle_keeps_the_newer_event(void)
{
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(1u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_CO));
    clear();
    notif(7u, 0x01u, 0x01u);
    notif(7u, 0x01u, 0x02u);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x01); /* stale */
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x01); /* the stale idle again */
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x02);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_FALSE(info(1u).active);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x02); /* a duplicate idle */
    notif_p(7u, 0u, 0x01u, 0x00u, 0x01);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_FALSE(info(1u).active);
    /* 02 then 01: the variable holds 01, and the idle of 02 is the stale one */
    clear();
    notif(7u, 0x01u, 0x02u);
    notif(7u, 0x01u, 0x01u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x02);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x01);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    /* the probe's controls: 01 -> 02 -> idle(02), and 02 -> idle without a parameter */
    clear();
    notif(7u, 0x01u, 0x01u);
    notif(7u, 0x01u, 0x02u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x02);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    clear();
    notif(7u, 0x01u, 0x02u);
    notif(7u, 0x01u, 0x00u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    /* another type's idle never touches smoke; CO's own variable has the same rule */
    clear();
    notif(7u, 0x01u, 0x02u);
    notif(7u, 0x02u, 0x02u);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x01);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    notif_p(7u, 0u, 0x02u, 0x00u, 0x02);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    /* Heat: overheat and underheat are one variable; underheat 06 holds it */
    zsrc_zw_desc_t h = base_desc(8u, 1u, false);
    add_notif(&h, 0, 0x04u, EV(1) | EV(2) | EV(5) | EV(6));
    add(&h);
    bind(3u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_HEAT));
    bind(4u, addr(8u, 1u, 0u, ZSRC_ZW_SLOT_FREEZE));
    clear();
    notif(8u, 0x04u, 0x02u);
    notif(8u, 0x04u, 0x06u); /* the variable takes underheat: overheat ends */
    EXPECT(3u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(4u, ALARM_ZEV_FAULT, 1u);
    clear();
    notif_p(8u, 0u, 0x04u, 0x00u, 0x02);
    notif_p(8u, 0u, 0x04u, 0x00u, 0x05);
    EXPECT(4u, ALARM_ZEV_RESTORE, 0u);
    notif_p(8u, 0u, 0x04u, 0x00u, 0x06);
    EXPECT(4u, ALARM_ZEV_RESTORE, 1u);
}

/* ZW22, ZW-013: motion (Home Security 07/08) and the maintenance, battery and system variables
 * follow the same rule: an idle of the event the variable no longer holds is dropped. */
static void test_stale_idle_motion_and_conditions(void)
{
    const zsrc_zw_desc_t p = pir(6u, 1u);
    add(&p);
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(1u, addr(6u, 1u, 0u, ZSRC_ZW_SLOT_MOTION));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    notif(6u, 0x07u, 0x07u);
    notif(6u, 0x07u, 0x08u);
    notif_p(6u, 0u, 0x07u, 0x00u, 0x07);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    notif_p(6u, 0u, 0x07u, 0x00u, 0x08);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    /* smoke maintenance {04,05} */
    notif(7u, 0x01u, 0x04u);
    notif(7u, 0x01u, 0x05u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x04);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x05);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x05); /* a duplicate idle */
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    /* System hardware failure {01,03} */
    notif(7u, 0x09u, 0x03u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x01);
    EXPECT(2u, ALARM_ZEV_TROUBLE, 2u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    notif_p(7u, 0u, 0x09u, 0x00u, 0x03);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 2u);
    /* Power Management battery {0A,0B} */
    clear();
    notif(7u, 0x08u, 0x0Au);
    notif(7u, 0x08u, 0x0Bu);
    notif_p(7u, 0u, 0x08u, 0x00u, 0x0A);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY, 1u);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    notif_p(7u, 0u, 0x08u, 0x00u, 0x0B);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
}

/* ZW22: a variable without an idle state (F_NO_IDLE: a door's 06 16/17, mains 08 02/03) ignores
 * every idle, naming its event or not; a variable never reported takes an idle as its state. */
static void test_stale_idle_no_idle_variables(void)
{
    const zsrc_zw_desc_t d = door(5u, 1u);
    add(&d);
    const zsrc_zw_desc_t s = smoke_co(7u, 1u);
    add(&s);
    up();
    bind(1u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_CONTACT));
    bind(2u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_MAINS));
    bind(3u, addr(7u, 1u, 0u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    notif(5u, 0x06u, 0x16u);
    notif_p(5u, 0u, 0x06u, 0x00u, 0x16);
    notif_p(5u, 0u, 0x06u, 0x00u, 0x17);
    notif(5u, 0x06u, 0x00u);
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(1u).active);
    notif(7u, 0x08u, 0x02u); /* mains lost */
    notif_p(7u, 0u, 0x08u, 0x00u, 0x02);
    notif_p(7u, 0u, 0x08u, 0x00u, 0x03);
    notif(7u, 0x08u, 0x00u);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(2u).active);
    notif(7u, 0x08u, 0x03u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    /* smoke, never reported: its idle is its first state, inactive (the fresh state's answer) */
    TEST_ASSERT_FALSE(info(3u).known);
    notif_p(7u, 0u, 0x01u, 0x00u, 0x02);
    TEST_ASSERT_TRUE(info(3u).known);
    TEST_ASSERT_FALSE(info(3u).active);
}

/* ZW22 with zones.md §8 idle_after_s: the quirk's timer still ends a variable whatever event it
 * holds, as if the device had sent its idle; a stale idle doesn't stop that timer. */
static void test_stale_idle_and_the_quirk_timer(void)
{
    const zsrc_zw_desc_t d = ecolink(5u, 1u, 0x0002u);
    add(&d);
    up();
    bind(2u, addr(5u, 1u, 0u, ZSRC_ZW_SLOT_INTRUSION));
    clear();
    notif(5u, 0x07u, 0x01u);
    notif(5u, 0x07u, 0x02u); /* the variable holds 02; its timer runs from now */
    notif_p(5u, 0u, 0x07u, 0x00u, 0x01);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    advance(299999u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(2u).active);
    advance(1u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_FALSE(info(2u).active);
}

/* ZW22: each endpoint's variable holds its own event; an idle at one endpoint is judged by that
 * endpoint's event and ends nothing at another. */
static void test_stale_idle_per_endpoint(void)
{
    const zsrc_zw_desc_t d = two_smokes(9u, 1u);
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 1u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(9u, 1u, 2u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    notif_p(9u, 1u, 0x01u, 0x01u, -1);
    notif_p(9u, 2u, 0x01u, 0x02u, -1);
    EXPECT(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT(2u, ALARM_ZEV_FAULT, 1u);
    notif_p(9u, 2u, 0x01u, 0x00u, 0x01); /* endpoint 2 holds 02 */
    notif_p(9u, 1u, 0x01u, 0x00u, 0x02); /* endpoint 1 holds 01 */
    EXPECT(1u, ALARM_ZEV_RESTORE, 0u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    notif_p(9u, 1u, 0x01u, 0x00u, 0x01);
    EXPECT(1u, ALARM_ZEV_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_TRUE(info(2u).active);
    notif_p(9u, 2u, 0x01u, 0x00u, 0x02);
    EXPECT(2u, ALARM_ZEV_RESTORE, 1u);
}

/* ZW21, ZW-013: tamper is the device's, told on every watched input, but each endpoint owns its
 * own: one endpoint's restore leaves the device tampered while another's tamper is outstanding,
 * and the last one's restore is told exactly once to each zone - in either order, by Notification
 * (Home Security 03 and its idle) and by Binary Sensor type 08, repeated reports counting once. */
static void test_tamper_per_endpoint(void)
{
    const zsrc_zw_desc_t d = two_smokes(9u, 1u);
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 1u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(9u, 1u, 2u, ZSRC_ZW_SLOT_SMOKE));
    for (unsigned order = 0u; order < 2u; order++) {
        const uint8_t first = order == 0u ? 1u : 2u;
        const uint8_t last = order == 0u ? 2u : 1u;
        /* Notification */
        clear();
        notif_p(9u, 1u, 0x07u, 0x03u, -1);
        notif_p(9u, 2u, 0x07u, 0x03u, -1);
        notif_p(9u, 2u, 0x07u, 0x03u, -1); /* repeated */
        EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
        EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
        notif_p(9u, first, 0x07u, 0x00u, 0x03);
        notif_p(9u, first, 0x07u, 0x00u, 0x03); /* repeated */
        notif_p(9u, first, 0x07u, 0x00u, -1);
        EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
        EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
        notif_p(9u, last, 0x07u, 0x00u, 0x03);
        EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
        EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
        notif_p(9u, last, 0x07u, 0x00u, 0x03);
        EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
        /* Binary Sensor type 08 */
        clear();
        binary(9u, 1u, 0x08, true);
        binary(9u, 2u, 0x08, true);
        binary(9u, 1u, 0x08, true); /* repeated */
        EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
        EXPECT(2u, ALARM_ZEV_TAMPER, 1u);
        binary(9u, first, 0x08, false);
        binary(9u, first, 0x08, false);
        EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
        EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
        binary(9u, last, 0x08, false);
        EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
        EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    }
    /* the two families apart at one endpoint: Notification's idle leaves Binary Sensor's tamper */
    clear();
    binary(9u, 1u, 0x08, true);
    notif_p(9u, 1u, 0x07u, 0x03u, -1);
    notif_p(9u, 1u, 0x07u, 0x00u, 0x03);
    notif_p(9u, 1u, 0x07u, 0x00u, -1);
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    binary(9u, 1u, 0x08, false);
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    clear(); /* and the other way round */
    notif_p(9u, 2u, 0x07u, 0x03u, -1);
    binary(9u, 2u, 0x08, true);
    binary(9u, 2u, 0x08, false);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    notif_p(9u, 2u, 0x07u, 0x00u, 0x03);
    EXPECT(2u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    const zsrc_zw_device_t *dev = zsrc_zw_find(&Z, 9u);
    TEST_ASSERT_EQUAL_HEX8(0u, dev->tamper_src);
}

/* ZW21: low battery (Power Management) and maintenance trouble are kept per endpoint the same
 * way, and apart from tamper: none of them ends another. */
static void test_battery_and_trouble_per_endpoint(void)
{
    const zsrc_zw_desc_t d = two_smokes(9u, 1u);
    add(&d);
    up();
    bind(1u, addr(9u, 1u, 1u, ZSRC_ZW_SLOT_SMOKE));
    bind(2u, addr(9u, 1u, 2u, ZSRC_ZW_SLOT_SMOKE));
    clear();
    notif_p(9u, 1u, 0x08u, 0x0Au, -1);
    notif_p(9u, 2u, 0x08u, 0x0Bu, -1);
    notif_p(9u, 1u, 0x01u, 0x04u, -1);
    notif_p(9u, 2u, 0x01u, 0x05u, -1);
    notif_p(9u, 1u, 0x07u, 0x03u, -1);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE, 1u);
    EXPECT(1u, ALARM_ZEV_TAMPER, 1u);
    notif_p(9u, 1u, 0x07u, 0x00u, 0x03); /* tamper ends; battery and trouble stay */
    EXPECT(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(9u, 1u, 0x08u, 0x00u, 0x0A); /* endpoint 2's battery is still low */
    notif_p(9u, 1u, 0x01u, 0x00u, 0x04); /* endpoint 2's maintenance is still due */
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(9u, 2u, 0x08u, 0x00u, 0x0A); /* stale at endpoint 2 (it holds 0B) */
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    notif_p(9u, 2u, 0x08u, 0x00u, 0x0B);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 0u);
    notif_p(9u, 2u, 0x01u, 0x00u, -1); /* idle without a parameter: all of endpoint 2's smoke */
    EXPECT(1u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    EXPECT(2u, ALARM_ZEV_TROUBLE_RESTORE, 1u);
    const zsrc_zw_device_t *dev = zsrc_zw_find(&Z, 9u);
    TEST_ASSERT_EQUAL_HEX8(0u, dev->lowbat_src);
    TEST_ASSERT_EQUAL_HEX16(0u, dev->trouble_src);
    /* a Battery Report is the device's own: endpoint idles never end it */
    clear();
    battery(9u, 5u, 0u);
    notif_p(9u, 1u, 0x08u, 0x0Au, -1);
    notif_p(9u, 1u, 0x08u, 0x00u, 0x0A);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
    battery(9u, 90u, 0u);
    EXPECT(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_address_layout);
    RUN_TEST(test_generations);
    RUN_TEST(test_new_network);
    RUN_TEST(test_slots_offered);
    RUN_TEST(test_candidates_and_words);
    RUN_TEST(test_notification_states_and_idle);
    RUN_TEST(test_a_bounce_ends_on_the_last_report);
    RUN_TEST(test_tamper_and_battery_reach_every_input);
    RUN_TEST(test_product_moved);
    RUN_TEST(test_momentary_holds);
    RUN_TEST(test_central_scene_by_quirk);
    RUN_TEST(test_life_safety_rows);
    RUN_TEST(test_heat_rapid_rise);
    RUN_TEST(test_co_test_failed);
    RUN_TEST(test_maintenance_variables_apart);
    RUN_TEST(test_life_safety_maintenance_events);
    RUN_TEST(test_other_rows);
    RUN_TEST(test_binary_sensor_rows);
    RUN_TEST(test_battery_thresholds);
    RUN_TEST(test_battery_disconnected);
    RUN_TEST(test_binary_sensor_any_type);
    RUN_TEST(test_access_control_tilt_parameter);
    RUN_TEST(test_notification_status_fe_never_idles);
    RUN_TEST(test_multi_channel_endpoint_isolation);
    RUN_TEST(test_temperature);
    RUN_TEST(test_alarm_sensor_rows);
    RUN_TEST(test_alarm_sensor_water_and_general);
    RUN_TEST(test_v1_alarm_map_zcombo);
    RUN_TEST(test_v1_alarm_map_zsmoke);
    RUN_TEST(test_v1_alarm_supported_types);
    RUN_TEST(test_v1_alarm_dropped_without_a_map);
    RUN_TEST(test_basic);
    RUN_TEST(test_unoffered_is_ignored);
    RUN_TEST(test_checkins);
    RUN_TEST(test_reachability_and_grace);
    RUN_TEST(test_check_in_zero_not_supervised);
    RUN_TEST(test_trust_per_device);
    RUN_TEST(test_link_trouble);
    RUN_TEST(test_fresh_state);
    RUN_TEST(test_jam_keeps_reports);
    RUN_TEST(test_jam_and_a_real_link_down);
    RUN_TEST(test_unknown_device_watched);
    RUN_TEST(test_limits);
    RUN_TEST(test_quirks);
    RUN_TEST(test_idle_reset_any_variable);
    RUN_TEST(test_idle_reset_needs_the_quirk);
    RUN_TEST(test_idle_reset_motion_and_limits);
    RUN_TEST(test_picker_pulses_and_classes);
    RUN_TEST(test_hold_across_a_break);
    RUN_TEST(test_heat_hold_across_a_break);
    RUN_TEST(test_condition_across_a_break);
    RUN_TEST(test_stale_idle_keeps_the_newer_event);
    RUN_TEST(test_stale_idle_motion_and_conditions);
    RUN_TEST(test_stale_idle_no_idle_variables);
    RUN_TEST(test_stale_idle_and_the_quirk_timer);
    RUN_TEST(test_stale_idle_per_endpoint);
    RUN_TEST(test_tamper_per_endpoint);
    RUN_TEST(test_battery_and_trouble_per_endpoint);
    return UNITY_END();
}
