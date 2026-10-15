# rtc_clock

**Tier:** core (portable C11) · **Features:** [35 Battery-backed clock](../../../docs/features/35-Battery-Backed-Clock/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `rtc_clock`)

The DS3231 real-time clock as data, and the rules for which clock the panel believes. Both are
proved on a PC; `components/drivers/rtc_ds3231` moves the bytes and
`components/services/panel_clock` runs the rules on the panel.

- **`ds3231.h`**: the register file (0x00..0x12), UTC calendar arithmetic of its own (C11 has no
  `timegm`), BCD time in and out (24-hour mode, the century bit, a clock another device left in
  12-hour mode), telling a DS3231 from another chip at 0x68 by the bits the datasheet says read
  0 (a DS1307, an MPU-6050), the status (oscillator-stop flag, 32 kHz output, temperature in
  quarter degrees) and the control/status values for a clock that lives on its coin cell.
- **`clock_keeper.h`**: which clock the panel believes. At start-up a module that kept a valid
  time sets the panel's clock; one that lost it (OSF, an impossible date, before 2025) is not
  believed. Each time-server sync writes the module; while the internet is trusted (a day after
  a sync) a module a second or more off is rewritten. Without the internet the module is the
  reference: compared hourly on its second tick, the panel's clock is slewed to it
  (250 ms..60 s); a bigger difference means the module jumped and it is put back. A person may
  set the time only while a module answers. Three bus failures in a row: "not answering".

## Public API
| Header | What it offers |
|---|---|
| `include/rtc_clock/ds3231.h` | register numbers and bits; `rtc_days_from_civil()`, `rtc_days_in_month()`, `rtc_civil_from_epoch()`, `rtc_epoch_from_civil()`; `ds3231_encode_time()`, `ds3231_decode_time()`, `ds3231_identify()`, `ds3231_parse_status()`, `ds3231_control_wanted()`, `ds3231_status_wanted()` |
| `include/rtc_clock/clock_keeper.h` | `clock_keeper_t` and its limits; `clock_keeper_start()`, `_internet()`, `_manual()`, `_write_due()`, `_written()`, `_check_due()`, `_compare()`, `_can_set()`, `_internet_trusted()`, `_epoch_ok()`; `clock_module_name()`, `clock_source_name()` |

## Depends on
- Project components: none.
- ESP-IDF: none (portable).

## Options
None (the firmware's switch is `CONFIG_ALARM_FEATURE_RTC`, read by `panel_clock`).

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; the host build stays warning-free.
- The keeper decides and never touches a clock itself: the service applies its actions
  (`settimeofday`, `adjtime`, a module write) and reports back.
- The module keeps UTC; the time zone is the panel's (docs/features/09).

## Tests
`rtc_clock` (`test_rtc_clock.c`, tags `RTC-001..RTC-009`): the calendar against known moments and
every day from 2000 to 2199, impossible dates, the time registers (BCD, 24-hour, century,
12-hour mode read back), garbled registers refused digit by digit, identification against other
chips, status and temperature below zero, the coin-cell set-up, and the keeper: no module,
another chip, a module that kept the time, one that lost it, internet writes and rewrites,
following the module without the internet (slew, drift left alone, a jump put back), setting by
hand, a module that stops answering and comes back, losing and regaining the time while running.
