/*
 * siren.c - the sirens' shared policy and the wired line's patterns (siren.h).
 */
#include "sirens/siren.h"

#include <string.h>

/* Temporal-3 (NFPA 72): on 500, off 500, on 500, off 500, on 500, off 1500 = a 4 s cycle. */
static const uint16_t T3[] = {500u, 500u, 500u, 500u, 500u, 1500u};
/* Temporal-4 (NFPA 720): on 100, off 100 four times, the last off 5 s = a 5.7 s cycle. */
static const uint16_t T4[] = {100u, 100u, 100u, 100u, 100u, 100u, 100u, 5000u};

void siren_view_init(siren_view_t *v)
{
    memset(v, 0, sizeof(*v));
}

bool siren_view_output(siren_view_t *v, uint64_t now, uint8_t partition, alarm_output_t out,
                       alarm_pattern_t pattern)
{
    if (out == ALARM_OUT_SMOKE_RESET) {
        const bool on = pattern != ALARM_PAT_OFF;
        const bool changed = on != v->smoke_reset;
        v->smoke_reset = on;
        return changed;
    }
    if (partition == 0u || partition > ALARM_MAX_PARTITIONS) {
        return false;
    }
    const size_t i = (size_t)partition - 1u;
    if (out == ALARM_OUT_STROBE) {
        const bool on = pattern != ALARM_PAT_OFF;
        const bool changed = on != v->strobe[i];
        v->strobe[i] = on;
        return changed;
    }
    if (out != ALARM_OUT_BELL) {
        return false;
    }
    if (pattern == ALARM_PAT_TEST) {
        /* A one-shot: no OFF follows it, and it doesn't replace a sounding bell. */
        v->test_at[i] = (now == 0u) ? 1u : now;
        return true;
    }
    const bool changed = v->bell[i] != (uint8_t)pattern;
    v->bell[i] = (uint8_t)pattern;
    return changed;
}

bool siren_view_armed(siren_view_t *v, uint8_t partition, bool armed)
{
    if (partition == 0u || partition > ALARM_MAX_PARTITIONS) {
        return false;
    }
    const bool changed = v->armed[partition - 1u] != armed;
    v->armed[partition - 1u] = armed;
    return changed;
}

siren_sound_t siren_sound_of(alarm_pattern_t pattern)
{
    switch (pattern) {
    case ALARM_PAT_TEMPORAL3: return SIREN_FIRE;
    case ALARM_PAT_TEMPORAL4: return SIREN_CO;
    case ALARM_PAT_STEADY:
    case ALARM_PAT_ON: return SIREN_BURGLARY;
    case ALARM_PAT_TEST: return SIREN_TEST;
    case ALARM_PAT_OFF:
    default: return SIREN_QUIET;
    }
}

/* Lower is more urgent. */
static int rank(siren_sound_t s)
{
    switch (s) {
    case SIREN_FIRE: return 0;
    case SIREN_CO: return 1;
    case SIREN_BURGLARY: return 2;
    case SIREN_TEST: return 3;
    case SIREN_QUIET:
    default: return 4;
    }
}

static bool serves(const siren_scope_t *s, size_t i)
{
    return ((s->partitions | s->also) & (1u << i)) != 0u;
}

static bool own(const siren_scope_t *s, size_t i)
{
    return (s->partitions & (1u << i)) != 0u;
}

static bool test_running(const siren_view_t *v, size_t i, uint64_t now)
{
    return v->test_at[i] != 0u && now >= v->test_at[i] && now - v->test_at[i] < SIREN_TEST_MS;
}

siren_sound_t siren_pick(const siren_scope_t *s, const siren_view_t *v, uint64_t now)
{
    siren_sound_t best = SIREN_QUIET;
    for (size_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if (!serves(s, i)) {
            continue;
        }
        siren_sound_t here = siren_sound_of((alarm_pattern_t)v->bell[i]);
        if ((here == SIREN_FIRE || here == SIREN_CO) && !s->fire) {
            here = SIREN_QUIET;
        } else if (here == SIREN_BURGLARY && !s->burglary) {
            here = SIREN_QUIET;
        }
        if (here == SIREN_QUIET && own(s, i) && test_running(v, i, now)) {
            here = SIREN_TEST;
        }
        if (rank(here) < rank(best)) {
            best = here;
        }
    }
    return best;
}

bool siren_strobe(const siren_scope_t *s, const siren_view_t *v)
{
    for (size_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if (serves(s, i) && v->strobe[i]) {
            return true;
        }
    }
    return false;
}

bool siren_armed(const siren_scope_t *s, const siren_view_t *v)
{
    for (size_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if (own(s, i) && v->armed[i]) {
            return true;
        }
    }
    return false;
}

uint64_t siren_view_deadline(const siren_scope_t *s, const siren_view_t *v, uint64_t now)
{
    uint64_t next = 0u;
    for (size_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if (own(s, i) && test_running(v, i, now)) {
            const uint64_t end = v->test_at[i] + SIREN_TEST_MS;
            if (next == 0u || end < next) {
                next = end;
            }
        }
    }
    return next;
}

/* A cycle of on/off steps starting on: the level at t and the time to the next step. */
static bool cycle_level(const uint16_t *steps, size_t n, uint32_t t_ms, uint32_t *next_ms)
{
    uint32_t period = 0u;
    for (size_t i = 0; i < n; i++) {
        period += steps[i];
    }
    uint32_t t = t_ms % period;
    for (size_t i = 0; i < n; i++) {
        if (t < steps[i]) {
            *next_ms = steps[i] - t;
            return (i % 2u) == 0u;
        }
        t -= steps[i];
    }
    *next_ms = 1u; /* not reached */
    return false;
}

bool siren_level(siren_sound_t sound, uint32_t t_ms, uint32_t *next_ms)
{
    switch (sound) {
    case SIREN_BURGLARY:
        *next_ms = UINT32_MAX;
        return true;
    case SIREN_FIRE: return cycle_level(T3, sizeof(T3) / sizeof(T3[0]), t_ms, next_ms);
    case SIREN_CO: return cycle_level(T4, sizeof(T4) / sizeof(T4[0]), t_ms, next_ms);
    case SIREN_TEST:
        if (t_ms < SIREN_TEST_MS) {
            *next_ms = SIREN_TEST_MS - t_ms;
            return true;
        }
        *next_ms = UINT32_MAX;
        return false;
    case SIREN_QUIET:
    default:
        *next_ms = UINT32_MAX;
        return false;
    }
}

uint16_t siren_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)((unsigned)buf[i] << 8));
        for (int b = 0; b < 8; b++) {
            const unsigned poly = ((unsigned)crc & 0x8000u) != 0u ? 0x1021u : 0u;
            const unsigned next = ((unsigned)crc << 1) ^ poly;
            crc = (uint16_t)(next & 0xFFFFu);
        }
    }
    return crc;
}

uint32_t siren_chirp_ms(uint8_t chirps)
{
    if (chirps == 0u) {
        return 0u;
    }
    return (uint32_t)chirps * SIREN_CHIRP_ON_MS + ((uint32_t)chirps - 1u) * SIREN_CHIRP_OFF_MS;
}

bool siren_chirp_level(uint8_t chirps, uint32_t t_ms, uint32_t *next_ms)
{
    if (t_ms >= siren_chirp_ms(chirps)) {
        *next_ms = UINT32_MAX;
        return false;
    }
    const uint32_t step = SIREN_CHIRP_ON_MS + SIREN_CHIRP_OFF_MS;
    const uint32_t t = t_ms % step;
    if (t < SIREN_CHIRP_ON_MS) {
        *next_ms = SIREN_CHIRP_ON_MS - t;
        return true;
    }
    *next_ms = step - t;
    return false;
}
