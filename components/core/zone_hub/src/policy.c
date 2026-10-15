/*
 * policy.c - addresses, mapping defaults and trust rules shared by the hub, the sources and
 * the zone editor (docs/features/22 ZSRC-006, ZSRC-008).
 */
#include <string.h>

#include "zone_hub/zone_hub.h"
#include "zone_hub/zone_source.h"

bool zsrc_address_equal(const zsrc_address_t *a, const zsrc_address_t *b)
{
    return a->kind == b->kind && a->input == b->input && a->id == b->id &&
           strncmp(a->ref, b->ref, sizeof(a->ref)) == 0;
}

static bool role_used(const zone_hub_binding_t *b, size_t r)
{
    return b != NULL && b->in[ZSRC_ROLE_MAIN].addr.kind != ZSRC_NONE &&
           b->in[r].addr.kind != ZSRC_NONE;
}

bool zone_hub_same_sensors(const zone_hub_binding_t *a, const zone_hub_binding_t *b)
{
    for (size_t r = 0; r < (size_t)ZSRC_ROLE_COUNT; r++) {
        const bool ua = role_used(a, r);
        if (ua != role_used(b, r) || (ua && !zsrc_address_equal(&a->in[r].addr, &b->in[r].addr))) {
            return false;
        }
    }
    return true;
}

zsrc_address_t zsrc_address(uint8_t kind, uint8_t input, uint64_t id)
{
    zsrc_address_t a;
    memset(&a, 0, sizeof(a));
    a.kind = kind;
    a.input = input;
    a.id = id;
    return a;
}

void zsrc_mapping_defaults(zsrc_mapping_t *m, alarm_device_class_t cls)
{
    memset(m, 0, sizeof(*m));
    if (cls == ALARM_DEVCLASS_TEMPERATURE) {
        m->mode = ZSRC_MAP_BELOW; /* freeze warning */
        m->threshold_x100 = 500;
        m->hysteresis_x100 = 100;
    }
}

alarm_zone_type_t zsrc_suggest_zone_type(alarm_device_class_t cls)
{
    switch (cls) {
    case ALARM_DEVCLASS_MOTION: return ALARM_ZT_INTERIOR_FOLLOWER;
    case ALARM_DEVCLASS_SMOKE:
    case ALARM_DEVCLASS_HEAT: return ALARM_ZT_FIRE;
    case ALARM_DEVCLASS_CO: return ALARM_ZT_CO;
    case ALARM_DEVCLASS_GAS:
    case ALARM_DEVCLASS_PANIC_BUTTON: return ALARM_ZT_24H_AUDIBLE;
    case ALARM_DEVCLASS_WATER:
    case ALARM_DEVCLASS_TEMPERATURE: return ALARM_ZT_24H_AUX;
    case ALARM_DEVCLASS_KEYFOB: return ALARM_ZT_ARM_AWAY;
    case ALARM_DEVCLASS_KEYSWITCH: return ALARM_ZT_KEYSWITCH;
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
    case ALARM_DEVCLASS_GARAGE_DOOR:
    case ALARM_DEVCLASS_GLASS_BREAK:
    case ALARM_DEVCLASS_VIBRATION:
    case ALARM_DEVCLASS_GENERIC:
    default: return ALARM_ZT_PERIMETER;
    }
}

bool zsrc_zone_type_life_safety(alarm_zone_type_t type)
{
    switch (type) {
    case ALARM_ZT_FIRE:
    case ALARM_ZT_FIRE_VERIFIED:
    case ALARM_ZT_CO:
    case ALARM_ZT_24H_SILENT:
    case ALARM_ZT_24H_AUDIBLE: return true;
    default: return false;
    }
}

bool zsrc_trusted_for(uint32_t caps, alarm_zone_type_t type)
{
    return !zsrc_zone_type_life_safety(type) || (caps & ZSRC_CAP_LIFE_SAFETY) != 0u;
}
