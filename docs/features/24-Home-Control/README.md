# 24 · Devices: lights, locks, climate and more from the panel

**Status:** In progress - the device registry (`components/core/home_control`) and Home Assistant
as its first device source are host-tested and built into the P4 firmware, wired to the Home
Assistant link (console `devices`); the Smart Home screen is built as approved (its tiles, detail
sheets and Arrange; every kind of device tried both ways with Home Assistant on a development
panel, 2026-10-06, docs/features/23 validation.md part 2). A device's values over the link
(brightness, setpoint and modes, position, fan speed) and dragging a tile in Arrange are built
and tried on a panel (2026-10-06, validation.md part 3). What a device can do,
its time and `hvac_action` over the link, and the fixes for the user acceptance run, are built
and tried on a panel (2026-10-06, validation.md part 5; 0f5223b). The approved words
and a moving cover's tap are built and tried on a panel (2026-10-07, validation.md part 6;
bd561d8): every case a screenshot can show passed; whether anything flashes as the words change,
"didn't turn on" and an earlier day's "Offline since" are left for a person at the panel (BL-183).
How the note keeps its outcome beside long room tabs (the open tab shortened, the tabs narrowed)
was fixed after that test: host tests and preview only until a panel runs it. A thermostat at
rest (Idle, Off only in mode Off), an offline device that stays readable and long names that
stay inside their tiles were built on 2026-10-07 (BL-171, BL-181, BL-184): host tests and
preview, not on a panel yet. A link keypad's Smart Home (HCTL-013, BL-216) is built and tried in
the PC simulator and the preview; on the panels only with an empty list, as Home Assistant wasn't
connected (2026-10-16). A change that reads as done at once (HCTL-006, BL-219) is built and tried in the
preview (2026-10-19), not on a panel yet.
Left: changes at once on the panels (BL-219), a link keypad's tiles on the panels (BL-216), a cover's class
and presets (BL-171), the
catalog staged (BL-178), Arrange's sizes (BL-180) - a light's warmth and a thermostat's fan mode and humidity, Settings > Devices, the
unlock and open events, and an unlock with a code typed at the panel (BL-045); the built-in automations are
docs/features/41 (BL-168 to BL-170);
native Zigbee devices (BL-036). · **Milestone:** M13 (through Home Assistant); native
Zigbee devices after M10 · **Depends on:** 03, 07, 20, 22, 23 · **Hardware:** P4 (the CYD has
neither the RAM nor the screen for it)

## Goal
A **Smart Home** screen on the panel for the things around the alarm: lights, switches and plugs,
locks, thermostats and AC, covers and garage doors, fans, water valves, scenes and scripts. The
screen doesn't care where a device lives: first through Home Assistant (docs/features/23), later
also Zigbee devices paired straight to the panel's own network (docs/features/20), so they keep
working when HA is down.

