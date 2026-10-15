# 36 · Z-Wave: devices as zones

Part of [docs/features/36](README.md). How a Z-Wave device's reports become zone events through
the zone hub (docs/features/22), what each device offers the zone editor, what the panel trusts,
and the table of models that need special handling. The source is `zsrc_zwave`, portable and
host-tested like `zsrc_zigbee`; it sees devices and their reports, never zones or the engine.

## 1. Addresses

A Z-Wave sensor is a `zsrc_address_t` (docs/features/22):

| Field | Value |
|---|---|
| `kind` | `ZSRC_ZWAVE` = **7** (appended to `zsrc_kind_t`; stored in bindings, never renumbered) |
| `id` | `home ID (32 bits) << 32 | generation (16) << 16 | node ID (16)` |
| `input` | `endpoint (bits 7..5, 0..7) | slot (bits 4..0, 1..31)` (below) |
| `ref` | `""` |

- **Home ID** ties a binding to one network: after "Reset the Z-Wave network" or a replaced
  module, old bindings match nothing and their zones show CHECK "source missing" until rebound
  (docs/features/22 ZSRC-010) - they can never be fed by whatever device gets the same node ID in
  the new network.
- **Generation** is the panel's inclusion counter at the time the device was added (node
  cache field `01`). Z-Wave reuses node IDs: the next device added after a removal may get the
  removed one's ID. Different generation, different address - the old zone keeps showing CHECK
  instead of silently following the new device. "Replace a failed device" keeps the
  generation on purpose (network.md §10). Fail closed (ZW-014): until the node cache without a
  removed or forgotten device is stored - or while its last write failed - Add and Replace are
  refused (BUSY while the write is only due, FULL after a failed one) and SmartStart waits, so a
  restart can never read the old cache and hand a reused node ID the removed device's
  generation. Removing still works while the write is only due; after a failed write Remove and
  Remove Failed are refused too (FULL) and a device reset locally stays as gone, not removed,
  until a write succeeds - the cache might not hold it yet, nor the counter its generation. The 16-bit counter never wraps: once it reaches 65535, new
  devices are refused (FULL), a node the cache lacks gets no record, and "Replace a failed
  device" still works.
- **Endpoint** 0 is the device itself; 1..7 are Multi Channel endpoints (a multi-input device
  such as a contact with an external terminal). Devices with more than 7 endpoints offer only
  the first 7.

`zsrc_zwave_address(home, gen, node, endpoint, slot)` builds it; `describe()` gives "Zooz ZSE41
open/close sensor · door contact" (model name from the quirks table, else manufacturer and
product IDs in hex, else "Z-Wave device 5").

**Decided while building it (Z6):** the node cache keeps 4 endpoints (network.md §2), so the
source offers endpoints 0-3 (`ZSRC_ZW_EP_MAX` 4); an endpoint beyond that offers nothing.

## 2. Slots: the inputs a device can offer

Slot numbers are stored in bindings: **append only**.

