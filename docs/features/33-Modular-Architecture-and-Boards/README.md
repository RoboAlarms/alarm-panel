# 33 · Modular architecture and board support

**Status:** Implemented v1 - the six tiers and their CI check, the board contract and its
validation, the feature options, the board manifests and the PC board profiles, all
host-tested; both firmwares are built on it, and the E32R40T firmware runs on a development
board. CI builds only the P4 while the 4" is on hold (BL-079). Left: the
smaller tasks (Tasks). · **Milestone:** M9
(prerequisite of every new board) · **Depends on:** 00, 07, 21 · **First user:**
[19](../19-CYD-ESP32-Port/README.md) (LCDWiki E32R40T)

## Goal
Support more and more cheap ESP32 screens by **composing** each firmware from components,
picking only what fits the board. The E32R40T (classic ESP32, no PSRAM, 4 MB flash, 480x320,
resistive touch) is about the lowest-end board the project will support, which is why the
split happens now: whatever fits there fits everywhere.

The standard for this work: **organisation, modularity and
reproducibility** are the gold standard; everything planned, documented and unit-tested; a
simulator for every board, like the 7" one; a folder layout that stays easy to maintain as
boards are added.

## Principles
1. **Tiers with one-way dependencies**, checked by a script in CI (not by review alone).
2. **A component is self-contained**: `CMakeLists.txt` (ESP-IDF and host), `include/`,
   `src/`, `test/` (host tests), `README.md` (purpose, tier, dependencies, API, options),
   `Kconfig` when it has options. Copying a component folder copies its tests and docs.
3. **A board is a composition**: a board component (drivers + pins + capabilities behind one
   contract) and a thin firmware project whose `sdkconfig.defaults` is the board's
   **manifest** (chip, features, UI layout, capacities). No board is named outside
   `components/boards/` and `firmware/<board>/`.
4. **A feature is a Kconfig option**; off means not linked (the component compiles to nothing
   or to stubs), and the UI hides what the backend doesn't provide.
5. **One source of truth per list**: a component's sources are listed once for both the
   ESP-IDF and the host build; a board's manifest drives both its firmware and its
   simulator; generated files are checked against their sources in CI.
6. **Reproducible**: ESP-IDF v6.1 and `dependencies.lock` pinned, LVGL 9.5.0 pinned, Unity
   vendored, one command per task on Windows and POSIX, and CI builds what a developer builds.
7. **PC first**: behaviour is proven by host tests; every board has a simulator profile with
   its resolution, features and memory limits, so a change is seen before any board is
   flashed.

## Target layout

```
alarm-panel/
  cmake/                    shared CMake: portable-component helper, warnings/sanitizers,
                            LVGL, the UI library, board manifests, firmware project settings
  components/
    README.md               tiers, dependency rules, how to add a component
    core/                   portable C11: no ESP-IDF, no LVGL, no heap, no globals
      alarm_core  alarm_proto  board_info  cc1101_regs  event_log  h264_sps  home_control
      http_redirect  notify  power_monitor  rf_decode  rtc_clock  sounder  storage
      touch_calib  ui_backend  zigbee_host  zone_hub  zwave_host  zwave_proto  zwave_zones
    drivers/                ESP-IDF drivers for chips any board may carry; no board knowledge
      rf_rx  rtc_ds3231  touch_xpt2046
    boards/                 the board contract, one component per board, and the family
                            libraries the boards on one main board share
      board_api  crowpanel_p4_family  board_crowpanel_p4  board_crowpanel_10  board_e32r40t
    services/               ESP-IDF, board-agnostic, feature-gated
      alarm_mem  platform_esp  net_mqtt  net_notify  ha_link  panel_ota  panel_web
      dev_remote  ui_port_esp  alarm_features  panel_storage  panel_clock  rf_service
      zigbee_service  device_service  zwave_service
    ui/                     portable UI logic (LVGL + core + ui_backend only)
      alarm_ui              + layouts/<WxH>/alarm_ui: one LVGL Pro project per screen size,
                            same C contract (1024x600 the 7", 480x320 the 4")
    app/                    the start-up every board shares (above services and the UI)
      panel_app
  firmware/
    README.md               how a firmware project is composed; adding a board
    crowpanel_p4/           thin project: CMakeLists, main/, sdkconfig.defaults*, partitions.csv
    crowpanel_10/           the 10.1", built on crowpanel_p4: what differs, nothing else
    e32r40t/                the same for the 4" board (the status screen until its touch UI
                            fits, docs/features/19; on hold)
  host/                     PC builds: tests runner, UI preview, simulator; board profiles
  tools/                    commands people type (idf, host tests, preview, sim, panelctl...)
  docs/                     manual/ (Astro), features/, bugs/, sensors/, research/, hardware/
```