## Reference behaviour
Touchscreen alarm panels control Z-Wave devices (lights, locks, thermostats,
garage doors) from the panel, and can run them from alarm events (for example lock the
doors when armed away). They keep this behind their own radio: only devices paired to the
panel's Z-Wave network show up. We do the same job source-agnostically.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| HCTL-001 | A device endpoint is an interface (`device_source_ops_t`, like `zone_source_ops_t` in docs/features/22) with implementations for Home Assistant (M13) and native Zigbee (after M10); the screen only sees the device model | host tests |
| HCTL-002 | Device model: kind (light, switch, lock, climate, cover, fan, valve, scene, script, sensor readout), name, area, state, attributes (brightness, setpoint, modes, position), capabilities, online | host tests |
| HCTL-003 | Rail item **Smart Home** (a house with Wi-Fi, its name on two lines; third, after Zones) appears only when a device source is compiled in and at least one device is available; tiles grouped by area with a Favorites group first | preview |
| HCTL-004 | Tap toggles (lights, switches, fans); a long press or the tile's arrow opens a detail sheet (brightness slider, thermostat setpoint and mode, cover position, fan speed). Each control shows only where the device can do it, when its source says what it can (HCTL-002): a cover's Stop and position, a fan's speeds, a light's brightness, a thermostat's single setpoint; a fan without speeds is switched on and off as a switch is. A thermostat says what it is doing where its source says (Home Assistant's hvac_action): Heating, Cooling and Preheating as reported, even in mode Off; a reported rest (off or idle) reads Idle while the reported mode is Heat, Cool, Auto or On, and Off only while it is Off (an unknown mode keeps the word reported; BL-171, 2026-10-07). Where the source doesn't say, or says drying, fan or defrosting, its mode (Heat, Cool, Auto, Off; On for dry or fan only) - never guessed from the temperature, the modes it has or a mode asked for and still pending. A tap on a moving cover stops it where it can, or turns it back where its source says it can't (Home Assistant's toggle; reopening asks for the code as opening does). A tap on a failed change of a value (brightness, position, speed, set point, mode) opens the device's sheet. A long name never covers the chip, the state or a control: a tile shows it on one line ending in "..." (two lines on a wide tile with no slider, keys or set point showing; a thermostat whose stepper comes or goes changes with it) and its state on one; nothing scrolls or shrinks, the stored name keeps every byte, and the sheet - a long press opens it on any tile, an offline one or one without an arrow included - shows the whole name and state, wrapped (BL-184) | preview |
| HCTL-005 | Security-relevant actions by a person require a valid user code: unlocking, opening any cover, and opening a water valve. Judged by effect, not the action's name: setting a cover's Position to any nonzero target requires the same code as Open, regardless of the cover's class, reported position or direction of travel. Only a Position target of exactly 0 is treated as fully closed. Lock, Close, Stop and Position 0 do not require a code. The code is checked by the engine like any keypad code (lockout included); an arm-only or guest code may not unlock or open (a duress code: a decision of BL-045). Invalid targets remain subject to action validation; they gain no code exemption. Automation authorization remains governed separately by HCTL-011 and AUT-015 | host tests, preview |
| HCTL-006 | Actions and their results are shown (done, under way, failed, offline). A change a person makes to a light, a switch, a fan, a thermostat or a scene reads as done the moment it is made, as Home Assistant's own screens do: the tile, its sheet and the page's note show what was asked (a slider stays where the finger lifted, a switch flips, the chip stays at full strength) until the device sends a new report, and from then on the report, whatever it says; with no new report, the last report shows again 2 s after the device answered. No "Changing…" or "Turning on…" for these, also when someone else (an automation, the console) started the change. A touch while the last change is still on its way is the newest wish: it shows at once and goes out once the device has answered the last one, never refused as busy. A lock, a garage door, a cover and a valve say they are under way ("Locking…", "Closing…", the chip dimmed) until they report: a security panel never says "Locked" or "Closed" before it is (BL-219, proposal approved 2026-10-19). A device whose source is offline stays readable - its tile and sheet at full opacity, its name and "Offline" with, when it is known, since when, in their usual colours - while its chip dims to 70 % and its controls (sliders, steppers, keys, the sheet's switch) rest at 40 % in their neutral grey, none lit and the switch shown off; a tap on the tile or a control sends nothing and shows no pressed colour (the tile, which keeps its long press, still gives its key click), its arrow is hidden, a long press still opens its sheet to read, Close works, and an open sheet gets its controls back in one step when the device answers again (BL-181). A failed action's words stay until the next action, or until the device's source reports something newer than what it showed when the action failed - another state word, level, setpoint or a scene's run time - as Home Assistant's own cards show the state apart from a failed call (BL-175). A time of an earlier day shows its date, with the year for an earlier year ("Ran on Sep 18", "Offline since Sep 18, 2025"), in the panel's time zone; none while the clock isn't set or the time is more than a minute ahead of it. The page's note says what didn't happen ("Patio Lights didn't turn on"), on one line with at most two facts, chosen by priority and then by the page's order (BL-182): jammed devices first, then unlocked locks and open entrance covers, then offline devices, then failed actions, each in the page's order, stopping at two; how many lights are on only where a slot is left; nothing cycles, nothing is ranked by time, and the note stays still while what it reads doesn't change (a failure it has no room for stays on its own tile). A long name shortens with "…" first, then a second fact that doesn't fit is dropped whole (nothing says it was), then the name goes down to "…"; what still doesn't fit (the tabs of a house with many rooms at their widest, a long translation) takes its room from the area tabs, which scroll with the open tab in view - where the other tabs alone can't give it (the open room's name is long), the open tab's name shortens with "…" too, never narrower than Favorites, and the tabs never pass the layout's widest - so the outcome words are never cut and the tiles don't move | preview, host tests |
| HCTL-007 | Unlocks and openings from the panel are logged (who, what, when) as events; ordinary light and switch changes are not | host tests |
| HCTL-008 | What appears: HA entities the integration's options allow for control (docs/features/23), picked on the panel from a search; the master or installer can rename and hide them (Settings > Devices); the order, each tile's size and the favourites are set on the screen itself (Arrange, no code: it changes how the screen looks, as brightness does; a tile moves when you hold it and drag it onto another tile's place, or with the arrows, the way that needs no dragging (WCAG 2.5.7); BL-045, approved 2026-10-01; the drag added 2026-10-06) | preview |
| HCTL-009 | Built-in automations (later): a short list of alarm-centric rules made on the panel. Triggers: an arming state (armed away / stay / night, disarmed), an alarm or fire, a zone opening or closing, a time of day. Actions: a device action or scene (turn on the porch light, lock the front door), or arming the panel. They run on the panel, so they work without Home Assistant. Built as docs/features/41 (AUT-001 to AUT-014) | host tests, preview |
| HCTL-010 | Compiled only with `CONFIG_ALARM_HOME_CONTROL` (needs `SPIRAM`); without it the rail and Settings entries don't exist | build profiles |
| HCTL-011 | An automation may do what the system master or installer who saved it could do at the panel: turn things on or off, lock and unlock, open and close, arm and disarm (docs/features/41 AUT-002, AUT-011; ADR-050). It asks no code when it runs (HCTL-005 is for a person at the screen): it acts as that person, re-checked each time. The less secure actions - disarm, unlock, open, stop - run only while the installer allows them (AUT-015) and, after a door, window, motion or alarm trigger, only once confirmed at save (AUT-016); a script never | host tests |
| HCTL-012 | A pending action has a bounded lifetime and is tied to the connection generation it was sent on: a reply that never comes times out, and a source that drops and reconnects fails whatever was in flight as an unknown outcome instead of leaving the device busy forever. Nothing here replays a command automatically - a person retries on purpose, which matters most for unlock and open. The screen says an unknown outcome is one: "Couldn't confirm Garage Door opened" on the page's note and "Not confirmed" on the tile and sheet - never "didn't". A change shown as done before its answer (HCTL-006, BL-219) never hides how it ended: a refusal, no answer in time or a dropped connection puts the device's report back with these words, and nothing is sent again by itself | host tests, preview |
| HCTL-013 | A link keypad's Smart Home works as the master's: the same page, words and states from the master's registry (docs/features/11 LINK-044..049), actions judged by the master as `{KEYPAD, n}` with the keypad's home partition's codes (LINK-048; the owner's decision 1, decided 2026-10-16); Arrange is the keypad's own (HCTL-008; decision 2, the same). Designed 2026-10-16, screens approved (proposal P1); built (BL-216, 787c2da) and tried in the PC simulator (`link_sim`, `link_smoke`) and the preview (`bl-216-kp-devices`, `-kp-device-unlock`, `-kp-devices-lost`, `-kp-devices-unconfirmed`); on the two development panels (2026-10-16) the keypad and the master shared Smart Home with an empty list (Home Assistant not connected), so no tile was tried on a panel yet | link tests, preview, HIL |

## Design

### Device interface (same pattern as zone sources)
```c
/* components/core/home_control/include/home_control/device_source.h (portable) */
typedef enum { DEV_NONE = 0, DEV_LIGHT = 1, DEV_SWITCH, DEV_LOCK, DEV_CLIMATE, DEV_COVER,
               DEV_FAN, DEV_VALVE, DEV_SCENE, DEV_SCRIPT, DEV_KIND_COUNT } dev_kind_t;
typedef enum { ACT_NONE = 0, ACT_ON = 1, ACT_OFF, ACT_TOGGLE, ACT_LOCK, ACT_UNLOCK, ACT_OPEN,
               ACT_CLOSE, ACT_STOP, ACT_SET_LEVEL, ACT_SET_TEMP, ACT_SET_MODE, ACT_RUN,
               ACT_COUNT } dev_action_t;

typedef struct device_source_ops {
    uint8_t kind;                                       /* DSRC_HA, DSRC_ZIGBEE */
    const char *name;
    bool (*online)(void *ctx);
    size_t (*list)(void *ctx, dev_info_t *out, size_t max);
    bool (*act)(void *ctx, const char *ref, dev_action_t a, int32_t value, const char *mode,
                uint32_t request_id);                   /* the result arrives later */
} device_source_ops_t;
```
Values are stored in events and requests: append, never renumber. A source reports back through
a `dsrc_sink_t` (`changed`, `result`) and never sees the registry or the engine.

`home_control` keeps the device list, favorites (with Settings > Devices, not built yet) and the
security class per device, applies HCTL-005 (`hc_needs_code()` says which actions need a code,
judged by effect - see the domain table below; the registry itself never touches the engine:
the platform's port has the engine check the code before it calls `hc_act()`), and hands results to the UI
through `ui_backend_t` (`devices_list`, `device_act`, `device_result`, planned with the screen).
One source per kind, `HC_MAX_SOURCES` of them, `HC_MAX_DEVICES` devices; a
refresh re-lists every source, and a device that is still there keeps its pending action and its
last failure. The failure lasts until the next action, or until its source reports something
newer than what the device showed when the action failed (`hc_device_t.failed_seen`: its state
word, its level and setpoint when known, a scene's run time; compared only while it is online; a
temperature, what a thermostat is doing and what a device can do move by themselves and don't
count) (BL-175). A state never touches a pending action. If the source's connection has dropped
since it was sent, it fails as an unknown outcome rather than riding along forever (HCTL-012):
the registry's own failures of an action in flight - its timeout, its connection or its row
dropped - leave the outcome unknown (`hc_device_t.unsure`, `hc_outcome_unknown()`), and so does a
source's answer that it sent the action on and couldn't confirm it: Home Assistant's "timeout"
(the integration's service call didn't finish within its 25 s, before the registry's 30 s, so the
device may have acted) and Z-Wave's "timeout" (sent, not confirmed). The source says which
(`dsrc_sink_t.result`'s `unknown`); the registry never reads the word. Only a refusal made before
anything was sent is definite and says it wasn't done: Home Assistant's "not_allowed",
"unavailable" and "failed", Z-Wave's "offline", "busy" and "send_failed".
One action at a time per device: `hc_act` hands out the request id tied to the source's current
connection generation, `hc_sink()->result` closes it normally (HCTL-006), and the platform calls
`hc_tick(hc, now_ms)` periodically so a reply that never arrives fails the same way once its
deadline passes.

What a device can do beyond its kind's actions travels with its values (`dev_values_t.caps`,
`DEV_CAP_*`, `dev_can()`; protocol.md `supports`): a light's brightness, a fan's speed, a
cover's or a valve's stop and position. When the source says, the screen offers only that - a
cover that can't stop has no Stop key, and a tap while it moves turns it back (closing: Open,
with the code opening asks for; opening: Close), as Home Assistant's cover toggle does (BL-179); a fan without speeds is switched on and off on one cell, as a switch
is; a light or a cover without a settable level has no slider - and a thermostat holding a range
(no single setpoint) has no - and + on its tile, as on its sheet. When the source doesn't say (an
older integration), the screen offers everything its kind has, as before. The registry doesn't
refuse an action a device can't do, and automations aren't filtered by it (`hc_auto_offer` stays
per kind and source): capabilities change at run time and a stored automation outlives them;
Home Assistant refuses what an entity can't do and the result says so.

A device's time (`dev_values_t.since`, Unix seconds) is Home Assistant's for what it knows -
since when an entity has been unavailable, when a scene last ran - and the panel's clock for when
the link to Home Assistant dropped (`dsrc_ha_link_at`; the list gives it to the devices that were
online then, and Home Assistant's own times stay as they were). The screen shows today's time
("Offline since 9:12 PM", "Ran at 8:30 PM"), an earlier day's date ("Ran on Sep 18"), with the
year for an earlier year ("Offline since Sep 18, 2025"), days and years in the panel's time zone
(`ui_when_text`); nothing ("Offline", "Scene") while the clock isn't set, or when the time can't
be converted or is more than a minute ahead of the panel's clock (BL-179).

BL-171's, BL-177's and BL-179's proposals were approved on 2026-10-06; a second review signed
them off that night with changes - the mode's neutral label where the action isn't known,
Preheating, "Couldn't confirm" for an unknown outcome, the year for an earlier year - and rejected
a position move toward closed without a code. BL-179's question (3) and BL-182 were settled on
2026-10-07: the code stays for every nonzero
Position target (HCTL-005: a lower target than a stale report shows can still open the cover),
and the page's note keeps the page's order (below). BL-171's thermostat question was settled the same
way: a thermostat at rest reads Idle in an enabled mode and Off only in mode Off (HCTL-004),
superseding the 2026-10-06 sign-off's Off in every mode. Home Assistant defines `off` as disabled
and `idle` as resting but able to resume, while sources differ in which word they send at rest:
some thermostat integrations say OFF whenever the system rests (Home Assistant already maps
that to idle outside mode Off), a thermostat with no equipment running may report idle in
mode Off; Z-Wave JS's operating state already tells the two apart. The
reported word alone read "Off · 72° now" with Heat lit, and "Idle" for a thermostat switched off.
The panel reads the reported mode (`ui_dev_t.state`) for the word only; the reported action and
the wire stay as they came (`resting_action()` in `ui_devices.c`).

### The screen
Drawn and approved (2026-10-01) in the redesign's mockup,
[mockup-next](../07-UI-LVGL-Pro/mockup-next/README.md) "The Smart Home screen (BL-045)": the
rail's Smart Home (a house with Wi-Fi), Favorites and the rooms as tabs, tiles of one or two
cells with the control on the tile, the state in words, detail sheets, and Arrange.
Tiles use the same design language as Home (colour means state, 64 px targets, words plus
icons). An unlock opens the code pad titled "Unlock Front Door".
A change reads as done the moment it is made (HCTL-006; BL-219, proposal
`mockup/proposals/BL-219-devices-at-once.js`, approved 2026-10-19). Seen on both panels with a
Home Assistant that answered at once: a light's slider sprang back when the finger lifted, the
tile said "Changing…", then jumped to the new level, because the screen showed the last report
until the next one came. Home Assistant's own toggle shows the new state at once and goes back to
the entity's state if no new report has come 2 s after the call; its slider stays where it was
let go; neither says anything while the call is on its way. The screen (`ui_devices.c`, "what was
asked") does the same for lights, switches, fans, thermostats and scenes: it keeps what was asked
per device, lays it over the device's report until a new report comes (or 2 s after the answer
with none), and drops it at once on a refusal, no answer or offline, which then show as before
(HCTL-012). Locks, garage doors, covers and valves keep their under-way words.
A link keypad shows the same view, from its master's registry over the link (HCTL-013; BL-216,
built as proposal P1 shows, approved 2026-10-16), and a lost link looks as a dropped Home
Assistant does: every tile "Offline since" the last device data.

The page's note is a short summary of the open page, not a log: each tile keeps its own outcome.
It picks at most two facts in four passes - jammed devices; unlocked locks and open entrance
covers; offline devices; failed actions (definite or unknown, HCTL-012) - each pass walking the
page in its order and stopping once two are found; how many lights are on fills a slot that is
left. Where every device on the page is offline it says so in one sentence. A fact it shows gives
way only when it resolves or a fact of higher priority, or earlier on the page, appears: a later
failure lower down the page never displaces the first two, and a poll that brings nothing new
leaves its words and layout still. No timers, no cycling, no newest-first: the newest failure
can't be told reliably from what the registry keeps (a failure's fingerprint, an offline or
scene time and a request id are not when it failed), and the page's order is stable (BL-182,
decided 2026-10-07). How it fits on one line is HCTL-006.

