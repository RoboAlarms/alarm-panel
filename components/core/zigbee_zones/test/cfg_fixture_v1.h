/*
 * cfg_fixture_v1.h - frozen `zbpanel/cfg` blobs, compat 1 (zbz_panel.h; ZB-016). Written by hand,
 * byte by byte, from the layout in zbz_panel.h and devices.md §4 - never by zbz_cfg_encode():
 * never edit them; a later format adds cfg_fixture_v2.h beside this one.
 *
 *   compat (1) | flags (bit 0: a channel was chosen) | channel | 0
 */
#ifndef CFG_FIXTURE_V1_H
#define CFG_FIXTURE_V1_H

#include <stdint.h>

/* The installer chose channel 20. */
static const uint8_t CFG_FIXTURE_V1_CHANNEL_20[] = {0x01, 0x01, 0x14, 0x00};

/* Nothing chosen: the build's channel mask decides. */
static const uint8_t CFG_FIXTURE_V1_DEFAULT[] = {0x01, 0x00, 0x00, 0x00};

#endif /* CFG_FIXTURE_V1_H */
