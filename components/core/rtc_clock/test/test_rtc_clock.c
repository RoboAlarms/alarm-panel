/*
 * test_rtc_clock.c - the DS3231's registers and the clock keeper's rules
 * (components/core/rtc_clock, docs/features/35), proved without a chip.
 *
 * Requirements: RTC-002, RTC-003, RTC-004, RTC-005, RTC-006, RTC-007, RTC-008, RTC-009
 */
#include <string.h>

#include "rtc_clock/clock_keeper.h"
#include "rtc_clock/ds3231.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define SEP24_2026 1790260205LL /* 2026-09-24 14:30:05 UTC, a Thursday */

/* A register file as a healthy DS3231 would show it: the time, INTCN set, 24.25 C. */
static void chip(uint8_t regs[DS3231_REG_COUNT], int64_t epoch_s)
{
    memset(regs, 0, DS3231_REG_COUNT);
    TEST_ASSERT_TRUE(ds3231_encode_time(epoch_s, regs));
    regs[DS3231_REG_CONTROL] = DS3231_CTRL_INTCN;
    regs[DS3231_REG_TEMP_MSB] = 24;
    regs[DS3231_REG_TEMP_LSB] = 0x40; /* .25 */
}

/* ---- the calendar ------------------------------------------------------------------------ */

/* RTC-007: UTC arithmetic of our own (C11 has no timegm), against known moments. */
static void test_the_calendar_against_known_moments(void)
{
    rtc_civil_t c;
    rtc_civil_from_epoch(SEP24_2026, &c);
    TEST_ASSERT_EQUAL_INT32(2026, c.year);
    TEST_ASSERT_EQUAL_UINT8(9, c.month);
    TEST_ASSERT_EQUAL_UINT8(24, c.day);
    TEST_ASSERT_EQUAL_UINT8(14, c.hour);
    TEST_ASSERT_EQUAL_UINT8(30, c.minute);
    TEST_ASSERT_EQUAL_UINT8(5, c.second);
    TEST_ASSERT_EQUAL_UINT8(4, c.weekday);
    int64_t back;
    TEST_ASSERT_TRUE(rtc_epoch_from_civil(&c, &back));
    TEST_ASSERT_EQUAL_INT64(SEP24_2026, back);

    rtc_civil_from_epoch(1709164800LL, &c); /* a leap day */
    TEST_ASSERT_EQUAL_UINT8(2, c.month);
    TEST_ASSERT_EQUAL_UINT8(29, c.day);
    rtc_civil_from_epoch(946684800LL, &c);
    TEST_ASSERT_EQUAL_INT32(2000, c.year);
    TEST_ASSERT_EQUAL_UINT8(6, c.weekday); /* a Saturday */
    rtc_civil_from_epoch(-1, &c);
    TEST_ASSERT_EQUAL_INT32(1969, c.year);
    TEST_ASSERT_EQUAL_UINT8(23, c.hour);
    TEST_ASSERT_EQUAL_UINT8(3, c.weekday);
    TEST_ASSERT_EQUAL_INT64(0, rtc_days_from_civil(1970, 1, 1));
}

/* RTC-007: every day from 2000 to 2199 goes there and back. */
static void test_every_day_the_module_can_hold_round_trips(void)
{
    const int64_t first = rtc_days_from_civil(2000, 1, 1);
    const int64_t last = rtc_days_from_civil(2199, 12, 31);
    for (int64_t d = first; d <= last; d++) {
        rtc_civil_t c;
        rtc_civil_from_epoch(d * 86400 + 43199, &c);
        int64_t back;
        TEST_ASSERT_TRUE(rtc_epoch_from_civil(&c, &back));
        TEST_ASSERT_EQUAL_INT64(d * 86400 + 43199, back);
    }
}

