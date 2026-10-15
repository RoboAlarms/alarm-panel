/*
 * engine.c - lifecycle, timer scheduling, change notification and queries.
 */
#include <string.h>

#include "engine_internal.h"

#include <stddef.h>

/* The stored runtime state ("rt", a fixed-size blob, ALARM_RUNTIME_VERSION 1): arm_src took
 * the partition entry's tail padding, so the layout an older firmware wrote is unchanged. */
_Static_assert(sizeof(alarm_partition_runtime_t) == 8u, "runtime partition entry size changed");
_Static_assert(offsetof(alarm_partition_runtime_t, arm_src) == 7u, "arm_src must stay in the pad");
_Static_assert(sizeof(alarm_runtime_state_t) == 72u, "stored runtime state size changed");

void eng_sound(alarm_engine_t *e, uint8_t p, alarm_sound_t sound, uint16_t zone)
{
    if (e->ports.on_sound != NULL) {
        e->ports.on_sound(e->ports.ctx, p, sound, zone);
    }
}

void eng_output(alarm_engine_t *e, alarm_output_t out, uint8_t p, alarm_pattern_t pat)
{
    if (e->ports.on_output != NULL) {
        e->ports.on_output(e->ports.ctx, out, p, pat);
    }
}

void eng_mark_all_dirty(alarm_engine_t *e)
{
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        e->parts[i].dirty = true;
    }
    e->sys_dirty = true;
}

/* ------------------------------------------------------------------------ */
/* Timers                                                                    */
/* ------------------------------------------------------------------------ */
static alarm_ms_t next_internal(const alarm_engine_t *e)
{
    alarm_ms_t t = ALARM_TIME_NEVER;
    if (e->settle_until != 0u && e->settle_until < t) {
        t = e->settle_until;
    }
    if (e->smoke_reset_until != 0u && e->smoke_reset_until < t) {
        t = e->smoke_reset_until;
    }
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (e->cfg.partitions[p - 1u].enabled) {
            const alarm_ms_t pt = eng_partition_next(e, p);
            if (pt < t) {
                t = pt;
            }
        }
    }
    alarm_ms_t x = eng_reports_next(e);
    if (x < t) {
        t = x;
    }
    x = eng_fire_verify_next(e);
    if (x < t) {
        t = x;
    }
    x = eng_supervision_next(e);
    if (x < t) {
        t = x;
    }
    x = eng_program_next(e);
    if (x < t) {
        t = x;
    }
    x = eng_rewatch_next(e);
    if (x < t) {
        t = x;
    }
    return t;
}

static void process_due(alarm_engine_t *e)
{
    if (e->settle_until != 0u && e->now >= e->settle_until) {
        eng_settle_end(e);
    }
    if (e->smoke_reset_until != 0u && e->now >= e->smoke_reset_until) {
        eng_smoke_reset_end(e);
    }
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (e->cfg.partitions[p - 1u].enabled) {
            eng_partition_timers(e, p);
        }
    }
    eng_reports_due(e);
    eng_fire_verify_due(e);
    eng_supervision_due(e);
    eng_program_due(e);
    eng_rewatch_due(e);
}

/* Run every timer up to 'now' in deadline order. */
void eng_advance(alarm_engine_t *e, alarm_ms_t now)
{
    if (now < e->now) {
        now = e->now; /* time never goes backwards */
    }
    for (unsigned guard = 0; guard < 10000u; guard++) {
        const alarm_ms_t d = next_internal(e);
        if (d > now) {
            break;
        }
        if (d > e->now) {
            e->now = d;
        }
        process_due(e);
    }
    e->now = now;
}

