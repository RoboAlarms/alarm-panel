/*
 * web_zigbee.c - the Panel Manager's Zigbee page (docs/features/20 devices.md §14, ZB-025; the
 * pages BL-145 draws): the radio, the devices, the operation running and the install codes read
 * through the touch UI's own zb_* calls (ui_backend.h), and the requests sent through
 * zigbee_service with the Panel Manager's own decision (panel_web's access table: a signed-in
 * installer, the panel not armed), as web_zwave.c does. A device is named by its registry id;
 * addresses travel as 16 hex digits. An install code goes in once and is never sent back. The
 * network reset re-asks the installer's password (panel_web.c, recent_password): the browser never
 * sends a keypad code, and the request's installer_code stays empty.
 */
#include "web_zigbee.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/platform_util.h"
#include "sdkconfig.h"
#include "web_mem.h"
#include "web_rules.h"

const char *web_zigbee_result_word(ui_zb_req_result_t r)
{
    switch (r) {
    case UI_ZB_REQ_OK: return "ok";
    case UI_ZB_REQ_NOT_ALLOWED: return "not_allowed";
    case UI_ZB_REQ_BUSY: return "busy";
    case UI_ZB_REQ_NO_RADIO: return "no_radio";
    case UI_ZB_REQ_INVALID: return "invalid";
    case UI_ZB_REQ_UNSUPPORTED: return "unsupported";
    case UI_ZB_REQ_BAD_CODE: return "bad_code";
    case UI_ZB_REQ_CHECKSUM: return "checksum";
    case UI_ZB_REQ_BAD_ADDRESS: return "bad_address";
    case UI_ZB_REQ_FULL: return "full";
    case UI_ZB_REQ_NOT_FOUND: return "not_found";
    case UI_ZB_REQ_IN_USE: return "in_use";
    case UI_ZB_REQ_WRONG_CODE: return "wrong_code";
    case UI_ZB_REQ_LOCKED_OUT: return "locked_out";
    default: return "invalid";
    }
}

#if CONFIG_ALARM_ZSRC_ZIGBEE
#include "zigbee_service.h"

#define WORD(table, i) ((size_t)(i) < sizeof(table) / sizeof((table)[0]) ? (table)[(i)] : (table)[0])

/* The enums of ui_backend.h's Zigbee block, in their order: the JSON's words (a contract once
 * shipped: only added to). */
static const char *const RADIO[] = {"not_connected", "starting", "working",  "not_answering",
                                    "paused",        "socket_zwave", "unusable"};
static const char *const PAUSE[] = {"", "firmware", "reset", "no_answer"};
static const char *const DEV_STATE[] = {"ready",       "setting_up", "wake_needed",
                                        "setup_stopped", "not_usable", "joining"};
static const char *const KIND[] = {"device", "sensor", "siren", "keypad", "keyfob", "repeater",
                                   "other"};
static const char *const POWER[] = {"unknown", "battery", "mains"};
static const char *const SIGNAL[] = {"unknown", "good", "fair", "weak"};
static const char *const JOIN[] = {"unknown", "install_code", "standard_key"};
static const char *const FIT[] = {"none", "available", "not_ready", "no_device_slot"};
static const char *const OP[] = {"none",     "add",   "add_code", "remove", "forget",
                                 "replace",  "ping",  "identify", "setup",  "channel",
                                 "survey",   "reset", "restart",  "siren_test"};
static const char *const STEP[] = {"idle",    "starting",   "listening", "joining",
                                   "reading", "setting_up", "working",   "done"};
static const char *const RESULT[] = {
    "none",    "added",      "nothing_joined", "join_failed", "already_added", "not_a_sensor",
    "setup_not_finished", "stopped", "removed", "no_answer", "forgotten", "replaced",
    "answered", "done",      "failed",         "full"};
static const uint8_t CHANNELS[UI_ZB_CHANNELS] = {11u, 15u, 20u, 25u};

static void hex64(uint64_t v, char out[17])
{
    snprintf(out, 17, "%08" PRIX32 "%08" PRIX32, (uint32_t)(v >> 32), (uint32_t)v);
}

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

static bool add_hex64(cJSON *o, const char *key, uint64_t v)
{
    char h[17];
    hex64(v, h);
    return cJSON_AddStringToObject(o, key, v != 0u ? h : "") != NULL;
}

