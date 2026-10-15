/*
 * zbz_panel.c - the panel's Zigbee devices: the registry and the install codes, each device's
 * setup, the operations the pages ask for, their views, what is stored and what is logged
 * (docs/features/20 devices.md; ZB-010..ZB-018, ZB-021, ZB-027). See zbz_panel.h.
 *
 * Shaped like zwave_zones' zwz_panel: requests answer at once and start an operation the pages
 * follow through zbz_panel_progress (one at a time); the radio's answers come back as reports
 * carrying the request's tag; zbz_panel_poll runs what is due at zbz_panel_next_deadline(); what
 * changed is handed out by the take calls and logged through take_log.
 *
 * Every report goes to the zone source first (zsrc_zb_on_report), then here. The setup tells the
 * source when it runs (zsrc_zb_device_setup), describes a sensor's inputs once its descriptors
 * and Basic are known (zsrc_zb_device_seen), marks an endpoint enrolled (zsrc_zb_ias_enrolled)
 * and passes a check-in interval it learned (zsrc_zb_check_in_interval); forgetting a device goes
 * through zsrc_zb_device_forget. Decisions taken here are recorded in devices.md §5, §6 and §9.
 *
 * The sirens (zbz_wd.h) and the keypads and fobs (zbz_ace.h) are wired here: their frames go out
 * through the radio's zcl_command under their own tags, the engine's news comes in through
 * zbz_panel_output, _partition and _ace_result, a keypad's command goes to the alarm task through
 * the port's ace_post. Decisions: alarm-devices.md §2 and §3, "Wired in zbz_panel".
 */
#include "zigbee_zones/zbz_panel.h"

#include <stdio.h>
#include <string.h>

#define HA_PROFILE   0x0104u
#define SETUP_STEPS  ((uint8_t)(ZSRC_ZB_STEP_DONE - 1)) /* NODE_DESC..POLL_CTRL */
#define LOOP_GUARD   96u
#define BASIC_READS  3u /* the BASIC step's reads (send_item) */

/* ---- small helpers ----------------------------------------------------------------------------- */

static uint64_t add_ms(uint64_t t, uint64_t ms)
{
    return t > UINT64_MAX - ms ? UINT64_MAX : t + ms;
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

/* `src` into dst (cap bytes with its NUL), cut at a UTF-8 character boundary. */
static void copy_text(char *dst, size_t cap, const char *src)
{
    if (cap == 0u) {
        return;
    }
    size_t n = 0u;
    if (src != NULL) {
        while (n + 1u < cap && src[n] != '\0') {
            n++;
        }
        if (src[n] != '\0') {
            while (n > 0u && (((unsigned)(uint8_t)src[n]) & 0xC0u) == 0x80u) {
                n--; /* never half a character */
            }
        }
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static void wipe(void *buf, size_t len)
{
    volatile uint8_t *v = buf;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0u;
    }
}

static void notify(const zbz_panel_t *p)
{
    if (p->port.changed != NULL) {
        p->port.changed(p->port.ctx);
    }
}

static uint32_t wall(const zbz_panel_t *p)
{
    return p->port.wall_clock != NULL ? p->port.wall_clock(p->port.ctx) : 0u;
}

/* The panel's own tags: 'Z' and a sequence number - never ZSRC_ZB_TAG_SOURCE, never 0. */
static uint32_t new_tag(zbz_panel_t *p)
{
    p->seq++;
    if (p->seq == 0u) {
        p->seq = 1u;
    }
    return ZBZ_TAG | (uint32_t)p->seq;
}

static bool radio_ok(const zbz_panel_t *p)
{
    return p->info.state == (uint8_t)UI_ZB_RADIO_WORKING;
}

static bool storage_full(const zbz_panel_t *p)
{
    return p->nvs_free != ZBZ_NVS_NOT_TOLD && p->nvs_free < ZBZ_NVS_ADD_MIN;
}

static size_t slot_of(const zbz_panel_t *p, const zbz_device_t *d)
{
    return (size_t)(d - p->reg.dev);
}

static bool setup_active(const zbz_device_t *d)
{
    return d->setup == (uint8_t)ZBZ_SETUP_RUNNING || d->setup == (uint8_t)ZBZ_SETUP_WAKE;
}

/* ---- the sirens' and keypads' ports: the panel's own ------------------------------------------- */

/* A siren's or keypad's frame: the radio's zcl_command, under the policy's own tag ('W', 'A'). */
static bool policy_send(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                        uint32_t tag)
{
    const zbz_panel_t *p = ctx;
    return p != NULL && radio_ok(p) && p->radio.zcl_command != NULL &&
           (tag & ZSRC_ZB_TAG_SOURCE) == 0u &&
           p->radio.zcl_command(p->radio.ctx, ieee, endpoint, cmd, tag);
}

/* Any siren in trouble (or none any more): the owner posts ALARM_TRBL_BELL to the alarm task. */
static void wd_bell_trouble(void *ctx, bool active)
{
    const zbz_panel_t *p = ctx;
    if (p != NULL && p->wd_owner.bell_trouble != NULL) {
        p->wd_owner.bell_trouble(p->wd_owner.ctx, active);
    }
}

/* A Default Response goes through the owner's stack, with the request's own sequence number. */
static bool ace_default_rsp(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t tsn,
                            uint8_t command, uint8_t status, uint32_t tag)
{
    const zbz_panel_t *p = ctx;
    return p != NULL && p->ace_owner.default_rsp != NULL &&
           p->ace_owner.default_rsp(p->ace_owner.ctx, ieee, endpoint, tsn, command, status, tag);
}

static zbw_port_t wd_port(zbz_panel_t *p)
{
    return (zbw_port_t){.ctx = p, .send = policy_send, .bell_trouble = wd_bell_trouble,
                        .checkin = NULL};
}

/* Without the owner's default_rsp the keypads' refusals are the owner's (zba_port_t: NULL). */
static zba_port_t ace_port(zbz_panel_t *p)
{
    return (zba_port_t){.ctx = p, .send = policy_send,
                        .default_rsp = p->ace_owner.default_rsp != NULL ? ace_default_rsp : NULL};
}

/* A device that goes (removed, forgotten, replaced, the network reset) loses its siren, keypad or
 * fob assignment - a keypad its number - and its test's result (alarm-devices.md §4). */
static void forget_roles(zbz_panel_t *p, uint16_t id)
{
    (void)zbw_clear(&p->wd, id); /* one giving a warning is told Stop */
    (void)zba_clear(&p->ace, id);
    for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
        if (p->siren_test[k].id == id) {
            p->siren_test[k].id = 0u;
            p->siren_test[k].result = 0u;
        }
    }
}

/* ---- the log ----------------------------------------------------------------------------------- */

static void log_ev(zbz_panel_t *p, zbz_event_t type, uint16_t zone, uint16_t device, uint8_t arg,
                   uint8_t user, uint8_t via)
{
    if (p->log_n >= ZBZ_LOG_MAX) {
        p->log_lost++;
        return;
    }
    zbz_log_t *l = &p->log[p->log_n++];
    l->type = (uint8_t)type;
    l->zone = zone;
    l->device = device;
    l->arg = arg;
    l->user = user;
    l->via = user != 0u ? via : (uint8_t)UI_ZB_VIA_PANEL;
}

/* ---- zones and classes ------------------------------------------------------------------------- */

static bool input_is(const zsrc_input_t *in, uint64_t ieee)
{
    return in->addr.kind == (uint8_t)ZSRC_ZIGBEE && in->addr.id == ieee && ieee != 0u;
}

static bool binding_has(const zone_hub_binding_t *b, uint64_t ieee)
{
    for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
        if (input_is(&b->in[r], ieee)) {
            return true;
        }
    }
    return false;
}

/* The zones a device feeds, in zone order: how many (out keeps the first `max`). */
static size_t device_zones(const zbz_panel_t *p, uint64_t ieee, uint16_t *out, size_t max)
{
    size_t n = 0u;
    if (p->port.binding == NULL || ieee == 0u) {
        return 0u;
    }
    for (uint16_t z = 1u; z <= (uint16_t)ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (p->port.binding(p->port.ctx, z, &b) && binding_has(&b, ieee)) {
            if (n < max) {
                out[n] = z;
            }
            n++;
        }
    }
    return n;
}

static uint16_t first_zone(const zbz_panel_t *p, uint64_t ieee)
{
    uint16_t z = 0u;
    (void)device_zones(p, ieee, &z, 1u);
    return z;
}

static const zbz_endpoint_t *endpoint_of(const zbz_device_t *d, uint8_t ep)
{
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        if (d->ep[k].endpoint == ep) {
            return &d->ep[k];
        }
    }
    return NULL;
}

/* A device id an actuator has (a light, a plug, a lock, a cover): its On/Off is not an input. */
static bool actuator_id(uint16_t device_id)
{
    switch (device_id) {
    case 0x0002u: /* on/off output */
    case 0x0009u: /* mains power outlet */
    case 0x000Au: /* door lock */
    case 0x0051u: /* smart plug */
    case 0x0100u: /* on/off light */
    case 0x0101u: /* dimmable light */
    case 0x0102u: /* colour dimmable light */
    case 0x010Au: /* on/off plug-in unit */
    case 0x010Bu: /* dimmable plug-in unit */
    case 0x010Cu: /* colour temperature light */
    case 0x010Du: /* extended colour light */
    case 0x0200u: /* shade */
    case 0x0202u: /* window covering */
        return true;
    default:
        return false;
    }
}

/* An endpoint whose clusters a zone can read (the source's usable clusters, an actuator's On/Off
 * left out), on the Home Automation profile: a vendor profile's or Green Power's never is. */
static bool ep_is_input(const zbz_endpoint_t *e)
{
    if (e->profile != HA_PROFILE) {
        return false;
    }
    const uint16_t in = e->in_clusters;
    if ((in & (ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_OCCUPANCY | ZSRC_ZB_CL_TEMP)) != 0u) {
        return true;
    }
    return (in & ZSRC_ZB_CL_ON_OFF) != 0u && !actuator_id(e->device_id);
}

static uint8_t class_of_ep(const zbz_panel_t *p, const zbz_device_t *d, uint8_t ep)
{
    const zsrc_zb_device_t *s = p->src != NULL ? zsrc_zb_device(p->src, d->ieee, ep) : NULL;
    if (s != NULL) {
        return s->device_class;
    }
    const zbz_endpoint_t *e = endpoint_of(d, ep);
    if (e == NULL) {
        return (uint8_t)ALARM_DEVCLASS_GENERIC;
    }
    if ((e->in_clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u && e->zone_type != ZSRC_ZB_ZONE_TYPE_NONE) {
        return (uint8_t)zsrc_zb_class_of_zone_type(e->zone_type);
    }
    return (uint8_t)zsrc_zb_class_of_clusters(e->in_clusters);
}

/* ---- roles -------------------------------------------------------------------------------------- */

uint8_t zbz_role_of(const zbz_device_t *d)
{
    if (d == NULL) {
        return (uint8_t)ZBZ_ROLE_UNKNOWN;
    }
    const zbz_model_t *m = zbz_model_for(d->manufacturer, d->model);
    if (m != NULL && m->role != 0u) {
        return m->role;
    }
    bool ace = false, fob = false, wd = false, sensor = false, actuator = false;
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        const zbz_endpoint_t *e = &d->ep[k];
        if ((e->out_clusters & ZSRC_ZB_CL_IAS_ACE) != 0u) {
            ace = true;
            fob = fob || e->zone_type == ZSRC_ZB_ZONE_TYPE_KEYFOB;
        }
        if ((e->in_clusters & ZSRC_ZB_CL_IAS_WD) != 0u &&
            (e->device_id == 0x0403u || e->zone_type == ZSRC_ZB_ZONE_TYPE_WARNING ||
             (e->in_clusters & ZSRC_ZB_CL_IAS_ZONE) == 0u)) {
            wd = true;
        }
        sensor = sensor || ep_is_input(e);
        actuator = actuator || ((e->in_clusters & ZSRC_ZB_CL_ON_OFF) != 0u &&
                                actuator_id(e->device_id));
    }
    if (m != NULL && (m->flags & ZBZ_MODEL_ACE_FOB) != 0u) {
        fob = true;
    }
    if (ace) {
        return (uint8_t)(fob ? ZBZ_ROLE_KEYFOB : ZBZ_ROLE_KEYPAD);
    }
    if (wd) {
        return (uint8_t)ZBZ_ROLE_SIREN;
    }
    if (sensor) {
        return (uint8_t)ZBZ_ROLE_SENSOR;
    }
    if (d->ep_count == 0u && d->node_type == (uint8_t)ZBZ_NODE_UNKNOWN && d->mac_caps == 0u &&
        d->power_source == 0u) {
        return (uint8_t)ZBZ_ROLE_UNKNOWN; /* nothing known yet */
    }
    if (zbz_dev_router(d) && !actuator) {
        return (uint8_t)ZBZ_ROLE_REPEATER;
    }
    return (uint8_t)ZBZ_ROLE_OTHER;
}

static bool role_used(uint8_t role)
{
    return role == (uint8_t)ZBZ_ROLE_SENSOR || role == (uint8_t)ZBZ_ROLE_SIREN ||
           role == (uint8_t)ZBZ_ROLE_KEYPAD || role == (uint8_t)ZBZ_ROLE_KEYFOB;
}

/* What the pages call its model: the model table's name, else what it reports. */
static void model_name(const zbz_device_t *d, char *out, size_t cap)
{
    const zbz_model_t *m = zbz_model_for(d->manufacturer, d->model);
    copy_text(out, cap, m != NULL ? m->name : d->model);
}

/* ---- describing a device to the zone source ----------------------------------------------------- */

/* A sensor's inputs, as its record holds them (zsrc_zb_device_seen), and its check-in. False when
 * the source had no room for one of them (ZSRC_ZB_DEV_MAX endpoints already): live.src_full. */
static bool describe(zbz_panel_t *p, size_t i, uint64_t now)
{
    const zbz_device_t *d = &p->reg.dev[i];
    const uint8_t role = d->role != 0u ? d->role : zbz_role_of(d);
    if (p->src == NULL || role != (uint8_t)ZBZ_ROLE_SENSOR) {
        return true;
    }
    bool room = true;
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        zsrc_zb_desc_t desc;
        if (ep_is_input(&d->ep[k]) && zbz_dev_desc(d, d->ep[k].endpoint, &desc) &&
            !zsrc_zb_device_seen(p->src, &desc, now) && p->src->link_up) {
            room = false;
        }
    }
    p->live[i].src_full = !room;
    if (d->check_in_s != 0u) {
        zsrc_zb_check_in_interval(p->src, d->ieee, d->check_in_s, now);
    }
    return room;
}

/* Registry slots of the devices that feed a zone, in any role (one pass over the bindings). */
static uint64_t feeding_slots(const zbz_panel_t *p)
{
    uint64_t feeds = 0u;
    for (uint16_t z = 1u; p->port.binding != NULL && z <= (uint16_t)ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
            if (b.in[r].addr.kind != ZSRC_ZIGBEE) {
                continue;
            }
            for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
                if (p->reg.dev[i].used && p->reg.dev[i].ieee == b.in[r].addr.id) {
                    feeds |= (uint64_t)1u << i;
                }
            }
        }
    }
    return feeds;
}

/* At link up: every registry device described again from its record (devices.md §5), so its
 * zones work before it speaks. A device whose setup finished counts as enrolled: its CIE address
 * is stored in the device, and a device that was reset joins afresh and is set up again. */
