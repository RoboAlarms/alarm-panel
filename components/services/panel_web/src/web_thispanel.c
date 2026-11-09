/*
 * web_thispanel.c - Setup's "This panel" and "Sensors and devices" pages through the server link
 * (web_thispanel.h): files in parts, the sirens and outputs, Smart Home's Owner-code devices.
 */
#include "web_thispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- base64 ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t web_b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3u) {
        const uint32_t a = in[i];
        const uint32_t b = i + 1u < n ? in[i + 1u] : 0u;
        const uint32_t c = i + 2u < n ? in[i + 2u] : 0u;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = i + 1u < n ? B64[(v >> 6) & 63u] : '=';
        out[o++] = i + 2u < n ? B64[v & 63u] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if (ch == '+')
        return 62;
    if (ch == '/')
        return 63;
    return -1;
}

int web_b64_decode(const char *in, uint8_t *out, size_t cap)
{
    const size_t n = strlen(in);
    if (n % 4u != 0u)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4u) {
        int v[4];
        int pad = 0;
        for (size_t k = 0; k < 4u; k++) {
            const char ch = in[i + k];
            if (ch == '=' && i + 4u == n && k >= 2u) {
                v[k] = 0;
                pad++;
                continue;
            }
            if (pad > 0 || (v[k] = b64_value(ch)) < 0)
                return -1;
        }
        const uint32_t x = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) |
                           (uint32_t)v[3];
        const size_t take = 3u - (size_t)pad;
        if (o + take > cap)
            return -1;
        out[o++] = (uint8_t)(x >> 16);
        if (take > 1u)
            out[o++] = (uint8_t)(x >> 8);
        if (take > 2u)
            out[o++] = (uint8_t)x;
    }
    return (int)o;
}

/* ---- files in parts ---- */

cJSON *web_part_json(const uint8_t *file, size_t len, size_t off)
{
    if (file == NULL || off >= len)
        return NULL;
    const size_t n = len - off < WEB_PART_OUT ? len - off : WEB_PART_OUT;
    char *text = malloc(4u * ((n + 2u) / 3u) + 1u);
    if (text == NULL)
        return NULL;
    web_b64_encode(file + off, n, text);
    cJSON *o = cJSON_CreateObject();
    if (o == NULL || cJSON_AddNumberToObject(o, "offset", (double)off) == NULL ||
        cJSON_AddStringToObject(o, "data", text) == NULL) {
        cJSON_Delete(o);
        o = NULL;
    }
    free(text);
    return o;
}

const char *web_part_take(uint8_t *buf, size_t size, size_t *have, size_t off, const char *data)
{
    if (buf == NULL || data == NULL || off != *have || strlen(data) > 4u * ((WEB_PART_IN + 2u) / 3u))
        return "backup_format";
    const int n = web_b64_decode(data, buf + off, size - off);
    if (n <= 0)
        return "backup_format";
    *have += (size_t)n;
    return NULL;
}

/* ---- sirens and outputs ---- */

static const char *const WIRED_KINDS[] = {NULL, "siren", "strobe", "smoke_reset", "armed", "automation"};

static bool add_common(cJSON *e, uint8_t partitions, uint8_t also, bool burglary, bool fire,
                       bool squawk)
{
    return cJSON_AddNumberToObject(e, "partitions", partitions) != NULL &&
           cJSON_AddNumberToObject(e, "also", also) != NULL &&
           cJSON_AddBoolToObject(e, "burglary", burglary) != NULL &&
           cJSON_AddBoolToObject(e, "fire", fire) != NULL &&
           cJSON_AddBoolToObject(e, "squawk", squawk) != NULL;
}

cJSON *web_sirens_json(const web_wired_t *w, size_t nw, const web_rf_t *r, size_t nr,
                       const uint8_t *free_terminals, size_t nt)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *list = o ? cJSON_AddArrayToObject(o, "outputs") : NULL;
    cJSON *terms = o ? cJSON_AddArrayToObject(o, "terminals") : NULL;
    bool ok = list != NULL && terms != NULL;
    for (size_t i = 0; ok && i < nw; i++) {
        const ow_config_t *c = &w[i].c;
        if (c->function == OW_FN_NONE || c->function >= OW_FN_COUNT)
            continue;
        cJSON *e = cJSON_CreateObject();
        ok = e != NULL && cJSON_AddItemToArray(list, e) &&
             cJSON_AddNumberToObject(e, "number", (double)(i + 1u)) != NULL &&
             cJSON_AddStringToObject(e, "kind", WIRED_KINDS[c->function]) != NULL &&
             cJSON_AddStringToObject(e, "name", c->name) != NULL &&
             cJSON_AddBoolToObject(e, "on", w[i].on) != NULL &&
             cJSON_AddBoolToObject(e, "trouble", w[i].trouble) != NULL &&
             add_common(e, c->partitions, c->also, c->burglary, c->fire, c->squawk) &&
             cJSON_AddNumberToObject(e, "terminal", c->gpio) != NULL &&
             cJSON_AddBoolToObject(e, "active_low", c->active_low) != NULL &&
             (c->sense == OW_SENSE_NONE ? cJSON_AddNullToObject(e, "sense")
                                        : cJSON_AddNumberToObject(e, "sense", c->sense)) != NULL;
    }
    for (size_t i = 0; ok && i < nr; i++) {
        const rfs_config_t *c = &r[i].c;
        if (!c->used)
            continue;
        cJSON *e = cJSON_CreateObject();
        ok = e != NULL && cJSON_AddItemToArray(list, e) &&
             cJSON_AddNumberToObject(e, "number", (double)(i + 1u)) != NULL &&
             cJSON_AddStringToObject(e, "kind", "rf_siren") != NULL &&
             cJSON_AddStringToObject(e, "name", "") != NULL &&
             cJSON_AddBoolToObject(e, "on", r[i].sounding) != NULL &&
             cJSON_AddBoolToObject(e, "trouble", false) != NULL &&
             add_common(e, c->partitions, c->also, c->burglary, c->fire, c->squawk);
    }
    for (size_t i = 0; ok && i < nt; i++)
        ok = cJSON_AddItemToArray(terms, cJSON_CreateNumber(free_terminals[i]));
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static bool num_in(const cJSON *o, const char *k, double lo, double hi, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        v->valuedouble != (double)(uint32_t)v->valuedouble)
        return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}

