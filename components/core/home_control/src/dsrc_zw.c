/*
 * dsrc_zw.c - the panel's own Z-Wave outputs as a device source (dsrc_zw.h, docs/features/41
 * DESIGN section 4 "Z-Wave (phase B)"): the platform feeds the outputs it switches, actions are
 * posted to its sender as a level, and results come back by request id straight to home_control's
 * sink. A garage door gets "close" (0) or "open" (100), never a blind press: the output layer
 * presses only when the door's sensor says it is the other way (zw_outputs.h). Portable;
 * zwave_service (or a test) feeds it.
 */
#include <string.h>

#include "home_control/dsrc_zw.h"

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

static bool key_ok(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        return false;
    }
    return strlen(key) < DSRC_ZW_KEY_LEN; /* refused, never cut: two cut keys could match */
}

static dev_info_t *find_in(dev_info_t *list, size_t n, const char *key)
{
    if (!key_ok(key)) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].ref, key) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

static void notify_changed(const dsrc_zw_t *d)
{
    if (d->sink.changed != NULL) {
        d->sink.changed(d->sink.ctx, DSRC_ZWAVE);
    }
}

static dev_kind_t kind_of(uint8_t zw_kind)
{
    switch (zw_kind) {
    case DSRC_ZW_SWITCH:
        return DEV_SWITCH;
    case DSRC_ZW_LIGHT:
        return DEV_LIGHT;
    case DSRC_ZW_GARAGE:
        return DEV_COVER;
    default:
        return DEV_NONE;
    }
}

/* The state words, as dsrc_ha reports Home Assistant's: "on" / "off" for a switch and a light (a
 * dimmer at any level above 0 is "on"), the sensor's own word for a garage door, "unknown" when
 * the output hasn't said. */
static void state_word(dev_info_t *dev, int level, const char *garage_word)
{
    const char *w;
    if (dev->kind == DEV_COVER) {
        w = (garage_word != NULL && garage_word[0] != '\0') ? garage_word : "unknown";
    } else if (level < 0 || level > 100) {
        w = "unknown";
    } else {
        w = level > 0 ? "on" : "off";
    }
    copy_str(dev->state, sizeof(dev->state), w);
}

/* ---- what an action sends -------------------------------------------------------------------- */

int dsrc_zw_level_for(dev_kind_t kind, dev_action_t action, int32_t value)
{
    switch (kind) {
    case DEV_SWITCH:
        if (action == ACT_ON) {
            return 100;
        }
        return action == ACT_OFF ? 0 : -1;
    case DEV_LIGHT:
        if (action == ACT_ON) {
            /* -1: "on" with no level. Z-Wave outputs have no "previous level" form (zwo_set
             * takes 0..100), so it is full on. */
            if (value == -1) {
                return 100;
            }
            return (value >= 0 && value <= 100) ? (int)value : -1;
        }
        if (action == ACT_SET_LEVEL) {
            return (value >= 0 && value <= 100) ? (int)value : -1;
        }
        return action == ACT_OFF ? 0 : -1;
    case DEV_COVER:
        /* A garage door is a relay press read back by a tilt sensor (zw_outputs.h): CLOSE is a
         * press only while the sensor says open, OPEN only while it says closed, never blind
         * (ZW-035). Who may ask is decided above (hc_needs_code for a person, hc_auto_allowed
         * and the installer option for an automation, AUT-015). OPEN is as safe as CLOSE only
         * through zwz_panel_garage_move's guards - ZWO_ALREADY (the sensor says it is that
         * way already: no press), ZWO_ERR_BUSY (a move under way) and ZWO_ERR_POSITION (the
         * sensor's state isn't known: never a blind press) - so phase B's sender must post through it, never press
         * the relay itself. A press can't stop a door or set a position (it reverses or
         * restarts the opener): STOP and SET_LEVEL are refused. */
        if (action == ACT_CLOSE) {
            return 0;
        }
        return action == ACT_OPEN ? 100 : -1;
    default:
        return -1; /* TOGGLE and everything else: never (a toggle of a garage, or of a state not
                    * known, could be an open nobody chose) */
    }
}

/* ---- fed by the platform --------------------------------------------------------------------- */

void dsrc_zw_init(dsrc_zw_t *d, const dsrc_sink_t *sink)
{
    memset(d, 0, sizeof(*d));
    if (sink != NULL) {
        d->sink = *sink;
    }
}

void dsrc_zw_set_sender(dsrc_zw_t *d, dsrc_zw_send_fn fn, void *ctx)
{
    d->send = fn;
    d->send_ctx = ctx;
}

void dsrc_zw_link(dsrc_zw_t *d, bool up)
{
    if (d->link_up == up) {
        return;
    }
    d->link_up = up;
    for (size_t i = 0; i < d->n; i++) {
        /* HCTL-006: offline while it is down. Coming back up, a device is online again only when
         * its own next state says so (an add or a state from the platform). */
        if (!up) {
            d->devices[i].online = false;
        }
    }
    for (size_t i = 0; i < d->n_staged; i++) {
        if (!up) {
            d->staged[i].online = false;
        }
    }
    notify_changed(d);
}

