/* Hardware/services configuration kept separate from HTTP and authentication. */
#include "web_extra.h"
#include "ui_backend/ui_check_words.h"
#include "ui_backend/ui_why_words.h"
#include "alarm/alarm_strings.h"
#include "alarm_service.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "net_mqtt.h"
#include "net_notify.h"
#include "net_wifi.h"
#include "notify/notify_words.h"
#include "panel_log.h"
#include "panel_storage.h"
#include "panel_system.h"
#include "sdkconfig.h"
#include "web_mem.h"
#include "web_rules.h"
#include "web_voice.h"
#include "zone_service.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const alarm_source_t SOURCE = {.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_PANEL_MANAGER};

/* ---- the wrong-guess limiter (see web_extra.h) --------------------------------------------- */
#define GATE_FREE_TRIES 5u
#define GATE_HOLD_US    (60LL * 1000000)
#define GATE_HOLD_MAX   (15LL * 60 * 1000000)
#define GATE_FORGET_US  (60LL * 60 * 1000000)

bool web_gate_open(web_gate_t *g)
{
    const int64_t now = esp_timer_get_time();
    if (g->fails && now - g->last_fail_us > GATE_FORGET_US)
        g->fails = 0;
    return now >= g->hold_until_us;
}

void web_gate_failed(web_gate_t *g)
{
    const int64_t now = esp_timer_get_time();
    g->last_fail_us = now;
    if (++g->fails >= GATE_FREE_TRIES) {
        const unsigned over = g->fails - GATE_FREE_TRIES;
        int64_t hold = GATE_HOLD_US << (over > 4u ? 4u : over);
        if (hold > GATE_HOLD_MAX)
            hold = GATE_HOLD_MAX;
        g->hold_until_us = now + hold;
    }
}

static cJSON *answer(bool ok, const char *why)
{
    cJSON *o = cJSON_CreateObject();
    if (ok)
        cJSON_AddBoolToObject(o, "ok", true);
    else
        cJSON_AddStringToObject(o, "error", why);
    return o;
}
static const char *str(const cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool num(const cJSON *o, const char *key, double low, double high, int64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        floor(v->valuedouble) != v->valuedouble || v->valuedouble < low || v->valuedouble > high)
        return false;
    *out = (int64_t)v->valuedouble;
    return true;
}
static bool indexed(const char *path, const char *prefix, unsigned max, unsigned *n)
{
    size_t len = strlen(prefix);
    if (strncmp(path, prefix, len) || !path[len])
        return false;
    char *end;
    unsigned long value = strtoul(path + len, &end, 10);
    if (*end || value < 1 || value > max)
        return false;
    *n = (unsigned)value;
    return true;
}
/* A builder's document, or "Memory busy" when one of its parts could not be attached
 * (web_attach(), code review M01): never a partial answer. */
static cJSON *built(cJSON *o, bool ok)
{
    if (ok && o)
        return o;
    cJSON_Delete(o);
    return answer(false, "Memory busy");
}
/* A sensor's address, or NULL when any of it could not be made: a binding is posted back as
 * read, so a half address would name another sensor (M01). */
