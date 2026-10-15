/*
 * zw_outputs.c - what the panel's Z-Wave devices switch. See zw_outputs.h.
 */
#include "zwave_zones/zw_outputs.h"

#include <string.h>

#include "zwave_host/zw_iv.h"
#include "zwave_proto/zw_cc_switch.h"
#include "zwave_proto/zw_crc16.h"

void zwo_init(zwo_t *o, const zwo_port_t *port)
{
    memset(o, 0, sizeof(*o));
    if (port != NULL) {
        o->port = *port;
    }
}

static uint32_t next_tag(zwo_t *o)
{
    o->seq = (uint16_t)((unsigned)o->seq + 1u);
    if (o->seq == 0u) {
        o->seq = 1u;
    }
    return ZWO_TAG_BASE | (uint32_t)o->seq;
}

static void changed(zwo_t *o)
{
    if (o->port.changed != NULL) {
        o->port.changed(o->port.ctx);
    }
}

static void report(zwo_t *o, uint32_t cmd, zwo_result_t r)
{
    if (cmd != 0u && o->port.result != NULL) {
        o->port.result(o->port.ctx, cmd, r);
    }
}

static void clear_garages_of(zwo_t *o, size_t output)
{
    for (size_t g = 0; g < ZWO_GARAGES; g++) {
        if (o->garage[g].used && o->garage[g].output == output) {
            const uint32_t cmd = o->garage[g].cmd;
            memset(&o->garage[g], 0, sizeof o->garage[g]);
            report(o, cmd, ZWO_ERR_UNKNOWN);
        }
    }
}

static void drop(zwo_t *o, size_t i)
{
    const uint32_t cmd = o->out[i].cmd;
    clear_garages_of(o, i);
    memset(&o->out[i], 0, sizeof o->out[i]);
    report(o, cmd, ZWO_ERR_UNKNOWN);
}

void zwo_forget(zwo_t *o, uint16_t node)
{
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        if (o->out[i].used && o->out[i].node == node) {
            drop(o, i);
        }
    }
    changed(o);
}

/* The switch CC of one end point (0: the device itself), 0 none; Multilevel first. */
static uint8_t switch_cc(const zw_node_record_t *rec, uint8_t ep)
{
    if (zw_iv_endpoint_has_cc(rec, ep, (uint8_t)ZW_CC_MULTILEVEL_SWITCH)) {
        return (uint8_t)ZW_CC_MULTILEVEL_SWITCH;
    }
    if (zw_iv_endpoint_has_cc(rec, ep, (uint8_t)ZW_CC_BINARY_SWITCH)) {
        return (uint8_t)ZW_CC_BINARY_SWITCH;
    }
    return 0u;
}

size_t zwo_scan(zwo_t *o, const zw_node_record_t *rec)
{
    if (rec == NULL) {
        return 0u;
    }
    uint8_t cc[ZW_NODE_ENDPOINTS_MAX] = {0};
    bool endpoints = false;
    for (uint8_t ep = 1u; ep < ZW_NODE_ENDPOINTS_MAX; ep++) {
        if (rec->endpoints[ep].used) {
            cc[ep] = switch_cc(rec, ep);
            endpoints = endpoints || cc[ep] != 0u;
        }
    }
    if (!endpoints) {
        cc[0] = switch_cc(rec, 0u); /* the root only when no end point switches */
    }
    /* what the device no longer has (or another device under the same node ID) goes */
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        zwo_output_t *u = &o->out[i];
        if (u->used && u->node == rec->node_id &&
            (u->generation != rec->generation || u->endpoint >= ZW_NODE_ENDPOINTS_MAX ||
             cc[u->endpoint] != u->cc)) {
            drop(o, i);
        }
    }
    size_t n = 0u;
    for (uint8_t ep = 0u; ep < ZW_NODE_ENDPOINTS_MAX; ep++) {
        if (cc[ep] == 0u) {
            continue;
        }
        n++;
        if (zwo_find(o, rec->node_id, ep) >= 0) {
            continue; /* kept, with its level */
        }
        for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
            if (!o->out[i].used) {
                zwo_output_t *u = &o->out[i];
                memset(u, 0, sizeof(*u));
                u->used = true;
                u->node = rec->node_id;
                u->generation = rec->generation;
                u->endpoint = ep;
                u->cc = cc[ep];
                u->kind = cc[ep] == (uint8_t)ZW_CC_MULTILEVEL_SWITCH ? ZWO_DIMMER
                          : ep != 0u                                 ? ZWO_RELAY
                                                                     : ZWO_SWITCH;
                u->level = -1;
                break;
            }
        }
    }
    changed(o);
    return n;
}

