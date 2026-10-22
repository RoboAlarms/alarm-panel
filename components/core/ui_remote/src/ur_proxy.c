/*
 * ur_proxy.c - the keypad's side (LINK-058, LINK-063): ui_backend_t entries that carry their call
 * to the alarm panel through the port and give back its answer. Their layouts are ur_serve.c's.
 * A call that gets no answer gives what the entry gives for "not now", never a success.
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "panel_link/plink_crypto.h"
#include "ur_internal.h"

static ui_remote_proxy_t *P(void *ctx)
{
    return ctx;
}

void *ui_remote_inner(void *ctx)
{
    return ctx != NULL ? P(ctx)->inner : NULL;
}

static void begin(ui_remote_proxy_t *p, plink_w_t *w)
{
    plink_w_init(w, p->args, sizeof(p->args));
}

/* Sends what w holds; r reads the answer when OK. */
static ui_remote_status_t send(ui_remote_proxy_t *p, uint16_t call, const plink_w_t *w,
                               plink_r_t *r)
{
    size_t n = 0;
    ui_remote_status_t st = UR_TOO_BIG;
    if (w->ok) {
        st = p->port.call(p->port.ctx, call, p->args, w->len, p->ans, sizeof(p->ans), &n);
    }
    const ui_remote_info_t *info = ui_remote_info(call);
    if (info != NULL && info->secret) {
        plink_wipe(p->args, w->len);
    }
    p->last = st;
    plink_r_init(r, p->ans, st == UR_OK ? n : 0u);
    return st;
}

/* "Not now" in the engine's words: a status that isn't OK, or an answer that didn't decode. */
static alarm_result_t result_of(ui_remote_status_t st)
{
    switch (st) {
    case UR_NOT_AUTHORIZED:
    case UR_NEED_INSTALLER: return ALARM_ERR_NOT_AUTHORIZED;
    case UR_LOCKED_OUT: return ALARM_ERR_LOCKED_OUT;
    case UR_BUSY: return ALARM_ERR_BAD_STATE;
    case UR_BAD: return ALARM_ERR_BAD_ARG;
    default: return ALARM_ERR_UNAVAILABLE;
    }
}

static alarm_result_t answer_result(ui_remote_proxy_t *p, ui_remote_status_t st, plink_r_t *r)
{
    if (st != UR_OK) {
        return result_of(st);
    }
    const uint8_t v = plink_r_u8(r);
    if (!r->ok) {
        p->last = UR_BAD;
        return ALARM_ERR_UNAVAILABLE;
    }
    return (alarm_result_t)v;
}

/* ---- session ---- */

static alarm_result_t px_program_enter(void *ctx, const char *code, uint8_t *user_out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    ur_put_str(&w, code != NULL ? code : "");
    const ui_remote_status_t st = send(p, UR_PROGRAM_ENTER, &w, &r);
    const alarm_result_t res = answer_result(p, st, &r);
    const uint8_t user = plink_r_u8(&r);
    if (user_out != NULL) {
        *user_out = res == ALARM_OK ? user : 0u;
    }
    return res;
}

static void px_program_exit(void *ctx)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    (void)send(p, UR_PROGRAM_EXIT, &w, &r);
}

static void px_program_keepalive(void *ctx)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    (void)send(p, UR_PROGRAM_KEEPALIVE, &w, &r);
}

static alarm_result_t px_program_set_code(void *ctx, uint8_t user, const char *new_code)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, user);
    ur_put_str(&w, new_code);
    return answer_result(p, send(p, UR_PROGRAM_SET_CODE, &w, &r), &r);
}

/* ---- configuration ---- */

static alarm_result_t px_set_config(void *ctx, const alarm_config_t *cfg)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u32(&w, p->port.cfg_crc != NULL ? p->port.cfg_crc(p->port.ctx) : 0u);
    const size_t at = w.len;
    plink_w_u32(&w, 0); /* the blob's length, below */
    size_t n = 0;
    if (w.ok && alarm_config_encode(cfg, p->args + w.len, sizeof(p->args) - w.len, &n) == ALARM_OK) {
        for (size_t i = 0; i < 4u; i++) {
            p->args[at + i] = (uint8_t)(n >> (8u * i));
        }
        w.len += n;
    } else {
        w.ok = false;
    }
    return answer_result(p, send(p, UR_SET_CONFIG, &w, &r), &r);
}

/* ---- users ---- */

static alarm_result_t px_user_set(void *ctx, const char *actor, uint8_t user, const char *new_code,
                                  alarm_authority_t authority, uint8_t partitions,
                                  const char *name)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    ur_put_str(&w, actor);
    plink_w_u8(&w, user);
    ur_put_str(&w, new_code);
    plink_w_u8(&w, (uint8_t)authority);
    plink_w_u8(&w, partitions);
    ur_put_str(&w, name);
    return answer_result(p, send(p, UR_USER_SET, &w, &r), &r);
}

static alarm_result_t px_user_update(void *ctx, const char *actor, uint8_t user,
                                     alarm_authority_t authority, uint8_t partitions,
                                     const char *name)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    ur_put_str(&w, actor);
    plink_w_u8(&w, user);
    plink_w_u8(&w, (uint8_t)authority);
    plink_w_u8(&w, partitions);
    ur_put_str(&w, name);
    return answer_result(p, send(p, UR_USER_UPDATE, &w, &r), &r);
}

