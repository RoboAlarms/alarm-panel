/*
 * hub.c - the zone hub: sources, bindings, mapping, grace timers (see zone_hub.h).
 */
#include <string.h>

#include "hub_internal.h"

/* ---- small helpers --------------------------------------------------------------------------- */

static uint64_t add_ms(uint64_t t, uint64_t d)
{
    return (t > UINT64_MAX - d) ? UINT64_MAX : t + d;
}

static void post(zone_hub_t *h, uint16_t zone, alarm_zone_event_t ev)
{
    if (h->port.zone_event != NULL) {
        h->port.zone_event(h->port.ctx, zone, ev);
    }
}

static void changed(zone_hub_t *h)
{
    if (h->port.changed != NULL) {
        h->port.changed(h->port.ctx);
    }
}

static int find_source(const zone_hub_t *h, uint8_t kind)
{
    for (int i = 0; i < (int)h->n_sources; i++) {
        if (h->sources[i].ops->kind == kind) {
            return i;
        }
    }
    return -1;
}

static int find_addr(const zone_hub_t *h, const zsrc_address_t *a)
{
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (h->inputs[i].zone != 0u && zsrc_address_equal(&h->inputs[i].in.addr, a)) {
            return i;
        }
    }
    return -1;
}

int zh_find_role(const zone_hub_t *h, uint16_t zone, uint8_t role)
{
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (h->inputs[i].zone == zone && h->inputs[i].role == role) {
            return i;
        }
    }
    return -1;
}

static int free_slot(const zone_hub_t *h)
{
    return zh_find_role(h, 0u, 0u);
}

static bool link_down(const zone_hub_t *h, uint8_t kind)
{
    const int s = find_source(h, kind);
    return s >= 0 && h->sources[s].link_down;
}

static uint64_t grace_ms(const zone_hub_t *h, const zone_hub_input_t *in)
{
    uint32_t s = in->in.map.grace_s;
    if (s == 0u) {
        const int src = find_source(h, in->in.addr.kind);
        if (src >= 0) {
            const zone_source_ops_t *ops = h->sources[src].ops;
            s = ops->grace_of != NULL ? ops->grace_of(h->sources[src].ctx, &in->in.addr) : 0u;
            s = s != 0u ? s : (uint32_t)ops->grace_s; /* this sensor's, else the source's */
        }
    }
    if (s == 0u) {
        s = ZONE_HUB_GRACE_DEFAULT_S;
    }
    return (uint64_t)s * 1000u;
}

/* How long a source's whole link may be down before it is a trouble of its own. */
static uint64_t link_grace_ms(const zone_hub_source_t *s)
{
    if (s->urgent) {
        return 0u; /* zone_hub_link_now: the source's own rule already waited */
    }
    const uint32_t g = s->ops->link_grace_s != 0u ? s->ops->link_grace_s
                       : s->ops->grace_s != 0u    ? s->ops->grace_s
                                                  : (uint32_t)ZONE_HUB_GRACE_DEFAULT_S;
    return (uint64_t)g * 1000u;
}

/* Zones depend on this source. */
static bool source_used(const zone_hub_t *h, uint8_t kind)
{
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (h->inputs[i].zone != 0u && h->inputs[i].in.addr.kind == kind) {
            return true;
        }
    }
    return false;
}

/* Every sensor of the source has either reported since the link came back or shows its zone's
 * own CHECK: nothing of it is left looking ready on state from before the break. */
static bool source_covered(const zone_hub_t *h, uint8_t kind)
{
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        const zone_hub_input_t *in = &h->inputs[i];
        if (in->zone != 0u && in->in.addr.kind == kind && in->unreachable && !in->check) {
            return false;
        }
    }
    return true;
}

/* ZSRC-015: the source's own supervision. A link down longer than the source's grace, while
 * zones depend on it, is one trouble for the source (ZONE-008) instead of a CHECK per zone; it
 * stays until every sensor has proved itself again or its zone shows CHECK. */
static void supervise(zone_hub_t *h, uint8_t kind, uint64_t now)
{
    const int i = find_source(h, kind);
    if (i < 0) {
        return;
    }
    zone_hub_source_t *s = &h->sources[i];
    bool on;
    if (s->link_down) {
        on = source_used(h, kind) && (s->trouble || now >= add_ms(s->link_at, link_grace_ms(s)));
    } else {
        on = s->trouble && !source_covered(h, kind);
    }
    if (on != s->trouble) {
        s->trouble = on;
        if (h->port.source_trouble != NULL) {
            h->port.source_trouble(h->port.ctx, kind, on);
        }
    }
}

static void supervise_all(zone_hub_t *h, uint64_t now)
{
    for (uint8_t s = 0; s < h->n_sources; s++) {
        supervise(h, h->sources[s].ops->kind, now);
    }
}

