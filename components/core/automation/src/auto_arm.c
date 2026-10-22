/* Arming and disarming automations (phase C; docs/features/41 DESIGN.md section 3 "Arming" 1-7
 * and "Disarming" 1-5; AUT-007, AUT-011 to AUT-014): the warning on the monotonic clock, the
 * whole-warning rule, the put-off, one warning at a time, the disarm, the partition gap and the
 * one-hop rule. Kept apart from auto_run.c's device sends, which share nothing with it but the
 * status and the failed port (auto_run_int.h). See auto_run.h. */
#include <string.h>

#include "auto_run_int.h"
#include "home_control/home_control.h"

#define MIN_MS UINT64_C(60000)
#define NIGHT_FROM 1080u /* 18:00: an arm time from here to 06:00 is "tonight" */
#define NIGHT_TO   360u  /* 06:00: when "Not tonight" ends */
/* A hold that waits for a local time also ends this long after its end by the local minutes, so
 * a clock that stops being valid can't hold it for ever (a daylight-saving hour and more). */
#define HOLD_SLACK_MS (UINT64_C(120) * MIN_MS)

bool auto_arm_is_arm_rule(const auto_rule_t *rule)
{
    return rule->n >= 1u && rule->then[0].kind == (uint8_t)AUTO_ACT_ARM;
}

static uint8_t part_bit(uint8_t p)
{
    if (p < 1u || p > ALARM_MAX_PARTITIONS) {
        return 0u;
    }
    return (uint8_t)(1u << (unsigned)(p - 1u));
}

/* AUT-007: the earliest an automatic arm or disarm of p may go. */
static uint64_t gap_end(const auto_run_t *r, uint8_t p)
{
    const uint8_t b = part_bit(p);
    if (b == 0u || (r->part_used & b) == 0u) {
        return 0u;
    }
    return r->part_at[p - 1u] + AUTO_PART_GAP_MS;
}

static void gap_note(auto_run_t *r, uint8_t p, uint64_t now)
{
    const uint8_t b = part_bit(p);
    if (b != 0u) {
        r->part_used = (uint8_t)(r->part_used | b);
        r->part_at[p - 1u] = now;
    }
}

static void fill_warning(const auto_run_t *r, const auto_entry_t *e, auto_warning_t *w)
{
    memset(w, 0, sizeof(*w));
    const auto_action_t *a = &e->rule.then[0];
    w->uid = e->rule.uid;
    w->partition = a->partition;
    w->level = a->arm_level;
    w->warn_min = a->warn_min;
    w->state = e->arm.state;
    w->put_off_by = e->arm.user;
    w->bypass = a->bypass_open != 0u && r->less_secure != 0u ? 1u : 0u;
    if (e->arm.state == (uint8_t)AUTO_WARN_ON || e->arm.state == (uint8_t)AUTO_WARN_HOUR) {
        w->arm_at_ms = e->arm.arm_at;
        w->arm_at_epoch_ms = auto_run_epoch_ms(r, e->arm.arm_at);
    } else if (e->arm.state == (uint8_t)AUTO_WARN_NIGHT ||
               e->arm.state == (uint8_t)AUTO_WARN_DAY) {
        w->until_day = e->arm.until_day;
        w->until_minute = e->arm.until_minute;
    }
}

static void publish(auto_run_t *r, const auto_entry_t *e)
{
    if (r->port.warning == NULL) {
        return;
    }
    auto_warning_t w;
    fill_warning(r, e, &w);
    r->port.warning(r->port.ctx, &w);
}

/* The status of an arm, a disarm or a put-off: the phase C fields, the rest cleared. */
static auto_status_t *status(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint8_t res,
                             uint64_t now)
{
    memset(&e->st, 0, sizeof(e->st));
    e->st.result = res;
    e->st.action_index = idx;
    e->st.at_epoch_ms = auto_run_epoch_ms(r, now);
    return &e->st;
}

void auto_arm_end(auto_run_t *r, auto_entry_t *e)
{
    e->arm.disarm_wait = 0u;
    if (e->arm.state == (uint8_t)AUTO_WARN_NONE) {
        return;
    }
    e->arm.state = (uint8_t)AUTO_WARN_NONE;
    publish(r, e);
    e->arm.user = 0u;
}

