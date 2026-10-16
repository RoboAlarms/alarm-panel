/*
 * zone_service.h - the zone hub on the panel (docs/features/22): its own task and lock, the binding
 * blob in NVS ("zones"/"bind"), the simulator source (service console) and the wired terminals.
 * Sources report here; zone events reach the alarm task through alarm_service_zone_event().
 * Every function is safe from any task; none blocks for long.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_service.h"
#include "esp_err.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_ha.h"
#include "zone_hub/zsrc_wired.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const zsrc_wired_terminal_t *terminals; /* wired zone terminals, a static table (or NULL) */
    size_t terminal_count;
} zone_service_config_t;

/* Register before alarm_service_start(): tells the hub which zones a configuration change
 * reset, so it repeats what their sensors say. */
const alarm_listener_t *zone_service_listener(void);

/* After alarm_service_start(): loads the bindings (on the first start with zone sources, every
 * configured zone is bound to the simulator, which is how they were driven before), registers
 * the sources and starts the hub's task. */
esp_err_t zone_service_start(const zone_service_config_t *cfg);

/* Completed passes of the hub's task. It turns at least every few seconds whether or not a
 * sensor says anything, so a value that stops moving means zone source processing has stopped -
 * which is what the panel's health check watches (docs/features/13 OTA-005). Safe from any task. */
uint32_t zone_service_passes(void);

/* The health check's verdict on this service: false raises the same "sensors may not be heard"
 * trouble a lost source link raises, so there is one message rather than two (ZSRC-015). */
void zone_service_report_health(bool running);
/* A source lost an event that may have carried an alarm (a full queue: a Zigbee zone status,
 * enroll request or keypad command, docs/features/20 ZB-003, ZB-024). While any source says so,
 * the same "sensors may not be heard" trouble is up; the source says when it is over. */
void zone_service_report_lost(uint8_t kind, bool on);

/* The simulator ("zone 3 open" on the console): false when no zone is bound to that input. */
bool zone_service_sim(uint16_t input, alarm_zone_event_t ev);

/* ---- the Home Assistant source, fed by ha_link (docs/features/23 M13; no-ops without
 * CONFIG_ALARM_ZSRC_HA) ---- */
void zone_service_ha_link_state(bool up); /* the paired connection came up or went down */
void zone_service_ha_catalog_begin(void); /* a page-0 catalog starts fresh */
void zone_service_ha_catalog_add(const zsrc_ha_entity_t *e);
void zone_service_ha_state(const char *entity_id, const char *state, bool available);
/* The entity_ids the panel's zones are bound to, copied out. */
size_t zone_service_ha_watch_list(char out[][ZSRC_REF_LEN], size_t max);
/* Fired under the zone lock when the watched set changes: only set a flag in it. */
void zone_service_ha_set_watch_changed(void (*fn)(void *ctx), void *ctx);

/* ---- the zone editor and the console ---- */
size_t zone_service_sources(zone_hub_source_info_t *out, size_t max);
/* A source another module owns joins the hub: the sensor radio's zigbee_service registers
 * zsrc_zb_ops() this way (docs/features/20). It reports through zone_service_sink(), and every call
 * it makes into its own zsrc_* functions has to sit between lock() and unlock(), because the
 * zone task is ticking the same hub. wake() tells that task the deadlines moved. */
zone_hub_result_t zone_service_register(const zone_source_ops_t *ops, void *ctx);
zsrc_sink_t zone_service_sink(void);
void zone_service_lock(void);
void zone_service_unlock(void);
/* lock() without waiting: false while another task holds it (or before the zone service runs).
 * For a reader that must never wait (the panel link's task, LINK-027); true is followed by
 * zone_service_unlock(). */
bool zone_service_try_lock(void);
void zone_service_wake(void);

/* Every partition is disarmed: nothing that takes sensors off the air may happen otherwise. */
bool zone_service_all_disarmed(void);

/* Bindings change only while every partition is disarmed (ZONE_HUB_ERR_BUSY otherwise); each
 * change is stored at once. */
zone_hub_result_t zone_service_bind(uint16_t zone, const zone_hub_binding_t *b);
zone_hub_result_t zone_service_unbind(uint16_t zone);
bool zone_service_get(uint16_t zone, zone_hub_binding_t *out);
/* D07: true while a changed sensor's binding for this zone is not yet durably stored - its
 * spoken-name approval invalidated, the new binding in flight. Lock-free, any task (the alarm
 * task included): approval and automatic zone speech refuse the zone meanwhile. */
bool zone_service_voice_settling(uint16_t zone);
bool zone_service_info(uint16_t zone, zone_hub_info_t *out);
bool zone_service_learn(uint8_t kind, bool on);
/* Whether something already has this source listening (the zone editor's picker), so the
 * console's "terminals" doesn't switch it off underneath. */
bool zone_service_learning(uint8_t kind);
size_t zone_service_candidates(uint8_t kind, const char *query, uint32_t filter,
                               zsrc_candidate_t *out, size_t max);
const char *zone_service_result_name(zone_hub_result_t r);

#ifdef __cplusplus
}
#endif