static void describe_all(zbz_panel_t *p, uint64_t now)
{
    if (!p->describe_owed || p->src == NULL || !p->src->link_up) {
        return;
    }
    p->describe_owed = false;
    /* The devices that feed a zone first: when the source's endpoint slots run short, a sensor
     * no zone uses goes without, never one a zone needs (its zone would lose it until CHECK). */
    const uint64_t feeds = feeding_slots(p);
    for (size_t n = 0; n < 2u * ZBZ_DEV_MAX; n++) {
        const size_t i = n % ZBZ_DEV_MAX;
        const bool feeding = (feeds & ((uint64_t)1u << i)) != 0u;
        const zbz_device_t *d = &p->reg.dev[i];
        if (!d->used || feeding != (n < ZBZ_DEV_MAX)) {
            continue;
        }
        const bool active = setup_active(d);
        if (!active) {
            zsrc_zb_device_setup(p->src, d->ieee, true, now);
        }
        (void)describe(p, i, now); /* no room: live.src_full, shown on its add row if any */
        if (!active) {
            for (uint8_t k = 0; d->setup == (uint8_t)ZBZ_SETUP_DONE && k < d->ep_count; k++) {
                if (ep_is_input(&d->ep[k]) && (d->ep[k].in_clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u) {
                    zsrc_zb_ias_enrolled(p->src, d->ieee, d->ep[k].endpoint, now);
                }
            }
            zsrc_zb_device_setup(p->src, d->ieee, false, now);
        }
    }
}

static bool src_forgot(const zbz_panel_t *p, uint64_t ieee)
{
    for (size_t k = 0; p->src != NULL && k < ZSRC_ZB_FORGOTTEN_MAX; k++) {
        if (p->src->forgotten[k] == ieee && ieee != 0u) {
            return true;
        }
    }
    return false;
}

/* Removed, forgotten, or replaced: the source forgets it, the radio forgets it as far as it
 * can, and its record goes. */
static void drop_device(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    const uint64_t ieee = d->ieee;
    if (p->src != NULL) {
        zsrc_zb_device_forget(p->src, ieee, now);
    }
    if (p->radio.forget != NULL && radio_ok(p)) {
        (void)p->radio.forget(p->radio.ctx, ieee);
    }
    forget_roles(p, d->id);
    (void)zbz_reg_drop(&p->reg, d->id);
    memset(&p->live[i], 0, sizeof(p->live[i]));
}

static void ask_leave(zbz_panel_t *p, uint64_t ieee, uint16_t nwk)
{
    if (p->radio.leave != NULL) {
        (void)p->radio.leave(p->radio.ctx, ieee, nwk, new_tag(p)); /* nobody follows it */
    } else if (p->radio.remove_device != NULL) {
        (void)p->radio.remove_device(p->radio.ctx, ieee);
    }
}

/* ---- the operation the pages follow -------------------------------------------------------------- */

static bool op_running(const zbz_panel_t *p)
{
    return p->progress.op != (uint8_t)UI_ZB_OP_NONE &&
           p->progress.step != (uint8_t)UI_ZB_STEP_DONE;
}

static bool op_is(const zbz_panel_t *p, ui_zb_op_t op)
{
    return op_running(p) && p->progress.op == (uint8_t)op;
}

static bool op_listening(const zbz_panel_t *p)
{
    return op_running(p) && p->progress.step == (uint8_t)UI_ZB_STEP_LISTENING &&
           (p->progress.op == (uint8_t)UI_ZB_OP_ADD || p->progress.op == (uint8_t)UI_ZB_OP_ADD_CODE ||
            p->progress.op == (uint8_t)UI_ZB_OP_REPLACE);
}

static void op_reset(zbz_panel_t *p, ui_zb_op_t op, uint8_t user, uint8_t via)
{
    memset(&p->progress, 0, sizeof(p->progress));
    p->progress.op = (uint8_t)op;
    p->deadline = 0u;
    p->op_tag = 0u;
    p->op_user = user;
    p->op_via = via;
    p->op_ieee = 0u;
    p->replacing = 0u;
    p->replace_with = 0u;
    p->op_wait_frame = false;
    p->op_channel = 0u;
    p->test_id = 0u;
}

static void op_step(zbz_panel_t *p, ui_zb_step_t step, uint64_t deadline)
{
    p->progress.step = (uint8_t)step;
    p->deadline = deadline;
    notify(p);
}

static void op_done(zbz_panel_t *p, ui_zb_result_t result)
{
    p->progress.step = (uint8_t)UI_ZB_STEP_DONE;
    p->progress.result = (uint8_t)result;
    p->deadline = 0u;
    p->op_tag = 0u;
    p->op_wait_frame = false;
    notify(p);
}

/* "Test it" ended: its result is the operation's and the siren page's last_test (RAM only). The
 * slot is the siren's own, else a free one, else one whose siren is no longer assigned. */
static void siren_test_end(zbz_panel_t *p, ui_zb_result_t result)
{
    size_t slot = ZBW_SIRENS_MAX;
    for (size_t k = 0; k < ZBW_SIRENS_MAX && slot == ZBW_SIRENS_MAX; k++) {
        if (p->siren_test[k].id == p->test_id) {
            slot = k;
        }
    }
    for (size_t k = 0; k < ZBW_SIRENS_MAX && slot == ZBW_SIRENS_MAX; k++) {
        if (p->siren_test[k].id == 0u || zbw_find(&p->wd, p->siren_test[k].id) == NULL) {
            slot = k;
        }
    }
    if (slot == ZBW_SIRENS_MAX) {
        slot = 0u;
    }
    p->siren_test[slot].id = p->test_id;
    p->siren_test[slot].result = (uint8_t)result;
    op_done(p, result);
}

/* How a test's Start Warning ended, for the page. */
static ui_zb_result_t test_result(uint8_t done)
{
    switch ((zsrc_zb_done_t)done) {
    case ZSRC_ZB_DONE_OK: return UI_ZB_RES_DONE;
    case ZSRC_ZB_DONE_TIMEOUT: return UI_ZB_RES_NO_ANSWER;
    default: return UI_ZB_RES_FAILED;
    }
}

/* A device the operation concerns, as it is now (its model and zones, for the result page). */
static void op_capture(zbz_panel_t *p, const zbz_device_t *d)
{
    ui_zb_progress_t *g = &p->progress;
    g->id = d->id;
    model_name(d, g->model, sizeof(g->model));
    const size_t n = device_zones(p, d->ieee, g->zones, UI_ZB_ZONES_MAX);
    g->zone_count = (uint8_t)(n < UI_ZB_ZONES_MAX ? n : UI_ZB_ZONES_MAX);
}

/* The join window: the source's learn (it opens permit join and marks what joins "Just added"). */
static bool open_window(zbz_panel_t *p, uint64_t now)
{
    const zone_source_ops_t *ops = zsrc_zb_ops();
    return p->src != NULL && ops->learn != NULL && ops->learn(p->src, true, now);
}

static void close_window(zbz_panel_t *p, uint64_t now)
{
    const zone_source_ops_t *ops = zsrc_zb_ops();
    if (p->src != NULL && ops->learn != NULL) {
        (void)ops->learn(p->src, false, now);
    }
    if (p->progress.op == (uint8_t)UI_ZB_OP_ADD_CODE && p->radio.codes_only != NULL) {
        (void)p->radio.codes_only(p->radio.ctx, false);
    }
}

/* ---- the add page's rows ------------------------------------------------------------------------ */

static uint8_t row_step(const zbz_device_t *d)
{
    switch ((zbz_setup_t)d->setup) {
    case ZBZ_SETUP_NEW: return (uint8_t)UI_ZB_STEP_JOINING;
    case ZBZ_SETUP_RUNNING:
    case ZBZ_SETUP_WAKE:
        return (uint8_t)(d->setup_step <= (uint8_t)ZSRC_ZB_STEP_POWER_DESC ? UI_ZB_STEP_READING
                                                                           : UI_ZB_STEP_SETTING_UP);
    default: return (uint8_t)UI_ZB_STEP_DONE;
    }
}

static uint8_t setup_result(const zbz_device_t *d)
{
    switch ((zbz_setup_t)d->setup) {
    case ZBZ_SETUP_DONE:
        return (uint8_t)(d->role == (uint8_t)ZBZ_ROLE_SENSOR ? UI_ZB_RES_ADDED
                                                              : UI_ZB_RES_NOT_A_SENSOR);
    case ZBZ_SETUP_NOT_USABLE: return (uint8_t)UI_ZB_RES_NOT_A_SENSOR;
    case ZBZ_SETUP_STOPPED: return (uint8_t)UI_ZB_RES_SETUP_NOT_FINISHED;
    default: return (uint8_t)UI_ZB_RES_NONE;
    }
}

/* ... and on the page: a sensor the zone source had no room for is FULL, not ADDED - no zone
 * could use it (devices.md §4). */
static uint8_t result_of(const zbz_panel_t *p, const zbz_device_t *d)
{
    const uint8_t r = setup_result(d);
    if (r == (uint8_t)UI_ZB_RES_ADDED && p->live[slot_of(p, d)].src_full) {
        return (uint8_t)UI_ZB_RES_FULL;
    }
    return r;
}

static bool row_sticky(const ui_zb_joined_t *row)
{
    return row->result == (uint8_t)UI_ZB_RES_ALREADY_ADDED ||
           row->result == (uint8_t)UI_ZB_RES_JOIN_FAILED;
}

static ui_zb_joined_t *row_of(zbz_panel_t *p, uint16_t id)
{
    for (uint8_t k = 0; k < p->progress.joined_count && k < UI_ZB_JOINED_MAX; k++) {
        if (p->progress.joined[k].id == id) {
            return &p->progress.joined[k];
        }
    }
    return NULL;
}

static ui_zb_joined_t *row_add(zbz_panel_t *p, const zbz_device_t *d)
{
    ui_zb_joined_t *row = row_of(p, d->id);
    if (row == NULL && p->progress.joined_count < UI_ZB_JOINED_MAX) {
        row = &p->progress.joined[p->progress.joined_count++];
    }
    if (row != NULL) {
        memset(row, 0, sizeof(*row));
        row->id = d->id;
        row->step = (uint8_t)UI_ZB_STEP_JOINING;
    }
    return row;
}

static bool rows_add_op(const zbz_panel_t *p)
{
    return p->progress.op == (uint8_t)UI_ZB_OP_ADD || p->progress.op == (uint8_t)UI_ZB_OP_ADD_CODE;
}

/* The rows follow their devices' setups, the operation ended or not (until it is dismissed). */
static void rows_refresh(zbz_panel_t *p)
{
    if (!rows_add_op(p)) {
        return;
    }
    for (uint8_t k = 0; k < p->progress.joined_count && k < UI_ZB_JOINED_MAX; k++) {
        ui_zb_joined_t *row = &p->progress.joined[k];
        const zbz_device_t *d = zbz_reg_cget(&p->reg, row->id);
        if (d == NULL) {
            continue; /* removed since: the row keeps what it last said */
        }
        model_name(d, row->model, sizeof(row->model));
        if (row_sticky(row)) {
            row->step = (uint8_t)UI_ZB_STEP_DONE;
            continue;
        }
        const size_t i = slot_of(p, d);
        row->step = row_step(d);
        row->result = row->step == (uint8_t)UI_ZB_STEP_DONE ? result_of(p, d) : 0u;
        row->asleep = setup_active(d) && p->live[i].asleep;
    }
}

/* ADD and ADD_CODE once the window has closed: done when every row is (ADDED if any was). */
static void rows_follow(zbz_panel_t *p)
{
    const ui_zb_progress_t *g = &p->progress;
    if (!rows_add_op(p) || !op_running(p) || g->step == (uint8_t)UI_ZB_STEP_LISTENING ||
        g->step == (uint8_t)UI_ZB_STEP_STARTING) {
        return;
    }
    if (g->joined_count == 0u) {
        return;
    }
    bool all_done = true, any_added = false, any_reading = false;
    for (uint8_t k = 0; k < g->joined_count && k < UI_ZB_JOINED_MAX; k++) {
        all_done = all_done && g->joined[k].step == (uint8_t)UI_ZB_STEP_DONE;
        any_added = any_added || g->joined[k].result == (uint8_t)UI_ZB_RES_ADDED;
        any_reading = any_reading || g->joined[k].step == (uint8_t)UI_ZB_STEP_READING ||
                      g->joined[k].step == (uint8_t)UI_ZB_STEP_JOINING;
    }
    if (all_done) {
        const size_t rows = g->joined_count < UI_ZB_JOINED_MAX ? g->joined_count : UI_ZB_JOINED_MAX;
        const uint8_t newest = g->joined[rows - 1u].result;
        op_done(p, any_added ? UI_ZB_RES_ADDED : (ui_zb_result_t)newest);
    } else {
        const uint8_t step = (uint8_t)(any_reading ? UI_ZB_STEP_READING : UI_ZB_STEP_SETTING_UP);
        if (g->step != step) {
            op_step(p, (ui_zb_step_t)step, 0u);
        }
    }
}

/* SETUP and REPLACE follow one device's setup. */
static void op_follow(zbz_panel_t *p)
{
    rows_refresh(p);
    rows_follow(p);
    if (op_is(p, UI_ZB_OP_SETUP)) {
        const zbz_device_t *d = zbz_reg_cget(&p->reg, p->progress.id);
        if (d == NULL) {
            op_done(p, UI_ZB_RES_FAILED);
        } else if (!setup_active(d)) {
            op_done(p, (ui_zb_result_t)result_of(p, d));
        } else if (p->progress.step != row_step(d)) {
            op_step(p, (ui_zb_step_t)row_step(d), 0u);
        }
    }
    if (op_is(p, UI_ZB_OP_REPLACE) && p->progress.step != (uint8_t)UI_ZB_STEP_LISTENING) {
        const zbz_device_t *d = zbz_reg_cget(&p->reg, p->progress.id);
        if (d == NULL || !setup_active(d)) {
            op_done(p, UI_ZB_RES_NOTHING_JOINED); /* the window ran out: nothing changed */
        }
    }
}

/* ---- each device's setup (devices.md §5) --------------------------------------------------------- */

typedef struct {
    uint8_t ep;
    uint16_t cluster;
} item_t;

typedef enum {
    SEND_SENT = 0,   /* a request is out: its DONE (or the step deadline) follows */
    SEND_WAIT,       /* nothing out, something awaited (ENROLL's gap, a retry) */
    SEND_SKIP,       /* the item is done (or doesn't apply): the next one */
    SEND_FAILED,     /* it could not even be queued: a try, then again after ZBZ_STEP_MS */
} send_t;

/* The clusters BIND and REPORTING take, in order. A sensor's readings; for every device its
 * battery and its check-ins (devices.md §5: the IAS Zone cluster is never bound - it sends to
 * the CIE). */
static const uint16_t BIND_CLUSTERS[] = {
    ZSRC_ZB_ZCL_ON_OFF, ZSRC_ZB_ZCL_OCCUPANCY, ZSRC_ZB_ZCL_TEMP_MEAS, ZSRC_ZB_ZCL_POWER_CFG,
    ZSRC_ZB_ZCL_POLL_CTRL,
};
#define BIND_COUNT (sizeof(BIND_CLUSTERS) / sizeof(BIND_CLUSTERS[0]))

static bool reporting_of(uint16_t cluster, zsrc_zb_reporting_t *r)
{
    memset(r, 0, sizeof(*r));
    r->cluster = cluster;
    switch (cluster) {
    case ZSRC_ZB_ZCL_POWER_CFG: /* battery percentage remaining: 1 h..12 h, 2 % (in half %) */
        r->attr = 0x0021u;
        r->type = 0x20u;
        r->min_s = 3600u;
        r->max_s = 43200u;
        r->change = 4u;
        return true;
    case ZSRC_ZB_ZCL_TEMP_MEAS: /* 5 min..1 h, 0.2 C */
        r->attr = 0x0000u;
        r->type = 0x29u;
        r->min_s = 300u;
        r->max_s = 3600u;
        r->change = 20u;
        return true;
    case ZSRC_ZB_ZCL_ON_OFF: /* 0..1 h */
        r->attr = 0x0000u;
        r->type = 0x10u;
        r->max_s = 3600u;
        return true;
    case ZSRC_ZB_ZCL_OCCUPANCY:
        r->attr = 0x0000u;
        r->type = 0x18u;
        r->max_s = 3600u;
        return true;
    default:
        return false;
    }
}

/* The idx-th (endpoint, cluster) pair BIND (or REPORTING) takes. */
static bool bind_item(const zbz_device_t *d, bool reporting, uint8_t idx, item_t *out)
{
    const bool sensor = d->role == (uint8_t)ZBZ_ROLE_SENSOR;
    unsigned n = 0u;
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        const zbz_endpoint_t *e = &d->ep[k];
        if (e->profile != HA_PROFILE) {
            continue;
        }
        for (size_t c = 0; c < BIND_COUNT; c++) {
            const uint16_t cl = BIND_CLUSTERS[c];
            const bool reading = cl != ZSRC_ZB_ZCL_POWER_CFG && cl != ZSRC_ZB_ZCL_POLL_CTRL;
            if ((e->in_clusters & zsrc_zb_cluster_bit(cl)) == 0u || (reading && !sensor) ||
                (reading && !ep_is_input(e))) {
                continue;
            }
            zsrc_zb_reporting_t r;
            if (reporting && !reporting_of(cl, &r)) {
                continue;
            }
            if (n == idx) {
                out->ep = e->endpoint;
                out->cluster = cl;
                return true;
            }
            n++;
        }
    }
    return false;
}

/* The idx-th endpoint with `bit` among its input clusters (HA profile). */
static bool ep_with(const zbz_device_t *d, uint16_t bit, uint8_t idx, item_t *out)
{
    unsigned n = 0u;
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        if (d->ep[k].profile == HA_PROFILE && (d->ep[k].in_clusters & bit) != 0u) {
            if (n == idx) {
                out->ep = d->ep[k].endpoint;
                return true;
            }
            n++;
        }
    }
    return false;
}

/* Basic is read from the first endpoint that has it, not always endpoint 1. */
static uint8_t basic_ep(const zbz_device_t *d)
{
    item_t it = {0u, 0u};
    if (ep_with(d, ZSRC_ZB_CL_BASIC, 0u, &it)) {
        return it.ep;
    }
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        if (d->ep[k].endpoint != 0u && d->ep[k].endpoint <= 240u) {
            return d->ep[k].endpoint;
        }
    }
    return 1u;
}

/* The step's idx-th item; false when it has no (more): the next step. */
static bool item_of(const zbz_panel_t *p, size_t i, uint8_t step, uint8_t idx, item_t *out)
{
    const zbz_device_t *d = &p->reg.dev[i];
    const zbz_live_t *l = &p->live[i];
    memset(out, 0, sizeof(*out));
    switch ((zsrc_zb_step_t)step) {
    case ZSRC_ZB_STEP_NODE_DESC:
    case ZSRC_ZB_STEP_ACTIVE_EP:
        return idx == 0u;
    case ZSRC_ZB_STEP_SIMPLE_DESC:
        if (!l->ep_list_known || idx >= l->ep_list_n) {
            return false;
        }
        out->ep = l->ep_list[idx];
        return true;
    case ZSRC_ZB_STEP_BASIC:
        out->ep = basic_ep(d);
        out->cluster = ZSRC_ZB_ZCL_BASIC;
        return idx < BASIC_READS;
    case ZSRC_ZB_STEP_POWER_DESC:
        return idx == 0u && d->power_source == 0u;
    case ZSRC_ZB_STEP_IAS_ZONE:
    case ZSRC_ZB_STEP_CIE_WRITE:
    case ZSRC_ZB_STEP_ENROLL:
        out->cluster = ZSRC_ZB_ZCL_IAS_ZONE;
        return ep_with(d, ZSRC_ZB_CL_IAS_ZONE, idx, out);
    case ZSRC_ZB_STEP_BIND:
        return bind_item(d, false, idx, out);
    case ZSRC_ZB_STEP_REPORTING:
        return bind_item(d, true, idx, out);
    case ZSRC_ZB_STEP_POLL_CTRL: {
        /* Its check-in interval, then (ZB03) an IAS WD server's MaxDuration: the step numbers
         * are stored (zsrc_zb_step_t), so the read joins the last step instead of a new one. */
        uint8_t k = idx;
        if (ep_with(d, ZSRC_ZB_CL_POLL_CTRL, 0u, out)) {
            if (k == 0u) {
                out->cluster = ZSRC_ZB_ZCL_POLL_CTRL;
                return true;
            }
            k--;
        }
        out->cluster = ZSRC_ZB_ZCL_IAS_WD;
        return k == 0u && ep_with(d, ZSRC_ZB_CL_IAS_WD, 0u, out);
    }
    default:
        return false;
    }
}

/* Steps whose failure doesn't stop the setup: some devices never answer them (devices.md §5). */
static bool step_optional(uint8_t step)
{
    return step == (uint8_t)ZSRC_ZB_STEP_NODE_DESC || step == (uint8_t)ZSRC_ZB_STEP_BASIC ||
           step == (uint8_t)ZSRC_ZB_STEP_POWER_DESC || step == (uint8_t)ZSRC_ZB_STEP_ENROLL ||
           step == (uint8_t)ZSRC_ZB_STEP_POLL_CTRL;
}

static void setup_go(zbz_panel_t *p, size_t i, uint64_t now);

static void setup_stop(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    d->setup = (uint8_t)ZBZ_SETUP_STOPPED; /* setup_step keeps where: "Set it up again" */
    zbz_reg_touch(&p->reg, d);
    l->tag = 0u;
    l->step_deadline = 0u;
    l->asleep = false;
    if (p->src != NULL) {
        zsrc_zb_device_setup(p->src, d->ieee, false, now);
    }
    notify(p);
}

/* REPLACE (ZB-013): a new sensor with an input of each class the old one fed takes its zones.
 * With port.replace_waits and not `go`, one found moves nothing: it waits for
 * zbz_panel_replace_gated, which comes back here with `go` under a claimed gate (RF32). */
