/*
 * zw_outputs.h - what the panel's Z-Wave devices switch (docs/features/36 ZW-034, ZW-035): lights,
 * dimmers, plugs and relays, and a garage door made of a relay and a tilt sensor.
 *
 * Outputs come from what the node cache says a device has: Binary Switch (25) or Multilevel
 * Switch (26) at the device itself or at its end points (a device with switching end points - a
 * multi-relay - is switched only through them, never through its root, which mirrors one of them).
 *
 * One state, whoever changes it: a device's own Report (someone pressed it), the panel's Set and
 * the Get that follows it, all land in the same `level`. A Set is done only when the device says
 * so (a Report with the level asked for); otherwise it ends TIMEOUT and the level stays what the
 * device last said. A command older than ZWO_STALE_MS when it arrives is refused (a delayed or
 * replayed one must not switch anything), and one command at a time per output.
 *
 * A garage door (ZW-035) is a relay pulsed like a wall button, and a tilt sensor that says where
 * the door is. The relay is never switched as a plain output. Open and close ask where the door is
 * first: already there, nothing is pulsed; not known (no report, the sensor stale or in trouble),
 * nothing is pulsed either - the panel never pulses blind. Then: relay on, ZWO_PULSE_MS later off
 * (confirmed, retried), and the door watched for up to its travel time: it reached the goal, or it
 * did not move/stopped short. A new pulse waits out the last one's travel (an opener reverses on a
 * second pulse) - unless the door already got there. A press the relay never confirmed (its ON
 * not acknowledged, or no answer at all within ZWO_SET_MS) may still have switched it: it is
 * released all the same (off, confirmed, retried) and the move then ends failed with the press's
 * own result - never idle with the relay perhaps held on (PL01). Only an off the radio took is
 * one of its ZWO_OFF_TRIES; one it refused (its queue full, perhaps with the press's own on still
 * in it) is tried again ZWO_SET_MS later - at once when that on's end frees the queue - at most
 * ZWO_OFF_REFUSALS times. Where the door is, is only ever the sensor's word: a press is "pressed,
 * waiting for its sensor", never "opening" (a tilt sensor can't see a door on its way). Stop is
 * refused: a pulse would reverse or restart it, not stop it. A garage's relay found on while no
 * move is under way (the panel restarted during a press, someone switched it) is released at
 * once - a relay held on blocks the opener's own button.
 *
 * Portable C11, caller-owned state, no heap. The owner serializes calls.
 */
#ifndef ZW_OUTPUTS_H
#define ZW_OUTPUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_nodes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZWO_OUTPUTS      16u
#define ZWO_GARAGES      2u
#define ZWO_SET_MS       10000u /* a Set's confirmation */
#define ZWO_VERIFY_MS    1500u  /* a Get after a Set the device did not report by itself */
#define ZWO_STALE_MS     5000u  /* a command older than this when it arrives is refused */
#define ZWO_PULSE_MS     1000u  /* a garage relay's press */
#define ZWO_OFF_TRIES    3u     /* offs the radio took (sent), each confirmed or not */
/* offs the radio refused, ZWO_SET_MS apart: a minute, past the radio's own end (30 s after it
 * queued it) of a press's on that may fill its queue */
#define ZWO_OFF_REFUSALS 6u
#define ZWO_TRAVEL_S_DEF 20u
#define ZWO_TRAVEL_S_MAX 120u
#define ZWO_TAG_BASE     0x4F000000u /* 'O' | sequence */
#define ZWO_TAG_MASK     0xFF000000u

typedef enum {
    ZWO_SWITCH = 1, /* Binary Switch at the device itself */
    ZWO_DIMMER,     /* Multilevel Switch */
    ZWO_RELAY,      /* Binary Switch at an end point (a multi-relay's) */
} zwo_kind_t;

typedef enum {
    ZWO_POS_UNKNOWN = 0,
    ZWO_POS_OPEN,
    ZWO_POS_CLOSED,
} zwo_position_t;

