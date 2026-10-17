/*
 * program.c - installer mode (programming mode), the power-up recovery window and
 * events recorded on behalf of the platform.
 */
#include <string.h>

#include "engine_internal.h"

bool eng_all_idle(const alarm_engine_t *e)
{
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_rt_t *pr = &e->parts[i];
        if (pr->state != ALARM_STATE_DISARMED || pr->sounding != 0u || pr->walk_test) {
            return false;
        }
    }
    return true;
}

void eng_program_end(alarm_engine_t *e, const alarm_source_t *src, uint8_t why)
{
    const uint8_t user = e->program_user;
    e->program = false;
    e->program_user = 0u;
    e->program_until = 0u;
    eng_emit(e, ALARM_EV_PROGRAM_EXIT, 0u, 0u, user, src, why, 0u, ALARM_EVF_SYSTEM, REPORT_NOW);
    eng_mark_all_dirty(e);
}

/* The recovery window needs no timer: it is checked when used, and keypads count the
 * snapshot's recovery_left_ms down themselves (like exit time). */
alarm_ms_t eng_program_next(const alarm_engine_t *e)
{
    return e->program ? e->program_until : ALARM_TIME_NEVER;
}

void eng_program_due(alarm_engine_t *e)
{
    if (e->program && e->now >= e->program_until) {
        eng_program_end(e, NULL, 1u);
    }
}

static bool same_source(const alarm_source_t *a, const alarm_source_t *b)
{
    return a->kind == b->kind && a->id == b->id;
}

/* as: for the already-identified as_user instead of by a code (CORE-PROG-011). */
static alarm_result_t program_enter(alarm_engine_t *e, const alarm_source_t *src,
                                    const char *code, bool as, uint8_t as_user, alarm_ms_t now,
                                    uint8_t *user_out)
{
    if (e == NULL || !e->initialized) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!eng_enter(e, now)) {
        return ALARM_ERR_BAD_STATE;
    }
    alarm_source_t source = {ALARM_SRC_SYSTEM, 0u};
    if (src != NULL) {
        source = *src;
    }
    const bool recovery = !as && (code == NULL || code[0] == '\0');
    uint8_t user = 0u;
    alarm_result_t r = ALARM_OK;
    if (as) {
        /* The installer, asked again now: a slot changed since the sign-in no longer is. */
        if (eng_user_auth(e, as_user) != ALARM_AUTH_INSTALLER) {
            r = ALARM_ERR_NOT_AUTHORIZED;
        } else {
            user = as_user;
        }
    } else if (recovery) {
        if (e->recovery_until == 0u || e->now >= e->recovery_until) {
            r = ALARM_ERR_CODE_REQUIRED;
        }
    } else {
        r = eng_auth(e, &source, code, 0u, &user);
        if (r == ALARM_OK && eng_user_auth(e, user) != ALARM_AUTH_INSTALLER) {
            r = ALARM_ERR_NOT_AUTHORIZED;
        }
    }
    if (r == ALARM_OK && !eng_all_idle(e)) {
        r = ALARM_ERR_BAD_STATE;
    }
    if (r == ALARM_OK && e->program && !same_source(&e->program_src, &source)) {
        r = ALARM_ERR_BAD_STATE; /* one installer session at a time */
    }
    if (r == ALARM_OK) {
        e->program_until = ms_add(e->now, MS(ALARM_PROGRAM_TIMEOUT_S));
        if (!e->program) {
            e->program = true;
            e->program_user = user;
            e->program_src = source;
            eng_emit(e, ALARM_EV_PROGRAM_ENTER, 0u, 0u, user, &source, recovery ? 1u : 0u, 0u,
                     ALARM_EVF_SYSTEM, REPORT_NOW);
            eng_mark_all_dirty(e);
        }
    }
    if (user_out != NULL) {
        *user_out = user;
    }
    eng_leave(e);
    return r;
}

alarm_result_t alarm_engine_program_enter(alarm_engine_t *e, const alarm_source_t *src,
                                          const char *code, alarm_ms_t now, uint8_t *user_out)
{
    return program_enter(e, src, code, false, 0u, now, user_out);
}

alarm_result_t alarm_engine_program_enter_as(alarm_engine_t *e, const alarm_source_t *src,
                                             uint8_t user, alarm_ms_t now)
{
    return program_enter(e, src, NULL, true, user, now, NULL);
}

void alarm_engine_program_exit(alarm_engine_t *e, const alarm_source_t *src, alarm_ms_t now)
{
    if (e == NULL || !eng_enter(e, now)) {
        return;
    }
    if (e->program) {
        eng_program_end(e, src, 0u);
    }
    eng_leave(e);
}

