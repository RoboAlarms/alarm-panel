# 20 · Zigbee sensors (ESP32-H2 radio): the first-class zone source

**Status:** In progress - tiers A and B (ZB-M1..M11) are built and host-tested, and run in the
PC simulator and the UI preview, not committed yet (2026-10-03): the zone source and the radio's
bring-up policy; device management (`zigbee_zones`: each device's record and setup, the
operations, install codes, the log, the network's records in the backup); the touchscreen's and
the Panel Manager's Zigbee pages; sirens and strobes as the panel's bell; keypads and key fobs
through the alarm task; MQTT diagnostics. The firmware side builds in every P4 image (ADR-047): the
panel as coordinator on Espressif's Zigbee stack over the socket's UART, network storage in the
panel's NVS, and the ESP32-H2's own RCP firmware (`firmware/radios/h2_rcp`). On a panel with an
ESP32-H2 (2026-10-01..03): the radio answers, the stack starts, through a signed update too,
losing and regaining the radio as step 4 expects, and the add window opens and closes. Left: the
bench acceptance with a real sensor, siren and keypad (BL-036, testing.md §11), updating the
radio from the panel, and the stack's own namespace in the backup (BL-036) ·
**Milestone:** M10, after the zone source framework (M11,
docs/features/22) · **Depends on:** 02, 15, 17, 22, 24 ·
**Code:** `components/core/zone_hub` (`zsrc_zigbee.c`, `zb_quirks.c`),
`components/core/zigbee_zones` (`zbz_zcl`, `zbz_registry`, `zbz_codes`, `zbz_models`, `zbz_panel`,
`zbz_wd`, `zbz_ace`; the fake radio in `fake/`), `components/core/zigbee_host`,
`components/services/zigbee_service`, `components/ui/alarm_ui/src/ui_zigbee.c`,
`components/services/panel_web/src/web_zigbee.c` (and `web/panel-pages.js`'s `zg*` pages),
`firmware/radios/h2_rcp` ·
**Tests:** `components/core/zone_hub/test/test_zigbee.c`,
`components/core/zigbee_host/test/test_zb_bringup.c`, `components/core/zigbee_zones/test/`,
`host/ui_preview/test_host_zigbee_real.c` (ctest `host_zigbee_real`), the preview walk's
`bl-144-*`, `bl-147-*`, `bl-148-*` and `zb-real-*` shots, the Panel Manager's node tests

## Goal
Pair off-the-shelf Zigbee sensors (door and window contacts, motion, water, smoke, CO,
temperature, key fobs) **directly to the panel**, through an ESP32-H2 802.15.4 radio attached
to the master. They become ordinary zones with supervision, tamper and low battery, like
wireless alarm sensors, and keep working with no Wi-Fi and no Home Assistant. This is
the source the zone editor recommends first (docs/features/22); Home Assistant and 433 MHz sensors
are the escape hatches for everything else.

## The documents

- This README: the goal, what is built, the requirements, the milestones of the Zigbee track and
  the tasks.
- [devices.md](devices.md): managing the devices - the radio contract, the device registry and
  its store, each device's setup, the operations, install codes, the pages' contract, events,
  the console, backup, the web API and MQTT.
- [alarm-devices.md](alarm-devices.md): sirens and strobes (IAS WD), keypads and key fobs (IAS
  ACE), and the rules that keep them from weakening the panel.
- [testing.md](testing.md): how all of it is proven, and which test proves which requirement.

## What is built, and its limits

The portable half exists and is host-tested, and the firmware half builds; the H2 module stays
optional. On a real H2 so far: the radio answering, the stack starting, and the radio lost and
regained (Next bench acceptance, steps 1 and 4); no sensor yet.

- `components/core/zigbee_zones` (tiers A and B, ZB-M1..M11): each device's registry record and
  its store (`zbz_registry`), the ZCL codec (`zbz_zcl`), install codes (`zbz_codes`), the model
  table (`zbz_models`), each device's setup, the operations, the pages' views and the log
  (`zbz_panel`), the sirens (`zbz_wd`) and the keypads and key fobs (`zbz_ace`); the host-only fake
  radio (`fake/zbz_fake_radio.c`) with virtual devices from `test/signatures/*.json`. Host-tested,
  in the PC simulator (the real `zbz_panel` over virtual devices, `host/ui_preview/host_zigbee_real.c`)
  and the UI preview; not yet with a real sensor (BL-036).
- The pages: the touchscreen's Installer > Zigbee (`ui_zigbee.c`, drawn in
  `docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js`; the radio, the devices, adding with and
  without an install code, a device's page, removing, the radio's details, the channel, the reset,
  sirens, keypads and fobs) and the Panel Manager's Zigbee pages (`web_zigbee.c`,
  `/api/v1/zigbee`, its `zg*` pages).

- `components/core/zone_hub/src/zsrc_zigbee.c`: 32 endpoint slots, IEEE/endpoint bindings, IAS
  enrollment retry policy, alarm/tamper/battery/trouble mapping, On/Off and Occupancy inputs,
  temperature thresholds, supervision, bounded learn windows, quirks and energy-scan policy.
- `components/core/zigbee_host`: portable updater policy with a fake transport in tests. It checks
  update permission at every destructive step, refuses writes without an image, bounds failed
  attempts, and verifies the reported version after restart. A future platform updater must
  also prevent arming throughout an update; pausing a write does not restore sensor coverage.
- `components/services/zigbee_service/src/radio_port.c`: the hardware seam, in every P4 image
  (`CONFIG_ALARM_ZSRC_ZIGBEE=y` in the P4's `sdkconfig.defaults`, ADR-047; the 4-inch board, which
  has no socket, keeps it off). Before the stack
  runs, the panel asks the socket's UART for the radio's firmware version (a Spinel
  `PROP_VALUE_GET` of `NCP_VERSION`); only an answer hands the UART to Espressif's Zigbee stack
  (`esp-zigbee-lib` 2.0.x, the first with ESP32-P4 libraries). The panel is coordinator and trust
  center, forms its network on channel 11, 15, 20 or 25 (`CONFIG_ALARM_ZB_CHANNEL_MASK`), keeps
  it in the panel's NVS partition in the stack's own namespace (no partition change), and opens
  joining only when asked (ZB-002): no steering after forming, nothing opened after a restart. A
  joining device's endpoints, clusters, Basic manufacturer/model and IAS zone type are asked for
  and handed to the source; IAS enroll requests and status changes, attribute reports and read
  answers, Poll Control check-ins and energy scans go the same way. The stack's callbacks only
  queue what they learn; the service's task hands it to the source under the zone lock, and the
  source's requests take the stack's lock briefly (never the other way round). Writing the H2's
  flash from the panel is not built (step 6).
