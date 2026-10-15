/*
 * walk.c - the walk test (docs/features/40): its start, end, extend and time limit, the end
 * warnings, and the marks of the zones it tested. Who may run which operation is decided by
 * the command dispatcher (arming.c); this file only does the work.
 */
#include <string.h>

#include "engine_internal.h"

#define WARN_5_MIN 0x01u
#define WARN_1_MIN 0x02u

/* Zones a walk test covers and counts in its total (WALK-007): every burglary and no-response
 * zone, monitor and momentary exit, the buttons and the keyswitch; 24-hour zones (panic
 * buttons among them) only with ALARM_WALK_F_LIFE24. Fire and CO stay live and aren't counted,
 * though the sensor's own test still marks them (ALARM_ZEV_TEST). */
bool alarm_zone_type_walk_counts(alarm_zone_type_t type, uint8_t walk_flags)
{
    switch (type) {
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
    case ALARM_ZT_PERIMETER:
    case ALARM_ZT_INTERIOR_FOLLOWER:
    case ALARM_ZT_DAY_NIGHT:
    case ALARM_ZT_INTERIOR_DELAY:
    case ALARM_ZT_SILENT_BURGLARY:
    case ALARM_ZT_NO_RESPONSE:
    case ALARM_ZT_MONITOR:
    case ALARM_ZT_MOMENTARY_EXIT:
    case ALARM_ZT_ARM_STAY:
    case ALARM_ZT_ARM_AWAY:
    case ALARM_ZT_DISARM:
    case ALARM_ZT_KEYSWITCH:
        return true;
    case ALARM_ZT_24H_SILENT:
    case ALARM_ZT_24H_AUDIBLE:
    case ALARM_ZT_24H_AUX:
        return (walk_flags & ALARM_WALK_F_LIFE24) != 0u;
    default:
        return false; /* fire, fire verified, CO, not used */
    }
}

bool eng_walk_covers(const alarm_engine_t *e, uint16_t z, uint8_t walk_flags)
{
    return alarm_zone_type_walk_counts(eng_zcfg(e, z)->type, walk_flags);
}

static bool in_partition(const alarm_engine_t *e, uint16_t z, uint8_t p)
{
    return eng_zone_valid(e, z) && e->cfg.zones[z - 1u].partition == p;
}

/* The end warnings already past for a limit this long are given up front, so a short limit
 * never warns the moment it starts. */
static uint8_t warnings_due_later(alarm_ms_t limit_ms)
{
    uint8_t given = 0u;
    if (limit_ms <= MS(300u)) {
        given = (uint8_t)(given | WARN_5_MIN);
    }
    if (limit_ms <= MS(60u)) {
        given = (uint8_t)(given | WARN_1_MIN);
    }
    return given;
}

static void set_limit(alarm_engine_t *e, alarm_partition_rt_t *pr)
{
    const alarm_ms_t limit = MS(e->cfg.opt.walk_test_timeout_s);
    pr->walk_test_until = ms_add(e->now, limit);
    pr->walk_warned = warnings_due_later(limit);
}

void eng_walk_start(alarm_engine_t *e, uint8_t p, uint8_t flags, uint8_t user,
                    const alarm_source_t *src)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    uint16_t total = 0u;

    /* A start clears only this partition's marks (WALK-006): a zone belongs to one. */
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (!in_partition(e, z, p)) {
            continue;
        }
        memset(&e->walk[z - 1u], 0, sizeof(e->walk[0]));
        if (eng_walk_covers(e, z, flags)) {
            total++;
        }
    }
    pr->walk_test = true;
    pr->walk_flags = (uint8_t)(flags & ALARM_WALK_F_LIFE24);
    pr->walk_started = e->now;
    pr->walk_started_by = user;
    pr->walk_started_src = *src;
    pr->walk_tested = 0u;
    pr->walk_total = total;
    pr->walk_ended_by = 0u;
    pr->walk_end_how = (uint8_t)ALARM_WALK_END_NONE;
    pr->walk_seq++; /* the list was cleared: a reader asking "since n" reads it again */
    set_limit(e, pr);
    eng_output(e, ALARM_OUT_BELL, p, ALARM_PAT_TEST); /* short siren test (WALK-003) */
    eng_emit(e, ALARM_EV_WALK_TEST_START, p, 0u, user, src, 0u, 0u, 0u, REPORT_NOW);
    pr->dirty = true;
}

void eng_walk_extend(alarm_engine_t *e, uint8_t p, uint8_t user, const alarm_source_t *src)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    set_limit(e, pr);
    eng_emit(e, ALARM_EV_WALK_TEST_EXTEND, p, 0u, user, src, 0u, 0u, 0u, REPORT_NOW);
    pr->dirty = true;
}

