/*
 * test_zone_hub.c - zone sources (components/core/zone_hub, docs/features/22): the source registry,
 * bindings, mapping, grace timers and CHECK, source links, resync after a configuration change,
 * the binding blob, the input debouncer, the simulator, wired and Home Assistant sources; end to
 * end with the engine.
 * Requirements: ZSRC-*, ZONE-003, ZONE-004, ZONE-006, HAI-009 (docs/features/23 protocol.md)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "sim.h"
#include "zone_hub/zone_debounce.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_ha.h"
#include "zone_hub/zsrc_rf.h"
#include "zone_hub/zsrc_sim.h"
#include "zone_hub/zsrc_wired.h"
#include "zone_hub/zone_eol.h"

/* ---- harness: a recording port, a fake source, optionally the real engine ---------------- */

typedef struct {
    uint16_t zone;
    alarm_zone_event_t ev;
} rec_t;

static zone_hub_t H;
static rec_t R[512];
static unsigned NR;
static unsigned CHANGES;
static sim_t E;         /* the engine, when a test wires the hub to it */
static bool TO_ENGINE;
static uint64_t NOW;
static uint8_t BLOB[ZONE_HUB_BLOB_MAX + 64];

static void port_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    if (NR < sizeof(R) / sizeof(R[0])) {
        R[NR].zone = zone;
        R[NR].ev = ev;
        NR++;
    }
    if (TO_ENGINE) {
        alarm_engine_zone_event(&E.eng, zone, ev, E.now);
    }
}

static void port_changed(void *ctx)
{
    (void)ctx;
    CHANGES++;
}

/* ZSRC-015: the supervision trouble of a whole source, as the platform hears it. */
typedef struct {
    uint8_t kind;
    bool on;
} trouble_rec_t;

static trouble_rec_t TR[32];
static unsigned NT;

static void port_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    TEST_ASSERT_TRUE(NT < sizeof(TR) / sizeof(TR[0]));
    TR[NT].kind = kind;
    TR[NT].on = on;
    NT++;
}

/* How often the port was told about this source, and what it last heard (-1 = never). */
static unsigned trouble_says(uint8_t kind)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NT; i++) {
        n += TR[i].kind == kind;
    }
    return n;
}

static int trouble_last(uint8_t kind)
{
    int state = -1;
    for (unsigned i = 0; i < NT; i++) {
        if (TR[i].kind == kind) {
            state = TR[i].on ? 1 : 0;
        }
    }
    return state;
}

static bool source_in_trouble(uint8_t kind)
{
    zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
    const size_t n = zone_hub_sources(&H, list, ZONE_HUB_MAX_SOURCES);
    for (size_t i = 0; i < n; i++) {
        if (list[i].kind == kind) {
            return list[i].trouble;
        }
    }
    return false;
}

static unsigned count(uint16_t zone, alarm_zone_event_t ev)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NR; i++) {
        n += R[i].zone == zone && R[i].ev == ev;
    }
    return n;
}

static void clear_records(void)
{
    NR = 0;
    CHANGES = 0;
    NT = 0;
}

#define EXPECT_POSTED(z_, e_, n) TEST_ASSERT_EQUAL_UINT_MESSAGE((n), count((z_), (e_)), #e_)
#define EXPECT_LAST(z_, e_)                             \
    do {                                                \
        TEST_ASSERT_TRUE(NR > 0u);                      \
        TEST_ASSERT_EQUAL_UINT16((z_), R[NR - 1].zone); \
        TEST_ASSERT_EQUAL_INT((e_), R[NR - 1].ev);      \
    } while (0)

/* A source with terminals 1..FAKE_IDS whose levels the test sets (like the wired source). */
#define FAKE_IDS 200u
typedef struct {
    uint8_t kind;
    unsigned watches;
    unsigned unwatches;
    zsrc_mapping_t last_map;
    bool report_on_watch;
    int32_t level[FAKE_IDS + 1u];
    bool usable;
    bool learning;
    unsigned ticks;
    uint64_t next;
} fake_t;

static fake_t F;  /* kind ZSRC_WIRED */
static fake_t F2; /* kind ZSRC_HA */

static bool fake_valid(void *ctx, const zsrc_address_t *a)
{
    const fake_t *f = ctx;
    return a->kind == f->kind && ((a->id >= 1u && a->id <= FAKE_IDS) || a->ref[0] != '\0');
}

static bool fake_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    fake_t *f = ctx;
    if (!fake_valid(ctx, a)) {
        return false;
    }
    f->watches++;
    f->last_map = *m;
    if (f->report_on_watch && a->id >= 1u && a->id <= FAKE_IDS) {
        zone_hub_input(&H, a, NULL, f->level[a->id], now);
    }
    return true;
}

static void fake_unwatch(void *ctx, const zsrc_address_t *a)
{
    (void)a;
    ((fake_t *)ctx)->unwatches++;
}

static bool fake_usable(void *ctx)
{
    return ((fake_t *)ctx)->usable;
}

static bool fake_learn(void *ctx, bool on, uint64_t now)
{
    (void)now;
    ((fake_t *)ctx)->learning = on;
    return true;
}

static size_t fake_candidates(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                              size_t max)
{
    (void)query;
    (void)filter;
    const fake_t *f = ctx;
    size_t n = 0;
    for (uint64_t id = 1; id <= 4u && n < max; id++) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].addr = zsrc_address(f->kind, 0, id);
        snprintf(out[n].name, sizeof(out[n].name), "T%u", (unsigned)id);
        n++;
    }
    return n;
}

static void fake_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    (void)ctx;
    snprintf(out, len, "terminal %u", (unsigned)a->id);
}

static void fake_tick(void *ctx, uint64_t now)
{
    (void)now;
    ((fake_t *)ctx)->ticks++;
}

static uint64_t fake_next(void *ctx, uint64_t now)
{
    (void)now;
    return ((fake_t *)ctx)->next;
}

static const zone_source_ops_t FAKE_OPS = {
    .kind = ZSRC_WIRED,
    .caps = ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_AUTH | ZSRC_CAP_LIFE_SAFETY | ZSRC_CAP_LEARN,
    .name = "Fake wired",
    .grace_s = 0,
    .usable = fake_usable,
    .valid = fake_valid,
    .watch = fake_watch,
    .unwatch = fake_unwatch,
    .learn = fake_learn,
    .candidates = fake_candidates,
    .describe = fake_describe,
    .tick = fake_tick,
    .next_tick = fake_next,
};

static const zone_source_ops_t FAKE_HA_OPS = {
    .kind = ZSRC_HA,
    .caps = ZSRC_CAP_SEARCH | ZSRC_CAP_SUPERVISED | ZSRC_CAP_AUTH | ZSRC_CAP_BATTERY,
    .name = "Fake Home Assistant",
    .grace_s = 120,
    .usable = fake_usable,
    .valid = fake_valid,
    .watch = fake_watch,
    .unwatch = fake_unwatch,
    .describe = fake_describe,
};

static zsrc_sim_t SIMSRC;

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    zone_hub_event(&H, a, ev, now);
}

static void hub_setup(bool start)
{
    memset(&F, 0, sizeof(F));
    memset(&F2, 0, sizeof(F2));
    F.kind = ZSRC_WIRED;
    F.usable = true;
    F.next = UINT64_MAX;
    F2.kind = ZSRC_HA;
    F2.usable = true;
    F2.next = UINT64_MAX;
    const zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed,
                                  .source_trouble = port_trouble};
    zone_hub_init(&H, &port);
    const zsrc_sink_t sink = {.ctx = NULL, .event = sink_event};
    zsrc_sim_init(&SIMSRC, &sink);
    NOW = 1000000u;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_OPS, &F, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_sim_ops(), &SIMSRC, NOW));
    if (start) {
        zone_hub_start(&H, NOW);
    }
    clear_records();
}

static zone_hub_binding_t main_only(uint8_t kind, uint64_t id)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_address(kind, 0, id);
    return b;
}

static zone_hub_binding_t ha_binding(const char *entity)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_address(ZSRC_HA, 0, 0);
    snprintf(b.in[ZSRC_ROLE_MAIN].addr.ref, ZSRC_REF_LEN, "%s", entity);
    return b;
}

static void advance(uint64_t ms)
{
    NOW += ms;
    zone_hub_tick(&H, NOW);
}

void setUp(void)
{
    TO_ENGINE = false;
    hub_setup(true);
}

void tearDown(void) {}

/* ---- ZSRC-002: sources are interfaces in a registry ------------------------------------------ */

/* ZSRC-002: several sources at once, one per kind; the list says what each can do and whether
 * zones can be added with it now (ZSRC-005). */
static void test_registry(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_register(&H, &FAKE_OPS, &F, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    F2.usable = false;
    zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
    const size_t n = zone_hub_sources(&H, list, ZONE_HUB_MAX_SOURCES);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_WIRED, list[0].kind);
    TEST_ASSERT_TRUE(list[0].usable);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_SIM, list[1].kind);
    TEST_ASSERT_TRUE((list[1].caps & ZSRC_CAP_TEST) != 0u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_HA, list[2].kind);
    TEST_ASSERT_FALSE(list[2].usable);
    TEST_ASSERT_EQUAL_PTR(&FAKE_HA_OPS, zone_hub_source_ops(&H, ZSRC_HA));
    TEST_ASSERT_NULL(zone_hub_source_ops(&H, ZSRC_ZIGBEE));
    zone_source_ops_t more[ZONE_HUB_MAX_SOURCES];
    size_t added = 0;
    zone_hub_result_t r = ZONE_HUB_OK;
    for (uint8_t k = 10; r == ZONE_HUB_OK; k++) {
        more[added] = FAKE_OPS;
        more[added].kind = k;
        r = zone_hub_register(&H, &more[added], &F, NOW);
        added += r == ZONE_HUB_OK;
    }
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, r);
    TEST_ASSERT_EQUAL_UINT(ZONE_HUB_MAX_SOURCES - 3u, added);
}

/* ---- ZSRC-003 / ZONE-003: one binding per zone, one zone per sensor --------------------------- */

static void test_one_binding_per_zone_one_zone_per_sensor(void)
{
    zone_hub_binding_t b = main_only(ZSRC_WIRED, 4);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &b, NOW));
    TEST_ASSERT_EQUAL_UINT(1, CHANGES);
    TEST_ASSERT_EQUAL_UINT(1, F.watches);
    TEST_ASSERT_EQUAL_UINT16(3, zone_hub_zone_of(&H, &b.in[0].addr));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_IN_USE, zone_hub_bind(&H, 4, &b, NOW));

    zone_hub_binding_t twice = main_only(ZSRC_WIRED, 5);
    twice.in[ZSRC_ROLE_TAMPER] = twice.in[ZSRC_ROLE_MAIN];
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_bind(&H, 5, &twice, NOW));
    zone_hub_binding_t none;
    memset(&none, 0, sizeof(none));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_bind(&H, 5, &none, NOW));
    zone_hub_binding_t zb = main_only(ZSRC_ZIGBEE, 0x1234);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_NO_SOURCE, zone_hub_bind(&H, 5, &zb, NOW));
    zone_hub_binding_t bad = main_only(ZSRC_WIRED, FAKE_IDS + 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, 5, &bad, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_bind(&H, 0, &b, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_bind(&H, ALARM_MAX_ZONES + 1u, &b, NOW));
    TEST_ASSERT_EQUAL_UINT(1, CHANGES); /* refusals change nothing */

    /* Rebinding replaces: the old sensor is let go and what it had said is undone. */
    zone_hub_input(&H, &b.in[0].addr, NULL, 1, NOW);
    EXPECT_LAST(3, ALARM_ZEV_FAULT);
    const zone_hub_binding_t other = main_only(ZSRC_WIRED, 7);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &other, NOW));
    EXPECT_POSTED(3, ALARM_ZEV_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT(1, F.unwatches);
    TEST_ASSERT_EQUAL_UINT16(0, zone_hub_zone_of(&H, &b.in[0].addr));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 4, &b, NOW)); /* free again */
    zone_hub_binding_t got;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 3, &got));
    TEST_ASSERT_EQUAL_UINT64(7, got.in[ZSRC_ROLE_MAIN].addr.id);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_NONE, got.in[ZSRC_ROLE_BATTERY].addr.kind);

    zone_hub_unbind(&H, 3, NOW);
    TEST_ASSERT_FALSE(zone_hub_get(&H, 3, &got));
    TEST_ASSERT_EQUAL_UINT(2, F.unwatches);
}

/* ZSRC-006: changing what a sensor's state means (a contact from normally closed to normally
 * open, a new threshold) turns the state it last reported into the right zone event at once,
 * without waiting for the sensor to move. */
static void test_rebind_new_mapping_remaps_state(void)
{
    zone_hub_binding_t b = main_only(ZSRC_WIRED, 4);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &b, NOW));
    zone_hub_input(&H, &b.in[0].addr, NULL, 1, NOW); /* the loop is open */
    EXPECT_LAST(3, ALARM_ZEV_FAULT);
    clear_records();
    b.in[0].map.invert = true; /* ... which a normally open contact calls closed */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &b, NOW));
    EXPECT_LAST(3, ALARM_ZEV_RESTORE);
    EXPECT_POSTED(3, ALARM_ZEV_RESTORE, 1);
    b.in[0].map.invert = false;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &b, NOW));
    EXPECT_LAST(3, ALARM_ZEV_FAULT);

    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t t = ha_binding("sensor.cellar_temperature");
    zsrc_mapping_defaults(&t.in[0].map, ALARM_DEVCLASS_TEMPERATURE); /* below 5 °C */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 8, &t, NOW));
    zone_hub_input(&H, &t.in[0].addr, "7.5", 0, NOW);
    EXPECT_LAST(8, ALARM_ZEV_RESTORE);
    t.in[0].map.threshold_x100 = 1000; /* warn below 10 °C from now on */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 8, &t, NOW));
    EXPECT_LAST(8, ALARM_ZEV_FAULT);
}

/* ZSRC-010: when the stored bindings can't be read at all, the zones that had one show CHECK
 * instead of looking ready while nothing can ever fault them; picking a sensor again clears it. */
static void test_lost_bindings_are_check(void)
{
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    zones[0] = 0x05u; /* zones 1 and 3 */
    zone_hub_lost(&H, zones, NOW);
    EXPECT_POSTED(1, ALARM_ZEV_UNREACHABLE, 1);
    EXPECT_POSTED(3, ALARM_ZEV_UNREACHABLE, 1);
    EXPECT_POSTED(2, ALARM_ZEV_UNREACHABLE, 0);
    zone_hub_lost(&H, zones, NOW); /* again: nothing new */
    EXPECT_POSTED(1, ALARM_ZEV_UNREACHABLE, 1);
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 1, &info));
    TEST_ASSERT_TRUE(info.lost && info.check);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_NONE, info.binding.in[ZSRC_ROLE_MAIN].addr.kind);
    advance(600000);
    zone_hub_resync(&H, zones, NOW); /* a configuration change doesn't lose the CHECK */
    EXPECT_POSTED(1, ALARM_ZEV_UNREACHABLE, 2);

    const zone_hub_binding_t b = main_only(ZSRC_WIRED, 4);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &b, NOW));
    EXPECT_POSTED(1, ALARM_ZEV_REACHABLE, 1);
    TEST_ASSERT_TRUE(zone_hub_info(&H, 1, &info));
    TEST_ASSERT_FALSE(info.lost);
    zone_hub_unbind(&H, 3, NOW); /* deliberately without a sensor: no CHECK either */
    EXPECT_POSTED(3, ALARM_ZEV_REACHABLE, 1);
    TEST_ASSERT_FALSE(zone_hub_info(&H, 3, &info));
}

