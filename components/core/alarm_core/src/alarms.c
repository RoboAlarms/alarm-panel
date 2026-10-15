/*
 * alarms.c - alarm activation, sounding, bell/sounder patterns.
 */
#include "engine_internal.h"

void eng_start_sounding(alarm_engine_t *e, uint8_t p, alarm_cause_t cause)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    const uint16_t bit = CAUSE_BIT(cause);
    if ((bit & SILENT_CAUSES) != 0u) {
        return;
    }
    const alarm_ms_t timeout =
        (e->cfg.opt.bell_timeout_s != 0u) ? ms_add(e->now, MS(e->cfg.opt.bell_timeout_s)) : 0u;

    pr->sounding = (uint16_t)(pr->sounding | bit);
    if ((bit & (FIRE_CAUSES | CO_CAUSES)) != 0u) {
        pr->fire_until = e->cfg.opt.fire_bell_timeout ? timeout : 0u;
    } else {
        pr->bell_until = timeout;
    }
    if ((bit & (LOUD_CAUSES | FIRE_CAUSES | CO_CAUSES)) != 0u) {
        pr->strobe = true;
    }
    pr->dirty = true;
}

void eng_silence(alarm_engine_t *e, uint8_t p, uint16_t causes)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    pr->sounding = (uint16_t)(pr->sounding & (uint16_t)~causes);
    if ((pr->sounding & (uint16_t)~(FIRE_CAUSES | CO_CAUSES)) == 0u) {
        pr->bell_until = 0u;
    }
    if ((pr->sounding & (FIRE_CAUSES | CO_CAUSES)) == 0u) {
        pr->fire_until = 0u;
    }
    pr->dirty = true;
}

void eng_sounding_timers(alarm_engine_t *e, uint8_t p)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    bool timed_out = false;
    if (pr->bell_until != 0u && e->now >= pr->bell_until) {
        pr->bell_until = 0u;
        const uint16_t keep = (uint16_t)(pr->sounding & (FIRE_CAUSES | CO_CAUSES));
        if (keep != pr->sounding) {
            pr->sounding = keep;
            timed_out = true;
        }
    }
    if (pr->fire_until != 0u && e->now >= pr->fire_until) {
        pr->fire_until = 0u;
        const uint16_t keep = (uint16_t)(pr->sounding & (uint16_t)~(FIRE_CAUSES | CO_CAUSES));
        if (keep != pr->sounding) {
            pr->sounding = keep;
            timed_out = true;
        }
    }
    if (timed_out) {
        eng_emit(e, ALARM_EV_BELL_TIMEOUT, p, 0u, 0u, NULL, 0u, 0u, 0u, REPORT_NONE);
        pr->dirty = true;
    }
}

