# 17 · Power and system supervision

**Status:** In progress - restore after power loss, the STC battery reading, the battery icon and
the power-saving policy are host-tested; the reading has run on a panel with no battery fitted
(2026-09-20); the reset reason is in `system_start` (on a panel 2026-10-01). Left: acceptance
with a battery and an AC-loss signal (BL-038), the task watchdog (BL-068).
· **Milestone:** M2 (watchdogs, restore), later (AC/battery sensing) · **Depends on:** 01, 06, 16
**Code:** [`components/core/power_monitor`](../../../components/core/power_monitor),
`components/boards/board_crowpanel_p4/src/board_battery.c`, the `health` task in
`components/app/panel_app/src/panel_app.c` · **Tests:**
`components/core/power_monitor/test/test_power_monitor.c`, `CORE-PWR-*` in
`components/core/alarm_core/test/test_persistence.c`, the UI preview's battery states

## Goal
Keep protecting through power cuts, crashes and failing subsystems, and turn every such
problem into a visible, reported trouble instead of a silent failure.

## Reference behaviour
A keypad alarm panel runs on a backup battery during AC loss, shows AC LOSS / LO BAT, delays the
AC-loss report (to avoid floods of reports during area-wide outages), returns to its previous arm
state after a total power loss, and supervises keypads, expanders, sirens and RF sensors (CHECK).

## Facts (CrowPanel)
- Power in: USB-C 5 V (8-10 W with the screen on). Li-ion battery via a TP4059 charger.
- **Power inputs (V1.1 schematic, read 2026-09-24, not measured):** three 5 V inputs, each
  through its own Schottky diode (DSS34), meet on VCC5V: the USB-C port marked UART0 (VBUS1,
  D10), the one marked USB2.0 (VBUS2, D007) and UART3-IN's +5V (J10, +5V_IN, D2; Elecrow: 5 V
  ±5 %, 2 A max). No input can feed another or a computer, so a second supply may stay
  connected while a computer is plugged in; Elecrow asks for one (USB2.0 on a supply) because
  a computer's port alone can black the screen out. VCC5V goes through the POWER switch (SW1
  driving Q5) to VDD5V, which feeds the regulators and **J7's 5V pins: an output, not an
  input**. The battery joins VCC5V through Q3, which conducts only while none of the three
  inputs has power (gate VCC5V_IN, the inputs through 1N5817 diodes, pulled low by R18); the
  TP4059 charges from VCC5V_IN. A supply wired to J7's 5V bypasses the POWER switch and, with a
  battery fitted, would drive the battery directly: use UART3-IN for a permanent supply.
- The STC8H1K08 helper is connected to I2C through R188/R189 on V1.1. The P4 reads
  its battery bank at address 0x2f on the shared touch bus, GPIO45/46. The earlier claim
  that this interface was undocumented was incorrect: Elecrow's 7-inch factory source
  includes it. See [hardware findings](hardware.md).
- A real empty connector reads approximately 4581 mV and **100% / fully charged** while
  USB-powered. The helper's percentage alone cannot establish that a battery is present.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| PWR-001 | Arm state, bypasses, memory and chime restored after power loss (power-up in previous state) | `CORE-PWR-001..007` |
| PWR-002 | Zone alarms held off during a 60 s settle time after boot | `CORE-PWR-005` |
| PWR-003 | Battery voltage/percentage/charging through the native STC I2C source; `none` fallback. Independent AC-absence sensing requires additional hardware | HIL (when hardware exists) |
| PWR-004 | AC loss -> `ac_loss` trouble (displayed at once, no beep by default); low battery -> `low_battery` trouble | `CORE-TRBL-001` + HIL |
| PWR-005 | With a plausible cell and no confirmed external input: cap backlight at 20%, suppress key clicks/chime, preserve alarm and code feedback | HIL |
| PWR-006 | Task watchdog on the alarm, UI, audio and network tasks; the alarm task has the highest application priority and never blocks on I/O | review + fault injection |
| PWR-007 | Reset reason and boot count recorded in the event log (`system_start` arg) | `test_persistence.c` `test_start_reason_recorded`, `test_notify.c`; bench 2026-10-01 (an update: software; a radio failure: panic) |
| PWR-008 | Subsystem supervision -> troubles: C6 transport (`coprocessor`), sensor radio (`sensor_radio`), storage errors (`storage`), keypads (device supervision), siren circuit (`bell`) | HIL |
| PWR-009 | Brownout detector enabled; NVS writes coalesced to limit wear and power-loss exposure | review |
| PWR-010 | Unknown/failed readings never imply healthy power; qualify known changes for 5 s, restart qualification after polling gaps over 15 s | `test_power_monitor` |
| PWR-011 | Steady battery indicator beside Wi-Fi: red struck-out battery when absent or sensing/percentage unavailable, level icon and valid percentage when present; low battery uses amber | UI preview battery assertions/screenshots |

## Design
- `components/core/power_monitor` is portable C11 with no heap or RTOS. A source returns a
  sample with three-state AC present, battery present and battery low values; voltage and
  percentage use -1 for unavailable. The default `none` source reports everything unknown.
- Known transitions require 5 s of stable samples. Failed reads invalidate immediately;
  gaps over 15 s or a backwards clock restart qualification. Battery readings are suppressed
  unless presence is confirmed; percentage is never estimated from an uncalibrated voltage.
