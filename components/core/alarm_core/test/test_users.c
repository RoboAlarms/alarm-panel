/*
 * test_users.c - authority levels, duress, lockout, user code programming,
 * keyfob / keyswitch zones.
 * Requirements: CORE-USR-* (the automation source: test_automation.c)
 */
#include <stdlib.h>

#include "alarm/alarm_snapshot.h"
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

static alarm_result_t user_set(const char *actor, uint8_t user_no, const char *code,
                               alarm_authority_t auth, uint8_t parts)
{
    return alarm_engine_user_set(&S.eng, &S.keypad, actor, user_no, code, auth, parts, "New",
                                 S.now);
}

/* CORE-USR-001 */
static void test_arm_only_cannot_disarm(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_ARMONLY, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_disarm(&S, 1, CODE_ARMONLY));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
}

/* CORE-USR-002 */
static void test_guest_disarms_only_what_guest_armed(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_disarm(&S, 1, CODE_GUEST));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_GUEST, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_GUEST));
}

/* CORE-USR-004: duress disarms normally and sends a silent duress report. */
static void test_duress(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_DURESS));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_REPORTS(&S, ALARM_EV_DISARMED, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_DURESS);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(6, ev->user);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_FALSE(st.triggered);
    TEST_ASSERT_EQUAL_UINT16(0, st.memory);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
}

/* CORE-USR-026: arming with a duress code arms normally and sends the same silent report. */
static void test_duress_on_arming(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_DURESS, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_REPORTS(&S, ALARM_EV_ARMED, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_DURESS);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(6, ev->user);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);
    TEST_ASSERT_FALSE(sim_status(&S, 1).triggered);

    /* A normal code arms without one. */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_REPORTS(&S, ALARM_EV_DURESS, 0);
}

/* CORE-USR-005: lockout per keypad after repeated invalid codes; panic still works. */
static void test_lockout(void)
{
    for (int i = 0; i < 5; i++) {
        EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_disarm(&S, 1, "0000"));
    }
    EXPECT_REPORTS(&S, ALARM_EV_LOCKOUT, 1);
    EXPECT_RESULT(ALARM_ERR_LOCKED_OUT, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));

    S.keypad.id = 2; /* another keypad is not locked */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));

    S.keypad.id = 1;
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE));
    sim_advance(&S, SEC(300));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
}

/* KPD-008: the time left of a lockout counts down to 0 and the snapshot carries the end for
 * the touchscreen only. */
static void test_lockout_left(void)
{
    const alarm_source_t local = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    S.keypad = local;
    TEST_ASSERT_EQUAL_UINT64(0, alarm_engine_lockout_left(&S.eng, &local, S.now));
    for (int i = 0; i < 5; i++) {
        EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_disarm(&S, 1, "0000"));
    }
    const alarm_ms_t left = alarm_engine_lockout_left(&S.eng, &local, S.now);
    TEST_ASSERT_TRUE(left > SEC(299) && left <= SEC(300));
    alarm_snapshot_t *snap = (alarm_snapshot_t *)calloc(1, sizeof(*snap));
    TEST_ASSERT_NOT_NULL(snap);
    alarm_engine_snapshot(&S.eng, snap);
    TEST_ASSERT_EQUAL_UINT64(S.now + left, snap->local_lockout_until);

    sim_advance(&S, SEC(100));
    TEST_ASSERT_EQUAL_UINT64(left - SEC(100), alarm_engine_lockout_left(&S.eng, &local, S.now));
    sim_advance(&S, SEC(200));
    TEST_ASSERT_EQUAL_UINT64(0, alarm_engine_lockout_left(&S.eng, &local, S.now));
    alarm_engine_snapshot(&S.eng, snap);
    TEST_ASSERT_EQUAL_UINT64(0, snap->local_lockout_until);

    /* Another source's lockout does not show on the touchscreen. */
    S.keypad.id = 2;
    for (int i = 0; i < 5; i++) {
        EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_disarm(&S, 1, "0000"));
    }
    TEST_ASSERT_TRUE(alarm_engine_lockout_left(&S.eng, &S.keypad, S.now) > 0u);
    alarm_engine_snapshot(&S.eng, snap);
    TEST_ASSERT_EQUAL_UINT64(0, snap->local_lockout_until);
    free(snap);
}

/* CORE-USR-006 */
static void test_valid_code_resets_failures(void)
{
    for (int i = 0; i < 4; i++) {
        (void)sim_disarm(&S, 1, "0000");
    }
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    for (int i = 0; i < 4; i++) {
        (void)sim_disarm(&S, 1, "0000");
    }
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 0);
}

/* CORE-USR-007 */
static void test_master_adds_user(void)
{
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    TEST_ASSERT_EQUAL_UINT(1, S.n_config_changed);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_USER_CHANGED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(8, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(2, ev->user);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "4444", ALARM_LEVEL_STAY, 0));
}

/* CORE-USR-008 */
static void test_code_must_be_unique(void)
{
    EXPECT_RESULT(ALARM_ERR_CODE_IN_USE, user_set(CODE_MASTER, 8, CODE_USER,
                                                  ALARM_AUTH_STANDARD, 0x01));
}

/* CORE-USR-009 */
static void test_standard_user_cannot_add_users(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, user_set(CODE_USER, 8, "4444",
                                                     ALARM_AUTH_STANDARD, 0x01));
}

/* CORE-USR-001: partition master limits. */
static void test_partition_master_limits(void)
{
    EXPECT_RESULT(ALARM_OK, user_set(CODE_PMASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  user_set(CODE_PMASTER, 9, "4445", ALARM_AUTH_PARTITION_MASTER, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  user_set(CODE_PMASTER, 2, "4446", ALARM_AUTH_MASTER, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  user_set(CODE_PMASTER, 10, "4447", ALARM_AUTH_STANDARD, 0x02));
    /* own code change keeps the authority */
    EXPECT_RESULT(ALARM_OK, user_set(CODE_PMASTER, 7, "4448", ALARM_AUTH_STANDARD, 0x01));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_PARTITION_MASTER, alarm_engine_config(&S.eng)->users[6].authority);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, CODE_PMASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "4448", ALARM_LEVEL_STAY, 0));
}

/* CORE-USR-012: the installer programs every user - its own code, the system master's and the
 * household's (decided 2026-09-23). */