void auto_arm_start(auto_run_t *r, auto_entry_t *e, uint64_t now, int32_t arm_run_mark)
{
    if (arm_run_mark != AUTO_RUN_NONE) {
        /* the arm minute this warning stands for is seen: crossing it isn't "no warning" */
        e->arm.arm_run = arm_run_mark;
    }
    if (e->arm.state != (uint8_t)AUTO_WARN_NONE) {
        return; /* DESIGN.md 3.4: one warning at a time; a put-off one stays put off */
    }
    e->arm.state = (uint8_t)AUTO_WARN_ON;
    e->arm.user = 0u;
    /* AUT-012: due a whole warning later on the monotonic clock */
    e->arm.arm_at = now + (uint64_t)e->rule.then[0].warn_min * MIN_MS;
    publish(r, e);
}

/* The arm, at its time (DESIGN.md "Arming" 5, 6). */
static void do_arm(auto_run_t *r, auto_entry_t *e, uint64_t now)
{
    const auto_action_t *a = &e->rule.then[0];
    /* D29 (decided 2026-10-05): "Bypass open zones" only while the installer option is
     * on now, read live with the maker; off, it arms without bypassing (and fails on an open
     * zone), never "can't run". */
    bool option = false;
    if (a->bypass_open != 0u && r->less_secure != 0u && r->port.maker_ok != NULL) {
        bool live = false;
        option = r->port.maker_ok(r->port.ctx, e->rule.made_by, &live) && live;
    }
    const uint8_t flags = option ? (uint8_t)ALARM_ARM_BYPASS_FAULTED : (uint8_t)0u;
    bool already = false;
    uint16_t zone = 0u;
    alarm_result_t res = ALARM_ERR_UNAVAILABLE;
    if (r->port.arm != NULL) {
        if (!auto_run_send_begin(r, e->rule.uid)) {
            return; /* PL04: still warning; the rules decide (auto_run_rules ends it if off) */
        }
        res = r->port.arm(r->port.ctx, e->rule.uid, a->partition, a->arm_level, flags,
                          e->rule.made_by, &already, &zone);
        auto_run_send_end(r);
        gap_note(r, a->partition, now);
    }
    auto_arm_end(r, e);
    if (already) {
        (void)status(r, e, 0u, AUTO_RES_ALREADY, now); /* 3.5: noted only, never re-armed */
    } else if (res == ALARM_OK) {
        (void)status(r, e, 0u, AUTO_RES_DONE, now);
        r->own_arm = (uint8_t)(r->own_arm | part_bit(a->partition));
    } else if (res == ALARM_ERR_NOT_READY) {
        status(r, e, 0u, AUTO_RES_NOT_READY, now)->zone = zone;
    } else {
        status(r, e, 0u, AUTO_RES_ARM_REFUSED, now)->alarm_res = (uint8_t)res;
    }
}

/* The disarm (DESIGN.md "Disarming" 1-3): re-checked like a less secure device action. */
static void do_disarm(auto_run_t *r, auto_entry_t *e, uint64_t now)
{
    const uint8_t idx = e->arm.disarm_idx;
    e->arm.disarm_wait = 0u;
    if (idx >= e->rule.n || e->rule.then[idx].kind != (uint8_t)AUTO_ACT_DISARM) {
        return;
    }
    const uint8_t p = e->rule.then[idx].partition;
    bool option = r->less_secure != 0u;
    bool maker = false;
    if (option && r->port.maker_ok != NULL) {
        maker = r->port.maker_ok(r->port.ctx, e->rule.made_by, &option);
    }
    if (!option) {
        (void)status(r, e, idx, AUTO_RES_LESS_SECURE_OFF, now);
        return;
    }
    if (!maker) {
        (void)status(r, e, idx, AUTO_RES_MAKER_GONE, now);
        return;
    }
    bool already = false;
    alarm_result_t res = ALARM_ERR_UNAVAILABLE;
    if (r->port.disarm != NULL) {
        if (!auto_run_send_begin(r, e->rule.uid)) {
            e->arm.disarm_wait = 1u; /* PL04: waits for the rules (auto_run_rules drops it if off) */
            return;
        }
        res = r->port.disarm(r->port.ctx, e->rule.uid, p, e->rule.made_by, &already);
        auto_run_send_end(r);
        gap_note(r, p, now);
    }
    if (already) {
        (void)status(r, e, idx, AUTO_RES_ALREADY, now);
    } else if (res == ALARM_OK) {
        (void)status(r, e, idx, AUTO_RES_DONE, now);
        r->own_disarm = (uint8_t)(r->own_disarm | part_bit(p));
    } else {
        /* "Disarming" 3: a refusal raises nothing here; the engine played its error sound */
        status(r, e, idx, AUTO_RES_ARM_REFUSED, now)->alarm_res = (uint8_t)res;
    }
}

