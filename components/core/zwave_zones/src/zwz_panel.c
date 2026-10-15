/*
 * zwz_panel.c - the panel's Z-Wave: the network, the bridge, the pages' view and requests, the
 * log and what is stored. See zwz_panel.h.
 *
 * The pages' view and the progress of an operation follow the PC simulator's backend
 * (host/ui_preview/host_panel.c), which the UI preview walks through: the same zw_net news turned
 * into the same ui_zw_* answers.
 */
#include "zwave_zones/zwz_panel.h"

#include <stdio.h>
#include <string.h>

#include "zwave_proto/zw_cc_misc.h"
#include "zwave_proto/zw_cc_notification.h"
#include "zwave_proto/zw_gbl.h"
#include "zwave_proto/zw_dsk.h"
#include "zwave_proto/zw_qr.h"
#include "zwave_host/zw_region.h"

/* ui_backend's words for a region change are zwave_host's (ZW-045). */
_Static_assert((int)UI_ZW_REGION_SAME_BAND == (int)ZW_REGION_SAME_BAND &&
                   (int)UI_ZW_REGION_DROPS_LR == (int)ZW_REGION_DROPS_LR &&
                   (int)UI_ZW_REGION_BAND_CHANGE == (int)ZW_REGION_BAND_CHANGE,
               "ui_zw_region_change_t follows zw_region_change_t");

/* The kinds of the panel's own tags (bits 16..23). */
#define TAG_PING 0x01u
#define TAG_IDENTIFY 0x02u

/* ZW-024: a device that can blink its own light when asked - Indicator v3 or later with the node
 * identify indicator (0x50), awake to hear it now; its class, the one it takes Indicator at. */
static bool can_identify(const zw_node_record_t *rec)
{
    if (rec->interview_step < ZW_IV_STEP_DONE || zw_iv_sleeping(rec)) {
        return false;
    }
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == ZW_CC_INDICATOR && rec->ccs[i].version >= 3u) {
            return true;
        }
    }
    return false;
}

/* serial-api.md §6.8: the wake-up reasons logged as "restarted by itself" - a watchdog, the
 * emergency watchdog, a brown-out, unknown. A reset through its line, a power-up or a soft
 * reset is not the module failing. */
static bool restart_logged(uint8_t reason)
{
    return reason == 0x03u || reason == 0x08u || reason == 0x09u || reason == 0xFFu;
}

/* ---- helpers --------------------------------------------------------------------------------- */

static const zw_module_facts_t *facts(const zwz_panel_t *p)
{
    return zw_host_facts(&p->net.host);
}

static const zw_node_record_t *record(const zwz_panel_t *p, uint16_t node, uint16_t generation)
{
    const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, node);
    return rec != NULL && rec->generation == generation ? rec : NULL;
}

static uint32_t next_tag(zwz_panel_t *p, uint32_t kind)
{
    p->stamp = (uint16_t)((unsigned)p->stamp + 1u);
    return ZWZ_PANEL_TAG | (kind << 16) | (uint32_t)p->stamp;
}

static const zw_health_node_t *health_of(const zwz_panel_t *p, uint16_t node)
{
    const zw_health_t *h = &p->net.health;
    for (size_t i = 0; i < h->node_count && i < ZW_HEALTH_NODES_MAX; i++) {
        if (h->nodes[i].node_id == node) {
            return &h->nodes[i];
        }
    }
    return NULL;
}

/* A binding's input is this device's: its node and generation, on this network when it has one. */
static bool is_device(const zwz_panel_t *p, const zsrc_address_t *a, uint16_t node,
                      uint16_t generation)
{
    uint32_t home = 0u;
    uint16_t gen = 0u;
    uint16_t nd = 0u;
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    if (!zsrc_zw_address_parse(a, &home, &gen, &nd, &ep, &slot)) {
        return false;
    }
    const uint32_t own = facts(p)->home_id;
    return nd == node && gen == generation && (own == 0u || home == own);
}

/* The zones whose inputs (any role) are this device's: the first max of them in zones; how many
 * there are. */
static size_t device_zones(const zwz_panel_t *p, uint16_t node, uint16_t generation,
                           uint16_t *zones, size_t max)
{
    size_t n = 0u;
    if (p->port.binding == NULL) {
        return 0u;
    }
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        for (size_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
            if (is_device(p, &b.in[r].addr, node, generation)) {
                if (n < max) {
                    zones[n] = z;
                }
                n++;
                break;
            }
        }
    }
    return n;
}

/* The zone one input feeds, 0 none. */
static uint16_t input_zone(const zwz_panel_t *p, const zsrc_address_t *a)
{
    if (p->port.binding == NULL) {
        return 0u;
    }
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        for (size_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
            if (zsrc_address_equal(&b.in[r].addr, a)) {
                return z;
            }
        }
    }
    return 0u;
}

/* The zone source's device for this record, when it is the same device. */
static const zsrc_zw_device_t *source_device(const zwz_panel_t *p, uint16_t node,
                                             uint16_t generation)
{
    const zsrc_zw_device_t *d = zsrc_zw_find(p->src, node);
    return d != NULL && d->generation == generation ? d : NULL;
}

static void model_of(const zsrc_zw_device_t *d, char *out, size_t len)
{
    out[0] = '\0';
    if (d != NULL && d->quirk != NULL && d->quirk->name != NULL) {
        snprintf(out, len, "%s", d->quirk->name);
    }
}

/* ---- the log --------------------------------------------------------------------------------- */

static void log_raw(zwz_panel_t *p, alarm_event_type_t type, uint16_t zone, uint8_t arg,
                    uint8_t arg2, uint8_t user)
{
    if (p->log_n >= ZWZ_PANEL_LOG_QUEUE) {
        p->log_lost++; /* ZW-041: counted, shown by `zwave status` */
        return;
    }
    zwz_log_t *l = &p->log[p->log_n++];
    l->type = (uint8_t)type;
    l->zone = zone;
    l->arg = arg;
    l->arg2 = arg2;
    l->user = user;
}

/* A device's event: its node ID when it fits the event (Long Range's don't: 0). */
static void log_device(zwz_panel_t *p, alarm_event_type_t type, uint16_t zone, uint16_t node,
                       uint8_t arg2, uint8_t user)
{
    log_raw(p, type, zone, (uint8_t)(node <= 0xFFu ? (unsigned)node : 0u), arg2, user);
}

/* ZW-041: a module stuck in a watchdog or brown-out loop restarts every few seconds: the first
 * restart is logged at once, the rest held and logged at most once an hour - one event whose arg2
 * says how many restarts it stands for (the homeowner hears of each event). */
static bool restart_hour_up(const zwz_panel_t *p)
{
    return !p->restart_logged || p->now >= p->restart_logged_at + ZWZ_PANEL_RESTART_LOG_MS;
}

static void restart_log_held(zwz_panel_t *p)
{
    if (p->log_n >= ZWZ_PANEL_LOG_QUEUE) {
        return; /* the queue is full: kept and counted, logged at the next poll that has room */
    }
    log_raw(p, ALARM_EV_ZWAVE_RADIO_RESTARTED, 0u, p->restart_reason, p->restarts_held, 0u);
    p->restart_logged = true;
    p->restart_logged_at = p->now;
    p->restarts_held = 0u;
}

static void restart_log(zwz_panel_t *p, uint8_t reason)
{
    if (p->restarts_held < 0xFFu) {
        p->restarts_held++;
    }
    p->restart_reason = reason;
    if (restart_hour_up(p)) {
        restart_log_held(p);
    }
}

/* The restarts held back, once the hour since the last logged one is up (poll). */
static void restart_flush(zwz_panel_t *p)
{
    if (p->restarts_held != 0u && restart_hour_up(p)) {
        restart_log_held(p);
    }
}

/* ---- devices' own check-ins ------------------------------------------------------------------ */

static const zwz_checkin_t *find_checkin(const zwz_panel_t *p, uint16_t node, uint16_t generation)
{
    for (size_t i = 0; i < p->checkin_count; i++) {
        if (p->checkins[i].node == node && p->checkins[i].generation == generation) {
            return &p->checkins[i];
        }
    }
    return NULL;
}

static bool set_checkin(zwz_panel_t *p, uint16_t node, uint16_t generation, uint32_t seconds)
{
    zwz_checkin_t *c = (zwz_checkin_t *)find_checkin(p, node, generation);
    if (c == NULL) {
        if (p->checkin_count >= ZWZ_PANEL_CHECKINS) {
            return false;
        }
        c = &p->checkins[p->checkin_count++];
        c->node = node;
        c->generation = generation;
    }
    c->seconds = seconds;
    p->dirty |= ZWZ_DIRTY_CHECKINS;
    return true;
}

static void drop_checkin(zwz_panel_t *p, uint16_t node, uint16_t generation)
{
    for (size_t i = 0; i < p->checkin_count; i++) {
        if (p->checkins[i].node == node && p->checkins[i].generation == generation) {
            memmove(&p->checkins[i], &p->checkins[i + 1u],
                    (p->checkin_count - i - 1u) * sizeof p->checkins[0]);
            p->checkin_count--;
            p->dirty |= ZWZ_DIRTY_CHECKINS;
            return;
        }
    }
}

/* The device's check-in zw_net carries (chosen since the panel started, not ended yet), or NULL. */
static const zw_net_checkin_t *net_checkin(const zwz_panel_t *p, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_NET_CHECK_INS; i++) {
        if (p->net.checkins[i].node == node) {
            return &p->net.checkins[i];
        }
    }
    return NULL;
}

/* network.md §7: a device's own check-in ended (zw_net_set_check_in). Taken: its record has the
 * interval now, so the page's "changes when it next wakes up" is gone by itself; the wanted value
 * stays, for its later interviews. Not taken - it answered "fail", nothing, or the Set could not
 * go - it keeps the interval it had, and the choice is dropped: the page shows what the device
 * has and no change to come (a later interview sets new devices' check-in); nothing is logged. */
static void checkin_ended(zwz_panel_t *p, const zw_net_event_t *ev)
{
    const zwz_checkin_t *c = find_checkin(p, ev->node, ev->generation);
    if (!ev->took && c != NULL && c->seconds == ev->check_in_s) {
        drop_checkin(p, ev->node, ev->generation);
    }
}

/* network.md §7 after a restart of the panel: a check-in chosen on a device's page but not
 * confirmed yet lived in zw_net's memory only. At a check that finds the network ours, each stored
 * one its device's record lacks is set again - unless zw_net still carries it (only the module
 * restarted) - as the installer chose it then. A refusal keeps it stored: a device still being
 * interviewed gets it from its interview (n_check_in). */
static void reissue(zwz_panel_t *p, uint64_t now)
{
    /* backwards: a Set that cannot go at all ends within the call, and its end drops the entry */
    for (size_t i = p->checkin_count; i > 0u; i--) {
        const zwz_checkin_t c = p->checkins[i - 1u];
        const zw_node_record_t *rec = record(p, c.node, c.generation);
        if (rec != NULL && rec->wake_up_interval_seconds != c.seconds &&
            net_checkin(p, c.node) == NULL) {
            (void)zw_net_set_check_in(&p->net, now, true, c.node, c.seconds);
        }
    }
}

/* ---- the operation the pages follow ---------------------------------------------------------- */

static void op_reset(zwz_panel_t *p, uint8_t op)
{
    memset(&p->progress, 0, sizeof p->progress);
    p->progress.op = op;
    p->deadline = 0u;
    p->pending = UI_ZW_RES_NONE;
}

static void op_step(zwz_panel_t *p, uint8_t step, uint64_t deadline)
{
    p->progress.step = step;
    p->deadline = deadline;
}

static void op_done(zwz_panel_t *p, uint8_t result)
{
    p->progress.step = UI_ZW_STEP_DONE;
    p->progress.result = result;
    p->deadline = 0u;
}

static bool op_running(const zwz_panel_t *p, uint8_t op)
{
    return p->progress.op == op && p->progress.step != UI_ZW_STEP_DONE;
}

/* A device that leaves: its model and zones as they were, for the removal's page. */
static void op_capture(zwz_panel_t *p, uint16_t node, uint16_t generation)
{
    ui_zw_progress_t *g = &p->progress;
    g->node = node;
    g->generation = generation;
    model_of(source_device(p, node, generation), g->model, sizeof g->model);
    const size_t n = device_zones(p, node, generation, g->zones, UI_ZW_ZONES_MAX);
    g->zone_count = (uint8_t)(n < UI_ZW_ZONES_MAX ? n : UI_ZW_ZONES_MAX);
}

static uint8_t s2_failure(zw_incl_s2_failure_t f)
{
    switch (f) {
    case ZW_INCL_S2_WRONG_PIN: return UI_ZW_S2_WRONG_PIN;
    case ZW_INCL_S2_TIMEOUT: return UI_ZW_S2_TIMEOUT;
    case ZW_INCL_S2_CANCELLED: return UI_ZW_S2_CANCELLED;
    case ZW_INCL_S2_DEVICE_FAIL: return UI_ZW_S2_DEVICE_STOPPED;
    case ZW_INCL_S2_NO_INFO: return UI_ZW_S2_NO_INFO;
    default: return UI_ZW_S2_FAILED;
    }
}

/* An added device's join, as the log keeps it. */
static void log_join(zwz_panel_t *p, const zw_incl_event_t *ev)
{
    alarm_event_type_t type = ALARM_EV_ZWAVE_ADDED;
    uint8_t join = ALARM_ZW_JOIN_NOT_SECURE;
    switch (ev->security) {
    case ZW_INCL_SEC_S2_UNAUTH: join = ALARM_ZW_JOIN_S2_UNAUTH; break;
    case ZW_INCL_SEC_S2_AUTH: join = ALARM_ZW_JOIN_S2_AUTH; break;
    case ZW_INCL_SEC_S2_ACCESS: join = ALARM_ZW_JOIN_S2_ACCESS; break;
    case ZW_INCL_SEC_FAILED:
        type = ALARM_EV_ZWAVE_ADDED_INSECURE;
        join = ALARM_ZW_JOIN_FAILED;
        break;
    case ZW_INCL_SEC_DOWNGRADED:
        type = ALARM_EV_ZWAVE_ADDED_INSECURE;
        join = ALARM_ZW_JOIN_DOWNGRADED;
        break;
    default:
        type = ALARM_EV_ZWAVE_ADDED_INSECURE;
        join = ALARM_ZW_JOIN_NOT_SECURE;
        break;
    }
    log_device(p, type, 0u, ev->node, join, p->op_user);
}

/* §11: the route repair's numbers, as its pages show them. */
static void routes_counts(zwz_panel_t *p)
{
    ui_zw_progress_t *g = &p->progress;
    zw_routes_counts(zw_net_routes(&p->net), &g->routes_done, &g->routes_total, &g->routes_failed,
                     &g->routes_skipped, &g->routes_waiting);
    /* BL-152: who is waiting, failed or skipped, for the pages to name */
    const zw_routes_t *r = zw_net_routes(&p->net);
    g->routes_list_n = 0u;
    for (uint8_t what = UI_ZW_ROUTE_WAITING; what <= UI_ZW_ROUTE_NO_ROUTE; what++) {
        for (size_t i = 0; i < r->n && g->routes_list_n < UI_ZW_ROUTES_LIST; i++) {
            const zw_routes_node_t *n = &r->nodes[i];
            const bool hit = what == UI_ZW_ROUTE_WAITING   ? n->state == ZW_ROUTES_ASLEEP
                             : what == UI_ZW_ROUTE_FAILED  ? n->state == ZW_ROUTES_FAILED
                             : what == UI_ZW_ROUTE_SKIPPED ? n->state == ZW_ROUTES_SKIPPED
                                                           : n->state == ZW_ROUTES_DONE &&
                                                                 n->route_failed;
            if (hit) {
                g->routes_list[g->routes_list_n].node = n->node;
                g->routes_list[g->routes_list_n].what = what;
                g->routes_list_n++;
            }
        }
    }
}

static void progress_routes(zwz_panel_t *p, const zw_net_event_t *ev)
{
    if (!op_running(p, UI_ZW_OP_ROUTES)) {
        return;
    }
    routes_counts(p);
    if (ev->routes.kind == ZW_ROUTES_EV_END) {
        p->progress.routes_stopped = ev->routes.stopped;
        p->progress.node = p->routes_one;
        op_done(p, ev->routes.stopped                     ? UI_ZW_RES_STOPPED
                   : ev->routes.done == ev->routes.skipped + ev->routes.failed &&
                             ev->routes.total > 0u
                       ? UI_ZW_RES_FAILED
                       : UI_ZW_RES_DONE);
    } else if (p->routes_one == 0u) {
        p->progress.node = ev->routes.state == ZW_ROUTES_RUNNING ? ev->node : 0u;
    }
}