static void test_installer_sets_master_code(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    EXPECT_RESULT(ALARM_OK, user_set(CODE_INSTALLER, 2, "8888", ALARM_AUTH_MASTER, 0x01));
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "8888", ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, "8888"));
    /* A household code, a new user and a partition master. */
    EXPECT_RESULT(ALARM_OK, user_set(CODE_INSTALLER, 3, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "4444", ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, "4444"));
    EXPECT_RESULT(ALARM_OK, user_set(CODE_INSTALLER, 20, "4445", ALARM_AUTH_PARTITION_MASTER, 0x03));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_PARTITION_MASTER, cfg->users[19].authority);
    TEST_ASSERT_EQUAL_UINT8(0x03, cfg->users[19].partitions);
    /* Still no second installer or system master, and the installer can't delete itself. */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, user_set(CODE_INSTALLER, 21, "4446", ALARM_AUTH_MASTER, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_user_delete(&S.eng, &S.keypad, CODE_INSTALLER, 1, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_delete(&S.eng, &S.keypad, CODE_INSTALLER, 20, S.now));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_NONE, cfg->users[19].authority);
}

/* CORE-USR-025: one rule for who manages whom, asked by the engine, the touchscreen and the
 * Panel Manager alike. */
static void test_user_manages_rule(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    /* users: 1 installer, 2 master, 3 standard, 4 arm-only, 5 guest, 6 duress, 7 partition
     * master (partition 1), empty slots above */
    for (uint8_t t = 1; t <= 8; t++) {
        TEST_ASSERT_TRUE(alarm_config_user_manages(cfg, 1, t));
        TEST_ASSERT_EQUAL(t != 1, alarm_config_user_manages(cfg, 2, t));
    }
    TEST_ASSERT_TRUE(alarm_config_user_manages(cfg, 7, 7));
    TEST_ASSERT_TRUE(alarm_config_user_manages(cfg, 7, 3));
    TEST_ASSERT_TRUE(alarm_config_user_manages(cfg, 7, 8)); /* an empty slot: a new user */
    TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, 7, 1));
    TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, 7, 2));
    for (uint8_t a = 3; a <= 6; a++) { /* standard, arm-only, guest, duress: nobody */
        TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, a, a));
        TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, a, 8));
    }
    TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, 8, 3)); /* no user in the slot */
    TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, 0, 3));
    TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, 1, 0));
    TEST_ASSERT_FALSE(alarm_config_user_manages(NULL, 1, 3));
    /* A partition master stops at users with partitions outside its own. */
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 9, "4444", ALARM_AUTH_STANDARD, 0x03));
    TEST_ASSERT_FALSE(alarm_config_user_manages(cfg, 7, 9));
}

/* CORE-USR-013 */
static void test_user_delete(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  alarm_engine_user_delete(&S.eng, &S.keypad, CODE_USER, 4, S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 2, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 3, S.now));
    EXPECT_REPORTS(&S, ALARM_EV_USER_DELETED, 1);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
}

/* CORE-USR-014 */
static void test_authenticate_api(void)
{
    uint8_t user = 0;
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_authenticate(&S.eng, &S.keypad, CODE_INSTALLER, 0, S.now, &user));
    TEST_ASSERT_EQUAL_UINT8(1, user);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                  alarm_engine_authenticate(&S.eng, &S.keypad, "7777", 0, S.now, &user));
}

/* CORE-USR-015: keyfob zones act as their assigned user. */
static void test_keyfob_arm_and_disarm(void)
{
    sim_fault(&S, Z_FOB_AWAY);
    sim_restore(&S, Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT8(3, sim_status(&S, 1).armed_by);
    sim_advance(&S, SEC(61));
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    sim_fault(&S, Z_FOB_OFF);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_report(&S, ALARM_EV_DISARMED)->user);
}

/* One press of a button zone. */
static void press(uint16_t zone)
{
    sim_fault(&S, zone);
    sim_restore(&S, zone);
}

/* Restart the basic panel with the arm-away and disarm fobs bound to another user. */
static void start_with_fob_user(uint8_t user_no)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_FOB_AWAY - 1].user = user_no;
    cfg.zones[Z_FOB_OFF - 1].user = user_no;
    sim_start(&S, &cfg, NULL);
}

/* CORE-USR-015: a button zone carries its user's authority, asked again at every press - an
 * arm-only user's fob arms but never disarms, a guest's fob ends only what the guest started,
 * and a fob left pointing at an emptied user slot does nothing at all. */
static void test_button_zone_authority_rules(void)
{
    start_with_fob_user(4); /* arm-only */
    press(Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    press(Z_FOB_OFF);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    start_with_fob_user(5); /* guest */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    press(Z_FOB_OFF);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY); /* the guest didn't arm it */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    press(Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    press(Z_FOB_OFF);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    start_with_fob_user(12); /* nobody: an empty (deleted) user slot */
    press(Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);

    /* Deleting a user while the panel runs takes their fob with it. */
    start_with_fob_user(3);
    press(Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    press(Z_FOB_OFF);
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 3, S.now));
    press(Z_FOB_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* ---- CORE-USR-027: a command as an already-identified user (a Panel Manager sign-in) ---- */
static const alarm_source_t PM = {ALARM_SRC_REMOTE, ALARM_SRC_ID_PANEL_MANAGER};

static alarm_result_t as_cmd(alarm_cmd_type_t type, uint8_t p, uint8_t user, uint16_t zone)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = p;
    cmd.level = ALARM_LEVEL_AWAY;
    cmd.zone = zone;
    cmd.panic = ALARM_PANIC_POLICE;
    return alarm_engine_command_as(&S.eng, &cmd, &PM, user, S.now, &S.last);
}

static alarm_result_t as_arm(uint8_t p, uint8_t user, alarm_arm_level_t level)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = p;
    cmd.level = level;
    return alarm_engine_command_as(&S.eng, &cmd, &PM, user, S.now, &S.last);
}

/* CORE-USR-027: no code, and the events name the user and the Panel Manager as the source, so a
 * disarm from it reads apart from one at a keypad. The remote code rule is not loosened. */
static void test_command_as_arms_and_disarms_without_code(void)
{
    EXPECT_RESULT(ALARM_OK, as_arm(1, ALARM_USER_MASTER, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, sim_status(&S, 1).armed_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, S.last.user);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ARMED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, ev->user);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_REMOTE, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);

    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, 3, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    ev = sim_last_event(&S, ALARM_EV_DISARMED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(3, ev->user);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_REMOTE, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);

    /* The same source without a sign-in still needs a code (remote_arm_without_code is off). */
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_AWAY;
    EXPECT_RESULT(ALARM_ERR_CODE_REQUIRED,
                  alarm_engine_command(&S.eng, &cmd, &PM, S.now, &S.last));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* cmd->code is ignored: a wrong one neither refuses nor counts toward a lockout, and a
     * source locked out for wrong codes still runs a signed-in user's command. */
    for (unsigned i = 0; i < 8u; i++) {
        strncpy(cmd.code, "0000", sizeof(cmd.code));
        (void)alarm_engine_command(&S.eng, &cmd, &PM, S.now, &S.last);
    }
    EXPECT_RESULT(ALARM_ERR_LOCKED_OUT, alarm_engine_command(&S.eng, &cmd, &PM, S.now, &S.last));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, alarm_engine_command_as(&S.eng, &cmd, &PM, 3, S.now, &S.last));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 0);
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 0);
    TEST_ASSERT_EQUAL_UINT8(3, sim_status(&S, 1).armed_by);
}

