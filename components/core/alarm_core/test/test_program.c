/*
 * test_program.c - installer mode, the power-up recovery window, installer code programming
 * and platform events.
 * Requirements: CORE-PROG-*
 */
#include "alarm/alarm_snapshot.h"
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
    sim_clear_records(&S);
}

void tearDown(void) {}

static alarm_result_t enter(const char *code, uint8_t *user)
{
    return alarm_engine_program_enter(&S.eng, &S.keypad, code, S.now, user);
}

static void leave(void)
{
    alarm_engine_program_exit(&S.eng, &S.keypad, S.now);
}

static alarm_snapshot_t snapshot(void)
{
    static alarm_snapshot_t snap;
    alarm_engine_snapshot(&S.eng, &snap);
    return snap;
}

/* CORE-PROG-001: the installer code (only) enters installer mode; logged and reported. */
static void test_installer_code_enters_installer_mode(void)
{
    uint8_t user = 99;
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, enter(CODE_MASTER, &user));
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, enter("8888", &user));
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 1);
    TEST_ASSERT_FALSE(alarm_engine_program_active(&S.eng));

    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, &user));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, user);
    TEST_ASSERT_TRUE(alarm_engine_program_active(&S.eng));
    TEST_ASSERT_TRUE(snapshot().program);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_PROGRAM_ENTER);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    TEST_ASSERT_EQUAL_UINT8(0, ev->arg);
    TEST_ASSERT_EQUAL_UINT16(1, ev->source.id);

    /* Again from the same keypad: just refreshed. Another keypad: refused. */
    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    EXPECT_EVENTS(&S, ALARM_EV_PROGRAM_ENTER, 1);
    const alarm_source_t other = {ALARM_SRC_KEYPAD, 2};
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_program_enter(&S.eng, &other, CODE_INSTALLER, S.now, NULL));
}

/* CORE-PROG-002: only while every partition is disarmed, silent and out of walk test. */
static void test_installer_mode_only_when_idle(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, enter(CODE_INSTALLER, NULL)); /* exit time */
    sim_advance(&S, SEC(61));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, enter(CODE_INSTALLER, NULL)); /* armed */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    sim_fault(&S, Z_SMOKE);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, enter(CODE_INSTALLER, NULL)); /* fire sounding */
    sim_restore(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));

    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, enter(CODE_INSTALLER, NULL)); /* walk test */
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_MASTER));

    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
}

/* CORE-PROG-003: nothing can be armed in installer mode; fire, CO, 24 h and panics stay live. */
static void test_installer_mode_blocks_arming_only(void)
{
    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    sim_fault(&S, Z_FOB_AWAY); /* keyfob arm button */
    sim_restore(&S, Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 2);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_simple(&S, ALARM_CMD_WALK_TEST, 1, CODE_MASTER));

    sim_fault(&S, Z_WATER);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WATER, ALARM_CAUSE_24H_AUX));
    sim_fault(&S, Z_SMOKE);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_SMOKE, ALARM_CAUSE_FIRE));
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, 0, ALARM_CAUSE_PANIC_POLICE));
}

static const alarm_source_t PM = {ALARM_SRC_REMOTE, ALARM_SRC_ID_PANEL_MANAGER};

/* CORE-PROG-011: the signed-in installer enters installer mode without a code; logged and
 * reported as the coded entry, by that user and source - never as a power-up recovery, even
 * while the recovery window is open. */
