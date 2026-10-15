/* auto_watch (docs/features/41 DESIGN.md section 2): a real engine's events and snapshots in,
 * occurrences out. AUT-003: the observer's rules (B1-B6, latches, silent and duress); AUT-004:
 * the triggers it reports. */
#include "automation/auto_watch.h"
#include "automation/automation.h"
#include "sim.h"

#define OCC_MAX 64u

static sim_t S;
static auto_watch_t W;
static alarm_snapshot_t SNAP;
static unsigned SEEN;
static uint32_t CFGSEQ;
static auto_occ_t OCC[OCC_MAX];
static size_t NOCC;

/* As the listener does: each engine call's events, then the snapshot after it. */
static size_t pump_max(size_t max)
{
    size_t got = 0;
    while (SEEN < S.n_events) {
        const size_t room = OCC_MAX - NOCC < max - got ? OCC_MAX - NOCC : max - got;
        const size_t k = auto_watch_event(&W, &S.events[SEEN++], &OCC[NOCC], room);
        NOCC += k;
        got += k;
    }
    alarm_engine_snapshot(&S.eng, &SNAP);
    SNAP.config_seq = CFGSEQ;
    const size_t room = OCC_MAX - NOCC < max - got ? OCC_MAX - NOCC : max - got;
    const size_t k = auto_watch_snapshot(&W, &SNAP, &OCC[NOCC], room);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(room, k);
    NOCC += k;
    return got + k;
}

static size_t pump(void)
{
    return pump_max(OCC_MAX);
}

static unsigned count(uint8_t kind)
{
    unsigned n = 0;
    for (size_t i = 0; i < NOCC; i++) {
        n += OCC[i].kind == kind ? 1u : 0u;
    }
    return n;
}

static const auto_occ_t *last(uint8_t kind)
{
    for (size_t i = NOCC; i > 0; i--) {
        if (OCC[i - 1u].kind == kind) {
            return &OCC[i - 1u];
        }
    }
    TEST_FAIL_MESSAGE("no such occurrence");
    return NULL;
}

/* Start an engine (cfg NULL = the basic layout) and a watch with this settle time. */
static void boot(const alarm_config_t *cfg, const alarm_runtime_state_t *restore, uint32_t settle)
{
    sim_start(&S, cfg, restore);
    auto_watch_init(&W, S.now, settle);
    SEEN = 0;
    CFGSEQ = 1;
    NOCC = 0;
    memset(OCC, 0, sizeof(OCC));
}

static void clear_occ(void)
{
    NOCC = 0;
}

void setUp(void)
{
    boot(NULL, NULL, 0);
    pump();
}

void tearDown(void) {}

static void walk_op_in(uint8_t partition, alarm_walk_op_t op)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = partition;
    cmd.walk_op = (uint8_t)op;
    strncpy(cmd.code, CODE_MASTER, ALARM_CODE_MAX_LEN);
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &cmd));
}

static void walk_op(alarm_walk_op_t op)
{
    walk_op_in(1, op);
}

/* ---- B1 boot baseline ---------------------------------------------------------------------- */

/* AUT-003 B1: a zone already open at start is the baseline, not an opening; its close is seen. */
static void test_boot_baseline_with_zone_faulted(void)
{
    boot(NULL, NULL, 0);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_size_t(0, pump());
    pump();
    TEST_ASSERT_EQUAL_size_t(0, NOCC);
    sim_restore(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_CLOSED, OCC[0].kind);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, OCC[0].zone);
}

/* AUT-003 B1: an armed state restored at power-up makes no ARMED occurrence, now or later. */
static void test_restored_armed_state_at_start(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    const alarm_runtime_state_t saved = S.persisted;
    const alarm_config_t cfg = S.cfg;
    boot(&cfg, &saved, 0);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    pump();
    sim_advance(&S, SEC(120));
    pump();
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ARMED));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_DISARMED));
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ARMED));
}

/* ---- B2 settle ----------------------------------------------------------------------------- */

/* AUT-003 B2: no zone occurrences before start + settle; the first snapshot after it is the
 * baseline. */
