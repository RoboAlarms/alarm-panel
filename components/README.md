# Components

Every piece of the panel is a component in one of six tiers. A firmware for a board is put
together from the components that board can carry (docs/features/33, ADR-034). Dependencies only
point down the table below; `tools/check_architecture.py` checks it in CI.

| Tier | What lives here | May depend on | Must not |
|---|---|---|---|
| [`core/`](core) | portable C11: the alarm logic and everything else that can be proven on a PC | core | include ESP-IDF, FreeRTOS or LVGL headers; allocate; keep globals |
| [`drivers/`](drivers) | ESP-IDF drivers for chips any board may carry | ESP-IDF, core | know a board, a service or the UI; use LVGL |
| [`boards/`](boards) | one component per board, the board contract (`board_api`), and the family libraries the boards on one main board share (`<name>_family`) | ESP-IDF, drivers, core, LVGL; a board its family | depend on services or the UI; reach a family from anywhere but its boards |
| [`services/`](services) | ESP-IDF services that don't care which board they run on | ESP-IDF, core, drivers, `board_api`, other services | name a board component; depend on the UI; use LVGL |
| [`ui/`](ui) | the touch UI's logic, portable (LVGL + core) | LVGL, core | include ESP-IDF or reach a service |
| [`app/`](app) | the start-up every board shares | everything above, `board_api` | name a board component |

A firmware sees the board contract and its own board component only, and that board's family
library when it has one (`cmake/alarm_idf.cmake`, `ALARM_BOARD_FAMILY`): another board's
component, and the registry packages it declares, are never part of its build. Boards on one
main board share everything but their description and wiring through the family library, and
a board's firmware can be built on another's (`ALARM_BOARD_BASE`, `firmware/README.md`).

Options follow the tiers too: only board components read `CONFIG_BOARD_*`, and board
components don't read the app's `CONFIG_ALARM_*`. There are no exceptions; should one ever be
needed, it goes in `KNOWN_EXCEPTIONS` in `tools/check_architecture.py` with the step that
removes it.

## Index

