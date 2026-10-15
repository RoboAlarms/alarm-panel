# event_log

**Tier:** core (portable C11) · **Features:** [05 Event log](../../../docs/features/05-Event-Log/README.md), [10 MQTT and Home Assistant](../../../docs/features/10-MQTT-and-Home-Assistant/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `elog`)

A power-fail-safe history of what the panel did: a ring of fixed 64-byte records in raw flash,
appended sector by sector. It is read newest first for the event viewer and oldest first for
MQTT store-and-forward (the reporter in `alarm_proto` replays from it). Every record carries a
CRC-32 and its own index, so a power cut during a write or an erase leaves at most one
unreadable slot, which is skipped and never rewritten in place; mounting scans every slot and
recovers the newest index and the last engine sequence number. It does not know the flash: the
platform passes an `elog_flash_t` (read, write, erase with NOR semantics), so the same code runs
on an ESP-IDF partition (`platform_esp/src/panel_log.c`), on a RAM "flash" in the PC preview and in
the tests. No heap, no globals, no RTOS, no locks: one writer at a time, and the platform
serialises the calls.

## Public API
| Header | What it offers |
|---|---|
| `include/event_log/elog.h` | `elog_open` (mount and recover), `elog_append`, `elog_read` (newest first, paged, filtered, scan-bounded), `elog_read_after` (oldest first, for replay), `elog_clear`, `elog_capacity` / `elog_newest_index`, and the record codec `elog_encode` / `elog_decode` for tests and tools |

## Depends on
- Project components: `alarm_core` (`alarm_event_t` from `alarm_types.h`, `alarm_crc32()` from
  `alarm_codec.h`).
- ESP-IDF: none (portable).

## Options
None. The geometry comes at run time from `elog_flash_t`: `size` is a multiple of
`sector_size` and at least two sectors, `sector_size` a multiple of `ELOG_RECORD_SIZE` (64).
The ring keeps `elog_capacity()` records: every slot except the sector erased ahead of the head.

## Rules
- Plain C11 like `alarm_core`: no malloc, no globals, no RTOS or ESP-IDF headers; the host
  build stays warning-free.
- The 64-byte record layout (version 1, documented in `src/elog.c` and docs/features/05) is an
  external contract: new fields go only in the reserved bytes (offset 39, 21 bytes), so older
  firmware can still read newer records.
- Records store `alarm_event_type_t` values, which is why `alarm_core` only ever appends new
  event types.
- Tests reference `LOG-NNN` from docs/features/05.
  (CONTRIBUTING.md, "Rules for the portable components")

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R <suite>`.
- `elog` (`test_elog.c`, on a simulated NOR flash): the record codec, append and read, sparse
  filters with a scan limit, wrap-around and even wear, sequence recovery, forward reads for
  replay (in order, over overwritten and torn slots), a power cut after every byte written or
  erased across a sector boundary, a refused or torn write that is tried again, a garbage
  partition, clear and geometry checks. Tags `LOG-001`..`LOG-007`, `LOG-010`.
- Replay from a real log in RAM is also covered by `alarm_proto`'s `report` suite (`LOG-006`).
