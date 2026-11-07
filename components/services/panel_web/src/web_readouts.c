/*
 * web_readouts.c - the sirens and outputs, and the last backup, as answers (web_readouts.h).
 */
#include "web_readouts.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alarm/alarm_strings.h"

const char *web_output_kind(uint8_t function)
{
    /* out_wired.h ow_function_t: stored numbers, never renumbered */
    static const char *const KINDS[] = {NULL, "siren", "strobe", "smoke_reset", "armed", "automation"};
    return function < sizeof(KINDS) / sizeof(KINDS[0]) ? KINDS[function] : NULL;
}

cJSON *web_outputs_json(const web_output_t *outs, size_t n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *list = o ? cJSON_AddArrayToObject(o, "outputs") : NULL;
    if (list == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (!outs[i].used || outs[i].kind == NULL)
            continue;
        cJSON *e = cJSON_CreateObject();
        if (e == NULL || !cJSON_AddItemToArray(list, e) ||
            !cJSON_AddNumberToObject(e, "number", outs[i].number) ||
            !cJSON_AddStringToObject(e, "kind", outs[i].kind) ||
            !cJSON_AddStringToObject(e, "name", outs[i].name ? outs[i].name : "") ||
            !cJSON_AddBoolToObject(e, "on", outs[i].on) ||
            !cJSON_AddBoolToObject(e, "trouble", outs[i].trouble)) {
            cJSON_Delete(o);
            return NULL;
        }
    }
    return o;
}

cJSON *web_backup_json(bool have, uint64_t at_s, uint8_t user, const char *user_name,
                       uint32_t size)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL)
        return NULL;
    bool ok = true;
    if (have && at_s != 0u) {
        const time_t t = (time_t)at_s;
        struct tm tm;
        char iso[32];
#ifdef _WIN32
        ok &= gmtime_s(&tm, &t) == 0;
#else
        ok &= gmtime_r(&t, &tm) != NULL;
#endif
        ok &= ok && strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0;
        ok &= ok && cJSON_AddStringToObject(o, "last_at", iso) != NULL;
    } else {
        ok &= cJSON_AddNullToObject(o, "last_at") != NULL;
    }
    if (have && user != 0u) {
        ok &= cJSON_AddNumberToObject(o, "by", user) != NULL;
        ok &= (user_name && user_name[0] ? cJSON_AddStringToObject(o, "by_name", user_name)
                                         : cJSON_AddNullToObject(o, "by_name")) != NULL;
    } else {
        ok &= cJSON_AddNullToObject(o, "by") != NULL;
        ok &= cJSON_AddNullToObject(o, "by_name") != NULL;
    }
    ok &= (have ? cJSON_AddNumberToObject(o, "size", size) : cJSON_AddNullToObject(o, "size")) != NULL;
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

void web_app_txt(web_app_txt_t *out, const char *id, const char *name, const char *host,
                 uint16_t port)
{
    static const char *const KEYS[] = {"id", "name", "srv"};
    const char *const values[] = {id, name};
    out->n = 0;
    for (unsigned i = 0; i < 2u; i++) {
        out->key[out->n] = KEYS[i];
        snprintf(out->value[out->n], WEB_APP_TXT_VALUE, "%s", values[i] ? values[i] : "");
        out->n++;
    }
    if (host != NULL && host[0] != '\0') {
        out->key[out->n] = KEYS[2];
        if (port == 0u || port == 443u)
            snprintf(out->value[out->n], WEB_APP_TXT_VALUE, "https://%s", host);
        else
            snprintf(out->value[out->n], WEB_APP_TXT_VALUE, "https://%s:%u", host, (unsigned)port);
        out->n++;
    }
}

/* The order the app shows them in: the everyday ones first, the hold-up last. */
static const alarm_panic_t APP_PANICS[] = {ALARM_PANIC_POLICE, ALARM_PANIC_FIRE, ALARM_PANIC_MEDICAL,
                                           ALARM_PANIC_SILENT};

cJSON *web_app_panic_json(uint8_t mask)
{
    cJSON *list = cJSON_CreateArray();
    for (size_t i = 0; list != NULL && i < sizeof(APP_PANICS) / sizeof(APP_PANICS[0]); i++) {
        if (!web_app_panic_allowed(mask, APP_PANICS[i]))
            continue;
        cJSON *s = cJSON_CreateString(alarm_panic_name(APP_PANICS[i]));
        if (s == NULL || !cJSON_AddItemToArray(list, s)) {
            cJSON_Delete(s);
            cJSON_Delete(list);
            return NULL;
        }
    }
    return list;
}

bool web_app_panic_type(const char *name, alarm_panic_t *out)
{
    for (size_t i = 0; name != NULL && i < sizeof(APP_PANICS) / sizeof(APP_PANICS[0]); i++) {
        if (strcmp(name, alarm_panic_name(APP_PANICS[i])) == 0) {
            *out = APP_PANICS[i];
            return true;
        }
    }
    return false;
}

bool web_app_panic_allowed(uint8_t mask, alarm_panic_t type)
{
    return (unsigned)type < 8u && (mask & (1u << (unsigned)type)) != 0u;
}