| Component | Tier | What | Features |
|---|---|---|---|
| [`alarm_core`](core/alarm_core) | core | the alarm engine: arming, zones, alarms, troubles, users, keypad sequences, configuration blob | [01](../docs/features/01-Core-Alarm-Engine/README.md) |
| [`alarm_proto`](core/alarm_proto) | core | MQTT JSON, Home Assistant discovery, store-and-forward, HA link frames, firmware-update helpers | [10](../docs/features/10-MQTT-and-Home-Assistant/README.md), [13](../docs/features/13-OTA-Updates/README.md), [23](../docs/features/23-Home-Assistant-Integration/README.md) |
| [`annunciator`](core/annunciator) | core | the voice's request policy: what the panel says, and when - chime, walk test, arming, armed, disarmed, entry, not ready; one active and four waiting, priorities, expiry and cancellation (D04/D05/D11) | [08](../docs/features/08-Audio-and-Annunciation/README.md) |
| [`automation`](core/automation) | core | the built-in automations: the model and its stored record, the store over a key-value port (`auto_store`), the observer of engine events, the local clock's crossings and the runner (never disarm, unlock or open) | [41](../docs/features/41-Automations/README.md) |
| [`backup`](core/backup) | core | the backup file: its records, its password-based encryption (PBKDF2, AES-256-GCM) over a crypto port, what it holds and the full check before a restore | [14](../docs/features/14-Configuration-and-Installer-Programming/README.md) |
| [`board_info`](core/board_info) | core | a board described in data (chip, screen, pins, terminals, SD, RF, radio) and the check that refuses a description that could harm the board | [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |
| [`event_log`](core/event_log) | core | power-fail-safe event log ring on raw flash | [05](../docs/features/05-Event-Log/README.md) |
| [`panel_link`](core/panel_link) | core | the panel link between a master and its keypads: crypto on a three-primitive port, the v1 frame and replay window, messages, pairing and sessions, the master's and the keypad's link managers, the stored records, Smart Home's device list and records for keypads | [11](../docs/features/11-Panel-Link-Master-Keypad/README.md) |
| [`h264_sps`](core/h264_sps) | core | the remote keypad's H.264 key pictures rewritten to show at once in a browser (no reordering, BT.601) | [31](../docs/features/31-Browser-Panel-Manager/README.md) |
| [`http_redirect`](core/http_redirect) | core | where a plain-HTTP request is sent: the same page over HTTPS, the Host repeated only when it is plainly a host | [31](../docs/features/31-Browser-Panel-Manager/README.md) |
| [`home_control`](core/home_control) | core | the devices registry (lights, locks...) and Home Assistant as a device source | [24](../docs/features/24-Home-Control/README.md) |
| [`pack_store`](core/pack_store) | core | the pack store: the table of blobs (language packs) in the assets partition - format 1, mount, first fit, power-safe commits | [37](../docs/features/37-Globalization/README.md) |
| [`i18n`](core/i18n) | core | translation lookup with context, CLDR plural rules, the `{n}` formatter, UTF-8 helpers, regions, dates and numbers; the catalogs `tools/i18n.py` builds | [37](../docs/features/37-Globalization/README.md) |
| [`notify`](core/notify) | core | the event sentences (one copy for the screen and phones), categories, request builders | [26](../docs/features/26-Notifications/README.md) |
| [`power_monitor`](core/power_monitor) | core | mains and battery supervision | [17](../docs/features/17-Power-and-Supervision/README.md) |
| [`rf_decode`](core/rf_decode) | core | 433 MHz pulse decoders (EV1527 / PT2262, HT6P20B, HT12E, KeeLoq, arctech): de-glitch filter, strict framing, consistency checks, encoders for the tests and simulator | [25](../docs/features/25-RF-Sensors-315-345-433/README.md) |
| [`rtc_clock`](core/rtc_clock) | core | the DS3231 clock module as data (BCD time, identification, coin-cell set-up) and the rules for which clock the panel believes | [35](../docs/features/35-Battery-Backed-Clock/README.md) |
| [`cc1101_regs`](core/cc1101_regs) | core | the CC1101 as data: the OOK receive table per band, frequency word, RSSI, genuine / clone / absent, read-back and reset detection | [25](../docs/features/25-RF-Sensors-315-345-433/README.md) |
| [`sirens`](core/sirens) | core | every siren's policy (which alarms, which partitions, fire before CO before burglary), the wired outputs with their temporal patterns, chirps and bell supervision, and the 433 MHz sirens the panel transmits to | [16](../docs/features/16-Outputs-Siren-and-Relays/README.md), [25](../docs/features/25-RF-Sensors-315-345-433/README.md) |
| [`sounder`](core/sounder) | core | the sounder's patterns, synthesizer and mixer: what the panel sounds like | [08](../docs/features/08-Audio-and-Annunciation/README.md) |
| [`voice_pack`](core/voice_pack) | core | the voice pack reader: RVOC format 1 (header, index, words, Opus packets), its full check, the clip IDs and a spoken name's 10-second limit | [08](../docs/features/08-Audio-and-Annunciation/README.md) |
| [`storage`](core/storage) | core | SD card policy: format protection, media states, what a busy card refuses | [29](../docs/features/29-Storage-and-SD-Card/README.md) |
| [`touch_calib`](core/touch_calib) | core | resistive touch: calibration (raw -> screen, three-target solve) and filtering (pressure, median, settling, release debounce, dead band) | [19](../docs/features/19-CYD-ESP32-Port/README.md) |
| [`ui_backend`](core/ui_backend) | core | the contract between the touch UI and whatever runs it (`ui_backend_t`); the Panel Manager uses it too | [07](../docs/features/07-UI-LVGL-Pro/README.md), [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |
| [`ui_remote`](core/ui_remote) | core | the touch UI's backend over a byte channel: a keypad's proxies for its alarm panel's pages, the alarm panel serving them under the keypad's grant, the calls' frozen ids and records | [11](../docs/features/11-Panel-Link-Master-Keypad/README.md) |
| [`zigbee_host`](core/zigbee_host) | core | bringing up the ESP32-H2 sensor radio | [20](../docs/features/20-ESP32-H2-Sensor-Radio/README.md) |
| [`zigbee_zones`](core/zigbee_zones) | core | the panel's Zigbee devices beside the zones: the device registry and install codes, each device's setup, the Zigbee pages' view and requests, events and storage (`zbz_panel`), IAS WD sirens and IAS ACE keypads and fobs; contracts only until ZB-M2 | [20](../docs/features/20-ESP32-H2-Sensor-Radio/README.md) |
| [`zone_hub`](core/zone_hub) | core | zone sources: the hub, bindings, simulator, wired, Home Assistant, Zigbee, 433 MHz and Z-Wave logic | [22](../docs/features/22-Zone-Sources/README.md) |
| [`zwave_host`](core/zwave_host) | core | the Z-Wave host: the link to the ZAC93 (start-up with the module checks, recovery, retry schedule), the Serial API transport and request scheduler, the S2/S0 crypto on a three-primitive port; a fake ZAC93 for tests | [36](../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) |
| [`zwave_proto`](core/zwave_proto) | core | Z-Wave on the wire: Serial API frames and message codecs, the command classes a sensor panel speaks (codecs, encapsulation, answer matching, the panel's answers), CRC-16, DSK text | [36](../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) |
| [`zwave_zones`](core/zwave_zones) | core | Z-Wave devices as zones: the glue between `zwave_host`'s network and `zone_hub`'s Z-Wave source - device facts from the node cache, reports decoded, Supervision and the panel's answers, devices asked for their state, proof of life; and `zwz_panel`, the panel's Z-Wave as `zwave_service` runs it (the pages' view and requests, the logged events, what is stored) | [36](../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) |
| [`rf_rx`](drivers/rf_rx) | drivers | the CC1101 over SPI and RMT capture of a receiver's data line | [25](../docs/features/25-RF-Sensors-315-345-433/README.md) |
| [`rtc_ds3231`](drivers/rtc_ds3231) | drivers | a DS3231 clock module on an I2C bus: probe and register reads/writes | [35](../docs/features/35-Battery-Backed-Clock/README.md) |
| [`touch_xpt2046`](drivers/touch_xpt2046) | drivers | the XPT2046 resistive touch controller on an SPI bus: one poll of raw samples | [19](../docs/features/19-CYD-ESP32-Port/README.md) |
| [`board_api`](boards/board_api) | boards | the board contract: the functions every board implements, and `board_info()` | [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |
| [`crowpanel_p4_family`](boards/crowpanel_p4_family) | boards | the CrowPanel Advanced ESP32-P4 family library: the board contract for every board on Elecrow's P4 main board (DSI display with flicker guard, GT911 touch, I2S audio with an ES8311 codec where one answers, battery) | [06](../docs/features/06-CrowPanel-P4-Platform/README.md) |
| [`board_crowpanel_p4`](boards/board_crowpanel_p4) | boards | Elecrow CrowPanel Advanced 7" (V1.1) on the family library: its description and wiring | [06](../docs/features/06-CrowPanel-P4-Platform/README.md) |
| [`board_crowpanel_10`](boards/board_crowpanel_10) | boards | Elecrow CrowPanel Advanced 10.1" (V1.2) on the family library: its description and wiring (socket UART 27/28, J7 53/54; the ES8311 codec its schematic draws, where one answers) | [06](../docs/features/06-CrowPanel-P4-Platform/README.md) |
| [`board_e32r40t`](boards/board_e32r40t) | boards | LCDWiki 4.0" ESP32-32E display (classic ESP32, no PSRAM): ST7796S display under LVGL, XPT2046 touch with its three-target calibration, backlight, bring-up console (`lcd`, `touch`); audio and battery to come | [19](../docs/features/19-CYD-ESP32-Port/README.md) |
| [`board_esp32s3_devkitc`](boards/board_esp32s3_devkitc) | boards | ESP32-S3-DevKitC-1 (N16R8) as a panel without a screen: its description, the BOOT button for setup recovery, the add-on I2C bus | [42](../docs/features/42-Headless-Panel-ESP32-S3/README.md) |
| [`alarm_features`](services/alarm_features) | services | the feature menu every firmware shares (Kconfig only): which optional parts a board carries | [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |
| [`alarm_mem`](services/alarm_mem) | services | the one memory placement policy: PSRAM when the chip has it, internal RAM otherwise | [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |
| [`espnow_bridge`](services/espnow_bridge) | services | C6 ESP-NOW radio and bounded Hosted peer-data bridge | [12](../docs/features/12-C6-Coprocessor-Firmware/README.md) |
| [`outputs_service`](services/outputs_service) | services | the wired outputs on the board's terminals and the 433 MHz sirens: their task, store and the `out` console command (`ALARM_OUTPUTS`) | [16](../docs/features/16-Outputs-Siren-and-Relays/README.md) |
| [`platform_esp`](services/platform_esp) | services | what every panel runs: alarm task, NVS store, event log writer, prefs, Wi-Fi and time, zones task, sounder task, console | [05](../docs/features/05-Event-Log/README.md), [09](../docs/features/09-WiFi-Time-and-Provisioning/README.md), [22](../docs/features/22-Zone-Sources/README.md) |
| [`backup_service`](services/backup_service) | services | the backup file on the panel: made from NVS and restored into it through the policy, sealed with PSA, `backup check` | [14](../docs/features/14-Configuration-and-Installer-Programming/README.md), [31](../docs/features/31-Browser-Panel-Manager/README.md) |
| [`lang_packs`](services/lang_packs) | services | language packs: the pack store on `assets`, signed packs loaded into PSRAM, the registry the screen and the Panel Manager read | [37](../docs/features/37-Globalization/language-packs.md) |
| [`panel_voice`](services/panel_voice) | services | the voice: the Opus decoder (micro-opus, pinned) and its adapter `voice_codec`, shared with the PC builds; the voice pack's loading, decoding and the announcements (BL-202) | [08](../docs/features/08-Audio-and-Annunciation/voice-first-build.md) |
| [`panel_storage`](services/panel_storage) | services | the optional microSD card and the flash accounting of the Storage page | [29](../docs/features/29-Storage-and-SD-Card/README.md) |
| [`panel_clock`](services/panel_clock) | services | the optional battery-backed clock module: the time at start-up, kept in step with the internet, set by hand | [35](../docs/features/35-Battery-Backed-Clock/README.md) |
| [`rf_service`](services/rf_service) | services | the 433 MHz receiver (CC1101 or a module) as a zone source | [25](../docs/features/25-RF-Sensors-315-345-433/README.md) |
| [`zigbee_service`](services/zigbee_service) | services | the ESP32-H2 sensor radio as a zone source: the coordinator on Espressif's Zigbee stack (in every P4 image, ADR-047; bench acceptance pending) | [20](../docs/features/20-ESP32-H2-Sensor-Radio/README.md) |
| [`zwave_service`](services/zwave_service) | services | Z-Wave sensors through a Zooz ZAC93 on a UART: the link and network tasks, NVS, the PSA crypto port, the `zwave` console, the touch UI's `zw_*` calls (`ALARM_ZSRC_ZWAVE`, on in the P4's manifest; the radio stays off until a place is chosen) | [36](../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) |
| [`automation_service`](services/automation_service) | services | the built-in automations on the panel: the store in NVS, the save lock, what the touchscreen and the Panel Manager read and edit | [41](../docs/features/41-Automations/README.md) |
| [`device_service`](services/device_service) | services | the Devices registry and Home Assistant as its source | [24](../docs/features/24-Home-Control/README.md) |
| [`net_mqtt`](services/net_mqtt) | services | MQTT client over TLS: availability, replay, discovery, commands | [10](../docs/features/10-MQTT-and-Home-Assistant/README.md) |
| [`net_notify`](services/net_notify) | services | sends notifications to ntfy and Pushover | [26](../docs/features/26-Notifications/README.md) |
| [`ha_link`](services/ha_link) | services | the local Home Assistant integration API (HACS) | [23](../docs/features/23-Home-Assistant-Integration/README.md) |
| [`plink_service`](services/plink_service) | services | the panel link on a panel: the role, the UDP link task, the master's listener and worker, the keypad's UI backend, sounds, time and update gate, the `plink` console | [11](../docs/features/11-Panel-Link-Master-Keypad/README.md) |
| [`panel_ota`](services/panel_ota) | services | signed firmware updates, rollback, release check, serial upload, C6 updates | [13](../docs/features/13-OTA-Updates/README.md) |
| [`panel_web`](services/panel_web) | services | the panel-hosted HTTPS Panel Manager; its pages and the PC mock in `web/`, their Node tests in `test/` | [31](../docs/features/31-Browser-Panel-Manager/README.md) |
| [`dev_remote`](services/dev_remote) | services | enrolled developer access: network flashing, diagnostics, screen | [30](../docs/features/30-Remote-Development/README.md) |
| [`ui_port_esp`](services/ui_port_esp) | services | the touch UI's backend on ESP-IDF: `ui_backend_t` filled from the services | [07](../docs/features/07-UI-LVGL-Pro/README.md), [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |
| [`alarm_ui`](ui/alarm_ui) | ui | the touch UI's logic (Home, Settings, installer, setup) and its pictures; portable | [07](../docs/features/07-UI-LVGL-Pro/README.md) |
| [`panel_app`](app/panel_app) | app | the panel's start-up, shared by every board; a firmware's `app_main()` fills its hooks | [33](../docs/features/33-Modular-Architecture-and-Boards/README.md) |

`python tools/check_architecture.py --list` prints each component's project dependencies;
`--graph` prints them as a Mermaid diagram.

## A component's folder

```
<tier>/<name>/
  CMakeLists.txt      ESP-IDF component; core components also build as a plain CMake library
                      for the PC (the else() branch), see core/alarm_core/CMakeLists.txt
  README.md           purpose, tier, public API, dependencies, options, rules, tests
  include/            public headers (core components: include/<name>/...)
  src/                sources and private headers
  test/               host tests (Unity): CMakeLists.txt calling alarm_add_test(); tests of
                      the component's other files too (panel_web's Node tests)
  Kconfig             options, when it has any
  <assets>/           files the component embeds, beside what is generated from them
                      (ui/alarm_ui/images/, services/panel_web/web/)
```

Every component follows it: nothing but build files, `Kconfig` and the README sits beside
`CMakeLists.txt`, and `tools/check_architecture.py` fails on a `.c` or `.h` there (ARCH-006).
Assets keep their own folder (`ui/alarm_ui/images/`, with the C files generated from them).
`storage` and `power_monitor` still keep their public header straight in `include/`.

## Adding a component
1. Pick the lowest tier it can live in. Logic that can be written without ESP-IDF belongs in
   `core/`, even when only one board uses it today: it is then tested on the PC and reusable.
2. Create the folder as above. For a core component, copy the CMake pattern of an existing one
   (`if(ESP_PLATFORM) idf_component_register(...) else() add_library(...) endif()`).
3. Tests: `test/CMakeLists.txt` with `alarm_add_test(<suite> SOURCES test_x.c LIBS <name> unity)`
   (see `cmake/alarm_host.cmake`); the host build finds it, nothing else to register. Tag tests
   with the requirement IDs of the Features doc.
4. Write its `README.md` and add a row to the index above.
5. If it is optional, give it a Kconfig option (app-level switches go in
   `services/alarm_features`). Requirements can't follow options in ESP-IDF, so handle "off" in
   the sources: either the whole source inside `#if CONFIG_<option>` with callers guarding their
   calls, or stubs that answer "not in this firmware" so callers need no guard. Say which in the
   README (see `services/alarm_features/README.md`).
6. Run `python tools/check_architecture.py` and the host tests.
