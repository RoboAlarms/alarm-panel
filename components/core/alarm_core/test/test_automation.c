/*
 * test_automation.c - the automation source (docs/features/41 phase C): the guard that lets only
 * an arm and a disarm through, as the person who saved the automation; ARM_FAILED reported for
 * the unattended sources; the automation's narrowed bypass, its mark on the ARMED and each zone
 * watched again once closed; ALARM_CMD_CODE_CHECK; who armed (arm_source) and the put-off event.
 * Requirements: CORE-USR-029..036; AUT-002, AUT-011, AUT-013, AUT-014, AUT-015 (docs/features/41)
 */
#include <stddef.h>

#include "sim.h"

static sim_t S;

static const alarm_source_t AUTO_SRC = {ALARM_SRC_AUTOMATION, 7u};

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

static void cmd_init(alarm_cmd_t *cmd, alarm_cmd_type_t type, const char *code)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    cmd->partition = 1;
    cmd->level = ALARM_LEVEL_AWAY;
    cmd->zone = Z_WINDOW;
    cmd->panic = ALARM_PANIC_POLICE;
    if (code != NULL) {
        strncpy(cmd->code, code, sizeof(cmd->code) - 1u);
    }
}

static alarm_result_t auto_cmd(alarm_cmd_type_t type, const char *code, bool as, uint8_t user)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, type, code);
    return as ? alarm_engine_command_as(&S.eng, &cmd, &AUTO_SRC, user, S.now, &S.last)
              : alarm_engine_command(&S.eng, &cmd, &AUTO_SRC, S.now, &S.last);
}

/* An automation's arm, as the service sends it: run as its maker, no code. */
static alarm_result_t auto_arm(uint8_t maker, alarm_arm_level_t level, uint8_t flags)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_ARM, NULL);
    cmd.level = level;
    cmd.arm_flags = flags;
    return alarm_engine_command_as(&S.eng, &cmd, &AUTO_SRC, maker, S.now, &S.last);
}

static alarm_result_t auto_disarm(uint8_t maker)
{
    return auto_cmd(ALARM_CMD_DISARM, NULL, true, maker);
}

static alarm_result_t src_arm(const alarm_source_t *src, alarm_arm_level_t level, uint8_t flags)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_ARM, NULL);
    cmd.level = level;
    cmd.arm_flags = flags;
    return alarm_engine_command(&S.eng, &cmd, src, S.now, &S.last);
}

static alarm_result_t code_check(const char *code)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_CODE_CHECK, code);
    return sim_cmd(&S, &cmd);
}

static void expect_no_auto_lockout(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)alarm_engine_lockout_left(&S.eng, &AUTO_SRC, S.now));
    for (unsigned i = 0; i < ALARM_MAX_SOURCES; i++) {
        TEST_ASSERT_FALSE(S.eng.lockouts[i].used &&
                          S.eng.lockouts[i].src.kind == (uint8_t)ALARM_SRC_AUTOMATION);
    }
}

static void expect_source(const alarm_event_t *ev, const alarm_source_t *src, uint8_t user)
{
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(src->kind, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(src->id, ev->source.id);
    TEST_ASSERT_EQUAL_UINT8(user, ev->user);
}

/* Everything but an arm and a disarm run as a user, with a valid code, none, a wrong one and as
 * the master: refused before any code check. */
static void auto_refuse_all(void)
{
    static const alarm_cmd_type_t types[] = {
        ALARM_CMD_PANIC,        ALARM_CMD_SILENCE,      ALARM_CMD_BYPASS,
        ALARM_CMD_UNBYPASS,     ALARM_CMD_QUICK_BYPASS, ALARM_CMD_CHIME,
        ALARM_CMD_EXIT_RESTART, ALARM_CMD_WALK_TEST,    ALARM_CMD_CODE_CHECK,
    };
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(types[i], CODE_MASTER, false, 0));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(types[i], "", false, 0));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(types[i], "0000", false, 0));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(types[i], NULL, true, ALARM_USER_MASTER));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                      auto_cmd(types[i], CODE_MASTER, true, ALARM_USER_MASTER));
    }
    /* An arm or a disarm by code (alarm_engine_command), right, wrong or none. */
    static const alarm_cmd_type_t pass[] = {ALARM_CMD_ARM, ALARM_CMD_DISARM};
    for (unsigned i = 0; i < 2u; i++) {
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(pass[i], CODE_MASTER, false, 0));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(pass[i], "", false, 0));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_cmd(pass[i], "0000", false, 0));
    }
}

/* CORE-USR-029, AUT-002, AUT-011: disarmed, with a fire sounding, everything an automation
 * could try but its arm and disarm is refused - panic, silence, bypass, chime, walk test, code
 * check; an arm or disarm by code, not as a user - with no state change, no event and nothing
 * toward a lockout. A refusal plays the error sound. */
