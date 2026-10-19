# 21 · Testing and CI

**Status:** Living document - the host tests (with sanitizers in CI), the UI preview, the
panel simulator (`host/ui_sim`) and the panel link's simulator (`host/link_sim`, its `link_smoke`
in CTest) run on the PC; CI adds the architecture, board, backlog and UI-contract checks and the
P4 firmware build with its size check; panels are checked through the service console and
`panelctl`. Left: on-target test apps (dropped), hardware checklists as issue templates (BL-077).
· **Milestone:** all · **Depends on:** all

## Goal
Prove the alarm behaves correctly before it goes on a wall. Test on the PC first, then on
hardware with repeatable checklists, and keep every requirement traceable to a test.

## Test layers

| Layer | What | Where | Status |
|---|---|---|---|
| Host unit/scenario | `alarm_core`: a suite per area (arming, exit/entry, alarms, life safety, users, troubles, keystrokes, persistence, partitions, config, installer mode, configuration blob) | `components/core/alarm_core/test`, Unity v2.6.1 | **done** |
| Host unit (zone sources) | `zone_hub` (registry, bindings, every mapping mode, remapping when a contact or threshold changes, bindings that couldn't be read, grace and CHECK per zone, source links, missing sources, resync after a configuration change with the real engine, the binding blob with a frozen fixture and newer-firmware items, the simulator end to end, the wired source on fake pins with response times and learning, the debouncer); the 433 MHz, Zigbee and Z-Wave sources in their own files | `components/core/zone_hub/test/test_zone_hub.c`, `test_rf.c`, `test_zigbee.c`, `test_zsrc_zwave.c` | **done** (M11) |
| Host unit (flash) | `event_log`: on a simulated NOR flash, including a power cut after every byte written or erased across a sector wrap (forward and backward reads compared after each) | `components/core/event_log/test/test_elog.c` | **done** (M4, M5) |
| Host unit (MQTT) | `alarm_proto`: the JSON writer and strict reader with hostile input, golden payloads, commands, discovery, payload sizes, and store-and-forward on the real event log | `components/core/alarm_proto/test/test_proto.c`, `test_report.c` | **done** (M5) |
| Host unit (notifications) | `notify`: the words for every event type (a golden table, so a reword is a visible diff), silent events, troubles and their restores, the shared categories, and every flood rule: the bucket, the daily cap, eight zones in one burglary collapsing to two notifications, trouble collapse by identity, and a clock that steps backwards | `components/core/notify/test/test_notify.c` | **done** (M15) |
| Host unit (sounder) | `sounder`: all by *listening* - the mixer is rendered to PCM and the beeps and gaps are measured off it: temporal-3 and temporal-4 to the millisecond and to the tone, every continuous pattern, a new pattern starting from its beginning while a repeated one does not stutter, alarms ignoring the volume while everything else follows it, one-shots dropped under an alarm and pausing a quiet pattern which then resumes where it stood, the silent first block that keeps the amplifier from popping, and power saving dropping key clicks and the chime but never an alarm | `components/core/sounder/test/test_sounder.c` | **done** (M2, split out 2026-09-21) |
| Host unit (updates) | `alarm_proto` firmware helpers: semantic versions and the upgrade rule, manifest pick and URL resolution, install policy, base64 and upload lines with CRC, the HA `update` state and discovery | `components/core/alarm_proto/test/test_fw.c` | **done** (M6) |
| Host unit (the other components) | Every other component's own `test/` folder, found by `host/CMakeLists.txt`: among them 433 MHz decoding (`rf_decode`) and the CC1101 (`cc1101_regs`), power (`power_monitor`), home control, storage, the clock (`rtc_clock`), Zigbee (`zigbee_host`), Z-Wave (`zwave_proto`, `zwave_host`, `zwave_zones`), the Home Assistant link (`alarm_proto` `test_link.c`, `test_pair.c`) and the boards' descriptions. The Panel Manager's pages run under Node (`components/services/panel_web/test`), the tools under Python (`tools/tests/test_*.py`) | `components/<tier>/<name>/test`, `tools/tests` | in place, grows with each component |
| Host unit (the panel link) | its crypto (RFC 3610, 4231, 5869, 7748 and vectors made with Python `cryptography`), frames, messages, pairing, sessions, the master and the keypads together over a pretend network with loss and a fake clock | `components/core/panel_link/test/` | **done** (BL-048); schedules come with BL-046 |
| PC simulator | The panel link's simulator (not `host/ui_sim`): `link_sim master` (the real engine and the master's side of the link) and `link_sim keypad` (the keypad's side and the status it rebuilds) over UDP with a console each, with the OS's random numbers so they pair with panels on the LAN too; `link_smoke`, the two in one process over 127.0.0.1 on a virtual clock, runs in CTest (LINK-013) | `host/link_sim`, `tools/link_sim.ps1` | **done** (M7, BL-048) |
| On-target unit | BSP drivers, NVS store (ESP-IDF Unity test apps); the audio patterns are host-tested instead, so only the I2S path and the amplifier are left for the board | `firmware/*/test` | dropped 2026-09-28: the console and `panelctl` checks cover the board |
| Hardware-in-the-loop | Checklists per feature (display, touch, audio, Wi-Fi, C6, MQTT, OTA, link). Tools: `tools/panel_console.py` (types console commands without resetting the panel, captures boot logs with `--reset`, stops the display with `--hold-reset`), `tools/ota_serial.py` (firmware upload, `--tamper`), `display [stress <s>]` (refresh monitor: frames/s, late frames, UI redraws, refresh times, touch polls, flicker guard) and `display watch <s>` (a line a second while someone uses the screen); for enrolled development panels `tools/panelctl.py` and the explicit, opt-in scripts `tools/tests/hardware_*.py` and `panel_web_*hil.py` ([30](../30-Remote-Development/README.md), [31](../31-Browser-Panel-Manager/README.md)). The checks still owed on a panel are docs/BACKLOG.md section 3 | feature READMEs ([06](../06-CrowPanel-P4-Platform/README.md), [13](../13-OTA-Updates/README.md)) | **started** 2026-09-19 (boot, display refresh, C6, OTA on the CrowPanel) |
| End-to-end | Mosquitto (TLS) + Home Assistant test instance + panel | integration rig | planned (M5): [10](../10-MQTT-and-Home-Assistant/README.md) and [23](../23-Home-Assistant-Integration/README.md) Tasks (BL-026, BL-027) |
| UI by hand, without a panel | `host/ui_sim` (`pwsh tools/ui_sim.ps1`): the same engine, event log and UI in a window, the panel's 1024x600 screen on the left and buttons on the right for the states worth looking at - arming, a door opening, a sensor that stops answering, mains power lost, and each way the house network can break (Wi-Fi away, no DHCP lease, router silent, name lookups, internet, time server, connected service). **It plays the panel's sounder out of the PC's speakers** (the same `components/core/sounder` mixer the board renders, at half the panel's level so a full-scale alarm is bearable through headphones), so the exit beeps, the chime, the trouble chirps and the temporal-3 and temporal-4 patterns can be heard and told apart without a panel; "Mute sound" stops the speakers while the panel goes on sounding underneath, and the status line names the pattern the engine is asking for. Click the screen as you would touch it; "Save screenshot" writes the panel's own 1024x600 frame. Windows only: it uses LVGL's Win32 backend, so there is nothing to install. **This is where a change is tried first** - never on a panel that is protecting a house | `host/ui_sim` | **done** (2026-09-20) |
| UI regression | `host/ui_preview`: the real engine, the real event log (RAM flash), the exported UI and its logic on a headless LVGL display, driven through every screen (the evening of M3, Settings, users, Wi-Fi, the address settings and the connection check, time, Integrations and Home Assistant, installer mode, the setup, a power-up recovery, the software update states, the screen saver armed, the lockdown while armed, Instant arming and the zone sources) with a virtual touchscreen, plus checks that fail the run: the setup walked with Wi-Fi skipped and a time zone picked (no button ignored), the screen saver waiting its full time after the exit time, Settings and Chime staying shut while armed (remote arming closes Settings and forgets the code), and a wired zone added through the terminal picker that follows its contact while a sensor that stops answering turns its zone CHECK, and the connection check run against a broken house network (wrong router, name lookups down, internet down, no DHCP lease, Wi-Fi gone) with what it must say about each; PNGs uploaded by CI; `tools/lvpro_gen.py --check` keeps the exported C in step with the XML | `host/ui_preview`, CI job `ui-preview` | **done** (M3-M6, M11) |

