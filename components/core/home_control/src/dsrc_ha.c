/*
 * dsrc_ha.c - Home Assistant as a device source (dsrc_ha.h, docs/features/24 M13): the catalog
 * entries the integration marks control:true become devices, actions go out as the protocol's
 * `call` words (docs/features/23 protocol.md) and results come back by request id straight to
 * home_control's sink. Portable; ha_link (or a test) feeds it.
 */
#include <string.h>

#include "home_control/dsrc_ha.h"

static void copy_str(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    if (len == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1u < len && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static dev_info_t *find_device(dsrc_ha_t *d, const char *entity_id)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < d->n; i++) {
        if (strcmp(d->devices[i].ref, entity_id) == 0) {
            return &d->devices[i];
        }
    }
    return NULL;
}

static void notify_changed(const dsrc_ha_t *d)
{
    if (d->sink.changed != NULL) {
        d->sink.changed(d->sink.ctx, DSRC_HA);
    }
}

/* ---- the domain and action tables (docs/features/24) ---------------------------------------------- */

dev_kind_t dsrc_ha_kind(const char *domain)
{
    static const struct {
        const char *domain;
        dev_kind_t kind;
    } map[] = {
        {"light", DEV_LIGHT},
        {"switch", DEV_SWITCH},
        {"input_boolean", DEV_SWITCH},
        {"lock", DEV_LOCK},
        {"climate", DEV_CLIMATE},
        {"cover", DEV_COVER}, /* covers and garage doors */
        {"fan", DEV_FAN},
        {"valve", DEV_VALVE},
        {"scene", DEV_SCENE},
        {"script", DEV_SCRIPT},
    };
    if (domain != NULL) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (strcmp(domain, map[i].domain) == 0) {
                return map[i].kind;
            }
        }
    }
    return DEV_NONE; /* alarm_control_panel, siren, sensors...: never controlled from here */
}

/* The same table as home_control's hc_action_valid, kept here so a source never reaches into
 * the registry; the host tests check the two agree for every pair. */
static bool action_fits(dev_kind_t kind, dev_action_t action)
{
    switch (kind) {
    case DEV_LIGHT:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE ||
               action == ACT_SET_LEVEL;
    case DEV_SWITCH:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_FAN:
        return action == ACT_ON || action == ACT_OFF || action == ACT_SET_LEVEL;
    case DEV_CLIMATE:
        return action == ACT_SET_TEMP || action == ACT_SET_MODE;
    case DEV_COVER:
        return action == ACT_OPEN || action == ACT_CLOSE || action == ACT_STOP ||
               action == ACT_SET_LEVEL;
    case DEV_LOCK:
        return action == ACT_LOCK || action == ACT_UNLOCK;
    case DEV_VALVE:
        return action == ACT_OPEN || action == ACT_CLOSE;
    case DEV_SCENE:
    case DEV_SCRIPT:
        return action == ACT_RUN;
    case DEV_NONE:
    case DEV_KIND_COUNT:
    default:
        return false;
    }
}

const char *dsrc_ha_action_word(dev_kind_t kind, dev_action_t action)
{
    if (!action_fits(kind, action)) {
        return NULL;
    }
    switch (action) {
    case ACT_ON:
        return "turn_on";
    case ACT_OFF:
        return "turn_off";
    case ACT_TOGGLE:
        return "toggle";
    case ACT_LOCK:
        return "lock";
    case ACT_UNLOCK:
        return "unlock";
    case ACT_OPEN:
        return "open";
    case ACT_CLOSE:
        return "close";
    case ACT_STOP:
        return "stop";
    case ACT_SET_LEVEL:
        /* one action on the panel, three words on the wire */
        return (kind == DEV_LIGHT)  ? "turn_on"
               : (kind == DEV_FAN)  ? "set_percentage"
                                    : "set_position";
    case ACT_SET_TEMP:
        return "set_temperature";
    case ACT_SET_MODE:
        return "set_hvac_mode"; /* presets come later */
    case ACT_RUN:
        return "turn_on"; /* scenes and scripts */
    case ACT_NONE:
    case ACT_COUNT:
    default:
        return NULL;
    }
}