/* CORE-USR-027: the user's own authority, asked again at every command, exactly as their code. */
static void test_command_as_follows_authority(void)
{
    /* Nobody: user 0, an empty slot, a number past the table. */
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(1, 0, ALARM_LEVEL_AWAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(1, 12, ALARM_LEVEL_AWAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(1, (uint8_t)(ALARM_MAX_USERS + 1u), ALARM_LEVEL_AWAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_SILENCE, 1, 0, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_PANIC, 1, 0, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, as_arm(0, ALARM_USER_MASTER, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    TEST_ASSERT_EQUAL_UINT(5, sim_count_sounds(&S, ALARM_SOUND_ERROR)); /* as a code's refusal */

    /* Arm-only arms, never disarms, bypasses or tests. */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_BYPASS, 1, 4, Z_WINDOW));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 1, 4, 0));
    EXPECT_RESULT(ALARM_OK, as_arm(1, 4, ALARM_LEVEL_STAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_DISARM, 1, 4, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);

    /* A guest ends only what the guest started; the installer nothing (CORE-USR-003). */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_DISARM, 1, 5, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_DISARM, 1, ALARM_USER_INSTALLER, 0));
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, ALARM_USER_MASTER, 0));
    EXPECT_RESULT(ALARM_OK, as_arm(1, 5, ALARM_LEVEL_STAY));
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, 5, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* Chime: any user of the partition, even with chime_needs_code on. Bypass: a standard user. */
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_CHIME, 1, 5, 0));
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
    TEST_ASSERT_EQUAL_UINT8(5, sim_last_event(&S, ALARM_EV_CHIME_ON)->user);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_BYPASS, 1, 3, Z_WINDOW));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_UNBYPASS, 1, 3, Z_WINDOW));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);

    /* Deleting a user takes their sign-in's authority with it at once. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 3, S.now));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(1, 3, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-USR-027: a signed-in user reaches only their own partitions (CORE-PART-002); the
 * installer and the system master reach every one. */
static void test_command_as_partition_scope(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_ENTRY_EXIT_1, 2);
    sim_set_user(&cfg, 8, ALARM_AUTH_STANDARD, 0x02, "8888");
    sim_start(&S, &cfg, NULL);

    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(1, 8, ALARM_LEVEL_AWAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_SILENCE, 1, 8, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_PANIC, 1, 8, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_CHIME, 1, 8, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_RESULT(ALARM_OK, as_arm(2, 8, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_DISARM, 2, 3, 0)); /* p1 only */
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 2, ALARM_USER_MASTER, 0));
    /* The installer reaches partition 2 too, but never to arm it (CORE-USR-003). */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(2, ALARM_USER_INSTALLER, ALARM_LEVEL_STAY));
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_CHIME, 2, ALARM_USER_INSTALLER, 0));
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);
}

/* CORE-USR-027: everything past who it is stays alarm_engine_command()'s - a duress user's
 * disarm still sends the silent duress report, a disarm in an alarm stops it and leaves the
 * memory, the next one clears it, and silence and panic name the user. */
static void test_command_as_keeps_duress_and_memory(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, 6, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_DURESS);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(6, ev->user);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);

    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, ALARM_USER_MASTER, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_TRUE(sim_status(&S, 1).memory != 0u);
    sim_restore(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, ALARM_USER_MASTER, 0));
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).memory);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, sim_last_event(&S, ALARM_EV_MEMORY_CLEARED)->user);

    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_PANIC, 1, 3, 0));
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_event(&S, ALARM_EV_ALARM)->user);
}

/* CORE-USR-027: installer mode and an arming hold refuse an arm as they do a code's. */
static void test_command_as_refused_in_program_and_hold(void)
{
    uint8_t user = 0;
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, &user));
    TEST_ASSERT_TRUE(S.eng.program);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, as_arm(1, ALARM_USER_MASTER, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);

    /* Out of programming again, a hold answers ALARM_ERR_HELD, and lifting it arms. */
    S.eng.program = false;
    alarm_engine_hold_arming(&S.eng, true);
    EXPECT_RESULT(ALARM_ERR_HELD, as_arm(1, ALARM_USER_MASTER, ALARM_LEVEL_AWAY));
    EXPECT_RESULT(ALARM_ERR_HELD, as_arm(1, 3, ALARM_LEVEL_STAY));
    EXPECT_RESULT(ALARM_ERR_HELD, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);
    alarm_engine_hold_arming(&S.eng, false);
    EXPECT_RESULT(ALARM_OK, as_arm(1, 3, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
}

/* CORE-USR-027: a duress user arming as the sign-in sends the same silent report as their code. */
static void test_command_as_duress_on_arming(void)
{
    EXPECT_RESULT(ALARM_OK, as_arm(1, 6, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_REPORTS(&S, ALARM_EV_ARMED, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_DURESS);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(6, ev->user);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);
    TEST_ASSERT_FALSE(sim_status(&S, 1).triggered);

    /* A normal user arms without one. */
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, 3, 0));
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, as_arm(1, 3, ALARM_LEVEL_AWAY));
    EXPECT_REPORTS(&S, ALARM_EV_DURESS, 0);
}

