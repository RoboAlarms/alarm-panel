# alarm_core

**Tier:** core (portable C11) · **Features:** [01 Core alarm engine](../../../docs/features/01-Core-Alarm-Engine/README.md), [04 Keypad behaviour and commands](../../../docs/features/04-Keypad-Behavior-and-Commands/README.md), [14 Configuration and installer programming](../../../docs/features/14-Configuration-and-Installer-Programming/README.md) ·
**Tests:** [`test/`](test/) (13 suites: `arming`, `exit_entry`, `alarms`, `life_safety`, `users`, `troubles`, `keyseq`, `persistence`, `partitions`, `config`, `program`, `codec`, `walk`)

The alarm engine. It decides everything a classic keypad alarm panel decides:
arming, exit and entry time, alarms and sounding, what is reported and when (burglary after the
abort window), troubles and supervision, users and codes, installer mode. It also parses keypad
key sequences and turns the configuration into a versioned blob. It exists so that one copy of
the alarm logic runs unchanged on the ESP32-P4, the classic ESP32 and a PC. It does no I/O of
its own: no heap, no globals, no RTOS, no clock. The caller owns the `alarm_engine_t`, passes
the time in as milliseconds and serialises every call through one task. Events, sounds, outputs
and state to persist leave through the callbacks in `alarm_ports_t`; storing them, hashing codes
and talking to the network are the platform's job.

## Public API
| Header | What it offers |
|---|---|
| `include/alarm/alarm_types.h` | Capacities, 1-based numbering, and the enums and value types: arm states and levels, zone types, zone events, troubles, commands, results, events |
| `include/alarm/alarm_config.h` | `alarm_config_t` (options, partitions, zones, users, devices) and the persisted runtime state; defaults, factory users, validation, zone-type helpers |
| `include/alarm/alarm_ports.h` | `alarm_ports_t`: the callbacks the platform provides (`hash_code` is required, the rest optional) |
| `include/alarm/alarm_engine.h` | The engine: init (and `alarm_engine_init_ex`, which records the start reason, PWR-007), tick and next deadline, zone events, system troubles, device check-ins, commands, users, installer mode, platform events, status queries |
| `include/alarm/alarm_snapshot.h` | `alarm_engine_snapshot()`: everything a keypad shows, in one copyable struct (with the touchscreen's lockout end, `alarm_engine_lockout_left()`) |
| `include/alarm/alarm_keyseq.h` | Keypad key sequences (code + OFF / AWAY / STAY..., bypass, user codes, panic keys) parsed into `alarm_cmd_t` |
| `include/alarm/alarm_codec.h` | The configuration as a versioned, self-describing blob (`alarm_config_encode` / `_decode`) and `alarm_crc32()` |
| `include/alarm/alarm_strings.h` | Stable snake_case names for the enums (event log export, MQTT JSON) |

## Depends on
- Project components: none.
- ESP-IDF: none (portable). The ESP-IDF branch of `CMakeLists.txt` only passes the Kconfig
  capacities in as compile definitions.

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_MAX_PARTITIONS` | 4 (range 1-8) | Partitions compiled in |
| `ALARM_MAX_ZONES` | 128 on ESP32-P4, else 64 (range 1-999) | Zones compiled in |
| `ALARM_MAX_USERS` | 64 on ESP32-P4, else 32 (range 2-99) | User codes, including the installer (01) and the system master (02) |
| `ALARM_MAX_DEVICES` | 16 (range 1-32) | Supervised devices (keypads, expanders, radios) |

On ESP-IDF they become public compile definitions (`ALARM_MAX_*`), so every dependent component
sees the same capacities; elsewhere `alarm_types.h` defaults them to 4, 128, 64 and 16 (`-D`
overrides). `ALARM_MAX_SOURCES` (16, lockout sources) and `ALARM_MAX_PENDING_REPORTS` (24,
reports held back by the abort window) are `-D` only.

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; time comes in as `uint64_t` ms.
  All state lives in the caller-owned `alarm_engine_t`, and port callbacks must not call back
  into the engine. The host build stays warning-free (`-Wall -Wextra -Wpedantic -Wconversion -Werror`).
- Behaviour follows classic keypad alarm panels and SIA CP-01; deliberate choices are listed in
  docs/features/01. Every `CORE-XXX-NNN` requirement there has at least one test in `test/` tagged
  with the ID in a comment: add or change both together.
- The names in `alarm_strings.h` (`src/strings.c`) are an external contract (MQTT/JSON): never
  rename, only add. `alarm_event_type_t` values are stored in the event log: append new types,
  never reorder.
- The configuration blob (`src/codec.c`) is stored in NVS and read by older and newer firmware:
  never reuse or renumber a field id; new fields get new ids (missing ones keep their defaults);
  raise `compat` only when older readers would misread the blob. When the format changes, add a
  new frozen fixture next to `test/codec_fixture_v1.h`; never edit one.
  (CONTRIBUTING.md, "Rules for the portable components")

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R <suite>`.
- `arming`, `exit_entry`, `alarms`, `life_safety`, `troubles`: readiness, bypass, chime, exit
  and entry time, abort and cancel windows, swinger, cross zoning, fire verification, CO, panics,
  walk test, troubles and supervision. Tags `CORE-ARM`, `-BYP`, `-CHIME`, `-EXIT`, `-ENTRY`,
  `-ALM`, `-FIRE`, `-24H`, `-PANIC`, `-TEST`, `-TRBL`.
- `users`, `partitions`, `keyseq`: authority, duress, lockout, code programming, keyfob and
  keyswitch zones; independent partitions and the snapshot; key sequences with 4-10 digit
  codes. Tags `CORE-USR`, `-PART`, `-KEY`.
- `persistence`, `program`: power up in the previous state, settle time, event numbering;
  installer mode, the power-up recovery window, platform events. Tags `CORE-PWR`, `-PROG`.
- `config`, `codec`: defaults and CP-01 validation, `alarm_zone_config_resets_state()`; the blob
  round trip, compatibility both ways, damaged blobs, CRC-32 and the frozen fixtures
  (`DUMP_FIXTURE=1 build/host/test_codec` prints a new one). Tags `CORE-CFG`.
- Every suite links `alarm_sim` (`test/sim.c`: one engine, a fake clock, recording ports), as
  do the `zone_hub` tests.
