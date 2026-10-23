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
 *   ARMED       on while any of its partitions is armed;
 *   AUTOMATION  "For automations" (BL-224, docs/features/41): on only while an automation has set
 *               it on (ow_auto_set) or "Test it" runs; it never follows the engine (no alarm, no
 *               chirp, no armed state: sirens stay outside automations, AUT-002), has no
 *               partitions and no supervision, and a name typed like a zone's ("Garden Lights").
 * The line is driven `on` as high, or low with `active_low` (most relay boards switch on low).
 *
 * Safe at boot (OUT-006): a channel's pin is made an output at its inactive level the moment it
 * is configured, before anything can turn it on, and the engine's outputs decide from then on.
 * An AUTOMATION channel starts off, at boot and whenever it is configured again (OUT-024).
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
#define OW_NAME_LEN   24u    /* an AUTOMATION channel's name, NUL included (a zone's is 24) */
/* The outputs for automations (BL-224) are a key of their own, so `wired` stays what firmware
 * from before BL-224 reads whole (its sirens survive a downgrade). */
#define OW_AUTO_KEY      "auto"
#define OW_AUTO_FORMAT   1u
#define OW_AUTO_ENTRY    3u  /* slot | gpio | flags, before the name */
#define OW_AUTO_BLOB_MAX (4u + OW_CHANNELS * (1u + OW_AUTO_ENTRY + 1u + OW_NAME_LEN - 1u) + 2u)

/* Stored: never renumber, only add. */
typedef enum {
    OW_FN_NONE = 0,
    OW_FN_SIREN = 1,
    OW_FN_STROBE = 2,
    OW_FN_SMOKE_RESET = 3,
    OW_FN_ARMED = 4,
    OW_FN_AUTOMATION = 5, /* BL-224: switched by automations only */
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
    char name[OW_NAME_LEN]; /* AUTOMATION: its name (UTF-8, may be empty); "" for the others */
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
    bool auto_on;        /* AUTOMATION: an automation set it on */
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
                     * SIREN with neither burglary nor fire, sense on its own pin; ow_auto_set on
                     * a channel that isn't an AUTOMATION one */
    OW_ERR_IN_USE,  /* the pin (or sense pin) is another channel's */
    OW_ERR_PIN,     /* the port couldn't set the pin up */
    OW_ERR_ARMED,   /* ow_test with one of its partitions armed */
    OW_ERR_BUSY,    /* ow_test while it sounds */
} ow_result_t;

void ow_init(ow_t *o, const ow_port_t *port);
/* Configure (or reconfigure) channel `slot`: its pin is set up at once at its inactive level.
 * A changed pin releases the old one. An AUTOMATION channel keeps only its gpio, active_low and
 * name (the rest is cleared: no partitions, no supervision) and starts off; another function's
 * name is cleared. */
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
/* "Test it": a SIREN, STROBE or AUTOMATION channel on for OW_TEST_MS; a siren or strobe is
 * refused while any of its partitions is armed or while it sounds. An AUTOMATION channel goes
 * back to what automations set when the test ends. */
ow_result_t ow_test(ow_t *o, uint64_t now, size_t slot);

/* Automations (BL-224, OUT-005, OUT-024): an AUTOMATION channel on or off, honouring active_low.
 * OW_ERR_ARG: no such channel or not an AUTOMATION one; OW_ERR_PIN: its pin isn't driven. */
ow_result_t ow_auto_set(ow_t *o, uint64_t now, size_t slot, bool on);
/* Bit n: channel n (slot, 0-based) is an AUTOMATION channel now. */
uint8_t ow_auto_mask(const ow_t *o);

void ow_poll(ow_t *o, uint64_t now);
/* When to poll next: the next pattern edge, a test's or squawk's end, a supervision input's
 * limit, or the 100 ms read of the inputs; UINT64_MAX when nothing is configured. */
uint64_t ow_next_deadline(const ow_t *o, uint64_t now);

/* Words for the console and pages: "siren", "strobe", "smoke reset", "armed", "automation", ""
 * none. */
const char *ow_function_name(uint8_t function);

/* The store (NVS outputs/wired): 'O' 'W' | format | count | count x (length | slot | function |
 * gpio | partitions | flags (b0 burglary, b1 fire, b2 squawk, b3 active_low) | sense | also |
 * reserved)
 * | CRC-16/CCITT of all before it. A shorter entry reads its missing fields as 0 (sense as
 * none); a longer one's extra bytes are skipped. All or nothing: a damaged blob changes nothing.
 * AUTOMATION channels are never written here (they are OW_AUTO_KEY's); an entry of a function
 * this firmware doesn't keep here (AUTOMATION, or a newer one) is skipped, that channel free.
 * ow_decode only fills the configuration; the owner then ow_set()s each (pins). */
size_t ow_encode(const ow_t *o, uint8_t *out, size_t cap);
bool ow_decode(const uint8_t *buf, size_t len, ow_config_t out[OW_CHANNELS]);

/* The outputs for automations (NVS outputs/auto, BL-224, OUT-023): 'O' 'A' | format | count |
 * count x (length | slot | gpio | flags (b0 active_low) | name length | name bytes) | CRC-16/CCITT
 * of all before it. A shorter entry reads its missing fields as 0 (the name empty); a longer
 * one's extra bytes are skipped. All or nothing. ow_auto_decode fills out[slot] as an AUTOMATION
 * channel for each entry and leaves every other slot as it was (the owner decodes `wired` first;
 * a slot `wired` already holds keeps that). */
size_t ow_auto_encode(const ow_t *o, uint8_t *out, size_t cap);
bool ow_auto_decode(const uint8_t *buf, size_t len, ow_config_t out[OW_CHANNELS]);

#ifdef __cplusplus
}
#endif

#endif /* SIRENS_OUT_WIRED_H */
