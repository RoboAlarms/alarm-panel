/* The runner: occurrences and times matched against the automations, actions sent and their
 * outcomes (docs/features/41 DESIGN.md sections 2, 3 and 5; AUT-006, AUT-007, HCTL-009,
 * AUT-015). See auto_run.h. */
#include "automation/auto_run.h"

#include <string.h>

#include "auto_run_int.h"

#include "home_control/home_control.h"

static auto_entry_t *find(auto_run_t *r, uint16_t uid)
{
    if (uid == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        if (r->e[i].used && r->e[i].rule.uid == uid) {
            return &r->e[i];
        }
    }
    return NULL;
}

static const auto_entry_t *find_c(const auto_run_t *r, uint16_t uid)
{
    for (size_t i = 0; uid != 0u && i < AUTO_MAX; i++) {
        if (r->e[i].used && r->e[i].rule.uid == uid) {
            return &r->e[i];
        }
    }
    return NULL;
}

static void note_clock(auto_run_t *r, const auto_local_t *lt, uint64_t now)
{
    if (lt == NULL) {
        return;
    }
    if (lt->valid) {
        r->epoch_base_ms = lt->epoch_s * 1000 - (int64_t)now;
        r->have_epoch = 1u;
    } else {
        r->have_epoch = 0u;
    }
}

static int64_t epoch_ms(const auto_run_t *r, uint64_t now)
{
    return r->have_epoch ? r->epoch_base_ms + (int64_t)now : 0;
}

int64_t auto_run_epoch_ms(const auto_run_t *r, uint64_t now)
{
    return epoch_ms(r, now);
}

static uint8_t fail_reason(uint8_t res)
{
    switch (res) {
    case AUTO_RES_UNREACHABLE: return (uint8_t)ALARM_AUTO_FAIL_UNREACHABLE;
    case AUTO_RES_NO_ANSWER: return (uint8_t)ALARM_AUTO_FAIL_NO_ANSWER;
    case AUTO_RES_REFUSED: return (uint8_t)ALARM_AUTO_FAIL_REFUSED;
    case AUTO_RES_MISSING: return (uint8_t)ALARM_AUTO_FAIL_MISSING;
    case AUTO_RES_BUSY: return (uint8_t)ALARM_AUTO_FAIL_BUSY;
    case AUTO_RES_NOT_ALLOWED: return (uint8_t)ALARM_AUTO_FAIL_NOT_ALLOWED;
    default: return 0u;
    }
}

/* An action's outcome: the status (last outcome wins) and, for a Lock or Close that didn't
 * happen, the failed port at most once an hour per automation unless the reason changes. */
/* AUT-009: the failed port hands a dev_action_t on as ALARM_EV_AUTOMATION_FAILED's action
 * (arg2); alarm_core names those numbers itself, as it can't include home_control. */
_Static_assert(ALARM_AUTO_ACT_ARM == (unsigned)ACT_NONE, "arming is ACT_NONE");
_Static_assert(ALARM_AUTO_ACT_LOCK == (unsigned)ACT_LOCK, "ALARM_AUTO_ACT_LOCK is ACT_LOCK");
_Static_assert(ALARM_AUTO_ACT_CLOSE == (unsigned)ACT_CLOSE, "ALARM_AUTO_ACT_CLOSE is ACT_CLOSE");
_Static_assert(ALARM_AUTO_ACT_LIMIT <= (unsigned)ACT_COUNT, "the limit is a dev_action_t");

static void finish(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint8_t dev_action, uint16_t zone,
                   uint8_t res, uint8_t tried, uint64_t now)
{
    const uint8_t reason = fail_reason(res);
    memset(&e->st, 0, sizeof(e->st)); /* phase C's fields too */
    e->st.result = res;
    e->st.action_index = idx;
    e->st.reason = reason;
    e->st.tried = tried; /* AUT-006: Try it now's outcome, or a trigger's */
    e->st.at_epoch_ms = epoch_ms(r, now);
    const bool lock_close = dev_action == (uint8_t)ACT_LOCK || dev_action == (uint8_t)ACT_CLOSE;
    const bool reported = res == AUTO_RES_REFUSED || res == AUTO_RES_NO_ANSWER ||
                          res == AUTO_RES_UNREACHABLE || res == AUTO_RES_MISSING ||
                          res == AUTO_RES_BUSY || res == AUTO_RES_NOT_ALLOWED;
    if (lock_close && reported) {
        auto_run_report_failed(r, e, reason, dev_action, zone, now);
    }
}

void auto_run_finish(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint8_t res, uint8_t tried,
                     uint64_t now)
{
    finish(r, e, idx, 0u, 0u, res, tried, now);
}

void auto_run_report_failed(auto_run_t *r, auto_entry_t *e, uint8_t reason, uint8_t dev_action,
                            uint16_t zone, uint64_t now)
{
    if (r->port.failed == NULL) {
        return;
    }
    if (e->failed_any && e->failed_reason == reason &&
        (now < e->failed_ms || now - e->failed_ms < AUTO_FAIL_DEDUP_MS)) {
        return;
    }
    e->failed_any = 1u;
    e->failed_reason = reason;
    e->failed_ms = now;
    r->port.failed(r->port.ctx, e->rule.uid, reason, dev_action, zone);
}