static void try_replace(zbz_panel_t *p, size_t i, uint64_t now, bool go)
{
    zbz_device_t *nd = &p->reg.dev[i];
    zbz_device_t *od = zbz_reg_get(&p->reg, p->replacing);
    if (od == NULL) {
        op_done(p, UI_ZB_RES_FAILED);
        return;
    }
    /* The old one's inputs per zone and role (bit r of roles[z-1], its endpoint in
     * inputs[z-1][r]): a zone already moved goes back if the hub refuses a later one (armed),
     * so its zones never end up split between two sensors (ZB-013). */
    uint8_t roles[ALARM_MAX_ZONES];
    uint8_t inputs[ALARM_MAX_ZONES][ZSRC_ROLE_COUNT];
    memset(roles, 0, sizeof(roles));
    memset(inputs, 0, sizeof(inputs));
    /* First: does every input of the old one find an input of its class on the new one? */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && p->port.replace_waits && !go) {
            if (p->replace_with == 0u) {
                p->replace_with = nd->id; /* the first one found; the window stays open */
            }
            return;
        }
        ui_zb_progress_t *g = &p->progress;
        uint8_t moved = 0u;
        uint16_t refused = 0u;
        for (uint16_t z = 1u;
             p->port.binding != NULL && z <= (uint16_t)ALARM_MAX_ZONES && refused == 0u; z++) {
            zone_hub_binding_t b;
            if (!p->port.binding(p->port.ctx, z, &b) || !binding_has(&b, od->ieee)) {
                continue;
            }
            for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
                if (!input_is(&b.in[r], od->ieee)) {
                    continue;
                }
                const uint8_t cls = class_of_ep(p, od, b.in[r].addr.input);
                uint8_t ep = 0u;
                for (uint8_t k = 0; k < nd->ep_count && ep == 0u; k++) {
                    if (ep_is_input(&nd->ep[k]) && class_of_ep(p, nd, nd->ep[k].endpoint) == cls) {
                        ep = nd->ep[k].endpoint;
                    }
                }
                if (ep == 0u) {
                    return; /* not its replacement: it stays added, the window stays open */
                }
                roles[z - 1u] = (uint8_t)(roles[z - 1u] | (1u << r));
                inputs[z - 1u][r] = b.in[r].addr.input;
                b.in[r].addr = zsrc_zb_address(nd->ieee, ep);
            }
            if (pass == 1) {
                if (p->port.rebind == NULL || !p->port.rebind(p->port.ctx, z, &b)) {
                    refused = z;
                } else if (moved < UI_ZB_ZONES_MAX) {
                    g->zones[moved++] = z;
                }
            }
        }
        if (pass == 1) {
            g->zone_count = moved;
            if (refused != 0u) {
                /* Put back what moved: every zone before the refused one went to the new one. */
                for (uint16_t z = 1u; z < refused; z++) {
                    zone_hub_binding_t b;
                    if (roles[z - 1u] == 0u || !p->port.binding(p->port.ctx, z, &b)) {
                        continue;
                    }
                    for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
                        if ((roles[z - 1u] & (1u << r)) != 0u) {
                            b.in[r].addr = zsrc_zb_address(od->ieee, inputs[z - 1u][r]);
                        }
                    }
                    (void)p->port.rebind(p->port.ctx, z, &b);
                }
                g->zone_count = 0u;
                close_window(p, now);
                op_done(p, UI_ZB_RES_FAILED);
                return;
            }
        }
    }
    ui_zb_progress_t *g = &p->progress;
    close_window(p, now);
    g->old_id = od->id;
    g->id = nd->id;
    model_name(nd, g->model, sizeof(g->model));
    log_ev(p, ZBZ_EV_REPLACED, g->zone_count > 0u ? g->zones[0] : 0u, nd->id, 0u, p->op_user,
           p->op_via);
    drop_device(p, slot_of(p, od), now);
    op_done(p, UI_ZB_RES_REPLACED);
}

static void setup_finish(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    d->role = zbz_role_of(d);
    d->setup = (uint8_t)(role_used(d->role) ? ZBZ_SETUP_DONE : ZBZ_SETUP_NOT_USABLE);
    d->setup_step = (uint8_t)ZSRC_ZB_STEP_DONE;
    zbz_reg_touch(&p->reg, d);
    l->tag = 0u;
    l->step_deadline = 0u;
    l->asleep = false;
    l->tries = 0u;
    (void)describe(p, i, now); /* no room in the source: its add row ends FULL (rows_refresh) */
    if (p->src != NULL) {
        zsrc_zb_device_setup(p->src, d->ieee, false, now);
    }
    if (op_is(p, UI_ZB_OP_REPLACE) && d->id != p->replacing &&
        d->role == (uint8_t)ZBZ_ROLE_SENSOR) {
        try_replace(p, i, now, false);
    }
    notify(p);
}

static void next_step(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    d->setup_step++;
    l->ep_next = 0u;
    l->tries = 0u;
    l->tag = 0u;
    l->step_deadline = 0u;
    zbz_reg_touch(&p->reg, d);
    if (d->setup_step == (uint8_t)ZSRC_ZB_STEP_IAS_ZONE) {
        /* Descriptors and Basic known: what it is, and a sensor's inputs to the source. */
        d->role = zbz_role_of(d);
        (void)describe(p, i, now);
    }
}

static send_t send_enroll(zbz_panel_t *p, size_t i, const item_t *it, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    const zsrc_zb_device_t *s = p->src != NULL ? zsrc_zb_device(p->src, d->ieee, it->ep) : NULL;
    /* Only the quirk skips it. A status heard (status_known) or the source's DONE is no proof
     * of enrollment: the source takes any status word as working (ZB-007), and a sensor that
     * waits for its Enroll Response before it sends Zone Status Change Notifications would never
     * trip its zone - fire, CO and panic above all (devices.md section 5, ENROLL). */
    if (s != NULL && (s->quirks & ZSRC_ZB_QUIRK_NO_ENROLL) != 0u) {
        zsrc_zb_ias_enrolled(p->src, d->ieee, it->ep, now); /* it reports: nothing to chase */
        return SEND_SKIP;
    }
    if (p->radio.ias_enroll_response == NULL) {
        return SEND_SKIP;
    }
    const uint8_t zone_id = s != NULL ? s->zone_id : (uint8_t)(ZBZ_ZONE_ID_OWN + i);
    if (s != NULL && s->enroll == (uint8_t)ZSRC_ZB_ENROLL_RESPOND) {
        /* Its Enroll Request came: answered now, in the wake window it opened. */
        if (p->radio.ias_enroll_response(p->radio.ctx, d->ieee, it->ep, zone_id)) {
            zsrc_zb_ias_enrolled(p->src, d->ieee, it->ep, now);
            return SEND_SKIP;
        }
    }
    if (l->tries >= ZSRC_ZB_ENROLL_TRIES) {
        return SEND_SKIP; /* not enrolled: the source chases it after the setup, patiently */
    }
    if (l->step_deadline > now) {
        return SEND_WAIT;
    }
    /* No request yet: an Enroll Response unasked (the auto-enroll-response way), then wait. */
    (void)p->radio.ias_enroll_response(p->radio.ctx, d->ieee, it->ep, zone_id);
    l->tries++;
    l->step_deadline = add_ms(now, ZSRC_ZB_ENROLL_GAP_MS);
    return SEND_WAIT;
}

static send_t send_item(zbz_panel_t *p, size_t i, const item_t *it, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    const zsrc_zb_radio_t *rd = &p->radio;
    const uint32_t tag = new_tag(p);
    bool ok;
    switch ((zsrc_zb_step_t)d->setup_step) {
    case ZSRC_ZB_STEP_NODE_DESC:
        if (rd->node_desc_req == NULL) {
            return SEND_SKIP;
        }
        ok = rd->node_desc_req(rd->ctx, d->ieee, d->nwk, tag);
        break;
    case ZSRC_ZB_STEP_ACTIVE_EP:
        if (rd->active_ep_req == NULL) {
            return SEND_SKIP;
        }
        ok = rd->active_ep_req(rd->ctx, d->ieee, d->nwk, tag);
        break;
    case ZSRC_ZB_STEP_SIMPLE_DESC:
        if (rd->simple_desc_req == NULL) {
            return SEND_SKIP;
        }
        ok = rd->simple_desc_req(rd->ctx, d->ieee, d->nwk, it->ep, tag);
        break;
    case ZSRC_ZB_STEP_BASIC: {
        /* Three reads (devices.md §5): a manufacturer and a model at their longest (32
         * characters each) don't fit one answer of ZSRC_ZB_ZCL_PAYLOAD_MAX bytes - IKEA's
         * PARASOLL needs 51 - and a device answers only the records that fit. The manufacturer
         * with the power source, the model alone, then the numbers and the software build. */
        static const uint16_t mfr[] = {0x0004u, 0x0007u};
        static const uint16_t model[] = {0x0005u};
        static const uint16_t rest[] = {0x0000u, 0x0001u, 0x0003u, 0x4000u};
        static const struct {
            const uint16_t *attrs;
            uint8_t count;
        } reads[BASIC_READS] = {{mfr, 2u}, {model, 1u}, {rest, 4u}};
        if (rd->read_attributes == NULL || l->ep_next >= BASIC_READS) {
            return SEND_SKIP;
        }
        ok = rd->read_attributes(rd->ctx, d->ieee, it->ep, ZSRC_ZB_ZCL_BASIC,
                                 reads[l->ep_next].attrs, reads[l->ep_next].count, tag);
        break;
    }
    case ZSRC_ZB_STEP_POWER_DESC:
        if (rd->power_desc_req == NULL) {
            return SEND_SKIP;
        }
        ok = rd->power_desc_req(rd->ctx, d->ieee, d->nwk, tag);
        break;
    case ZSRC_ZB_STEP_IAS_ZONE: {
        /* zone state, zone type, the CIE address it holds. Not the zone status (0x0002): the
         * radio side hands a read status to the source as a status heard (zsrc_zb_ias_status),
         * which the source takes as enrolled (devices.md section 5). */
        static const uint16_t attrs[] = {0x0000u, 0x0001u, 0x0010u};
        if (rd->read_attributes == NULL) {
            return SEND_SKIP;
        }
        ok = rd->read_attributes(rd->ctx, d->ieee, it->ep, ZSRC_ZB_ZCL_IAS_ZONE, attrs,
                                 (uint8_t)(sizeof(attrs) / sizeof(attrs[0])), tag);
        break;
    }
    case ZSRC_ZB_STEP_CIE_WRITE:
        /* Always written: nothing reports the CIE address it holds yet (devices.md §5). */
        if (rd->ias_cie_write == NULL) {
            return SEND_SKIP;
        }
        return rd->ias_cie_write(rd->ctx, d->ieee, it->ep) ? SEND_SKIP : SEND_FAILED;
    case ZSRC_ZB_STEP_ENROLL:
        return send_enroll(p, i, it, now);
    case ZSRC_ZB_STEP_BIND:
        if (rd->bind == NULL) {
            return SEND_SKIP;
        }
        ok = rd->bind(rd->ctx, d->ieee, it->ep, it->cluster, tag);
        break;
    case ZSRC_ZB_STEP_REPORTING: {
        zsrc_zb_reporting_t r;
        if (rd->configure_reporting == NULL || !reporting_of(it->cluster, &r)) {
            return SEND_SKIP;
        }
        ok = rd->configure_reporting(rd->ctx, d->ieee, it->ep, &r, tag);
        break;
    }
    case ZSRC_ZB_STEP_POLL_CTRL: {
        /* Check-in Interval, or an IAS WD's MaxDuration (ZBW_ATTR_MAX_DURATION): both 0x0000. */
        static const uint16_t attrs[] = {0x0000u};
        if (rd->read_attributes == NULL) {
            return SEND_SKIP;
        }
        ok = rd->read_attributes(rd->ctx, d->ieee, it->ep, it->cluster, attrs, 1u, tag);
        break;
    }
    default:
        return SEND_SKIP;
    }
    if (!ok) {
        return SEND_FAILED;
    }
    l->tag = tag;
    l->step_deadline = add_ms(now, ZBZ_STEP_MS);
    return SEND_SENT;
}

/* A try of the current item failed: again, or past ZBZ_SETUP_TRIES the step is given up - the
 * next step for an optional one, else the setup STOPS there. True: go on at once. */
static bool setup_try_failed(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    l->tag = 0u;
    if (l->tries < 0xFFu) {
        l->tries++;
    }
    if (l->tries < ZBZ_SETUP_TRIES) {
        return true;
    }
    if (step_optional(d->setup_step)) {
        next_step(p, i, now);
        return true;
    }
    setup_stop(p, i, now);
    return false;
}

static void setup_go(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    for (unsigned guard = 0u; guard < LOOP_GUARD; guard++) {
        if (!d->used || !setup_active(d) || l->tag != 0u || l->asleep) {
            return;
        }
        if (l->key_wait != 0u) {
            if (now < l->key_wait) {
                l->step_deadline = l->key_wait; /* its own key exchange first (B021) */
                return;
            }
            l->key_wait = 0u;
        }
        if (!radio_ok(p)) {
            l->step_deadline = 0u; /* it waits for the radio (zbz_panel_radio goes on) */
            return;
        }
        if (d->setup_step < (uint8_t)ZSRC_ZB_STEP_NODE_DESC) {
            d->setup_step = (uint8_t)ZSRC_ZB_STEP_NODE_DESC;
        }
        if (d->setup_step >= (uint8_t)ZSRC_ZB_STEP_DONE) {
            setup_finish(p, i, now);
            return;
        }
        item_t it;
        if (!item_of(p, i, d->setup_step, l->ep_next, &it)) {
            next_step(p, i, now);
            continue;
        }
        switch (send_item(p, i, &it, now)) {
        case SEND_SENT:
        case SEND_WAIT:
            return;
        case SEND_SKIP:
            l->ep_next++;
            l->tries = 0u;
            l->step_deadline = 0u;
            continue;
        case SEND_FAILED:
        default: {
            const uint8_t step = d->setup_step;
            if (!setup_try_failed(p, i, now)) {
                return; /* STOPPED */
            }
            if (d->setup_step != step) {
                continue; /* given up as optional: the next step at once */
            }
            l->step_deadline = add_ms(now, ZBZ_STEP_MS); /* not queued: again after a while */
            return;
        }
        }
    }
}

static void setup_start(zbz_panel_t *p, size_t i, uint8_t from, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    if (from < (uint8_t)ZSRC_ZB_STEP_NODE_DESC || from >= (uint8_t)ZSRC_ZB_STEP_DONE) {
        from = (uint8_t)ZSRC_ZB_STEP_NODE_DESC;
    }
    if (from == (uint8_t)ZSRC_ZB_STEP_SIMPLE_DESC && !l->ep_list_known) {
        from = (uint8_t)ZSRC_ZB_STEP_ACTIVE_EP; /* the endpoint list isn't kept across restarts */
    }
    if (from <= (uint8_t)ZSRC_ZB_STEP_ACTIVE_EP) {
        l->ep_list_known = false;
        l->ep_list_n = 0u;
    }
    l->tries = 0u;
    l->ep_next = 0u;
    l->tag = 0u;
    l->step_deadline = 0u;
    l->setup_since = now;
    l->asleep = false;
    d->setup = (uint8_t)ZBZ_SETUP_RUNNING;
    d->setup_step = from;
    zbz_reg_touch(&p->reg, d);
    if (p->src != NULL) {
        zsrc_zb_device_setup(p->src, d->ieee, true, now);
    }
    notify(p);
    setup_go(p, i, now);
}

static void setup_asleep(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_live_t *l = &p->live[i];
    l->tag = 0u;
    l->asleep = true;
    l->step_deadline = add_ms(now, ZBZ_WAKE_WAIT_MS); /* then the step again anyway */
    p->reg.dev[i].setup = (uint8_t)ZBZ_SETUP_WAKE;    /* reads back STOPPED: not written */
    notify(p);
}

/* The current item's request ended (REP_DONE with its tag, or its deadline as TIMEOUT). */
static void setup_done(zbz_panel_t *p, size_t i, uint8_t result, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    l->tag = 0u;
    l->step_deadline = 0u;
    switch ((zsrc_zb_done_t)result) {
    case ZSRC_ZB_DONE_OK:
        if (d->setup_step == (uint8_t)ZSRC_ZB_STEP_IAS_ZONE && p->src != NULL) {
            /* The zone type it read back reached the source's slot: the record keeps it. */
            item_t it;
            const zsrc_zb_device_t *s =
                item_of(p, i, d->setup_step, l->ep_next, &it)
                    ? zsrc_zb_device(p->src, d->ieee, it.ep)
                    : NULL;
            for (uint8_t k = 0; s != NULL && k < d->ep_count; k++) {
                if (d->ep[k].endpoint == it.ep && s->zone_type != 0u &&
                    s->zone_type != ZSRC_ZB_ZONE_TYPE_NONE && d->ep[k].zone_type != s->zone_type) {
                    d->ep[k].zone_type = s->zone_type;
                    zbz_reg_touch(&p->reg, d);
                }
            }
        } else if (d->setup_step == (uint8_t)ZSRC_ZB_STEP_POLL_CTRL && p->src != NULL) {
            /* Its check-in interval, when the radio side gave it to the source (devices.md §5). */
            item_t it;
            const zsrc_zb_device_t *s =
                item_of(p, i, d->setup_step, l->ep_next, &it) &&
                        it.cluster == ZSRC_ZB_ZCL_POLL_CTRL
                    ? zsrc_zb_device(p->src, d->ieee, it.ep)
                    : NULL; /* an IAS WD's MaxDuration came as its own report (take_content) */
            if (s != NULL && s->check_in_s != 0u && s->check_in_s != d->check_in_s) {
                d->check_in_s = s->check_in_s;
                zbz_reg_touch(&p->reg, d);
                zsrc_zb_check_in_interval(p->src, d->ieee, d->check_in_s, now);
            }
        }
        l->ep_next++;
        l->tries = 0u;
        break;
    case ZSRC_ZB_DONE_UNSUPPORTED:
        l->ep_next++; /* not a failure: it hasn't got that (an endpoint without a descriptor) */
        l->tries = 0u;
        break;
    case ZSRC_ZB_DONE_NO_RADIO:
        /* No try: again after a while (setup_go waits while the radio is down, and goes on
         * when it is back), so a NO_RADIO while the radio says WORKING doesn't stall it. */
        l->step_deadline = add_ms(now, ZBZ_STEP_MS);
        return;
    case ZSRC_ZB_DONE_TIMEOUT:
        if (zbz_dev_sleepy(d)) {
            setup_asleep(p, i, now); /* its next frame is its wake window */
            return;
        }
        if (!setup_try_failed(p, i, now)) {
            return;
        }
        break;
    case ZSRC_ZB_DONE_REFUSED:
        if (d->setup_step == (uint8_t)ZSRC_ZB_STEP_SIMPLE_DESC) {
            l->ep_next++; /* an endpoint listed without a descriptor is dropped (Aqara H1) */
            l->tries = 0u;
            break;
        }
        if (!setup_try_failed(p, i, now)) {
            return;
        }
        break;
    case ZSRC_ZB_DONE_SEND_FAILED:
    default:
        if (!setup_try_failed(p, i, now)) {
            return;
        }
        break;
    }
    setup_go(p, i, now);
}

/* ---- life cycle ---------------------------------------------------------------------------------- */

void zbz_panel_init(zbz_panel_t *p, const zbz_panel_port_t *port, zsrc_zb_t *src,
                    const zsrc_zb_radio_t *radio, const zbw_port_t *wd, const zba_port_t *ace)
{
    memset(p, 0, sizeof(*p));
    if (port != NULL) {
        p->port = *port;
    }
    p->src = src;
    if (radio != NULL) {
        p->radio = *radio;
    }
    zbz_reg_init(&p->reg);
    zbz_codes_init(&p->codes);
    /* The owner's ports: only what the owner alone can do (zbz_panel.h); the policies get the
     * panel's own. */
    if (wd != NULL) {
        p->wd_owner = *wd;
    }
    if (ace != NULL) {
        p->ace_owner = *ace;
    }
    const zbw_port_t wp = wd_port(p);
    zbw_init(&p->wd, &wp);
    const zba_port_t ap = ace_port(p);
    zba_init(&p->ace, &ap);
    p->info.state = (uint8_t)UI_ZB_RADIO_NOT_CONNECTED;
    p->nvs_free = ZBZ_NVS_NOT_TOLD;
}

