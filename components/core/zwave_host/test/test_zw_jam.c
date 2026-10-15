/*
 * test_zw_jam.c - is the radio jammed? (docs/features/36 network.md §9; ZW-024): every channel
 * loud for 30 s, or three frames that never got on the air within 30 s; one busy channel is
 * ordinary traffic; a missing reading neither proves nor ends a jam; it clears 30 s after the air
 * is quiet again.
 */
#include <string.h>

#include "unity.h"
#include "zwave_host/zw_jam.h"

void setUp(void) {}
void tearDown(void) {}

static zw_jam_t J;

/* Readings every 5 s from `t` for `ms`, all channels at these levels; the last change seen. */
static bool readings(uint64_t *t, uint64_t ms, int8_t a, int8_t b, int8_t c)
{
    const uint8_t r[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c};
    bool changed = false;
    for (uint64_t end = *t + ms; *t < end; *t += ZW_JAM_POLL_MS) {
        changed = zw_jam_reading(&J, *t, r, 3u) || changed;
    }
    return changed;
}

/* ZW-024: quiet air, and one busy channel, never jam */
static void test_quiet_and_one_busy_channel(void)
{
    zw_jam_init(&J, ZW_JAM_THRESHOLD_DBM);
    uint64_t t = 1000u;
    TEST_ASSERT_FALSE(readings(&t, 120000u, -95, -92, -97));
    TEST_ASSERT_FALSE(readings(&t, 120000u, -40, -95, -94)); /* traffic on one channel */
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    TEST_ASSERT_TRUE(J.has_reading);
    TEST_ASSERT_EQUAL_INT8(-95, J.quietest);
}

/* ZW-024: every channel loud for 30 s jams; less does not; it clears 30 s after the air is quiet */
static void test_every_channel_loud(void)
{
    zw_jam_init(&J, ZW_JAM_THRESHOLD_DBM);
    uint64_t t = 1000u;
    TEST_ASSERT_FALSE(readings(&t, 30000u, -50, -45, -55)); /* 0..25 s: not yet */
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    TEST_ASSERT_TRUE(readings(&t, 5000u, -50, -45, -55)); /* 30 s */
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    TEST_ASSERT_EQUAL_INT8(-55, J.quietest);
    /* the threshold itself counts as loud */
    zw_jam_init(&J, -60);
    t = 1000u;
    (void)readings(&t, 35000u, -60, -60, -60);
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    /* quiet again: still jammed until 30 s have passed */
    TEST_ASSERT_FALSE(readings(&t, 25000u, -95, -95, -95));
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    TEST_ASSERT_TRUE(readings(&t, 10000u, -95, -95, -95));
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
}

/* ZW-024: a missing reading restarts the wait; while jammed it ends nothing */
static void test_missing_readings(void)
{
    zw_jam_init(&J, ZW_JAM_THRESHOLD_DBM);
    uint64_t t = 1000u;
    (void)readings(&t, 25000u, -50, -50, -50);
    zw_jam_no_reading(&J, t);
    t += ZW_JAM_POLL_MS;
    TEST_ASSERT_FALSE(readings(&t, 25000u, -50, -50, -50)); /* the 30 s start again */
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    TEST_ASSERT_TRUE(readings(&t, 10000u, -50, -50, -50));
    for (int i = 0; i < 20; i++) { /* 100 s of no answer */
        zw_jam_no_reading(&J, t);
        TEST_ASSERT_FALSE(zw_jam_poll(&J, t));
        t += ZW_JAM_POLL_MS;
    }
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    /* a reading of nothing but "not available" is no reading */
    const uint8_t none[3] = {0x7Fu, 0x7Fu, 0x7Fu};
    TEST_ASSERT_FALSE(zw_jam_reading(&J, t, none, 3u));
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    TEST_ASSERT_FALSE(J.has_reading);
}

/* ZW-024: saturated is loud, below sensitivity is quiet, not available is left out */
static void test_special_readings(void)
{
    zw_jam_init(&J, ZW_JAM_THRESHOLD_DBM);
    uint64_t t = 1000u;
    (void)readings(&t, 35000u, 0x7E, 0x7E, 0x7F); /* two saturated, one unknown */
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    zw_jam_init(&J, ZW_JAM_THRESHOLD_DBM);
    t = 1000u;
    (void)readings(&t, 60000u, 0x7E, 0x7E, 0x7D); /* one below sensitivity: quiet */
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    TEST_ASSERT_EQUAL_INT8(-128, J.quietest);
}

/* ZW-024: three frames that never got the air within 30 s jam; spread out, they don't */
static void test_transmit_failures(void)
{
    zw_jam_init(&J, ZW_JAM_THRESHOLD_DBM);
    TEST_ASSERT_FALSE(zw_jam_tx_failed(&J, 1000u));
    TEST_ASSERT_FALSE(zw_jam_tx_failed(&J, 16000u));
    TEST_ASSERT_FALSE(zw_jam_tx_failed(&J, 32000u)); /* the first is 31 s old */
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    TEST_ASSERT_TRUE(zw_jam_tx_failed(&J, 40000u)); /* 16, 32, 40 */
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    TEST_ASSERT_FALSE(zw_jam_poll(&J, 60000u));
    TEST_ASSERT_TRUE(zw_jam_poll(&J, 70001u)); /* the last one 30 s old, no noise: clear */
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    /* the link went away: nothing known */
    (void)zw_jam_tx_failed(&J, 80000u);
    (void)zw_jam_tx_failed(&J, 80001u);
    (void)zw_jam_tx_failed(&J, 80002u);
    TEST_ASSERT_TRUE(zw_jam_jammed(&J));
    zw_jam_reset(&J);
    TEST_ASSERT_FALSE(zw_jam_jammed(&J));
    TEST_ASSERT_EQUAL_INT8(ZW_JAM_THRESHOLD_DBM, J.threshold);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quiet_and_one_busy_channel);
    RUN_TEST(test_every_channel_loud);
    RUN_TEST(test_missing_readings);
    RUN_TEST(test_special_readings);
    RUN_TEST(test_transmit_failures);
    return UNITY_END();
}