/* ------------------------------------------------------------------------ */
/* Change notification                                                       */
/* ------------------------------------------------------------------------ */
void eng_finish(alarm_engine_t *e)
{
    eng_rewatch_sync(e); /* zones changed in this call start or stop their wait now */
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!e->cfg.partitions[p - 1u].enabled) {
            continue;
        }
        alarm_partition_rt_t *pr = eng_part(e, p);
        const alarm_pattern_t bell = eng_bell_pattern(pr);
        if (bell != pr->last_bell) {
            pr->last_bell = bell;
            eng_output(e, ALARM_OUT_BELL, p, bell);
            pr->dirty = true;
        }
        if (pr->strobe != pr->last_strobe) {
            pr->last_strobe = pr->strobe;
            eng_output(e, ALARM_OUT_STROBE, p, pr->strobe ? ALARM_PAT_ON : ALARM_PAT_OFF);
            pr->dirty = true;
        }
        const alarm_sound_t s = eng_sounder(e, p);
        if (s != pr->last_sounder) {
            pr->last_sounder = s;
            pr->dirty = true;
        }
    }
    if (e->sys_dirty) {
        for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
            e->parts[i].dirty = true;
        }
    }
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        alarm_partition_rt_t *pr = eng_part(e, p);
        if (pr->dirty) {
            pr->dirty = false;
            if (e->cfg.partitions[p - 1u].enabled && e->ports.on_status != NULL) {
                e->ports.on_status(e->ports.ctx, p);
            }
        }
    }
    if (e->sys_dirty) {
        e->sys_dirty = false;
        if (e->ports.on_status != NULL) {
            e->ports.on_status(e->ports.ctx, 0u);
        }
    }
    if (e->persist_dirty) {
        e->persist_dirty = false;
        if (e->ports.on_persist != NULL) {
            alarm_runtime_state_t st;
            alarm_engine_runtime_state(e, &st);
            e->ports.on_persist(e->ports.ctx, &st);
        }
    }
    if (e->config_dirty) {
        e->config_dirty = false;
        e->cfg_generation++; /* a user change, a voice approval or withdrawal */
        if (e->ports.on_config_changed != NULL) {
            e->ports.on_config_changed(e->ports.ctx);
        }
    }
}

bool eng_enter(alarm_engine_t *e, alarm_ms_t now)
{
    if (e == NULL || !e->initialized || e->in_call) {
        return false;
    }
    e->in_call = true;
    eng_advance(e, now);
    return true;
}