static void test_automation_refuses_all_but_arm_and_disarm(void)
{
    /* Its name is an MQTT/JSON contract (strings.c): added, never renamed. */
    TEST_ASSERT_EQUAL_STRING("automation", alarm_source_name(ALARM_SRC_AUTOMATION));
    sim_fault(&S, Z_SMOKE);
    sim_clear_records(&S);
    const alarm_partition_status_t before = sim_status(&S, 1);
    auto_refuse_all();
    /* The fire sounds: its disarm (silence) is refused too (D22), and so is an arm. */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    const alarm_partition_status_t after = sim_status(&S, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL(before.chime, after.chime);
    TEST_ASSERT_EQUAL(before.walk_test, after.walk_test);
    TEST_ASSERT_EQUAL_UINT16(before.memory, after.memory);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_TRUE(sim_count_sounds(&S, ALARM_SOUND_ERROR) > 0u);
    expect_no_auto_lockout();
    /* The keypad is untouched. */
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM_SILENCED, 1);
}

/* CORE-USR-029, AUT-011: an arm with any flag but BYPASS_FAULTED, or at no level, and a command
 * for a partition that doesn't exist, are refused without an event; so is the arming flag for a
 * maker who may not bypass (arm-only). */
static void test_automation_arm_flags_refused(void)
{
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_NO_ENTRY_DELAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_STAY,
                           ALARM_ARM_BYPASS_FAULTED | ALARM_ARM_NO_ENTRY_DELAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_NONE, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_arm(4, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_ARM, NULL);
    cmd.partition = 3; /* not enabled */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  alarm_engine_command_as(&S.eng, &cmd, &AUTO_SRC, ALARM_USER_MASTER, S.now,
                                          &S.last));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    expect_no_auto_lockout();
}

/* CORE-USR-029, AUT-011: an automation arms as the person who saved it - Away, Stay or Night,
 * exit time as for anyone, the ARMED naming the automation and that person - and only from
 * disarmed: never over another level, without an event. */
static void test_automation_arms_as_its_maker(void)
{
    static const alarm_arm_level_t levels[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY,
                                               ALARM_LEVEL_NIGHT};
    for (unsigned i = 0; i < 3u; i++) {
        sim_clear_records(&S);
        EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, levels[i], 0));
        EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
        EXPECT_LEVEL(&S, 1, levels[i]);
        TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, sim_status(&S, 1).armed_by);
        TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, S.last.user);
        const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ARMED);
        expect_source(ev, &AUTO_SRC, ALARM_USER_MASTER);
        TEST_ASSERT_EQUAL_UINT8(0u, ev->arg2); /* no flags, nothing bypassed */
        EXPECT_REPORTS(&S, ALARM_EV_ARMED, 1);

        sim_clear_records(&S);
        EXPECT_RESULT(ALARM_ERR_BAD_STATE, auto_arm(ALARM_USER_MASTER, levels[(i + 1u) % 3u], 0));
        EXPECT_RESULT(ALARM_ERR_BAD_STATE, auto_arm(ALARM_USER_MASTER, levels[i], 0));
        EXPECT_LEVEL(&S, 1, levels[i]);
        TEST_ASSERT_EQUAL_UINT(0, S.n_events);
        EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    }
    /* An arm-only user may arm (CORE-USR-001), as at the keypad. */
    EXPECT_RESULT(ALARM_OK, auto_arm(4, ALARM_LEVEL_STAY, 0));
    TEST_ASSERT_EQUAL_UINT8(4, sim_status(&S, 1).armed_by);
}

/* CORE-USR-029, AUT-011, AUT-015: the engine re-checks the maker every time - an empty slot (a
 * deleted maker) is refused, as is a user without that partition - and refuses without an event. */
static void test_automation_rechecks_its_maker(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_user(&cfg, 8, ALARM_AUTH_STANDARD, 0x02, "8888"); /* partition 2 only */
    sim_start(&S, &cfg, NULL);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_arm(9, ALARM_LEVEL_AWAY, 0)); /* empty slot */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_arm(0, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_arm(8, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(9));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(8));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(4)); /* arm-only never disarms */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(5)); /* a guest: not what it armed */
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    expect_no_auto_lockout();
}

/* CORE-USR-003, USR-001, CORE-USR-029: an automation the installer saved runs as the installer,
 * so at its time its arm (any level) and its disarm are refused like the installer's code -
 * NOT_AUTHORIZED, the error sound, no event, no lockout - while the master's still run. */
static void test_automation_saved_by_installer_never_arms_or_disarms(void)
{
    S.eng.cfg.opt.auto_less_secure = true;
    sim_clear_records(&S);
    static const alarm_arm_level_t levels[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY,
                                               ALARM_LEVEL_NIGHT};
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_arm(ALARM_USER_INSTALLER, levels[i], 0));
        EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    }
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_EQUAL_UINT(3, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    expect_no_auto_lockout();

    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_INSTALLER));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    EXPECT_RESULT(ALARM_OK, auto_disarm(ALARM_USER_MASTER)); /* the same run by the master */
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    expect_no_auto_lockout();
}

/* CORE-USR-029, AUT-011, AUT-015: an automation disarms an armed partition as its maker while
 * the installer option allows it; off, or while that partition's alarm sounds (burglary, fire),
 * NOT_AUTHORIZED with the error sound and no event; a disarmed partition is BAD_STATE, so an
 * automation never clears memory or acknowledges troubles. */
