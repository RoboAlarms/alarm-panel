# B022 · Pieces of the factory demo flash behind our screens after an install (display safety)

**Status (2026-10-04): fixed in code (both draw buffers cleared at every start), built for v1
dev without warnings; not yet tried on a panel. The same build still showed pieces of our own
earlier screens behind the current one: [B023](B023-old-screens-behind-new-ones.md). B023's
self-check then found the frame buffer holding only what LVGL drew. What is left of this one is
most likely on the glass: the demo's last picture sat on the panel, undriven, while the Flasher
held the P4 in download mode. B023's fix starts the panel in its datasheet's order, but it can't
shorten that hold.**
Seen on a new panel on its first start after the Flasher installed RoboAlarms over Elecrow's
factory demo firmware. It stopped on its own after a few minutes of use (the Panel Manager's
browser setup among them), with no power cut in between.

## Symptom

Our screens came up, but parts of the factory demo's screen showed behind them, flashing very
quickly. That breaks the display-safety rule (docs/features/07 UI-020, 06 P4-012): nothing on
the screen may flash. The flicker guard can't catch it, because it watches frame timing and these
frames arrive on time with the wrong pixels in them.

The demo's picture isn't coming from flash. A panel without RoboAlarms can only take an Install
(an Update needs our firmware already there), and an Install erases the whole chip before
writing (`tools/flasher/roboflash/jobs.py` `install()`).

## Not yet known

- Which screens and areas it showed on, and whether it flashed only when something redrew (the
  clock, a touch) or also when nothing changed.
- The board's silicon revision, the image installed (version, profile) and the Flasher's version.
- Whether the bench panel did the same on its first start after its own Install over the factory
  firmware (not recorded).

## Likely cause (not proven)

PSRAM keeps its contents through a reset; only a power cut clears it. After writing, the Flasher
restarts the panel with its reset line while it stays powered, so our firmware starts with the
demo's pictures still in PSRAM. ESP-IDF's boot test (`CONFIG_SPIRAM_MEMTEST`) writes only one word
in every 32 bytes (`esp_psram.c` `s_test_psram()`), so seven eighths of an old picture survives
it.

- The frame buffer the panel scans out isn't the cause: ESP-IDF allocates it zeroed and writes
  it back from the cache (`esp_lcd_panel_dpi.c`, `heap_caps_calloc`), and the backlight starts at
  0 (`board.c` `board_backlight_init()`).
- The two LVGL draw buffers are. `board_display.c` asks esp_lvgl_port for two whole-screen
  buffers in PSRAM (`buff_spiram`, `double_buffer`, partial mode), and esp_lvgl_port allocates
  them with `heap_caps_malloc`, uncleared. Each is 1024 x 600 x 2 bytes, the size of a demo
  frame buffer, so one can land on a whole demo picture.
- LVGL 9.5 clears a draw buffer before rendering only when the display's colour format has
  alpha (`lv_refr.c` `refr_configured_layer()`), and ours is RGB565. So every pixel of a
  refreshed area has to be painted by an opaque object. A pixel that nothing covers keeps the
  buffer's old bytes, which then go to the screen. The bottom layer is drawn only when no object
  of the screen covers the whole area, and it is transparent unless it is styled.
- The two buffers are used in turn, so an uncovered pixel shows one buffer's leftover on one
  refresh and the other buffer's on the next. That gives a picture flickering behind ours each
  time the area redraws.
- Why nobody has seen it before: after a restart from our own firmware, the leftovers are our own
  screens, which look almost the same as what is drawn over them. After a power-on they are
  whatever the RAM holds when it wakes up. A foreign picture only shows on a first start after
  another firmware. If this is right, the uncovered areas exist on every panel, and every panel
  installed with the Flasher over the factory firmware shows this on its first start.
- It also explains why the picture went away on its own: each refresh writes our pixels over
  more of the buffers' bytes. Once the leftovers are our own pixels, the gaps no longer show
  anything foreign, but they are still there.

Also to rule out: any other PSRAM buffer the UI shows before filling it (a canvas, a snapshot).

## Fix

`board_display.c` `clear_draw_buffers()` zeroes both LVGL draw buffers (2 x 1.2 MB) at every
start, right after the display is created and before the backlight comes on. esp_lvgl_port
allocates the buffers and LVGL's public API returns only the active one, so the function reaches
both through `lv_display_t` (`lvgl_private.h`).

This doesn't change one thing: a pixel that no object paints still keeps what that buffer last
held, but that is now black or our own pixels, never another firmware's picture. Nobody has seen
it, and the preview walk below would find any such area. The 4" board (`board_e32r40t`, on hold)
doesn't clear its draw buffers either; it was not changed.

## Left

1. Verify on a panel (BL-159): on a bench panel whose factory image was backed up, restore it,
   let the demo run, then install this firmware with no power cut. Its first start must show no
   foreign picture. With the dark build (`-Dark`), screenshots of the frame buffer show the
   result with the backlight off. The self-check build (`CONFIG_BOARD_DISPLAY_DIAG`, B023) tells
   the frame buffer from the glass: a ghost the eye sees while `dispdiag` counts no difference
   is on the glass, and it should fade within minutes.
2. Optional, on the PC: in the UI preview, fill the draw buffers with a marker colour before each
   screen is drawn and fail on any marker pixel in a shot. That finds areas no object paints and
   keeps them from coming back. On board 3 the self-check's marker found none on the screens it
   saw (home, the saver, the code keypads); the preview would cover every screen.