/* ZSRC-003: the same sensor with a new response time keeps its state (no blip in the zone). */
static void test_rebind_same_sensor_keeps_state(void)
{
    zone_hub_binding_t b = main_only(ZSRC_WIRED, 4);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &b, NOW));
    zone_hub_input(&H, &b.in[0].addr, NULL, 1, NOW);
    clear_records();
    b.in[0].map.response_ms = 1200;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 3, &b, NOW));
    TEST_ASSERT_EQUAL_UINT(0, NR);
    TEST_ASSERT_EQUAL_UINT(2, F.watches); /* the source got the new response time */
    TEST_ASSERT_EQUAL_UINT16(1200, F.last_map.response_ms);
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 3, &info));
    TEST_ASSERT_TRUE(info.known && info.active);
    TEST_ASSERT_EQUAL_STRING("terminal 4", info.address);
    TEST_ASSERT_TRUE(info.source_present);
}

/* CORE-CFG-021, voice-first-build.md D07: only a different sensor (any role's address, a role
 * added or removed, unbinding) counts as a new sensor; a new mapping on the same one does not. */
static void test_same_sensors(void)
{
    zone_hub_binding_t a = main_only(ZSRC_WIRED, 4);
    zone_hub_binding_t b = a;
    TEST_ASSERT_TRUE(zone_hub_same_sensors(&a, &b));
    b.in[0].map.response_ms = 1200; /* same sensor, new mapping */
    b.in[0].map.invert = true;
    b.in[ZSRC_ROLE_TAMPER].map.grace_s = 30; /* an unused role's leftovers don't count */
    TEST_ASSERT_TRUE(zone_hub_same_sensors(&a, &b));
    b = main_only(ZSRC_WIRED, 5); /* another terminal */
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&a, &b));
    b = a;
    b.in[0].addr.input = 2; /* another input on the same device */
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&a, &b));
    b = a;
    b.in[0].addr.kind = ZSRC_SIM; /* another source with the same number */
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&a, &b));
    b = a;
    b.in[ZSRC_ROLE_BATTERY].addr = zsrc_address(ZSRC_WIRED, 0, 6); /* a role added */
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&a, &b));
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&b, &a)); /* and removed */
    zone_hub_binding_t ha1, ha2;
    memset(&ha1, 0, sizeof(ha1));
    ha1.in[0].addr.kind = ZSRC_HA;
    strcpy(ha1.in[0].addr.ref, "binary_sensor.front_door");
    ha2 = ha1;
    TEST_ASSERT_TRUE(zone_hub_same_sensors(&ha1, &ha2));
    strcpy(ha2.in[0].addr.ref, "binary_sensor.back_door");
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&ha1, &ha2));
    /* Unbinding (NULL) is a change; no sensor before and after is not. */
    zone_hub_binding_t none;
    memset(&none, 0, sizeof(none));
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&a, NULL));
    TEST_ASSERT_FALSE(zone_hub_same_sensors(NULL, &a));
    TEST_ASSERT_FALSE(zone_hub_same_sensors(&a, &none));
    TEST_ASSERT_TRUE(zone_hub_same_sensors(&none, NULL));
    TEST_ASSERT_TRUE(zone_hub_same_sensors(NULL, NULL));
    /* A tamper input without a main one is no sensor (zone_hub_bind refuses it). */
    none.in[ZSRC_ROLE_TAMPER].addr = zsrc_address(ZSRC_WIRED, 0, 7);
    TEST_ASSERT_TRUE(zone_hub_same_sensors(&none, NULL));
}

/* ---- ZSRC-006: mapping --------------------------------------------------------------------------- */

/* ZSRC-006: binary inputs, by value or by word, inverted for normally open contacts; the first
 * report always counts; words the mapping doesn't know change nothing. */
static void test_binary_mapping(void)
{
    zone_hub_binding_t b = main_only(ZSRC_WIRED, 4);
    zone_hub_bind(&H, 3, &b, NOW);
    const zsrc_address_t *a = &b.in[0].addr;
    zone_hub_input(&H, a, NULL, 0, NOW);
    EXPECT_LAST(3, ALARM_ZEV_RESTORE); /* first report */
    zone_hub_input(&H, a, NULL, 1, NOW);
    EXPECT_LAST(3, ALARM_ZEV_FAULT);
    const unsigned before = NR;
    zone_hub_input(&H, a, "on", 0, NOW);
    zone_hub_input(&H, a, "Open", 0, NOW);
    TEST_ASSERT_EQUAL_UINT(before, NR); /* unchanged: nothing (check-ins are rate-limited) */
    zone_hub_input(&H, a, " closed ", 0, NOW);
    EXPECT_LAST(3, ALARM_ZEV_RESTORE);
    zone_hub_input(&H, a, "unavailable", 0, NOW);
    zone_hub_input(&H, a, "maybe", 1, NOW);
    EXPECT_LAST(3, ALARM_ZEV_RESTORE);

    b.in[0].map.invert = true; /* a normally open contact */
    zone_hub_bind(&H, 3, &b, NOW);
    zone_hub_input(&H, a, NULL, 0, NOW);
    EXPECT_LAST(3, ALARM_ZEV_FAULT);
    zone_hub_input(&H, a, "true", 0, NOW);
    EXPECT_LAST(3, ALARM_ZEV_RESTORE);
}

/* ZSRC-006: thresholds with hysteresis, values as numbers or as text ("4.5"). */
static void test_threshold_mapping(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t b = ha_binding("sensor.garage_temperature");
    zsrc_mapping_defaults(&b.in[0].map, ALARM_DEVCLASS_TEMPERATURE); /* below 5 °C, 1 °C */
    TEST_ASSERT_EQUAL_INT(ZSRC_MAP_BELOW, b.in[0].map.mode);
    TEST_ASSERT_EQUAL_INT32(500, b.in[0].map.threshold_x100);
    TEST_ASSERT_EQUAL_INT32(100, b.in[0].map.hysteresis_x100);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 12, &b, NOW));
    const zsrc_address_t *a = &b.in[0].addr;
    zone_hub_input(&H, a, "12.5", 0, NOW);
    EXPECT_LAST(12, ALARM_ZEV_RESTORE);
    zone_hub_input(&H, a, "5.01", 0, NOW);
    EXPECT_POSTED(12, ALARM_ZEV_FAULT, 0);
    zone_hub_input(&H, a, NULL, 500, NOW); /* 5.00 °C: at the threshold */
    EXPECT_LAST(12, ALARM_ZEV_FAULT);
    zone_hub_input(&H, a, "5.9", 0, NOW); /* inside the hysteresis: still cold */
    EXPECT_LAST(12, ALARM_ZEV_FAULT);
    zone_hub_input(&H, a, "unknown", 0, NOW); /* says nothing */
    zone_hub_input(&H, a, "6.01", 0, NOW);
    EXPECT_LAST(12, ALARM_ZEV_RESTORE);
    zone_hub_input(&H, a, "-3", 0, NOW);
    EXPECT_LAST(12, ALARM_ZEV_FAULT);
    zone_hub_info_t info;
    zone_hub_info(&H, 12, &info);
    TEST_ASSERT_TRUE(info.has_value);
    TEST_ASSERT_EQUAL_INT32(-300, info.value_x100);

    zone_hub_binding_t hot = ha_binding("sensor.attic_temperature");
    hot.in[0].map.mode = ZSRC_MAP_ABOVE;
    hot.in[0].map.threshold_x100 = 4000;
    hot.in[0].map.hysteresis_x100 = 250;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 13, &hot, NOW));
    const zsrc_address_t *h = &hot.in[0].addr;
    zone_hub_input(&H, h, "40", 0, NOW);
    EXPECT_LAST(13, ALARM_ZEV_FAULT);
    zone_hub_input(&H, h, "37.6", 0, NOW);
    EXPECT_LAST(13, ALARM_ZEV_FAULT);
    zone_hub_input(&H, h, "37.49", 0, NOW);
    EXPECT_LAST(13, ALARM_ZEV_RESTORE);
}

/* ZSRC-006: a state list ("open,unlocked"), case and spaces ignored. */
static void test_state_list_mapping(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t b = ha_binding("lock.front_door");
    b.in[0].map.mode = ZSRC_MAP_STATES;
    snprintf(b.in[0].map.states, ZSRC_STATES_LEN, "%s", "unlocked, open ,jammed");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 20, &b, NOW));
    const zsrc_address_t *a = &b.in[0].addr;
    zone_hub_input(&H, a, "locked", 0, NOW);
    EXPECT_LAST(20, ALARM_ZEV_RESTORE);
    zone_hub_input(&H, a, "Unlocked", 0, NOW);
    EXPECT_LAST(20, ALARM_ZEV_FAULT);
    zone_hub_input(&H, a, "open", 0, NOW);
    zone_hub_input(&H, a, "jammed", 0, NOW);
    EXPECT_POSTED(20, ALARM_ZEV_FAULT, 1);
    zone_hub_input(&H, a, "locking", 0, NOW);
    EXPECT_LAST(20, ALARM_ZEV_RESTORE);
    zone_hub_input(&H, a, NULL, 1, NOW); /* no state: nothing to compare */
    EXPECT_LAST(20, ALARM_ZEV_RESTORE);
}

/* ZSRC-006: linked tamper and battery inputs become the zone's tamper and low battery; an
 * unchanged report counts as a check-in at most once a minute. */
static void test_linked_inputs_and_checkins(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t b = ha_binding("binary_sensor.back_door");
    b.in[ZSRC_ROLE_TAMPER].addr = zsrc_address(ZSRC_HA, 0, 0);
    snprintf(b.in[ZSRC_ROLE_TAMPER].addr.ref, ZSRC_REF_LEN, "binary_sensor.back_door_tamper");
    b.in[ZSRC_ROLE_BATTERY].addr = zsrc_address(ZSRC_HA, 0, 0);
    snprintf(b.in[ZSRC_ROLE_BATTERY].addr.ref, ZSRC_REF_LEN, "sensor.back_door_battery");
    b.in[ZSRC_ROLE_BATTERY].map.mode = ZSRC_MAP_BELOW;
    b.in[ZSRC_ROLE_BATTERY].map.threshold_x100 = 2000;
    b.in[ZSRC_ROLE_BATTERY].map.hysteresis_x100 = 500;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 7, &b, NOW));
    zone_hub_input(&H, &b.in[ZSRC_ROLE_TAMPER].addr, "on", 0, NOW);
    EXPECT_LAST(7, ALARM_ZEV_TAMPER);
    zone_hub_input(&H, &b.in[ZSRC_ROLE_TAMPER].addr, "off", 0, NOW);
    EXPECT_LAST(7, ALARM_ZEV_TAMPER_RESTORE);
    zone_hub_input(&H, &b.in[ZSRC_ROLE_BATTERY].addr, "19", 0, NOW);
    EXPECT_LAST(7, ALARM_ZEV_LOW_BATTERY);
    zone_hub_input(&H, &b.in[ZSRC_ROLE_BATTERY].addr, "26", 0, NOW);
    EXPECT_LAST(7, ALARM_ZEV_LOW_BATTERY_RESTORE);
    zone_hub_input(&H, &b.in[ZSRC_ROLE_MAIN].addr, "off", 0, NOW);
    EXPECT_LAST(7, ALARM_ZEV_RESTORE);

    clear_records();
    zone_hub_input(&H, &b.in[ZSRC_ROLE_MAIN].addr, "off", 0, NOW + 30000u);
    TEST_ASSERT_EQUAL_UINT(0, NR);
    zone_hub_input(&H, &b.in[ZSRC_ROLE_MAIN].addr, "off", 0, NOW + 60000u);
    EXPECT_POSTED(7, ALARM_ZEV_CHECKIN, 1);
    zone_hub_input(&H, &b.in[ZSRC_ROLE_MAIN].addr, "off", 0, NOW + 90000u);
    EXPECT_POSTED(7, ALARM_ZEV_CHECKIN, 1);

    zone_hub_unbind(&H, 7, NOW); /* undoes nothing it didn't say: all were restored */
    EXPECT_POSTED(7, ALARM_ZEV_RESTORE, 0);
    EXPECT_POSTED(7, ALARM_ZEV_TAMPER_RESTORE, 0);
}

/* ZSRC-006, WALK-007, ZB-022: a sensor's own test reaches the engine as ALARM_ZEV_TEST (a walk-test
 * mark, a check-in otherwise), from the zone's own sensor only, never from its tamper input. */
static void test_sensor_test_passes_through(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t b = ha_binding("binary_sensor.smoke");
    b.in[ZSRC_ROLE_TAMPER].addr = zsrc_address(ZSRC_HA, 0, 0);
    snprintf(b.in[ZSRC_ROLE_TAMPER].addr.ref, ZSRC_REF_LEN, "binary_sensor.smoke_tamper");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 7, &b, NOW));
    clear_records();
    TEST_ASSERT_TRUE(zone_hub_event(&H, &b.in[ZSRC_ROLE_MAIN].addr, ALARM_ZEV_TEST, NOW));
    EXPECT_LAST(7, ALARM_ZEV_TEST);
    EXPECT_POSTED(7, ALARM_ZEV_CHECKIN, 0);
    clear_records();
    zone_hub_event(&H, &b.in[ZSRC_ROLE_TAMPER].addr, ALARM_ZEV_TEST, NOW);
    TEST_ASSERT_EQUAL_UINT(0, NR);
}

/* ZSRC-006: defaults from the sensor kind; ZSRC-008: the zone type each kind suggests, and
 * which sources may feed life-safety zones without an installer override. */
