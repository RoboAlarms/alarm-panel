/*
 * users.c - code authentication, keypad lockout and user code programming.
 */
#include <string.h>

#include "engine_internal.h"

alarm_authority_t eng_user_auth(const alarm_engine_t *e, uint8_t user)
{
    if (user < 1u || user > ALARM_MAX_USERS) {
        return ALARM_AUTH_NONE;
    }
    return e->cfg.users[user - 1u].authority;
}

/* Number of digits in a code, 0 if it isn't 4..ALARM_CODE_MAX_LEN digits. */
static size_t code_digits(const char *code)
{
    if (code == NULL) {
        return 0u;
    }
    size_t n = 0;
    while (n <= ALARM_CODE_MAX_LEN && code[n] != '\0') {
        if (code[n] < '0' || code[n] > '9') {
            return 0u;
        }
        n++;
    }
    return (n >= ALARM_CODE_MIN_LEN && n <= ALARM_CODE_MAX_LEN) ? n : 0u;
}

bool eng_code_format_ok(const char *code)
{
    return code_digits(code) != 0u;
}

/* New codes must also meet the installer's minimum length. */
static bool code_policy_ok(const alarm_engine_t *e, const char *code)
{
    const size_t n = code_digits(code);
    return n != 0u && n >= e->cfg.opt.code_min_len;
}

static bool hash_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < ALARM_CODE_HASH_LEN; i++) {
        diff = (uint8_t)(diff | (a[i] ^ b[i]));
    }
    return diff == 0u;
}

static bool hash_code(alarm_engine_t *e, const char *code, uint8_t out[ALARM_CODE_HASH_LEN])
{
    const uint32_t faults = e->ports.hash_faults != NULL ? e->ports.hash_faults(e->ports.ctx) : 0u;
    e->ports.hash_code(e->ports.ctx, e->cfg.code_salt, code, out);
    return e->ports.hash_faults == NULL || e->ports.hash_faults(e->ports.ctx) == faults;
}

/* A successful hash can match no user; a failed hash must never become a wrong-code attempt. */
static alarm_result_t find_user(alarm_engine_t *e, const char *code, uint8_t *user_out,
                                uint8_t hash_out[ALARM_CODE_HASH_LEN])
{
    uint8_t h[ALARM_CODE_HASH_LEN];
    *user_out = 0u;
    if (!hash_code(e, code, h)) {
        return ALARM_ERR_UNAVAILABLE;
    }
    if (hash_out != NULL) {
        memcpy(hash_out, h, sizeof(h));
    }
    uint8_t match = 0;
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        const alarm_user_t *usr = &e->cfg.users[u];
        if (usr->authority != ALARM_AUTH_NONE && hash_equal(h, usr->code_hash) && match == 0u) {
            match = (uint8_t)(u + 1u);
        }
    }
    *user_out = match;
    return ALARM_OK;
}

static bool same_source(const alarm_source_t *a, const alarm_source_t *b)
{
    return a->kind == b->kind && a->id == b->id;
}

static alarm_lockout_t *lockout_slot(alarm_engine_t *e, const alarm_source_t *src, bool create)
{
    alarm_lockout_t *free_slot = NULL;
    alarm_lockout_t *oldest = NULL;
    for (unsigned i = 0; i < ALARM_MAX_SOURCES; i++) {
        alarm_lockout_t *l = &e->lockouts[i];
        if (l->used && same_source(&l->src, src)) {
            return l;
        }
        if (!l->used && free_slot == NULL) {
            free_slot = l;
        }
        if (l->used && l->locked_until <= e->now && (oldest == NULL || l->last < oldest->last)) {
            oldest = l;
        }
    }
    if (!create) {
        return NULL;
    }
    alarm_lockout_t *l = (free_slot != NULL) ? free_slot : oldest;
    if (l == NULL) {
        l = &e->lockouts[0];
    }
    memset(l, 0, sizeof(*l));
    l->used = true;
    l->src = *src;
    return l;
}