zbz_reg_load_t zbz_panel_load(zbz_panel_t *p, const zbz_store_port_t *store)
{
    const zbz_reg_load_t verdict = zbz_reg_load(&p->reg, store);
    zbz_codes_init(&p->codes);
    memset(&p->cfg, 0, sizeof(p->cfg));
    memset(p->live, 0, sizeof(p->live));
    const zbw_port_t wp = wd_port(p);
    zbw_init(&p->wd, &wp);
    const zba_port_t ap = ace_port(p);
    zba_init(&p->ace, &ap);
    memset(p->siren_test, 0, sizeof(p->siren_test));
    memset(p->part_known, 0, sizeof(p->part_known));
    if (store == NULL || store->read == NULL) {
        return verdict;
    }
    _Static_assert(ZBA_BLOB_MAX <= ZBZ_CODES_BLOB_MAX && ZBW_BLOB_MAX <= ZBZ_CODES_BLOB_MAX &&
                       ZBZ_CFG_BLOB <= ZBZ_CODES_BLOB_MAX,
                   "the load buffer holds every blob zbz_panel_load reads");
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    size_t n = store->read(store->ctx, ZBZ_KEY_CODES, buf, sizeof(buf));
    if (n > 0u && (n > sizeof(buf) || !zbz_codes_decode(&p->codes, buf, n))) {
        zbz_codes_init(&p->codes); /* damaged or later: empty (the trust center keeps its own) */
    }
    wipe(buf, sizeof(buf));
    /* Read with room to spare: a later firmware's longer cfg is read as far as known. */
    n = store->read(store->ctx, ZBZ_CFG_KEY, buf, sizeof(buf));
    if (n > 0u && (n > sizeof(buf) || !zbz_cfg_decode(&p->cfg, buf, n))) {
        memset(&p->cfg, 0, sizeof(p->cfg));
    }
    /* The sirens and the keypads and fobs: a damaged or later value loads nothing (each is all
     * or nothing) and isn't written over until the installer changes an assignment. Then the
     * fields that aren't stored, from the model table and the registry. */
    n = store->read(store->ctx, ZBW_KEY, buf, sizeof(buf));
    if (n > 0u && n <= sizeof(buf)) {
        (void)zbw_decode(&p->wd, buf, n);
    }
    for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
        if (p->wd.siren[k].a.used) {
            zbw_assign_t a = p->wd.siren[k].a;
            const zbz_device_t *d = zbz_reg_cget(&p->reg, a.id);
            const zbz_model_t *m = d != NULL ? zbz_model_for(d->manufacturer, d->model) : NULL;
            a.model_flags = m != NULL ? m->flags : 0u;
            a.max_duration_s = m != NULL ? m->max_duration_s : 0u;
            a.max_read = d != NULL && d->wd_max_read; /* ZB03: as its setup read it */
            a.max_read_s = d != NULL && d->wd_max_read ? d->wd_max_s : 0u;
            a.sleepy = d != NULL && zbz_dev_sleepy(d);
            (void)zbw_assign(&p->wd, &a);
        }
    }
    n = store->read(store->ctx, ZBA_KEY, buf, sizeof(buf));
    if (n > 0u && n <= sizeof(buf)) {
        (void)zba_decode(&p->ace, buf, n);
    }
    for (size_t k = 0; k < ZBA_KEYPADS_MAX; k++) {
        zba_keypad_t *kp = &p->ace.keypad[k];
        const zbz_device_t *d = kp->used ? zbz_reg_cget(&p->reg, kp->id) : NULL;
        const zbz_model_t *m = d != NULL ? zbz_model_for(d->manufacturer, d->model) : NULL;
        kp->model_flags = m != NULL ? m->flags : 0u;
    }
    for (size_t k = 0; k < ZBA_FOBS_MAX; k++) {
        zba_fob_t *f = &p->ace.fob[k];
        const zbz_device_t *d = f->used ? zbz_reg_cget(&p->reg, f->id) : NULL;
        const zbz_model_t *m = d != NULL ? zbz_model_for(d->manufacturer, d->model) : NULL;
        f->model_flags = m != NULL ? m->flags : 0u;
    }
    wipe(buf, sizeof(buf));
    p->wd.dirty = false; /* as read: nothing to write back */
    p->ace.dirty = false;
    if (radio_ok(p)) {
        zbw_radio(&p->wd, p->now, true); /* loaded while the radio works: silence counts now */
    }
    return verdict;
}

static void radio_lost(zbz_panel_t *p, uint64_t now)
{
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if (p->reg.dev[i].used && setup_active(&p->reg.dev[i])) {
            p->live[i].tag = 0u; /* lost with the radio: the step goes again once it is back */
            p->live[i].step_deadline = 0u;
        }
    }
    if (!op_running(p)) {
        return;
    }
    switch ((ui_zb_op_t)p->progress.op) {
    case UI_ZB_OP_RESTART:
        p->restart_down = true;
        break;
    case UI_ZB_OP_RESET:
    case UI_ZB_OP_SETUP:
    case UI_ZB_OP_FORGET:
        break; /* the reset may restart the radio; a setup waits for it */
    case UI_ZB_OP_ADD:
    case UI_ZB_OP_ADD_CODE:
    case UI_ZB_OP_REPLACE:
        close_window(p, now);
        op_done(p, UI_ZB_RES_FAILED);
        break;
    case UI_ZB_OP_SIREN_TEST:
        siren_test_end(p, UI_ZB_RES_FAILED); /* went with the radio */
        break;
    default:
        op_done(p, UI_ZB_RES_FAILED);
        break;
    }
}

static void radio_up(zbz_panel_t *p, uint64_t now)
{
    p->up_at = now;
    p->describe_owed = true;
    for (size_t k = 0; p->radio.install_code != NULL && k < p->codes.count; k++) {
        const zbz_code_t *c = &p->codes.codes[k];
        (void)p->radio.install_code(p->radio.ctx, c->ieee, c->code, c->len);
    }
    if (op_is(p, UI_ZB_OP_RESTART) && p->restart_down) {
        log_ev(p, ZBZ_EV_RADIO_RESTARTED, 0u, 0u, 0u, p->op_user, p->op_via);
        op_done(p, UI_ZB_RES_DONE);
    }
    describe_all(p, now);
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if (p->reg.dev[i].used && setup_active(&p->reg.dev[i]) && p->live[i].tag == 0u) {
            setup_go(p, i, now);
        }
    }
    zbw_radio(&p->wd, now, true); /* the sirens' silence counts from now */
}

void zbz_panel_radio(zbz_panel_t *p, uint64_t now, const zbz_radio_info_t *info)
{
    p->now = now;
    if (info == NULL) {
        return;
    }
    const zbz_radio_info_t old = p->info;
    p->info = *info;
    const bool was = old.state == (uint8_t)UI_ZB_RADIO_WORKING;
    const bool is = info->state == (uint8_t)UI_ZB_RADIO_WORKING;
    if (info->restarts > old.restarts && !op_is(p, UI_ZB_OP_RESTART)) {
        log_ev(p, ZBZ_EV_RADIO_RESTARTED, 0u, 0u, 1u, 0u, UI_ZB_VIA_PANEL); /* by itself */
    }
    if (info->state == (uint8_t)UI_ZB_RADIO_PAUSED && old.state != (uint8_t)UI_ZB_RADIO_PAUSED) {
        log_ev(p, ZBZ_EV_RADIO_RESTARTED, 0u, 0u, 2u, 0u, UI_ZB_VIA_PANEL); /* the guard paused it */
    }
    if (!is && op_is(p, UI_ZB_OP_RESTART)) {
        p->restart_down = true;
    }
    if (was && !is) {
        radio_lost(p, now);
        zbw_radio(&p->wd, now, false); /* no siren is silent while the radio is down */
    } else if (!was && is) {
        radio_up(p, now);
    }
    op_follow(p);
    if (old.state != info->state || old.learn_s != info->learn_s || old.jammed != info->jammed) {
        notify(p);
    }
}

/* ---- reports ------------------------------------------------------------------------------------ */

static void joined(zbz_panel_t *p, const zsrc_zb_report_t *r, uint64_t now)
{
    const uint64_t ieee = r->ieee;
    if (ieee == 0u) {
        return;
    }
    const bool fresh = !r->u.joined.rejoin;
    const bool listening = op_listening(p);
    zbz_device_t *d = zbz_reg_find(&p->reg, ieee);
    if (d != NULL) {
        const size_t i = slot_of(p, d);
        p->live[i].heard_at = now;
        if (r->nwk != 0u && r->nwk != d->nwk) {
            d->nwk = r->nwk;
            zbz_reg_touch(&p->reg, d);
        }
        if (!fresh) {
            (void)zba_greet(&p->ace, now, d->id); /* an assigned keypad: its state again */
            return; /* a rejoin: everything else stays */
        }
        /* A fresh join of a known address: it was reset - how it came in, and its setup again. */
        d->join = r->u.joined.join;
        d->joined_at = wall(p);
        zbz_reg_touch(&p->reg, d);
        if (d->join == (uint8_t)ZSRC_ZB_JOIN_INSTALL_CODE) {
            (void)zbz_codes_forget(&p->codes, ieee);
        }
        if (listening && (p->progress.op == (uint8_t)UI_ZB_OP_ADD ||
                          (p->progress.op == (uint8_t)UI_ZB_OP_ADD_CODE && ieee == p->op_ieee))) {
            ui_zb_joined_t *row = row_add(p, d);
            if (row != NULL) {
                row->result = (uint8_t)UI_ZB_RES_ALREADY_ADDED;
                row->step = (uint8_t)UI_ZB_STEP_DONE;
            }
            if (p->progress.op == (uint8_t)UI_ZB_OP_ADD_CODE) {
                close_window(p, now);
                op_step(p, UI_ZB_STEP_SETTING_UP, 0u);
            }
        }
        (void)zba_greet(&p->ace, now, d->id); /* reset, its assignment kept: its state again */
        p->live[i].key_wait = r->u.joined.key_pending ? add_ms(now, ZBZ_KEY_WAIT_MS) : 0u;
        setup_start(p, i, (uint8_t)ZSRC_ZB_STEP_NODE_DESC, now);
        return;
    }
    if (!fresh && src_forgot(p, ieee)) {
        return; /* forgotten, and its key outlived it (devices.md open question 1) */
    }
    if (!storage_full(p) && !p->reg.read_only) {
        d = zbz_reg_add(&p->reg, ieee, r->nwk);
    }
    if (d == NULL) {
        /* No room (the registry, or the storage's reserve): asked to leave (devices.md §6). */
        ask_leave(p, ieee, r->nwk);
        if (listening) {
            close_window(p, now);
            op_done(p, UI_ZB_RES_FULL);
        }
        return;
    }
    const size_t i = slot_of(p, d);
    memset(&p->live[i], 0, sizeof(p->live[i]));
    p->live[i].heard_at = now;
    d->join = r->u.joined.join;
    d->joined_at = wall(p);
    zbz_reg_touch(&p->reg, d);
    if (d->join == (uint8_t)ZSRC_ZB_JOIN_INSTALL_CODE) {
        (void)zbz_codes_forget(&p->codes, ieee);
    }
    log_ev(p, ZBZ_EV_ADDED, 0u, d->id, d->join, listening ? p->op_user : 0u, p->op_via);
    if (listening) {
        if (p->progress.op == (uint8_t)UI_ZB_OP_ADD) {
            (void)row_add(p, d);
        } else if (p->progress.op == (uint8_t)UI_ZB_OP_ADD_CODE && ieee == p->op_ieee) {
            (void)row_add(p, d);
            close_window(p, now); /* its device came: the window and codes_only end */
            op_step(p, UI_ZB_STEP_JOINING, 0u);
        } else if (p->progress.op == (uint8_t)UI_ZB_OP_REPLACE) {
            p->progress.id = d->id; /* the newest candidate */
        }
    }
    p->live[i].key_wait = r->u.joined.key_pending ? add_ms(now, ZBZ_KEY_WAIT_MS) : 0u;
    setup_start(p, i, (uint8_t)ZSRC_ZB_STEP_NODE_DESC, now);
}

/* B021: the device's own key exchange ended: its setup need not wait any longer. */
static void authorized(zbz_panel_t *p, const zsrc_zb_report_t *r, uint64_t now)
{
    zbz_device_t *d = zbz_reg_find(&p->reg, r->ieee);
    if (d == NULL) {
        return;
    }
    const size_t i = slot_of(p, d);
    zbz_live_t *l = &p->live[i];
    if (l->key_wait == 0u) {
        return;
    }
    l->key_wait = 0u;
    if (setup_active(d) && l->tag == 0u && !l->asleep) {
        l->step_deadline = 0u;
        setup_go(p, i, now);
    }
}

static void removed(zbz_panel_t *p, size_t i, uint64_t now)
{
    const zbz_device_t *d = &p->reg.dev[i];
    log_ev(p, ZBZ_EV_REMOVED, first_zone(p, d->ieee), d->id, 0u, p->op_user, p->op_via);
    drop_device(p, i, now);
    op_done(p, UI_ZB_RES_REMOVED);
}

static void left(zbz_panel_t *p, size_t i, const zsrc_zb_report_t *r, uint64_t now)
{
    const zbz_device_t *d = &p->reg.dev[i];
    if (op_is(p, UI_ZB_OP_REMOVE) && p->progress.id == d->id) {
        removed(p, i, now);
        return;
    }
    if (!r->u.left.rejoin) {
        log_ev(p, ZBZ_EV_LEFT, first_zone(p, d->ieee), d->id, 0u, 0u, UI_ZB_VIA_PANEL);
    }
    if (setup_active(d)) {
        setup_stop(p, i, now);
        ui_zb_joined_t *row = rows_add_op(p) ? row_of(p, d->id) : NULL;
        if (row != NULL) {
            row->result = (uint8_t)UI_ZB_RES_JOIN_FAILED; /* it started and didn't finish */
            row->step = (uint8_t)UI_ZB_STEP_DONE;
        }
    }
}