static void test_program_enter_as_installer(void)
{
    TEST_ASSERT_TRUE(snapshot().recovery_left_ms > 0u);
    EXPECT_RESULT(ALARM_OK, alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    TEST_ASSERT_TRUE(alarm_engine_program_active(&S.eng));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_PROGRAM_ENTER);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    TEST_ASSERT_EQUAL_UINT8(0, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_REMOTE, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);

    /* Again from the same source: refreshed, not logged twice; the codes can be set. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    EXPECT_EVENTS(&S, ALARM_EV_PROGRAM_ENTER, 1);
    EXPECT_RESULT(ALARM_OK, alarm_engine_program_set_code(&S.eng, &PM, ALARM_USER_MASTER,
                                                          "8642", S.now));
    alarm_engine_program_exit(&S.eng, &PM, S.now);
    TEST_ASSERT_FALSE(alarm_engine_program_active(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 0);
}

/* CORE-PROG-011: installer authority only, asked again now; nothing counts toward a lockout. */
static void test_program_enter_as_refusals(void)
{
    for (int i = 0; i < 4; i++) {
        EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                      alarm_engine_program_enter(&S.eng, &PM, "8888", S.now, NULL));
    }
    const uint8_t nobody[] = {0u, ALARM_USER_MASTER, 3u, 7u, 12u, (uint8_t)(ALARM_MAX_USERS + 1u)};
    for (unsigned i = 0; i < sizeof(nobody); i++) {
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                      alarm_engine_program_enter_as(&S.eng, &PM, nobody[i], S.now));
        TEST_ASSERT_FALSE(alarm_engine_program_active(&S.eng));
    }
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 4);
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 0);
    EXPECT_EVENTS(&S, ALARM_EV_PROGRAM_ENTER, 0);

    /* An accepted entry does not reset the count either: the fifth wrong code still locks. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    alarm_engine_program_exit(&S.eng, &PM, S.now);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                  alarm_engine_program_enter(&S.eng, &PM, "8888", S.now, NULL));
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 1);
}

/* CORE-PROG-011: the coded entry's conditions: every partition idle, one source at a time. */
static void test_program_enter_as_conditions(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    sim_advance(&S, SEC(61));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    /* Someone is in installer mode at the keypad: the Panel Manager waits, and the other way. */
    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    leave();
    EXPECT_RESULT(ALARM_OK, alarm_engine_program_enter_as(&S.eng, &PM, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, enter(CODE_INSTALLER, NULL));
    EXPECT_EVENTS(&S, ALARM_EV_PROGRAM_ENTER, 2);
}

/* CORE-PROG-004: left with *99 or after 15 minutes without activity. */
static void test_installer_mode_exit_and_timeout(void)
{
    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    leave();
    TEST_ASSERT_FALSE(alarm_engine_program_active(&S.eng));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_PROGRAM_EXIT);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(0, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    sim_advance(&S, SEC(ALARM_PROGRAM_TIMEOUT_S - 60));
    alarm_engine_program_keepalive(&S.eng, S.now);
    sim_advance(&S, SEC(ALARM_PROGRAM_TIMEOUT_S - 60));
    TEST_ASSERT_TRUE(alarm_engine_program_active(&S.eng));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &S.cfg, S.now)); /* activity too */
    sim_advance(&S, SEC(ALARM_PROGRAM_TIMEOUT_S - 1));
    TEST_ASSERT_TRUE(alarm_engine_program_active(&S.eng));
    sim_advance(&S, SEC(2));
    TEST_ASSERT_FALSE(alarm_engine_program_active(&S.eng));
    ev = sim_last_report(&S, ALARM_EV_PROGRAM_EXIT);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(1, ev->arg);
    EXPECT_EVENTS(&S, ALARM_EV_PROGRAM_EXIT, 2);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_engine_next_deadline(&S.eng));
}

/* CORE-PROG-005: power-up recovery. For ALARM_RECOVERY_WINDOW_S after start, installer mode
 * opens without a code, and a new installer code can be set without the old one. */
static void test_power_up_recovery(void)
{
    TEST_ASSERT_EQUAL_UINT32(SEC(ALARM_RECOVERY_WINDOW_S), snapshot().recovery_left_ms);
    sim_advance(&S, SEC(100));
    TEST_ASSERT_EQUAL_UINT32(SEC(ALARM_RECOVERY_WINDOW_S - 100), snapshot().recovery_left_ms);

    uint8_t user = 99;
    EXPECT_RESULT(ALARM_OK, enter("", &user));
    TEST_ASSERT_EQUAL_UINT8(0, user);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_PROGRAM_ENTER);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(1, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(0, ev->user);

    EXPECT_RESULT(ALARM_OK, alarm_engine_program_set_code(&S.eng, &S.keypad, ALARM_USER_INSTALLER,
                                                          "24680", S.now));
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEFAULT_CODE_MASTER, alarm_engine_default_codes(&S.eng));
    TEST_ASSERT_EQUAL_UINT(1, S.n_config_changed);
    leave();
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, enter(CODE_INSTALLER, NULL));
    EXPECT_RESULT(ALARM_OK, enter("24680", &user));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, user);
}

/* CORE-PROG-006: the recovery window closes, can be switched off and needs every
 * partition disarmed. */
static void test_recovery_window_limits(void)
{
    sim_advance(&S, SEC(ALARM_RECOVERY_WINDOW_S));
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, enter(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, snapshot().recovery_left_ms);

    alarm_config_t c;
    sim_config_basic(&c);
    c.opt.power_up_program = false;
    sim_start(&S, &c, NULL);
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, enter("", NULL));
    TEST_ASSERT_EQUAL_UINT32(0, snapshot().recovery_left_ms);

    alarm_runtime_state_t rt;
    memset(&rt, 0, sizeof(rt));
    rt.version = ALARM_RUNTIME_VERSION;
    rt.part[0].state = ALARM_STATE_ARMED;
    rt.part[0].level = ALARM_LEVEL_AWAY;
    sim_start(&S, NULL, &rt); /* powers up armed */
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, enter("", NULL));

    /* Turning the option off in installer mode closes a window that is still open. */
    sim_start(&S, NULL, NULL);
    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    c = S.cfg;
    c.opt.power_up_program = false;
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &c, S.now));
    TEST_ASSERT_EQUAL_UINT64(0, alarm_engine_recovery_left(&S.eng));
}