static alarm_result_t px_user_delete(void *ctx, const char *actor, uint8_t user)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    ur_put_str(&w, actor);
    plink_w_u8(&w, user);
    return answer_result(p, send(p, UR_USER_DELETE, &w, &r), &r);
}

static bool px_web_login_get(void *ctx, uint8_t user)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, user);
    if (send(p, UR_WEB_LOGIN_GET, &w, &r) != UR_OK) {
        return false;
    }
    const uint8_t set = plink_r_u8(&r);
    return r.ok && set != 0u;
}

static bool px_web_login_set(void *ctx, uint8_t user, const char *password, bool temporary,
                             char *why, size_t why_len)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, user);
    ur_put_str(&w, password);
    plink_w_u8(&w, temporary ? 1u : 0u);
    if (why != NULL && why_len > 0u) {
        why[0] = '\0';
    }
    if (send(p, UR_WEB_LOGIN_SET, &w, &r) != UR_OK) {
        return false;
    }
    const bool ok = plink_r_u8(&r) != 0u;
    if (why != NULL && why_len > 0u) {
        ur_get_str(&r, why, why_len, true);
    }
    return r.ok && ok;
}

static void px_web_login_clear(void *ctx, uint8_t user)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, user);
    (void)send(p, UR_WEB_LOGIN_CLEAR, &w, &r);
}

/* ---- zones' sensors ---- */

static size_t px_zone_sources(void *ctx, ui_zone_source_t *out, size_t max)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u16(&w, (uint16_t)(max > 0xFFFFu ? 0xFFFFu : max));
    if (send(p, UR_ZONE_SOURCES, &w, &r) != UR_OK) {
        return 0;
    }
    return ur_get_list(&r, &ur_t_source, out, max);
}

static void px_zone_learn(void *ctx, uint8_t kind, bool on)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, kind);
    plink_w_u8(&w, on ? 1u : 0u);
    (void)send(p, UR_ZONE_LEARN, &w, &r);
}

static size_t px_zone_candidates(void *ctx, uint8_t kind, const char *query,
                                 zsrc_candidate_t *out, size_t max)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, kind);
    ur_put_str(&w, query != NULL ? query : "");
    plink_w_u16(&w, (uint16_t)(max > 0xFFFFu ? 0xFFFFu : max));
    if (send(p, UR_ZONE_CANDIDATES, &w, &r) != UR_OK) {
        return 0;
    }
    return ur_get_list(&r, &ur_t_candidate, out, max);
}

static zone_hub_result_t zone_answer(ui_remote_proxy_t *p, ui_remote_status_t st, plink_r_t *r)
{
    if (st != UR_OK) {
        return st == UR_BAD ? ZONE_HUB_ERR_ARG : ZONE_HUB_ERR_BUSY;
    }
    const uint8_t v = plink_r_u8(r);
    if (!r->ok) {
        p->last = UR_BAD;
        return ZONE_HUB_ERR_BUSY;
    }
    return (zone_hub_result_t)v;
}

static zone_hub_result_t px_zone_bind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u16(&w, zone);
    ur_put(&w, &ur_t_binding, b);
    return zone_answer(p, send(p, UR_ZONE_BIND, &w, &r), &r);
}

static zone_hub_result_t px_zone_unbind(void *ctx, uint16_t zone)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u16(&w, zone);
    return zone_answer(p, send(p, UR_ZONE_UNBIND, &w, &r), &r);
}

static bool px_zone_info(void *ctx, uint16_t zone, zone_hub_info_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u16(&w, zone);
    memset(out, 0, sizeof(*out));
    if (send(p, UR_ZONE_INFO, &w, &r) != UR_OK) {
        return false;
    }
    const bool found = plink_r_u8(&r) != 0u;
    ur_get(&r, &ur_t_info, out);
    if (!r.ok) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return found;
}

void ui_remote_proxy(ui_backend_t *be, ui_remote_proxy_t *p, uint32_t areas)
{
    p->inner = be->ctx;
    p->last = UR_OK;
    be->ctx = p;
    if (areas & UR_AREA_SESSION) {
        be->program_enter = px_program_enter;
        be->program_exit = px_program_exit;
        be->program_keepalive = px_program_keepalive;
        be->program_set_code = px_program_set_code;
    }
    if (areas & UR_AREA_CONFIG) {
        be->set_config = px_set_config;
    }
    if (areas & UR_AREA_USERS) {
        be->user_set = px_user_set;
        be->user_update = px_user_update;
        be->user_delete = px_user_delete;
        be->web_login_get = px_web_login_get;
        be->web_login_set = px_web_login_set;
        be->web_login_clear = px_web_login_clear;
    }
    if (areas & UR_AREA_ZONES) {
        be->zone_sources = px_zone_sources;
        be->zone_learn = px_zone_learn;
        be->zone_candidates = px_zone_candidates;
        be->zone_bind = px_zone_bind;
        be->zone_unbind = px_zone_unbind;
        be->zone_info = px_zone_info;
    }
}