static void test_impossible_dates_are_refused(void)
{
    rtc_civil_t c = {2026, 2, 29, 0, 0, 0, 0};
    int64_t e;
    TEST_ASSERT_FALSE(rtc_epoch_from_civil(&c, &e)); /* 2026 is no leap year */
    c.year = 2028;
    TEST_ASSERT_TRUE(rtc_epoch_from_civil(&c, &e));
    c.year = 2100; /* a century that isn't divisible by 400 */
    TEST_ASSERT_FALSE(rtc_epoch_from_civil(&c, &e));
    const rtc_civil_t bad[] = {
        {2026, 4, 31, 0, 0, 0, 0}, {2026, 13, 1, 0, 0, 0, 0}, {2026, 0, 1, 0, 0, 0, 0},
        {2026, 1, 0, 0, 0, 0, 0},  {2026, 1, 1, 24, 0, 0, 0}, {2026, 1, 1, 0, 60, 0, 0},
        {2026, 1, 1, 0, 0, 60, 0},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE(rtc_epoch_from_civil(&bad[i], &e));
    }
    TEST_ASSERT_EQUAL_UINT(30, rtc_days_in_month(2026, 9));
    TEST_ASSERT_EQUAL_UINT(0, rtc_days_in_month(2026, 0));
}

/* ---- the time registers ------------------------------------------------------------------ */

/* RTC-007: BCD, 24-hour mode, the weekday, the century bit. */
static void test_the_time_registers(void)
{
    uint8_t r[DS3231_TIME_LEN];
    TEST_ASSERT_TRUE(ds3231_encode_time(SEP24_2026, r));
    const uint8_t want[DS3231_TIME_LEN] = {0x05, 0x30, 0x14, 0x04, 0x24, 0x09, 0x26};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, r, DS3231_TIME_LEN);
    int64_t e;
    TEST_ASSERT_TRUE(ds3231_decode_time(r, &e));
    TEST_ASSERT_EQUAL_INT64(SEP24_2026, e);

    TEST_ASSERT_TRUE(ds3231_encode_time(4107585600LL, r)); /* 2100-03-01 12:00, a Monday */
    TEST_ASSERT_EQUAL_HEX8(0x83, r[DS3231_REG_MONTH]);     /* century bit + March */
    TEST_ASSERT_EQUAL_HEX8(0x00, r[DS3231_REG_YEAR]);
    TEST_ASSERT_EQUAL_HEX8(0x01, r[DS3231_REG_WEEKDAY]);
    TEST_ASSERT_TRUE(ds3231_decode_time(r, &e));
    TEST_ASSERT_EQUAL_INT64(4107585600LL, e);

    TEST_ASSERT_FALSE(ds3231_encode_time(946684799LL, r)); /* 1999 */
    TEST_ASSERT_FALSE(ds3231_encode_time(7258118400LL, r)); /* 2200 */
}

/* RTC-007: a clock another device left in 12-hour mode still reads right. */
static void test_twelve_hour_mode_reads_right(void)
{
    uint8_t r[DS3231_TIME_LEN] = {0x05, 0x30, 0x40 | 0x20 | 0x02, 0x04, 0x24, 0x09, 0x26};
    int64_t e;
    TEST_ASSERT_TRUE(ds3231_decode_time(r, &e)); /* 2:30:05 PM */
    TEST_ASSERT_EQUAL_INT64(SEP24_2026, e);
    r[DS3231_REG_HOURS] = 0x40 | 0x12; /* 12 AM = midnight */
    TEST_ASSERT_TRUE(ds3231_decode_time(r, &e));
    TEST_ASSERT_EQUAL_INT64(0, e % 86400 / 3600);
    r[DS3231_REG_HOURS] = 0x40 | 0x20 | 0x12; /* 12 PM = noon */
    TEST_ASSERT_TRUE(ds3231_decode_time(r, &e));
    TEST_ASSERT_EQUAL_INT64(12, e % 86400 / 3600);
    r[DS3231_REG_HOURS] = 0x40 | 0x13; /* hour 13 in 12-hour mode */
    TEST_ASSERT_FALSE(ds3231_decode_time(r, &e));
}

