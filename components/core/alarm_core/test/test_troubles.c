/*
 * test_troubles.c - system/zone/device troubles, supervision, day/night and monitor zones.
 * Requirements: CORE-TRBL-*
 */
#include "sim.h"

static sim_t S;

void setUp(void)
{
    sim_start(&S, NULL, NULL);
}

void tearDown(void) {}

/* CORE-TRBL-001: AC loss is shown but does not beep (default). */
static void test_ac_loss(void)
{
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_AC_LOSS, true, S.now);
    alarm_partition_status_t st = sim_status(&S, 1);
    TEST_ASSERT_EQUAL_UINT16(1, st.troubles);
    TEST_ASSERT_FALSE(st.trouble_unack);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_TROUBLE);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_AC_LOSS, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_SYSTEM, ev->arg2);
    TEST_ASSERT_EQUAL_UINT32(1u << ALARM_TRBL_AC_LOSS, alarm_engine_system_troubles(&S.eng, NULL));

    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_AC_LOSS, false, S.now);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles); /* shown until code+OFF */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLES_ACKED, 1);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);
}

#define STORAGE_BIT (1u << ALARM_TRBL_STORAGE)
#define OWNER(o) (1u << (unsigned)(o))

/* CORE-TRBL-001, P4-013, ZW-039: the storage trouble stands while any owner holds it - one
 * owner's recovery (the configuration store) never clears another's failure (the Z-Wave node
 * cache) - and is restored, with exactly one restore event, when the last one clears. */
static void test_storage_trouble_owners(void)
{
    sim_clear_records(&S);
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_CONFIG, true, S.now);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_TROUBLE);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_STORAGE, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_SYSTEM, ev->arg2);
    TEST_ASSERT_EQUAL_UINT32(STORAGE_BIT, alarm_engine_system_troubles(&S.eng, NULL));

    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, true, S.now);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE, 1); /* already raised: no second event */
    TEST_ASSERT_EQUAL_UINT32(OWNER(ALARM_STORAGE_CONFIG) | OWNER(ALARM_STORAGE_ZWAVE),
                             alarm_engine_storage_owners(&S.eng));

    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_CONFIG, false, S.now); /* A recovers */
    TEST_ASSERT_EQUAL_UINT32(STORAGE_BIT, alarm_engine_system_troubles(&S.eng, NULL));
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE_RESTORE, 0);
    TEST_ASSERT_EQUAL_UINT32(OWNER(ALARM_STORAGE_ZWAVE), alarm_engine_storage_owners(&S.eng));

    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, false, S.now); /* B recovers */
    uint32_t restored = 0u;
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_system_troubles(&S.eng, &restored));
    TEST_ASSERT_EQUAL_UINT32(STORAGE_BIT, restored); /* shown until code + OFF */
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    ev = sim_last_report(&S, ALARM_EV_TROUBLE_RESTORE);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_STORAGE, ev->arg);
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_storage_owners(&S.eng));

    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, false, S.now); /* again: nothing */
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE, 1);
}

/* ZW-039, CORE-TRBL-001: a failing owner that says so on every pass raises the trouble once. */
static void test_storage_trouble_repeat_is_idempotent(void)
{
    sim_clear_records(&S);
    for (unsigned i = 0; i < 5u; i++) {
        alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, true, S.now);
        sim_advance(&S, 30000u);
    }
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE, 1);
    TEST_ASSERT_EQUAL_UINT32(OWNER(ALARM_STORAGE_ZWAVE), alarm_engine_storage_owners(&S.eng));
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, false, S.now);
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, false, S.now);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_system_troubles(&S.eng, NULL));
}

/* ZW-039, CORE-TRBL-001: an owner clearing what it never raised changes nothing - not while
 * another owner holds the trouble, not while none does; an unknown owner is refused. */