/* A text the backend filled, cut at its buffer's end whatever it holds. */
static bool add_text(cJSON *o, const char *key, char *text, size_t cap)
{
    text[cap - 1u] = '\0';
    return cJSON_AddStringToObject(o, key, text) != NULL;
}

static bool add_radio(cJSON *o, ui_zb_status_t *st)
{
    cJSON *r = cJSON_AddObjectToObject(o, "radio");
    bool ok = r != NULL;
    ok &= cJSON_AddStringToObject(r, "state", st->radio < sizeof(RADIO) / sizeof(RADIO[0])
                                            ? RADIO[st->radio]
                                            : "unusable") != NULL;
    ok &= cJSON_AddStringToObject(r, "paused", WORD(PAUSE, st->paused_why)) != NULL;
    ok &= cJSON_AddBoolToObject(r, "jammed", st->jammed) != NULL;
    ok &= cJSON_AddNumberToObject(r, "devices", st->devices) != NULL;
    ok &= cJSON_AddNumberToObject(r, "not_heard", st->not_heard) != NULL;
    ok &= cJSON_AddNumberToObject(r, "codes_waiting", st->codes_waiting) != NULL;
    ok &= cJSON_AddNumberToObject(r, "learn_s", st->learn_s) != NULL;
    ok &= cJSON_AddNumberToObject(r, "channel", st->network_known ? st->channel : 0) != NULL;
    char pan[8];
    snprintf(pan, sizeof(pan), "%04X", (unsigned)st->pan_id);
    ok &= cJSON_AddStringToObject(r, "pan", st->network_known ? pan : "") != NULL;
    ok &= add_hex64(r, "ext_pan", st->network_known ? st->ext_pan_id : 0u);
    ok &= add_hex64(r, "ieee", st->panel_ieee);
    ok &= add_text(r, "version", st->radio_version, sizeof(st->radio_version));
    if (st->noise_known) {
        ok &= cJSON_AddNumberToObject(r, "noise", st->noise_dbm) != NULL;
    }
    cJSON *survey = cJSON_AddArrayToObject(r, "survey");
    ok &= survey != NULL;
    for (unsigned i = 0; i < UI_ZB_CHANNELS; i++) {
        cJSON *c = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(c, "channel", CHANNELS[i]) != NULL;
        if ((st->survey_known & (1u << i)) != 0u) {
            ok &= cJSON_AddNumberToObject(c, "dbm", st->survey_dbm[i]) != NULL;
        }
        ok &= web_attach(survey, NULL, c);
    }
    ok &= cJSON_AddNumberToObject(r, "quietest", st->quietest) != NULL;
    ok &= cJSON_AddNumberToObject(r, "restarts", st->restarts) != NULL;
    ok &= cJSON_AddBoolToObject(r, "storage_full", st->storage_full) != NULL;
    return ok;
}

static bool add_zones(cJSON *o, const uint16_t *zones, size_t count, size_t max)
{
    cJSON *z = cJSON_AddArrayToObject(o, "zones");
    bool ok = z != NULL;
    for (size_t k = 0; k < count && k < max; k++) {
        ok &= web_attach(z, NULL, cJSON_CreateNumber(zones[k]));
    }
    return ok;
}

