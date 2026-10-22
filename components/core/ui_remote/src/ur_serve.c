/*
 * ur_serve.c - the calls' table and the alarm panel's side (LINK-058..LINK-062): a keypad's call
 * runs on the alarm panel's own backend, under the grant the alarm panel holds for that keypad.
 *
 * Arguments and answers, call by call (u8, u16, u32 little endian; str = u16 length + bytes;
 * rec = a record, ur_internal.h; list = u16 count + records):
 *   GRANT_END            -                                  -> -
 *   PROGRAM_ENTER        str code                           -> u8 result, u8 user
 *   PROGRAM_EXIT         -                                  -> -
 *   PROGRAM_KEEPALIVE    -                                  -> -
 *   PROGRAM_SET_CODE     u8 user, str code                  -> u8 result
 *   SET_CONFIG           u32 base cfg_crc, u32 len, blob    -> u8 result
 *   USER_SET             str actor, u8 user, str code, u8 authority, u8 partitions, str name
 *                                                           -> u8 result
 *   USER_UPDATE          str actor, u8 user, u8 authority, u8 partitions, str name -> u8 result
 *   USER_DELETE          str actor, u8 user                 -> u8 result
 *   WEB_LOGIN_GET        u8 user                            -> u8 set
 *   WEB_LOGIN_SET        u8 user, str password, u8 temporary -> u8 ok, str why
 *   WEB_LOGIN_CLEAR      u8 user                            -> -
 *   ZONE_SOURCES         u16 max                            -> list of sources
 *   ZONE_LEARN           u8 kind, u8 on                     -> -
 *   ZONE_CANDIDATES      u8 kind, str query, u16 max        -> list of candidates
 *   ZONE_BIND            u16 zone, rec binding              -> u8 result
 *   ZONE_UNBIND          u16 zone                           -> u8 result
 *   ZONE_INFO            u16 zone                           -> u8 found, rec info
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "panel_link/plink_crypto.h"
#include "ur_internal.h"

static const ui_remote_info_t CALLS[] = {
    {UR_GRANT_END, UR_ACCESS_SESSION, false, false},
    {UR_PROGRAM_ENTER, UR_ACCESS_SESSION, false, true},
    {UR_PROGRAM_EXIT, UR_ACCESS_SESSION, false, false},
    {UR_PROGRAM_KEEPALIVE, UR_ACCESS_SESSION, false, false},
    {UR_PROGRAM_SET_CODE, UR_ACCESS_INSTALLER, false, true},
    {UR_SET_CONFIG, UR_ACCESS_INSTALLER, false, false},
    {UR_USER_SET, UR_ACCESS_USERS, false, true},
    {UR_USER_UPDATE, UR_ACCESS_USERS, false, true},
    {UR_USER_DELETE, UR_ACCESS_USERS, false, true},
    {UR_WEB_LOGIN_GET, UR_ACCESS_USERS, true, false},
    {UR_WEB_LOGIN_SET, UR_ACCESS_USERS, false, true},
    {UR_WEB_LOGIN_CLEAR, UR_ACCESS_USERS, false, false},
    {UR_ZONE_SOURCES, UR_ACCESS_INSTALLER, true, false},
    {UR_ZONE_LEARN, UR_ACCESS_INSTALLER, false, false},
    {UR_ZONE_CANDIDATES, UR_ACCESS_INSTALLER, true, false},
    {UR_ZONE_BIND, UR_ACCESS_INSTALLER, false, false},
    {UR_ZONE_UNBIND, UR_ACCESS_INSTALLER, false, false},
    {UR_ZONE_INFO, UR_ACCESS_INSTALLER, true, false},
};

const ui_remote_info_t *ui_remote_info(uint16_t call)
{
    for (size_t i = 0; i < sizeof(CALLS) / sizeof(CALLS[0]); i++) {
        if ((uint16_t)CALLS[i].call == call) {
            return &CALLS[i];
        }
    }
    return NULL;
}

uint32_t ui_remote_areas(const ui_backend_t *be)
{
    uint32_t a = 0;
    if (be->program_enter != NULL && be->program_exit != NULL) {
        a |= UR_AREA_SESSION;
    }
    if (be->set_config != NULL && be->get_config != NULL) {
        a |= UR_AREA_CONFIG;
    }
    if (be->user_set != NULL && be->user_update != NULL && be->user_delete != NULL) {
        a |= UR_AREA_USERS;
    }
    if (be->zone_sources != NULL && be->zone_bind != NULL && be->zone_info != NULL) {
        a |= UR_AREA_ZONES;
    }
    return a;
}

/* ---- the grant (LINK-059, LINK-060) ---- */

