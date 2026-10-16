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
| `plink_vfeed.h` | what a keypad's voice hears (LINK-028): the keypad's events, sounds and snapshot turned into the voice's baseline, pause, facts and commits - covered events only, never older than its baseline, held for a STATUS on its way, settled for a late tone |

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

`test_plink_pair.c` and `test_plink_session.c` check one honest pairing and one handshake byte for
byte against `test/vectors_plink_pair.py` and `test/vectors_plink_session.py` (random numbers
scripted through the port), then each state machine against a device in the middle, replays and
every timer; pairing asked by broadcast, and a newer firmware's longer payloads.

Every parser gets random bytes, each input ending at the end of its buffer: the message decoders
(`test_plink_msg.c`), the stored records with their CRC made right again (`test_plink_store.c`),
the keypads' configuration copy (`test_plink_view.c`), HELLO and WELCOME with their MACs made
right again (`test_plink_session.c`), and both pairing sides at every step (`test_plink_pair.c`).

`test_plink_master.c` runs the master's manager over the pretend network against scripted
keypads built from the real pairing, handshake, session and message codecs, with the real engine
(`alarm_sim`) behind it as the platform wires it (port.run answering from inside the call or
later, a snapshot after every engine call). Every vector it needs comes from those codecs, which
have their own known answers. A test of a network that damages frames watches only its own side's
seals (`plink_test_port_t.only_dir`): a frame whose tag fails may be opened again under the same
nonce, which is harmless.

`test_plink_keypad.c` runs the keypad's manager against a scripted master made of the real pieces
(pairing, the handshake answer, sessions, the codecs, STATUS from snapshots of the real engine)
over the pretend network: pairing, the tentative session, the link's states and timers, LINK-026's
ordering, the configuration pull, zones and events, commands and panic, BYE, REKEY and the
handshake's back-off, the master moving. The keypad's port has an optional `resolve`: the platform
looks for the master again before every handshake retry after the first failure.

`test_plink_link.c` runs the real master and up to eight real keypads together over the pretend
network, the engine (`alarm_sim`) behind the master as the platform wires it, every side on its
own clock (a restart starts one again): pairing from scratch to a keypad that shows the master in
every displayed field, commands and the lockout of one keypad, status within a second with loss,
restarts of either side, both directions cut, a keypad switched off, the master's Wi-Fi down, panic
through an outage, removal, eight keypads pulling the configuration, replays, a hostile keypad with
valid keys, addresses moving, and six hours on a damaged network. After every test it checks that
no side used a key and nonce twice and no command ran twice. Its random numbers all come from
seeds: `PLINK_LINK_SEED=<n>` in the environment runs it on other ones (a change to the managers
is worth a few dozen).

A keypad's voice (LINK-028): `test_plink_vfeed.c` drives `plink_vfeed` over a fake keypad (its
link state, snapshot and event ring) and a recording port: nothing before the first ONLINE, the
baseline's floor, covered events only and once, the hold and the settle, LOST, a new home, a log
started again, the walk test's bell, stale events, the deadlines. `test_plink_link.c` puts the
real policy (`annunciator`) behind a keypad's `plink_vfeed` and another fed straight from the
engine as `panel_app` feeds a master's, and compares what each decides: the same on a working
network (arming, armed, entry, disarmed, not ready, a chime's words, a walk test); nothing old
after a keypad restart, a master restart, a cut over 30 s or one under it; a moved home; under
loss never a kind the master didn't decide then, nor one twice. `PLINK_VOICE_SHOW=1` prints the
loss test's utterances.
