/*
 * test_exit_entry.c - exit time, auto-stay, exit error, entry time, recent closing.
 * Requirements: CORE-EXIT-*, CORE-ENTRY-*
 */
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

/* CORE-EXIT-001: slow beeps, fast beeps in the last 10 s. */
static void test_exit_sounds_slow_then_fast(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_EXIT);
    sim_advance(&S, SEC(49));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_EXIT);
    sim_advance(&S, SEC(1));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_EXIT_FAST);
}

/* CORE-EXIT-002: silent exit mutes beeps and doubles exit time. */
static void test_silent_exit_doubles_exit_time(void)
{
    EXPECT_RESULT(ALARM_OK,
                  sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT));
    TEST_ASSERT_EQUAL_UINT64(SEC(120), sim_status(&S, 1).delay_remaining_ms);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    sim_advance(&S, SEC(61));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    sim_advance(&S, SEC(60));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
}

/* CORE-EXIT-003: violation, restore, violation restarts exit time once. */
static void test_exit_restart_on_reentry_once(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(3));
    sim_restore(&S, Z_FRONT); /* left */
    sim_advance(&S, SEC(12));
    sim_fault(&S, Z_FRONT); /* came back in */
    EXPECT_EVENTS(&S, ALARM_EV_EXIT_RESTART, 1);
    TEST_ASSERT_EQUAL_UINT8(1, sim_last_event(&S, ALARM_EV_EXIT_RESTART)->arg);
    TEST_ASSERT_EQUAL_UINT64(SEC(60), sim_status(&S, 1).delay_remaining_ms);
    sim_advance(&S, SEC(5));
    sim_restore(&S, Z_FRONT);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT); /* second re-entry: no further restart */
    EXPECT_EVENTS(&S, ALARM_EV_EXIT_RESTART, 1);
    TEST_ASSERT_EQUAL_UINT64(SEC(50), sim_status(&S, 1).delay_remaining_ms);
    sim_advance(&S, SEC(2));
    sim_restore(&S, Z_FRONT);
    sim_advance(&S, SEC(48));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-EXIT-004: auto-stay (unvacated premises). */
static void test_auto_stay_when_nobody_leaves(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(61));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    EXPECT_REPORTS(&S, ALARM_EV_AUTO_STAY, 1);
    sim_fault(&S, Z_HALL_PIR);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-EXIT-005: no auto-stay for remote (MQTT/HA) arming. */
static void test_no_auto_stay_for_remote_arming(void)
{
    S.keypad.kind = ALARM_SRC_REMOTE;
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(61));
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    EXPECT_EVENTS(&S, ALARM_EV_AUTO_STAY, 0);
}

/* CORE-EXIT-006: no auto-stay for keyfob arming. */
static void test_no_auto_stay_for_keyfob(void)
{
    sim_fault(&S, Z_FOB_AWAY);
    sim_restore(&S, Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_ZONE, sim_last_event(&S, ALARM_EV_ARMED)->source.kind);
    sim_advance(&S, SEC(61));
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
}

/* CORE-EXIT-007: exit error disarmed during entry time = canceled alarm, nothing sent. */
static void test_exit_error_disarmed_is_canceled_alarm(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(10));
    sim_fault(&S, Z_FRONT); /* door left open */
    sim_advance(&S, SEC(50));
    const alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    TEST_ASSERT_TRUE(st.exit_error);
    TEST_ASSERT_EQUAL_UINT64(SEC(30), st.delay_remaining_ms);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ALARM); /* local alarm sounds at once */
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
    EXPECT_EVENTS(&S, ALARM_EV_EXIT_ERROR, 1);
    EXPECT_REPORTS(&S, ALARM_EV_EXIT_ERROR, 0);

    sim_advance(&S, SEC(10));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM_ABORTED, 1);
    const alarm_partition_status_t after = sim_status(&S, 1);
    TEST_ASSERT_TRUE((after.memory & ALARM_MEM_CANCELED) != 0u);
    TEST_ASSERT_TRUE((after.memory & (1u << ALARM_CAUSE_EXIT_ERROR)) != 0u);
    EXPECT_ZONE_FLAG(&S, Z_FRONT, ALARM_ZS_ALARM);
    sim_advance(&S, SEC(600));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-EXIT-008: exit error not disarmed = exit alarm, reported after abort window. */