/* The inclusion's news (network.md §4), as the add pages follow it. */
static void progress_incl(zwz_panel_t *p, const zw_incl_event_t *ev)
{
    ui_zw_progress_t *g = &p->progress;
    if (ev->kind == ZW_INCL_EV_LISTENING && g->op != UI_ZW_OP_ADD) {
        op_reset(p, UI_ZW_OP_ADD);
    }
    if (!op_running(p, UI_ZW_OP_ADD)) {
        return;
    }
    switch (ev->kind) {
    case ZW_INCL_EV_LISTENING: op_step(p, UI_ZW_STEP_LISTENING, ev->deadline); break;
    case ZW_INCL_EV_FOUND: op_step(p, UI_ZW_STEP_CONNECTING, 0u); break;
    case ZW_INCL_EV_ADDING:
        g->node = ev->node;
        op_step(p, UI_ZW_STEP_CONNECTING, 0u);
        break;
    case ZW_INCL_EV_JOINED: {
        g->node = ev->node;
        const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, ev->node);
        g->generation = (uint16_t)(rec != NULL ? (unsigned)rec->generation : 0u);
        op_step(p, UI_ZW_STEP_SECURING, 0u);
        break;
    }
    case ZW_INCL_EV_GRANT:
        g->requested = ev->requested;
        g->csa = (p->net.incl.report.flags & ZW_S2_KEX_CSA) != 0u;
        op_step(p, UI_ZW_STEP_ASK_ALLOW, ev->deadline);
        break;
    case ZW_INCL_EV_PIN:
        snprintf(g->dsk, sizeof g->dsk, "%s", ev->dsk);
        op_step(p, UI_ZW_STEP_ASK_PIN, ev->deadline);
        break;
    case ZW_INCL_EV_DONE:
        switch (ev->result) {
        case ZW_INCL_ADDED: {
            g->node = ev->node;
            const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, ev->node);
            if (rec != NULL) {
                g->generation = rec->generation;
            }
            if (ev->dsk[0] != '\0') {
                snprintf(g->dsk, sizeof g->dsk, "%s", ev->dsk);
            }
            switch (ev->security) {
            case ZW_INCL_SEC_S2_UNAUTH: g->security = UI_ZW_SEC_S2_UNAUTH; break;
            case ZW_INCL_SEC_S2_AUTH: g->security = UI_ZW_SEC_S2_AUTH; break;
            case ZW_INCL_SEC_S2_ACCESS: g->security = UI_ZW_SEC_S2_ACCESS; break;
            default:
                /* an empty node information and no answer to KEX Get: whether it runs without
                 * encryption is not known - not "Not encrypted" */
                g->security = ev->security == ZW_INCL_SEC_FAILED &&
                                      ev->failure == ZW_INCL_S2_NO_INFO
                                  ? UI_ZW_SEC_UNCONFIRMED
                                  : UI_ZW_SEC_NONE;
                break;
            }
            p->pending = ev->security == ZW_INCL_SEC_FAILED       ? UI_ZW_RES_ADDED_INSECURE
                         : ev->security == ZW_INCL_SEC_DOWNGRADED ? UI_ZW_RES_DOWNGRADED
                                                                  : UI_ZW_RES_ADDED;
            g->s2_failure = ev->security == ZW_INCL_SEC_FAILED ? s2_failure(ev->failure)
                                                               : UI_ZW_S2_OK;
            g->setup_total = ZW_IV_STEP_DONE;
            op_step(p, UI_ZW_STEP_READING, 0u); /* its interview: done at NODE_READY */
            break;
        }
        case ZW_INCL_NO_DEVICE: op_done(p, UI_ZW_RES_NOTHING_JOINED); break;
        case ZW_INCL_RADIO_RESTARTED: op_done(p, UI_ZW_RES_RADIO_RESTARTED); break;
        case ZW_INCL_STOPPED: op_done(p, UI_ZW_RES_STOPPED); break;
        case ZW_INCL_CONTROLLER: op_done(p, UI_ZW_RES_CONTROLLER); break;
        case ZW_INCL_FAILED:
        case ZW_INCL_TIMEOUT: op_done(p, UI_ZW_RES_JOIN_FAILED); break;
        case ZW_INCL_ALREADY_ADDED:
            g->node = ev->node;
            op_done(p, UI_ZW_RES_ALREADY_ADDED);
            break;
        /* §10 "Replace it": the old device answered (a ping, or the radio's report) */
        case ZW_INCL_STILL_ANSWERS: op_done(p, UI_ZW_RES_STILL_ANSWERS); break;
        /* the radio doesn't count it as failed yet (IsFailedNode, or the response's bit 08) */
        case ZW_INCL_NOT_FAILED: op_done(p, UI_ZW_RES_REFUSED); break;
        /* its other refusals (REFUSED) and "busy" (BUSY): it could not be done */
        default: op_done(p, UI_ZW_RES_FAILED); break;
        }
        break;
    default: break;
    }
}

/* The network's news, as the pages follow it: an added device's interview, removals. */
static void progress_net(zwz_panel_t *p, const zw_net_event_t *ev)
{
    ui_zw_progress_t *g = &p->progress;
    const bool reading = op_running(p, UI_ZW_OP_ADD) && g->step == UI_ZW_STEP_READING &&
                         ev->node == g->node;
    switch (ev->kind) {
    case ZW_NET_EV_NODE_READY:
        if (reading) {
            const bool silent =
                ev->inputs == ZW_IV_INPUTS_NO_LIFELINE || ev->inputs == ZW_IV_INPUTS_PULL;
            op_done(p, silent && p->pending == UI_ZW_RES_ADDED ? UI_ZW_RES_NO_REPORTS
                                                                : p->pending);
        }
        break;
    case ZW_NET_EV_NOT_USABLE:
        if (reading) {
            op_done(p, UI_ZW_RES_CONTROLLER);
        }
        break;
    case ZW_NET_EV_WAKE_NEEDED: /* only a sleeping device's interview waits for a wake-up */
        if (reading) {
            g->asleep = true;
        }
        break;
    case ZW_NET_EV_STALLED: /* its attempts ran out: it didn't answer - asleep or not */
        if (reading) {
            g->stalled = true;
        }
        break;
    case ZW_NET_EV_REMOVE: {
        const zw_remove_event_t *r = &ev->remove;
        const uint8_t op = r->how == ZW_REMOVE_BY_EXCLUSION ? UI_ZW_OP_REMOVE
                           : r->how == ZW_REMOVE_BY_FAILED  ? UI_ZW_OP_REMOVE_FAILED
                                                            : UI_ZW_OP_NONE;
        if (op == UI_ZW_OP_NONE) {
            break; /* a device that reset itself: nobody's operation */
        }
        if (r->kind == ZW_REMOVE_EV_LISTENING && g->op != op) {
            op_reset(p, op);
        }
        if (!op_running(p, op)) {
            break;
        }
        if (r->kind == ZW_REMOVE_EV_LISTENING) {
            op_step(p, UI_ZW_STEP_LISTENING, r->deadline);
        } else if (r->kind == ZW_REMOVE_EV_FOUND) {
            op_step(p, UI_ZW_STEP_CONNECTING, 0u);
        } else {
            switch (r->result) {
            case ZW_REMOVE_DONE:
                op_done(p, r->node != 0u ? UI_ZW_RES_REMOVED : UI_ZW_RES_REMOVED_FOREIGN);
                break;
            case ZW_REMOVE_NO_DEVICE: op_done(p, UI_ZW_RES_NOTHING_REMOVED); break;
            case ZW_REMOVE_STOPPED: op_done(p, UI_ZW_RES_STOPPED); break;
            case ZW_REMOVE_RADIO_RESTARTED: op_done(p, UI_ZW_RES_RADIO_RESTARTED); break;
            case ZW_REMOVE_STILL_ANSWERS: op_done(p, UI_ZW_RES_STILL_ANSWERS); break;
            case ZW_REMOVE_TIMEOUT: op_done(p, UI_ZW_RES_TIMED_OUT); break;
            case ZW_REMOVE_REFUSED:
            case ZW_REMOVE_NOT_FAILED: op_done(p, UI_ZW_RES_REFUSED); break;
            default: op_done(p, UI_ZW_RES_FAILED); break; /* status Failed, the radio went away */
            }
        }
        break;
    }
    default: break;
    }
}

/* ---- zw_net's port --------------------------------------------------------------------------- */

static bool n_write(void *ctx, const uint8_t *bytes, size_t len)
{
    zwz_panel_t *p = ctx;
    return p->port.write != NULL && p->port.write(p->port.ctx, bytes, len);
}

static void n_reset_line(void *ctx, bool asserted)
{
    zwz_panel_t *p = ctx;
    if (p->port.reset_line != NULL) {
        p->port.reset_line(p->port.ctx, asserted);
    }
}

/* Region, reset and restart end when the radio is up again (or is not). */
/* ---- a device's software update (ZW-033) ------------------------------------------------------ */

static bool lists_cc(const zw_node_record_t *rec, uint8_t cc);
/* ZW-040: Good / Fair / Weak from the newer of the last frame heard from a node and the last ACK
 * it sent. With the background noise known (the jam watch's quietest channel, network.md §9) the
 * rule is the margin over it - what a receiver needs is signal above noise: Good from 20 dB,
 * Fair from 8 dB, below that Weak. Without a noise reading, absolute RSSI: Good from -75 dBm,
 * Fair from -87 dBm (a quiet house's -95 dBm floor plus the same 20 and 8 dB). Z-Wave
 * receivers work down to about -100 dBm, so Weak is not "lost", only "little to spare". */
#define ZWZ_SIGNAL_GOOD_MARGIN_DB 20
#define ZWZ_SIGNAL_FAIR_MARGIN_DB 8
#define ZWZ_SIGNAL_GOOD_DBM       (-75)
#define ZWZ_SIGNAL_FAIR_DBM       (-87)

static uint8_t zwz_signal_of(int rssi_dbm, bool noise_known, int noise_dbm)
{
    if (noise_known) {
        const int margin = rssi_dbm - noise_dbm;
        if (margin >= ZWZ_SIGNAL_GOOD_MARGIN_DB) {
            return (uint8_t)UI_ZW_SIGNAL_GOOD;
        }
        return (uint8_t)(margin >= ZWZ_SIGNAL_FAIR_MARGIN_DB ? UI_ZW_SIGNAL_FAIR
                                                             : UI_ZW_SIGNAL_WEAK);
    }
    if (rssi_dbm >= ZWZ_SIGNAL_GOOD_DBM) {
        return (uint8_t)UI_ZW_SIGNAL_GOOD;
    }
    return (uint8_t)(rssi_dbm >= ZWZ_SIGNAL_FAIR_DBM ? UI_ZW_SIGNAL_FAIR : UI_ZW_SIGNAL_WEAK);
}

static uint8_t signal_view(const zwz_panel_t *p, uint16_t node)
{
    zw_net_signal_t s;
    if (!zw_net_node_signal(&p->net, node, &s)) {
        return (uint8_t)UI_ZW_SIGNAL_UNKNOWN;
    }
    const bool use_tx = s.has_tx && (!s.has_rx || s.tx_at_ms > s.rx_at_ms);
    const zw_jam_t *j = zw_net_jam(&p->net);
    const bool noise = j->has_reading && j->quietest != ZW_RSSI_NOT_AVAILABLE &&
                       j->quietest != ZW_RSSI_SATURATED && j->quietest != ZW_RSSI_NO_SIGNAL;
    return zwz_signal_of(use_tx ? s.tx_rssi : s.rx_rssi, noise, j->quietest);
}

static void device_row(const zwz_panel_t *p, uint64_t now, const zw_node_record_t *rec,
                       ui_zw_device_t *out);

/* ---- what the devices switch (ZW-034, ZW-035) ------------------------------------------------- */

#define OUT_CMD    0x01000000u /* the owner's command ids: kind | index << 16 | sequence */
#define GARAGE_CMD 0x02000000u

static bool out_send(void *ctx, uint16_t node, uint8_t endpoint, const uint8_t *cmd, size_t len,
                     uint32_t tag)
{
    zwz_panel_t *p = ctx;
    uint8_t buf[ZWZ_CMD_MAX];
    const uint8_t *c = cmd;
    size_t n = len;
    if (endpoint != 0u) {
        n = zw_cc_wrap_multi_channel(buf, sizeof buf, 0u, endpoint, false, cmd, len);
        c = buf;
        if (n == 0u) {
            return false;
        }
    }
    return zw_net_send(&p->net, p->now, node,
                       zw_s2_class_for_endpoint(&p->net.s2, node, endpoint, cmd[0]), c, n,
                       tag) == ZW_S2_OK;
}

static zwo_position_t out_position(void *ctx, uint16_t zone)
{
    zwz_panel_t *p = ctx;
    return p->port.zone_position != NULL ? p->port.zone_position(p->port.ctx, zone)
                                         : ZWO_POS_UNKNOWN;
}

static uint8_t out_words(zwo_result_t r)
{
    switch (r) {
    case ZWO_OK: return UI_ZW_OUT_DONE;
    case ZWO_ALREADY: return UI_ZW_OUT_ALREADY;
    case ZWO_ERR_TIMEOUT: return UI_ZW_OUT_NO_ANSWER;
    case ZWO_ERR_SEND:
    case ZWO_ERR_OFFLINE: return UI_ZW_OUT_SEND_FAILED;
    case ZWO_ERR_POSITION: return UI_ZW_OUT_NO_POSITION;
    case ZWO_ERR_NO_MOVE: return UI_ZW_OUT_NO_MOVE;
    case ZWO_ERR_RELAY_STUCK: return UI_ZW_OUT_STUCK;
    default: return UI_ZW_OUT_FAILED;
    }
}

static void out_result(void *ctx, uint32_t cmd, zwo_result_t r)
{
    zwz_panel_t *p = ctx;
    const size_t i = (cmd >> 16) & 0xFFu;
    if ((cmd & 0xFF000000u) == OUT_CMD && i < ZWO_OUTPUTS) {
        p->out_last[i] = out_words(r);
    } else if ((cmd & 0xFF000000u) == GARAGE_CMD && i < ZWO_GARAGES) {
        p->garage_last[i] = out_words(r);
        const zwo_garage_t *g = &p->outputs.garage[i];
        const zwo_output_t *u = &p->outputs.out[g->output];
        /* every pressed move's end follows its "asked" entry: done, or failed - the door didn't
         * get there, the relay won't let go, or the press itself unconfirmed (PL01: released, the
         * outcome unknown). A garage dropped with its device has no zone left to log. */
        if (g->used) {
            log_device(p, ALARM_EV_ZWAVE_GARAGE, g->zone, u->node,
                       r == ZWO_OK ? (g->goal == ZWO_POS_OPEN ? ALARM_ZW_GARAGE_OPENED
                                                              : ALARM_ZW_GARAGE_CLOSED)
                                   : ALARM_ZW_GARAGE_FAILED,
                       p->garage_user[i]);
        }
    }
    /* PL02: its caller's wait ends now, on the command's own end - once (the output layer ends
     * each command once, and a wait is taken by its own command id only) */
    for (size_t k = 0; k < ZWZ_OUT_WAITS; k++) {
        zwz_out_wait_t *w = &p->out_wait[k];
        if (w->cmd == cmd && !w->ended) {
            w->ended = true;
            w->result = (uint8_t)r;
            break;
        }
    }
}

static uint32_t out_cmd(zwz_panel_t *p, uint32_t kind, size_t index)
{
    p->out_seq = (uint16_t)((unsigned)p->out_seq + 1u);
    return kind | (uint32_t)index << 16 | (uint32_t)p->out_seq;
}

/* Every device whose setup is done: its outputs (at start, from the stored cache). */
static void out_scan_all(zwz_panel_t *p)
{
    for (size_t i = 0; i < zw_node_count(&p->net.nodes); i++) {
        const zw_node_record_t *rec = zw_node_at_const(&p->net.nodes, i);
        if (rec->interview_step >= ZW_IV_STEP_DONE) {
            (void)zwo_scan(&p->outputs, rec);
        }
    }
}

static int out_index(const zwz_panel_t *p, uint16_t node, uint16_t generation, uint8_t endpoint)
{
    const int i = zwo_find(&p->outputs, node, endpoint);
    return i >= 0 && p->outputs.out[i].generation == generation ? i : -1;
}

zwo_result_t zwz_panel_output_set(zwz_panel_t *p, uint64_t now, uint16_t node, uint16_t generation,
                                  uint8_t endpoint, int level, uint64_t issued_ms)
{
    p->now = now;
    const int i = out_index(p, node, generation, endpoint);
    if (i < 0) {
        return ZWO_ERR_UNKNOWN;
    }
    if (zw_host_link(&p->net.host) != ZW_LINK_UP) {
        return ZWO_ERR_OFFLINE;
    }
    const zwo_result_t r = zwo_set(&p->outputs, now, (size_t)i, level, issued_ms,
                                   out_cmd(p, OUT_CMD, (size_t)i));
    if (r == ZWO_OK) {
        p->out_last[i] = UI_ZW_OUT_NONE;
    }
    return r;
}

zwo_result_t zwz_panel_garage_move(zwz_panel_t *p, uint64_t now, uint16_t node,
                                   uint16_t generation, uint8_t endpoint, bool open,
                                   uint64_t issued_ms, uint8_t user)
{
    p->now = now;
    const int i = out_index(p, node, generation, endpoint);
    size_t slot = ZWO_GARAGES;
    for (size_t g = 0; i >= 0 && g < ZWO_GARAGES; g++) {
        if (p->outputs.garage[g].used && p->outputs.garage[g].output == (uint8_t)i) {
            slot = g;
        }
    }
    if (slot >= ZWO_GARAGES) {
        return ZWO_ERR_UNKNOWN;
    }
    if (zw_host_link(&p->net.host) != ZW_LINK_UP) {
        return ZWO_ERR_OFFLINE;
    }
    const zwo_result_t r =
        zwo_garage_move(&p->outputs, now, slot, open ? ZWO_POS_OPEN : ZWO_POS_CLOSED, issued_ms,
                        out_cmd(p, GARAGE_CMD, slot));
    if (r == ZWO_OK) {
        p->garage_last[slot] = UI_ZW_OUT_NONE;
        p->garage_user[slot] = user;
        log_device(p, ALARM_EV_ZWAVE_GARAGE, p->outputs.garage[slot].zone, node,
                   open ? ALARM_ZW_GARAGE_OPEN_ASKED : ALARM_ZW_GARAGE_CLOSE_ASKED, user);
    } else if (r == ZWO_ALREADY || r == ZWO_ERR_POSITION || r == ZWO_ERR_RELAY_STUCK) {
        p->garage_last[slot] = out_words(r); /* nothing pressed: the page says why */
    } else if (r == ZWO_ERR_BUSY) {
        p->garage_last[slot] = UI_ZW_OUT_STILL_MOVING; /* B015 UX-044: not "the radio is busy" */
    }
    return r;
}

static int garage_of(const zwz_panel_t *p, size_t output)
{
    for (size_t g = 0; g < ZWO_GARAGES; g++) {
        if (p->outputs.garage[g].used && p->outputs.garage[g].output == output) {
            return (int)g;
        }
    }
    return -1;
}

int zwz_panel_output_wait(zwz_panel_t *p, uint16_t node, uint16_t generation, uint8_t endpoint,
                          uint32_t request)
{
    const int i = out_index(p, node, generation, endpoint);
    if (i < 0 || request == 0u) {
        return -1;
    }
    const int g = garage_of(p, (size_t)i);
    const uint32_t cmd = g >= 0 ? p->outputs.garage[g].cmd : p->outputs.out[i].cmd;
    if (cmd == 0u) {
        return -1; /* nothing under way there: it ended already, or never started */
    }
    int free_slot = -1;
    for (size_t k = 0; k < ZWZ_OUT_WAITS; k++) {
        const zwz_out_wait_t *w = &p->out_wait[k];
        if (w->cmd == cmd) {
            return -1; /* someone waits for it already */
        }
        if (w->cmd == 0u && free_slot < 0) {
            free_slot = (int)k;
        }
    }
    if (free_slot >= 0) {
        zwz_out_wait_t *w = &p->out_wait[free_slot];
        memset(w, 0, sizeof(*w));
        w->cmd = cmd;
        w->request = request;
    }
    return free_slot;
}