- The existing health task polls the STC every 2 s with bounded I2C transactions. The alarm
  task reads only a cached sample and applies qualification; neither alarm nor UI tasks
  perform bus I/O. One device handle, no new task/stack or periodic allocation. A cached
  sample older than 10 s is unavailable.
- Byte-wise decoding avoids C struct padding. Validate field ranges and the board's 39k/100k
  divider ratio. Only 2000..4350 mV, a valid charge state and 0..100% qualify as a plausible
  cell. These are estimates, not a hardware insertion switch or precision fuel gauge.
  Out-of-range/empty readings remain critical; they never become a fake 100% battery.
- Low battery: <=3500 mV sets low, >=3700 mV restores, with the established state held in
  between and five-second qualification. These thresholds match ordinary one-cell Li-ion
  operation and still require acceptance with the selected battery.
- Charging/full proves external input is present. The helper's NO_CHARGE state cannot prove
  external input is absent, so it does not generate a false AC-loss event. A plausible cell
  without confirmed external power conservatively enables power saving. Read failure retains
  existing savings until external input is confirmed. No deep sleep or shutdown is triggered.
- Backlight cap preserves the requested brightness (including screen-saver darkness) and
  restores it when external input is confirmed. Only key clicks and chime are suppressed;
  alarm, fire, CO, entry/exit, trouble, acknowledgment, errors and walk test remain active.
- `power` prints filtered values, charging state, transport/decode status and the raw register
  bank. Invalid readings remain distinguishable from unavailable communication.
- The UI reads the monitor through its platform backend at the existing one-second status
  cadence. Missing battery or unavailable/invalid sensing shows a steady red crossed-out
  battery with no percentage or question mark. The default `none` source uses this critical
  indication. Unknown and absent remain distinct in the HAL and console; no battery presence
  or percentage is inferred. Valid low-battery measurements retain the amber warning.
- **Verified on a battery-less development panel (2026-09-20):** the native driver reads the
  real helper; for the empty connector external power is yes, battery presence and percentage are
  unavailable (percentage withheld) and the icon is the red crossed-out battery. During 20 s of
  flash-write stress the I2C reads stayed valid with 55.9 frames/s, zero late frames, zero guard
  trips and zero touch I2C errors, with about 280 KB of internal RAM free. Physical sensing,
  battery runtime and power saving with a battery are not validated, and no synthetic battery
  values are exposed.
- **AC-loss report delay** (later): hold the `ac_loss` report for a configurable 10-60 min with
  random jitter; the local display is immediate (docs/features/01 Tasks, BL-089).
- **Health monitor task**: watches heartbeats from the alarm, UI, audio, net and link tasks and
  reboots through the task watchdog if the alarm task stalls. The engine's persisted state makes
  the reboot transparent.
  As built: five heartbeats are watched - the alarm task, the event log's
  writer, the zone hub's task, the LVGL task and the touch poll (docs/features/06 P4-018). The
  `health` task in `panel_app.c` relays them every 2 s, `panel_ota` logs any that goes quiet,
  `health` on the console lists them, and sensor input going quiet raises `sensor_radio`. Audio and network have no heartbeat
  (the panel link does not exist yet), and nothing restarts the panel: no task subscribes to the
  task watchdog (BL-068).

## Open questions
- Independent AC-loss detection needs a separately confirmed input-power signal; the STC
  charge state alone is ambiguous. Do not connect 5 V directly to a P4 GPIO. (BL-038)
- Expected battery runtime target (hours) with the screen dimmed? (BL-038)

## Tasks
- [x] Health monitor (M2): five heartbeats, relayed by the `health` task to `panel_ota`'s watch;
      sensor input going quiet raises `sensor_radio`
- [ ] Task watchdog on the alarm, UI, audio and network tasks (PWR-006, M2): no task subscribes
      to the ESP-IDF task watchdog yet (no `esp_task_wdt_add` call anywhere), so a stalled alarm
      task is reported but does not restart the panel. Decided 2026-09-28: a stalled alarm task
      restarts the panel (the watchdog's panic on), and the reset reason is logged (PWR-007, built)
      (BL-068)
- [x] `power_monitor` HAL + "none" implementation, read-only console, host filtering tests
- [x] Native STC I2C polling, register validation and low-voltage supervision
- [ ] Independent AC-loss signal: a separately confirmed input-power signal, since the STC's
      charge state cannot prove external power is absent (see Open questions) (BL-038)
- [x] Conservative battery power-saving policy, brightness cap and cosmetic-audio suppression
- [ ] Acceptance with a battery fitted: runtime, charging, unplug and replug, the low-battery
      thresholds and the power saving (PWR-003..005); no battery is available yet (BL-038)
- [x] Record the reset reason with system_start (PWR-007) (BL-099, 5153415)
- Covered elsewhere: the AC-loss report delay, docs/features/01 Tasks (BL-089); the siren
  circuit's `bell` trouble (PWR-008) with the output hardware, docs/features/16 (BL-037);
  keypad supervision (PWR-008) with the panel link, docs/features/11 (BL-048); the `storage`
  trouble's flash fault injection, docs/features/06 P4-013 (BL-032).
