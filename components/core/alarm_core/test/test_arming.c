/*
 * test_arming.c - readiness, arm levels, bypass, quick arm, chime.
 * Requirements: CORE-ARM-*, CORE-BYP-*, CORE-CHIME-*
 */
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

/* CORE-ARM-001 */
static void test_starts_disarmed_and_ready(void)
{
    const alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_TRUE(st.ready);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY) | ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) |
                                ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT),
                            st.ready_levels);
    EXPECT_EVENTS(&S, ALARM_EV_SYSTEM_START, 1);
    EXPECT_REPORTS(&S, ALARM_EV_SYSTEM_START, 1);
}

/* CORE-ARM-002 */
static void test_arm_away_starts_exit_delay(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    const alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    TEST_ASSERT_EQUAL_UINT64(SEC(60), st.delay_remaining_ms);
    TEST_ASSERT_EQUAL_UINT8(3, st.armed_by);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_EXIT);
    EXPECT_REPORTS(&S, ALARM_EV_ARMED, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_ARMED);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(3, ev->user);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ACK));
}

/* CORE-ARM-003 */
static void test_faulted_zone_blocks_arming(void)
{
    sim_fault(&S, Z_WINDOW);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_FALSE(st.ready);
    TEST_ASSERT_EQUAL_UINT8(0, st.ready_levels);
    TEST_ASSERT_EQUAL_UINT16(1, st.faults);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, S.last.not_ready_zone);
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 1);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-ARM-004: readiness is per level; interior zones do not block STAY. */
static void test_faulted_motion_blocks_away_not_stay(void)
{
    sim_fault(&S, Z_HALL_PIR);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_FALSE(st.ready);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) | ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT),
                            st.ready_levels);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
}

/* CORE-ARM-005: a zone in trouble (CHECK) is not ready; low battery is. */
static void test_zone_trouble_blocks_but_low_battery_does_not(void)
{
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE_RESTORE);
    sim_zev(&S, Z_FRONT, ALARM_ZEV_LOW_BATTERY);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
}

/* CORE-BYP-001 */
static void test_bypass_then_arm_and_disarm_clears_bypass(void)
{
    sim_fault(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_WINDOW));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).bypassed);
    EXPECT_REPORTS(&S, ALARM_EV_BYPASS, 1);

    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(10));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
}

/* CORE-BYP-002: "Bypass and arm" / force arm. */
static void test_arm_with_bypass_faulted_flag(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_HALL_PIR);
    EXPECT_RESULT(ALARM_OK,
                  sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 2);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_HALL_PIR, ALARM_ZS_BYPASSED);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
}

/* CORE-BYP-003 */
static void test_fire_and_24h_zones_not_bypassable(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_BYPASSABLE, sim_bypass(&S, 1, CODE_USER, Z_SMOKE));
    EXPECT_RESULT(ALARM_ERR_NOT_BYPASSABLE, sim_bypass(&S, 1, CODE_USER, Z_CO));
    EXPECT_RESULT(ALARM_ERR_NOT_BYPASSABLE, sim_bypass(&S, 1, CODE_USER, Z_PANIC));
}

/* CORE-BYP-004: a 24 h zone may be bypassed when its zone option allows it. */
static void test_24h_zone_bypassable_by_option(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_PANIC - 1].flags |= ALARM_ZF_BYPASSABLE;
    sim_start(&S, &cfg, NULL);
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_PANIC));
    sim_fault(&S, Z_PANIC);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-BYP-005 */
static void test_bypass_only_while_disarmed(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_bypass(&S, 1, CODE_USER, Z_WINDOW));
}

/* CORE-BYP-006: code + 6 + # */
static void test_quick_bypass(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_HALL_PIR);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_QUICK_BYPASS, 1, CODE_USER));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_HALL_PIR, ALARM_ZS_BYPASSED);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
}

/* CORE-BYP-007 */
static void test_unbypass(void)
{
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_WINDOW));
    alarm_cmd_t cmd = {0};
    cmd.type = ALARM_CMD_UNBYPASS;
    cmd.partition = 1;
    cmd.zone = Z_WINDOW;
    memcpy(cmd.code, CODE_USER, 5);
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &cmd));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_REPORTS(&S, ALARM_EV_UNBYPASS, 1);
}

