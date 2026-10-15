/*
 * test_partitions.c - two independent partitions sharing one panel.
 * Requirements: CORE-PART-*
 */
#include "sim.h"

#include "alarm/alarm_snapshot.h"

#define Z_P2_DOOR   30
#define Z_P2_WINDOW 31
#define CODE_P2     "6666"
#define CODE_P2_MGR "7777"

/* Button and keyswitch zones for the partition-scope tests. The partition-1 arm-away, disarm
 * and keyswitch buttons are the basic layout's (17, 18, 19), re-bound to another user. */
#define Z_P1_STAY  32
#define Z_P1_AWAY  Z_FOB_AWAY
#define Z_P1_OFF   Z_FOB_OFF
#define Z_P1_KEY   Z_KEYSWITCH
#define Z_P2_STAY  33
#define Z_P2_AWAY  34
#define Z_P2_OFF   35
#define Z_P2_KEY   36
#define Z_P1_MKEY  37 /* keyswitch held by the system master */
#define Z_P2_MKEY  38
#define Z_P1_IAWAY 39 /* arm away / disarm held by the installer */
#define Z_P1_IOFF  40

static sim_t S;

void setUp(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, Z_P2_DOOR, ALARM_ZT_ENTRY_EXIT_1, 2);
    sim_set_zone(&cfg, Z_P2_WINDOW, ALARM_ZT_PERIMETER, 2);
    sim_set_user(&cfg, 8, ALARM_AUTH_STANDARD, 0x02, CODE_P2);
    sim_set_user(&cfg, 9, ALARM_AUTH_PARTITION_MASTER, 0x02, CODE_P2_MGR);
    sim_start(&S, &cfg, NULL);
}

void tearDown(void) {}

/* One press of a button zone. */
static void press(uint16_t zone)
{
    sim_fault(&S, zone);
    sim_restore(&S, zone);
}

/* Two partitions, both full of button zones. Every button of a partition pair is bound to the
 * same user 8, so the only thing that decides whether a press works is that user's partition
 * mask; the master and installer buttons are there to show their reach is unchanged. */
static void start_with_buttons(uint8_t user8_partitions)
{
    static const uint16_t user8_buttons[] = {Z_P1_STAY, Z_P1_AWAY, Z_P1_OFF, Z_P1_KEY,
                                             Z_P2_STAY, Z_P2_AWAY, Z_P2_OFF, Z_P2_KEY};
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, Z_P2_DOOR, ALARM_ZT_ENTRY_EXIT_1, 2);
    sim_set_zone(&cfg, Z_P1_STAY, ALARM_ZT_ARM_STAY, 1);
    sim_set_zone(&cfg, Z_P2_STAY, ALARM_ZT_ARM_STAY, 2);
    sim_set_zone(&cfg, Z_P2_AWAY, ALARM_ZT_ARM_AWAY, 2);
    sim_set_zone(&cfg, Z_P2_OFF, ALARM_ZT_DISARM, 2);
    sim_set_zone(&cfg, Z_P2_KEY, ALARM_ZT_KEYSWITCH, 2);
    sim_set_zone(&cfg, Z_P1_MKEY, ALARM_ZT_KEYSWITCH, 1);
    sim_set_zone(&cfg, Z_P2_MKEY, ALARM_ZT_KEYSWITCH, 2);
    sim_set_zone(&cfg, Z_P1_IAWAY, ALARM_ZT_ARM_AWAY, 1);
    sim_set_zone(&cfg, Z_P1_IOFF, ALARM_ZT_DISARM, 1);
    for (unsigned i = 0; i < sizeof(user8_buttons) / sizeof(user8_buttons[0]); i++) {
        cfg.zones[user8_buttons[i] - 1u].user = 8;
    }
    cfg.zones[Z_P1_MKEY - 1u].user = ALARM_USER_MASTER;
    cfg.zones[Z_P2_MKEY - 1u].user = ALARM_USER_MASTER;
    cfg.zones[Z_P1_IAWAY - 1u].user = ALARM_USER_INSTALLER;
    cfg.zones[Z_P1_IOFF - 1u].user = ALARM_USER_INSTALLER;
    cfg.zones[Z_MOM_EXIT - 1u].user = 8; /* partition 1, like the rest of the basic layout */
    sim_set_user(&cfg, 8, ALARM_AUTH_STANDARD, user8_partitions, CODE_P2);
    sim_start(&S, &cfg, NULL);
}

