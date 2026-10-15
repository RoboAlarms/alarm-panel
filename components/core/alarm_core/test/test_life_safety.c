/*
 * test_life_safety.c - fire (with verification), CO, 24-hour zones, panics, walk test.
 * Requirements: CORE-FIRE-*, CORE-24H-*, CORE-PANIC-*, CORE-TEST-*
 */
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

/* CORE-FIRE-001: fire alarms at any time, report immediately, temporal-3, no timeout. */
static void test_fire_alarm_while_disarmed(void)
{
    sim_fault(&S, Z_SMOKE);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_SMOKE, ALARM_CAUSE_FIRE));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_SMOKE, ALARM_CAUSE_FIRE));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    EXPECT_BELL(&S, 1, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
    sim_advance(&S, SEC(600));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
}

/* CORE-FIRE-002: OFF key (no code) silences fire. */
static void test_fire_silenced_by_off_key(void)
{
    sim_fault(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM_SILENCED, 1);
    TEST_ASSERT_TRUE((sim_status(&S, 1).memory & (1u << ALARM_CAUSE_FIRE)) != 0u);
    EXPECT_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_ALARM);
}

/* CORE-FIRE-003: clearing fire memory resets smoke power. */
static void test_fire_memory_clear_resets_smoke(void)
{
    sim_fault(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* code + OFF: clear */
    EXPECT_EVENTS(&S, ALARM_EV_MEMORY_CLEARED, 1);
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_ON, sim_output(&S, ALARM_OUT_SMOKE_RESET, 0));
    sim_restore(&S, Z_SMOKE); /* detector drops out while unpowered */
    sim_advance(&S, SEC(6));
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_OFF, sim_output(&S, ALARM_OUT_SMOKE_RESET, 0));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 1);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).memory);
}

/* CORE-FIRE-004: smoke still present after the reset -> alarm again. */
static void test_fire_still_present_after_reset_realarms(void)
{
    sim_fault(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* first code+OFF silences */
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* second clears + resets */
    sim_advance(&S, SEC(6));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 2);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
}

/* CORE-FIRE-005: fire verification - reset, then a second trip within 90 s. */
static void test_fire_verification_second_trip(void)
{
    sim_fault(&S, Z_SMOKE_VER);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_ZONE_FLAG(&S, Z_SMOKE_VER, ALARM_ZS_VERIFYING);
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_ON, sim_output(&S, ALARM_OUT_SMOKE_RESET, 0));
    sim_restore(&S, Z_SMOKE_VER);
    sim_advance(&S, SEC(6));
    EXPECT_ZONE_FLAG(&S, Z_SMOKE_VER, ALARM_ZS_VERIFYING);
    sim_advance(&S, SEC(20));
    sim_fault(&S, Z_SMOKE_VER);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_SMOKE_VER, ALARM_CAUSE_FIRE));
    EXPECT_NO_ZONE_FLAG(&S, Z_SMOKE_VER, ALARM_ZS_VERIFYING);
}

/* CORE-FIRE-006 */
static void test_fire_verification_times_out(void)
{
    sim_fault(&S, Z_SMOKE_VER);
    sim_restore(&S, Z_SMOKE_VER);
    sim_advance(&S, SEC(91));
    EXPECT_NO_ZONE_FLAG(&S, Z_SMOKE_VER, ALARM_ZS_VERIFYING);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_fault(&S, Z_SMOKE_VER); /* a new single trip starts over */
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_ZONE_FLAG(&S, Z_SMOKE_VER, ALARM_ZS_VERIFYING);
}

/* CORE-FIRE-007: still in alarm after the reset counts as verified. */
static void test_fire_verification_persistent_trip(void)
{
    sim_fault(&S, Z_SMOKE_VER);
    sim_advance(&S, SEC(6));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_SMOKE_VER, ALARM_CAUSE_FIRE));
}

/* CORE-FIRE-008: CO uses temporal-4. */
static void test_co_alarm(void)
{
    sim_fault(&S, Z_CO);
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_CO, ALARM_CAUSE_CO));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_CO);
    EXPECT_BELL(&S, 1, ALARM_PAT_TEMPORAL4);
}

/* CORE-FIRE-009: fire sounding takes priority over burglary. */
static void test_fire_priority_over_burglary(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_SMOKE);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    EXPECT_BELL(&S, 1, ALARM_PAT_TEMPORAL3);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ALARM); /* burglary needs a code */
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
}

