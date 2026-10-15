/*
 * zsrc_sim.h - the simulator source (docs/features/22, ZONE-006): no sensor at all; the service
 * console ("zone 3 open") and the PC preview say what the "sensor" does. By convention zone N
 * is bound to simulator input N. Portable C11.
 */
#ifndef ZSRC_SIM_H
#define ZSRC_SIM_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    zsrc_sink_t sink;
} zsrc_sim_t;

void zsrc_sim_init(zsrc_sim_t *s, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_sim_ops(void); /* register with the zsrc_sim_t as ctx */

/* Simulator input n (1..ALARM_MAX_ZONES). */
zsrc_address_t zsrc_sim_address(uint16_t input);

/* The "sensor" on simulator input n does ev (any alarm_zone_event_t, UNREACHABLE too). */
void zsrc_sim_event(zsrc_sim_t *s, uint16_t input, alarm_zone_event_t ev, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_SIM_H */