## Scenario harness
`components/core/alarm_core/test/sim.h` wraps one engine with a fake clock and recording ports, and provides a
standard house layout (20 zones covering every zone type and 7 users covering every authority).
Tests read like the requirements:

```c
sim_arm_away_and_leave(&S, CODE_USER);   // arm, walk out through the front door
sim_fault(&S, Z_FRONT);                  // come back in
sim_advance(&S, SEC(30));                // entry time runs out
TEST_ASSERT_TRUE(sim_alarm_logged(&S, Z_FRONT, ALARM_CAUSE_BURGLARY));
sim_advance(&S, SEC(30));                // abort window
TEST_ASSERT_TRUE(sim_alarm_reported(&S, Z_FRONT, ALARM_CAUSE_BURGLARY));
```

`sim_advance` ticks at every engine deadline and fails the test if a deadline doesn't advance
(catches timer bugs).

## Display safety checks
A panel hangs on a wall and is looked at in the dark, by whoever walks past, so a display that
strobes is a defect, not a cosmetic bug (UI-020, P4-012, WCAG 2.3.1). These tools measure the
refresh from the numbers. They are optional, for investigating a suspected refresh problem; a
normal flash does not need them (CONTRIBUTING.md "Display safety"):
1. Build the dark image and flash it: `pwsh tools/idf.ps1 -Dark build` (POSIX
   `tools/idf.sh --dark build`) adds `sdkconfig.defaults.dark` (`CONFIG_BOARD_BACKLIGHT_FORCE_OFF=y`)
   in its own `_dark` build folder and sdkconfig, so `sdkconfig.defaults` is not touched.