An offline device stays readable (BL-181, decided and its drawing approved 2026-10-07; the mockup's
devices-offline-controls and device-sheet-offline). The whole tile used to be
faded to 40 %, which left its name at about 3.45:1 and "Offline since" at about 2.07:1; now the
tile, the sheet's card and its body stay at full opacity, the name in `color_text` (15.8:1 on
`color_surface`) and the words in `color_text_2` (6.8:1). What says it can't be used: the chip at
`opa_device_offline_chip` (70 %, its tinted circle keeping `opa_tint`; about 3.3:1, above the 3:1 a
meaningful graphic needs), and each control - a tile's slider, stepper or keys, the sheet's switch,
slider, set point and keys - resting at 40 % (`style_disabled`) in its neutral grey: no tone, no lit
key, the switch shown off, an offline thermostat's mode keys all unlit (its reported mode is kept
and lit again when it answers). The controls are disabled (`LV_STATE_DISABLED`): LVGL gives them no
press, no pressed colour and no click, and the handlers refuse offline as before; the tile itself
takes its long press, so pressed it gives its key click and keeps its colour (`style_device_still`),
and a tap on it sends nothing (decided 2026-10-07). Section headings such
as "Mode" and the sheet's Close stay at full opacity. Nothing fades in or out: availability changes
the screen in one step, at the next refresh.

