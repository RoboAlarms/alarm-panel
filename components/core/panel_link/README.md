# panel_link

**Tier:** core · **Features:** [11](../../../docs/features/11-Panel-Link-Master-Keypad/README.md)
· **Tests:** `test/` (LINK-NNN)

The panel link between a master panel and its keypads, portable: everything that can be proved on
a PC. The platform owns the sockets, NVS, PSA and the tasks (`components/services/plink_service`
on a panel, `host/link_sim` on a PC) and drives the managers here with frames and time.

## Public API
| Header | What |
|---|---|
| `plink_crypto.h` | the crypto port (AES block, X25519, random), SHA-256 over pieces, HMAC and HKDF-SHA256, X25519 with the all-zero check, AES-128-CCM, the start-up self-check |
| `plink_frame.h` | the v1 header, message types, sealing and opening, the replay window, peer addresses |
| `plink_wire.h` | little-endian writing and reading, sections, names safe to show |
| `plink_msg.h` | every message's payload: structs, encoders, decoders |
| `plink_view.h` | the master's STATUS from its snapshot; the keypad's snapshot rebuilt on its clock; the keypads' configuration copy and its lenient decode |
| `plink_store.h` | the master's and the keypad's records in NVS `link` (stored contracts) |
| `plink_pair.h` | pairing on both sides: commit-then-reveal, the code, the keys |
| `plink_session.h` | the handshake and the sessions it makes |
| `plink_master.h` | the master's link manager: keypads, sessions, pairing, status, commands, supervision |
| `plink_keypad.h` | a keypad's link manager: its master, the handshake, status, configuration, events, commands and panic |

## Depends on
`alarm_core` (types, snapshot, configuration codec); privately `alarm_proto` (SHA-256) and
`zwave_host` (AES-128-CCM and X25519 on the same three-primitive port).

## Rules
- Plain C11, no heap, no globals; time comes in as ms. One task owns a manager.
- The frame (`plink_frame.h`), the message payloads (`plink_msg.h`) and the stored records
  (`plink_store.h`) are contracts between firmware versions: never renumber or reuse, only append
  (LINK-017); a stored format change adds a frozen fixture next to `test/plink_store_fixture_v1.h`.
- No code, key or nonce is logged; secrets are wiped with `plink_wipe` (LINK-018).
- Tests name the requirement they prove (`/* LINK-003: ... */`).

## Tests
`test/plink_test_port.c` is the PC crypto port (zwave_host's primitives, marked insecure, with a
check that no AES-CCM key and nonce is ever used twice on one side); `test/plink_test_net.c` is a
pretend network with loss, repeats, reordering, corruption and one direction cut on a fake clock.
`test_plink_crypto.c` and `test_plink_frame.c` hold the known answers (RFC 3610, 4231, 5869, 7748)
and golden frames; `test/vectors_plink_crypto.py` and `test/vectors_plink_frame.py` recompute every
one of them with Python's `cryptography`, independently of the C code.
