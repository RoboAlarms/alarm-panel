# 10 · MQTT and Home Assistant

**Status:** Implemented v1 - the messages, discovery, commands and store-and-forward are
host-tested in `alarm_proto`, with the firmware update entity (M6) and the switched Z-Wave
devices (MQTT-018..020); the client and its Settings page are built. Part 1 of the hardware
check passed on a panel on 2026-09-29 with Mosquitto (TLS, own CA) and Home Assistant
2026.9 ("On a panel" below; B018 and B019 fixed on the way); the code scenarios,
MQTT-017's fault injections and the Z-Wave devices are still to run (BL-026), and a broker
restart outruns Home Assistant's reconnect (BL-112, a decision); event log export and
configuration backup and restore over MQTT are open (BL-054). · **Milestone:** M5 · **Depends on:** 01, 05, 09, 14, 15

## Goal
Publish the panel's state and events over **MQTT over TLS** using our own documented JSON
messages, and accept authorised commands. For Home Assistant the **recommended** path is now
the native integration from HACS ([23](../23-Home-Assistant-Integration/README.md)): discovered
in Devices & services, paired on the panel, no broker, and two-way (HA entities as zones, a
Devices screen). MQTT stays for Node-RED, your own server, and HA users who skip HACS; while the
native integration is paired, the panel turns MQTT discovery off so HA never sees the panel
twice. **Home Assistant** integrates directly through MQTT discovery, with no server of ours in
between. A custom server you write later is optional and simply another MQTT client using the
same topics.

