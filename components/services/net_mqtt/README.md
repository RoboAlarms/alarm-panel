# net_mqtt

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [10 MQTT and Home Assistant](../../../docs/features/10-MQTT-and-Home-Assistant/README.md), [13 OTA updates](../../../docs/features/13-OTA-Updates/README.md) (the update entity) ·
**Tests:** the messages, discovery, command parsing and store-and-forward are portable and live in `alarm_proto`: [test_proto.c](../../core/alarm_proto/test/test_proto.c) (golden payloads, `MQTT-NNN`), [test_report.c](../../core/alarm_proto/test/test_report.c) (replay from the event log) and [test_fw.c](../../core/alarm_proto/test/test_fw.c) (the update entity, `OTA-010`). This component has no host test of its own. On a panel: the `mqtt` console command (`mqtt help`). docs/features/10 records it as built but not yet run against a broker.

The panel's MQTT client. It connects to a broker (plain, TLS with the certificate bundle, or
TLS with an owner-supplied CA), announces availability with a last will, keeps partition, zone
and trouble state retained, publishes reportable events from the event log with
store-and-forward, executes commands and announces the panel to Home Assistant by MQTT
discovery. While the native Home Assistant link (`ha_link`) is paired, discovery is suppressed
and retracted so the panel does not appear twice (HAI-010). It builds no payload itself: every
topic and JSON body comes from `alarm_proto`. It raises `comm_fail` when reports cannot be
delivered, and it tells `net_wifi` that the network is needed while MQTT is on.

## Public API
| Header | What it offers |
|---|---|
| `include/net_mqtt.h` | `net_mqtt_listener()` (register before `alarm_service_start()`), `net_mqtt_start()`, settings get/set (the password is never returned), `net_mqtt_get_status()`, `net_mqtt_set_ca()`, `net_mqtt_suppress_discovery()`, the Home Assistant update entity (`net_mqtt_set_firmware()`, `net_mqtt_firmware_changed()`), the Zigbee network (`net_mqtt_set_zigbee()`: `~/zigbee/state` when it changes, at most once a second, its discovery sensor, the battery and signal of Zigbee-fed zones; docs/features/20 devices.md §15), `net_mqtt_console_register()` |

## Depends on
- Project components: `platform_esp` (`alarm_service` for snapshots, reports and commands; `panel_log` for replay and the lost-report handler; `net_wifi` for the network requirement), `alarm_proto` (topics, payloads, discovery, the strict JSON reader, `proto_report` store-and-forward), `alarm_mem` (slow memory for the big buffers), `alarm_core` types through `platform_esp`.
- ESP-IDF: `console`, `esp_app_format`, `esp_timer`, `esp-tls`, `mbedtls`, `nvs_flash`; `espressif/mqtt` `^1.1.0` from the component registry (`idf_component.yml`; ESP-IDF 6 no longer ships esp-mqtt).

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_FEATURE_MQTT` (`firmware/crowpanel_p4/main/Kconfig.projbuild`) | y | Off: `net_mqtt.c` compiles to nothing, `app_main.c` neither registers the listener nor starts the client, and `ui_port_esp.c` leaves the MQTT backend entries out, which hides the page |

Everything else (broker, port, user, TLS mode, discovery, commands, panic commands, topic
prefix, panel id) is a runtime setting in NVS, changed on the screen, in the Panel Manager or
with `mqtt` on the console.

## Resources
- Task `mqtt`: 8192-byte internal stack, priority 4 (`net_mqtt.c:1222`, `TASK_STACK` at `:51`). It owns the client; the API only takes the lock and notifies it.
- espressif/mqtt's own task: 6144-byte stack, priority 5 (`net_mqtt.c:885`). Buffers of `PROTO_PAYLOAD_MAX` (4096 bytes) each way and an outbox of at most 64 KiB (`:882-884`); keepalive 30 s (`:879`).
- Slow memory (`alarm_mem`, PSRAM on the P4): a payload scratch buffer and a copy of the alarm configuration (`net_mqtt.c:1207-1208`), the status snapshot (`ALARM_SLOW_BSS`, `:118`). A loaded CA (at most 4096 bytes, `CA_MAX` at `:50`) is ordinary heap.
- Queues: acks 32, commands 4, live reports 16 (`net_mqtt.c:1204-1206`).
- At most one TLS client session. mbedTLS allocates from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` in `sdkconfig.defaults`).
- NVS namespace `mqtt` (`net_mqtt.c:48`): settings, CA, delivered position, discovery bookkeeping.

## Rules
- Topics, JSON keys and discovery ids are an external contract (docs/features/10): never rename, only add, and change them in `alarm_proto` with its golden payloads. Incoming JSON goes through the strict reader only (CONTRIBUTING.md, `alarm_proto` rules).
- Only the alarm task touches the engine: commands go through `alarm_service_command()` as a remote source; the listener callbacks only queue and never block (`alarm_service.h`).
- The `mqtt` task writes NVS, so its stack stays in internal RAM (CONTRIBUTING.md, "Rules for the firmware").
- Codes from commands are cleared after use and never logged; passwords and CA contents are never printed.
- Planned (docs/features/33 step 6): the firmware requires this component only when `ALARM_FEATURE_MQTT` is on.