static void test_automation_disarm(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    expect_source(sim_last_event(&S, ALARM_EV_DISARMED), &AUTO_SRC, ALARM_USER_MASTER);
    EXPECT_REPORTS(&S, ALARM_EV_DISARMED, 1);

    /* The installer option off (AUT-015). */
    S.eng.cfg.opt.auto_less_secure = false;
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    /* Turned back on, it runs again; the arming isn't touched by the option. */
    S.eng.cfg.opt.auto_less_secure = true;
    EXPECT_RESULT(ALARM_OK, auto_disarm(ALARM_USER_MASTER));
    S.eng.cfg.opt.auto_less_secure = false;
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    S.eng.cfg.opt.auto_less_secure = true;

    /* A burglary alarm sounding (D22). */
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_NOT_EQUAL(ALARM_PAT_OFF, sim_status(&S, 1).bell);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    sim_restore(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER)); /* clear the memory */

    /* A fire sounding while armed. */
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_SMOKE);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_FIRE);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    sim_restore(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    /* Disarmed, with alarm memory to clear: refused, the memory kept. */
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    sim_restore(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    const uint16_t memory = sim_status(&S, 1).memory;
    TEST_ASSERT_NOT_EQUAL(0, memory);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, auto_disarm(ALARM_USER_MASTER));
    TEST_ASSERT_EQUAL_UINT16(memory, sim_status(&S, 1).memory);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    expect_no_auto_lockout();
}

/* CORE-USR-029, AUT-011, D22: after any alarm of the armed period - a silent burglary that never
 * sounds, a hold-up, a bell that timed out - an automation's disarm is refused: it would drop the
 * waiting report or send a cancel in the automation's name. After a silent one the refusal makes
 * no sound either. */
static void test_automation_never_disarms_after_an_alarm(void)
{
    /* A silent burglary, its report still inside the abort window. */
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_SHED);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    sim_advance(&S, SEC(30));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_SHED, ALARM_CAUSE_SILENT_BURGLARY));
    /* Reported now, still refused for the rest of the period. */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    sim_restore(&S, Z_SHED);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER)); /* clear the memory */

    /* A loud alarm whose bell has timed out: no longer sounding, still refused. */
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, SEC(60) * 30u);
    TEST_ASSERT_EQUAL_UINT(ALARM_PAT_OFF, sim_status(&S, 1).bell);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    /* The next armed period starts clean: an automation disarms it again. */
    sim_arm_away_and_leave(&S, CODE_USER);
    EXPECT_RESULT(ALARM_OK, auto_disarm(ALARM_USER_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    expect_no_auto_lockout();
}

/* CORE-USR-029, AUT-015 (decided 2026-10-05): "Bypass open zones" is under the installer
 * option too - with it off, an automation's BYPASS_FAULTED is refused, never honoured. */
static void test_automation_bypass_needs_the_option(void)
{
    S.eng.cfg.opt.auto_less_secure = false;
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    /* Without the flag it arms as usual. */
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    S.eng.cfg.opt.auto_less_secure = true;
    expect_no_auto_lockout();
}

/* CORE-USR-029, CORE-USR-030, AUT-011: installer mode, a walk test and a held arming refuse an
 * automation's arm as anyone's - an ARM_FAILED, reported since nobody stands at the panel. */
static void test_automation_arm_refused_as_for_anyone(void)
{
    alarm_cmd_t walk;
    cmd_init(&walk, ALARM_CMD_WALK_TEST, CODE_MASTER);
    walk.walk_op = (uint8_t)ALARM_WALK_START;
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &walk));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 1);
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    /* Its disarm can't end the walk test either. */
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, auto_disarm(ALARM_USER_MASTER));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    walk.walk_op = (uint8_t)ALARM_WALK_END;
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &walk));

    alarm_engine_hold_arming(&S.eng, true);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_HELD, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 1);
    alarm_engine_hold_arming(&S.eng, false);

    EXPECT_RESULT(ALARM_OK, alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now,
                                                       NULL));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-USR-030, AUT-009, AUT-014: an automation's or a schedule's failed arming is reported,
 * the not-ready zone in the event; a keypad's is logged only, as before. */
static void test_arm_failed_reported_for_unattended_sources(void)
{
    static const alarm_source_t SCHED = {ALARM_SRC_SCHEDULE, 2u};
    sim_fault(&S, Z_WINDOW);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, S.last.not_ready_zone);
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 1);
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_ARM_FAILED);
    expect_source(ev, &AUTO_SRC, ALARM_USER_MASTER);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, ev->zone);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_NOT_READY, ev->arg);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ERROR));

    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, src_arm(&SCHED, ALARM_LEVEL_AWAY, 0));
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 1);
    expect_source(sim_last_report(&S, ALARM_EV_ARM_FAILED), &SCHED, 0u);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, sim_last_report(&S, ALARM_EV_ARM_FAILED)->zone);

    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 1);
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 0);
    static const alarm_source_t SYS = {ALARM_SRC_SYSTEM, 0u};
    static const alarm_source_t REMOTE = {ALARM_SRC_REMOTE, 1u};
    EXPECT_RESULT(ALARM_ERR_NOT_READY, src_arm(&SYS, ALARM_LEVEL_AWAY, 0));
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_ARM, CODE_MASTER);
    EXPECT_RESULT(ALARM_ERR_NOT_READY,
                  alarm_engine_command(&S.eng, &cmd, &REMOTE, S.now, &S.last));
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 3);
    EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 0);
}

/* CORE-USR-031, CORE-USR-032, AUT-014: with "Bypass open zones" an automation bypasses the open
 * burglary zones - each BYPASS its own, reported - and arms; the ARMED counts them. */
