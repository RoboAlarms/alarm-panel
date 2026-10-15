/*
 * web_zwave.c - the Panel Manager's Z-Wave page (docs/features/36 screens.md §11, ZW-024): the
 * radio, the device table and SmartStart's list read through the touch UI's own calls, and the
 * list changed through zwave_service with the Panel Manager's own decision (panel_web's access
 * table: a signed-in installer, the panel not armed). A registered sensor is named by an `id`,
 * its key's bytes 2-15 in hex: the first two bytes are its PIN, which never leaves the panel.
 */
#include "web_zwave.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "web_mem.h"
#include "web_rules.h"

#if CONFIG_ALARM_ZSRC_ZWAVE
#include "zwave_service.h"

static const char *radio_word(uint8_t r)
{
    static const char *const W[] = {"not_connected", "starting", "working",  "not_answering",
                                    "unusable",      "foreign",  "replaced", "updating"};
    return r < sizeof(W) / sizeof(W[0]) ? W[r] : "unknown";
}

static const char *prov_state_word(uint8_t s)
{
    static const char *const W[] = {"waiting", "joined", "stopped", "off"};
    return s < sizeof(W) / sizeof(W[0]) ? W[s] : "waiting";
}

static const char *prov_result_word(ui_zw_prov_result_t r)
{
    switch (r) {
    case UI_ZW_PROV_ADDED: return "added";
    case UI_ZW_PROV_UPDATED: return "updated";
    case UI_ZW_PROV_JOINED_ALREADY: return "joined_already";
    case UI_ZW_PROV_FULL: return "full";
    case UI_ZW_PROV_BAD_CODE: return "bad_code";
    case UI_ZW_PROV_CHECKSUM: return "checksum";
    case UI_ZW_PROV_S2_ONLY: return "s2_only";
    case UI_ZW_PROV_NOT_ALLOWED: return "not_allowed";
    default: return "not_found";
    }
}

/* Bytes 2-15 of a key in hex: 28 characters. */
static void key_id(const uint8_t dsk[16], char out[29])
{
    for (size_t i = 0; i < 14u; i++) {
        snprintf(&out[i * 2u], 3u, "%02x", (unsigned)dsk[2u + i]);
    }
}

static void key_words(const uint8_t dsk[16], char *out, size_t cap)
{
    size_t at = (size_t)snprintf(out, cap, "xxxxx");
    for (unsigned g = 1u; g < 8u && at < cap; g++) {
        at += (size_t)snprintf(&out[at], cap - at, "-%05u",
                               (unsigned)dsk[g * 2u] << 8 | (unsigned)dsk[g * 2u + 1u]);
    }
}

static const char *op_word(uint8_t op)
{
    static const char *const W[] = {"none",     "add",     "remove", "remove_failed", "replace",
                                    "ping",     "identify", "routes", "check_in",      "region",
                                    "reset",    "restart", "firmware", "radio_update"};
    return op < sizeof(W) / sizeof(W[0]) ? W[op] : "none";
}

static const char *step_word(uint8_t s)
{
    static const char *const W[] = {"idle",    "starting", "listening", "connecting", "ask_allow",
                                    "ask_pin", "securing", "reading",   "working",    "done"};
    return s < sizeof(W) / sizeof(W[0]) ? W[s] : "idle";
}

static const char *result_word(uint8_t r)
{
    static const char *const W[] = {
        "none",        "added",          "added_insecure", "downgraded",      "no_reports",
        "nothing_joined", "join_failed", "already_added",  "controller",      "stopped",
        "removed",     "removed_foreign", "nothing_removed", "still_answers",  "answered",
        "no_answer",   "done",           "failed",         "setup_paused",    "timed_out",
        "refused",     "fw_updated",     "fw_failed"};
    return r < sizeof(W) / sizeof(W[0]) ? W[r] : "none";
}

static const char *s2_word(uint8_t f)
{
    static const char *const W[] = {"ok",     "wrong_pin", "timeout", "cancelled",
                                    "device_stopped", "failed", "no_info"};
    return f < sizeof(W) / sizeof(W[0]) ? W[f] : "failed";
}