void ui_remote_grant_user(ui_remote_grant_t *g, uint8_t user, uint8_t authority, uint64_t now)
{
    g->user = user;
    g->authority = authority;
    g->last_ms = now;
}

static void stop_learning(const ui_backend_t *be, ui_remote_grant_t *g)
{
    if (g->learning != 0u && be->zone_learn != NULL) {
        be->zone_learn(be->ctx, (uint8_t)(g->learning - 1u), false);
    }
    g->learning = 0;
}

static void end_installer(const ui_backend_t *be, ui_remote_grant_t *g, bool exit)
{
    stop_learning(be, g);
    if (g->installer && exit && be->program_exit != NULL) {
        be->program_exit(be->ctx);
    }
    g->installer = false;
}

void ui_remote_grant_tick(const ui_backend_t *be, ui_remote_grant_t *g, bool gone, uint64_t now)
{
    if (gone) {
        end_installer(be, g, true);
        memset(g, 0, sizeof(*g));
        return;
    }
    if (g->installer) {
        return; /* installer mode has its own idle time (ALARM_PROGRAM_TIMEOUT_S) */
    }
    if (g->user != 0u && now - g->last_ms >= UR_GRANT_IDLE_MS) {
        stop_learning(be, g);
        memset(g, 0, sizeof(*g));
    }
}

static bool is_master(uint8_t a)
{
    return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_INSTALLER;
}

/* docs/features/14 "Who can change what", as ui_settings_allowed() judges it on a screen */
static ui_remote_status_t judge(ui_remote_server_t *s, const ui_backend_t *be,
                                ui_remote_grant_t *g, ui_remote_access_t need)
{
    if (g->installer || need == UR_ACCESS_INSTALLER) {
        /* installer mode ended elsewhere (the alarm panel's screen, its idle time): ours too */
        be->get_snapshot(be->ctx, &s->snap);
        if (g->installer && !s->snap.program) {
            end_installer(be, g, false);
        }
    }
    const uint8_t a = g->user != 0u ? g->authority : (uint8_t)ALARM_AUTH_NONE;
    switch (need) {
    case UR_ACCESS_SESSION: return UR_OK;
    case UR_ACCESS_USERS:
        return is_master(a) || a == ALARM_AUTH_PARTITION_MASTER ? UR_OK : UR_NOT_AUTHORIZED;
    case UR_ACCESS_SHARED:
        return g->installer || is_master(a) || a == ALARM_AUTH_PARTITION_MASTER
                   ? UR_OK
                   : UR_NOT_AUTHORIZED;
    case UR_ACCESS_SYSTEM: return g->installer || is_master(a) ? UR_OK : UR_NOT_AUTHORIZED;
    case UR_ACCESS_INSTALLER:
        if (g->installer) {
            return UR_OK;
        }
        return s->snap.program ? UR_BUSY : UR_NEED_INSTALLER;
    default: return UR_NOT_AUTHORIZED;
    }
}

/* ---- the calls ---- */

#define CODE_CAP (ALARM_CODE_MAX_LEN + 1u)

static ui_remote_status_t program_enter(ui_remote_server_t *s, const ui_backend_t *be,
                                        ui_remote_grant_t *g, plink_r_t *r, plink_w_t *w)
{
    char code[CODE_CAP];
    ur_get_str(r, code, sizeof(code), false);
    if (!r->ok) {
        plink_wipe(code, sizeof(code));
        return UR_BAD;
    }
    be->get_snapshot(be->ctx, &s->snap);
    if (s->snap.program && !g->installer) {
        plink_wipe(code, sizeof(code));
        return UR_BUSY; /* one place at a time (LINK-060) */
    }
    uint8_t user = 0;
    const alarm_result_t res = be->program_enter(be->ctx, code, &user);
    plink_wipe(code, sizeof(code));
    if (res == ALARM_OK) {
        g->installer = true;
        ui_remote_grant_user(g, user, (uint8_t)ALARM_AUTH_INSTALLER, g->last_ms);
    }
    plink_w_u8(w, (uint8_t)res);
    plink_w_u8(w, user);
    return UR_OK;
}

