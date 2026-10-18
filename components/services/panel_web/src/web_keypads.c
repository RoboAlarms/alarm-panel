/* BL-048: Panel Manager and future app use this same authenticated service, including
 * on a master without a display. Pairing still requires the two physical code comparisons. */
#include "web_keypads.h"
#include "alarm_service.h"
#include "esp_timer.h"
#include "nvs.h"
#include "panel_prefs.h"
#include "sdkconfig.h"
#include "ui_backend/ui_backend.h"
#include "web_mem.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if CONFIG_ALARM_LINK
#include "plink_service.h"
#endif

static const alarm_source_t SOURCE = {.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_PANEL_MANAGER};
/* Appended UI preference: the built-in keypad's browser label, independent of partition names
 * and every sound/display preference. Existing panel_prefs writers leave this key alone. */
#if CONFIG_ALARM_FEATURE_UI
static void local_name(char out[ALARM_NAME_LEN])
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t len = ALARM_NAME_LEN;
    if (nvs_get_str(h, "keypad_name", out, &len) != ESP_OK)
        out[0] = '\0';
    nvs_close(h);
}
#endif
static bool local_name_save(const char *name)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK)
        return false;
    const bool ok = nvs_set_str(h, "keypad_name", name) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
static struct {
    char id[33];
    uint8_t owner;
    bool active;
    int64_t until;
    ui_link_pair_t pair;
} attempt;

static const char *word(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool integer(const cJSON *o, const char *key, unsigned low, unsigned high, unsigned *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= low && v->valuedouble <= high) ||
        floor(v->valuedouble) != v->valuedouble)
        return false;
    *out = (unsigned)v->valuedouble;
    return true;
}
static bool begin(uint8_t user)
{
    alarm_snapshot_t *s = web_alloc(sizeof *s);
    if (!s)
        return false;
    alarm_service_get_snapshot(s);
    const bool busy = s->program;
    free(s);
    return !busy && alarm_service_program_enter_as(&SOURCE, user) == ALARM_OK;
}
void web_keypads_tick(void *unused)
{
    (void)unused;
#if CONFIG_ALARM_LINK
    if (!attempt.active)
        return;
    ui_link_pair_t p = {0};
    plink_service_pair_status(NULL, &p);
    /* Never extend the server's deadline through browser polling. */
    if (esp_timer_get_time() >= attempt.until && p.state != UI_LINK_PAIR_DONE &&
        p.state != UI_LINK_PAIR_FAILED) {
        const ui_link_req_t r = {.kind = UI_LINK_REQ_PAIR_STOP};
        plink_service_manage(NULL, &r);
        plink_service_pair_status(NULL, &p);
        if (p.state != UI_LINK_PAIR_DONE) {
            p.state = UI_LINK_PAIR_FAILED;
            p.fail = UI_LINK_FAIL_TIMEOUT;
        }
    }
    attempt.pair = p;
    if (p.state == UI_LINK_PAIR_DONE || p.state == UI_LINK_PAIR_FAILED ||
        p.state == UI_LINK_PAIR_IDLE) {
        attempt.active = false;
        alarm_service_program_exit_owned(&SOURCE);
    }
#endif
}
#if CONFIG_ALARM_FEATURE_UI || CONFIG_ALARM_LINK
static bool policy(cJSON *o, unsigned home, bool allowed, bool never, unsigned seconds)
{
    bool ok = cJSON_AddNumberToObject(o, "home", home) != NULL;
    ok &= cJSON_AddBoolToObject(o, "allowed", allowed) != NULL;
    ok &= cJSON_AddBoolToObject(o, "never", never) != NULL;
    ok &= cJSON_AddNumberToObject(o, "seconds", seconds ? seconds : 30u) != NULL;
    return ok;
}
#endif
cJSON *web_keypads_get(uint8_t user)
{
    web_keypads_tick(NULL);
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    bool ok = true;
    ok &= cJSON_AddBoolToObject(o, "available", false) != NULL;
    ok &= cJSON_AddNumberToObject(o, "capacity", 8) != NULL;
    cJSON *rows = cJSON_AddArrayToObject(o, "keypads");
    if (!rows) {
        cJSON_Delete(o);
        return NULL;
    }
#if CONFIG_ALARM_FEATURE_UI
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);
    cJSON *local = cJSON_AddObjectToObject(o, "local");
    char name[ALARM_NAME_LEN];
    local_name(name);
    ok &= cJSON_AddStringToObject(local, "name", name) != NULL;
    ok &= policy(local, prefs.partition_home, prefs.switch_allowed, prefs.return_never,
                 prefs.return_s);
