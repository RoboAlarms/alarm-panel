/*
 * zones.c - zone input processing: burglary response by arm state, 24-hour,
 * fire (with verification), CO, monitor, troubles, supervision.
 */
#include "engine_internal.h"

#define TRBL_BIT(k) ((uint8_t)(1u << (unsigned)(k)))
#define BLOCKING_TROUBLES \
    (TRBL_BIT(ALARM_ZTRBL_LOOP) | TRBL_BIT(ALARM_ZTRBL_TAMPER) | TRBL_BIT(ALARM_ZTRBL_SUPERVISION))

static bool settling(const alarm_engine_t *e)
{
    return e->settle_until != 0u && e->now < e->settle_until;
}

static alarm_cause_t burglary_cause(alarm_zone_type_t type)
{
    return (type == ALARM_ZT_SILENT_BURGLARY) ? ALARM_CAUSE_SILENT_BURGLARY
                                              : ALARM_CAUSE_BURGLARY;
}

bool eng_zone_not_ready(const alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_rt_t *zr = &e->zones[z - 1u];
    return (zr->cond & ZC_FAULT) != 0u || (zr->trouble & BLOCKING_TROUBLES) != 0u;
}

bool eng_zone_only_open(const alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_rt_t *zr = &e->zones[z - 1u];
    return (zr->cond & ZC_FAULT) != 0u && (zr->trouble & BLOCKING_TROUBLES) == 0u;
}

bool eng_zone_blocks_level(const alarm_engine_t *e, uint16_t z, alarm_arm_level_t level)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    if (!alarm_zone_type_is_burglary(zc->type)) {
        return false;
    }
    if ((e->zones[z - 1u].flags & ZRT_BYPASSED) != 0u) {
        return false;
    }
    return alarm_zone_armed_in_level(zc, level) && eng_zone_not_ready(e, z);
}

void eng_zone_set_trouble(alarm_engine_t *e, uint16_t z, alarm_zone_trouble_t kind, bool on)
{
    alarm_zone_rt_t *zr = eng_zrt(e, z);
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    alarm_partition_rt_t *pr = eng_part(e, zc->partition);
    const uint8_t bit = TRBL_BIT(kind);
    const report_mode_t mode = eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE;

    if (on) {
        if ((zr->trouble & bit) != 0u) {
            return;
        }
        zr->trouble = (uint8_t)(zr->trouble | bit);
        zr->trouble_mem = (uint8_t)(zr->trouble_mem & (uint8_t)~bit);
        pr->trouble_unack = true;
        eng_emit(e, ALARM_EV_TROUBLE, zc->partition, z, 0u, NULL, (uint8_t)kind,
                 ALARM_TRBL_FAMILY_ZONE, 0u, mode);
    } else {
        if ((zr->trouble & bit) == 0u) {
            return;
        }
        zr->trouble = (uint8_t)(zr->trouble & (uint8_t)~bit);
        zr->trouble_mem = (uint8_t)(zr->trouble_mem | bit);
        eng_emit(e, ALARM_EV_TROUBLE_RESTORE, zc->partition, z, 0u, NULL, (uint8_t)kind,
                 ALARM_TRBL_FAMILY_ZONE, 0u, mode);
    }
    pr->dirty = true;
}

/* Cross zoning pairs two zones whichever of them names the other:
 * the zone editor sets the partner of the zone being edited only, and a zone named by another
 * must not alarm alone because nobody set the pair from its side too. */
static bool cross_paired(const alarm_engine_t *e, uint16_t a, uint16_t b)
{
    return a != b && eng_zone_valid(e, b) &&
           (eng_zcfg(e, a)->cross_zone == b || eng_zcfg(e, b)->cross_zone == a);
}

/* A partner that could trip now: a burglary zone armed in its partition's level, neither
 * bypassed nor shut down by swinger suppression. */
static bool cross_live(const alarm_engine_t *e, uint16_t p)
{
    const alarm_zone_config_t *pc = eng_zcfg(e, p);
    const alarm_partition_rt_t *pp = &e->parts[pc->partition - 1u];
    return alarm_zone_type_is_burglary(pc->type) &&
           (e->zones[p - 1u].flags & (ZRT_BYPASSED | ZRT_SWINGER)) == 0u &&
           pp->state != ALARM_STATE_DISARMED && alarm_zone_armed_in_level(pc, pp->level);
}