void auto_arm_disarm_start(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint64_t now)
{
    e->arm.disarm_wait = 1u;
    e->arm.disarm_idx = idx;
    e->arm.disarm_at = now;
    if (now >= gap_end(r, e->rule.then[idx].partition)) {
        do_disarm(r, e, now);
    }
}

/* AUT-012, DESIGN.md "Arming" 3: the time came with no warning running. */
static void no_warning(auto_run_t *r, auto_entry_t *e, uint64_t now)
{
    (void)status(r, e, 0u, AUTO_RES_NO_WARNING, now);
    e->st.reason = (uint8_t)ALARM_AUTO_FAIL_NO_WARNING;
    auto_run_report_failed(r, e, (uint8_t)ALARM_AUTO_FAIL_NO_WARNING, (uint8_t)ALARM_AUTO_ACT_ARM,
                           0u, now);
}

void auto_arm_time(auto_run_t *r, auto_entry_t *e, const auto_span_t *s, size_t n, uint64_t now)
{
    const auto_trigger_t *t = &e->rule.when;
    const unsigned warn = e->rule.then[0].warn_min;
    /* The warning's minute: warn minutes before, on the day before when that is past midnight
     * (the weekday bits move back a day: Monday's arm warns on Sunday). */
    auto_trigger_t wt = *t;
    bool wraps = false;
    if (t->minute >= warn) {
        wt.minute = (uint16_t)(t->minute - warn);
    } else {
        wraps = true;
        wt.minute = (uint16_t)(t->minute + AUTO_MINUTES_PER_DAY - warn);
        const unsigned d = t->days;
        wt.days = (uint8_t)(((d >> 1) | ((d & 1u) << 6)) & AUTO_DAYS_ALL);
    }
    bool warn_due = false;
    bool arm_due = false;
    int32_t mark = AUTO_RUN_NONE;
    for (size_t k = 0; k < n; k++) {
        if (auto_time_due(&wt, &s[k], &e->last_run)) {
            warn_due = true;
            const int64_t run = ((int64_t)s[k].day_no + (wraps ? 1 : 0)) *
                                    (int64_t)AUTO_MINUTES_PER_DAY + (int64_t)t->minute;
            mark = run <= INT32_MAX && run > INT32_MIN ? (int32_t)run : AUTO_RUN_NONE;
        }
        if (auto_time_due(t, &s[k], &e->arm.arm_run)) {
            arm_due = true;
        }
    }
    if (arm_due && e->arm.state == (uint8_t)AUTO_WARN_NONE) {
        /* Its minute passed with no warning running (or the warning's and its own in one
         * reading): no arm that day. */
        no_warning(r, e, now);
        return;
    }
    if (warn_due) {
        /* auto_arm_start marks the arm minute seen even when put off: a skipped day's arm
         * minute is no "no warning". */
        auto_arm_start(r, e, now, mark);
    }
}

void auto_arm_hold_check(auto_run_t *r, const auto_local_t *lt)
{
    if (lt == NULL || !lt->valid) {
        return;
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (!e->used || (e->arm.state != (uint8_t)AUTO_WARN_NIGHT &&
                         e->arm.state != (uint8_t)AUTO_WARN_DAY)) {
            continue;
        }
        /* Local date and minute: 6:00 AM and midnight are local whatever daylight saving does
         * (a date whose midnight is skipped ends at its first minute) */
        if (lt->day_no > e->arm.until_day ||
            (lt->day_no == e->arm.until_day && lt->minute >= e->arm.until_minute)) {
            auto_arm_end(r, e);
        }
    }
}

/* The partition was armed during a warning - by a person, or by another automation: the
 * warning ends now (no banner or chime over their exit time) and is noted "already armed", as
 * its arm time would have found it. A put-off hold stays. */
