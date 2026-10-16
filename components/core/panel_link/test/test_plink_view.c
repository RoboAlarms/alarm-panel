/*
 * test_plink_view.c - the panel link's status on both sides and the keypads' configuration copy
 * (plink_view.h): the master's STATUS built from snapshots of the real engine (alarm_sim) with
 * every duration worked out when it is sealed; the keypad's snapshot rebuilt on its own clock -
 * a countdown that never goes up whatever the network's delay does, the keypad's own lockout,
 * the walk test's start mapped once per test, the partitions mask, the zones and their CRC (made
 * independently by vectors_plink_view.py); the configuration copy with no code material, duress
 * shown as standard, and its lenient decode.
 * Requirements: LINK-002, LINK-008, LINK-019 (docs/features/11); UI-023's countdowns
 * (docs/features/11 "STATUS")
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_view.h"
#include "sim.h"

#define KP_CLOCK 5000000u /* the keypad's clock reads this much more than the master's */

static sim_t S;
static alarm_snapshot_t SNAP;
static plink_view_t V;
static plink_status_t ST;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
    plink_view_init(&V);
    memset(&SNAP, 0, sizeof(SNAP));
    memset(&ST, 0, sizeof(ST));
}

void tearDown(void) {}

static void take_snap(void)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
}

static plink_status_extra_t extra(void)
{
    plink_status_extra_t x;
    memset(&x, 0, sizeof(x));
    x.epoch_ms = 1790000000000LL;
    x.home = 1u;
    x.report = PLINK_REPORT_ONLINE;
    x.cfg_crc = 0xC0FFEE11u;
    x.cfg_len = 1234u;
    return x;
}

/* What the keypad's UI counts down at local time t (ui_app.c update_countdown): the delay left
 * from the snapshot's taken_ms. */
static uint64_t shown_ms(const plink_view_t *v, uint8_t p, uint64_t t)
{
    const alarm_partition_status_t *ps = &v->snap.part[p - 1u];
    const uint64_t gone = t > v->snap.taken_ms ? t - v->snap.taken_ms : 0u;
    return ps->delay_remaining_ms > gone ? ps->delay_remaining_ms - gone : 0u;
}

static uint64_t shown_walk_ms(const plink_view_t *v, uint8_t p, uint64_t t)
{
    const alarm_partition_status_t *ps = &v->snap.part[p - 1u];
    const uint64_t gone = t > v->snap.taken_ms ? t - v->snap.taken_ms : 0u;
    return ps->walk_test_remaining_ms > gone ? ps->walk_test_remaining_ms - gone : 0u;
}

static alarm_result_t walk(const char *code, alarm_walk_op_t op)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = 1u;
    cmd.walk_op = (uint8_t)op;
    strncpy(cmd.code, code, ALARM_CODE_MAX_LEN);
    return sim_cmd(&S, &cmd);
}

/* A 7-byte zone record written by hand from the document (docs/features/11 "Zone record"), not
 * by the code under test. */
static void zone_rec(const alarm_zone_status_t *z, uint8_t *out)
{
    out[0] = (uint8_t)(z->flags & 0xFFu);
    out[1] = (uint8_t)(z->flags >> 8);
    out[2] = z->trouble;
    out[3] = z->walk;
    out[4] = (uint8_t)(z->walk_first_s & 0xFFu);
    out[5] = (uint8_t)(z->walk_first_s >> 8);
    out[6] = z->walk_trips;
}

static void expect_zone(const alarm_zone_status_t *want, const alarm_zone_status_t *got)
{
    TEST_ASSERT_EQUAL_HEX16(want->flags, got->flags);
    TEST_ASSERT_EQUAL_HEX8(want->trouble, got->trouble);
    TEST_ASSERT_EQUAL_HEX8(want->walk, got->walk);
    TEST_ASSERT_EQUAL_UINT16(want->walk_first_s, got->walk_first_s);
    TEST_ASSERT_EQUAL_UINT8(want->walk_trips, got->walk_trips);
}

static bool zone_zero(const alarm_zone_status_t *z)
{
    return z->flags == 0u && z->trouble == 0u && z->walk == 0u && z->walk_first_s == 0u &&
           z->walk_trips == 0u;
}

/* ---- the master's STATUS ------------------------------------------------------------------- */

/* LINK-008, LINK-002: the master's STATUS carries the snapshot with every duration worked out at
 * the moment it is sealed, never below 0 - a frame never presents an old count as current. */
