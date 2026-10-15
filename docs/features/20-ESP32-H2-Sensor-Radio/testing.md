# 20 · Zigbee: how it is proven

Part of [docs/features/20](README.md). The test design for the Zigbee track (ZB-M1 to ZB-M11):
what each layer is tested with, the fixtures, and which test file proves which requirement. It
follows what the mature open Zigbee stacks learned (zigbee-herdsman's mocked adapter and golden
ZCL frames, zigpy's per-step ZDO failures and exhaustive IAS WD bit tests, ZHA's recorded device
signatures, zha-device-handlers' whole-registry quirk checks, deCONZ's declarative device files),
in this repository's way: portable C, host tests named after the requirement they prove
(AGENTS.md), frozen fixtures for stored formats, no network in a test. Device management is
[devices.md](devices.md), sirens and keypads [alarm-devices.md](alarm-devices.md).

## 1. The layers

| Layer | Proven by | Where |
|---|---|---|
| The contracts compile and agree | `test_zbz_contract.c` (STRICT warnings) | `components/core/zigbee_zones/test` (A1, now) |
| ZCL frames as bytes | golden frames both ways (§3, §4) | `test_zbz_zcl.c`, `test_zbz_wd.c`, `test_zbz_ace.c` |
| The registry and its store | codec, frozen fixtures, damaged and future blobs (§6) | `test_zbz_registry.c` |
| A device's setup and the operations | the fake radio and virtual devices from signatures (§2), replays at three profiles (§7) | `test_zbz_panel.c`, `test_zbz_replay.c` |
| The sensor source | as today, plus test mode, forget and setup hand-off | `components/core/zone_hub/test/test_zigbee.c` |
| Model and quirk tables | whole-table checks (§5) | `test_zbz_models.c`, `test_zigbee.c` |
| Sirens and keypads as policy | pure functions and tables (§4, §9) | `test_zbz_wd.c`, `test_zbz_ace.c` |
| The pages | the preview's walk shots and its stuck-walk and cut-text checks (§10) | `host/ui_preview` (BL-144) |
| The Panel Manager | the mock panel and node tests (§10) | `components/services/panel_web/web` tests (BL-145) |
| The firmware | builds with Zigbee in every P4 image, size check | CI (BL-146) |
| The radio, real devices | the bench steps (§11) | BL-036 and the milestones' bench steps |

## 2. Device signatures and the fake radio

**Signatures are data.** Each fixture row is one real device as it describes itself: a fictional
IEEE and short address (locally administered addresses such as `0x02AB_CD00_0000_0001`: never a
real device's, AGENTS.md "Conventions"), the node descriptor (logical type, MAC capabilities,
manufacturer code, server mask), Basic manufacturer and model, power source, and each endpoint's
profile, device id, input and output clusters. And what the panel must make of it: the
`zsrc_zb_desc_t` per endpoint, the matched quirk and model rows, the role, the device class,
router or sleepy, the enrollment path and the supervision window. Rows live in reviewed JSON
(`components/core/zigbee_zones/test/signatures/*.json`); `tools/zb_signatures.py` generates the
C table and its `--check` mode keeps them in step in CI (like `lvpro_gen.py --check`). New rows
come from the console (`zigbee device <id> signature`, ZB-M6) on the bench, not by hand.

Seed rows (from the zigpy/zha device files and deCONZ device files; each still to be captured
from a real unit before a quirk depends on it):