/* CORE-PART-001 */
static void test_partitions_are_independent(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);
    sim_fault(&S, Z_P2_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    TEST_ASSERT_FALSE(sim_status(&S, 2).ready);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    sim_restore(&S, Z_P2_WINDOW);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 2, CODE_P2, ALARM_LEVEL_STAY, 0));
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
}

/* CORE-PART-002: users only operate their partitions; masters operate all. */
static void test_partition_access(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_arm(&S, 1, CODE_P2, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_arm(&S, 2, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_EVENTS(&S, ALARM_EV_INVALID_CODE, 0); /* not counted toward lockout */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 2, CODE_MASTER, ALARM_LEVEL_STAY, 0));
}

/* CORE-PART-002 / CORE-USR-015: a button zone is its user's code in another shape, so it obeys
 * the same partition mask. A user restricted to partition 2 cannot arm or disarm partition 1
 * with an arm-stay, arm-away, disarm or keyswitch zone, and works normally in partition 2. */
static void test_button_zones_follow_partition_access(void)
{
    start_with_buttons(0x02); /* user 8: partition 2 only */

    /* Partition 1: the code is refused, and so is every button. */
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_arm(&S, 1, CODE_P2, ALARM_LEVEL_STAY, 0));
    press(Z_P1_STAY);
    press(Z_P1_AWAY);
    press(Z_P1_KEY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);

    /* Partition 2: the same user, the same kinds of button, normal behaviour. */
    press(Z_P2_STAY);
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 2, ALARM_LEVEL_STAY);
    press(Z_P2_OFF);
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);
    press(Z_P2_AWAY);
    EXPECT_LEVEL(&S, 2, ALARM_LEVEL_AWAY);
    press(Z_P2_OFF);
    press(Z_P2_KEY);
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    press(Z_P2_KEY);
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);

    /* An armed partition 1 stays armed: neither the disarm fob nor the keyswitch reaches it. */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    sim_advance(&S, SEC(61));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_disarm(&S, 1, CODE_P2));
    press(Z_P1_OFF);
    press(Z_P1_KEY);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
}

/* CORE-USR-015: the partition is checked on every press, not when the button was bound, so
 * granting and taking away a partition - or deleting the user - changes what the same physical
 * button does straight away. */
static void test_button_access_follows_user_changes(void)
{
    start_with_buttons(0x02);
    press(Z_P1_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* Add partition 1 to the user: the button that was dead now arms it. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_update(&S.eng, &S.keypad, CODE_MASTER, 8,
                                                     ALARM_AUTH_STANDARD, 0x03, NULL, S.now));
    press(Z_P1_AWAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    press(Z_P1_OFF);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* Take it away again: the same button goes dead at once, with no re-binding. */
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_update(&S.eng, &S.keypad, CODE_MASTER, 8,
                                                     ALARM_AUTH_STANDARD, 0x02, NULL, S.now));
    press(Z_P1_AWAY);
    press(Z_P1_STAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);

    /* Deleting the user takes their partition-2 buttons with it. */
    press(Z_P2_AWAY);
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    press(Z_P2_OFF);
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_delete(&S.eng, &S.keypad, CODE_MASTER, 8, S.now));
    press(Z_P2_AWAY);
    press(Z_P2_KEY);
    press(Z_P2_STAY);
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);
}

/* CORE-PART-002 / CORE-USR-015: the system master's buttons still reach every partition, and
 * the installer's keep the installer's rule: they never arm or disarm (CORE-USR-003, USR-001). */
