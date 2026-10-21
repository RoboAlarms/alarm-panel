/*
 * out_wired.h - the panel's wired outputs (docs/features/16 OUT-002..OUT-006, OUT-009): a siren,
 * a strobe, the smoke detectors' power or an "armed" light on a terminal, through a relay or
 * MOSFET module (never a siren straight off a pin: a 12 V siren has its own supply and driver).
 *
 * Each channel is a terminal (the same J7 pins the wired zones use; the owner keeps the two
 * apart) and a function:
 *   SIREN       the bell: steady for burglary, temporal-3 for fire, temporal-4 for CO, the bell
 *               test's pulse, and chirps when its partitions arm (one) and disarm (two) when
 *               `squawk` is on - for the partitions and alarms of its scope (siren.h);
 *   STROBE      on while any of its partitions' strobe output is on;
 *   SMOKE_RESET on while the engine interrupts the smoke detectors' power (system);
 *   ARMED       on while any of its partitions is armed.
 * The line is driven `on` as high, or low with `active_low` (most relay boards switch on low).
 *
 * Safe at boot (OUT-006): a channel's pin is made an output at its inactive level the moment it
 * is configured, before anything can turn it on, and the engine's outputs decide from then on.
 *
 * Bell supervision (OUT-004): a channel may name a second terminal, `sense`, wired to the
 * siren's fault contact or a supervision loop (closed = healthy, to ground with the pull-up).
 * Open for OW_SENSE_MS is the bell trouble (ALARM_TRBL_BELL, through the port); closed again
 * clears it.
 *
 * Patterns are exact to the poll: ow_next_deadline() says when the next edge is due; the owner
 * runs ow_poll() then (a high-priority timer on the panel).
 *
 * Portable C11, caller-owned state, no heap. The owner serializes calls.
 */
#ifndef SIRENS_OUT_WIRED_H
#define SIRENS_OUT_WIRED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sirens/siren.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OW_CHANNELS   4u
#define OW_SENSE_MS   10000u /* a supervision input open this long is the bell trouble */
#define OW_TEST_MS    2000u  /* "Test it" */
#define OW_NAMESPACE  "outputs"
#define OW_KEY        "wired"
#define OW_FORMAT     1u
#define OW_ENTRY      8u     /* bytes after an entry's length byte */
#define OW_BLOB_MAX   (4u + OW_CHANNELS * (1u + OW_ENTRY) + 2u)

/* Stored: never renumber, only add. */
typedef enum {
    OW_FN_NONE = 0,
    OW_FN_SIREN = 1,
    OW_FN_STROBE = 2,
    OW_FN_SMOKE_RESET = 3,
    OW_FN_ARMED = 4,
    OW_FN_COUNT
} ow_function_t;

#define OW_SENSE_NONE 0xFFu

typedef struct {
    uint8_t function;   /* ow_function_t; NONE: the channel is free */
    uint8_t gpio;       /* the terminal it drives */
    uint8_t partitions; /* bit 0 = partition 1 (SIREN, STROBE, ARMED) */
    uint8_t also;       /* SIREN, STROBE: other partitions whose alarms it also sounds for (OUT-015) */
    bool burglary;      /* SIREN: burglary and audible panic */
    bool fire;          /* SIREN: fire and CO */
    bool squawk;        /* SIREN: chirps when its partitions arm and disarm */
    bool active_low;    /* the line is low when on */
    uint8_t sense;      /* the supervision input's terminal, OW_SENSE_NONE none */
} ow_config_t;

typedef struct {
    ow_config_t c;
    bool on;             /* the level it should have, logically */
    bool driven;         /* what the pin was last set to, logically */
    bool pin_ok;         /* the port configured its pin */
    siren_sound_t sound; /* SIREN: the sound under way */
    uint64_t sound_at;   /* ... since */
    uint8_t chirps;      /* a squawk under way: how many */
    uint64_t chirp_at;
    uint64_t test_until; /* "Test it" under way until then, 0 none */
    uint64_t next;       /* the next edge of its pattern, 0 none */
    /* supervision */
    bool sense_ok;       /* its sense pin was configured */
    bool sense_open;
    uint64_t sense_since;
    bool trouble;
} ow_channel_t;

