/*
 * test_bind_gate.c - a sensor binding change against arming (zone_hub/zone_bind_gate.h,
 * docs/features/22 ZSRC-012, review RF32).
 *
 * The real engine (alarm_sim), the real hub and the real 433 MHz source. change() below takes the
 * steps of zone_service.c change() for a task without the zone lock, in its order: the entry
 * check, the store mutex, the spoken-name invalidation (an engine configuration change), the
 * reservation on the alarm task, the zone lock, the claim, the hub's change, the end. It is a copy
 * of that order, not the service itself (which builds only into the firmware): the service's own
 * order is checked by code review. Every engine step is bracketed as alarm_service brackets it
 * (step_begin/step_end), and a household arm is tried at each boundary of the change, and inside
 * an engine step that overlaps the claim. Whatever the boundary, a binding never changes once a
 * partition has left disarmed, and arming is never held by a reservation: an arm before the claim
 * goes ahead and refuses the change; an arm after it is held (CORE-ARM-013's hold) until the
 * in-memory change is made.
 *
 * Requirements: ZSRC-012 (docs/features/22), CORE-ARM-013 (the hold it uses), review RF32.
 */
#include <string.h>

#include "alarm/alarm_config.h"
#include "sim.h"
#include "unity.h"
#include "zone_hub/zone_bind_gate.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_rf.h"

#define DEV_A 0xA2B3Cu /* the window's sensor */
#define DEV_B 0x0BEEFu /* its replacement */
#define OPEN  0x5u
#define CODE(dev, data) (((dev) << 4) | (data))
#define Z_P2      21u /* a window in partition 2 */
#define CODE_P2   "8888"

typedef enum {
    AT_NONE,
    AT_ENTRY,       /* after the entry check (a snapshot) */
    AT_STORE_WAIT,  /* waiting for the store mutex */
    AT_INVALIDATED, /* after the spoken-name invalidation */
    AT_LOCK_WAIT,   /* reserved, waiting for the zone lock */
    AT_IN_STEP,     /* the claim overlaps the engine step that arms */
    AT_LAPSED,      /* waited for the zone lock past ZONE_BIND_GATE_MS */
    AT_CLAIMED,     /* the zone lock taken, the reservation claimed */
    AT_CHANGED,     /* the hub changed, the reservation not yet ended */
} at_t;

typedef enum { REPLACE, UNBIND, REMAP } kind_t;

static sim_t E;
static zone_hub_t H;
static zsrc_rf_t SRC;
static zone_bind_gate_t G;
static bool RADIO;          /* a sensor radio's own hold (alarm_service_hold_arming) */
static bool FAIL_INVALIDATION;
static bool IDLE_STEP;      /* an engine step that doesn't arm runs between reserve and claim */
static bool IDLE_IN_STEP;   /* ...and overlaps the claim */
static at_t ARM_AT;
static uint8_t ARM_PART;
static alarm_result_t ARM_RESULT;
static bool ARM_TRIED;
static unsigned RETRIES;
static unsigned FAULTS[ALARM_MAX_ZONES + 1u];
static alarm_config_t SCRATCH;

/* ---- the alarm task ------------------------------------------------------------------------ */

/* What alarm_service does around every engine step (bind_step_begin, bind_step_end). */
static void step_begin(void)
{
    const bool bind = zone_bind_gate_step_begin(&G);
    alarm_engine_hold_arming(&E.eng, RADIO || bind);
}

static void step_end(void)
{
    zone_bind_gate_step_end(&G, zone_bind_gate_all_disarmed(&E.eng));
}

static alarm_result_t arm_in_step(uint8_t p)
{
    return sim_arm(&E, p, p == 1u ? CODE_USER : CODE_P2, ALARM_LEVEL_AWAY, 0u);
}

static alarm_result_t household_arm(uint8_t p)
{
    step_begin();
    const alarm_result_t r = arm_in_step(p);
    step_end();
    return r;
}

/* alarm_service_bind_reserve: one step on the alarm task. */
static bool reserve(void)
{
    step_begin();
    const bool ok = zone_bind_gate_reserve(&G, &E.eng, E.now);
    step_end();
    return ok;
}

/* ---- the hub, wired into the engine as zone_service wires it ------------------------------- */

static void port_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    if (ev == ALARM_ZEV_FAULT && zone <= ALARM_MAX_ZONES) {
        FAULTS[zone]++;
    }
    step_begin();
    sim_zev(&E, zone, ev);
    step_end();
}