/* CORE-PROG-007: installer mode sets the installer and master codes without the old ones. */
static void test_program_set_code_rules(void)
{
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, alarm_engine_program_set_code(
                                           &S.eng, &S.keypad, ALARM_USER_MASTER, "13579", S.now));
    EXPECT_RESULT(ALARM_OK, enter(CODE_INSTALLER, NULL));
    const alarm_source_t other = {ALARM_SRC_KEYPAD, 2};
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, alarm_engine_program_set_code(
                                           &S.eng, &other, ALARM_USER_MASTER, "13579", S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_program_set_code(&S.eng, &S.keypad, 3, "13579", S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_program_set_code(
                                         &S.eng, &S.keypad, ALARM_USER_MASTER, "123", S.now));
    EXPECT_RESULT(ALARM_ERR_CODE_IN_USE, alarm_engine_program_set_code(
                                             &S.eng, &S.keypad, ALARM_USER_MASTER, CODE_USER,
                                             S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_program_set_code(&S.eng, &S.keypad, ALARM_USER_MASTER,
                                                          "13579", S.now));
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEFAULT_CODE_INSTALLER, alarm_engine_default_codes(&S.eng));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_USER_CHANGED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    leave();
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "13579", ALARM_LEVEL_STAY, 0));
}

/* CORE-PROG-008, CORE-PROG-009: platform events (factory reset, firmware updates) are numbered,
 * logged and reported like engine events. */
static void test_platform_event(void)
{
    const uint32_t before = snapshot().last_event_seq;
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event(&S.eng, ALARM_EV_FACTORY_RESET, 1,
                                                        &S.keypad, ALARM_USER_INSTALLER, S.now));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_FACTORY_RESET);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(1, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    TEST_ASSERT_EQUAL_STRING("factory_reset", alarm_event_name(ALARM_EV_FACTORY_RESET));
    TEST_ASSERT_EQUAL_STRING("program_enter", alarm_event_name(ALARM_EV_PROGRAM_ENTER));
    TEST_ASSERT_EQUAL_STRING("program_exit", alarm_event_name(ALARM_EV_PROGRAM_EXIT));
    TEST_ASSERT_EQUAL_STRING("user_updated", alarm_event_name(ALARM_EV_USER_UPDATED));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event(&S.eng, ALARM_EV_ALARM, 0, NULL, 0, S.now));

    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                        ALARM_FW_ROLLED_BACK, NULL, 0, S.now));
    ev = sim_last_report(&S, ALARM_EV_FIRMWARE_UPDATE);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 2u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(ALARM_FW_ROLLED_BACK, ev->arg);
    TEST_ASSERT_EQUAL_STRING("firmware_update", alarm_event_name(ALARM_EV_FIRMWARE_UPDATE));
    TEST_ASSERT_EQUAL_STRING("rolled_back", alarm_fw_stage_name(ALARM_FW_ROLLED_BACK));
    TEST_ASSERT_EQUAL_STRING("coprocessor_failed", alarm_fw_stage_name(ALARM_FW_CP_FAILED));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                                 14, NULL, 0, S.now));
}

/* OTA-023, OTA-027, OTA-028, OTA-031: the appended firmware_update stages keep their names (an
 * MQTT/JSON contract) and the last of them is a valid event argument; the one after it is a
 * keypad's, which the panel can't log as its own. */
static void test_firmware_update_new_stages(void)
{
    TEST_ASSERT_EQUAL_STRING("server_changed", alarm_fw_stage_name(ALARM_FW_SERVER_CHANGED));
    TEST_ASSERT_EQUAL_STRING("stopped", alarm_fw_stage_name(ALARM_FW_STOPPED));
    TEST_ASSERT_EQUAL_STRING("rolling_back", alarm_fw_stage_name(ALARM_FW_ROLLING_BACK));
    TEST_ASSERT_EQUAL_STRING("restart_put_off", alarm_fw_stage_name(ALARM_FW_RESTART_PUT_OFF));
    /* appended after the five older stages, never renumbered (they are stored in the log) */
    TEST_ASSERT_EQUAL_INT(5, ALARM_FW_SERVER_CHANGED);
    TEST_ASSERT_EQUAL_INT(8, ALARM_FW_RESTART_PUT_OFF);

    const uint32_t before = snapshot().last_event_seq;
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                        ALARM_FW_RESTART_PUT_OFF, NULL, 0, S.now));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_FIRMWARE_UPDATE);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(ALARM_FW_RESTART_PUT_OFF, ev->arg);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                              (uint8_t)(ALARM_FW_RESTART_PUT_OFF + 1u), NULL, 0,
                                              S.now));
    TEST_ASSERT_EQUAL_UINT32(before + 1u, snapshot().last_event_seq);
}

/* OTA-010 and docs/features/11 "Updates through the link" (What people see, the event log; P2
 * decisions 6 and 8): a keypad's update is logged on its master as its own firmware_update stages,
 * appended after restart_put_off (stored in the log, named in the JSON: never renumbered), from
 * {ALARM_SRC_KEYPAD, its device number}; only keypad_failed carries a why (any value but 0: one
 * this firmware doesn't know is still logged, LINK-039 reads it by its decade). */
