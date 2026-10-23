/*
 * arming.c - arming, disarming, exit/entry time and the command dispatcher.
 */
#include <string.h>

#include "engine_internal.h"


static alarm_ms_t exit_time_ms(const alarm_engine_t *e, uint8_t p, uint8_t flags)
{
    alarm_ms_t t = MS(e->cfg.partitions[p - 1u].exit_delay_s);
    if ((flags & ALARM_ARM_SILENT_EXIT) != 0u) {
        t *= 2u; /* SIA CP-01: silent exit doubles exit time */
    }
    return t;
}

static bool zone_in_partition(const alarm_engine_t *e, uint16_t z, uint8_t p)
{
    return eng_zone_valid(e, z) && e->cfg.zones[z - 1u].partition == p;
}

/* Forget everything that belongs to one armed period. */
static void reset_period(alarm_engine_t *e, uint8_t p)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (!zone_in_partition(e, z, p)) {
            continue;
        }
        alarm_zone_rt_t *zr = eng_zrt(e, z);
        zr->swinger = 0u;
        zr->flags = (uint16_t)(zr->flags & (uint16_t)~(ZRT_SWINGER | ZRT_CROSS_PENDING));
    }
    memset(pr->followers, 0, sizeof(pr->followers));
    pr->period_alarms = 0u;
    pr->alarm_reported = false;
    pr->abort_until = 0u;
    pr->cancel_until = 0u;
    pr->recent_close_until = 0u;
    pr->recent_close_sent = false;
    pr->exit_restarts = 0u;
    pr->exit_door = 0u;
    pr->exit_fast_done = false;
    pr->entry_exit_error = false;
    pr->entry_zone = 0u;
}

static void clear_memory(alarm_engine_t *e, uint8_t p)
{
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (zone_in_partition(e, z, p)) {
            alarm_zone_rt_t *zr = eng_zrt(e, z);
            zr->flags = (uint16_t)(zr->flags & (uint16_t)~(ZRT_MEMORY | ZRT_SILENT_MEM));
        }
    }
    eng_part(e, p)->memory = 0u;
}

static bool memory_present(const alarm_engine_t *e, uint8_t p)
{
    if (e->parts[p - 1u].memory != 0u) {
        return true;
    }
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (zone_in_partition(e, z, p) && (e->zones[z - 1u].flags & ZRT_MEMORY) != 0u) {
            return true;
        }
    }
    return false;
}

/* CORE-USR-003, USR-001 (decided 2026-10-07): the installer code never arms - the installer
 * programs, the household arms (docs/features/01 "Deliberate choices"). */
static bool can_arm(alarm_authority_t a)
{
    return a != ALARM_AUTH_NONE && a != ALARM_AUTH_INSTALLER;
}

static bool can_disarm(const alarm_engine_t *e, uint8_t p, uint8_t user)
{
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    switch (eng_user_auth(e, user)) {
    case ALARM_AUTH_MASTER:
    case ALARM_AUTH_PARTITION_MASTER:
    case ALARM_AUTH_STANDARD:
    case ALARM_AUTH_DURESS:
        return true;
    case ALARM_AUTH_GUEST:
        /* Only if this code armed the partition. */
        return pr->state == ALARM_STATE_DISARMED || pr->armed_by == user;
    case ALARM_AUTH_INSTALLER:
        /* CORE-USR-003, USR-001: never gives protection up - not in exit time, entry time or an
         * alarm. On a disarmed partition code + OFF still ends a walk test (WALK-025), silences
         * and clears memory, none of which disarms anything. */
        return pr->state == ALARM_STATE_DISARMED;
    default:
        return false;
    }
}

static bool can_bypass(alarm_authority_t a)
{
    return a == ALARM_AUTH_INSTALLER || a == ALARM_AUTH_MASTER ||
           a == ALARM_AUTH_PARTITION_MASTER || a == ALARM_AUTH_STANDARD ||
           a == ALARM_AUTH_DURESS;
}

/* Starting or extending a walk test (WALK-001, CORE-TEST-004): the installer, the system master
 * and a partition master. Ending one needs only can_disarm() (WALK-025). */
static bool can_test(alarm_authority_t a)
{
    return a == ALARM_AUTH_INSTALLER || a == ALARM_AUTH_MASTER ||
           a == ALARM_AUTH_PARTITION_MASTER;
}

/* ------------------------------------------------------------------------ */
/* Arm / disarm                                                              */
/* ------------------------------------------------------------------------ */
static bool from_automation(const alarm_source_t *src)
{
    return src != NULL && src->kind == (uint8_t)ALARM_SRC_AUTOMATION;
}

/* CORE-USR-030: an arming nobody stands at - an automation's or a schedule's - is reported when
 * it fails, so someone hears of it (docs/features/41 AUT-009, AUT-014); a person at a keypad,
 * a browser or a fob sees the failure and is not reported. */
static alarm_result_t arm_failed(alarm_engine_t *e, uint8_t p, uint16_t zone, uint8_t user,
                                 const alarm_source_t *src, alarm_result_t r)
{
    const bool unattended = src != NULL && (src->kind == (uint8_t)ALARM_SRC_AUTOMATION ||
                                            src->kind == (uint8_t)ALARM_SRC_SCHEDULE);
    eng_emit(e, ALARM_EV_ARM_FAILED, p, zone, user, src, (uint8_t)r, 0u, 0u,
             unattended ? REPORT_NOW : REPORT_NONE);
    return r;
}

/* May this blocking zone be bypassed by this arming? BYPASS_FAULTED bypasses any bypassable
 * zone; an automation's only a zone that is just open - one in tamper, loop or missing-sensor
 * trouble fails the arming, as a tampered sensor can be an attack (CORE-USR-031, AUT-014). */
static bool arm_may_bypass(const alarm_engine_t *e, uint16_t z, uint8_t flags,
                           const alarm_source_t *src)
{
    if ((flags & ALARM_ARM_BYPASS_FAULTED) == 0u || !alarm_zone_bypassable(eng_zcfg(e, z))) {
        return false;
    }
    return !from_automation(src) || eng_zone_only_open(e, z);
}

