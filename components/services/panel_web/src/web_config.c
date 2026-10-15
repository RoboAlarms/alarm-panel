/* Feature 31: explicit field tables are the public configuration contract. No raw
 * structures, padding, code salts, hashes or stored credentials cross the wire. */
#include "web_config.h"
#include "alarm/alarm_strings.h"
#include "auto_names.h"
#include "i18n/i18n.h"
#include "i18n/i18n_date.h"
#include "lang_packs.h"
#include "net_wifi.h"
#include "panel_prefs.h"
#include "ui_port_esp.h"
#include "web_json_merge.h"
#include "web_rules.h"
#include "web_voice.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum { F_BOOL, F_U8, F_U16, F_U32, F_STR, F_SECRET } kind_t;
typedef struct {
    const char *name;
    size_t offset, size;
    kind_t kind;
    double min, max;
} field_t;
#define B(T, n)            {#n, offsetof(T, n), sizeof(((T *)0)->n), F_BOOL, 0, 1}
#define N(T, n, k, lo, hi) {#n, offsetof(T, n), sizeof(((T *)0)->n), k, lo, hi}
#define S(T, n)            {#n, offsetof(T, n), sizeof(((T *)0)->n), F_STR, 0, 0}
#define P(T, n)            {#n, offsetof(T, n), sizeof(((T *)0)->n), F_SECRET, 0, 0}
#define END                {NULL, 0, 0, 0, 0, 0}
/* The same ranges as the touchscreen's installer pages (ui_installer.c), at their loosest (the
 * "outside SIA CP-01" setting); the engine then applies the strict limits it owns
 * (alarm_config_validate). A 0 s cross-zone window, for one, would leave a pair unable to ever
 * confirm, and a 0 s walk test would end as it began. */
static const field_t options[] = {N(alarm_options_t, code_min_len, F_U8, 4, 10),
                                  N(alarm_options_t, abort_window_s, F_U8, 0, 45),
                                  N(alarm_options_t, cancel_window_s, F_U16, 0, 3600),
                                  N(alarm_options_t, bell_timeout_s, F_U16, 0, 1800),
                                  B(alarm_options_t, fire_bell_timeout),
                                  N(alarm_options_t, swinger_trips, F_U8, 1, 15),
                                  N(alarm_options_t, cross_zone_window_s, F_U16, 10, 900),
                                  N(alarm_options_t, fire_verify_window_s, F_U16, 30, 300),
                                  N(alarm_options_t, smoke_reset_s, F_U8, 1, 30),
                                  N(alarm_options_t, exit_restart_limit, F_U8, 0, 3),
                                  N(alarm_options_t, recent_closing_s, F_U16, 0, 600),
                                  B(alarm_options_t, auto_stay),
                                  B(alarm_options_t, exit_sounds),
                                  B(alarm_options_t, quick_arm),
                                  B(alarm_options_t, quick_exit),
                                  B(alarm_options_t, remote_arm_without_code),
                                  B(alarm_options_t, auto_less_secure), /* AUT-015, D25 */
                                  B(alarm_options_t, power_up_previous_state),
                                  N(alarm_options_t, power_up_settle_s, F_U16, 0, 300),
                                  N(alarm_options_t, lockout_attempts, F_U8, 0, 10),
                                  N(alarm_options_t, lockout_s, F_U16, 60, 1800),
                                  N(alarm_options_t, walk_test_timeout_s, F_U16, 600, 14400),
                                  N(alarm_options_t, trouble_beep_mask, F_U32, 0, 4294967295.0),
                                  B(alarm_options_t, relaxed_limits),
                                  B(alarm_options_t, power_up_program),
                                  B(alarm_options_t, chime_needs_code),
                                  END};
static const field_t partition[] = {B(alarm_partition_config_t, enabled),
                                    S(alarm_partition_config_t, name),
                                    N(alarm_partition_config_t, exit_delay_s, F_U16, 0, 600),
                                    N(alarm_partition_config_t, entry_delay1_s, F_U16, 0, 600),
                                    N(alarm_partition_config_t, entry_delay2_s, F_U16, 0, 600),
                                    END};
