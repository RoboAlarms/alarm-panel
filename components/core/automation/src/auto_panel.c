/* The runner's actions that need no device (BL-224, docs/features/41 DESIGN.md section 15): a
 * wired output, the door chime, a notification, a reminder to arm and "still armed". See
 * auto_run.h "Without devices". */
#include <string.h>

#include "auto_run_int.h"

#include "home_control/home_control.h"

#define NOTE_ENDS_MINUTE   360u                   /* 06:00: a reminder's note goes */
#define NOTIFY_GAP_MS      UINT64_C(60000)        /* one notification a minute per automation */
#define NOTE_CAP_MS        (UINT64_C(24) * UINT64_C(3600000)) /* without a clock: a day at most */

bool auto_panel_kind(uint8_t kind)
{
    return kind == (uint8_t)AUTO_ACT_OUTPUT || kind == (uint8_t)AUTO_ACT_REMIND ||
           kind == (uint8_t)AUTO_ACT_CHECK_ARMED || kind == (uint8_t)AUTO_ACT_CHIME ||
           kind == (uint8_t)AUTO_ACT_NOTIFY;
}

/* ---- wired outputs ------------------------------------------------------------------------- */

static bool out_ok(uint8_t output)
{
    return output >= 1u && output <= AUTO_OUTPUTS;
}

/* The 10 s gap (AUT-007) on one output: when it may be switched again, or now. */
static uint64_t out_free_at(const auto_run_t *r, uint8_t output, uint64_t now)
{
    const unsigned b = (unsigned)output - 1u;
    if ((r->out_used & (1u << b)) == 0u) {
        return now;
    }
    const uint64_t at = r->out_at[b] + AUTO_DEVICE_GAP_MS;
    return at > now ? at : now;
}

static void out_note(auto_run_t *r, uint8_t output, uint64_t now)
{
    const unsigned b = (unsigned)output - 1u;
    r->out_at[b] = now;
    r->out_used = (uint8_t)(r->out_used | (1u << b));
}

/* Action idx's output send: now, or once the gap allows. */
static void out_send(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint64_t now)
{
    auto_panel_t *p = &e->pn;
    const auto_action_t *a = &e->rule.then[idx];
    const uint8_t tried = p->wait_tried[idx];
    if (!out_ok(a->output)) {
        p->waiting[idx] = 0u;
        auto_run_finish(r, e, idx, AUTO_RES_CANT_RUN, tried, now);
        return;
    }
    const uint64_t free_at = out_free_at(r, a->output, now);
    if (free_at > now) {
        p->waiting[idx] = 1u;
        p->wait_at[idx] = free_at;
        return;
    }
    if (auto_run_cap_full(r, now)) {
        p->waiting[idx] = 0u;
        auto_run_finish(r, e, idx, AUTO_RES_HELD_BACK, tried, now);
        return;
    }
    if (r->port.output != NULL && r->port.send_begin != NULL &&
        !r->port.send_begin(r->port.ctx, e->rule.uid)) {
        /* PL04: changed, turned off or deleted since the rules came in: wait for them */
        auto_run_send_end(r);
        p->waiting[idx] = 1u;
        p->wait_at[idx] = now;
        return;
    }
    p->waiting[idx] = 0u;
    bool ok = false;
    if (r->port.output != NULL) {
        ok = r->port.output(r->port.ctx, a->output, a->dev_action == (uint8_t)ACT_ON);
        if (r->port.send_begin != NULL) {
            auto_run_send_end(r);
        }
        out_note(r, a->output, now);
        auto_run_cap_note(r, now);
    }
    if (!ok) {
        auto_run_finish(r, e, idx, AUTO_RES_UNREACHABLE, tried, now);
        return;
    }
    auto_run_finish(r, e, idx, AUTO_RES_DONE, tried, now);
    /* "Turn it off after", as a device's: a new On restarts it, an On without one stops it */
    if (a->dev_action == (uint8_t)ACT_ON && a->off_after_min > 0u) {
        p->off_running[idx] = 1u;
        p->off_waiting[idx] = 0u;
        p->off_output[idx] = a->output;
        p->off_tried[idx] = tried;
        p->off_at[idx] = now + (uint64_t)a->off_after_min * UINT64_C(60000);
    } else if (p->off_running[idx] && p->off_output[idx] == a->output) {
        p->off_running[idx] = 0u;
    }
}

