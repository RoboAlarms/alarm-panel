/*
 * zwz_store.h - what zwz_panel says changed, written to the NVS namespace "zwave" (docs/features/36
 * network.md §2; ZW-017, ZW-039). One copy for the firmware's zwave_service and the PC simulator.
 *
 * A pass takes every dirty value from zwz_panel (zwz_panel_take_*) under the owner's lock, then,
 * with the lock released, writes them in this order, each one set and then committed:
 *   keys; gen, then nodes (nodes only after gen stored: a cache never outlives its counter);
 *   cfg; chk; garage; prov; siren.
 * A value that was not stored - its set or commit refused, gen refused before nodes, or a value too
 * big for its buffer (it stayed dirty in zwz_panel) - is asked for again with zwz_panel_redo(), and
 * nothing is tried before ZWZ_STORE_RETRY_MS has passed. What was stored is told with
 * zwz_panel_stored(), under the lock again (ZW-014: the node cache's ends zwz_panel_identity_held).
 * Every pass that tried to write tells the
 * port's trouble() whether anything failed in it: not latched, so a later pass that stores
 * everything says false. Nothing is called while nothing is dirty or a retry is waited out.
 * The keys' buffer is wiped once written.
 *
 * Portable C11, caller-owned state, no heap. The owner serializes calls.
 */
#ifndef ZWZ_STORE_H
#define ZWZ_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_zones/zwz_panel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZWZ_STORE_RETRY_MS 30000u /* a refused write: everything tried again this much later */

/* The NVS keys (network.md §2): never renamed - stored panels hold them. */
#define ZWZ_STORE_KEY_KEYS   "keys"
#define ZWZ_STORE_KEY_GEN    "gen"
#define ZWZ_STORE_KEY_NODES  "nodes"
#define ZWZ_STORE_KEY_CFG    "cfg"
#define ZWZ_STORE_KEY_CHK    "chk"
#define ZWZ_STORE_KEY_GARAGE "garage"
#define ZWZ_STORE_KEY_PROV   "prov"
#define ZWZ_STORE_KEY_SIREN  "siren" /* zw_siren.h ZWS_KEY */

typedef struct {
    void *ctx;
    /* the owner's lock around zwz_panel (the zone service's); NULL when there is none */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* one value into the namespace; true when it was accepted */
    bool (*set_blob)(void *ctx, const char *key, const uint8_t *data, size_t len);
    bool (*set_u16)(void *ctx, const char *key, uint16_t v);
    /* after every accepted set: true when it is kept; NULL when set_* already keeps it */
    bool (*commit)(void *ctx);
    /* after every pass that tried to write: whether anything failed in this pass; may be NULL */
    void (*trouble)(void *ctx, bool failed);
    /* the caller's buffers for the two big values (their encoders' largest; the rest are on the
     * stack): the node cache and the SmartStart list */
    uint8_t *nodes_buf;
    size_t nodes_cap;
    uint8_t *prov_buf;
    size_t prov_cap;
} zwz_store_port_t;

typedef struct {
    uint64_t retry_at; /* 0: none waited for */
} zwz_store_t;

void zwz_store_init(zwz_store_t *s);

/* One pass at `now` (ms): the ZWZ_DIRTY_* bits that failed in it, 0 when everything due was stored
 * or nothing was due (or a retry is still waited out). */
uint8_t zwz_store_run(zwz_store_t *s, zwz_panel_t *p, uint64_t now, const zwz_store_port_t *port);

/* ---- reading a stored value back (zwave_service's load_blob; network.md §2, ZW-039) ----------
 * A value is read at its stored length, not today's size, so a longer one from newer firmware (an
 * OTA rollback) reaches its decoder, which takes what it knows. That holds for keys, gen, nodes,
 * cfg, chk, garage, prov and siren, whose decoders accept longer input; keypad_fb and keypad_ctl
 * (zw_feedback.c, zw_keyctl.c) still need their exact length, so a longer one is refused there. */

/* The most a value whose encoder writes at most `today` bytes is read at: twice that and some,
 * never under ZWZ_STORE_READ_FLOOR (what the firmware before this read cfg and keys at, so nothing
 * it read is refused now). Anything longer is refused as damaged. */
#define ZWZ_STORE_READ_FLOOR 1024u
size_t zwz_store_read_max(size_t today);

typedef enum {
    ZWZ_STORE_READ_ABSENT = 0, /* never stored (or stored empty): the value's default */
    ZWZ_STORE_READ_REFUSED,    /* unreadable or longer than max: logged, then the default */
    ZWZ_STORE_READ_OK,         /* read whole, `stored` bytes, for its decoder */
} zwz_store_read_t;

/* What to do with a stored value: `found` false when the store says it was never written,
 * `readable` false when it was there but its length could not be read, `stored` its length. */
zwz_store_read_t zwz_store_read_verdict(bool found, bool readable, size_t stored, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* ZWZ_STORE_H */
