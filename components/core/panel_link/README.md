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
| `plink_home.h` | Smart Home on keypads, both sides: the device list blob built from `home_control`'s registry rows (`plink_home_list_build`, `plink_home_recs_build`), `list_crc` (never 0) and `states_crc`, the records by index (`plink_dev_recs_t`) and their values for a screen (`plink_home_values_of`), a keypad's lenient walk of a pulled blob (`plink_home_list_check`, `_next`, `_find`), and a row's record carried over by ref at a new list's install (`plink_home_list_keys`, `plink_home_carry`) |
| `plink_master.h` | the master's link manager: keypads, sessions, pairing, status, commands, supervision; Smart Home and settings on keypads (`plink_master_home`, `plink_master_dev_answer`, `plink_master_prefs_set`, `plink_master_keypad_home`, `port.dev_run`, `port.prefs_done`) |
| `plink_keypad.h` | a keypad's link manager: its master, the handshake, status, configuration, events, commands and panic; Smart Home and settings on keypads: the mirror of the master's devices in the caller's memory (`plink_keypad_home_attach`, `plink_dev_mirror_t`; without one HOME is never offered), the list pulled as the configuration is (one `plink_pull_t` each), the rows, records and freshness (`plink_keypad_dev_info`, `_dev_row`, `_dev_rec`), device actions and what a device shows meanwhile (`plink_keypad_dev_act`, `_dev_act_state`, `_dev_act_release`, `_dev_overlay`), a row as the screen shows it - the record with this keypad's action over it, offline while the device data is stale (`plink_keypad_dev_view`), the settings report and each change applied once (`plink_keypad_prefs_report`, `plink_keypad_take_prefs_set`, `plink_keypad_prefs_answer`) |
| `plink_fw.h` | updates through the link on the master: the planner (`plink_fw_plan`, a pure step on a world of the master's target and image and each keypad's FW_INFO and FW_STATE: its orders, each keypad's row, the copy wanted, the INSTALLs not yet echoed), `plink_fw_again` (ask a keypad again), the rows' names for MQTT (`plink_fw_row_name`) and what Activity logs of each keypad's update, each stage once (`plink_fw_log_step`) |
| `plink_vfeed.h` | what a keypad's voice hears (LINK-028): the keypad's events, sounds and snapshot turned into the voice's baseline, pause, facts and commits - covered events only, never older than its baseline, held for a STATUS on its way, settled for a late tone |

## Depends on
`alarm_core` (types, snapshot, configuration codec) and `home_control` (the registry's rows that
`plink_home` lists); privately `alarm_proto` (SHA-256, the settings fields' CRC) and `zwave_host`
(AES-128-CCM and X25519 on the same three-primitive port).

## Rules
- Plain C11, no heap, no globals; time comes in as ms. One task owns a manager.
- The frame (`plink_frame.h`), the message payloads (`plink_msg.h`) and the stored records
  (`plink_store.h`) are contracts between firmware versions: never renumber or reuse, only append
  (LINK-017); a stored format change adds a frozen fixture next to `test/plink_store_fixture_v1.h`,
  and every v1 payload, with every v1 number of the tables, is frozen in
  `test/plink_wire_fixture_v1.h` (never edited).
- A PREFS or PREFS_SET fields body travels as one span, its count first, exactly as
  `proto_prefs_encode` writes it and `proto_prefs_decode` and `proto_prefs_crc` read it (bytes a
  later version appends inside the section included).
- What a panel offers (`PLINK_CAP_*`) is an argument of `plink_master_init` and
  `plink_keypad_init`; a session keeps the bits both sides offered, masked with
  `PLINK_CAPS_KNOWN`, and a capability's types go only in a session with its bit (LINK-043).
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

`test_plink_msg.c` holds a golden payload of every message, written by hand from the tables;
`test/vectors_plink_msg.py` builds the same bytes with Python's `struct` (`--check` compares them,
and the frozen `test/plink_wire_fixture_v1.h`, which `--fixture` made once). The test reads each
fixture payload as the values frozen with it and pins every v1 number to the fixture's.

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
nonce, which is harmless. Its Smart Home and settings tests hand in a fake registry as the
platform does and check every rule of LINK-043..051 on the master: nothing without the capability
bit or under 250 B frames, the list's pages, the full set after a new session's STATUS and TIME,
rounds, summaries and requests, a list change, device actions run once from their own cache and
bucket with each DEV_ACK's record as it is then, the settings report, PREFS_REQ and writes (the
next write started from inside `port.prefs_done`, the answer's report already kept), and all of
it behind a waiting STATUS or RESULT - before the engine's first snapshot too, with no deadline
already past - at most 2 frames per keypad and 8 per tick.

`test_plink_home.c` checks the shared pieces: the blob from the registry's rows (rows whose ref
the wire can't carry left out of the blob and the records alike, at most 64, the longest list
within `PLINK_DEV_BLOB_MAX`), `list_crc` never 0, a newer master's blob read leniently and a newer
compat refused, the records' flags and `states_crc`, the registry's values given back from a
record's bytes (`plink_home_values_of`), and records carried over by ref.