No legacy alarm protocols (Contact ID, SIA DC-09) are used.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| MQTT-001 | Connect to a user-configured broker over TLS (default port 8883), verifying the server with the public certificate bundle or a certificate authority of your own; user name and password | integration (client certificates: later) |
| MQTT-002 | Plain MQTT (port 1883) only with an explicit "Not encrypted" choice, shown with a warning in the UI | preview (`44_mqtt_not_encrypted.png`), review |
| MQTT-003 | Availability: LWT `offline` (retained) on `…/availability`; `online` after connect; `offline` before a deliberate disconnect | integration |
| MQTT-004 | Retained partition status, HA state and zone state topics, updated on change (debounced 200 ms) | `test_proto.c` (payloads), integration |
| MQTT-005 | Every reportable engine event is published on `…/event` (QoS 1, not retained), in the order it became due, with its sequence number | `test_report.c`, `test_proto.c` |
| MQTT-006 | Store-and-forward: events not acknowledged by the broker are replayed from the event log after a reconnect or restart, marked `replayed` ([05](../05-Event-Log/README.md)) | `test_report.c`, integration |
| MQTT-007 | Commands on `…/cmd` (JSON) and the HA command topic are executed as `ALARM_SRC_REMOTE` commands: codes checked by the core, lockout per client, no auto-stay | `test_proto.c` (parsing), `CORE-ARM-*`, integration |
| MQTT-008 | Disarm always needs a valid user code; arming without a code only if `remote_arm_without_code` is enabled | `test_proto.c`, `CORE-ARM-006`, integration |
| MQTT-009 | Home Assistant discovery (retained, `homeassistant/…`): one `alarm_control_panel` per partition, one `binary_sensor` per zone, a trouble sensor, an `event` entity; retracted when a zone is deleted, discovery is turned off or the prefix / panel id changes | `test_proto.c`, HA test instance |
| MQTT-010 | Duress and silent alarms never change the HA panel state; they're published as events (`silent: true`) so automations can notify discreetly | `test_proto.c` |
| MQTT-011 | The `comm_fail` trouble is raised when a reportable event waits without progress for 5 min or the broker is unreachable for 5 min (only when MQTT is configured) | `test_report.c`, integration |
| MQTT-012 | Heartbeat: status re-published at least every 10 min (retained); `…/heartbeat` every 60 s with uptime and versions | `test_proto.c` (payload), integration |
| MQTT-013 | Topic prefix configurable (default `alarmpanel`); panel id default = last 6 hex digits of the MAC | `test_proto.c` |
| MQTT-014 | Read-only mode: commands off, nothing subscribed, and Home Assistant offers no arming | `test_proto.c` (discovery), integration |
| MQTT-015 | Incoming JSON is parsed strictly (RFC 8259) with bounded depth and sizes; malformed or oversized input is rejected without side effects; outgoing JSON is escaped and never overflows its buffer | `test_proto.c` |
| MQTT-016 | The connection is set up on the touchscreen under Settings > Integrations > **MQTT**, an integration in its own right beside Home Assistant, not a page under it (decided). System master or installer code (docs/features/14); its words assume no particular system, and no screen outside Settings > Integrations names Home Assistant | preview (`34`, `40`-`44`), review |
| MQTT-017 | Every accepted report either reaches the broker or leaves a fault behind: one the event log could not keep is published live instead of waiting for a replay that can never come, and one that can't even go live is counted as undelivered and latches the `storage` trouble ([05](../05-Event-Log/README.md) LOG-010) | `test_report.c: test_lost_marker_is_never_replayed`, `test_elog.c: test_failed_append_is_retried`; broker and flash fault injection |
| MQTT-018 | The Z-Wave devices the panel switches have ids from the network's home ID, the device's node ID and generation and the end point (`zw_<home>_<node>_<generation>_<endpoint>`): the same after a restart, a new one when a node ID is given to another device; topics `~/zwave/<key>/state` (retained JSON), `~/zwave/<key>/available` (retained `online`/`offline`), `~/zwave/<key>/set` (docs/features/36 ZW-036) | `test_zwdev.c: test_keys_and_topics` |
| MQTT-019 | Their discovery: a `switch` (a switch or relay), a `light` (JSON schema, brightness 0..100), a garage door as a `cover` (device class garage) that closes but never opens from Home Assistant (`payload_open` and `payload_stop` null); available only while the panel is online and the device answers; state as the device last said (unknown as null / "None"); retracted when the device goes or discovery is off - the announced list is kept in NVS (`zwdisc`) so a device removed while the panel was off goes too | `test_zwdev.c: test_discovery`, `test_state`; broker test pending |
| MQTT-020 | Commands are parsed strictly for their kind (`ON`/`OFF`; a light's `{"state","brightness"}`; a garage's `CLOSE` only). A **retained** command - one the broker kept from some time ago - is never run, for the partitions and `/cmd` too; a Z-Wave command delayed past 5 s never switches | `test_zwdev.c: test_commands`; `test_zwz_panel` (stale commands) |
| MQTT-021 | The Zigbee network's state, retained on `~/zigbee/state`: `radio` (its state's word), `channel` (null without a network), `devices`, `not_heard`, `jammed`; in Home Assistant one diagnostic sensor "Zigbee" with the payload as attributes (docs/features/20 ZB-026) | `test_proto.c: test_zigbee_state_golden` |
| MQTT-022 | A zone fed by a Zigbee device carries the device's `battery` (percent) and `signal` (`good`, `fair`, `weak`) in its zone state, each only when known, after the keys every zone has: attributes of the zone's existing entity, no entity per device | `test_proto.c: test_zone_diag_golden` |

## Topics

Base: `alarmpanel/<panel_id>`