/* ---- conditions and CHECK ---------------------------------------------------------------------- */

static alarm_zone_event_t cond_event(uint8_t bit, bool on)
{
    switch (bit) {
    case ZONE_HUB_COND_FAULT: return on ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE;
    case ZONE_HUB_COND_TROUBLE: return on ? ALARM_ZEV_TROUBLE : ALARM_ZEV_TROUBLE_RESTORE;
    case ZONE_HUB_COND_TAMPER: return on ? ALARM_ZEV_TAMPER : ALARM_ZEV_TAMPER_RESTORE;
    default: return on ? ALARM_ZEV_LOW_BATTERY : ALARM_ZEV_LOW_BATTERY_RESTORE;
    }
}

/* The condition an input's "active" stands for. */
static uint8_t role_cond(uint8_t role)
{
    switch (role) {
    case ZSRC_ROLE_TAMPER: return ZONE_HUB_COND_TAMPER;
    case ZSRC_ROLE_BATTERY: return ZONE_HUB_COND_LOWBAT;
    default: return ZONE_HUB_COND_FAULT;
    }
}

/* Tells the engine a condition of the zone, remembering it for resync and release. */
static void tell(zone_hub_t *h, zone_hub_input_t *in, uint8_t bit, bool on, uint64_t now)
{
    in->cond = on ? (uint8_t)(in->cond | bit) : (uint8_t)(in->cond & (uint8_t)~bit);
    post(h, in->zone, cond_event(bit, on));
    in->checkin_at = now;
}

/* The zone's stored binding couldn't be read: it has no sensor until the installer gives it one. */
static bool zone_lost(const zone_hub_t *h, uint16_t zone)
{
    const unsigned i = (unsigned)(zone - 1u);
    return zone >= 1u && zone <= ALARM_MAX_ZONES && (h->lost[i / 8u] & (1u << (i % 8u))) != 0u;
}

/* The installer gave the zone a sensor (or took it away): it is no longer simply lost. */
static void clear_lost(zone_hub_t *h, uint16_t zone)
{
    if (zone_lost(h, zone)) {
        const unsigned i = (unsigned)(zone - 1u);
        h->lost[i / 8u] = (uint8_t)(h->lost[i / 8u] & ~(1u << (i % 8u)));
        post(h, zone, ALARM_ZEV_REACHABLE);
    }
}

/* The zone shows CHECK while any of its inputs does. */
static bool zone_check(const zone_hub_t *h, uint16_t zone)
{
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (h->inputs[i].zone == zone && h->inputs[i].check) {
            return true;
        }
    }
    return false;
}

static void set_check(zone_hub_t *h, zone_hub_input_t *in, bool on)
{
    if (in->check == on) {
        return;
    }
    const bool before = zone_check(h, in->zone);
    in->check = on;
    if (zone_check(h, in->zone) != before) {
        post(h, in->zone, on ? ALARM_ZEV_UNREACHABLE : ALARM_ZEV_REACHABLE);
    }
}

static void mark_unreachable(zone_hub_input_t *in, uint64_t now)
{
    if (!in->unreachable) {
        in->unreachable = true;
        in->unreachable_at = now;
    }
}

/* A report from the sensor: it is there. */
static void mark_heard(zone_hub_t *h, zone_hub_input_t *in, uint64_t now)
{
    in->heard_at = now;
    in->unreachable = false;
    set_check(h, in, false);
}

/* ---- watching ---------------------------------------------------------------------------------- */

static void begin_watch(zone_hub_t *h, zone_hub_input_t *in, uint64_t now)
{
    const int s = find_source(h, in->in.addr.kind);
    if (s < 0) {
        mark_unreachable(in, now); /* no such source in this firmware: CHECK (ZSRC-010) */
        return;
    }
    const zone_source_ops_t *ops = h->sources[s].ops;
    in->watched = true; /* before watch(): the source may report at once */
    if (ops->watch != NULL && !ops->watch(h->sources[s].ctx, &in->in.addr, &in->in.map, now)) {
        in->watched = false;
        mark_unreachable(in, now); /* the source has no such sensor any more, or no room */
        return;
    }
    if (ops->group != NULL) {
        ops->group(h->sources[s].ctx, &in->in.addr, in->zone); /* the zone's inputs: one group */
    }
}

/* The kinds of source whose watched inputs a change released, as bits (RF31). */
static uint32_t kind_bit(uint8_t kind)
{
    return kind < 32u ? (uint32_t)1u << kind : 0u;
}

/* Sources of these kinds watch fewer inputs now: the inputs they had no room for are watched
 * again, in table order (RF31). An input whose source still refuses it stays as it was. */