#endif
#if CONFIG_ALARM_LINK
    const bool available = plink_service_role() == PLINK_SERVICE_MASTER;
    cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "available");
    if (a)
        a->type = available ? cJSON_True : cJSON_False;
    ui_link_keypad_t *k = web_alloc(8 * sizeof *k);
    if (!k) {
        cJSON_Delete(o);
        return NULL;
    }
    const size_t count = available ? plink_service_managed_keypads(NULL, k, 8) : 0;
    for (size_t i = 0; i < count; i++) {
        cJSON *v = cJSON_CreateObject();
        if (!v || !cJSON_AddItemToArray(rows, v)) {
            cJSON_Delete(v);
            free(k);
            cJSON_Delete(o);
            return NULL;
        }
        ok &= cJSON_AddNumberToObject(v, "number", k[i].device) != NULL;
        ok &= cJSON_AddStringToObject(v, "name", k[i].name) != NULL;
        ok &= cJSON_AddBoolToObject(v, "online", k[i].connected) != NULL;
        ok &= cJSON_AddBoolToObject(v, "waiting", k[i].waiting) != NULL;
        ok &= cJSON_AddStringToObject(v, "firmware", k[i].fw) != NULL;
        ok &= cJSON_AddNumberToObject(v, "update_state", k[i].fw_state) != NULL;
        ok &= cJSON_AddNumberToObject(v, "update_percent", k[i].fw_percent) != NULL;
        ok &= policy(v, k[i].home, k[i].switch_allowed, k[i].return_never, k[i].return_s);
    }
    free(k);
