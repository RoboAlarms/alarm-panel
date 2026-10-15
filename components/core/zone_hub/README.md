# zone_hub

**Tier:** core (portable C11) · **Features:** [22 Zone sources](../../../docs/features/22-Zone-Sources/README.md), [02 Zones and sensor inputs](../../../docs/features/02-Zones-and-Sensor-Inputs/README.md), [20 Zigbee sensors](../../../docs/features/20-ESP32-H2-Sensor-Radio/README.md), [25 RF sensors](../../../docs/features/25-RF-Sensors-315-345-433/README.md), [23 Home Assistant integration](../../../docs/features/23-Home-Assistant-Integration/README.md), [36 Z-Wave sensors](../../../docs/features/36-Z-Wave-Sensors-ZAC93/zones.md) ·
**Tests:** [`test/`](test/) (4 suites: `zone_hub`, `rf`, `zigbee`, `zsrc_zwave`)

Everything between a sensor and a zone. Sources (the simulator, wired terminals, Home Assistant
entities, Zigbee devices, 433 MHz sensors, Z-Wave devices) implement `zone_source_ops_t` and report
by address through a `zsrc_sink_t`. The hub owns the binding table, maps each report onto a zone
event (binary, a list of states, or a threshold with hysteresis), runs the grace timers that turn an
unreachable sensor into CHECK, supervises each source's link, and stores the bindings as a versioned
blob. It exists so that the alarm core never learns where a zone's sensor is (ZSRC-001), and so that
each source's rules are proved on a PC. It touches no hardware: pins come through
`zsrc_gpio_port_t`, the Zigbee radio through `zsrc_zb_radio_t`, RF frames, Home Assistant states and
Z-Wave devices' decoded facts and reports (from `zwave_zones`) are handed in by the platform, and
zone events leave through `zone_hub_port_t` for the platform to queue to the alarm task. No heap, no
globals, no RTOS; not thread-safe (the platform serialises calls).

## Public API
| Header | What it offers |
|---|---|
| `include/zone_hub/zone_source.h` | The source interface: `zsrc_kind_t`, capabilities (`ZSRC_CAP_*`), `zsrc_address_t`, mapping modes and roles, `zsrc_sink_t`, `zone_source_ops_t` (with the optional per-sensor `caps_of` and `grace_of`, the hub's `group` of a zone's inputs and a source's watch `room`), trust helpers (`zsrc_trusted_for`) |
| `include/zone_hub/zone_hub.h` | The hub: register sources, bind and unbind zones, report inputs, grace and CHECK, source links, resync, learn and search candidates, the binding blob (`zone_hub_encode` / `_decode`) |
| `include/zone_hub/zone_debounce.h` | Response time for sampled inputs (10 / 350 / 700 / 1200 ms, ZONE-004) |
| `include/zone_hub/zsrc_sim.h` | Simulator source: the service console and the PC preview drive any zone bound to it |
| `include/zone_hub/zsrc_wired.h` | Wired terminals: dry contacts sampled every 10 ms, debounced, learnt by opening and closing |
| `include/zone_hub/zsrc_ha.h` | Home Assistant entities: the catalog the paired integration shares, watched entities, "unavailable" into grace and CHECK |
| `include/zone_hub/zsrc_zigbee.h` | Zigbee sensors: device table, IAS Zone enrollment, cluster mapping, quirks table, supervision, jamming |
| `include/zone_hub/zsrc_zwave.h` | Z-Wave devices (docs/features/36 zones.md): the address (`zsrc_zwave_address`: home ID, generation, node ID; end point and slot), the slots, the device facts and decoded reports `zwave_zones` hands in (`zsrc_zw_desc_t`, `zsrc_zw_report_t`), the quirks table (`zw_quirks.c`), per-device trust and grace, fresh-state requests (`zsrc_zw_take_refresh`) |
| `include/zone_hub/zsrc_rf.h` | One-way 433 MHz sensors (EV1527 / PT2262, HT6P20B, HT12E, KeeLoq, arctech): learnt codes, zones on confirmed frames, holds, check-ins, rolling-code replay refusal, bursts, jamming over a minute |

## Depends on
- Project components: `alarm_core` (zone events, zone types, device classes; `alarm_crc32()`
  for the blob); the tests also link its `alarm_sim` harness. Not `rf_decode`: `zsrc_rf` takes
  decoded packets and repeats `rf_proto_t`'s numbers as `ZSRC_RF_PROTO_*`.
- ESP-IDF: none (portable).

## Options
No Kconfig here. The panel chooses which sources to register in `platform_esp`'s Kconfig
(`ALARM_ZSRC_SIM`, `_WIRED`, `_HA`, `_ZIGBEE`, `_RF`); this component always compiles all of them.

| Macro (`-D`) | Default | Effect |
|---|---|---|
| `ZONE_HUB_MAX_SOURCES` | 8 | Sources the hub can register |
| `ZONE_HUB_MAX_INPUTS` | `ALARM_MAX_ZONES + 16` | Binding slots: main inputs plus separate tamper and battery inputs |
| `ZONE_HUB_KEEP_BYTES` | 1024 | Room for blob items from a newer firmware, kept and written back |
| `ZSRC_REF_LEN` | 96 | Text address incl. NUL (a Home Assistant entity_id), 1-256; `alarm_proto`'s `ZSRC_REF_LEN_LINK` repeats it |
| `ZSRC_STATES_LEN` | 48 | State list of a `ZSRC_MAP_STATES` mapping incl. NUL, 1-256 |

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; time comes in as `uint64_t` ms;
  the host build stays warning-free.
- `zsrc_kind_t`, `zsrc_role_t`, `zsrc_map_mode_t` and the binding blob's field ids
  (`src/bind_codec.c`) are stored in NVS (`zones`/`bind`): never renumber, only add. Every
  field's zero is its default, and zeros aren't written.
- When the blob format changes, add a new frozen fixture next to `test/zone_bind_fixture_v1.h`;
  never edit one.
- Sources report by address through a `zsrc_sink_t` and never see zones or the engine.
- `zsrc_rf`'s protocol numbers (`ZSRC_RF_PROTO_*`, the same as `rf_proto_t`) and its address
  layout (id = protocol << 32 | device, input = code + 1) are stored in bindings too.
- `zsrc_zwave`'s address layout (id = home ID << 32 | generation << 16 | node ID, input = end
  point << 5 | slot) and its slot numbers (`zsrc_zw_slot_t`, zones.md §2) are stored in
  bindings: append only. It depends on neither `zwave_host` nor `zwave_proto`.
