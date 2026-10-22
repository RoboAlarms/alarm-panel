/*
 * notify_gate.c - what actually reaches a phone (docs/features/26, NOTE-030..038).
 *
 * Pure decision logic: the caller supplies the clock and does the sending.
 */
#include "notify/notify_gate.h"

#include <string.h>

static void roll_day(notify_gate_t *g, uint64_t now)
{
    if (now - g->day_start >= NOTIFY_DAY_MS) {
        g->day_start = now;
        g->day_count = 0u;
    }
}

static void refill(notify_gate_t *g, uint64_t now)
{
    if (now < g->refilled) {
        g->refilled = now; /* the clock stepped back (SNTP); start the window again */
        return;
    }
    const uint64_t due = (now - g->refilled) / NOTIFY_BUCKET_REFILL_MS;
    if (due == 0u) {
        return;
    }
    const uint64_t tokens = (uint64_t)g->tokens + due;
    g->tokens = (uint8_t)(tokens > NOTIFY_BUCKET_MAX ? NOTIFY_BUCKET_MAX : tokens);
    g->refilled += due * NOTIFY_BUCKET_REFILL_MS;
}

/* One trouble, on one zone: what makes two reports "the same thing again". */
static uint32_t trouble_id(const alarm_event_t *ev)
{
    return ((uint32_t)ev->arg2 << 24) | ((uint32_t)ev->arg << 16) | (uint32_t)ev->zone;
}

static notify_trouble_slot_t *trouble_find(notify_gate_t *g, uint32_t id)
{
    for (unsigned i = 0; i < NOTIFY_TRBL_SLOTS; i++) {
        if (g->trouble[i].id == id) {
            return &g->trouble[i];
        }
    }
    return NULL;
}

/* The oldest slot, so a busy panel loses the stalest memory rather than refusing to record. */
static notify_trouble_slot_t *trouble_slot(notify_gate_t *g)
{
    notify_trouble_slot_t *oldest = &g->trouble[0];
    for (unsigned i = 0; i < NOTIFY_TRBL_SLOTS; i++) {
        if (g->trouble[i].id == 0u) {
            return &g->trouble[i];
        }
        if (g->trouble[i].at < oldest->at) {
            oldest = &g->trouble[i];
        }
    }
    return oldest;
}

void notify_gate_init(notify_gate_t *g, uint8_t cats, bool silent_too, uint64_t now)
{
    memset(g, 0, sizeof(*g));
    g->cats = cats;
    g->silent_too = silent_too;
    g->tokens = NOTIFY_BUCKET_MAX;
    g->refilled = now;
    g->day_start = now;
    /* Nothing has been sent yet, so the first alarm must not be held by the minimum gap. */
    g->last_alarm = now - NOTIFY_ALARM_MIN_GAP_MS;
}

void notify_gate_set(notify_gate_t *g, uint8_t cats, bool silent_too)
{
    g->cats = cats;
    g->silent_too = silent_too;
}

bool notify_gate_wanted(uint8_t cats, const alarm_event_t *ev)
{
    if (ev->type == ALARM_EV_AUTOMATION_NOTICE) {
        return true; /* NOTE-042: asked for by name; the limits below still apply */
    }
    return (cats & NOTIFY_CAT_BIT(notify_category(ev->type))) != 0u;
}

bool notify_gate_paused(const notify_gate_t *g)
{
    return g->day_count >= NOTIFY_DAY_HARD;
}

/* The newer of two event sequence numbers (they only grow; compared as a difference, so the
 * wrap after four billion events doesn't turn the answer round). */
static uint32_t newer_seq(uint32_t a, uint32_t b)
{
    return (int32_t)(b - a) > 0 ? b : a;
}

/* An alarm already running in this partition: fold the zone into it. */
static notify_act_t fold_into_alarm(notify_rollup_t *r, const alarm_event_t *ev, uint64_t now)
{
    r->seq = newer_seq(r->seq, ev->seq);
    if (r->total < 0xFFFFu) {
        r->total++;
    }
    if (r->n < NOTIFY_ROLLUP_ZONES) {
        /* The same zone tripping twice is one zone, not two. */
        for (unsigned i = 0; i < r->n; i++) {
            if (r->zones[i] == ev->zone) {
                r->total--;
                return NOTIFY_ACT_HOLD;
            }
        }
        r->zones[r->n++] = ev->zone;
    }
    r->follow_up = true;
    (void)now;
    return NOTIFY_ACT_HOLD;
}

/* An alarm with a partition: the one kind that opens a rollup (NOTE-033). */
static bool opens_rollup(const alarm_event_t *ev)
{
    return ev->type == ALARM_EV_ALARM && ev->partition >= 1u &&
           ev->partition <= ALARM_MAX_PARTITIONS;
}