typedef enum {
    ZWO_OK = 0,             /* done (or, from zwo_set/zwo_garage: started) */
    ZWO_ERR_UNKNOWN,        /* no such output / garage */
    ZWO_ERR_BUSY,           /* a command under way; a garage still travelling */
    ZWO_ERR_STALE,          /* the command is too old */
    ZWO_ERR_ARG,            /* a level it can't take; the garage relay as a plain output */
    ZWO_ERR_OFFLINE,        /* it can't be reached (the owner says so) */
    ZWO_ERR_POSITION,       /* the garage door's position isn't known: nothing pulsed */
    ZWO_ERR_SEND,           /* the radio couldn't send it */
    ZWO_ERR_TIMEOUT,        /* the device didn't confirm */
    ZWO_ERR_NO_MOVE,        /* the door didn't reach the goal within its travel time */
    ZWO_ERR_RELAY_STUCK,    /* the relay didn't confirm off after its pulse */
    ZWO_ALREADY,            /* the garage was there already: nothing pulsed */
} zwo_result_t;

typedef struct {
    bool used;
    uint16_t node, generation;
    uint8_t endpoint;
    uint8_t cc;           /* ZW_CC_BINARY_SWITCH / ZW_CC_MULTILEVEL_SWITCH */
    uint8_t kind;         /* zwo_kind_t */
    int8_t level;         /* -1 unknown, 0..100 % (a switch: 0 or 100) */
    uint64_t heard_ms;    /* its last Report (0: none) */
    bool garage;          /* a garage's relay: never switched as an output */
    /* the command under way */
    uint32_t cmd;         /* its id (the owner's), 0 none */
    int8_t target;
    bool set_out, get_due;
    uint64_t get_at, deadline;
    uint32_t tag;
} zwo_output_t;

typedef enum {
    ZWO_G_IDLE = 0,
    ZWO_G_PRESS,    /* relay on sent */
    ZWO_G_HOLD,     /* on confirmed: off at `at` */
    ZWO_G_RELEASE,  /* off sent (tries) */
    ZWO_G_TRAVEL,   /* watching the door until `at` */
} zwo_gstate_t;

typedef struct {
    bool used;
    uint8_t output;        /* its relay: an index into outputs */
    uint16_t zone;         /* its tilt sensor's zone (the owner reads its position) */
    uint16_t travel_s;
    /* the move under way */
    uint8_t state;         /* zwo_gstate_t */
    uint8_t goal;          /* zwo_position_t */
    uint8_t moving_from;   /* its position when the press went (the page's "opening") */
    uint8_t tries;         /* RELEASE: off sent this many times (at most ZWO_OFF_TRIES) */
    uint8_t refused;       /* RELEASE: off refused this many times (at most ZWO_OFF_REFUSALS) */
    uint8_t unsure;        /* zwo_result_t: its on wasn't confirmed - released, then this (PL01) */
    bool verifying, get_sent; /* RELEASE: its off being confirmed */
    uint32_t cmd;
    uint32_t tag;          /* the send awaited (0: none - RELEASE: its off refused) */
    uint32_t on_tag;       /* its press's on until the radio ends it (0: ended) */
    uint64_t at, last_press;
} zwo_garage_t;

typedef struct {
    void *ctx;
    /* A command to (node, endpoint): the owner wraps it (Multi Channel for an end point, its
     * security). false: it could not be queued. */
    bool (*send)(void *ctx, uint16_t node, uint8_t endpoint, const uint8_t *cmd, size_t len,
                 uint32_t tag);
    /* Where a garage's tilt sensor says the door is (its zone's input, fresh). */
    zwo_position_t (*position)(void *ctx, uint16_t zone);
    /* A command's end (cmd as given). */
    void (*result)(void *ctx, uint32_t cmd, zwo_result_t result);
    /* Something changed that a page or Home Assistant shows (optional). */
    void (*changed)(void *ctx);
} zwo_port_t;

typedef struct {
    zwo_port_t port;
    zwo_output_t out[ZWO_OUTPUTS];
    zwo_garage_t garage[ZWO_GARAGES];
    uint16_t seq;
    uint64_t now;
} zwo_t;