bool zwz_panel_output_ended(zwz_panel_t *p, size_t *slot, uint32_t *request,
                            zwo_result_t *result)
{
    for (size_t k = 0; k < ZWZ_OUT_WAITS; k++) {
        zwz_out_wait_t *w = &p->out_wait[k];
        if (w->cmd != 0u && w->ended) {
            *slot = k;
            *request = w->request;
            *result = (zwo_result_t)w->result;
            memset(w, 0, sizeof(*w));
            return true;
        }
    }
    return false;
}

static const zsrc_zw_device_t *source_device(const zwz_panel_t *p, uint16_t node,
                                             uint16_t generation);
static void model_of(const zsrc_zw_device_t *d, char *out, size_t len);
static const zw_health_node_t *health_of(const zwz_panel_t *p, uint16_t node);

size_t zwz_panel_ha_devices(const zwz_panel_t *p, uint64_t now, proto_zw_device_t *out,
                            size_t max)
{
    (void)now;
    size_t n = 0u;
    const bool up = zw_host_link(&p->net.host) == ZW_LINK_UP;
    for (size_t i = 0; i < ZWO_OUTPUTS && n < max; i++) {
        const zwo_output_t *u = &p->outputs.out[i];
        if (!u->used) {
            continue;
        }
        proto_zw_device_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        if (!proto_zw_key(facts(p)->home_id, u->node, u->generation, u->endpoint, d->key)) {
            continue;
        }
        const int g = garage_of(p, i);
        d->kind = g >= 0 ? PROTO_ZW_GARAGE : u->kind == ZWO_DIMMER ? PROTO_ZW_LIGHT
                                                                   : PROTO_ZW_SWITCH;
        char model[UI_ZW_MODEL_LEN];
        model_of(source_device(p, u->node, u->generation), model, sizeof model);
        if (g >= 0) {
            snprintf(d->name, sizeof d->name, "Garage door");
        } else if (u->kind == ZWO_RELAY) {
            snprintf(d->name, sizeof d->name, "%.32s relay %u",
                     model[0] != '\0' ? model : "Z-Wave", (unsigned)u->endpoint);
        } else if (model[0] != '\0') {
            snprintf(d->name, sizeof d->name, "%.40s", model);
        } else {
            snprintf(d->name, sizeof d->name, "Z-Wave %s %u",
                     u->kind == ZWO_DIMMER ? "dimmer" : "switch", (unsigned)u->node);
        }
        d->level = u->level;
        const zw_health_node_t *h = health_of(p, u->node);
        d->online = up && (h == NULL || h->reachable);
        if (g >= 0) {
            snprintf(d->garage, sizeof d->garage, "%s", zwo_garage_state(&p->outputs, (size_t)g));
        }
        n++;
    }
    return n;
}

bool zwz_panel_ha_command(zwz_panel_t *p, uint64_t now, const char *key, int level,
                          uint64_t received_ms)
{
    for (size_t i = 0; key != NULL && i < ZWO_OUTPUTS; i++) {
        const zwo_output_t *u = &p->outputs.out[i];
        char k[PROTO_ZW_KEY_LEN];
        if (!u->used ||
            !proto_zw_key(facts(p)->home_id, u->node, u->generation, u->endpoint, k) ||
            strcmp(k, key) != 0) {
            continue;
        }
        if (garage_of(p, i) >= 0) {
            if (level != 0) {
                return false; /* opening takes a user's code at the panel (HCTL-005) */
            }
            const zwo_result_t r = zwz_panel_garage_move(p, now, u->node, u->generation,
                                                         u->endpoint, false, received_ms, 0u);
            return r == ZWO_OK || r == ZWO_ALREADY;
        }
        return zwz_panel_output_set(p, now, u->node, u->generation, u->endpoint, level,
                                    received_ms) == ZWO_OK;
    }
    return false;
}

bool zwz_panel_load_garage(zwz_panel_t *p, const uint8_t *blob, size_t len)
{
    out_scan_all(p);
    return zwo_garage_decode(&p->outputs, blob, len);
}

size_t zwz_panel_take_garage(zwz_panel_t *p, uint8_t *buf, size_t cap)
{
    if ((p->dirty & ZWZ_DIRTY_GARAGE) == 0u) {
        return 0u;
    }
    const size_t n = zwo_garage_encode(&p->outputs, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)((unsigned)p->dirty & ~ZWZ_DIRTY_GARAGE);
    }
    return n;
}

static bool fw_send(void *ctx, uint16_t node, const uint8_t *cmd, size_t len, uint32_t tag)
{
    zwz_panel_t *p = ctx;
    return zw_net_send(&p->net, p->now, node,
                       zw_s2_class_for(&p->net.s2, node, ZW_CC_FIRMWARE_UPDATE), cmd, len,
                       tag) == ZW_S2_OK;
}

static uint8_t fw_failure(const zwz_panel_t *p)
{
    const zw_fwupd_t *f = &p->fw;
    switch (f->result) {
    case ZW_FWUPD_NOT_UPGRADABLE: return UI_ZW_FWF_NOT_UPGRADABLE;
    case ZW_FWUPD_WRONG_DEVICE: return UI_ZW_FWF_WRONG_DEVICE;
    case ZW_FWUPD_REFUSED:
        return f->detail == ZW_FWU_REQ_BATTERY_LOW      ? UI_ZW_FWF_LOW_BATTERY
               : f->detail == ZW_FWU_REQ_NOT_UPGRADABLE ? UI_ZW_FWF_NOT_UPGRADABLE
               : f->detail == ZW_FWU_REQ_WRONG_ID       ? UI_ZW_FWF_WRONG_DEVICE
                                                        : UI_ZW_FWF_REFUSED;
    case ZW_FWUPD_DEVICE_FAILED:
        if (f->detail == ZW_FWU_STATUS_CHECKSUM || f->detail == ZW_FWU_STATUS_TRANSMISSION) {
            return UI_ZW_FWF_BAD_TRANSFER;
        }
        if (f->detail == ZW_FWU_STATUS_WRONG_MANUF || f->detail == ZW_FWU_STATUS_WRONG_ID ||
            f->detail == ZW_FWU_STATUS_WRONG_TARGET || f->detail == ZW_FWU_STATUS_WRONG_HARDWARE) {
            return UI_ZW_FWF_WRONG_DEVICE;
        }
        return UI_ZW_FWF_DEVICE_FAILED;
    case ZW_FWUPD_ACTIVATION_FAILED: return UI_ZW_FWF_DEVICE_FAILED;
    case ZW_FWUPD_TIMEOUT:
        return p->fw_sleeper && f->detail == ZW_FWUPD_MD && f->total == 0u ? UI_ZW_FWF_NOT_WOKEN
                                                                          : UI_ZW_FWF_NO_ANSWER;
    case ZW_FWUPD_SEND_FAILED: return UI_ZW_FWF_SEND_FAILED;
    case ZW_FWUPD_ABORTED: return p->fw_radio_lost ? UI_ZW_FWF_NO_RADIO : UI_ZW_FWF_STOPPED;
    default: return UI_ZW_FWF_DEVICE_FAILED;
    }
}

/* BL-152: how long after a successful update its setup is read again - the time the device said
 * it needs, plus a margin, as Z-Wave JS's getConservativeWaitTimeAfterFirmwareUpdate (devices
 * take longer than they say): none said 5 s, under 20 s +5, under 60 s +10, else +30. */
static uint64_t fw_reread_delay_ms(unsigned wait_s)
{
    const unsigned s = wait_s == 0u  ? 5u
                       : wait_s < 20u ? wait_s + 5u
                       : wait_s < 60u ? wait_s + 10u
                                      : wait_s + 30u;
    return (uint64_t)s * 1000u;
}

/* The update ended: the page's result, the log, the device let sleep, and its setup read again
 * once it restarted. */
static void fw_settle(zwz_panel_t *p)
{
    if (!op_running(p, UI_ZW_OP_FIRMWARE) || zw_fwupd_running(&p->fw)) {
        return;
    }
    ui_zw_progress_t *g = &p->progress;
    zw_net_hold_awake(&p->net, p->now, 0u);
    g->fw_done = p->fw.sent_max;
    g->fw_total = p->fw.total;
    g->fw_wake = false;
    const uint16_t zone = zwz_panel_zone_of(p, g->node, g->generation);
    if (p->fw.result == ZW_FWUPD_OK_RESTART || p->fw.result == ZW_FWUPD_OK_NO_RESTART) {
        g->fw_wait_s = (uint16_t)(p->fw.result == ZW_FWUPD_OK_RESTART ? (unsigned)p->fw.wait_s : 0u);
        g->fw_done = g->fw_total; /* its verdict can come before the last send's own end */
        op_done(p, UI_ZW_RES_FW_UPDATED);
        log_device(p, ALARM_EV_ZWAVE_SOFTWARE, zone, g->node, ALARM_ZW_SW_UPDATED, p->op_user);
        p->fw_reinterview = g->node;
        p->fw_reinterview_gen = g->generation;
        p->fw_reinterview_at = p->now + fw_reread_delay_ms(g->fw_wait_s);
    } else {
        g->fw_why = fw_failure(p);
        op_done(p, UI_ZW_RES_FW_FAILED);
        log_device(p, ALARM_EV_ZWAVE_SOFTWARE, zone, g->node, ALARM_ZW_SW_FAILED, p->op_user);
    }
}

static void fw_stop(zwz_panel_t *p, bool radio_lost)
{
    if (op_running(p, UI_ZW_OP_FIRMWARE)) {
        p->fw_radio_lost = radio_lost;
        zw_fwupd_abort(&p->fw, p->now);
        fw_settle(p);
    }
}

bool zwz_panel_fw_updatable(const zwz_panel_t *p, uint16_t node, uint16_t generation)
{
    const zw_node_record_t *rec = record(p, node, generation);
    return rec != NULL && rec->interview_step >= ZW_IV_STEP_DONE && rec->has_identity &&
           rec->has_versions && lists_cc(rec, ZW_CC_FIRMWARE_UPDATE);
}

ui_zw_req_result_t zwz_panel_fw_start(zwz_panel_t *p, uint64_t now, uint16_t node,
                                      uint16_t generation, uint8_t target, const uint8_t *image,
                                      size_t len, bool allowed, uint8_t user)
{
    p->now = now;
    if (!allowed) {
        return UI_ZW_REQ_NOT_ALLOWED;
    }
    const zw_node_record_t *rec = record(p, node, generation);
    if (!p->started || rec == NULL || !zwz_panel_fw_updatable(p, node, generation) ||
        image == NULL || len == 0u || (target > 0u && target > rec->fw_target_count)) {
        return UI_ZW_REQ_INVALID;
    }
    if ((p->progress.op != UI_ZW_OP_NONE && p->progress.step != UI_ZW_STEP_DONE) ||
        zw_incl_state(&p->net.incl) != ZW_INCL_IDLE ||
        zw_remove_state(&p->net.rm) != ZW_REMOVE_IDLE || zw_fwupd_running(&p->fw)) {
        return UI_ZW_REQ_BUSY;
    }
    if (zw_host_link(&p->net.host) != ZW_LINK_UP) {
        return UI_ZW_REQ_NO_RADIO;
    }
    ui_zw_device_t row;
    device_row(p, now, rec, &row);
    if (row.power == UI_ZW_POWER_BATTERY &&
        (row.battery_low ||
         (row.battery_pct != UI_ZW_BATTERY_UNKNOWN && row.battery_pct < ZWZ_FW_BATTERY_MIN))) {
        return UI_ZW_REQ_LOW_BATTERY;
    }
    uint8_t cc_version = 1u;
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == ZW_CC_FIRMWARE_UPDATE && rec->ccs[i].version != 0u) {
            cc_version = rec->ccs[i].version;
        }
    }
    /* a frame's command bytes, less Security 2 around it: at most the 64 the transfer sends */
    size_t payload = facts(p)->max_payload != 0u ? (size_t)facts(p)->max_payload : ZWZ_FW_PAYLOAD;
    if (zw_s2_class_for(&p->net.s2, node, ZW_CC_FIRMWARE_UPDATE) != ZW_SEC_NONE) {
        payload = payload > ZWZ_FW_SECURE_COST ? payload - ZWZ_FW_SECURE_COST : 0u;
    }
    payload = payload > 64u ? 64u : payload;
    p->fw_sleeper = zw_iv_sleeping(rec);
    p->fw_radio_lost = false;
    if (p->fw_sleeper) {
        zw_fwupd_expect_sleep(&p->fw, ZWZ_FW_WAKE_WAIT_MS);
    }
    zw_net_hold_awake(&p->net, now, node);
    if (!zw_fwupd_start(&p->fw, now, node, cc_version, rec->manufacturer_id, target, image, len,
                        payload)) {
        zw_fwupd_expect_sleep(&p->fw, 0u);
        zw_net_hold_awake(&p->net, now, 0u);
        return UI_ZW_REQ_INVALID; /* a frame too small for a fragment */
    }
    op_reset(p, UI_ZW_OP_FIRMWARE);
    op_step(p, UI_ZW_STEP_WORKING, 0u);
    p->progress.node = node;
    p->progress.generation = generation;
    p->op_user = user;
    if (p->fw_reinterview == node) {
        p->fw_reinterview = 0u; /* a new update: the old one's reading is superseded */
    }
    log_device(p, ALARM_EV_ZWAVE_SOFTWARE, zwz_panel_zone_of(p, node, generation), node,
               ALARM_ZW_SW_STARTED, user);
    fw_settle(p); /* a start that could not even send ends at once */
    return UI_ZW_REQ_OK;
}

/* ---- network.md §14: the radio's own firmware -------------------------------------------- */

ui_zw_req_result_t zwz_panel_radio_update(zwz_panel_t *p, uint64_t now, const uint8_t *image,
                                          size_t len, bool allowed, uint8_t user)
{
    p->now = now;
    if (!allowed) {
        return UI_ZW_REQ_NOT_ALLOWED;
    }
    if (!p->started || zw_gbl_check(image, len, NULL) != ZW_GBL_OK) {
        return UI_ZW_REQ_INVALID;
    }
    if ((p->progress.op != UI_ZW_OP_NONE && p->progress.step != UI_ZW_STEP_DONE) ||
        zw_incl_state(&p->net.incl) != ZW_INCL_IDLE ||
        zw_remove_state(&p->net.rm) != ZW_REMOVE_IDLE || zw_fwupd_running(&p->fw)) {
        return UI_ZW_REQ_BUSY;
    }
    switch (zw_net_update_radio(&p->net, now, true, image, len)) {
    case ZW_NET_STARTED: break;
    case ZW_NET_INVALID: return UI_ZW_REQ_INVALID;
    case ZW_NET_LINK_DOWN: return UI_ZW_REQ_NO_RADIO;
    default: return UI_ZW_REQ_BUSY;
    }
    op_reset(p, UI_ZW_OP_RADIO_UPDATE);
    op_step(p, UI_ZW_STEP_WORKING, 0u);
    p->progress.fw_total = (uint16_t)(zw_net_otw(&p->net)->total > 0xFFFFu
                                          ? 0xFFFFu
                                          : zw_net_otw(&p->net)->total);
    p->op_user = user;
    p->otw_hold = true;
    p->otw_hold_until = UINT64_MAX;
    log_raw(p, ALARM_EV_ZWAVE_SOFTWARE, 0u, 0u, ALARM_ZW_SW_RADIO_STARTED, user);
    return UI_ZW_REQ_OK;
}

/* Its end (ZW_NET_EV_OTW): the page's result and the log; arming stays held until the radio is
 * back and ours (NETWORK), ZWZ_PANEL_OTW_HOLD_MS at most. */
static void radio_update_ended(zwz_panel_t *p, const zw_otw_progress_t *o)
{
    if (!op_running(p, UI_ZW_OP_RADIO_UPDATE)) {
        return;
    }
    ui_zw_progress_t *g = &p->progress;
    g->fw_done = (uint16_t)(o->sent > 0xFFFFu ? 0xFFFFu : o->sent);
    p->otw_hold_until = p->now + ZWZ_PANEL_OTW_HOLD_MS;
    if (o->result == ZW_OTW_OK) {
        op_done(p, UI_ZW_RES_FW_UPDATED);
        log_raw(p, ALARM_EV_ZWAVE_SOFTWARE, 0u, 0u, ALARM_ZW_SW_RADIO_UPDATED, p->op_user);
        return;
    }
    g->fw_error = o->error;
    g->fw_why = o->result == ZW_OTW_TIMEOUT        ? UI_ZW_FWF_NO_ANSWER
                : o->result == ZW_OTW_RETRIES      ? UI_ZW_FWF_BAD_TRANSFER
                : o->result == ZW_OTW_ABORTED      ? UI_ZW_FWF_REFUSED
                : o->result == ZW_OTW_NOT_SUPPORTED ? UI_ZW_FWF_NOT_UPGRADABLE
                                                    : UI_ZW_FWF_DEVICE_FAILED;
    op_done(p, UI_ZW_RES_FW_FAILED);
    log_raw(p, ALARM_EV_ZWAVE_SOFTWARE, 0u, 0u, ALARM_ZW_SW_RADIO_FAILED, p->op_user);
}

bool zwz_panel_arming_held(const zwz_panel_t *p, uint64_t now)
{
    return p->otw_hold && now < p->otw_hold_until;
}

static void op_link(zwz_panel_t *p, bool up)
{
    if (!up) {
        fw_stop(p, true);
    }
    if (op_running(p, UI_ZW_OP_RESTART) || op_running(p, UI_ZW_OP_REGION) ||
        op_running(p, UI_ZW_OP_RESET)) {
        op_done(p, up ? UI_ZW_RES_DONE : UI_ZW_RES_FAILED);
    }
}