static void test_status_build_works_out_durations(void)
{
    /* The power-up recovery window, open at the start */
    take_snap();
    TEST_ASSERT_TRUE(SNAP.recovery_left_ms > 10000u);
    const plink_status_extra_t x = extra();
    plink_status_build(&SNAP, SNAP.taken_ms + 10000u, &x, &ST);
    TEST_ASSERT_EQUAL_UINT32(SNAP.recovery_left_ms - 10000u, ST.recovery_left_ms);
    plink_status_build(&SNAP, SNAP.taken_ms + SEC(600), &x, &ST);
    TEST_ASSERT_EQUAL_UINT32(0u, ST.recovery_left_ms);

    /* An entry delay */
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    sim_advance(&S, 500u);
    take_snap();
    const uint32_t d = (uint32_t)SNAP.part[0].delay_remaining_ms;
    TEST_ASSERT_EQUAL_UINT32(SEC(30) - 500u, d);

    plink_status_build(&SNAP, SNAP.taken_ms + 1234u, &x, &ST);
    TEST_ASSERT_EQUAL_UINT32(d - 1234u, ST.part[0].delay_left_ms);
    TEST_ASSERT_EQUAL_UINT8(1u, ST.count);
    TEST_ASSERT_EQUAL_HEX8(0x01u, ST.partitions);
    const plink_part_t *r = &ST.part[0];
    TEST_ASSERT_EQUAL_UINT8(1u, r->number);
    TEST_ASSERT_EQUAL_UINT8(ALARM_STATE_ENTRY_DELAY, r->state);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, r->level);
    TEST_ASSERT_EQUAL_UINT8(3u, r->armed_by);
    TEST_ASSERT_EQUAL_UINT8(SNAP.part[0].arm_source.kind, r->arm_src_kind);
    TEST_ASSERT_EQUAL_UINT16(SNAP.part[0].arm_source.id, r->arm_src_id);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_ENTRY, r->sounder);
    TEST_ASSERT_EQUAL_UINT16(SNAP.part[0].faults, r->faults);
    TEST_ASSERT_TRUE(r->faults >= 1u);
    TEST_ASSERT_EQUAL_UINT16(SNAP.part[0].troubles, r->troubles);
    TEST_ASSERT_EQUAL_UINT8(SNAP.part[0].ready ? PLINK_PF_READY : 0u, r->flags & PLINK_PF_READY);
    TEST_ASSERT_EQUAL_UINT32(0u, r->walk_id); /* no walk test since the restart */
    TEST_ASSERT_EQUAL_UINT32(0u, r->walk_age_ms);

    /* The system section and what the master adds */
    TEST_ASSERT_TRUE(ST.epoch_ms == x.epoch_ms);
    TEST_ASSERT_EQUAL_UINT8(1u, ST.home);
    TEST_ASSERT_EQUAL_UINT8(PLINK_REPORT_ONLINE, ST.report);
    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE11u, ST.cfg_crc);
    TEST_ASSERT_EQUAL_UINT32(1234u, ST.cfg_len);
    TEST_ASSERT_EQUAL_UINT16(20u, ST.zones_used);
    TEST_ASSERT_EQUAL_UINT32(SNAP.last_event_seq, ST.last_event_seq);
    TEST_ASSERT_EQUAL_HEX32(SNAP.sys_troubles, ST.sys_troubles);
    TEST_ASSERT_EQUAL_HEX8(PLINK_SF_EXIT_SOUNDS, ST.flags);
    TEST_ASSERT_EQUAL_HEX32(plink_zones_crc(SNAP.zone, SNAP.zones_used), ST.zones_crc);

    /* Sealed after the delay ran out: 0, never below */
    plink_status_build(&SNAP, SNAP.taken_ms + SEC(45), &x, &ST);
    TEST_ASSERT_EQUAL_UINT32(0u, ST.part[0].delay_left_ms);
    /* A clock behind the snapshot takes no time off */
    plink_status_build(&SNAP, SNAP.taken_ms - 5u, &x, &ST);
    TEST_ASSERT_EQUAL_UINT32(d, ST.part[0].delay_left_ms);

    /* No extra: zeros there; no snapshot: an empty status */
    plink_status_build(&SNAP, SNAP.taken_ms, NULL, &ST);
    TEST_ASSERT_EQUAL_UINT8(0u, ST.home);
    TEST_ASSERT_EQUAL_UINT32(0u, ST.cfg_crc);
    TEST_ASSERT_EQUAL_UINT8(1u, ST.count);
    plink_status_build(NULL, SNAP.taken_ms, &x, &ST);
    TEST_ASSERT_EQUAL_UINT8(0u, ST.count);
    TEST_ASSERT_EQUAL_HEX8(0u, ST.partitions);
    plink_status_build(&SNAP, SNAP.taken_ms, &x, NULL); /* nothing to fill: no crash */
}

/* ---- the keypad's view --------------------------------------------------------------------- */

/* LINK-002, UI-023: an entry delay as the keypad shows it. The master seals STATUS frames from
 * fresh snapshots and from repeated older ones; each reaches the keypad's own clock after a delay
 * that varies (1-80 ms), in order. Sampled every 10 ms the countdown never goes up - not even by
 * a millisecond when a frame comes faster than the last - never shows less than the master
 * has left, and lags it by no more than the network's delay. */
static void test_keypad_countdown_never_goes_up(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    const uint64_t end = S.now + SEC(30); /* when the master's entry time runs out */
    const plink_status_extra_t x = extra();
    take_snap();

    uint32_t rng = 12345u;
    uint64_t last_k = 0u, last_ms = UINT64_MAX;
    unsigned frames = 0u, rises = 0u, samples = 0u;
    while (S.now < end + SEC(2)) {
        rng = rng * 1103515245u + 12345u;
        sim_advance(&S, 50u + (rng >> 16) % 700u);
        if (((rng >> 8) & 3u) != 0u) {
            take_snap(); /* most frames carry a fresh snapshot, some repeat the last one */
        }
        plink_status_build(&SNAP, S.now, &x, &ST);
        uint64_t k = S.now + KP_CLOCK + 1u + (rng >> 20) % 80u;
        if (k <= last_k) {
            k = last_k + 1u;
        }
        for (uint64_t t = last_k; frames > 0u && t < k; t += 10u) {
            if (V.snap.part[0].state == ALARM_STATE_ENTRY_DELAY) {
                const uint64_t ms = shown_ms(&V, 1, t);
                TEST_ASSERT_TRUE(ms <= last_ms);
                const uint64_t master_left = end > t - KP_CLOCK ? end - (t - KP_CLOCK) : 0u;
                TEST_ASSERT_TRUE(ms >= master_left);
                TEST_ASSERT_TRUE(ms <= master_left + 80u);
                last_ms = ms;
                samples++;
            }
        }
        if (V.have_status && ST.part[0].state == ALARM_STATE_ENTRY_DELAY &&
            ST.part[0].delay_left_ms > shown_ms(&V, 1, k)) {
            rises++; /* sent longer than what is showing: the network was faster this time */
        }
        plink_view_apply_status(&V, &ST, k);
        if (V.snap.part[0].state == ALARM_STATE_ENTRY_DELAY) {
            const uint64_t ms = shown_ms(&V, 1, k);
            TEST_ASSERT_TRUE(ms <= last_ms);
            last_ms = ms;
        }
        last_k = k;
        frames++;
    }
    TEST_ASSERT_TRUE(frames > 40u);
    TEST_ASSERT_TRUE(samples > 2000u);
    TEST_ASSERT_TRUE(rises > 5u); /* the case the rule exists for happened */
    TEST_ASSERT_NOT_EQUAL(ALARM_STATE_ENTRY_DELAY, V.snap.part[0].state); /* it ended */
}

