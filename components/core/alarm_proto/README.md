# alarm_proto

**Tier:** core (portable C11) · **Features:** [10 MQTT and Home Assistant](../../../docs/features/10-MQTT-and-Home-Assistant/README.md), [13 OTA updates](../../../docs/features/13-OTA-Updates/README.md), [23 Home Assistant integration](../../../docs/features/23-Home-Assistant-Integration/README.md) ·
**Tests:** [`test/`](test/) (5 suites: `proto`, `report`, `link`, `pair`, `fw`)

The panel's messages to the outside world, as functions over caller-owned buffers: MQTT topics
and JSON payloads (partition status, zones, troubles, events, heartbeat), command parsing and
results, Home Assistant MQTT discovery, the store-and-forward reporter that replays
unacknowledged events from the event log, the firmware-update helpers (versions, release
manifest, install policy, serial upload framing), and the local Home Assistant link (framing,
hello, pairing, SHA-256). A small JSON writer and a strict, bounded reader sit underneath. It
exists so that every byte the panel sends or accepts is host-tested against golden payloads. It
opens no socket and keeps no connection: `net_mqtt`, `ha_link` and `panel_ota` own the
transport, TLS, clocks and randomness. No heap, no globals, no RTOS.

## Public API
| Header | What it offers |
|---|---|
| `include/alarm_proto/proto_json.h` | JSON writer into a fixed buffer (overflow remembered, checked once with `pj_finish()`) and a strict RFC 8259 reader limited to `PJ_MAX_DEPTH` (8) |
| `include/alarm_proto/proto.h` | Identity and topics, the Home Assistant panel state, status / zone / trouble / event / heartbeat payloads, `proto_cmd_parse` / `proto_ha_cmd_parse` and results, discovery documents |
| `include/alarm_proto/proto_report.h` | Store-and-forward: publishes the event log's "reported" markers in order, one at a time, moves on at the broker's acknowledgement (QoS 1), and says when `comm_fail` is due (`PROTO_COMM_FAIL_MS`, 5 min) |
| `include/alarm_proto/proto_fw.h` | Semantic versions, release-manifest pick, `proto_fw_install_allowed()`, base64 and upload-line decoding, the `…/firmware` state JSON and the Home Assistant `update` entity |
| `include/alarm_proto/ota_retry.h`, `ota_profile.h` | The update task's retry schedule after a failed release check (10 min doubling to 6 h, OTA-015) and which signed project names may replace the running one (DEV-004/015); header-only, moved here from `panel_ota` in docs/features/33 step 8 |
| `include/alarm_proto/proto_link.h` | Local Home Assistant link v1: SHA-256, hex, frame header, pairing code, message writers (hello, snapshot, delta, event, result, watch, call) and parsers (catalog, state, call result, incoming messages) |
| `include/alarm_proto/proto_zw.h` | The Z-Wave devices the panel switches, as Home Assistant switch, light and cover entities (MQTT-018..020) |
| `include/alarm_proto/proto_zb.h` | The Zigbee network's diagnostics: `~/zigbee/state`, its Home Assistant sensor, and a zone's device battery and signal (MQTT-021, MQTT-022) |
| `include/alarm_proto/proto_pair.h` | The panel's pairing state machine: 2-minute window, commitment check, the code, allow or reject |

## Depends on
- Project components: `alarm_core` (configuration, snapshot, events, `alarm_strings.h`,
  `alarm_crc32()`), `event_log` (`elog_entry_t` and `ELOG_KIND_REPORTED` for the reporter).
- ESP-IDF: none (portable).

## Options
None. The limits in the headers are fixed, not `-D` options: `PROTO_PAYLOAD_MAX` (4096),
`PROTO_TOPIC_LEN` (112), `PJ_MAX_DEPTH` (8), `PROTO_LINK_FRAME_MAX` (65536),
`PROTO_LINK_SNAPSHOT_MAX` (49152). `ZSRC_REF_LEN_LINK` (96) repeats `zone_hub`'s `ZSRC_REF_LEN`
and must stay equal to it.

## Rules
- Plain C11 like `alarm_core`: no malloc, no globals, no RTOS or ESP-IDF headers; the host
  build stays warning-free.
- Topics, JSON keys and discovery ids are an external contract (docs/features/10): never rename,
  only add, and update the golden payloads in `test/test_proto.c` with them.
- Every payload must fit `PROTO_PAYLOAD_MAX`; incoming JSON goes through the strict reader only.
- The link's message keys, framing and pairing derivation are shared with the aioroboalarms
  Python client, with the same golden vectors in both test suites: never rename or reuse, only
  add (`proto_link.h`; docs/features/23 `protocol.md`).
- Tests reference `MQTT-NNN`; the firmware-update helpers (`proto_fw.h`) are tested in
  `test/test_fw.c` with `OTA-NNN` tags (docs/features/13).
  (CONTRIBUTING.md, "Rules for the portable components")

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R <suite>`.
- `proto`: JSON writer and reader including hostile input, golden payloads, command parsing,
  discovery, payload sizes. Tags `MQTT-NNN`.
- `report`: order, acknowledgement, replay after a reconnect and a restart, `comm_fail`, a
  marker the log never kept; on a real `event_log` in RAM with a scripted broker. Tags
  `MQTT-NNN`, `LOG-006`.
- `fw`: versions, the manifest, the install policy, upload framing, the update entity, plus
  `panel_ota`'s retry schedule and signed-profile rules (plain C headers included from
  `components/services/panel_ota`). Tags `OTA-NNN`, and `DEV-004` / `DEV-015` (docs/features/30
  `DESIGN.md`).
- `link`, `pair`: SHA-256 against the FIPS vectors, framing, the pairing code (golden vectors
  shared with aioroboalarms), message writers and parser; the pairing state machine. Tags
  `HAI-NNN` (docs/features/23).
