# panel_app

**Tier:** app (the shared start-up) · **Features:**
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 7), [13 OTA](../../../docs/features/13-OTA-Updates/README.md) (the health heartbeats) ·
**Tests:** every firmware build; on a panel, the boot log and the console's `health`,
`display` and `power`

The panel's start-up, shared by every board. A firmware's `app_main()` fills a
`panel_app_config_t` and calls `panel_app_run()`; everything happens in one order for every
board: storage and networking stacks, the board's power and audio, the event log and the
alarm engine (the panel protects from there on), the zone sources from `board_info()` (RF pins
validated against the board), firmware updates with the board's probation mask, the display
and the touch UI, the health heartbeats, the network, the integrations and the console. Two
rules run through it: nothing is ever erased to get past a failure (R06), and a start-up that
can't finish brings the console up and restarts, which lets an update on probation roll back
(R07).

It names no board: board-specific parts come through the board contract (`board_api.h`) and
the hooks. Before docs/features/33 step 7 this was `firmware/crowpanel_p4/main/app_main.c`.

## Public API
| Header | What it offers |
|---|---|
| `include/panel_app.h` | `panel_app_config_t` (model names for the integrations, the probation mask, hooks: `log_chip`, `before_engine`, `extend_backend`, `power_report`, `console`, `started`), `panel_app_run()`, `panel_app_power_status()` |

## Depends on
- Project components: public `alarm_features`, `power_monitor`, `ui_backend`; private
  `alarm_core`, `alarm_mem`, `alarm_ui`, `board_api`, `device_service`, `ha_link`, `net_mqtt`,
  `net_notify`, `panel_ota`, `panel_storage`, `platform_esp`, `rf_service`, `ui_port_esp`,
  `zigbee_service`, `zone_hub`
- ESP-IDF: `console`, `esp_driver_spi`, `esp_event`, `esp_netif`, `esp_partition`, `esp_timer`,
  `lvgl__lvgl`, `nvs_flash`

## Options
It follows the feature options (`components/services/alarm_features`): `ALARM_FEATURE_UI`
(off: headless, no display or LVGL use), `_MQTT`, `_NOTIFY`, `_HA`, and the zone-source options
of `rf_service` and `zigbee_service`; `ALARM_SERVICE_CONSOLE` for the console.

## Resources
- Task `health`: 4096 bytes, slow (`alarm_mem_task_create()`), priority 3, every 2 s: the log
  and zone-source heartbeats, the battery poll, a posted engine heartbeat, device timeouts.
- An LVGL timer for the UI and touch heartbeats (with the UI).
- The console's `display stress` writes to a partition named `fw_cache` when the board has one.

## Rules
- The order is part of the design: the event log before the engine (its numbers continue
  after the newest record), the engine before anything that could fail, the console last. A
  change to the order is reviewed against R05-R08 and R18 (see the comments in `panel_app.c`).
- Nothing after `alarm_service_start()` aborts: a part that fails is logged and lived without.
- Hooks are for what one board has; anything a second board needs belongs in a component.