/* What a report says about a device's identity, into its record. */
static void take_content(zbz_panel_t *p, size_t i, const zsrc_zb_report_t *r)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    switch ((zsrc_zb_report_kind_t)r->kind) {
    case ZSRC_ZB_REP_ANNOUNCE:
        if (r->nwk != 0u) {
            d->nwk = r->nwk; /* a rejoin keeps everything else */
        }
        if (d->mac_caps == 0u) {
            d->mac_caps = r->u.announce.mac_caps;
        }
        break;
    case ZSRC_ZB_REP_NODE_DESC:
        d->node_type = (uint8_t)(r->u.node.logical_type == ZSRC_ZB_LOGICAL_END_DEVICE
                                     ? ZBZ_NODE_END_DEVICE
                                     : ZBZ_NODE_ROUTER);
        d->mac_caps = r->u.node.mac_caps;
        d->manufacturer_code = r->u.node.manufacturer_code;
        break;
    case ZSRC_ZB_REP_POWER_DESC:
        if (d->power_source == 0u) {
            const unsigned cur = r->u.power.current_source;
            d->power_source = (uint8_t)((cur & 0x01u) != 0u ? 1u : (cur & 0x06u) != 0u ? 3u : 0u);
        }
        break;
    case ZSRC_ZB_REP_ACTIVE_EP:
        if (setup_active(d)) {
            const uint8_t n = r->u.active.count < ZSRC_ZB_EP_MAX ? r->u.active.count
                                                                 : (uint8_t)ZSRC_ZB_EP_MAX;
            memcpy(l->ep_list, r->u.active.ep, n);
            l->ep_list_n = n;
            l->ep_list_known = true;
        }
        break;
    case ZSRC_ZB_REP_SIMPLE_DESC:
        (void)zbz_dev_set_endpoint(d, &r->u.simple);
        break;
    case ZSRC_ZB_REP_BASIC: {
        const zsrc_zb_basic_t *b = &r->u.basic;
        if ((b->have & ZSRC_ZB_BASIC_MANUFACTURER) != 0u) {
            copy_text(d->manufacturer, sizeof(d->manufacturer), b->manufacturer);
        }
        if ((b->have & ZSRC_ZB_BASIC_MODEL) != 0u) {
            copy_text(d->model, sizeof(d->model), b->model);
        }
        if ((b->have & ZSRC_ZB_BASIC_SW_BUILD) != 0u) {
            copy_text(d->sw_build, sizeof(d->sw_build), b->sw_build);
        }
        if ((b->have & ZSRC_ZB_BASIC_APP_VERSION) != 0u) {
            d->app_version = b->app_version;
        }
        if ((b->have & ZSRC_ZB_BASIC_HW_VERSION) != 0u) {
            d->hw_version = b->hw_version;
        }
        if ((b->have & ZSRC_ZB_BASIC_POWER) != 0u) {
            d->power_source = b->power_source;
        }
        break;
    }
    case ZSRC_ZB_REP_LINK:
        if (r->u.link.lqi != 0u) {
            l->lqi = r->u.link.lqi;
        }
        if (r->u.link.rssi != 0) {
            l->rssi = r->u.link.rssi;
        }
        break;
    case ZSRC_ZB_REP_ZCL: {
        /* ZB03: an IAS WD server's answer to the setup's MaxDuration read, its records as sent
         * (only a valid value is kept: a failed or damaged answer leaves what was known). */
        uint16_t s = 0u;
        bool wd_ep = false;
        for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
            wd_ep = wd_ep || (d->ep[k].endpoint == r->endpoint &&
                              (d->ep[k].in_clusters & ZSRC_ZB_CL_IAS_WD) != 0u);
        }
        if (!wd_ep || !zbw_max_duration_of(&r->u.zcl, &s)) {
            break;
        }
        d->wd_max_read = true;
        d->wd_max_s = s;
        for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
            zbw_assign_t *a = &p->wd.siren[k].a;
            if (a->used && a->id == d->id) {
                a->max_read = true; /* its next warning (or refresh) asks for that */
                a->max_read_s = s;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* A frame from it: alive, and awake - what waited for its wake window goes now. */
static void heard_from(zbz_panel_t *p, size_t i, uint64_t now)
{
    zbz_device_t *d = &p->reg.dev[i];
    zbz_live_t *l = &p->live[i];
    l->heard_at = now;
    if (p->op_wait_frame && op_is(p, UI_ZB_OP_PING) && p->progress.id == d->id) {
        op_done(p, UI_ZB_RES_ANSWERED);
    }
    if (setup_active(d) && l->tag == 0u &&
        (l->asleep || d->setup_step == (uint8_t)ZSRC_ZB_STEP_ENROLL)) {
        if (l->asleep) {
            l->asleep = false;
            d->setup = (uint8_t)ZBZ_SETUP_RUNNING;
            l->step_deadline = 0u;
        }
        setup_go(p, i, now);
    }
}

static void op_answer(zbz_panel_t *p, const zsrc_zb_report_t *r, uint64_t now)
{
    const bool ok = r->u.done.result == (uint8_t)ZSRC_ZB_DONE_OK;
    /* The radio went away with the request: the device was never asked, so not "it didn't
     * answer" (a remove would offer "Forget it anyway?" for a device still in the network). */
    const bool no_radio = r->u.done.result == (uint8_t)ZSRC_ZB_DONE_NO_RADIO;
    p->op_tag = 0u;
    switch ((ui_zb_op_t)p->progress.op) {
    case UI_ZB_OP_REMOVE: {
        zbz_device_t *d = zbz_reg_get(&p->reg, p->progress.id);
        if (ok && d != NULL) {
            removed(p, slot_of(p, d), now); /* it answered: it is leaving */
        } else {
            /* "Forget it anyway?" */
            op_done(p, no_radio ? UI_ZB_RES_FAILED : UI_ZB_RES_NO_ANSWER);
        }
        break;
    }
    case UI_ZB_OP_PING:
        op_done(p, ok ? UI_ZB_RES_ANSWERED : no_radio ? UI_ZB_RES_FAILED : UI_ZB_RES_NO_ANSWER);
        break;
    case UI_ZB_OP_IDENTIFY:
    case UI_ZB_OP_SURVEY:
        op_done(p, ok ? UI_ZB_RES_DONE : UI_ZB_RES_FAILED);
        break;
    case UI_ZB_OP_CHANNEL:
        if (ok) {
            p->cfg.channel_set = true;
            p->cfg.channel = p->op_channel;
            p->dirty |= ZBZ_DIRTY_CFG;
            log_ev(p, ZBZ_EV_CHANNEL, 0u, 0u, p->op_channel, p->op_user, p->op_via);
        }
        op_done(p, ok ? UI_ZB_RES_DONE : UI_ZB_RES_FAILED);
        break;
    case UI_ZB_OP_RESET:
        if (ok) {
            /* A new network: every device must be added again; waiting codes are kept. */
            for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
                zbz_device_t *d = &p->reg.dev[i];
                if (d->used) {
                    if (p->src != NULL) {
                        zsrc_zb_device_forget(p->src, d->ieee, now);
                    }
                    forget_roles(p, d->id); /* sirens, keypads and fobs: added again first */
                    (void)zbz_reg_drop(&p->reg, d->id);
                    memset(&p->live[i], 0, sizeof(p->live[i]));
                }
            }
            log_ev(p, ZBZ_EV_NETWORK_RESET, 0u, 0u, 0u, p->op_user, p->op_via);
        }
        op_done(p, ok ? UI_ZB_RES_DONE : UI_ZB_RES_FAILED);
        break;
    default:
        break;
    }
}

/* A cluster command from a device (REP_ZCL): an IAS ACE command goes to the keypads' policy, and
 * one it posts to the alarm task through the owner (alarm-devices.md §3). A Default Response
 * (command 0x0B: no IAS ACE client command has that id) is a confirmation, never answered. */
static void ace_frame(zbz_panel_t *p, uint64_t now, const zsrc_zb_report_t *r)
{
    const zsrc_zb_zcl_t *z = &r->u.zcl;
    if (z->cluster != ZSRC_ZB_ZCL_IAS_ACE || z->server_to_client ||
        z->command == ZBZ_ZCL_DEFAULT_RSP) {
        return;
    }
    zba_post_t post;
    if (zba_receive(&p->ace, now, r->ieee, r->endpoint, z, &post) == ZBA_POST) {
        const bool queued = p->port.ace_post != NULL && p->port.ace_post(p->port.ctx, &post);
        for (size_t k = 0; !queued && k < ZBA_PENDING_MAX; k++) {
            if (p->ace.pending[k].used && p->ace.pending[k].ticket == post.ticket) {
                wipe(&p->ace.pending[k], sizeof(p->ace.pending[k])); /* its retry runs again */
            }
        }
    }
    wipe(&post, sizeof(post)); /* the code: the owner keeps the one copy it queued */
}

static void join_refused(zbz_panel_t *p, const zsrc_zb_report_t *r, uint64_t now)
{
    size_t slot = 0u;
    for (size_t k = 0; k < ZBZ_REFUSED_MAX; k++) {
        if (p->refused[k].ieee == r->ieee) {
            if (p->refused[k].at != 0u && now - p->refused[k].at < ZBZ_REFUSED_LOG_MS) {
                return; /* once an hour per address */
            }
            slot = k;
            break;
        }
        if (p->refused[k].at < p->refused[slot].at) {
            slot = k; /* else the oldest gives way */
        }
    }
    p->refused[slot].ieee = r->ieee;
    p->refused[slot].at = now != 0u ? now : 1u;
    log_ev(p, ZBZ_EV_JOIN_REFUSED, 0u, 0u, r->u.refused.why, 0u, UI_ZB_VIA_PANEL);
}

/* A report that is a frame the device sent (or a request it took): proof it is there. Not its
 * leaving, a refusal, nor a request that failed. */
static bool heard(const zsrc_zb_report_t *r)
{
    switch ((zsrc_zb_report_kind_t)r->kind) {
    case ZSRC_ZB_REP_NETWORK:
    case ZSRC_ZB_REP_SURVEY:
    case ZSRC_ZB_REP_JOIN_REFUSED:
    case ZSRC_ZB_REP_LEFT:
        return false;
    case ZSRC_ZB_REP_DONE:
        return r->ieee != 0u && r->u.done.result == (uint8_t)ZSRC_ZB_DONE_OK;
    default:
        return r->ieee != 0u;
    }
}

void zbz_panel_on_report(zbz_panel_t *p, uint64_t now, const zsrc_zb_report_t *r)
{
    p->now = now;
    if (r == NULL) {
        return;
    }
    if (heard(r)) {
        zbw_heard(&p->wd, now, r->ieee); /* a siren's sign of life: not silent */
    }
    if (p->src != NULL && zsrc_zb_on_report(p->src, r, now)) {
        return; /* the source's own request ended */
    }
    switch ((zsrc_zb_report_kind_t)r->kind) {
    case ZSRC_ZB_REP_NETWORK:
        p->network_known = true;
        p->channel = r->u.network.channel;
        p->pan_id = r->u.network.pan_id;
        p->ext_pan_id = r->u.network.ext_pan_id;
        p->panel_ieee = r->u.network.panel_ieee;
        notify(p);
        break;
    case ZSRC_ZB_REP_SURVEY:
        p->survey_known = (uint8_t)(r->u.survey.known & ((1u << ZSRC_ZB_CHANNELS) - 1u));
        memcpy(p->survey_dbm, r->u.survey.dbm, sizeof(p->survey_dbm));
        notify(p);
        break;
    case ZSRC_ZB_REP_JOIN_REFUSED:
        join_refused(p, r, now);
        break;
    case ZSRC_ZB_REP_JOINED:
        joined(p, r, now);
        break;
    case ZSRC_ZB_REP_AUTHORIZED:
        authorized(p, r, now);
        break;
    case ZSRC_ZB_REP_DONE: {
        if ((r->tag & ZBW_TAG_MASK) == ZBW_TAG) {
            /* A siren's command ended: confirmed, failed, or its test's outcome. */
            (void)zbw_send_done(&p->wd, now, r->tag, (zsrc_zb_done_t)r->u.done.result);
            if (r->tag == p->op_tag && op_is(p, UI_ZB_OP_SIREN_TEST)) {
                siren_test_end(p, test_result(r->u.done.result));
            }
            break;
        }
        if ((r->tag & ZBA_TAG_MASK) == ZBA_TAG) {
            break; /* a keypad's frame went or didn't: nothing waits for it (it asks again) */
        }
        if (r->tag != 0u && r->tag == p->op_tag && op_running(p)) {
            op_answer(p, r, now);
            break;
        }
        for (size_t i = 0; r->tag != 0u && i < ZBZ_DEV_MAX; i++) {
            if (p->reg.dev[i].used && p->live[i].tag == r->tag) {
                if (r->u.done.result == (uint8_t)ZSRC_ZB_DONE_OK) {
                    p->live[i].heard_at = now; /* it answered */
                }
                setup_done(p, i, r->u.done.result, now);
                break;
            }
        }
        break;
    }
    case ZSRC_ZB_REP_NONE:
        break;
    default: {
        if (r->kind == (uint8_t)ZSRC_ZB_REP_ZCL) {
            ace_frame(p, now, r); /* any device's: one not assigned is refused */
        }
        zbz_device_t *d = zbz_reg_find(&p->reg, r->ieee);
        if (d == NULL) {
            break; /* not one of the registry's (only a trust-center join makes a record) */
        }
        const size_t i = slot_of(p, d);
        if (r->kind == (uint8_t)ZSRC_ZB_REP_LEFT) {
            left(p, i, r, now);
            break;
        }
        const zbz_device_t before = *d;
        take_content(p, i, r);
        if (memcmp(&before, d, sizeof(before)) != 0) {
            zbz_reg_touch(&p->reg, d);
        }
        heard_from(p, i, now);
        break;
    }
    }
    op_follow(p);
}

/* ---- polling ------------------------------------------------------------------------------------- */

static void op_timeout(zbz_panel_t *p, uint64_t now)
{
    switch ((ui_zb_op_t)p->progress.op) {
    case UI_ZB_OP_ADD:
    case UI_ZB_OP_ADD_CODE:
        close_window(p, now);
        if (p->progress.joined_count == 0u) {
            op_done(p, UI_ZB_RES_NOTHING_JOINED);
        } else {
            op_step(p, UI_ZB_STEP_SETTING_UP, 0u); /* the rows decide (rows_follow) */
        }
        break;
    case UI_ZB_OP_REPLACE:
        close_window(p, now);
        op_step(p, UI_ZB_STEP_SETTING_UP, 0u); /* a candidate still setting up decides */
        break;
    case UI_ZB_OP_REMOVE:
    case UI_ZB_OP_PING:
        op_done(p, UI_ZB_RES_NO_ANSWER);
        break;
    case UI_ZB_OP_SIREN_TEST:
        siren_test_end(p, UI_ZB_RES_NO_ANSWER); /* its end never came */
        break;
    default:
        op_done(p, UI_ZB_RES_FAILED);
        break;
    }
}

/* A listening (mains) siren is heard only through what it sends, and its setup asks it for
 * nothing more often than its battery report (12 h): read one attribute of it each third of its
 * silence limit it has been quiet, so a quiet, working siren never turns into the bell trouble
 * (ZB-023). Its REP_DONE OK counts as heard (zbz_panel_on_report). A sleepy one can't be read:
 * its own check-ins count. When the next read is due; UINT64_MAX none. */
static uint64_t wd_ping_due(const zbz_panel_t *p, size_t k)
{
    const zbw_siren_t *s = &p->wd.siren[k];
    if (!s->a.used || s->a.sleepy || s->fresh || !radio_ok(p) || !p->wd.radio_up ||
        p->radio.read_attributes == NULL) {
        return UINT64_MAX;
    }
    const uint64_t since = s->heard_at > p->wd_ping_at[k] ? s->heard_at : p->wd_ping_at[k];
    return add_ms(since, zbw_silent_limit_ms(&s->a) / 3u);
}

static void wd_keep_heard(zbz_panel_t *p, uint64_t now)
{
    for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
        if (now < wd_ping_due(p, k)) {
            continue;
        }
        const zbw_siren_t *s = &p->wd.siren[k];
        const zbz_device_t *d = zbz_reg_find(&p->reg, s->a.ieee);
        if (d == NULL) {
            continue;
        }
        static const uint16_t zcl_version[] = {0x0000u}; /* Basic's ZCL version, as a ping */
        p->wd_ping_at[k] = now;
        (void)p->radio.read_attributes(p->radio.ctx, s->a.ieee, basic_ep(d), ZSRC_ZB_ZCL_BASIC,
                                       zcl_version, 1u, new_tag(p));
    }
}

void zbz_panel_poll(zbz_panel_t *p, uint64_t now)
{
    p->now = now;
    describe_all(p, now);
    if (op_running(p) && p->deadline != 0u && now >= p->deadline) {
        op_timeout(p, now);
    }
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        zbz_device_t *d = &p->reg.dev[i];
        zbz_live_t *l = &p->live[i];
        if (!d->used || !setup_active(d)) {
            continue;
        }
        if (now >= l->setup_since && now - l->setup_since >= ZBZ_SETUP_MAX_MS) {
            setup_stop(p, i, now); /* still waiting after 30 min (a sleepy device asleep) */
            continue;
        }
        if (l->step_deadline == 0u || now < l->step_deadline) {
            continue;
        }
        l->step_deadline = 0u;
        if (l->tag != 0u) {
            setup_done(p, i, (uint8_t)ZSRC_ZB_DONE_TIMEOUT, now); /* no DONE came */
        } else if (l->asleep) {
            l->asleep = false; /* no frame in ZBZ_WAKE_WAIT_MS: the step again anyway */
            d->setup = (uint8_t)ZBZ_SETUP_RUNNING;
            setup_go(p, i, now);
        } else {
            setup_go(p, i, now);
        }
    }
    wd_keep_heard(p, now);
    zbw_poll(&p->wd, now); /* warnings asked again, retries, silence */
    zba_poll(&p->ace, now); /* posts nobody answered, the retransmission memory */
    op_follow(p);
}

uint64_t zbz_panel_next_deadline(const zbz_panel_t *p)
{
    if (p->describe_owed && p->src != NULL && p->src->link_up) {
        return 0u;
    }
    uint64_t t = UINT64_MAX;
    if (op_running(p) && p->deadline != 0u) {
        t = p->deadline;
    }
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        const zbz_device_t *d = &p->reg.dev[i];
        if (!d->used || !setup_active(d)) {
            continue;
        }
        if (p->live[i].step_deadline != 0u) {
            t = min_u64(t, p->live[i].step_deadline);
        }
        t = min_u64(t, add_ms(p->live[i].setup_since, ZBZ_SETUP_MAX_MS));
    }
    t = min_u64(t, zbw_next_deadline(&p->wd));
    t = min_u64(t, zba_next_deadline(&p->ace));
    for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
        t = min_u64(t, wd_ping_due(p, k));
    }
    return t;
}

/* ---- requests ------------------------------------------------------------------------------------ */

static ui_zb_req_result_t code_result(zbz_code_result_t r)
{
    switch (r) {
    case ZBZ_CODE_OK: return UI_ZB_REQ_OK;
    case ZBZ_CODE_CHECKSUM: return UI_ZB_REQ_CHECKSUM;
    case ZBZ_CODE_BAD_ADDRESS: return UI_ZB_REQ_BAD_ADDRESS;
    case ZBZ_CODE_FULL: return UI_ZB_REQ_FULL;
    case ZBZ_CODE_NOT_FOUND: return UI_ZB_REQ_NOT_FOUND;
    case ZBZ_CODE_BAD_LENGTH:
    default: return UI_ZB_REQ_BAD_CODE;
    }
}

/* A typed code with the address on its label, or a scanned QR code's text (`ieee` 0). */
static ui_zb_req_result_t parse_code(const ui_zb_request_t *req, uint64_t *ieee,
                                     uint8_t code[ZBZ_CODE_MAX], uint8_t *len)
{
    char text[UI_ZB_CODE_TEXT];
    copy_text(text, sizeof(text), req->code);
    zbz_code_result_t r;
    if (req->ieee == 0u) {
        r = zbz_code_parse_qr(text, ieee, code, len);
    } else if (req->ieee == UINT64_MAX) {
        r = ZBZ_CODE_BAD_ADDRESS;
    } else {
        *ieee = req->ieee;
        r = zbz_code_parse(text, code, len);
    }
    wipe(text, sizeof(text));
    return code_result(r);
}

bool zbz_panel_needs_allowed(uint8_t kind)
{
    return kind != (uint8_t)UI_ZB_REQ_PING && kind != (uint8_t)UI_ZB_REQ_IDENTIFY &&
           kind != (uint8_t)UI_ZB_REQ_SURVEY && kind != (uint8_t)UI_ZB_REQ_DISMISS;
}

/* The lowest engine device number neither a Zigbee keypad nor the rest of the panel holds. */
static uint8_t free_device(const zbz_panel_t *p)
{
    uint32_t taken = zbz_panel_devices_held(p);
    if (p->port.devices_taken != NULL) {
        taken |= p->port.devices_taken(p->port.ctx);
    }
    for (unsigned n = ZBA_DEVICE_FIRST; n <= (unsigned)ZBA_DEVICE_LAST; n++) {
        if ((taken & (1u << (n - 1u))) == 0u) {
            return (uint8_t)n;
        }
    }
    return 0u;
}

static const zba_keypad_t *keypad_of(const zbz_panel_t *p, uint16_t id)
{
    for (size_t k = 0; k < ZBA_KEYPADS_MAX; k++) {
        if (p->ace.keypad[k].used && p->ace.keypad[k].id == id) {
            return &p->ace.keypad[k];
        }
    }
    return NULL;
}

static bool keypad_assigned(const zbz_panel_t *p, uint16_t id)
{
    return keypad_of(p, id) != NULL;
}

static const zba_fob_t *fob_of(const zbz_panel_t *p, uint16_t id)
{
    for (size_t k = 0; k < ZBA_FOBS_MAX; k++) {
        if (p->ace.fob[k].used && p->ace.fob[k].id == id) {
            return &p->ace.fob[k];
        }
    }
    return NULL;
}

/* Its first HA endpoint with `bit` among its output (client) clusters: a keypad's IAS ACE. */
static uint8_t ep_with_out(const zbz_device_t *d, uint16_t bit)
{
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        if (d->ep[k].profile == HA_PROFILE && (d->ep[k].out_clusters & bit) != 0u) {
            return d->ep[k].endpoint;
        }
    }
    return 0u;
}

/* ZB01: what may take a siren assignment and its test - a siren, or a sensor with an IAS WD
 * server of its own (a smoke detector's sounder, alarm-devices.md §2), which keeps its inputs and
 * its role. A device whose sounder speaks only a manufacturer cluster (0xEF00) has no IAS WD. */
static bool siren_capable(const zbz_device_t *d)
{
    item_t it = {0u, 0u};
    return d->role == (uint8_t)ZBZ_ROLE_SIREN ||
           (d->role == (uint8_t)ZBZ_ROLE_SENSOR && ep_with(d, ZSRC_ZB_CL_IAS_WD, 0u, &it));
}

static uint16_t model_flags_of(const zbz_device_t *d)
{
    const zbz_model_t *m = zbz_model_for(d->manufacturer, d->model);
    return m != NULL ? m->flags : 0u;
}

/* SIREN (page 14, ZB-023): its settings; no partition clears it. */
static ui_zb_req_result_t req_siren(zbz_panel_t *p, const ui_zb_request_t *req,
                                    const zbz_device_t *d)
{
    const ui_zb_siren_t *in = &req->siren;
    if (in->partitions == 0u) {
        if (zbw_clear(&p->wd, d->id) == ZBW_OK) {
            notify(p);
        }
        return UI_ZB_REQ_OK;
    }
    item_t it = {0u, 0u};
    if (!ep_with(d, ZSRC_ZB_CL_IAS_WD, 0u, &it)) {
        return UI_ZB_REQ_INVALID;
    }
    const zbz_model_t *m = zbz_model_for(d->manufacturer, d->model);
    zbw_assign_t a;
    memset(&a, 0, sizeof(a));
    a.id = d->id;
    a.ieee = d->ieee;
    a.endpoint = it.ep;
    a.partitions = in->partitions;
    a.burglary = in->burglary;
    a.fire = in->fire;
    a.strobe = in->strobe;
    a.squawk = in->squawk;
    a.volume = in->volume;
    a.supervision_min = in->supervision_min;
    a.model_flags = m != NULL ? m->flags : 0u;
    a.max_duration_s = m != NULL ? m->max_duration_s : 0u;
    a.max_read = d->wd_max_read; /* ZB03: as its setup read it */
    a.max_read_s = d->wd_max_read ? d->wd_max_s : 0u;
    a.sleepy = zbz_dev_sleepy(d);
    switch (zbw_assign(&p->wd, &a)) {
    case ZBW_OK:
        notify(p);
        return UI_ZB_REQ_OK;
    case ZBW_ERR_FULL:
        return UI_ZB_REQ_FULL;
    default:
        return UI_ZB_REQ_INVALID; /* a partition past the panel's, a volume, a supervision time */
    }
}

/* SIREN_TEST: ZBW_TEST_S of its sound, followed as an operation; its end is the page's
 * last_test (alarm-devices.md §2, "Wired in zbz_panel"). */