static cJSON *address(const zsrc_address_t *a)
{
    cJSON *o = cJSON_CreateObject();
    char id[24];
    snprintf(id, sizeof(id), "%" PRIu64, a->id);
    if (!o || !cJSON_AddNumberToObject(o, "kind", a->kind) ||
        !cJSON_AddNumberToObject(o, "input", a->input) || !cJSON_AddStringToObject(o, "id", id) ||
        !cJSON_AddStringToObject(o, "ref", a->ref)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
/* An input's mapping, or NULL when any of it could not be made (as address()). */
static cJSON *mapping(const zsrc_mapping_t *m)
{
    cJSON *o = cJSON_CreateObject();
    if (!o || !cJSON_AddNumberToObject(o, "mode", m->mode) ||
        !cJSON_AddBoolToObject(o, "invert", m->invert) ||
        !cJSON_AddNumberToObject(o, "threshold_x100", m->threshold_x100) ||
        !cJSON_AddNumberToObject(o, "hysteresis_x100", m->hysteresis_x100) ||
        !cJSON_AddStringToObject(o, "states", m->states) ||
        !cJSON_AddNumberToObject(o, "grace_s", m->grace_s) ||
        !cJSON_AddNumberToObject(o, "response_ms", m->response_ms) ||
        !cJSON_AddNumberToObject(o, "hold_s", m->hold_s)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
static cJSON *candidates(uint8_t kind, const char *query)
{
    zsrc_candidate_t *list = web_alloc(32 * sizeof(*list));
    if (!list)
        return answer(false, "Memory busy");
    size_t count = zone_service_candidates(kind, query, 0, list, 32);
    cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
    bool ok = true;
    for (size_t i = 0; ok && i < count && i < 32; i++) {
        cJSON *v = cJSON_CreateObject();
        ok &= web_attach(v, "address", address(&list[i].addr));
        /* every key or none (M01): a dropped "unusable" would read as false and offer a code the
         * source advises against */
        ok &= v != NULL && cJSON_AddStringToObject(v, "name", list[i].name) != NULL;
        ok &= v != NULL && cJSON_AddStringToObject(v, "detail", list[i].detail) != NULL;
        /* the source's words: open/closed for a contact, how long ago for a 433 MHz sensor */
        ok &= v != NULL && cJSON_AddStringToObject(v, "state", list[i].state) != NULL;
        ok &= v != NULL && cJSON_AddNumberToObject(v, "zone", list[i].zone) != NULL;
        ok &= v != NULL && cJSON_AddNumberToObject(v, "device_class", list[i].device_class) != NULL;
        /* What the source knows about the one being tested, as the touchscreen's picker reads
         * it (code review RF24): changed while learning, the one it is sure of (RF-011),
         * advised against for this input - detail says why (RF-021). New keys only. */
        ok &= v != NULL && cJSON_AddBoolToObject(v, "changed", list[i].changed) != NULL;
        ok &= v != NULL && cJSON_AddBoolToObject(v, "best", list[i].best) != NULL;
        ok &= v != NULL && cJSON_AddBoolToObject(v, "unusable", list[i].unusable) != NULL;
        ok &= web_attach(a, NULL, v);
    }
    free(list);
    ok &= web_attach(o, "candidates", a);
    return built(o, ok);
}
/* How a zone takes part in arming (WEB-024), for the Overview's arming choices: the levels that
 * arm it, whether it is on all the time, the entry time it starts, whether it may be bypassed. */
static bool zone_arming(cJSON *v, const alarm_zone_config_t *zc, const alarm_partition_config_t *pc)
{
    static const alarm_arm_level_t LEVELS[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY, ALARM_LEVEL_NIGHT};
    cJSON *levels = cJSON_CreateArray();
    bool ok = true;
    for (size_t l = 0; l < sizeof(LEVELS) / sizeof(LEVELS[0]); l++)
        if (alarm_zone_armed_in_level(zc, LEVELS[l]))
            ok &= web_attach(levels, NULL, cJSON_CreateString(alarm_level_name(LEVELS[l])));
    ok &= web_attach(v, "levels", levels);
    /* 24-hour, fire, CO and monitor zones: armed whatever the level ("always on"). */
    const alarm_zone_type_t t = zc->type;
    cJSON_AddBoolToObject(v, "always", alarm_zone_type_is_24h(t) || alarm_zone_type_is_fire(t) ||
                                           t == ALARM_ZT_CO || t == ALARM_ZT_MONITOR);
    const unsigned entry = t == ALARM_ZT_ENTRY_EXIT_1 || t == ALARM_ZT_INTERIOR_DELAY
                               ? pc->entry_delay1_s
                           : t == ALARM_ZT_ENTRY_EXIT_2 ? pc->entry_delay2_s
                                                        : 0u;
    cJSON_AddNumberToObject(v, "entry_s", entry);
    cJSON_AddBoolToObject(v, "bypassable", alarm_zone_bypassable(zc));
    return ok;
}
/* ---- the Troubles list (UI-030, BL-137) ---------------------------------------------------- */
#define TROUBLE_SCAN  256u /* newest log slots looked at for "since" */
#define TROUBLE_KEEP  64u  /* trouble events kept out of them */

static bool trouble_event(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_EVENT &&
           (e->ev.type == ALARM_EV_TROUBLE || e->ev.type == ALARM_EV_TROUBLE_RESTORE);
}
/* The newest event of this trouble (entries are newest first); the kind of a restored zone
 * trouble comes from it, the snapshot doesn't say. NULL when the scan didn't reach one. */
static const alarm_event_t *trouble_find(const elog_entry_t *log, size_t n,
                                         const alarm_trouble_item_t *t)
{
    for (size_t i = 0; i < n; i++) {
        const alarm_event_t *e = &log[i].ev;
        if (e->type != (t->restored ? ALARM_EV_TROUBLE_RESTORE : ALARM_EV_TROUBLE) ||
            e->arg2 != t->family || e->zone != t->number)
            continue;
        if (e->arg == t->kind || (t->restored && t->family == ALARM_TRBL_FAMILY_ZONE))
            return e;
    }
    return NULL;
}
static cJSON *troubles_list(uint8_t partitions)
{
    alarm_snapshot_t *s = web_alloc(sizeof(*s));
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (!s || !cfg || alarm_service_get_config(cfg) != ESP_OK) {
        free(s);
        free(cfg);
        return answer(false, "Memory busy");
    }
    alarm_service_get_snapshot(s);
    const size_t count = alarm_snapshot_troubles(s, cfg, partitions, NULL, 0);
    alarm_trouble_item_t *items = count ? web_alloc(count * sizeof(*items)) : NULL;
    if (count && !items) { /* never an empty list for want of memory: it would read "nothing to fix" */
        free(s);
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
        return answer(false, "Memory busy");
    }
    elog_entry_t *log = web_alloc(TROUBLE_KEEP * sizeof(*log));
    cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
    if (items && count)
        alarm_snapshot_troubles(s, cfg, partitions, items, count);
    /* the same fingerprint as status's trouble_sig: the page reloads when they differ */
    cJSON_AddNumberToObject(o, "sig", (double)alarm_snapshot_troubles_sig(s, cfg, partitions));
    size_t n = 0;
    bool ok = true;
    if (items && log && panel_log_ready()) {
        uint32_t next;
        n = panel_log_read(UINT32_MAX, trouble_event, NULL, log, TROUBLE_KEEP, TROUBLE_SCAN, &next);
    }
    for (size_t i = 0; items && i < count; i++) {
        const alarm_event_t *e = trouble_find(log, n, &items[i]);
        cJSON *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "family",
                                items[i].family == ALARM_TRBL_FAMILY_SYSTEM ? "system"
                                : items[i].family == ALARM_TRBL_FAMILY_DEVICE ? "keypad"
                                                                              : "zone");
        cJSON_AddNumberToObject(v, "kind", e && items[i].restored &&
                                                   items[i].family == ALARM_TRBL_FAMILY_ZONE
                                               ? e->arg
                                               : items[i].kind);
        cJSON_AddNumberToObject(v, "number", items[i].number);
        cJSON_AddBoolToObject(v, "restored", items[i].restored);
        cJSON_AddNumberToObject(v, "since_ms", e && e->epoch_ms > 0 ? (double)e->epoch_ms : 0);
        ok &= web_attach(a, NULL, v);
    }
    free(items);
    free(log);
    free(s);
    mbedtls_platform_zeroize(cfg, sizeof(*cfg)); /* it holds the code hashes too */
    free(cfg);
    ok &= web_attach(o, "troubles", a);
    return built(o, ok); /* never a short list: it would read "nothing to fix" */
}
/* Every zone with a sensor bound and where it is (the zone page's "Sensor" column). */
static cJSON *zone_sources(void)
{
    cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (!cfg || alarm_service_get_config(cfg) != ESP_OK) {
        free(cfg);
        cJSON_Delete(o);
        cJSON_Delete(a);
        return answer(false, "Memory busy");
    }
    bool ok = true;
    for (uint16_t z = 1; ok && z <= ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t binding = {0};
        if (cfg->zones[z - 1].type == ALARM_ZT_NOT_USED || !zone_service_get(z, &binding) ||
            binding.in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_NONE)
            continue;
        cJSON *v = cJSON_CreateObject();
        cJSON_AddNumberToObject(v, "zone", z);
        ok &= web_attach(v, "address", address(&binding.in[ZSRC_ROLE_MAIN].addr));
        ok &= web_attach(a, NULL, v);
    }
    mbedtls_platform_zeroize(cfg, sizeof(*cfg));
    free(cfg);
    ok &= web_attach(o, "zones", a);
    return built(o, ok);
}
cJSON *web_extra_get(const ui_backend_t *b, const char *path, uint8_t partitions,
                     const i18n_catalog_t *words_in)
{
    if (!strcmp(path, "zone-status")) {
        /* What the Zones view of a keypad shows anyone (no code): each zone of the viewer's
         * partitions with its name, sensor kind and the panel's own words for its state. */
        alarm_snapshot_t *s = web_alloc(sizeof(*s));
        alarm_config_t *cfg = web_alloc(sizeof(*cfg));
        if (!s || !cfg || alarm_service_get_config(cfg) != ESP_OK) {
            free(s);
            free(cfg);
            return answer(false, "Memory busy");
        }
        alarm_service_get_snapshot(s);
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        web_voice_zones_t voice; /* each zone's spoken name, read-only (D14) */
        web_voice_zones_begin(&voice);
        bool ok = true;
        for (uint16_t i = 1; ok && i <= s->zones_used && i <= ALARM_MAX_ZONES; i++) {
            const alarm_zone_config_t *zc = &cfg->zones[i - 1];
            if (zc->type == ALARM_ZT_NOT_USED || zc->partition < 1 ||
                zc->partition > ALARM_MAX_PARTITIONS || !(partitions & (1u << (zc->partition - 1))))
                continue;
            zone_hub_info_t z = {0};
            bool bound = zone_service_info(i, &z);
            char words[48];
            const notify_accent_t accent =
                notify_zone_state(words_in, cfg, i, &s->zone[i - 1], words, sizeof(words));
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "zone", i);
            cJSON_AddStringToObject(v, "name", zc->name);
            cJSON_AddNumberToObject(v, "device_class", zc->device_class);
            cJSON_AddNumberToObject(v, "partition", zc->partition);
            cJSON_AddStringToObject(v, "words", words);
            cJSON_AddNumberToObject(v, "accent", accent);
            cJSON_AddNumberToObject(v, "flags", s->zone[i - 1].flags);
            cJSON_AddNumberToObject(v, "trouble", s->zone[i - 1].trouble);
            cJSON_AddBoolToObject(v, "bound", bound);
            cJSON_AddBoolToObject(v, "known", bound && z.known);
            cJSON_AddBoolToObject(v, "active", bound && z.active);
            cJSON_AddBoolToObject(v, "unreachable", bound && z.unreachable);
            cJSON_AddBoolToObject(v, "check", bound && z.check);
            cJSON_AddNumberToObject(v, "heard_at_ms", bound ? (double)z.heard_at : 0);
            if (bound && z.has_value)
                cJSON_AddNumberToObject(v, "value_x100", z.value_x100);
            ok &= zone_arming(v, zc, &cfg->partitions[zc->partition - 1]);
            ok &= web_voice_zone(&voice, zc, v);
            ok &= web_attach(a, NULL, v);
        }
        web_voice_zones_end(&voice);
        free(s);
        mbedtls_platform_zeroize(cfg, sizeof(*cfg)); /* it holds the code hashes too */
        free(cfg);
        ok &= web_attach(o, "zones", a);
        return built(o, ok);
    }
    if (!strcmp(path, "troubles"))
        return troubles_list(partitions);
    if (!strcmp(path, "zone-sources"))
        return zone_sources();
    if (!strcmp(path, "storage")) {
        storage_sd_t sd;
        storage_flash_t flash;
        b->storage_sd(b->ctx, &sd);
        b->storage_flash(b->ctx, &flash);
        cJSON *o = cJSON_CreateObject(), *s = cJSON_CreateObject(), *f = cJSON_CreateObject(),
              *a = cJSON_CreateArray();
        cJSON_AddStringToObject(s, "state", storage_state_name(sd.state)); /* a key: English */
        cJSON_AddStringToObject(s, "words",
                                i18n_text(words_in, "storage state", storage_state_name(sd.state)));
        cJSON_AddStringToObject(s, "name", sd.name);
        cJSON_AddStringToObject(s, "filesystem", sd.filesystem);
        cJSON_AddStringToObject(s, "message", i18n_text(words_in, "storage", sd.message));
        cJSON_AddNumberToObject(s, "generation", sd.generation);
        cJSON_AddNumberToObject(s, "capacity_bytes", (double)sd.capacity_bytes);
        cJSON_AddNumberToObject(s, "free_bytes", (double)sd.free_bytes);
        cJSON_AddBoolToObject(s, "usage_valid", sd.usage_valid);
        cJSON_AddStringToObject(f, "running_slot", flash.running_slot);
        cJSON_AddNumberToObject(f, "chip_bytes", flash.chip_bytes);
        cJSON_AddNumberToObject(f, "image_bytes", flash.image_bytes);
        cJSON_AddNumberToObject(f, "slot_bytes", flash.slot_bytes);
        cJSON_AddNumberToObject(f, "update_bytes", flash.update_bytes);
        bool ok = true;
        for (size_t i = 0; i < flash.count && i < STORAGE_PARTITIONS; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "name", flash.partitions[i].label);
            cJSON_AddNumberToObject(v, "bytes", flash.partitions[i].bytes);
            ok &= web_attach(a, NULL, v);
        }
        ok &= web_attach(f, "partitions", a);
        ok &= web_attach(o, "sd", s);
        ok &= web_attach(o, "flash", f);
        return built(o, ok);
    }
    if (!strcmp(path, "sources")) {
        zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
        size_t count = zone_service_sources(list, ZONE_HUB_MAX_SOURCES);
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        bool ok = true;
        for (size_t i = 0; i < count; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "kind", list[i].kind);
            cJSON_AddNumberToObject(v, "capabilities", list[i].caps);
            cJSON_AddBoolToObject(v, "usable", list[i].usable);
            cJSON_AddBoolToObject(v, "link_down", list[i].link_down);
            cJSON_AddBoolToObject(v, "trouble", list[i].trouble);
            ok &= web_attach(a, NULL, v);
        }
        ok &= web_attach(o, "sources", a);
        return built(o, ok);
    }
    unsigned number;
    if (indexed(path, "binding/", ALARM_MAX_ZONES, &number)) {
        zone_hub_binding_t binding = {0};
        zone_service_get((uint16_t)number, &binding);
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        bool ok = true;
        for (unsigned i = 0; i < ZSRC_ROLE_COUNT; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "role", i);
            ok &= web_attach(v, "address", address(&binding.in[i].addr));
            ok &= web_attach(v, "mapping", mapping(&binding.in[i].map));
            ok &= web_attach(a, NULL, v);
        }
        ok &= web_attach(o, "inputs", a);
        return built(o, ok); /* never a binding without one of its inputs: it is posted back */
    }
    if (!strcmp(path, "network-check")) {
        ui_netcheck_t c;
        b->net_check_status(b->ctx, &c);
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        cJSON_AddBoolToObject(o, "running", c.running);
        cJSON_AddBoolToObject(o, "done", c.done);
        bool ok = true;
        for (unsigned i = 0; i < UI_CHECK_COUNT; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "state", c.steps[i].state);
            cJSON_AddNumberToObject(v, "ms", c.steps[i].ms);
            char line[96];
            ui_check_line(line, sizeof(line),
                          c.steps[i].words ? i18n_text(words_in, "connection check", c.steps[i].words)
                                           : NULL,
                          c.steps[i].value[0], c.steps[i].value[1]);
            cJSON_AddStringToObject(v, "detail", line);
            ok &= web_attach(a, NULL, v);
        }
        ok &= web_attach(o, "steps", a);
        return built(o, ok);
    }
    if (!strcmp(path, "wifi-scan")) {
        ui_wifi_ap_t aps[20];
        bool done = false;
        size_t count = b->wifi_results(b->ctx, aps, 20, &done);
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        cJSON_AddBoolToObject(o, "done", done);
        bool ok = true;
        for (size_t i = 0; i < count; i++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(v, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(v, "secure", aps[i].secure);
            ok &= web_attach(a, NULL, v);
        }
        ok &= web_attach(o, "networks", a);
        return built(o, ok);
    }
    if (!strcmp(path, "notification-status") && b->notify_status) {
        cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
        bool ok = true;
        for (unsigned i = 0; i < UI_NOTIFY_PROVIDERS; i++) {
            ui_notify_status_t s;
            b->notify_status(b->ctx, (uint8_t)i, &s);
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "provider", i);
            cJSON_AddNumberToObject(v, "state", s.state);
            cJSON_AddNumberToObject(v, "sent", s.sent);
            char detail[160];
            ui_why_line(words_in, "why", s.detail, detail, sizeof(detail));
            cJSON_AddStringToObject(v, "detail", detail);
            ok &= web_attach(a, NULL, v);
        }
        ok &= web_attach(o, "providers", a);
        bool test_ok;
        char why[128];
        if (b->notify_test_result(b->ctx, &test_ok, why, sizeof(why))) {
            cJSON_AddBoolToObject(o, "test_ok", test_ok);
            char said[160];
            ui_why_line(words_in, "why", why, said, sizeof(said));
            cJSON_AddStringToObject(o, "test_result", said);
        }
        return built(o, ok);
    }
    return NULL;
}
static bool parse_input(zsrc_input_t *input, const cJSON *o)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "address"),
                *m = cJSON_GetObjectItemCaseSensitive(o, "mapping");
    if (!cJSON_IsObject(a) || !cJSON_IsObject(m))
        return false;
    int64_t v;
    if (!num(a, "kind", 0, 255, &v))
        return false;
    input->addr.kind = (uint8_t)v;
    if (!num(a, "input", 0, 255, &v))
        return false;
    input->addr.input = (uint8_t)v;
    const char *id = str(a, "id");
    if (!id[0] || strlen(id) > 20 || strspn(id, "0123456789") != strlen(id))
        return false;
    char *end;
    errno = 0;
    input->addr.id = strtoull(id, &end, 10);
    if (*end || errno == ERANGE)
        return false;
    if (strlen(str(a, "ref")) >= sizeof(input->addr.ref))
        return false;
    strcpy(input->addr.ref, str(a, "ref"));
    if (!num(m, "mode", 0, ZSRC_MAP_COUNT - 1, &v))
        return false;
    input->map.mode = (uint8_t)v;
    cJSON *invert = cJSON_GetObjectItemCaseSensitive(m, "invert");
    if (!cJSON_IsBool(invert))
        return false;
    input->map.invert = cJSON_IsTrue(invert);
    if (!num(m, "threshold_x100", INT32_MIN, INT32_MAX, &v))
        return false;
    input->map.threshold_x100 = (int32_t)v;
    if (!num(m, "hysteresis_x100", 0, INT32_MAX, &v))
        return false;
    input->map.hysteresis_x100 = (int32_t)v;
    if (!num(m, "grace_s", 0, 65535, &v))
        return false;
    input->map.grace_s = (uint16_t)v;
    if (!num(m, "response_ms", 0, 65535, &v))
        return false;
    input->map.response_ms = (uint16_t)v;
    /* hold_s was added later: a client that doesn't send it leaves the automatic hold (0) */
    if (cJSON_GetObjectItemCaseSensitive(m, "hold_s")) {
        if (!num(m, "hold_s", 0, 65535, &v))
            return false;
        input->map.hold_s = (uint16_t)v;
    }
    if (strlen(str(m, "states")) >= sizeof(input->map.states))
        return false;
    strcpy(input->map.states, str(m, "states"));
    return true;
}
typedef struct {
    uint8_t user;
    bool erase_log;
} reset_t;
static void reset_task(void *arg)
{
    reset_t request = *(reset_t *)arg;
    free(arg);
    vTaskDelay(pdMS_TO_TICKS(1500));
    panel_system_factory_reset(&SOURCE, request.user, request.erase_log);
    vTaskDelete(NULL);
}
cJSON *web_extra_action(const ui_backend_t *b, const char *path, const cJSON *o, uint8_t user,
                        alarm_authority_t authority, const i18n_catalog_t *words)
{
    char why[160] = {0};
    const char *action = str(o, "action");
    int64_t value;
    if (!strcmp(path, "sources/search")) {
        if (!num(o, "kind", 1, 255, &value) || strlen(str(o, "query")) > 95)
            return answer(false, "Invalid source query");
        return candidates((uint8_t)value, str(o, "query"));
    }
    if (!strcmp(path, "sources/learn")) {
        if (authority != ALARM_AUTH_INSTALLER || !num(o, "kind", 1, 255, &value) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(o, "enabled")))
            return answer(false, "Installer authority and source required");
        return answer(
            zone_service_learn((uint8_t)value,
                               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "enabled"))),
            "Source could not enter learning mode");
    }
    unsigned number;
    if (indexed(path, "binding/", ALARM_MAX_ZONES, &number)) {
        /* The installer's, as the signed-in installer: no code (BL-135). */
        if (authority != ALARM_AUTH_INSTALLER)
            return answer(false, "Installer authority required");
        if (!strcmp(action, "unbind"))
            return answer(zone_service_unbind((uint16_t)number) == ZONE_HUB_OK,
                          "Could not remove binding while armed or busy");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "inputs");
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != ZSRC_ROLE_COUNT)
            return answer(false, "Three input roles are required");
        zone_hub_binding_t binding = {0};
        for (unsigned i = 0; i < ZSRC_ROLE_COUNT; i++)
            if (!parse_input(&binding.in[i], cJSON_GetArrayItem(a, (int)i)))
                return answer(false, "Invalid sensor address or mapping");
        zone_hub_result_t e = zone_service_bind((uint16_t)number, &binding);
        return answer(e == ZONE_HUB_OK, zone_service_result_name(e));
    }
    if (!strcmp(path, "storage")) {
        int64_t gen;
        if (!num(o, "generation", 0, UINT32_MAX, &gen))
            return answer(false, "Storage generation required");
        storage_action_t op = !strcmp(action, "check")   ? STORAGE_CHECK
                              : !strcmp(action, "eject") ? STORAGE_EJECT
                                                         : STORAGE_FORMAT;
        if (strcmp(action, "check") && strcmp(action, "eject") && strcmp(action, "format"))
            return answer(false, "Unknown storage action");
        if (op == STORAGE_FORMAT && strcmp(str(o, "confirm"), "ERASE SD CARD"))
            return answer(false, "Type ERASE SD CARD to confirm");
        esp_err_t e = panel_storage_request(op, (uint32_t)gen, user);
        return answer(e == ESP_OK, esp_err_to_name(e));
    }
    if (!strcmp(path, "network-check")) {
        b->net_check_start(b->ctx);
        return answer(true, "");
    }
    if (!strcmp(path, "wifi-scan")) {
        b->wifi_scan(b->ctx);
        return answer(true, "");
    }
    if (!strcmp(path, "notification-test") && b->notify_test) {
        if (!num(o, "provider", 0, UI_NOTIFY_PROVIDERS - 1, &value))
            return answer(false, "Unknown provider");
        bool ok = b->notify_test(b->ctx, (uint8_t)value, why, sizeof(why));
        char said[160];
        ui_why_line(words, "why", why, said, sizeof(said));
        return answer(ok, said);
    }
    if (!strcmp(path, "ntfy-topic") && b->ntfy_new_topic) {
        char topic[64];
        b->ntfy_new_topic(b->ctx, topic, sizeof(topic));
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "topic", topic);
        return result;
    }
    if (!strcmp(path, "certificate-authority")) {
        const char *provider = str(o, "provider"), *pem = str(o, "pem");
        if (strlen(pem) > 8192)
            return answer(false, "Certificate is too large");
        esp_err_t e = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_ALARM_FEATURE_MQTT
        if (!strcmp(provider, "mqtt") && b->mqtt_get)
            e = net_mqtt_set_ca(pem);
#endif
#if CONFIG_ALARM_FEATURE_NOTIFY
        if (!strcmp(provider, "ntfy") && b->ntfy_get)
            e = net_notify_set_ca(pem);
#endif
        (void)provider;
        return answer(e == ESP_OK, esp_err_to_name(e));
    }
    if (!strcmp(path, "installer-code")) {
        /* The installer and master codes, set in installer mode as at the panel: entered as
         * the signed-in installer (CORE-PROG-011), no code. */
        if (authority != ALARM_AUTH_INSTALLER || !num(o, "number", 1, 2, &value))
            return answer(false, "Installer authority required");
        alarm_result_t e = alarm_service_program_enter_as(&SOURCE, user);
        if (e == ALARM_OK) {
            e = alarm_service_program_set_code(&SOURCE, (uint8_t)value, str(o, "new_code"));
            alarm_service_program_exit(&SOURCE);
        }
        return answer(e == ALARM_OK, alarm_result_name(e));
    }
    if (!strcmp(path, "factory-reset")) {
        /* panel_web.c asked for a recent password first; installer mode as the signed-in
         * installer (CORE-PROG-011) holds the panel still until the restart. */
        if (authority != ALARM_AUTH_INSTALLER || strcmp(str(o, "confirm"), "ERASE THIS PANEL"))
            return answer(false, "Installer authority and ERASE THIS PANEL confirmation required");
        const alarm_result_t e = alarm_service_program_enter_as(&SOURCE, user);
        if (e != ALARM_OK)
            return answer(false, alarm_result_name(e));
        reset_t *request = web_alloc(sizeof(*request));
        if (!request) {
            alarm_service_program_exit(&SOURCE);
            return answer(false, "Memory busy");
        }
        request->user = user;
        request->erase_log = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "erase_log"));
        if (xTaskCreate(reset_task, "web_reset", 4096, request, 2, NULL) != pdPASS) {
            free(request);
            alarm_service_program_exit(&SOURCE);
            return answer(false, "Could not schedule reset");
        }
        return answer(true, "");
    }
    return NULL;
}
