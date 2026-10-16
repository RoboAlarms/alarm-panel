# 23 · Home Assistant integration (HACS): the panel in Settings > Devices & services

**Status:** In progress - M12 (pairing, entities, events, commands) is host- and Docker-tested,
released as integration v0.1.0 (testing) and paired with a panel (2026-09-20); M13's
entities as zones and device calls passed on a panel with a test Home Assistant
(2026-10-06, validation.md part 2), which found the link dropping every 95 s (B029, fixed in the
integration, not released yet); the panel's Z-Wave devices are in the integration, not yet on a
panel with a radio. The bench's findings were fixed and tried on the same panel (2026-10-06,
validation.md part 3), the integration's side committed (`ha-addon` 7b6671a); its 0.2.0 release
candidate is prepared and checked, its frames read correctly by the 0.6.0 panel's parser (Test and
acceptance), not committed or released (BL-173). Left:
acceptance on a home network (BL-027, parts 1 and 2 passed), BL-171's leftovers, PyPI (BL-040), the Devices screen (BL-045, docs/features/24)
· **Milestone:** M12 (integration: pairing, entities,
commands), M13 (two-way: HA entities as zones and devices) · **Depends on:** 09, 10, 11, 14, 15, 22, 24 ·
**Hardware:** P4; a CYD master could carry the entities and commands part
(`ALARM_FEATURE_HA`, which its Kconfig turns on by default only with PSRAM: 112 KB of link
buffers), but the two-way catalog needs the P4's PSRAM