static void test_exit_error_becomes_exit_alarm(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(10));
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(50)); /* exit error */
    sim_advance(&S, SEC(30)); /* entry time expires */
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_FRONT, ALARM_CAUSE_EXIT_ERROR));
    TEST_ASSERT_FALSE(sim_alarm_reported(&S, Z_FRONT, ALARM_CAUSE_EXIT_ERROR));
    sim_advance(&S, SEC(30)); /* abort window */
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_FRONT, ALARM_CAUSE_EXIT_ERROR));
}

/* CORE-ENTRY-001 */
static void test_entry_delay_then_disarm(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    TEST_ASSERT_EQUAL_UINT64(SEC(30), sim_status(&S, 1).delay_remaining_ms);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ENTRY);
    EXPECT_EVENTS(&S, ALARM_EV_ENTRY_DELAY, 1);
    sim_advance(&S, SEC(20));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    sim_advance(&S, SEC(120));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-ENTRY-002 */
static void test_entry_delay_2_is_longer(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_GARAGE);
    TEST_ASSERT_EQUAL_UINT64(SEC(45), sim_status(&S, 1).delay_remaining_ms);
}

/* CORE-ENTRY-003 */
static void test_entry_expiry_alarms_and_reports_after_abort_window(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(30));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_FRONT, ALARM_CAUSE_BURGLARY));
    const alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ALARM);
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
    TEST_ASSERT_TRUE(st.triggered);
    sim_advance(&S, SEC(29));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 0);
    sim_advance(&S, SEC(1));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 1);
}

/* CORE-ENTRY-004 */
static void test_follower_follows_entry_delay(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_HALL_PIR);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_advance(&S, SEC(10));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    sim_advance(&S, SEC(60));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-ENTRY-005 */
static void test_followers_alarm_at_entry_expiry(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_HALL_PIR);
    sim_advance(&S, SEC(25));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_FRONT, ALARM_CAUSE_BURGLARY));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_HALL_PIR, ALARM_CAUSE_BURGLARY));
}

/* CORE-ENTRY-006 */
static void test_interior_first_is_instant(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_HALL_PIR);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_HALL_PIR, ALARM_CAUSE_BURGLARY));
}

/* CORE-ENTRY-007 */
static void test_interior_delay_zone_starts_entry(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_FAMILY);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    TEST_ASSERT_EQUAL_UINT64(SEC(30), sim_status(&S, 1).delay_remaining_ms);
}

/* CORE-ENTRY-008: MAXIMUM = away with no entry delay. */
static void test_maximum_has_no_entry_delay(void)
{
    EXPECT_RESULT(ALARM_OK,
                  sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, ALARM_ARM_NO_ENTRY_DELAY));
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(3));
    sim_restore(&S, Z_FRONT);
    sim_advance(&S, SEC(60));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    sim_fault(&S, Z_FRONT);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_FRONT, ALARM_CAUSE_BURGLARY));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
}

/* CORE-ENTRY-009: INSTANT = stay with no entry delay. Like every mode it keeps the
 * exit time (silent, CORE-EXIT-017); once armed, the doors alarm at once. */
static void test_instant_has_no_entry_delay(void)
{
    EXPECT_RESULT(ALARM_OK,
                  sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT64(SEC(60), sim_status(&S, 1).delay_remaining_ms);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARM_NO_ENTRY_DELAY, sim_status(&S, 1).arm_flags);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT); /* the door during exit time: no alarm */
    sim_advance(&S, SEC(2));
    sim_restore(&S, Z_FRONT);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_advance(&S, SEC(54));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED); /* no entry time */
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_FRONT, ALARM_CAUSE_BURGLARY));
}

