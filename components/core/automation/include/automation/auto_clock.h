/* auto_clock - which minutes of which local dates have passed, for time-of-day automations
 * (docs/features/41 AUT-005, DESIGN.md section 2 "Time rules" T1-T7).
 *
 * The platform reads its clock (gettimeofday + localtime_r with the panel's POSIX TZ) and hands
 * each reading in as an auto_local_t; this code never reads a clock itself. auto_clock_step
 * compares a reading with the previous one and returns the local minutes passed since, split per
 * date; auto_time_due says whether a time trigger falls in one of them and hasn't yet run that
 * date, against a run memory (last_run) the caller keeps per rule. All state is RAM in caller-owned structs: no heap, no globals.
 */
#ifndef AUTOMATION_AUTO_CLOCK_H
#define AUTOMATION_AUTO_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "automation/automation.h"
#include "rtc_clock/ds3231.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2025-01-01 00:00:00 UTC: an earlier epoch means the clock was never set. */
#define AUTO_CLOCK_EPOCH_MIN 1735689600LL
/* T2: a step or a stall larger than this re-baselines (|delta epoch - delta mono|, delta mono);
 * a backward step larger than this also bumps the generation (see auto_clock_gen). */
#define AUTO_CLOCK_STEP_MS (10LL * 60LL * 1000LL)
/* T5: a local advance beyond delta epoch + this many minutes is a time-zone change. */
#define AUTO_CLOCK_TZ_SLACK_MIN 61

/* Local civil time as the platform sees it. */
typedef struct {
    bool valid;       /* clock set: epoch >= 2025-01-01 and no clock-not-set trouble */
    int64_t epoch_s;  /* UTC seconds */
    int32_t day_no;   /* days since 1970-01-01 of the LOCAL date */
    uint16_t minute;  /* local minute of the day, 0..1439 */
    uint8_t weekday;  /* of the local date: 1 = Monday .. 7 = Sunday */
} auto_local_t;

/* Minutes from..to of one local date, both INCLUDED (from <= to). Inclusive rather than (from, to]
 * so that minute 0 of a new date can be expressed in a uint16_t. */
typedef struct {
    int32_t day_no;
    uint8_t weekday;
    uint16_t from, to;
} auto_span_t;

/* Caller-owned; set up with auto_clock_init. */
typedef struct {
    bool have_base;      /* a previous valid reading exists */
    bool have_hi;        /* hi_epoch holds a valid epoch */
    uint64_t prev_mono;  /* ms */
    int64_t prev_epoch;
    int32_t prev_day;
    uint16_t prev_minute;
    uint8_t prev_weekday;
    int64_t hi_epoch;    /* the latest valid epoch seen since init or the last gen bump */
    uint32_t gen;        /* bumped when the clock went back more than 10 minutes: see below */
    int32_t floor;       /* the run (day_no * 1440 + minute) of the reading that bumped gen */
} auto_clock_t;

void auto_clock_init(auto_clock_t *c);

/* One reading (T1-T7). Returns 0, 1 or 2 spans in out (2 when the minutes cross local midnight,
 * the earlier date first, each date with its own weekday). Call it at least once a second while
 * any time rule exists, and on every wake otherwise. */
size_t auto_clock_step(auto_clock_t *c, uint64_t mono_ms, const auto_local_t *lt, auto_span_t out[2]);

/* "Has never run": the value a caller gives a last_run it hasn't used yet or has forgotten. */
#define AUTO_RUN_NONE INT32_MIN

/* The run memory's generation and floor. A run is day_no * 1440 + minute. The generation changes
 * when the clock goes back by more than 10 minutes (T2 with the epoch going back, a day or more
 * included, also across a stretch while the clock wasn't valid); the floor is then the run of the
 * reading that went back. Contract: after each auto_clock_step, if auto_clock_gen differs from the
 * value seen after the previous step, set every last_run greater than auto_clock_floor to
 * AUTO_RUN_NONE before calling auto_time_due on that step's spans: runs from the clock's wrong
 * future are forgotten, earlier ones stay. Fall back (the epoch moves forward) and a backward step
 * of 10 minutes or less change neither. */
static inline uint32_t auto_clock_gen(const auto_clock_t *c) { return c->gen; }
static inline int32_t auto_clock_floor(const auto_clock_t *c) { return c->floor; }

/* Is an AUTO_TRIG_TIME trigger due in this span? Its minute lies in the span [from, to], the
 * span's weekday is allowed (days 0 = every day) and *last_run isn't on this date
 * (floor(*last_run / 1440) != day_no); when true, *last_run becomes this date's run of the rule's
 * minute (at most once per local date). False for any other trigger kind, and for a date too far
 * away for a run to fit in an int32_t. */
bool auto_time_due(const auto_trigger_t *t, const auto_span_t *s, int32_t *last_run);

/* Fills *out from a LOCAL civil date and time (c->weekday is ignored and computed from the date)
 * and the UTC epoch of the same moment. valid needs clock_ok, epoch_s >= AUTO_CLOCK_EPOCH_MIN and
 * a possible date and time. Returns out->valid. */
bool auto_local_from_civil(const rtc_civil_t *c, int64_t epoch_s, bool clock_ok, auto_local_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_AUTO_CLOCK_H */
