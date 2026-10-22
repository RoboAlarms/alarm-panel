/*
 * web_automations.c - the Panel Manager's Automations page (docs/features/41 DESIGN.md section 10;
 * AUT-001, AUT-002, AUT-006, AUT-008, AUT-010..AUT-016): the stored rules, the zones and
 * partitions a trigger may name, the devices an action may name, and the arming warnings (phase C),
 * as JSON; and the edits back
 * through automation_service, which checks every save against the panel as it is (the validator,
 * the installer option, the confirms, the lock). Nothing here decides what is allowed: the
 * pickers come from hc_auto_offer and every refusal from the service, mapped to the page's words.
 *
 * Without devices (BL-224, DESIGN.md section 15): the wired outputs set "For automations"
 * (outputs_service), the phone services set up (net_notify) and the system master's name, whom
 * an installer's Arm acts as; the trouble trigger and the output, reminder, "still armed", door
 * chime and notification actions, read and written in the same strict way.
 *
 * Memory: the configuration copy (it holds code hashes: zeroized before it is freed), the rules
 * and the device list sit in one web_alloc block (PSRAM), never on the HTTPS worker's stack. The
 * edits write NVS on that worker, whose stack is internal (HTTPD_SSL_CONFIG_DEFAULT's task_caps).
 */
#include "web_automations.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_AUTOMATIONS
#include "alarm/alarm_strings.h"
#include "alarm_service.h"
#include "automation_service.h"
#include "device_service.h"
#include "home_control/home_control.h"
#include "i18n/i18n.h"
#include "mbedtls/platform_util.h"
#include "net_notify.h"
#include "outputs_service.h"
#include "web_mem.h"
#include "web_rules.h"

/* Everything one request reads, in one slow-memory block. */
typedef struct {
    alarm_config_t cfg;
    alarm_snapshot_t snap;
    auto_rule_t rules[AUTO_MAX];
    hc_device_t devices[HC_MAX_DEVICES];
    auto_zone_t zones[ALARM_MAX_ZONES];
    auto_rule_t rule; /* a save's rule */
    outputs_auto_t outputs[AUTO_OUTPUTS]; /* the wired outputs "For automations" (BL-224) */
    size_t n_rules;
    size_t n_devices;
    size_t n_outputs;
} ctx_t;

static ctx_t *ctx_new(void)
{
    return web_alloc(sizeof(ctx_t));
}

static void ctx_free(ctx_t *c)
{
    if (c != NULL) {
        mbedtls_platform_zeroize(c, sizeof(*c)); /* the configuration's code hashes */
        free(c);
    }
}

/* ---- words (the page's contract: never renamed) ------------------------------------------- */

static const char *const TRIG_WORDS[AUTO_TRIG_COUNT] = {
    "none", "armed", "disarmed", "alarm", "fire", "zone_opened", "zone_closed", "time", "trouble"};
/* By auto_trouble_t, the ones a trigger may name (BL-224). */
static const char *const TROUBLE_WORDS[AUTO_TROUBLE_LAST + 1u] = {"any", "ac", "battery", "sensor",
                                                                  "siren"};
