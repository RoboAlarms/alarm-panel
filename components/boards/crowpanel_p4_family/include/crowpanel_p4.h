/*
 * crowpanel_p4.h - what every CrowPanel Advanced ESP32-P4 board shares (the family library).
 *
 * Implements the board contract (components/boards/board_api/include/board_api.h: power,
 * backlight, display, touch, audio, battery) for the boards built on Elecrow's ESP32-P4 main
 * board, and adds the family's extras below. Each board of the family (board_crowpanel_p4, the 7";
 * board_crowpanel_10, the 10.1") adds its own description - board_info() and board_name() -
 * and its board.h, which includes this file. Bring-up order: see board_api.h and
 * docs/features/06-CrowPanel-P4-Platform.
 *
 * Display safety on these boards: every frame the DSI DMA is restarted from an interrupt; a
 * frame that comes late means the picture may have dropped out. The flicker guard turns the
 * backlight off at the first late frame and back on only after
 * CONFIG_BOARD_FLICKER_GUARD_HOLD_S seconds without one (docs/features/06 P4-012); its counters are
 * the late/guard fields of board_display_stats_t.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_api.h"
#include "crowpanel_p4_pins.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared I2C0 bus (touch + the I2C connector); created by board_i2c_ext() or board_touch_init,
 * whichever comes first. NULL before that. */
i2c_master_bus_handle_t board_i2c_bus(void);

#if CONFIG_BOARD_FRAME_CAPTURE
/* Caller holds the UI lock. Copies the composed hardware framebuffer, whole or at half
 * resolution (count = BOARD_LCD_H_RES * BOARD_LCD_V_RES, or a quarter of it). */
bool board_dev_frame(uint16_t *out, size_t count);
#endif

#if CONFIG_BOARD_DISPLAY_DIAG
/* What the display self-check has seen since start (docs/bugs/B023), on stdout. */
void board_display_diag_print(void);
/* The backlight off for ms, then back to its level, the screen redrawn first or not: tries the
 * wait for a fresh picture before it comes on (the log says when and why it did). */
void board_display_diag_dark(uint32_t ms, bool redraw);
#endif

#ifdef __cplusplus
}
#endif
