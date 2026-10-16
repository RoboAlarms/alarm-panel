/*
 * plink_vfeed.c - a keypad's voice, fed from the panel link (docs/features/11 LINK-028). See
 * include/panel_link/plink_vfeed.h for the rules.
 */
#include "panel_link/plink_vfeed.h"

#include <string.h>

void plink_vfeed_init(plink_vfeed_t *f, const plink_vfeed_port_t *port)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof *f);
    if (port != NULL) {
        f->port = *port;
    }
}

bool plink_vfeed_live(const plink_vfeed_t *f)
{
    return f != NULL && f->live;
}

/* The open batch, the hold and the settle are gone (a baseline, a pause). */
static void batch_reset(plink_vfeed_t *f)
{
    f->batch_sound = false;
    f->batch_events = false;
    f->settle_at = 0u;
    f->holding = false;
    f->hold_spent = false;
    f->hold_since = 0u;
}

void plink_vfeed_sound(plink_vfeed_t *f, uint64_t now_ms, const plink_sound_t *s, uint32_t ticket)
{
    if (f == NULL || s == NULL || !f->live) {
        return;
    }
    if (f->port.sound != NULL) {
        f->port.sound(f->port.ctx, s->partition, (alarm_sound_t)s->sound, s->zone, ticket);
    }
    f->sounds++;
    f->batch_sound = true;
    /* With its events in already (an older master sends the STATUS first) the batch is whole;
     * alone, it waits for the STATUS that may follow - a chime has none. */
    f->settle_at = f->batch_events ? now_ms : now_ms + PLINK_VFEED_SETTLE_MS;
}

static bool home_ok(uint8_t home)
{
    return home >= 1u && home <= ALARM_MAX_PARTITIONS;
}

/* The event with the smallest seq in (after, upto], NULL when there is none: the events in seq
 * order, each seq once, whatever order (or repeats) the view holds them in. */
static const alarm_event_t *next_after(const plink_vfeed_view_t *v, uint32_t after, uint32_t upto)
{
    const alarm_event_t *best = NULL;
    for (size_t i = 0u; i < v->n_ev; i++) {
        const alarm_event_t *e = &v->ev[i];
        if (e->seq > after && e->seq <= upto && (best == NULL || e->seq < best->seq)) {
            best = e;
        }
    }
    return best;
}

/* Stamped by the master long before its time now: a frame of it was held up (a link that lost
 * everything for a while without going LOST), so it is news no longer. */
static bool stale(const plink_vfeed_view_t *v, const alarm_event_t *e)
{
    return v->epoch_ms > 0 && e->epoch_ms > 0 &&
           e->epoch_ms < v->epoch_ms - (int64_t)PLINK_VFEED_STALE_MS;
}

static void feed_event(plink_vfeed_t *f, const alarm_event_t *e)
{
    /* The engine pulses the bell's test pattern just before it starts a walk test (WALK-003);
     * no OUTPUT frame carries it, and "Walk test started" waits for it (WALK-016). */
    if (e->type == ALARM_EV_WALK_TEST_START && e->partition == f->home && f->port.output != NULL) {
        f->port.output(f->port.ctx, ALARM_OUT_BELL, f->home, ALARM_PAT_TEST);
    }
    if (f->port.event != NULL) {
        f->port.event(f->port.ctx, e);
    }
    f->events++;
}

static void baseline(plink_vfeed_t *f, const plink_vfeed_view_t *v)
{
    f->live = true;
    f->home = v->home;
    f->floor = v->snap->last_event_seq;
    f->snap_seq = v->snap->seq;
    batch_reset(f);
    f->baselines++;
    if (f->port.baseline != NULL) {
        f->port.baseline(f->port.ctx, v->home, v->snap);
    }
}

void plink_vfeed_poll(plink_vfeed_t *f, uint64_t now_ms, const plink_vfeed_view_t *v)
{
    if (f == NULL || v == NULL) {
        return;
    }
    if (!v->online || v->snap == NULL || !home_ok(v->home)) {
        if (f->live) {
            f->live = false;
            batch_reset(f);
            f->pauses++;
            if (f->port.pause != NULL) {
                f->port.pause(f->port.ctx);
            }
        }
        return;
    }
    const alarm_snapshot_t *s = v->snap;
    if (!f->live || v->home != f->home || s->last_event_seq < f->floor || v->resync) {
        baseline(f, v);
        return;
    }
    plink_vfeed_view_t view = *v;
    if (view.ev == NULL) {
        view.n_ev = 0u;
    }
    const uint32_t last = s->last_event_seq;

    /* The hold: events pushed ahead of this snapshot (their STATUS comes next), or covered ones
     * the ring lacks (the engine numbers its events one by one; a pull fills the hole). */
    bool ahead = false;
    for (size_t i = 0u; i < view.n_ev; i++) {
        ahead = ahead || view.ev[i].seq > last;
    }
    uint32_t covered = 0u;
    for (const alarm_event_t *e = next_after(&view, f->floor, last); e != NULL;
         e = next_after(&view, e->seq, last)) {
        covered++;
    }
    const bool missing = covered < last - f->floor;
    if (!ahead && !missing) {
        f->holding = false;
        f->hold_spent = false;
    } else if (!f->hold_spent) {
        if (!f->holding) {
            f->holding = true;
            f->hold_since = now_ms;
        }
        if (now_ms - f->hold_since >= PLINK_VFEED_HOLD_MS) {
            f->holding = false;
            f->hold_spent = true; /* what is there goes; the rest is never said */
        }
    }
    if (f->holding) {
        return;
    }

    /* The covered events, in seq order, once; the floor moves past every one of them. */
    if (last > f->floor) {
        bool fed = false;
        for (const alarm_event_t *e = next_after(&view, f->floor, last); e != NULL;
             e = next_after(&view, e->seq, last)) {
            if (stale(&view, e)) {
                f->stale++;
                continue;
            }
            feed_event(f, e);
            fed = true;
        }
        f->floor = last;
        if (fed) {
            f->batch_events = true;
            /* with its sound in already the batch is whole; else a late ACK or ERROR joins */
            f->settle_at = f->batch_sound ? now_ms : now_ms + PLINK_VFEED_SETTLE_MS;
        }
    }

    /* The commit: a new snapshot, or the facts fed, once the batch has settled. */
    const bool snap_new = s->seq != f->snap_seq;
    if ((snap_new || f->batch_events || f->batch_sound) && now_ms >= f->settle_at) {
        if (f->port.snapshot != NULL) {
            f->port.snapshot(f->port.ctx, s);
        }
        f->snap_seq = s->seq;
        f->batch_sound = false;
        f->batch_events = false;
        f->settle_at = 0u;
        f->commits++;
    }
}

uint64_t plink_vfeed_next(const plink_vfeed_t *f)
{
    if (f == NULL || !f->live) {
        return UINT64_MAX;
    }
    if (f->holding) {
        return f->hold_since + PLINK_VFEED_HOLD_MS;
    }
    if (f->batch_sound || f->batch_events) {
        return f->settle_at;
    }
    return UINT64_MAX;
}
