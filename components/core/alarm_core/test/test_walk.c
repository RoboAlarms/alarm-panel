/*
 * test_walk.c - the walk test: start, end and extend, who may, the zones it covers and their
 * marks, the end warnings and the summary.
 * Requirements: WALK-* (docs/features/40), CORE-TEST-*, CORE-USR-027
 */
#include "sim.h"

#include "alarm/alarm_snapshot.h"

static sim_t S;

/* The basic layout's zones a walk test covers without the 24-hour option: entry/exit 1-2,
 * perimeter, follower, interior delay, day/night, monitor, silent burglary, follower+NIGHT,
 * no response, the arm-away and disarm fobs, the keyswitch and momentary exit. */
#define COVERED        14u
#define COVERED_LIFE24 17u /* and the panic, hold-up and water zones */

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

static alarm_result_t walk(uint8_t p, const char *code, alarm_walk_op_t op, uint8_t flags)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = p;
    cmd.walk_op = (uint8_t)op;
    cmd.walk_flags = flags;
    strncpy(cmd.code, code, ALARM_CODE_MAX_LEN);
    return sim_cmd(&S, &cmd);
}

static const alarm_source_t PM = {ALARM_SRC_REMOTE, ALARM_SRC_ID_PANEL_MANAGER};

static alarm_result_t walk_as(uint8_t user, alarm_walk_op_t op)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = 1;
    cmd.walk_op = (uint8_t)op;
    return alarm_engine_command_as(&S.eng, &cmd, &PM, user, S.now, &S.last);
}

static bool testing(void)
{
    return sim_status(&S, 1).walk_test;
}

static void press(uint16_t zone)
{
    sim_fault(&S, zone);
    sim_restore(&S, zone);
}

/* WALK-002, CORE-TEST-005: start, end and extend are separate; a repeat does nothing, says so and never
 * undoes the first. */
static void test_start_end_and_extend_are_separate(void)
{
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_END, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_EXTEND, 0));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_END, 0);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_EXTEND, 0);
    TEST_ASSERT_EQUAL_UINT(2, sim_count_sounds(&S, ALARM_SOUND_ERROR));

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    TEST_ASSERT_TRUE(testing());
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    TEST_ASSERT_TRUE(testing());
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_START, 1);
    TEST_ASSERT_EQUAL_UINT(3, sim_count_sounds(&S, ALARM_SOUND_ERROR));

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_EXTEND, 0));
    TEST_ASSERT_TRUE(testing());
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_END, 0));
    TEST_ASSERT_FALSE(testing());
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_END, 0));
    TEST_ASSERT_FALSE(testing());
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_END, 1);

    /* An operation the engine doesn't know is refused, not taken for a toggle. */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, walk(1, CODE_MASTER, (alarm_walk_op_t)4, 0));
    TEST_ASSERT_FALSE(testing());

    /* The keypad's zeroed command still toggles (code + TEST). */
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_TOGGLE, 0));
    TEST_ASSERT_TRUE(testing());
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_TOGGLE, 0));
    TEST_ASSERT_FALSE(testing());
}

/* WALK-002, CORE-TEST-005, CORE-PROG-002: a start only while disarmed, silent and out of installer mode. */
static void test_start_needs_a_quiet_disarmed_partition(void)
{
    sim_arm_stay_settled(&S, CODE_USER);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));

    sim_fault(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_MASTER, ALARM_WALK_TOGGLE, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* silence */
    sim_restore(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* clear the memory */

    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, NULL));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, CODE_INSTALLER, ALARM_WALK_START, 0));
    TEST_ASSERT_FALSE(testing());
    alarm_engine_program_exit(&S.eng, &S.keypad, S.now);
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_INSTALLER, ALARM_WALK_START, 0));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_START, 1);
}

/* WALK-001, CORE-TEST-004: the installer, the system master and a partition master start and
 * extend; a standard, arm-only, guest or duress code may not. WALK-025: whoever may disarm
 * the partition ends it. */
