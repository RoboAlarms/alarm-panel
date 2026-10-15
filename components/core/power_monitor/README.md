# power_monitor

**Tier:** core (portable C11) · **Features:** [17 Power and supervision](../../../docs/features/17-Power-and-Supervision/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `power_monitor`)

Mains and battery supervision without hardware. A power source (a read function the board
supplies) returns a sample with three-state values - unknown, no, yes - for external power,
battery present, battery low and charging, plus voltage and percentage (-1 when unavailable).
The monitor qualifies each known change for 5 s, drops to unknown at once on a failed read, and
restarts qualification after more than 15 s between polls or a clock that ran backwards. It also
decodes the Elecrow STC8H1K08 helper's battery bank, caps the backlight while saving power and
decides when power saving starts and ends. It exists so that "unknown never looks healthy" is a
host test. It does not read the I2C bus (the board's `board_battery.c` does, from the health
task), keep a clock, raise troubles or touch the sounder.

## Public API
| Header | What it offers |
|---|---|
| `include/power_monitor.h` | `power_value_t`, `power_sample_t`, `power_source_t` (read callback and context), `power_monitor_t`; `power_monitor_init()`, `power_monitor_poll()`, `power_source_none()` (everything unknown), `power_decode_stc()` (the 11-byte STC bank), `power_backlight_limit()` (20 % cap while saving), `power_saving_t` and `power_saving_update()` |

## Depends on
- Project components: none
- ESP-IDF: none (portable)

## Options
None.

## Rules
- Unknown is zero and never implies healthy power: an unreadable battery is not an absent one,
  and a failed read is not a mains loss (PWR-010). Poll at least every 5 s with monotonic ms.
- Voltage, percentage and the low flag are reported only while the battery's presence is
  confirmed; the percentage is never estimated from a voltage (PWR-003).
- STC frames are decoded byte by byte and checked against the V1.1 divider (39k/100k): an empty
  connector reading about 4581 mV and 100 % must not become a battery. Charging or full proves
  external power; "not charging" does not prove it is gone.
- Low battery at 3500 mV or below, restored at 3700 mV or above, held in between. Entering and
  leaving power saving both need 15 s of stable evidence.
- Portable rules (CONTRIBUTING.md): C11, no heap, no globals, no RTOS or ESP-IDF headers, time as
  `uint64_t` ms; warning-free host build and test (`STRICT`).

## Tests
Run all with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite alone with `ctest --test-dir build/host -R power_monitor`.
- The `none` source reports unknown, never a healthy reading (PWR-003).
- Loss and restore qualified for 5 s, a bouncing input rejected; failed reads and polling gaps
  cannot qualify a change (PWR-010).
- A disconnected battery suppresses voltage, percentage and the low flag.
- The STC decoder on the real empty-connector capture, valid frames and malformed ones.
- Low-battery hysteresis, the backlight cap (PWR-004/005) and power saving settling after 15 s.
Battery-connected acceptance is still pending: the panel tried has no battery
(docs/features/17).
