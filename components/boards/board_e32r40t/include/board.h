/*
 * board.h - LCDWiki 4.0" ESP32-32E display (E32R40T) board support.
 *
 * Implements the board contract (components/boards/board_api/include/board_api.h): the
 * ST7796S display under LVGL (board_display.c), the XPT2046 touch with its three-target
 * calibration (board_touch.c), the backlight; audio and the battery are still to come
 * (docs/features/19 Phase 2). The wiring is in board_pins.h.
 */
#pragma once

#include "board_api.h"
#include "board_pins.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The backlight level last asked for (0-100). */
uint8_t board_backlight_requested(void);

/* The board's console commands: `lcd` (panel settings, live) and `touch` (calibration). */
esp_err_t board_e32r40t_console(void);

#ifdef __cplusplus
}
#endif
