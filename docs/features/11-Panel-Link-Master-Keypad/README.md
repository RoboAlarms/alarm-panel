# 11 · Panel link: master and remote keypads

**Status:** Implemented v1 over Wi-Fi (BL-048): the portable protocol (`panel_link`), the master's
and the keypad's service (`plink_service`), the keypad role on the P4 panels, the screens (approved
2026-10-15) and the PC simulator (`host/link_sim`, `link_smoke` in CI), reviewed three times; on
the two development panels (2026-10-16) pairing, commands, lockout, supervision, a master restart,
removal and pairing again, and a PC keypad with a panel worked, a change shown on the keypad in
about 0.3 s. A keypad's own voice (LINK-028, decided 2026-10-16) works on the panels, tried with
both speakers muted. Smart Home and Home Assistant on keypads (BL-216, designed and approved
2026-10-16) is built: the wire, the link core, the master's and the keypad's services, the
keypad's Smart Home page as proposal P1 shows, the PC simulator and the preview. On the two
panels (2026-10-16, both speakers muted) the keypad and the master share HOME and PREFS, a
settings change from the master reaches the keypad's screen and its report within a second (held
during an exit time), the keypad's commands still answer in 150-190 ms, and its device data stays
0-10 s old on the master's summaries; the device tiles were tried only on the PC, as the bench
master had no devices (Home Assistant not connected). The integration's 0.3.0 is built in
`RoboAlarms/ha-addon` on a local branch, not released. Keypad updates at the master's order (M8)
are built and tried on the two panels: direct FETCH with an exact-version server request, relay,
restart together and following the master's rollback, with both speakers muted. Left:
the bench cases below that need a person or a network change, Smart Home and settings on the
panels with Home Assistant connected, ESP-NOW range/channel-change and firmware-relay acceptance,
433 MHz, keypad inputs,
and the keypad-update Panel Manager pages ·
The master's settings and installer menu on keypads (BL-223) is designed and its
screens approved (2026-10-21), being built ·
**Milestone:** M7 (keypad updates M8) · **Depends on:** 01, 04, 09, 12, 15

The P4/C6 ESP-NOW transport is also built and verified on hardware: authenticated commands,
automatic failover in both directions, and operation during an actual Wi-Fi disconnect followed
by automatic reconnection. A keypad reboot with the master's LAN blocked also completed its
session and received all eight configured zones. Full firmware relay over ESP-NOW, range and
AP channel changes still need hardware acceptance. The 7-inch display recurrence is B023.

## Nearby enrollment and Wi-Fi provisioning (LINK-053)

Approved 2026-10-18 by the owner: ESP-NOW discovery first, automatic Wi-Fi setup after both comparison-code
confirmations, and **Select Wi-Fi** available before discovery and after pairing. The same
Panel Manager enrollment instructions serve normal and headless masters. Language, display
and audio stay local; date, time and time zone continue to follow the master.

Implemented and host-tested. On the two development panels (2026-10-18, dev.13), USB/API
service-level tests passed enrollment with saved Wi-Fi erased, matching comparison codes and
both confirmations, authenticated radio credential transfer and automatic Wi-Fi connection.
Unpaired requests were refused, automatic setup preserved manually entered credentials, and
Select Wi-Fi cancellation followed by manual LAN enrollment passed with the radio path blocked.
The tests invoked the same service requests as setup; physical touchscreen acceptance and
factory-first-boot/resume acceptance remain open. Both speakers stayed muted and displays lit;
local display/audio preferences were unchanged and language settings were not modified.
The keypad persists its pairing and role before opening its first authenticated session. Wi-Fi
failure, manual selection and power loss after that point retain enrollment. An unfinished
setup resumes after reboot. Done requires a fresh authenticated master connection; without
Wi-Fi it is enabled only after the automatic attempt ends and the radio link is online.

`PLINK_CAP_WIFI` (0x0008) negotiates two appended sealed messages: `WIFI_REQ` (0x3B, K->M)
contains a nonzero request ID (u32 little endian); `WIFI` (0x3C, M->K) echoes that ID followed
by SSID[33] and password[65], both NUL terminated. Empty SSID means unavailable. The 102-byte
reply fits ESP-NOW's 250-byte frame. Existing type numbers and frozen v1 fixtures are unchanged.
No credential is advertised, logged or sent before a paired authenticated session, and the
master shares the home network only within 10 minutes of that keypad's PAIR_DONE
(`PLINK_WIFI_SHARE_MS`, kept in RAM: a master restart ends it); any later request, or one from a
keypad paired before the master started, gets an empty SSID. Requests use
the master's request-rate bucket; retries reuse the ID, replies are consumed once, and cancelling
before manual entry rejects late replies. The service retries once per second for up to 30 seconds.
A master without this capability falls back to manual selection. Temporary browser network
trials cannot be shared. Stored credentials are installed only into an empty configuration under
the same lock as manual changes; unreadable storage fails closed. Plaintext scratch buffers are wiped.

PC coverage: credential codec boundaries, malformed/stale replies, open/unavailable networks,
pre-pair refusal, negotiated capabilities, lost replies/retry, cancellation, older peers and removal;
the real LVGL setup walk covers discovery without Wi-Fi, confirmation, transfer, loss, radio-only
completion and manual selection after pairing. See [current enrollment screens](../07-UI-LVGL-Pro/mockup/screens/radio-enrollment.js).

Recovery coverage: the dev.13 bench keypad retained enrollment through an orderly restart
while its credential request was blocked, then accepted a resumed setup request without
pairing again. A second restart after credential storage automatically rejoined Wi-Fi and
the authenticated session. A 30-second provisioning timeout retained enrollment; restoring
radio connectivity and retrying succeeded. USB invoked the setup service request after the
first restart; this does not establish physical first-boot or abrupt-power-cut acceptance.
Protocol regressions reject a pre-restart credential reply, recover with a fresh request, and
retain pairing with a master lacking `PLINK_CAP_WIFI` across either peer's restart and a later
master upgrade. Older-master coverage is simulated, not an installed firmware downgrade.
The real LVGL walkthrough also checks delayed publication of stored enrollment at startup,
automatic resumption, offline completion disabled, and manual fallback without pairing again.

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
- **One protocol, pluggable transports** (ADR-054): LAN UDP over Wi-Fi and ESP-NOW through our
  C6 build; 433 MHz later. Frames, pairing and sessions are the same on every transport; only
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
  partitions" remain separate from temporary view switching.
- **A factory-C6 keypad needs the router and master powered**. Our C6 bridge adds direct radio
  fallback; the installer guide still recommends backup power for the router.