static ui_zb_req_result_t req_siren_test(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                         const zbz_device_t *d)
{
    if (op_running(p)) {
        return UI_ZB_REQ_BUSY;
    }
    if (!radio_ok(p)) {
        return UI_ZB_REQ_NO_RADIO;
    }
    if (zbw_find(&p->wd, d->id) == NULL) {
        return UI_ZB_REQ_INVALID; /* not one of the panel's sirens yet */
    }
    const zbw_result_t r = zbw_test(&p->wd, now, d->id);
    if (r == ZBW_ERR_ARMED) {
        return UI_ZB_REQ_NOT_ALLOWED; /* one of its partitions is armed */
    }
    if (r == ZBW_ERR_BUSY) {
        return UI_ZB_REQ_BUSY; /* it gives a warning, or its last command isn't confirmed */
    }
    if (r != ZBW_OK && r != ZBW_ERR_SEND) {
        return UI_ZB_REQ_INVALID;
    }
    op_reset(p, UI_ZB_OP_SIREN_TEST, req->user, req->via);
    op_capture(p, d);
    p->test_id = d->id;
    if (r == ZBW_ERR_SEND) {
        siren_test_end(p, UI_ZB_RES_FAILED); /* not even queued */
        return UI_ZB_REQ_OK;
    }
    p->op_tag = zbw_find(&p->wd, d->id)->tag; /* the test's own ('W'): its REP_DONE ends it */
    op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_SIREN_TEST_MS));
    return UI_ZB_REQ_OK;
}

/* KEYPAD (page 15, ZB-024): its settings, its device number given here; no partition clears it
 * (its number is free again). */
static ui_zb_req_result_t req_keypad(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                     const zbz_device_t *d)
{
    const ui_zb_keypad_t *in = &req->keypad;
    if (in->partition == 0u) {
        if (zba_clear(&p->ace, d->id) == ZBA_OK) {
            notify(p);
        }
        return UI_ZB_REQ_OK;
    }
    if ((in->panic_mask & (1u << (unsigned)ALARM_PANIC_SILENT)) != 0u) {
        return UI_ZB_REQ_INVALID; /* a keypad's buttons have no silent panic (alarm-devices.md §3) */
    }
    const uint8_t ep = ep_with_out(d, ZSRC_ZB_CL_IAS_ACE);
    if (ep == 0u) {
        return UI_ZB_REQ_INVALID;
    }
    const zba_keypad_t *was = keypad_of(p, d->id);
    zba_keypad_t k;
    memset(&k, 0, sizeof(k));
    k.id = d->id;
    k.ieee = d->ieee;
    k.endpoint = ep;
    k.partition = in->partition;
    k.panic_mask = in->panic_mask;
    k.show_state = in->show_state;
    k.device = was != NULL ? was->device : free_device(p); /* never the request's */
    k.model_flags = model_flags_of(d);
    if (k.device == 0u) {
        return UI_ZB_REQ_FULL; /* UI_ZB_FIT_NO_DEVICE_SLOT */
    }
    switch (zba_assign_keypad(&p->ace, &k)) {
    case ZBA_OK:
        (void)zba_greet(&p->ace, now, d->id); /* its partition's state at once */
        notify(p);
        return UI_ZB_REQ_OK;
    case ZBA_ERR_FULL:
        return UI_ZB_REQ_FULL;
    case ZBA_ERR_IN_USE:
        return UI_ZB_REQ_IN_USE;
    default:
        return UI_ZB_REQ_INVALID; /* a partition past the panel's, an unknown panic */
    }
}

/* FOB (page 16, ZB-024): whose it is, its partition, disarm and panic (both off unless chosen);
 * no user clears it. One fob for each user: another's is IN_USE. */
static ui_zb_req_result_t req_fob(zbz_panel_t *p, const ui_zb_request_t *req, const zbz_device_t *d)
{
    const ui_zb_fob_t *in = &req->fob;
    if (in->user == 0u) {
        if (zba_clear(&p->ace, d->id) == ZBA_OK) {
            notify(p);
        }
        return UI_ZB_REQ_OK;
    }
    const uint8_t ep = ep_with_out(d, ZSRC_ZB_CL_IAS_ACE);
    if (ep == 0u) {
        return UI_ZB_REQ_INVALID;
    }
    zba_fob_t f;
    memset(&f, 0, sizeof(f));
    f.id = d->id;
    f.ieee = d->ieee;
    f.endpoint = ep;
    f.user = in->user;
    f.partition = in->partition;
    f.may_disarm = in->may_disarm;
    f.panic = in->panic;
    f.model_flags = model_flags_of(d);
    switch (zba_assign_fob(&p->ace, &f)) {
    case ZBA_OK:
        notify(p);
        return UI_ZB_REQ_OK;
    case ZBA_ERR_FULL:
        return UI_ZB_REQ_FULL;
    case ZBA_ERR_IN_USE:
        return UI_ZB_REQ_IN_USE; /* that user has a fob */
    default:
        return UI_ZB_REQ_INVALID; /* no partition, a user past the panel's */
    }
}

static ui_zb_req_result_t req_window(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req)
{
    if (op_running(p)) {
        return UI_ZB_REQ_BUSY;
    }
    if (!radio_ok(p)) {
        return UI_ZB_REQ_NO_RADIO;
    }
    if (storage_full(p) || p->reg.read_only || zbz_reg_count(&p->reg) >= ZBZ_DEV_MAX) {
        return UI_ZB_REQ_FULL;
    }
    if (p->radio.permit_join == NULL) {
        return UI_ZB_REQ_UNSUPPORTED;
    }
    uint64_t ieee = 0u;
    if (req->kind == (uint8_t)UI_ZB_REQ_ADD_CODE) {
        if (p->radio.install_code == NULL || p->radio.codes_only == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        uint8_t code[ZBZ_CODE_MAX];
        uint8_t len = 0u;
        ui_zb_req_result_t r = parse_code(req, &ieee, code, &len);
        if (r == UI_ZB_REQ_OK && zbz_codes_find(&p->codes, ieee) == NULL &&
            p->codes.count >= ZBZ_CODES_MAX) {
            r = UI_ZB_REQ_FULL;
        }
        if (r == UI_ZB_REQ_OK) {
            if (!p->radio.install_code(p->radio.ctx, ieee, code, len)) {
                r = UI_ZB_REQ_NO_RADIO;
            } else {
                r = code_result(zbz_codes_add(&p->codes, ieee, code, len, wall(p)));
            }
        }
        wipe(code, sizeof(code));
        if (r != UI_ZB_REQ_OK) {
            return r;
        }
        if (!p->radio.codes_only(p->radio.ctx, true)) {
            return UI_ZB_REQ_NO_RADIO; /* the code stays waiting: "Add a device" takes it */
        }
    } else if (req->kind == (uint8_t)UI_ZB_REQ_REPLACE) {
        if (zbz_reg_cget(&p->reg, req->id) == NULL || req->id == 0u) {
            return UI_ZB_REQ_NOT_FOUND;
        }
    }
    if (!open_window(p, now)) {
        if (req->kind == (uint8_t)UI_ZB_REQ_ADD_CODE) {
            (void)p->radio.codes_only(p->radio.ctx, false);
        }
        return UI_ZB_REQ_NO_RADIO;
    }
    const ui_zb_op_t op = req->kind == (uint8_t)UI_ZB_REQ_ADD        ? UI_ZB_OP_ADD
                          : req->kind == (uint8_t)UI_ZB_REQ_ADD_CODE ? UI_ZB_OP_ADD_CODE
                                                                     : UI_ZB_OP_REPLACE;
    op_reset(p, op, req->user, req->via);
    p->op_ieee = ieee;
    if (op == UI_ZB_OP_REPLACE) {
        p->replacing = req->id;
        p->progress.old_id = req->id;
    }
    op_step(p, UI_ZB_STEP_LISTENING, add_ms(now, (uint64_t)ZSRC_ZB_LEARN_S * 1000u));
    return UI_ZB_REQ_OK;
}

/* An operation on one device: started with its tag, or failed at once. */
static ui_zb_req_result_t req_device_op(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                        zbz_device_t *d)
{
    const zsrc_zb_radio_t *rd = &p->radio;
    switch ((ui_zb_req_kind_t)req->kind) {
    case UI_ZB_REQ_REMOVE: {
        if (rd->leave == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        op_reset(p, UI_ZB_OP_REMOVE, req->user, req->via);
        op_capture(p, d);
        p->op_tag = new_tag(p);
        if (!rd->leave(rd->ctx, d->ieee, d->nwk, p->op_tag)) {
            op_done(p, UI_ZB_RES_FAILED);
        } else {
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_LEAVE_MS)); /* "Asked it to leave" */
        }
        return UI_ZB_REQ_OK;
    }
    case UI_ZB_REQ_PING: {
        if (rd->read_attributes == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        op_reset(p, UI_ZB_OP_PING, req->user, req->via);
        op_capture(p, d);
        if (zbz_dev_sleepy(d)) {
            p->op_wait_frame = true; /* it answers only when it wakes: its next frame */
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_WAKE_WAIT_MS));
            return UI_ZB_REQ_OK;
        }
        static const uint16_t zcl_version[] = {0x0000u};
        p->op_tag = new_tag(p);
        if (!rd->read_attributes(rd->ctx, d->ieee, basic_ep(d), ZSRC_ZB_ZCL_BASIC, zcl_version, 1u,
                                 p->op_tag)) {
            op_done(p, UI_ZB_RES_NO_ANSWER);
        } else {
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_PING_MS));
        }
        return UI_ZB_REQ_OK;
    }
    case UI_ZB_REQ_IDENTIFY: {
        if (rd->identify == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        item_t it = {0u, 0u};
        if (!ep_with(d, ZSRC_ZB_CL_IDENTIFY, 0u, &it)) {
            return UI_ZB_REQ_INVALID; /* it has no Identify cluster: nothing to blink */
        }
        op_reset(p, UI_ZB_OP_IDENTIFY, req->user, req->via);
        op_capture(p, d);
        p->op_tag = new_tag(p);
        if (!rd->identify(rd->ctx, d->ieee, it.ep, (uint16_t)ZBZ_IDENTIFY_S, p->op_tag)) {
            op_done(p, UI_ZB_RES_FAILED);
        } else {
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_PING_MS));
        }
        return UI_ZB_REQ_OK;
    }
    case UI_ZB_REQ_SETUP: {
        op_reset(p, UI_ZB_OP_SETUP, req->user, req->via);
        op_capture(p, d);
        const bool finished = d->setup == (uint8_t)ZBZ_SETUP_DONE ||
                              d->setup == (uint8_t)ZBZ_SETUP_NOT_USABLE;
        op_step(p, UI_ZB_STEP_READING, 0u);
        setup_start(p, slot_of(p, d),
                    finished ? (uint8_t)ZSRC_ZB_STEP_NODE_DESC : d->setup_step, now);
        op_follow(p);
        return UI_ZB_REQ_OK;
    }
    default:
        return UI_ZB_REQ_INVALID;
    }
}

ui_zb_req_result_t zbz_panel_request(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                     bool allowed)
{
    p->now = now;
    if (req == NULL) {
        return UI_ZB_REQ_INVALID;
    }
    if (zbz_panel_needs_allowed(req->kind) && !allowed) {
        return UI_ZB_REQ_NOT_ALLOWED;
    }
    zbz_device_t *d = req->id != 0u ? zbz_reg_get(&p->reg, req->id) : NULL;
    ui_zb_req_result_t r = UI_ZB_REQ_OK;
    switch ((ui_zb_req_kind_t)req->kind) {
    case UI_ZB_REQ_ADD:
    case UI_ZB_REQ_ADD_CODE:
    case UI_ZB_REQ_REPLACE:
        if (req->kind == (uint8_t)UI_ZB_REQ_REPLACE && d == NULL) {
            return UI_ZB_REQ_NOT_FOUND;
        }
        return req_window(p, now, req);
    case UI_ZB_REQ_KEEP_LISTENING:
        if (!op_listening(p) || !open_window(p, now)) {
            return UI_ZB_REQ_INVALID;
        }
        op_step(p, UI_ZB_STEP_LISTENING, add_ms(now, (uint64_t)ZSRC_ZB_LEARN_S * 1000u));
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_STOP:
        if (!op_running(p) ||
            (p->progress.op != (uint8_t)UI_ZB_OP_ADD && p->progress.op != (uint8_t)UI_ZB_OP_ADD_CODE &&
             p->progress.op != (uint8_t)UI_ZB_OP_REPLACE)) {
            return UI_ZB_REQ_INVALID;
        }
        close_window(p, now);
        op_done(p, UI_ZB_RES_STOPPED); /* setups still running go on */
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_REMOVE:
    case UI_ZB_REQ_PING:
    case UI_ZB_REQ_IDENTIFY:
    case UI_ZB_REQ_SETUP:
        if (d == NULL) {
            return UI_ZB_REQ_NOT_FOUND;
        }
        if (op_running(p)) {
            return UI_ZB_REQ_BUSY;
        }
        if (!radio_ok(p)) {
            return UI_ZB_REQ_NO_RADIO;
        }
        return req_device_op(p, now, req, d);
    case UI_ZB_REQ_FORGET:
        if (d == NULL) {
            return UI_ZB_REQ_NOT_FOUND;
        }
        if (op_running(p)) {
            return UI_ZB_REQ_BUSY;
        }
        /* No radio needed: the record goes whatever the radio can still do (devices.md §6). */
        op_reset(p, UI_ZB_OP_FORGET, req->user, req->via);
        op_capture(p, d);
        log_ev(p, ZBZ_EV_FORGOTTEN, first_zone(p, d->ieee), d->id, 0u, req->user, req->via);
        drop_device(p, slot_of(p, d), now);
        op_done(p, UI_ZB_RES_FORGOTTEN);
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_RENAME: {
        if (d == NULL) {
            return UI_ZB_REQ_NOT_FOUND;
        }
        if (memchr(req->name, '\0', sizeof(req->name)) == NULL) {
            return UI_ZB_REQ_INVALID;
        }
        if (p->reg.read_only) {
            return UI_ZB_REQ_UNSUPPORTED; /* a later firmware's records: never written over */
        }
        if (strcmp(d->name, req->name) != 0) {
            copy_text(d->name, sizeof(d->name), req->name);
            zbz_reg_touch(&p->reg, d);
            notify(p);
        }
        return UI_ZB_REQ_OK;
    }
    case UI_ZB_REQ_CODE_ADD: {
        if (storage_full(p)) {
            return UI_ZB_REQ_FULL;
        }
        uint64_t ieee = 0u;
        uint8_t code[ZBZ_CODE_MAX];
        uint8_t len = 0u;
        r = parse_code(req, &ieee, code, &len);
        if (r == UI_ZB_REQ_OK) {
            r = code_result(zbz_codes_add(&p->codes, ieee, code, len, wall(p)));
        }
        if (r == UI_ZB_REQ_OK && radio_ok(p) && p->radio.install_code != NULL) {
            (void)p->radio.install_code(p->radio.ctx, ieee, code, len); /* else at its start */
        }
        wipe(code, sizeof(code));
        if (r == UI_ZB_REQ_OK) {
            notify(p);
        }
        return r;
    }
    case UI_ZB_REQ_CODE_FORGET:
        r = code_result(zbz_codes_forget(&p->codes, req->ieee));
        if (r == UI_ZB_REQ_OK) {
            if (radio_ok(p) && p->radio.install_code != NULL) {
                (void)p->radio.install_code(p->radio.ctx, req->ieee, NULL, 0u);
            }
            notify(p);
        }
        return r;
    case UI_ZB_REQ_CHANNEL: {
        const uint8_t ch = req->channel;
        if (ch != 11u && ch != 15u && ch != 20u && ch != 25u) {
            return UI_ZB_REQ_INVALID;
        }
        if (op_running(p)) {
            return UI_ZB_REQ_BUSY;
        }
        if (!radio_ok(p)) {
            return UI_ZB_REQ_NO_RADIO;
        }
        if (p->radio.set_channel == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        op_reset(p, UI_ZB_OP_CHANNEL, req->user, req->via);
        p->op_channel = ch;
        if (p->network_known && p->channel == ch) {
            p->cfg.channel_set = true; /* already there: kept as the installer's choice */
            p->cfg.channel = ch;
            p->dirty |= ZBZ_DIRTY_CFG;
            op_done(p, UI_ZB_RES_DONE);
            return UI_ZB_REQ_OK;
        }
        p->op_tag = new_tag(p);
        if (!p->radio.set_channel(p->radio.ctx, ch, p->op_tag)) {
            op_done(p, UI_ZB_RES_FAILED);
        } else {
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_CHANNEL_MS));
        }
        return UI_ZB_REQ_OK;
    }
    case UI_ZB_REQ_SURVEY:
        if (op_running(p) || (p->src != NULL && p->src->learning)) {
            return UI_ZB_REQ_BUSY; /* not while devices may join */
        }
        if (!radio_ok(p)) {
            return UI_ZB_REQ_NO_RADIO;
        }
        if (p->radio.channel_survey == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        op_reset(p, UI_ZB_OP_SURVEY, req->user, req->via);
        p->op_tag = new_tag(p);
        if (!p->radio.channel_survey(p->radio.ctx, p->op_tag)) {
            op_done(p, UI_ZB_RES_FAILED);
        } else {
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_CHANNEL_MS));
        }
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_RESET:
        /* The installer code was checked by the caller (zbz_reset_code_result) and never
         * reaches this. */
        if (op_running(p)) {
            return UI_ZB_REQ_BUSY;
        }
        if (!radio_ok(p)) {
            return UI_ZB_REQ_NO_RADIO;
        }
        if (p->radio.reset_network == NULL) {
            return UI_ZB_REQ_UNSUPPORTED;
        }
        op_reset(p, UI_ZB_OP_RESET, req->user, req->via);
        p->op_tag = new_tag(p);
        if (!p->radio.reset_network(p->radio.ctx, p->op_tag)) {
            op_done(p, UI_ZB_RES_FAILED);
        } else {
            op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_RESET_MS));
        }
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_RESTART:
    case UI_ZB_REQ_RETRY:
        if (req->kind == (uint8_t)UI_ZB_REQ_RETRY &&
            p->info.state != (uint8_t)UI_ZB_RADIO_PAUSED) {
            return UI_ZB_REQ_INVALID;
        }
        if (op_running(p)) {
            return UI_ZB_REQ_BUSY;
        }
        /* The service restarts the stack on this OK; the operation ends when the radio works
         * again (zbz_panel_radio) or after ZBZ_RESET_MS. */
        op_reset(p, UI_ZB_OP_RESTART, req->user, req->via);
        p->restart_down = !radio_ok(p);
        op_step(p, UI_ZB_STEP_WORKING, add_ms(now, ZBZ_RESET_MS));
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_DISMISS:
        if (op_running(p)) {
            return UI_ZB_REQ_INVALID;
        }
        op_reset(p, UI_ZB_OP_NONE, 0u, UI_ZB_VIA_PANEL);
        notify(p);
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_SIREN:
    case UI_ZB_REQ_SIREN_TEST:
    case UI_ZB_REQ_KEYPAD:
    case UI_ZB_REQ_FOB: {
        if (d == NULL) {
            return UI_ZB_REQ_NOT_FOUND;
        }
        const uint8_t want = req->kind == (uint8_t)UI_ZB_REQ_KEYPAD ? (uint8_t)ZBZ_ROLE_KEYPAD
                             : req->kind == (uint8_t)UI_ZB_REQ_FOB  ? (uint8_t)ZBZ_ROLE_KEYFOB
                                                                    : (uint8_t)ZBZ_ROLE_SIREN;
        const bool siren_req = want == (uint8_t)ZBZ_ROLE_SIREN;
        if ((siren_req ? !siren_capable(d) : d->role != want) ||
            d->setup != (uint8_t)ZBZ_SETUP_DONE) {
            return UI_ZB_REQ_INVALID; /* not that kind of device, or not set up yet */
        }
        if (req->kind == (uint8_t)UI_ZB_REQ_SIREN_TEST && op_running(p)) {
            return UI_ZB_REQ_BUSY;
        }
        if (req->kind == (uint8_t)UI_ZB_REQ_KEYPAD && req->keypad.partition != 0u &&
            !keypad_assigned(p, d->id) && free_device(p) == 0u) {
            return UI_ZB_REQ_FULL; /* UI_ZB_FIT_NO_DEVICE_SLOT */
        }
        switch ((ui_zb_req_kind_t)req->kind) {
        case UI_ZB_REQ_SIREN: return req_siren(p, req, d);
        case UI_ZB_REQ_SIREN_TEST: return req_siren_test(p, now, req, d);
        case UI_ZB_REQ_KEYPAD: return req_keypad(p, now, req, d);
        default: return req_fob(p, req, d);
        }
    }
    default:
        return UI_ZB_REQ_INVALID;
    }
}

