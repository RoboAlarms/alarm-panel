/*
 * plink_backend.c - the touch UI's side of the panel link (ui_backend.h "The panel link"): the four
 * link entries for either role and, on a keypad, the entries the link serves instead of the engine
 * (docs/features/11 "Roles on a P4 panel"). Every entry runs on the LVGL task and reads what the
 * link task published (link_pub_lock) or hands it a request; none waits longer than the link's
 * own answer (2 s for a request, 3 s for a command). The keypad-update fields (docs/features/11
 * "Updates through the link", What people see) come from plink_service_fw and
 * plink_service_fw_keypad: copies under the same lock, taken one after the other, never nested.
 *
 * A keypad keeps what ui_port_esp gives it for this panel itself - Wi-Fi and addresses, the screen,
 * sound, languages and packs, updates, About, the setup and the factory reset (which makes it a
 * stand-alone panel again), a change of its Screen and sound settings from elsewhere (the
 * prefs_remote_* pair: its master's PREFS_SET, handed to the screen by the link task through
 * panel_prefs' mailbox, UI-046) and Smart Home's Arrange (its own layout, decision 2 of BL-216) -
 * and reads the time zone without setting it (the master's, LINK-020). Smart Home's devices and
 * actions are the master's, through the link (BL-216).
 * The master-only pages are left out by NULL entries; the ones the UI calls without looking
 * (installer mode, users) answer ALARM_ERR_UNAVAILABLE until the role gate hides them.
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK && CONFIG_ALARM_FEATURE_UI

#include <stdio.h>
#include <string.h>

/* ---- the four link entries ------------------------------------------------------------------- */

static bool link_info(void *ctx, ui_link_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    bool known = true;
    link_pub_lock();
    if (plink_service_role() == PLINK_SERVICE_KEYPAD) {
        known = g_pub.kinfo_valid;
        if (known) {
            *out = g_pub.kinfo;
        }
    } else {
        out->role = UI_LINK_ROLE_MASTER;
        out->keypads = (uint8_t)g_pub.keypads;
        out->keypads_down = g_pub.keypads_down;
        if (g_pub.setup_keypad && g_pub.setup_paired) {
            /* the setup's pairing as a keypad (Option C): what it was given, for its last step
             * (the home partition by name where it is known: this panel's own configuration is
             * not that master's) */
            out->device = g_pub.setup_device;
            out->home = g_pub.setup_home;
            snprintf(out->master_name, sizeof out->master_name, "%s", g_pub.setup_master);
            snprintf(out->master_addr, sizeof out->master_addr, "%s", g_pub.setup_master_addr);
            snprintf(out->home_name, sizeof out->home_name, "%s", g_pub.setup_home_name);
        }
    }
    link_pub_unlock();
    if (known && out->role == UI_LINK_ROLE_KEYPAD) {
        /* its updates from its master: the page's "Updates come from <master>", the banner's
         * "restarts with <master>", and "Receiving from <master>" while the master relays it */
        plink_service_fw_keypad_t fk;
        if (plink_service_fw_keypad(&fk)) {
            out->fw_follows = fk.follows;
            out->fw_from_master = fk.follows && fk.state == PLINK_SERVICE_FWK_RECEIVING;
            if (out->master_name[0] == '\0' && fk.master[0] != '\0') {
                snprintf(out->master_name, sizeof out->master_name, "%s", fk.master);
            }
        }
    }
    return known;
}

static bool link_pair_status(void *ctx, ui_link_pair_t *out)
{
    (void)ctx;
    link_pub_lock();
    const bool valid = g_pub.pair_valid;
    *out = g_pub.pair;
    link_pub_unlock();
    return valid;
}

/* A keypad's line on the master's Software update: the planner's row (the same numbers, mapped
 * by name all the same). */
static uint8_t fw_state_of(uint8_t row)
{
    switch (row) {
    case PLINK_SERVICE_FWR_UP_TO_DATE: return UI_LINK_FW_UP_TO_DATE;
    case PLINK_SERVICE_FWR_DOWNLOADING: return UI_LINK_FW_DOWNLOADING;
    case PLINK_SERVICE_FWR_RECEIVING: return UI_LINK_FW_RECEIVING;
    case PLINK_SERVICE_FWR_READY: return UI_LINK_FW_READY;
    case PLINK_SERVICE_FWR_RESTARTING: return UI_LINK_FW_RESTARTING;
    case PLINK_SERVICE_FWR_NOT_UPDATED: return UI_LINK_FW_NOT_UPDATED;
    case PLINK_SERVICE_FWR_NEWER: return UI_LINK_FW_NEWER;
    default: return UI_LINK_FW_NONE;
    }
}

