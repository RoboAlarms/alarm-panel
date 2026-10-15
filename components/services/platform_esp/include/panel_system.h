/*
 * panel_system.h - whole-panel operations: version information and the factory reset.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char firmware[32]; /* application version (git describe) */
    char built[32];    /* build date and time */
    char idf[32];      /* ESP-IDF version */
    char chip[48];     /* "ESP32-P4 v1.0 (v1.x image)" */
    uint32_t uptime_s;
    uint32_t heap_internal_kb; /* free internal RAM */
    uint32_t heap_psram_kb;    /* free PSRAM */
} panel_about_t;

void panel_system_about(panel_about_t *out);

/* Factory reset: records the factory_reset event, erases NVS (configuration, users, Wi-Fi,
 * preferences) and, when asked, the event log, then restarts with the factory codes and the
 * setup wizard. Does not return. */
void panel_system_factory_reset(const alarm_source_t *src, uint8_t user, bool erase_log);

#ifdef __cplusplus
}
#endif
