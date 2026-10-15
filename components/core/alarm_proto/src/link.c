/*
 * link.c - the local Home Assistant link, version 1: framing, hex, the pairing code, the
 * panel's messages and the strict parse of the client's (proto_link.h, docs/features/23
 * protocol.md). The keys and the derivation are an external contract shared with the
 * aioroboalarms Python client.
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"

/* ---- hex ---------------------------------------------------------------------------------- */

void proto_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0fu];
    }
    out[2 * len] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool proto_unhex(const char *hex, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex[2 * i] != '\0' ? hex_nibble(hex[2 * i + 1]) : -1;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return hex[2 * len] == '\0';
}

/* ---- framing ------------------------------------------------------------------------------ */

void proto_link_frame_header(uint32_t payload_len, uint8_t out[PROTO_LINK_HDR_LEN])
{
    out[0] = (uint8_t)(payload_len >> 24);
    out[1] = (uint8_t)(payload_len >> 16);
    out[2] = (uint8_t)(payload_len >> 8);
    out[3] = (uint8_t)payload_len;
}

bool proto_link_frame_length(const uint8_t hdr[PROTO_LINK_HDR_LEN], uint32_t *payload_len)
{
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) |
                   (uint32_t)hdr[3];
    *payload_len = len;
    return len >= 1u && len <= PROTO_LINK_FRAME_MAX;
}

/* ---- pairing code (HAI-003, protocol.md "Code derivation") -------------------------------- */

void proto_link_pair_code(const uint8_t nonce_ha[PROTO_LINK_NONCE_LEN],
                          const uint8_t nonce_panel[PROTO_LINK_NONCE_LEN],
                          const uint8_t fp_ha[PROTO_LINK_FP_LEN],
                          const uint8_t fp_panel[PROTO_LINK_FP_LEN], char out[PROTO_LINK_CODE_LEN])
{
    static const char context[] = "roboalarms-pair-v1"; /* 18 bytes, no NUL */
    uint8_t input[18 + 4 * 32];
    memcpy(input, context, 18);
    memcpy(input + 18, nonce_ha, PROTO_LINK_NONCE_LEN);
    memcpy(input + 50, nonce_panel, PROTO_LINK_NONCE_LEN);
    memcpy(input + 82, fp_ha, PROTO_LINK_FP_LEN);
    memcpy(input + 114, fp_panel, PROTO_LINK_FP_LEN);
    uint8_t digest[32];
    proto_sha256(input, sizeof(input), digest);
    uint32_t v = ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) |
                 ((uint32_t)digest[2] << 8) | (uint32_t)digest[3];
    v %= 1000000u;
    for (size_t i = 0; i < 6; i++) {
        out[5 - i] = (char)('0' + v % 10u);
        v /= 10u;
    }
    out[6] = '\0';
}

/* ---- messages the panel sends ------------------------------------------------------------- */

void proto_link_hello(pj_writer_t *w, const proto_ident_t *id, const char *name, bool paired)
{
    pj_obj(w);
    pj_kstr(w, "t", "hello");
    pj_kint(w, "api", PROTO_LINK_API);
    pj_kstr(w, "id", id->panel_id);
    pj_kstr(w, "name", name != NULL ? name : "");
    pj_kstr(w, "model", id->model);
    pj_kstr(w, "fw", id->fw);
    pj_kbool(w, "paired", paired);
    pj_obj_end(w);
}

void proto_link_pair_nonce(pj_writer_t *w, const uint8_t nonce[PROTO_LINK_NONCE_LEN])
{
    char hex[2 * PROTO_LINK_NONCE_LEN + 1];
    proto_hex(nonce, PROTO_LINK_NONCE_LEN, hex);
    pj_obj(w);
    pj_kstr(w, "t", "pair_nonce");
    pj_kstr(w, "nonce", hex);
    pj_obj_end(w);
}

void proto_link_pair_result(pj_writer_t *w, const char *result)
{
    pj_obj(w);
    pj_kstr(w, "t", "pair_result");
    pj_kstr(w, "result", result);
    pj_obj_end(w);
}

