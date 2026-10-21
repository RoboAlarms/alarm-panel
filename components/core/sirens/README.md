# sirens

**Tier:** core (portable C11) · **Features:** [16 Outputs, siren and relays](../../../docs/features/16-Outputs-Siren-and-Relays/README.md),
[25 RF sensors](../../../docs/features/25-RF-Sensors-315-345-433/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `sirens`)

Every kind of siren the panel drives follows one policy, so every siren in a house agrees. The
engine's bell and strobe outputs (`alarm_ports.h` `on_output`) are kept per partition in a
`siren_view_t`; a siren serves a scope - its own partitions, other partitions whose alarms it
`also` sounds for (the house's siren for the shop's alarm, OUT-015), burglary and audible panic,
fire and CO - and `siren_pick()` gives the most urgent sound: fire, then CO, then burglary, then
the walk test's one-shot bell test. Zigbee's sirens (`zigbee_zones/zbz_wd`) use the same order;
Z-Wave's (`zwave_zones/zw_siren`) use this module.

Two drivers live here:
- **Wired outputs** (`out_wired`): a siren, strobe, the smoke detectors' power or an "armed"
  light on a terminal, through a relay or MOSFET module. Temporal-3 (NFPA 72) and temporal-4
  (NFPA 720) on the line to the millisecond, chirps (one to arm, two to disarm), a safe level at
  configuration, and bell supervision from a second terminal (open 10 s = the bell trouble).
- **433 MHz sirens** (`rf_siren`): the cheap one-way sirens that learn their panel like a key fob.
  The panel transmits as a fob of its own (a fresh EV1527 address per siren): the alarm code at
  once and every 20 s while the sound is wanted, the stop code three times when it ends, the arm
  and disarm codes as chirps, a pairing burst. Nothing can supervise them; a missing transmitter
  is the bell trouble.

It doesn't touch pins or the radio (ports), keep a clock, or post troubles (the owner does:
`outputs_service`, `zwave_service`).

## Public API
| Header | What it offers |
|---|---|
| `include/sirens/siren.h` | `siren_view_t`, `siren_scope_t`, `siren_sound_t`; `siren_view_output()`, `siren_view_armed()`, `siren_pick()`, `siren_strobe()`, `siren_armed()`, `siren_level()` (the wired patterns), `siren_chirp_level()`, `siren_crc16()` |
| `include/sirens/out_wired.h` | `ow_t`, `ow_config_t`, `ow_port_t`; `ow_set/clear/output/armed/set_armed/test/poll/next_deadline`, the store `ow_encode/ow_decode` (NVS `outputs`/`wired`) |
| `include/sirens/rf_siren.h` | `rfs_t`, `rfs_config_t`, `rfs_port_t`; `rfs_defaults/pick_address/set/clear/output/armed/transmitter/test/pair/poll/next_deadline`, the store `rfs_encode/rfs_decode` (NVS `outputs`/`rf`) |

## Depends on
- Project components: `alarm_core` (the output and pattern types), `rf_decode` (the packets sent)
- ESP-IDF: none (portable)

## Stored formats
`outputs`/`wired` and `outputs`/`rf` are stored contracts (components/core/AGENTS.md): never
renumber a function or move a field; a new field goes at an entry's end (a shorter entry reads it
as 0). Their frozen fixtures are `test/sirens_fixture_v1.h`.