static void rewatch_waiting(zone_hub_t *h, uint32_t kinds, uint64_t now)
{
    if (!h->started || kinds == 0u) {
        return;
    }
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        zone_hub_input_t *in = &h->inputs[i];
        if (in->zone != 0u && !in->watched && (kinds & kind_bit(in->in.addr.kind)) != 0u &&
            find_source(h, in->in.addr.kind) >= 0) {
            begin_watch(h, in, now);
        }
    }
}

static void end_watch(zone_hub_t *h, zone_hub_input_t *in)
{
    const int s = find_source(h, in->in.addr.kind);
    if (in->watched && s >= 0 && h->sources[s].ops->unwatch != NULL) {
        h->sources[s].ops->unwatch(h->sources[s].ctx, &in->in.addr);
    }
    in->watched = false;
}

/* The zone no longer gets anything from this input: undo what it told the engine. */
static void release(zone_hub_t *h, zone_hub_input_t *in)
{
    set_check(h, in, false);
    for (uint8_t bit = ZONE_HUB_COND_FAULT; bit <= ZONE_HUB_COND_LOWBAT; bit = (uint8_t)(bit << 1)) {
        if ((in->cond & bit) != 0u) {
            post(h, in->zone, cond_event(bit, false));
        }
    }
    end_watch(h, in);
    zh_keep_drop(h, in->zone, in->role);
    memset(in, 0, sizeof(*in));
}

/* ---- life cycle ---------------------------------------------------------------------------------- */

void zone_hub_init(zone_hub_t *h, const zone_hub_port_t *port)
{
    memset(h, 0, sizeof(*h));
    if (port != NULL) {
        h->port = *port;
    }
}

zone_hub_result_t zone_hub_register(zone_hub_t *h, const zone_source_ops_t *ops, void *ctx,
                                    uint64_t now)
{
    if (ops == NULL || ops->kind == ZSRC_NONE || find_source(h, ops->kind) >= 0) {
        return ZONE_HUB_ERR_ARG;
    }
    if (h->n_sources >= ZONE_HUB_MAX_SOURCES) {
        return ZONE_HUB_ERR_FULL;
    }
    h->sources[h->n_sources].ops = ops;
    h->sources[h->n_sources].ctx = ctx;
    /* A source that can't be used yet hasn't got its link up (Home Assistant before it
     * connects): assume down until it says otherwise, so a panel that restarts while its
     * bridge is away doesn't look ready with nothing behind its zones (ZSRC-015). */
    h->sources[h->n_sources].link_down = ops->usable != NULL && !ops->usable(ctx);
    h->sources[h->n_sources].trouble = false;
    h->sources[h->n_sources].link_at = now;
    h->n_sources++;
    if (h->started) {
        for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
            zone_hub_input_t *in = &h->inputs[i];
            if (in->zone != 0u && in->in.addr.kind == ops->kind && !in->watched) {
                in->unreachable = false; /* its source is back; a CHECK stays until it reports */
                begin_watch(h, in, now);
            }
        }
    }
    return ZONE_HUB_OK;
}

void zone_hub_start(zone_hub_t *h, uint64_t now)
{
    h->started = true;
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (h->inputs[i].zone != 0u && !h->inputs[i].watched) {
            begin_watch(h, &h->inputs[i], now);
        }
    }
}

void zone_hub_tick(zone_hub_t *h, uint64_t now)
{
    if (!h->started) {
        return;
    }
    for (uint8_t s = 0; s < h->n_sources; s++) {
        if (h->sources[s].ops->tick != NULL) {
            h->sources[s].ops->tick(h->sources[s].ctx, now);
        }
    }
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        zone_hub_input_t *in = &h->inputs[i];
        if (in->zone == 0u || !in->unreachable || in->check || link_down(h, in->in.addr.kind)) {
            continue;
        }
        if (now >= add_ms(in->unreachable_at, grace_ms(h, in))) {
            set_check(h, in, true);
        }
    }
    supervise_all(h, now); /* after the zones: a new CHECK can cover a source's own trouble */
}

uint64_t zone_hub_next_deadline(const zone_hub_t *h, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    if (!h->started) {
        return next;
    }
    for (uint8_t s = 0; s < h->n_sources; s++) {
        if (h->sources[s].ops->next_tick != NULL) {
            const uint64_t t = h->sources[s].ops->next_tick(h->sources[s].ctx, now);
            if (t < next) {
                next = t;
            }
        }
        const zone_hub_source_t *src = &h->sources[s];
        if (src->link_down && !src->trouble && source_used(h, src->ops->kind)) {
            const uint64_t t = add_ms(src->link_at, link_grace_ms(src)); /* ZSRC-015 */
            if (t < next) {
                next = t;
            }
        }
    }
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        const zone_hub_input_t *in = &h->inputs[i];
        if (in->zone == 0u || !in->unreachable || in->check || link_down(h, in->in.addr.kind)) {
            continue;
        }
        const uint64_t t = add_ms(in->unreachable_at, grace_ms(h, in));
        if (t < next) {
            next = t;
        }
    }
    return next;
}