static void test_defaults_and_trust(void)
{
    zsrc_mapping_t m;
    zsrc_mapping_defaults(&m, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZSRC_MAP_BINARY, m.mode);
    TEST_ASSERT_FALSE(m.invert);
    TEST_ASSERT_EQUAL_UINT16(0, m.grace_s);
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_PERIMETER, zsrc_suggest_zone_type(ALARM_DEVCLASS_DOOR));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_PERIMETER, zsrc_suggest_zone_type(ALARM_DEVCLASS_WINDOW));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_INTERIOR_FOLLOWER, zsrc_suggest_zone_type(ALARM_DEVCLASS_MOTION));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_FIRE, zsrc_suggest_zone_type(ALARM_DEVCLASS_SMOKE));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_CO, zsrc_suggest_zone_type(ALARM_DEVCLASS_CO));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_24H_AUX, zsrc_suggest_zone_type(ALARM_DEVCLASS_WATER));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_24H_AUX, zsrc_suggest_zone_type(ALARM_DEVCLASS_TEMPERATURE));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_24H_AUDIBLE, zsrc_suggest_zone_type(ALARM_DEVCLASS_PANIC_BUTTON));

    const uint32_t wired = ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_AUTH | ZSRC_CAP_LIFE_SAFETY;
    const uint32_t ha = ZSRC_CAP_SEARCH | ZSRC_CAP_SUPERVISED | ZSRC_CAP_AUTH;
    const alarm_zone_type_t life[] = {ALARM_ZT_FIRE, ALARM_ZT_FIRE_VERIFIED, ALARM_ZT_CO,
                                      ALARM_ZT_24H_SILENT, ALARM_ZT_24H_AUDIBLE};
    for (size_t i = 0; i < sizeof(life) / sizeof(life[0]); i++) {
        TEST_ASSERT_TRUE(zsrc_zone_type_life_safety(life[i]));
        TEST_ASSERT_TRUE(zsrc_trusted_for(wired, life[i]));
        TEST_ASSERT_FALSE(zsrc_trusted_for(ha, life[i]));
    }
    TEST_ASSERT_TRUE(zsrc_trusted_for(ha, ALARM_ZT_PERIMETER));
    TEST_ASSERT_TRUE(zsrc_trusted_for(ha, ALARM_ZT_24H_AUX));
    TEST_ASSERT_TRUE(zsrc_trusted_for(zsrc_sim_ops()->caps, ALARM_ZT_FIRE));
}

/* ---- ZSRC-007: unreachable sensors, grace, links ------------------------------------------- */

/* ZSRC-007: an unreachable sensor becomes CHECK after the grace (binding, else source, else hub
 * default) and clears when it answers; a report proves it is there. */
static void test_grace_then_check(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t w = main_only(ZSRC_WIRED, 4);
    zone_hub_binding_t ha = ha_binding("binary_sensor.shed");
    zone_hub_binding_t own = ha_binding("binary_sensor.gate");
    own.in[0].map.grace_s = 10;
    zone_hub_bind(&H, 1, &w, NOW);
    zone_hub_bind(&H, 2, &ha, NOW);
    zone_hub_bind(&H, 3, &own, NOW);
    clear_records();
    zone_hub_reachable(&H, &w.in[0].addr, false, NOW);
    zone_hub_reachable(&H, &ha.in[0].addr, false, NOW);
    zone_hub_reachable(&H, &own.in[0].addr, false, NOW);
    TEST_ASSERT_EQUAL_UINT64(NOW + 10000u, zone_hub_next_deadline(&H, NOW));
    advance(9999);
    TEST_ASSERT_EQUAL_UINT(0, NR);
    advance(1);
    EXPECT_POSTED(3, ALARM_ZEV_UNREACHABLE, 1); /* its own 10 s */
    TEST_ASSERT_EQUAL_UINT64(NOW + 50000u, zone_hub_next_deadline(&H, NOW));
    advance(50000);
    EXPECT_POSTED(1, ALARM_ZEV_UNREACHABLE, 1); /* hub default: 60 s */
    EXPECT_POSTED(2, ALARM_ZEV_UNREACHABLE, 0);
    advance(60000);
    EXPECT_POSTED(2, ALARM_ZEV_UNREACHABLE, 1); /* the source's 120 s */
    advance(600000);
    TEST_ASSERT_EQUAL_UINT(3, NR); /* once each */
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_hub_next_deadline(&H, NOW));
    zone_hub_info_t info;
    zone_hub_info(&H, 1, &info);
    TEST_ASSERT_TRUE(info.unreachable && info.check);

    zone_hub_input(&H, &w.in[0].addr, NULL, 0, NOW); /* it speaks: back, then its state */
    TEST_ASSERT_EQUAL_INT(ALARM_ZEV_REACHABLE, R[NR - 2].ev);
    EXPECT_LAST(1, ALARM_ZEV_RESTORE);
    zone_hub_reachable(&H, &ha.in[0].addr, true, NOW);
    EXPECT_LAST(2, ALARM_ZEV_REACHABLE);
    zone_hub_event(&H, &own.in[0].addr, ALARM_ZEV_REACHABLE, NOW);
    EXPECT_LAST(3, ALARM_ZEV_REACHABLE);

    clear_records(); /* back within the grace: nothing reaches the engine */
    zone_hub_reachable(&H, &w.in[0].addr, false, NOW);
    advance(30000);
    zone_hub_reachable(&H, &w.in[0].addr, true, NOW);
    advance(60000);
    TEST_ASSERT_EQUAL_UINT(0, NR);

    /* A zone whose sensor is taken away doesn't keep its CHECK. */
    zone_hub_reachable(&H, &ha.in[0].addr, false, NOW);
    advance(120000);
    EXPECT_POSTED(2, ALARM_ZEV_UNREACHABLE, 1);
    zone_hub_unbind(&H, 2, NOW);
    EXPECT_POSTED(2, ALARM_ZEV_REACHABLE, 1);
    advance(600000);
    EXPECT_POSTED(2, ALARM_ZEV_UNREACHABLE, 1);
}

/* ZSRC-007: a zone shows CHECK while any of its inputs is unreachable; one answering input
 * doesn't clear another's CHECK. */
static void test_check_per_zone(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t b = ha_binding("binary_sensor.porch");
    b.in[ZSRC_ROLE_BATTERY].addr = zsrc_address(ZSRC_HA, 0, 0);
    snprintf(b.in[ZSRC_ROLE_BATTERY].addr.ref, ZSRC_REF_LEN, "sensor.porch_battery");
    b.in[ZSRC_ROLE_BATTERY].map.mode = ZSRC_MAP_BELOW;
    b.in[ZSRC_ROLE_BATTERY].map.threshold_x100 = 1500;
    zone_hub_bind(&H, 4, &b, NOW);
    const zsrc_address_t *m = &b.in[ZSRC_ROLE_MAIN].addr;
    const zsrc_address_t *bat = &b.in[ZSRC_ROLE_BATTERY].addr;
    zone_hub_reachable(&H, m, false, NOW);
    zone_hub_reachable(&H, bat, false, NOW);
    advance(120000);
    EXPECT_POSTED(4, ALARM_ZEV_UNREACHABLE, 1);
    zone_hub_input(&H, bat, "80", 0, NOW); /* the battery entity is back, the sensor isn't */
    EXPECT_POSTED(4, ALARM_ZEV_REACHABLE, 0);
    zone_hub_input(&H, m, "off", 0, NOW);
    EXPECT_POSTED(4, ALARM_ZEV_REACHABLE, 1);

    /* End to end: the engine keeps the CHECK while another input of the zone reports. */
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    zone_hub_binding_t w = main_only(ZSRC_WIRED, 9);
    w.in[ZSRC_ROLE_BATTERY].addr = zsrc_address(ZSRC_WIRED, 0, 10);
    zone_hub_bind(&H, Z_WINDOW, &w, NOW);
    zone_hub_reachable(&H, &w.in[ZSRC_ROLE_MAIN].addr, false, NOW);
    advance(60000);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    zone_hub_input(&H, &w.in[ZSRC_ROLE_BATTERY].addr, NULL, 1, NOW);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_LOW_BATTERY);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    TEST_ASSERT_FALSE(sim_status(&E, 1).ready);
    zone_hub_input(&H, &w.in[ZSRC_ROLE_MAIN].addr, NULL, 0, NOW);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    TEST_ASSERT_TRUE(sim_status(&E, 1).ready);
}

/* ZSRC-007 / ZONE-008: while a source's link is down its sensors don't turn CHECK (the link is
 * the source's own trouble, ZSRC-015); with the link back every sensor has the grace to report
 * again. */
static void test_link_down_is_not_check(void)
{
    zone_hub_binding_t b = main_only(ZSRC_WIRED, 4);
    zone_hub_bind(&H, 1, &b, NOW);
    clear_records();
    zone_hub_link(&H, ZSRC_WIRED, false, NOW);
    zone_hub_reachable(&H, &b.in[0].addr, false, NOW);
    /* the only deadline is the source's own (ZSRC-015), not the sensor's grace */
    TEST_ASSERT_EQUAL_UINT64(NOW + 60000u, zone_hub_next_deadline(&H, NOW));
    advance(3600000);
    TEST_ASSERT_EQUAL_UINT(0, NR); /* no zone hears about it */
    TEST_ASSERT_EQUAL_UINT(1, trouble_says(ZSRC_WIRED));
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_WIRED));
    zone_hub_source_info_t list[4];
    zone_hub_sources(&H, list, 4);
    TEST_ASSERT_TRUE(list[0].link_down);
    TEST_ASSERT_TRUE(list[0].trouble);
    zone_hub_link(&H, ZSRC_WIRED, true, NOW);
    advance(59999);
    TEST_ASSERT_EQUAL_UINT(0, NR);
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_WIRED)); /* nothing has proved itself yet */
    advance(1);
    EXPECT_POSTED(1, ALARM_ZEV_UNREACHABLE, 1);
    TEST_ASSERT_EQUAL_INT(0, trouble_last(ZSRC_WIRED)); /* the zone's CHECK says it now */
    zone_hub_link(&H, ZSRC_WIRED, false, NOW); /* a CHECK already raised stays */
    advance(1000);
    EXPECT_POSTED(1, ALARM_ZEV_REACHABLE, 0);
}

/* ---- ZSRC-010: a source that isn't there --------------------------------------------------- */

/* ZSRC-010: bindings of a source this firmware lacks (compiled out, or written by a newer
 * firmware) are kept; their zones show CHECK after the grace; when the source registers, its
 * sensors are watched again and the CHECK clears once each reports. */
static void test_missing_source_is_check_and_kept(void)
{
    hub_setup(false);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t b = ha_binding("binary_sensor.side_door");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 6, &b, NOW));
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB, sizeof(BLOB), &len));

    hub_setup(false); /* a firmware without the Home Assistant source */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, BLOB, len));
    zone_hub_start(&H, NOW);
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 6, &info));
    TEST_ASSERT_FALSE(info.source_present);
    TEST_ASSERT_TRUE(info.unreachable);
    advance(59999);
    TEST_ASSERT_EQUAL_UINT(0, NR);
    advance(1);
    EXPECT_POSTED(6, ALARM_ZEV_UNREACHABLE, 1);
    size_t len2 = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB + len, sizeof(BLOB) - len, &len2));
    TEST_ASSERT_EQUAL_UINT(len, len2);
    TEST_ASSERT_EQUAL_MEMORY(BLOB, BLOB + len, len); /* kept exactly */

    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    TEST_ASSERT_EQUAL_UINT(1, F2.watches);
    advance(600000);
    EXPECT_POSTED(6, ALARM_ZEV_REACHABLE, 0); /* until the sensor itself reports */
    zone_hub_input(&H, &b.in[0].addr, "off", 0, NOW);
    EXPECT_POSTED(6, ALARM_ZEV_REACHABLE, 1);
    zone_hub_info(&H, 6, &info);
    TEST_ASSERT_TRUE(info.source_present && !info.check);
}

/* ---- resync after a configuration change -------------------------------------------------- */

/* ZSRC-001 / CORE-CFG-018: when a configuration change makes the engine forget zones (a new
 * zone, a retyped one), the hub tells it again what their sensors say; other zones hear
 * nothing. End to end: a zone added after its sensor was bound gets the sensor's state. */
static void test_resync_after_config_change(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    zone_hub_binding_t w = main_only(ZSRC_WIRED, 4);
    zone_hub_binding_t s = main_only(ZSRC_SIM, Z_HALL_PIR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 40, &w, NOW)); /* zone 40: not yet */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_HALL_PIR, &s, NOW));
    zone_hub_input(&H, &w.in[0].addr, NULL, 1, NOW);                  /* ignored by the engine */
    zone_hub_event(&H, &s.in[0].addr, ALARM_ZEV_TAMPER, NOW);
    zone_hub_reachable(&H, &s.in[0].addr, false, NOW);
    advance(60000);
    EXPECT_ZONE_FLAG(&E, Z_HALL_PIR, ALARM_ZS_SUPERVISION);

    alarm_config_t cfg = E.cfg;
    alarm_zone_config_defaults(&cfg.zones[39], 40, ALARM_ZT_PERIMETER, 1);
    cfg.zones[Z_HALL_PIR - 1].type = ALARM_ZT_INTERIOR_DELAY;
    uint8_t reset[ALARM_ZONE_BITMAP_BYTES] = {0};
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (alarm_zone_config_resets_state(&E.eng.cfg.zones[z - 1], &cfg.zones[z - 1])) {
            reset[(z - 1) / 8] |= (uint8_t)(1u << ((z - 1) % 8));
        }
    }
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&E.eng, &cfg, E.now));
    EXPECT_NO_ZONE_FLAG(&E, 40, ALARM_ZS_FAULT);
    EXPECT_NO_ZONE_FLAG(&E, Z_HALL_PIR, ALARM_ZS_TAMPER);
    EXPECT_NO_ZONE_FLAG(&E, Z_HALL_PIR, ALARM_ZS_SUPERVISION);
    clear_records();
    zone_hub_resync(&H, reset, NOW);
    EXPECT_ZONE_FLAG(&E, 40, ALARM_ZS_FAULT);
    EXPECT_ZONE_FLAG(&E, Z_HALL_PIR, ALARM_ZS_TAMPER);
    EXPECT_ZONE_FLAG(&E, Z_HALL_PIR, ALARM_ZS_SUPERVISION);
    for (unsigned i = 0; i < NR; i++) {
        TEST_ASSERT_TRUE(R[i].zone == 40 || R[i].zone == Z_HALL_PIR);
    }
    EXPECT_POSTED(Z_HALL_PIR, ALARM_ZEV_UNREACHABLE, 1);
}

/* ---- ZSRC-004: the binding blob ----------------------------------------------------------------- */

static void reseal(uint8_t *b, size_t len)
{
    const uint32_t payload = (uint32_t)(len - 16u);
    const uint32_t crc = alarm_crc32(0u, b + 16, payload);
    for (unsigned i = 0; i < 4u; i++) {
        b[8 + i] = (uint8_t)(payload >> (8u * i));
        b[12 + i] = (uint8_t)(crc >> (8u * i));
    }
}

static void fixture_bindings(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    zone_hub_binding_t w = main_only(ZSRC_WIRED, 4);
    w.in[0].map.response_ms = 700;
    w.in[0].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &w, NOW));
    const zone_hub_binding_t s = main_only(ZSRC_SIM, 2);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 2, &s, NOW));
    zone_hub_binding_t d = ha_binding("binary_sensor.front_door");
    d.in[0].map.mode = ZSRC_MAP_STATES;
    snprintf(d.in[0].map.states, ZSRC_STATES_LEN, "%s", "open,on");
    d.in[ZSRC_ROLE_BATTERY].addr = zsrc_address(ZSRC_HA, 0, 0);
    snprintf(d.in[ZSRC_ROLE_BATTERY].addr.ref, ZSRC_REF_LEN, "sensor.front_door_battery");
    d.in[ZSRC_ROLE_BATTERY].map.mode = ZSRC_MAP_BELOW;
    d.in[ZSRC_ROLE_BATTERY].map.threshold_x100 = 2000;
    d.in[ZSRC_ROLE_BATTERY].map.hysteresis_x100 = 500;
    d.in[ZSRC_ROLE_BATTERY].map.grace_s = 300;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 5, &d, NOW));
    zone_hub_binding_t t = ha_binding("sensor.garage_temperature");
    zsrc_mapping_defaults(&t.in[0].map, ALARM_DEVCLASS_TEMPERATURE);
    t.in[0].map.threshold_x100 = -250;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 12, &t, NOW));
}

