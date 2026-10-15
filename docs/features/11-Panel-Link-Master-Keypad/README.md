# 11 · Panel link: master and remote keypads

**Status:** Planned - designed, not built: the frame, messages and pairing below are the plan.
Only the master's own UI (through `ui_backend_t`) and the engine's device supervision
(CORE-TRBL-008) exist; everything else is BL-048: the codec, LAN transport and PC simulator,
pairing, the keypad client, supervision over the link, then ESP-NOW through our C6 build ·
**Milestone:** M7 (the firmware relay M8) · **Depends on:** 01, 04, 09, 12, 15

## Goal
Let any device act as a remote keypad for a master panel, like the keypads on a classic
panel's wired keypad bus. It must keep working when the home router is down, stay secure
against sniffing and replay, supervise every keypad, and carry firmware updates to keypads.

## Reference behaviour
Classic keypads are "dumb" terminals on a wired bus: they send keystrokes and show what the
panel tells them. The panel validates codes and supervises each keypad address (a missing
keypad shows a CHECK trouble). Some panels add encrypted wireless keypads.

## Decisions
- **Thin terminals**: keypads never hold user codes; the master validates everything (ADR-004).
- **Transports**: *loopback* (the master's own UI), **ESP-NOW** (primary, router-independent)
  and **LAN UDP** (fallback, faster bulk transfers, PC tools). One protocol over all of them.
- **Our own encryption** on every transport (ESP-NOW's built-in encryption has small peer limits
  and isn't available on LAN).
- The ESP32-H2 is **not** used for the keypad link (reserved for sensors, ADR-007).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| LINK-001 | One master, up to 8 panel-link keypads on device numbers 1-8, shared with the Zigbee keypads (a new keypad takes the lowest number neither kind holds; docs/features/20 alarm-devices.md §4); numbers 9-16 are the Z-Wave keypads' (docs/features/36 ZW-031). | unit |
| LINK-002 | Keypads render the master's status and send commands; they never validate codes | review |
| LINK-003 | Every frame is authenticated and encrypted (AES-128-CCM, 8-byte tag) with per-keypad keys; replayed or reordered frames are rejected (64-frame sliding window) | host crypto tests |
| LINK-004 | Pairing needs the installer at the master, and both screens show the same 6-digit code, confirmed by the installer (defeats man-in-the-middle) | HIL |
| LINK-005 | ESP-NOW control frames fit in 250 bytes (ESP-NOW v1 compatible); bulk frames use up to 1470 bytes (v2) or LAN | unit |
| LINK-006 | The master sends status on change and at least every 10 s; keypads send heartbeats every 10 s | HIL |
| LINK-007 | The master raises a device supervision trouble for a keypad silent for 60 s (per-device configurable) | `CORE-TRBL-008` + HIL |
| LINK-008 | A keypad shows "No system communication" after 30 s without status and disables arming controls | HIL |
| LINK-009 | Automatic failover ESP-NOW <-> LAN within 15 s; duplicates suppressed by sequence number | HIL |
| LINK-010 | The master's ESP-NOW channel follows its Wi-Fi AP; beacons carry the channel; keypads rescan channels 1-13 when lost | HIL |
| LINK-011 | Keypad-attached inputs are reported as zone events (zone expander role) | M7 |
| LINK-012 | Firmware relay to keypads ([13](../13-OTA-Updates/README.md)) | M8 |
| LINK-013 | A PC "virtual keypad" and "virtual master" (host build, LAN transport) interoperate with devices | CI + manual |
| LINK-014 | Removing a keypad at the master revokes its keys immediately | HIL |

## Frame format (v1)

```
+-------+-----+------+-------+--------+------------+---------+----------------------+-----+
| magic | ver | type | flags | net_id | sender (2) | seq (4) | ciphertext (payload) | tag |
| 'AL'  |  1  |  1   |   1   |   2    |            |         |                      |  8  |
+-------+-----+------+-------+--------+------------+---------+----------------------+-----+
header (13 bytes) = AAD; nonce = sender || seq || direction || net_id (13 bytes for CCM)
```

Keys per keypad: `K = HKDF-SHA256(ECDH shared secret, salt = net_id, info = "alarm-link v1")`,
split into master->keypad and keypad->master AES-128 keys. Crypto uses the PSA API (mbedTLS 4
in ESP-IDF 6.x; hardware AES/SHA on both chips).

## Messages

| Type | Dir | Content |
|---|---|---|
| `BEACON` | M->all | net_id, master name, Wi-Fi channel, protocol version, pairing-open flag (unencrypted, informational only) |
| `PAIR_REQ` / `PAIR_RESP` / `PAIR_CONFIRM` | both | ephemeral public keys (X25519 or P-256), device type, fw version; confirm MACs over the transcript |
| `STATUS` | M->K | partition snapshot: state, level, flags, ready levels, sounder, delay remaining, memory, counts, trouble summary (~48 B) |
| `ZONE_PAGE_REQ` / `ZONE_PAGE` | K<->M | zone flags + names in pages (fault lists, zones screen) |
| `CONFIG_SYNC` | M->K | partition names, keypad settings (home partition, options), zone names version |
| `COMMAND` | K->M | `alarm_cmd_t` (type, partition, level, flags, zone, panic, code) + request id |
| `RESULT` | M->K | request id, `alarm_result_t`, user, not-ready zone |
| `SOUND` | M->K | one-shot sound + zone (chime, ack, error) |
| `EVENT_NOTIFY` | M->K | selected events for toasts |
| `HEARTBEAT` | K->M | device number, fw version, RSSI, uptime, lockout state |
| `ZONE_REPORT` | K->M | keypad-attached input events |
| `TIME` | M->K | epoch + TZ |
| `OTA_*` | both | offer / chunk / ack / done ([13](../13-OTA-Updates/README.md)) |

## Pairing flow
1. Installer on the master: *Keypads -> Add keypad* (installer code); pairing opens for 2 minutes.
2. The new device chose *Keypad* in its wizard. It listens for beacons with pairing open
   (ESP-NOW on each channel, and LAN mDNS `_roboalarms-link._udp`).
3. ECDH exchange. Both screens show a 6-digit code derived from the transcript.
4. The installer confirms on the master and assigns a name, home partition and supervision time.
5. Keys and the device number are stored in NVS on both sides; the keypad starts receiving status.

## Design
- **Link manager** (per side): keeps peers, sequence windows, and transport health; picks
  ESP-NOW when healthy, else LAN; probes the other transport every 30 s.
- **Master (P4)**: ESP-NOW goes through our C6 co-processor bridge ([12](../12-C6-Coprocessor-Firmware/README.md));
  LAN UDP through the P4's lwIP.
- **Keypad (CYD)**: native ESP-NOW + Wi-Fi.
- **Keypad client API** (the original sketch; see the correction below):
  ```c
  void keypad_client_send(const alarm_cmd_t *cmd);                 // -> RESULT callback
  void keypad_client_on_status(void (*cb)(const kp_status_t *));   // snapshot updates
  bool keypad_client_connected(void);
  ```
  The UI does not call this API. Per ADR-012 it reaches the system only
  through `ui_backend_t` (`components/core/ui_backend`): on the master that loopback is
  `ui_port_esp.c` (alarm_service, local keypad source 0); on a remote keypad the link's client
  will be a `ui_backend_t` backend (docs/features/07, docs/features/19 CYD-015). So the
  Decisions' "one protocol over all of them" does not reach the loopback: it is `ui_backend_t`,
  not link frames.
- **Mesh Wi-Fi caveat**: ESP-NOW needs master and keypad on the same channel. If the house
  uses several APs on different channels, keypads should join the same AP as the master or
  run in LAN mode.

## Open questions
- ECDH curve: X25519 (compact) or P-256 (hardware-accelerated on both chips)? Leaning P-256.
  Z-Wave S2 (docs/features/36) already brought X25519 to the P4 firmware
  through PSA (`zwave_service` `zw_psa.c`), and `zwave_host` has a portable AES-128-CCM
  (`zw_aes_ccm_encrypt`) proved on the PC. Decided: X25519, already in the
  firmware; pairing is rare, so P-256's acceleration buys nothing.
- Should keypads cache the last status and show it greyed out while the link is down?
- Device numbers: a Z-Wave keypad given keypad control (ZW-031) takes one of
  the top eight device numbers (9-16) as its keypad source (docs/features/36, `zw_keyctl.h`
  `ZWKC_DEVICE`). How do link keypads share the 16 of LINK-001 with them? Decided:
  fixed ranges - link keypads take 1-8, Z-Wave keypads keep 9-16 as built, so nothing stored
  moves (LINK-001 as reworded). Zigbee keypads (2026-10-02, docs/features/20 alarm-devices.md
  §4) share 1-8 with the link keypads: pairing a link keypad skips the numbers the Zigbee
  keypads hold (`zbz_panel_devices_held`), and the Zigbee side skips the link keypads' (its
  port's `devices_taken`).

## Tasks
- [ ] Frame codec for the v1 frame, AES-128-CCM with per-keypad keys and the 64-frame replay
  window in `alarm_proto`, with host tests (LINK-003, LINK-005) (M7) (BL-048)
- [ ] LAN UDP transport and the PC virtual keypad and virtual master (`host/sim`, the PC
  simulator of docs/features/21) (LINK-013) (M7) (BL-048)
- [x] Loopback on the master: the touch UI reaches the alarm task through `ui_backend_t` (M3)
  (`components/services/ui_port_esp/src/ui_port_esp.c` `command` calls `alarm_service_command`)
- [ ] Keypad client: the panel link as a remote keypad's `ui_backend_t` backend, rendering the
  master's status and sending its commands (LINK-002); the 4" board's side is docs/features/19
  CYD-015 (M7) (BL-048)
- [ ] Pairing flow and screens (LINK-004) that assign one of the device numbers (LINK-001, skipping
  the Zigbee keypads' numbers), and
  removing a keypad revokes its keys (LINK-014) (M7) (BL-048)
- [ ] ESP-NOW transport: through our C6 bridge on the P4 (docs/features/12 CP-003), native on the
  ESP32; the channel follows the master's access point (LINK-010) (M7) (BL-048)
- [x] The engine's device supervision trouble for a silent keypad (LINK-007's engine side,
  CORE-TRBL-008): `test_troubles.c` test_device_supervision, `alarm_engine_device_checkin`
- [ ] Supervision over the link: status on change and every 10 s, keypad heartbeats feeding
  `alarm_engine_device_checkin` (LINK-006, LINK-007), the keypad's "No system communication"
  (LINK-008), and ESP-NOW <-> LAN failover (LINK-009) (M7) (BL-048)

Covered by boxes in other documents (all BL-048):
- Keypad-attached inputs as zones (LINK-011): docs/features/02.
- Firmware relay to keypads (LINK-012, M8): docs/features/13, the keypad relay protocol.
- The keypads' mDNS announcement (`_roboalarms-link._udp`): docs/features/09.
- Cross-partition GOTO on the keypads (KPD-006): docs/features/01, "On the keypads".