/* A zone with no partner that could trip acts as a normal zone:
 * it must never wait for a partner that cannot come. */
static bool cross_zoned(const alarm_engine_t *e, uint16_t z)
{
    for (uint16_t p = 1u; p <= ALARM_MAX_ZONES; p++) {
        if (cross_paired(e, z, p) && cross_live(e, p)) {
            return true;
        }
    }
    return false;
}

/* Instant burglary trip, honouring cross zoning. */
static void instant(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    alarm_zone_rt_t *zr = eng_zrt(e, z);

    if (cross_zoned(e, z)) {
        const alarm_ms_t window = MS(e->cfg.opt.cross_zone_window_s);
        for (uint16_t partner = 1u; partner <= ALARM_MAX_ZONES; partner++) {
            if (!cross_paired(e, z, partner)) {
                continue;
            }
            alarm_zone_rt_t *pz = eng_zrt(e, partner);
            const alarm_zone_config_t *pc = eng_zcfg(e, partner);
            if ((pz->flags & ZRT_CROSS_PENDING) != 0u && e->now - pz->cross_at <= window) {
                pz->flags = (uint16_t)(pz->flags & (uint16_t)~ZRT_CROSS_PENDING);
                zr->flags = (uint16_t)(zr->flags & (uint16_t)~ZRT_CROSS_PENDING);
                eng_alarm(e, pc->partition, partner, burglary_cause(pc->type), NULL, 0u);
                eng_alarm(e, zc->partition, z, burglary_cause(zc->type), NULL, 0u);
                return;
            }
        }
        zr->flags = (uint16_t)(zr->flags | ZRT_CROSS_PENDING);
        zr->cross_at = e->now;
        return;
    }
    eng_alarm(e, zc->partition, z, burglary_cause(zc->type), NULL, 0u);
}

static void burglary_fault(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    alarm_zone_rt_t *zr = eng_zrt(e, z);
    alarm_partition_rt_t *pr = eng_part(e, zc->partition);

    if ((zr->flags & (ZRT_BYPASSED | ZRT_SWINGER)) != 0u || pr->state == ALARM_STATE_DISARMED ||
        !alarm_zone_armed_in_level(zc, pr->level)) {
        return;
    }
    const bool ee = zc->type == ALARM_ZT_ENTRY_EXIT_1 || zc->type == ALARM_ZT_ENTRY_EXIT_2;
    const bool delayed_interior = zc->type == ALARM_ZT_INTERIOR_DELAY;
    const bool follower = zc->type == ALARM_ZT_INTERIOR_FOLLOWER;
    const bool ned = (pr->flags & ALARM_ARM_NO_ENTRY_DELAY) != 0u;

    switch (pr->state) {
    case ALARM_STATE_EXIT_DELAY:
        if (ee) {
            /* violation -> restore -> violation: the user came back in */
            if (pr->exit_door == 2u && pr->exit_restarts < e->cfg.opt.exit_restart_limit) {
                pr->exit_restarts++;
                eng_exit_restart(e, zc->partition, true, 0u, NULL);
            }
            pr->exit_door = 1u;
            pr->dirty = true;
            return;
        }
        if (zc->type == ALARM_ZT_PERIMETER || zc->type == ALARM_ZT_SILENT_BURGLARY) {
            instant(e, z); /* perimeter is armed as soon as arming starts */
        }
        return; /* interior zones may move during exit time */

    case ALARM_STATE_ARMED:
        if (ee || delayed_interior) {
            if (ned) {
                eng_alarm(e, zc->partition, z, ALARM_CAUSE_BURGLARY, NULL, 0u);
            } else {
                eng_start_entry(e, zc->partition, z);
            }
            return;
        }
        instant(e, z); /* perimeter, follower tripped first, day/night, silent */
        return;

    case ALARM_STATE_ENTRY_DELAY:
        if (ee || delayed_interior || follower) {
            bm_set(pr->followers, z); /* follows the running entry time */
            pr->dirty = true;
            return;
        }
        instant(e, z);
        return;

    default:
        return;
    }
}

