/*
 * zw_region.c - what a region change does to the devices (zw_region.h; network.md §12, ZW-045).
 */
#include "zwave_host/zw_region.h"

#include <stddef.h>

bool zw_region_long_range(uint8_t region)
{
    return region == ZW_REGION_US_LR || region == ZW_REGION_US_LR_B || region == ZW_REGION_EU_LR;
}

uint8_t zw_region_family(uint8_t region)
{
    switch (region) {
    case ZW_REGION_US_LR:
    case ZW_REGION_US_LR_B: return ZW_REGION_US;
    case ZW_REGION_EU_LR:
    case ZW_REGION_DEFAULT: return ZW_REGION_EU;
    default: return region;
    }
}

zw_region_change_t zw_region_change(bool from_known, uint8_t from, uint8_t to, uint16_t classic,
                                    uint16_t lr, uint16_t *stop)
{
    zw_region_change_t r = ZW_REGION_SAME_BAND;
    uint16_t n = 0u;
    if (from_known && from == to) {
        r = ZW_REGION_SAME_BAND;
    } else if (!from_known || from == ZW_REGION_UNKNOWN || to == ZW_REGION_UNKNOWN ||
               zw_region_family(from) != zw_region_family(to)) {
        r = ZW_REGION_BAND_CHANGE;
        n = (uint16_t)((unsigned)classic + (unsigned)lr);
    } else if (zw_region_long_range(from) && !zw_region_long_range(to) && lr > 0u) {
        r = ZW_REGION_DROPS_LR;
        n = lr;
    }
    if (stop != NULL) {
        *stop = n;
    }
    return r;
}
