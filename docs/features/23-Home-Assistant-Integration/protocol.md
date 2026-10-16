# The RoboAlarms link protocol, version 1

The local API between a RoboAlarms Panel and its Home Assistant integration (docs/features/23).
Implemented twice - `components/core/alarm_proto` (`proto_link.h`, C11, host-tested) on the panel
and the `aioroboalarms` client in the integration repository (`RoboAlarms/ha-addon`) - with
the same golden vectors in both test suites, so the two cannot drift.

**Published with integration v0.1.0.** Message keys, framing and pairing derivation
are external contracts: never rename or reuse, only add. The `api`
number changes only when an old peer would misread a message (HAI-013).

## Transport

- TCP, default port **6054**, the panel is the server. TLS 1.2 or later (decided 2026-09-28; the link runs 1.2 today); both sides always present a
  certificate (self-signed is expected). Before pairing neither certificate is pinned - the
  pairing code is what defeats a man in the middle. After pairing each side accepts only the
  certificate whose SHA-256 fingerprint it pinned (HAI-002, mutual TLS).
- The panel accepts one paired Home Assistant instance and, while the pairing window is open,
  one pairing attempt at a time.
- Keepalive: the client sends `ping` after 30 s in which it has sent nothing, and after 30 s in
  which it has heard nothing; the panel answers `pong`. The panel may send `ping` too, and the
  client answers it the same way. Each side measures silence by what it
  receives, and either may drop a connection it has heard nothing on for 90 s. The panel's own
  updates come about every 10 s, so a client that pinged only when it heard nothing never
  pinged, and the panel closed a quiet house's link every 90 s (docs/bugs/B029).

## Framing

Every message is one frame: a 4-byte **big-endian** unsigned payload length, then exactly that
many bytes of UTF-8 JSON. The length is 1 to 65536; a peer that receives 0 or more than 65536
closes the connection. Anything larger than a frame (the two-way catalog, M13) is sent in
pages, each its own message.

## Optional diagnostic status (API 1 extension)

After the authenticated hello, the panel sends `status` before its first snapshot
and every ten seconds thereafter. Older clients may ignore it. It contains
`uptime_s` (nonnegative integer), `rssi` (negative dBm, or null when unavailable),
and `update`: `installed`, `latest` (empty before a successful release check),
`available`, `in_progress`, and `progress` (-1 unknown, otherwise 0..100).
This reports the panel's existing signed OTA subsystem; it does not grant remote
installation authority or expose release-server URLs. Missing readings are unknown,
never zero. Firmware installation remains authorized on the panel.

The P4 panel identity comes from its factory eFuse MAC, available before the hosted
C6 radio starts. Firmware must retain peer certificates after TLS negotiation to
perform the mandatory pinned-fingerprint check.

## Messages

One JSON object per frame. `"t"` names the type; unknown keys are ignored (forward
compatibility); a message missing a required key is invalid and the connection is closed.
Unknown `"t"` values are ignored (skipped, not an error), so either side can add messages.

On connect, the client sends its `hello` first, then the panel answers with its own. The panel
rejects (closes after `pair_result "unsupported"` / an empty reply) a client whose `api` major
it doesn't support.