alarm_result_t eng_arm(alarm_engine_t *e, uint8_t p, alarm_arm_level_t level, uint8_t flags,
                       uint8_t user, const alarm_source_t *src, alarm_cmd_result_t *res)
{
    alarm_partition_rt_t *pr = eng_part(e, p);

    if (level != ALARM_LEVEL_AWAY && level != ALARM_LEVEL_STAY && level != ALARM_LEVEL_NIGHT) {
        return ALARM_ERR_BAD_ARG;
    }
    uint8_t mode_flags = (uint8_t)(flags & (ALARM_ARM_NO_ENTRY_DELAY | ALARM_ARM_SILENT_EXIT));
    if (level != ALARM_LEVEL_AWAY) {
        /* Silent exit is Away's: Stay and Night exit time is silent anyway, and
         * isn't doubled. */
        mode_flags = (uint8_t)(mode_flags & (uint8_t)~ALARM_ARM_SILENT_EXIT);
    }
    if (e->program || pr->walk_test || pr->sounding != 0u || pr->state == ALARM_STATE_EXIT_DELAY ||
        pr->state == ALARM_STATE_ENTRY_DELAY ||
        (pr->state == ALARM_STATE_ARMED && pr->level == level && pr->flags == mode_flags)) {
        return arm_failed(e, p, 0u, user, src, ALARM_ERR_BAD_STATE);
    }
    if (e->arm_hold) { /* CORE-ARM-013 */
        return arm_failed(e, p, 0u, user, src, ALARM_ERR_HELD);
    }

    /* Readiness: every zone armed in this level must be intact (or bypassed). */
    bool need_bypass = false;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (!zone_in_partition(e, z, p) || !eng_zone_blocks_level(e, z, level)) {
            continue;
        }
        if (!arm_may_bypass(e, z, flags, src)) {
            if (res != NULL) {
                res->not_ready_zone = z;
            }
            return arm_failed(e, p, z, user, src, ALARM_ERR_NOT_READY);
        }
        need_bypass = true;
    }
    /* An automation's bypasses are marked to be watched again (CORE-USR-033) and counted on the
     * ARMED (CORE-USR-032); a person's last until the disarm. */
    const bool auto_bypass = from_automation(src);
    unsigned bypassed = 0u;
    if (need_bypass) {
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (zone_in_partition(e, z, p) && eng_zone_blocks_level(e, z, level)) {
                alarm_zone_rt_t *zr = eng_zrt(e, z);
                zr->flags = (uint16_t)(zr->flags | ZRT_BYPASSED);
                if (auto_bypass) {
                    zr->flags = (uint16_t)(zr->flags | ZRT_AUTO_BYPASS);
                    zr->rewatch_at = 0u;
                }
                bypassed++;
                eng_emit(e, ALARM_EV_BYPASS, p, z, user, src, 0u, 0u, 0u,
                         eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE);
            }
        }
        if (auto_bypass) {
            pr->auto_bypass_src = *src;
            pr->auto_bypass_user = user;
        }
    }
    uint8_t armed_arg2 = mode_flags;
    if (auto_bypass && bypassed > 0u) {
        const unsigned n = (bypassed > ALARM_ARMED_BYPASSED_MAX) ? ALARM_ARMED_BYPASSED_MAX
                                                                 : bypassed;
        armed_arg2 = (uint8_t)(armed_arg2 | (n << ALARM_ARMED_BYPASSED_SHIFT));
    }

    if (pr->state == ALARM_STATE_DISARMED) {
        clear_memory(e, p); /* arming acknowledges old alarm memory */
    }
    reset_period(e, p);
    pr->level = level;
    pr->flags = mode_flags;
    pr->armed_by = user;
    if (src != NULL) {
        pr->arm_source = *src;
    } else {
        pr->arm_source.kind = ALARM_SRC_SYSTEM;
        pr->arm_source.id = 0u;
    }

    const alarm_ms_t exit_ms = exit_time_ms(e, p, mode_flags);
    pr->quick_exit = false;
    if (exit_ms > 0u) {
        pr->state = ALARM_STATE_EXIT_DELAY;
        pr->deadline = ms_add(e->now, exit_ms);
    } else {
        pr->state = ALARM_STATE_ARMED;
        pr->deadline = 0u;
        pr->recent_close_until = ms_add(e->now, MS(e->cfg.opt.recent_closing_s));
    }

    eng_emit(e, ALARM_EV_ARMED, p, 0u, user, src, (uint8_t)level, armed_arg2, 0u, REPORT_NOW);
    if (eng_user_auth(e, user) == ALARM_AUTH_DURESS) {
        /* A duress code reports when it arms, not only when it disarms. */
        eng_emit(e, ALARM_EV_DURESS, p, 0u, user, src, 0u, 0u, ALARM_EVF_SILENT, REPORT_NOW);
    }
    eng_sound(e, p, ALARM_SOUND_ACK, 0u);
    pr->dirty = true;
    e->persist_dirty = true;
    return ALARM_OK;
}

/* A device's remembered trouble is its partition's (0: every partition's), as it is counted. */
static uint32_t dev_mem_of(const alarm_engine_t *e, uint8_t p)
{
    uint32_t mem = 0u;
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        const uint8_t dp = e->dev_trouble_partition[d];
        if ((e->dev_trouble_mem & (1u << d)) != 0u && (dp == 0u || dp == p)) {
            mem |= 1u << d;
        }
    }
    return mem;
}

static void ack_troubles(alarm_engine_t *e, uint8_t p)
{
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (zone_in_partition(e, z, p)) {
            eng_zrt(e, z)->trouble_mem = 0u;
        }
    }
    e->sys_trouble_mem = 0u;
    e->dev_trouble_mem &= ~dev_mem_of(e, p);
    eng_part(e, p)->trouble_unack = false;
    e->sys_dirty = true;
}

static bool troubles_to_ack(const alarm_engine_t *e, uint8_t p)
{
    if (e->parts[p - 1u].trouble_unack || e->sys_trouble_mem != 0u || dev_mem_of(e, p) != 0u) {
        return true;
    }
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (zone_in_partition(e, z, p) && e->zones[z - 1u].trouble_mem != 0u) {
            return true;
        }
    }
    return false;
}

