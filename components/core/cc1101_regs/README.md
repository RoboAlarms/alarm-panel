# cc1101_regs

**Tier:** core (portable C11) · **Features:** [25 RF sensors](../../../docs/features/25-RF-Sensors-315-345-433/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `cc1101_regs`)

The TI CC1101 as data and arithmetic, so the receiver's rules are proved on a PC: the
asynchronous OOK receive table for a frequency (every configuration register the panel writes,
the FREQ word and the band's TEST0 included), the 26 MHz frequency word, RSSI in dBm, what PARTNUM
and VERSION say is on the bus (a genuine chip, a clone, nothing), checking the table against what
the chip reads back, and noticing that the chip has reset under the panel (IOCFG0 back at its
reset value). `components/drivers/rf_rx` does the SPI with it.

## Public API
| Header | What it offers |
|---|---|
| `include/cc1101_regs/cc1101_regs.h` | strobe and register numbers; `cc1101_band_ok()`, `cc1101_freq_word()`, `cc1101_ook_table()`, `cc1101_verify()`, `cc1101_rssi_dbm()`, `cc1101_rssi_settle()` (RSSI only when two consecutive reads agree; else the lowest read, a lower bound), `cc1101_edges_per_s()` (the storm guard's input rate), `cc1101_state_name()`, `cc1101_identify()`, `cc1101_lost_config()` |

## Depends on
- Project components: none.
- ESP-IDF: none (portable).

## Options
None.

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; the host build stays warning-free.
- The table follows TI's datasheet (SWRS061) and DN022 and the open-source receivers; a CC1101
  has received with it on a panel since 2026-09-25 (docs/features/25 "First bench", B011, B012),
  whose fixtures are still to record. Record captures before changing it, and change the test
  with it.

## Tests
`cc1101_regs` (`test_cc1101_regs.c`, tags `RF-017`): the frequency words against TI's formula
(433.92 MHz = 0x10B071), the bands, the table (async serial on GDO0, OOK without sync word,
RX_TIME_RSSI off, TEST0 per band, every register once), read-back verification, RSSI (and its two-reads rule: unstable and bus failure told apart; an unstable one's lowest read, in dBm), the edge rate over a window, genuine /
clone / absent, reset detection, state names.