static void test_firmware_update_keypad_stages(void)
{
    TEST_ASSERT_EQUAL_INT(9, ALARM_FW_KEYPAD_INSTALLING);
    TEST_ASSERT_EQUAL_INT(10, ALARM_FW_KEYPAD_UPDATED);
    TEST_ASSERT_EQUAL_INT(11, ALARM_FW_KEYPAD_ROLLED_BACK);
    TEST_ASSERT_EQUAL_INT(12, ALARM_FW_KEYPAD_FAILED);
    TEST_ASSERT_EQUAL_INT(13, ALARM_FW_KEYPAD_KEY_ACCEPTED);
    TEST_ASSERT_EQUAL_STRING("keypad_installing", alarm_fw_stage_name(ALARM_FW_KEYPAD_INSTALLING));
    TEST_ASSERT_EQUAL_STRING("keypad_updated", alarm_fw_stage_name(ALARM_FW_KEYPAD_UPDATED));
    TEST_ASSERT_EQUAL_STRING("keypad_rolled_back",
                             alarm_fw_stage_name(ALARM_FW_KEYPAD_ROLLED_BACK));
    TEST_ASSERT_EQUAL_STRING("keypad_failed", alarm_fw_stage_name(ALARM_FW_KEYPAD_FAILED));
    TEST_ASSERT_EQUAL_STRING("keypad_key_accepted",
                             alarm_fw_stage_name(ALARM_FW_KEYPAD_KEY_ACCEPTED));
    TEST_ASSERT_EQUAL_STRING("unknown", alarm_fw_stage_name((alarm_fw_stage_t)14));

    const alarm_source_t kp3 = {ALARM_SRC_KEYPAD, 3u};
    const uint32_t before = snapshot().last_event_seq;
    static const uint8_t PLAIN[] = {ALARM_FW_KEYPAD_INSTALLING, ALARM_FW_KEYPAD_UPDATED,
                                    ALARM_FW_KEYPAD_ROLLED_BACK, ALARM_FW_KEYPAD_KEY_ACCEPTED};
    for (size_t i = 0; i < sizeof(PLAIN); i++) {
        EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                            PLAIN[i], &kp3, 0u, S.now));
        const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_FIRMWARE_UPDATE);
        TEST_ASSERT_NOT_NULL(ev);
        TEST_ASSERT_EQUAL_UINT8(PLAIN[i], ev->arg);
        TEST_ASSERT_EQUAL_UINT8(0u, ev->arg2);
        TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, ev->source.kind);
        TEST_ASSERT_EQUAL_UINT16(3u, ev->source.id);
        TEST_ASSERT_EQUAL_UINT16(0u, ev->zone);
    }
    /* the installer who accepted a key at the keypad is the event's user */
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                        ALARM_FW_KEYPAD_KEY_ACCEPTED, &kp3,
                                                        ALARM_USER_INSTALLER, S.now));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER,
                            sim_last_report(&S, ALARM_EV_FIRMWARE_UPDATE)->user);
    /* keypad_failed with its why: STALLED 24, and 77, a later firmware's */
    const alarm_source_t kp16 = {ALARM_SRC_KEYPAD, (uint16_t)ALARM_MAX_DEVICES};
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 0u,
                                                 ALARM_FW_KEYPAD_FAILED, 24u, &kp16, 0u, S.now));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_FIRMWARE_UPDATE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_FW_KEYPAD_FAILED, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(24u, ev->arg2);
    TEST_ASSERT_EQUAL_UINT16(ALARM_MAX_DEVICES, ev->source.id);
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 0u,
                                                 ALARM_FW_KEYPAD_FAILED, 77u, &kp3, 0u, S.now));
    const uint32_t logged = before + 7u;
    TEST_ASSERT_EQUAL_UINT32(logged, snapshot().last_event_seq);

    /* refused: a failure without its why, a why on another stage, no keypad named (the system,
     * this panel's own screen, a number past the devices, another kind of source), a zone, and
     * the stage after the last */
    static const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0u};
    static const alarm_source_t local = {ALARM_SRC_KEYPAD, ALARM_KEYPAD_LOCAL};
    const alarm_source_t past = {ALARM_SRC_KEYPAD, (uint16_t)(ALARM_MAX_DEVICES + 1u)};
    const alarm_source_t remote = {ALARM_SRC_REMOTE, 3u};
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 0u,
                                                 ALARM_FW_KEYPAD_FAILED, 0u, &kp3, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 0u,
                                                 ALARM_FW_KEYPAD_UPDATED, 24u, &kp3, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 0u,
                                                 ALARM_FW_UPDATED, 24u, &kp3, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                              ALARM_FW_KEYPAD_INSTALLING, NULL, 0u, S.now));
    const alarm_source_t *const NOT_A_KEYPAD[] = {&sys, &local, &past, &remote};
    for (size_t i = 0; i < sizeof(NOT_A_KEYPAD) / sizeof(NOT_A_KEYPAD[0]); i++) {
        EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                      alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                  ALARM_FW_KEYPAD_UPDATED, NOT_A_KEYPAD[i], 0u,
                                                  S.now));
    }
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 3u,
                                                 ALARM_FW_KEYPAD_UPDATED, 0u, &kp3, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                              (uint8_t)(ALARM_FW_KEYPAD_KEY_ACCEPTED + 1u), &kp3,
                                              0u, S.now));
    TEST_ASSERT_EQUAL_UINT32(logged, snapshot().last_event_seq);

    /* the panel's own stages still take any source, a keypad's included */
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event(&S.eng, ALARM_EV_FIRMWARE_UPDATE,
                                                        ALARM_FW_INSTALLING, &kp3, 0u, S.now));
    TEST_ASSERT_EQUAL_UINT32(logged + 1u, snapshot().last_event_seq);
}

