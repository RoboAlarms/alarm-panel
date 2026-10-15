/*
 * test_alarms.c - bell timeout, abort/cancel windows, alarm memory, swinger,
 * cross zoning, silent burglary, report ordering.
 * Requirements: CORE-ALM-*
 */
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

/* CORE-ALM-001 */
static void test_bell_timeout(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_STEADY, sim_output(&S, ALARM_OUT_BELL, 1));
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_ON, sim_output(&S, ALARM_OUT_STROBE, 1));
    sim_advance(&S, SEC(239));
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
    sim_advance(&S, SEC(1));
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_OFF, sim_output(&S, ALARM_OUT_BELL, 1));
    EXPECT_EVENTS(&S, ALARM_EV_BELL_TIMEOUT, 1);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_TRUE(st.triggered); /* still armed with an alarm this period */
    TEST_ASSERT_TRUE((st.memory & (1u << ALARM_CAUSE_BURGLARY)) != 0u);
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_ON, sim_output(&S, ALARM_OUT_STROBE, 1));
}

/* CORE-ALM-002: disarm inside the abort window -> nothing is reported. */
static void test_abort_window_suppresses_report(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(10));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 1);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM_ABORTED, 1);
    sim_advance(&S, SEC(600));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_REPORTS(&S, ALARM_EV_RECENT_CLOSING, 0);
    EXPECT_REPORTS(&S, ALARM_EV_ALARM_CANCELED, 0);
    const alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_TRUE((st.memory & ALARM_MEM_CANCELED) != 0u);
    TEST_ASSERT_TRUE((st.memory & (1u << ALARM_CAUSE_BURGLARY)) != 0u);
}

/* CORE-ALM-003: disarm inside the cancel window after a report -> cancel report. */
static void test_cancel_window_sends_cancel(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(31));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM, 1);
    sim_advance(&S, SEC(30));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM_CANCELED, 1);
}

/* CORE-ALM-004 */
static void test_no_cancel_after_cancel_window(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(31));
    sim_advance(&S, SEC(300));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_REPORTS(&S, ALARM_EV_ALARM_CANCELED, 0);
}

/* CORE-ALM-005: alarm memory stays until a second code + OFF. */
static void test_alarm_memory_needs_second_code_off(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    sim_restore(&S, Z_WINDOW);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_TRUE((st.memory & (1u << ALARM_CAUSE_BURGLARY)) != 0u);
    TEST_ASSERT_EQUAL_UINT16(1, st.alarms);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_ALARM);
    TEST_ASSERT_FALSE(st.triggered);

    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    st = sim_status(&S, 1);
    EXPECT_EVENTS(&S, ALARM_EV_MEMORY_CLEARED, 1);
    TEST_ASSERT_EQUAL_UINT16(0, st.memory);
    TEST_ASSERT_EQUAL_UINT16(0, st.alarms);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_ALARM);
}

/* CORE-ALM-006: swinger shutdown after N trips per armed period. */
static void test_swinger_shutdown(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    for (int i = 0; i < 3; i++) {
        sim_fault(&S, Z_WINDOW);
        sim_advance(&S, SEC(1));
        sim_restore(&S, Z_WINDOW);
        sim_advance(&S, SEC(1));
    }
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 2);
    EXPECT_EVENTS(&S, ALARM_EV_SWINGER_SHUTDOWN, 1);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SWINGER);
    sim_fault(&S, Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 2);
    sim_restore(&S, Z_WINDOW);

    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SWINGER);
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 3);
}

/* CORE-ALM-007 */
static void test_swinger_exempt_zone(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_WINDOW - 1].flags |= ALARM_ZF_SWINGER_EXEMPT;
    sim_start(&S, &cfg, NULL);
    sim_arm_away_and_leave(&S, CODE_USER);
    for (int i = 0; i < 4; i++) {
        sim_fault(&S, Z_WINDOW);
        sim_restore(&S, Z_WINDOW);
    }
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 4);
}

static void start_cross_zoned(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_set_zone(&cfg, 21, ALARM_ZT_PERIMETER, 1);
    cfg.zones[Z_WINDOW - 1].cross_zone = 21;
    cfg.zones[21 - 1].cross_zone = Z_WINDOW;
    sim_start(&S, &cfg, NULL);
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
}

/* CORE-ALM-008: cross zoning needs both zones inside the window. */
static void test_cross_zone_requires_partner(void)
{
    start_cross_zoned();
    sim_fault(&S, Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_advance(&S, SEC(60));
    sim_fault(&S, 21);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, 21, ALARM_CAUSE_BURGLARY));
}

/* CORE-ALM-008: a pair is a pair whichever side names the other. The zone editor sets the
 * partner of the zone being edited only, so a zone named by another must not alarm alone, and
 * the one that names it must alarm when its partner confirms it. */
static void start_one_sided(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_set_zone(&cfg, 21, ALARM_ZT_PERIMETER, 1);
    cfg.zones[Z_WINDOW - 1].cross_zone = 21; /* zone 21 names nobody */
    sim_start(&S, &cfg, NULL);
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
}

static void test_cross_zone_named_side_needs_partner(void)
{
    start_one_sided();
    sim_fault(&S, 21);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_advance(&S, SEC(60));
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, 21, ALARM_CAUSE_BURGLARY));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
}