/* RTC-004: registers that are not a time the chip wrote are refused, digit by digit. */
static void test_garbled_time_registers_are_refused(void)
{
    const uint8_t good[DS3231_TIME_LEN] = {0x05, 0x30, 0x14, 0x04, 0x24, 0x09, 0x26};
    int64_t e;
    const struct {
        uint8_t reg, value;
    } bad[] = {
        {DS3231_REG_SECONDS, 0x60}, {DS3231_REG_SECONDS, 0x0A}, {DS3231_REG_SECONDS, 0x85},
        {DS3231_REG_MINUTES, 0x5A}, {DS3231_REG_HOURS, 0x24},   {DS3231_REG_HOURS, 0x1F},
        {DS3231_REG_DATE, 0x00},    {DS3231_REG_DATE, 0x31},    /* September 31st */
        {DS3231_REG_DATE, 0x4A},    {DS3231_REG_MONTH, 0x13},   {DS3231_REG_MONTH, 0x00},
        {DS3231_REG_MONTH, 0x29},   {DS3231_REG_YEAR, 0xA0},    {DS3231_REG_YEAR, 0x2F},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t r[DS3231_TIME_LEN];
        memcpy(r, good, sizeof(r));
        r[bad[i].reg] = bad[i].value;
        TEST_ASSERT_FALSE_MESSAGE(ds3231_decode_time(r, &e), "garbled register accepted");
    }
    const uint8_t all_ones[DS3231_TIME_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_FALSE(ds3231_decode_time(all_ones, &e)); /* a bus that floats high */
}

/* ---- identification, status and set-up --------------------------------------------------- */

/* RTC-002: a DS3231 is told from other chips that answer at 0x68. */
static void test_identification(void)
{
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    TEST_ASSERT_TRUE(ds3231_identify(r));
    /* Fresh from the factory: 2000-01-01 00:00, OSF set, EOSC clear. */
    memset(r, 0, sizeof(r));
    r[DS3231_REG_WEEKDAY] = 1;
    r[DS3231_REG_DATE] = 1;
    r[DS3231_REG_MONTH] = 1;
    r[DS3231_REG_CONTROL] = 0x1C;
    r[DS3231_REG_STATUS] = 0x88;
    r[DS3231_REG_TEMP_MSB] = 22;
    TEST_ASSERT_TRUE(ds3231_identify(r));

    uint8_t other[DS3231_REG_COUNT];
    memset(other, 0xFF, sizeof(other)); /* nothing but pull-ups */
    TEST_ASSERT_FALSE(ds3231_identify(other));
    chip(other, SEP24_2026);
    other[DS3231_REG_SECONDS] |= 0x80; /* a DS1307 with its clock halted */
    TEST_ASSERT_FALSE(ds3231_identify(other));
    chip(other, SEP24_2026);
    other[DS3231_REG_STATUS] = 0x40; /* DS1307 RAM or an MPU-6050 register */
    TEST_ASSERT_FALSE(ds3231_identify(other));
    chip(other, SEP24_2026);
    other[DS3231_REG_TEMP_LSB] = 0x41;
    TEST_ASSERT_FALSE(ds3231_identify(other));
    chip(other, SEP24_2026);
    other[DS3231_REG_TEMP_MSB] = 100; /* no DS3231 is at 100 C */
    TEST_ASSERT_FALSE(ds3231_identify(other));
    chip(other, SEP24_2026);
    other[DS3231_REG_WEEKDAY] = 0x09;
    TEST_ASSERT_FALSE(ds3231_identify(other));
}

/* RTC-007: temperature in quarter degrees, below zero too; the flags. */
static void test_the_status(void)
{
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    ds3231_status_t st;
    ds3231_parse_status(r, &st);
    TEST_ASSERT_EQUAL_INT16(97, st.temp_c4); /* 24.25 C */
    TEST_ASSERT_FALSE(st.osc_stopped);
    TEST_ASSERT_FALSE(st.stops_on_battery);
    TEST_ASSERT_FALSE(st.out_32khz);
    r[DS3231_REG_TEMP_MSB] = 0xFE; /* -2 */
    r[DS3231_REG_TEMP_LSB] = 0xC0; /* +.75: -1.25 C */
    r[DS3231_REG_STATUS] = DS3231_STAT_OSF | DS3231_STAT_EN32K;
    r[DS3231_REG_CONTROL] = DS3231_CTRL_EOSC;
    r[DS3231_REG_AGING] = 0xFD;
    ds3231_parse_status(r, &st);
    TEST_ASSERT_EQUAL_INT16(-5, st.temp_c4);
    TEST_ASSERT_TRUE(st.osc_stopped);
    TEST_ASSERT_TRUE(st.out_32khz);
    TEST_ASSERT_TRUE(st.stops_on_battery);
    TEST_ASSERT_EQUAL_INT8(-3, st.aging);
}

/* RTC-007: set up for a coin cell - the oscillator keeps running on it, nothing else draws. */
static void test_the_set_up_for_a_coin_cell(void)
{
    /* EOSC (a module some firmware set to stop on battery), a square wave, alarm interrupts. */
    TEST_ASSERT_EQUAL_HEX8(DS3231_CTRL_INTCN, ds3231_control_wanted(0xC3));
    TEST_ASSERT_EQUAL_HEX8(DS3231_CTRL_INTCN | DS3231_CTRL_RS1 | DS3231_CTRL_RS2,
                           ds3231_control_wanted(0x1C)); /* the power-on value keeps RS */
    /* 32 kHz output and alarm flags off; OSF kept until the time is written. */
    TEST_ASSERT_EQUAL_HEX8(DS3231_STAT_OSF, ds3231_status_wanted(0x8B, false));
    TEST_ASSERT_EQUAL_HEX8(0x00, ds3231_status_wanted(0x8B, true));
    TEST_ASSERT_EQUAL_HEX8(0x00, ds3231_status_wanted(0x0B, false));
}

/* ---- the keeper -------------------------------------------------------------------------- */

/* RTC-001: no module - nothing changes, nothing may be set by hand. */
static void test_without_a_module_nothing_changes(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    memset(r, 0xFF, sizeof(r));
    const clock_action_t a = clock_keeper_start(&k, false, r, 0);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, a.kind);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_ABSENT, k.module);
    TEST_ASSERT_FALSE(clock_keeper_can_set(&k));
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_manual(&k, SEP24_2026 * 1000, 10).kind);
    clock_keeper_internet(&k, 1000);
    TEST_ASSERT_FALSE(clock_keeper_write_due(&k, 1000));
    TEST_ASSERT_FALSE(clock_keeper_check_due(&k, 10u * CLOCK_CHECK_EVERY_MS));
    TEST_ASSERT_EQUAL(CLOCK_SRC_INTERNET, k.source);
}