/* ---- a device's values (HCTL-002) ------------------------------------------------------------ */

/* What a device of this kind shows of what Home Assistant sent, each within its range: a level for
 * a light, a fan, a cover or a valve; a setpoint, a temperature, modes and what it is doing for a
 * thermostat; since when (every kind); what it can do (the kind's own DEV_CAP_*). A light or a
 * cover keeps DEV_VAL_LEVEL_SET when its source doesn't say what it can (an older integration):
 * once it reported a level (had: its flags so far, or the remembered list after a new catalog),
 * so a light switched off - Home Assistant then has no brightness - still offers its slider. When
 * it says (DEV_CAP_KNOWN), DIM or POSITION decide the slider. An unavailable device whose message
 * says nothing of what it can keeps had_caps, so an offline tile keeps its shape. */
static uint8_t caps_for(dev_kind_t kind)
{
    switch (kind) {
    case DEV_LIGHT:
        return DEV_CAP_DIM;
    case DEV_FAN:
        return DEV_CAP_SPEED;
    case DEV_COVER:
    case DEV_VALVE:
        return (uint8_t)(DEV_CAP_STOP | DEV_CAP_POSITION);
    case DEV_NONE:
    case DEV_SWITCH:
    case DEV_LOCK:
    case DEV_CLIMATE:
    case DEV_SCENE:
    case DEV_SCRIPT:
    case DEV_KIND_COUNT:
    default:
        return 0u;
    }
}

static dev_values_t keep_values(dev_kind_t kind, const dev_values_t *in, uint8_t had,
                                uint8_t had_caps, bool available)
{
    dev_values_t v;
    memset(&v, 0, sizeof(v));
    const uint8_t own = caps_for(kind);
    if (own != 0u) {
        if (in != NULL && (in->caps & DEV_CAP_KNOWN) != 0u) {
            v.caps = (uint8_t)(DEV_CAP_KNOWN | (in->caps & own));
        } else if (!available) {
            v.caps = had_caps;
        }
    }
    if (in != NULL) {
        v.since = in->since;
    }
    const bool settable = kind == DEV_LIGHT || kind == DEV_COVER;
    const bool levelled = settable || kind == DEV_FAN || kind == DEV_VALVE;
    const bool caps_known = (v.caps & DEV_CAP_KNOWN) != 0u;
    if (settable) {
        if (caps_known) {
            /* the source says: its word decides, not what it reported before (HCTL-002) */
            const uint8_t cap = kind == DEV_LIGHT ? DEV_CAP_DIM : DEV_CAP_POSITION;
            if ((v.caps & cap) != 0u) {
                v.flags = DEV_VAL_LEVEL_SET;
            }
        } else {
            v.flags = (uint8_t)(had & DEV_VAL_LEVEL_SET);
            if (available && in != NULL && (in->flags & DEV_VAL_LEVEL_SET) != 0u) {
                v.flags = (uint8_t)(v.flags | DEV_VAL_LEVEL_SET);
            }
        }
    }
    if (!available || in == NULL) {
        return v; /* level, setpoint, temperature, modes and what it is doing: not known */
    }
    if (levelled && (in->flags & DEV_VAL_LEVEL) != 0u && in->level <= 100u) {
        v.flags = (uint8_t)(v.flags | DEV_VAL_LEVEL);
        v.level = in->level;
        if (settable && !caps_known) {
            v.flags = (uint8_t)(v.flags | DEV_VAL_LEVEL_SET);
        }
    }
    if (kind == DEV_CLIMATE) {
        if ((in->flags & DEV_VAL_SETPOINT) != 0u && in->setpoint >= -DEV_TEMP_MAX &&
            in->setpoint <= DEV_TEMP_MAX) {
            v.flags = (uint8_t)(v.flags | DEV_VAL_SETPOINT);
            v.setpoint = in->setpoint;
        }
        if ((in->flags & DEV_VAL_TEMP) != 0u && in->temp >= -DEV_TEMP_MAX &&
            in->temp <= DEV_TEMP_MAX) {
            v.flags = (uint8_t)(v.flags | DEV_VAL_TEMP);
            v.temp = in->temp;
        }
        v.modes = (uint8_t)(in->modes & (DEV_MODE_OFF | DEV_MODE_HEAT | DEV_MODE_COOL |
                                         DEV_MODE_AUTO | DEV_MODE_HEAT_COOL));
        if (in->hvac_action >= (uint8_t)DEV_HVAC_OFF && in->hvac_action < (uint8_t)DEV_HVAC_COUNT) {
            v.hvac_action = in->hvac_action;
        }
    }
    return v;
}