**Protocol:** [API 1](protocol.md), additive changes only · **Integration:**
`RoboAlarms/ha-addon`, [v0.1.0 (testing)](https://github.com/RoboAlarms/ha-addon/releases/tag/v0.1.0),
0.2.0 prepared, not released · **Evidence:** [validation.md](validation.md)

## Goal
Home Assistant users add the panel the way they add ESPHome or Shelly hardware:
Home Assistant **discovers** it, they click **Add**, confirm a code **on the panel's screen**,
and the panel appears as a device with its partitions, zones, troubles and events. The same
connection carries the other direction: the panel can use HA entities as zones (docs/features/22)
and control HA lights, locks and climate from its Devices screen (docs/features/24). No MQTT broker,
no typed credentials, no HA password on the panel.

## What the best integrations do (research, September 2026)

| Project | How it gets into HA | Lessons for us |
|---|---|---|
| **ESPHome** | Found by mDNS and shown under **Discovered**; host and port pre-filled; a 32-byte encryption key protects the native API. Per device, HA offers "Allow the device to perform Home Assistant actions", off by default ([HA docs](https://www.home-assistant.io/integrations/esphome/)) | One click from discovery; **HA decides** what the device may do in HA |
| **Shelly** | Discovered by zeroconf (`_shelly._tcp`), local RPC over WebSocket, push updates ([HA docs](https://www.home-assistant.io/integrations/shelly/)) | Local push, no cloud, no broker |

What makes the difference for the end user: discovered automatically; nothing long to type;
pairing confirmed on the device; the connection tested before the entry exists; reconfigure,
re-authenticate and repair from HA's UI; local push; every sensor a device you can put in a room.

### Why a HACS integration, not the other options
- **MQTT discovery (docs/features/10)** works without installing anything, but needs a broker, the
  MQTT integration and broker credentials typed on the panel, and can't carry the two-way
  features safely. It stays for Node-RED, your own server, and HA users who skip HACS.
- **Speaking ESPHome's protocol** would need no install at all, but the panel would borrow
  ESPHome's identity and have to track a protocol that evolves with ESPHome itself.
- **An HA app (add-on)** runs only on HA OS and would be a middleman container (see the earlier
  comparison, [HA blog](https://www.home-assistant.io/blog/2025/05/22/deprecating-core-and-supervised-installation-methods-and-32-bit-systems/)).
- **The panel signing in to HA** (the previous plan) puts an unscoped HA session on the panel.
  With the integration, the panel holds no HA credential and HA decides what it may read and do.
- A custom integration from HACS is also the path to Home Assistant core later: core needs a
  config flow, tests, and the device protocol in a library on PyPI, which we build from the start.

## The end user's journey
1. **Install**: the README's "Open in HACS" button (a My Home Assistant link) adds the repository;
   Download, restart Home Assistant.
2. **Discovered**: Settings > Devices & services shows *RoboAlarms panel "Home"* under
   Discovered (or: Add integration / Add device > search "RoboAlarms", host pre-filled from
   mDNS, or typed).
3. **Pair**: the flow says "On the panel, open Settings > Integrations > Home Assistant and tap
   *Pair*". The panel asks for the master or installer code, then both screens show the same
   6-digit code; the user taps **Allow** on the panel. HA tests the connection, then creates the
   entry.
4. **Done**: a device named after the panel ("RoboAlarms" when it has no name of its own) with
   an alarm panel per partition, each zone as its
   own device (so it can go into a room), troubles, events, diagnostics. Nothing else to set up
   for arming and disarming from HA.
5. **Optional, two-way** (Configure on the entry): pick which HA entities the panel may use as
   zones and which devices it may control (areas, domains or single entities). The panel's zone
   editor and Devices screen then search exactly those.
6. **Later**: IP address changes are picked up from discovery; a factory-reset panel raises a
   repair "Pair the panel again"; Reconfigure changes host or port without losing entities.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| HAI-001 | The panel advertises `_roboalarms._tcp` by mDNS (TXT: panel id, model, firmware, API version, paired flag) while the integration is enabled on the panel; the HA manifest matches it, so the panel shows under Discovered | integration |
| HAI-002 | The panel's local API listens only when enabled on the panel (Settings > Integrations > Home Assistant), accepts one paired HA instance, and uses **mutual TLS with pinned certificates** after pairing (an exception to NET-007, which docs/features/09 records) | HIL, review |
| HAI-003 | **Pairing** opens for 2 minutes from the panel (master or installer code). Both sides exchange their certificate fingerprints and random nonces (one side commits first); both screens show a 6-digit code derived from them; the user allows it on the panel. A man in the middle can't make the codes match. Then each side pins the other's certificate (same idea as keypad pairing, docs/features/11). The panel only answers `paired` once the pairing is **on flash**; if it isn't, the client is told `rejected` and nothing is pinned | host tests (code derivation), HIL |
| HAI-014 | **No client can own the link.** Every read and every write on the connection has a deadline and is cancellable: a header that starts must finish within 10 s and its body within 10 s more, a frame being sent within 30 s and with a byte accepted at least every 10 s, and a stalled socket hands control back every 2 s (`SO_SNDTIMEO`). A partly-arrived frame header keeps its bytes across polls instead of being discarded or spun on. Switching the integration off and unpairing take effect during any of that | review, HIL (slow/stalled client) |
| HAI-015 | **Unpairing is durable or it says so.** The trust record (paired flag, fingerprint, client, generation) is one NVS blob written in one go; it decides at boot, whatever the older `peer`/`client` keys hold. A revocation takes effect in the session immediately, and `ha_link_unpair()` returns non-`ESP_OK` when it did not reach flash; the panel retries every 5 s and reports `unpair_unsaved` until it lands. No path may bring a revoked client back - not a power cut between writes, not a firmware rollback | review, HIL (NVS fault injection) |
| HAI-004 | The config flow tests the connection before creating the entry; reauthentication after a panel factory reset or unpairing; reconfigure for host and port; discovery updates the host of an existing entry | integration tests |
| HAI-005 | Entities (below) with unique ids from the panel id, `has_entity_name`, device classes and translations; zones become devices under the panel (`via_device`); zones added or removed on the panel add or remove devices without a restart | integration tests |
| HAI-006 | Local push: state changes reach HA within 1 s on a LAN; entities become unavailable when the link drops and recover by themselves (reconnect with backoff) | integration tests |
| HAI-007 | Arming, disarming, bypass and the other commands go to the panel with the code typed in HA; the engine checks it with the usual rules (lockout per link, no auto-stay, MQTT-008 rules) and HA shows the result as an error when refused | integration tests |
| HAI-008 | Duress and silent alarms never change the HA alarm panel state; they arrive as events marked silent (same rule as MQTT-010) | integration tests |
| HAI-009 | Two-way: the integration's options choose what the panel may **read** (for zones) and **control** (Devices screen); it sends the panel that catalog and the states of the entities in use, and executes the panel's action requests only for allowed entities and the per-domain action list (docs/features/24); the panel's own entities are never offered back | integration tests |
| HAI-010 | While the integration is paired, the panel turns MQTT discovery off and retracts it (no duplicate entities); MQTT publishing for other systems can stay on | integration |
| HAI-011 | Diagnostics download (panel info, link state, no codes or keys); repair issues for "pair again", "panel firmware too old for this integration version" | integration tests |
| HAI-012 | Distribution: its own GitHub repository (HACS allows one integration per repository), brand icon in `brand/` (HA 2026.3+), releases, "Open in HACS" and "Add integration" My Home Assistant buttons in the README; the protocol client is a separate Python package on PyPI so the integration can move to HA core later | review |
| HAI-013 | Message protocol versioned (`api` number); the integration refuses a panel with an unsupported major version with a clear repair message | integration tests |
| HAI-016 | The master is the only panel Home Assistant talks to. It relays its link keypads (they run no `ha_link` or its mDNS service, docs/features/11 LINK-020) through the API 1 additions `settings`, `keypads`, `settings_set` and `settings_result`. The integration makes these entities only once the messages arrive, so an older panel or integration is unaffected; `api` stays 1 (designed 2026-10-16, BL-216) | integration tests, `test_link.c` |
| HAI-017 | Each panel's display, sound and announcement settings (master and keypads) are config entities whose state comes only from the panel's report. A `settings_set` needs the pinned connection and no code (anyone at a panel may change them, `panel_prefs.h:2-4`). It is answered ok, or with each refused key and why. Language, region, a keypad's name, home partition and supervision are not settable (designed, BL-216; the master's own settings and no language or region are the owner's decisions 7 and 5 in docs/features/11 Decisions, decided 2026-10-16) | integration tests |
| HAI-018 | Each keypad is its own device under the panel, identified by its own panel id, with connectivity, signal and firmware. It is unavailable while not heard for 30 s or while the link is down. It is never removed by itself and can be deleted once unlisted. It has entities only for the keys it accepts (designed, BL-216) | integration tests |
| HAI-019 | At most 1 `settings_set` a second per target (burst 3), one in flight per target, a queue of 8. `not_found`, `offline` and `unsupported` are answered at once, `timeout` after 7 s. The panel enforces display safety (docs/features/07 UI-046) (designed, BL-216) | `test_link.c`, HIL |

## Design

### Pieces
- **Panel, `components/services/ha_link`** (ESP-IDF): mDNS (`espressif/mdns`), TLS server
  (mbedTLS directly over lwIP sockets; the client must present a certificate, and since nothing
  chains to an authority, `MBEDTLS_SSL_VERIFY_OPTIONAL` lets the pinned SHA-256 fingerprint
  decide), pairing, the message loop, state snapshots and deltas from the alarm service,
  command execution, and (M13) the two-way catalog, subscriptions and action requests for
  `zsrc_ha` (docs/features/22) and the Devices screen.
- **Panel, `alarm_proto` additions** (portable, host-tested): message codecs for this API,
  pairing code derivation, catalog model.
- **Python package `aioroboalarms`** (PyPI): asyncio client for the panel API (TLS, framing,
  pairing, typed messages), usable outside HA and tested with a fake panel.
- **HA custom integration** (repository `RoboAlarms/ha-addon`, domain `roboalarms`):
  config flow (zeroconf, manual, pairing, reauth, reconfigure), options flow (two-way
  exposure), push coordinator, platforms, diagnostics, repairs, translations, brand icon (the
  main website's teal R/house mark, e6cdcf8). Zones and Z-Wave devices name the panel's device by
  its registry id (`via_device_id`) on Home Assistant 2026.8 and later (the old `via_device`
  stops working in 2027.8), and by identifier on older versions, so the minimum stays 2026.3 (BL-111).
  `translations/es.json` (a draft for native review, BL-005) is checked against `strings.json`.
  Home Assistant lists the integration as **RoboAlarms**.

### Connection
TLS 1.2 or later over TCP (default port 6054), frames of `u32 length + JSON` (at most 64 KB; catalogs in
pages). Before pairing only the pairing messages are accepted; afterwards both certificates
must match the pinned ones. Keepalive every 30 s. (Today's panel builds negotiate TLS 1.2, not
1.3: see Open questions.)

**Bounds on the panel's side** (HAI-014). One task serves the one connection, so nothing the
client does may keep it. Each pass through the loop makes one read attempt of at most 500 ms,
then pushes whatever is owed and looks at the drop flag, so a frame header that arrives a byte
at a time keeps its bytes between passes rather than being discarded or spun on. A header that
has started must be complete within 10 s and its body within 10 s after that; a frame being
sent must finish within 30 s and must have a byte accepted at least every 10 s; the accepted
socket carries `SO_SNDTIMEO` (2 s), which
lwIP reports as `WANT_WRITE`, so a client that stops reading never parks the task inside
`send()`. Beyond those, the 90-second silence limit still ends a connection nobody is using.
Switching the integration off and unpairing set the same drop flag, which every one of those
waits checks, so both complete while a client is stalling.

### What is stored (NVS namespace `halink`)

| Key | What |
|---|---|
| `on` | the integration is switched on |
| `crt`, `key` | the panel's own certificate and private key (DER), made on the first enable |
| `trust` | the trust record: version, paired flag, generation, pinned fingerprint, client name. One blob written in one call, and **what the panel believes at boot** |
| `peer`, `client` | the pre-`trust` keys, still written while paired so a firmware rollback keeps the pairing, and erased on unpair so a rollback cannot restore a revoked client |

Stored contract (components/core/AGENTS.md): the record's fields are never renumbered or reused, only added in
its reserved bytes, and the build asserts its size and field offsets.

| Message | Direction | Content |
|---|---|---|
| `hello` | both | API version, panel id, model, firmware |
| `snapshot` / `delta` | panel -> HA | partitions (state, level, ready, faults, memory...), zones (open, bypassed, alarm, trouble, tamper, battery, supervision, signal), troubles, configuration (names, types, device classes) |
| `event` | panel -> HA | each reportable event with seq and `silent` (docs/features/10 event JSON) |
| `command` / `result` | HA -> panel | arm (level, flags), disarm, bypass, unbypass, chime, exit restart, panic (only if allowed), with the code |
| `catalog` | HA -> panel | entities the panel may read or control: id, name, area, domain, device class, unit, state (M13) |
| `watch` / `state` | panel -> HA / HA -> panel | entities in use (bound zones, device tiles) and their changes (M13) |
| `call` / `call_result` | panel -> HA / HA -> panel | device actions from the Devices screen, checked against the options (M13) |
| `zwave_devices` / `zwave_set` | panel -> HA / HA -> panel | the panel's Z-Wave switches, dimmers, relays and garage doors, and their commands (API 1 addition, additive) |
| `settings` / `keypads` / `settings_set` / `settings_result` | panel -> HA; `settings_set` HA -> panel | each panel's Screen and sound settings and the keypads, relayed by the master (API 1 addition; designed 2026-10-16, BL-216) |

### Entities in Home Assistant

| Platform | Entities |
|---|---|
| `alarm_control_panel` | one per partition (Home / Away / Night, code format number, arming without a code only if the panel allows it) |
| `binary_sensor` | per zone: open (device class from the zone), plus diagnostic tamper, low battery, supervision (CHECK); panel: trouble (problem), AC power, installer mode |
| `sensor` | per zone: battery %, signal (when the source reports them, e.g. Zigbee); panel: Wi-Fi signal, uptime (diagnostic) |
| `switch` | chime per partition |
| `button` | exit restart per partition |
| `event` | panel events (armed, disarmed, alarm, trouble, duress...) for automations |
| `update` | panel firmware (with OTA, M6): `in_progress` from the download to the restart (and while it goes back), `progress` the percent while it downloads and -1 otherwise (a developer file and the Wi-Fi module's update have no steps, so their percent shows while they are sent); the status goes out as it changes, at most once a second ([13](../13-OTA-Updates/README.md) OTA-026) |
| actions | `roboalarms.bypass_zone`, `roboalarms.unbypass_zone`, `roboalarms.arm` (with silent exit / instant), `roboalarms.panic` (only if allowed) |
| Z-Wave devices | the panel's Z-Wave switches, dimmers and close-only garage doors (`zwave_devices`): `switch` (on 100, off 0), `light` (brightness as the level, back to the last level), `cover`, each its own device under the panel, unavailable while offline or missing, never removed by themselves and deletable once unlisted; commands go as `zwave_set` |
| Panel settings (designed, BL-216) | On the panel's device and on each keypad's: `number` (Brightness, Screen saver brightness, Night brightness, Beeps, Chime and Voice volume), `select` (Screen saver after, Screen off after, Night screen off after, Appearance, Clock), `switch` (Night hours, High contrast, Key click, Announcements, Status announcements, Zone chime announcements), `time` (Night starts, Night ends). `EntityCategory.CONFIG`, named as the panel's own Settings rows. Enabled by default: Brightness, Night hours, Appearance, the 3 volumes and Announcements; the others disabled (the owner's decision 9 in docs/features/11 Decisions, decided 2026-10-16) |
| Keypads (designed, BL-216) | Each its own device under the panel (`parent_of`): `binary_sensor` connectivity, `sensor` signal (dBm), firmware as `sw_version`; never removed by itself, deletable once unlisted |

### Panel screens (Settings > Integrations > Home Assistant)
The page is the integration alone: state, "Pair", the paired HA version, "Unpair". **MQTT is a
separate entry in the Integrations list** (decided 2026-09-20: the two are separate things -
MQTT serves Node-RED, your own server or scripts as much as Home Assistant), so nothing here
links to it; see docs/features/10 and [07](../07-UI-LVGL-Pro/README.md) UI-019. The MQTT page says in
plain words when discovery is quiet because this integration is paired (HAI-010).

### Quality target
Built to the Integration Quality Scale from the start: Bronze and Silver rules (config flow with
full test coverage, unique ids, `has_entity_name`, test before configure, unavailable entities,
reauthentication, unloading, 95 % test coverage) and the Gold rules users notice (discovery and
discovery updates, devices, dynamic and stale devices, diagnostics, reconfigure, repair issues,
entity and icon translations, entity categories) ([rules](https://developers.home-assistant.io/docs/core/integration-quality-scale/rules)).

### Panel settings and keypads (designed 2026-10-16, BL-216)
The owner (2026-10-16): "Home Assistant should also be able to control the settings of the
slave panel as well." Home Assistant still talks only to the master, which relays its link keypads
(docs/features/11 "Smart Home and Home Assistant on keypads"; the messages are protocol.md "Panel
settings and keypads"). Nothing of it is built. The owner decided the questions it needs on
2026-10-16, each as recommended (docs/features/11 Decisions). The integration's 0.2.0 isn't
released (BL-173), so this goes into 0.3.0.

**On the panel:**
- `alarm_proto`:
  - `proto_prefs.h/.c`: the table, the TLV, the JSON values writer, the `settings_set` parser,
    the CRC;
  - `proto_link.h`: `PROTO_LINK_IN_SETTINGS_SET` appended to `proto_link_in_type_t` (`:201-210`);
  - the writers `proto_link_settings`, `proto_link_keypads` and `proto_link_settings_result`, and
    `proto_link_parse_settings_set`.
- `ha_link.c`:
  - `push_state` (`:830`) gains `push_settings` (on `panel_prefs_seq()`, at most once a second)
    and `push_keypads` (an FNV hash, at most once a second, as `push_zwave`, `:803-828`), both
    reset beside `S.zw_crc = 0u` (`:1259`);
  - `handle` (`:971`) gets the SETTINGS_SET case: the pinned peer, parse, target lookup, bucket,
    queue, the 7 s deadline;
  - results are drained like `send_calls`.
- `ha_link.h`:
  - `ha_link_settings_port_t { ctx; keypads(); set() }` and `ha_link_set_settings()`;
  - `ha_link_settings_done(ticket, why[], n)`, callable from any task, queued;
  - the master's own settings (target 0) go straight to `panel_prefs_remote_submit` (`ha_link`
    already requires `platform_esp`).
- `panel_app.c` wires the port beside `ha_link_set_zwave` (`:2062`) on a master with the link. It
  has to be a port: `plink_service` already depends on `ha_link`.

**In the integration** (`RoboAlarms/ha-addon` 0.3.0, after BL-173's 0.2.0):

| File | Change |
|---|---|
| `aiopanel.py` | `CLIENT_INFO` "aioroboalarms 0.3.0" (`:29`). `PanelSettings` and `KeypadInfo` dataclasses. `PanelState` (`:272`) gains `settings` and `keypads` (both None until their first message), with `apply_settings` and `apply_keypads` validated whole first (HA05, as `apply_zwave` `:284-298`); unknown keys are dropped. Mirror to `protocol/aioroboalarms.py` with `scripts/sync_protocol.py`. |
| `coordinator.py` | `_async_handle` (`:573-613`): `settings` and `keypads` return `state.ready`; `settings_result` goes through `_pending` beside `zwave_result`. `async_settings_set(target, values)` uses `_async_request` (`:1011`). `in_use`, `held`, `voice_not_ready` and `out_of_range` raise `ServiceValidationError` (translated); the rest raise `HomeAssistantError`. |
| `entity.py` | `keypad_device_identifier(panel_id, kp_id)` = `f"{panel_id}_kp_{kp_id}"`. Keypad `DeviceInfo` with `parent_of` (`:39-47`). `RoboAlarmsSettingEntity(target_id, key)`: available when connected, the target listed and online, its settings known, and the key in `can`. `async_add_keypad_entities`, modelled on the Z-Wave helpers (`:120-188`). Unique ids `<panel_id>_set_<key>` and `<panel_id>_kp_<kp_id>_<key>`. |
| New `number.py`, `select.py`, `time.py` | The settings entities. `PLATFORMS` (`__init__.py:21-31`) adds NUMBER, SELECT and TIME. |
| `switch.py`, `binary_sensor.py`, `sensor.py` | The 6 setting switches; keypad connectivity; keypad signal |
| `__init__.py` | `async_remove_config_entry_device` (`:74-94`) also allows deleting an unlisted `_kp_` device |
| `diagnostics.py` | Adds `keypads`: names, versions, online. No codes and no keys. |
| `strings.json`, `translations/en.json`, `es.json` | Entity names (the panel's own row titles), select options, errors |
| `tests/test_settings.py`, `tests/test_keypads.py` | Against the firmware's golden strings from `test_link.c`: entities from `can`; values only from reports; a set sends a frame; a refusal raises; unavailable while offline; an older panel gives no entities; an older keypad gives no device |
| `manifest.json`, README, CLAUDE.md status | 0.3.0. The release only on the maintainers' go. |

## Test and acceptance
- Host: `alarm_proto` message codecs and the pairing code derivation (golden vectors shared
  with the Python package): `components/core/alarm_proto/test/test_link.c` (framing, pairing
  code, messages, the two-way frames and the Z-Wave devices; HAI-002, -003, -009, -013) and
  `test_pair.c` (the pairing state machine, HAI-003); the entities as zones in
  `components/core/zone_hub/test/test_zone_hub.c` (HAI-009) and the device source in
  `components/core/home_control/test/test_home_control.c`. `ha_link` itself has no host test.
- UI preview: the panel page (shots `35`-`39`) and the picker's Home Assistant search (`74`-`76`).
- Python: `aioroboalarms` against a fake panel; the integration with
  `pytest-homeassistant-custom-component` (config, options, reauth, reconfigure flows, entities,
  commands, two-way allow-lists), in the `tests/` folder of `RoboAlarms/ha-addon`.
- Integration rig: a real panel and an HA test instance: discovery, pairing (including a wrong
  code and a man in the middle), arming from HA, entry alarm, duress (state unchanged, silent
  event), link loss, panel factory reset (repair), zone added and removed, two-way zone and
  device control.
- Bench, the bounds (HAI-014, none of it host-testable - the transport is mbedTLS and lwIP):
  a client that sends one byte of a header and stops (the panel must drop it in ~10 s); a
  client that pairs and then never reads while the panel pushes a full snapshot (dropped in
  ~10-30 s); `ha off` and `ha unpair` while either is happening (both must complete); a
  healthy client reconnecting straight afterwards; and thousands of interrupted connections
  watched with `status` for the task's stack high-water mark and free heap.
- Bench, durability (HAI-015): pair, then cut power between the unpair and the next boot;
  fill NVS so writes fail and check `ha unpair` says it wasn't stored, that the session is
  revoked anyway, and that the retry clears it once there is room; then reboot and confirm
  the client is still revoked.
- Older panels (BL-173, 2026-10-07): the integration's 0.2.0 candidate adds `level`,
  `setpoint`, `temp`, `modes`, `hvac_action`, `supports` and `since` to what it sends. A one-off
  harness (in the git-ignored `build/`, not committed) built the 0.6.0 panel's unchanged
  `json_read.c` and `link.c` (93f0c0e) and fed them the frames the candidate's own code sent the
  test suite's fake panel:
  its hello, a full 20-row catalog page and a second of 6 rows, 33 states (7 unavailable) and 2
  call results, each as sent (the new keys after the old ones), without the new keys, with them
  in front of the old ones, and in a stress variant with 16 modes, four supports and extreme
  numeric values (the largest catalog page tested: 9,077 bytes, against the 65,536-byte frame
  limit). Every frame was
  accepted with its type, every row walked without a reader error, and the old fields read
  exactly as without the new keys: a key too long for the 0.6.0 parser's buffer (`key[8]` in a
  state, `key[12]` in a catalog row) reads as empty and its value is skipped. A frame nested
  deeper than the reader allows was refused, so the harness can fail. No old panel was flashed.

## Open questions
- Name and domain: the integration shows the brand name everywhere in HA. Decided 2026-09-19
  in `RoboAlarms/ha-addon` (the rename `d6f04fa`, before v0.1.0; the display name `d96659f`,
  2026-09-25): the product is RoboAlarms, the domain `roboalarms`, and Home Assistant lists the
  integration as **RoboAlarms**.
- Keep the MQTT path for HA users who won't use HACS, or recommend the integration only?
  Decided: the MQTT path ("Panel screens" above; docs/features/10)
  stays, as its own entry in the Integrations list. Still open: calling the integration
  "recommended" (nothing says so today).
- Replay events missed while HA was down (like MQTT store-and-forward), marked replayed?
  Leaning: yes, the log already has them. Today they are not replayed: `ha_link.c` empties its
  event queue when the paired client connects, and the full snapshot that follows gives the
  state.
- Several HA instances pairing with one panel? Decided 2026-09-20 in HAI-002 and protocol.md
  "Transport" and "Pairing": one; a new pairing replaces the previous one, whose client gets a
  repair issue.
- TLS 1.3 or 1.2? The design and protocol.md say TLS 1.3, but no `sdkconfig.defaults*` turns
  on `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3`, so ESP-IDF leaves it off (the generated `sdkconfig.v1`
  and `sdkconfig.v3.dev` have TLS 1.2 only), and the integration sets no minimum version: the
  link runs TLS 1.2 today. Turn TLS 1.3 on (its flash and RAM measured first) or write
  "TLS 1.2 or later" in the protocol? Decided 2026-09-28: "TLS 1.2 or
  later" - the pinned certificate is what protects the link, and TLS 1.2 with ECDHE and
  AES-GCM is sound; TLS 1.3 isn't planned. protocol.md and the design say so.
- Home Assistant's chime switch on a default panel: the installer's default "chime needs a
  code" refuses it ("code required", found 2026-10-06), so the switch never works. Let the
  paired Home Assistant toggle the chime without a code, give the integration a chime service
  that takes one, or say in Home Assistant why it is refused? Decided 2026-10-06 (BL-172): the first - the link is authenticated and the chime
  protects nothing, as the Panel Manager's signed-in chime already works without a code
  (WEB-024). The engine lets any remote client (this link, MQTT) switch it without one
  (docs/features/01 CORE-CHIME-007); the keypads still need it.

## Tasks
- [x] Integration repository scaffolded (`RoboAlarms/ha-addon`): manifest (zeroconf
  `_roboalarms._tcp`, HA 2026.3+), config flow, translations, brand icon, `hacs.json`, CI
  (hassfest, HACS action, pytest, ruff), README with My Home Assistant buttons
- [x] Protocol spec ([protocol.md](protocol.md): framing, hello, pairing, versioning; golden
  vectors) and `alarm_proto` codecs + host tests (suites `link` and `pair`)
- [x] `ha_link` on the panel, the pairing half: mDNS `_roboalarms._tcp`, the panel's own P-256
  certificate in NVS, one TLS connection with fingerprint pinning, hello/ping, pairing over
  `proto_pair`, console `ha`; verified on development hardware
- [x] `ha_link` hardening after the external review (findings R09/R10/R11): bounded and
  cancellable transport (HAI-014), the trust record and honest unpairing (HAI-015), FreeRTOS
  queues and atomic flags between tasks; bench evidence in [validation.md](validation.md)
- [x] `ha_link`: snapshot/delta, events with seq and silent, commands with the code (M12)
- [x] Panel page (UI-027): the integration first (status, enable, Pair, the code with
  allow/reject, hold-to-Unpair), MQTT beneath as its own page; preview shots `35`-`39`
- [x] The protocol client `custom_components/roboalarms/aiopanel.py` (the future `aioroboalarms`
  package; no HA imports), tested against a fake asyncio panel including pairing
- [x] Integration config flow: zeroconf + manual, test-before-configure, the pairing steps, the
  entry pins the panel's fingerprint and keeps the client identity
- [x] Integration: options/reauth/reconfigure flows, platforms, push coordinator, diagnostics,
  repairs; released as v0.1.0 (testing) (M12, 5ce044e)
- [x] Two-way, the zones half (M13): `catalog`/`watch`/`state` on both sides - `zsrc_ha` in
  zone_hub, `ha_link` routing, the picker's search row (shots `74`-`76`), the integration's
  options flow; HAI-009 enforced both ways; **not yet run on hardware**
- [x] Two-way, the Devices foundation (M13): `call`/`call_result` on both sides -
  `components/core/home_control` (registry, HCTL-005, the HA device source), `device_service`,
  `ha_link` queueing calls, the integration executing them only against its options and
  docs/features/24's action table; **not on hardware yet**
- Duplicate of docs/features/24 (Tasks: "Devices view, detail sheets, code-checked actions,
  Settings > Devices", "Events for unlock / open" and "Built-in automations"; BL-045): the
  Devices screen itself - the panel UI (tiles, detail sheets, the code-checked unlock over the
  modal pad), Settings > Devices (hide, rename, favorites), unlock/open events, then built-in
  automations (docs/features/24 HCTL-009).
- [x] TLS pairing on a panel, Docker HA setup/reconfigure/unload, bounded transport,
  1,000 interrupted connections and simulated persistence failure/recovery; see [validation.md](validation.md).
- [ ] Acceptance with a real Home Assistant, from the "Integration rig" list in Test
  and acceptance: discovery across the home network (not exercised on 2026-09-20: the test
  computer was on another subnet, validation.md); arm and disarm with an authorized code, an
  entry alarm, duress (state unchanged, a silent event), a wrong pairing code and a man in the
  middle during pairing, link loss, a zone added and removed, and a panel factory reset raising
  the "pair again" repair. The rig's two-way zone is the bullet below; its device control waits
  for the Devices screen (BL-045) (BL-027). Part 1 passed on a panel with Home Assistant
  in Docker (2026-09-29, validation.md): pairing with a rejection and an allow, entities, zone
  events, link loss, a restart and the "pair again" reauth. Part 2 (2026-10-06): arming and
  disarming both ways with a code, a wrong code, an entry delay, Home Assistant sensors as
  zones, link loss both ways, every device kind. Left: duress, an entry alarm that sounds, a
  wrong pairing code and a man in the middle, a zone added and removed, the factory reset and
  discovery on a home network
- Duplicate of docs/features/22 ("Home Assistant entities as zones on a panel with a paired Home
  Assistant"; BL-027): an entity bound through the picker follows open and closed, and the zone
  goes to CHECK after its grace time when Home Assistant reports it unavailable (passed on a
  panel bound from the console, 2026-10-06, validation.md part 2; the picker not yet).
- [x] The integration's side of the Z-Wave devices (protocol.md, API 1 addition): `switch`,
  `light` (brightness) and `cover` (garage, close only) from `zwave_devices`, sending
  `zwave_set`, tested in `RoboAlarms/ha-addon` with the firmware's golden list (BL-043, 53209bd)
- [x] The update's percent while it downloads and its stages on the link's status (OTA-026, BL-188, 3a13059); the
  integration in `RoboAlarms/ha-addon` reads `progress` as -1 when there is none (not yet checked there)
- [x] Spanish translations (docs/features/37 G4): `translations/es.json`, a machine draft for
  native review, with a parity test against `strings.json` (BL-005)
- [ ] Publish the prepared standalone protocol package (`aioroboalarms`, the wheel attached to
  the v0.1.0 release) to PyPI once a publisher - a PyPI project or a trusted publisher - is set
  up (BL-040)
- [ ] The bench's findings in the integration (validation.md parts 2 to 5): B029's keepalive
  fix, the scene fix, `pong` to the panel's `ping`, `changed_by` from the panel's events, a
  wrong code as a `ServiceValidationError`, codes of 4-10 digits, the 25 missing event types,
  BL-171's values, `supports`, `since` and `hvac_action`, committed (`ha-addon` 7b6671a) and
  tried with a panel. The 0.2.0 candidate on top (a valve's `supports` always `[]`, the version,
  hacs.json's name, the README) passes 195 tests, Ruff, the protocol sync check and the package
  build, and the 0.6.0 parser reads its frames (Test and acceptance); left: commit it, CI's
  hassfest and HACS results, the maintainers' go, the GitHub release v0.2.0, then HACS offering it
  and an upgrade keeping pairing and entity ids (BL-173)
- [ ] Panel settings and keypads: `alarm_proto` (`proto_prefs`, the four messages, goldens),
  `ha_link` (pushes, the set queue, the settings port), `panel_app` wiring; the integration's side
  in `RoboAlarms/ha-addon` 0.3.0 (HAI-016..019) (BL-216)