void eng_leave(alarm_engine_t *e)
{
    eng_finish(e);
    e->in_call = false;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */
alarm_result_t alarm_engine_init(alarm_engine_t *e, const alarm_config_t *cfg,
                                 const alarm_ports_t *ports,
                                 const alarm_runtime_state_t *restore, alarm_ms_t now)
{
    return alarm_engine_init_ex(e, cfg, ports, restore, now, ALARM_START_UNKNOWN);
}

alarm_result_t alarm_engine_init_ex(alarm_engine_t *e, const alarm_config_t *cfg,
                                    const alarm_ports_t *ports,
                                    const alarm_runtime_state_t *restore, alarm_ms_t now,
                                    alarm_start_reason_t reason)
{
    if (e == NULL || cfg == NULL || ports == NULL || ports->hash_code == NULL) {
        return ALARM_ERR_BAD_ARG;
    }
    if (alarm_config_validate(cfg, NULL, 0u) != ALARM_OK) {
        return ALARM_ERR_BAD_ARG;
    }
    memset(e, 0, sizeof(*e));
    e->cfg = *cfg;
    e->ports = *ports;
    e->now = now;

    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        e->zones[i].checkin = now;
    }
    for (unsigned i = 0; i < ALARM_MAX_DEVICES; i++) {
        e->devices[i].checkin = now;
    }

    if (restore != NULL && restore->version == ALARM_RUNTIME_VERSION) {
        e->seq = restore->event_seq;
        for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
            if (!e->cfg.partitions[p - 1u].enabled) {
                continue;
            }
            const alarm_partition_runtime_t *rp = &restore->part[p - 1u];
            alarm_partition_rt_t *pr = eng_part(e, p);
            pr->chime = rp->chime;
            pr->memory = rp->memory;
            const bool level_ok = rp->level == ALARM_LEVEL_AWAY || rp->level == ALARM_LEVEL_STAY ||
                                  rp->level == ALARM_LEVEL_NIGHT;
            if (cfg->opt.power_up_previous_state && rp->state != ALARM_STATE_DISARMED &&
                level_ok) {
                pr->state = ALARM_STATE_ARMED;
                pr->level = (alarm_arm_level_t)rp->level;
                pr->flags = (uint8_t)(rp->flags &
                                      (ALARM_ARM_NO_ENTRY_DELAY | ALARM_ARM_SILENT_EXIT));
                pr->armed_by = rp->armed_by;
                /* CORE-USR-035: the kind only (0 = not known, a state from before the field) */
                if (rp->arm_src != 0u && rp->arm_src <= (uint8_t)ALARM_SRC_AUTOMATION + 1u) {
                    pr->arm_source.kind = (uint8_t)(rp->arm_src - 1u);
                    pr->arm_source.id = 0u;
                }
            }
        }
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (!eng_zone_valid(e, z)) {
                continue;
            }
            alarm_zone_rt_t *zr = eng_zrt(e, z);
            if (bm_get(restore->bypassed, z) && alarm_zone_bypassable(eng_zcfg(e, z))) {
                zr->flags = (uint16_t)(zr->flags | ZRT_BYPASSED);
            }
            if (bm_get(restore->in_memory, z)) {
                zr->flags = (uint16_t)(zr->flags | ZRT_MEMORY);
            }
        }
    }

    if (cfg->opt.power_up_settle_s != 0u) {
        e->settle_until = ms_add(now, MS(cfg->opt.power_up_settle_s));
    }
    if (cfg->opt.power_up_program) {
        e->recovery_until = ms_add(now, MS(ALARM_RECOVERY_WINDOW_S));
    }
    eng_update_default_codes(e);
    e->initialized = true;
    e->in_call = true;
    /* PWR-007: arg = why the panel started (alarm_start_reason_t). */
    eng_emit(e, ALARM_EV_SYSTEM_START, 0u, 0u, 0u, NULL, (uint8_t)reason, 0u, ALARM_EVF_SYSTEM, REPORT_NOW);
    eng_mark_all_dirty(e);
    eng_leave(e);
    return ALARM_OK;
}

void alarm_engine_tick(alarm_engine_t *e, alarm_ms_t now)
{
    if (eng_enter(e, now)) {
        eng_leave(e);
    }
}

alarm_ms_t alarm_engine_next_deadline(const alarm_engine_t *e)
{
    if (e == NULL || !e->initialized) {
        return ALARM_TIME_NEVER;
    }
    return next_internal(e);
}

/* ------------------------------------------------------------------------ */
/* Inputs                                                                    */
/* ------------------------------------------------------------------------ */
void alarm_engine_zone_event(alarm_engine_t *e, uint16_t zone, alarm_zone_event_t ev,
                             alarm_ms_t now)
{
    if (e == NULL || !eng_zone_valid(e, zone)) {
        return;
    }
    if (eng_enter(e, now)) {
        eng_zone_input(e, zone, ev);
        eng_leave(e);
    }
}

void alarm_engine_hold_arming(alarm_engine_t *e, bool hold)
{
    e->arm_hold = hold; /* CORE-ARM-013 */
}

bool alarm_engine_arming_held(const alarm_engine_t *e)
{
    return e->arm_hold;
}

/* Raise or restore one system trouble, inside an engine call. Only a change emits. */
static void sys_trouble_set(alarm_engine_t *e, alarm_sys_trouble_t trouble, bool active)
{
    const uint32_t bit = 1u << (unsigned)trouble;
    if (active && (e->sys_trouble & bit) == 0u) {
        e->sys_trouble |= bit;
        e->sys_trouble_mem &= ~bit;
        if ((e->cfg.opt.trouble_beep_mask & bit) != 0u) {
            for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
                e->parts[i].trouble_unack = true;
            }
        }
        eng_emit(e, ALARM_EV_TROUBLE, 0u, 0u, 0u, NULL, (uint8_t)trouble,
                 ALARM_TRBL_FAMILY_SYSTEM, ALARM_EVF_SYSTEM, REPORT_NOW);
        e->sys_dirty = true;
    } else if (!active && (e->sys_trouble & bit) != 0u) {
        e->sys_trouble &= ~bit;
        e->sys_trouble_mem |= bit;
        eng_emit(e, ALARM_EV_TROUBLE_RESTORE, 0u, 0u, 0u, NULL, (uint8_t)trouble,
                 ALARM_TRBL_FAMILY_SYSTEM, ALARM_EVF_SYSTEM, REPORT_NOW);
        e->sys_dirty = true;
    }
}