/* LINK-002, UI-023: the rule's edges. Within one delay a frame that reads a little longer than
 * what is showing (the network's delay varied) keeps the count; an exit restart, a new state, a
 * walk test extended or a new lockout is taken as sent. */
static void test_countdown_rises_only_for_real(void)
{
    plink_status_t st;
    memset(&st, 0, sizeof(st));
    st.partitions = 0x01u;
    st.count = 1u;
    plink_part_t *r = &st.part[0];
    r->number = 1u;
    r->state = ALARM_STATE_EXIT_DELAY;
    r->delay_left_ms = 40000u;
    plink_view_apply_status(&V, &st, 1000u);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 1000u) == 40000u);
    r->delay_left_ms = 39050u; /* 50 ms more than showing at 2000 */
    plink_view_apply_status(&V, &st, 2000u);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 2000u) == 39000u);
    r->delay_left_ms = 37900u; /* less: taken */
    plink_view_apply_status(&V, &st, 3000u);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 3000u) == 37900u);
    r->delay_left_ms = 60000u; /* the exit time restarted */
    plink_view_apply_status(&V, &st, 4000u);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 4000u) == 60000u);
    r->state = ALARM_STATE_ENTRY_DELAY; /* another state: taken, even a little longer */
    r->delay_left_ms = 59500u;
    plink_view_apply_status(&V, &st, 5000u);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 5000u) == 59500u);
    r->state = ALARM_STATE_ARMED;
    r->delay_left_ms = 0u;
    plink_view_apply_status(&V, &st, 6000u);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 6000u) == 0u);

    /* The walk test's time left */
    r->flags = PLINK_PF_WALK_TEST;
    r->walk_left_ms = 3600000u;
    plink_view_apply_status(&V, &st, 10000u);
    r->walk_left_ms = 3599300u; /* 300 ms more than showing at 11000 */
    plink_view_apply_status(&V, &st, 11000u);
    TEST_ASSERT_TRUE(shown_walk_ms(&V, 1, 11000u) == 3599000u);
    r->walk_left_ms = 3600000u; /* extended: the whole limit again */
    plink_view_apply_status(&V, &st, 12000u);
    TEST_ASSERT_TRUE(shown_walk_ms(&V, 1, 12000u) == 3600000u);

    /* The keypad's own lockout */
    st.lockout_left_ms = 300000u;
    plink_view_apply_status(&V, &st, 20000u);
    TEST_ASSERT_TRUE(V.snap.local_lockout_until == 320000u);
    st.lockout_left_ms = 299100u; /* ends 100 ms later than the last said */
    plink_view_apply_status(&V, &st, 21000u);
    TEST_ASSERT_TRUE(V.snap.local_lockout_until == 320000u);
    st.lockout_left_ms = 0u;
    plink_view_apply_status(&V, &st, 22000u);
    TEST_ASSERT_TRUE(V.snap.local_lockout_until == 0u);
    st.lockout_left_ms = 300000u; /* locked again: a new lockout */
    plink_view_apply_status(&V, &st, 23000u);
    TEST_ASSERT_TRUE(V.snap.local_lockout_until == 323000u);
}

/* LINK-002, KPD-008: the keypad's own lockout comes from the master (alarm_engine_lockout_left
 * for that keypad, in the extra) and shows as an end on the keypad's clock; the touchscreen's
 * lockout at the master is not the keypad's. */
static void test_keypad_lockout(void)
{
    S.keypad.id = 3u; /* commands from link keypad 3 */
    for (unsigned i = 0; i < 5u; i++) {
        (void)sim_disarm(&S, 1, "0000");
    }
    const alarm_source_t kp3 = {ALARM_SRC_KEYPAD, 3u};
    const alarm_ms_t left = alarm_engine_lockout_left(&S.eng, &kp3, S.now);
    TEST_ASSERT_TRUE(left > 0u);
    take_snap();
    TEST_ASSERT_TRUE(SNAP.local_lockout_until == 0u); /* the master's own screen: not locked */

    plink_status_extra_t x = extra();
    x.lockout_left_ms = (uint32_t)left;
    plink_status_build(&SNAP, S.now, &x, &ST);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)left, ST.lockout_left_ms);
    const uint64_t k = S.now + KP_CLOCK + 30u;
    plink_view_apply_status(&V, &ST, k);
    TEST_ASSERT_TRUE(V.snap.local_lockout_until == k + left);

    sim_advance(&S, left + 1u);
    take_snap();
    x.lockout_left_ms = (uint32_t)alarm_engine_lockout_left(&S.eng, &kp3, S.now);
    TEST_ASSERT_EQUAL_UINT32(0u, x.lockout_left_ms);
    plink_status_build(&SNAP, S.now, &x, &ST);
    plink_view_apply_status(&V, &ST, S.now + KP_CLOCK + 30u);
    TEST_ASSERT_TRUE(V.snap.local_lockout_until == 0u);
}