`components/README.md` indexes every component, with what it does and its features.

### Dependency rules

| Tier | May depend on | Must not |
|---|---|---|
| core | core | include ESP-IDF, FreeRTOS or LVGL headers; allocate; keep globals |
| drivers | ESP-IDF, core | know a board, a service or the UI |
| boards | ESP-IDF, drivers, core, `board_api`, LVGL (display glue); a board its family library | depend on services or the UI; reach a family library from anywhere but its boards |
| services | ESP-IDF, core, drivers, `board_api` (the contract only), `alarm_mem`, other services | name a board component; depend on `alarm_ui` |
| ui | LVGL, core (incl. `ui_backend`) | include ESP-IDF or any service |
| app | everything above, `board_api`, ESP-IDF, LVGL | name a board component or read `CONFIG_BOARD_*` |
| firmware | anything | contain logic that another board would need (that belongs in a component) |

`tools/check_architecture.py` reads every component's `CMakeLists.txt` and `#include`s and
fails CI on a violation; it has its own unit tests.

## The board model
- **`board_api`** (header; the description and its portable validation are the core component
  `board_info`): the functions every board provides (power, display into LVGL, touch,
  backlight, audio sink, UI lock, battery, stats) and a `board_info_t` describing it: name,
  chip, screen size and UI layout, capabilities (touch type, audio kind, SD bus, battery, LED),
  the pins services need (wired terminals, SD, RF, radio UART, the add-on I2C connector) and
  reserved pins, the audio sample rate (`audio_rate_hz`, 8-32 kHz in whole hundreds of Hz; the
  CrowPanel says 32000, the E32R40T 24000, since the DAC's continuous mode wants 20 kHz or
  more). Services receive `board_info_t` from the app. `board_i2c_ext()` hands out the
  add-on connector's bus (the battery-backed clock, docs/features/35), created on
  first use; NULL on a board without one.
- **`board_info_validate()`** (portable, host-tested) rejects a description that would harm
  the board: pins used twice, outputs on input-only pins, anything on the flash/PSRAM pins,
  strapping pins used for inputs that could hold them at boot. It would have caught the RF
  defaults 6-10, which are the classic ESP32's flash pins.
- **Drivers** are shared: the same XPT2046 or ST7796 driver serves every board that carries
  it; the board component only wires pins and picks drivers.
- **A firmware sees one board.** `cmake/alarm_idf.cmake` gives a project every tier but, of
  `boards/`, only `board_api` and its own `board_<board>` (`ALARM_BOARD`), and that board's
  family library when it names one (`ALARM_BOARD_FAMILY`). The component manager solves the
  registry dependencies of every component it can see, so the P4's display driver (published
  for the esp32p4 only) stopped the first ESP32 build; a board's packages need no target rules
  this way.
