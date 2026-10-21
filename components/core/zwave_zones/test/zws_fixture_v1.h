/*
 * zws_fixture_v1.h - the Z-Wave sirens' stored blob (NVS zwave/siren), format 1, frozen (OUT-019).
 * Written by hand from the layout in zw_siren.h, the CRC computed apart from the C code (a short
 * Python CRC-16/CCITT-FALSE). Never edit: a format change adds a v2 file beside this one.
 */
#ifndef ZWS_FIXTURE_V1_H
#define ZWS_FIXTURE_V1_H

#include <stdint.h>

/* Slot 1: node 18 generation 3 end point 2, Sound Switch, partitions 1-2, also partition 3's
 * alarms, burglary and squawk (no fire), tones burglary 5, fire 10, CO 0 (burglary's), arm 0x11,
 * disarm 0x12, volume 80 %, supervision 120 min.
 * Slot 3: node 261 generation 1 at the device itself, Binary Switch, partition 1, fire only, the
 * defaults for the rest. */
static const uint8_t ZWS_FIXTURE_V1[] = {
    0x5A, 0x53, 0x01, 0x02,
    0x12, 0x01, 0x00, 0x12, 0x00, 0x03, 0x02, 0x01, 0x03, 0x04, 0x05, 0x05, 0x0A, 0x00, 0x11,
    0x12, 0x50, 0x00, 0x78,
    0x12, 0x03, 0x01, 0x05, 0x00, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xEB, 0xC4,
};

#endif /* ZWS_FIXTURE_V1_H */