static void n_link_up(void *ctx, const zw_module_facts_t *f)
{
    zwz_panel_t *p = ctx;
    zwz_link_up(&p->bridge, f, p->now);
    p->down = ZW_DOWN_NONE;
    op_link(p, true);
}

static void n_link_down(void *ctx, const zw_link_down_t *why)
{
    zwz_panel_t *p = ctx;
    p->down = why != NULL ? why->reason : ZW_DOWN_NOT_ANSWERING;
    zwz_link_down(&p->bridge, p->now);
    op_link(p, false);
}

static void n_restarted(void *ctx, uint8_t reason)
{
    zwz_panel_t *p = ctx;
    zwz_link_down(&p->bridge, p->now);
    if (restart_logged(reason) && p->now >= p->expect_restart_until) {
        restart_log(p, reason);
    }
}

/* network.md §9: a device's own Home Security "RF jamming detected" (07/0C): its view, not proof
 * for the network - kept for its page, logged once an hour at most. */
static void jam_report(zwz_panel_t *p, const zw_s2_rx_t *rx)
{
    zw_cc_notification_report_t n;
    if (rx->len < 2u || rx->command[0] != ZW_CC_NOTIFICATION ||
        rx->command[1] != ZW_CMD_NOTIFICATION_REPORT ||
        !zw_cc_notification_report_parse(rx->command, rx->len, &n) || n.is_v1 ||
        n.status != 0xFFu || n.notification_type != 0x07u || n.event != 0x0Cu) {
        return;
    }
    const uint16_t gen = zwz_panel_generation(p, rx->node);
    size_t slot = ZWZ_PANEL_JAM_REPORTS;
    size_t pick = 0u; /* a free one, else the one heard from longest ago */
    bool free_found = false;
    for (size_t i = 0; i < ZWZ_PANEL_JAM_REPORTS; i++) {
        if (p->jam_reports[i].node == rx->node && p->jam_reports[i].generation == gen) {
            slot = i;
            break;
        }
        if (free_found) {
            continue;
        }
        if (p->jam_reports[i].node == 0u) {
            pick = i;
            free_found = true;
        } else if (p->jam_reports[i].at < p->jam_reports[pick].at) {
            pick = i;
        }
    }
    if (slot == ZWZ_PANEL_JAM_REPORTS) {
        slot = pick;
        memset(&p->jam_reports[slot], 0, sizeof p->jam_reports[slot]);
        p->jam_reports[slot].node = rx->node;
        p->jam_reports[slot].generation = gen;
    }
    p->jam_reports[slot].at = p->now;
    if (p->jam_reports[slot].logged_at == 0u ||
        p->now - p->jam_reports[slot].logged_at >= ZWZ_PANEL_JAM_LOG_MS) {
        p->jam_reports[slot].logged_at = p->now;
        log_device(p, ALARM_EV_ZWAVE_JAMMING, 0u, rx->node, 0u, 0u);
    }
}

static void n_received(void *ctx, const zw_s2_rx_t *rx)
{
    zwz_panel_t *p = ctx;
    jam_report(p, rx);
    zwz_received(&p->bridge, rx, p->now);
}

static void n_send_done(void *ctx, uint32_t tag, zw_s2_result_t result)
{
    zwz_panel_t *p = ctx;
    if (zw_fwupd_send_done(&p->fw, p->now, tag, result == ZW_S2_OK)) {
        fw_settle(p);
        return;
    }
    if (zwo_send_done(&p->outputs, p->now, tag, result == ZW_S2_OK)) {
        return;
    }
    if ((tag & ZWZ_PANEL_TAG_MASK) == ZWZ_PANEL_TAG) {
        if (tag == p->ping_tag) {
            p->ping_tag = 0u;
            if (op_running(p, UI_ZW_OP_PING)) {
                op_done(p, result == ZW_S2_OK ? UI_ZW_RES_ANSWERED : UI_ZW_RES_NO_ANSWER);
            }
        } else if (tag == p->identify_tag) {
            p->identify_tag = 0u;
            if (op_running(p, UI_ZW_OP_IDENTIFY)) {
                op_done(p, result == ZW_S2_OK ? UI_ZW_RES_ANSWERED : UI_ZW_RES_NO_ANSWER);
            }
        }
        return;
    }
    if (!zwz_send_done(&p->bridge, tag, result) && p->port.send_done != NULL) {
        p->port.send_done(p->port.ctx, tag, result);
    }
}

static void n_incl_event(void *ctx, const zw_incl_event_t *ev)
{
    zwz_panel_t *p = ctx;
    if (ev->kind == ZW_INCL_EV_DONE && ev->result == ZW_INCL_ADDED && ev->smartstart) {
        /* §4.2: it joined by itself - the panel's doing; one without its keys is removed again
         * (not logged: it never was usable) */
        if (ev->security == ZW_INCL_SEC_S2_UNAUTH || ev->security == ZW_INCL_SEC_S2_AUTH ||
            ev->security == ZW_INCL_SEC_S2_ACCESS) {
            const uint8_t user = p->op_user;
            p->op_user = 0u;
            log_join(p, ev);
            p->op_user = user;
        }
        return; /* no page follows it */
    }
    if (ev->kind == ZW_INCL_EV_DONE && ev->result == ZW_INCL_ADDED) {
        log_join(p, ev);
    }
    progress_incl(p, ev);
}

/* ---- SmartStart (network.md §4.2) ------------------------------------------------------------ */

static bool n_smart_find(void *ctx, const uint8_t nwi[4], bool long_range,
                         zw_incl_smartstart_t *out)
{
    const zwz_panel_t *p = ctx;
    if (zwz_panel_storage_full(p)) {
        return false; /* ZW-039: no room for its record - it asks again later */
    }
    if (zwz_panel_identity_held(p) || !zw_net_generations_left(&p->net)) {
        return false; /* ZW-014: not while a node ID's old occupant may come back - it asks again */
    }
    const int i = zw_prov_find_nwi(&p->prov, nwi, long_range);
    if (i < 0) {
        return false;
    }
    memcpy(out->dsk, p->prov.e[i].dsk, sizeof out->dsk);
    out->grant = p->prov.e[i].grant;
    out->long_range = p->prov.e[i].long_range;
    return true;
}

static void smart_ended(zwz_panel_t *p, const zw_net_event_t *ev)
{
    if (ev->smart == ZW_NET_SMART_JOINED) {
        (void)zw_prov_joined(&p->prov, ev->dsk, ev->node, ev->generation);
    } else {
        (void)zw_prov_failed(&p->prov, ev->dsk); /* the fifth stops it: the page says so */
    }
    p->dirty |= ZWZ_DIRTY_PROV;
}

/* A device leaves: removed by hand, its code is forgotten with it (the removal's page may keep
 * it); gone by itself, its entry waits for it again. */
static void smart_leaving(zwz_panel_t *p, const zw_net_event_t *ev, bool by_hand)
{
    const int i = zw_prov_find_node(&p->prov, ev->node, ev->generation);
    if (i < 0) {
        return;
    }
    if (by_hand) {
        p->prov_kept = p->prov.e[i];
        p->prov_kept.node = 0u;
        p->prov_kept.generation = 0u;
        p->prov_kept_valid = true;
        (void)zw_prov_remove(&p->prov, (size_t)i);
        p->progress.prov_forgot = true;
    } else {
        zw_prov_node_gone(&p->prov, ev->node, ev->generation);
    }
    p->dirty |= ZWZ_DIRTY_PROV;
}

static void prov_to_ui(const zwz_panel_t *p, const zw_prov_entry_t *e, ui_zw_prov_t *u)
{
    memset(u, 0, sizeof *u);
    memcpy(u->dsk, e->dsk, sizeof u->dsk);
    snprintf(u->name, sizeof u->name, "%s", e->name);
    u->state = (uint8_t)zw_prov_status(e);
    u->failures = e->failures;
    u->long_range = e->long_range;
    u->requested = e->requested;
    u->grant = e->grant;
    u->node = e->node;
    u->generation = e->generation;
    u->generic = e->generic;
    u->manufacturer = e->manufacturer;
    u->product_type = e->product_type;
    u->product_id = e->product_id;
    /* ZW-024: its model from the table that names devices (a code that carries no product: none) */
    if (e->manufacturer != 0u || e->product_type != 0u || e->product_id != 0u) {
        const zsrc_zw_quirk_t *q = p->src != NULL
            ? zsrc_zw_quirk_lookup(p->src, e->manufacturer, e->product_type, e->product_id)
            : zsrc_zw_quirk_for(e->manufacturer, e->product_type, e->product_id);
        if (q != NULL && q->name != NULL) {
            snprintf(u->model, sizeof u->model, "%s", q->name);
        }
    }
}

size_t zwz_panel_prov(const zwz_panel_t *p, ui_zw_prov_t *out, size_t max)
{
    size_t n = 0u;
    for (size_t i = 0; i < p->prov.n && n < max; i++) {
        prov_to_ui(p, &p->prov.e[i], &out[n++]);
    }
    return n;
}

/* A device of the network whose key this is: it is in already. */
static void smart_mark_joined(zwz_panel_t *p, const uint8_t dsk[16])
{
    for (size_t i = 0; i < zw_node_count(&p->net.nodes); i++) {
        const zw_node_record_t *rec = zw_node_at_const(&p->net.nodes, i);
        if (rec->has_dsk && memcmp(rec->dsk, dsk, sizeof rec->dsk) == 0) {
            (void)zw_prov_joined(&p->prov, dsk, rec->node_id, rec->generation);
            return;
        }
    }
}

ui_zw_prov_result_t zwz_panel_prov_register(zwz_panel_t *p, uint64_t now, const char *code,
                                            const char *name, bool allowed)
{
    p->now = now;
    if (!allowed) {
        return UI_ZW_PROV_NOT_ALLOWED;
    }
    if (code == NULL) {
        return UI_ZW_PROV_BAD_CODE;
    }
    size_t digits = 0u;
    for (const char *c = code; *c != '\0'; c++) {
        digits += *c >= '0' && *c <= '9' ? 1u : 0u;
    }
    zw_prov_result_t r;
    uint8_t dsk[16];
    if (digits == 40u) { /* a key typed from its label */
        if (!zw_dsk_from_digits(code, dsk)) {
            return UI_ZW_PROV_BAD_CODE;
        }
        r = zw_prov_add_dsk(&p->prov, dsk, false, name);
    } else {
        zw_qr_t q;
        const zw_qr_result_t qr = zw_qr_parse(code, &q);
        if (qr == ZW_QR_CHECKSUM) {
            return UI_ZW_PROV_CHECKSUM;
        }
        if (qr != ZW_QR_OK) {
            return UI_ZW_PROV_BAD_CODE;
        }
        memcpy(dsk, q.dsk, sizeof dsk);
        r = zw_prov_add_qr(&p->prov, &q, name);
    }
    switch (r) {
    case ZW_PROV_FULL: return UI_ZW_PROV_FULL;
    case ZW_PROV_S2_ONLY: return UI_ZW_PROV_S2_ONLY;
    case ZW_PROV_INVALID: return UI_ZW_PROV_BAD_CODE;
    default: break;
    }
    p->dirty |= ZWZ_DIRTY_PROV;
    smart_mark_joined(p, dsk);
    const int i = zw_prov_find_dsk(&p->prov, dsk);
    if (i >= 0 && p->prov.e[i].node != 0u) {
        return UI_ZW_PROV_JOINED_ALREADY;
    }
    return r == ZW_PROV_UPDATED ? UI_ZW_PROV_UPDATED : UI_ZW_PROV_ADDED;
}

ui_zw_prov_result_t zwz_panel_prov_change(zwz_panel_t *p, uint64_t now, const uint8_t dsk[16],
                                          uint8_t op, uint8_t arg, bool allowed)
{
    p->now = now;
    if (!allowed) {
        return UI_ZW_PROV_NOT_ALLOWED;
    }
    if (op == UI_ZW_PROV_KEEP) {
        if (!p->prov_kept_valid) {
            return UI_ZW_PROV_NOT_FOUND;
        }
        if (zw_prov_find_dsk(&p->prov, p->prov_kept.dsk) < 0) {
            if (p->prov.n >= ZW_PROV_MAX) {
                return UI_ZW_PROV_FULL;
            }
            p->prov.e[p->prov.n++] = p->prov_kept;
        }
        p->prov_kept_valid = false;
        p->progress.prov_forgot = false;
        p->dirty |= ZWZ_DIRTY_PROV;
        return UI_ZW_PROV_UPDATED;
    }
    const int i = dsk != NULL ? zw_prov_find_dsk(&p->prov, dsk) : -1;
    if (i < 0) {
        return UI_ZW_PROV_NOT_FOUND;
    }
    zw_prov_entry_t *e = &p->prov.e[i];
    switch (op) {
    case UI_ZW_PROV_ON: (void)zw_prov_set_active(&p->prov, (size_t)i, true); break;
    case UI_ZW_PROV_OFF_OP: (void)zw_prov_set_active(&p->prov, (size_t)i, false); break;
    case UI_ZW_PROV_FORGET: (void)zw_prov_remove(&p->prov, (size_t)i); break;
    case UI_ZW_PROV_GRANT: {
        const uint8_t may = e->requested != 0u ? e->requested : (uint8_t)ZW_S2_KEYS_S2;
        if (arg == 0u || (arg & (uint8_t)~may) != 0u) {
            return UI_ZW_PROV_BAD_CODE; /* nothing it did not ask for; never none */
        }
        e->grant = arg;
        break;
    }
    default: return UI_ZW_PROV_NOT_FOUND;
    }
    p->dirty |= ZWZ_DIRTY_PROV;
    return UI_ZW_PROV_UPDATED;
}

bool zwz_panel_load_prov(zwz_panel_t *p, const uint8_t *blob, size_t len)
{
    return zw_prov_decode(&p->prov, blob, len);
}

size_t zwz_panel_take_prov(zwz_panel_t *p, uint8_t *buf, size_t cap)
{
    if ((p->dirty & ZWZ_DIRTY_PROV) == 0u) {
        return 0u;
    }
    const size_t n = zw_prov_encode(&p->prov, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)((unsigned)p->dirty & ~ZWZ_DIRTY_PROV);
    }
    return n;
}

/* A device leaves (removed, reset locally, no longer listed): the removal page's words and the
 * log, while the zone source and the hub still know it. */
static void leaving(zwz_panel_t *p, const zw_net_event_t *ev)
{
    const uint16_t zone = zwz_panel_zone_of(p, ev->node, ev->generation);
    if (op_running(p, UI_ZW_OP_FIRMWARE) && p->progress.node == ev->node) {
        fw_stop(p, false); /* the device it was for is gone */
    }
    if (p->fw_reinterview == ev->node) {
        p->fw_reinterview = 0u;
    }
    for (size_t g = 0; g < ZWO_GARAGES; g++) {
        const zwo_garage_t *gar = &p->outputs.garage[g];
        if (gar->used && p->outputs.out[gar->output].node == ev->node) {
            p->dirty |= ZWZ_DIRTY_GARAGE; /* its garage goes with it */
        }
    }
    zwo_forget(&p->outputs, ev->node);
    if (ev->kind == ZW_NET_EV_NODE_REMOVED) {
        /* the installer's removal; one zw_net does itself (after a reset locally) was logged when
         * the device left */
        const bool failed = op_running(p, UI_ZW_OP_REMOVE_FAILED);
        if (failed || op_running(p, UI_ZW_OP_REMOVE)) {
            op_capture(p, ev->node, ev->generation);
            log_device(p, ALARM_EV_ZWAVE_REMOVED, zone, ev->node,
                       failed ? ALARM_ZW_REMOVED_FAILED : ALARM_ZW_REMOVED_EXCLUDED, p->op_user);
            smart_leaving(p, ev, true);
        } else {
            smart_leaving(p, ev, false);
        }
    } else if (ev->gone == ZW_NET_GONE_RESET_LOCALLY || !p->quiet) {
        smart_leaving(p, ev, false);
        /* not the network reset's or a replaced radio's own forgetting (the installer did it) */
        log_device(p, ALARM_EV_ZWAVE_LEFT, zone, ev->node,
                   ev->gone == ZW_NET_GONE_RESET_LOCALLY ? ALARM_ZW_LEFT_RESET_LOCALLY
                                                         : ALARM_ZW_LEFT_MISSING,
                   0u);
    }
    drop_checkin(p, ev->node, ev->generation);
}

/* network.md §12 power: the first read of each is the cap every change stays within. */
static void power_read(zwz_panel_t *p, const zw_net_power_t *pw)
{
    zwz_cfg_t *c = &p->setup.cfg;
    if (pw->known && !c->power_cap_set) {
        c->power_cap_set = true;
        c->power_cap_dbm10 = pw->dbm10;
        p->dirty |= ZWZ_DIRTY_CFG;
    }
    if (pw->lr_known && !c->lr_cap_set) {
        c->lr_cap_set = true;
        c->lr_cap_dbm10 = pw->lr_dbm10;
        p->dirty |= ZWZ_DIRTY_CFG;
    }
}

_Static_assert(ZWKC_SLOTS <= 8u, "keyctl_down holds one bit per keypad control slot");

/* ZW-038 (CORE-TRBL-008): an assigned keypad that may arm and disarm is supervised - the owner
 * hears when it can't be reached and when it is heard again, as an engine device trouble. A slot
 * with a partition is one (its emergency buttons need one too); a keypad that only shows the
 * panel's state (zw_feedback.h) isn't. The slot's partition decides, not zwkc_live: a keypad
 * included again at another security class stays supervised (its silence is still news), and a
 * radio restart can't read as "heard again". Level-based: what is said follows what is true, so
 * a slot turned off, a keypad removed or given another generation, or a network forgotten
 * restores. */
static void keypads_supervise(zwz_panel_t *p)
{
    if (p->port.keypad_reachable == NULL) {
        return;
    }
    for (size_t s = 0; s < ZWKC_SLOTS; s++) {
        bool down = false;
        if (p->keyctl != NULL && p->keyctl->slot[s].binding.partition != 0u) {
            const zwkc_binding_t *b = &p->keyctl->slot[s].binding;
            const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, b->node);
            const zw_health_node_t *h =
                rec != NULL && rec->generation == b->generation ? health_of(p, b->node) : NULL;
            down = h != NULL && !h->reachable;
        }
        const uint8_t bit = (uint8_t)(1u << s);
        if (down != ((p->keyctl_down & bit) != 0u)) {
            p->keyctl_down = (uint8_t)(down ? (p->keyctl_down | bit) : (p->keyctl_down & ~bit));
            p->port.keypad_reachable(p->port.ctx, (uint8_t)ZWKC_DEVICE(s), !down);
        }
    }
}