/* By net_notify_provider_t: the phone services' names. */
static const char *const NOTIFY_WORDS[NET_NOTIFY_PROVIDERS] = {"ntfy", "pushover"};
static const char *const SOURCE_WORDS[] = {"none", "ha", "zigbee", "zwave"};
static const char *const DAY_WORDS[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
static const char *const KIND_WORDS[DEV_KIND_COUNT] = {
    "none", "light", "switch", "lock", "climate", "cover", "fan", "valve", "scene", "script"};
/* By auto_res_t (append with it): the reason a "failed" or "skipped" status names. */
static const char *const REASON_WORDS[] = {
    NULL,          NULL,          "refused",  "no_answer", "unreachable",     "missing", "busy",
    "held_back",   "not_allowed", "cant_run", "no_clock",  "less_secure_off", "maker_gone",
    NULL /* ALREADY */, "not_ready", "no_warning", "arm_refused", NULL /* PUT_OFF */,
    "no_service" /* BL-224 */};
/* By auto_warn_state_t. */
static const char *const WARN_WORDS[] = {"none", "on", "hour", "night", "day"};

#define COUNT(t) (sizeof(t) / sizeof((t)[0]))

/* The page's action words, and what each is stored as. */
static const struct {
    const char *word;
    uint8_t action;
} ACTIONS[] = {
    {"on", ACT_ON},       {"off", ACT_OFF},     {"toggle", ACT_TOGGLE}, {"lock", ACT_LOCK},
    {"unlock", ACT_UNLOCK}, {"close", ACT_CLOSE}, {"open", ACT_OPEN},     {"stop", ACT_STOP},
    {"run", ACT_RUN},
};

static int find_word(const char *const *table, size_t n, const char *w)
{
    for (size_t i = 0; i < n; i++) {
        if (table[i] != NULL && !strcmp(table[i], w)) {
            return (int)i;
        }
    }
    return -1;
}

/* A stored or offered action as the page's word: a thermostat's SET_MODE "off" is "off". */
static const char *action_word(uint8_t action, const char *mode)
{
    if (action == ACT_SET_MODE && mode != NULL && !strcmp(mode, "off")) {
        return "off";
    }
    for (size_t i = 0; i < COUNT(ACTIONS); i++) {
        if (ACTIONS[i].action == action) {
            return ACTIONS[i].word;
        }
    }
    return "none";
}

static const char *source_word(uint8_t s)
{
    return s < COUNT(SOURCE_WORDS) ? SOURCE_WORDS[s] : "none";
}

static const char *kind_word(uint8_t k)
{
    return k < COUNT(KIND_WORDS) ? KIND_WORDS[k] : "none";
}

static const hc_device_t *find_device(const ctx_t *c, uint8_t source, const char *ref)
{
    for (size_t i = 0; i < c->n_devices; i++) {
        if (c->devices[i].info.source == source && !strcmp(c->devices[i].info.ref, ref)) {
            return &c->devices[i];
        }
    }
    return NULL;
}

/* BL-224: the wired outputs an automation may switch now. */
static void read_outputs(ctx_t *c)
{
    c->n_outputs = outputs_service_auto_list(c->outputs, AUTO_OUTPUTS);
    if (c->n_outputs > AUTO_OUTPUTS) {
        c->n_outputs = AUTO_OUTPUTS;
    }
    for (size_t i = 0; i < c->n_outputs; i++) {
        c->outputs[i].name[sizeof(c->outputs[i].name) - 1u] = '\0';
    }
}

static const outputs_auto_t *find_output(const ctx_t *c, uint8_t number)
{
    for (size_t i = 0; i < c->n_outputs; i++) {
        if (c->outputs[i].number == number) {
            return &c->outputs[i];
        }
    }
    return NULL;
}

static void read_devices(ctx_t *c)
{
    c->n_devices = device_service_list(c->devices, HC_MAX_DEVICES);
    for (size_t i = 0; i < c->n_devices; i++) { /* copies from the registry: NUL them anyway */
        dev_info_t *d = &c->devices[i].info;
        d->ref[sizeof(d->ref) - 1u] = '\0';
        d->name[sizeof(d->name) - 1u] = '\0';
        d->area[sizeof(d->area) - 1u] = '\0';
    }
}

/* An arming or disarming action (phase C), a reminder or "still armed" (BL-224): never tried -
 * they mean something only at their trigger (auto_run_try's AUTO_TRY_ARM). A device, an output,
 * the door chime and a notification are. */
static bool has_untried_action(const auto_rule_t *r)
{
    for (unsigned i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        const uint8_t k = r->then[i].kind;
        if (k == AUTO_ACT_ARM || k == AUTO_ACT_DISARM || k == AUTO_ACT_REMIND ||
            k == AUTO_ACT_CHECK_ARMED) {
            return true;
        }
    }
    return false;
}

/* A user's name as status's "name" gives it (the installer's slot reads "Installer"); "" for none. */
static const char *user_name(const alarm_config_t *cfg, uint8_t user)
{
    if (user == ALARM_USER_INSTALLER) {
        return "Installer";
    }
    return user >= 1u && user <= ALARM_MAX_USERS ? cfg->users[user - 1u].name : "";
}

static const char *record_word(const auto_rule_t *r)
{
    switch (r->rec) {
    case AUTO_REC_OK: return "ok";
    case AUTO_REC_NEWER: return "newer";
    case AUTO_REC_BROKEN: return "broken";
    default: return "bad";
    }
}

/* ---- GET automations ------------------------------------------------------------------------ */

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

static bool add_mask_list(cJSON *o, const char *key, unsigned mask, unsigned first, unsigned n)
{
    cJSON *a = cJSON_AddArrayToObject(o, key);
    bool ok = a != NULL;
    for (unsigned i = 0; i < n; i++) {
        if (mask & (1u << i)) {
            ok &= web_attach(a, NULL, cJSON_CreateNumber(first + i));
        }
    }
    return ok;
}

static bool add_when(cJSON *o, const ctx_t *c, const auto_rule_t *r)
{
    const auto_trigger_t *w = &r->when;
    cJSON *j = cJSON_AddObjectToObject(o, "when");
    bool ok = j != NULL;
    ok &= cJSON_AddStringToObject(j, "kind", w->kind < AUTO_TRIG_COUNT ? TRIG_WORDS[w->kind] : "none") != NULL;
    if (w->kind == AUTO_TRIG_TROUBLE) { /* BL-224: which trouble */
        ok &= cJSON_AddStringToObject(j, "trouble", w->trouble <= AUTO_TROUBLE_LAST ? TROUBLE_WORDS[w->trouble]
                                                                                   : "any") != NULL;
    }
    ok &= add_mask_list(j, "partitions", w->partitions, 1u, ALARM_MAX_PARTITIONS);
    cJSON *lv = cJSON_AddArrayToObject(j, "levels");
    ok &= lv != NULL;
    for (unsigned l = ALARM_LEVEL_AWAY; l <= ALARM_LEVEL_NIGHT; l++) {
        if (w->levels & ALARM_LEVEL_BIT(l)) {
            ok &= web_attach(lv, NULL, cJSON_CreateString(alarm_level_name((alarm_arm_level_t)l)));
        }
    }
    ok &= cJSON_AddNumberToObject(j, "zone", w->zone) != NULL;
    const char *zn = w->zone >= 1u && w->zone <= ALARM_MAX_ZONES ? c->cfg.zones[w->zone - 1u].name
                                                                 : "";
    ok &= cJSON_AddStringToObject(j, "zone_name", zn) != NULL;
    ok &= cJSON_AddNumberToObject(j, "minute", w->minute) != NULL;
    cJSON *days = cJSON_AddArrayToObject(j, "days");
    ok &= days != NULL;
    const unsigned mask = w->days != 0u ? w->days : AUTO_DAYS_ALL; /* 0: every day */
    for (unsigned d = 0; d < 7u; d++) {
        if (mask & (1u << d)) {
            ok &= web_attach(days, NULL, cJSON_CreateString(DAY_WORDS[d]));
        }
    }
    if (w->win_from != 0u && w->win_to != 0u) { /* stored as minute + 1 */
        cJSON *win = cJSON_AddObjectToObject(j, "window");
        ok &= win != NULL;
        ok &= cJSON_AddNumberToObject(win, "from", w->win_from - 1u) != NULL;
        ok &= cJSON_AddNumberToObject(win, "to", w->win_to - 1u) != NULL;
    } else {
        ok &= cJSON_AddNullToObject(j, "window") != NULL;
    }
    return ok;
}

static bool add_then(cJSON *o, const ctx_t *c, const auto_rule_t *r)
{
    cJSON *a = cJSON_AddArrayToObject(o, "then");
    bool ok = a != NULL;
    for (unsigned i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        const auto_action_t *x = &r->then[i];
        cJSON *j = cJSON_CreateObject();
        if (x->kind == AUTO_ACT_ARM || x->kind == AUTO_ACT_DISARM) { /* phase C (AUT-011) */
            const bool arm = x->kind == AUTO_ACT_ARM;
            ok &= cJSON_AddStringToObject(j, "kind", arm ? "arm" : "disarm") != NULL;
            ok &= cJSON_AddNumberToObject(j, "partition", x->partition) != NULL;
            if (arm) {
                ok &= cJSON_AddStringToObject(j, "level", alarm_level_name((alarm_arm_level_t)x->arm_level)) != NULL;
                ok &= cJSON_AddNumberToObject(j, "warn_min", x->warn_min) != NULL;
                ok &= cJSON_AddBoolToObject(j, "bypass", x->bypass_open != 0u) != NULL;
            } else {
                ok &= cJSON_AddBoolToObject(j, "confirmed", x->confirmed != 0u) != NULL;
            }
            /* who set it up (D30: never "arms as"): it acts as them, re-checked each time */
            ok &= cJSON_AddStringToObject(j, "made_by", user_name(&c->cfg, r->made_by)) != NULL;
            /* BL-224: an installer's Arm acts as the system master; saved_by says who set it up
             * ("Set up by Installer · uses Alex's permission") */
            if (r->saved_by != 0u) {
                ok &= cJSON_AddStringToObject(j, "saved_by", user_name(&c->cfg, r->saved_by)) != NULL;
            } else {
                ok &= cJSON_AddNullToObject(j, "saved_by") != NULL;
            }
            ok &= web_attach(a, NULL, j);
            continue;
        }
        switch (x->kind) { /* BL-224: without a device */
        case AUTO_ACT_OUTPUT:
            ok &= cJSON_AddStringToObject(j, "kind", "output") != NULL;
            ok &= cJSON_AddNumberToObject(j, "output", x->output) != NULL;
            ok &= cJSON_AddStringToObject(j, "action", x->dev_action == ACT_ON ? "on" : "off") != NULL;
            ok &= cJSON_AddNumberToObject(j, "off_after", x->off_after_min) != NULL;
            ok &= web_attach(a, NULL, j);
            continue;
        case AUTO_ACT_REMIND:
            ok &= cJSON_AddStringToObject(j, "kind", "remind") != NULL;
            ok &= cJSON_AddNumberToObject(j, "partition", x->partition) != NULL;
            ok &= cJSON_AddBoolToObject(j, "notify", x->option != 0u) != NULL;
            ok &= web_attach(a, NULL, j);
            continue;
        case AUTO_ACT_CHECK_ARMED:
            ok &= cJSON_AddStringToObject(j, "kind", "still_armed") != NULL;
            ok &= cJSON_AddNumberToObject(j, "partition", x->partition) != NULL;
            ok &= web_attach(a, NULL, j);
            continue;
        case AUTO_ACT_CHIME:
            ok &= cJSON_AddStringToObject(j, "kind", "chime") != NULL;
            ok &= cJSON_AddNumberToObject(j, "partition", x->partition) != NULL;
            ok &= cJSON_AddBoolToObject(j, "on", x->option != 0u) != NULL;
            ok &= web_attach(a, NULL, j);
            continue;
        case AUTO_ACT_NOTIFY:
            ok &= cJSON_AddStringToObject(j, "kind", "notify") != NULL;
            ok &= web_attach(a, NULL, j);
            continue;
        default: break;
        }
        const hc_device_t *d = find_device(c, x->source, x->ref);
        ok &= cJSON_AddStringToObject(j, "kind", "device") != NULL;
        ok &= cJSON_AddStringToObject(j, "source", source_word(x->source)) != NULL;
        ok &= cJSON_AddStringToObject(j, "ref", x->ref) != NULL;
        ok &= cJSON_AddStringToObject(j, "device", d != NULL ? d->info.name : x->name) != NULL;
        ok &= cJSON_AddStringToObject(j, "area", d != NULL ? d->info.area : "") != NULL;
        ok &= cJSON_AddBoolToObject(j, "found", d != NULL) != NULL;
        ok &= cJSON_AddBoolToObject(j, "online", d != NULL && d->info.online) != NULL;
        ok &= cJSON_AddStringToObject(j, "action", action_word(x->dev_action, x->mode)) != NULL;
        ok &= cJSON_AddNumberToObject(j, "off_after", x->off_after_min) != NULL;
        ok &= cJSON_AddBoolToObject(j, "confirmed", x->confirmed != 0u) != NULL;
        ok &= web_attach(a, NULL, j);
    }
    return ok;
}

/* The "Last run" status (AUT-006): ran (or tried, from Try it now), failed when a send went wrong,
 * skipped when it was held back or not sent by rule (no clock, the option off, the maker gone). */
static cJSON *status_json(const ctx_t *c, const auto_rule_t *r)
{
    auto_status_t st;
    memset(&st, 0, sizeof(st));
    if (r->uid != 0u) {
        (void)automation_service_status(r->uid, &st);
    }
    const char *result = "none";
    switch (st.result) {
    case AUTO_RES_NONE: break;
    case AUTO_RES_DONE: result = st.tried ? "tried" : "ran"; break;
    case AUTO_RES_HELD_BACK:
    case AUTO_RES_NO_CLOCK:
    case AUTO_RES_LESS_SECURE_OFF:
    case AUTO_RES_MAKER_GONE:
    case AUTO_RES_NOT_READY:  /* phase C: not armed, a zone was open (AUT-014) */
    case AUTO_RES_NO_WARNING: /* no whole warning (AUT-012) */
    case AUTO_RES_NO_SERVICE: result = "skipped"; break; /* BL-224: no phone service set up */
    case AUTO_RES_ALREADY: result = "already"; break;
    case AUTO_RES_PUT_OFF: result = "put_off"; break; /* D20: who, until when */
    default: result = "failed"; break; /* ARM_REFUSED among them: "refused" names the answer */
    }
    cJSON *s = cJSON_CreateObject();
    bool ok = s != NULL;
    ok &= cJSON_AddStringToObject(s, "result", result) != NULL;
    ok &= cJSON_AddBoolToObject(s, "tried", st.tried != 0u) != NULL; /* any outcome of Try it now, failed ones too */
    if (st.result != AUTO_RES_NONE && st.at_epoch_ms > 0) {
        ok &= cJSON_AddNumberToObject(s, "at", (double)st.at_epoch_ms) != NULL;
    } else {
        ok &= cJSON_AddNullToObject(s, "at") != NULL;
    }
    const bool has_action =
        st.result != AUTO_RES_NONE && st.action_index < r->n && st.action_index < AUTO_ACTIONS &&
        r->then[st.action_index].kind == AUTO_ACT_DEVICE;
    const bool has_output =
        st.result != AUTO_RES_NONE && st.action_index < r->n && st.action_index < AUTO_ACTIONS &&
        r->then[st.action_index].kind == AUTO_ACT_OUTPUT;
    if (has_action) {
        const auto_action_t *x = &r->then[st.action_index];
        const hc_device_t *d = find_device(c, x->source, x->ref);
        ok &= cJSON_AddStringToObject(s, "action", action_word(x->dev_action, x->mode)) != NULL;
        ok &= cJSON_AddStringToObject(s, "device", d != NULL ? d->info.name : x->name) != NULL;
    } else if (has_output) { /* BL-224: "Garden Lights wasn't reachable" */
        const auto_action_t *x = &r->then[st.action_index];
        const outputs_auto_t *out = find_output(c, x->output);
        ok &= cJSON_AddStringToObject(s, "action", x->dev_action == ACT_ON ? "on" : "off") != NULL;
        ok &= cJSON_AddStringToObject(s, "device", out != NULL ? out->name : "") != NULL;
    } else {
        ok &= cJSON_AddNullToObject(s, "action") != NULL;
        ok &= cJSON_AddNullToObject(s, "device") != NULL;
    }
    const char *why = st.result < COUNT(REASON_WORDS) ? REASON_WORDS[st.result] : NULL;
    if (why != NULL) {
        ok &= cJSON_AddStringToObject(s, "reason", why) != NULL;
    } else {
        ok &= cJSON_AddNullToObject(s, "reason") != NULL;
    }
    if (st.result == AUTO_RES_NOT_READY) { /* the zone the panel named, if it did */
        const bool named = st.zone >= 1u && st.zone <= ALARM_MAX_ZONES;
        ok &= cJSON_AddStringToObject(s, "zone", named ? c->cfg.zones[st.zone - 1u].name : "") != NULL;
    } else if (st.result == AUTO_RES_ARM_REFUSED) {
        ok &= cJSON_AddStringToObject(s, "refused", alarm_result_name((alarm_result_t)st.alarm_res)) != NULL;
    } else if (st.result == AUTO_RES_PUT_OFF) {
        ok &= cJSON_AddStringToObject(s, "by", user_name(&c->cfg, st.user)) != NULL;
        ok &= cJSON_AddStringToObject(s, "choice", st.choice == AUTO_PUT_OFF_HOUR ? "hour"
                                             : st.night != 0u              ? "night"
                                                                           : "day") != NULL;
        if (st.choice == AUTO_PUT_OFF_HOUR && st.until_epoch_ms > 0) {
            ok &= cJSON_AddNumberToObject(s, "until", (double)st.until_epoch_ms) != NULL;
        } else {
            ok &= cJSON_AddNullToObject(s, "until") != NULL;
        }
    }
    return built(s, ok);
}

static cJSON *rule_json(const ctx_t *c, const auto_rule_t *r)
{
    cJSON *o = cJSON_CreateObject();
    bool ok = o != NULL;
    ok &= cJSON_AddNumberToObject(o, "slot", r->slot) != NULL;
    ok &= cJSON_AddNumberToObject(o, "uid", r->uid) != NULL;
    ok &= cJSON_AddBoolToObject(o, "on", !r->off) != NULL;
    const char *rec = record_word(r);
    ok &= cJSON_AddStringToObject(o, "record", rec) != NULL;
    if (!strcmp(rec, "ok")) {
        ok &= add_when(o, c, r);
        ok &= add_then(o, c, r);
    } else { /* shown, never edited: Delete only (AUT-008) */
        ok &= cJSON_AddNullToObject(o, "when") != NULL;
        ok &= cJSON_AddArrayToObject(o, "then") != NULL;
    }
    ok &= web_attach(o, "status", status_json(c, r));
    return built(o, ok);
}

/* AUT-004: what the trigger pickers may offer. Only zones in use. */
static bool add_zones(cJSON *o, const ctx_t *c)
{
    cJSON *a = cJSON_AddArrayToObject(o, "zones");
    bool ok = a != NULL;
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = &c->cfg.zones[z];
        if (zc->type == ALARM_ZT_NOT_USED) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(j, "number", z + 1u) != NULL;
        ok &= cJSON_AddStringToObject(j, "name", zc->name) != NULL;
        ok &= cJSON_AddStringToObject(j, "class",
                                alarm_device_class_name((alarm_device_class_t)zc->device_class)) != NULL;
        ok &= cJSON_AddNumberToObject(j, "partition", zc->partition) != NULL;
        ok &= cJSON_AddBoolToObject(j, "usable_open",
                              auto_zone_usable((uint8_t)zc->type, zc->device_class, false)) != NULL;
        ok &= cJSON_AddBoolToObject(j, "usable_close",
                              auto_zone_usable((uint8_t)zc->type, zc->device_class, true)) != NULL;
        ok &= web_attach(a, NULL, j);
    }
    return ok;
}

