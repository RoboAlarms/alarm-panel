/*
 * zsrc_ha.h - Home Assistant entities as zones (docs/features/22 ZSRC-005/-008, docs/features/23 M13).
 *
 * The paired integration decides what the panel may see (HAI-009) and sends it as a catalog;
 * the panel's zone editor searches that catalog, and bound entities are watched: Home
 * Assistant pushes each state change, this source maps it onto the zone. An entity that goes
 * unavailable - or the whole link dropping - runs the zone's grace time into CHECK, like any
 * supervised sensor.
 *
 * Portable C11 like the other sources: ha_link feeds it on the panel, the tests and the PC
 * preview feed it directly. Calls come with the platform's lock held (zone_service).
 */
#ifndef ZSRC_HA_H
#define ZSRC_HA_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZSRC_HA_WATCH_MAX   48 /* entities bound to zones (main, tamper or battery roles) */
#define ZSRC_HA_CATALOG_MAX 64 /* catalog entries kept for the picker (newest pages win) */
#define ZSRC_HA_GRACE_S     120 /* unavailable this long -> CHECK (a Home Assistant restart) */

/* One entity Home Assistant shares (a catalog row). */
typedef struct {
    char id[ZSRC_REF_LEN]; /* entity_id */
    char name[48];
    char area[48];
    char state[24];        /* live words for the picker, "" = unknown */
    uint8_t device_class;  /* alarm_device_class_t */
} zsrc_ha_entity_t;

typedef struct {
    char ref[ZSRC_REF_LEN];
    bool used;
    bool known;    /* a state has arrived since the watch began */
    bool avail;
    char state[24];
} zsrc_ha_watch_t;

typedef struct {
    zsrc_sink_t sink;
    bool link_up; /* the paired Home Assistant is connected */
    zsrc_ha_watch_t watch[ZSRC_HA_WATCH_MAX];
    zsrc_ha_entity_t catalog[ZSRC_HA_CATALOG_MAX];
    size_t n_catalog;
    /* The set of watched entities changed: ha_link sends the new watch list. Optional. */
    void (*watch_changed)(void *ctx);
    void *watch_ctx;
} zsrc_ha_t;

void zsrc_ha_init(zsrc_ha_t *h, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_ha_ops(void); /* register with the zsrc_ha_t as ctx */

void zsrc_ha_set_watch_changed(zsrc_ha_t *h, void (*fn)(void *ctx), void *ctx);

/* ---- fed by ha_link (or a test) ------------------------------------------------------------ */

/* The paired connection came up or went down (ZONE-008: zones run their grace into CHECK). */
void zsrc_ha_link(zsrc_ha_t *h, bool up, uint64_t now);

/* A new catalog begins (page 0): the old entries are dropped. */
void zsrc_ha_catalog_begin(zsrc_ha_t *h);
/* One catalog entity (device_class already mapped with zsrc_ha_device_class). */
void zsrc_ha_catalog_add(zsrc_ha_t *h, const zsrc_ha_entity_t *e);

/* A state push. available=false: unknown or unavailable over there. */
void zsrc_ha_state(zsrc_ha_t *h, const char *entity_id, const char *state, bool available,
                   uint64_t now);

/* The entity_ids ha_link should ask Home Assistant to watch. Returns how many. */
size_t zsrc_ha_watch_list(const zsrc_ha_t *h, const char *out[], size_t max);

/* Home Assistant's device-class words to ours ("door", "motion", "carbon_monoxide"...). */
alarm_device_class_t zsrc_ha_device_class(const char *ha_class);

/* The address of an entity. */
zsrc_address_t zsrc_ha_address(const char *entity_id);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_HA_H */