/* RTC-002: another chip at 0x68 is left alone. */
static void test_another_chip_is_never_written(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    memset(r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_start(&k, true, r, 0).kind);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_OTHER, k.module);
    clock_keeper_internet(&k, 5);
    TEST_ASSERT_FALSE(clock_keeper_write_due(&k, 5));
    TEST_ASSERT_FALSE(clock_keeper_can_set(&k));
    TEST_ASSERT_FALSE(clock_keeper_check_due(&k, 10u * CLOCK_CHECK_EVERY_MS));
}

/* RTC-003: a module that kept the time sets the clock at start-up. */
static void test_a_module_that_kept_the_time_sets_the_clock(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    const clock_action_t a = clock_keeper_start(&k, true, r, 1200);
    TEST_ASSERT_EQUAL(CLOCK_DO_SET, a.kind);
    TEST_ASSERT_EQUAL_INT64(SEP24_2026 * 1000 + 500, a.epoch_ms); /* the middle of its second */
    TEST_ASSERT_EQUAL(CLOCK_MODULE_OK, k.module);
    TEST_ASSERT_EQUAL(CLOCK_SRC_MODULE, k.source);
    TEST_ASSERT_FALSE(k.lost_at_start);
    TEST_ASSERT_FALSE(k.write_pending);
    TEST_ASSERT_TRUE(k.temp_known);
    TEST_ASSERT_TRUE(clock_keeper_can_set(&k));
    TEST_ASSERT_FALSE(clock_keeper_check_due(&k, 1200 + CLOCK_CHECK_FIRST_MS - 1));
    TEST_ASSERT_TRUE(clock_keeper_check_due(&k, 1200 + CLOCK_CHECK_FIRST_MS));
}