static bool add_partitions(cJSON *o, const ctx_t *c)
{
    cJSON *a = cJSON_AddArrayToObject(o, "partitions");
    bool ok = a != NULL;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (!c->cfg.partitions[p].enabled) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(j, "number", p + 1u) != NULL;
        ok &= cJSON_AddStringToObject(j, "name", c->cfg.partitions[p].name) != NULL;
        ok &= web_attach(a, NULL, j);
    }
    return ok;
}

/* BL-224: the wired outputs "For automations", the phone services set up, and the system
 * master's name (an installer's Arm uses their permission). */
static bool add_panel_parts(cJSON *o, const ctx_t *c)
{
    cJSON *a = cJSON_AddArrayToObject(o, "outputs");
    bool ok = a != NULL;
    for (size_t i = 0; i < c->n_outputs; i++) {
        cJSON *j = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(j, "number", c->outputs[i].number) != NULL;
        ok &= cJSON_AddStringToObject(j, "name", c->outputs[i].name) != NULL;
        ok &= cJSON_AddNumberToObject(j, "gpio", c->outputs[i].gpio) != NULL;
        ok &= web_attach(a, NULL, j);
    }
    cJSON *n = cJSON_AddArrayToObject(o, "notify");
    ok &= n != NULL;
    const uint8_t services = net_notify_services();
    for (unsigned p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
        if (services & (1u << p)) {
            ok &= web_attach(n, NULL, cJSON_CreateString(NOTIFY_WORDS[p]));
        }
    }
    ok &= cJSON_AddStringToObject(o, "master", user_name(&c->cfg, ALARM_USER_MASTER)) != NULL;
    return ok;
}

