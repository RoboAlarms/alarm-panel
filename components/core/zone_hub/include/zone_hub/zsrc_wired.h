/*
 * zsrc_wired.h - wired zone inputs (docs/features/22, ZONE-004): a dry contact between a terminal
 * GPIO and GND, read with the internal pull-up. A closed loop reads 0; an open loop (door open,
 * or a cut wire) reads 1 and is reported as value 1, "active". Normally open contacts are the
 * binding's 'invert'. Bound terminals are sampled every ZSRC_WIRED_POLL_MS and debounced with
 * each binding's response time (default 350 ms). Only terminals a zone is bound to are
 * configured, and all free ones while learning ("open and close the sensor"); those are
 * debounced too (350 ms), so a loose wire can't make the picker's words flicker.
 *
 * End-of-line loops (ZONE-005, zone_eol.h): a terminal that can measure (`eol`) may be bound with
 * the address input ZEOL_IN_NC, _NO or _DEOL instead of 0. Its pin is then an input without the
 * chip's pull-up (the installer fits ZEOL_PULLUP_OHM), sampled in millivolts every poll, its
 * reading debounced with the same response time, and reported as the zone's fault (an input of
 * 0/1, so its mapping's invert is never used) and as TAMPER / TROUBLE events, each with its restore.
 *
 * Portable C11: the pins are reached through zsrc_gpio_port_t (ESP-IDF gpio on the panel, a
 * fake in the host tests and the PC preview).
 */
#ifndef ZSRC_WIRED_H
#define ZSRC_WIRED_H

#include "zone_hub/zone_debounce.h"
#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZSRC_WIRED_MAX        16   /* terminals */
#define ZSRC_WIRED_POLL_MS    10u  /* sampling period */
#define ZSRC_WIRED_RESPONSE_MS 350u /* default response time (ZONE-004) */

typedef struct {
    void *ctx;
    /* on: make the pin an input with its pull-up; off: give the pin back (reset it). */
    bool (*setup)(void *ctx, uint8_t gpio, bool on);
    /* The pin's level, 0 or 1. */
    uint8_t (*read)(void *ctx, uint8_t gpio);
    /* End-of-line loops (optional; NULL: no terminal measures): the pin as an analog input without
     * pull-up (on) or given back (off); its voltage and the reference, in millivolts (false: no
     * reading now). */
    bool (*setup_analog)(void *ctx, uint8_t gpio, bool on);
    bool (*read_mv)(void *ctx, uint8_t gpio, uint32_t *mv, uint32_t *vref_mv);
} zsrc_gpio_port_t;

typedef struct {
    uint8_t gpio;
    const char *where; /* "J7 header" */
    bool eol;          /* it can measure: end-of-line loops (ZONE-005) are offered on it */
} zsrc_wired_terminal_t;

typedef struct {
    bool watched;        /* a zone is bound to it */
    bool setup;          /* configured as an input */
    bool changed;        /* changed since learning started */
    uint8_t learn_level; /* its level when learning started */
    zone_debounce_t db;  /* the level that counts, whenever it is set up */
    bool reserved;       /* an output's pin (OUT-021): never set up, offered or bound here */
    uint8_t mode;        /* ZEOL_IN_* it is watched as (0: a plain contact) */
    bool analog;         /* set up as an analog input (an end-of-line loop) */
    bool told;           /* an end-of-line loop's state was reported */
    bool tamper, trouble; /* ... and its tamper and trouble as last reported */
    uint32_t mv;         /* its last reading, for the console */
} zsrc_wired_term_t;

typedef struct {
    zsrc_sink_t sink;
    zsrc_gpio_port_t gpio;
    const zsrc_wired_terminal_t *terms;
    size_t n;
    zsrc_wired_term_t st[ZSRC_WIRED_MAX];
    bool learning;
} zsrc_wired_t;

/* terms must stay valid (a static table); at most ZSRC_WIRED_MAX are used. */
void zsrc_wired_init(zsrc_wired_t *w, const zsrc_wired_terminal_t *terms, size_t n,
                     const zsrc_gpio_port_t *gpio, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_wired_ops(void); /* register with the zsrc_wired_t as ctx */

zsrc_address_t zsrc_wired_address(uint8_t gpio);
/* The same terminal as an end-of-line loop (input ZEOL_IN_*). */
zsrc_address_t zsrc_wired_eol_address(uint8_t gpio, uint8_t input);
/* Whether the terminal can be an end-of-line loop. */
bool zsrc_wired_eol_capable(const zsrc_wired_t *w, uint8_t gpio);

/* OUT-021: a terminal taken by an output (docs/features/16) or given back. A reserved terminal is
 * not a valid address, is never set up while learning and is not a candidate. Refused (false)
 * while a zone watches it, and for a pin that is no terminal. */
bool zsrc_wired_reserve(zsrc_wired_t *w, uint8_t gpio, bool reserved);
/* A zone watches this terminal. */
bool zsrc_wired_in_use(const zsrc_wired_t *w, uint8_t gpio);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_WIRED_H */
