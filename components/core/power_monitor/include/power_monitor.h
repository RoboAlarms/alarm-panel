#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Unknown is zero, including an unconfigured or failed source. A source must
 * explicitly measure absence; an unreadable battery is not an absent battery. */
typedef enum { POWER_UNKNOWN, POWER_NO, POWER_YES } power_value_t;
typedef struct {
    power_value_t ac_present;
    power_value_t battery_present;
    power_value_t battery_low;
    power_value_t charging;
    int battery_mv;      /* -1 unavailable */
    int battery_percent; /* -1 unavailable; only a calibrated source supplies it */
} power_sample_t;
typedef bool (*power_read_fn)(void *ctx, power_sample_t *out);
typedef struct { power_read_fn read; void *ctx; } power_source_t;
typedef struct {
    power_value_t value, candidate;
    uint64_t since;
} power_filter_t;
typedef struct {
    power_sample_t status;
    power_filter_t ac, present, low, charging;
    uint64_t last_poll;
    bool sampled;
} power_monitor_t;

void power_monitor_init(power_monitor_t *monitor);
/* Call with monotonic milliseconds. Poll at least every 5 seconds. Unknown
 * invalidates immediately; known transitions require 5 seconds of stability.
 * Gaps over 15 seconds restart qualification rather than invent continuity. */
void power_monitor_poll(power_monitor_t *monitor, const power_source_t *source, uint64_t now);
bool power_source_none(void *ctx, power_sample_t *out);
/* Elecrow STC bank 0x00..0x0a: two little-endian mV fields, percent,
 * charge state, LED state. An empty connector can read 4581 mV / 100%.
 * Reject implausible cell readings rather than reporting a fictitious battery. */
bool power_decode_stc(const uint8_t *bytes, size_t len, power_sample_t *out);
uint8_t power_backlight_limit(uint8_t requested, bool saving);
typedef struct {
    bool saving, candidate;
    uint64_t since;
} power_saving_t;
/* Both entry and restoration need 15 seconds of stable evidence. */
bool power_saving_update(power_saving_t *state, const power_sample_t *sample, uint64_t now);

#endif
