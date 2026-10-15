# 06 · CrowPanel Advanced 7" platform (ESP32-P4 BSP)

**Status:** Implemented v1 - the board support, the shared start-up and the platform services,
on a CrowPanel 7" V1.1 (P4 silicon v1.3) since 2026-09-19: display, touch, Wi-Fi through the C6
and the console proven there; the v3-silicon image builds but has not run on a board. Left: the
rest of the hardware acceptance (the 24 h soak among it) and the fault injections of P4-013..018
(BL-032). The 10.1" V1.2, the same board with a bigger screen, runs the same firmware with the
same features since 2026-10-10 (P4 v1.3); its update channel and tools are BL-209 · **Milestone:** M2 · **Depends on:** 01, 08, 09, 12

**Code:** `components/boards/crowpanel_p4_family` (what both boards share),
`components/boards/board_crowpanel_p4`, `components/boards/board_crowpanel_10`,
`components/app/panel_app`, `components/services/platform_esp`, `firmware/crowpanel_p4`,
`firmware/crowpanel_10`

## Goal
A clean board-support component (`components/boards/board_crowpanel_p4`) and an ESP-IDF v6.1
application skeleton that bring up the display, touch, backlight, audio and Wi-Fi (through the
C6) on the CrowPanel Advanced 7" **rev V1.1**, and host the alarm engine in its own task.

## Hardware facts (V1.1)
Sources: `docs/hardware/CrowPanel7inScreen/WikiPage.html`; Elecrow repo
`Eagle_SCH&PCB/1.2/readme.md` (hardware driver guide); the V1.1 ESP-IDF Wi-Fi lesson `sdkconfig`.