void dsrc_zw_begin(dsrc_zw_t *d)
{
    d->building = true;
    d->n_staged = 0;
}

void dsrc_zw_add(dsrc_zw_t *d, const char *key, uint8_t kind, const char *name, int level,
                 bool online, const char *garage_word, uint32_t act_ms)
{
    const dev_kind_t k = kind_of(kind);
    if (!d->building || k == DEV_NONE || !key_ok(key)) {
        return; /* not something the registry can show */
    }
    dev_info_t *dev = find_in(d->staged, d->n_staged, key);
    if (dev == NULL) {
        if (d->n_staged >= DSRC_ZW_MAX) {
            return; /* the outputs' own bound: never more */
        }
        dev = &d->staged[d->n_staged++];
    }
    memset(dev, 0, sizeof(*dev));
    dev->source = DSRC_ZWAVE;
    dev->kind = (uint8_t)k;
    copy_str(dev->ref, sizeof(dev->ref), key);
    copy_str(dev->name, sizeof(dev->name), (name != NULL && name[0] != '\0') ? name : key);
    dev->area[0] = '\0'; /* Z-Wave outputs have no area yet */
    state_word(dev, level, garage_word);
    dev->online = online && d->link_up;
    dev->act_ms = act_ms; /* HCTL-012: a garage's whole move outlasts home_control's 30 s */
}

void dsrc_zw_end(dsrc_zw_t *d)
{
    if (!d->building) {
        return;
    }
    d->building = false;
    memcpy(d->devices, d->staged, d->n_staged * sizeof(d->devices[0]));
    d->n = d->n_staged;
    d->n_staged = 0;
    notify_changed(d);
}

static bool apply_state(dev_info_t *dev, bool link_up, int level, bool online,
                        const char *garage_word)
{
    const dev_info_t before = *dev;
    state_word(dev, level, garage_word);
    dev->online = online && link_up;
    return before.online != dev->online || strcmp(before.state, dev->state) != 0;
}

void dsrc_zw_state(dsrc_zw_t *d, const char *key, int level, bool online,
                   const char *garage_word)
{
    dev_info_t *staged = find_in(d->staged, d->n_staged, key);
    if (staged != NULL) {
        (void)apply_state(staged, d->link_up, level, online, garage_word);
    }
    dev_info_t *dev = find_in(d->devices, d->n, key);
    if (dev == NULL) {
        return; /* not an output this source lists */
    }
    if (apply_state(dev, d->link_up, level, online, garage_word)) {
        notify_changed(d);
    }
}

void dsrc_zw_result(dsrc_zw_t *d, uint32_t request_id, bool ok, const char *error)
{
    if (request_id == 0 || d->sink.result == NULL) {
        return;
    }
    /* HCTL-012: "timeout" - sent, and not confirmed (no acknowledgement, no answer, a garage
     * relay that won't let go, dropped mid-way) - may have happened. Every other word is definite:
     * a refusal made when the command was queued, before anything was sent ("offline", "busy",
     * "send_failed", "failed"), or - "failed" at a garage move's end - a press whose door its
     * sensor says didn't get there. */
    const bool unknown = !ok && error != NULL && strcmp(error, "timeout") == 0;
    d->sink.result(d->sink.ctx, request_id, ok, ok ? "" : error, unknown);
}

/* ---- the source interface -------------------------------------------------------------------- */

static bool zw_online(void *ctx)
{
    return ((const dsrc_zw_t *)ctx)->link_up;
}

static size_t zw_list(void *ctx, dev_info_t *out, size_t max)
{
    const dsrc_zw_t *d = ctx;
    size_t n = 0;
    for (size_t i = 0; i < d->n && n < max; i++) {
        out[n++] = d->devices[i];
    }
    return n;
}

static bool zw_act(void *ctx, const char *ref, dev_action_t action, int32_t value,
                   const char *mode, uint32_t request_id)
{
    (void)mode; /* no Z-Wave action takes a mode */
    dsrc_zw_t *d = ctx;
    if (!d->link_up || d->send == NULL) {
        return false; /* failed at once: a Z-Wave command is never held */
    }
    const dev_info_t *dev = find_in(d->devices, d->n, ref);
    if (dev == NULL || !dev->online) {
        return false;
    }
    const int level = dsrc_zw_level_for((dev_kind_t)dev->kind, action, value);
    if (level < 0) {
        return false; /* not something a Z-Wave output can do: nothing posted */
    }
    return d->send(d->send_ctx, dev->ref, level, request_id);
}

const device_source_ops_t *dsrc_zw_ops(void)
{
    static const device_source_ops_t ops = {
        .kind = DSRC_ZWAVE,
        .name = "zwave",
        .online = zw_online,
        .list = zw_list,
        .act = zw_act,
    };
    return &ops;
}