static void test_automation_bypasses_open_zones(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_FAMILY);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_BYPASSED);
    EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 2);
    EXPECT_REPORTS(&S, ALARM_EV_BYPASS, 2);
    expect_source(sim_last_event(&S, ALARM_EV_BYPASS), &AUTO_SRC, ALARM_USER_MASTER);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ARMED);
    expect_source(ev, &AUTO_SRC, ALARM_USER_MASTER);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(2u, ALARM_ARMED_BYPASSED(ev->arg2));
    TEST_ASSERT_EQUAL_UINT8(0u, ev->arg2 & 0x07u); /* the mode flags are untouched */
    /* A level that doesn't need the open interior zone bypasses only the window. */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_STAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 1);
    EXPECT_NO_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_BYPASSED);
    TEST_ASSERT_EQUAL_UINT8(1u, ALARM_ARMED_BYPASSED(sim_last_event(&S, ALARM_EV_ARMED)->arg2));
}

/* CORE-USR-032: the count saturates at ALARM_ARMED_BYPASSED_MAX and never reaches the flags. */
static void test_armed_bypass_count_saturates(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    for (uint16_t z = 21u; z <= 60u; z++) {
        sim_set_zone(&cfg, z, ALARM_ZT_PERIMETER, 1);
    }
    sim_start(&S, &cfg, NULL);
    for (uint16_t z = 21u; z <= 60u; z++) {
        sim_fault(&S, z);
    }
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_STAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 40);
    const uint8_t arg2 = sim_last_event(&S, ALARM_EV_ARMED)->arg2;
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARMED_BYPASSED_MAX, ALARM_ARMED_BYPASSED(arg2));
    TEST_ASSERT_EQUAL_UINT8(0u, arg2 & 0x07u);
}

/* CORE-USR-031, AUT-014: a zone in tamper, loop trouble or missing-sensor trouble is never
 * bypassed by an automation: the arming fails NOT_READY naming it, reported, and no zone is
 * bypassed - the open window neither. */
static void test_automation_never_bypasses_troubled_zone(void)
{
    static const alarm_zone_event_t bad[] = {ALARM_ZEV_TAMPER, ALARM_ZEV_TROUBLE,
                                             ALARM_ZEV_UNREACHABLE};
    static const alarm_zone_event_t good[] = {ALARM_ZEV_TAMPER_RESTORE,
                                              ALARM_ZEV_TROUBLE_RESTORE, ALARM_ZEV_REACHABLE};
    for (unsigned i = 0; i < 3u; i++) {
        sim_fault(&S, Z_WINDOW);
        sim_zev(&S, Z_FAMILY, bad[i]);
        sim_clear_records(&S);
        EXPECT_RESULT(ALARM_ERR_NOT_READY,
                      auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
        TEST_ASSERT_EQUAL_UINT16(Z_FAMILY, S.last.not_ready_zone);
        EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
        EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 0);
        EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
        EXPECT_NO_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_BYPASSED);
        EXPECT_REPORTS(&S, ALARM_EV_ARM_FAILED, 1);
        TEST_ASSERT_EQUAL_UINT16(Z_FAMILY, sim_last_report(&S, ALARM_EV_ARM_FAILED)->zone);

        /* A zone both open and tampered with is not "just open" either. */
        sim_zev(&S, Z_FAMILY, good[i]);
        sim_zev(&S, Z_WINDOW, bad[i]);
        sim_clear_records(&S);
        EXPECT_RESULT(ALARM_ERR_NOT_READY,
                      auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
        TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, S.last.not_ready_zone);
        EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 0);
        sim_zev(&S, Z_WINDOW, good[i]);
        sim_restore(&S, Z_WINDOW);
        EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER)); /* ack the troubles */
    }
}

/* CORE-USR-031: another source's BYPASS_FAULTED is unchanged - a keypad's bypasses a tampered
 * zone with the open one, never watches either again, and its ARMED carries no count. */
static void test_other_sources_bypass_unchanged(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_TAMPER);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_BYPASSED);
    EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 2);
    TEST_ASSERT_EQUAL_UINT8(0u, sim_last_event(&S, ALARM_EV_ARMED)->arg2);
    sim_restore(&S, Z_WINDOW);
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_TAMPER_RESTORE);
    sim_advance(&S, SEC(120));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_BYPASSED);
    EXPECT_EVENTS(&S, ALARM_EV_UNBYPASS, 0);

    /* A schedule's as well. */
    static const alarm_source_t SCHED = {ALARM_SRC_SCHEDULE, 2u};
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    sim_fault(&S, Z_WINDOW);
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_TROUBLE);
    EXPECT_RESULT(ALARM_OK, src_arm(&SCHED, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_BYPASSED);
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, SEC(120));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
}

/* CORE-USR-031, AUT-014: fire, CO and 24-hour zones never block an arming - in trouble, tampered
 * or open - and are never bypassed, for an automation (with or without its flag) and for anyone. */