static size_t link_keypads(void *ctx, ui_link_keypad_t *out, size_t max)
{
    (void)ctx;
    if (plink_service_role() != PLINK_SERVICE_MASTER) {
        return 0u;
    }
    link_pub_lock();
    const size_t n = g_pub.keypads < max ? g_pub.keypads : max;
    memcpy(out, g_pub.keypad, n * sizeof *out);
    link_pub_unlock();
    /* each keypad's part in this panel's updates (the LVGL task's stack: 8 short rows) */
    plink_service_fw_kp_t rows[PLINK_KEYPADS_MAX];
    const size_t nr = plink_service_fw(NULL, rows, PLINK_KEYPADS_MAX);
    for (size_t i = 0; i < n; i++) {
        ui_link_keypad_t *k = &out[i];
        for (size_t j = 0; j < nr; j++) {
            const plink_service_fw_kp_t *r = &rows[j];
            if (r->device != k->device) {
                continue;
            }
            if (r->version[0] != '\0') {
                snprintf(k->fw, sizeof k->fw, "%s", r->version); /* its FW_INFO's, once known */
            }
            k->fw_state = fw_state_of(r->state);
            k->fw_percent = r->percent > 100u ? 100u : r->percent;
            if (r->busy && !k->connected) {
                /* installing or on its self-test, its supervision held: Installer > Keypads and
                 * devices says "Updating" for "Not responding" */
                k->fw_state = UI_LINK_FW_RESTARTING;
            }
            break;
        }
    }
    return n;
}

static uint8_t link_req(void *ctx, const ui_link_req_t *req)
{
    (void)ctx;
    if (req == NULL || req->kind < UI_LINK_REQ_PAIR_OPEN || req->kind > UI_LINK_REQ_KP_DONE) {
        return UI_LINK_REQ_REFUSED;
    }
    link_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = req->kind;
    r.allow = req->allow;
    r.device = req->device;
    r.home = req->home;
    r.supervision_s = req->supervision_s;
    memcpy(r.name, req->name, sizeof r.name);
    r.name[sizeof r.name - 1u] = '\0';
    return link_request(&r);
}

/* ---- a keypad: the master's status, configuration and events --------------------------------- */

static uint32_t kp_snapshot_seq(void *ctx)
{
    (void)ctx;
    return ksvc_ui_seq();
}

static void kp_get_snapshot(void *ctx, alarm_snapshot_t *out)
{
    (void)ctx;
    ksvc_ui_snapshot(out);
}

static bool kp_get_config(void *ctx, alarm_config_t *out)
{
    (void)ctx;
    return ksvc_ui_config(out);
}

static size_t kp_recent_events(void *ctx, alarm_event_t *out, size_t max)
{
    (void)ctx;
    return ksvc_ui_events(out, max);
}

/* ---- a keypad: commands and codes, run by the master as this keypad (LINK-002) --------------- */

static uint8_t kp_home(void)
{
    link_pub_lock();
    const uint8_t home = g_pub.kinfo.home;
    link_pub_unlock();
    return home >= 1u && home <= ALARM_MAX_PARTITIONS ? home : 1u;
}

static void put_code(char *dst, size_t cap, const char *code)
{
    memset(dst, 0, cap);
    if (code != NULL) {
        snprintf(dst, cap, "%s", code);
    }
}

static alarm_result_t kp_command(void *ctx, const alarm_cmd_t *cmd, alarm_cmd_result_t *res)
{
    (void)ctx;
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = PLINK_OP_COMMAND;
    c.cmd = *cmd;
    plink_result_t out;
    const alarm_result_t r = ksvc_command(&c, &out);
    plink_wipe(&c, sizeof c);
    if (res != NULL) {
        memset(res, 0, sizeof *res);
        res->user = out.user;
        res->not_ready_zone = out.not_ready_zone;
        res->may_disarm = out.may_disarm != 0u;
    }
    return r;
}

static alarm_result_t kp_authenticate(void *ctx, const char *code, uint8_t *user_out)
{
    (void)ctx;
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = PLINK_OP_AUTH;
    c.cmd.partition = kp_home(); /* LINK-027: this keypad's partition only */
    put_code(c.cmd.code, sizeof c.cmd.code, code);
    plink_result_t out;
    const alarm_result_t r = ksvc_command(&c, &out);
    plink_wipe(&c, sizeof c);
    if (user_out != NULL) {
        *user_out = r == ALARM_OK ? out.user : 0u;
    }
    return r;
}

