/*
 * hc_home_fixture_v1.h - the house's Smart Home settings as version 1 writes them (hc_home.h),
 * written out by hand from the format's description, not by the encoder. FROZEN: never edit it. A
 * later version adds its own fixture next to this one, and this one must keep reading the same.
 *   compat 1, arranging asks for a master code;
 *   two rooms: id 3 "Kitchen and dining", made from the area "Kitchen" and renamed;
 *              id 7 "Garage", made by a person;
 *   two devices: climate.hall put in Garage, asking for a master code;
 *                lock.front following its source, asking for a master code;
 *   then two bytes of a later version's section, which version 1 ignores.
 */
#pragma once

#include <stdint.h>

static const uint8_t HC_HOME_FIXTURE_V1[] = {
    0x01, 0x01, 0x02,
    0x03, 0x01, 18, 'K', 'i', 't', 'c', 'h', 'e', 'n', ' ', 'a', 'n', 'd', ' ', 'd', 'i', 'n', 'i',
    'n', 'g', 7, 'K', 'i', 't', 'c', 'h', 'e', 'n',
    0x07, 0x00, 6, 'G', 'a', 'r', 'a', 'g', 'e', 0,
    0x02,
    0x03, 0x07, 12, 'c', 'l', 'i', 'm', 'a', 't', 'e', '.', 'h', 'a', 'l', 'l',
    0x02, 0x00, 10, 'l', 'o', 'c', 'k', '.', 'f', 'r', 'o', 'n', 't',
    0xEE, 0x01,
};
/* Without the trailing section: what version 1 writes back. */
#define HC_HOME_FIXTURE_V1_LEN 71u
