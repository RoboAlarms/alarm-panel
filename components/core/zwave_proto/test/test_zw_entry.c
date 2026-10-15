/* ZW-026, ZW-023: Entry Control wire vectors, malformed lengths and credential padding. */
#include "unity.h"
#include "zwave_proto/zw_cc.h"
#include <string.h>
void setUp(void) {}
void tearDown(void) {}

static void test_notification_wire_and_padding(void)
{
    uint8_t wire[40];
    const zw_cc_entry_notification_t in = {7u, ZW_ENTRY_DATA_ASCII, ZW_ENTRY_EVENT_ARM_AWAY,
                                          (const uint8_t *)"2468", 4u};
    TEST_ASSERT_EQUAL_UINT(22u, zw_cc_entry_notification_build(wire, sizeof(wire), &in));
    const uint8_t prefix[] = {0x6F, 1, 7, 2, 5, 16, '2', '4', '6', '8'};
    TEST_ASSERT_EQUAL_MEMORY(prefix, wire, sizeof(prefix));
    for (size_t i = sizeof(prefix); i < 22u; i++) { TEST_ASSERT_EQUAL_HEX8(0xFF, wire[i]); }
    zw_cc_entry_notification_t out;
    for (size_t n = 0; n < 22u; n++) { TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, n, &out)); }
    TEST_ASSERT_TRUE(zw_cc_entry_notification_parse(wire, 22u, &out));
    TEST_ASSERT_EQUAL_UINT(16u, out.data_len);
    TEST_ASSERT_EQUAL_UINT8(7u, out.sequence);
    wire[5] = 4; /* interoperability: an unpadded ASCII message is still bounded */
    TEST_ASSERT_TRUE(zw_cc_entry_notification_parse(wire, 10u, &out));
    wire[6] = 0xFF; /* padding must be trailing, never hide embedded data */
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, 10u, &out));
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_entry_notification_build(wire, 21u, &in));
}

static void test_notification_rejects_bad_data(void)
{
    uint8_t wire[40] = {0x6F, 1, 0, 2, 5, 33};
    zw_cc_entry_notification_t out;
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, sizeof(wire), &out));
    wire[5] = 1; wire[3] = ZW_ENTRY_DATA_NONE;
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, sizeof(wire), &out));
    wire[3] = ZW_ENTRY_DATA_MD5;
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, sizeof(wire), &out));
    wire[5] = 16;
    TEST_ASSERT_TRUE(zw_cc_entry_notification_parse(wire, 22u, &out));
    wire[3] = ZW_ENTRY_DATA_RAW; wire[5] = 0;
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, sizeof(wire), &out));
    wire[3] = ZW_ENTRY_DATA_NONE;
    TEST_ASSERT_TRUE(zw_cc_entry_notification_parse(wire, 6u, &out));
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(NULL, 6u, &out));
    TEST_ASSERT_FALSE(zw_cc_entry_notification_parse(wire, 6u, NULL));
    const zw_cc_entry_notification_t bad = {0u, 2u, 5u, NULL, 4u};
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_entry_notification_build(wire, sizeof(wire), &bad));
}

