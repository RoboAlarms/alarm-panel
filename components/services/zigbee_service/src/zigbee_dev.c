/*
 * zigbee_dev.c - the Zigbee service on the developer link (docs/features/30; devices.md §12,
 * BL-143 ZB-M6): the state as JSON, and requests from JSON through the request slot the pages
 * and the console use. dev_remote serves them as /dev/v1/zigbee; tools/panelctl.py `zigbee`
 * reads them.
 *
 * The words are the developer link's contract: panelctl reads them, so they change only by
 * adding (the radio's are proto_zb.h's, the answers the web API's, devices.md §14). Every answer
 * is bounded (ZIGBEE_SERVICE_*_JSON_MAX in zigbee_service.h); the request goes through the strict
 * JSON reader (alarm_proto's proto_json.h). Requests change the network only with every partition
 * disarmed, as on the console (zbz_panel decides which need it); forget and a channel move need
 * "confirm": true, and there is no network reset here: its installer code is never sent over the
 * network (devices.md §14). No install code is ever written back.
 */
#include "zigbee_internal.h"

#if CONFIG_ALARM_ZSRC_ZIGBEE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_proto/proto_json.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "zone_service.h"

static const char *TAG = "zigbee";

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the words ------------------------------------------------------------------------------ */

static const char *const RADIO[] = {"not_connected", "starting",     "working", "not_answering",
                                    "paused",        "socket_zwave", "unusable"};
static const char *const PAUSE[] = {"none", "firmware", "reset", "no_answer"};
static const char *const DEVICE[] = {"ready",      "setting_up",    "wake_needed",
                                     "setup_stopped", "not_usable", "joining"};
static const char *const KIND[] = {"device", "sensor",   "siren", "keypad",
                                   "keyfob", "repeater", "other"};
static const char *const POWER[] = {"unknown", "battery", "mains"};
static const char *const SIGNAL[] = {"unknown", "good", "fair", "weak"};
static const char *const JOIN[] = {"unknown", "install_code", "standard_key"};
static const char *const OP[] = {"none",     "add",      "add_code", "remove", "forget",
                                 "replace",  "ping",     "identify", "setup",  "channel",
                                 "survey",   "reset",    "restart",  "siren_test"};
static const char *const STEP[] = {"idle",    "starting",   "listening", "joining",
                                   "reading", "setting_up", "working",   "done"};
static const char *const RESULT[] = {
    "none",    "added",     "nothing_joined", "join_failed", "already_added", "not_a_sensor",
    "setup_not_finished", "stopped", "removed", "no_answer", "forgotten",     "replaced",
    "answered", "done",     "failed",         "full"};
static const char *const ANSWER[] = {"ok",       "not_allowed", "busy",        "no_radio",
                                     "invalid",  "unsupported", "bad_code",    "checksum",
                                     "bad_address", "full",     "not_found",   "in_use",
                                     "wrong_code", "locked_out"};

/* An enum value appended in ui_backend.h needs its word here first. */
_Static_assert(COUNT(RADIO) == UI_ZB_RADIO_UNUSABLE + 1, "a word for every radio state");
_Static_assert(COUNT(PAUSE) == UI_ZB_PAUSE_NO_ANSWER + 1, "a word for every pause");
_Static_assert(COUNT(DEVICE) == UI_ZB_DEV_JOINING + 1, "a word for every device state");
_Static_assert(COUNT(KIND) == UI_ZB_KIND_OTHER + 1, "a word for every kind");
_Static_assert(COUNT(POWER) == UI_ZB_POWER_MAINS + 1, "a word for every power source");
_Static_assert(COUNT(SIGNAL) == UI_ZB_SIGNAL_WEAK + 1, "a word for every signal");
_Static_assert(COUNT(JOIN) == UI_ZB_JOIN_STANDARD_KEY + 1, "a word for every join");
_Static_assert(COUNT(OP) == UI_ZB_OP_SIREN_TEST + 1, "a word for every operation");
_Static_assert(COUNT(STEP) == UI_ZB_STEP_DONE + 1, "a word for every step");
_Static_assert(COUNT(RESULT) == UI_ZB_RES_FULL + 1, "a word for every result");
_Static_assert(COUNT(ANSWER) == UI_ZB_REQ_LOCKED_OUT + 1, "a word for every request result");
/* The longest answers (11 letters) fit ZIGBEE_SERVICE_ANSWER_JSON_MAX with the NUL. */
_Static_assert(sizeof("{\"result\":\"unsupported\"}") <= ZIGBEE_SERVICE_ANSWER_JSON_MAX,
               "every answer fits");