static bool read_config(ctx_t *c)
{
    if (alarm_service_get_config(&c->cfg) != ESP_OK) {
        return false;
    }
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        c->cfg.zones[z].name[sizeof(c->cfg.zones[z].name) - 1u] = '\0';
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        c->cfg.partitions[p].name[sizeof(c->cfg.partitions[p].name) - 1u] = '\0';
    }
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) { /* the makers' and the put-offs' names */
        c->cfg.users[u].name[sizeof(c->cfg.users[u].name) - 1u] = '\0';
    }
    return true;
}

bool web_automations_available(void)
{
    return true;
}

bool web_automations_add_warnings(cJSON *o, const char *key, const alarm_config_t *cfg,
                                  uint8_t partitions)
{
    cJSON *a = cJSON_AddArrayToObject(o, key);
    auto_warning_t *w = web_alloc(AUTO_MAX * sizeof(*w)); /* PSRAM: never on the worker's stack */
    if (a == NULL || w == NULL) {
        free(w);
        return false;
    }
    bool ok = true;
    const size_t n = automation_service_warnings(w, AUTO_MAX);
    for (size_t i = 0; i < n && i < AUTO_MAX; i++) {
        const auto_warning_t *x = &w[i];
        if (x->state == AUTO_WARN_NONE || x->state >= COUNT(WARN_WORDS) || x->partition < 1u ||
            x->partition > ALARM_MAX_PARTITIONS || (partitions & (1u << (x->partition - 1u))) == 0u) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(j, "uid", x->uid) != NULL;
        ok &= cJSON_AddNumberToObject(j, "partition", x->partition) != NULL;
        ok &= cJSON_AddStringToObject(j, "level", alarm_level_name((alarm_arm_level_t)x->level)) != NULL;
        if (x->arm_at_epoch_ms > 0) {
            ok &= cJSON_AddNumberToObject(j, "at", (double)(x->arm_at_epoch_ms / 1000)) != NULL;
        } else {
            ok &= cJSON_AddNullToObject(j, "at") != NULL; /* the clock isn't set */
        }
        ok &= cJSON_AddStringToObject(j, "state", WARN_WORDS[x->state]) != NULL;
        if (x->put_off_by != 0u) {
            ok &= cJSON_AddStringToObject(j, "by", user_name(cfg, x->put_off_by)) != NULL;
        } else {
            ok &= cJSON_AddNullToObject(j, "by") != NULL;
        }
        ok &= cJSON_AddBoolToObject(j, "bypass", x->bypass != 0u) != NULL;
        ok &= web_attach(a, NULL, j);
    }
    free(w);
    return ok;
}

bool web_automations_add_reminders(cJSON *o, const char *key, uint8_t partitions)
{
    cJSON *a = cJSON_AddArrayToObject(o, key);
    if (a == NULL) {
        return false;
    }
    auto_remind_t m[AUTO_MAX]; /* 96 bytes */
    bool ok = true;
    const size_t n = automation_service_reminders(m, AUTO_MAX);
    for (size_t i = 0; i < n && i < AUTO_MAX; i++) {
        const auto_remind_t *x = &m[i];
        if (x->on == 0u || x->partition < 1u || x->partition > ALARM_MAX_PARTITIONS ||
            (partitions & (1u << (x->partition - 1u))) == 0u) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(j, "uid", x->uid) != NULL;
        ok &= cJSON_AddNumberToObject(j, "partition", x->partition) != NULL;
        ok &= cJSON_AddNumberToObject(j, "minute", x->minute) != NULL;
        ok &= web_attach(a, NULL, j);
    }
    return ok;
}

cJSON *web_automations_get(void)
{
    ctx_t *c = ctx_new();
    if (c == NULL || !read_config(c)) {
        ctx_free(c);
        return NULL;
    }
    alarm_service_get_snapshot(&c->snap);
    read_devices(c);
    read_outputs(c);
    const uint16_t gen = automation_service_gen(); /* before the list: a stale gen only refuses */
    c->n_rules = automation_service_list(c->rules, AUTO_MAX);
    cJSON *o = cJSON_CreateObject();
    bool ok = o != NULL;
    ok &= cJSON_AddNumberToObject(o, "maximum", AUTO_MAX) != NULL;
    ok &= cJSON_AddNumberToObject(o, "used", (double)c->n_rules) != NULL;
    ok &= cJSON_AddNumberToObject(o, "gen", gen) != NULL;
    ok &= cJSON_AddBoolToObject(o, "clock_set",
                          (c->snap.sys_troubles & (1u << (unsigned)ALARM_TRBL_CLOCK_NOT_SET)) == 0u) != NULL;
    ok &= cJSON_AddBoolToObject(o, "read_only", automation_service_read_only()) != NULL;
    ok &= cJSON_AddBoolToObject(o, "less_secure", c->cfg.opt.auto_less_secure) != NULL; /* AUT-015 */
    cJSON *list = cJSON_AddArrayToObject(o, "automations");
    ok &= list != NULL;
    for (size_t i = 0; i < c->n_rules; i++) {
        c->rules[i].then[0].ref[DEV_REF_LEN - 1u] = '\0';
        c->rules[i].then[1].ref[DEV_REF_LEN - 1u] = '\0';
        c->rules[i].then[0].name[DEV_NAME_LEN - 1u] = '\0';
        c->rules[i].then[1].name[DEV_NAME_LEN - 1u] = '\0';
        c->rules[i].then[0].mode[DEV_MODE_LEN - 1u] = '\0';
        c->rules[i].then[1].mode[DEV_MODE_LEN - 1u] = '\0';
        ok &= web_attach(list, NULL, rule_json(c, &c->rules[i]));
    }
    ok &= add_zones(o, c);
    ok &= add_partitions(o, c);
    ok &= add_panel_parts(o, c);
    ok &= web_automations_add_warnings(o, "warnings", &c->cfg, 0xFFu); /* the page's own: every one */
    ok &= web_automations_add_reminders(o, "reminders", 0xFFu);
    ctx_free(c);
    return built(o, ok);
}

/* ---- GET automations/devices ----------------------------------------------------------------- */

