/*
 * ui_dev_layout_fixture_v1.h - a Smart Home layout blob as version 1 writes it (ui_dev_model.h),
 * written out by hand from the format's description, not by the encoder. FROZEN: never edit it. A
 * later version adds its own fixture next to this one, and this one must keep reading the same.
 *   compat 1, three entries:
 *     Favorites, two cells:   light.porch
 *     Favorites, kind's size: lock.front_door
 *     not on Favorites, one cell: cover.garage
 *   then two bytes of a later version's section, which version 1 ignores.
 */
#pragma once

#include <stdint.h>

static const uint8_t UI_DEV_LAYOUT_FIXTURE_V1[] = {
    0x01, 0x03,
    0x05, 11, 'l', 'i', 'g', 'h', 't', '.', 'p', 'o', 'r', 'c', 'h',
    0x01, 15, 'l', 'o', 'c', 'k', '.', 'f', 'r', 'o', 'n', 't', '_', 'd', 'o', 'o', 'r',
    0x02, 12, 'c', 'o', 'v', 'e', 'r', '.', 'g', 'a', 'r', 'a', 'g', 'e',
    0xEE, 0x01,
};
/* The entries alone, without the trailing section: what version 1 writes back. */
#define UI_DEV_LAYOUT_FIXTURE_V1_ENTRIES_LEN 46u