static const char *req_word(ui_zw_req_result_t r)
{
    switch (r) {
    case UI_ZW_REQ_OK: return "ok";
    case UI_ZW_REQ_NOT_ALLOWED: return "not_allowed";
    case UI_ZW_REQ_BUSY: return "busy";
    case UI_ZW_REQ_NO_RADIO: return "no_radio";
    case UI_ZW_REQ_UNSUPPORTED: return "unsupported";
    case UI_ZW_REQ_LOW_BATTERY: return "low_battery";
    case UI_ZW_REQ_FULL: return "full"; /* ZW-039: as Zigbee's */
    default: return "invalid";
    }
}

/* The operation running (or ended, until dismissed): what the touchscreen's add and remove pages
 * show. The PIN is never here: `dsk` is the key as the touchscreen prints it. */
/* A builder's document: o, or NULL when one of its parts could not be made or attached (ok
 * false: web_attach(), code review M01), never a partial answer. */
static cJSON *built(cJSON *o, bool ok)
{
    if (ok) {
        return o;
    }
    cJSON_Delete(o);
    return NULL;
}

static bool add_progress(cJSON *o, const ui_backend_t *b)
{
    ui_zw_progress_t g;
    memset(&g, 0, sizeof(g));
    if (b->zw_progress != NULL) {
        b->zw_progress(b->ctx, &g);
    }
    cJSON *p = cJSON_AddObjectToObject(o, "progress");
    bool ok = p != NULL;
    ok &= cJSON_AddStringToObject(p, "op", op_word(g.op)) != NULL;
    ok &= cJSON_AddStringToObject(p, "step", step_word(g.step)) != NULL;
    ok &= cJSON_AddStringToObject(p, "result", result_word(g.result)) != NULL;
    ok &= cJSON_AddNumberToObject(p, "seconds_left", g.seconds_left) != NULL;
    ok &= cJSON_AddNumberToObject(p, "requested", g.requested) != NULL;
    ok &= cJSON_AddBoolToObject(p, "csa", g.csa) != NULL;
    g.dsk[sizeof(g.dsk) - 1u] = '\0';
    ok &= cJSON_AddStringToObject(p, "dsk", g.dsk) != NULL;
    ok &= cJSON_AddNumberToObject(p, "node", g.node) != NULL;
    ok &= cJSON_AddNumberToObject(p, "generation", g.generation) != NULL;
    ok &= cJSON_AddNumberToObject(p, "security", g.security) != NULL;
    ok &= cJSON_AddStringToObject(p, "s2_failure", s2_word(g.s2_failure)) != NULL;
    ok &= cJSON_AddNumberToObject(p, "setup_done", g.setup_done) != NULL;
    ok &= cJSON_AddNumberToObject(p, "setup_total", g.setup_total) != NULL;
    ok &= cJSON_AddBoolToObject(p, "asleep", g.asleep) != NULL;
    ok &= cJSON_AddBoolToObject(p, "stalled", g.stalled) != NULL;
    g.model[sizeof(g.model) - 1u] = '\0';
    ok &= cJSON_AddStringToObject(p, "model", g.model) != NULL;
    cJSON *z = cJSON_AddArrayToObject(p, "zones");
    ok &= z != NULL;
    for (size_t k = 0; k < g.zone_count && k < UI_ZW_ZONES_MAX; k++) {
        ok &= web_attach(z, NULL, cJSON_CreateNumber(g.zones[k]));
    }
    ok &= cJSON_AddBoolToObject(p, "replacing", g.replacing) != NULL;
    ok &= cJSON_AddNumberToObject(p, "routes_done", g.routes_done) != NULL;
    ok &= cJSON_AddNumberToObject(p, "routes_total", g.routes_total) != NULL;
    ok &= cJSON_AddNumberToObject(p, "routes_failed", g.routes_failed) != NULL;
    ok &= cJSON_AddNumberToObject(p, "routes_skipped", g.routes_skipped) != NULL;
    ok &= cJSON_AddNumberToObject(p, "routes_waiting", g.routes_waiting) != NULL;
    ok &= cJSON_AddBoolToObject(p, "routes_stopped", g.routes_stopped) != NULL;
    cJSON *rl = cJSON_AddArrayToObject(p, "routes_list"); /* BL-152: who needs a look */
    ok &= rl != NULL;
    for (size_t k = 0; k < g.routes_list_n && k < UI_ZW_ROUTES_LIST; k++) {
        static const char *const W[] = {"waiting", "failed", "skipped", "no_route"};
        cJSON *o = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(o, "node", g.routes_list[k].node) != NULL;
        ok &= cJSON_AddStringToObject(o, "what", W[g.routes_list[k].what & 3u]) != NULL;
        ok &= web_attach(rl, NULL, o);
    }
    ok &= cJSON_AddNumberToObject(p, "fw_done", g.fw_done) != NULL;
    ok &= cJSON_AddNumberToObject(p, "fw_total", g.fw_total) != NULL;
    ok &= cJSON_AddNumberToObject(p, "fw_why", g.fw_why) != NULL;
    ok &= cJSON_AddNumberToObject(p, "fw_error", g.fw_error) != NULL;
    return ok;
}