static void take_bool(const cJSON *o, const char *k, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v))
        *out = cJSON_IsTrue(v);
}

static void take_u8(const cJSON *o, const char *k, uint32_t hi, uint8_t *out)
{
    uint32_t v;
    if (num_in(o, k, 0, hi, &v))
        *out = (uint8_t)v;
}

web_siren_op_t web_siren_request(const cJSON *o, bool *rf_out, size_t *slot, ow_config_t *wired,
                                 rfs_config_t *rf)
{
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "action"));
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "kind"));
    if (action == NULL || kind == NULL)
        return WEB_SIREN_BAD;
    const bool rf_siren = !strcmp(kind, "rf_siren");
    uint8_t function = OW_FN_NONE;
    for (uint8_t f = 1; !rf_siren && f < OW_FN_COUNT; f++)
        if (!strcmp(kind, WIRED_KINDS[f]))
            function = f;
    uint32_t number;
    if ((!rf_siren && function == OW_FN_NONE) ||
        !num_in(o, "number", 1, rf_siren ? RFS_SIRENS : OW_CHANNELS, &number))
        return WEB_SIREN_BAD;
    *rf_out = rf_siren;
    *slot = number - 1u;
    if (!strcmp(action, "test"))
        return WEB_SIREN_TEST;
    if (!strcmp(action, "remove"))
        return WEB_SIREN_CLEAR;
    if (strcmp(action, "set"))
        return WEB_SIREN_BAD;
    if (rf_siren) {
        take_u8(o, "partitions", 255, &rf->partitions);
        take_u8(o, "also", 255, &rf->also);
        take_bool(o, "burglary", &rf->burglary);
        take_bool(o, "fire", &rf->fire);
        take_bool(o, "squawk", &rf->squawk);
        return WEB_SIREN_SET;
    }
    wired->function = function;
    take_u8(o, "partitions", 255, &wired->partitions);
    take_u8(o, "also", 255, &wired->also);
    take_bool(o, "burglary", &wired->burglary);
    take_bool(o, "fire", &wired->fire);
    take_bool(o, "squawk", &wired->squawk);
    take_u8(o, "terminal", 254, &wired->gpio);
    take_bool(o, "active_low", &wired->active_low);
    const cJSON *sense = cJSON_GetObjectItemCaseSensitive(o, "sense");
    if (cJSON_IsNull(sense))
        wired->sense = OW_SENSE_NONE;
    else
        take_u8(o, "sense", 254, &wired->sense);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name"));
    if (name != NULL) {
        if (strlen(name) >= OW_NAME_LEN)
            return WEB_SIREN_BAD;
        snprintf(wired->name, sizeof(wired->name), "%s", name);
    }
    return WEB_SIREN_SET;
}

/* ---- Smart Home ---- */

cJSON *web_home_json(const hc_home_t *h, const web_home_dev_t *devs, size_t n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *list = o ? cJSON_AddArrayToObject(o, "devices") : NULL;
    bool ok = list != NULL && cJSON_AddBoolToObject(o, "arrange_code", hc_home_arrange_code(h)) != NULL;
    for (size_t i = 0; ok && i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        ok = e != NULL && cJSON_AddItemToArray(list, e) &&
             cJSON_AddStringToObject(e, "ref", devs[i].ref) != NULL &&
             cJSON_AddStringToObject(e, "name", devs[i].name) != NULL &&
             cJSON_AddStringToObject(e, "room", hc_home_room_name(h, devs[i].ref, devs[i].area ? devs[i].area : "")) != NULL &&
             cJSON_AddBoolToObject(e, "code", hc_home_protected(h, devs[i].ref)) != NULL;
    }
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static bool known(const web_home_dev_t *devs, size_t n, const char *ref)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(devs[i].ref, ref))
            return true;
    return false;
}

bool web_home_apply(hc_home_t *h, const cJSON *o, const web_home_dev_t *devs, size_t n)
{
    const cJSON *arrange = cJSON_GetObjectItemCaseSensitive(o, "arrange_code");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(o, "devices");
    if (!cJSON_IsBool(arrange) && !cJSON_IsArray(list))
        return false;
    if (list != NULL && !cJSON_IsArray(list))
        return false;
    const cJSON *d;
    cJSON_ArrayForEach(d, list) {
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d, "ref"));
        if (ref == NULL || !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(d, "code")) || !known(devs, n, ref))
            return false;
    }
    hc_home_t *was = malloc(sizeof(*was));
    if (was == NULL)
        return false;
    *was = *h;
    bool ok = true;
    cJSON_ArrayForEach(d, list) {
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d, "ref"));
        const bool on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "code"));
        if (hc_home_protected(h, ref) != on && !hc_home_protect(h, ref, on))
            ok = false;
    }
    if (ok && cJSON_IsBool(arrange)) {
        if (cJSON_IsTrue(arrange))
            h->flags |= HC_HOME_F_ARRANGE_CODE;
        else
            h->flags &= (uint8_t)~HC_HOME_F_ARRANGE_CODE;
    }
    if (!ok)
        *h = *was;
    free(was);
    return ok;
}