static void test_master_and_installer_buttons_keep_their_reach(void)
{
    start_with_buttons(0x02);

    press(Z_P1_MKEY);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    press(Z_P2_MKEY);
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    press(Z_P1_MKEY);
    press(Z_P2_MKEY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED);

    sim_clear_records(&S);
    press(Z_P1_IAWAY); /* the installer's arm button does nothing */
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_EVENTS(&S, ALARM_EV_ARMED, 0);

    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    press(Z_P1_IOFF); /* nor does its disarm button, in exit time... */
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    sim_advance(&S, 120000u);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    press(Z_P1_IOFF); /* ...or armed */
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_EVENTS(&S, ALARM_EV_DISARMED, 0);
}

/* CORE-EXIT-013 / CORE-USR-015: the momentary-exit button needs no code, so it keeps working
 * whoever is bound to it, but it is only named after that user while the user may operate the
 * partition; otherwise the restart is the system's. */
static void test_momentary_exit_names_only_a_partition_user(void)
{
    start_with_buttons(0x02); /* user 8: partition 2 only */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    sim_advance(&S, SEC(61));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    press(Z_MOM_EXIT);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT8(0, sim_last_event(&S, ALARM_EV_EXIT_RESTART)->user);

    sim_advance(&S, SEC(61));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_update(&S.eng, &S.keypad, CODE_MASTER, 8,
                                                     ALARM_AUTH_STANDARD, 0x03, NULL, S.now));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    sim_advance(&S, SEC(61));
    press(Z_MOM_EXIT);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT8(8, sim_last_event(&S, ALARM_EV_EXIT_RESTART)->user);
}

/* CORE-PART-003 */
static void test_alarm_stays_in_its_partition(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 2, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    sim_advance(&S, SEC(61));
    sim_fault(&S, Z_P2_WINDOW);
    TEST_ASSERT_TRUE(sim_status(&S, 2).triggered);
    TEST_ASSERT_FALSE(sim_status(&S, 1).triggered);
    EXPECT_BELL(&S, 2, ALARM_PAT_STEADY);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    TEST_ASSERT_EQUAL_UINT8(2, sim_last_event(&S, ALARM_EV_ALARM)->partition);
}

/* CORE-PART-004: system troubles show on every partition. */
static void test_system_trouble_on_all_partitions(void)
{
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_AC_LOSS, true, S.now);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 2).troubles);
}

/* CORE-PART-005 */
static void test_disabled_partition_rejected(void)
{
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, sim_arm(&S, 3, CODE_MASTER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, sim_arm(&S, 0, CODE_MASTER, ALARM_LEVEL_STAY, 0));
}

/* CORE-PART-006: partition masters manage users of their own partition only. */
static void test_partition_master_scope(void)
{
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED,
                  alarm_engine_user_set(&S.eng, &S.keypad, CODE_P2_MGR, 10, "4444",
                                        ALARM_AUTH_STANDARD, 0x01, "x", S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_set(&S.eng, &S.keypad, CODE_P2_MGR, 10, "4444",
                                                  ALARM_AUTH_STANDARD, 0x02, "x", S.now));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, sim_arm(&S, 1, "4444", ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 2, "4444", ALARM_LEVEL_STAY, 0));
}

/* CORE-USR-023: an armed partition locks only its own users' codes. */
static void test_user_changes_follow_their_partition(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_RESULT(ALARM_OK, alarm_engine_user_set(&S.eng, &S.keypad, CODE_P2_MGR, 10, "4444",
                                                  ALARM_AUTH_STANDARD, 0x02, "x", S.now));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_user_set(&S.eng, &S.keypad, CODE_MASTER, 11, "4445",
                                        ALARM_AUTH_STANDARD, 0x03, "y", S.now));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 2, CODE_P2, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE,
                  alarm_engine_user_delete(&S.eng, &S.keypad, CODE_P2_MGR, 10, S.now));
}