static void port_changed(void *ctx)
{
    (void)ctx;
}

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t v,
                       uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H, a, state, v, now);
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

static void advance(uint64_t ms)
{
    const uint64_t until = E.now + ms;
    while (E.now < until) {
        const uint64_t step = until - E.now < 500u ? until - E.now : 500u;
        step_begin();
        sim_advance(&E, step);
        step_end();
        zone_hub_tick(&H, E.now);
    }
}

/* One press of the window's sensor: its frames, then the finger off. */
static void open_window(uint32_t device)
{
    const zsrc_rf_packet_t p = zsrc_rf_ev1527(CODE(device, OPEN));
    for (unsigned i = 0; i < 4u; i++) {
        zsrc_rf_packet(&SRC, &p, E.now);
        advance(40u);
    }
    advance(1000u);
}

static zone_hub_binding_t rf_binding(uint32_t device, uint16_t hold_s)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, device, OPEN);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_DOOR);
    b.in[ZSRC_ROLE_MAIN].map.hold_s = hold_s;
    return b;
}

static void boundary(at_t at)
{
    if (ARM_AT != at) {
        return;
    }
    ARM_TRIED = true;
    if (at == AT_LAPSED) {
        E.now += ZONE_BIND_GATE_MS; /* the changer waited this long for the zone lock */
    }
    if (at == AT_IN_STEP) {
        /* The changer claims while the alarm task runs the arm: told to retry; the arm goes. */
        step_begin();
        TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_RETRY, zone_bind_gate_claim(&G, E.now));
        RETRIES++;
        ARM_RESULT = arm_in_step(ARM_PART);
        step_end();
        return;
    }
    ARM_RESULT = household_arm(ARM_PART);
}

/* The spoken-name invalidation: one alarm-task transaction applying a changed configuration. */
static bool invalidate(uint16_t zone)
{
    if (FAIL_INVALIDATION) {
        return false;
    }
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    zones[(zone - 1u) / 8u] = (uint8_t)(1u << ((zone - 1u) % 8u));
    SCRATCH = *alarm_engine_config(&E.eng);
    (void)alarm_config_voice_invalidate_zones(&SCRATCH, zones);
    step_begin();
    const bool ok = alarm_engine_set_config(&E.eng, &SCRATCH, E.now) == ALARM_OK;
    step_end();
    return ok;
}

/* zone_service.c change(), for a task without the zone lock, step by step. */
static zone_hub_result_t change(uint16_t zone, const zone_hub_binding_t *b)
{
    if (!zone_bind_gate_all_disarmed(&E.eng)) {
        return ZONE_HUB_ERR_BUSY;
    }
    boundary(AT_ENTRY);
    boundary(AT_STORE_WAIT);
    if (!invalidate(zone)) {
        return ZONE_HUB_ERR_BUSY;
    }
    boundary(AT_INVALIDATED);
    if (!reserve()) {
        return ZONE_HUB_ERR_BUSY;
    }
    boundary(AT_LOCK_WAIT);
    if (IDLE_STEP) {
        advance(100u); /* a tick, a zone event: no arm, the reservation stands */
    }
    if (IDLE_IN_STEP) {
        step_begin();
        TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_RETRY, zone_bind_gate_claim(&G, E.now));
        RETRIES++;
        step_end();
    }
    boundary(AT_IN_STEP);
    boundary(AT_LAPSED);
    /* Under the zone lock. RETRY comes only while an engine step runs, which here is never. */
    const zone_bind_claim_t c = zone_bind_gate_claim(&G, E.now);
    TEST_ASSERT_NOT_EQUAL_INT(ZONE_BIND_CLAIM_RETRY, c);
    if (c != ZONE_BIND_CLAIMED) {
        zone_bind_gate_end(&G);
        return ZONE_HUB_ERR_BUSY;
    }
    boundary(AT_CLAIMED);
    zone_hub_result_t r = ZONE_HUB_OK;
    if (b != NULL) {
        r = zone_hub_bind(&H, zone, b, E.now);
    } else {
        zone_hub_unbind(&H, zone, E.now);
    }
    boundary(AT_CHANGED);
    zone_bind_gate_end(&G);
    return r;
}

static zone_hub_result_t change_kind(kind_t kind)
{
    zone_hub_binding_t b;
    switch (kind) {
    case REPLACE:
        b = rf_binding(DEV_B, 0u);
        return change(Z_WINDOW, &b);
    case REMAP:
        b = rf_binding(DEV_A, 30u); /* the same sensor, a new hold time */
        return change(Z_WINDOW, &b);
    case UNBIND:
    default:
        return change(Z_WINDOW, NULL);
    }
}

