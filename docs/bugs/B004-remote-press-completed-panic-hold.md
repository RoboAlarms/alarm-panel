# B004 · A remote press cut off mid-hold finished the panic hold by itself

**Status (2026-09-22): committed and pushed as `8367725`; fixed in code, built for every profile, not yet flashed.** Found by
code review of the remote-touch release path; never observed.

## Symptom

A browser or workstation viewer presses and holds Fire, Medical or Police on the Emergency
screen and then drops (the page closes, the network breaks, the 2 s input lease runs out, or
a local finger takes over). Two seconds after the press began the panel sets the panic alarm
off although nobody is holding the button any more. The same gap applied to every
hold-to-confirm row in Settings (factory reset and the like).

## Root cause

`dev_adapter.c` `release()` lets go of a remote pointer with `lv_indev_reset()`. In LVGL 9.5
that sends `LV_EVENT_INDEV_RESET` to the pressed object, not `LV_EVENT_RELEASED` or
`LV_EVENT_PRESS_LOST`. `on_panic` (`ui_app.c`) and `on_hold` (`ui_forms.c`) only cleared their
"holding" state on those two, so `panic_tick()` / `ui_forms_tick()` saw a hold that had
lasted 2 s and fired.

## Fix

Both handlers treat `LV_EVENT_INDEV_RESET` as the press being lost (`on_hold` is now also
registered for it). Fixed alongside, same area:

- The GT911 touch controller failing to answer while a finger was down left that finger
  "down" for good (`board_touch.c`): three unanswered polls (about 50 ms) now count as a lift,
  so a wedged I2C bus cannot complete a hold either.
- A remote user's half-typed code stayed in the Home code pad for up to 90 s after their
  session ended, for the next person at the panel to complete with the tick (`ui_app.c`
  `ui_dev_control_end()` now forgets the typing).
- The installer menu, built unlocked, stayed usable after installer mode ended elsewhere
  (timeout, another keypad): it is rebuilt locked at the transition and the Web server /
  Developer toggles and "Run the setup again" re-check the mode (`ui_settings.c`,
  `ui_installer.c`).
- The dev service's HTTPS worker stack is 16 KiB like the LVGL task's, since injected taps
  run the UI's click handlers on it.

## Verify on hardware

With a browser viewing the panel (Panel Manager keypad), press and hold Police remotely and
close the browser tab within a second: no alarm, no `panic` event in `events`. Repeat with a
local finger touching the screen during the remote hold.
