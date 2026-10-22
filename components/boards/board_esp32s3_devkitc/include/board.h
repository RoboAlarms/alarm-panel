/*
 * board.h - ESP32-S3-DevKitC-1 (N16R8) as a headless panel (docs/features/42).
 *
 * Implements the board contract (components/boards/board_api/include/board_api.h) for a board
 * without a screen, touch, speaker or battery: those calls answer ESP_ERR_NOT_SUPPORTED, and the
 * panel runs without a UI (ALARM_FEATURE_UI off). The wiring of every add-on is board_pins.h.
 */
#pragma once

#include "board_api.h"
#include "board_pins.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The BOOT button is held down now (it reads low while pressed). */
bool board_boot_button_down(void);

#ifdef __cplusplus
}
#endif