| Device | What it tests |
|---|---|
| Sonoff SNZB-04PR2 (mfr 4742, end device, ep 1 0x0104/0x0402, in 0000 0001 0003 0020 0500 FC57 FC11, out 0003 0019) | a plain IAS contact with Poll Control and vendor clusters |
| Sonoff SNZB-02D (ep 1 0x0104/0x0302, in 0000 0001 0003 0020 0402 0405 FC11 FC57) | temperature only, no IAS |
| Third Reality 3RDS17BZ (4659, ep 1 0x0104/0x0402, in 0000 0001 0500 FF01, out 0019) | IAS without Identify |
| Aqara lumi.magnet.agl02 (4447, ep 1 0x0104/0x0402, in 0000 0001 0003 0500) | a newer Aqara contact that IS IAS, unlike the older `lumi.sensor_magnet` row |
| Aqara lumi.motion.agl04 (ep 1 0x0104/0x0107, in 0000 0001 0003 0406) | occupancy, not IAS |
| IKEA PARASOLL (4476, IAS on ep 2 0x0104/0x0402 in 0000 0003 0500; ep 1 0x0103 with 1000 FC7C) | which endpoint is chosen |
| CentraLite 3300 (4174, ep 1 in 0000 0001 0003 0020 0402 0500 0B05) and 3326-L (a second endpoint on profile 0xC2DF) | IAS plus temperature; a vendor profile endpoint kept last |
| Heiman SmokeSensor-EM (4619, ep 1 0x0402 with 0500 and 0502) | a smoke sensor with its own sounder |
| Tuya TS0216 `_TYZB01_8scntis1` (router, mains, ep 1 0x0104/0x0403 in 0000 0001 0003 0500 0502 0B05, Green Power ep 242 profile 0xA1E0 type 0x0061) | a siren; the Green Power endpoint ignored |
| Frient SIRZB-111 (router, MAC flags 142, 4117, ep 1 profile 0xC0C9, ep 43 0x0104/0x0403 in 0000 0001 0003 0004 0500 0502, ep 242) | a siren on endpoint 43 |
| Frient KEPZB-110 (end device, 4117, ep 1 profile 0xC0C9, ep 44 0x0104/0x0401 in 0000 0001 0003 0020 0500 0B05, out 0003 000A 0019 0501) | a keypad on endpoint 44 |
| CentraLite 3405-L (ep 1 0x0104/0x0401, 0501 in and out) | a keypad with ACE both ways |
| Xfinity XHK1-UE as URC4450BC0-X-R (ep 1 0x0104/0x0401, in 0000 0001 0500 0402, out 0501) | a keypad (deCONZ) |
| Heiman HS1RC (RC-EF-3.0, RC_V14, RC-EM, RC-N; sleepy; in 0000 0500, out 0501) | a key fob |
| Neo/Tuya TS0601 siren (0xEF00 only) | unsupported: decided by clusters, not by brand |

A **converter test** checks each row's `zbz_dev_desc` against the `zsrc_zb_desc_t` the row
expects - the exact facts `zigbee_service` will hand the source.