/* CORE-USR-027: exit restart, quick bypass and the walk test follow the user's authority. */
static void test_command_as_restart_quick_bypass_and_walk_test(void)
{
    /* Quick bypass: disarmed only; a duress user may (as their code), an arm-only user may not. */
    sim_fault(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_QUICK_BYPASS, 1, 4, 0));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_QUICK_BYPASS, 1, 3, 0));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_event(&S, ALARM_EV_BYPASS)->user);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_UNBYPASS, 1, 6, Z_WINDOW));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_QUICK_BYPASS, 1, 6, 0));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_UNBYPASS, 1, 3, Z_WINDOW));
    sim_restore(&S, Z_WINDOW);

    /* Walk test (WALK-001, WALK-025): a partition master starts it; standard, guest, arm-only
     * and duress users may not. Anyone who may disarm the partition ends it. */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 1, 3, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 1, 5, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 1, 4, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 1, 6, 0));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_WALK_TEST, 1, 7, 0));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    TEST_ASSERT_EQUAL_UINT8(7, sim_last_event(&S, ALARM_EV_WALK_TEST_START)->user);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 1, 4, 0));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_WALK_TEST, 1, 3, 0));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_event(&S, ALARM_EV_WALK_TEST_END)->user);

    /* Quick bypass is for a disarmed partition; exit restart for one armed Stay or Night. */
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, as_cmd(ALARM_CMD_EXIT_RESTART, 1, 3, 0));
    sim_arm_stay_settled(&S, CODE_USER);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, as_cmd(ALARM_CMD_QUICK_BYPASS, 1, 3, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_EXIT_RESTART, 1, 0, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_EXIT_RESTART, 1, 3, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_EXIT_RESTART);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(3, ev->user);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);
}

/* CORE-USR-027: a partition master acts on its own partitions and no other; the installer and
 * the system master reach every one. */
static void test_command_as_partition_master(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_ENTRY_EXIT_1, 2);
    sim_start(&S, &cfg, NULL);

    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(2, 7, ALARM_LEVEL_AWAY));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_BYPASS, 2, 7, 30));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_WALK_TEST, 2, 7, 0));
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);
    EXPECT_RESULT(ALARM_OK, as_arm(1, 7, ALARM_LEVEL_STAY));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 1, 7, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    EXPECT_RESULT(ALARM_OK, as_arm(2, ALARM_USER_MASTER, ALARM_LEVEL_AWAY));
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_cmd(ALARM_CMD_DISARM, 2, 7, 0));
    EXPECT_RESULT(ALARM_OK, as_cmd(ALARM_CMD_DISARM, 2, ALARM_USER_MASTER, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_arm(2, ALARM_USER_INSTALLER, ALARM_LEVEL_STAY));
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED); /* the installer never arms (CORE-USR-003) */
}

/* ---- CORE-USR-028: user programming as an already-identified user (a Panel Manager sign-in) */
static alarm_result_t as_set(uint8_t actor, uint8_t user_no, const char *code,
                             alarm_authority_t auth, uint8_t parts)
{
    return alarm_engine_user_set_as(&S.eng, &PM, actor, user_no, code, auth, parts, "Sam", S.now);
}

static alarm_result_t as_update(uint8_t actor, uint8_t user_no, alarm_authority_t auth,
                                uint8_t parts, const char *name)
{
    return alarm_engine_user_update_as(&S.eng, &PM, actor, user_no, auth, parts, name, S.now);
}

static alarm_result_t as_delete(uint8_t actor, uint8_t user_no)
{
    return alarm_engine_user_delete_as(&S.eng, &PM, actor, user_no, S.now);
}

static void expect_by_pm(alarm_event_type_t type, uint8_t actor, uint8_t target)
{
    const alarm_event_t *ev = sim_last_event(&S, type);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(actor, ev->user);
    TEST_ASSERT_EQUAL_UINT8(target, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_REMOTE, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(ALARM_SRC_ID_PANEL_MANAGER, ev->source.id);
}

/* CORE-USR-028: add, change and delete without a code; the events name the actor and the
 * Panel Manager as the source, and the new code works at a keypad. */
static void test_user_as_add_change_delete(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_OK, as_set(ALARM_USER_MASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_STANDARD, cfg->users[7].authority);
    TEST_ASSERT_EQUAL_STRING("Sam", cfg->users[7].name);
    expect_by_pm(ALARM_EV_USER_CHANGED, ALARM_USER_MASTER, 8);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "4444", ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, "4444"));

    EXPECT_RESULT(ALARM_OK, as_update(ALARM_USER_MASTER, 8, ALARM_AUTH_ARM_ONLY, 0x01, "Pat"));
    TEST_ASSERT_EQUAL_STRING("Pat", cfg->users[7].name);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_ARM_ONLY, cfg->users[7].authority);
    expect_by_pm(ALARM_EV_USER_UPDATED, ALARM_USER_MASTER, 8);

    /* The installer changes the master's code (CORE-USR-012), as with the installer code. */
    EXPECT_RESULT(ALARM_OK, as_set(ALARM_USER_INSTALLER, 2, "8642", ALARM_AUTH_STANDARD, 0x01));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_MASTER, cfg->users[1].authority);
    expect_by_pm(ALARM_EV_USER_CHANGED, ALARM_USER_INSTALLER, 2);

    EXPECT_RESULT(ALARM_OK, as_delete(ALARM_USER_MASTER, 8));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_NONE, cfg->users[7].authority);
    expect_by_pm(ALARM_EV_USER_DELETED, ALARM_USER_MASTER, 8);
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 0);
    TEST_ASSERT_EQUAL_UINT(4, S.n_config_changed);
}

/* CORE-USR-028: the actor's slot is read again at every call: nobody, an empty slot, a number
 * past the table, a user deleted or lowered since the sign-in are all refused. */
