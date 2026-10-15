/*
 * discovery.c - Home Assistant MQTT discovery documents (see proto.h and docs/features/10).
 *
 * Each entity is its own retained config under homeassistant/<component>/<node>/<object>/config.
 * Every document carries the full device block (retained configs arrive in any order) and uses
 * "~" for the panel's base topic to stay small.
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "alarm_proto/proto.h"
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_zb.h"
#include "alarm_proto/proto_zw.h"

#define DEVICE_NAME  "Alarm Panel"
#define MANUFACTURER "RoboAlarms Panel (open source)"
#define ORIGIN_NAME  "RoboAlarms Panel"

/* "<prefix>_<panel_id>" with anything but letters, digits, '-' and '_' turned into '_'. */
static void node_id(const proto_ident_t *id, char out[PROTO_PREFIX_LEN + PROTO_PANEL_ID_LEN])
{
    const size_t cap = PROTO_PREFIX_LEN + PROTO_PANEL_ID_LEN;
    const int n = snprintf(out, cap, "%s_%s", id->prefix, id->panel_id);
    const size_t len = (n > 0 && (size_t)n < cap) ? (size_t)n : 0u;
    out[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        const char c = out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            out[i] = '_';
        }
    }
}

bool proto_ha_topic(const proto_ident_t *id, const char *component, const char *object,
                    char *out, size_t cap)
{
    char node[PROTO_PREFIX_LEN + PROTO_PANEL_ID_LEN];
    node_id(id, node);
    const int n = snprintf(out, cap, PROTO_HA_DISCOVERY "/%s/%s/%s/config", component, node,
                           object);
    return n > 0 && (size_t)n < cap;
}

const char *proto_ha_device_class(alarm_device_class_t cls)
{
    switch (cls) {
    case ALARM_DEVCLASS_DOOR: return "door";
    case ALARM_DEVCLASS_WINDOW: return "window";
    case ALARM_DEVCLASS_GARAGE_DOOR: return "garage_door";
    case ALARM_DEVCLASS_MOTION: return "motion";
    case ALARM_DEVCLASS_GLASS_BREAK: return "safety";
    case ALARM_DEVCLASS_SMOKE: return "smoke";
    case ALARM_DEVCLASS_HEAT: return "heat";
    case ALARM_DEVCLASS_CO: return "carbon_monoxide";
    case ALARM_DEVCLASS_GAS: return "gas";
    case ALARM_DEVCLASS_WATER: return "moisture";
    case ALARM_DEVCLASS_TEMPERATURE: return "cold";
    case ALARM_DEVCLASS_VIBRATION: return "vibration";
    case ALARM_DEVCLASS_PANIC_BUTTON: return "safety";
    default: return NULL;
    }
}

/* Fields shared by every entity: unique id, base topic, availability, device and origin. */
/* unique_id and "~" (the panel's base topic). */
static void ids(pj_writer_t *w, const proto_ident_t *id, const char *object)
{
    char node[PROTO_PREFIX_LEN + PROTO_PANEL_ID_LEN];
    char buf[PROTO_TOPIC_LEN];
    node_id(id, node);
    if (snprintf(buf, sizeof(buf), "%s_%s", node, object) >= (int)sizeof(buf)) {
        w->overflow = true;
    }
    pj_kstr(w, "unique_id", buf);
    if (!proto_topic(id, "", buf, sizeof(buf))) {
        w->overflow = true;
    }
    pj_kstr(w, "~", buf);
}

/* The device and origin blocks every document carries. */
static void device_blocks(pj_writer_t *w, const proto_ident_t *id)
{
    char node[PROTO_PREFIX_LEN + PROTO_PANEL_ID_LEN];
    node_id(id, node);
    pj_kobj(w, "device");
    pj_karr(w, "identifiers");
    pj_str(w, node);
    pj_arr_end(w);
    pj_kstr(w, "name", DEVICE_NAME);
    pj_kstr(w, "manufacturer", MANUFACTURER);
    pj_kstr(w, "model", id->model);
    pj_kstr(w, "sw_version", id->fw);
    pj_obj_end(w);
    pj_kobj(w, "origin");
    pj_kstr(w, "name", ORIGIN_NAME);
    pj_kstr(w, "sw_version", id->fw);
    pj_obj_end(w);
}