static uint8_t sat8(uint16_t v)
{
    return (v > 255u) ? 255u : (uint8_t)v;
}

void eng_walk_end(alarm_engine_t *e, uint8_t p, uint8_t user, const alarm_source_t *src,
                  alarm_walk_end_t how)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    pr->walk_test = false;
    pr->walk_ended_by = user;
    pr->walk_end_how = (uint8_t)how;
    /* WALK-010: N of M, on every way it ends. The marks and counts stay as its summary. */
    eng_emit(e, ALARM_EV_WALK_TEST_END, p, 0u, user, src, sat8(pr->walk_tested),
             sat8(pr->walk_total), 0u, REPORT_NOW);
    pr->dirty = true;
}

void eng_walk_mark(alarm_engine_t *e, uint16_t z, alarm_walk_mark_t how)
{
    const uint8_t p = eng_zcfg(e, z)->partition;
    alarm_partition_rt_t *pr = eng_part(e, p);
    alarm_walk_zone_rt_t *w = &e->walk[z - 1u];
    const uint8_t bit = (how == ALARM_WALK_MARK_COVER) ? ALARM_WALK_ZS_COVER : ALARM_WALK_ZS_TESTED;

    if ((w->mark & (ALARM_WALK_ZS_TESTED | ALARM_WALK_ZS_COVER)) == 0u) {
        const alarm_ms_t s = (e->now - pr->walk_started) / 1000u;
        w->mark = (uint8_t)((unsigned)how << ALARM_WALK_ZS_KIND_SHIFT);
        w->first_s = (s > UINT16_MAX) ? UINT16_MAX : (uint16_t)s;
    }
    if ((w->mark & bit) == 0u) {
        /* WALK-011: logged on the zone's first test and its first cover test only. */
        w->mark = (uint8_t)(w->mark | bit);
        pr->walk_seq++;
        if (bit == ALARM_WALK_ZS_TESTED && eng_walk_covers(e, z, pr->walk_flags)) {
            pr->walk_tested++;
        }
        eng_emit(e, ALARM_EV_WALK_TEST_ZONE, p, z, 0u, NULL, (uint8_t)how, 0u, 0u, REPORT_NONE);
    }
    if (w->trips < UINT8_MAX) {
        w->trips++;
    }
    eng_sound(e, p, ALARM_SOUND_WALK_TEST, z); /* every trip, repeats too (WALK-012) */
    pr->dirty = true;
}

void eng_walk_forget(alarm_engine_t *e, uint16_t z)
{
    alarm_walk_zone_rt_t *w = &e->walk[z - 1u];
    if (w->mark != 0u || w->trips != 0u) {
        const uint8_t p = eng_zcfg(e, z)->partition;
        if (p >= 1u && p <= ALARM_MAX_PARTITIONS) {
            eng_part(e, p)->walk_seq++;
        }
    }
    memset(w, 0, sizeof(*w));
}

void eng_walk_timers(alarm_engine_t *e, uint8_t p)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    if (!pr->walk_test) {
        return;
    }
    if (e->now >= pr->walk_test_until) {
        eng_walk_end(e, p, 0u, NULL, ALARM_WALK_END_TIME_LIMIT);
        return;
    }
    /* WALK-004: one double beep at 5 minutes and one at 1 minute before the limit. */
    const alarm_ms_t left = pr->walk_test_until - e->now;
    uint8_t minutes = 0u;
    if ((pr->walk_warned & WARN_1_MIN) == 0u && left <= MS(60u)) {
        pr->walk_warned = (uint8_t)(pr->walk_warned | WARN_1_MIN | WARN_5_MIN);
        minutes = 1u;
    } else if ((pr->walk_warned & WARN_5_MIN) == 0u && left <= MS(300u)) {
        pr->walk_warned = (uint8_t)(pr->walk_warned | WARN_5_MIN);
        minutes = 5u;
    }
    if (minutes != 0u) {
        eng_emit(e, ALARM_EV_WALK_TEST_WARNING, p, 0u, 0u, NULL, minutes, 0u, 0u, REPORT_NOW);
        eng_sound(e, p, ALARM_SOUND_WALK_TEST_WARNING, 0u);
        pr->dirty = true;
    }
}

alarm_ms_t eng_walk_next(const alarm_engine_t *e, uint8_t p)
{
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    if (!pr->walk_test) {
        return ALARM_TIME_NEVER;
    }
    const alarm_ms_t until = pr->walk_test_until;
    if ((pr->walk_warned & WARN_5_MIN) == 0u && until > MS(300u)) {
        return until - MS(300u);
    }
    if ((pr->walk_warned & WARN_1_MIN) == 0u && until > MS(60u)) {
        return until - MS(60u);
    }
    return until;
}