static void day_night_fault(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    const alarm_zone_rt_t *zr = &e->zones[z - 1u];
    const alarm_partition_rt_t *pr = &e->parts[zc->partition - 1u];

    if ((zr->flags & (ZRT_BYPASSED | ZRT_SWINGER)) != 0u) {
        return;
    }
    if (pr->state == ALARM_STATE_DISARMED) {
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_DAY_FAULT, true); /* trouble by day */
        return;
    }
    if (!alarm_zone_armed_in_level(zc, pr->level) || pr->state == ALARM_STATE_EXIT_DELAY) {
        return;
    }
    instant(e, z); /* alarm by night */
}

void eng_smoke_reset_start(alarm_engine_t *e)
{
    if (e->smoke_reset_until == 0u) {
        eng_output(e, ALARM_OUT_SMOKE_RESET, 0u, ALARM_PAT_ON);
    }
    e->smoke_reset_until = ms_add(e->now, MS(e->cfg.opt.smoke_reset_s));
}

static void fire_trip(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    alarm_zone_rt_t *zr = eng_zrt(e, z);
    if ((zr->flags & ZRT_IN_ALARM) != 0u || e->smoke_reset_until != 0u) {
        return; /* already alarmed, or smoke power is being reset */
    }
    if (zc->type == ALARM_ZT_FIRE) {
        eng_alarm(e, zc->partition, z, ALARM_CAUSE_FIRE, NULL, 0u);
        return;
    }
    /* Fire with verification: reset smoke power, then require a second trip. */
    if (zr->verify == FV_WAIT) {
        zr->verify = FV_IDLE;
        eng_alarm(e, zc->partition, z, ALARM_CAUSE_FIRE, NULL, 0u);
        return;
    }
    if (zr->verify == FV_IDLE) {
        zr->verify = FV_RESET;
        zr->verify_until = ms_add(e->now, MS(e->cfg.opt.fire_verify_window_s));
        eng_smoke_reset_start(e);
        eng_part(e, zc->partition)->dirty = true;
    }
}

void eng_smoke_reset_end(alarm_engine_t *e)
{
    e->smoke_reset_until = 0u;
    eng_output(e, ALARM_OUT_SMOKE_RESET, 0u, ALARM_PAT_OFF);
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = eng_zcfg(e, z);
        if (!alarm_zone_type_is_fire(zc->type)) {
            continue;
        }
        alarm_zone_rt_t *zr = eng_zrt(e, z);
        const bool faulted = (zr->cond & ZC_FAULT) != 0u;
        if (zr->verify == FV_RESET) {
            if (faulted) { /* still in alarm after the reset: verified */
                zr->verify = FV_IDLE;
                eng_alarm(e, zc->partition, z, ALARM_CAUSE_FIRE, NULL, 0u);
            } else {
                zr->verify = FV_WAIT;
            }
            eng_part(e, zc->partition)->dirty = true;
        } else if (faulted && (zr->flags & ZRT_IN_ALARM) == 0u) {
            eng_alarm(e, zc->partition, z, ALARM_CAUSE_FIRE, NULL, 0u);
        }
    }
}

void eng_fire_verify_due(alarm_engine_t *e)
{
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        alarm_zone_rt_t *zr = eng_zrt(e, z);
        if (zr->verify == FV_WAIT && e->now >= zr->verify_until) {
            zr->verify = FV_IDLE; /* no confirming trip: not a fire */
            eng_part(e, eng_zcfg(e, z)->partition)->dirty = true;
        }
    }
}

alarm_ms_t eng_fire_verify_next(const alarm_engine_t *e)
{
    alarm_ms_t next = ALARM_TIME_NEVER;
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        const alarm_zone_rt_t *zr = &e->zones[i];
        if (zr->verify == FV_WAIT && zr->verify_until < next) {
            next = zr->verify_until;
        }
    }
    return next;
}

