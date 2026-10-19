/*
 * lv_conf.h - LVGL 9.5 configuration of the PC UI preview. Matches the panel's settings where
 * they change the picture (colour depth, observer, object names, theme); everything else uses
 * LVGL's defaults.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16 /* RGB565, like the CrowPanel's DPI frame buffer */

/* A board whose manifest gives the UI an LVGL budget (CONFIG_ALARM_UI_LVGL_BUDGET_KB, read by
 * cmake/alarm_board.cmake) gets LVGL's own allocator here, with room enough for the whole walk,
 * so the walk can measure the most it ever needed and report it against the budget. 0 = the C
 * library, as on the 7" panel (PSRAM). */
#if defined(ALARM_LV_BUDGET_KB) && ALARM_LV_BUDGET_KB > 0
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_MEM_SIZE           (4096U * 1024U)
#else
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#endif
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS             LV_OS_NONE

#define LV_DEF_REFR_PERIOD 16
/* The board's DPI (its manifest's CONFIG_LV_DPI_DEF): LVGL sizes some widget defaults by it. */
#if defined(ALARM_LV_DPI) && ALARM_LV_DPI > 0
#define LV_DPI_DEF         ALARM_LV_DPI
#else
#define LV_DPI_DEF         160
#endif

#define LV_USE_OBSERVER     1
#define LV_USE_OBJ_NAME     1
#define LV_USE_THEME_SIMPLE 1
#define LV_USE_THEME_DEFAULT 0 /* the UI sets the simple theme itself, as on the panel */
/* The screenshots' PNG encoder is compiled separately on the C library's heap (shot_png.c):
 * LVGL's copy would spend the board's LVGL heap. */
#define LV_USE_LODEPNG      0
/* The widgets, layouts, draw formats and libraries a board's manifest states come in as
 * definitions (ALARM_BOARD_LV_DEFINITIONS, cmake/alarm_board.cmake); these are the defaults
 * for a manifest that doesn't. */
#ifndef LV_USE_TJPGD
#define LV_USE_TJPGD        1 /* the screen saver's picture (ui_saver.c) */
#endif
#ifndef LV_USE_QRCODE
#define LV_USE_QRCODE       1 /* subscribing a phone to an ntfy topic (ui_notify.c) */
#endif
#ifndef LV_USE_FONT_COMPRESSED
#define LV_USE_FONT_COMPRESSED 1 /* the icon fonts (tools/lvpro_gen.py) */
#endif
/* A language pack's fonts are loaded from memory (lv_binfont_create_from_buffer, ui_app.c,
 * I18N-049): LVGL's memory file system, on in the P4's manifest too (CONFIG_LV_USE_FS_MEMFS). */
#ifndef LV_USE_FS_MEMFS
#define LV_USE_FS_MEMFS 1
#endif
#ifndef LV_FS_MEMFS_LETTER
#define LV_FS_MEMFS_LETTER 'M'
#endif
#ifndef LV_USE_FONT_PLACEHOLDER
#define LV_USE_FONT_PLACEHOLDER 1 /* a character with no glyph draws as a box (I18N-011) */
#endif

#define LV_USE_LOG      1
#define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF   1

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         1
#define LV_USE_ASSERT_MEM_INTEGRITY 0
/* Off, as on the panel: with it, every call on an object first walks every object of every
 * screen to prove it alive, and the walk took 30 min instead of 30 s for the same pictures.
 * NULL objects still end the run; set it to 1 to hunt the use of a deleted object. */
#define LV_USE_ASSERT_OBJ           0
/* A failed assertion ends the run (LVGL's default loops for ever, which leaves a CI job to time
 * out). The message is already printed. */
#include <stdlib.h>
#define LV_ASSERT_HANDLER abort();

/* The panel's fallback font (sdkconfig.defaults): the UI names every font it draws. */
#define LV_FONT_UNSCII_8       1
#define LV_FONT_DEFAULT        &lv_font_unscii_8

#endif /* LV_CONF_H */