- **Keypad updates** ("Updates through the link"): decided 2026-10-16 (the owner: "Start building
  keypad updates with the four recommended choices"):
  1. A keypad takes its master's update server and signing key from its master's orders (changed
     2026-10-17, the owner: changes from the master get pushed to the keypads; the key screen on a
     keypad was unnecessary). The installer changes them only on the master (LINK-036). It replaces
     the first choice, a key accepted at each keypad's screen with the installer's code.
  2. Keypads never fall behind: updating the master updates its keypads, and a keypad found behind
     later (newly paired, or back on line) is brought to the master's version by itself, with no
     person's step (changed 2026-10-17, the owner: keypad updates should be seamless, updating the
     master should update the keypads, and they must not get behind). It replaces the first choice,
     that such a keypad waited for Update keypads. Failures are tried again by themselves
     ("Updates through the link", The rules).
  3. A master's self-test needs one of its keypads back (LINK-041), accepting the risk that a
     master whose only keypad is off during its update goes back to its old version.
  4. The update server takes `want` (BL-050): without it a keypad offered a newer version than
     its master's would take the relay.
  5. A keypad never updates past its master (2026-10-17, the owner: the keypads must not update to
     a version past the master). Every way a paired keypad can update is capped at the version its
     master runs ("The keypad", Never past its master); only a cable goes past, for development.
  6. Downloading goes on while armed; a keypad restarts only once everything is disarmed, so the
     restart is quick (2026-10-17, the owner).
  7. Keypads update at the same time as the master: they download alongside it and restart with
     it, on trial until it passes its self-test, and go back with it if it goes back (2026-10-17, the
     owner: "99% of the time the remote keypad should be able to update at the same time as the
     master"; chosen: restart together).
  8. The simple way (2026-10-17, the owner: "I feel you are doing this the complicated way"): no
     turn-taking between keypads, no first-keypad-per-model check, no Stop or Try again on any
     screen, no keypad that waits for a person ("Updates through the link", Not done).
  9. Nobody steps in (2026-10-17, the owner: the people using the alarm system want something that
     works without having to do anything; 99.9 % would not know what to do when asked by the panel,
     most installers included, who are gone once it is installed): no screen asks anyone to act, and
     the master does what it can itself (it fetches the image for another model of keypad).
  10. The master waits for its keypads still getting its update (2026-10-17, the owner: "Yes, OK:
     the master waits for keypads still downloading (up to 30 min), with the line "Its keypads are
     getting the update""): its own restart waits while a keypad on line still downloads or
     receives it, at most 30 min from when its own update was ready ("Updates through the link",
     Restarting together).
- **Keypad updates' screens** (proposal P2, built: `docs/features/07-UI-LVGL-Pro/mockup/screens/panel-link.js` D):
  eight screens with nothing to tap, approved 2026-10-17 (the owner: "Approve, recommendations OK";
  they replace the 31 approved earlier that day, found too complicated). Its two questions, as
  recommended: the master keeps today's "Stop the update" for whoever chose to install, which stops
  its keypads' downloads too; a keypad's banner names its master ("Updating · restarts with Harbor
  House"), its Software update page showing the name whole.
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
      parts and words only, its pictures from the preview's `host_link_demo`. Built as it shows
      (`ui_devices.c`, `ui_link.c`; 787c2da), its screens now in
      `docs/features/07-UI-LVGL-Pro/mockup/screens/panel-link.js` (`kp-devices`,
      `kp-device-unlock`, `kp-devices-lost`, `kp-devices-unconfirmed`).

- **Keypads as full front ends to their alarm panel** (BL-223): decided 2026-10-21 (the owner:
  "Approve the BL-223 proposal and its five questions as recommended"):
  1. Installer on a keypad is a section of its Settings list under the alarm panel's name.
  2. The alarm panel's factory reset, setup, update server and channel, developer access and,
     in the first version, the zones' spoken-name editor stay on it and its Panel Manager.
  3. A keypad's clock and time zone keep following its alarm panel (LINK-053); its 12- or
     24-hour clock, language and region are its own.
  4. Installer mode is open in one place at a time; the alarm panel's screen and Panel Manager
     may end a keypad's.
  5. Another keypad may be paired from a keypad in installer mode.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| LINK-001 | One master, up to 8 panel-link keypads on device numbers 1-8, shared with the Zigbee keypads: a new keypad takes the lowest number no Zigbee keypad, no link keypad and no enabled entry of the installer's device table holds, chosen under the Zigbee zone lock (docs/features/20 alarm-devices.md §4); numbers 9-16 are the Z-Wave keypads' (docs/features/36 ZW-031) | unit |
| LINK-002 | Keypads render the master's status and send commands; they never validate codes | review + unit |
| LINK-003 | Every data frame is authenticated and encrypted (AES-128-CCM, 8-byte tag) under per-session, per-direction keys; sequence numbers start at 1 and a 64-frame sliding window, updated only after the tag verifies, rejects replayed frames; frames of another session, net or direction are refused | host crypto tests |
| LINK-004 | Pairing needs the installer at the master (touchscreen installer mode or an authenticated Panel Manager installer session, including a headless master), uses commit-then-reveal, and both sides show the same 6-digit code and peer identity, confirmed on both (defeats a device in the middle; [browser enrollment](../31-Browser-Panel-Manager/keypad-enrollment.md), approved 2026-10-18 by the owner) | unit + HIL |
| LINK-005 | ESP-NOW control frames fit in 250 bytes; a UDP datagram is at most 1200 bytes; bulk data (configuration, events, zones) is paged to the transport's largest frame | unit |
| LINK-006 | The master sends status on change (repeated at +250 ms and +1 s, at most one per 100 ms per keypad) and at least every 10 s; keypads send heartbeats every 10 s (±1 s jitter) | unit + HIL |
| LINK-007 | The master raises a device supervision trouble (`alarm_service_device_reachable(n, false)`) for a keypad not heard for its supervision time (5 min by default, per keypad: 1 min, 5 min, 15 min or not supervised; decided 2026-10-15), or one that reports no fresh status for that long; it clears it when both directions are fresh again and before a removed keypad's number is freed; supervision pauses while the master has no transport to its keypads - its own Wi-Fi down and its ESP-NOW radio not ready (the network trouble covers it) - and resumes one period after either is back; with Wi-Fi down and the radio up it runs | unit + HIL |
| LINK-008 | A keypad shows "No system communication" after 30 s without an authenticated status and refuses arming; it never shows stale status as current; from start-up until the first status and configuration are in it shows a steady "Connecting" state | unit + HIL |
| LINK-009 | Automatic failover ESP-NOW <-> LAN within 15 s; duplicates suppressed by sequence number | HIL (ESP-NOW) |
| LINK-010 | The master's ESP-NOW channel follows its Wi-Fi AP; beacons carry the channel; keypads rescan channels 1-13 when lost, pausing their own Wi-Fi's connection attempts for a radio window only while that Wi-Fi is down (15 s, doubling to 5 min, from 15 s again once the master is heard) | HIL (ESP-NOW) |
| LINK-011 | Keypad-attached inputs are reported as zone events (zone expander role): built as LINK-055 (wired terminals) | M7 |
| LINK-012 | Keypad updates at the master's order: a keypad that reaches the update server fetches its own image, otherwise the master relays it over the link (LINK-029..037; [13](../13-OTA-Updates/README.md)) | unit + link + HIL |
| LINK-013 | A PC "virtual keypad" and "virtual master" (host build, UDP, the OS's random numbers) interoperate with each other in CI and with panels | CI + manual |
| LINK-014 | Removing a keypad at the master clears its trouble, sends a sealed BYE (three times), then wipes its keys (RAM first, then NVS); the keypad erases its master record and says it is not paired | unit + HIL |
| LINK-015 | Each connection starts with a handshake (ephemeral X25519 and the pairing key) giving fresh session keys and a session id; a session's keys are installed once and nothing resets its counters; a replayed or forged HELLO never drops a live session; no side sends with seq ≥ 2^31 | unit |
| LINK-016 | Pairing: within an attempt each value (pk_k, pk_m and commit, n_k, n_m, mac_k) is set once - only a byte-identical repeat gets the stored answer, a different one ends the attempt; an attempt counts once its secret is disclosed (the master's at PAIR_REVEAL, the keypad's at PAIR_NONCE), whatever ends it; 3 attempts per window, at least 3 s apart; the window lasts 2 min, 30 s per step, 2 min to compare the codes; a keypad starts an attempt only when a person asks; two keypads or two masters at once end the attempt; nothing is stored before both confirmation MACs verify | unit |
| LINK-017 | Version skew: every payload section carries its length and a receiver takes what it knows and skips the rest; fields are only appended; the session uses the lower protocol version and the common capabilities (this side's offer, masked with `PLINK_CAPS_KNOWN`, AND the peer's, so a bit either side doesn't know is never shared), and a request the master can't serve is answered UNAVAILABLE; an unknown message type is dropped; v1's header never changes; the rules for later versions are LINK-038..042 | unit |
| LINK-018 | Every command carries a request id (a random base drawn at each keypad start, then counted); the master answers a retry from its cache keyed on (keypad, request id) with a digest of the command, kept 90 s after the last copy arrived, and never runs a command twice; the same id with a different command runs as a new one; codes are never stored or logged, and every copy is wiped after use | unit |
| LINK-019 | The keypad's copy of the configuration holds no code material (each code hash replaced by a filler unique to its slot, the salt zeroed) and shows duress users as standard users | unit |
| LINK-020 | A panel in the keypad role runs no alarm engine, event log, zone sources, device sources or integrations; it shows and controls its master's devices through the link (LINK-044..049; built, BL-216); it updates itself only at its master's order (LINK-029) or through LINK-025, only while its link is up and the master's partitions allow it (OTA-004), and its update self-test needs the link | HIL |
| LINK-021 | On Wi-Fi the master is found by mDNS `_roboalarms-link._udp` on UDP port 6056, announced only while it has a keypad or pairing is open (NET-007), and by a subnet broadcast when mDNS finds nothing; a peer's address moves only on a sealed frame that passes the tag and replay checks | HIL |
| LINK-022 | The link keys are never in a backup (the `link` namespace is not in the backup policy); a factory reset erases them | unit |
| LINK-023 | Panic keyed on a keypad is resent with the same request id every 1 s until answered or 60 s have passed, whatever the link state, and runs once (KPD-007) | unit |
| LINK-024 | On a working Wi-Fi network a keypad shows a change of state within 1 s, and a command's answer within 1 s (Wi-Fi power save off on a master with keypads and on keypads) | HIL |
| LINK-025 | After 10 min without system communication a keypad opens Wi-Fi, Software update, Pair again and "Make it a stand-alone panel" without a code: a keypad holds no codes and the master supervises it, so cutting it off is no worse than unplugging it; a keypad that is not paired (removed at the master, or never paired) opens them at once - no master could check a code, and it protects nothing | unit + HIL |
| LINK-026 | A keypad applies a status block, partition record, zone record or time only when it came in a newer frame of the session than the one it last applied for that item | unit |
| LINK-027 | The master accepts a keypad's commands for its home partition or its authenticated temporary selection only (NOT_AUTHORIZED otherwise), one at a time with panic in a slot of its own, at most about 2 a second (a burst of 5); requests over their limits are dropped in the link task, before anything reaches the alarm task | unit |
| LINK-028 | A keypad speaks its home partition's announcements (docs/features/08 D05) from the master's pushed events, sounds and status, with its own Announcements switches and Voice volume (D06; decided 2026-10-16); nothing older than its last baseline (a reconnect, a backfill, a master restart) is ever spoken, nor an event the master made more than 5 s before its time now (by the engine time every STATUS carries, or its wall clock), nor the end of an exit time learned more than 5 s after the keypad's own countdown ended, and nothing while the link is not ONLINE; a keypad whose baseline falls in an exit time (it started, or its link came back) says that exit time's end as the master does | unit + HIL |
| LINK-029 | A keypad whose session has `PLINK_CAP_FW` installs firmware only at its master's order, never older or equal and never past its master (decided 2026-10-17): its own ways to update are capped at the version INFO names (its Install only in LINK-025's maintenance and the needs-update state, its check sending `want` = that version; its console and every writer), an ordered run at its order's version and on trial until its master runs that version confirmed; no update check of its own, no Install, no Stop; only a cable goes past | unit, HIL |
| LINK-030 | The master keeps every keypad with `PLINK_CAP_FW` on its version with no person's step (decided 2026-10-17): it sends FETCH{V} to every keypad behind as soon as it has its target V (its own update downloaded and checked, or the version it runs) and INSTALL{V} to every READY keypad when its own restart is allowed, restarting with them; a keypad found behind later installs when READY and its gate passes; a failed download is tried again after an hour; a keypad that went back on V waits for the master's next version; meanwhile a restarting keypad's supervision trouble isn't raised (up to 10 min) and a keypad installing alone holds the master's own restart | unit, link |
| LINK-031 | FETCH first: the keypad downloads V from the update server itself. RELAY from the master's image of V (just downloaded, or running) when the keypad can't reach the server, its download broke off, or it isn't offered V (13-15), and it is the same device, within the image's chip revisions and of an accepted project. A refusal by the server (10-12, 16-19) is never relayed; another model without internet isn't updated | unit (planner) |
| LINK-032 | The keypad pulls bytes by image digest (`id8`) and offset, never by session, in order into its writer, within the order's page, count and gap and a 16 KB window, with flash busy at most half the time; it continues across a new session and a master restart with the same image. The master answers statelessly, only to the keypad holding the relay, from a bucket of its own; LINK-024 holds while it runs | link (lossy) |
| LINK-033 | A keypad checks every image itself: the header (chip, revision range, project, newer) before the first flash write, then length, SHA-256, the signature with its own keys, and the version named. Nothing is forced or older. An order's channel serves that run only; its server and key become the keypad's trust record (LINK-036); nothing else the master sends becomes trust | unit; HIL |
| LINK-034 | A READY keypad restarts only after an INSTALL for that version and when its gate passes, asked twice: a live session with a status under 15 s old, the master's UPDATING and HOLD clear (except the INSTALL sent with the master's own restart), `proto_fw_install_allowed()`, no panic still being resent, no kept fire or CO tone. Downloading and the relay go on while armed (decided 2026-10-17) and pause only while an alarm sounds, an entry or exit time runs, a panic is being sent, a fire or CO tone is kept, or the link is down. A keypad that installed with its master stays on trial until an INFO names that version; an older one, or none within 10 min, sends it back | unit; HIL |
| LINK-035 | A relay fails after 60 s without a byte while not paused, or after 30 min paused (not counted while paused for an alarm, entry or exit time). A READY image is discarded after 24 h without INSTALL, on CANCEL, on another image's order, or on removal. After 3 failed runs in 24 h a keypad answers BUDGET. Orders are resent after 300 ms, doubling to 2 s; the keypad counts as away after 30 s | unit |
| LINK-036 | A keypad takes its update server, channel and signing key from its paired master's sealed orders, which name the ones the master trusts (decided 2026-10-17): one whose trust record differs stores the order's server and key as its own before it checks or downloads anything; the installer changes them only on the master (installer's code and a hold, never with the factory code, OTA-020, OTA-021); no keypad screen asks for a key | unit |
| LINK-037 | FW_* messages go only on transports whose largest frame is 250 B or more, never on 433 MHz; FW_INFO, FW_ORDER and FW_STATE fit 225 B at their longest; FW_PAGE fits the peer's frame | unit |
| LINK-038 | The floor: a master serves every v1 message above to a protocol-1 keypad for as long as one may be paired, and a later keypad works with a v1 master; neither refuses a peer at protocol 1 or above; a HELLO stays at most 128 B | frozen fixture, frozen peer |
| LINK-039 | New types, ops, kinds and values go only to a peer with their capability bit; the master maps values a protocol-1 peer doesn't know to the nearest v1 one and answers an unknown command type UNAVAILABLE; a keypad decodes an unknown arm state as armed and an unknown sounder as the alarm, sounds fire and CO from STATUS FIRE/CO, and reads an unknown `why` by its decade | future peer, `test_plink_view.c` |
| LINK-040 | Fields are appended only, and absence is known from a section's length; a configuration field is never widened in place; a link record never raises compat and stays at most 1024 B; new per-keypad facts go in new NVS keys | `test_plink_msg.c`, `test_plink_store.c` |
| LINK-041 | With keypads, a master's self-test needs one: `PANEL_OTA_OK_KEYPADS` (bit 1 << 6, `PANEL_OTA_HEALTH_COUNT` 7), required once the master registers its question (`panel_ota_set_keypads_check`), beats while a keypad that was online in the minute before "installing" (NVS `link/kup`) has a session again and sends heartbeats; with no such keypad it always beats. Until 180 s a keypad not back yet, or a pause in its answers, is waited for, never a failure; 10 s of answers prove it, so one that first answers by 170 s counts (`alarm_proto/ota_selftest.h`). A newer master none of whose keypads is back by 180 s goes back and records "no keypad answered" (NVS `ota`/`rbh`, `last.failed`) for its rolled-back page (`panel_ota`'s side built, BL-048; the link's question not yet) | `test_fw.c: test_selftest_keypads_come_back`, HIL |
| LINK-042 | CI runs the frozen v1 message fixture, a future peer and, once released, the frozen v1 binaries in both roles, including a v1 keypad updated on each path | CI |
| LINK-043 | Capabilities are chosen at run time: what a panel offers is an argument of `plink_master_init` and `plink_keypad_init`, masked with `PLINK_CAPS_KNOWN` (0x0007), and each session keeps its own offer AND the peer's, so an unknown bit is never shared (built). `PLINK_CAP_HOME` (0x0002) is offered by a master in a build with home control whose device registry runs, and by a keypad only with its mirror allocated. `PLINK_CAP_PREFS` (0x0004) by every build with the UI. No panel offers `PLINK_CAP_FW` yet. Types 0x32-0x3A go only in a session with their bit and only on a transport whose largest frame is 250 B or more, never 433 MHz. Every payload fits 225 B except DEV_LIST_PAGE, which fits the peer's frame (built, BL-216; on the two panels both sessions shared HOME and PREFS, 2026-10-16) | unit (`test_plink_session.c`, `test_plink_link.c`, `test_plink_msg.c`), frozen fixture |
| LINK-044 | A keypad mirrors its master's device list: a catalog blob pulled by `list_crc` and paged as the configuration, at most 64 devices (v1; more needs a new bit), decoded leniently. A blob that fails is not pulled again and asks for no update. The old list is kept until a new one installs, with each row's state carried over by ref. RAM only (built, BL-216; tried on the PC, and on the panels with an empty list only) | unit, link |
| LINK-045 | The master pushes changed device records at most one round per 500 ms per keypad (1 s on frames under 1,200 B), the full set at each new session and on DEV_STATE_REQ, and a summary every 10 s. A keypad whose `states_crc` differs asks after 250 ms, then at least 1 s apart, doubling to 30 s. A record applies only under the installed list and from a newer frame of the session for its device (LINK-026). On a working Wi-Fi network a keypad shows a change within 1 s of the master's own screen (built, BL-216; on the panels the summaries kept the keypad's device data 0-10 s old, a device's change not yet seen there) | unit, link, HIL |
| LINK-046 | While a keypad isn't ONLINE, or has had no device data for 30 s, every mirrored device reads offline since its last device data, as the master's do when Home Assistant drops (HCTL-006). Device data is data under the installed list: data under a list the keypad can't install keeps nothing fresh. A tap then sends nothing (built, BL-216; tried on the PC and in the preview) | unit, preview |
| LINK-047 | DEV_ACT names a device by ref, never by index; the ref and code are zero-padded, so every DEV_ACT has the same size. The master answers copies from its own cache (8 per keypad; an entry is reused 3 s after its last copy and kept up to 90 s) and runs an action at most once. The keypad resends every 500 ms for 3 s, never after, and shows an unanswered action as "Not confirmed" (HCTL-012). The keypad asks for the code itself; the master judges again (built, BL-216; tried on the PC and in the preview) | unit, link |
| LINK-048 | One judge for both screens (`device_service_person_act`). A keypad's action runs as `{KEYPAD, n}` with n from the session, its code checked for the keypad's home partition (decision 1, decided 2026-10-16). A wrong code counts toward that keypad's lockout and its STATUS shows it. Arm-only and guest codes are refused; Z-Wave rows are refused until BL-169 (built, BL-216: the judge in 8abf492, the keypads' actions through it in 787c2da; tried on the PC, not yet on a panel) | unit, HIL |
| LINK-049 | Device and settings traffic shares no cache, slot, bucket, task or turn with alarm commands, and goes only in a turn with no STATUS change or RESULT due for that keypad. With 64 devices changing every 100 ms, disarm and panic answer as fast as without it (built, BL-216; the storm run on the PC; on the panels, with HOME and PREFS shared and no devices, the keypad's commands answered in 150-190 ms) | link (storm), HIL |
| LINK-050 | A keypad with `PLINK_CAP_PREFS` reports its settings (field ids frozen in `proto_prefs.h`), its panel id, its voice state and the fields it accepts: at each session's first STATUS, after each save (at most 1 a second), and on PREFS_REQ. HEARTBEAT appends the settings CRC; the master asks again while it differs, at most every 2 s. The master keeps the report in RAM only (built, BL-216; on the panels a change's report reached the master within 1 s) | unit, link |
| LINK-051 | PREFS_SET carries only listed fields to change. One is in flight per keypad, resent every 1 s and given up at 6 s. The answer is a PREFS with its req, the refusals per field and the values now stored. A req is applied once (the answer is kept 30 s). The keypad checks every value itself and applies it through its screen (UI-046) (built, BL-216; on the panels from the master's console: on the keypad's screen within 1 s, refused as held during an exit time) | unit, link |
| LINK-052 | Never set through PREFS_SET: codes, role, pairing, home partition, name, supervision, Wi-Fi and IP, time, language and region, updates, factory reset, the setup, the Panel Manager, developer access. PREFS_SET and PREFS_REQ come from the master only (built, BL-216) | unit (`test_plink_frame.c`) |
| LINK-053 | Enrollment discovers nearby panels without Wi-Fi; only mutually confirmed, authenticated peers may transfer credentials, and only within 10 minutes of that keypad's pairing. Manual Wi-Fi never gets overwritten; failure retains pairing; completion needs a live master connection. Language/audio/display remain local. | codec/link unit tests, LVGL setup walk; hardware service acceptance passed (2026-10-18, dev.13); physical setup acceptance open |
| LINK-054 | A keypad's screen never waits for its master: its commands, code checks, code change, device actions and partition switches run on a task of their own (`plink_call`, through `ui_backend_t.run_later`), one at a time, a person's before the partition's activity, at most four waiting; the screen draws on meanwhile, a transparent layer takes the taps while a person's call waits (nothing on the screen changes), and the answer is handled on the screen's task, against the pad that is still open. The alarm panel's own screen, without `run_later`, runs them at once as before. The link task itself never waits (select at most 50 ms, hand-overs that never block). | PC (`calls_smoke`) |
| LINK-055 | A keypad's own wired terminals are its master's zone inputs (BL-222): a keypad with terminals - with a screen or without one - offers PLINK_CAP_IO and sends IO_STATE (0x3D, keypad to master): each terminal's debounced level and a count of its changes, at each change, every 5 s and when a session comes up; the master takes it only in a session holding the bit, rate-limited, and its zone source "Keypad input" (ZSRC_KEYPAD, address keypad << 8 | GPIO) reports an opening a lost frame hid, and makes a keypad's watched inputs unreachable while the link doesn't hear it (their zones CHECK after 60 s). Nothing of the alarm runs on the keypad. Frozen in `plink_io_fixture_v1.h` | unit (`test_plink_msg.c`, `test_plink_link.c`, `components/core/zone_hub/test/test_zsrc_keypad.c`); bench open |
| LINK-056 | An automation's arming warning reaches the keypads (BL-222; docs/features/41 AUT-012, AUT-013): a master with automations offers PLINK_CAP_AUTO, a keypad with a screen too; the master sends its whole warning table (AUTO_WARN 0x3E, at most 8, arm time as epoch and as ms from sending) at a change, to a new session and every 30 s while not empty; the keypad shows it on Home as the alarm panel's own screen does, chimes once a minute while its partition's warning is on, and forgets a table older than 75 s. Put off from a keypad is PLINK_OP_PUT_OFF (6): the code checked on the master as the keypad's (ALARM_CMD_CODE_CHECK, its lockout), then the put-off by its user; the RESULT's not_ready_zone carries auto_put_off_t. Matters most on a master without a screen (docs/features/42). Frozen in `plink_auto_fixture_v1.h` | unit (`test_plink_msg.c`, `test_plink_link.c`, `test_plink_frame.c`); panels open |
| LINK-057 | A keypad's own settings stay its own: Screen and sound, Wi-Fi and network, Time and language (its clock and time zone following its master, LINK-053), About, its software update and "Make it a stand-alone panel". Everything else on its Settings - users and codes, integrations, automations, Smart Home's settings, the walk test and the installer menu - is its master's, read from and saved on the master (BL-223; designed, not built) | unit, preview |
| LINK-058 | The remote backend: in a session with `PLINK_CAP_RB` (0x0040) a keypad calls its master's `ui_backend_t` entries by frozen call id (RB_CALL 0x3F, RB_ANSWER 0x40, RB_MORE 0x41, RB_GRANT 0x42); arguments and answers are encoded in sections, appended only, paged to the transport's frame; an unknown call is answered UNAVAILABLE; a repeat of a req is answered from a cache kept 30 s and never runs twice; never a call: the master's factory reset, setup, updates and update server, developer access, its own Wi-Fi, time and Screen and sound, the voice editor. The calls and their records are `components/core/ui_remote`'s, frozen in `ui_remote_fixture_v1.h` | unit (`test_ui_remote.c`, `test_plink_msg.c`), frozen fixture |
| LINK-059 | The master is the only judge: it runs each call on its own backend as `{KEYPAD, n}` from the session, under the grant it holds for that keypad (a code the keypad's code check, CMD op 2, had accepted on the master - a wrong one counts toward that keypad's lockout - or its installer code); each group needs its access (docs/features/14); user arguments are the grant's user; the grant ends after 5 min idle, when the keypad leaves the master's pages, or 60 s after its session is lost; answers never carry code material or a stored password or token | unit (`test_ui_remote.c`), link |
| LINK-060 | Installer mode from a keypad: entered with the installer code as `{KEYPAD, n}`, held by one source at a time (BUSY elsewhere, RB_GRANT names the holder), ended by that keypad, by the master's own screen or Panel Manager installer, 60 s after the master stops hearing the keypad, or after `ALARM_PROGRAM_TIMEOUT_S` idle; every panel's status bar says where it is held | unit, link, preview |
| LINK-061 | A configuration saved from a keypad carries the cfg_crc it was read at; the master refuses it STALE when its configuration changed since, and otherwise takes every field but code material, which it keeps; codes change only through `program_set_code` and the user calls | unit |
| LINK-062 | Learn and pairing modes a keypad starts (zone learn, Z-Wave, Zigbee, 433 MHz, another keypad) run on the master's radios and belong to the keypad's grant: they stop when the keypad stops them or leaves the page, when the grant ends, or at the mode's own limit; the keypad shows the master's progress, read at most once a second; PINs and install codes go sealed and are wiped after use | unit, link (`link_smoke`), preview |
| LINK-063 | A keypad's screen never waits for the remote backend: reads answer at once from the keypad's copy, asked again at most once a second while their page is open (a first read shows a steady "Getting it from Harbor House"); writes go through LINK-054's queue. While the link is down the master's pages are dimmed and send nothing; a write with no answer reads "Not confirmed" | PC (`calls_smoke`), preview |
| LINK-064 | Remote-backend traffic shares no cache, slot, bucket, task or turn with alarm traffic (LINK-049): one call and one read at a time per keypad, at most 5 calls a second (a burst of 10), dropped in the link task above that; disarm and panic answer as fast with an installer working on a keypad | link (storm) |
| LINK-065 | A keypad or master without `PLINK_CAP_RB` keeps today's role gate; a call group the master doesn't serve is left out of RB_GRANT's areas and its rows are hidden on the keypad | unit, future peer |

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
| 0x18 | `BEACON` | M->all | plain | net_id in header; payload: version 1, channel (1–13), pairing open (0/1), one byte each; every 100 ms while pairing is open, otherwise once a second while a keypad is paired |
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
| 0x2A | `HEARTBEAT` | K->M | sealed | flags u8 (bit 0 mains lost, 1 battery low, 2 battery present), rssi i8, uptime_s u32, status_age_s u16 (0xFFFF none yet), fw (≤ 31); in a session with `PLINK_CAP_PREFS`, an appended section with prefs_crc u32 (BL-216), read only when bytes remain |
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
Their codecs, goldens and frozen fixture are built (`plink_msg.h`), and so are the link core and
the services that send and take them (BL-216). The panels send 0x32-0x3A and the HEARTBEAT
section; nothing sends 0x2D-0x31 yet, as no panel offers `PLINK_CAP_FW` (LINK-043).

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
changes the code's own user's code (`change_code`). Op 4 selects an enabled partition
(KPD-006), without a code, when the installer allows switching; returning home is always
allowed. Op 5 reports interaction on the selected view. Selection belongs to this keypad's
session and returns home after its configured inactivity delay (default 30 s), unless Never is
set. Actual touchscreen/remote input renews it across every page; successful new commands also
renew it. Heartbeats and replayed results do not. Reconnect, reassignment and policy changes
clear it. The saved home and audio preferences do not change. Alarm operations run as
`{ALARM_SRC_KEYPAD, n}` with n from the session, never from the frame, so keypad semantics (quick
arm, auto-stay) and the per-source lockout apply (USR-005), on a worker that never holds the link
task (LINK-027). A command outside the keypad's current selection (home when no temporary selection is active) is answered
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

The last eight rows, from "Device data build", are built and host-tested (BL-216; "Smart Home
and Home Assistant on keypads"). On the panels the summary every 10 s kept the keypad's device
data 0-10 s old, and a settings change reached the keypad within 1 s (2026-10-16).

## Transports

| Transport | Peer address | Largest frame | Discovery | Status |
|---|---|---|---|---|
| UDP over Wi-Fi (LAN) | IPv4 + port | 1200 B (no IP fragments: lwIP reassembly is off) | mDNS `_roboalarms-link._udp`, port 6056, TXT `v`, `net`, `pair`, `id`; subnet broadcast to 6056 when mDNS finds nothing | built first |
| ESP-NOW (C6 bridge on the P4) | MAC | 250 B (v1) | `BEACON` on the master's channel | implemented; command/failover HIL passed (CP-003) |
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
  code checks and code changes, and the master's devices and their actions; locally: Wi-Fi,
  screen, sound (its Voice volume and Announcements too, LINK-028), language, updates, About. The
  master-only pages (installer mode, users, zone sources, integrations, event log, automations,
  the zones' spoken-name editor) are hidden by a role gate the UI reads (`ui_link_keypad`,
  `ui_voice_editor_present`); `program_enter` and `user_*` answer UNAVAILABLE. User editing never
  goes over the link. Smart Home (BL-216, built) shows under the master's rule, from the master's
  registry, with its own Arrange (LINK-044..049; "Smart Home and Home Assistant on keypads").
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
  code and offers at most its master's version (LINK-029), and the update gate decides on the
  master's newest status as for ONLINE.
- **Time**: the keypad follows the master's TIME (epoch and time zone; it ignores epoch 0) and
  runs no time server of its own while paired.
- **Wi-Fi power save** is held off through `net_wifi_hold_low_latency(owner, on)`, one hold per
  reason (link keypads, remote viewing), on a master with keypads and on a keypad.

## Updates through the link

Decided 2026-10-17 (Decisions, keypad updates): keypads follow their master. The master tells each
keypad which version to have; each keypad gets it, by itself from the update server when it can,
and restarts together with the master. Keypads never fall behind and never go past the master. The
owner asked for the simple way: no turn-taking, no buttons, no rule tables.

```
the master starts its own update to V (Install; later BL-190 automatic updates)
  every keypad behind V:  FW_ORDER FETCH{V, server, channel, key}
    the keypad downloads V itself (a check with want=V), checks it                -> READY
    it can't reach the server, or isn't offered V:
      FW_ORDER RELAY{V, size, sha256} -> FW_REQ / FW_PAGE from the master's image  -> READY
  the master's restart is allowed (everything disarmed):
    FW_ORDER INSTALL{V} to every READY keypad, then the master restarts; they restart with it
  the master passes its self-test -> INFO{V}: the keypads on trial are updated
  the master goes back to its old version -> INFO{old}: the keypads on trial go back too
a keypad found behind later (newly paired, back on line): FETCH or RELAY, then INSTALL when READY
```

### The rules
- **Target.** The version the master is installing (its own update, downloaded and checked) or,
  when it runs none, the version it runs. A keypad on the target or newer is left alone: nothing is
  ever sent back to an older version (OTA-014).
- **Getting the image.** FETCH goes to every keypad behind the target at once, as soon as the
  master has the target. The keypad downloads its own board's image from the update server, as the
  master does (`want` = V, FWS-028).
  - It answers with RELAY wanted when it can't reach the server (1-9), its download broke off or
    was too slow (20, 21), or the server offers another version, nothing newer or no release of V
    (13-15). The master then relays its image of V (the one it just downloaded, in its spare slot,
    or the one it runs) when the keypad is the same device, within the image's chip revisions and of
    a project it accepts.
  - Another model without internet: the master downloads that model's image itself
    (`panel_ota_fetch_for`, one at a time, in PSRAM) and relays it.
  - Only a refusal by the server (10-12, 16-19) leaves a keypad behind; it is asked again at the
    master's next version.
- **Downloading goes on while armed.** It pauses only while an alarm sounds, an entry or exit time
  runs, a panic is being sent, a fire or CO tone is kept on the keypad, or the link is down.
  Nothing restarts while anything is armed.
- **Restarting together.** While a keypad on line still downloads or receives V, the master's own
  restart waits, at most 30 min from when its own update was ready; its Software update says "Its
  keypads are getting the update" (decision 10). When its restart is allowed (everything disarmed,
  no alarm, OTA-004), it sends INSTALL{V} to every READY keypad, waits up to 5 s for their answers,
  and restarts. Each keypad's "installing" (`keypad_installing`) is logged on its INSTALL's echo, and
  the log is flushed before the master restarts. Each keypad restarts as soon as its gate passes. A
  keypad found behind while the master already runs V installs as soon as it is READY and its gate
  passes.
- **On trial with the master.** A keypad that installed V (kept across the restart in NVS
  `ota`/`ord`) stays on probation until its master runs V confirmed: an INFO naming V. An INFO naming an older version, or none within 10 minutes of its
  restart, sends the keypad back to its old version (its self-test fails). So no keypad stays past
  its master. Its own self-test (UI, touch, the link) applies as before.
- **Failures.** A failed download is tried again after an hour, and at the master's next version.
  A keypad that went back on V (its own self-test, or with its master) isn't asked for V again: it
  waits for the master's next version, so it never restarts in a loop.
- **The master's self-test needs a keypad back** (Decisions, keypad updates, 3; LINK-041). Keypads
  restarting with it come back on V, on trial, and count.
- **Nobody steps in** (decision 9). Nothing on any screen asks anyone to do anything: the people
  who live with a panel, and most installers (who are gone once it is installed), would not know
  what to do. Whatever the master can do itself, it does; what it can't, it tries again by itself.
- **Not done** (the simple way): no turn-taking between keypads, no first-keypad-per-model check,
  no Stop or Try again on any screen, no keypad that waits for a person. The console keeps `plink fw` (each keypad's state) and `plink fw update <n>` (ask again
  now).

### The keypad
- **Follows its master.** While paired with a master whose session has `PLINK_CAP_FW`, a keypad
  installs only at its master's order. It runs no scheduled check of its own
  (`panel_ota_set_checks(false)`); its Software update says "Updates come from <master>" and offers
  no Install and no Stop.
- **Never past its master** (Decisions, keypad updates, 5). INFO names the version the master runs;
  the keypad keeps it in NVS (`link/mver`) and caps its own ways to update at it with
  `panel_ota_set_version_cap(cap, version, master)`:
  - `PROTO_FW_CAP_AT`: its own Install stays only in LINK-025's maintenance and the needs-update
    state, and its check then sends `want` = that version; every other writer (`ota recv`,
    `tools/ota_serial.py`, a developer's file) refuses a newer image before its first flash write
    ("<master> runs 0.6.2: update it first");
  - `PROTO_FW_CAP_UNKNOWN`, paired but its master's version never heard: nothing is offered and
    every writer refuses;
  - `PROTO_FW_CAP_NONE`: no master, no cap (unpaired, or a stand-alone panel);
  - an ordered run is capped at the version its order names instead (the master's target), and on
    trial with the master (above);
  - only a cable (the USB bootloader) goes past, for development.

  A keypad already newer than its master (the master went back by cable) is never sent back.
- **The master's server and key** come with the orders (LINK-036, below).
- **Orders** (`plink_keypad.c`, `plink_keypad_take_fw_order`): an order id seen before is answered
  again, never run twice; unknown ops and kinds are refused; RELAY needs a size within the slot and
  a SHA-256; a RELAY with the run's `id8` continues it; FETCH or RELAY ends a run that hasn't started
  its restart.
- **FETCH** (`panel_ota_order_fetch`): a check with `want` = V at the order's server and channel
  (this run only); the offer must be V; the download into the other slot; READY.
- **RELAY** (`panel_ota_order_relay`, `PANEL_OTA_KIND_LINK`): the keypad pulls the bytes it lacks,
  in order, by image digest (`id8`) and offset (FW_REQ), within a 16 KB window, resent after 300 ms
  doubling to 2 s; it writes them with `panel_ota_write` from the link task (internal stack), flash
  busy at most half the time; it goes on across a new session and a master restart; it fails after
  60 s without a byte while not paused (STALLED) or 30 min paused (TIMEOUT, not counted while paused
  for an alarm, entry or exit time). A fetch that never reached the server, or that the server
  answered, isn't a failed run either (the keypad's 3 failed runs in 24 h count only transfers).
- **Checks, on both paths:** only the paired master's sealed frames; the header before any flash
  write (`proto_fw_header_judge`: magic, chip, chip revision range, project, newer, the cap); length
  and SHA-256; the signature with the keypad's keys (`esp_ota_end`); the version must be V.
- **READY** waits for INSTALL{V} ("Waiting for <master>"); it is discarded on CANCEL, another
  image's order, removal, or 24 h without INSTALL. An INSTALL taken stays taken through INFO and the
  master's UPDATING: it comes with the master's own restart.
- **The gate** before the restart, asked twice about a second apart: a live session with a status
  under 15 s old; the master's HOLD clear and `proto_fw_install_allowed()` (everything disarmed, no
  alarm); no panic still being resent; no fire or CO tone kept. UPDATING doesn't hold it: keypads
  restart with their master.

### The master
- **Planner.** `plink_fw_plan(world, now, steps)`, a small pure function run from the master
  service's poll. It sees the target and its image (own slot or spare slot), and each keypad's
  FW_INFO and FW_STATE, and sends FETCH, RELAY, INSTALL and INFO as the rules say. Orders are resent
  after 300 ms, doubling to 2 s, until a FW_STATE echoes them.
- **Serving FW_REQ.** Stateless (`on_cfg_req`'s way), only to a keypad holding a RELAY with that
  `id8`, armed or not and during the master's own update, several keypads at once, from a bucket of
  its own behind alarm traffic: 256 KB/s on UDP, 16 KB/s on frames under
  1,200 B, at most 16 pages a turn, read through `fw_read`: the running slot
  (`panel_ota_running_read`) or the spare slot (`panel_ota_spare_read`), on the link task (mapping
  a window disables the cache for a moment), or the copy in PSRAM (`panel_ota_copy_read`).
- **Its own restart** (`panel_ota_set_restart_with`, `plink_fwm_svc.c`): `hold` says what holds it
  ("Its keypads are getting the update", "A keypad is checking its update"), asked at every gate;
  once it may go, `installs_out` waits for the INSTALLs' echoes (5 s at most). The keypads on line
  when its restart came are kept in NVS `link/kup` for its self-test (LINK-041).
- **While a keypad restarts:** its supervision trouble isn't raised, for up to 10 min; a keypad that
  installs alone (it caught up later) holds the master's own restart until it is back
  ("A keypad is checking its update").
- **INFO** names the version the master runs, at each keypad session's start and when its confirmed
  version changes: the keypads' cap and their trial depend on it.

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
| 0x2D | `FW_INFO` | K->M | one section: can u8 (bit 0 fetch, 1 relay, 2 takes its master's key, 3 configuration unreadable), device (≤ 31), board_rev (≤ 15), chip_rev (≤ 3), chip_rev_full u16, project (≤ 32), version (≤ 32), slot u32, own_key8[8], accepted_key8[8] (zeros: none), cp_version (≤ 23) | 166 B |
| 0x2E | `FW_ORDER` | M->K | order section: order u32 (not 0: both sides refuse it), op u8, kind u8, version (≤ 32), size u32, sha256[32], key[32] (zeros: not known); fetch section: server (≤ 95), channel (≤ 7); relay section: page u16, count u8, gap_ms u16 | 219 B |
| 0x2F | `FW_STATE` | K->M | run section: order u32, op u8, state u8, flags u8, why u8, bytes u32, size u32, id8[8], target (≤ 32); last section: probation u8, last u8, last_version (≤ 32), failed_mask u8; probe section: asked u8, why u8, age_s u32, offered (≤ 32), offered_key8[8], key_trusted u8 | 144 B |
| 0x30 | `FW_REQ` | K->M | one section: id8[8], offset u32, page u16, count u8 | 16 B |
| 0x31 | `FW_PAGE` | M->K | id8[8], total u32, offset u32, len u16, len bytes (fixed, like CFG_PAGE; bytes after the data are ignored) | 18 B + data: 1,157 B on UDP, 207 B in 250 B |

**When each is sent:**
- **FW_ORDER INFO:** at each session's first STATUS and whenever the master's confirmed version
  changes, so a keypad always knows the version it may not pass.
- **FW_INFO:** at the first STATUS of each session, on a change, and on INFO; at most one every 2 s.
- **FW_STATE:** in answer to every order, on each change of state or `why`, at most once a second
  for `bytes`, and every 10 s while a run exists.

**Values** (never renumbered):
- **op:** 1 INFO (its `version` is the master's running version), 2 PROBE, 3 FETCH, 4 RELAY, 5
  INSTALL, 6 CANCEL.
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
| 10-19 | the server's answer | UNKNOWN_DEVICE 10, HELD_BACK 11, STEPPING_STONE 12, OTHER_VERSION 13, UP_TO_DATE 14, UNKNOWN_VERSION 15, NOT_OFFERED 16 (the update server's `unknown_version` and `not_offered`, answered only to a check with `want`, FWS-028), REFUSED 17 (a reason the panel doesn't know, on a check with `want`: a refusal) |
| 20-29 | transfer: retried, or the relay instead | BROKE_OFF 20, TOO_SLOW 21, BAD_SIZE 22, BAD_SHA 23, STALLED 24, TIMEOUT 25, NO_MEMORY 26, FLASH 27 |
| 30-49 | refused: no retry | KEY 30, CHIP 31, PROJECT 32, NOT_NEWER 33, SIGNATURE 34 (also not an image), TOO_BIG 35, UNKNOWN_KIND 36, UNKNOWN_OP 37, BUDGET 38, BAD_ORDER 40, WRONG_VERSION 41 (not the version ordered, or past its master's) |
| 50-59 | waiting | MASTER_BUSY 50, NOT_QUIET 51, PANIC_PENDING 52, KEPT_TONE 53, NO_STATUS 54, BUSY 55 |
| 60-69 | stopped | STOPPED 60 (a person at the keypad), CANCELLED 61 (the master) |
| 70 and up | unknown class: a later firmware's | |

In `alarm_proto`: `proto_fwsrv_why_t` (0-17, a check's outcome, `proto_fwsrv_check_why`) and
`proto_fw_why_t` (20-61), the same numbers as `PLINK_FW_WHY_*`; `proto_fw_why_name()` names each
in lower case (`stalled`), "unknown" for one it doesn't know. As `panel_ota` builds them, a check
with no server set, or none that makes an address, is NO_LOOKUP 2; an answer that broke off is
BROKE_OFF 20, one below the floor TOO_SLOW 21, one too long NOT_A_SERVER 5.

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

### Per transport

| Transport | FW_PAGE data | Relay of 3.6 MB |
|---|---|---|
| UDP over Wi-Fi | 1,024 B, 16 KB per request | paced by the keypad's flash: about 2 min |
| ESP-NOW, 250 B frames | 207 B (runtime clamp) | measured 3,739,648 B in about 11 min 49 s (5.3 KB/s), 2026-10-18 |
| 433 MHz | never | - |

- **Factory-C6 keypads cannot relay over ESP-NOW.** They first need the signed bridge installed.
  Bridge-equipped keypads use these same messages with the page size from the order; a complete
  radio-only relay passed on the 7" master and 10.1" keypad (2026-10-18).
- **The ESP-NOW bridge** must send control frames before bulk ones (CP-003).
- **No FW_* frame** goes on a transport whose largest frame is under 250 B.

### What people see
Simple, as the owner asked (2026-10-17); drawn as proposal P2, approved and built (docs/features/07
mockup, screens/panel-link.js D).
- **Master's Software update:** a Keypads list under its own update, one row per keypad: name,
  version and one status line ("Up to date", "Downloading · 45 %", "Receiving from this panel ·
  30 %", "Ready · restarts with this panel", "Restarting", "Not updated yet · tries again by
  itself", "Newer than this panel"; `ui_link_keypad_t.fw_state`, `fw_percent`, `fw`). No buttons,
  and nothing that asks anyone to act. Keypads and devices says "Updating" for a keypad restarting
  with its update, instead of "Not responding".
- **Master's Install question:** "... It restarts once everything is disarmed, then its keypads
  update with it."
- **Keypad's Software update:** "Updates come from <master>", its version, and while an update runs
  the run's line ("Downloading · 45 %", "Receiving from <master> · 30 %", "Ready · restarts with
  <master> once everything is disarmed") and stepped bar. No Install, no Check again, no Stop
  (`ui_link_info_t.fw_follows`, `fw_from_master`). About's Software update row: "From <master>".
- **Keypad's Home:** today's steady update banner while it downloads or restarts: "Updating ·
  restarts with <master>".
- **Event log, on the master.** `firmware_update` stages appended after `ALARM_FW_RESTART_PUT_OFF`
  (the catalog is in docs/features/01): `keypad_installing` 9, `keypad_updated` 10,
  `keypad_rolled_back` 11, `keypad_failed` 12 (arg2 = why, never 0); 13 stays unused; source
  {keypad, n}, n 1..`ALARM_MAX_DEVICES`.
- **MQTT:** `…/firmware` gains `keypads` [{device, version, state}]. Keys are added only.
- **Console:** master `plink fw`, `plink fw update <n>` (ask again now); keypad `plink fw`; on a
  development build's master, `ota copy` (the bench, below).
- Bars are stepped and words change at most once a second (UI-020).

### The bench (development builds)
The update server has releases only. For a bench, a development build's master takes images over
its service console (never on a screen):
- **`ota recv`** (`tools/ota_serial.py`): in a development build a developer's file leads the
  master's keypads as a release does: their target while it waits, relayed to keypads of its own
  model, and they restart with it.
- **`ota copy <size> <sha256>`** (`tools/ota_serial.py --copy`): another model's image into the
  master's copy in PSRAM (`panel_ota_copy_load_*`), as if `panel_ota_fetch_for` had fetched it;
  nothing is written to flash. The link judges its header first: another device's image, of a
  project a paired keypad accepts, for its chip revision and slot, newer than what that keypad runs,
  and the keypads' target (so it goes after the master's own `ota recv`, whose start frees any
  copy). Its length, SHA-256 and signature block are checked at the end; the keypad checks the
  signature itself. The copy is marked bench (`plink_fw_copy_t.bench`): kept while no keypad needs
  one, and relayed to that keypad at once, without a FETCH first.
- The master's restart waits until everything is disarmed: armed, it waits for the copy's ~7 min
  over the console.

### The master's update server and key (LINK-036)
Decided 2026-10-17 (Decisions, keypad updates, 1): changes made on the master go to its keypads.
- **What the master sends.** Every PROBE, FETCH and RELAY names the update server the master uses
  (the fetch section) and the key it trusts for the image (`key`).
- **What the keypad does.** When its trust record (`ota`/`trust`) names another server or key, it
  stores the order's as its own, as a server change does on a master, before it checks or
  downloads anything; it takes the order's channel too (`ota`/`chan`), so its own Install in
  maintenance asks for its master's release on the right channel. Only its paired master's sealed
  orders can change them.
- **Who changes them.** The installer, on the master only: the installer's code and a hold, never
  with the factory code (OTA-020, OTA-021). The master's event log records it (OTA-023); its keypads
  follow at their next order.
- **No key screen on a keypad.** A keypad still checks every image's signature with the key it
  holds (LINK-033). KEY_WAIT (state 4) and KEY 30 stay in the v1 wire: KEY 30 means an image signed
  with a key that neither the keypad nor its master's order names, a refused image (it waits for
  the master's next version).

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
panel as well." Designed 2026-10-16 (BL-216), and built: the wire (the codecs, the run-time
capabilities, `proto_prefs` and the frozen fixture; a97a745), the link core (`plink_home`, the
master's and the keypad's managers; ed58b12), the one device judge and the path of a change from
elsewhere through the screen (UI-046; 8abf492), the master's and the keypad's services, the
keypad's Smart Home page as proposal P1 shows, the PC simulator and the preview (787c2da;
"Where the code goes"). On the two development panels (2026-10-16, both speakers muted, the
7" master and the 10.1" keypad):
- the keypad and the master agree on HOME and PREFS;
- a settings change from the master's console reaches the keypad's screen and its report within
  a second, and one during an exit time is refused as held;
- the keypad's commands still answer in 150-190 ms;
- the master's summaries every 10 s keep the keypad's device data 0-10 s old (the keypad's
  `plink dev` showed an older age, its screen copy's, until 77e1f93; the link was right).

The device tiles were not tried on the panels: the bench master's registry was empty, as Home
Assistant was not connected. They were tried in the PC simulator and the preview only. Home
Assistant's side, the integration's 0.3.0, is built in `RoboAlarms/ha-addon` on a local branch,
not released and not yet tried with a panel (docs/features/23).

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
- A bit is offered only with the code that uses it: the services and `host/link_sim` offer HOME
  and PREFS as below (BL-216, 787c2da); no panel offers FW yet.

Who offers each bit (built):

| Bit | Name | Offered by the master | Offered by a keypad |
|---|---|---|---|
| 0x0002 | `PLINK_CAP_HOME` | Built with `CONFIG_ALARM_HOME_CONTROL` and its device registry running (`plink_service_set_home`, from `panel_app`). The list is empty until Home Assistant's catalog arrives, so the keypad's rail stays hidden, as on such a master. | Built with `CONFIG_ALARM_HOME_CONTROL` and its mirror allocated in PSRAM. Never on the 4" board. |
| 0x0004 | `PLINK_CAP_PREFS` | Every build with the UI | Every build with the UI |

Next free: bit 0x0010 and type 0x3D (LINK-053 uses WIFI 0x0008 and 0x3B/0x3C).

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
- **A session without HOME.** The keypad drops its list at the session's start, so Smart Home
  hides and no old list is kept, and pulls it again once a session has HOME.

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
   `s_cmd_mutex`). Wait at most 1 s, or 3 s with a code. Then wait up to 150 ms more
   (`DEV_RELEASE_MS`): an answer the link task takes in the turn the wait ends still reaches the
   pad, so it says "wrong code" or the lockout rather than closing as if the action went.
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

All of it is built (BL-216): the link core in ed58b12, the parts without the link in 8abf492
(stage S2a), the services, the screen and the PC's tools in 787c2da (stage S2b), and the keypad's
device data age in 77e1f93.

**`panel_link` (portable), host-tested:**
- **The wire:** run-time caps; the codecs in `plink_msg.h/.c` (DEV_LIST_* reuse
  `plink_enc/dec_cfg_req/_page`), with the device record and catalog record codecs and
  `plink_dev_act_digest`.
- **`plink_home.h/.c`, both sides:** the catalog blob from the registry's rows
  (`plink_home_list_build`, `plink_home_recs_build`), `list_crc` (never 0) and `states_crc`, the
  records by index (`plink_dev_recs_t`) and their values for a screen (`plink_home_values_of`), a
  keypad's lenient walk of a pulled blob (`plink_home_list_check`, `_next`, `_find`), and a row's
  record carried over by ref at an install (`plink_home_list_keys`, `plink_home_carry`).
- **The master (`plink_master.h`):** `plink_master_home` takes the list and its records from one
  copy of the registry; `port.dev_run` runs an action off the link task and
  `plink_master_dev_answer` brings its answer back; `plink_master_prefs_set` starts a write and
  `port.prefs_done` ends it; `plink_master_keypad_home` gives a keypad's caps, whether it is online
  and its settings report. Each keypad's state is a `plink_home_peer_t` in its `plink_peer_t`.
- **The keypad (`plink_keypad.h`):** `plink_keypad_home_attach` gives it the mirror
  (`plink_dev_mirror_t`, the caller's memory; HOME is offered only with one); one `plink_pull_t`
  each for the configuration and the list, which a session without HOME drops.
  `plink_keypad_dev_info`, `_dev_row`, `_dev_rec` and `_dev_overlay` read the mirror, and
  `plink_keypad_dev_view` gives a row as the screen shows it (the record, this keypad's own action
  over it once released, offline while the data is stale); `plink_keypad_dev_act`,
  `_dev_act_state` and `_dev_act_release` run an action; `plink_keypad_prefs_report`,
  `plink_keypad_take_prefs_set` and `plink_keypad_prefs_answer` carry the settings.

**On the panels and the PC:**

| Area | What it does |
|---|---|
| `alarm_proto` | `proto_prefs.h/.c` (a97a745): the settings fields' table, the TLV list and its CRC, and Home Assistant's `values` writer and parser. `proto_link`'s `settings`, `keypads`, `settings_set` and `settings_result` (8abf492; docs/features/23 HAI-016..019). |
| `plink_service`, the master | `plink_home_svc.c`: the `plink_dev` task (`alarm_mem_task_create`, a 4 KB PSRAM stack, priority 3, below `plink_work`'s 4) builds the list and its records into two alternating PSRAM buffers, and runs the keypads' device actions (8 job slots) through `device_service_person_act`; the build after an action (RES_DEVICES) is queued before its answer (RES_DEV_ACK). `plink_service_set_home(registry_runs)`, called by `panel_app`: HOME is offered only with a running registry. `plink_service_prefs_set()`: never blocks, a 4-deep queue to the link task, exactly one `done` once queued. `plink_service_keypads()`, under `link_pub_lock` from what the link task publishes every 0.5 s: Home Assistant's keypads list, in which a keypad online in a session without PREFS is listed with its id and settings null. `g_pub` gains each keypad's signal, caps and settings report. Console: `plink dev [n]` and `plink prefs <n> [set <key> <v>]` (`set` in development builds only). |
| `plink_service`, the keypad | `plink_keypad_svc.c`: the core's mirror and the screen's `ui_dev_t[64]`, all PSRAM, allocated once at the start (HOME is offered only with them). The screen's copy is made again under its own `s_dev_lock` (never `link_pub_lock`) whenever the mirror moves; its device data's age is copied every link turn (77e1f93). `ksvc_dev_act`: its own box, mutex and semaphore; 1 s, 3 s with a code, then 150 ms of grace ("Device actions" step 5). The settings report when `panel_prefs_seq()` or the voice's state moves; a change from the master through the screen's mailbox (UI-046). Console: `plink dev [n]`. |
| `plink_backend.c` | `be->dev_list = kp_dev_list` and `be->dev_act = kp_dev_act` on a keypad with the mirror; `ui_port_esp`'s layout entries stay (Arrange per panel, decision 2). Set before `ui_app_start`, because `ui_devices_init` returns early without `dev_list`. |
| `device_service` | `device_service_seq()`, moved on every change a reader may need; `device_service_person_act()`, the one judge, returning `ds_person_t` with the numbers of `ui_dev_result_t` (asserted) (8abf492). |
| `home_control` | `hc_mode_word(bit)` (8abf492) |
| `ui_backend` | `ui_dev_from_parts()` in `ui_dev_model.h`, byte-identical to the master's old conversion over 21 rows; `ui_prefs.h`: `UI_PREFS_F_*` (= the proto ids, asserted), `UI_PREFS_LOOK`, `ui_prefs_merge`; the entries `prefs_remote_take` and `prefs_remote_done` (8abf492). |
| `ui_port_esp` | `dev_list` through `ui_dev_from_parts`, `dev_act` a wrapper over `device_service_person_act`, and the two `prefs_remote_*` entries for both roles (8abf492). |
| `platform_esp` | `panel_prefs_seq()`, `panel_prefs_current()`, `panel_prefs_remote_submit`, `_take`, `_done` and `_cancel`. The seq and RAM copy are updated in `panel_prefs_save`, so every writer is caught (8abf492). |
| `ha_link` | The `settings` and `keypads` pushes, the `settings_set` queue and the settings port (`ha_link_set_settings`); the master's own settings go to the screen's mailbox (8abf492). |
| `panel_app` | `plink_service_set_home` from the registry's start, and `ha_link`'s settings port set onto `plink_service_keypads` and `plink_service_prefs_set` once the link runs (787c2da). |
| UI | The keypad's rail shows Smart Home under the master's rule, as P1 shows (`ui_devices.c`, `ui_link.c`); the note "Your devices can't be reached right now" takes room from the tabs, so it is never cut (787c2da). `prefs_remote_tick` in `ui_settings.c` and `ui_app_held()` (8abf492). Integrations and Automations stay master-only. |
| `host/` | `link_sim`: home_control's registry over a fake Home Assistant source on the virtual master (`devices`, `device REF online\|offline`, `house off\|on`), Smart Home and settings on the virtual keypad; `link_smoke`: one device action and one settings change end to end; the preview's `host_link_demo`: the keypad's Smart Home live, with the link lost and "Not confirmed" (787c2da, 77e1f93). `prefs_smoke` walks UI-046 (8abf492). |

The screens still poll the devices every 1 s (`ui_devices.c`, UI-023); `device_service_seq()`
tells the master's `plink_dev` task when to build.

### Memory

Measured on both panels (787c2da, 2026-10-16):
- **Internal RAM:** +264 B of statics at start-up on both boards. At run time the keypad's free
  internal RAM is about 355 KB, and the master's about 167 KB with HOME offered.
- **Image size:** 76.5 % of the slot on the 7" and 77.9 % on the 10.1", under the 85 % limit.

Estimates, not measured:
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

Sizes are exact from the field widths. Latencies other than those measured on the panels, ESP-NOW
air time and the PSRAM figures are estimates. A keypad unlock's code check holds the alarm task
for about 150 ms, as any keypad code does: a disarm at that moment waits for it, within
LINK-024's 1 s. During packet loss a keypad's tap is answered within 1 s (3 s with a code) and a
command within 3 s (`CMD_WAIT_MS`, `plink_keypad_svc.c`); the screen goes on drawing meanwhile
(LINK-054). This design and "Updates
through the link" freeze together: `OWN_PAYLOAD_MAX` (225) and the v1 fixture serve both, and
freeze at the first release. Home Assistant's changes to how a screen looks are rate-limited on
the panel, never trusted to Home Assistant (WCAG 2.3.1, "Display safety" in AGENTS.md).

## The master's settings and installer menu on keypads (BL-223)
The owner (2026-10-21): with a headless master in a closet, every keypad must be a full front end
to it, so an installer can set the whole system up from any keypad - zones, users, sirens,
devices on every radio, automations, Smart Home, integrations, the installer menu - as if standing
at the master. Network, time, language and About stay each keypad's own. **Designed and
approved** (2026-10-21, proposal `docs/features/07-UI-LVGL-Pro/mockup/proposals/BL-223-keypad-master-settings.js`;
its five questions in Decisions); being built.

### What people get
- A keypad's Settings list has two groups: **This keypad** (Screen and sound, Wi-Fi and network,
  Time and language, About) and **Harbor House** (the master's name: Users, Integrations,
  Automations, Smart Home, Walk test, Installer). The second group's pages are the master's
  pages, the same rows and words as on a master with a screen, read from and saved on the master.
- An installer types the installer code on any keypad and gets the master's installer menu:
  zones, partitions, keypads and devices, delays and arming, codes, sirens and outputs, Z-Wave,
  Zigbee and 433 MHz. Adding a zone, listening for a sensor, pairing a Z-Wave or Zigbee device,
  a 433 MHz siren or another keypad all run on the master's radios; the keypad shows the
  master's progress.
- Installer mode is one place at a time. While a keypad holds it, every panel's status bar says
  so ("Installer at Keypad 2 · Arming off"), and the master's own screen, if it has one, or its
  Panel Manager can end it.
- A few things stay on the master itself or its Panel Manager: its factory reset, running its
  setup again, its update server and channel (LINK-036), its developer access and, in this first
  version, a zone's spoken-name editor.
- While the master can't be reached, its pages on the keypad stay on screen, dimmed, with the
  line "Harbor House can't be reached right now", and nothing can be changed. The keypad's own
  pages work as before.

### How it works: the remote backend
The touchscreen reaches everything through `ui_backend_t` (`components/core/ui_backend`). On a
master, `ui_port_esp` fills it from the panel's services, and the Panel Manager wraps the same
backend, on a headless master too. On a keypad, the entries for the master's pages become
**proxies**: each one sends a call over the link, and the master runs the same entry of its own
backend and sends the answer back. So the keypad runs the master's UI code unchanged (one UI,
ADR-036), and the master judges every call with the same functions its screen and its Panel
Manager use: installer mode, the partitions' state, codes and lockouts.
- **Calls** have frozen ids (`ui_remote.h`), grouped by area. Each call's arguments and answer are
  encoded field by field in sections (LINK-017, LINK-040), never as a C struct, so a keypad and a
  master on different versions still agree. Strings carry their length.
- **Reads** (status, lists, a device's page, progress) are answered on the keypad at once from
  its copy of the master's last answer. While a page is open, the keypad asks for each of its
  reads again in the background, at most once a second. A page's first read waits for the
  answer; until then the page shows a steady "Getting it from Harbor House".
- **Writes** (save, bind, start pairing, stop) go through LINK-054's queue: on the keypad's call
  task, with the transparent layer taking the taps, the answer handled on the screen's task. The
  screen never freezes.
- **The configuration** the installer edits is the keypad's copy (CFG pages, LINK-019). A save
  sends the whole edited copy with the cfg_crc it was read at. The master refuses it if its own
  configuration changed since then. Otherwise it keeps its own code material and takes every
  other field.
- **Long answers and arguments** (a device list, the configuration) are paged to the transport's
  largest frame, as CFG_PAGE.

### Messages
| Type | Name | Dir | Bit | Payload (v1) |
|---|---|---|---|---|
| 0x3F | `RB_CALL` | K->M | RB | req u32 (not 0); call u16; part section (offset u16, total u16); args (len u16 + bytes) |
| 0x40 | `RB_ANSWER` | M->K | RB | req u32; status u8; total u32, offset u32; answer (len u16 + bytes) |
| 0x41 | `RB_MORE` | K->M | RB | req u32, offset u32, max u16: the next part of a long answer |
| 0x42 | `RB_GRANT` | M->K | RB | grant section (user u8, authority u8, partitions u8, installer u8, idle_left_s u16); holder section (installer mode's source: kind u8, id u16); areas u32 (the call groups this master serves) |

`PLINK_CAP_RB` is 0x0040, offered by a master whose backend serves the calls and by a keypad with
a screen. Statuses: OK, UNAVAILABLE (an unknown call, or one this master doesn't serve),
NOT_AUTHORIZED (no grant for it), NEED_INSTALLER, BUSY (installer mode held elsewhere, or a call
already running), LOCKED_OUT, STALE (the configuration changed since it was read), TOO_BIG, GONE
(the answer is no longer kept: call again).

The call groups (ids frozen once released; the full list is in `ui_remote.h`):

| Ids | Group | Calls (the `ui_backend_t` entries) | Needs |
|---|---|---|---|
| 0x0001-0x000F | Session | end the grant, `program_enter`, `program_exit`, `program_keepalive`, `program_set_code` | the installer code for installer mode |
| 0x0010-0x001F | Configuration | `set_config` (with its base cfg_crc) | installer mode |
| 0x0020-0x002F | Users | `user_set`, `user_update`, `user_delete`, `web_login_get`, `_set`, `_clear` | a master code (docs/features/14) |
| 0x0030-0x003F | Zones' sensors | `zone_sources`, `zone_learn`, `zone_candidates`, `zone_bind`, `zone_unbind`, `zone_info` | installer mode |
| 0x0040-0x004F | Z-Wave | `zw_status`, `zw_devices`, `zw_device`, `zw_request`, `zw_progress`, `zw_prov`, `zw_prov_register`, `zw_prov_change` | installer mode |
| 0x0050-0x005F | Zigbee | `zb_status`, `zb_devices`, `zb_device`, `zb_request`, `zb_progress`, `zb_codes` | installer mode |
| 0x0060-0x006F | Sirens, outputs, 433 MHz | `out_status`, `out_wired`, `out_wired_set`, `out_wired_test`, `out_terminals`, `out_rf`, `out_rf_add`, `_set`, `_clear`, `_pair`, `_test` | installer mode |
| 0x0070-0x007F | Automations | `auto_list`, `auto_gen`, `auto_read_only`, `auto_less_secure`, `auto_save`, `auto_delete_slot`, `auto_set_off`, `auto_status`, `auto_try` | a master code |
| 0x0080-0x008F | Smart Home settings | `dev_home_get`, `dev_home_set` | a master code |
| 0x0090-0x009F | Integrations | `mqtt_*`, `halink_*`, `notify_*`, `ntfy_*`, `pushover_*`, `remote_get`, `remote_set` (the Panel Manager only, never developer access) | a master code |
| 0x00A0-0x00AF | Keypads | `link_keypads`, `link_request`, `link_pair_status` (the master's keypads page; pairing) | installer mode |
| 0x00B0-0x00BF | Event log | `log_read`, `log_loss`, `log_clear_loss` | a master code |

The walk test needs no call: it already goes as CMD's walk operations.

Never a call: the master's `factory_reset` and `setup_done`, `fw_*` (its updates, update server
and channel), developer access, the master's own Wi-Fi, address, time, time server and clock,
language and asset packs, its Screen and sound (each panel's own; PREFS), storage actions, and
the voice editor (`voice_seen`, `voice_apply_approve`).

### Rules
- **One judge.** The master runs a call on its own backend, as the keypad's source `{KEYPAD, n}`
  (n from the session, never the frame). It checks the call's group against the grant it holds
  for that keypad, never against anything the keypad says. Every user argument (`user` in
  `auto_save`, `fw_install`...) is replaced by the grant's user.
- **The grant.** A code typed at the keypad for a Harbor House page is checked on the master by
  the keypad's code check (CMD op 2, `authenticate`), as today. A wrong one counts toward that
  keypad's lockout (USR-005), as a code at the master's own pad does. A right one gives a grant: the user, their authority and partitions. It ends after 5
  minutes without a call, when the keypad leaves the master's pages, and 60 s after the keypad's
  session is lost (a new session within 60 s keeps it). RB_GRANT tells the keypad what it holds.
- **Installer mode.** `program_enter` from a keypad enters the engine's installer mode as
  `{KEYPAD, n}` (`program_src`), with the usual event. Only one holder at a time: a keypad that
  asks while another source holds it gets BUSY, and RB_GRANT names the holder. The master's own
  screen and its Panel Manager's installer may end a keypad's installer mode. A keypad sends
  `program_keepalive` every 30 s while a page of it is open. The master ends a keypad's
  installer mode 60 s after it stops hearing that keypad, and after 15 minutes without activity as
  today (`ALARM_PROGRAM_TIMEOUT_S`).
- **Learn and pairing modes run on the master** (zone learn, Z-Wave inclusion and exclusion,
  Zigbee joining, 433 MHz pairing, keypad pairing). Each one a keypad started belongs to its
  grant: it stops when the keypad stops it or leaves the page, when the grant ends, or at the
  mode's own limit, whichever comes first. A PIN or install code typed for it goes sealed and is
  wiped after use, as codes are.
- **Pairing another keypad from a keypad** (LINK-004): the installer in installer mode at a
  paired, online keypad opens pairing on the master. The comparison code shows on that keypad
  through the link. The new keypad confirms on its own screen as today.
- **Secrets are write-only.** Answers never carry code material (LINK-019) or a stored password
  or token (MQTT, notifications, the Panel Manager's sign-in). A field that has one says only
  that one is set, as the master's own pages show it.
- **Alarm traffic always wins** (LINK-049): RB traffic shares no cache, slot, bucket, task or
  turn with alarm commands, STATUS or RESULT. One call runs at a time per keypad, plus one read.
  At most 5 calls a second per keypad (a burst of 10); over that, calls are dropped in the link
  task. An answer is kept 30 s, keyed on (keypad, req): a repeat gets the kept answer and never
  runs again (LINK-018).
- **Link down.** Pages from the master stay on screen, dimmed, with the note "Harbor House can't
  be reached right now". Nothing is sent. A write with no answer reads "Not confirmed", never
  "Saved", and is safe to try again. When the link is back, the open page reads again.
- **Versions.** A keypad or master without `PLINK_CAP_RB` keeps today's role gate (the master's
  pages hidden on the keypad). A group the master doesn't serve (not built, or a NULL entry) is
  left out of RB_GRANT's areas, and the keypad hides its rows, as a NULL entry hides them on a
  master.

### Where the code goes
- `components/core/ui_remote` (new): the call ids, every call's arguments and answer as records
  described by tables of their fields, the master's side (`ui_remote_serve` under a keypad's
  grant) and the keypad's proxies (`ui_remote_proxy` over a port). Frozen fixture
  `ui_remote_fixture_v1.h`.
- `components/core/panel_link`: the four messages and the transport in the link managers: the
  answer cache, paging, RB_GRANT, and the keypad's read copy and pending call.
- `components/services/plink_service`: `plink_rb_svc.c` runs the master's calls on its worker
  through the master's `ui_backend_t` (`ui_port_esp_backend` with the keypad as the source); the
  keypad's proxies in `plink_backend.c`.
- `components/ui/alarm_ui`: the role gate becomes per group (RB_GRANT's areas); the Settings
  list's two groups; the "Getting it", "can't be reached" and "Not confirmed" states;
  installer mode's holder in the status bar; writes from the master's pages through `ui_calls.c`.
- `host/link_sim`: the virtual master serves the calls from the PC simulator's backend;
  `link_smoke` adds a zone and pairs a simulated device from the virtual keypad.

### The owner's five questions (decided 2026-10-21, as recommended; in Decisions)
1. Installer on a keypad sits in the Settings list under Harbor House (recommended), not under
   the keypad's About as on a master (where About and Installer are the same panel's).
2. Kept on the master and its Panel Manager only: its factory reset, its setup, its update server
   and channel, developer access, and in this first version the zones' spoken-name editor
   (recommended).
3. Time: the keypad's clock and time zone keep following its master, as decided with LINK-053 on
   2026-10-18; its 12- or 24-hour clock, language and region stay its own (recommended).
4. Installer mode at one place at a time; the master's screen and Panel Manager may end a
   keypad's (recommended).
5. Another keypad may be paired from a keypad in installer mode (recommended).

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
- With Smart Home and Home Assistant on keypads (BL-216, built), a device with a keypad's keys
  can also do in Smart Home what a person at that keypad can: switch what Home
  Assistant's options allow, and unlock or open only with a valid code for the keypad's home
  partition (lockout after 5).
- It can also forge that keypad's PREFS, which changes what the master and Home Assistant show
  about it, never another panel's settings.
- With the master's settings on keypads (BL-223, designed), a device with a keypad's keys can
  read and change the master's settings only with a code the master accepts - a master code for
  users, integrations and automations, the installer code for the installer menu - under the
  keypad's lockout. It never gets a code, a code hash or a stored password, and can't reset,
  update or set up the master, or turn on its developer access (LINK-058, LINK-059).

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
- The v1 wire of updates, Smart Home and settings (built, on the PC; nothing sends the updates'
  types yet):
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
- The link core of Smart Home and settings (BL-216; built and host-tested in ed58b12, with
  787c2da's and 77e1f93's additions):
  - `test_plink_home.c`, 9 tests: the blob from the registry's rows (a row whose ref the wire
    can't carry left out of the blob and the records alike), at most 64 rows within
    `PLINK_DEV_BLOB_MAX`, `list_crc` never 0, a newer master's blob read leniently and a newer
    compat refused, the records' flags and `states_crc`, the registry's values given back from a
    record's bytes (`plink_home_values_of`), records carried over by ref.
  - `test_plink_master.c`, 21 tests (`test_home_needs_the_capability` to
    `test_home_frames_per_tick`): nothing without the bit or on frames under 250 B; the list's
    pages; the full set at a new session; changed records in rounds, the summary every 10 s (an
    empty list's too, `test_dev_summary_of_an_empty_list_every_10_s`), state requests; a list
    change named at once and again at +250 ms and +1 s; an action run once from its cache, one at
    a time from its own bucket; DEV_ACK's record as it is when it goes; an unlisted ref REFUSED,
    no list UNAVAILABLE; no code kept; the settings report, PREFS_REQ while the CRC differs,
    writes, and `port.prefs_done` starting the next write; all of it behind a waiting STATUS or
    RESULT and the engine's first snapshot, at most 2 frames per keypad and 8 per tick.
  - `test_plink_keypad.c`, 23 tests (`test_home_offered_only_with_a_mirror` to
    `test_prefs_set_applied_once`): HOME offered only with a mirror; nothing of either capability
    without its bit or on frames under 250 B; the list pulled, installed whole, a list that fails
    remembered and read offline 30 s after its last data, records carried over by ref, the list
    dropped at a session without HOME and pulled again once HOME is back
    (`test_dev_list_dropped_without_home`); records in frame order and only under the installed
    list, kept as they came; the catch-up; freshness at 30 s, each summary moving the data's age
    but not its seq; actions refused at once, resent every 500 ms, given up at 3 s, answered late,
    in 2 slots apart from the commands; a row as the screen gets it (`test_dev_view_for_the_screen`:
    the record, this keypad's own action over it once released, every row offline since the last
    data once stale); list and state requests held while a panic is sent; the settings report's
    timing, the HEARTBEAT's section, a PREFS_SET applied once.
  - `test_plink_link.c`, the real master and keypads together: `test_home_and_settings_end_to_end`,
    `test_home_list_and_states_converge_through_loss` (10 % loss, 2 % copies, 1 % damaged, 300 ms
    of jitter, the list itself changing every 20 s), `test_home_converges_on_frames_of_250`,
    `test_a_device_action_runs_once_through_loss`,
    `test_a_renamed_or_reordered_list_never_acts_on_another_device`,
    `test_a_master_restart_during_a_pull_never_mixes_lists`,
    `test_a_45_s_cut_shows_offline_since_the_cut_and_replays_nothing`,
    `test_an_empty_list_stays_fresh_on_summaries` and
    `test_twenty_idle_devices_stay_fresh_on_summaries` (2 minutes on summaries alone, never more
    than 10 s without a DEV_STATE, the data never stale),
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
- The rest of Smart Home and settings on the PC (BL-216; built in 8abf492 and 787c2da):
  - `home_control/test`: `hc_mode_word`. `ui_backend/test`: `ui_dev_from_parts` byte-identical
    to the master's old conversion over 21 rows in every status (no regression on the master);
    `ui_prefs_merge` (`test_ui_prefs.c`).
  - `alarm_proto`'s `link` suite (`test_link.c`): the goldens of `settings`, `keypads` and
    `settings_result`, the `settings_set` parser, and the set queue (HAI-019).
  - `prefs_smoke` (in CI): the real touch UI takes a change from elsewhere through its own copy:
    IN_USE, HELD, VOICE_NOT_READY, RANGE, the look and the backlight at most once per 2 s, the
    mailbox's BUSY and cancel (UI-046).
  - Preview: `host_link_demo` with devices in the keypad role: `bl-216-kp-devices`,
    `bl-216-kp-device-unlock`, `bl-216-kp-devices-unconfirmed` and `bl-216-kp-devices-lost`; the
    walk passes with no cut-off text (444 shots). CI: `link_smoke` with home_control's registry on
    the virtual master, one device action and one settings change end to end.
  - Still to come: the future peer (`link_peer.c`, `-DPLINK_FUTURE_TEST=1`, LINK-039 and
    LINK-042): a v1 keypad (caps 0x0001) never receives type 0x32 or above, and Home Assistant
    gets `unsupported` for it; a newer keypad with a caps-0x0001 master sends nothing new; bit
    0x8000, type 0x3B, field id 25, kind 10, action 13, result 9, why 20 and catalog compat 2 are
    each handled as "Versions" (in "Smart Home and Home Assistant on keypads") says.
- Smart Home and settings on the two development panels (BL-216, 2026-10-16, with 787c2da; both
  speakers muted; the 7" master and the 10.1" keypad; Home Assistant not connected, so the
  master's registry was empty and offered an empty list):
  - the keypad and the master agree on HOME and PREFS;
  - a settings change from the master reaches the keypad's screen and its report within a
    second; one during an exit time is refused as held;
  - the keypad's commands still answer in 150-190 ms;
  - the master's summaries every 10 s keep the keypad's device data 0-10 s old (the keypad's
    `plink dev` showed an older age, its screen copy's: fixed in 77e1f93);
  - internal RAM at start-up +264 B on both boards; at run time the keypad's free internal RAM
    about 355 KB, the master's about 167 KB with HOME offered;
  - not tried there: the device tiles (only in the PC simulator and the preview).
- Smart Home and Home Assistant on keypads, still to do on the panels with Home Assistant
  connected: the 7" master and the 10.1" keypad, both speakers muted. Flash only on the word of
  the person you work for in that session, with each panel disarmed; ports and silicon come from
  `PANEL.local.md`. Home Assistant is paired with integration 0.3.0 and allows a dimmable light,
  a switch, a lock, a cover, a thermostat and a scene.
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
  9. `plink_dev`'s stack high-water mark after the storm.

### Temporary partition selection implementation (KPD-006)
The behavior approved 2026-10-18 by the owner is a switcher below Home's left status card when multiple
partitions are enabled and switching is allowed on that panel. A locked panel keeps today's
Home layout, with no partition control, label, or lock notice below the left card.
Each panel has installer-only default-partition, switching-allowed/locked,
and return-delay settings: Never or a delay in seconds, default 30 seconds of inactivity anywhere
on the panel. Never disables the inactivity return without changing the saved default partition;
restart/reconnection and installer reassignment still reset temporary selection. A Never switch
sits directly below the seconds control on the same policy screen. On disables the seconds
control; off re-enables it with its previous value. There is no separate delay-selection screen.
Navigation, scrolling, tapping and code entry reset that timer, including on Activity and other
pages; it is not elapsed time since the partition was selected. Selecting an enabled
partition requires no code. This changes the displayed/command-target partition, not the normal
authorization of arm/disarm commands or any audio preferences. The master must enforce the
per-panel switching policy, including console requests, rather than rely on a hidden UI control.

The switcher's subtitle is consistently **Switch partition**, naming the action rather than
explaining a gesture or calling the view temporary (which need not time out when Never is set).
This follows [NN/g's concise, consistent command-label guidance](https://www.nngroup.com/articles/ui-copy/)
and [GOV.UK's advice to avoid help text explaining the interface](https://www.gov.uk/service-manual/design/designing-good-questions).
These sources inform the wording; the exact phrase is this project's design choice.

The touchscreen implements the approved
[partition-switch mockup](../07-UI-LVGL-Pro/mockup/screens/partition-switch.js).
Installer > Keypads and devices > This panel (or a paired keypad) sets the default, switching
permission, seconds and Never toggle. Locked is the migration default. The switcher appears
below Home's left card only with switching allowed and multiple enabled partitions. Switching
closes any old code prompt and refreshes the current view without writing local preferences.

STATUS appends optional `selected` u8 after `mono_ms` (0/absent means home), then optional policy
flags u8 (bit 0 allow switching, bit 1 Never) and idle seconds u16 (0 means 30). The peer store
appends those same three policy bytes after the name in each length-delimited peer record.
Absent policy means locked, auto-return, 30 seconds. Original fixtures remain unchanged;
`status_policy_fixture_v1.h` and `plink_store_policy_fixture_v1.h` freeze the extensions.
Older receivers ignore the tail; older masters refuse unknown operations.

The bench console supports `plink policy <device> allow|lock <seconds|never>` on the master and
`plink cmd goto <partition>` on a keypad. Alarm commands remain authorized for the selected
partition; announcements, supervision and Smart Home retain their saved-home scope.
Host tests cover no-code selection, lock enforcement, command isolation, interaction renewal,
expiry, Never, replay and policy revocation. The focused `ALARM_PREVIEW_PARTITION` walk covers
Home/chooser/Activity and installer policy. Live acceptance must use matching firmware on both
panels; a successful host preview is not hardware acceptance.

## Open questions
- Keypad-attached inputs (LINK-011): wired terminals built (LINK-055, BL-222); a keypad's 433 MHz receiver, Z-Wave and outputs on its terminals are open.
- A link keypad's supervision trouble uses its assigned home partition, supplied explicitly to
  the engine (LINK-007). Trouble, restore and remembered trouble retain the scope captured at
  loss, even if the assignment changes before recovery. A later loss uses the new home.
  Legacy external devices with a disabled device-table slot ignore that slot's stale partition.
  Trouble names still identify the keypad by number; a shared name lookup remains open.
- LINK-001's number shared with Zigbee: the link chooses from Zigbee's numbers as its worker read
  them (at most a second old: the zone lock is never taken on the link task, LINK-027), reserves
  the chosen one with Zigbee at once and reads Zigbee's numbers again without waiting before the
  pairing is answered (a clash ends the pairing, "Nothing was saved"). Left open: a Zigbee keypad
  given the same number within that second while the zone lock is busy at the moment of the check.
  A Zigbee-side call that claims a number under the zone lock would close it.
- The setup's keypad manager starts while the panel still runs its master service (Option C).
  Before authenticated provisioning it persists the pairing and keypad role (LINK-053). Done
  ends setup and restarts into the keypad service; leaving before confirmation remains cancellable.
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
- Keypad updates (its four choices decided 2026-10-16, in Decisions). Estimates, not measured
  yet: the relay's speed, `esp_partition_mmap` windows of the running slot, the slot's exact length
  with its signature sector, free PSRAM on a master for a copy.
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
- [x] Muted bench checks: keypad voice/status/chime switches and Voice volume 0 applied and
  restored; 43 s without STATUS reached No system communication and automatically recovered.
  Speakers remained off, displays enabled. This verifies settings/transport, not audible output.
- [x] Multi-partition supervision: a missing link keypad reports against its assigned home;
  restore retains the loss's partition. Host regressions cover a stale disabled device slot,
  configuration saves, reassignment and command rejection outside the newly assigned home.
- [x] Panel Manager keypad assignments, enrollment and automatic update status (approved
  2026-10-18 by the owner): production popup editors, configured lists with capacity-limited Add and popup-only
  Remove, default/allow/idle/Never policy, hidden return controls when locked, automatic keypad
  supervision. The [shared enrollment service](../31-Browser-Panel-Manager/keypad-enrollment.md)
  supports normal/headless masters; the physical keypad still confirms its code. Normal/headless
  service tests, browser tests and both P4 builds pass. Physical enrollment acceptance remains below.
- [x] Live Panel Manager API enrollment/assignment on a display-equipped master and physical
  keypad: both confirmations (keypad via console), stale-code-sequence refusal, reconnect,
  cancel/expiry and policy delivery; details in feature 31's keypad-enrollment.md (BL-048).
- [x] Approved nearby enrollment, authenticated Wi-Fi provisioning and Select Wi-Fi fallback; language remains local (BL-048, LINK-053).
- [x] Hardware service acceptance: erase keypad Wi-Fi and pairing, enroll via ESP-NOW, automatically receive credentials and join Wi-Fi; reject unpaired requests, preserve manual credentials, and enroll over manually configured LAN with radio blocked (2026-10-18, dev.13; BL-048, LINK-053).
- [x] Recovery service tests: restart before/after credential storage, timeout then retry on restored radio; portable regressions for stale replies and older-master restart/upgrade without re-enrollment (BL-048, LINK-053).
- [ ] Physical setup acceptance: factory-first-boot touchscreen flow, abrupt-power-cut setup resume, older firmware on the master and actual out-of-range manual selection (BL-048, LINK-053). Orderly restart and radio-unavailable service behavior are verified above.
- [ ] Human comparison-screen and physical headless enrollment/assignment acceptance (BL-048).
- [ ] On the panels, what needs a person at the keypad or a network change: its Announcements,
  Status messages and Zone chime off and Voice volume 0 while the master speaks; a link cut over
  30 s saying nothing old; its internal RAM at start-up beside before (LINK-028) (BL-048)
- [x] ESP-NOW transport through our C6 bridge on the P4, commands verified on both panel sizes (docs/features/12 CP-003) (BL-048)
- [x] Full ESP-NOW firmware relay (2026-10-18, BL-048): signed 3,739,648-byte keypad image,
  0.7.4-dev.3 to dev.4, from the 7" master's bench copy to the 10.1" keypad. LAN packets were
  blocked throughout reception; both C6 radios remained 3.0.10-ra3. Reception took about
  11 min 49 s; arm/disarm commands during reception succeeded in 327/227 ms. Signature
  verification, automatic installation and the 60 s self-test passed over the radio path.
  Normal LAN transport was restored afterward; master disarmed, keypad ONLINE/up to date,
  speakers off and displays enabled. Builds used 79.3% of each OTA slot. No firmware code changed.
- [ ] Native ESP32 transport and hardware acceptance of changed AP channels (LINK-010) (M7) (BL-048)
- [x] The engine's device supervision trouble for a silent keypad (LINK-007's engine side,
  CORE-TRBL-008): `test_troubles.c` test_device_supervision, `alarm_engine_device_checkin`
- [x] ESP-NOW <-> LAN failover (LINK-009): selective transport loss switches the live path in
  about 3.2 s in either direction with continuous ONLINE; arm/disarm commands succeed on each (BL-048).
  P4/C6 transport and channel scan are implemented; changed AP-channel acceptance remains open.
  `test_transport_change_keeps_session_and_commands_once` proves the core keeps its session
  across UDP/250-byte radio address changes and suppresses duplicate commands. The service
  mirrors small sealed frames over both transports; only authenticated frames move a peer.
  A keypad with STATUS older than 11 s scans channels, using a 5 s disconnected Wi-Fi window.
  Bridge callbacks and RPC run outside the link task; control queues precede bulk queues.
  `plink drop lan|radio|off` supplies a development-only 90 s transport fault for HIL.
- [x] Wire: types 0x2D-0x31, `PLINK_CAP_FW`, the STATUS bits UPDATING, HOLD, FIRE and CO defined,
  the codecs and Python goldens; every v1 payload and number in a frozen
  `test/plink_wire_fixture_v1.h` (LINK-032, -037, -038, -040) (BL-048, a97a745)
- [x] The STATUS bits UPDATING, HOLD, FIRE and CO set and read; keypad fail-safes; UNAVAILABLE for
  an unknown command type (LINK-030, -034, -039) (BL-048, 14d159d)
- [x] Keypad core: orders, the in-order pull, FW_INFO and FW_STATE, the master's version for the cap
  (LINK-029, -032, -035) (BL-048, 14d159d)
- [x] `panel_ota`: PROBE, FETCH and RELAY (`panel_ota_order_*`, `PANEL_OTA_KIND_LINK`), the cap,
  `panel_ota_set_checks`, `_set_restart_hold` and `_set_keypads_check` (`PANEL_OTA_OK_KEYPADS`,
  `ota`/`rbh`), the image sources (`ota_source.c`); in `alarm_proto` the header judge, the image walk
  (`fw_image.c`) and the self-test's rule (`ota_selftest.h`) (LINK-029, -031, -033, -041) (BL-048,
  b387e1e)
- [x] Event stages on the master: `firmware_update` 9-12 from {keypad, n}, `keypad_failed`'s why in
  the JSON and Activity's words (docs/features/01 CORE-PROG-009) (BL-048, b387e1e)
- [x] The simple design: the small planner, the keypad on trial with its master, downloads paused only for
  an alarm, entry or exit time, a panic or a kept tone (LINK-029..036) (BL-048, 2421ee0)
- [x] Services: the master and the keypad, the PC simulation (`host/link_sim`, `tools/link_fw_sim.py`)
  (LINK-029 to -036, -041) (BL-048, 8e29ac5)
- [x] MQTT `keypads`, `plink fw` console (BL-048, 8e29ac5)
- [x] Screens as the approved drawing has them, their data on the panels (BL-048, 3e9ef1d, 74eb4b4)
- [x] `proto_fwsrv`: `want` in the check, the server's `unknown_version` and `not_offered`, an
  unknown reason on a check with `want` taken as a refusal, `proto_fwsrv_check_why` (LINK-031)
  (BL-048, b387e1e)
- [ ] `host/link_sim/link_peer.c`, the future peer (`-DPLINK_FUTURE_TEST=1`), `tools/link_compat.py`
  (LINK-042) (BL-048)
- [ ] The update server takes `want` (update-server repository, BL-050): built on its local branch
  `bl-050-want`, tried in local Docker with both boards' signed development releases;
  not merged or deployed to the public server
- [x] On the panels (2026-10-17, muted): the 7" master and the 10.1" keypad updated together from
  0.7.0 to 0.7.1-bench.1, the keypad relayed from a bench copy while the master was armed (3.6 MB in
  about a minute), both restarted within 4 s of the disarm, the master passed its self-test with
  its keypad back, the keypad's trial passed once its master ran the version, Activity logged
  installing and updated; the keypad took its master's update server by itself (BL-048)
- [x] On the panels (2026-10-17, muted), the rest: the keypad hard-reset (its reset line) at 30 % of
  its relay came back on its old version and was relayed again to READY; a master whose keypad was
  held off past 180 s failed its self-test ("the keypads not running") and went back, and dropped
  that version; with bench fault images (0.7.3-fault.z, `no_zone_service`, which only a master's
  self-test needs) the master went back on its own self-test and the keypad, healthy on the new
  version and on trial, went back with it ("its master doesn't run ..."), Activity logging the
  keypad rolled back (BL-048)
- [x] Direct FETCH on the two panels (BL-048, firmware 41aef62): 0.7.0 to 0.7.4-dev.1, muted;
  the keypad took its master's server and dev channel, requested `want=0.7.4-dev.1`, and
  downloaded its own 3,739,648-byte image directly. The master waited for it, both restarted
  about 4 s apart, both self-tests passed, and Activity recorded installing and updated.
- [x] Keypad-only USB power cut at 30% of FETCH: booted 0.7.4-dev.1, automatically fetched again,
  then both panels confirmed 0.7.4-dev.2 after disarming; both speakers stayed off (BL-048, a9716ef).
- [x] Touch/navigation and visible download progress remained responsive and steady to the owner;
  `display watch 20` during writes recorded 55-58 frames/s, zero late frames and touch errors,
  a maximum refresh of 851.4 ms and touch-poll gap of 1023.6 ms (BL-048, a9716ef).
- [x] Run-time caps: the offered caps an argument of `plink_master_init` and `plink_keypad_init`,
  each session keeping both offers ANDed under `PLINK_CAPS_KNOWN` (LINK-043) (BL-048,
  a97a745)
- [x] Wire: 0x32-0x3A, the HEARTBEAT section, the codecs and Python goldens, in the frozen v1
  fixture; `proto_prefs` (LINK-043, -050, -051, -052; HAI-017) (BL-216, a97a745)
- [x] Master core: list, pushes, summary, catch-up, DEV_ACT cache and buckets, PREFS report and
  PREFS_REQ, PREFS_SET, send order; `plink_home`; the link tests and the storm (LINK-044..051)
  (BL-216, ed58b12)
- [x] Keypad core: `plink_pull_t` from the configuration pull, mirror and catch-up, freshness,
  DEV_ACT slots and overlay, PREFS report and set answers (LINK-044..051) (BL-216, ed58b12)
- [x] One judge and one converter: `device_service_person_act`, `device_service_seq`,
  `hc_mode_word`, `ui_dev_from_parts`; `ui_port_esp` moved onto them with the master's screen
  unchanged (LINK-048; HCTL-005, -006, -012) (BL-216, 8abf492)
- [x] Master service: the `plink_dev` task, builds and jobs, `plink_service_set_home`,
  `plink_service_prefs_set`, `plink_service_keypads`, `g_pub`, the console (LINK-049) (BL-216,
  787c2da)
- [x] Keypad service and backend: mirror, `ksvc_dev_act`, `kp_dev_list`, `kp_dev_act`, layout
  entries kept, PREFS report, mailbox hand-over (LINK-046, -047, -050, -051) (BL-216, 787c2da,
  77e1f93)
- [x] UI-046: `panel_prefs_seq`, `_current` and the mailbox; `prefs_remote_*`; `ui_prefs_merge`;
  `prefs_remote_tick`; `ui_app_held`; `prefs_smoke` (BL-216, 8abf492)
- [x] The role gates removed as proposal P1 shows (approved 2026-10-16); `host_link_demo` with
  devices; preview shots compared (HCTL-013, UI-044) (BL-216, 787c2da)
- [x] `host/link_sim`: a fake registry on the virtual master, `devices` and `prefs` on the virtual
  keypad; `link_smoke` extended (LINK-013) (BL-216, 787c2da, 77e1f93)
- [x] Home Assistant on the panel: `alarm_proto`'s four JSON messages (`proto_link`; `proto_prefs`
  is in the Wire box) and the set queue, `ha_link`'s pushes and settings port, `panel_app`'s
  keypads port (docs/features/23 HAI-016..019) (BL-216, 8abf492, 787c2da)
- [ ] Home Assistant's integration 0.3.0 in `RoboAlarms/ha-addon` (built on the local branch
  `bl-216-settings`, 285 tests pass on Home Assistant 2026.10.1): tried with a panel, then
  released after BL-173's 0.2.0, on the maintainers' go (docs/features/23 HAI-016..019) (BL-216)
- [x] On the two panels without devices (2026-10-16, both speakers muted): HOME and PREFS shared,
  a settings change from the master on the keypad's screen and in its report within a second,
  one during an exit time held, commands in 150-190 ms, device data 0-10 s old, internal RAM
  +264 B (LINK-043, -045, -049, -050, -051) (BL-216, 787c2da, 77e1f93)
- [ ] On the panels with Home Assistant connected and devices: the tile states, an unlock with a
  code and the keypad's lockout, link lost, the storm, Home Assistant's settings entities, the
  rest of the list in Test and acceptance (BL-216)
- [ ] Docs: the pull request notes for the documentation repository's `UPDATING.md` (BL-216)

The master's settings and installer menu on keypads (BL-223):
- [x] Design and requirements LINK-057..LINK-065 ("The master's settings and installer menu on
  keypads")
- [x] The owner approves the proposal `BL-223-keypad-master-settings.js` and its five questions
  (2026-10-21)
- [ ] `ui_remote` and the link's messages: call ids, the codecs of every call, the messages, the frozen fixture; the
  master's grant, holder, answer cache and paging; the keypad's read copy (LINK-058, -059, -061,
  -064, -065) (BL-223)
- [ ] The session, configuration and zones' sensor groups served and proxied: an installer enters
  the master's installer menu and adds a zone from a keypad in `link_smoke` (LINK-060, -061)
  (BL-223)
- [ ] The radios' groups (Z-Wave, Zigbee, sirens and 433 MHz, keypads): pairing a device on the
  master from a keypad in `link_smoke` (LINK-062) (BL-223)
- [ ] Users, automations, Smart Home's settings, integrations and the event log (BL-223)
- [ ] The screens as approved, in the simulator and the preview (`host_link_demo`); the mockup's
  screens made today's (LINK-057, -063) (BL-223)
- [ ] On the panels: the S3 master with the 7" and 10.1" as its keypads - a zone added and a
  device paired from a keypad, the link cut in installer mode, commands' timing while an
  installer works (BL-223)

Covered by boxes in other documents (all BL-048):
- Keypad-attached inputs as zones (LINK-011): docs/features/02.
- The keypads' mDNS announcement (`_roboalarms-link._udp`): docs/features/09.
- Cross-partition GOTO on the keypads (KPD-006): docs/features/01, "On the keypads".