static alarm_result_t kp_change_code(void *ctx, const char *current, const char *new_code,
                                     uint8_t *user_out)
{
    (void)ctx;
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = PLINK_OP_CHANGE_CODE;
    c.cmd.partition = kp_home();
    put_code(c.cmd.code, sizeof c.cmd.code, current);
    put_code(c.new_code, sizeof c.new_code, new_code);
    plink_result_t out;
    const alarm_result_t r = ksvc_command(&c, &out);
    plink_wipe(&c, sizeof c);
    if (user_out != NULL) {
        *user_out = out.user;
    }
    return r;
}

/* ---- a keypad: the master's reporting for the Emergency words ---------------------------------
 * Only mqtt_status is filled, from the master's report byte, so report_state() (ui_app.c) says what
 * happens to an alarm keyed here; mqtt_get, mqtt_set and every halink_* stay NULL, which keeps the
 * Integrations rows hidden (ui_integrations.c wants all three MQTT entries). */
static void kp_mqtt_status(void *ctx, ui_mqtt_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    link_pub_lock();
    const uint8_t report = g_pub.kinfo_valid ? g_pub.kreport : (uint8_t)PLINK_REPORT_NONE;
    link_pub_unlock();
    out->state = report == PLINK_REPORT_ONLINE    ? UI_MQTT_CONNECTED
                 : report == PLINK_REPORT_OFFLINE ? UI_MQTT_CONNECTING
                                                  : UI_MQTT_OFF;
}

/* ---- a keypad: the master's devices on Smart Home (BL-216) -----------------------------------
 * The master's page, unchanged: its devices and states from the link's mirror (the screen's copy
 * the link task keeps), each tap judged again on the master as its own screen's (LINK-047,
 * LINK-048). Arrange stays this panel's own (ui_port_esp's layout entries, decision 2). */

#if CONFIG_ALARM_HOME_CONTROL
static size_t kp_dev_list(void *ctx, ui_dev_t *out, size_t max)
{
    (void)ctx;
    return ksvc_dev_copy(out, max, NULL);
}

static ui_dev_result_t kp_dev_act(void *ctx, const char *ref, uint8_t action, int32_t value,
                                  const char *code, alarm_result_t *code_result)
{
    (void)ctx;
    return ksvc_dev_act(ref, action, value, code, code_result);
}
#endif

/* ---- a keypad: what only the master does (until the role gate hides it) ---------------------- */

static alarm_result_t kp_program_enter(void *ctx, const char *code, uint8_t *user_out)
{
    (void)ctx;
    (void)code;
    if (user_out != NULL) {
        *user_out = 0u;
    }
    return ALARM_ERR_UNAVAILABLE;
}

static void kp_program_nothing(void *ctx)
{
    (void)ctx;
}

static alarm_result_t kp_program_set_code(void *ctx, uint8_t user, const char *new_code)
{
    (void)ctx;
    (void)user;
    (void)new_code;
    return ALARM_ERR_UNAVAILABLE;
}

static alarm_result_t kp_user_set(void *ctx, const char *actor, uint8_t user, const char *code,
                                  alarm_authority_t authority, uint8_t partitions, const char *name)
{
    (void)ctx;
    (void)actor;
    (void)user;
    (void)code;
    (void)authority;
    (void)partitions;
    (void)name;
    return ALARM_ERR_UNAVAILABLE; /* user editing never goes over the link (docs/features/11) */
}

static alarm_result_t kp_user_update(void *ctx, const char *actor, uint8_t user,
                                     alarm_authority_t authority, uint8_t partitions,
                                     const char *name)
{
    (void)ctx;
    (void)actor;
    (void)user;
    (void)authority;
    (void)partitions;
    (void)name;
    return ALARM_ERR_UNAVAILABLE;
}

static alarm_result_t kp_user_delete(void *ctx, const char *actor, uint8_t user)
{
    (void)ctx;
    (void)actor;
    (void)user;
    return ALARM_ERR_UNAVAILABLE;
}