static const field_t zone[] = {N(alarm_zone_config_t, type, F_U8, 0, 255),
                               N(alarm_zone_config_t, partition, F_U8, 1, ALARM_MAX_PARTITIONS),
                               N(alarm_zone_config_t, flags, F_U16, 0, 65535),
                               N(alarm_zone_config_t, device_class, F_U8, 0, 255),
                               N(alarm_zone_config_t, user, F_U8, 0, ALARM_MAX_USERS),
                               N(alarm_zone_config_t, swinger_trips, F_U8, 0, 6),
                               N(alarm_zone_config_t, cross_zone, F_U16, 0, ALARM_MAX_ZONES),
                               N(alarm_zone_config_t, supervision_s, F_U16, 0, 65535),
                               S(alarm_zone_config_t, name),
                               END};
static const field_t device[] = {B(alarm_device_config_t, enabled),
                                 N(alarm_device_config_t, partition, F_U8, 0, ALARM_MAX_PARTITIONS),
                                 N(alarm_device_config_t, supervision_s, F_U16, 0, 65535),
                                 S(alarm_device_config_t, name), END};
static const field_t mqtt[] = {B(ui_mqtt_settings_t, enabled),
                               S(ui_mqtt_settings_t, host),
                               N(ui_mqtt_settings_t, port, F_U16, 0, 65535),
                               S(ui_mqtt_settings_t, user),
                               P(ui_mqtt_settings_t, password),
                               N(ui_mqtt_settings_t, tls, F_U8, 0, 2),
                               B(ui_mqtt_settings_t, discovery),
                               B(ui_mqtt_settings_t, commands),
                               B(ui_mqtt_settings_t, allow_panic), /* installer: panel_web.c */
                               S(ui_mqtt_settings_t, prefix),
                               S(ui_mqtt_settings_t, panel_id),
                               END};
static const field_t ntfy[] = {
    B(ui_ntfy_settings_t, enabled),    B(ui_ntfy_settings_t, self_hosted),
    S(ui_ntfy_settings_t, server),     P(ui_ntfy_settings_t, topic),
    P(ui_ntfy_settings_t, token),      N(ui_ntfy_settings_t, cats, F_U8, 0, 255),
    B(ui_ntfy_settings_t, silent_too), END};
static const field_t pushover[] = {
    B(ui_pushover_settings_t, enabled),    P(ui_pushover_settings_t, user_key),
    P(ui_pushover_settings_t, app_token),  N(ui_pushover_settings_t, cats, F_U8, 0, 255),
    B(ui_pushover_settings_t, silent_too), B(ui_pushover_settings_t, emergency),
    S(ui_pushover_settings_t, sound),      END};
static const field_t ip[] = {B(ui_ip_config_t, use_static),
                             S(ui_ip_config_t, ip),
                             S(ui_ip_config_t, mask),
                             S(ui_ip_config_t, gateway),
                             S(ui_ip_config_t, dns1),
                             S(ui_ip_config_t, dns2),
                             END};
static const field_t prefs[] = {N(ui_options_t, brightness, F_U8, 10, 100),
                                N(ui_options_t, saver_brightness, F_U8, 0, 50),
                                N(ui_options_t, saver_after_ms, F_U32, 0, 86400000),
                                N(ui_options_t, off_after_ms, F_U32, 0, 86400000),
                                B(ui_options_t, night_on),
                                N(ui_options_t, night_start, F_U16, 0, 1439),
                                N(ui_options_t, night_end, F_U16, 0, 1439),
                                N(ui_options_t, night_brightness, F_U8, 10, 100),
                                N(ui_options_t, night_off_after_ms, F_U32, 0, 86400000),
                                N(ui_options_t, clock_mode, F_U8, 0, 2),
                                N(ui_options_t, volume, F_U8, 0, 100),
                                N(ui_options_t, chime_volume, F_U8, 0, 100), /* AUD-032 */
                                N(ui_options_t, voice_volume, F_U8, 0, 100),
                                B(ui_options_t, key_click),
                                S(ui_options_t, lang),
                                S(ui_options_t, region),
                                N(ui_options_t, appearance, F_U8, 0, 2), /* UI_APPEARANCE_* */
                                B(ui_options_t, high_contrast),
                                END};