static const char *fw_check_word(uint8_t c)
{
    static const char *const W[] = {"none",      "checking",     "up_to_date",  "unknown",
                                    "available", "check_failed", "downloading", "file_failed"};
    return c < sizeof(W) / sizeof(W[0]) ? W[c] : "none";
}

/* The Radio page's facts (screens.md §11): what the touchscreen leaves out. */
static bool add_diag(cJSON *o)
{
    zwave_diag_t *d = web_alloc(sizeof(*d));
    if (d == NULL) {
        return false;
    }
    if (!zwave_service_diag(d)) {
        free(d);
        return true; /* the page's facts are optional: no diag, not a memory error */
    }
    cJSON *g = cJSON_AddObjectToObject(o, "diag");
    bool ok = g != NULL;
    char v[32];
    snprintf(v, sizeof(v), "%08" PRIX32, d->home_id);
    ok &= cJSON_AddStringToObject(g, "home_id", v) != NULL;
    ok &= cJSON_AddNumberToObject(g, "node_id", d->node_id) != NULL;
    ok &= cJSON_AddNumberToObject(g, "suc_node_id", d->suc_node_id) != NULL;
    snprintf(v, sizeof(v), "%02X v%u", (unsigned)d->chip_type, (unsigned)d->chip_version);
    ok &= cJSON_AddStringToObject(g, "chip", v) != NULL;
    ok &= cJSON_AddStringToObject(g, "library", d->library) != NULL;
    ok &= cJSON_AddNumberToObject(g, "library_type", d->library_type) != NULL;
    snprintf(v, sizeof(v), "%04X:%04X:%04X", (unsigned)d->manufacturer, (unsigned)d->product_type,
             (unsigned)d->product_id);
    ok &= cJSON_AddStringToObject(g, "product", v) != NULL;
    if (d->noise_known) {
        cJSON *n = cJSON_AddArrayToObject(g, "noise_dbm");
        ok &= n != NULL;
        for (uint8_t i = 0; i < d->channels; i++) {
            ok &= web_attach(n, NULL, cJSON_CreateNumber(d->noise[i]));
        }
    }
    ok &= cJSON_AddNumberToObject(g, "jam_threshold_dbm", d->jam_threshold) != NULL;
    if (d->power_known) {
        ok &= cJSON_AddNumberToObject(g, "power_dbm", d->power_dbm10 / 10.0) != NULL;
    }
    if (d->lr_power_known) {
        ok &= cJSON_AddNumberToObject(g, "lr_power_dbm", d->lr_power_dbm10 / 10.0) != NULL;
    }
    if (d->power_cap_set) {
        ok &= cJSON_AddNumberToObject(g, "power_came_with_dbm", d->power_cap_dbm10 / 10.0) != NULL;
    }
    ok &= cJSON_AddNumberToObject(g, "link_ups", d->link_ups) != NULL;
    ok &= cJSON_AddNumberToObject(g, "link_downs", d->link_downs) != NULL;
    ok &= cJSON_AddNumberToObject(g, "restarts", d->restarts) != NULL;
    ok &= cJSON_AddNumberToObject(g, "recoveries", d->recoveries) != NULL;
    ok &= cJSON_AddNumberToObject(g, "foreign", d->foreign) != NULL;
    ok &= cJSON_AddNumberToObject(g, "malformed", d->malformed) != NULL;
    cJSON *nodes = cJSON_AddArrayToObject(g, "nodes");
    ok &= nodes != NULL;
    for (uint8_t i = 0; i < d->node_count; i++) {
        cJSON *e = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(e, "node", d->nodes[i].node) != NULL;
        ok &= cJSON_AddNumberToObject(e, "wrong_level", d->nodes[i].wrong_level) != NULL;
        ok &= cJSON_AddNumberToObject(e, "decrypt_failures", d->nodes[i].decrypt_failures) != NULL;
        ok &= cJSON_AddNumberToObject(e, "duplicates", d->nodes[i].duplicates) != NULL;
        ok &= web_attach(nodes, NULL, e);
    }
    free(d);
    return ok;
}