static void test_who_starts_extends_and_ends(void)
{
    static const char *const refused[] = {CODE_USER, CODE_ARMONLY, CODE_GUEST, CODE_DURESS};
    for (unsigned i = 0; i < 4u; i++) {
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(1, refused[i], ALARM_WALK_START, 0));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(1, refused[i], ALARM_WALK_TOGGLE, 0));
    }
    TEST_ASSERT_FALSE(testing());
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_START, 0);

    static const char *const allowed[] = {CODE_INSTALLER, CODE_MASTER, CODE_PMASTER};
    for (unsigned i = 0; i < 3u; i++) {
        EXPECT_RESULT(ALARM_OK, walk(1, allowed[i], ALARM_WALK_START, 0));
        EXPECT_RESULT(ALARM_OK, walk(1, allowed[i], ALARM_WALK_EXTEND, 0));
        EXPECT_RESULT(ALARM_OK, walk(1, allowed[i], ALARM_WALK_END, 0));
    }

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(1, CODE_USER, ALARM_WALK_EXTEND, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(1, CODE_ARMONLY, ALARM_WALK_END, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(1, CODE_ARMONLY, ALARM_WALK_TOGGLE, 0));
    TEST_ASSERT_TRUE(testing());
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_USER, ALARM_WALK_END, 0));
    TEST_ASSERT_FALSE(testing());
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_event(&S, ALARM_EV_WALK_TEST_END)->user);

    /* A guest may disarm a disarmed partition, so a guest ends a test too. */
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_GUEST, ALARM_WALK_TOGGLE, 0));
    TEST_ASSERT_FALSE(testing());

    /* A duress code ends it as it disarms: with the silent duress report. */
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_DURESS, ALARM_WALK_END, 0));
    TEST_ASSERT_FALSE(testing());
    EXPECT_REPORTS(&S, ALARM_EV_DURESS, 1);
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_DURESS));
    TEST_ASSERT_FALSE(testing());
    EXPECT_REPORTS(&S, ALARM_EV_DURESS, 2);
}

/* WALK-025: an explicit End needs no code (it only gives protection back) and is the system's;
 * a start, an extend or the keypad's toggle still asks for one. */
static void test_end_needs_no_code(void)
{
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, "", ALARM_WALK_END, 0));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, walk(1, "", ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, walk(1, "", ALARM_WALK_TOGGLE, 0));
    TEST_ASSERT_FALSE(testing());

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, walk(1, "", ALARM_WALK_EXTEND, 0));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, walk(1, "", ALARM_WALK_TOGGLE, 0));
    TEST_ASSERT_TRUE(testing());
    EXPECT_RESULT(ALARM_OK, walk(1, "", ALARM_WALK_END, 0));
    TEST_ASSERT_FALSE(testing());
    TEST_ASSERT_EQUAL_UINT8(0, sim_last_event(&S, ALARM_EV_WALK_TEST_END)->user);
    TEST_ASSERT_EQUAL_UINT8(0, sim_status(&S, 1).walk_ended_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_COMMAND, sim_status(&S, 1).walk_end_how);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, walk(1, "", ALARM_WALK_END, 0));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_END, 1);
}

/* CORE-USR-027, WALK-001, WALK-025: a signed-in user follows the same rules. */
static void test_signed_in_users_follow_the_same_rules(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk_as(3, ALARM_WALK_START));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk_as(0, ALARM_WALK_START));
    TEST_ASSERT_FALSE(testing());
    EXPECT_RESULT(ALARM_OK, walk_as(7, ALARM_WALK_START));
    TEST_ASSERT_TRUE(testing());
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk_as(3, ALARM_WALK_EXTEND));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk_as(4, ALARM_WALK_END));
    EXPECT_RESULT(ALARM_OK, walk_as(ALARM_USER_MASTER, ALARM_WALK_EXTEND));
    EXPECT_RESULT(ALARM_OK, walk_as(3, ALARM_WALK_END));
    TEST_ASSERT_FALSE(testing());

    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_WALK_TEST_EXTEND);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, ev->user);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);
    TEST_ASSERT_EQUAL_UINT8(7, sim_status(&S, 1).walk_started_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_REMOTE, sim_status(&S, 1).walk_started_src.kind);
    TEST_ASSERT_EQUAL_UINT8(3, sim_status(&S, 1).walk_ended_by);
}