void eng_alarm(alarm_engine_t *e, uint8_t p, uint16_t zone, alarm_cause_t cause,
               const alarm_source_t *src, uint8_t user)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    const uint16_t bit = CAUSE_BIT(cause);
    const bool burglary = (bit & ABORTABLE_CAUSES) != 0u;

    if (zone != 0u) {
        alarm_zone_rt_t *zr = eng_zrt(e, zone);
        const alarm_zone_config_t *zc = eng_zcfg(e, zone);
        if (burglary && (zc->flags & ALARM_ZF_SWINGER_EXEMPT) == 0u) {
            if ((zr->flags & ZRT_SWINGER) != 0u) {
                return;
            }
            const uint8_t limit =
                (zc->swinger_trips != 0u) ? zc->swinger_trips : e->cfg.opt.swinger_trips;
            if (zr->swinger >= limit) {
                zr->flags = (uint16_t)(zr->flags | ZRT_SWINGER);
                eng_emit(e, ALARM_EV_SWINGER_SHUTDOWN, p, zone, 0u, src, 0u, 0u, 0u,
                         eng_zone_reportable(e, zone) ? REPORT_NOW : REPORT_NONE);
                pr->dirty = true;
                return;
            }
            zr->swinger++;
        }
        zr->flags = (uint16_t)(zr->flags | ZRT_IN_ALARM);
        if ((bit & HIDDEN_CAUSES) == 0u) {
            zr->flags = (uint16_t)(zr->flags | ZRT_MEMORY);
            if ((bit & SILENT_CAUSES) != 0u) {
                zr->flags = (uint16_t)(zr->flags | ZRT_SILENT_MEM);
            } else {
                zr->flags = (uint16_t)(zr->flags & (uint16_t)~ZRT_SILENT_MEM);
            }
        }
    }

    if ((bit & HIDDEN_CAUSES) == 0u) {
        pr->memory = (uint16_t)(pr->memory | bit);
    }
    pr->period_alarms = (uint16_t)(pr->period_alarms | bit);
    eng_start_sounding(e, p, cause);

    report_mode_t mode = REPORT_NOW;
    if (burglary) {
        if (!pr->alarm_reported && pr->abort_until == 0u && e->cfg.opt.abort_window_s != 0u) {
            pr->abort_until = ms_add(e->now, MS(e->cfg.opt.abort_window_s));
        }
        mode = pr->alarm_reported ? REPORT_NOW : REPORT_DEFERRED;
    }
    const report_mode_t zone_mode = eng_zone_reportable(e, zone) ? mode : REPORT_NONE;
    const uint8_t flags = ((bit & SILENT_CAUSES) != 0u) ? ALARM_EVF_SILENT : 0u;
    eng_emit(e, ALARM_EV_ALARM, p, zone, user, src, (uint8_t)cause, 0u, flags, zone_mode);

    /* SIA CP-01 recent closing: an alarm within 2 minutes after exit time ended. */
    if (burglary && pr->state != ALARM_STATE_DISARMED && !pr->recent_close_sent &&
        pr->recent_close_until != 0u && e->now < pr->recent_close_until) {
        pr->recent_close_sent = true;
        eng_emit(e, ALARM_EV_RECENT_CLOSING, p, zone, pr->armed_by, src, 0u, 0u, 0u, mode);
    }
    pr->dirty = true;
    e->persist_dirty = true;
}

alarm_pattern_t eng_bell_pattern(const alarm_partition_rt_t *pr)
{
    if ((pr->sounding & FIRE_CAUSES) != 0u) {
        return ALARM_PAT_TEMPORAL3;
    }
    if ((pr->sounding & CO_CAUSES) != 0u) {
        return ALARM_PAT_TEMPORAL4;
    }
    if ((pr->sounding & LOUD_CAUSES) != 0u) {
        return ALARM_PAT_STEADY;
    }
    return ALARM_PAT_OFF;
}

alarm_sound_t eng_sounder(const alarm_engine_t *e, uint8_t p)
{
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    if ((pr->sounding & FIRE_CAUSES) != 0u) {
        return ALARM_SOUND_FIRE;
    }
    if ((pr->sounding & CO_CAUSES) != 0u) {
        return ALARM_SOUND_CO;
    }
    if ((pr->sounding & LOUD_CAUSES) != 0u) {
        return ALARM_SOUND_ALARM;
    }
    if ((pr->sounding & AUX_CAUSES) != 0u) {
        return ALARM_SOUND_AUX;
    }
    if (pr->state == ALARM_STATE_ENTRY_DELAY) {
        return ((pr->period_alarms & (uint16_t)~HIDDEN_CAUSES) != 0u) ? ALARM_SOUND_ENTRY_MEMORY
                                                                      : ALARM_SOUND_ENTRY;
    }
    /* Exit beeps only for Away (and Maximum): Stay, Night and Instant exit time is silent
     * after the arming beeps. */
    if (pr->state == ALARM_STATE_EXIT_DELAY && pr->level == ALARM_LEVEL_AWAY &&
        e->cfg.opt.exit_sounds && (pr->flags & ALARM_ARM_SILENT_EXIT) == 0u) {
        const alarm_ms_t left = (pr->deadline > e->now) ? pr->deadline - e->now : 0u;
        return (left <= 10000u) ? ALARM_SOUND_EXIT_FAST : ALARM_SOUND_EXIT;
    }
    if (pr->trouble_unack) {
        return ALARM_SOUND_TROUBLE;
    }
    return ALARM_SOUND_NONE;
}