/* CORE-PROG-010, ZW-022 (docs/features/36 network.md §15): the Z-Wave network's events are
 * platform events with a zone and a second argument, numbered, logged and reported like the
 * others; each type takes only its own fields. */
static void test_zwave_platform_events(void)
{
    static const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0};
    const uint32_t before = snapshot().last_event_seq;
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_ADDED, 0u, 7u,
                                                 ALARM_ZW_JOIN_S2_AUTH, &S.keypad,
                                                 ALARM_USER_INSTALLER, S.now));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_ZWAVE_ADDED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(7u, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_JOIN_S2_AUTH, ev->arg2);
    TEST_ASSERT_EQUAL_UINT8(0u, ev->partition);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SYSTEM) != 0u);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_REPORT) != 0u);

    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_ADDED_INSECURE, 0u, 8u,
                                                 ALARM_ZW_JOIN_FAILED, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_REMOVED, 3u, 7u,
                                                 ALARM_ZW_REMOVED_FAILED, &S.keypad,
                                                 ALARM_USER_INSTALLER, S.now));
    ev = sim_last_report(&S, ALARM_EV_ZWAVE_REMOVED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT16(3u, ev->zone);
    TEST_ASSERT_EQUAL_UINT8(7u, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZW_REMOVED_FAILED, ev->arg2);
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_LEFT, 0u, 9u,
                                                 ALARM_ZW_LEFT_RESET_LOCALLY, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_NETWORK_RESET, 0u, 0u, 0u,
                                                 &S.keypad, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_RADIO_RESTARTED,
                                                           0u, 0x09u, 0u, &sys, 0u, S.now));
    ev = sim_last_report(&S, ALARM_EV_ZWAVE_RADIO_RESTARTED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 6u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(0x09u, ev->arg);
    TEST_ASSERT_EQUAL_STRING("zwave_device_added", alarm_event_name(ALARM_EV_ZWAVE_ADDED));
    TEST_ASSERT_EQUAL_STRING("zwave_device_added_insecure",
                             alarm_event_name(ALARM_EV_ZWAVE_ADDED_INSECURE));
    TEST_ASSERT_EQUAL_STRING("zwave_device_removed", alarm_event_name(ALARM_EV_ZWAVE_REMOVED));
    TEST_ASSERT_EQUAL_STRING("zwave_device_left", alarm_event_name(ALARM_EV_ZWAVE_LEFT));
    TEST_ASSERT_EQUAL_STRING("zwave_network_reset",
                             alarm_event_name(ALARM_EV_ZWAVE_NETWORK_RESET));
    TEST_ASSERT_EQUAL_STRING("zwave_radio_restarted",
                             alarm_event_name(ALARM_EV_ZWAVE_RADIO_RESTARTED));
    TEST_ASSERT_EQUAL_STRING("zwave_device_software", alarm_event_name(ALARM_EV_ZWAVE_SOFTWARE));
    TEST_ASSERT_EQUAL_STRING("zwave_garage", alarm_event_name(ALARM_EV_ZWAVE_GARAGE));
    TEST_ASSERT_EQUAL_STRING("zwave_jamming_reported", alarm_event_name(ALARM_EV_ZWAVE_JAMMING));
    TEST_ASSERT_EQUAL_STRING("backup_made", alarm_event_name(ALARM_EV_BACKUP_MADE));
    TEST_ASSERT_EQUAL_STRING("backup_restored", alarm_event_name(ALARM_EV_BACKUP_RESTORED));

    /* each type its own fields */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* secured: not "not secure" */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_ADDED, 0u, 7u,
                                                 ALARM_ZW_JOIN_NOT_SECURE, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* not encrypted: not S2 */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_ADDED_INSECURE, 0u, 7u,
                                                 ALARM_ZW_JOIN_S2_AUTH, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_ADDED, 0u, 7u,
                                                 ALARM_ZW_JOIN_COUNT, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* a zone past the last */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_REMOVED,
                                                 (uint16_t)(ALARM_MAX_ZONES + 1u), 7u, 0u, &sys,
                                                 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_LEFT, 1u, 7u, 2u, &sys,
                                                 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* ZW-033: a software update's outcome is one of three */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_SOFTWARE, 2u, 7u,
                                                 ALARM_ZW_SW_FAILED + 1u, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* ZW-035: a garage's outcome is one of five */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_GARAGE, 2u, 7u,
                                                 ALARM_ZW_GARAGE_FAILED + 1u, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* a firmware update names no zone */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_FIRMWARE_UPDATE, 1u,
                                                 ALARM_FW_UPDATED, 0u, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ALARM, 0u, 0u, 0u, &sys, 0u,
                                                 S.now));
    TEST_ASSERT_EQUAL_UINT32(before + 6u, snapshot().last_event_seq);
}

/* ZW-041, ZW-022: the radio's own restarts are logged once an hour at most; arg2 says how many
 * restarts the event stands for - any count, 0 being what records before the count hold (one).
 * Still zone 0 only. */