/* The three Announcements switches (docs/features/08 voice-first-build.md D06), kept in the same
 * preferences (panel_prefs_t) but not in ui_options_t: the display domain carries them beside the
 * others, and a write sets them with panel_voice_set_prefs(). */
typedef struct {
    bool voice_enabled, voice_status, voice_chime;
} voice_opts_t;
static const field_t voice[] = {B(voice_opts_t, voice_enabled), B(voice_opts_t, voice_status),
                                B(voice_opts_t, voice_chime), END};

/* A builder's document: o, or NULL when one of its parts could not be made or attached (ok
 * false: web_attach(), code review M01), never a partial answer. */
static cJSON *built(cJSON *o, bool ok)
{
    if (ok)
        return o;
    cJSON_Delete(o);
    return NULL;
}
static cJSON *fields_get(const field_t *f, const void *data)
{
    cJSON *o = cJSON_CreateObject();
    bool ok = o != NULL;
    for (; ok && f->name; f++) {
        const uint8_t *p = (const uint8_t *)data + f->offset;
        if (f->kind == F_SECRET)
            continue;
        if (f->kind == F_STR)
            ok = cJSON_AddStringToObject(o, f->name, (const char *)p) != NULL;
        else if (f->kind == F_BOOL)
            ok = cJSON_AddBoolToObject(o, f->name, *(const bool *)p) != NULL;
        else {
            uint32_t n = 0;
            memcpy(&n, p, f->size);
            ok = cJSON_AddNumberToObject(o, f->name, n) != NULL;
        }
    }
    return built(o, ok);
}
/* Adds one {value,label} choice; false when it could not be made (value is freed either way). */
static bool choice(cJSON *choices, cJSON *value, const char *label)
{
    cJSON *c = cJSON_CreateObject();
    bool ok = web_attach(c, "value", value);
    ok = ok && cJSON_AddStringToObject(c, "label", label) != NULL;
    if (!ok) {
        cJSON_Delete(c); /* NULL-safe */
        return false;
    }
    return web_attach(choices, NULL, c);
}

/* The display domain's choices (docs/features/37 I18N-013, I18N-015) in the page's language
 * (`words`, X-Panel-Language; NULL is English): the languages this firmware carries, each in its
 * own name - a draft, which only a development build carries (I18N-005), says so - the regions
 * and the clock. */