static void start(bool two_partitions)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    if (two_partitions) {
        cfg.partitions[1].enabled = true;
        sim_set_zone(&cfg, Z_P2, ALARM_ZT_PERIMETER, 2);
        sim_set_user(&cfg, 8, ALARM_AUTH_STANDARD, 0x02, CODE_P2);
    }
    sim_start(&E, &cfg, NULL);
    zone_bind_gate_init(&G);
    RADIO = false;
    FAIL_INVALIDATION = false;
    IDLE_STEP = false;
    IDLE_IN_STEP = false;
    ARM_AT = AT_NONE;
    ARM_PART = 1u;
    ARM_TRIED = false;
    ARM_RESULT = ALARM_OK;
    RETRIES = 0u;
    memset(FAULTS, 0, sizeof(FAULTS));

    const zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed};
    zone_hub_init(&H, &port);
    const zsrc_sink_t sink = {.ctx = NULL,
                              .input = sink_input,
                              .event = sink_event,
                              .reachable = sink_reachable,
                              .link = sink_link};
    zsrc_rf_init(&SRC, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_rf_ops(), &SRC, E.now));
    zone_hub_start(&H, E.now);
    zsrc_rf_receiver(&SRC, true, E.now);
    const zone_hub_binding_t b = rf_binding(DEV_A, 0u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, E.now));
    advance(1000u);
}

static zone_hub_binding_t window_binding(void)
{
    zone_hub_binding_t b;
    (void)zone_hub_get(&H, Z_WINDOW, &b); /* zeroed first: compared whole */
    return b;
}

void setUp(void)
{
    start(false);
}

void tearDown(void) {}

/* ---- tests --------------------------------------------------------------------------------- */

/* ZSRC-012, RF32: an ordinary replacement while everything is disarmed goes through, holds
 * nothing afterwards, and the new sensor is the zone's. */
static void test_a_disarmed_replacement_goes_through(void)
{
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, change_kind(REPLACE));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
    open_window(DEV_B);
    TEST_ASSERT_EQUAL_UINT(1u, FAULTS[Z_WINDOW]);
    open_window(DEV_A);
    TEST_ASSERT_EQUAL_UINT(1u, FAULTS[Z_WINDOW]); /* the old sensor is no longer the zone's */
    advance(SEC(60));                             /* the new sensor's automatic hold ends */
    TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(1u));
}

/* ZSRC-012, RF32: armed before the claim - at the entry snapshot, while waiting for the store
 * mutex, after the invalidation, while the reservation stands waiting for the zone lock, and in
 * an engine step that overlaps the claim. The arm goes ahead (a reservation never holds arming),
 * the change is refused, the binding is untouched and the old sensor still protects the zone once
 * armed. Replacement, unbind and a mapping edit. */
static void test_an_arm_before_the_claim_goes_ahead_and_refuses_the_change(void)
{
    const at_t ats[] = {AT_ENTRY, AT_STORE_WAIT, AT_INVALIDATED, AT_LOCK_WAIT, AT_IN_STEP};
    const kind_t kinds[] = {REPLACE, UNBIND, REMAP};
    for (unsigned a = 0; a < sizeof(ats) / sizeof(ats[0]); a++) {
        for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            start(false);
            const zone_hub_binding_t before = window_binding();
            ARM_AT = ats[a];
            TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_BUSY, change_kind(kinds[k]));
            TEST_ASSERT_TRUE(ARM_TRIED);
            TEST_ASSERT_EQUAL_INT(ALARM_OK, ARM_RESULT);
            TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, sim_status(&E, 1u).state);
            const zone_hub_binding_t after = window_binding();
            TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
            TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
            advance(SEC(61));
            TEST_ASSERT_EQUAL_INT(ALARM_STATE_ARMED, sim_status(&E, 1u).state);
            open_window(DEV_A);
            TEST_ASSERT_EQUAL_UINT(1u, FAULTS[Z_WINDOW]); /* still the zone's sensor */
        }
    }
}

/* ZSRC-012, RF32, CORE-ARM-013: armed once the change is claimed (the hub not yet changed, or
 * changed but not yet ended) - the arm is held, the change completes with every partition still
 * disarmed, and the next arm goes. */
