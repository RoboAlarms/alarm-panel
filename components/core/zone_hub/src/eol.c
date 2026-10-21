/*
 * eol.c - wired loops with end-of-line resistors (see zone_eol.h).
 */
#include "zone_hub/zone_eol.h"

uint32_t zeol_ohms(uint32_t mv, uint32_t vref_mv)
{
    if (vref_mv == 0u || mv >= vref_mv) {
        return UINT32_MAX;
    }
    const uint64_t r = (uint64_t)ZEOL_PULLUP_OHM * mv / (vref_mv - mv);
    return r > UINT32_MAX - 1u ? UINT32_MAX - 1u : (uint32_t)r;
}

zeol_reading_t zeol_classify(uint8_t input, uint32_t mv, uint32_t vref_mv)
{
    const uint32_t r = zeol_ohms(mv, vref_mv);
    if (r < ZEOL_SHORT_OHM) {
        return ZEOL_SHORT;
    }
    if (r > ZEOL_OPEN_OHM) {
        return ZEOL_OPEN;
    }
    if (input == ZEOL_IN_DEOL && r > ZEOL_DEOL_OHM) {
        return ZEOL_TRIPPED;
    }
    return ZEOL_NORMAL;
}

zeol_meaning_t zeol_mean(uint8_t input, zeol_reading_t reading)
{
    zeol_meaning_t m = {false, false, false};
    switch (input) {
    case ZEOL_IN_NO:
        m.fault = reading == ZEOL_SHORT;
        m.trouble = reading == ZEOL_OPEN;
        break;
    case ZEOL_IN_DEOL:
        m.fault = reading == ZEOL_TRIPPED;
        m.tamper = reading == ZEOL_OPEN || reading == ZEOL_SHORT;
        break;
    case ZEOL_IN_NC:
    default:
        m.fault = reading == ZEOL_OPEN;
        m.tamper = reading == ZEOL_SHORT;
        break;
    }
    return m;
}

const char *zeol_reading_name(zeol_reading_t reading)
{
    switch (reading) {
    case ZEOL_SHORT: return "short";
    case ZEOL_NORMAL: return "normal";
    case ZEOL_TRIPPED: return "tripped";
    case ZEOL_OPEN: return "open";
    default: return "?";
    }
}