| Slot | Input | Suggested device class | Kind of state | Reported by |
|---|---|---|---|---|
| 1 | door/window contact | `DOOR` (or `WINDOW` from the quirk / Z-Wave Plus icon) | open/closed | Access Control `16`/`17` (open, including tilted - the tilt parameter never changes the result); Binary Sensor `0A` |
| 2 | intrusion | from the device (contact or motion), else `GENERIC` | active/idle | Home Security `01`/`02` |
| 3 | motion | `MOTION` | active/idle | Home Security `07`/`08`; Binary Sensor `0C` |
| 4 | glass break | `GLASS_BREAK` | momentary | Home Security `05`/`06`; Binary Sensor `0D` (state) |
| 5 | tilt | `GARAGE_DOOR` | active/idle | Binary Sensor `0B` |
| 6 | impact / moved | `VIBRATION` | momentary | Home Security `0A`, `09` (when the device has no cover tamper, else `09` is tamper) |
| 7 | water leak | `WATER` | active/idle | Water `01`/`02`; Binary Sensor `06`; Alarm Sensor type `05` |
| 8 | smoke | `SMOKE` | active/idle | Smoke `01`/`02`; Binary Sensor `02`; Alarm Sensor type `01` |
| 9 | carbon monoxide | `CO` | active/idle | CO `01`/`02`; Binary Sensor `03`; Alarm Sensor type `02` |
| 10 | heat | `HEAT` | active/idle | Heat `01`/`02`; Heat `03`/`04` (rapid temperature rise: an event, held like a momentary input); Binary Sensor `05`; Alarm Sensor type `04` |
| 11 | freeze | `TEMPERATURE` | active/idle | Weather `03`; Heat `05`/`06` (underheat); Binary Sensor `07` |
| 12 | gas | `GAS` | active/idle | Gas `01`..`04` |
| 13 | emergency button | `PANIC_BUTTON` | momentary | Emergency `01`..`04`; Central Scene (by quirk: the row's `panic_scenes`, Z10) |
| 14 | temperature | `TEMPERATURE` | value (hundredths of °C) | Multilevel Sensor type `01` |
| 15 | mains power | `GENERIC` | active = mains lost | Power Management `02`/`03` |
| 16 | general input | `GENERIC` | active/idle | Binary Sensor `01`, `09`; Alarm Sensor type `00`; Basic (§5) |

A device offers a slot on an endpoint when its interview shows it can report it: the event is
in its Event Supported Report, the type in its Binary Sensor Supported Report, air temperature
in its Multilevel Supported Sensor Report, or a quirk says so. **Tamper and battery are not
slots**: they belong to the whole device (§4). Pull-mode devices (network.md §5) and devices
without a lifeline offer nothing ("Does not report to the panel").

`candidates()` returns one `zsrc_candidate_t` per offered (endpoint, slot) of every ready
device: `name` = the device's name (its zone's name when bound, else the model), `detail` =
the input in words ("door contact", "motion", "input 2 · water"), `state` = its last report in
words ("Open", "Closed", "Motion", "Clear", "21.5 °C"), `device_class` from the table,
`changed` = it changed since the picker opened, `best` = the device was added in this picker
session (it is the one being set up).

## 3. Reports → zone events

The source reports through its sink ([docs/features/22](../22-Zone-Sources/README.md) "The
interface"):

- **Binary inputs** (slots 1-13, 15, 16) as `sink.input(addr, "open"/"closed"`,
  or `"on"/"off", value 100/0)` so the hub's mapping applies (normally-open inversion, check-ins
  once a minute, CHECK).
- **Momentary inputs** (glass break, impact, emergency): `"on"`, then `"off"` after the
  binding's `hold_s` (default **30 s**, like the 433 MHz source), restarted by a repeat. Heat's
  rapid temperature rise (Heat `03`/`04`, events in Z-Wave JS) holds the heat input on the same
  way; an overheat state reported meanwhile outlasts it, and when the hold ends the input is told
  `"off"` even if the device never reported an overheat state. A hold whose end the link's break
  hid (or cut short) ends at the device's next check-in, `"off"` unless an overheat state was
  reported since, as a momentary input's does.
- **Temperature** (slot 14): `sink.input(addr, NULL, value_x100)`; the zone's ABOVE/BELOW
  mapping decides (default "below 5 °C", `zsrc_mapping_defaults`).
- **Tamper** (device-level): `sink.event(addr, ALARM_ZEV_TAMPER / _TAMPER_RESTORE)` for
  **every watched input of the device**.
- **Battery** (device-level): `ALARM_ZEV_LOW_BATTERY / _LOW_BATTERY_RESTORE` for every watched
  input of the device.
- **Trouble** (device-level, life-safety devices): `ALARM_ZEV_TROUBLE / _TROUBLE_RESTORE` on
  the smoke, CO, heat and gas inputs.
- **Reachability:** `sink.reachable(addr, false/true)` per network.md §8, for every watched
  input of the device.
- **Check-in:** a wake-up notification, a heartbeat or any frame that changes nothing:
  `sink.event(addr, ALARM_ZEV_CHECKIN)` for every watched input (the hub limits check-ins to
  once a minute per input).