static ui_remote_status_t set_config(ui_remote_server_t *s, const ui_backend_t *be,
                                     uint32_t cfg_crc, bool same_fw, plink_r_t *r, plink_w_t *w)
{
    const uint32_t base = plink_r_u32(r);
    const uint32_t len = plink_r_u32(r);
    const uint8_t *blob = plink_r_take(r, len);
    if (!r->ok || blob == NULL) {
        return UR_BAD;
    }
    if (!same_fw) {
        return UR_UNAVAILABLE; /* it could drop fields its firmware doesn't know (LINK-061) */
    }
    if (base != cfg_crc) {
        return UR_STALE;
    }
    if (alarm_config_decode(blob, len, &s->cfg) != ALARM_OK || !be->get_config(be->ctx, &s->mine)) {
        return UR_BAD;
    }
    /* A keypad's copy holds no code material (LINK-019): the users and their codes stay the
     * alarm panel's, and change only through the user calls and PROGRAM_SET_CODE. */
    memcpy(s->cfg.users, s->mine.users, sizeof(s->cfg.users));
    memcpy(s->cfg.code_salt, s->mine.code_salt, sizeof(s->cfg.code_salt));
    plink_w_u8(w, (uint8_t)be->set_config(be->ctx, &s->cfg));
    plink_wipe(&s->mine, sizeof(s->mine));
    plink_wipe(&s->cfg, sizeof(s->cfg));
    return UR_OK;
}

static ui_remote_status_t users(const ui_backend_t *be, uint16_t call, plink_r_t *r, plink_w_t *w)
{
    char actor[CODE_CAP], code[CODE_CAP], name[ALARM_NAME_LEN];
    memset(code, 0, sizeof(code));
    memset(name, 0, sizeof(name));
    ur_get_str(r, actor, sizeof(actor), false);
    const uint8_t user = plink_r_u8(r);
    if (call == UR_USER_SET) {
        ur_get_str(r, code, sizeof(code), false);
    }
    uint8_t authority = 0, parts = 0;
    if (call != UR_USER_DELETE) {
        authority = plink_r_u8(r);
        parts = plink_r_u8(r);
        ur_get_str(r, name, sizeof(name), true);
    }
    ui_remote_status_t st = UR_BAD;
    if (r->ok) {
        alarm_result_t res;
        if (call == UR_USER_SET) {
            res = be->user_set(be->ctx, actor, user, code, (alarm_authority_t)authority, parts, name);
        } else if (call == UR_USER_UPDATE) {
            res = be->user_update(be->ctx, actor, user, (alarm_authority_t)authority, parts, name);
        } else {
            res = be->user_delete(be->ctx, actor, user);
        }
        plink_w_u8(w, (uint8_t)res);
        st = UR_OK;
    }
    plink_wipe(actor, sizeof(actor));
    plink_wipe(code, sizeof(code));
    return st;
}

static ui_remote_status_t web_login(const ui_backend_t *be, uint16_t call, plink_r_t *r,
                                    plink_w_t *w)
{
    const uint8_t user = plink_r_u8(r);
    if (call == UR_WEB_LOGIN_GET || call == UR_WEB_LOGIN_CLEAR) {
        if (!r->ok) {
            return UR_BAD;
        }
        if (call == UR_WEB_LOGIN_GET) {
            plink_w_u8(w, be->web_login_get != NULL && be->web_login_get(be->ctx, user) ? 1u : 0u);
        } else if (be->web_login_clear != NULL) {
            be->web_login_clear(be->ctx, user);
        }
        return UR_OK;
    }
    char password[72];
    ur_get_str(r, password, sizeof(password), false);
    const bool temporary = plink_r_u8(r) != 0u;
    ui_remote_status_t st = UR_BAD;
    if (r->ok && be->web_login_set != NULL) {
        char why[96] = "";
        const bool ok = be->web_login_set(be->ctx, user, password, temporary, why, sizeof(why));
        plink_w_u8(w, ok ? 1u : 0u);
        ur_put_str(w, why);
        st = UR_OK;
    }
    plink_wipe(password, sizeof(password));
    return st;
}

