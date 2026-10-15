/* The local clock's crossings (docs/features/41 AUT-005, DESIGN.md section 2 "Time rules" T1-T7).
 * Readings are built from civil dates with auto_local_from_civil, never from the wall clock. */
#include <string.h>

#include "unity.h"

#include "automation/auto_clock.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- Readings ---------------------------------------------------------------------------- */

/* UTC epoch of a UTC civil time. */
static int64_t utc(int32_t y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    rtc_civil_t c = { y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s, 0 };
    int64_t e = 0;
    TEST_ASSERT_TRUE(rtc_epoch_from_civil(&c, &e));
    return e;
}

/* A time zone: a fixed offset, or EST5EDT with the 2026 change dates. */
typedef enum { TZ_UTC, TZ_EST5EDT, TZ_PLUS2 } tz_t;

static int64_t tz_offset_s(tz_t tz, int64_t epoch_s)
{
    switch (tz) {
    case TZ_EST5EDT: {
        /* DST 2026: from 2026-03-08 07:00 UTC (02:00 EST) to 2026-11-01 06:00 UTC (02:00 EDT). */
        int64_t on = utc(2026, 3, 8, 7, 0, 0), off = utc(2026, 11, 1, 6, 0, 0);
        return (epoch_s >= on && epoch_s < off) ? -4 * 3600 : -5 * 3600;
    }
    case TZ_PLUS2:
        return 2 * 3600;
    default:
        return 0;
    }
}

/* The reading the platform would make at this UTC epoch in this zone. */
static auto_local_t reading(tz_t tz, int64_t epoch_s)
{
    rtc_civil_t c;
    rtc_civil_from_epoch(epoch_s + tz_offset_s(tz, epoch_s), &c);
    auto_local_t lt;
    TEST_ASSERT_TRUE(auto_local_from_civil(&c, epoch_s, true, &lt));
    return lt;
}

/* A reading of a LOCAL civil time in a fixed-offset zone (offset_s = local - UTC). */
static auto_local_t local_at(int32_t y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s,
                             int64_t offset_s)
{
    rtc_civil_t c = { y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s, 0 };
    int64_t local = 0;
    TEST_ASSERT_TRUE(rtc_epoch_from_civil(&c, &local));
    auto_local_t lt;
    TEST_ASSERT_TRUE(auto_local_from_civil(&c, local - offset_s, true, &lt));
    return lt;
}

static uint16_t hm(unsigned h, unsigned m) { return (uint16_t)(h * 60u + m); }

/* ---- A small runner: a clock and some time rules, each with its own run memory -------------- */

#define RULES_MAX 6

typedef struct {
    auto_clock_t clock;
    uint64_t mono_ms;
    size_t n;
    auto_trigger_t trig[RULES_MAX];
    int32_t last_run[RULES_MAX];
    unsigned fired[RULES_MAX];
    int32_t fired_day[RULES_MAX];   /* the date of the latest run */
    uint16_t fired_at[RULES_MAX];   /* the reading's minute at the latest run */
    uint32_t gen;
    size_t spans_total;
} rig_t;

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    auto_clock_init(&r->clock);
    r->mono_ms = 5000u;
    for (size_t i = 0; i < RULES_MAX; i++) {
        r->last_run[i] = AUTO_RUN_NONE;
    }
}

static size_t rig_rule(rig_t *r, uint16_t minute, uint8_t days)
{
    auto_trigger_t t;
    memset(&t, 0, sizeof(t));
    t.kind = (uint8_t)AUTO_TRIG_TIME;
    t.minute = minute;
    t.days = days;
    r->trig[r->n] = t;
    return r->n++;
}

/* One reading, mono_advance_ms after the previous one. Returns the spans returned. */
static size_t rig_tick(rig_t *r, uint64_t mono_advance_ms, const auto_local_t *lt)
{
    auto_span_t sp[2];
    r->mono_ms += mono_advance_ms;
    size_t n = auto_clock_step(&r->clock, r->mono_ms, lt, sp);
    TEST_ASSERT_TRUE(n <= 2u);
    if (auto_clock_gen(&r->clock) != r->gen) {
        /* the caller's contract: forget the runs after the floor */
        r->gen = auto_clock_gen(&r->clock);
        for (size_t i = 0; i < RULES_MAX; i++) {
            if (r->last_run[i] > auto_clock_floor(&r->clock)) {
                r->last_run[i] = AUTO_RUN_NONE;
            }
        }
    }
    for (size_t k = 0; k < n; k++) {
        TEST_ASSERT_TRUE(sp[k].from <= sp[k].to);
        TEST_ASSERT_TRUE(sp[k].to < AUTO_MINUTES_PER_DAY);
        if (k == 1u) {
            TEST_ASSERT_EQUAL_INT32(sp[0].day_no + 1, sp[1].day_no);
        }
        for (size_t i = 0; i < r->n; i++) {
            if (auto_time_due(&r->trig[i], &sp[k], &r->last_run[i])) {
                r->fired[i]++;
                r->fired_day[i] = sp[k].day_no;
                r->fired_at[i] = lt->minute;
            }
        }
    }
    r->spans_total += n;
    return n;
}