/* KPD-008: the time a source stays locked out, from `now`; 0 when it is not locked. */
alarm_ms_t alarm_engine_lockout_left(const alarm_engine_t *e, const alarm_source_t *src,
                                     alarm_ms_t now)
{
    if (e == NULL || src == NULL) {
        return 0u;
    }
    for (unsigned i = 0; i < ALARM_MAX_SOURCES; i++) {
        const alarm_lockout_t *l = &e->lockouts[i];
        if (l->used && same_source(&l->src, src)) {
            return l->locked_until > now ? l->locked_until - now : 0u;
        }
    }
    return 0u;
}

/* The one partition-scope rule: everything a user does to a partition goes through it, whether
 * the user typed a code or pressed a button zone (CORE-PART-002, CORE-USR-015). Asked again at
 * every use, so taking a partition away from a user revokes it at once. */
bool eng_user_in_partition(const alarm_engine_t *e, uint8_t user, uint8_t partition)
{
    if (user < 1u || user > ALARM_MAX_USERS || partition < 1u ||
        partition > ALARM_MAX_PARTITIONS) {
        return false;
    }
    const alarm_user_t *u = &e->cfg.users[user - 1u];
    if (u->authority == ALARM_AUTH_MASTER || u->authority == ALARM_AUTH_INSTALLER) {
        return true; /* the installer and the system master reach every partition */
    }
    return (u->partitions & (uint8_t)(1u << (partition - 1u))) != 0u;
}

alarm_result_t eng_auth(alarm_engine_t *e, const alarm_source_t *src, const char *code,
                        uint8_t partition, uint8_t *user_out)
{
    const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0u};
    if (src == NULL) {
        src = &sys;
    }
    alarm_lockout_t *lo = lockout_slot(e, src, false);
    if (lo != NULL && lo->locked_until != 0u) {
        if (lo->locked_until > e->now) {
            return ALARM_ERR_LOCKED_OUT;
        }
        lo->locked_until = 0u;
        lo->fails = 0u;
    }

    uint8_t user = 0u;
    if (eng_code_format_ok(code)) {
        const alarm_result_t r = find_user(e, code, &user, NULL);
        if (r != ALARM_OK) {
            return r;
        }
    }
    if (user == 0u) {
        lo = lockout_slot(e, src, true);
        lo->fails++;
        lo->last = e->now;
        eng_emit(e, ALARM_EV_INVALID_CODE, partition, 0u, 0u, src, 0u, 0u, 0u, REPORT_NONE);
        if (e->cfg.opt.lockout_attempts != 0u && lo->fails >= e->cfg.opt.lockout_attempts) {
            lo->fails = 0u;
            lo->locked_until = ms_add(e->now, MS(e->cfg.opt.lockout_s));
            eng_emit(e, ALARM_EV_LOCKOUT, partition, 0u, 0u, src, 0u, 0u, 0u, REPORT_NOW);
        }
        return ALARM_ERR_INVALID_CODE;
    }
    if (lo != NULL) {
        lo->fails = 0u;
        lo->last = e->now;
    }
    if (partition != 0u && !eng_user_in_partition(e, user, partition)) {
        return ALARM_ERR_NOT_AUTHORIZED;
    }
    if (user_out != NULL) {
        *user_out = user;
    }
    return ALARM_OK;
}

alarm_result_t alarm_engine_authenticate(alarm_engine_t *e, const alarm_source_t *src,
                                         const char *code, uint8_t partition, alarm_ms_t now,
                                         uint8_t *user_out)
{
    if (e == NULL || !e->initialized || partition > ALARM_MAX_PARTITIONS) {
        return ALARM_ERR_BAD_ARG;
    }
    if (e->in_call) {
        return ALARM_ERR_BAD_STATE;
    }
    e->in_call = true;
    eng_advance(e, now);
    const alarm_result_t r = eng_auth(e, src, code, partition, user_out);
    eng_finish(e);
    e->in_call = false;
    return r;
}

static bool low_authority(alarm_authority_t a)
{
    return a == ALARM_AUTH_STANDARD || a == ALARM_AUTH_ARM_ONLY || a == ALARM_AUTH_GUEST ||
           a == ALARM_AUTH_DURESS;
}