| Topic | Dir | Retain | QoS | Payload |
|---|---|---|---|---|
| `…/availability` | out | yes | 1 | `online` / `offline` (LWT) |
| `…/partition/<n>/status` | out | yes | 1 | partition status JSON |
| `…/partition/<n>/state` | out | yes | 1 | HA state string (`disarmed`, `arming`, `armed_home`, `armed_away`, `armed_night`, `pending`, `triggered`) |
| `…/partition/<n>/set` | in | - | 1 | HA command: `{"action":"ARM_AWAY","code":"246810"}` |
| `…/zone/<z>/state` | out | yes | 1 | zone JSON |
| `…/troubles` | out | yes | 1 | active + unacknowledged troubles JSON |
| `…/event` | out | no | 1 | event JSON (Home Assistant's event entity reads `event_type`) |
| `…/cmd` | in | - | 1 | command JSON |
| `…/cmd/result` | out | no | 1 | result JSON |
| `…/heartbeat` | out | no | 0 | `{"uptime_s":…,"fw":"0.5.0","ip":"…","rssi":-58,"free_heap_kb":…,"boot":…,"unsent":0}` |
| `…/firmware` | out | yes | 1 | firmware state for the HA `update` entity (M6): `{"installed_version":"0.6.0","latest_version":"0.6.1","title":"RoboAlarms Panel firmware","release_url":"…","in_progress":false,"update_percentage":null}`; `release_url` only when the panel knows one (Home Assistant drops the whole payload over a null, B019) |
| `…/firmware/set` | in | - | 1 | `install`: download the newest release and restart into it once the system is idle (only with commands on) |
| `…/zigbee/state` | out | yes | 1 | the Zigbee network (MQTT-021): `{"radio":"working","channel":15,"devices":5,"not_heard":0,"jammed":false}` |

### Payload examples
These are the golden payloads of `components/core/alarm_proto/test/test_proto.c` (keys and values are an external
contract: never renamed, only added). Partition status (`delay_remaining_s`, `walk_test_remaining_s` and `ts` are taken
when published; `walk_test_tested`, `walk_test_total` and `walk_test_remaining_s` are the walk test's
zones tested, zones it covers and seconds left, all 0 when none runs; docs/features/40 WALK-019):
```json
{"partition":1,"name":"House","state":"armed","ha_state":"triggered","level":"away",
 "no_entry_delay":false,"silent_exit":false,"ready":false,"ready_levels":["stay","night"],
 "delay_remaining_s":0,"triggered":true,"exit_error":false,"chime":false,"walk_test":false,
 "walk_test_tested":0,"walk_test_total":0,"walk_test_remaining_s":0,"installer_mode":false,"armed_by":{"user":3,"name":"Alex"},"memory":["burglary"],
 "memory_canceled":false,"faults":1,"bypassed":0,"alarms":1,"troubles":0,"trouble_beeps":false,
 "sounder":"alarm","ts":"2026-09-19T22:41:00Z"}
```
Zone state:
```json
{"zone":3,"name":"Living Window","type":"perimeter","device_class":"window","partition":1,
 "open":true,"bypassed":false,"alarm":false,"trouble":false,"tamper":false,
 "low_battery":true,"supervision":false,"not_ready":true}
```
A zone fed by a Zigbee device adds what the device last said, each only when known (MQTT-022):
`…,"not_ready":true,"battery":87,"signal":"fair"}`.
The Zigbee network (MQTT-021; `radio` is `not_connected`, `starting`, `working`, `not_answering`,
`paused`, `socket_zwave` or `unusable`; `channel` is null while there is no network):
```json
{"radio":"working","channel":15,"devices":5,"not_heard":0,"jammed":false}
```
Troubles (`active` drives the Home Assistant problem sensor; restored ones wait for code + OFF):
```json
{"active":true,"count":3,"trouble_beeps":true,"system":["ac_loss"],"system_restored":["network"],
 "zones":[{"zone":3,"name":"Living Window","troubles":["low_battery"]}],"zones_more":0,
 "devices":[{"device":2,"name":"Hall Keypad"}]}
```
Event (`ts` is when it happened, `null` while the clock wasn't set; type-specific fields:
`level` and arm flags for `armed`, `cause` for `alarm`, `family` and `trouble` for troubles,
`result` for `arm_failed`, `target` for user changes, `recovery` / `timeout` / `log_erased`
for installer mode and factory reset; for `automation_failed` (docs/features/41 AUT-009, with
`"source":"automation"`, its uid in `source_id`), `automation` (its trigger clause, as on every
event an automation causes), `action` (`lock`, `close` or `arm`) and `reason` (`unreachable`, `no_answer`, `refused`, `missing`, `busy`, `not_allowed` or
`no_warning`), golden payloads in `test_proto.c: test_automation_failed_event_golden`):
```json
{"seq":1523,"ts":"2026-09-19T22:41:00Z","event_type":"alarm","partition":1,"zone":3,
 "zone_name":"Living Window","user":null,"user_name":null,"source":"zone","source_id":3,
 "cause":"burglary","silent":false,"replayed":false}
```
Command and result:
```json
{"id":"8f2c","action":"arm","level":"away","partition":1,"code":"246810","flags":["silent_exit"]}
{"id":"8f2c","action":"arm","result":"not_ready","user":null,"zone":3}
```
Actions: `arm` (level `away`, `stay` or `home`, `night`; flags `no_entry_delay`, `silent_exit`,
`bypass_faulted`), `disarm`, `bypass` and `unbypass` (`zone`), `chime`, `exit_restart`, `panic`
(`panic`: `police`, `silent`, `fire`, `medical`; refused with `disabled` unless the installer
allowed them: the console's `mqtt panic on`, or the Panel Manager's MQTT page). `partition` defaults to 1; `code` is up to 10 digits or null. Unknown keys are ignored.
Result names are the `alarm_result_name()` strings; anything malformed is `bad_arg`.

## Home Assistant discovery

Every entity is its own retained config at
`homeassistant/<component>/<prefix>_<panel id>/<object>/config` (objects `p1`..`p4`, `z1`..,
`trouble`, `event`, `firmware` under `update/` and `zigbee` under `sensor/`). Each carries the full device block (retained configs arrive in any order)
and uses `~` for the panel's base topic. Alarm panel (one per partition):
```json
{"name":null,"unique_id":"alarmpanel_a1b2c3_p1","~":"alarmpanel/a1b2c3",
 "availability_topic":"~/availability",
 "device":{"identifiers":["alarmpanel_a1b2c3"],"name":"Alarm Panel",
           "manufacturer":"RoboAlarms Panel (open source)","model":"CrowPanel Advanced 7\" (ESP32-P4)",
           "sw_version":"0.5.0"},
 "origin":{"name":"RoboAlarms Panel","sw_version":"0.5.0"},
 "state_topic":"~/partition/1/state","command_topic":"~/partition/1/set",
 "json_attributes_topic":"~/partition/1/status",
 "command_template":"{{ {\"action\": action, \"code\": code} | to_json }}",
 "code":"REMOTE_CODE","code_arm_required":true,"code_disarm_required":true,
 "code_trigger_required":false,"supported_features":["arm_home","arm_away","arm_night"]}
```
`code: REMOTE_CODE` makes HA show a keypad and pass the typed code through; the **panel**
validates it with the same rules as a keypad. `to_json` sends `null` when no code was typed.
`code_arm_required` follows `remote_arm_without_code`; read-only mode sends
`supported_features: []`; `trigger` appears only when panic from HA is allowed. With one
partition the entity takes the device's name (`alarm_control_panel.alarm_panel`), with more
each is named after its partition.

State mapping:

| Engine | HA state |
|---|---|
| `DISARMED` | `disarmed` |
| `EXIT_DELAY` | `arming` |
| `ARMED` + Stay / Away / Night | `armed_home` / `armed_away` / `armed_night` |
| `ENTRY_DELAY` | `pending` |
| `status.triggered` (audible/visible alarm, until disarm) | `triggered` |

Zones: `binary_sensor` per enrolled zone, on = open (`value_template` on `open`), the zone
JSON as attributes. `device_class` from the zone's class: door, window, garage_door, motion,
smoke, heat, carbon_monoxide, gas, moisture (water), cold (temperature), vibration, safety
(glass break, panic button); none for generic zones, key fobs and keyswitches. Troubles: one
`binary_sensor` "Trouble" (`device_class: problem`, diagnostic) with the troubles JSON as
attributes. Events: an MQTT `event` entity listing every `alarm_event_name()` in `event_types`
(docs/features/36 adds nine: `zwave_device_added`, `zwave_device_added_insecure`,
`zwave_device_removed`, `zwave_device_left`, `zwave_network_reset`, `zwave_radio_restarted`,
`zwave_device_software`, `zwave_garage`, `zwave_jamming_reported`; docs/features/20 adds nine
more: `zigbee_added`, `zigbee_removed`, `zigbee_forgotten`, `zigbee_left`, `zigbee_replaced`,
`zigbee_join_refused`, `zigbee_channel`, `zigbee_network_reset`, `zigbee_radio_restarted`;
docs/features/41 adds `automation_failed`).
Zigbee (MQTT-021): a `sensor` "Zigbee" at `homeassistant/sensor/<prefix>_<panel id>/zigbee/config`,
`entity_category: diagnostic`, state `{{ value_json.radio }}` from `…/zigbee/state` and the same
payload as attributes; a Zigbee device's battery and signal are attributes of its zone's
`binary_sensor` (MQTT-022), not entities of their own.
Firmware (M6, [13](../13-OTA-Updates/README.md)): an MQTT `update` entity "Firmware"
(`device_class: firmware`, `entity_category: config`) reading `…/firmware`; with commands on it
has `command_topic: ~/firmware/set` and `payload_install: install`, so HA's Install button starts
the update (the panel still waits until everything is disarmed). `latest_version` equals
`installed_version` unless the last release check found a newer one. The discovery map in NVS has
a bit for it, so it is retracted with the rest (golden payloads: `components/core/alarm_proto/test/test_fw.c`).

## Design (as built, M5)

### `components/core/alarm_proto` (portable C11, host-tested)
Same rules as `alarm_core`: no heap, no globals, no RTOS.
- `proto_json.h`: a JSON writer into a fixed buffer (escaping, UTF-8 checked, overflow
  remembered and reported by `pj_finish()`), and a strict reader (every escape, surrogate pairs,
  integers only where asked, depth limit 8, unread values skipped).
- `proto.h`: topics and identity (prefix and panel id rules, id from the MAC), the payloads
  above, the HA state mapping, command parsing (`proto_cmd_parse`, `proto_ha_cmd_parse`),
  result JSON, discovery documents. Every payload for a full panel fits `PROTO_PAYLOAD_MAX`
  (4 KB), which the tests check with 128 named zones in trouble and every user named.
- `proto_zb.h`: the Zigbee network's state payload and diagnostic sensor (MQTT-021) and the zone
  state with a device's battery and signal (`proto_zone_state_diag`, MQTT-022); the radio and
  signal numbers are ui_backend's `ui_zb_radio_t` and `ui_zb_signal_t`.
- `proto_report.h`: store-and-forward. The event log is the queue: every event that becomes due
  gets a "reported" marker (docs/features/05). The reporter publishes markers in log order, one at a
  time, and moves on only when the broker acknowledged (QoS 1 PUBACK); `acked` (a log index) is
  saved in NVS every few seconds. After a reconnect or restart everything after `acked` is sent
  again, marked `replayed` (at-least-once: consumers drop duplicates by `seq`). Stuck for 5
  minutes, or no broker for 5 minutes, raises `comm_fail`. A first setup (or switching MQTT on)
  starts from the newest record, so the history before isn't sent.

### `components/services/net_mqtt` (ESP-IDF)
- Client: `espressif/mqtt` 1.1.0 from the component registry (ESP-IDF 6 no longer ships
  esp-mqtt), MQTT 3.1.1, keepalive 30 s, reconnect every 10 s, 4 KB buffers, 64 KB outbox.
  Client id `alarmpanel-<panel id>`.
- TLS: `NET_MQTT_TLS_PUBLIC` verifies with the ESP-IDF certificate bundle (Let's Encrypt and
  other public CAs: the image keeps the common authorities, the full list comes as the
  certificates pack made for this version, docs/features/39 asset-packs.md),
  `NET_MQTT_TLS_OWN_CA` with a PEM stored in NVS (pasted on the service console, `mqtt ca`, or in
  the Panel Manager's MQTT page, docs/features/31),
  `NET_MQTT_TLS_OFF` is plain MQTT for a trusted home network.
- One task owns the client: state changes are debounced 200 ms and only changed payloads are
  sent (CRC per topic); everything retained is sent again every 10 minutes; heartbeat every 60 s;
  events through the reporter; commands executed with `alarm_service_command` as
  `ALARM_SRC_REMOTE` id 1 (one lockout slot for everything arriving over MQTT). Codes are wiped
  from memory after use and never logged. Incoming commands wait in a 16-deep queue in PSRAM
  (4 on a board without it): esp-mqtt delivers them holding the lock the task needs to publish
  each result, so a burst must fit rather than wait; one that doesn't is logged as dropped.
- Discovery bookkeeping in NVS (`dmap`, `dpfx`, `did`): what was announced under which prefix
  and panel id, so deleted zones, switching discovery off, or a new prefix / panel id retract
  the old retained configs (and a deleted zone's state topic).
- Connection problems are classified for the screen: address or port unreachable, user name or
  password refused, certificate not trusted, other.
- `net_wifi_set_required(NET_WIFI_REQ_MQTT, true)` while MQTT is on, so a lost network becomes
  the `network` trouble (docs/features/09).
- A client that won't start at all (esp-mqtt short of memory or a task slot) is tried again
  after 2 s, doubling to a minute with jitter, until it comes up - a single bad moment can't
  take remote reporting away for the rest of the boot. Only settings that cannot work (own
  certificate authority chosen with none loaded, a prefix and panel id too long for a topic)
  stop the attempts and wait for new settings, with the reason on the screen; changing the
  settings always gets one immediate attempt. While nothing connects, `comm_fail` follows after
  5 minutes as usual (MQTT-011).
- **Every accepted report leaves by one path or the other (MQTT-017).** Normally that is the
  event log's "reported" markers with replay. If the log is unusable, or it could not keep one
  particular marker (a full queue, a failed write), the report goes to a small RAM queue and is
  published live, once, without replay - `panel_log_add_lost_handler()` hands it over, so
  exactly one of the two paths owns a report and the broker never sees it twice (and when it
  does, `seq` is the same). A report that can't even go live is counted in the log's NVS loss
  counters and latches the `storage` trouble ([05](../05-Event-Log/README.md) LOG-010).

NVS namespace `mqtt`: `on`, `host`, `port`, `user`, `pass`, `tls`, `disc`, `cmds`, `panic`,
`prefix`, `id`, `ca` (PEM), `acked` (log index), `dmap` / `dpfx` / `did` (discovery), `zwdisc`
(the Z-Wave devices announced, MQTT-019).

### Setup on the touchscreen (Settings > Integrations > MQTT)
MQTT is one of the integrations in the list, beside Home Assistant and independent of it: the
list gives each a row with what it is doing right now, and MQTT's page is about the broker, not
about any one system that listens to it. It opens with the system master or the installer code
(docs/features/14): status (connected, waiting for Wi-Fi, can't reach the broker, sign-in refused,
certificate not trusted, and how many events were sent), "Connect to the broker", broker
address, port (empty = the usual one), user name, password (stored, never shown), security
(encrypted with a public certificate, encrypted with your own certificate authority, or not
encrypted with a warning), then "What goes over the broker": "Announce the panel" (discovery),
"Arm and disarm over MQTT" (off = anything listening can only watch), whether arming needs a
code (an installer option: Installer > System options > "Arm remotely without a code"), topic
prefix and panel id. Panics over MQTT are not on the touchscreen: the installer allows them on
the Panel Manager's MQTT page or the service console (BL-107).

While the local Home Assistant integration is paired, discovery is retracted so the panel isn't
added twice ([23](../23-Home-Assistant-Integration/README.md) HAI-010); the page says so in
plain words under the switch instead of leaving the owner to wonder.

### Service console
`mqtt` shows the state; `mqtt on|off`, `mqtt broker <host> [port]`, `mqtt user <name|-> [password]`,
`mqtt password [p]`, `mqtt tls off|public|ca`, `mqtt ca` (paste PEM, end with a line `.`),
`mqtt ca clear`, `mqtt discovery on|off`, `mqtt commands on|off`, `mqtt panic on|off`,
`mqtt prefix <p>`, `mqtt id <id|auto>`.

## Test and acceptance
- Host: `components/core/alarm_proto/test/test_proto.c` (JSON writer and reader including
  hostile input, golden payloads, command parsing, discovery, payload sizes) and
  `components/core/alarm_proto/test/test_report.c` (order, acknowledgement, replay after
  reconnect and restart, cleared log, comm_fail, and a marker the log never kept - which replay
  can never find, hence MQTT-017) on the real event log in RAM;
  `components/core/alarm_proto/test/test_zwdev.c` (the switched Z-Wave devices' keys, topics,
  discovery, state and commands, MQTT-018..020) and `components/core/alarm_proto/test/test_fw.c`
  (the update entity, OTA-010); `components/core/event_log/test/test_elog.c`
  (`test_failed_append_is_retried`, MQTT-017) and
  `components/core/zwave_zones/test/test_zwz_panel.c` (a stale Z-Wave command, MQTT-020).
- Integration (with the hardware, BL-026): Mosquitto with TLS on a PC + a Home Assistant test
  instance; scripted scenarios (arm from HA, entry alarm, abort, cancel, duress, broker down for
  6 minutes, replay) and expected messages. Still to add: make `esp_mqtt_client_init/start` fail
  once and check the panel connects by itself (MQTT-017 / R13), and burst events past the log
  queue with the broker down, then restore it, and check nothing accepted is missing (see Tasks).

### On a panel (BL-026 part 1, 2026-09-29)
A 7" panel (P4 v1.3, dev image 0.6.0) against Mosquitto 2 in Docker (TLS on 8883 with
a certificate authority of our own, plain 1883 for the test tools) and Home Assistant core
2026.9.3 in Docker with its MQTT integration. What passed:
- TLS (MQTT-001): a certificate from another authority is refused ("certificate not trusted";
  the broker logs "unknown ca"), the right one connects. Every handshake on P4 v1.x logged
  `psa_crypto_driver_esp_ecdsa: ECDSA peripheral not supported on this chip revision`: the
  software path took over and the connection was fine (since 2026-10-05 the v1.x image leaves
  the peripheral out: docs/features/06, Hardware findings 7).
- Topics (MQTT-004, MQTT-012): partition status and state, zones, troubles and firmware
  retained; heartbeat every 60 s, not retained.
- Discovery (MQTT-009): 12 documents (`p1`, `z1`..`z8`, trouble, event, firmware update), and
  Home Assistant made them 12 entities on one device with the right device classes (door,
  garage door, window, motion, smoke, carbon monoxide, moisture; problem and firmware in their
  categories) and the right states; the panel asks for a numeric code to arm and disarm.
- Pairing the native integration retracts all 12 (empty retained documents) and Home Assistant
  removes the entities and the device; unpairing brings them back with their states on the
  same connection (HAI-010, docs/features/23; this second half failed until B018).
- Live: a zone opened and closed, a trouble raised and restored, each in Home Assistant within
  a second, with `trouble` and `trouble_restore` on the event entity.
- Availability (MQTT-003): `restart` - back after 9 s; the broker saw the new connection take
  the old session over and published the will, so Home Assistant showed unavailable, then the
  states and a `system_start` event. `mqtt off` - `offline` retained at once; `mqtt on` - back
  in under a second. The update entity reads "up to date" (0.6.0) once B019 was fixed.
- Store-and-forward (MQTT-006, MQTT-011): broker down 6 minutes with four events -
  `comm_fail` at about 5 minutes, then the four and the trouble replayed in order, marked
  `replayed`, with their original times. The broker unreachable (a relay in between taken
  away) during a burst of 80 events: all 80 replayed in 0.85 s, sequence numbers without a gap
  or a duplicate and in order, and Home Assistant's event entity received all 80.
- Switching MQTT on sends nothing from before: 80 events while it was off, none sent (as
  designed, see the reporter above).
- Found: a broker restart. The panel is back 3 s after the broker and replays within a second;
  Home Assistant's MQTT client came back 2 s after the panel and missed all 80 replayed events
  (events aren't retained; the states were right): BL-112.
- Still to run: the code scenarios through MQTT (arm from Home Assistant, a wrong code, the
  entry alarm and its abort, duress; MQTT-007, MQTT-008, MQTT-010), MQTT-017's fault
  injections, a zone set Not used and back (B018's zone half), the Z-Wave devices.

## Open questions
- Trust-on-first-use for self-signed brokers (show the certificate's fingerprint on the screen,
  pin it on confirmation) instead of pasting a CA on the console or in the Panel Manager?
- Client certificates (mutual TLS): needed by anyone?
- MQTT 5 (reason codes, message expiry) once brokers commonly run it.
- Replay after a broker restart (BL-112): the panel replays before Home
  Assistant's MQTT client is back, so Home Assistant misses the replayed events. Proposed:
  listen for Home Assistant's birth message (`homeassistant/status` `online`) and, when it
  comes within a minute of the panel's own connect, send again what went out since that
  connect, marked `replayed` (consumers already drop duplicates by `seq`). The cost:
  duplicates for a consumer that stayed connected, and for Home Assistant if it restarts
  within that minute for some other reason.
- Decided (834e0ad): zone state only for enrolled zones; panic from HA off by default (console to
  allow). The Panel Manager's MQTT page offers the installer the same
  switch, since release builds have no console (BL-107, built: "Allow panic alarms
  over MQTT", shown while commands are on; a master sees it locked, and the panel refuses a
  change to it from anyone but the installer).

## Tasks
- [x] `alarm_proto` JSON codecs + tests (M5)
- [x] `net_mqtt` client with TLS + LWT + replay (M5)
- [x] HA discovery publisher (M5)
- [x] Settings > Integrations > MQTT screen (M5; its own entry in the Integrations list)
- [x] Firmware `update` entity + install command (M6, OTA-010)
- [x] The Z-Wave devices the panel switches: ids, topics, discovery, state and strict commands
  (MQTT-018..020, docs/features/36 ZW-036): `test_zwdev.c`, `net_mqtt.c` zw_sync /
  zw_command (53209bd)
- [ ] Integration test rig with the hardware: Mosquitto with TLS on a PC and a Home Assistant
  test instance, the scripted scenarios of "Test and acceptance" run against a panel and their
  messages checked (BL-026; part 1 passed 2026-09-29, "On a panel": the rig, TLS,
  discovery, live states, availability, outages and replay. Left: the code scenarios)
- [ ] Fault injection on a panel: `esp_mqtt_client_init/start` failing once, then the panel
  connecting by itself; a burst of events past the log queue with the broker down, then the
  broker back, and nothing accepted missing (MQTT-017) (BL-026; so far a burst of 80 events
  across an outage, all replayed, 2026-09-29 - the failing init/start and a full queue need a
  bench fault image)
- [ ] The switched Z-Wave devices' entities (MQTT-019, MQTT-020) on a real broker and Home
  Assistant, with a real device (BL-026; the devices themselves are BL-020)
- Duplicate of docs/features/05 ("Export over MQTT", LOG-009); its topic joins this doc's
  contract when it is built (BL-054)
- [ ] Configuration backup and restore over MQTT on request (CFG-005/006); the backup file
  itself is BL-002 (BL-054)