/* ZSRC-004: every field round-trips; the bytes don't depend on the order bindings were made. */
static void test_blob_round_trip(void)
{
    fixture_bindings();
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB, sizeof(BLOB), &len));
    TEST_ASSERT_EQUAL_MEMORY("ZBND", BLOB, 4);
    zone_hub_binding_t before[4];
    const uint16_t zones[4] = {1, 2, 5, 12};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(zone_hub_get(&H, zones[i], &before[i]));
    }
    hub_setup(false);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, BLOB, len));
    for (int i = 0; i < 4; i++) {
        zone_hub_binding_t after;
        TEST_ASSERT_TRUE(zone_hub_get(&H, zones[i], &after));
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after, sizeof(after));
    }
    /* The same table made in another order encodes to the same bytes. */
    static uint8_t again[ZONE_HUB_BLOB_MAX];
    hub_setup(false);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 12, &before[3], NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 5, &before[2], NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 2, &before[1], NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &before[0], NOW));
    size_t len2 = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, again, sizeof(again), &len2));
    TEST_ASSERT_EQUAL_UINT(len, len2);
    TEST_ASSERT_EQUAL_MEMORY(BLOB, again, len);
}

#include "zone_bind_fixture_v1.h"

/* ZSRC-004: a blob written by format 1 firmware decodes forever (frozen fixture), and this
 * firmware still writes exactly those bytes for it. Never edit the fixture; add a new one. */
static void test_blob_format_1_fixture(void)
{
    fixture_bindings();
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB, sizeof(BLOB), &len));
    if (getenv("DUMP_FIXTURE") != NULL) {
        printf("static const uint8_t ZBND_FIXTURE_V1[%u] = {", (unsigned)len);
        for (size_t i = 0; i < len; i++) {
            printf("%s0x%02X,", (i % 12) ? " " : "\n    ", BLOB[i]);
        }
        printf("\n};\n");
    }
    TEST_ASSERT_EQUAL_UINT(sizeof(ZBND_FIXTURE_V1), len);
    TEST_ASSERT_EQUAL_MEMORY(ZBND_FIXTURE_V1, BLOB, len);
    hub_setup(false);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, ZBND_FIXTURE_V1, sizeof(ZBND_FIXTURE_V1)));
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 5, &b));
    TEST_ASSERT_EQUAL_STRING("sensor.front_door_battery", b.in[ZSRC_ROLE_BATTERY].addr.ref);
    TEST_ASSERT_EQUAL_UINT16(300, b.in[ZSRC_ROLE_BATTERY].map.grace_s);
    TEST_ASSERT_TRUE(zone_hub_get(&H, 12, &b));
    TEST_ASSERT_EQUAL_INT32(-250, b.in[0].map.threshold_x100);
    TEST_ASSERT_TRUE(zone_hub_get(&H, 1, &b));
    TEST_ASSERT_TRUE(b.in[0].map.invert);
    TEST_ASSERT_EQUAL_UINT16(700, b.in[0].map.response_ms);
}

static size_t append(uint8_t *b, size_t len, uint8_t field, uint8_t group, const char *val,
                     uint8_t vlen)
{
    b[len++] = field;
    b[len++] = group;
    b[len++] = vlen;
    memcpy(b + len, val, vlen);
    return len + vlen;
}

static bool contains(const uint8_t *hay, size_t n, const uint8_t *needle, size_t m)
{
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) {
            return true;
        }
    }
    return false;
}

/* ZSRC-004: items of a newer firmware (a new field of an input, a new group, an entry with a
 * role or zone this build doesn't have) are kept and written back, so a rollback loses nothing;
 * a newer compat is refused; when the installer rebinds or unbinds such a zone its newer items
 * go with it. */
static void test_blob_keeps_unknown_items(void)
{
    fixture_bindings();
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB, sizeof(BLOB), &len));
    /* After the last input (zone 12): a new field 0x3C of zone 12's input, a new group 0x07,
     * and an entry with role 9 for zone 2 carrying one of its own fields. */
    len = append(BLOB, len, 0x3C, 0x01, "NEWF", 4);
    len = append(BLOB, len, 0x01, 0x07, "GROUP7", 6);
    len = append(BLOB, len, 0x00, 0x01, "\x02\x00\x09", 3);
    len = append(BLOB, len, 0x01, 0x01, "\x03", 1);
    len = append(BLOB, len, 0x00, 0x01, "\xE8\x03\x00", 3); /* zone 1000 */
    len = append(BLOB, len, 0x01, 0x01, "\x02", 1);
    reseal(BLOB, len);
    hub_setup(false);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, BLOB, len));
    TEST_ASSERT_EQUAL_UINT16(0, H.keep_lost);
    static uint8_t out[ZONE_HUB_BLOB_MAX];
    size_t olen = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, out, sizeof(out), &olen));
    TEST_ASSERT_EQUAL_UINT(len, olen);
    TEST_ASSERT_EQUAL_MEMORY(BLOB, out, len); /* written back byte for byte */
    TEST_ASSERT_TRUE(contains(out, olen, (const uint8_t *)"\x3C\x01\x04NEWF", 7));
    TEST_ASSERT_TRUE(contains(out, olen, (const uint8_t *)"\x01\x07\x06GROUP7", 9));
    TEST_ASSERT_TRUE(contains(out, olen, (const uint8_t *)"\x00\x01\x03\x02\x00\x09\x01\x01\x01\x03", 10));
    TEST_ASSERT_TRUE(contains(out, olen, (const uint8_t *)"\x00\x01\x03\xE8\x03\x00\x01\x01\x01\x02", 10));
    /* and it reads back the same */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, out, olen));
    size_t olen2 = 0;
    static uint8_t out2[ZONE_HUB_BLOB_MAX];
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, out2, sizeof(out2), &olen2));
    TEST_ASSERT_EQUAL_MEMORY(out, out2, olen);

    zone_hub_start(&H, NOW);
    zone_hub_unbind(&H, 2, NOW); /* the role-9 entry of zone 2 goes too */
    zone_hub_binding_t t = ha_binding("sensor.garage_temperature");
    zone_hub_bind(&H, 12, &t, NOW); /* rebinding drops zone 12's newer field */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, out, sizeof(out), &olen));
    TEST_ASSERT_FALSE(contains(out, olen, (const uint8_t *)"NEWF", 4));
    TEST_ASSERT_FALSE(contains(out, olen, (const uint8_t *)"\x02\x00\x09", 3));
    TEST_ASSERT_TRUE(contains(out, olen, (const uint8_t *)"GROUP7", 6));
    TEST_ASSERT_TRUE(contains(out, olen, (const uint8_t *)"\xE8\x03\x00", 3));

    BLOB[5] = ZONE_HUB_BLOB_COMPAT + 1u; /* a format older firmware must not read */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_decode(&H, BLOB, len));
}

/* ZSRC-004: damaged blobs are refused as a whole (the bindings are then empty); keep_lost counts
 * newer items that didn't fit while the known bindings still load. */
static void test_blob_rejects_damage(void)
{
    fixture_bindings();
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB, sizeof(BLOB), &len));
    static uint8_t b[ZONE_HUB_BLOB_MAX + 64];
    zone_hub_binding_t got;

    memcpy(b, BLOB, len);
    b[len - 1] ^= 0x01u; /* CRC */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_decode(&H, b, len));
    TEST_ASSERT_FALSE(zone_hub_get(&H, 1, &got));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_decode(&H, BLOB, len - 1u)); /* truncated */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_decode(&H, BLOB, 10));
    memcpy(b, BLOB, len);
    b[0] = 'X';
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_decode(&H, b, len));

    /* The same input twice, an input without a source, a field before any input, a bad value,
     * two zones on one sensor. */
    const struct {
        const char *items;
        uint8_t n;
    } bad[] = {
        {"\x00\x01\x03\x01\x00\x00\x01\x01\x01\x02\x00\x01\x03\x01\x00\x00\x01\x01\x01\x02", 20},
        {"\x00\x01\x03\x01\x00\x00", 6},
        {"\x01\x01\x01\x02", 4},
        {"\x00\x01\x03\x01\x00\x00\x01\x01\x01\x02\x05\x01\x01\x09", 14},
        {"\x00\x01\x03\x01\x00\x00\x01\x01\x01\x02\x00\x01\x03\x02\x00\x00\x01\x01\x01\x02", 20},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(b, BLOB, 16);
        memcpy(b + 16, bad[i].items, bad[i].n);
        reseal(b, 16u + bad[i].n);
        char msg[16];
        snprintf(msg, sizeof(msg), "case %u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ZONE_HUB_ERR_ARG, zone_hub_decode(&H, b, 16u + bad[i].n), msg);
    }

    /* Too many newer items to keep: the known bindings load anyway. */
    memcpy(b, BLOB, len);
    size_t n = len;
    char filler[200];
    memset(filler, 'x', sizeof(filler));
    for (int i = 0; i < 8; i++) {
        n = append(b, n, 0x01, 0x09, filler, sizeof(filler));
    }
    reseal(b, n);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, b, n));
    TEST_ASSERT_TRUE(H.keep_lost > 0u);
    TEST_ASSERT_TRUE(zone_hub_get(&H, 12, &got));
}

/* ZSRC-004: every input slot filled with the longest texts and every field set fits
 * ZONE_HUB_BLOB_MAX; a short buffer is refused. */
static void test_blob_worst_case_fits(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, &FAKE_HA_OPS, &F2, NOW));
    unsigned placed = 0;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES && placed < ZONE_HUB_MAX_INPUTS; z++) {
        zone_hub_binding_t b;
        memset(&b, 0, sizeof(b));
        const unsigned roles = (z <= ZONE_HUB_MAX_INPUTS - ALARM_MAX_ZONES) ? 2u : 1u;
        for (unsigned r = 0; r < roles; r++) {
            zsrc_input_t *in = &b.in[r];
            in->addr = zsrc_address(ZSRC_HA, 255, 0xFFFFFFFFFFFFFF00ull + placed);
            memset(in->addr.ref, 'r', ZSRC_REF_LEN - 1u);
            snprintf(in->addr.ref, ZSRC_REF_LEN, "e%u.", placed);
            in->addr.ref[strlen(in->addr.ref)] = 'r';
            in->map.mode = ZSRC_MAP_STATES;
            in->map.invert = true;
            in->map.threshold_x100 = INT32_MIN;
            in->map.hysteresis_x100 = INT32_MAX;
            memset(in->map.states, 's', ZSRC_STATES_LEN - 1u);
            in->map.grace_s = 65535;
            in->map.response_ms = 65535;
            placed++;
        }
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, z, &b, NOW));
    }
    TEST_ASSERT_EQUAL_UINT(ZONE_HUB_MAX_INPUTS, placed);
    zone_hub_binding_t more = main_only(ZSRC_WIRED, 1);
    zone_hub_unbind(&H, 1, NOW);
    zone_hub_binding_t two = main_only(ZSRC_WIRED, 2);
    two.in[ZSRC_ROLE_TAMPER].addr = zsrc_address(ZSRC_WIRED, 0, 3);
    two.in[ZSRC_ROLE_BATTERY].addr = zsrc_address(ZSRC_WIRED, 0, 4);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, 1, &two, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &more, NOW));
    static uint8_t big[ZONE_HUB_BLOB_MAX];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, big, sizeof(big), &len));
    TEST_ASSERT_TRUE(len <= ZONE_HUB_BLOB_MAX);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_ARG, zone_hub_encode(&H, big, len - 1u, NULL));
    hub_setup(false);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, big, len));
}

/* ---- the simulator and learning ------------------------------------------------------------------ */

/* ZONE-006: the simulator source drives any zone bound to it, end to end with the engine;
 * inputs nobody is bound to go nowhere. */
static void test_simulator_source(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    const zone_hub_binding_t b = main_only(ZSRC_SIM, Z_FRONT);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    const zone_hub_binding_t bad = main_only(ZSRC_SIM, ALARM_MAX_ZONES + 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, 2, &bad, NOW));
    zsrc_sim_event(&SIMSRC, Z_FRONT, ALARM_ZEV_FAULT, NOW);
    EXPECT_ZONE_FLAG(&E, Z_FRONT, ALARM_ZS_FAULT);
    zsrc_sim_event(&SIMSRC, Z_FRONT, ALARM_ZEV_TAMPER, NOW);
    EXPECT_ZONE_FLAG(&E, Z_FRONT, ALARM_ZS_TAMPER);
    zsrc_sim_event(&SIMSRC, Z_FRONT, ALARM_ZEV_UNREACHABLE, NOW);
    advance(60000);
    EXPECT_ZONE_FLAG(&E, Z_FRONT, ALARM_ZS_SUPERVISION);
    zsrc_sim_event(&SIMSRC, Z_FRONT, ALARM_ZEV_RESTORE, NOW);
    EXPECT_NO_ZONE_FLAG(&E, Z_FRONT, ALARM_ZS_FAULT);
    EXPECT_NO_ZONE_FLAG(&E, Z_FRONT, ALARM_ZS_SUPERVISION);
    clear_records();
    zsrc_sim_event(&SIMSRC, Z_WINDOW, ALARM_ZEV_FAULT, NOW); /* not bound */
    TEST_ASSERT_EQUAL_UINT(0, NR);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
    zone_hub_info_t info;
    zone_hub_info(&H, Z_FRONT, &info);
    TEST_ASSERT_EQUAL_STRING("console input 1", info.address);
}

/* ZSRC-002: learning and candidates go to the source; the hub says which zone each candidate
 * already feeds; sources' ticks run from the hub's tick and ask for the next one. */
static void test_learn_candidates_ticks(void)
{
    zone_hub_binding_t b = main_only(ZSRC_WIRED, 3);
    zone_hub_bind(&H, 8, &b, NOW);
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, true, NOW));
    TEST_ASSERT_TRUE(F.learning);
    TEST_ASSERT_FALSE(zone_hub_learn(&H, ZSRC_SIM, true, NOW)); /* nothing to learn */
    zsrc_candidate_t c[8];
    const size_t n = zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 8);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT16(0, c[0].zone);
    TEST_ASSERT_EQUAL_UINT16(8, c[2].zone);
    TEST_ASSERT_EQUAL_STRING("T3", c[2].name);
    TEST_ASSERT_EQUAL_UINT(0, zone_hub_candidates(&H, ZSRC_ZIGBEE, "", 0, c, 8));
    zone_hub_learn(&H, ZSRC_WIRED, false, NOW);
    TEST_ASSERT_FALSE(F.learning);

    F.next = NOW + 10u;
    TEST_ASSERT_EQUAL_UINT64(NOW + 10u, zone_hub_next_deadline(&H, NOW));
    advance(10);
    TEST_ASSERT_EQUAL_UINT(1, F.ticks);

    hub_setup(false); /* before start nothing is watched or ticked */
    F.report_on_watch = true;
    F.level[5] = 1;
    zone_hub_binding_t w = main_only(ZSRC_WIRED, 5);
    zone_hub_bind(&H, 2, &w, NOW);
    TEST_ASSERT_EQUAL_UINT(0, F.watches);
    zone_hub_tick(&H, NOW);
    TEST_ASSERT_EQUAL_UINT(0, F.ticks);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_hub_next_deadline(&H, NOW));
    zone_hub_start(&H, NOW);
    TEST_ASSERT_EQUAL_UINT(1, F.watches);
    EXPECT_LAST(2, ALARM_ZEV_FAULT); /* the source reported from inside watch() */
}