bool alarm_config_user_manages(const alarm_config_t *cfg, uint8_t actor, uint8_t target)
{
    if (cfg == NULL || actor < 1u || actor > ALARM_MAX_USERS || target < 1u ||
        target > ALARM_MAX_USERS) {
        return false;
    }
    const alarm_user_t *a = &cfg->users[actor - 1u];
    const alarm_user_t *t = &cfg->users[target - 1u];
    switch (a->authority) {
    case ALARM_AUTH_INSTALLER:
        /* The installer manages every user, household codes included (decided
         * 2026-09-23). */
        return true;
    case ALARM_AUTH_MASTER: return target != ALARM_USER_INSTALLER;
    case ALARM_AUTH_PARTITION_MASTER:
        if (target == actor) {
            return true; /* own code; authority/partitions are kept */
        }
        return target > ALARM_USER_MASTER &&
               (t->authority == ALARM_AUTH_NONE || low_authority(t->authority)) &&
               (t->partitions & (uint8_t)~a->partitions) == 0u;
    default: return false;
    }
}

/* May 'actor' program 'target' with the requested authority/partitions? */
static bool may_program(const alarm_engine_t *e, uint8_t actor, uint8_t target,
                        alarm_authority_t authority, uint8_t partitions)
{
    if (!alarm_config_user_manages(&e->cfg, actor, target)) {
        return false;
    }
    if (eng_user_auth(e, actor) != ALARM_AUTH_PARTITION_MASTER || target == actor) {
        return true;
    }
    /* A partition master hands out low authorities within its own partitions only. */
    const uint8_t own = e->cfg.users[actor - 1u].partitions;
    return low_authority(authority) && (partitions & (uint8_t)~own) == 0u;
}

static void copy_name(char *dst, const char *src)
{
    size_t n = 0;
    if (src != NULL) {
        while (n + 1u < ALARM_NAME_LEN && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    memset(dst + n, 0, ALARM_NAME_LEN - n);
}

static uint8_t all_partitions(void)
{
    return (uint8_t)((1u << ALARM_MAX_PARTITIONS) - 1u);
}

/* User codes are added, changed and deleted only while every partition the user belongs to
 * (before and after the change) is disarmed and quiet. */
static alarm_result_t partitions_idle(const alarm_engine_t *e, uint8_t mask)
{
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_rt_t *pr = &e->parts[i];
        if ((mask & (1u << i)) != 0u &&
            (pr->state != ALARM_STATE_DISARMED || pr->sounding != 0u)) {
            return ALARM_ERR_BAD_STATE;
        }
    }
    return ALARM_OK;
}

/* Settle what 'actor' may give 'user_no': slots 1 and 2 have fixed authority, changing your
 * own entry keeps your authority and partitions, masters see every partition. */
static alarm_result_t resolve_target(const alarm_engine_t *e, uint8_t actor, uint8_t user_no,
                                     alarm_authority_t *authority, uint8_t *partitions)
{
    if (user_no == ALARM_USER_INSTALLER) {
        *authority = ALARM_AUTH_INSTALLER;
    } else if (user_no == ALARM_USER_MASTER) {
        *authority = ALARM_AUTH_MASTER;
    } else if (user_no == actor) {
        *authority = eng_user_auth(e, actor);
        *partitions = e->cfg.users[actor - 1u].partitions;
    }
    if (*authority == ALARM_AUTH_INSTALLER || *authority == ALARM_AUTH_MASTER) {
        *partitions = all_partitions();
    }
    if (*authority == ALARM_AUTH_NONE || (unsigned)*authority > (unsigned)ALARM_AUTH_DURESS ||
        (user_no > ALARM_USER_MASTER &&
         (*authority == ALARM_AUTH_INSTALLER || *authority == ALARM_AUTH_MASTER)) ||
        (*partitions & all_partitions()) == 0u) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!may_program(e, actor, user_no, *authority, *partitions)) {
        return ALARM_ERR_NOT_AUTHORIZED;
    }
    return ALARM_OK;
}

