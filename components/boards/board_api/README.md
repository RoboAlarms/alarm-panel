# board_api

**Tier:** boards (the board contract) · **Features:**
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 4, ADR-035) · **Tests:** each board's `test/` validates its description; the contract
itself is checked by every board compiling against it

The functions every board component implements, so the app can bring any board up the same
way: power, backlight, display into LVGL, the UI lock, touch, audio, battery, statistics, and
`board_info()`, the board described in data (`components/core/board_info`). Exactly one board
component is linked into a firmware. A board's extras (the CrowPanel's I2C bus and frame
capture) stay in that board's own header.

It is the one boards-tier component other tiers may name (`tools/check_architecture.py`).
Services don't need it at all: they receive a `board_info_t` or plain pins from the app.

## Public API
| Header | What it offers |
|---|---|
| `include/board_api.h` | `board_info`, `board_name`, `board_power_init`, `board_backlight_init/set`, `board_display_init`, `board_display_stats` (+ `board_display_stats_t`), `board_ui_lock/unlock`, `board_touch_init`, `board_touch_stats` (+ `board_touch_stats_t`), `board_audio_init/write/amp`, `board_battery_poll/read` |

## Depends on
- Project components: `board_info` (the description), `power_monitor` (`power_sample_t`)
- ESP-IDF: `lvgl__lvgl`, `esp_common`, `freertos`

## Options
None. Boards define their own (for example `CONFIG_BOARD_BACKLIGHT_FORCE_OFF`,
`CONFIG_BOARD_FLICKER_GUARD`, `CONFIG_BOARD_FRAME_CAPTURE` on the CrowPanel).

## Rules
- The backlight stays off until a first full frame is on the panel, and nothing may flash;
  every board keeps that promise its own way (CONTRIBUTING.md, "Display safety").
- Touch is polled from the LVGL read callback on every board so far (the GT911's interrupt
  mode lost releases).
- Statistics a board can't measure stay 0; the console's `display` command prints them all.
- A new board implements all of it; a board without a battery or audio returns "none" rather
  than leaving a function out, so the app never needs to know which board it runs on.