static void test_settle_window(void)
{
    boot(NULL, NULL, 60000u);
    pump();
    sim_advance(&S, SEC(10));
    sim_fault(&S, Z_WINDOW);
    pump();
    sim_restore(&S, Z_WINDOW);
    pump();
    sim_fault(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_size_t(0, NOCC);
    sim_advance(&S, SEC(50)); /* exactly start + 60 s: the baseline, with the window open */
    TEST_ASSERT_EQUAL_size_t(0, pump());
    sim_restore(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_CLOSED, OCC[0].kind);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, OCC[0].zone);
}

/* ---- ARMED --------------------------------------------------------------------------------- */

/* AUT-004 ARMED: Away fires when the exit time ends, with the level and the snapshot's time. */
static void test_armed_away_after_exit_time(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    pump();
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ARMED));
    sim_fault(&S, Z_FRONT);
    pump();
    sim_restore(&S, Z_FRONT);
    pump();
    sim_advance(&S, SEC(61));
    pump();
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ARMED));
    const auto_occ_t *o = last(AUTO_TRIG_ARMED);
    TEST_ASSERT_EQUAL_UINT8(1, o->partition);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, o->level);
    TEST_ASSERT_TRUE(o->at == SNAP.taken_ms);
    sim_advance(&S, SEC(60));
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ARMED));
}

/* AUT-004 ARMED: with zero exit time the arm goes straight DISARMED -> ARMED and fires. */
static void test_armed_with_zero_exit_time(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.relaxed_limits = true;
    cfg.partitions[0].exit_delay_s = 0;
    boot(&cfg, NULL, 0);
    pump();
    S.keypad.kind = ALARM_SRC_REMOTE;
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ARMED));
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, last(AUTO_TRIG_ARMED)->level);
}

/* AUT-004 ARMED: an exit restart (ARMED -> EXIT_DELAY -> ARMED) doesn't fire again. */
static void test_exit_restart_fires_once(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    pump();
    sim_advance(&S, SEC(61));
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ARMED));
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, CODE_USER));
    pump();
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    sim_advance(&S, SEC(61));
    pump();
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ARMED));
}

/* AUT-004 ARMED: keypad Away that AUTO_STAY turned into Stay fires "armed Stay". */
static void test_armed_auto_stay_level(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    pump();
    sim_advance(&S, SEC(61));
    pump();
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ARMED));
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_STAY, last(AUTO_TRIG_ARMED)->level);
}

/* AUT-004 ARMED: re-arming Stay -> Away fires the new level once. */
static void test_rearm_stay_to_away(void)
{
    S.keypad.kind = ALARM_SRC_REMOTE; /* no auto-stay */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    pump();
    sim_advance(&S, SEC(61));
    pump();
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    pump();
    sim_advance(&S, SEC(61));
    pump();
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    TEST_ASSERT_EQUAL_UINT(2, count(AUTO_TRIG_ARMED));
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_STAY, OCC[0].level);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, last(AUTO_TRIG_ARMED)->level);
}

/* AUT-004 ARMED: entry expiry (ENTRY_DELAY -> ARMED, in alarm) is never an arming. */
static void test_entry_expiry_is_not_armed(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    pump();
    clear_occ();
    sim_fault(&S, Z_FRONT);
    pump();
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    sim_advance(&S, SEC(31));
    pump();
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ARMED));
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ALARM));
    TEST_ASSERT_EQUAL_UINT8(ALARM_CAUSE_BURGLARY, last(AUTO_TRIG_ALARM)->cause);
}

/* AUT-004 ARMED: entry expiry is never an arming even with no arming latch to stop it (the watch
 * started during entry time). */
static void test_entry_expiry_without_latch(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    auto_watch_init(&W, S.now, 0);
    SEEN = S.n_events;
    clear_occ();
    pump();
    sim_advance(&S, SEC(31));
    pump();
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ARMED));
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ALARM));
}