void auto_arm_partition_armed(auto_run_t *r, uint8_t partition, uint64_t now)
{
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (e->used && e->arm.state == (uint8_t)AUTO_WARN_ON && auto_arm_is_arm_rule(&e->rule) &&
            e->rule.then[0].partition == partition) {
            auto_arm_end(r, e);
            (void)status(r, e, 0u, AUTO_RES_ALREADY, now);
        }
    }
}

void auto_arm_tick(auto_run_t *r, auto_entry_t *e, uint64_t now)
{
    switch (e->arm.state) {
    case AUTO_WARN_ON: {
        const uint64_t g = gap_end(r, e->rule.then[0].partition);
        if (now >= e->arm.arm_at && now >= g) {
            do_arm(r, e, now);
        }
        break;
    }
    case AUTO_WARN_HOUR:
        if (now >= e->arm.rewarn_at) {
            e->arm.state = (uint8_t)AUTO_WARN_ON; /* D20: the warning again, 5 minutes */
            e->arm.user = 0u; /* the put-off line goes when the new warning starts */
            if (e->arm.arm_at < now + AUTO_REWARN_MS) {
                e->arm.arm_at = now + AUTO_REWARN_MS; /* a late tick never shortens it */
            }
            publish(r, e);
        }
        break;
    case AUTO_WARN_NIGHT:
    case AUTO_WARN_DAY:
        if (now >= e->arm.hold_cap) {
            auto_arm_end(r, e);
        }
        break;
    default:
        break;
    }
    if (e->arm.disarm_wait && e->arm.disarm_idx < e->rule.n &&
        now >= gap_end(r, e->rule.then[e->arm.disarm_idx].partition)) {
        do_disarm(r, e, now);
    }
}

