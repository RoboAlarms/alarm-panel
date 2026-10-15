/*
 * events.c - event emission and the abort-window report queue.
 */
#include <string.h>

#include "engine_internal.h"

bool eng_zone_reportable(const alarm_engine_t *e, uint16_t zone)
{
    if (zone == 0u || zone > ALARM_MAX_ZONES) {
        return true;
    }
    return (eng_zcfg(e, zone)->flags & ALARM_ZF_NO_REPORT) == 0u;
}

/* Bookkeeping when an alarm report actually leaves the panel. */
static void on_alarm_reported(alarm_engine_t *e, const alarm_event_t *ev)
{
    if (ev->type != ALARM_EV_ALARM || ev->partition == 0u) {
        return;
    }
    alarm_partition_rt_t *pr = eng_part(e, ev->partition);
    if ((CAUSE_BIT(ev->arg) & ABORTABLE_CAUSES) != 0u) {
        pr->alarm_reported = true;
        pr->cancel_until = ms_add(e->now, MS(e->cfg.opt.cancel_window_s));
    }
    if (ev->zone != 0u && ev->zone <= ALARM_MAX_ZONES) {
        alarm_zone_rt_t *zr = eng_zrt(e, ev->zone);
        zr->flags = (uint16_t)(zr->flags | ZRT_REPORTED);
    }
}

static void deliver(alarm_engine_t *e, const alarm_event_t *ev)
{
    on_alarm_reported(e, ev);
    if (e->ports.on_report != NULL) {
        e->ports.on_report(e->ports.ctx, ev);
    }
}

void eng_emit(alarm_engine_t *e, alarm_event_type_t type, uint8_t partition, uint16_t zone,
              uint8_t user, const alarm_source_t *src, uint8_t arg, uint8_t arg2,
              uint8_t flags, report_mode_t mode)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = ++e->seq;
    ev.mono_ms = e->now;
    ev.epoch_ms = (e->ports.get_epoch_ms != NULL) ? e->ports.get_epoch_ms(e->ports.ctx) : 0;
    ev.type = type;
    ev.partition = partition;
    ev.zone = zone;
    ev.user = user;
    if (src != NULL) {
        ev.source = *src;
    }
    ev.arg = arg;
    ev.arg2 = arg2;
    ev.flags = flags;
    if (mode != REPORT_NONE) {
        ev.flags = (uint8_t)(ev.flags | ALARM_EVF_REPORT);
    }

    if (e->ports.on_event != NULL) {
        e->ports.on_event(e->ports.ctx, &ev);
    }

    if (mode == REPORT_NONE) {
        return;
    }
    if (mode == REPORT_NOW || partition == 0u) {
        deliver(e, &ev);
        return;
    }

    const alarm_ms_t due = eng_part(e, partition)->abort_until;
    if (due <= e->now) {
        deliver(e, &ev);
        return;
    }
    for (unsigned i = 0; i < ALARM_MAX_PENDING_REPORTS; i++) {
        if (!e->pending[i].used) {
            e->pending[i].used = true;
            e->pending[i].ev = ev;
            e->pending[i].due = due;
            return;
        }
    }
    deliver(e, &ev); /* queue full: fail open, report now */
}

void eng_reports_due(alarm_engine_t *e)
{
    /* Deliver in sequence order so held reports keep their original order. */
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < ALARM_MAX_PENDING_REPORTS; i++) {
            const alarm_pending_report_t *r = &e->pending[i];
            if (r->used && r->due <= e->now &&
                (best < 0 || r->ev.seq < e->pending[best].ev.seq)) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        alarm_event_t ev = e->pending[best].ev;
        e->pending[best].used = false;
        deliver(e, &ev);
    }
}

alarm_ms_t eng_reports_next(const alarm_engine_t *e)
{
    alarm_ms_t next = ALARM_TIME_NEVER;
    for (unsigned i = 0; i < ALARM_MAX_PENDING_REPORTS; i++) {
        if (e->pending[i].used && e->pending[i].due < next) {
            next = e->pending[i].due;
        }
    }
    return next;
}

unsigned eng_reports_drop(alarm_engine_t *e, uint8_t partition)
{
    unsigned alarms = 0;
    for (unsigned i = 0; i < ALARM_MAX_PENDING_REPORTS; i++) {
        alarm_pending_report_t *r = &e->pending[i];
        if (r->used && r->ev.partition == partition) {
            if (r->ev.type == ALARM_EV_ALARM) {
                alarms++;
            }
            r->used = false;
        }
    }
    return alarms;
}
