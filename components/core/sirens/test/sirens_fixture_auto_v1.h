/*
 * sirens_fixture_auto_v1.h - the outputs for automations' stored blob (NVS outputs/auto, BL-224,
 * OUT-023), format 1, frozen; and the outputs/wired blob written beside it, which holds no output
 * for automations (format 1 as before, so firmware from before BL-224 reads it whole). Written by
 * hand from the layouts in out_wired.h, the CRCs computed apart from the C code. Never edit: a
 * format change adds a v2 file beside this one.
 */
#ifndef SIRENS_FIXTURE_AUTO_V1_H
#define SIRENS_FIXTURE_AUTO_V1_H

#include <stdint.h>

/* outputs/auto: channel 3 "For automations" on pin 47, active low, named "Garden Lights". */
static const uint8_t OW_AUTO_FIXTURE_V1[] = {
    0x4F, 0x41, 0x01, 0x01,
    0x11, 0x02, 0x2F, 0x01,
    0x0D, 0x47, 0x61, 0x72, 0x64, 0x65, 0x6E, 0x20, 0x4C, 0x69, 0x67, 0x68, 0x74, 0x73,
    0xB0, 0xE8,
};

/* outputs/wired beside it: channel 1 a siren on pin 4 for partition 1, burglary and fire,
 * unsupervised - and nothing of channel 3. */
static const uint8_t OW_FIXTURE_V1_BESIDE_AUTO[] = {
    0x4F, 0x57, 0x01, 0x01,
    0x08, 0x00, 0x01, 0x04, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x40, 0x6B,
};

#endif /* SIRENS_FIXTURE_AUTO_V1_H */