/* LINK-002: the walk test's start goes as its age and an identity; the keypad maps it to its own
 * clock once per test, so a later frame with more network delay doesn't move it, and a new test
 * maps again. A start whose low 32 bits are 0 still has an identity; none reads 0. */
static void test_walk_start_mapped_once(void)
{
    EXPECT_RESULT(ALARM_OK, walk(CODE_MASTER, ALARM_WALK_START));
    const uint64_t start = sim_status(&S, 1).walk_started_ms;
    TEST_ASSERT_TRUE(start != 0u);
    const plink_status_extra_t x = extra();

    sim_advance(&S, 2000u);
    take_snap();
    plink_status_build(&SNAP, S.now + 300u, &x, &ST);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(S.now + 300u - start), ST.part[0].walk_age_ms);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)start, ST.part[0].walk_id);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PF_WALK_TEST, ST.part[0].flags & PLINK_PF_WALK_TEST);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SNAP.part[0].walk_test_remaining_ms - 300u,
                             ST.part[0].walk_left_ms);
    const uint64_t k1 = S.now + 300u + KP_CLOCK + 20u;
    plink_view_apply_status(&V, &ST, k1);
    const uint64_t local = V.snap.part[0].walk_started_ms;
    TEST_ASSERT_TRUE(local == start + KP_CLOCK + 20u);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_test);

    sim_fault(&S, Z_WINDOW); /* a mark, and a later frame with more delay */
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, 5000u);
    take_snap();
    plink_status_build(&SNAP, S.now, &x, &ST);
    plink_view_apply_status(&V, &ST, S.now + KP_CLOCK + 75u);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_started_ms == local);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_tested >= 1u);
    TEST_ASSERT_EQUAL_UINT32(SNAP.part[0].walk_seq, V.snap.part[0].walk_seq);

    /* Ended: the summary keeps the same start */
    EXPECT_RESULT(ALARM_OK, walk(CODE_MASTER, ALARM_WALK_END));
    sim_advance(&S, 1000u);
    take_snap();
    plink_status_build(&SNAP, S.now, &x, &ST);
    plink_view_apply_status(&V, &ST, S.now + KP_CLOCK + 5u);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_started_ms == local);
    TEST_ASSERT_FALSE(V.snap.part[0].walk_test);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_COMMAND, V.snap.part[0].walk_end_how);

    /* A new walk test: a new identity, mapped again */
    sim_advance(&S, 10000u);
    EXPECT_RESULT(ALARM_OK, walk(CODE_MASTER, ALARM_WALK_START));
    const uint64_t start2 = sim_status(&S, 1).walk_started_ms;
    take_snap();
    plink_status_build(&SNAP, S.now, &x, &ST);
    plink_view_apply_status(&V, &ST, S.now + KP_CLOCK + 40u);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_started_ms == start2 + KP_CLOCK + 40u);

    /* A start whose low 32 bits are 0 */
    SNAP.part[0].walk_started_ms = (uint64_t)1u << 32;
    plink_status_build(&SNAP, SNAP.part[0].walk_started_ms + 10u, &x, &ST);
    TEST_ASSERT_EQUAL_UINT32(1u, ST.part[0].walk_id);
    TEST_ASSERT_EQUAL_UINT32(10u, ST.part[0].walk_age_ms);
    /* An age older than the keypad's clock maps to 1, never 0 (0 is none) */
    plink_view_apply_status(&V, &ST, 5u);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_started_ms == 1u);
    /* None */
    ST.part[0].walk_id = 0u;
    plink_view_apply_status(&V, &ST, 100u);
    TEST_ASSERT_TRUE(V.snap.part[0].walk_started_ms == 0u);
}

/* LINK-002: one record per enabled partition; on the keypad the partitions not in the mask are
 * zeroed, records beyond this build's partitions ignored, and a record a frame doesn't carry (a
 * small transport sends STATUS in pieces) keeps its last values, counted down. */