- **Boards on one main board share a family library** (`components/boards/<name>_family`):
  everything but the description, the wiring that differs and the names, which stay in each
  board's component (BRD-023). The 7" and the 10.1" share `crowpanel_p4_family`; a difference
  in the parts fitted is an option of the family (the 10.1"'s ES8311,
  `CONFIG_BOARD_CROWPANEL_P4_ES8311`, selected by its board's `Kconfig`), never an `#if` on a
  board. Only board components may require a family (`tools/check_architecture.py`), and the
  firmware names the one its board requires (`tools/check_boards.py`).
- **A firmware can be built on another** (`ALARM_BOARD_BASE`): the base's `sdkconfig.defaults*`
  come first and the project's after them, and its `main/` builds the base's sources, so
  `firmware/crowpanel_10` holds only what differs (`firmware/README.md`).

## The feature model
Every optional part is a Kconfig option: the app-level switches in one "Alarm features" menu
(`components/services/alarm_features`), the rest with the component that uses them (zone
sources in `rf_service`, `zigbee_service`, `zwave_service` and `platform_esp`, Devices in
`device_service`). Each board's `sdkconfig.defaults` states every one of them explicitly
(checked from step 9), so the manifest is complete without reading Kconfig defaults. `panel_storage` drives each card
bus behind its own option (`PANEL_STORAGE_SDMMC`, default y where the chip has SDMMC;
`PANEL_STORAGE_SDSPI`, default n: linking both cost the P4 about 10 KB). ESP-IDF
resolves requirements before it reads the configuration, so requirements can't follow options:
an optional component is always safe to require, and "off" is handled in its sources, either
absent (callers guard) or stubs (callers don't); `alarm_features`' README has the rule.

| Option | Component(s) | 7" P4 | E32R40T |
|---|---|---|---|
| `ALARM_FEATURE_UI` | `alarm_ui`, LVGL, the display start in `panel_app` | y | y (the 480x320 layout; the status screen until the touch UI fits its RAM, docs/features/19 Phase 3) |
| `ALARM_FEATURE_MQTT` | `net_mqtt` | y | y |
| `ALARM_FEATURE_NOTIFY` | `net_notify`, notification pages + QR | y | n (optional later) |
| `ALARM_FEATURE_HA` | `ha_link`, integration page | y | n |
| `ALARM_ZSRC_HA` | `zsrc_ha` | y | n |
| `ALARM_HOME_CONTROL` (needs SPIRAM) | `home_control` service | y | n |
| `ALARM_FEATURE_AUTOMATIONS` (needs `ALARM_HOME_CONTROL`) | `automation`, `automation_service` (docs/features/41) | y | n |
| `ALARM_LINK` (needs SPIRAM) | `panel_link`, `plink_service`: keypad panels and the keypad role (docs/features/11) | y | n (its keypad role is docs/features/19 CYD-015) |
| `ALARM_PANEL_WEB` | `panel_web` | y | n |
| `ALARM_DEV_REMOTE` | `dev_remote`, screen capture | y | n |
| `ALARM_ZSRC_ZIGBEE` | `zigbee_host`, `zsrc_zigbee` | y (ADR-047; the radio probe keeps an empty socket from starting the stack) | n |
| `ALARM_ZSRC_RF` | `rf_rx`, `rf_decode`, `zsrc_rf` | y (the receiver is optional) | y (the receiver is optional) |
| `ALARM_ZSRC_ZWAVE` (needs SPIRAM; `ALARM_ZWAVE_S0`/`ALARM_ZWAVE_LR` inside it, phase 3) | `zwave_proto`, `zwave_host`, `zwave_zones`, `zsrc_zwave`, `zwave_service` | y (the radio stays off, touching no pin, until a place is chosen) | n |
| `ALARM_FEATURE_STORAGE` | `panel_storage`, the Storage page | y | n (SPI card later) |
| `ALARM_FEATURE_PHOTO_SAVER` | the screen saver's JPEG picture | y | n |
| `ALARM_FEATURE_RTC` | `panel_clock`, `rtc_ds3231`, `rtc_clock` (docs/features/35) | y | n (its I2C header's pins are wired terminals) |
| `ALARM_SERVICE_CONSOLE` | console | dev | dev |

## The UI model
- `components/ui/alarm_ui` is the single copy of UI behaviour. It reaches the panel only
  through `ui_backend_t` (now its own core component, so the web server can use it without
  the UI), and its ESP implementation lives in `services/ui_port_esp`.
- Each screen size is an LVGL Pro project under
  `components/ui/alarm_ui/layouts/<width>x<height>/alarm_ui`, sharing the subjects, object
  names and component signatures the C code uses; `lvpro_gen.py --check` checks each project's
  generated code against its XML, and `tools/check_ui_contract.py` the contract between them.
- Feature pages compile only with their feature; Settings rows are shown by capability.

## The host model
- One way to build portable components for the PC (`cmake/alarm_host.cmake`), used by the
  tests, the preview and the simulator: sources listed once, warnings and sanitizers applied
  the same way everywhere.
- Tests live in each component's `test/` folder and are found automatically.
- **Board profiles**: `-Board crowpanel_p4 | e32r40t` for the preview and the simulator. The
  profile is read from `firmware/<board>/sdkconfig.defaults` (layout, screen, DPI, pages, every
  feature option as `CONFIG_<option>=0|1`): the simulated panel offers what that firmware's
  backend would and the one scripted walk visits the board's pages; a board with an LVGL
  budget (`CONFIG_ALARM_UI_LVGL_BUDGET_KB`) gets the walk's peak reported against it. The LVGL
  parts a manifest states (widgets, flex/grid, `LV_DRAW_SW_SUPPORT_*`, TJPGD, QR code,
  compressed fonts) build the PC's LVGL too (`ALARM_BOARD_LV_DEFINITIONS`): a widget
  the board leaves out fails the PC build, and a draw format it leaves out is missing from the
  screenshots (with LVGL's "Not supported source color format" warning) - how the P4's RGB888
  was found to be needed for gradients.
- **Firmware optimisation**: a board builds for size (`CONFIG_COMPILER_OPTIMIZATION_SIZE`); one
  with flash to spare calls `alarm_optimize_hot_paths()` after `project()` (cmake/
  alarm_firmware.cmake) to build LVGL at `-O2`, as the P4 does.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| ARCH-001 | Components sit in the six tiers and depend only downwards (CMake requirements and `#include`s) | `tools/check_architecture.py` in CI, `tools/tests/test_check_architecture.py` |
| ARCH-002 | Only board components read `CONFIG_BOARD_*`; board components don't read `CONFIG_ALARM_*` | same |
| ARCH-003 | Every component has a README and a row in `components/README.md`; every core component has host tests | same |
| ARCH-004 | Known exceptions name the step that removes them, and an exception that matches nothing fails the check | same |
| ARCH-005 | A component's sources are listed once for ESP-IDF and the PC builds; tests are found, not registered | review, host build |
| ARCH-006 | Every component has the same shape (principle 2): public headers in `include/`, sources and private headers in `src/`, host tests in `test/`; no `.c` or `.h` at its root | `tools/check_architecture.py` in CI, `tools/tests/test_check_architecture.py` |
| MEM-001 | Slow memory is PSRAM with no fallback on chips that have it, internal RAM on chips without; services never name `MALLOC_CAP_SPIRAM` | P4 memory map unchanged (step 3), E32R40T firmware (step 10) |
| BRD-001 | A board description names its chip; only pins that chip has pass | `components/core/board_info/test` |
| BRD-002 | Flash pins, and PSRAM pins on modules with PSRAM, are refused | same |
| BRD-003 | An output on an input-only pin is refused | same |
| BRD-004 | A contact on a pin without an internal pull-up is refused unless one is fitted | same |
| BRD-005 | An outside signal (terminal, receiver data, radio RX) on a strapping pin is refused | same |
| BRD-006 | Two uses of one pin are refused unless both are lines of one bus carrying one signal (BRD-020). `board_info_validate` judges a whole layout; whether a person may put a part of their own on a pin at all is the reachable-pin check of a custom place (BRD-021) | same |
| BRD-007 | An incomplete description is refused | same |
| BRD-008 | Every board's own description passes, and matches its pin header | each board's `test/` |
| BRD-009 | Pins chosen in Kconfig are validated against the board at start-up; a refused pin leaves the feature off with a log line instead of configuring the pin | review; E32R40T bring-up |
| BRD-010 | No service names a board: services get pins, terminals and model names from the app | `tools/check_architecture.py`, review |
| BRD-011 | A firmware's build contains its own board component and no other (nor the packages another board declares), and its board's family library when it has one | `cmake/alarm_idf.cmake`; `tools/check_boards.py` (main names its own board only; the family named is the one the board requires); the E32R40T build |
| BRD-012 | Every board's firmware builds in CI, and its image fills at most 85 % of its OTA slot (docs/features/19 CYD-013) | `.github/workflows/firmware.yml` (the E32R40T left out while docs/features/19 is on hold, BL-079), `tools/check_image_size.py`, `tools/tests/test_check_image_size.py` |
| BRD-013 | A 433 MHz receiver's data line may move to a wired terminal (`board_rf_data_on`): the terminal leaves the offered list, only the board's data pin or a terminal is accepted, and the result is validated like any description (docs/features/25 RF-008) | `components/core/board_info/test`, `components/boards/board_crowpanel_p4/test` |
| BRD-014 | A board may offer other places for the whole 433 MHz receiver (`rf_places`, `board_rf_place_on`): the receiver takes that place's pins, every one of them that was a wired terminal leaves the list, and the result is validated like any description (docs/features/25 RF-019) | `components/core/board_info/test`, `components/boards/board_crowpanel_p4/test` |
| BRD-015 | ... and other places for its add-on I2C modules with a controller of their own (`i2c_places`, `board_i2c_place_on`, docs/features/35 RTC-013); a description with an unreadable place (no id, no array, no port) is refused | same |
| BRD-016 | A board may offer places for a Z-Wave radio (`zwave_places`, `board_zwave_place_on`, docs/features/36 hardware.md §4): the radio takes that place's TX/RX, a place that shares the sensor-radio socket's UART takes the socket too (no ESP32-H2 there), every pin that was a wired terminal leaves the offered list, and the result is validated like any description | `components/core/board_info/test`, `components/boards/board_crowpanel_p4/test` |
| BRD-017 | The Z-Wave radio's reset line may move onto one of the board's wired terminals (`board_zwave_reset_on`, docs/features/36 hardware.md §5), the way a 433 MHz receiver's data line does: the terminal leaves the offered list, and the result is validated like any description | `components/core/board_info/test`, `components/boards/board_crowpanel_p4/test` |
| BRD-018 | A description lists the pins a person can reach for an add-on of their own (`reach`: GPIO, label as printed, connector; on the 7" J7's ten IO pins, IO25 excluded; none on the E32R40T); each is a pin the chip has, with a label and a connector, or the description is refused | `components/core/board_info/test`, `components/boards/board_crowpanel_p4/test` (the Flasher reads `REACHABLE` too) |
| BRD-019 | A description's own pins are complete: every pin wired to a part of the board, used by the firmware or not (on the 7": the microphone, camera, UART1 and UART3 IN connectors), so none is offered to an add-on | `components/boards/board_crowpanel_p4/test` |
| BRD-020 | Sharing knows each bus: a shared line names its bus (`BOARD_BUS_SPI(host)`, `BOARD_BUS_I2C(port)`) and signal (SCK, MOSI, MISO, SDA, SCL), and two uses share a pin only on the same bus and for the same signal (`BOARD_ISSUE_OTHER_BUS`, `BOARD_ISSUE_OTHER_SIGNAL`); every layout of the named places the firmware accepted before still passes | `components/core/board_info/test`, `components/boards/board_crowpanel_p4/test` (every clock x receiver x Z-Wave place, `rf pin` on each terminal) |
| BRD-021 | A part on custom pins (`board_rf_custom_on`, `board_i2c_custom_on`, `board_zwave_custom_on`) is accepted only on reachable pins, each signal on its own pin, none of the board's own nor another placed part's; a wired terminal it takes leaves the offered list (the zone keeps its binding and shows Check later); the result is validated, and a refusal names the GPIO and signal. It fails closed: the caller turns the part off, never back to its default pins | same |
| BRD-023 | Boards on one main board share one family library that only their board components require, and a firmware built on another (`ALARM_BOARD_BASE`) reads the base's settings first and its own after; a base has no base | `tools/check_architecture.py` and `tools/check_boards.py` with their tests; `tools/tests/test_check_boards.py` (the 10.1"'s PC profile is the 7"'s); the 10.1" build |
| BRD-022 | The console's `key=value` arguments for custom pins (`board_custom_parse`): required and optional keys in any order, decimal GPIO 0-63 only; an unknown or repeated key, a missing required one or a value that is not a GPIO number is refused with words | `components/core/board_info/test` |

## Steps
Each step leaves the P4 firmware's behaviour unchanged unless it says otherwise, and is
verified before the next: host tests, UI preview screenshots byte-identical to the baseline,
`lvpro_gen --check`, image checks, Python and Node tool tests, a P4 v1 dev build (image size
compared). Nothing is flashed or committed unless asked.

- [x] **1. Folder reorganisation** (moves and path fixes only): components into the tiers, host
  tests into each `test/`, shared `cmake/`, the UI layouts moved
- [x] **2. Guardrails**: `tools/check_architecture.py` (+ unit tests, CI job), a `README.md`
  in every component, `components/README.md`, `firmware/README.md`, `host/README.md`
- [x] **3. `alarm_mem`**: one memory policy (PSRAM when present, internal otherwise); the
  PSRAM-only allocations and stacks replaced
- [x] **4. `board_api`**: contract, `board_info_t`, `board_info_validate()` with host tests;
  the CrowPanel implements it; services take pins and terminals from it
- [x] **5. `ui_backend` and `ui_port_esp`**: the contract becomes a core component; the ESP
  implementation moves to services; `alarm_ui` becomes portable; `panel_web` stops requiring
  the UI
- [x] **6. Features really optional**: one feature menu; optional components required only when
  enabled; `platform_esp` sheds four parts; UI pages gated; minimal-feature UI build
- [x] **7. `panel_app`**: the start-up sequence leaves `app_main.c` for a shared component
  driven by `board_info_t` and the features; each firmware's `main` shrinks to a few lines
- [x] **8. Service trims**: `panel_ota` per board, `sounder` rate per board (tests at 8, 16 and
  32 kHz; AUD-006, docs/features/08), `panel_storage` drives SPI cards too
- [x] **9. Board profiles on the PC**: preview and simulator take `-Board`; manifests parsed
  and checked (every feature stated); LVGL heap capped per board
- [x] **10. Proof**: a headless classic-ESP32 firmware (`firmware/e32r40t`) builds from the same
  components (image 66.9 % of its 1,966,080 B slot) and CI builds both (1633c0b; the E32R40T
  left CI while docs/features/19 is on hold, 0c54bb3, BL-079)

After step 10 the board-specific work continued in docs/features/19 (bring-up, the 480x320
layout that makes the E32R40T simulator useful, acceptance on the board); it is on hold
(BL-079).

## Test plan (new)
| Test | Where | Proves |
|---|---|---|
| Architecture rules | `tools/tests/test_check_architecture.py` + CI | tiers stay one-way |
| Board description validation | `components/core/board_info/test`, each board's `test/` | bad pin maps are refused per chip; every board's own map passes |
| Manifest completeness and parsing | `tools/tests/test_check_boards.py` | every board states every feature; host and firmware read the same values; a headless board has no PC profile |
| Image against its OTA slot | `tools/tests/test_check_image_size.py`, `.github/workflows/firmware.yml` (the E32R40T left out while docs/features/19 is on hold, BL-079) | every board's firmware builds and leaves room to grow (BRD-012) |
| UI layout contract | `tools/check_ui_contract.py`, `tools/tests/test_check_ui_contract.py` + CI | layouts expose what the C code finds |
| Minimal-feature UI build | `ui_smoke_minimal` (`host/ui_preview/smoke_minimal.c`), run by `tools/ui_preview.ps1` and CI | disabled features really compile out |
| Sounder at other rates | `components/core/sounder/test` | fire/CO timing identical at every rate |
| Memory policy | by construction (a compile-time choice) and the P4 memory map | placement unchanged on the P4; exercised on the ESP32 by step 10 |
| Touch filter and calibration | `components/core/touch_calib/test` (docs/features/19) | resistive filtering and calibration maths |

## How to add a board
`board_e32r40t` and `firmware/e32r40t` are the smallest complete example.
1. Save the vendor documentation in `docs/hardware/<board>/` (git-ignored) and add its links to
   `docs/hardware/README.md`.
2. `components/boards/board_<name>/`: `board_pins.h` from the vendor's pin table, a portable
   `board_desc.c` (`board_info_t`), `board.c` implementing `board_api` (functions the board
   can't do yet answer `ESP_ERR_NOT_SUPPORTED`), a `test/` that runs `board_info_validate()` on
   the description and checks it against the pin table (BRD-008), a README, a row in
   `components/README.md`.
3. `firmware/<name>/`: `CMakeLists.txt` setting `ALARM_BOARD <name>` (copy one), a
   `sdkconfig.defaults` stating the chip, flash, signing and every manifest option
   (`python tools/check_boards.py` says what is missing), `.dev` / `.release` fragments,
   `partitions.csv`, and a `main` that fills `panel_app_config_t` and calls `panel_app_run()`.
4. Build with `pwsh tools/idf.ps1 -Project <name> build` (`tools/idf.sh --project <name>`), then
   `python tools/check_image_size.py firmware/<name>/build --ram`; add the board to the matrix
   in `.github/workflows/firmware.yml`.
5. If no existing layout matches the screen, add `components/ui/alarm_ui/layouts/<w>x<h>/alarm_ui`
   (`components/ui/alarm_ui/layouts/README.md`: copy the nearest layout, keep its subjects,
   component APIs and looked-up names - `python tools/check_ui_contract.py` says what is
   missing), give the manifest an LVGL budget, and run `pwsh tools/ui_preview.ps1 -Board <name>`;
   until then the board is headless (`ALARM_FEATURE_UI` off) and has no PC preview.
6. A Features doc for the board (like docs/features/19).

A board on a main board we already support (the 10.1" on the 7"'s) takes less: a
`board_<name>` with only `board_desc.c`, `board_id.c` (`board_info()`, `board_name()`),
`board_pins.h` (including the family's shared pins) and `board.h` (its MQTT and Home Assistant
names), requiring the family library, and its test; a `firmware/<name>` whose `CMakeLists.txt`
sets `ALARM_BOARD_FAMILY` and `ALARM_BOARD_BASE`, whose `sdkconfig.defaults` states only what
differs (the base's `partitions.csv`, its own update channel), and whose `main/CMakeLists.txt`
includes the base's app (`crowpanel_p4_app.cmake`) with its own board component; an entry in the
Flasher's `boards.json` (`"offered": false` until the Flasher can tell the board apart); and a
section in the family's feature document (docs/features/06 "The 10.1" (V1.2)").

## Open questions
- Whether `tools/` should be grouped into subfolders. Leaning no: it holds the commands
  people type, the docs quote them, and there are few enough to read at a glance. Decided
  2026-09-28: no.
- Whether to rename `ui_backend_t` to something like `panel_api_t` now that the web server
  uses it too. Leaning later: the rename touches every UI module for no behaviour change.
  Decided 2026-09-28: no rename.

## Tasks
The ten steps of the restructure are under "Steps" above, all done. After them:
- [x] Fix the drift in the code's comments, docs/features/25 and 29 and CONTRIBUTING.md, and
  remove those items from "Found along the way" (297d699, b856211; docs/features/08 AUD-003,
  docs/features/01 CORE-CFG-016, LOG-007 and NOTE-035 in test comments)
- [x] Fix the rest of the drift under "Found along the way": `dev_cert.h` and `dev_cert.c` use
  their own `DEV_CERT_MAX`/`DEV_KEY_MAX`; the list is empty
- [ ] The PC's simulated panel offers only the zone sources the board's manifest carries
  (`host/ui_preview/host_panel.c` `be_zone_sources`); refresh the 7" screenshots with it
  (BL-087; the P4's manifest now carries Zigbee, ADR-047, so the preview keeps its Zigbee pages)
- [ ] Generalise the P4-only tools (`tools/check_release_profile.py`, `tools/panelctl.py`,
  `tools/ota_serial.py` examples) when a second board gets releases or remote development
  (BL-079)
- Duplicate of docs/features/19 (Resuming, 'Then undo the four exclusions of commit 0c54bb3'):
  putting the E32R40T back into `.github/workflows/firmware.yml` makes BRD-012 hold for every
  board again (BL-079).

## Found along the way (to fix after step 10)
The README pass of step 2 read every component against its docs and found drift that
predates this work. The list is empty; Tasks names what became backlog rows (BL-087, BL-079).