/* Readings every step_s seconds of UTC from a to b (both included), mono following the epoch. */
static void rig_walk(rig_t *r, tz_t tz, int64_t a, int64_t b, int64_t step_s)
{
    for (int64_t e = a; e <= b; e += step_s) {
        auto_local_t lt = reading(tz, e);
        rig_tick(r, (e == a) ? 1000u : (uint64_t)step_s * 1000u, &lt);
    }
}

/* ---- auto_local_from_civil ---------------------------------------------------------------- */

/* AUT-005: the reading's date number and weekday come from the local civil date. */
static void test_local_from_civil(void)
{
    rtc_civil_t c = { 2026, 10, 5, 23, 59, 30, 0 };  /* a Monday */
    auto_local_t lt;
    TEST_ASSERT_TRUE(auto_local_from_civil(&c, utc(2026, 10, 6, 3, 59, 30), true, &lt));
    TEST_ASSERT_EQUAL_INT32((int32_t)(utc(2026, 10, 5, 0, 0, 0) / 86400), lt.day_no);
    TEST_ASSERT_EQUAL_UINT16(1439u, lt.minute);
    TEST_ASSERT_EQUAL_UINT8(1u, lt.weekday);
    c.day = 11; /* Sunday */
    TEST_ASSERT_TRUE(auto_local_from_civil(&c, utc(2026, 10, 12, 3, 59, 30), true, &lt));
    TEST_ASSERT_EQUAL_UINT8(7u, lt.weekday);
    /* T1: not valid without the clock, before 2025, or for an impossible date */
    TEST_ASSERT_FALSE(auto_local_from_civil(&c, utc(2026, 10, 12, 3, 59, 30), false, &lt));
    TEST_ASSERT_FALSE(lt.valid);
    rtc_civil_t old = { 2024, 12, 31, 23, 0, 0, 0 };
    TEST_ASSERT_FALSE(auto_local_from_civil(&old, utc(2024, 12, 31, 23, 0, 0), true, &lt));
    rtc_civil_t bad = { 2026, 2, 30, 8, 0, 0, 0 };
    TEST_ASSERT_FALSE(auto_local_from_civil(&bad, utc(2026, 3, 2, 8, 0, 0), true, &lt));
    TEST_ASSERT_FALSE(lt.valid);
}

/* ---- auto_time_due ----------------------------------------------------------------------- */