static void test_partitions_mask(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[2].enabled = true; /* partitions 1 and 3 */
    sim_start(&S, &cfg, NULL);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    take_snap();
    const plink_status_extra_t x = extra();
    plink_status_build(&SNAP, S.now, &x, &ST);
    TEST_ASSERT_EQUAL_HEX8(0x05u, ST.partitions);
    TEST_ASSERT_EQUAL_UINT8(2u, ST.count);
    TEST_ASSERT_EQUAL_UINT8(1u, ST.part[0].number);
    TEST_ASSERT_EQUAL_UINT8(3u, ST.part[1].number);
    TEST_ASSERT_EQUAL_UINT8(ALARM_STATE_DISARMED, ST.part[1].state);

    memset(&V.snap.part[1], 0x5A, sizeof(V.snap.part[1])); /* stale bytes the mask must clear */
    plink_view_apply_status(&V, &ST, 1000u);
    TEST_ASSERT_EQUAL_HEX8(0x05u, V.snap.partitions);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, V.snap.part[0].state);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_STAY, V.snap.part[0].level);
    TEST_ASSERT_TRUE(V.snap.part[0].delay_remaining_ms == SNAP.part[0].delay_remaining_ms);
    TEST_ASSERT_EQUAL_INT(SNAP.part[2].ready, V.snap.part[2].ready);
    TEST_ASSERT_EQUAL_UINT8(SNAP.part[2].ready_levels, V.snap.part[2].ready_levels);
    alarm_partition_status_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &V.snap.part[1], sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &V.snap.part[3], sizeof(zero));

    /* A piece with partition 3 only: partition 1 keeps its exit time, counted down */
    const uint64_t left1 = shown_ms(&V, 1, 4000u);
    plink_status_t piece = ST;
    piece.count = 1u;
    piece.part[0] = ST.part[1];
    plink_view_apply_status(&V, &piece, 4000u);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, V.snap.part[0].state);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 4000u) == left1);
    TEST_ASSERT_TRUE(shown_ms(&V, 1, 5000u) == left1 - 1000u);

    /* Partition 3 disabled at the master, and records for partitions this build doesn't have */
    plink_status_t only1 = ST;
    only1.partitions = 0x81u;
    only1.count = 3u;
    only1.part[1] = ST.part[1];
    only1.part[1].number = 8u;
    only1.part[1].state = ALARM_STATE_ARMED;
    only1.part[2] = ST.part[1];
    only1.part[2].number = 0u;
    plink_view_apply_status(&V, &only1, 6000u);
    TEST_ASSERT_EQUAL_HEX8(0x01u, V.snap.partitions);
    TEST_ASSERT_EQUAL_MEMORY(&zero, &V.snap.part[2], sizeof(zero));
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, V.snap.part[0].state);

    /* A record for a partition not in the mask is ignored too */
    plink_status_t odd = ST;
    odd.partitions = 0x01u;
    odd.count = 2u;
    odd.part[1].state = ALARM_STATE_ARMED; /* partition 3, not in the mask */
    plink_view_apply_status(&V, &odd, 7000u);
    TEST_ASSERT_EQUAL_MEMORY(&zero, &V.snap.part[2], sizeof(zero));
}

/* LINK-002, LINK-008: the system section lands in the snapshot and the view; zones beyond
 * zones_used are zeroed; ZONES records fill the zones and the keypad's zones CRC then equals the
 * master's, whole or in pieces. seq and config_seq are the keypad manager's, untouched. */
static void test_zones_and_crc(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_SHED); /* a zone in the second piece below */
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_HALL_PIR));
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_TROUBLE);
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_LOW_BATTERY);
    take_snap();
    const plink_status_extra_t x = extra();
    plink_status_build(&SNAP, S.now, &x, &ST);

    V.snap.seq = 77u;
    V.snap.config_seq = 9u;
    V.snap.zone[50].flags = 0xFFFFu; /* beyond zones_used: must be cleared */
    plink_view_apply_status(&V, &ST, 123456u);
    TEST_ASSERT_TRUE(V.have_status);
    TEST_ASSERT_EQUAL_UINT32(77u, V.snap.seq);
    TEST_ASSERT_EQUAL_UINT32(9u, V.snap.config_seq);
    TEST_ASSERT_TRUE(V.snap.taken_ms == 123456u);
    TEST_ASSERT_EQUAL_UINT16(20u, V.snap.zones_used);
    TEST_ASSERT_TRUE(zone_zero(&V.snap.zone[50]));
    TEST_ASSERT_EQUAL_HEX32(SNAP.sys_troubles, V.snap.sys_troubles);
    TEST_ASSERT_EQUAL_HEX32(SNAP.sys_troubles_restored, V.snap.sys_troubles_restored);
    TEST_ASSERT_EQUAL_HEX32(SNAP.dev_troubles, V.snap.dev_troubles);
    TEST_ASSERT_EQUAL_HEX8(SNAP.default_codes, V.snap.default_codes);
    TEST_ASSERT_EQUAL_UINT32(SNAP.last_event_seq, V.snap.last_event_seq);
    TEST_ASSERT_TRUE(V.snap.exit_sounds);
    TEST_ASSERT_FALSE(V.snap.program);
    TEST_ASSERT_TRUE(V.epoch_ms == x.epoch_ms);
    TEST_ASSERT_TRUE(V.epoch_at_ms == 123456u);
    TEST_ASSERT_EQUAL_UINT8(1u, V.home);
    TEST_ASSERT_EQUAL_UINT8(PLINK_REPORT_ONLINE, V.report);
    TEST_ASSERT_EQUAL_HEX32(x.cfg_crc, V.cfg_crc);
    TEST_ASSERT_EQUAL_UINT32(x.cfg_len, V.cfg_len);
    TEST_ASSERT_EQUAL_HEX32(ST.zones_crc, V.zones_crc);
    TEST_ASSERT_NOT_EQUAL(ST.zones_crc, plink_view_zones_crc(&V)); /* no zones yet */

    /* The zones, in two pieces */
    uint8_t recs[20u * PLINK_ZONE_REC_LEN];
    for (unsigned z = 0; z < 20u; z++) {
        zone_rec(&SNAP.zone[z], &recs[z * PLINK_ZONE_REC_LEN]);
    }
    plink_zones_t zm = {ST.zones_crc, 1u, 12u, PLINK_ZONE_REC_LEN, recs};
    plink_view_apply_zones(&V, &zm);
    TEST_ASSERT_NOT_EQUAL(ST.zones_crc, plink_view_zones_crc(&V));
    zm.first = 13u;
    zm.count = 8u;
    zm.recs = &recs[12u * PLINK_ZONE_REC_LEN];
    plink_view_apply_zones(&V, &zm);
    TEST_ASSERT_EQUAL_HEX32(ST.zones_crc, plink_view_zones_crc(&V));
    for (unsigned z = 0; z < 20u; z++) {
        expect_zone(&SNAP.zone[z], &V.snap.zone[z]);
    }
    TEST_ASSERT_TRUE((V.snap.zone[Z_WINDOW - 1].flags & ALARM_ZS_FAULT) != 0u);
    TEST_ASSERT_TRUE((V.snap.zone[Z_HALL_PIR - 1].flags & ALARM_ZS_BYPASSED) != 0u);
    TEST_ASSERT_TRUE((V.snap.zone[Z_SMOKE - 1].flags & ALARM_ZS_TROUBLE) != 0u);
    TEST_ASSERT_TRUE(V.snap.zone[Z_FAMILY - 1].trouble != 0u);

    /* A zone changes at the master: the CRCs differ until its record comes */
    sim_restore(&S, Z_WINDOW);
    take_snap();
    plink_status_build(&SNAP, S.now, &x, &ST);
    plink_view_apply_status(&V, &ST, 124000u);
    TEST_ASSERT_NOT_EQUAL(ST.zones_crc, plink_view_zones_crc(&V));
    zone_rec(&SNAP.zone[Z_WINDOW - 1], recs);
    const plink_zones_t one = {ST.zones_crc, Z_WINDOW, 1u, PLINK_ZONE_REC_LEN, recs};
    plink_view_apply_zones(&V, &one);
    TEST_ASSERT_EQUAL_HEX32(ST.zones_crc, plink_view_zones_crc(&V));

    plink_view_apply_zones(NULL, &one); /* nothing to do: no crash */
    plink_view_apply_status(&V, NULL, 0u);
    TEST_ASSERT_EQUAL_HEX32(0u, plink_view_zones_crc(NULL));
}