alarm_result_t eng_disarm(alarm_engine_t *e, uint8_t p, uint8_t user, const alarm_source_t *src)
{
    alarm_partition_rt_t *pr = eng_part(e, p);

    if (pr->state != ALARM_STATE_DISARMED) {
        const bool exit_error = pr->state == ALARM_STATE_ENTRY_DELAY && pr->entry_exit_error;
        const unsigned dropped = eng_reports_drop(e, p);

        if (dropped > 0u || exit_error) {
            /* Disarmed inside the abort window / exit-error entry time: nothing sent. */
            pr->memory = (uint16_t)(pr->memory | ALARM_MEM_CANCELED);
            eng_emit(e, ALARM_EV_ALARM_ABORTED, p, pr->entry_zone, user, src, 0u, 0u, 0u,
                     REPORT_NONE);
        }
        if (exit_error && pr->entry_zone != 0u) {
            alarm_zone_rt_t *zr = eng_zrt(e, pr->entry_zone);
            zr->flags = (uint16_t)(zr->flags | ZRT_MEMORY);
            pr->memory = (uint16_t)(pr->memory | CAUSE_BIT(ALARM_CAUSE_EXIT_ERROR));
        }
        if (pr->alarm_reported && e->now <= pr->cancel_until) {
            pr->memory = (uint16_t)(pr->memory | ALARM_MEM_CANCELED);
            eng_emit(e, ALARM_EV_ALARM_CANCELED, p, 0u, user, src, 0u, 0u, 0u, REPORT_NOW);
        }

        eng_silence(e, p, 0xFFFFu);
        pr->strobe = false;
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (zone_in_partition(e, z, p)) {
                alarm_zone_rt_t *zr = eng_zrt(e, z);
                zr->flags = (uint16_t)(zr->flags & (uint16_t)~(ZRT_BYPASSED | ZRT_AUTO_BYPASS));
                zr->rewatch_at = 0u;
            }
        }
        pr->state = ALARM_STATE_DISARMED;
        pr->level = ALARM_LEVEL_NONE;
        pr->flags = 0u;
        pr->deadline = 0u;
        pr->armed_by = 0u;
        reset_period(e, p);

        eng_emit(e, ALARM_EV_DISARMED, p, 0u, user, src, 0u, 0u, 0u, REPORT_NOW);
        if (eng_user_auth(e, user) == ALARM_AUTH_DURESS) {
            eng_emit(e, ALARM_EV_DURESS, p, 0u, user, src, 0u, 0u, ALARM_EVF_SILENT, REPORT_NOW);
        }
        eng_sound(e, p, ALARM_SOUND_ACK, 0u);
        pr->dirty = true;
        e->persist_dirty = true;
        return ALARM_OK;
    }

    /* Already disarmed: silence, else end walk test, else clear memory + ack troubles. */
    if (pr->sounding != 0u) {
        eng_silence(e, p, 0xFFFFu);
        pr->strobe = false;
        eng_emit(e, ALARM_EV_ALARM_SILENCED, p, 0u, user, src, 0u, 0u, 0u, REPORT_NONE);
    } else if (pr->walk_test) {
        eng_walk_end(e, p, user, src, ALARM_WALK_END_DISARM); /* WALK-025 */
        if (eng_user_auth(e, user) == ALARM_AUTH_DURESS) {
            eng_emit(e, ALARM_EV_DURESS, p, 0u, user, src, 0u, 0u, ALARM_EVF_SILENT, REPORT_NOW);
        }
    } else {
        if (memory_present(e, p)) {
            const bool fire = (pr->memory & FIRE_CAUSES) != 0u;
            if (fire) {
                /* Reset smoke detectors; zones still in alarm afterwards re-alarm. */
                for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
                    if (zone_in_partition(e, z, p) &&
                        alarm_zone_type_is_fire(eng_zcfg(e, z)->type)) {
                        alarm_zone_rt_t *zr = eng_zrt(e, z);
                        zr->flags = (uint16_t)(zr->flags & (uint16_t)~ZRT_IN_ALARM);
                    }
                }
                eng_smoke_reset_start(e);
            }
            clear_memory(e, p);
            eng_emit(e, ALARM_EV_MEMORY_CLEARED, p, 0u, user, src, 0u, 0u, 0u, REPORT_NONE);
            e->persist_dirty = true;
        }
        if (troubles_to_ack(e, p)) {
            ack_troubles(e, p);
            eng_emit(e, ALARM_EV_TROUBLES_ACKED, p, 0u, user, src, 0u, 0u, 0u, REPORT_NONE);
        }
    }
    eng_sound(e, p, ALARM_SOUND_ACK, 0u);
    pr->dirty = true;
    return ALARM_OK;
}

/* ------------------------------------------------------------------------ */
/* Exit / entry time                                                         */
/* ------------------------------------------------------------------------ */
void eng_exit_restart(alarm_engine_t *e, uint8_t p, bool automatic, uint8_t user,
                      const alarm_source_t *src)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    /* from the armed state a quick exit; within an arming's exit time it stays that arming's */
    pr->quick_exit = pr->state != ALARM_STATE_EXIT_DELAY || pr->quick_exit;
    pr->state = ALARM_STATE_EXIT_DELAY;
    pr->deadline = ms_add(e->now, exit_time_ms(e, p, pr->flags));
    pr->exit_fast_done = false;
    pr->exit_door = 0u;
    eng_emit(e, ALARM_EV_EXIT_RESTART, p, 0u, user, src, automatic ? 1u : 0u, 0u, 0u,
             REPORT_NONE);
    pr->dirty = true;
}

static uint16_t entry_delay_s(const alarm_engine_t *e, uint8_t p, uint16_t z)
{
    const alarm_partition_config_t *pc = &e->cfg.partitions[p - 1u];
    return (eng_zcfg(e, z)->type == ALARM_ZT_ENTRY_EXIT_2) ? pc->entry_delay2_s
                                                           : pc->entry_delay1_s;
}

void eng_start_entry(alarm_engine_t *e, uint8_t p, uint16_t z)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    pr->state = ALARM_STATE_ENTRY_DELAY;
    pr->deadline = ms_add(e->now, MS(entry_delay_s(e, p, z)));
    pr->entry_zone = z;
    pr->entry_exit_error = false;
    memset(pr->followers, 0, sizeof(pr->followers));
    eng_emit(e, ALARM_EV_ENTRY_DELAY, p, z, 0u, NULL, 0u, 0u, 0u, REPORT_NONE);
    pr->dirty = true;
}