static void tamper_alarm(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    const alarm_zone_rt_t *zr = &e->zones[z - 1u];
    const alarm_partition_rt_t *pr = &e->parts[zc->partition - 1u];
    if (alarm_zone_type_is_burglary(zc->type) && pr->state != ALARM_STATE_DISARMED &&
        !pr->walk_test && (zr->flags & (ZRT_BYPASSED | ZRT_SWINGER)) == 0u &&
        alarm_zone_armed_in_level(zc, pr->level)) {
        eng_alarm(e, zc->partition, z, ALARM_CAUSE_TAMPER, NULL, 0u);
    }
}

void eng_zone_fault(alarm_engine_t *e, uint16_t z, bool quiet)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    const uint8_t p = zc->partition;
    alarm_partition_rt_t *pr = eng_part(e, p);
    const bool bypassed = (e->zones[z - 1u].flags & ZRT_BYPASSED) != 0u;

    /* Walk test: the zones it covers beep and are marked instead of acting (WALK-007); fire and
     * CO stay live, and 24-hour zones unless the start included them. A button arms nothing; a
     * Disarm button still ends the test, as code + OFF does (WALK-025), after its mark. */
    if (pr->walk_test && eng_walk_covers(e, z, pr->walk_flags)) {
        const bool button = zc->type == ALARM_ZT_ARM_STAY || zc->type == ALARM_ZT_ARM_AWAY ||
                            zc->type == ALARM_ZT_DISARM || zc->type == ALARM_ZT_KEYSWITCH;
        eng_walk_mark(e, z, button ? ALARM_WALK_MARK_BUTTON : ALARM_WALK_MARK_FAULT);
        if (zc->type == ALARM_ZT_DISARM) {
            eng_button_zone(e, z);
        }
        return;
    }

    switch (zc->type) {
    case ALARM_ZT_FIRE:
    case ALARM_ZT_FIRE_VERIFIED:
        fire_trip(e, z);
        break;
    case ALARM_ZT_CO:
        if ((e->zones[z - 1u].flags & ZRT_IN_ALARM) == 0u) {
            eng_alarm(e, p, z, ALARM_CAUSE_CO, NULL, 0u);
        }
        break;
    case ALARM_ZT_24H_SILENT:
        if (!bypassed) {
            eng_alarm(e, p, z, ALARM_CAUSE_24H_SILENT, NULL, 0u);
        }
        break;
    case ALARM_ZT_24H_AUDIBLE:
        if (!bypassed) {
            eng_alarm(e, p, z, ALARM_CAUSE_24H_AUDIBLE, NULL, 0u);
        }
        break;
    case ALARM_ZT_24H_AUX:
        if (!bypassed) {
            eng_alarm(e, p, z, ALARM_CAUSE_24H_AUX, NULL, 0u);
        }
        break;
    case ALARM_ZT_MONITOR:
        eng_emit(e, ALARM_EV_MONITOR_FAULT, p, z, 0u, NULL, 0u, 0u, 0u,
                 eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE);
        break;
    case ALARM_ZT_ARM_STAY:
    case ALARM_ZT_ARM_AWAY:
    case ALARM_ZT_DISARM:
    case ALARM_ZT_KEYSWITCH:
        eng_button_zone(e, z);
        break;
    case ALARM_ZT_MOMENTARY_EXIT:
        if (pr->state == ALARM_STATE_ARMED &&
            (pr->level == ALARM_LEVEL_STAY || pr->level == ALARM_LEVEL_NIGHT)) {
            const alarm_source_t src = {ALARM_SRC_ZONE, z};
            /* The button needs no code (CORE-EXIT-013), so it always works; it is only named
             * after its user while that user may still operate the partition. */
            uint8_t actor = 0u;
            if (eng_user_in_partition(e, zc->user, p)) {
                actor = zc->user;
            }
            eng_exit_restart(e, p, false, actor, &src);
        }
        break;
    case ALARM_ZT_DAY_NIGHT:
        day_night_fault(e, z);
        break;
    case ALARM_ZT_NO_RESPONSE:
    case ALARM_ZT_NOT_USED:
        break;
    default:
        burglary_fault(e, z);
        break;
    }

    /* A bypassed zone is out of service until the next disarm: it doesn't chime either
     * (CORE-CHIME-006). */
    if (!quiet && !bypassed && pr->state == ALARM_STATE_DISARMED && pr->chime &&
        !pr->walk_test && (zc->flags & ALARM_ZF_CHIME) != 0u) {
        eng_sound(e, p, ALARM_SOUND_CHIME, z);
    }
}