void alarm_engine_system_trouble(alarm_engine_t *e, alarm_sys_trouble_t trouble, bool active,
                                 alarm_ms_t now)
{
    if (trouble == ALARM_TRBL_STORAGE) { /* one hold among the owners' (ZW-039) */
        alarm_engine_storage_trouble(e, ALARM_STORAGE_OTHER, active, now);
        return;
    }
    if (e == NULL || (unsigned)trouble >= (unsigned)ALARM_TRBL_COUNT || !eng_enter(e, now)) {
        return;
    }
    sys_trouble_set(e, trouble, active);
    eng_leave(e);
}

/* CORE-TRBL-001, P4-013, ZW-039: the storage trouble stands while any owner holds it, so one
 * owner's recovery never clears another's failure. */
void alarm_engine_storage_trouble(alarm_engine_t *e, alarm_storage_owner_t owner, bool active,
                                  alarm_ms_t now)
{
    if (e == NULL || (unsigned)owner >= (unsigned)ALARM_STORAGE_OWNER_COUNT ||
        !eng_enter(e, now)) {
        return;
    }
    const uint32_t bit = 1u << (unsigned)owner;
    if (active) {
        e->storage_owners |= bit;
    } else {
        e->storage_owners &= ~bit;
    }
    sys_trouble_set(e, ALARM_TRBL_STORAGE, e->storage_owners != 0u);
    eng_leave(e);
}

uint32_t alarm_engine_storage_owners(const alarm_engine_t *e)
{
    return e == NULL ? 0u : e->storage_owners;
}

void alarm_engine_device_checkin(alarm_engine_t *e, uint8_t device, alarm_ms_t now)
{
    if (e == NULL || device < 1u || device > ALARM_MAX_DEVICES || !eng_enter(e, now)) {
        return;
    }
    const unsigned d = device - 1u;
    e->devices[d].checkin = e->now;
    const uint32_t bit = 1u << d;
    if ((e->dev_trouble & bit) != 0u) {
        e->dev_trouble &= ~bit;
        e->dev_trouble_mem |= bit;
        eng_emit(e, ALARM_EV_TROUBLE_RESTORE, e->cfg.devices[d].partition, device, 0u, NULL,
                 ALARM_TRBL_DEVICE_SUPERVISION, ALARM_TRBL_FAMILY_DEVICE, ALARM_EVF_SYSTEM,
                 REPORT_NOW);
        e->sys_dirty = true;
    }
    eng_leave(e);
}

/* CORE-TRBL-008, ZW-038: the platform's own word that a device can't be reached (a Z-Wave
 * keypad its controller marked dead) or is heard again. The trouble and its restore are exactly
 * the supervision ones; a configured supervision_s isn't needed. Idempotent. */
