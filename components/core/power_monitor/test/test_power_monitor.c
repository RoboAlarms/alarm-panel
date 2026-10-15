#include "power_monitor.h"
#include "unity.h"

static power_monitor_t monitor;
static power_sample_t input;
static bool failed;
void setUp(void);
void tearDown(void);
void setUp(void)
{
    power_monitor_init(&monitor);
    (void)power_source_none(NULL, &input);
    failed = false;
}
void tearDown(void) {}
static bool read_sample(void *ctx, power_sample_t *out)
{
    (void)ctx;
    *out = input;
    return !failed;
}
static void poll(uint64_t now)
{
    const power_source_t source = {.read = read_sample};
    power_monitor_poll(&monitor, &source, now);
}
/* PWR-003: unsupported sensing is unknown, never a fabricated healthy reading. */
static void test_none(void)
{
    const power_source_t source = {.read = power_source_none};
    power_monitor_poll(&monitor, &source, 0);
    power_monitor_poll(&monitor, &source, 5000);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.ac_present);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.battery_present);
    TEST_ASSERT_EQUAL(-1, monitor.status.battery_mv);
    TEST_ASSERT_EQUAL(-1, monitor.status.battery_percent);
}
/* PWR-010: debounce loss and restoration, rejecting a bouncing input. */
static void test_debounce(void)
{
    input.ac_present = POWER_YES;
    poll(0); poll(4999);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.ac_present);
    poll(5000);
    TEST_ASSERT_EQUAL(POWER_YES, monitor.status.ac_present);
    input.ac_present = POWER_NO; poll(6000);
    input.ac_present = POWER_YES; poll(9000);
    input.ac_present = POWER_NO; poll(10000); poll(14999);
    TEST_ASSERT_EQUAL(POWER_YES, monitor.status.ac_present);
    poll(15000);
    TEST_ASSERT_EQUAL(POWER_NO, monitor.status.ac_present);
    input.ac_present = POWER_YES; poll(16000); poll(21000);
    TEST_ASSERT_EQUAL(POWER_YES, monitor.status.ac_present);
}
/* PWR-010: failed reads and lost polling cannot qualify a transition. */
static void test_failure_and_gap(void)
{
    input.ac_present = POWER_NO; poll(0); poll(5000);
    failed = true; poll(6000);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.ac_present);
    failed = false; poll(7000); poll(30000);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.ac_present);
    poll(35000);
    TEST_ASSERT_EQUAL(POWER_NO, monitor.status.ac_present);
    poll(1);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.ac_present);
}
/* PWR-003/010: disconnected battery suppresses voltage/percent and low flag. */
static void test_battery(void)
{
    input.battery_present = POWER_YES;
    input.battery_low = POWER_YES;
    input.battery_mv = 3300;
    input.battery_percent = 101;
    poll(0); poll(5000); poll(10000);
    TEST_ASSERT_EQUAL(POWER_YES, monitor.status.battery_low);
    TEST_ASSERT_EQUAL(3300, monitor.status.battery_mv);
    TEST_ASSERT_EQUAL(-1, monitor.status.battery_percent);
    input.battery_present = POWER_NO; poll(11000);
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, monitor.status.battery_low);
    TEST_ASSERT_EQUAL(-1, monitor.status.battery_mv);
    poll(16000);
    TEST_ASSERT_EQUAL(POWER_NO, monitor.status.battery_present);
}
/* PWR-003: exact empty-connector capture from the V1.1 device must not
 * become a full battery. USB powers the charger without an attached cell. */
