/* ZW-034 (docs/features/36): Binary Switch and Multilevel Switch - Sets only with values the spec
 * allows, Reports read with and without their target, values kept as percent, other CCs and short
 * commands refused. */
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_cc_switch.h"

void setUp(void) {}
void tearDown(void) {}

#define BIN ((uint8_t)ZW_CC_BINARY_SWITCH)
#define ML  ((uint8_t)ZW_CC_MULTILEVEL_SWITCH)

static void test_sets(void)
{
    uint8_t out[8];
    zw_cc_switch_set_t s = {ZW_SWITCH_ON, false, 0u};
    TEST_ASSERT_EQUAL_UINT(3u, zw_cc_switch_set_build(out, sizeof out, BIN, &s));
    const uint8_t on[] = {0x25, 0x01, 0xFF};
    TEST_ASSERT_EQUAL_MEMORY(on, out, 3u);
    s.value = 0x32u; /* a level: not for Binary Switch */
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_switch_set_build(out, sizeof out, BIN, &s));
    s.has_duration = true;
    s.duration = 0x05u;
    TEST_ASSERT_EQUAL_UINT(4u, zw_cc_switch_set_build(out, sizeof out, ML, &s));
    const uint8_t half[] = {0x26, 0x01, 0x32, 0x05};
    TEST_ASSERT_EQUAL_MEMORY(half, out, 4u);
    s.value = 0x64u; /* 100: past 99 */
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_switch_set_build(out, sizeof out, ML, &s));
    s.value = ZW_SWITCH_UNKNOWN;
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_switch_set_build(out, sizeof out, ML, &s));
    s.value = 0u;
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_switch_set_build(out, 3u, ML, &s)); /* no room */
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_switch_set_build(out, sizeof out, 0x20u, &s));

    zw_cc_switch_set_t back;
    TEST_ASSERT_TRUE(zw_cc_switch_set_parse(half, sizeof half, ML, &back));
    TEST_ASSERT_EQUAL_HEX8(0x32u, back.value);
    TEST_ASSERT_TRUE(back.has_duration);
    TEST_ASSERT_FALSE(zw_cc_switch_set_parse(half, sizeof half, BIN, &back)); /* another CC */
    TEST_ASSERT_FALSE(zw_cc_switch_set_parse(half, 2u, ML, &back));
    TEST_ASSERT_EQUAL_UINT(2u, zw_cc_switch_get_build(out, sizeof out, BIN));
    TEST_ASSERT_TRUE(zw_cc_switch_get_parse(out, 2u, BIN));
    TEST_ASSERT_FALSE(zw_cc_switch_get_parse(out, 2u, ML));
}

static void test_reports(void)
{
    uint8_t out[8];
    zw_cc_switch_report_t r = {0x40u, true, 0x63u, 0x02u};
    TEST_ASSERT_EQUAL_UINT(5u, zw_cc_switch_report_build(out, sizeof out, ML, &r));
    zw_cc_switch_report_t back;
    TEST_ASSERT_TRUE(zw_cc_switch_report_parse(out, 5u, ML, &back));
    TEST_ASSERT_EQUAL_HEX8(0x40u, back.current);
    TEST_ASSERT_TRUE(back.has_target);
    TEST_ASSERT_EQUAL_HEX8(0x63u, back.target);
    TEST_ASSERT_TRUE(zw_cc_switch_report_parse(out, 3u, ML, &back)); /* v1 */
    TEST_ASSERT_FALSE(back.has_target);
    TEST_ASSERT_FALSE(zw_cc_switch_report_parse(out, 2u, ML, &back));
    const uint8_t set[] = {0x26, 0x01, 0x10};
    TEST_ASSERT_FALSE(zw_cc_switch_report_parse(set, sizeof set, ML, &back));
}

static void test_percent(void)
{
    TEST_ASSERT_EQUAL_INT(0, zw_cc_switch_percent(BIN, 0x00u));
    TEST_ASSERT_EQUAL_INT(100, zw_cc_switch_percent(BIN, 0xFFu));
    TEST_ASSERT_EQUAL_INT(100, zw_cc_switch_percent(BIN, 0x01u));
    TEST_ASSERT_EQUAL_INT(-1, zw_cc_switch_percent(BIN, 0xFEu));
    TEST_ASSERT_EQUAL_INT(-1, zw_cc_switch_percent(BIN, 0x80u));
    TEST_ASSERT_EQUAL_INT(50, zw_cc_switch_percent(ML, 50u));
    TEST_ASSERT_EQUAL_INT(100, zw_cc_switch_percent(ML, 99u));
    TEST_ASSERT_EQUAL_INT(-1, zw_cc_switch_percent(ML, 0xFEu));
    TEST_ASSERT_EQUAL_INT(-1, zw_cc_switch_percent(ML, 0x70u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sets);
    RUN_TEST(test_reports);
    RUN_TEST(test_percent);
    return UNITY_END();
}