int zwo_find(const zwo_t *o, uint16_t node, uint8_t endpoint)
{
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        if (o->out[i].used && o->out[i].node == node && o->out[i].endpoint == endpoint) {
            return (int)i;
        }
    }
    return -1;
}

static bool send_get(zwo_t *o, const zwo_output_t *u, uint32_t tag)
{
    uint8_t cmd[4];
    const size_t n = zw_cc_switch_get_build(cmd, sizeof cmd, u->cc);
    return n > 0u && o->port.send != NULL &&
           o->port.send(o->port.ctx, u->node, u->endpoint, cmd, n, tag);
}

static bool send_set(zwo_t *o, const zwo_output_t *u, int level, uint32_t tag)
{
    zw_cc_switch_set_t s;
    memset(&s, 0, sizeof s);
    if (u->cc == (uint8_t)ZW_CC_BINARY_SWITCH) {
        s.value = level > 0 ? ZW_SWITCH_ON : ZW_SWITCH_OFF;
    } else {
        s.value = (uint8_t)(level >= 99 ? ZW_SWITCH_LEVEL_MAX : (unsigned)level);
    }
    uint8_t cmd[6];
    const size_t n = zw_cc_switch_set_build(cmd, sizeof cmd, u->cc, &s);
    return n > 0u && o->port.send != NULL &&
           o->port.send(o->port.ctx, u->node, u->endpoint, cmd, n, tag);
}

void zwo_refresh(zwo_t *o, uint64_t now, uint16_t node)
{
    o->now = now;
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        zwo_output_t *u = &o->out[i];
        if (u->used && u->node == node && u->cmd == 0u) {
            (void)send_get(o, u, next_tag(o)); /* its answer is its Report */
        }
    }
}

static void finish(zwo_t *o, zwo_output_t *u, zwo_result_t r)
{
    const uint32_t cmd = u->cmd;
    u->cmd = 0u;
    u->set_out = false;
    u->get_due = false;
    u->tag = 0u;
    report(o, cmd, r);
    changed(o);
}

zwo_result_t zwo_set(zwo_t *o, uint64_t now, size_t index, int level, uint64_t issued_ms,
                     uint32_t cmd)
{
    o->now = now;
    if (index >= ZWO_OUTPUTS || !o->out[index].used) {
        return ZWO_ERR_UNKNOWN;
    }
    zwo_output_t *u = &o->out[index];
    if (u->garage) {
        return ZWO_ERR_ARG; /* a garage door moves through its garage, never as a switch */
    }
    if (issued_ms > now || now - issued_ms > ZWO_STALE_MS) {
        return ZWO_ERR_STALE;
    }
    if (u->cmd != 0u) {
        return ZWO_ERR_BUSY;
    }
    const bool binary = u->cc == (uint8_t)ZW_CC_BINARY_SWITCH;
    if (level < 0 || level > 100 || (binary && level != 0 && level != 100)) {
        return ZWO_ERR_ARG;
    }
    const uint32_t tag = next_tag(o);
    if (!send_set(o, u, level, tag)) {
        return ZWO_ERR_SEND;
    }
    u->cmd = cmd != 0u ? cmd : 0xFFFFFFFFu;
    u->target = (int8_t)level;
    u->set_out = true;
    u->tag = tag;
    u->deadline = now + ZWO_SET_MS;
    changed(o);
    return ZWO_OK;
}

static bool reached(const zwo_output_t *u, int level)
{
    if (level < 0) {
        return false;
    }
    if (u->cc == (uint8_t)ZW_CC_BINARY_SWITCH) {
        return (level > 0) == (u->target > 0);
    }
    return level == u->target || (u->target >= 99 && level >= 99);
}

