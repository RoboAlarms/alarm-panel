# 11 · Panel link: master and remote keypads

**Status:** In progress (BL-048): the design below is for Wi-Fi first (ADR-054), reviewed from
four sides (security, protocol, fit with the code, safety) before building; the portable protocol,
the master's and the keypad's services, the keypad role and the PC simulator are being built. Built
before this work: the master's own UI through `ui_backend_t` and the engine's device supervision
(CORE-TRBL-008). Screens: approved 2026-10-15 and built (`ui_link.c`, the setup's role and
pairing steps), walked in the PC preview over a demo backend. Left after Wi-Fi: ESP-NOW through our C6 build, 433 MHz, the firmware relay (M8) · **Milestone:** M7 (the
firmware relay M8) · **Depends on:** 01, 04, 09, 12, 15

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
| LINK-012 | Firmware relay to keypads ([13](../13-OTA-Updates/README.md)) | M8 |
| LINK-013 | A PC "virtual keypad" and "virtual master" (host build, UDP, the OS's random numbers) interoperate with each other in CI and with panels | CI + manual |
| LINK-014 | Removing a keypad at the master clears its trouble, sends a sealed BYE (three times), then wipes its keys (RAM first, then NVS); the keypad erases its master record and says it is not paired | unit + HIL |
| LINK-015 | Each connection starts with a handshake (ephemeral X25519 and the pairing key) giving fresh session keys and a session id; a session's keys are installed once and nothing resets its counters; a replayed or forged HELLO never drops a live session; no side sends with seq ≥ 2^31 | unit |
| LINK-016 | Pairing: within an attempt each value (pk_k, pk_m and commit, n_k, n_m, mac_k) is set once - only a byte-identical repeat gets the stored answer, a different one ends the attempt; an attempt counts once its secret is disclosed (the master's at PAIR_REVEAL, the keypad's at PAIR_NONCE), whatever ends it; 3 attempts per window, at least 3 s apart; the window lasts 2 min, 30 s per step, 2 min to compare the codes; a keypad starts an attempt only when a person asks; two keypads or two masters at once end the attempt; nothing is stored before both confirmation MACs verify | unit |
| LINK-017 | Version skew: every payload section carries its length and a receiver takes what it knows and skips the rest; fields are only appended; the session uses the lower protocol version and the common capabilities, and a request the master can't serve is answered UNAVAILABLE; an unknown message type is dropped; v1's header never changes | unit |
| LINK-018 | Every command carries a request id (a random base drawn at each keypad start, then counted); the master answers a retry from its cache keyed on (keypad, request id) with a digest of the command, kept 90 s after the last copy arrived, and never runs a command twice; the same id with a different command runs as a new one; codes are never stored or logged, and every copy is wiped after use | unit |
| LINK-019 | The keypad's copy of the configuration holds no code material (each code hash replaced by a filler unique to its slot, the salt zeroed) and shows duress users as standard users | unit |
| LINK-020 | A panel in the keypad role runs no alarm engine, event log, zone sources or integrations; it updates itself only while its link is up and the master's partitions allow it (OTA-004), and its update self-test needs the link | HIL |
| LINK-021 | On Wi-Fi the master is found by mDNS `_roboalarms-link._udp` on UDP port 6056, announced only while it has a keypad or pairing is open (NET-007), and by a subnet broadcast when mDNS finds nothing; a peer's address moves only on a sealed frame that passes the tag and replay checks | HIL |
| LINK-022 | The link keys are never in a backup (the `link` namespace is not in the backup policy); a factory reset erases them | unit |
| LINK-023 | Panic keyed on a keypad is resent with the same request id every 1 s until answered or 60 s have passed, whatever the link state, and runs once (KPD-007) | unit |
| LINK-024 | On a working Wi-Fi network a keypad shows a change of state within 1 s, and a command's answer within 1 s (Wi-Fi power save off on a master with keypads and on keypads) | HIL |
| LINK-025 | After 10 min without system communication a keypad opens Wi-Fi, Software update, Pair again and "Make it a stand-alone panel" without a code: a keypad holds no codes and the master supervises it, so cutting it off is no worse than unplugging it; a keypad that is not paired (removed at the master, or never paired) opens them at once - no master could check a code, and it protects nothing | unit + HIL |
| LINK-026 | A keypad applies a status block, partition record, zone record or time only when it came in a newer frame of the session than the one it last applied for that item | unit |
| LINK-027 | The master accepts a keypad's commands for its home partition only (NOT_AUTHORIZED otherwise), one at a time with panic in a slot of its own, at most about 2 a second (a burst of 5); requests over their limits are dropped in the link task, before anything reaches the alarm task | unit |

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
| 0x2A | `HEARTBEAT` | K->M | sealed | flags u8 (bit 0 mains lost, 1 battery low, 2 battery present), rssi i8, uptime_s u32, status_age_s u16 (0xFFFF none yet), fw (≤ 31) |
| 0x2B | `TIME` | M->K | sealed | epoch_ms i64, tz (≤ 63, POSIX TZ) |
| 0x2C | `BYE` | M->K | sealed | reason u8 (removed at the master) |

**STATUS.** System section: epoch_ms i64, partitions u8 (mask), home u8 (this keypad's home
partition), sys_troubles u32, sys_restored u32, dev_troubles u32, default_codes u8, flags u8 (bit 0
program, bit 1 exit_sounds), recovery_left_ms u32, lockout_left_ms u32 (this keypad's own), report
u8 (0 none, 1 offline, 2 online: the master's reporting, for the Emergency words, UI-029),
zones_used u16, last_event_seq u32, cfg_crc u32, cfg_len u32, zones_crc u32. Then one record per
enabled partition: number u8, length u8 (the bytes after it), then the fields of
`alarm_partition_status_t` one by one
(state, level, arm_flags, armed_by, arm_source kind u8 + id u16, ready_levels, a flags byte for
ready, chime, walk_test, triggered, exit_error, trouble_unack, sounder, bell, delay_left_ms u32,
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
from the events when the state changes), and a STATUS never carries a last_event_seq below an
event already pushed to that keypad; EV_REQ fills gaps: the newest `max` events after after_seq,
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
and the keypad says it needs an update. The keypad states its capacities in PAIR_REQ and HELLO;
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
after its first, and panics never hold the last 2, so a disarm after a run of panics is not
dropped (a command that finds nothing else free takes the oldest answered panic not sent for 3 s).

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
small order (MISMATCH), both before anything is disclosed. A keypad that asks by broadcast (an
address ending in .255) asks so in every attempt; a `PAIR_FAIL` that comes before any
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
  it went by broadcast); mac2 needs an X25519 first, so it works out at most 8 WELCOMEs per HELLO
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
- HELLO and WELCOME each carry their sender's own proto and caps; both sides take the lower and
  the common ones, and a proto of 0 is refused. A sealed frame spends its seq even when sealing
  fails, so no nonce is tried twice; a frame with seq ≥ 2^31 is refused on arrival too.

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

## Transports

| Transport | Peer address | Largest frame | Discovery | Status |
|---|---|---|---|---|
| UDP over Wi-Fi (LAN) | IPv4 + port | 1200 B (no IP fragments: lwIP reassembly is off) | mDNS `_roboalarms-link._udp`, port 6056, TXT `v`, `net`, `pair`, `id`; subnet broadcast to 6056 when mDNS finds nothing | built first |
| ESP-NOW (C6 bridge on the P4) | MAC | 250 B (v1) | `BEACON` on the master's channel | later (CP-003) |
| 433 MHz | radio id | tens of bytes: status and commands only | - | later, an idea (needs a transmitter on both) |

The core sends through a port (`plink_net_port_t`: an opaque address with its transport, the
largest frame per transport); the platform hands every received datagram in with its source
address. The master answers a keypad on the address of its last authenticated frame (a WELCOME
goes to its HELLO's source without moving it). Firmware with the link sets
`CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` so a burst from 8 keypads isn't dropped.

## Roles on a P4 panel

- **Master** (the default, and every panel before this feature): runs everything it runs today,
  plus the link's master side when the feature is in the build (`ALARM_LINK`).
- **Keypad**: `panel_app` starts the board, the sounder, the language and asset packs, Wi-Fi,
  updates, the console, power monitoring (on the health task) and the touch UI with the link's
  keypad backend, and nothing that runs the alarm: no engine (`alarm_service` drops posts instead
  of queueing them), event log, zone sources, MQTT, notifications, Home Assistant, devices,
  automations, voice or Panel Manager.
- **Updates on a keypad**: the update gate is the link's (`panel_ota_set_idle_gate`): an update or
  a C6 update starts only while the link is up and the newest status, under 15 s old, passes
  `proto_fw_install_allowed()`; refused while the link is lost, except through LINK-025. The
  self-test mask is UI + TOUCH + LINK (`PANEL_OTA_OK_LINK`, beaten on authenticated frames).
  Keypads update after the master has confirmed its own update.
- **The keypad backend** serves from the link: snapshot, configuration, recent events, commands,
  code checks and code changes; locally: Wi-Fi, screen, sound, language, updates, About. The
  master-only pages (installer mode, users, zone sources, integrations, event log, devices,
  automations, voice) are hidden by a role gate the UI reads (proposal); until it is built,
  `program_enter` and `user_*` answer UNAVAILABLE. User editing never goes over the link.
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
  (LINK-008).
- **Time**: the keypad follows the master's TIME (epoch and time zone; it ignores epoch 0) and
  runs no time server of its own while paired.
- **Wi-Fi power save** is held off through `net_wifi_hold_low_latency(owner, on)`, one hold per
  reason (link keypads, remote viewing), on a master with keypads and on a keypad.

## Design
- **`components/core/panel_link`** (portable, host-tested, prefix `plink_`): the crypto helpers
  (HMAC/HKDF-SHA256 over `alarm_proto`'s streaming SHA-256; AES-CCM and X25519 from `zwave_host`
  on a port of three primitives), the frame codec and replay window, the message codecs, pairing
  and handshake, the master's and the keypad's link managers (time in, frames out through ports;
  no sockets), the stored records with frozen fixtures, the keypad configuration copy and the
  keypad's snapshot rebuild. No heap, no globals.
- **`components/services/plink_service`** (ESP-IDF, `ALARM_LINK`): the role, one task (internal
  stack: it writes NVS at pairing and removal) that owns a UDP socket, the managers and a PSA
  crypto port, and a worker for the alarm task's calls; mDNS; NVS `link`; on the master an alarm
  listener (snapshot mailbox, events, sounds); on a keypad the `ui_backend_t` overrides, the
  sounder and the update gate; the `plink` console command.
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
  link backend.
- CI: `link_smoke` over UDP on 127.0.0.1.
- Panels (HIL): pair the 10.1" as a keypad of the 7"; arm, disarm and a wrong code from the keypad;
  power the keypad off (trouble after its supervision time, cleared when back); restart the master (keypad back
  within 15 s); remove the keypad (it is told, its frames refused); latency under 1 s; one direction
  dropped; both addresses changed across a router restart; mDNS blocked; the access point's password
  changed and the keypad recovered from its own screen; the master factory-reset and the keypad
  paired again; an update refused during an entry delay; the PC virtual keypad with the 7" and the
  PC virtual master with the 10.1".

## Open questions
- Keypad-attached inputs (LINK-011), cross-partition GOTO (KPD-006) and partitions beyond home:
  after the Wi-Fi keypad works.
- A link keypad's trouble scopes to all partitions (the engine's device table has no entry for it),
  as Z-Wave's: a core call taking the partition would fix both. The trouble names the keypad by
  number only ("Keypad 3 not responding"); a name lookup shared with Zigbee and Z-Wave would fix it.
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

## Tasks
- [ ] `panel_link` core: crypto helpers with known-answer tests, the v1 frame codec and replay
  window, message codecs, pairing (LINK-003, -004, -005, -016, -017) (M7) (BL-048)
- [ ] `panel_link` core: handshake and sessions, master and keypad managers, stored records with
  frozen fixtures, the keypad configuration copy and lenient decode, the keypad's snapshot rebuild
  (LINK-001, -006, -007, -008, -014, -015, -018, -019, -023, -026, -027) (M7) (BL-048)
- [ ] LAN UDP transport and the PC virtual keypad and virtual master (`host/link_sim`, the PC
  simulator of docs/features/21), `link_smoke` in CI (LINK-013) (M7) (BL-048)
- [x] Loopback on the master: the touch UI reaches the alarm task through `ui_backend_t` (M3)
  (`components/services/ui_port_esp/src/ui_port_esp.c` `command` calls `alarm_service_command`)
- [ ] `plink_service` on the master: UDP, mDNS (LINK-021), the listener, the worker, supervision
  through `alarm_service_device_reachable` (LINK-007), the device numbers shared with Zigbee
  (`zigbee_service_devices_held`, `zigbee_service_set_link_devices`; LINK-001), Wi-Fi power-save
  holds, the `plink` console (M7) (BL-048)
- [ ] Keypad role on a P4: `panel_app` without the engine and the master's services, the keypad
  backend for `ui_backend_t` rendering the master's status and sending its commands (LINK-002,
  LINK-020), sounds from the master, the update gate and `PANEL_OTA_OK_LINK`; the 4" board's side
  is docs/features/19 CYD-015 (M7) (BL-048)
- [x] Pairing screens and the keypads' section in installer mode, the setup's role step, the
  keypad's Connecting, "No system communication", removed and no-code maintenance states, the role
  gate in Settings and About (LINK-004, -008, -014, -025; UI-011, UI-012): `ui_link.c`,
  `ui_wizard.c`, walked in the preview over `host/ui_preview/host_link_demo.c`; the mockup's
  `screens/panel-link.js` (BL-048, 18cc635)
- [ ] On the two development panels: pairing, commands, supervision, master restart, removal,
  latency and the other HIL cases above (LINK-004, -006, -007, -008, -014, -021, -024, -025) (M7)
  (BL-048)
- [ ] ESP-NOW transport: through our C6 bridge on the P4 (docs/features/12 CP-003), native on the
  ESP32; the channel follows the master's access point (LINK-010) (M7) (BL-048)
- [x] The engine's device supervision trouble for a silent keypad (LINK-007's engine side,
  CORE-TRBL-008): `test_troubles.c` test_device_supervision, `alarm_engine_device_checkin`
- [ ] ESP-NOW <-> LAN failover (LINK-009) (M7) (BL-048)

Covered by boxes in other documents (all BL-048):
- Keypad-attached inputs as zones (LINK-011): docs/features/02.
- Firmware relay to keypads (LINK-012, M8): docs/features/13, the keypad relay protocol.
- The keypads' mDNS announcement (`_roboalarms-link._udp`): docs/features/09.
- Cross-partition GOTO on the keypads (KPD-006): docs/features/01, "On the keypads".