/* ---- bindings ------------------------------------------------------------------------------------ */

/* Declared here, defined with the rest of the mapping below. */
static int map_report(const zsrc_mapping_t *m, bool known, bool raw_was, const char *state,
                      int32_t value);

/* What the sensor itself said, before 'invert' turned it into the zone's state. */
static void keep_raw(zone_hub_input_t *in)
{
    in->raw = in->in.map.invert ? !in->active : in->active;
}

/* A new mapping for a sensor that already reported: what it said may now mean the opposite
 * (a contact changed from normally closed to normally open, a new threshold). Works from the
 * sensor's own sense (in->raw), so a source that reports afresh from inside watch() - already
 * under the new mapping - is not inverted a second time (ZSRC-006). */
static void remap(zone_hub_t *h, zone_hub_input_t *in, const zsrc_mapping_t *was, uint64_t now)
{
    if (!in->known) {
        return;
    }
    int on = -1;
    if (in->has_value) {
        on = map_report(&in->in.map, false, false, NULL, in->value_x100); /* decide afresh */
    } else if (was->invert != in->in.map.invert) {
        on = in->in.map.invert ? !in->raw : in->raw;
    }
    if (on < 0) {
        return;
    }
    const bool differs = (on != 0) != in->active;
    in->active = on != 0;
    keep_raw(in);
    if (differs) {
        tell(h, in, role_cond(in->role), in->active, now);
    }
}

static bool text_ok(const char *s, size_t size)
{
    return memchr(s, '\0', size) != NULL;
}

static bool mapping_ok(const zsrc_mapping_t *m)
{
    return m->mode < ZSRC_MAP_COUNT && m->hysteresis_x100 >= 0 &&
           text_ok(m->states, sizeof(m->states));
}

static bool used(const zsrc_input_t *in)
{
    return in->addr.kind != ZSRC_NONE;
}

static zone_hub_result_t check_binding(const zone_hub_t *h, uint16_t zone,
                                       const zone_hub_binding_t *b)
{
    if (zone < 1u || zone > ALARM_MAX_ZONES || b == NULL || !used(&b->in[ZSRC_ROLE_MAIN])) {
        return ZONE_HUB_ERR_ARG;
    }
    int needed = 0;
    for (uint8_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
        const zsrc_input_t *in = &b->in[r];
        if (!used(in)) {
            continue;
        }
        if (!text_ok(in->addr.ref, sizeof(in->addr.ref)) || !mapping_ok(&in->map)) {
            return ZONE_HUB_ERR_ARG;
        }
        for (uint8_t o = 0; o < r; o++) {
            if (used(&b->in[o]) && zsrc_address_equal(&b->in[o].addr, &in->addr)) {
                return ZONE_HUB_ERR_ARG; /* one sensor, one role */
            }
        }
        const int s = find_source(h, in->addr.kind);
        if (s < 0) {
            return ZONE_HUB_ERR_NO_SOURCE;
        }
        const zone_source_ops_t *ops = h->sources[s].ops;
        if (ops->valid != NULL && !ops->valid(h->sources[s].ctx, &in->addr)) {
            return ZONE_HUB_ERR_REFUSED;
        }
        const int at = find_addr(h, &in->addr);
        if (at >= 0 && h->inputs[at].zone != zone) {
            return ZONE_HUB_ERR_IN_USE;
        }
        const int old = zh_find_role(h, zone, r);
        if (old < 0 || !zsrc_address_equal(&h->inputs[old].in.addr, &in->addr)) {
            needed++;
        }
    }
    int free_n = 0;
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (h->inputs[i].zone == 0u) {
            free_n++;
        } else if (h->inputs[i].zone == zone) {
            const uint8_t r = h->inputs[i].role;
            if (r < ZSRC_ROLE_COUNT &&
                (!used(&b->in[r]) || !zsrc_address_equal(&h->inputs[i].in.addr, &b->in[r].addr))) {
                free_n++; /* released before the new inputs are placed */
            }
        }
    }
    if (needed > free_n) {
        return ZONE_HUB_ERR_FULL;
    }
    /* A source with a table of its own (RF's watch slots): room for each new input, counting
     * the ones of this zone it watches now and the binding releases (RF31). Only once started:
     * bindings loaded at start are kept whatever the source's room, and wait for a slot. */
    for (uint8_t s = 0; h->started && s < h->n_sources; s++) {
        const zone_source_ops_t *ops = h->sources[s].ops;
        if (ops->room == NULL) {
            continue;
        }
        size_t want = 0;
        size_t waiting = 0; /* kept inputs still without a slot: retried first, before new ones */
        size_t freed = 0;
        for (uint8_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
            const zsrc_input_t *in = &b->in[r];
            if (!used(in) || in->addr.kind != ops->kind) {
                continue;
            }
            const int old = zh_find_role(h, zone, r);
            if (old < 0 || !zsrc_address_equal(&h->inputs[old].in.addr, &in->addr)) {
                want++;
            } else if (!h->inputs[old].watched) {
                waiting++;
            }
        }
        for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
            const zone_hub_input_t *in = &h->inputs[i];
            const uint8_t r = in->role;
            if (in->zone == zone && in->watched && in->in.addr.kind == ops->kind &&
                (r >= ZSRC_ROLE_COUNT || !used(&b->in[r]) ||
                 !zsrc_address_equal(&in->in.addr, &b->in[r].addr))) {
                freed++;
            }
        }
        if (want > 0u && want + waiting > ops->room(h->sources[s].ctx) + freed) {
            return ZONE_HUB_ERR_FULL;
        }
    }
    return ZONE_HUB_OK;
}

