# rtc_ds3231

**Tier:** drivers (ESP-IDF) · **Features:** [35 Battery-backed clock](../../../docs/features/35-Battery-Backed-Clock/README.md) ·
**Tests:** what the registers mean is host-tested in [`rtc_clock`](../../core/rtc_clock/test/); the
driver itself on a board

A DS3231 real-time clock module on an I2C bus: an address probe at 0x68 (nothing is written, so
a panel without a module is left exactly as it was), then register reads and writes, each one
transaction bounded by `RTC_DS3231_TIMEOUT_MS` (50 ms). At 100 kHz: modules sit on a lead, and
the whole register file is 19 bytes. The bus is the board's (`board_i2c_ext()`), shared with its
own parts (the CrowPanel's GT911): the ESP-IDF I2C master driver runs one transaction at a time.

## Public API
| Header | What it offers |
|---|---|
| `include/rtc_ds3231/rtc_ds3231.h` | `rtc_ds3231_open(bus)` (probe + add the device), `rtc_ds3231_read(reg, out, len)`, `rtc_ds3231_write(reg, data, len)`; `RTC_DS3231_CLOCK_HZ`, `RTC_DS3231_TIMEOUT_MS` |

## Depends on
- Project components: `rtc_clock` (the register numbers)
- ESP-IDF: `esp_driver_i2c`

## Options
None.

## Rules
- One module per panel (one device handle); it knows nothing about time, only registers.
- The buffers the I2C interrupt reads are the caller's: call it from a task whose stack is
  internal RAM (`panel_clock`'s is).