/* AUT-005: the minute must lie in the span (both ends included), on an allowed weekday, once per date. */
static void test_time_due_span_days_once(void)
{
    auto_trigger_t t;
    memset(&t, 0, sizeof(t));
    t.kind = (uint8_t)AUTO_TRIG_TIME;
    t.minute = 600;
    int32_t last = AUTO_RUN_NONE;
    auto_span_t s = { 20000, 3, 590, 599 };
    TEST_ASSERT_FALSE(auto_time_due(&t, &s, &last));
    s.to = 600;
    TEST_ASSERT_TRUE(auto_time_due(&t, &s, &last));
    TEST_ASSERT_EQUAL_INT32(20000 * 1440 + 600, last);
    TEST_ASSERT_FALSE(auto_time_due(&t, &s, &last));  /* once per date */
    s.from = 600; s.to = 610; s.day_no = 20001; s.weekday = 4;
    TEST_ASSERT_TRUE(auto_time_due(&t, &s, &last));
    s.from = 601;
    s.day_no = 20002;
    TEST_ASSERT_FALSE(auto_time_due(&t, &s, &last));

    /* weekdays only (Mon-Fri) */
    t.days = (uint8_t)(AUTO_DAY_BIT(1) | AUTO_DAY_BIT(2) | AUTO_DAY_BIT(3) | AUTO_DAY_BIT(4) | AUTO_DAY_BIT(5));
    last = AUTO_RUN_NONE;
    auto_span_t sat = { 20003, 6, 599, 601 }, sun = { 20004, 7, 599, 601 }, mon = { 20005, 1, 599, 601 };
    TEST_ASSERT_FALSE(auto_time_due(&t, &sat, &last));
    TEST_ASSERT_FALSE(auto_time_due(&t, &sun, &last));
    TEST_ASSERT_TRUE(auto_time_due(&t, &mon, &last));
    /* every day */
    t.days = 0;
    last = AUTO_RUN_NONE;
    TEST_ASSERT_TRUE(auto_time_due(&t, &sun, &last));
    t.days = AUTO_DAYS_ALL;
    TEST_ASSERT_TRUE(auto_time_due(&t, &sat, &last));

    /* only time triggers; a bad weekday never */
    t.kind = (uint8_t)AUTO_TRIG_ARMED;
    last = AUTO_RUN_NONE;
    TEST_ASSERT_FALSE(auto_time_due(&t, &mon, &last));
    t.kind = (uint8_t)AUTO_TRIG_TIME;
    auto_span_t bad = { 20005, 0, 599, 601 };
    TEST_ASSERT_FALSE(auto_time_due(&t, &bad, &last));
    TEST_ASSERT_EQUAL_INT32(AUTO_RUN_NONE, last);
    /* a run memory from a later date (a clock once ahead) doesn't silence today */
    last = 30000 * 1440 + 600;
    TEST_ASSERT_TRUE(auto_time_due(&t, &mon, &last));
    /* nor one from the day before, at any minute; one from earlier the same date does */
    last = (20005 - 1) * 1440 + 1439;
    TEST_ASSERT_TRUE(auto_time_due(&t, &mon, &last));
    last = 20005 * 1440;
    TEST_ASSERT_FALSE(auto_time_due(&t, &mon, &last));
    /* negative runs use the floor of the division: day -1 minute 1439 is day -1, not day 0 */
    auto_span_t d0 = { 0, 4, 599, 601 }, dm1 = { -1, 3, 599, 601 };
    last = -1;
    TEST_ASSERT_TRUE(auto_time_due(&t, &d0, &last));
    last = -1;
    TEST_ASSERT_FALSE(auto_time_due(&t, &dm1, &last));
    /* a date too far away for a run to fit: never due, memory untouched */
    auto_span_t far = { INT32_MAX / 1440 + 1, 1, 599, 601 };
    last = AUTO_RUN_NONE;
    TEST_ASSERT_FALSE(auto_time_due(&t, &far, &last));
    TEST_ASSERT_EQUAL_INT32(AUTO_RUN_NONE, last);
    last = 0;
    TEST_ASSERT_TRUE(auto_time_due(&t, &mon, &last));
}

/* ---- T1, T7 ------------------------------------------------------------------------------ */

/* AUT-005 T1, T7: no spans without a valid clock; the first valid reading (and the first after init)
 * is a baseline, so nothing missed runs late. */
static void test_t1_t7_baseline(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    auto_local_t lt = local_at(2026, 10, 5, 7, 59, 0, 0);
    auto_local_t unset = lt;
    unset.valid = false;
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &unset));
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));  /* baseline */
    /* the clock goes not-valid for a while, then comes back past 08:00: no late run */
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &unset));
    lt = local_at(2026, 10, 5, 8, 0, 30, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 60000, &lt));
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at8]);
    /* an epoch before 2025 is not valid even when flagged so */
    auto_local_t early = lt;
    early.epoch_s = AUTO_CLOCK_EPOCH_MIN - 1;
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &early));

    /* T7: a restart (init) re-baselines: 08:00 already passed before it doesn't run */
    rig_init(&r);
    at8 = rig_rule(&r, hm(8, 0), 0);
    lt = local_at(2026, 10, 5, 8, 0, 10, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    lt = local_at(2026, 10, 5, 8, 0, 11, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at8]);
}

/* ---- T4 ---------------------------------------------------------------------------------- */

/* AUT-005 T4: normal progress fires each rule once at its minute; repeated readings within a
 * minute make no second span; minute 0 and 1439 are reachable; midnight splits per date. */