cJSON *web_zwave_get(const ui_backend_t *b)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    ui_zw_status_t st;
    memset(&st, 0, sizeof(st));
    const bool on = b->zw_status != NULL && b->zw_status(b->ctx, &st);
    bool ok = cJSON_AddBoolToObject(o, "available", on) != NULL;
    if (!on) {
        return built(o, ok);
    }
    cJSON *radio = cJSON_AddObjectToObject(o, "radio");
    ok &= radio != NULL;
    ok &= cJSON_AddStringToObject(radio, "state", radio_word(st.radio)) != NULL;
    ok &= cJSON_AddBoolToObject(radio, "region_known", st.region_known) != NULL;
    ok &= cJSON_AddNumberToObject(radio, "region", st.region) != NULL;
    ok &= cJSON_AddNumberToObject(radio, "devices", st.devices) != NULL;
    ok &= cJSON_AddNumberToObject(radio, "not_heard", st.not_heard) != NULL;
    ok &= cJSON_AddNumberToObject(radio, "joining", st.joining) != NULL;
    char v[24];
    snprintf(v, sizeof(v), "%u.%u", (unsigned)st.firmware_major, (unsigned)st.firmware_minor);
    ok &= cJSON_AddStringToObject(radio, "firmware", v) != NULL;
    snprintf(v, sizeof(v), "%u.%u.%u", (unsigned)st.sdk_major, (unsigned)st.sdk_minor,
             (unsigned)st.sdk_patch);
    ok &= cJSON_AddStringToObject(radio, "sdk", v) != NULL;
    ok &= cJSON_AddBoolToObject(radio, "update_advised", st.update_advised) != NULL;
    /* network.md §14: its own software's check (the Z-Wave JS update service, asked as Z-Wave JS
     * asks about a controller) */
    cJSON *sw = cJSON_AddObjectToObject(radio, "software");
    ok &= sw != NULL;
    ok &= cJSON_AddStringToObject(sw, "check", fw_check_word(st.radio_fw_check)) != NULL;
    st.radio_fw_version[sizeof st.radio_fw_version - 1u] = '\0';
    st.radio_fw_words[sizeof st.radio_fw_words - 1u] = '\0';
    ok &= cJSON_AddStringToObject(sw, "version", st.radio_fw_version) != NULL;
    ok &= cJSON_AddStringToObject(sw, "words", st.radio_fw_words) != NULL;
    ok &= cJSON_AddBoolToObject(radio, "jammed", st.jammed) != NULL; /* network.md §9 */
    ok &= cJSON_AddBoolToObject(radio, "storage_full", st.storage_full) != NULL; /* ZW-039: as Zigbee's */
    if (st.noise_known) {
        ok &= cJSON_AddNumberToObject(radio, "noise_dbm", st.noise_dbm) != NULL;
    }

    cJSON *devs = cJSON_AddArrayToObject(o, "devices");
    ok &= devs != NULL;
    ui_zw_device_t *d = web_alloc(UI_ZW_DEVICES_MAX * sizeof(*d));
    ok &= d != NULL; /* no memory is "Memory busy", never an empty list (BL-113) */
    const size_t n = d != NULL && b->zw_devices != NULL
                         ? b->zw_devices(b->ctx, d, UI_ZW_DEVICES_MAX)
                         : 0u;
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(e, "node", d[i].node) != NULL;
        ok &= cJSON_AddNumberToObject(e, "generation", d[i].generation) != NULL;
        ok &= cJSON_AddStringToObject(e, "model", d[i].model) != NULL;
        ok &= cJSON_AddBoolToObject(e, "listening", d[i].listening) != NULL;
        if (d[i].heard_s != UI_ZW_NEVER) {
            ok &= cJSON_AddNumberToObject(e, "heard_s", d[i].heard_s) != NULL;
        }
        switch (d[i].signal) { /* ZW-040: left out until a frame was measured */
        case UI_ZW_SIGNAL_GOOD: ok &= cJSON_AddStringToObject(e, "signal", "good") != NULL; break;
        case UI_ZW_SIGNAL_FAIR: ok &= cJSON_AddStringToObject(e, "signal", "fair") != NULL; break;
        case UI_ZW_SIGNAL_WEAK: ok &= cJSON_AddStringToObject(e, "signal", "weak") != NULL; break;
        default: break;
        }
        ok &= cJSON_AddNumberToObject(e, "kind", d[i].kind) != NULL;
        ok &= cJSON_AddNumberToObject(e, "security", d[i].security) != NULL;
        ok &= cJSON_AddNumberToObject(e, "state", d[i].state) != NULL;
        ok &= cJSON_AddNumberToObject(e, "setup_done", d[i].setup_done) != NULL; /* BL-152: all done = a re-read */
        ok &= cJSON_AddNumberToObject(e, "setup_total", d[i].setup_total) != NULL;
        ok &= cJSON_AddBoolToObject(e, "unreachable", d[i].unreachable) != NULL;
        if (d[i].battery_pct <= 100u) {
            ok &= cJSON_AddNumberToObject(e, "battery", d[i].battery_pct) != NULL;
        }
        ok &= cJSON_AddBoolToObject(e, "battery_low", d[i].battery_low) != NULL;
        cJSON *z = cJSON_AddArrayToObject(e, "zones");
        ok &= z != NULL;
        for (size_t k = 0; k < d[i].zone_count && k < UI_ZW_ZONES_MAX; k++) {
            ok &= web_attach(z, NULL, cJSON_CreateNumber(d[i].zones[k]));
        }
        ok &= web_attach(devs, NULL, e);
    }
    free(d);

    cJSON *smart = cJSON_AddArrayToObject(o, "smartstart");
    ok &= smart != NULL;
    ui_zw_prov_t *p = web_alloc(UI_ZW_PROV_MAX * sizeof(*p));
    ok &= p != NULL;
    const size_t m = p != NULL && b->zw_prov != NULL ? b->zw_prov(b->ctx, p, UI_ZW_PROV_MAX) : 0u;
    for (size_t i = 0; i < m; i++) {
        char id[29], key[48];
        key_id(p[i].dsk, id);
        key_words(p[i].dsk, key, sizeof(key));
        cJSON *e = cJSON_CreateObject();
        ok &= cJSON_AddStringToObject(e, "id", id) != NULL;
        ok &= cJSON_AddStringToObject(e, "key", key) != NULL;
        ok &= cJSON_AddStringToObject(e, "name", p[i].name) != NULL;
        ok &= cJSON_AddStringToObject(e, "state", prov_state_word(p[i].state)) != NULL;
        ok &= cJSON_AddNumberToObject(e, "failures", p[i].failures) != NULL;
        ok &= cJSON_AddBoolToObject(e, "long_range", p[i].long_range) != NULL;
        ok &= cJSON_AddNumberToObject(e, "requested", p[i].requested) != NULL;
        ok &= cJSON_AddNumberToObject(e, "grant", p[i].grant) != NULL;
        ok &= cJSON_AddNumberToObject(e, "node", p[i].node) != NULL;
        ok &= cJSON_AddNumberToObject(e, "generic", p[i].generic) != NULL;
        if (p[i].model[0] != '\0') {
            ok &= cJSON_AddStringToObject(e, "model", p[i].model) != NULL; /* ZW-024 */
        }
        if (p[i].manufacturer != 0u || p[i].product_type != 0u || p[i].product_id != 0u) {
            char prod[20];
            snprintf(prod, sizeof(prod), "%04X:%04X:%04X", (unsigned)p[i].manufacturer,
                     (unsigned)p[i].product_type, (unsigned)p[i].product_id);
            ok &= cJSON_AddStringToObject(e, "product", prod) != NULL;
        }
        ok &= web_attach(smart, NULL, e);
    }
    if (p != NULL) {
        memset(p, 0, UI_ZW_PROV_MAX * sizeof(*p)); /* the keys (and their PINs) */
        free(p);
    }
    ok &= add_progress(o, b);
    ok &= add_diag(o);
    return built(o, ok);
}