zone_hub_result_t zone_hub_bind(zone_hub_t *h, uint16_t zone, const zone_hub_binding_t *b,
                                uint64_t now)
{
    const zone_hub_result_t r = check_binding(h, zone, b);
    if (r != ZONE_HUB_OK) {
        return r;
    }
    clear_lost(h, zone);
    zh_keep_drop(h, zone, ZH_ANY_ROLE); /* the installer replaced it: newer items go too */
    /* Inputs that stay keep their state; the others are released first. */
    uint32_t freed = 0;
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        zone_hub_input_t *in = &h->inputs[i];
        if (in->zone != zone) {
            continue;
        }
        const uint8_t role = in->role;
        if (role >= ZSRC_ROLE_COUNT || !used(&b->in[role]) ||
            !zsrc_address_equal(&in->in.addr, &b->in[role].addr)) {
            if (in->watched) {
                freed |= kind_bit(in->in.addr.kind);
            }
            release(h, in);
        }
    }
    for (uint8_t role = 0; role < ZSRC_ROLE_COUNT; role++) {
        const zsrc_input_t *want = &b->in[role];
        if (!used(want)) {
            continue;
        }
        const int at = zh_find_role(h, zone, role);
        if (at >= 0) {
            zone_hub_input_t *in = &h->inputs[at];
            if (memcmp(&in->in.map, &want->map, sizeof(want->map)) != 0) {
                const zsrc_mapping_t was = in->in.map;
                in->in.map = want->map;
                if (h->started) {
                    begin_watch(h, in, now); /* the source takes the new mapping */
                }
                remap(h, in, &was, now); /* the same state may mean the opposite now */
            } else if (h->started && !in->watched) {
                begin_watch(h, in, now); /* saved again: its source may have room now (RF31) */
            }
            continue;
        }
        const int slot = free_slot(h);
        if (slot < 0) {
            break; /* can't happen: check_binding counted the room */
        }
        zone_hub_input_t *in = &h->inputs[slot];
        memset(in, 0, sizeof(*in));
        in->zone = zone;
        in->role = role;
        in->in = *want;
        if (h->started) {
            begin_watch(h, in, now);
        }
    }
    rewatch_waiting(h, freed, now); /* after the new inputs: the room was counted for them */
    supervise_all(h, now); /* a zone more, or one less, on a source whose link is down */
    changed(h);
    return ZONE_HUB_OK;
}

void zone_hub_unbind(zone_hub_t *h, uint16_t zone, uint64_t now)
{
    clear_lost(h, zone); /* deliberately without a sensor: no CHECK */
    bool any = false;
    uint32_t freed = 0;
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        if (zone != 0u && h->inputs[i].zone == zone) {
            if (h->inputs[i].watched) {
                freed |= kind_bit(h->inputs[i].in.addr.kind);
            }
            release(h, &h->inputs[i]);
            any = true;
        }
    }
    rewatch_waiting(h, freed, now); /* RF31 */
    const uint16_t before = h->keep_len;
    zh_keep_drop(h, zone, ZH_ANY_ROLE);
    supervise_all(h, now);
    if (any || h->keep_len != before) {
        changed(h);
    }
}

bool zone_hub_get(const zone_hub_t *h, uint16_t zone, zone_hub_binding_t *out)
{
    memset(out, 0, sizeof(*out));
    bool found = false;
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        const zone_hub_input_t *in = &h->inputs[i];
        if (zone != 0u && in->zone == zone && in->role < ZSRC_ROLE_COUNT) {
            out->in[in->role] = in->in;
            found = true;
        }
    }
    return found;
}

