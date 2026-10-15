/*
 * wd_fixture_v1.h - a frozen `zbpanel/wd` blob, compat 1 (zbz_wd.h "The store"; ZB-023). Made
 * once by an independent Python encoder (struct.pack '<HQBBBBBH' per entry) from the layout in
 * zbz_wd.h, never by zbw_encode(): never edit it; a later format adds wd_fixture_v2.h beside it.
 *
 *   siren 1: registry id 3, 02AB:CD00:0000:0001 endpoint 43, partition 1, burglary + fire +
 *            strobe, volume high, the default supervision
 *   siren 2: registry id 7, 02AB:CD00:0000:0002 endpoint 1, partitions 1 and 2, fire + squawk,
 *            volume very high, silent at most 120 minutes
 */
#ifndef WD_FIXTURE_V1_H
#define WD_FIXTURE_V1_H

#include <stdint.h>

static const uint8_t WD_FIXTURE_V1[] = {
    0x01, 0x02, 0x11, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xCD, 0xAB,
    0x02, 0x2B, 0x01, 0x07, 0x02, 0x00, 0x00, 0x00, 0x11, 0x07, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0xCD, 0xAB, 0x02, 0x01, 0x03, 0x0A, 0x03, 0x00,
    0x78, 0x00,
};

#endif /* WD_FIXTURE_V1_H */