/* Who is changing the users: the user the code names (counts toward lockout), or - as
 * CORE-USR-028 - a user the platform has already identified (as_user != 0), whose slot must
 * still hold a user, read again every time. */
static alarm_result_t actor_of(alarm_engine_t *e, const alarm_source_t *src, const char *code,
                               bool as, uint8_t as_user, uint8_t *actor)
{
    if (as) {
        if (eng_user_auth(e, as_user) == ALARM_AUTH_NONE) {
            return ALARM_ERR_NOT_AUTHORIZED;
        }
        *actor = as_user;
        return ALARM_OK;
    }
    return eng_auth(e, src, code, 0u, actor);
}

static alarm_result_t user_set(alarm_engine_t *e, const alarm_source_t *src,
                               const char *actor_code, bool as, uint8_t as_user, uint8_t user_no,
                               const char *new_code, alarm_authority_t authority,
                               uint8_t partitions, const char *name, alarm_ms_t now)
{
    if (e == NULL || !e->initialized) {
        return ALARM_ERR_BAD_ARG;
    }
    if (e->in_call) {
        return ALARM_ERR_BAD_STATE;
    }
    e->in_call = true;
    eng_advance(e, now);

    alarm_result_t r = ALARM_OK;
    uint8_t actor = 0u;
    if (user_no < 1u || user_no > ALARM_MAX_USERS || !code_policy_ok(e, new_code)) {
        r = ALARM_ERR_BAD_ARG;
    } else {
        r = actor_of(e, src, actor_code, as, as_user, &actor);
    }
    if (r == ALARM_OK) {
        r = resolve_target(e, actor, user_no, &authority, &partitions);
    }
    if (r == ALARM_OK) {
        r = partitions_idle(e, (uint8_t)(partitions | e->cfg.users[user_no - 1u].partitions));
    }

    if (r == ALARM_OK) {
        uint8_t h[ALARM_CODE_HASH_LEN];
        uint8_t holder = 0u;
        r = find_user(e, new_code, &holder, h);
        if (r == ALARM_OK && holder != 0u && holder != user_no) {
            r = ALARM_ERR_CODE_IN_USE;
        } else if (r == ALARM_OK) {
            alarm_user_t *u = &e->cfg.users[user_no - 1u];
            const bool keep_name = (name == NULL) && u->authority != ALARM_AUTH_NONE;
            u->authority = authority;
            u->partitions = (uint8_t)(partitions & all_partitions());
            memcpy(u->code_hash, h, sizeof(h));
            if (!keep_name) {
                copy_name(u->name, name);
            }
            eng_emit(e, ALARM_EV_USER_CHANGED, 0u, 0u, actor, src, user_no, (uint8_t)authority,
                     ALARM_EVF_SYSTEM, REPORT_NOW);
            e->config_dirty = true;
            eng_update_default_codes(e);
        }
    }

    eng_finish(e);
    e->in_call = false;
    return r;
}

alarm_result_t alarm_engine_user_set(alarm_engine_t *e, const alarm_source_t *src,
                                     const char *actor_code, uint8_t user_no,
                                     const char *new_code, alarm_authority_t authority,
                                     uint8_t partitions, const char *name, alarm_ms_t now)
{
    return user_set(e, src, actor_code, false, 0u, user_no, new_code, authority, partitions,
                    name, now);
}

alarm_result_t alarm_engine_user_set_as(alarm_engine_t *e, const alarm_source_t *src,
                                        uint8_t actor, uint8_t user_no, const char *new_code,
                                        alarm_authority_t authority, uint8_t partitions,
                                        const char *name, alarm_ms_t now)
{
    return user_set(e, src, NULL, true, actor, user_no, new_code, authority, partitions, name,
                    now);
}

