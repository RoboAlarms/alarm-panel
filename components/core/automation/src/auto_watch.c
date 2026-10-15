/* The observer: engine events and snapshots to occurrences, on the alarm task (docs/features/41
 * DESIGN.md section 2, AUT-003, AUT-004). */
#include "automation/auto_watch.h"

#include <string.h>

#include "automation/automation.h"

#define EP_ALARM 0x01u /* the ALARM trigger fired this episode */
#define EP_FIRE  0x02u /* the FIRE trigger fired this episode */

/* B5: a zone edge next to these is a sensor's trouble, not a door. */
#define BAD_FLAGS (ALARM_ZS_TROUBLE | ALARM_ZS_TAMPER | ALARM_ZS_SUPERVISION)

static bool bit_get(const uint8_t *bits, unsigned i)
{
    return ((unsigned)bits[i >> 3] & (1u << (i & 7u))) != 0u;
}

static void bit_put(uint8_t *bits, unsigned i, bool on)
{
    const uint8_t m = (uint8_t)(1u << (i & 7u));
    bits[i >> 3] = on ? (uint8_t)(bits[i >> 3] | m) : (uint8_t)(bits[i >> 3] & (uint8_t)~m);
}

/* B6: write an occurrence if there is room, else count it. */
static void put(auto_watch_t *w, auto_occ_t *out, size_t max, size_t *n, const auto_occ_t *o)
{
    if (out != NULL && *n < max) {
        out[*n] = *o;
        (*n)++;
    } else {
        w->missed++;
    }
}

void auto_watch_init(auto_watch_t *w, alarm_ms_t now, uint32_t settle_ms)
{
    if (w == NULL) {
        return;
    }
    memset(w, 0, sizeof(*w));
    w->settle_at = now + settle_ms;
}

/* The trigger an alarm cause may start, AUTO_TRIG_NONE for every other cause (silent ones,
 * keypad-only ones and anything newer than this list). */
static uint8_t alarm_kind(uint8_t cause)
{
    switch (cause) {
    case ALARM_CAUSE_BURGLARY:
    case ALARM_CAUSE_EXIT_ERROR:
    case ALARM_CAUSE_TAMPER:
    case ALARM_CAUSE_24H_AUDIBLE:
    case ALARM_CAUSE_PANIC_POLICE:
        return AUTO_TRIG_ALARM;
    case ALARM_CAUSE_FIRE:
    case ALARM_CAUSE_PANIC_FIRE:
    case ALARM_CAUSE_CO:
        return AUTO_TRIG_FIRE;
    default:
        return AUTO_TRIG_NONE;
    }
}

size_t auto_watch_event(auto_watch_t *w, const alarm_event_t *ev, auto_occ_t *out, size_t max)
{
    size_t n = 0;
    if (w == NULL || ev == NULL) {
        return 0;
    }
    if ((ev->type == ALARM_EV_DISARMED || ev->type == ALARM_EV_ALARM) &&
        (ev->partition < 1u || ev->partition > ALARM_MAX_PARTITIONS)) {
        return 0; /* no partition to name or to latch: not an occurrence, nothing counted */
    }
    auto_occ_t o;
    memset(&o, 0, sizeof(o));
    o.partition = ev->partition;
    o.at = ev->mono_ms;
    switch (ev->type) {
    case ALARM_EV_DISARMED:
        o.kind = AUTO_TRIG_DISARMED;
        o.by_auto = ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION ? 1u : 0u;
        put(w, out, max, &n, &o);
        break;
    case ALARM_EV_ALARM: {
        if ((ev->flags & ALARM_EVF_SILENT) != 0u) {
            break; /* a silent alarm never shows itself */
        }
        const uint8_t kind = alarm_kind(ev->arg);
        if (kind == AUTO_TRIG_NONE) {
            break;
        }
        const uint8_t ep = kind == AUTO_TRIG_FIRE ? EP_FIRE : EP_ALARM;
        uint8_t *latch = &w->episode[ev->partition - 1u];
        if ((*latch & ep) != 0u) {
            break; /* once per episode */
        }
        *latch = (uint8_t)(*latch | ep);
        o.kind = kind;
        o.cause = ev->arg;
        o.zone = ev->zone;
        put(w, out, max, &n, &o);
        break;
    }
    default:
        break; /* ALARM_EV_DURESS and the rest are never occurrences */
    }
    return n;
}