static void test_cross_zone_naming_side_confirmed_by_partner(void)
{
    start_one_sided();
    sim_fault(&S, Z_WINDOW);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_advance(&S, SEC(60));
    sim_fault(&S, 21);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, 21, ALARM_CAUSE_BURGLARY));
}

/* CORE-ALM-008: a zone whose partner can't trip now (bypassed, or not armed in this level) acts
 * as a normal zone - it must never wait for a partner that cannot come. */
static void test_cross_zone_partner_bypassed_acts_alone(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_set_zone(&cfg, 21, ALARM_ZT_PERIMETER, 1);
    cfg.zones[Z_WINDOW - 1].cross_zone = 21;
    cfg.zones[21 - 1].cross_zone = Z_WINDOW;
    sim_start(&S, &cfg, NULL);
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, 21));
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
}

static void test_cross_zone_partner_not_armed_acts_alone(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_WINDOW - 1].cross_zone = Z_HALL_PIR; /* interior: not armed in Stay */
    sim_start(&S, &cfg, NULL);
    sim_arm_stay_settled(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_BURGLARY));
}

/* CORE-ALM-009 */
static void test_cross_zone_window_expires(void)
{
    start_cross_zoned();
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(301));
    sim_fault(&S, 21);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

/* CORE-ALM-010: silent burglary never sounds and is hidden until disarm. */
static void test_silent_burglary(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_SHED);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ALARM);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_TRUE((ev->flags & ALARM_EVF_SILENT) != 0u);
    alarm_partition_status_t st = sim_status(&S, 1);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    TEST_ASSERT_FALSE(st.triggered);
    TEST_ASSERT_EQUAL_UINT16(0, st.memory);
    EXPECT_NO_ZONE_FLAG(&S, Z_SHED, ALARM_ZS_ALARM);
    sim_advance(&S, SEC(30));
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_SHED, ALARM_CAUSE_SILENT_BURGLARY));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    st = sim_status(&S, 1);
    TEST_ASSERT_TRUE((st.memory & (1u << ALARM_CAUSE_SILENT_BURGLARY)) != 0u);
    EXPECT_ZONE_FLAG(&S, Z_SHED, ALARM_ZS_ALARM);
}

/* CORE-ALM-011: restore is reported once the alarm was reported. */
static void test_alarm_restore_reported_after_report(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(31));
    sim_restore(&S, Z_WINDOW);
    EXPECT_REPORTS(&S, ALARM_EV_ALARM_RESTORE, 1);
}

/* CORE-ALM-012 */
static void test_strobe_until_disarm(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(241));
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_ON, sim_output(&S, ALARM_OUT_STROBE, 1));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    TEST_ASSERT_EQUAL_INT(ALARM_PAT_OFF, sim_output(&S, ALARM_OUT_STROBE, 1));
}

/* CORE-ALM-013: held reports are released in order. */
static void test_reports_released_in_order(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(5));
    sim_fault(&S, Z_HALL_PIR); /* interior tripped with no entry time running */
    sim_advance(&S, SEC(25));
    int first = -1;
    int second = -1;
    for (unsigned i = 0; i < S.n_reports; i++) {
        if (S.reports[i].type == ALARM_EV_ALARM && S.reports[i].zone == Z_WINDOW) {
            first = (int)i;
        }
        if (S.reports[i].type == ALARM_EV_ALARM && S.reports[i].zone == Z_HALL_PIR) {
            second = (int)i;
        }
    }
    TEST_ASSERT_TRUE(first >= 0 && second >= 0);
    TEST_ASSERT_TRUE(first < second);
}

/* CORE-ALM-014: after the first report, later alarms go out immediately. */
static void test_subsequent_alarm_reported_immediately(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);
    sim_fault(&S, Z_WINDOW);
    sim_advance(&S, SEC(31));
    sim_fault(&S, Z_HALL_PIR);
    TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_HALL_PIR, ALARM_CAUSE_BURGLARY));
}

/* CORE-ALM-015 */
static void test_disarm_clears_triggered(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_TRUE(sim_status(&S, 1).triggered);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    TEST_ASSERT_FALSE(sim_status(&S, 1).triggered);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bell_timeout);
    RUN_TEST(test_abort_window_suppresses_report);
    RUN_TEST(test_cancel_window_sends_cancel);
    RUN_TEST(test_no_cancel_after_cancel_window);
    RUN_TEST(test_alarm_memory_needs_second_code_off);
    RUN_TEST(test_swinger_shutdown);
    RUN_TEST(test_swinger_exempt_zone);
    RUN_TEST(test_cross_zone_requires_partner);
    RUN_TEST(test_cross_zone_named_side_needs_partner);
    RUN_TEST(test_cross_zone_naming_side_confirmed_by_partner);
    RUN_TEST(test_cross_zone_partner_bypassed_acts_alone);
    RUN_TEST(test_cross_zone_partner_not_armed_acts_alone);
    RUN_TEST(test_cross_zone_window_expires);
    RUN_TEST(test_silent_burglary);
    RUN_TEST(test_alarm_restore_reported_after_report);
    RUN_TEST(test_strobe_until_disarm);
    RUN_TEST(test_reports_released_in_order);
    RUN_TEST(test_subsequent_alarm_reported_immediately);
    RUN_TEST(test_disarm_clears_triggered);
    return UNITY_END();
}