static void test_life_safety_zones_never_block_arming(void)
{
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_TROUBLE);
    sim_zev(&S, Z_CO, ALARM_ZEV_TROUBLE);
    sim_zev(&S, Z_SMOKE_VER, ALARM_ZEV_TAMPER);
    sim_zev(&S, Z_PANIC, ALARM_ZEV_TAMPER);
    sim_zev(&S, Z_HOLDUP, ALARM_ZEV_TROUBLE);
    sim_zev(&S, Z_WATER, ALARM_ZEV_UNREACHABLE);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_NIGHT, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_EVENTS(&S, ALARM_EV_BYPASS, 0);
    TEST_ASSERT_EQUAL_UINT8(0u, sim_last_event(&S, ALARM_EV_ARMED)->arg2);
    static const uint16_t life[] = {Z_SMOKE, Z_SMOKE_VER, Z_CO, Z_PANIC, Z_HOLDUP, Z_WATER};
    for (unsigned i = 0; i < sizeof(life) / sizeof(life[0]); i++) {
        EXPECT_NO_ZONE_FLAG(&S, life[i], ALARM_ZS_BYPASSED);
    }
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
}

/* CORE-USR-033, AUT-014: a zone the automation bypassed is watched again once it has been
 * closed for 10 s while armed - exit time included - with an UNBYPASS by the automation as its
 * maker, reported as a bypass; opening inside the 10 s starts the wait over. Watched again, the
 * zone alarms as any other. */
static void test_automation_bypass_watched_again(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_GARAGE);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    EXPECT_ZONE_FLAG(&S, Z_GARAGE, ALARM_ZS_BYPASSED);
    sim_advance(&S, SEC(5));
    sim_restore(&S, Z_GARAGE); /* during exit time */
    sim_clear_records(&S);
    sim_advance(&S, SEC(10) - 1u);
    EXPECT_ZONE_FLAG(&S, Z_GARAGE, ALARM_ZS_BYPASSED);
    sim_advance(&S, 1u);
    EXPECT_NO_ZONE_FLAG(&S, Z_GARAGE, ALARM_ZS_BYPASSED);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_EVENTS(&S, ALARM_EV_UNBYPASS, 1);
    EXPECT_REPORTS(&S, ALARM_EV_UNBYPASS, 1);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_UNBYPASS);
    expect_source(ev, &AUTO_SRC, ALARM_USER_MASTER);
    TEST_ASSERT_EQUAL_UINT16(Z_GARAGE, ev->zone);
    TEST_ASSERT_EQUAL_UINT8(1, ev->partition);

    sim_advance(&S, SEC(60));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    /* Closed, opened again after 9 s, closed: the wait starts over. A trouble does the same. */
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, SEC(9));
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_FALSE(sim_status(&S, 1).triggered); /* still bypassed: no alarm */
    sim_advance(&S, SEC(30));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, SEC(5));
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE);
    sim_advance(&S, SEC(20));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE_RESTORE);
    sim_clear_records(&S);
    sim_advance(&S, SEC(9));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    sim_advance(&S, SEC(1));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_EVENTS(&S, ALARM_EV_UNBYPASS, 1);
    TEST_ASSERT_TRUE(S.persisted.bypassed[(Z_WINDOW - 1) / 8] == 0u); /* stored unbypassed */

    sim_fault(&S, Z_WINDOW); /* protected again: the perimeter alarms */
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
}

/* CORE-USR-033: the disarm ends every bypass, the automation's ones included, without an UNBYPASS;
 * a person's bypass is never watched again; a restart forgets the mark (runtime only, no stored
 * format change), leaving the zone bypassed until the disarm as any bypass. */
static void test_rewatch_ends_at_disarm_and_restart(void)
{
    sim_fault(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_EVENTS(&S, ALARM_EV_UNBYPASS, 0);
    /* Bypassed by a person, then armed by the automation: never watched again. */
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_MASTER, Z_WINDOW));
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    TEST_ASSERT_EQUAL_UINT8(0u, sim_last_event(&S, ALARM_EV_ARMED)->arg2);
    sim_restore(&S, Z_WINDOW);
    sim_advance(&S, SEC(120));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    /* A restart while the automation's bypass waits. */
    sim_fault(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, ALARM_ARM_BYPASS_FAULTED));
    sim_advance(&S, SEC(70));
    const alarm_runtime_state_t saved = S.persisted;
    const alarm_config_t cfg = S.cfg;
    TEST_ASSERT_TRUE(saved.bypassed[(Z_WINDOW - 1) / 8] != 0u);
    sim_start(&S, &cfg, &saved);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    sim_restore(&S, Z_WINDOW);
    sim_clear_records(&S);
    sim_advance(&S, SEC(120));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_EVENTS(&S, ALARM_EV_UNBYPASS, 0);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
}

/* CORE-USR-034, AUT-013: a code check answers the code's user and whether that user may disarm
 * the partition, changing nothing and logging nothing. */
