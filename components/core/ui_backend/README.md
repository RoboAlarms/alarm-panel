# ui_backend

**Tier:** core (portable C11, header only) · **Features:**
[07 UI](../../../docs/features/07-UI-LVGL-Pro/README.md) (ADR-012),
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 5) · **Tests:** [`test/`](test/) (4 suites: `ui_backend`, `ui_dev_model`, `ui_voice`,
`ui_prefs`)

What the touch UI needs from the panel, as data types and one struct of function pointers,
`ui_backend_t`. The UI (`components/ui/alarm_ui`) talks to nothing else, so whatever fills the
struct runs it: the panel's services on ESP-IDF (`components/services/ui_port_esp`), a panel
link to a master for a remote keypad (docs/features/11, planned), or the simulated world of the PC
preview and simulator (`host/ui_preview/host_panel.c`). The browser Panel Manager
(`components/services/panel_web`) wraps the same backend, which is why the contract lives
here and not in the UI: the web server needs it without LVGL.

A NULL entry hides what depends on it, so a board that leaves a feature out simply doesn't
fill that entry.

## Public API
| Header | What it offers |
|---|---|
| `include/ui_backend/ui_backend.h` | `ui_backend_t`; the types its entries use (network, Wi-Fi scan, connection check, power, MQTT, Home Assistant link, notifications, firmware update, zone sources, about, Z-Wave: the radio's status, devices, a device's page, requests and an operation's progress - docs/features/36 screens.md §12; Smart Home: `ui_dev_t` (with a thermostat's `modes`: unknown 0, the sheet shows no mode keys; what it is doing, `hvac_action`; `unsure`: a failed action's outcome isn't known), `dev_list`, `dev_act` (ACT_SET_MODE takes a `ui_dev_state_t` in `value`), `dev_layout_get/_set` - docs/features/24, BL-045); `ui_options_t` (the appearance among them: `UI_APPEARANCE_*`, `high_contrast` - BL-132) and `ui_options_defaults()`; a change of the options from elsewhere, `ui_prefs_patch_t`, which `prefs_remote_take` hands the screen and `prefs_remote_done` answers (docs/features/07 UI-046) |
| `include/ui_backend/ui_prefs.h` | A change of the Screen and sound settings from elsewhere (UI-046; docs/features/11 "Settings fields"): the settings ids `UI_PREFS_F_*`, `UI_PREFS_LOOK` and the refusals `UI_PREFS_WHY_*` (alarm_proto's `PROTO_PREFS_*` numbers, asserted), `UI_PREFS_SHOWN` (the UI's own: what the screen holds for `UI_PREFS_LOOK_MS` - the look, the three screen-off and saver delays, the clock's format), `ui_prefs_valid()` (the wire's ranges), `ui_prefs_get/_put()`, `ui_prefs_merge()` (the fields a patch names, never the partition, the language or the region) and `ui_prefs_refuse_all()`; the mailbox's bookkeeping, `ui_prefs_box_submit/_take/_done/_cancel()` (one patch at a time, BUSY after `UI_PREFS_TAKE_MS`), used by `platform_esp`'s `panel_prefs_remote_*` and the PC's `host_panel.c` |
| `include/ui_backend/ui_light.h` | How lit the screen is (UI-009, UI-043: `ui_light_night()`, `ui_light_stage()`, `ui_light_percent()`) and the palette the appearance asks for (UI-010: `ui_light_theme()`, Automatic following the night hours) |
| `include/ui_backend/ui_dev_model.h` | The Smart Home screen's layout (Favorites, the tiles' order and sizes) as a stored blob - `ui_devl_encode()`, `ui_devl_decode()`, its defaults and `ui_devl_page()` - and `ui_dev_state_parse()` for a source's state words; the one conversion both screens use (docs/features/11, BL-216):`ui_dev_parts_t` and `ui_dev_from_parts()` (a device's parts as the `ui_dev_t` the screen shows), `ui_dev_parts_hc()` (the master's parts, a registry row) and `ui_dev_mode_bit()` (a mode key's `ui_dev_state_t` as its `DEV_MODE_*` bit). The layout is a stored format: version 1's frozen fixture is `test/ui_dev_layout_fixture_v1.h` (never edit it) |

## Depends on
- Project components: `alarm_core`, `event_log`, `home_control` (`dev_kind_t`, `dev_action_t`), `notify`, `zone_hub`, `storage` (types only), `automation` (the rule, status and save answers - docs/features/41); its `ui_prefs` test also links `alarm_proto`, whose numbers `ui_prefs.h` copies
- ESP-IDF: none (portable)

## Options
None.

## Rules
- Portable: no LVGL, no ESP-IDF, no service headers. The test compiles the header alone with
  the portable components' warning set to keep it that way.
- Adding an entry: optional entries may be NULL and the UI must hide what they drive; say in
  the comment which Settings page uses it.
- Who may open a page follows docs/features/14 regardless of which entries a backend fills.

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite with `ctest --test-dir build/host -R ui_backend`.
- The headers build alone, portably; a zeroed backend is all NULL; the keypad's default
  preferences; the screen's light and night hours; the appearance's palette (Automatic, high
  contrast, an unknown appearance read as Dark); the Z-Wave contract's zero values (an idle operation, a request that asks for
  nothing) and its security classes.
- `ui_dev_model`: the layout blob and its frozen fixture, the defaults and Arrange's edits, the
  state words and values; `ui_dev_from_parts()` byte for byte the master's own conversion from
  before BL-216 over a fixture table (every kind, online and offline, idle, pending, failed,
  unsure, every action), the same tile from a link keypad's parts, and `ui_dev_mode_bit()`
  giving the words the screen sent before (HCTL-006, HCTL-012, HCTL-013, LINK-048).
- `ui_voice`: the touchscreen's voice approval against the engine and the pack (D07).
- `ui_prefs` (UI-046, LINK-051, LINK-052): the ids, the look's fields, the why numbers and every
  range are `proto_prefs.h`'s; the fields held for 2 s are exactly those whose change moves
  the backlight or the palette (`ui_light.h`, over sampled values, idle times and minutes), and the
  clock's format; the screen's defaults read as the panel's (`test_prefs.c`'s CRC);
  a merge writes the fields named and nothing else, refuses a value out of range alone and reports
  only real changes; the mailbox holds one patch, is BUSY while one waits or is taken, withdraws
  one not taken within 3 s (at 3000 ms, across a clock wrap), lets the submitter cancel only
  before the take, answers each ticket once and never hands out ticket 0.
- The UI's behaviour against a backend is proven by the UI preview (`host/ui_preview`), which
  drives the real UI through the simulated backend (a change from elsewhere: `prefs_smoke`).