static cJSON *prefs_choices(const char *name, const i18n_catalog_t *words, bool *failed)
{
    cJSON *choices = cJSON_CreateArray();
    bool ok = choices != NULL;
    if (!strcmp(name, "lang")) {
        ok &= choice(choices, cJSON_CreateString("en-US"), i18n_language_name(NULL));
        for (size_t i = 0; i < i18n_catalog_count(); i++) {
            const i18n_catalog_t *c = i18n_catalog_at(i);
            if (c == NULL) {
                continue;
            }
            char label[96];
            if (c->preview) {
                /* TR: the Panel Manager's language list: a translation still being checked,
                 * offered only by a development build; {0} is the language's own name ("Español").
                 * At most 60 bytes */
                I18N_FORMAT(label, sizeof(label), i18n_text(words, NULL, "{0} · a draft, for testing"),
                            i18n_language_name(c));
            } else {
                snprintf(label, sizeof(label), "%s", i18n_language_name(c));
            }
            ok &= choice(choices, cJSON_CreateString(c->locale), label);
        }
        /* The installed language packs (docs/features/37 language-packs.md): a usable one, in
         * its own name; a built-in catalog of the same locale is already listed above. */
        lang_pack_info_t packs[LANG_PACKS_LIST_MAX];
        const size_t n = lang_packs_list(packs, LANG_PACKS_LIST_MAX);
        for (size_t i = 0; i < n; i++) {
            const lang_pack_info_t *p = &packs[i];
            if (p->state != LANG_PACK_READY && p->state != LANG_PACK_OTHER) {
                continue;
            }
            if (i18n_catalog(p->locale) != NULL) {
                continue;
            }
            char label[96];
            if (p->draft) {
                I18N_FORMAT(label, sizeof(label), i18n_text(words, NULL, "{0} · a draft, for testing"),
                            p->name);
            } else {
                snprintf(label, sizeof(label), "%s", p->name);
            }
            ok &= choice(choices, cJSON_CreateString(p->locale), label);
        }
    } else if (!strcmp(name, "region")) {
        /* TR: Language and region, the region list's first entry: the language's own region is
         * used. At most 63 bytes */
        ok &= choice(choices, cJSON_CreateString(""), i18n_text(words, NULL, "The language's"));
        for (size_t i = 0; i < i18n_region_count(); i++) {
            const i18n_region_t *r = i18n_region_at(i);
            ok &= choice(choices, cJSON_CreateString(r->tag), i18n_text(words, "region", r->name));
        }
    } else if (!strcmp(name, "clock_mode")) {
        /* TR: Language and region, the clock list: the region decides between 12 and 24 hours.
         * At most 63 bytes */
        ok &= choice(choices, cJSON_CreateNumber(I18N_CLOCK_AUTO), i18n_text(words, NULL, "The region's"));
        /* TR: Language and region, the clock list: always 12 hours with AM and PM. At most 63 bytes */
        ok &= choice(choices, cJSON_CreateNumber(I18N_CLOCK_12H), i18n_text(words, NULL, "12-hour"));
        /* TR: Language and region, the clock list: always 24 hours. At most 63 bytes */
        ok &= choice(choices, cJSON_CreateNumber(I18N_CLOCK_24H), i18n_text(words, NULL, "24-hour"));
    } else if (!strcmp(name, "appearance")) {
        /* The appearance (docs/features/07 UI-010, BL-132): the touchscreen's three choices. */
        /* TR: Display and sound, the appearance list: light words on a dark screen. At most 63
         * bytes */
        ok &= choice(choices, cJSON_CreateNumber(UI_APPEARANCE_DARK), i18n_text(words, "appearance", "Dark"));
        /* TR: Display and sound, the appearance list: dark words on a light screen. At most 63
         * bytes */
        ok &= choice(choices, cJSON_CreateNumber(UI_APPEARANCE_LIGHT), i18n_text(words, "appearance", "Light"));
        /* TR: Display and sound, the appearance list: dark during the night hours, light by day.
         * At most 63 bytes */
        ok &= choice(choices, cJSON_CreateNumber(UI_APPEARANCE_AUTO), i18n_text(words, "appearance", "Automatic"));
    } else {
        cJSON_Delete(choices);
        return NULL;
    }
    *failed = !ok; /* a list this field has but could not be built */
    return built(choices, ok);
}