/* RTC-004: a module that lost the time is not believed - OSF, 2000-01-01, garbage. */
static void test_a_module_that_lost_the_time_is_not_believed(void)
{
    uint8_t r[DS3231_REG_COUNT];
    clock_keeper_t k;

    clock_keeper_init(&k);
    chip(r, SEP24_2026);
    r[DS3231_REG_STATUS] = DS3231_STAT_OSF; /* the coin cell ran flat, then power came back */
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_start(&k, true, r, 0).kind);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_LOST, k.module);
    TEST_ASSERT_TRUE(k.lost_at_start);
    TEST_ASSERT_EQUAL(CLOCK_SRC_NONE, k.source);
    TEST_ASSERT_TRUE(clock_keeper_can_set(&k)); /* a person may give it the time */

    clock_keeper_init(&k);
    chip(r, 946684800LL); /* 2000: a fresh chip whose flag somebody cleared */
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_start(&k, true, r, 0).kind);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_LOST, k.module);

    clock_keeper_init(&k);
    chip(r, SEP24_2026);
    r[DS3231_REG_DATE] = 0x31; /* September 31st */
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_start(&k, true, r, 0).kind);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_LOST, k.module);

    /* ... and the next time-server sync gives it the time. */
    TEST_ASSERT_FALSE(clock_keeper_write_due(&k, 10));
    clock_keeper_internet(&k, 10);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, 10));
    clock_keeper_written(&k, true, 20);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_OK, k.module);
    TEST_ASSERT_FALSE(k.write_pending);
    TEST_ASSERT_TRUE(k.lost_at_start); /* still said: the coin cell may be flat */
}

/* RTC-005: every sync writes the module; while the internet is trusted it wins. */
static void test_the_internet_writes_the_module(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    (void)clock_keeper_start(&k, true, r, 0);
    clock_keeper_internet(&k, 30000);
    TEST_ASSERT_EQUAL(CLOCK_SRC_INTERNET, k.source);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, 30000));
    clock_keeper_written(&k, true, 30100);
    TEST_ASSERT_FALSE(clock_keeper_write_due(&k, 30200));
    TEST_ASSERT_EQUAL_UINT32(1, k.writes);

    /* An hour later the module is 1.2 s off (someone else wrote it): rewrite, never slew. */
    const uint64_t t = 30100 + CLOCK_CHECK_FIRST_MS;
    chip(r, SEP24_2026 + 60);
    clock_action_t a = clock_keeper_compare(&k, true, r, (SEP24_2026 + 60) * 1000 - 1200, t);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, a.kind);
    TEST_ASSERT_EQUAL_INT64(1200, k.last_offset_ms);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, t));
    clock_keeper_written(&k, true, t);
    /* Within a second of internet time: left alone. */
    a = clock_keeper_compare(&k, true, r, (SEP24_2026 + 60) * 1000 + 900, t + CLOCK_CHECK_FIRST_MS);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, a.kind);
    TEST_ASSERT_FALSE(k.write_pending);
    TEST_ASSERT_TRUE(clock_keeper_internet_trusted(&k, 30000 + CLOCK_INTERNET_TRUST_MS - 1));
    TEST_ASSERT_FALSE(clock_keeper_internet_trusted(&k, 30000 + CLOCK_INTERNET_TRUST_MS));
}

/* RTC-006: without the internet the module is followed - slewed, never stepped - and a module
 * that jumped is put back. */
