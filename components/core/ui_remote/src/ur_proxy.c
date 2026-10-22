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

/* ---- Z-Wave ---- */

/* An answer that is a presence flag and a record (the status calls, a device's page). */
static bool flag_rec(ui_remote_proxy_t *p, uint16_t call, plink_w_t *w, const ur_table_t *t,
                     void *out)
{
    plink_r_t r;
    memset(out, 0, t->size);
    if (send(p, call, w, &r) != UR_OK) {
        return false;
    }
    const bool flag = plink_r_u8(&r) != 0u;
    ur_get(&r, t, out);
    if (!r.ok) {
        memset(out, 0, t->size);
        p->last = UR_BAD;
        return false;
    }
    return flag;
}

static size_t list_of(ui_remote_proxy_t *p, uint16_t call, size_t max, const ur_table_t *t,
                      void *out)
{
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u16(&w, (uint16_t)(max > 0xFFFFu ? 0xFFFFu : max));
    if (send(p, call, &w, &r) != UR_OK) {
        return 0;
    }
    return ur_get_list(&r, t, out, max);
}

/* An answer that is one result byte; dflt when none came. */
static uint8_t byte_answer(ui_remote_proxy_t *p, uint16_t call, const plink_w_t *w, uint8_t refused,
                           uint8_t dflt)
{
    plink_r_t r;
    const ui_remote_status_t st = send(p, call, w, &r);
    if (st != UR_OK) {
        return st == UR_NOT_AUTHORIZED || st == UR_NEED_INSTALLER ? refused : dflt;
    }
    const uint8_t v = plink_r_u8(&r);
    if (!r.ok) {
        p->last = UR_BAD;
        return dflt;
    }
    return v;
}

static bool px_zw_status(void *ctx, ui_zw_status_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    return flag_rec(p, UR_ZW_STATUS, &w, &ur_t_zw_status, out);
}

static size_t px_zw_devices(void *ctx, ui_zw_device_t *out, size_t max)
{
    return list_of(P(ctx), UR_ZW_DEVICES, max, &ur_t_zw_device, out);
}

static bool px_zw_device(void *ctx, uint16_t node, uint16_t generation, ui_zw_device_page_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    plink_w_u16(&w, node);
    plink_w_u16(&w, generation);
    return flag_rec(p, UR_ZW_DEVICE, &w, &ur_t_zw_device_page, out);
}

static ui_zw_req_result_t px_zw_request(void *ctx, const ui_zw_request_t *req)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    ur_put(&w, &ur_t_zw_request, req);
    return (ui_zw_req_result_t)byte_answer(p, UR_ZW_REQUEST, &w, UI_ZW_REQ_NOT_ALLOWED,
                                           UI_ZW_REQ_BUSY);
}

static void px_zw_progress(void *ctx, ui_zw_progress_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    memset(out, 0, sizeof(*out));
    if (send(p, UR_ZW_PROGRESS, &w, &r) == UR_OK) {
        ur_get(&r, &ur_t_zw_progress, out);
        if (!r.ok) {
            memset(out, 0, sizeof(*out));
        }
    }
}

static size_t px_zw_prov(void *ctx, ui_zw_prov_t *out, size_t max)
{
    return list_of(P(ctx), UR_ZW_PROV, max, &ur_t_zw_prov, out);
}

static ui_zw_prov_result_t px_zw_prov_register(void *ctx, const char *code, const char *name)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    ur_put_str(&w, code);
    ur_put_str(&w, name != NULL ? name : "");
    return (ui_zw_prov_result_t)byte_answer(p, UR_ZW_PROV_REGISTER, &w, UI_ZW_PROV_NOT_ALLOWED,
                                            UI_ZW_PROV_NOT_ALLOWED);
}

static ui_zw_prov_result_t px_zw_prov_change(void *ctx, const uint8_t dsk[16], uint8_t op,
                                             uint8_t arg)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    plink_w_bytes(&w, dsk, 16u);
    plink_w_u8(&w, op);
    plink_w_u8(&w, arg);
    return (ui_zw_prov_result_t)byte_answer(p, UR_ZW_PROV_CHANGE, &w, UI_ZW_PROV_NOT_ALLOWED,
                                            UI_ZW_PROV_NOT_ALLOWED);
}