/* The entry whose key's bytes 2-15 are `id`; false when none. */
static bool find_id(const ui_backend_t *b, const char *id, uint8_t dsk[16])
{
    ui_zw_prov_t *p = web_alloc(UI_ZW_PROV_MAX * sizeof(*p));
    if (p == NULL || b->zw_prov == NULL || strlen(id) != 28u) {
        free(p);
        return false;
    }
    const size_t m = b->zw_prov(b->ctx, p, UI_ZW_PROV_MAX);
    bool found = false;
    for (size_t i = 0; i < m && !found; i++) {
        char mine[29];
        key_id(p[i].dsk, mine);
        if (strcmp(mine, id) == 0) {
            memcpy(dsk, p[i].dsk, 16u);
            found = true;
        }
    }
    memset(p, 0, UI_ZW_PROV_MAX * sizeof(*p));
    free(p);
    return found;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : "";
}

/* A whole number in lo..hi; false when absent or not one. */
static bool jnum(const cJSON *o, const char *key, double lo, double hi, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        v->valuedouble != (double)(uint32_t)v->valuedouble) {
        return false;
    }
    *out = (uint32_t)v->valuedouble;
    return true;
}

/* The touchscreen's own network operations, with the Panel Manager's own decision (the access
 * table's: a signed-in installer, the panel not armed); the region, the network reset, outputs
 * and software updates stay at the panel. NULL: not one of them. */