/* A device as the list shows it (devices.md §14's row); page: its page when read (LQI, RSSI). */
static cJSON *device_row(ui_zb_device_t *d, const ui_zb_device_page_t *page)
{
    cJSON *e = cJSON_CreateObject();
    bool ok = e != NULL;
    ok &= cJSON_AddNumberToObject(e, "id", d->id) != NULL;
    ok &= add_text(e, "name", d->name, sizeof(d->name));
    ok &= add_text(e, "model", d->model, sizeof(d->model));
    ok &= cJSON_AddStringToObject(e, "state", WORD(DEV_STATE, d->state)) != NULL;
    ok &= cJSON_AddStringToObject(e, "kind", WORD(KIND, d->kind)) != NULL;
    ok &= cJSON_AddStringToObject(e, "power", WORD(POWER, d->power)) != NULL;
    ok &= cJSON_AddBoolToObject(e, "router", d->router) != NULL;
    if (d->battery_pct <= 100u) {
        ok &= cJSON_AddNumberToObject(e, "battery", d->battery_pct) != NULL;
    }
    ok &= cJSON_AddBoolToObject(e, "low", d->battery_low) != NULL;
    if (d->heard_s != UI_ZB_NEVER) {
        ok &= cJSON_AddNumberToObject(e, "heard_s", d->heard_s) != NULL;
    }
    ok &= cJSON_AddStringToObject(e, "signal", WORD(SIGNAL, d->signal)) != NULL;
    if (page != NULL) {
        ok &= cJSON_AddNumberToObject(e, "lqi", page->lqi) != NULL;
        ok &= cJSON_AddNumberToObject(e, "rssi", page->rssi) != NULL;
    }
    ok &= cJSON_AddBoolToObject(e, "unreachable", d->unreachable) != NULL;
    ok &= cJSON_AddNumberToObject(e, "device_class", d->device_class) != NULL;
    ok &= cJSON_AddStringToObject(e, "join", WORD(JOIN, d->join)) != NULL;
    ok &= cJSON_AddNumberToObject(e, "setup_done", d->setup_done) != NULL;
    ok &= cJSON_AddNumberToObject(e, "setup_total", d->setup_total) != NULL;
    ok &= add_zones(e, d->zones, d->zone_count, UI_ZB_ZONES_MAX);
    return built(e, ok);
}

/* A device's page (ui_zb_device_page_t): the row, its facts, its inputs and its tier B roles
 * (each only when the device can take it: `fit` not none). */
static cJSON *device_page(ui_zb_device_page_t *p)
{
    cJSON *e = device_row(&p->dev, p);
    bool ok = e != NULL;
    ok &= add_hex64(e, "ieee", p->ieee);
    ok &= cJSON_AddNumberToObject(e, "nwk", p->nwk) != NULL;
    ok &= add_text(e, "manufacturer", p->manufacturer, sizeof(p->manufacturer));
    ok &= add_text(e, "model_id", p->model_id, sizeof(p->model_id));
    ok &= add_text(e, "sw_build", p->sw_build, sizeof(p->sw_build));
    ok &= cJSON_AddNumberToObject(e, "joined_at", p->joined_at) != NULL;
    ok &= cJSON_AddBoolToObject(e, "sleepy", p->sleepy) != NULL;
    ok &= cJSON_AddNumberToObject(e, "check_in_s", p->check_in_s) != NULL;
    ok &= cJSON_AddNumberToObject(e, "setup_step", p->setup_step) != NULL;
    ok &= cJSON_AddBoolToObject(e, "can_identify", p->can_identify) != NULL;
    ok &= add_text(e, "wake_words", p->wake_words, sizeof(p->wake_words));
    ok &= add_text(e, "add_words", p->add_words, sizeof(p->add_words));
    ok &= add_text(e, "note", p->note, sizeof(p->note));
    cJSON *in = cJSON_AddArrayToObject(e, "inputs");
    ok &= in != NULL;
    for (size_t i = 0; i < p->input_count && i < UI_ZB_INPUTS_MAX; i++) {
        ui_zb_input_t *x = &p->inputs[i];
        cJSON *o = cJSON_CreateObject();
        ok &= o != NULL;
        ok &= cJSON_AddNumberToObject(o, "endpoint", x->endpoint) != NULL;
        ok &= cJSON_AddNumberToObject(o, "device_class", x->device_class) != NULL;
        ok &= add_text(o, "words", x->words, sizeof(x->words));
        ok &= add_text(o, "state", x->state, sizeof(x->state));
        ok &= cJSON_AddNumberToObject(o, "zone", x->zone) != NULL;
        ok &= web_attach(in, NULL, o);
    }
    if (p->siren.fit != UI_ZB_FIT_NONE) {
        const ui_zb_siren_t *s = &p->siren;
        cJSON *o = cJSON_AddObjectToObject(e, "siren");
        ok &= cJSON_AddStringToObject(o, "fit", WORD(FIT, s->fit)) != NULL;
        ok &= cJSON_AddNumberToObject(o, "partitions", s->partitions) != NULL;
        ok &= cJSON_AddBoolToObject(o, "burglary", s->burglary) != NULL;
        ok &= cJSON_AddBoolToObject(o, "fire", s->fire) != NULL;
        ok &= cJSON_AddBoolToObject(o, "strobe", s->strobe) != NULL;
        ok &= cJSON_AddBoolToObject(o, "squawk", s->squawk) != NULL;
        ok &= cJSON_AddNumberToObject(o, "volume", s->volume) != NULL;
        ok &= cJSON_AddNumberToObject(o, "supervision_min", s->supervision_min) != NULL;
        ok &= cJSON_AddBoolToObject(o, "live", s->live) != NULL;
        ok &= cJSON_AddStringToObject(o, "last_test", WORD(RESULT, s->last_test)) != NULL;
    }
    if (p->keypad.fit != UI_ZB_FIT_NONE) {
        const ui_zb_keypad_t *k = &p->keypad;
        cJSON *o = cJSON_AddObjectToObject(e, "keypad");
        ok &= cJSON_AddStringToObject(o, "fit", WORD(FIT, k->fit)) != NULL;
        ok &= cJSON_AddNumberToObject(o, "partition", k->partition) != NULL;
        ok &= cJSON_AddNumberToObject(o, "panic_mask", k->panic_mask) != NULL;
        ok &= cJSON_AddBoolToObject(o, "show_state", k->show_state) != NULL;
        ok &= cJSON_AddNumberToObject(o, "device", k->device) != NULL;
        ok &= cJSON_AddBoolToObject(o, "live", k->live) != NULL;
    }
    if (p->fob.fit != UI_ZB_FIT_NONE) {
        const ui_zb_fob_t *f = &p->fob;
        cJSON *o = cJSON_AddObjectToObject(e, "fob");
        ok &= cJSON_AddStringToObject(o, "fit", WORD(FIT, f->fit)) != NULL;
        ok &= cJSON_AddNumberToObject(o, "user", f->user) != NULL;
        ok &= cJSON_AddNumberToObject(o, "partition", f->partition) != NULL;
        ok &= cJSON_AddBoolToObject(o, "may_disarm", f->may_disarm) != NULL;
        ok &= cJSON_AddBoolToObject(o, "panic", f->panic) != NULL;
        ok &= cJSON_AddBoolToObject(o, "live", f->live) != NULL;
    }
    return built(e, ok);
}

