/*
 * test_persistence.c - power up in previous state, settle period, event sequence.
 * Requirements: CORE-PWR-*
 */
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

static void bm_set_zone(uint8_t *bm, uint16_t z)
{
    bm[(z - 1u) / 8u] = (uint8_t)(bm[(z - 1u) / 8u] | (1u << ((z - 1u) % 8u)));
}

static alarm_runtime_state_t armed_away_state(void)
{
    alarm_runtime_state_t rs;
    memset(&rs, 0, sizeof(rs));
    rs.version = ALARM_RUNTIME_VERSION;
    rs.part[0].state = ALARM_STATE_ARMED;
    rs.part[0].level = ALARM_LEVEL_AWAY;
    rs.part[0].armed_by = 3;
    rs.event_seq = 100;
    return rs;
}

/* CORE-PWR-001: arming state is persisted (exit time stored as armed). */
static void test_persist_on_arm(void)
{
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    TEST_ASSERT_TRUE(S.n_persist >= 1u);
    TEST_ASSERT_EQUAL_UINT8(ALARM_STATE_ARMED, S.persisted.part[0].state);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_AWAY, S.persisted.part[0].level);
    TEST_ASSERT_EQUAL_UINT8(3, S.persisted.part[0].armed_by);
    TEST_ASSERT_EQUAL_UINT16(ALARM_RUNTIME_VERSION, S.persisted.version);
}

/* CORE-PWR-002: power up armed, no exit delay; event numbering continues. */
static void test_restore_armed_state(void)
{
    const alarm_runtime_state_t rs = armed_away_state();
    sim_start(&S, NULL, &rs);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    TEST_ASSERT_EQUAL_UINT8(3, sim_status(&S, 1).armed_by);
    TEST_ASSERT_EQUAL_UINT32(101, sim_last_event(&S, ALARM_EV_SYSTEM_START)->seq);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
}

/* PWR-007: init_ex records why the panel started as system_start's arg; plain init records 0. */
static void test_start_reason_recorded(void)
{
    TEST_ASSERT_EQUAL_UINT8(ALARM_START_UNKNOWN, sim_last_event(&S, ALARM_EV_SYSTEM_START)->arg);
    const alarm_ports_t ports = S.eng.ports;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, alarm_engine_init_ex(&S.eng, &S.cfg, &ports, NULL, S.now,
                                                         ALARM_START_WATCHDOG));
    TEST_ASSERT_EQUAL_UINT8(ALARM_START_WATCHDOG,
                            sim_last_event(&S, ALARM_EV_SYSTEM_START)->arg);
}

/* CORE-PWR-003 */
static void test_restore_disabled_by_option(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.power_up_previous_state = false;
    const alarm_runtime_state_t rs = armed_away_state();
    sim_start(&S, &cfg, &rs);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* CORE-PWR-004: bypasses, alarm memory and chime survive a restart. */
static void test_restore_bypass_memory_chime(void)
{
    alarm_runtime_state_t rs;
    memset(&rs, 0, sizeof(rs));
    rs.version = ALARM_RUNTIME_VERSION;
    rs.part[0].chime = true;
    rs.part[0].memory = (uint16_t)(1u << ALARM_CAUSE_BURGLARY);
    bm_set_zone(rs.bypassed, Z_WINDOW);
    bm_set_zone(rs.in_memory, Z_HALL_PIR);
    sim_start(&S, NULL, &rs);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_TRUE(st.chime);
    TEST_ASSERT_TRUE((st.memory & (1u << ALARM_CAUSE_BURGLARY)) != 0u);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_HALL_PIR, ALARM_ZS_ALARM);
}

/* CORE-PWR-005: zone alarms are held off during the power-up settle time. */
static void test_settle_period(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.opt.power_up_settle_s = 60;
    const alarm_runtime_state_t rs = armed_away_state();
    sim_start(&S, &cfg, &rs);
    sim_fault(&S, Z_FRONT);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).faults);
    sim_advance(&S, SEC(59));
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    sim_advance(&S, SEC(1));
    EXPECT_STATE(&S, 1, ALARM_STATE_ENTRY_DELAY);
}

/* CORE-PWR-006: runtime state round trip. */
static void test_runtime_state_round_trip(void)
{
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_WINDOW));
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_CHIME, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    alarm_runtime_state_t rs;
    alarm_engine_runtime_state(&S.eng, &rs);
    TEST_ASSERT_EQUAL_MEMORY(&rs, &S.persisted, sizeof(rs));

    sim_start(&S, NULL, &rs);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    TEST_ASSERT_TRUE(sim_status(&S, 1).chime);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
}

/* CORE-PWR-007: an unknown runtime version is ignored. */
static void test_unknown_runtime_version_ignored(void)
{
    alarm_runtime_state_t rs = armed_away_state();
    rs.version = 999;
    sim_start(&S, NULL, &rs);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_persist_on_arm);
    RUN_TEST(test_restore_armed_state);
    RUN_TEST(test_restore_disabled_by_option);
    RUN_TEST(test_restore_bypass_memory_chime);
    RUN_TEST(test_start_reason_recorded);
    RUN_TEST(test_settle_period);
    RUN_TEST(test_runtime_state_round_trip);
    RUN_TEST(test_unknown_runtime_version_ignored);
    return UNITY_END();
}
