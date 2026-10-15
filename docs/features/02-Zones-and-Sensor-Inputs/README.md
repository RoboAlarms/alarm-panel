# 02 · Zones and sensor inputs

**Status:** Implemented v1 - the zone model is in the core, and the source framework with the
simulator and wired sources ([22](../22-Zone-Sources/README.md)) is host-tested and on a panel
since 2026-09-19; the Home Assistant, 433 MHz, Zigbee (portable half) and Z-Wave sources plug
into it. Left: a wired contact on J7 (BL-029), Zigbee's radio (BL-036), keypad inputs (BL-048),
EOL supervision (BL-063). · **Milestone:** M1 (core), M2 (simulator), M11 (source framework,
wired), M7 (keypad-attached), M10 (Zigbee), M13 (Home Assistant), M14 (RF), Z-Wave on its own
track (36) · **Depends on:** 01, 06, 11, 20, 22

## Goal
Turn physical and virtual sensors into zone events for the engine, regardless of where the
sensor is connected. Zone behaviour is defined once, in the core; the input path is pluggable.

## Reference behaviour
A classic keypad panel programs each zone with a type, partition, report code and input type
(wired, RF, button), names it with descriptors, and enrols RF sensors by serial number
plus loop number. Wired zones have end-of-line (EOL) resistor supervision and a programmable
response time. RF sensors check in periodically; a missed check-in shows **CHECK** (supervision
failure). Tamper and low battery are reported per sensor.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| ZONE-001 | Each zone has type, partition, name (<= 23 chars), flags (chime, night, bypassable, no-report, swinger-exempt), device class, optional cross-zone partner, supervision interval, assigned user for buttons | core config + `test_config.c` |
| ZONE-002 | Every input source reports through one API: `alarm_engine_zone_event(zone, FAULT/RESTORE/TROUBLE/TAMPER/LOW_BATTERY/CHECKIN)` via the alarm task queue | platform code review, M2 |
| ZONE-003 | Each physical input maps to exactly one zone; one zone may have only one source | `test_zone_hub.c` (ZSRC-003) |
| ZONE-004 | Wired inputs are debounced with a per-zone response time (default 350 ms; options 10/350/700/1200 ms) | `test_zone_hub.c: test_debounce_response_time, test_wired_terminals`; HIL |
| ZONE-005 | Wired EOL supervision (optional): short = fault, nominal = normal, open = trouble (fire) or tamper (burglary) | HIL test with resistors |
| ZONE-006 | A simulator source (serial console + developer UI screen) can drive any zone bound to it | `test_zone_hub.c: test_simulator_source`; console `zone <n> <event>` |
| ZONE-007 | Supervised zones raise CHECK when no event or check-in arrives within `supervision_s` | `CORE-TRBL-007` |
| ZONE-008 | A source that loses contact (radio link, expander, keypad) reports a system or device trouble, not zone faults | `test_zone_hub.c: test_link_down_is_not_check, test_source_link_trouble` (ZSRC-007, ZSRC-015); keypad inputs with M7 |
| ZONE-009 | Zone state is reported with fault, bypass, alarm, trouble, tamper, low battery, supervision, not-ready flags | `alarm_engine_zone_status` |

## Design

### Zone configuration
Stored in `alarm_config_t.zones[]` (see `alarm_config.h`). Platform-only data (which input feeds
the zone) lives in a separate **zone source table** in NVS so the core stays hardware-agnostic.
**Superseded:** the source interface, binding blob and mapping are now specified in
[22 · Zone sources](../22-Zone-Sources/README.md) (Zigbee, wired, Home Assistant, RF, Z-Wave,
keypad, simulator); the struct below was the first sketch:

```c
typedef struct {
    uint8_t  kind;       // NONE, SIM, GPIO, GPIO_EOL, EXPANDER, KEYPAD_INPUT, RADIO
    uint8_t  bus;        // expander / keypad device number
    uint16_t channel;    // GPIO number, expander pin, keypad input index
    uint64_t radio_id;   // RADIO: sensor EUI-64 / serial
    uint8_t  radio_loop; // RADIO: input on the sensor (its "loop")
    uint8_t  response;   // debounce preset
    uint8_t  eol;        // NONE, SINGLE, DOUBLE
} zone_source_t;
```

### Sources

| Source | Milestone | Notes |
|---|---|---|
| **Simulator** | M2, behind the interface in M11 | Console commands (`zone 3 open`, `zone 3 tamper`, `zone 3 unreachable`) for zones bound to it; every zone of a panel from before M11 is bound to it on the first start (docs/features/22 ZSRC-014). `CONFIG_ALARM_ZSRC_SIM` |
| **Wired GPIO** | M11 | CrowPanel J7 header GPIO 2/3/4/5/49-52 (3.3 V, not 5 V tolerant; J7's GPIO 25 is the P4's USB D+ and 27/28 become the wireless socket's UART on a V1.2 board, so they are not offered; 53/54 are the socket's UART, not on J7; see [06](../06-CrowPanel-P4-Platform/README.md)). A CC1101 placed on J7 (`rf place j7`, docs/features/25 RF-019) takes 5 and 49-52. Dry contacts to GND with the internal pull-up, sampled every 10 ms, debounced with the zone's response time; normally closed (recommended) or normally open; a pin is set up only when a zone uses it. `CONFIG_ALARM_ZSRC_WIRED` |
| **Wired EOL** | later (BL-063) | Needs ADC-capable pins and a resistor network; lets fire loops detect open circuits |
| **I2C expander** | later | e.g. MCP23017 on the I2C_OUT connector (shared with the GT911 touch controller, 400 kHz) |
| **Keypad-attached** | M7 | A remote keypad's own inputs, reported over the panel link like a zone expander |
| **Zigbee (H2 radio)** | M10 | First-class wireless sensors; see [20](../20-ESP32-H2-Sensor-Radio/README.md) |
| **Home Assistant** | M13 | Any HA sensor entity, P4 only; see [23](../23-Home-Assistant-Integration/README.md) |
| **315/345/433 MHz RF** | M14 | EV1527 and the sensors of older keypad alarm panels: five 433 MHz families built (EV1527 / PT2262 among them), the decoders for those older sensors still to come (BL-039); see [25](../25-RF-Sensors-315-345-433/README.md) |
| **Z-Wave (Zooz ZAC93)** | its own track, Z1-Z11 | Z-Wave devices on the panel's own network, each input of a device a zone of its own; see [36](../36-Z-Wave-Sensors-ZAC93/README.md) |

Hardware choices (which pins, expanders, resistor values) are yours; the software keeps them
in the source table so no code changes are needed.

### Enrollment flow (installer mode)
The first sketch. As built (M11), the zone editor asks "Where is the sensor?" and offers the
sources that can be used now ([22](../22-Zone-Sources/README.md), `ui_sources.c`).

1. Zones -> Add zone -> choose number (next free), type, partition, name, device class.
2. Choose the source: *Wired* (pick a pin), *Keypad input*, *Radio* ("learn": fault the
   sensor, the radio reports its ID) or *Simulator*.
3. Options: chime, night zone, bypassable (24 h), swinger exempt, cross-zone partner,
   supervision (default 12 h for radio burglary sensors, 4 h for radio life-safety sensors,
   off for wired).
4. Test: the zone's live state is shown; walk test confirms it.

## Interfaces and data
- Core API: `alarm_engine_zone_event()`, `alarm_engine_zone_status()`, zone flags `ALARM_ZS_*`.
- Alarm task queue message: `{ MSG_ZONE, zone, event }`, posted by `alarm_service_zone_event()`;
  the alarm task takes the time when it handles it.
- NVS: `alarm`/`config` (core config blob), `zones`/`bind` (the binding blob, versioned:
  [22](../22-Zone-Sources/README.md)).