/* ---- the wired source ------------------------------------------------------------------------------ */

static uint8_t PIN[64];
static bool PIN_SETUP[64];
static zsrc_wired_t W;
static const zsrc_wired_terminal_t TERMS[] = {
    {2, "J7 header", false}, {3, "J7 header", false}, {4, "J7 header", false}, {25, NULL, false}};

static bool gpio_setup(void *ctx, uint8_t gpio, bool on)
{
    (void)ctx;
    PIN_SETUP[gpio] = on;
    return true;
}

static uint8_t gpio_read(void *ctx, uint8_t gpio)
{
    (void)ctx;
    return PIN[gpio];
}

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                       uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H, a, state, value, now);
}

static void rf_contact(zsrc_rf_t *rf, bool open)
{
    const zsrc_rf_packet_t packet = zsrc_rf_ev1527(open ? 0xA2B3C5u : 0xA2B3CEu);
    for (unsigned i = 0; i < 4u; i++) {
        zsrc_rf_packet(rf, &packet, NOW);
        advance(40u);
        sim_advance(&E, 40u);
    }
    advance(1000u);
    sim_advance(&E, 1000u);
}

/* CORE-BYP-001/007, RF-003, ZSRC-006: bypass suppresses protection, never sensor updates.
 * Exercise the actual RF source, hub and engine together, including repeated bypasses. */
static void test_rf_contact_keeps_reporting_through_bypass(void)
{
    static zsrc_rf_t rf;
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    const zsrc_sink_t sink = {.input = sink_input, .event = sink_event};
    zsrc_rf_init(&rf, &sink);
    zsrc_rf_receiver(&rf, true, NOW);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_rf_ops(), &rf, NOW));
    zone_hub_binding_t binding = {0};
    binding.in[0].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, 0xA2B3Cu, 5u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &binding, NOW));
    rf_contact(&rf, true);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
    TEST_ASSERT_FALSE(sim_status(&E, 1).ready);

    for (unsigned i = 0; i < 3u; i++) {
        EXPECT_RESULT(ALARM_OK, sim_bypass(&E, 1, CODE_USER, Z_WINDOW));
        EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_BYPASSED);
        TEST_ASSERT_TRUE(sim_status(&E, 1).ready);
        rf_contact(&rf, false);
        EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
        rf_contact(&rf, true);
        EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
        TEST_ASSERT_TRUE(sim_status(&E, 1).ready);
        EXPECT_EVENTS(&E, ALARM_EV_ALARM, 0);

        alarm_cmd_t cmd = {.type = ALARM_CMD_UNBYPASS, .partition = 1, .zone = Z_WINDOW};
        memcpy(cmd.code, CODE_USER, sizeof(CODE_USER));
        EXPECT_RESULT(ALARM_OK, sim_cmd(&E, &cmd));
        EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_BYPASSED);
        EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
        TEST_ASSERT_FALSE(sim_status(&E, 1).ready);
        rf_contact(&rf, false);
        EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
        TEST_ASSERT_TRUE(sim_status(&E, 1).ready);
        rf_contact(&rf, true);
        EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
        TEST_ASSERT_FALSE(sim_status(&E, 1).ready);
    }
    zone_hub_binding_t after;
    TEST_ASSERT_TRUE(zone_hub_get(&H, Z_WINDOW, &after));
    TEST_ASSERT_EQUAL_MEMORY(&binding, &after, sizeof(binding));
}

static void wired_setup(void)
{
    const zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed};
    zone_hub_init(&H, &port);
    memset(PIN, 0, sizeof(PIN));
    memset(PIN_SETUP, 0, sizeof(PIN_SETUP));
    const zsrc_gpio_port_t gpio = {.ctx = NULL, .setup = gpio_setup, .read = gpio_read};
    const zsrc_sink_t sink = {.ctx = NULL, .input = sink_input, .event = sink_event};
    zsrc_wired_init(&W, TERMS, sizeof(TERMS) / sizeof(TERMS[0]), &gpio, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_wired_ops(), &W, NOW));
    zone_hub_start(&H, NOW);
    clear_records();
}

static void run_ms(uint64_t ms)
{
    for (uint64_t t = 0; t < ms; t += ZSRC_WIRED_POLL_MS) {
        advance(ZSRC_WIRED_POLL_MS);
    }
}

/* ZONE-004, ZSRC-006: a wired terminal is set up only once a zone is bound to it, reports its
 * state at once, then changes after the response time (350 ms unless the binding says
 * otherwise); glitches are ignored; a normally open contact is inverted; unbinding gives the
 * pin back and polling stops when nothing is watched. */
static void test_wired_terminals(void)
{
    wired_setup();
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_hub_next_deadline(&H, NOW));
    const zone_hub_binding_t none = main_only(ZSRC_WIRED, 5);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, 1, &none, NOW));
    TEST_ASSERT_FALSE(PIN_SETUP[5]);

    const zone_hub_binding_t door = main_only(ZSRC_WIRED, 3);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &door, NOW));
    TEST_ASSERT_TRUE(PIN_SETUP[3]);
    TEST_ASSERT_FALSE(PIN_SETUP[2]);
    EXPECT_LAST(1, ALARM_ZEV_RESTORE); /* closed loop, reported at once */
    TEST_ASSERT_EQUAL_UINT64(NOW + ZSRC_WIRED_POLL_MS, zone_hub_next_deadline(&H, NOW));

    PIN[3] = 1; /* the door opens */
    run_ms(340);
    EXPECT_POSTED(1, ALARM_ZEV_FAULT, 0);
    run_ms(20);
    EXPECT_POSTED(1, ALARM_ZEV_FAULT, 1);
    PIN[3] = 0; /* a 100 ms glitch */
    run_ms(100);
    PIN[3] = 1;
    run_ms(1000);
    EXPECT_POSTED(1, ALARM_ZEV_RESTORE, 1);
    PIN[3] = 0;
    run_ms(400);
    EXPECT_POSTED(1, ALARM_ZEV_RESTORE, 2);

    zone_hub_binding_t slow = door; /* a new response time keeps the state */
    slow.in[0].map.response_ms = 1200;
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &slow, NOW));
    TEST_ASSERT_EQUAL_UINT(0, NR);
    PIN[3] = 1;
    run_ms(1000);
    TEST_ASSERT_EQUAL_UINT(0, NR);
    run_ms(300);
    EXPECT_LAST(1, ALARM_ZEV_FAULT);

    zone_hub_binding_t button = main_only(ZSRC_WIRED, 4); /* normally open: closes on alarm */
    button.in[0].map.invert = true;
    PIN[4] = 1;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 9, &button, NOW));
    EXPECT_LAST(9, ALARM_ZEV_RESTORE);
    PIN[4] = 0;
    run_ms(400);
    EXPECT_LAST(9, ALARM_ZEV_FAULT);

    zone_hub_unbind(&H, 1, NOW);
    TEST_ASSERT_FALSE(PIN_SETUP[3]);
    EXPECT_POSTED(1, ALARM_ZEV_RESTORE, 1); /* what the door had said is undone */
    zone_hub_unbind(&H, 9, NOW);
    TEST_ASSERT_FALSE(PIN_SETUP[4]);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_hub_next_deadline(&H, NOW));
}

/* ZSRC-010, RF-025, RTC-016, ZW-046 (ADR-048): an add-on on custom pins takes a wired terminal a
 * zone is bound to. After the restart the wired source no longer offers that terminal: the zone
 * keeps its binding exactly, its pin is never set up, and it shows CHECK after the grace. */
static void test_terminal_no_longer_offered_is_check(void)
{
    wired_setup();
    const zone_hub_binding_t door = main_only(ZSRC_WIRED, 4);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 7, &door, NOW));
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, BLOB, sizeof(BLOB), &len));

    /* the next start: GPIO 4 went to the custom place, the other terminals are still offered */
    static const zsrc_wired_terminal_t LEFT[] = {{2, "J7 header", false}, {3, "J7 header", false}, {25, NULL, false}};
    const zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed};
    zone_hub_init(&H, &port);
    memset(PIN_SETUP, 0, sizeof(PIN_SETUP));
    const zsrc_gpio_port_t gpio = {.ctx = NULL, .setup = gpio_setup, .read = gpio_read};
    const zsrc_sink_t sink = {.ctx = NULL, .input = sink_input, .event = sink_event};
    zsrc_wired_init(&W, LEFT, sizeof(LEFT) / sizeof(LEFT[0]), &gpio, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_wired_ops(), &W, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, BLOB, len));
    zone_hub_start(&H, NOW);
    clear_records();
    TEST_ASSERT_FALSE(PIN_SETUP[4]); /* the add-on's pin is left alone */

    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 7, &info));
    TEST_ASSERT_TRUE(info.source_present);
    TEST_ASSERT_TRUE(info.unreachable);
    advance(59999);
    EXPECT_POSTED(7, ALARM_ZEV_UNREACHABLE, 0);
    advance(1);
    EXPECT_POSTED(7, ALARM_ZEV_UNREACHABLE, 1); /* CHECK */
    TEST_ASSERT_TRUE(zone_hub_info(&H, 7, &info));
    TEST_ASSERT_TRUE(info.check);
    zone_hub_binding_t kept;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 7, &kept));
    TEST_ASSERT_EQUAL_MEMORY(&door, &kept, sizeof(door)); /* the binding stays */
    TEST_ASSERT_FALSE(PIN_SETUP[4]);
}

/* ZSRC-002 / ZSRC-009: learning sets up every free terminal so the zone editor can show them
 * live, debounced (a chattering wire never makes the words flicker, UI-020); the one opened
 * and closed meanwhile is marked; candidates say which zone uses a terminal; learning off gives
 * the free pins back. */
static void test_wired_learning(void)
{
    wired_setup();
    const zone_hub_binding_t door = main_only(ZSRC_WIRED, 2);
    zone_hub_bind(&H, 1, &door, NOW);
    zsrc_candidate_t c[8];
    size_t n = zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 8);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_STRING("closed", c[0].state);
    TEST_ASSERT_EQUAL_STRING("", c[1].state); /* not read while nobody watches it */

    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, true, NOW));
    TEST_ASSERT_TRUE(PIN_SETUP[3] && PIN_SETUP[4] && PIN_SETUP[25]);
    PIN[3] = 1; /* a loose wire chattering on GPIO 3 never settles: no change shown */
    for (int i = 0; i < 40; i++) {
        PIN[3] = (uint8_t)(i & 1);
        run_ms(100);
    }
    PIN[3] = 0;
    PIN[4] = 1; /* the installer opens the new sensor */
    run_ms(20);
    n = zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 8);
    TEST_ASSERT_FALSE(c[2].changed); /* not settled yet */
    run_ms(400);
    n = zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 8);
    TEST_ASSERT_EQUAL_STRING("GPIO 2", c[0].name);
    TEST_ASSERT_EQUAL_STRING("J7 header", c[0].detail);
    TEST_ASSERT_EQUAL_UINT16(1, c[0].zone);
    TEST_ASSERT_FALSE(c[1].changed);
    TEST_ASSERT_EQUAL_STRING("closed", c[1].state);
    TEST_ASSERT_TRUE(c[2].changed);
    TEST_ASSERT_EQUAL_STRING("open", c[2].state);
    TEST_ASSERT_EQUAL_UINT16(0, c[2].zone);
    PIN[4] = 0; /* ... and closes it: still the one that changed */
    run_ms(400);
    zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 8);
    TEST_ASSERT_TRUE(c[2].changed);
    TEST_ASSERT_EQUAL_STRING("closed", c[2].state);
    zone_hub_info_t info;
    zone_hub_info(&H, 1, &info);
    TEST_ASSERT_EQUAL_STRING("GPIO 2 on the J7 header", info.address);
    TEST_ASSERT_TRUE((info.caps & ZSRC_CAP_LIFE_SAFETY) != 0u);

    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, false, NOW));
    TEST_ASSERT_TRUE(PIN_SETUP[2]);
    TEST_ASSERT_FALSE(PIN_SETUP[3] || PIN_SETUP[4] || PIN_SETUP[25]);
    const zone_hub_binding_t pir = main_only(ZSRC_WIRED, 25);
    zone_hub_bind(&H, 2, &pir, NOW);
    zone_hub_info(&H, 2, &info);
    TEST_ASSERT_EQUAL_STRING("GPIO 25", info.address);
}

/* OUT-021: a terminal an output takes is no zone's: refused while a zone watches it; reserved, it
 * is no valid address, never set up while learning and not offered; given back, it is again. */
static void test_wired_terminal_reserved_for_an_output(void)
{
    wired_setup();
    const zone_hub_binding_t door = main_only(ZSRC_WIRED, 2);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &door, NOW));
    TEST_ASSERT_TRUE(zsrc_wired_in_use(&W, 2));
    TEST_ASSERT_FALSE(zsrc_wired_reserve(&W, 2, true)); /* the door's */
    TEST_ASSERT_FALSE(zsrc_wired_reserve(&W, 99, true)); /* no terminal */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, true, NOW));
    TEST_ASSERT_TRUE(PIN_SETUP[4]);
    TEST_ASSERT_TRUE(zsrc_wired_reserve(&W, 4, true)); /* set up for learning: given back */
    TEST_ASSERT_FALSE(PIN_SETUP[4]);
    zsrc_candidate_t c[8];
    const size_t n = zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 8);
    TEST_ASSERT_EQUAL_UINT(3, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(4u, c[i].addr.id);
    }
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, false, NOW));
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, true, NOW));
    TEST_ASSERT_FALSE(PIN_SETUP[4]);
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_WIRED, false, NOW));
    const zone_hub_binding_t pir = main_only(ZSRC_WIRED, 4);
    TEST_ASSERT_NOT_EQUAL(ZONE_HUB_OK, zone_hub_bind(&H, 2, &pir, NOW));
    TEST_ASSERT_TRUE(zsrc_wired_reserve(&W, 4, false));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 2, &pir, NOW));
    TEST_ASSERT_TRUE(PIN_SETUP[4]);
}

/* ---- end-of-line loops (ZONE-005, BL-063) ---------------------------------------------------- */

static uint32_t MV[64];       /* each terminal's voltage */
static bool ANALOG[64];       /* set up as an analog input */
static const zsrc_wired_terminal_t EOL_TERMS[] = {{49, "J7 header", true}, {2, "J7 header", false}};

static bool gpio_setup_analog(void *ctx, uint8_t gpio, bool on)
{
    (void)ctx;
    ANALOG[gpio] = on;
    return true;
}

static bool gpio_read_mv(void *ctx, uint8_t gpio, uint32_t *mv, uint32_t *vref)
{
    (void)ctx;
    *mv = MV[gpio];
    *vref = 3300u;
    return true;
}