static void test_without_the_internet_the_module_is_followed(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    (void)clock_keeper_start(&k, true, r, 0);
    uint64_t t = CLOCK_CHECK_FIRST_MS;

    /* The start-up read guessed the middle of the second; the tick says 300 ms more. */
    chip(r, SEP24_2026 + 60);
    clock_action_t a = clock_keeper_compare(&k, true, r, (SEP24_2026 + 60) * 1000 - 300, t);
    TEST_ASSERT_EQUAL(CLOCK_DO_SLEW, a.kind);
    TEST_ASSERT_EQUAL_INT64(300, a.delta_ms);
    TEST_ASSERT_FALSE(clock_keeper_check_due(&k, t + CLOCK_CHECK_EVERY_MS - 1));

    /* An hour later, 72 ms of the panel's drift: left alone. */
    t += CLOCK_CHECK_EVERY_MS;
    chip(r, SEP24_2026 + 3660);
    a = clock_keeper_compare(&k, true, r, (SEP24_2026 + 3660) * 1000 + 72, t);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, a.kind);
    TEST_ASSERT_EQUAL_INT64(-72, k.last_offset_ms);

    /* The panel ran fast by 20 s (a day on the internal crystal, or a person's typo corrected
     * on the module): slewed back. */
    t += CLOCK_CHECK_EVERY_MS;
    a = clock_keeper_compare(&k, true, r, (SEP24_2026 + 3660) * 1000 + 20000, t);
    TEST_ASSERT_EQUAL(CLOCK_DO_SLEW, a.kind);
    TEST_ASSERT_EQUAL_INT64(-20000, a.delta_ms);

    /* The module suddenly reads a day later: it is the one that moved. */
    t += CLOCK_CHECK_EVERY_MS;
    chip(r, SEP24_2026 + 86400);
    a = clock_keeper_compare(&k, true, r, SEP24_2026 * 1000, t);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, a.kind);
    TEST_ASSERT_EQUAL_UINT32(1, k.jumps);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, t));
    TEST_ASSERT_EQUAL(CLOCK_SRC_MODULE, k.source);
}

/* RTC-008: a person sets the time only while a module can keep it, within 2025..2099. */
static void test_setting_the_time_by_hand(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    r[DS3231_REG_STATUS] = DS3231_STAT_OSF;
    (void)clock_keeper_start(&k, true, r, 0);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_manual(&k, 946684800000LL, 5).kind);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_manual(&k, 4102444800000LL, 5).kind);
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_manual(&k, -1, 5).kind);
    const clock_action_t a = clock_keeper_manual(&k, SEP24_2026 * 1000 + 123, 5);
    TEST_ASSERT_EQUAL(CLOCK_DO_SET, a.kind);
    TEST_ASSERT_EQUAL_INT64(SEP24_2026 * 1000 + 123, a.epoch_ms);
    TEST_ASSERT_EQUAL(CLOCK_SRC_MANUAL, k.source);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, 5));
    clock_keeper_written(&k, true, 1005);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_OK, k.module);
}

/* RTC-009: a module that stops answering is said so after three failures, and comes back. */
static void test_a_module_that_stops_answering(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    (void)clock_keeper_start(&k, true, r, 0);
    uint64_t t = CLOCK_CHECK_FIRST_MS;
    for (unsigned i = 0; i < CLOCK_FAILS_TO_LOSE - 1u; i++) {
        (void)clock_keeper_compare(&k, false, r, 0, t);
        TEST_ASSERT_EQUAL(CLOCK_MODULE_OK, k.module);
        TEST_ASSERT_TRUE(clock_keeper_check_due(&k, t + CLOCK_RETRY_MS)); /* tried again soon */
        t += CLOCK_RETRY_MS;
    }
    uint8_t garbled[DS3231_REG_COUNT];
    memset(garbled, 0xFF, sizeof(garbled));
    (void)clock_keeper_compare(&k, true, garbled, 0, t); /* garbage counts as a failure */
    TEST_ASSERT_EQUAL(CLOCK_MODULE_FAILED, k.module);
    TEST_ASSERT_FALSE(clock_keeper_can_set(&k));
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING, clock_keeper_manual(&k, SEP24_2026 * 1000, t).kind);
    clock_keeper_internet(&k, t);
    TEST_ASSERT_FALSE(clock_keeper_write_due(&k, t)); /* not while it isn't answering */

    t += CLOCK_RETRY_MS;
    TEST_ASSERT_TRUE(clock_keeper_check_due(&k, t));
    chip(r, SEP24_2026 + 200);
    (void)clock_keeper_compare(&k, true, r, (SEP24_2026 + 200) * 1000, t);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_OK, k.module);
    TEST_ASSERT_EQUAL_UINT8(0, k.failures);

    /* A write that fails is retried a minute later, not at once. */
    clock_keeper_internet(&k, t);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, t));
    clock_keeper_written(&k, false, t);
    TEST_ASSERT_FALSE(clock_keeper_write_due(&k, t + CLOCK_RETRY_MS - 1));
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, t + CLOCK_RETRY_MS));
    TEST_ASSERT_EQUAL_UINT32(1, k.write_failures);
}