static void send_done(auto_run_t *r, auto_entry_t *e, uint8_t i, auto_send_t *s, uint8_t res,
                      uint64_t now)
{
    s->waiting = 0u;
    s->held = 0u;
    finish(r, e, i, s->act.dev_action, s->zone, res, s->tried, now);
}

static bool same_dev(const auto_action_t *a, const auto_action_t *b)
{
    return a->source == b->source && strncmp(a->ref, b->ref, DEV_REF_LEN) == 0;
}

/* ---- the panel-wide cap and the device gap (AUT-007) --------------------------------------- */

/* A now older than the oldest send counts as inside the window: a caller's step back never
 * lifts the cap. */
static bool cap_full(const auto_run_t *r, uint64_t now)
{
    const uint64_t t = r->cap[r->cap_next];
    return r->cap_used >= AUTO_CAP_ACTIONS && (now < t || now - t < AUTO_CAP_WINDOW_MS);
}

static uint64_t cap_free_at(const auto_run_t *r)
{
    return r->cap[r->cap_next] + AUTO_CAP_WINDOW_MS;
}

static void cap_note(auto_run_t *r, uint64_t now)
{
    r->cap[r->cap_next] = now;
    r->cap_next = (uint8_t)((r->cap_next + 1u) % AUTO_CAP_ACTIONS);
    if (r->cap_used < AUTO_CAP_ACTIONS) {
        r->cap_used++;
    }
}

bool auto_run_cap_full(const auto_run_t *r, uint64_t now)
{
    return cap_full(r, now);
}

void auto_run_cap_note(auto_run_t *r, uint64_t now)
{
    cap_note(r, now);
}

static auto_gap_t *gap_find(auto_run_t *r, uint8_t source, const char *ref)
{
    for (size_t i = 0; i < AUTO_RUN_GAPS; i++) {
        if (r->gap[i].used && r->gap[i].source == source && strcmp(r->gap[i].ref, ref) == 0) {
            return &r->gap[i];
        }
    }
    return NULL;
}

static void gap_note(auto_run_t *r, uint8_t source, const char *ref, uint64_t now)
{
    auto_gap_t *g = gap_find(r, source, ref);
    if (g == NULL) {
        g = &r->gap[0];
        for (size_t i = 0; i < AUTO_RUN_GAPS; i++) {
            if (!r->gap[i].used) {
                g = &r->gap[i];
                break;
            }
            if (r->gap[i].at < g->at) {
                g = &r->gap[i];
            }
        }
        g->used = 1u;
        g->source = source;
        memcpy(g->ref, ref, sizeof(g->ref));
        g->ref[DEV_REF_LEN - 1u] = '\0';
    }
    g->at = now;
}

/* ---- sends awaiting a result --------------------------------------------------------------- */

static void flight_end(auto_run_t *r, auto_flight_t *f, uint8_t res, uint64_t now)
{
    f->used = 0u;
    auto_entry_t *e = find(r, f->uid);
    if (e != NULL) {
        finish(r, e, f->idx, f->dev_action, f->zone, res, f->tried, now);
    }
}

static void flight_add(auto_run_t *r, const auto_entry_t *e, uint8_t i, const auto_send_t *s,
                       uint32_t req, uint64_t now)
{
    auto_flight_t *f = NULL;
    for (size_t k = 0; k < AUTO_RUN_FLIGHTS; k++) {
        if (!r->flight[k].used) {
            f = &r->flight[k];
            break;
        }
        if (f == NULL || r->flight[k].deadline < f->deadline) {
            f = &r->flight[k];
        }
    }
    if (f->used) {
        /* More awaiting than the cap allows: the oldest is given up on. */
        flight_end(r, f, AUTO_RES_NO_ANSWER, now);
    }
    f->used = 1u;
    f->req = req;
    f->uid = e->rule.uid;
    f->idx = i;
    f->zone = s->zone;
    f->dev_action = s->act.dev_action;
    f->tried = s->tried;
    f->deadline = now + AUTO_RESULT_WAIT_MS;
}

/* ---- the revocation gate (PL04) ------------------------------------------------------------ */

bool auto_run_send_begin(auto_run_t *r, uint16_t uid)
{
    if (r->port.send_begin == NULL) {
        return true;
    }
    const bool go = r->port.send_begin(r->port.ctx, uid);
    if (!go) {
        auto_run_send_end(r);
    }
    return go;
}

void auto_run_send_end(auto_run_t *r)
{
    if (r->port.send_end != NULL) {
        r->port.send_end(r->port.ctx);
    }
}

void auto_revoke_clear(auto_revoke_t *v)
{
    memset(v, 0, sizeof(*v));
}

void auto_revoke_add(auto_revoke_t *v, uint16_t uid)
{
    if (v->all || auto_revoke_has(v, uid)) {
        return;
    }
    if (uid == 0u || v->n >= AUTO_MAX) {
        v->all = 1u; /* can't name it, or no room: every automation waits for the rules */
        return;
    }
    v->uid[v->n++] = uid;
}

bool auto_revoke_has(const auto_revoke_t *v, uint16_t uid)
{
    if (v->all) {
        return true;
    }
    for (size_t i = 0; i < v->n && i < AUTO_MAX; i++) {
        if (v->uid[i] == uid) {
            return true;
        }
    }
    return false;
}

/* ---- sending ------------------------------------------------------------------------------- */

/* The action's current device, when it is a device action. */
static const auto_action_t *rule_dev(const auto_entry_t *e, size_t i)
{
    if (i >= e->rule.n || e->rule.then[i].kind != (uint8_t)AUTO_ACT_DEVICE) {
        return NULL;
    }
    return &e->rule.then[i];
}