bool zwo_received(zwo_t *o, uint64_t now, uint16_t node, uint8_t endpoint, const uint8_t *cmd,
                  size_t len)
{
    o->now = now;
    if (cmd == NULL || len < 2u ||
        (cmd[0] != (uint8_t)ZW_CC_BINARY_SWITCH && cmd[0] != (uint8_t)ZW_CC_MULTILEVEL_SWITCH)) {
        return false;
    }
    zw_cc_switch_report_t r;
    if (!zw_cc_switch_report_parse(cmd, len, cmd[0], &r)) {
        return cmd[1] == ZW_CMD_SWITCH_REPORT; /* a damaged Report is still no one else's */
    }
    const int i = zwo_find(o, node, endpoint);
    if (i < 0 || o->out[i].cc != cmd[0]) {
        return true; /* a root Report of a multi-relay (it mirrors an end point), or unknown */
    }
    zwo_output_t *u = &o->out[i];
    const int level = zw_cc_switch_percent(u->cc, r.current);
    const bool moved = u->level != level;
    u->level = (int8_t)level;
    u->heard_ms = now;
    if (u->garage && level > 0 && u->cmd == 0u) {
        bool moving = false;
        for (size_t g = 0; g < ZWO_GARAGES; g++) {
            moving = moving || (o->garage[g].used && o->garage[g].output == (uint8_t)i &&
                                o->garage[g].state != ZWO_G_IDLE);
        }
        if (!moving) {
            (void)send_set(o, u, 0, next_tag(o)); /* held on by nobody's press: let go */
        }
    }
    if (u->cmd != 0u && !u->set_out && reached(u, level)) {
        finish(o, u, ZWO_OK);
    } else if (u->cmd != 0u && reached(u, level)) {
        u->set_out = false; /* its Report beat the Set's own end */
        finish(o, u, ZWO_OK);
    } else if (moved) {
        changed(o);
    }
    return true;
}

static zwo_garage_t *garage_of_tag(zwo_t *o, uint32_t tag)
{
    for (size_t g = 0; g < ZWO_GARAGES; g++) {
        if (o->garage[g].used && o->garage[g].state != ZWO_G_IDLE && o->garage[g].tag == tag) {
            return &o->garage[g];
        }
    }
    return NULL;
}

static void garage_end(zwo_t *o, zwo_garage_t *g, zwo_result_t r)
{
    const uint32_t cmd = g->cmd;
    g->state = ZWO_G_IDLE;
    g->cmd = 0u;
    g->tag = 0u;
    g->on_tag = 0u;
    g->verifying = false;
    g->get_sent = false;
    g->unsure = 0u;
    report(o, cmd, r);
    changed(o);
}

/* One off to the relay. Only one the radio took is a try. One it refused (its queue full - perhaps
 * with the press's own on still in it - or its link down) is no try: it goes again ZWO_SET_MS
 * later, or at once when that on ends (zwo_send_done), at most ZWO_OFF_REFUSALS times - never
 * given up after a few seconds while the on may still go out after it (PL01). */
static void garage_release(zwo_t *o, zwo_garage_t *g)
{
    g->tag = next_tag(o);
    g->state = ZWO_G_RELEASE;
    g->verifying = false;
    g->get_sent = false;
    if (send_set(o, &o->out[g->output], 0, g->tag)) {
        g->tries++;
    } else {
        g->tag = 0u; /* nothing out: no end of it will come */
        g->refused++;
    }
    g->at = o->now + ZWO_SET_MS;
}

/* The off didn't take: again, or - its tries or refusals spent - the relay is stuck. */
static void garage_release_again(zwo_t *o, zwo_garage_t *g)
{
    if (g->tries >= ZWO_OFF_TRIES || g->refused >= ZWO_OFF_REFUSALS) {
        garage_end(o, g, ZWO_ERR_RELAY_STUCK);
    } else {
        garage_release(o, g);
    }
}

/* PL01: the press's on was queued but never confirmed (not acknowledged, or no answer at all). It
 * may have switched the relay all the same: released now, and the move ends `r` once the relay is
 * off (or stuck). The door may be moving: a new press waits out its travel. */
static void garage_unsure(zwo_t *o, zwo_garage_t *g, zwo_result_t r)
{
    g->unsure = (uint8_t)r;
    g->last_press = o->now;
    g->tries = 0u;
    g->refused = 0u;
    garage_release(o, g);
    changed(o);
}

/* The press's on, ended by the radio after its press timed out (PL01): the radio's queue has room
 * again - an off it refused goes now. Its outcome changes nothing: the move stays unsure. */
static bool garage_on_ended(zwo_t *o, uint32_t tag)
{
    for (size_t k = 0; k < ZWO_GARAGES; k++) {
        zwo_garage_t *g = &o->garage[k];
        if (g->used && g->state != ZWO_G_IDLE && g->on_tag == tag) {
            g->on_tag = 0u;
            if (g->state == ZWO_G_RELEASE && g->tag == 0u && g->refused < ZWO_OFF_REFUSALS) {
                garage_release(o, g);
            }
            return true;
        }
    }
    return false;
}

