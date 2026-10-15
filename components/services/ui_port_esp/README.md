# ui_port_esp

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[07 UI](../../../docs/features/07-UI-LVGL-Pro/README.md) (ADR-012),
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 5) · **Tests:** the UI side is proven by the PC preview against the simulated backend;
this glue is checked on the panel (every Settings page it serves)

The touch UI's backend on ESP-IDF: it fills `ui_backend_t` (`components/core/ui_backend`)
from the panel's services, so the UI component itself never sees a service. Status, commands,
codes and installer mode go to `alarm_service` (the local touchscreen is keypad 0); the event
log to `panel_log`; Wi-Fi, addresses and time to `net_wifi`; the connection check to
`net_check`; the MQTT and Home Assistant pages to `net_mqtt` and `ha_link`; notifications to
`net_notify`; updates to `panel_ota`; preferences to `panel_prefs` and the sounder; versions
and the factory reset to `panel_system`; zone sources to `zone_service`; storage to
`panel_storage`. Calls block the LVGL task until the alarm task has answered; the alarm task
never waits for LVGL, so this can't deadlock. The Panel Manager wraps the same backend.

Before docs/features/33 step 5 this was `components/ui/alarm_ui/port/`.

## Public API
| Header | What it offers |
|---|---|
| `include/ui_port_esp.h` | `ui_port_esp_backend()` (everything except the backlight, which the app sets from the board), `ui_port_esp_options()` (stored preferences, and whether the setup wizard must run) |

## Depends on
- Project components: `ui_backend` (public); `alarm_core`, `platform_esp`, `ha_link`,
  `net_mqtt`, `net_notify`, `panel_ota`, `automation_service` (with
  `CONFIG_ALARM_FEATURE_AUTOMATIONS`)
- ESP-IDF: through those services

## Options
The firmware's feature options decide which entries are filled:
`CONFIG_ALARM_FEATURE_MQTT`, `CONFIG_ALARM_FEATURE_NOTIFY`, `CONFIG_ALARM_FEATURE_HA`
(`firmware/<board>/main/Kconfig.projbuild`). Leaving an entry NULL hides its page.

## Rules
- Only the alarm task touches the engine: everything here goes through `alarm_service`
  (CONTRIBUTING.md, "Rules for the firmware").
- LVGL calls from other tasks take `board_ui_lock()`; this file makes none of its own.
- Planned (docs/features/33 step 6): require the optional services only when their feature is on,
  so a board without them links none of their code.