void alarm_engine_device_reachable(alarm_engine_t *e, uint8_t device, bool reachable,
                                   alarm_ms_t now)
{
    if (e == NULL || device < 1u || device > ALARM_MAX_DEVICES || !eng_enter(e, now)) {
        return;
    }
    const unsigned d = device - 1u;
    const uint32_t bit = 1u << d;
    const alarm_device_config_t *dc = &e->cfg.devices[d];
    if (reachable) {
        e->devices[d].checkin = e->now;
        if ((e->dev_trouble & bit) != 0u) {
            e->dev_trouble &= ~bit;
            e->dev_trouble_mem |= bit;
            eng_emit(e, ALARM_EV_TROUBLE_RESTORE, dc->partition, device, 0u, NULL,
                     ALARM_TRBL_DEVICE_SUPERVISION, ALARM_TRBL_FAMILY_DEVICE, ALARM_EVF_SYSTEM,
                     REPORT_NOW);
            e->sys_dirty = true;
        }
    } else if ((e->dev_trouble & bit) == 0u) {
        e->dev_trouble |= bit;
        e->dev_trouble_mem &= ~bit;
        for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
            if (dc->partition == 0u || dc->partition == p) {
                eng_part(e, p)->trouble_unack = true;
            }
        }
        eng_emit(e, ALARM_EV_TROUBLE, dc->partition, device, 0u, NULL,
                 ALARM_TRBL_DEVICE_SUPERVISION, ALARM_TRBL_FAMILY_DEVICE, ALARM_EVF_SYSTEM,
                 REPORT_NOW);
        e->sys_dirty = true;
    }
    eng_leave(e);
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */
alarm_result_t alarm_engine_set_config(alarm_engine_t *e, const alarm_config_t *cfg,
                                       alarm_ms_t now)
{
    if (e == NULL || cfg == NULL) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!eng_enter(e, now)) {
        return ALARM_ERR_BAD_STATE;
    }
    alarm_result_t r = eng_all_idle(e) ? ALARM_OK : ALARM_ERR_BAD_STATE;
    if (r == ALARM_OK && alarm_config_validate(cfg, NULL, 0u) != ALARM_OK) {
        r = ALARM_ERR_BAD_ARG;
    }
    if (r == ALARM_OK) {
        for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
            if (alarm_zone_config_resets_state(&e->cfg.zones[i], &cfg->zones[i])) {
                alarm_zone_rt_t *zr = &e->zones[i];
                /* A retyped zone keeps its sensor's state; a new or removed one starts clean. */
                const bool same_sensor = e->cfg.zones[i].type != ALARM_ZT_NOT_USED &&
                                         cfg->zones[i].type != ALARM_ZT_NOT_USED;
                const uint8_t cond = same_sensor ? zr->cond : 0u;
                memset(zr, 0, sizeof(*zr));
                zr->cond = cond;
                zr->checkin = e->now;
                eng_walk_forget(e, (uint16_t)(i + 1u)); /* WALK-006 */
            }
        }
        /* CORE-TRBL-008, ZW-038: a device on a check-in timer (in the old or the new config)
         * starts its clock again with no trouble; the clock raises it again if the device stays
         * silent. Any other device trouble came from alarm_engine_device_reachable (a Z-Wave
         * keypad its controller marked dead): no clock would raise it again, and its restore
         * comes when the device is heard, so it stays. */
        uint32_t timed = 0u;
        for (unsigned i = 0; i < ALARM_MAX_DEVICES; i++) {
            e->devices[i].checkin = e->now;
            if ((e->cfg.devices[i].enabled && e->cfg.devices[i].supervision_s != 0u) ||
                (cfg->devices[i].enabled && cfg->devices[i].supervision_s != 0u)) {
                timed |= 1u << i;
            }
        }
        e->dev_trouble &= ~timed;
        /* CORE-CFG-021: the zones' voice revisions follow the stored configuration's identity
         * rules, whatever the writer sent (alarm_zone_voice_reconcile). Zone by zone in place,
         * as each old zone is needed only for itself; then the rest of the configuration. */
        uint32_t voice_counter = e->cfg.opt.voice_revision_counter;
        for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
            alarm_zone_config_t z = cfg->zones[i];
            alarm_zone_voice_reconcile(&e->cfg.zones[i], &z, &voice_counter);
            e->cfg.zones[i] = z;
        }
        const size_t zones_at = offsetof(alarm_config_t, zones);
        const size_t zones_end = zones_at + sizeof(e->cfg.zones);
        memcpy(&e->cfg, cfg, zones_at);
        memcpy((uint8_t *)&e->cfg + zones_end, (const uint8_t *)cfg + zones_end,
               sizeof(alarm_config_t) - zones_end);
        e->cfg.opt.voice_revision_counter = voice_counter;
        e->cfg_generation++;
        if (e->program) {
            e->program_until = ms_add(e->now, MS(ALARM_PROGRAM_TIMEOUT_S));
        }
        if (!e->cfg.opt.power_up_program) {
            e->recovery_until = 0u;
        }
        eng_update_default_codes(e);
        eng_emit(e, ALARM_EV_CONFIG_CHANGED, 0u, 0u, 0u, NULL, 0u, 0u, ALARM_EVF_SYSTEM,
                 REPORT_NOW);
        eng_mark_all_dirty(e);
        e->persist_dirty = true;
    }
    eng_leave(e);
    return r;
}