static void test_user_as_needs_a_current_user(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_set(0, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_set(12, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  as_set((uint8_t)(ALARM_MAX_USERS + 1u), 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_update(0, 3, ALARM_AUTH_ARM_ONLY, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_update(12, 3, ALARM_AUTH_ARM_ONLY, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_delete(0, 3));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_delete(12, 3));

    /* The partition master signs in, then the master lowers and deletes them. */
    EXPECT_RESULT(ALARM_OK, as_set(7, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, as_update(ALARM_USER_MASTER, 7, ALARM_AUTH_STANDARD, 0x01, NULL));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_set(7, 9, "2468", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, as_delete(ALARM_USER_MASTER, 7));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_set(7, 9, "2468", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_update(7, 8, ALARM_AUTH_ARM_ONLY, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_delete(7, 8));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_NONE, cfg->users[8].authority);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_STANDARD, cfg->users[7].authority);
    TEST_ASSERT_EQUAL_STRING("Sam", cfg->users[7].name);
    EXPECT_EVENTS(&S, ALARM_EV_USER_CHANGED, 1);
    EXPECT_EVENTS(&S, ALARM_EV_USER_UPDATED, 1);
    EXPECT_EVENTS(&S, ALARM_EV_USER_DELETED, 1);
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 0);
}

/* CORE-USR-028: exactly the coded rules with the actor's authority (CORE-USR-010..012, 025):
 * the master can't touch the installer, a standard user manages nobody, a partition master
 * only low authorities in its own partitions and never itself away. */
static void test_user_as_follows_authority(void)
{
    alarm_config_t c;
    sim_config_basic(&c);
    c.partitions[1].enabled = true;
    sim_set_user(&c, 8, ALARM_AUTH_STANDARD, 0x02, "4321");
    sim_start(&S, &c, NULL);
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);

    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  as_set(ALARM_USER_MASTER, 1, "97531", ALARM_AUTH_INSTALLER, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  as_update(ALARM_USER_MASTER, 1, ALARM_AUTH_INSTALLER, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, as_delete(ALARM_USER_MASTER, 1));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_set(3, 9, "2468", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_update(3, 4, ALARM_AUTH_STANDARD, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_delete(3, 4));

    /* Partition master 7 (partition 1). */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_update(7, 8, ALARM_AUTH_ARM_ONLY, 0x02, "x"));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_delete(7, 8));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_set(7, 9, "2468", ALARM_AUTH_STANDARD, 0x02));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  as_set(7, 9, "2468", ALARM_AUTH_PARTITION_MASTER, 0x01));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, as_delete(7, 7));
    EXPECT_RESULT(ALARM_OK, as_set(7, 9, "2468", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, as_update(7, 7, ALARM_AUTH_MASTER, 0x03, "Upstairs"));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_PARTITION_MASTER, cfg->users[6].authority);
    TEST_ASSERT_EQUAL_UINT8(0x01, cfg->users[6].partitions);
    EXPECT_RESULT(ALARM_OK, as_delete(7, 9));
    EXPECT_RESULT(ALARM_OK, as_delete(ALARM_USER_MASTER, 8));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_NONE, cfg->users[7].authority);
}

/* CORE-USR-028: code policy, code in use and the partitions' state, as with a code. */
static void test_user_as_code_policy_and_state(void)
{
    alarm_config_t c;
    sim_config_basic(&c);
    c.opt.code_min_len = 6;
    sim_start(&S, &c, NULL);
    sim_clear_records(&S);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, as_set(ALARM_USER_MASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, as_set(ALARM_USER_MASTER, 8, "44a444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, as_set(ALARM_USER_MASTER, 8, NULL, ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_CODE_IN_USE,
                  as_set(ALARM_USER_MASTER, 8, CODE_MASTER, ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, as_update(ALARM_USER_MASTER, 8, ALARM_AUTH_STANDARD, 0x01, "x"));
    EXPECT_RESULT(ALARM_OK, as_set(ALARM_USER_MASTER, 8, "444444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, as_set(ALARM_USER_MASTER, 8, "444444", ALARM_AUTH_STANDARD, 0x01));

    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, as_set(ALARM_USER_MASTER, 9, "246800", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, as_update(ALARM_USER_MASTER, 8, ALARM_AUTH_ARM_ONLY, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, as_delete(ALARM_USER_MASTER, 8));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, as_delete(ALARM_USER_MASTER, 8));
    EXPECT_EVENTS(&S, ALARM_EV_USER_CHANGED, 2);
    EXPECT_EVENTS(&S, ALARM_EV_USER_DELETED, 1);
}

/* CORE-USR-028: no wrong-code counting either way: a keypad's count of wrong codes is neither
 * raised by a refused call nor reset by an accepted one, and a locked-out source still runs. */
static void test_user_as_leaves_lockout_alone(void)
{
    for (int i = 0; i < 4; i++) {
        EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_disarm(&S, 1, "0000"));
    }
    for (int i = 0; i < 4; i++) {
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                      alarm_engine_user_set_as(&S.eng, &S.keypad, 0, 8, "4444",
                                               ALARM_AUTH_STANDARD, 0x01, NULL, S.now));
        EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                      alarm_engine_user_delete_as(&S.eng, &S.keypad, 3, 4, S.now));
    }
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 0);
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_set_as(&S.eng, &S.keypad, ALARM_USER_MASTER, 8,
                                                     "4444", ALARM_AUTH_STANDARD, 0x01, NULL,
                                                     S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_update_as(&S.eng, &S.keypad, ALARM_USER_MASTER, 8,
                                                        ALARM_AUTH_STANDARD, 0x01, "Alex",
                                                        S.now));
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 4);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_disarm(&S, 1, "0000")); /* the fifth: not reset */
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 1);
    EXPECT_RESULT(ALARM_ERR_LOCKED_OUT,
                  alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 8, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_delete_as(&S.eng, &S.keypad, ALARM_USER_MASTER, 8,
                                                        S.now));
    TEST_ASSERT_TRUE(alarm_engine_lockout_left(&S.eng, &S.keypad, S.now) > 0u);
}

/* CORE-USR-016: keyswitch toggles arm away / disarm. */
static void test_keyswitch_toggles(void)
{
    sim_fault(&S, Z_KEYSWITCH);
    sim_restore(&S, Z_KEYSWITCH);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    sim_fault(&S, Z_KEYSWITCH);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-USR-017: each user's code can be 4-10 digits (iPhone-style). */
static void test_codes_can_be_4_to_10_digits(void)
{
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 8, "1234567890", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 9, "24680", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, user_set(CODE_MASTER, 10, "123", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG,
                  user_set(CODE_MASTER, 10, "12345678901", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, "1234567890", ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, "24680"));
    /* A code that only starts with someone else's code is a different code. */
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE, sim_arm(&S, 1, "11112", ALARM_LEVEL_STAY, 0));
}

/* CORE-USR-018: the minimum length applies to new codes; existing codes keep working. */
static void test_min_length_policy_for_new_codes(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.code_min_len = 6;
    sim_start(&S, &cfg, NULL);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, user_set(CODE_MASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 8, "444444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0)); /* 4-digit, older */
}

/* CORE-USR-019: factory codes are installer 69420 and master 123456; the panel
 * reports while they are still in use. */
static void test_default_codes(void)
{
    TEST_ASSERT_EQUAL_STRING("69420", ALARM_DEFAULT_INSTALLER_CODE);
    TEST_ASSERT_EQUAL_STRING("123456", ALARM_DEFAULT_MASTER_CODE);
    uint8_t user = 0;
    EXPECT_RESULT(ALARM_OK, alarm_engine_authenticate(&S.eng, &S.keypad, "69420", 0, S.now, &user));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, user);
    EXPECT_RESULT(ALARM_OK, alarm_engine_authenticate(&S.eng, &S.keypad, "123456", 0, S.now, &user));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, user);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEFAULT_CODE_INSTALLER | ALARM_DEFAULT_CODE_MASTER,
                            alarm_engine_default_codes(&S.eng));

    const unsigned before = S.status_calls[0];
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 2, "8642", ALARM_AUTH_MASTER, 0x01));
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEFAULT_CODE_INSTALLER, alarm_engine_default_codes(&S.eng));
    TEST_ASSERT_TRUE(S.status_calls[0] > before);
    EXPECT_RESULT(ALARM_OK, user_set(CODE_INSTALLER, 1, "97531", ALARM_AUTH_INSTALLER, 0x01));
    TEST_ASSERT_EQUAL_UINT8(0, alarm_engine_default_codes(&S.eng));
}