**The fake radio** implements every `zsrc_zb_radio_t` hook by *encoding* the real ZCL or ZDO bytes
into a transmit log (with `zbz_zcl.h`); a **virtual device** built from a signature row decodes
them and answers through the same decoders the firmware uses, as `zsrc_zb_report_t`s and the
source's inbound calls. Fault knobs per device and per step: drop the next N frames, delay,
duplicate the last frame with the same sequence number, answer with another sequence number,
answer a non-success status, time out; and a **sleepy** mode where the device answers only inside
a wake window opened by its own frame or check-in. The same fake runs in `host/ui_preview` and
`host/ui_sim` (replacing today's `zb_*` stubs in `host_panel.c`), so the pages and the tests
exercise one model.

## 3. Golden ZCL frames (ZB-003, ZB-022, ZB-024)

Each vector decodes, then re-encodes to the same bytes (header | payload, hex). The Source column
is what the open projects say, read on 2026-10-03 (BL-143) at the commits named under the table:

- **verified (bytes)**: the same frame is in a project's own test.
- **verified (layout)**: the command ids, field order, widths and header bits match the projects'
  definitions and what their code sends or parses, but no project holds this exact byte string.
- **derived**: the ZCL field rules only; still to be checked against Wireshark's ZCL dissector on a capture.

| Frame | Bytes | Decodes to | Source |
|---|---|---|---|
| IAS ACE Arm | `01 57 00 \| 00 06 31 32 33 34 35 36 00` | disarm, code 123456, zone 0 | **verified (bytes)**: zigbee-herdsman [test/zcl.test.ts][hz], test "ZclFrame from buffer ssIasAce arm command" (`1, 87, 0, 0, 6, 49..54, 0`: frame type 1, direction 0, Default Response not disabled, payload armmode 0, code "123456", zoneid 0); layout also zigpy [security.py][zs] `arm` |
| Arm all zones, empty code (a fob) | `01 57 00 \| 03 00 00` | all zones, no code | **verified (layout)**: zigpy [security.py][zs] `arm` (enum8 mode, character string - empty is `00` - u8 zone id), zigpy/zha [alarm panel test][zhp] (`[Arm_All_Zones, '', 0]`), deCONZ [ias_ace.cpp][da] (code "allowed to be empty"); the bytes are in no test. A real fob sends the zone id the CIE enrolled it with (herdsman gives 23), not 0: the policy only reads it |
| Arm Response all armed / invalid code | `19 57 00 \| 03` / `19 57 00 \| 04` | same sequence as the request | **verified (layout)**: zigpy [security.py][zs] `arm_response` 0x00, one ArmNotification byte (3 All_Zones_Armed, 4 Invalid_Arm_Disarm_Code), deCONZ [ias_ace.cpp][da] writes one byte. Frame control `19` (cluster command, server to client, Default Response disabled): deCONZ's Arm Response, herdsman's [commandResponse][hep] (server to client, disabled by default) and zigpy's [Cluster.reply][zr] do the same |
| Panel Status Changed: exit delay 30 s, sound | `19 58 04 \| 04 1E 01 00` | | **verified (layout)**: zigpy [security.py][zs] `panel_status_changed` 0x04 (panel status, seconds remaining u8, audible notification, alarm status; Exit_Delay 4, Default_Sound 1, No_Alarm 0), zigpy/zha [alarm panel test][zhp] (command 4, seconds 0); frame control as above |
| Get Panel Status / its answer in alarm | `01 57 07` / `19 57 05 \| 07 00 01 03` | in alarm, emergency | **verified (layout)**: zigpy [security.py][zs] `get_panel_status` 0x07 (no payload) and `panel_status_response` 0x05 (the four fields above); deCONZ [ias_ace.cpp][da] answers with panel status, seconds, audible `01` and, in alarm, alarm status `03` - the payload `07 00 01 03` - but with frame control `09` (Default Response not disabled): **differs**, ours is `19` |
| Emergency, Fire, Panic | `01 57 02`, `01 57 03`, `01 57 04` | | **verified (layout)**: zigpy [security.py][zs] `emergency` 0x02, `fire` 0x03, `panic` 0x04, no payload; zigpy/zha [alarm panel test][zhp] sends a Panic as command 4 with an empty list |
| IAS Zone Enroll Request | `09 57 01 \| 15 00 15 10` | contact 0x0015, manufacturer 0x1015 | **verified (layout)**: zigpy [security.py][zs] `enroll` 0x01 (zone type u16, manufacturer code u16; Contact_Switch 0x0015), herdsman [test/device.test.ts][hd] ("replies to IAS Zone Enroll request": cluster command, server to client, Default Response not disabled = `09`, zone type then manufacturer code), deCONZ [ias_zone.cpp][dz] (u16, u16) |
| Enroll Response | `01 57 00 \| 00 05` | success, zone id 5 | **verified (layout), frame control differs**: zigpy [security.py][zs] `enroll_response` 0x00 (code, zone id; 0 Success, 1 Not_supported, 2 No_enroll_permit, 3 Too_many_zones); herdsman [device.ts][hdev] and deCONZ [ias_zone.cpp][dz] both send it with the Default Response disabled (`11`), herdsman with the request's sequence number; ours is `01`, which asks for one |
| Write IAS CIE Address | `00 57 02 \| 10 00 F0` + 8 address bytes low first | | **verified (layout)**: zigpy [security.py][zs] `cie_addr` 0x0010 EUI64, herdsman [device.ts][hdev] writes `iasCieAddr`, deCONZ [ias_zone.cpp][dz] attribute 0x0010 of type 0xF0; a Write Attributes record is attribute id u16, data type, value (herdsman [foundation.test.ts][hf] `write`: `18 0A 02 \| 01 40 21 08 00 \| 02 40 21 04 00`); an EUI64 goes low byte first (zigpy [test_types.py][zu]). herdsman asks for no Default Response on a write (`10`), ours is `00` |
| Zone Status Change Notification | `09 4B 00 \| 26 00 00 00 00 00` | status 0x0026 (alarm 2, tamper, restore), no extended status, zone 0, delay 0 | **verified (bytes)**: zigpy [tests/test_zcl_clusters.py][zt] `test_ias_zone` (`b"\tK\x00&\x00\x00\x00\x00\x00"`); fields in the order of zigpy [security.py][zs] and deCONZ [ias_zone.cpp][dz] |
| Global frames: Read (`10 0A 00 \| 00 00 05 00`), Read Response, Report, Write, Write Response (also two refusals), Configure Reporting (three records: analog with a change, a bool without, a timeout), its Response, Default Response (`18 04 0B \| 0C 82`), a manufacturer-specific Write Response (`14 5F 11 0F 04 \| 11 22 00`) | as in `test_golden_project_vectors` | status, attribute ids, values as the projects' tests assert | **verified (bytes)**: zigbee-herdsman [test/zspec/zcl/foundation.test.ts][hf] and [test/zcl.test.ts][hz], copied byte for byte into `test_zbz_zcl.c`; the Configure Reporting request is read with its direction bit cleared and its timeout record refused by design (the panel never sends one) |

The commits read (permalinks): zigbee-herdsman `03495312e131` (2026-10-02), zigpy `fb63170d520e`
(2026-09-29), zigpy/zha `f3eb54c0fa42` (2026-10-01), deconz-rest-plugin `a4c17adfa04a`
(2026-09-10). The IAS WD frames of §4 are sourced in the header of `test_zbz_wd.c`: the spec layout
(zigpy, Wireshark, ZHA, deCONZ's SIRZB-111 file) against the reversed one (NXP's JNUG3132
Zigbee guide, zigbee-herdsman-converters). The pages were read through a fetch tool that summarises them, so a
byte string that matters was fetched at least twice; none of these links is a substitute for the
bench capture of §11.

Where the projects differ from ours (no code changed; the maintainers decide): the frame control of the
Enroll Response (`11` there, `01` here), of Read, Write and Configure Reporting requests (`10`,
`00`) and of deCONZ's Get Panel Status Response (`09`, `19`). The enroll refusal codes follow
the ZCL: 0x02 "No enroll permit" (`ZBZ_IAS_ENROLL_NO_PERMIT`), 0x03 "Too many zones"
(`ZBZ_IAS_ENROLL_TOO_MANY`).

"Derived" vectors are built from the ZCL field rules; each test says which kind it is. Negative
cases: a code length byte past the frame (invalid code, never truncated), a code longer than any
the panel has, an arm mode above 3, a frame shorter than its fields, a payload longer than
`ZSRC_ZB_ZCL_PAYLOAD_MAX`.

[hz]: https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts
[hf]: https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts
[hd]: https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/device.test.ts
[hdev]: https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/src/controller/model/device.ts
[hep]: https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/src/controller/model/endpoint.ts
[zs]: https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py
[zt]: https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_zcl_clusters.py
[zr]: https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/__init__.py
[zu]: https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_types.py
[zhp]: https://github.com/zigpy/zha/blob/f3eb54c0fa42/tests/test_alarm_control_panel.py
[da]: https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/ias_ace.cpp
[dz]: https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/ias_zone.cpp

## 4. IAS WD bits (ZB-023)

- All 256 Warning Info bytes decode and encode back to themselves, in the spec layout and in the
  reversed layout (zigpy's exhaustive test style); masks per Wireshark's
  `packet-zbee-zcl-sas.c`.
- Start Warning burglar + strobe + high, 60 s, duty 50, strobe high: `01 57 00 | 16 3C 00 32 02`
  (spec layout); the same in the reversed layout; Stop (mode 0, level high: `02`); Squawk armed,
  no strobe, very high `01 57 01 | 03`; disarmed, strobe, high `01 57 01 | 1A`.
- `zbw_mode_for` for every engine pattern and the one-tone flag.
- The policy against the fake radio: a bell output on sounds every siren of its partition that
  wants that kind of alarm, refreshes before MaxDuration, stops when the output goes off; a
  squawk on arm and disarm; Test refused while armed; an unconfirmed warning raises the bell
  trouble after `ZBW_SEND_TRIES`, cleared when confirmed; IAS Zone trouble bits raise it too; a
  siren silent past `ZBW_SILENT_ROUTER_S` (or `ZBW_SILENT_SLEEPY_S` when sleepy, or its own
  `supervision_min`; under `ZBW_SUPERVISION_MIN_MIN` refused as ZBW_ERR_ARG) raises it, one
  second earlier doesn't, its next frame clears it, and nothing counts while the radio is down
  (`zbw_radio`); the device port's `checkin` is never called (alarm-devices.md §4).
- **Wired in zbz_panel** (`test_zbz_panel.c`, `test_siren_follows_the_bell`): a Frient SIRZB-111
  set up through the fake radio, assigned with SIREN, follows the real engine's bell (alarm_sim):
  a burglary's STEADY bell is a burglar Start Warning, the code's silence a Stop; "Test it" sounds
  it once and its result (DONE, then NO_ANSWER when it never confirms) reaches the page's
  `last_test`, refused while armed; silent past its 60 min supervision it raises the bell trouble
  through the owner's port, its next frame clears it; `zbz_panel_take_wd` and a load give the
  assignment back with `model_flags`, `max_duration_s` and `sleepy` refilled, and a damaged value
  loads none.
- **Before BL-147 ships:** one real siren's Warning Info bit order is captured on the bench and
  its row's layout fixed; until then the reversed layout is marked unconfirmed.

## 5. The model and quirk tables (ZB-007)

In zha-device-handlers' style, over every row of `zb_quirks.c` and `zbz_models.c`: every seeded
signature resolves to its expected row; no row is unreachable behind an earlier prefix row (a
`_TZ3000_` row before a more specific model row); near misses don't match (`LUMI` vs `lumi`,
`_TZE200_` vs `_TZ3000_`, an empty or NULL manufacturer); strings at and past the 32-byte
buffers truncate deterministically and still match by prefix; per-row invariants (UNSUPPORTED
excludes every other flag, a note is non-empty, a supervision time is 0 or sane, no row makes a
life-safety device less so); a brand that ships two behaviours is decided by its clusters (TS0216
with IAS WD, TS0601 with 0xEF00 only).

## 6. The registry and what is stored (ZB-010, ZB-015, ZB-020, ZB-021)

- **Frozen fixtures** next to the test, never edited once released: `registry_fixture_v1.h` (made
  once by the encoder, `DUMP_FIXTURE=1 build/host/test_zbz_registry`, then checked field by field
  against devices.md §4's table) with a full table (64 devices), a device at `ZBZ_EP_MAX`
  endpoints, and a sleepy device stopped mid-setup; `codes_fixture_v1.h` (made by an independent
  Python encoder); the `cfg`, `wd` and `ace` blobs. Each loads with the current code, saves, and
  compares byte for byte. The records carry no CRC of their own: NVS keeps a CRC-32 with every
  blob, and each record has its length, compat and required fields checked.
- The setup step stored in a record (`zsrc_zb_step_t`) has fixed numbers: never renumbered.
- Negative cases: a truncated record, a length past the end, a record without id or IEEE, two
  records with one IEEE, unknown fields (skipped and dropped on the next write only of that
  record), a higher compat (read-only: nothing taken for writing, ever), an empty store. A compat
  byte of 0xFF (`ZBZ_COMPAT_ERASED`, erased or overwritten flash, never a format's compat) is
  damage, not a later format: that record is dropped and erased, the registry stays writable
  (`test_load_erased_compat`, BL-143).
- A record left RUNNING or WAKE reads back STOPPED.
- One migration test per compat or format change, against the earlier fixture.
- Install codes: CRC-16/X.25 vectors for each length (the console's tests' vectors), typed text
  with spaces, dashes and lower case, QR text forms, a wrong length, a wrong CRC, a full list.
- **The NVS budget** (devices.md §4): `test_zbz_contract.c` pins the arithmetic (a record 10
  entries, `zbpanel` at its largest 686, the reserve covering the alarm configuration's rewrite);
  `test_zbz_panel.c` tells `zbz_panel_nvs_free` one entry under `ZBZ_NVS_ADD_MIN` and checks that
  ADD, ADD_CODE, REPLACE and CODE_ADD answer FULL, a device that joins anyway is asked to leave
  (RES_FULL), the status shows `storage_full`, a rename and a keypad's settings still go
  through, and at exactly `ZBZ_NVS_ADD_MIN` (and when never told) nothing is refused.

## 7. Replays (ZB-012)

Interviews and enrollments recorded on the bench (`zigbee trace`, ZB-M6, scrubbed of real
addresses like `zwtrace_scrub.py`) are ordered scripts of {delay, direction, bytes or ZDO tuple},
replayed into `zbz_panel` with the fake radio at three profiles:

- **clean**: the setup ends READY with the expected record;
- **lossy**: every step dropped once, twice, then `ZBZ_SETUP_TRIES` + 1 times - it retries, then
  STOPS at that step, and "Set it up again" finishes it; enrollment gives up after
  `ZSRC_ZB_ENROLL_TRIES` at `ZSRC_ZB_ENROLL_GAP_MS` and status reports still work;
- **sleepy**: steps land only in wake windows; WAKE shows while it sleeps; it STOPS after
  `ZBZ_SETUP_MAX_MS`.

Basic is read in three (devices.md §5): `test_basic_read_split` sets IKEA's PARASOLL up through
the fake radio - its manufacturer and model whole, every answer within 48 bytes.

Edge cases each get a script: Basic on endpoint 2 of [4, 1] and [2, 1]; no node descriptor
(Xiaomi); Basic fails and a model row supplies the identity; an endpoint listed without a simple
descriptor (dropped); IAS Zone already enrolled; status without an Enroll Request; a duplicate
Enroll Request after success; a rejoin with a new short address; an announce from an unknown
address; sequence number 0xFF to 0x00.

Until bench captures exist, the scripts are made by hand from the signatures and the fake radio's
fault knobs, in `test_zbz_replay.c`, through `zbz_panel`, zone_hub's `zsrc_zigbee` and a real
zone_hub with virtual time; each asserts the pages' views and progress, the zones' events, what is
stored and what is logged. Covered (BL-143): the three profiles; every edge case above (Basic read
in three is `test_zbz_panel.c`); a device that leaves mid-setup; a Default Response failure; the
radio unplugged mid-setup and mid-operation and back; a replace; a remove without an answer, then
forget; the walk test in test mode (the frame's seconds, the refresh, one device that refuses and
one that ignores it); the NVS reserve (FULL); a siren following an alarm; a keypad arming. Since
the review of wave 6: the walk test's scope by partition and zone type against the real engine (a
partition-2 burglar and a partition-1 fire during partition 1's walk test both alarm; ZB-022), a
quiet mains siren read by the panel and no trouble in six hours (ZB-023), a network reset whose
every zone shows CHECK within seconds (ZB-013); in `test_zbz_panel.c` a failed erase redone and a
sensor the source has no room for ending FULL (ZB-010), in `test_zigbee.c` the scope per input and
twelve forgotten devices at once. Each bench capture becomes one more script.

**Robustness.** `test_zbz_fuzz.c` feeds random and mutated input from fixed seeds (the test's own
generator) into every decoder: the ZCL frame decoders and attribute iterator, the IAS Zone, WD
and ACE command decoders (`zba_decode_request`), the stored registry, codes, cfg, wd and ace
blobs, and the install-code parsers. Each input is decoded twice with different bytes after its
end (a read past the input shows as a difference), encoders write in front of guard bytes, and
whatever decodes encodes back the same (byte for byte for a ZCL frame; decode(encode(decode(x)))
for a stored blob). CI runs it under the address and undefined sanitizers (`ALARM_SANITIZE`).

## 8. Supervision and sleepy devices (ZB-005, ZB-011, ZB-014)

With real numbers: an hourly Aqara check-in never goes CHECK under the 2 h + 1 h rule; a mains
siren that stops answering a Basic read does after 30 min + 1 h; a Poll Control check-in
releases a queued step or ping; rate-limited unchanged reports still count as check-ins; "Check
it now" on a sleepy device waits for its next frame. Link quality: LQI buckets at their edges;
RSSI and LQI 0 mean "not measured". Each is a script in `test_zbz_replay.c` (BL-143).

## 9. Keypads and fobs (ZB-024)

The policy as a table-driven pure function (`zba_receive` + `zba_result`), against a scripted
alarm task: arm modes 0-3 x code valid, invalid, empty x the partition's state -> the Arm
Response and the posted command; a mode above 3 refused; a length byte past the payload treated
as an invalid code; an empty code accepted only from a fob, acting as its user; a fob's disarm
refused unless `may_disarm`, its panic unless `panic`; five wrong codes lock the keypad (the
engine's counter, through the scripted task) and a locked-out Arm answers invalid code; exit and
entry delays carry seconds that decrease with `now` and are re-sent by Get Panel Status; audible
1 on delay frames; in alarm with the alarm status of the cause; Emergency/Fire/Panic only within
`panic_mask`; Bypass and the zone questions answered (empty, "not allowed"); a frame from an
unassigned device refused; the same sequence number within 60 s answered again without running
again; the answer carries the request's sequence number; per-model rows (0x80 armed, touch
wake, audible, init frame). No test, log or blob ever holds a code after the post (the tests read
the posted command and check the wipe).

Device numbers and sources (alarm-devices.md §3-§4): `zba_free_device` gives 1 first, skips the
numbers other keypads hold and those `taken` names, never gives one above `ZBA_DEVICE_LAST` (8: so
never a Z-Wave keypad's 9-16) and gives 0 when all are held - the page's `NO_DEVICE_SLOT`, the
request's FULL; a keypad re-assigned keeps its number, a cleared one frees it; `zba_assign_keypad`
refuses 0, 9 and another keypad's number; `zba_decode` drops a stored keypad whose number is out of
range or doubled; `zba_devices_held` matches the assignments. A keypad's post carries
`ALARM_SRC_KEYPAD` with its number, a fob's `ALARM_SRC_ZONE` id 0 with its user, and the scripted
alarm task sees a keypad's wrong codes and a Z-Wave keypad's counted apart. The contract's limits
are static asserts in `test_zbz_contract.c`.

A panic always finds room - it takes the oldest waiting Arm's place, whose answer then finds no
ticket and whose retransmission isn't run again - and is BUSY only when panics fill every place;
an Arm with the last one's sequence number and another mode is a new command
(`test_panic_finds_room`, `test_retransmission_matches_mode`, BL-143).

Wired in zbz_panel (`test_zbz_panel.c`): a Frient KEPZB-110 set up through the fake radio and
assigned with KEYPAD gets a number the panel's own keypads don't hold, is greeted, and arms and
disarms the real engine (alarm_sim) through the owner's queue - its post wiped of the code, the
Arm Response with the engine's note; a post the owner couldn't queue is forgotten and the
keypad's retry runs, an answered one sent again is answered again without running; five wrong
codes lock that keypad out while the panel's keypad 1 still works; a panic gets through and its
Default Response carries the keypad's own sequence number, as does a refused one; a Heiman HS1RC
assigned with FOB arms as Alex (user 3); the assignments store and load back with `model_flags`
refilled. With the scripted radio: keypad numbers held, kept on re-assigning, freed when cleared
or removed, `NO_DEVICE_SLOT` and FULL when none is free, and the partition's news sent only when
something the keypad shows changed (`test_keypad_device_numbers`).

## 10. The pages and the Panel Manager (ZB-025, ZB-027)

- **Preview walk** (`pwsh tools/ui_preview.ps1`, BL-144): shots of every page in the list of
  devices.md §9 - the installer menu row in each radio state, the Zigbee page with devices in
  each state, add (listening with its countdown, a device joining, the wake box, each result),
  add with a code (the hex keyboard, a CRC error), the device page (sensor, siren, keypad, fob),
  remove (asked, removed, no answer then forget), radio details (working, paused), channel (with
  the quietest marked), reset, codes waiting, the zone editor hand-off with "Just added", the
  zone page's Zigbee row, the trouble's deep link. The walk fails on a stuck walk or cut-off
  text; the pictures are looked at after each change. Nothing blinks; live words change at most
  once a second.
- **The real Zigbee on the PC** (BL-143 ZB-M6): `host/ui_preview/host_zigbee_real.c` runs the
  firmware's own `zbz_panel`, its `zsrc_zigbee` source registered with the pretend panel's zone
  hub, and the fake radio with seven virtual devices built from the signatures (a door contact, a
  motion sensor, a smoke alarm, a water sensor - a contact signature given the water zone type, as
  no water signature exists yet - a siren, a keypad, a key fob), wired as `zigbee_service` wires
  them: what it stores is kept in memory (each slot as large as the largest value, codes
  included) and read back by a simulated restart, what it logs and a keypad's or fob's command go
  to the engine after the panel's own work (a fob's arm or disarm only in the state
  `alarm_service_command_as` asks for it), a silent siren raises the bell trouble. Decided without waiting for review (2026-10-03): the simulator (`pwsh
  tools/ui_sim.ps1`) runs it by default (`ALARM_SIM_ZIGBEE_DEMO=1` brings back the demo model);
  when the join window opens the first two devices not yet in the network press their pairing
  button 3 s and 6 s later (an install-code add: its own device only) and a sleepy one listens 30 s
  after each of its frames while it is set up, each setup answer 250 ms late so the add page shows
  the steps go by; the side panel's buttons trip their sensors, press
  the next device's button and restart the panel. The preview walk keeps the demo model for its
  BL-144/147/148 shots and adds one segment over the real one (`zb-real-*`: the empty page, the
  add listening, two devices joining and reaching Ready, the device page, a rename);
  `ALARM_PREVIEW_ZIGBEE=1` runs that segment alone. `test_host_zigbee_real.c` (ctest
  `host_zigbee_real`) drives the same zb_* calls without LVGL: add to Ready, the device page,
  rename across a restart, a zone fed through the hub, remove answered when the device wakes, an
  install code, every code at its longest across a restart, a siren's and a keypad's settings, a
  keypad's Arm reaching the engine and its answer coming back, a silent siren's bell trouble, the
  reset's code and the radio unplugged.
- **Panel Manager** (BL-145): the mock panel gains a Zigbee radio and devices; node tests for the
  page's table, the add flow following progress each second, install code entry and QR parsing
  errors, the device actions and the refusal words.
- **Who may, and the reset's code** (ZB-017; devices.md §1, §6): `test_zbz_panel.c` runs every
  request kind with `allowed` false - all NOT_ALLOWED but PING, IDENTIFY, SURVEY and DISMISS - and
  `zbz_reset_code_result` over a table: ALARM_OK from the installer is OK; ALARM_OK from a master
  or a standard user, a wrong code, an empty one and every other result are WRONG_CODE;
  ALARM_ERR_LOCKED_OUT is LOCKED_OUT. The preview walk shows the reset page's wrong-code and
  locked-out words; the Panel Manager's node tests send `reset` without `me_password` (`403
  password_required`) and with a wrong one (`403 wrong_password`), and never an installer code.

## 11. On the bench (BL-036 and each milestone's step)

BL-036's steps 1-5 first (the radio, the network, a real sensor, losing things, keeping the
network). Then per milestone: a sensor's setup on the device page; remove, forget and replace
with a real device; identify; an install-code join; a channel change with a sleepy sensor; a
network reset; a backup and a restore onto the same panel; a siren's Warning Info capture and
each mode heard; a keypad and a fob arming and disarming, five wrong codes, the panel's state on
the keypad. Each recorded device goes into docs/sensors/ with its signature.

## 12. Constants checked against independent sources

An embedded expected table (no network in a test) cross-checks the enums against zigpy's
`security.py` (arm modes, arm notifications 0-6, panel status 0-10, alarm status 0-6, zone types
including 0x0225 warning device and 0x021D keypad), Espressif's ezbee `ias_ace.h` and `ias_wd.h`
(enum values, MaxDuration default 0x00F0, range 0..0xFFFE) and Wireshark's dissector masks.

## Which test proves which requirement

| Requirement | Test |
|---|---|
| ZB-010 registry | `test_zbz_contract.c` (capacities, names, the NVS budget), `test_zbz_registry.c`, `test_zbz_panel.c`, `test_zbz_replay.c` (the reserve), `test_zbz_fuzz.c` (the blobs) |
| ZB-011 signal and diagnostics | `test_zbz_panel.c`, preview |
| ZB-012 setup | `test_zbz_panel.c` (the key exchange's wait: `test_setup_waits_for_the_key_exchange`, `test_a_failed_key_exchange_is_left_alone`), `test_zbz_replay.c` |
| ZB-013 remove, forget, replace | `test_zbz_panel.c`, `test_zbz_replay.c`, `test_zigbee.c` (forget), preview |
| ZB-014 identify, check it now | `test_zbz_panel.c` |
| ZB-015 install codes | `test_zbz_codes.c`, `test_zbz_panel.c`, `test_zbz_fuzz.c` (the parsers), preview, Panel Manager tests |
| ZB-016 channel | `test_zbz_panel.c`, bench |
| ZB-017 network reset | `test_zbz_contract.c` (the code in the request, the appended results), `test_zbz_panel.c`, preview, bench |
| ZB-018 radio page | preview, bench |
| ZB-019 events | `components/core/alarm_core/test` (names, golden), `test_zbz_panel.c` (what is logged) |
| ZB-020 backup | `components/core/backup/test` (policy), bench |
| ZB-021 trust record | `test_zbz_contract.c` (values agree), `test_zbz_registry.c`, `test_zbz_panel.c`, `test_zbz_fuzz.c` |
| ZB-022 walk-test mode | `test_zigbee.c`, `test_zbz_zcl.c`, `test_zbz_replay.c` |
| ZB-023 sirens | `test_zbz_wd.c`, `test_zbz_replay.c`, `test_zbz_fuzz.c` (the wd blob), bench |
| ZB-024 keypads and fobs | `test_zbz_contract.c` (device numbers, the fob's source), `test_zbz_ace.c`, `test_zbz_replay.c`, `test_zbz_fuzz.c` (requests, the ace blob), bench |
| ZB-025 Panel Manager | Panel Manager node tests |
| ZB-026 MQTT diagnostics | `components/core/alarm_proto/test` (golden payloads) |
| ZB-027 touchscreen pages | `test_zbz_contract.c` (empty backend hides them), preview |
| ZB-028 the newest state wins | `test_zigbee.c` (a burst's re-read, a momentary opening while armed), bench (B020) |