uint16_t zone_hub_zone_of(const zone_hub_t *h, const zsrc_address_t *a)
{
    const int i = find_addr(h, a);
    return i >= 0 ? h->inputs[i].zone : 0u;
}

void zone_hub_resync(zone_hub_t *h, const uint8_t *zones, uint64_t now)
{
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        zone_hub_input_t *in = &h->inputs[i];
        const uint16_t z = in->zone;
        if (z == 0u || (zones[(z - 1u) / 8u] & (1u << ((z - 1u) % 8u))) == 0u) {
            continue;
        }
        const uint8_t own = role_cond(in->role);
        if (in->known && (in->cond & own) == 0u) {
            tell(h, in, own, false, now); /* clears a state the engine may have kept */
        }
        for (uint8_t bit = ZONE_HUB_COND_FAULT; bit <= ZONE_HUB_COND_LOWBAT;
             bit = (uint8_t)(bit << 1)) {
            if ((in->cond & bit) != 0u) {
                tell(h, in, bit, true, now);
            }
        }
    }
    /* The engine dropped its CHECKs with the rest: raise them again, once per zone. */
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if ((zones[(z - 1u) / 8u] & (1u << ((z - 1u) % 8u))) != 0u &&
            (zone_check(h, z) || zone_lost(h, z))) {
            post(h, z, ALARM_ZEV_UNREACHABLE);
        }
    }
}

void zone_hub_lost(zone_hub_t *h, const uint8_t *zones, uint64_t now)
{
    (void)now;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        const unsigned i = (unsigned)(z - 1u);
        if ((zones[i / 8u] & (1u << (i % 8u))) != 0u && !zone_lost(h, z)) {
            h->lost[i / 8u] = (uint8_t)(h->lost[i / 8u] | (1u << (i % 8u)));
            post(h, z, ALARM_ZEV_UNREACHABLE);
        }
    }
}

/* ---- what sources report ------------------------------------------------------------------------- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* state is one of the comma-separated words in list (ASCII case and spaces around words
 * ignored). */
static bool word_in(const char *state, const char *list)
{
    while (*state == ' ') {
        state++;
    }
    size_t n = strlen(state);
    while (n > 0u && state[n - 1u] == ' ') {
        n--;
    }
    if (n == 0u) {
        return false;
    }
    const char *p = list;
    while (*p != '\0') {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char *w = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        size_t wn = (size_t)(p - w);
        while (wn > 0u && w[wn - 1u] == ' ') {
            wn--;
        }
        if (wn == n) {
            size_t k = 0;
            while (k < n && lower(w[k]) == lower(state[k])) {
                k++;
            }
            if (k == n) {
                return true;
            }
        }
    }
    return false;
}

/* "21.5" -> 2150, "-3" -> -300; digits beyond hundredths are cut off. */
static bool parse_x100(const char *s, int32_t *out)
{
    while (*s == ' ') {
        s++;
    }
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
    }
    int64_t v = 0;
    int digits = 0;
    int frac = -1; /* digits after the point, -1 = no point yet */
    for (; *s != '\0' && *s != ' '; s++) {
        if (*s == '.' && frac < 0) {
            frac = 0;
            continue;
        }
        if (*s < '0' || *s > '9') {
            return false;
        }
        if (frac >= 2) {
            continue;
        }
        v = v * 10 + (*s - '0');
        digits++;
        if (frac >= 0) {
            frac++;
        }
        if (v > 2000000000) {
            return false;
        }
    }
    if (digits == 0) {
        return false;
    }
    for (int f = frac < 0 ? 0 : frac; f < 2; f++) {
        v *= 10;
    }
    if (v > INT32_MAX) {
        return false;
    }
    *out = (int32_t)(neg ? -v : v);
    return true;
}

/* 1 active, 0 inactive, -1 the report says nothing this mapping understands. raw_was: the last
 * state before 'invert' (hysteresis works on the sensor's own sense). */
static int map_report(const zsrc_mapping_t *m, bool known, bool raw_was, const char *state,
                      int32_t value)
{
    int on = -1;
    const bool text = state != NULL && state[0] != '\0';
    switch (m->mode) {
    case ZSRC_MAP_BINARY:
        if (!text) {
            on = value != 0;
        } else if (word_in(state, "on,open,true,1")) {
            on = 1;
        } else if (word_in(state, "off,closed,false,0")) {
            on = 0;
        }
        break;
    case ZSRC_MAP_ABOVE: {
        const int64_t limit = (known && raw_was)
                                  ? (int64_t)m->threshold_x100 - m->hysteresis_x100
                                  : (int64_t)m->threshold_x100;
        on = (int64_t)value >= limit;
        break;
    }
    case ZSRC_MAP_BELOW: {
        const int64_t limit = (known && raw_was)
                                  ? (int64_t)m->threshold_x100 + m->hysteresis_x100
                                  : (int64_t)m->threshold_x100;
        on = (int64_t)value <= limit;
        break;
    }
    case ZSRC_MAP_STATES:
        if (text) {
            on = word_in(state, m->states);
        }
        break;
    default: break;
    }
    if (on >= 0 && m->invert) {
        on = !on;
    }
    return on;
}