static void n_event(void *ctx, const zw_net_event_t *ev)
{
    zwz_panel_t *p = ctx;
    if (ev->kind == ZW_NET_EV_NODE_REMOVED || ev->kind == ZW_NET_EV_NODE_GONE) {
        /* ZW-014: its node ID may go to a new device only once the cache without it is stored -
         * written even when the cache never had it (nothing else would end the hold) */
        p->gone_seq++;
        p->dirty |= ZWZ_DIRTY_NODES;
        leaving(p, ev);
    }
    zwz_event(&p->bridge, ev, p->now); /* devices added/interviewed/gone, reachability, grace */
    progress_net(p, ev);
    if (ev->kind == ZW_NET_EV_NODE_READY) {
        const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, ev->node);
        if (rec != NULL && zwo_scan(&p->outputs, rec) > 0u) {
            zwo_refresh(&p->outputs, p->now, ev->node); /* where they are now */
        }
    } else if (ev->kind == ZW_NET_EV_NETWORK && ev->radio == ZW_NET_RADIO_OURS) {
        p->out_refresh = true;
    }
    if (ev->kind == ZW_NET_EV_ROUTES) {
        progress_routes(p, ev);
    } else if (ev->kind == ZW_NET_EV_CHECK_IN) {
        checkin_ended(p, ev);
    } else if (ev->kind == ZW_NET_EV_SMARTSTART) {
        smart_ended(p, ev);
    } else if (ev->kind == ZW_NET_EV_NETWORK) {
        /* its devices are registered once the check is over: stored check-ins at the next poll */
        p->reissue = ev->radio == ZW_NET_RADIO_OURS;
        if (ev->radio == ZW_NET_RADIO_OURS && !op_running(p, UI_ZW_OP_RADIO_UPDATE)) {
            p->otw_hold = false; /* §14: the radio is back, with its network */
        }
        if (ev->radio == ZW_NET_RADIO_REPLACED &&
            (p->setup.cfg.power_cap_set || p->setup.cfg.lr_cap_set)) {
            p->setup.cfg.power_cap_set = p->setup.cfg.lr_cap_set = false; /* another radio's */
            p->dirty |= ZWZ_DIRTY_CFG;
        }
    } else if (ev->kind == ZW_NET_EV_POWER && ev->power_ev == ZW_NET_POWER_READ) {
        power_read(p, &ev->power);
    } else if (ev->kind == ZW_NET_EV_OTW) {
        radio_update_ended(p, &ev->otw);
    } else if (ev->kind == ZW_NET_EV_REGION) {
        p->setup.cfg.region_set = true; /* network.md §12: kept for every start-up */
        p->setup.cfg.region = ev->region;
        p->dirty |= ZWZ_DIRTY_CFG;
    } else if (ev->kind == ZW_NET_EV_RESET) {
        log_raw(p, ALARM_EV_ZWAVE_NETWORK_RESET, 0u, 0u, 0u, p->op_user);
        if (p->prov.n > 0u) { /* network.md §12: the provisioning list goes too */
            zw_prov_clear(&p->prov);
            p->prov_kept_valid = false;
            p->dirty |= ZWZ_DIRTY_PROV;
        }
        if (p->checkin_count > 0u) {
            p->checkin_count = 0u;
            p->dirty |= ZWZ_DIRTY_CHECKINS;
        }
    }
    keypads_supervise(p); /* ZW-038: reachability, removals, a network gone */
}

static void n_store_nodes(void *ctx, const zw_node_cache_t *nodes, uint16_t generation)
{
    zwz_panel_t *p = ctx;
    (void)nodes; /* encoded when the owner takes it (zwz_panel_take_nodes) */
    p->setup.generation = generation;
    p->dirty |= ZWZ_DIRTY_NODES;
}

static void n_store_keys(void *ctx, uint32_t home_id, const zw_s2_keys_t *keys)
{
    zwz_panel_t *p = ctx;
    p->setup.keys.bound = true;
    p->setup.keys.home_id = home_id;
    p->setup.keys.keys = *keys;
    p->dirty |= ZWZ_DIRTY_KEYS;
}

static bool n_quirk(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out)
{
    zwz_panel_t *p = ctx;
    return zwz_quirk(&p->bridge, rec, out);
}

static uint32_t n_check_in(void *ctx, const zw_node_record_t *rec)
{
    const zwz_panel_t *p = ctx;
    const zwz_checkin_t *c = find_checkin(p, rec->node_id, rec->generation);
    return c != NULL ? c->seconds : 0u;
}

/* zw_net with what is stored now. */
static void net_init(zwz_panel_t *p)
{
    zw_net_port_t port;
    memset(&port, 0, sizeof port);
    port.ctx = p;
    port.write = n_write;
    port.reset_line = p->port.reset_line != NULL ? n_reset_line : NULL;
    port.link_up = n_link_up;
    port.link_down = n_link_down;
    port.restarted = n_restarted;
    port.received = n_received;
    port.send_done = n_send_done;
    port.incl_event = n_incl_event;
    port.event = n_event;
    port.store_nodes = n_store_nodes;
    port.store_keys = n_store_keys;
    port.quirk = n_quirk;
    port.check_in = n_check_in;
    port.smartstart_find = n_smart_find;
    zw_net_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.host.set_region = p->setup.cfg.region_set;
    cfg.host.region = p->setup.cfg.region;
    cfg.host.long_range = p->setup.long_range;
    cfg.jam = p->setup.jam; /* §9 */
    cfg.jam_dbm = p->setup.cfg.jam_dbm;
    cfg.keys = p->setup.keys.keys;
    cfg.keys_bound = p->setup.keys.bound;
    cfg.keys_home_id = p->setup.keys.home_id;
    cfg.generation = p->setup.generation;
    cfg.check_in_s = p->setup.cfg.check_in_s;
    cfg.interview = true;
    zw_net_init(&p->net, &port, &cfg, &p->crypto);
    zw_wipe(&cfg.keys, sizeof cfg.keys);
}

/* ---- life cycle ------------------------------------------------------------------------------ */

static bool claim(void *ctx, const zw_s2_rx_t *rx);

void zwz_panel_init(zwz_panel_t *p, const zwz_panel_port_t *port, const zwz_panel_setup_t *setup,
                    const zwz_checkin_t *checkins, size_t checkin_count, zsrc_zw_t *src,
                    const zw_crypto_port_t *crypto)
{
    memset(p, 0, sizeof *p);
    p->port = *port;
    p->crypto = *crypto;
    p->src = src;
    p->setup = *setup;
    p->down = ZW_DOWN_NONE;
    p->nvs_free = ZWZ_NVS_NOT_TOLD;
    for (size_t i = 0; checkins != NULL && i < checkin_count && i < ZWZ_PANEL_CHECKINS; i++) {
        if (checkins[i].node != 0u && checkins[i].seconds != 0u) {
            p->checkins[p->checkin_count++] = checkins[i];
        }
    }
    net_init(p);
    zwz_init(&p->bridge, &p->net, src, &p->setup.identity);
    zwz_set_clock(&p->bridge, p->port.local_time, p->port.ctx);
    zwz_set_claim(&p->bridge, claim, p);
    const zw_fwupd_port_t fw_port = {p, fw_send};
    zw_fwupd_init(&p->fw, &fw_port);
    const zwo_port_t out_port = {p, out_send, out_position, out_result, NULL};
    zwo_init(&p->outputs, &out_port);
}

bool zwz_panel_load_nodes(zwz_panel_t *p, const uint8_t *blob, size_t len)
{
    return zw_net_load_nodes(&p->net, blob, len);
}

void zwz_panel_start(zwz_panel_t *p, uint64_t now)
{
    p->now = now;
    p->started = true;
    zw_net_start(&p->net, now);
}

void zwz_panel_set_feedback(zwz_panel_t *p, const zwf_t *feedback)
{
    p->feedback = feedback;
}

void zwz_panel_set_keyctl(zwz_panel_t *p, const zwkc_t *keyctl)
{
    p->keyctl = keyctl;
    keypads_supervise(p); /* ZW-038: none supervised any more says restored */
}

static bool claim(void *ctx, const zw_s2_rx_t *rx)
{
    zwz_panel_t *p = ctx;
    if (zwo_received(&p->outputs, p->now, rx->node,
                     (uint8_t)(rx->multi_channel ? (unsigned)rx->source_endpoint : 0u),
                     rx->command, rx->len)) {
        return true; /* a switch's Report: an output's state (ZW-034) */
    }
    if (zw_fwupd_running(&p->fw) && rx->node == p->fw.node && rx->len >= 1u &&
        rx->command[0] == ZW_CC_FIRMWARE_UPDATE && !rx->multi_channel) {
        /* the updating device's, at the class the panel sends it in: nothing else counts */
        if (rx->security == zw_s2_class_for(&p->net.s2, rx->node, ZW_CC_FIRMWARE_UPDATE)) {
            (void)zw_fwupd_received(&p->fw, p->now, rx->node, rx->command, rx->len);
            fw_settle(p);
        }
        return true;
    }
    return p->port.command != NULL && p->port.command(p->port.ctx, rx);
}

void zwz_panel_on_control(zwz_panel_t *p, uint64_t now, uint8_t byte)
{
    p->now = now;
    zw_net_on_control(&p->net, now, byte);
}

void zwz_panel_on_frame(zwz_panel_t *p, uint64_t now, const zw_frame_t *frame)
{
    p->now = now;
    zw_net_on_frame(&p->net, now, frame);
}

void zwz_panel_on_text(zwz_panel_t *p, uint64_t now, const uint8_t *bytes, size_t len)
{
    p->now = now;
    zw_net_on_text(&p->net, now, bytes, len);
}

void zwz_panel_poll(zwz_panel_t *p, uint64_t now)
{
    p->now = now;
    if (!p->started) {
        return;
    }
    /* §4.2: listen for SmartStart while a registered device waits, nothing is armed and there
     * is room for it (ZW-039) */
    zw_net_smartstart(&p->net, now,
                      zw_prov_waiting(&p->prov) && p->port.disarmed != NULL &&
                          p->port.disarmed(p->port.ctx) && !zwz_panel_storage_full(p) &&
                          !zwz_panel_identity_held(p) && zw_net_generations_left(&p->net));
    zw_net_poll(&p->net, now);
    restart_flush(p);
    if (p->reissue) {
        p->reissue = false;
        reissue(p, now);
    }
    zwz_poll(&p->bridge, now);
    if (zw_fwupd_running(&p->fw)) {
        zw_fwupd_poll(&p->fw, now);
        fw_settle(p);
    }
    if (p->out_refresh) {
        /* the network is ours again: ask the listening devices' outputs where they are */
        p->out_refresh = false;
        for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
            const zwo_output_t *u = &p->outputs.out[i];
            const zw_node_record_t *rec = u->used ? zw_node_find_const(&p->net.nodes, u->node)
                                                  : NULL;
            if (rec != NULL && !zw_iv_sleeping(rec) && (i == 0u || u->node != p->outputs.out[i - 1u].node)) {
                zwo_refresh(&p->outputs, now, u->node);
            }
        }
    }
    zwo_poll(&p->outputs, now);
    keypads_supervise(p); /* ZW-038: the owner's slots may have changed */
    if (p->fw_reinterview != 0u && now >= p->fw_reinterview_at) {
        /* its new software can change what it is: its setup reads it again now, a sleeper's too
         * (its questions wait in its wake-up queue), as Z-Wave JS's refreshInfo({ waitForWakeup:
         * false }) after an update. Refused while every scratch record is in use (busy): tried
         * again, never dropped - Z-Wave JS always re-interviews after a successful update (BL-152) */
        const uint16_t node = p->fw_reinterview;
        if (record(p, node, p->fw_reinterview_gen) == NULL ||
            zw_net_interview_now(&p->net, now, node)) {
            p->fw_reinterview = 0u;
        } else {
            p->fw_reinterview_at = now + ZWZ_FW_REINTERVIEW_MS;
        }
    }
}

uint64_t zwz_panel_next_deadline(const zwz_panel_t *p)
{
    if (!p->started) {
        return UINT64_MAX;
    }
    if (p->reissue) {
        return p->now; /* the network was just checked: stored check-ins at once */
    }
    uint64_t t = zw_net_next_deadline(&p->net);
    const uint64_t b = zwz_next_deadline(&p->bridge);
    const uint64_t f = zw_fwupd_next_deadline(&p->fw);
    const uint64_t o = zwo_next_deadline(&p->outputs);
    t = b < t ? b : t;
    t = f < t ? f : t;
    t = o < t ? o : t;
    if (p->out_refresh) {
        t = p->now;
    }
    if (p->fw_reinterview != 0u && p->fw_reinterview_at < t) {
        t = p->fw_reinterview_at;
    }
    /* ZW-041: the held restarts, once the hour is up - or at once, when they found the queue full
     * and it has room again. Never while it is full, which would spin: the owner takes the log
     * after each poll and polls at least once a second. */
    if (p->restarts_held != 0u && p->log_n < ZWZ_PANEL_LOG_QUEUE) {
        const uint64_t due =
            p->restart_logged ? p->restart_logged_at + ZWZ_PANEL_RESTART_LOG_MS : p->now;
        t = due < t ? due : t;
    }
    return t;
}

void zwz_panel_expect_restart(zwz_panel_t *p, uint64_t now, uint32_t ms)
{
    p->expect_restart_until = now + ms;
}

/* ---- the NVS budget (ZW-039; docs/features/20 devices.md "The NVS budget") ------------------- */

uint32_t zwz_panel_nvs_need(const zwz_panel_t *p)
{
    /* after the add the cache holds one more device and still has to be rewritable whole beside
     * its old copy; the reserve covers the configuration's rewrite (never at the same time: each
     * is one NVS call) */
    const uint32_t rewrite =
        ZWZ_NVS_NODE_GROWTH + ZWZ_NVS_CACHE_ENTRIES(zw_node_count(&p->net.nodes) + 1u);
    return rewrite > ZWZ_NVS_ADD_MIN ? rewrite : ZWZ_NVS_ADD_MIN;
}

bool zwz_panel_identity_held(const zwz_panel_t *p)
{
    return p->nodes_failed || p->gone_stored != p->gone_seq;
}

bool zwz_panel_storage_full(const zwz_panel_t *p)
{
    return p->nvs_free != ZWZ_NVS_NOT_TOLD && p->nvs_free < zwz_panel_nvs_need(p);
}

void zwz_panel_nvs_free(zwz_panel_t *p, uint32_t entries)
{
    p->nvs_free = entries;
}

/* ---- requests -------------------------------------------------------------------------------- */

static ui_zw_req_result_t net_result(zw_net_result_t r)
{
    switch (r) {
    case ZW_NET_STARTED: return UI_ZW_REQ_OK;
    case ZW_NET_NOT_ALLOWED: return UI_ZW_REQ_NOT_ALLOWED;
    case ZW_NET_BUSY: return UI_ZW_REQ_BUSY;
    case ZW_NET_LINK_DOWN: return UI_ZW_REQ_NO_RADIO;
    case ZW_NET_UNSUPPORTED: return UI_ZW_REQ_UNSUPPORTED;
    default: return UI_ZW_REQ_INVALID; /* UNCHANGED: nothing to do */
    }
}

static ui_zw_req_result_t remove_result(zw_remove_result_t r)
{
    switch (r) {
    case ZW_REMOVE_STARTED: return UI_ZW_REQ_OK;
    case ZW_REMOVE_NOT_ALLOWED: return UI_ZW_REQ_NOT_ALLOWED;
    case ZW_REMOVE_BUSY: return UI_ZW_REQ_BUSY;
    case ZW_REMOVE_LINK_DOWN: return UI_ZW_REQ_NO_RADIO;
    default: return UI_ZW_REQ_INVALID;
    }
}

/* ZW-014: a new occupant of a node ID waits for the cache that no longer has the old one. A
 * failed write is the storage's trouble (FULL: its words); one only due is a moment (BUSY). */
static ui_zw_req_result_t identity_refusal(const zwz_panel_t *p)
{
    return p->nodes_failed ? UI_ZW_REQ_FULL : UI_ZW_REQ_BUSY;
}

static ui_zw_req_result_t add(zwz_panel_t *p, uint64_t now, const ui_zw_request_t *req,
                              bool allowed)
{
    /* ProtocolDone's timeout grows with the devices that route (network.md §4 step 4) */
    uint16_t listening = 0u;
    uint16_t flirs = 0u;
    for (size_t i = 0; i < zw_node_count(&p->net.nodes); i++) {
        const zw_node_record_t *rec = zw_node_at_const(&p->net.nodes, i);
        listening = (uint16_t)((unsigned)listening + (rec->is_listening ? 1u : 0u));
        flirs = (uint16_t)((unsigned)flirs + (rec->is_flirs ? 1u : 0u));
    }
    if (allowed && zwz_panel_storage_full(p)) {
        return UI_ZW_REQ_FULL; /* ZW-039: the room the alarm needs is kept */
    }
    if (allowed && !zw_net_generations_left(&p->net)) {
        /* ZW-014: no generation left that no device had - fail closed. Practically unreachable
         * (65535 inclusions on one panel). Its own words ("full" is not quite true) and a way to
         * start the counter again (a new home ID, a network reset) are an open decision. */
        return UI_ZW_REQ_FULL;
    }
    if (allowed && zwz_panel_identity_held(p)) {
        return identity_refusal(p); /* ZW-014 */
    }
    p->quiet = true; /* a replaced radio's old devices go: the installer's doing, not news */
    const zw_incl_result_t a = zw_net_add_device(&p->net, now, allowed, listening, flirs);
    p->quiet = false;
    switch (a) {
    case ZW_INCL_STARTED:
        op_reset(p, UI_ZW_OP_ADD);
        op_step(p, UI_ZW_STEP_STARTING, 0u);
        p->op_user = req->user;
        return UI_ZW_REQ_OK;
    case ZW_INCL_NOT_ALLOWED: return UI_ZW_REQ_NOT_ALLOWED;
    case ZW_INCL_BUSY: return UI_ZW_REQ_BUSY;
    case ZW_INCL_LINK_DOWN:
    case ZW_INCL_FOREIGN_RADIO: return UI_ZW_REQ_NO_RADIO;
    default: return UI_ZW_REQ_INVALID;
    }
}