static void test_t4_progress_minute_edges_and_midnight(void)
{
    rig_t r;
    rig_init(&r);
    size_t first = rig_rule(&r, 0, 0);
    size_t last = rig_rule(&r, 1439, 0);
    size_t mid = rig_rule(&r, hm(12, 0), 0);
    /* Sunday 2026-10-04 00:00 local (UTC+2) to Tuesday 10-06 00:30, a reading every second */
    int64_t a = utc(2026, 10, 3, 22, 0, 0) - 1, b = utc(2026, 10, 5, 22, 30, 0);
    rig_walk(&r, TZ_PLUS2, a, b, 1);
    TEST_ASSERT_EQUAL_UINT(3, r.fired[first]);  /* 10-04, 10-05, 10-06 */
    TEST_ASSERT_EQUAL_UINT(2, r.fired[last]);
    TEST_ASSERT_EQUAL_UINT(2, r.fired[mid]);
    TEST_ASSERT_EQUAL_UINT16(0, r.fired_at[first]);
    TEST_ASSERT_EQUAL_UINT16(1439, r.fired_at[last]);
    /* one span per minute passed, never two for one minute */
    TEST_ASSERT_EQUAL_size_t(2u * 1440u + 31u, r.spans_total);
}

/* AUT-005 T4: an interval crossing midnight returns two spans, each with its own date and weekday
 * (Sunday -> Monday). */
static void test_t4_midnight_split_sunday_monday(void)
{
    auto_clock_t c;
    auto_clock_init(&c);
    auto_span_t sp[2];
    auto_local_t sun = local_at(2026, 10, 4, 23, 58, 30, 0);
    auto_local_t mon = local_at(2026, 10, 5, 0, 1, 30, 0);
    TEST_ASSERT_EQUAL_UINT8(7u, sun.weekday);
    TEST_ASSERT_EQUAL_UINT8(1u, mon.weekday);
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 1000, &sun, sp));
    TEST_ASSERT_EQUAL_size_t(2, auto_clock_step(&c, 181000, &mon, sp));
    TEST_ASSERT_EQUAL_INT32(sun.day_no, sp[0].day_no);
    TEST_ASSERT_EQUAL_UINT8(7u, sp[0].weekday);
    TEST_ASSERT_EQUAL_UINT16(1439u, sp[0].from);
    TEST_ASSERT_EQUAL_UINT16(1439u, sp[0].to);
    TEST_ASSERT_EQUAL_INT32(mon.day_no, sp[1].day_no);
    TEST_ASSERT_EQUAL_UINT8(1u, sp[1].weekday);
    TEST_ASSERT_EQUAL_UINT16(0u, sp[1].from);
    TEST_ASSERT_EQUAL_UINT16(1u, sp[1].to);

    /* from 23:59 exactly: only the new date's span */
    auto_clock_init(&c);
    auto_local_t s59 = local_at(2026, 10, 4, 23, 59, 50, 0);
    auto_local_t m00 = local_at(2026, 10, 5, 0, 0, 1, 0);
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 1000, &s59, sp));
    TEST_ASSERT_EQUAL_size_t(1, auto_clock_step(&c, 12000, &m00, sp));
    TEST_ASSERT_EQUAL_INT32(m00.day_no, sp[0].day_no);
    TEST_ASSERT_EQUAL_UINT16(0u, sp[0].from);
    TEST_ASSERT_EQUAL_UINT16(0u, sp[0].to);

    /* weekday masks across the split: a Sunday-only 23:59 and a Monday-only 00:00 each run once */
    rig_t r;
    rig_init(&r);
    size_t sun_only = rig_rule(&r, 1439, (uint8_t)AUTO_DAY_BIT(7));
    size_t mon_only = rig_rule(&r, 0, (uint8_t)AUTO_DAY_BIT(1));
    size_t sun_at0 = rig_rule(&r, 0, (uint8_t)AUTO_DAY_BIT(7));
    rig_tick(&r, 1000, &sun);
    rig_tick(&r, 180000, &mon);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[sun_only]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[mon_only]);
    TEST_ASSERT_EQUAL_UINT(0, r.fired[sun_at0]);
    TEST_ASSERT_EQUAL_INT32(sun.day_no, r.fired_day[sun_only]);
    TEST_ASSERT_EQUAL_INT32(mon.day_no, r.fired_day[mon_only]);
}

/* ---- T2, T3 ------------------------------------------------------------------------------ */

/* AUT-005 T2, T3: an SNTP step at 23:30 over a 23:00 rule fires nothing; a 95 s correction across
 * 23:00 fires it; a stall of more than 10 minutes re-baselines. */