/* A new run of action i restarts its running timer only while the action still has a "Turn it
 * off after" and the timer is for the action's current device; the length is the action's own
 * now (an edit may have changed it), and its Off now belongs to that run: tried is the run's
 * (AUT-006). One for a device an edit replaced keeps its own end, and one whose action lost its
 * "Turn it off after" ends when it was due (unless an On without a timer is sent to that device:
 * attempt stops it). */
static void off_restart_one(auto_entry_t *e, size_t i, uint64_t now, uint8_t tried)
{
    const auto_action_t *a = rule_dev(e, i);
    auto_offtimer_t *t = &e->off[i];
    if (t->running && a != NULL && a->off_after_min > 0u && same_dev(&t->s.act, a)) {
        t->len_ms = (uint64_t)a->off_after_min * UINT64_C(60000);
        t->at = now + t->len_ms;
        t->s.tried = tried;
    }
}

/* A repeat inside the cooldown: each action's timer, as off_restart_one says. */
static void off_restart(auto_entry_t *e, uint64_t now, uint8_t tried)
{
    for (size_t i = 0; i < AUTO_ACTIONS; i++) {
        off_restart_one(e, i, now, tried);
    }
}

static void off_start(auto_entry_t *e, uint8_t i, const auto_send_t *on, uint64_t now)
{
    auto_offtimer_t *t = &e->off[i];
    memset(&t->s, 0, sizeof(t->s));
    t->s.act = on->act;
    t->s.act.dev_action = (uint8_t)ACT_OFF;
    t->s.act.value = -1;
    t->s.act.mode[0] = '\0';
    t->s.act.off_after_min = 0u;
    t->s.zone = on->zone;
    t->s.tried = on->tried; /* the Off of a tried On is part of the try */
    t->len_ms = (uint64_t)on->act.off_after_min * UINT64_C(60000);
    t->at = now + t->len_ms;
    t->running = 1u;
}

/* The timer ended (or must end now): the Off becomes a send of its own. */
static void off_due(auto_offtimer_t *t, uint64_t now)
{
    t->running = 0u;
    t->s.waiting = 1u;
    t->s.held = 0u;
    t->s.next_ms = now;
}

/* An On that would start a timer while the action's slot still holds another device's (an edit
 * changed the device): that device's Off falls due at once, and the On waits until it has gone. */
static bool off_slot_taken(auto_entry_t *e, uint8_t i, const auto_action_t *a, uint64_t now)
{
    auto_offtimer_t *t = &e->off[i];
    if ((!t->running && !t->s.waiting) || same_dev(&t->s.act, a)) {
        return false;
    }
    if (t->running) {
        off_due(t, now);
    }
    return true;
}

/* One try of a waiting send (an action's, or an Off): the live kind, the gap, the cap, then the
 * port. */