/* LINK-002, LINK-017: the zone record and zones_crc against vectors made from the document with
 * Python's zlib (vectors_plink_view.py); a longer record's appended bytes are skipped and a
 * shorter one reads zeros past its end; zones outside 1..ALARM_MAX_ZONES are ignored. */
static void test_zone_record_vectors(void)
{
    static const uint8_t REC[21] = {
        0x01, 0x02, 0x08, 0x05, 0x34, 0x12, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x41, 0x04, 0x01, 0x0D, 0xFF, 0xFF, 0xFF,
    };
    const alarm_zone_status_t want[3] = {
        {0x0201u, 0x08u, 0x05u, 0x1234u, 7u},
        {0u, 0u, 0u, 0u, 0u},
        {0x0441u, 0x01u, 0x0Du, 0xFFFFu, 255u},
    };
    TEST_ASSERT_EQUAL_HEX32(0xFF6E3FF0u, plink_zones_crc(want, 3u));
    TEST_ASSERT_EQUAL_HEX32(0xB48F660Eu, plink_zones_crc(want, 1u));
    TEST_ASSERT_EQUAL_HEX32(0u, plink_zones_crc(want, 0u));
    TEST_ASSERT_EQUAL_HEX32(0u, plink_zones_crc(NULL, 3u));

    plink_zones_t m = {0xFF6E3FF0u, 1u, 3u, PLINK_ZONE_REC_LEN, REC};
    alarm_zone_status_t got;
    for (uint16_t i = 0; i < 3u; i++) {
        plink_zones_rec(&m, i, &got);
        expect_zone(&want[i], &got);
    }
    plink_zones_rec(&m, 3u, &got); /* past count: zeros */
    TEST_ASSERT_TRUE(zone_zero(&got));

    /* Records of 9 bytes (two appended) */
    uint8_t wide[27];
    for (unsigned i = 0; i < 3u; i++) {
        memcpy(&wide[i * 9u], &REC[i * 7u], 7u);
        wide[i * 9u + 7u] = 0xAAu;
        wide[i * 9u + 8u] = 0xBBu;
    }
    m.rec_len = 9u;
    m.recs = wide;
    for (uint16_t i = 0; i < 3u; i++) {
        plink_zones_rec(&m, i, &got);
        expect_zone(&want[i], &got);
    }
    /* Records of 4 bytes: walk_first_s and walk_trips read 0 */
    uint8_t narrow[12];
    for (unsigned i = 0; i < 3u; i++) {
        memcpy(&narrow[i * 4u], &REC[i * 7u], 4u);
    }
    m.rec_len = 4u;
    m.recs = narrow;
    plink_zones_rec(&m, 2u, &got);
    TEST_ASSERT_EQUAL_HEX16(0x0441u, got.flags);
    TEST_ASSERT_EQUAL_HEX8(0x0Du, got.walk);
    TEST_ASSERT_EQUAL_UINT16(0u, got.walk_first_s);
    TEST_ASSERT_EQUAL_UINT8(0u, got.walk_trips);

    /* Zones 0 and beyond ALARM_MAX_ZONES are ignored */
    m.rec_len = PLINK_ZONE_REC_LEN;
    m.recs = REC;
    m.first = 0u;
    plink_view_apply_zones(&V, &m);
    expect_zone(&want[1], &V.snap.zone[0]);
    expect_zone(&want[2], &V.snap.zone[1]);
    m.first = ALARM_MAX_ZONES - 1u;
    plink_view_apply_zones(&V, &m);
    expect_zone(&want[0], &V.snap.zone[ALARM_MAX_ZONES - 2u]);
    expect_zone(&want[1], &V.snap.zone[ALARM_MAX_ZONES - 1u]);
}