A long name stays inside its tile (BL-184, decided and its drawing approved 2026-10-07; the
mockup's devices-long-names and device-sheet-long-name). A name may be 47 bytes
(`DEV_NAME_LEN` 48): 47 W's are about 1000 px in `font_label`, where a small tile's inside is 176 x
108 and a wide one's 400 x 108. The name's label is whole lines of its font (`device_name_h`, 26 px,
one line; `device_name_two_h`, two) with no line spacing, and the state one line of `font_caption`
(`device_state_h`, 19 px), so LVGL ends what doesn't fit with "..." on the last line, in place: a
small tile is the chip (48), the gap, then the name at 63 and the state at 89 of its inside; a wide
tile with a slider, keys or a stepper a 48-high header (the chip, 12, then 300 px of words clear of
the 40 the arrow keeps, the name's one line and the state centred in it) over the control at 60; a
wide tile without one a 71-high header, the name's two lines and the state. A thermostat holding a
range hides its stepper and gets its second line back (`tile_name_lines`). LVGL writes its "..."
into the label's own copy of the text, so the tile keeps what it gave each label and compares the
device's name and words with that, not with the label: a poll that finds nothing new leaves a
shortened label alone. The sheet's title and state wrap in full (a name without spaces breaks
between characters, never inside one), the chip, the switch and Close stay at the header's first
row, and where a long name and a tall body don't fit the card (`pick_max_h`) the body scrolls under
the header. When the sheet's words change, the refit keeps the body where the person scrolled it,
clamped to its new range (`ui_sheet_body_fit`); a sheet always opens at its top. A long press opens the sheet on every tile, so two names that read the same on their
tiles can be told apart.

### Actions per domain
The integration only executes these, and only for entities its options allow (HAI-009); the
panel asks for a user code first where the table says so (HCTL-005).

| Domain | Actions | User code on the panel by default |
|---|---|---|
| `light` | turn_on (brightness, colour temperature), turn_off, toggle | no |
| `switch`, `input_boolean` | turn_on, turn_off, toggle | no |
| `fan` | turn_on, turn_off, set_percentage | no |
| `climate` | set_hvac_mode, set_temperature, set_preset_mode | no |
| `cover` | open, close, stop, set_position | **open, or set_position to any target but 0**: yes, for every cover whatever its class, reported position or direction of travel; close, stop and position 0: no |
| `lock` | lock, unlock | **unlock**: yes |
| `valve` | open, close | **open**: yes |
| `scene` | turn_on | no |
| `script` | turn_on | no (the owner opts in per script) |
| `alarm_control_panel`, `siren` | none | never controllable from the panel (no alarm loops) |

### Built-in automations, not a rules engine
Some installs won't run Home Assistant at all - native Zigbee devices (after M10) with nothing
behind them - so the basics have to live on the panel: the porch light when the front door
opens, the doors locked at a set hour, the alarm armed Night at bedtime. That is HCTL-009: one
trigger, one or two actions, alarm-centric on purpose. It is deliberately not a rules engine - no condition chains, no scripts. Anyone who
wants more will be running Home Assistant, already sees every panel state there (docs/features/10,
/23), and can build their own automations on the panel's entities; we don't duplicate that.
Automations can arm and disarm, lock and unlock, open and close and switch (HCTL-011): they act
as the person who saved them, and an installer option can take the less secure ones away.

## Test and acceptance
- Host: `home_control` model, security classes, code check path, result handling
  (`components/core/home_control/test/test_home_control.c`, tagged HCTL-*; it links `home_control` and unity only -
  the registry never touches the alarm engine). Tagged: HCTL-001, 002, 004,
  005, 006 and 012, and the Home Assistant source's round trip through the registry.
- Preview: the Smart Home view with each kind, detail sheets, unlock with code, offline source.
- Integration rig: each HA domain through the bridge (docs/features/23).

## Open questions
- Show a few favorite devices on Home as well, or keep Home for the alarm only? Leaning: keep
  Home for the alarm; Smart Home is one tap away.
- Allow device control while armed away (for example from a remote keypad at the door)?
  Decided 2026-10-16 (the owner's decision 3 for link keypads, docs/features/11 Decisions,
  BL-216): the same on both screens - allowed; unlock and open need the code (HCTL-005).
- A Z-Wave device source, so the panel's own Z-Wave switches, dimmers and garage doors
  (docs/features/36 ZW-034 to ZW-036, switched from their device pages today) also appear here?
  Leaning: yes, once the screen exists; `zw_outputs` already follows this document's rules (a
  garage door opens only with a code, closes without; a pending action has a bounded lifetime).