static void attempt(auto_run_t *r, auto_entry_t *e, uint8_t i, bool is_off, uint64_t now)
{
    auto_send_t *s = is_off ? &e->off[i].s : &e->job[i].s;
    if (!s->waiting || now < s->next_ms) {
        return;
    }
    const auto_action_t *a = &s->act;
    if (r->port.device_kind == NULL) {
        /* AUT-002 layer 4 can't be checked: nothing is sent. */
        send_done(r, e, i, s, AUTO_RES_CANT_RUN, now);
        return;
    }
    uint8_t kind = 0u;
    const bool listed = r->port.device_kind(r->port.ctx, a->source, a->ref, &kind);
    if (listed && kind != a->dev_kind) {
        /* AUT-002 layer 4: the ref names another kind of device now. */
        send_done(r, e, i, s, AUTO_RES_CANT_RUN, now);
        return;
    }
    if (!is_off && hc_auto_less_secure((dev_kind_t)a->dev_kind, (dev_action_t)a->dev_action,
                                       a->value)) {
        /* AUT-015 layer 4: the live kind is the stored one here (or the ref isn't listed). The
         * option and the maker are read live, together: an env not yet refreshed after the
         * installer turned the option off never lets this send go. */
        bool option = r->less_secure != 0u;
        bool maker = false;
        if (option && r->port.maker_ok != NULL) {
            maker = r->port.maker_ok(r->port.ctx, e->rule.made_by, &option);
        }
        if (!option) {
            send_done(r, e, i, s, AUTO_RES_LESS_SECURE_OFF, now);
            return;
        }
        if (!maker) {
            send_done(r, e, i, s, AUTO_RES_MAKER_GONE, now);
            return;
        }
    }
    int res;
    if (!listed) {
        res = (int)HC_ERR_UNKNOWN;
    } else {
        const auto_gap_t *g = gap_find(r, a->source, a->ref);
        if (g != NULL && now < g->at + AUTO_DEVICE_GAP_MS) {
            s->next_ms = g->at + AUTO_DEVICE_GAP_MS;
            return;
        }
        const bool timed = !is_off && a->dev_action == (uint8_t)ACT_ON && a->off_after_min > 0u;
        if (timed && off_slot_taken(e, i, a, now)) {
            /* Bounded like a held send: at most 30 s from its first wait behind that Off. */
            if (!s->behind) {
                s->behind = 1u;
                s->behind_from = now;
            }
            if (now >= s->behind_from && now - s->behind_from >= AUTO_HOLD_MAX_MS) {
                send_done(r, e, i, s, cap_full(r, now) ? AUTO_RES_HELD_BACK : AUTO_RES_UNREACHABLE,
                          now);
                return;
            }
            s->next_ms = now + AUTO_HOLD_RETRY_MS;
            if (s->next_ms > s->behind_from + AUTO_HOLD_MAX_MS) {
                s->next_ms = s->behind_from + AUTO_HOLD_MAX_MS;
            }
            return;
        }
        if (cap_full(r, now)) {
            if (is_off) {
                /* An Off is never lost: it goes when the window allows. */
                s->next_ms = cap_free_at(r) > now ? cap_free_at(r) : now + AUTO_HOLD_RETRY_MS;
                return;
            }
            send_done(r, e, i, s, AUTO_RES_HELD_BACK, now);
            return;
        }
        if (!auto_run_send_begin(r, e->rule.uid)) {
            /* PL04: edited, turned off or deleted since the rules came in: wait for them */
            s->next_ms = now;
            return;
        }
        uint32_t req = 0u;
        res = r->port.device_act != NULL
                  ? r->port.device_act(r->port.ctx, a, r->less_secure != 0u, &req)
                  : (int)HC_ERR_OFFLINE;
        auto_run_send_end(r);
        if (res == (int)HC_OK) {
            /* AUT-006: sent; never sent again. */
            s->waiting = 0u;
            s->held = 0u;
            gap_note(r, a->source, a->ref, now);
            cap_note(r, now);
            flight_add(r, e, i, s, req, now);
            if (!is_off && a->dev_action == (uint8_t)ACT_ON) {
                auto_offtimer_t *t = &e->off[i];
                if (t->s.waiting && same_dev(&t->s.act, a)) {
                    /* A newer On went after the Off fell due: its own timer owns the device. */
                    t->s.waiting = 0u;
                    t->s.held = 0u;
                }
                if (a->off_after_min > 0u) {
                    off_start(e, i, s, now);
                } else if (t->running && same_dev(&t->s.act, a)) {
                    /* An On without a timer: the newest rule says "stay on". */
                    t->running = 0u;
                }
            }
            return;
        }
    }
    switch (res) {
    case HC_ERR_BUSY:
        send_done(r, e, i, s, AUTO_RES_BUSY, now);
        return;
    case HC_ERR_ARG:
        send_done(r, e, i, s, AUTO_RES_CANT_RUN, now);
        return;
    case HC_ERR_NOT_ALLOWED:
        send_done(r, e, i, s, AUTO_RES_NOT_ALLOWED, now);
        return;
    case HC_ERR_OFFLINE:
    case HC_ERR_UNKNOWN: {
        const bool absent = !listed || res == (int)HC_ERR_UNKNOWN;
        if (a->source != (uint8_t)DSRC_HA) {
            send_done(r, e, i, s, absent ? AUTO_RES_MISSING : AUTO_RES_UNREACHABLE, now);
            return;
        }
        if (!s->held) {
            s->held = 1u;
            s->held_from = now;
        }
        if (now >= s->held_from && now - s->held_from >= AUTO_HOLD_MAX_MS) {
            const bool settled = r->port.source_settled != NULL &&
                                 r->port.source_settled(r->port.ctx, a->source);
            send_done(r, e, i, s, absent && settled ? AUTO_RES_MISSING : AUTO_RES_UNREACHABLE, now);
            return;
        }
        s->next_ms = now + AUTO_HOLD_RETRY_MS;
        if (s->next_ms > s->held_from + AUTO_HOLD_MAX_MS) {
            s->next_ms = s->held_from + AUTO_HOLD_MAX_MS;
        }
        return;
    }
    default:
        send_done(r, e, i, s, AUTO_RES_UNREACHABLE, now);
        return;
    }
}

/* The automation's due sends: its actions in order, then its Offs. */
static void attempt_all(auto_run_t *r, auto_entry_t *e, uint64_t now)
{
    for (uint8_t i = 0; i < AUTO_ACTIONS; i++) {
        attempt(r, e, i, false, now);
    }
    for (uint8_t i = 0; i < AUTO_ACTIONS; i++) {
        attempt(r, e, i, true, now);
    }
}

/* Start the automation's device actions, in order; a newer one replaces an older held send (an
 * Off that fell due is a send of its own and stays). */
static void run_rule(auto_run_t *r, auto_entry_t *e, uint8_t occ_kind, uint8_t occ_part,
                     uint16_t zone, bool alarm_ok, const auto_occ_t *o, uint64_t now)
{
    const uint8_t n = e->rule.n <= AUTO_ACTIONS ? e->rule.n : (uint8_t)AUTO_ACTIONS;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t kind = e->rule.then[i].kind;
        if (kind == (uint8_t)AUTO_ACT_ARM || kind == (uint8_t)AUTO_ACT_DISARM) {
            /* AUT-007: an automation's own arm or disarm starts no other (one hop); Try it
             * now never runs one */
            if (!alarm_ok || occ_kind == (uint8_t)AUTO_TRIG_NONE) {
                continue;
            }
            if (kind == (uint8_t)AUTO_ACT_ARM) {
                auto_arm_start(r, e, now, AUTO_RUN_NONE);
            } else {
                auto_arm_disarm_start(r, e, i, now);
            }
            continue;
        }
        if (auto_panel_kind(kind)) {
            /* BL-224: an output, the chime, a notification (a reminder waits for its time) */
            auto_panel_run(r, e, i, o, occ_kind == (uint8_t)AUTO_TRIG_NONE ? 1u : 0u, now);
            continue;
        }
        if (kind != (uint8_t)AUTO_ACT_DEVICE) {
            continue;
        }
        auto_job_t *j = &e->job[i];
        memset(j, 0, sizeof(*j));
        j->s.act = e->rule.then[i];
        j->s.act.ref[DEV_REF_LEN - 1u] = '\0';
        j->s.act.mode[DEV_MODE_LEN - 1u] = '\0';
        j->s.next_ms = now;
        j->s.zone = zone;
        j->s.waiting = 1u;
        j->occ_kind = occ_kind;
        j->occ_part = occ_part;
        j->s.tried = occ_kind == (uint8_t)AUTO_TRIG_NONE ? 1u : 0u; /* only auto_run_try runs
                                                                      * without an occurrence */
        off_restart_one(e, i, now, j->s.tried);
    }
    attempt_all(r, e, now);
}