- The restart guard (`radio_port.c`): the stack's Spinel client (OpenThread in
  ESP-IDF) ends the program - the panel restarts - when the RCP fails a reset, is incompatible
  or stops answering for good. The probe keeps an empty socket from starting the stack, but a
  module that answers the probe and then fails the stack would restart the panel every time it
  came up. A record kept through a restart (`RTC_NOINIT_ATTR`) counts the starts the radio ended
  (an `atexit` hook, and a crash while the stack was starting); after two in a row the stack
  stays off, `zigbee status` says why, and `zigbee retry` tries again. Ten minutes of a running
  network, or a power-on, clear the count.
- `components/services/zigbee_service/src/zigbee_service.c`: the task, the bring-up policy and
  the console: `zigbee status` (with the latest energy scan, and the guard's count), `zigbee
  retry`, `zigbee pair [off]` (the zone editor's learn window), `zigbee devices` and `zigbee code
  <ieee> <install code>` (ZB-002: the code is checked against its CRC and given to the trust
  center before the device joins).
- `firmware/radios/h2_rcp`: the H2's own firmware, ESP-IDF's OpenThread RCP on the UART that
  reaches the socket at 460800 baud. Its first image goes on over the module's own USB-C before
  it is seated (its README).
- The PC simulator supports pairing a demo contact and motion sensor, binding through the
  existing zone editor, contact/tamper/battery reports, leave, radio loss and channel noise.
  It runs the actual portable source and alarm engine. Interference needs fresh samples over
  30 seconds; one stale high sample cannot assert a jam, and a jam is the `sensor_radio`
  trouble at once. A lost radio follows the hub's grace (one hour), and recovery requires
  fresh sensor reports.
- Recovery fixes include stale tamper/trouble restoration after reconnect, battery replay when
  moving a sensor between zones, numeric remapping on IAS+temperature sensors, unique IAS IDs
  after repeated re-pairing, rejecting invalid ZCL measurements/endpoints, and ignoring reports
  while the radio is disconnected. Interference recovery invalidates cached input states too.
- Checked against esp-zigbee-lib 2.0.4's headers, Espressif's 2.x `zigbee_gateway` example and
  ESP-IDF's Spinel client: the panel's endpoint has a Basic client (a sensor's manufacturer and
  model arrive as a read answer, server to client), a battery reading of 0xFF is "unknown", not
  100 %, and the restart guard exists. Found right: the probe's Spinel frame, the start-up and
  forming sequence, IAS enrollment (the stack leaves the Enroll Response to the application),
  stored bindings coming back after a restart before a sensor speaks (a first report recreates
  the source's state), the energy scan's dBm, and the H2 image against ESP-IDF's `ot_rcp`.

**Still required before real sensors work (BL-036):** the bench acceptance below - the socket's
pins and baud rate, forming and keeping the network, pairing and enrolling a real sleepy sensor,
its reports, and what the stack does when the radio stops answering. An accepted enroll-response
send means the stack queued it, not proof that a sleepy device received it. Quirks are
provisional. The radio's page and install codes on the touchscreen are built (BL-144); updating the
H2 from the panel, the stack's namespace in the backup and native actuators remain later work.

## Research (September 2026)
- Espressif's **Zigbee gateway** architecture runs the Zigbee stack on the host SoC and uses an
  ESP32-H2 or ESP32-C6 as **radio co-processor (RCP)** over UART (plus reset and boot pins).
  The current ESP-IDF example lists **ESP32-P4** among the host targets, and the host can update
  the RCP firmware itself when the stored version differs
  ([esp_zigbee_gateway](https://github.com/espressif/esp-idf/blob/master/examples/zigbee/esp_zigbee_gateway/README.md)).
- [2026-09-28] `esp-zigbee-lib` 1.6.x ships no ESP32-P4 libraries; **2.0.1 added the P4**
  (release notes), and 2.0 dropped the separate ZBOSS library and renamed the API (`ezb_*`,
  `esp_zigbee_*`). 2.x keeps its network in an NVS partition the application names, and 2.0.4
  can stop and restart the stack. Espressif's `examples/zigbee_gateway` in the SDK (with an
  ESP32-P4 configuration) is the reference for start-up.
- The alternative is a **network co-processor (NCP)**: the H2 runs the whole stack and the host
  talks to it over a UART protocol
  ([esp_zigbee_ncp](https://github.com/espressif/esp-zigbee-sdk/blob/main/examples/esp_zigbee_ncp/README.md)).
  Lighter on the host, so it suits a CYD.
- The CrowPanel's own C6 already carries Wi-Fi through ESP-Hosted; sharing its single radio with
  802.15.4 costs reliability, so a **separate H2 module** stays the plan.
- **IAS Zone** (ZCL cluster 0x0500) is the standard security-sensor interface: a zone type
  (contact, motion, fire, water, CO, key fob...) and a status word with alarm, tamper, battery
  low, supervision reports, restore reports and trouble bits
  ([ESP Zigbee SDK](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/api-reference/zcl/esp_zigbee_zcl_ias_zone.html)).
  Hobby coordinators on ESP32-C6 already handle generic IAS sensors
  ([example](https://github.com/Drsplump/esp32c6-zigbee-coordinator)).
- Known pitfall: sleepy end devices (e.g. SOS buttons) can miss the coordinator's Zone Enroll
  Response because they sleep right after answering, so enrollment must be retried in their
  wake windows ([ESP-IDF issue](https://github.com/espressif/esp-idf/issues/18964)).
- Brand quirks are real: Tuya devices often use a manufacturer cluster (0xEF00) instead of the
  standard ones, and Aqara devices use their own attributes. Zigbee2MQTT and ZHA solve this with
  per-device converters; we need a small **quirks table** and accept that exotic devices come in
  through Home Assistant instead (docs/features/23).
- A Zigbee device belongs to **one** network. A sensor paired to the panel isn't in ZHA or
  Zigbee2MQTT, but Home Assistant still sees it as a zone through our MQTT discovery (docs/features/10).

## Decision (provisional until hardware bring-up)
- **P4: RCP architecture.** The Zigbee stack runs on the P4 (plenty of RAM and flash), the H2
  runs Espressif's RCP firmware, updated by the P4. This is Espressif's supported gateway layout
  with the P4 as a listed host, and it keeps all device handling (clusters, quirks) in our code
  where it's easy to test and update.
- **`zigbee_host` API** hides the stack behind a small interface (`permit_join`, device list,
  attribute reports, IAS enroll, commands, leave), so a CYD master could later use an **NCP** H2
  behind the same API without touching `zsrc_zigbee`.

Zigbee as the first-class wireless source on the P4 + ESP32-H2 RCP is recorded as ADR-020
(docs/features/00).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| ZB-001 | Zigbee 3.0 coordinator and trust center on the P4 with an ESP32-H2 RCP on a UART; the panel updates the RCP firmware | HIL |
| ZB-002 | Pairing only in a learn window opened from the zone editor (installer mode), 3 minutes, install codes supported; permit-join closes afterwards | HIL, preview |
| ZB-003 | IAS Zone devices are enrolled (CIE address, enroll response, retried in sleepy devices' wake windows); status bits map to FAULT / RESTORE / TAMPER / LOW_BATTERY; the IAS zone type suggests the zone type (docs/features/22 defaults) | host tests (mapping), HIL |
| ZB-004 | Also supported: On/Off and Occupancy Sensing clusters (contacts and motion that don't use IAS), Temperature Measurement (freeze zones), Power Configuration (battery %) | host tests, HIL |
| ZB-005 | Supervision: periodic IAS reports or Poll Control check-ins count as check-ins; a device silent past the zone's supervision time, or gone from the network, is CHECK (ZSRC-007); the radio itself not answering is the `sensor_radio` trouble | HIL |
| ZB-006 | Jamming: the channel's energy (periodic energy scans) held high for 30 s raises `sensor_radio`, like RF jam detection | HIL |
| ZB-007 | Quirks table (data, not code) for known brands whose sensors don't follow the standard; unknown devices fall back to the standard clusters | host tests |
| ZB-008 | Network key and device list survive restarts and OTA (the stack's own namespace in the panel's NVS) and are included in backups (CFG-005) encrypted | HIL |
| ZB-009 | Native Zigbee actuators (plugs, bulbs, locks) show on the Devices screen (docs/features/24) | HIL (after M13) |
| ZB-010 | A registry of devices, one record per device (its endpoints grouped under it), up to 64 devices with 4 endpoints each: identity, model, role, the installer's name (rename), setup state; it survives restarts and updates in the panel's NVS (namespace `zbpanel`, one key per device, field ids only added, a frozen fixture per format) and is read-only when stored by a later format (devices.md §4) | host tests (codec, fixtures, damaged and future records), HIL |
| ZB-011 | Signal and diagnostics per device: LQI and RSSI of its last frame as good, fair or weak, last heard, battery, power source, "extends the network", its software version (devices.md §7) | host tests, preview, HIL |
| ZB-012 | Each device's setup (descriptors, Basic, IAS Zone, CIE address, enrollment, bindings, reporting, check-in) as a state machine with timeouts and tries, sleepy devices served only in their wake windows; it shows where it is, stops after its tries ("Set it up again" goes on), and a setup cut by a restart reads back stopped (devices.md §5) | host tests (fake radio, replays clean, lossy and sleepy), HIL |
| ZB-013 | Remove asks a device to leave and says whether it answered; Forget removes a gone or broken device without its answer; Replace gives a new device the old one's zones; each lists the zones that now need a sensor (devices.md §6) | host tests (`zbz_panel`: Remove and Replace under the binding gate, armed meanwhile refused - ZB21, RF32), preview, HIL |
| ZB-014 | Identify makes the device blink or beep itself (never the panel's screen); "Check it now" asks a listening device at once and a sleepy one at its next wake-up | host tests, HIL |
| ZB-015 | Install codes on the touchscreen and in the Panel Manager (typed, or scanned there): length and CRC checked, given to the trust center, listed (address, added when; the code never shown again), kept until their device joins with them, forgotten on request; optional - any Zigbee 3.0 device may join in the window without one (devices.md §8) | host tests, preview, Panel Manager tests, HIL |
| ZB-016 | The channel (11, 15, 20, 25) chosen by the installer with the quietest of the latest survey marked, after a warning that sleeping devices move at their next check-in and some may need adding again; stored; installer mode with every partition disarmed | host tests, HIL |
| ZB-017 | Reset the Zigbee network: two confirmations and the installer code again; every device must be added again; logged | host tests, preview, HIL |
| ZB-018 | The radio page: state (and why it is paused), the radio's firmware version, channel, PAN id, extended PAN id, the panel's Zigbee address, background noise, the restart guard's count; Restart the radio; Try again when paused | preview, HIL |
| ZB-019 | Events in the log: added (and how it joined), removed, forgotten, left by itself, replaced, a stranger refused, channel changed, network reset, radio restarted - appended event types with stable names and sentences (devices.md §11) | host tests (golden names) |
| ZB-020 | Backup and restore of the network: the panel's `zbpanel` namespace, and the stack's network once its namespace is known; a restore re-enrolls every sensor and never lets the frame counter go back (devices.md §13) | host tests (policy), HIL |
| ZB-021 | A per-device trust record: how the device joined (install code or the standard key) and when, stored and shown on its page; every way may feed any zone, fire, CO and panic included (decided 2026-10-02) | host tests |
| ZB-022 | During a walk test, the IAS sensors whose every zone the walk test covers (a partition in its walk test, a type it counts: never fire or CO, panic and 24-hour zones only when included - docs/features/40 WALK-007) are put into IAS test mode (Initiate Test Mode) and their TEST bit marks the zone tested; every other sensor stays in normal operation and alarms as usual; afterwards they are returned to normal operation. The service names the scope from the engine's configuration and state (`zbz_walk_covered`, `zbz_panel_walk_scope`), again every 30 s while the walk test runs | host tests, HIL |
| ZB-023 | Zigbee sirens and strobes (IAS WD) sound with the panel's bell: burglary and fire (fire's own sound; CO the siren's emergency sound) per partition, their strobe, a chirp on arm and disarm, volume, a test; the engine owns the timing; supervised by the panel (no engine device number), a siren in trouble - silent too long included - is the bell trouble (alarm-devices.md §2, §4) | host tests (golden frames, all 256 Warning Info bytes, policy), HIL |
| ZB-024 | Zigbee keypads and key fobs (IAS ACE) arm and disarm a chosen partition: codes always checked by the engine through the alarm task, the five-wrong-codes lockout applies, the panel's state shown on the keypad, its emergency buttons only as allowed; a fob acts as its user, may disarm only when the installer turned that on (off by default), its panic button off by default (alarm-devices.md §3) | host tests (golden frames, policy tables), HIL |
| ZB-025 | The Panel Manager's Zigbee page: the radio, the devices, adding (with install codes typed or scanned), each device's actions and the radio details, following the operation each second (`/api/v1/zigbee`, devices.md §14) | Panel Manager tests, HIL |
| ZB-026 | MQTT diagnostics: the Zigbee radio's state and counts as a retained topic and a diagnostic entity, and each Zigbee zone's battery and signal; additive keys with golden payloads (devices.md §15) | host tests (golden payloads) |
| ZB-027 | The touchscreen's Zigbee pages in installer mode (the installer menu row, the Zigbee page, adding with and without an install code, the device page, removing, radio details, channel, reset, codes waiting, the zone editor hand-off, the zone page's device row, the trouble's link, and the siren, keypad and fob pages), hidden when the panel has no Zigbee (devices.md §9) | host tests (contract), preview |
| ZB-028 | **The newest state wins** (B020): every zone status notification is followed at once, in order, so an opening that lasted a moment is told to the engine (on an armed partition, an alarm). Two changes of a device's alarm bits within `ZSRC_ZB_BURST_MS` (3 s) are a burst - a magnet bouncing, which may leave out its last change: once the device has been quiet for `ZSRC_ZB_REREAD_MS` (1 s) the panel reads its zoneStatus, and the answer wins. Unanswered within `ZSRC_ZB_REREAD_WAIT_MS` (10 s), it is asked again at the device's next frame (a router at once), twice in all. A plain change, a repeated status or an unbound device is never read. No dedupe by ZCL sequence number: a repeat changes nothing, and a lost change is worse than a repeated one | `test_zigbee.c` (`test_a_burst_is_read_again_and_the_answer_wins`, `test_a_plain_change_is_not_read_again`, `test_an_unanswered_reread_is_asked_at_the_next_frame`, `test_a_momentary_opening_while_armed_counts`), bench |

## Design
- `components/core/zigbee_host`: portable C11 with no stack (the core tier, ADR-034). Today it
  holds the RCP bring-up and update policy (`zb_bringup.h`), with the probe, transport, image
  writer and restart behind `zb_boot_port_t`. The ESP-IDF half - the esp-zigbee stack in gateway
  mode, the RCP probe and transport, network storage and energy scans - is
  `components/services/zigbee_service/src/radio_port.c` (all but the RCP update's writer) and
  reaches the source through `zsrc_zigbee.h` (`zsrc_zb_radio_t` outbound, the `zsrc_zb_*` calls
  inbound). The Decision's `zigbee_host` API is today `zsrc_zigbee.h`'s
  `zsrc_zb_radio_t` (permit join, IAS CIE write and enroll response, attribute read, remove
  device, energy scan) and the inbound `zsrc_zb_*` calls.
- `components/core/zone_hub/src/zsrc_zigbee.c` (the quirks table in `zb_quirks.c`): implements
  `zone_source_ops_t` (`zsrc_zb_ops()`, docs/features/22): learn = permit join + "found devices"
  list; bindings = IEEE address + endpoint; the IAS and cluster mapping; quirks.
  A jam is reported through the sink's `link_now` (`zone_hub_link_now`), so `sensor_radio`
  follows ZB-006's 30 s at once; the other sources are unchanged.
- A device source for docs/features/24 on the same host API (later).
- UI: the learn screen shows each device as it joins (model, battery, signal, which inputs it
  has) and lets the installer pick one; the zone page shows last seen, signal and battery.

## Hardware notes

The selected module is the [Elecrow wireless ESP32-H2 module](https://www.elecrow.com/wireless-module-for-crowpanel-advanced-series.html),
in the board's wireless socket. The V1.1 schematic maps P4 TX/RX to GPIO53/54; V1.2 uses
GPIO27/28. The current BSP constants are for V1.1. Keep module GPIO12/14 untouched: they share
the touchscreen I2C bus. The wired terminal list still uses only 2, 3, 4, 5, 49, 50, 51, 52.

**First installation: over the module's own USB-C (settled 2026-09-28).** The
[module datasheet](https://www.elecrow.com/download/product/DAC0010/Wireless_Module-ESP32-H2_Datasheet.pdf)
gives the module a USB-C port (the H2's USB Serial/JTAG) and BOOT/RESET buttons (sections
4.1-4.2), but labels the socket U1TXD/U1RXD (UART1) with no host-controlled BOOT/RESET (section
4.4); [Espressif's download guide](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32h2/download-guidelines.html)
describes UART0 for download mode. So the RCP image goes on over USB-C before the module is
seated (`firmware/radios/h2_rcp/README.md`), and updating it later from the panel needs an RCP
that reboots itself into its loader on request (step 6). The datasheet does not give the GPIOs
behind U1TXD/U1RXD: the RCP assumes the H2's UART0 pins (GPIO24 out, GPIO23 in) and can be
rebuilt for others. Elecrow's product page (checked 2026-09-30) calls the same two pads the
"Serial port 0" receive and transmit pins, which supports UART0; the datasheet's pin table names
no GPIO. Elecrow's shared folder for the module (linked from the product page) holds their own
test code, `CrowPanel_Advance_HM_zigbeeTest_CODE.zip`, which would settle it if step 1 finds
the socket silent (not downloaded). The product flow must not depend on inaccessible case
buttons. The panel's partition table is unchanged: the stack keeps its network in the panel's
NVS partition.

### Sharing the socket

**Socket shared meanwhile (2026-09-22):** until the H2 arrives, the socket's SPI pads (6/7/8,
IO9, IO10) are the default wiring for a CC1101 or a 433 MHz receiver module (docs/features/25). The
H2 only uses the socket's UART pads, so the two can share the nets electrically but not the
physical socket: when the H2 goes in, the 433 MHz receiver moves to wires or other pins
(`CONFIG_ALARM_RF_*`).

The socket's UART is also a place for a Z-Wave radio (docs/features/36): `zwave place socket`
puts a Zooz ZAC93 there. Since Zigbee is in every P4 image (ADR-047) the rule is checked at run
time: `zwave place socket` is allowed, and while the Z-Wave radio holds the socket Zigbee does not
start and its status says the socket is the Z-Wave radio's.

## Test and acceptance
- Host tests: `components/core/zone_hub/test/test_zigbee.c` (the source, tagged ZB-002..ZB-008
  with the ZSRC tags of docs/features/22) and `components/core/zigbee_host/test/test_zb_bringup.c`
  (the bring-up and update policy, ZB-001). They cover software policy, not radio compatibility;
  the ZB-008 cases prove the binding codec, not the Zigbee network's persistence. Run with
  `pwsh tools/host_test.ps1`, `pwsh tools/ui_preview.ps1` and `pwsh tools/ui_sim.ps1 -BuildOnly`.
- The UI preview's Zigbee walk (`host/ui_preview/main.c`): the pages over the demo backend
  (`host_zigbee_demo.c`, shots `bl-144-*`, `bl-147-*`, `bl-148-*`, named after the screens of
  `docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js`) and over the real `zbz_panel` with
  virtual devices (`host_zigbee_real.c`, shots `zb-real-*`); the PC simulator runs the same real
  panel (testing.md §10). `pwsh tools/ui_preview.ps1 -Locale es-US` walks the same pages in
  Spanish.
- Firmware: Zigbee is in every P4 image (ADR-047), so CI's P4 v1 and v3 builds fetch the Zigbee
  library (170 MB) and the image-size check covers it. `pwsh tools/idf.ps1 [-Profile dev] build`
  (POSIX `tools/idf.sh [--profile dev] build`) builds the panel with the stack into `build_v1`
  (`build_v1_dev`), with the project's one `dependencies.lock`. Before ADR-047 the stack had a
  build switch of its own; measured then: on 2026-09-28 3,084,288 bytes (65.4 % of the slot),
  static internal RAM 14 KB above the same dev build without Zigbee (the stack's heap at run time
  is a bench check); on 2026-09-30, from a main that had grown meanwhile, 3,477,504 bytes (74 %);
  after the size work 68.1 % against 58.4 % without (ADR-047). `pwsh tools/idf.ps1 -Project
  radios/h2_rcp build` builds the H2's image (263,632 bytes on 2026-09-30).
- Hardware: on a panel with an ESP32-H2 (2026-10-01) the radio answers and the stack starts, and
  the add window opens and closes; no real sensor yet (steps 3-6 below, BL-036).

### Next bench acceptance (BL-036)

Steps 2 and 5's code is written (2026-09-28); what each step proves is still owed.

1. **The radio answers.** Put `firmware/radios/h2_rcp` on the module over its own USB-C (the
   Flasher's Advanced tools › Zigbee module, FLS-030; or its README), then, with the panel disarmed and switched off, seat it in the socket. Give the
   panel a current build, which has Zigbee in it (`pwsh tools/idf.ps1 -Profile dev build`; a
   signed update keeps everything, no partition change). `zigbee status` should say `ready=1`, the RCP's
   version, a channel of 11/15/20/25 and a PAN id. If it says the socket is silent: the H2's
   UART pins (the README's `CONFIG_RCP_UART_PIN_MANUAL`; Elecrow's test code in Hardware notes
   names them), then the board revision's P4 pins (V1.1 GPIO53/54). If the panel restarts
   while the stack starts, it does so at most twice: then `zigbee status` says the radio is
   paused and why (its firmware, a reset it didn't come back from, or no answer) - send the
   boot log. Confirm the touch still works (module IO12/IO14 share its I2C bus).
2. **The coordinator** (written): bounded, queued callbacks; nothing sent while holding the
   zone lock after the stack's lock; the service owns bring-up state and publishes snapshots;
   `retry` goes to its task. On the bench: the network forms once and the free heap after the
   stack started (`status`) leaves the panel its internal-RAM reserve (docs/bugs/B001).
3. **A real sensor.** `zigbee pair` (or the zone editor's learn window), put a sleepy IAS contact
   in pairing mode: `zigbee devices` lists it with its model and zone type, and its enrollment
   reaches `enroll 3` (done) - including when a response is lost. Bind it to a zone: open,
   close, tamper and battery reach the zone. Repeat with Wi-Fi and Home Assistant unavailable.
   `devices` should show the manufacturer and model (read through the panel's Basic client). A
   device with an install code: `zigbee code <ieee> <code>` from its label, then `zigbee pair`.
4. **Losing things.** The RCP restarting under a running panel (the module's RESET button),
   the panel starting with the socket empty, a device leaving and rejoining, a quiet device
   going CHECK, fresh energy scans during interference (`zigbee status` shows the latest), and
   recovery with no replay of stale sensor state. Expected from the source (Open questions): a
   module pulled out of a running panel restarts it once, after three tries to restore it; the
   panel then comes up with the socket silent and no stack. Resetting the module (its RESET
   button) may be restored without a restart.
   Bench, 2026-10-01 (the module held in its ROM loader through its own USB-C, no sensor yet):
   about a second away restarted the panel once in two tries (timing); about ten seconds away
   restarted it about 9 s in, its `system_start` saying a panic (BL-099: the Spinel client's
   exit after its restore attempts), as expected above. The panel then came up while the module
   was still silent: no stack, a new look every 60 s, and the network back about a minute after
   the module returned, with no restart. A plain reset (esptool's reset sequence over the USB-C,
   like its RESET button) left the panel running. Owed: whether the stack restored the radio
   after a short outage (needs `zigbee status` or a sensor), an empty socket at power-on.
5. **Keeping the network** (written: the stack's namespace in the panel's NVS). Network
   identity, keys, device IDs and bindings across a restart and a signed P4 update: the sensor
   keeps reporting without being paired again. Binding-codec tests alone don't prove this.
6. Only then implement image delivery/update with compatible-image validation, power-loss
   recovery and an alarm-task interlock that prevents arming during updates. No eFuse changes.

## Open questions
- Can the socket's UART reach the ROM loader, or does first installation require USB or a
  preprogrammed module? Can later updates use a verified application-level updater? First
  installation settled 2026-09-28: over the module's USB-C (Hardware notes). Later updates are
  still open (step 6, BL-036).
- What does the stack do when its RCP stops answering while it runs: carry on, stop, or restart
  the panel? The panel probes before starting it, so a missing module never starts the stack;
  the running case is step 4's. Answered from ESP-IDF's source on 2026-09-30: the stack drives
  the RCP through ESP-IDF's `esp_radio_spinel` (OpenThread's Spinel client), which tries to
  restore a timed-out RCP three times (`OPENTHREAD_SPINEL_CONFIG_RCP_RESTORATION_MAX_COUNT`) and
  then ends the program (`DieNow`), as it does when the RCP fails its reset or is incompatible:
  the panel restarts. After such a restart an unplugged module leaves the socket silent and the
  stack is not started; the restart guard stops a module that answers the probe but fails the
  stack from restarting the panel again and again (What is built). Step 4 confirms it.
- How does the panel update the H2 without BOOT or RESET lines? Espressif's gateway
  (`esp-zigbee-sdk` `examples/zigbee_gateway`, `esp_rcp_update`) drives both pins and writes the
  RCP through its ROM loader, which the socket can't do. The route found on 2026-09-30: ESP-IDF's
  RCP can run console commands sent over Spinel (`CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE`, the
  vendor property `SPINEL_PROP_VENDOR_ESP_SET_CONSOLE_CMD`), so an H2 image with two OTA slots
  and a small update command (begin, data, end with the image's SHA-256) could write its other
  slot and restart into it, the old one kept if anything fails (step 6). The commands get no
  answer: the panel would judge the result by the version the H2 reports after its restart.
  Installing that image needs one more flash over the module's USB-C.
- Build our own sensor nodes later (custom 802.15.4 with our AEAD), or rely on commercial
  Zigbee sensors only? Leaning: commercial sensors first. Decided 2026-09-28: commercial Zigbee
  sensors only; our own nodes aren't planned.

## Milestones (the Zigbee track)

Full Zigbee support is its **own track, ZB-M1 to ZB-M13**, in this document, as Z-Wave's Z1-Z11
are in docs/features/36. It is built library first: the portable code in
`components/core/zigbee_zones` beside `zone_hub`'s source, proven on the PC against a fake radio
and recorded devices ([testing.md](testing.md)), then the service, the pages and the firmware.

### Decisions (2026-10-02)

- Full Zigbee support, on this track. **Now:** tier A - device management and sensors to
  Z-Wave's level - and tier B - IAS WD sirens and strobes as alarm outputs, IAS ACE keypads and
  key fobs arming and disarming. **Later rows:** tier C - lights, plugs, locks, thermostats and
  covers on the Devices screen and in Home Assistant (BL-149) - and tier D - a mesh map, channel
  survey, device updates, updating the H2 from the panel (BL-150).
- Zigbee is in every P4 image, like Z-Wave (ADR-046): ADR-047 (BL-146).
- Joining: any Zigbee 3.0 device may join during the installer's 3-minute window; an install
  code is optional (typed or scanned); each device records and shows how it joined (install code
  or the standard key). Devices that joined without an install code may still feed fire, CO and
  panic zones.
- The proposals for the pages are drawn, then built, without waiting for review;
  they are now today's screens (BL-144: `docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js`;
  BL-145: the Panel Manager's own Zigbee pages, `panel-pages.js`'s `zg*`, and the docs/features/31
  README's row).
- BL-036 stays the bench acceptance with a real sensor.

### Decisions (the service review, 2026-10-03; taken without waiting for review)

- The radio's queue keeps the alarm path (ZB-003, ZB-024): a zone status, an enroll request and a
  keypad's or fob's ACE command may use all its slots, other events leave 8 free and frames 12;
  one lost even so is counted, shown and logged, and a lost zone status is read back
  (`components/services/zigbee_service/README.md` "Rules"), and raises the "sensors may not be
  heard" trouble through `zone_service_report_lost` for 5 minutes after the last loss (BL-151).
- A restore does not yet set IAS devices' setup back to CIE_WRITE: it is planned work with the
  stack's namespace (BL-036 step 5). Until then a restore makes every device join again anyway
  (the replacement's network is new), and its setup writes the CIE address then; devices.md §13
  and `backup_policy.c` describe the step to build.
- A page's request answers with its own result once the zigbee task has taken it (2 s more), and
  the Panel Manager's `GET /api/v1/zigbee` answers 503 "Memory busy" when it has no memory for
  the list or the device's page, never 404 or an empty list (BL-113).
- The channel survey is refused (`not_allowed`) while any partition is armed, from every caller:
  its scans leave the panel deaf about half a second, and its result serves only a channel change
  (ZB-016). The service refuses it; `zbz_panel` still takes it without `allowed` (devices.md §6).
- A request's source is in the log: `ui_zb_request_t.via` (set by the backend: the Panel
  Manager's entry stamps `UI_ZB_VIA_WEB`) goes with the operation's user into each log record, and
  the Panel Manager's changes are logged as its own (`ALARM_SRC_REMOTE`, the Panel Manager's id),
  not the touchscreen's (BL-151; Z-Wave's still are). The zone rebind's NVS write happens as the
  zigbee task lets the zone lock go (`zone_service_unlock`), not under it (BL-151).

### Decisions after the review of the pages (2026-10-03, without waiting for review)

- A dead or paused radio: its sensors' zones keep their last state (the hub skips a source whose
  link is down) and the `sensor_radio` trouble follows after the link's grace (ZSRC-015). Both
  pages say so ("Their zones keep their last state; a Sensor radio problem follows if it
  lasts."), never "their zones show CHECK".
- A setup that stopped (its tries spent, ZB-012) never goes on by itself: the add's ending names
  the device with no step count and says to open it and set it up again (wake it first if it
  sleeps), on both pages. Only a setup still running (asleep) "finishes when it next wakes".
- The network reset asks twice on both pages (ZB-017): "Reset the Zigbee network?" with Continue,
  then "Reset it now?", then the installer code (the touchscreen) or the password (the Panel
  Manager).
- A Zigbee keypad has no silent panic, and a key fob's owner is a master, partition master,
  standard, arm-only or guest user (alarm-devices.md §3).
- The channel's noise in one set of words on both pages: Busy at -75 dBm and louder, Some noise to
  -85 dBm, else Quiet; no cause is guessed.
- The model table's words for waking a device are English until `zbz_models.c` marks them for
  translation (owed: `TR_NOOP_C("wake up", ...)` and `("pairing", ...)` as `zw_quirks.c` does,
  the file in `po/MARKED`, WAKE1 reworded to read after a colon). Meanwhile the Panel Manager shows
  them as they come in English and, in another language, their translation under "wake up" once
  the catalog has one, else its own sentence - never an English clause in a translated page.
- Kept as built: outside installer mode the sensor radio trouble still opens the locked Installer
  page ("Tap to see the radio", page 13): the person at the panel is often the installer, who types
  the code there and the Zigbee page follows.

### Separation rules

- `zone_hub`'s `zsrc_zigbee` keeps its calls; it gains only what ZB-M1 declared
  (`zsrc_zb_device_forget`, `zsrc_zb_device_setup`, `zsrc_zb_walk_test`, the appended radio
  hooks and report types). Everything else is new code in `zigbee_zones`, `zigbee_service` and
  the pages.
- Stored formats only add fields and get a frozen fixture when they change; event types are
  appended before `ALARM_EV_COUNT`; names, MQTT topics and JSON keys are only added.
- Nothing reaches a panel without the word of the person you work for in the session, and only while disarmed.

### The milestones

State of each (2026-10-03): ZB-M1..M11 are built and host-tested, in the simulator and the
preview; ZB-M9 runs on the bench panel (radio working, the add window opens and
closes); the bench steps with a real sensor, siren, keypad and fob are owed (BL-036, testing.md
§11). ZB-M12 and ZB-M13 are later rows.

| | Milestone | Row | Delivers | Proved by |
|---|---|---|---|---|
| **ZB-M1** | Contracts and design | BL-143 | devices.md, alarm-devices.md, testing.md; the radio contract's additions (`zsrc_zigbee.h`), the `zigbee_zones` headers, ui_backend's `zb_*` block | host build: `test_zbz_contract`; `check_architecture.py` |
| **ZB-M2** | Radio plumbing, ZCL codec and the registry | BL-143 | `radio_port.c` reports (LQI and RSSI, descriptors, Basic, joins and the key used, leaves, check-ins, request ends) and the appended hooks; `zbz_zcl`, `zbz_registry` with its first frozen fixture; the source's new calls; the fake radio and signature fixtures | host tests (golden frames, codec, converter, fake radio); the P4 firmware build |
| **ZB-M3** | Setup, the model table and the pages' view | BL-143 | each device's setup (devices.md §5); `zbz_models`; `zbz_panel`'s status, devices, device and progress | host tests, replays clean, lossy and sleepy |
| **ZB-M4** | Operations and install codes | BL-143 | add (with and without a code), remove, forget, replace, check it now, identify, set up again, survey, channel, reset, restart; `zbz_codes` | host tests |
| **ZB-M5** | Events, backup and MQTT | BL-143 | `ALARM_EV_ZIGBEE_*` with names and sentences; `zbpanel` in the backup policy (the stack's namespace once known); MQTT diagnostics | host tests (golden names and payloads) |
| **ZB-M6** | The service, the console and the PC | BL-143 | `zigbee_service` and `ui_port_esp` fill `zb_*`; the `zigbee` console with `trace`; `/dev/v1/zigbee` and `panelctl zigbee`; the fake radio in the preview and the simulator | firmware build; preview build; bench (BL-036) |
| **ZB-M7** | The touchscreen's Zigbee pages | BL-144 | the pages of devices.md §9 from the proposal | preview walk shots; host tests |
| **ZB-M8** | The Panel Manager's Zigbee page | BL-145 | `/api/v1/zigbee`, the page and its actions, install codes scanned | Panel Manager node tests; bench |
| **ZB-M9** | Zigbee in every P4 image | BL-146 | ADR-047: on in the P4 manifest, built by CI, the image-size check, the preview's sources (BL-087) | CI firmware build, `check_image_size.py`, the internal-RAM reserve on the bench |
| **ZB-M10** | Sirens and strobes | BL-147 | `zbz_wd`: IAS WD with the bell, squawks, tests, supervision (alarm-devices.md §2) | host tests (all 256 Warning Info bytes, policy); a real siren captured and heard |
| **ZB-M11** | Keypads and key fobs | BL-148 | `zbz_ace`: the IAS ACE server through the alarm task (alarm-devices.md §3) | host tests (golden frames, policy tables); a real keypad and fob |
| **ZB-M12** | Smart-home devices (tier C, later) | BL-149 | lights, plugs, locks, thermostats, covers on the Devices screen and in Home Assistant (ZB-009) | host tests, HIL |
| **ZB-M13** | Mesh tools and updates (tier D, later) | BL-150 | the mesh map, channel survey over time, device updates (OTA server), updating the H2 from the panel | host tests, HIL |

Each milestone is done when its host tests pass in CI and its bench step (testing.md §11) has
been run on the word of the person you work for; BL-036's steps 1-5 come first for anything that needs the radio.

## Tasks
What is still open is mainly the radio side (BL-036); most of it needs an ESP32-H2 module on a
panel, following "Next bench acceptance" above.

- [x] Select the Elecrow module and document the V1.1 socket mapping (bench verification pending)
- [x] Portable RCP bring-up and update policy in `zigbee_host`, tested with a fake radio
      (`zb_boot_tick`, `test_zb_bringup.c` ZB-001)
- [x] The coordinator in firmware: `esp-zigbee-lib` 2.0.x over the socket's UART, the Spinel
      version probe, queued callbacks into the source, network storage in the panel's NVS, the
      `zigbee` console (BL-036, c026569); built then with `-Zigbee` (in every P4 image since
      ADR-047), not yet run on an H2
- [x] The H2's RCP image `firmware/radios/h2_rcp`, first installed over the module's USB-C
      (BL-036, c026569); builds, not yet on a module
- [ ] On the bench with an H2: the radio answers, the network forms, a real sensor pairs,
      enrolls and reports (steps 1-3) (BL-036)
- [x] Portable `zsrc_zigbee` mapping, enrollment policy, supervision and quirks with host tests
- [x] Existing learn picker and zone details exercised with the PC radio simulator
- [x] Install codes on the console: `zigbee code <ieee> <install code>`, the code's length and
      CRC-16/X.25 checked, then `ezb_secur_ic_add` before the device joins (ZB-002, 71be56c)
- [x] The radio's page, live battery and signal details, and install codes on the touchscreen
      and in the Panel Manager (ZB-002, ZB-011, ZB-015) (BL-144, BL-145, 6da5940)
- [ ] Real pairing acceptance on the bench: a sensor joined with and without an install code,
      its battery and signal on the pages (ZB-002; steps 3 and 4) (BL-036)
- [x] A radio that ends the panel does not do it over and over: the restart guard pauses the
      stack after two such starts in a row until `zigbee retry` (BL-036, 71be56c)
- [x] Jamming policy: fresh energy samples held high for 30 s mark the channel jammed (the source
      unusable, no pairing), a stale sample or a gap in scans cannot; the `sensor_radio` trouble
      follows at once (`zsrc_zb_energy`, `test_zigbee.c` ZB-006) (BL-104, c026569)
- [x] A jam skips the hub's link grace: `sensor_radio` after ZB-006's 30 s, not an hour later,
      through the sink's `link_now` (`zone_hub_link_now`); an unplugged radio still waits its
      grace (BL-104, c026569)
- [ ] Radio loss, leave/rejoin, supervision and jamming on the bench: periodic energy scans from
      the stack, radio unplug/reconnect, device leave/rejoin and quiet-device supervision, with
      no replay of stale state (ZB-005, ZB-006; step 4) (BL-036)
- [ ] Persistent network storage on the bench: network identity, keys, device IDs and bindings
      across a restart and a signed P4 update (the stack's namespace in the panel's NVS, no
      partition change) (ZB-008; step 5) (BL-036)
- [ ] Backup of network data: the network key and device list in the panel's encrypted backup
      (ZB-008, CFG-005) (BL-036; the file exists since 2026-09-29: the stack's NVS namespace joins
      its policy, `components/core/backup/src/backup_policy.c`, once known on the bench)
- [ ] The RCP image on the panel: first installation and later updates, with compatible-image
      validation, power-loss recovery and no arming during an update (ZB-001; step 6, after the
      first open question) (BL-036)
- [ ] Native Zigbee devices on the Devices screen (with docs/features/24) (BL-036)
- [x] ZB-M1 contracts and design (ZB-010..ZB-027) (BL-143, 6da5940)
- [x] ZB-M2 radio plumbing, the ZCL codec and the registry (ZB-010, ZB-021, ZB-022) (BL-143, 6da5940)
- [x] ZB-M3 each device's setup, the model table and the pages' view (ZB-011, ZB-012, ZB-018)
      (BL-143, 6da5940)
- [x] ZB-M4 the operations and install codes (ZB-013..ZB-017) (BL-143, 6da5940)
- [x] ZB-M5 events with names, `zbpanel` in the backup policy, MQTT diagnostics (ZB-019, ZB-020,
      ZB-026) (BL-143, 6da5940)
- [x] ZB-M6 the service, the console, the dev link, `panelctl zigbee`, the fake radio in the
      preview and the simulator (BL-143, 6da5940)
- [x] ZB-M7 the touchscreen's Zigbee pages, `mockup/screens/zigbee.js` (ZB-027) (BL-144, 6da5940)
- [x] ZB-M8 the Panel Manager's Zigbee pages and `/api/v1/zigbee` (ZB-025) (BL-145, 6da5940)
- [x] ZB-M9 Zigbee in every P4 image (ADR-047) (BL-146, 6da5940)
- [x] ZB-M10 sirens and strobes (IAS WD) as the panel's bell (ZB-023) (BL-147, 6da5940)
- [x] ZB-M11 keypads and key fobs (IAS ACE) through the alarm task (ZB-024) (BL-148, 6da5940)
- [ ] A real siren's bit order and its supervision reads, a real keypad and key fob, on the bench
      (ZB-023, ZB-024; testing.md §11) (BL-036)
- [x] A lost radio event that carried an alarm raises the sensor radio trouble through
      `zone_service_report_lost` (ZB-003, ZB-024) (BL-151, a836424)
- [x] The IAS ACE commands' codes kept out of the radio queue's slots (code slots, wiped)
      (ZB-024) (BL-151, a836424)
- [x] The Panel Manager logged as the source of its Zigbee changes (ZB-019) (BL-151,
      a836424)
- [x] The zone rebind's NVS write outside the zone lock (BL-151, a836424)
- [ ] A request the zigbee task has taken answers "still running" instead of BUSY
      (`UI_ZB_REQ_PENDING`): new words on the touchscreen and in the Panel Manager, drawn as a
      proposal first (BL-151; drafts: `proposals/BL-151-zb-pending.js` in docs/features/07 and 31
      mockups, waiting for review)
- [x] The simulator does what the firmware does: a fob's user's authority (`zba_fob_may_be`,
      shared) and the walk test with its scope (`zbz_walk_covered`) (ZB-022, ZB-024) (BL-151,
      a836424)
- [ ] ZB-M12 smart-home devices: lights, plugs, locks, thermostats, covers (ZB-009) (BL-149)
- [ ] ZB-M13 mesh map, channel survey over time, device updates, the H2 updated from the panel
      (BL-150)