static bool is_trouble(const alarm_event_t *ev)
{
    return ev->type == ALARM_EV_TROUBLE || ev->type == ALARM_EV_TROUBLE_RESTORE;
}

static void open_rollup(notify_rollup_t *r, const alarm_event_t *ev, uint64_t now)
{
    memset(r, 0, sizeof(*r));
    r->open = true;
    r->opened = now;
    r->cause = ev->arg;
    r->partition = ev->partition;
    r->total = 1u;
    r->zones[r->n++] = ev->zone;
    r->seq = ev->seq;
}

/* A rollup closed with a follow-up to send. One still owed from the last rollup of this
 * partition takes it in, so neither is lost: the zones named are the first ones. */
static void owe(notify_rollup_t *o, const notify_rollup_t *r)
{
    if (!o->follow_up) {
        *o = *r;
        o->open = false;
        o->follow_up = true;
        return;
    }
    o->seq = newer_seq(o->seq, r->seq);
    uint32_t total = (uint32_t)o->total + r->total;
    for (unsigned i = 0; i < r->n; i++) {
        bool named = false;
        for (unsigned j = 0; j < o->n; j++) {
            if (o->zones[j] == r->zones[i]) {
                named = true;
                total--; /* the same zone in both is one zone */
                break;
            }
        }
        if (!named && o->n < NOTIFY_ROLLUP_ZONES) {
            o->zones[o->n++] = r->zones[i];
        }
    }
    o->total = (uint16_t)(total > 0xFFFFu ? 0xFFFFu : total);
}

/* Partition i's rollup is over: its follow-up, if it has one, is owed from now on. Called when
 * its minute is up and, before that, by anything about to open a new one in its place - a new
 * alarm sent or held right after the minute, before notify_gate_follow_up() ran - so the zones
 * it took in are never cleared unsaid (code review). */
static void close_rollup(notify_gate_t *g, unsigned i)
{
    notify_rollup_t *r = &g->rollup[i];
    if (!r->open) {
        return;
    }
    r->open = false;
    if (r->follow_up) { /* one zone, one alert: otherwise nothing to add */
        r->follow_up = false;
        owe(&g->owed[i], r);
    }
}

/* NOTE-034: a trouble is remembered as said; its restore forgets it. */
static void trouble_record(notify_gate_t *g, const alarm_event_t *ev, uint64_t now)
{
    const uint32_t id = trouble_id(ev);
    notify_trouble_slot_t *slot = trouble_find(g, id);
    if (ev->type == ALARM_EV_TROUBLE_RESTORE) {
        if (slot != NULL) {
            slot->id = 0u;
            slot->at = 0u;
        }
        return;
    }
    if (slot == NULL) {
        slot = trouble_slot(g);
    }
    slot->id = id;
    slot->at = now;
}

notify_act_t notify_gate_admit(notify_gate_t *g, const alarm_event_t *ev, uint64_t now)
{
    roll_day(g, now);

    const notify_cat_t cat = notify_category(ev->type);
    if (!notify_gate_wanted(g->cats, ev)) {
        return NOTIFY_ACT_DROP; /* NOTE-037: filtered before anything is spent */
    }
    if (ev->type == ALARM_EV_ARMING_PUT_OFF && ev->arg == (uint8_t)ALARM_PUT_OFF_HOUR) {
        /* NOTE-040 (docs/features/41 AUT-013): an hour later is said by the ARMED or "Arming
         * failed" that follows; only a skip (tonight, today) reaches a phone. */
        return NOTIFY_ACT_DROP;
    }
    if ((ev->flags & ALARM_EVF_SILENT) && !g->silent_too) {
        return NOTIFY_ACT_DROP; /* NOTE-038 */
    }
    if (g->day_count >= NOTIFY_DAY_HARD) {
        return NOTIFY_ACT_DROP; /* NOTE-031 */
    }

    /* NOTE-033: one running alarm is one conversation. */
    if (opens_rollup(ev)) {
        notify_rollup_t *r = &g->rollup[ev->partition - 1u];
        if (r->open && now - r->opened < NOTIFY_ROLLUP_MS) {
            return fold_into_alarm(r, ev, now);
        }
        /* NOTE-032: even alarms keep a floor between them. */
        if (now - g->last_alarm < NOTIFY_ALARM_MIN_GAP_MS) {
            close_rollup(g, ev->partition - 1u);
            open_rollup(r, ev, now);
            r->follow_up = true;
            return NOTIFY_ACT_HOLD;
        }
        return NOTIFY_ACT_SEND; /* the rollup opens when it has gone: notify_gate_sent() */
    }

    /* NOTE-034: the same trouble on the same zone is not said twice within the hour, and a
     * restore is only worth sending when the trouble itself was. */
    const bool trouble = is_trouble(ev);
    if (trouble) {
        const notify_trouble_slot_t *slot = trouble_find(g, trouble_id(ev));
        if (ev->type == ALARM_EV_TROUBLE_RESTORE && slot == NULL) {
            return NOTIFY_ACT_DROP; /* nobody was told it broke */
        }
        if (ev->type == ALARM_EV_TROUBLE && slot != NULL &&
            now - slot->at < NOTIFY_TRBL_COLLAPSE_MS) {
            return NOTIFY_ACT_DROP;
        }
    }

    if (cat != NOTIFY_CAT_ALARM) {
        refill(g, now);
        if (g->day_count >= NOTIFY_DAY_SOFT || g->tokens == 0u) {
            /* NOTE-031: alarms only from here on; NOTE-030: the bucket is empty. A trouble
             * dropped by a limit still counts as said, as it always has. */
            if (trouble) {
                trouble_record(g, ev, now);
            }
            return NOTIFY_ACT_DROP;
        }
    }
    return NOTIFY_ACT_SEND;
}