/* ---- matching ------------------------------------------------------------------------------ */

static bool is_zone_trig(uint8_t kind)
{
    return kind == (uint8_t)AUTO_TRIG_ZONE_OPENED || kind == (uint8_t)AUTO_TRIG_ZONE_CLOSED;
}

static bool matches(const auto_run_t *r, const auto_rule_t *rule, const auto_occ_t *o)
{
    const auto_trigger_t *w = &rule->when;
    if (w->kind != o->kind || o->kind == (uint8_t)AUTO_TRIG_TIME ||
        o->kind == (uint8_t)AUTO_TRIG_NONE || o->kind >= AUTO_TRIG_COUNT) {
        return false;
    }
    uint8_t part = o->partition;
    if (is_zone_trig(o->kind)) {
        if (o->zone == 0u || o->zone > ALARM_MAX_ZONES || w->zone != o->zone) {
            return false;
        }
        const auto_zone_t *z = &r->zones[o->zone - 1u];
        if (!auto_zone_usable(z->type, z->dev_class, o->kind == (uint8_t)AUTO_TRIG_ZONE_CLOSED)) {
            return false;
        }
        part = z->partition;
    }
    if (w->partitions != 0u) {
        if (part == 0u || part > 8u || (w->partitions & (1u << (unsigned)(part - 1u))) == 0u) {
            return false;
        }
    }
    if (o->kind == (uint8_t)AUTO_TRIG_ARMED && w->levels != 0u &&
        (w->levels & ALARM_LEVEL_BIT(o->level)) == 0u) {
        return false;
    }
    /* BL-224: a trouble trigger's own trouble, or any */
    if (o->kind == (uint8_t)AUTO_TRIG_TROUBLE && w->trouble != (uint8_t)AUTO_TROUBLE_ANY &&
        w->trouble != o->cause) {
        return false;
    }
    return true;
}

/* The "Only between" window: 1 inside, 0 outside, -1 no clock. */
static int in_window(const auto_trigger_t *w, const auto_local_t *lt)
{
    if (w->win_from == 0u || w->win_to == 0u) {
        return 1;
    }
    if (lt == NULL || !lt->valid) {
        return -1;
    }
    const unsigned from = (unsigned)w->win_from - 1u;
    const unsigned to = (unsigned)w->win_to - 1u;
    const unsigned m = lt->minute;
    if (from < to) {
        return m >= from && m < to ? 1 : 0;
    }
    return m >= from || m < to ? 1 : 0;
}

/* ---- the calls ----------------------------------------------------------------------------- */

void auto_run_init(auto_run_t *r, const auto_ports_t *p, uint64_t now_ms)
{
    (void)now_ms;
    memset(r, 0, sizeof(*r));
    if (p != NULL) {
        r->port = *p;
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        r->e[i].last_run = AUTO_RUN_NONE;
        r->e[i].arm.arm_run = AUTO_RUN_NONE;
    }
}

/* An edit that changes what a warning announces, or what starts it, ends it (DESIGN.md 3.4). */
static bool arming_changed(const auto_rule_t *was, const auto_rule_t *now)
{
    if (was->n != now->n || was->when.kind != now->when.kind ||
        was->when.minute != now->when.minute || was->when.days != now->when.days ||
        was->when.zone != now->when.zone || was->when.partitions != now->when.partitions ||
        was->when.levels != now->when.levels) {
        return true;
    }
    for (size_t i = 0; i < was->n && i < AUTO_ACTIONS; i++) {
        const auto_action_t *a = &was->then[i];
        const auto_action_t *b = &now->then[i];
        if (a->kind != b->kind || a->partition != b->partition || a->arm_level != b->arm_level ||
            a->warn_min != b->warn_min || a->bypass_open != b->bypass_open) {
            return true;
        }
    }
    return false;
}

static bool time_trigger_changed(const auto_trigger_t *a, const auto_trigger_t *b)
{
    return a->kind != b->kind || a->minute != b->minute || a->days != b->days;
}

static bool trigger_moved(const auto_trigger_t *a, const auto_trigger_t *b)
{
    return a->kind != b->kind || a->zone != b->zone || a->partitions != b->partitions;
}

static bool trigger_changed(const auto_trigger_t *a, const auto_trigger_t *b)
{
    return trigger_moved(a, b) || time_trigger_changed(a, b) || a->levels != b->levels ||
           a->win_from != b->win_from || a->win_to != b->win_to;
}

