# 36 · Z-Wave: the ZAC93 module and where it connects

Part of [docs/features/36](README.md). Facts about the module come from Zooz's manual, FAQ,
installation guide and change log (links at the end, read 2026-09-25); facts about the
CrowPanel come from `docs/hardware/`, `board_pins.h` and
[the add-on wiring research](../../research/AddOnWiringResearch.md). Anything not verified on a
bench says so. **A ZAC93 was wired to a panel on the bench on 2026-09-26** (the no-sensor checks
of §7, README Z9); what this page marks "to verify", "to confirm" or "measure" is not recorded
here yet.

## 1. The module

| | |
|---|---|
| Product | Zooz ZAC93 LR, "800 Series Z-Wave Long Range GPIO Module" |
| Radio chip | Silicon Labs 800 series; the Serial API reports chip type `08` (EFR32ZG23 / ZGM230S family) |
| Firmware | Zooz's build of Silicon Labs' **static controller** Serial API ("Z-Wave Plus Static Controller") |
| Supply | 3.3 V DC (current not published by Zooz: to measure, section 6) |
| Interface | UART, 115200 baud 8N1 (Zooz's ser2net example), 3.3 V logic |
| Connector | 2x5 female header, made to sit on pins 1-10 of a Raspberry Pi header |
| Antenna | on-board trace antenna, -2.75 dBi; Zooz describes an external-antenna modification (IPEX4 connector, 915 MHz antenna) that voids the warranty |
| Frequencies | 908.42 MHz (US/CA/MX, with Long Range), 868.42 MHz (EU), 921.4 MHz (AU/NZ): separate module variants |
| Size | 1.1 x 1.1 in |
| Environment | indoors, 32-104 °F (the FAQ says 14-104 °F), up to 85 % humidity |
| Factory reset | only from the host software (Serial API `HardReset`); no button does it |
| Identification | manufacturer `027A`, product type `0004`, product ID `0611` (firmware 2.20+ may report Silicon Labs defaults instead, section 2) |

The module has a small push button next to the chip (visible in Zooz's photo); its function is
not documented. The panel never relies on it (no flow may need a physical button,
ADR-014).

**Hardware version 2** (from November 2024, "Version 2.0" laser-etched on the board): header
position 7 is connected to the Z-Wave chip's **reset** input. Version 1 boards have nothing
there.

**LED** (firmware 2.10+): solid red when powered, **blinking red when it receives commands**.
It is on the module, not the screen, so the display-safety rules (no flashing on screen,
UI-020) do not cover it - but a panel lives on a wall in the dark: mount the module so the LED
cannot be seen through the case's openings, or cover it (open question 5).

## 2. Firmware versions (Zooz change log)

| Version | Date | SDK | Notes |
|---|---|---|---|
| 1.01 | 12/2022 | 7.18.1 | first release |
| 1.03, 1.04 | 12/2022, 02/2023 | 7.18.3 | |
| 1.10 | 05/2023 | 7.18.3 | OTW update fixes |
| 1.20 | 07/2023 | 7.19.3 | |
| 1.30 | 05/2024 | 7.21.3 | |
| 1.40 | 07/2024 | 7.22.0 | |
| 1.50 | 09/2024 | 7.22.1 | **the minimum the panel recommends** (800-series SDKs before 7.22.1 have known stability bugs) |
| 2.00 | 11/2024 | 7.22.1 | hardware version 2 (reset on position 7); 1.xx boards can take 2.00+ |
| 2.10 | 07/2025 | 7.23.2 | LED indicator; Long Range for Europe |
| 2.20 | 03/2026 | - | fixes updating from 1.50/2.00 (bootloader error `0x18`); firmware version and product type now the SDK defaults |
| 2.30 | 08/2026 | 7.24.2 | |

Updates are "over the wire" through the host (serial-api.md §8); only Zooz's files for the
ZAC93 are allowed. They take 5-15 minutes (Zooz).

## 3. The header

Zooz: the module plugs onto **pins 1-10** of a Raspberry Pi header and uses the Pi's UART
(`/dev/ttyAMA0`, with Bluetooth disabled). A working ESP32 build in the Home Assistant community
wired **four** connections: 3.3 V, ground, TX and RX. In Raspberry Pi numbering:

| Position | Raspberry Pi signal | ZAC93 use | Connect to the panel |
|---|---|---|---|
| 1 | 3.3 V | **power** | 3V3 |
| 2 | 5 V | - | **nothing** |
| 3 | GPIO2 (SDA) | - | nothing |
| 4 | 5 V | - | **nothing** |
| 5 | GPIO3 (SCL) | - | nothing |
| 6 | GND | **ground** | GND |
| 7 | GPIO4 | **reset** (hardware v2 only) | optional: a free GPIO, open-drain (section 5) |
| 8 | GPIO14 (Pi TXD) | **module RX** | the panel's **TX** |
| 9 | GND | ground | GND (optional second ground) |
| 10 | GPIO15 (Pi RXD) | **module TX** | the panel's **RX** |

**To verify before wiring:** Zooz's own pinout picture (FAQ "Can I use the ZAC93 with an
Odroid-C4") failed to load on 2026-09-25, so the positions above are the Raspberry Pi header's,
which Zooz says the module follows. With the module unpowered, check with a meter that
position 1 and 6 are the module's supply and ground (6 and 9 connected together), and never
feed 5 V to any position.

Cables: Dupont female-to-female jumpers from the ZAC93's header (it takes male pins: use
male-to-female, or a 2x5 IDC ribbon with a breakout) to the panel's pins. Keep the four UART
wires short (10-20 cm) and together.

## 4. Where it connects on the CrowPanel 7" (V1.1)

The P4 routes any UART to any GPIO; the Z-Wave service uses **UART3** (UART0 is the console,
UART2 is reserved for the ESP32-H2, docs/features/20). Two places are offered, chosen at run time
like the 433 MHz receiver's and the clock module's places (`zwave place socket|j7`, stored in
NVS, also set from the Flasher):

### Place "socket" (recommended while no ESP32-H2 is fitted)

The wireless-module socket carries the P4's radio UART, made for a radio co-processor
(`board_info.radio`, `BOARD_RADIO_UART_*`).

| ZAC93 | Socket pad | P4 GPIO |
|---|---|---|
| 1 (3.3 V) | 3V3 | - |
| 6 (GND) | GND | - |
| 8 (module RX) | IO53 (`P4_TXD2`) | 53 (panel TX) |
| 10 (module TX) | IO54 (`P4_RXD2`) | 54 (panel RX) |
| 7 (reset, v2) | - | optional: a J7 terminal, e.g. IO4 (`zwave reset-pin 4`) |

- Excludes the Elecrow ESP32-H2 module (it plugs into the same socket, docs/features/20). The
  CC1101's default place (the socket's SPI pads IO6-IO10) is electrically separate, but both
  add-ons then wire to the socket; move the CC1101 to J7 (`rf place j7`) to keep the socket
  tidy.