static bool exit_error_zone(const alarm_engine_t *e, uint8_t p, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    const alarm_zone_rt_t *zr = &e->zones[z - 1u];
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    const bool candidate = zc->type == ALARM_ZT_ENTRY_EXIT_1 ||
                           zc->type == ALARM_ZT_ENTRY_EXIT_2 ||
                           zc->type == ALARM_ZT_INTERIOR_FOLLOWER ||
                           zc->type == ALARM_ZT_INTERIOR_DELAY || zc->type == ALARM_ZT_DAY_NIGHT;
    return candidate && alarm_zone_armed_in_level(zc, pr->level) &&
           (zr->flags & (ZRT_BYPASSED | ZRT_SWINGER)) == 0u && (zr->cond & ZC_FAULT) != 0u;
}

static void exit_expired(alarm_engine_t *e, uint8_t p)
{
    alarm_partition_rt_t *pr = eng_part(e, p);

    /* Auto-stay (unvacated premises): nobody opened and closed an exit door.
     * Not for remote / keyfob / keyswitch / scheduled arming. */
    if (pr->level == ALARM_LEVEL_AWAY && e->cfg.opt.auto_stay && pr->exit_door == 0u &&
        pr->arm_source.kind == ALARM_SRC_KEYPAD) {
        pr->level = ALARM_LEVEL_STAY;
        pr->flags = (uint8_t)(pr->flags & (uint8_t)~ALARM_ARM_SILENT_EXIT); /* Instant stays */
        eng_emit(e, ALARM_EV_AUTO_STAY, p, 0u, pr->armed_by, &pr->arm_source,
                 (uint8_t)ALARM_LEVEL_STAY, pr->flags, 0u, REPORT_NOW);
        e->persist_dirty = true;
    }

    /* Exit error: a door or interior zone still faulted when exit time ends. */
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (!zone_in_partition(e, z, p) || !exit_error_zone(e, p, z)) {
            continue;
        }
        pr->state = ALARM_STATE_ENTRY_DELAY;
        pr->deadline = ms_add(e->now, MS(entry_delay_s(e, p, z)));
        pr->entry_zone = z;
        pr->entry_exit_error = true;
        memset(pr->followers, 0, sizeof(pr->followers));
        eng_start_sounding(e, p, ALARM_CAUSE_EXIT_ERROR); /* local alarm sounds at once */
        eng_emit(e, ALARM_EV_EXIT_ERROR, p, z, pr->armed_by, NULL, 0u, 0u, 0u, REPORT_NONE);
        pr->dirty = true;
        return;
    }

    pr->state = ALARM_STATE_ARMED;
    pr->deadline = 0u;
    pr->recent_close_until = ms_add(e->now, MS(e->cfg.opt.recent_closing_s));
    pr->recent_close_sent = false;
    pr->dirty = true;
}

static void entry_expired(alarm_engine_t *e, uint8_t p)
{
    alarm_partition_rt_t *pr = eng_part(e, p);
    const uint16_t z = pr->entry_zone;
    const alarm_cause_t cause =
        pr->entry_exit_error ? ALARM_CAUSE_EXIT_ERROR : ALARM_CAUSE_BURGLARY;

    pr->state = ALARM_STATE_ARMED;
    pr->deadline = 0u;
    pr->entry_exit_error = false;
    pr->entry_zone = 0u;
    if (z != 0u) {
        eng_alarm(e, p, z, cause, NULL, 0u);
    }
    for (uint16_t f = 1u; f <= ALARM_MAX_ZONES; f++) {
        if (f != z && bm_get(pr->followers, f) && zone_in_partition(e, f, p)) {
            eng_alarm(e, p, f, ALARM_CAUSE_BURGLARY, NULL, 0u);
        }
    }
    memset(pr->followers, 0, sizeof(pr->followers));
    pr->dirty = true;
}

void eng_partition_timers(alarm_engine_t *e, uint8_t p)
{
    alarm_partition_rt_t *pr = eng_part(e, p);

    if (pr->state == ALARM_STATE_EXIT_DELAY && !pr->exit_fast_done &&
        e->now + 10000u >= pr->deadline) {
        pr->exit_fast_done = true; /* status: switch to fast exit beeps */
        pr->dirty = true;
    }
    if (pr->state == ALARM_STATE_EXIT_DELAY && e->now >= pr->deadline) {
        exit_expired(e, p);
    } else if (pr->state == ALARM_STATE_ENTRY_DELAY && e->now >= pr->deadline) {
        entry_expired(e, p);
    }
    eng_sounding_timers(e, p);
    eng_walk_timers(e, p);
}

alarm_ms_t eng_partition_next(const alarm_engine_t *e, uint8_t p)
{
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    alarm_ms_t t = ALARM_TIME_NEVER;
    if (pr->state == ALARM_STATE_EXIT_DELAY || pr->state == ALARM_STATE_ENTRY_DELAY) {
        t = pr->deadline;
    }
    if (pr->state == ALARM_STATE_EXIT_DELAY && !pr->exit_fast_done) {
        const alarm_ms_t fast = (pr->deadline > 10000u) ? pr->deadline - 10000u : 0u;
        if (fast < t) {
            t = fast;
        }
    }
    if (pr->bell_until != 0u && pr->bell_until < t) {
        t = pr->bell_until;
    }
    if (pr->fire_until != 0u && pr->fire_until < t) {
        t = pr->fire_until;
    }
    const alarm_ms_t walk = eng_walk_next(e, p);
    if (walk < t) {
        t = walk;
    }
    return t;
}

/* ------------------------------------------------------------------------ */
/* An automation's bypasses watched again (CORE-USR-033, docs/features/41 AUT-014)            */
/* ------------------------------------------------------------------------ */
static bool rewatching(const alarm_engine_t *e, uint16_t z)
{
    const uint16_t f = e->zones[z - 1u].flags;
    return (f & ZRT_AUTO_BYPASS) != 0u && (f & ZRT_BYPASSED) != 0u && eng_zone_valid(e, z) &&
           eng_partition_valid(e, e->cfg.zones[z - 1u].partition) &&
           e->parts[e->cfg.zones[z - 1u].partition - 1u].state != ALARM_STATE_DISARMED;
}