/* The fields that make an action: a change of any of them makes it another action. The "Turn it
 * off after" length isn't one: a waiting On is kept and takes the new length when it is sent. */
static bool action_changed(const auto_rule_t *was, const auto_rule_t *now, size_t i)
{
    const bool had = i < was->n;
    const bool has = i < now->n && i < AUTO_ACTIONS;
    if (had != has) {
        return true;
    }
    if (!had) {
        return false;
    }
    const auto_action_t *a = &was->then[i];
    const auto_action_t *b = &now->then[i];
    return a->kind != b->kind || a->source != b->source || a->dev_kind != b->dev_kind ||
           a->dev_action != b->dev_action || a->value != b->value || a->output != b->output ||
           a->option != b->option || a->partition != b->partition ||
           strncmp(a->ref, b->ref, DEV_REF_LEN) != 0 || strncmp(a->mode, b->mode, DEV_MODE_LEN) != 0;
}

static void drop_job(auto_job_t *j)
{
    j->s.waiting = 0u;
    j->s.held = 0u;
}

void auto_run_rules(auto_run_t *r, const auto_rule_t *rules, size_t n, const auto_env_t *env)
{
    if (rules == NULL) {
        n = 0;
    }
    if (n > AUTO_MAX) {
        n = AUTO_MAX;
    }
    memset(r->zones, 0, sizeof(r->zones));
    r->partitions_enabled = 0u;
    r->less_secure = 0u;
    auto_env_t mine = {0};
    if (env != NULL) {
        r->partitions_enabled = env->partitions_enabled;
        r->less_secure = env->less_secure ? 1u : 0u;
        if (env->zones != NULL) {
            memcpy(r->zones, env->zones, sizeof(r->zones));
        }
        mine = *env;
    }
    mine.zones = r->zones;
    /* AUT-015: the option never stops a rule as a whole, nor changes what is stored; each less
     * secure send is checked against it (attempt). */
    mine.less_secure = true;

    /* An automation that disappeared: its held sends, off timers and pending results go. */
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (!e->used) {
            continue;
        }
        bool kept = false;
        for (size_t k = 0; k < n && !kept; k++) {
            kept = rules[k].uid == e->rule.uid;
        }
        if (!kept) {
            for (size_t f = 0; f < AUTO_RUN_FLIGHTS; f++) {
                if (r->flight[f].used && r->flight[f].uid == e->rule.uid) {
                    r->flight[f].used = 0u;
                }
            }
            auto_arm_end(r, e); /* deleted: its warning or hold ends at once */
            auto_panel_end(r, e, false); /* and a reminder's note (BL-224) */
            memset(e, 0, sizeof(*e));
            e->last_run = AUTO_RUN_NONE;
            e->arm.arm_run = AUTO_RUN_NONE;
        }
    }

    for (size_t k = 0; k < n; k++) {
        const auto_rule_t *in = &rules[k];
        bool dup = in->uid == 0u;
        for (size_t q = 0; q < k && !dup; q++) {
            dup = rules[q].uid == in->uid;
        }
        if (dup) {
            continue;
        }
        auto_entry_t *e = find(r, in->uid);
        if (e == NULL) {
            for (size_t i = 0; i < AUTO_MAX && e == NULL; i++) {
                if (!r->e[i].used) {
                    e = &r->e[i];
                }
            }
            if (e == NULL) {
                continue; /* can't happen: n <= AUTO_MAX and the gone ones were freed */
            }
            memset(e, 0, sizeof(*e));
            e->last_run = AUTO_RUN_NONE;
            e->arm.arm_run = AUTO_RUN_NONE;
            e->used = 1u;
        } else {
            if (time_trigger_changed(&e->rule.when, &in->when)) {
                e->last_run = AUTO_RUN_NONE;
                e->arm.arm_run = AUTO_RUN_NONE;
            }
            if (arming_changed(&e->rule, in)) {
                if (e->arm.state == (uint8_t)AUTO_WARN_ON) {
                    e->arm.arm_run = AUTO_RUN_NONE; /* its minute then says "no warning" */
                }
                auto_arm_end(r, e);
            }
            if (trigger_moved(&e->rule.when, &in->when)) {
                /* Another trigger: the old one's close hold and cooldown don't apply. */
                e->closing = 0u;
                e->cooling = 0u;
            }
            const bool new_trigger = trigger_changed(&e->rule.when, &in->when) ||
                                     e->rule.when.trouble != in->when.trouble;
            bool new_action = false;
            for (size_t a = 0; a < AUTO_ACTIONS; a++) {
                new_action = new_action || action_changed(&e->rule, in, a);
            }
            if (new_trigger || new_action) {
                auto_panel_end(r, e, true); /* BL-224: the note goes; an off timer finishes */
            }
            for (size_t a = 0; a < AUTO_ACTIONS; a++) {
                if (new_trigger || action_changed(&e->rule, in, a)) {
                    /* The old action's waiting send goes, and so does a run the old trigger
                     * started (PL04); an off timer keeps its own copy. */
                    drop_job(&e->job[a]);
                } else if (a < in->n) {
                    /* The same action, perhaps another length: a waiting On (sent once, as
                     * before) starts its timer with the new one. */
                    e->job[a].s.act.off_after_min = in->then[a].off_after_min;
                }
            }
        }
        e->rule = *in;
        if (e->rule.n > AUTO_ACTIONS) {
            e->rule.n = (uint8_t)AUTO_ACTIONS;
        }
        if (e->rule.keep_len > AUTO_KEEP_MAX) {
            e->rule.keep_len = (uint8_t)AUTO_KEEP_MAX;
        }
        const bool ok = e->rule.rec == (uint8_t)AUTO_REC_OK &&
                        auto_rule_check(&e->rule, &mine) == AUTO_OK;
        e->valid = ok ? 1u : 0u;
        e->runnable = ok && !e->rule.off ? 1u : 0u;
        if (!e->runnable) {
            /* Turned off (or no longer valid): nothing new starts; a running off timer and its
             * send finish. A warning or a hold ends at once (DESIGN.md 3.4). */
            for (size_t a = 0; a < AUTO_ACTIONS; a++) {
                drop_job(&e->job[a]);
            }
            e->closing = 0u;
            auto_arm_end(r, e);
            auto_panel_end(r, e, true);
        }
    }
}

