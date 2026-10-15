# device_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[24 Devices](../../../docs/features/24-Home-Control/README.md) · **Tests:** the registry and the
Home Assistant device source are host-tested ([home_control](../../core/home_control/test));
this service is checked on a panel with the console's `devices` command

The Devices registry (`components/core/home_control`) on the panel and Home Assistant as its
device source: devices the paired Home Assistant allows for control appear on the panel;
unlocking and opening ask for a user code first, checked by the engine (HCTL-010). `ha_link`
feeds it. With Z-Wave built (`ALARM_ZSRC_ZWAVE`), the panel's own switches, dimmers and garage
doors are a source too (`dsrc_zw`, docs/features/41 DESIGN section 4 "Z-Wave (phase B)"),
registered before Home Assistant's so its long list can't crowd them out:

- **Feed.** `device_service_tick` (the health task, once a second) reads
  `zwave_service_ha_devices` and `zwave_service_link_up` outside the device lock, and gives the
  registry a new list (begin/add/end) only when the list or the link changed.
- **Commands.** Posted, never waited for: `zwave_service_output_post` queues the command for the
  zwave task (a full queue or no radio fails it at once) and the task's answer comes back once,
  by request id. `received_ms` is when the act was asked (`device_service_act` /
  `_act_auto`), so `ZWO_STALE_MS` refuses a command the zwave task reaches too late. A garage
  moves through `zwz_panel_garage_move`, whose output layer never presses blind.
- **Smart Home** doesn't show them yet (their tiles need their own mockup, BL-169 B3):
  `ui_port_esp`'s `dev_list` leaves out `DSRC_ZWAVE` rows; automations list them.

Split out of `platform_esp` in docs/features/33 step 6.

## Public API
| Header | What it offers |
|---|---|
| `include/device_service.h` | `device_service_start()`, `device_service_tick()`, the Home Assistant feed (`device_service_ha_link/_ha_catalog_begin/_ha_state/_ha_result`, `device_service_set_sender()`), `device_service_list()`, `device_service_get()` (one device, for the touch UI's port), `device_service_act()`, `device_service_act_auto()`, `device_service_console_register()` (the `devices` command) |

## Depends on
- Project components: public `home_control`; private `platform_esp`, `zone_hub`, `alarm_mem`,
  `alarm_proto`, `rtc_clock` (the clock floor `CLOCK_EPOCH_MIN_S`, so a link drop's "Offline
  since" is stamped only while the clock is set), `zwave_service` (used only with
  `ALARM_ZSRC_ZWAVE`)
- ESP-IDF: private `console`

## Options
| Kconfig (`Kconfig`) | Default | Effect |
|---|---|---|
| `ALARM_HOME_CONTROL` | y (needs `SPIRAM` and `ALARM_FEATURE_HA` or `ALARM_ZSRC_ZWAVE`; Z-Wave alone needs no Home Assistant link) | The Devices registry; off, the panel has no Devices screen and the service answers with stubs |

## Resources
- The registry (about 35 KB), the Home Assistant device source (about 15 KB, only with
  `ALARM_FEATURE_HA`) and the Z-Wave source with its two list copies (about 12 KB) prefer slow
  memory and fall back to internal (`alarm_mem_prefer_slow_calloc`), which is why the option
  needs PSRAM.

## Rules
- Security classes decide what needs a code (docs/features/24); the engine checks the code.
- General screens never name Home Assistant (CONTRIBUTING.md, "Rules for the UI").
- Off means stubs: callers need no guard (components/services/alarm_features).
- Lock order: the device lock is never held while zwave_service takes the zone lock, and the
  zwave task answers a posted command with no lock of its own held.