/* The voltage a loop of `ohms` gives with the 4.7 k pull-up at 3.3 V (UINT32_MAX: open). */
static uint32_t loop_mv(uint32_t ohms)
{
    return ohms == UINT32_MAX ? 3300u : (uint32_t)(3300ull * ohms / (ohms + ZEOL_PULLUP_OHM));
}

static void eol_setup(void)
{
    const zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed};
    zone_hub_init(&H, &port);
    memset(PIN, 0, sizeof(PIN));
    memset(PIN_SETUP, 0, sizeof(PIN_SETUP));
    memset(ANALOG, 0, sizeof(ANALOG));
    for (size_t i = 0; i < 64u; i++) {
        MV[i] = loop_mv(ZEOL_OHM);
    }
    const zsrc_gpio_port_t gpio = {.ctx = NULL, .setup = gpio_setup, .read = gpio_read,
                                   .setup_analog = gpio_setup_analog, .read_mv = gpio_read_mv};
    const zsrc_sink_t sink = {.ctx = NULL, .input = sink_input, .event = sink_event};
    zsrc_wired_init(&W, EOL_TERMS, sizeof(EOL_TERMS) / sizeof(EOL_TERMS[0]), &gpio, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_wired_ops(), &W, NOW));
    zone_hub_start(&H, NOW);
    clear_records();
}

static zone_hub_binding_t eol_binding(uint8_t gpio, uint8_t input)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_wired_eol_address(gpio, input);
    return b;
}

/* ZONE-005: what a loop's resistance reads as - short under half the resistor, normal, tripped
 * (two resistors, DEOL only), open beyond five resistors - and what each wiring makes of it. */
static void test_eol_readings(void)
{
    TEST_ASSERT_EQUAL_INT(ZEOL_SHORT, zeol_classify(ZEOL_IN_NC, loop_mv(0u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_SHORT, zeol_classify(ZEOL_IN_NC, loop_mv(900u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_NORMAL, zeol_classify(ZEOL_IN_NC, loop_mv(2000u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_NORMAL, zeol_classify(ZEOL_IN_NC, loop_mv(4000u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_OPEN, zeol_classify(ZEOL_IN_NC, loop_mv(12000u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_OPEN, zeol_classify(ZEOL_IN_NC, 3300u, 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_NORMAL, zeol_classify(ZEOL_IN_DEOL, loop_mv(2000u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_TRIPPED, zeol_classify(ZEOL_IN_DEOL, loop_mv(4000u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_OPEN, zeol_classify(ZEOL_IN_DEOL, loop_mv(UINT32_MAX), 3300u));
    /* 5 % resistors and a few ohms of wire stay where they are */
    TEST_ASSERT_EQUAL_INT(ZEOL_NORMAL, zeol_classify(ZEOL_IN_DEOL, loop_mv(2150u), 3300u));
    TEST_ASSERT_EQUAL_INT(ZEOL_TRIPPED, zeol_classify(ZEOL_IN_DEOL, loop_mv(3750u), 3300u));
    const zeol_meaning_t nc_open = zeol_mean(ZEOL_IN_NC, ZEOL_OPEN);
    const zeol_meaning_t nc_short = zeol_mean(ZEOL_IN_NC, ZEOL_SHORT);
    TEST_ASSERT_TRUE(nc_open.fault && !nc_open.tamper && !nc_open.trouble);
    TEST_ASSERT_TRUE(!nc_short.fault && nc_short.tamper);
    const zeol_meaning_t no_short = zeol_mean(ZEOL_IN_NO, ZEOL_SHORT);
    const zeol_meaning_t no_open = zeol_mean(ZEOL_IN_NO, ZEOL_OPEN);
    TEST_ASSERT_TRUE(no_short.fault && !no_short.trouble);
    TEST_ASSERT_TRUE(!no_open.fault && no_open.trouble); /* a fire loop's cut wire */
    const zeol_meaning_t d_trip = zeol_mean(ZEOL_IN_DEOL, ZEOL_TRIPPED);
    TEST_ASSERT_TRUE(d_trip.fault && !d_trip.tamper);
    TEST_ASSERT_TRUE(zeol_mean(ZEOL_IN_DEOL, ZEOL_OPEN).tamper);
    TEST_ASSERT_TRUE(zeol_mean(ZEOL_IN_DEOL, ZEOL_SHORT).tamper);
    TEST_ASSERT_FALSE(zeol_mean(ZEOL_IN_DEOL, ZEOL_NORMAL).fault);
}

/* ZONE-005: a normally closed contact with its end-of-line resistor - the door opening is the
 * zone's fault, a short across the loop its tamper, each after the response time; only a terminal
 * that measures takes one, and its pin is analog without the pull-up. */
static void test_eol_normally_closed_loop(void)
{
    eol_setup();
    zone_hub_binding_t on_digital = eol_binding(2u, ZEOL_IN_NC);
    TEST_ASSERT_NOT_EQUAL(ZONE_HUB_OK, zone_hub_bind(&H, 2, &on_digital, NOW));
    zone_hub_binding_t door = eol_binding(49u, ZEOL_IN_NC);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 1, &door, NOW));
    TEST_ASSERT_TRUE(ANALOG[49]);
    TEST_ASSERT_FALSE(PIN_SETUP[49]);
    EXPECT_POSTED(1, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(1, ALARM_ZEV_TAMPER, 0);
    MV[49] = loop_mv(UINT32_MAX); /* the door opens */
    run_ms(340);
    EXPECT_POSTED(1, ALARM_ZEV_FAULT, 0);
    run_ms(20);
    EXPECT_POSTED(1, ALARM_ZEV_FAULT, 1);
    MV[49] = loop_mv(ZEOL_OHM); /* shut */
    run_ms(400);
    EXPECT_POSTED(1, ALARM_ZEV_RESTORE, 2); /* the first at its binding */
    MV[49] = loop_mv(50u); /* someone bridges the loop */
    run_ms(400);
    EXPECT_POSTED(1, ALARM_ZEV_TAMPER, 1);
    EXPECT_POSTED(1, ALARM_ZEV_FAULT, 1);
    MV[49] = loop_mv(ZEOL_OHM);
    run_ms(400);
    EXPECT_POSTED(1, ALARM_ZEV_TAMPER_RESTORE, 2);
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 1, &info));
    TEST_ASSERT_EQUAL_STRING("GPIO 49 on the J7 header, end-of-line, normally closed", info.address);
    /* unbound: the pin is given back; learning sets up the plain terminals only */
    zone_hub_unbind(&H, 1, NOW);
    TEST_ASSERT_FALSE(ANALOG[49]);
    zsrc_candidate_t c[4];
    const size_t n = zone_hub_candidates(&H, ZSRC_WIRED, "", 0, c, 4);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_TRUE(c[0].eol);
    TEST_ASSERT_FALSE(c[1].eol);
}

/* ZONE-005: a fire loop (normally open detectors across the resistor) - a detector shorting the
 * loop is the fault, a cut wire the zone's trouble, never its fault. */
static void test_eol_fire_loop(void)
{
    eol_setup();
    zone_hub_binding_t smoke = eol_binding(49u, ZEOL_IN_NO);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 6, &smoke, NOW));
    MV[49] = loop_mv(UINT32_MAX); /* a cut wire */
    run_ms(400);
    EXPECT_POSTED(6, ALARM_ZEV_TROUBLE, 1);
    EXPECT_POSTED(6, ALARM_ZEV_FAULT, 0);
    MV[49] = loop_mv(ZEOL_OHM);
    run_ms(400);
    EXPECT_POSTED(6, ALARM_ZEV_TROUBLE_RESTORE, 2);
    MV[49] = loop_mv(0u); /* a detector goes off */
    run_ms(400);
    EXPECT_POSTED(6, ALARM_ZEV_FAULT, 1);
}

/* ZONE-005: double end-of-line - two resistors are the fault, an open or shorted loop its tamper;
 * a value that changes again while it is being timed starts the timing over. */
static void test_eol_double_loop(void)
{
    eol_setup();
    zone_hub_binding_t pir = eol_binding(49u, ZEOL_IN_DEOL);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 5, &pir, NOW));
    MV[49] = loop_mv(2u * ZEOL_OHM); /* motion: the contact opens, both resistors count */
    run_ms(200);
    MV[49] = loop_mv(UINT32_MAX); /* ... and the wire is cut while that is being timed */
    run_ms(200);
    EXPECT_POSTED(5, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(5, ALARM_ZEV_TAMPER, 0);
    run_ms(200);
    EXPECT_POSTED(5, ALARM_ZEV_TAMPER, 1);
    MV[49] = loop_mv(2u * ZEOL_OHM);
    run_ms(400);
    EXPECT_POSTED(5, ALARM_ZEV_FAULT, 1);
    EXPECT_POSTED(5, ALARM_ZEV_TAMPER_RESTORE, 2);
}

/* ---- the Home Assistant source (ZSRC-005, ZSRC-008, HAI-009) ------------------------------- */

/* zsrc_ha is fed by ha_link on the panel; here a sink that only records lets the source be
 * checked on its own (the hub's side of these calls is covered above). */
typedef enum { HA_INPUT = 1, HA_REACHABLE, HA_LINK } ha_call_t;

typedef struct {
    ha_call_t call;
    char ref[ZSRC_REF_LEN];
    char state[24];
    bool flag; /* reachable, or the link is up */
} ha_rec_t;

static zsrc_ha_t HA;
static ha_rec_t HAR[128];
static unsigned NHA;
static unsigned HA_CHANGED;

static void ha_record(ha_call_t call, const zsrc_address_t *a, const char *state, bool flag)
{
    TEST_ASSERT_TRUE(NHA < sizeof(HAR) / sizeof(HAR[0]));
    ha_rec_t *r = &HAR[NHA++];
    memset(r, 0, sizeof(*r));
    r->call = call;
    r->flag = flag;
    if (a != NULL) {
        TEST_ASSERT_EQUAL_UINT8(ZSRC_HA, a->kind);
        snprintf(r->ref, sizeof(r->ref), "%.*s", (int)(sizeof(r->ref) - 1u), a->ref);
    }
    if (state != NULL) {
        snprintf(r->state, sizeof(r->state), "%.*s", (int)(sizeof(r->state) - 1u), state);
    }
}

static void ha_sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                          uint64_t now)
{
    (void)ctx;
    (void)value;
    (void)now;
    ha_record(HA_INPUT, a, state, true);
}

static void ha_sink_reachable(void *ctx, const zsrc_address_t *a, bool reachable, uint64_t now)
{
    (void)ctx;
    (void)now;
    ha_record(HA_REACHABLE, a, NULL, reachable);
}

static void ha_sink_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    (void)now;
    TEST_ASSERT_EQUAL_UINT8(ZSRC_HA, kind);
    ha_record(HA_LINK, NULL, NULL, up);
}

static void ha_watch_changed(void *ctx)
{
    (void)ctx;
    HA_CHANGED++;
}

static void ha_setup(bool link_up)
{
    const zsrc_sink_t sink = {.ctx = NULL,
                              .input = ha_sink_input,
                              .reachable = ha_sink_reachable,
                              .link = ha_sink_link};
    zsrc_ha_init(&HA, &sink);
    zsrc_ha_set_watch_changed(&HA, ha_watch_changed, NULL);
    NOW = 1000000u;
    if (link_up) {
        zsrc_ha_link(&HA, true, NOW);
    }
    NHA = 0;
    HA_CHANGED = 0;
}

static void ha_add(const char *id, const char *name, const char *area, const char *state,
                   const char *ha_class)
{
    zsrc_ha_entity_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "%s", id);
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.area, sizeof(e.area), "%s", area);
    snprintf(e.state, sizeof(e.state), "%s", state);
    e.device_class = (uint8_t)zsrc_ha_device_class(ha_class);
    zsrc_ha_catalog_add(&HA, &e);
}

static bool ha_watch(const char *id)
{
    const zsrc_address_t a = zsrc_ha_address(id);
    zsrc_mapping_t m;
    memset(&m, 0, sizeof(m));
    return zsrc_ha_ops()->watch(&HA, &a, &m, NOW);
}

static void ha_unwatch(const char *id)
{
    const zsrc_address_t a = zsrc_ha_address(id);
    zsrc_ha_ops()->unwatch(&HA, &a);
}

static unsigned ha_count(ha_call_t call, const char *ref)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NHA; i++) {
        n += HAR[i].call == call && (ref == NULL || strcmp(HAR[i].ref, ref) == 0);
    }
    return n;
}

static zsrc_candidate_t HAC[ZSRC_HA_CATALOG_MAX + 4u];

static size_t ha_search(const char *query)
{
    return zsrc_ha_ops()->candidates(&HA, query, 0, HAC, sizeof(HAC) / sizeof(HAC[0]));
}

static void ha_three(void)
{
    ha_setup(true);
    zsrc_ha_catalog_begin(&HA);
    ha_add("binary_sensor.front_door", "Front Door", "Hall", "off", "door");
    ha_add("sensor.cellar_temperature", "Thermometer", "Cellar", "4.5", "temperature");
    ha_add("binary_sensor.shed", "Shed door", "Garden", "", "door");
}

/* ---- the real source wired to the hub, as zone_service wires it on the panel -------------- */

/* The tests above check zsrc_ha on its own. These feed it into the hub, so that watch() reports
 * from inside zone_hub_bind (the synchronous callback R02 was about) and link changes reach the
 * hub's own supervision (ZSRC-015). */
static zsrc_ha_t HAHUB;

static void hub_sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                           uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H, a, state, value, now);
}

static void hub_sink_reachable(void *ctx, const zsrc_address_t *a, bool reachable, uint64_t now)
{
    (void)ctx;
    zone_hub_reachable(&H, a, reachable, now);
}

static void hub_sink_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    zone_hub_link(&H, kind, up, now);
}

/* Registers the real Home Assistant source with the hub. It is registered while it is still
 * disconnected, exactly as zone_service does at startup. */
static void ha_on_hub(bool link_up)
{
    const zsrc_sink_t sink = {.ctx = NULL,
                              .input = hub_sink_input,
                              .event = sink_event,
                              .reachable = hub_sink_reachable,
                              .link = hub_sink_link};
    zsrc_ha_init(&HAHUB, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_ha_ops(), &HAHUB, NOW));
    if (link_up) {
        zsrc_ha_link(&HAHUB, true, NOW);
    }
    clear_records();
}

static void ha_hub_add(const char *id, const char *state, const char *ha_class)
{
    zsrc_ha_entity_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "%s", id);
    snprintf(e.state, sizeof(e.state), "%s", state);
    e.device_class = (uint8_t)zsrc_ha_device_class(ha_class);
    zsrc_ha_catalog_add(&HAHUB, &e);
}

/* What Home Assistant pushes for a bound entity. */
static void ha_hub_push(const char *id, const char *state)
{
    zsrc_ha_state(&HAHUB, id, state, true, NOW);
}

static zone_hub_info_t INFO;

static const zone_hub_info_t *zone_state(uint16_t zone)
{
    TEST_ASSERT_TRUE(zone_hub_info(&H, zone, &INFO));
    return &INFO;
}