static void common(pj_writer_t *w, const proto_ident_t *id, const char *object)
{
    ids(w, id, object);
    pj_kstr(w, "availability_topic", "~/availability");
    device_blocks(w, id);
}

/* key: "~/<kind>/<n>/<leaf>" */
static void topic_key(pj_writer_t *w, const char *key, const char *kind, unsigned n,
                      const char *leaf)
{
    char buf[48];
    if (snprintf(buf, sizeof(buf), "~/%s/%u/%s", kind, n, leaf) >= (int)sizeof(buf)) {
        w->overflow = true;
    }
    pj_kstr(w, key, buf);
}

void proto_ha_panel(pj_writer_t *w, const proto_ident_t *id, const alarm_config_t *cfg,
                    uint8_t partition, const proto_ha_opts_t *opts)
{
    if (partition == 0u || partition > ALARM_MAX_PARTITIONS) {
        w->overflow = true;
        return;
    }
    unsigned enabled = 0;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        enabled += cfg->partitions[p].enabled ? 1u : 0u;
    }
    char object[8];
    snprintf(object, sizeof(object), "p%u", (unsigned)partition);

    pj_obj(w);
    /* One partition: the entity takes the device's name (alarm_control_panel.alarm_panel). */
    const char *name = cfg->partitions[partition - 1u].name;
    if (enabled <= 1u) {
        pj_knull(w, "name");
    } else if (name[0] != '\0') {
        pj_kstr(w, "name", name);
    } else {
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "Partition %u", (unsigned)partition);
        pj_kstr(w, "name", fallback);
    }
    common(w, id, object);
    topic_key(w, "state_topic", "partition", partition, "state");
    topic_key(w, "command_topic", "partition", partition, "set");
    topic_key(w, "json_attributes_topic", "partition", partition, "status");
    pj_kstr(w, "command_template", "{{ {\"action\": action, \"code\": code} | to_json }}");
    pj_kstr(w, "code", "REMOTE_CODE");
    pj_kbool(w, "code_arm_required", !opts->arm_without_code);
    pj_kbool(w, "code_disarm_required", true);
    pj_kbool(w, "code_trigger_required", false);
    pj_karr(w, "supported_features");
    if (opts->commands) {
        pj_str(w, "arm_home");
        pj_str(w, "arm_away");
        pj_str(w, "arm_night");
        if (opts->allow_trigger) {
            pj_str(w, "trigger");
        }
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

void proto_ha_zone(pj_writer_t *w, const proto_ident_t *id, const alarm_config_t *cfg,
                   uint16_t zone)
{
    if (zone == 0u || zone > ALARM_MAX_ZONES) {
        w->overflow = true;
        return;
    }
    const alarm_zone_config_t *zc = &cfg->zones[zone - 1u];
    char object[8];
    snprintf(object, sizeof(object), "z%u", (unsigned)zone);

    pj_obj(w);
    if (zc->name[0] != '\0') {
        pj_kstr(w, "name", zc->name);
    } else {
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "Zone %u", (unsigned)zone);
        pj_kstr(w, "name", fallback);
    }
    common(w, id, object);
    topic_key(w, "state_topic", "zone", zone, "state");
    topic_key(w, "json_attributes_topic", "zone", zone, "state");
    pj_kstr(w, "value_template", "{{ 'ON' if value_json.open else 'OFF' }}");
    const char *cls = proto_ha_device_class((alarm_device_class_t)zc->device_class);
    if (cls != NULL) {
        pj_kstr(w, "device_class", cls);
    }
    pj_obj_end(w);
}

void proto_ha_trouble(pj_writer_t *w, const proto_ident_t *id)
{
    pj_obj(w);
    pj_kstr(w, "name", "Trouble");
    common(w, id, "trouble");
    pj_kstr(w, "state_topic", "~/troubles");
    pj_kstr(w, "json_attributes_topic", "~/troubles");
    pj_kstr(w, "value_template", "{{ 'ON' if value_json.active else 'OFF' }}");
    pj_kstr(w, "device_class", "problem");
    pj_kstr(w, "entity_category", "diagnostic");
    pj_obj_end(w);
}