bool zwo_send_done(zwo_t *o, uint64_t now, uint32_t tag, bool delivered)
{
    o->now = now;
    if ((tag & ZWO_TAG_MASK) != ZWO_TAG_BASE) {
        return false;
    }
    zwo_garage_t *g = garage_of_tag(o, tag);
    if (g == NULL && garage_on_ended(o, tag)) {
        return true;
    }
    if (g != NULL) {
        if (g->state == ZWO_G_PRESS) {
            g->on_tag = 0u;
            if (!delivered) {
                garage_unsure(o, g, ZWO_ERR_SEND); /* unacknowledged is not "never got there" */
            } else {
                g->state = ZWO_G_HOLD;
                g->last_press = now;
                g->at = now + ZWO_PULSE_MS;
            }
        } else if (g->state == ZWO_G_RELEASE && !g->get_sent) {
            if (delivered) {
                g->verifying = true;
                g->at = now + ZWO_VERIFY_MS;
            } else {
                garage_release_again(o, g);
            }
        }
        return true;
    }
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        zwo_output_t *u = &o->out[i];
        if (!u->used || u->cmd == 0u || u->tag != tag) {
            continue;
        }
        if (!delivered) {
            finish(o, u, ZWO_ERR_SEND);
        } else if (u->set_out) {
            u->set_out = false;
            u->get_due = true; /* asked, unless its own Report comes first */
            u->get_at = now + ZWO_VERIFY_MS;
        }
        return true;
    }
    return true; /* a refresh's Get, or one whose command ended */
}

void zwo_poll(zwo_t *o, uint64_t now)
{
    o->now = now;
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        zwo_output_t *u = &o->out[i];
        if (!u->used || u->cmd == 0u) {
            continue;
        }
        if (now >= u->deadline) {
            finish(o, u, ZWO_ERR_TIMEOUT);
        } else if (u->get_due && now >= u->get_at) {
            u->get_due = false;
            u->tag = next_tag(o);
            (void)send_get(o, u, u->tag);
        }
    }
    for (size_t k = 0; k < ZWO_GARAGES; k++) {
        zwo_garage_t *g = &o->garage[k];
        if (!g->used || g->state == ZWO_G_IDLE) {
            continue;
        }
        const zwo_output_t *relay = &o->out[g->output];
        switch (g->state) {
        case ZWO_G_PRESS:
            if (now >= g->at) {
                garage_unsure(o, g, ZWO_ERR_TIMEOUT); /* PL01: no answer - released all the same */
            }
            break;
        case ZWO_G_HOLD:
            if (now >= g->at) {
                g->tries = 0u;
                g->refused = 0u;
                garage_release(o, g);
            }
            break;
        case ZWO_G_RELEASE:
            if (g->verifying && relay->level == 0 && relay->heard_ms >= g->last_press) {
                if (g->unsure != 0u) {
                    garage_end(o, g, (zwo_result_t)g->unsure); /* released; the press unknown */
                } else {
                    g->state = ZWO_G_TRAVEL; /* released: now the door */
                    g->at = now + (uint64_t)g->travel_s * 1000u;
                    changed(o);
                }
            } else if (g->verifying && now >= g->at && !g->get_sent) {
                g->get_sent = true;
                g->at = now + ZWO_VERIFY_MS;
                (void)send_get(o, relay, next_tag(o));
            } else if (now >= g->at) {
                garage_release_again(o, g);
            }
            break;
        case ZWO_G_TRAVEL: {
            const zwo_position_t pos =
                o->port.position != NULL ? o->port.position(o->port.ctx, g->zone) : ZWO_POS_UNKNOWN;
            if (pos == (zwo_position_t)g->goal) {
                g->last_press = 0u; /* it got there: a new press may go at once */
                garage_end(o, g, ZWO_OK);
            } else if (now >= g->at) {
                garage_end(o, g, ZWO_ERR_NO_MOVE);
            }
            break;
        }
        default: break;
        }
    }
}