/* CORE-USR-020: the default-code check only looks at users 01 and 02. */
static void test_default_codes_absent(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_set_user(&cfg, 2, ALARM_AUTH_MASTER, 0x0F, "7777");
    sim_start(&S, &cfg, NULL);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEFAULT_CODE_INSTALLER, alarm_engine_default_codes(&S.eng));
}

static alarm_result_t user_update(const char *actor, uint8_t user_no, alarm_authority_t auth,
                                  uint8_t parts, const char *name)
{
    return alarm_engine_user_update(&S.eng, &S.keypad, actor, user_no, auth, parts, name, S.now);
}

/* CORE-USR-021: rename a user and change its authority; the code keeps working. */
static void test_user_update_keeps_code(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    EXPECT_RESULT(ALARM_OK, user_update(CODE_MASTER, 3, ALARM_AUTH_ARM_ONLY, 0x01, "Dog walker"));
    TEST_ASSERT_EQUAL_STRING("Dog walker", cfg->users[2].name);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_ARM_ONLY, cfg->users[2].authority);
    EXPECT_REPORTS(&S, ALARM_EV_USER_UPDATED, 1);
    TEST_ASSERT_EQUAL_UINT8(3, sim_last_event(&S, ALARM_EV_USER_UPDATED)->arg);
    TEST_ASSERT_EQUAL_UINT(1, S.n_config_changed);

    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0)); /* same code */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_disarm(&S, 1, CODE_USER));   /* now arm only */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    EXPECT_RESULT(ALARM_OK, user_update(CODE_MASTER, 3, ALARM_AUTH_STANDARD, 0x01, NULL));
    TEST_ASSERT_EQUAL_STRING("Dog walker", cfg->users[2].name); /* NULL keeps the name */
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_STANDARD, cfg->users[2].authority);
}

/* CORE-USR-022: user updates follow the programming rules. */
static void test_user_update_rules(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  user_update(CODE_USER, 4, ALARM_AUTH_STANDARD, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  user_update(CODE_PMASTER, 3, ALARM_AUTH_PARTITION_MASTER, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, user_update(CODE_MASTER, 20, ALARM_AUTH_STANDARD, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                  user_update("8888", 3, ALARM_AUTH_STANDARD, 0x01, "x"));

    /* The installer renames the master; slot 2 stays the system master. */
    EXPECT_RESULT(ALARM_OK, user_update(CODE_INSTALLER, 2, ALARM_AUTH_STANDARD, 0x01, "Homeowner"));
    TEST_ASSERT_EQUAL_STRING("Homeowner", cfg->users[1].name);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_MASTER, cfg->users[1].authority);
    /* ...and changes household users too (CORE-USR-012). */
    EXPECT_RESULT(ALARM_OK, user_update(CODE_INSTALLER, 3, ALARM_AUTH_ARM_ONLY, 0x01, "Kitchen"));
    TEST_ASSERT_EQUAL_STRING("Kitchen", cfg->users[2].name);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_ARM_ONLY, cfg->users[2].authority);

    /* Your own entry: new name, same authority and partitions. */
    EXPECT_RESULT(ALARM_OK, user_update(CODE_PMASTER, 7, ALARM_AUTH_STANDARD, 0x03, "Upstairs"));
    TEST_ASSERT_EQUAL_STRING("Upstairs", cfg->users[6].name);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_PARTITION_MASTER, cfg->users[6].authority);
    TEST_ASSERT_EQUAL_UINT8(0x01, cfg->users[6].partitions);
}

/* CORE-USR-023: users are added, changed and deleted only while their partitions are
 * disarmed; from exit time on, the codes are locked. */
static void test_no_user_changes_while_armed(void)
{
    const alarm_config_t *cfg = alarm_engine_config(&S.eng);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, user_set(CODE_MASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, user_set(CODE_MASTER, 2, "8642", ALARM_AUTH_MASTER, 0x01));
    sim_advance(&S, SEC(61));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, user_update(CODE_MASTER, 3, ALARM_AUTH_ARM_ONLY, 0x01, "x"));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 3, S.now));
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_NONE, cfg->users[7].authority);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_STANDARD, cfg->users[2].authority);
    EXPECT_EVENTS(&S, ALARM_EV_USER_CHANGED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_USER_UPDATED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_USER_DELETED, 0);
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                  user_set("8888", 8, "4444", ALARM_AUTH_STANDARD, 0x01)); /* still counted */

    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 8, "4444", ALARM_AUTH_STANDARD, 0x01));
}

static uint32_t hash_failures;
static unsigned hash_fail_after;

static void fault_hash(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                        uint8_t out[ALARM_CODE_HASH_LEN])
{
    sim_hash(ctx, salt, code, out);
    if (hash_fail_after != 0u && --hash_fail_after == 0u) {
        /* Even a digest that happens to match must not authorize a failed operation. */
        hash_failures++;
    }
}

static uint32_t fault_count(void *ctx)
{
    (void)ctx;
    return hash_failures;
}

static void enable_hash_faults(void)
{
    hash_failures = 0;
    hash_fail_after = 0;
    S.eng.ports.hash_code = fault_hash;
    S.eng.ports.hash_faults = fault_count;
}

/* CORE-USR-024 / B001: a resource failure is neither a wrong code nor a keypad lockout. */
static void test_hash_failure_does_not_lock_out_installer(void)
{
    enable_hash_faults();
    uint8_t user = 0;
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                  alarm_engine_program_enter(&S.eng, &S.keypad, "8888", S.now, &user));
    EXPECT_RESULT(ALARM_ERR_INVALID_CODE,
                  alarm_engine_program_enter(&S.eng, &S.keypad, "8888", S.now, &user));
    for (unsigned i = 0; i < 8; i++) {
        hash_fail_after = 1;
        EXPECT_RESULT(ALARM_ERR_UNAVAILABLE,
                      alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, &user));
        TEST_ASSERT_FALSE(S.eng.program);
    }
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 2);
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 0);
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, &user));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, user);
}