/* ---- the binding gate (ZB21, RF32) --------------------------------------------------------------- */

typedef enum { GATE_GO, GATE_ARMED, GATE_BUSY } gate_t;

/* Holding no lock: the reservation (a busy gate asked again, bounded), then the lock and the
 * claim. Returns with the lock held whatever the answer; GATE_GO: claimed - end it before the
 * lock goes. A refused claim (armed meanwhile, or its time up) is ended here; a claim the
 * alarm task's step keeps answering RETRY lets the lock go for each pause. */
static gate_t gate_enter(const zbz_gate_port_t *g)
{
    zone_bind_reserve_t r = ZONE_BIND_BUSY;
    for (unsigned t = 0u; t < ZBZ_GATE_TRIES && r == ZONE_BIND_BUSY; t++) {
        if (t > 0u) {
            g->pause(g->ctx);
        }
        r = g->reserve(g->ctx);
    }
    g->lock(g->ctx);
    if (r != ZONE_BIND_RESERVED) {
        return r == ZONE_BIND_ARMED ? GATE_ARMED : GATE_BUSY; /* nothing reserved: no end */
    }
    for (unsigned n = 0u;; n++) {
        const zone_bind_claim_t c =
            n < ZBZ_GATE_PAUSES ? g->claim(g->ctx) : ZONE_BIND_CLAIM_REFUSED;
        if (c == ZONE_BIND_CLAIMED) {
            return GATE_GO;
        }
        if (c == ZONE_BIND_CLAIM_REFUSED) {
            g->end(g->ctx);
            return GATE_ARMED;
        }
        g->unlock(g->ctx); /* RETRY: an engine step runs - never wait under the lock */
        g->pause(g->ctx);
        g->lock(g->ctx);
    }
}

ui_zb_req_result_t zbz_panel_request_gated(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                           bool allowed, const zbz_gate_port_t *gate)
{
    if (req == NULL || gate == NULL) {
        return UI_ZB_REQ_INVALID;
    }
    const bool gated = allowed && zbz_panel_needs_allowed(req->kind);
    gate_t at = GATE_GO;
    if (gated) {
        at = gate_enter(gate);
    } else {
        gate->lock(gate->ctx);
    }
    if (at == GATE_BUSY) {
        gate->unlock(gate->ctx);
        return UI_ZB_REQ_BUSY; /* another change holds the gate: nothing done, ask again */
    }
    /* Armed since the snapshot: without `allowed`, so NOT_ALLOWED before any radio hook. */
    const ui_zb_req_result_t r = zbz_panel_request(p, now, req, allowed && at == GATE_GO);
    if (gate->after != NULL) {
        gate->after(gate->ctx, req, r);
    }
    if (gated && at == GATE_GO) {
        gate->end(gate->ctx); /* arming goes again from the alarm task's next step */
    }
    gate->unlock(gate->ctx);
    return r;
}

bool zbz_panel_replace_waiting(const zbz_panel_t *p)
{
    return p->replace_with != 0u && op_is(p, UI_ZB_OP_REPLACE);
}

/* Under the lock: the waiting replacement, claimed (granted) or refused. */
static void replace_go(zbz_panel_t *p, uint64_t now, bool granted)
{
    const uint16_t id = p->replace_with;
    p->replace_with = 0u;
    zbz_device_t *nd = id != 0u ? zbz_reg_get(&p->reg, id) : NULL;
    if (!op_is(p, UI_ZB_OP_REPLACE) || nd == NULL || nd->id == p->replacing ||
        nd->role != (uint8_t)ZBZ_ROLE_SENSOR) {
        return; /* stopped or timed out meanwhile, or the device went: nothing to move */
    }
    if (!granted) {
        /* Armed meanwhile (or the gate stayed busy): as when the hub refuses - nothing moved,
         * the old device keeps its zones, the new one stays added. */
        close_window(p, now);
        op_done(p, UI_ZB_RES_FAILED);
        return;
    }
    try_replace(p, slot_of(p, nd), now, true);
}

void zbz_panel_replace_gated(zbz_panel_t *p, uint64_t now, const zbz_gate_port_t *gate)
{
    if (gate == NULL) {
        return;
    }
    gate->lock(gate->ctx);
    const bool waiting = zbz_panel_replace_waiting(p);
    gate->unlock(gate->ctx);
    if (!waiting) {
        return;
    }
    const gate_t at = gate_enter(gate);
    p->now = now;
    replace_go(p, now, at == GATE_GO);
    if (at == GATE_GO) {
        gate->end(gate->ctx); /* after every rebind, the put-back too; before the lock goes */
    }
    gate->unlock(gate->ctx);
}

ui_zb_req_result_t zbz_reset_code_result(alarm_result_t r, alarm_authority_t authority)
{
    if (r == ALARM_OK) {
        return authority == ALARM_AUTH_INSTALLER ? UI_ZB_REQ_OK : UI_ZB_REQ_WRONG_CODE;
    }
    return r == ALARM_ERR_LOCKED_OUT ? UI_ZB_REQ_LOCKED_OUT : UI_ZB_REQ_WRONG_CODE;
}

/* ---- device numbers and the NVS budget ----------------------------------------------------------- */

uint32_t zbz_panel_devices_held(const zbz_panel_t *p)
{
    uint32_t held = 0u;
    for (size_t k = 0; k < ZBA_KEYPADS_MAX; k++) {
        const zba_keypad_t *kp = &p->ace.keypad[k];
        const unsigned n = kp->device;
        if (kp->used && n >= ZBA_DEVICE_FIRST && n <= (unsigned)ZBA_DEVICE_LAST) {
            held |= 1u << (n - 1u);
        }
    }
    return held;
}

void zbz_panel_nvs_free(zbz_panel_t *p, uint32_t entries)
{
    const bool was = storage_full(p);
    p->nvs_free = entries;
    if (storage_full(p) != was) {
        notify(p);
    }
}

/* ---- the engine's news for sirens and keypads ---------------------------------------------------- */

void zbz_panel_output(zbz_panel_t *p, uint64_t now, uint8_t partition, alarm_output_t out,
                      alarm_pattern_t pattern)
{
    p->now = now;
    zbw_output(&p->wd, now, partition, out, pattern); /* the engine owns the timing */
}

static bool view_armed(const zba_view_t *v)
{
    return v->state != (uint8_t)ALARM_STATE_DISARMED; /* exit delay included */
}

void zbz_panel_partition(zbz_panel_t *p, uint64_t now, uint8_t partition, const zba_view_t *view)
{
    p->now = now;
    if (view == NULL || partition < 1u || partition > ALARM_MAX_PARTITIONS) {
        return;
    }
    const size_t i = (size_t)partition - 1u;
    const zba_view_t *o = &p->part_view[i];
    const bool known = p->part_known[i];
    if (!known) {
        zbw_set_armed(&p->wd, partition, view_armed(view)); /* start-up: no chirp */
    } else if (view_armed(view) != view_armed(o)) {
        zbw_armed(&p->wd, now, partition, view_armed(view)); /* armed or disarmed: the squawk */
    }
    const bool changed = !known || o->state != view->state || o->level != view->level ||
                         o->ready != view->ready || o->delay_s != view->delay_s ||
                         o->in_alarm != view->in_alarm || o->cause != view->cause;
    p->part_view[i] = *view;
    p->part_known[i] = true;
    if (changed) {
        zba_panel(&p->ace, now, partition, view); /* a sleepy keypad isn't woken for nothing */
    }
}

bool zbz_panel_ace_result(zbz_panel_t *p, uint64_t now, uint32_t ticket, alarm_result_t result,
                          const alarm_cmd_result_t *detail)
{
    p->now = now;
    return zba_result(&p->ace, now, ticket, result, detail);
}

void zbz_panel_walk_test(zbz_panel_t *p, uint64_t now, bool on)
{
    p->now = now;
    if (p->src != NULL) {
        zsrc_zb_walk_test(p->src, on, now); /* IAS test mode, in seconds (ZB-022) */
    }
}

static bool bm_has(const uint8_t *bm, uint16_t z)
{
    return bm != NULL && z >= 1u && z <= (uint16_t)ALARM_MAX_ZONES &&
           (bm[(z - 1u) / 8u] & (uint8_t)(1u << ((z - 1u) % 8u))) != 0u;
}

void zbz_walk_covered(const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                      uint8_t covered[ALARM_ZONE_BITMAP_BYTES])
{
    if (covered == NULL) {
        return;
    }
    memset(covered, 0, ALARM_ZONE_BITMAP_BYTES);
    if (cfg == NULL || snap == NULL) {
        return;
    }
    for (uint16_t z = 1u; z <= (uint16_t)ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = &cfg->zones[z - 1u];
        const unsigned part = zc->partition;
        if (zc->type == ALARM_ZT_NOT_USED || part < 1u || part > ALARM_MAX_PARTITIONS ||
            (snap->partitions & (1u << (part - 1u))) == 0u) {
            continue;
        }
        const alarm_partition_status_t *ps = &snap->part[part - 1u];
        if (ps->walk_test && alarm_zone_type_walk_counts(zc->type, ps->walk_flags)) {
            covered[(z - 1u) / 8u] = (uint8_t)(covered[(z - 1u) / 8u] | (1u << ((z - 1u) % 8u)));
        }
    }
}

/* What the zones bound to one watched input say about the walk test's scope. */
#define WALK_NONE    0u /* bound to no zone (yet): out */
#define WALK_COVERED 1u /* every zone so far is covered */
#define WALK_OUT     2u /* at least one is not: out, whatever the others say */

void zbz_panel_walk_scope(zbz_panel_t *p, uint64_t now,
                          const uint8_t covered[ALARM_ZONE_BITMAP_BYTES])
{
    p->now = now;
    if (p->src == NULL) {
        return;
    }
    uint8_t state[ZSRC_ZB_WATCH_MAX];
    memset(state, 0, sizeof(state));
    for (uint16_t z = 1u; p->port.binding != NULL && z <= (uint16_t)ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        const bool in = bm_has(covered, z);
        for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
            const zsrc_address_t *a = &b.in[r].addr;
            if (a->kind != ZSRC_ZIGBEE) {
                continue;
            }
            for (size_t w = 0; w < ZSRC_ZB_WATCH_MAX; w++) {
                const zsrc_zb_watch_t *wt = &p->src->watch[w];
                if (wt->used && wt->ieee == a->id && wt->endpoint == a->input) {
                    /* The main input decides; any other role only keeps it out. */
                    if (!in) {
                        state[w] = (uint8_t)WALK_OUT;
                    } else if (r == (size_t)ZSRC_ROLE_MAIN && state[w] == (uint8_t)WALK_NONE) {
                        state[w] = (uint8_t)WALK_COVERED;
                    }
                }
            }
        }
    }
    for (size_t w = 0; w < ZSRC_ZB_WATCH_MAX; w++) {
        const zsrc_zb_watch_t *wt = &p->src->watch[w];
        if (wt->used) {
            const zsrc_address_t a = zsrc_zb_address(wt->ieee, wt->endpoint);
            zsrc_zb_walk_test_input(p->src, &a, state[w] == (uint8_t)WALK_COVERED, now);
        }
    }
}

/* ---- the views ------------------------------------------------------------------------------------ */

uint8_t zbz_signal_of(uint8_t lqi)
{
    if (lqi == 0u) {
        return (uint8_t)UI_ZB_SIGNAL_UNKNOWN;
    }
    if (lqi >= ZBZ_LQI_GOOD) {
        return (uint8_t)UI_ZB_SIGNAL_GOOD;
    }
    return (uint8_t)(lqi >= ZBZ_LQI_FAIR ? UI_ZB_SIGNAL_FAIR : UI_ZB_SIGNAL_WEAK);
}

static bool mains(uint8_t power_source)
{
    const unsigned ps = (unsigned)power_source & 0x7Fu;
    return ps == 1u || ps == 2u || ps == 4u || ps == 5u || ps == 6u;
}

static uint8_t power_of(const zbz_device_t *d)
{
    if (mains(d->power_source)) {
        return (uint8_t)UI_ZB_POWER_MAINS;
    }
    if (((unsigned)d->power_source & 0x7Fu) == 3u) {
        return (uint8_t)UI_ZB_POWER_BATTERY;
    }
    if (d->mac_caps != 0u) {
        return (uint8_t)((d->mac_caps & ZSRC_ZB_MAC_MAINS) != 0u ? UI_ZB_POWER_MAINS
                                                                 : UI_ZB_POWER_BATTERY);
    }
    return (uint8_t)(d->node_type == (uint8_t)ZBZ_NODE_ROUTER ? UI_ZB_POWER_MAINS
                                                              : UI_ZB_POWER_UNKNOWN);
}

static uint8_t kind_of(uint8_t role)
{
    switch ((zbz_role_t)role) {
    case ZBZ_ROLE_SENSOR: return (uint8_t)UI_ZB_KIND_SENSOR;
    case ZBZ_ROLE_SIREN: return (uint8_t)UI_ZB_KIND_SIREN;
    case ZBZ_ROLE_KEYPAD: return (uint8_t)UI_ZB_KIND_KEYPAD;
    case ZBZ_ROLE_KEYFOB: return (uint8_t)UI_ZB_KIND_KEYFOB;
    case ZBZ_ROLE_REPEATER: return (uint8_t)UI_ZB_KIND_REPEATER;
    case ZBZ_ROLE_OTHER: return (uint8_t)UI_ZB_KIND_OTHER;
    default: return (uint8_t)UI_ZB_KIND_DEVICE;
    }
}

/* What the panel uses it as: its siren, keypad or fob assignment, else its role. */
static uint8_t used_as(const zbz_panel_t *p, uint16_t id, uint8_t role)
{
    /* A sensor with a siren assignment (ZB01, a smoke detector's sounder) stays a sensor: its
     * inputs are what its page and the list show. */
    if (role != (uint8_t)ZBZ_ROLE_SENSOR && zbw_find(&p->wd, id) != NULL) {
        return (uint8_t)ZBZ_ROLE_SIREN;
    }
    if (keypad_of(p, id) != NULL) {
        return (uint8_t)ZBZ_ROLE_KEYPAD;
    }
    if (fob_of(p, id) != NULL) {
        return (uint8_t)ZBZ_ROLE_KEYFOB;
    }
    return role;
}

static uint8_t state_of(const zbz_panel_t *p, size_t i)
{
    const zbz_device_t *d = &p->reg.dev[i];
    switch ((zbz_setup_t)d->setup) {
    case ZBZ_SETUP_NEW: return (uint8_t)UI_ZB_DEV_JOINING;
    case ZBZ_SETUP_RUNNING:
        return (uint8_t)(p->live[i].asleep ? UI_ZB_DEV_WAKE_NEEDED : UI_ZB_DEV_SETTING_UP);
    case ZBZ_SETUP_WAKE: return (uint8_t)UI_ZB_DEV_WAKE_NEEDED;
    case ZBZ_SETUP_STOPPED: return (uint8_t)UI_ZB_DEV_SETUP_STOPPED;
    case ZBZ_SETUP_NOT_USABLE: return (uint8_t)UI_ZB_DEV_NOT_USABLE;
    case ZBZ_SETUP_DONE:
    default: return (uint8_t)UI_ZB_DEV_READY;
    }
}

static void device_row(const zbz_panel_t *p, uint64_t now, size_t i, ui_zb_device_t *out)
{
    const zbz_device_t *d = &p->reg.dev[i];
    const zbz_live_t *l = &p->live[i];
    memset(out, 0, sizeof(*out));
    out->id = d->id;
    copy_text(out->name, sizeof(out->name), d->name);
    model_name(d, out->model, sizeof(out->model));
    out->state = state_of(p, i);
    /* What the panel uses it as: an assignment's kind, else its role. */
    out->kind = kind_of(used_as(p, d->id, d->role != 0u ? d->role : zbz_role_of(d)));
    out->power = power_of(d);
    out->router = zbz_dev_router(d);
    out->battery_pct = UI_ZB_BATTERY_UNKNOWN;
    out->join = d->join;
    out->device_class = (uint8_t)ALARM_DEVCLASS_GENERIC;
    const bool finished = d->setup == (uint8_t)ZBZ_SETUP_DONE ||
                          d->setup == (uint8_t)ZBZ_SETUP_NOT_USABLE;
    out->setup_total = SETUP_STEPS;
    out->setup_done = (uint8_t)(finished                ? SETUP_STEPS
                                : d->setup_step > 0u    ? d->setup_step - 1u
                                                        : 0u);
    uint64_t heard = l->heard_at;
    uint8_t lqi = l->lqi;
    bool any_slot = false, unreachable = false;
    for (uint8_t k = 0; p->src != NULL && k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        const zsrc_zb_device_t *s = zsrc_zb_device(p->src, d->ieee, d->ep[k].endpoint);
        if (s == NULL) {
            continue;
        }
        if (!any_slot) {
            out->device_class = s->device_class;
        }
        any_slot = true;
        unreachable = unreachable || s->unreachable;
        heard = s->heard_at > heard ? s->heard_at : heard;
        lqi = lqi == 0u ? s->lqi : lqi;
        if (out->battery_pct == UI_ZB_BATTERY_UNKNOWN && s->battery_pct != ZSRC_ZB_BATTERY_UNKNOWN) {
            out->battery_pct = s->battery_pct;
        }
        out->battery_low = out->battery_low || s->battery_low;
    }
    if (!any_slot && radio_ok(p)) {
        /* No zone input (a siren, a keypad, a repeater): silent past the source's window. */
        const uint64_t window =
            (uint64_t)(zbz_dev_sleepy(d) ? ZSRC_ZB_SUPERVISION_S : ZSRC_ZB_ROUTER_SUPERVISION_S) *
            1000u;
        const uint64_t since = heard > p->up_at ? heard : p->up_at;
        unreachable = since != 0u && now > since && now - since > window;
    }
    out->unreachable = unreachable;
    out->heard_s = heard == 0u ? UI_ZB_NEVER
                               : (uint32_t)(now > heard ? (now - heard) / 1000u : 0u);
    out->signal = zbz_signal_of(lqi);
}

static int text_cmp(const char *a, const char *b)
{
    for (size_t k = 0;; k++) {
        unsigned ca = (unsigned)(uint8_t)a[k], cb = (unsigned)(uint8_t)b[k];
        if (ca >= 'A' && ca <= 'Z') {
            ca += 32u;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 32u;
        }
        if (ca != cb || ca == 0u) {
            return ca < cb ? -1 : (ca > cb ? 1 : 0);
        }
    }
}

static const char *sort_name(const ui_zb_device_t *d)
{
    return d->name[0] != '\0' ? d->name : d->model;
}