uint64_t zwo_next_deadline(const zwo_t *o)
{
    uint64_t t = UINT64_MAX;
    for (size_t i = 0; i < ZWO_OUTPUTS; i++) {
        const zwo_output_t *u = &o->out[i];
        if (u->used && u->cmd != 0u) {
            const uint64_t d = u->get_due && u->get_at < u->deadline ? u->get_at : u->deadline;
            t = d < t ? d : t;
        }
    }
    for (size_t k = 0; k < ZWO_GARAGES; k++) {
        const zwo_garage_t *g = &o->garage[k];
        if (g->used && g->state != ZWO_G_IDLE) {
            /* the door is watched each second while it travels */
            const uint64_t d = g->state == ZWO_G_TRAVEL && o->now + 1000u < g->at ? o->now + 1000u
                                                                                 : g->at;
            t = d < t ? d : t;
        }
    }
    return t;
}

zwo_result_t zwo_garage_assign(zwo_t *o, size_t slot, size_t output, uint16_t zone,
                               uint16_t travel_s)
{
    if (slot >= ZWO_GARAGES || output >= ZWO_OUTPUTS || !o->out[output].used ||
        o->out[output].cc != (uint8_t)ZW_CC_BINARY_SWITCH || zone == 0u ||
        travel_s > ZWO_TRAVEL_S_MAX) {
        return ZWO_ERR_ARG;
    }
    if (o->out[output].cmd != 0u) {
        return ZWO_ERR_BUSY;
    }
    for (size_t g = 0; g < ZWO_GARAGES; g++) {
        if (g != slot && o->garage[g].used && o->garage[g].output == output) {
            return ZWO_ERR_BUSY; /* one garage a relay */
        }
    }
    if (o->garage[slot].used && o->garage[slot].state != ZWO_G_IDLE) {
        return ZWO_ERR_BUSY;
    }
    (void)zwo_garage_clear(o, slot); /* idle: checked above */
    zwo_garage_t *g = &o->garage[slot];
    g->used = true;
    g->output = (uint8_t)output;
    g->zone = zone;
    g->travel_s = (uint16_t)(travel_s != 0u ? (unsigned)travel_s : ZWO_TRAVEL_S_DEF);
    o->out[output].garage = true;
    changed(o);
    return ZWO_OK;
}

zwo_result_t zwo_garage_clear(zwo_t *o, size_t slot)
{
    if (slot >= ZWO_GARAGES || !o->garage[slot].used) {
        return ZWO_OK;
    }
    if (o->garage[slot].state != ZWO_G_IDLE) {
        return ZWO_ERR_BUSY; /* PL03: its move (and the release of its relay) runs to its end */
    }
    const size_t output = o->garage[slot].output;
    memset(&o->garage[slot], 0, sizeof o->garage[slot]);
    if (output < ZWO_OUTPUTS) {
        o->out[output].garage = false;
    }
    changed(o);
    return ZWO_OK;
}

zwo_result_t zwo_garage_move(zwo_t *o, uint64_t now, size_t slot, zwo_position_t goal,
                             uint64_t issued_ms, uint32_t cmd)
{
    o->now = now;
    if (slot >= ZWO_GARAGES || !o->garage[slot].used ||
        (goal != ZWO_POS_OPEN && goal != ZWO_POS_CLOSED)) {
        return ZWO_ERR_UNKNOWN;
    }
    zwo_garage_t *g = &o->garage[slot];
    zwo_output_t *relay = &o->out[g->output];
    if (issued_ms > now || now - issued_ms > ZWO_STALE_MS) {
        return ZWO_ERR_STALE;
    }
    if (g->state != ZWO_G_IDLE || relay->cmd != 0u ||
        (g->last_press != 0u && now - g->last_press < (uint64_t)g->travel_s * 1000u)) {
        return ZWO_ERR_BUSY; /* a second press would reverse it */
    }
    const zwo_position_t pos =
        o->port.position != NULL ? o->port.position(o->port.ctx, g->zone) : ZWO_POS_UNKNOWN;
    if (pos == goal) {
        return ZWO_ALREADY;
    }
    if (pos == ZWO_POS_UNKNOWN) {
        return ZWO_ERR_POSITION; /* never a blind press */
    }
    if (relay->level > 0) {
        return ZWO_ERR_RELAY_STUCK; /* still on: a press would make no edge */
    }
    g->tag = next_tag(o);
    if (!send_set(o, relay, 100, g->tag)) {
        return ZWO_ERR_SEND;
    }
    g->state = ZWO_G_PRESS;
    g->goal = (uint8_t)goal;
    g->moving_from = (uint8_t)pos;
    g->cmd = cmd != 0u ? cmd : 0xFFFFFFFFu;
    g->tries = 0u;
    g->refused = 0u;
    g->on_tag = g->tag;
    g->at = now + ZWO_SET_MS;
    changed(o);
    return ZWO_OK;
}