## Tasks
- [x] `home_control` model + device interface + host tests (commit eadd9d2)
- [x] HA device source (`src/dsrc_ha.c`): catalog, domain and action tables, `call` out and
      `call_result` in; `ha_link.c` feeds `device_service_ha_*` and sends calls through
      `device_service_set_sender` (eadd9d2, 1747d43)
- [x] Smart Home view as approved in mockup-next: the rail item, tiles of one or two cells
      grouped by area with Favorites first (HCTL-003), the stored layout (`ui_dev_model.h`), a
      tap, an unlock or an opening over the code pad (HCTL-005), pending, failed and offline
      shown (HCTL-006), through new `ui_backend_t` hooks; preview shots (BL-045 part A)
- [x] The Smart Home view's detail sheets: a light's brightness, a thermostat's target and mode,
      a cover's position, a fan's speed (HCTL-004) (BL-045 part B, 0a228fe)
- [x] Arrange on the Smart Home view: the pencil, a tile picked, moved by the arrows, small or
      wide, off Favorites and added back, saved on Done (HCTL-008) (BL-045 part C, 244462b)
- [x] Arrange: hold a tile to lift it and drag it onto another tile's place (`ui_devl_move_to`);
      the grid doesn't scroll under the drag and steps at its top and bottom edges, a still
      finger moves the tile once at most, the arrows stay; saved on Done (HCTL-008;
      preview, a panel 2026-10-06; 0f5223b)