static void test_zwave_restart_count(void)
{
    static const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0};
    const uint32_t before = snapshot().last_event_seq;
    static const uint8_t COUNTS[] = {0u, 1u, 2u, 120u, 255u};
    for (size_t i = 0; i < sizeof COUNTS; i++) {
        EXPECT_RESULT(ALARM_OK,
                      alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_RADIO_RESTARTED, 0u,
                                                     0x03u, COUNTS[i], &sys, 0u, S.now));
        const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_ZWAVE_RADIO_RESTARTED);
        TEST_ASSERT_NOT_NULL(ev);
        TEST_ASSERT_EQUAL_UINT8(0x03u, ev->arg);
        TEST_ASSERT_EQUAL_UINT8(COUNTS[i], ev->arg2);
    }
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* the radio's, never a zone's */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_RADIO_RESTARTED, 1u,
                                                 0x03u, 2u, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZWAVE_RADIO_RESTARTED,
                                                 (uint16_t)(ALARM_MAX_ZONES + 1u), 0x03u, 0u,
                                                 &sys, 0u, S.now));
    TEST_ASSERT_EQUAL_UINT32(before + sizeof COUNTS, snapshot().last_event_seq);
}

/* RTC-014: the clock set by hand is a platform event with the user who set it. */
static void test_clock_set_event(void)
{
    static const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0};
    const alarm_event_t *ev;
    TEST_ASSERT_EQUAL_STRING("clock_set", alarm_event_name(ALARM_EV_CLOCK_SET));
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event_ex(&S.eng, ALARM_EV_CLOCK_SET, 0u, 0u, 0u,
                                                           &S.keypad, ALARM_USER_INSTALLER,
                                                           S.now));
    ev = sim_last_report(&S, ALARM_EV_CLOCK_SET);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event_ex(&S.eng, ALARM_EV_CLOCK_SET, 0u, 0u, 0u,
                                                           &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_CLOCK_SET, 0u, 1u, 0u, &sys, 0u,
                                                 S.now));
}

/* AUT-009, CORE-PROG-012 (docs/features/41 section 7): an automation that didn't lock, close or
 * arm is a platform event, numbered, logged and reported now with its source; partition 0 (the
 * call has none to give), a reason 1..7, an action below ALARM_AUTO_ACT_LIMIT, a zone in range. */
static void test_automation_failed_event(void)
{
    static const alarm_source_t aut = {ALARM_SRC_AUTOMATION, 12u};
    const alarm_event_t *ev;
    TEST_ASSERT_EQUAL_INT(63, ALARM_EV_AUTOMATION_FAILED); /* stored: pinned */
    TEST_ASSERT_EQUAL_INT(1, ALARM_AUTO_FAIL_UNREACHABLE);
    TEST_ASSERT_EQUAL_INT(7, ALARM_AUTO_FAIL_NO_WARNING);
    TEST_ASSERT_EQUAL_STRING("automation_failed", alarm_event_name(ALARM_EV_AUTOMATION_FAILED));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 3u,
                                                 (uint8_t)ALARM_AUTO_FAIL_NO_ANSWER,
                                                 (uint8_t)ALARM_AUTO_ACT_LOCK, &aut, 2u, S.now));
    ev = sim_last_report(&S, ALARM_EV_AUTOMATION_FAILED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(0u, ev->partition);
    TEST_ASSERT_EQUAL_UINT16(3u, ev->zone);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_NO_ANSWER, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_ACT_LOCK, ev->arg2);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_AUTOMATION, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(12u, ev->source.id);
    TEST_ASSERT_EQUAL_UINT8(2u, ev->user);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SYSTEM) != 0u);
    /* every device reason with Lock and with Close (zone 0 and the last zone); no_warning only
     * with arming */
    for (unsigned r = ALARM_AUTO_FAIL_UNREACHABLE; r < (unsigned)ALARM_AUTO_FAIL_NO_WARNING; r++) {
        EXPECT_RESULT(ALARM_OK,
                      alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u,
                                                     (uint8_t)r, (uint8_t)ALARM_AUTO_ACT_LOCK,
                                                     &aut, 0u, S.now));
        EXPECT_RESULT(ALARM_OK,
                      alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED,
                                                     (uint16_t)ALARM_MAX_ZONES, (uint8_t)r,
                                                     (uint8_t)ALARM_AUTO_ACT_CLOSE, &aut, 0u,
                                                     S.now));
    }
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u,
                                                 (uint8_t)ALARM_AUTO_FAIL_NO_WARNING,
                                                 (uint8_t)ALARM_AUTO_ACT_ARM, &aut, 0u, S.now));
    const uint32_t before = snapshot().last_event_seq;
    /* refused, nothing recorded: no reason, a reason past the last, an action out of range, a
     * zone out of range, and the pairs it has no words for (a device reason with arming or with
     * Turn on, no_warning with a Lock) */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u,
                                                 (uint8_t)ALARM_AUTO_FAIL_BUSY,
                                                 (uint8_t)ALARM_AUTO_ACT_ARM, &aut, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u,
                                                 (uint8_t)ALARM_AUTO_FAIL_REFUSED, 1u /* ACT_ON */,
                                                 &aut, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u,
                                                 (uint8_t)ALARM_AUTO_FAIL_NO_WARNING,
                                                 (uint8_t)ALARM_AUTO_ACT_LOCK, &aut, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u, 0u,
                                                 (uint8_t)ALARM_AUTO_ACT_LOCK, &aut, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u, 8u,
                                                 (uint8_t)ALARM_AUTO_ACT_LOCK, &aut, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, 0u,
                                                 (uint8_t)ALARM_AUTO_FAIL_BUSY,
                                                 (uint8_t)ALARM_AUTO_ACT_LIMIT, &aut, 0u, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED,
                                                 (uint16_t)(ALARM_MAX_ZONES + 1u),
                                                 (uint8_t)ALARM_AUTO_FAIL_BUSY,
                                                 (uint8_t)ALARM_AUTO_ACT_LOCK, &aut, 0u, S.now));
    TEST_ASSERT_EQUAL_UINT32(before, snapshot().last_event_seq);
}