/* CORE-USR-024: disarming retries normally after a hash failure, with no false success. */
static void test_disarm_after_hash_failure(void)
{
    enable_hash_faults();
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    hash_fail_after = 1;
    EXPECT_RESULT(ALARM_ERR_UNAVAILABLE, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 0);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-USR-024: failure hashing the NEW code must not replace a usable stored credential. */
static void test_failed_new_hash_keeps_old_user(void)
{
    enable_hash_faults();
    const alarm_user_t old = S.eng.cfg.users[2];
    hash_fail_after = 2; /* actor succeeds; new credential fails */
    EXPECT_RESULT(ALARM_ERR_UNAVAILABLE,
                  user_set(CODE_MASTER, 3, "4444", ALARM_AUTH_STANDARD, 0x01));
    TEST_ASSERT_EQUAL_MEMORY(&old, &S.eng.cfg.users[2], sizeof(old));
    TEST_ASSERT_EQUAL_UINT(0, S.n_config_changed);
    EXPECT_EVENTS(&S, ALARM_EV_USER_CHANGED, 0);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, user_set(CODE_MASTER, 3, "4444", ALARM_AUTH_STANDARD, 0x01));
}

/* CORE-USR-024: installer programming uses the same pre-storage fault gate. */
static void test_failed_program_hash_keeps_old_code(void)
{
    enable_hash_faults();
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, NULL));
    const alarm_user_t old = S.eng.cfg.users[ALARM_USER_MASTER - 1u];
    hash_fail_after = 1;
    EXPECT_RESULT(ALARM_ERR_UNAVAILABLE,
                  alarm_engine_program_set_code(&S.eng, &S.keypad, ALARM_USER_MASTER,
                                                 "4444", S.now));
    TEST_ASSERT_EQUAL_MEMORY(&old, &S.eng.cfg.users[ALARM_USER_MASTER - 1u], sizeof(old));
    TEST_ASSERT_EQUAL_UINT(0, S.n_config_changed);
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_set_code(&S.eng, &S.keypad, ALARM_USER_MASTER,
                                                 "4444", S.now));
}

/* ---- CORE-USR-003, USR-001 (decided 2026-10-07): the installer code never arms or disarms */
static alarm_result_t src_cmd(const alarm_source_t *src, alarm_cmd_type_t type, const char *code,
                              alarm_arm_level_t level, uint8_t flags)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = 1;
    cmd.level = level;
    cmd.arm_flags = flags;
    strncpy(cmd.code, code, ALARM_CODE_MAX_LEN);
    return alarm_engine_command(&S.eng, &cmd, src, S.now, &S.last);
}

/* A refusal as for any code that may not do it: NOT_AUTHORIZED, the error sound, no arm or
 * disarm event, nothing changed. */
static void expect_installer_refused(alarm_result_t r, alarm_arm_state_t state)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, r);
    EXPECT_STATE(&S, 1, state);
    TEST_ASSERT_EQUAL_UINT(1, sim_count_sounds(&S, ALARM_SOUND_ERROR));
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_ARM_FAILED, 0);
    EXPECT_EVENTS(&S, ALARM_EV_DISARMED, 0);
    sim_clear_records(&S);
}

/* CORE-USR-003, USR-001: no arming at any level - Away, Stay, Night, Instant (Stay with no entry
 * delay), Maximum (Away with none) - from this keypad, a remote keypad, Home Assistant/MQTT or a
 * Panel Manager sign-in; the same code keeps its lockout count clean (a right code). */
static void test_installer_never_arms(void)
{
    static const alarm_source_t sources[] = {
        {ALARM_SRC_KEYPAD, ALARM_KEYPAD_LOCAL},
        {ALARM_SRC_KEYPAD, 1u}, /* a remote keypad */
        {ALARM_SRC_REMOTE, 1u}, /* Home Assistant / MQTT */
        {ALARM_SRC_REMOTE, ALARM_SRC_ID_PANEL_MANAGER},
    };
    static const struct {
        alarm_arm_level_t level;
        uint8_t flags;
    } modes[] = {
        {ALARM_LEVEL_AWAY, 0u},  {ALARM_LEVEL_STAY, 0u},
        {ALARM_LEVEL_NIGHT, 0u}, {ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY},
        {ALARM_LEVEL_AWAY, ALARM_ARM_NO_ENTRY_DELAY},
    };
    sim_clear_records(&S);
    for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            expect_installer_refused(src_cmd(&sources[i], ALARM_CMD_ARM, CODE_INSTALLER,
                                             modes[m].level, modes[m].flags),
                                     ALARM_STATE_DISARMED);
        }
        /* The same arm with the master's code goes through, so only the code was refused. */
        EXPECT_RESULT(ALARM_OK, src_cmd(&sources[i], ALARM_CMD_ARM, CODE_MASTER,
                                        ALARM_LEVEL_STAY, 0u));
        EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
        sim_clear_records(&S);
    }
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) { /* signed in, no code */
        alarm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ALARM_CMD_ARM;
        cmd.partition = 1;
        cmd.level = modes[m].level;
        cmd.arm_flags = modes[m].flags;
        expect_installer_refused(alarm_engine_command_as(&S.eng, &cmd, &PM, ALARM_USER_INSTALLER,
                                                         S.now, &S.last),
                                 ALARM_STATE_DISARMED);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)alarm_engine_lockout_left(&S.eng, &S.keypad, S.now));
}

/* CORE-USR-003, USR-001: no disarm by the installer, whoever armed - in exit time, armed, in
 * entry time or during an alarm - at a keypad, from Home Assistant/MQTT or signed in. */
static void test_installer_never_disarms(void)
{
    static const alarm_source_t ha = {ALARM_SRC_REMOTE, 1u};
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    sim_clear_records(&S);
    expect_installer_refused(sim_disarm(&S, 1, CODE_INSTALLER), ALARM_STATE_EXIT_DELAY);
    expect_installer_refused(as_cmd(ALARM_CMD_DISARM, 1, ALARM_USER_INSTALLER, 0),
                             ALARM_STATE_EXIT_DELAY);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));

    sim_arm_away_and_leave(&S, CODE_MASTER);
    sim_clear_records(&S);
    expect_installer_refused(sim_disarm(&S, 1, CODE_INSTALLER), ALARM_STATE_ARMED);
    expect_installer_refused(src_cmd(&ha, ALARM_CMD_DISARM, CODE_INSTALLER, ALARM_LEVEL_NONE, 0u),
                             ALARM_STATE_ARMED);

    sim_fault(&S, Z_FRONT);
    sim_restore(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
    sim_clear_records(&S);
    expect_installer_refused(sim_disarm(&S, 1, CODE_INSTALLER), ALARM_STATE_ENTRY_DELAY);
    expect_installer_refused(as_cmd(ALARM_CMD_DISARM, 1, ALARM_USER_INSTALLER, 0),
                             ALARM_STATE_ENTRY_DELAY);

    sim_advance(&S, SEC(120)); /* entry time runs out: the alarm sounds */
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
    const alarm_arm_state_t sounding = sim_status(&S, 1).state;
    TEST_ASSERT_TRUE(sounding != ALARM_STATE_DISARMED);
    sim_clear_records(&S);
    expect_installer_refused(sim_disarm(&S, 1, CODE_INSTALLER), sounding);
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
    expect_installer_refused(src_cmd(&ha, ALARM_CMD_DISARM, CODE_INSTALLER, ALARM_LEVEL_NONE, 0u),
                             sounding);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* Restarting exit time of a Stay arming opens the doors: refused too. */
    sim_arm_stay_settled(&S, CODE_MASTER);
    sim_clear_records(&S);
    expect_installer_refused(sim_simple(&S, ALARM_CMD_EXIT_RESTART, 1, CODE_INSTALLER),
                             ALARM_STATE_ARMED);
}

