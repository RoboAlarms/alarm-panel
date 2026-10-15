/*
 * voice_epoch.c - see zone_voice_epoch.h (D07, CORE-CFG-021).
 */
#include "zone_hub/zone_voice_epoch.h"

#include <string.h>

static bool in_range(uint16_t zone)
{
    return zone >= 1u && zone <= ALARM_MAX_ZONES;
}

void zone_voice_init(zone_voice_epochs_t *v)
{
    memset(v, 0, sizeof(*v));
}

bool zone_voice_note_change(zone_voice_epochs_t *v, uint16_t zone)
{
    if (!in_range(zone)) {
        return false;
    }
    v->epoch[zone - 1u]++;
    return true;
}

bool zone_voice_undo_change(zone_voice_epochs_t *v, uint16_t zone, uint32_t noted)
{
    if (!in_range(zone) || v->epoch[zone - 1u] != noted) {
        return false;
    }
    v->epoch[zone - 1u]--;
    return true;
}

uint32_t zone_voice_epoch(const zone_voice_epochs_t *v, uint16_t zone)
{
    return in_range(zone) ? v->epoch[zone - 1u] : 0u;
}

bool zone_voice_owed(zone_voice_epochs_t *v, uint8_t *out)
{
    bool any = false;
    memset(out, 0, ALARM_ZONE_BITMAP_BYTES);
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        v->inv_cap[i] = v->epoch[i];
        if (v->invalidated[i] != v->epoch[i]) {
            out[i / 8u] |= (uint8_t)(1u << (i % 8u));
            any = true;
        }
    }
    return any;
}

void zone_voice_invalidated(zone_voice_epochs_t *v, const uint8_t *zones)
{
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        if ((zones[i / 8u] & (1u << (i % 8u))) != 0u) {
            /* only the epoch it captured: a later change stays owed (epoch != invalidated) */
            v->invalidated[i] = v->inv_cap[i];
        }
    }
}

bool zone_voice_any_owed(const zone_voice_epochs_t *v)
{
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        if (v->invalidated[i] != v->epoch[i]) {
            return true;
        }
    }
    return false;
}

void zone_voice_capture(zone_voice_epochs_t *v)
{
    memcpy(v->store_cap, v->epoch, sizeof(v->store_cap));
}

void zone_voice_stored(zone_voice_epochs_t *v)
{
    memcpy(v->stored, v->store_cap, sizeof(v->stored));
}

bool zone_voice_settling(const zone_voice_epochs_t *v, uint16_t zone)
{
    if (!in_range(zone)) {
        return false;
    }
    const unsigned i = zone - 1u;
    return v->stored[i] != v->epoch[i] || v->invalidated[i] != v->epoch[i];
}
