/*
 * hub_internal.h - shared by the hub and its blob codec.
 */
#ifndef ZONE_HUB_INTERNAL_H
#define ZONE_HUB_INTERNAL_H

#include "zone_hub/zone_hub.h"

/* Kept blob items (h->keep): records of zone u16 | role u8 | item (field, group, len, value).
 * Items of an input this firmware knows carry its zone and role; everything else (items of
 * unknown groups, and whole entries with an unknown role or a zone beyond this build) carries
 * its own selector and is written back after the known inputs, in the order it was read. */
#define ZH_KEEP_HDR  3u
#define ZH_ITEM_HDR  3u
#define ZH_GRP_INPUT 0x01u

bool zh_keep_add(zone_hub_t *h, uint16_t zone, uint8_t role, const uint8_t *item, size_t len);
/* role ZH_ANY_ROLE drops every record of the zone. */
#define ZH_ANY_ROLE 0xFFFFu
void zh_keep_drop(zone_hub_t *h, uint16_t zone, unsigned role);

int zh_find_role(const zone_hub_t *h, uint16_t zone, uint8_t role);

#endif /* ZONE_HUB_INTERNAL_H */