static void test_an_arm_after_the_claim_is_held(void)
{
    const at_t ats[] = {AT_CLAIMED, AT_CHANGED};
    const kind_t kinds[] = {REPLACE, UNBIND, REMAP};
    for (unsigned a = 0; a < sizeof(ats) / sizeof(ats[0]); a++) {
        for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            start(false);
            const zone_hub_binding_t before = window_binding();
            ARM_AT = ats[a];
            TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, change_kind(kinds[k]));
            TEST_ASSERT_TRUE(ARM_TRIED);
            TEST_ASSERT_EQUAL_INT(ALARM_ERR_HELD, ARM_RESULT);
            TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, sim_status(&E, 1u).state);
            const zone_hub_binding_t after = window_binding();
            TEST_ASSERT_TRUE(memcmp(&before, &after, sizeof(before)) != 0);
            TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
            TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(1u)); /* the hold went with the change */
        }
    }
}

/* ZSRC-012, RF32: engine steps that don't arm (a tick, a zone event) between the reservation and
 * the claim leave it standing, and one that overlaps the claim only makes the changer retry: the
 * change goes through. A reservation holds nothing meanwhile. */
static void test_steps_without_an_arm_keep_the_reservation(void)
{
    IDLE_STEP = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, change_kind(REPLACE));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);

    start(false);
    IDLE_IN_STEP = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, change_kind(UNBIND));
    TEST_ASSERT_EQUAL_UINT(1u, RETRIES);

    start(false);
    TEST_ASSERT_TRUE(reserve());
    step_begin();
    TEST_ASSERT_FALSE(alarm_engine_arming_held(&E.eng)); /* reserved: nothing held */
    step_end();
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_RESERVED, G.state);
}

/* ZSRC-012, RF32: a changer that waits for the zone lock past ZONE_BIND_GATE_MS is refused - the
 * claim's retries are bounded - and arming was never held while it waited. */
static void test_a_late_claim_is_refused(void)
{
    const kind_t kinds[] = {REPLACE, UNBIND, REMAP};
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        start(false);
        const zone_hub_binding_t before = window_binding();
        ARM_AT = AT_LAPSED;
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_BUSY, change_kind(kinds[k]));
        TEST_ASSERT_EQUAL_INT(ALARM_OK, ARM_RESULT);
        TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, sim_status(&E, 1u).state);
        const zone_hub_binding_t after = window_binding();
        TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
        TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
    }
    /* Late with no arm: refused all the same, and a step running then can't bring it back. */
    start(false);
    TEST_ASSERT_TRUE(reserve());
    const uint64_t at = E.now;
    step_begin();
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_RETRY,
                          zone_bind_gate_claim(&G, at + ZONE_BIND_GATE_MS - 1u));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_REFUSED, zone_bind_gate_claim(&G, at + ZONE_BIND_GATE_MS));
    step_end();
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_REFUSED, zone_bind_gate_claim(&G, at));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_bind_gate_deadline(&G));
}

/* ZSRC-012, RF32: once claimed, the change never lapses (the race would come back): arming stays
 * held however long it takes, and a stall is reported once, at ZONE_BIND_GATE_STALL_MS - the
 * deadline the alarm task wakes for. */
static void test_a_stalled_change_holds_arming_and_is_reported_once(void)
{
    TEST_ASSERT_TRUE(reserve());
    const uint64_t at = E.now;
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_bind_gate_deadline(&G)); /* reserved: nothing due */
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIMED, zone_bind_gate_claim(&G, at));
    TEST_ASSERT_EQUAL_UINT64(at + ZONE_BIND_GATE_STALL_MS, zone_bind_gate_deadline(&G));
    TEST_ASSERT_FALSE(zone_bind_gate_stalled(&G, at + ZONE_BIND_GATE_STALL_MS - 1u));
    TEST_ASSERT_TRUE(zone_bind_gate_stalled(&G, at + ZONE_BIND_GATE_STALL_MS));
    TEST_ASSERT_FALSE(zone_bind_gate_stalled(&G, at + 2u * ZONE_BIND_GATE_STALL_MS)); /* once */
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zone_bind_gate_deadline(&G));
    E.now += 10u * ZONE_BIND_GATE_STALL_MS;
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_HELD, household_arm(1u));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_CHANGING, G.state);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_REFUSED, zone_bind_gate_claim(&G, E.now)); /* one */
    zone_bind_gate_end(&G);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(1u));
}

/* ZSRC-012, RF32: with two partitions, either one leaving disarmed counts - partition 2 armed
 * before the claim refuses a partition 1 window's change, armed after it is held. */