typedef struct {
    void *ctx;
    /* The pin as an output at `level` (true high), or - output false - an input with its
     * pull-up; false: it can't be. */
    bool (*setup)(void *ctx, uint8_t gpio, bool output, bool level);
    /* Give the pin back (as the wired zones' terminals are). */
    void (*release)(void *ctx, uint8_t gpio);
    void (*write)(void *ctx, uint8_t gpio, bool level);
    uint8_t (*read)(void *ctx, uint8_t gpio);
    /* ALARM_TRBL_BELL on or off (any channel in trouble), for the owner to post. */
    void (*bell_trouble)(void *ctx, bool active);
} ow_port_t;

typedef struct {
    ow_port_t port;
    ow_channel_t ch[OW_CHANNELS];
    siren_view_t view;
    bool trouble; /* as last told */
    bool dirty;   /* the configuration is to be stored */
} ow_t;

typedef enum {
    OW_OK = 0,
    OW_ERR_ARG,     /* no such channel or function, a SIREN/STROBE/ARMED with no partition, a
                     * SIREN with neither burglary nor fire, sense on its own pin */
    OW_ERR_IN_USE,  /* the pin (or sense pin) is another channel's */
    OW_ERR_PIN,     /* the port couldn't set the pin up */
    OW_ERR_ARMED,   /* ow_test with one of its partitions armed */
    OW_ERR_BUSY,    /* ow_test while it sounds */
} ow_result_t;

void ow_init(ow_t *o, const ow_port_t *port);
/* Configure (or reconfigure) channel `slot`: its pin is set up at once at its inactive level.
 * A changed pin releases the old one. */
ow_result_t ow_set(ow_t *o, uint64_t now, size_t slot, const ow_config_t *c);
/* Free the channel: its pin goes inactive, then is released. */
ow_result_t ow_clear(ow_t *o, size_t slot);
/* The channel using `gpio` as its output or sense pin, or -1. */
int ow_find_pin(const ow_t *o, uint8_t gpio);

/* The engine's news. */
void ow_output(ow_t *o, uint64_t now, uint8_t partition, alarm_output_t out,
               alarm_pattern_t pattern);
/* A partition armed or disarmed: its sirens with `squawk` chirp (not while sounding). */
void ow_armed(ow_t *o, uint64_t now, uint8_t partition, bool armed);
/* Its armed state without a chirp (start-up, from a snapshot). */
void ow_set_armed(ow_t *o, uint64_t now, uint8_t partition, bool armed);
/* "Test it": a SIREN or STROBE on for OW_TEST_MS; refused while any of its partitions is armed
 * or while it sounds. */
ow_result_t ow_test(ow_t *o, uint64_t now, size_t slot);

void ow_poll(ow_t *o, uint64_t now);
/* When to poll next: the next pattern edge, a test's or squawk's end, a supervision input's
 * limit, or the 100 ms read of the inputs; UINT64_MAX when nothing is configured. */
uint64_t ow_next_deadline(const ow_t *o, uint64_t now);

/* Words for the console and pages: "siren", "strobe", "smoke reset", "armed", "" none. */
const char *ow_function_name(uint8_t function);

/* The store (NVS outputs/wired): 'O' 'W' | format | count | count x (length | slot | function |
 * gpio | partitions | flags (b0 burglary, b1 fire, b2 squawk, b3 active_low) | sense | also |
 * reserved)
 * | CRC-16/CCITT of all before it. A shorter entry reads its missing fields as 0 (sense as
 * none); a longer one's extra bytes are skipped. All or nothing: a damaged blob changes nothing.
 * ow_decode only fills the configuration; the owner then ow_set()s each (pins). */
size_t ow_encode(const ow_t *o, uint8_t *out, size_t cap);
bool ow_decode(const uint8_t *buf, size_t len, ow_config_t out[OW_CHANNELS]);

#ifdef __cplusplus
}
#endif

#endif /* SIRENS_OUT_WIRED_H */
