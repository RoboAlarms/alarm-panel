# rf_rx

**Tier:** drivers (ESP-IDF chip driver) · **Features:** [25 RF sensors 315/345/433](../../../docs/features/25-RF-Sensors-315-345-433/README.md) ·
**Tests:** on hardware (see docs/features/25)

The hardware half of the 433 MHz receiver: a TI CC1101 on SPI, run as an asynchronous OOK
receiver, and an RMT capture that turns a receiver's data line (the CC1101's GDO0, or a fixed
superheterodyne module's DATA) into pulse widths. It knows registers, pins and interrupts and
nothing else: the pulses go to `components/core/rf_decode`, the codes to `zone_hub`'s
`zsrc_rf`, and `components/services/rf_service/src/rf_service.c` runs the task, picks the receiver
(auto, CC1101 or module), supervises it and hosts the `rf` console command. The panel never
transmits.

## Public API
| Header | What it offers |
|---|---|
| `include/rf_rx/cc1101.h` | `cc1101_t`, `cc1101_pins_t`; `cc1101_open()`, `cc1101_probe()` (reset and identify), `cc1101_rx_ook()` (band register table, calibration, RX with data on GDO0), `cc1101_strobe()`, `cc1101_read()`, `cc1101_write()`, `cc1101_marcstate()`, `cc1101_read_rssi()` (stable, unstable - no two consecutive reads agreed, the sample is skipped - or a bus error; RF-017), `cc1101_state_name()`, `cc1101_rssi_dbm()`, `cc1101_freq_word()`; strobe and status register numbers |
| `include/rf_rx/rf_capture.h` | `rf_capture_t`, `rf_capture_pulse_t`; `rf_capture_start()`, `rf_capture_pause()`, `rf_capture_stop()`, `rf_capture_next()` (the next pulse; width 0 ends a transmission); 1 us ticks, the ~3 us glitch filter, 30 ms idle (B025), ring sizes, `RF_CAPTURE_STORM_EDGES`; counters `symbols` (queued), `overruns` (dropped) and `received` (both: the input rate the storm guard measures) |

## Depends on
- Project components: `cc1101_regs` (the receive table, read-back, reset detection: host-tested)
- ESP-IDF: `esp_driver_spi`, `esp_driver_rmt`, `esp_driver_gpio` (public; the headers also use
  FreeRTOS task handles), `esp_timer` (private)

## Options
None in this component. The pins, receiver choice, frequency and line inversion are
`platform_esp` options (`CONFIG_ALARM_ZSRC_RF`, `CONFIG_ALARM_RF_SPI_MOSI/MISO/SCK/CS_GPIO`,
`CONFIG_ALARM_RF_GDO0_GPIO`, `ALARM_RF_MODULE`, `CONFIG_ALARM_RF_FREQ_KHZ`,
`CONFIG_ALARM_RF_INVERT`) that `rf_service.c` passes in.

## Rules
- Drivers tier (docs/features/33): no board, service or UI knowledge; every pin comes from the
  caller.
- The RMT interrupt handler is in IRAM and the capture buffers are internal RAM: anything an
  ISR touches stays internal (CONTRIBUTING.md, "Rules for the firmware"; docs/bugs/B001). The interrupt is
  not cache-safe (`CONFIG_RMT_RX_ISR_CACHE_SAFE` is off), so it waits while flash is written: a
  short write costs a sensor a repeat or two of its frame, a long one (an erase) can cost a
  transmission. Making it cache-safe also needs `CONFIG_RMT_RECV_FUNC_IN_IRAM`, since the
  handler re-arms the receive; measure on the bench before changing it.
- CS is driven by hand so the driver can wait for CHIP_RDYn (MISO low) before the first clock.
- The data pin gets a pull-down, so an unwired pin shows no edges. GDO0 must not sit on a
  strapping pin: it idles high on noise and would hold the strap through a reset.
- The OOK register table lives in `components/core/cc1101_regs` (host-tested); it follows
  open-source receivers and TI's advice but has not been proved on this panel's air yet;
  record captures before changing it. Every configuration is read back; `cc1101_check()` finds
  a chip that reset (RF-017).

## Tests
No host tests: this is register and peripheral code. It is checked on the bench with the
service console (`python tools/panel_console.py -p <PORT> rf`, port from `PANEL.local.md`),
following "First bench" in docs/features/25:
- `rf` (`rf status`): a CC1101 is found (version 0x14) and receiving at the set frequency with
  its noise floor, or a module shows its edges per second and line-high share; `rf retry` after
  fixing the wiring.
- `rf raw on`: one decoded line per transmission; `rf capture 3000` dumps the raw pulses (frames
  upside down mean `CONFIG_ALARM_RF_INVERT`).
- `rf freq <kHz>` retunes a CC1101; `rf module auto|cc1101|ook` fixes the receiver choice.
- Captures of real sensors become fixtures under `components/core/rf_decode/test/rf/`.
The portable parts are host-tested elsewhere: decoding in `rf_decode` (suite `rf_decode`), the
source, learning and jamming in `zone_hub` (suite `rf`), the RSSI two-reads rule and the edge rate in
`cc1101_regs`. By firmware review only: the RMT callback counts `received` before the ring's room
clips it (RF12). Status: in [docs/features/25](../../../docs/features/25-RF-Sensors-315-345-433/README.md).