| Block | Part | Connection (ESP32-P4 GPIO) |
|---|---|---|
| SoC | ESP32-P4NRW32 (32 MB PSRAM), 40 MHz + 32.768 kHz crystals | - |
| Flash | W25Q128JVSIQ, 16 MB | dedicated pins |
| Display | 7" IPS 1024x600, **EK79007**, MIPI-DSI 2 lanes | DSI dedicated; backlight PWM **GPIO31** (LEDC, 30 kHz); GPIO29 backlight power path is NC; LCD reset (GRB, active low) **GPIO41**, 2K pull-up to VDDPST_5 (LDO4) |
| Touch | **GT911** | I2C0 SDA **45**, SCL **46** (400 kHz, shared with I2C_OUT), RST **40**, INT **42**; address 0x5D or 0x14 |
| Wi-Fi/BLE | **ESP32-C6-MINI-1-N4** (4 MB), ESP-Hosted over **SDIO slot 1, 4-bit** | CLK **18**, CMD **19**, D0 **17**, D1 **16**, D2 **15**, D3 **14** (V1.1 order), C6 enable **GPIO32 (high = C6 runs; ESP-Hosted from 3.0.8 resets it that way)**, 40 MHz, RX streaming mode |
| Audio out | 2x **NS4168** I2S amps + speakers | LRCLK **21**, BCLK **22**, DATA **23**; amp enable **GPIO30 (active low)** |
| Microphone | PDM mic | CLK 24, DATA 26 (not used yet) |
| SD card | microSD, SDMMC slot 0, 1-bit (docs/features/29; application support built, bench acceptance pending, BL-034) | CLK **43**, CMD **44**, D0 **39** (the schematic differs; use the code values); no GPIO38 power control on this board, it is UART RX |
| UARTs | UART0 debug (CH340K) 37/38; UART1 connector RX48/TX47 (also routed toward the C6); UART3-IN connector RX33/TX34 (5 V side, level shifted) | - |
| Expansion | J7 2x12 header: GPIO 2, 3, 4, 5, 25, 27, 28, 49-52, 3.3 V/5 V/GND (`BOARD_J7_GPIOS`; the V1.1 schematic: 53/54 are the wireless socket's UART, and board V1.2 swaps them with 27/28) | 3.3 V logic, not 5 V tolerant. Wired zone terminals since M11 (`BOARD_WIRED_TERMINALS`, [22](../22-Zone-Sources/README.md)): GPIO 2, 3, 4, 5 and 49-52, a contact between a GPIO and GND, internal pull-up, a pin set up only when a zone uses it. GPIO 25 is the P4's USB D+ (unused: the console is on UART0) and 27/28 become the socket's UART on a V1.2 board, so they are not offered as terminals. Places for add-ons, chosen at runtime: a CC1101 on 49-52 and 5 (`rf place j7`, docs/features/25), the clock module on 27/28 (`rtc place j7`, docs/features/35), the Z-Wave radio's UART on 27/28 (`zwave place j7`, docs/features/36) |
| Wireless socket | SPI 6/7/8 (+ IO9/IO10) for LoRa/nRF24 modules; UART 53/54 for the ESP32-H2 (docs/features/20); the module's IO12/IO14 sit on the touch bus (45/46) | default wiring for a 433 MHz CC1101 (SPI 6/7/8, CSn 9, GDO0 10) or receiver module (DATA 10), docs/features/25; not yet wired |
| Power | USB 5 V, Li-ion via TP4059 charger, STC8H1K08 aux MCU (charge LEDs, battery ADC) | I2C 0x2f on GPIO45/46; native battery reads verified (docs/features/17) |
| Buttons | BOOT (download mode / general input), RESET | - |

Power budget: 8-10 W with the screen on. Use a USB port or supply that can deliver it: with
a computer on UART0, Elecrow's advice is a supply on the USB2.0 port as well. Each 5 V input
(UART0, USB2.0, UART3-IN) has its own diode, and J7's 5V pins are the switched 5 V output, not
an input ([17](../17-Power-and-Supervision/README.md) "Power inputs").

## The 10.1" (V1.2)
The CrowPanel Advanced 10.1" is the 7"'s main board behind a bigger glass: the same SoC, the same
1024x600 EK79007 timing, touch, C6, SD card, STC8 and sound lines. So the board support is one
family library (`components/boards/crowpanel_p4_family`) and each board keeps only its
description and wiring (`board_crowpanel_p4`, `board_crowpanel_10`); `firmware/crowpanel_10` is
built on `firmware/crowpanel_p4` (`firmware/README.md`, "A board built on another") and uses the
7"'s 1024x600 layout. Sources: Elecrow's 10.1" repository (`docs/hardware/README.md`): the V1.2
schematic, read net by net, and the V1.2 ESP-IDF lessons; and the board itself.

| Block | Against the 7" V1.1 |
|---|---|
| Wireless socket | its UART on GPIO 27 (to the module) and 28 (from it): V1.2 swapped them with 53/54 |
| J7 | GPIO 2, 3, 4, 5, 25, 49-54, in another pin order (`tools/flasher/roboflash/boards.json`); the same wired terminals (2-5, 49-52) and CC1101 place; the clock module's and the Z-Wave radio's J7 place on 53/54 |
| I2C connector | J13, HY2.0 (SCL, SDA, 3V3, GND): I2C0 behind the BSS138 level shifters, with the STC8 |
| Sound | plain I2S into the amplifier, enable GPIO30 low, as on the 7": Elecrow's V1.2 lessons drive it so, and the board answers no codec. The schematic also draws an ES8311 (I2C0 0x18, MCLK GPIO24) into an NS4263B and marks the NS4168s not fitted; the family sets the codec up where one answers (`CONFIG_BOARD_CROWPANEL_P4_ES8311`) and otherwise lets GPIO24 go. It plays on a 10.1-inch panel (2026-10-14); which amplifier is fitted is not verified |
| Microphone | a PDM microphone on CLK 24, DATA 26 in Elecrow's lessons (not used) |
| GPIO29 | a 10K pull-up and the gate of a backlight power switch that is not fitted: never driven |
| USB serial | the same CH340K (1A86:7522) as the 7": RoboAlarms names its board (`dev describe`); on a new panel the Flasher asks which it is (docs/features/34 FLS-005) |

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| P4-001 | ESP-IDF **v6.1** project `firmware/crowpanel_p4` builds from a clean checkout with pinned components (`dependencies.lock` committed) | CI (`.github/workflows/firmware.yml`, v1 and v3), local build |
| P4-002 | Two silicon variants: `sdkconfig.defaults.p4rev1` (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`) and default v3.x; firmware reports which it is | boot log |
| P4-003 | Display: EK79007 via `esp_lcd_ek79007`, 2 lanes @ 900 Mbps, DPI 51 MHz (55.9 frames/s), RGB565, timings HBP160/HPW70/HFP160, VBP23/VPW10/VFP12; one DPI frame buffer in PSRAM, LVGL draws partial areas into two PSRAM draw buffers, copied by DMA2D | HIL (steady 55.9-56.0 frames/s) |
| P4-004 | Backlight off until the first frame; 0-100 % brightness; screen-saver dimming API | HIL |
| P4-005 | GT911 touch via `esp_lcd_touch_gt911`, trying both addresses; **polled** by LVGL's input timer (about 50 reads a second), never read on the INT interrupt (see findings); scroll starts after 20 px so taps stay taps | HIL: presses and releases pair up, 0 I2C errors |
| P4-006 | Audio: I2S1 TX 32 kHz 16-bit stereo, about 32 ms of DMA buffering; amp stays disabled until silent frames are flowing (no pop) and switches off after 1.5 s of silence | HIL |
| P4-007 | Wi-Fi through ESP-Hosted 3.x with the V1.1 SDIO pin map; C6 enable on GPIO32 (high runs it, see findings); C6 firmware version logged | HIL: transport, version 2.12.3, Wi-Fi init; joined a network on the touchscreen, time synced (2026-09-19, "Hardware acceptance") |
| P4-008 | Alarm task owns the engine; engine callbacks post to queues; tick scheduled at `next_deadline` | code review + HIL |
| P4-009 | NVS for config/runtime state; runtime state is written once after each engine call that changed it (only arming, bypass, alarm memory and chime change it, so writes are rare) | code review + HIL |
| P4-010 | Serial console (UART0) for diagnostics and the zone simulator; `CONFIG_ALARM_SERVICE_CONSOLE` turns it off for release builds (it has no code protection: it sits behind the case, and release builds leave it out) | HIL |
| P4-011 | Boot to "Ready to arm" in < 5 s (excluding Wi-Fi association) | HIL: engine running 0.9 s after reset, UI 1.1 s |
| P4-012 | **No flicker, ever** (photosensitivity safety): the DSI frame DMA keeps running during flash writes (`CONFIG_LCD_DSI_ISR_CACHE_SAFE=y`, never turn it off); every frame is timed, and the flicker guard (`CONFIG_BOARD_FLICKER_GUARD`) turns the backlight off at the first late frame until the refresh has been steady for 10 s | HIL `display stress`: 0 late frames in 45 s of flash writes with full-screen redraws (before the fix: 178 late frames in 20 s) |
| P4-013 | **A write that failed is never reported as durable.** `alarm_service` tracks the revision the engine has reached apart from the revision NVS holds; a failed configuration or runtime write stays outstanding, is retried with a 1 s-to-60 s backoff (never a busy loop), coalesces to the newest state, and raises `ALARM_TRBL_STORAGE` until it lands. The engine's state is never rolled back because flash refused - a disarmed panel stays disarmed. `alarm_service_store_status()` reports it | code review; flash fault injection still to do |
| P4-014 | **No factory reset to recover from a failure we could not diagnose.** Only a positively absent NVS key writes the factory configuration; "no memory", "NVS not mounted" and read errors leave the stored installation alone and keep `alarm_service_start()` from starting (it retries the read 3 times first), and a stored configuration the engine rejects is replaced in RAM only. Configuration is never handed to the engine before it is known valid | code review; allocation fault injection still to do |
| P4-015 | **Code hashing fails closed without latching transient failures.** Every PSA result is checked and operations aborted on exit. Startup requires the known-answer self-test. Later failures increment `hash_faults` but retain readiness; the engine returns `unavailable` before comparison, lockout or credential storage (CORE-USR-024). Temporaries use `mbedtls_platform_zeroize`. | Host fault tests + COM4 `crypto_test 100`: 75 injected clone failures, every next hash recovers; no sustained heap loss |
| P4-016 | **The panel never erases the owner's installation to get past a failure.** `nvs_flash_init()` returning `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` no longer erases the default partition; both leave codes, users, zone bindings, preferences and network settings on flash and are reported instead. Erasing is `factory_reset confirm` - something the owner asks for, with the consequences written out first | code review; NVS fault injection and a downgrade run still to do |
| P4-017 | **A start-up that cannot finish says so and does not carry on.** Storage that cannot be opened, or an alarm engine that refuses to start, brings up the service console (so the fault can be read and a factory reset is possible), leaves everything stored exactly as it is, and restarts after 2 minutes - which is also what lets a firmware update still on probation roll itself back | code review; bench run still to do |
| P4-018 | **Each part of the panel keeps proving it works.** The alarm task, the event log's writer, the zone hub's task, the LVGL task and the GT911 poll each report progress at least every 5 s; `panel_ota` requires all five (OTA-005) and reports any that goes quiet at any time, not only on probation. Sensor input going quiet raises the same "sensors may not be heard" trouble a lost source link raises. `health` on the console shows it, with no need for anyone to look at the screen | code review; sensor/touch fault injection still to do |
| P4-019 | **The panel is started and stopped in its datasheet's order** (EK79007, docs/bugs/B023): a hardware reset (GRB low 2 ms, then 60 ms) once the DSI lanes are idle; the backlight off first thing in `app_main` (a panic restart keeps the PWM running), and on from dark only over a picture drawn since it was asked for and after 200 ms of video; before every planned restart the backlight off, black, sleep-in with video still running, then a reset pulse, leaving GRB high for a firmware that doesn't drive it | HIL on a panel (2026-10-04, self-check build): 11 console restarts and 6 USB resets, the panel asleep before each restart, the frame buffer always right, 0 late frames; the backlight from dark after the redraw plus 3 frames, or 100 ms with nothing to redraw. The glass: a person's look (BL-160) |

## Design

### Components and versions (pin in `idf_component.yml`)
| Component | Version (Sept 2026) | Use |
|---|---|---|
| ESP-IDF | v6.1 | framework |
| `espressif/esp_hosted` | 3.0.x (3.0.9; at least 3.0.8, findings 1 and 8) | Wi-Fi/BLE through the C6 |
| `espressif/esp_wifi_remote` | 1.6.x | `esp_wifi` API on the P4 |
| `espressif/esp_lcd_ek79007` | 2.0.x | panel driver |
| `espressif/esp_lcd_touch_gt911` | 1.2.x | touch |
| `espressif/esp_lvgl_port` | 2.9.x | LVGL task and DSI display glue (touch is our own polled input device) |
| `lvgl/lvgl` | 9.5.x (must match the LVGL Pro project) | UI |

### Partition table (16 MB, `firmware/crowpanel_p4/partitions.csv`)

The partition table sits at **0xC000** (`CONFIG_PARTITION_TABLE_OFFSET`), not the usual 0x8000:
the bootloader starts at 0x2000 on the P4, and the v3.x bootloader is already 0x6140 bytes
(over the 0x6000 limit that 0x8000 leaves). Secure boot and flash encryption will make it bigger.

| Name | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| nvs | data/nvs | 0xD000 | 128 KB | configuration blob (a few KB), runtime state, Wi-Fi, time zone, preferences ([14](../14-Configuration-and-Installer-Programming/README.md)) |
| otadata | data/ota | 0x2D000 | 8 KB | |
| phy_init | data/phy | 0x2F000 | 4 KB | (unused on the P4; kept for tooling) |
| ota_0 | app/ota_0 | 0x30000 | 4.5 MB | P4 firmware (the M2 image is about 1.3 MB) |
| ota_1 | app/ota_1 | | 4.5 MB | P4 firmware |
| nvs_keys | data/nvs_keys | | 4 KB | NVS encryption keys (production) |
| assets | data/littlefs | | 2.5 MB | the pack store (ADR-045): language packs now, voice libraries later (docs/features/37 [language-packs.md](../37-Globalization/language-packs.md)); found by its label, never mounted as LittleFS (the subtype is as the table was first written) |
| cp_fw | data/undefined | | 1.5 MB | C6 co-processor image for co-processor OTA |
| fw_cache | data/undefined | | 2 MB | keypad firmware images for the OTA relay |
| eventlog | data/undefined | | 384 KB | event log ring: 6144 records, at least 6080 kept ([05](../05-Event-Log/README.md)) |
| coredump | data/coredump | | 64 KB | crash analysis |

### Boot sequence (`components/app/panel_app/src/panel_app.c`)
Since docs/features/33 step 7 the start-up is `panel_app_run()`, shared by every board;
`firmware/crowpanel_p4/main/app_main.c` fills its hooks (among them the silicon log, the Panel
Manager's sign-ins and its UI backend, the `power` report and remote development). The clock
module, when fitted, is read after step 2 (docs/features/35), and the zone sources start after
step 5 ([22](../22-Zone-Sources/README.md)).
1. The backlight's PWM at 0 (a panic restart keeps it running, P4-019); log the silicon revision
   and the firmware variant; NVS init; default event loop and netif.
2. `board_power_init()`: MIPI PHY and aux LDOs; audio amps held off.
3. I2S1 primed with silence, then the **sounder** task starts (amps only switch on with sound);
   the stored volume is applied.
4. `panel_log_start()`: mount the event log (scan, recover, boot number); its newest event number
   goes to the alarm service so numbering continues.
5. `alarm_service_start()`: load the configuration blob and runtime state from NVS (factory
   defaults on first boot), start the engine and the **alarm task**. The panel protects from
   here on; the 2-minute power-up recovery window for a lost installer code starts now.
6. Backlight PWM (off; set up in step 1), MIPI-DSI, the EK79007's hardware
   reset once the lanes are idle, its init, the LVGL port, GT911 touch, UI with the stored
   preferences (the setup wizard on first boot), backlight on once a drawn picture is on the panel
   (P4-019). A display failure is logged and
   the panel keeps running headless; a **touch** failure is logged too and is not passed off as
   success - the panel keeps protecting but nobody can operate it on the glass (P4-017).
7. The `health` task (P4-018): once every 2 s it turns each part's progress into the heartbeat
   `panel_ota` watches, runs the device-command timeout backstop (HCTL-012) and tells
   `zone_service` whether sensor input is still running.
8. The `net` task: ESP-Hosted brings up the C6 over SDIO (reset on GPIO32), `esp_wifi` via
   `esp_wifi_remote`, C6 firmware version logged, Wi-Fi from our NVS, SNTP, time zone.
9. MQTT client, the device registry, the Home Assistant link, then the serial console (UART0).

Between 5 and 6, `panel_ota_start()` runs ([13](../13-OTA-Updates/README.md)): after an update
the new firmware is on probation and must keep reporting the engine, the event log, the screen,
sensor input and touch healthy, or the previous firmware returns.

**Nothing in this sequence erases or rewrites storage to get past a failure** (P4-016). Steps 1
and 5 are the two that can refuse: stored settings that cannot be opened, and an alarm engine
that will not start. Both take the same way out (P4-017) - say so in the log, start the service
console so the fault can be read and `factory_reset confirm` is possible, leave everything on
flash exactly as it is, and restart after 2 minutes so a transient cause is retried and an
update on probation is rolled back by the bootloader.

ESP-Hosted's auto-init runs in its own task before `app_main`, so a missing or slow C6 never
delays the display. Troubles raised by the platform:

| Trouble | Raised when |
|---|---|
| `coprocessor` | `esp_wifi_init()` fails (C6 not answering); retried every 30 s |
| `network` | something needs the network (MQTT from M5) and it has been down for 5 minutes |
| `clock_not_set` | at start-up when nothing has set the clock yet (a fitted clock module that kept the time has, [35](../35-Battery-Backed-Clock/README.md)); cleared by the first SNTP sync or a time set by hand |
| `storage` | the event log can't be written, or the stored configuration can't be read |
| `sensor_radio` | the panel's sensor input can't be trusted: a source's whole link has been down longer than its grace while zones use it (ZSRC-015), or the zone hub's task stopped running (P4-018). One trouble for both, raised by `zone_service` |

### Components
| Component | Contents |
|---|---|
| `components/boards/board_api` | the board contract every board implements (API below; docs/features/33 step 4, ADR-035) |
| `components/boards/board_crowpanel_p4` | this board's side of `board_api.h`, its extras in `board.h`; `board_pins.h` is the single source of truth for wiring, `board_desc.c` the board in data (`board_info()`) |
| `components/app/panel_app` | the start-up every board shares (`panel_app_run()`, boot sequence above) |
| `components/services/platform_esp` | `alarm_service` (alarm task, queue, snapshot, listeners, installer mode), `alarm_store` (NVS: configuration blob, runtime state), `code_hash` (PBKDF2-HMAC-SHA256 on PSA SHA-256, 1024 iterations), `panel_log` (event log on the `eventlog` partition, writer task), `panel_prefs` (display and sound preferences, setup state), `panel_system` (versions, factory reset), `sounder` (the task that plays `components/core/sounder`'s mixer through the board, [08](../08-Audio-and-Annunciation/README.md)), `net_wifi` (Wi-Fi, scan, SNTP, time zone, time server), `net_check` (the connection check), `zone_service` (the zone hub's task, [22](../22-Zone-Sources/README.md)), `panel_console` |
| `components/core/sounder` | the patterns, the synthesizer and the mixer: portable, host-tested ([08](../08-Audio-and-Annunciation/README.md)) |
| `components/core/event_log` | portable event log ring (C11, host-tested with power cuts) |
| `firmware/crowpanel_p4` | `app_main.c` (this board's hooks for `panel_app`), `dev_adapter.c` (remote development), `partitions.csv`, `sdkconfig.defaults` (+ `.p4rev1`, `.dev`, `.release`, `.dark`) |

### BSP API (`components/boards/board_api/include/board_api.h`)
Since docs/features/33 step 4 these functions are the board contract every board implements
(ADR-035), and `board_api.h` is the authority for their signatures. The comments say what this
board does behind each; `board_info()`, `board_i2c_ext()` and `board_battery_poll/read()` joined
the list with the contract. This board's own extras are in `board_crowpanel_p4/include/board.h`
(last lines).
```c
const board_info_t *board_info(void);             // the board in data (board_desc.c)
esp_err_t board_power_init(void);                 // LDO3 2.5 V (MIPI PHY), LDO4 3.3 V, amps off
esp_err_t board_backlight_init(void);             // LEDC 30 kHz on GPIO31, starts dark
esp_err_t board_backlight_set(uint8_t percent);   // 0 = off, 1..100
esp_err_t board_display_init(lv_display_t **out); // DSI + EK79007 + LVGL port + refresh monitor
void      board_display_stats(board_display_stats_t *out, bool reset_max); // frames, late, guard
bool      board_ui_lock(uint32_t timeout_ms);     // LVGL lock for calls from other tasks
void      board_ui_unlock(void);
esp_err_t board_touch_init(lv_display_t *disp, lv_indev_t **out); // GT911 at 0x5D or 0x14, polled
void      board_touch_stats(board_touch_stats_t *out, bool reset_max); // polls, touches, gaps
esp_err_t board_audio_init(uint32_t sample_rate_hz);             // I2S1 TX, primed with silence
esp_err_t board_audio_write(const int16_t *stereo, size_t frames, TickType_t timeout);
void      board_audio_amp(bool on);               // GPIO30, active low
i2c_master_bus_handle_t board_i2c_ext(void);      // the I2C connector: I2C0, shared with touch
void      board_battery_poll(void);               // the STC helper at 0x2f, health task only
bool      board_battery_read(void *ctx, power_sample_t *out); // its cached sample
const char *board_name(void);

/* board.h, this board's extras */
i2c_master_bus_handle_t board_i2c_bus(void);      // I2C0, shared with the I2C_OUT connector
bool      board_dev_frame(uint16_t *out, size_t count); // frame copy, BOARD_FRAME_CAPTURE
```

### Alarm service API (`components/services/platform_esp/include/alarm_service.h`)
- Inputs are posted and never block: `alarm_service_zone_event()`, `_system_trouble()`,
  `_device_checkin()`.
- Commands run on the alarm task and return the engine's result: `alarm_service_command()`,
  `_authenticate()`, `_user_set()`, `_user_delete()`, `_set_config()`, `_get_config()`,
  `_keypad_execute()` (a completed keypad sequence, with classic keypad semantics for
  code + CODE + user), or any function through `alarm_service_call()`.
- Status: `alarm_service_snapshot_seq()` (cheap change counter) and `_get_snapshot()` (partition
  status, troubles, zone status, factory-code flags); `_recent_events()` (RAM ring, 100 events).
- Durability: `alarm_service_store_status()` is `ESP_OK` only when everything the engine changed
  is on flash; otherwise the last write error (or `ESP_ERR_NOT_FINISHED` before the first try,
  `ESP_ERR_INVALID_STATE` while stored data this firmware could not use is still there). The
  panel raises `ALARM_TRBL_STORAGE` for the same condition, so the screen says "Storage problem"
  (P4-013).
- `alarm_service_start()` failing is fatal, not something to carry on past: it means either
  code hashing does not work (`ESP_ERR_INVALID_STATE`) or the stored configuration could not be
  read (`ESP_ERR_NO_MEM`, an NVS error). The stored installation is left untouched in both
  cases and the panel should be restarted so the read can be tried again (P4-014, P4-015).
- Listeners (`on_event`, `on_report`, `on_sound`, `on_output`, `on_snapshot`) run on the alarm
  task: they must not block or call the synchronous functions.
- Keypad sources: the touchscreen is `ALARM_SRC_KEYPAD` id 0, the console id 0xFFFE, remote
  keypads their device number.

### Building and flashing
Windows (PowerShell):
```
pwsh tools/idf.ps1 build                  # P4 silicon v1.x (default) -> firmware/crowpanel_p4/build_v1
pwsh tools/idf.ps1 -ChipRev v3 build      # P4 silicon v3.x           -> build_v3
pwsh tools/idf.ps1 -p COM4 flash          # flash the v1 image (COM4 is an example; -ChipRev v3 for v3 silicon)
pwsh tools/idf.ps1 -Version 0.6.1-test build   # a test image with another version (build_v1_0.6.1-test)
```
Linux / macOS - `tools/idf.sh` is the same wrapper with the same options:
```
tools/idf.sh build
tools/idf.sh --chip-rev v3 build
tools/idf.sh -p /dev/ttyACM0 flash
tools/idf.sh --version 0.6.1-test build
```
- Run `tools/idf.ps1` from **PowerShell**: `idf.py` refuses to run under Git Bash / MSYS.
  (`-p` is an alias of `-Port`.)
- **The two wrappers must stay in step**: same options, same build folders, same
  `sdkconfig.v1`/`.v3`, same `SDKCONFIG_DEFAULTS` (v1 adds `sdkconfig.defaults.p4rev1`). They
  exist only to find ESP-IDF, create the signing key and pass those `-D` arguments; change one
  and change the other, or the two platforms build different images from the same source.
- `tools/idf.sh` looks for ESP-IDF in this order: an already-activated shell (`idf.py` on PATH),
  `$IDF_PATH/export.sh`, `$IDF_EIM_JSON` or an EIM `eim_idf.json` in the usual places, then
  `~/esp/esp-idf` and `/opt/esp/idf`. It warns when the active ESP-IDF is not v6.1.
- The first build creates the firmware signing key `secrets/ota_signing_key.pem` (git-ignored).
  Back it up: panels only accept updates signed with it ([13](../13-OTA-Updates/README.md)).
- The COM port's RTS line resets the P4 (esptool uses it). To keep the board from starting
  after a flash, use esptool with `--after no-reset` from the build folder:
  `python -m esptool --chip esp32p4 -p COM4 -b 460800 --after no-reset write-flash "@flash_args"`.
- Back up the factory flash (Elecrow demo `elecrow_p4_idf`, IDF 5.5.3) **before the first flash
  of a new board** - there is no download of it. CONTRIBUTING.md ("Set up a panel") has the
  `esptool read-flash` command; it lands in `build/device_backup/` (git-ignored). Restore with
  `esptool write-flash 0 <file>`.
`tools/idf.ps1` activates the EIM-installed ESP-IDF v6.1 and sets `IDF_COMPONENT_CACHE_PATH` to
`C:\Espressif\cm`: esp_hosted's example paths are too long for Windows' 260-character limit
under the default cache folder. Each variant keeps its own `sdkconfig.v1` / `sdkconfig.v3`
(generated, not committed); delete it after changing `sdkconfig.defaults*`.

### Serial console (`help` lists everything)
From a script: `python tools/panel_console.py -p COM4 status zones` (IDF venv Python); it types the
commands without resetting the panel and prints the output with timestamps.
```
status                     partition, troubles, zones not normal, network, factory-code warning
zones                      configured zones with type, class and state
zone 3 open | close        simulate a sensor (also trouble, tamper, lowbat, checkin and _ok forms)
keys 123456W               virtual keypad: code + AWAY (O=OFF S=STAY N=NIGHT M=MAX I=INSTANT
                           T=TEST Y=BYPASS U=CODE H=CHIME, * # A B C)
trouble ac_loss on|off     raise or clear a system trouble
health                     which parts of the panel still report that they work, and their
                           progress counters (nobody has to look at the screen)
events [n]                 recent events ('all' adds report markers; 'clear_loss' acknowledges
                           records the log could not keep and drops the storage trouble)
wifi <ssid> <password>     save Wi-Fi (wifi forget / wifi = status)
tz EST5EDT,M3.2.0,M11.1.0  POSIX time zone
volume 0-100 · factory_reset confirm · restart
mqtt ...                   MQTT / Home Assistant settings and status (mqtt help)
ota ...                    firmware updates: status, source, check, install, recv (docs/features/13)
c6 [status|update]         Wi-Fi co-processor firmware
display [stress <s>]       refresh check: frames/s, late frames, UI redraws, refresh times,
                           touch polls, free RAM, flicker guard;
                           stress = flash writes on fw_cache while the screen is redrawn
display watch <s>          one line a second (frames, refreshes, touch polls and gaps, touches)
                           while someone uses the screen
```
With `CONFIG_ALARM_DEMO_ZONES` (on in dev builds, off in release builds: a panel installed for a
home starts with no zones) the factory configuration has eight simulator zones:
1 Front Door (E/E 1), 2 Garage Door (E/E 2), 3 Living Room Window and 4 Back Door (perimeter),
5 Hallway Motion (interior follower), 6 Kitchen Smoke (fire), 7 Hallway CO, 8 Basement Water
(24 h aux).

## Hardware findings
1. **C6 held in reset.** With `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH` (what Elecrow's
   ESP-Hosted 2.x examples use) the C6 never answered (`sdmmc_init_ocr ... returned 0x107`).
   ESP-Hosted 3.x pulses the pin to the *active* level and parks it at the other one, while 2.x
   ended at the *active* level; the C6 runs while GPIO32 is high. `ACTIVE_LOW` fixed it at once:
   `slave chip id: 0x0d (esp32c6)`, co-processor firmware **2.12.3**, Wi-Fi up. ESP-Hosted 3.0.8
   dropped the choice and always resets this way (high, a low pulse, parked high), so the setting
   left `sdkconfig.defaults` with the move to 3.0.9 (2026-10-05).
2. **Display strobe during flash writes (photosensitivity hazard).** The first run showed the
   screen "flashing like a strobe light" while firmware was being uploaded. Cause: the DPI
   driver restarts the frame DMA from an interrupt after every frame; during a flash erase or
   write the cache is off and that interrupt (in flash) waits, so the picture drops out. Measured
   with the backlight forced off (`display stress`): 178 late frames in 20 s, gaps up to 67 ms,
   about 9 dropouts a second. Fixed with `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y` (DSI and DW-GDMA
   interrupts in IRAM; our IRAM-safe callbacks replace esp_lvgl_port's): 0 late frames in 45 s
   of flash writes with full-screen redraws. The flicker guard (P4-012) stays on as a backstop,
   and `CONFIG_BOARD_BACKLIGHT_FORCE_OFF` builds plus `display stress` remain available for
   investigating a suspected refresh problem (optional, docs/features/21). The backlight level (LVGL
   task) and the guard's hold (timer task) apply under one mutex, so the two cannot cross and leave
   the backlight on while the guard believes it holds it off
   ([B003](../../bugs/B003-text-cursor-blinked.md), 8367725).
3. **Taps ignored, jumpy scrolling (2026-09-20).** In the setup, "Next" sometimes did nothing
   until Back was pressed (after picking a time zone, and "Go to Home" on the last step), and
   scrolling Settings pages was erratic. Cause: esp_lvgl_port reads the GT911 only on its INT
   interrupt (LVGL event mode). The GT911 writes its next report only after the host has cleared
   the previous one; when the UI was busy at the moment a finger lifted, the release report was
   lost and no further interrupt came, so LVGL kept the finger "pressed" at its old spot and the
   next tap looked like a drag from there. Fixed in `board_touch.c`: our own LVGL input device
   polls the GT911 (and clears its report) every LVGL input period, tolerates I2C errors instead
   of aborting, and counts polls, touches and the longest gap between polls (`display`,
   `display watch <s>`). The INT pin is still driven during the GT911 reset (it selects the I2C
   address); the `gpio: conflict found for GPIO[42]` warning at start comes from that
   output-then-input sequence and is harmless. The poll counts three unanswered reads (about 50 ms)
   as a lift, so a touch controller or I2C bus that stops answering cannot hold a finger down and
   complete a panic or hold-to-confirm
   ([B004](../../bugs/B004-remote-press-completed-panic-hold.md); seeing both on a panel is BL-024).
4. **Rendering cost (measured 2026-09-20).** A full 1024x600 redraw takes about 55-90 ms (Home,
   with the 72 px glow of the state disk), scrolling a Settings page about 25-28 ms a frame
   (about 30 frames/s), and building a long page (the 45 time zones) up to about 145 ms, during
   which touch isn't read. Two LVGL render threads (`LV_DRAW_SW_DRAW_UNIT_CNT=2`) and 64-line
   draw buffers were both measured and were not faster (the latter much slower during flash
   writes), so the build keeps one render thread and whole-screen buffers. Ideas for later: the
   P4's PPA for fills (`LV_USE_PPA`), a cheaper glow on Home (BL-059), shorter lists. These
   figures are from `-Og` builds: since 2026-09-25 the image is built with `-Os` and LVGL with `-O2`
   (`alarm_optimize_hot_paths()`), with unused widgets and draw formats left out (the image 18 %
   smaller, 15 KB less internal RAM). Measured on the panel after that (2026-09-25, `display`'s
   two full redraws): the saver 121-131 ms (140.9 ms with the `-Og` image, the same screen,
   earlier that day); Home 87-90 ms; page switches 29-58 ms and back to Home 78 ms
   (`display watch`); no late frames. Full redraws are mostly memory traffic (1.2 MB buffers
   in PSRAM), so the compiler buys ~10-15 % there; the PPA idea above is the bigger lever. An
   LVGL shadow cache would spare the glow's blur on every redraw, but LVGL keeps it in its
   global state (internal RAM), about 25 KB for the disk (72 px glow + 88 px radius): left off.
5. **Screen saver picture (2026-09-20).** `CONFIG_LV_USE_TJPGD=y`: the built-in 126 KB JPEG is
   decoded once (about 370 ms, the first time the saver shows after a start) into a 1.2 MB RGB565
   image in PSRAM; the first saver frame takes about 150 ms, a minute change of the clock about
   8 ms.
6. **Ghosts of earlier screens after a power-on (2026-10-04, [B023](../../bugs/B023-old-screens-behind-new-ones.md)).**
   Pieces of earlier screens showed behind the current one after a power-on and faded once the
   panel had been on a while. A self-check build (`CONFIG_BOARD_DISPLAY_DIAG`, console
   `dispdiag`) found the frame buffer always holding exactly what LVGL drew, so the ghost is on
   the glass. Our start-up broke the EK79007's order:
   - GRB's pull-up sits on LDO4, so the reset rose before the DSI lanes existed;
   - `esp_restart` resets only the CPUs (GPIOs and the backlight's PWM keep their state), so a
     restart never reset the panel;
   - nothing put the panel to sleep first;
   - the backlight came on over whatever was on the glass.

   Fixed by P4-019. A power cut can't be ordered from firmware. The DSI host flags one LP1
   contention at each start, while the panel takes its init commands (unchanged code), and none
   while video runs.
7. **An error at every TLS handshake on v1.x (2026-10-05).** Each HTTPS or MQTT-over-TLS
   connection logged `psa_crypto_driver_esp_ecdsa: ECDSA peripheral not supported on this chip
   revision`, then verified in software and connected. ESP-IDF builds the ECDSA peripheral's
   signature check in for the P4, but the peripheral works only from silicon v3.0
   (`ecdsa_ll_is_supported()`). `sdkconfig.defaults.p4rev1` leaves it out
   (`CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY=n`), so the v1.x image goes straight to software (on a
   bench panel a handshake no longer logs it); the v3 image keeps the peripheral.
8. **The Wi-Fi signal read 0 dBm (2026-10-05, BL-031).** `esp_wifi_sta_get_ap_info()` goes
   through ESP-Hosted to the C6, whose 2.12.3 firmware answers with the whole record; ESP-Hosted
   3.0.7's host kept only its name, address, channel and security and zeroed the rest, the signal
   included. 3.0.8 keeps every field ("return every AP-record field from sta_get_ap_info"). With
   3.0.9 a bench panel reads -60 dBm, and the touchscreen and MQTT get the real signal; nothing on
   the C6 changed.

## Test and acceptance (HIL checklist, M2)

Host tests: `components/boards/board_crowpanel_p4/test/test_board_desc.c` holds the board's
description to `board_pins.h` - the validator, the wired terminals and the J7 places for add-ons
(docs/features/33 BRD-008). The portable logic the platform runs is host-tested in its own
components (`alarm_core`, `event_log`, `zone_hub`, `sounder`); the rest is proven on a panel with
the service console (below).

### Internal RAM and PSRAM (B001, 2026-09-20)

LVGL uses a board-owned allocator (`board_lvgl_memory.c`) that explicitly allocates and
reallocates objects, styles, text and software-rendering scratch in PSRAM. General `malloc`
still uses the 4096-byte preference threshold for other components. The UI allocator does
not spill into internal RAM when PSRAM is full. Frame/draw buffers retain the capabilities
selected by the display driver. The cache-safe frame interrupt does not touch LVGL; the
DMA2D completion interrupt already runs only with cache available.

The HA zone catalog (`s_ha`, 19,828 bytes) uses `EXT_RAM_BSS_ATTR`. The console's device list
previously reserved 17,408 bytes of internal BSS even when never used; it now allocates
temporary PSRAM and frees it after printing. These are task-context buffers. Task stacks,
alarm-engine state, synchronization objects, DMA descriptors and hardware SHA contexts stay
internal. MbedTLS first allocated internal-only, then used the default malloc
policy (`CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC`), so TLS record buffers (> 4 KiB) went to PSRAM
while its small key/SHA/session allocations stayed internal (docs/features/31 "Architecture and bounds"), and
since the second pass below it allocates from PSRAM outright (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`;
hardware confirmation pending). See Espressif's
[external RAM guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/external-ram.html).

`memory [check]` reports byte-accurate free/allocated memory, largest free block, minimum
free memory and allocation count separately for internal RAM and PSRAM, plus crypto readiness
and fault count. `check` also validates heap integrity. Capture it before/after a suspected
operation; the About screen's free-total alone cannot distinguish fragmentation from growth.

The measurements on a panel and the reproduction limits are recorded in
[B001](../../bugs/B001-installer-locked-out-crypto-latch.md). Display allocator changes were
first tested with the backlight forced off, including repeated UI navigation and flash stress.

#### Second pass (2026-09-22): what else moved to PSRAM, and the rule for task stacks

Measured before the change on the running panel (`memory`, idle, ELF `7d9b1fe2…65ec`):
internal free 203,731 B, largest block 110,592 B, minimum ever 122,788 B, 285,704 B allocated
in 584 blocks; PSRAM 25.9 MB free. The map put 134 KB of static `.bss` and 16 KB of `.data` in
internal RAM, only 19.8 KB of statics in PSRAM. Flash auto-suspend is off on this build, so
every flash or NVS write runs with the cache disabled: a task whose **stack** is in PSRAM
would fault the moment it wrote flash, and an ISR that runs during a write must not touch
PSRAM data. Everything below respects that; the rule is in CONTRIBUTING.md.

- **Statics** now carrying `EXT_RAM_BSS_ATTR` (all task-only, none read by an ISR): the alarm
  engine, its snapshot and the event ring (`alarm_service.c`, 22 KB), the HA link's state and
  watch list (`ha_link.c`, 18 KB), the dev service's log ring and certificates (`dev_remote.c`,
  9 KB), the Wi-Fi scan tables (`net_wifi.c`, 3 KB), the notifier's providers and snapshot,
  the MQTT and OTA snapshots, the Panel Manager's certificate and sessions. The portable UI got
  `UI_SLOW_BSS` (`ui_internal.h`: `EXT_RAM_BSS_ATTR` on the panel, nothing on the host) on its
  state, the trouble/event page scratch and the form rows (`ui_app.c`, `ui_forms.c`,
  `ui_settings.c`, 25 KB). About 82 KB of static internal RAM in all.
- **mbedTLS** allocates from PSRAM outright (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`): every TLS
  context, key, handshake buffer and PSA operation, not only the record buffers. The P4's AES
  and SHA DMA drivers accept PSRAM buffers, no TLS code runs while the cache is off, and PSA
  hashing no longer competes with the drivers for internal RAM (the B001 failure mode).
- **mDNS** task stack and packet memory come from PSRAM (`CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM`,
  `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM`): it never writes flash.
- **PSRAM task stacks** (`xTaskCreate*WithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`; the
  TCB stays internal) for the three long-lived tasks that never write flash or NVS: the
  sounder (4 KB: renders PCM and hands it to the I2S driver), the health messenger (4 KB: reads
  counters, posts to the alarm task) and the Panel Manager's maintenance task (6 KB: starts
  and stops the HTTPS server). The HTTPS workers, the alarm, log, zone, OTA, storage, console,
  net and dev-remote maintenance tasks all write flash or NVS and keep internal stacks; the
  LVGL task keeps its internal stack because settings are saved from it.
- Left alone on purpose: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` (lwIP and the esp_hosted
  SDIO path allocate through plain `malloc` and copy into DMA buffers; not proven safe to
  lower without the C6 link under load), the 4 KiB upload buffer in `panel_web.c` (it relies
  on that threshold), the sounder's PCM block, the C6 update buffer and the flash test buffer.

Result in the map (v1 dev): internal RAM used by the image 230,570 B -> 147,282 B; static
`.bss` 134,344 B -> 51,440 B; statics in PSRAM 19,828 B -> 105,216 B. What mbedTLS, mDNS and
the three stacks save at runtime is only measurable on the panel (`memory`).

Status: committed and pushed as `8367725`; built for v1 dev, v1 release, v3 dev and dark,
release check passes, host-tested;
**hardware confirmation pending** (`memory` before/after browser, MQTT and HA use; a box under
"Hardware acceptance", BL-032).

### Hardware acceptance
- [x] Boot log shows the P4 chip revision and the matching firmware variant (v1.3, v1.x image)
- [x] Display refresh steady at 55.9 frames/s, also under flash writes (`display stress`)
- [ ] Display shows a test pattern with no tearing; brightness 0-100 % works (BL-032)
- [x] Touch works; polled (finding 3). Retest the setup's Next buttons and Settings scrolling
- [ ] Touch accuracy at the four corners and the centre (BL-032)
- [ ] Key click and ACK beep play with no pop at power-up; amps switch off after 1.5 s of silence
      (P4-006; levels and the pop by ear are also in docs/features/08's by-ear box; BL-032)
- [x] Wi-Fi connects through the C6 (joined on the touchscreen, time synced); C6 firmware 2.12.3
- [ ] Wi-Fi reconnects after an access point reboot (BL-032)
- [x] Simulator: `keys 123456S` arms Stay, `keys 123456O` disarms (console)
- [ ] 24 h soak: no heap growth, no watchdog resets (BL-032)
- [ ] Internal RAM after the second pass (2026-09-22): `memory` before and after using the
      browser, MQTT and Home Assistant, compared with the figures in "Second pass" (BL-032)

## Open questions
- ~~Which P4 silicon revision?~~ Decided 2026-09-19 by the boot log ("Hardware acceptance"):
  v1.3, flash the v1 build.
- ~~Is the factory C6 firmware usable with ESP-Hosted 3.x?~~ Decided 2026-09-19 in "Hardware
  findings" 1: yes, it is already 2.12.3 (with the reset polarity fix above).
- SD card foundation implemented in docs/features/29; consumers such as event export (LOG-009,
  docs/features/05) and a firmware cache on the card (an update from the card is BL-061) remain
  future work.

## Tasks
- [x] Install ESP-IDF v6.1 (EIM)
- [x] `firmware/crowpanel_p4` skeleton, partitions, sdkconfig overlays
- [x] `board_crowpanel_p4` component (display, touch, backlight, audio, C6 reset through ESP-Hosted)
- [x] `platform_esp`: alarm task, NVS store, code hashing, sounder, console simulator
- [x] ESP-Hosted Wi-Fi bring-up + SNTP + time zone
- [x] First hardware run: boot, engine, event log, display refresh, touch, C6, console, OTA (2026-09-19)
- [x] Touch fixed (polling) and retested: 120 touches, every press released (2026-09-20)
- [x] Durable-write tracking with retry and `ALARM_TRBL_STORAGE`, a start-up that never resets
      to factory defaults to recover, and code hashing that fails closed (P4-013..015)
- [x] Prove P4-015 with fault injection at the hash: a failed hash is neither a wrong code nor a
      lockout, and the next one recovers (CORE-USR-024; the console's `crypto_test`, B001)
- [ ] Prove P4-013 and P4-014 with fault injection: a stub NVS that fails at allocation, set-blob
      and commit, plus a power cut either side of the commit; and P4-015 at the PSA calls other
      than the clone (BL-032)
- [x] Storage recovery that never erases, a start-up that parks with the console instead of
      carrying on, and a heartbeat from every part of the panel (P4-016..018)
- [ ] Prove P4-016..018 on the bench: an NVS partition filled until `nvs_flash_init` refuses (the
      panel must come up with the console and lose nothing; `factory_reset confirm` must still
      work); a build with the zone task or the GT911 blocked (`health` must name it, `status`
      must show `sensor_radio`, an update must roll back); and a downgrade onto a newer stored
      format (BL-032)
- [ ] The 10.1" V1.2 (BL-209): a v3-silicon image on a board, and the Flasher installing it on
  the panel
- [x] Screen dimming / screen saver (UI, M3)
- [x] Event log partition + writer, preferences, factory reset, Wi-Fi scan and time server (M4)
