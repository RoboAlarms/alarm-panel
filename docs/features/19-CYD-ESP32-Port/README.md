# 19 · 4" ESP32 panel (LCDWiki E32R40T, "CYD")

**Status:** On hold (decided) - the module split is done, the 480x320 layout previews,
and display and touch drivers run on the board, which shows the status screen because the UI
needs about three times its LVGL memory; Left: paged lists, DAC and battery drivers, acceptance
(BL-079), keypad role (BL-048); to resume, bring the 480x320 layout up to date, then undo four
exclusions (Tasks, Resuming) · **Milestone:** M9 · **Depends on:** 01, 04, 07, 13, 22; keypad
role needs 11

**Tests:** `components/core/touch_calib/test/test_touch_calib.c` (CYD-008, CYD-009),
`components/boards/board_e32r40t/test/test_board_desc.c`, `tools/tests/test_check_ui_contract.py`
(CYD-007), `tools/tests/test_check_image_size.py` (CYD-013),
`components/core/sounder/test/test_sounder.c` AUD-006 (the board rates of CYD-014)

## Goal
Run the same alarm on 4" ESP32 boards: first as a **compact
stand-alone panel** (its own engine, zones, sounder, Wi-Fi, MQTT, signed updates), later as a
**remote keypad** for the 7" panel once the panel link (docs/features/11) exists. The board has a
tenth of the P4's RAM and a quarter of its flash, and a **resistive** touch screen. So the work
starts by splitting the code base, so that each board takes only the modules it can carry,
and both boards share one copy of everything else.

## The board
LCDWiki "4.0inch ESP32-32E Display", SKU **E32R40T** (with touch; E32N40T without).
Wiki: https://www.lcdwiki.com/4.0inch_ESP32-32E_Display. A unit identified itself
over USB on 2026-09-23.

| Part | Detail |
|---|---|
| SoC | ESP32-D0WD-V3, **silicon v3.1**, dual core 240 MHz, 520 KB SRAM, **no PSRAM** |
| Flash | 4 MB (QSPI, DIO boot), 3.3 V set by the IO12 strapping pin |
| USB | CH340C serial with auto-reset (RTS/DTR) |
| Display | 3.95" TN, **ST7796S**, 320x480, 4-wire SPI: SCK 14, MOSI 13, MISO 12, CS 15, DC 2; reset tied to EN; backlight IO27 (high = on) |
| Touch | **XPT2046 resistive**, on the display's SPI bus: CS 33, PENIRQ 36 |
| Audio | DAC IO26 -> FM8002E amplifier (1.5 W / 8 ohm), amplifier enable IO4 (low = on) |
| microSD | SPI: CS 5, MOSI 23, SCK 18, MISO 19 (shared with the SPI header, whose CS is IO21) |
| RGB LED | common anode, low = on: red 22, green 16, blue 17 |
| Battery | 3.7 V LiPo socket, TP4054 charger, voltage on ADC IO34 |
| Free pins | IO35, IO39 (input only, no internal pull-ups), IO25 / IO32 (I2C header), IO21 (SPI header CS) |
| Offered (`board_desc.c`) | wired terminals IO39 (fit an external pull-up), IO25, IO32; a CC1101 on the SPI header (CS IO21) or a receiver module, data on IO35; sounder at 24 kHz |
| Buttons | BOOT (IO0) and RESET, both behind the case in use: never needed by a flow (ADR-014) |

What differs from the P4 and matters to the design:
- **No PSRAM.** The P4 build keeps about 108 KB of statics, the LVGL heap, two 1.2 MB draw
  buffers and several task stacks in PSRAM, and seven allocations there have no fallback.
- **4 MB flash**: two app slots of about 1.9 MB, where today's P4 release image is 2.69 MB.
- **An SPI display with its own frame memory.** The P4's picture dropouts during flash writes
  (a DSI panel refreshed from PSRAM) cannot happen here; the display risks are noise in the
  panel's memory at power-up and tearing, not strobing.