static void test_t2_t3_steps_and_stalls(void)
{
    rig_t r;
    rig_init(&r);
    size_t at23 = rig_rule(&r, hm(23, 0), 0);
    auto_local_t lt = local_at(2026, 10, 5, 22, 40, 0, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 23, 30, 0, 0);  /* 50 min ahead in 1 s */
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    lt = local_at(2026, 10, 5, 23, 30, 1, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at23]);

    /* T3: 22:59:00 -> 23:00:35 in 1 s */
    rig_init(&r);
    at23 = rig_rule(&r, hm(23, 0), 0);
    lt = local_at(2026, 10, 5, 22, 59, 0, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 23, 0, 35, 0);
    TEST_ASSERT_EQUAL_size_t(1, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at23]);

    /* T3 at the limit: 10 min ahead fires, 10 min 2 s ahead doesn't */
    rig_init(&r);
    at23 = rig_rule(&r, hm(23, 0), 0);
    lt = local_at(2026, 10, 5, 22, 50, 59, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 23, 1, 0, 0);  /* +601 s of epoch in 1 s of mono: skew 600 s */
    rig_tick(&r, 1000, &lt);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at23]);
    rig_init(&r);
    at23 = rig_rule(&r, hm(23, 0), 0);
    lt = local_at(2026, 10, 5, 22, 50, 58, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 23, 1, 0, 0);
    rig_tick(&r, 1000, &lt);
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at23]);

    /* T2: a stall (no reading for 11 minutes, epoch and mono agree) re-baselines */
    rig_init(&r);
    at23 = rig_rule(&r, hm(23, 0), 0);
    lt = local_at(2026, 10, 5, 22, 55, 0, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 23, 6, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 660000, &lt));
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at23]);
    /* ...and progress resumes from there */
    lt = local_at(2026, 10, 5, 23, 7, 0, 0);
    TEST_ASSERT_EQUAL_size_t(1, rig_tick(&r, 60000, &lt));
}

/* ---- T5, T6: daylight saving ---------------------------------------------------------------- */

/* AUT-005 T5: EST5EDT spring forward 2026-03-08 (01:59 EST -> 03:00 EDT): rules at 01:30, 02:00,
 * 02:30 and 03:00 each run once; the skipped ones at 03:00. */
static void test_t5_spring_forward_est5edt(void)
{
    rig_t r;
    rig_init(&r);
    size_t r0130 = rig_rule(&r, hm(1, 30), 0);
    size_t r0200 = rig_rule(&r, hm(2, 0), 0);
    size_t r0230 = rig_rule(&r, hm(2, 30), 0);
    size_t r0300 = rig_rule(&r, hm(3, 0), 0);
    /* 00:00 EST (05:00 UTC) to 05:00 EDT (09:00 UTC), every second */
    rig_walk(&r, TZ_EST5EDT, utc(2026, 3, 8, 5, 0, 0), utc(2026, 3, 8, 9, 0, 0), 1);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0130]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0200]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0230]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0300]);
    TEST_ASSERT_EQUAL_UINT16(hm(1, 30), r.fired_at[r0130]);
    TEST_ASSERT_EQUAL_UINT16(hm(3, 0), r.fired_at[r0200]);
    TEST_ASSERT_EQUAL_UINT16(hm(3, 0), r.fired_at[r0230]);
    TEST_ASSERT_EQUAL_UINT16(hm(3, 0), r.fired_at[r0300]);

    /* the same with a reading only every 30 s */
    rig_init(&r);
    r0230 = rig_rule(&r, hm(2, 30), 0);
    rig_walk(&r, TZ_EST5EDT, utc(2026, 3, 8, 6, 30, 0), utc(2026, 3, 8, 7, 30, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0230]);
}

/* AUT-005 T6: EST5EDT fall back 2026-11-01 (01:59 EDT -> 01:00 EST): 01:30 runs once though it
 * happens twice; 02:00, 02:30 and 03:00 once each. */
static void test_t6_fall_back_est5edt(void)
{
    rig_t r;
    rig_init(&r);
    size_t r0130 = rig_rule(&r, hm(1, 30), 0);
    size_t r0200 = rig_rule(&r, hm(2, 0), 0);
    size_t r0230 = rig_rule(&r, hm(2, 30), 0);
    size_t r0300 = rig_rule(&r, hm(3, 0), 0);
    /* 00:00 EDT (04:00 UTC) to 04:00 EST (09:00 UTC), every second */
    rig_walk(&r, TZ_EST5EDT, utc(2026, 11, 1, 4, 0, 0), utc(2026, 11, 1, 9, 0, 0), 1);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0130]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0200]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0230]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[r0300]);
    /* the epoch moved forward throughout: no generation change (the run memory stops 01:30) */
    TEST_ASSERT_EQUAL_UINT32(0, auto_clock_gen(&r.clock));

    /* the step back itself returns no span */
    auto_clock_t c;
    auto_clock_init(&c);
    auto_span_t sp[2];
    auto_local_t before = reading(TZ_EST5EDT, utc(2026, 11, 1, 5, 59, 59));
    auto_local_t after = reading(TZ_EST5EDT, utc(2026, 11, 1, 6, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(hm(1, 59), before.minute);
    TEST_ASSERT_EQUAL_UINT16(hm(1, 0), after.minute);
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 1000, &before, sp));
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 2000, &after, sp));
}

