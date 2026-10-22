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
 *   ZW_STATUS            -                                  -> u8 present, rec status
 *   ZW_DEVICES           u16 max                            -> list of devices
 *   ZW_DEVICE            u16 node, u16 generation           -> u8 found, rec page
 *   ZW_REQUEST           rec request (its user: the grant's) -> u8 result
 *   ZW_PROGRESS          -                                  -> rec progress
 *   ZW_PROV              u16 max                            -> list of entries
 *   ZW_PROV_REGISTER     str code, str name                 -> u8 result
 *   ZW_PROV_CHANGE       16 bytes dsk, u8 op, u8 arg        -> u8 result
 *   ZW_RADIO_SD_INSTALL  -                                  -> u8 result (the grant's user)
 *   ZW_RADIO_SD_STATE    -                                  -> u8 state
 *   ZB_STATUS            -                                  -> u8 present, rec status
 *   ZB_DEVICES           u16 max                            -> list of devices
 *   ZB_DEVICE            u16 id                             -> u8 found, rec page
 *   ZB_REQUEST           rec request                        -> u8 result
 *   ZB_PROGRESS          -                                  -> rec progress
 *   ZB_CODES             u16 max                            -> list of codes
 *   OUT_STATUS           -                                  -> u8 present, rec status, str where
 *   OUT_WIRED            -                                  -> list of UI_OUT_WIRED_MAX channels
 *   OUT_WIRED_SET        u8 slot, rec channel               -> u8 result
 *   OUT_WIRED_TEST       u8 slot                            -> u8 result
 *   OUT_TERMINALS        u8 slot, u16 max                   -> u16 count, count bytes
 *   OUT_RF               -                                  -> list of UI_OUT_RF_MAX sirens
 *   OUT_RF_ADD           -                                  -> u8 result, u8 slot
 *   OUT_RF_SET           u8 slot, rec siren                 -> u8 result
 *   OUT_RF_CLEAR, OUT_RF_PAIR, OUT_RF_TEST   u8 slot        -> u8 result
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "panel_link/plink_crypto.h"
#include "ur_internal.h"

_Static_assert(UR_AREA_SESSION == UI_LINK_AREA_SESSION && UR_AREA_CONFIG == UI_LINK_AREA_CONFIG &&
                   UR_AREA_USERS == UI_LINK_AREA_USERS && UR_AREA_ZONES == UI_LINK_AREA_ZONES &&
                   UR_AREA_ZWAVE == UI_LINK_AREA_ZWAVE && UR_AREA_ZIGBEE == UI_LINK_AREA_ZIGBEE &&
                   UR_AREA_OUTPUTS == UI_LINK_AREA_OUTPUTS,
               "the screen's area bits are ui_remote's");

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
    {UR_ZW_STATUS, UR_ACCESS_INSTALLER, true, false},
    {UR_ZW_DEVICES, UR_ACCESS_INSTALLER, true, false},
    {UR_ZW_DEVICE, UR_ACCESS_INSTALLER, true, false},
    {UR_ZW_REQUEST, UR_ACCESS_INSTALLER, false, true},
    {UR_ZW_PROGRESS, UR_ACCESS_INSTALLER, true, false},
    {UR_ZW_PROV, UR_ACCESS_INSTALLER, true, false},
    {UR_ZW_PROV_REGISTER, UR_ACCESS_INSTALLER, false, true},
    {UR_ZW_PROV_CHANGE, UR_ACCESS_INSTALLER, false, false},
    {UR_ZW_RADIO_SD_INSTALL, UR_ACCESS_INSTALLER, false, false},
    {UR_ZW_RADIO_SD_STATE, UR_ACCESS_INSTALLER, true, false},
    {UR_ZB_STATUS, UR_ACCESS_INSTALLER, true, false},
    {UR_ZB_DEVICES, UR_ACCESS_INSTALLER, true, false},
    {UR_ZB_DEVICE, UR_ACCESS_INSTALLER, true, false},
    {UR_ZB_REQUEST, UR_ACCESS_INSTALLER, false, true},
    {UR_ZB_PROGRESS, UR_ACCESS_INSTALLER, true, false},
    {UR_ZB_CODES, UR_ACCESS_INSTALLER, true, false},
    {UR_OUT_STATUS, UR_ACCESS_INSTALLER, true, false},
    {UR_OUT_WIRED, UR_ACCESS_INSTALLER, true, false},
    {UR_OUT_WIRED_SET, UR_ACCESS_INSTALLER, false, false},
    {UR_OUT_WIRED_TEST, UR_ACCESS_INSTALLER, false, false},
    {UR_OUT_TERMINALS, UR_ACCESS_INSTALLER, true, false},
    {UR_OUT_RF, UR_ACCESS_INSTALLER, true, false},
    {UR_OUT_RF_ADD, UR_ACCESS_INSTALLER, false, false},
    {UR_OUT_RF_SET, UR_ACCESS_INSTALLER, false, false},
    {UR_OUT_RF_CLEAR, UR_ACCESS_INSTALLER, false, false},
    {UR_OUT_RF_PAIR, UR_ACCESS_INSTALLER, false, false},
    {UR_OUT_RF_TEST, UR_ACCESS_INSTALLER, false, false},
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
    if (be->zw_status != NULL && be->zw_devices != NULL && be->zw_device != NULL &&
        be->zw_request != NULL && be->zw_progress != NULL) {
        a |= UR_AREA_ZWAVE;
    }
    if (be->zb_status != NULL && be->zb_devices != NULL && be->zb_device != NULL &&
        be->zb_request != NULL && be->zb_progress != NULL) {
        a |= UR_AREA_ZIGBEE;
    }
    if (be->out_status != NULL && be->out_wired != NULL && be->out_rf != NULL) {
        a |= UR_AREA_OUTPUTS;
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
    if (code[0] == '\0') {
        return UR_NOT_AUTHORIZED; /* the power-up recovery is for whoever is at the alarm panel */
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

/* The radios (LINK-062): their pairing and learn modes run on this panel's radios; what follows
 * them is their progress, read by the keypad. Entries an alarm panel lacks answer UNAVAILABLE. */
#define NEED(fn)                                                                                \
    do {                                                                                        \
        if (be->fn == NULL) {                                                                   \
            return UR_UNAVAILABLE;                                                              \
        }                                                                                       \
    } while (0)

static size_t cap_max(plink_r_t *r, size_t room)
{
    const size_t max = plink_r_u16(r);
    return max < room ? max : room;
}

static ui_remote_status_t zwave(ui_remote_server_t *s, const ui_backend_t *be,
                                const ui_remote_grant_t *g, uint16_t call, plink_r_t *r,
                                plink_w_t *w)
{
    switch (call) {
    case UR_ZW_STATUS: {
        memset(&s->u.zw_status, 0, sizeof(s->u.zw_status));
        const bool ok = be->zw_status(be->ctx, &s->u.zw_status);
        plink_w_u8(w, ok ? 1u : 0u);
        ur_put(w, &ur_t_zw_status, &s->u.zw_status);
        break;
    }
    case UR_ZW_DEVICES: {
        const size_t max = cap_max(r, UI_ZW_DEVICES_MAX);
        const size_t n = r->ok ? be->zw_devices(be->ctx, s->u.zw_devices, max) : 0u;
        ur_put_list(w, &ur_t_zw_device, s->u.zw_devices, n);
        break;
    }
    case UR_ZW_DEVICE: {
        const uint16_t node = plink_r_u16(r), gen = plink_r_u16(r);
        if (!r->ok) {
            return UR_BAD;
        }
        memset(&s->u.zw_page, 0, sizeof(s->u.zw_page));
        const bool found = be->zw_device(be->ctx, node, gen, &s->u.zw_page);
        plink_w_u8(w, found ? 1u : 0u);
        ur_put(w, &ur_t_zw_device_page, &s->u.zw_page);
        break;
    }
    case UR_ZW_REQUEST: {
        ui_zw_request_t q;
        ur_get(r, &ur_t_zw_request, &q);
        if (!r->ok) {
            return UR_BAD;
        }
        q.user = g->user; /* who asks is the grant's, never what the keypad says */
        plink_w_u8(w, (uint8_t)be->zw_request(be->ctx, &q));
        plink_wipe(&q, sizeof(q));
        break;
    }
    case UR_ZW_PROGRESS:
        memset(&s->u.zw_progress, 0, sizeof(s->u.zw_progress));
        be->zw_progress(be->ctx, &s->u.zw_progress);
        ur_put(w, &ur_t_zw_progress, &s->u.zw_progress);
        break;
    case UR_ZW_PROV: {
        NEED(zw_prov);
        const size_t max = cap_max(r, UI_ZW_PROV_MAX);
        const size_t n = r->ok ? be->zw_prov(be->ctx, s->u.zw_prov, max) : 0u;
        ur_put_list(w, &ur_t_zw_prov, s->u.zw_prov, n);
        break;
    }
    case UR_ZW_PROV_REGISTER: {
        NEED(zw_prov_register);
        char code[128], name[64];
        ur_get_str(r, code, sizeof(code), false);
        ur_get_str(r, name, sizeof(name), true);
        ui_remote_status_t st = UR_BAD;
        if (r->ok) {
            plink_w_u8(w, (uint8_t)be->zw_prov_register(be->ctx, code, name));
            st = UR_OK;
        }
        plink_wipe(code, sizeof(code));
        return st;
    }
    case UR_ZW_PROV_CHANGE: {
        NEED(zw_prov_change);
        uint8_t dsk[16];
        plink_r_bytes(r, dsk, sizeof(dsk));
        const uint8_t op = plink_r_u8(r), arg = plink_r_u8(r);
        if (!r->ok) {
            return UR_BAD;
        }
        plink_w_u8(w, (uint8_t)be->zw_prov_change(be->ctx, dsk, op, arg));
        break;
    }
    case UR_ZW_RADIO_SD_INSTALL:
        NEED(zw_radio_sd_install);
        plink_w_u8(w, be->zw_radio_sd_install(be->ctx, g->user));
        break;
    case UR_ZW_RADIO_SD_STATE:
        NEED(zw_radio_sd_state);
        plink_w_u8(w, be->zw_radio_sd_state(be->ctx));
        break;
    default: return UR_UNAVAILABLE;
    }
    return UR_OK;
}

static ui_remote_status_t zigbee(ui_remote_server_t *s, const ui_backend_t *be, uint16_t call,
                                 plink_r_t *r, plink_w_t *w)
{
    switch (call) {
    case UR_ZB_STATUS: {
        memset(&s->u.zb_status, 0, sizeof(s->u.zb_status));
        const bool ok = be->zb_status(be->ctx, &s->u.zb_status);
        plink_w_u8(w, ok ? 1u : 0u);
        ur_put(w, &ur_t_zb_status, &s->u.zb_status);
        break;
    }
    case UR_ZB_DEVICES: {
        const size_t max = cap_max(r, UI_ZB_DEVICES_MAX);
        const size_t n = r->ok ? be->zb_devices(be->ctx, s->u.zb_devices, max) : 0u;
        ur_put_list(w, &ur_t_zb_device, s->u.zb_devices, n);
        break;
    }
    case UR_ZB_DEVICE: {
        const uint16_t id = plink_r_u16(r);
        if (!r->ok) {
            return UR_BAD;
        }
        memset(&s->u.zb_page, 0, sizeof(s->u.zb_page));
        const bool found = be->zb_device(be->ctx, id, &s->u.zb_page);
        plink_w_u8(w, found ? 1u : 0u);
        ur_put(w, &ur_t_zb_device_page, &s->u.zb_page);
        break;
    }
    case UR_ZB_REQUEST: {
        ui_zb_request_t q;
        ur_get(r, &ur_t_zb_request, &q);
        if (!r->ok) {
            plink_wipe(&q, sizeof(q));
            return UR_BAD;
        }
        plink_w_u8(w, (uint8_t)be->zb_request(be->ctx, &q));
        plink_wipe(&q, sizeof(q)); /* its install code, its installer code */
        break;
    }
    case UR_ZB_PROGRESS:
        memset(&s->u.zb_progress, 0, sizeof(s->u.zb_progress));
        be->zb_progress(be->ctx, &s->u.zb_progress);
        ur_put(w, &ur_t_zb_progress, &s->u.zb_progress);
        break;
    case UR_ZB_CODES: {
        NEED(zb_codes);
        const size_t max = cap_max(r, UI_ZB_CODES_MAX);
        const size_t n = r->ok ? be->zb_codes(be->ctx, s->u.zb_codes, max) : 0u;
        ur_put_list(w, &ur_t_zb_code, s->u.zb_codes, n);
        break;
    }
    default: return UR_UNAVAILABLE;
    }
    return UR_OK;
}

static ui_remote_status_t outputs(ui_remote_server_t *s, const ui_backend_t *be, uint16_t call,
                                  plink_r_t *r, plink_w_t *w)
{
    switch (call) {
    case UR_OUT_STATUS: {
        memset(&s->u.out_status, 0, sizeof(s->u.out_status));
        const bool ok = be->out_status(be->ctx, &s->u.out_status);
        plink_w_u8(w, ok ? 1u : 0u);
        ur_put(w, &ur_t_out_status, &s->u.out_status);
        ur_put_str(w, s->u.out_status.where); /* a pointer: its words */
        break;
    }
    case UR_OUT_WIRED:
        memset(s->u.out_wired, 0, sizeof(s->u.out_wired));
        be->out_wired(be->ctx, s->u.out_wired);
        ur_put_list(w, &ur_t_out_wired, s->u.out_wired, UI_OUT_WIRED_MAX);
        break;
    case UR_OUT_WIRED_SET: {
        NEED(out_wired_set);
        const uint8_t slot = plink_r_u8(r);
        ui_out_wired_t c;
        ur_get(r, &ur_t_out_wired, &c);
        if (!r->ok) {
            return UR_BAD;
        }
        plink_w_u8(w, be->out_wired_set(be->ctx, slot, &c));
        break;
    }
    case UR_OUT_TERMINALS: {
        NEED(out_terminals);
        const uint8_t slot = plink_r_u8(r);
        const size_t max = cap_max(r, sizeof(s->u.terminals));
        const size_t n = r->ok ? be->out_terminals(be->ctx, slot, s->u.terminals, max) : 0u;
        plink_w_u16(w, (uint16_t)n);
        plink_w_bytes(w, s->u.terminals, n);
        break;
    }
    case UR_OUT_RF:
        memset(s->u.out_rf, 0, sizeof(s->u.out_rf));
        be->out_rf(be->ctx, s->u.out_rf);
        ur_put_list(w, &ur_t_out_rf, s->u.out_rf, UI_OUT_RF_MAX);
        break;
    case UR_OUT_RF_ADD: {
        NEED(out_rf_add);
        uint8_t slot = 0;
        plink_w_u8(w, be->out_rf_add(be->ctx, &slot));
        plink_w_u8(w, slot);
        break;
    }
    case UR_OUT_RF_SET: {
        NEED(out_rf_set);
        const uint8_t slot = plink_r_u8(r);
        ui_out_rf_t c;
        ur_get(r, &ur_t_out_rf, &c);
        if (!r->ok) {
            return UR_BAD;
        }
        plink_w_u8(w, be->out_rf_set(be->ctx, slot, &c));
        break;
    }
    case UR_OUT_WIRED_TEST:
    case UR_OUT_RF_CLEAR:
    case UR_OUT_RF_PAIR:
    case UR_OUT_RF_TEST: {
        const uint8_t slot = plink_r_u8(r);
        if (!r->ok) {
            return UR_BAD;
        }
        uint8_t (*fn)(void *, uint8_t) = call == UR_OUT_WIRED_TEST ? be->out_wired_test
                                         : call == UR_OUT_RF_CLEAR ? be->out_rf_clear
                                         : call == UR_OUT_RF_PAIR  ? be->out_rf_pair
                                                                   : be->out_rf_test;
        if (fn == NULL) {
            return UR_UNAVAILABLE;
        }
        plink_w_u8(w, fn(be->ctx, slot));
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
    default:
        switch (UR_GROUP(call)) {
        case UR_GROUP(UR_ZW_STATUS): st = zwave(s, be, g, call, &r, &w); break;
        case UR_GROUP(UR_ZB_STATUS): st = zigbee(s, be, call, &r, &w); break;
        case UR_GROUP(UR_OUT_STATUS): st = outputs(s, be, call, &r, &w); break;
        default: st = zones(s, be, g, call, &r, &w); break;
        }
        break;
    }
    if (st == UR_OK && !w.ok) {
        st = UR_TOO_BIG;
    }
    *out_len = st == UR_OK ? w.len : 0u;
    return st;
}