/* AUT-004 ARMED: the latch clears on DISARMED, so the next arming at the same level fires. */
static void test_rearm_same_level_after_disarm(void)
{
    S.keypad.kind = ALARM_SRC_REMOTE;
    for (unsigned i = 0; i < 2u; i++) {
        EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
        pump();
        sim_advance(&S, SEC(61));
        pump();
        EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
        pump();
    }
    TEST_ASSERT_EQUAL_UINT(2, count(AUTO_TRIG_ARMED));
    TEST_ASSERT_EQUAL_UINT(2, count(AUTO_TRIG_DISARMED));
}

/* AUT-003 B5: an edge whose old flags carried trouble is dropped even when the new ones are clean
 * (a sensor that comes back open hasn't opened). */
static void test_edge_after_trouble_dropped(void)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    SNAP.config_seq = CFGSEQ;
    static const uint16_t bad[] = {ALARM_ZS_TROUBLE, ALARM_ZS_TAMPER, ALARM_ZS_SUPERVISION};
    auto_occ_t o[2];
    for (size_t i = 0; i < 3u; i++) {
        const unsigned z = Z_WINDOW - 1u;
        SNAP.zone[z].flags = bad[i];
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_snapshot(&W, &SNAP, o, 2));
        SNAP.zone[z].flags = ALARM_ZS_FAULT;
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_snapshot(&W, &SNAP, o, 2));
        SNAP.zone[z].flags = (uint16_t)(ALARM_ZS_FAULT | bad[i]);
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_snapshot(&W, &SNAP, o, 2));
        SNAP.zone[z].flags = 0;
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_snapshot(&W, &SNAP, o, 2));
    }
    SNAP.zone[Z_WINDOW - 1u].flags = ALARM_ZS_FAULT | ALARM_ZS_LOW_BATTERY; /* not a trouble */
    TEST_ASSERT_EQUAL_size_t(1, auto_watch_snapshot(&W, &SNAP, o, 2));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, o[0].kind);
}

/* ---- DISARMED ------------------------------------------------------------------------------ */

/* AUT-004 DISARMED: a real disarm, with its partition and the event's time; a disarm of a
 * disarmed partition is none. */
static void test_disarmed(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    pump();
    sim_advance(&S, SEC(5));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_DISARMED));
    const auto_occ_t *o = last(AUTO_TRIG_DISARMED);
    TEST_ASSERT_EQUAL_UINT8(1, o->partition);
    TEST_ASSERT_TRUE(o->at == sim_last_event(&S, ALARM_EV_DISARMED)->mono_ms);
    sim_disarm(&S, 1, CODE_USER);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_DISARMED));
}

/* ---- ALARM / FIRE -------------------------------------------------------------------------- */

/* AUT-003, AUT-004 ALARM: once per episode, again after a disarm ends it. */
static void test_burglary_once_per_episode(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    pump();
    clear_occ();
    sim_fault(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ALARM));
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, last(AUTO_TRIG_ALARM)->zone);
    TEST_ASSERT_EQUAL_UINT8(1, last(AUTO_TRIG_ALARM)->partition);
    sim_fault(&S, Z_HALL_PIR);
    pump();
    sim_advance(&S, SEC(300)); /* the bell times out: still the same episode while armed */
    pump();
    sim_fault(&S, Z_FAMILY);
    pump();
    TEST_ASSERT_GREATER_THAN_UINT(1, sim_count_events(&S, ALARM_EV_ALARM));
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ALARM));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    pump();
    sim_restore(&S, Z_WINDOW);
    sim_restore(&S, Z_HALL_PIR);
    sim_restore(&S, Z_FAMILY);
    pump();
    sim_arm_away_and_leave(&S, CODE_USER);
    pump();
    sim_fault(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_UINT(2, count(AUTO_TRIG_ALARM));
}

/* AUT-003 silent: silent burglary, 24-hour silent and silent panic never fire. */
static void test_silent_alarms_never(void)
{
    sim_fault(&S, Z_HOLDUP);
    pump();
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_SILENT));
    pump();
    sim_arm_away_and_leave(&S, CODE_USER);
    pump();
    sim_fault(&S, Z_SHED);
    pump();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(3, sim_count_events(&S, ALARM_EV_ALARM));
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ALARM));
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_FIRE));
}