void alarm_engine_program_keepalive(alarm_engine_t *e, alarm_ms_t now)
{
    if (e == NULL || !eng_enter(e, now)) {
        return;
    }
    if (e->program) {
        e->program_until = ms_add(e->now, MS(ALARM_PROGRAM_TIMEOUT_S));
    }
    eng_leave(e);
}

bool alarm_engine_program_active(const alarm_engine_t *e)
{
    return e != NULL && e->program;
}

alarm_ms_t alarm_engine_recovery_left(const alarm_engine_t *e)
{
    if (e == NULL || e->recovery_until == 0u || e->now >= e->recovery_until) {
        return 0u;
    }
    return e->recovery_until - e->now;
}

/* A firmware update's stage with its fields: the panel's own (arg2 0, any source), or a keypad's
 * through the link (docs/features/11, What people see), from {ALARM_SRC_KEYPAD, its device
 * number}, with a why only when it failed. */
static bool fw_event_ok(uint16_t zone, uint8_t arg, uint8_t arg2, const alarm_source_t *src)
{
    if (zone != 0u || arg > (uint8_t)ALARM_FW_KEYPAD_KEY_ACCEPTED) {
        return false;
    }
    if (arg <= (uint8_t)ALARM_FW_RESTART_PUT_OFF) {
        return arg2 == 0u;
    }
    if (src == NULL || src->kind != (uint8_t)ALARM_SRC_KEYPAD || src->id == 0u ||
        src->id > ALARM_MAX_DEVICES) {
        return false;
    }
    return arg == (uint8_t)ALARM_FW_KEYPAD_FAILED ? arg2 != 0u : arg2 == 0u;
}

/* Which platform events exist, with which fields. */
static bool platform_event_ok(alarm_event_type_t type, uint16_t zone, uint8_t arg, uint8_t arg2,
                              const alarm_source_t *src)
{
    if (zone > ALARM_MAX_ZONES) {
        return false;
    }
    switch (type) {
    case ALARM_EV_FACTORY_RESET: return zone == 0u && arg2 == 0u;
    case ALARM_EV_FIRMWARE_UPDATE: return fw_event_ok(zone, arg, arg2, src);
    case ALARM_EV_ZWAVE_ADDED:
        return zone == 0u && arg2 >= ALARM_ZW_JOIN_S2_UNAUTH && arg2 <= ALARM_ZW_JOIN_S0;
    case ALARM_EV_ZWAVE_ADDED_INSECURE:
        return zone == 0u && (arg2 == ALARM_ZW_JOIN_NOT_SECURE || arg2 == ALARM_ZW_JOIN_FAILED ||
                              arg2 == ALARM_ZW_JOIN_DOWNGRADED);
    case ALARM_EV_ZWAVE_REMOVED: return arg2 <= ALARM_ZW_REMOVED_FAILED;
    case ALARM_EV_ZWAVE_LEFT: return arg2 <= ALARM_ZW_LEFT_MISSING;
    case ALARM_EV_ZWAVE_NETWORK_RESET: return zone == 0u && arg == 0u && arg2 == 0u;
    /* ZW-041: arg2 how many restarts the event stands for (0 in records before the count: one) */
    case ALARM_EV_ZWAVE_RADIO_RESTARTED: return zone == 0u;
    case ALARM_EV_ZWAVE_SOFTWARE:
        return arg2 <= ALARM_ZW_SW_FAILED ||
               (arg2 <= ALARM_ZW_SW_RADIO_FAILED && zone == 0u && arg == 0u);
    case ALARM_EV_ZWAVE_GARAGE: return arg2 <= ALARM_ZW_GARAGE_FAILED;
    case ALARM_EV_ZWAVE_JAMMING: return zone == 0u && arg2 == 0u;
    /* Zigbee (devices.md §11): a device found again may already feed a zone */
    case ALARM_EV_ZIGBEE_ADDED: return arg2 < ALARM_ZB_JOIN_COUNT;
    case ALARM_EV_ZIGBEE_REMOVED:
    case ALARM_EV_ZIGBEE_FORGOTTEN:
    case ALARM_EV_ZIGBEE_LEFT:
    case ALARM_EV_ZIGBEE_REPLACED: return arg2 == 0u;
    case ALARM_EV_ZIGBEE_JOIN_REFUSED:
        return zone == 0u && arg == 0u && arg2 < ALARM_ZB_REFUSED_COUNT;
    case ALARM_EV_ZIGBEE_CHANNEL:
        return zone == 0u && arg == 0u && arg2 >= ALARM_ZB_CHANNEL_MIN &&
               arg2 <= ALARM_ZB_CHANNEL_MAX;
    case ALARM_EV_ZIGBEE_NETWORK_RESET: return zone == 0u && arg == 0u && arg2 == 0u;
    case ALARM_EV_ZIGBEE_RADIO_RESTARTED:
        return zone == 0u && arg == 0u && arg2 < ALARM_ZB_RESTART_COUNT;
    case ALARM_EV_BACKUP_MADE:
    case ALARM_EV_BACKUP_RESTORED:
    case ALARM_EV_CLOCK_SET: return zone == 0u && arg == 0u && arg2 == 0u;
    /* AUT-009, CORE-PROG-012: zone the triggering zone or 0 (checked above) */
    case ALARM_EV_AUTOMATION_FAILED:
        /* only what it has words for: a Lock or Close that failed for a device's reason, or an
         * arming skipped for want of a warning */
        if (arg == (uint8_t)ALARM_AUTO_FAIL_NO_WARNING) {
            return arg2 == ALARM_AUTO_ACT_ARM;
        }
        return arg >= (uint8_t)ALARM_AUTO_FAIL_UNREACHABLE && arg < (uint8_t)ALARM_AUTO_FAIL_NO_WARNING &&
               (arg2 == ALARM_AUTO_ACT_LOCK || arg2 == ALARM_AUTO_ACT_CLOSE);
    default: return false;
    }
}