/* WALK-006, WALK-011, WALK-012, CORE-TEST-006: the first trip marks and logs a zone; repeats beep and count. */
static void test_first_trip_marks_and_logs_repeats_only_beep(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT16(COVERED, st.walk_total);
    TEST_ASSERT_EQUAL_UINT16(0, st.walk_tested);
    const uint32_t seq = st.walk_seq;
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_WINDOW).walk);

    sim_advance(&S, SEC(30));
    press(Z_WINDOW);
    sim_advance(&S, SEC(10));
    press(Z_WINDOW);
    press(Z_WINDOW);

    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 1);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_FAULT,
                            sim_last_event(&S, ALARM_EV_WALK_TEST_ZONE)->arg);
    TEST_ASSERT_EQUAL_UINT(3, sim_count_zone_sounds(&S, ALARM_SOUND_WALK_TEST, Z_WINDOW));
    const alarm_zone_status_t zs = sim_zstatus(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_ZS_TESTED, zs.walk & ALARM_WALK_ZS_TESTED);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_FAULT, ALARM_WALK_ZS_KIND(zs.walk));
    TEST_ASSERT_EQUAL_UINT8(3, zs.walk_trips);
    TEST_ASSERT_EQUAL_UINT16(30, zs.walk_first_s);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).walk_tested);
    TEST_ASSERT_EQUAL_UINT32(seq + 1u, sim_status(&S, 1).walk_seq);

    /* The trip count saturates instead of wrapping. */
    for (unsigned i = 0; i < 300u; i++) {
        press(Z_FRONT);
    }
    TEST_ASSERT_EQUAL_UINT8(255, sim_zstatus(&S, Z_FRONT).walk_trips);
    TEST_ASSERT_EQUAL_UINT16(2, sim_status(&S, 1).walk_tested);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 2);
}

/* WALK-007, WALK-025, CORE-TEST-001: buttons, keyfobs and the keyswitch beep and are marked; nothing arms. A
 * keyfob's Disarm is marked, then ends the test as code + OFF does. Monitor and momentary
 * exit zones beep and do nothing else. */
static void test_buttons_are_marked_and_arm_nothing(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    sim_clear_records(&S);
    press(Z_FOB_AWAY);
    press(Z_KEYSWITCH);
    press(Z_MOM_EXIT);
    press(Z_FREEZER);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_MONITOR_FAULT, 0);
    EXPECT_EVENTS(&S, ALARM_EV_MONITOR_RESTORE, 0); /* WALK-007, WALK-011 */
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 4);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_BUTTON, ALARM_WALK_ZS_KIND(sim_zstatus(&S, Z_FOB_AWAY).walk));
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_BUTTON, ALARM_WALK_ZS_KIND(sim_zstatus(&S, Z_KEYSWITCH).walk));
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_FAULT, ALARM_WALK_ZS_KIND(sim_zstatus(&S, Z_MOM_EXIT).walk));
    TEST_ASSERT_EQUAL_UINT16(4, sim_status(&S, 1).walk_tested);
    TEST_ASSERT_TRUE(testing());

    press(Z_FOB_OFF);
    TEST_ASSERT_FALSE(testing());
    TEST_ASSERT_TRUE((sim_zstatus(&S, Z_FOB_OFF).walk & ALARM_WALK_ZS_TESTED) != 0u);
    const alarm_event_t *end = sim_last_event(&S, ALARM_EV_WALK_TEST_END);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(3, end->user);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_ZONE, end->source.kind);
    TEST_ASSERT_EQUAL_UINT8(5, end->arg);
    TEST_ASSERT_EQUAL_UINT8(COVERED, end->arg2);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_DISARM, sim_status(&S, 1).walk_end_how);

    /* After the test the buttons work again. */
    press(Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
}