/* AUT-003: the cause decides even when the silent flag is missing; the flag refuses an audible
 * cause; keypad-only and unknown causes never fire. */
static void test_alarm_cause_allowlist(void)
{
    static const uint8_t refused[] = {ALARM_CAUSE_NONE, ALARM_CAUSE_SILENT_BURGLARY,
                                      ALARM_CAUSE_24H_SILENT, ALARM_CAUSE_24H_AUX,
                                      ALARM_CAUSE_PANIC_SILENT, ALARM_CAUSE_PANIC_MEDICAL,
                                      ALARM_CAUSE_COUNT, 200u};
    auto_occ_t o[2];
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ALARM_EV_ALARM;
    ev.partition = 2;
    for (size_t i = 0; i < sizeof(refused); i++) {
        ev.arg = refused[i];
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
    }
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.flags = ALARM_EVF_SILENT;
    TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
    ev.flags = ALARM_EVF_REPORT;
    static const uint8_t alarm[] = {ALARM_CAUSE_BURGLARY, ALARM_CAUSE_EXIT_ERROR,
                                    ALARM_CAUSE_TAMPER, ALARM_CAUSE_24H_AUDIBLE,
                                    ALARM_CAUSE_PANIC_POLICE};
    static const uint8_t fire[] = {ALARM_CAUSE_FIRE, ALARM_CAUSE_PANIC_FIRE, ALARM_CAUSE_CO};
    for (size_t i = 0; i < sizeof(alarm); i++) {
        auto_watch_init(&W, 0, 0);
        ev.arg = alarm[i];
        TEST_ASSERT_EQUAL_size_t(1, auto_watch_event(&W, &ev, o, 2));
        TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ALARM, o[0].kind);
        TEST_ASSERT_EQUAL_UINT8(alarm[i], o[0].cause);
        TEST_ASSERT_EQUAL_UINT8(2, o[0].partition);
    }
    for (size_t i = 0; i < sizeof(fire); i++) {
        auto_watch_init(&W, 0, 0);
        ev.arg = fire[i];
        TEST_ASSERT_EQUAL_size_t(1, auto_watch_event(&W, &ev, o, 2));
        TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_FIRE, o[0].kind);
    }
    /* Duress is never an occurrence. */
    ev.type = ALARM_EV_DURESS;
    ev.flags = 0;
    TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
}

/* AUT-003, AUT-004: DISARMED and ALARM events with partition 0 or past ALARM_MAX_PARTITIONS make no
 * occurrence and count nothing (an ALARM without a partition would skip the episode latch). */
static void test_events_without_valid_partition_refused(void)
{
    static const uint8_t bad[] = {0u, (uint8_t)(ALARM_MAX_PARTITIONS + 1u), 255u};
    auto_occ_t o[2];
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    for (size_t i = 0; i < sizeof(bad); i++) {
        ev.partition = bad[i];
        ev.type = ALARM_EV_DISARMED;
        ev.arg = 0;
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
        ev.type = ALARM_EV_ALARM;
        ev.arg = ALARM_CAUSE_BURGLARY;
        for (unsigned k = 0; k < 3u; k++) {
            TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
        }
        ev.arg = ALARM_CAUSE_FIRE;
        TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
    }
    TEST_ASSERT_EQUAL_UINT32(0, W.missed);
    /* the edges themselves still work */
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.partition = ALARM_MAX_PARTITIONS;
    TEST_ASSERT_EQUAL_size_t(1, auto_watch_event(&W, &ev, o, 2));
    TEST_ASSERT_EQUAL_UINT8(ALARM_MAX_PARTITIONS, o[0].partition);
    TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 2));
    ev.type = ALARM_EV_DISARMED;
    ev.partition = 1;
    TEST_ASSERT_EQUAL_size_t(1, auto_watch_event(&W, &ev, o, 2));
}