static ui_remote_status_t zones(ui_remote_server_t *s, const ui_backend_t *be,
                                ui_remote_grant_t *g, uint16_t call, plink_r_t *r, plink_w_t *w)
{
    switch (call) {
    case UR_ZONE_SOURCES: {
        size_t max = plink_r_u16(r);
        const size_t room = sizeof(s->u.sources) / sizeof(s->u.sources[0]);
        max = max < room ? max : room;
        const size_t n = r->ok ? be->zone_sources(be->ctx, s->u.sources, max) : 0u;
        ur_put_list(w, &ur_t_source, s->u.sources, n);
        break;
    }
    case UR_ZONE_LEARN: {
        const uint8_t kind = plink_r_u8(r);
        const bool on = plink_r_u8(r) != 0u;
        if (!r->ok || be->zone_learn == NULL) {
            return r->ok ? UR_UNAVAILABLE : UR_BAD;
        }
        if (g->learning != 0u && g->learning != kind + 1u) {
            stop_learning(be, g); /* one learn mode per keypad */
        }
        be->zone_learn(be->ctx, kind, on);
        g->learning = on ? (uint8_t)(kind + 1u) : 0u;
        break;
    }
    case UR_ZONE_CANDIDATES: {
        const uint8_t kind = plink_r_u8(r);
        char query[64];
        ur_get_str(r, query, sizeof(query), true);
        size_t max = plink_r_u16(r);
        const size_t room = sizeof(s->u.candidates) / sizeof(s->u.candidates[0]);
        max = max < room ? max : room;
        if (!r->ok || be->zone_candidates == NULL) {
            return r->ok ? UR_UNAVAILABLE : UR_BAD;
        }
        const size_t n = be->zone_candidates(be->ctx, kind, query, s->u.candidates, max);
        ur_put_list(w, &ur_t_candidate, s->u.candidates, n);
        break;
    }
    case UR_ZONE_BIND: {
        const uint16_t zone = plink_r_u16(r);
        zone_hub_binding_t b;
        ur_get(r, &ur_t_binding, &b);
        if (!r->ok) {
            return UR_BAD;
        }
        plink_w_u8(w, (uint8_t)be->zone_bind(be->ctx, zone, &b));
        break;
    }
    case UR_ZONE_UNBIND: {
        const uint16_t zone = plink_r_u16(r);
        if (!r->ok || be->zone_unbind == NULL) {
            return r->ok ? UR_UNAVAILABLE : UR_BAD;
        }
        plink_w_u8(w, (uint8_t)be->zone_unbind(be->ctx, zone));
        break;
    }
    case UR_ZONE_INFO: {
        const uint16_t zone = plink_r_u16(r);
        if (!r->ok) {
            return UR_BAD;
        }
        memset(&s->u.info, 0, sizeof(s->u.info));
        const bool found = be->zone_info(be->ctx, zone, &s->u.info);
        plink_w_u8(w, found ? 1u : 0u);
        ur_put(w, &ur_t_info, &s->u.info);
        break;
    }
    default: return UR_UNAVAILABLE;
    }
    return UR_OK;
}

ui_remote_status_t ui_remote_serve(ui_remote_server_t *s, const ui_backend_t *be,
                                   ui_remote_grant_t *g, uint32_t cfg_crc, bool same_fw,
                                   uint16_t call, const uint8_t *args, size_t args_len,
                                   uint8_t *out, size_t cap, size_t *out_len, uint64_t now)
{
    *out_len = 0;
    const ui_remote_info_t *info = ui_remote_info(call);
    if (info == NULL || (ui_remote_areas(be) & (1u << UR_GROUP(call))) == 0u) {
        return UR_UNAVAILABLE;
    }
    ui_remote_status_t st = judge(s, be, g, info->need);
    if (st != UR_OK) {
        return st;
    }
    if (g->user != 0u) {
        g->last_ms = now;
    }
    plink_r_t r;
    plink_r_init(&r, args, args_len);
    plink_w_t w;
    plink_w_init(&w, out, cap);
    switch (call) {
    case UR_GRANT_END:
        stop_learning(be, g);
        if (!g->installer) {
            memset(g, 0, sizeof(*g));
        }
        break;
    case UR_PROGRAM_ENTER: st = program_enter(s, be, g, &r, &w); break;
    case UR_PROGRAM_EXIT:
        if (g->installer) {
            end_installer(be, g, true);
        }
        break;
    case UR_PROGRAM_KEEPALIVE:
        if (!g->installer) {
            st = UR_NEED_INSTALLER;
        } else if (be->program_keepalive != NULL) {
            be->program_keepalive(be->ctx);
        }
        break;
    case UR_PROGRAM_SET_CODE: {
        const uint8_t user = plink_r_u8(&r);
        char code[CODE_CAP];
        ur_get_str(&r, code, sizeof(code), false);
        if (!r.ok || be->program_set_code == NULL) {
            st = r.ok ? UR_UNAVAILABLE : UR_BAD;
        } else {
            plink_w_u8(&w, (uint8_t)be->program_set_code(be->ctx, user, code));
        }
        plink_wipe(code, sizeof(code));
        break;
    }
    case UR_SET_CONFIG: st = set_config(s, be, cfg_crc, same_fw, &r, &w); break;
    case UR_USER_SET:
    case UR_USER_UPDATE:
    case UR_USER_DELETE: st = users(be, call, &r, &w); break;
    case UR_WEB_LOGIN_GET:
    case UR_WEB_LOGIN_SET:
    case UR_WEB_LOGIN_CLEAR: st = web_login(be, call, &r, &w); break;
    default: st = zones(s, be, g, call, &r, &w); break;
    }
    if (st == UR_OK && !w.ok) {
        st = UR_TOO_BIG;
    }
    *out_len = st == UR_OK ? w.len : 0u;
    return st;
}
