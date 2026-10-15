# B011 - The CC1101 could be retuned by its own receptions, and its table wrote a test register

**Status: fixed in code, host-tested, flashed on a panel (2026-09-25 12:17); on air since a CC1101 was wired on J7 (2026-09-25 evening): over two
minutes of a contact tested at the panel, with `rf raw on` and `rf` polled every 5 s, `rf` kept
`state=rx(0x0D) spi_errors=0 verify_errors=0 resets=0` and the log showed no retune lines
between ~20 transmissions. The hourly recalibration's log line is not checked yet. The same
bench found [B012](B012-own-sensor-read-as-jamming-and-tamper-code.md).** Found in a code
review before the first CC1101 bench session, not seen on air. On the panel: `rf retry` runs one probe at once through
the new request path and nothing more; `rf` reads the state without SPI; no CC1101 answers on
J7 yet (`partnum 0x00 version 0x00`: nothing on the bus), so the retune and table fixes wait
for the chip. Three faults on the same path, all fixed together.

## 1. Receptions asked the task to retune or re-probe
`components/services/rf_service/src/rf_service.c` kept two commands in the rf task's notification
value (`NB_RETRY` 0x01, `NB_RETUNE` 0x02, sent with `xTaskNotify(..., eSetBits)`) and read them
with `xTaskNotifyWait(0, UINT32_MAX, &bits, ...)`. The same task is the capture's waker, and the
RMT interrupt wakes it with `vTaskNotifyGiveFromISR()` (`rf_capture.c`), which *increments* that
value. So the value the task read was a count of wake-ups, and its low bits were taken for
commands:

- two or three wake-ups before the task ran (a sensor's burst while the task sat in
  `deliver()`, or a CC1101's GDO0 toggling on band noise, which wakes it far more often) read
  as `NB_RETUNE`: `cc1101_rx_ook()` - SIDLE, the whole table rewritten and read back, SCAL,
  SRX - in the middle of a transmission, a few ms deaf each time and logged each time; on a
  noisy band it could repeat;
- an odd count with no receiver yet read as `NB_RETRY`: a CC1101 probe (reset, identify) on
  every capture wake-up instead of every `RF_SERVICE_PROBE_MS`.

**Fix:** the console's requests go in their own atomic (`S.requests`, set with
`atomic_fetch_or` and a plain `xTaskNotifyGive`); the task waits with `ulTaskNotifyTake()` and
takes the requests with `atomic_exchange()`. The notification only ever wakes the task. The
driver's header now says the waker's value is a count.

## 2. `rf status` talked to the chip from the console's task
`rf_service_status()` read MARCSTATE over SPI from whichever task called it (the `rf` command:
the console, which the Flasher's 433 MHz page polls), while the rf task reads RSSI every 200 ms.
CS is driven by hand (`cc1101.c` waits for CHIP_RDYn before clocking), so one task's
`deselect()` could end the other's transaction half-way; the chip then takes the next byte as a
new header - the RSSI read's dummy 0x00 is "write IOCFG2" - and a register could change under
the receiver. **Fix:** only the rf task talks to the chip; it keeps the state it last saw
(`S.marcstate`: after tuning, and at every 5 s check) and `rf status` reports that.

## 3. The receive table wrote FSTEST's value into PTEST
`components/core/cc1101_regs/src/cc1101_regs.c` had `{0x2A, 0x59}, /* FSTEST */`. FSTEST is
0x29 (SWRS061 table 43); 0x2A is PTEST, a production-test register TI says to leave at 0x7F
("other use of this register is for test only"). Every configuration put 0x59 there, and the
read-back check (RF-017) could not notice, since it compares with what was written.
**Fix:** `{0x29, 0x59}` (FSTEST's reset value, as SmartRF Studio exports it); PTEST and AGCTEST
are never written. New test `test_the_table_uses_the_datasheet_addresses` (RF-017) fails on the
old table and passes on the new one.

## With it (performance, not a fault)
With a receiver module, the task took the zone lock and called `zsrc_rf_carrier()` on every
wake-up (tens to hundreds a second with a module's static); `zsrc_rf_carrier()` documents one
sample a second from a module, and the duty it samples only changes when a window closes. It is
now called once per window.

## Verify on the bench (docs/features/25, "First bench")
- `rf` shows the CC1101 receiving (`state=rx`), `spi_errors=0 verify_errors=0 resets=0`, and
  stays so while the Flasher's 433 MHz page polls.
- `rf raw on` and a sensor held for a few seconds: one line per transmission, and no
  "receiving OOK at ..." lines in the log between frames (each one is a retune).
- The hourly recalibration still logs once an hour.