static void test_storage_trouble_foreign_clear(void)
{
    sim_clear_records(&S);
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_LOG, false, S.now); /* none held */
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_system_troubles(&S.eng, NULL));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 0);

    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZIGBEE, true, S.now);
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_LOG, false, S.now); /* not its hold */
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_OWNER_COUNT, false, S.now); /* bad */
    alarm_engine_storage_trouble(&S.eng, (alarm_storage_owner_t)31, false, S.now);
    TEST_ASSERT_EQUAL_UINT32(STORAGE_BIT, alarm_engine_system_troubles(&S.eng, NULL));
    TEST_ASSERT_EQUAL_UINT32(OWNER(ALARM_STORAGE_ZIGBEE), alarm_engine_storage_owners(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 0);

    /* the old system-trouble call is one more hold, so it can't clear Zigbee's either */
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_STORAGE, false, S.now);
    TEST_ASSERT_EQUAL_UINT32(STORAGE_BIT, alarm_engine_system_troubles(&S.eng, NULL));
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_STORAGE, true, S.now);
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZIGBEE, false, S.now);
    TEST_ASSERT_EQUAL_UINT32(STORAGE_BIT, alarm_engine_system_troubles(&S.eng, NULL));
    TEST_ASSERT_EQUAL_UINT32(OWNER(ALARM_STORAGE_OTHER), alarm_engine_storage_owners(&S.eng));
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_STORAGE, false, S.now);
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_system_troubles(&S.eng, NULL));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);

    /* runtime only: the persisted runtime state carries no hold, so a restart from it holds
     * nothing; the owner that still fails says so again on its next pass (ZW-039) */
    alarm_engine_storage_trouble(&S.eng, ALARM_STORAGE_ZWAVE, true, S.now);
    alarm_runtime_state_t rt;
    alarm_engine_runtime_state(&S.eng, &rt);
    sim_start(&S, NULL, &rt);
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_storage_owners(&S.eng));
    TEST_ASSERT_EQUAL_UINT32(0u, alarm_engine_system_troubles(&S.eng, NULL));
}

/* CORE-TRBL-002: beeping troubles are silenced by the OFF key; display stays. */
static void test_comm_fail_beeps_until_silenced(void)
{
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_COMM_FAIL, true, S.now);
    TEST_ASSERT_TRUE(sim_status(&S, 1).trouble_unack);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_TROUBLE);
    EXPECT_RESULT(ALARM_OK, sim_simple(&S, ALARM_CMD_SILENCE, 1, ""));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_NONE);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
}

/* CORE-TRBL-003 */
static void test_zone_loop_trouble(void)
{
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TROUBLE);
    TEST_ASSERT_FALSE(sim_status(&S, 1).ready);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_TROUBLE);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));

    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TROUBLE_RESTORE);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TROUBLE);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TROUBLE_MEM);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TROUBLE_MEM);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);
}

/* CORE-TRBL-004 */
static void test_tamper_disarmed_is_trouble(void)
{
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TAMPER);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TAMPER);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    TEST_ASSERT_FALSE(sim_status(&S, 1).ready);
}

/* CORE-TRBL-005 */
static void test_tamper_armed_is_alarm(void)
{
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_TAMPER);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_WINDOW, ALARM_CAUSE_TAMPER));
    EXPECT_BELL(&S, 1, ALARM_PAT_STEADY);
}

/* CORE-TRBL-006 */
static void test_low_battery_does_not_block(void)
{
    sim_zev(&S, Z_FRONT, ALARM_ZEV_LOW_BATTERY);
    EXPECT_ZONE_FLAG(&S, Z_FRONT, ALARM_ZS_LOW_BATTERY);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    TEST_ASSERT_TRUE(sim_status(&S, 1).trouble_unack);
}

/* CORE-TRBL-007: missed check-in raises a supervision (CHECK) trouble. */
static void test_zone_supervision(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_WINDOW - 1].supervision_s = 3600;
    sim_start(&S, &cfg, NULL);
    sim_advance(&S, SEC(3599));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    sim_advance(&S, SEC(1));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZTRBL_SUPERVISION, sim_last_event(&S, ALARM_EV_TROUBLE)->arg);
    TEST_ASSERT_FALSE(sim_status(&S, 1).ready);
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_CHECKIN);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
}

/* CORE-TRBL-013: a source that can't reach a sensor makes the zone CHECK at once (no waiting for
 * supervision_s); it blocks arming and beeps like any CHECK. REACHABLE clears it; the restored
 * trouble stays shown until code + OFF. */
static void test_unreachable_sensor_is_check_at_once(void)
{
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_UNREACHABLE);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_FAULT);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_TROUBLE);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZTRBL_SUPERVISION, ev->arg);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_ZONE, ev->arg2);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, ev->zone);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE, 1);
    TEST_ASSERT_FALSE(sim_status(&S, 1).ready);
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_TROUBLE);
    EXPECT_RESULT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));

    sim_zev(&S, Z_WINDOW, ALARM_ZEV_UNREACHABLE); /* repeated: nothing new */
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);

    sim_zev(&S, Z_WINDOW, ALARM_ZEV_REACHABLE);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TROUBLE_MEM);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_TROUBLE_MEM);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);
}