static uint8_t px_zw_radio_sd_install(void *ctx, uint8_t user)
{
    ui_remote_proxy_t *p = P(ctx);
    (void)user; /* the master's grant says who */
    plink_w_t w;
    begin(p, &w);
    return byte_answer(p, UR_ZW_RADIO_SD_INSTALL, &w, UI_ZW_SD_NOT_ALLOWED, UI_ZW_SD_BUSY);
}

static uint8_t px_zw_radio_sd_state(void *ctx)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    return byte_answer(p, UR_ZW_RADIO_SD_STATE, &w, UI_ZW_SD_IDLE, UI_ZW_SD_IDLE);
}

/* ---- Zigbee ---- */

static bool px_zb_status(void *ctx, ui_zb_status_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    return flag_rec(p, UR_ZB_STATUS, &w, &ur_t_zb_status, out);
}

static size_t px_zb_devices(void *ctx, ui_zb_device_t *out, size_t max)
{
    return list_of(P(ctx), UR_ZB_DEVICES, max, &ur_t_zb_device, out);
}

static bool px_zb_device(void *ctx, uint16_t id, ui_zb_device_page_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    plink_w_u16(&w, id);
    return flag_rec(p, UR_ZB_DEVICE, &w, &ur_t_zb_device_page, out);
}

static ui_zb_req_result_t px_zb_request(void *ctx, const ui_zb_request_t *req)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    ur_put(&w, &ur_t_zb_request, req);
    return (ui_zb_req_result_t)byte_answer(p, UR_ZB_REQUEST, &w, UI_ZB_REQ_NOT_ALLOWED,
                                           UI_ZB_REQ_BUSY);
}

static void px_zb_progress(void *ctx, ui_zb_progress_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    memset(out, 0, sizeof(*out));
    if (send(p, UR_ZB_PROGRESS, &w, &r) == UR_OK) {
        ur_get(&r, &ur_t_zb_progress, out);
        if (!r.ok) {
            memset(out, 0, sizeof(*out));
        }
    }
}

static size_t px_zb_codes(void *ctx, ui_zb_code_t *out, size_t max)
{
    return list_of(P(ctx), UR_ZB_CODES, max, &ur_t_zb_code, out);
}

/* ---- sirens, outputs and 433 MHz ---- */

static bool px_out_status(void *ctx, ui_out_status_t *out)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    memset(out, 0, sizeof(*out));
    p->out_where[0] = '\0';
    out->where = p->out_where;
    if (send(p, UR_OUT_STATUS, &w, &r) != UR_OK) {
        return false;
    }
    const bool flag = plink_r_u8(&r) != 0u;
    ur_get(&r, &ur_t_out_status, out);
    ur_get_str(&r, p->out_where, sizeof(p->out_where), true);
    out->where = p->out_where;
    if (!r.ok) {
        memset(out, 0, sizeof(*out));
        out->where = p->out_where;
        p->last = UR_BAD;
        return false;
    }
    return flag;
}

static void fixed_list(ui_remote_proxy_t *p, uint16_t call, const ur_table_t *t, void *out,
                       size_t n)
{
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    memset(out, 0, t->size * n);
    if (send(p, call, &w, &r) == UR_OK) {
        (void)ur_get_list(&r, t, out, n);
    }
}

static void px_out_wired(void *ctx, ui_out_wired_t out[UI_OUT_WIRED_MAX])
{
    fixed_list(P(ctx), UR_OUT_WIRED, &ur_t_out_wired, out, UI_OUT_WIRED_MAX);
}

static void px_out_rf(void *ctx, ui_out_rf_t out[UI_OUT_RF_MAX])
{
    fixed_list(P(ctx), UR_OUT_RF, &ur_t_out_rf, out, UI_OUT_RF_MAX);
}

static uint8_t px_out_wired_set(void *ctx, uint8_t slot, const ui_out_wired_t *c)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    plink_w_u8(&w, slot);
    ur_put(&w, &ur_t_out_wired, c);
    return byte_answer(p, UR_OUT_WIRED_SET, &w, UI_OUT_NOT_ALLOWED, UI_OUT_BUSY);
}