/* CORE-ARM-006: quick arm (# 2 / # 3) only when enabled. */
static void test_quick_arm_option(void)
{
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_arm(&S, 1, "", ALARM_LEVEL_AWAY, 0));
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.quick_arm = true;
    sim_start(&S, &cfg, NULL);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "", ALARM_LEVEL_AWAY, 0));
    TEST_ASSERT_EQUAL_UINT8(0, sim_status(&S, 1).armed_by);
    /* Disarming always needs a code. */
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_disarm(&S, 1, ""));
}

/* CORE-USR-010 */
static void test_invalid_code_rejected(void)
{
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, "0000", ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, "12345", ALARM_LEVEL_AWAY, 0));
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 2);
    TEST_ASSERT_EQUAL_UINT(2, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-ARM-007 */
static void test_disarm_during_exit_delay(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(10));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_REPORTS(&S, ALARM_EV_DISARMED, 1);
    sim_advance(&S, SEC(120));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-CHIME-001..003 */
static void test_chime(void)
{
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
    EXPECT_EVENTS(&S, ALARM_EV_CHIME_ON, 1);

    sim_fault(&S, Z_FRONT);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_zone_sounds(&S, ALARM_SOUND_CHIME, Z_FRONT));
    sim_restore(&S, Z_FRONT);

    sim_fault(&S, Z_HALL_PIR); /* no chime attribute */
    TEST_ASSERT_EQUAL_UINT(0, sim_count_zone_sounds(&S, ALARM_SOUND_CHIME, Z_HALL_PIR));
    sim_restore(&S, Z_HALL_PIR);

    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    sim_fault(&S, Z_FRONT); /* never chimes while armed */
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_CHIME));

    sim_restore(&S, Z_FRONT);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).chime);
    EXPECT_EVENTS(&S, ALARM_EV_CHIME_OFF, 1);
}

/* CORE-CHIME-004: chime is changed only while disarmed. */
static void test_chime_only_while_disarmed(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    sim_advance(&S, SEC(61));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).chime);
    EXPECT_EVENTS(&S, ALARM_EV_CHIME_ON, 0);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
}

/* CORE-CHIME-005: with the installer option off, chime toggles without a code (one touch);
 * everything else about chime stays. */
static void test_chime_without_code_option(void)
{
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_simple(&S, ALARM_CMD_CHIME, 1, ""));
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.chime_needs_code = false;
    sim_start(&S, &cfg, NULL);

    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, ""));
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
    EXPECT_EVENTS(&S, ALARM_EV_CHIME_ON, 1);
    TEST_ASSERT_EQUAL_UINT8(0, sim_last_event(&S, ALARM_EV_CHIME_ON)->user); /* nobody typed */

    /* A code still works, and names the user. */
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).chime);
    TEST_ASSERT_TRUE(sim_last_event(&S, ALARM_EV_CHIME_OFF)->user != 0u);
    /* A wrong code is still a wrong code. */
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_simple(&S, ALARM_CMD_CHIME, 1, "0000"));

    /* CORE-CHIME-004 still holds: only while disarmed. */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_simple(&S, ALARM_CMD_CHIME, 1, ""));
}

/* CORE-CHIME-007: a remote client (Home Assistant's link, MQTT) switches the chime without a
 * code even with "chime needs a code" on (BL-172); everything else about chime stays. */
static void test_chime_from_remote_without_code(void)
{
    TEST_ASSERT_TRUE(S.cfg.opt.chime_needs_code); /* the default */
    S.keypad.kind = ALARM_SRC_REMOTE;
    S.keypad.id = 2u;

    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, ""));
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
    TEST_ASSERT_EQUAL_UINT8(0, sim_last_event(&S, ALARM_EV_CHIME_ON)->user); /* nobody typed */
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_REMOTE, sim_last_event(&S, ALARM_EV_CHIME_ON)->source.kind);

    /* A code from there still names the user, and a wrong one is still refused. */
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).chime);
    TEST_ASSERT_TRUE(sim_last_event(&S, ALARM_EV_CHIME_OFF)->user != 0u);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_simple(&S, ALARM_CMD_CHIME, 1, "0000"));

    /* CORE-CHIME-004 still holds: only while disarmed. */
    S.keypad.kind = ALARM_SRC_KEYPAD;
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    S.keypad.kind = ALARM_SRC_REMOTE;
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_simple(&S, ALARM_CMD_CHIME, 1, ""));

    /* The keypad still needs the code. */
    S.keypad.kind = ALARM_SRC_KEYPAD;
    S.keypad.id = 1u;
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_simple(&S, ALARM_CMD_CHIME, 1, ""));
}