void eng_rewatch_sync(alarm_engine_t *e)
{
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        alarm_zone_rt_t *zr = eng_zrt(e, z);
        if ((zr->flags & ZRT_AUTO_BYPASS) == 0u) {
            continue;
        }
        if (!rewatching(e, z)) {
            /* Unbypassed, disarmed or reconfigured: nothing left to watch. */
            zr->flags = (uint16_t)(zr->flags & (uint16_t)~ZRT_AUTO_BYPASS);
            zr->rewatch_at = 0u;
        } else if (eng_zone_not_ready(e, z)) {
            zr->rewatch_at = 0u; /* open or in trouble again: the wait starts over */
        } else if (zr->rewatch_at == 0u) {
            zr->rewatch_at = ms_add(e->now, REWATCH_MS);
        }
    }
}

void eng_rewatch_due(alarm_engine_t *e)
{
    eng_rewatch_sync(e); /* a timer just before this one may have changed a zone */
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        alarm_zone_rt_t *zr = eng_zrt(e, z);
        if ((zr->flags & ZRT_AUTO_BYPASS) == 0u || zr->rewatch_at == 0u ||
            e->now < zr->rewatch_at) {
            continue;
        }
        const uint8_t p = eng_zcfg(e, z)->partition;
        alarm_partition_rt_t *pr = eng_part(e, p);
        zr->flags = (uint16_t)(zr->flags & (uint16_t)~(ZRT_BYPASSED | ZRT_AUTO_BYPASS));
        zr->rewatch_at = 0u;
        eng_emit(e, ALARM_EV_UNBYPASS, p, z, pr->auto_bypass_user, &pr->auto_bypass_src, 0u,
                 0u, 0u, eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE);
        pr->dirty = true;
        e->persist_dirty = true;
    }
}

alarm_ms_t eng_rewatch_next(const alarm_engine_t *e)
{
    alarm_ms_t t = ALARM_TIME_NEVER;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        const alarm_zone_rt_t *zr = &e->zones[z - 1u];
        if ((zr->flags & ZRT_AUTO_BYPASS) != 0u && zr->rewatch_at != 0u && zr->rewatch_at < t) {
            t = zr->rewatch_at;
        }
    }
    return t;
}