void proto_link_pong(pj_writer_t *w)
{
    pj_obj(w);
    pj_kstr(w, "t", "pong");
    pj_obj_end(w);
}

/* ---- state for the paired Home Assistant -------------------------------------------------- */

static void state_arrays(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                         const proto_when_t *when, uint8_t partitions, const uint8_t *zones)
{
    pj_karr(w, "partitions");
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        if (cfg->partitions[p - 1u].enabled && (partitions & (1u << (p - 1u))) != 0u) {
            proto_partition_status(w, cfg, snap, p, when);
        }
    }
    pj_arr_end(w);
    pj_karr(w, "zones");
    if (zones != NULL) {
        for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
            if (cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED &&
                (zones[(z - 1u) / 8u] & (1u << ((z - 1u) % 8u))) != 0u) {
                proto_zone_state(w, cfg, snap, z);
            }
        }
    }
    pj_arr_end(w);
}

void proto_link_snapshot(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                         const proto_when_t *when)
{
    uint8_t all_zones[ALARM_ZONE_BITMAP_BYTES];
    memset(all_zones, 0xFF, sizeof(all_zones));
    pj_obj(w);
    pj_kstr(w, "t", "snapshot");
    pj_kuint(w, "seq", snap->seq);
    state_arrays(w, cfg, snap, when, 0xFFu, all_zones);
    pj_key(w, "troubles");
    proto_troubles(w, cfg, snap);
    pj_obj_end(w);
}

void proto_link_delta(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                      const proto_when_t *when, uint8_t partitions, const uint8_t *zones,
                      bool with_troubles)
{
    pj_obj(w);
    pj_kstr(w, "t", "delta");
    pj_kuint(w, "seq", snap->seq);
    state_arrays(w, cfg, snap, when, partitions, zones);
    if (with_troubles) {
        pj_key(w, "troubles");
        proto_troubles(w, cfg, snap);
    }
    pj_obj_end(w);
}

void proto_link_event(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev,
                      bool replayed)
{
    pj_obj(w);
    pj_kstr(w, "t", "event");
    proto_event_fields(w, cfg, ev, replayed);
    pj_obj_end(w);
}

void proto_link_result(pj_writer_t *w, const proto_cmd_t *cmd, alarm_result_t result,
                       const alarm_cmd_result_t *res)
{
    pj_obj(w);
    pj_kstr(w, "t", "result");
    proto_cmd_result_fields(w, cmd, result, res);
    pj_obj_end(w);
}

/* ---- two-way: Home Assistant entities as zones (M13, protocol.md) ------------------------- */

