/*
 * snapshot.c - one-call status snapshot for keypads and the UI.
 */
#include "alarm/alarm_snapshot.h"

#include <string.h>

void alarm_engine_snapshot(const alarm_engine_t *e, alarm_snapshot_t *out)
{
    if (e == NULL || out == NULL) {
        return;
    }
    const alarm_config_t *cfg = alarm_engine_config(e);
    out->taken_ms = e->now;
    out->partitions = 0u;
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (cfg->partitions[p - 1u].enabled) {
            out->partitions = (uint8_t)(out->partitions | (1u << (p - 1u)));
            alarm_engine_partition_status(e, p, &out->part[p - 1u]);
        } else {
            memset(&out->part[p - 1u], 0, sizeof(out->part[0]));
        }
    }
    out->sys_troubles = alarm_engine_system_troubles(e, &out->sys_troubles_restored);
    out->dev_troubles = alarm_engine_device_troubles(e);
    out->default_codes = alarm_engine_default_codes(e);
    out->program = alarm_engine_program_active(e);
    const alarm_ms_t left = alarm_engine_recovery_left(e);
    out->recovery_left_ms = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
    const alarm_source_t local = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    const alarm_ms_t lock_left = alarm_engine_lockout_left(e, &local, e->now);
    out->local_lockout_until = lock_left != 0u ? e->now + lock_left : 0u;
    out->zones_used = 0u;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED) {
            out->zones_used = z;
            alarm_engine_zone_status(e, z, &out->zone[z - 1u]);
        } else {
            memset(&out->zone[z - 1u], 0, sizeof(out->zone[0]));
        }
    }
    out->last_event_seq = e->seq;
    out->exit_sounds = cfg->opt.exit_sounds;
}

bool alarm_snapshot_any_armed(const alarm_snapshot_t *s)
{
    for (unsigned i = 0; s != NULL && i < ALARM_MAX_PARTITIONS; i++) {
        if ((s->partitions & (1u << i)) != 0u && s->part[i].state != ALARM_STATE_DISARMED) {
            return true;
        }
    }
    return false;
}

bool alarm_snapshot_locked(const alarm_snapshot_t *s)
{
    if (s == NULL || alarm_snapshot_any_armed(s)) {
        return true;
    }
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_sound_t sound = s->part[i].sounder;
        if ((s->partitions & (1u << i)) != 0u &&
            (sound == ALARM_SOUND_ALARM || sound == ALARM_SOUND_FIRE || sound == ALARM_SOUND_CO ||
             sound == ALARM_SOUND_AUX)) {
            return true;
        }
    }
    return false;
}

/* One walker for the Troubles list (UI-030): the touchscreen and the Panel Manager count the
 * same lines. */
typedef struct {
    size_t n;
    size_t first; /* the line that goes into out[0] */
    alarm_trouble_item_t *out;
    size_t max;
    uint32_t sig; /* FNV-1a over every line, in order */
} trouble_walk_t;

static void trouble_put(trouble_walk_t *w, uint8_t family, uint8_t kind, uint16_t number,
                        bool restored)
{
    if (w->out != NULL && w->n >= w->first && w->n - w->first < w->max) {
        alarm_trouble_item_t *it = &w->out[w->n - w->first];
        it->family = family;
        it->kind = kind;
        it->number = number;
        it->restored = restored;
    }
    const uint8_t b[5] = {family, kind, (uint8_t)(number & 0xFFu), (uint8_t)(number >> 8),
                          restored ? 1u : 0u};
    for (size_t i = 0; i < sizeof(b); i++) {
        w->sig = (w->sig ^ b[i]) * 16777619u;
    }
    w->n++;
}

static void troubles_walk(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                          uint8_t partitions, trouble_walk_t *w)
{
    for (int pass = 0; pass < 2; pass++) {
        const bool restored = pass == 1;
        for (unsigned t = 0; t < (unsigned)ALARM_TRBL_COUNT; t++) {
            const uint32_t bit = 1u << t;
            const bool now = (s->sys_troubles & bit) != 0u;
            const bool was = !now && (s->sys_troubles_restored & bit) != 0u;
            if (restored ? was : now) {
                trouble_put(w, ALARM_TRBL_FAMILY_SYSTEM, (uint8_t)t, 0u, restored);
            }
        }
        if (!restored) {
            for (unsigned d = 0; d < 32u; d++) {
                if ((s->dev_troubles & (1u << d)) != 0u) {
                    trouble_put(w, ALARM_TRBL_FAMILY_DEVICE, ALARM_TRBL_DEVICE_SUPERVISION,
                                (uint16_t)(d + 1u), false);
                }
            }
        }
        for (uint16_t z = 1u; z <= s->zones_used && z <= ALARM_MAX_ZONES; z++) {
            const alarm_zone_config_t *zc = &cfg->zones[z - 1u];
            if (zc->type == ALARM_ZT_NOT_USED) {
                continue;
            }
            if (zc->partition < 1u || zc->partition > ALARM_MAX_PARTITIONS ||
                (partitions & (1u << (zc->partition - 1u))) == 0u) {
                continue;
            }
            const alarm_zone_status_t *zs = &s->zone[z - 1u];
            if (!restored) {
                for (unsigned t = 0; t < (unsigned)ALARM_ZTRBL_COUNT; t++) {
                    if ((zs->trouble & (1u << t)) != 0u) {
                        trouble_put(w, ALARM_TRBL_FAMILY_ZONE, (uint8_t)t, z, false);
                    }
                }
            } else if (zs->trouble == 0u && (zs->flags & ALARM_ZS_TROUBLE_MEM) != 0u) {
                trouble_put(w, ALARM_TRBL_FAMILY_ZONE, (uint8_t)ALARM_ZTRBL_LOOP, z, true);
            }
        }
    }
}

size_t alarm_snapshot_troubles(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                               uint8_t partitions, alarm_trouble_item_t *out, size_t max)
{
    return alarm_snapshot_troubles_from(s, cfg, partitions, 0u, out, max);
}

size_t alarm_snapshot_troubles_from(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                                    uint8_t partitions, size_t first, alarm_trouble_item_t *out,
                                    size_t max)
{
    trouble_walk_t w = {0, first, out, max, 2166136261u};
    if (s == NULL || cfg == NULL) {
        return 0;
    }
    troubles_walk(s, cfg, partitions, &w);
    return w.n;
}

uint32_t alarm_snapshot_troubles_sig(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                                     uint8_t partitions)
{
    trouble_walk_t w = {0, 0, NULL, 0, 2166136261u};
    if (s != NULL && cfg != NULL) {
        troubles_walk(s, cfg, partitions, &w);
    }
    return w.sig;
}