cJSON *web_automations_devices(void)
{
    ctx_t *c = ctx_new();
    if (c == NULL) {
        return NULL;
    }
    /* The installer option as it is now (AUT-015): with it off no Unlock, Open or Stop row */
    auto_env_t env;
    if (!automation_service_env(&env, c->zones)) {
        ctx_free(c);
        return NULL;
    }
    read_devices(c);
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "devices");
    bool ok = a != NULL;
    for (size_t i = 0; i < c->n_devices; i++) {
        const dev_info_t *d = &c->devices[i].info;
        hc_auto_offer_t offer[HC_AUTO_OFFER_MAX];
        const size_t n = hc_auto_offer((dev_kind_t)d->kind, d->source, env.less_secure, offer);
        if (n == 0u) {
            continue; /* scripts, and devices an automation can do nothing with (AUT-002) */
        }
        cJSON *j = cJSON_CreateObject();
        ok &= cJSON_AddStringToObject(j, "source", source_word(d->source)) != NULL;
        ok &= cJSON_AddStringToObject(j, "ref", d->ref) != NULL;
        ok &= cJSON_AddStringToObject(j, "name", d->name) != NULL;
        ok &= cJSON_AddStringToObject(j, "area", d->area) != NULL;
        ok &= cJSON_AddStringToObject(j, "kind", kind_word(d->kind)) != NULL;
        ok &= cJSON_AddBoolToObject(j, "online", d->online) != NULL;
        cJSON *acts = cJSON_AddArrayToObject(j, "actions");
        ok &= acts != NULL;
        for (size_t k = 0; k < n && k < HC_AUTO_OFFER_MAX; k++) {
            offer[k].mode[DEV_MODE_LEN - 1u] = '\0';
            cJSON *x = cJSON_CreateObject();
            ok &= cJSON_AddStringToObject(x, "action", action_word(offer[k].action, offer[k].mode)) != NULL;
            /* D8, D9: asked once when it is picked */
            ok &= cJSON_AddBoolToObject(x, "confirm", auto_action_needs_confirm(d->kind, offer[k].action)) != NULL;
            ok &= web_attach(acts, NULL, x);
        }
        ok &= web_attach(a, NULL, j);
    }
    ctx_free(c);
    return built(o, ok);
}

/* ---- POST automations ------------------------------------------------------------------------ */

static cJSON *answer_error(int *http, int status, const char *error, int index)
{
    *http = status;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", error);
    if (index >= 0) {
        cJSON_AddNumberToObject(o, "index", index);
    }
    return o;
}

/* A whole number in [lo, hi]; absent gives *out = dflt when dflt >= 0, else false. */
static bool get_number(const cJSON *o, const char *key, long lo, long hi, long dflt, long *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (v == NULL || cJSON_IsNull(v)) {
        if (dflt < 0) {
            return false;
        }
        *out = dflt;
        return true;
    }
    /* the range first: casting a double outside long's is undefined */
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= (double)lo && v->valuedouble <= (double)hi) ||
        v->valuedouble != (double)(long)v->valuedouble) {
        return false;
    }
    *out = (long)v->valuedouble;
    return true;
}

static const char *get_string(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : NULL;
}

/* The lists into masks: partitions as numbers, levels and days as words; false when malformed. */
static bool partitions_mask(const cJSON *o, uint8_t *mask)
{
    *mask = 0u;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "partitions");
    if (a == NULL || cJSON_IsNull(a)) {
        return true; /* any */
    }
    if (!cJSON_IsArray(a)) {
        return false;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, a)
    {
        if (!cJSON_IsNumber(e) || !(e->valuedouble >= 1 && e->valuedouble <= ALARM_MAX_PARTITIONS) ||
            e->valuedouble != (double)(int)e->valuedouble) {
            return false;
        }
        *mask |= (uint8_t)(1u << ((unsigned)e->valuedouble - 1u));
    }
    return true;
}

static bool levels_mask(const cJSON *o, uint8_t *mask)
{
    *mask = 0u;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "levels");
    if (a == NULL || cJSON_IsNull(a)) {
        return true; /* any level */
    }
    if (!cJSON_IsArray(a)) {
        return false;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, a)
    {
        if (!cJSON_IsString(e)) {
            return false;
        }
        unsigned l = ALARM_LEVEL_AWAY;
        while (l <= ALARM_LEVEL_NIGHT && strcmp(alarm_level_name((alarm_arm_level_t)l),
                                                e->valuestring)) {
            l++;
        }
        if (l > ALARM_LEVEL_NIGHT) {
            return false;
        }
        *mask |= ALARM_LEVEL_BIT(l);
    }
    return true;
}

/* days: an array of "mon".."sun"; missing or empty gives 0 (the caller refuses it). */
static bool days_mask(const cJSON *o, uint8_t *mask)
{
    *mask = 0u;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "days");
    if (a == NULL || cJSON_IsNull(a)) {
        return true;
    }
    if (!cJSON_IsArray(a)) {
        return false;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, a)
    {
        const int d = cJSON_IsString(e) ? find_word(DAY_WORDS, COUNT(DAY_WORDS), e->valuestring)
                                        : -1;
        if (d < 0) {
            return false;
        }
        *mask |= (uint8_t)(1u << (unsigned)d);
    }
    return true;
}

typedef enum { PARSE_OK, PARSE_BAD, PARSE_INCOMPLETE } parse_t;

/* when -> r->when. Fields another trigger kind doesn't use are left at zero, whatever was sent. */
static parse_t parse_when(const cJSON *w, auto_trigger_t *t)
{
    memset(t, 0, sizeof(*t));
    if (!cJSON_IsObject(w)) {
        return PARSE_BAD;
    }
    const char *k = get_string(w, "kind");
    const int kind = k != NULL ? find_word(TRIG_WORDS, AUTO_TRIG_COUNT, k) : -1;
    if (kind <= 0) {
        return PARSE_BAD;
    }
    t->kind = (uint8_t)kind;
    const bool zone_trig = kind == AUTO_TRIG_ZONE_OPENED || kind == AUTO_TRIG_ZONE_CLOSED;
    if (kind == AUTO_TRIG_ARMED || kind == AUTO_TRIG_DISARMED || kind == AUTO_TRIG_ALARM ||
        kind == AUTO_TRIG_FIRE) {
        if (!partitions_mask(w, &t->partitions)) {
            return PARSE_BAD;
        }
    }
    if (kind == AUTO_TRIG_ARMED && !levels_mask(w, &t->levels)) {
        return PARSE_BAD;
    }
    if (kind == AUTO_TRIG_TROUBLE) { /* BL-224: which trouble; none named is any */
        const cJSON *tv = cJSON_GetObjectItemCaseSensitive(w, "trouble");
        int which = 0;
        if (tv != NULL && !cJSON_IsNull(tv)) {
            which = cJSON_IsString(tv) ? find_word(TROUBLE_WORDS, COUNT(TROUBLE_WORDS), tv->valuestring)
                                       : -1;
            if (which < 0) {
                return PARSE_BAD;
            }
        }
        t->trouble = (uint8_t)which;
    }
    long n;
    if (zone_trig) {
        if (!get_number(w, "zone", 0, ALARM_MAX_ZONES, 0, &n)) {
            return PARSE_BAD;
        }
        t->zone = (uint16_t)n; /* 0: the validator's incomplete */
    }
    if (kind == AUTO_TRIG_TIME) {
        if (!get_number(w, "minute", 0, AUTO_MINUTES_PER_DAY - 1u, -1, &n) ||
            !days_mask(w, &t->days)) {
            return PARSE_BAD;
        }
        t->minute = (uint16_t)n;
        if (t->days == 0u) {
            return PARSE_INCOMPLETE; /* no day chosen: never "every day" by accident */
        }
        return PARSE_OK; /* a time of day has no "Only between" */
    }
    const cJSON *win = cJSON_GetObjectItemCaseSensitive(w, "window");
    if (win != NULL && !cJSON_IsNull(win)) {
        long from, to;
        if (!cJSON_IsObject(win) ||
            !get_number(win, "from", 0, AUTO_MINUTES_PER_DAY - 1u, -1, &from) ||
            !get_number(win, "to", 0, AUTO_MINUTES_PER_DAY - 1u, -1, &to)) {
            return PARSE_BAD;
        }
        t->win_from = (uint16_t)(from + 1); /* stored as minute + 1: 0 is "any time" */
        t->win_to = (uint16_t)(to + 1);
    }
    return PARSE_OK;
}

