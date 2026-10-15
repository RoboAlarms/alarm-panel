/*
 * bind_gate.c - the reservation that keeps arming out of a sensor binding change
 * (zone_hub/zone_bind_gate.h, docs/features/22 ZSRC-012, review RF32).
 */
#include "zone_hub/zone_bind_gate.h"

#include <string.h>

void zone_bind_gate_init(zone_bind_gate_t *g)
{
    memset(g, 0, sizeof(*g));
}

bool zone_bind_gate_all_disarmed(const alarm_engine_t *e)
{
    const alarm_config_t *cfg = alarm_engine_config(e);
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!cfg->partitions[p - 1u].enabled) {
            continue;
        }
        alarm_partition_status_t ps;
        alarm_engine_partition_status(e, p, &ps);
        if (ps.state != ALARM_STATE_DISARMED) {
            return false;
        }
    }
    return true;
}

static void set_idle(zone_bind_gate_t *g)
{
    g->state = ZONE_BIND_GATE_IDLE;
    g->at = 0u;
    g->reported = false;
}

zone_bind_reserve_t zone_bind_gate_try(zone_bind_gate_t *g, bool all_disarmed, uint64_t now)
{
    if (!all_disarmed) {
        return ZONE_BIND_ARMED;
    }
    if (g->state != ZONE_BIND_GATE_IDLE) {
        return ZONE_BIND_BUSY;
    }
    g->state = ZONE_BIND_GATE_RESERVED;
    g->at = now;
    g->reported = false;
    return ZONE_BIND_RESERVED;
}

bool zone_bind_gate_take(zone_bind_gate_t *g, bool all_disarmed, uint64_t now)
{
    return zone_bind_gate_try(g, all_disarmed, now) == ZONE_BIND_RESERVED;
}

bool zone_bind_gate_reserve(zone_bind_gate_t *g, const alarm_engine_t *e, uint64_t now)
{
    return zone_bind_gate_take(g, zone_bind_gate_all_disarmed(e), now);
}

zone_bind_claim_t zone_bind_gate_claim(zone_bind_gate_t *g, uint64_t now)
{
    if (g->state != ZONE_BIND_GATE_RESERVED && g->state != ZONE_BIND_GATE_STEPPING) {
        return ZONE_BIND_CLAIM_REFUSED; /* cancelled by an arm, never made, or already claimed */
    }
    if (now >= g->at + ZONE_BIND_GATE_MS) {
        set_idle(g); /* too late: a step ending now finds it gone */
        return ZONE_BIND_CLAIM_REFUSED;
    }
    if (g->state == ZONE_BIND_GATE_STEPPING) {
        return ZONE_BIND_CLAIM_RETRY; /* the step running may be an arm */
    }
    g->state = ZONE_BIND_GATE_CHANGING;
    g->at = now;
    g->reported = false;
    return ZONE_BIND_CLAIMED;
}

void zone_bind_gate_end(zone_bind_gate_t *g)
{
    set_idle(g);
}

bool zone_bind_gate_step_begin(zone_bind_gate_t *g)
{
    if (g->state == ZONE_BIND_GATE_RESERVED) {
        g->state = ZONE_BIND_GATE_STEPPING;
    }
    return g->state == ZONE_BIND_GATE_CHANGING;
}

void zone_bind_gate_step_end(zone_bind_gate_t *g, bool all_disarmed)
{
    if (g->state != ZONE_BIND_GATE_STEPPING) {
        return; /* idle, ended, claimed before this step began, or reserved by this step */
    }
    if (all_disarmed) {
        g->state = ZONE_BIND_GATE_RESERVED;
    } else {
        set_idle(g); /* an arm went ahead: the changer's claim is refused */
    }
}

bool zone_bind_gate_stalled(zone_bind_gate_t *g, uint64_t now)
{
    if (g->state != ZONE_BIND_GATE_CHANGING || g->reported ||
        now < g->at + ZONE_BIND_GATE_STALL_MS) {
        return false;
    }
    g->reported = true;
    return true;
}

uint64_t zone_bind_gate_deadline(const zone_bind_gate_t *g)
{
    return g->state == ZONE_BIND_GATE_CHANGING && !g->reported ? g->at + ZONE_BIND_GATE_STALL_MS
                                                               : UINT64_MAX;
}
