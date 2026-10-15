/*
 * test_touch_calib.c - resistive touch calibration (CYD-008) and filtering (CYD-009),
 * docs/features/19. The raw values are what an XPT2046 on a 480x320 landscape panel reports.
 */
#include <string.h>

#include "touch_calib/touch_calib.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const touch_filter_cfg_t CFG = {.press_min = 300, .release_polls = 3, .dead_band = 4};

/* A poll whose samples all sit at one raw point, pressed (or not). */
static touch_raw_t poll_at(uint16_t rx, uint16_t ry, bool pressed)
{
    touch_raw_t r;
    for (uint32_t i = 0; i < TOUCH_SAMPLES; i++) {
        r.x[i] = rx;
        r.y[i] = ry;
    }
    r.z1 = pressed ? 600u : 0u;
    r.z2 = pressed ? 3000u : 4095u; /* pressure 1695 when pressed */
    return r;
}

/* ---- calibration ---------------------------------------------------------------------- */

/* CYD-008: a raw range maps its ends onto the screen's edges and its middle onto the middle. */
static void test_linear_maps_the_range_onto_the_screen(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 200, 3900, 300, 3800, false, false, false);
    int16_t x, y;
    touch_calib_map(&c, 200, 300, &x, &y);
    TEST_ASSERT_EQUAL_INT16(0, x);
    TEST_ASSERT_EQUAL_INT16(0, y);
    touch_calib_map(&c, 3900, 3800, &x, &y);
    TEST_ASSERT_EQUAL_INT16(479, x);
    TEST_ASSERT_EQUAL_INT16(319, y);
    touch_calib_map(&c, 2050, 2050, &x, &y);
    TEST_ASSERT_INT_WITHIN(1, 239, x);
    TEST_ASSERT_INT_WITHIN(1, 159, y);
}

/* CYD-008: beyond the calibrated range the point stays on the screen. */
static void test_map_clamps_to_the_screen(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 200, 3900, 300, 3800, false, false, false);
    int16_t x, y;
    touch_calib_map(&c, 0, 0, &x, &y);
    TEST_ASSERT_EQUAL_INT16(0, x);
    TEST_ASSERT_EQUAL_INT16(0, y);
    touch_calib_map(&c, 4095, 4095, &x, &y);
    TEST_ASSERT_EQUAL_INT16(479, x);
    TEST_ASSERT_EQUAL_INT16(319, y);
}

/* CYD-008: a panel glued in portrait and used in landscape: swapped axes, one mirrored. */
static void test_linear_swaps_and_mirrors(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 200, 3900, 300, 3800, true, false, true);
    int16_t x, y;
    touch_calib_map(&c, 300, 200, &x, &y); /* raw y at its low end is the screen's left */
    TEST_ASSERT_EQUAL_INT16(0, x);
    TEST_ASSERT_EQUAL_INT16(319, y); /* mirrored: raw x low is the bottom */
    touch_calib_map(&c, 3800, 3900, &x, &y);
    TEST_ASSERT_EQUAL_INT16(479, x);
    TEST_ASSERT_EQUAL_INT16(0, y);
}

/* CYD-008: a range given high-to-low runs the other way, like a mirror. */
static void test_a_reversed_range_is_a_mirror(void)
{
    touch_calib_t a, b;
    touch_calib_linear(&a, 480, 320, 3900, 200, 300, 3800, false, false, false);
    touch_calib_linear(&b, 480, 320, 200, 3900, 300, 3800, false, true, false);
    for (uint16_t raw = 200; raw <= 3900; raw = (uint16_t)(raw + 185)) {
        int16_t ax, ay, bx, by;
        touch_calib_map(&a, raw, 2000, &ax, &ay);
        touch_calib_map(&b, raw, 2000, &bx, &by);
        TEST_ASSERT_INT_WITHIN(1, ax, bx);
    }
}