/* An arm or disarm item -> *a (phase C, AUT-011): only its own fields, the ranges the model holds;
 * what is allowed (the level, 1-15 minutes, the partition in use, the option, the confirm, the
 * trigger) is the validator's. made_by is never read: the service makes the saver the maker. */
static parse_t parse_alarm_action(const cJSON *j, bool arm, auto_action_t *a)
{
    long part, warn = 0;
    const cJSON *conf = cJSON_GetObjectItemCaseSensitive(j, "confirmed");
    const cJSON *byp = cJSON_GetObjectItemCaseSensitive(j, "bypass");
    if (!get_number(j, "partition", 1, ALARM_MAX_PARTITIONS, -1, &part) ||
        (conf != NULL && !cJSON_IsNull(conf) && !cJSON_IsBool(conf)) ||
        (byp != NULL && !cJSON_IsNull(byp) && !cJSON_IsBool(byp))) {
        return PARSE_BAD;
    }
    a->kind = arm ? AUTO_ACT_ARM : AUTO_ACT_DISARM;
    a->partition = (uint8_t)part;
    if (!arm) {
        a->confirmed = cJSON_IsTrue(conf) ? 1u : 0u; /* D24: after a door, window or alarm */
        return cJSON_IsTrue(byp) ? PARSE_BAD : PARSE_OK;
    }
    const char *level = get_string(j, "level");
    unsigned l = ALARM_LEVEL_AWAY;
    while (level != NULL && l <= ALARM_LEVEL_NIGHT &&
           strcmp(alarm_level_name((alarm_arm_level_t)l), level)) {
        l++;
    }
    if (level == NULL || l > ALARM_LEVEL_NIGHT || !get_number(j, "warn_min", 0, UINT8_MAX, -1, &warn) ||
        cJSON_IsTrue(conf)) {
        return PARSE_BAD;
    }
    a->arm_level = (uint8_t)l;
    a->warn_min = (uint8_t)warn;          /* 1..15: the validator's INCOMPLETE otherwise */
    a->bypass_open = cJSON_IsTrue(byp) ? 1u : 0u; /* D29, D31: refused while the option is off */
    return PARSE_OK;
}