static alarm_result_t user_update(alarm_engine_t *e, const alarm_source_t *src,
                                  const char *actor_code, bool as, uint8_t as_user,
                                  uint8_t user_no, alarm_authority_t authority,
                                  uint8_t partitions, const char *name, alarm_ms_t now)
{
    if (e == NULL || !e->initialized) {
        return ALARM_ERR_BAD_ARG;
    }
    if (e->in_call) {
        return ALARM_ERR_BAD_STATE;
    }
    e->in_call = true;
    eng_advance(e, now);

    alarm_result_t r = ALARM_OK;
    uint8_t actor = 0u;
    if (user_no < 1u || user_no > ALARM_MAX_USERS ||
        e->cfg.users[user_no - 1u].authority == ALARM_AUTH_NONE) {
        r = ALARM_ERR_BAD_ARG; /* add users with alarm_engine_user_set (they need a code) */
    } else {
        r = actor_of(e, src, actor_code, as, as_user, &actor);
    }
    if (r == ALARM_OK) {
        r = resolve_target(e, actor, user_no, &authority, &partitions);
    }
    if (r == ALARM_OK) {
        r = partitions_idle(e, (uint8_t)(partitions | e->cfg.users[user_no - 1u].partitions));
    }
    if (r == ALARM_OK) {
        alarm_user_t *u = &e->cfg.users[user_no - 1u];
        u->authority = authority;
        u->partitions = (uint8_t)(partitions & all_partitions());
        if (name != NULL) {
            copy_name(u->name, name);
        }
        eng_emit(e, ALARM_EV_USER_UPDATED, 0u, 0u, actor, src, user_no, (uint8_t)authority,
                 ALARM_EVF_SYSTEM, REPORT_NOW);
        e->config_dirty = true;
    }

    eng_finish(e);
    e->in_call = false;
    return r;
}

alarm_result_t alarm_engine_user_update(alarm_engine_t *e, const alarm_source_t *src,
                                        const char *actor_code, uint8_t user_no,
                                        alarm_authority_t authority, uint8_t partitions,
                                        const char *name, alarm_ms_t now)
{
    return user_update(e, src, actor_code, false, 0u, user_no, authority, partitions, name, now);
}

alarm_result_t alarm_engine_user_update_as(alarm_engine_t *e, const alarm_source_t *src,
                                           uint8_t actor, uint8_t user_no,
                                           alarm_authority_t authority, uint8_t partitions,
                                           const char *name, alarm_ms_t now)
{
    return user_update(e, src, NULL, true, actor, user_no, authority, partitions, name, now);
}

alarm_result_t alarm_engine_program_set_code(alarm_engine_t *e, const alarm_source_t *src,
                                             uint8_t user_no, const char *new_code,
                                             alarm_ms_t now)
{
    if (e == NULL || !e->initialized) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!eng_enter(e, now)) {
        return ALARM_ERR_BAD_STATE;
    }
    const alarm_source_t sys = {ALARM_SRC_SYSTEM, 0u};
    if (src == NULL) {
        src = &sys;
    }
    alarm_result_t r = ALARM_OK;
    if (!e->program || src->kind != e->program_src.kind || src->id != e->program_src.id) {
        r = ALARM_ERR_BAD_STATE;
    } else if ((user_no != ALARM_USER_INSTALLER && user_no != ALARM_USER_MASTER) ||
               !code_policy_ok(e, new_code)) {
        r = ALARM_ERR_BAD_ARG;
    }
    if (r == ALARM_OK) {
        uint8_t h[ALARM_CODE_HASH_LEN];
        uint8_t holder = 0u;
        r = find_user(e, new_code, &holder, h);
        if (r == ALARM_OK && holder != 0u && holder != user_no) {
            r = ALARM_ERR_CODE_IN_USE;
        } else if (r == ALARM_OK) {
            alarm_user_t *u = &e->cfg.users[user_no - 1u];
            const bool was_empty = u->authority == ALARM_AUTH_NONE;
            u->authority = (user_no == ALARM_USER_INSTALLER) ? ALARM_AUTH_INSTALLER
                                                             : ALARM_AUTH_MASTER;
            u->partitions = all_partitions();
            memcpy(u->code_hash, h, sizeof(h));
            if (was_empty) {
                copy_name(u->name, (user_no == ALARM_USER_INSTALLER) ? "Installer" : "Master");
            }
            e->program_until = ms_add(e->now, MS(ALARM_PROGRAM_TIMEOUT_S));
            eng_emit(e, ALARM_EV_USER_CHANGED, 0u, 0u, e->program_user, src, user_no,
                     (uint8_t)u->authority, ALARM_EVF_SYSTEM, REPORT_NOW);
            e->config_dirty = true;
            eng_update_default_codes(e);
        }
    }
    eng_leave(e);
    return r;
}