/* ---- which lights and covers can be set, across catalogs (HCTL-002) -------------------------- */

static uint32_t ref_hash(const char *ref)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)ref; *p != 0u; p++) {
        h = (h ^ (uint32_t)*p) * 16777619u;
    }
    return h;
}

static bool was_dimmable(const dsrc_ha_t *d, const char *ref)
{
    const uint32_t h = ref_hash(ref);
    for (size_t i = 0; i < d->n_dimmable; i++) {
        if (d->dimmable[i] == h) {
            return true;
        }
    }
    return false;
}

static void remember_dimmable(dsrc_ha_t *d, const dev_info_t *dev)
{
    if ((dev->values.flags & DEV_VAL_LEVEL_SET) == 0u || was_dimmable(d, dev->ref)) {
        return;
    }
    const uint32_t h = ref_hash(dev->ref);
    if (d->n_dimmable < DSRC_HA_MAX) {
        d->dimmable[d->n_dimmable++] = h;
        return;
    }
    d->dimmable[d->next_dimmable] = h; /* full: the oldest goes */
    d->next_dimmable = (uint8_t)((d->next_dimmable + 1u) % DSRC_HA_MAX);
}

/* ---- fed by ha_link -------------------------------------------------------------------------- */

void dsrc_ha_init(dsrc_ha_t *d, const dsrc_sink_t *sink)
{
    memset(d, 0, sizeof(*d));
    if (sink != NULL) {
        d->sink = *sink;
    }
}

void dsrc_ha_set_sender(dsrc_ha_t *d, dsrc_ha_send_fn fn, void *ctx)
{
    d->send = fn;
    d->send_ctx = ctx;
}

void dsrc_ha_link_at(dsrc_ha_t *d, bool up, uint32_t now_s)
{
    if (d->link_up == up) {
        return;
    }
    d->down_was_on = 0u;
    d->down_at = up ? 0u : now_s;
    for (size_t i = 0; i < d->n; i++) {
        if (!up && d->devices[i].online) {
            d->down_was_on |= (uint64_t)1u << i; /* listed "offline since now_s" (ha_list) */
        }
        d->devices[i].online = up; /* HCTL-006: offline, its controls rest, while it is down */
    }
    d->link_up = up;
    notify_changed(d);
}

void dsrc_ha_link(dsrc_ha_t *d, bool up)
{
    dsrc_ha_link_at(d, up, 0u);
}

void dsrc_ha_catalog_begin(dsrc_ha_t *d)
{
    d->n = 0;
    d->down_was_on = 0u;
    notify_changed(d);
}

void dsrc_ha_catalog_add(dsrc_ha_t *d, const char *entity_id, const char *name, const char *area,
                         const char *domain, const char *state, const dev_values_t *values)
{
    const dev_kind_t kind = dsrc_ha_kind(domain);
    if (kind == DEV_NONE || entity_id == NULL || entity_id[0] == '\0') {
        return; /* not something the Devices screen can show */
    }
    dev_info_t *dev = find_device(d, entity_id);
    uint8_t had = 0u;
    if (dev == NULL) {
        if (d->n >= DSRC_HA_MAX) {
            return; /* the options list should stay sane (HAI-009) */
        }
        dev = &d->devices[d->n++];
    } else if (dev->kind == (uint8_t)kind) {
        had = dev->values.flags;
    }
    if (was_dimmable(d, entity_id)) {
        had = (uint8_t)(had | DEV_VAL_LEVEL_SET); /* a catalog after a reconnect: off, no level */
    }
    memset(dev, 0, sizeof(*dev));
    dev->source = DSRC_HA;
    dev->kind = (uint8_t)kind;
    copy_str(dev->ref, sizeof(dev->ref), entity_id);
    copy_str(dev->name, sizeof(dev->name), (name != NULL && name[0] != '\0') ? name : entity_id);
    copy_str(dev->area, sizeof(dev->area), area);
    copy_str(dev->state, sizeof(dev->state), state);
    dev->values = keep_values(kind, values, had, 0u, true); /* a catalog row: available */
    remember_dimmable(d, dev);
    dev->online = d->link_up;
    notify_changed(d);
}