static void zone_restore(alarm_engine_t *e, uint16_t z)
{
    const alarm_zone_config_t *zc = eng_zcfg(e, z);
    alarm_zone_rt_t *zr = eng_zrt(e, z);
    alarm_partition_rt_t *pr = eng_part(e, zc->partition);

    if ((zr->flags & ZRT_IN_ALARM) != 0u) {
        const bool reported = (zr->flags & ZRT_REPORTED) != 0u && eng_zone_reportable(e, z);
        zr->flags = (uint16_t)(zr->flags & (uint16_t)~(ZRT_IN_ALARM | ZRT_REPORTED));
        eng_emit(e, ALARM_EV_ALARM_RESTORE, zc->partition, z, 0u, NULL, 0u, 0u, 0u,
                 reported ? REPORT_NOW : REPORT_NONE);
    }

    switch (zc->type) {
    case ALARM_ZT_MONITOR:
        /* The fault was skipped during a walk test (WALK-007), so its restore is too. */
        if (!(pr->walk_test && eng_walk_covers(e, z, pr->walk_flags))) {
            eng_emit(e, ALARM_EV_MONITOR_RESTORE, zc->partition, z, 0u, NULL, 0u, 0u, 0u,
                     eng_zone_reportable(e, z) ? REPORT_NOW : REPORT_NONE);
        }
        break;
    case ALARM_ZT_DAY_NIGHT:
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_DAY_FAULT, false);
        break;
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
        if (pr->state == ALARM_STATE_EXIT_DELAY && pr->exit_door == 1u &&
            (zr->flags & ZRT_BYPASSED) == 0u) {
            pr->exit_door = 2u; /* opened and closed: somebody left */
        }
        break;
    default:
        break;
    }
    pr->dirty = true;
}

void eng_zone_input(alarm_engine_t *e, uint16_t z, alarm_zone_event_t ev)
{
    alarm_zone_rt_t *zr = eng_zrt(e, z);
    alarm_partition_rt_t *pr = eng_part(e, eng_zcfg(e, z)->partition);

    if (ev == ALARM_ZEV_UNREACHABLE) {
        /* The source knows the sensor is gone (docs/features/22 ZSRC-007): CHECK without waiting
         * for supervision_s, until the source says REACHABLE. Other inputs of the zone (a
         * separate battery sensor) still count but don't clear it. */
        zr->flags = (uint16_t)(zr->flags | ZRT_UNREACHABLE);
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_SUPERVISION, true);
        return;
    }
    if (ev == ALARM_ZEV_REACHABLE) {
        zr->flags = (uint16_t)(zr->flags & (uint16_t)~ZRT_UNREACHABLE);
    }
    zr->checkin = e->now;
    if ((zr->trouble & TRBL_BIT(ALARM_ZTRBL_SUPERVISION)) != 0u &&
        (zr->flags & ZRT_UNREACHABLE) == 0u) {
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_SUPERVISION, false);
    }

    switch (ev) {
    case ALARM_ZEV_FAULT:
        if ((zr->cond & ZC_FAULT) != 0u) {
            return;
        }
        zr->cond = (uint8_t)(zr->cond | ZC_FAULT);
        pr->dirty = true;
        if (!settling(e)) {
            eng_zone_fault(e, z, false);
        }
        return;
    case ALARM_ZEV_RESTORE:
        if ((zr->cond & ZC_FAULT) == 0u) {
            return;
        }
        zr->cond = (uint8_t)(zr->cond & (uint8_t)~ZC_FAULT);
        zone_restore(e, z);
        return;
    case ALARM_ZEV_TROUBLE:
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_LOOP, true);
        return;
    case ALARM_ZEV_TROUBLE_RESTORE:
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_LOOP, false);
        return;
    case ALARM_ZEV_TAMPER:
        if ((zr->trouble & TRBL_BIT(ALARM_ZTRBL_TAMPER)) != 0u) {
            return;
        }
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_TAMPER, true);
        if (pr->walk_test && eng_walk_covers(e, z, pr->walk_flags)) {
            /* WALK-008: the cover is tested; a disarmed partition's tamper never alarms. */
            eng_walk_mark(e, z, ALARM_WALK_MARK_COVER);
        }
        if (!settling(e)) {
            tamper_alarm(e, z);
        }
        return;
    case ALARM_ZEV_TAMPER_RESTORE:
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_TAMPER, false);
        return;
    case ALARM_ZEV_LOW_BATTERY:
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_LOW_BATTERY, true);
        return;
    case ALARM_ZEV_LOW_BATTERY_RESTORE:
        eng_zone_set_trouble(e, z, ALARM_ZTRBL_LOW_BATTERY, false);
        return;
    case ALARM_ZEV_TEST:
        /* The sensor's own test (WALK-007): marks it during a walk test, fire and CO included,
         * without an alarm; otherwise a check-in (counted above). */
        if (pr->walk_test) {
            eng_walk_mark(e, z, ALARM_WALK_MARK_SENSOR_TEST);
        }
        return;
    case ALARM_ZEV_CHECKIN:
    case ALARM_ZEV_REACHABLE:
    default:
        return;
    }
}