/* ------------------------------------------------------------------------ */
/* Button / keyswitch zones                                                  */
/* ------------------------------------------------------------------------ */
void eng_button_zone(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    const uint8_t p = zc->partition;
    const uint8_t user = zc->user;
    const alarm_source_t src = {ALARM_SRC_ZONE, z};
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    const alarm_authority_t auth = eng_user_auth(e, user);

    /* A button is the user's code in a different shape: it obeys the same authority and the
     * same partition scope, asked again on every press (CORE-USR-015). A user who lost the
     * partition - or the whole slot - loses the button with it. */
    if (!can_arm(auth) || !eng_user_in_partition(e, user, p)) {
        return;
    }
    switch (zc->type) {
    case ALARM_ZT_ARM_STAY:
        (void)eng_arm(e, p, ALARM_LEVEL_STAY, 0u, user, &src, NULL);
        break;
    case ALARM_ZT_ARM_AWAY:
        (void)eng_arm(e, p, ALARM_LEVEL_AWAY, 0u, user, &src, NULL);
        break;
    case ALARM_ZT_DISARM:
        if (can_disarm(e, p, user)) {
            (void)eng_disarm(e, p, user, &src);
        }
        break;
    case ALARM_ZT_KEYSWITCH:
        if (pr->state == ALARM_STATE_DISARMED) {
            (void)eng_arm(e, p, ALARM_LEVEL_AWAY, 0u, user, &src, NULL);
        } else if (can_disarm(e, p, user)) {
            (void)eng_disarm(e, p, user, &src);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */
static bool code_given(const alarm_cmd_t *cmd)
{
    return cmd->code[0] != '\0';
}

static bool quick_allowed(const alarm_engine_t *e, const alarm_source_t *src)
{
    switch (src->kind) {
    case ALARM_SRC_SYSTEM:
    case ALARM_SRC_SCHEDULE:
        return true;
    case ALARM_SRC_KEYPAD:
        return e->cfg.opt.quick_arm;
    case ALARM_SRC_REMOTE:
        return e->cfg.opt.remote_arm_without_code;
    default:
        return false;
    }
}

static bool panic_cause(alarm_panic_t panic, alarm_cause_t *cause)
{
    switch (panic) {
    case ALARM_PANIC_POLICE:
        *cause = ALARM_CAUSE_PANIC_POLICE;
        return true;
    case ALARM_PANIC_SILENT:
        *cause = ALARM_CAUSE_PANIC_SILENT;
        return true;
    case ALARM_PANIC_FIRE:
        *cause = ALARM_CAUSE_PANIC_FIRE;
        return true;
    case ALARM_PANIC_MEDICAL:
        *cause = ALARM_CAUSE_PANIC_MEDICAL;
        return true;
    default:
        return false;
    }
}

static alarm_result_t cmd_bypass(alarm_engine_t *e, const alarm_cmd_t *cmd, uint8_t user,
                                 const alarm_source_t *src, alarm_cmd_result_t *res)
{
    const uint8_t p = cmd->partition;
    if (eng_part(e, p)->state != ALARM_STATE_DISARMED) {
        return ALARM_ERR_BAD_STATE; /* bypass while disarmed only */
    }

    if (cmd->type == ALARM_CMD_QUICK_BYPASS) {
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (!zone_in_partition(e, z, p)) {
                continue;
            }
            const alarm_zone_config_t *zc = eng_zcfg(e, z);
            alarm_zone_rt_t *zr = eng_zrt(e, z);
            if (!alarm_zone_type_is_burglary(zc->type) || (zr->flags & ZRT_BYPASSED) != 0u ||
                !eng_zone_not_ready(e, z)) {
                continue;
            }
            zr->flags = (uint16_t)(zr->flags | ZRT_BYPASSED);
            eng_emit(e, ALARM_EV_BYPASS, p, z, user, src, 0u, 0u, 0u,
                     eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE);
        }
    } else {
        const uint16_t z = cmd->zone;
        if (!zone_in_partition(e, z, p)) {
            return ALARM_ERR_BAD_ARG;
        }
        alarm_zone_rt_t *zr = eng_zrt(e, z);
        const bool bypass = cmd->type == ALARM_CMD_BYPASS;
        if (bypass && !alarm_zone_bypassable(eng_zcfg(e, z))) {
            if (res != NULL) {
                res->not_ready_zone = z;
            }
            return ALARM_ERR_NOT_BYPASSABLE;
        }
        const bool is_bypassed = (zr->flags & ZRT_BYPASSED) != 0u;
        if (bypass != is_bypassed) {
            if (bypass) {
                zr->flags = (uint16_t)(zr->flags | ZRT_BYPASSED);
            } else {
                zr->flags = (uint16_t)(zr->flags & (uint16_t)~ZRT_BYPASSED);
            }
            eng_emit(e, bypass ? ALARM_EV_BYPASS : ALARM_EV_UNBYPASS, p, z, user, src, 0u, 0u,
                     0u, eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE);
        }
    }
    eng_sound(e, p, ALARM_SOUND_ACK, 0u);
    eng_part(e, p)->dirty = true;
    e->persist_dirty = true;
    return ALARM_OK;
}

/* forward: identify() is below */
static alarm_result_t identify(alarm_engine_t *e, const alarm_cmd_t *cmd, const alarm_source_t *src,
                               bool as, uint8_t *user);

/* ALARM_CMD_WALK_TEST (docs/features/40). Start, end and extend are separate, so a retried
 * request never undoes itself (WALK-002); the keypad's toggle picks one by the state. Who may:
 * can_test() starts and extends (WALK-001), whoever may disarm the partition ends (WALK-025). */
static alarm_result_t cmd_walk_test(alarm_engine_t *e, const alarm_cmd_t *cmd,
                                    const alarm_source_t *src, bool as, uint8_t user,
                                    alarm_cmd_result_t *res)
{
    const uint8_t p = cmd->partition;
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    if (cmd->walk_op > (uint8_t)ALARM_WALK_EXTEND) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!as && !code_given(cmd) && cmd->walk_op == (uint8_t)ALARM_WALK_END) {
        /* WALK-025 (decided 2026-10-01): an explicit End needs no code - it only gives protection
         * back (every zone alarms again), so a code there guards nothing. The system ends it. */
        if (!pr->walk_test) {
            return ALARM_ERR_BAD_STATE;
        }
        res->user = 0u;
        eng_walk_end(e, p, 0u, src, ALARM_WALK_END_COMMAND);
        eng_sound(e, p, ALARM_SOUND_ACK, 0u);
        return ALARM_OK;
    }
    if (!as && !code_given(cmd)) {
        return ALARM_ERR_CODE_REQUIRED;
    }
    const alarm_result_t r = identify(e, cmd, src, as, &user);
    if (r != ALARM_OK) {
        return r;
    }
    res->user = user;

    alarm_walk_op_t op = (alarm_walk_op_t)cmd->walk_op;
    if (op == ALARM_WALK_TOGGLE) {
        op = pr->walk_test ? ALARM_WALK_END : ALARM_WALK_START;
    }
    const bool allowed = (op == ALARM_WALK_END) ? can_disarm(e, p, user)
                                                : can_test(eng_user_auth(e, user));
    if (!allowed) {
        return ALARM_ERR_NOT_AUTHORIZED;
    }
    switch (op) {
    case ALARM_WALK_START:
        if (pr->walk_test || pr->state != ALARM_STATE_DISARMED || pr->sounding != 0u ||
            e->program) {
            return ALARM_ERR_BAD_STATE;
        }
        eng_walk_start(e, p, cmd->walk_flags, user, src);
        break;
    case ALARM_WALK_EXTEND:
        if (!pr->walk_test) {
            return ALARM_ERR_BAD_STATE;
        }
        eng_walk_extend(e, p, user, src);
        break;
    default: /* ALARM_WALK_END */
        if (!pr->walk_test) {
            return ALARM_ERR_BAD_STATE;
        }
        eng_walk_end(e, p, user, src, ALARM_WALK_END_COMMAND);
        if (eng_user_auth(e, user) == ALARM_AUTH_DURESS) { /* as a disarm by that code */
            eng_emit(e, ALARM_EV_DURESS, p, 0u, user, src, 0u, 0u, ALARM_EVF_SILENT, REPORT_NOW);
        }
        break;
    }
    eng_sound(e, p, ALARM_SOUND_ACK, 0u);
    return ALARM_OK;
}

/* Who is giving a command: the user its code names (counts toward lockout), or - for a command
 * run as an already-identified user - that user, checked before dispatch() got this far. */
static alarm_result_t identify(alarm_engine_t *e, const alarm_cmd_t *cmd, const alarm_source_t *src,
                               bool as, uint8_t *user)
{
    if (as) {
        return ALARM_OK;
    }
    return eng_auth(e, src, cmd->code, cmd->partition, user);
}

/* May the disarm let a code's user give a partition's protection up? can_disarm() as if another
 * user had armed it: a guest only what it armed itself, so never while it is disarmed, and the
 * installer never (CORE-USR-003) (ALARM_CMD_CODE_CHECK, CORE-USR-034). */
static bool may_disarm(const alarm_engine_t *e, uint8_t p, uint8_t user)
{
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    switch (eng_user_auth(e, user)) {
    case ALARM_AUTH_MASTER:
    case ALARM_AUTH_PARTITION_MASTER:
    case ALARM_AUTH_STANDARD:
    case ALARM_AUTH_DURESS:
        return true;
    case ALARM_AUTH_GUEST:
        return pr->state != ALARM_STATE_DISARMED && pr->armed_by == user;
    default:
        return false;
    }
}

/* The automation source (docs/features/41 AUT-002 layer 7, AUT-011, AUT-015, D22; CORE-USR-029):
 * before any code check, lockout count or event. Only two commands pass, and only run as the
 * person who saved the automation (alarm_engine_command_as; dispatch() then checks that this
 * user still holds the partition, and can_arm or can_disarm, as for anyone):
 *  - an arm at Away, Stay or Night from disarmed, with no arm flag but BYPASS_FAULTED (AUT-014:
 *    eng_arm bypasses open burglary zones only), and that flag only for a user who may bypass
 *    and while opt.auto_less_secure is on;
 *  - a disarm of an armed partition while opt.auto_less_secure is on and no alarm has happened
 *    in this armed period, sounding or silent (an automation never silences one or drops or
 *    cancels its report, D22).
 * Everything else - panic, silence, bypass, unbypass, quick bypass, chime, exit restart, walk
 * test, code check, any command not run as a user - is NOT_AUTHORIZED, with the error sound and
 * no event. A partition in the wrong state is BAD_STATE, also without an event: an automation
 * never re-arms at another level (AUT-011), and never disarms a disarmed partition, which would
 * silence, end a walk test or acknowledge troubles. */
static alarm_result_t automation_guard(const alarm_engine_t *e, const alarm_cmd_t *cmd, bool as,
                                       uint8_t as_user)
{
    const uint8_t p = cmd->partition;
    if (!as || !eng_partition_valid(e, p)) {
        return ALARM_ERR_NOT_AUTHORIZED;
    }
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    switch (cmd->type) {
    case ALARM_CMD_ARM:
        if ((cmd->level != ALARM_LEVEL_AWAY && cmd->level != ALARM_LEVEL_STAY &&
             cmd->level != ALARM_LEVEL_NIGHT) ||
            (cmd->arm_flags & (uint8_t)~ALARM_ARM_BYPASS_FAULTED) != 0u) {
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        /* "Bypass open zones" (41 D29) is under the installer option "Allow less secure
         * automations" too (decided 2026-10-05): the service arms without the flag while it is
         * off, so a flag reaching here then is refused, never honoured. */
        if ((cmd->arm_flags & ALARM_ARM_BYPASS_FAULTED) != 0u &&
            (!e->cfg.opt.auto_less_secure || !can_bypass(eng_user_auth(e, as_user)))) {
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        return (pr->state == ALARM_STATE_DISARMED) ? ALARM_OK : ALARM_ERR_BAD_STATE;
    case ALARM_CMD_DISARM:
        /* Never while it sounds, and never after an alarm of this armed period at all (D22): a
         * silent burglary or hold-up never sounds and its report waits out the abort window, and
         * a timed-out bell's cancel window is still open - a disarm then would drop that report or
         * send a cancel in the automation's name. A person disarms those. period_alarms covers
         * both windows: they open only with an alarm and, like it, reset when arming starts. */
        if (!e->cfg.opt.auto_less_secure || pr->sounding != 0u ||
            pr->period_alarms != 0u) { /* AUT-015, D22 */
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        return (pr->state != ALARM_STATE_DISARMED) ? ALARM_OK : ALARM_ERR_BAD_STATE;
    case ALARM_CMD_CHIME:
        /* docs/features/41 BL-224: an automation turns the door chime on or off as the person
         * who saved it (CORE-CHIME-008); only while disarmed, as anyone */
        return (pr->state == ALARM_STATE_DISARMED) ? ALARM_OK : ALARM_ERR_BAD_STATE;
    default:
        return ALARM_ERR_NOT_AUTHORIZED;
    }
}

/* as: run as the already-identified as_user instead of by the command's code (CORE-USR-027). */
static alarm_result_t dispatch(alarm_engine_t *e, const alarm_cmd_t *cmd,
                               const alarm_source_t *src, bool as, uint8_t as_user,
                               alarm_cmd_result_t *res)
{
    if (src->kind == (uint8_t)ALARM_SRC_AUTOMATION) {
        const alarm_result_t g = automation_guard(e, cmd, as, as_user);
        if (g != ALARM_OK) {
            return g;
        }
    }
    const uint8_t p = cmd->partition;
    if (!eng_partition_valid(e, p)) {
        return ALARM_ERR_BAD_ARG;
    }
    alarm_partition_rt_t *pr = eng_part(e, p);
    uint8_t user = 0u;
    alarm_result_t r;

    /* A signed-in user is their code in a different shape, as a button zone is (CORE-USR-015):
     * the slot must hold a user and the user must have the partition, asked again every time.
     * Every command needs it, even the ones a keypad runs without a code: nobody stands at
     * this panel's keys, so the sign-in is the only thing that makes the command anyone's. */
    if (as) {
        if (eng_user_auth(e, as_user) == ALARM_AUTH_NONE ||
            !eng_user_in_partition(e, as_user, p)) {
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        user = as_user;
    }
    const bool given = as || code_given(cmd);

    switch (cmd->type) {
    case ALARM_CMD_PANIC: {
        alarm_cause_t cause;
        if (!panic_cause(cmd->panic, &cause)) {
            return ALARM_ERR_BAD_ARG;
        }
        res->user = user;
        eng_alarm(e, p, 0u, cause, src, user);
        return ALARM_OK;
    }

    case ALARM_CMD_SILENCE: {
        res->user = user;
        const uint16_t quiet = (uint16_t)(FIRE_CAUSES | CO_CAUSES | AUX_CAUSES);
        if ((pr->sounding & quiet) != 0u) {
            eng_silence(e, p, quiet);
            if (pr->sounding == 0u) {
                pr->strobe = false;
            }
            eng_emit(e, ALARM_EV_ALARM_SILENCED, p, 0u, user, src, 0u, 0u, 0u, REPORT_NONE);
        }
        if (pr->trouble_unack) {
            pr->trouble_unack = false;
            pr->dirty = true;
        }
        return ALARM_OK;
    }

    case ALARM_CMD_EXIT_RESTART:
        if (pr->state != ALARM_STATE_ARMED ||
            (pr->level != ALARM_LEVEL_STAY && pr->level != ALARM_LEVEL_NIGHT)) {
            return ALARM_ERR_BAD_STATE;
        }
        if (given) {
            r = identify(e, cmd, src, as, &user);
            if (r != ALARM_OK) {
                return r;
            }
            /* Restarting exit time opens the doors for a while: an arming, so not for the
             * installer (CORE-USR-003). */
            if (!can_arm(eng_user_auth(e, user))) {
                return ALARM_ERR_NOT_AUTHORIZED;
            }
        } else if (!e->cfg.opt.quick_exit) {
            return ALARM_ERR_CODE_REQUIRED;
        }
        res->user = user;
        eng_exit_restart(e, p, false, user, src);
        eng_sound(e, p, ALARM_SOUND_ACK, 0u);
        return ALARM_OK;

    case ALARM_CMD_ARM:
        if (given) {
            r = identify(e, cmd, src, as, &user);
            if (r != ALARM_OK) {
                return r;
            }
            if (!can_arm(eng_user_auth(e, user))) {
                return ALARM_ERR_NOT_AUTHORIZED;
            }
        } else if (!quick_allowed(e, src)) {
            return ALARM_ERR_CODE_REQUIRED;
        }
        res->user = user;
        return eng_arm(e, p, cmd->level, cmd->arm_flags, user, src, res);

    case ALARM_CMD_DISARM:
        if (!given) {
            return ALARM_ERR_CODE_REQUIRED;
        }
        r = identify(e, cmd, src, as, &user);
        if (r != ALARM_OK) {
            return r;
        }
        res->user = user;
        if (!can_disarm(e, p, user)) {
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        return eng_disarm(e, p, user, src);

    case ALARM_CMD_BYPASS:
    case ALARM_CMD_UNBYPASS:
    case ALARM_CMD_QUICK_BYPASS:
        if (!given) {
            return ALARM_ERR_CODE_REQUIRED;
        }
        r = identify(e, cmd, src, as, &user);
        if (r != ALARM_OK) {
            return r;
        }
        res->user = user;
        if (!can_bypass(eng_user_auth(e, user))) {
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        return cmd_bypass(e, cmd, user, src, res);

    case ALARM_CMD_CHIME:
        /* A code by default; the installer can make chime one touch (CORE-CHIME-005). The code
         * guards the keypad: a remote client (Home Assistant's paired link, MQTT) proved who it
         * is when it connected, as the Panel Manager's sign-in does, so it switches the chime
         * without one (CORE-CHIME-007). */
        if (given) {
            r = identify(e, cmd, src, as, &user);
            if (r != ALARM_OK) {
                return r;
            }
        } else if (e->cfg.opt.chime_needs_code && src->kind != (uint8_t)ALARM_SRC_REMOTE) {
            return ALARM_ERR_CODE_REQUIRED;
        }
        res->user = user;
        if (pr->state != ALARM_STATE_DISARMED) {
            return ALARM_ERR_BAD_STATE; /* chime mode only while disarmed */
        }
        pr->chime = !pr->chime;
        eng_emit(e, pr->chime ? ALARM_EV_CHIME_ON : ALARM_EV_CHIME_OFF, p, 0u, user, src, 0u, 0u,
                 0u, REPORT_NONE);
        if (src->kind != (uint8_t)ALARM_SRC_AUTOMATION) {
            eng_sound(e, p, ALARM_SOUND_ACK, 0u); /* nobody at the keys to hear it (BL-224) */
        }
        pr->dirty = true;
        e->persist_dirty = true;
        return ALARM_OK;

    case ALARM_CMD_WALK_TEST:
        return cmd_walk_test(e, cmd, src, as, user, res);

    case ALARM_CMD_CODE_CHECK:
        /* CORE-USR-034: authenticate as a disarm would (lockout counts), change nothing. */
        if (!given) {
            return ALARM_ERR_CODE_REQUIRED;
        }
        r = identify(e, cmd, src, as, &user);
        if (r != ALARM_OK) {
            return r;
        }
        res->user = user;
        res->may_disarm = may_disarm(e, p, user);
        if (eng_user_auth(e, user) == ALARM_AUTH_DURESS) { /* as a disarm by that code */
            eng_emit(e, ALARM_EV_DURESS, p, 0u, user, src, 0u, 0u, ALARM_EVF_SILENT, REPORT_NOW);
        }
        return ALARM_OK;

    default:
        return ALARM_ERR_BAD_ARG;
    }
}

static alarm_result_t run_command(alarm_engine_t *e, const alarm_cmd_t *cmd,
                                  const alarm_source_t *src, bool as, uint8_t as_user,
                                  alarm_ms_t now, alarm_cmd_result_t *result)
{
    if (e == NULL || cmd == NULL || !e->initialized) {
        return ALARM_ERR_BAD_ARG;
    }
    if (e->in_call) {
        return ALARM_ERR_BAD_STATE;
    }
    e->in_call = true;
    eng_advance(e, now);

    alarm_cmd_result_t local;
    alarm_cmd_result_t *res = (result != NULL) ? result : &local;
    memset(res, 0, sizeof(*res));
    alarm_source_t source = {ALARM_SRC_SYSTEM, 0u};
    if (src != NULL) {
        source = *src;
    }

    const alarm_result_t r = dispatch(e, cmd, &source, as, as_user, res);
    /* An automation refused after a silent alarm stays silent too: its error beep would be the
     * one sound such an alarm makes (CORE-ALM-010, LOG-008). */
    if (r != ALARM_OK && eng_partition_valid(e, cmd->partition) &&
        !(source.kind == (uint8_t)ALARM_SRC_AUTOMATION &&
          (e->parts[cmd->partition - 1u].period_alarms & SILENT_CAUSES) != 0u)) {
        eng_sound(e, cmd->partition, ALARM_SOUND_ERROR, 0u);
    }
    eng_finish(e);
    e->in_call = false;
    return r;
}

alarm_result_t alarm_engine_command(alarm_engine_t *e, const alarm_cmd_t *cmd,
                                    const alarm_source_t *src, alarm_ms_t now,
                                    alarm_cmd_result_t *result)
{
    return run_command(e, cmd, src, false, 0u, now, result);
}

alarm_result_t alarm_engine_command_as(alarm_engine_t *e, const alarm_cmd_t *cmd,
                                       const alarm_source_t *src, uint8_t user, alarm_ms_t now,
                                       alarm_cmd_result_t *result)
{
    return run_command(e, cmd, src, true, user, now, result);
}
