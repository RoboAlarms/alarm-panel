# rf_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[25 RF sensors](../../../docs/features/25-RF-Sensors-315-345-433/README.md),
[22 Zone sources](../../../docs/features/22-Zone-Sources/README.md) · **Tests:** the decoding and the
source are host-tested ([rf_decode](../../core/rf_decode/test), [zone_hub](../../core/zone_hub/test)
`rf`); the receiver is checked on a panel with the console's `rf` command

The 433 MHz receiver as a zone source: it drives a CC1101 over SPI or listens to a fixed
receiver module's data line (`components/drivers/rf_rx`), decodes the pulse trains
(`components/core/rf_decode`) and feeds `zsrc_rf` through the zones task. `auto` probes SPI for
a CC1101 and otherwise recognises a module by the static on the line (ADR-044). The pins come
from the app: the board's wiring (`board_info_t.rf`) unless Kconfig names others, validated
against the board before anything is configured (docs/features/33, BRD-009).

Split out of `platform_esp` in docs/features/33 step 6.

## Public API
| Header | What it offers |
|---|---|
| `include/rf_service.h` | `rf_service_start()` (`rf_service_config_t`: SPI host, pins, invert, band, module), `rf_service_status/retry/set_frequency/set_module/raw()`, `rf_service_source()` (the `zsrc_rf` it feeds), `rf_service_pin_rules()` / `rf_service_stored_pin()` / `rf_service_stored_place()` / `rf_service_stored_custom()` (`rf pin`, RF-008; `rf place`, RF-019; `rf place custom`, RF-025 - the app decides what the board allows), `rf_service_console_register()` (the `rf` command) |

## Depends on
- Project components: public `zone_hub`; private `platform_esp` (zones task), `rf_rx`, `rf_decode`, `alarm_mem`, `board_info` (`board_custom_parse`)
- ESP-IDF: public `esp_driver_spi`; private `console`, `esp_timer`, `nvs_flash`

## Options
| Kconfig (`Kconfig`) | Default | Effect |
|---|---|---|
| `ALARM_ZSRC_RF` | y | The source and its receiver; off, the service answers "not in this firmware" (stubs) |
| `ALARM_RF_SPI_MOSI/MISO/SCK/CS_GPIO`, `ALARM_RF_GDO0_GPIO` | -1 (the board's wiring) | Other pins for the receiver |
| `ALARM_RF_MODULE` | auto | CC1101 only, module only, or automatic (`rf module` overrides, remembered; `rf module off` turns the receiver off, RF-020) |
| `ALARM_RF_FREQ_KHZ` | 433920 | The band (`rf freq` overrides, remembered) |
| `ALARM_RF_INVERT` | n | The data line idles high |

## Resources
- Task `rf`: 4096 bytes, internal, priority 4; about 5 KB of internal RMT buffers in `rf_rx`.
- The console's capture dump takes slow memory (`alarm_mem`) only while capturing.
- NVS namespace `rf`: the band (`khz`), module (`mode`), data pin (`pin`) and place (`place`) chosen on the console; a custom place's pins (`u8`: `c_sck` `c_mosi` `c_miso` `c_cs` `c_gdo0` for a CC1101, `c_data` for a receiver module; RF-025, ADR-048), the place erased first and written last; a store that fails part-way writes back the place, mode or custom pins there were before. Leaving a custom place (`rf place default|<place>`, `rf module off`) stores the new place first and is done once it is stored; forgetting the old custom pins is a tidy-up after it, logged when it fails and tried again by the next `rf place` or by a start of the service (the app skips that start when the receiver's wiring isn't safe; left over, the pins are unused) (RF13). `rf freq` tunes only to a frequency it could store (RF11).

## Console (`rf`)
`status` · `retry` · `freq <kHz>` · `module auto|cc1101|ook|off` · `pin <gpio>|default` ·
`place [<place>|default]` · `place custom sck=<g> mosi=<g> miso=<g> cs=<g> gdo0=<g>` or
`place custom data=<g>` (RF-025: replies start "place custom from the next restart (`restart`)"
or "not custom: "; read back as "custom sck=.. mosi=.. miso=.. cs=.. gdo0=.." or "custom data=.."
in `rf` and `rf place`; while custom, `mode` stays `off`, `rf module cc1101|ook|auto` is refused
and `rf module off` takes the place away) · `raw on|off` · `capture <ms>`. `rf status` ends
with lines the Flasher does not read; `rssi unstable=<n>` counts the CC1101's RSSI samples whose
reads never agreed (RF-017, RF14).

## Rules
- One-way 433 MHz sensors send no heartbeat and can be replayed: the source is not supervised
  and not trusted for fire, CO or 24-hour zones by default (docs/features/22 ZSRC-008).
- The data line must never be a strapping pin (it idles high on noise and would hold the strap
  through a reset); the app's validation refuses one.
- Off means stubs: callers need no guard (components/services/alarm_features).
- Jamming (RF-005) fails toward being reported: an RSSI sample whose reads never agreed still
  counts as a carrier when even its lowest read is at `ZSRC_RF_JAM_DBM`, and a module's window
  with an edge storm or its pause in it counts as a carrier.

## Tests
No host tests of its own (ESP-IDF code). The decisions it delegates are host-tested: the RSSI
two-reads rule and its lowest read, and the storm guard's edge rate in `cc1101_regs`; the source
and jamming in `zone_hub` (suite `rf`). By firmware review only: the order of RF11 (the
frequency stored before the retune) and RF13 (the place stored before the old custom pins are
forgotten), the unstable sample's use and the storm window counted as a carrier.