static const struct {
    const char *const *words;
    size_t n;
} TABLES[ZBSVC_W_COUNT] = {
    [ZBSVC_W_RADIO] = {RADIO, COUNT(RADIO)},    [ZBSVC_W_PAUSE] = {PAUSE, COUNT(PAUSE)},
    [ZBSVC_W_DEVICE] = {DEVICE, COUNT(DEVICE)}, [ZBSVC_W_KIND] = {KIND, COUNT(KIND)},
    [ZBSVC_W_POWER] = {POWER, COUNT(POWER)},    [ZBSVC_W_SIGNAL] = {SIGNAL, COUNT(SIGNAL)},
    [ZBSVC_W_JOIN] = {JOIN, COUNT(JOIN)},       [ZBSVC_W_OP] = {OP, COUNT(OP)},
    [ZBSVC_W_STEP] = {STEP, COUNT(STEP)},       [ZBSVC_W_RESULT] = {RESULT, COUNT(RESULT)},
    [ZBSVC_W_ANSWER] = {ANSWER, COUNT(ANSWER)},
};

const char *zbsvc_word(zbsvc_words_t table, uint8_t v)
{
    if ((unsigned)table >= (unsigned)ZBSVC_W_COUNT || v >= TABLES[table].n) {
        return "";
    }
    return TABLES[table].words[v];
}
#define WORD(t, v) zbsvc_word((t), (uint8_t)(v))

bool zbsvc_parse_ieee(const char *s, uint64_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    uint64_t v = 0u;
    unsigned digits = 0u;
    for (; *s != '\0'; s++) {
        const char c = *s;
        if (c == ':' || c == '-') {
            continue;
        }
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
        if (++digits > 16u) {
            return false;
        }
        v = (v << 4) | d;
    }
    if (digits != 16u) {
        return false;
    }
    *out = v;
    return true;
}

static void ieee_text(uint64_t ieee, char out[17])
{
    static const char HEX[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16u; i++) {
        out[i] = HEX[(ieee >> (60u - 4u * i)) & 0xFu];
    }
    out[16] = '\0';
}

/* ---- GET /dev/v1/zigbee --------------------------------------------------------------------- */

