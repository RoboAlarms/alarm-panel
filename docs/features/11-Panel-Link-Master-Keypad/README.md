# 11 · Panel link: master and remote keypads

**Status:** Implemented v1 over Wi-Fi (BL-048): the portable protocol (`panel_link`), the master's
and the keypad's service (`plink_service`), the keypad role on the P4 panels, the screens (approved
2026-10-15) and the PC simulator (`host/link_sim`, `link_smoke` in CI), reviewed three times; on
the two development panels (2026-10-16) pairing, commands, lockout, supervision, a master restart,
removal and pairing again, and a PC keypad with a panel worked, a change shown on the keypad in
about 0.3 s. A keypad's own voice (LINK-028, decided 2026-10-16) works on the panels, tried with
both speakers muted. Left: the bench cases below that need a person or a network change, ESP-NOW
through our C6 build, 433 MHz, keypad inputs, GOTO, keypad updates at the master's order (M8,
designed 2026-10-16: "Updates through the link"). Smart Home and Home Assistant on keypads
designed 2026-10-16 (BL-216), its decisions and screens approved that day. The v1 wire of both
(types 0x2D-0x3A, run-time capabilities, the frozen fixture) is built and host-tested, and so is
Smart Home's and settings' link core (`plink_home`, the master's and the keypad's managers); its
services, the keypad's page and Home Assistant are not built yet. Nothing new is sent, as no
panel offers a capability yet ·
**Milestone:** M7 (keypad updates M8) · **Depends on:** 01, 04, 09, 12, 15

## Goal
Let another panel act as a remote keypad for a master panel, like the keypads on a classic panel's
wired keypad bus: it shows what the master says and sends what a person keys. It must be secure
against sniffing, replay and a device in the middle, supervise every keypad, and keep the master
protecting the house whatever a keypad does. Wi-Fi comes first, because that is what most homes
will use; ESP-NOW and 433 MHz are further transports for the same protocol.

## Reference behaviour
Classic keypads are "dumb" terminals on a wired bus: they send keystrokes and show what the panel
tells them. The panel validates codes and supervises each keypad address (a missing keypad shows a
CHECK trouble). A keypad that loses the bus says so and offers nothing it cannot do. Some panels
add encrypted wireless keypads, enrolled at the panel by the installer.

## Decisions
- **Thin terminals**: keypads never hold user codes or their hashes; the master validates
  everything (ADR-004).
- **One protocol, pluggable transports** (ADR-054): LAN UDP over Wi-Fi first; ESP-NOW (through our
  C6 build) and 433 MHz later. Frames, pairing and sessions are the same on every transport; only
  the peer address and the largest frame differ.