- Tests reference `ZSRC-NNN` (docs/features/22); `test/test_rf.c` references `RF-NNN` (docs/features/25),
  and so does `test/test_rf_devices.c`: the 433 MHz products people buy as mock devices
  (`test/rf_mock.c`), played as pulses through the real decoder into this source (docs/features/25
  "Mock devices"; a new product is a profile there, with its sources).
  (CONTRIBUTING.md, "Rules for the portable components")

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R <suite>`.
- `zone_hub`: registry, bindings, every mapping mode, grace and CHECK, source links and their
  supervision, resync with the real engine, the blob (round trip, frozen fixture, kept items,
  damage, worst case), the debouncer, and the simulator, wired (fake pins) and Home Assistant
  sources. Tags `ZSRC-NNN`, `ZONE-NNN` (docs/features/02), `HAI-009`.
- `rf`: learning codes, closed codes and holds, tamper and low-battery codes, repeats, bursts,
  jamming, a receiver that goes missing. Tags `RF-003`..`RF-006`, `ZSRC-NNN`. `-R` is a regular
  expression: `-R '^rf$'` leaves out `rf_decode`.
- `zigbee`: pairing windows, IAS enrollment with retries, status mapping, non-IAS clusters,
  battery, supervision, jamming, the quirks table. Tags `ZB-NNN` (docs/features/20), `ZSRC-NNN`.
- `zsrc_zwave` (34 tests): addresses and generations (a node ID given out again never feeds the
  old binding; a new network's none; a replaced device's its own), the slots a device offers,
  every row of zones.md §3 (states and idle, tamper, moved, momentary holds, life-safety
  trouble, tests and heartbeats, Power Management, Binary and Alarm Sensor, battery thresholds
  and "disconnected", temperature, Basic), Binary Sensor type `FF` ("Any" - the endpoint's one
  supported type, else dropped and counted), the Access Control tilt parameter (no effect),
  Notification status `FE` never idling an active state, Multi Channel endpoint isolation (a
  report never credited to another endpoint's zone; a root report is endpoint 0's alone),
  check-ins and each device's grace, trust per device, the link and fresh state - a hold or a
  condition that ends during a break is told when the device next speaks - the picker, the
  limits and the quirks table. Tags `ZW-013`..`ZW-015` (docs/features/36), `ZSRC-NNN`.
- All four drive the real hub and link `alarm_sim`; `zone_hub` and `zigbee` also send zone
  events on to a real engine. `DUMP_FIXTURE=1 build/host/test_zone_hub` prints
  a new blob fixture.