/* HAI-009: the catalog is what Home Assistant lets the panel see; page 0 replaces it, a repeated
 * entity_id replaces that row, and the picker searches it by name, id or area, case and part
 * words alike (an empty query is every entity). */
static void test_ha_catalog_and_candidates(void)
{
    ha_three();
    TEST_ASSERT_EQUAL_UINT(3, ha_search(""));
    TEST_ASSERT_EQUAL_STRING("Front Door", HAC[0].name);
    TEST_ASSERT_EQUAL_STRING("Hall", HAC[0].detail);
    TEST_ASSERT_EQUAL_STRING("off", HAC[0].state);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_HA, HAC[0].addr.kind);
    TEST_ASSERT_EQUAL_STRING("binary_sensor.front_door", HAC[0].addr.ref);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, HAC[0].device_class);
    /* Home Assistant's "temperature" maps to the thermometer class, so the picker suggests
     * the freeze-warning mapping (zsrc_mapping_defaults: below 5 °C with 1 °C hysteresis). */
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_TEMPERATURE, HAC[1].device_class);

    TEST_ASSERT_EQUAL_UINT(1, ha_search("front"));  /* part of a name, lower case */
    TEST_ASSERT_EQUAL_UINT(1, ha_search("THERMO")); /* upper case */
    TEST_ASSERT_EQUAL_UINT(1, ha_search("cellar_temp")); /* part of the entity_id */
    TEST_ASSERT_EQUAL_UINT(1, ha_search("Garden"));      /* an area */
    TEST_ASSERT_EQUAL_UINT(2, ha_search("door"));        /* "Front Door" and "Shed door" */
    TEST_ASSERT_EQUAL_UINT(0, ha_search("no such thing"));

    /* the same entity again replaces its row, it isn't added twice */
    ha_add("binary_sensor.front_door", "Hall Door", "Hallway", "on", "door");
    TEST_ASSERT_EQUAL_UINT(3, ha_search(""));
    TEST_ASSERT_EQUAL_STRING("Hall Door", HAC[0].name);
    TEST_ASSERT_EQUAL_STRING("on", HAC[0].state);

    /* an entity without a name is offered under its entity_id */
    ha_add("binary_sensor.gate", "", "", "off", "");
    TEST_ASSERT_EQUAL_UINT(4, ha_search(""));
    TEST_ASSERT_EQUAL_UINT(1, ha_search("gate"));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.gate", HAC[0].name);

    /* a new page 0 drops the old catalog */
    zsrc_ha_catalog_begin(&HA);
    TEST_ASSERT_EQUAL_UINT(0, ha_search(""));

    /* more entities than the panel keeps: the first ones stay, the rest are left out */
    for (unsigned i = 0; i < ZSRC_HA_CATALOG_MAX + 3u; i++) {
        char id[ZSRC_REF_LEN];
        snprintf(id, sizeof(id), "binary_sensor.e%u", i);
        ha_add(id, "", "", "off", "door");
    }
    TEST_ASSERT_EQUAL_UINT(ZSRC_HA_CATALOG_MAX, HA.n_catalog);
    TEST_ASSERT_EQUAL_UINT(ZSRC_HA_CATALOG_MAX, ha_search(""));
    TEST_ASSERT_EQUAL_UINT(1, ha_search("sensor.e0")); /* the first one is kept */
    TEST_ASSERT_EQUAL_UINT(0, ha_search("binary_sensor.e65")); /* the last ones are not */
}

/* HAI-009 / ZSRC-005: watching an entity reports what the catalog already says, so the zone
 * settles at once; ha_link is told whenever the watch list changes, and only then. */
static void test_ha_watch_list(void)
{
    ha_three();
    TEST_ASSERT_TRUE(ha_watch("binary_sensor.front_door"));
    TEST_ASSERT_EQUAL_UINT(1, HA_CHANGED);
    TEST_ASSERT_EQUAL_UINT(1, ha_count(HA_INPUT, "binary_sensor.front_door"));
    TEST_ASSERT_EQUAL_STRING("off", HAR[NHA - 1u].state);

    /* an entity the catalog knows nothing about yet: nothing to report */
    TEST_ASSERT_TRUE(ha_watch("binary_sensor.gate"));
    TEST_ASSERT_EQUAL_UINT(2, HA_CHANGED);
    TEST_ASSERT_EQUAL_UINT(0, ha_count(HA_INPUT, "binary_sensor.gate"));

    /* the same sensor with a new mapping: reported again, but the watch list is unchanged */
    TEST_ASSERT_TRUE(ha_watch("binary_sensor.front_door"));
    TEST_ASSERT_EQUAL_UINT(2, HA_CHANGED);
    TEST_ASSERT_EQUAL_UINT(2, ha_count(HA_INPUT, "binary_sensor.front_door"));

    const char *list[ZSRC_HA_WATCH_MAX + 2u];
    TEST_ASSERT_EQUAL_UINT(2, zsrc_ha_watch_list(&HA, list, ZSRC_HA_WATCH_MAX + 2u));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.front_door", list[0]);
    TEST_ASSERT_EQUAL_STRING("binary_sensor.gate", list[1]);

    ha_unwatch("binary_sensor.gate");
    TEST_ASSERT_EQUAL_UINT(3, HA_CHANGED);
    TEST_ASSERT_EQUAL_UINT(1, zsrc_ha_watch_list(&HA, list, ZSRC_HA_WATCH_MAX + 2u));
    ha_unwatch("binary_sensor.gate"); /* not watched: nothing to tell ha_link */
    TEST_ASSERT_EQUAL_UINT(3, HA_CHANGED);

    /* every slot bound: the zone editor is told this source is full */
    ha_setup(true);
    for (unsigned i = 0; i < ZSRC_HA_WATCH_MAX; i++) {
        char id[ZSRC_REF_LEN];
        snprintf(id, sizeof(id), "binary_sensor.e%u", i);
        TEST_ASSERT_TRUE(ha_watch(id));
    }
    TEST_ASSERT_EQUAL_UINT(ZSRC_HA_WATCH_MAX, HA_CHANGED);
    TEST_ASSERT_FALSE(ha_watch("binary_sensor.one_too_many"));
    TEST_ASSERT_EQUAL_UINT(ZSRC_HA_WATCH_MAX, HA_CHANGED);
    TEST_ASSERT_EQUAL_UINT(ZSRC_HA_WATCH_MAX,
                           zsrc_ha_watch_list(&HA, list, ZSRC_HA_WATCH_MAX + 2u));
}

/* HAI-009: a state push becomes the zone's input; "unavailable" over there becomes unreachable
 * here (ZSRC-007 then runs the grace into CHECK); states for entities no zone uses only keep
 * the picker's words up to date. */
static void test_ha_state_pushes(void)
{
    ha_three();
    TEST_ASSERT_TRUE(ha_watch("binary_sensor.front_door"));
    NHA = 0;

    zsrc_ha_state(&HA, "binary_sensor.front_door", "on", true, NOW);
    TEST_ASSERT_EQUAL_UINT(1, NHA); /* it was already known to be there: just the state */
    TEST_ASSERT_EQUAL_INT(HA_INPUT, HAR[0].call);
    TEST_ASSERT_EQUAL_STRING("binary_sensor.front_door", HAR[0].ref);
    TEST_ASSERT_EQUAL_STRING("on", HAR[0].state);

    NHA = 0;
    zsrc_ha_state(&HA, "binary_sensor.front_door", "unavailable", false, NOW);
    TEST_ASSERT_EQUAL_UINT(1, NHA);
    TEST_ASSERT_EQUAL_INT(HA_REACHABLE, HAR[0].call);
    TEST_ASSERT_FALSE(HAR[0].flag);
    zsrc_ha_state(&HA, "binary_sensor.front_door", "unavailable", false, NOW);
    TEST_ASSERT_EQUAL_UINT(1, NHA); /* still gone: said once */
    TEST_ASSERT_EQUAL_UINT(3, ha_search(""));
    TEST_ASSERT_EQUAL_STRING("", HAC[0].state); /* the picker stops claiming a state */

    NHA = 0;
    zsrc_ha_state(&HA, "binary_sensor.front_door", "off", true, NOW);
    TEST_ASSERT_EQUAL_UINT(2, NHA); /* back first, then what it says */
    TEST_ASSERT_EQUAL_INT(HA_REACHABLE, HAR[0].call);
    TEST_ASSERT_TRUE(HAR[0].flag);
    TEST_ASSERT_EQUAL_INT(HA_INPUT, HAR[1].call);
    TEST_ASSERT_EQUAL_STRING("off", HAR[1].state);

    /* an entity no zone is bound to: the catalog's words change, nothing reaches a zone */
    NHA = 0;
    zsrc_ha_state(&HA, "sensor.cellar_temperature", "3.5", true, NOW);
    TEST_ASSERT_EQUAL_UINT(0, NHA);
    TEST_ASSERT_EQUAL_UINT(3, ha_search(""));
    TEST_ASSERT_EQUAL_STRING("3.5", HAC[1].state);

    /* an entity neither watched nor in the catalog, and a push with no id: ignored */
    zsrc_ha_state(&HA, "binary_sensor.nowhere", "on", true, NOW);
    zsrc_ha_state(&HA, "", "on", true, NOW);
    zsrc_ha_state(&HA, NULL, "on", true, NOW);
    TEST_ASSERT_EQUAL_UINT(0, NHA);
}

/* ZSRC-005 / ZONE-008: zones can only be added with this source while the paired Home Assistant
 * is connected; the link going up or down is the source's own trouble, and after it comes back
 * every watched entity is unknown again until Home Assistant says otherwise. */
static void test_ha_link_up_down(void)
{
    ha_setup(false);
    TEST_ASSERT_FALSE(zsrc_ha_ops()->usable(&HA));
    zsrc_ha_link(&HA, true, NOW);
    TEST_ASSERT_TRUE(zsrc_ha_ops()->usable(&HA));
    TEST_ASSERT_EQUAL_UINT(1, ha_count(HA_LINK, NULL));
    TEST_ASSERT_TRUE(HAR[NHA - 1u].flag);
    zsrc_ha_link(&HA, true, NOW); /* already up: nothing new */
    TEST_ASSERT_EQUAL_UINT(1, ha_count(HA_LINK, NULL));

    TEST_ASSERT_TRUE(ha_watch("binary_sensor.front_door"));
    zsrc_ha_state(&HA, "binary_sensor.front_door", "off", true, NOW);

    NHA = 0;
    zsrc_ha_link(&HA, false, NOW);
    TEST_ASSERT_FALSE(zsrc_ha_ops()->usable(&HA));
    TEST_ASSERT_EQUAL_UINT(1, ha_count(HA_LINK, NULL));
    TEST_ASSERT_FALSE(HAR[0].flag);

    NHA = 0;
    zsrc_ha_link(&HA, true, NOW);
    TEST_ASSERT_EQUAL_UINT(1, NHA);
    /* the watch is unknown again: the first state after the reconnection says it is there */
    zsrc_ha_state(&HA, "binary_sensor.front_door", "off", true, NOW);
    TEST_ASSERT_EQUAL_UINT(3, NHA);
    TEST_ASSERT_EQUAL_INT(HA_REACHABLE, HAR[1].call);
    TEST_ASSERT_TRUE(HAR[1].flag);
    TEST_ASSERT_EQUAL_INT(HA_INPUT, HAR[2].call);
}

/* ZSRC-006: changing what a sensor's state means must be applied to that state exactly once.
 * Home Assistant reports what it has cached from inside watch(), which the hub calls with the
 * new mapping already in place: the state that comes back must not be inverted a second time,
 * or the zone ends up in the opposite state - a fault suppressed, or one invented. */
static void test_rebind_polarity_with_a_source_that_reports_at_once(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    ha_on_hub(true);
    ha_hub_add("binary_sensor.front_door", "off", "door");
    ha_hub_add("binary_sensor.shed", "on", "door");
    ha_hub_add("binary_sensor.gate", "unknown", "door");
    ha_hub_add("sensor.cellar_temperature", "7.5", "temperature");

    zone_hub_binding_t door = ha_binding("binary_sensor.front_door");
    zone_hub_binding_t shed = ha_binding("binary_sensor.shed");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FAMILY, &shed, NOW));
    TEST_ASSERT_FALSE(zone_state(Z_WINDOW)->active); /* "off" */
    TEST_ASSERT_TRUE(zone_state(Z_FAMILY)->active);  /* "on" */
    EXPECT_ZONE_FLAG(&E, Z_FAMILY, ALARM_ZS_FAULT);

    /* the installer says the front door contact is normally open: "off" now means open */
    clear_records();
    door.in[ZSRC_ROLE_MAIN].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    TEST_ASSERT_TRUE(zone_state(Z_WINDOW)->active);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_RESTORE, 0); /* not inverted a second time */
    TEST_ASSERT_EQUAL_UINT(1, NR);                 /* and no blip on the way there */
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);

    /* and back: the other zone on the same source is left alone */
    clear_records();
    door.in[ZSRC_ROLE_MAIN].map.invert = false;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    TEST_ASSERT_FALSE(zone_state(Z_WINDOW)->active);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT(1, NR);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
    TEST_ASSERT_TRUE(zone_state(Z_FAMILY)->active);

    /* the same the other way round, from a cached "on" */
    clear_records();
    shed.in[ZSRC_ROLE_MAIN].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FAMILY, &shed, NOW));
    TEST_ASSERT_FALSE(zone_state(Z_FAMILY)->active);
    EXPECT_POSTED(Z_FAMILY, ALARM_ZEV_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT(1, NR);
    EXPECT_NO_ZONE_FLAG(&E, Z_FAMILY, ALARM_ZS_FAULT);

    /* a state Home Assistant doesn't know: there is nothing to invert, and the zone doesn't
     * pretend to know either */
    clear_records();
    zone_hub_binding_t gate = ha_binding("binary_sensor.gate");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_SHED, &gate, NOW));
    TEST_ASSERT_FALSE(zone_state(Z_SHED)->known);
    gate.in[ZSRC_ROLE_MAIN].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_SHED, &gate, NOW));
    TEST_ASSERT_FALSE(zone_state(Z_SHED)->known);
    TEST_ASSERT_EQUAL_UINT(0, NR);

    /* an entity that went unavailable says nothing from watch() either, so the change lands on
     * the last state the panel really had, once */
    clear_records();
    zsrc_ha_state(&HAHUB, "binary_sensor.shed", "unavailable", false, NOW);
    TEST_ASSERT_TRUE(zone_state(Z_FAMILY)->unreachable);
    shed.in[ZSRC_ROLE_MAIN].map.invert = false;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FAMILY, &shed, NOW));
    TEST_ASSERT_TRUE(zone_state(Z_FAMILY)->active); /* "on" means open again */
    EXPECT_POSTED(Z_FAMILY, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_TRUE(zone_state(Z_FAMILY)->unreachable); /* and it still hasn't answered */

    /* numeric mappings decide afresh, and the report from watch() doesn't double that either */
    clear_records();
    zone_hub_binding_t cellar = ha_binding("sensor.cellar_temperature");
    zsrc_mapping_defaults(&cellar.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_TEMPERATURE); /* < 5 */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FREEZER, &cellar, NOW));
    TEST_ASSERT_FALSE(zone_state(Z_FREEZER)->active); /* 7.5 °C */
    clear_records();
    cellar.in[ZSRC_ROLE_MAIN].map.threshold_x100 = 1000; /* warn below 10 °C from now on */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FREEZER, &cellar, NOW));
    TEST_ASSERT_TRUE(zone_state(Z_FREEZER)->active);
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_EQUAL_UINT(1, NR);

    /* a linked tamper entity of the same zone inverts on its own */
    clear_records();
    ha_hub_add("binary_sensor.front_door_tamper", "off", "");
    door.in[ZSRC_ROLE_TAMPER].addr = zsrc_ha_address("binary_sensor.front_door_tamper");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_TAMPER_RESTORE, 1); /* "off": no tamper */
    TEST_ASSERT_EQUAL_UINT(1, NR);
    clear_records();
    door.in[ZSRC_ROLE_TAMPER].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_TAMPER, 1);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 0);
    TEST_ASSERT_EQUAL_UINT(1, NR);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_TAMPER);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
}