void auto_run_occurrence(auto_run_t *r, const auto_occ_t *o, const auto_local_t *lt,
                         uint64_t now_ms)
{
    if (o == NULL) {
        return;
    }
    note_clock(r, lt, now_ms);
    auto_arm_hold_check(r, lt);
    auto_panel_hold_check(r, lt);
    const bool arming_state = o->kind == (uint8_t)AUTO_TRIG_ARMED ||
                              o->kind == (uint8_t)AUTO_TRIG_DISARMED;
    /* AUT-007: an ARMED or DISARMED an automation caused starts no arm or disarm */
    const bool alarm_ok = !(arming_state && auto_arm_own_occurrence(r, o));
    if (o->kind == (uint8_t)AUTO_TRIG_ARMED && o->partition != 0u) {
        auto_arm_partition_armed(r, o->partition, now_ms);
        auto_panel_partition_armed(r, o->partition); /* BL-224: its reminders' notes go */
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (!e->used) {
            continue;
        }
        if (arming_state && o->partition != 0u) {
            /* The partition's state changed since: a held arming-state send is stale. */
            for (size_t a = 0; a < AUTO_ACTIONS; a++) {
                auto_job_t *j = &e->job[a];
                if (j->s.waiting && j->occ_part == o->partition &&
                    (j->occ_kind == (uint8_t)AUTO_TRIG_ARMED ||
                     j->occ_kind == (uint8_t)AUTO_TRIG_DISARMED)) {
                    drop_job(j); /* nothing was sent: "Last run" keeps its outcome */
                }
            }
        }
        if (o->kind == (uint8_t)AUTO_TRIG_ZONE_OPENED && e->closing &&
            e->rule.when.zone == o->zone) {
            e->closing = 0u; /* re-opened inside the close hold */
        }
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (!e->used || !e->runnable || !matches(r, &e->rule, o)) {
            continue;
        }
        const int win = in_window(&e->rule.when, lt);
        if (win < 0) {
            finish(r, e, 0u, 0u, o->zone, AUTO_RES_NO_CLOCK, 0u, now_ms);
            continue;
        }
        if (win == 0) {
            continue;
        }
        if (o->kind == (uint8_t)AUTO_TRIG_ZONE_CLOSED) {
            e->closing = 1u;
            e->close_at = now_ms + AUTO_CLOSE_HOLD_MS;
            continue;
        }
        if (o->kind == (uint8_t)AUTO_TRIG_ZONE_OPENED) {
            if (e->cooling && now_ms < e->cool_until) {
                off_restart(e, now_ms, 0u); /* a triggered repeat */
                continue;
            }
            e->cooling = 1u;
            e->cool_until = now_ms + AUTO_ZONE_COOLDOWN_MS;
        }
        const uint8_t part = is_zone_trig(o->kind) ? 0u : o->partition;
        run_rule(r, e, o->kind, part, is_zone_trig(o->kind) ? o->zone : 0u, alarm_ok, o, now_ms);
    }
}

void auto_run_time(auto_run_t *r, const auto_clock_t *c, const auto_local_t *lt,
                   const auto_span_t *s, size_t n, uint64_t now_ms)
{
    note_clock(r, lt, now_ms);
    auto_arm_hold_check(r, lt);
    auto_panel_hold_check(r, lt);
    if (c != NULL && auto_clock_gen(c) != r->gen) {
        /* The clock went back: runs from its wrong future are forgotten (auto_clock.h). */
        const int32_t floor = auto_clock_floor(c);
        for (size_t i = 0; i < AUTO_MAX; i++) {
            if (r->e[i].last_run != AUTO_RUN_NONE && r->e[i].last_run > floor) {
                r->e[i].last_run = AUTO_RUN_NONE;
            }
            if (r->e[i].arm.arm_run != AUTO_RUN_NONE && r->e[i].arm.arm_run > floor) {
                r->e[i].arm.arm_run = AUTO_RUN_NONE;
            }
        }
        r->gen = auto_clock_gen(c);
    }
    if (s == NULL) {
        return;
    }
    for (size_t k = 0; k < n; k++) {
        for (size_t i = 0; i < AUTO_MAX; i++) {
            auto_entry_t *e = &r->e[i];
            if (!e->used || !e->runnable || e->rule.when.kind != (uint8_t)AUTO_TRIG_TIME ||
                auto_arm_is_arm_rule(&e->rule)) {
                continue;
            }
            if (auto_time_due(&e->rule.when, &s[k], &e->last_run)) {
                if (e->rule.n >= 1u && (e->rule.then[0].kind == (uint8_t)AUTO_ACT_REMIND ||
                                        e->rule.then[0].kind == (uint8_t)AUTO_ACT_CHECK_ARMED)) {
                    auto_panel_time(r, e, &s[k], now_ms); /* BL-224: looks at the panel now */
                } else {
                    run_rule(r, e, (uint8_t)AUTO_TRIG_TIME, 0u, 0u, true, NULL, now_ms);
                }
            }
        }
    }
    /* A time-of-day Arm: its warning's minute and its own, over the whole reading */
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (e->used && e->runnable && e->rule.when.kind == (uint8_t)AUTO_TRIG_TIME &&
            auto_arm_is_arm_rule(&e->rule)) {
            auto_arm_time(r, e, s, n, now_ms);
        }
    }
}