/* CORE-CHIME-006: a bypassed zone doesn't chime; unbypassed, it chimes again. */
static void test_bypassed_zone_does_not_chime(void)
{
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_FRONT));

    sim_fault(&S, Z_FRONT);
    sim_restore(&S, Z_FRONT);
    sim_fault(&S, Z_FRONT);
    sim_restore(&S, Z_FRONT);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_sounds(&S, ALARM_SOUND_CHIME));

    alarm_cmd_t cmd = {0};
    cmd.type = ALARM_CMD_UNBYPASS;
    cmd.partition = 1;
    cmd.zone = Z_FRONT;
    memcpy(cmd.code, CODE_USER, 5);
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &cmd));
    sim_fault(&S, Z_FRONT);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_zone_sounds(&S, ALARM_SOUND_CHIME, Z_FRONT));
}

/* CORE-ARM-008 */
static void test_stay_bypasses_interior(void)
{
    sim_arm_stay_settled(&S, CODE_USER);
    sim_fault(&S, Z_HALL_PIR);
    sim_fault(&S, Z_FAMILY);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
}

/* CORE-ARM-009 */
static void test_night_arms_night_zones(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_NIGHT, 0));
    sim_advance(&S, SEC(61));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_NIGHT);
    sim_fault(&S, Z_HALL_PIR);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_fault(&S, Z_UPSTAIRS);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_UPSTAIRS, ALARM_CAUSE_BURGLARY));
}

/* CORE-ARM-010: the arm level can be changed from armed stay. */
static void test_arm_away_from_armed_stay(void)
{
    sim_arm_stay_settled(&S, CODE_USER);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
}

/* CORE-ARM-011 */
static void test_arm_rejected_while_sounding_or_delaying(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
}

/* CORE-ARM-013 */
static void test_platform_holds_arming(void)
{
    alarm_engine_hold_arming(&S.eng, true);
    TEST_ASSERT_TRUE(alarm_engine_arming_held(&S.eng));
    alarm_partition_status_t ps;
    alarm_engine_partition_status(&S.eng, 1, &ps);
    TEST_ASSERT_FALSE(ps.ready);
    TEST_ASSERT_EQUAL_HEX8(0u, ps.ready_levels);
    EXPECT_RESULT(ALARM_ERR_HELD, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_ERR_HELD, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE)); /* panics go on */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    alarm_engine_hold_arming(&S.eng, false);
    alarm_engine_partition_status(&S.eng, 1, &ps);
    TEST_ASSERT_TRUE(ps.ready);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
}

/* CORE-ARM-012 */
static void test_disarm_needs_code(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, sim_disarm(&S, 1, ""));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_disarmed_and_ready);
    RUN_TEST(test_arm_away_starts_exit_delay);
    RUN_TEST(test_faulted_zone_blocks_arming);
    RUN_TEST(test_faulted_motion_blocks_away_not_stay);
    RUN_TEST(test_zone_trouble_blocks_but_low_battery_does_not);
    RUN_TEST(test_bypass_then_arm_and_disarm_clears_bypass);
    RUN_TEST(test_arm_with_bypass_faulted_flag);
    RUN_TEST(test_fire_and_24h_zones_not_bypassable);
    RUN_TEST(test_24h_zone_bypassable_by_option);
    RUN_TEST(test_bypass_only_while_disarmed);
    RUN_TEST(test_quick_bypass);
    RUN_TEST(test_unbypass);
    RUN_TEST(test_quick_arm_option);
    RUN_TEST(test_invalid_code_rejected);
    RUN_TEST(test_disarm_during_exit_delay);
    RUN_TEST(test_chime);
    RUN_TEST(test_chime_only_while_disarmed);
    RUN_TEST(test_chime_without_code_option);
    RUN_TEST(test_chime_from_remote_without_code);
    RUN_TEST(test_bypassed_zone_does_not_chime);
    RUN_TEST(test_stay_bypasses_interior);
    RUN_TEST(test_night_arms_night_zones);
    RUN_TEST(test_arm_away_from_armed_stay);
    RUN_TEST(test_arm_rejected_while_sounding_or_delaying);
    RUN_TEST(test_platform_holds_arming);
    RUN_TEST(test_disarm_needs_code);
    return UNITY_END();
}