/* ---- the configuration copy (LINK-019) ------------------------------------------------------ */

static alarm_config_t CFG, SCRATCH, DEC, STRICT_CFG;
static uint8_t BLOB[ALARM_CONFIG_BLOB_MAX + 256u];
static uint8_t BLOB2[ALARM_CONFIG_BLOB_MAX + 256u];

static bool contains(const uint8_t *hay, size_t n, const uint8_t *needle, size_t m)
{
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) {
            return true;
        }
    }
    return false;
}

/* LINK-019: the keypads' copy of the configuration holds no code material - no 4-byte run of any
 * real code hash or of the salt anywhere in the blob - each hash replaced by a filler unique to
 * its slot (so the copy still passes the check that no two users share a code), duress users
 * shown as standard; zones, users' names and rights and partition names come back whole. The
 * master's own configuration is untouched, the blob the same at every build, and a code change
 * doesn't change it. */
static void test_cfg_copy_has_no_code_material(void)
{
    sim_config_basic(&CFG);
    snprintf(CFG.users[5].name, ALARM_NAME_LEN, "%s", "Sam"); /* user 6, the duress code */
    snprintf(CFG.partitions[0].name, ALARM_NAME_LEN, "%s", "Harbor House");
    const alarm_config_t before = CFG;
    size_t len = 0u;
    uint32_t crc = 0u;
    EXPECT_RESULT(ALARM_OK, plink_cfg_for_keypads(&CFG, &SCRATCH, BLOB, sizeof(BLOB), &len, &crc));
    TEST_ASSERT_EQUAL_HEX32(alarm_crc32(0u, BLOB, len), crc);
    TEST_ASSERT_EQUAL_MEMORY(&before, &CFG, sizeof(CFG));

    unsigned users = 0u;
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        if (CFG.users[u].authority == ALARM_AUTH_NONE) {
            continue;
        }
        users++;
        for (unsigned off = 0; off + 4u <= ALARM_CODE_HASH_LEN; off++) {
            TEST_ASSERT_FALSE(contains(BLOB, len, &CFG.users[u].code_hash[off], 4u));
        }
    }
    TEST_ASSERT_EQUAL_UINT(7u, users);
    for (unsigned off = 0; off + 4u <= ALARM_CODE_SALT_LEN; off++) {
        TEST_ASSERT_FALSE(contains(BLOB, len, &CFG.code_salt[off], 4u));
    }

    EXPECT_RESULT(ALARM_OK, plink_cfg_decode(BLOB, len, &DEC));
    const uint8_t no_salt[ALARM_CODE_SALT_LEN] = {0};
    TEST_ASSERT_EQUAL_MEMORY(no_salt, DEC.code_salt, sizeof(no_salt));
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        const alarm_user_t *a = &CFG.users[u];
        const alarm_user_t *b = &DEC.users[u];
        TEST_ASSERT_EQUAL_STRING(a->name, b->name);
        TEST_ASSERT_EQUAL_HEX8(a->partitions, b->partitions);
        if (a->authority == ALARM_AUTH_NONE) {
            TEST_ASSERT_EQUAL_INT(ALARM_AUTH_NONE, b->authority);
            continue;
        }
        TEST_ASSERT_EQUAL_INT(
            a->authority == ALARM_AUTH_DURESS ? ALARM_AUTH_STANDARD : a->authority, b->authority);
        TEST_ASSERT_FALSE(memcmp(a->code_hash, b->code_hash, ALARM_CODE_HASH_LEN) == 0);
        for (unsigned w = 0; w < u; w++) { /* unique per slot */
            if (DEC.users[w].authority != ALARM_AUTH_NONE) {
                TEST_ASSERT_FALSE(
                    memcmp(DEC.users[w].code_hash, b->code_hash, ALARM_CODE_HASH_LEN) == 0);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_DURESS, CFG.users[5].authority);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_STANDARD, DEC.users[5].authority);
    TEST_ASSERT_EQUAL_MEMORY(CFG.zones, DEC.zones, sizeof(CFG.zones));
    TEST_ASSERT_EQUAL_MEMORY(CFG.partitions, DEC.partitions, sizeof(CFG.partitions));
    TEST_ASSERT_EQUAL_MEMORY(CFG.devices, DEC.devices, sizeof(CFG.devices));
    TEST_ASSERT_EQUAL_MEMORY(&CFG.opt, &DEC.opt, sizeof(CFG.opt));
    /* The fillers keep the copy a valid configuration: the strict decode takes it too. */
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(BLOB, len, &STRICT_CFG));

    /* The same at every build (cfg_crc versions it across master restarts) */
    size_t len2 = 0u;
    uint32_t crc2 = 0u;
    EXPECT_RESULT(ALARM_OK,
                  plink_cfg_for_keypads(&CFG, &SCRATCH, BLOB2, sizeof(BLOB2), &len2, &crc2));
    TEST_ASSERT_EQUAL_UINT(len, len2);
    TEST_ASSERT_EQUAL_MEMORY(BLOB, BLOB2, len);
    TEST_ASSERT_EQUAL_HEX32(crc, crc2);
    /* A new code for user 3: the keypads' copy doesn't change (nothing of it is there) */
    sim_set_user(&CFG, 3, ALARM_AUTH_STANDARD, 0x01, "4826");
    EXPECT_RESULT(ALARM_OK,
                  plink_cfg_for_keypads(&CFG, &SCRATCH, BLOB2, sizeof(BLOB2), &len2, &crc2));
    TEST_ASSERT_EQUAL_HEX32(crc, crc2);
    /* A new zone name does */
    snprintf(CFG.zones[0].name, ALARM_NAME_LEN, "%s", "Porch door");
    EXPECT_RESULT(ALARM_OK,
                  plink_cfg_for_keypads(&CFG, &SCRATCH, BLOB2, sizeof(BLOB2), &len2, &crc2));
    TEST_ASSERT_NOT_EQUAL(crc, crc2);

    /* Refused: the master's own configuration as scratch, a buffer too small, no output */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  plink_cfg_for_keypads(&CFG, &CFG, BLOB2, sizeof(BLOB2), &len2, &crc2));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  plink_cfg_for_keypads(&CFG, &SCRATCH, BLOB2, len2 - 1u, &len2, &crc2));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  plink_cfg_for_keypads(NULL, &SCRATCH, BLOB2, sizeof(BLOB2), &len2, &crc2));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  plink_cfg_for_keypads(&CFG, &SCRATCH, NULL, sizeof(BLOB2), &len2, &crc2));
}