void notify_gate_sent(notify_gate_t *g, const alarm_event_t *ev, uint64_t now)
{
    roll_day(g, now);
    if (opens_rollup(ev)) {
        close_rollup(g, ev->partition - 1u);
        open_rollup(&g->rollup[ev->partition - 1u], ev, now);
        g->last_alarm = now;
    } else {
        if (is_trouble(ev)) {
            trouble_record(g, ev, now);
        }
        if (notify_category(ev->type) != NOTIFY_CAT_ALARM) {
            refill(g, now);
            if (g->tokens > 0u) {
                g->tokens--;
            }
        }
    }
    if (g->day_count < 0xFFFFu) {
        g->day_count++;
    }
}

notify_act_t notify_gate_offer(notify_gate_t *g, const alarm_event_t *ev, uint64_t now)
{
    const notify_act_t act = notify_gate_admit(g, ev, now);
    if (act == NOTIFY_ACT_SEND) {
        notify_gate_sent(g, ev, now);
    }
    return act;
}

bool notify_gate_deliver(notify_gate_t *g, const alarm_event_t *ev, uint64_t now,
                         notify_send_fn send, void *ctx)
{
    if (notify_gate_admit(g, ev, now) != NOTIFY_ACT_SEND) {
        return true; /* dropped or folded in: nothing to deliver */
    }
    if (!send(ctx, ev)) {
        return false; /* S02: not recorded, so the retry is admitted and sent again */
    }
    notify_gate_sent(g, ev, now);
    return true;
}

bool notify_fan_out(uint8_t *done, uint8_t wanted, notify_try_fn try_one, void *ctx)
{
    for (uint8_t n = 0; n < 8u; n++) {
        const uint8_t bit = (uint8_t)(1u << n);
        if ((wanted & bit) != 0u && (*done & bit) == 0u && try_one(ctx, n)) {
            *done = (uint8_t)(*done | bit);
        }
    }
    if ((wanted & (uint8_t)~*done) != 0u) {
        return false; /* somebody still owes it a delivery */
    }
    *done = 0u;
    return true;
}

bool notify_gate_follow_up(notify_gate_t *g, uint64_t now, notify_rollup_t *out)
{
    roll_day(g, now); /* yesterday's hard cap must not drop one owed today */
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const notify_rollup_t *r = &g->rollup[i];
        if (r->open && now - r->opened >= NOTIFY_ROLLUP_MS) {
            close_rollup(g, i);
        }
    }
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        notify_rollup_t *o = &g->owed[i];
        if (!o->follow_up) {
            continue;
        }
        if (g->day_count >= NOTIFY_DAY_HARD) {
            o->follow_up = false; /* NOTE-031: nothing more today */
            continue;
        }
        *out = *o;
        return true;
    }
    return false;
}

void notify_gate_follow_up_sent(notify_gate_t *g, uint8_t partition, uint64_t now)
{
    if (partition < 1u || partition > ALARM_MAX_PARTITIONS ||
        !g->owed[partition - 1u].follow_up) {
        return;
    }
    g->owed[partition - 1u].follow_up = false;
    if (g->day_count < 0xFFFFu) {
        g->day_count++;
    }
    g->last_alarm = now;
}

bool notify_gate_tick(notify_gate_t *g, uint64_t now, notify_rollup_t *out)
{
    if (!notify_gate_follow_up(g, now, out)) {
        return false;
    }
    notify_gate_follow_up_sent(g, out->partition, now);
    return true;
}