#endif
    ok &= cJSON_AddBoolToObject(o, "busy", attempt.active) != NULL;
    if (attempt.owner == user && attempt.id[0]) {
        cJSON *p = cJSON_AddObjectToObject(o, "enrollment");
        ok &= cJSON_AddStringToObject(p, "attempt", attempt.id) != NULL;
        ok &= cJSON_AddNumberToObject(p, "state", attempt.pair.state) != NULL;
        ok &= cJSON_AddNumberToObject(p, "fail", attempt.pair.fail) != NULL;
        ok &= cJSON_AddNumberToObject(p, "left_s", attempt.pair.left_s) != NULL;
        ok &= cJSON_AddNumberToObject(p, "sequence", attempt.pair.attempts) != NULL;
        ok &= cJSON_AddStringToObject(p, "code", attempt.pair.code) != NULL;
        ok &= cJSON_AddStringToObject(p, "peer", attempt.pair.peer_name) != NULL;
        ok &= cJSON_AddNumberToObject(p, "device", attempt.pair.device) != NULL;
    }
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
const char *web_keypads_post(uint8_t user, const cJSON *o)
{
    web_keypads_tick(NULL);
    const char *op = word(o, "op"), *id = word(o, "attempt");
#if CONFIG_ALARM_LINK
    if (!strcmp(op, "start")) {
        if (strlen(id) != 32 || strspn(id, "0123456789abcdef") != 32)
            return "Invalid attempt";
        if (attempt.owner == user && !strcmp(attempt.id, id))
            return NULL; /* retry */
        if (attempt.active || plink_service_role() != PLINK_SERVICE_MASTER)
            return "Pairing is busy";
        if (!begin(user))
            return "Installer mode is busy or the system is armed";
        ui_link_req_t r = {.kind = UI_LINK_REQ_PAIR_OPEN};
        const uint8_t result = plink_service_manage(NULL, &r);
        if (result != UI_LINK_REQ_OK) {
            alarm_service_program_exit_owned(&SOURCE);
            return "Pairing could not start; check capacity and try again";
        }
        memset(&attempt, 0, sizeof attempt);
        snprintf(attempt.id, sizeof attempt.id, "%s", id);
        attempt.owner = user;
        attempt.active = true;
        attempt.until = esp_timer_get_time() + 120000000;
        attempt.pair.state = UI_LINK_PAIR_OPEN;
        return NULL;
    }
    if (!strcmp(op, "confirm") || !strcmp(op, "cancel")) {
        if (!id[0] || attempt.owner != user || strcmp(attempt.id, id))
            return "Pairing attempt changed; reload";
        if (!attempt.active)
            return NULL;
        ui_link_req_t r = {.kind = UI_LINK_REQ_PAIR_STOP};
        if (!strcmp(op, "confirm")) {
            const cJSON *allow = cJSON_GetObjectItemCaseSensitive(o, "allow");
            unsigned sequence;
            if (!integer(o, "sequence", 0, 255, &sequence) || sequence != attempt.pair.attempts)
                return "Pairing attempt changed; compare again";
            if (!cJSON_IsBool(allow) || !attempt.pair.code[0] ||
                strcmp(word(o, "code"), attempt.pair.code))
                return "Pairing code changed; compare again";
            if (attempt.pair.state == UI_LINK_PAIR_WAITING && cJSON_IsTrue(allow))
                return NULL;
            if (attempt.pair.state != UI_LINK_PAIR_COMPARE &&
                attempt.pair.state != UI_LINK_PAIR_WAITING)
                return "Pairing is not ready";
            r.kind = attempt.pair.state == UI_LINK_PAIR_WAITING ? UI_LINK_REQ_PAIR_STOP
                                                                : UI_LINK_REQ_PAIR_DECIDE;
            r.allow = cJSON_IsTrue(allow);
            snprintf(r.pair_code, sizeof r.pair_code, "%s", attempt.pair.code);
            r.pair_attempts = attempt.pair.attempts;
        }
        if (plink_service_manage(NULL, &r) != UI_LINK_REQ_OK)
            return "Pairing request refused";
        if (r.kind == UI_LINK_REQ_PAIR_STOP) {
            /* A peer may have finished just before cancel reached the link task. Read its
             * actual terminal result; cancellation must not conceal an enrolled keypad. */
            web_keypads_tick(NULL);
        }
        return NULL;
    }
#else
    (void)id;
#endif
    if (strcmp(op, "save") && strcmp(op, "remove"))
        return "Unknown keypad operation";
    if (attempt.active)
        return "Finish pairing first";
    unsigned device, home = 0, seconds = 0;
    if (!integer(o, "device", 0, 8, &device))
        return "Invalid keypad";
    const bool remove = !strcmp(op, "remove");
    const char *name = word(o, "name");
    if (!remove && (!name[0] || strlen(name) >= ALARM_NAME_LEN))
        return "Invalid keypad name";
    const cJSON *allow = cJSON_GetObjectItemCaseSensitive(o, "allowed"),
                *never = cJSON_GetObjectItemCaseSensitive(o, "never");
    if (!remove &&
        (!integer(o, "home", 1, ALARM_MAX_PARTITIONS, &home) ||
         !integer(o, "seconds", 1, 3600, &seconds) || !cJSON_IsBool(allow) || !cJSON_IsBool(never)))
        return "Invalid keypad policy";
    if (device == 0 && remove)
        return "This panel has no removable built-in keypad";
#if !CONFIG_ALARM_FEATURE_UI
    if (device == 0)
        return "This panel has no built-in keypad";
#endif
    if (!begin(user))
        return "Installer mode is busy or the system is armed";
    const char *error = NULL;
    alarm_config_t *cfg = web_alloc(sizeof *cfg);
    if (!cfg || alarm_service_get_config(cfg) != ESP_OK ||
        (!remove && !cfg->partitions[home - 1].enabled))
        error = "Partition is not available";
    if (cfg) {
        memset(cfg, 0, sizeof *cfg);
        free(cfg);
    }
    if (!error && device == 0) {
        panel_prefs_t p;
        panel_prefs_load(&p);
        p.partition_home = (uint8_t)home;
        p.switch_allowed = cJSON_IsTrue(allow);
        p.return_never = cJSON_IsTrue(never);
        p.return_s = (uint16_t)seconds;
        if (panel_prefs_save(&p) != ESP_OK)
            error = "Could not persist preferences";
        else if (!local_name_save(name))
            error = "Could not persist keypad name; reload before retrying";
    }
#if CONFIG_ALARM_LINK
    if (!error && device != 0) {
        ui_link_keypad_t rows[8];
        size_t n = plink_service_managed_keypads(NULL, rows, 8), i = 0;
        while (i < n && rows[i].device != device)
            i++;
        if (i == n)
            error = "Keypad no longer enrolled";
        else if (!remove && !rows[i].connected && !rows[i].waiting)
            error = "Keypad is not responding";
        else {
            ui_link_req_t r = {.kind = remove ? UI_LINK_REQ_KEYPAD_REMOVE : UI_LINK_REQ_KEYPAD_SET,
                               .device = (uint8_t)device,
                               .home = (uint8_t)home,
                               .supervision_s = rows[i].supervision_s};
            if (!remove && (!name[0] || strlen(name) >= ALARM_NAME_LEN))
                error = "Invalid keypad name";
            else {
                snprintf(r.name, sizeof r.name, "%s", name);
                if (plink_service_manage(NULL, &r) != UI_LINK_REQ_OK)
                    error = "Keypad change failed; reload before retrying";
                if (!error && !remove) {
                    r.kind = UI_LINK_REQ_PARTITION_POLICY;
                    r.switch_allowed = cJSON_IsTrue(allow);
                    r.return_never = cJSON_IsTrue(never);
                    r.return_s = (uint16_t)seconds;
                    if (plink_service_manage(NULL, &r) != UI_LINK_REQ_OK)
                        error = "Keypad policy failed; reload before retrying";
                }
            }
        }
    }
#else
    if (device != 0)
        error = "Keypad link is unavailable";
#endif
    alarm_service_program_exit_owned(&SOURCE);
    return error;
}
bool web_keypads_config_valid(const cJSON *changes, const alarm_config_t *cfg)
{
    for (unsigned i = 0; i < ALARM_MAX_DEVICES; i++) {
        const alarm_device_config_t *d = &cfg->devices[i];
        if (d->enabled && d->partition &&
            (d->partition > ALARM_MAX_PARTITIONS || !cfg->partitions[d->partition - 1].enabled))
            return false;
    }
#if CONFIG_ALARM_FEATURE_UI
    panel_prefs_t p;
    panel_prefs_load(&p);
    if (p.partition_home < 1 || p.partition_home > ALARM_MAX_PARTITIONS ||
        !cfg->partitions[p.partition_home - 1].enabled)
        return false;
#endif
#if CONFIG_ALARM_LINK
    ui_link_keypad_t rows[8];
    size_t n = plink_service_managed_keypads(NULL, rows, 8);
    for (size_t i = 0; i < n; i++) {
        if (rows[i].home < 1 || rows[i].home > ALARM_MAX_PARTITIONS ||
            !cfg->partitions[rows[i].home - 1].enabled)
            return false;
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(changes, "devices"))
        {
            unsigned d;
            if (integer(v, "number", 1, ALARM_MAX_DEVICES, &d) && d == rows[i].device)
                return false;
        }
    }
#else
    (void)changes;
#endif
    return true;
}