/* CORE-PART-007 */
static void test_status_counts_per_partition(void)
{
    sim_fault(&S, Z_WINDOW);
    sim_fault(&S, Z_P2_WINDOW);
    sim_fault(&S, Z_P2_DOOR);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).faults);
    TEST_ASSERT_EQUAL_UINT16(2, sim_status(&S, 2).faults);
}

/* CORE-PART-008: one snapshot matches the individual status queries. */
static void test_snapshot_matches_queries(void)
{
    sim_fault(&S, Z_P2_WINDOW);
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_AC_LOSS, true, S.now);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    sim_advance(&S, SEC(5));

    static alarm_snapshot_t snap;
    snap.seq = 77u;
    snap.config_seq = 5u;
    alarm_engine_snapshot(&S.eng, &snap);

    TEST_ASSERT_EQUAL_UINT32(77u, snap.seq); /* owner fields untouched */
    TEST_ASSERT_EQUAL_UINT32(5u, snap.config_seq);
    TEST_ASSERT_EQUAL_UINT64(S.now, snap.taken_ms);
    TEST_ASSERT_EQUAL_UINT8(0x03u, snap.partitions);
    for (uint8_t p = 1; p <= 2; p++) {
        const alarm_partition_status_t st = sim_status(&S, p);
        TEST_ASSERT_EQUAL_INT(st.state, snap.part[p - 1u].state);
        TEST_ASSERT_EQUAL_INT(st.level, snap.part[p - 1u].level);
        TEST_ASSERT_EQUAL(st.ready, snap.part[p - 1u].ready);
        TEST_ASSERT_EQUAL_UINT64(st.delay_remaining_ms, snap.part[p - 1u].delay_remaining_ms);
        TEST_ASSERT_EQUAL_UINT16(st.faults, snap.part[p - 1u].faults);
        TEST_ASSERT_EQUAL_UINT16(st.troubles, snap.part[p - 1u].troubles);
        TEST_ASSERT_EQUAL_INT(st.sounder, snap.part[p - 1u].sounder);
    }
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, snap.part[0].state);
    TEST_ASSERT_EQUAL_UINT64(SEC(55), snap.part[0].delay_remaining_ms);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, snap.part[2].state); /* disabled: zeroed */
    TEST_ASSERT_EQUAL_UINT32(1u << ALARM_TRBL_AC_LOSS, snap.sys_troubles);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEFAULT_CODE_INSTALLER | ALARM_DEFAULT_CODE_MASTER,
                            snap.default_codes);
    TEST_ASSERT_EQUAL_UINT16(Z_P2_WINDOW, snap.zones_used);
    TEST_ASSERT_TRUE(snap.zone[Z_P2_WINDOW - 1u].flags & ALARM_ZS_FAULT);
    TEST_ASSERT_FALSE(snap.zone[Z_FRONT - 1u].flags & ALARM_ZS_FAULT);
    TEST_ASSERT_EQUAL_UINT16(0u, snap.zone[Z_P2_WINDOW].flags); /* unused zone */
    TEST_ASSERT_EQUAL_UINT32(sim_last_event(&S, ALARM_EV_ARMED)->seq, snap.last_event_seq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_partitions_are_independent);
    RUN_TEST(test_partition_access);
    RUN_TEST(test_button_zones_follow_partition_access);
    RUN_TEST(test_button_access_follows_user_changes);
    RUN_TEST(test_master_and_installer_buttons_keep_their_reach);
    RUN_TEST(test_momentary_exit_names_only_a_partition_user);
    RUN_TEST(test_alarm_stays_in_its_partition);
    RUN_TEST(test_system_trouble_on_all_partitions);
    RUN_TEST(test_disabled_partition_rejected);
    RUN_TEST(test_partition_master_scope);
    RUN_TEST(test_user_changes_follow_their_partition);
    RUN_TEST(test_status_counts_per_partition);
    RUN_TEST(test_snapshot_matches_queries);
    return UNITY_END();
}