/* WALK-007, CORE-TEST-002: 24-hour zones and panic buttons stay live unless the start includes them. */
static void test_24_hour_zones_only_with_the_option(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    sim_fault(&S, Z_PANIC);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_PANIC, ALARM_CAUSE_24H_AUDIBLE));
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_PANIC).walk);
    sim_restore(&S, Z_PANIC);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* silence */
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_END, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* memory */

    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, ALARM_WALK_F_LIFE24));
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_F_LIFE24, st.walk_flags);
    TEST_ASSERT_EQUAL_UINT16(COVERED_LIFE24, st.walk_total);
    press(Z_PANIC);
    press(Z_HOLDUP);
    press(Z_WATER);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 3);
    TEST_ASSERT_EQUAL_UINT16(3, sim_status(&S, 1).walk_tested);

    /* A keypad panic is a person asking for help, not a zone: it stays live. */
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 1);
}

/* WALK-007, CORE-TEST-002: fire and CO stay live and aren't counted; the sensor's own test
 * marks them without an alarm. Outside a test that report is only a check-in. */
static void test_fire_and_co_stay_live_but_their_test_marks_them(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_SMOKE - 1u].supervision_s = 60u;
    sim_start(&S, &cfg, NULL);

    sim_advance(&S, SEC(61));
    EXPECT_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_SUPERVISION);
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_TEST);
    EXPECT_NO_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_SUPERVISION); /* a check-in */
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_SMOKE).walk);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 0);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_sounds(&S, ALARM_SOUND_WALK_TEST));

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_TEST);
    sim_zev(&S, Z_CO, ALARM_ZEV_TEST);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 2);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_SENSOR_TEST,
                            sim_last_event(&S, ALARM_EV_WALK_TEST_ZONE)->arg);
    const alarm_zone_status_t zs = sim_zstatus(&S, Z_SMOKE);
    TEST_ASSERT_TRUE((zs.walk & ALARM_WALK_ZS_TESTED) != 0u);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_SENSOR_TEST, ALARM_WALK_ZS_KIND(zs.walk));
    TEST_ASSERT_EQUAL_UINT(1, sim_count_zone_sounds(&S, ALARM_SOUND_WALK_TEST, Z_SMOKE));
    /* Not counted: the count can never read more than the total. */
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).walk_tested);

    /* A burglary sensor's own test marks it, and counts. */
    sim_zev(&S, Z_HALL_PIR, ALARM_ZEV_TEST);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).walk_tested);

    /* A real fire or CO still alarms. */
    sim_fault(&S, Z_CO);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_CO, ALARM_CAUSE_CO));
    sim_fault(&S, Z_SMOKE);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_SMOKE, ALARM_CAUSE_FIRE));
    TEST_ASSERT_TRUE(testing());
}

/* WALK-008, CORE-TEST-006: a tamper marks the cover and beeps; it never alarms, and its trouble is a disarmed
 * partition's. */
static void test_tamper_marks_the_cover(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    sim_advance(&S, SEC(5));
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TAMPER);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TAMPER);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_TROUBLE);
    alarm_zone_status_t zs = sim_zstatus(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_ZS_COVER, zs.walk & (ALARM_WALK_ZS_COVER | ALARM_WALK_ZS_TESTED));
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_COVER, ALARM_WALK_ZS_KIND(zs.walk));
    TEST_ASSERT_EQUAL_UINT16(5, zs.walk_first_s);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).walk_tested); /* a cover isn't the sensor */
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_COVER, sim_last_event(&S, ALARM_EV_WALK_TEST_ZONE)->arg);

    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TAMPER_RESTORE);
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TAMPER);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 1); /* the second cover only beeps */
    TEST_ASSERT_EQUAL_UINT(2, sim_count_zone_sounds(&S, ALARM_SOUND_WALK_TEST, Z_WINDOW));

    press(Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 2);
    zs = sim_zstatus(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_ZS_COVER | ALARM_WALK_ZS_TESTED,
                            zs.walk & (ALARM_WALK_ZS_COVER | ALARM_WALK_ZS_TESTED));
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_MARK_COVER, ALARM_WALK_ZS_KIND(zs.walk)); /* first */
    TEST_ASSERT_EQUAL_UINT8(3, zs.walk_trips);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).walk_tested);

    /* A fire zone's tamper is outside the test: a trouble, no mark. */
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_TAMPER);
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_SMOKE).walk);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* WALK-004, CORE-TEST-003: one double beep at 5 minutes and one at 1 minute, through the engine's deadline;
 * the limit ends it (WALK-010: with the counts). */
