# zigbee_zones

**Tier:** core (portable C11) · **Features:** [20 Zigbee sensors (ESP32-H2 radio)](../../../docs/features/20-ESP32-H2-Sensor-Radio/README.md) ([devices.md](../../../docs/features/20-ESP32-H2-Sensor-Radio/devices.md), [alarm-devices.md](../../../docs/features/20-ESP32-H2-Sensor-Radio/alarm-devices.md), [testing.md](../../../docs/features/20-ESP32-H2-Sensor-Radio/testing.md)) ·
**Tests:** [`test/`](test/) (3 suites: `zbz_contract`, `zbz_zcl`, `zbz_registry`)

The panel's Zigbee devices, beside the zones: `zone_hub`'s `zsrc_zigbee` keeps one slot per
device endpoint for the zones, and this component keeps the devices themselves - the way
`zwave_zones`' `zwz_panel` sits beside `zsrc_zwave`. The PC simulator, the UI preview, the host
tests and the firmware's `zigbee_service` share it. `zone_hub` stays free of it; nothing in the
core depends on it.

**State: ZB-M2's codec and registry built (BL-143).** `src/zbz_zcl.c` (the ZCL header of both
frame types, the global commands' attribute records, Configure Reporting, Default Response and
IAS Zone's commands, with golden frames in `test_zbz_zcl.c`) and `src/zbz_registry.c` (the device
table, its NVS records and the take/redo write-back, read against the frozen
`test/registry_fixture_v1.h` in `test_zbz_registry.c`) are built; the other headers are still
contracts whose sources come milestone by milestone (docs/features/20 "Milestones"). The library
is a static library on the PC and a normal component in ESP-IDF.

| Header | What | Milestone |
|---|---|---|
| `zbz_registry.h` | one record per device (name, model, how it joined, endpoints, setup state), one NVS key per device in namespace `zbpanel`, the codec, the take pattern for writes | ZB-M2 |
| `zbz_zcl.h` | ZCL frames as bytes: the header, IAS Zone's CIE write, enrollment, status and test mode | ZB-M2 |
| `zbz_codes.h` | install codes: CRC-16/X.25, typed and QR text, the waiting list and its blob | ZB-M4 |
| `zbz_models.h` | per-model names, pairing and wake words, siren and keypad encodings (data) | ZB-M3 |
| `zbz_panel.h` | the adapter: setup state machine, one operation at a time, `ui_backend_t`'s `zb_*` view and requests, events, storage | ZB-M3, ZB-M4 |
| `zbz_wd.h` | IAS WD sirens and strobes as the panel's bell, squawks, bell and device supervision | ZB-M10 |
| `zbz_ace.h` | IAS ACE keypads and key fobs: codes checked by the engine through the alarm task | ZB-M11 |

Rules it keeps (docs/features/20 devices.md §1): calls come with the zone service's lock held,
from the Zigbee task, never from a stack callback; the radio is reached only through
`zsrc_zb_radio_t`; the engine only through what the owner posts to the alarm task; NVS is written
by the owner after unlocking, from a task with an internal-RAM stack (docs/bugs/B001); stored
formats only add fields and get a frozen fixture when they change.

**Depends on:** `zone_hub` (the radio contract), `ui_backend` (the pages' types), `alarm_core`
(alarm types). **Options:** none of its own; built into the firmware with `zigbee_service`
(ADR-047 makes Zigbee part of every P4 image, BL-146).