static bool sounding(alarm_sound_t s)
{
    return s == ALARM_SOUND_ALARM || s == ALARM_SOUND_FIRE || s == ALARM_SOUND_CO ||
           s == ALARM_SOUND_AUX;
}

size_t auto_watch_snapshot(auto_watch_t *w, const alarm_snapshot_t *s, auto_occ_t *out,
                           size_t max)
{
    size_t n = 0;
    if (w == NULL || s == NULL) {
        return 0;
    }
    const bool first = w->based == 0u;

    /* Partitions: the armed latch, the alarm episode latch. */
    bool walking = false;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_status_t *p = &s->part[i];
        const uint8_t state = (uint8_t)p->state;
        const uint8_t level = (uint8_t)p->level;
        if ((s->partitions & (1u << i)) != 0u && p->walk_test) {
            walking = true;
        }
        if (state == ALARM_STATE_DISARMED) {
            w->armed_level[i] = 0;
            if (!sounding(p->sounder)) {
                w->episode[i] = 0;
            }
        } else if (state == ALARM_STATE_ARMED) {
            const uint8_t prev = w->prev_state[i];
            if (first) {
                w->armed_level[i] = level; /* B1: restored at power-up */
            } else if ((prev == ALARM_STATE_DISARMED || prev == ALARM_STATE_EXIT_DELAY) &&
                       w->armed_level[i] != level) {
                w->armed_level[i] = level;
                auto_occ_t o;
                memset(&o, 0, sizeof(o));
                o.kind = AUTO_TRIG_ARMED;
                o.partition = (uint8_t)(i + 1u);
                o.level = level;
                o.at = s->taken_ms;
                put(w, out, max, &n, &o);
            }
        }
        w->prev_state[i] = state;
    }

    /* B3: a new configuration re-baselines every zone. */
    if (first || s->config_seq != w->config_seq) {
        memset(w->known, 0, sizeof(w->known));
        w->config_seq = s->config_seq;
    }
    w->based = 1;

    /* B2 settle, B4 installer mode and walk test: no baselines, so the first snapshot after
     * them is one. The snapshot doesn't say which partition a zone is in, so a walk test in any
     * partition holds every zone (never a false occurrence; a door elsewhere waits for it). */
    if (s->taken_ms < w->settle_at || s->program || walking) {
        memset(w->known, 0, sizeof(w->known));
        return n;
    }

    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        const uint16_t flags = s->zone[z].flags;
        const bool fault = (flags & ALARM_ZS_FAULT) != 0u;
        const bool bad = (flags & BAD_FLAGS) != 0u;
        if (bit_get(w->known, z) && fault != bit_get(w->fault, z) && !bad &&
            !bit_get(w->bad, z)) {
            auto_occ_t o;
            memset(&o, 0, sizeof(o));
            o.kind = fault ? AUTO_TRIG_ZONE_OPENED : AUTO_TRIG_ZONE_CLOSED;
            o.zone = (uint16_t)(z + 1u);
            o.at = s->taken_ms;
            put(w, out, max, &n, &o);
        }
        bit_put(w->known, z, true);
        bit_put(w->fault, z, fault);
        bit_put(w->bad, z, bad);
    }
    return n;
}

void auto_watch_zones_reset(auto_watch_t *w, const uint8_t *zone_bits)
{
    if (w == NULL || zone_bits == NULL) {
        return;
    }
    for (unsigned i = 0; i < ALARM_ZONE_BITMAP_BYTES; i++) {
        w->known[i] = (uint8_t)(w->known[i] & (uint8_t)~zone_bits[i]);
    }
}