static bool add_progress(cJSON *o, const ui_backend_t *b)
{
    ui_zb_progress_t g;
    memset(&g, 0, sizeof(g));
    if (b->zb_progress != NULL) {
        b->zb_progress(b->ctx, &g);
    }
    cJSON *p = cJSON_AddObjectToObject(o, "progress");
    bool ok = p != NULL;
    ok &= cJSON_AddStringToObject(p, "op", WORD(OP, g.op)) != NULL;
    ok &= cJSON_AddStringToObject(p, "step", WORD(STEP, g.step)) != NULL;
    ok &= cJSON_AddStringToObject(p, "result", WORD(RESULT, g.result)) != NULL;
    ok &= cJSON_AddNumberToObject(p, "seconds", g.seconds_left) != NULL;
    ok &= cJSON_AddNumberToObject(p, "id", g.id) != NULL;
    ok &= cJSON_AddNumberToObject(p, "old_id", g.old_id) != NULL;
    ok &= add_text(p, "model", g.model, sizeof(g.model));
    cJSON *j = cJSON_AddArrayToObject(p, "joined");
    ok &= j != NULL;
    for (size_t i = 0; i < g.joined_count && i < UI_ZB_JOINED_MAX; i++) {
        ui_zb_joined_t *x = &g.joined[i];
        if (x->id == 0u) {
            continue;
        }
        cJSON *e = cJSON_CreateObject();
        ok &= cJSON_AddNumberToObject(e, "id", x->id) != NULL;
        ok &= cJSON_AddStringToObject(e, "step", WORD(STEP, x->step)) != NULL;
        ok &= cJSON_AddStringToObject(e, "result", WORD(RESULT, x->result)) != NULL;
        ok &= cJSON_AddBoolToObject(e, "asleep", x->asleep) != NULL;
        ok &= add_text(e, "model", x->model, sizeof(x->model));
        ok &= web_attach(j, NULL, e);
    }
    ok &= add_zones(p, g.zones, g.zone_count, UI_ZB_ZONES_MAX);
    return ok;
}