/* CYD-008: three touched targets give back the map that produced them. */
static void test_solve_recovers_the_map(void)
{
    touch_calib_t truth;
    touch_calib_linear(&truth, 480, 320, 250, 3850, 3700, 350, true, false, false);
    /* the targets the calibration screen draws: near three corners */
    const uint16_t sx[3] = {48, 432, 240};
    const uint16_t sy[3] = {32, 160, 288};
    touch_calib_pair_t pairs[3];
    /* invert the truth by search: the raw point that maps onto each target */
    for (int i = 0; i < 3; i++) {
        int best = 1 << 30;
        for (uint16_t rx = 0; rx < 4096; rx = (uint16_t)(rx + 5)) {
            for (uint16_t ry = 0; ry < 4096; ry = (uint16_t)(ry + 5)) {
                int16_t x, y;
                touch_calib_map(&truth, rx, ry, &x, &y);
                const int d = (x - sx[i]) * (x - sx[i]) + (y - sy[i]) * (y - sy[i]);
                if (d < best) {
                    best = d;
                    pairs[i] = (touch_calib_pair_t){rx, ry, sx[i], sy[i]};
                }
            }
        }
    }
    touch_calib_t c;
    TEST_ASSERT_TRUE(touch_calib_solve(&c, 480, 320, pairs));
    for (uint16_t rx = 400; rx < 3700; rx = (uint16_t)(rx + 330)) {
        for (uint16_t ry = 400; ry < 3700; ry = (uint16_t)(ry + 330)) {
            int16_t ex, ey, gx, gy;
            touch_calib_map(&truth, rx, ry, &ex, &ey);
            touch_calib_map(&c, rx, ry, &gx, &gy);
            TEST_ASSERT_INT_WITHIN(2, ex, gx);
            TEST_ASSERT_INT_WITHIN(2, ey, gy);
        }
    }
}

/* CYD-008: targets nearly on a line can't tell the axes apart: refused, nothing changed. */
static void test_solve_refuses_targets_on_a_line(void)
{
    const touch_calib_pair_t pairs[3] = {
        {500, 500, 48, 32}, {2000, 2010, 240, 160}, {3500, 3505, 432, 288}};
    touch_calib_t c;
    memset(&c, 0x5A, sizeof(c));
    TEST_ASSERT_FALSE(touch_calib_solve(&c, 480, 320, pairs));
    TEST_ASSERT_EQUAL_HEX32(0x5A5A5A5Au, (uint32_t)c.a);
}

/* ---- filtering ------------------------------------------------------------------------ */

/* CYD-009: the pressure reading: nothing on the film is 0, a press is more. */
static void test_pressure(void)
{
    touch_raw_t r = poll_at(0, 0, false);
    TEST_ASSERT_EQUAL_UINT16(0, touch_pressure(&r));
    r = poll_at(0, 0, true);
    TEST_ASSERT_EQUAL_UINT16(1695, touch_pressure(&r));
}

/* CYD-009: the median ignores the odd sample out. */
static void test_median_rejects_outliers(void)
{
    const uint16_t s[5] = {2000, 4095, 2010, 0, 2005};
    TEST_ASSERT_EQUAL_UINT16(2005, touch_median(s, 5));
    const uint16_t t[3] = {7, 1, 4};
    TEST_ASSERT_EQUAL_UINT16(4, touch_median(t, 3));
}

/* CYD-009: the first poll after touch-down is dropped (not settled), then the press lands. */
static void test_first_poll_is_dropped(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 0, 4095, 0, 4095, false, false, false);
    touch_filter_t f;
    touch_filter_init(&f, &CFG);
    int16_t x, y;
    touch_raw_t r = poll_at(2048, 2048, true);
    TEST_ASSERT_FALSE(touch_filter_step(&f, &c, &r, &x, &y));
    TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &r, &x, &y));
    TEST_ASSERT_INT_WITHIN(1, 240, x);
    TEST_ASSERT_INT_WITHIN(1, 160, y);
}

/* CYD-009: a light touch below the threshold is no touch at all. */
static void test_light_touch_is_ignored(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 0, 4095, 0, 4095, false, false, false);
    touch_filter_t f;
    touch_filter_init(&f, &CFG);
    touch_raw_t r = poll_at(2048, 2048, true);
    r.z1 = 100;
    r.z2 = 3950; /* pressure 245 */
    int16_t x, y;
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(touch_filter_step(&f, &c, &r, &x, &y));
    }
}