static void test_end_warnings_and_the_time_limit(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    const alarm_ms_t start = S.now;
    TEST_ASSERT_EQUAL_UINT64(start + SEC(3300), alarm_engine_next_deadline(&S.eng));
    TEST_ASSERT_EQUAL_UINT64(SEC(3600), sim_status(&S, 1).walk_test_remaining_ms);
    press(Z_WINDOW);

    sim_advance(&S, SEC(3299));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 0);
    sim_advance(&S, SEC(1));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 1);
    TEST_ASSERT_EQUAL_UINT8(5, sim_last_event(&S, ALARM_EV_WALK_TEST_WARNING)->arg);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_WALK_TEST_WARNING));
    TEST_ASSERT_EQUAL_UINT64(start + SEC(3540), alarm_engine_next_deadline(&S.eng));

    sim_advance(&S, SEC(240));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 2);
    TEST_ASSERT_EQUAL_UINT8(1, sim_last_event(&S, ALARM_EV_WALK_TEST_WARNING)->arg);
    TEST_ASSERT_EQUAL_UINT(2, sim_count_sounds(&S, ALARM_SOUND_WALK_TEST_WARNING));
    TEST_ASSERT_EQUAL_UINT64(SEC(60), sim_status(&S, 1).walk_test_remaining_ms);
    TEST_ASSERT_TRUE(testing());

    sim_advance(&S, SEC(60));
    TEST_ASSERT_FALSE(testing());
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 2);
    TEST_ASSERT_EQUAL_UINT(2, sim_count_sounds(&S, ALARM_SOUND_WALK_TEST_WARNING));
    const alarm_event_t *end = sim_last_report(&S, ALARM_EV_WALK_TEST_END);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_UINT8(0, end->user);
    TEST_ASSERT_EQUAL_UINT8(1, end->arg);
    TEST_ASSERT_EQUAL_UINT8(COVERED, end->arg2);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_TIME_LIMIT, st.walk_end_how);
    TEST_ASSERT_EQUAL_UINT64(0, st.walk_test_remaining_ms);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_engine_next_deadline(&S.eng));
}

/* WALK-004: a short limit never warns the moment it starts. */
static void test_a_short_limit_warns_only_where_it_can(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.walk_test_timeout_s = 200u;
    sim_start(&S, &cfg, NULL);
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 0);
    sim_advance(&S, SEC(200));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 1);
    TEST_ASSERT_EQUAL_UINT8(1, sim_last_event(&S, ALARM_EV_WALK_TEST_WARNING)->arg);
    TEST_ASSERT_FALSE(testing());
}

/* WALK-004, CORE-TEST-003: Extend starts the full limit again, warnings included, and is logged. */
static void test_extend_starts_the_limit_again(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_PMASTER, ALARM_WALK_START, 0));
    sim_advance(&S, SEC(3400));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 1);
    press(Z_WINDOW);
    const uint32_t seq = sim_status(&S, 1).walk_seq;

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_PMASTER, ALARM_WALK_EXTEND, 0));
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT64(SEC(3600), st.walk_test_remaining_ms);
    TEST_ASSERT_EQUAL_UINT16(1, st.walk_tested); /* the marks stay */
    TEST_ASSERT_EQUAL_UINT32(seq, st.walk_seq);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_WALK_TEST_EXTEND);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(7, ev->user);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, ev->source.kind);

    sim_advance(&S, SEC(3300));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_WARNING, 2);
    TEST_ASSERT_TRUE(testing());
    sim_advance(&S, SEC(300));
    TEST_ASSERT_FALSE(testing());
}

/* WALK-010, WALK-006, CORE-TEST-006: the end keeps a summary and the marks until the next start, which
 * clears only its own partition's marks. */
