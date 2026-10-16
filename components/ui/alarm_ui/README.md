# alarm_ui

**Tier:** ui (portable LVGL logic) · **Features:** [07 UI with LVGL Pro](../../../docs/features/07-UI-LVGL-Pro/README.md), [14 Configuration and installer programming](../../../docs/features/14-Configuration-and-Installer-Programming/README.md#who-can-change-what) (who can change what) ·
**Tests:** the PC builds compile the same logic against the real engine: `pwsh tools/ui_preview.ps1` ([host/ui_preview](../../../host/ui_preview)) renders every screen to `build/ui_preview/shots` and fails when the scripted walk gets stuck or a check reports the wrong thing (it carries `UI-NNN` checks and runs in CI); `ui_words_test` (same folder) reads Home's headline, detail and note and checks `ui_text.c`'s tones, glyphs and zone card (`KPD-004`); `prefs_smoke` (same folder) hands the running UI changes of its settings from elsewhere (`UI-046`); `pwsh tools/ui_sim.ps1` ([host/ui_sim](../../../host/ui_sim)) is the interactive simulator. CI also runs `lvpro_gen.py --check`, `embed_image.py --check` and `embed_logo.py --check`. On a panel: the `display` / `display watch <s>` console commands and `tools/panelctl.py screen`.

The panel's touch UI: Home, the code pad, the zone, activity and trouble lists, the emergency
buttons, the screen saver, Settings, installer mode and the first-boot setup. The logic in
`src/` reads status snapshots, derives the view model (the subjects of the LVGL Pro layout)
and turns taps into engine commands, all on the LVGL thread. It reaches the panel only through
`ui_backend_t`; a NULL entry hides what depends on it, which is how integrations switched off
in the build disappear from the screen. The same `src/` runs on the panel and in the PC preview
and simulator. The contract is `components/core/ui_backend`; its ESP-IDF implementation is
`components/services/ui_port_esp` (it was `port/` here until docs/features/33 step 5). The event sentences
("Burglary alarm" / "Front Door") are not here: their single copy is `notify`'s
`notify_words.c`, shared with phone notifications.

| Module (`src/`) | What |
|---|---|
| `ui_app.c` | Start-up, the tick, snapshots, Home, the code pad, the lists, emergency buttons, the screen saver |
| `ui_forms.c` | Settings rows, sheets (text entry with the on-screen keyboard, choices, confirmation) |
| `ui_settings.c` | Settings sections and who may open them, the page stack, display and sound (and a change of them from elsewhere: `prefs_remote_tick`, docs/features/07 UI-046 - in use, held, the voice's readiness, what the screen shows - the look, the screen-off and saver delays, the clock's format - at most once per 2 s), users, About |
| `ui_installer.c` | Installer mode: options, zones, system name, installer and master codes, setup, factory reset |
| `ui_sources.c` | Where a zone's sensor is: source picker, live terminal / learn list, source rows |
| `ui_net.c` | Wi-Fi, address settings and the connection check, date and time; the setup's network steps |
| `ui_integrations.c` / `ui_notify.c` | Integrations: the Home Assistant link and MQTT; ntfy (with its QR code) and Pushover |
| `ui_update.c` / `ui_storage.c` | About > Software update; About > SD card |
| `ui_zwave.c` | Installer > Z-Wave (docs/features/36 screens.md): the installer menu's row and the Z-Wave page - the radio, the devices, the network's settings; a page table the add, device, remove, region and radio pages slot into (Z7b). Only with `ALARM_UI_WITH_ZWAVE` |
| `ui_walk.c` | The walk test (docs/features/40): Settings > Walk test (start it, follow it, its list of zones, the summary after it), Home's card while one runs (who started it when that was elsewhere, the end warning), the status bar's walk test banner, the installer menu's Walk test |
| `ui_wizard.c`, `ui_tz.c` | First-boot setup (with the panel link: the role, and a keypad's Wi-Fi and pairing steps); the time zone table |
| `ui_link.c` | The panel link (docs/features/11, BL-048): Installer > Keypads and devices' keypads on Wi-Fi (adding one with the code compared on both screens, a keypad's page, removing it), and a panel in the keypad role: Home while it connects, without the alarm panel or not paired, Emergency's words, About's rows, Make it a stand-alone panel, the home partition. Read through `ui_backend_t.link_*`; nothing shows without them |
| `ui_saver.c` | The screen saver picture, decoded once with LVGL's TJPGD |
| `ui_status.c` (`include/ui_status.h`) | The status screen a board shows when the full UI doesn't fit in its RAM: state, troubles, clock, a ring under the finger; operates nothing |
| `ui_text.c` / `ui_text.h` | Icons and tones, the zone card, the words only this screen uses; wraps `notify`'s sentences |
| `ui_internal.h` | State and helpers shared by the modules (`UI_SLOW_BSS`, `ui_slow_alloc()`, `ui_settings_allowed()`, ...) |

The layout is one LVGL Pro project per screen size, `components/ui/alarm_ui/layouts/<WxH>/alarm_ui` (the board's manifest
names it; `components/ui/alarm_ui/layouts/README.md` has the contract every layout keeps) (XML source, generated `*_gen.c/.h`,
fonts, `file_list_gen.cmake`, and the hand-written callbacks `alarm_ui.c/.h`). `images/` holds
the pictures that are not icons (`saver_forest.jpg`, the `roboalarms_mark.pgm` rail mark) and
their generated C (see [images/README.md](images/README.md)). `sources.cmake` lists the logic
and image sources once; this component's `CMakeLists.txt` and `cmake/alarm_ui.cmake`
(`alarm_add_ui_library(LAYOUT <WxH>)`, the PC builds) both read it.

## Public API
| Header | What it offers |
|---|---|
| `include/ui_app.h` | `ui_app_start()`, `ui_app_default_options()`, `ui_app_apply_options()`, `ui_app_run_setup()`, `ui_dev_control_begin/end()` (with `ALARM_DEV_REMOTE` or `ALARM_PANEL_WEB`), `ui_memory_diagnostic_step()` (with `ALARM_CRYPTO_DIAGNOSTICS`); it includes the contract, `ui_backend/ui_backend.h` |
| `alarm_ui.h`, `alarm_ui_gen.h` (layout folder) | The generated screens, components and subjects |

## Depends on
- Project components: `ui_backend` (the contract), `alarm_core`, `event_log`, `notify`, `zone_hub`, `storage`. No service: `tools/check_architecture.py` keeps it that way.
- ESP-IDF: `lvgl/lvgl` `~9.5.0` from the component registry (`idf_component.yml`). Public definition `LV_LVGL_H_INCLUDE_SIMPLE`; private `UI_SUBJECT_STRING_LENGTH=128`.

## Options
None of its own. It follows: `ALARM_FEATURE_MQTT`, `_NOTIFY`, `_HA` (the backend fills those
entries only when on, see `components/services/ui_port_esp`); `ALARM_ZSRC_ZWAVE` (the Z-Wave
pages, `ALARM_UI_WITH_ZWAVE`; on in the P4's firmware since docs/features/36 Z9, the PC builds add
them with `-ZWave`); `ALARM_DEV_REMOTE ||
ALARM_PANEL_WEB` (remote control hooks, `ui_app.c`); `ALARM_CRYPTO_DIAGNOSTICS` (the diagnostic
page walk, which the app calls only in dark builds). It needs LVGL's `LV_USE_TJPGD` and `LV_USE_QRCODE`, set in
`firmware/crowpanel_p4/sdkconfig.defaults` and `host/ui_preview/lv_conf.h`.

## Resources
- No task of its own: everything runs on the board's LVGL render thread. Backend calls block that thread until the alarm task answers; the alarm task never waits for LVGL (`components/services/ui_port_esp`).
- The UI state uses `UI_SLOW_BSS` (PSRAM on the panel, an ordinary static on the PC); the big working tables only the LVGL thread uses (trouble lines, activity entries, form rows, the log page: 23 KB) come from `ui_slow_alloc()` on first use and are kept - PSRAM on the panel, and on a board without PSRAM never allocated while only the status screen runs (`src/ui_internal.h`).
- The screen saver picture is decoded once into a 1.2 MB RGB565 image in LVGL's heap (PSRAM on the panel); without the memory the saver stays plain (`src/ui_saver.c`). The JPEG and the 1,936-byte A8 mark live in flash.

## Rules
- The XML in each `components/ui/alarm_ui/layouts/<WxH>/alarm_ui` is the source: never hand-edit `*_gen.c/.h`, `file_list_gen.cmake` or `fonts/*_data.c`; run `python tools/lvpro_gen.py components/ui/alarm_ui/layouts/<WxH>/alarm_ui` (`--fonts` after font changes). Every layout offers the same subjects, component APIs and looked-up names (`tools/check_ui_contract.py`); geometry the code needs comes from layout constants. Colours bound with `bind_style` must not also be local styles. Pictures go through `tools/embed_image.py` (CONTRIBUTING.md, "Rules for the UI").
- `src/` stays portable (LVGL and core components); the platform is reached only through `ui_backend_t`. LVGL calls from any other task take `board_ui_lock()` (CONTRIBUTING.md).
- Who may open a Settings page follows docs/features/14: check with `ui_settings_allowed(ACCESS_...)` and show `ui_settings_locked()` otherwise; homeowner settings never need installer mode.
- String subjects hold at most 127 bytes; the sheets' note 191 (`UI_SHEET_NOTE_LENGTH`, two joined sentences). General screens never name Home Assistant: only Integrations, the integration's page and a zone's source picker / Source row.
- Display safety: nothing flashes, strobes or blinks; live words change at most once a second and only when settled (UI-020, UI-023). No flow may need a physical button (CONTRIBUTING.md, "Display safety").
- After a UI change, look at the preview PNGs; adding a `shot()` renumbers the later ones, so refresh `docs/features/07-UI-LVGL-Pro/screens/` with it.
- Planned (docs/features/33 step 6): feature pages compile only with their feature, Settings rows follow capabilities, and a minimal-feature host build passes its preview walk.