cJSON *web_zigbee_get(const ui_backend_t *b, uint32_t device, bool may_change, bool *found)
{
    *found = true;
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    ui_zb_status_t st;
    memset(&st, 0, sizeof(st));
    const bool on = b->zb_status != NULL && b->zb_status(b->ctx, &st);
    if (cJSON_AddBoolToObject(o, "available", on) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    if (!on) {
        *found = device == 0u;
        return o;
    }
    bool ok = true;
    ok &= cJSON_AddBoolToObject(o, "may_change", may_change) != NULL;
    ok &= cJSON_AddStringToObject(o, "why", may_change ? "" : "armed") != NULL;
    ok &= add_radio(o, &st);

    ui_zb_device_page_t *page = web_alloc(sizeof(*page));
    ui_zb_device_t *d = web_alloc(UI_ZB_DEVICES_MAX * sizeof(*d));
    if (d == NULL || (device != 0u && page == NULL)) {
        /* BL-113: no memory is 503 "Memory busy" (NULL), never an empty list or a device that
         * reads as removed (404). Without `device`, the page only adds LQI and RSSI. */
        free(page);
        free(d);
        cJSON_Delete(o);
        return NULL;
    }
    cJSON *devs = cJSON_AddArrayToObject(o, "devices");
    ok &= devs != NULL;
    const size_t n = d != NULL && b->zb_devices != NULL
                         ? b->zb_devices(b->ctx, d, UI_ZB_DEVICES_MAX)
                         : 0u;
    for (size_t i = 0; i < n && i < UI_ZB_DEVICES_MAX; i++) {
        /* the radio details' LQI and RSSI are the page's: read it when there is memory for it */
        const bool read = page != NULL && b->zb_device != NULL &&
                          b->zb_device(b->ctx, d[i].id, page);
        ok &= web_attach(devs, NULL, device_row(&d[i], read ? page : NULL));
    }
    free(d);

    /* allocated once the device list is freed, so the two never take the heap at once */
    ui_zb_code_t *c = web_alloc(UI_ZB_CODES_MAX * sizeof(*c));
    if (c == NULL) {
        free(page);
        cJSON_Delete(o);
        return NULL; /* BL-113: 503 "Memory busy", never an empty list of codes */
    }
    cJSON *codes = cJSON_AddArrayToObject(o, "codes");
    ok &= codes != NULL;
    const size_t m = b->zb_codes != NULL ? b->zb_codes(b->ctx, c, UI_ZB_CODES_MAX) : 0u;
    for (size_t i = 0; i < m && i < UI_ZB_CODES_MAX; i++) {
        cJSON *e = cJSON_CreateObject();
        ok &= add_hex64(e, "ieee", c[i].ieee);
        ok &= cJSON_AddNumberToObject(e, "added", c[i].added_at) != NULL;
        ok &= web_attach(codes, NULL, e);
    }
    free(c);
    ok &= add_progress(o, b);

    if (device != 0u) {
        *found = device <= UINT16_MAX && page != NULL && b->zb_device != NULL &&
                 b->zb_device(b->ctx, (uint16_t)device, page);
        if (*found) {
            ok &= web_attach(o, "device", device_page(page));
        }
    }
    free(page);
    return built(o, ok); /* NULL: *found is still true unless the device was not there */
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : NULL;
}

/* A whole number in lo..hi; false when absent or not one. */
static bool jnum(const cJSON *o, const char *key, uint32_t lo, uint32_t hi, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < (double)lo || v->valuedouble > (double)hi ||
        v->valuedouble != (double)(uint32_t)v->valuedouble) {
        return false;
    }
    *out = (uint32_t)v->valuedouble;
    return true;
}

