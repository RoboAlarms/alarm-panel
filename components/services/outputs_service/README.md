# outputs_service

**Tier:** services (ESP-IDF) · **Features:** [16 Outputs, siren and relays](../../../docs/features/16-Outputs-Siren-and-Relays/README.md) ·
**Tests:** the portable logic's, in [`components/core/sirens/test`](../../core/sirens/test/)

The panel's own sirens and outputs: the wired outputs on the board's terminals and the 433 MHz
sirens, both from [`components/core/sirens`](../../core/sirens/). Zigbee and Z-Wave sirens are their
radios' services; every kind holds the bell trouble as its own owner
(`alarm_service_bell_trouble`).

- The engine's news comes from the alarm task through `panel_app`'s listener
  (`outputs_service_engine_output`, `outputs_service_engine_snapshot` for the squawks), queued;
  nothing there blocks.
- Its own task (internal stack, priority 8) runs the patterns at their deadlines, reads the
  supervision inputs every 100 ms, sends the 433 MHz codes (`rf_service_transmit`) and stores the
  configuration (NVS `outputs`: `wired`, `rf`).
- A wired output's terminal is taken from the wired zones (`zone_service_reserve_terminal`) and
  given back when the output goes; a terminal a zone uses is refused.
- Console: `out` lists everything; `out wired <n> siren|strobe|smoke|armed <gpio> [parts 1,2]
  [also 3] [burglary|fire|both] [squawk] [low] [sense <gpio>]`, `out wired <n> test|clear`;
  `out rf add [...]`, `out rf <n> pair|test|clear|set [...]|codes <alarm> <stop> <arm> <disarm>`.

## Public API
| Header | What it offers |
|---|---|
| `include/outputs_service.h` | `outputs_service_start()`, the engine's hooks, the wired outputs' and 433 MHz sirens' reads and changes for the screens, `outputs_service_console_register()` |

## Depends on
- Project components: `alarm_core`, `sirens`, `platform_esp` (alarm and zone services), `rf_service` (the transmitter)
- ESP-IDF: `esp_driver_gpio`, `nvs_flash`, `console`

## Options
- `CONFIG_ALARM_OUTPUTS` (default on): off, every call answers "not supported" and nothing is driven.