static void radio_json(pj_writer_t *w, ui_zb_status_t *st, const zbsvc_stats_t *stats)
{
    char ext[17], ieee[17];
    ieee_text(st->ext_pan_id, ext);
    ieee_text(st->panel_ieee, ieee);
    st->radio_version[sizeof(st->radio_version) - 1u] = '\0';
    pj_kobj(w, "radio");
    pj_kstr(w, "state", WORD(ZBSVC_W_RADIO, st->radio));
    pj_kstr(w, "paused", WORD(ZBSVC_W_PAUSE, st->paused_why));
    pj_kbool(w, "jammed", st->jammed);
    if (st->network_known) {
        pj_kuint(w, "channel", st->channel);
        pj_kuint(w, "pan", st->pan_id);
        pj_kstr(w, "ext_pan", ext);
        pj_kstr(w, "ieee", ieee);
    } else {
        pj_knull(w, "channel");
        pj_knull(w, "pan");
        pj_knull(w, "ext_pan");
        pj_knull(w, "ieee");
    }
    pj_kstr(w, "version", st->radio_version);
    if (st->noise_known) {
        pj_kint(w, "noise", st->noise_dbm);
    } else {
        pj_knull(w, "noise");
    }
    pj_karr(w, "survey"); /* the channels 11, 15, 20, 25: dBm, null not measured */
    for (unsigned i = 0; i < UI_ZB_CHANNELS; i++) {
        if ((st->survey_known & (1u << i)) != 0u) {
            pj_int(w, st->survey_dbm[i]);
        } else {
            pj_null(w);
        }
    }
    pj_arr_end(w);
    pj_kuint(w, "quietest", st->quietest);
    pj_kuint(w, "restarts", st->restarts);
    pj_kuint(w, "learn_s", st->learn_s);
    pj_kbool(w, "storage_full", st->storage_full);
    pj_kuint(w, "devices", st->devices);
    pj_kuint(w, "not_heard", st->not_heard);
    pj_kuint(w, "codes_waiting", st->codes_waiting);
    /* the service's own (the bench's): the task's stack never used, NVS's free entries */
    pj_kuint(w, "stack_free", stats->stack_free);
    pj_kuint(w, "stack_size", stats->stack_size);
    pj_kuint(w, "nvs_free", stats->nvs_free);
    /* radio_port's queue since start: events dropped, alarm-path events lost (README "Rules") */
    pj_kuint(w, "events_dropped", stats->radio_dropped);
    pj_kuint(w, "alarm_lost", stats->alarm_lost);
    pj_obj_end(w);
}

static void zones_json(pj_writer_t *w, const uint16_t *zones, uint8_t count)
{
    pj_karr(w, "zones");
    for (uint8_t z = 0u; z < count && z < UI_ZB_ZONES_MAX; z++) {
        pj_uint(w, zones[z]);
    }
    pj_arr_end(w);
}

/* page: the device's own page (NULL when it couldn't be read). */
static void device_json(pj_writer_t *w, ui_zb_device_t *d, const ui_zb_device_page_t *page)
{
    d->name[UI_ZB_NAME_LEN - 1u] = '\0';
    d->model[UI_ZB_MODEL_LEN - 1u] = '\0';
    char ieee[17] = "";
    if (page != NULL) {
        ieee_text(page->ieee, ieee);
    }
    pj_obj(w);
    pj_kuint(w, "id", d->id);
    pj_kstr(w, "ieee", ieee);
    pj_kstr(w, "name", d->name);
    pj_kstr(w, "model", d->model);
    pj_kstr(w, "state", WORD(ZBSVC_W_DEVICE, d->state));
    pj_kstr(w, "kind", WORD(ZBSVC_W_KIND, d->kind));
    pj_kstr(w, "power", WORD(ZBSVC_W_POWER, d->power));
    pj_kbool(w, "router", d->router);
    pj_kint(w, "battery", d->battery_pct == UI_ZB_BATTERY_UNKNOWN ? -1 : (int64_t)d->battery_pct);
    pj_kbool(w, "low", d->battery_low);
    pj_kint(w, "heard_s", d->heard_s == UI_ZB_NEVER ? -1 : (int64_t)d->heard_s);
    pj_kstr(w, "signal", WORD(ZBSVC_W_SIGNAL, d->signal));
    pj_kuint(w, "lqi", page != NULL ? page->lqi : 0u);
    pj_kint(w, "rssi", page != NULL ? page->rssi : 0);
    pj_kbool(w, "unreachable", d->unreachable);
    pj_kstr(w, "join", WORD(ZBSVC_W_JOIN, d->join));
    pj_kuint(w, "setup_done", d->setup_done);
    pj_kuint(w, "setup_total", d->setup_total);
    zones_json(w, d->zones, d->zone_count);
    pj_obj_end(w);
}