/* CORE-EXIT-017: exit beeps only for Away; Stay, Night and Instant exit time is silent after
 * the arming beep. */
static void test_exit_beeps_only_for_away(void)
{
    static const struct {
        alarm_arm_level_t level;
        uint8_t flags;
    } silent[] = {
        {ALARM_LEVEL_STAY, 0u},
        {ALARM_LEVEL_NIGHT, 0u},
        {ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY},
        {ALARM_LEVEL_NIGHT, ALARM_ARM_NO_ENTRY_DELAY},
    };
    for (size_t i = 0; i < sizeof(silent) / sizeof(silent[0]); i++) {
        sim_start(&S, NULL, NULL);
        EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, silent[i].level, silent[i].flags));
        TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ACK));
        EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
        EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
        sim_advance(&S, SEC(55));
        EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE); /* no fast beeps either */
        sim_advance(&S, SEC(6));
        EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    }
    /* Maximum (Away, no entry delay) beeps like Away. */
    sim_start(&S, NULL, NULL);
    EXPECT_RESULT(ALARM_OK,
                  sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, ALARM_ARM_NO_ENTRY_DELAY));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_EXIT);
    /* Quick exit from Stay restarts a silent exit time. */
    sim_start(&S, NULL, NULL);
    sim_arm_stay_settled(&S, CODE_USER);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, ""));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
}

/* CORE-EXIT-018: silent exit is Away's; Stay and Night ignore it: their exit
 * time is silent anyway and isn't doubled. */
static void test_silent_exit_is_away_only(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY,
                                    ALARM_ARM_SILENT_EXIT | ALARM_ARM_NO_ENTRY_DELAY));
    TEST_ASSERT_EQUAL_UINT64(SEC(60), sim_status(&S, 1).delay_remaining_ms);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARM_NO_ENTRY_DELAY, sim_status(&S, 1).arm_flags);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARM_NO_ENTRY_DELAY, sim_last_report(&S, ALARM_EV_ARMED)->arg2);
}

/* CORE-EXIT-009: perimeter is armed immediately at arming. */
static void test_perimeter_during_exit_delay_is_instant(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(10));
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
}

/* CORE-EXIT-010: quick exit ([*] / # 9) while armed stay. */
static void test_quick_exit_in_stay(void)
{
    sim_arm_stay_settled(&S, CODE_USER);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, ""));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(3));
    sim_restore(&S, Z_FRONT);
    sim_advance(&S, SEC(60));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* LINK-028: the snapshot tells a quick exit's time (from the armed state) from an arming's,
 * whose own re-entry restart stays the arming's; arming again clears it. */
static void test_quick_exit_is_flagged(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    TEST_ASSERT_FALSE(sim_status(&S, 1).quick_exit);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(3));
    sim_restore(&S, Z_FRONT);
    sim_advance(&S, SEC(12));
    sim_fault(&S, Z_FRONT); /* came back in: the arming's exit time restarts */
    EXPECT_EVENTS(&S, ALARM_EV_EXIT_RESTART, 1);
    TEST_ASSERT_FALSE(sim_status(&S, 1).quick_exit);
    sim_restore(&S, Z_FRONT);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));

    sim_arm_stay_settled(&S, CODE_USER);
    TEST_ASSERT_FALSE(sim_status(&S, 1).quick_exit);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, ""));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_TRUE(sim_status(&S, 1).quick_exit);
    sim_advance(&S, SEC(61));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_FALSE(sim_status(&S, 1).quick_exit); /* only while its exit time runs */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_FALSE(sim_status(&S, 1).quick_exit);
}

/* CORE-EXIT-011 */
static void test_quick_exit_not_in_away(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, ""));
}

