# platform_esp

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [05 Event log](../../../docs/features/05-Event-Log/README.md), [08 Audio](../../../docs/features/08-Audio-and-Annunciation/README.md), [09 Wi-Fi and time](../../../docs/features/09-WiFi-Time-and-Provisioning/README.md), [22 Zone sources](../../../docs/features/22-Zone-Sources/README.md) ·
**Tests:** the logic these services wrap is portable and host-tested in its own component: [alarm_core](../../core/alarm_core/test), [event_log](../../core/event_log/test), [zone_hub](../../core/zone_hub/test), [sounder](../../core/sounder/test). On the PC, `host/ui_preview/host_panel.c` stands in for these services in the preview and the simulator. On a panel it is checked with the service console (`status`, `zones`, `zone`, `terminals`, `sources`, `events`, `wifi`, `ip`, `netcheck`, `memory`; `help` lists all).

The ESP-IDF services every panel runs around the portable core: the alarm task that owns the
engine, NVS storage of the configuration and runtime state, code hashing, the event log on
flash, this keypad's preferences, the sounder task, Wi-Fi, time and addresses (through the C6
on the P4, natively elsewhere), the connection check, the zones task (the hub with the
simulator, wired and Home Assistant sources), system functions and the serial console. It holds
no alarm behaviour of its own: decisions stay in `alarm_core`, `zone_hub`, `sounder` and the
other core components. It draws nothing, knows no board, and starts nothing by itself: the
firmware's `app_main.c` passes pins and terminals in (from `board_info()`) and decides what
starts.

The optional parts that used to live here are their own components since docs/features/33 step 6:
[`rf_service`](../rf_service), [`zigbee_service`](../zigbee_service),
[`device_service`](../device_service) and [`panel_storage`](../panel_storage).

| File | What it does |
|---|---|
| `alarm_service.c` | The alarm task: owns the engine, serialises inputs, commands and queries, persists state, publishes snapshots, calls listeners |
| `alarm_store.c` | NVS namespace `alarm`: the configuration blob and runtime state; factory configuration (demo zones) only when positively absent |
| `code_hash.c` | PBKDF2-HMAC-SHA256 for user codes on PSA SHA-256, with a fault counter the engine checks |
| `panel_log.c` | The event log on the `eventlog` partition: a queue fed by the alarm task, a writer task, lost-report handlers, loss counters in NVS `log` |
| `panel_prefs.c` | Display and sound preferences (NVS `ui`) and the setup state (NVS `sys`); what is stored, kept in RAM by every save for any task (`panel_prefs_seq()`, `panel_prefs_current()`) and as settings fields (`panel_prefs_to_values()`: the master's report to Home Assistant, a keypad's PREFS); the one-patch mailbox that hands a change from elsewhere to the screen's task (`panel_prefs_remote_*`, docs/features/07 UI-046; its bookkeeping is `ui_backend`'s host-tested `ui_prefs_box_*`) |
| `sounder.c` | The task that renders `sounder`'s mixer in 10 ms blocks through the board's sink, and the amplifier; any task may post a sound, `sounder_play_background` one heard only over silence (the arming warning's chime, docs/features/41 D22); in dev builds the bench silence (`sounder_set_silenced`, NVS `sounder`/`silent`, the console's `speaker`, docs/features/08 AUD-025) keeps the amplifier off, alarms included |
| `net_wifi.c` | Wi-Fi station (`esp_wifi_remote` over ESP-Hosted on the P4, native elsewhere), SNTP, time zone, fixed address, trial settings (NVS `net`, `nettrial`), the network, co-processor and clock troubles |
| `net_check.c` | "Check the connection": Wi-Fi, address, router, lookup, internet, time, service, on a short-lived task |
| `zone_service.c` | The zone hub on its own task and lock, the binding blob (NVS `zones`/`bind`), the simulator, wired and Home Assistant sources; other sources register through `zone_service_register()` |
| `panel_system.c` | Version information and the factory reset |
| `panel_console.c` | The serial service console (UART or USB-Serial-JTAG): status, zone simulator, virtual keypad, events, network; other components register their own commands |

