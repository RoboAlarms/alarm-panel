/*
 * zwave_dev.c - the Z-Wave service on the developer link (docs/features/30 "Z-Wave"; dev_remote's
 * adapter, wired in the firmware's dev_adapter.c): the state as JSON, and requests from JSON
 * through the request slot the console uses. The frame trace is zw_trace.c's.
 *
 * The words are the developer link's contract (docs/features/30): panelctl reads them, so they
 * change only by adding. Every answer is bounded (ZWAVE_SERVICE_*_JSON_MAX in zwave_service.h);
 * the request goes through the strict JSON reader (alarm_proto's proto_json.h). Requests change the
 * network only with every partition disarmed, as on the console (zwz_panel decides which need it);
 * the network reset needs "confirm": true; there is no way to send a frame of one's own, and the
 * keys never appear - not here, not in the log.
 */
#include "zwave_internal.h"

#if CONFIG_ALARM_ZSRC_ZWAVE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_proto/proto_json.h"
#include "esp_log.h"
#include "zone_service.h"

static const char *TAG = "zwave";

/* ---- the words ------------------------------------------------------------------------------ */

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *const RADIO[] = {"not_connected", "starting",  "working", "not_answering",
                                    "unusable",      "foreign",   "replaced", "updating"};
static const char *const SECURITY[] = {"unknown",   "none",    "s0",          "s2_unauth",
                                       "s2_auth",   "s2_access", "unconfirmed", "none_capable"};
static const char *const DEVICE[] = {"ready",      "setting_up", "wake_needed", "no_reports",
                                     "not_usable", "setup_stopped"};
static const char *const POWER[] = {"unknown", "battery", "mains"};
static const char *const OP[] = {"none",     "add",      "remove", "remove_failed",
                                 "replace",  "ping",     "identify", "routes",
                                 "check_in", "region",   "reset",  "restart"};
static const char *const STEP[] = {"idle",      "starting", "listening", "connecting", "ask_allow",
                                   "ask_pin",   "securing", "reading",   "working",    "done"};
static const char *const RESULT[] = {
    "none",          "added",           "added_insecure",  "downgraded",    "no_reports",
    "nothing_joined", "join_failed",    "already_added",   "controller",    "stopped",
    "removed",       "removed_foreign", "nothing_removed", "still_answers", "answered",
    "no_answer",     "done",            "failed",          "setup_paused",  "timed_out",
    "refused",       "fw_updated",      "fw_failed",       "radio_restarted"};
static const char *const ANSWER[] = {"ok", "not_allowed", "busy", "no_radio", "invalid",
                                     "unsupported", "low_battery", "full"};

/* An enum value appended in ui_backend.h needs its word here first. */
_Static_assert(COUNT(RADIO) == UI_ZW_RADIO_UPDATING + 1, "a word for every radio state");
_Static_assert(COUNT(SECURITY) == UI_ZW_SEC_NONE_CAPABLE + 1, "a word for every security class");
_Static_assert(COUNT(DEVICE) == UI_ZW_DEV_SETUP_STOPPED + 1, "a word for every device state");
_Static_assert(COUNT(POWER) == UI_ZW_POWER_MAINS + 1, "a word for every power source");
_Static_assert(COUNT(OP) == UI_ZW_OP_RESTART + 1, "a word for every operation");
_Static_assert(COUNT(STEP) == UI_ZW_STEP_DONE + 1, "a word for every step");
_Static_assert(COUNT(RESULT) == UI_ZW_RES_RADIO_RESTARTED + 1, "a word for every result");
_Static_assert(COUNT(ANSWER) == UI_ZW_REQ_FULL + 1, "a word for every request result");

/* serial-api.md §6.7's regions, as the developer link names them. */
static const struct {
    uint8_t code;
    const char *id;
} REGIONS[] = {
    {0x00u, "eu"}, {0x01u, "us"}, {0x02u, "anz"},   {0x03u, "hk"},    {0x05u, "in"}, {0x06u, "il"},
    {0x07u, "ru"}, {0x08u, "cn"}, {0x09u, "us_lr"}, {0x0Bu, "eu_lr"}, {0x20u, "jp"}, {0x21u, "kr"},
};