void auto_run_result(auto_run_t *r, uint32_t request_id, bool ok, uint8_t why, uint64_t now_ms)
{
    if (request_id == 0u) {
        return;
    }
    for (size_t f = 0; f < AUTO_RUN_FLIGHTS; f++) {
        if (r->flight[f].used && r->flight[f].req == request_id) {
            const uint8_t res = ok ? (uint8_t)AUTO_RES_DONE
                                   : why == (uint8_t)AUTO_WHY_NO_ANSWER ? (uint8_t)AUTO_RES_NO_ANSWER
                                                                        : (uint8_t)AUTO_RES_REFUSED;
            flight_end(r, &r->flight[f], res, now_ms);
            return;
        }
    }
}

void auto_run_tick(auto_run_t *r, uint64_t now_ms)
{
    for (size_t f = 0; f < AUTO_RUN_FLIGHTS; f++) {
        if (r->flight[f].used && now_ms >= r->flight[f].deadline) {
            flight_end(r, &r->flight[f], AUTO_RES_NO_ANSWER, now_ms);
        }
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_entry_t *e = &r->e[i];
        if (!e->used) {
            continue;
        }
        if (e->closing && now_ms >= e->close_at) {
            e->closing = 0u;
            if (e->runnable) {
                if (e->cooling && now_ms < e->cool_until) {
                    off_restart(e, now_ms, 0u); /* a triggered repeat */
                } else {
                    e->cooling = 1u;
                    e->cool_until = now_ms + AUTO_ZONE_COOLDOWN_MS;
                    run_rule(r, e, (uint8_t)AUTO_TRIG_ZONE_CLOSED, 0u, e->rule.when.zone, true,
                             NULL, now_ms);
                }
            }
        }
        for (uint8_t a = 0; a < AUTO_ACTIONS; a++) {
            auto_offtimer_t *t = &e->off[a];
            if (t->running && now_ms >= t->at) {
                off_due(t, now_ms);
            }
        }
        attempt_all(r, e, now_ms);
        auto_arm_tick(r, e, now_ms);
        auto_panel_tick(r, e, now_ms);
    }
}

uint64_t auto_run_next_deadline(const auto_run_t *r, uint64_t now_ms)
{
    uint64_t d = UINT64_MAX;
    for (size_t f = 0; f < AUTO_RUN_FLIGHTS; f++) {
        if (r->flight[f].used && r->flight[f].deadline < d) {
            d = r->flight[f].deadline;
        }
    }
    for (size_t i = 0; i < AUTO_MAX; i++) {
        const auto_entry_t *e = &r->e[i];
        if (!e->used) {
            continue;
        }
        if (e->closing && e->close_at < d) {
            d = e->close_at;
        }
        for (size_t a = 0; a < AUTO_ACTIONS; a++) {
            if (e->off[a].running && e->off[a].at < d) {
                d = e->off[a].at;
            }
            if (e->off[a].s.waiting && e->off[a].s.next_ms < d) {
                d = e->off[a].s.next_ms;
            }
            if (e->job[a].s.waiting && e->job[a].s.next_ms < d) {
                d = e->job[a].s.next_ms;
            }
        }
        d = auto_arm_deadline(r, e, d);
        d = auto_panel_deadline(e, d);
    }
    return d < now_ms ? now_ms : d;
}

auto_try_t auto_run_try(auto_run_t *r, uint16_t uid, uint64_t now_ms)
{
    auto_entry_t *e = find(r, uid);
    if (e == NULL) {
        return AUTO_TRY_UNKNOWN;
    }
    for (size_t i = 0; i < AUTO_ACTIONS && i < e->rule.n; i++) {
        if (e->rule.then[i].kind == (uint8_t)AUTO_ACT_ARM ||
            e->rule.then[i].kind == (uint8_t)AUTO_ACT_DISARM ||
            e->rule.then[i].kind == (uint8_t)AUTO_ACT_REMIND ||
            e->rule.then[i].kind == (uint8_t)AUTO_ACT_CHECK_ARMED) {
            return AUTO_TRY_ARM;
        }
    }
    if (!e->valid) {
        return AUTO_TRY_INVALID;
    }
    run_rule(r, e, (uint8_t)AUTO_TRIG_NONE, 0u, 0u, false, NULL, now_ms);
    return AUTO_TRY_OK;
}

void auto_run_status(const auto_run_t *r, uint16_t uid, auto_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const auto_entry_t *e = find_c(r, uid);
    if (e != NULL) {
        *out = e->st;
    }
}
