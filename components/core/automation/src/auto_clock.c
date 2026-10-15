/* The local clock's crossings: which minutes of which dates have passed (docs/features/41 AUT-005,
 * DESIGN.md section 2 "Time rules" T1-T7). */
#include "automation/auto_clock.h"

#include <string.h>

void auto_clock_init(auto_clock_t *c)
{
    /* T7: everything is RAM; the first valid reading after init is a baseline. */
    memset(c, 0, sizeof(*c));
}

static bool reading_ok(const auto_local_t *lt)
{
    return lt != NULL && lt->valid && lt->epoch_s >= AUTO_CLOCK_EPOCH_MIN &&
           lt->minute < AUTO_MINUTES_PER_DAY && lt->weekday >= 1u && lt->weekday <= 7u;
}

static void baseline(auto_clock_t *c, uint64_t mono_ms, const auto_local_t *lt)
{
    c->have_base = true;
    c->prev_mono = mono_ms;
    c->prev_epoch = lt->epoch_s;
    c->prev_day = lt->day_no;
    c->prev_minute = lt->minute;
    c->prev_weekday = lt->weekday;
}

/* A date and minute as a run: day_no * 1440 + minute. False when it doesn't fit in an int32_t
 * (a date thousands of years away), which also keeps it clear of AUTO_RUN_NONE. */
static bool run_of(int32_t day_no, uint16_t minute, int32_t *out)
{
    int64_t r = (int64_t)day_no * (int64_t)AUTO_MINUTES_PER_DAY + (int64_t)minute;
    if (r <= (int64_t)INT32_MIN + (int64_t)AUTO_MINUTES_PER_DAY || r > (int64_t)INT32_MAX) {
        return false;
    }
    *out = (int32_t)r;
    return true;
}

/* The date of a run: floor(run / 1440), for negative runs too. */
static int32_t run_day(int32_t run)
{
    int32_t d = run / (int32_t)AUTO_MINUTES_PER_DAY;
    if (run % (int32_t)AUTO_MINUTES_PER_DAY < 0) {
        d--;
    }
    return d;
}

/* T2, T6: a backward step of more than 10 minutes, measured against the latest epoch seen (so it
 * also counts across a stretch without a valid clock), bumps the generation and records the new
 * reading as the floor: runs remembered after it came from the clock's wrong future. Fall back
 * moves the epoch forward and a step of 10 minutes or less stays under the limit: neither bumps. */
static void track_high(auto_clock_t *c, const auto_local_t *lt)
{
    const int64_t epoch_s = lt->epoch_s;
    if (!c->have_hi) {
        c->have_hi = true;
        c->hi_epoch = epoch_s;
    } else if (epoch_s < c->hi_epoch - AUTO_CLOCK_STEP_MS / 1000) {
        c->gen++;
        c->hi_epoch = epoch_s;
        if (!run_of(lt->day_no, lt->minute, &c->floor)) {
            c->floor = lt->day_no < 0 ? AUTO_RUN_NONE : INT32_MAX;
        }
    } else if (epoch_s > c->hi_epoch) {
        c->hi_epoch = epoch_s;
    }
}