static void put_item(size_t *len, uint8_t group, uint8_t field, const void *val, uint8_t n)
{
    BLOB[*len] = field;
    BLOB[*len + 1u] = group;
    BLOB[*len + 2u] = n;
    memcpy(BLOB + *len + 3u, val, n);
    *len += 3u + n;
}

static void reseal(size_t len)
{
    const uint32_t payload = (uint32_t)(len - 16u);
    const uint32_t crc = alarm_crc32(0u, BLOB + 16, payload);
    for (unsigned i = 0; i < 4u; i++) {
        BLOB[8u + i] = (uint8_t)(payload >> (8u * i));
        BLOB[12u + i] = (uint8_t)(crc >> (8u * i));
    }
}

/* LINK-019, CORE-CFG-022: the keypad decodes the copy leniently, so a newer, bigger master
 * doesn't leave it blank: zones and users beyond its capacities are skipped, values its check
 * would refuse or doesn't know are kept, and nothing is validated; a damaged copy is still
 * refused. */
static void test_cfg_decode_lenient(void)
{
    sim_config_basic(&CFG);
    CFG.zones[0].partition = 2u;                      /* a partition disabled */
    CFG.zones[1].type = (alarm_zone_type_t)99;        /* a type from a later firmware */
    CFG.users[3].authority = (alarm_authority_t)0x20; /* an authority from a later firmware */
    snprintf(CFG.zones[0].name, ALARM_NAME_LEN, "%s", "Porch door");
    size_t len = 0u;
    uint32_t crc = 0u;
    EXPECT_RESULT(ALARM_OK, plink_cfg_for_keypads(&CFG, &SCRATCH, BLOB, sizeof(BLOB), &len, &crc));

    const uint16_t big_zone = ALARM_MAX_ZONES + 7u;
    const uint8_t zsel[2] = {(uint8_t)(big_zone & 0xFFu), (uint8_t)(big_zone >> 8)};
    const uint8_t usel = ALARM_MAX_USERS + 1u, perim = ALARM_ZT_PERIMETER, one = 1u;
    put_item(&len, 0x02, 0x00, zsel, 2u);
    put_item(&len, 0x02, 0x01, &perim, 1u);
    put_item(&len, 0x02, 0x02, &one, 1u);
    put_item(&len, 0x02, 0x09, "Annex", 5u);
    put_item(&len, 0x03, 0x00, &usel, 1u);
    put_item(&len, 0x03, 0x01, &one, 1u);
    put_item(&len, 0x03, 0x04, "Alex", 4u);
    reseal(len);

    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(BLOB, len, &STRICT_CFG));
    EXPECT_RESULT(ALARM_OK, plink_cfg_decode(BLOB, len, &DEC));
    TEST_ASSERT_EQUAL_MEMORY(CFG.zones, DEC.zones, sizeof(CFG.zones));
    TEST_ASSERT_EQUAL_MEMORY(CFG.partitions, DEC.partitions, sizeof(CFG.partitions));
    TEST_ASSERT_EQUAL_INT(0x20, (int)DEC.users[3].authority);
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        TEST_ASSERT_EQUAL_STRING(CFG.users[u].name, DEC.users[u].name);
    }
    TEST_ASSERT_EQUAL_STRING("Porch door", DEC.zones[0].name);
    TEST_ASSERT_EQUAL_UINT8(2u, DEC.zones[0].partition);

    BLOB[40] ^= 0x10u; /* damaged */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, plink_cfg_decode(BLOB, len, &DEC));
    BLOB[40] ^= 0x10u;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, plink_cfg_decode(BLOB, len - 2u, &DEC));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, plink_cfg_decode(NULL, len, &DEC));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_build_works_out_durations);
    RUN_TEST(test_keypad_countdown_never_goes_up);
    RUN_TEST(test_countdown_rises_only_for_real);
    RUN_TEST(test_keypad_lockout);
    RUN_TEST(test_walk_start_mapped_once);
    RUN_TEST(test_partitions_mask);
    RUN_TEST(test_zones_and_crc);
    RUN_TEST(test_zone_record_vectors);
    RUN_TEST(test_cfg_copy_has_no_code_material);
    RUN_TEST(test_cfg_decode_lenient);
    return UNITY_END();
}