- **Resistive touch**: needs calibration, a firm press, and filtering; drags and small targets
  work badly.
- **Native Wi-Fi** (no co-processor, no ESP-Hosted), native ESP-NOW for the keypad role.

## Where the code stood (survey 2026-09-23)

Resolved by docs/features/33 steps 3-10; kept short as the reason for the split.

**Reusable as is** (portable C11, host-tested): `alarm_core` (capacities default to 64 zones / 32
users off the P4), `event_log`, `alarm_proto`, `zone_hub`, `rf_decode`, `notify`, `sounder`,
`storage`, `power_monitor`, `home_control`. Board coupling is small: apart from
`firmware/crowpanel_p4/main`, only `platform_esp/panel_storage.c` touches the board (SD pins).

**What had stopped a second board**: PSRAM-only allocations and stacks (start-up failed);
nothing optional in the build (P4-only drivers, an image too big); a UI that compiled every page
and a web server that required `alarm_ui`; one 1024x600 layout; P4 names in shared code
(OTA device, partition labels, RF pin defaults 6-10 that are the ESP32's flash pins: a brick
risk); a fixed sounder rate; up to 9 concurrent TLS sessions (~21 KB each; the ESP32 affords one
lasting and one short-lived).

The UI's C logic finds screen objects **by name** (`ui_find`, 57 uses) and builds pages from
about 15 generated components (`list_row`, `row_*`, `zone_card`, `event_row`...), so a second
layout that keeps the same names, subjects and component signatures runs the **same C logic
unchanged**.

## Target module structure

```
PORTABLE (host-tested; unchanged contracts)
  alarm_core  event_log  alarm_proto  zone_hub  rf_decode  notify  sounder  storage
  power_monitor  home_control

BOARD LAYER  (one shared contract, one component per board)
  board_api            header only: board.h + board_info_t (capabilities, pins, terminals)
  board_crowpanel_p4   EK79007 DSI, GT911, I2S + NS4168, flicker guard, battery (existing)
  board_e32r40t        ST7796S SPI, XPT2046 + calibration, DAC + FM8002E, RGB LED, battery ADC

ESP SERVICES  (board-agnostic; each behind a Kconfig option; memory through alarm_mem)
  platform_esp   alarm task, store, codes, event log writer, prefs, zones task, sounder task,
                 Wi-Fi/time/netcheck, console, system
  panel_storage  SD card: SDMMC (P4) or SDSPI (ESP32) chosen from board_info_t   [split out]
  panel_ota      signed OTA core; cp_ota only with ESP-Hosted                   [trimmed]
  net_mqtt  net_notify  ha_link  panel_web  dev_remote  zigbee/device services  [optional]
  ui_backend     header only: ui_backend_t (used by alarm_ui AND panel_web)      [split out]

UI
  alarm_ui       portable logic; feature pages compiled only when enabled (Kconfig)
  components/ui/alarm_ui/layouts/1024x600/alarm_ui   LVGL Pro project, 1024x600 (7")
  components/ui/alarm_ui/layouts/480x320/alarm_ui    LVGL Pro project, 480x320 (4"); same subjects, object names,
                         component names and props; a check keeps the two in step

FIRMWARE
  firmware/crowpanel_p4   (existing)       firmware/e32r40t   (new)
```

Which board carries what:

| Module | P4 7" | E32R40T panel | E32R40T keypad (M7) |
|---|---|---|---|
| `alarm_core`, `event_log`, `notify`, `zone_hub` (simulator, wired) | yes | yes (64 zones / 32 users, 128 KB log) | no (the master's) |
| `sounder` | yes (I2S 32 kHz) | yes (DAC, rate per board) | yes |
| Wi-Fi, time, connection check | yes (via C6) | yes (native) | yes |
| `panel_ota` signed updates + rollback | yes (+ C6 updates) | yes (no C6 part) | via the master's relay (M8) |
| `net_mqtt` (MQTT/TLS, HA discovery) | yes | yes: the one lasting TLS session | no |
| `net_notify` (ntfy, Pushover) | yes | optional, measured (short-lived TLS, never alongside an update download) | no |
| RF 433 MHz (`rf_rx`, `zsrc_rf`) | optional | optional: CC1101 on the SPI header (CS 21) with its data on IO35, or a receiver module on IO35 | optional |
| Wired zones | terminals of the P4 board | IO39 (external pull-up), IO25, IO32 (IO35 is the receiver's data line) | inputs over the link |
| `ha_link` (HACS integration), `zsrc_ha`, `home_control` | yes | **no** (112 KB of link buffers, a TLS server) | no |
| `panel_web`, `dev_remote` (browser manager, screen capture) | yes | **no** (TLS server, frame buffer, JPEG) | no |
| Zigbee (`zigbee_host`) | planned (H2) | no (maybe an NCP later) | no |
| SD card (`panel_storage`) | yes | not in v1 (slot is there, SDSPI later) | no |
| Photo screen saver (JPEG), notification QR codes, Storage page | yes | **no** (plain saver: clock and state) | no |
| Service console | dev builds | dev builds (UART0) | dev builds |

## Decisions
Proposed 2026-09-23. Three became ADRs in docs/features/00 and one was already ADR-022 (each says
which); the TLS slot is still a proposal.
- **Board contract.** Every board implements one header (`board_api`), and services get pins,
  terminals and capabilities from a `board_info_t` passed in by the app, never from a board
  header. Only `firmware/<board>/main` names a board. Decided 2026-09-23 in docs/features/00
  ADR-035.
- **One UI logic, one layout per screen size.** `components/ui/alarm_ui` stays the single copy of
  behaviour; each screen size is its own LVGL Pro project (the editor keeps working) that
  exposes the same subjects, object names and component signatures. `lvpro_gen.py --check`
  also checks that contract between the projects. Rejected: one responsive layout (1024x600
  and 480x320 need different compositions, not different sizes), and per-target constants in
  the generator (the editor wouldn't understand them). Decided 2026-09-23 in docs/features/00
  ADR-036; the contract check between the projects was built as `tools/check_ui_contract.py`.
- **One memory policy.** `alarm_mem.h` gives `ALARM_SLOW_BSS` and `alarm_mem_slow_malloc()`:
  PSRAM when the chip has it, internal RAM otherwise, in one place instead of scattered
  `MALLOC_CAP_SPIRAM` calls. Anything that writes flash keeps an internal stack (docs/bugs/B001).
  Decided 2026-09-23 in docs/features/00 ADR-037.
- **Optional means not linked.** A disabled feature is neither compiled nor linked (ADR-022;
  the P4 build follows it since docs/features/33 step 6, task 1.3 below); the UI hides its rows
  (capabilities, not only NULL-safe pages). Decided in ADR-022 and built 2026-09-23: an optional
  component compiles to nothing or to stubs (the rule is in `alarm_features`' README).
- **One TLS session at a time beyond MQTT** on boards without PSRAM: a shared "TLS slot" that
  notify, the release check and OTA downloads take in turn. Still proposed and not built
  (Phase 2, BL-079).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| CYD-001 | The portable components are the same code on both boards; only compile-time capacities differ | build |
| CYD-002 | Capacities on the E32R40T: 64 zones, 32 users (the core's non-P4 Kconfig defaults) | build |
| CYD-003 | Stand-alone panel role: arm/disarm, status, zones and bypass, troubles, activity, users, installer programming, Wi-Fi and time, chime, panic, sounder | HIL |
| CYD-004 | Disabled features are not compiled or linked; the UI hides their Settings rows | build + preview |
| CYD-005 | MQTT over TLS with HA discovery; at most one lasting TLS session plus one short-lived one, taken in turn | HIL |
| CYD-006 | Signed updates with rollback, the same signing key as the P4 (needs `CONFIG_ESP32_REV_MIN_3`; the board is v3.1) | HIL |
| CYD-007 | The 4" UI is its own LVGL Pro project at 480x320, with the same subjects, object names and component signatures as the 7"; the check fails when they drift | CI |
| CYD-008 | Touch calibration at first start-up, again from Settings > Display, and by keeping a finger on the screen during the first seconds after power-up (no physical button); stored in NVS | HIL |
| CYD-009 | Touch filtering: pressure threshold, median of samples, and a release debounce so that a 2 s hold (panic, factory reset) survives short pressure dips; no phantom touches | host test of the filter + HIL |
| CYD-010 | Targets at least 56 px (primary) and 48 px (any), 8 px apart; no function needs a drag, swipe or scroll: paged lists with buttons, steppers instead of sliders | preview review |
| CYD-011 | Display safety (UI-020 applies unchanged): the backlight stays off until the first full frame is in the panel's memory; backlight PWM at 20 kHz or more; nothing flashes; the RGB LED is steady or off | HIL |
| CYD-012 | After 24 h with Wi-Fi, MQTT/TLS connected and the UI in use: free internal heap never under 32 KB, largest free block never under 16 KB (reported by `status`) | HIL soak |
| CYD-013 | The image fills at most 85 % of its OTA slot; CI reports the size | CI |
| CYD-014 | Sounder through the DAC and FM8002E; fire (temporal-3) and CO (temporal-4) timings identical to the P4 (same mixer, rate chosen per board), measured by the host tests at the board's rate | host test + listening |
| CYD-015 | Keypad role (after M7): the same 4" UI with the panel-link backend | HIL |

## Budgets (estimates, to be measured on the board)

**Flash, 4 MB** (`firmware/e32r40t/partitions.csv`; flashed 2026-09-23, so changing it now
wipes the panel):

| Partition | Size | Note |
|---|---|---|
| bootloader, table, nvs, otadata, phy | up to 0x20000 | table at 0xC000 as on the P4 (the bootloader is 26 KB: the default 0x8000 would leave it under 2 KB to grow); NVS 64 KB at 0xD000 |
| ota_0, ota_1 | 1920 KB each | target image at most 1.6 MB (CYD-013) |
| eventlog | 128 KB | 2048 events (the P4 keeps 384 KB) |

**Measured 2026-09-23.** Headless (docs/features/33 step 10): image 1,314,816 B = 66.9 % of the
slot. With the touch UI linked (LVGL, `components/ui/alarm_ui/layouts/480x320/alarm_ui`, fonts): 1,773,568 B = 90.2 %, over
the line; then `-Os`, the common-CA certificate bundle and 28/40 px icon fonts cut to the glyphs
drawn that large: **1,511,424 B = 76.9 %**; static DRAM 114 KB of 180.7 KB (43 KB of it the UI's
static state), IRAM 92.9 KB of 128 KB. LVGL allocates from the system heap (no fixed pool while
headless). `tools/check_image_size.py` prints the same; CI ran it for every board until the
hold (commit 0c54bb3, 2026-09-24), and runs it only for the P4 while the 4" is on hold.

**Latest (2026-09-24, `esptool image-info`):** 83.5 % of the slot (1,642,496 B; 80.2 % at
`4371cc1`), after the installer pages and sensor settings the touchscreen gained for
docs/features/31 parity. The signed image is padded to 64 KB steps, so the percentage moves in
steps of 3.3 %: about 51 KB more content makes it 86.9 % and fails the check. The firmware carries
the whole touch UI although the board shows the status screen (`panel_app` picks at run time), so
leaving the UI out while it can't run is the first saving if needed. The icon fonts are
RLE-compressed (`CONFIG_LV_USE_FONT_COMPRESSED=y`).

**Free heap on the board** (status screen, Wi-Fi idle, no MQTT broker): 105 KB before the
display's UI starts, 25 KB at "running", 68 KB once start-up settles.

Image estimate: ESP-IDF + Wi-Fi + lwIP + mbedTLS 0.8-0.9 MB, LVGL trimmed 0.25-0.3 MB, 4" UI
and fonts about 0.25 MB (the 7" fonts alone are 249 KB; the 96 px icon font is 72 KB), alarm
and services about 0.2 MB: 1.5-1.65 MB. Tight but possible.

**Internal RAM, about 300 KB usable:**

| Item | Estimate |
|---|---|
| Wi-Fi, lwIP, event loop | 55-70 KB |
| One TLS session (dynamic buffers) | 25-40 KB |
| LVGL draw buffers, 2 x 480x20 lines, DMA | 38 KB |
| LVGL objects and styles (paged lists, one resident screen) | 40-60 KB |
| Engine, snapshot, event ring at 64/32 | about 15 KB |
| Task stacks | about 50 KB |
| UI state, string subjects, one config copy | about 20 KB |

## Touch and screen design (resistive, 480x320)
- **Driver** (`board_e32r40t`): polled from the LVGL read callback like the P4's GT911 (the
  interrupt mode lost releases there); PENIRQ only skips the SPI read when nothing presses.
  Z1/Z2 pressure threshold, the first sample after touch-down dropped, median of 5 for X/Y,
  a 4 px jitter dead band, and a release only after 60-80 ms without pressure. The XPT2046 runs
  at 2 MHz as a second device on the display's SPI bus; the bus driver serialises it against
  the display's DMA transfers. The filter is portable C and host-tested.
- **Display driver** (`board_display.c`): SPI2 at 40 MHz, two 5-line DMA buffers (9.6 KB), a 6 KB
  LVGL task; colour order and orientation in MADCTL (defaults BGR, landscape; the console's `lcd
  ...` changes them live while bringing a board up). Backlight: 25 kHz PWM, at 0 % from power-up.
- **Calibration**: three targets plus a check tap; coefficients in NVS with a version; runs
  before the setup wizard on a fresh board; from Settings > Display for anyone who can open
  Display; and at start-up when a finger stays on the screen for 3 s (the recovery path when
  a bad calibration makes Settings unreachable).
- **Layout** (built: `components/ui/alarm_ui/layouts/480x320/alarm_ui`, landscape; `components/ui/alarm_ui/layouts/README.md`). The plan was no rail
  and a bottom bar; building it against the UI code changed that, for two reasons: while the
  panel is armed the Disarm pad *is* Home (no X, nothing behind it), so the pad must fit beside a
  navigation that stays visible - Panic has to be one tap away - and a bottom bar left only
  228 px of height for four rows of keys. So: a **72 px rail** (the logo in the state's colour,
  Home, Zones, Activity, Troubles, Panic, each 64 x 48); a **36 px status bar** (the date or a
  banner, Chime as a button, troubles, Wi-Fi, battery, clock, the Settings cog); **Home** = a
  compact state card (ring, icon or seconds left, the state in words) beside three arming tiles;
  the **code pad** fills the view - three columns of **80 x 58 keys** with the action's icon,
  title, hint or dots, options and Silence beside them - and while it has taken Home the status
  bar carries the state and the seconds left, so an exit or entry time is never out of sight;
  **Zones, Activity, Troubles** are one column of rows (lists still scroll; only the trouble
  list pages, 48 rows a page, and paging that suits the 4" is still to do); **Settings** = the eight sections as a row of icon tabs above the page (the page
  title names the section) - a section's code pad takes the whole view, so switching section
  from a locked one is X, then the tab; **sheets** (keyboard with keys about 42 x 46, choices,
  confirmation) and the pad over the screen are full-screen. Every button has a steady pressed
  colour and a key click. Screens: [`screens/`](screens) (from the preview; e.g.
  [Home](screens/home_ready.png), [the pad while arming](screens/pad_arm_away.png),
  [entry time](screens/entry_delay.png), [Settings](screens/settings_display.png),
  [the keyboard](screens/keyboard.png)).
- **Fonts**: body 15 px, labels 16, captions 13, titles 20, headline 26, pad digits 28,
  countdown and clock 56; icons 20 (every glyph), 28 and 40 (only what is drawn that large);
  no 96 px icons.

## Tasks
By phase. Every open box is BL-079 (the 4" port, on hold), except the keypad role (BL-048).

### Resuming (BL-079)
In this order (CONTRIBUTING.md, "Build and test"): undoing the exclusions first would put the
out-of-date layout back into CI and turn it red.
- [ ] First bring the 480x320 layout up to date with the 7"'s UI changes made during the hold
  (`python tools/check_ui_contract.py --all` shows how far it fell behind); read "Risks"
  first (horizontal gradients without RGB888)
- [ ] Then undo the four exclusions of commit 0c54bb3 (2026-09-24): add the E32R40T back to the
  matrix in `.github/workflows/firmware.yml` and the 480x320 steps (layout check, preview and
  smoke test) to `.github/workflows/host-tests.yml` - their comments say how; remove `480x320`
  from `ON_HOLD` in `tools/check_ui_contract.py`; set `"offered": true` in
  `tools/flasher/roboflash/boards.json`

### Phase 0 - Record and prepare (BL-079)
- [x] Identify the board and survey the code (this document)
- [ ] Answer the open questions below
- Dropped: the backup of the board's 4 MB flash before the first flash; it is a
  development board and needs none (AGENTS.md, "Working with a real panel").

### Phase 1 - Split into reusable modules (no behaviour change on the P4) - done 2026-09-23
Carried out as [docs/features/33](../33-Modular-Architecture-and-Boards/README.md) steps 1-10, each
proven on the PC by host tests, the UI preview (screenshots unchanged), a P4 v1 dev build and a
size/RAM map comparison (its step log has the numbers).
- [x] 1.1 `alarm_mem.h` memory policy; PSRAM-only allocations and stacks replaced (33 step 3)
- [x] 1.2 `board_api` contract and `board_info_t`; the CrowPanel implements it; services and the
  UI read pins, terminals and options from it, not from a board (steps 4 and 5)
- [x] 1.3 Optional means not linked: an optional component compiles to nothing or to stubs
  (step 6; the rule is in `alarm_features`' README); `home_control`, `zigbee_host`, `storage`,
  `sdmmc` left `platform_esp`
- [x] 1.4 `ui_backend` header component; `panel_web` stops requiring `alarm_ui` (step 5)
- [x] 1.5 `alarm_ui` feature gating for integrations, notifications + QR, storage and the
  photo saver; Settings rows hidden when absent (step 6)
- [x] 1.6 `panel_ota`: `cp_ota.c` only with ESP-Hosted, device name from the board, partition
  labels from the table (step 8)
- [x] 1.7 `sounder`: rate per board (`board_info_t.audio_rate_hz`), ramps in time; host tests
  at 8, 12, 16, 24 and 32 kHz (step 8, AUD-006)
- [x] 1.8 `panel_storage` split out of `platform_esp` (step 6), SDSPI from `board_info_t`
  (step 8)
- [x] 1.10 `tools/idf.ps1` / `idf.sh`: `-Project e32r40t` without the P4 revision split (step 10)
- 1.9 (UI lists paged instead of one object per row) moved to Phase 3: it is layout work.

### Phase 2 - Board bring-up (`board_e32r40t`, `firmware/e32r40t`) (BL-079)
- [x] Partitions and `sdkconfig.defaults` (esp32, REV_MIN_3, 4 MB DIO 40 MHz, no SPIRAM,
  signed apps + rollback, table at 0xC000), built and flashed 2026-09-23 (33 step 10)
- [x] Headless first: engine, event log, zone sources, RF capture and console without a UI,
  flashed and running on the board 2026-09-23 (commit 1633c0b); its first boot found
  [B009](../../bugs/B009-rf-capture-classic-esp32.md), fixed
- Duplicate of Phase 4 (its Wi-Fi and MQTT box, its OTA box): the rest of the headless box -
  Wi-Fi, MQTT, a signed OTA and a rollback on the board.
- [ ] The sounder on the DAC and FM8002E (CYD-014): `board_audio_init`, `board_audio_write`
  and `board_audio_amp` in `components/boards/board_e32r40t/src/board.c` still answer
  `ESP_ERR_NOT_SUPPORTED` or do nothing
- [x] ST7796S (esp_lcd) with partial LVGL buffers, backlight after the first frame
  (`board_display.c`); flashed 2026-09-23: boots, draws
- [x] XPT2046 driver, filter, calibration: `components/drivers/touch_xpt2046`,
  `components/core/touch_calib` (14 host tests, CYD-008/009), `board_touch.c`; flashed
  2026-09-23: polls with 0 errors
- [x] Status screen when the touch UI does not fit (`components/ui/alarm_ui/src/ui_status.c`,
  started by `panel_app` when free internal RAM is under the UI budget + 64 KB): the state in
  its colour, troubles, clock, a ring where the panel feels a finger; it operates nothing
- [x] RGB LED off by default (`board_power_init` in `components/boards/board_e32r40t/src/board.c`)
- [ ] Battery ADC (IO34) into `power_monitor`: `board_battery_poll` and `board_battery_read` in
  `board.c` are stubs, so the power monitor reports "unknown"
- [ ] If it is wanted: the RGB LED showing the state in a steady colour (never blinking,
  CYD-011); today it is only switched off
- [ ] Decide and build the TLS slot of "Decisions" (CYD-005): on a board without PSRAM, notify,
  the release check and OTA downloads take one short-lived TLS session in turn beside MQTT's
  lasting one; nothing of it is built yet
- [x] CI builds the firmware and reports image size and static RAM (CYD-013):
  `.github/workflows/firmware.yml` + `tools/check_image_size.py` (commit 1633c0b); left out of
  CI since the hold (commit 0c54bb3), the script still runs locally (headroom: "Budgets")

### Phase 3 - The 4" UI (BL-079)
- [x] `components/ui/alarm_ui/layouts/480x320/alarm_ui` LVGL Pro project (30 components, 3
  screens, the 78 subjects of the 7") and its contract check `tools/check_ui_contract.py`
- [x] The trouble list pages (UI-030: `troubles_update` and `subject_troubles_pager` in both
  layouts' `screen_main.xml`)
- [ ] Zones and Activity paged too, with buttons and no scrolling needed, and page sizes the 4"
  can carry (the trouble list's 48 rows are too many objects here) (CYD-010; was Phase 1 step
  1.9; fewer objects on the P4 too, and what resistive touch needs)
- [x] Home, code pad, zones, activity, troubles, emergency, Settings, installer, setup,
  plain screen saver - every page the board's firmware carries, through the one scripted walk
- [x] Touch calibration screens: a three-target overlay on LVGL's top layer, drawn by the board
  (`board_touch.c`), so it works under the status screen and the UI alike; it runs when
  nothing is stored, on `touch cal`, or when a finger is held 3 s in the first 10 s after start
- [ ] Touch calibration from Settings > Display for anyone who can open Display (CYD-008): no
  Settings row starts it yet; the board runs it on first start, on `touch cal` and on a 3 s
  hold after power-up
- [x] UI preview and simulator at 480x320 (`pwsh tools/ui_preview.ps1 -Board e32r40t`, `pwsh
  tools/ui_sim.ps1 -Board e32r40t`); the walk follows the board's options
  (`host/ui_preview/host_features.h`); curated shots in `screens/`
- [x] The UI's 23 KB of working tables come from `ui_slow_alloc()` on first use, from LVGL's
  heap (not allocated at all while the 4" shows the status screen), so the preview counts them
- [ ] **LVGL memory** (found 2026-09-23): the walk needs at most 593 KB of LVGL heap on the PC,
  about 297 KB on the ESP32, against a budget of 96 KB (`CONFIG_ALARM_UI_LVGL_BUDGET_KB`; the
  preview reports it, not yet fatal). The three permanent screens alone take 224 KB on the PC;
  the event log page brings it to 342 KB. Levers: create the setup screen only when needed
  and delete it after; one set of sheets (keyboard, choices, confirmation) on the top layer
  instead of one per screen; paged lists; a lighter saver. Of the UI's 43 KB of static state
  in internal RAM here, the 23 KB of working tables are counted now (box above): 617 KB on the
  PC, about 309 KB on the board. The generated subjects' string buffers (12 KB) remain static.
  Then make the budget check fatal (`host/ui_preview/main.c` only warns today). The free heap
  measured on the board is in "Budgets"

### Phase 4 - Acceptance on the board (only when told to flash) (BL-079)
- [ ] The ST7796S's colour order and orientation confirmed by a person (left open at the first
  flash, 2026-09-23)
- [ ] The three-target calibration run on the board, then touch accuracy over the whole screen
  and the panic hold with a light press (CYD-008, CYD-009)
- [ ] Tones and levels (fire, CO, burglary, chime, exit beeps), once the DAC driver exists
  (CYD-014)
- [ ] Wi-Fi on the board, then MQTT/TLS to a broker, HA discovery (CYD-005)
- [ ] Signed OTA and a forced rollback (CYD-006)
- [ ] 24 h soak with the RAM limits of CYD-012

### Phase 5 - Keypad role (after docs/features/11) (BL-048)
- [ ] Panel-link backend for `ui_backend_t`; native ESP-NOW; pairing screens (CYD-015)

## Open questions
- **Orientation**: landscape 480x320 (like the 7") or portrait
  320x480 (roomier code pad and lists)? The plan assumes landscape until answered; the 480x320
  layout was built landscape (2026-09-23).
- **Role order**: stand-alone panel first, keypad after docs/features/11? (The plan's leaning:
  yes. The panel role exercises the whole module split, and the keypad reuses its UI.)
- **Extras on the 4"**: ntfy/Pushover (RAM permitting), 433 MHz, wired zones on IO35/IO39?
  (Meanwhile `board_desc.c` offers wired terminals on IO39, IO25 and IO32 and a 433 MHz
  receiver's data on IO35; ntfy/Pushover are off in the board's manifest.)
- **Power**: will these boards run with a LiPo? The board can't sense mains loss directly,
  only the battery voltage; AC-loss troubles would be approximate or absent.
- Mounting: behind a case with an opening, or bare? (Affects how hard the resistive film is
  pressed and whether the RGB LED is visible.)
- **Languages** ([docs/features/37](../37-Globalization/README.md)): the plan's leaning is
  Latin-script languages only on the 4" (`ALARM_I18N_WIDE_SCRIPTS` off: no fallback fonts, BiDi or
  input methods). Its Latin-extended fonts and every built-in catalog count against the 85 % slot
  budget, so `ALARM_I18N_LOCALES` may list fewer languages here than on the P4.

## Risks
- **Flash**: 1.6 MB is tight; LVGL widget trimming, the font diet and `-Os` are the levers,
  and CI keeps the number visible (while on hold, only `tools/check_image_size.py` run locally).
- **RAM**: TLS plus LVGL plus Wi-Fi leaves little headroom; the TLS slot, dynamic TLS buffers
  and small draw buffers are the levers; the soak test is the proof.
- **IO12 strapping**: the display/touch MISO is on the flash-voltage strapping pin. The board
  boots correctly as shipped; never burn the flash-voltage eFuse to "fix" it without the panel's owner asking.
- **Calibration drift** on resistive films: the start-up recovery path covers a bad one.
- **RGB888 gradients**: `# CONFIG_LV_DRAW_SW_SUPPORT_RGB888 is not set` stops LVGL drawing
  horizontal gradients (it blends them from an RGB888 line; LVGL warns "Not supported source color
  format"). The screen saver's shade draws nothing, which is invisible while it is black; turn
  RGB888 back on if a photo or another horizontal gradient comes. The local preview
  (`-Board e32r40t`) builds LVGL from this manifest's LVGL lines (docs/features/33).