alarm_result_t alarm_engine_platform_event_ex(alarm_engine_t *e, alarm_event_type_t type,
                                              uint16_t zone, uint8_t arg, uint8_t arg2,
                                              const alarm_source_t *src, uint8_t user,
                                              alarm_ms_t now)
{
    if (e == NULL || !e->initialized || !platform_event_ok(type, zone, arg, arg2, src) ||
        user > ALARM_MAX_USERS) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!eng_enter(e, now)) {
        return ALARM_ERR_BAD_STATE;
    }
    eng_emit(e, type, 0u, zone, user, src, arg, arg2, ALARM_EVF_SYSTEM, REPORT_NOW);
    eng_leave(e);
    return ALARM_OK;
}

uint8_t alarm_put_off_minutes(int64_t ev_epoch_ms, int64_t until_epoch_ms)
{
    if (ev_epoch_ms <= 0 || until_epoch_ms <= ev_epoch_ms) {
        return 0u;
    }
    const int64_t m = (until_epoch_ms - ev_epoch_ms + 30000) / 60000;
    if (m < 1) {
        return 1u; /* due within half a minute: still a time to show */
    }
    return (m > 255) ? 255u : (uint8_t)m;
}

int64_t alarm_put_off_until(const alarm_event_t *ev)
{
    if (ev == NULL || ev->type != ALARM_EV_ARMING_PUT_OFF || ev->arg2 == 0u || ev->epoch_ms <= 0) {
        return 0;
    }
    /* minutes were rounded, so the whole minute nearest to the sum is the arm time */
    const int64_t t = ev->epoch_ms + (int64_t)ev->arg2 * 60000 + 29999;
    return t - t % 60000;
}

alarm_result_t alarm_engine_arming_put_off(alarm_engine_t *e, uint8_t partition, uint16_t uid,
                                           uint8_t user, alarm_put_off_t choice,
                                           int64_t until_epoch_ms, alarm_ms_t now)
{
    if (e == NULL || !e->initialized || uid == 0u || user == 0u || user > ALARM_MAX_USERS ||
        choice < ALARM_PUT_OFF_HOUR || choice >= ALARM_PUT_OFF_COUNT ||
        !eng_partition_valid(e, partition)) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!eng_enter(e, now)) {
        return ALARM_ERR_BAD_STATE;
    }
    const alarm_source_t src = {ALARM_SRC_AUTOMATION, uid};
    uint8_t minutes = 0u;
    if (choice == ALARM_PUT_OFF_HOUR && e->ports.get_epoch_ms != NULL) {
        minutes = alarm_put_off_minutes(e->ports.get_epoch_ms(e->ports.ctx), until_epoch_ms);
    }
    eng_emit(e, ALARM_EV_ARMING_PUT_OFF, partition, 0u, user, &src, (uint8_t)choice, minutes, 0u,
             REPORT_NOW);
    eng_leave(e);
    return ALARM_OK;
}

alarm_result_t alarm_engine_platform_event(alarm_engine_t *e, alarm_event_type_t type,
                                           uint8_t arg, const alarm_source_t *src, uint8_t user,
                                           alarm_ms_t now)
{
    return alarm_engine_platform_event_ex(e, type, 0u, arg, 0u, src, user, now);
}