static void progress_json(pj_writer_t *w, ui_zb_progress_t *g)
{
    pj_kobj(w, "progress");
    pj_kstr(w, "op", WORD(ZBSVC_W_OP, g->op));
    pj_kstr(w, "step", WORD(ZBSVC_W_STEP, g->step));
    pj_kstr(w, "result", WORD(ZBSVC_W_RESULT, g->result));
    pj_kuint(w, "seconds", g->seconds_left);
    pj_kuint(w, "id", g->id);
    pj_kuint(w, "old_id", g->old_id);
    pj_karr(w, "joined");
    for (uint8_t i = 0u; i < g->joined_count && i < UI_ZB_JOINED_MAX; i++) {
        pj_obj(w);
        pj_kuint(w, "id", g->joined[i].id);
        pj_kstr(w, "step", WORD(ZBSVC_W_STEP, g->joined[i].step));
        pj_kstr(w, "result", WORD(ZBSVC_W_RESULT, g->joined[i].result));
        pj_kbool(w, "asleep", g->joined[i].asleep);
        pj_obj_end(w);
    }
    pj_arr_end(w);
    zones_json(w, g->zones, g->zone_count);
    pj_obj_end(w);
}

size_t zigbee_service_state_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    ui_zb_device_t *list = alarm_mem_slow_calloc(UI_ZB_DEVICES_MAX, sizeof(*list));
    ui_zb_device_page_t *page = alarm_mem_slow_calloc(1u, sizeof(*page));
    ui_zb_code_t *codes = alarm_mem_slow_calloc(UI_ZB_CODES_MAX, sizeof(*codes));
    ui_zb_progress_t *g = alarm_mem_slow_calloc(1u, sizeof(*g));
    if (list == NULL || page == NULL || codes == NULL || g == NULL) {
        free(list);
        free(page);
        free(codes);
        free(g);
        return 0u;
    }
    /* What the pages read, the same way (the radio block too before the service started). */
    ui_zb_status_t st;
    (void)zigbee_service_ui_status(&st);
    zbsvc_stats_t stats;
    zbsvc_stats(&stats);
    const size_t n = zigbee_service_ui_devices(list, UI_ZB_DEVICES_MAX);
    const size_t nc = zigbee_service_ui_codes(codes, UI_ZB_CODES_MAX);
    zigbee_service_ui_progress(g);
    pj_writer_t w;
    pj_init(&w, out, cap);
    pj_obj(&w);
    pj_kint(&w, "api", 1);
    radio_json(&w, &st, &stats);
    pj_karr(&w, "devices");
    for (size_t i = 0; i < n && i < UI_ZB_DEVICES_MAX; i++) {
        const bool have = zigbee_service_ui_device(list[i].id, page);
        device_json(&w, &list[i], have ? page : NULL);
    }
    pj_arr_end(&w);
    progress_json(&w, g);
    pj_karr(&w, "codes"); /* the addresses waiting, never the codes */
    for (size_t i = 0; i < nc && i < UI_ZB_CODES_MAX; i++) {
        char ieee[17];
        ieee_text(codes[i].ieee, ieee);
        pj_obj(&w);
        pj_kstr(&w, "ieee", ieee);
        pj_kuint(&w, "added", codes[i].added_at);
        pj_obj_end(&w);
    }
    pj_arr_end(&w);
    pj_obj_end(&w);
    free(list);
    free(page);
    free(codes);
    free(g);
    return pj_finish(&w);
}

/* ---- POST /dev/v1/zigbee -------------------------------------------------------------------- */

typedef enum {
    F_OP,
    F_ID,
    F_IEEE,
    F_CODE,
    F_NAME,
    F_CHANNEL,
    F_CONFIRM,
    F_COUNT,
} field_t;
static const char *const FIELDS[F_COUNT] = {"op",   "id",      "ieee",   "code",
                                            "name", "channel", "confirm"};

typedef struct {
    uint16_t seen; /* bit per field */
    bool bad;      /* a field of the wrong type or too long: "invalid" */
    char op[16];
    int64_t id, channel;
    char ieee[24];
    char code[UI_ZB_CODE_TEXT];
    char name[UI_ZB_NAME_LEN];
    bool confirm;
} body_t;