static void test_the_summary_stays_until_the_next_start(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_PERIMETER, 2);
    sim_start(&S, &cfg, NULL);

    EXPECT_RESULT(ALARM_OK, walk(2, CODE_MASTER, ALARM_WALK_START, 0));
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 2).walk_total);
    press(30);
    EXPECT_RESULT(ALARM_OK, walk(2, CODE_MASTER, ALARM_WALK_END, 0));

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    const alarm_ms_t started = S.now;
    press(Z_WINDOW);
    press(Z_HALL_PIR);
    sim_advance(&S, SEC(90));
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_INSTALLER, ALARM_WALK_TOGGLE, 0));
    const alarm_event_t *end = sim_last_report(&S, ALARM_EV_WALK_TEST_END);
    TEST_ASSERT_EQUAL_UINT8(2, end->arg);
    TEST_ASSERT_EQUAL_UINT8(COVERED, end->arg2);

    sim_advance(&S, SEC(600));
    alarm_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    alarm_engine_snapshot(&S.eng, &snap);
    const alarm_partition_status_t *st = &snap.part[0];
    TEST_ASSERT_FALSE(st->walk_test);
    TEST_ASSERT_EQUAL_UINT16(2, st->walk_tested);
    TEST_ASSERT_EQUAL_UINT16(COVERED, st->walk_total);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_COMMAND, st->walk_end_how);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, st->walk_ended_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, st->walk_started_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, st->walk_started_src.kind);
    TEST_ASSERT_EQUAL_UINT64(started, st->walk_started_ms);
    TEST_ASSERT_TRUE((snap.zone[Z_WINDOW - 1u].walk & ALARM_WALK_ZS_TESTED) != 0u);
    TEST_ASSERT_TRUE((snap.zone[30 - 1u].walk & ALARM_WALK_ZS_TESTED) != 0u);
    const uint32_t seq = st->walk_seq;

    /* Nothing marks a zone between tests. */
    press(Z_FRONT);
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_FRONT).walk);

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_WINDOW).walk);
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_WINDOW).walk_trips);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).walk_tested);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_NONE, sim_status(&S, 1).walk_end_how);
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_seq > seq);
    TEST_ASSERT_TRUE((sim_zstatus(&S, 30).walk & ALARM_WALK_ZS_TESTED) != 0u); /* partition 2 */
}

/* WALK-006, CORE-TEST-006 (as CORE-CFG-018): a zone whose type or partition changes loses its mark. The
 * configuration can't change while a test runs. */
static void test_a_changed_zone_loses_its_mark(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    press(Z_WINDOW);
    press(Z_HALL_PIR);
    alarm_config_t cfg = S.eng.cfg;
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_END, 0));
    const uint32_t seq = sim_status(&S, 1).walk_seq;

    cfg.zones[Z_WINDOW - 1u].type = ALARM_ZT_INTERIOR_FOLLOWER;
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_WINDOW).walk);
    TEST_ASSERT_TRUE((sim_zstatus(&S, Z_HALL_PIR).walk & ALARM_WALK_ZS_TESTED) != 0u);
    TEST_ASSERT_EQUAL_UINT32(seq + 1u, sim_status(&S, 1).walk_seq);
}

/* WALK-004, CORE-TEST-006: a walk test isn't kept through a restart, nor its marks or summary. */
static void test_a_restart_ends_it(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    press(Z_WINDOW);
    alarm_runtime_state_t st;
    alarm_engine_runtime_state(&S.eng, &st);
    S.cfg.opt.power_up_previous_state = true;

    const alarm_config_t cfg = S.cfg;
    sim_start(&S, &cfg, &st);
    const alarm_partition_status_t ps = sim_status(&S, 1);
    TEST_ASSERT_FALSE(ps.walk_test);
    TEST_ASSERT_EQUAL_UINT64(0, ps.walk_started_ms);
    TEST_ASSERT_EQUAL_UINT16(0, ps.walk_tested);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_END_NONE, ps.walk_end_how);
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_WINDOW).walk);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_engine_next_deadline(&S.eng));
    press(Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 0);
}