/* AUT-004 FIRE: a smoke alarm fires once while the sounder goes, CO after it is quiet again. */
static void test_fire_and_co(void)
{
    sim_fault(&S, Z_SMOKE);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_FIRE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_CAUSE_FIRE, last(AUTO_TRIG_FIRE)->cause);
    TEST_ASSERT_EQUAL_UINT16(Z_SMOKE, last(AUTO_TRIG_FIRE)->zone);
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ALARM));
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_FIRE));
    pump(); /* same episode: the fire sounder still goes in a disarmed partition */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, sim_count_events(&S, ALARM_EV_ALARM));
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_FIRE));
    sim_restore(&S, Z_SMOKE);
    sim_disarm(&S, 1, CODE_USER);
    pump();
    TEST_ASSERT_FALSE(alarm_snapshot_locked(&SNAP));
    sim_fault(&S, Z_CO);
    pump();
    TEST_ASSERT_EQUAL_UINT(2, count(AUTO_TRIG_FIRE));
    TEST_ASSERT_EQUAL_UINT8(ALARM_CAUSE_CO, last(AUTO_TRIG_FIRE)->cause);
}

/* ---- Duress -------------------------------------------------------------------------------- */

static size_t duress_run(const char *code, auto_occ_t *got)
{
    boot(NULL, NULL, 0);
    pump();
    S.keypad.kind = ALARM_SRC_REMOTE;
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, code, ALARM_LEVEL_AWAY, 0));
    pump();
    sim_advance(&S, SEC(61));
    pump();
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, code));
    pump();
    memcpy(got, OCC, NOCC * sizeof(OCC[0]));
    return NOCC;
}

/* AUT-003 duress: a duress arm and disarm produce exactly what a normal one does. */
static void test_duress_identical_to_normal(void)
{
    auto_occ_t normal[8], duress[8];
    const size_t nn = duress_run(CODE_USER, normal);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_events(&S, ALARM_EV_DURESS));
    const size_t nd = duress_run(CODE_DURESS, duress);
    TEST_ASSERT_EQUAL_UINT(2, sim_count_events(&S, ALARM_EV_DURESS));
    TEST_ASSERT_EQUAL_size_t(2, nn);
    TEST_ASSERT_EQUAL_size_t(nn, nd);
    for (size_t i = 0; i < nn; i++) {
        TEST_ASSERT_EQUAL_UINT8(normal[i].kind, duress[i].kind);
        TEST_ASSERT_EQUAL_UINT8(normal[i].partition, duress[i].partition);
        TEST_ASSERT_EQUAL_UINT8(normal[i].level, duress[i].level);
        TEST_ASSERT_EQUAL_UINT8(normal[i].cause, duress[i].cause);
        TEST_ASSERT_EQUAL_UINT16(normal[i].zone, duress[i].zone);
    }
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ARMED, duress[0].kind);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_DISARMED, duress[1].kind);
}

/* ---- Zones --------------------------------------------------------------------------------- */

/* AUT-004 ZONE_OPENED / ZONE_CLOSED: one edge per snapshot, each seen, any zone type. */
static void test_zone_open_and_close_edges(void)
{
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, OCC[0].kind);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, OCC[0].zone);
    TEST_ASSERT_TRUE(OCC[0].at == SNAP.taken_ms);
    sim_restore(&S, Z_WINDOW); /* a fast close: the very next engine call */
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_CLOSED, OCC[1].kind);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, OCC[1].zone);
    sim_fault(&S, Z_DOORBELL); /* the runner filters by type, not the watch */
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT16(Z_DOORBELL, OCC[2].zone);
    pump();
    TEST_ASSERT_EQUAL_size_t(3, NOCC);
}

/* AUT-003 B5: edges next to trouble, tamper or supervision trouble are dropped. */
static void test_trouble_and_tamper_edges_dropped(void)
{
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE);
    pump();
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE_RESTORE);
    pump();
    sim_zev(&S, Z_HALL_PIR, ALARM_ZEV_TAMPER);
    pump();
    sim_fault(&S, Z_HALL_PIR);
    pump();
    EXPECT_ZONE_FLAG(&S, Z_HALL_PIR, ALARM_ZS_FAULT);
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_UNREACHABLE);
    pump();
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_REACHABLE);
    pump();
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_OPENED));
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_CLOSED));
    /* Healthy again: the next real edge counts. */
    sim_fault(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ZONE_OPENED));
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, last(AUTO_TRIG_ZONE_OPENED)->zone);
}

