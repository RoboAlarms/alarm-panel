/*
 * registry_fixture_v2.h - two device records carrying the field added after format 1 firmware
 * (docs/features/20 devices.md §4, field 21; ZBZ_F_WD_MAX_DURATION, ZB03): a siren's IAS WD
 * MaxDuration as u16 seconds + 1, written after the endpoints, compat still 1.
 *   siren60  id 5, a siren (role 2, setup done 4), MaxDuration read as 60 s: 15 02 3D 00 (61)
 *   siren0   id 6, the same, MaxDuration read as a real 0 s: 15 02 01 00 (1)
 * Written by hand from the field table (ids, lengths, order), not made by the encoder. The v1
 * fixture's records carry no field 21 and still read as "not read". Fictional, locally
 * administered addresses 0x02ABCD00000000xx. Never edit it: a format change adds
 * registry_fixture_v3.h and a migration test against this one.
 */
#ifndef REGISTRY_FIXTURE_V2_H
#define REGISTRY_FIXTURE_V2_H

#include <stdint.h>

static const uint8_t ZBZ_REG_FIXTURE_V2_SIREN60[28] = {
    0x01, /* compat */
    0x01, 0x02, 0x05, 0x00, /* 1 id */
    0x02, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00, 0xCD, 0xAB, 0x02, /* 2 IEEE */
    0x0E, 0x01, 0x02, /* 14 how it joined */
    0x10, 0x01, 0x02, /* 16 role */
    0x11, 0x01, 0x04, /* 17 setup */
    0x15, 0x02, 0x3D, 0x00, /* 21 WD MaxDuration 60 s + 1 */
};
static const uint8_t ZBZ_REG_FIXTURE_V2_SIREN0[28] = {
    0x01, /* compat */
    0x01, 0x02, 0x06, 0x00, /* 1 id */
    0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0xCD, 0xAB, 0x02, /* 2 IEEE */
    0x0E, 0x01, 0x02, /* 14 how it joined */
    0x10, 0x01, 0x02, /* 16 role */
    0x11, 0x01, 0x04, /* 17 setup */
    0x15, 0x02, 0x01, 0x00, /* 21 WD MaxDuration 0 s + 1 */
};

#endif /* REGISTRY_FIXTURE_V2_H */