const alarm_config_t *alarm_engine_config(const alarm_engine_t *e)
{
    return (e != NULL) ? &e->cfg : NULL;
}

uint32_t alarm_engine_config_generation(const alarm_engine_t *e)
{
    return (e != NULL) ? e->cfg_generation : 0u;
}

/* CORE-CFG-021: approval and withdrawal are their own transactions on the stored
 * configuration; a configuration write can never carry one (alarm_zone_voice_reconcile). */
static alarm_voice_result_t eng_voice_change(alarm_engine_t *e, uint16_t zone_no,
                                             uint32_t expected_generation,
                                             uint32_t expected_binding, const uint16_t *words,
                                             uint32_t pack_revision, bool approve, alarm_ms_t now)
{
    if (e == NULL || (approve && words == NULL)) {
        return ALARM_VOICE_BAD_ARG;
    }
    if (!eng_enter(e, now)) {
        return ALARM_VOICE_BUSY;
    }
    alarm_voice_result_t r = ALARM_VOICE_OK;
    if (!eng_all_idle(e)) {
        r = ALARM_VOICE_BUSY;
    } else if (expected_generation != e->cfg_generation) {
        r = ALARM_VOICE_STALE;
    } else if (approve) {
        r = alarm_config_voice_approve(&e->cfg, zone_no, expected_binding, words, pack_revision);
    } else {
        r = alarm_config_voice_withdraw(&e->cfg, zone_no, expected_binding);
    }
    if (r == ALARM_VOICE_OK) {
        eng_emit(e, ALARM_EV_CONFIG_CHANGED, 0u, 0u, 0u, NULL, 0u, 0u, ALARM_EVF_SYSTEM,
                 REPORT_NOW);
        if (e->program) {
            e->program_until = ms_add(e->now, MS(ALARM_PROGRAM_TIMEOUT_S));
        }
        e->config_dirty = true; /* the platform stores it; the generation moves on */
    }
    eng_leave(e);
    return r;
}

alarm_voice_result_t alarm_engine_voice_approve(alarm_engine_t *e, uint16_t zone_no,
                                                uint32_t expected_generation,
                                                uint32_t expected_binding,
                                                const uint16_t words[ALARM_ZONE_WORDS],
                                                uint32_t pack_revision, alarm_ms_t now)
{
    return eng_voice_change(e, zone_no, expected_generation, expected_binding, words,
                            pack_revision, true, now);
}