- **Our own encryption** on every transport (ESP-NOW's has small peer limits and none on LAN).
- **Commit-then-reveal pairing** with a 6-digit code compared on both screens, so a device in the
  middle gets one guess in a million per attempt (as Bluetooth's numeric comparison). Every value
  of an attempt is set once (LINK-016).
- **A session handshake on every connection** (ephemeral X25519 keyed by the pairing key): fresh
  keys per session, counters only in RAM, nothing written to flash per frame, forward secrecy for
  the codes typed on a keypad.
- The ESP32-H2 is **not** used for the keypad link (reserved for sensors, ADR-007).
- **Link keypads don't write the engine's device table** (docs/features/20 alarm-devices.md §4):
  the link keeps its own keypad records and supervises them through
  `alarm_service_device_reachable`, as Z-Wave's keypads do (ZW-038).
- **The role is chosen once** (master or keypad), stored in NVS `link`; changing it is a factory
  reset (docs/features/00 "Devices and firmware images").
- **In v1 a keypad works one partition**, its home partition (1..8, given by the master); "all
  partitions" and GOTO wait for KPD-006.
- **Until ESP-NOW exists a Wi-Fi keypad works only while the router and the master have power**:
  the installer guide says to put the router on backup power.
- **Smart Home and Home Assistant on keypads** (BL-216): decided 2026-10-16 (the owner: "Approve
  P1, recommendations OK"), each as the design recommended. The numbers stay, so other documents
  can name them:
  1. A keypad unlocks or opens only with a code valid for its home partition, as for all its
     commands (LINK-027, LINK-048), never any partition as on the master's screen.
  2. Smart Home's Arrange: each panel keeps its own (HCTL-008, "one panel setting").
  3. Device control while armed Away is the same on both screens: allowed; unlock and open need
     the code (HCTL-005; docs/features/24 Open questions).
  4. Home Assistant may change settings while a partition is armed (the panel's own gear is
     locked then, `ui_app.c:2893-2896`), not only while disarmed. Changes are held during an
     alarm, entry time or exit time (UI-046).
  5. Language and region are not set from Home Assistant in v1 (their ids can be appended later).
  6. Home Assistant may set the Beeps volume to 0 (it silences entry beeps, never alarms), as the
     screen and the Panel Manager may (`web_config.c:122`).
  7. The master's own settings are in Home Assistant too: the same entities on every panel.
  8. A keypad without PREFS (older firmware) gets no Home Assistant device until it is updated.
  9. Entities enabled by default: Brightness, Night hours, Appearance, the 3 volumes and
     Announcements; the other 12 are disabled (171 entities with 8 keypads otherwise).
  10. No notice on the panel when a setting changed from elsewhere, as with the Panel Manager
      today.
  11. "Smart Home controls" means the Smart Home view. The automations' warning and Put off are
      left out of BL-216, as their own row (BL-217). Settings > Integrations stays master-only
      (LINK-020).
  12. The Panel Manager's lost update (docs/bugs/B031) is its own row (BL-218), fixed on BL-216's
      mailbox.
  13. Proposal P1 is approved: on the 10.1" keypad, its rail with Smart Home, Smart Home live,
      the code pad "Unlock {0}", link lost ("Offline since 9:12 PM" on every tile, the note
      "Your devices can't be reached right now") and a give-up ("Not confirmed"), from today's
      parts and words only, its pictures from the preview's `host_link_demo`. `ui_devices.c`
      and `ui_link.c` change as it shows, and its screens move into
      `docs/features/07-UI-LVGL-Pro/mockup/screens/panel-link.js` once it is built
      (`kp-devices`, `kp-device-unlock`, `kp-devices-lost`, `kp-devices-unconfirmed`).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| LINK-001 | One master, up to 8 panel-link keypads on device numbers 1-8, shared with the Zigbee keypads: a new keypad takes the lowest number no Zigbee keypad, no link keypad and no enabled entry of the installer's device table holds, chosen under the Zigbee zone lock (docs/features/20 alarm-devices.md §4); numbers 9-16 are the Z-Wave keypads' (docs/features/36 ZW-031) | unit |
| LINK-002 | Keypads render the master's status and send commands; they never validate codes | review + unit |
| LINK-003 | Every data frame is authenticated and encrypted (AES-128-CCM, 8-byte tag) under per-session, per-direction keys; sequence numbers start at 1 and a 64-frame sliding window, updated only after the tag verifies, rejects replayed frames; frames of another session, net or direction are refused | host crypto tests |
| LINK-004 | Pairing needs the installer at the master (installer mode), uses commit-then-reveal, and both screens show the same 6-digit code with the other side's name and address, confirmed on both (defeats a device in the middle) | unit + HIL |
| LINK-005 | ESP-NOW control frames fit in 250 bytes; a UDP datagram is at most 1200 bytes; bulk data (configuration, events, zones) is paged to the transport's largest frame | unit |
| LINK-006 | The master sends status on change (repeated at +250 ms and +1 s, at most one per 100 ms per keypad) and at least every 10 s; keypads send heartbeats every 10 s (±1 s jitter) | unit + HIL |
| LINK-007 | The master raises a device supervision trouble (`alarm_service_device_reachable(n, false)`) for a keypad not heard for its supervision time (5 min by default, per keypad: 1 min, 5 min, 15 min or not supervised; decided 2026-10-15), or one that reports no fresh status for that long; it clears it when both directions are fresh again and before a removed keypad's number is freed; supervision pauses while the master's own Wi-Fi is down (the network trouble covers it) and resumes one period after it is back | unit + HIL |
| LINK-008 | A keypad shows "No system communication" after 30 s without an authenticated status and refuses arming; it never shows stale status as current; from start-up until the first status and configuration are in it shows a steady "Connecting" state | unit + HIL |
| LINK-009 | Automatic failover ESP-NOW <-> LAN within 15 s; duplicates suppressed by sequence number | HIL (ESP-NOW) |
| LINK-010 | The master's ESP-NOW channel follows its Wi-Fi AP; beacons carry the channel; keypads rescan channels 1-13 when lost | HIL (ESP-NOW) |
| LINK-011 | Keypad-attached inputs are reported as zone events (zone expander role) | M7 |
| LINK-012 | Keypad updates at the master's order: a keypad that reaches the update server fetches its own image, otherwise the master relays it over the link (LINK-029..037; [13](../13-OTA-Updates/README.md)) | unit + link + HIL |
| LINK-013 | A PC "virtual keypad" and "virtual master" (host build, UDP, the OS's random numbers) interoperate with each other in CI and with panels | CI + manual |
| LINK-014 | Removing a keypad at the master clears its trouble, sends a sealed BYE (three times), then wipes its keys (RAM first, then NVS); the keypad erases its master record and says it is not paired | unit + HIL |
| LINK-015 | Each connection starts with a handshake (ephemeral X25519 and the pairing key) giving fresh session keys and a session id; a session's keys are installed once and nothing resets its counters; a replayed or forged HELLO never drops a live session; no side sends with seq ≥ 2^31 | unit |
| LINK-016 | Pairing: within an attempt each value (pk_k, pk_m and commit, n_k, n_m, mac_k) is set once - only a byte-identical repeat gets the stored answer, a different one ends the attempt; an attempt counts once its secret is disclosed (the master's at PAIR_REVEAL, the keypad's at PAIR_NONCE), whatever ends it; 3 attempts per window, at least 3 s apart; the window lasts 2 min, 30 s per step, 2 min to compare the codes; a keypad starts an attempt only when a person asks; two keypads or two masters at once end the attempt; nothing is stored before both confirmation MACs verify | unit |
| LINK-017 | Version skew: every payload section carries its length and a receiver takes what it knows and skips the rest; fields are only appended; the session uses the lower protocol version and the common capabilities (this side's offer, masked with `PLINK_CAPS_KNOWN`, AND the peer's, so a bit either side doesn't know is never shared), and a request the master can't serve is answered UNAVAILABLE; an unknown message type is dropped; v1's header never changes; the rules for later versions are LINK-038..042 | unit |
| LINK-018 | Every command carries a request id (a random base drawn at each keypad start, then counted); the master answers a retry from its cache keyed on (keypad, request id) with a digest of the command, kept 90 s after the last copy arrived, and never runs a command twice; the same id with a different command runs as a new one; codes are never stored or logged, and every copy is wiped after use | unit |
| LINK-019 | The keypad's copy of the configuration holds no code material (each code hash replaced by a filler unique to its slot, the salt zeroed) and shows duress users as standard users | unit |
| LINK-020 | A panel in the keypad role runs no alarm engine, event log, zone sources, device sources or integrations; it shows and controls its master's devices through the link (LINK-044..049; designed, BL-216); it updates itself only at its master's order (LINK-029) or through LINK-025, only while its link is up and the master's partitions allow it (OTA-004), and its update self-test needs the link | HIL |
| LINK-021 | On Wi-Fi the master is found by mDNS `_roboalarms-link._udp` on UDP port 6056, announced only while it has a keypad or pairing is open (NET-007), and by a subnet broadcast when mDNS finds nothing; a peer's address moves only on a sealed frame that passes the tag and replay checks | HIL |
| LINK-022 | The link keys are never in a backup (the `link` namespace is not in the backup policy); a factory reset erases them | unit |
| LINK-023 | Panic keyed on a keypad is resent with the same request id every 1 s until answered or 60 s have passed, whatever the link state, and runs once (KPD-007) | unit |
| LINK-024 | On a working Wi-Fi network a keypad shows a change of state within 1 s, and a command's answer within 1 s (Wi-Fi power save off on a master with keypads and on keypads) | HIL |
| LINK-025 | After 10 min without system communication a keypad opens Wi-Fi, Software update, Pair again and "Make it a stand-alone panel" without a code: a keypad holds no codes and the master supervises it, so cutting it off is no worse than unplugging it; a keypad that is not paired (removed at the master, or never paired) opens them at once - no master could check a code, and it protects nothing | unit + HIL |
| LINK-026 | A keypad applies a status block, partition record, zone record or time only when it came in a newer frame of the session than the one it last applied for that item | unit |
| LINK-027 | The master accepts a keypad's commands for its home partition only (NOT_AUTHORIZED otherwise), one at a time with panic in a slot of its own, at most about 2 a second (a burst of 5); requests over their limits are dropped in the link task, before anything reaches the alarm task | unit |
| LINK-028 | A keypad speaks its home partition's announcements (docs/features/08 D05) from the master's pushed events, sounds and status, with its own Announcements switches and Voice volume (D06; decided 2026-10-16); nothing older than its last baseline (a reconnect, a backfill, a master restart) is ever spoken, nor an event the master made more than 5 s before its time now (by the engine time every STATUS carries, or its wall clock), nor the end of an exit time learned more than 5 s after the keypad's own countdown ended, and nothing while the link is not ONLINE; a keypad whose baseline falls in an exit time (it started, or its link came back) says that exit time's end as the master does | unit + HIL |
| LINK-029 | A keypad whose session has `PLINK_CAP_FW` installs firmware only at its master's order, for the version the master names, never older or equal; it runs no update check of its own and offers no Install. Its own Install stays for LINK-025's maintenance, the needs-update state and a master without the bit; the console isn't held back; Stop stays (OTA-027) | unit (`test_plink_fw.c`), HIL |
| LINK-030 | The master orders keypads only after its own update is confirmed, while it runs no update and `proto_fw_install_allowed()` passes on its snapshot; STATUS carries UPDATING and HOLD, and a keypad's gate refuses while either is set; one keypad installs or self-tests at a time, in device-number order; a rollback, or no return within 10 min, stops the rollout for that device and chip revision; meanwhile the master puts off its own restart and doesn't raise that keypad's supervision trouble (up to 10 min) | unit, link |
| LINK-031 | PROBE comes first. FETCH when the keypad's server offers exactly the target. RELAY when the keypad can't reach the server or is offered another version: from the master's running image (same device, chip range, accepted project) or a copy fetched with the keypad's facts. A refusal by the server for that keypad, or of the image itself, is never relayed. A fetch that fails for a reach or transfer reason falls back to the relay once | unit (planner table) |
| LINK-032 | The keypad pulls bytes by image digest (`id8`) and offset, never by session, in order into its writer, within the order's page, count and gap and a 16 KB window, with flash busy at most half the time; it continues across a new session and a master restart with the same image. The master answers statelessly, only to the keypad holding the relay, from a bucket of its own; LINK-024 holds while it runs | link (lossy) |
| LINK-033 | A keypad checks every image itself: the header (chip, revision range, project, newer) before the first flash write, then length, SHA-256, the signature with its own keys, and the version named. Nothing is forced or older. An order's server and channel serve that run only; nothing the master sends becomes trust, except a key a person accepts (LINK-036) | unit; HIL |
| LINK-034 | A READY keypad restarts only after an INSTALL for that version and when its gate passes, asked twice: a live session with a status under 15 s old (ONLINE or needs-update), the master's UPDATING and HOLD clear, `proto_fw_install_allowed()`, no panic still being resent, no kept fire or CO tone. The relay pauses under the same conditions. The panic and tone checks apply in maintenance too | unit; HIL |
| LINK-035 | A relay fails after 60 s without a byte while not paused, or after 30 min paused. A READY image is discarded after 24 h without INSTALL, on CANCEL, on another image's order, or on removal. After 3 failed runs in 24 h a keypad answers BUDGET. Orders are resent after 300 ms, doubling to 2 s; the keypad counts as away after 30 s | unit |
| LINK-036 | An image signed with a key the keypad doesn't trust stops before the download and shows the key's fingerprint; a hold and the installer's code (checked by the master) accept it, never while the factory installer code is in use; accepting stores the order's server and that key as the keypad's trust record | unit; preview |
| LINK-037 | FW_* messages go only on transports whose largest frame is 250 B or more, never on 433 MHz; FW_INFO, FW_ORDER and FW_STATE fit 225 B at their longest; FW_PAGE fits the peer's frame | unit |
| LINK-038 | The floor: a master serves every v1 message above to a protocol-1 keypad for as long as one may be paired, and a later keypad works with a v1 master; neither refuses a peer at protocol 1 or above; a HELLO stays at most 128 B | frozen fixture, frozen peer |
| LINK-039 | New types, ops, kinds and values go only to a peer with their capability bit; the master maps values a protocol-1 peer doesn't know to the nearest v1 one and answers an unknown command type UNAVAILABLE; a keypad decodes an unknown arm state as armed and an unknown sounder as the alarm, sounds fire and CO from STATUS FIRE/CO, and reads an unknown `why` by its decade | future peer, `test_plink_view.c` |
| LINK-040 | Fields are appended only, and absence is known from a section's length; a configuration field is never widened in place; a link record never raises compat and stays at most 1024 B; new per-keypad facts go in new NVS keys | `test_plink_msg.c`, `test_plink_store.c` |
| LINK-041 | With keypads, a master's self-test needs one: `PANEL_OTA_OK_KEYPADS` (bit 1 << 6, `PANEL_OTA_HEALTH_COUNT` 7) beats while a keypad that was online in the minute before "installing" (NVS `link/kup`) has a session again and sends heartbeats; with no such keypad it always beats. A newer master that can't talk to its keypads goes back | HIL |
| LINK-042 | CI runs the frozen v1 message fixture, a future peer and, once released, the frozen v1 binaries in both roles, including a v1 keypad updated on each path | CI |
| LINK-043 | Capabilities are chosen at run time: what a panel offers is an argument of `plink_master_init` and `plink_keypad_init`, masked with `PLINK_CAPS_KNOWN` (0x0007), and each session keeps its own offer AND the peer's, so an unknown bit is never shared (built; no panel offers a bit yet). `PLINK_CAP_HOME` (0x0002) is to be offered by a build with home control, and on a keypad only with its mirror allocated. `PLINK_CAP_PREFS` (0x0004) by every build with the UI. Types 0x32-0x3A go only in a session with their bit and only on a transport whose largest frame is 250 B or more, never 433 MHz. Every payload fits 225 B except DEV_LIST_PAGE, which fits the peer's frame (the codecs, the frozen fixture and the link core's sending built, HOME offered only with a mirror; offering by a panel designed, BL-216) | unit (`test_plink_session.c`, `test_plink_link.c`, `test_plink_msg.c`), frozen fixture |
| LINK-044 | A keypad mirrors its master's device list: a catalog blob pulled by `list_crc` and paged as the configuration, at most 64 devices (v1; more needs a new bit), decoded leniently. A blob that fails is not pulled again and asks for no update. The old list is kept until a new one installs, with each row's state carried over by ref. RAM only (the link core built; the rest designed, BL-216) | unit, link |
| LINK-045 | The master pushes changed device records at most one round per 500 ms per keypad (1 s on frames under 1,200 B), the full set at each new session and on DEV_STATE_REQ, and a summary every 10 s. A keypad whose `states_crc` differs asks after 250 ms, then at least 1 s apart, doubling to 30 s. A record applies only under the installed list and from a newer frame of the session for its device (LINK-026). On a working Wi-Fi network a keypad shows a change within 1 s of the master's own screen (the link core built; the rest designed, BL-216) | unit, link, HIL |
| LINK-046 | While a keypad isn't ONLINE, or has had no device data for 30 s, every mirrored device reads offline since its last device data, as the master's do when Home Assistant drops (HCTL-006). Device data is data under the installed list: data under a list the keypad can't install keeps nothing fresh. A tap then sends nothing (the link core built; the rest designed, BL-216) | unit, preview |
| LINK-047 | DEV_ACT names a device by ref, never by index; the ref and code are zero-padded, so every DEV_ACT has the same size. The master answers copies from its own cache (8 per keypad; an entry is reused 3 s after its last copy and kept up to 90 s) and runs an action at most once. The keypad resends every 500 ms for 3 s, never after, and shows an unanswered action as "Not confirmed" (HCTL-012). The keypad asks for the code itself; the master judges again (the link core built; the rest designed, BL-216) | unit, link |
| LINK-048 | One judge for both screens (`device_service_person_act`). A keypad's action runs as `{KEYPAD, n}` with n from the session, its code checked for the keypad's home partition (decision 1, decided 2026-10-16). A wrong code counts toward that keypad's lockout and its STATUS shows it. Arm-only and guest codes are refused; Z-Wave rows are refused until BL-169 (designed, BL-216; the link core hands `port.dev_run` the keypad's number and home partition) | unit, HIL |
| LINK-049 | Device and settings traffic shares no cache, slot, bucket, task or turn with alarm commands, and goes only in a turn with no STATUS change or RESULT due for that keypad. With 64 devices changing every 100 ms, disarm and panic answer as fast as without it (the link core built, its storm run on the PC; the rest designed, BL-216) | link (storm), HIL |
| LINK-050 | A keypad with `PLINK_CAP_PREFS` reports its settings (field ids frozen in `proto_prefs.h`), its panel id, its voice state and the fields it accepts: at each session's first STATUS, after each save (at most 1 a second), and on PREFS_REQ. HEARTBEAT appends the settings CRC; the master asks again while it differs, at most every 2 s. The master keeps the report in RAM only (the link core built; the rest designed, BL-216) | unit, link |
| LINK-051 | PREFS_SET carries only listed fields to change. One is in flight per keypad, resent every 1 s and given up at 6 s. The answer is a PREFS with its req, the refusals per field and the values now stored. A req is applied once (the answer is kept 30 s). The keypad checks every value itself and applies it through its screen (UI-046) (the link core built; the rest designed, BL-216) | unit, link |
| LINK-052 | Never set over the link: codes, role, pairing, home partition, name, supervision, Wi-Fi and IP, time, language and region, updates, factory reset, the setup, the Panel Manager, developer access. PREFS_SET and PREFS_REQ come from the master only (designed, BL-216) | unit (`test_plink_frame.c`) |

## Frame format (v1)

Little-endian throughout. The 17-byte header is the CCM associated data.

```
off len  field
  0   2  magic 'A' 'L'
  2   1  version = 1
  3   1  type (table below)
  4   1  flags: bit 0 SEALED (CCM payload + tag), bit 1 FROM_MASTER; other bits 0 (refused)
  5   2  net_id: the master's network, random, never 0
  7   2  sender: 0 = the master, 1..8 = a keypad's device number, 0xFFFF = a keypad not paired
  9   4  sid: the session id (0 on plain frames)
 13   4  seq: per session and direction, from 1 (0 on plain frames)
 17   n  payload (ciphertext when SEALED)
17+n  8  tag (SEALED only)
```

Nonce (13 bytes) = dir ‖ sid ‖ sender ‖ net_id ‖ seq, dir = `'M'` (0x4D) from the master, `'K'`
(0x4B) from a keypad. The keys are per session and per direction, so the nonce is unique as long as
seq is.

Plain frames (SEALED clear) are only the pairing and handshake messages and the hints; everything
else is refused unless SEALED. A frame whose SEALED flag doesn't match its type's class, whose
FROM_MASTER flag doesn't match the sender, with an unknown flag bit, or longer than 1200 bytes is
dropped.

A MAC in a plain frame (mac1, mac2, mac_k, mac_m) is the frame's last 16 bytes and covers every
byte before it, header included. Every MAC, tag and commitment is compared in constant time. Both
X25519 results (pairing and handshake) refuse an all-zero answer.

## Messages

Strings are a length byte and UTF-8, at most the size shown; a receiver drops control and bidi
override characters and clamps the length before showing one. "Section" means a length byte
followed by fields, so later fields can be appended (LINK-017).

| Type | Name | Dir | Class | Payload (v1) |
|---|---|---|---|---|
| 0x01 | `PAIR_REQ` | K->M | plain | attempt u32, proto u8, caps u16, kind u8, capacities section (partitions u8, zones u16, users u8, devices u8), pk_k[32], name (≤ 47), fw (≤ 31) |
| 0x02 | `PAIR_COMMIT` | M->K | plain | attempt u32, proto u8, caps u16, pk_m[32], commit[32], master name (≤ 47), panel id (6 chars) |
| 0x03 | `PAIR_NONCE` | K->M | plain | attempt u32, n_k[16] |
| 0x04 | `PAIR_REVEAL` | M->K | plain | attempt u32, n_m[16] |
| 0x05 | `PAIR_CONFIRM` | K->M | plain | attempt u32, mac_k[16] |
| 0x06 | `PAIR_DONE` | M->K | plain | attempt u32, device u8, home u8, mac_m[16] |
| 0x07 | `PAIR_FAIL` | both | plain | attempt u32, reason u8 (honoured only for the current attempt from its peer) |
| 0x10 | `HELLO` | K->M | plain | proto u8, caps u16, capacities section, n_k[16], e_k[32], mac1[16] |
| 0x11 | `WELCOME` | M->K | plain | proto u8, caps u16, n_m[16], e_m[32], mac2[16] |
| 0x12 | `REKEY` | M->K | plain | sid u32 the master doesn't know (a hint, at most 1 a second per keypad) |
| 0x13 | `NOT_PAIRED` | M->K | plain | (none) - a HELLO the master can't place (a hint, rate-limited) |
| 0x18 | `BEACON` | M->all | plain | reserved for ESP-NOW: net_id, channel, name, pairing open |
| 0x20 | `STATUS` | M->K | sealed | system section, then partition records (below) |
| 0x21 | `ZONES` | M->K | sealed | zones_crc u32, first u16, count u16, rec_len u8, count x zone record |
| 0x22 | `ZONES_REQ` | K->M | sealed | first u16, count u16 |
| 0x23 | `CFG_REQ` | K->M | sealed | cfg_crc u32, offset u32, max u16 |
| 0x24 | `CFG_PAGE` | M->K | sealed | cfg_crc u32, total u32, offset u32, len u16, len bytes |
| 0x25 | `EV_REQ` | K->M | sealed | after_seq u32, max u8 |
| 0x26 | `EVENTS` | M->K | sealed | flags u8 (bit 0: older ones were left out), count u8, rec_len u8, count x event record, oldest first |
| 0x27 | `CMD` | K->M | sealed | req u32, op u8, op section, code (len u8 + ALARM_CODE_MAX_LEN bytes, zero-padded), new code (the same) |
| 0x28 | `RESULT` | M->K | sealed | req u32, result u8, user u8, not_ready_zone u16, may_disarm u8 |
| 0x29 | `SOUND` | M->K | sealed | partition u8, sound u8, zone u16 |
| 0x2A | `HEARTBEAT` | K->M | sealed | flags u8 (bit 0 mains lost, 1 battery low, 2 battery present), rssi i8, uptime_s u32, status_age_s u16 (0xFFFF none yet), fw (≤ 31); in a session with `PLINK_CAP_PREFS`, an appended section with prefs_crc u32 (its codec built, not sent yet: BL-216), read only when bytes remain |
| 0x2B | `TIME` | M->K | sealed | epoch_ms i64, tz (≤ 63, POSIX TZ) |
| 0x2C | `BYE` | M->K | sealed | reason u8 (removed at the master) |
| 0x32 | `DEV_LIST_REQ` | K->M | sealed | list_crc u32, offset u32, max u16 (CFG_REQ's layout and codec) |
| 0x33 | `DEV_LIST_PAGE` | M->K | sealed | list_crc u32, total u32, offset u32, len u16, len bytes (CFG_PAGE's layout and codec) |
| 0x34 | `DEV_STATE` | M->K | sealed | head section (list_crc u32, states_crc u32, count u8), then count x (index u8, record section) |
| 0x35 | `DEV_STATE_REQ` | K->M | sealed | list_crc u32, first u8, count u8 (0 = all) |
| 0x36 | `DEV_ACT` | K->M | sealed | req u32; act section (action u8, value i32, mode u8, flags u8 = 0); ref (len u8 + 95 B, zero-padded); code (len u8 + `ALARM_CODE_MAX_LEN` 10 B, zero-padded) |
| 0x37 | `DEV_ACK` | M->K | sealed | req u32, result u8, why u8; optional section (list_crc u32, index u8, record section) |
| 0x38 | `PREFS` | K->M | sealed | answer section (req u32, 0 = unsolicited); info section (panel id ≤ 6, voice u8, accepts u32); refusals section (count u8, count x (id u8, why u8)); fields section (count u8, TLVs) |
| 0x39 | `PREFS_REQ` | M->K | sealed | have_crc u32 |
| 0x3A | `PREFS_SET` | M->K | sealed | req u32 (not 0), origin u8; fields section (count u8, TLVs of the fields to change) |

Types 0x2D-0x31 are in "Updates through the link". Types 0x32-0x3A and the HEARTBEAT section, with
their capability bits, sizes, values and rules, are in "Smart Home and Home Assistant on keypads".
Their codecs, goldens and frozen fixture are built (`plink_msg.h`), and so is the link core that
sends and takes them (BL-216); nothing sends them yet, as no panel offers a capability (LINK-043).

**STATUS.** System section: epoch_ms i64, partitions u8 (mask), home u8 (this keypad's home
partition), sys_troubles u32, sys_restored u32, dev_troubles u32, default_codes u8, flags u8 (bit 0
program, bit 1 exit_sounds), recovery_left_ms u32, lockout_left_ms u32 (this keypad's own), report
u8 (0 none, 1 offline, 2 online: the master's reporting, for the Emergency words, UI-029),
zones_used u16, last_event_seq u32, cfg_crc u32, cfg_len u32, zones_crc u32, mono_ms u64 (the
master's engine time when the frame was sealed, the clock of an event's mono_ms: a keypad's voice
dates an event without the wall clock, LINK-028). Then one record per
enabled partition: number u8, length u8 (the bytes after it), then the fields of
`alarm_partition_status_t` one by one
(state, level, arm_flags, armed_by, arm_source kind u8 + id u16, ready_levels, a flags byte for
ready, chime, walk_test, triggered, exit_error, trouble_unack and quick_exit (bit 6: the exit time
running is a quick exit from the armed state, not an arming; LINK-028), sounder, bell, delay_left_ms u32,
memory, faults, bypassed, alarms, troubles u16 each, walk_left_ms u32, walk_age_ms u32, walk_id
u32, walk_seq u32, walk_tested u16, walk_total u16, walk_flags, walk_started_by, walk_ended_by,
walk_end_how, walk_started_src kind u8 + id u16). A transport whose frame is too small gets several
STATUS frames, each with the system section and some records.

Every duration is worked out when the frame is sealed: left = max(0, field - (now -
snap.taken_ms)) for the delay, walk-test time and recovery; lockout_left_ms is
`alarm_engine_lockout_left({KEYPAD, n})` at that moment. The walk test's start goes as its age and
its raw engine time as an opaque id; the keypad maps local start = arrival - age once per distinct
id (at least 1: 0 means none). taken_ms = arrival; a record a frame doesn't carry keeps its last
one, counted down; and within one countdown a frame that reads less than a second longer than what
the keypad shows (the network's delay varied) keeps the count. So within one delay the seconds a
keypad shows never go up (UI-023).

**Zone record** (7 B): flags u16, trouble u8, walk u8, walk_first_s u16, walk_trips u8.
zones_crc is the CRC-32 of the records of zones 1..min(zones_used, the keypad's zone capacity as
its HELLO states it; all of them when it states 0), the same in its STATUS and its ZONES: a keypad
with fewer zone slots than the master uses (zones added since it paired) compares the zones it
holds and is sent only those. The master pushes ZONES for the range that changed; a keypad whose
own CRC differs after a STATUS asks with ZONES_REQ.

**Event record** (32 B): seq u32, mono_ms u64, epoch_ms i64, type u16, partition u8, zone u16,
user u8, source kind u8 + id u16, arg u8, arg2 u8, flags u8. The master pushes each new event,
sealed, before the STATUS that carries its seq (the Home screen reads the entry zone and who armed
from the events when the state changes), the engine call's one-shot SOUNDs between them (a
keypad's voice then has a batch's tone before its STATUS, LINK-028; a keypad takes either order),
and a STATUS never carries a last_event_seq below an event already pushed to that keypad, under
this session or the one before (only a log that started again lowers it); EV_REQ fills gaps: the
newest `max` events after after_seq,
up to 50, in as many EVENTS as the transport needs (the "left out" flag on the first). A keypad
with nothing asks for the last 50; it keeps 50 and drops its ring on a new pairing and when
last_event_seq goes below its newest. The engine numbers its events one by one, so events that
don't start right after the newest kept (in a frame without the "left out" flag) mean a push was
lost: the keypad keeps them and asks again after the newest it had, at most 4 times. A keypad
holding fewer than 50 whose oldest isn't event 1 (an answer lost, or only a pushed event kept)
asks for the newest 50 again after a STATUS, at most 4 times for one ring.

**Configuration.** The master keeps one encoded copy of its configuration (`alarm_config_encode`)
made for keypads (LINK-019). cfg_crc is that blob's CRC-32 (it also versions it across master
restarts). A keypad whose cfg_crc differs waits 0-500 ms, then pulls it page by page (2 requests
outstanding, retried after 300 ms doubling to 2 s), checks the CRC and decodes it leniently: an
index beyond its capacities is skipped, an unknown value reads as unknown, no validation (a newer
master must not leave older keypads blank). A blob that fails is remembered and not pulled again,
and the keypad says it needs an update while the master's latest status names it; it is
forgotten once another configuration installs or the master names the installed one again (a
release rolled back), so a link lost later never asks for an update. The keypad states its
capacities in PAIR_REQ and HELLO;
the master refuses to pair one that can't hold its zones.

**Commands.** `CMD` op 1 is an `alarm_cmd_t` (op section: type, partition, level, arm_flags, zone
u16, panic, walk_op, walk_flags); op 2 checks a code (Settings access, `authenticate`); op 3
changes the code's own user's code (`change_code`). The master runs every op as
`{ALARM_SRC_KEYPAD, n}` with n from the session, never from the frame, so keypad semantics (quick
arm, auto-stay) and the per-source lockout apply (USR-005), on a worker that never holds the link
task (LINK-027). A command for a partition other than the keypad's home is answered
NOT_AUTHORIZED. Codes are zero-padded to the same length so the frame's size doesn't tell a code's
length; link traffic still shows when a command is sent. Retries: every 500 ms with the same req,
up to 3 s; panic every 1 s for 60 s (LINK-023). While the link is down or still connecting a
command is answered at once with ALARM_ERR_UNAVAILABLE; panic is queued and keeps being sent. The
master keeps 8 answers per keypad: a command's for 3 s after its last copy, a panic's for 60 s
after its first (an answer that may still be asked for is never given up, so nothing runs twice),
and panics never hold the last 2, so a disarm after a run of panics runs. With all 8 held, a new
command is dropped and its keypad's resend gets it once a command's answer is 3 s old.

## Pairing

1. The installer, in installer mode at the master, opens pairing: 2 minutes, announced in mDNS
   (`pair=1`).
2. A person on the new panel chooses *Keypad* (setup wizard, or `plink role keypad` on a bench) and
   starts pairing. It finds a master with pairing open (two of them end it: "More than one alarm
   panel is open for pairing") and sends `PAIR_REQ` with a fresh X25519 key pk_k and a random
   attempt id. When mDNS finds nothing it asks by subnet broadcast (LINK-021): the first master
   to answer an attempt with `PAIR_COMMIT` is the one it pairs with, fixed before n_k goes, and a
   `PAIR_COMMIT` from any other address ends the attempt.
3. The master answers `PAIR_COMMIT`: its fresh key pk_m and
   `commit = SHA-256("plink v1 commit" ‖ pk_m ‖ pk_k ‖ n_m)`; n_m stays hidden. A PAIR_REQ from a
   second key or address during the window ends the attempt ("More than one keypad is trying to
   pair").
4. Only then the keypad sends its nonce n_k (`PAIR_NONCE`); the master reveals n_m
   (`PAIR_REVEAL`); the keypad checks n_m against the commitment it held when it sent n_k.
5. Both show `code = be32(SHA-256("plink v1 sas" ‖ pk_k ‖ pk_m ‖ n_k ‖ n_m)[0..3]) mod 10^6` as
   "123 456", with the other side's name and IPv4 address.
6. Keys: `DH = X25519(own, peer)` (all-zero refused); `TH = SHA-256("plink v1 transcript" ‖
   PAIR_REQ payload ‖ PAIR_COMMIT payload ‖ net_id ‖ n_k ‖ n_m)`, every element after the label
   prefixed with its length as u16; `PRK = HKDF-Extract("plink v1 pair", DH)`;
   `K_pair = HKDF-Expand(PRK, "plink v1 key" ‖ TH, 32)`;
   `K_conf = HKDF-Expand(PRK, "plink v1 conf" ‖ TH, 32)`.
7. The person confirms on the keypad: `PAIR_CONFIRM` carries `mac_k = HMAC(K_conf, frame up to
   mac_k)[0..15]`. The installer allows it on the master. With both, the master picks the device
   number (LINK-001), stores the keypad as provisional (not supervised until its first handshake;
   dropped and its number freed if not heard within 10 min) and with no name: it is "Keypad N"
   until renamed on the alarm panel (decided 2026-10-15; the name it sent shows only during
   pairing, next to the code), and answers `PAIR_DONE` with
   `mac_m = HMAC(K_conf, frame up to mac_m)[0..15]` (device and home included). An identical
   PAIR_CONFIRM gets the same PAIR_DONE for 60 s, even after the window closes. The keypad checks
   mac_m, stores the master and its role, and sends HELLO at once.
8. Repeats: the keypad resends its last message every 1 s (doubling to 4 s). A message that is
   byte-identical to the one already taken for that step gets the stored answer again; a different
   one for the same attempt ends it (PAIR_FAIL, counted) and never replaces a stored value. The
   code, TH, K_pair and K_conf are computed once. A refusal, a timeout or a bad MAC ends the
   attempt: `PAIR_FAIL`, nothing stored.
9. Limits: LINK-016. A paired keypad ignores pairing traffic unless a person started Pair again;
   the master takes PAIR_REQ only while its window is open and only from sender 0xFFFF.

Settled in the code (`plink_pair.c`): u16 lengths, the net id and the attempt id are little-endian
like the rest of the link; a PAIR_REQ for another master (net id neither 0 nor the master's) is
dropped, one to a closed window answered CLOSED; the first address to send a request owns the
window, even one refused for capacity or version; a request less than 3 s after the last attempt
ended is dropped unanswered (the keypad's resend comes back), and a keypad's retry within those
3 s goes when they are up; any message of the attempt that just failed, from its keypad, gets that
attempt's PAIR_FAIL again (the first may have been lost); an attempt under way outlives the
window, which closes when it ends; a new attempt id from the window's keypad ends the attempt
under way (INTERRUPTED); a port failure answers BUSY (master) or fails locally (keypad), not
counted. Each step's payload is kept whole, up to a frame's (1183 bytes), so a newer firmware's
longer one is taken (LINK-017); only a protocol version of 0 is refused (VERSION), and a key of
small order (MISMATCH), both before anything is disclosed. A keypad that asks by broadcast asks
so in every attempt; the service says when it does (its subnet's broadcast, ip | ~netmask, looks
like any address: x.y.z.127 on a /25, while x.y.z.255 can be a host on a /23), and only
255.255.255.255 is a broadcast by its bytes; a `PAIR_FAIL` that comes before any
`PAIR_COMMIT` doesn't end the attempt (another master may still answer) but names the reason if
it times out; the `PAIR_FAIL` of two masters answering goes by broadcast, with net id 0, so both
end their attempt.

## Sessions

After pairing, and at every (re)connection, the keypad starts a handshake:

- `K_hs = HKDF-Expand(K_pair, "plink v1 hello", 32)`.
- `HELLO`: fresh ephemeral e_k and nonce n_k (never reused), `mac1 = HMAC(K_hs, frame up to
  mac1)[0..15]`.
- The master checks mac1 and drops a HELLO whose n_k it has seen in the live session or the last 4
  handshakes. A byte-identical HELLO while its handshake is pending gets the same WELCOME; a new
  one makes e_m and n_m (X25519 at most about 4 a second over all keypads, and for any one keypad
  a burst of 2, then 1 a second, so a device with one keypad's keys can't use up the others').
- `TH_s = SHA-256(HELLO frame ‖ WELCOME frame up to mac2)`,
  `PRK_s = HKDF-Extract(n_k ‖ n_m, K_pair ‖ X25519(e, E_peer))`,
  `HKDF-Expand(PRK_s, "plink v1 session" ‖ TH_s, 68)` = K_m2k[16] ‖ K_k2m[16] ‖ K_fin[32] ‖ sid[4]
  (a zero sid becomes 1). The WELCOME header carries sid 0.
- `WELCOME`: e_m, n_m, `mac2 = HMAC(K_fin, frame up to mac2)[0..15]`. The keypad accepts it only
  while it waits for the answer to its latest HELLO, and only from where that HELLO went (unless
  it went by broadcast, as the service said when it gave the address: the subnet's broadcast is
  named, never guessed from its bytes); mac2 needs an X25519 first, so it works out at most 8
  WELCOMEs per HELLO
  (the next HELLO opens the count again). It wipes e_k and n_k, and makes the new session
  tentative: it sends a HEARTBEAT at once and every 500 ms until a sealed frame of the new session
  arrives, and still accepts the old session's frames meanwhile.
- The master keeps the live session until a sealed frame arrives under the pending one (so a
  replayed HELLO can't cut off a live keypad), then promotes it and sends STATUS and TIME at once,
  the STATUS repeated at +250 ms and +1 s as a change's is (the keypad has dropped the old session
  and has no status in the new one until one arrives).
  A pending session that equals the live sid is refused; a pending session expires after 20 s;
  promotion or expiry discards the cached WELCOME.
- The keypad starts a handshake at start-up, after 15 s without an authenticated STATUS (counted
  from its last WELCOME too: a new session brings a STATUS at once; other frames getting through
  don't keep a session whose STATUS doesn't), on a `REKEY` hint for its own sid (at most every
  5 s), at seq 2^30, and every 24 h; failures back off
  1, 2, 4 ... 30 s with jitter (2 s at most while a panic waits). It re-resolves the master before
  every retry after the first failure. It keeps its live session until a WELCOME verifies.
- `NOT_PAIRED` hints only change what the keypad says, after they repeat for 5 min; the keypad
  keeps its keys until a new pairing completes.
- HELLO and WELCOME each carry their sender's own proto and caps; both sides take the lower proto
  and the common caps (their own offer AND the peer's, LINK-043), and a proto of 0 is refused. A
  sealed frame spends its seq even when sealing fails, so no nonce is tried twice; a frame with
  seq ≥ 2^31 is refused on arrival too.

## Timers

| What | Value |
|---|---|
| Status from the master | on change (+250 ms, +1 s repeats, ≤ 1 per 100 ms per keypad), and every 10 s |
| Heartbeat from a keypad | every 10 s, ±1 s jitter; every 500 ms while a new session is tentative |
| Keypad: new handshake / link lost | 15 s / 30 s without an authenticated STATUS |
| Master: supervision trouble | the keypad's supervision time (5 min default) |
| Command retry / give up | 500 ms / 3 s |
| Panic resend / kept | 1 s / 60 s from keying |
| Master's result cache | 90 s after the last copy of a request |
| Pending handshake | 20 s |
| Configuration and event pulls | retried after 300 ms, doubling to 2 s; 2 outstanding |
| Pairing window / per step / code comparison / between attempts | 2 min / 30 s / 2 min / 3 s |
| Provisional keypad dropped | 10 min without its first handshake |
| Keypad's no-code maintenance | after 10 min without system communication |
| TIME | at each new session, on a change of time zone or a clock step, every 10 min |
| Device data build (master) | On a registry change, at most every 250 ms; at once after a device action; compared every 2 s |
| DEV_STATE | Changed records ≤ 1 round per 500 ms per keypad (1 s under 1,200 B frames); full set at a new session; summary every 10 s |
| Keypad: device data stale | 30 s without a DEV_STATE or DEV_ACK under the installed list |
| DEV_STATE_REQ | 250 ms after a mismatch, then ≥ 1 s apart, doubling to 30 s |
| DEV_ACT retry / give up / screen wait | 500 ms / 3 s / 1 s (3 s with a code) |
| PREFS / PREFS_REQ | ≤ 1 a second / ≤ every 2 s while the CRC differs |
| PREFS_SET retry / give up / keypad's answer kept | 1 s / 6 s / 30 s |
| A remote change of how the screen looks | ≤ 1 per 2 s per panel; waits up to 2 s |

Of the last eight rows, from "Device data build", the link core's timers are built and
host-tested (BL-216); the device data build, the screen's wait and a remote change of how the
screen looks are designed, not built ("Smart Home and Home Assistant on keypads").

## Transports

| Transport | Peer address | Largest frame | Discovery | Status |
|---|---|---|---|---|
| UDP over Wi-Fi (LAN) | IPv4 + port | 1200 B (no IP fragments: lwIP reassembly is off) | mDNS `_roboalarms-link._udp`, port 6056, TXT `v`, `net`, `pair`, `id`; subnet broadcast to 6056 when mDNS finds nothing | built first |
| ESP-NOW (C6 bridge on the P4) | MAC | 250 B (v1) | `BEACON` on the master's channel | later (CP-003) |
| 433 MHz | radio id | tens of bytes: status and commands only; never firmware (LINK-037) | - | later, an idea (needs a transmitter on both) |

The core sends through a port (`plink_net_port_t`: an opaque address with its transport, the
largest frame per transport); the platform hands every received datagram in with its source
address. The master answers a keypad on the address of its last authenticated frame (a WELCOME
goes to its HELLO's source without moving it). Firmware with the link sets
`CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` so a burst from 8 keypads isn't dropped.

## Roles on a P4 panel

- **Master** (the default, and every panel before this feature): runs everything it runs today,
  plus the link's master side when the feature is in the build (`ALARM_LINK`).
- **Keypad**: `panel_app` starts the board, the sounder, the language and asset packs, its voice
  (fed from the link, LINK-028), Wi-Fi, updates, the console, power monitoring (on the health task)
  and the touch UI with the link's keypad backend, and nothing that runs the alarm: no engine
  (`alarm_service` drops posts instead of queueing them), event log, zone sources, MQTT,
  notifications, Home Assistant, device sources, automations or Panel Manager.
- **Updates on a keypad**: the update gate is the link's (`panel_ota_set_idle_gate`): an update or
  a C6 update starts only while the link is up and the newest status, under 15 s old, passes
  `proto_fw_install_allowed()`; refused while the link is lost, except through LINK-025. The
  self-test mask is UI + TOUCH + LINK (`PANEL_OTA_OK_LINK`, beaten on authenticated frames).
  Keypads update at the master's order, after the master has confirmed its own update (LINK-030),
  and restart only at its INSTALL ("Updates through the link").
- **The keypad backend** serves from the link: snapshot, configuration, recent events, commands,
  code checks and code changes; locally: Wi-Fi, screen, sound (its Voice volume and Announcements
  too, LINK-028), language, updates, About. The master-only pages (installer mode, users, zone
  sources, integrations, event log, automations, the zones' spoken-name editor, and devices until
  BL-216 is built) are hidden by a role gate the UI reads (`ui_link_keypad`,
  `ui_voice_editor_present`); `program_enter` and `user_*` answer UNAVAILABLE. User editing never
  goes over the link. Designed for BL-216, its link core built, its service and page not: Smart
  Home from the master's registry, and its own Arrange (LINK-044..049; "Smart Home and Home
  Assistant on keypads").
- **Counters the UI relies on**: snap.seq rises with every status or zone record applied and
  equals `snapshot_seq()`; config_seq rises only when a newly decoded configuration is installed;
  `get_config` is true once one is.
- **Home partition**: kept in the keypad's link record; `panel_app` sets `ui_options_t.partition`
  from it before the UI starts; a later change reaches the UI through a backend entry with a
  generation number, checked on the LVGL task. The plink task never takes `board_ui_lock` and never
  calls the UI.
- **Sounds**: the keypad plays its home partition's continuous pattern and one-shot sounds, and
  fire and CO from any partition. On link loss it stops a continuous pattern (no stale entry or
  burglary tone) except fire and CO, which keep sounding until OFF is pressed on the keypad or the
  link returns (decided 2026-10-15), and shows the steady "No system communication" state
  (LINK-008). OFF is a Disarm or Silence the master didn't run (no link, or no answer within 3 s,
  also in the 30 s before the loss); while the tone is kept, Home offers Silence in the dimmed
  tiles' place (`ui_link_info_t.local_alarm`), and the screen is as during an alarm: woken from
  its saver when the tone is kept, never dimmed or dark while it sounds, lit by day's brightness
  at night too.
- **The voice** (LINK-028; decided 2026-10-16: "It should have its own audio settings for that as
  well to turn it on or off and control the volume like the master panel"): the keypad runs
  `panel_voice` with its own Announcements switches and Voice volume (`panel_prefs`, on as shipped,
  D06) and its own pack, and speaks what the master's voice says for the keypad's home partition
  (D05), the walk test included (WALK-016, every keypad of the partition). Its link task is the
  voice's producer (`panel_voice_set_source`: a baseline is asked of the link, the zones' words
  come from the configuration copy, a pack install answers locked until BL-205), through the
  portable `plink_vfeed`: live only while ONLINE (leaving it pauses the voice, entering it gives a
  silent baseline, as do a new home and a master's log that started again); an event is fed only
  once a STATUS covers it (floor < seq <= last_event_seq), in order and once, and the floor moves
  up, so a backfill, a catch-up or a hole filled late never speaks, nor an event the master made
  more than 5 s before its time now (by the master's engine time in every STATUS, `mono_ms`, or
  else its wall clock); the end of an exit time has no event, so the commit that brings it more
  than 5 s after the keypad's own countdown ended (that STATUS and both repeats lost) is a silent
  baseline instead; the commit waits up to 300 ms for events pushed ahead of their STATUS or a
  hole being pulled, and a batch with events but no sound yet, or a sound alone (a chime), waits
  100 ms for the rest; the bell's test pulse is fed before a walk test's start, as the engine
  emits it. A keypad's baseline in an arming's exit time joins that arming (`ann_join_exit`; its
  silent exit from the snapshot's flags; never a quick exit's time, which the STATUS flags), and
  so does a move from disarmed into an exit time whose arm came too late to say, so a keypad that
  restarts or comes back during an exit time says its end as the master does (found on the
  panels 2026-10-16: the keypad was silent). The master hands a keypad an engine call's events
  only with the snapshot that covers them (`take_engine_news` holds a newer one, each 1 s at most,
  with its call's sounds behind it), so no STATUS covers an event with the state from before it.
  One-shot sounds play tracked while the voice is fed. A zone's
  words speak only with the pack revision they were approved with (the master's installer
  approves them; a keypad has no editor), and the master's D07 settling isn't on the wire, so for
  that moment a keypad may still say a zone's old approved name. Development builds log
  `voice: says <kind> (partition <n>, Voice <v> %)` for each utterance; the `voice` console
  command counts them (both work with the speaker muted).
- **Settings while armed**: when LINK-025's maintenance or a needed update opens Settings though
  the last status said armed, only the gear looks usable (`subject_settings_locked`); Chime and
  Troubles' Acknowledge stay dimmed, as they stay locked.
- **Needs an update**: a keypad that hears its master but can't read its configuration (newer than
  its firmware reads) never gets ONLINE; Home says to update it, Software update opens without a
  code, and the update gate decides on the master's newest status as for ONLINE.
- **Time**: the keypad follows the master's TIME (epoch and time zone; it ignores epoch 0) and
  runs no time server of its own while paired.
- **Wi-Fi power save** is held off through `net_wifi_hold_low_latency(owner, on)`, one hold per
  reason (link keypads, remote viewing), on a master with keypads and on a keypad.

## Updates through the link

The master decides when its keypads update and to which version. That is always the version it runs,
and only after its own update has passed its self-test (OTA-005). A keypad that reaches the update
server downloads its own board's image itself, through its release run (`panel_ota`, OTA-018,
OTA-026). Only a keypad that can't reach the server gets the image from the master over the link
(the relay). Both paths end in the keypad's own writer and checks. The keypad restarts only at the
master's INSTALL, and only when its own gate passes (decided 2026-10-16).

```
master confirmed on V, every partition quiet (proto_fw_install_allowed)
  each keypad behind V:  FW_ORDER PROBE{V, server, channel}  ->  FW_STATE{probe: offered, why}
  the master picks:      FETCH | RELAY | BLOCKED(why)
    FETCH, all at once:     the keypad's release run: check with want=V, download, verify -> READY
    RELAY, one at a time:   FW_REQ{id8, offset} / FW_PAGE into the keypad's writer, verify -> READY
  installs, one at a time, lowest device number first:
    FW_ORDER INSTALL{V} -> the keypad's gate -> restart -> self-test
    FW_INFO{version V}, FW_STATE{probation} ... probation over -> UPDATED -> the next keypad
```

### When a rollout starts and stops
- **Start.** A rollout starts in one of two ways:
  - an update that a person asked for on the master is confirmed (that Install covers its keypads);
  - a person chooses Update keypads (Software update, the Panel Manager, or `plink fw update`).

  Nothing else starts one (OTA-024). A keypad found behind later (newly paired, or back on line)
  shows "Update keypads" until automatic updates are decided (BL-190).
- **Persistence.** The target version is kept in NVS `link/fwgo` until the rollout ends, so a master
  restart carries on.
- **Who gets orders.** Orders go only to a keypad whose session has `PLINK_CAP_FW`, and only while
  all of these hold:
  - the master runs no update of its own;
  - the master is not on its self-test;
  - `proto_fw_install_allowed()` passes on the master's snapshot.

  When a master update starts, the master sends CANCEL to every keypad that isn't installing yet.
- **Never older.** A keypad on V or newer is left alone. The master never orders an older version
  (OTA-014).

### Choosing the path, per keypad

| The keypad's PROBE says | The master's image for it | Path |
|---|---|---|
| the server offers exactly V | - | FETCH |
| the server refuses this keypad (unknown device, held back, stepping stone; any other 10-12 or 15-19) | - | BLOCKED: never relayed past the server's rules |
| the server offers another version, or nothing newer (13, 14) | its own slot, or a copy | RELAY |
| not reached (1-9) | its own slot | RELAY |
| not reached | a copy, when the master reaches its server for the keypad's facts | RELAY |
| not reached | none | BLOCKED (NO_IMAGE): "Connect keypad n to the internet, or update it with a cable" |

- **Own slot.** The master's running slot serves a keypad when V is the version the master runs and
  all of these match:
  - the same device;
  - a chip revision within the running image's range;
  - a project that `ota_profile_accepts()` takes.
- **Copy.** Any other keypad needs a copy. The master asks its server with the keypad's facts. An
  image refusal blocks, as above.
- **Fall-back.** A FETCH that fails for a reach or transfer reason (1-9 or 20-29) falls back to
  RELAY once per rollout. A refusal of the image (30-49) never does: the same signed image would
  fail the same way.

### The keypad

**Policy:**
- While paired with a master whose session has `PLINK_CAP_FW`, a keypad installs only at its
  master's order:
  - it runs no scheduled check of its own (`panel_ota_set_checks(false)`); PROBE is its check;
  - its Software update says "Updates come from <master>" and offers no Install.
- Its own Install stays in three cases: LINK-025 maintenance, the needs-update state, and a session
  without the bit. The console is not held back.
- Stop stays (OTA-027). The installer or the system master can stop a run at the keypad (their code
  is checked by the master). The master then waits for Update keypads.

**Orders** (`plink_keypad.c`, taken like SOUND with `plink_keypad_take_fw_order`):
- An order id seen before gets FW_STATE again; nothing runs twice. Every order is answered with
  FW_STATE at once.
- The op and kind must be known; otherwise REFUSED with UNKNOWN_OP or UNKNOWN_KIND.
- RELAY needs a size no larger than the slot, and a SHA-256.
- A RELAY with the run's `id8` continues the run. Another `id8` ends it and starts over.
- A FETCH or RELAY ends any run that hasn't started its restart.

**PROBE.** One check (`POST /api/v1/check`) with the keypad's own facts and `want` = V, at the
server and channel the order names. They are used for this check only. The answer is kept as any
check's answer is.

**FETCH.** The release run (`do_install`, `panel_ota.c:2574-2716`), with the order's server, channel
and version for this run only:
1. A check with `want` = V. The offered version must be V (otherwise OTHER_VERSION).
2. The download into the other slot, then `panel_ota_end`.

It downloads as any panel does (OTA-003). The master orders a fetch only while the system is quiet.

**RELAY.** A run of a new kind, `PANEL_OTA_KIND_LINK`, through the writer
(`panel_ota_begin/write/end`, as `ota recv` does):
- **Requests.** The core asks for bytes from the first one it lacks: FW_REQ {id8, offset, page,
  count}.
  - The page and count are no larger than the order's, and the window is at most 16 KB.
  - Pages ahead of the first missing byte wait in the window buffer; nothing is written out of
    order.
  - A request is sent again after 300 ms, doubling to 2 s, for the bytes still missing.
- **Pages.** A FW_PAGE is taken only when its `id8` and total match the run and it lies inside the
  range asked for. Everything else is dropped.
- **Writing.** The link task (internal stack) writes the in-order bytes with `panel_ota_write`:
  - at most 4 KB a turn, through a 4 KB internal buffer;
  - the next request goes no sooner than the last write took, so flash is busy at most half the
    time;
  - and no sooner than the order's gap.
- **Pause.** The run pauses (FW_STATE flag PAUSED) with the writer kept open while the session has
  no STATUS under 15 s old, or while the gate below says no.
- **Failure.** It fails after 30 min paused (TIMEOUT), or after 60 s without a new byte while not
  paused (STALLED).
- **"The link is up"** means a live session with a STATUS under 15 s old, not ONLINE. A keypad that
  can't read a newer master's configuration never gets ONLINE, and it is the one that most needs the
  update.

**Checks, in this order, on both paths:**
1. Only the paired master's sealed frames are taken.
2. The writer holds the first 288 bytes until the header passes: magic, chip, chip revision range,
   project (`ota_profile_accepts`), and newer than the running version. `allow_older` is never set.
   A wrong image is refused before any flash is written.
3. The length and SHA-256 must match the order (relay) or the server's answer (fetch).
4. `esp_ota_end` checks the whole image and its signature, with the keypad's own keys: the running
   image's key, or the one its installer accepted (OTA-020).
5. The image's version must be V; otherwise WRONG_VERSION, and the image is discarded.

Nothing the master sends replaces a check. The server and channel in an order serve that run only.

**READY.** `panel_ota_apply` runs, and then the keypad's gate holds the restart ("Waiting for the
alarm panel") until an INSTALL names that version. A READY image is discarded on any of these:
- CANCEL;
- an order for another image;
- removal;
- 24 h without INSTALL.

**The gate** (`plink_service_update_idle`, `plink_keypad_svc.c:995`) is asked twice before the
restart (OTA-004, OTA-028). It passes only when all of these hold:
- a live session with a STATUS under 15 s old (ONLINE, or needs-update), as today;
- the STATUS flags UPDATING and HOLD are clear;
- `proto_fw_install_allowed()` passes on the rebuilt snapshot, as today;
- no panic is still being resent (`plink_keypad_panic_pending`), on every branch, maintenance
  included;
- no fire or CO tone is kept (`K.cont`), on every branch;
- for an image the master ordered, the master's INSTALL for it has arrived.

The panic and kept-tone checks are fixes the keypad needs with or without these updates.

**After the restart.** The self-test is UI, touch and LINK, as today. FW_INFO carries the new
version. FW_STATE reports probation, then the last result: updated, or rolled back with the health
bits that failed.

### The master
- **Planner.** `plink_fw_plan(world, now, steps)` is a pure function in a new
  `components/core/panel_link/src/plink_fw.c`, run from the master service's poll. It sees:
  - the master's own update state and its running image;
  - whether the master reaches its server (a check answered in the last 24 h);
  - each keypad's FW_INFO and FW_STATE.
- **Per-keypad states:** UNKNOWN, CURRENT, BEHIND, PROBING, FETCH, RELAY_WAIT, RELAY, KEY_WAIT,
  READY, INSTALLING, CHECKING, UPDATED, FAILED(why), BLOCKED(why), STOPPED.
- **Resending orders.** An order is resent after 300 ms, doubling to 2 s, until a FW_STATE echoes
  its id. After 30 s the keypad counts as away, and the rollout waits for it for up to 24 h.
- **Concurrency.**
  - PROBE and FETCH go to every keypad at once.
  - One RELAY runs at a time.
  - One INSTALL at a time: the READY keypad with the lowest device number goes next, once the
    previous one is UPDATED.
- **Canary.** The first keypad is the canary. A rollback, or no return within 10 min, stops the
  rollout for keypads of that device and chip revision until Update keypads.
- **Retries.** A relay that fails in the transfer class is tried again after 2 min, at most 3 times
  per keypad in 24 h. The keypad refuses a fourth with BUDGET.
- **While a keypad installs or self-tests:**
  - its supervision trouble isn't raised, for up to 10 min (`supervise`, `plink_master.c:320`);
  - the master puts off its own restart (`panel_ota_set_restart_hold`, asked in `restart_allowed`,
    `panel_ota.c:351`: "A keypad is checking its update"). Without the hold, a master restart
    silences the keypad's LINK beat for more than 15 s and the keypad rolls back.
- **Serving FW_REQ.** Stateless, like `on_cfg_req` (`plink_master.c:875`).
  - Only for the keypad holding the relay, with that `id8`, while the master is quiet.
  - FW_REQ has its own bucket; it never uses the CFG/ZONES/EV bucket (`REQ_BURST`, `:27`).
  - A byte budget: 256 KB/s on UDP, 16 KB/s on ESP-NOW. At most 16 pages a turn.
  - Each page is min(the request's page, the order's page, the frame's room − 18) bytes, read
    through a port, `fw_read(ctx, id8, offset, out, len)`.
  - Anything else is dropped; the keypad's retry comes back.
- **Image sources.** No partition change; `fw_cache` is not used.
  - **Own slot.** `panel_ota_running_image()` works out, once after confirmation, the slot's exact
    size (the header walk of `cp_ota.c:53-97`, made for the P4), its SHA-256, version, chip range
    and key. `fw_read` maps 256 KB windows of the running partition (`esp_partition_mmap`). No flash
    is written.
  - **A copy.** `panel_ota_fetch_for(facts, V)` runs on the update task: a check with the keypad's
    facts and `want`, then `panel_ota_download` (`panel_ota.c:2483`) into PSRAM, at most 4,010,803 B
    (85 % of the 4,718,592 B slot).
    - One copy at a time.
    - Freed when no keypad needs it, after 30 min unused, or when the master's own run starts.
    - A foreign image never goes into the master's own spare slot, because
      `esp_ota_set_boot_partition` would boot it.

### Messages
- **Placement.** All five are sealed and appended after BYE in `plink_frame.h`. They are in
  `plink_type_known`, with their directions in `plink_type_from_master`.
- **When sent.** Only in a session whose common caps include `PLINK_CAP_FW` (0x0001), a bit a panel
  offers at run time (LINK-043). No panel offers it until the keypad's side is built.
- **Size.** Every payload except FW_PAGE is made of sections and fits 225 B (`PLINK_PAYLOAD_SMALL`,
  a 250-byte frame) at its longest. `OWN_PAYLOAD_MAX` (`plink_keypad.c:49`) is 225.
- **Sections.** FW_INFO and FW_REQ are one section each. FW_ORDER and FW_STATE always carry their
  three sections: a decoder refuses a payload without one, and reads the fields a shorter section
  lacks as 0.
- **Refused by both sides:** an order of 0, and a FW_PAGE outside its image (offset + len > total).

| Type | Name | Dir | Payload (v1) | Largest |
|---|---|---|---|---|
| 0x2D | `FW_INFO` | K->M | one section: can u8 (bit 0 fetch, 1 relay, 2 key accept, 3 configuration unreadable), device (≤ 31), board_rev (≤ 15), chip_rev (≤ 3), chip_rev_full u16, project (≤ 32), version (≤ 32), slot u32, own_key8[8], accepted_key8[8] (zeros: none), cp_version (≤ 23) | 166 B |
| 0x2E | `FW_ORDER` | M->K | order section: order u32 (not 0: both sides refuse it), op u8, kind u8, version (≤ 32), size u32, sha256[32], key[32] (zeros: not known); fetch section: server (≤ 95), channel (≤ 7); relay section: page u16, count u8, gap_ms u16 | 219 B |
| 0x2F | `FW_STATE` | K->M | run section: order u32, op u8, state u8, flags u8, why u8, bytes u32, size u32, id8[8], target (≤ 32); last section: probation u8, last u8, last_version (≤ 32), failed_mask u8; probe section: asked u8, why u8, age_s u32, offered (≤ 32), offered_key8[8], key_trusted u8 | 144 B |
| 0x30 | `FW_REQ` | K->M | one section: id8[8], offset u32, page u16, count u8 | 16 B |
| 0x31 | `FW_PAGE` | M->K | id8[8], total u32, offset u32, len u16, len bytes (fixed, like CFG_PAGE; bytes after the data are ignored) | 18 B + data: 1,157 B on UDP, 207 B in 250 B |

**When each is sent:**
- **FW_INFO:** at the first STATUS of each session, on a change, and on INFO; at most one every 2 s.
- **FW_STATE:** in answer to every order, on each change of state or `why`, at most once a second
  for `bytes`, and every 10 s while a run exists.

**Values** (never renumbered):
- **op:** 1 INFO, 2 PROBE, 3 FETCH, 4 RELAY, 5 INSTALL, 6 CANCEL.
- **kind:** 0 is the P4 application. 1 (C6), 2 (language pack) and 3 (asset pack) are reserved, and
  v1 refuses them.
- **state:** 0 IDLE, 1 PROBING, 2 FETCHING, 3 RECEIVING, 4 KEY_WAIT, 5 VERIFYING, 6 READY, 7
  INSTALLING, 8 FAILED, 9 STOPPED.
- **flags:** bit 0 PAUSED.
- **last:** the `PANEL_OTA_LAST_*` values (0 none, 1 updated, 2 rolled back, 3 stopped, 4 failed).
- **why:** grouped by decade. A reader takes a value it doesn't know by its decade; an unknown value
  in 10-19 blocks.

| Values | Class | Known in v1 |
|---|---|---|
| 0 | OK | |
| 1-9 | not reached | NO_NETWORK 1, NO_LOOKUP 2, NO_CONNECT 3, SERVER_ERROR 4, NOT_A_SERVER 5 |
| 10-19 | the server's answer | UNKNOWN_DEVICE 10, HELD_BACK 11, STEPPING_STONE 12, OTHER_VERSION 13, UP_TO_DATE 14 |
| 20-29 | transfer: retried, or the relay instead | BROKE_OFF 20, TOO_SLOW 21, BAD_SIZE 22, BAD_SHA 23, STALLED 24, TIMEOUT 25, NO_MEMORY 26, FLASH 27 |
| 30-49 | refused: no retry | KEY 30, CHIP 31, PROJECT 32, NOT_NEWER 33, SIGNATURE 34, TOO_BIG 35, UNKNOWN_KIND 36, UNKNOWN_OP 37, BUDGET 38, BAD_ORDER 40, WRONG_VERSION 41 |
| 50-59 | waiting | MASTER_BUSY 50, NOT_QUIET 51, PANIC_PENDING 52, KEPT_TONE 53, NO_STATUS 54, BUSY 55 |
| 60-69 | stopped | STOPPED 60 (a person at the keypad), CANCELLED 61 (the master) |
| 70 and up | unknown class: a later firmware's | |

**STATUS flags.** New bits in the existing flags byte, so no byte is added. Defined in
`plink_msg.h` (`PLINK_SF_*`, frozen in the fixture); not yet set at `plink_view.c:157` from the
master's whole snapshot, nor read at `:312`:
- 0x04 UPDATING: the master runs an update or is on its self-test.
- 0x08 HOLD: `proto_fw_install_allowed()` says no on the master's snapshot.
- 0x10 FIRE / 0x20 CO: fire or CO sounds on any of the master's partitions.

A change of any of them sends a STATUS, as a change does (LINK-006).

**Rules** (v1, frozen once released):
- `id8` is the first 8 bytes of the image's SHA-256. It is never tied to a session, so a relay goes
  on across a new session, the 24 h rekey and a master restart.
- Order ids are a random base at each master start, then counted, as LINK-018's are.
- FW_ORDER is at 219 of 225 B. A later field goes in a new section, sent only where it fits.
- A relay v2 takes new type numbers.

### Per transport (estimates, not measured)

| Transport | FW_PAGE data | Relay of 3.6 MB |
|---|---|---|
| UDP over Wi-Fi | 1,024 B, 16 KB per request | paced by the keypad's flash: about 2 min |
| ESP-NOW, 250 B frames (later) | 192 B | about 13 KB/s, about a fifth of the air at 1 Mbps: about 5 min |
| 433 MHz | never | - |

- **v1 keypads never relay over ESP-NOW.** The factory C6 (2.12.3) has no ESP-NOW, so a keypad gets
  the firmware that brings ESP-NOW over Wi-Fi. Later keypads use these same messages with the page
  size from the order.
- **The ESP-NOW bridge** must send control frames before bulk ones (CP-003).
- **No FW_* frame** goes on a transport whose largest frame is under 250 B.

### What people see
- **Master's Software update:** a Keypads block, one row per keypad: name, version and state.
  - States: "Up to date", "Waiting for its turn", "Downloading 45 %", "Receiving from this panel 45
    %", "Ready", "Waits: the system is armed", "Installing", "Self-test", "Updated", "Accept the new
    key at keypad n", "Not updated: <reason>".
  - Buttons: Update keypads and Stop.
  - It is drawn as a proposal first: the touchscreen per
    `docs/features/07-UI-LVGL-Pro/mockup/README.md`, the Panel Manager per
    `docs/features/31-Browser-Panel-Manager/mockup/README.md`.
  - Bars are stepped and words change at most once a second (UI-020).
  - `ui_link_keypad_t` (`ui_backend.h:1615-1626`) gains `fw_state`, `fw_why`, `fw_path`,
    `fw_percent` and `fw_behind`.
- **Keypad's Software update:** "Updates come from <master>", the run with the existing steps, Stop,
  and the key screen (below).
- **Event log, on the master.** `firmware_update` stages are appended after
  `ALARM_FW_RESTART_PUT_OFF` (`alarm_types.h:636-646`):
  - `keypad_installing` 9, `keypad_updated` 10, `keypad_rolled_back` 11, `keypad_failed` 12 (arg2 =
    why);
  - source {keypad, n};
  - the master's own stages are not reused, because OTA-029 reads the master's last result from
    them.
- **MQTT:** `…/firmware` gains `keypads` [{device, version, state}]. Keys are added only.
- **Console:**
  - master: `plink fw`, `plink fw update [n|all]`, `plink fw cancel [n|all]`, `plink fw path <n>
    auto|fetch|relay` (bench only, RAM only);
  - keypad: `plink fw`.

### A new signing key (LINK-036)
- **When.** The order or the server's answer names the image's key. If that key is neither the
  keypad's own nor its accepted key, the keypad stops before downloading and goes to KEY_WAIT.
- **What it shows.** "The new software is signed with a key this keypad doesn't know yet", with the
  fingerprint (the first 16 hex digits, in groups of four; OTA-020), a hold to accept, and the
  installer's code.
  - The code is checked by the master (CMD op 2).
  - The screen is refused while STATUS says `default_codes` (OTA-021).
- **On accept.** The keypad stores the order's server and that key as its trust record
  (`ota`/`trust`), as a server change does on a master.
- **Without this,** a keypad whose master moved to another key needs its service console (`ota
  server check`, `ota server use`) or a cable.

## Versions and compatibility
The owner (2026-10-16): a newer panel must still work with an older one. Nothing of the link is
released yet, so whatever a keypad needs in order to be updated later goes into v1.

- **The floor, both ways.** A master keeps serving all of the following to a keypad that says
  protocol 1:
  - the v1 header and pairing;
  - HELLO (≤ 128 B, `pending_hello`, `plink_master.h:123`) and WELCOME;
  - STATUS v1 (the system section and partition records);
  - ZONES (the 7-byte record, with `zones_crc` over it);
  - CFG paging;
  - EV_REQ and EVENTS (the 32-byte record);
  - CMD and RESULT, SOUND, HEARTBEAT and TIME;
  - BYE, which always means "removed";
  - FW_* v1.

  So a paired keypad, however old, can always be brought to its master's version. A keypad of any
  later version works with a v1 master at protocol 1. Neither side refuses a peer at protocol 1 or
  above.
- **Additions.**
  - A new type, op, kind, command type or value goes only to a peer whose session has its capability
    bit.
  - For a protocol-1 peer, the master maps a value v1 doesn't know to the nearest v1 value.
  - Whatever a keypad must show, sound or obey stays expressible in v1.
- **Fail-safe keypads** (v1, `plink_view.c:206-219`):
  - An arm state the keypad doesn't know decodes as armed (Away, when the level is unknown too).
  - A sounder value it doesn't know decodes as the alarm.
  - Fire and CO sound from the STATUS FIRE and CO bits, so a later master's partitions beyond the
    keypad's 4 are heard (`ALL_PARTS`, `plink_view.c:27`).
  - Today the raw value goes through, and `compute_mode` (`ui_app.c:1495`) shows an unknown state as
    Ready.
- **Unknown command type.** The master answers it UNAVAILABLE (LINK-017), not BAD_ARG as today
  (`plink_master.c:729-731`).
- **Fields.**
  - Fields are appended only.
  - Absence is known from a section's length, never from a 0.
  - An appended field refines its v1 meaning and never changes it.
  - A configuration field is never widened in place; a wider one gets a new id.
- **Stored state.**
  - A link record never raises compat and stays at most 1024 B (`PLINK_STORE_MAX`).
  - New per-keypad facts go in new NVS keys.
- **The master's self-test proves its keypads** (LINK-041).

**What a v1 keypad must have** (it can't be added to a keypad later):
- `PLINK_CAP_FW` and the five messages above;
- PROBE and FETCH through its release run, with `want`;
- the in-order pull into the writer;
- the gate with the new STATUS flags, the panic and kept-tone checks, and the INSTALL hold;
- the fail-safe decoding, with FIRE and CO;
- the key screen.

The master's side (planner, sources, buckets, screens) can be finished or improved later without
touching keypads. It still ships in v1, because the same firmware runs both roles.

## Smart Home and Home Assistant on keypads
The owner (2026-10-16): "we need to make sure that the Smart Home controls work on the Slave as
they do on the Master. Home Assistant should also be able to control the settings of the slave
panel as well." Designed 2026-10-16 (BL-216). Built and host-tested: its wire (the codecs, the
run-time capabilities, `proto_prefs` and the frozen fixture) and its link core (`plink_home`, the
master's and the keypad's managers; "Where the code goes"). Not built yet: the services, the
keypad's page and Home Assistant. No panel offers HOME or PREFS yet, so nothing new is sent.

The owner decided the 13 questions this design needs on 2026-10-16 ("Approve P1, recommendations
OK"): each as recommended, listed in Decisions. The keypad's Smart Home screens are proposal P1,
approved that day, now in the mockup as `kp-devices`, `kp-device-unlock`, `kp-devices-lost` and
`kp-devices-unconfirmed` (`docs/features/07-UI-LVGL-Pro/mockup/screens/panel-link.js`).

### What people get

**On a keypad:**
- The rail shows Smart Home under the master's rule: at least one listed device that isn't from
  Z-Wave (`ui_devices.c:2478-2490`).
- The page is the master's page, unchanged: the same tiles, sheets, code pad, words, pending and
  failed states, "Couldn't confirm", the note, and Arrange.
- The devices and their states are the master's. Each action is judged again on the master, by
  the same function the master's own screen uses.
- Arrange is the keypad's own (decision 2, decided 2026-10-16).
- While the link is down, or no device data has come for 30 s, every tile reads "Offline since"
  with the time of the last device data ("Offline since 9:12 PM"). The note reads "Your devices
  can't be reached right now". A tap sends nothing.

**In Home Assistant:**
- The master's own Screen and sound settings become config entities.
- Each keypad becomes its own device under the panel, with:
  - the same settings entities;
  - connectivity and signal;
  - its firmware version.
- Every write goes Home Assistant -> the master's `ha_link` -> the panel link -> the keypad. The
  keypad checks it, applies it on its screen's task and answers.

**What does not change:**
- Home Assistant still talks only to the master. A keypad runs no `ha_link`, no Home Assistant
  mDNS service, no MQTT and no device source (LINK-020).
- HAI-009's allow-list still applies: a keypad's tap becomes the master's `call`.
- `api` stays 1.
- No stored format changes. HELLO does not change.

### Capabilities

Built with BL-048's wire, so v1 ships with it:
- `PLINK_CAPS` is gone. `PLINK_CAPS_KNOWN` (0x0007) = FW | HOME | PREFS: the bits this firmware's
  codecs know.
- What a panel offers is an argument of `plink_master_init` (after `panel_id`) and
  `plink_keypad_init` (after `cap`), passed down to `plink_pm_init`, `plink_pk_init`,
  `plink_hs_hello` and `plink_hs_answer`. Each masks it with `PLINK_CAPS_KNOWN`.
- Each session keeps its own offer AND the peer's (`plink_session_t.caps`), so a bit either side
  doesn't know is never shared. `plink_master_peers` and `plink_keypad_info` give a live session's
  caps.
- Every panel offers 0 for now (both services and `host/link_sim`): a bit is offered only with the
  code that uses it.

Who offers each bit (designed):

| Bit | Name | Offered by the master | Offered by a keypad |
|---|---|---|---|
| 0x0002 | `PLINK_CAP_HOME` | Built with `CONFIG_ALARM_HOME_CONTROL`. The list is empty while `device_service` isn't running, so the keypad's rail stays hidden, as on such a master. | Built with `CONFIG_ALARM_HOME_CONTROL` and its mirror allocated in PSRAM. Never on the 4" board. |
| 0x0004 | `PLINK_CAP_PREFS` | Every build with the UI | Every build with the UI |

Next free: bit 0x0008 and type 0x3B.

### Messages

| Type | Name | Dir | Bit | Payload (v1) | Largest |
|---|---|---|---|---|---|
| 0x32 | `DEV_LIST_REQ` | K->M | HOME | list_crc u32, offset u32, max u16 (CFG_REQ's layout and codec) | 10 B |
| 0x33 | `DEV_LIST_PAGE` | M->K | HOME | list_crc u32, total u32, offset u32, len u16, len bytes (CFG_PAGE's layout and codec) | 14 B + data: 1,161 B on UDP, 211 B in a 250 B frame |
| 0x34 | `DEV_STATE` | M->K | HOME | Head section (list_crc u32, states_crc u32, count u8), then count × (index u8, record section) | 225 B: 4 records at their longest, about 8 typical. On UDP, 25 and about 48. |
| 0x35 | `DEV_STATE_REQ` | K->M | HOME | list_crc u32, first u8, count u8 (0 = all) | 6 B |
| 0x36 | `DEV_ACT` | K->M | HOME | req u32; act section (action u8, value i32, mode u8, flags u8 = 0); ref (len u8 + 95 B, zero-padded); code (len u8 + `ALARM_CODE_MAX_LEN` 10 B, zero-padded). Both codecs refuse a ref longer than 95 B or one that text cleaning would change, never cut (it could name another device), and a code that isn't digits only | 119 B, always (144 B frame) |
| 0x37 | `DEV_ACK` | M->K | HOME | req u32, result u8, why u8; optional section (list_crc u32, index u8, record section). An empty section means no record, and a later section may follow it; a non-empty one must hold list_crc, the index and a whole record section, or the payload is refused | 56 B |
| 0x38 | `PREFS` | K->M | PREFS | Always four sections: answer section (req u32, 0 = unsolicited); info section (panel id ≤ 6, voice u8, accepts u32); refusals section (count u8, count × (id u8, why u8)); fields section ("Settings fields") | 128 B |
| 0x39 | `PREFS_REQ` | M->K | PREFS | have_crc u32 | 4 B |
| 0x3A | `PREFS_SET` | M->K | PREFS | req u32 (not 0: both sides refuse it), origin u8; fields section (the fields to change, "Settings fields") | 75 B |
| 0x2A | `HEARTBEAT` (+) | K->M | PREFS | Appended section: prefs_crc u32. It counts only when it holds prefs_crc whole; an empty or shorter one carries no CRC and never drops the heartbeat | +5 B, 45 B in all |

**Record section** (43 B at most, 45 B with its index and length):

| Field | Size |
|---|---|
| flags | u8: bit 0 online, 1 pending, 2 failed, 3 unsure (`hc_outcome_unknown`) |
| action | u8 (`dev_action_t`) |
| vflags, level, modes, hvac_action | u8 each |
| setpoint, temp | i32 each |
| since | u32, Unix seconds |
| caps | u8 |
| state word | len u8 + up to 23 bytes |

These are the registry's `dev_values_t` (`device_source.h:105-115`), whose values are already
"append, never renumber" (`:8-10`). `ui_dev_t` and the UI enums never go on the wire.

**Catalog record section:** kind u8, source u8, ref (≤ 95), name (≤ 47), area (≤ 47). Later fields
are appended inside the section, such as a cover's class (BL-171). A ref is refused as DEV_ACT's
is, never cut: the encoder doesn't write it, and the decoder refuses that record but gives its
length, so a list steps over the device. A name or an area is cut on a whole character.

**Frozen values:**

| Value | Meaning |
|---|---|
| `plink_dev_result_t` | 0 OK, 1 NEED_CODE, 2 CODE_REFUSED, 3 BUSY, 4 UNREACHABLE, 5 REFUSED, 6 UNAVAILABLE (this master serves no devices). Numbers 0-5 equal `ui_dev_result_t`, asserted in `test_plink_msg.c`. An unknown value reads as REFUSED. |
| DEV_ACK `why` | The `alarm_result_t` when the code was refused |
| `mode` | A `DEV_MODE_*` bit: OFF 0x01, HEAT 0x02, COOL 0x04, AUTO 0x08, equal to `device_source.h`'s, asserted in `test_plink_msg.c`. Any other value is REFUSED. |
| Settings `why` | 1 NOT_ALLOWED, 2 RANGE, 3 IN_USE, 4 HELD, 5 VOICE_NOT_READY, 6 BAD, 7 BUSY: `proto_prefs.h`'s numbers. 6 BAD means a TLV of the wrong length or an id named twice; it has no word of its own in `settings_result`'s `refused` and reads as `not_allowed` there. An unknown value reads as NOT_ALLOWED. |
| `voice` | 0 not known, 1 ready, 2 loading, 3 none |
| `origin` | 1 Home Assistant, 2 the master's console. An unknown value means "elsewhere". |

Every v1 number of the link's messages (capability and flag bits, and the values above and in
"Updates through the link") is frozen in `test/plink_wire_fixture_v1.h` (`PLINK_WIRE_V1_NUMBERS`,
125 values), and `test_wire_numbers_v1` pins `plink_msg.h`'s to them. The fixture also freezes a
payload of every v1 type that carries one, which `test_wire_fixture_v1` reads as its frozen values
and encodes again to the same bytes.

### Rules
v1, frozen once released, and in `test/plink_wire_fixture_v1.h`:
- All nine types are sealed and appended after FW_PAGE 0x31 in `plink_frame.h`, and listed in
  `plink_type_known`. Types 0x33, 0x34, 0x37, 0x39 and 0x3A are also listed in
  `plink_type_from_master`, so `plink_hdr_read` enforces the direction.
- They go only in a session whose common caps hold their bit. A receiver drops them from a
  session without it.
- They go only while the peer's transport carries frames of 250 B or more. This is checked at
  each send, so a failover is safe. They never go over 433 MHz.
- Every payload except DEV_LIST_PAGE fits in 225 B. Fields sit in sections and are appended only
  (LINK-040). Strings are read with `plink_r_str`.
- `OWN_PAYLOAD_MAX` (`plink_keypad.c:49`) is 225 (`PLINK_PAYLOAD_SMALL`), so PREFS's 128 B fit.
- The HEARTBEAT section is sent only in a session with PREFS. A master reads it only when bytes
  remain: `plink_r_section` on a reader with nothing left fails the whole decode, because
  `plink_r_u8` on a reader that isn't padded clears `ok`, so it is read only when
  `plink_r_left() > 0`.
- Request ids start from a random base at each start, then count up (LINK-018).

### The device list
- **Contents.** The master keeps one catalog blob: compat u8 (= 1), count u8, then one catalog
  record per device in registry order. Z-Wave rows are included; both screens drop them until
  BL-169 (`ui_devices.c:2478-2485`).
- **Limit.** At most 64 rows: `PLINK_DEV_MAX`, the v1 wire limit. More would need a new bit.
  `HC_MAX_DEVICES` and `UI_DEVL_MAX` are 64 today (`home_control.h:25`, `ui_dev_model.h:39`).
- **Size.** At most 12,482 B, so the buffer `PLINK_DEV_BLOB_MAX` is 12,544 B. A typical list is
  under 3 KB.
- **list_crc** is the blob's CRC-32, never 0. It also tells lists apart across master restarts.
- **Pull.** As the configuration's ("Messages", "Configuration"): wait 0-500 ms, keep 2 requests
  outstanding, retry after 300 ms doubling to 2 s. The keypad's configuration pull is factored
  into a `plink_pull_t` (`plink_keypad.h`), used twice.
- **Lenient decode.** Unknown fields are skipped.
- **A blob that fails.** It is remembered by its CRC and not pulled again. Smart Home keeps the
  old list, which reads "Offline since" 30 s after its last data ("Device state"). The keypad
  doesn't say it needs an update, because devices aren't alarm-critical.
- **Install.** The old list stays on screen until a new one installs. At install, each new row
  keeps the last record of the old row with the same ref until its own record arrives, so a
  rename never blinks. Then DEV_STATE_REQ asks for all.
- **Storage.** RAM only. After a keypad restart, Smart Home appears once the list is in.

### Device state

**On the master:**
- **Building.** The `plink_dev` task copies the registry (`device_service_list`) and encodes the
  records.
  - It builds when `device_service_seq()` moves, at most every 250 ms, and at once after a
    device action.
  - Every 2 s it builds and compares, in case a change was missed.
  - A CRC of each encoded record gives a `changed` mask.
  - It runs only while a live peer has HOME.
- **Pushes.** Changed records go at most one round per 500 ms per keypad (1 s on frames under
  1,200 B), at most 2 frames per keypad per turn.
- **Full set.** It goes at each new session (after the promotion's STATUS and TIME,
  `plink_master.c:1169-1176`) and on DEV_STATE_REQ.
- **A list change.** That peer's `changed` mask is cleared, because records under the new list
  are useless until the keypad installs it. A DEV_STATE naming the new list goes at once and
  again at +250 ms and +1 s, as a STATUS change does (LINK-006), so one lost frame costs no 10 s.
- **Summary.** A DEV_STATE with count 0 goes every 10 s (a 35 B frame). It also proves the device
  data is fresh.
- **states_crc** is the CRC-32 over the encoded records (index plus section) in index order.

**On the keypad:**
- A record is applied only when the head's list_crc is the installed list's, and only from a
  frame of the session newer than the last applied for that index. This is LINK-026's rule: the
  mirror's `frame_seq[64]` is reset by `items_reset` and at install.
- It keeps each record's raw bytes (≤ 2.9 KB), so its own CRC is exact even for fields a newer
  master appended.
- **Catch-up.** When the CRCs differ, it sends DEV_STATE_REQ after 250 ms, then at least 1 s
  apart, doubling to 30 s (the zones pattern; `PLINK_DEV_REQ_WAIT_MS`, `_GAP_MS`, `_GAP_MAX_MS`).
- **Freshness.** Only device data sets `dev_heard_ms`: a DEV_STATE (a summary counts) naming the
  installed list, or any while none is installed, or a DEV_ACK whose record is under that list.
  Data under a list the keypad can't install (a newer compat, one too big, a CRC that lies) never
  keeps the old rows fresh: they read offline 30 s after their own last data (LINK-046).

### Device actions

**On the keypad** (`kp_dev_act`, on the LVGL task; `ui_devices.c:657`, `:673`):
1. Link not ONLINE, data stale, or the row offline: UNREACHABLE at once, with no frame sent.
2. Unknown ref, a Z-Wave row, or `!hc_action_valid`: REFUSED.
3. `hc_needs_code` with no code: NEED_CODE, decided locally so the pad opens with no round trip.
   The master judges again.
4. ACT_SET_MODE: `UI_DEV_ST_OFF/HEAT/COOL/AUTO` becomes the `DEV_MODE_*` bit.
5. Hand it to the link task (`ksvc_dev_act`: its own box, mutex and semaphore, never
   `s_cmd_mutex`, `plink_keypad_svc.c:152`). Wait at most 1 s, or 3 s with a code.
6. Map the answer. For CODE_REFUSED, `*code_result = why`, so the pad says why, lockout included.
7. No answer in time: return UI_DEV_OK and mark the device PENDING in an overlay. The core keeps
   resending every 500 ms. The overlay changes as follows:
   - A DEV_ACK OK clears it (the ACK's record is the truth).
   - A late refusal makes it FAILED, definite.
   - The 3 s give-up makes it FAILED and unsure ("Not confirmed", HCTL-012).
   - Any later record for that index, or the next action on it, clears it.

   Nothing is sent after the give-up. Link resends of the same request are not replays: the
   master's cache runs each request at most once (HCTL-012).

The keypad core has 2 device slots (`PLINK_DEV_ACTS_MAX`), separate from `cmds[PLINK_CMDS_MAX]`
(`plink_keypad.h:60`).

**On the master:**
1. Decode. The digest is SHA-256 over action, value, mode, flags and the padded ref, never the
   code (`plink_dev_act_digest`).
2. Look in the answer cache (`PLINK_DEV_RESULTS_PER_KEYPAD` 8, kept up to 90 s after the last
   copy):
   - same req and digest: send the cached result, or wait while it is pending;
   - same req with another digest: a new action, once the first is answered;
   - a new req takes a free entry, else the one whose last copy is oldest, once it is 3 s old (a
     keypad never resends after 3 s), answered and its DEV_ACK sent. So the 500 ms bucket, not
     the cache, sets the pace (decided 2026-10-16 in the build: with 4, fast taps were dropped).
3. Check `act_running` (one in flight) and its own bucket.
4. Call `port.dev_run(ticket, device, home, &act)`. The code copy is wiped as in `on_cmd`
   (`plink_master.c:824-833`).
5. The answer comes back through `plink_master_dev_answer`.

DEV_ACK's record section is filled from the current records each time it is sent, never from the
cache. The `plink_dev` task posts RES_DEVICES (a build right after the act) before RES_DEV_ACK on
the same queue, so the record already shows the action pending.

**One judge for both screens:** `device_service_person_act(src, partition, ref, action, value,
mode_bit, code, &why)`.
- Its body is today's `dev_act` (`ui_port_esp.c:1258-1310`), moved:
  1. refuse an unknown ref, `DSRC_ZWAVE` (until BL-169) or an invalid action;
  2. check `hc_needs_code` against the live kind;
  3. call `alarm_service_authenticate_as(src, code, partition, …)`;
  4. refuse ARM_ONLY and GUEST codes (duress acts, as decided in BL-045);
  5. call `device_service_act` with `hc_mode_word(mode_bit)`.
- **Master's screen:** `{KEYPAD, ALARM_KEYPAD_LOCAL}` with partition 0, as today.
- **Link keypad n:** `{KEYPAD, n}`, with n from the session and never from the frame, and
  partition = its home (decision 1, decided 2026-10-16). A wrong code counts toward that
  keypad's own lockout (USR-005). After each job the task refreshes `s_lockout[n]` with
  `run_job`'s helper (`plink_master_svc.c:316-325`), so STATUS `lockout_left_ms` shows it.
- HCTL-007's event goes here when it is built, with `src`.

### Settings fields

These are a contract, never renumbered:
`components/core/alarm_proto/include/alarm_proto/proto_prefs.h`, under components/core/AGENTS.md.

The TLV is: id u8, len u8, value little-endian. Ranges follow `panel_prefs.c:66-126` and
`web_config.c:112-129`.

| Id | `panel_prefs_t` field | Wire | Range | JSON key | Home Assistant | Look |
|---|---|---|---|---|---|---|
| 1 | brightness | u8 | 10..100 | `brightness` | number % | yes |
| 2 | saver_brightness | u8 | 0..50 | `saver_level` | number % | yes |
| 3 | saver_after_ms | u32 | 0, or 10 s..24 h | `saver_after` | select (the screen's 9 choices) | |
| 4 | off_after_ms | u32 | as 3 | `off_after` | select | |
| 5 | night_on | u8 | 0/1 | `night` | switch | yes |
| 6 | night_start | u16 | 0..1439 | `night_start` | time | yes |
| 7 | night_end | u16 | 0..1439 | `night_end` | time | yes |
| 8 | night_brightness | u8 | 10..100 | `night_level` | number % | yes |
| 9 | night_off_after_ms | u32 | as 3 | `night_off` | select | |
| 10 | clock_mode | u8 | 0..2 | `clock` | select: region, 12 h, 24 h | |
| 11 | appearance | u8 | 0..2 | `appearance` | select: dark, light, automatic | yes |
| 12 | high_contrast | u8 | 0/1 | `contrast` | switch | yes |
| 13 | volume (Beeps) | u8 | 0..100 | `beeps` | number % | |
| 14 | chime_volume | u8 | 0..100 | `chime_vol` | number % | |
| 15 | voice_volume | u8 | 0..100 | `voice_vol` | number % | |
| 16 | key_click | u8 | 0/1 | `key_click` | switch | |
| 17 | voice_enabled | u8 | 0/1; on only while the voice is ready | `voice` | switch | |
| 18 | voice_status | u8 | 0/1 | `voice_stat` | switch | |
| 19 | voice_chime | u8 | 0/1 | `voice_chime` | switch | |

- **The fields section.** Its body is the count u8, exactly count well-formed TLVs (each whole
  within the body), then any bytes a later version appends, which are kept. An empty body holds no
  fields. Both codecs refuse a TLV that runs past the body. `plink_msg` carries the body as one
  span: `proto_prefs_encode` writes it, in id order, and `proto_prefs_decode` reads it.
- **Sizes.** All 19 fields take 68 B, so the fields section is 70 B.
- **Unknown and wrong-size ids.** An unknown id is skipped by its length. A known id with the
  wrong length, or named twice, is refused (BAD) for that field alone, and no copy is taken.
- **`accepts`** has bit id-1 set for each field the keypad takes.
- **prefs_crc** is the CRC-32 of the fields section's whole body (count, TLVs and appended bytes),
  without the section's length byte, as the keypad encodes it (`proto_prefs_crc`). The master
  takes it over the body it received, so it holds across versions.
- **Never a settings field:** codes, role, pairing, home partition, name, supervision, Wi-Fi and
  IP, time and time zone, language and region (v1, decision 5, decided 2026-10-16), updates,
  factory reset, the setup, the Panel Manager, developer access.

### Settings over the link

**From the keypad:**
- It sends PREFS at a session's first STATUS, after every save (at most 1 a second), and on
  PREFS_REQ.
- It reads its values from `panel_prefs_current()`, a RAM copy, so the link task never reads NVS
  for this.
- HEARTBEAT carries prefs_crc.

**On the master:**
- It keeps the newest report per keypad in RAM, taken only from a newer frame. This includes the
  panel id, which must be 6 lowercase hex characters or it is not kept.
- While HEARTBEAT's CRC differs from the held report's, it sends PREFS_REQ, at most every 2 s.
- If two keypads report the same id, the higher device number is published with `id` null, and a
  line is logged.

**A write:** `plink_master_prefs_set(m, now, device, req, origin, fields, len, &why)`.
- It refuses at once with:
  - OFFLINE: no live session, or not heard for 30 s;
  - UNSUPPORTED: no PREFS bit, or a transport under 250 B;
  - BUSY: one write already in flight for that keypad.
- Otherwise it sends PREFS_SET, resends it every 1 s with the same req, and gives up at 6 s
  (TIMEOUT).
- The answer is the PREFS that carries the req: refusals per field, and the values now stored.
- However it ends, `port.prefs_done` runs once, after the master's state is settled (the
  answer's report already kept), and may start the next queued write.
- The keypad keeps its answer for 30 s. A repeat is answered from it, or ignored while still
  pending, so a change is never applied twice.

**The keypad's path to its screen** (the link task never takes `board_ui_lock` and never writes
NVS behind the screen):
1. The service decodes the TLVs with `proto_prefs`. RANGE, BAD and NOT_ALLOWED are refused here.
2. It submits the patch with `panel_prefs_remote_submit`, a single-slot mailbox in
   `platform_esp`.
3. The screen's task takes it and applies it (UI-046).
4. Its `done` callback, on the screen's task, only copies the answer into an atomic box.
5. The link task sends `plink_keypad_prefs_answer` with fresh values.

A patch not taken within 3 s is withdrawn and answered BUSY.

### Applying a change from elsewhere (UI-046)

`prefs_remote_tick()` in `ui_settings.c` runs from `ui_settings_tick` (`:239`), on every LVGL
loop. It takes a patch through `be->prefs_remote_take`, then:

1. The setup is running, or Settings > Screen and sound (any of its pages) is open, or
   `s_opts_dirty`: refuse all fields, IN_USE.
2. `ui_app_held()` (today's `language_held`, `ui_app.c:3638`, made public) is true during an
   alarm, entry time or exit time: refuse all, HELD.
3. `voice` turning on while the voice isn't ready (`voice_info`): refuse that field,
   VOICE_NOT_READY.
4. Look bits (ids 1, 2, 5-8, 11, 12) within 2 s of the last remote look change: hold the whole
   patch until the 2 s have passed, then check steps 1-3 again.
5. Merge the masked fields into a copy of `S.opt` (`ui_prefs_merge`), then call
   `ui_app_apply_options(&copy)` (`:742`). That runs `ui_options_changed` and `save_options`, so
   the voice settings stay in step (`ui_port_esp.c:509-555`).
   - Language and partition are left unchanged, because the copy is taken from `S.opt`.
   - The save runs on the LVGL task (internal stack).
6. `be->prefs_remote_done(ticket, why[])`.

The master's own settings (Home Assistant's target 0) use the same mailbox and the same rules.
Home Assistant may change settings while a partition is armed; they are held during an alarm,
entry time or exit time (decision 4, decided 2026-10-16).

### Alarm traffic always wins

| Traffic | Limit |
|---|---|
| DEV_ACT per keypad | 1 in flight; burst 4, then 1 per 500 ms; its own cache of 8, an entry reused 3 s after its last copy. Never the CMD bucket, `cmd_running`, the 8 command answers or their bitmasks (`plink_master.h:53`, `plink_master.c:25-26`). |
| DEV_LIST_REQ and DEV_STATE_REQ | Burst 16, then 1 per 100 ms; never `requests` (`plink_master.c:1152-1165`) |
| PREFS received | Burst 4, then 1 per second |
| PREFS_SET | 1 in flight per keypad |
| Sending order | DEV_* and PREFS* frames go only in a turn where that keypad has no STATUS change waiting (`change_ms == 0`) and no RESULT due; at most 2 per keypad and 8 per turn. On the ESP-NOW bridge (CP-003), control frames go first, then DEV_ACT, DEV_ACK and PREFS_SET, then DEV_STATE, PREFS, DEV_LIST_PAGE and FW_PAGE. |
| Work on the master | Device builds and actions run on `plink_dev` (priority 3, below `plink_work`'s 4), never on the alarm worker or its 16 slots (`plink_master_svc.c:54`). The one shared resource is the alarm task: a code check holds it for about 150 ms, as any keypad code does (measured, Tasks). |
| Keypad | 2 device slots and its own mutex; list and state requests pause while a panic is pending |

### Failures

| Case | Keypad screen | Home Assistant |
|---|---|---|
| Link not ONLINE, or no device data for 30 s | Every tile "Offline since …", the existing note; taps send nothing | That keypad's entities unavailable; connectivity off |
| The master's Home Assistant link drops | Both screens show offline since the drop (the registry's own) | Everything unavailable (today) |
| DEV_ACT or DEV_ACK lost | Resent for 3 s; the cache answers copies; then "Not confirmed"; never run twice | - |
| The list changes during an action | The action names the device by ref, never by index | - |
| Master restart | The list follows the master's registry, which is empty until Home Assistant's catalog arrives, as on the master's screen | `keypads` again; `id` null for seconds until each keypad reports |
| Keypad restart | The list is pulled again; Smart Home appears once it is in | Unavailable, then back with the same entities |
| PREFS lost | - | The heartbeat CRC differs, so PREFS_REQ goes; caught up within about 10 s |
| PREFS_SET lost | - | Resent; `timeout` after 6 s; the next push shows the real values |
| A list blob that won't decode | The old list stays and reads "Offline since" 30 s after its last data (data under a list it can't install keeps nothing fresh); no update message | - |
| The list changes while frames are lost | The master names the new list at once and again at +250 ms and +1 s, as a STATUS change (LINK-006), so one lost frame costs no 10 s | - |

### Versions

| Master | Keypad | Result |
|---|---|---|
| New | v1 (FW bit only) | No new message types. Smart Home stays hidden there, as today. In Home Assistant it is listed with `id` null and gets no device. |
| v1 | New | No common bits. Nothing new is sent, not even the HEARTBEAT section. Smart Home is hidden. |
| New | New, built without home control | PREFS only |
| Newer | New | Unknown TLV ids are skipped. An unknown kind, action or hvac value reads as DEV_NONE, ACT_NONE or DEV_HVAC_UNKNOWN. Appended record bytes are kept for the CRC. A catalog with a newer compat is remembered as bad, with no update message. |
| New | Newer | The master sends only ids that both it and `accepts` know. An unknown `why` reads as NOT_ALLOWED; an unknown result reads as REFUSED. |

The link's side of the first two rows is tested on the PC: `test_plink_link.c`
`test_a_keypad_offering_only_fw_gets_nothing_new` runs a keypad offering FW only beside one with
HOME and PREFS, then restarts the master as a v1 master (FW only).

### Where the code goes

**`panel_link` (portable), built and host-tested** (BL-216 S1; no panel offers HOME or PREFS yet):
- **The wire:** run-time caps; the codecs in `plink_msg.h/.c` (DEV_LIST_* reuse
  `plink_enc/dec_cfg_req/_page`), with the device record and catalog record codecs and
  `plink_dev_act_digest`.
- **`plink_home.h/.c`, both sides:** the catalog blob from the registry's rows
  (`plink_home_list_build`, `plink_home_recs_build`), `list_crc` (never 0) and `states_crc`, the
  records by index (`plink_dev_recs_t`), a keypad's lenient walk of a pulled blob
  (`plink_home_list_check`, `_next`, `_find`), and a row's record carried over by ref at an
  install (`plink_home_list_keys`, `plink_home_carry`).
- **The master (`plink_master.h`):** `plink_master_home` takes the list and its records from one
  copy of the registry; `port.dev_run` runs an action off the link task and
  `plink_master_dev_answer` brings its answer back; `plink_master_prefs_set` starts a write and
  `port.prefs_done` ends it; `plink_master_keypad_home` gives a keypad's caps, whether it is online
  and its settings report. Each keypad's state is a `plink_home_peer_t` in its `plink_peer_t`.
- **The keypad (`plink_keypad.h`):** `plink_keypad_home_attach` gives it the mirror
  (`plink_dev_mirror_t`, the caller's memory; HOME is offered only with one); one `plink_pull_t`
  each for the configuration and the list. `plink_keypad_dev_info`, `_dev_row`, `_dev_rec` and
  `_dev_overlay` read the mirror; `plink_keypad_dev_act`, `_dev_act_state` and `_dev_act_release`
  run an action; `plink_keypad_prefs_report`, `plink_keypad_take_prefs_set` and
  `plink_keypad_prefs_answer` carry the settings.

The rest is still to build, except `alarm_proto`'s part:

| Area | Change |
|---|---|
| `alarm_proto` | Built: `proto_prefs.h/.c`, the settings fields' table, the TLV list and its CRC, and Home Assistant's `values` writer and parser (docs/features/23 HAI-017). |
| `plink_service` | New `plink_home_svc.c`: the `plink_dev` task via `alarm_mem_task_create` (4 KB PSRAM stack), builds into `s_dblob[2]` and records, device jobs (8 slots), RES_DEVICES and RES_DEV_ACK. `plink_service_prefs_set()` (non-blocking, a 4-deep queue to the link task). `plink_service_keypads()` under `link_pub_lock`. `g_pub` gains rssi, caps and each peer's report. Console: `plink dev [n]` and `plink prefs <n> [set <key> <v>]` (`set` in development builds only). |
| Keypad service | The core's mirror (`plink_dev_mirror_t`, about 31 KB) and `ui_dev_t[64]` (14.3 KB), all PSRAM, under its own `s_dev_lock`, never `link_pub_lock`. `ksvc_dev_act`, modelled on `command()` (`:592-634`). PREFS report on `panel_prefs_seq()`. Mailbox hand-over. |
| `plink_backend.c` | `be->dev_list = kp_dev_list` and `be->dev_act = kp_dev_act` replace the NULLs at `:314-315`. Lines `:316-317` are deleted, so `ui_port_esp`'s layout entries stay (Arrange per panel). These are set before `ui_app_start` (`panel_app.c:1584` < `:1627`), which matters because `ui_devices_init` returns early without `dev_list` (`ui_devices.c:2498`). |
| `device_service` | `device_service_seq()`, bumped in `port_changed`, `port_result`, and after `device_service_act` or `_act_auto` return HC_OK. `device_service_person_act()`, returning `ds_person_t` with the same numbers as `ui_dev_result_t` (asserted). |
| `home_control` | `hc_mode_word(bit)` |
| `ui_backend` | `ui_dev_from_parts()` in `ui_dev_model.h` (moved from `ui_port_esp.c:1227-1253`). New `ui_prefs.h`: `UI_PREFS_F_*` (= proto ids, asserted), `UI_PREFS_LOOK`, `ui_prefs_merge`. New entries `prefs_remote_take` and `prefs_remote_done`. The comment at `ui_backend.h:1875` changes. |
| `ui_port_esp` | `dev_list` uses `ui_dev_from_parts`. `dev_act` becomes a wrapper over `device_service_person_act`. It implements the two `prefs_remote_*` entries for both roles. |
| `platform_esp` | `panel_prefs_seq()`, `panel_prefs_current()`, `panel_prefs_remote_submit`, `_take`, `_done` and `_cancel`. The seq and RAM copy are updated in `panel_prefs_save` (`panel_prefs.c:134`), so every writer is caught. |
| UI | Drop `&& !ui_link_keypad()` and `\|\| ui_link_keypad()` at `ui_devices.c:2486-2490`, and the forced 0 at `ui_link.c:392-396` (P1, approved 2026-10-16). `prefs_remote_tick` in `ui_settings.c`. `ui_app_held()`. Unchanged: `ui_settings.c:136-138` and `:171` (Integrations and Automations stay master-only). |

Today no device change is signalled: `port_changed` in `device_service.c:93-96` does nothing, and
the screen polls every 1 s (`ui_devices.c:40`). Hence `device_service_seq()` above, also bumped
where `hc_act` sets pending without calling `port.changed` (`home_control.c:563-568`) and where a
timeout in `hc_tick` only calls `port.result` (`:614-620`).

### Memory (estimates)

- **Master:** about 70 KB of PSRAM:
  - the registry copy, `hc_device_t[64]`, about 19 KB;
  - catalog blobs, 2 × 12.5 KB;
  - records, 2 × 2.9 KB in the service, and the link core's own copy (2.9 KB);
  - per keypad in the link core, `plink_home_peer_t`: 1,728 B on the PC build (8 cached DEV_ACT
    answers of 152 B, the settings report), 13.8 KB for 8;
  - the 4 KB task stack.

  The link core's part (about 17 KB) is in `plink_master_t` (`s_m`) with or without home
  control; the rest only with it.
- **Keypad:** about 62 KB of PSRAM: the core's mirror (`plink_dev_mirror_t`, 31,760 B on the PC
  build: the installed list and the one being pulled, the records and an install's scratch), the
  service's `ui_dev_t[64]` (14.3 KB), and `ui_devices`' own buffers, now allocated on a keypad too
  (about 16 KB).
- **Internal RAM:** small statics only, under 1 KB. Check at start-up on both panels.
- **Image size:** run the 85 % check after the build.

Sizes are exact from the field widths. Latencies, ESP-NOW air time and PSRAM figures are
estimates, not measured. A keypad unlock's code check holds the alarm task for about 150 ms, as
any keypad code does: a disarm at that moment waits for it, within LINK-024's 1 s. During packet
loss a keypad's screen waits up to 1 s on a tap (3 s with a code); commands already wait up to
3 s (`CMD_WAIT_MS`, `plink_keypad_svc.c:68`). This design and "Updates through the link" freeze
together: `OWN_PAYLOAD_MAX` (225) and the v1 fixture serve both, and freeze at the first release.
Home Assistant's changes to how a screen looks are rate-limited on the panel, never trusted to
Home Assistant (WCAG 2.3.1, "Display safety" in AGENTS.md).

## Design
- **`components/core/panel_link`** (portable, host-tested, prefix `plink_`): the crypto helpers
  (HMAC/HKDF-SHA256 over `alarm_proto`'s streaming SHA-256; AES-CCM and X25519 from `zwave_host`
  on a port of three primitives), the frame codec and replay window, the message codecs, pairing
  and handshake, the master's and the keypad's link managers (time in, frames out through ports;
  no sockets), the stored records with frozen fixtures, the keypad configuration copy and the
  keypad's snapshot rebuild, and what a keypad's voice hears (`plink_vfeed`, LINK-028). No heap,
  no globals.
- **`components/services/plink_service`** (ESP-IDF, `ALARM_LINK`): the role, one task (internal
  stack: it writes NVS at pairing and removal) that owns a UDP socket, the managers and a PSA
  crypto port, and a worker for the alarm task's calls; mDNS; NVS `link`; on the master an alarm
  listener (snapshot mailbox, events, sounds); on a keypad the `ui_backend_t` overrides, the
  sounder, the voice's feed (`plink_service_set_voice`) and the update gate; the `plink` console
  command.
- **`host/link_sim`**: the PC virtual master (the real engine) and virtual keypad over UDP with the
  OS's random numbers, and `link_smoke`, the two in one process over 127.0.0.1 for CI (LINK-013);
  the seeded test port is refused for peers not on loopback.
- **Mesh Wi-Fi caveat** (ESP-NOW only): master and keypad must share a channel.

## Security notes
- The pairing key, the Wi-Fi password and the keypad's records are in plain NVS (SEC-009: no
  profile encrypts NVS yet). Someone with a keypad in hand for a while can read them; remove and
  pair the keypad again at the master. docs/features/15 lists them.
- A device with a keypad's keys can do what that keypad can: send commands with codes for its
  home partition (the master checks every code and locks the source out after 5 wrong ones) and
  read the state and the configuration's names. A keypad that completes a handshake from a new
  address while its live session was heard in the last 15 s is logged ("a second device is using
  keypad n's keys").
- Once Smart Home and Home Assistant on keypads is built (BL-216; its link core is), a device with a
  keypad's keys can also do in Smart Home what a person at that keypad can: switch what Home
  Assistant's options allow, and unlock or open only with a valid code for the keypad's home
  partition (lockout after 5).
- It can also forge that keypad's PREFS, which changes what the master and Home Assistant show
  about it, never another panel's settings.

## Test and acceptance
- Host: crypto known answers (RFC 3610, RFC 4231, RFC 5869, RFC 7748) and vectors made
  independently with Python `cryptography`; frame golden bytes, and every section with bytes
  appended; the replay window against a set model; pairing (honest; a substituted key, nonce or
  commitment ends the attempt; an attacker aborting after REVEAL is stopped after 3; repeats;
  overlap; PAIR_DONE lost); handshake (replayed HELLO, duplicate WELCOME, first heartbeat lost,
  rekey; the test port asserts no key and nonce is used twice); master and keypad managers together
  over a pretend network with loss, repeats, reordering and corruption on a fake clock (status
  10 s, link lost 30 s, supervision 60 s both ways, commands and their cache across keypad
  restarts and sessions, panic for 60 s, configuration and events, a hostile keypad); random bytes
  into every parser; frozen fixtures of the stored records; the real `ui_app` through a loopback
  link backend; a keypad's voice (`test_plink_vfeed.c`, and in `test_plink_link.c` the real
  policy behind it against the master's fed from the engine: the same decisions, nothing old after
  restarts and cuts, never a wrong or late one under loss) (LINK-028).
- CI: `link_smoke` over UDP on 127.0.0.1.
- Panels (HIL): pair the 10.1" as a keypad of the 7"; arm, disarm and a wrong code from the keypad;
  power the keypad off (trouble after its supervision time, cleared when back); restart the master (keypad back
  within 15 s); remove the keypad (it is told, its frames refused); latency under 1 s; one direction
  dropped; both addresses changed across a router restart; mDNS blocked; the access point's password
  changed and the keypad recovered from its own screen; the master factory-reset and the keypad
  paired again; an update refused during an entry delay; the PC virtual keypad with the 7" and the
  PC virtual master with the 10.1"; the keypad's announcements with both speakers muted (the
  `voice` counters and the development build's `says` lines on both panels: arm, disarm, entry,
  not ready, a chime, a walk test; the keypad's own switches and Voice volume; a keypad restart in
  an exit time and a cut over 30 s saying nothing old).
- The v1 wire of updates, Smart Home and settings (built, on the PC; nothing sends it yet):
  - `test_plink_msg.c` and `vectors_plink_msg.py`: goldens for the 14 types 0x2D-0x3A and both
    record sections; every new section grown with appended bytes, and cut short; each payload's
    longest size (static asserts and `test_longest_payloads`); HEARTBEAT with and without its
    section, read by both the old and the new decoder (`test_heartbeat_section`); DEV_ACK's
    optional record; DEV_ACT the same size for every ref and code, and its digest's known answer;
    what the new codecs refuse; random round trips and random bytes into every decoder; the frozen
    fixture read as its values and encoded again (`test_wire_fixture_v1`) and every v1 number
    pinned (`test_wire_numbers_v1`); a fields body's CRC the same on both sides
    (`test_prefs_body_crc`).
  - `test_plink_frame.c` `test_new_types_one_way`: the new types are known and sealed, each one way
    only (a PREFS_SET without FROM_MASTER, or a DEV_ACT with it, is refused).
  - `test_plink_session.c` `test_caps_meet_in_the_and` and `test_plink_link.c`
    `test_capabilities_offered_meet_in_the_and`: each session keeps the bits both sides offered,
    never one either side doesn't know.
  - `alarm_proto`'s `prefs` suite (`test_prefs.c`): the frozen ids, ranges and keys (each 11 bytes
    or less), the TLV golden and its CRC, wrong lengths, unknown ids, ranges, the JSON values and
    their parser (a request refused whole as `bad`), random input.
- The link core of Smart Home and settings (BL-216 S1; built, on the PC; no panel offers it yet):
  - `test_plink_home.c`, 8 tests: the blob from the registry's rows (a row whose ref the wire
    can't carry left out of the blob and the records alike), at most 64 rows within
    `PLINK_DEV_BLOB_MAX`, `list_crc` never 0, a newer master's blob read leniently and a newer
    compat refused, the records' flags and `states_crc`, records carried over by ref.
  - `test_plink_master.c`, 20 tests (`test_home_needs_the_capability` to
    `test_home_frames_per_tick`): nothing without the bit or on frames under 250 B; the list's
    pages; the full set at a new session; changed records in rounds, the summary every 10 s, state
    requests; a list change named at once and again at +250 ms and +1 s; an action run once from
    its cache, one at a time from its own bucket; DEV_ACK's record as it is when it goes; an
    unlisted ref REFUSED, no list UNAVAILABLE; no code kept; the settings report, PREFS_REQ while
    the CRC differs, writes, and `port.prefs_done` starting the next write; all of it behind a
    waiting STATUS or RESULT and the engine's first snapshot, at most 2 frames per keypad and 8
    per tick.
  - `test_plink_keypad.c`, 21 tests (`test_home_offered_only_with_a_mirror` to
    `test_prefs_set_applied_once`): HOME offered only with a mirror; nothing of either capability
    without its bit or on frames under 250 B; the list pulled, installed whole, a list that fails
    remembered and read offline 30 s after its last data, records carried over by ref; records in
    frame order and only under the installed list, kept as they came; the catch-up; freshness at
    30 s; actions refused at once, resent every 500 ms, given up at 3 s, answered late, in 2 slots
    apart from the commands; list and state requests held while a panic is sent; the settings
    report's timing, the HEARTBEAT's section, a PREFS_SET applied once.
  - `test_plink_link.c`, the real master and keypads together: `test_home_and_settings_end_to_end`,
    `test_home_list_and_states_converge_through_loss` (10 % loss, 2 % copies, 1 % damaged, 300 ms
    of jitter, the list itself changing every 20 s), `test_home_converges_on_frames_of_250`,
    `test_a_device_action_runs_once_through_loss`,
    `test_a_renamed_or_reordered_list_never_acts_on_another_device`,
    `test_a_master_restart_during_a_pull_never_mixes_lists`,
    `test_a_45_s_cut_shows_offline_since_the_cut_and_replays_nothing`,
    `test_a_settings_write_applies_once_and_times_out_when_the_keypad_is_gone`,
    `test_a_keypad_offering_only_fw_gets_nothing_new` (a v1 keypad, then a v1 master) and
    `test_the_storm_never_delays_disarm_or_panic`.
  - After every link test: no command and no device action ran twice, no DEV_ACT went after its
    3 s, no keypad installed a list the master never had or showed a device another's record, and
    every master turn sent a keypad's DEV_* and PREFS* frames after its alarm traffic, never while
    its STATUS change or a RESULT waited, at most 2 to a keypad and 8 in all.
  - The storm runs without loss, so its two runs compare exactly: 64 devices changing every
    100 ms, an action every 500 ms and a settings write every second, while a keypad arms,
    disarms, keys a panic and disarms again. Arm, disarm and panic are answered in 24 ms with and
    without the storm.
- Smart Home and Home Assistant on keypads (designed, BL-216), still to come on the PC:
  - The future peer (`link_peer.c`, `-DPLINK_FUTURE_TEST=1`, LINK-039 and LINK-042): a v1 keypad
    (caps 0x0001) never receives type 0x32 or above, and Home Assistant gets `unsupported` for
    it; a newer keypad with a caps-0x0001 master sends nothing new; bit 0x8000, type 0x3B, field
    id 25, kind 10, action 13, result 9, why 20 and catalog compat 2 are each handled as
    "Versions" (in "Smart Home and Home Assistant on keypads") says.
  - `home_control/test`: `hc_mode_word`. `ui_backend/test`: `ui_dev_from_parts` gives
    byte-identical output to today's conversion on a fixture table (no regression on the
    master); `ui_prefs_merge`.
  - The real `ui_app` through the loopback link backend: the IN_USE, HELD and look-wait paths.
  - Preview: `host_link_demo` with devices in the keypad role (live, link lost, "Not
    confirmed"); `ui_preview.ps1` passes with no cut-off text. CI: `link_smoke` with a fake
    registry on the virtual master, one DEV_ACT and one PREFS_SET.
- Smart Home and Home Assistant on keypads (designed, BL-216), on the panels: the 7" master and
  the 10.1" keypad, both speakers muted. Flash only on the word of the person you work for in
  that session, with each panel disarmed; ports and silicon come from `PANEL.local.md`. Home
  Assistant is paired and allows a dimmable light, a switch, a lock, a cover, a thermostat and a
  scene.
  1. The keypad's rail shows Smart Home. Tiles, areas and states match the master's side by
     side. Arrange on the keypad leaves the master's layout alone.
  2. Each kind from the keypad: Home Assistant and the master follow, and the keypad shows the
     change within 1 s of the master (the development build's `home:` lines).
  3. Unlocks: an unlock with a valid code works; an arm-only code is refused; 5 wrong codes lock
     out only that keypad, and the master's own keypad is unaffected; a code valid only for
     another partition is refused (decision 1).
  4. Link cut (UDP 6056 blocked for 45 s): every tile reads "Offline since…" with the note. A cut
     during an unlock gives "Not confirmed", never a second unlock. After the cut it recovers by
     itself.
  5. The master's Home Assistant link stopped: both screens show offline since the same time.
  6. Storm: a script toggles 20 entities every 200 ms for 60 s. Meanwhile the keypad arms and
     disarms (each answered within 1 s, LINK-024) and a panic is keyed (it reaches the master
     within 1 s). The `plink` counters show no dropped CMD.
  7. Settings from Home Assistant: brightness, volumes, night hours and Announcements reach the
     keypad within 1 s, and its Settings page shows them. With Screen and sound open on the
     keypad, Home Assistant gets `in_use`; during an entry delay, `held`. An automation flipping
     Appearance every 0.5 s for 30 s changes the screen at most once per 2 s. The master's own
     settings work too. The keypad powered off: its entities are unavailable within about 31 s;
     back on, the same entity ids. The keypad removed and paired again: the same Home Assistant
     device.
  8. Versions: the 0.2.0 integration with the new firmware: nothing new, nothing broken. 0.3.0
     with today's firmware: no settings entities and no timeouts.
  9. Internal RAM at start-up on both panels compared with before, and `plink_dev`'s stack
     high-water mark after the storm.

## Open questions
- Keypad-attached inputs (LINK-011), cross-partition GOTO (KPD-006) and partitions beyond home:
  after the Wi-Fi keypad works.
- A link keypad's supervision trouble takes its partition from the engine's device-table entry with
  the keypad's number (`alarm_engine_device_reachable`), enabled or not: all partitions while that
  entry was never given one, but an entry once used for partition 2 and switched off puts a
  partition-1 keypad's trouble, its restore, its report and `trouble_unack` on partition 2. Known
  limitation, as for Z-Wave's keypads: a core call taking the partition (the keypad's home), or the
  engine using all partitions for an entry that isn't enabled, would fix both. The trouble names
  the keypad by number only ("Keypad 3 not responding"); a name lookup shared with Zigbee and
  Z-Wave would fix it.
- LINK-001's number shared with Zigbee: the link chooses from Zigbee's numbers as its worker read
  them (at most a second old: the zone lock is never taken on the link task, LINK-027), reserves
  the chosen one with Zigbee at once and reads Zigbee's numbers again without waiting before the
  pairing is answered (a clash ends the pairing, "Nothing was saved"). Left open: a Zigbee keypad
  given the same number within that second while the zone lock is busy at the moment of the check.
  A Zigbee-side call that claims a number under the zone lock would close it.
- The setup's pairing on a master (Option C) sends the other master nothing after PAIR_DONE until
  the setup's Done, so an abandoned setup leaves only a provisional keypad there, dropped after
  10 minutes (LINK-016); a setup left on its last step for longer loses the pairing too (the
  keypad then says it isn't paired: pair again). Its last step names the home partition from the
  master's name (a master is named after partition 1), "Partition 2" otherwise: PAIR_DONE carrying
  the home partition's name (an appended field, LINK-017) would name every partition.
- For the maintainers: should panic be resent longer than 60 s; do a keypad's mains and battery
  flags become device troubles (an engine change) or stay in the keypads list. Decided 2026-10-15:
  fire and CO keep sounding on a keypad that lost the link (Roles); 5 min default supervision
  (LINK-007); the screens as proposed (`BL-048-panel-link.js`, approved).
- Not taken from the review: a persisted boot counter in HELLO (the seen-n_k list and the X25519
  budget already bound replays); a new `alarm_result_t` for "no link" (UNAVAILABLE says it).
- For the maintainers, from the six-hour host run (`test_plink_link.c`, 10 % loss and 1 %
  damaged): a keypad starts a handshake about every two minutes (15 s without a STATUS, after one
  is lost). Since the handshake waits for a STATUS rather than any frame (the end-to-end wave's
  suggestion, taken), no keypad showed "No system communication" in 84 runs (seeds 0..20, four
  keypads each), against 5 in 84 before: a new session brings a STATUS at once. On a working
  network neither is seen.
- The result cache keeps a command from running twice, but every CMD frame gets a RESULT (a
  refusal, or the cached answer to a copy): a device with a keypad's keys gets one RESULT for each
  CMD it sends, though its commands and requests stay within LINK-027's limits.
- Keypad updates, for the owner (designed 2026-10-16; built as recommended unless decided
  otherwise): a keypad accepts a new signing key at its own screen with the installer's code
  (LINK-036; without it a master moved to another key leaves every keypad needing its console or a
  cable); a keypad found behind later waits for Update keypads until BL-190 decides automatic
  updates; a master's self-test needs one of its keypads back (LINK-041; the risk: a master whose
  only keypad is off during its update goes back to its old version); the update server takes
  `want` (BL-050; without it a keypad offered a newer version than its master's takes the relay).
  Estimates, not measured yet: the relay's speed, `esp_partition_mmap` windows of the running
  slot, the slot's exact length with its signature sector, free PSRAM on a master for a copy.
- Smart Home and Home Assistant on keypads (BL-216; its 13 questions were decided 2026-10-16, in
  Decisions). Not designed, only if the owner asks (each needs its own proposal): a notice on a
  panel when a setting changed from elsewhere (new words); a keypad's Screen and sound rows on the
  master's Installer > Keypads page; a Panel Manager page for keypads (docs/features/31 mockup).
  No proposal is needed for a setting applied from elsewhere (the existing rows show it, and an
  open page refuses the change) or for the Home Assistant entity names (the integration's own
  strings, reviewed in its pull request). A keypad's mains and battery in Home Assistant wait for
  the maintainers' question above.

## Tasks
- [x] `panel_link` core: crypto helpers with known-answer tests, the v1 frame codec and replay
  window, message codecs, pairing (LINK-003, -004, -005, -016, -017) (BL-048, e59c7f6, 9384622,
  f6d7ee0, 3ae2b70)
- [x] `panel_link` core: handshake and sessions, master and keypad managers, stored records with
  frozen fixtures, the keypad configuration copy and lenient decode, the keypad's snapshot rebuild
  (LINK-001, -006, -007, -008, -014, -015, -018, -019, -023, -026, -027) (BL-048, e05415a,
  1aeeba6, 2205fbc, 1c5f6fa, aaaec6d)
- [x] LAN UDP transport and the PC virtual keypad and virtual master (`host/link_sim`, the PC
  simulator of docs/features/21), `link_smoke` in CI (LINK-013) (BL-048, 317253d, 7c9e3d6)
- [x] Loopback on the master: the touch UI reaches the alarm task through `ui_backend_t` (M3)
  (`components/services/ui_port_esp/src/ui_port_esp.c` `command` calls `alarm_service_command`)
- [x] `plink_service` on the master: UDP, mDNS (LINK-021), the listener, the worker, supervision
  through `alarm_service_device_reachable` (LINK-007), the device numbers shared with Zigbee
  (`zigbee_service_devices_held`, `zigbee_service_set_link_devices`; LINK-001), Wi-Fi power-save
  holds, the `plink` console (BL-048, 3c0f63c, c7e4396, adac98d, 32894bf)
- [x] Keypad role on a P4: `panel_app` without the engine and the master's services, the keypad
  backend for `ui_backend_t` rendering the master's status and sending its commands (LINK-002,
  LINK-020), sounds from the master, the update gate and `PANEL_OTA_OK_LINK` (BL-048, c7e4396,
  2bea2f7, dbf172f, 32894bf); the 4" board's side is docs/features/19 CYD-015
- [x] Pairing screens and the keypads' section in installer mode, the setup's role step, the
  keypad's Connecting, "No system communication", removed and no-code maintenance states, the role
  gate in Settings and About (LINK-004, -008, -014, -025; UI-011, UI-012): `ui_link.c`,
  `ui_wizard.c`, walked in the preview over `host/ui_preview/host_link_demo.c`; the mockup's
  `screens/panel-link.js` (BL-048, 18cc635)
- [x] On the two development panels, the 7" master and the 10.1" keypad (2026-10-16): pairing by
  the setup's path and by the keypad role, found by mDNS, the same code on both; arm, disarm and a
  wrong code from the keypad (about 150 ms with the code's hashing), its own lockout after 5 wrong
  codes with the master's keypad unaffected; a change on the master shown on the keypad in about
  0.3 s; supervision raised 60 s after the keypad went silent and cleared when it was back; a
  master restart (status again within 15 s, never "No system communication"); removal (the keypad
  told) and pairing again; a PC virtual keypad paired with the 7" from another subnet; hours of
  soak with an arm and disarm every 30 min (LINK-004, -006, -007, -008, -013, -014, -021, -024)
  (BL-048, cf6ad2b)
- [ ] On the panels, what needs a person or a network change: the screens on the glass (pairing,
  Connecting, "No system communication", the kept fire tone's Silence, removed, maintenance), one
  direction dropped, both addresses changed across a router restart, mDNS blocked (the broadcast
  fallback), the access point's password changed and the keypad recovered from its own screen,
  the master factory-reset and the keypad paired again, an update refused during an entry delay,
  the PC virtual master with a panel keypad (needs the PC's firewall to let UDP 6056 in)
  (LINK-008, -021, -025) (M7) (BL-048)
- [x] A keypad's own voice (LINK-028; decided 2026-10-16): `plink_vfeed` and its tests,
  `ann_set_keypad`, `panel_voice`'s source, baseline and pause, the keypad's feed and tracked
  sounds, the master's SOUND before its STATUS, the keypad's Voice volume and Announcements (the
  role gate split: `ui_voice_editor_present`), the voice compared with the master's in
  `test_plink_link.c`, the preview's `bl-048-kp-announcements` (BL-048, 5720f24, 674a736)
- [x] On the panels with both speakers muted (2026-10-16; the `voice` counters and the `says` lines
  on both, the keypad within 0.2 s of the master): arm (Arming, then Armed), disarm, entry, not
  ready, a chime with approved words (the keypad holding the master's pack revision), a walk test's
  start and end (its trips, and a chime, on zones without approved words silent on both), a keypad
  restart in an exit time (Armed at its end once a baseline there joins the arming) (LINK-028)
  (BL-048, 5720f24, 674a736, 660b91c)
- [ ] On the panels, what needs a person at the keypad or a network change: its Announcements,
  Status messages and Zone chime off and Voice volume 0 while the master speaks; a link cut over
  30 s saying nothing old; its internal RAM at start-up beside before (LINK-028) (BL-048)
- [ ] ESP-NOW transport: through our C6 bridge on the P4 (docs/features/12 CP-003), native on the
  ESP32; the channel follows the master's access point (LINK-010) (M7) (BL-048)
- [x] The engine's device supervision trouble for a silent keypad (LINK-007's engine side,
  CORE-TRBL-008): `test_troubles.c` test_device_supervision, `alarm_engine_device_checkin`
- [ ] ESP-NOW <-> LAN failover (LINK-009) (M7) (BL-048)
- [x] Wire: types 0x2D-0x31, `PLINK_CAP_FW`, the STATUS bits UPDATING, HOLD, FIRE and CO defined,
  the codecs and Python goldens; every v1 payload and number in a frozen
  `test/plink_wire_fixture_v1.h` (LINK-032, -037, -038, -040) (BL-048, a97a745)
- [ ] The STATUS bits UPDATING, HOLD, FIRE and CO in `plink_view.c`: set from the master's
  snapshot, read on the keypad (LINK-030, -034, -039) (BL-048)
- [ ] Keypad fail-safes in `plink_view.c`, FIRE/CO sounding in `continuous_of`, and UNAVAILABLE for
  an unknown command type (LINK-039) (BL-048)
- [ ] Keypad gate: master flags, panic pending, kept tone, the INSTALL hold (LINK-034) (BL-048)
- [ ] Keypad core: orders, the in-order pull, FW_INFO and FW_STATE (LINK-029, -032, -035) (BL-048)
- [ ] `panel_ota`: `want` in `proto_fwsrv_facts_t`; `post_check` taking facts, a server and a
  channel for one run; the ordered release run (exact version); `PANEL_OTA_KIND_LINK` with Stop;
  `panel_ota_set_checks`, `panel_ota_set_restart_hold`, `panel_ota_running_image`,
  `panel_ota_fetch_for`; `PANEL_OTA_OK_KEYPADS` (LINK-031, -033, -041) (BL-048)
- [ ] Keypad service: dispatch, the own Install refused in the keypad backend, the key screen
  (LINK-029, -036) (BL-048)
- [ ] Master: `plink_fw.c` planner, FW_REQ serving and buckets, `fw_read` over mmap or PSRAM,
  `link/fwgo`, the supervision hold (LINK-030, -031, -032) (BL-048)
- [ ] Event stages (docs/features/05), MQTT `keypads`, `plink fw` console (BL-048)
- [ ] Screens: a proposal first, then the master's Keypads block and the keypad's view (BL-048)
- [ ] `host/link_sim/link_peer.c`, the future peer (`-DPLINK_FUTURE_TEST=1`), `tools/link_compat.py`
  (LINK-042) (BL-048)
- [ ] The update server takes `want` (update-server repository, BL-050)
- [ ] On the panels: the fetch path; the keypad's WAN blocked at the router, so it relays; the 10.1"
  keypad of the 7" master, from a copy; armed mid-relay; a panic pending; a kept fire tone; power
  pulled mid-relay and mid-write; a master restart during a keypad's self-test; a v1.x image ordered
  to a v3 keypad (refused before any write); `display watch` and touch on the keypad while it writes
  (BL-048)
- [x] Run-time caps: the offered caps an argument of `plink_master_init` and `plink_keypad_init`,
  each session keeping both offers ANDed under `PLINK_CAPS_KNOWN`; no panel offers a bit yet
  (LINK-043) (BL-048, a97a745)
- [x] Wire: 0x32-0x3A, the HEARTBEAT section, the codecs and Python goldens, in the frozen v1
  fixture; `proto_prefs` (LINK-043, -050, -051, -052; HAI-017) (BL-216, a97a745)
- [x] Master core: list, pushes, summary, catch-up, DEV_ACT cache and buckets, PREFS report and
  PREFS_REQ, PREFS_SET, send order; `plink_home`; the link tests and the storm (LINK-044..051)
  (BL-216, ed58b12)
- [x] Keypad core: `plink_pull_t` from the configuration pull, mirror and catch-up, freshness,
  DEV_ACT slots and overlay, PREFS report and set answers (LINK-044..051) (BL-216, ed58b12)
- [ ] One judge and one converter: `device_service_person_act`, `device_service_seq`,
  `hc_mode_word`, `ui_dev_from_parts`; `ui_port_esp` moved onto them with the master's screen
  unchanged (LINK-048; HCTL-005, -006, -012) (BL-216)
- [ ] Master service: the `plink_dev` task, builds and jobs, `plink_service_prefs_set`,
  `plink_service_keypads`, `g_pub`, the console (LINK-049) (BL-216)
- [ ] Keypad service and backend: mirror, `ksvc_dev_act`, `kp_dev_list`, `kp_dev_act`, layout
  entries kept, PREFS report, mailbox hand-over (LINK-046, -047, -050, -051) (BL-216)
- [ ] UI-046: `panel_prefs_seq`, `_current` and the mailbox; `prefs_remote_*`; `ui_prefs_merge`;
  `prefs_remote_tick`; `ui_app_held` (BL-216)
- [ ] The role gates removed as proposal P1 shows (approved 2026-10-16); `host_link_demo` with
  devices; preview shots compared (HCTL-013, UI-044) (BL-216)
- [ ] `host/link_sim`: a fake registry on the virtual master, `devices` and `prefs` on the virtual
  keypad; `link_smoke` extended (LINK-013) (BL-216)
- [ ] Home Assistant: `alarm_proto`'s four JSON messages (`proto_link`; `proto_prefs` is in the
  Wire box), `ha_link`, `panel_app` wiring (docs/features/23 HAI-016..019); ha-addon 0.3.0
  (BL-216)
- [ ] On the panels: the Smart Home and settings list in Test and acceptance (BL-216)
- [ ] Docs: the 11, 23, 24, 07 and 15 status lines; docs/features/README "Current status"; the
  pull request notes for the documentation repository's `UPDATING.md` (BL-216)

Covered by boxes in other documents (all BL-048):
- Keypad-attached inputs as zones (LINK-011): docs/features/02.
- The keypads' mDNS announcement (`_roboalarms-link._udp`): docs/features/09.
- Cross-partition GOTO on the keypads (KPD-006): docs/features/01, "On the keypads".
