#include "alarm_proto/espnow_bridge_proto.h"
#include "unity.h"
#include <string.h>
void setUp(void)
{
}
void tearDown(void)
{
}

/* CP-003: reject truncated RPCs, unknown protocol versions/operations, oversized
 * radio frames and trailing bytes before the radio callback can copy them. */
static void test_bridge_wire_bounds(void)
{
    uint8_t p[ENB_MAX + 1u] = {ENB_VERSION, ENB_SEND, 0x34, 0x12};
    enb_put16(p + 12, ENB_DATA_MAX);
    TEST_ASSERT_EQUAL_HEX16(0x1234, enb_u16(p + 2));
    TEST_ASSERT_TRUE(enb_valid(p, ENB_MAX));
    for (size_t n = 0; n < ENB_MAX; ++n)
        TEST_ASSERT_FALSE(enb_valid(p, n));
    TEST_ASSERT_FALSE(enb_valid(p, ENB_MAX + 1u));
    p[0]++;
    TEST_ASSERT_FALSE(enb_valid(p, ENB_MAX));
    p[0] = ENB_VERSION;
    p[1] = 99;
    TEST_ASSERT_FALSE(enb_valid(p, ENB_MAX));
    p[1] = ENB_SEND;
    p[10] = 14;
    TEST_ASSERT_FALSE(enb_valid(p, ENB_MAX));
    p[10] = 13;
    TEST_ASSERT_TRUE(enb_valid(p, ENB_MAX));
    enb_put16(p + 12, ENB_DATA_MAX + 1u);
    TEST_ASSERT_FALSE(enb_valid(p, ENB_MAX + 1u));
    TEST_ASSERT_FALSE(enb_valid(NULL, ENB_HEADER));
}

static void test_control_messages_have_no_payload(void)
{
    uint8_t p[ENB_HEADER + 1u] = {ENB_VERSION};
    for (unsigned op = ENB_INIT; op <= ENB_STATE; ++op) {
        p[1] = (uint8_t)op;
        const bool data = op == ENB_SEND || op == ENB_RECV;
        enb_put16(p + 12, 0);
        TEST_ASSERT_EQUAL(!data, enb_valid(p, ENB_HEADER));
        enb_put16(p + 12, 1);
        TEST_ASSERT_EQUAL(data, enb_valid(p, sizeof p));
    }
}
static void test_bridge_version_is_bounded_text(void)
{
    uint8_t p[ENB_HEADER + 24] = {ENB_VERSION, ENB_INFO};
    const char version[] = "3.0.10-ra3";
    memcpy(p + ENB_HEADER, version, sizeof version - 1);
    enb_put16(p + 12, sizeof version - 1);
    TEST_ASSERT_TRUE(enb_valid(p, ENB_HEADER + sizeof version - 1));
    p[ENB_HEADER] = '\0';
    TEST_ASSERT_FALSE(enb_valid(p, ENB_HEADER + sizeof version - 1));
    enb_put16(p + 12, 24);
    TEST_ASSERT_FALSE(enb_valid(p, sizeof p));
    enb_put16(p + 12, 0);
    TEST_ASSERT_FALSE(enb_valid(p, ENB_HEADER));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bridge_wire_bounds);
    RUN_TEST(test_control_messages_have_no_payload);
    RUN_TEST(test_bridge_version_is_bounded_text);
    return UNITY_END();
}