/* AUT-003 B4: a walk test holds zone occurrences and re-baselines when it ends. */
static void test_walk_test_suppresses_and_rebaselines(void)
{
    walk_op(ALARM_WALK_START);
    pump();
    TEST_ASSERT_TRUE(SNAP.part[0].walk_test);
    sim_fault(&S, Z_WINDOW);
    pump();
    sim_restore(&S, Z_WINDOW);
    pump();
    sim_fault(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_size_t(0, NOCC);
    walk_op(ALARM_WALK_END);
    pump(); /* the window, still open, is the new baseline */
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_OPENED));
    sim_restore(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ZONE_CLOSED));
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_OPENED));
}

/* AUT-003 B4: the snapshot carries no zone partition, so a walk test in partition 2 holds a
 * partition-1 door too, and re-baselines it when the test ends. */
static void test_walk_test_in_other_partition_holds_every_zone(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_PERIMETER, 2);
    boot(&cfg, NULL, 0);
    pump();
    walk_op_in(2, ALARM_WALK_START);
    pump();
    TEST_ASSERT_TRUE(SNAP.part[1].walk_test);
    TEST_ASSERT_FALSE(SNAP.part[0].walk_test);
    sim_fault(&S, Z_FRONT); /* partition 1 */
    pump();
    sim_restore(&S, Z_FRONT);
    pump();
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_OPENED));
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_CLOSED));
    walk_op_in(2, ALARM_WALK_END);
    TEST_ASSERT_EQUAL_size_t(0, pump()); /* the first snapshot after it is a baseline */
    TEST_ASSERT_FALSE(SNAP.part[1].walk_test);
    TEST_ASSERT_EQUAL_size_t(0, NOCC);
    sim_fault(&S, Z_FRONT);
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, OCC[0].kind);
    TEST_ASSERT_EQUAL_UINT16(Z_FRONT, OCC[0].zone);
    TEST_ASSERT_EQUAL_UINT8(0, OCC[0].partition); /* the runner reads it from the configuration */
}

/* AUT-003 B4: installer mode holds zone occurrences and re-baselines when it ends. */
static void test_installer_mode_suppresses_and_rebaselines(void)
{
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, NULL));
    pump();
    TEST_ASSERT_TRUE(SNAP.program);
    sim_fault(&S, Z_WINDOW);
    pump();
    sim_fault(&S, Z_HALL_PIR);
    pump();
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_OPENED));
    alarm_engine_program_exit(&S.eng, &S.keypad, S.now);
    pump();
    TEST_ASSERT_EQUAL_UINT(0, count(AUTO_TRIG_ZONE_OPENED));
    sim_restore(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, count(AUTO_TRIG_ZONE_CLOSED));
}

/* AUT-003 B3: a configuration change re-baselines every zone. */
static void test_config_seq_rebaselines(void)
{
    sim_fault(&S, Z_WINDOW);
    pump();
    clear_occ();
    sim_restore(&S, Z_WINDOW);
    CFGSEQ++;
    TEST_ASSERT_EQUAL_size_t(0, pump());
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_size_t(1, pump());
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, OCC[0].kind);
}

/* AUT-003 B3: on_zones_reset re-baselines the named zones only; NULL changes nothing. */
static void test_zones_reset_bitmap(void)
{
    sim_fault(&S, Z_WINDOW);
    pump();
    clear_occ();
    uint8_t bits[ALARM_ZONE_BITMAP_BYTES];
    memset(bits, 0, sizeof(bits));
    bits[(Z_WINDOW - 1u) / 8u] = (uint8_t)(1u << ((Z_WINDOW - 1u) % 8u));
    auto_watch_zones_reset(&W, bits);
    auto_watch_zones_reset(&W, NULL);
    sim_restore(&S, Z_WINDOW);
    pump();
    sim_fault(&S, Z_HALL_PIR); /* not named: still reported */
    pump();
    TEST_ASSERT_EQUAL_size_t(1, NOCC);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, OCC[0].kind);
    TEST_ASSERT_EQUAL_UINT16(Z_HALL_PIR, OCC[0].zone);
    sim_fault(&S, Z_WINDOW);
    pump();
    TEST_ASSERT_EQUAL_UINT(2, count(AUTO_TRIG_ZONE_OPENED));
}