/* An output's Off when its time is up: never lost, never held back by the cap. */
static void off_send(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint64_t now)
{
    auto_panel_t *p = &e->pn;
    const uint8_t output = p->off_output[idx];
    const uint64_t free_at = out_free_at(r, output, now);
    if (free_at > now) {
        p->off_waiting[idx] = 1u;
        p->off_at[idx] = free_at;
        return;
    }
    p->off_waiting[idx] = 0u;
    p->off_running[idx] = 0u;
    /* An Off only ever turns something off: it goes whatever the edit gate says (PL04's rule
     * for a running off timer) */
    if (r->port.output != NULL) {
        (void)r->port.output(r->port.ctx, output, false);
        out_note(r, output, now);
        auto_run_cap_note(r, now);
    }
}

/* ---- notifications, the chime -------------------------------------------------------------- */

static uint8_t notify(auto_run_t *r, auto_entry_t *e, const auto_notice_t *n, uint64_t now)
{
    auto_panel_t *p = &e->pn;
    if (p->notified && now >= p->notify_at && now - p->notify_at < NOTIFY_GAP_MS && !n->tried) {
        return (uint8_t)AUTO_RES_NONE; /* one a minute: this one is dropped */
    }
    if (r->port.notify == NULL || !r->port.notify(r->port.ctx, n)) {
        return (uint8_t)AUTO_RES_NO_SERVICE;
    }
    p->notified = 1u;
    p->notify_at = now;
    return (uint8_t)AUTO_RES_DONE;
}

static void notice_of(const auto_entry_t *e, const auto_occ_t *o, uint8_t tried, auto_notice_t *n)
{
    memset(n, 0, sizeof(*n));
    n->uid = e->rule.uid;
    n->what = (uint8_t)AUTO_NOTICE_EVENT;
    n->tried = tried;
    n->trig = e->rule.when.kind;
    n->zone = e->rule.when.zone;
    n->minute = e->rule.when.minute;
    n->cause = e->rule.when.kind == (uint8_t)AUTO_TRIG_TROUBLE ? e->rule.when.trouble : 0u;
    if (o != NULL) {
        n->partition = o->partition;
        n->level = o->level;
        n->cause = o->cause;
        if (o->zone != 0u) {
            n->zone = o->zone;
        }
    }
}

static void chime(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint8_t tried, uint64_t now)
{
    const auto_action_t *a = &e->rule.then[idx];
    if (r->port.chime == NULL) {
        auto_run_finish(r, e, idx, AUTO_RES_UNREACHABLE, tried, now);
        return;
    }
    if (!auto_run_send_begin(r, e->rule.uid)) {
        return; /* PL04: the new version decides; this run is dropped */
    }
    bool already = false;
    const alarm_result_t res =
        r->port.chime(r->port.ctx, e->rule.uid, a->partition, a->option != 0u, e->rule.made_by,
                      &already);
    auto_run_send_end(r);
    if (res != ALARM_OK) {
        auto_run_finish(r, e, idx, AUTO_RES_ARM_REFUSED, tried, now);
        e->st.alarm_res = (uint8_t)res;
        return;
    }
    auto_run_finish(r, e, idx, already ? AUTO_RES_ALREADY : AUTO_RES_DONE, tried, now);
}

void auto_panel_run(auto_run_t *r, auto_entry_t *e, uint8_t idx, const auto_occ_t *o,
                    uint8_t tried, uint64_t now)
{
    const auto_action_t *a = &e->rule.then[idx];
    switch (a->kind) {
    case AUTO_ACT_OUTPUT:
        e->pn.wait_tried[idx] = tried;
        out_send(r, e, idx, now);
        return;
    case AUTO_ACT_CHIME:
        chime(r, e, idx, tried, now);
        return;
    case AUTO_ACT_NOTIFY: {
        auto_notice_t n;
        notice_of(e, o, tried, &n);
        const uint8_t res = notify(r, e, &n, now);
        if (res != (uint8_t)AUTO_RES_NONE) {
            auto_run_finish(r, e, idx, res, tried, now);
        }
        return;
    }
    default:
        return; /* a reminder and "still armed" run at their time only (auto_panel_time) */
    }
}

/* ---- Remind to arm, "still armed" ---------------------------------------------------------- */

static void note_end(auto_run_t *r, auto_entry_t *e)
{
    auto_panel_t *p = &e->pn;
    if (!p->remind_on) {
        return;
    }
    p->remind_on = 0u;
    if (r->port.remind != NULL) {
        const auto_remind_t m = {e->rule.uid, p->remind_part, 0u, e->rule.when.minute};
        r->port.remind(r->port.ctx, &m);
    }
}