/* A true or false field; absent or null gives dflt (or false when dflt < 0: required). */
static bool get_bool(const cJSON *o, const char *key, int dflt, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (v == NULL || cJSON_IsNull(v)) {
        *out = dflt > 0;
        return dflt >= 0;
    }
    if (!cJSON_IsBool(v)) {
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

/* BL-224: an action without a device -> *a, only its own fields in the ranges the model holds;
 * what is allowed (an output "For automations", a reminder after a time, the partition in use)
 * is the validator's. The page's words: output {output, action on|off, off_after}, remind
 * {partition, notify}, still_armed {partition}, chime {partition, on}, notify {}. Returns
 * PARSE_INCOMPLETE when the kind isn't one of them (the caller goes on). */
static parse_t parse_panel_action(const cJSON *j, const char *kind, auto_action_t *a)
{
    long part = 0, out = 0, off_after = 0;
    bool flag = false;
    if (!strcmp(kind, "output")) {
        const char *act = get_string(j, "action");
        if (!get_number(j, "output", 1, AUTO_OUTPUTS, -1, &out) || act == NULL ||
            (strcmp(act, "on") != 0 && strcmp(act, "off") != 0) ||
            !get_number(j, "off_after", 0, AUTO_MINUTES_PER_DAY, 0, &off_after) ||
            (off_after != 0 && strcmp(act, "on") != 0)) {
            return PARSE_BAD; /* "Turn it off after" only with Turn on */
        }
        a->kind = AUTO_ACT_OUTPUT;
        a->output = (uint8_t)out;
        a->dev_action = !strcmp(act, "on") ? ACT_ON : ACT_OFF;
        a->off_after_min = (uint16_t)off_after;
        return PARSE_OK;
    }
    if (!strcmp(kind, "notify")) {
        a->kind = AUTO_ACT_NOTIFY;
        return PARSE_OK;
    }
    const bool remind = !strcmp(kind, "remind"), still = !strcmp(kind, "still_armed"),
               chime = !strcmp(kind, "chime");
    if (!remind && !still && !chime) {
        return PARSE_INCOMPLETE;
    }
    if (!get_number(j, "partition", 1, ALARM_MAX_PARTITIONS, -1, &part) ||
        (remind && !get_bool(j, "notify", 0, &flag)) || (chime && !get_bool(j, "on", -1, &flag))) {
        return PARSE_BAD;
    }
    a->kind = remind ? AUTO_ACT_REMIND : still ? AUTO_ACT_CHECK_ARMED : AUTO_ACT_CHIME;
    a->partition = (uint8_t)part;
    a->option = flag ? 1u : 0u; /* a reminder's notification, the chime on or off */
    return PARSE_OK;
}

/* One then[] item -> *a. *err: a 409 the page shows on that action (missing_device,
 * action_not_allowed); PARSE_BAD: 400. */
static parse_t parse_action(const ctx_t *c, const cJSON *j, auto_action_t *a, const char **err)
{
    memset(a, 0, sizeof(*a));
    a->value = -1;
    if (!cJSON_IsObject(j)) {
        return PARSE_BAD;
    }
    const char *kind = get_string(j, "kind");
    if (kind != NULL && (!strcmp(kind, "arm") || !strcmp(kind, "disarm"))) {
        return parse_alarm_action(j, !strcmp(kind, "arm"), a);
    }
    if (kind != NULL && strcmp(kind, "device") != 0) {
        const parse_t pp = parse_panel_action(j, kind, a);
        if (pp != PARSE_INCOMPLETE) {
            return pp;
        }
        return PARSE_BAD; /* no such kind */
    }
    const char *src = get_string(j, "source");
    const char *ref = get_string(j, "ref");
    const char *act = get_string(j, "action");
    const int source = src != NULL ? find_word(SOURCE_WORDS, COUNT(SOURCE_WORDS), src) : -1;
    if (kind == NULL || strcmp(kind, "device") != 0 || source <= 0 || ref == NULL ||
        act == NULL) {
        return PARSE_BAD; /* a device action, or arm or disarm above */
    }
    /* A ref is never cut: a shortened one could name another device. */
    const size_t ref_len = strlen(ref);
    if (ref_len == 0u || ref_len >= DEV_REF_LEN) {
        return PARSE_BAD;
    }
    size_t w = 0;
    while (w < COUNT(ACTIONS) && strcmp(ACTIONS[w].word, act) != 0) {
        w++;
    }
    if (w == COUNT(ACTIONS)) {
        return PARSE_BAD;
    }
    long off_after;
    const cJSON *conf = cJSON_GetObjectItemCaseSensitive(j, "confirmed");
    if (!get_number(j, "off_after", 0, AUTO_MINUTES_PER_DAY, 0, &off_after) ||
        (conf != NULL && !cJSON_IsNull(conf) && !cJSON_IsBool(conf)) ||
        (off_after != 0 && ACTIONS[w].action != ACT_ON)) {
        return PARSE_BAD; /* "Turn it off after" only with Turn on */
    }
    a->kind = AUTO_ACT_DEVICE;
    a->source = (uint8_t)source;
    memcpy(a->ref, ref, ref_len + 1u);
    a->confirmed = cJSON_IsTrue(conf) ? 1u : 0u;
    a->off_after_min = (uint16_t)off_after;
    /* The kind is the live device's, matched on source and ref (AUT-002 layer 4 re-checks it). */
    const hc_device_t *d = find_device(c, (uint8_t)source, ref);
    if (d == NULL) {
        *err = "missing_device";
        return PARSE_INCOMPLETE;
    }
    a->dev_kind = d->info.kind;
    i18n_utf8_copy(a->name, sizeof(a->name), d->info.name);
    a->dev_action = ACTIONS[w].action;
    if (a->dev_action == ACT_OFF && a->dev_kind == DEV_CLIMATE) {
        a->dev_action = ACT_SET_MODE; /* D18: a thermostat's Turn off */
        memcpy(a->mode, "off", 4u);
    }
    /* Only what the pickers offer for this kind from this source (the option aside: the
     * service answers less_secure_off for that). */
    hc_auto_offer_t offer[HC_AUTO_OFFER_MAX];
    const size_t n = hc_auto_offer((dev_kind_t)a->dev_kind, a->source, true, offer);
    for (size_t k = 0; k < n && k < HC_AUTO_OFFER_MAX; k++) {
        if (offer[k].action == a->dev_action && !strcmp(offer[k].mode, a->mode)) {
            a->value = (int16_t)offer[k].value; /* Turn on: -1, no level */
            return PARSE_OK;
        }
    }
    *err = "action_not_allowed";
    return PARSE_INCOMPLETE;
}

/* Which action a refusal is about (the page marks it): the first that fails the test. */
static int first_action(const auto_rule_t *r, auto_check_t why)
{
    for (unsigned i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        const auto_action_t *a = &r->then[i];
        bool hit = false;
        switch (why) {
        case AUTO_ERR_NEEDS_CONFIRM:
            hit = auto_action_needs_trigger_confirm(r->when.kind, a) && a->confirmed == 0u;
            break;
        case AUTO_ERR_LESS_SECURE_OFF:
            hit = auto_action_less_secure(a) || a->bypass_open != 0u; /* D31: the bypass too */
            break;
        case AUTO_ERR_UNDOES_ARMING: hit = a->kind == AUTO_ACT_DISARM; break;
        case AUTO_ERR_NOT_ALLOWED:
            /* a device action no automation may do, or a Disarm (after an alarm, or the
             * installer's: BL-187) */
            hit = a->kind == AUTO_ACT_DEVICE
                      ? !hc_auto_allowed((dev_kind_t)a->dev_kind, (dev_action_t)a->dev_action,
                                         a->value, a->mode, true) ||
                            (auto_action_needs_confirm(a->dev_kind, a->dev_action) &&
                             a->confirmed == 0u)
                      : a->kind == AUTO_ACT_DISARM;
            break;
        case AUTO_ERR_OUTPUT: hit = a->kind == AUTO_ACT_OUTPUT; break; /* BL-224 */
        case AUTO_ERR_NEEDS_TIME:
            hit = a->kind == AUTO_ACT_REMIND || a->kind == AUTO_ACT_CHECK_ARMED;
            break;
        default: break;
        }
        if (hit) {
            return (int)i;
        }
    }
    return why == AUTO_ERR_SAME_DEVICE ? 1 : -1;
}

/* The validator's answer as the page's 409. */
static cJSON *check_error(int *http, const auto_rule_t *r, auto_check_t why)
{
    switch (why) {
    case AUTO_ERR_ZONE: return answer_error(http, 409, "bad_zone", -1);
    case AUTO_ERR_NOT_ALLOWED: return answer_error(http, 409, "action_not_allowed", first_action(r, why));
    case AUTO_ERR_SAME_DEVICE: /* one device twice (AUT-007): the second action */
        return answer_error(http, 409, "same_device", first_action(r, why));
    case AUTO_ERR_LESS_SECURE_OFF:
        return answer_error(http, 409, "less_secure_off", first_action(r, why));
    case AUTO_ERR_NEEDS_CONFIRM:
        return answer_error(http, 409, "needs_confirm", first_action(r, why));
    case AUTO_ERR_UNDOES_ARMING: /* AUT-016, D31: a Disarm after its own partition's arming */
        return answer_error(http, 409, "undoes_arming", first_action(r, why));
    case AUTO_ERR_ARM_ALONE: return answer_error(http, 409, "arm_alone", -1); /* AUT-011 */
    /* BL-224: a wired output that isn't "For automations" (now); Remind or "still armed" after
     * a trigger other than a time of day */
    case AUTO_ERR_OUTPUT: return answer_error(http, 409, "bad_output", first_action(r, why));
    case AUTO_ERR_NEEDS_TIME: return answer_error(http, 409, "needs_time", first_action(r, why));
    case AUTO_ERR_UNSUPPORTED: return answer_error(http, 409, "newer", -1);
    case AUTO_ERR_MAKER: return answer_error(http, 503, "failed", -1);
    default: return answer_error(http, 409, "incomplete", -1); /* partition, time, window */
    }
}

/* An edit's refusal (auto_save_t) as the page's answer. */
static cJSON *save_error(int *http, const auto_rule_t *r, auto_save_t res, auto_check_t why)
{
    switch (res) {
    case AUTO_SAVE_INVALID: return check_error(http, r, why);
    case AUTO_SAVE_CHANGED:
    case AUTO_SAVE_NOT_FOUND: return answer_error(http, 409, "changed", -1);
    case AUTO_SAVE_FULL: return answer_error(http, 409, "full", -1);
    case AUTO_SAVE_NO_ROOM: return answer_error(http, 409, "storage_full", -1);
    case AUTO_SAVE_READ_ONLY: return answer_error(http, 409, "read_only", -1);
    case AUTO_SAVE_NOT_EDITABLE: return answer_error(http, 409, "newer", -1);
    case AUTO_SAVE_LOCKED: return answer_error(http, 409, "locked", -1); /* AUT-010 */
    default: return answer_error(http, 503, "failed", -1);
    }
}

static const auto_rule_t *rule_in_slot(const ctx_t *c, long slot)
{
    for (size_t i = 0; i < c->n_rules; i++) {
        if (c->rules[i].slot == slot) {
            return &c->rules[i];
        }
    }
    return NULL;
}

static cJSON *post_save(ctx_t *c, const cJSON *body, long slot, uint16_t gen, uint8_t user,
                        int *http)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(body, "automation");
    if (!cJSON_IsObject(a)) {
        return answer_error(http, 400, "bad_request", -1);
    }
    auto_rule_t *r = &c->rule;
    memset(r, 0, sizeof(*r));
    if (slot != 0) { /* a change: the rule now in that slot */
        const auto_rule_t *old = rule_in_slot(c, slot);
        if (old == NULL) {
            return answer_error(http, 409, "changed", -1);
        }
        if (old->rec != AUTO_REC_OK) {
            return answer_error(http, 409, "newer", -1);
        }
        r->uid = old->uid;
    }
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(a, "on");
    if (on != NULL && !cJSON_IsNull(on) && !cJSON_IsBool(on)) {
        return answer_error(http, 400, "bad_request", -1);
    }
    r->off = cJSON_IsFalse(on);
    const parse_t pw = parse_when(cJSON_GetObjectItemCaseSensitive(a, "when"), &r->when);
    if (pw == PARSE_BAD) {
        return answer_error(http, 400, "bad_request", -1);
    }
    const cJSON *then = cJSON_GetObjectItemCaseSensitive(a, "then");
    if (!cJSON_IsArray(then) || cJSON_GetArraySize(then) > (int)AUTO_ACTIONS) {
        return answer_error(http, 400, "bad_request", -1);
    }
    /* Every action is read before any 409, so a malformed one is always a 400. */
    const char *act_err = NULL;
    int act_index = -1;
    const cJSON *j;
    cJSON_ArrayForEach(j, then)
    {
        const char *err = NULL;
        const parse_t pa = parse_action(c, j, &r->then[r->n], &err);
        if (pa == PARSE_BAD) {
            return answer_error(http, 400, "bad_request", -1);
        }
        if (pa == PARSE_INCOMPLETE && act_err == NULL) {
            act_err = err;
            act_index = (int)r->n;
        }
        r->n++;
    }
    if (pw == PARSE_INCOMPLETE || r->n == 0u) {
        return answer_error(http, 409, "incomplete", -1);
    }
    if (act_err != NULL) {
        return answer_error(http, 409, act_err, act_index);
    }
    auto_check_t why = AUTO_OK;
    uint16_t uid = 0;
    /* The service checks it against the panel as it is now and makes `user` the maker. */
    const auto_save_t res = automation_service_save(r, user, gen, &why, &uid);
    if (res != AUTO_SAVE_OK) {
        return save_error(http, r, res, why);
    }
    auto_rule_t *saved = &c->rule; /* reused: the request's rule is no longer needed */
    cJSON *o = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(o, "result", "saved") != NULL;
    if (automation_service_get(uid, saved)) {
        ok &= cJSON_AddNumberToObject(o, "slot", (double)saved->slot) != NULL;
    } else {
        /* gone already (another edit): the page reads the list again */
        ok &= cJSON_AddNullToObject(o, "slot") != NULL;
    }
    ok &= cJSON_AddNumberToObject(o, "gen", automation_service_gen()) != NULL;
    return built(o, ok); /* M01: a second press meets "changed", nothing saved twice */
}

static cJSON *post_try(ctx_t *c, const auto_rule_t *r, uint16_t gen, int *http)
{
    if (gen != automation_service_gen()) {
        return answer_error(http, 409, "changed", -1);
    }
    if (r->rec != AUTO_REC_OK) {
        return answer_error(http, 409, "newer", -1);
    }
    if (has_untried_action(r)) { /* nothing arms without its warning; the page offers no Try */
        return answer_error(http, 409, "action_not_allowed", 0);
    }
    /* M01: the answer exists before the action runs. Once it ran, "Memory busy" would invite a
     * second press that runs it again (a try doesn't change gen): "tried" goes out even when its
     * gen or status could not be added (the page reads the list again). */
    cJSON *o = cJSON_CreateObject();
    if (cJSON_AddStringToObject(o, "result", "tried") == NULL) {
        cJSON_Delete(o);
        return NULL; /* "Memory busy", nothing tried */
    }
    /* Waits 100 ms at most for the task to take it; once taken, auto_run_try never blocks. */
    const auto_try_t t = automation_service_try(r->uid);
    if (t != AUTO_TRY_OK) {
        cJSON_Delete(o);
    }
    switch (t) {
    case AUTO_TRY_OK: break;
    case AUTO_TRY_UNKNOWN: return answer_error(http, 409, "changed", -1);
    case AUTO_TRY_ARM: return answer_error(http, 409, "action_not_allowed", 0);
    case AUTO_TRY_INVALID: {
        /* The runner checks as if the option were on (auto_run.h): say what fails now. */
        auto_env_t env;
        if (!automation_service_env(&env, c->zones)) {
            return answer_error(http, 503, "failed", -1);
        }
        env.less_secure = true;
        const auto_check_t why = auto_rule_check(r, &env);
        return why == AUTO_OK ? answer_error(http, 409, "incomplete", -1)
                              : check_error(http, r, why);
    }
    default: return answer_error(http, 503, "busy", -1); /* the task didn't take it: try again */
    }
    cJSON_AddNumberToObject(o, "gen", automation_service_gen());
    (void)web_attach(o, "status", status_json(c, r));
    return o;
}

cJSON *web_automations_post(const cJSON *body, uint8_t user, int *http)
{
    *http = 200;
    const char *action = get_string(body, "action");
    long slot, gen;
    if (action == NULL || !get_number(body, "slot", 0, AUTO_MAX, 0, &slot) ||
        !get_number(body, "gen", 0, UINT16_MAX, -1, &gen)) {
        return answer_error(http, 400, "bad_request", -1);
    }
    const bool save = !strcmp(action, "save");
    if (!save && strcmp(action, "switch") && strcmp(action, "delete") && strcmp(action, "try")) {
        return answer_error(http, 400, "bad_request", -1);
    }
    if (!save && slot == 0) {
        return answer_error(http, 400, "bad_request", -1);
    }
    ctx_t *c = ctx_new();
    if (c == NULL) {
        *http = 503;
        return NULL;
    }
    read_devices(c);
    c->n_rules = automation_service_list(c->rules, AUTO_MAX);
    cJSON *o = NULL;
    if (save) {
        o = post_save(c, body, slot, (uint16_t)gen, user, http);
    } else if (!strcmp(action, "delete")) {
        /* By slot: a broken, bad or newer record may have no uid to name it by (AUT-008). */
        const auto_save_t res = automation_service_delete_slot((uint8_t)slot, (uint16_t)gen);
        if (res != AUTO_SAVE_OK) {
            o = save_error(http, &c->rule, res, AUTO_OK);
        } else {
            o = cJSON_CreateObject();
            bool ok = cJSON_AddStringToObject(o, "result", "deleted") != NULL;
            ok &= cJSON_AddNumberToObject(o, "gen", automation_service_gen()) != NULL;
            o = built(o, ok);
        }
    } else {
        const auto_rule_t *r = rule_in_slot(c, slot);
        if (r == NULL) {
            o = answer_error(http, 409, "changed", -1);
        } else if (!strcmp(action, "try")) {
            o = post_try(c, r, (uint16_t)gen, http);
        } else { /* switch: `on` beside the action (or in `automation`) */
            const cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
            if (on == NULL) {
                on = cJSON_GetObjectItemCaseSensitive(
                    cJSON_GetObjectItemCaseSensitive(body, "automation"), "on");
            }
            if (!cJSON_IsBool(on)) {
                o = answer_error(http, 400, "bad_request", -1);
            } else if (r->rec != AUTO_REC_OK) {
                o = answer_error(http, 409, "newer", -1);
            } else {
                const auto_save_t res =
                    automation_service_set_off(r->uid, !cJSON_IsTrue(on), (uint16_t)gen);
                if (res != AUTO_SAVE_OK) {
                    o = save_error(http, r, res, AUTO_OK);
                } else {
                    o = cJSON_CreateObject();
                    bool ok = cJSON_AddStringToObject(o, "result", "switched") != NULL;
                    ok &= cJSON_AddNumberToObject(o, "gen", automation_service_gen()) != NULL;
                    o = built(o, ok);
                }
            }
        }
    }
    ctx_free(c);
    if (o == NULL) {
        *http = 503;
    }
    return o;
}

#else /* !CONFIG_ALARM_FEATURE_AUTOMATIONS: panel_web answers 404 to the three paths */

bool web_automations_available(void)
{
    return false;
}

bool web_automations_add_warnings(cJSON *o, const char *key, const alarm_config_t *cfg,
                                  uint8_t partitions)
{
    (void)o;
    (void)key;
    (void)cfg;
    (void)partitions;
    return true;
}

bool web_automations_add_reminders(cJSON *o, const char *key, uint8_t partitions)
{
    (void)o;
    (void)key;
    (void)partitions;
    return true;
}

cJSON *web_automations_get(void)
{
    return NULL;
}

cJSON *web_automations_devices(void)
{
    return NULL;
}

cJSON *web_automations_post(const cJSON *body, uint8_t user, int *http)
{
    (void)body;
    (void)user;
    *http = 404;
    return NULL;
}

#endif /* CONFIG_ALARM_FEATURE_AUTOMATIONS */