/* Unreachable first, then by name (the installer's, else the model), then by id. */
static bool row_before(const ui_zb_device_t *a, const ui_zb_device_t *b)
{
    if (a->unreachable != b->unreachable) {
        return a->unreachable;
    }
    const int c = text_cmp(sort_name(a), sort_name(b));
    return c != 0 ? c < 0 : a->id < b->id;
}

size_t zbz_panel_devices(const zbz_panel_t *p, uint64_t now, ui_zb_device_t *out, size_t max)
{
    /* Every device is ranked, then the first `max` kept: a short out[] still begins with the
     * unreachable ones, wherever their slots are (devices.md section 9). Insertion into the
     * sorted rows: at most 64 devices. */
    size_t n = 0u;
    for (size_t i = 0; i < ZBZ_DEV_MAX && max > 0u; i++) {
        if (!p->reg.dev[i].used) {
            continue;
        }
        ui_zb_device_t row;
        device_row(p, now, i, &row);
        size_t j = n;
        if (n < max) {
            n++;
        } else if (row_before(&row, &out[n - 1u])) {
            j = n - 1u; /* the last row kept falls off */
        } else {
            continue;
        }
        for (; j > 0u && row_before(&row, &out[j - 1u]); j--) {
            out[j] = out[j - 1u];
        }
        out[j] = row;
    }
    /* the zones each feeds: every zone's binding read once */
    for (uint16_t z = 1u; p->port.binding != NULL && n > 0u && z <= (uint16_t)ALARM_MAX_ZONES;
         z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        for (size_t k = 0; k < n; k++) {
            const zbz_device_t *d = zbz_reg_cget(&p->reg, out[k].id);
            if (d != NULL && binding_has(&b, d->ieee) && out[k].zone_count < UI_ZB_ZONES_MAX) {
                out[k].zones[out[k].zone_count++] = z;
            }
        }
    }
    return n;
}

static const char *class_words(uint8_t cls)
{
    switch ((alarm_device_class_t)cls) {
    case ALARM_DEVCLASS_DOOR: return "door contact";
    case ALARM_DEVCLASS_WINDOW: return "window contact";
    case ALARM_DEVCLASS_GARAGE_DOOR: return "garage door";
    case ALARM_DEVCLASS_MOTION: return "motion";
    case ALARM_DEVCLASS_GLASS_BREAK: return "glass break";
    case ALARM_DEVCLASS_SMOKE: return "smoke";
    case ALARM_DEVCLASS_HEAT: return "heat";
    case ALARM_DEVCLASS_CO: return "carbon monoxide";
    case ALARM_DEVCLASS_GAS: return "gas";
    case ALARM_DEVCLASS_WATER: return "water";
    case ALARM_DEVCLASS_TEMPERATURE: return "temperature";
    case ALARM_DEVCLASS_VIBRATION: return "vibration";
    case ALARM_DEVCLASS_PANIC_BUTTON: return "panic button";
    case ALARM_DEVCLASS_KEYFOB: return "key fob";
    default: return "input";
    }
}

/* An input's state in the source's words (zsrc_zigbee.c state_words: "open", "21.50"). */
static void input_state(const zsrc_zb_device_t *s, char *out, size_t len)
{
    out[0] = '\0';
    if (s->device_class == (uint8_t)ALARM_DEVCLASS_TEMPERATURE && s->has_value) {
        const int32_t v = s->value_x100;
        const long whole = (long)(v / 100);
        const long frac = (long)(v % 100 < 0 ? -(v % 100) : v % 100);
        snprintf(out, len, "%s%ld.%02ld", (v < 0 && whole == 0) ? "-" : "", whole, frac);
        return;
    }
    bool active;
    if (s->status_known) {
        const uint16_t mask = (s->quirks & ZSRC_ZB_QUIRK_ALARM2) != 0u
                                  ? (uint16_t)ZSRC_ZB_IAS_ALARM2
                                  : (uint16_t)(ZSRC_ZB_IAS_ALARM1 | ZSRC_ZB_IAS_ALARM2);
        active = ((s->status & mask) != 0u) != ((s->quirks & ZSRC_ZB_QUIRK_INVERT) != 0u);
    } else if (s->on_known) {
        active = s->on;
    } else {
        return;
    }
    const char *on_word = "open", *off_word = "closed";
    switch ((alarm_device_class_t)s->device_class) {
    case ALARM_DEVCLASS_MOTION:
        on_word = "motion";
        off_word = "clear";
        break;
    case ALARM_DEVCLASS_WATER:
        on_word = "wet";
        off_word = "dry";
        break;
    case ALARM_DEVCLASS_SMOKE:
    case ALARM_DEVCLASS_HEAT:
    case ALARM_DEVCLASS_CO:
    case ALARM_DEVCLASS_GAS:
    case ALARM_DEVCLASS_GLASS_BREAK:
    case ALARM_DEVCLASS_VIBRATION:
        on_word = "alarm";
        off_word = "clear";
        break;
    case ALARM_DEVCLASS_PANIC_BUTTON:
    case ALARM_DEVCLASS_KEYFOB:
        on_word = "pressed";
        off_word = "idle";
        break;
    default:
        break;
    }
    copy_text(out, len, active ? on_word : off_word);
}

static uint16_t zone_of_input(const zbz_panel_t *p, uint64_t ieee, uint8_t ep)
{
    const zsrc_address_t a = zsrc_zb_address(ieee, ep);
    for (uint16_t z = 1u; p->port.binding != NULL && z <= (uint16_t)ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
            if (b.in[r].addr.kind == a.kind && b.in[r].addr.id == a.id &&
                b.in[r].addr.input == a.input) {
                return z;
            }
        }
    }
    return 0u;
}

static void fits(const zbz_panel_t *p, const zbz_device_t *d, ui_zb_device_page_t *out)
{
    const uint8_t role = d->role != 0u ? d->role : zbz_role_of(d);
    const bool ready = d->setup == (uint8_t)ZBZ_SETUP_DONE;
    const uint8_t fit = (uint8_t)(ready ? UI_ZB_FIT_AVAILABLE : UI_ZB_FIT_NOT_READY);
    if (role == (uint8_t)ZBZ_ROLE_SIREN) {
        out->siren.fit = fit;
        for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
            const zbw_assign_t *a = &p->wd.siren[k].a;
            if (a->used && a->id == d->id) {
                out->siren.partitions = a->partitions;
                out->siren.burglary = a->burglary;
                out->siren.fire = a->fire;
                out->siren.strobe = a->strobe;
                out->siren.squawk = a->squawk;
                out->siren.volume = a->volume;
                out->siren.supervision_min = a->supervision_min;
                out->siren.live = a->ieee == d->ieee;
            }
        }
        out->siren.device = 0u; /* a siren takes no device number (alarm-devices.md §4) */
        for (size_t k = 0; k < ZBW_SIRENS_MAX; k++) {
            if (p->siren_test[k].id == d->id) {
                out->siren.last_test = p->siren_test[k].result;
            }
        }
    } else if (role == (uint8_t)ZBZ_ROLE_KEYPAD) {
        out->keypad.fit = fit;
        bool assigned = false;
        for (size_t k = 0; k < ZBA_KEYPADS_MAX; k++) {
            const zba_keypad_t *kp = &p->ace.keypad[k];
            if (kp->used && kp->id == d->id) {
                assigned = true;
                out->keypad.partition = kp->partition;
                out->keypad.panic_mask = kp->panic_mask;
                out->keypad.show_state = kp->show_state;
                out->keypad.device = kp->device;
                out->keypad.live = kp->ieee == d->ieee;
            }
        }
        if (ready && !assigned && free_device(p) == 0u) {
            out->keypad.fit = (uint8_t)UI_ZB_FIT_NO_DEVICE_SLOT;
        }
    } else if (role == (uint8_t)ZBZ_ROLE_KEYFOB) {
        out->fob.fit = fit;
        for (size_t k = 0; k < ZBA_FOBS_MAX; k++) {
            const zba_fob_t *f = &p->ace.fob[k];
            if (f->used && f->id == d->id) {
                out->fob.user = f->user;
                out->fob.partition = f->partition;
                out->fob.may_disarm = f->may_disarm;
                out->fob.panic = f->panic;
                out->fob.live = f->ieee == d->ieee;
            }
        }
    }
}

bool zbz_panel_device(const zbz_panel_t *p, uint64_t now, uint16_t id, ui_zb_device_page_t *out)
{
    const zbz_device_t *d = id != 0u ? zbz_reg_cget(&p->reg, id) : NULL;
    if (d == NULL) {
        return false;
    }
    const size_t i = slot_of(p, d);
    const zbz_live_t *l = &p->live[i];
    memset(out, 0, sizeof(*out));
    device_row(p, now, i, &out->dev);
    out->dev.zone_count = (uint8_t)0u;
    const size_t nz = device_zones(p, d->ieee, out->dev.zones, UI_ZB_ZONES_MAX);
    out->dev.zone_count = (uint8_t)(nz < UI_ZB_ZONES_MAX ? nz : UI_ZB_ZONES_MAX);
    out->ieee = d->ieee;
    out->nwk = d->nwk;
    copy_text(out->manufacturer, sizeof(out->manufacturer), d->manufacturer);
    copy_text(out->model_id, sizeof(out->model_id), d->model);
    copy_text(out->sw_build, sizeof(out->sw_build), d->sw_build);
    out->lqi = l->lqi;
    out->rssi = l->rssi;
    out->joined_at = d->joined_at;
    out->sleepy = zbz_dev_sleepy(d);
    out->check_in_s = d->check_in_s;
    out->setup_step = d->setup_step;
    item_t it = {0u, 0u};
    out->can_identify = ep_with(d, ZSRC_ZB_CL_IDENTIFY, 0u, &it);
    const zbz_model_t *m = zbz_model_for(d->manufacturer, d->model);
    if (m != NULL) {
        copy_text(out->wake_words, sizeof(out->wake_words), m->wake_words);
        copy_text(out->add_words, sizeof(out->add_words), m->add_words);
    }
    size_t inputs = 0u;
    for (uint8_t k = 0; k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        if (ep_is_input(&d->ep[k])) {
            inputs++;
        }
    }
    for (uint8_t k = 0; p->src != NULL && k < d->ep_count && k < ZBZ_EP_MAX; k++) {
        const zsrc_zb_device_t *s = zsrc_zb_device(p->src, d->ieee, d->ep[k].endpoint);
        if (s == NULL) {
            continue;
        }
        if (s->note != NULL && out->note[0] == '\0') {
            copy_text(out->note, sizeof(out->note), s->note);
        }
        if (!ep_is_input(&d->ep[k]) || out->input_count >= UI_ZB_INPUTS_MAX) {
            continue;
        }
        ui_zb_input_t *in = &out->inputs[out->input_count++];
        in->addr = zsrc_zb_address(d->ieee, s->endpoint);
        in->endpoint = s->endpoint;
        in->device_class = s->device_class;
        if (inputs > 1u) {
            snprintf(in->words, sizeof(in->words), "endpoint %u \xC2\xB7 %s",
                     (unsigned)s->endpoint, class_words(s->device_class));
        } else {
            copy_text(in->words, sizeof(in->words), class_words(s->device_class));
        }
        input_state(s, in->state, sizeof(in->state));
        in->zone = zone_of_input(p, d->ieee, s->endpoint);
    }
    fits(p, d, out);
    return true;
}

void zbz_panel_status(const zbz_panel_t *p, uint64_t now, ui_zb_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->radio = p->info.state;
    out->paused_why = p->info.paused_why;
    out->jammed = p->info.jammed || (p->src != NULL && zsrc_zb_jammed(p->src));
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if (p->reg.dev[i].used) {
            out->devices++;
            ui_zb_device_t row;
            device_row(p, now, i, &row);
            if (row.unreachable) {
                out->not_heard++;
            }
        }
    }
    out->codes_waiting = (uint16_t)p->codes.count;
    if (op_listening(p) && p->deadline > now) {
        out->learn_s = (uint16_t)((p->deadline - now + 999u) / 1000u);
    } else {
        out->learn_s = p->info.learn_s;
    }
    out->network_known = p->network_known;
    out->channel = p->channel;
    out->pan_id = p->pan_id;
    out->ext_pan_id = p->ext_pan_id;
    out->panel_ieee = p->panel_ieee;
    copy_text(out->radio_version, sizeof(out->radio_version), p->info.version);
    out->noise_known = p->info.noise_known;
    out->noise_dbm = p->info.noise_dbm;
    static const uint8_t channels[UI_ZB_CHANNELS] = {11u, 15u, 20u, 25u};
    out->survey_known = p->survey_known;
    memcpy(out->survey_dbm, p->survey_dbm, sizeof(out->survey_dbm));
    int best = -1;
    for (size_t k = 0; k < UI_ZB_CHANNELS; k++) {
        if ((p->survey_known & (1u << k)) != 0u &&
            (best < 0 || p->survey_dbm[k] < p->survey_dbm[(size_t)best])) {
            best = (int)k;
        }
    }
    out->quietest = best < 0 ? 0u : channels[(size_t)best];
    out->restarts = p->info.restarts;
    out->storage_full = storage_full(p);
}

void zbz_panel_progress(const zbz_panel_t *p, uint64_t now, ui_zb_progress_t *out)
{
    *out = p->progress;
    out->seconds_left = 0u;
    if (op_running(p) && p->progress.step == (uint8_t)UI_ZB_STEP_LISTENING && p->deadline > now) {
        out->seconds_left = (uint32_t)((p->deadline - now + 999u) / 1000u); /* once a second */
    }
}

size_t zbz_panel_codes(const zbz_panel_t *p, ui_zb_code_t *out, size_t max)
{
    size_t n = 0u;
    for (size_t k = 0; k < p->codes.count && n < max; k++) {
        out[n].ieee = p->codes.codes[k].ieee; /* the code itself is never shown again */
        out[n].added_at = p->codes.codes[k].added_at;
        n++;
    }
    return n;
}

uint16_t zbz_panel_device_of_zone(const zbz_panel_t *p, uint16_t zone)
{
    zone_hub_binding_t b;
    if (p->port.binding == NULL || zone == 0u || !p->port.binding(p->port.ctx, zone, &b)) {
        return 0u;
    }
    for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
        for (size_t i = 0; b.in[r].addr.kind == (uint8_t)ZSRC_ZIGBEE && i < ZBZ_DEV_MAX; i++) {
            if (p->reg.dev[i].used && p->reg.dev[i].ieee == b.in[r].addr.id) {
                return p->reg.dev[i].id;
            }
        }
    }
    return 0u;
}

/* ---- what is stored and logged -------------------------------------------------------------------- */

uint8_t zbz_panel_dirty(const zbz_panel_t *p)
{
    uint8_t d = (uint8_t)(p->dirty & ZBZ_DIRTY_CFG);
    if (!p->reg.read_only && (p->reg.dirty != 0u || p->reg.meta_dirty)) {
        d |= ZBZ_DIRTY_REGISTRY;
    }
    if (p->codes.dirty) {
        d |= ZBZ_DIRTY_CODES;
    }
    if (p->wd.dirty) {
        d |= ZBZ_DIRTY_WD;
    }
    if (p->ace.dirty) {
        d |= ZBZ_DIRTY_ACE;
    }
    return d;
}

zbz_take_t zbz_panel_take_registry(zbz_panel_t *p, char key[ZBZ_KEY_LEN], uint8_t *buf,
                                   size_t cap, size_t *len)
{
    return zbz_reg_take(&p->reg, key, buf, cap, len);
}

size_t zbz_panel_take_codes(zbz_panel_t *p, uint8_t *buf, size_t cap)
{
    if (!p->codes.dirty || buf == NULL) {
        return 0u;
    }
    const size_t n = zbz_codes_encode(&p->codes, buf, cap);
    if (n > 0u) {
        p->codes.dirty = false;
    }
    return n;
}

size_t zbz_panel_take_cfg(zbz_panel_t *p, uint8_t *buf, size_t cap)
{
    if ((p->dirty & ZBZ_DIRTY_CFG) == 0u || buf == NULL) {
        return 0u;
    }
    const size_t n = zbz_cfg_encode(&p->cfg, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)(p->dirty & ~ZBZ_DIRTY_CFG);
    }
    return n;
}

size_t zbz_panel_take_wd(zbz_panel_t *p, uint8_t *buf, size_t cap)
{
    if (!p->wd.dirty || buf == NULL) {
        return 0u;
    }
    const size_t n = zbw_encode(&p->wd, buf, cap);
    if (n > 0u) {
        p->wd.dirty = false;
    }
    return n;
}

size_t zbz_panel_take_ace(zbz_panel_t *p, uint8_t *buf, size_t cap)
{
    if (!p->ace.dirty || buf == NULL) {
        return 0u;
    }
    const size_t n = zba_encode(&p->ace, buf, cap);
    if (n > 0u) {
        p->ace.dirty = false;
    }
    return n;
}

void zbz_panel_redo(zbz_panel_t *p, uint8_t dirty)
{
    if ((dirty & ZBZ_DIRTY_REGISTRY) != 0u && !p->reg.read_only) {
        /* Which key failed isn't said here: every record and meta again (zbz_reg_redo takes one
         * key when the owner knows it). */
        /* Every slot, used or not: the failed key may be the ERASE of a removed, forgotten or
         * replaced device's record, which must not come back after a restart. An unused slot's
         * erase of a key that is not there is harmless (the owner takes NOT_FOUND as done). */
        p->reg.dirty = UINT64_MAX >> (64u - ZBZ_DEV_MAX); /* ZBZ_DEV_MAX bits */
        p->reg.meta_dirty = true;
    }
    if ((dirty & ZBZ_DIRTY_CODES) != 0u) {
        p->codes.dirty = true;
    }
    if ((dirty & ZBZ_DIRTY_CFG) != 0u) {
        p->dirty |= ZBZ_DIRTY_CFG;
    }
    if ((dirty & ZBZ_DIRTY_WD) != 0u) {
        p->wd.dirty = true;
    }
    if ((dirty & ZBZ_DIRTY_ACE) != 0u) {
        p->ace.dirty = true;
    }
}

bool zbz_panel_take_log(zbz_panel_t *p, zbz_log_t *out)
{
    if (p->log_n == 0u || out == NULL) {
        return false;
    }
    *out = p->log[0];
    memmove(&p->log[0], &p->log[1], (p->log_n - 1u) * sizeof(p->log[0]));
    p->log_n--;
    return true;
}

/* cfg: compat (1) | flags (bit 0 a channel was chosen) | channel | 0 */
size_t zbz_cfg_encode(const zbz_cfg_t *c, uint8_t *out, size_t cap)
{
    if (c == NULL || out == NULL || cap < ZBZ_CFG_BLOB) {
        return 0u;
    }
    out[0] = (uint8_t)ZBZ_CFG_COMPAT;
    out[1] = (uint8_t)(c->channel_set ? 0x01u : 0x00u);
    out[2] = c->channel_set ? c->channel : 0u;
    out[3] = 0u;
    return ZBZ_CFG_BLOB;
}

bool zbz_cfg_decode(zbz_cfg_t *c, const uint8_t *buf, size_t len)
{
    if (c == NULL) {
        return false;
    }
    memset(c, 0, sizeof(*c));
    /* A later firmware may append bytes at this compat: read as far as known. */
    if (buf == NULL || len < ZBZ_CFG_BLOB || buf[0] == 0u || buf[0] > ZBZ_CFG_COMPAT) {
        return false;
    }
    if ((buf[1] & 0x01u) != 0u) {
        if (buf[2] < 11u || buf[2] > 26u) {
            return false; /* not a channel: damaged */
        }
        c->channel_set = true;
        c->channel = buf[2];
    }
    return true;
}