static void test_two_partitions(void)
{
    start(true);
    zone_hub_binding_t before = window_binding();
    ARM_AT = AT_LOCK_WAIT;
    ARM_PART = 2u;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_BUSY, change_kind(REPLACE));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, ARM_RESULT);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, sim_status(&E, 2u).state);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, sim_status(&E, 1u).state);
    zone_hub_binding_t after = window_binding();
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));

    /* Partition 2 armed and partition 1 disarmed: nothing reserves. */
    TEST_ASSERT_FALSE(reserve());
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_BUSY, change_kind(UNBIND));

    start(true);
    ARM_AT = AT_CLAIMED;
    ARM_PART = 2u;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, change_kind(UNBIND));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_HELD, ARM_RESULT);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, sim_status(&E, 2u).state);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(2u));
}

/* ZSRC-012, RF32: a refused invalidation refuses the change before anything is reserved, and
 * arming goes at once. */
static void test_a_failing_invalidation_holds_nothing(void)
{
    const zone_hub_binding_t before = window_binding();
    FAIL_INVALIDATION = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_BUSY, change_kind(REPLACE));
    const zone_hub_binding_t after = window_binding();
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(1u));
}

/* ZSRC-012, RF32: refused while armed from the start, at the reservation too (a stale entry
 * check), and only one reservation and one claim at a time. */
static void test_armed_or_reserved_refuses_a_reservation(void)
{
    TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(1u));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_BUSY, change_kind(UNBIND));
    TEST_ASSERT_FALSE(reserve()); /* in exit time */
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, G.state);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_disarm(&E, 1u, CODE_USER));

    TEST_ASSERT_TRUE(reserve());
    TEST_ASSERT_FALSE(reserve()); /* one at a time */
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIMED, zone_bind_gate_claim(&G, E.now));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_REFUSED, zone_bind_gate_claim(&G, E.now));
    zone_bind_gate_end(&G);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIM_REFUSED, zone_bind_gate_claim(&G, E.now)); /* ended */
    step_begin();
    TEST_ASSERT_FALSE(alarm_engine_arming_held(&E.eng));
    step_end();
}

/* ZSRC-012, ZB21: zone_bind_gate_try tells "armed" (the caller's "not allowed") from "busy"
 * (another change holds the gate: try again later), and reserves exactly as take does. */
static void test_try_tells_armed_from_busy(void)
{
    zone_bind_gate_t g;
    zone_bind_gate_init(&g);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_ARMED, zone_bind_gate_try(&g, false, 10u));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_IDLE, g.state);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_RESERVED, zone_bind_gate_try(&g, true, 10u));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_RESERVED, g.state);
    TEST_ASSERT_EQUAL_UINT64(10u, g.at);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_BUSY, zone_bind_gate_try(&g, true, 20u)); /* reserved */
    TEST_ASSERT_EQUAL_UINT64(10u, g.at);
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_ARMED, zone_bind_gate_try(&g, false, 20u)); /* armed wins */
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_CLAIMED, zone_bind_gate_claim(&g, 20u));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_BUSY, zone_bind_gate_try(&g, true, 30u)); /* changing */
    TEST_ASSERT_FALSE(zone_bind_gate_take(&g, true, 30u));
    TEST_ASSERT_EQUAL_INT(ZONE_BIND_GATE_CHANGING, g.state);
    zone_bind_gate_end(&g);
    TEST_ASSERT_TRUE(zone_bind_gate_take(&g, true, 40u));
}

/* CORE-ARM-013, RF32: a sensor radio's own hold outlives a binding change's: ending the change
 * never lets arming go while the radio still holds it. */
static void test_a_radio_hold_outlives_the_change(void)
{
    RADIO = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, change_kind(REPLACE));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_HELD, household_arm(1u));
    RADIO = false;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, household_arm(1u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_disarmed_replacement_goes_through);
    RUN_TEST(test_an_arm_before_the_claim_goes_ahead_and_refuses_the_change);
    RUN_TEST(test_an_arm_after_the_claim_is_held);
    RUN_TEST(test_steps_without_an_arm_keep_the_reservation);
    RUN_TEST(test_a_late_claim_is_refused);
    RUN_TEST(test_a_stalled_change_holds_arming_and_is_reported_once);
    RUN_TEST(test_two_partitions);
    RUN_TEST(test_a_failing_invalidation_holds_nothing);
    RUN_TEST(test_armed_or_reserved_refuses_a_reservation);
    RUN_TEST(test_a_radio_hold_outlives_the_change);
    RUN_TEST(test_try_tells_armed_from_busy);
    return UNITY_END();
}