/* AUT-003 B6: never more than max; the rest is counted in missed, and NULL out writes none. */
static void test_max_overflow_counted_in_missed(void)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    SNAP.config_seq = CFGSEQ;
    SNAP.zone[Z_WINDOW - 1u].flags |= ALARM_ZS_FAULT;
    SNAP.zone[Z_HALL_PIR - 1u].flags |= ALARM_ZS_FAULT;
    SNAP.zone[Z_FAMILY - 1u].flags |= ALARM_ZS_FAULT;
    auto_occ_t o[3];
    memset(o, 0, sizeof(o));
    TEST_ASSERT_EQUAL_UINT32(0, W.missed);
    TEST_ASSERT_EQUAL_size_t(1, auto_watch_snapshot(&W, &SNAP, o, 1));
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, o[0].zone);
    TEST_ASSERT_EQUAL_UINT16(0, o[1].zone);
    TEST_ASSERT_EQUAL_UINT32(2, W.missed);
    SNAP.zone[Z_WINDOW - 1u].flags = 0;
    TEST_ASSERT_EQUAL_size_t(0, auto_watch_snapshot(&W, &SNAP, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(3, W.missed);
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ALARM_EV_DISARMED;
    ev.partition = 1;
    TEST_ASSERT_EQUAL_size_t(0, auto_watch_event(&W, &ev, o, 0));
    TEST_ASSERT_EQUAL_UINT32(4, W.missed);
    /* Through the engine, too: three edges in one snapshot, room for two. */
    sim_restore(&S, Z_WINDOW);
    pump();
    W.missed = 0;
    clear_occ();
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_UPSTAIRS);
    sim_fault(&S, Z_SHED);
    TEST_ASSERT_EQUAL_size_t(2, pump_max(2));
    TEST_ASSERT_EQUAL_UINT32(1, W.missed);
    /* The state stays small for the alarm task. */
    TEST_ASSERT_LESS_THAN_UINT(128u + 3u * ALARM_ZONE_BITMAP_BYTES, sizeof(auto_watch_t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_baseline_with_zone_faulted);
    RUN_TEST(test_restored_armed_state_at_start);
    RUN_TEST(test_settle_window);
    RUN_TEST(test_armed_away_after_exit_time);
    RUN_TEST(test_armed_with_zero_exit_time);
    RUN_TEST(test_exit_restart_fires_once);
    RUN_TEST(test_armed_auto_stay_level);
    RUN_TEST(test_rearm_stay_to_away);
    RUN_TEST(test_entry_expiry_is_not_armed);
    RUN_TEST(test_entry_expiry_without_latch);
    RUN_TEST(test_rearm_same_level_after_disarm);
    RUN_TEST(test_edge_after_trouble_dropped);
    RUN_TEST(test_disarmed);
    RUN_TEST(test_burglary_once_per_episode);
    RUN_TEST(test_silent_alarms_never);
    RUN_TEST(test_alarm_cause_allowlist);
    RUN_TEST(test_events_without_valid_partition_refused);
    RUN_TEST(test_fire_and_co);
    RUN_TEST(test_duress_identical_to_normal);
    RUN_TEST(test_zone_open_and_close_edges);
    RUN_TEST(test_trouble_and_tamper_edges_dropped);
    RUN_TEST(test_walk_test_suppresses_and_rebaselines);
    RUN_TEST(test_walk_test_in_other_partition_holds_every_zone);
    RUN_TEST(test_installer_mode_suppresses_and_rebaselines);
    RUN_TEST(test_config_seq_rebaselines);
    RUN_TEST(test_zones_reset_bitmap);
    RUN_TEST(test_max_overflow_counted_in_missed);
    return UNITY_END();
}