/* CORE-TRBL-014: an unreachable sensor is a trouble, never an alarm, armed or not, burglary or
 * fire; the zone keeps its last state and still reacts to its inputs (another input of the zone,
 * such as a separate battery sensor, may report meanwhile), but only REACHABLE clears the CHECK;
 * the supervision timer doesn't report it twice and restarts once the sensor is back. */
static void test_unreachable_is_trouble_not_alarm(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.zones[Z_WINDOW - 1].supervision_s = 3600;
    sim_start(&S, &cfg, NULL);
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_clear_records(&S);

    sim_zev(&S, Z_WINDOW, ALARM_ZEV_UNREACHABLE);
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_UNREACHABLE);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 2);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    EXPECT_BELL(&S, 1, ALARM_PAT_OFF);
    EXPECT_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_SUPERVISION);

    sim_advance(&S, SEC(7200)); /* past supervision_s: no second report */
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 2);

    sim_zev(&S, Z_SMOKE, ALARM_ZEV_LOW_BATTERY); /* another input of the zone: CHECK stays */
    EXPECT_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_SUPERVISION);
    EXPECT_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_LOW_BATTERY);
    sim_fault(&S, Z_SMOKE); /* a fault still counts */
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_SMOKE, ALARM_CAUSE_FIRE));
    EXPECT_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_SUPERVISION);
    sim_zev(&S, Z_SMOKE, ALARM_ZEV_REACHABLE);
    EXPECT_NO_ZONE_FLAG(&S, Z_SMOKE, ALARM_ZS_SUPERVISION);

    sim_zev(&S, Z_WINDOW, ALARM_ZEV_REACHABLE);
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 1); /* only the fire */
    sim_advance(&S, SEC(3599));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    sim_advance(&S, SEC(1)); /* the timer restarted when the sensor came back */
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
}

/* CORE-TRBL-008: supervised devices (keypads, expanders). */
static void test_device_supervision(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.devices[0].enabled = true;
    cfg.devices[0].partition = 1;
    cfg.devices[0].supervision_s = 60;
    sim_start(&S, &cfg, NULL);
    sim_advance(&S, SEC(59));
    TEST_ASSERT_EQUAL_UINT32(0, alarm_engine_device_troubles(&S.eng));
    sim_advance(&S, SEC(1));
    TEST_ASSERT_EQUAL_UINT32(1, alarm_engine_device_troubles(&S.eng));
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_TROUBLE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_DEVICE, ev->arg2);
    TEST_ASSERT_EQUAL_UINT16(1, ev->zone);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
    alarm_engine_device_checkin(&S.eng, 1, S.now);
    TEST_ASSERT_EQUAL_UINT32(0, alarm_engine_device_troubles(&S.eng));
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);
}

/* CORE-TRBL-008, ZW-038: the platform says a device (a Z-Wave keypad, number 9-16) can't be
 * reached: the same device supervision trouble at once, with no supervision_s configured; heard
 * again, one restore. Repeats are no news. */