`test_plink_keypad.c` runs the keypad's manager against a scripted master made of the real pieces
(pairing, the handshake answer, sessions, the codecs, STATUS from snapshots of the real engine)
over the pretend network: pairing, the tentative session, the link's states and timers, LINK-026's
ordering, the configuration pull, zones and events, commands and panic, BYE, REKEY and the
handshake's back-off, the master moving. The keypad's port has an optional `resolve`: the platform
looks for the master again before every handshake retry after the first failure. Its scripted
master also builds a device list and records from a fake registry with `plink_home.h` and offers
HOME and PREFS when a test asks: HOME offered only with a mirror, nothing of either capability
without its bit or on frames under 250 B; the list pulled (its first page alone, then two out),
installed whole, a list that fails remembered, records carried over by ref, the list dropped at a
session without HOME (Smart Home hidden) and pulled again once HOME is back; records in frame order
per device and only under the installed list, caught up 250 ms after a mismatch then 1 s apart
doubling to 30 s; device data stale after 30 s, kept fresh only by data under the installed list
(a list that fails leaves the old one reading offline); device actions refused at once, resent
every 500 ms and given up at 3 s (the code wiped after the last copy), late answers and what the
device shows, a row as the screen gets it (`plink_keypad_dev_view`: the record, this keypad's own
action over it once released, every row offline since the last data once stale), 2 slots apart
from the commands; list and state requests held while a panic is
sent; the settings report's timing, the HEARTBEAT's settings CRC, a PREFS_SET applied once.

`test_plink_link.c` runs the real master and up to eight real keypads together over the pretend
network, the engine (`alarm_sim`) behind the master as the platform wires it, every side on its
own clock (a restart starts one again): pairing from scratch to a keypad that shows the master in
every displayed field, commands and the lockout of one keypad, status within a second with loss,
restarts of either side, both directions cut, a keypad switched off, the master's Wi-Fi down, panic
through an outage, removal, eight keypads pulling the configuration, replays, a hostile keypad with
valid keys, addresses moving, six hours on a damaged network, and Smart Home and settings: end to
end (the mirror's `states_crc` the master's, an action run once, a write applied once); behind a
fake registry (twenty devices of every kind; a judge that checks codes with the engine, answering
after a delay) and keypads whose service applies the master's settings writes, the list and
records converging through loss, damage, copies and reordering and on 250 B frames, an action run
once through lost answers and never after its 3 s, a renamed or reordered list never acting on
another device, a master restart in the middle of a pull, a 45 s cut (offline since the cut,
nothing replayed), a write applied once or ended TIMEOUT at 6 s, a v1 keypad and a v1 master
getting nothing new, and the storm (64 devices changing every 100 ms, an action every 500 ms and a
write every second) leaving disarm and panic exactly as fast as calm. After every test it checks
that no side used a key and nonce twice, no command and no device action ran twice, no DEV_ACT
went after its 3 s, no keypad installed a list the master never had or showed a device another's
record, and every master turn sent a keypad's DEV_* and PREFS* frames after its alarm traffic,
never while its STATUS change or a RESULT waited, at most 2 to a keypad and 8 in all. Its random
numbers all come from seeds: `PLINK_LINK_SEED=<n>` in the environment runs it on other ones (a
change to the managers is worth a few dozen).

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

KPD-006 partition views: GOTO (op 4) selects without a code when the persisted peer policy
allows it; ACTIVITY (op 5) renews the current selection. Alarm operations still run through the
authorization worker and must name the selected partition. Default idle return is 30 seconds;
Never disables expiry. Policy changes, reconnects and reassignment clear selection. Missing
stored/wire policy means locked. Frozen policy fixtures extend, rather than alter, v1 fixtures.