/* The classes of security, in the order the lists give them. */
static const struct {
    uint8_t bit;
    const char *id;
} KEYS[] = {
    {UI_ZW_KEY_S2_ACCESS, "access"},
    {UI_ZW_KEY_S2_AUTH, "auth"},
    {UI_ZW_KEY_S2_UNAUTH, "unauth"},
    {UI_ZW_KEY_S0, "s0"},
};

static const char *word_of(const char *const *words, size_t n, uint8_t v)
{
    return v < n ? words[v] : "";
}
#define WORD(table, v) word_of((table), COUNT(table), (v))

static const char *crypto_word(zwsvc_crypto_t c)
{
    switch (c) {
    case ZWSVC_CRYPTO_PASSED: return "passed";
    case ZWSVC_CRYPTO_FAILED: return "failed";
    default: return "not_run";
    }
}

static void key_list(pj_writer_t *w, const char *key, uint8_t keys)
{
    pj_karr(w, key);
    for (size_t i = 0; i < COUNT(KEYS); i++) {
        if ((keys & KEYS[i].bit) != 0u) {
            pj_str(w, KEYS[i].id);
        }
    }
    pj_arr_end(w);
}

/* ---- GET /dev/v1/zwave ---------------------------------------------------------------------- */

static void radio_json(pj_writer_t *w, const ui_zw_status_t *st)
{
    const zwave_service_config_t *c = zwsvc_config();
    uint8_t chip = 0u;
    uint16_t node_id = 0u;
    if (zwsvc_running()) {
        zone_service_lock();
        const zw_module_facts_t *f = zw_host_facts(&zwsvc_panel()->net.host);
        chip = f->chip_type;
        node_id = f->node_id;
        zone_service_unlock();
    }
    /* sized for any value of their fields: 10 digits each */
    char firmware[24] = "";
    char sdk[36] = "";
    char home[12] = "";
    const char *region = "";
    if (st->firmware_major != 0u || st->firmware_minor != 0u) {
        snprintf(firmware, sizeof firmware, "%u.%02u", (unsigned)st->firmware_major,
                 (unsigned)st->firmware_minor);
    }
    if (st->sdk_major != 0u || st->sdk_minor != 0u || st->sdk_patch != 0u) {
        snprintf(sdk, sizeof sdk, "%u.%u.%u", (unsigned)st->sdk_major, (unsigned)st->sdk_minor,
                 (unsigned)st->sdk_patch);
    }
    if (st->home_id != 0u) {
        snprintf(home, sizeof home, "%08" PRIx32, st->home_id);
    }
    for (size_t i = 0; st->region_known && i < COUNT(REGIONS); i++) {
        if (REGIONS[i].code == st->region) {
            region = REGIONS[i].id;
        }
    }
    pj_kobj(w, "radio");
    pj_kstr(w, "state", WORD(RADIO, st->radio));
    pj_kstr(w, "place", c->place != NULL ? c->place : "");
    pj_kstr(w, "firmware", firmware);
    pj_kstr(w, "sdk", sdk);
    pj_kuint(w, "chip", chip);
    pj_kstr(w, "region", region);
    pj_kstr(w, "home_id", home);
    pj_kuint(w, "node_id", node_id);
    pj_kuint(w, "devices", st->devices);
    pj_kuint(w, "check_in_s", st->check_in_s);
    pj_kbool(w, "update_advised", st->update_advised);
    pj_kstr(w, "crypto", crypto_word(zwsvc_crypto_check()));
    /* the tasks' stacks never used, bytes (the bench's), 0 of 0: not started */
    zwsvc_stats_t ss;
    zwsvc_stats(&ss);
    pj_kuint(w, "stack_free", ss.stack_free);
    pj_kuint(w, "stack_size", ss.stack_size);
    pj_kuint(w, "link_stack_free", ss.link_stack_free);
    pj_kuint(w, "link_stack_size", ss.link_stack_size);
    pj_kuint(w, "fw_stack_free", ss.fw_stack_free);
    pj_kuint(w, "fw_stack_size", ss.fw_stack_size);
    pj_obj_end(w);
}

