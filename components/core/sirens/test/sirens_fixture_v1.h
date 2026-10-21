/*
 * sirens_fixture_v1.h - the sirens' stored blobs, format 1, frozen (OUT-009, OUT-013). Written
 * by hand from the layouts in out_wired.h and rf_siren.h, the CRC computed apart from the C code.
 * Never edit: a format change adds a v2 file beside this one.
 */
#ifndef SIRENS_FIXTURE_V1_H
#define SIRENS_FIXTURE_V1_H

#include <stdint.h>

/* outputs/wired: channel 1 a siren on pin 4 for partitions 1-2 and also 3's alarms, burglary,
 * fire, squawk, active low, supervised on pin 5; channel 3 "armed" on pin 2 for partition 1. */
static const uint8_t OW_FIXTURE_V1[] = {
    0x4F, 0x57, 0x01, 0x02,
    0x08, 0x01, 0x01, 0x04, 0x03, 0x0F, 0x06, 0x04, 0x00,
    0x08, 0x03, 0x04, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
    0xC5, 0x3D,
};

/* outputs/rf: siren 2 at address A1B2C (EV1527), the default codes (alarm 8, stop 1, arm 2,
 * disarm 1), partitions 1 and 3, also partition 2's alarms, burglary, squawk, the default T. */
static const uint8_t RFS_FIXTURE_V1[] = {
    0x52, 0x53, 0x01, 0x01,
    0x0F, 0x02, 0x01, 0x00, 0x0A, 0x1B, 0x2C, 0x08, 0x01, 0x02, 0x01, 0x05, 0x05, 0x00, 0x00, 0x02,
    0xDF, 0xFF,
};

#endif /* SIRENS_FIXTURE_V1_H */