/* ZSRC-015: Wi-Fi is up but Home Assistant is gone. Its zones keep their last state and don't
 * flood the engine with CHECKs (ZONE-008), but the source itself becomes one supervision
 * trouble within a bounded time, so the panel never looks ready with nothing behind its zones.
 * A source nothing is bound to is not a trouble. */
static void test_source_link_trouble(void)
{
    const uint64_t grace = (uint64_t)ZSRC_HA_GRACE_S * 1000u;
    ha_on_hub(true);
    ha_hub_add("binary_sensor.front_door", "off", "door");
    zone_hub_binding_t door = ha_binding("binary_sensor.front_door");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    const uint64_t heard = zone_state(Z_WINDOW)->heard_at;
    clear_records();

    zsrc_ha_link(&HAHUB, false, NOW); /* only Home Assistant is gone */
    TEST_ASSERT_TRUE(zone_state(Z_WINDOW)->link_down);
    TEST_ASSERT_EQUAL_UINT64(NOW + grace, zone_hub_next_deadline(&H, NOW));
    advance(grace - 1u);
    TEST_ASSERT_EQUAL_UINT(0, trouble_says(ZSRC_HA));
    advance(1);
    TEST_ASSERT_EQUAL_UINT(1, trouble_says(ZSRC_HA));
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    TEST_ASSERT_TRUE(source_in_trouble(ZSRC_HA));
    advance(3600000);
    TEST_ASSERT_EQUAL_UINT(0, NR);                    /* no CHECK per zone while it is down */
    TEST_ASSERT_EQUAL_UINT(1, trouble_says(ZSRC_HA)); /* said once, not every tick */

    /* Nothing Home Assistant said before it went away counts as its sensor answering: the
     * installer changing the grace re-watches the entity, which must stay silent. */
    door.in[ZSRC_ROLE_MAIN].map.grace_s = 30;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    TEST_ASSERT_EQUAL_UINT64(heard, zone_state(Z_WINDOW)->heard_at);
    TEST_ASSERT_EQUAL_UINT(0, NR);

    /* an integration no zone uses any more isn't a trouble */
    zone_hub_unbind(&H, Z_WINDOW, NOW);
    TEST_ASSERT_EQUAL_INT(0, trouble_last(ZSRC_HA));
    TEST_ASSERT_FALSE(source_in_trouble(ZSRC_HA));

    /* A panel that restarts while Home Assistant is away: the source is registered before it
     * connects (zone_service does that at startup), so the hub counts its link as down from
     * the start instead of trusting what the bindings used to say. */
    hub_setup(true);
    ha_on_hub(false);
    zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
    const size_t n = zone_hub_sources(&H, list, ZONE_HUB_MAX_SOURCES);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_TRUE(list[2].link_down);
    TEST_ASSERT_FALSE(list[2].trouble); /* nothing is bound to it yet */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    advance(grace);
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    TEST_ASSERT_FALSE(zone_state(Z_WINDOW)->check); /* one trouble, not a CHECK per zone */
    TEST_ASSERT_FALSE(zone_state(Z_WINDOW)->known);
}

/* ZSRC-015: with the link back, nothing Home Assistant said before the break counts any more.
 * Every sensor has its grace to report again: the ones that do clear the source's trouble, the
 * ones that don't show their own zone's CHECK - at no point does the panel show a zone as ready
 * on state from before the break. */
static void test_link_back_needs_fresh_state(void)
{
    const uint64_t grace = (uint64_t)ZSRC_HA_GRACE_S * 1000u;
    ha_on_hub(true);
    ha_hub_add("binary_sensor.front_door", "off", "door");
    ha_hub_add("binary_sensor.shed", "off", "door");
    zone_hub_binding_t door = ha_binding("binary_sensor.front_door");
    zone_hub_binding_t shed = ha_binding("binary_sensor.shed");
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &door, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_SHED, &shed, NOW));
    clear_records();

    /* no state at all after the reconnection: both zones end up CHECK, and only then does the
     * source's own trouble step back */
    zsrc_ha_link(&HAHUB, false, NOW);
    advance(grace);
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    zsrc_ha_link(&HAHUB, true, NOW);
    TEST_ASSERT_TRUE(zone_state(Z_WINDOW)->known);       /* the last state is still shown ... */
    TEST_ASSERT_TRUE(zone_state(Z_WINDOW)->unreachable); /* ... but it isn't trusted */
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    advance(grace - 1u);
    TEST_ASSERT_EQUAL_UINT(0, NR);
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    advance(1);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 1);
    EXPECT_POSTED(Z_SHED, ALARM_ZEV_UNREACHABLE, 1);
    TEST_ASSERT_EQUAL_INT(0, trouble_last(ZSRC_HA)); /* the zones say it themselves now */

    /* both answer: the zones are whole again */
    ha_hub_push("binary_sensor.front_door", "off");
    ha_hub_push("binary_sensor.shed", "off");
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_REACHABLE, 1);
    EXPECT_POSTED(Z_SHED, ALARM_ZEV_REACHABLE, 1);
    TEST_ASSERT_FALSE(zone_state(Z_WINDOW)->unreachable);

    /* half a refresh: the door answers, the shed doesn't. The trouble stays until the shed's
     * own CHECK takes it over, and the door never shows CHECK. */
    clear_records();
    zsrc_ha_link(&HAHUB, false, NOW);
    advance(grace);
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    zsrc_ha_link(&HAHUB, true, NOW);
    ha_hub_push("binary_sensor.front_door", "on"); /* and it opened while nobody could see */
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_TRUE(zone_state(Z_WINDOW)->active); /* mapped afresh, not from what was cached */
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    advance(grace);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 0);
    EXPECT_POSTED(Z_SHED, ALARM_ZEV_UNREACHABLE, 1);
    TEST_ASSERT_EQUAL_INT(0, trouble_last(ZSRC_HA));

    /* a full refresh inside the grace: no zone ever shows CHECK and the trouble goes at once */
    ha_hub_push("binary_sensor.shed", "off");
    clear_records();
    zsrc_ha_link(&HAHUB, false, NOW);
    advance(grace);
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA));
    zsrc_ha_link(&HAHUB, true, NOW);
    advance(1000);
    ha_hub_push("binary_sensor.front_door", "on");
    TEST_ASSERT_EQUAL_INT(1, trouble_last(ZSRC_HA)); /* one of two */
    ha_hub_push("binary_sensor.shed", "off");
    TEST_ASSERT_EQUAL_INT(0, trouble_last(ZSRC_HA));
    advance(grace);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 0);
    EXPECT_POSTED(Z_SHED, ALARM_ZEV_UNREACHABLE, 0);
    TEST_ASSERT_EQUAL_UINT(2, trouble_says(ZSRC_HA)); /* raised once, cleared once */
}

/* ZSRC-008: what this source promises - Home Assistant's device classes in our words, and no
 * life-safety zones without an installer override (it needs Wi-Fi and someone else's server). */
static void test_ha_device_class_and_trust(void)
{
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, zsrc_ha_device_class("door"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_WINDOW, zsrc_ha_device_class("window"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GARAGE_DOOR, zsrc_ha_device_class("garage_door"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_MOTION, zsrc_ha_device_class("occupancy"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_SMOKE, zsrc_ha_device_class("smoke"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_CO, zsrc_ha_device_class("carbon_monoxide"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_WATER, zsrc_ha_device_class("moisture"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_VIBRATION, zsrc_ha_device_class("vibration"));
    /* a class this firmware doesn't know, and none at all: the installer picks the zone type */
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_ha_device_class("sparkle"));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_ha_device_class(""));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_ha_device_class(NULL));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_ha_device_class("Door")); /* exact words */

    const zone_source_ops_t *ops = zsrc_ha_ops();
    TEST_ASSERT_EQUAL_UINT8(ZSRC_HA, ops->kind);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_HA_GRACE_S, ops->grace_s);
    TEST_ASSERT_TRUE((ops->caps & ZSRC_CAP_SEARCH) != 0u);
    TEST_ASSERT_TRUE((ops->caps & ZSRC_CAP_SUPERVISED) != 0u);
    TEST_ASSERT_EQUAL_UINT32(0, ops->caps & ZSRC_CAP_OFFLINE_OK);
    TEST_ASSERT_EQUAL_UINT32(0, ops->caps & ZSRC_CAP_LIFE_SAFETY);
    TEST_ASSERT_FALSE(zsrc_trusted_for(ops->caps, ALARM_ZT_FIRE));
    TEST_ASSERT_FALSE(zsrc_trusted_for(ops->caps, ALARM_ZT_CO));
    TEST_ASSERT_FALSE(zsrc_trusted_for(ops->caps, ALARM_ZT_24H_AUDIBLE));
    TEST_ASSERT_TRUE(zsrc_trusted_for(ops->caps, ALARM_ZT_PERIMETER));

    /* an address needs an entity_id: an empty one names nothing */
    ha_three();
    const zsrc_address_t good = zsrc_ha_address("binary_sensor.front_door");
    const zsrc_address_t empty = zsrc_ha_address("");
    TEST_ASSERT_TRUE(ops->valid(&HA, &good));
    TEST_ASSERT_FALSE(ops->valid(&HA, &empty));
    char where[64];
    ops->describe(&HA, &good, where, sizeof(where));
    /* the zone page prefixes "Home Assistant ·" itself, like "Wired · GPIO 4" */
    TEST_ASSERT_EQUAL_STRING("Front Door", where);
    const zsrc_address_t unknown = zsrc_ha_address("binary_sensor.nowhere");
    ops->describe(&HA, &unknown, where, sizeof(where));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.nowhere", where);
}

/* ---- ZONE-004: response time ------------------------------------------------------------------- */

/* ZONE-004: a new level counts after it held for the response time (10/350/700/1200 ms); a
 * shorter glitch or bounce is ignored; the first sample counts at once. */
static void test_debounce_response_time(void)
{
    zone_debounce_t d;
    zone_debounce_init(&d, 350);
    TEST_ASSERT_TRUE(zone_debounce_sample(&d, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(1, d.stable);
    TEST_ASSERT_FALSE(zone_debounce_sample(&d, 0, 10));
    TEST_ASSERT_FALSE(zone_debounce_sample(&d, 0, 350)); /* 340 ms so far */
    TEST_ASSERT_FALSE(zone_debounce_sample(&d, 1, 355)); /* glitch over */
    TEST_ASSERT_FALSE(zone_debounce_sample(&d, 0, 400));
    for (uint64_t t = 410; t < 750; t += 10) {
        TEST_ASSERT_FALSE(zone_debounce_sample(&d, 0, t));
    }
    TEST_ASSERT_TRUE(zone_debounce_sample(&d, 0, 750)); /* held 350 ms */
    TEST_ASSERT_EQUAL_UINT8(0, d.stable);
    TEST_ASSERT_FALSE(zone_debounce_sample(&d, 0, 760));

    /* A bouncing contact: 20 ms of chatter, then steady. */
    uint64_t t = 1000;
    for (int i = 0; i < 10; i++, t += 2) {
        TEST_ASSERT_FALSE(zone_debounce_sample(&d, (uint8_t)(i & 1), t));
    }
    TEST_ASSERT_FALSE(zone_debounce_sample(&d, 1, t));
    TEST_ASSERT_TRUE(zone_debounce_sample(&d, 1, t + 350));

    const uint16_t presets[] = {10, 700, 1200};
    for (size_t i = 0; i < 3; i++) {
        zone_debounce_init(&d, presets[i]);
        TEST_ASSERT_TRUE(zone_debounce_sample(&d, 0, 0));
        TEST_ASSERT_FALSE(zone_debounce_sample(&d, 1, 100));
        TEST_ASSERT_FALSE(zone_debounce_sample(&d, 1, 100u + presets[i] - 1u));
        TEST_ASSERT_TRUE(zone_debounce_sample(&d, 1, 100u + presets[i]));
        TEST_ASSERT_EQUAL_UINT8(1, d.stable);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registry);
    RUN_TEST(test_one_binding_per_zone_one_zone_per_sensor);
    RUN_TEST(test_rebind_same_sensor_keeps_state);
    RUN_TEST(test_same_sensors);
    RUN_TEST(test_rebind_new_mapping_remaps_state);
    RUN_TEST(test_lost_bindings_are_check);
    RUN_TEST(test_binary_mapping);
    RUN_TEST(test_threshold_mapping);
    RUN_TEST(test_state_list_mapping);
    RUN_TEST(test_linked_inputs_and_checkins);
    RUN_TEST(test_sensor_test_passes_through);
    RUN_TEST(test_defaults_and_trust);
    RUN_TEST(test_grace_then_check);
    RUN_TEST(test_check_per_zone);
    RUN_TEST(test_link_down_is_not_check);
    RUN_TEST(test_missing_source_is_check_and_kept);
    RUN_TEST(test_resync_after_config_change);
    RUN_TEST(test_blob_round_trip);
    RUN_TEST(test_blob_format_1_fixture);
    RUN_TEST(test_blob_keeps_unknown_items);
    RUN_TEST(test_blob_rejects_damage);
    RUN_TEST(test_blob_worst_case_fits);
    RUN_TEST(test_simulator_source);
    RUN_TEST(test_rf_contact_keeps_reporting_through_bypass);
    RUN_TEST(test_learn_candidates_ticks);
    RUN_TEST(test_wired_terminals);
    RUN_TEST(test_terminal_no_longer_offered_is_check);
    RUN_TEST(test_wired_learning);
    RUN_TEST(test_wired_terminal_reserved_for_an_output);
    RUN_TEST(test_eol_readings);
    RUN_TEST(test_eol_normally_closed_loop);
    RUN_TEST(test_eol_fire_loop);
    RUN_TEST(test_eol_double_loop);
    RUN_TEST(test_ha_catalog_and_candidates);
    RUN_TEST(test_ha_watch_list);
    RUN_TEST(test_ha_state_pushes);
    RUN_TEST(test_ha_link_up_down);
    RUN_TEST(test_rebind_polarity_with_a_source_that_reports_at_once);
    RUN_TEST(test_source_link_trouble);
    RUN_TEST(test_link_back_needs_fresh_state);
    RUN_TEST(test_ha_device_class_and_trust);
    RUN_TEST(test_debounce_response_time);
    return UNITY_END();
}