static void keypad_backend(ui_backend_t *be)
{
    be->snapshot_seq = kp_snapshot_seq;
    be->get_snapshot = kp_get_snapshot;
    be->get_config = kp_get_config;
    be->recent_events = kp_recent_events;
    be->command = kp_command;
    be->authenticate = kp_authenticate;
    be->change_code = kp_change_code;
    be->program_enter = kp_program_enter;
    be->program_exit = kp_program_nothing;
    be->program_keepalive = kp_program_nothing;
    be->program_set_code = kp_program_set_code;
    be->user_set = kp_user_set;
    be->user_update = kp_user_update;
    be->user_delete = kp_user_delete;
    be->set_config = NULL;
    /* the master's: its event log, sensors, integrations, automations, the zones' spoken names
     * (the keypad's own voice, its Announcements and Voice volume stay: LINK-028); its devices
     * come through the link (below) */
    be->log_read = NULL;
    be->boot_id = NULL;
    be->log_loss = NULL;
    be->log_clear_loss = NULL;
    be->zone_sources = NULL;
    be->zone_learn = NULL;
    be->zone_candidates = NULL;
    be->zone_bind = NULL;
    be->zone_unbind = NULL;
    be->zone_info = NULL;
    be->mqtt_get = NULL;
    be->mqtt_set = NULL;
    be->mqtt_status = kp_mqtt_status; /* the master's reporting, nothing to set */
    be->halink_status = NULL;
    be->halink_enable = NULL;
    be->halink_pair_begin = NULL;
    be->halink_pair_decide = NULL;
    be->halink_pair_cancel = NULL;
    be->halink_unpair = NULL;
    be->notify_status = NULL;
    be->notify_test = NULL;
    be->notify_test_result = NULL;
    be->ntfy_get = NULL;
    be->ntfy_set = NULL;
    be->ntfy_new_topic = NULL;
    be->pushover_get = NULL;
    be->pushover_set = NULL;
#if CONFIG_ALARM_HOME_CONTROL
    /* Smart Home from the master through the link (BL-216); the layout entries stay ui_port_esp's:
     * Arrange is each panel's own (decision 2) */
    be->dev_list = kp_dev_list;
    be->dev_act = kp_dev_act;
#else
    be->dev_list = NULL;
    be->dev_act = NULL;
#endif
    be->auto_list = NULL;
    be->auto_gen = NULL;
    be->auto_read_only = NULL;
    be->auto_less_secure = NULL;
    be->auto_save = NULL;
    be->auto_delete_slot = NULL;
    be->auto_set_off = NULL;
    be->auto_status = NULL;
    be->auto_try = NULL;
    be->auto_warnings = NULL;
    be->auto_put_off = NULL;
    be->zw_status = NULL;
    be->zw_devices = NULL;
    be->zw_device = NULL;
    be->zw_request = NULL;
    be->zw_progress = NULL;
    be->zw_prov = NULL;
    be->zw_prov_register = NULL;
    be->zw_prov_change = NULL;
    be->zb_status = NULL;
    be->zb_devices = NULL;
    be->zb_device = NULL;
    be->zb_request = NULL;
    be->zb_progress = NULL;
    be->zb_codes = NULL;
    /* the voice: voice_info, the pack's lease, Preview and its stop are this keypad's own
     * (ui_port_esp's, kept); approving a zone's words is the installer's at the alarm panel, so
     * the spoken-name editor isn't offered here (ui_voice_editor_present) */
    be->voice_seen = NULL;
    be->voice_apply_approve = NULL;
    be->remote_get = NULL;
    be->remote_set = NULL;
    be->remote_info = NULL;
    be->remote_pause = NULL;
    be->web_login_get = NULL;
    be->web_login_set = NULL;
    be->web_login_clear = NULL;
    /* the time is the master's (it runs no time server and has no clock module of its own) */
    be->set_timezone = NULL;
    be->get_ntp = NULL;
    be->set_ntp = NULL;
    be->clock_info = NULL;
    be->clock_set = NULL;
    /* no card here: the storage page goes with the master's pages */
    be->storage_sd = NULL;
    be->storage_flash = NULL;
    be->storage_action = NULL;
}

/* A master keeps every entry of its own; during its setup's pairing as a keypad (Option C) the
 * last step reads what it needs from link_info (never the other master's configuration). */
void plink_service_extend_backend(ui_backend_t *be)
{
    if (plink_service_role() == PLINK_SERVICE_KEYPAD) {
        keypad_backend(be);
    }
    be->link_info = link_info;
    be->link_pair_status = link_pair_status;
    be->link_keypads = link_keypads;
    be->link_request = link_req;
}

#endif /* CONFIG_ALARM_LINK && CONFIG_ALARM_FEATURE_UI */