/* RTC-004: a module that loses the time while the panel runs gets the panel's back; a panel
 * with no time takes the module's once it has one. */
static void test_losing_and_regaining_the_time_while_running(void)
{
    clock_keeper_t k;
    clock_keeper_init(&k);
    uint8_t r[DS3231_REG_COUNT];
    chip(r, SEP24_2026);
    (void)clock_keeper_start(&k, true, r, 0);
    r[DS3231_REG_STATUS] = DS3231_STAT_OSF;
    TEST_ASSERT_EQUAL(CLOCK_DO_NOTHING,
                      clock_keeper_compare(&k, true, r, SEP24_2026 * 1000, 60000).kind);
    TEST_ASSERT_EQUAL(CLOCK_MODULE_LOST, k.module);
    TEST_ASSERT_TRUE(clock_keeper_write_due(&k, 60000));

    clock_keeper_init(&k);
    r[DS3231_REG_STATUS] = DS3231_STAT_OSF;
    (void)clock_keeper_start(&k, true, r, 0);
    r[DS3231_REG_STATUS] = 0; /* something else set it (the Flasher, before this start) */
    const clock_action_t a = clock_keeper_compare(&k, true, r, 0, 60000);
    TEST_ASSERT_EQUAL(CLOCK_DO_SET, a.kind);
    TEST_ASSERT_EQUAL_INT64(SEP24_2026 * 1000, a.epoch_ms); /* read on the tick: exact */
    TEST_ASSERT_EQUAL(CLOCK_SRC_MODULE, k.source);
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("not fitted", clock_module_name(CLOCK_MODULE_ABSENT));
    TEST_ASSERT_EQUAL_STRING("lost the time", clock_module_name(CLOCK_MODULE_LOST));
    TEST_ASSERT_EQUAL_STRING("set by hand", clock_source_name(CLOCK_SRC_MANUAL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_calendar_against_known_moments);
    RUN_TEST(test_every_day_the_module_can_hold_round_trips);
    RUN_TEST(test_impossible_dates_are_refused);
    RUN_TEST(test_the_time_registers);
    RUN_TEST(test_twelve_hour_mode_reads_right);
    RUN_TEST(test_garbled_time_registers_are_refused);
    RUN_TEST(test_identification);
    RUN_TEST(test_the_status);
    RUN_TEST(test_the_set_up_for_a_coin_cell);
    RUN_TEST(test_without_a_module_nothing_changes);
    RUN_TEST(test_another_chip_is_never_written);
    RUN_TEST(test_a_module_that_kept_the_time_sets_the_clock);
    RUN_TEST(test_a_module_that_lost_the_time_is_not_believed);
    RUN_TEST(test_the_internet_writes_the_module);
    RUN_TEST(test_without_the_internet_the_module_is_followed);
    RUN_TEST(test_setting_the_time_by_hand);
    RUN_TEST(test_a_module_that_stops_answering);
    RUN_TEST(test_losing_and_regaining_the_time_while_running);
    RUN_TEST(test_names);
    return UNITY_END();
}