static cJSON *schema(const field_t *f, const i18n_catalog_t *words)
{
    bool zone_fields = f == zone;
    bool prefs_fields = f == prefs;
    cJSON *a = cJSON_CreateArray();
    bool ok = a != NULL;
    for (; ok && f->name; f++) {
        cJSON *o = cJSON_CreateObject();
        bool row = cJSON_AddStringToObject(o, "name", f->name) != NULL; /* NULL o: none added */
        row &= cJSON_AddStringToObject(o, "type",
                                       f->kind == F_BOOL     ? "boolean"
                                       : f->kind == F_STR    ? "text"
                                       : f->kind == F_SECRET ? "password"
                                                             : "number") != NULL;
        row &= cJSON_AddNumberToObject(o, "maxLength", f->size ? f->size - 1 : 0) != NULL;
        row &= cJSON_AddNumberToObject(o, "min", f->min) != NULL;
        row &= cJSON_AddNumberToObject(o, "max", f->max) != NULL;
        if (zone_fields && (!strcmp(f->name, "type") || !strcmp(f->name, "device_class"))) {
            cJSON *choices = cJSON_CreateArray();
            bool list = choices != NULL;
            for (unsigned i = 0; list && i <= 255; i++) {
                const char *name = !strcmp(f->name, "type")
                                       ? alarm_zone_type_name((alarm_zone_type_t)i)
                                       : alarm_device_class_name((alarm_device_class_t)i);
                if (!strcmp(name, "unknown"))
                    continue;
                list = choice(choices, cJSON_CreateNumber(i), name);
            }
            if (!list) {
                cJSON_Delete(choices);
                choices = NULL;
            }
            row &= web_attach(o, "choices", choices);
        }
        bool failed = false;
        cJSON *choices = prefs_fields ? prefs_choices(f->name, words, &failed) : NULL;
        if (choices != NULL)
            row &= web_attach(o, "choices", choices);
        row &= !failed;
        if (!row) {
            cJSON_Delete(o);
            ok = false;
        } else {
            ok = web_attach(a, NULL, o);
        }
    }
    return built(a, ok);
}
static cJSON *domain_get(const field_t *f, const void *p, const i18n_catalog_t *words)
{
    cJSON *o = cJSON_CreateObject();
    bool ok = web_attach(o, "values", fields_get(f, p));
    ok &= web_attach(o, "fields", schema(f, words));
    return built(o, ok);
}
static bool fields_patch(const field_t *fields, void *data, const cJSON *o, char *why, size_t cap)
{
    if (!cJSON_IsObject(o)) {
        snprintf(why, cap, "Expected an object");
        return false;
    }
    const cJSON *v;
    cJSON_ArrayForEach(v, o)
    {
        const field_t *f = fields;
        for (; f->name && strcmp(f->name, v->string); f++)
            ;
        if (!f->name) {
            snprintf(why, cap, "Unknown field: %.40s", v->string);
            return false;
        }
        /* Duplicate JSON members are rejected, including two attempts at the same credential. */
        if (cJSON_GetObjectItemCaseSensitive(o, v->string) != v) {
            snprintf(why, cap, "Duplicate field");
            return false;
        }
        uint8_t *p = (uint8_t *)data + f->offset;
        if (f->kind == F_BOOL) {
            if (!cJSON_IsBool(v))
                goto invalid;
            *(bool *)p = cJSON_IsTrue(v);
        } else if (f->kind == F_STR || f->kind == F_SECRET) {
            if (!cJSON_IsString(v) || strlen(v->valuestring) >= f->size)
                goto invalid;
            strcpy((char *)p, v->valuestring);
        } else {
            double n = v->valuedouble;
            if (!cJSON_IsNumber(v) || !isfinite(n) || n < f->min || n > f->max || floor(n) != n)
                goto invalid;
            uint32_t value = (uint32_t)n;
            memcpy(p, &value, f->size);
        }
        continue;
    invalid:
        snprintf(why, cap, "Invalid value for %.40s", f->name);
        return false;
    }
    return true;
}
cJSON *web_config_get(const ui_backend_t *b, const char *d, const i18n_catalog_t *words, bool *busy)
{
    *busy = false;
    if (!strcmp(d, "mqtt") && b->mqtt_get) {
        ui_mqtt_settings_t x;
        b->mqtt_get(b->ctx, &x);
        cJSON *o = domain_get(mqtt, &x, words);
        o = built(o, cJSON_AddBoolToObject(o, "password_set", x.password_set) != NULL &&
                         cJSON_AddBoolToObject(o, "ca_loaded", x.ca_loaded) != NULL);
        *busy = !o; /* a domain this firmware has: nothing back is memory */
        return o;
    }
    if (!strcmp(d, "ntfy") && b->ntfy_get) {
        ui_ntfy_settings_t x;
        b->ntfy_get(b->ctx, &x);
        cJSON *o = domain_get(ntfy, &x, words);
        o = built(o, cJSON_AddBoolToObject(o, "token_set", x.token_set) != NULL &&
                         cJSON_AddBoolToObject(o, "topic_set", x.topic[0] != 0) != NULL);
        *busy = !o; /* a domain this firmware has: nothing back is memory */
        return o;
    }
    if (!strcmp(d, "pushover") && b->pushover_get) {
        ui_pushover_settings_t x;
        b->pushover_get(b->ctx, &x);
        cJSON *o = domain_get(pushover, &x, words);
        o = built(o, cJSON_AddBoolToObject(o, "user_key_set", x.user_key_set) != NULL &&
                         cJSON_AddBoolToObject(o, "app_token_set", x.app_token_set) != NULL);
        *busy = !o; /* a domain this firmware has: nothing back is memory */
        return o;
    }
    if (!strcmp(d, "network")) {
        ui_ip_config_t x;
        b->ip_get(b->ctx, &x);
        cJSON *o = domain_get(ip, &x, words);
        *busy = !o; /* a domain this firmware has: nothing back is memory */
        return o;
    }
    if (!strcmp(d, "display")) {
        ui_options_t x;
        ui_port_esp_options(&x);
        cJSON *o = domain_get(prefs, &x, words);
        panel_prefs_t p;
        panel_prefs_load(&p);
        const voice_opts_t vo = {p.voice_enabled, p.voice_status, p.voice_chime};
        /* The voice switches join the display's own values and fields. Any allocation that
         * failed - the document, its values or fields, a temporary, or a member that couldn't
         * be added (each temporary must carry every voice field) - drops the whole answer: the
         * route says 503 "Memory busy" rather than send part of it, or crash (review 3, #6). */
        const int voice_fields = (int)(sizeof(voice) / sizeof(voice[0])) - 1; /* without END */
        cJSON *values = cJSON_GetObjectItemCaseSensitive(o, "values"); /* NULL-safe */
        cJSON *fields = cJSON_GetObjectItemCaseSensitive(o, "fields");
        cJSON *more = values && fields ? fields_get(voice, &vo) : NULL;
        bool ok = more && cJSON_GetArraySize(more) == voice_fields;
        ok = web_json_move_members(ok ? values : NULL, more) && ok; /* frees `more` either way */
        more = ok ? schema(voice, words) : NULL;
        ok = more && cJSON_GetArraySize(more) == voice_fields;
        ok = web_json_move_elements(ok ? fields : NULL, more) && ok;
        if (!ok) {
            cJSON_Delete(o);
            *busy = true;
            return NULL;
        }
        return o;
    }
    return NULL;
}
bool web_config_set(const ui_backend_t *b, const char *d, const cJSON *o, const char *operation,
                    char *why, size_t cap)
{
    if (!strcmp(d, "mqtt") && b->mqtt_get && b->mqtt_set) {
        ui_mqtt_settings_t x;
        b->mqtt_get(b->ctx, &x);
        if (!fields_patch(mqtt, &x, o, why, cap))
            return false;
        x.password_change = cJSON_HasObjectItem(o, "password");
        return b->mqtt_set(b->ctx, &x, why, cap);
    }
    if (!strcmp(d, "ntfy") && b->ntfy_get && b->ntfy_set) {
        ui_ntfy_settings_t x;
        b->ntfy_get(b->ctx, &x);
        if (!fields_patch(ntfy, &x, o, why, cap))
            return false;
        x.token_change = cJSON_HasObjectItem(o, "token");
        return b->ntfy_set(b->ctx, &x, why, cap);
    }
    if (!strcmp(d, "pushover") && b->pushover_get && b->pushover_set) {
        ui_pushover_settings_t x;
        b->pushover_get(b->ctx, &x);
        if (!fields_patch(pushover, &x, o, why, cap))
            return false;
        x.user_key_change = cJSON_HasObjectItem(o, "user_key");
        x.app_token_change = cJSON_HasObjectItem(o, "app_token");
        return b->pushover_set(b->ctx, &x, why, cap);
    }
    if (!strcmp(d, "network")) {
        ui_ip_config_t x;
        b->ip_get(b->ctx, &x);
        if (!fields_patch(ip, &x, o, why, cap))
            return false;
        net_ip_config_t candidate = {.use_static = x.use_static};
        strcpy(candidate.ip, x.ip);
        strcpy(candidate.mask, x.mask);
        strcpy(candidate.gateway, x.gateway);
        strcpy(candidate.dns1, x.dns1);
        strcpy(candidate.dns2, x.dns2);
        esp_err_t e = net_wifi_trial_ip(&candidate, operation);
        if (e != ESP_OK)
            snprintf(why, cap, "Network trial refused: %s", esp_err_to_name(e));
        return e == ESP_OK;
    }
    if (!strcmp(d, "display")) {
        ui_options_t x;
        ui_port_esp_options(&x);
        /* Every stored preference stays as it is unless the request names it: the voice
         * switches too, which ui_options_t doesn't carry. */
        panel_prefs_t p;
        panel_prefs_load(&p);
        voice_opts_t vo = {p.voice_enabled, p.voice_status, p.voice_chime};
        if (!cJSON_IsObject(o)) {
            snprintf(why, cap, "Expected an object");
            return false;
        }
        cJSON *rest = cJSON_Duplicate(o, true), *spoken = cJSON_CreateObject();
        if (!rest || !spoken) {
            cJSON_Delete(rest);
            cJSON_Delete(spoken);
            snprintf(why, cap, "Memory busy");
            return false;
        }
        for (const field_t *f = voice; f->name; f++) {
            cJSON *item = cJSON_DetachItemFromObjectCaseSensitive(rest, f->name);
            if (item && !web_attach(spoken, f->name, item)) {
                cJSON_Delete(rest);
                cJSON_Delete(spoken);
                snprintf(why, cap, "Memory busy");
                return false;
            }
        }
        /* A switch named twice leaves its second copy in `rest`, which refuses it. */
        const bool patched = fields_patch(prefs, &x, rest, why, cap) && fields_patch(voice, &vo, spoken, why, cap);
        cJSON_Delete(rest);
        cJSON_Delete(spoken);
        if (!patched)
            return false;
        /* D06: turning Announcements on needs a ready voice ("no_pack", "unavailable"); one
         * already on stays on, so saving another setting never fails for it. */
        const char *refusal = vo.voice_enabled && !p.voice_enabled ? web_voice_refusal() : NULL;
        if (refusal) {
            snprintf(why, cap, "%s", refusal);
            return false;
        }
        if (x.saver_after_ms && x.saver_after_ms < 10000) {
            snprintf(why, cap, "Screen saver delay must be zero or at least 10000 milliseconds");
            return false;
        }
        if ((x.off_after_ms && x.off_after_ms < 10000) ||
            (x.night_off_after_ms && x.night_off_after_ms < 10000)) {
            snprintf(why, cap, "Screen off delay must be zero or at least 10000 milliseconds");
            return false;
        }
        if (!lang_packs_known(x.lang)) {
            snprintf(why, cap, "This panel has no such language");
            return false;
        }
        if (x.region[0] != '\0' && i18n_region(x.region) == NULL) {
            snprintf(why, cap, "Unknown region");
            return false;
        }
        p.brightness = x.brightness;
        p.saver_brightness = x.saver_brightness;
        p.saver_after_ms = x.saver_after_ms;
        p.off_after_ms = x.off_after_ms;
        p.night_on = x.night_on;
        p.night_start = x.night_start;
        p.night_end = x.night_end;
        p.night_brightness = x.night_brightness;
        p.night_off_after_ms = x.night_off_after_ms;
        p.clock_mode = x.clock_mode;
        p.volume = x.volume;
        p.chime_volume = x.chime_volume;
        p.voice_volume = x.voice_volume;
        p.key_click = x.key_click;
        p.appearance = x.appearance;
        p.high_contrast = x.high_contrast;
        p.voice_enabled = vo.voice_enabled;
        p.voice_status = vo.voice_status;
        p.voice_chime = vo.voice_chime;
        snprintf(p.lang, sizeof(p.lang), "%s", x.lang[0] != '\0' ? x.lang : "en-US");
        snprintf(p.region, sizeof(p.region), "%s", x.region);
        if (panel_prefs_save(&p) != ESP_OK) {
            snprintf(why, cap, "Could not persist preferences");
            return false;
        }
        auto_names_set_clock(p.clock_mode, p.region, p.lang); /* an automation's words, D30 */
        /* What the panel may say now (D06): a change that forbids speech cancels it (AUD-015). */
        const panel_voice_prefs_t vp = {.enabled = p.voice_enabled,
                                        .status = p.voice_status,
                                        .chime = p.voice_chime,
                                        .volume = p.voice_volume};
        panel_voice_set_prefs(&vp);
        return true;
    }
    snprintf(why, cap, "Unavailable configuration domain");
    return false;
}
static cJSON *array_get(const field_t *f, const void *data, size_t stride, unsigned count)
{
    cJSON *a = cJSON_CreateArray();
    bool ok = a != NULL;
    for (unsigned i = 0; ok && i < count; i++) {
        cJSON *o = fields_get(f, (const uint8_t *)data + i * stride);
        if (o && cJSON_AddNumberToObject(o, "number", i + 1) == NULL) {
            cJSON_Delete(o);
            o = NULL;
        }
        ok = web_attach(a, NULL, o);
    }
    return built(a, ok);
}
cJSON *web_alarm_config_get(const alarm_config_t *c)
{
    cJSON *o = cJSON_CreateObject(), *s = cJSON_CreateObject(), *panic = cJSON_CreateArray();
    bool ok = web_attach(o, "options", fields_get(options, &c->opt));
    for (unsigned i = 0; i < 3; i++)
        ok &= web_attach(panic, NULL, cJSON_CreateNumber(c->opt.panic_keys[i]));
    ok &= web_attach(o, "panic_keys", panic);
    ok &= web_attach(
        o, "partitions",
        array_get(partition, c->partitions, sizeof(c->partitions[0]), ALARM_MAX_PARTITIONS));
    ok &= web_attach(o, "zones", array_get(zone, c->zones, sizeof(c->zones[0]), ALARM_MAX_ZONES));
    ok &= web_attach(o, "devices",
                     array_get(device, c->devices, sizeof(c->devices[0]), ALARM_MAX_DEVICES));
    ok &= web_attach(s, "options", schema(options, NULL));
    ok &= web_attach(s, "partitions", schema(partition, NULL));
    ok &= web_attach(s, "zones", schema(zone, NULL));
    ok &= web_attach(s, "devices", schema(device, NULL));
    ok &= web_attach(o, "schema", s);
    return built(o, ok);
}
static bool array_patch(const field_t *f, void *data, size_t stride, unsigned count, const cJSON *a,
                        char *why, size_t cap)
{
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > (int)count)
        return false;
    bool seen[ALARM_MAX_ZONES] = {0};
    const cJSON *v;
    cJSON_ArrayForEach(v, a)
    {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(v, "number");
        if (!cJSON_IsNumber(n) || n->valuedouble < 1 || n->valuedouble > count ||
            n->valuedouble != floor(n->valuedouble))
            return false;
        unsigned ix = (unsigned)n->valueint - 1;
        if (seen[ix])
            return false;
        seen[ix] = true;
        cJSON *copy = cJSON_Duplicate(v, true);
        if (!copy)
            return false;
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "number");
        bool ok = fields_patch(f, (uint8_t *)data + stride * ix, copy, why, cap);
        cJSON_Delete(copy);
        if (!ok)
            return false;
    }
    return true;
}
bool web_alarm_config_patch(alarm_config_t *c, const cJSON *o, char *why, size_t cap)
{
    if (!cJSON_IsObject(o))
        return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, o)
    {
        if (cJSON_GetObjectItemCaseSensitive(o, v->string) != v)
            return false;
        bool ok = false;
        if (!strcmp(v->string, "options"))
            ok = fields_patch(options, &c->opt, v, why, cap);
        else if (!strcmp(v->string, "partitions"))
            ok = array_patch(partition, c->partitions, sizeof(c->partitions[0]),
                             ALARM_MAX_PARTITIONS, v, why, cap);
        else if (!strcmp(v->string, "zones"))
            ok = array_patch(zone, c->zones, sizeof(c->zones[0]), ALARM_MAX_ZONES, v, why, cap);
        else if (!strcmp(v->string, "devices"))
            ok = array_patch(device, c->devices, sizeof(c->devices[0]), ALARM_MAX_DEVICES, v, why,
                             cap);
        else if (!strcmp(v->string, "panic_keys") && cJSON_IsArray(v) &&
                 cJSON_GetArraySize(v) == 3) {
            ok = true;
            for (int i = 0; i < 3; i++) {
                cJSON *k = cJSON_GetArrayItem(v, i);
                if (!cJSON_IsNumber(k) || k->valuedouble < 0 || k->valuedouble > 255 ||
                    floor(k->valuedouble) != k->valuedouble) {
                    ok = false;
                    break;
                }
                c->opt.panic_keys[i] = (uint8_t)k->valueint;
            }
        }
        if (!ok) {
            if (!why[0])
                snprintf(why, cap, "Invalid alarm configuration field");
            return false;
        }
    }
    return true;
}