void proto_ha_event(pj_writer_t *w, const proto_ident_t *id)
{
    pj_obj(w);
    pj_kstr(w, "name", "Event");
    common(w, id, "event");
    pj_kstr(w, "state_topic", "~/event");
    pj_karr(w, "event_types");
    for (unsigned t = 0; t < ALARM_EV_COUNT; t++) {
        pj_str(w, alarm_event_name((alarm_event_type_t)t));
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

/* The Zigbee network's diagnostic sensor (proto_zb.h, MQTT-021). */
void proto_ha_zb(pj_writer_t *w, const proto_ident_t *id)
{
    pj_obj(w);
    pj_kstr(w, "name", "Zigbee");
    common(w, id, PROTO_ZB_OBJECT);
    pj_kstr(w, "state_topic", "~" PROTO_ZB_STATE_SUFFIX);
    pj_kstr(w, "json_attributes_topic", "~" PROTO_ZB_STATE_SUFFIX);
    pj_kstr(w, "value_template", "{{ value_json.radio }}");
    pj_kstr(w, "icon", "mdi:zigbee");
    pj_kstr(w, "entity_category", "diagnostic");
    pj_obj_end(w);
}

void proto_ha_update(pj_writer_t *w, const proto_ident_t *id, bool can_install)
{
    pj_obj(w);
    pj_kstr(w, "name", "Firmware");
    common(w, id, "firmware");
    pj_kstr(w, "state_topic", "~/firmware");
    pj_kstr(w, "device_class", "firmware");
    pj_kstr(w, "entity_category", "config");
    if (can_install) {
        pj_kstr(w, "command_topic", "~/firmware/set");
        pj_kstr(w, "payload_install", "install");
    }
    pj_obj_end(w);
}

/* ---- Z-Wave devices the panel switches (proto_zw.h, MQTT-018..020) --------------------------- */

bool proto_zw_key(uint32_t home_id, uint16_t node, uint16_t generation, uint8_t endpoint,
                  char out[PROTO_ZW_KEY_LEN])
{
    const int n = snprintf(out, PROTO_ZW_KEY_LEN, "zw_%08lx_%u_%u_%u", (unsigned long)home_id,
                           (unsigned)node, (unsigned)generation, (unsigned)endpoint);
    return n > 0 && n < PROTO_ZW_KEY_LEN;
}

const char *proto_zw_component(uint8_t kind)
{
    switch (kind) {
    case PROTO_ZW_SWITCH: return "switch";
    case PROTO_ZW_LIGHT: return "light";
    case PROTO_ZW_GARAGE: return "cover";
    default: return NULL;
    }
}

bool proto_zw_topic(const proto_ident_t *id, const char *key, const char *leaf, char *out,
                    size_t cap)
{
    char suffix[PROTO_ZW_KEY_LEN + 24];
    const int n = snprintf(suffix, sizeof(suffix), "/zwave/%s/%s", key, leaf);
    return n > 0 && (size_t)n < sizeof(suffix) && proto_topic(id, suffix, out, cap);
}

void proto_ha_zw_device(pj_writer_t *w, const proto_ident_t *id, const proto_zw_device_t *d)
{
    if (proto_zw_component(d->kind) == NULL) {
        w->overflow = true;
        return;
    }
    char topic[PROTO_ZW_KEY_LEN + 24];
    pj_obj(w);
    pj_kstr(w, "name", d->name);
    ids(w, id, d->key);
    /* the panel online and the device reachable */
    pj_karr(w, "availability");
    pj_obj(w);
    pj_kstr(w, "topic", "~/availability");
    pj_obj_end(w);
    pj_obj(w);
    snprintf(topic, sizeof(topic), "~/zwave/%s/available", d->key);
    pj_kstr(w, "topic", topic);
    pj_obj_end(w);
    pj_arr_end(w);
    pj_kstr(w, "availability_mode", "all");
    device_blocks(w, id);
    snprintf(topic, sizeof(topic), "~/zwave/%s/state", d->key);
    pj_kstr(w, "state_topic", topic);
    snprintf(topic, sizeof(topic), "~/zwave/%s/set", d->key);
    pj_kstr(w, "command_topic", topic);
    switch (d->kind) {
    case PROTO_ZW_SWITCH:
        pj_kstr(w, "value_template", "{{ value_json.state }}");
        pj_kstr(w, "payload_on", "ON");
        pj_kstr(w, "payload_off", "OFF");
        pj_kstr(w, "state_on", "ON");
        pj_kstr(w, "state_off", "OFF");
        break;
    case PROTO_ZW_LIGHT:
        pj_kstr(w, "schema", "json");
        pj_kbool(w, "brightness", true);
        pj_kint(w, "brightness_scale", 100);
        pj_karr(w, "supported_color_modes");
        pj_str(w, "brightness");
        pj_arr_end(w);
        break;
    default: /* a garage door: it closes from here, never opens (HCTL-005) */
        pj_kstr(w, "device_class", "garage");
        pj_knull(w, "payload_open");
        pj_knull(w, "payload_stop");
        pj_kstr(w, "payload_close", "CLOSE");
        pj_kstr(w, "value_template",
                "{{ value_json.state if value_json.state in "
                "['open','closed','opening','closing'] else 'None' }}");
        break;
    }
    pj_obj_end(w);
}

void proto_zw_state(pj_writer_t *w, const proto_zw_device_t *d)
{
    pj_obj(w);
    if (d->kind == PROTO_ZW_GARAGE) {
        pj_kstr(w, "state", d->garage[0] != '\0' ? d->garage : "unknown");
    } else if (d->level < 0) {
        pj_knull(w, "state"); /* not known: Home Assistant's unknown */
    } else {
        pj_kstr(w, "state", d->level > 0 ? "ON" : "OFF");
        if (d->kind == PROTO_ZW_LIGHT) {
            pj_kint(w, "brightness", d->level);
            pj_kstr(w, "color_mode", "brightness");
        }
    }
    pj_obj_end(w);
}

alarm_result_t proto_zw_cmd_parse(const char *payload, size_t len, uint8_t kind, int *level)
{
    if (payload == NULL || level == NULL || len == 0u || len > 128u) {
        return ALARM_ERR_BAD_ARG;
    }
    const bool on = len == 2u && memcmp(payload, "ON", 2u) == 0;
    const bool off = len == 3u && memcmp(payload, "OFF", 3u) == 0;
    switch (kind) {
    case PROTO_ZW_SWITCH:
        if (!on && !off) {
            return ALARM_ERR_BAD_ARG;
        }
        *level = on ? 100 : 0;
        return ALARM_OK;
    case PROTO_ZW_GARAGE:
        if (len == 5u && memcmp(payload, "CLOSE", 5u) == 0) {
            *level = 0;
            return ALARM_OK;
        }
        return ALARM_ERR_BAD_ARG; /* OPEN, STOP, anything else: not from here */
    case PROTO_ZW_LIGHT: {
        pj_reader_t r;
        pj_read_init(&r, payload, len);
        if (!pj_enter_obj(&r)) {
            return ALARM_ERR_BAD_ARG;
        }
        char key[16];
        char state[8] = "";
        int64_t brightness = -1;
        while (pj_next_member(&r, key, sizeof(key))) {
            if (strcmp(key, "state") == 0) {
                if (!pj_get_str(&r, state, sizeof(state))) {
                    return ALARM_ERR_BAD_ARG;
                }
            } else if (strcmp(key, "brightness") == 0) {
                if (!pj_get_int(&r, &brightness) || brightness < 0 || brightness > 100) {
                    return ALARM_ERR_BAD_ARG;
                }
            } else {
                pj_skip(&r); /* a transition, a color: not ours */
            }
        }
        if (r.error || !pj_finished(&r)) {
            return ALARM_ERR_BAD_ARG;
        }
        if (strcmp(state, "OFF") == 0) {
            *level = 0;
        } else if (strcmp(state, "ON") == 0) {
            *level = brightness >= 0 ? (int)brightness : 100;
        } else {
            return ALARM_ERR_BAD_ARG;
        }
        return ALARM_OK;
    }
    default: return ALARM_ERR_BAD_ARG;
    }
}