/* network.md §7: new devices' check-in (node 0), or a device's own through zw_net_set_check_in -
 * its Interval Set waits for the device's next wake-up, and only the device's word makes it count
 * (ZW_NET_EV_CHECK_IN, checkin_ended). The value zw_net sets (within the device's limits) is kept
 * (`chk`): the page says "changes when it next wakes up" until the device has it, a later
 * interview sets it (n_check_in), and a restart of the panel sets it again (reissue). */
static ui_zw_req_result_t check_in(zwz_panel_t *p, uint64_t now, const ui_zw_request_t *req,
                                   bool allowed)
{
    if (!allowed) {
        return UI_ZW_REQ_NOT_ALLOWED;
    }
    if (req->seconds < ZWZ_CHECK_IN_MIN_S || req->seconds > ZWZ_CHECK_IN_MAX_S) {
        return UI_ZW_REQ_INVALID;
    }
    if (req->node == 0u) {
        p->setup.cfg.check_in_s = req->seconds; /* new devices' (and the interview's default) */
        p->net.cfg.check_in_s = req->seconds;
        p->dirty |= ZWZ_DIRTY_CFG;
        return UI_ZW_REQ_OK;
    }
    if (record(p, req->node, req->generation) == NULL) {
        return UI_ZW_REQ_INVALID; /* not this device: removed, or its node ID given to another */
    }
    if (find_checkin(p, req->node, req->generation) == NULL &&
        p->checkin_count >= ZWZ_PANEL_CHECKINS) {
        return UI_ZW_REQ_BUSY; /* nowhere to keep it (never: one a device) */
    }
    /* refused: not allowed, no radio (LINK_DOWN), not a device whose check-in can be set, its
     * wake-up queue full - whatever was chosen before stands */
    const zw_net_result_t r = zw_net_set_check_in(&p->net, now, allowed, req->node, req->seconds);
    if (r != ZW_NET_STARTED) {
        return net_result(r);
    }
    const zw_net_checkin_t *c = net_checkin(p, req->node);
    if (c != NULL) {
        (void)set_checkin(p, req->node, req->generation, c->seconds);
    } else {
        /* its Set could not go at all and ended within the call, replacing any earlier choice:
         * nothing is waiting now */
        drop_checkin(p, req->node, req->generation);
    }
    return UI_ZW_REQ_OK;
}

ui_zw_req_result_t zwz_panel_request(zwz_panel_t *p, uint64_t now, const ui_zw_request_t *req,
                                     bool allowed)
{
    p->now = now;
    if (req == NULL) {
        return UI_ZW_REQ_INVALID;
    }
    if (!p->started) {
        return UI_ZW_REQ_NO_RADIO;
    }
    zw_incl_t *in = &p->net.incl;
    zw_remove_t *rm = &p->net.rm;
    ui_zw_progress_t *g = &p->progress;
    ui_zw_req_result_t r = UI_ZW_REQ_OK;
    if (op_running(p, UI_ZW_OP_FIRMWARE) &&
        (req->kind == UI_ZW_REQ_ADD || req->kind == UI_ZW_REQ_REMOVE ||
         req->kind == UI_ZW_REQ_REMOVE_FAILED || req->kind == UI_ZW_REQ_REGION ||
         req->kind == UI_ZW_REQ_RESET || req->kind == UI_ZW_REQ_RESTART ||
         req->kind == UI_ZW_REQ_PING || req->kind == UI_ZW_REQ_IDENTIFY)) {
        return UI_ZW_REQ_BUSY; /* a device's software update runs: nothing else on the network */
    }
    switch ((ui_zw_req_kind_t)req->kind) {
    case UI_ZW_REQ_ADD: r = add(p, now, req, allowed); break;
    case UI_ZW_REQ_REMOVE:
        if (allowed && p->nodes_failed) {
            r = UI_ZW_REQ_FULL; /* ZW-014: see zwz_panel_redo - as Add, until the cache is stored */
            break;
        }
        r = remove_result(zw_net_remove_device(&p->net, now, allowed));
        if (r == UI_ZW_REQ_OK) {
            op_reset(p, UI_ZW_OP_REMOVE);
            op_step(p, UI_ZW_STEP_STARTING, 0u);
            p->op_user = req->user;
        }
        break;
    case UI_ZW_REQ_REMOVE_FAILED:
        if (record(p, req->node, req->generation) == NULL) {
            r = UI_ZW_REQ_INVALID;
            break;
        }
        if (allowed && p->nodes_failed) {
            r = UI_ZW_REQ_FULL; /* ZW-014: as Remove */
            break;
        }
        r = remove_result(zw_net_remove_failed(&p->net, now, allowed, req->node));
        if (r == UI_ZW_REQ_OK) {
            op_reset(p, UI_ZW_OP_REMOVE_FAILED);
            op_step(p, UI_ZW_STEP_WORKING, 0u);
            g->node = req->node;
            g->generation = req->generation;
            p->op_user = req->user;
        }
        break;
    case UI_ZW_REQ_KEEP_LISTENING:
        if (zw_incl_state(in) == ZW_INCL_LISTENING) {
            zw_incl_keep_listening(in, now);
        } else if (zw_remove_state(rm) == ZW_REMOVE_LISTENING) {
            zw_remove_keep_listening(rm, now);
        } else {
            r = UI_ZW_REQ_INVALID;
        }
        break;
    case UI_ZW_REQ_STOP:
        if (op_running(p, UI_ZW_OP_ROUTES)) {
            zw_net_stop_routes(&p->net, now); /* its end follows (ROUTES) */
        } else if (op_running(p, UI_ZW_OP_FIRMWARE)) {
            fw_stop(p, false); /* it keeps its old software: the device drops the partial one */
        } else if (op_running(p, UI_ZW_OP_ADD) && g->step == UI_ZW_STEP_READING) {
            /* "Stop and finish later": its interview goes on, but it is not set up - never
             * "Added" until it is (the device page follows it, and offers "Try again") */
            op_done(p, UI_ZW_RES_SETUP_PAUSED);
        } else if (zw_incl_state(in) == ZW_INCL_STARTING ||
                   zw_incl_state(in) == ZW_INCL_LISTENING) {
            zw_incl_stop(in, now);
        } else if (zw_remove_state(rm) == ZW_REMOVE_STARTING ||
                   zw_remove_state(rm) == ZW_REMOVE_LISTENING) {
            zw_remove_stop(rm, now);
        } else {
            r = UI_ZW_REQ_INVALID;
        }
        break;
    case UI_ZW_REQ_ALLOW:
        if (zw_incl_state(in) == ZW_INCL_GRANT && op_running(p, UI_ZW_OP_ADD) &&
            g->step == UI_ZW_STEP_ASK_ALLOW) {
            op_step(p, UI_ZW_STEP_SECURING, 0u); /* before: a grant that fails ends within the call */
        }
        r = zw_incl_grant(in, now, req->keys) ? UI_ZW_REQ_OK : UI_ZW_REQ_INVALID;
        if (r != UI_ZW_REQ_OK && op_running(p, UI_ZW_OP_ADD) && g->step == UI_ZW_STEP_SECURING &&
            zw_incl_state(in) == ZW_INCL_GRANT) {
            op_step(p, UI_ZW_STEP_ASK_ALLOW, p->deadline); /* refused: the question stands */
        }
        break;
    case UI_ZW_REQ_PIN:
        /* the DSK's first group is 16 bits: 5 digits above 65535 are no PIN (never truncated) */
        if (req->pin > UI_ZW_PIN_MAX || zw_incl_state(in) != ZW_INCL_PIN) {
            r = UI_ZW_REQ_INVALID; /* refused before anything changes: the question stands */
            break;
        }
        if (op_running(p, UI_ZW_OP_ADD)) {
            op_step(p, UI_ZW_STEP_SECURING, 0u); /* before: its end can come within the call */
        }
        r = zw_incl_pin(in, now, (uint16_t)req->pin) ? UI_ZW_REQ_OK : UI_ZW_REQ_INVALID;
        break;
    case UI_ZW_REQ_INSECURE:
        if (zw_incl_state(in) == ZW_INCL_GRANT || zw_incl_state(in) == ZW_INCL_PIN) {
            /* "Securing" first: the cancel ends the exchange within the call, and its end moves
             * the page on to reading what it can do (it used to be set back to "Securing" after,
             * and the page froze there: B015 UX-001) */
            if (op_running(p, UI_ZW_OP_ADD)) {
                op_step(p, UI_ZW_STEP_SECURING, 0u);
            }
            zw_incl_cancel(in, now);
        } else {
            r = UI_ZW_REQ_INVALID;
        }
        break;
    case UI_ZW_REQ_PING: {
        static const uint8_t NOP[1] = {0x00u};
        const zw_node_record_t *rec = record(p, req->node, req->generation);
        if (rec == NULL || zw_iv_sleeping(rec)) {
            r = UI_ZW_REQ_INVALID; /* a sleeper answers at its next wake-up only */
        } else if (op_running(p, UI_ZW_OP_PING) || p->ping_tag != 0u) {
            r = UI_ZW_REQ_BUSY;
        } else {
            const uint32_t tag = next_tag(p, TAG_PING);
            if (zw_net_send(&p->net, now, req->node, ZW_SEC_NONE, NOP, sizeof NOP, tag) !=
                ZW_S2_OK) {
                r = zw_host_link(&p->net.host) == ZW_LINK_UP ? UI_ZW_REQ_BUSY : UI_ZW_REQ_NO_RADIO;
            } else {
                p->ping_tag = tag;
                op_reset(p, UI_ZW_OP_PING);
                op_step(p, UI_ZW_STEP_WORKING, 0u);
                g->node = req->node;
                g->generation = req->generation;
                p->op_user = req->user;
            }
        }
        break;
    }
    case UI_ZW_REQ_REPLACE: {
        /* network.md §10: a new device in `node`'s place - followed as an add (`replacing`) */
        if (record(p, req->node, req->generation) == NULL) {
            r = UI_ZW_REQ_INVALID;
            break;
        }
        if (allowed && zwz_panel_storage_full(p)) {
            r = UI_ZW_REQ_FULL; /* ZW-039: its record may grow; the room the alarm needs is kept */
            break;
        }
        if (allowed && zwz_panel_identity_held(p)) {
            r = identity_refusal(p); /* ZW-014: its record starts again - stored first */
            break;
        }
        switch (zw_net_replace_failed(&p->net, now, allowed, req->node)) {
        case ZW_INCL_STARTED:
            op_reset(p, UI_ZW_OP_ADD);
            g->replacing = true;
            g->node = req->node;
            g->generation = req->generation;
            op_step(p, UI_ZW_STEP_STARTING, 0u);
            p->op_user = req->user;
            break;
        case ZW_INCL_NOT_ALLOWED: r = UI_ZW_REQ_NOT_ALLOWED; break;
        case ZW_INCL_BUSY: r = UI_ZW_REQ_BUSY; break;
        case ZW_INCL_LINK_DOWN:
        case ZW_INCL_NOT_LISTENING: r = UI_ZW_REQ_NO_RADIO; break;
        default: r = UI_ZW_REQ_INVALID; break;
        }
        break;
    }
    case UI_ZW_REQ_IDENTIFY: {
        /* command-classes.md: Indicator Set v3, the node identify indicator - its light blinks
         * 3 times, 0.8 s apart (0.6 s on): the device's own light, never the panel's screen */
        const zw_node_record_t *rec = record(p, req->node, req->generation);
        if (rec == NULL) {
            r = UI_ZW_REQ_INVALID;
        } else if (!can_identify(rec)) {
            r = UI_ZW_REQ_UNSUPPORTED;
        } else if (op_running(p, UI_ZW_OP_IDENTIFY) || p->identify_tag != 0u ||
                   op_running(p, UI_ZW_OP_PING)) {
            r = UI_ZW_REQ_BUSY;
        } else {
            static const uint8_t ENTRIES[9] = {ZW_INDICATOR_ID_NODE_IDENTIFY, 0x03u, 0x08u,
                                               ZW_INDICATOR_ID_NODE_IDENTIFY, 0x04u, 0x03u,
                                               ZW_INDICATOR_ID_NODE_IDENTIFY, 0x05u, 0x06u};
            const zw_cc_indicator_set_t set = {true, 0u, 0u, ENTRIES, 3u};
            uint8_t cmd[16];
            const size_t n = zw_cc_indicator_set_build(cmd, sizeof cmd, &set);
            const uint32_t tag = next_tag(p, TAG_IDENTIFY);
            if (n == 0u ||
                zw_net_send(&p->net, now, req->node, ZW_SEC_AUTO, cmd, n, tag) != ZW_S2_OK) {
                r = zw_host_link(&p->net.host) == ZW_LINK_UP ? UI_ZW_REQ_BUSY : UI_ZW_REQ_NO_RADIO;
            } else {
                p->identify_tag = tag;
                op_reset(p, UI_ZW_OP_IDENTIFY);
                op_step(p, UI_ZW_STEP_WORKING, 0u);
                g->node = req->node;
                g->generation = req->generation;
                p->op_user = req->user;
            }
        }
        break;
    }
    case UI_ZW_REQ_ROUTES: {
        /* network.md §11: one device (`node`) or every device (0) */
        if (req->node != 0u && record(p, req->node, req->generation) == NULL) {
            r = UI_ZW_REQ_INVALID;
            break;
        }
        r = net_result(zw_net_rebuild_routes(&p->net, now, allowed, req->node));
        if (r == UI_ZW_REQ_OK && zw_routes_running(zw_net_routes(&p->net))) {
            op_reset(p, UI_ZW_OP_ROUTES);
            op_step(p, UI_ZW_STEP_WORKING, 0u);
            g->node = req->node;
            g->generation = req->node != 0u ? req->generation : 0u;
            p->routes_one = req->node;
            p->op_user = req->user;
            routes_counts(p);
        }
        break;
    }
    case UI_ZW_REQ_CHECK_IN: r = check_in(p, now, req, allowed); break;
    case UI_ZW_REQ_SETUP: {
        /* "Try its setup again": its interview continues from the step it stopped at. "Set it up
         * again" (a finished one, BL-152): read again from step 1, as after its software update -
         * the same node, keys, generation and zone bindings; its zones keep their state, as the
         * zone source hears of it only when the reading ends; a sleeper's at its next wake-up. */
        const zw_node_record_t *rec = record(p, req->node, req->generation);
        if (!allowed) {
            r = UI_ZW_REQ_NOT_ALLOWED;
        } else if (rec == NULL || zw_iv_inputs(rec, 0u) == ZW_IV_INPUTS_NOT_USABLE) {
            r = UI_ZW_REQ_INVALID;
        } else if (zw_host_link(&p->net.host) != ZW_LINK_UP) {
            r = UI_ZW_REQ_NO_RADIO;
        } else if (rec->interview_step >= ZW_IV_STEP_DONE) {
            if ((g->op != UI_ZW_OP_NONE && g->step != UI_ZW_STEP_DONE) ||
                zw_incl_state(in) != ZW_INCL_IDLE || zw_remove_state(rm) != ZW_REMOVE_IDLE ||
                zw_fwupd_running(&p->fw) || zw_routes_running(zw_net_routes(&p->net)) ||
                (zw_iv_running(&p->net.iv, req->node) &&
                 !zw_iv_refreshing(&p->net.iv, req->node))) {
                /* one thing at a time; its own re-read is restarted (Z-Wave JS's refreshInfo),
                 * not refused - its Alarm Sensor catch-up (BL-130) is short and runs on */
                r = UI_ZW_REQ_BUSY;
            } else {
                r = zw_net_interview(&p->net, now, req->node) ? UI_ZW_REQ_OK : UI_ZW_REQ_BUSY;
                if (r == UI_ZW_REQ_OK && p->fw_reinterview == req->node) {
                    p->fw_reinterview = 0u; /* this reading is the one its update waited for */
                }
            }
        } else {
            r = zw_net_resume_interview(&p->net, now, req->node) ? UI_ZW_REQ_OK : UI_ZW_REQ_BUSY;
            if (r == UI_ZW_REQ_OK && op_running(p, UI_ZW_OP_ADD) && g->node == req->node) {
                g->stalled = false; /* the add page shows it running again (B015 UX-008) */
                g->asleep = false;
            }
        }
        break;
    }
    case UI_ZW_REQ_REGION:
    case UI_ZW_REQ_RESET:
    case UI_ZW_REQ_RESTART: {
        /* The event that says it happened can come inside the call: the page's operation first,
         * put back when it was refused. */
        const ui_zw_progress_t before = p->progress;
        const uint64_t deadline = p->deadline;
        const uint8_t pending = p->pending;
        const uint8_t user = p->op_user;
        const uint8_t op = req->kind == UI_ZW_REQ_REGION  ? UI_ZW_OP_REGION
                           : req->kind == UI_ZW_REQ_RESET ? UI_ZW_OP_RESET
                                                          : UI_ZW_OP_RESTART;
        op_reset(p, op);
        op_step(p, UI_ZW_STEP_WORKING, 0u);
        p->op_user = req->user;
        if (req->kind == UI_ZW_REQ_REGION) {
            r = net_result(zw_net_set_region(&p->net, now, allowed, req->region));
        } else if (req->kind == UI_ZW_REQ_RESET) {
            p->quiet = true; /* its devices go: the reset is the news */
            r = net_result(zw_net_reset_network(&p->net, now, allowed, NULL));
            p->quiet = false;
        } else {
            r = net_result(zw_net_restart(&p->net, now, allowed));
            if (r == UI_ZW_REQ_OK) {
                zwz_link_down(&p->bridge, now); /* until the network is checked again */
            }
        }
        if (r != UI_ZW_REQ_OK) {
            p->progress = before;
            p->deadline = deadline;
            p->pending = pending;
            p->op_user = user;
        }
        break;
    }
    case UI_ZW_REQ_DISMISS:
        if (g->step == UI_ZW_STEP_DONE) {
            op_reset(p, UI_ZW_OP_NONE);
        }
        break;
    case UI_ZW_REQ_OUTPUT: {
        const zwo_result_t o = zwz_panel_output_set(p, now, req->node, req->generation,
                                                    req->endpoint, req->level, now);
        r = o == ZWO_OK             ? UI_ZW_REQ_OK
            : o == ZWO_ERR_BUSY     ? UI_ZW_REQ_BUSY
            : o == ZWO_ERR_OFFLINE || o == ZWO_ERR_SEND ? UI_ZW_REQ_NO_RADIO
                                                        : UI_ZW_REQ_INVALID;
        break;
    }
    case UI_ZW_REQ_GARAGE_MOVE: {
        if (req->level != 0 && !allowed) {
            r = UI_ZW_REQ_NOT_ALLOWED; /* opening takes a user's code (HCTL-005): installer mode */
            break;
        }
        const zwo_result_t o = zwz_panel_garage_move(p, now, req->node, req->generation,
                                                     req->endpoint, req->level != 0, now,
                                                     req->user);
        r = o == ZWO_OK || o == ZWO_ALREADY || o == ZWO_ERR_POSITION ||
                    o == ZWO_ERR_RELAY_STUCK || o == ZWO_ERR_BUSY
                ? UI_ZW_REQ_OK /* the page says what happened */
            : o == ZWO_ERR_BUSY ? UI_ZW_REQ_BUSY
            : o == ZWO_ERR_OFFLINE || o == ZWO_ERR_SEND ? UI_ZW_REQ_NO_RADIO
                                                        : UI_ZW_REQ_INVALID;
        break;
    }
    case UI_ZW_REQ_GARAGE: {
        const int i = out_index(p, req->node, req->generation, req->endpoint);
        size_t slot = ZWO_GARAGES, free_slot = ZWO_GARAGES;
        for (size_t k = 0; k < ZWO_GARAGES; k++) {
            if (p->outputs.garage[k].used && i >= 0 &&
                p->outputs.garage[k].output == (uint8_t)i) {
                slot = k;
            } else if (!p->outputs.garage[k].used && free_slot == ZWO_GARAGES) {
                free_slot = k;
            }
        }
        if (!allowed) {
            r = UI_ZW_REQ_NOT_ALLOWED;
        } else if (i < 0 || req->seconds > ZWO_TRAVEL_S_MAX) {
            r = UI_ZW_REQ_INVALID;
        } else if (req->zone == 0u) {
            if (slot < ZWO_GARAGES) {
                /* PL03: never while its move runs - its relay's release would go with it; the
                 * same refusal as a reassignment then */
                if (zwo_garage_clear(&p->outputs, slot) == ZWO_OK) {
                    p->dirty |= ZWZ_DIRTY_GARAGE;
                } else {
                    r = UI_ZW_REQ_BUSY;
                }
            }
        } else {
            const size_t use = slot < ZWO_GARAGES ? slot : free_slot;
            const zwo_result_t o =
                use < ZWO_GARAGES
                    ? zwo_garage_assign(&p->outputs, use, (size_t)i, req->zone,
                                        (uint16_t)req->seconds)
                    : ZWO_ERR_BUSY;
            r = o == ZWO_OK ? UI_ZW_REQ_OK : o == ZWO_ERR_BUSY ? UI_ZW_REQ_BUSY : UI_ZW_REQ_INVALID;
            if (o == ZWO_OK) {
                p->garage_last[use] = UI_ZW_OUT_NONE;
                p->dirty |= ZWZ_DIRTY_GARAGE;
            }
        }
        break;
    }
    default: /* FEEDBACK, CONTROL, FW_*: the owner's */
        r = UI_ZW_REQ_UNSUPPORTED;
        break;
    }
    return r;
}