/* CORE-EXIT-012: quick exit without a code can be disabled. */
static void test_quick_exit_option(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.quick_exit = false;
    sim_start(&S, &cfg, NULL);
    sim_arm_stay_settled(&S, CODE_USER);
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, ""));
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, CODE_USER));
}

/* CORE-EXIT-013: momentary exit zone (type 29). */
static void test_momentary_exit_zone(void)
{
    sim_arm_stay_settled(&S, CODE_USER);
    sim_fault(&S, Z_MOM_EXIT);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
}

/* CORE-EXIT-014: recent closing reported together with the alarm. */
static void test_recent_closing_reported_with_alarm(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_advance(&S, SEC(30));
    sim_fault(&S, Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_RECENT_CLOSING, 1);
    EXPECT_REPORTS(&S, ALARM_EV_RECENT_CLOSING, 0);
    sim_advance(&S, SEC(30));
    EXPECT_REPORTS(&S, ALARM_EV_RECENT_CLOSING, 1);
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 1);
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_report(&S, ALARM_EV_RECENT_CLOSING)->user);
}

/* CORE-EXIT-015 */
static void test_no_recent_closing_after_two_minutes(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_advance(&S, SEC(200));
    sim_fault(&S, Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_RECENT_CLOSING, 0);
}

/* CORE-ENTRY-010: rapid entry beeps when an alarm happened while armed. */
static void test_entry_with_alarm_memory_beeps_rapidly(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, SEC(241)); /* bell timed out */
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ENTRY_MEMORY);
}

/* CORE-EXIT-016: exit error on an interior zone still faulted at exit expiry. */
static void test_exit_error_on_interior_zone(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_FRONT);
    sim_advance(&S, SEC(3));
    sim_restore(&S, Z_FRONT);
    sim_fault(&S, Z_HALL_PIR); /* something still moving */
    sim_advance(&S, SEC(60));
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    TEST_ASSERT_TRUE(sim_status(&S, 1).exit_error);
    TEST_ASSERT_EQUAL_UINT16(Z_HALL_PIR, sim_last_event(&S, ALARM_EV_EXIT_ERROR)->zone);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exit_sounds_slow_then_fast);
    RUN_TEST(test_silent_exit_doubles_exit_time);
    RUN_TEST(test_exit_restart_on_reentry_once);
    RUN_TEST(test_auto_stay_when_nobody_leaves);
    RUN_TEST(test_no_auto_stay_for_remote_arming);
    RUN_TEST(test_no_auto_stay_for_keyfob);
    RUN_TEST(test_exit_error_disarmed_is_canceled_alarm);
    RUN_TEST(test_exit_error_becomes_exit_alarm);
    RUN_TEST(test_entry_delay_then_disarm);
    RUN_TEST(test_entry_delay_2_is_longer);
    RUN_TEST(test_entry_expiry_alarms_and_reports_after_abort_window);
    RUN_TEST(test_follower_follows_entry_delay);
    RUN_TEST(test_followers_alarm_at_entry_expiry);
    RUN_TEST(test_interior_first_is_instant);
    RUN_TEST(test_interior_delay_zone_starts_entry);
    RUN_TEST(test_maximum_has_no_entry_delay);
    RUN_TEST(test_instant_has_no_entry_delay);
    RUN_TEST(test_exit_beeps_only_for_away);
    RUN_TEST(test_silent_exit_is_away_only);
    RUN_TEST(test_perimeter_during_exit_delay_is_instant);
    RUN_TEST(test_quick_exit_in_stay);
    RUN_TEST(test_quick_exit_is_flagged);
    RUN_TEST(test_quick_exit_not_in_away);
    RUN_TEST(test_quick_exit_option);
    RUN_TEST(test_momentary_exit_zone);
    RUN_TEST(test_recent_closing_reported_with_alarm);
    RUN_TEST(test_no_recent_closing_after_two_minutes);
    RUN_TEST(test_entry_with_alarm_memory_beeps_rapidly);
    RUN_TEST(test_exit_error_on_interior_zone);
    return UNITY_END();
}