/* What each operation takes besides "op". */
typedef enum {
    TAKES_NOTHING,
    TAKES_ID,         /* "id" 1..65535 */
    TAKES_ID_CONFIRM, /* "id", "confirm": true */
    TAKES_RENAME,     /* "id", "name" ("" its default) */
    TAKES_CODE,       /* "code" (a QR code's text, or hex), "ieee" (may be left out with a QR) */
    TAKES_IEEE,       /* "ieee" */
    TAKES_CHANNEL,    /* "channel", "confirm": true */
} takes_t;

static const struct {
    const char *op;
    uint8_t kind; /* ui_zb_req_kind_t */
    uint8_t takes;
} OPS[] = {
    {"add", UI_ZB_REQ_ADD, TAKES_NOTHING},
    {"add_code", UI_ZB_REQ_ADD_CODE, TAKES_CODE},
    {"keep", UI_ZB_REQ_KEEP_LISTENING, TAKES_NOTHING},
    {"stop", UI_ZB_REQ_STOP, TAKES_NOTHING},
    {"remove", UI_ZB_REQ_REMOVE, TAKES_ID},
    {"forget", UI_ZB_REQ_FORGET, TAKES_ID_CONFIRM},
    {"replace", UI_ZB_REQ_REPLACE, TAKES_ID},
    {"ping", UI_ZB_REQ_PING, TAKES_ID},
    {"identify", UI_ZB_REQ_IDENTIFY, TAKES_ID},
    {"setup", UI_ZB_REQ_SETUP, TAKES_ID},
    {"rename", UI_ZB_REQ_RENAME, TAKES_RENAME},
    {"code_add", UI_ZB_REQ_CODE_ADD, TAKES_CODE},
    {"code_forget", UI_ZB_REQ_CODE_FORGET, TAKES_IEEE},
    {"survey", UI_ZB_REQ_SURVEY, TAKES_NOTHING},
    {"channel", UI_ZB_REQ_CHANNEL, TAKES_CHANNEL},
    {"restart", UI_ZB_REQ_RESTART, TAKES_NOTHING},
    {"retry", UI_ZB_REQ_RETRY, TAKES_NOTHING},
    {"dismiss", UI_ZB_REQ_DISMISS, TAKES_NOTHING},
};

static bool has(const body_t *b, field_t f)
{
    return (b->seen & (1u << f)) != 0u;
}

/* The body through the strict reader: false when it is not one JSON object, when "op" is not a
 * string, or when it names a field twice (which one would count?). Members it does not know are
 * passed over. */
static bool parse(const char *json, size_t len, body_t *b)
{
    memset(b, 0, sizeof *b);
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof key)) {
        unsigned f = 0u;
        while (f < F_COUNT && strcmp(key, FIELDS[f]) != 0) {
            f++;
        }
        if (f == F_COUNT) {
            pj_skip(&r);
            continue;
        }
        if (has(b, (field_t)f)) {
            return false;
        }
        b->seen = (uint16_t)(b->seen | (1u << f));
        bool ok = false;
        switch ((field_t)f) {
        case F_OP: ok = pj_get_str(&r, b->op, sizeof b->op); break;
        case F_ID: ok = pj_get_int(&r, &b->id); break;
        case F_IEEE: ok = pj_get_str(&r, b->ieee, sizeof b->ieee); break;
        case F_CODE: ok = pj_get_str(&r, b->code, sizeof b->code); break;
        case F_NAME: ok = pj_get_str(&r, b->name, sizeof b->name); break;
        case F_CHANNEL: ok = pj_get_int(&r, &b->channel); break;
        case F_CONFIRM: ok = pj_get_bool(&r, &b->confirm); break;
        default: pj_skip(&r); break;
        }
        if (r.error || (!ok && f == F_OP)) {
            return false;
        }
        if (!ok) {
            b->bad = true;
        }
    }
    return pj_finished(&r);
}

