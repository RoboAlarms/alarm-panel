/*
 * debounce.c - response time for sampled inputs (see zone_debounce.h).
 */
#include <string.h>

#include "zone_hub/zone_debounce.h"

void zone_debounce_init(zone_debounce_t *d, uint16_t hold_ms)
{
    memset(d, 0, sizeof(*d));
    d->hold_ms = hold_ms;
}

bool zone_debounce_sample(zone_debounce_t *d, uint8_t level, uint64_t now)
{
    const uint8_t l = level != 0u ? 1u : 0u;
    if (!d->primed) {
        d->primed = true;
        d->stable = l;
        d->waiting = false;
        return true;
    }
    if (l == d->stable) {
        d->waiting = false; /* a glitch shorter than the response time */
        return false;
    }
    if (!d->waiting) {
        d->waiting = true;
        d->pending = l;
        d->since = now;
    }
    if (now - d->since >= d->hold_ms) {
        d->stable = l;
        d->waiting = false;
        return true;
    }
    return false;
}