alarm_voice_result_t alarm_engine_voice_withdraw(alarm_engine_t *e, uint16_t zone_no,
                                                 uint32_t expected_generation,
                                                 uint32_t expected_binding, alarm_ms_t now)
{
    return eng_voice_change(e, zone_no, expected_generation, expected_binding, NULL, 0u, false,
                            now);
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                   */
/* ------------------------------------------------------------------------ */
static unsigned popcount32(uint32_t v)
{
    unsigned n = 0;
    while (v != 0u) {
        v &= v - 1u;
        n++;
    }
    return n;
}

void alarm_engine_partition_status(const alarm_engine_t *e, uint8_t p,
                                   alarm_partition_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (e == NULL || !eng_partition_valid(e, p)) {
        return;
    }
    const alarm_partition_rt_t *pr = &e->parts[p - 1u];
    const bool armed = pr->state != ALARM_STATE_DISARMED;

    out->state = pr->state;
    out->level = pr->level;
    out->arm_flags = pr->flags;
    out->armed_by = pr->armed_by;
    if (armed) {
        out->arm_source = pr->arm_source;
    }
    out->chime = pr->chime;
    out->walk_test = pr->walk_test;
    if (pr->walk_test && pr->walk_test_until > e->now) {
        out->walk_test_remaining_ms = pr->walk_test_until - e->now;
    }
    out->walk_started_ms = pr->walk_started;
    out->walk_seq = pr->walk_seq;
    out->walk_tested = pr->walk_tested;
    out->walk_total = pr->walk_total;
    out->walk_flags = pr->walk_flags;
    out->walk_started_by = pr->walk_started_by;
    out->walk_started_src = pr->walk_started_src;
    out->walk_ended_by = pr->walk_ended_by;
    out->walk_end_how = pr->walk_end_how;
    out->trouble_unack = pr->trouble_unack;
    out->exit_error = pr->state == ALARM_STATE_ENTRY_DELAY && pr->entry_exit_error;
    out->sounder = eng_sounder(e, p);
    out->bell = eng_bell_pattern(pr);
    if ((pr->state == ALARM_STATE_EXIT_DELAY || pr->state == ALARM_STATE_ENTRY_DELAY) &&
        pr->deadline > e->now) {
        out->delay_remaining_ms = pr->deadline - e->now;
    }
    out->memory = armed ? (uint16_t)(pr->memory & (uint16_t)~SILENT_CAUSES) : pr->memory;
    out->triggered = (pr->sounding & (uint16_t)~SILENT_CAUSES) != 0u ||
                     (armed && (pr->period_alarms & (uint16_t)~SILENT_CAUSES) != 0u);

    const alarm_arm_level_t levels[3] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY, ALARM_LEVEL_NIGHT};
    bool ok[3] = {true, true, true};
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (!eng_zone_valid(e, z) || e->cfg.zones[z - 1u].partition != p) {
            continue;
        }
        const alarm_zone_rt_t *zr = &e->zones[z - 1u];
        for (unsigned l = 0; l < 3u; l++) {
            if (ok[l] && eng_zone_blocks_level(e, z, levels[l])) {
                ok[l] = false;
            }
        }
        if ((zr->cond & ZC_FAULT) != 0u) {
            out->faults++;
        }
        if ((zr->flags & ZRT_BYPASSED) != 0u) {
            out->bypassed++;
        }
        if ((zr->flags & ZRT_MEMORY) != 0u && !(armed && (zr->flags & ZRT_SILENT_MEM) != 0u)) {
            out->alarms++;
        }
        if (zr->trouble != 0u || zr->trouble_mem != 0u) {
            out->troubles++;
        }
    }
    for (unsigned l = 0; l < 3u; l++) {
        if (ok[l] && !e->arm_hold) { /* CORE-ARM-013 */
            out->ready_levels = (uint8_t)(out->ready_levels | ALARM_LEVEL_BIT(levels[l]));
        }
    }
    out->ready = ok[0] && !e->arm_hold;

    out->troubles = (uint16_t)(out->troubles + popcount32(e->sys_trouble | e->sys_trouble_mem));
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        const uint32_t bit = 1u << d;
        const uint8_t dp = e->cfg.devices[d].partition;
        if (((e->dev_trouble | e->dev_trouble_mem) & bit) != 0u && (dp == 0u || dp == p)) {
            out->troubles++;
        }
    }
}

/* The snapshot keeps one per zone: keep it small (BL-128 decision 12). */
_Static_assert(sizeof(alarm_zone_status_t) <= 8u, "alarm_zone_status_t grew past 8 bytes");