static uint8_t px_out_rf_set(void *ctx, uint8_t slot, const ui_out_rf_t *c)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    plink_w_u8(&w, slot);
    ur_put(&w, &ur_t_out_rf, c);
    return byte_answer(p, UR_OUT_RF_SET, &w, UI_OUT_NOT_ALLOWED, UI_OUT_BUSY);
}

static uint8_t out_slot(void *ctx, uint16_t call, uint8_t slot)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    begin(p, &w);
    plink_w_u8(&w, slot);
    return byte_answer(p, call, &w, UI_OUT_NOT_ALLOWED, UI_OUT_BUSY);
}

static uint8_t px_out_wired_test(void *ctx, uint8_t slot)
{
    return out_slot(ctx, UR_OUT_WIRED_TEST, slot);
}
static uint8_t px_out_rf_clear(void *ctx, uint8_t slot)
{
    return out_slot(ctx, UR_OUT_RF_CLEAR, slot);
}
static uint8_t px_out_rf_pair(void *ctx, uint8_t slot)
{
    return out_slot(ctx, UR_OUT_RF_PAIR, slot);
}
static uint8_t px_out_rf_test(void *ctx, uint8_t slot)
{
    return out_slot(ctx, UR_OUT_RF_TEST, slot);
}

static size_t px_out_terminals(void *ctx, uint8_t slot, uint8_t *out, size_t max)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    plink_w_u8(&w, slot);
    plink_w_u16(&w, (uint16_t)(max > 0xFFFFu ? 0xFFFFu : max));
    if (send(p, UR_OUT_TERMINALS, &w, &r) != UR_OK) {
        return 0;
    }
    const size_t n = plink_r_u16(&r);
    const uint8_t *b = plink_r_take(&r, n);
    if (b == NULL || n > max) {
        return 0;
    }
    memcpy(out, b, n);
    return n;
}

static uint8_t px_out_rf_add(void *ctx, uint8_t *slot)
{
    ui_remote_proxy_t *p = P(ctx);
    plink_w_t w;
    plink_r_t r;
    begin(p, &w);
    if (slot != NULL) {
        *slot = 0;
    }
    const ui_remote_status_t st = send(p, UR_OUT_RF_ADD, &w, &r);
    if (st != UR_OK) {
        return st == UR_NOT_AUTHORIZED || st == UR_NEED_INSTALLER ? UI_OUT_NOT_ALLOWED : UI_OUT_BUSY;
    }
    const uint8_t res = plink_r_u8(&r), s = plink_r_u8(&r);
    if (!r.ok) {
        return UI_OUT_BUSY;
    }
    if (slot != NULL) {
        *slot = s;
    }
    return res;
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
    if (areas & UR_AREA_ZWAVE) {
        be->zw_status = px_zw_status;
        be->zw_devices = px_zw_devices;
        be->zw_device = px_zw_device;
        be->zw_request = px_zw_request;
        be->zw_progress = px_zw_progress;
        be->zw_prov = px_zw_prov;
        be->zw_prov_register = px_zw_prov_register;
        be->zw_prov_change = px_zw_prov_change;
        be->zw_radio_sd_install = px_zw_radio_sd_install;
        be->zw_radio_sd_state = px_zw_radio_sd_state;
    }
    if (areas & UR_AREA_ZIGBEE) {
        be->zb_status = px_zb_status;
        be->zb_devices = px_zb_devices;
        be->zb_device = px_zb_device;
        be->zb_request = px_zb_request;
        be->zb_progress = px_zb_progress;
        be->zb_codes = px_zb_codes;
    }
    if (areas & UR_AREA_OUTPUTS) {
        be->out_status = px_out_status;
        be->out_wired = px_out_wired;
        be->out_wired_set = px_out_wired_set;
        be->out_wired_test = px_out_wired_test;
        be->out_terminals = px_out_terminals;
        be->out_rf = px_out_rf;
        be->out_rf_add = px_out_rf_add;
        be->out_rf_set = px_out_rf_set;
        be->out_rf_clear = px_out_rf_clear;
        be->out_rf_pair = px_out_rf_pair;
        be->out_rf_test = px_out_rf_test;
    }
}
