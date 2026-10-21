/*
 * rf_siren.h - 433 MHz sirens (docs/features/16 OUT-010..OUT-013, docs/features/25 RF-026): the
 * cheap one-way sirens sold with 433 MHz alarm kits, which learn their panel's code the way they
 * learn a key fob, and sound, stop and chirp on the codes it sends.
 *
 * The panel is a transmitter to them: each siren gets an address of the panel's own (a random
 * 20-bit EV1527 address no sensor of the house uses and no factory shares, RF-018) and four
 * codes - alarm, stop, arm, disarm (defaults RFS_CODE_*, the data nibbles of the common
 * four-button fob; changeable for a siren that wants others). To pair, the siren is put in its
 * learning mode and the panel sends the pairing code (the arm code) a few times (rfs_pair).
 *
 * What it sounds for is the same policy as every siren (siren.h): its partitions, burglary and
 * audible panic, fire and CO. A one-way siren has one sound, so fire and CO sound as burglary.
 * While its sound is wanted the alarm code goes out at once and again every RFS_REPEAT_MS (a
 * frame lost to noise must not leave a house quiet, and it keeps the siren going past its own
 * timer until the engine's bell times out); when the sound ends the stop code goes out
 * RFS_STOP_SENDS times, RFS_STOP_GAP_MS apart. With `squawk`, the arm and disarm codes go out
 * when its partitions arm and disarm (the siren's own chirp) - not while it sounds.
 *
 * Supervision: none is possible (the siren never answers; docs/features/22's trust facts say
 * "not supervised"). What the panel can know - its transmitter missing or refusing while a siren
 * is assigned - is the bell trouble (ALARM_TRBL_BELL through the port).
 *
 * Portable C11, caller-owned state, no heap. The owner serializes calls.
 */
#ifndef SIRENS_RF_SIREN_H
#define SIRENS_RF_SIREN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rf_decode/rf_decode.h"
#include "sirens/siren.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFS_SIRENS       4u
#define RFS_UNIT_US      350u   /* T of the frames sent (an EV1527 fob's usual 300-400 us) */
#define RFS_FRAMES       12u    /* frames per transmission (~0.55 s at 350 us) */
#define RFS_PAIR_FRAMES  24u    /* the pairing code's transmissions are longer */
#define RFS_PAIR_SENDS   3u     /* ... and sent this often, RFS_STOP_GAP_MS apart */
#define RFS_REPEAT_MS    20000u /* the alarm code again while the sound is wanted */
#define RFS_STOP_SENDS   3u
#define RFS_STOP_GAP_MS  1500u
#define RFS_TEST_MS      2000u  /* "Test it": alarm, then stop this much later */
/* The common four-button fob's data nibbles (one bit a button). To be confirmed with each
 * siren model on the bench (docs/features/16 "433 MHz sirens"); changeable per siren. */
#define RFS_CODE_ARM     0x2u
#define RFS_CODE_DISARM  0x1u
#define RFS_CODE_ALARM   0x8u
#define RFS_CODE_STOP    RFS_CODE_DISARM

#define RFS_NAMESPACE    "outputs"
#define RFS_KEY          "rf"
#define RFS_FORMAT       1u
#define RFS_ENTRY        15u   /* bytes after an entry's length byte */
#define RFS_BLOB_MAX     (4u + RFS_SIRENS * (1u + RFS_ENTRY) + 2u)

typedef struct {
    bool used;
    uint8_t proto;       /* rf_proto_t: EV1527 (the only one sent today) */
    uint32_t address;    /* the panel's address for it (20 bits) */
    uint8_t code_alarm, code_stop, code_arm, code_disarm; /* data nibbles */
    uint8_t partitions;  /* bit 0 = partition 1 */
    uint8_t also;        /* other partitions whose alarms it also sounds for (OUT-015) */
    bool burglary;       /* burglary and audible panic */
    bool fire;           /* fire and CO (it sounds its one sound) */
    bool squawk;         /* the arm and disarm codes when its partitions arm and disarm */
    uint16_t unit_us;    /* 0: RFS_UNIT_US */
} rfs_config_t;