- [ ] The rest of the drawn sheets: a light's warmth and a thermostat's fan mode (new
      `home_control` actions) and its humidity (a field in `ui_dev_t`) (HCTL-004) (BL-045)
- [ ] Settings > Devices (hide, rename): devices picked from a search, renamed and hidden by
      the master or installer (HCTL-008) (BL-045)
- [x] `device_service_tick()` calls `hc_tick` from the health task every 2 s (HCTL-012's timeout
      backstop; `hc_refresh` handles a reconnect at once) (1747d43)
- [ ] Events for unlock / open (append-only event types): an unlock or an opening from the panel
      logged with who and which device (HCTL-007), new `alarm_event_type_t` values appended
      (BL-045)
- Built-in automations (HCTL-009, HCTL-011): their own track, docs/features/41 (BL-168 to
  BL-170)
- [ ] Native Zigbee device source (after M10): `DSRC_ZIGBEE` on `zigbee_host` once the H2 runs,
      host-tested (HCTL-001); the devices on a panel with the H2 are docs/features/20's box
      (ZB-009, HIL) (BL-036)
- [ ] On a panel with Home Assistant: each domain through the link both ways and a source going
      offline passed (2026-10-06, docs/features/23 validation.md part 2); left: an unlock and an
      opening with a code typed at the panel (BL-045)
