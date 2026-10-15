/*
 * zone_debounce.h - response time for sampled inputs (wired terminals, ZONE-004): a new level
 * counts once it has held for the zone's response time (10, 350, 700 or 1200 ms), so a
 * bouncing contact or a short glitch doesn't make an event. Portable C11.
 */
#ifndef ZONE_DEBOUNCE_H
#define ZONE_DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t hold_ms; /* response time */
    bool primed;      /* a first sample arrived */
    uint8_t stable;   /* the level that counts */
    uint8_t pending;  /* a different level seen since 'since' */
    bool waiting;     /* 'pending' is being timed */
    uint64_t since;
} zone_debounce_t;

void zone_debounce_init(zone_debounce_t *d, uint16_t hold_ms);

/* Feeds one sample (level 0 or 1) taken at 'now'. Returns true when the stable level changed
 * (read d->stable). The first sample is stable at once: the state at start counts right away. */
bool zone_debounce_sample(zone_debounce_t *d, uint8_t level, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_DEBOUNCE_H */