static void test_code_check_answers_user(void)
{
    sim_clear_records(&S);
    const alarm_partition_status_t before = sim_status(&S, 1);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_MASTER));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, S.last.user);
    TEST_ASSERT_TRUE(S.last.may_disarm);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_USER));
    TEST_ASSERT_EQUAL_UINT8(3, S.last.user);
    TEST_ASSERT_TRUE(S.last.may_disarm);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_PMASTER));
    TEST_ASSERT_TRUE(S.last.may_disarm);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_ARMONLY));
    TEST_ASSERT_EQUAL_UINT8(4, S.last.user);
    TEST_ASSERT_FALSE(S.last.may_disarm);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_GUEST));
    TEST_ASSERT_EQUAL_UINT8(5, S.last.user);
    TEST_ASSERT_FALSE(S.last.may_disarm); /* only what the guest armed */
    EXPECT_RESULT(ALARM_OK, code_check(CODE_INSTALLER));
    TEST_ASSERT_FALSE(S.last.may_disarm); /* never the installer (CORE-USR-003) */
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED, code_check(""));
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    TEST_ASSERT_EQUAL_UINT(0, S.n_persist);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_sounds(&S, ALARM_SOUND_ACK));
    const alarm_partition_status_t after = sim_status(&S, 1);
    TEST_ASSERT_EQUAL(before.state, after.state);
    TEST_ASSERT_EQUAL(before.chime, after.chime);

    /* Armed by the guest, the guest may disarm it; armed: still nothing changes. */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_GUEST, ALARM_LEVEL_STAY, 0));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_GUEST));
    TEST_ASSERT_TRUE(S.last.may_disarm);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_INSTALLER));
    TEST_ASSERT_FALSE(S.last.may_disarm);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
    /* Other commands leave may_disarm false. */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    TEST_ASSERT_FALSE(S.last.may_disarm);
}

/* CORE-USR-034, AUT-013: a wrong code counts toward the lockout as at a disarm, and a locked-out
 * keypad refuses even a right code. */
static void test_code_check_counts_toward_lockout(void)
{
    sim_clear_records(&S);
    for (unsigned i = 0; i < 4u; i++) {
        EXPECT_RESULT(ALARM_ERR_INVALID_CODE, code_check("0000"));
    }
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 4);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, code_check("0000"));
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 1);
    EXPECT_RESULT(ALARM_ERR_LOCKED_OUT, code_check(CODE_MASTER));
    TEST_ASSERT_EQUAL_UINT8(0, S.last.user);
    TEST_ASSERT_FALSE(S.last.may_disarm);
    EXPECT_RESULT(ALARM_ERR_LOCKED_OUT, sim_disarm(&S, 1, CODE_MASTER));
}

/* CORE-USR-034, AUT-013: a duress code passes and emits the silent, reported DURESS a disarm by
 * it would - nothing else, so the put-off looks the same. */
static void test_code_check_duress(void)
{
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, code_check(CODE_DURESS));
    TEST_ASSERT_EQUAL_UINT8(6, S.last.user);
    TEST_ASSERT_TRUE(S.last.may_disarm);
    TEST_ASSERT_EQUAL_UINT(1, S.n_events);
    EXPECT_REPORTS(&S, ALARM_EV_DURESS, 1);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_DURESS);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);
    TEST_ASSERT_EQUAL_UINT8(1, ev->partition);
    TEST_ASSERT_EQUAL_UINT8(6, ev->user);
    expect_source(ev, &S.keypad, 6);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_sounds(&S, ALARM_SOUND_ERROR));
}

/* CORE-USR-035, D30, AUT-009: the snapshot says an automation armed it ({AUTOMATION, uid}),
 * beside armed_by = its maker; a person's arming keeps its own source; disarmed, none. */
static void test_arm_source_in_snapshot(void)
{
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_NIGHT, 0));
    alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_AUTOMATION, st.arm_source.kind);
    TEST_ASSERT_EQUAL_UINT16(AUTO_SRC.id, st.arm_source.id);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, st.armed_by);
    sim_advance(&S, SEC(120)); /* exit time over: still the automation's */
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_AUTOMATION, sim_status(&S, 1).arm_source.kind);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_SYSTEM, st.arm_source.kind);
    TEST_ASSERT_EQUAL_UINT16(0u, st.arm_source.id);

    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, st.arm_source.kind);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, st.armed_by);
}

/* CORE-USR-035, D30: after a restart the arm source's kind is kept (the uid isn't), in the stored
 * runtime state's former padding byte; a state written before the byte (0) or a value no kind
 * has reads as not known ({SYSTEM, 0}) and the armed state itself restores as before. */
static void test_arm_source_survives_restart(void)
{
    EXPECT_RESULT(ALARM_OK, auto_arm(ALARM_USER_MASTER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(120));
    alarm_runtime_state_t saved = S.persisted;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ALARM_SRC_AUTOMATION + 1u, saved.part[0].arm_src);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, saved.part[0].armed_by);
    const alarm_config_t cfg = S.cfg;
    sim_start(&S, &cfg, &saved);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_AUTOMATION, st.arm_source.kind);
    TEST_ASSERT_EQUAL_UINT16(0u, st.arm_source.id); /* the uid isn't stored */
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, st.armed_by);

    /* An older firmware's state: the byte was padding, zeroed. */
    saved.part[0].arm_src = 0u;
    sim_start(&S, &cfg, &saved);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_SYSTEM, st.arm_source.kind);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, st.armed_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, st.level);
    /* Junk where the padding was: not known, nothing else changes. */
    saved.part[0].arm_src = 200u;
    sim_start(&S, &cfg, &saved);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_SYSTEM, sim_status(&S, 1).arm_source.kind);
    /* A disarmed partition stores no source. */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    TEST_ASSERT_EQUAL_UINT8(0u, S.persisted.part[0].arm_src);
}

/* CORE-USR-035: the stored runtime state keeps the layout ALARM_RUNTIME_VERSION 1 had - the
 * same size and the same offsets - so an older blob restores and an older reader reads ours. */