/* AUT-005 T5: a local advance beyond delta epoch + 61 min is a time-zone change and re-baselines;
 * exactly an hour behaves like daylight saving (DESIGN.md). */
static void test_t5_time_zone_change(void)
{
    rig_t r;
    rig_init(&r);
    size_t at9 = rig_rule(&r, hm(9, 0), 0);
    int64_t e = utc(2026, 10, 5, 7, 0, 0);
    auto_local_t lt = local_at(2026, 10, 5, 8, 0, 0, 3600);  /* UTC+1 */
    TEST_ASSERT_EQUAL_INT64(e, lt.epoch_s);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 10, 0, 1, 3 * 3600);  /* now UTC+3: local +2 h, epoch +1 s */
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at9]);

    /* one hour: like DST, the skipped 09:00 runs */
    rig_init(&r);
    at9 = rig_rule(&r, hm(9, 0), 0);
    lt = local_at(2026, 10, 5, 8, 0, 0, 3600);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 9, 0, 1, 2 * 3600);
    TEST_ASSERT_EQUAL_size_t(1, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at9]);
}

/* AUT-005 T6: a small backward step returns nothing and a rule already run that date doesn't run
 * again; the minutes then pass forward as usual. */
static void test_t6_small_backward_step(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    size_t at805 = rig_rule(&r, hm(8, 5), 0);
    auto_local_t lt = local_at(2026, 10, 5, 7, 59, 59, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 5, 8, 6, 0, 0);
    rig_tick(&r, 361000, &lt);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at805]);
    lt = local_at(2026, 10, 5, 7, 58, 0, 0);  /* 8 min back */
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    lt = local_at(2026, 10, 5, 8, 6, 0, 0);
    rig_tick(&r, 480000, &lt);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at805]);
    TEST_ASSERT_EQUAL_UINT32(0, auto_clock_gen(&r.clock));
}

/* AUT-005 T6: a clock set wrongly into the future (by a day, or a year), then corrected back:
 * the generation changes, the runs after the floor are forgotten and the rules run on the true
 * dates. */