/* WALK-001: a partition master's code works only on its own partitions. */
static void test_a_partition_master_is_refused_on_another_partition(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_PERIMETER, 2);
    sim_start(&S, &cfg, NULL);

    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(2, CODE_PMASTER, ALARM_WALK_START, 0));
    TEST_ASSERT_FALSE(sim_status(&S, 2).walk_test);
    EXPECT_RESULT(ALARM_OK, walk(2, CODE_MASTER, ALARM_WALK_START, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(2, CODE_PMASTER, ALARM_WALK_EXTEND, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, walk(2, CODE_PMASTER, ALARM_WALK_END, 0));
    TEST_ASSERT_TRUE(sim_status(&S, 2).walk_test);
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_PMASTER, ALARM_WALK_START, 0));
}

/* WALK-007, WALK-008: a 24-hour zone's tamper without the option behaves as it always did: a
 * trouble, no mark, no walk-test beep. */
static void test_a_24_hour_tamper_without_the_option_is_untouched(void)
{
    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    sim_clear_records(&S);
    sim_zev(&S, Z_PANIC, ALARM_ZEV_TAMPER);
    EXPECT_ZONE_FLAG(&S, Z_PANIC, ALARM_ZS_TAMPER);
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, Z_PANIC).walk);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 0);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_zone_sounds(&S, ALARM_SOUND_WALK_TEST, Z_PANIC));
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).walk_tested);
}

/* WALK-007: the sensor's own test on a partition that isn't testing is only a check-in. */
static void test_a_sensor_test_on_a_partition_not_testing_is_a_checkin(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_PERIMETER, 2);
    sim_start(&S, &cfg, NULL);

    EXPECT_RESULT(ALARM_OK, walk(1, CODE_MASTER, ALARM_WALK_START, 0));
    sim_clear_records(&S);
    sim_zev(&S, 30, ALARM_ZEV_TEST);
    TEST_ASSERT_EQUAL_UINT8(0, sim_zstatus(&S, 30).walk);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 0);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_sounds(&S, ALARM_SOUND_WALK_TEST));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    TEST_ASSERT_FALSE(sim_status(&S, 2).walk_test);
}

/* Event and sound names are a contract (strings.c): the new ones have theirs. */
static void test_new_names(void)
{
    TEST_ASSERT_EQUAL_STRING("walk_test_extend", alarm_event_name(ALARM_EV_WALK_TEST_EXTEND));
    TEST_ASSERT_EQUAL_STRING("walk_test_warning", alarm_event_name(ALARM_EV_WALK_TEST_WARNING));
    TEST_ASSERT_EQUAL_STRING("walk_test_warning", alarm_sound_name(ALARM_SOUND_WALK_TEST_WARNING));
    TEST_ASSERT_EQUAL_STRING("walk_test_zone", alarm_event_name(ALARM_EV_WALK_TEST_ZONE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_end_and_extend_are_separate);
    RUN_TEST(test_start_needs_a_quiet_disarmed_partition);
    RUN_TEST(test_who_starts_extends_and_ends);
    RUN_TEST(test_end_needs_no_code);
    RUN_TEST(test_signed_in_users_follow_the_same_rules);
    RUN_TEST(test_first_trip_marks_and_logs_repeats_only_beep);
    RUN_TEST(test_buttons_are_marked_and_arm_nothing);
    RUN_TEST(test_24_hour_zones_only_with_the_option);
    RUN_TEST(test_fire_and_co_stay_live_but_their_test_marks_them);
    RUN_TEST(test_tamper_marks_the_cover);
    RUN_TEST(test_end_warnings_and_the_time_limit);
    RUN_TEST(test_a_short_limit_warns_only_where_it_can);
    RUN_TEST(test_extend_starts_the_limit_again);
    RUN_TEST(test_the_summary_stays_until_the_next_start);
    RUN_TEST(test_a_changed_zone_loses_its_mark);
    RUN_TEST(test_a_restart_ends_it);
    RUN_TEST(test_a_partition_master_is_refused_on_another_partition);
    RUN_TEST(test_a_24_hour_tamper_without_the_option_is_untouched);
    RUN_TEST(test_a_sensor_test_on_a_partition_not_testing_is_a_checkin);
    RUN_TEST(test_new_names);
    return UNITY_END();
}