- On **V1.2** boards the socket UART is GPIO27/28 instead (a board variant, docs/features/20).

### Place "j7"

| ZAC93 | J7 pin | P4 GPIO |
|---|---|---|
| 1 (3.3 V) | 3V3 (row B, columns 1-3) | - |
| 6 (GND) | GND (column 4 or 11-12) | - |
| 8 (module RX) | IO27 | 27 (panel TX) |
| 10 (module TX) | IO28 | 28 (panel RX) |
| 7 (reset, v2) | optional: a free terminal | `zwave reset-pin <gpio>` |

- Conflicts with the battery-backed clock's J7 place (`rtc place j7` uses IO27/IO28): the clock
  then goes back to the I2C connector J13. IO27 is also the D+ of the P4's unused second USB
  PHY and comes up with a pull-up (harmless for a UART TX idle-high line; to confirm).
- Check J7's rows (5V vs 3V3) with a meter first (the research note's warning).

### With everything fitted: ESP32-H2, CC1101, DS3231 and ZAC93

Pins from `board_pins.h` (the CC1101's J7 place, the clock's places) and the add-on wiring
research; the ZAC93's place is `zwave place j7` (built at Z8: `board_pins.h`
`BOARD_J7_ZWAVE_*`). J7 then has only IO2, IO3 (and IO4 without the ZAC93's reset) left for
wired zones.

| Module | Connects to | Signals | Firmware |
|---|---|---|---|
| ESP32-H2 (Zigbee) | wireless-module socket (plugs in) | its own UART, GPIO53/54 | not yet (2026-09-25): Zigbee has no backend (docs/features/20) |
| CC1101 (433 MHz) | J7 | SCK IO52, MOSI IO51, MISO IO50, CSn IO49, GDO0 IO5 | works (2026-09-25): `rf module cc1101`, `rf place j7` |
| ZAC93 (Z-Wave) | J7 | panel TX IO27 → ZAC93 position 8; panel RX IO28 ← position 10; optional reset IO4 → position 7 (hardware version 2 only) | built (Z8, 2026-09-26): `zwave place j7`, `zwave reset-pin 4` |
| DS3231 (clock) | J13, the I2C connector | SCL, SDA, 3V3, GND as printed next to J13 | works (2026-09-25): `rtc place default` |

The clock must use J13 here: its J7 place (IO27/IO28, `rtc place j7`) is the ZAC93's.

```
col:   1      2      3      4    5     6      7      8      9      10      11   12
A:    5V     5V     5V     GND  GND   IO52   IO51   IO50   IO49   IO28    GND  GND
                                      CC SCK CC MOSI CC MISO CC CSn ZAC TX→  CC GND ZAC GND
B:    3V3    3V3    3V3    GND  IO2   IO3    IO4    IO5    IO25   IO27    GND  GND
      CC VCC ZAC VCC        (spare) zone  zone   ZAC rst CC GDO0 leave  →ZAC RX
```

"ZAC TX→" is the ZAC93's TX (header position 10) arriving on the panel's RX, IO28; "→ZAC RX" is
the panel's TX, IO27, going to the ZAC93's RX (position 8).

- **CC1101:** VCC to 3V3, GND to GND; GDO2 unconnected; a 433 MHz antenna or 17.3 cm of wire.
- **ZAC93:** position 1 to 3V3, position 6 to GND, position 8 to IO27, position 10 to IO28;
  position 7 to IO4 only on a board etched "Version 2.0" (older boards have no reset there).
  Never connect positions 2 or 4 (5 V on a Pi header).
- **DS3231 (ZS-042):** VCC to 3V3, never 5 V; SQW and 32K unconnected; with a CR2032, remove the
  charging diode or resistor, or fit an LIR2032 (docs/features/35).
- **Wired zones left:** IO2 and IO3, plus IO4 when the ZAC93's reset is not wired. IO25 stays
  free (the P4's USB D+).

Before wiring:
1. **Check J7's rows with a meter:** the vendor photo does not show which row is 5V and which
   3V3. Every module here is 3.3 V only, and J7 is not 5 V tolerant.
2. **Check the ZAC93's header positions** (section 3): Raspberry Pi numbering, unverified;
   confirm on the unpowered module that positions 1 and 6 are its supply and ground.
3. **Keep the clock's leads short** (10-20 cm): J13 is the touch screen's bus; a faulty module
   or long leads can freeze touch (the add-on wiring research).
4. **Power:** the 3.3 V outputs are rated 1 A (Elecrow). The CC1101, DS3231 and H2 draw tens of
   mA together; the ZAC93's draw is not published: measure it (section 6).
5. **Antennas** apart from each other and away from the P4 and the display ribbon.

**Alternative, the clock off the touch bus** (not taken: Z8 fixed the ZAC93's J7 place at
IO27/IO28): the ZAC93 could take IO2/IO3 (reset on IO4) instead, freeing IO27/IO28 for the
clock on its own I2C controller (`rtc place j7`, works today). The cost: no wired-zone terminal
is left. Worth it only when the panel will have no wired zones.

### The firmware side (built at Z8)

- `board_info_t` gains `zwave_places[]` (`id`, `tx`, `rx`, `where`), next to `rf_places` and
  `i2c_places`; `board_info_validate()` refuses a place whose pins are in use by the chosen
  places of the other add-ons (the same check that keeps the CC1101 off wired terminals).
- The reset pin, when set, is taken out of the wired-terminal list the way `board_rf_data_on()`
  takes the 433 MHz module's data pin.
- Pins are configured only when the Z-Wave service starts with a place chosen: a panel without
  a ZAC93 touches none of them (`zwave place off` is the default until the installer or the
  Flasher picks one).

## 5. The reset line

Optional, hardware version 2 only. Drive the chosen GPIO **open-drain**: low for 20 ms to
reset, then released (high impedance) - never driven high, so a wrong wire cannot fight the
module. Silicon Labs' EFR32 reset input has an internal pull-up (to confirm on the bench that
the line idles high with the GPIO released). The panel uses it only when the module stops
answering on the Serial API (serial-api.md §7) and at start-up; without it, a hung module stays
down until the panel restarts (power cycle), and the Z-Wave page says "Connect the radio's reset
pin to recover it without a restart".

## 6. Power and RF

- **Supply**: the board's 3.3 V (the wiki gives 1 A for the 3.3 V outputs). Zooz does not
  publish the module's current; an 800-series chip at its highest (Long Range) power draws far
  more than when listening. **Measure it on the bench** (idle, receiving, transmitting on Long
  Range) and add it to the battery-runtime budget (docs/features/17).
- **Antenna**: inside the case, away from the P4, the display ribbon, the C6's 2.4 GHz antenna
  and the 433 MHz antenna; not flat against metal or the wall's back box. Zooz recommends a
  horizontal module for best reception (their answer about a case).
- **Regulatory**: the module's FCC grant says it must stay at least **20 cm** from people and
  must not be co-located with other transmitters; the latter is a condition for the host
  product's certification. A DIY panel is not a certified product; the docs for builders repeat
  the 20 cm distance and name the co-location condition without claiming compliance.

## 7. Bench (the panel with the ZAC93, through the developer tools)

Wiring and the console, as before:

1. Meter: module positions 1/6/9, J7 or socket 3V3 and GND.
2. Wire the place; `zwave place socket` (or `j7`); `zwave status`: finds the module, prints
   firmware, SDK, region, home ID, node count.
3. Measure the supply current idle and during an inclusion.
4. With hardware version 2: `zwave reset-pin <gpio>`; `zwave reset` makes the module restart
   (`SerialAPIStarted`, reason "reset"). An accepted command and a still-Working status are
   insufficient: capture a new SerialAPIStarted and the completed startup. The 2026-09-26
   stricter bench check found no response to the configured reset pulse; wiring and hardware
   revision still need confirmation. Software reset works.
5. `display watch 60` while the module is busy: no late frames (UART interrupts must not
   disturb the display; the DSI ISR is cache-safe, docs/features/06).
6. The LED: visible through the case? Decide on covering it.

Then, from the PC over the enrolled developer link (docs/features/30 `panelctl`) - this needs
only the ZAC93, no sensor:

7. `python tools/panelctl.py zwave check --device <alias> --yes` says what it is about to do
   (it restarts the radio and opens the add and remove windows for a few seconds) and refuses
   without `--yes` or an interactive yes; the panel must be disarmed. It then checks, each
   PASS/WARN/FAIL with a reason: status (radio working, crypto self-test passed, chip 8, firmware
   and SDK read - WARN under SDK 7.22.1 - node 1, region known), a restart (back to working within
   30 s, home ID and device count unchanged), an add window and a remove window (each opens,
   stops after 3 s, ends stopped/nothing joined or removed, dismissed), a pulse-reset (skipped,
   not a failure, where the hardware doesn't support it) and a final status. It captures the
   module's frame trace throughout and writes a report, all under `build/zwave-check-<date>/` by
   default (`--output-dir` for another folder), and exits 1 on any FAIL.

Then, with one sensor (a Zooz ZSE41 is enough):

8. `python tools/panelctl.py zwave check --device <alias> --yes --sensor` adds a guided pass
   after the radio checks: put the sensor in pairing mode, grant what it asks for, compare the
   PIN with the label on the device, wait for the interview to finish (waking the sensor if it
   sleeps), open and close it and confirm the panel heard it, change its check-in and confirm the
   change took effect once it wakes, then remove it again. Every step can be skipped with `s` -
   useful when only part of the pass matters, or the same sensor is reused across benches.

**What to keep:** the check's output folder - `report.md`, `steps.json`, `status_initial.json`,
`status_final.json`, `check.zwtrace` and `startup.zwtrace` (the trace format is in
docs/features/30's quick start; its header never carries the home or panel ID). Before a trace
becomes a replay fixture for `test_zw_startup` and friends, check the raw frame bytes for this
panel's home ID (some Serial API frames carry it on the wire) and scrub it, the same as
`rf capture` dumps are cleaned before they join `components/core/rf_decode/test/rf/` for 433 MHz
(docs/features/25, CONTRIBUTING.md "Sensor captures").

## Sources

- [ZAC93 user manual (PDF)](https://www.getzooz.com/downloads/zooz-z-wave-800-series-long-range-gpio-module-zac93-LR-manual.pdf)
- [ZAC93 specs](https://www.support.getzooz.com/kb/article/1380-zac93-800-z-wave-long-range-gpio-module-specs/)
- [ZAC93 FAQs](https://www.support.getzooz.com/kb/article/1426-zac93-gpio-module-faqs/)
- [ZAC93 installation guide](https://www.support.getzooz.com/kb/article/1215-zac93-800-gpio-module-installation-guide/)
- [ZAC93 change log](https://www.support.getzooz.com/kb/article/1389-zac93-gpio-module-change-log/)
- [Home Assistant community: ESP32 + ZAC93 bridge](https://community.home-assistant.io/t/make-a-z-wave-to-wifi-bridge-with-esphome-stream-server-and-zooz-zac93-gpio/565913)