size_t auto_clock_step(auto_clock_t *c, uint64_t mono_ms, const auto_local_t *lt, auto_span_t out[2])
{
    /* T1: no valid clock, no spans; the next valid reading is a baseline. */
    if (!reading_ok(lt)) {
        c->have_base = false;
        return 0;
    }
    track_high(c, lt);
    if (!c->have_base || mono_ms < c->prev_mono) {
        baseline(c, mono_ms, lt);  /* T1, T7 (and a monotonic clock that went back) */
        return 0;
    }

    /* T2: a stall. Compared unsigned, so that no gap of the monotonic clock is too large. */
    if (mono_ms - c->prev_mono > (uint64_t)AUTO_CLOCK_STEP_MS) {
        baseline(c, mono_ms, lt);
        return 0;
    }
    const int64_t d_mono = (int64_t)(mono_ms - c->prev_mono);
    /* Both epochs are >= AUTO_CLOCK_EPOCH_MIN, so the difference can't overflow; the product
     * could, so a difference well past the limit is a step before multiplying. */
    const int64_t d_epoch = lt->epoch_s - c->prev_epoch;
    const int64_t lim_s = 2 * AUTO_CLOCK_STEP_MS / 1000 + 1;
    if (d_epoch > lim_s || d_epoch < -lim_s) {
        baseline(c, mono_ms, lt);  /* T2: a step */
        return 0;
    }
    const int64_t skew = d_epoch * 1000 - d_mono;
    if (skew > AUTO_CLOCK_STEP_MS || skew < -AUTO_CLOCK_STEP_MS) {
        baseline(c, mono_ms, lt);  /* T2: a step */
        return 0;
    }

    int64_t prev_abs = (int64_t)c->prev_day * (int64_t)AUTO_MINUTES_PER_DAY + c->prev_minute;
    int64_t now_abs = (int64_t)lt->day_no * (int64_t)AUTO_MINUTES_PER_DAY + lt->minute;
    int64_t d_local = now_abs - prev_abs;
    /* T5: a local advance well beyond the epoch's is a time-zone change. */
    if (d_local * 60 > d_epoch + (int64_t)AUTO_CLOCK_TZ_SLACK_MIN * 60) {
        baseline(c, mono_ms, lt);
        return 0;
    }

    size_t n = 0;
    if (d_local > 0) {
        /* T3, T4, T5: the minutes in (previous, now], one span per date. d_local is at most
         * 10 + 10 + 61 minutes here, so it covers at most two dates. */
        if (lt->day_no == c->prev_day) {
            out[n++] = (auto_span_t){ lt->day_no, lt->weekday, (uint16_t)(c->prev_minute + 1u), lt->minute };
        } else {
            if (c->prev_minute + 1u < AUTO_MINUTES_PER_DAY) {
                out[n++] = (auto_span_t){ c->prev_day, c->prev_weekday, (uint16_t)(c->prev_minute + 1u),
                                          (uint16_t)(AUTO_MINUTES_PER_DAY - 1u) };
            }
            out[n++] = (auto_span_t){ lt->day_no, lt->weekday, 0u, lt->minute };
        }
    }
    /* T6: fall back or a small backward step: an empty interval; last_run stops a second run.
     * A repeated reading within the same minute: nothing. Either way the reading becomes the
     * previous one. */
    baseline(c, mono_ms, lt);
    return n;
}

bool auto_time_due(const auto_trigger_t *t, const auto_span_t *s, int32_t *last_run)
{
    if (t == NULL || s == NULL || last_run == NULL || t->kind != (uint8_t)AUTO_TRIG_TIME) {
        return false;
    }
    if (t->minute < s->from || t->minute > s->to || s->weekday < 1u || s->weekday > 7u) {
        return false;
    }
    if (t->days != 0u && (t->days & AUTO_DAY_BIT(s->weekday)) == 0u) {
        return false;
    }
    int32_t run = 0;
    if (!run_of(s->day_no, t->minute, &run)) {
        return false;  /* a date too far away to remember: never run */
    }
    if (*last_run != AUTO_RUN_NONE && run_day(*last_run) == s->day_no) {
        return false;  /* once per local date (T6) */
    }
    *last_run = run;
    return true;
}

bool auto_local_from_civil(const rtc_civil_t *c, int64_t epoch_s, bool clock_ok, auto_local_t *out)
{
    memset(out, 0, sizeof(*out));
    int64_t local_s = 0;
    if (c == NULL || !rtc_epoch_from_civil(c, &local_s)) {
        return false;
    }
    int64_t day = local_s / 86400;
    if (local_s % 86400 < 0) {
        day--;
    }
    if (day < INT32_MIN || day > INT32_MAX) {
        return false;
    }
    int64_t wd = (day + 3) % 7;  /* 1970-01-01 was a Thursday (ISO 4) */
    if (wd < 0) {
        wd += 7;
    }
    out->epoch_s = epoch_s;
    out->day_no = (int32_t)day;
    out->minute = (uint16_t)((unsigned)c->hour * 60u + c->minute);
    out->weekday = (uint8_t)(wd + 1);
    out->valid = clock_ok && epoch_s >= AUTO_CLOCK_EPOCH_MIN;
    return out->valid;
}
