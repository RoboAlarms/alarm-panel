/*
 * zone_eol.h - wired loops with end-of-line resistors (docs/features/02 ZONE-005, docs/features/22,
 * BL-063): what a loop's voltage says, and what that means for its zone.
 *
 * The loop: the terminal pulled up to the reference by ZEOL_PULLUP_OHM (a resistor the installer
 * fits at the terminal: the chip's own pull-up is too loose to measure with), the loop to ground
 * through the sensor's contact and ZEOL_OHM at its far end. Its resistance follows from the voltage:
 * R = pull-up x V / (Vref - V). Three wirings, chosen per zone as the wired address's `input`
 * (stored numbers: never renumbered, only added):
 *   ZEOL_IN_NC   a normally closed contact in series with the resistor (doors, windows, most
 *                motion sensors): the resistor is normal, open is the sensor tripped (FAULT),
 *                a short across the loop is sabotage (TAMPER);
 *   ZEOL_IN_NO   normally open devices across the resistor (smoke detectors, panic buttons): the
 *                resistor is normal, a short is the sensor tripped (FAULT), open is a cut wire
 *                (TROUBLE) - the fire loop of SIA and NFPA 72;
 *   ZEOL_IN_DEOL two resistors (the contact across the second, a tamper switch in series): one
 *                resistor is normal, two is tripped (FAULT), open or short is tampering (TAMPER) -
 *                DSC's DEOL.
 * Bands, as the classic panels read them: short below half the resistor; normal up to the
 * geometric middle of one and two resistors (DEOL) or five times it (single); tripped (DEOL) up to
 * five times it; open beyond. With 2 kOhm and 4.7 kOhm: short < 1 k (0.58 V at 3.3 V), normal
 * 1-2.8 k (DEOL) or 1-10 k, tripped 2.8-10 k, open > 10 k (2.24 V).
 *
 * Portable C11, no state.
 */
#ifndef ZONE_EOL_H
#define ZONE_EOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The wired address's input (zsrc_address_t.input for ZSRC_WIRED): stored, never renumber. */
#define ZEOL_IN_NONE 0u /* a plain contact, read as a level (ZONE-004) */
#define ZEOL_IN_NC   1u
#define ZEOL_IN_NO   2u
#define ZEOL_IN_DEOL 3u
#define ZEOL_IN_LAST ZEOL_IN_DEOL

#define ZEOL_OHM        2000u /* the end-of-line resistor (decided 2026-09-28: a single 2 kOhm) */
#define ZEOL_PULLUP_OHM 4700u /* the pull-up the installer fits at the terminal */
#define ZEOL_OPEN_OHM   (5u * ZEOL_OHM)
#define ZEOL_SHORT_OHM  (ZEOL_OHM / 2u)
#define ZEOL_DEOL_OHM   2828u /* sqrt(2) x ZEOL_OHM: between one and two resistors */

typedef enum {
    ZEOL_SHORT = 0,
    ZEOL_NORMAL,
    ZEOL_TRIPPED, /* DEOL only: two resistors */
    ZEOL_OPEN,
} zeol_reading_t;

typedef struct {
    bool fault;   /* the sensor tripped */
    bool tamper;  /* the loop is tampered with */
    bool trouble; /* the loop is broken (a fire loop's cut wire) */
} zeol_meaning_t;

/* The loop's resistance in ohms from its voltage; UINT32_MAX at or above the reference (open). */
uint32_t zeol_ohms(uint32_t mv, uint32_t vref_mv);
/* What the voltage reads as, for a loop wired as `input` (ZEOL_IN_*; NONE reads as NC). */
zeol_reading_t zeol_classify(uint8_t input, uint32_t mv, uint32_t vref_mv);
/* What a reading means for the zone. */
zeol_meaning_t zeol_mean(uint8_t input, zeol_reading_t reading);
/* "short", "normal", "tripped", "open": the console's words. */
const char *zeol_reading_name(zeol_reading_t reading);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_EOL_H */