- [x] A device's values over the link: brightness, setpoint and modes, position and fan speed
      in the catalog and the state message (protocol.md, `ha-addon` in step), kept in
      `ui_dev_t` so the tiles and sheets show them (`proto_link_values_t`, `dev_values_t`,
      `ui_dev_values_fill`); the console's `devices act <ref> 11 <mode>`; a failed action in
      the words of its action whoever started it (`hc_device_t.action`) (HCTL-002, HCTL-004,
      HCTL-006; host tests, a panel 2026-10-06; 0f5223b)
- [x] What a device can do over the link (`supports`), its time (`since`) and `hvac_action`
      kept: Stop and Position only where a cover can, a fan without speeds as a switch, "Ran
      at" and "Offline since" on a panel (HCTL-002, HCTL-006; host tests, preview, a panel
      2026-10-06, docs/features/23 validation.md part 5; 0f5223b)
- [x] The user acceptance run's defects (validation.md part 4): B030, Arrange's scroll after a
      header edit, a range thermostat's tile without - and +, and a newer state clearing a failed
      action's words (BL-175) (a panel 2026-10-06, part 5; 0f5223b)
- [x] The approved words: what a thermostat is doing, the note's failed and unconfirmed
      actions, dates for an earlier day, a moving cover turned back, a failed value's tap opening
      its sheet; Home Assistant's call "timeout" an unknown outcome; the note's outcome kept
      whole beside the widest tabs (BL-171, BL-179 (1) and (2); host tests, preview, a panel
      2026-10-07, docs/features/23 validation.md part 6; bd561d8)