ui_zw_req_result_t zwz_panel_interview_result(zwz_panel_t *p, uint64_t now, uint16_t node)
{
    p->now = now;
    if (!p->started) {
        return UI_ZW_REQ_NO_RADIO;
    }
    if (zw_node_find_const(&p->net.nodes, node) == NULL) {
        return UI_ZW_REQ_INVALID;
    }
    /* BL-152: a known device refused - its re-read would start now while both scratch records
     * are in use - is busy, as the touchscreen's "Set it up again" says */
    return zw_net_interview(&p->net, now, node) ? UI_ZW_REQ_OK : UI_ZW_REQ_BUSY;
}

bool zwz_panel_interview(zwz_panel_t *p, uint64_t now, uint16_t node)
{
    return zwz_panel_interview_result(p, now, node) == UI_ZW_REQ_OK;
}

/* ---- what the pages show --------------------------------------------------------------------- */

static uint8_t ui_security(uint8_t classes)
{
    if (classes == ZW_NODE_SECURITY_UNKNOWN) {
        return UI_ZW_SEC_UNKNOWN;
    }
    if ((classes & ZW_S2_KEY_ACCESS) != 0u) {
        return UI_ZW_SEC_S2_ACCESS;
    }
    if ((classes & ZW_S2_KEY_AUTH) != 0u) {
        return UI_ZW_SEC_S2_AUTH;
    }
    if ((classes & ZW_S2_KEY_UNAUTH) != 0u) {
        return UI_ZW_SEC_S2_UNAUTH;
    }
    return ((unsigned)classes & ZW_S2_KEY_S0) != 0u ? UI_ZW_SEC_S0 : UI_ZW_SEC_NONE;
}

static bool lists_cc(const zw_node_record_t *rec, uint8_t cc)
{
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == cc) {
            return true;
        }
    }
    return false;
}

/* Its security as the pages say it. No class granted is "Not encrypted" only once its own list of
 * command classes is known (from joining, or read again after a failed key exchange): before that
 * it is not confirmed; and one that lists Security 2 was offered it and failed or declined. */
static uint8_t device_security(const zw_node_record_t *rec)
{
    const uint8_t sec = ui_security(rec->security_classes);
    if (sec != UI_ZW_SEC_NONE) {
        return sec;
    }
    if (rec->cc_count == 0u) {
        return UI_ZW_SEC_UNCONFIRMED;
    }
    return lists_cc(rec, ZW_S2_CC) ? UI_ZW_SEC_NONE_CAPABLE : UI_ZW_SEC_NONE;
}

/* Its power: a Battery command class (or a battery level heard) says battery; mains only when it
 * always listens and its full list (the secure one read too) has no Battery; else not known.
 * Never from FLiRS or listening alone. */
static uint8_t device_power(const zw_node_record_t *rec, const zsrc_zw_device_t *d)
{
    if (lists_cc(rec, ZW_CC_BATTERY) ||
        (d != NULL && d->battery_pct != ZSRC_ZW_BATTERY_UNKNOWN)) {
        return UI_ZW_POWER_BATTERY;
    }
    if (rec->has_protocol && rec->is_listening && !rec->is_flirs && rec->cc_count > 0u &&
        rec->interview_step >= ZW_IV_STEP_SECURE_CCS) {
        return UI_ZW_POWER_MAINS;
    }
    return UI_ZW_POWER_UNKNOWN;
}

/* Its kind, from what it lists (for its name when it has no model, never a brand). */
static uint8_t device_kind(const zw_node_record_t *rec)
{
    if (lists_cc(rec, ZW_CC_ENTRY_CONTROL)) {
        return UI_ZW_KIND_KEYPAD;
    }
    if (lists_cc(rec, 0x79u) /* Sound Switch */ ||
        (rec->generic_class == 0x10u && lists_cc(rec, 0x71u) && !lists_cc(rec, 0x30u))) {
        return UI_ZW_KIND_SIREN;
    }
    if (lists_cc(rec, 0x26u)) {
        return UI_ZW_KIND_DIMMER;
    }
    if (lists_cc(rec, 0x25u)) {
        return lists_cc(rec, ZW_CC_MULTI_CHANNEL) ? UI_ZW_KIND_RELAY : UI_ZW_KIND_SWITCH;
    }
    if (rec->generic_class == 0x0Fu) {
        return UI_ZW_KIND_REPEATER;
    }
    if (lists_cc(rec, 0x71u) || lists_cc(rec, 0x30u) || lists_cc(rec, 0x31u)) {
        return UI_ZW_KIND_SENSOR;
    }
    return UI_ZW_KIND_DEVICE;
}

/* A question of its interview waits for it to wake up. */
static bool parked(const zwz_panel_t *p, uint16_t node)
{
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        const zw_iv_node_t *s = &p->net.iv.slots[i];
        if (s->node == node && (s->parked || s->told_asleep)) {
            return true;
        }
    }
    return false;
}

static void device_row(const zwz_panel_t *p, uint64_t now, const zw_node_record_t *rec,
                       ui_zw_device_t *out)
{
    memset(out, 0, sizeof *out);
    const zsrc_zw_device_t *d = source_device(p, rec->node_id, rec->generation);
    out->node = rec->node_id;
    out->generation = rec->generation;
    model_of(d, out->model, sizeof out->model);
    out->has_identity = rec->has_identity;
    out->manufacturer = rec->manufacturer_id;
    out->product_type = rec->product_type;
    out->product_id = rec->product_id;
    out->security = device_security(rec);
    switch (zw_iv_inputs(rec, 0u)) {
    case ZW_IV_INPUTS_NOT_USABLE: out->state = UI_ZW_DEV_NOT_USABLE; break;
    case ZW_IV_INPUTS_NOT_READY:
        if (zw_incl_state(&p->net.incl) != ZW_INCL_IDLE &&
            zw_incl_node(&p->net.incl) == rec->node_id) {
            out->state = UI_ZW_DEV_JOINING; /* its question or PIN waits on the add page */
        } else if (parked(p, rec->node_id)) {
            out->state = UI_ZW_DEV_WAKE_NEEDED;
        } else if (!zw_iv_running(&p->net.iv, rec->node_id) && p->net.checked &&
                   zw_host_link(&p->net.host) == ZW_LINK_UP) {
            /* its interview gave up (network.md §5: its attempts) and nothing runs it now */
            out->state = UI_ZW_DEV_SETUP_STOPPED;
        } else {
            out->state = UI_ZW_DEV_SETTING_UP;
        }
        break;
    case ZW_IV_INPUTS_NO_LIFELINE:
    case ZW_IV_INPUTS_PULL: out->state = UI_ZW_DEV_NO_REPORTS; break;
    default: out->state = UI_ZW_DEV_READY; break;
    }
    if (out->state == UI_ZW_DEV_READY || out->state == UI_ZW_DEV_NO_REPORTS) {
        /* BL-152: a finished device read again keeps working on its old facts; the pages tell a
         * re-read by its setup's steps, all done (setup_done == setup_total): reading, waiting
         * for its wake-up, or stopped (it works as before) */
        if (zw_iv_refreshing(&p->net.iv, rec->node_id)) {
            out->state = parked(p, rec->node_id) ? UI_ZW_DEV_WAKE_NEEDED : UI_ZW_DEV_SETTING_UP;
        } else if (zw_iv_reread_stopped(&p->net.iv, rec->node_id)) {
            out->state = UI_ZW_DEV_SETUP_STOPPED;
        }
    }
    out->listening = rec->is_listening || rec->is_flirs;
    out->flirs = rec->is_flirs;
    out->power = device_power(rec, d);
    out->setup_done = (uint8_t)(rec->interview_step <= ZW_IV_STEP_DONE ? (unsigned)rec->interview_step
                                                                       : ZW_IV_STEP_DONE);
    out->setup_total = ZW_IV_STEP_DONE;
    out->kind = device_kind(rec);
    out->heard_s = UI_ZW_NEVER;
    const zw_health_node_t *h = health_of(p, rec->node_id);
    if (h != NULL) {
        out->unreachable = !h->reachable;
        const uint64_t at = h->last_heard_ms;
        const uint64_t ago = now >= at ? (now - at) / 1000u : 0u;
        out->heard_s = ago < UI_ZW_NEVER ? (uint32_t)ago : UI_ZW_NEVER - 1u;
    }
    out->battery_pct = (uint8_t)(d != NULL ? (unsigned)d->battery_pct : UI_ZW_BATTERY_UNKNOWN);
    out->battery_low = d != NULL && d->lowbat_src != 0u;
    out->signal = signal_view(p, rec->node_id);
    out->device_class = ALARM_DEVCLASS_GENERIC;
    for (uint8_t e = 0u; d != NULL && e < ZSRC_ZW_EP_MAX && out->device_class == ALARM_DEVCLASS_GENERIC;
         e++) {
        for (uint8_t s = 1u; s < 32u; s++) {
            if ((d->offered[e] & (1u << s)) != 0u) {
                out->device_class = (uint8_t)zsrc_zw_slot_class(d, s);
                break;
            }
        }
    }
}

/* The link down: why, in the pages' words. */
static uint8_t down_radio(const zwz_panel_t *p)
{
    switch (p->down) {
    case ZW_DOWN_BAD_ANSWER:
    case ZW_DOWN_NOT_CONTROLLER:
    case ZW_DOWN_WRONG_CHIP:
    case ZW_DOWN_MISSING_FUNCTION:
    case ZW_DOWN_WRONG_LIBRARY: return UI_ZW_RADIO_UNUSABLE;
    case ZW_DOWN_UPDATING: return UI_ZW_RADIO_UPDATING;
    default: return UI_ZW_RADIO_NOT_ANSWERING;
    }
}

bool zwz_panel_read_power(zwz_panel_t *p, uint64_t now)
{
    p->now = now;
    return zw_net_power_read(&p->net, now);
}

zw_net_result_t zwz_panel_set_power(zwz_panel_t *p, uint64_t now, bool allowed, bool long_range,
                                    int16_t dbm10)
{
    p->now = now;
    const zwz_cfg_t *c = &p->setup.cfg;
    if (long_range ? !c->lr_cap_set : !c->power_cap_set) {
        return ZW_NET_INVALID; /* nothing read yet: no cap */
    }
    return zw_net_set_power(&p->net, now, allowed, long_range, dbm10,
                            long_range ? c->lr_cap_dbm10 : c->power_cap_dbm10);
}

bool zwz_panel_set_jam(zwz_panel_t *p, uint64_t now, int8_t dbm)
{
    p->now = now;
    if (!zw_net_set_jam_threshold(&p->net, dbm)) {
        return false;
    }
    p->setup.cfg.jam_dbm = dbm;
    p->dirty |= ZWZ_DIRTY_CFG;
    return true;
}

void zwz_panel_status(const zwz_panel_t *p, uint64_t now, ui_zw_status_t *out)
{
    memset(out, 0, sizeof *out);
    {
        const zw_jam_t *j = zw_net_jam(&p->net); /* §9 */
        out->jammed = zw_jam_jammed(j);
        out->noise_known = j->has_reading;
        out->noise_dbm = j->quietest;
    }
    out->storage_full = zwz_panel_storage_full(p); /* ZW-039 */
    if (zw_routes_running(zw_net_routes(&p->net))) { /* §11: "3 of 7 done" */
        uint16_t failed, skipped, waiting;
        zw_routes_counts(zw_net_routes(&p->net), &out->routes_done, &out->routes_total, &failed,
                         &skipped, &waiting);
    }
    for (size_t i = 0; i < p->prov.n; i++) { /* SmartStart: registered, waiting to join */
        out->joining = (uint16_t)(out->joining +
                                  (zw_prov_status(&p->prov.e[i]) == ZW_PROV_WAITING ? 1u : 0u));
    }
    const zw_module_facts_t *f = facts(p);
    switch (zw_host_link(&p->net.host)) {
    case ZW_LINK_UP:
        switch (zw_net_radio(&p->net)) {
        case ZW_NET_RADIO_OURS: out->radio = UI_ZW_RADIO_WORKING; break;
        case ZW_NET_RADIO_REPLACED: out->radio = UI_ZW_RADIO_REPLACED; break;
        case ZW_NET_RADIO_FOREIGN: out->radio = UI_ZW_RADIO_FOREIGN; break;
        default: out->radio = UI_ZW_RADIO_STARTING; break;
        }
        break;
    case ZW_LINK_DOWN: out->radio = down_radio(p); break;
    default:
        /* off, starting, recovering: a radio that stopped answering stays "not answering" while
         * the panel keeps trying it - its retries are not news (B015 UX-026); a restart the
         * panel asked for is "starting" */
        out->radio = p->down != ZW_DOWN_NONE && now >= p->expect_restart_until &&
                             !op_running(p, UI_ZW_OP_RESTART) && !op_running(p, UI_ZW_OP_REGION) &&
                             !op_running(p, UI_ZW_OP_RESET)
                         ? down_radio(p)
                         : UI_ZW_RADIO_STARTING;
        break;
    }
    out->update_advised = f->update_note;
    out->region_known = f->region_known;
    out->region = f->region;
    out->region_count = (uint8_t)((unsigned)f->region_count < UI_ZW_REGIONS_MAX
                                      ? (unsigned)f->region_count : UI_ZW_REGIONS_MAX);
    memcpy(out->regions, f->regions, out->region_count);
    const zw_node_cache_t *nodes = &p->net.nodes;
    out->devices = (uint16_t)zw_node_count(nodes);
    for (size_t i = 0; i < zw_node_count(nodes); i++) {
        const uint16_t id = zw_node_at_const(nodes, i)->node_id;
        const zw_health_node_t *h = health_of(p, id);
        out->not_heard =
            (uint16_t)((unsigned)out->not_heard + (h != NULL && !h->reachable ? 1u : 0u));
        out->devices_lr =
            (uint16_t)((unsigned)out->devices_lr + (id >= ZW_REGION_LR_NODE_MIN ? 1u : 0u));
    }
    /* ZW-045: what choosing each region would do to the devices */
    for (size_t i = 0; i < (size_t)out->region_count; i++) {
        out->region_change[i] = (uint8_t)zw_region_change(
            out->region_known, out->region, out->regions[i],
            (uint16_t)((unsigned)out->devices - (unsigned)out->devices_lr), out->devices_lr,
            &out->region_stop[i]);
    }
    out->replaced = (uint16_t)(out->radio == UI_ZW_RADIO_REPLACED ? (unsigned)out->devices : 0u);
    out->check_in_s = p->setup.cfg.check_in_s != 0u ? p->setup.cfg.check_in_s : ZW_NET_CHECK_IN_S;
    out->firmware_major = f->firmware_major;
    out->firmware_minor = f->firmware_minor;
    out->sdk_major = f->protocol_major;
    out->sdk_minor = f->protocol_minor;
    out->sdk_patch = f->protocol_patch;
    out->home_id = f->home_id;
}

