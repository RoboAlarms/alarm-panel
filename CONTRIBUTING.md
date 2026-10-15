# Contributing

External contributions are not being accepted at this time.

This guide is for the maintainers and for anyone building, testing or changing a copy of their
own. RoboAlarms is an alarm panel that people put on their walls, so the bar is "works, is tested
and doesn't surprise anyone": a change comes with its tests, and anything a person sees or hears
is tried in the PC simulator before it goes near a panel.

- **Bugs and ideas:** open an issue. For a panel problem, give the board, its P4 silicon
  revision (the boot log prints it), the firmware version (Settings > About) and what the
  screen or the console said.
- **Security problems:** not in a public issue; see [SECURITY.md](SECURITY.md).
- **Sensor captures** (`rf capture` on the service console) can go with a bug report; remove
  anything that identifies your home first.
- **Changes** (the maintainers' pull requests): small and focused, with the checks below passing.
  When behaviour changes, update the feature's document in `docs/features/<NN-Name>/README.md`
  (status line, requirements, design, tasks) and its row in [`docs/BACKLOG.md`](docs/BACKLOG.md)
  in the same pull request.
- **The rules** for the code, the screen and real panels are in [AGENTS.md](AGENTS.md) and the
  files it names, written for people and AI agents alike ("The rules" below). Any coding agent
  reads AGENTS.md by itself; Claude Code reads it through [CLAUDE.md](CLAUDE.md).

## Getting started
1. **No panel needed to begin.** The host tests and the PC simulator run the real alarm engine and
   UI: `pwsh tools/host_test.ps1` (Windows) or the cmake line in "Build and test" (Linux/macOS),
   then `pwsh tools/ui_sim.ps1` (Windows) to use the panel in a window. You need CMake 3.20 or
   later, Ninja, a C11 GCC and git; the first configure downloads LVGL and micro-opus.
2. **Pick something.** `python tools/backlog.py list --pc` lists the [backlog](docs/BACKLOG.md)
   rows a PC is enough for; rows marked `PC (good first)` are small and self-contained.
   `python tools/backlog.py show BL-NNN` shows a row with its open boxes and the feature document
   that has the details.
3. **Work on a branch** `bl-NNN-short-name` and open a draft pull request early: its description
   (the template) says what is done, what was checked and what is left.
4. **A panel** (an Elecrow CrowPanel Advanced 7"; the boot log prints its P4 silicon revision): back
   up its factory flash first, then build and flash it ("Set up a panel" below). The firmware needs
   ESP-IDF v6.1.

## Where things are
An open-source alarm panel (classic keypad alarm behaviour, SIA CP-01), first on the
Elecrow CrowPanel Advanced 7" (ESP32-P4 + ESP32-C6, board rev V1.1), next on cheap ESP32
screens, starting with the LCDWiki E32R40T 4" (docs/features/19). Portable C11 components hold the
alarm logic and are tested on the PC; the ESP-IDF firmware wraps them.

Components sit in tiers under `components/` with one-way dependencies (ADR-034): `core`
(portable C11, no ESP-IDF), `drivers` (chips, no board knowledge), `boards` (one per board,
behind the `board_api` contract), `services` (ESP-IDF, board-agnostic, feature-gated), `ui`
(portable LVGL logic) and `app` (`panel_app`, the start-up every board shares; a firmware's
`app_main.c` only fills its hooks). Every
component has the same shape: public headers in `include/`, sources and private headers in
`src/`, host tests in `test/`, a `README.md`; `components/README.md` indexes
every component (the table below lists the ones most tasks touch). The rules, the board model,
the feature options (the shared "Alarm features" menu is `components/services/alarm_features`)
and the restructure's progress are in docs/features/33; read it before adding a component, a board
or a feature option. `python tools/check_architecture.py` checks the rules (CI runs it).

| Path | What |
|---|---|
| `components/core/alarm_core` | the alarm engine: portable C11, no heap, host-tested (docs/features/01) |
| `components/core/event_log` | power-fail-safe event log ring on raw flash, portable (docs/features/05) |
| `components/core/alarm_proto` | MQTT JSON, Home Assistant discovery, store-and-forward, firmware-update helpers; portable (docs/features/10, /13) |
| `components/core/zone_hub` | zone sources: the source interface, the hub (bindings, mapping, grace and CHECK, the binding blob), the simulator, wired, Home Assistant, Zigbee, 433 MHz (`zsrc_rf`) and Z-Wave (`zsrc_zwave`, fed by `zwave_zones`, docs/features/36) sources' logic; portable (docs/features/22) |
| `components/core/rf_decode` | pulse-train decoders for one-way RF sensors (EV1527 / PT2262, HT6P20B, HT12E, KeeLoq, arctech) with a de-glitch filter and strict framing, and the encoders the tests use; portable (docs/features/25) |
| `components/core/rtc_clock` | the DS3231 battery-backed clock as data and the rules for which clock the panel believes; portable. `components/drivers/rtc_ds3231` does its I2C, `components/services/panel_clock` runs it (docs/features/35) |
| `components/core/cc1101_regs` | the CC1101 as data: receive table per band, frequency word, RSSI, identification, read-back and reset detection; portable (docs/features/25 RF-017) |
| `components/drivers/rf_rx` | the CC1101 receiver over SPI and the RMT capture of a receiver's data line (a CC1101 or a fixed 433 MHz module) (docs/features/25); `rf_service/src/rf_service.c` runs them, picks the receiver and hosts the `rf` console command |
| `components/core/notify` | the event sentences (the single copy: the Activity list and a phone notification both read them), categories, priorities; portable (docs/features/26) |
| `components/core/i18n` | languages: the catalog lookup, CLDR plural rules, the `{n}` formatter, UTF-8 helpers, regions, dates and numbers; the translation template and catalogs in `po/`, generated tables in `catalogs/`; `tools/i18n.py` extracts, checks, merges and builds them and makes the language packs (`pack`); `i18n_pack_open()` reads a pack; portable (docs/features/37) |
| `components/core/pack_store`, `components/services/lang_packs` | language packs (docs/features/37 [language-packs.md](docs/features/37-Globalization/language-packs.md), ADR-045): the power-safe store of signed blobs in the `assets` partition (portable, its Python twin is `tools/flasher/roboflash/packstore.py`) and the service that checks, loads, installs, removes and downloads packs (`lang` on the console) |
| `components/core/sounder` | the keypad sounder: the patterns, the synthesizer and the mixer - the single copy of what the panel sounds like (the board, the PC simulator and the tests render it); portable (docs/features/08) |
| `components/ui/alarm_ui` | touch UI logic in `src/` (portable, LVGL), pictures in `images/`; its ESP backend is `components/services/ui_port_esp` (docs/features/07) |
| `components/ui/alarm_ui/layouts/<WxH>/alarm_ui` | one LVGL Pro project per screen size (`1024x600` the 7", `480x320` the 4"): XML is the source, `*_gen.c/.h` are generated; `components/ui/alarm_ui/layouts/README.md` has the contract between them |
| `components/boards/crowpanel_p4_family` | the CrowPanel P4 boards' shared BSP: MIPI-DSI display with refresh monitor and flicker guard, polled GT911 touch, backlight, audio, battery (docs/features/06) |
| `components/boards/board_crowpanel_p4`, `board_crowpanel_10` | the 7" V1.1 and the 10.1" V1.2 on that family library: each board's description, wiring and names |
| `components/boards/board_e32r40t` | the 4" LCDWiki E32R40T (classic ESP32, no PSRAM): its description, the ST7796S display and XPT2046 touch with its calibration, a bring-up console (`lcd`, `touch`); audio and battery drivers are docs/features/19 Phase 2 |
| `components/services/platform_esp` | alarm task, NVS store, event log writer, the sounder's task and amplifier, Wi-Fi/addresses/time, the connection check, zone service (the hub's task, NVS bindings, GPIO for wired zones), service console |
| `components/services/net_mqtt` | MQTT client (TLS, availability, replay, discovery, commands) |
| `components/services/panel_ota` | firmware updates: signed images, rollback, release check, serial upload, C6 updates (docs/features/13) |
| `components/services/panel_web` (its pages in `web/`) | panel-hosted HTTPS, browser configuration, remote keypad and runtime access policy (docs/features/31): one access table per API path in `panel_web.c` mirroring docs/features/14, the remote keypad's WebSocket and H.264/JPEG stream in `web_screen.c` (the P4 side in `firmware/crowpanel_p4/main/dev_adapter.c`); production is `panel.html/.css/.js`, `panel-pages.js`, `panel-keypad.js`, `panel-words.js` (the words, docs/features/37); try UI changes with `mock-panel.cjs` + `mock-screen.cjs` (see its README) |
| `components/services/dev_remote`, `tools/panelctl.py` | enrolled workstation transport, signed network/USB development tools and hardware adapter contract (docs/features/30, /31) |
| `tools/voice` | the voice library (docs/features/08 voice-library.md, ADR-052): `voice.py` makes the engines' Python environments under `build/voice/`, auditions voices (`audition.json`), records the whole script in the chosen voice (`library.json`) with local text-to-speech models (downloaded on first use, never committed) and encodes it to Opus or LC3 to hear and measure; `export`, `verify` and `pack` keep the chosen takes in `assets/voice/` and build the voice pack from them with the standard library alone (no model, no encoder); tests `tools/tests/test_voice_tools.py` |
| `tools/flasher` | the RoboAlarms Flasher (docs/features/34): desktop app for people who don't build firmware - port list, board detection with override, backup, install/update from a `.roboflash` bundle, console, 433 MHz page (`rf pin`); `run.py --mock` for pretend boards, `build.ps1`/`build.sh` for the one-file program, tests `tools/tests/test_flasher_*.py` |
| `firmware/crowpanel_p4` | the application: `main/app_main.c`, `partitions.csv`, `sdkconfig.defaults*` |
| `firmware/crowpanel_10` | the 10.1"'s: built on `crowpanel_p4` (`ALARM_BOARD_BASE`), with only what differs (`firmware/README.md`) |
| `firmware/e32r40t` | the 4" board's application: the status screen until the full touch UI fits in its RAM (docs/features/19); runs on a development board |
| `components/<tier>/<name>/test` | that component's host tests (Unity), found by `host/CMakeLists.txt`; `components/core/alarm_core/test/sim.c` is the engine harness other tests share |
| `cmake/` | shared CMake: `alarm_firmware.cmake` (a firmware project from `ALARM_BOARD`: version, signed name, profile check) and `alarm_idf.cmake` (the tiers for ESP-IDF, only the project's own board), `alarm_host.cmake` (portable components, warnings, `alarm_add_test`), `alarm_lvgl.cmake`, `alarm_ui.cmake`, `alarm_board.cmake` (a board's manifest for the PC builds) |
| `host/` | the PC test runner (`host/CMakeLists.txt`, Unity in `host/third_party`), the scripted PC preview of the UI (`host/ui_preview`) and the interactive panel simulator (`host/ui_sim`) |
| `tools/` | `idf.ps1` / `idf.sh` (Windows / POSIX, keep in step), `host_test.ps1`, `ui_sim.ps1`, `ui_preview.ps1`, `ui_mockup.py` (the touchscreen mockup), `lvpro_gen.py`, `i18n.py` (the translation catalogs), `embed_image.py`, `ota_serial.py`, `panel_console.py`, `stamp_app_desc.py` (the firmware build runs it after each link: the image's own build date), the checks `check_architecture.py`, `check_boards.py`, `check_image_size.py` (tests in `tools/tests`) |
| `docs/` | all documentation, indexed in `docs/README.md`: `features/`, `bugs/`, `sensors/` (how real sensors behave), `research/`, `hardware/`. The user and installer guides are their own repository, [RoboAlarms/documentation](https://github.com/RoboAlarms/documentation), published at <https://docs.roboalarms.com> |
| `docs/features/` | one planning doc per feature, index and roadmap in `docs/features/README.md` |
| `docs/BACKLOG.md` | everything left to do, in order, each row with what it needs (the PC, a panel, a part, a decision) and where its details are |
| `docs/hardware/` | vendor documentation saved locally and git-ignored (its README lists the links): read it, never edit it; a fresh clone has only the README |

## Build and test
- Host tests (Windows): `pwsh tools/host_test.ps1` (finds CLion's bundled gcc/cmake/ninja).
- Host tests (Linux/macOS): `cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host --output-on-failure`
- Firmware: ESP-IDF **v6.1** (EIM install). Windows: `pwsh tools/idf.ps1 build` (P4 silicon
  v1.x) and `pwsh tools/idf.ps1 -ChipRev v3 build`, **from PowerShell** (`idf.py` refuses Git
  Bash / MSYS); the script sets `IDF_COMPONENT_CACHE_PATH=C:\Espressif\cm` (Windows path-length
  limit). Linux/macOS: `tools/idf.sh build`, `tools/idf.sh --chip-rev v3 build`,
  `tools/idf.sh -p /dev/ttyACM0 flash`. The two wrappers must stay in step: same options, same
  build folders (`build_v1`/`build_v3`), same `sdkconfig.v1`/`.v3` and the same
  `SDKCONFIG_DEFAULTS` (v1 adds `sdkconfig.defaults.p4rev1`) - change one, change the other.
  Other boards: `-Project <board>` / `--project <board>` (`e32r40t`), built in `build`,
  `build_<profile>` with `sdkconfig`, `sdkconfig.<profile>`; `-ChipRev` is the P4's alone, and
  `-Project crowpanel_10` (built on `crowpanel_p4`) builds per silicon like the 7".
  After editing `sdkconfig.defaults*`, delete `sdkconfig.v1`/`.v3` (another board: its
  `sdkconfig*`); a full rebuild follows.
  After adding a component or a `Kconfig` file, run `pwsh tools/idf.ps1 [same options]
  reconfigure` once: an incremental build doesn't see a new `Kconfig`, so its options read as
  off and their code silently compiles out (it happened with `panel_storage`'s SD drivers;
  the image-size comparison caught it).
- Builds are signed with `secrets/ota_signing_key.pem` (created by either wrapper, git-ignored).
  Never commit it, never replace it: the panel only accepts updates signed with it. A panel
  installs an update only when it carries the running image's key, or the one key its installer
  accepted for an update server they chose (docs/features/13 OTA-020). The official releases on
  `firmware.roboalarms.com`, every build's default update server, carry the project's release
  key: a panel running your build checks that server but doesn't install from it, and a panel
  running an official release takes your build only over USB. To build without the official
  server, set `CONFIG_PANEL_OTA_DEFAULT_DOMAIN` to your own domain, or to empty for no automatic
  checks. The Flasher downloads only images signed with the release key; it installs a bundle of
  your own from a file (Advanced tools > Software). The version
  is the `VERSION` file; `-Version x.y.z-test` builds a test image in its own folder. A bench
  fault image, to prove the update self-test catches a broken part, sets `ALARM_BENCH_FAULT`
  (`no_zone_service`, `no_touch`, `zones_stop`) with a `-Version x.y.z-fault.<name>`; the build
  refuses either without the other (docs/features/13 "Still to run on the bench"). Never install
  one on a panel that protects a house.
- Remote development (docs/features/30): `pwsh tools/idf.ps1 -Profile dev build` / POSIX
  `tools/idf.sh --profile dev build`; `-Dark` / `--dark` builds a backlight-off image (optional, AGENTS.md "Display safety"). Profiles
  have isolated `build_v1_dev`, `build_v1_release`, etc. and matching sdkconfig files. Default
  legacy/dev/release builds now include Panel Manager and developer resources (docs/features/31).
  Local Installer controls persistent Web server / Developer switches, both off on fresh panels.
  Use explicit `-Profile release` for releases and run `tools/check_release_profile.py` before
  packaging. Release excludes the physical service console and diagnostic injection, while
  preserving certificates/enrollment through updates. Signed profile matching still prevents
  accidental network profile transitions. Hardened eFuses remain separate.
- **Panel simulator (PC): `pwsh tools/ui_sim.ps1`** - the real engine, event log and UI in a
  window, the panel's screen on the left and buttons on the right (arming, a door opening, a
  sensor that stops answering, mains power lost, and each way the house network can break).
  **It plays the sounder out of the PC's speakers**, the same mixer the panel renders, at half the
  panel's level ("Mute sound" stops the speakers; the status line names the pattern), so exit
  beeps, the chime, trouble chirps and temporal-3 against temporal-4 can be heard without a panel.
  **Try a UI or behaviour change here before flashing a panel**: a panel on a wall is
  protecting a house. `-Board <board>` (both scripts) builds another board's profile from its
  manifest (`firmware/<board>/sdkconfig.defaults`): layout, pages and LVGL memory. Windows only (LVGL's Win32 backend); `host/ui_sim/sim_main.c` holds the
  buttons, `host/ui_preview/host_panel.c` the simulated world they drive.
- UI preview (PC): `pwsh tools/ui_preview.ps1` renders every screen to `build/ui_preview/shots`
  and fails if the scripted walk-through gets stuck or a check reports the wrong thing. Look at
  the PNGs after UI changes; adding a `shot()` renumbers the later ones, so refresh the whole
  of `docs/features/07-UI-LVGL-Pro/screens/` and the `NN_name.png` references in the docs with it.
  `-ZWave` (both `ui_preview.ps1` and `ui_sim.ps1`) adds the Z-Wave world and pages
  (docs/features/36) in their own build folders; the preview appends its Z-Wave shots after the
  others, which stay unchanged. Every walk fails when a label's text is cut off or runs out of
  the button or card around it, or a text is longer than a screen text holds (docs/features/37
  I18N-022): rows and cards grow with their words, and only a label meant to shorten ends in
  "...". `-Locale en-XA` (about 40 % longer, accented) and `-Locale ar-XB` (right to left), for
  both scripts, show the screens in a test language (I18N-024); `-Locale en-XA -Live` starts in
  English, changes language while running and fails unless every picture matches the walk that
  started in it (I18N-014: run the plain `-Locale` walk first, it is the reference).
  `-Locale es-US` shows them in Spanish and `-Locale en-GB` in British English, from the
  committed catalogs (drafts until a native speaker reviews them). The English walk builds without draft languages, as a release firmware
  does (I18N-005), so a draft adds no language step to its setup; the simulator offers drafts in
  Settings, as a development firmware does.
- UI mockup: `python tools/ui_mockup.py build --shots` builds the touchscreen's HTML mockup
  (`build/mockups/panel/index.html`, beside the preview's pictures), `check` checks it (CI) and
  `compare` scores each screen against its picture. A change a person sees starts there as a
  proposal (`docs/features/07-UI-LVGL-Pro/mockup/README.md`, UI-044).
- Panel Manager mockup: `node docs/features/31-Browser-Panel-Manager/mockup/build.cjs` builds
  `build/mockups/manager/index.html`, the real pages with their pretend panel in the page (any role,
  state or proposal); a change to the pages starts there as a
  proposal (its README, WEB-026). `node --test components/services/panel_web/test/manager_mockup.test.cjs` checks it (CI).
- CI (`.github/workflows/host-tests.yml`) runs the host tests with sanitizers,
  `lvpro_gen.py --check` for the 7" layout, `check_ui_contract.py`, `embed_image.py --check`,
  `i18n.py extract --check`, `check` and `build --check` (docs/features/37), and the 7"'s UI
  preview in English (then `ui_mockup.py check`), in `en-XA` and changed to `en-XA` while running. Run the same locally.
  `.github/workflows/firmware.yml` builds the P4 v1 and v3 firmware signed with a throwaway key
  and runs `tools/check_image_size.py <build> --ram`: an image may fill at most 85 % of its OTA
  slot (docs/features/19 CYD-013).
- **The 4" E32R40T is on hold (docs/features/19 blocked, 2026-09-24):** CI does not build its
  firmware, check its layout or run its preview; `check_ui_contract.py` skips `480x320`
  (`ON_HOLD`, `--all` checks it) and the Flasher does not offer the board (`"offered": false` in
  `tools/flasher/roboflash/boards.json`). Its image no longer builds (2026-10-05): the 4" layout
  fell behind the 7"'s UI (rollers, the calendar and clock pickers); bring it up to date when the
  work resumes, then undo those
  four exclusions (the comments in the workflows say how). Linux GCC in CI warns about things
  the local MinGW GCC does not (`-Wformat-truncation`, `-Wsign-conversion` on promoted bytes):
  cast bytes to `unsigned` before shifting and size snprintf buffers for the worst case.
- If `python` isn't on PATH, the ESP-IDF venv has one with pyserial (Windows/EIM:
  `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`). Pillow is usually not installed.
- The wrappers only find and activate ESP-IDF v6.1, create the signing key on the first build and
  pass the build folder, `sdkconfig` and defaults for the silicon revision to `idf.py`;
  `tools/idf.sh --help` shows what it runs, for anyone who would rather call `idf.py` directly.
- The touchscreen runs on LVGL 9. Its layouts (`components/ui/alarm_ui/layouts/<WxH>/alarm_ui`)
  are XML in LVGL Pro's project format: edit the XML and regenerate the C with
  `python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui`. No LVGL Pro
  licence is needed; the LVGL Pro Editor can open a layout if you have it.

## Set up a panel
To install the official firmware without a build environment, use the
[Flasher](https://docs.roboalarms.com/installer/flasher/). This is the developer's route, for a
CrowPanel Advanced 7" just out of its box. Steps 1 and 2 are the ones you can't undo later.
`COM4` is an example: use your board's port (`/dev/ttyACM0` and the like on Linux and macOS).

1. **Install the tools:** [ESP-IDF v6.1](https://dl.espressif.com/dl/esp-idf/), through
   Espressif's Installation Manager (EIM) or a manual install, and for the PC builds GCC, CMake
   and Ninja on PATH (CLion's bundled toolchain is found by itself). `tools/idf.ps1` reads the
   default `C:\Espressif` EIM layout (set `IDF_EIM_JSON` if yours is elsewhere); `tools/idf.sh`
   takes an activated shell, `$IDF_PATH`, an EIM installation or `~/esp/esp-idf`. Run the host
   tests and the preview ("Build and test") before you touch the board.
2. **Back up the factory flash before you write anything.** The board ships with Elecrow's demo
   and there is no download of it: this backup is the only way back. `esptool` comes with
   ESP-IDF:
   ```powershell
   $py = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
   mkdir build\device_backup
   & $py -m esptool --chip esp32p4 -p COM4 -b 921600 read-flash 0 0x1000000 build\device_backup\crowpanel_factory_16MB.bin
   ```
   That is all 16 MB and takes a few minutes. Keep the file somewhere safe (git ignores `build/`).
   Restore it with `& $py -m esptool --chip esp32p4 -p COM4 write-flash 0 build\device_backup\crowpanel_factory_16MB.bin`.
3. **Find the P4 silicon revision:** watch the board start (`pwsh tools/idf.ps1 -p COM4 monitor`,
   or any serial terminal at 115200) and look for `chip revision: v1.3` or `v3.0`. v1.x takes the
   default build and v3.x `-ChipRev v3` (`--chip-rev v3`): the wrong one boots to a black screen.
4. **Build and flash:** `pwsh tools/idf.ps1 -p COM4 flash monitor` (`tools/idf.sh -p /dev/ttyACM0
   flash monitor`). The first build creates your signing key: back it up ("Build and test").
5. **Write down what is particular to your panel:** copy
   [`PANEL.local.example.md`](PANEL.local.example.md) to `PANEL.local.md` (git-ignored) with the
   port, the silicon revision and where the backup is. AGENTS.md has a coding agent read it first,
   so it stops guessing which board and which port it is talking to.

On first boot the panel runs its setup on the touchscreen: Wi-Fi, the time zone, a name and your
own installer and master codes. The factory codes are installer **69420** and master **123456**;
until they change, the status bar says so on every screen. A lost installer code is recovered at
the panel: unplug it for ten seconds, and for two minutes after it starts Settings > Installer
opens without the code.

If the first boot says `stored settings could not be opened` and the screen stays empty, the
settings area still holds what the vendor demo left there. The panel won't erase it by itself (on
a panel in use that area holds the codes, users, zones and Wi-Fi): type `factory_reset confirm` at
the serial console (or `python tools/panel_console.py -p COM4 "factory_reset confirm"`). It
restarts by itself every two minutes until you do.

The **service console** (115200 baud; `help` lists every command) shows the status, simulates
zones (`zone 3 open`), types on a virtual keypad (`keys 123456W` arms Away with the factory master
code), prints the event log (`events`, `log_export` as CSV), scans for Wi-Fi (`wifi scan`), sets
how the panel gets its address (`ip`, `ip static 192.0.2.42 255.255.255.0 192.0.2.1`) and walks
the connection step by step (`netcheck`). From a script, without resetting the panel:
`python tools/panel_console.py -p COM4 status zones`. To update a running panel without the USB
bootloader, `python tools/ota_serial.py -p COM4 firmware/crowpanel_p4/build_v1/alarm_crowpanel_p4.bin`
(about 7 minutes; it restarts once everything is disarmed and goes back to the old firmware by
itself if the new one fails its self-test), or set a release list under Settings > About >
Update source ([docs/features/13](docs/features/13-OTA-Updates/README.md)).

## The rules
The project's rules are written once, for people and AI agents alike, and kept where they are read
when they are needed. Older notes that cite a section of this file find it here:
- **Display safety**, **Rules for the firmware**, **Conventions**, working with a real panel and
  recording work: [AGENTS.md](AGENTS.md).
- **Rules for the portable components** (`alarm_core`, `event_log`, `alarm_proto`, `zone_hub`,
  `sounder`, `backup`, `i18n`, language packs and the rest of `components/core`):
  [components/core/AGENTS.md](components/core/AGENTS.md).
- **Rules for the UI** (the layouts, `alarm_ui`, the words on screen and in the Panel Manager):
  [components/ui/AGENTS.md](components/ui/AGENTS.md).