void alarm_engine_zone_status(const alarm_engine_t *e, uint16_t zone, alarm_zone_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (e == NULL || !eng_zone_valid(e, zone)) {
        return;
    }
    const alarm_zone_config_t *zc = eng_zcfg(e, zone);
    const alarm_zone_rt_t *zr = &e->zones[zone - 1u];
    const bool armed = e->parts[zc->partition - 1u].state != ALARM_STATE_DISARMED;
    uint16_t f = 0u;

    if ((zr->cond & ZC_FAULT) != 0u) {
        f |= ALARM_ZS_FAULT;
    }
    if ((zr->flags & ZRT_BYPASSED) != 0u) {
        f |= ALARM_ZS_BYPASSED;
    }
    if ((zr->flags & ZRT_MEMORY) != 0u && !(armed && (zr->flags & ZRT_SILENT_MEM) != 0u)) {
        f |= ALARM_ZS_ALARM;
    }
    if ((zr->trouble & (1u << ALARM_ZTRBL_LOOP)) != 0u ||
        (zr->trouble & (1u << ALARM_ZTRBL_DAY_FAULT)) != 0u) {
        f |= ALARM_ZS_TROUBLE;
    }
    if ((zr->trouble & (1u << ALARM_ZTRBL_TAMPER)) != 0u) {
        f |= ALARM_ZS_TAMPER;
    }
    if ((zr->trouble & (1u << ALARM_ZTRBL_LOW_BATTERY)) != 0u) {
        f |= ALARM_ZS_LOW_BATTERY;
    }
    if ((zr->trouble & (1u << ALARM_ZTRBL_SUPERVISION)) != 0u) {
        f |= ALARM_ZS_SUPERVISION;
    }
    if ((zr->flags & ZRT_SWINGER) != 0u) {
        f |= ALARM_ZS_SWINGER;
    }
    if (zr->verify != FV_IDLE) {
        f |= ALARM_ZS_VERIFYING;
    }
    if (zr->trouble_mem != 0u) {
        f |= ALARM_ZS_TROUBLE_MEM;
    }
    if (eng_zone_blocks_level(e, zone, ALARM_LEVEL_AWAY) ||
        eng_zone_blocks_level(e, zone, ALARM_LEVEL_NIGHT)) {
        f |= ALARM_ZS_NOT_READY;
    }
    out->flags = f;
    out->trouble = zr->trouble;
    const alarm_walk_zone_rt_t *w = &e->walk[zone - 1u];
    out->walk = w->mark;
    out->walk_first_s = w->first_s;
    out->walk_trips = w->trips;
}

uint32_t alarm_engine_system_troubles(const alarm_engine_t *e, uint32_t *restored_unacked)
{
    if (e == NULL) {
        return 0u;
    }
    if (restored_unacked != NULL) {
        *restored_unacked = e->sys_trouble_mem;
    }
    return e->sys_trouble;
}

uint32_t alarm_engine_device_troubles(const alarm_engine_t *e)
{
    return (e != NULL) ? e->dev_trouble : 0u;
}

void alarm_engine_runtime_state(const alarm_engine_t *e, alarm_runtime_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (e == NULL) {
        return;
    }
    out->version = ALARM_RUNTIME_VERSION;
    out->event_seq = e->seq;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_rt_t *pr = &e->parts[i];
        alarm_partition_runtime_t *rp = &out->part[i];
        rp->state = (uint8_t)((pr->state == ALARM_STATE_DISARMED) ? ALARM_STATE_DISARMED
                                                                   : ALARM_STATE_ARMED);
        rp->level = (uint8_t)pr->level;
        rp->flags = pr->flags;
        rp->armed_by = pr->armed_by;
        if (pr->state != ALARM_STATE_DISARMED) {
            rp->arm_src = (uint8_t)(pr->arm_source.kind + 1u);
        }
        rp->memory = pr->memory;
        rp->chime = pr->chime;
    }
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        const uint16_t f = e->zones[z - 1u].flags;
        if ((f & ZRT_BYPASSED) != 0u) {
            bm_set(out->bypassed, z);
        }
        if ((f & ZRT_MEMORY) != 0u) {
            bm_set(out->in_memory, z);
        }
    }
}