static void test_t6_future_clock_corrected(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    /* true date 10-05, clock set to 10-06 07:59 and runs over 08:00 */
    auto_local_t lt = local_at(2026, 10, 6, 7, 59, 0, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 6, 8, 0, 0, 0);
    rig_tick(&r, 60000, &lt);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    /* corrected to 10-05 08:00:01: a day back */
    lt = local_at(2026, 10, 5, 8, 0, 1, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT32(1, auto_clock_gen(&r.clock));
    /* the next day at 08:00 the rule runs again (its run memory was 10-06, now forgotten) */
    lt = local_at(2026, 10, 6, 7, 59, 0, 0);
    rig_tick(&r, 1000, &lt);  /* T2 re-baseline (an artificial jump to save a day of readings) */
    lt = local_at(2026, 10, 6, 8, 0, 0, 0);
    rig_tick(&r, 60000, &lt);
    TEST_ASSERT_EQUAL_UINT(2, r.fired[at8]);

    /* a year ahead, then corrected while the clock read not-valid in between */
    rig_init(&r);
    at8 = rig_rule(&r, hm(8, 0), 0);
    lt = local_at(2027, 10, 5, 7, 59, 0, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2027, 10, 5, 8, 0, 0, 0);
    rig_tick(&r, 60000, &lt);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    auto_local_t unset = lt;
    unset.valid = false;
    rig_tick(&r, 1000, &unset);
    lt = local_at(2026, 10, 5, 7, 59, 0, 0);
    rig_tick(&r, 1000, &lt);
    TEST_ASSERT_EQUAL_UINT32(1, auto_clock_gen(&r.clock));
    lt = local_at(2026, 10, 5, 8, 0, 0, 0);
    rig_tick(&r, 60000, &lt);
    TEST_ASSERT_EQUAL_UINT(2, r.fired[at8]);

}

/* AUT-005 T2, T6 (i): a clock set ahead by less than a day (20 h), run past 08:00 on the wrong
 * date, then corrected: the real 08:00 of that date still runs. */
static void test_t6_ahead_under_a_day_next_date(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    /* real 10-05 11:00; the clock is set to 10-06 07:00 (a forward step: a baseline) */
    auto_local_t lt = local_at(2026, 10, 5, 11, 0, 0, 0);
    rig_tick(&r, 1000, &lt);
    lt = local_at(2026, 10, 6, 7, 0, 0, 0);
    rig_tick(&r, 1000, &lt);
    rig_walk(&r, TZ_UTC, utc(2026, 10, 6, 7, 0, 0), utc(2026, 10, 6, 8, 5, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    TEST_ASSERT_EQUAL_INT32(local_at(2026, 10, 6, 8, 0, 0, 0).day_no, r.fired_day[at8]);
    /* corrected 20 h back to the real 10-05 12:05: the generation changes, floor 10-05 12:05 */
    lt = local_at(2026, 10, 5, 12, 5, 30, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT32(1, auto_clock_gen(&r.clock));
    TEST_ASSERT_EQUAL_INT32(lt.day_no * 1440 + hm(12, 5), auto_clock_floor(&r.clock));
    TEST_ASSERT_EQUAL_INT32(AUTO_RUN_NONE, r.last_run[at8]);
    /* the real 10-06 08:00 runs (a forward jump to save a day of readings, then minutes) */
    lt = local_at(2026, 10, 6, 7, 58, 0, 0);
    rig_tick(&r, 1000, &lt);
    rig_walk(&r, TZ_UTC, utc(2026, 10, 6, 7, 58, 0), utc(2026, 10, 6, 8, 2, 0), 30);
    TEST_ASSERT_EQUAL_UINT(2, r.fired[at8]);
}

/* AUT-005 T2, T6 (ii): a clock 2 h ahead runs 08:00 at the real 06:00; corrected back to 06:30, the
 * real 08:00 of the same date still runs. */
static void test_t6_ahead_two_hours_same_date(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 7, 50, 0), utc(2026, 10, 5, 8, 30, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    auto_local_t lt = local_at(2026, 10, 5, 6, 30, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT32(1, auto_clock_gen(&r.clock));
    TEST_ASSERT_EQUAL_INT32(lt.day_no * 1440 + hm(6, 30), auto_clock_floor(&r.clock));
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 6, 30, 30), utc(2026, 10, 5, 8, 1, 0), 30);
    TEST_ASSERT_EQUAL_UINT(2, r.fired[at8]);
    /* and only once more */
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 8, 1, 30), utc(2026, 10, 5, 9, 0, 0), 30);
    TEST_ASSERT_EQUAL_UINT(2, r.fired[at8]);
}

/* AUT-005 T6: a backward step of 10 minutes or less (here 5) bumps nothing: a rule that ran
 * 3 minutes early doesn't run again the same date. */
static void test_t6_five_minute_step_back_keeps_memory(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 7, 55, 0), utc(2026, 10, 5, 8, 3, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    auto_local_t lt = local_at(2026, 10, 5, 7, 58, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT32(0, auto_clock_gen(&r.clock));
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 7, 58, 30), utc(2026, 10, 5, 8, 10, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
}

/* AUT-005 T6: a 30-minute backward step after a rule ran at its real time: the generation changes
 * but the run is not after the floor, so it stays and the rule doesn't run again that date. */
static void test_t6_thirty_minute_step_back_after_real_run(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 7, 55, 0), utc(2026, 10, 5, 8, 45, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
    auto_local_t lt = local_at(2026, 10, 5, 8, 15, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &lt));
    TEST_ASSERT_EQUAL_UINT32(1, auto_clock_gen(&r.clock));
    TEST_ASSERT_EQUAL_INT32(lt.day_no * 1440 + hm(8, 0), r.last_run[at8]);
    rig_walk(&r, TZ_UTC, utc(2026, 10, 5, 8, 15, 30), utc(2026, 10, 5, 9, 0, 0), 30);
    TEST_ASSERT_EQUAL_UINT(1, r.fired[at8]);
}

/* AUT-005 T2: extreme epochs (a difference past int64 range once multiplied to ms, INT64_MAX, and
 * a very negative one) neither crash nor fire: each is a step or not a valid clock. */
static void test_t2_extreme_epochs(void)
{
    rig_t r;
    rig_init(&r);
    size_t at8 = rig_rule(&r, hm(8, 0), 0);
    auto_local_t a = local_at(2026, 10, 5, 7, 59, 0, 0);
    rig_tick(&r, 1000, &a);
    /* 08:00 locally, but the epoch jumped by (2^64 + 384) / 1000 s: times 1000 that wraps to
     * 384 ms in 64 bits, which would pass for a normal second */
    auto_local_t b = local_at(2026, 10, 5, 8, 0, 0, 0);
    b.epoch_s = a.epoch_s + 18446744073709552LL;
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &b));
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at8]);
    rig_init(&r);
    at8 = rig_rule(&r, hm(8, 0), 0);
    rig_tick(&r, 1000, &a);
    b.epoch_s = INT64_MAX;
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 60000, &b));
    auto_local_t c = local_at(2026, 10, 5, 8, 1, 0, 0);
    c.epoch_s = INT64_MAX - 1;
    TEST_ASSERT_EQUAL_size_t(1, rig_tick(&r, 60000, &c));  /* a normal minute at the very end */
    auto_local_t d = local_at(2026, 10, 5, 8, 2, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 60000, &d));  /* back from INT64_MAX: a step */
    b.epoch_s = INT64_MIN;
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &b));   /* not a valid clock */
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, 1000, &d));   /* a baseline after it */
    TEST_ASSERT_EQUAL_UINT(0, r.fired[at8]);
    /* a monotonic clock that leaps past INT64_MAX ms is a stall, not a negative gap */
    auto_local_t e = local_at(2026, 10, 5, 8, 2, 30, 0);
    TEST_ASSERT_EQUAL_size_t(0, rig_tick(&r, (uint64_t)INT64_MAX + 2u, &e));
}