bool zone_hub_input(zone_hub_t *h, const zsrc_address_t *a, const char *state,
                    int32_t value_x100, uint64_t now)
{
    const int i = find_addr(h, a);
    if (i < 0) {
        return false;
    }
    zone_hub_input_t *in = &h->inputs[i];
    const zsrc_mapping_t *m = &in->in.map;
    const bool numeric = m->mode == ZSRC_MAP_ABOVE || m->mode == ZSRC_MAP_BELOW;
    int32_t value = value_x100;
    if (numeric && state != NULL && state[0] != '\0' && !parse_x100(state, &value)) {
        return true; /* "unknown" and the like: says nothing */
    }
    const int on = map_report(m, in->known, in->raw, state, value);
    if (on < 0) {
        return true;
    }
    mark_heard(h, in, now);
    if (numeric || state == NULL || state[0] == '\0') {
        in->has_value = true;
        in->value_x100 = value;
    }
    const bool first = !in->known;
    in->known = true;
    in->active = on != 0;
    keep_raw(in);
    const uint8_t bit = role_cond(in->role);
    if (first || ((in->cond & bit) != 0u) != in->active) {
        tell(h, in, bit, in->active, now);
    } else if (in->role == ZSRC_ROLE_MAIN && now - in->checkin_at >= ZONE_HUB_CHECKIN_EVERY_MS) {
        /* Unchanged, but the sensor is alive. Only the zone's own sensor may keep the engine's
         * supervision timer (supervision_s) alive: a linked battery entity must not. */
        post(h, in->zone, ALARM_ZEV_CHECKIN);
        in->checkin_at = now;
    }
    supervise(h, in->in.addr.kind, now); /* its source may be whole again (ZSRC-015) */
    return true;
}

bool zone_hub_event(zone_hub_t *h, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    if (ev == ALARM_ZEV_UNREACHABLE || ev == ALARM_ZEV_REACHABLE) {
        return zone_hub_reachable(h, a, ev == ALARM_ZEV_REACHABLE, now);
    }
    const int i = find_addr(h, a);
    if (i < 0) {
        return false;
    }
    zone_hub_input_t *in = &h->inputs[i];
    mark_heard(h, in, now);
    switch (ev) {
    case ALARM_ZEV_FAULT:
    case ALARM_ZEV_RESTORE:
        in->known = true;
        in->active = ev == ALARM_ZEV_FAULT;
        keep_raw(in); /* the source said what the zone shows: 'invert' hasn't touched it */
        tell(h, in, role_cond(in->role), in->active, now);
        break;
    case ALARM_ZEV_TROUBLE:
    case ALARM_ZEV_TROUBLE_RESTORE:
        tell(h, in, ZONE_HUB_COND_TROUBLE, ev == ALARM_ZEV_TROUBLE, now);
        break;
    case ALARM_ZEV_TAMPER:
    case ALARM_ZEV_TAMPER_RESTORE:
        tell(h, in, ZONE_HUB_COND_TAMPER, ev == ALARM_ZEV_TAMPER, now);
        break;
    case ALARM_ZEV_LOW_BATTERY:
    case ALARM_ZEV_LOW_BATTERY_RESTORE:
        tell(h, in, ZONE_HUB_COND_LOWBAT, ev == ALARM_ZEV_LOW_BATTERY, now);
        break;
    case ALARM_ZEV_TEST:
        /* The sensor's own test (an IAS device in test mode, WALK-007): the engine marks it in a
         * walk test and counts it as a check-in otherwise. Only the zone's own sensor. */
        if (in->role == ZSRC_ROLE_MAIN) {
            post(h, in->zone, ALARM_ZEV_TEST);
            in->checkin_at = now;
        }
        break;
    default:
        if (in->role == ZSRC_ROLE_MAIN) { /* only the zone's own sensor checks in for it */
            post(h, in->zone, ALARM_ZEV_CHECKIN);
            in->checkin_at = now;
        }
        break;
    }
    supervise(h, in->in.addr.kind, now); /* its source may be whole again (ZSRC-015) */
    return true;
}