/* Recompute which factory codes are still in use (two hashes; config changes only). */
void eng_update_default_codes(alarm_engine_t *e)
{
    uint8_t mask = 0u;
    uint8_t h[ALARM_CODE_HASH_LEN];
    const alarm_user_t *inst = &e->cfg.users[ALARM_USER_INSTALLER - 1u];
    const alarm_user_t *master = &e->cfg.users[ALARM_USER_MASTER - 1u];

    if (inst->authority == ALARM_AUTH_INSTALLER) {
        if (!hash_code(e, ALARM_DEFAULT_INSTALLER_CODE, h)) {
            return; /* retain the last known warning; never clear it on an unknown hash */
        }
        if (hash_equal(h, inst->code_hash)) {
            mask = (uint8_t)(mask | ALARM_DEFAULT_CODE_INSTALLER);
        }
    }
    if (master->authority == ALARM_AUTH_MASTER) {
        if (!hash_code(e, ALARM_DEFAULT_MASTER_CODE, h)) {
            return;
        }
        if (hash_equal(h, master->code_hash)) {
            mask = (uint8_t)(mask | ALARM_DEFAULT_CODE_MASTER);
        }
    }
    if (mask != e->default_codes) {
        e->default_codes = mask;
        e->sys_dirty = true;
    }
}

uint8_t alarm_engine_default_codes(const alarm_engine_t *e)
{
    return (e != NULL) ? e->default_codes : 0u;
}

static alarm_result_t user_delete(alarm_engine_t *e, const alarm_source_t *src,
                                  const char *actor_code, bool as, uint8_t as_user,
                                  uint8_t user_no, alarm_ms_t now)
{
    if (e == NULL || !e->initialized) {
        return ALARM_ERR_BAD_ARG;
    }
    if (e->in_call) {
        return ALARM_ERR_BAD_STATE;
    }
    e->in_call = true;
    eng_advance(e, now);

    alarm_result_t r = ALARM_OK;
    uint8_t actor = 0u;
    if (user_no <= ALARM_USER_MASTER || user_no > ALARM_MAX_USERS) {
        r = ALARM_ERR_BAD_ARG; /* installer and system master cannot be deleted */
    } else {
        r = actor_of(e, src, actor_code, as, as_user, &actor);
    }
    if (r == ALARM_OK) {
        alarm_user_t *u = &e->cfg.users[user_no - 1u];
        if (u->authority == ALARM_AUTH_NONE) {
            r = ALARM_ERR_BAD_ARG;
        } else if (actor == user_no ||
                   !may_program(e, actor, user_no, ALARM_AUTH_STANDARD, u->partitions)) {
            r = ALARM_ERR_NOT_AUTHORIZED;
        } else if (partitions_idle(e, u->partitions) != ALARM_OK) {
            r = ALARM_ERR_BAD_STATE;
        } else {
            memset(u, 0, sizeof(*u));
            eng_emit(e, ALARM_EV_USER_DELETED, 0u, 0u, actor, src, user_no, 0u,
                     ALARM_EVF_SYSTEM, REPORT_NOW);
            e->config_dirty = true;
        }
    }

    eng_finish(e);
    e->in_call = false;
    return r;
}

alarm_result_t alarm_engine_user_delete(alarm_engine_t *e, const alarm_source_t *src,
                                        const char *actor_code, uint8_t user_no,
                                        alarm_ms_t now)
{
    return user_delete(e, src, actor_code, false, 0u, user_no, now);
}

alarm_result_t alarm_engine_user_delete_as(alarm_engine_t *e, const alarm_source_t *src,
                                           uint8_t actor, uint8_t user_no, alarm_ms_t now)
{
    return user_delete(e, src, NULL, true, actor, user_no, now);
}
