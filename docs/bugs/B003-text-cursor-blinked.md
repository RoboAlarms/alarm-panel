# B003 · The text-entry cursor blinked (display safety)

**Status: fixed (`8367725`), asserted by the UI preview, and seen steady on a bench panel
2026-10-05** (see "On the panel"). Found by code review of the LVGL theme, not by a report.

## Symptom

Every text sheet on the panel (Wi-Fi password, names, MQTT and ntfy settings, the setup
wizard's text fields) showed a cursor switching on and off twice a second for as long as the
sheet was open: a 1 Hz square-wave flash on a wall panel anyone may look at, and a
plain breach of UI-020 (nothing on the screen may blink).

## Root cause

`alarm_ui_init` uses LVGL's simple theme. That theme adds a cursor style with
`anim_duration = 500` on the selector `LV_PART_CURSOR | LV_STATE_FOCUSED`
(`lv_theme_simple.c`). `ui_forms.c` adds `LV_STATE_FOCUSED` to the text area when a sheet
opens so the cursor shows, and `lv_textarea` then reads `anim_duration` for the cursor part:
500 ms means "blink". The sheet's own cursor style in `text_sheet.xml` has no state in its
selector, and in LVGL a style whose state matches more specifically wins regardless of
whether it came from the theme or the object, so it could not override the theme's value.

## Fix

`ui_forms_steady_cursor()` (`ui_forms.c`, called for both text areas at init) sets
`anim_duration = 0` on `LV_PART_CURSOR | LV_STATE_FOCUSED` and on `LV_PART_CURSOR`. With 0
`lv_textarea` deletes the blink animation and shows the cursor steadily. The UI preview now
asserts, with a text sheet open, that the cursor's animation duration is 0
(`host/ui_preview/main.c` `sheet_text()`), so a theme change cannot bring it back unnoticed.

Fixed in the same review, same rule (UI-020): the home screen's colour ("hero") followed a
flapping sensor at up to 20 changes a second (`ui_app.c` `hero_tone()`: at most one change
every 500 ms unless the new state is an alarm or an entry delay), and the backlight could be
left on while the flicker guard believed it was holding it off (`board.c`: the UI's level and
the guard's hold now apply under one mutex).

## Verify on hardware

Open Settings > Wi-Fi > a network name (a text sheet): the cursor stays steady. Optionally,
with the sheet open, `display watch 10` shows no redraw storm (a blinking cursor redraws twice
a second; a steady one does not).

## On the panel

On a bench panel (P4 v1.3, a dev image of 2d4cde9), 2026-10-05, through the remote viewer:
Settings > Wi-Fi and network > a nearby network opened its password sheet with the cursor
showing; six frames taken a quarter of a second apart and more were identical, so the cursor
stayed on (a 500 ms blink changes at least one of them).