static void test_device_unreachable_without_supervision(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.devices[12].supervision_s);
    sim_start(&S, &cfg, NULL);
    alarm_engine_device_reachable(&S.eng, 13, false, S.now);
    TEST_ASSERT_EQUAL_UINT32(1u << 12, alarm_engine_device_troubles(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE, 1);
    const alarm_event_t *ev = sim_last_report(&S, ALARM_EV_TROUBLE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_DEVICE, ev->arg2);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_DEVICE_SUPERVISION, ev->arg);
    TEST_ASSERT_EQUAL_UINT16(13, ev->zone);
    TEST_ASSERT_TRUE(sim_status(&S, 1).trouble_unack);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);

    alarm_engine_device_reachable(&S.eng, 13, false, S.now); /* idempotent */
    sim_advance(&S, SEC(3600));                              /* no supervision clock runs */
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    TEST_ASSERT_EQUAL_UINT32(1u << 12, alarm_engine_device_troubles(&S.eng));

    alarm_engine_device_reachable(&S.eng, 13, true, S.now);
    TEST_ASSERT_EQUAL_UINT32(0, alarm_engine_device_troubles(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    EXPECT_REPORTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    ev = sim_last_report(&S, ALARM_EV_TROUBLE_RESTORE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_DEVICE, ev->arg2);
    TEST_ASSERT_EQUAL_UINT16(13, ev->zone);
    alarm_engine_device_reachable(&S.eng, 13, true, S.now); /* idempotent */
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); /* the memory is acknowledged */
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);

    /* bad device numbers are ignored */
    alarm_engine_device_reachable(&S.eng, 0, false, S.now);
    alarm_engine_device_reachable(&S.eng, ALARM_MAX_DEVICES + 1u, false, S.now);
    alarm_engine_device_reachable(NULL, 13, false, S.now);
    TEST_ASSERT_EQUAL_UINT32(0, alarm_engine_device_troubles(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
}

/* CORE-TRBL-008, ZW-038: saving the programming while a Z-Wave keypad is unreachable keeps its
 * trouble (no clock would raise it again); it is restored once when the keypad is heard. A
 * device on a check-in timer still starts again clean. */
static void test_device_unreachable_survives_config_save(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.devices[0].enabled = true;
    cfg.devices[0].partition = 1;
    cfg.devices[0].supervision_s = 60;
    sim_start(&S, &cfg, NULL);
    sim_advance(&S, SEC(60));
    alarm_engine_device_reachable(&S.eng, 13, false, S.now);
    TEST_ASSERT_EQUAL_UINT32((1u << 12) | 1u, alarm_engine_device_troubles(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 2);

    TEST_ASSERT_EQUAL(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_EQUAL_UINT32(1u << 12, alarm_engine_device_troubles(&S.eng));
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 2);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 0);

    /* supervision turned off on the timed device: its trouble clears too, the keypad's stays */
    cfg.devices[0].supervision_s = 0;
    sim_advance(&S, SEC(60));
    TEST_ASSERT_EQUAL_UINT32((1u << 12) | 1u, alarm_engine_device_troubles(&S.eng));
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_EQUAL_UINT32(1u << 12, alarm_engine_device_troubles(&S.eng));

    alarm_engine_device_reachable(&S.eng, 13, true, S.now);
    TEST_ASSERT_EQUAL_UINT32(0, alarm_engine_device_troubles(&S.eng));
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
}

/* CORE-TRBL-008, ZW-038: a supervised device's missed check-in and the platform's "unreachable"
 * are one trouble: either raises it once, a check-in or "reachable" restores it once. */
static void test_device_unreachable_with_supervision(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.devices[0].enabled = true;
    cfg.devices[0].partition = 1;
    cfg.devices[0].supervision_s = 60;
    sim_start(&S, &cfg, NULL);
    alarm_engine_device_reachable(&S.eng, 1, false, S.now);
    sim_advance(&S, SEC(120)); /* the missed check-in adds nothing */
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    alarm_engine_device_checkin(&S.eng, 1, S.now);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    alarm_engine_device_reachable(&S.eng, 1, true, S.now);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT32(0, alarm_engine_device_troubles(&S.eng));
}

/* CORE-TRBL-008, LINK-007: a link keypad's home wins over a stale device-table
 * assignment. Restore and remembered trouble retain the original scope. */
static void test_keypad_supervision_home_partition(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    cfg.devices[0].enabled = false;
    cfg.devices[0].partition = 1;
    sim_start(&S, &cfg, NULL);
    alarm_engine_device_reachable_at(&S.eng, 1, 2, false, S.now);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 2).troubles);
    TEST_ASSERT_FALSE(sim_status(&S, 1).trouble_unack);
    TEST_ASSERT_TRUE(sim_status(&S, 2).trouble_unack);
    TEST_ASSERT_EQUAL_UINT8(2, sim_last_event(&S, ALARM_EV_TROUBLE)->partition);
    alarm_engine_device_reachable_at(&S.eng, 1, 2, false, S.now);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE, 1);
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 2).troubles);
    /* Reassignment must not put the restore on a different partition. */
    alarm_engine_device_reachable_at(&S.eng, 1, 1, true, S.now);
    TEST_ASSERT_EQUAL_UINT8(2, sim_last_event(&S, ALARM_EV_TROUBLE_RESTORE)->partition);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 1).troubles);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 2).troubles);
    alarm_engine_device_reachable_at(&S.eng, 1, 1, false, S.now);
    TEST_ASSERT_EQUAL_UINT8(1, sim_last_event(&S, ALARM_EV_TROUBLE)->partition);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
    TEST_ASSERT_EQUAL_UINT16(0, sim_status(&S, 2).troubles);
    alarm_engine_device_reachable_at(&S.eng, 1, ALARM_MAX_PARTITIONS + 1u, true, S.now);
    TEST_ASSERT_EQUAL_UINT32(1, alarm_engine_device_troubles(&S.eng));
}