static void test_runtime_state_layout_unchanged(void)
{
    typedef struct {
        uint8_t state, level, flags, armed_by;
        uint16_t memory;
        bool chime;
    } v1_part_t; /* the entry as ALARM_RUNTIME_VERSION 1 first shipped it */
    TEST_ASSERT_EQUAL_UINT(sizeof(v1_part_t), sizeof(alarm_partition_runtime_t));
    TEST_ASSERT_EQUAL_UINT(offsetof(v1_part_t, memory), offsetof(alarm_partition_runtime_t, memory));
    TEST_ASSERT_EQUAL_UINT(offsetof(v1_part_t, chime), offsetof(alarm_partition_runtime_t, chime));
    TEST_ASSERT_EQUAL_UINT(72u, sizeof(alarm_runtime_state_t));
    TEST_ASSERT_EQUAL_INT(1, ALARM_RUNTIME_VERSION);
    /* An old blob, byte for byte, armed Stay by user 02: restores, source not known. */
    alarm_runtime_state_t old;
    memset(&old, 0, sizeof(old));
    v1_part_t p1;
    memset(&p1, 0, sizeof(p1)); /* the padding too: the old firmware's spare byte was zero */
    p1.state = ALARM_STATE_ARMED;
    p1.level = ALARM_LEVEL_STAY;
    p1.armed_by = ALARM_USER_MASTER;
    old.version = 1u;
    memcpy(&old.part[0], &p1, sizeof(p1));
    const alarm_config_t cfg = S.cfg;
    sim_start(&S, &cfg, &old);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, sim_status(&S, 1).armed_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_SYSTEM, sim_status(&S, 1).arm_source.kind);
}

/* CORE-USR-036, AUT-013, D26, D30: a put-off is ALARM_EV_ARMING_PUT_OFF - whose code, the
 * automation's source, the partition, the choice and the minutes to the new arm time - reported
 * at once, never silent, changing nothing else. */