bool zone_hub_reachable(zone_hub_t *h, const zsrc_address_t *a, bool reachable, uint64_t now)
{
    const int i = find_addr(h, a);
    if (i < 0) {
        return false;
    }
    zone_hub_input_t *in = &h->inputs[i];
    if (reachable) {
        mark_heard(h, in, now);
    } else {
        mark_unreachable(in, now);
    }
    supervise(h, in->in.addr.kind, now);
    return true;
}

void zone_hub_link(zone_hub_t *h, uint8_t kind, bool up, uint64_t now)
{
    const int s = find_source(h, kind);
    if (s < 0 || h->sources[s].link_down == !up) {
        return;
    }
    h->sources[s].link_down = !up;
    h->sources[s].link_at = now;
    h->sources[s].urgent = false;
    if (up) {
        for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
            zone_hub_input_t *in = &h->inputs[i];
            if (in->zone != 0u && in->in.addr.kind == kind && !in->check) {
                /* ZSRC-015: what the source said before the break is old news. Every sensor
                 * has the grace to report again; the ones that don't show CHECK, and the
                 * source's own trouble stays until then. */
                in->unreachable = true;
                in->unreachable_at = now;
            }
        }
    }
    supervise(h, kind, now);
}

void zone_hub_link_now(zone_hub_t *h, uint8_t kind, uint64_t now)
{
    const int s = find_source(h, kind);
    if (s < 0) {
        return;
    }
    if (!h->sources[s].link_down) {
        h->sources[s].link_down = true;
        h->sources[s].link_at = now;
    }
    h->sources[s].urgent = true; /* also when the link was already down for another reason */
    supervise(h, kind, now);
}

/* ---- queries ------------------------------------------------------------------------------------- */

size_t zone_hub_sources(const zone_hub_t *h, zone_hub_source_info_t *out, size_t max)
{
    size_t n = 0;
    for (uint8_t s = 0; s < h->n_sources && n < max; s++) {
        const zone_source_ops_t *ops = h->sources[s].ops;
        out[n].kind = ops->kind;
        out[n].caps = ops->caps;
        out[n].name = ops->name;
        out[n].usable = ops->usable == NULL || ops->usable(h->sources[s].ctx);
        out[n].link_down = h->sources[s].link_down;
        out[n].trouble = h->sources[s].trouble;
        n++;
    }
    return n;
}

const zone_source_ops_t *zone_hub_source_ops(const zone_hub_t *h, uint8_t kind)
{
    const int s = find_source(h, kind);
    return s >= 0 ? h->sources[s].ops : NULL;
}

bool zone_hub_info(const zone_hub_t *h, uint16_t zone, zone_hub_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->lost = zone_lost(h, zone);
    if (!zone_hub_get(h, zone, &out->binding)) {
        out->check = out->lost;
        return out->lost;
    }
    for (int i = 0; i < ZONE_HUB_MAX_INPUTS; i++) {
        const zone_hub_input_t *in = &h->inputs[i];
        if (in->zone == zone) {
            out->unreachable = out->unreachable || in->unreachable;
            out->check = out->check || in->check;
        }
    }
    const int m = zh_find_role(h, zone, ZSRC_ROLE_MAIN);
    if (m < 0) {
        return true;
    }
    const zone_hub_input_t *in = &h->inputs[m];
    out->known = in->known;
    out->active = in->active;
    out->has_value = in->has_value;
    out->value_x100 = in->value_x100;
    out->heard_at = in->heard_at;
    const int s = find_source(h, in->in.addr.kind);
    if (s >= 0) {
        const zone_source_ops_t *ops = h->sources[s].ops;
        out->source_present = true;
        out->caps =
            ops->caps_of != NULL ? ops->caps_of(h->sources[s].ctx, &in->in.addr) : ops->caps;
        out->link_down = h->sources[s].link_down;
        if (ops->describe != NULL) {
            ops->describe(h->sources[s].ctx, &in->in.addr, out->address, sizeof(out->address));
        }
    }
    return true;
}

bool zone_hub_learn(zone_hub_t *h, uint8_t kind, bool on, uint64_t now)
{
    const int s = find_source(h, kind);
    if (s < 0 || h->sources[s].ops->learn == NULL) {
        return false;
    }
    return h->sources[s].ops->learn(h->sources[s].ctx, on, now);
}

size_t zone_hub_candidates(zone_hub_t *h, uint8_t kind, const char *query, uint32_t filter,
                           zsrc_candidate_t *out, size_t max)
{
    const int s = find_source(h, kind);
    if (s < 0 || h->sources[s].ops->candidates == NULL || max == 0u) {
        return 0u;
    }
    size_t n = h->sources[s].ops->candidates(h->sources[s].ctx, query, filter, out, max);
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out[i].zone = zone_hub_zone_of(h, &out[i].addr);
    }
    return n;
}