void proto_link_watch(pj_writer_t *w, const char *const *ids, size_t n)
{
    pj_obj(w);
    pj_kstr(w, "t", "watch");
    pj_karr(w, "ids");
    for (size_t i = 0; i < n; i++) {
        pj_str(w, ids[i]);
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

/* ---- a controllable entity's values (protocol.md "the Devices screen") ------------------- */

void proto_link_values_none(proto_link_values_t *v)
{
    v->level = (int8_t)PROTO_LINK_LEVEL_UNKNOWN;
    v->modes = 0u;
    v->setpoint = PROTO_LINK_TEMP_UNKNOWN;
    v->temp = PROTO_LINK_TEMP_UNKNOWN;
    v->caps = 0u;
    v->hvac_action = 0u;
    v->since = 0u;
}

static uint8_t mode_bit(const char *word)
{
    static const struct {
        const char *word;
        uint8_t bit;
    } map[] = {
        {"off", PROTO_LINK_MODE_OFF},   {"heat", PROTO_LINK_MODE_HEAT},
        {"cool", PROTO_LINK_MODE_COOL}, {"auto", PROTO_LINK_MODE_AUTO},
        {"heat_cool", PROTO_LINK_MODE_HEAT_COOL},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(word, map[i].word) == 0) {
            return map[i].bit;
        }
    }
    return 0u; /* "dry", "fan_only", a word from a later Home Assistant: not shown here */
}

/* "modes": the words the panel can show; another type, or more than PROTO_LINK_MODES_MAX
 * elements, is not known (0). */
static uint8_t read_modes(pj_reader_t *r)
{
    if (pj_peek(r) != PJ_ARRAY) {
        pj_skip(r);
        return 0u;
    }
    if (!pj_enter_arr(r)) {
        return 0u;
    }
    uint8_t modes = 0u;
    size_t n = 0;
    while (pj_next_element(r)) {
        char word[12]; /* "heat_cool" and a little room; a longer word is no mode of ours */
        n++;
        if (n > PROTO_LINK_MODES_MAX) {
            pj_skip(r);
        } else if (pj_get_str(r, word, sizeof(word))) {
            modes = (uint8_t)(modes | mode_bit(word));
        }
    }
    return n > PROTO_LINK_MODES_MAX ? 0u : modes;
}

static uint8_t supports_bit(const char *word)
{
    static const struct {
        const char *word;
        uint8_t bit;
    } map[] = {
        {"stop", PROTO_LINK_CAP_STOP},
        {"set_position", PROTO_LINK_CAP_POSITION},
        {"set_percentage", PROTO_LINK_CAP_PERCENTAGE},
        {"brightness", PROTO_LINK_CAP_BRIGHTNESS},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(word, map[i].word) == 0) {
            return map[i].bit;
        }
    }
    return 0u; /* a word from a later client: ignored, the list still counts as known */
}

/* "supports": PROTO_LINK_CAP_KNOWN and the words the panel knows; another type, or more than
 * PROTO_LINK_SUPPORTS_MAX elements, is not known (0). */
static uint8_t read_supports(pj_reader_t *r)
{
    if (pj_peek(r) != PJ_ARRAY) {
        pj_skip(r);
        return 0u;
    }
    if (!pj_enter_arr(r)) {
        return 0u;
    }
    uint8_t caps = PROTO_LINK_CAP_KNOWN;
    size_t n = 0;
    while (pj_next_element(r)) {
        char word[16]; /* "set_percentage" and a little room; a longer word is none of ours */
        n++;
        if (n > PROTO_LINK_SUPPORTS_MAX) {
            pj_skip(r);
        } else if (pj_get_str(r, word, sizeof(word))) {
            caps = (uint8_t)(caps | supports_bit(word));
        }
    }
    return n > PROTO_LINK_SUPPORTS_MAX ? 0u : caps;
}

/* "hvac_action": PROTO_LINK_HVAC_* for Home Assistant's word; another word or type is 0. */
static uint8_t read_hvac_action(pj_reader_t *r)
{
    static const char *const words[] = {
        "off", "heating", "cooling", "drying", "idle", "fan", "preheating", "defrosting",
    };
    char word[12]; /* "preheating" and a little room */
    if (!pj_get_str(r, word, sizeof(word))) {
        return 0u;
    }
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(word, words[i]) == 0) {
            return (uint8_t)(i + 1u);
        }
    }
    return 0u;
}

static int32_t read_temp(pj_reader_t *r)
{
    int64_t v = 0;
    if (!pj_get_int(r, &v) || v < -PROTO_LINK_TEMP_LIMIT || v > PROTO_LINK_TEMP_LIMIT) {
        return PROTO_LINK_TEMP_UNKNOWN;
    }
    return (int32_t)v;
}

/* One member that is a value: read it into v and return true; false for any other key (its
 * value still waits to be read). */
