#pragma once
/*
 * board_internal.h - what the E32R40T's own files share (not part of the board contract).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

/* The panel and its settings, adjustable from the console while bringing a board up
 * (board_console.c): what the panel shows depends on how it was glued and wired. */
typedef struct {
    bool invert;   /* colour inversion (IPS panels need it, TN panels don't) */
    bool bgr;      /* the panel's colour order is blue-green-red */
    bool swap_xy;  /* landscape on a portrait panel */
    bool mirror_x;
    bool mirror_y;
} board_lcd_settings_t;

esp_lcd_panel_handle_t board_lcd_panel(void);
esp_lcd_panel_io_handle_t board_lcd_io(void);
lv_display_t *board_lcd_display(void);
const board_lcd_settings_t *board_lcd_settings(void);
esp_err_t board_lcd_apply(const board_lcd_settings_t *s); /* caller holds the UI lock */

/* Touch: raw readings for the console, and the calibration run (board_touch.c). */
typedef struct {
    bool ok;
    uint16_t x, y;      /* raw medians */
    uint16_t pressure;
} board_touch_raw_t;

void board_touch_last_raw(board_touch_raw_t *out);
esp_err_t board_touch_calibrate(void);   /* draws the targets; the result is stored */

/* Keeps the backlight lit (at least 70 %) while the calibration's targets show, whether or not
 * a UI has lit it; off again, the UI's own level applies (board.c). */
void board_backlight_hold_on(bool on);
bool board_touch_calibrated(void);       /* a stored calibration is in use */
