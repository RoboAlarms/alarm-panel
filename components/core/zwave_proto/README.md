# zwave_proto

**Tier:** core (portable C11) · **Features:** [36 Z-Wave sensors (Zooz ZAC93)](../../../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) ·
**Tests:** [`test/`](test/) (5 suites: `zw_frame`, `zw_api`, `zw_cc`, `zw_entry`, `zw_fuzz`)

The Z-Wave Serial API's frame layer between the panel and the ZAC93
(`docs/features/36-Z-Wave-Sensors-ZAC93/serial-api.md` §1 "The wire"): the checksum, building a
frame, and a byte-at-a-time receive parser with the byte timeout and the counters a console status
line wants. On top of that, the Serial API's own commands (§2-§6): every function ID, the §3 value
tables and list helpers, a build/parse pair for every phase-1 message's payload (plus `DA`
GetLongRangeNodes and the SerialAPISetup sub-commands the start-up sequence uses), typed
SerialAPISetup sub-command helpers, and what a transport needs to run any command without a
per-message switch (§6 as data, and where a built request's callback ID sits) - proved on a PC
before a ZAC93 ever sends the panel a byte. Also a CRC-16 helper (the parameters Z-Wave's CRC-16
Encapsulation command class uses) and the DSK/PIN text an installer types or reads off a label
(`docs/features/36-Z-Wave-Sensors-ZAC93/security.md` §4).

On top of the Serial API sits the application layer: the command classes a security-sensor
controller speaks (`command-classes.md`, `zw_cc.h` and its themed sub-headers) - every CC and
command ID (§2/§4), a build/parse pair for every command §4 lists working on the full command
bytes (`CC | command | parameters`) as `ApplicationCommandHandler` carries them, §1's encapsulation
(Multi Channel, Supervision, CRC-16, Multi Command - wrap to send, unwrap one layer at a time to
receive; Security 0/2 and Transport Service are only recognised here, not decoded), §6's "which
answer belongs to which question" and §5's "what the panel answers".

This is only framing and codecs. Sending one Serial API command at a time, waiting for its ACK,
response and callback, retrying it on NAK/CAN/timeout, running the start-up sequence, the security
layer (S0/S2 crypto) and the node-information cache is `components/core/zwave_host`, coded against
`zw_api.h`'s and `zw_cc.h`'s names; nothing here decides to retransmit anything, and a codec never
re-enters the frame parser or another codec.

## Public API
| Header | What it offers |
|---|---|
| `include/zwave_proto/zw_frame.h` | `ZW_SOF`/`ZW_ACK`/`ZW_NAK`/`ZW_CAN`, `ZW_TYPE_REQUEST`/`ZW_TYPE_RESPONSE`, `zw_frame_checksum()`, `zw_frame_build()`, `zw_frame_t`; the receive parser `zw_rx_t` with `zw_rx_init()`, `zw_rx_feed()`, `zw_rx_expire()`, `zw_rx_frame()`, `zw_rx_busy()` |
| `include/zwave_proto/zw_crc16.h` | `ZW_CRC16_INIT`, `zw_crc16()` |
| `include/zwave_proto/zw_dsk.h` | `ZW_DSK_TEXT_SIZE`, `zw_dsk_to_text()`, `zw_dsk_from_text()`, `zw_dsk_pin()`, `zw_pin_from_text()` |
| `include/zwave_proto/zw_api.h` | `zw_func_t` (every Serial API function ID), `zw_nid_width_t` and `zw_api_node_id_build()`/`_parse()`; the §3 list helpers (`zw_node_list_*`, `zw_lr_segment_*`, `zw_func_mask_test()`, `zw_cc_iter_*`), transmit options/status, RSSI reserved values; `zw_api_tx_report_t`, `zw_api_node_update_t`, `zw_api_smartstart_join_t`; a `zw_api_<msg>_build()`/`_parse()` pair and `zw_api_<msg>_t` for every phase-1 message, `DA` GetLongRangeNodes and the SerialAPISetup envelope; typed SerialAPISetup helpers (`zw_api_setup_unsupported()`, `_supported_cmds_parse()`/`_supports()`, `_regions_parse()`, `_region_info_parse()`, `_rsp_u8()`, and builders for the requests that carry data); `zw_api_func_info()` and `zw_api_callback_id_offset()` for a generic transport - see the header's own "How to use" comment |
| `include/zwave_proto/zw_cc.h` | the umbrella: `zw_cc_t` (every CC ID §2 lists), then includes the themed headers below - one `#include "zwave_proto/zw_cc.h"` is enough for all of it |
| `include/zwave_proto/zw_cc_entry_control.h` | §4.26 Entry Control: bounded notification, supported keys/events and cache configuration codecs; zero-copy credential views |
| `include/zwave_proto/zw_cc_basic.h` | §4.1-§4.4, §4.6: No Operation, Basic, Binary Sensor, Multilevel Sensor (signed integer decode/encode, never float), Battery |
| `include/zwave_proto/zw_cc_notification.h` | §4.5 Notification: type/event support masks, Get/Set/Report with the v1-alarm-vs-v2+ split by length |
| `include/zwave_proto/zw_cc_supervision.h` | §4.14 Supervision: `Get`/`Report` on their own (also used by `zw_cc_encap.h` for the §1 layer) |
| `include/zwave_proto/zw_cc_multichannel.h` | §4.15 Multi Channel: End Point/Capability Get/Report, Command Encapsulation (also the §1 layer) |
| `include/zwave_proto/zw_cc_network.h` | §4.7-§4.13: Wake Up, Association, Multi Channel Association (with `zw_cc_mc_dest_iter_t` for its destination list), Association Group Information, Version, Manufacturer Specific, Z-Wave Plus Info |
| `include/zwave_proto/zw_cc_misc.h` | §4.18-§4.24: Indicator, Device Reset Locally, Configuration, Central Scene, Alarm Sensor (legacy; with Supported Get/Report), Hail (legacy), Time/Date |
| `include/zwave_proto/zw_cc_encap.h` | §1 encapsulation: `zw_cc_unwrap()` (one layer at a time; `zw_cc_unwrap_kind_t` names which), `zw_cc_multi_command_iter_t`, `zw_cc_wrap_multi_channel()`/`_supervision()`/`_crc16()`/`_multi_command()` |
| `include/zwave_proto/zw_cc_match.h` | §6: `zw_cc_answers()` - whether an incoming command is the answer to a Get the panel sent |
| `include/zwave_proto/zw_cc_answer.h` | §5: `zw_cc_panel_answer()` - what the panel answers, given its identity (`zw_cc_panel_identity_t`) and its own clock |

## Depends on
- Project components: none.
- ESP-IDF: none (portable).

## Options
None.

## Rules
- Plain C11: no malloc, no globals (the static tables, if any, are `const`), no RTOS or ESP-IDF
  headers; time comes in as `uint64_t` milliseconds; the host build stays warning-free
  (CONTRIBUTING.md, "Rules for the portable components").
- `zw_rx_t` is caller-owned, no heap: call `zw_rx_init()` before use, then feed it one received
  byte at a time. It never validates TYPE or FUNC - a higher layer (`zwave_host`) ACKs every
  frame whose checksum verifies, then drops the ones it has no use for.
- A `zw_api_*` codec works on the frame **payload** only (after FUNC, before CHK): it never touches
  TYPE or FUNC. `build()` returns the payload length or 0; `parse()` never reads past `len`.
  **Trailing bytes:** every parser for what the panel *hears* from the module (`_rsp`, `_cb`,
  `app_cmd`, `app_update`, `started`, and the shared shapes they use - node_update, smartstart_join,
  tx_report) accepts bytes after its own content and ignores them, so a newer module firmware that
  appends fields cannot break the panel; minimum lengths, internal length fields (one that says how
  many more bytes follow must still fit inside what was given) and values that pick a payload shape
  stay strict. The `_req` parsers - only a fake module uses them, to check exactly what the host
  sent - stay strict on length throughout. `zw_api_node_id_parse()` is a building block: it only
  enforces the minimum, and a `_req` wrapper that is nothing but a node ID adds its own exact-length
  check on top. `zw_api_tx_report_parse()` follows the same idea by size: below 17 bytes there is no
  report at all (never fails a SendData callback over its report); more bytes progressively add the
  last working node, the first failing node and the Long Range fields; anything past 24 bytes is
  ignored. Variable-length parts (node lists, the function mask, CC lists, command bytes, the
  version string, a SerialAPISetup sub-command's own data) are views into the buffer just parsed,
  or caller-owned pointers to splice into one being built - no copies, no heap.
- AddNode's and RemoveNode's status reports share one wire layout for every status - Silicon Labs'
  actual Serial API, not serial-api.md's now-being-corrected "context -" rows:
  `callback ID | status | source node ID (nid) | L | L bytes of node info` (a real "Ready" arrives
  as `cb 01 00 00`). A report that ends right after the status byte is also accepted (some
  firmware, and simple mocks). AddNode's AddingEndNode/AddingController require real node info once
  anything follows the status byte; RemoveNode's node info is always optional. `build()` always
  writes the full layout.
- `zw_func_t` carries every function ID (phases 1-3) as a value; only phase-1 messages (plus `DA`
  and the SerialAPISetup sub-commands §5 uses) have a codec so far - phase 2 comes in a later
  milestone. `zw_api_func_info()` and `zw_api_callback_id_offset()` are read-only data derived from
  §6's table and from where each callback-bearing request puts its callback ID; they do not change
  when phase 2's codecs are added, only grow.
- The frame format, the checksum and the 150 ms byte timeout follow
  `docs/features/36-Z-Wave-Sensors-ZAC93/serial-api.md` §1; the commands follow §2-§6; the CRC-16
  parameters and the DSK/PIN text format follow `security.md` §4 and §11. Tests reference `ZW-NNN`
  (docs/features/36).
- A `zw_cc_*` codec works on the **full command bytes** (`CC | command | parameters`) exactly as
  `ApplicationCommandHandler` carries them - builders write both the CC and command bytes, parsers
  check them. **Trailing bytes are always accepted and ignored** (`command-classes.md` §1: "extra
  trailing bytes are allowed", since a newer CC version appends fields); parsers still check every
  minimum length and internal length/count field strictly, and a value that picks a payload shape
  (a report's own declared length, Indicator Set's v1-vs-v2+ split, Notification Report's
  v1-vs-v2+ split) is honoured exactly as the doc describes it. Version-dependent optional fields
  (Notification's event/sequence number, Multilevel Sensor's v5+ scale byte, Battery's v2 flags,
  Version's v2 hardware/other-targets) are a `has_*` bool next to the field. Multilevel Sensor
  decodes its signed 1/2/3/4-byte values (3 bytes sign-extended) and Configuration its 1/2/4-byte
  ones as `int32_t` - **never floating point**; the caller divides by `10^precision` however it
  prefers. No heap, no copies: variable-length parts
  (masks, node/destination lists, CC lists, event parameters, a Supervision-wrapped command) are
  views into the buffer just parsed, or caller-owned pointers to splice into one being built.
- §1's encapsulation (`zw_cc_encap.h`) unwraps one layer at a time - `zw_cc_unwrap()` returns
  `ZW_CC_UNWRAP_NONE` with the same bytes echoed back once nothing more is wrapped, so a caller
  loops on it without a special case for "not wrapped at all". A command that only *contains* an
  encapsulation CC's ID and command byte without being that exact wrapper form (a Multi Channel
  End Point Report, a Supervision Report) is `ZW_CC_UNWRAP_NONE`, not mistaken for the wrapper.
  Security 0/2 (`98`/`9F`) and Transport Service (`55`) are only *recognised* - `zwave_host`'s job
  decrypts or reassembles them; Transport Service is unsupported in v1 per §1 ("drop and count").
  A CRC-16 mismatch refuses the whole command (`zw_cc_unwrap()` returns false), matching "check the
  CRC, drop on mismatch". Wrapping (`zw_cc_wrap_*()`) only ever produces the shapes the panel itself
  sends (Multi Channel, Supervision); CRC-16 and Multi Command wrapping exist for a simulated
  device or a test, since §1 says the panel never sends either.
- §6's `zw_cc_answers()` and §5's `zw_cc_panel_answer()` work on one already-unwrapped command;
  re-wrapping a reply with the request's own encapsulation (endpoint, Supervision session) is the
  caller's job, since it is the one holding both commands' unwrap results. Security's own Get/Report
  exchanges are out of scope for both (their command IDs live in `security.md`, not
  `command-classes.md`).

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
one suite with `ctest --test-dir build/host -R <suite>`.
- `zw_frame` (`test_zw_frame.c`, 21 tests, tags `ZW-003`, `ZW-SEC-012`, `ZW-SEC-013`): the §1 worked
  example built byte-exact and parsed; `zw_frame_build()`'s refusals and the largest possible
  frame; ACK, NAK and CAN while idle, and the same three bytes as ordinary data inside a frame;
  garbage before a SOF counted and discarded; a bad checksum; LEN below 3 reported at once, then
  recovery; the 150/151 ms byte-timeout boundary in `zw_rx_feed()` and in `zw_rx_expire()`; a clock
  that steps backwards; two frames back to back; the frame view's fields; `zw_rx_busy()`; the CRC-16
  vector (security.md §11) and the CRC catalogue's AUG-CCITT check value, chained across two calls;
  the DSK/PIN vector both ways, round trips of random DSKs, and every rejected DSK/PIN text.
- `zw_api` (`test_zw_api.c`, 41 tests, tags `ZW-001`, `ZW-003`, `ZW-004`, `ZW-023`): every message's
  build/parse pair round-trips, at both node-ID widths where `nid` appears; the doc's own worked
  examples (the §1 frame example's `GetControllerVersion`, `06`'s `A0 0F` timeout bytes,
  AddNode/RemoveNode's mode bytes `C1`/`C5`/`49`, a real "Ready" status report `cb 01 00 00`) come
  out byte-exact; every `_req` parser refuses one byte short, a length field inconsistent with the
  payload, and a trailing byte; every module-side parser instead *accepts* a trailing byte and
  ignores it, while still refusing one byte short and an inconsistent length field; the §4 fields
  `GetSerialApiInitData` and `GetControllerVersion` (`ZW-001`) and the start-up commands (`ZW-004`)
  decode to the exact bit meanings serial-api.md gives them; the typed SerialAPISetup helpers
  (legacy and extended support masks, regions, region info, one-byte results, the builders); and
  `zw_api_func_info()`/`zw_api_callback_id_offset()` checked row by row against §6's table, the
  latter for every callback-bearing request including the phase-2 ones this milestone has no codec
  for yet. Last, known-answer frames from real controllers, as Z-Wave JS's tests carry them, fed
  whole through the receive parser and then the codecs: a bridge controller's
  BridgeApplicationCommandHandler with an empty multicast mask and no RSSI byte (their issue
  #4335; one byte shorter refused); two frames in one read, a SendData callback whose two bytes are
  too few for a TX report and a node's Manufacturer Specific Report, both reported with nothing
  discarded between them; and a frame with a good checksum whose Binary Sensor Report is cut short,
  refused by the codecs, with the node's next complete report parsed after it.
- `zw_cc` (`test_zw_cc.c`, 29 tests, tags command-classes.md §4.1-§4.24, §1, §5, §6): every codec's
  build/parse pair round-trips (grouped one test function per command class); the doc's own worked
  examples come out byte-exact (§4.16 CRC-16 Encapsulation's `56 01 20 02` → `4D26`, §4.18's
  Identify indicator command `87 01 00 03 50 03 08 50 04 03 50 05 06`); §1's four wrapping layers
  (Multi Channel, Supervision, CRC-16, Multi Command) wrap and unwrap correctly, including a nested
  Supervision-around-Multi-Channel case, a CRC-16 mismatch being refused (by flipping the trailing
  CRC's own byte, which is provably always a different 16-bit value - not left to a probabilistic
  body-byte flip that could in principle collide), Security/Transport Service being recognised but
  not decoded, and a Multi Channel command being told apart from a same-CC command that merely
  isn't the encapsulation form; §6's `zw_cc_answers()` checked row by row including every
  parameter check (sensor/notification type, group, endpoint, CC, parameter, indicator ID,
  Supervision session) and the explicit non-matches (a Set, No Operation, Security); §5's
  `zw_cc_panel_answer()` checked for every row it implements, including the group-gated ones
  (Association/AGI answer only for the group the panel actually has) and `NO_REPLY` vs
  `NOT_HANDLED` vs `REPLY`.
- `zw_fuzz` (`test_zw_fuzz.c`, 7 tests, tag `ZW-023`): a splitmix64 PRNG (fixed seed, no `rand()`)
  drives the receive parser with at least a million random bytes and random gaps (every reported
  frame's checksum is checked to genuinely verify; run under ASan/UBSan in CI), 10,000 valid frames
  recovered exactly through random noise, and 5,000 valid frames with one byte corrupted followed
  by a 200 ms idle gap and a further valid frame - proving by construction, not by sampling, that
  a corrupted frame can never be reported back with the original frame's own length, and that the
  parser always recovers after the gap. Every `zw_api_*_parse()` is then run over thousands of
  random payloads at both node-ID widths (never out of bounds), and a representative cross-section
  of the trickiest shapes is built from random valid data and parsed back unchanged. The same two
  passes repeat for every `zw_cc_*_parse()`, `zw_cc_unwrap()`, the Multi Command iterator and
  (bonus, beyond one wrapper per codec) `zw_cc_answers()`/`zw_cc_panel_answer()` themselves, since
  both take attacker-controlled bytes and call back into several parsers above. Keeps the whole
  file to well under a second in a Debug build.
