/*
 * zw_region.h - what a region change does to the devices (docs/features/36 network.md §12,
 * ZW-045). Regions come in families, as Z-Wave JS groups them (getSupportedRFRegions: a Long Range
 * region includes its classic one): United States with United States Long Range (and 0x0A, its
 * backup channel, which Z-Wave JS's RFRegion enum dropped), Europe with Europe Long Range; every
 * other region is its own family.
 * Within a family the classic band stays, so classic devices keep working; Long Range devices exist
 * only in a Long Range region, so leaving one for its classic region drops them; between families
 * the band changes and every device stops until it is added again (its zones show CHECK).
 * Portable C11, no state.
 */
#ifndef ZW_REGION_H
#define ZW_REGION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Region codes (serial-api.md §6.7) this file names. */
#define ZW_REGION_EU      0x00u
#define ZW_REGION_US      0x01u
#define ZW_REGION_US_LR   0x09u
#define ZW_REGION_US_LR_B 0x0Au /* United States Long Range, the backup channel (deprecated) */
#define ZW_REGION_EU_LR   0x0Bu
#define ZW_REGION_UNKNOWN 0xFEu
#define ZW_REGION_DEFAULT 0xFFu /* the module's default: Europe */

/* The first Long Range node ID: classic IDs are 1..232, Long Range ones 256..4000. */
#define ZW_REGION_LR_NODE_MIN 256u

typedef enum {
    ZW_REGION_SAME_BAND = 0, /* every device keeps working (the radio restarts for a few seconds) */
    ZW_REGION_DROPS_LR,      /* the Long Range devices stop: leaving a Long Range region for its
                              * classic one */
    ZW_REGION_BAND_CHANGE,   /* another band: every device stops until it is added again */
} zw_region_change_t;

/* true for a region with Long Range (09, 0A, 0B). */
bool zw_region_long_range(uint8_t region);

/* The family a region belongs to: its classic region (US for 09 and 0A, Europe for 0B and FF),
 * itself for every other one. */
uint8_t zw_region_family(uint8_t region);

/* What changing from `from` (`from_known` false: the module never said, so nothing is assumed to
 * keep working) to `to` does to `classic` classic and `lr` Long Range devices; `*stop` (may be
 * NULL) gets how many stop working. A Long Range region left for its classic one with no Long
 * Range devices is SAME_BAND. BAND_CHANGE with no devices stops none. `from` == `to`: SAME_BAND. */
zw_region_change_t zw_region_change(bool from_known, uint8_t from, uint8_t to, uint16_t classic,
                                    uint16_t lr, uint16_t *stop);

#ifdef __cplusplus
}
#endif

#endif /* ZW_REGION_H */