2. On the console (`python tools/panel_console.py -p <PORT> --gap 25 display "display stress 20"`):
   `display` (expect about 56 frames/s, 0 late, longest gap about 17.9 ms) and `display stress 20`
   (flash writes with full-screen redraws: 0 late frames, UI redraws counted).
3. Flash the normal image again when done (its build folder is left as it was).

## Rules
1. Every requirement ID `CORE-XXX-NNN` in [01](../01-Core-Alarm-Engine/README.md) (and
   `LOG-NNN` in [05](../05-Event-Log/README.md), `AUD-NNN` in [08](../08-Audio-and-Annunciation/README.md),
   `MQTT-NNN` in [10](../10-MQTT-and-Home-Assistant/README.md),
   `OTA-NNN` in [13](../13-OTA-Updates/README.md) where host-testable) is referenced in a comment
   above at least one test; the other components' IDs (`ZSRC-NNN`, `RF-NNN`, `NOTE-NNN`, ...)
   are tagged in their own `test/` folders. New behaviour = new ID + new test in the same change.
2. The host build is warning-free with `-Wall -Wextra -Wpedantic -Wconversion -Werror`.
3. CI runs the host tests with AddressSanitizer + UBSan on Linux, applied to every portable
   component in `components/core` itself, not just the harnesses that link them (each library
   target is instrumented explicitly by `alarm_add_core_components()` in
   `cmake/alarm_host.cmake`, since directory-wide sanitizer flags added after their
   `add_subdirectory()` calls would otherwise miss them). A CI step reads the compile commands
   and fails if one of the library sources it samples was built without them (R19).
4. Mutation spot-checks when touching delicate logic: break the rule on purpose and make sure a
   test fails. Checked on 2026-09-18: disabling auto-stay, the abort window, and the
   guest/installer disarm restriction each fail the expected tests. Checked on 2026-09-21 for
   the sounder: shortening the temporal-3 gap, letting an alarm follow the user's volume,
   dropping the silent block before the amplifier, and unwiring either the engine's one-shots
   or the continuous pattern from the keypad are each caught (the first three by the `sounder`
   suite, the last two by the preview).