static void test_capabilities_and_answer_matching(void)
{
    const uint8_t data[] = {0x05};
    const uint8_t events[] = {0x68, 0, 0x0B};
    const zw_cc_entry_events_t in = {data, events, 1, 3, 1, 32, 1, 10};
    uint8_t wire[48], get[2];
    const size_t n = zw_cc_entry_events_build(wire, sizeof(wire), &in);
    const uint8_t expected[] = {0x6F, 5, 1, 5, 3, 0x68, 0, 0x0B, 1, 32, 1, 10};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, wire, n);
    zw_cc_entry_events_t out;
    for (size_t i = 0; i < n; i++) { TEST_ASSERT_FALSE(zw_cc_entry_events_parse(wire, i, &out)); }
    TEST_ASSERT_TRUE(zw_cc_entry_events_parse(wire, n, &out));
    TEST_ASSERT_EQUAL_UINT(3, out.event_types_len);
    TEST_ASSERT_EQUAL_UINT(2, zw_cc_entry_event_supported_get_build(get, sizeof(get)));
    TEST_ASSERT_TRUE(zw_cc_answers(get, sizeof(get), wire, n));
    TEST_ASSERT_FALSE(zw_cc_answers(get, sizeof(get), wire, n - 1u));
    wire[4] = 31;
    TEST_ASSERT_FALSE(zw_cc_entry_events_parse(wire, n, &out));
    wire[4] = 3; wire[8] = 33;
    TEST_ASSERT_FALSE(zw_cc_entry_events_parse(wire, n, &out));

    const zw_cc_entry_keys_t keys = {events, sizeof(events)};
    zw_cc_entry_keys_t keys_out;
    TEST_ASSERT_EQUAL_UINT(6, zw_cc_entry_keys_build(wire, sizeof(wire), &keys));
    TEST_ASSERT_TRUE(zw_cc_entry_keys_parse(wire, 6, &keys_out));
    TEST_ASSERT_EQUAL_MEMORY(events, keys_out.mask, sizeof(events));
    TEST_ASSERT_FALSE(zw_cc_answers(get, sizeof(get), wire, 6));
    TEST_ASSERT_EQUAL_UINT(2, zw_cc_entry_key_supported_get_build(get, sizeof(get)));
    TEST_ASSERT_TRUE(zw_cc_answers(get, sizeof(get), wire, 6));
    for (size_t i = 0; i < 6u; i++) { TEST_ASSERT_FALSE(zw_cc_entry_keys_parse(wire, i, &keys_out)); }
}

static void test_configuration_limits(void)
{
    uint8_t wire[4], get[2];
    zw_cc_entry_configuration_t in = {10, 2}, out;
    TEST_ASSERT_EQUAL_UINT(4, zw_cc_entry_configuration_set_build(wire, sizeof(wire), &in));
    TEST_ASSERT_TRUE(zw_cc_entry_configuration_set_parse(wire, sizeof(wire), &out));
    TEST_ASSERT_EQUAL_UINT8(10, out.cache_size);
    TEST_ASSERT_EQUAL_UINT(2, zw_cc_entry_configuration_get_build(get, sizeof(get)));
    TEST_ASSERT_FALSE(zw_cc_answers(get, sizeof(get), wire, sizeof(wire)));
    TEST_ASSERT_EQUAL_UINT(4, zw_cc_entry_configuration_report_build(wire, sizeof(wire), &in));
    TEST_ASSERT_TRUE(zw_cc_answers(get, sizeof(get), wire, sizeof(wire)));
    for (size_t n = 0; n < 4u; n++) {
        TEST_ASSERT_FALSE(zw_cc_entry_configuration_report_parse(wire, n, &out));
    }
    in.cache_size = 0;
    TEST_ASSERT_EQUAL_UINT(0, zw_cc_entry_configuration_set_build(wire, sizeof(wire), &in));
    wire[3] = 0;
    TEST_ASSERT_FALSE(zw_cc_entry_configuration_report_parse(wire, sizeof(wire), &out));
    wire[3] = 11;
    TEST_ASSERT_FALSE(zw_cc_entry_configuration_report_parse(wire, sizeof(wire), &out));
}

static void test_indicator_index_cannot_wrap(void)
{
    const uint8_t entries[] = {0x50, 3, 10};
    zw_cc_indicator_entry_t out;
    TEST_ASSERT_TRUE(zw_cc_indicator_entry_get(entries, sizeof(entries), 0, &out));
    TEST_ASSERT_FALSE(zw_cc_indicator_entry_get(entries, sizeof(entries), 1, &out));
    TEST_ASSERT_FALSE(zw_cc_indicator_entry_get(entries, sizeof(entries), SIZE_MAX, &out));
    TEST_ASSERT_FALSE(zw_cc_indicator_entry_get(entries, sizeof(entries), SIZE_MAX / 3u + 1u, &out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_notification_wire_and_padding);
    RUN_TEST(test_notification_rejects_bad_data);
    RUN_TEST(test_capabilities_and_answer_matching);
    RUN_TEST(test_configuration_limits);
    RUN_TEST(test_indicator_index_cannot_wrap);
    return UNITY_END();
}