/* ZB-019, CORE-PROG-010 (docs/features/20 devices.md §11): the Zigbee network's events are
 * platform events, numbered, logged and reported like the others; each type takes only its own
 * fields. The numbers are stored in the event log: the ones before stay where they were and the
 * new ones are pinned. */
static void test_zigbee_platform_events(void)
{
    static const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0};
    TEST_ASSERT_EQUAL_INT(53, ALARM_EV_CLOCK_SET);
    TEST_ASSERT_EQUAL_INT(54, ALARM_EV_ZIGBEE_ADDED);
    TEST_ASSERT_EQUAL_INT(55, ALARM_EV_ZIGBEE_REMOVED);
    TEST_ASSERT_EQUAL_INT(56, ALARM_EV_ZIGBEE_FORGOTTEN);
    TEST_ASSERT_EQUAL_INT(57, ALARM_EV_ZIGBEE_LEFT);
    TEST_ASSERT_EQUAL_INT(58, ALARM_EV_ZIGBEE_REPLACED);
    TEST_ASSERT_EQUAL_INT(59, ALARM_EV_ZIGBEE_JOIN_REFUSED);
    TEST_ASSERT_EQUAL_INT(60, ALARM_EV_ZIGBEE_CHANNEL);
    TEST_ASSERT_EQUAL_INT(61, ALARM_EV_ZIGBEE_NETWORK_RESET);
    TEST_ASSERT_EQUAL_INT(62, ALARM_EV_ZIGBEE_RADIO_RESTARTED);
    /* the join, refusal and restart numbers are the registry's and the radio's (zsrc_zigbee.h) */
    TEST_ASSERT_EQUAL_INT(0, ALARM_ZB_JOIN_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(1, ALARM_ZB_JOIN_INSTALL_CODE);
    TEST_ASSERT_EQUAL_INT(2, ALARM_ZB_JOIN_STANDARD_KEY);
    TEST_ASSERT_EQUAL_INT(1, ALARM_ZB_REFUSED_NO_CODE);
    TEST_ASSERT_EQUAL_INT(2, ALARM_ZB_REFUSED_REJOIN);
    TEST_ASSERT_EQUAL_INT(2, ALARM_ZB_RESTART_PAUSED);

    TEST_ASSERT_EQUAL_STRING("zigbee_added", alarm_event_name(ALARM_EV_ZIGBEE_ADDED));
    TEST_ASSERT_EQUAL_STRING("zigbee_removed", alarm_event_name(ALARM_EV_ZIGBEE_REMOVED));
    TEST_ASSERT_EQUAL_STRING("zigbee_forgotten", alarm_event_name(ALARM_EV_ZIGBEE_FORGOTTEN));
    TEST_ASSERT_EQUAL_STRING("zigbee_left", alarm_event_name(ALARM_EV_ZIGBEE_LEFT));
    TEST_ASSERT_EQUAL_STRING("zigbee_replaced", alarm_event_name(ALARM_EV_ZIGBEE_REPLACED));
    TEST_ASSERT_EQUAL_STRING("zigbee_join_refused",
                             alarm_event_name(ALARM_EV_ZIGBEE_JOIN_REFUSED));
    TEST_ASSERT_EQUAL_STRING("zigbee_channel", alarm_event_name(ALARM_EV_ZIGBEE_CHANNEL));
    TEST_ASSERT_EQUAL_STRING("zigbee_network_reset",
                             alarm_event_name(ALARM_EV_ZIGBEE_NETWORK_RESET));
    TEST_ASSERT_EQUAL_STRING("zigbee_radio_restarted",
                             alarm_event_name(ALARM_EV_ZIGBEE_RADIO_RESTARTED));

    const uint32_t before = snapshot().last_event_seq;
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_ADDED, 0u, 12u,
                                                 ALARM_ZB_JOIN_INSTALL_CODE, &S.keypad,
                                                 ALARM_USER_INSTALLER, S.now));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_ZIGBEE_ADDED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(12u, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZB_JOIN_INSTALL_CODE, ev->arg2);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, ev->user);
    TEST_ASSERT_EQUAL_UINT8(0u, ev->partition);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SYSTEM) != 0u);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_REPORT) != 0u);
    /* a device found again by its traffic may already feed a zone */
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_ADDED, 4u, 13u,
                                                 ALARM_ZB_JOIN_UNKNOWN, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_REMOVED, 3u, 12u, 0u,
                                                 &S.keypad, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_FORGOTTEN, 0u, 0u, 0u,
                                                 &S.keypad, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_LEFT, 2u, 9u,
                                                           0u, &sys, 0u, S.now));
    ev = sim_last_report(&S, ALARM_EV_ZIGBEE_LEFT);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT16(2u, ev->zone);
    TEST_ASSERT_EQUAL_UINT8(9u, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(0u, ev->user);
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_REPLACED, 3u, 14u, 0u,
                                                 &S.keypad, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_JOIN_REFUSED, 0u, 0u,
                                                 ALARM_ZB_REFUSED_REJOIN, &sys, 0u, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_CHANNEL, 0u, 0u, 20u,
                                                 &S.keypad, ALARM_USER_INSTALLER, S.now));
    ev = sim_last_report(&S, ALARM_EV_ZIGBEE_CHANNEL);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(20u, ev->arg2);
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_NETWORK_RESET, 0u, 0u,
                                                 0u, &S.keypad, ALARM_USER_INSTALLER, S.now));
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_ZIGBEE_RADIO_RESTARTED, 0u, 0u,
                                                 ALARM_ZB_RESTART_PAUSED, &sys, 0u, S.now));
    ev = sim_last_report(&S, ALARM_EV_ZIGBEE_RADIO_RESTARTED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(before + 10u, ev->seq);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZB_RESTART_PAUSED, ev->arg2);

    /* each type its own fields: refused, and nothing logged */
    static const struct {
        alarm_event_type_t type;
        uint16_t zone;
        uint8_t arg, arg2;
    } bad[] = {
        {ALARM_EV_ZIGBEE_ADDED, 0u, 12u, ALARM_ZB_JOIN_COUNT},
        {ALARM_EV_ZIGBEE_ADDED, (uint16_t)(ALARM_MAX_ZONES + 1u), 12u, 0u},
        {ALARM_EV_ZIGBEE_REMOVED, 1u, 12u, 1u},
        {ALARM_EV_ZIGBEE_FORGOTTEN, 0u, 12u, 1u},
        {ALARM_EV_ZIGBEE_LEFT, 0u, 12u, 1u},
        {ALARM_EV_ZIGBEE_REPLACED, 1u, 14u, 1u},
        {ALARM_EV_ZIGBEE_JOIN_REFUSED, 0u, 0u, ALARM_ZB_REFUSED_COUNT},
        {ALARM_EV_ZIGBEE_JOIN_REFUSED, 1u, 0u, 0u}, /* a stranger feeds no zone */
        {ALARM_EV_ZIGBEE_JOIN_REFUSED, 0u, 5u, 0u}, /* nor has a registry id */
        {ALARM_EV_ZIGBEE_CHANNEL, 0u, 0u, 10u},
        {ALARM_EV_ZIGBEE_CHANNEL, 0u, 0u, 27u},
        {ALARM_EV_ZIGBEE_CHANNEL, 1u, 0u, 15u},
        {ALARM_EV_ZIGBEE_NETWORK_RESET, 0u, 1u, 0u},
        {ALARM_EV_ZIGBEE_NETWORK_RESET, 0u, 0u, 1u},
        {ALARM_EV_ZIGBEE_RADIO_RESTARTED, 0u, 0u, ALARM_ZB_RESTART_COUNT},
        {ALARM_EV_ZIGBEE_RADIO_RESTARTED, 0u, 3u, 0u},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                      alarm_engine_platform_event_ex(&S.eng, bad[i].type, bad[i].zone,
                                                     bad[i].arg, bad[i].arg2, &sys, 0u, S.now));
    }
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, /* past the last type is no event */
                  alarm_engine_platform_event_ex(&S.eng, ALARM_EV_COUNT, 0u, 0u, 0u, &sys, 0u,
                                                 S.now));
    TEST_ASSERT_EQUAL_UINT32(before + 10u, snapshot().last_event_seq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_installer_code_enters_installer_mode);
    RUN_TEST(test_installer_mode_only_when_idle);
    RUN_TEST(test_installer_mode_blocks_arming_only);
    RUN_TEST(test_installer_mode_exit_and_timeout);
    RUN_TEST(test_program_enter_as_installer);
    RUN_TEST(test_program_enter_as_refusals);
    RUN_TEST(test_program_enter_as_conditions);
    RUN_TEST(test_power_up_recovery);
    RUN_TEST(test_recovery_window_limits);
    RUN_TEST(test_program_set_code_rules);
    RUN_TEST(test_platform_event);
    RUN_TEST(test_firmware_update_new_stages);
    RUN_TEST(test_firmware_update_keypad_stages);
    RUN_TEST(test_zwave_platform_events);
    RUN_TEST(test_zwave_restart_count);
    RUN_TEST(test_clock_set_event);
    RUN_TEST(test_automation_failed_event);
    RUN_TEST(test_zigbee_platform_events);
    return UNITY_END();
}