/* AUT-005: a reading repeated within the same minute (or the same second) makes no span; a reading
 * that isn't well-formed is treated as a clock not set. */
static void test_repeated_and_malformed_readings(void)
{
    auto_clock_t c;
    auto_clock_init(&c);
    auto_span_t sp[2];
    auto_local_t a = local_at(2026, 10, 5, 12, 0, 1, 0);
    auto_local_t b = local_at(2026, 10, 5, 12, 0, 59, 0);
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 1000, &a, sp));
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 1000, &a, sp));
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 59000, &b, sp));
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 59500, &b, sp));
    auto_local_t m = local_at(2026, 10, 5, 12, 1, 0, 0);
    TEST_ASSERT_EQUAL_size_t(1, auto_clock_step(&c, 60000, &m, sp));
    TEST_ASSERT_EQUAL_UINT16(hm(12, 1), sp[0].from);
    TEST_ASSERT_EQUAL_UINT16(hm(12, 1), sp[0].to);

    auto_local_t bad = m;
    bad.minute = 1440;
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 61000, &bad, sp));
    bad = m;
    bad.weekday = 0;
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 62000, &bad, sp));
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 63000, NULL, sp));
    /* after a malformed one, the next good reading is a baseline */
    auto_local_t n = local_at(2026, 10, 5, 12, 2, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 120000, &n, sp));
    /* a monotonic clock that goes back re-baselines too */
    auto_local_t o = local_at(2026, 10, 5, 12, 3, 0, 0);
    TEST_ASSERT_EQUAL_size_t(0, auto_clock_step(&c, 100000, &o, sp));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_local_from_civil);
    RUN_TEST(test_time_due_span_days_once);
    RUN_TEST(test_t1_t7_baseline);
    RUN_TEST(test_t4_progress_minute_edges_and_midnight);
    RUN_TEST(test_t4_midnight_split_sunday_monday);
    RUN_TEST(test_t2_t3_steps_and_stalls);
    RUN_TEST(test_t5_spring_forward_est5edt);
    RUN_TEST(test_t6_fall_back_est5edt);
    RUN_TEST(test_t5_time_zone_change);
    RUN_TEST(test_t6_small_backward_step);
    RUN_TEST(test_t6_future_clock_corrected);
    RUN_TEST(test_t6_ahead_under_a_day_next_date);
    RUN_TEST(test_t6_ahead_two_hours_same_date);
    RUN_TEST(test_t6_five_minute_step_back_keeps_memory);
    RUN_TEST(test_t6_thirty_minute_step_back_after_real_run);
    RUN_TEST(test_t2_extreme_epochs);
    RUN_TEST(test_repeated_and_malformed_readings);
    return UNITY_END();
}