/* CORE-TRBL-008: an unused legacy slot must not scope a radio keypad to an old partition. */
static void test_disabled_device_scope_is_not_reused(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    cfg.devices[0].partition = 2;
    sim_start(&S, &cfg, NULL);
    alarm_engine_device_reachable(&S.eng, 1, false, S.now);
    TEST_ASSERT_EQUAL_UINT8(0, sim_last_event(&S, ALARM_EV_TROUBLE)->partition);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 1).troubles);
    TEST_ASSERT_EQUAL_UINT16(1, sim_status(&S, 2).troubles);
}

/* CORE-TRBL-009: trouble by day / alarm by night (type 05). */
static void test_day_night_zone(void)
{
    sim_fault(&S, Z_CABINET);
    EXPECT_ZONE_FLAG(&S, Z_CABINET, ALARM_ZS_TROUBLE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZTRBL_DAY_FAULT, sim_last_event(&S, ALARM_EV_TROUBLE)->arg);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_restore(&S, Z_CABINET);
    EXPECT_EVENTS(&S, ALARM_EV_TROUBLE_RESTORE, 1);

    sim_arm_stay_settled(&S, CODE_USER);
    sim_fault(&S, Z_CABINET); /* bypassed in stay */
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_restore(&S, Z_CABINET);
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));

    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_CABINET);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_CABINET, ALARM_CAUSE_BURGLARY));
}

/* CORE-TRBL-010: monitor zones report but never alarm or block arming. */
static void test_monitor_zone(void)
{
    sim_fault(&S, Z_FREEZER);
    EXPECT_REPORTS(&S, ALARM_EV_MONITOR_FAULT, 1);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
    sim_restore(&S, Z_FREEZER);
    EXPECT_REPORTS(&S, ALARM_EV_MONITOR_RESTORE, 1);
}

/* CORE-TRBL-011: exit beeps take priority over trouble beeps. */
static void test_trouble_beeps_below_exit(void)
{
    alarm_engine_system_trouble(&S.eng, ALARM_TRBL_COMM_FAIL, true, S.now);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    EXPECT_SOUNDER(&S, 1, ALARM_SOUND_EXIT);
}

/* CORE-TRBL-012: no-response zones never alarm or block arming. */
static void test_no_response_zone(void)
{
    sim_fault(&S, Z_DOORBELL);
    TEST_ASSERT_TRUE(sim_status(&S, 1).ready);
    sim_arm_away_and_leave(&S, CODE_USER);
    sim_fault(&S, Z_DOORBELL);
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ac_loss);
    RUN_TEST(test_comm_fail_beeps_until_silenced);
    RUN_TEST(test_storage_trouble_owners);
    RUN_TEST(test_storage_trouble_repeat_is_idempotent);
    RUN_TEST(test_storage_trouble_foreign_clear);
    RUN_TEST(test_zone_loop_trouble);
    RUN_TEST(test_tamper_disarmed_is_trouble);
    RUN_TEST(test_tamper_armed_is_alarm);
    RUN_TEST(test_low_battery_does_not_block);
    RUN_TEST(test_zone_supervision);
    RUN_TEST(test_device_supervision);
    RUN_TEST(test_keypad_supervision_home_partition);
    RUN_TEST(test_disabled_device_scope_is_not_reused);
    RUN_TEST(test_device_unreachable_without_supervision);
    RUN_TEST(test_device_unreachable_with_supervision);
    RUN_TEST(test_device_unreachable_survives_config_save);
    RUN_TEST(test_day_night_zone);
    RUN_TEST(test_monitor_zone);
    RUN_TEST(test_trouble_beeps_below_exit);
    RUN_TEST(test_no_response_zone);
    RUN_TEST(test_unreachable_sensor_is_check_at_once);
    RUN_TEST(test_unreachable_is_trouble_not_alarm);
    return UNITY_END();
}
