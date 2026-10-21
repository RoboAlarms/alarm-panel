/*
 * outputs_service.h - the panel's own sirens and outputs (docs/features/16): the wired outputs on
 * the board's terminals (components/core/sirens out_wired) and the 433 MHz sirens it transmits to
 * (rf_siren, through rf_service). Zigbee and Z-Wave sirens are their radios' services; every kind
 * follows the same policy (sirens/siren.h) and holds the bell trouble as its own owner
 * (alarm_service_bell_trouble, OUT-020).
 *
 * The engine's news comes from the alarm task (panel_app's listener hands on_output and
 * on_snapshot here): it is queued and never blocks. The service's own task (internal stack, high
 * priority) runs the patterns to the millisecond, the supervision inputs, the transmissions, and
 * stores the configuration (NVS "outputs": "wired", "rf").
 *
 * A wired output takes a terminal away from the wired zones (zone_service_reserve_terminal): a
 * terminal a zone uses can't be an output, and an output's terminal is never offered to a zone.
 */
#ifndef OUTPUTS_SERVICE_H
#define OUTPUTS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "alarm/alarm_types.h"
#include "esp_err.h"
#include "sirens/out_wired.h"
#include "sirens/rf_siren.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *terminals; /* the board's terminals an output may use (GPIO numbers) */
    size_t terminal_count;
    const char *where;        /* "J7 header" */
} outputs_service_config_t;

esp_err_t outputs_service_start(const outputs_service_config_t *cfg);

/* From the alarm task (panel_app's listener): never blocks. */
void outputs_service_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);
void outputs_service_engine_snapshot(const alarm_snapshot_t *snap);

/* ---- wired outputs -------------------------------------------------------------------------- */
typedef struct {
    ow_config_t c;   /* function OW_FN_NONE: free */
    bool on;         /* driven on now */
    bool trouble;    /* its supervision input says so (or its pin failed) */
} outputs_wired_info_t;

void outputs_service_wired(outputs_wired_info_t out[OW_CHANNELS]);
/* ESP_ERR_INVALID_ARG (the rules of ow_set, or a pin that is no terminal), ESP_ERR_INVALID_STATE
 * (a zone uses the pin, or another output), ESP_FAIL (the pin can't be set up); stored when OK. */
esp_err_t outputs_service_wired_set(size_t slot, const ow_config_t *c);
esp_err_t outputs_service_wired_clear(size_t slot);
/* ESP_ERR_INVALID_STATE armed or sounding. */
esp_err_t outputs_service_wired_test(size_t slot);
/* The terminals an output may use now (not a zone's, not another output's; `slot`'s own count). */
size_t outputs_service_free_terminals(size_t slot, uint8_t *out, size_t max);

/* ---- 433 MHz sirens ------------------------------------------------------------------------- */
typedef struct {
    rfs_config_t c;        /* used false: free */
    bool sounding;
    uint32_t sent;         /* transmissions so far */
    bool transmitter;      /* something can transmit */
} outputs_rf_info_t;

void outputs_service_rf(outputs_rf_info_t out[RFS_SIRENS]);
bool outputs_service_rf_transmitter(void);
/* A new siren in the first free slot with the defaults (rfs_defaults) and a fresh address;
 * *slot set. ESP_ERR_NO_MEM when all RFS_SIRENS are taken. */
esp_err_t outputs_service_rf_add(size_t *slot);
esp_err_t outputs_service_rf_set(size_t slot, const rfs_config_t *c);
esp_err_t outputs_service_rf_clear(size_t slot);
/* ESP_ERR_INVALID_STATE armed or sounding; ESP_ERR_NOT_FOUND nothing can transmit. */
esp_err_t outputs_service_rf_pair(size_t slot);
esp_err_t outputs_service_rf_test(size_t slot);

/* The `out` console command. */
esp_err_t outputs_service_console_register(void);

#ifdef __cplusplus
}
#endif

#endif /* OUTPUTS_SERVICE_H */