typedef struct {
    rfs_config_t c;
    siren_sound_t sound;  /* the sound being kept up (QUIET none) */
    uint64_t next_send;   /* the alarm code again then (0 none) */
    uint8_t stops_left;   /* stop codes still to send */
    uint64_t stop_at;
    uint64_t test_until;  /* "Test it": stop then (0 none) */
    uint8_t pairs_left;   /* pairing transmissions still to send */
    uint64_t pair_at;
    uint32_t sent;        /* transmissions the port took */
    uint64_t last_sent;   /* when, 0 never */
    bool refused;         /* the port refused the last one (part of the trouble) */
} rfs_siren_t;

typedef struct {
    void *ctx;
    /* Transmit `p` (proto, device, data) `frames` times back to back at unit_us; false: no
     * transmitter, or it is busy past the call. */
    bool (*send)(void *ctx, const rf_packet_t *p, uint16_t unit_us, uint8_t frames);
    void (*bell_trouble)(void *ctx, bool active);
} rfs_port_t;

typedef struct {
    rfs_port_t port;
    rfs_siren_t s[RFS_SIRENS];
    siren_view_t view;
    bool transmitter;  /* the owner says a transmitter is there */
    bool trouble;      /* as last told */
    bool dirty;        /* to be stored */
} rfs_t;

typedef enum {
    RFS_OK = 0,
    RFS_ERR_ARG,     /* no such slot, no partition, neither burglary nor fire, a code past the
                      * protocol's data bits, an address the factory shares */
    RFS_ERR_IN_USE,  /* another siren has that address */
    RFS_ERR_UNKNOWN, /* no siren there */
    RFS_ERR_ARMED,   /* test or pair with one of its partitions armed */
    RFS_ERR_BUSY,    /* test while it sounds */
    RFS_ERR_SEND,    /* no transmitter */
} rfs_result_t;

void rfs_init(rfs_t *r, const rfs_port_t *port);
/* A new siren's configuration: the address given, the default codes, partition 1, burglary and
 * fire, no squawk. */
void rfs_defaults(rfs_config_t *c, uint32_t address);
/* An address for a new siren from a random number: 20 bits, never one the factory shares and
 * never one of `avoid` (the house's sensors and the other sirens). 0 when none was found. */
uint32_t rfs_pick_address(uint32_t random, const uint32_t *avoid, size_t n_avoid);
rfs_result_t rfs_set(rfs_t *r, uint64_t now, size_t slot, const rfs_config_t *c);
/* Remove it: a stop goes out first if it sounds. */
rfs_result_t rfs_clear(rfs_t *r, uint64_t now, size_t slot);
/* The first free slot, or -1. */
int rfs_free_slot(const rfs_t *r);

void rfs_output(rfs_t *r, uint64_t now, uint8_t partition, alarm_output_t out,
                alarm_pattern_t pattern);
void rfs_armed(rfs_t *r, uint64_t now, uint8_t partition, bool armed);
void rfs_set_armed(rfs_t *r, uint8_t partition, bool armed);
/* The transmitter came (true) or went (false). */
void rfs_transmitter(rfs_t *r, uint64_t now, bool present);
/* "Test it": the alarm code, then stop RFS_TEST_MS later; refused while any of its partitions is
 * armed or while it sounds. */
rfs_result_t rfs_test(rfs_t *r, uint64_t now, size_t slot);
/* Send its pairing code (the arm code) RFS_PAIR_SENDS times: the siren in learning mode takes it.
 * Refused while any of its partitions is armed. */
rfs_result_t rfs_pair(rfs_t *r, uint64_t now, size_t slot);

void rfs_poll(rfs_t *r, uint64_t now);
uint64_t rfs_next_deadline(const rfs_t *r, uint64_t now);

/* The store (NVS outputs/rf): 'R' 'S' | format | count | count x (length | slot | proto |
 * address u32 BE | alarm | stop | arm | disarm | partitions | flags (b0 burglary, b1 fire,
 * b2 squawk) | unit u16 BE | also) | CRC-16/CCITT. Missing fields read as 0, extra bytes are
 * skipped, all or nothing. */
size_t rfs_encode(const rfs_t *r, uint8_t *out, size_t cap);
bool rfs_decode(const uint8_t *buf, size_t len, rfs_config_t out[RFS_SIRENS]);

#ifdef __cplusplus
}
#endif

#endif /* SIRENS_RF_SIREN_H */