static bool read_value_member(pj_reader_t *r, const char *key, proto_link_values_t *v)
{
    if (strcmp(key, "level") == 0) {
        int64_t level = 0;
        v->level = (pj_get_int(r, &level) && level >= 0 && level <= 100)
                       ? (int8_t)level
                       : (int8_t)PROTO_LINK_LEVEL_UNKNOWN;
    } else if (strcmp(key, "setpoint") == 0) {
        v->setpoint = read_temp(r);
    } else if (strcmp(key, "temp") == 0) {
        v->temp = read_temp(r);
    } else if (strcmp(key, "modes") == 0) {
        v->modes = read_modes(r);
    } else if (strcmp(key, "supports") == 0) {
        v->caps = read_supports(r);
    } else if (strcmp(key, "hvac_action") == 0) {
        v->hvac_action = read_hvac_action(r);
    } else if (strcmp(key, "since") == 0) {
        int64_t s = 0;
        v->since = (pj_get_int(r, &s) && s >= 1 && s <= (int64_t)UINT32_MAX) ? (uint32_t)s : 0u;
    } else {
        return false;
    }
    return true;
}

bool proto_link_catalog_init(proto_link_catalog_t *c, const char *json, size_t len)
{
    memset(c, 0, sizeof(*c));
    pj_read_init(&c->r, json, len);
    if (!pj_enter_obj(&c->r)) {
        return false;
    }
    bool is_catalog = false;
    char key[16];
    while (pj_next_member(&c->r, key, sizeof(key))) {
        if (strcmp(key, "t") == 0) {
            char type[16] = "";
            is_catalog = pj_get_str(&c->r, type, sizeof(type)) && strcmp(type, "catalog") == 0;
        } else if (strcmp(key, "page") == 0) {
            if (!pj_get_int(&c->r, &c->page)) {
                c->page = 0;
            }
        } else if (strcmp(key, "more") == 0) {
            if (!pj_get_bool(&c->r, &c->more)) {
                c->more = false;
            }
        } else if (strcmp(key, "entities") == 0) {
            /* protocol.md: page and more come before the entities */
            c->in_entities = pj_enter_arr(&c->r);
            return is_catalog && c->in_entities;
        } else {
            pj_skip(&c->r);
        }
    }
    return is_catalog; /* a catalog without entities: an empty page */
}

bool proto_link_catalog_next(proto_link_catalog_t *c, proto_link_entity_t *out)
{
    if (!c->in_entities || !pj_next_element(&c->r)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->zones = true; /* protocol.md: absent means it may feed a zone */
    proto_link_values_none(&out->values);
    if (!pj_enter_obj(&c->r)) {
        return false;
    }
    char key[12];
    while (pj_next_member(&c->r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            if (!pj_get_str(&c->r, out->id, sizeof(out->id))) {
                out->id[0] = '\0'; /* an overlong id can't be a binding: skip the entity */
            }
        } else if (strcmp(key, "name") == 0) {
            (void)pj_get_str(&c->r, out->name, sizeof(out->name));
        } else if (strcmp(key, "area") == 0) {
            (void)pj_get_str(&c->r, out->area, sizeof(out->area));
        } else if (strcmp(key, "domain") == 0) {
            (void)pj_get_str(&c->r, out->domain, sizeof(out->domain));
        } else if (strcmp(key, "class") == 0) {
            (void)pj_get_str(&c->r, out->class_, sizeof(out->class_));
        } else if (strcmp(key, "state") == 0) {
            (void)pj_get_str(&c->r, out->state, sizeof(out->state));
        } else if (strcmp(key, "zones") == 0) {
            if (!pj_get_bool(&c->r, &out->zones)) {
                out->zones = true;
            }
        } else if (strcmp(key, "control") == 0) {
            if (!pj_get_bool(&c->r, &out->control)) {
                out->control = false;
            }
        } else if (!read_value_member(&c->r, key, &out->values)) {
            pj_skip(&c->r);
        }
    }
    return !c->r.error;
}

bool proto_link_parse_state(const char *json, size_t len, char id[ZSRC_REF_LEN_LINK],
                            char state[24], bool *available, proto_link_values_t *values)
{
    id[0] = '\0';
    state[0] = '\0';
    *available = true;
    proto_link_values_t unused;
    if (values == NULL) {
        values = &unused;
    }
    proto_link_values_none(values);
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char key[12]; /* "setpoint" and room: a longer key matches nothing */
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            if (!pj_get_str(&r, id, ZSRC_REF_LEN_LINK)) {
                id[0] = '\0';
            }
        } else if (strcmp(key, "state") == 0) {
            if (!pj_get_str(&r, state, 24)) {
                state[0] = '\0'; /* an overlong state maps like unknown */
            }
        } else if (strcmp(key, "avail") == 0) {
            if (!pj_get_bool(&r, available)) {
                *available = true;
            }
        } else if (!read_value_member(&r, key, values)) {
            pj_skip(&r);
        }
    }
    return !r.error && pj_finished(&r) && id[0] != '\0';
}