static uint64_t at_least(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

uint64_t auto_arm_deadline(const auto_run_t *r, const auto_entry_t *e, uint64_t d)
{
    uint64_t t = UINT64_MAX;
    switch (e->arm.state) {
    case AUTO_WARN_ON:
        t = at_least(e->arm.arm_at, gap_end(r, e->rule.then[0].partition));
        break;
    case AUTO_WARN_HOUR:
        t = e->arm.rewarn_at;
        break;
    case AUTO_WARN_NIGHT:
    case AUTO_WARN_DAY:
        t = e->arm.hold_cap;
        break;
    default:
        break;
    }
    if (t < d) {
        d = t;
    }
    if (e->arm.disarm_wait && e->arm.disarm_idx < e->rule.n) {
        t = at_least(e->arm.disarm_at, gap_end(r, e->rule.then[e->arm.disarm_idx].partition));
        if (t < d) {
            d = t;
        }
    }
    return d;
}

bool auto_arm_own_occurrence(auto_run_t *r, const auto_occ_t *o)
{
    const uint8_t b = part_bit(o->partition);
    bool own = o->by_auto != 0u;
    if (o->kind == (uint8_t)AUTO_TRIG_ARMED) {
        own = own || (r->own_arm & b) != 0u;
        r->own_arm = (uint8_t)(r->own_arm & (uint8_t)~b);
    } else if (o->kind == (uint8_t)AUTO_TRIG_DISARMED) {
        own = own || (r->own_disarm & b) != 0u;
        /* a disarm ends whatever arming was coming: a later ARMED is someone else's */
        r->own_disarm = (uint8_t)(r->own_disarm & (uint8_t)~b);
        r->own_arm = (uint8_t)(r->own_arm & (uint8_t)~b);
    }
    return own;
}

auto_put_off_t auto_run_put_off(auto_run_t *r, uint16_t uid, uint8_t choice, uint8_t user,
                                bool may_disarm, const auto_local_t *lt, uint64_t now_ms)
{
    if (r == NULL || user == 0u ||
        (choice != (uint8_t)AUTO_PUT_OFF_HOUR && choice != (uint8_t)AUTO_PUT_OFF_NIGHT)) {
        return AUTO_POFF_BAD_ARG;
    }
    auto_entry_t *e = NULL;
    for (size_t i = 0; uid != 0u && i < AUTO_MAX; i++) {
        if (r->e[i].used && r->e[i].rule.uid == uid) {
            e = &r->e[i];
        }
    }
    if (e == NULL || e->arm.state != (uint8_t)AUTO_WARN_ON) {
        return AUTO_POFF_NO_WARNING;
    }
    const auto_action_t *a = &e->rule.then[0];
    int64_t until_epoch = 0;
    uint8_t night = 0u;
    if (choice == (uint8_t)AUTO_PUT_OFF_HOUR) {
        /* AUT-013, D20: the planned time + 60 minutes; the warning again 5 minutes before */
        e->arm.arm_at += AUTO_PUT_OFF_HOUR_MS;
        e->arm.rewarn_at = e->arm.arm_at - AUTO_REWARN_MS;
        e->arm.state = (uint8_t)AUTO_WARN_HOUR;
        until_epoch = auto_run_epoch_ms(r, e->arm.arm_at);
    } else {
        /* Decided 2026-10-05: only a code that may disarm that partition */
        if (!may_disarm) {
            return AUTO_POFF_NOT_ALLOWED;
        }
        if (lt == NULL || !lt->valid) {
            return AUTO_POFF_NO_CLOCK;
        }
        /* The planned arm time in local minutes (at most a warning away, so one date ahead at
         * most; a daylight-saving change inside those minutes is not counted) */
        const uint64_t left = e->arm.arm_at > now_ms ? e->arm.arm_at - now_ms : 0u;
        const uint64_t left_min = (left + MIN_MS - 1u) / MIN_MS;
        const uint64_t total = (uint64_t)lt->minute + left_min;
        const int32_t arm_day = lt->day_no + (int32_t)(total / AUTO_MINUTES_PER_DAY);
        const unsigned arm_min = (unsigned)(total % AUTO_MINUTES_PER_DAY);
        if (arm_min >= NIGHT_FROM || arm_min < NIGHT_TO) {
            night = 1u; /* "Not tonight": before 6:00 AM */
            e->arm.until_day = arm_min >= NIGHT_FROM ? arm_day + 1 : arm_day;
            e->arm.until_minute = (uint16_t)NIGHT_TO;
            e->arm.state = (uint8_t)AUTO_WARN_NIGHT;
        } else {
            e->arm.until_day = arm_day + 1; /* "Not today": until midnight */
            e->arm.until_minute = 0u;
            e->arm.state = (uint8_t)AUTO_WARN_DAY;
        }
        /* Local minutes from now to the end, plus slack: the latest the hold lasts if the clock
         * stops being valid (a valid reading ends it on the local time first) */
        const int64_t to_end = ((int64_t)e->arm.until_day - lt->day_no) *
                                   (int64_t)AUTO_MINUTES_PER_DAY +
                               (int64_t)e->arm.until_minute - (int64_t)lt->minute;
        e->arm.hold_cap = now_ms + (uint64_t)(to_end > 0 ? to_end : 0) * MIN_MS + HOLD_SLACK_MS;
    }
    e->arm.user = user;
    auto_status_t *st = status(r, e, 0u, AUTO_RES_PUT_OFF, now_ms);
    st->user = user;
    st->choice = choice;
    st->night = night;
    st->until_epoch_ms = until_epoch;
    publish(r, e);
    if (r->port.put_off != NULL) {
        r->port.put_off(r->port.ctx, uid, a->partition, user, choice, until_epoch);
    }
    return AUTO_POFF_OK;
}

void auto_run_warning(const auto_run_t *r, uint16_t uid, auto_warning_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; r != NULL && uid != 0u && i < AUTO_MAX; i++) {
        const auto_entry_t *e = &r->e[i];
        if (e->used && e->rule.uid == uid && e->arm.state != (uint8_t)AUTO_WARN_NONE) {
            fill_warning(r, e, out);
            return;
        }
    }
}

bool auto_run_wants_clock(const auto_run_t *r)
{
    if (auto_panel_wants_clock(r)) {
        return true; /* BL-224: a reminder's note ends at 6:00 AM */
    }
    for (size_t i = 0; r != NULL && i < AUTO_MAX; i++) {
        if (r->e[i].used && (r->e[i].arm.state == (uint8_t)AUTO_WARN_NIGHT ||
                             r->e[i].arm.state == (uint8_t)AUTO_WARN_DAY)) {
            return true;
        }
    }
    return false;
}