void auto_panel_time(auto_run_t *r, auto_entry_t *e, const auto_span_t *s, uint64_t now)
{
    if (e->rule.n < 1u) {
        return;
    }
    const auto_action_t *a = &e->rule.then[0];
    const int armed = r->port.part_armed != NULL ? r->port.part_armed(r->port.ctx, a->partition)
                                                 : -1;
    if (armed < 0) {
        auto_run_finish(r, e, 0u, AUTO_RES_CANT_RUN, 0u, now);
        return;
    }
    auto_notice_t n;
    notice_of(e, NULL, 0u, &n);
    n.partition = a->partition;
    if (a->kind == (uint8_t)AUTO_ACT_CHECK_ARMED) {
        if (armed == 0) {
            auto_run_finish(r, e, 0u, AUTO_RES_ALREADY, 0u, now); /* disarmed: nothing to say */
            return;
        }
        n.what = (uint8_t)AUTO_NOTICE_STILL_ARMED;
        const uint8_t res = notify(r, e, &n, now);
        auto_run_finish(r, e, 0u, res == (uint8_t)AUTO_RES_NONE ? (uint8_t)AUTO_RES_DONE : res, 0u,
                        now);
        return;
    }
    if (a->kind != (uint8_t)AUTO_ACT_REMIND) {
        return;
    }
    if (armed != 0) {
        auto_run_finish(r, e, 0u, AUTO_RES_ALREADY, 0u, now);
        return;
    }
    auto_panel_t *p = &e->pn;
    p->remind_on = 1u;
    p->remind_part = a->partition;
    /* the note stands until 6:00 AM: the same date's for a reminder before it, else the next */
    const unsigned minute = e->rule.when.minute;
    p->remind_day = minute < NOTE_ENDS_MINUTE ? s->day_no : s->day_no + 1;
    p->remind_minute = (uint16_t)NOTE_ENDS_MINUTE;
    p->note_cap = now + NOTE_CAP_MS;
    if (r->port.remind != NULL) {
        const auto_remind_t m = {e->rule.uid, a->partition, 1u, (uint16_t)minute};
        r->port.remind(r->port.ctx, &m);
    }
    if (a->option != 0u) {
        n.what = (uint8_t)AUTO_NOTICE_REMIND;
        (void)notify(r, e, &n, now);
    }
    auto_run_finish(r, e, 0u, AUTO_RES_DONE, 0u, now);
}

void auto_panel_partition_armed(auto_run_t *r, uint8_t partition)
{
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (e->used && e->pn.remind_on && e->pn.remind_part == partition) {
            note_end(r, e);
        }
    }
}

void auto_panel_hold_check(auto_run_t *r, const auto_local_t *lt)
{
    if (lt == NULL || !lt->valid) {
        return;
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (!e->used || !e->pn.remind_on) {
            continue;
        }
        if (lt->day_no > e->pn.remind_day ||
            (lt->day_no == e->pn.remind_day && lt->minute >= e->pn.remind_minute)) {
            note_end(r, e);
        }
    }
}

bool auto_panel_wants_clock(const auto_run_t *r)
{
    for (size_t i = 0; r != NULL && i < AUTO_MAX; i++) {
        if (r->e[i].used && r->e[i].pn.remind_on) {
            return true;
        }
    }
    return false;
}

/* ---- the task's calls ---------------------------------------------------------------------- */

void auto_panel_tick(auto_run_t *r, auto_entry_t *e, uint64_t now)
{
    auto_panel_t *p = &e->pn;
    for (uint8_t i = 0; i < AUTO_ACTIONS; i++) {
        if (p->waiting[i] && now >= p->wait_at[i] && i < e->rule.n &&
            e->rule.then[i].kind == (uint8_t)AUTO_ACT_OUTPUT) {
            out_send(r, e, i, now);
        }
        if ((p->off_running[i] || p->off_waiting[i]) && now >= p->off_at[i]) {
            off_send(r, e, i, now);
        }
    }
    if (p->remind_on && now >= p->note_cap) {
        note_end(r, e); /* no clock to end it by: a day at most */
    }
}

uint64_t auto_panel_deadline(const auto_entry_t *e, uint64_t d)
{
    const auto_panel_t *p = &e->pn;
    for (size_t i = 0; i < AUTO_ACTIONS; i++) {
        if (p->waiting[i] && p->wait_at[i] < d) {
            d = p->wait_at[i];
        }
        if ((p->off_running[i] || p->off_waiting[i]) && p->off_at[i] < d) {
            d = p->off_at[i];
        }
    }
    if (p->remind_on && p->note_cap < d) {
        d = p->note_cap;
    }
    return d;
}

void auto_panel_end(auto_run_t *r, auto_entry_t *e, bool keep_timers)
{
    auto_panel_t *p = &e->pn;
    note_end(r, e);
    for (size_t i = 0; i < AUTO_ACTIONS; i++) {
        p->waiting[i] = 0u;
        if (!keep_timers) {
            p->off_running[i] = 0u;
            p->off_waiting[i] = 0u;
        }
    }
}