| Message | Direction | Fields |
|---|---|---|
| `hello` | client -> panel | `api` (int, 1), `client` (string, e.g. `"aioroboalarms 0.1.0"`) |
| `hello` | panel -> client | `api` (int, 1), `id` (panel id, lowercase), `name` (the panel's friendly name), `model`, `fw`, `paired` (bool) |
| `ping` / `pong` | both | - |
| `pair_start` | client -> panel | `commit`: lowercase hex SHA-256 of the client's 32-byte nonce |
| `pair_nonce` | panel -> client | `nonce`: lowercase hex, the panel's 32-byte nonce |
| `pair_reveal` | client -> panel | `nonce`: lowercase hex, the client's 32-byte nonce |
| `pair_result` | panel -> client | `result`: `paired`, `rejected` (the user said no), `timeout` (window closed), `mismatch` (commit didn't verify), `not_pairing` (window not open), `busy` (another attempt is running) |

### State, events and commands (paired connections only)

The payload shapes are the panel's MQTT JSON (docs/features/10), which is already an external
contract with golden tests: the link wraps the same objects rather than inventing new ones.
The panel sends nothing but `hello`, `pong` and pairing messages until the peer is the
pinned one.

| Message | Direction | Fields |
|---|---|---|
| `snapshot` | panel -> client | `seq` (the engine's change counter), `partitions`: array of partition-status objects (each carries its `partition` number and name), `zones`: array of zone-state objects for the configured zones (each carries `zone`, `name`, `type`, `device_class`, `partition` and the live booleans), `troubles`: the troubles object. Sent right after the paired hello and again after a configuration change |
| `delta` | panel -> client | `seq`, plus only what changed since the last `snapshot`/`delta`: `partitions` and/or `zones` arrays in the same shapes, and `troubles` when they changed |
| `event` | panel -> client | the event object of docs/features/10 (its own `seq`, `silent`, `replayed`) with `"t":"event"` merged in |
| `command` | client -> panel | the MQTT command object (`id`, `action`, `level`, `partition`, `zone`, `code`, `flags`) with `"t":"command"` merged in; the panel feeds it to the same strict parser and the engine checks it with the same rules (HAI-007) |
| `result` | panel -> client | the MQTT command-result object with `"t":"result"` merged in (`id` echoes the command) |

A `snapshot` can be large (`PROTO_LINK_SNAPSHOT_MAX`, 48 KB, holds a full 128-zone panel;
host-tested); everything else stays small.

Duress and silent alarms never change the states above; they arrive only as events marked
`silent` (HAI-008, the MQTT rule).

### Two-way: Home Assistant entities as zones (M13, paired connections only)

Home Assistant decides what the panel may see (HAI-009): the integration's options pick the
entities, and only those ever appear in a `catalog` or `state`. A `watch` for anything else
is ignored.

| Message | Direction | Fields |
|---|---|---|
| `catalog` | client -> panel | the entities the panel may use as zones, in pages: `page` (0 first; a page 0 replaces the whole catalog), `more` (another page follows), then `entities`: array of `{ "id" (entity_id), "name", "area" ("" = none), "domain", "class" (the device class, "" = none), "state" (current, "" = unknown) }`. `page` and `more` come before `entities`. Sent after the paired hello and whenever the options change |
| `watch` | panel -> client | `ids`: the entity_ids the panel's zones are bound to - the full list every time (an empty list clears it). The client answers with a `state` for each and pushes changes from then on |
| `state` | client -> panel | one entity: `id`, `state` (its state text), `avail` (false: unknown or unavailable in Home Assistant - the zone shows CHECK through the grace time); for a `control` entity also its values, `level`, `setpoint`, `temp`, `modes`, `hvac_action`, `supports` and `since` (the Devices screen, below) |

### Two-way: the Devices screen (M13 second half)

The same catalog carries what the panel may **control** (docs/features/24). Each entry gains two
booleans (absent means its default): `zones` (default **true**: may feed a zone) and
`control` (default **false**: may be acted on). The integration sets them from its two
option lists; an entity can carry both. The panel never offers an entity back that carries
neither.

A `control` entity's catalog entry and its `state` messages also carry its values (BL-171,
2026-10-06). Each is optional: the client leaves out what it doesn't know, and the panel reads
absent, null, a fraction, another type or out of range as not known, so a client that sends
none of them works as before. Unknown keys stay ignored.
- `level`: integer 0..100. A light's brightness in percent (Home Assistant's 0..255, rounded;
  0 for a light that is off but can dim, so the panel keeps its Brightness slider; left out for
  one that can't dim), a fan's `percentage`, a cover's or a valve's `current_position`.
- `setpoint`: integer, hundredths of a degree, at most 100000 either way. A thermostat's single
  target (climate `temperature`) in Home Assistant's unit - the same numbers `set_temperature`
  sends back. Left out when there is none (a thermostat holding a range).
- `temp`: integer, hundredths of a degree, at most 100000 either way: what a thermostat
  measures now (climate `current_temperature`).
- `modes`: array of strings, a thermostat's `hvac_modes` as Home Assistant names them. The
  panel keeps `off`, `heat`, `cool`, `auto` and `heat_cool` (its Auto) and ignores the rest; more
  than 16 reads as not known. For its Auto key the panel sends `set_hvac_mode` with `auto`, or
  with `heat_cool` when the thermostat lists that and not `auto`.
- `hvac_action`: string, what a thermostat is doing now, Home Assistant's `hvac_action`:
  `off`, `heating`, `cooling`, `drying`, `idle`, `fan`, `preheating` or `defrosting`; another
  word or type reads as not known. The Smart Home screen shows it in a thermostat's words
  (docs/features/24 HCTL-004): Heating, Cooling and Preheating as sent; `off` and `idle` read
  Idle while the thermostat's state is heat, cool, auto, heat_cool, dry or fan_only, and Off while
  it is `off`; the rest, or none, read the mode's word. The panel keeps the word as sent.
- `supports`: array of strings, what the entity can do beyond the actions its domain always
  has: `brightness` (a light whose `supported_color_modes` include a brightness mode),
  `set_percentage` (a fan with `FanEntityFeature.SET_SPEED`), `stop` and `set_position` (a
  cover's `STOP` and `SET_POSITION`). A valve's is always `[]` from integration 0.2.0: the panel
  may only open and close one, so `stop` and `set_position` are refused for a valve whatever it
  can do. When it is there it is the whole list: what
  it leaves out the panel doesn't offer (`[]`: none of them; a fan without `set_percentage` is
  switched on and off, as a switch is; a cover without `stop` has no Stop; a light without
  `brightness` or a cover without `set_position` has no slider). Absent - an older client,
  another domain, or Home Assistant not saying - is not known: the panel offers what the kind
  has. Unknown words are ignored; more than 16 elements or another type read as not known.
  Sent while the entity is unavailable too, so an offline tile keeps its shape, its controls
  resting (docs/features/24 HCTL-006).
- `since`: integer, Unix time in seconds (UTC). On a `state` with `avail` false: since when the
  entity has been unavailable (Home Assistant's `last_changed`). For a scene that has run, on
  its catalog row and its states: when it last ran (the time its state word holds). Left out
  otherwise, and never on the catalog row of an unavailable entity (a catalog row can't say
  unavailable; its `state` follows at once). 0, a negative number, a fraction or more than
  4294967295 reads as not known.

The client sends a `state` for a `control` entity whenever its state word or any of these
values changes (a brightness-only change too), and none when nothing the panel receives has
changed; on every connection, `watch` and options change it sends each state again. A `state`
replaces the values of the one before: what it leaves out is not known now. A change of
`supports` or `hvac_action` alone sends a state too. Without `supports`, a light or cover that
has reported a level keeps its slider on the panel while it sends none, across a new catalog
too, until the panel restarts; with it, `supports` decides. `hvac_action`, `supports` and
`since` are API 1 additions (2026-10-06): a panel older than them skips them as unknown keys,
and a client older than them sends none. New value keys stay within 11 bytes, the longest key
this panel's catalog and state parsers read. A 0.6.0 panel reads keys of at most 7 bytes in a
`state` and 11 in a catalog row; a longer key reads as empty and its value is skipped, so
`setpoint`, `supports` and `hvac_action` pass it too (shown with its parser, docs/features/23
README, Test and acceptance).

| Message | Direction | Fields |
|---|---|---|
| `call` | panel -> client | `id` (echoed), `entity`, `action` (exactly one of the domain table of docs/features/24: `turn_on`, `turn_off`, `toggle`, `lock`, `unlock`, `open`, `close`, `stop`, `set_position`, `set_percentage`, `set_temperature`, `set_hvac_mode`, `set_preset_mode`), optional `value` (a number: brightness percent, position, percentage, or temperature in hundredths for `set_temperature`), optional `mode` (a string for `set_hvac_mode` / `set_preset_mode`) |
| `call_result` | client -> panel | `id`, `ok` (bool), `error` ("" when ok; `not_allowed` - the entity or action is outside the options, `unavailable`, `failed`, `timeout` - the service call was started and didn't finish within the integration's 25 s, so the panel says it couldn't confirm the outcome rather than that it wasn't done, HCTL-012) |

The integration executes a `call` only for an entity its control options allow and only with
an action from the table for that entity's domain (HAI-009 again); anything else is answered
`not_allowed`, never guessed. The panel asks for a user code before unlock, open-a-cover and
open-a-valve (HCTL-005) - the code never crosses this link for a call; the engine on the
panel checks it.

### The Z-Wave devices the panel switches (API 1 addition, 2026-09-27; docs/features/36 ZW-036)

Paired connections only. The panel's own Z-Wave outputs - switches, dimmers, relays and garage
doors (a relay and a tilt sensor) - with the same ids and rules as their MQTT entities
(docs/features/10 MQTT-018..020). Panel side implemented and host-tested
(`test_link.c: test_zwave_devices_golden`, `test_parse_zwave_set`); the integration's side is
written and tested against that golden list (`ha-addon` `tests/test_zwave.py`, 2026-09-29) - a
client that doesn't know these types ignores them. A device missing from a list is
unavailable there, not removed: the panel sends an empty list while its radio starts or is
away (`zwave_service_ha_devices`).

| Message | Direction | Fields |
|---|---|---|
| `zwave_devices` | panel -> client | `devices`: the whole list, every time (the client replaces what it holds): `{ "key" (stable id: network, device, generation, end point), "kind" ("switch", "light", "garage"), "name", "level" (0..100, null when not known), "online" (bool), "state" (a garage's, its sensor's word: "open", "closed", "unknown" - a press is never reported as "opening") }`. Sent after the paired hello and whenever anything in it changes, at most once a second |
| `zwave_set` | client -> panel | `id` (echoed), `key`, `level` (0..100; a switch 0 or 100; a garage 0 = close) |
| `zwave_result` | panel -> client | `id`, `ok` (bool), `error` ("" when ok; `not_allowed` - a garage's open, a switch's in-between level; `not_taken` - unknown device, busy, too old, the radio away) |

A garage door never opens from Home Assistant: opening takes a user's code at the panel
(docs/features/24 HCTL-005). A command is refused when it reaches the device more than 5 s after
it arrived. Suggested entities on the integration's side: `switch`, `light` (brightness),
`cover` (device class garage, close only), unavailable while `online` is false.

### Panel settings and keypads (API 1 addition, 2026-10-16; docs/features/11, BL-216)

Designed, not built on either side yet. Paired connections only. A client that doesn't know these
types ignores them. One that knows them makes entities only once they arrive, so an older panel
gets none and is never sent a `settings_set`. The panel's link keypads run no connection of their
own: the master relays them (docs/features/11 "Smart Home and Home Assistant on keypads").

| Message | Direction | Fields |
|---|---|---|
| `settings` | panel -> client | `values` (the master's own; keys below), `can` (keys it may set), `voice` (`"ready"`, `"loading"`, `"none"`). Whole every time; after the paired hello and on any change, at most once a second. |
| `keypads` | panel -> client | `keypads`: the whole list every time (the client replaces what it holds). Each entry: `device` (1..8), `id` (the keypad's own panel id, null until it reports one), `name`, `home` (its partition), `online` (a live session heard in the last 30 s), `fw`, `rssi` (dBm, null when not known), `settings` (`values`, null until reported), `can`, `voice`. After the paired hello and on any change, at most once a second. |
| `settings_set` | client -> panel | `id` (echoed), `target` (the panel id of the master or of a keypad), `values` (only the keys to change) |
| `settings_result` | panel -> client | `id`, `ok` (true only when nothing was refused), `error` (`""`, or for the whole request: `not_found`, `offline`, `unsupported`, `busy`, `timeout`, `bad`), `refused` (`{key: "not_allowed" \| "out_of_range" \| "in_use" \| "held" \| "voice_not_ready" \| "busy"}`) |

Keys and units are those in docs/features/11 "Settings fields". Delays are in seconds (0 = never;
the panel's 9 choices). Night times are minutes after midnight (0..1439). Levels and volumes are
percent. Every key is 11 bytes or less (the parser rule in "Two-way: the Devices screen"). An
unknown key in `values` is refused as `not_allowed`. The panel answers within 7 s, inside the
client's 15 s. A `settings_set` names its target by panel id, never by device number: a device
number is reused after a removal (docs/features/11 LINK-001), so it could reach another keypad.

Example (fictional ids): `{"t":"settings_set","id":"3f9a1c2e","target":"a1b2c3",
"values":{"brightness":40}}` gets `{"t":"settings_result","id":"3f9a1c2e","ok":false,"error":"",
"refused":{"brightness":"in_use"}}`.

## Pairing (HAI-003)

Numeric comparison with a commitment, like Bluetooth LE numeric comparison: the client commits
to its nonce before it sees the panel's, so neither side can steer the code, and the code binds
both TLS certificates, so the codes only match when both sides are talking to each other.

1. The user opens pairing on the panel (master or installer code; a 2-minute window).
2. The client connects, `hello`s, and sends `pair_start` with `commit = SHA-256(nonce_ha)`.
3. The panel answers `pair_nonce` with its fresh 32-byte `nonce_panel`.
4. The client reveals `nonce_ha`; the panel checks `SHA-256(nonce_ha) == commit`
   (`mismatch` otherwise).
5. Both compute the 6-digit code below and show it - the panel on its screen, the integration
   in the config flow. The user compares them and taps **Allow** on the panel.
6. The panel sends `pair_result`; on `paired`, both sides pin the other's certificate
   fingerprint. A new pairing replaces the previous one (the old client gets a repair issue).

### Code derivation

```
fp_ha    = SHA-256 of the client's certificate (DER), 32 bytes
fp_panel = SHA-256 of the panel's certificate (DER), 32 bytes
input    = "roboalarms-pair-v1"                       (18 bytes, ASCII, no NUL)
        || nonce_ha (32) || nonce_panel (32) || fp_ha (32) || fp_panel (32)
code     = (first 4 bytes of SHA-256(input), read big-endian) mod 1000000
```

Rendered as exactly six digits, zero-padded. One attempt per window; a failed comparison means
starting over with fresh nonces.

### Golden vectors

Shared verbatim by `components/core/alarm_proto/test/test_link.c` and the integration's `tests/test_aiopanel.py`.
Byte sequences written as hex; vector 1 uses the bytes 0x00-0x1f, 0x20-0x3f, 0x40-0x5f,
0x60-0x7f for `nonce_ha`, `nonce_panel`, `fp_ha`, `fp_panel` respectively.

| # | nonce_ha | nonce_panel | fp_ha | fp_panel | code |
|---|---|---|---|---|---|
| 1 | 00..1f | 20..3f | 40..5f | 60..7f | `588460` |
| 2 | all zero | all zero | all zero | all zero | `064511` |
| 3 | vector 1 with nonce_ha and nonce_panel swapped | | | | `195600` |

## Versioning

- `api` is a single integer, this document is 1. A panel refuses a client with a higher major
  with a clear result; the integration raises a repair issue for a panel that is too old
  (HAI-013).
- New optional keys and new message types don't bump `api`.