bool zwo_garage_moving(const zwo_t *o, size_t slot, zwo_position_t *goal)
{
    if (slot >= ZWO_GARAGES || !o->garage[slot].used || o->garage[slot].state == ZWO_G_IDLE) {
        return false;
    }
    if (goal != NULL) {
        *goal = (zwo_position_t)o->garage[slot].goal;
    }
    return true;
}

uint32_t zwo_garage_budget_ms(const zwo_t *o, size_t slot)
{
    if (slot >= ZWO_GARAGES || !o->garage[slot].used) {
        return 0u;
    }
    /* the press (its on's end, or ZWO_SET_MS without one) and its pulse; each off the radio took:
     * its end, then a wait for its Report and one for the Get's; each one it refused; the door */
    return ZWO_SET_MS + ZWO_PULSE_MS + ZWO_OFF_TRIES * (ZWO_SET_MS + 2u * ZWO_VERIFY_MS) +
           ZWO_OFF_REFUSALS * ZWO_SET_MS + (uint32_t)o->garage[slot].travel_s * 1000u +
           ZWO_BUDGET_SLACK_MS;
}

const char *zwo_end_word(zwo_result_t r)
{
    switch (r) {
    case ZWO_OK: return "";
    case ZWO_ERR_NO_MOVE: return "failed"; /* pressed; its sensor says the door didn't get there */
    default: return "timeout";
    }
}

const char *zwo_garage_state(const zwo_t *o, size_t slot)
{
    if (slot >= ZWO_GARAGES || !o->garage[slot].used) {
        return "unknown";
    }
    const zwo_garage_t *g = &o->garage[slot];
    const zwo_position_t pos =
        o->port.position != NULL ? o->port.position(o->port.ctx, g->zone) : ZWO_POS_UNKNOWN;
    return pos == ZWO_POS_OPEN ? "open" : pos == ZWO_POS_CLOSED ? "closed" : "unknown";
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
}

size_t zwo_garage_encode(const zwo_t *o, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < ZWO_GARAGE_BLOB) {
        return 0u;
    }
    memset(out, 0, ZWO_GARAGE_BLOB);
    out[0] = 'G';
    out[1] = 'A';
    out[2] = 1u; /* format */
    out[3] = (uint8_t)ZWO_GARAGES;
    for (size_t k = 0; k < ZWO_GARAGES; k++) {
        const zwo_garage_t *g = &o->garage[k];
        uint8_t *p = &out[4u + k * 10u];
        if (!g->used) {
            continue;
        }
        const zwo_output_t *u = &o->out[g->output];
        p[0] = 1u;
        put16(&p[1], u->node);
        put16(&p[3], u->generation);
        p[5] = u->endpoint;
        put16(&p[6], g->zone);
        put16(&p[8], g->travel_s);
    }
    put16(&out[ZWO_GARAGE_BLOB - 2u], zw_crc16(ZW_CRC16_INIT, out, ZWO_GARAGE_BLOB - 2u));
    return ZWO_GARAGE_BLOB;
}

bool zwo_garage_decode(zwo_t *o, const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < ZWO_GARAGE_BLOB || buf[0] != 'G' || buf[1] != 'A' || buf[2] != 1u ||
        buf[3] != ZWO_GARAGES ||
        get16(&buf[ZWO_GARAGE_BLOB - 2u]) != zw_crc16(ZW_CRC16_INIT, buf, ZWO_GARAGE_BLOB - 2u)) {
        return false;
    }
    for (size_t k = 0; k < ZWO_GARAGES; k++) {
        const uint8_t *p = &buf[4u + k * 10u];
        (void)zwo_garage_clear(o, k); /* a slot mid-move keeps its move (PL03) */
        if (p[0] != 1u) {
            continue;
        }
        const int i = zwo_find(o, get16(&p[1]), p[5]);
        if (i >= 0 && o->out[i].generation == get16(&p[3])) {
            (void)zwo_garage_assign(o, k, (size_t)i, get16(&p[6]), get16(&p[8]));
        } /* its relay is gone, or another device has its node ID now: dropped */
    }
    return true;
}
