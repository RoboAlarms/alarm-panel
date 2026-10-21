/*
 * siren.h - what a siren should be doing, whatever carries it (docs/features/16 OUT-001, OUT-003,
 * OUT-007, OUT-008): the policy every kind of siren shares - a wired output, a 433 MHz siren, a
 * Z-Wave siren - and the patterns a wired line plays.
 *
 * The engine decides; the siren follows. The engine's bell and strobe outputs (alarm_ports.h
 * on_output) are kept per partition in a siren_view_t; each siren serves a scope (the partitions
 * it sounds for, and whether for burglary and audible panic, for fire and CO, or both) and
 * siren_pick() gives the most urgent sound of its partitions: fire, then CO, then burglary, then
 * the bell test. This is the order Zigbee's sirens use (docs/features/20 alarm-devices.md §2,
 * zbw_wanted), so every siren in a house agrees.
 *
 * The bell test (ALARM_PAT_TEST, the walk test's start, WALK-003) is a one-shot: the engine sends
 * it and no OFF after it, so the view keeps when it came and siren_pick() answers SIREN_TEST for
 * SIREN_TEST_MS.
 *
 * Patterns on a wired line (siren_level): steady for burglary, temporal-3 for fire (NFPA 72 /
 * ANSI S3.41: 0.5 s on, 0.5 s off, three times, then 1.5 s off), temporal-4 for CO (NFPA 720:
 * 0.1 s on, 0.1 s off, four times, then 5 s off), one pulse for the test. Squawks (chirps when a
 * partition arms or disarms) are SIREN_CHIRP_ON_MS pulses SIREN_CHIRP_OFF_MS apart: one when it
 * arms, two when it disarms (the classic panels' convention).
 *
 * Portable C11, caller-owned state, no heap.
 */
#ifndef SIRENS_SIREN_H
#define SIRENS_SIREN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIREN_TEST_MS      1000u /* the bell test's pulse (WALK-003: "a short siren test") */
#define SIREN_CHIRP_ON_MS  150u
#define SIREN_CHIRP_OFF_MS 250u
#define SIREN_CHIRPS_ARM    1u
#define SIREN_CHIRPS_DISARM 2u

/* What a siren is asked to sound. The numbers are not stored. */
typedef enum {
    SIREN_QUIET = 0,
    SIREN_BURGLARY, /* burglary, audible panic, tamper, 24-hour audible: steady */
    SIREN_FIRE,     /* temporal-3 */
    SIREN_CO,       /* temporal-4 */
    SIREN_TEST,     /* the bell test: one short pulse */
} siren_sound_t;

/* Which alarms a siren serves. */
typedef struct {
    uint8_t partitions; /* its own partitions: their alarms, strobe, arming chirps and the
                         * bell test; bit 0 = partition 1; 0 serves none */
    uint8_t also;       /* other partitions whose alarms it also sounds for (OUT-015: the
                         * house's siren for the shop's alarm) - alarms and strobe only, never
                         * their chirps, tests or armed state */
    bool burglary;      /* burglary and audible panic */
    bool fire;          /* fire and CO */
} siren_scope_t;

/* The engine's outputs as last told, per partition. */
typedef struct {
    uint8_t bell[ALARM_MAX_PARTITIONS];     /* alarm_pattern_t */
    bool strobe[ALARM_MAX_PARTITIONS];
    bool armed[ALARM_MAX_PARTITIONS];       /* any state but disarmed (exit delay included) */
    uint64_t test_at[ALARM_MAX_PARTITIONS]; /* the last bell test's start, 0 none */
    bool smoke_reset;                       /* ALARM_OUT_SMOKE_RESET (system) */
} siren_view_t;

void siren_view_init(siren_view_t *v);
/* The engine's on_output. True when anything changed. Partition 0 carries only the smoke reset;
 * a partition past ALARM_MAX_PARTITIONS is ignored. */
bool siren_view_output(siren_view_t *v, uint64_t now, uint8_t partition, alarm_output_t out,
                       alarm_pattern_t pattern);
/* A partition armed (any state but disarmed) or disarmed. True when it changed. */
bool siren_view_armed(siren_view_t *v, uint8_t partition, bool armed);

/* The sound for a bell pattern (ALARM_PAT_ON, a bell simply "on", is burglary). */
siren_sound_t siren_sound_of(alarm_pattern_t pattern);
/* The most urgent sound of the partitions it serves (its own and `also`): fire, CO, burglary, then the test
 * (a test only while no partition of it sounds, and for SIREN_TEST_MS after it came). A scope
 * with neither burglary nor fire still takes the test (it is the siren's own check). */
siren_sound_t siren_pick(const siren_scope_t *s, const siren_view_t *v, uint64_t now);
/* Any of its partitions' (own or `also`) strobe output is on (whatever the alarm). */
bool siren_strobe(const siren_scope_t *s, const siren_view_t *v);
/* Any of its own partitions armed (`also` never counts). */
bool siren_armed(const siren_scope_t *s, const siren_view_t *v);
/* When the view's answers for this scope change by themselves (a test ending), 0 never. */
uint64_t siren_view_deadline(const siren_scope_t *s, const siren_view_t *v, uint64_t now);

/* A wired line's level `t_ms` into the sound (t counted from when the sound started), and in
 * *next_ms how long until it changes (UINT32_MAX: never). QUIET is off; BURGLARY on; TEST on for
 * SIREN_TEST_MS from its start (siren_pick ends it). */
bool siren_level(siren_sound_t sound, uint32_t t_ms, uint32_t *next_ms);
/* A squawk of `chirps` chirps `t_ms` after it began: its level, and in *next_ms the time to the
 * next change (UINT32_MAX once it is over, off). */
bool siren_chirp_level(uint8_t chirps, uint32_t t_ms, uint32_t *next_ms);
/* How long a squawk of `chirps` lasts, ms. */
uint32_t siren_chirp_ms(uint8_t chirps);

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) of the sirens' stored blobs. */
uint16_t siren_crc16(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SIRENS_SIREN_H */