## Running
- The panel in a window, to try a change by hand: `pwsh tools/ui_sim.ps1` (Windows).
- The panel link on a PC: `pwsh tools/link_sim.ps1 master` and `pwsh tools/link_sim.ps1 keypad
  --master 127.0.0.1` in two terminals (Linux/macOS: host/README.md); `-Smoke` runs `link_smoke`,
  which CTest runs with the host tests.
- Windows: `pwsh tools/host_test.ps1` (uses gcc/cmake/ninja from PATH or CLion's bundled toolchain).
- Linux/macOS: `cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host --output-on-failure`
- CLion: open `host/` as a CMake project; each suite is a run target.
- CI: `.github/workflows/host-tests.yml` (push / pull request): job `alarm-core` builds and runs
  the host tests with sanitizers (and checks that the libraries themselves were instrumented),
  then runs `tools/check_architecture.py`, `tools/check_boards.py`, `tools/check_backlog.py`
  (every open box in docs/features has its docs/BACKLOG.md row) and the tool tests
  (`tools/tests`). Job `ui-preview` checks the 7" layout's generated code (`lvpro_gen.py
  --check`), runs `tools/check_ui_contract.py` (docs/features/19 CYD-007), `embed_image.py
  --check` and `embed_logo.py --check` and the Panel Manager's Node tests
  (`components/services/panel_web/test`), then builds and runs the 7"'s UI preview, its smoke
  test without the optional pages and the Z-Wave pages' walk (docs/features/36), and uploads
  the screenshots.
  **The 4" E32R40T is left out** (docs/features/19 is on hold):
  its layout check, its preview and smoke test and its firmware build are off in CI, and
  `check_ui_contract.py` skips layouts in its `ON_HOLD` list; each workflow comment says how to
  put it back.
- CI: `.github/workflows/firmware.yml` (push / pull request, docs/features/33 step 10): builds every
  board's firmware in the `espressif/idf:v6.1` container (the P4 v1 and v3; the E32R40T is left
  out while docs/features/19 is on hold),
  signed with a key made for the run (CI images are never for a panel), then
  `tools/check_image_size.py <build> --ram` fails an image that fills more than 85 % of its OTA
  slot and writes the image size and static RAM to the job summary (1633c0b).

## Tasks
- [x] B001 bench diagnostics: `crypto_test 100` (`CONFIG_ALARM_CRYPTO_DIAGNOSTICS=y`) and
      `ui_memory_test 5` (with `CONFIG_BOARD_BACKLIGHT_FORCE_OFF=y`), absent from normal builds
- [x] Unity + scenario harness + 10 core suites
- [x] CI workflow (host tests, sanitizers, generated UI code and images, UI preview)
- [x] ESP-IDF build job with `espressif/idf:v6.1` container: `firmware.yml`, every finished
      board; the E32R40T is left out while docs/features/19 is on hold (BL-079, 1633c0b)
- [x] Firmware size report per image: `tools/check_image_size.py` (16 tests), the job summary
- [ ] Hardware checklists as issue templates: `.github/ISSUE_TEMPLATE` holds only the bug report
      and feature request forms today (M2) (BL-077)
- [x] The panel link's PC simulator, `host/link_sim` (the "PC simulator" layer above): the
      engine with a virtual keypad and master over LAN UDP, `link_smoke` in CTest; `host/ui_sim`
      is the touchscreen's simulator, a different thing (BL-048, 317253d, 7c9e3d6)
- Dropped 2026-09-28: on-target unit test apps (`firmware/*/test`, the "On-target unit"
  layer above) - the service console and `panelctl` hardware checks cover the board, and the
  drivers' logic is host-tested
- Covered elsewhere: the end-to-end rig with Mosquitto and a Home Assistant test instance,
  docs/features/10 and /23 Tasks (BL-026, BL-027); schedules' host tests (BL-046) come with
  that feature.