/* CORE-USR-003, CORE-USR-015: the installer's fob and keyswitch do nothing either. */
static void test_installer_buttons_never_arm_or_disarm(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_FOB_AWAY - 1].user = ALARM_USER_INSTALLER;
    cfg.zones[Z_FOB_OFF - 1].user = ALARM_USER_INSTALLER;
    cfg.zones[Z_KEYSWITCH - 1].user = ALARM_USER_INSTALLER;
    sim_start(&S, &cfg, NULL);
    sim_clear_records(&S);
    press(Z_FOB_AWAY);
    press(Z_KEYSWITCH);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    press(Z_FOB_OFF);
    press(Z_KEYSWITCH);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_EVENTS(&S, ALARM_EV_DISARMED, 0);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, sim_status(&S, 1).armed_by);
}

/* CORE-USR-003, WALK-025, CORE-USR-034: what the installer keeps on a disarmed partition - code +
 * OFF ends a walk test it started, silences a fire and clears the memory (none disarms
 * anything); chime and bypass; installer mode. A code check never says it may disarm. */
static void test_installer_keeps_service_work(void)
{
    alarm_cmd_t walk;
    memset(&walk, 0, sizeof(walk));
    walk.type = ALARM_CMD_WALK_TEST;
    walk.partition = 1;
    walk.walk_op = (uint8_t)ALARM_WALK_START;
    strncpy(walk.code, CODE_INSTALLER, ALARM_CODE_MAX_LEN);
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &walk));
    TEST_ASSERT_TRUE(sim_status(&S, 1).walk_test);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_INSTALLER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);
    EXPECT_EVENTS(&S, ALARM_EV_WALK_TEST_END, 1);
    EXPECT_EVENTS(&S, ALARM_EV_DISARMED, 0);
    walk.walk_op = (uint8_t)ALARM_WALK_TOGGLE; /* and the explicit end, by toggle */
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &walk));
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &walk));
    TEST_ASSERT_FALSE(sim_status(&S, 1).walk_test);

    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_INSTALLER));
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_INSTALLER, Z_WINDOW));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);

    sim_fault(&S, Z_SMOKE);
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
    sim_restore(&S, Z_SMOKE);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_INSTALLER)); /* silences the fire */
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_DISARMED, 0);

    alarm_cmd_t check;
    memset(&check, 0, sizeof(check));
    check.type = ALARM_CMD_CODE_CHECK;
    check.partition = 1;
    strncpy(check.code, CODE_INSTALLER, ALARM_CODE_MAX_LEN);
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &check));
    TEST_ASSERT_FALSE(S.last.may_disarm);

    uint8_t user = 0u;
    EXPECT_RESULT(ALARM_OK,
                  alarm_engine_program_enter(&S.eng, &S.keypad, CODE_INSTALLER, S.now, &user));
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, user);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arm_only_cannot_disarm);
    RUN_TEST(test_guest_disarms_only_what_guest_armed);
    RUN_TEST(test_installer_never_arms);
    RUN_TEST(test_installer_never_disarms);
    RUN_TEST(test_installer_buttons_never_arm_or_disarm);
    RUN_TEST(test_installer_keeps_service_work);
    RUN_TEST(test_duress);
    RUN_TEST(test_duress_on_arming);
    RUN_TEST(test_lockout);
    RUN_TEST(test_lockout_left);
    RUN_TEST(test_valid_code_resets_failures);
    RUN_TEST(test_master_adds_user);
    RUN_TEST(test_code_must_be_unique);
    RUN_TEST(test_standard_user_cannot_add_users);
    RUN_TEST(test_partition_master_limits);
    RUN_TEST(test_installer_sets_master_code);
    RUN_TEST(test_user_manages_rule);
    RUN_TEST(test_user_delete);
    RUN_TEST(test_authenticate_api);
    RUN_TEST(test_keyfob_arm_and_disarm);
    RUN_TEST(test_button_zone_authority_rules);
    RUN_TEST(test_command_as_arms_and_disarms_without_code);
    RUN_TEST(test_command_as_follows_authority);
    RUN_TEST(test_command_as_partition_scope);
    RUN_TEST(test_command_as_keeps_duress_and_memory);
    RUN_TEST(test_command_as_refused_in_program_and_hold);
    RUN_TEST(test_command_as_duress_on_arming);
    RUN_TEST(test_command_as_restart_quick_bypass_and_walk_test);
    RUN_TEST(test_command_as_partition_master);
    RUN_TEST(test_user_as_add_change_delete);
    RUN_TEST(test_user_as_needs_a_current_user);
    RUN_TEST(test_user_as_follows_authority);
    RUN_TEST(test_user_as_code_policy_and_state);
    RUN_TEST(test_user_as_leaves_lockout_alone);
    RUN_TEST(test_keyswitch_toggles);
    RUN_TEST(test_codes_can_be_4_to_10_digits);
    RUN_TEST(test_min_length_policy_for_new_codes);
    RUN_TEST(test_default_codes);
    RUN_TEST(test_default_codes_absent);
    RUN_TEST(test_user_update_keeps_code);
    RUN_TEST(test_user_update_rules);
    RUN_TEST(test_no_user_changes_while_armed);
    RUN_TEST(test_hash_failure_does_not_lock_out_installer);
    RUN_TEST(test_disarm_after_hash_failure);
    RUN_TEST(test_failed_new_hash_keeps_old_user);
    RUN_TEST(test_failed_program_hash_keeps_old_code);
    return UNITY_END();
}