static void test_stc_empty_connector(void)
{
    const uint8_t raw[11] = {0xe0,0x0c,0,0,0xe5,0x11,0,0,100,2,2};
    TEST_ASSERT_TRUE(power_decode_stc(raw, sizeof(raw), &input));
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, input.battery_present);
    TEST_ASSERT_EQUAL(-1, input.battery_percent);
    TEST_ASSERT_EQUAL(POWER_YES, input.ac_present);
}
static void test_stc_valid_and_bad_frames(void)
{
    uint8_t raw[11] = {0x3d,0x0b,0,0,0xa0,0x0f,0,0,82,1,1}; /* 2877 / 4000 mV */
    TEST_ASSERT_TRUE(power_decode_stc(raw, sizeof(raw), &input));
    TEST_ASSERT_EQUAL(POWER_YES, input.battery_present);
    TEST_ASSERT_EQUAL(POWER_YES, input.charging);
    TEST_ASSERT_EQUAL(4000, input.battery_mv);
    TEST_ASSERT_EQUAL(82, input.battery_percent);
    raw[9] = 3;
    TEST_ASSERT_TRUE(power_decode_stc(raw, sizeof(raw), &input));
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, input.ac_present); /* not a mains-loss detector */
    TEST_ASSERT_EQUAL(POWER_NO, input.charging);
    raw[8] = 255;
    TEST_ASSERT_FALSE(power_decode_stc(raw, sizeof(raw), &input));
    TEST_ASSERT_EQUAL(POWER_UNKNOWN, input.battery_present);
    raw[8] = 82; raw[5] = 0;
    TEST_ASSERT_FALSE(power_decode_stc(raw, sizeof(raw), &input)); /* torn voltage */
    TEST_ASSERT_FALSE(power_decode_stc(raw, 10, &input));
    TEST_ASSERT_FALSE(power_decode_stc(NULL, 11, &input));
}
/* PWR-004/005: preserve the low warning in the voltage deadband; dark stays dark. */
static void test_low_hysteresis_and_power_limit(void)
{
    input.battery_present = POWER_YES;
    input.battery_low = POWER_YES;
    input.battery_mv = 3400;
    poll(0); poll(5000); poll(10000);
    TEST_ASSERT_EQUAL(POWER_YES, monitor.status.battery_low);
    input.battery_low = POWER_UNKNOWN; input.battery_mv = 3600;
    poll(15000); poll(20000);
    TEST_ASSERT_EQUAL(POWER_YES, monitor.status.battery_low);
    input.battery_low = POWER_NO; input.battery_mv = 3750;
    poll(25000); poll(30000);
    TEST_ASSERT_EQUAL(POWER_NO, monitor.status.battery_low);
    TEST_ASSERT_EQUAL(20, power_backlight_limit(65, true));
    TEST_ASSERT_EQUAL(65, power_backlight_limit(65, false));
    TEST_ASSERT_EQUAL(0, power_backlight_limit(0, true));
    TEST_ASSERT_EQUAL(10, power_backlight_limit(10, true));
}
static void test_power_saving_settles(void)
{
    power_saving_t policy = {0};
    input.battery_present = POWER_YES;
    TEST_ASSERT_FALSE(power_saving_update(&policy, &input, 100));
    TEST_ASSERT_FALSE(power_saving_update(&policy, &input, 15099));
    TEST_ASSERT_TRUE(power_saving_update(&policy, &input, 15100));
    input.ac_present = POWER_YES;
    TEST_ASSERT_TRUE(power_saving_update(&policy, &input, 16000));
    input.ac_present = POWER_UNKNOWN;
    TEST_ASSERT_TRUE(power_saving_update(&policy, &input, 20000));
    input.ac_present = POWER_YES;
    TEST_ASSERT_TRUE(power_saving_update(&policy, &input, 21000));
    TEST_ASSERT_FALSE(power_saving_update(&policy, &input, 36000));
    input.battery_present = POWER_UNKNOWN;
    input.ac_present = POWER_UNKNOWN;
    TEST_ASSERT_FALSE(power_saving_update(&policy, &input, 60000));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_power_saving_settles);
    RUN_TEST(test_stc_empty_connector);
    RUN_TEST(test_stc_valid_and_bad_frames);
    RUN_TEST(test_low_hysteresis_and_power_limit);
    RUN_TEST(test_none);
    RUN_TEST(test_debounce);
    RUN_TEST(test_failure_and_gap);
    RUN_TEST(test_battery);
    return UNITY_END();
}