/* CYD-009: a hold survives pressure dips shorter than the release time - the 2 s panic hold
 * must not restart because a finger eased off for a moment. */
static void test_hold_survives_short_dips(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 0, 4095, 0, 4095, false, false, false);
    touch_filter_t f;
    touch_filter_init(&f, &CFG);
    int16_t x, y;
    const touch_raw_t down = poll_at(1000, 1000, true);
    const touch_raw_t up = poll_at(1000, 1000, false);
    touch_filter_step(&f, &c, &down, &x, &y);
    TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &down, &x, &y));
    for (int round = 0; round < 20; round++) {
        TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &up, &x, &y)); /* two quiet polls */
        TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &up, &x, &y));
        TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &down, &x, &y));
    }
}

/* CYD-009: a real lift is reported after the release time, at the last point. */
static void test_release_after_quiet_polls(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 0, 4095, 0, 4095, false, false, false);
    touch_filter_t f;
    touch_filter_init(&f, &CFG);
    int16_t x, y;
    const touch_raw_t down = poll_at(1000, 3000, true);
    const touch_raw_t up = poll_at(0, 0, false);
    touch_filter_step(&f, &c, &down, &x, &y);
    touch_filter_step(&f, &c, &down, &x, &y);
    const int16_t px = x, py = y;
    TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &up, &x, &y));
    TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &up, &x, &y));
    TEST_ASSERT_FALSE(touch_filter_step(&f, &c, &up, &x, &y));
    TEST_ASSERT_EQUAL_INT16(px, x); /* LVGL releases where the finger was */
    TEST_ASSERT_EQUAL_INT16(py, y);
}

/* CYD-009: a resting finger's jitter doesn't move the point; a real move does. */
static void test_dead_band(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 0, 4095, 0, 4095, false, false, false);
    touch_filter_t f;
    touch_filter_init(&f, &CFG);
    int16_t x, y;
    touch_raw_t r = poll_at(2048, 2048, true);
    touch_filter_step(&f, &c, &r, &x, &y);
    touch_filter_step(&f, &c, &r, &x, &y);
    const int16_t px = x;
    r = poll_at(2048 + 25, 2048, true); /* about 3 px */
    touch_filter_step(&f, &c, &r, &x, &y);
    TEST_ASSERT_EQUAL_INT16(px, x);
    r = poll_at(2048 + 170, 2048, true); /* about 20 px */
    touch_filter_step(&f, &c, &r, &x, &y);
    TEST_ASSERT_INT_WITHIN(1, px + 20, x);
}

/* CYD-009: a press that jumps from one noisy sample still lands where the median says. */
static void test_noisy_samples_land_on_the_median(void)
{
    touch_calib_t c;
    touch_calib_linear(&c, 480, 320, 0, 4095, 0, 4095, false, false, false);
    touch_filter_t f;
    touch_filter_init(&f, &CFG);
    touch_raw_t r = poll_at(2048, 1024, true);
    r.x[1] = 4095;
    r.y[3] = 0;
    int16_t x, y;
    touch_filter_step(&f, &c, &r, &x, &y);
    TEST_ASSERT_TRUE(touch_filter_step(&f, &c, &r, &x, &y));
    TEST_ASSERT_INT_WITHIN(1, 240, x);
    TEST_ASSERT_INT_WITHIN(1, 80, y);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_linear_maps_the_range_onto_the_screen);
    RUN_TEST(test_map_clamps_to_the_screen);
    RUN_TEST(test_linear_swaps_and_mirrors);
    RUN_TEST(test_a_reversed_range_is_a_mirror);
    RUN_TEST(test_solve_recovers_the_map);
    RUN_TEST(test_solve_refuses_targets_on_a_line);
    RUN_TEST(test_pressure);
    RUN_TEST(test_median_rejects_outliers);
    RUN_TEST(test_first_poll_is_dropped);
    RUN_TEST(test_light_touch_is_ignored);
    RUN_TEST(test_hold_survives_short_dips);
    RUN_TEST(test_release_after_quiet_polls);
    RUN_TEST(test_dead_band);
    RUN_TEST(test_noisy_samples_land_on_the_median);
    return UNITY_END();
}