static bool id_of(const body_t *b, ui_zb_request_t *q)
{
    if (!has(b, F_ID) || b->id < 1 || b->id > UINT16_MAX) {
        return false;
    }
    q->id = (uint16_t)b->id;
    return true;
}

/* The fields the operation takes, into q; false: missing or out of range ("invalid"). A wrong
 * address answers "bad_address" (*why). */
static bool fill(const body_t *b, uint8_t takes, ui_zb_request_t *q, ui_zb_req_result_t *why)
{
    switch ((takes_t)takes) {
    case TAKES_ID: return id_of(b, q);
    case TAKES_ID_CONFIRM: return id_of(b, q) && has(b, F_CONFIRM) && b->confirm;
    case TAKES_RENAME:
        if (!id_of(b, q) || !has(b, F_NAME)) {
            return false;
        }
        memcpy(q->name, b->name, sizeof(q->name));
        return true;
    case TAKES_CODE:
        if (!has(b, F_CODE) || b->code[0] == '\0') {
            return false;
        }
        if (has(b, F_IEEE) && !zbsvc_parse_ieee(b->ieee, &q->ieee)) {
            *why = UI_ZB_REQ_BAD_ADDRESS;
            return false;
        }
        memcpy(q->code, b->code, sizeof(q->code));
        return true;
    case TAKES_IEEE:
        if (!has(b, F_IEEE) || !zbsvc_parse_ieee(b->ieee, &q->ieee)) {
            *why = UI_ZB_REQ_BAD_ADDRESS;
            return false;
        }
        return true;
    case TAKES_CHANNEL:
        if (!has(b, F_CHANNEL) || b->channel < ALARM_ZB_CHANNEL_MIN ||
            b->channel > ALARM_ZB_CHANNEL_MAX || !has(b, F_CONFIRM) || !b->confirm) {
            return false;
        }
        q->channel = (uint8_t)b->channel;
        return true;
    default: return true;
    }
}

bool zigbee_service_request_json(const char *body, size_t len, char *out, size_t cap)
{
    static EXT_RAM_BSS_ATTR body_t b; /* the HTTPS worker's: one request at a time */
    static EXT_RAM_BSS_ATTR ui_zb_request_t q;
    if (body == NULL || out == NULL || cap < ZIGBEE_SERVICE_ANSWER_JSON_MAX ||
        !parse(body, len, &b) || !has(&b, F_OP)) {
        memset(&b, 0, sizeof b); /* an install code may have passed through it */
        return false;
    }
    size_t i = 0;
    while (i < COUNT(OPS) && strcmp(b.op, OPS[i].op) != 0) {
        i++;
    }
    if (i == COUNT(OPS)) {
        memset(&b, 0, sizeof b);
        return false;
    }
    memset(&q, 0, sizeof q);
    q.kind = OPS[i].kind;
    q.user = 0u; /* the developer link is the panel's own, as the console */
    ui_zb_req_result_t result = UI_ZB_REQ_INVALID;
    if (!b.bad && fill(&b, OPS[i].takes, &q, &result)) {
        result = zbsvc_submit(&q, zone_service_all_disarmed()); /* as the console asks */
    }
    memset(&b, 0, sizeof b);
    memset(&q, 0, sizeof q);
    ESP_LOGI(TAG, "developer link: %s: %s", OPS[i].op, WORD(ZBSVC_W_ANSWER, result));
    pj_writer_t w;
    pj_init(&w, out, cap);
    pj_obj(&w);
    pj_kstr(&w, "result", WORD(ZBSVC_W_ANSWER, result));
    pj_obj_end(&w);
    return pj_finish(&w) > 0u;
}

#else /* CONFIG_ALARM_ZSRC_ZIGBEE */

#include <stddef.h>

size_t zigbee_service_state_json(char *out, size_t cap)
{
    (void)out;
    (void)cap;
    return 0u;
}

bool zigbee_service_request_json(const char *body, size_t len, char *out, size_t cap)
{
    (void)body;
    (void)len;
    (void)out;
    (void)cap;
    return false;
}

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */
