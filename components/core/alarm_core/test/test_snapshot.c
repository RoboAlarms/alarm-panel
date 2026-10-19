/*
 * test_snapshot.c - the Troubles list walker shared by the touchscreen and the Panel Manager.
 * Requirements: UI-030 (docs/features/07), BL-137
 */
#include "sim.h"

#include <stdlib.h>

#include "alarm/alarm_snapshot.h"

static alarm_config_t cfg;
static alarm_snapshot_t snap;

void setUp(void)
{
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_set_zone(&cfg, 30, ALARM_ZT_PERIMETER, 2);
    memset(&snap, 0, sizeof(snap));
    snap.partitions = 3u;
    snap.zones_used = 30;
}

void tearDown(void) {}

/* UI-030: each family, active before restored, in a steady order. */
static void test_families_and_order(void)
{
    snap.sys_troubles = (1u << ALARM_TRBL_AC_LOSS);
    snap.sys_troubles_restored = (1u << ALARM_TRBL_COMM_FAIL) | (1u << ALARM_TRBL_AC_LOSS);
    snap.dev_troubles = 1u << 1; /* keypad 2 */
    snap.zone[2].trouble = (1u << ALARM_ZTRBL_LOW_BATTERY) | (1u << ALARM_ZTRBL_TAMPER);
    snap.zone[3].flags = ALARM_ZS_TROUBLE_MEM;
    alarm_trouble_item_t it[8];
    TEST_ASSERT_EQUAL_UINT32(6, alarm_snapshot_troubles(&snap, &cfg, 0xFFu, it, 8));
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_SYSTEM, it[0].family);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_AC_LOSS, it[0].kind);
    TEST_ASSERT_EQUAL_UINT16(0, it[0].number);
    TEST_ASSERT_FALSE(it[0].restored);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_DEVICE, it[1].family);
    TEST_ASSERT_EQUAL_UINT16(2, it[1].number);
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_FAMILY_ZONE, it[2].family);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZTRBL_TAMPER, it[2].kind);
    TEST_ASSERT_EQUAL_UINT16(3, it[2].number);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZTRBL_LOW_BATTERY, it[3].kind);
    /* active AC loss is not also listed as restored */
    TEST_ASSERT_EQUAL_UINT8(ALARM_TRBL_COMM_FAIL, it[4].kind);
    TEST_ASSERT_TRUE(it[4].restored);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ZTRBL_LOOP, it[5].kind);
    TEST_ASSERT_EQUAL_UINT16(4, it[5].number);
    TEST_ASSERT_TRUE(it[5].restored);
}

/* UI-030: unused zones are skipped; an active zone trouble hides its restored memory. */
static void test_unused_and_active_over_restored(void)
{
    sim_set_zone(&cfg, 5, ALARM_ZT_NOT_USED, 1);
    snap.zone[4].trouble = 1u << ALARM_ZTRBL_LOOP;
    snap.zone[0].trouble = 1u << ALARM_ZTRBL_LOOP;
    snap.zone[0].flags = ALARM_ZS_TROUBLE_MEM;
    TEST_ASSERT_EQUAL_UINT32(1, alarm_snapshot_troubles(&snap, &cfg, 0xFFu, NULL, 0));
}

/* BL-137: the viewer's partitions filter zones, never the system or a keypad whose partition
 * isn't known (dev_partition 0). */
static void test_partition_filter(void)
{
    snap.sys_troubles = 1u << ALARM_TRBL_AC_LOSS;
    snap.dev_troubles = 1u;
    snap.zone[29].trouble = 1u << ALARM_ZTRBL_TAMPER; /* zone 30, partition 2 */
    snap.zone[0].trouble = 1u << ALARM_ZTRBL_TAMPER;  /* zone 1, partition 1 */
    TEST_ASSERT_EQUAL_UINT32(4, alarm_snapshot_troubles(&snap, &cfg, 0x03u, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(3, alarm_snapshot_troubles(&snap, &cfg, 0x01u, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(3, alarm_snapshot_troubles(&snap, &cfg, 0x02u, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(2, alarm_snapshot_troubles(&snap, &cfg, 0x00u, NULL, 0));
}

/* LINK-007, CORE-TRBL-008: a link keypad's trouble is listed on its home partition only. */
static void test_keypad_trouble_listed_on_its_home(void)
{
    snap.dev_troubles = (1u << 0) | (1u << 1);
    snap.dev_partition[0] = 2u; /* keypad 1: partition 2 */
    snap.dev_partition[1] = 0u; /* keypad 2: every partition */
    alarm_trouble_item_t it[4];
    TEST_ASSERT_EQUAL_UINT32(2, alarm_snapshot_troubles(&snap, &cfg, 0x03u, it, 4));
    TEST_ASSERT_EQUAL_UINT32(1, alarm_snapshot_troubles(&snap, &cfg, 0x01u, it, 4));
    TEST_ASSERT_EQUAL_UINT16(2, it[0].number);
    TEST_ASSERT_EQUAL_UINT32(2, alarm_snapshot_troubles(&snap, &cfg, 0x02u, it, 4));
    TEST_ASSERT_EQUAL_UINT16(1, it[0].number);
}

/* BL-137: the fingerprint moves when a line is restored, though the count stays. */
static void test_sig_follows_restore(void)
{
    const uint32_t none = alarm_snapshot_troubles_sig(&snap, &cfg, 0xFFu);
    snap.sys_troubles = 1u << ALARM_TRBL_AC_LOSS;
    const uint32_t active = alarm_snapshot_troubles_sig(&snap, &cfg, 0xFFu);
    snap.sys_troubles = 0u;
    snap.sys_troubles_restored = 1u << ALARM_TRBL_AC_LOSS;
    const uint32_t restored = alarm_snapshot_troubles_sig(&snap, &cfg, 0xFFu);
    TEST_ASSERT_EQUAL_UINT32(1, alarm_snapshot_troubles(&snap, &cfg, 0xFFu, NULL, 0));
    TEST_ASSERT_NOT_EQUAL(none, active);
    TEST_ASSERT_NOT_EQUAL(active, restored);
    TEST_ASSERT_EQUAL_UINT32(restored, alarm_snapshot_troubles_sig(&snap, &cfg, 0xFFu));
    snap.zone[0].trouble = 1u << ALARM_ZTRBL_TAMPER;
    TEST_ASSERT_NOT_EQUAL(restored, alarm_snapshot_troubles_sig(&snap, &cfg, 0xFFu));
    TEST_ASSERT_EQUAL_UINT32(restored, alarm_snapshot_troubles_sig(&snap, &cfg, 0x02u));
}

/* BL-137: out NULL only counts; a short out still returns the full count. */
static void test_count_only_and_short_buffer(void)
{
    snap.sys_troubles = 7u;
    alarm_trouble_item_t it[1];
    TEST_ASSERT_EQUAL_UINT32(3, alarm_snapshot_troubles(&snap, &cfg, 0xFFu, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(3, alarm_snapshot_troubles(&snap, &cfg, 0xFFu, it, 1));
    TEST_ASSERT_EQUAL_UINT8(0, it[0].kind);
    TEST_ASSERT_EQUAL_UINT32(0, alarm_snapshot_troubles(NULL, &cfg, 0xFFu, NULL, 0));
}

/* BL-140: read a piece at a time, the list is the same lines in the same order. */
static void test_window_matches_whole_list(void)
{
    snap.sys_troubles = (1u << ALARM_TRBL_AC_LOSS) | (1u << ALARM_TRBL_LOW_BATTERY);
    snap.sys_troubles_restored = 1u << ALARM_TRBL_COMM_FAIL;
    snap.dev_troubles = 1u << 2;
    snap.zone[0].trouble = 1u << ALARM_ZTRBL_TAMPER;
    snap.zone[29].flags = ALARM_ZS_TROUBLE_MEM;
    alarm_trouble_item_t whole[8];
    const size_t n = alarm_snapshot_troubles(&snap, &cfg, 0xFFu, whole, 8);
    TEST_ASSERT_EQUAL_UINT32(6, n);
    for (size_t first = 0; first <= n; first++) {
        alarm_trouble_item_t two[2];
        memset(two, 0xA5, sizeof(two));
        TEST_ASSERT_EQUAL_UINT32(n, alarm_snapshot_troubles_from(&snap, &cfg, 0xFFu, first, two, 2));
        for (size_t i = 0; i < 2 && first + i < n; i++) {
            TEST_ASSERT_EQUAL_UINT8(whole[first + i].family, two[i].family);
            TEST_ASSERT_EQUAL_UINT8(whole[first + i].kind, two[i].kind);
            TEST_ASSERT_EQUAL_UINT16(whole[first + i].number, two[i].number);
            TEST_ASSERT_EQUAL(whole[first + i].restored, two[i].restored);
        }
        if (first + 1u >= n) { /* nothing past the end is written */
            TEST_ASSERT_EQUAL_UINT8(0xA5u, two[1].family);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(n, alarm_snapshot_troubles_from(&snap, &cfg, 0xFFu, 99u, NULL, 0));
}

/* AUD-011, docs/features/08 voice-first-build.md D05: the snapshot carries the configuration's exit_sounds as it is
 * now, so the voice's arming decision never reads a stale copy. */
static void test_exit_sounds_follows_config(void)
{
    static sim_t S;
    sim_start(&S, &cfg, NULL);
    alarm_snapshot_t *s = calloc(1, sizeof(*s));
    TEST_ASSERT_NOT_NULL(s);
    alarm_engine_snapshot(&S.eng, s);
    TEST_ASSERT_EQUAL(cfg.opt.exit_sounds, s->exit_sounds);
    alarm_config_t next = *alarm_engine_config(&S.eng);
    next.opt.exit_sounds = !cfg.opt.exit_sounds;
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_engine_set_config(&S.eng, &next, S.now));
    alarm_engine_snapshot(&S.eng, s);
    TEST_ASSERT_EQUAL(!cfg.opt.exit_sounds, s->exit_sounds);
    next.opt.exit_sounds = cfg.opt.exit_sounds;
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_engine_set_config(&S.eng, &next, S.now));
    alarm_engine_snapshot(&S.eng, s);
    TEST_ASSERT_EQUAL(cfg.opt.exit_sounds, s->exit_sounds);
    free(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_families_and_order);
    RUN_TEST(test_unused_and_active_over_restored);
    RUN_TEST(test_partition_filter);
    RUN_TEST(test_count_only_and_short_buffer);
    RUN_TEST(test_keypad_trouble_listed_on_its_home);
    RUN_TEST(test_sig_follows_restore);
    RUN_TEST(test_window_matches_whole_list);
    RUN_TEST(test_exit_sounds_follows_config);
    return UNITY_END();
}
