/*
 * board_internal.h - shared between the files of crowpanel_p4_family (not a public API).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

/* The flicker guard holds the backlight off (true) or gives it back (false). */
void board_backlight_hold_off(bool off);

/* Brightness the UI asked for (the guard doesn't change it). */
uint8_t board_backlight_requested(void);

/* From dark, the backlight waits for a picture drawn since it was asked for (docs/bugs/B023):
 * board.c asks board_display.c, whose timer answers with board_backlight_reveal() and the
 * request's number: the answer to an older request, which a newer one replaced, lights nothing. */
uint32_t board_display_reveal_request(void);
void board_backlight_reveal(uint32_t id);

/* Off for good: a restart is under way. */
void board_backlight_shutdown(void);

#if CONFIG_BOARD_DISPLAY_DIAG
/* The display self-check of investigation builds (board_display_diag.c, docs/bugs/B023). */
typedef struct _lv_display_t lv_display_t;
void board_display_diag_start(lv_display_t *disp, void *fb);
void board_display_diag_flush_done(void); /* from the copy-done interrupt */
void board_display_diag_lit(void);        /* the backlight came on */
void board_display_diag_link_step(const char *step); /* link errors so far, put on this step */
#define DISPLAY_DIAG_STEP(step) board_display_diag_link_step(step)
#else
#define DISPLAY_DIAG_STEP(step) ((void)0)
#endif
