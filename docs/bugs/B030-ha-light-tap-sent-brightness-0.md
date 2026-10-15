# B030: a tap on an off Home Assistant light didn't turn it on

**Status: fixed (0f5223b) and proven on board 3, 2026-10-06 (docs/features/23 validation.md
part 5).** Found by the user acceptance run the same day (part 4).

## Symptom
On Smart Home, a tap on the tile of a Home Assistant light that was off, or the light sheet's
on/off switch, left the light off; the tile still read "Off", with no "Didn't turn on". A tap on a
light that was on turned it off, and the console's `devices act <ref> 1` turned it on.

## Evidence
On the bench (Home Assistant 2026.9.4 in Docker, a recorder of every service call), three tries
on the demo's Bed Light, disarmed and armed, one right after the panel itself had turned it off:
each tap reached Home Assistant as `light.turn_on` with `brightness_pct: 0`, which Home Assistant
answered as a success and left the light off. The console's ON reached it as `light.turn_on`
without a brightness, and the light came on. An independent re-run found the same.

## Cause
The device source's rule (`device_source.h`; `dsrc_ha.c`): with `ACT_ON`, a value of -1 is a
plain "on" and 0 or more is a brightness. The touchscreen sends 0 in three places in
`components/ui/alarm_ui/src/ui_devices.c`:
- the tile's tap, `send(d, tap_action(d), 0)`;
- the sheet's on/off switch, `send(d, ... ? ACT_ON : ACT_OFF, 0)`;
- a tap that retries a failed `ACT_ON` (its value starts at 0).

For a light, `dsrc_ha` then sends `turn_on` with the value; the integration passes it on as
`brightness_pct`, and Home Assistant takes brightness 0 as off. Any Home Assistant light is hit,
dimmable or not: the rule doesn't ask whether the light dims, and a light that can't dim has no
sheet to fall back on. Switches and fans are not: only a light's `turn_on` carries a value.

Older than 2026-10-06's work: the committed code has the same lines. Nothing caught it because the
PC simulator's backend ignores `ACT_ON`'s value (`host/ui_preview/host_panel.c` `dev_apply`),
the console and the automations send -1, and part 2 of the bench run turned lights on from the
console.

## Fix
Every action without a value now sends -1 (`NO_VALUE` in `ui_devices.c`): the three places
above and the cover, lock, valve, scene and fan-Off keys, which sent 0 too (only a light's
`ACT_ON` read it, but a Z-Wave light's `ACT_ON` with 0 would have set level 0). The simulator's
`dev_apply` now treats a light's `ACT_ON` as the Home Assistant source does (0 or more is a
brightness, -1 its last level), and the preview's walk taps an off light's tile and its sheet's
switch and expects it on; each check was seen to fail with the fix taken out.

## Verify
On board 3 with Home Assistant (part 5): a tap on an off light's tile and its sheet's switch each
sent `light.turn_on` without a brightness and the light came on at its last level; a light that
can't dim the same.