static bool jbool(const cJSON *o, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsBool(v)) {
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

/* 16 hex digits, most significant first, with spaces, ':' or '-' between them allowed. */
static bool parse_ieee(const char *s, uint64_t *out)
{
    uint64_t v = 0u;
    unsigned digits = 0u;
    for (; *s != '\0'; s++) {
        const char ch = *s;
        unsigned x;
        if (ch >= '0' && ch <= '9') {
            x = (unsigned)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            x = (unsigned)(ch - 'a') + 10u;
        } else if (ch >= 'A' && ch <= 'F') {
            x = (unsigned)(ch - 'A') + 10u;
        } else if (ch == ' ' || ch == ':' || ch == '-') {
            continue;
        } else {
            return false;
        }
        if (++digits > 16u) {
            return false;
        }
        v = v << 4 | x;
    }
    *out = v;
    return digits == 16u && v != 0u;
}

static bool read_siren(const cJSON *o, ui_zb_siren_t *s)
{
    uint32_t parts = 0u, volume = 0u, minutes = 0u;
    if (!cJSON_IsObject(o) || !jnum(o, "partitions", 0u, 0xFFu, &parts) ||
        !jnum(o, "volume", 0u, 3u, &volume) || !jnum(o, "supervision_min", 0u, 0xFFFFu, &minutes) ||
        !jbool(o, "burglary", &s->burglary) || !jbool(o, "fire", &s->fire) ||
        !jbool(o, "strobe", &s->strobe) || !jbool(o, "squawk", &s->squawk)) {
        return false;
    }
    if (minutes != 0u && minutes < 60u) {
        return false; /* never off: 0 the default, else 60 and up (zbz_wd.h) */
    }
    s->partitions = (uint8_t)parts;
    s->volume = (uint8_t)volume;
    s->supervision_min = (uint16_t)minutes;
    return true;
}

static bool read_keypad(const cJSON *o, ui_zb_keypad_t *k)
{
    uint32_t part = 0u, mask = 0u;
    if (!cJSON_IsObject(o) || !jnum(o, "partition", 0u, ALARM_MAX_PARTITIONS, &part) ||
        !jnum(o, "panic_mask", 0u, 0xFFu, &mask) || !jbool(o, "show_state", &k->show_state)) {
        return false;
    }
    k->partition = (uint8_t)part;
    k->panic_mask = (uint8_t)mask;
    return true;
}

static bool read_fob(const cJSON *o, ui_zb_fob_t *f)
{
    uint32_t user = 0u, part = 0u;
    if (!cJSON_IsObject(o) || !jnum(o, "user", 0u, ALARM_MAX_USERS, &user) ||
        !jnum(o, "partition", 0u, ALARM_MAX_PARTITIONS, &part) ||
        !jbool(o, "may_disarm", &f->may_disarm) || !jbool(o, "panic", &f->panic)) {
        return false;
    }
    f->user = (uint8_t)user;
    f->partition = (uint8_t)part;
    return true;
}

enum { NEED_ID = 1u, NEED_IEEE = 2u, MAY_IEEE = 4u, NEED_CODE = 8u };

static const struct {
    const char *op;
    uint8_t kind;
    uint8_t need;
} OPS[] = {
    {"add", UI_ZB_REQ_ADD, 0u},
    {"add_code", UI_ZB_REQ_ADD_CODE, MAY_IEEE | NEED_CODE}, /* a QR code's text carries both */
    {"keep", UI_ZB_REQ_KEEP_LISTENING, 0u},
    {"stop", UI_ZB_REQ_STOP, 0u},
    {"remove", UI_ZB_REQ_REMOVE, NEED_ID},
    {"forget", UI_ZB_REQ_FORGET, NEED_ID},
    {"replace", UI_ZB_REQ_REPLACE, NEED_ID},
    {"ping", UI_ZB_REQ_PING, NEED_ID},
    {"identify", UI_ZB_REQ_IDENTIFY, NEED_ID},
    {"setup", UI_ZB_REQ_SETUP, NEED_ID},
    {"rename", UI_ZB_REQ_RENAME, NEED_ID},
    {"code_add", UI_ZB_REQ_CODE_ADD, MAY_IEEE | NEED_CODE},
    {"code_forget", UI_ZB_REQ_CODE_FORGET, NEED_IEEE},
    {"survey", UI_ZB_REQ_SURVEY, 0u},
    {"channel", UI_ZB_REQ_CHANNEL, 0u},
    {"reset", UI_ZB_REQ_RESET, 0u}, /* the password re-asked by panel_web.c first */
    {"restart", UI_ZB_REQ_RESTART, 0u},
    {"retry", UI_ZB_REQ_RETRY, 0u},
    {"dismiss", UI_ZB_REQ_DISMISS, 0u},
    {"siren", UI_ZB_REQ_SIREN, NEED_ID},
    {"siren_test", UI_ZB_REQ_SIREN_TEST, NEED_ID},
    {"keypad", UI_ZB_REQ_KEYPAD, NEED_ID},
    {"fob", UI_ZB_REQ_FOB, NEED_ID},
};

/* The request the body asks for, or a result word when it can't be read. */
static const char *read_request(const cJSON *body, uint8_t user, ui_zb_request_t *req)
{
    const char *op = jstr(body, "op");
    size_t i = 0u;
    while (op != NULL && i < sizeof(OPS) / sizeof(OPS[0]) && strcmp(op, OPS[i].op) != 0) {
        i++;
    }
    if (op == NULL || i == sizeof(OPS) / sizeof(OPS[0])) {
        return "invalid";
    }
    req->kind = OPS[i].kind;
    req->user = user;
    uint32_t v = 0u;
    if ((OPS[i].need & NEED_ID) != 0u) {
        if (!jnum(body, "id", 1u, UINT16_MAX, &v)) {
            return "not_found"; /* id 0 or none names no device (devices.md §6) */
        }
        req->id = (uint16_t)v;
    }
    const char *ieee = jstr(body, "ieee");
    if ((OPS[i].need & (NEED_IEEE | MAY_IEEE)) != 0u && ieee != NULL && ieee[0] != '\0') {
        if (!parse_ieee(ieee, &req->ieee)) {
            return "bad_address";
        }
    } else if ((OPS[i].need & NEED_IEEE) != 0u) {
        return "bad_address";
    }
    if ((OPS[i].need & NEED_CODE) != 0u) {
        const char *code = jstr(body, "code");
        if (code == NULL || code[0] == '\0' || strlen(code) >= sizeof(req->code)) {
            return "bad_code";
        }
        memcpy(req->code, code, strlen(code) + 1u);
    }
    switch (req->kind) {
    case UI_ZB_REQ_RENAME: {
        const char *name = jstr(body, "name");
        if (name == NULL || strlen(name) >= sizeof(req->name)) {
            return "invalid";
        }
        memcpy(req->name, name, strlen(name) + 1u);
        break;
    }
    case UI_ZB_REQ_CHANNEL:
        if (!jnum(body, "channel", 11u, 26u, &v)) {
            return "invalid";
        }
        req->channel = (uint8_t)v;
        break;
    case UI_ZB_REQ_SIREN:
        if (!read_siren(cJSON_GetObjectItemCaseSensitive(body, "siren"), &req->siren)) {
            return "invalid";
        }
        break;
    case UI_ZB_REQ_KEYPAD:
        if (!read_keypad(cJSON_GetObjectItemCaseSensitive(body, "keypad"), &req->keypad)) {
            return "invalid";
        }
        break;
    case UI_ZB_REQ_FOB:
        if (!read_fob(cJSON_GetObjectItemCaseSensitive(body, "fob"), &req->fob)) {
            return "invalid";
        }
        break;
    default: break;
    }
    return NULL;
}

const char *web_zigbee_post(const ui_backend_t *b, const cJSON *body, uint8_t user)
{
    ui_zb_status_t st;
    if (b->zb_status == NULL || !b->zb_status(b->ctx, &st)) {
        return NULL; /* no Zigbee on this panel */
    }
    ui_zb_request_t *req = web_alloc(sizeof(*req));
    if (req == NULL) {
        return "busy";
    }
    const char *refused = read_request(body, user, req);
    /* allowed: panel_web decided it (a signed-in installer, the panel not armed) */
    const char *w = refused != NULL ? refused
                                    : web_zigbee_result_word(zigbee_service_web_request(req, true));
    mbedtls_platform_zeroize(req, sizeof(*req)); /* an install code passed through it */
    free(req);
    return w;
}

#else

cJSON *web_zigbee_get(const ui_backend_t *b, uint32_t device, bool may_change, bool *found)
{
    (void)b;
    (void)may_change;
    *found = device == 0u;
    cJSON *o = cJSON_CreateObject();
    if (o != NULL && cJSON_AddBoolToObject(o, "available", false) == NULL) {
        cJSON_Delete(o);
        o = NULL;
    }
    return o;
}

const char *web_zigbee_post(const ui_backend_t *b, const cJSON *body, uint8_t user)
{
    (void)b;
    (void)body;
    (void)user;
    return NULL;
}

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */
