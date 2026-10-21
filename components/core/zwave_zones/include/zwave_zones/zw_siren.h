/*
 * zw_siren.h - Z-Wave sirens as the panel's bell (docs/features/16 OUT-016..OUT-019,
 * docs/features/36 sirens.md): a siren that lists Sound Switch (79) - or, failing that, Binary
 * Switch (25) - sounds when the engine's bell sounds, with the same policy as every siren of the
 * panel (sirens/siren.h: its partitions, the partitions it `also` sounds for, burglary and audible
 * panic, fire and CO; fire, then CO, then burglary, then the bell test).
 *
 * What makes it sound is the engine, never the radio: the alarm task's on_output is posted to the
 * zwave task, which calls zws_output. Sound Switch: Tone Play Set (79 08 tone [volume]) with the
 * siren's tone for the sound (burglary, fire, CO; 0xFF the device's default tone), stop is tone
 * 00; Binary Switch: Set FF / 00 (one sound only). The bell's timeout and silencing stay the
 * engine's: the siren is stopped when the bell goes off.
 *
 * Confirmation (OUT-017): a Set is done when the radio says the siren took it (delivered; with
 * Supervision where the transport adds it). Not delivered - or no answer within ZWS_CONFIRM_MS -
 * it goes again ZWS_RETRY_MS later; after ZWS_SEND_TRIES failures it is the bell trouble, and it
 * goes again every ZWS_RETRY_SLOW_MS while it is still wanted, until one is delivered. While it
 * sounds the siren is asked (Tone Play Get / Switch Get) every ZWS_CHECK_MS: a Report that says
 * it is quiet while its sound is wanted sends the sound again (some sirens stop on their own
 * timer).
 *
 * Supervision (OUT-018): any frame from the siren's node is "heard"; unheard past its limit
 * (supervision_min, 0: ZWS_SUPERVISION_DEF_MIN, the usual Z-Wave siren check-in; at least
 * ZWS_SUPERVISION_MIN_MIN) while the radio works is the bell trouble, heard again clears it.
 * While the radio is down nothing is silent (the radio's own trouble says it); coming up, every
 * siren's silence counts from then.
 *
 * Squawk (OUT-019): with `squawk`, a Sound Switch siren plays its arm or disarm tone when one of
 * its own partitions arms or disarms (never for `also`), not while it sounds or a command of it
 * waits; sent once, never confirmed or retried. A tone 0 plays nothing; a Binary Switch siren
 * never squawks (its one sound is the alarm). zws_set_armed tells the state without a squawk (the
 * owner's start-up).
 *
 * "Test it" (zws_test): ZWS_TEST_MS of its burglary tone, then stop - refused while any of its own
 * partitions is armed or while it sounds.
 *
 * Portable C11, caller-owned state, no heap. The owner serializes calls.
 */
#ifndef ZW_SIREN_H
#define ZW_SIREN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "sirens/siren.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZWS_SIRENS              4u
#define ZWS_RETRY_MS            2000u   /* a Set not delivered: again this much later */
#define ZWS_SEND_TRIES          3u      /* ... this often, then the bell trouble */
#define ZWS_RETRY_SLOW_MS       10000u  /* then again this slowly while still wanted */
#define ZWS_STOP_TRIES          15u     /* a stop: given up after this many failed tries (about
                                         * two minutes), sent again when the siren is heard */
#define ZWS_CONFIRM_MS          15000u  /* a send whose end the radio never told: a failed try */
#define ZWS_CHECK_MS            60000u  /* while it sounds: asked whether it still does */
#define ZWS_TEST_MS             2000u   /* "Test it" */
#define ZWS_SUPERVISION_DEF_MIN 240u    /* 4 h: the usual Z-Wave siren's check-in */
#define ZWS_SUPERVISION_MIN_MIN 60u     /* the shortest limit the installer may set */
#define ZWS_VOLUME_MAX          100u
#define ZWS_TONE_STOP           0x00u
#define ZWS_TONE_DEFAULT        0xFFu   /* the device's own default tone */
#define ZWS_TAG                 0x53000000u /* its sends: top byte 'S' | slot << 16 | sequence */
#define ZWS_TAG_MASK            0xFF000000u

/* The store (NVS zwave/siren). */
#define ZWS_KEY      "siren"
#define ZWS_FORMAT   1u
#define ZWS_ENTRY    18u /* bytes after an entry's length byte */
#define ZWS_BLOB_MAX (4u + ZWS_SIRENS * (1u + ZWS_ENTRY) + 2u)

/* How it is driven. The numbers are stored. */
typedef enum {
    ZWS_KIND_SOUND = 1,  /* Sound Switch (79): a tone per alarm kind, volume, squawks */
    ZWS_KIND_BINARY = 2, /* Binary Switch (25): on and off */
} zws_kind_t;

/* An assignment (the installer's), as stored. */
typedef struct {
    bool used;
    uint16_t node, generation;
    uint8_t endpoint;        /* 0: the device itself */
    uint8_t kind;            /* zws_kind_t */
    uint8_t partitions;      /* its own partitions, bit 0 = partition 1 */
    uint8_t also;            /* other partitions whose alarms it also sounds for (alarms only) */
    bool burglary;           /* burglary and audible panic */
    bool fire;               /* fire and CO */
    bool squawk;             /* arm and disarm tones (Sound Switch only) */
    /* Sound Switch tone ids: 0xFF the device's default tone; tone_burglary 0 the default too,
     * tone_fire and tone_co 0 the burglary tone; tone_arm and tone_disarm 0 no chirp */
    uint8_t tone_burglary, tone_fire, tone_co, tone_arm, tone_disarm;
    uint8_t volume;          /* 0..100 %, 0 the device's own volume (Sound Switch v2) */
    uint16_t supervision_min; /* 0: ZWS_SUPERVISION_DEF_MIN; else at least ZWS_SUPERVISION_MIN_MIN */
} zws_config_t;

typedef struct {
    zws_config_t c;
    /* the command it should be following: a tone id or switch value, 0 quiet */
    uint8_t value;
    siren_sound_t sound;     /* the engine's sound it follows (QUIET: none) */
    bool pending;            /* `value` not confirmed yet */
    uint32_t tag;            /* the Set in flight, 0 none */
    uint64_t sent_at;
    uint8_t tries;           /* failed tries of `value` */
    bool unconfirmed;        /* ZWS_SEND_TRIES failed (part of the trouble) */
    uint64_t retry_at;
    uint32_t get_tag;        /* the keep-up Get in flight, 0 none */
    uint64_t check_at;       /* the next keep-up Get */
    uint32_t chirp_tag;      /* a squawk in flight (never retried), 0 none */
    uint64_t test_until;     /* "Test it": stop then, 0 none */
    uint64_t heard_at;       /* its last frame, or the radio's start or its assignment if later */
    bool silent;             /* unheard past its limit (part of the trouble) */
    bool fresh;              /* just assigned or read back: heard_at from the next call */
    uint32_t sets, reports;  /* counters for the console */
} zws_siren_t;

typedef struct {
    void *ctx;
    /* A command to (node, endpoint): the owner wraps it (Multi Channel, its security). false: it
     * could not be queued (a failed try). */
    bool (*send)(void *ctx, uint16_t node, uint8_t endpoint, const uint8_t *cmd, size_t len,
                 uint32_t tag);
    /* ALARM_TRBL_BELL on or off (any siren unconfirmed or silent), for the owner to post. */
    void (*bell_trouble)(void *ctx, bool active);
} zws_port_t;

typedef struct {
    zws_port_t port;
    zws_siren_t s[ZWS_SIRENS];
    siren_view_t view;
    uint16_t seq;
    bool radio_up;   /* silence counts only while the radio works */
    bool trouble;    /* as last told */
    bool dirty;      /* the assignments are to be stored */
} zws_t;

typedef enum {
    ZWS_OK = 0,
    ZWS_ERR_ARG,      /* no such slot, no node, no partition, neither burglary nor fire, a kind,
                       * volume or supervision time out of range */
    ZWS_ERR_FULL,     /* no free slot */
    ZWS_ERR_UNKNOWN,  /* no siren there */
    ZWS_ERR_ARMED,    /* test with one of its own partitions armed */
    ZWS_ERR_BUSY,     /* test while it sounds */
    ZWS_ERR_OFFLINE,  /* test while the radio is down */
} zws_result_t;

void zws_init(zws_t *z, const zws_port_t *port);
/* A new assignment's configuration: partition 1, burglary and fire, default tones, no squawk. */
void zws_defaults(zws_config_t *c, uint16_t node, uint16_t generation, uint8_t endpoint,
                  uint8_t kind);
/* The slot of (node, endpoint), or -1. */
int zws_find(const zws_t *z, uint16_t node, uint8_t endpoint);
/* Its own slot, else the first free one; -1: full. */
int zws_slot_for(const zws_t *z, uint16_t node, uint8_t endpoint);
/* Assign or change: a siren that sounds follows its new scope at once. */
zws_result_t zws_set(zws_t *z, uint64_t now, size_t slot, const zws_config_t *c);
/* Remove it: a stop goes out first if it sounds (best effort, once). */
zws_result_t zws_clear(zws_t *z, uint64_t now, size_t slot);
/* Its device left the network: every assignment of the node dropped (no stop: it is gone). True
 * when any was. */
bool zws_forget(zws_t *z, uint16_t node);
/* Read back (start-up): replaces the assignments, nothing sent; silence counts from the next
 * call that carries the time. */
void zws_load(zws_t *z, const zws_config_t cfg[ZWS_SIRENS]);

/* The engine's output (posted from the alarm task's on_output). */
void zws_output(zws_t *z, uint64_t now, uint8_t partition, alarm_output_t out,
                alarm_pattern_t pattern);
/* A partition armed or disarmed: the squawks. */
void zws_armed(zws_t *z, uint64_t now, uint8_t partition, bool armed);
/* The same without a squawk (the owner's start-up, from the engine's snapshot). */
void zws_set_armed(zws_t *z, uint8_t partition, bool armed);
/* The radio started working (true) or stopped (false). */
void zws_radio(zws_t *z, uint64_t now, bool up);
/* "Test it". */
zws_result_t zws_test(zws_t *z, uint64_t now, size_t slot);

/* The radio's news: a send ended (true when the tag was a siren's); a frame from `node` (heard);
 * a command from (node, endpoint) - true when it was an assigned siren's Report (taken). */
bool zws_send_done(zws_t *z, uint64_t now, uint32_t tag, bool delivered);
void zws_heard(zws_t *z, uint64_t now, uint16_t node);
bool zws_received(zws_t *z, uint64_t now, uint16_t node, uint8_t endpoint, const uint8_t *cmd,
                  size_t len);

void zws_poll(zws_t *z, uint64_t now);
/* When to poll again: `now` at once, UINT64_MAX not before news. */
uint64_t zws_next_deadline(const zws_t *z, uint64_t now);
/* How long it may stay unheard, ms. */
uint64_t zws_silent_limit_ms(const zws_config_t *c);
/* The command for a value (Sound Switch Tone Play Set with its volume, or Binary Switch Set):
 * its length. */
size_t zws_command(const zws_config_t *c, uint8_t value, uint8_t *out, size_t cap);
/* Whether a siren is in trouble now (unconfirmed or silent). */
bool zws_in_trouble(const zws_siren_t *s);

/* The store: 'Z' 'S' | format 1 | count | count x (length | slot | node u16 BE | generation u16
 * BE | endpoint | kind | partitions | also | flags (b0 burglary, b1 fire, b2 squawk) | tone
 * burglary | fire | co | arm | disarm | volume | supervision u16 BE minutes) | CRC-16/CCITT
 * (siren_crc16) BE. Missing fields read as 0, extra bytes are skipped; all or nothing. */
size_t zws_encode(const zws_t *z, uint8_t *out, size_t cap);
bool zws_decode(const uint8_t *buf, size_t len, zws_config_t out[ZWS_SIRENS]);

#ifdef __cplusplus
}
#endif

#endif /* ZW_SIREN_H */