/* page: the device's own page, for its check-in (NULL when it couldn't be read: 0s). */
static void device_json(pj_writer_t *w, ui_zw_device_t *d, const ui_zw_device_page_t *page)
{
    d->model[UI_ZW_MODEL_LEN - 1u] = '\0';
    pj_obj(w);
    pj_kuint(w, "node", d->node);
    pj_kuint(w, "generation", d->generation);
    pj_kstr(w, "model", d->model);
    pj_kstr(w, "security", WORD(SECURITY, d->security));
    pj_kbool(w, "listening", d->listening);
    pj_kbool(w, "flirs", d->flirs);
    pj_kstr(w, "power", WORD(POWER, d->power));
    pj_kuint(w, "setup_done", d->setup_done);
    pj_kuint(w, "setup_total", d->setup_total);
    pj_kint(w, "battery", d->battery_pct == UI_ZW_BATTERY_UNKNOWN ? -1 : (int64_t)d->battery_pct);
    pj_kbool(w, "battery_low", d->battery_low);
    pj_kint(w, "heard_s", d->heard_s == UI_ZW_NEVER ? -1 : (int64_t)d->heard_s);
    pj_kstr(w, "state", WORD(DEVICE, d->state));
    pj_kbool(w, "unreachable", d->unreachable);
    /* its check-in now (0: listening, or never) and one chosen for its next wake-up (0: none) */
    pj_kuint(w, "check_in_s", page != NULL ? page->check_in_s : 0u);
    pj_kuint(w, "check_in_next_s", page != NULL ? page->check_in_next_s : 0u);
    pj_karr(w, "zones");
    for (uint8_t z = 0u; z < d->zone_count && z < UI_ZW_ZONES_MAX; z++) {
        pj_uint(w, d->zones[z]);
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

static void progress_json(pj_writer_t *w, ui_zw_progress_t *g)
{
    g->dsk[UI_ZW_DSK_LEN - 1u] = '\0';
    pj_kobj(w, "progress");
    pj_kstr(w, "op", WORD(OP, g->op));
    pj_kstr(w, "step", WORD(STEP, g->step));
    pj_kuint(w, "seconds_left", g->seconds_left);
    pj_kstr(w, "result", WORD(RESULT, g->result));
    pj_kuint(w, "node", g->node);
    pj_kuint(w, "generation", g->generation);
    key_list(w, "requested", g->requested);
    pj_kstr(w, "dsk", g->dsk);
    pj_obj_end(w);
}

size_t zwave_service_state_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    ui_zw_device_t *list = alarm_mem_slow_calloc(UI_ZW_DEVICES_MAX, sizeof(*list));
    ui_zw_device_page_t *page = alarm_mem_slow_calloc(1u, sizeof(*page));
    if (list == NULL || page == NULL) {
        free(list);
        free(page);
        return 0u;
    }
    /* What the pages read, the same way (the radio block too before the service started). */
    ui_zw_status_t st;
    ui_zw_progress_t g;
    (void)zwave_service_ui_status(&st);
    const size_t n = zwave_service_ui_devices(list, UI_ZW_DEVICES_MAX);
    zwave_service_ui_progress(&g);
    pj_writer_t w;
    pj_init(&w, out, cap);
    pj_obj(&w);
    pj_kint(&w, "api", 1);
    radio_json(&w, &st);
    pj_karr(&w, "devices");
    for (size_t i = 0; i < n && i < UI_ZW_DEVICES_MAX; i++) {
        const bool have = zwave_service_ui_device(list[i].node, list[i].generation, page);
        device_json(&w, &list[i], have ? page : NULL);
    }
    pj_arr_end(&w);
    progress_json(&w, &g);
    pj_obj_end(&w);
    free(list);
    free(page);
    return pj_finish(&w);
}

/* ---- POST /dev/v1/zwave --------------------------------------------------------------------- */

typedef enum {
    F_OP,
    F_KEYS,
    F_PIN,
    F_NODE,
    F_GENERATION,
    F_SECONDS,
    F_REGION,
    F_CONFIRM,
    F_COUNT,
} field_t;
static const char *const FIELDS[F_COUNT] = {"op",      "keys",   "pin",    "node",
                                            "generation", "seconds", "region", "confirm"};

typedef struct {
    uint16_t seen; /* bit per field */
    bool bad;      /* a field of the wrong type, or a word it does not know: "invalid" */
    char op[16];
    uint8_t keys;
    char pin[8];
    int64_t node, generation, seconds;
    char region[8];
    bool confirm;
} body_t;

/* What each operation takes besides "op". */
typedef enum {
    TAKES_NOTHING,
    TAKES_KEYS,    /* allow: "keys" (none given: what the device asked for, as on the console) */
    TAKES_PIN,     /* pin: "pin", 5 digits as a string */
    TAKES_DEVICE,  /* "node" 1..65535, "generation" (none given: the node's now, as the console) */
    TAKES_CHECKIN, /* "node" (0: new devices), "generation", "seconds" */
    TAKES_REGION,  /* "region", one of REGIONS */
    TAKES_CONFIRM, /* "confirm": true */
} takes_t;

#define KIND_PULSE 0u /* not a ui_zw_request_t: the module's reset line (ZWSVC_REQ_PULSE) */

static const struct {
    const char *op;
    uint8_t kind; /* ui_zw_req_kind_t */
    uint8_t takes;
} OPS[] = {
    {"add", UI_ZW_REQ_ADD, TAKES_NOTHING},
    {"remove", UI_ZW_REQ_REMOVE, TAKES_NOTHING},
    {"stop", UI_ZW_REQ_STOP, TAKES_NOTHING},
    {"keep_listening", UI_ZW_REQ_KEEP_LISTENING, TAKES_NOTHING},
    {"allow", UI_ZW_REQ_ALLOW, TAKES_KEYS},
    {"pin", UI_ZW_REQ_PIN, TAKES_PIN},
    {"insecure", UI_ZW_REQ_INSECURE, TAKES_NOTHING},
    {"remove_failed", UI_ZW_REQ_REMOVE_FAILED, TAKES_DEVICE},
    {"setup", UI_ZW_REQ_SETUP, TAKES_DEVICE},
    {"ping", UI_ZW_REQ_PING, TAKES_DEVICE},
    {"check_in", UI_ZW_REQ_CHECK_IN, TAKES_CHECKIN},
    {"region", UI_ZW_REQ_REGION, TAKES_REGION},
    {"restart", UI_ZW_REQ_RESTART, TAKES_NOTHING},
    {"reset_network", UI_ZW_REQ_RESET, TAKES_CONFIRM},
    {"dismiss", UI_ZW_REQ_DISMISS, TAKES_NOTHING},
    {"pulse_reset", KIND_PULSE, TAKES_NOTHING},
};

static bool has(const body_t *b, field_t f)
{
    return (b->seen & (1u << f)) != 0u;
}

/* "keys": an array of the words KEYS knows. */
static bool read_keys(pj_reader_t *r, uint8_t *out)
{
    *out = 0u;
    if (pj_peek(r) != PJ_ARRAY) {
        pj_skip(r);
        return false;
    }
    bool ok = pj_enter_arr(r);
    char id[8];
    while (pj_next_element(r)) {
        size_t k = 0;
        if (pj_get_str(r, id, sizeof id)) {
            while (k < COUNT(KEYS) && strcmp(id, KEYS[k].id) != 0) {
                k++;
            }
        } else {
            k = COUNT(KEYS);
        }
        if (k < COUNT(KEYS)) {
            *out |= KEYS[k].bit;
        } else {
            ok = false;
        }
    }
    return ok && !r->error;
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
        case F_KEYS: ok = read_keys(&r, &b->keys); break;
        case F_PIN: ok = pj_get_str(&r, b->pin, sizeof b->pin); break;
        case F_NODE: ok = pj_get_int(&r, &b->node); break;
        case F_GENERATION: ok = pj_get_int(&r, &b->generation); break;
        case F_SECONDS: ok = pj_get_int(&r, &b->seconds); break;
        case F_REGION: ok = pj_get_str(&r, b->region, sizeof b->region); break;
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

static bool in_range(int64_t v, int64_t lo, int64_t hi)
{
    return v >= lo && v <= hi;
}

/* "node" and "generation" into req; the node's generation now when none is given. */
static bool device_of(const body_t *b, ui_zw_request_t *req, int64_t lowest_node)
{
    if (!has(b, F_NODE) || !in_range(b->node, lowest_node, UINT16_MAX)) {
        return false;
    }
    req->node = (uint16_t)b->node;
    if (has(b, F_GENERATION)) {
        if (!in_range(b->generation, 0, UINT16_MAX)) {
            return false;
        }
        req->generation = (uint16_t)b->generation;
    } else if (req->node != 0u && zwsvc_running()) {
        zone_service_lock();
        req->generation = zwz_panel_generation(zwsvc_panel(), req->node);
        zone_service_unlock();
    }
    return true;
}

/* The fields the operation takes, into r; false: missing or out of range ("invalid"). */
static bool fill(const body_t *b, uint8_t takes, zwsvc_req_t *r)
{
    ui_zw_request_t *q = &r->ui;
    switch ((takes_t)takes) {
    case TAKES_KEYS:
        if (has(b, F_KEYS)) {
            q->keys = b->keys;
        } else {
            ui_zw_progress_t g;
            zwave_service_ui_progress(&g);
            q->keys = g.requested;
        }
        return true;
    case TAKES_PIN: {
        if (!has(b, F_PIN) || strlen(b->pin) != 5u) {
            return false;
        }
        uint32_t pin = 0u;
        for (size_t i = 0; i < 5u; i++) {
            if (b->pin[i] < '0' || b->pin[i] > '9') {
                return false;
            }
            pin = pin * 10u + (uint32_t)(b->pin[i] - '0');
        }
        if (pin > UI_ZW_PIN_MAX) {
            return false; /* 5 digits, but no 16-bit key group: refused, never truncated */
        }
        q->pin = pin;
        return true;
    }
    case TAKES_DEVICE: return device_of(b, q, 1);
    case TAKES_CHECKIN:
        if (!device_of(b, q, 0) || !has(b, F_SECONDS) || !in_range(b->seconds, 1, UINT32_MAX)) {
            return false;
        }
        q->seconds = (uint32_t)b->seconds;
        return true;
    case TAKES_REGION:
        for (size_t i = 0; has(b, F_REGION) && i < COUNT(REGIONS); i++) {
            if (strcmp(b->region, REGIONS[i].id) == 0) {
                q->region = REGIONS[i].code;
                return true;
            }
        }
        return false;
    case TAKES_CONFIRM: return has(b, F_CONFIRM) && b->confirm;
    default: return true;
    }
}

bool zwave_service_request_json(const char *body, size_t len, char *out, size_t cap)
{
    body_t b;
    if (body == NULL || out == NULL || cap < ZWAVE_SERVICE_ANSWER_JSON_MAX ||
        !parse(body, len, &b) || !has(&b, F_OP)) {
        return false;
    }
    size_t i = 0;
    while (i < COUNT(OPS) && strcmp(b.op, OPS[i].op) != 0) {
        i++;
    }
    if (i == COUNT(OPS)) {
        return false;
    }
    zwsvc_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = OPS[i].kind == KIND_PULSE ? ZWSVC_REQ_PULSE : ZWSVC_REQ_UI;
    r.ui.kind = OPS[i].kind;
    ui_zw_req_result_t result = UI_ZW_REQ_INVALID;
    if (!b.bad && fill(&b, OPS[i].takes, &r)) {
        r.allowed = zone_service_all_disarmed(); /* as the console asks */
        result = zwsvc_submit(&r);
    }
    ESP_LOGI(TAG, "developer link: %s: %s", OPS[i].op, WORD(ANSWER, (uint8_t)result));
    pj_writer_t w;
    pj_init(&w, out, cap);
    pj_obj(&w);
    pj_kstr(&w, "result", WORD(ANSWER, (uint8_t)result));
    pj_obj_end(&w);
    return pj_finish(&w) > 0u; /* {"result":"unsupported"} fits ZWAVE_SERVICE_ANSWER_JSON_MAX */
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