static void test_arming_put_off_event(void)
{
    sim_clear_records(&S);
    const int64_t now_epoch = (int64_t)(1790000000000ull + S.now);
    const int64_t until = (now_epoch / 60000 + 70) * 60000; /* a whole minute, 69-70 min on */
    EXPECT_RESULT(ALARM_OK, alarm_engine_arming_put_off(&S.eng, 1, AUTO_SRC.id, 4,
                                                        ALARM_PUT_OFF_HOUR, until, S.now));
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ARMING_PUT_OFF);
    expect_source(ev, &AUTO_SRC, 4);
    TEST_ASSERT_EQUAL_UINT8(1, ev->partition);
    TEST_ASSERT_EQUAL_UINT16(0u, ev->zone);
    TEST_ASSERT_EQUAL_UINT8(ALARM_PUT_OFF_HOUR, ev->arg);
    TEST_ASSERT_TRUE(ev->arg2 == 69u || ev->arg2 == 70u);
    TEST_ASSERT_TRUE(alarm_put_off_until(ev) == until);
    TEST_ASSERT_EQUAL_UINT8(0u, ev->flags & ALARM_EVF_SILENT);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_REPORT) != 0u);
    EXPECT_REPORTS(&S, ALARM_EV_ARMING_PUT_OFF, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_STRING("arming_put_off", alarm_event_name(ALARM_EV_ARMING_PUT_OFF));
    TEST_ASSERT_EQUAL_STRING("hour", alarm_put_off_name(ALARM_PUT_OFF_HOUR));
    TEST_ASSERT_EQUAL_STRING("night", alarm_put_off_name(ALARM_PUT_OFF_NIGHT));
    TEST_ASSERT_EQUAL_STRING("day", alarm_put_off_name(ALARM_PUT_OFF_DAY));
    TEST_ASSERT_EQUAL_STRING("unknown", alarm_put_off_name((alarm_put_off_t)0));

    /* Not tonight / not today: no time is carried. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_arming_put_off(&S.eng, 1, AUTO_SRC.id, 4,
                                                        ALARM_PUT_OFF_NIGHT, until, S.now));
    TEST_ASSERT_EQUAL_UINT8(0u, sim_last_event(&S, ALARM_EV_ARMING_PUT_OFF)->arg2);
    EXPECT_RESULT(ALARM_OK, alarm_engine_arming_put_off(&S.eng, 1, AUTO_SRC.id, 4,
                                                        ALARM_PUT_OFF_DAY, 0, S.now));
    TEST_ASSERT_EQUAL_UINT8(ALARM_PUT_OFF_DAY, sim_last_event(&S, ALARM_EV_ARMING_PUT_OFF)->arg);
    /* An hour without a known time: arg2 0, until 0. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_arming_put_off(&S.eng, 1, AUTO_SRC.id, 4,
                                                        ALARM_PUT_OFF_HOUR, 0, S.now));
    ev = sim_last_event(&S, ALARM_EV_ARMING_PUT_OFF);
    TEST_ASSERT_EQUAL_UINT8(0u, ev->arg2);
    TEST_ASSERT_TRUE(alarm_put_off_until(ev) == 0);
    EXPECT_EVENTS(&S, ALARM_EV_ARMING_PUT_OFF, 4);
}

/* CORE-USR-036: a put-off with a field out of range is refused and logs nothing. */
static void test_arming_put_off_refused(void)
{
    sim_clear_records(&S);
    const int64_t until = 0;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_arming_put_off(&S.eng, 1, 0u, 4,
                                                                 ALARM_PUT_OFF_HOUR, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_arming_put_off(&S.eng, 1, 7u, 0,
                                                                 ALARM_PUT_OFF_HOUR, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_arming_put_off(&S.eng, 1, 7u, ALARM_MAX_USERS + 1u,
                                              ALARM_PUT_OFF_HOUR, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_arming_put_off(&S.eng, 1, 7u, 4,
                                                                 (alarm_put_off_t)0, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_arming_put_off(&S.eng, 1, 7u, 4, ALARM_PUT_OFF_COUNT, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_arming_put_off(&S.eng, 0, 7u, 4,
                                                                 ALARM_PUT_OFF_HOUR, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_arming_put_off(&S.eng, 2, 7u, 4,
                                                                 ALARM_PUT_OFF_HOUR, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_arming_put_off(&S.eng, ALARM_MAX_PARTITIONS + 1u, 7u, 4,
                                              ALARM_PUT_OFF_HOUR, until, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_arming_put_off(NULL, 1, 7u, 4,
                                                                 ALARM_PUT_OFF_HOUR, until, S.now));
    TEST_ASSERT_EQUAL_UINT(0, S.n_events);
}

/* CORE-USR-036: the minutes to the new arm time round, saturate and are 0 without both times;
 * the time read back is the whole minute it stood for. */
static void test_put_off_minutes(void)
{
    const int64_t t0 = 1790000012345;
    TEST_ASSERT_EQUAL_UINT8(0u, alarm_put_off_minutes(0, t0));
    TEST_ASSERT_EQUAL_UINT8(0u, alarm_put_off_minutes(t0, 0));
    TEST_ASSERT_EQUAL_UINT8(0u, alarm_put_off_minutes(t0, t0));
    TEST_ASSERT_EQUAL_UINT8(0u, alarm_put_off_minutes(t0, t0 - 60000));
    TEST_ASSERT_EQUAL_UINT8(1u, alarm_put_off_minutes(t0, t0 + 1000));
    TEST_ASSERT_EQUAL_UINT8(60u, alarm_put_off_minutes(t0, t0 + 60 * 60000 + 29000));
    TEST_ASSERT_EQUAL_UINT8(61u, alarm_put_off_minutes(t0, t0 + 60 * 60000 + 30000));
    TEST_ASSERT_EQUAL_UINT8(255u, alarm_put_off_minutes(t0, t0 + 600 * 60000));
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ALARM_EV_ARMING_PUT_OFF;
    /* every offset within a minute of seconds before a whole-minute arm time reads it back */
    const int64_t arm = (t0 / 60000 + 75) * 60000;
    for (int64_t back = 60 * 60000; back < 61 * 60000; back += 997) {
        ev.epoch_ms = arm - back;
        ev.arg2 = alarm_put_off_minutes(ev.epoch_ms, arm);
        TEST_ASSERT_TRUE(alarm_put_off_until(&ev) == arm);
    }
    ev.epoch_ms = 0;
    TEST_ASSERT_TRUE(alarm_put_off_until(&ev) == 0);
    ev.epoch_ms = t0;
    ev.arg2 = 0u;
    TEST_ASSERT_TRUE(alarm_put_off_until(&ev) == 0);
    ev.arg2 = 5u;
    ev.type = ALARM_EV_ARMED;
    TEST_ASSERT_TRUE(alarm_put_off_until(&ev) == 0);
    TEST_ASSERT_TRUE(alarm_put_off_until(NULL) == 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_automation_refuses_all_but_arm_and_disarm);
    RUN_TEST(test_automation_arm_flags_refused);
    RUN_TEST(test_automation_arms_as_its_maker);
    RUN_TEST(test_automation_rechecks_its_maker);
    RUN_TEST(test_automation_saved_by_installer_never_arms_or_disarms);
    RUN_TEST(test_automation_disarm);
    RUN_TEST(test_automation_never_disarms_after_an_alarm);
    RUN_TEST(test_automation_bypass_needs_the_option);
    RUN_TEST(test_automation_arm_refused_as_for_anyone);
    RUN_TEST(test_arm_failed_reported_for_unattended_sources);
    RUN_TEST(test_automation_bypasses_open_zones);
    RUN_TEST(test_armed_bypass_count_saturates);
    RUN_TEST(test_automation_never_bypasses_troubled_zone);
    RUN_TEST(test_other_sources_bypass_unchanged);
    RUN_TEST(test_life_safety_zones_never_block_arming);
    RUN_TEST(test_automation_bypass_watched_again);
    RUN_TEST(test_rewatch_ends_at_disarm_and_restart);
    RUN_TEST(test_code_check_answers_user);
    RUN_TEST(test_code_check_counts_toward_lockout);
    RUN_TEST(test_code_check_duress);
    RUN_TEST(test_arm_source_in_snapshot);
    RUN_TEST(test_arm_source_survives_restart);
    RUN_TEST(test_runtime_state_layout_unchanged);
    RUN_TEST(test_arming_put_off_event);
    RUN_TEST(test_arming_put_off_refused);
    RUN_TEST(test_put_off_minutes);
    return UNITY_END();
}
