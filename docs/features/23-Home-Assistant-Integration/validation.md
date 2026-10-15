# M12 validation — 2026-09-20

## Software and release

- Public integration: [v0.1.0 testing release](https://github.com/RoboAlarms/ha-addon/releases/tag/v0.1.0), commit `b03ab4f`.
- 145 Python tests passed under Home Assistant 2026.9.3 / Python 3.14 in Docker. Ruff lint,
  formatting and standalone-client synchronization passed.
- GitHub [Hassfest/HACS validation](https://github.com/RoboAlarms/ha-addon/actions/runs/35539639930)
  and [Python/package checks](https://github.com/RoboAlarms/ha-addon/actions/runs/35539639919) passed.
- Standalone `aioroboalarms` wheel built, installed and checked against a pairing golden vector
  without importing Home Assistant. Its module matches the integration's protocol client byte-for-byte.
- 20 portable firmware host suites passed. P4 v1 and v3 firmware builds passed.

## On a real panel

- Numeric-comparison pairing matched both sides and committed successfully.
- Reconnected using the saved client certificate; received one partition, eight zones and
  diagnostic/firmware status. Device identity is nonzero and stable across resets.
- Home Assistant running in Docker loaded the actual TLS connection, exposed disarmed alarm
  and uptime entities, reconfigured the loaded entry and unloaded cleanly.
- Incomplete headers disconnected in 10.03 seconds; incomplete bodies in 10.10 seconds.
  Switching the integration off cancelled an incomplete frame, and a healthy client reconnected.
- A client sending pings without reading responses was disconnected; a healthy client then connected.
- A 1,000-connection interruption soak passed. Before/after internal free bytes: 285,707 /
  287,179. PSRAM free bytes: 28,257,428 / 28,257,428. Minimum remaining link stack: 6,540 bytes.
- Simulated NVS write failures rejected pairing without saving trust. A failed unpair revoked
  the active stalled session immediately and reported `unpair_unsaved`; disabling the fault
  let the five-second retry persist revocation across reset. Successful pairing and unpairing
  both survived immediate post-result resets. `bench-durability.log` ends `DURABILITY PASS`.
- The test hook is guarded by `CONFIG_HA_LINK_DIAGNOSTICS` (default off); normal images
  compile it out. This simulates failed writes without filling/corrupting production NVS.
  These tests do not physically interrupt power in the middle of an individual flash write.
- Final normal v1 image flashed; both final v1/v3 builds passed. The normal binary contains
  no fault-injection command, and invoking that development command only prints status.
  The panel is disarmed, HA enabled but unpaired, ready for a HACS setup.
  Final internal free RAM: 291,611 bytes (284 KB); PSRAM free: 28,257,528 bytes.
  Crypto ready, zero faults; display check: zero late frames and zero backlight guard trips.
  Final evidence: `build/m12/final-device.log` and `flash-final.log`.

Evidence is in git-ignored `build/m12/`: `final-python-tests.log`, `host-tests.log`,
`live-ha.log`, `bench-pair.log`, `bench-transport.log`, `bench-soak.log`, and
`bench-durability.log`. The test identity and all device details remain there, not in the
public integration repository.

## Fixes exposed by the hardware run

1. P4 has no local Wi-Fi MAC; derive the panel ID from its own factory eFuse identity and
   check the read result instead of advertising `000000`.
2. Retain peer certificates after TLS negotiation so pinned fingerprints can be checked;
   fail compilation if that configuration is absent.
3. Reuse the existing PSRAM transmit buffer instead of nesting 4 KB stack buffers in
   message dispatch and hello sending. The previous nesting overflowed the link stack.
4. Notice EOF while waiting for pairing approval, releasing the single connection slot.
5. Reset/publish session state when pairing succeeds, propagate installer-mode changes,
   and refuse a socket if its bounded send timeout cannot be configured.

## Scope and remaining external verification

- HACS testing release is published. PyPI publication still requires an account/project or
  trusted-publisher configuration; the wheel is ready and attached to GitHub.
- Automatic mDNS discovery could not be exercised across the test computer's and panel's
  different subnets. The flow is tested in Docker; routed networks need an mDNS reflector
  or manual host entry. Manual host entry and live TLS are verified.
- Live arming/disarming/duress with real codes was not attempted. Alarm rules have
  portable engine tests; integration command/error paths have Docker tests. An acceptance run on a
  real Home Assistant should exercise these with an authorized code.
- Battery percentages and zone radio signal depend on future source telemetry. Current
  sources expose low-battery and supervision flags. The firmware update entity reports
  OTA status; installation stays authorized on the panel.
- M13 Devices UI and policy work are separate. HACS default-store inclusion is separate
  from installation through this custom repository.

# BL-027 acceptance, part 1 — 2026-09-29

The run was on a Home Assistant in Docker Desktop, with the panel flashed with the dev profile.
Rig: Home Assistant 2026.9.3 core (the `roboalarms-test` image,
pytest-homeassistant-custom-component) kept running as one session and driven step by step,
the integration from `RoboAlarms/ha-addon` at v0.1.0 plus the fix below, the panel's service
console on USB for the panel-side steps. Evidence and the session's scripts are in the
git-ignored `build/bl027/`.

## Found and fixed first

- **B017** (docs/bugs): current `main` rolled back on the panel - every start aborted with the
  web server on, one URI handler slot short. Fixed and flashed; the image passed probation.
- **CO detectors had no device class in Home Assistant**: the panel names the class
  `carbon_monoxide`, the integration's table knew `co`. Mapped, with a test in `ha-addon`
  (149 pass), and seen live as `carbon_monoxide`.

## Passed on the panel

- Pairing by address (manual entry): the flow reaches the panel over TLS; with no pairing
  window open it says `not_pairing`; the code matched on both sides and **Reject** on the panel
  ended the flow `pair_rejected` with nothing added; a second pairing with **Allow** created the
  entry and the panel reported "connected".
- Entities: the alarm panel (disarmed, numeric code, arm home/away/night), eight zones each with
  tamper, low-battery and supervision sensors, AC power, trouble, installer mode, chime switch,
  exit-restart button, event, uptime, Wi-Fi signal and firmware update.
- Zone events from the panel's simulator: open, close, tamper and low battery in Home Assistant
  as soon as the console command returned; supervision after the panel's 58 s grace for a
  silent sensor. Events carry the type, zone and name, the trouble, silent and replayed.
- Link loss: the link switched off made every entity unavailable at once; on again, back
  within a second or two. A panel restart: unavailable at 21 s, back and disarmed at 27 s.
- "Pair again": an unpair on the panel made every entity unavailable and started the reauth
  flow; pairing again with Allow ended it `reauth_successful` and everything came back.
- Two-way zones (HAI-009): a Home Assistant door sensor shared in the options reached the
  panel, whose Home Assistant source turned usable.

## Not yet

- The code steps (arm away and home with a real code, a wrong code, an entry alarm,
  duress) - a script asks for the codes and never shows them.
- A zone bound to the shared door at the panel's picker (installer mode), following it and
  going to CHECK when it turns unavailable; the zone set back to Not used.
- The factory-reset repair (it erases the panel's Wi-Fi too) and discovery on a real
  home network (Docker Desktop's network doesn't carry mDNS from the LAN).
- The Wi-Fi signal entity stays unknown: the panel reads RSSI 0, its C6 co-processor firmware
  (2.12.3) being a major version behind esp-hosted 3.0.7. A C6 update is the panel owner's call.
- The firmware update entity is unknown while the panel has no release source.

# BL-027 and BL-045 on a panel, part 2 — 2026-10-06

A test Home Assistant with fake devices in it, and every kind of device tried on a panel both
ways (its codes are the factory ones).
Rig: Home Assistant 2026.9.4 (the `stable` container) in Docker on a Linux machine on
another subnet, onboarded without the browser; the fakes are the `demo` integration's lights,
switches, locks, thermostats, covers, fans and valves, plus template helpers: thirteen binary
sensors (one per zone device class, each fed by an input boolean, all taken unavailable by one
switch), a garage door, blinds and a lock, a scene and a script. The integration from
`RoboAlarms/ha-addon` at 3fa831c plus the two fixes below. A panel (v1 silicon,
`bl-168-automations` at cdae263, later with the console's `zone bind ha`), driven through its
service console and panelctl's remote screen; a recorder on Home Assistant's websocket logged
every state change, service call and panel event.

## Found and fixed
- **B029** (docs/bugs): the link dropped every 95 s while nothing happened - the integration
  never pinged. Fixed in the integration.
- **A scene nobody had run was offline on the panel**: a scene's state in Home Assistant is the
  time it last ran, "unknown" until then, and the integration sent unknown as unavailable, so its
  tile was greyed and a tap refused. A scene's unknown now counts as there.
- Both fixes are in `ha-addon` (committed since as 7b6671a): 165 tests and Ruff pass, each
  with a test that failed before it (BL-173).
- **No way to bind a Home Assistant sensor without the installer's code typed at the panel** (a
  remote screen session can't type one, DEV-010): the console's `zone <n> bind ha <entity_id>`
  (docs/features/22 "Console").

## Passed on the panel
- Pairing by address: the same code on both screens, Allow, the entry created; the panel's eight
  zones (each with tamper, battery and supervision), trouble, AC power, installer mode, chime,
  exit restart, event, uptime and Wi-Fi signal as entities.
- Devices, panel to Home Assistant (the console's `devices act` and taps on Smart Home): lights
  on, off and to 30 %; switches on, off and toggled; an input boolean; locks unlocked and locked
  (the demo's badly fitted lock jammed: Smart Home showed it jammed, in amber, and named it at
  the top); a thermostat's setpoint 22.5; covers closed, opened and set to 40 %; a fan on and
  to Medium (67 %); a valve closed; a script and a scene run. Opening a garage door and a valve
  asked for a code (HCTL-005); closing and locking didn't.
- Devices, Home Assistant to panel: every one of those changed in Home Assistant reached the
  panel's list and Smart Home within a second.
- A thermostat in heat/cool mode refused a single setpoint (Home Assistant's own rule): the
  panel showed the action failed.
- Alarm: Stay, Away and Night armed from Home Assistant with a code (the exit delay as
  `arming`, then armed); Away armed and Night disarmed at the panel's keypad, seen in Home
  Assistant; disarmed from Home Assistant during an exit delay, in an entry delay (`pending`)
  and when armed; a wrong code refused and logged as `invalid_code`; bypass and unbypass by
  service; the exit restart refused in Away and during the exit delay, accepted armed Stay (the
  engine's rule).
- Zones from the panel's simulator: open, tamper, low battery and a smoke zone's trouble in Home
  Assistant at once, and back.
- Home Assistant sensors as zones (bound with `zone bind ha`): a garage door, window, motion,
  smoke, CO and water sensor each followed its source, from Home Assistant through the panel
  and back to Home Assistant's zone entity in 67 ms; armed Away, the garage sensor opened in Home
  Assistant started the entry delay and Home Assistant disarmed it; all thirteen sensors
  unavailable: nothing during the 120 s grace, then every bound zone CHECK (supervision) with the
  trouble beeps and Home Assistant's supervision sensors on, all clear within 3 s of their
  return. The zones were put back on the simulator afterwards.
- Link loss: Home Assistant stopped: every device greyed, "Your devices can't be reached right
  now"; started again: connected within half a minute and every device back. The panel's link
  off: every entity unavailable; on: back within seconds. The panel flashed: Home Assistant
  reconnected by itself.
- Every panel event of the run (armed, disarmed, entry delay, exit restart, invalid code, bypass,
  unbypass, trouble, trouble restore, troubles acknowledged) reached both the event entity and
  the `roboalarms_event` bus event.

## Found, not fixed
- **Only a device's state word crosses the link**: a light's brightness, a thermostat's setpoint
  and modes, a cover's position and a fan's speed never reach the panel. Smart Home shows "Heat
  to –" and a fan with no speed lit; a thermostat's sheet is empty, so its mode can't be changed
  from the panel, and a light can't be dimmed there (BL-171).
- The console's `devices act` sends no mode: a mode change from it is refused (`not_allowed`)
  (BL-171).
- An action the screen didn't start (the console's, an automation's) that fails reads "Didn't
  respond" on its tile, even when Home Assistant refused it at once (BL-171).
- With the installer's default, "chime needs a code", the panel refuses Home Assistant's chime
  switch: it never works on a default panel (BL-172; fixed in the engine since, CORE-CHIME-007:
  remote clients switch the chime without a code).
- The alarm entity's `changed_by` stays empty, though every panel event names the user; a wrong
  code shows in Home Assistant as a general error rather than a `ServiceValidationError`
  (BL-173).

## Not yet
- Duress, an entry alarm that sounds, a wrong pairing code and a man in the middle, a zone added
  and removed, the factory-reset repair and discovery on a home network (BL-027).
- An unlock or an opening with a code typed at the touchscreen (a remote session can't type
  one): someone at the panel (BL-045).
- A Home Assistant sensor bound at the touchscreen's picker (installer mode) (BL-027).

# The bench's fixes on the same panel, part 3 — 2026-10-06

A panel (v1 silicon) running `bl-168-automations` at cdecf36 with the uncommitted fixes for
BL-171, BL-172 and Arrange's drag; Home Assistant 2026.9.4 with the integration's uncommitted
BL-171 and BL-173 changes (186 tests). The rig of part 2.

## Passed
- A device's values reach Smart Home (BL-171): a light "On · 71%" with its Brightness slider, an
  off light that can dim keeps its slider (it sends level 0), a thermostat "Cooling · 22° now",
  "Cool to 21°" and its Off, Heat, Cool and Auto keys, a cover "Open · 10%", a fan's speed keys;
  a thermostat holding a range reads "Set to –" (no single target, as protocol.md says).
- Heat on the thermostat's sheet reached Home Assistant and the sheet followed; the console's
  `devices act climate.hvac 11 cool` set `cool`.
- A setpoint Home Assistant refused (a thermostat holding a range), sent from the console: its
  tile reads "Didn't change", its action's words.
- Home Assistant's chime switch on a default panel ("chime needs a code" on): on and off,
  `chime_on` and `chime_off` from source remote (BL-172, CORE-CHIME-007).
- A wrong code from Home Assistant's UI (its websocket): `service_validation_error`, "The panel
  did not accept that code" (BL-173). Home Assistant's REST API answers any
  `ServiceValidationError` with a 500; that is Home Assistant's, not the integration's.
- Arm Stay and disarm from Home Assistant with the master code: `changed_by` names the master
  both times (BL-173).
- Arrange: holding a tile lifted it (blue edge, its actions in the header) and dragging it along
  a row moved it there and back; the new header words fit.

## Found
- Arrange: a tile held on the last row's tiles kept stepping the grid down under the finger:
  the edge band was half a tile high and the grid stepped a quarter row 6.7 times a second.
  Now a third of a tile and half a row every 350 ms (under 3 a second): on a panel a tile
  dragged onto the last row stays there, and held at the very bottom it steps the grid to
  the end; the preview checks both.
- Smart Home's note still says "{0} didn't respond" for a device whose last action failed,
  whatever the action (BL-171).

## Not yet
- The integration's fixes committed and released (BL-173); what BL-171 left (its row).

# User acceptance on the same panel, part 4 — 2026-10-06

The panel and Home Assistant as in part 3. It was tested one area at a time, about 12:40 to 16:07:
Smart Home's devices both ways, Arrange, the alarm and the link, every main screen; each failure
was run again independently, a reviewer judged what was covered and what the evidence showed, and
a last round ran its gaps. 104 cases: 85 passed, 7 failed (6 confirmed, 1 the bench's own), 12
blocked (a code typed at the touchscreen, installer programming, a ping the panel never sends,
flicker a screenshot can't catch).

## Passed
- A device's values both ways (BL-171), every kind on the panel: lights (a tile's or a sheet's
  slider sends one brightness, on release; an off light that can dim keeps its slider, a light the
  panel had never seen too, so the integration sends level 0), fans (Home Assistant's 20, 50 and
  90 light Low, Medium and High), covers and the valve (position 40 from the console reached Home
  Assistant; Close, Stop, the slider to 0), thermostats (setpoint, "now" and the mode keys both
  ways; Auto on a thermostat with `heat_cool` and no `auto` sends `heat_cool`; one added to the
  options shows only its own modes), locks (Lock; Unlock asks for a code; jammed shown), the
  scene and the script. The console's `devices act <ref> 11 heat|cool|auto` (`dry` gets the
  usage line). A refused action in its own words ("Didn't change", "Didn't turn on").
- Home Assistant restarted: every device offline, all back within about 10 s. An options change
  sent every state again; the other tiles kept their words and Favorites its order.
- Live words: with Home Assistant changing a brightness six times a second for 8.7 s, the tile
  changed at most once a second and showed the last value within 0.6 s of the last change.
- Arrange (HCTL-008): a 0.6 s hold lifts a tile, a 0.25 s press scrolls; drags along a row,
  between rows and onto the bottom row (no runaway); held at an edge the grid steps exactly half
  a row every 350 ms (about 2.9 a second) and stops at its end; a still finger moves nothing
  more; a release over the header's Remove or Done, or over the rail, presses nothing; a device
  changed in Home Assistant during a held drag leaves the grid still and its new words appear
  once, after the release; Small and Wide, Remove and Add, Done, leaving the view; the Other page.
- The chime (BL-172): Home Assistant's switch on and off without a code (`chime_on`/`chime_off`
  from source remote); the touchscreen's bell asks for a code; the console's keypad without a
  code is refused (`code_required`) and with the master's code Home Assistant's switch follows.
- The alarm from Home Assistant and from the console's keypad: Arm Stay, Arm Night and disarm
  with the master code, `changed_by` naming the master each time; Smart Home works while armed.
  An 11-digit code is stopped by Home Assistant's schema and never reaches the panel; a wrong
  10-digit code reaches it and comes back as "The panel did not accept that code" (BL-173).
  `automation_failed`, one of the 25 new event types, reached `event.home_event` at 12:30.
- A zone bound to a Home Assistant sensor (`zone 3 bind ha`) followed it within 81 ms: Open and
  NOT READY, then Closed and READY.
- A scene whose state is `unknown` (never run) stays a working Scene tile; a switch's `unknown`
  is Offline.
- The link stayed up through every quiet stretch (B029's keepalive): 40 minutes of Home
  Assistant's log and a 35-minute run without a drop. The integration's `pong`: its test only.
- Every main screen: Security, Zones and its filters, Smart Home, Activity and its filters, the
  Settings pages a code doesn't guard and the words behind each code pad, About and System
  details, the screen saver.

## Found
- **B030** (docs/bugs): a tap on an off Home Assistant light's tile, or its sheet's switch, sent
  brightness 0: the light stayed off and nothing said so. Older than today's work (BL-174).
- Arrange: an arrow, Small, Wide or Remove rebuilds the grid and scrolls it to the top, so a
  picked tile further down leaves the view after every tap (BL-174).
- A failed action's words stay on a tile after Home Assistant reports a newer state (a fan turned
  on in Home Assistant still reads "Didn't change"): docs/features/24 keeps the last failure on
  purpose (BL-175, a decision).
- The link carries no capabilities: a cover that can't stop offers Stop, and a fan with presets
  and no speeds gets only speed keys, which Home Assistant refuses (BL-171).
- A scene's "Ran at" and a device's "Offline since" never show on a panel (only the simulator
  fills the time); a thermostat holding a range shows - and + on its tile that send nothing
  (BL-174). A setpoint shows in whole degrees (23.5 as 24°), and a thermostat idle in heat reads
  "Heating": what it is doing (`hvac_action`) doesn't cross (BL-171).
- System details gave the date of an earlier build: ESP-IDF stamps it only when
  `esp_app_desc.c` is compiled, so an incremental build keeps an old one (BL-176).
- Words for proposals: an empty Zones filter shows nothing at all, and Arrange's Add sheet says
  "Devices in your rooms" where no device has a room (BL-177).
- The bench, not the panel: taking the remote session's lease wakes the screen saver (by design),
  so a "wake" tap sent blind landed on Security's Arm tiles and opened their code pads; nothing
  armed. The bench's script now wakes the panel without a touch.

## Not checked
- Flicker: a screenshot takes 0.5 to 1.6 s, so a flash shorter than that can't be seen. Arrange's
  edge steps, the grid's rebuild and value updates need a person at the panel.
- Everything behind a code typed at the touchscreen: Users, Wi-Fi, Integrations and its Home
  Assistant page, Automations, Walk test, Installer; an unlock or an opening (BL-045, BL-027).
- The integration's `pong` with a panel: the panel never sends a `ping`.

# The fixes on the same panel, part 5 — 2026-10-06

The panel was flashed at 21:12 with the fixes for part 4 (still on `bl-168-automations`, not yet
committed), the image built 20:38; the integration with its side of them on the same Home
Assistant. Every check CI runs passed first on the PC: host 112/112, the preview's every walk,
both mockups, the firmware at 75.1 % of its slot, the integration's 193 tests.

## Passed
- B030: a tap on an off light's tile, or its sheet's switch, sends `light.turn_on` without a
  brightness and the light comes on at its last level; a light that can't dim the same.
- Arrange: after an arrow, Small, Wide, Remove or Add the grid stays where it was and the picked
  tile stays in view, at the bottom of the list and part way up.
- A thermostat holding a range has no - and + on its tile; one with a single setpoint keeps them
  and they work.
- What a device can do crosses the link (`supports`, protocol.md): a garage door that can't stop
  has Open and Close only, on its tile and its sheet, and a tap while it moves opens its sheet; a
  window that can stop and take a position keeps Stop and its slider; a fan without speeds is one
  cell that switches on and off; a fan with speeds keeps its keys.
- "Ran at" with the right time after a scene ran from its tile and from Home Assistant; "Offline
  since" with Home Assistant's time for a device made unavailable, and with the panel's own time
  for every device while the link was down; all back after.
- A failed action's words go when Home Assistant reports something newer (BL-175): a refused
  setpoint's "Didn't change", then a mode set in Home Assistant, and the tile read the new mode.
- `hvac_action` reaches the panel (the console's `devices`: `hvac:cooling`, `hvac:idle`), and
  nothing new shows on the screen.
- System details: "built Oct 6, 2026", this build's date (BL-176).
- Again: a light's brightness and a thermostat's mode both ways, a lock, a drag in Arrange, Arm
  Stay and disarm from Home Assistant with `changed_by`, the chime switch, Activity.

## Found
- A cover's Position slider moved toward closed (20 % to 10 %) asks for a code, as an opening
  does: HCTL-005 counts any position above 0 as opening (BL-179, a decision).
- Arrange offers Small and Wide for a fan without speeds, and Wide draws an empty two-cell tile;
  after Small or Wide a tile keeps that size, with no way back to the size of its kind (BL-180).
- The integration's catalog re-lists every device on a reconnect or an options change, so a
  failed action's words go then and an action under way fails as offline (BL-178).

## Not checked
- Flicker, for the reason in part 4.

# The approved words on the same panel, part 6 — 2026-10-07

The panel was flashed at about 05:58 with the touchscreen proposals as approved and signed off by
review (BL-171, BL-177, BL-179 (1) and (2); still on `bl-168-automations`, not yet
committed), the image built at 05:30 with no source newer than it; disarmed first, the hash
verified, booted in about 30 s to 0.7.0 with Wi-Fi, the time, the Home Assistant link, its 19
devices and both stored automations. The integration was not redeployed. The checks for this
change passed first on the PC: host 112/112, the preview's every walk (238 checks of words and a
device's time) and the build without its optional pages, the firmware at 75.1 % of its slot.
The bench made each condition: thermostat activity and covers written into Home Assistant's state
machine (`/api/states`); definite failures from a Stop to a cover that can't stop, a set point
above the maximum and a position to a cover without one; an unknown outcome from Home Assistant's
container paused for 44 s, longer than the panel's 30 s action timeout; zones from the panel's
zone simulator. The bench's recorder of Home Assistant's service calls confirmed every "sends
nothing". 15 cases: 14 passed, 1 blocked (flicker).

## Passed
- A thermostat's words come from `hvac_action` only (mode Heat): "Heating · 22° now", "Idle · 22°
  now", "Off · 22° now", with no action the mode's "Heat · 22° now"; the sheet's "Preheating" for
  preheating, and the neutral "Heat" for no action and for one with no words of ours (`drying`);
  the set point "Heat to 21°" throughout.
- A definite failure names the device on the page's note: "Garage Door didn't stop" (Stop to a
  cover whose source refused it); a set point above the maximum read "Hvac didn't change" on the
  note and "Didn't change" on the tile and the sheet; a refused position read "Didn't change".
- An unknown outcome: with Home Assistant frozen, a tap on AC read "Turning on…", then after the
  timeout "Not confirmed" on the tile and "Couldn't confirm AC turned on" on the note; the link
  stayed up, and when Home Assistant ran the queued call 41 s after the tap the tile read "On"
  with the uncertain words gone.
- A moving cover that can't stop turns back (BL-179 (1)): closing, a tap opened the code pad
  ("Open Garage Door"), and Cancel sent nothing; opening, a tap sent `close_cover` without a code.
  A cover that can stop still sent `stop_cover`.
- A tap on a tile whose position or set point change failed opens its sheet and sends nothing,
  instead of retrying with the reported value.
- Zones' empty filters: "No open zones", "No bypassed zones", "No zone troubles" with the green
  check; with Open selected, `zone 3 open` brought the list back in the first frame captured
  after it (within about 1.5 s) and `zone 3 close` the empty words.
- Arrange's Add sheet: "Tap a device to add it at the end", and the tile went back at the end;
  with every device on Favorites, "Every device is on Favorites already".
- A scene's dates (BL-179 (2)): "Ran on Oct 6" for a run at 02:27 UTC, the previous local day;
  "Ran at 6:12 AM" after a run from its tile; "Ran on Sep 19, 2025" for an earlier year; "Scene"
  for a time in the future. "Offline since 6:13 AM" for a device made unavailable today.
- Again: B030 (a tap on an off light turns it on at its last level, no brightness 0); a failed
  action's words cleared by a newer state (BL-175), "Not confirmed" too; what a device can do
  (no Stop or Position where a cover can't, a fan without speeds as one cell, a fan with speeds
  keeps its keys); Arm Stay and disarm with the master code typed on the touchscreen's pad by remote
  taps, Home Assistant's entity following with `changed_by`, a digit typed in a separate remote
  session discarded when that session ended (DEV-010); the chime switched from Home Assistant.
- Afterwards every device, Favorites' order, the chime and the alarm state were as before the
  flash; the console's `devices` matched its list from before line for line.

## Found
- The note's two-fact limit keeps the failures that come first on the page, not the newest: a
  later Hvac failure replaced "Garage Door didn't stop", and a later Garage Door failure didn't
  replace Hvac's. The rules that pick the facts were kept unchanged on purpose; whether the newest
  should win was open (BL-182). Decided 2026-10-07: keep the page's order - what was seen here is the policy, now written in
  docs/features/24 HCTL-006 and proved in the preview walk; a failure the note has no room for
  stays on its tile.
- A fan's turn-on with a percentage on a fan without speeds isn't refused: the integration drops
  the percentage and the fan comes on. Right, but no bench device could then produce "didn't turn
  on" (BL-183).
- The bench, not the panel: Smart Home goes back to Security after its idle time (by design), so a
  blind tap or swipe after a pause landed on Security's Away tile and opened its code pad; nothing
  armed. Wake the panel and look before each touch after a pause.

## Not checked
- Flicker, for the reason in part 4: none of about 70 frames showed anything flashing, but a
  person has to watch (BL-183).
- "Didn't turn on" on a panel, and "Offline since" with an earlier date: Home Assistant's
  `last_changed` can't be set back through its API (BL-183).