static const char *operation(const cJSON *body, const char *action, uint8_t user)
{
    static const struct {
        const char *action;
        uint8_t kind;
        bool node;   /* needs `node` and `generation` */
    } OPS[] = {
        {"add", UI_ZW_REQ_ADD, false},
        {"remove", UI_ZW_REQ_REMOVE, false},
        {"keep_listening", UI_ZW_REQ_KEEP_LISTENING, false},
        {"stop", UI_ZW_REQ_STOP, false},
        {"allow", UI_ZW_REQ_ALLOW, false},
        {"pin", UI_ZW_REQ_PIN, false},
        {"insecure", UI_ZW_REQ_INSECURE, false},
        {"dismiss", UI_ZW_REQ_DISMISS, false},
        {"restart", UI_ZW_REQ_RESTART, false},
        {"remove_failed", UI_ZW_REQ_REMOVE_FAILED, true},
        {"ping", UI_ZW_REQ_PING, true},
        {"setup", UI_ZW_REQ_SETUP, true},
        {"routes", UI_ZW_REQ_ROUTES, false},
        {"radio_check", UI_ZW_REQ_FW_CHECK, false},    /* node 0: the radio itself (§14) */
        {"radio_install", UI_ZW_REQ_FW_INSTALL, false},
    };
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (strcmp(action, OPS[i].action) != 0) {
            continue;
        }
        ui_zw_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = OPS[i].kind;
        req.user = user;
        uint32_t v = 0u;
        if (OPS[i].node || OPS[i].kind == UI_ZW_REQ_ROUTES) {
            const bool has = jnum(body, "node", 0, 4000, &v);
            if ((OPS[i].node && (!has || v == 0u)) || (!has && OPS[i].kind != UI_ZW_REQ_ROUTES)) {
                return "bad_request";
            }
            req.node = (uint16_t)v;
            if (req.node != 0u) {
                if (!jnum(body, "generation", 0, 65535, &v)) {
                    return "bad_request";
                }
                req.generation = (uint16_t)v;
            }
        }
        if (OPS[i].kind == UI_ZW_REQ_ALLOW) {
            if (!jnum(body, "keys", 1, 7, &v)) {
                return "bad_request";
            }
            req.keys = (uint8_t)v;
        }
        if (OPS[i].kind == UI_ZW_REQ_PIN) {
            if (!jnum(body, "pin", 0, UI_ZW_PIN_MAX, &v)) {
                return "bad_request";
            }
            req.pin = v;
        }
        const ui_zw_req_result_t r = zwave_service_request(&req, true);
        req.pin = 0u;
        return req_word(r);
    }
    return NULL;
}