/* ---- the Devices screen (M13 second half, protocol.md) ------------------------------------ */

void proto_link_call(pj_writer_t *w, const char *id, const char *entity, const char *action,
                     bool has_value, int32_t value, const char *mode)
{
    pj_obj(w);
    pj_kstr(w, "t", "call");
    pj_kstr(w, "id", id);
    pj_kstr(w, "entity", entity);
    pj_kstr(w, "action", action);
    if (has_value) {
        pj_kint(w, "value", value);
    }
    if (mode != NULL && mode[0] != '\0') {
        pj_kstr(w, "mode", mode);
    }
    pj_obj_end(w);
}

void proto_link_zwave_devices(pj_writer_t *w, const proto_zw_device_t *d, size_t n)
{
    pj_obj(w);
    pj_kstr(w, "t", "zwave_devices");
    pj_karr(w, "devices");
    for (size_t i = 0; d != NULL && i < n; i++) {
        pj_obj(w);
        pj_kstr(w, "key", d[i].key);
        pj_kstr(w, "kind", d[i].kind == PROTO_ZW_LIGHT    ? "light"
                           : d[i].kind == PROTO_ZW_GARAGE ? "garage"
                                                          : "switch");
        pj_kstr(w, "name", d[i].name);
        if (d[i].level < 0) {
            pj_knull(w, "level");
        } else {
            pj_kint(w, "level", d[i].level);
        }
        pj_kbool(w, "online", d[i].online);
        if (d[i].kind == PROTO_ZW_GARAGE) {
            pj_kstr(w, "state", d[i].garage[0] != '\0' ? d[i].garage : "unknown");
        }
        pj_obj_end(w);
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

bool proto_link_parse_zwave_set(const char *json, size_t len, char id[PROTO_LINK_CALL_ID_LEN],
                                char key[PROTO_ZW_KEY_LEN], int *level)
{
    id[0] = '\0';
    key[0] = '\0';
    *level = -1;
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char name[8];
    int64_t v = -1;
    while (pj_next_member(&r, name, sizeof(name))) {
        if (strcmp(name, "id") == 0) {
            if (!pj_get_str(&r, id, PROTO_LINK_CALL_ID_LEN)) {
                id[0] = '\0';
            }
        } else if (strcmp(name, "key") == 0) {
            if (!pj_get_str(&r, key, PROTO_ZW_KEY_LEN)) {
                key[0] = '\0';
            }
        } else if (strcmp(name, "level") == 0) {
            if (!pj_get_int(&r, &v)) {
                v = -1;
            }
        } else {
            pj_skip(&r);
        }
    }
    if (r.error || !pj_finished(&r) || id[0] == '\0' || key[0] == '\0' || v < 0 || v > 100) {
        return false;
    }
    *level = (int)v;
    return true;
}

void proto_link_zwave_result(pj_writer_t *w, const char *id, bool ok, const char *error)
{
    pj_obj(w);
    pj_kstr(w, "t", "zwave_result");
    pj_kstr(w, "id", id);
    pj_kbool(w, "ok", ok);
    pj_kstr(w, "error", ok || error == NULL ? "" : error);
    pj_obj_end(w);
}

bool proto_link_parse_call_result(const char *json, size_t len,
                                  char id[PROTO_LINK_CALL_ID_LEN], bool *ok, char error[24])
{
    id[0] = '\0';
    error[0] = '\0';
    *ok = false;
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char key[8];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            if (!pj_get_str(&r, id, PROTO_LINK_CALL_ID_LEN)) {
                id[0] = '\0';
            }
        } else if (strcmp(key, "ok") == 0) {
            if (!pj_get_bool(&r, ok)) {
                *ok = false;
            }
        } else if (strcmp(key, "error") == 0) {
            if (!pj_get_str(&r, error, 24)) {
                snprintf(error, 24, "failed"); /* an overlong reason is still a failure */
            }
        } else {
            pj_skip(&r);
        }
    }
    return !r.error && pj_finished(&r) && id[0] != '\0';
}

