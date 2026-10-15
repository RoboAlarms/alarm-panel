# rf_decode

**Tier:** core (portable C11) · **Features:** [25 RF sensors 315/345/433 MHz](../../../docs/features/25-RF-Sensors-315-345-433/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `rf_decode`)

A streaming pulse-train decoder for one-way RF sensors. The receiver (a CC1101 in asynchronous
OOK mode, or a plain superheterodyne module) turns the carrier into a level on one pin, the RMT
capture in `components/drivers/rf_rx` turns that level into pulse widths, and this component
turns the widths into packets: feed it every pulse (level and width) and it hands back a packet
the moment a frame completes. A de-glitch filter heals the line first; then five protocol
families decode the same pulses at once with one pair engine - EV1527 / PT2262, HT6P20B, HT12E /
SM5212, KeeLoq HCS200-301 and arctech (Nexa; 32 bits, or 36 in the LMST-606's open report) -
each frame exactly its bit count between syncs, its bits consistent with one transmitter, and its
protocol's own rules passed (docs/features/25 "Protocols", RF-012, RF-013). It carries encoders for every protocol (the tests and the
simulator), `rf_address_is_common()` (RF-018) and bench counters. It knows nothing about zones
(`zone_hub`'s `zsrc_rf` gives packets a meaning), never allocates, never waits and touches no
radio. On the panel `rf_service/src/rf_service.c` feeds it the captured pulses.

## Public API
| Header | What it offers |
|---|---|
| `include/rf_decode/rf_decode.h` | `rf_proto_t` (stored numbers), `rf_proto_info()`, `rf_packet_t` (proto, device, data, flags, code, extra, unit), `rf_decoder_t` with `rf_decoder_init()`, `rf_decoder_enable()`, `rf_decoder_pulse()` / `rf_decoder_gap()` (up to `RF_DECODE_MAX_OUT` packets per call); encoders `rf_encode()`, `rf_encode_burst()`, `rf_ev1527_encode()`, `rf_packet_make()`; `rf_address_is_common()` |

## Depends on
- Project components: none.
- ESP-IDF: none (portable).

## Options
None. The protocol table (units, sync and bit templates, tolerances) is in `src/rf_decode.c`;
`RF_GLITCH_US` (60) and the EV1527 unit range (120-800 us) are in the header, not `-D` options.

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; the host build stays
  warning-free.
- `rf_proto_t` values are stored in the binding blob through `zone_hub`'s `zsrc_rf`: never
  renumber, only add, and keep them equal to `ZSRC_RF_PROTO_*`.
- Tests reference `RF-NNN` (docs/features/25).
  (CONTRIBUTING.md, "Rules for the portable components")

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R <suite>`.
- `rf_decode` (`test_rf_decode.c`, 45 tests, tags `RF-002`, `RF-012`, `RF-013`, `RF-018`, `RF-024`): frames
  from the encoders sent through a model of the air and the receiver - slicer shifts, jitter,
  AGC settling, static, glitches, lost and extra pulse pairs at every position, bit flips,
  collisions of two transmitters, other protocols and Manchester signals, an inverted line, a
  carrier - with the safety property checked everywhere: frames may be lost, codes never made
  up. The list maps to docs/features/25 "What goes wrong on the air". The products people buy,
  as mock devices through this decoder and the zone source: `zone_hub`'s `test_rf_devices.c`
  (docs/features/25 "Mock devices").
- `test/rf/*.txt`: captures in the Flasher's / `rf capture` format with `# expect:` lines,
  decoded by `test_recorded_captures` (see `test/rf/README.md`; the two there are synthetic
  until the bench records real sensors).
- What a packet does to a zone is tested in `zone_hub`'s `rf` suite (`RF-003`..`RF-016`).