Mapping in detail:

| Report | Effect |
|---|---|
| Notification, a **state** event of a slot's variable | that slot active (door `16` = open, `17` = closed) |
| Notification event `00` (idle) with parameter `e` | the slot whose variable contains event `e` goes inactive; tamper if `e` is a cover event (`07 03`, `09 06`) - only while that variable, at that endpoint, holds `e`: an idle naming an event the variable no longer holds (Smoke `01` after `02` replaced it) is stale and dropped, as in Z-Wave JS (ZW22). Our one exception: a variable holding nothing takes the idle too - Z-Wave JS ignores it, but its value database keeps the event across a link break, where ours is wiped (ZSRC-015), so a sleeping device's real idle after one would otherwise keep its zone faulted until it wakes |
| Notification event `00` without parameter | every state slot of that notification type goes inactive; for Home Security also tamper restore |
| Notification, a momentary event | the slot's momentary pulse |
| Notification status `00` (reports switched off) or `FE` | nothing |
| Home Security `03`, System `06` (cover removed) | TAMPER; its idle: TAMPER_RESTORE |
| Home Security `09` (product moved) on a device with a cover tamper | TAMPER, restore after 30 s |
| Home Security `04` (invalid code) | nothing (logged on the device page) |
| Maintenance of a life-safety device and System failures, one condition per state variable (Z-Wave JS): Smoke `04`/`05` (maintenance), `07` (periodic inspection), `08` (dust); CO `04`/`05` (maintenance), `07` (periodic inspection); Heat `08` (maintenance), `0A` (dust), `0B` (periodic inspection); Gas `06`; System `01`/`03` (hardware failure), `02`/`04` (software failure) | TROUBLE while any is active; the idle of one variable ends only that one; TROUBLE_RESTORE when none is left |
| Smoke `03`, CO `03`, Heat `07`, Gas `05` (test) | CHECKIN and "tested" on the device page - **not** a fault (open question 3) |
| CO `03` with parameter `02` (test failed) | TROUBLE (and CHECKIN), not "tested"; CO `03` has no idle, so parameter `01` (test OK), or a CO `03` without a parameter, ends it - TROUBLE_RESTORE and "tested"; another parameter value is only a check-in |
| Smoke/CO `06`, Heat `09` (alarm silenced) | nothing |
| System `05` heartbeat | CHECKIN |
| Power Management `0A`, `0B`, `0E`, `0F`, `10` | LOW_BATTERY; idle of that variable: restore |
| Binary Sensor Report | the slot of its type: `FF` active, `00` inactive; type `08` (tamper): TAMPER / restore |
| Binary Sensor Report, type `FF` ("Any") | the endpoint's one supported type; several (or none of ours): dropped, counted (a quirk can name the type) |
| Battery Report `FF`, level below **20 %**, v2 "replace soon/now", or v2 "disconnected" | LOW_BATTERY |
| Battery Report at or above **30 %** without replace or disconnected flags | LOW_BATTERY_RESTORE |
| Multilevel Sensor Report, air temperature | slot 14 value |
| Alarm Sensor Report (legacy) | the slot of its type: state `00` inactive, else active |
| Basic Set / Report | §5 |
| Wake Up Notification | CHECKIN; then the queue (network.md §7) |
| anything else | ignored |

Only reports **at the device's highest security class** get here (security.md §8). A report
for an endpoint the device does not have, or a slot it did not offer, is ignored - even when a
*different* endpoint of the same device offers that slot, it is never credited there: a report
is always the endpoint it names, nothing else. A root (unencapsulated) report is endpoint 0's
alone; on a device whose real inputs live on its Multi Channel endpoints (endpoint 0 offering
nothing itself) that makes it simply a check-in - it is never moved to another endpoint (no
"identical endpoints" compatibility remapping, out of scope by design, README).

