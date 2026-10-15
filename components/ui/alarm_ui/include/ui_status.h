/*
 * ui_status.h - the status screen: what a panel shows when its full touch UI cannot run.
 *
 * A board without PSRAM whose free RAM is too small for the touch UI (docs/features/19) still draws
 * something useful instead of a dark screen: the alarm state in its colour, what it means, the
 * troubles, the clock, and a ring where the panel feels a finger (a touch accuracy check that
 * needs no code). It operates nothing. Its words follow the Home screen's.
 *
 * Portable: LVGL and alarm_core, the panel through ui_backend_t (snapshot, clock, backlight).
 * Everything here runs on the LVGL thread; call with the LVGL lock held.
 */
#pragma once

#include <stdint.h>

#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *board;  /* shown small at the top; NULL for none */
    const char *note;   /* why this screen and not the full one; NULL for the default */
    uint8_t partition;  /* the partition this keypad shows, 1-based (0 = 1) */
    uint8_t brightness; /* backlight after the first frame, percent */
} ui_status_options_t;

/* Builds the status screen on the default display and loads it. The backend is copied; it
 * needs get_snapshot, and uses snapshot_seq, epoch_ms and backlight when they are set. Returns
 * false when LVGL has no room for it (nothing is left behind). */
bool ui_status_start(const ui_backend_t *backend, const ui_status_options_t *opt);

#ifdef __cplusplus
}
#endif