void dsrc_ha_state(dsrc_ha_t *d, const char *entity_id, const char *state, bool available,
                   const dev_values_t *values)
{
    dev_info_t *dev = find_device(d, entity_id);
    if (dev == NULL) {
        return; /* a state for something outside the control catalog: not ours */
    }
    copy_str(dev->state, sizeof(dev->state), available ? state : "");
    const dev_values_t prev = dev->values;
    dev->values = keep_values((dev_kind_t)dev->kind, values, prev.flags, prev.caps, available);
    remember_dimmable(d, dev);
    dev->online = available && d->link_up;
    notify_changed(d);
}

void dsrc_ha_result(dsrc_ha_t *d, uint32_t request_id, bool ok, const char *error)
{
    if (request_id == 0 || d->sink.result == NULL) {
        return;
    }
    if (error == NULL) {
        error = "";
    }
    /* HCTL-012: the integration answers "timeout" when the service call it started didn't
     * finish within its own time - the device may have acted and not said so */
    const bool unknown = !ok && strcmp(error, "timeout") == 0;
    d->sink.result(d->sink.ctx, request_id, ok, ok ? "" : error, unknown);
}

/* ---- the source interface ---------------------------------------------------------------------
 * A call's result comes back by request id through dsrc_ha_result. */

static bool ha_online(void *ctx)
{
    return ((const dsrc_ha_t *)ctx)->link_up;
}

static size_t ha_list(void *ctx, dev_info_t *out, size_t max)
{
    const dsrc_ha_t *d = ctx;
    size_t n = 0;
    for (size_t i = 0; i < d->n && n < max; i++) {
        out[n] = d->devices[i];
        if (!d->link_up && ((d->down_was_on >> i) & 1u) != 0u) {
            out[n].values.since = d->down_at; /* offline since the link dropped (HCTL-006) */
        }
        n++;
    }
    return n;
}

static bool ha_act(void *ctx, const char *ref, dev_action_t action, int32_t value,
                   const char *mode, uint32_t request_id)
{
    dsrc_ha_t *d = ctx;
    const dev_info_t *dev = find_device(d, ref);
    if (dev == NULL) {
        return false;
    }
    const char *word = dsrc_ha_action_word((dev_kind_t)dev->kind, action);
    if (word == NULL || d->send == NULL) {
        return false;
    }
    /* `value` only travels when the action carries one: a brightness with a light's turn_on,
     * the level of a set_position / set_percentage, the setpoint of a set_temperature. */
    const bool has_value = action == ACT_SET_LEVEL || action == ACT_SET_TEMP ||
                           (action == ACT_ON && dev->kind == DEV_LIGHT && value >= 0);
    /* The screen's Auto is Home Assistant's "auto" or "heat_cool": the word this thermostat has. */
    const uint8_t modes = dev->values.modes;
    if (action == ACT_SET_MODE && mode != NULL && strcmp(mode, "auto") == 0 &&
        (modes & DEV_MODE_HEAT_COOL) != 0u && (modes & DEV_MODE_AUTO) == 0u) {
        mode = "heat_cool";
    }
    return d->send(d->send_ctx, dev->ref, word, has_value, value, mode, request_id);
}

const device_source_ops_t *dsrc_ha_ops(void)
{
    static const device_source_ops_t ops = {
        .kind = DSRC_HA,
        .name = "home_assistant",
        .online = ha_online,
        .list = ha_list,
        .act = ha_act,
    };
    return &ops;
}