/* ---- messages the panel receives ---------------------------------------------------------- */

bool proto_link_parse(const char *json, size_t len, proto_link_in_t *out)
{
    memset(out, 0, sizeof(*out));
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char type[16] = "";
    char hexbuf[2 * PROTO_LINK_NONCE_LEN + 2];
    bool saw_type = false;
    bool has_commit = false;
    bool has_nonce = false;
    uint8_t commit[PROTO_LINK_NONCE_LEN] = {0};
    uint8_t nonce[PROTO_LINK_NONCE_LEN] = {0};
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "t") == 0) {
            saw_type = true;
            if (!pj_get_str(&r, type, sizeof(type))) {
                type[0] = '\0'; /* longer than any known type: unknown */
            }
        } else if (strcmp(key, "api") == 0) {
            if (!pj_get_int(&r, &out->api)) {
                out->api = 0;
            }
        } else if (strcmp(key, "client") == 0) {
            if (!pj_get_str(&r, out->client, sizeof(out->client))) {
                out->client[0] = '\0'; /* longer than the field: keep "" */
            }
        } else if (strcmp(key, "commit") == 0) {
            has_commit = pj_get_str(&r, hexbuf, sizeof(hexbuf)) &&
                         proto_unhex(hexbuf, commit, PROTO_LINK_NONCE_LEN);
        } else if (strcmp(key, "nonce") == 0) {
            has_nonce = pj_get_str(&r, hexbuf, sizeof(hexbuf)) &&
                        proto_unhex(hexbuf, nonce, PROTO_LINK_NONCE_LEN);
        } else {
            pj_skip(&r);
        }
    }
    if (r.error || !pj_finished(&r)) {
        return false;
    }
    if (strcmp(type, "hello") == 0) {
        out->type = PROTO_LINK_IN_HELLO;
        return out->api >= 1;
    }
    if (strcmp(type, "ping") == 0) {
        out->type = PROTO_LINK_IN_PING;
        return true;
    }
    if (strcmp(type, "pair_start") == 0) {
        out->type = PROTO_LINK_IN_PAIR_START;
        memcpy(out->commit, commit, sizeof(out->commit));
        return has_commit;
    }
    if (strcmp(type, "pair_reveal") == 0) {
        out->type = PROTO_LINK_IN_PAIR_REVEAL;
        memcpy(out->nonce, nonce, sizeof(out->nonce));
        return has_nonce;
    }
    if (strcmp(type, "command") == 0) {
        out->type = PROTO_LINK_IN_COMMAND; /* the caller re-parses with proto_cmd_parse */
        return true;
    }
    if (strcmp(type, "catalog") == 0) {
        out->type = PROTO_LINK_IN_CATALOG; /* the caller walks it with proto_link_catalog_* */
        return true;
    }
    if (strcmp(type, "state") == 0) {
        out->type = PROTO_LINK_IN_STATE; /* the caller uses proto_link_parse_state */
        return true;
    }
    if (strcmp(type, "call_result") == 0) {
        out->type = PROTO_LINK_IN_CALL_RESULT; /* proto_link_parse_call_result */
        return true;
    }
    if (strcmp(type, "zwave_set") == 0) {
        out->type = PROTO_LINK_IN_ZWAVE_SET; /* proto_link_parse_zwave_set */
        return true;
    }
    if (!saw_type) {
        return false; /* no "t" at all */
    }
    out->type = PROTO_LINK_IN_UNKNOWN;
    return true;
}