const char *web_zwave_radio_file(const uint8_t *data, size_t len, uint8_t user)
{
    return req_word(zwave_service_radio_file(data, len, true, user));
}

const char *web_zwave_post(const ui_backend_t *b, const cJSON *body, uint8_t user)
{
    if (b->zw_status == NULL || b->zw_prov == NULL) {
        return NULL; /* no Z-Wave running */
    }
    const char *action = jstr(body, "action");
    const char *code = jstr(body, "code");
    const char *name = jstr(body, "name");
    const char *id = jstr(body, "id");
    uint32_t keys = 0u;
    (void)jnum(body, "keys", 0, 7, &keys);
    const char *net = operation(body, action, user);
    if (net != NULL) {
        return net;
    }
    if (strcmp(action, "register") == 0) {
        if (strlen(code) > 600u || strlen(name) >= UI_ZW_PROV_NAME) {
            return "bad_code";
        }
        return prov_result_word(zwave_service_prov_register(code, name, true));
    }
    uint8_t dsk[16];
    if (!find_id(b, id, dsk)) {
        return "not_found";
    }
    uint8_t op = 0u;
    if (strcmp(action, "on") == 0) {
        op = UI_ZW_PROV_ON;
    } else if (strcmp(action, "off") == 0) {
        op = UI_ZW_PROV_OFF_OP;
    } else if (strcmp(action, "forget") == 0) {
        op = UI_ZW_PROV_FORGET;
    } else if (strcmp(action, "grant") == 0 && keys > 0u && keys <= 0x07u) {
        op = UI_ZW_PROV_GRANT;
    } else {
        return "bad_request";
    }
    const char *w = prov_result_word(zwave_service_prov_change(dsk, op, (uint8_t)keys, true));
    memset(dsk, 0, sizeof(dsk));
    return w;
}

#else

cJSON *web_zwave_get(const ui_backend_t *b)
{
    (void)b;
    cJSON *o = cJSON_CreateObject();
    if (o != NULL) {
        cJSON_AddBoolToObject(o, "available", false);
    }
    return o;
}

const char *web_zwave_post(const ui_backend_t *b, const cJSON *body, uint8_t user)
{
    (void)b;
    (void)body;
    (void)user;
    return NULL;
}

const char *web_zwave_radio_file(const uint8_t *data, size_t len, uint8_t user)
{
    (void)data;
    (void)len;
    (void)user;
    return NULL;
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