/* CORE-24H-001 */
static void test_24h_audible_while_disarmed(void)
{
    sim_fault(&S, Z_PANIC);
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_PANIC, ALARM_CAUSE_24H_AUDIBLE));
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ALARM);
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_disarm(&S, 1, ""));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM_SILENCED, 1);
}

/* CORE-24H-002: 24 h silent (hold-up) is never annunciated locally. */
static void test_24h_silent_is_hidden(void)
{
    sim_fault(&S, Z_HOLDUP);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_ALARM);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_CAUSE_24H_SILENT, ev->arg);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);
    const alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    TEST_ASSERT_EQUAL_UINT16(0, st.memory);
    TEST_ASSERT_FALSE(st.triggered);
    EXPECT_NO_ZONE_FLAG(&S, Z_HOLDUP, ALARM_ZS_ALARM);
}

/* CORE-24H-003: 24 h auxiliary sounds the keypads only. */
static void test_24h_aux_keypad_only(void)
{
    sim_fault(&S, Z_WATER);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_AUX);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
}

/* CORE-PANIC-001 */
static void test_panic_police(void)
{
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, 0, ALARM_CAUSE_PANIC_POLICE));
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_ALARM);
}

/* CORE-PANIC-002 */
static void test_panic_medical(void)
{
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_MEDICAL));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, 0, ALARM_CAUSE_PANIC_MEDICAL));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_AUX);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
}

/* CORE-PANIC-003 */
static void test_panic_fire(void)
{
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_FIRE));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, 0, ALARM_CAUSE_PANIC_FIRE));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    EXPECT_BELL(&S, 1, ALARM_PAT_TEMPORAL3);
}

/* CORE-PANIC-004 */
static void test_panic_silent(void)
{
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_SILENT));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, 0, ALARM_CAUSE_PANIC_SILENT));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    TEST_ASSERT_FALSE(sim_status(&S, 1).triggered);
}

/* CORE-TEST-001: walk test beeps for burglary zones instead of alarming. */
static void test_walk_test(void)
{
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_MASTER));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    EXPECT_REPORTS(&S, ALARM_EV_WALK_TEST_START, 1);
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_TEST, sim_output(&S, ALARM_OUT_BELL, 1));
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_zone_sounds(&S, ALARM_SOUND_WALK_TEST, Z_WINDOW));
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_ZONE, 1);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_restore(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
    EXPECT_REPORTS(&S, ALARM_EV_WALK_TEST_END, 1);
}

/* CORE-TEST-002: life-safety zones stay live during walk test; code + OFF
 * silences first, the next one ends the test. */
static void test_walk_test_keeps_fire_live(void)
{
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_MASTER));
    sim_fault(&S, Z_SMOKE);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_SMOKE, ALARM_CAUSE_FIRE));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
}

/* CORE-TEST-003 */
static void test_walk_test_times_out(void)
{
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_MASTER));
    sim_advance(&S, SEC(3600));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_END, 1);
}

/* CORE-TEST-004, WALK-001: only the installer, the system master and a partition master start a
 * walk test; a standard or arm-only code may not. A standard code ends one (WALK-025). */
static void test_walk_test_authority(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_ARMONLY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_PMASTER));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fire_alarm_while_disarmed);
    RUN_TEST(test_fire_silenced_by_off_key);
    RUN_TEST(test_fire_memory_clear_resets_smoke);
    RUN_TEST(test_fire_still_present_after_reset_realarms);
    RUN_TEST(test_fire_verification_second_trip);
    RUN_TEST(test_fire_verification_times_out);
    RUN_TEST(test_fire_verification_persistent_trip);
    RUN_TEST(test_co_alarm);
    RUN_TEST(test_fire_priority_over_burglary);
    RUN_TEST(test_24h_audible_while_disarmed);
    RUN_TEST(test_24h_silent_is_hidden);
    RUN_TEST(test_24h_aux_keypad_only);
    RUN_TEST(test_panic_police);
    RUN_TEST(test_panic_medical);
    RUN_TEST(test_panic_fire);
    RUN_TEST(test_panic_silent);
    RUN_TEST(test_walk_test);
    RUN_TEST(test_walk_test_keeps_fire_live);
    RUN_TEST(test_walk_test_times_out);
    RUN_TEST(test_walk_test_authority);
    return UNITY_END();
}
