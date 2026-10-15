# 20 · Zigbee: managing the devices

Part of [docs/features/20](README.md) (requirements ZB-010 to ZB-022 and ZB-025 to ZB-027; the
milestones are the README's "Milestones"). How the panel keeps track of its Zigbee devices as
devices - not only as zone inputs - and what the installer can do with them: the registry, each
device's setup, the operations the pages ask for, what is stored, logged and backed up. Sirens,
keypads and key fobs are [alarm-devices.md](alarm-devices.md); how all of it is proven is
[testing.md](testing.md). The contracts are headers in `components/core/zigbee_zones` and the
additions to `components/core/zone_hub/include/zone_hub/zsrc_zigbee.h` and
`components/core/ui_backend/include/ui_backend/ui_backend.h` (BL-143 A1).

## 1. The panel's role and the rules

- The panel is the Zigbee 3.0 **coordinator and trust center** of a network it formed itself
  (ZB-001). It joins no other network. Any Zigbee 3.0 device may join while the installer's
  window is open (3 minutes, ZB-002); an install code is optional (section 8), and each device
  records how it joined (ZB-021). It was decided on 2026-10-02 that a device that joined with
  the standard key may still feed fire, CO and panic zones: how it joined is shown, not used to
  refuse it.
- **Two halves, one lock.** `zsrc_zigbee` (zone_hub) keeps one slot per device endpoint for the
  zones; `zbz_panel` (zigbee_zones) keeps the devices: the registry, their setup, the operations,
  the pages' view. Both are fed by the `zigbee` task of `zigbee_service` with the zone service's
  lock held. The stack's callbacks (task `zb_stack`) only queue what they learn as `ev_t`
  entries (`radio_port.c`), as today; the lock order stays the zone lock, then the stack's lock,
  never the reverse.
- **Only the alarm task touches the engine.** Events, troubles, device check-ins and a keypad's
  arm or disarm are posted to it (`alarm_service_*`), never called from the Zigbee task or a
  stack callback.
- **One operation at a time** (section 6), as Z-Wave: add, add with a code, remove, forget,
  replace, ping, identify, set up again, channel, survey, reset, restart, a siren's test. A second
  is refused BUSY. Settings (rename, codes, a siren's or keypad's assignment) answer at once.
- **Who may.** `allowed` means what it means for `zwz_panel_request`: one flag, installer mode
  with every partition disarmed (docs/features/22 ZSRC-012). Every request needs it except
  "Check it now", Identify, the survey and Dismiss, which change nothing and don't read it - as
  Z-Wave's ping and identify don't: for them installer mode is the caller's alone, and every
  caller (the pages, `/api/v1/zigbee`, the console) is installer-only. Restarting the radio, a
  siren's test and every setting need `allowed` too. The network reset also needs the installer
  code typed again (section 6). Reading needs nothing. (Decided 2026-10-02 without waiting for review: the first contract let one flag stand for two levels.)
- **Nothing on the panel flashes.** Identify blinks the device's own light, never the panel's
  screen; countdowns and live words change at most once a second (UI-020, UI-023).

## 2. What the radio side reports (`zsrc_zb_report_t`)

`radio_port.c` turns what the stack tells it into reports, queued from the callback and handed to
`zbz_panel_on_report` by the zigbee task. Sensor traffic (IAS status, attribute reports, battery)
keeps going straight to the source's `zsrc_zb_*` calls as today.

| Report | From the stack (esp-zigbee-lib 2.0.x) | `zbz_panel` does |
|---|---|---|
| `JOINED` | the trust center's device-authorized / update-device signals, with whether an install code's key was used | a new registry record (or the existing one, rejoin), its setup starts; ADD's row appears |
| `ANNOUNCE` | `DEVICE_ANNCE` | the record's short address follows (a rejoin keeps everything else); an unknown address is looked up by its IEEE (`ezb_zdo_ieee_addr_req`) |
| `LEFT` | `LEAVE_INDICATION` | the device left by itself (logged) or answered the panel's leave (REMOVE ends) |
| `JOIN_REFUSED` | the trust center refused it (no install code matched while codes are required) | logged once an hour per address (ZBZ_EV_JOIN_REFUSED) |
| `NODE_DESC`, `POWER_DESC`, `ACTIVE_EP`, `SIMPLE_DESC` | ZDO answers | the setup's steps (section 5) |
| `BASIC` | a read answer or report of Basic | manufacturer, model, versions, power source |
| `LINK` | the last frame's LQI and RSSI (today always 0: `radio_port.c` must read them from the APS indication) | signal (section 7) |
| `CHECK_IN` | a Poll Control check-in | a sleepy device is awake: its waiting step goes now |
| `ZCL` | a cluster command the source doesn't decode: an IAS ACE command, a Default Response | to `zbz_ace` / `zbz_wd` (alarm-devices.md) |
| `DONE` | the end of any request the panel made (`tag`), with its status | the step or operation goes on, retries, or ends |
| `NETWORK`, `SURVEY` | formation, a channel change; energy scans of 11, 15, 20, 25 | the radio page; the channel page |

An endpoint with IAS Zone feeds a binary zone from IAS only (ZB32): its On/Off or Occupancy reports
are kept for the pages but never fault the zone, and still count as the device being heard. A
device that lists IAS Zone but reports its contact only through On/Off (an enrollment that failed,
say) would therefore stay supervised and never fault; no model in `zbz_models.h` does this, and
one that did would need a quirk. After the radio comes back, a router is asked first for the
reading its zone watches (ZB31).

Each report kind becomes a new `ev_t` kind in `radio_port.c` (ZB-M2); the queue stays 32 deep in
PSRAM with drops counted, and a `SIMPLE_DESC` or `BASIC` report is the largest entry (about 120
bytes).

## 3. What may be asked of the radio (`zsrc_zb_radio_t`)

The six hooks of today stay as they are; the ones appended carry a `tag` that comes back in the
`DONE` report. A hook left NULL means "this radio can't": the request needing it answers
UNSUPPORTED (the fake radio in the tests sets them all).

| Hook | Library call (to confirm in ZB-M2) | Used by |
|---|---|---|
| `node_desc_req`, `power_desc_req`, `active_ep_req`, `simple_desc_req` | `ezb_zdo_node_desc_req`, `ezb_zdo_power_desc_req`, `ezb_zdo_active_ep_req`, `ezb_zdo_simple_desc_req` | the setup |
| `read_attributes` | `ezb_zcl_read_attr_cmd_req` (up to 8 attributes) | setup (Basic, IAS Zone, Poll Control), ping |
| `bind` | `ezb_zdo_bind_req` (to the panel's endpoint) | setup |
| `configure_reporting` | `ezb_zcl_config_report_cmd_req` | setup |
| `leave` | `ezb_zdo_nwk_mgmt_leave_req` (rejoin off) | remove |
| `forget` | `ezb_secur_ic_remove` and the address map; no call found that evicts one device's link key (open question 1) | forget, replace |
| `identify` | Identify cluster's Identify command | identify |
| `ias_test_mode` | `ezb_zcl_ias_zone_initiate_test_mode_cmd_req` (in seconds: `ZSRC_ZB_TEST_MODE_LEN`, 60, asked again every 45 s), `..._normal_operation_mode_...` | walk test (ZB-022) |
| `zcl_command` | the cluster's command request (IAS WD client, IAS ACE server answers) | sirens, keypads |
| `install_code` | `ezb_secur_ic_add` / `ezb_secur_ic_remove` | codes |
| `codes_only` | `ezb_secur_set_ic_required` | add with a code |
| `set_channel` | `ezb_zdo_nwk_mgmt_nwk_update_req` (broadcast), then the panel's own channel | channel |
| `reset_network` | `ezb_bdb_reset_via_local_action` or the stack's factory reset - which may restart the whole panel (open question 2) | reset |
| `channel_survey` | `ezb_nwk_scan` (energy detect) over the four channels | channel page |

The panel's own endpoint gains, before `esp_zigbee_start`, the clusters these need: Identify
client, IAS WD client (ZB-M10) and IAS ACE server (ZB-M11); today it carries client roles only
(`radio_port.c` `make_endpoint`).

## 4. The registry (ZB-010, ZB-021)

One record per device (its IEEE address), with its endpoints grouped under it - not one per
endpoint as the source keeps them. The record is `zbz_device_t` (`zbz_registry.h`):

- **Identity:** a registry **id** (1..65535, never 0, never given to another device while the
  first one's record exists; the pages and the Panel Manager name a device by it - a page's
  argument is an `int32_t`, never the 64-bit address), the IEEE address, the last short address.
- **What it is:** manufacturer and model (Basic), software build and versions, manufacturer code,
  node type (router or end device), MAC capabilities, power source, its **role** (sensor, siren,
  keypad, key fob, repeater, other).
- **The installer's:** a name (24 bytes; "" shows its zone's name, else its model).
- **Trust record (ZB-021):** how it joined (install code, standard key, not known) and when
  (Unix seconds, 0 when the clock wasn't set).
- **Setup:** where it is (new, running, waiting for a wake-up, stopped, done, not usable) and the
  step it is at or stopped at; its Poll Control check-in interval.
- **Endpoints:** up to 4 per device - endpoint number, profile, device id, input and output
  clusters as `ZSRC_ZB_CL_*` bits, IAS zone type. Endpoints with clusters the panel uses come
  first; a Green Power proxy (242) or a vendor profile is kept only when there is room.

Live state (link quality, last heard, the setup's tries and deadline) is `zbz_live_t` in
`zbz_panel_t`, never stored.

**Capacity.** 64 devices, 4 endpoints each. The coordinator's own children stay at the stack's
`ZB_MAX_CHILDREN` (32); the rest join through routers. The source's endpoint slots
(`ZSRC_ZB_DEV_MAX`, one per endpoint with usable inputs) are 128 - two per registry device, in
PSRAM (each slot is about 150 bytes) - and its bound inputs (`ZSRC_ZB_WATCH_MAX`) 128, one per
zone; `test_zbz_contract.c` asserts both against `ZBZ_DEV_MAX` and `ALARM_MAX_ZONES` (decided in
the review of wave 6: they had stayed at 32). Should the endpoint slots still run short (many
multi-endpoint devices, or traffic from devices no zone uses), a sensor added then ends its add
row FULL rather than ADDED, and at link up the devices that feed a zone are described first, so
an unused device never takes the slot of one a zone needs. That FULL is accepted as it is (decided
in the review of wave 6's repair): the device stays in the registry and on the device list, where
it can be removed or forgotten, but no zone can use it until a slot frees. While the link is up
`zsrc_zb_device_seen` refuses an input only for room (its other refusals, IEEE 0 or an endpoint
outside 1-240, are records the registry never holds as inputs), so `live.src_full` means no room.

**Stored** in the panel's NVS partition (no partition change), namespace **`zbpanel`** (7
characters; not the stack's own namespace, whose name the bench still has to record), one key per
device so a change rewrites one record:

```
meta       compat 1 | next id u16 | devices u8
d00..d63   compat 1 | fields (id u8 | length u8 | value)...      one per registry slot
codes      install codes waiting (section 8)
cfg        compat 1 | flags (bit 0 a channel was chosen) | channel | 0
wd, ace    siren and keypad assignments (alarm-devices.md)
```

Little-endian. Field ids (`ZBZ_F_*`, never renumbered, never reused): 1 id u16, 2 IEEE u64,
3 short address u16, 4 name, 5 manufacturer, 6 model, 7 software build (text, no NUL), 8 app
version u8, 9 hardware version u8, 10 manufacturer code u16, 11 node type u8, 12 MAC capabilities
u8, 13 power source u8, 14 how it joined u8, 15 joined at u32, 16 role u8, 17 setup u8, 18 setup
step u8, 19 check-in u32, 20 endpoint (11 bytes, repeated: endpoint, profile u16, device u16, in
bits u16, out bits u16, zone type u16), 21 WD MaxDuration u16 (a siren's IAS WD MaxDuration in
seconds + 1, written after the endpoints; missing or 0 = not read, so a read 0 s is stored as 1; read
during the Poll Control setup step; added with compat still 1). A zero field is not written and reads as zero; an unknown
field is skipped (a newer firmware's); a record without id or IEEE, or with a length past its
end, is dropped as damaged and that device is found again by its traffic and offered "Set it up
again". `compat` rises only when an older reader would misread a record; a reader refuses a
higher one and keeps the registry read-only, so a downgrade never overwrites a newer panel's
records. The longest record is 230 bytes (`ZBZ_RECORD_MAX` allows 256).

`cfg` (ZB-M3) is read as far as known: compat 0 or above 1, fewer than 4 bytes, or a chosen
channel outside 11..26 is damaged and reads as the default (no channel chosen); bytes past the
fourth at compat 1 are a later firmware's and are skipped. Its frozen fixture is
`components/core/zigbee_zones/test/cfg_fixture_v1.h`. Each stored part loads alone: damaged codes
are empty, a damaged cfg is the default, and the registry gives its own verdict. A failed write
asked for again (`zbz_panel_redo` with the registry's bit) rewrites every record and meta, and
erases every unused slot's key again, since the owner doesn't say which key failed: the failed
key may be the erase of a removed, forgotten or replaced device, which must not come back after a
restart (decided in the review of wave 6; erasing a key that isn't there is harmless, the owner
takes `ESP_ERR_NVS_NOT_FOUND` as done).

**The NVS budget** (decided 2026-10-02 without waiting for review). NVS keeps 32-byte
entries, 126 to a 4 KiB page; the 128 KiB partition is 32 pages, one of which NVS keeps empty for
its own clean-up: 31 x 126 = 3,906 entries (`ZBZ_NVS_USABLE`). A blob costs an index entry, then
for each chunk of at most 4,000 bytes a header entry and its bytes in whole entries
(`ZBZ_NVS_ENTRIES`); a namespace costs one. `zbpanel` at its largest (`ZBZ_NVS_WORST`, pinned by
`test_zbz_contract.c`):

| Key | Bytes at most | Entries |
|---|---|---|
| the namespace | - | 1 |
| meta | 4 | 3 |
| d00..d63 | 64 x 256 | 64 x 10 = 640 |
| codes | 514 (`ZBZ_CODES_BLOB_MAX`) | 19 |
| cfg | 4 | 3 |
| wd | 74 (`ZBW_BLOB_MAX`) | 5 |
| ace | 395 (`ZBA_BLOB_MAX`) | 15 |
| **zbpanel** | | **686 (21.4 KiB)** |

A typical house of 10-20 devices is about 110-250 entries. The same partition holds, at their largest:

- the Z-Wave namespace at its largest (`ZWZ_NVS_WORST`, pinned by `test_zwz_panel.c`): 1,753
  entries, 1,647 of them the node cache (64 devices of at most 800 bytes and 1 KB more, 52,224
  bytes, rewritten whole), the rest keys, the provisioning list, the check-ins, the keypads'
  assignments and the settings;
- the stack's own namespace: not known until the bench names it (BL-036 step 5); estimated at
  16 KiB (about 520 entries) for 64 devices - for each its address, its link key and its
  neighbour entry, about 120 bytes, 8 KiB in all, twice over because a value is written anew
  before the old one is erased;
- the alarm configuration, up to `ALARM_CONFIG_BLOB_MAX` (32,595 bytes at the P4's capacities:
  1,029 entries), and as much again free whenever it is saved, for the same reason;
- the automations (`automations`, docs/features/41 DESIGN.md "Stored format"): about 80 entries
  for a typical set and 308 at most (16 records, pinned in its tests); an add is refused
  below the same kind of reserve;
- the rest of the panel (the zones' bindings, Wi-Fi, MQTT, the panel's own settings), measured
  rather than estimated.

So the worst case does not fit (686 + 1,753 + 520 + 2 x 1,029 = 5,017 > 3,906 before the rest), and
a full NVS would stop the alarm's own configuration from being saved. **The rule:** Zigbee never
takes the room the alarm needs. The service tells `zbz_panel` the free entries NVS reports
(`zbz_panel_nvs_free`, from `nvs_get_stats`: at start, before the radio is up, after each
write it makes, and again just before it passes on an Add, Add with a code, Replace or a code
added), and while they are fewer than `ZBZ_NVS_ADD_MIN` - the reserve (`ZBZ_NVS_RESERVE`:
the alarm configuration's rewrite at its largest, 1,029, and one more record, 10: 1,039 entries,
32.5 KiB) and the record a new device needs (10) - a new device and a new install code are
refused: Add, Add with a code, Replace and a code added answer FULL, a device that joins anyway is
asked to leave (RES_FULL), and the Zigbee page says why (`storage_full`: "The panel's storage is
full"). Changing what is there (a rename, a setup, an assignment) still writes: it replaces a
value of about its own size. The bench still measures the stack's namespace and the panel's real
free entries (BL-036 step 5); if the stack's share proves larger, the reserve stays and fewer
devices fit.

**Z-Wave keeps the same rule** (ZW-039, decided 2026-10-03 in the audit's ZD-3 (h)). The service
tells `zwz_panel` the free entries (`zwz_panel_nvs_free`, the same `nvs_get_stats` figure: at start,
before the radio is up, after each pass of its store - the network, the cache, the list - just
before it passes on an Add or a Replace, and every 10 seconds, which covers its other writes, since
a SmartStart join asks nobody first and other owners write NVS too).
Until it is told there is no limit (the host tests, the PC preview), as for Zigbee. While the free
entries are fewer than `zwz_panel_nvs_need()`, a new device is refused: Add and Replace answer FULL
(`UI_ZW_REQ_FULL`, appended to `ui_zw_req_result_t`; "full" in the Panel Manager's API), the radio
stops listening for SmartStart and a registered device's request is not answered (it asks again
later, and joins once there is room). The need is `ZWZ_NVS_ADD_MIN` - the reserve
(`ZWZ_NVS_RESERVE`: the alarm configuration's rewrite at its largest, 1,029, and the node cache's
growth by one more record, `ZWZ_NVS_NODE_GROWTH` = 27: 1,056 entries) and that growth again for the
new device: 1,083 entries at the P4's capacities - or, once the cache is large, more: the cache with
one more device at its largest (`ZWZ_NVS_CACHE_ENTRIES`) has to be writable whole beside its old
copy after the add, plus that growth (with 40 devices or more this passes `ZWZ_NVS_ADD_MIN`,
1,083; with 63 it is 1,674). The two rewrites never overlap: each is one NVS call. The pages say
why in Zigbee's words
("No room for another device: the panel's storage is full"), and while it lasts the status says
so (`storage_full`, appended to `ui_zw_status_t`; "storage_full" in the Panel Manager's radio
object, as Zigbee's): the touchscreen's radio row and the Panel Manager's radio card show one
line, so a SmartStart device that waits is explained. Changing what is there (a setup, a
check-in, a removal) still writes. The cache stays one blob (ZD-3 (h)); one NVS key per device
is a decision still open (BL-154).

The first frozen fixture, `components/core/zigbee_zones/test/registry_fixture_v1.h`, came with the
codec in ZB-M2: made by the encoder (`DUMP_FIXTURE=1`), checked against this table and never
edited after (testing.md §6). A record carries no checksum of its own: NVS checks each entry with
its CRC-32.

Writes follow zwz_panel's take pattern: under the zone lock the owner takes the next changed key
and its bytes (`zbz_panel_take_registry`, `_codes`, `_cfg`, `_wd`, `_ace`), then writes NVS after
unlocking, from a task whose stack is in internal RAM (docs/bugs/B001: a task with its stack in
PSRAM never writes flash).

## 5. A device's setup (ZB-012)

The setup (Zigbee2MQTT's "interview" and "configure" in one) runs once per device after it joins,
and again on "Set it up again". Its steps, in order (`zsrc_zb_step_t`):

| Step | Asks | Skipped when |
|---|---|---|
| NODE_DESC | Node_Desc_req | - (a failure is not fatal: some Xiaomi/Aqara devices never answer; MAC capabilities from its announce stand in) |
| ACTIVE_EP | Active_EP_req | - |
| SIMPLE_DESC | Simple_Desc_req per endpoint | an endpoint listed but without a descriptor is dropped (zigpy's Aqara H1 case) |
| BASIC | manufacturer, model, versions, power source, SW build - from the first endpoint that has Basic, not always endpoint 1 - in three reads: the manufacturer with the power source, the model alone, then the versions and the SW build (a device answers only the records that fit `ZSRC_ZB_ZCL_PAYLOAD_MAX`, 48 bytes; IKEA's PARASOLL needs 51 for all at once; decided 2026-10-03, BL-143) | answered by a model row when Basic fails (Control4-like devices) |
| POWER_DESC | Power_Desc_req | Basic said its power source |
| IAS_ZONE | zone type, zone state, the CIE address it holds | no IAS Zone |
| CIE_WRITE | the panel's address as its CIE address | never: always written, since no report carries the CIE address a device holds yet |
| ENROLL | an Enroll Response at once when its Enroll Request came; else one unasked every `ZSRC_ZB_ENROLL_GAP_MS` (5 s), at most `ZSRC_ZB_ENROLL_TRIES` (5), then the setup goes on and the source chases it patiently | the quirk says NO_ENROLL (a status heard is no proof of enrollment) |
| BIND | battery and Poll Control for every device; on/off, occupancy and temperature only on a sensor's input endpoints | the IAS Zone cluster is never bound (it sends to the CIE) |
| REPORTING | battery (0x0021, 1 h..12 h, 2 %), temperature (5 min..1 h, 0.2 C), on/off and occupancy (0..1 h) | it refuses (UNSUPPORTED is not a failure) |
| POLL_CTRL | its check-in interval | no Poll Control |
| DONE | - | - |

**Timeouts and retries.** A listening device (a router, or receiver on when idle) gets
`ZBZ_STEP_MS` (10 s) per step and `ZBZ_SETUP_TRIES` (3) tries, then the setup STOPS ("Set it up
again" goes on from that step). A sleepy device is sent the next step only right after a frame
from it (its wake window), as `zsrc_zigbee` already does for enrollment; while it is silent the
setup is WAKE ("Press its button to wake it", or the model's own words), and after
`ZBZ_SETUP_MAX_MS` (30 min) without finishing it STOPS. During a setup, a Poll Control check-in
should be answered with fast polling, so a sleepy device stays awake until its setup ends. No
radio hook does this yet (see below).
A non-success status other than UNSUPPORTED counts as a try.

**The walk test and the TEST bit (ZB-022, fail-safe).** A report with the IAS TEST bit set counts
as the sensor's own test (ALARM_ZEV_TEST, which the walk test marks) only
while a walk test runs, the sensor's zones are in its scope and the panel asked that device into
test mode. Any other time the TEST bit is ignored and the alarm bits are read as they are: a
sensor stuck in test mode, or one that sets the bit by itself, still raises its alarm rather than
hiding it.

*The scope is per input* (decided in the review of wave 6, which found one walk-test flag for the
whole panel: a partition-2 burglar's trip during partition 1's walk test was a "test", and so was
a real fire on a partition-1 smoke detector). The engine's walk test belongs to a partition and
covers only the zone types docs/features/40 WALK-007 names: fire and CO stay live, panic and
24-hour zones too unless "Include panic and 24-hour zones" is on. So `zbz_walk_covered` works out
the covered zones from the engine's configuration and snapshot (each zone in use, its partition
testing, `alarm_zone_type_walk_counts` with that test's flags), and `zbz_panel_walk_scope` puts
an input in the scope (`zsrc_zb_walk_test_input`) only when every zone bound to it, in any role,
is covered. `zsrc_zb_walk_test(on)` alone asks nothing; off clears every input's scope and asks
each device back to normal operation, and a zone told a test while its sensor is still open is
told the fault at once. An input out of the scope is never asked into test mode, and its trips
reach the engine as faults: a covered zone is still marked (the engine marks a fault in a
walk-tested partition), an armed partition alarms, a fire zone sounds. The owner calls
`zbz_panel_walk_scope` after `zbz_panel_walk_test(on)` and again when the covered zones or the
bindings change; until it does, no sensor is put into test mode and the walk test still works
from faults.

**Edge cases the tests replay** (testing.md §7): several endpoints with Basic on endpoint 2; a
node descriptor that never comes; Basic that fails with a model row to fill in; a second IAS
Zone read that shows it enrolled already; status reports without an Enroll Request; a duplicate
Enroll Request after success; a rejoin with a new short address (state kept, only the address
changes); an announce from an unknown address; the sequence number wrapping from 0xFF to 0x00.

**After a restart** a record left RUNNING or WAKE becomes STOPPED (herdsman resets interviews left
in progress the same way): nothing runs by itself, the device page offers "Set it up again". At
link up every registry device is described to the source again from its record, so its zones
work before it speaks.

**The join interview** (decided 2026-10-03, ZB-M6, without waiting for review):
`radio_port.c` no longer asks a joiner anything by itself (it used to send its own untagged
Active Endpoints, Simple Descriptor and Basic reads after each announce). zbz_panel's tagged
setup asks every question once, so a sleepy device's short awake window is spent once, not
twice; the source learns the endpoints from the setup's answers, and nothing else depended on
the untagged interview. The announce is still noted (short address, capability) for the
answers to the setup's questions.

**A new device's own key exchange first** (B021): a Zigbee 3.0 device at stack revision 21 or
later asks the trust center for its own link key after its announce (BDB v3.0.1 10.2.5: its
node descriptor request, three tries of 5 s, then Request Key and Verify Key) and leaves if that
fails. A join the radio side tells from the announce, before the stack saw the exchange end, is
`key_pending`; its setup sends nothing for `ZBZ_KEY_WAIT_MS` (16 s), or until the stack's
authorization (`ZSRC_ZB_REP_AUTHORIZED`), so the panel's questions don't take the device's polls
from the trust center's answers. A device that leaves in that time is JOIN_FAILED, as before.

**While a setup runs**, the source does not start its own enrollment for that device
(`zsrc_zb_device_setup`): the setup does the CIE write and enrollment in order. The source still
takes the device's reports.

**The result** decides the record's role and the add page's result: ADDED (inputs to offer: "Use
it for", a new zone with the suggested type or an existing zone), NOT_A_SENSOR (nothing usable;
a siren, keypad or fob goes to its own page instead), SETUP_NOT_FINISHED (stopped).

**Decided in building it (ZB-M3, `zbz_panel.c`):**

- NODE_DESC, BASIC, POWER_DESC, ENROLL and POLL_CTRL are optional: given up after their tries,
  the setup goes on. Every other step stops it at that step.
- A request the radio can't even queue counts as a try and goes again after `ZBZ_STEP_MS`. A
  request with no DONE by `ZBZ_STEP_MS` is a TIMEOUT. A DONE saying the radio went away is no
  try: the step goes again after `ZBZ_STEP_MS`, or when the radio is back if it is down by then.
- IAS_ZONE reads no zone status (0x0002), and ENROLL is skipped only for the NO_ENROLL quirk:
  the radio side hands any status word, a read one too, to the source as a status heard, and
  the source takes that as enrolled (ZB-007). A sensor that waits for its Enroll Response before
  it sends Zone Status Change Notifications must still get one, or its fire, CO or panic zone
  would never trip. Zone State (0x0000) has no report to the source yet, so it can't end the
  step early either.
- A sleepy device that times out waits for its next frame. With no frame in `ZBZ_WAKE_WAIT_MS`
  (10 min), the step is sent anyway.
- The panel asks for no fast polling: no radio hook does it. A check-in answer that keeps a
  sleepy device awake is the radio side's to give.
- Its role is decided once its descriptors and Basic are known (at IAS_ZONE), and again at the
  end. A sensor's inputs are then described to the source.
- The role (`zbz_role_of`), in order:
  1. the model row's role;
  2. an IAS ACE client is a keypad (a key fob by zone type or model flag);
  3. an IAS WD server that is a warning device (device 0x0403, zone type 0x0225, or no IAS Zone
     beside it) is a siren;
  4. an input endpoint (IAS Zone, Occupancy, Temperature, or On/Off on anything but a light,
     plug, lock or cover) is a sensor;
  5. a router with nothing else is a repeater;
  6. anything else is OTHER.
- The zone id in an Enroll Response is the source's for that endpoint. An endpoint the source
  keeps no slot for (a siren's, a keypad's) gets `ZBZ_ZONE_ID_OWN` (0x80) + its registry slot.
- At link up (`describe_all`), a device whose setup is DONE counts as enrolled for its IAS inputs
  (`zsrc_zb_ias_enrolled`), so the source doesn't enroll it again.
- The setup takes the zone type it read, and a Poll Control check-in interval, from the source's
  slot, where the radio side's reports put them. No report carries the IAS Zone state or the CIE
  address a device holds, so those two decide nothing yet.

## 6. Operations

One at a time; `zb_progress` reports it once a second (`ui_zb_progress_t`).

| Operation | Needs | What happens | Ends | Logged |
|---|---|---|---|---|
| Add | installer, all disarmed, radio working, storage above its reserve (section 4) | permit join 180 s (`ZSRC_ZB_LEARN_S`); Keep listening restarts it; every device that joins gets a row (`joined[]`, up to 4) and its setup | the window runs out (NOTHING_JOINED when none joined) or Stop; setups still running go on | ADDED per device (arg: how it joined) |
| Add with a code | same | the code is checked (section 8) and given to the trust center, `codes_only` on, the window opens for that address only | its device joined (row as Add) or the window ran out; `codes_only` off | ADDED |
| Remove | same | `leave`; "Asked it to leave" | REMOVED when it answered (its zones listed: they need a new sensor), NO_ANSWER after `ZBZ_LEAVE_MS` (15 s): "Forget it anyway?" | REMOVED |
| Forget | same | `forget`, the registry record dropped, `zsrc_zb_device_forget` | FORGOTTEN (its zones listed) | FORGOTTEN |
| Replace | same | listen as Add; the first device that joins and is set up with an input of the old one's class takes each of the old one's zones (`rebind`, the hub's: refused while armed), then the old one is forgotten | REPLACED (`id` the new one, `old_id`, `zones`), or the window ran out (nothing changed) | REPLACED |
| Check it now | installer (`allowed` not read) | a listening device: read Basic's ZCL version; a sleepy one: its next frame within `ZBZ_WAKE_WAIT_MS` | ANSWERED or NO_ANSWER | - |
| Identify | installer (`allowed` not read) | Identify for `ZBZ_IDENTIFY_S` (10 s) | DONE when sent and confirmed, FAILED | - |
| Set it up again | installer, all disarmed | the setup from where it stopped (from NODE_DESC when it had finished) | ADDED / NOT_A_SENSOR / SETUP_NOT_FINISHED | - |
| Survey | installer (`allowed` not read); the service refuses it while any partition is armed (its scans leave the coordinator deaf for about half a second) | energy of 11, 15, 20, 25 (not while devices join) | DONE; the status's `survey_dbm`, `quietest` | - |
| Channel | installer, all disarmed | `set_channel`: every device is told, then the panel moves; stored (`cfg`) | DONE / FAILED (`ZBZ_CHANNEL_MS`) | CHANNEL (arg: channel) |
| Reset the network | installer, all disarmed; the page asks twice and the installer code again (`installer_code`: the backend checks it with the engine first, on its own task - `alarm_service_authenticate_as`, the installer's code only, a wrong one counting toward that keypad's lockout - maps the answer with `zbz_reset_code_result` to WRONG_CODE or LOCKED_OUT, and wipes it; the code never reaches the zigbee task). The Panel Manager asks its password instead (section 14) | `reset_network`; registry, siren and keypad assignments emptied (waiting codes kept); zones go unreachable, each CHECK within seconds: the source remembers every device it forgot (`ZSRC_ZB_FORGOTTEN_MAX` = `ZBZ_DEV_MAX`, decided in the review of wave 6; it was 8, and the other zones kept the hour's grace) | DONE / FAILED (`ZBZ_RESET_MS`) | NETWORK_RESET |
| Restart the radio | installer, all disarmed (as Z-Wave's) | the service restarts the stack (zones keep their state) | DONE | RADIO_RESTARTED (arg 0) |
| Test a siren | installer, all disarmed | alarm-devices.md §2 | DONE / FAILED | - |

A device that joins while the registry is full, or the storage is at its reserve (section 4), is
asked to leave (RES_FULL); one that joins
outside any window (the stack refuses it) is JOIN_REFUSED. A channel change moves listening
devices at once; sleepy devices follow at their next check-in, and one that misses it must be
added again - the channel page says so before the installer confirms (ZB-016).

**Decided in building it (ZB-M3, `zbz_panel.c`):**

- Order of refusals:
  - Add, Add with a code and Replace check, in turn: BUSY, NO_RADIO, FULL (the storage's
    reserve, a read-only registry, 64 devices), UNSUPPORTED (no hook), and then the code.
  - The other device operations check NOT_FOUND (id 0 included), then BUSY, then NO_RADIO.
- **Forget** needs no radio: the record goes whatever the radio can still do. The radio is told
  to forget the device only while it works.
- **Ends:**
  - Stop ends Add, Add with a code and Replace as STOPPED. Setups still running go on.
  - Add and Add with a code end when the window does: NOTHING_JOINED when no device came, else
    once every row is done (ADDED if any was, else the newest row's result). A sensor whose
    inputs the zone source had no room for ends its row FULL, not ADDED (section 4).
  - Replace whose window ran out waits for a candidate still being set up, and ends
    NOTHING_JOINED if none takes the zones.
  - Replace moves every zone or none: when the hub refuses a zone (its partition armed), the
    zones already moved are bound to the old device again, and Replace ends FAILED with the old
    device kept.
  - A device that joins when there's no room ends the window as FULL.
- **Channel:**
  - Asking for the channel the network is already on is DONE at once, and kept as the
    installer's choice.
  - A channel is stored (`cfg`) only when the move succeeds.
- **Restart and Try again:** the service restarts the stack when the request answers OK. The
  operation ends DONE when the radio has gone down and works again (logged arg 0), or FAILED
  after `ZBZ_RESET_MS`. Try again is only for a paused radio (INVALID otherwise). A restart the
  radio made by itself is logged arg 1, and the guard pausing it arg 2.
- **Rename** on a registry kept read-only (a later firmware's records) is UNSUPPORTED.
- **Joins and rejoins:**
  - A rejoin keeps everything but the short address.
  - A fresh join of a known address is a device that was reset. Its add row says
    ALREADY_ADDED, its join is recorded again, and it is set up from the start.
  - A rejoin of a device the source forgot is ignored. Its key outlived it: open question 1.
- **JOIN_REFUSED** is logged once an hour per address. The last `ZBZ_REFUSED_MAX` (4) addresses
  are remembered; the oldest gives way.
- The log keeps `ZBZ_LOG_MAX` events waiting, oldest first. Events past that are counted
  (`log_lost`), never written over the waiting ones.
- **The siren, keypad and fob settings:**
  - They check NOT_FOUND, then INVALID (not that kind of device, or not set up).
  - A keypad with no free device number is FULL.
  - The settings themselves answer UNSUPPORTED until ZB-M10 and ZB-M11.

## 7. Signal, battery and diagnostics (ZB-011)

- **Signal** from the last frame's LQI: 150 and above good, 80 and above fair, below weak
  (`ZBZ_LQI_GOOD`, `ZBZ_LQI_FAIR`; the ESP32-H2 derives LQI from RSSI, so the thresholds are
  settled against RSSI on the bench and may change). RSSI in dBm on the device page and in the
  Panel Manager.
- **Last heard** is any frame (`zsrc_zb_frame_seen`); supervision is unchanged (ZB-005: 2 h for a
  sleepy device, 30 min for a router, then the hub's grace of 1 h).
- **Battery** from Power Configuration (0xFF unknown, low below 20 %); **power** from Basic, else
  the node and power descriptors; **router**: "Extends the network".
- **The radio page** (ZB-018): state and why it is paused, the radio's firmware version, channel,
  PAN id, extended PAN id, the panel's Zigbee address, background noise, the restart guard's
  count, Restart the radio, Try again when paused.
- The device page shows its address, short address, manufacturer and model as reported, software
  build, how and when it joined, its setup step - the words the touchscreen keeps for the
  installer; IEEE, LQI and cluster names appear only there, in the Panel Manager and on the
  console.

## 8. Install codes (ZB-015)

- A code is 6, 8, 12 or 16 bytes and its CRC-16/X.25 (8, 10, 14 or 18 bytes printed, in hex,
  often in groups of four), with the device's 16-hex-digit address. Typed on the touchscreen's
  hex keyboard or in the Panel Manager, or scanned there with the camera (the panel parses the QR
  text: "Z:<address>$I:<code>" and a bare "<address> <code>"; which forms real labels use is
  checked on the bench). A wrong length is BAD_CODE, a CRC mismatch CHECKSUM ("check it for a
  typo"), a bad address BAD_ADDRESS.
- A code is **kept until used**: listed on "Install codes waiting" (address, added when - the
  code itself is never shown again), given to the trust center at once and again at each start,
  forgotten when its device joins with it, or by the installer (Forget). At most 16 wait; none is
  added while the storage is at its reserve (section 4: FULL).
- "Add with an install code" gives the code and opens the window for that address only
  (`codes_only`): other devices are turned away for that window. "Add a device" lets any device
  join, a waiting code's device included (it then joins with its code).
- Codes are stored in `zbpanel/codes` and travel in the encrypted backup (section 13); the
  buffers that held a typed code are wiped after use.

## 9. The pages' contract (ZB-027; ui_backend's `zb_*`)

Optional as a set: a NULL `zb_status`, or one that returns false, hides every Zigbee row and page
(a firmware or a simulator without Zigbee). The touchscreen pages are drawn in
`docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js`, the Panel Manager's in its `panel-pages.js`
(`zg*` pages). Looking at any Zigbee page needs installer mode (decided in building it: the radio's
page shows the network's addresses): outside it the Troubles row asks for the installer code first,
then the page follows.

| Page | Reads | Asks |
|---|---|---|
| 1 Installer menu row | `zb_status`: radio, devices, jammed | - |
| 2 The Zigbee page | `zb_status`, `zb_devices` (one row per device, the UI sorts: unreachable first, then by name) | RETRY |
| 3 Add a device | `zb_progress` (LISTENING `seconds_left`, `joined[]`, `asleep`), `zb_device` for "Use it for" | ADD, KEEP_LISTENING, STOP, DISMISS |
| 4 Add with an install code | `zb_progress` | ADD_CODE (`ieee`, `code`) |
| 5 The device page | `zb_device` (`ui_zb_device_page_t`) | RENAME, IDENTIFY, PING, SETUP, REMOVE, FORGET, REPLACE |
| 6 Remove a device | `zb_progress` (REMOVED, NO_ANSWER) | REMOVE, then FORGET |
| 7 Radio details | `zb_status` | RESTART, RETRY |
| 8 Channel | `zb_status` (`survey_dbm`, `quietest`, `channel`) | SURVEY, CHANNEL |
| 9 Reset the network | - | RESET with `installer_code` (after two confirmations); WRONG_CODE and LOCKED_OUT have their own words |
| 10 Install codes waiting | `zb_codes` | CODE_ADD, CODE_FORGET |
| 11 Zone editor hand-off | the picker's "Add a new Zigbee device" opens page 3 and returns with the new device on top, "Just added" (the source's `new_join`) | ADD |
| 12 Zone page's "Zigbee device" row | `zb_device` via the zone's device id | - |
| 13 Trouble "Sensor radio problem" | opens page 2 in installer mode; otherwise the locked Installer page asks for the installer code first (toast "Enter the installer code to see the radio"), and page 2 follows | - |
| 14 Siren, 15 keypad, 16 key fob | `zb_device`'s `siren`, `keypad`, `fob` (a keypad's device number is shown, never asked for: alarm-devices.md §4) | SIREN, SIREN_TEST, KEYPAD, FOB |

Pages 14-16 read the assignments zbz_panel holds (alarm-devices.md §2 and §3, "Wired in
zbz_panel"): `live` is "assigned and still this address", a siren's `device` is always 0 and its
`last_test` is the result of its last "Test it" since start-up (`SIREN_TEST` runs as the operation
`UI_ZB_OP_SIREN_TEST`, followed on `zb_progress`). The device list's kind (page 2) is what the panel
uses the device as - its siren, keypad or fob assignment - before its role.

Words are the UI's (`TR()`), except a model's name, its pairing and wake words (the model table,
section 10) and the source's input words. A page held open on a removed device reads false from
`zb_device` and goes back.

## 10. The model table (`zbz_models.h`)

Per model: the name the pages use ("Door sensor"), how to put it into pairing mode, how to wake
it, a role when the clusters can't tell (a key fob that looks like a keypad), and the siren and
keypad encodings (alarm-devices.md). Matching is zb_quirks.c's: manufacturer then model, both
prefixes, first match wins. zb_quirks.c stays the sensor-behaviour table, unchanged; both tables
are checked whole by the tests (testing.md §5). Every row is provisional until a real unit is
recorded in docs/sensors/.

The first rows (ZB-M3, `zbz_models.c`) are seeded from testing.md §2's signatures and
alarm-devices.md's model notes. A more specific prefix comes before a shorter one ("3400-D"
before "3400"). The pairing and wake words are given only where the maker's sheet says them, and
a model with no row shows the model it reports. No row has the siren layout flag
(`ZBZ_MODEL_WD_REVERSED`): open implementations disagree for Frient's sirens, and one real siren
decides (testing.md §4).

## 11. Events and the log (ZB-019)

Built (ZB-M5): appended to `alarm_event_type_t` before `ALARM_EV_COUNT` (numbers 54-62, pinned
in `test_program.c`), names in `strings.c` (an external contract: never renamed), sentences in
`notify_words.c`, `program.c`'s platform-event check, golden tests in `test_program.c`,
`test_notify.c` and `test_proto.c`. Built in ZB-M6 (BL-143): `zbz_log_t.type` is a
`zbz_event_t` (1..9), so zigbee_service maps it (a table ZBZ_EV_* to ALARM_EV_ZIGBEE_*, the
device to `arg`, the record's arg to `arg2`; `_Static_assert`s that every `zbz_event_t` has its
type, that the two enums run in the same order, and that `alarm_zb_join_t`, `ui_zb_join_t`,
`alarm_zb_refused_t` and `alarm_zb_restart_t` carry the numbers the records do) and posts it
after letting go of the zone lock. The owner records each through
`alarm_service_platform_event()` (the alarm task's post path Z-Wave uses), as the touchscreen's
keypad when an installer asked, else as the panel:

| Event | Name | zone | arg | arg2 | Source |
|---|---|---|---|---|---|
| ALARM_EV_ZIGBEE_ADDED | `zigbee_added` | first zone fed | registry id | how it joined (`alarm_zb_join_t` = `zsrc_zb_join_t`) | the installer (user) |
| ALARM_EV_ZIGBEE_REMOVED | `zigbee_removed` | first zone fed | registry id | 0 | the installer |
| ALARM_EV_ZIGBEE_FORGOTTEN | `zigbee_forgotten` | first zone fed | registry id | 0 | the installer |
| ALARM_EV_ZIGBEE_LEFT | `zigbee_left` | first zone fed | registry id | 0 | the panel (user 0) |
| ALARM_EV_ZIGBEE_REPLACED | `zigbee_replaced` | the zone taken over | the new device's id | 0 | the installer |
| ALARM_EV_ZIGBEE_JOIN_REFUSED | `zigbee_join_refused` | 0 | 0 | why (`alarm_zb_refused_t` = `u.refused.why`) | the panel; once an hour per address |
| ALARM_EV_ZIGBEE_CHANNEL | `zigbee_channel` | 0 | 0 | the channel, 11..26 | the installer |
| ALARM_EV_ZIGBEE_NETWORK_RESET | `zigbee_network_reset` | 0 | 0 | 0 | the installer |
| ALARM_EV_ZIGBEE_RADIO_RESTARTED | `zigbee_radio_restarted` | 0 | 0 | `alarm_zb_restart_t`: 0 asked, 1 by itself, 2 paused by the guard | the panel or the installer |

Decided 2026-10-02 (ZB-M5, without waiting for review): the event keeps Z-Wave's shape -
`arg` the device, `arg2` the type's own value - so `zbz_log_t.device` goes in `arg` (0 when the
registry id is over 255, as a Long Range node ID is for Z-Wave: the words then say "A device") and
`zbz_log_t.arg` in `arg2`. The 64-byte log record is unchanged (no field added). A device's
events may name a zone, `zigbee_added` too (one found again by its traffic may already feed
some). The words: "Zigbee device added" with how it joined ("Joined with its install code",
"Joined with the standard key"), "... removed", "... forgotten · Reset it before adding it
again", "... left the network · It left by itself", "... replaced · A new device took its
place", "Zigbee device turned away" with why, "Zigbee channel changed · Channel 20 · By
...", "Zigbee network reset", "Zigbee radio restarted" or, paused by the guard, "Zigbee radio
paused"; all in the system bucket (NOTE-005: off for phones by default). The ha-addon
repository's event list follows (docs/features/23).

## 12. The console

`zigbee` grows (ZB-M6), changes gated like Z-Wave's console (every partition disarmed, a
`confirm` word for the destructive ones): `devices` (one line per device: id, address, model,
state, signal, battery, zones), `device <id> [signature]` (the signature as a fixture row,
testing.md §2), `trace on|off` (the exchanges for a replay, testing.md §7), `rename <id> <name>`,
`remove <id>`, `forget <id> confirm`, `replace <id>`, `identify <id>`, `ping <id>`, `setup <id>`,
`codes`, `code <ieee> <code>` (today's), `code forget <ieee>`, `survey`, `channel <n> confirm`,
`reset confirm <installer code>` (checked as the pages' is, as the console's keypad source),
`restart`. `status` and `retry` stay. The developer link gains `/dev/v1/zigbee`
(state and requests) and `panelctl zigbee` for bench and hardware-in-the-loop runs.

Built in ZB-M6 (`zigbee_console.c`): `status` (with the zigbee task's stack high-water mark, NVS's
free entries and the store's counts), `retry`, `restart`, `devices`, `device <id>`, `add`, `stop`,
`keep`, `remove`, `forget <id> confirm`, `replace`, `identify`, `ping`, `setup`, `rename`, `codes`,
`code <ieee> <code>`, `code forget <ieee>`, `survey`, `channel <n> confirm`, `reset confirm
<installer code>` (checked with the engine as the console's keypad source, never echoed),
`progress`, `dismiss`, and the bench's two (`zigbee_bench.c`): `device <id> signature` prints
the device as a fixture row of testing.md §2 (no `expect` block: the reviewer writes it) from the
raw descriptors the radio reported - every cluster id, vendor ones too - captured as they pass to
zbz_panel for the last four devices set up since the start (about 600 bytes each, PSRAM); without
them it prints what the record kept (only the clusters the panel knows) and says to run `zigbee
setup <id>`, which reads the descriptors again. `trace on|off` prints one `zbtrace <ms> <dir>
<ieee> ...` line per exchange (`>` what the panel asked of the radio, `<` each report and each
event the pump hands the source), RAM only, off at each start; a keypad's Arm and Bypass show
their length, never their code. Both print real addresses: a row or a trace is scrubbed to
fictional ones before it is committed (testing.md §7).
The developer link (`zigbee_dev.c`): `GET /dev/v1/zigbee` writes `{"api":1,"radio":{...},
"devices":[...],"progress":{...},"codes":[...]}` (the web API's keys of section 14, plus the
bench's `stack_free`, `stack_size`, `nvs_free`; install codes never, only the addresses waiting),
`POST /dev/v1/zigbee` takes the web API's ops but `reset` and the siren and keypad settings, with
`"confirm": true` for forget and channel, as the console allows them (every partition disarmed).
Decided (ZB-M6): dev_remote serves these two routes straight from zigbee_service - they answer
404 on a panel without Zigbee - rather than through the board's adapter as Z-Wave's are, so no
board file changes; `"zigbee"` joins `info`'s capabilities when the panel has it. The network
reset is never offered on the link (its installer code never travels the network): `panelctl
zigbee reset-network` refuses locally and says where to do it.

## 13. Backup and restore (ZB-020)

- Built (ZB-M5): the panel's namespace **`zbpanel`** is in the backup policy
  (`components/core/backup/src/backup_policy.c`) whole: registry (`meta`, `d00`..`d63`), install
  codes (`codes`), channel (`cfg`), siren (`wd`) and keypad (`ace`) assignments; `test_backup.c`
  proves it, and that a file from before Zigbee is still wholly kept. Backups are encrypted
  (CFG-005). As with every namespace added before it, a file that holds `zbpanel` is refused
  whole by firmware older than this policy (its check refuses entries it doesn't keep), and a
  restore of an older file empties `zbpanel` on the board (a restore replaces each policy
  namespace): the stack's devices are then found again by their traffic (`ALARM_ZB_JOIN_UNKNOWN`).
- The **stack's own namespace** (network key, PAN, the stack's device table, frame counter) joins
  once its name is recorded on the bench (BL-036 step 5; a TODO in `backup_policy.c` names it).
  Until then a restored `zbpanel` lists devices the replacement's new network doesn't hold: they
  must be added again. A restore is for a replacement board, and once the stack's namespace is
  restored too (planned with BL-036 step 5, not built - decided in the review of wave 6, which
  found this written as if it were):
  - the replacement's H2 or panel may have another address, which every sensor holds as its CIE
    address: after such a restore each IAS device's setup is to be set STOPPED at CIE_WRITE, so it
    is re-enrolled at its next frame;
  - the network frame counter must not go back: the restore raises it well past the backup's
    (the stack's frame-counter call), or devices drop the panel's frames;
  - restoring a network onto a panel while the original still runs makes two coordinators of one
    PAN: the restore page says to switch the old panel off first.
- A backup blob is at most 64 KiB (BACKUP_BLOB_MAX); a namespace and its keys at most 15
  characters.

## 14. The web API (`/api/v1/zigbee`, ZB-025)

Installer-only, like `/api/v1/zwave`; the capability `caps.zigbee` in `/status` shows the page.
The JSON keys are a contract once shipped (only added, never renamed); B017: a new URI handler
raises `WEB_URI_HANDLERS`.

- `GET /api/v1/zigbee` -> `{"radio":{"state","paused","jammed","channel","pan","ext_pan",
  "ieee","version","noise","survey":[...],"quietest","restarts","learn_s","storage_full"},
  "devices":[{"id",
  "name","model","state","kind","power","router","battery","low","heard_s","signal","lqi",
  "rssi","unreachable","join","zones":[...]}],"progress":{"op","step","result","seconds",
  "id","joined":[...],"zones":[...]},"codes":[{"ieee","added"}]}`; addresses as 16 hex digits.
- `GET /api/v1/zigbee?device=<id>` -> the device page (`ui_zb_device_page_t`'s fields).
- `POST /api/v1/zigbee` `{"op":"add"|"add_code"|"keep"|"stop"|"remove"|"forget"|"replace"|
  "ping"|"identify"|"setup"|"rename"|"code_add"|"code_forget"|"survey"|"channel"|"reset"|
  "restart"|"retry"|"dismiss"|"siren"|"siren_test"|"keypad"|"fob", "id", "ieee", "code",
  "name", "channel", "me_password", ...}` -> `{"result":"ok"|"not_allowed"|"busy"|"no_radio"|
  "invalid"|"unsupported"|"bad_code"|"checksum"|"bad_address"|"full"|"not_found"|"in_use"}`.
  The QR text goes in `code` as scanned; the panel parses it.
- `reset` is re-authenticated the Panel Manager's way, not with the installer code, which the
  browser never takes (BL-145's proposal, docs/features/31 "Recent password"): it carries
  `me_password` and follows the recent-password rule as `factory-reset` does (missing: `403
  password_required`, wrong: `403 wrong_password`, counted by the sign-in gate), checked by the
  handler before the request goes on; the touchscreen's and the console's `installer_code` is
  never sent over the network. Decided 2026-10-02 without waiting for review; whether
  a network reset asks the password every time is BL-145's open question.

**Decided in building it (ZB-M8, `panel_web` `web_zigbee.c`):**

- The page and its API follow devices.md and `ui_backend.h` where BL-145's drawing differs: the
  body's verb is `op`, a device is named by its registry `id` (the page's address is
  `#/zigbee-device/<id>`), the operation's countdown is `progress.seconds`, and a siren, keypad or
  fob is changed with `op` `siren`, `keypad` or `fob` and its settings as one object of
  `ui_zb_siren_t`'s, `ui_zb_keypad_t`'s or `ui_zb_fob_t`'s names (`supervision_min`, `squawk`,
  `panic_mask`...). The drawing's `use`, `keep` (a non-sensor), `armed` and `wake_it` aren't built:
  a zone is made from the zone editor, a refused change while armed is the panel's `409 locked`
  (the GET's `may_change` false, `why` "armed").
- Added beside the keys above (additive): the radio's `devices`, `not_heard`, `codes_waiting`; a
  device row's `device_class`, `setup_done`, `setup_total`; `progress.old_id` and `model`; the
  GET's `available`, `may_change` and `why`. `survey` is `[{"channel","dbm"}]` for 11, 15, 20, 25
  (`dbm` absent until measured); `noise`, `battery` and `heard_s` are absent when not known; empty
  addresses are "". A device's page adds `ieee`, `nwk`, `manufacturer`, `model_id`, `sw_build`,
  `joined_at`, `sleepy`, `check_in_s`, `setup_step`, `can_identify`, `wake_words`, `add_words`,
  `note`, `inputs` and `siren`/`keypad`/`fob` (each only when the device can take it, with `fit`).
- Every `ui_zb_req_result_t` has its word: the list above, then `wrong_code` and `locked_out`
  (the Panel Manager never sends a code, so it sees them only if a later backend answers them). A
  body it can't read answers `invalid` (an `id` missing or 0 `not_found`, an address
  `bad_address`, a code longer than `UI_ZB_CODE_TEXT` `bad_code`); `?device=` naming no device is
  `404 not_found`.
- The request goes through `zigbee_service_web_request(req, allowed)` with `allowed` true: the
  Panel Manager's access table decided it (a signed-in installer, the panel not armed), as for
  Z-Wave. It never reads `installer_code` (wiped on the way).
- A siren's supervision offers the default, 1 h, 4 h and 24 h and keeps a stored value outside
  them; never "Off" (`zbz_wd.h`). The network reset asks twice, then the password (BL-135's
  10-minute rule, the open question above unchanged).

## 15. MQTT (ZB-026)

Built in `alarm_proto` (ZB-M5; `include/alarm_proto/proto_zb.h`, golden payloads in
`test_proto.c`; docs/features/10 MQTT-021, MQTT-022). Additive (topics and keys are a contract),
with docs/features/10 and the ha-addon in step:

- `<base>/zigbee/state` (retained): `{"radio":"working","channel":15,"devices":5,
  "not_heard":0,"jammed":false}`; `radio` is one of `not_connected`, `starting`, `working`,
  `not_answering`, `paused`, `socket_zwave`, `unusable` (`ui_zb_radio_t`'s numbers), `channel`
  null while there is no network. Home Assistant: a sensor "Zigbee"
  (`homeassistant/sensor/<node>/zigbee/config`, entity_category diagnostic) whose state is
  `radio` and whose attributes are the payload.
- Per zone fed by a Zigbee device, `"battery":87` (percent) and `"signal":"good"|"fair"|"weak"`
  (`ui_zb_signal_t`'s buckets) after the keys every zone state has, each only when known
  (`proto_zone_state_diag`): attributes of the zone's existing entity (no new entity per device;
  the device's own page stays the panel's).
- Join, leave and network events reach MQTT as the events of section 11 (`event_type`
  `zigbee_*`), like every other event; Home Assistant's event entity lists them.
- Built in ZB-M6 (net_mqtt, wired by panel_app through `net_mqtt_zb_port_t`): the state is
  published when it changes, checked at most once a second, and again with every full
  republish and on each connection; the sensor's discovery document has bit 2 of the stored
  discovery map's `extra` byte (`DISC_EXTRA_ZIGBEE`; the map's size is unchanged and older
  firmware ignores the bit, so after a downgrade the Zigbee sensor stays in Home Assistant until
  it is deleted there or the panel is upgraded again), so it is retracted with the rest; each zone a Zigbee device feeds is
  published through `proto_zone_state_diag` with that device's battery and signal, read again
  whenever zbz_panel says something a page shows changed.

## 16. Memory and tasks

`zbz_panel_t` is about 20 KB (the registry most of it) and lives in PSRAM (`EXT_RAM_BSS_ATTR`);
nothing an ISR or DMA touches is in it. The zigbee task's stack stays in internal RAM because it
writes NVS. The stack's own heap is measured on the bench before raising `ZB_MAX_CHILDREN`.

Decided in ZB-M6 (without waiting for review): the zigbee task's stack is 8 KB, as
Z-Wave's, raised from 4 KB before it was measured - `zbz_panel_on_report` copies a registry
record (about 300 bytes) and the keypad policy's frames, the store takes a 256-byte record at a
time, and the pump holds an event (about 180 bytes) - 4 KB more internal RAM. `zigbee status` and
`GET /dev/v1/zigbee` (`stack_free`) show the high-water mark; the bench reads it with devices
joining, a keypad arming and the store writing, and the size comes down if more than half stays
unused. The engine's news for the sirens and keypads (outputs and partition views) come through
the start-up's own listener (panel_app's `on_output`, `on_snapshot`), since alarm_service's
listener table is full; they are copied into a queue and a mailbox the zigbee task empties, so
the alarm task never waits for the zone lock. Install codes and a keypad's Arm code pass through
PSRAM copies (the request slot, the store's buffer, the keypads' queue, the radio's code slots)
that are wiped once used: the request slot as soon as the zigbee task takes it (and never holds
the installer code); a keypad's Arm or Bypass never enters the radio's event queue, whose slots
keep their bytes after a read, but waits in a code slot the pump wipes (BL-151). A key
fob's command runs as its user with panel_web's state guard (an arm only while the partition is
disarmed, a disarm only while it is not; a panic in any state), so a stale partition view in
zbz_ace can't arm or disarm against the engine's state.

## Open questions

1. Which esp-zigbee-lib call drops one device's link key and address entry (Forget)? None was
   found; until then Forget removes what it can and the device, if it comes back, is refused only
   when `codes_only` is on.
2. Does the stack's network reset restart the whole panel (as `esp_zigbee_factory_reset` is said
   to)? If so the reset page says so first, and the registry is emptied before.
3. The stack's NVS namespace name, whether install codes survive a restart, and NVS free pages
   (BL-036 step 5).
4. LQI thresholds on the H2 (section 7).