size_t zwz_panel_devices(const zwz_panel_t *p, uint64_t now, ui_zw_device_t *out, size_t max)
{
    const zw_node_cache_t *nodes = &p->net.nodes;
    size_t n = 0u;
    for (size_t i = 0; i < zw_node_count(nodes) && n < max; i++) {
        device_row(p, now, zw_node_at_const(nodes, i), &out[n++]);
    }
    /* the zones each device feeds: every zone's binding once */
    for (uint16_t z = 1u; p->port.binding != NULL && n > 0u && z <= ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!p->port.binding(p->port.ctx, z, &b)) {
            continue;
        }
        for (size_t k = 0; k < n; k++) {
            ui_zw_device_t *d = &out[k];
            bool feeds = false;
            for (size_t r = 0; r < ZSRC_ROLE_COUNT && !feeds; r++) {
                feeds = is_device(p, &b.in[r].addr, d->node, d->generation);
            }
            if (feeds && d->zone_count < UI_ZW_ZONES_MAX) {
                d->zones[d->zone_count++] = z;
            }
        }
    }
    return n;
}

bool zwz_panel_device(const zwz_panel_t *p, uint64_t now, uint16_t node, uint16_t generation,
                      ui_zw_device_page_t *out)
{
    memset(out, 0, sizeof *out);
    const zw_node_record_t *rec = record(p, node, generation);
    if (rec == NULL) {
        return false;
    }
    device_row(p, now, rec, &out->dev);
    out->identify = can_identify(rec);
    out->jam_report_s = UI_ZW_NEVER;
    for (size_t i = 0; i < ZWZ_PANEL_JAM_REPORTS; i++) {
        if (p->jam_reports[i].node == node && p->jam_reports[i].generation == generation &&
            p->jam_reports[i].at != 0u) {
            out->jam_report_s = (uint32_t)((now - p->jam_reports[i].at) / 1000u);
        }
    }
    const size_t zn = device_zones(p, node, generation, out->dev.zones, UI_ZW_ZONES_MAX);
    out->dev.zone_count = (uint8_t)(zn < UI_ZW_ZONES_MAX ? zn : UI_ZW_ZONES_MAX);
    out->check_in_s = rec->has_wake_up ? rec->wake_up_interval_seconds : 0u;
    out->check_in_known = rec->has_wake_up || rec->interview_step >= ZW_IV_STEP_WAKE_UP;
    out->check_in_min_s = rec->wake_up_min_seconds;
    out->check_in_max_s = rec->wake_up_max_seconds;
    const zwz_checkin_t *c = find_checkin(p, node, generation);
    if (c != NULL && rec->has_wake_up && c->seconds != rec->wake_up_interval_seconds) {
        out->check_in_next_s = c->seconds; /* "Changes when it next wakes up" */
    }
    if (p->feedback != NULL) {
        switch (zwf_fit(&p->net, node)) {
        case ZWF_FIT_OK: out->feedback.fit = UI_ZW_FB_AVAILABLE; break;
        case ZWF_FIT_NOT_READY: out->feedback.fit = UI_ZW_FB_NOT_READY; break;
        case ZWF_FIT_NOT_SECURE: out->feedback.fit = UI_ZW_FB_NOT_SECURE; break;
        case ZWF_FIT_UNSUPPORTED: out->feedback.fit = UI_ZW_FB_UNSUPPORTED; break;
        default: out->feedback.fit = UI_ZW_FB_NONE; break;
        }
        const size_t slot = zwf_slot_of(p->feedback, node);
        if (slot < ZWF_TARGETS && p->feedback->target[slot].binding.generation == generation) {
            out->feedback.partition = p->feedback->target[slot].binding.partition;
            out->feedback.live = p->feedback->target[slot].live;
        }
    }
    if (p->keyctl != NULL) {
        switch (zwkc_fit(&p->net, node)) {
        case ZWKC_FIT_OK: out->control.fit = UI_ZW_FB_AVAILABLE; break;
        case ZWKC_FIT_NOT_READY: out->control.fit = UI_ZW_FB_NOT_READY; break;
        case ZWKC_FIT_NOT_SECURE: out->control.fit = UI_ZW_FB_NOT_SECURE; break;
        default: out->control.fit = UI_ZW_FB_NONE; break;
        }
        const size_t slot = zwkc_slot_of(p->keyctl, node);
        if (slot < ZWKC_SLOTS && p->keyctl->slot[slot].binding.generation == generation) {
            out->control.partition = p->keyctl->slot[slot].binding.partition;
            out->control.panic_mask = p->keyctl->slot[slot].binding.panic_mask;
            out->control.live = zwkc_live(p->keyctl, slot, &p->net);
        }
    }
    for (size_t i = 0; i < ZWO_OUTPUTS && out->output_count < UI_ZW_OUTPUTS_MAX; i++) {
        const zwo_output_t *u = &p->outputs.out[i];
        if (!u->used || u->node != node || u->generation != generation) {
            continue;
        }
        ui_zw_output_t *o = &out->outputs[out->output_count++];
        o->endpoint = u->endpoint;
        o->kind = u->kind == ZWO_DIMMER ? UI_ZW_OUT_DIMMER
                  : u->kind == ZWO_RELAY ? UI_ZW_OUT_RELAY
                                         : UI_ZW_OUT_SWITCH;
        o->level = u->level;
        o->busy = u->cmd != 0u;
        o->last = p->out_last[i];
        for (size_t g = 0; g < ZWO_GARAGES; g++) {
            const zwo_garage_t *gar = &p->outputs.garage[g];
            if (gar->used && gar->output == i) {
                o->garage = true;
                o->garage_zone = gar->zone;
                o->garage_travel_s = gar->travel_s;
                o->busy = gar->state != ZWO_G_IDLE;
                o->last = p->garage_last[g];
                snprintf(o->garage_state, sizeof o->garage_state, "%s",
                         zwo_garage_state(&p->outputs, g));
                zwo_position_t goal = ZWO_POS_UNKNOWN;
                o->garage_goal = (uint8_t)(!zwo_garage_moving(&p->outputs, g, &goal) ? 0u
                                           : goal == ZWO_POS_OPEN                   ? 1u
                                                                                    : 2u);
            }
        }
    }
    out->fw.known = rec->has_versions;
    out->fw.major = rec->fw_major;
    out->fw.minor = rec->fw_minor;
    out->fw.has_hardware = rec->has_versions && rec->hw_version != 0xFFu;
    out->fw.hardware = rec->hw_version;
    out->fw.updatable = zwz_panel_fw_updatable(p, node, generation);
    const zsrc_zw_device_t *d = source_device(p, node, generation);
    if (d == NULL) {
        return true;
    }
    if (d->quirk != NULL) {
        snprintf(out->wake_words, sizeof out->wake_words, "%s",
                 d->quirk->wake_words != NULL ? d->quirk->wake_words : "");
        snprintf(out->add_words, sizeof out->add_words, "%s",
                 d->quirk->add_words != NULL ? d->quirk->add_words : "");
        snprintf(out->note, sizeof out->note, "%s", d->quirk->note != NULL ? d->quirk->note : "");
    }
    const uint32_t home = facts(p)->home_id;
    for (uint8_t e = 0u; e < ZSRC_ZW_EP_MAX; e++) {
        for (uint8_t s = 1u; s < 32u && out->input_count < UI_ZW_INPUTS_MAX; s++) {
            if ((d->offered[e] & (1u << s)) == 0u) {
                continue;
            }
            ui_zw_input_t *in = &out->inputs[out->input_count++];
            in->addr = zsrc_zwave_address(home, generation, node, e, s);
            in->endpoint = e;
            in->slot = s;
            in->device_class = (uint8_t)zsrc_zw_slot_class(d, s);
            zsrc_zw_input_words(d, e, s, in->words, sizeof in->words);
            zsrc_zw_state_words(d, e, s, now, in->state, sizeof in->state);
            in->zone = input_zone(p, &in->addr);
        }
    }
    return true;
}

void zwz_panel_progress(const zwz_panel_t *p, uint64_t now, ui_zw_progress_t *out)
{
    *out = p->progress;
    out->seconds_left =
        p->deadline > now ? (uint32_t)((p->deadline - now + 999u) / 1000u) : 0u;
    if (out->op == UI_ZW_OP_ADD && out->step == UI_ZW_STEP_READING) {
        const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, out->node);
        out->setup_done = (uint8_t)(rec != NULL ? (unsigned)rec->interview_step : 0u);
        out->setup_total = ZW_IV_STEP_DONE;
    }
    if (out->op == UI_ZW_OP_RADIO_UPDATE && out->step != UI_ZW_STEP_DONE) {
        const zw_otw_progress_t *o = zw_net_otw(&p->net);
        out->fw_done = (uint16_t)(o->sent > 0xFFFFu ? 0xFFFFu : o->sent);
        out->fw_total = (uint16_t)(o->total > 0xFFFFu ? 0xFFFFu : o->total);
    }
    if (out->op == UI_ZW_OP_FIRMWARE && out->step != UI_ZW_STEP_DONE) {
        out->fw_done = p->fw.sent_max;
        out->fw_total = p->fw.total;
        out->fw_wake = p->fw.waiting_wake;
        if (out->fw_wake && p->fw.deadline != UINT64_MAX && p->fw.deadline > now) {
            out->seconds_left = (uint32_t)((p->fw.deadline - now + 999u) / 1000u);
        }
    }
    if (out->model[0] == '\0' && out->node != 0u) {
        model_of(source_device(p, out->node, out->generation), out->model, sizeof out->model);
    }
}

uint16_t zwz_panel_generation(const zwz_panel_t *p, uint16_t node)
{
    const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, node);
    return (uint16_t)(rec != NULL ? (unsigned)rec->generation : 0u);
}

uint16_t zwz_panel_zone_of(const zwz_panel_t *p, uint16_t node, uint16_t generation)
{
    uint16_t zone = 0u;
    return (uint16_t)(device_zones(p, node, generation, &zone, 1u) > 0u ? (unsigned)zone : 0u);
}

/* ---- what is to be stored and logged ---------------------------------------------------------- */

uint8_t zwz_panel_dirty(const zwz_panel_t *p)
{
    return p->dirty;
}

size_t zwz_panel_take_nodes(zwz_panel_t *p, uint8_t *buf, size_t cap, uint16_t *generation)
{
    if ((p->dirty & ZWZ_DIRTY_NODES) == 0u) {
        return 0u;
    }
    const size_t n = zw_node_cache_encode(&p->net.nodes, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)((unsigned)p->dirty & ~ZWZ_DIRTY_NODES);
        p->gone_taken = p->gone_seq; /* ZW-014: every device gone so far is out of this copy */
        if (generation != NULL) {
            *generation = p->setup.generation;
        }
    }
    return n;
}

size_t zwz_panel_take_keys(zwz_panel_t *p, uint8_t *buf, size_t cap)
{
    if ((p->dirty & ZWZ_DIRTY_KEYS) == 0u) {
        return 0u;
    }
    const size_t n = zwz_keys_encode(&p->setup.keys, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)((unsigned)p->dirty & ~ZWZ_DIRTY_KEYS);
    }
    return n;
}

size_t zwz_panel_take_cfg(zwz_panel_t *p, uint8_t *buf, size_t cap)
{
    if ((p->dirty & ZWZ_DIRTY_CFG) == 0u) {
        return 0u;
    }
    const size_t n = zwz_cfg_encode(&p->setup.cfg, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)((unsigned)p->dirty & ~ZWZ_DIRTY_CFG);
    }
    return n;
}

size_t zwz_panel_take_checkins(zwz_panel_t *p, uint8_t *buf, size_t cap)
{
    if ((p->dirty & ZWZ_DIRTY_CHECKINS) == 0u) {
        return 0u;
    }
    const size_t n = zwz_checkins_encode(p->checkins, p->checkin_count, buf, cap);
    if (n > 0u) {
        p->dirty = (uint8_t)((unsigned)p->dirty & ~ZWZ_DIRTY_CHECKINS);
    }
    return n;
}

void zwz_panel_redo(zwz_panel_t *p, uint8_t dirty)
{
    p->dirty |= dirty;
    if ((dirty & ZWZ_DIRTY_NODES) != 0u) {
        /* ZW-014: identities held until a write succeeds - and no device leaves the module
         * meanwhile, not even one reset locally: a restart would read a cache that may lack it
         * and a counter below its generation, and give that generation to its node's next
         * device (its zones would follow that device) */
        p->nodes_failed = true;
        p->net.gone_held = true;
    }
}

void zwz_panel_stored(zwz_panel_t *p, uint8_t stored)
{
    if ((stored & ZWZ_DIRTY_NODES) != 0u) {
        p->gone_stored = p->gone_taken;
        p->nodes_failed = false;
        p->net.gone_held = false; /* a reset-locally device's removal goes on at the next poll */
    }
}

bool zwz_panel_take_log(zwz_panel_t *p, zwz_log_t *out)
{
    if (p->log_n == 0u) {
        return false;
    }
    *out = p->log[0];
    memmove(&p->log[0], &p->log[1], (p->log_n - 1u) * sizeof p->log[0]);
    p->log_n--;
    return true;
}

/* ---- the stored formats ------------------------------------------------------------------------ */

#define FORMAT 1u

static void put_u16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *out, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        out[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)((unsigned)in[0] | ((unsigned)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

size_t zwz_cfg_encode(const zwz_cfg_t *c, uint8_t *out, size_t cap)
{
    if (c == NULL || out == NULL || cap < ZWZ_CFG_BLOB) {
        return 0u;
    }
    out[0] = FORMAT;
    out[1] = (uint8_t)((c->region_set ? 0x01u : 0u) | (c->s0_allowed ? 0x02u : 0u));
    out[2] = c->region;
    out[3] = (uint8_t)c->jam_dbm;
    put_u32(&out[4], c->check_in_s);
    out[8] = (uint8_t)((c->power_cap_set ? 0x01u : 0u) | (c->lr_cap_set ? 0x02u : 0u));
    put_u16(&out[9], (uint16_t)c->power_cap_dbm10);
    put_u16(&out[11], (uint16_t)c->lr_cap_dbm10);
    return ZWZ_CFG_BLOB;
}

bool zwz_cfg_decode(zwz_cfg_t *c, const uint8_t *buf, size_t len)
{
    memset(c, 0, sizeof *c);
    if (buf == NULL || len < ZWZ_CFG_BLOB_MIN || buf[0] < FORMAT) {
        return false;
    }
    if (len >= ZWZ_CFG_BLOB) { /* the power caps (phase 3): absent, none yet */
        c->power_cap_set = (buf[8] & 0x01u) != 0u;
        c->lr_cap_set = (buf[8] & 0x02u) != 0u;
        c->power_cap_dbm10 = (int16_t)get_u16(&buf[9]);
        c->lr_cap_dbm10 = (int16_t)get_u16(&buf[11]);
    }
    c->region_set = (buf[1] & 0x01u) != 0u;
    c->s0_allowed = (buf[1] & 0x02u) != 0u;
    c->region = buf[2];
    c->jam_dbm = (int8_t)buf[3];
    if (c->jam_dbm < -100 || c->jam_dbm > -20) {
        c->jam_dbm = 0; /* none stored (older firmware writes 00), or nonsense: the default */
    }
    c->check_in_s = get_u32(&buf[4]);
    return true;
}

/* The six keys, in their stored order. */
static uint8_t *key_at(zw_s2_keys_t *k, unsigned i)
{
    uint8_t *const all[6] = {k->s2_unauth, k->s2_auth, k->s2_access, k->s0, k->lr_auth,
                             k->lr_access};
    return all[i];
}

size_t zwz_keys_encode(const zwz_keys_t *k, uint8_t *out, size_t cap)
{
    if (k == NULL || out == NULL || cap < ZWZ_KEYS_BLOB) {
        return 0u;
    }
    zw_s2_keys_t keys = k->keys;
    out[0] = FORMAT;
    out[1] = k->bound ? 1u : 0u;
    out[2] = 0u;
    out[3] = 0u;
    put_u32(&out[4], k->home_id);
    for (unsigned i = 0; i < 6u; i++) {
        memcpy(&out[8u + 16u * i], key_at(&keys, i), 16u);
    }
    zw_wipe(&keys, sizeof keys);
    return ZWZ_KEYS_BLOB;
}

bool zwz_keys_decode(zwz_keys_t *k, const uint8_t *buf, size_t len)
{
    memset(k, 0, sizeof *k);
    if (buf == NULL || len < ZWZ_KEYS_BLOB || buf[0] < FORMAT) {
        return false;
    }
    k->bound = buf[1] == 1u;
    k->home_id = get_u32(&buf[4]);
    for (unsigned i = 0; i < 6u; i++) {
        memcpy(key_at(&k->keys, i), &buf[8u + 16u * i], 16u);
    }
    return true;
}

size_t zwz_checkins_encode(const zwz_checkin_t *c, size_t n, uint8_t *out, size_t cap)
{
    if (out == NULL || n > ZWZ_PANEL_CHECKINS || (n > 0u && c == NULL) || cap < 2u + 8u * n) {
        return 0u;
    }
    out[0] = FORMAT;
    out[1] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        uint8_t *e = &out[2u + 8u * i];
        put_u16(&e[0], c[i].node);
        put_u16(&e[2], c[i].generation);
        put_u32(&e[4], c[i].seconds);
    }
    return 2u + 8u * n;
}

bool zwz_checkins_decode(zwz_checkin_t *c, size_t max, size_t *n, const uint8_t *buf, size_t len)
{
    *n = 0u;
    if (buf == NULL || len < 2u || buf[0] < FORMAT || len < 2u + 8u * (size_t)buf[1]) {
        return false;
    }
    const size_t count = buf[1];
    for (size_t i = 0; i < count && *n < max; i++) {
        const uint8_t *e = &buf[2u + 8u * i];
        c[*n].node = get_u16(&e[0]);
        c[*n].generation = get_u16(&e[2]);
        c[*n].seconds = get_u32(&e[4]);
        (*n)++;
    }
    return true;
}