void eng_settle_end(alarm_engine_t *e)
{
    e->settle_until = 0u;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (!eng_zone_valid(e, z)) {
            continue;
        }
        const alarm_zone_rt_t *zr = &e->zones[z - 1u];
        if ((zr->cond & ZC_FAULT) != 0u) {
            eng_zone_fault(e, z, true);
        }
        if ((zr->trouble & TRBL_BIT(ALARM_ZTRBL_TAMPER)) != 0u) {
            tamper_alarm(e, z);
        }
    }
}

void eng_supervision_due(alarm_engine_t *e)
{
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = eng_zcfg(e, z);
        const alarm_zone_rt_t *zr = &e->zones[z - 1u];
        if (zc->type == ALARM_ZT_NOT_USED || zc->supervision_s == 0u ||
            (zr->trouble & TRBL_BIT(ALARM_ZTRBL_SUPERVISION)) != 0u) {
            continue;
        }
        if (e->now >= ms_add(zr->checkin, MS(zc->supervision_s))) {
            eng_zone_set_trouble(e, z, ALARM_ZTRBL_SUPERVISION, true);
        }
    }
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        const alarm_device_config_t *dc = &e->cfg.devices[d];
        const uint32_t bit = 1u << d;
        if (!dc->enabled || dc->supervision_s == 0u || (e->dev_trouble & bit) != 0u) {
            continue;
        }
        if (e->now >= ms_add(e->devices[d].checkin, MS(dc->supervision_s))) {
            e->dev_trouble |= bit;
            e->dev_trouble_mem &= ~bit;
            for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
                if (dc->partition == 0u || dc->partition == p) {
                    eng_part(e, p)->trouble_unack = true;
                }
            }
            eng_emit(e, ALARM_EV_TROUBLE, dc->partition, (uint16_t)(d + 1u), 0u, NULL,
                     ALARM_TRBL_DEVICE_SUPERVISION, ALARM_TRBL_FAMILY_DEVICE, ALARM_EVF_SYSTEM,
                     REPORT_NOW);
            e->sys_dirty = true;
        }
    }
}

alarm_ms_t eng_supervision_next(const alarm_engine_t *e)
{
    alarm_ms_t next = ALARM_TIME_NEVER;
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        const alarm_zone_config_t *zc = &e->cfg.zones[i];
        const alarm_zone_rt_t *zr = &e->zones[i];
        if (zc->type == ALARM_ZT_NOT_USED || zc->supervision_s == 0u ||
            (zr->trouble & TRBL_BIT(ALARM_ZTRBL_SUPERVISION)) != 0u) {
            continue;
        }
        const alarm_ms_t t = ms_add(zr->checkin, MS(zc->supervision_s));
        if (t < next) {
            next = t;
        }
    }
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        const alarm_device_config_t *dc = &e->cfg.devices[d];
        if (!dc->enabled || dc->supervision_s == 0u || (e->dev_trouble & (1u << d)) != 0u) {
            continue;
        }
        const alarm_ms_t t = ms_add(e->devices[d].checkin, MS(dc->supervision_s));
        if (t < next) {
            next = t;
        }
    }
    return next;
}