- [x] Decided 2026-10-07: the code stays for every nonzero cover Position
      target, a target below the reported position included (BL-179 (3)), and the page's note
      keeps the page's order (BL-182); HCTL-005 and HCTL-006 say so (host tests, preview;
      c6324d2)
- [x] A thermostat at rest reads Idle in an enabled mode, Off only in mode Off (HCTL-004)
      (BL-171, 12b5489)
- [x] An offline device stays readable: the chip at 70 %, the controls resting at 40 % (HCTL-006)
      (BL-181, 12b5489)
- [x] Long names stay in their tiles and show whole in the sheet (HCTL-004) (BL-184,
      12b5489)
- [ ] On a panel: a thermostat at rest, an offline device's tile and sheet, long names on tiles,
      in Arrange and in sheets, and that nothing flashes as they change (UI-020) (BL-183)
- [ ] On a panel, what part 6 couldn't show: a person watching that nothing flashes as Zones'
      empty words, the note and the tiles' words change; "didn't turn on"; "Offline since" with
      an earlier day's date (HCTL-006, UI-020) (BL-183)
- [ ] A name with line breaks: drop them (and other control characters) from names, or let the
      sheet's header scroll too (BL-185)
- [ ] What BL-171 left: a cover's class and a thermostat's presets over the link; Z-Wave
      devices' values in `dsrc_zw` (BL-171)
- [ ] Stage the Home Assistant source's catalog as `dsrc_zw` does, so a reconnect or an options
      change keeps a failed action's words and an action under way (HCTL-006, HCTL-012) (BL-178)
- [ ] Arrange: no Small and Wide for a fan without speeds, and a way back to a kind's own size
      (HCTL-008) (BL-180)
- [x] A link keypad's Smart Home as the master's, as proposal P1 shows (approved 2026-10-16), in
      the PC simulator and the preview (HCTL-013) (BL-216, 787c2da)
- [ ] A link keypad's Smart Home on the panels with Home Assistant connected and devices:
      docs/features/11 Test and acceptance (HCTL-013) (BL-216)
- [x] A change to a light, a switch, a fan, a thermostat or a scene reads as done at once, as proposal
      BL-219 shows (approved 2026-10-19), in the preview (HCTL-006, HCTL-012) (BL-219)
- [ ] Changes at once on both panels with the bench Home Assistant (HCTL-006) (BL-219)