## Public API
| Header | What it offers |
|---|---|
| `alarm_service.h` | Listeners, start, inputs (zone events, troubles, check-ins), synchronous commands, codes, users, installer mode, configuration, snapshots, `alarm_service_call/post()` onto the alarm task; `alarm_service_set_dormant()` for a panel in the keypad role (no engine: inputs dropped, calls answered at once, docs/features/11) |
| `alarm_store.h`, `code_hash.h` | Load and save configuration and runtime state; code hashing and its fault counter |
| `panel_log.h`, `panel_prefs.h`, `panel_system.h`, `sounder.h` | Event log read and flush, lost-report handlers; preferences (and, for any task, their counter, RAM copy and the mailbox a change from elsewhere goes through: `panel_prefs_remote_submit()`, `_cancel()` for the submitter, `_take()` and `_done()` for the screen's port; docs/features/11 "Settings over the link"); about and factory reset; sounds |
| `net_wifi.h`, `net_check.h` | Wi-Fi, addresses, trial settings, time zone, NTP (and switching it off, `net_wifi_set_time_server`), scan, network requirement, the low-latency holds (`net_wifi_hold_low_latency`); the connection check |
| `zone_service.h` | Zone hub access, bindings, source registration and the Home Assistant feed |
| `panel_console.h` | The console |

## Depends on
- Project components: public `alarm_core`, `alarm_proto` (`panel_prefs.h`'s settings fields), `event_log`, `zone_hub`, `sounder`; private `alarm_mem`, `i18n`, `ui_backend`.
- ESP-IDF: public `esp_event`, `esp_netif`; private `spi_flash`, `app_update`, `bootloader_support`, `console`, `esp_app_format`, `esp_driver_gpio`, `esp_driver_uart`, `esp_partition`, `esp_timer`, `esp_wifi`, `lwip`, `mbedtls`, `nvs_flash`; `espressif/esp_wifi_remote` `~1.6.4` and `espressif/esp_hosted` `~3.0.9` on the ESP32-P4 only (`idf_component.yml`).

## Options
| Kconfig (`Kconfig`) | Default | Effect |
|---|---|---|
| `ALARM_DEMO_ZONES` | y (off in release builds) | Eight simulator zones in the factory configuration, driven from the service console |
| `ALARM_ZSRC_SIM` / `ALARM_ZSRC_WIRED` | y / y | The simulator source (console) and the wired terminals |
| `ALARM_ZSRC_HA` | y (needs `ALARM_FEATURE_HA`) | Home Assistant entities as zones, fed by `ha_link` |
| `ALARM_SERVICE_CONSOLE` / `ALARM_CRYPTO_DIAGNOSTICS` | y / n | The console (off in `sdkconfig.defaults.release`); a development-only crypto fault test |
| `ALARM_TASK_PRIORITY` / `ALARM_EVENT_RING` | 10 / 100 | Alarm task priority; recent events kept in RAM |

## Resources
| Task | Stack | Priority | Where |
|---|---|---|---|
| `alarm` | 8192, internal | `ALARM_TASK_PRIORITY` | `alarm_service.c` |
| `zones` | 6144, internal | `ALARM_TASK_PRIORITY` - 1 | `zone_service.c` |
| `log` / `net` | 4096, internal | 3 / 5 | `panel_log.c`, `net_wifi.c` |
| `netchk` (per check; plus an esp_ping task of 3072) | 5120, internal | 4 | `net_check.c` |
| `sounder` | 4096, slow (`alarm_mem_task_create()`) | 6 | `sounder.c` |
| console REPL | 6144 | ESP-IDF default | `panel_console.c` |

- Slow memory (`alarm_mem`, PSRAM on the P4): the engine, the snapshot and the event ring
  (`alarm_service.c`), the Wi-Fi scan tables (`net_wifi.c`), the Home Assistant source
  (`zone_service.c`); the zone hub prefers slow memory and falls back to internal.
- Partitions: `eventlog` (`panel_log.c`), `nvs`.

## Rules
- Only the alarm task touches the engine; listener callbacks are quick, never block and never call the synchronous `alarm_service_*` functions (`alarm_service.h`; CONTRIBUTING.md, "Rules for the firmware").
- A task whose stack is in PSRAM must never write flash or NVS; the tasks above that write keep internal stacks. Anything an ISR or DMA touches stays internal (CONTRIBUTING.md).
- Nothing is ever erased or rewritten to recover from a failure that was not diagnosed (`alarm_store.h`); losing an event-log record is a defect (LOG-010).
- `zone_service`'s binding blob and `zsrc_*` numbers are stored in NVS: never renumber (CONTRIBUTING.md, `zone_hub` rules).
- It names no board: pins and terminals come from the app (`board_info()`), docs/features/33.