void zwo_init(zwo_t *o, const zwo_port_t *port);

/* The device's outputs from its record (after its interview, and after a restart from the stored
 * cache): kept ones keep their level, new ones start unknown; a device gone (or reused under a
 * new generation) loses its outputs - and a garage built on them. How many it has. */
size_t zwo_scan(zwo_t *o, const zw_node_record_t *rec);
void zwo_forget(zwo_t *o, uint16_t node);
/* Ask every output of the device where it is (after a scan, a wake of the link). */
void zwo_refresh(zwo_t *o, uint64_t now, uint16_t node);

/* A command from `node` (only 25 and 26 Reports are taken): true when it was an output's. */
bool zwo_received(zwo_t *o, uint64_t now, uint16_t node, uint8_t endpoint, const uint8_t *cmd,
                  size_t len);
bool zwo_send_done(zwo_t *o, uint64_t now, uint32_t tag, bool delivered);
void zwo_poll(zwo_t *o, uint64_t now);
uint64_t zwo_next_deadline(const zwo_t *o);

/* The output of (node, endpoint), or -1. */
int zwo_find(const zwo_t *o, uint16_t node, uint8_t endpoint);
/* Switch it: level 0 off, 100 on (a switch or relay: only those), 1..99 a dimmer's level.
 * `issued_ms` is when the person asked (now for the panel's own pages). */
zwo_result_t zwo_set(zwo_t *o, uint64_t now, size_t index, int level, uint64_t issued_ms,
                     uint32_t cmd);

/* A garage from a relay output and a tilt sensor's zone; travel 0: ZWO_TRAVEL_S_DEF. */
zwo_result_t zwo_garage_assign(zwo_t *o, size_t slot, size_t output, uint16_t zone,
                               uint16_t travel_s);
/* No longer a garage: its relay a plain output again. ZWO_ERR_BUSY while a move is under way
 * (pressed, held, released or travelling): its release must not be dropped (PL03). */
zwo_result_t zwo_garage_clear(zwo_t *o, size_t slot);
zwo_result_t zwo_garage_move(zwo_t *o, uint64_t now, size_t slot, zwo_position_t goal,
                             uint64_t issued_ms, uint32_t cmd);
/* Where its sensor says it is: "open", "closed", "unknown". */
const char *zwo_garage_state(const zwo_t *o, size_t slot);
/* A press of it is under way (pressed, released, or its sensor awaited): true, and *goal. */
bool zwo_garage_moving(const zwo_t *o, size_t slot, zwo_position_t *goal);
/* The longest a move of it can take from zwo_garage_move to its end, ms (0: no such garage):
 * the press, its pulse, every off it may try or have refused, then its travel time, plus
 * ZWO_BUDGET_SLACK_MS for the polls. A caller waiting for the end waits at least this long
 * (HCTL-012's backstop, the automations' result wait). At ZWO_TRAVEL_S_MAX: 232 s. */
#define ZWO_BUDGET_SLACK_MS 2000u
uint32_t zwo_garage_budget_ms(const zwo_t *o, size_t slot);

/* A started command's end (the result callback's) in the words device_service's caller takes
 * (HCTL-012): "" done; "failed" for a door its sensor says didn't get there (pressed: a definite
 * failure); "timeout" for everything else - once sent, a command that wasn't confirmed (not
 * acknowledged, no answer, a relay that won't let go, dropped with its device) may have
 * happened. */
const char *zwo_end_word(zwo_result_t r);

/* The garage store (NVS zwave/garage): 'G' 'A' | format 1 | per slot: used, node u16, generation
 * u16, endpoint, zone u16, travel u16 | CRC-16. Decoding needs the outputs scanned first. */
#define ZWO_GARAGE_BLOB (4u + ZWO_GARAGES * 10u + 2u)
size_t zwo_garage_encode(const zwo_t *o, uint8_t *out, size_t cap);
bool zwo_garage_decode(zwo_t *o, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_OUTPUTS_H */
