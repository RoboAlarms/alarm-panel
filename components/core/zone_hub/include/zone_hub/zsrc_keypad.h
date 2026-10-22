/*
 * zsrc_keypad.h - a keypad's own wired inputs as the master's zones (docs/features/11 LINK-055,
 * docs/features/22; ZSRC_KEYPAD). A panel in the keypad role - a headless board in a cupboard, or
 * a touchscreen by the door - reads its wired terminals itself and sends their debounced levels
 * over the panel link (IO_STATE); the master's zone hub binds a zone to "keypad 2, IO5" through
 * this source like any other sensor.
 *
 * The link carries each terminal's level and a count of its changes: when the count moved by more
 * than the level shows (a frame lost while a door opened and closed again), the missed opening is
 * reported before the level now, so no fault goes unseen. A keypad the link stops hearing makes
 * its watched inputs unreachable: their zones run their grace into CHECK (ZONE-008). A keypad
 * reports every 5 s (PLINK_IO_EVERY_MS): one silent for ZSRC_KEYPAD_STALE_MS counts as unheard here
 * at once, without waiting for the link's own supervision (minutes).
 *
 * Address: kind ZSRC_KEYPAD, input 0, id = device << 8 | gpio (device 1..8, the link's keypad
 * number). Portable C11 like the other sources: plink_service feeds it on the master, the tests
 * feed it directly. Calls come with the platform's lock held (zone_service).
 */
#ifndef ZSRC_KEYPAD_H
#define ZSRC_KEYPAD_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZSRC_KEYPAD_DEVICES   8u  /* the link's keypads 1..8 */
#define ZSRC_KEYPAD_TERMS     16u /* terminals per keypad (PLINK_IO_MAX) */
#define ZSRC_KEYPAD_GRACE_S   60u    /* a keypad unheard this long: its zones CHECK */
#define ZSRC_KEYPAD_STALE_MS  20000u /* no report this long: the keypad is unheard */

#define ZSRC_KEYPAD_ACTIVE 0x01u /* a terminal's loop is open (PLINK_IO_ACTIVE) */
#define ZSRC_KEYPAD_KNOWN  0x02u /* the keypad has read it (PLINK_IO_KNOWN) */

/* One terminal as a keypad reported it. */
typedef struct {
    uint8_t gpio;
    uint8_t flags;    /* ZSRC_KEYPAD_* */
    uint16_t changes; /* the keypad's count of its level changes, wrapping */
} zsrc_keypad_term_t;

typedef struct {
    bool known;     /* a report came since this start */
    bool online;    /* the link hears the keypad */
    uint64_t heard; /* its last report */
    uint8_t n;
    zsrc_keypad_term_t term[ZSRC_KEYPAD_TERMS];
    uint64_t watched; /* bit gpio: a zone is bound to that terminal (kept before any report) */
    uint8_t learn_flags[ZSRC_KEYPAD_TERMS]; /* its level when learning began */
    bool changed[ZSRC_KEYPAD_TERMS];        /* changed while learning */
} zsrc_keypad_dev_t;

typedef struct {
    zsrc_sink_t sink;
    bool learning;
    zsrc_keypad_dev_t dev[ZSRC_KEYPAD_DEVICES];
} zsrc_keypad_t;

void zsrc_keypad_init(zsrc_keypad_t *k, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_keypad_ops(void); /* register with the zsrc_keypad_t as ctx */

zsrc_address_t zsrc_keypad_address(uint8_t device, uint8_t gpio);

/* ---- fed by plink_service on the master (or a test) ------------------------------------------ */

/* Keypad device's terminals, its newest report (n at most ZSRC_KEYPAD_TERMS are used). */
void zsrc_keypad_report(zsrc_keypad_t *k, uint8_t device, const zsrc_keypad_term_t *terms,
                        size_t n, uint64_t now);
/* The link hears keypad device again, or stopped hearing it (supervision). */
void zsrc_keypad_online(zsrc_keypad_t *k, uint8_t device, bool online, uint64_t now);
/* Keypad device was removed from the link: what it reported is forgotten (its zones CHECK). */
void zsrc_keypad_forget(zsrc_keypad_t *k, uint8_t device, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_KEYPAD_H */