**Fresh state.** When the link comes up, and whenever a device is watched: listening devices
are asked at once (Notification Get per type, Binary Sensor Get, Alarm Sensor Get per type,
air temperature, battery); sleeping ones get the same
questions queued for their next wake-up. Until a device reports, its inputs have no state
(the hub's grace runs, ZSRC-015).

**Decided while building it (Z6, `zsrc_zwave`):**
- Check-ins are limited by the source to one a minute per device (the hub's event path does
  not limit them); a momentary input's check-in is its "off". A frame that ends a device's
  unreachability also counts as a check-in, and a quiet listening device's delivered pings
  and ACKs count as its check-ins (network.md §8) - otherwise it would show CHECK.
- An idle without a parameter also ends that type's trouble and battery conditions. System
  `01`..`04` are TROUBLE only on life-safety inputs.
- A Notification v2+ device without an event mask offers every slot of its type; Binary
  Sensor v1 is the general input (slot 16); Multilevel Sensor below v5 offers temperature.
- A device whose quirk has an alarm map (§8) also offers, at the device itself, the slots its
  rows' notification events feed, whatever its interview found: a Notification v1 device lists
  no types (ZW-043).
- Alarm Sensor slots come from the device's Supported Report: the interview asks it in step 12
  after Binary Sensor (network.md §5), the node cache keeps the types (field `12`), and the bridge
  passes them on as the endpoint's `alarm_types` (ZW-024, BL-085). A device that hears that
  question and never answers it, or answers it badly, finishes its interview with none offered
  (field `12` empty); a device interviewed before 2026-10-01, or out of reach when asked, is asked
  its types at the next start (its wake-up, for a sleeper) and offers them from then on
  (network.md §5).
- Fresh state is asked on every watch. Alarm Sensor's is a Get per type the Supported Report
  listed (types 0-15, those the source holds); an end point with no types known is asked none -
  every Alarm Sensor Get names a type (no untyped Get as Binary Sensor v1's), and it offers no
  Alarm Sensor input. A sleeper with check-in interval 0 is vouched
  reachable at link up (it can never check in, and is not supervised).
- A network change (a replaced module is a new home ID) takes the source link down and up:
  old zones show CHECK at once, and nothing is offered until devices are added again; a
  module from another system keeps the source down.

## 4. Tamper, battery and signal on the zone page

- Tamper and low battery arrive on the zone's own input, so a Z-Wave zone needs no separate
  tamper or battery binding (the "Sensor settings" page hides those roles for Z-Wave, as for
  Zigbee).
- Each endpoint owns its tamper, low-battery and trouble conditions, apart per report family
  (Binary Sensor `08`, each Notification variable); the device's condition is any endpoint's (or
  its own: "moved", the Battery Report), so one endpoint's restore or idle leaves another's
  standing, and the restore is told once, when the last one ends (ZW21).
- The zone page shows the device's facts in the existing rows (docs/features/22 ZSRC-009): battery
  (%, or "Low"), last heard, signal (the ACK RSSI from the last TX report, or the RSSI of its
  last frame, in words: "Good", "Fair", "Weak" at > -75, -75..-88, < -88 dBm - thresholds to
  settle on the bench), security ("Secure", "Not encrypted"...), check-in ("Every hour").

## 5. Basic

Many sensors also send Basic Set to the lifeline. To avoid double counting:
- A device that offers any slot from Notification, Binary Sensor or Alarm Sensor ignores Basic.
- A device with none of those, but that sends Basic Set/Report, offers **slot 16** (general
  input) fed by Basic: value `00` inactive, anything else active.
- A quirk can map Basic to another slot (for example a water sensor that only speaks Basic).

## 6. Trust

The source's capabilities (`zone_source_ops_t.caps`) are what every Z-Wave device can do:
`ZSRC_CAP_LEARN | ZSRC_CAP_SUPERVISED | ZSRC_CAP_TAMPER | ZSRC_CAP_BATTERY |
ZSRC_CAP_OFFLINE_OK`. Encryption differs **per device**, so the zone hub gains an optional
per-address query (an additive change to `zone_source_ops_t`, docs/features/22):

```c
/* What this particular sensor can promise; NULL = the source's caps. */
uint32_t (*caps_of)(void *ctx, const zsrc_address_t *a);
```

| Device | Adds | Removes |
|---|---|---|
| any S2 class, check-in interval set | `ZSRC_CAP_AUTH`, `ZSRC_CAP_LIFE_SAFETY` | |
| S0 | `ZSRC_CAP_AUTH` | |
| no security | | |
| interval 0 (never checks in) | | `ZSRC_CAP_SUPERVISED` (and so no life safety) |
| pull mode, no lifeline | offers no inputs at all | |

The zone editor turns them into the usual words (docs/features/22 "Trust"): "Works without Wi-Fi.",
"Encrypted: its signal can't be copied." / "Not encrypted: its signal can be copied.",
"Checks in every hour: if it stops, the zone shows CHECK." / "Never checks in: a dead sensor
goes unnoticed.". Fire, CO and 24-hour panic zones accept S2 devices that check in; others get
the amber warning and, when docs/features/22 has it, the installer override (ZSRC-008).

Row to add to docs/features/22's trust table:

| | Z-Wave S2 (native) | Z-Wave without security |
|---|---|---|
| Supervised | yes: wake-up check-ins, pings | yes |
| Authenticated, not spoofable | yes: S2 keys, PIN or SmartStart | **no** |
| Tamper | Notification cover events | same |
| Low battery | Battery CC, Power Management | same |
| Works without Wi-Fi / HA | yes | yes |
| Jamming detection | background RSSI, failed transmissions | same |

## 7. The source's timers and limits

| Constant | Value | Why |
|---|---|---|
| `ZSRC_ZW_DEV_MAX` | 64 devices | 64 x about 550 bytes of state in PSRAM; the module takes 232 classic + Long Range, the panel's zones are the limit |
| `ZSRC_ZW_WATCH_MAX` | 96 inputs | zones + linked inputs |
| `ZSRC_ZW_GRACE_S` | 4200 s | the source default: the default 1 h check-in + 10 min (network.md §8); bindings get their own device's value when bound |
| `ZSRC_ZW_LEARN_S` | 60 s | the add/remove window |
| `ZSRC_ZW_HOLD_S` | 30 s | momentary inputs |
| `ZSRC_ZW_PING_IDLE_S` | 900 s | ping a listening device heard from nothing for this long |
| `ZSRC_ZW_BATTERY_LOW_PCT` | 20 % (restore at 30 %) | like Zigbee |
| `ZSRC_ZW_JAM_HOLD_MS` / `_DBM` | 30000 / -60 | network.md §9 |

**Decided while building it (Z6):** the binding default grace comes from the source per
sensor (`grace_of`, docs/features/22): the device's check-in + 600 s, 1800 s for a listening
device, never for interval 0, 1 s for an address that is not in the network. Nothing is
written into the binding, so a changed check-in changes the grace too.

## 8. Quirks table

Data, not code (like `zb_quirks.c`): `components/core/zone_hub/src/zw_quirks.c`. A row matches
manufacturer ID, product type and product ID (`FFFF` = any), optionally a firmware range.

| Field | Meaning |
|---|---|
| `name` | model name for people: "Zooz ZSE41 open/close sensor" |
| `device_class` | override the suggested class |
| `slots` | slots to offer or hide, per endpoint |
| `basic_slot` | Basic feeds this slot |
| `idle_after_s` | the device never sends idle (Z-Wave JS's `forceNotificationIdleReset`): each notification variable that has an idle state - intrusion, cover tamper, smoke, CO, water, the maintenance and battery conditions: every §3 state, tamper, trouble and battery variable except a door's open/closed (06 16/17), mains (08 02/03) and the CO test result (02 03); momentary events and product moved keep their own holds - ends as if the device had sent its idle (for the event it holds) this long after it was last set, each variable on its own timer (a repeat starts it again, the device's own idle stops it); motion, Notification's or Binary Sensor's, ends this long after its last motion report. A device with more than `ZSRC_ZW_IDLE_RESET_MAX` (8) such variables set at once ends the one set longest ago early. A tamper, trouble or battery condition's timer outlives a link break, as the condition does; a state's is forgotten with the state (ZSRC-015). 0: none |
| `lifeline_group` | when it is not group 1 |
| `no_supervision` | never wrap commands to it in Supervision (firmware answers "fail") |
| `config[]` | parameters to set during the interview: `(number, size, value)` |
| `add_words`, `wake_words` | what to press, in the model's words |
| `note` | one line for the device page |
| `offer` / `hide`, `remap_from` / `remap_to`, `flags` | added while building (Z6): slots per endpoint, a slot moved to another, `MOVED_IS_IMPACT` (the ZSE43) |
| `panic_scenes` | added in Z10: bit n = Central Scene n (1..31) is its emergency button (slot 13, offered at the device itself) - pressed, held, or pressed 2-5 times, never its release; a repeated sequence number counts once |
| `alarm_map[]` | added for ZW-043 (Z-Wave JS's `compat.alarmMapping`): up to `ZSRC_ZW_ALARM_MAP_MAX` (10) rows `(V1 alarm type, alarm level or any) -> (notification type, event)`. A Notification Report that names its event only by its V1 alarm type and level (command-classes.md §4.5) - and that its Supported Reports do not already name - is read as the first matching row's event, and goes on through §3 as that event: an idle (`00`) ends the type's states, a test is a test. Alarm type `00` ends the list and is never mapped. The slots the rows feed are offered at the device itself |

Starter rows, **to verify on the bench** before relying on them (the IDs are the ones Z-Wave
JS's device files give for these models, 2026-09):

| Model | Manufacturer / type / ID | Class | Notes for the row |
|---|---|---|---|
| Zooz ZSE41 open/close XS | `027A` / `7000` / `E001` | door | add: "click the Z-Wave button 3 times quickly"; wake: "click it 4 times quickly" |
| Zooz ZSE42 water leak XS | `027A` / `7000` / `E002` | water | same presses as ZSE41 |
| Zooz ZSE43 tilt/shock XS | `027A` / `7000` / `E003` | garage door | tilt = slot 5, shock = slot 6; wake: "click 4 times" |
| Zooz ZSE44 temperature/humidity XS | `027A` / `7000` / `E004` | temperature | freeze and heat zones only |
| Zooz ZSE18 motion | `027A` / `0301` / `0012` | motion | wake: "press and hold the Z-Wave button 5 s" |
| Zooz ZSE11 Q sensor | `027A` / `0201` / `0006` | motion | firmware < 2.0: `no_supervision` for Wake Up Interval Set |
| Zooz ZSE29 outdoor motion | `027A` / `0001` / `0005` | motion | firmware < 2.2: `no_supervision` (supervised commands always "fail") |
| Zooz ZSE70 800LR outdoor motion | `027A` / `0004` / `0006` | motion | Long Range capable (phase 3) |
| Ecolink DWZWAVE25 door/window | `014A` / `0004` / `0002` | door | `idle_after_s` 300: never sends idle, so intrusion and cover tamper would latch (Z-Wave JS: `forceNotificationIdleReset`, 5 min); add: pull the plastic tab from the back; wake: open the top cover, which reports a cover tamper that ends 5 min later |
| First Alert ZCOMBO smoke/CO | `0138` / `0001` / `0002`, `0138` / `000C` / `0001` | - | `alarm_map` (Z-Wave JS zcombo.json): `01 FF` smoke detected (Smoke `02`), `01 00` idle, `02 FF` CO detected (CO `02`), `02 00` idle, `0C FF` smoke test (Smoke `03`), `0C 00` idle, `0D FF` idle; offers smoke and CO (BL-153: bench check); classic, no S2: the zone page warns it isn't trusted for fire/CO zones |
| First Alert ZSMOKE smoke | `0138` / `0001` / `0001` | - | `alarm_map` (Z-Wave JS zsmoke.json): the ZCOMBO's rows without CO's; offers smoke (BL-153); classic, no S2: the zone page warns it isn't trusted for fire zones |

Other makers' Z-Wave sensors get rows
as they are tried; a device without a row works when it follows the standard CCs above.