## Test and acceptance
- Core behaviour: `components/core/alarm_core/test` (all zone types covered, see 01).
- Zone sources: `components/core/zone_hub/test/test_zone_hub.c` (the hub, the simulator, the
  wired terminals and their response time, Home Assistant), `test_rf.c`, `test_zigbee.c`,
  `test_zsrc_zwave.c`.
- Simulator: scripted console scenarios reproduce the core scenarios on hardware.
- HIL checklist: each wired input faults and restores within its response time; bouncing
  contacts don't produce double events; EOL open/short detected.

## Open questions
- Which inputs will you wire first (GPIO header vs. I2C expander)? The wired source built in
  M11 ([22](../22-Zone-Sources/README.md)) uses the J7 header's free pins
  (`BOARD_WIRED_TERMINALS`); an I2C expander stays a later option.
- EOL style (none / single 2k2 / double) for wired zones. Decided: a single
  end-of-line resistor of 2 kΩ, read as normal, open or short; the J7 pins
  that can read it come from the board description when it is built (BL-063).
- Should the simulator be available in release builds (installer-only), or dev builds only?
  Partly answered in [22](../22-Zone-Sources/README.md)'s open questions: the release profile
  keeps `CONFIG_ALARM_ZSRC_SIM` but has no service console to drive it; whether release turns
  it off once real sensors replace the demo zones was open there. Decided:
  release builds leave the simulator out and start without demo zones; dev and legacy builds
  keep it (BL-106).

## Tasks
- [x] Zone model and all zone types in the core
- [x] Zone programming on the touchscreen: add, name, type, sensor kind, chime, night, bypassable, report, remove (M4, installer mode)
- [x] Zone source framework, bindings + NVS persistence ([22](../22-Zone-Sources/README.md), M11)
- [x] Simulator source: serial console (`zone <n> open|close|trouble|tamper|lowbat|checkin|unreachable|reachable`) with eight demo zones (M2; behind the interface in M11)
- [ ] Developer screen with a toggle per zone (the console's `zone <n> <event>` covers this on a
      panel; the PC simulator has buttons for the demo zones) (BL-074)
- [x] GPIO scanner with debounce (M11: the wired source, 10 ms sampling, per-zone response time)
- [x] Enrollment UI: "Where is the sensor?", live terminal picker, Source / Contact / Response time rows (M11)
- Duplicate of docs/features/22 ("On the panel: wire a test contact on J7 and add it on the
  touchscreen, with `display watch 60` while the terminal picker is open", BL-029): the first
  wired contact on a panel.
- Duplicate of docs/features/22 ("EOL supervision on wired inputs", BL-063): ZONE-005's EOL style
  and ADC-capable pins.
- [ ] Keypad-attached inputs over the panel link (M7, docs/features/11) (BL-048)
- [x] Home Assistant source, `zsrc_ha` (M13, docs/features/23): `zone_hub/src/zsrc_ha.c`,
      `test_zone_hub.c` `test_ha_*`
- [x] 433 MHz source, `zsrc_rf` (M14, docs/features/25): five protocol families,
      `zone_hub/src/zsrc_rf.c`, `test_rf.c`
- [x] Zigbee source, the portable half `zsrc_zigbee` (M10, docs/features/20):
      `zone_hub/src/zsrc_zigbee.c`, `test_zigbee.c`
- Duplicate of docs/features/20 (`zigbee_host` on the P4 with the H2 RCP; real pairing
  acceptance, BL-036): Zigbee's radio half.
- [x] Z-Wave source, `zsrc_zwave` (docs/features/36): `zone_hub/src/zsrc_zwave.c`,
      `test_zsrc_zwave.c`
- Duplicate of docs/features/22 ("Home Assistant entities as zones on a panel", BL-027) and
  docs/features/25 (the bench fixtures and low-battery code, BL-022; more families, BL-039): the
  rest of the Home Assistant and 433 MHz sources.
