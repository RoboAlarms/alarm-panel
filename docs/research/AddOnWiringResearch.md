# CrowPanel 7": where the add-on modules plug in (DS3231, CC1101, ESP32-H2)

Research date: 2026-09-24. Board: Elecrow CrowPanel Advanced 7" (ESP32-P4), board V1.1;
repository at `c7b70ff` plus the working tree. Questions: *explain the Flasher's clock
note ("A DS3231 module on the I2C connector (PH2.0, 4 pins): four wires, VCC to 3V3. It shares the
touch screen's bus, so keep the leads short.") and look for a better way to connect it, using the
board's documentation*; then *what is the best way to have a CC1101, a DS3231 and the ESP32-H2
module (the one made to plug into the top of the board) using the available headers?*

**Nothing was built or changed by this research:** no firmware, Flasher, board description or
docs/features/20, /25, /35 edit, nothing flashed or measured on a board. It is a plan to choose from.

## Answer

1. **The DS3231 on its own:** the I2C connector (J13) works and is what the firmware supports,
   but it is the touch screen's bus: a fault on the module or its leads can freeze touch. Better:
   J7's **IO27 + IO28** on the P4's second I2C controller. Both are free on V1.1 and are not zone
   terminals. Needs a small firmware change.
2. **All three at once:** the **ESP32-H2 in the wireless-module socket**, the **CC1101 on J7
   IO49-IO52 + IO5**, the **DS3231 on J7 IO27/IO28** (its own bus). Wired zones keep **IO2, IO3,
   IO4**; J13 stays free. The CC1101 has to leave the socket because the H2 uses every socket pad.
3. **Neither layout runs on today's firmware** ("Firmware work" below), and the panel cannot use
   the H2 yet: docs/features/20 has no Zigbee stack.

## The board's connectors (V1.1)

| Connector | Type | Pins as printed | P4 GPIO | Notes |
|---|---|---|---|---|
| I2C (J13, printed "I2C1") | PH2.0-4P | SCL, SDA, 3V3, GND | 46, 45 | the touch screen's bus, behind BSS138 level shifters; 3.3 V out, 1 A max (wiki). The board calls it I2C1; the firmware drives it with I2C controller 0 |
| UART1 (J2) | PH2.0-4P | RX1, TX1, 3V3, GND | 48, 47 | also wired to the ESP32-C6 (Elecrow's guide: nets `RXD1`/`TXD1`, "ESP32-C6/UART2"); 3.3 V out, 1 A max |
| UART3 IN | XH2.54-4P | RX3, TX3, +5V, GND | 33, 34 | a 5 V power input; its data lines are on the 5 V side of BSS138 shifters |
| J7 | 2x12, 2.54 mm | below | 2-5, 25, 27, 28, 49-52 | straight to the P4: 3.3 V logic, not 5 V tolerant |
| Wireless-module socket (J9 + J11) | 2x7 | below | 6/7/8, 9, 10, 45/46, 53/54 | Elecrow's radio modules (H2, C6, nRF24, SX1262) |
| Camera (CIS-CAM) | FPC | - | I2C on 12/13 (BSS138), CSI reset 11 | camera not used; fine-pitch ribbon |
| microSD | slot | - | CLK 43, CMD 44, D0 39 | docs/features/29 |
| SPK-L, SPK-R, BAT | PH2.0-2P | - | - | speakers, Li-ion cell |

Everything else on the P4 is taken or unsafe: display (DSI, dedicated), backlight 31
(`LCD_BK_POWER` 29 is not fitted: don't drive it), touch reset 40 and INT 42, audio 21/22/23 and
amplifier enable 30, C6 SDIO 14-19 and enable 32, UART0 37/38, PDM microphone 24/26, LCD reset 41
(unused), strapping pins 34-38.

### J7

The silkscreen table in the wiki's board photo (a V1.0 board; `board_pins.h` has the same pins
for V1.1, from the V1.1 schematic):

```
col:     1    2    3    4    5    6     7     8     9     10    11   12
row A:  5V   5V   5V   GND  GND  IO52  IO51  IO50  IO49  IO28  GND  GND
row B:  3V3  3V3  3V3  GND  IO2  IO3   IO4   IO5   IO25  IO27  GND  GND
```

The photo doesn't show which pad row is row A: **check 5V and 3V3 with a meter before wiring.**
IO3's place is proven in use (a development panel's receiver module is on J7 IO3, docs/features/25).
Row A's 5V pins are VDD5V in the V1.1 schematic: the board's 5 V after the POWER switch, an
output. A supply for the panel belongs on UART3 IN, not here (docs/features/17 "Power inputs",
2026-09-24).

| J7 GPIO | Today |
|---|---|
| IO2, IO3, IO4, IO5, IO49-IO52 | wired zone terminals (`BOARD_WIRED_TERMINALS`); a receiver module's data line may move onto one with `rf pin` |
| IO25 | the P4's USB Serial/JTAG D+, listed as in use in `board_desc.c` and left alone |
| IO27, IO28 | free on V1.1. Not offered as terminals only because V1.2 boards move the socket's UART to 27/28 (J7 then carries 53/54 in these two places). IO27 is also the D+ of the P4's second USB PHY (unused here; its D- partner, GPIO26, is the microphone); ESP-IDF notes the D+ pads come up with a pull-up |

No P4 strapping pin (34-38) is on J7.

### Wireless-module socket and the Elecrow ESP32-H2 module

The socket's printed labels, the board nets (wiki schematic excerpt `P4-7-sch-3`) and the H2
module's pins (its datasheet, section 4.4):

| Socket pad | Board net | P4 GPIO | H2 module pin | Today's firmware uses it for |
|---|---|---|---|---|
| IO53 | P4_TXD2 | 53 | U1RXD | the radio's UART (docs/features/20) |
| IO8 | SPI2_SCK | 8 | GPIO2 (MTMS) | CC1101 SCK (default place) |
| IO7 | SPI2_MISO | 7 | GPIO1 | CC1101 MISO |
| IO6 | SPI2_MOSI | 6 | GPIO0 | CC1101 MOSI |
| 3V3 | VDD_3V3 | - | 3V3 | power |
| GND | GND | - | GND | |
| NC | VDD5V_W | - | 5V (VBUS) | not powered: L4 and C88 are marked NC in the schematic |
| IO54 | P4_RXD2 | 54 | U1TXD | the radio's UART |
| IO46 | I2C1_SCL_3V3 | 46 | GPIO14 (XTAL_32K_N) | the touch bus |
| IO45 | I2C1_SDA_3V3 | 45 | GPIO12 | the touch bus |
| NC | - | - | GPIO11 | |
| IO9 | IO9_W | 9 | GPIO5 (MTDI) | CC1101 CSn |
| IO10 | IO10_W_BUSY | 10 | GPIO4 (MTCK) | CC1101 GDO0, or a receiver module's DATA (default place) |
| NC | - | - | GPIO3 (MTDO) | |

On V1.2 boards IO53/IO54 become GPIO27/28 (the wiki's "Version 1.2 Update").

The H2 module (datasheet V1.1): ESP32-H2FH4 with 4 MB flash; Bluetooth LE 5.3 and 802.15.4
(Zigbee 3.0, Thread 1.3); 3.0-3.6 V on the module interface; an IPEX-1 antenna on a 120 mm lead;
**its own USB-C, wired to the H2's USB Serial/JTAG (GPIO26 D-, GPIO27 D+)**, so a first firmware
can be loaded over USB before the module goes in the socket (docs/features/20 asks how the first image
gets there). BOOT and RESET buttons: the datasheet's table pairs BOOT with CHIP_EN and RESET with
GPIO9, which looks swapped (GPIO9 is the H2's boot strap). A blue USER LED is on GPIO25. The
datasheet gives 8.8 mA "operating" current without saying in which mode.

## The I2C connector (J13) and the Flasher's clock note

**Pin order and cable.** SCL, SDA, 3V3, GND as printed next to J13. PH2.0 is 2.0 mm pitch,
JST-PH style: a pre-crimped "PH2.0 4-pin" lead with Dupont ends plugs onto a ZS-042's header.
Match the wires to the printed labels, not their colours.

**Who else is on the bus.** The GT911 touch controller (0x5D or 0x14, polled about 50 times a
second at 400 kHz), the STC8H1K08 battery helper (0x2F), and the wireless socket's IO45/IO46 pads
(an H2's GPIO12/GPIO14). The DS3231 (0x68) and the ZS-042's AT24C32 EEPROM (0x57) don't clash.

**Level shifters.** Elecrow's guide: GPIO45/46 are the nets `I2C1_SDA`/`I2C1_SCL` and go
"through BSS138 to the 3.3 V side" (`I2C1_*_3V3`), which the connector and the socket use. The
touch controller's FPC connector (FPC2, wiki excerpt `P4-7-sch-2`) is on the unshifted nets, with
the P4. The firmware also turns on the P4's internal pull-ups (`board_touch.c`). A BSS138
shifter passes a low in both directions.

**"VCC to 3V3".** The module's pull-ups (4.7 kOhm on a ZS-042) go to its own VCC, so VCC sets
the bus's high level, and the connector gives 3.3 V. At 5 V the pull-ups would lift the
connector side of the shifters and the socket's I2C pads above 3.3 V. The shifters should keep
it off the P4 and the GT911, so the reason given in `boards.json` ("would put on the touch
screen's bus") overstates it slightly. It is still wrong: never 5 V.

**"Keep the leads short".** Whatever happens on the leads reaches the touch bus through the
shifters: their capacitance slows the edges of the 400 kHz touch traffic, noise they pick up
lands on it, and a short or a failed module freezes touch (and the battery readings). Guidance:
the module inside the case next to the connector, 10-20 cm of lead, away from siren and mains
wiring. A metre is asking for touch errors.

**A DS3231 can hold SDA low.** It keeps its I2C interface running on the coin cell, so a panel
restart in the middle of a read (watchdog, power dip, the Flasher's reset) can leave it driving
SDA low mid-byte; its datasheet (I2C interface section) says to clock SCL until SDA is released,
then send a START. ESP-IDF v6.1 does not clear the bus when the bus is created
(`i2c_new_master_bus()`), but it resets the controller and clocks the bus free before a
transaction when the previous one timed out or the bus reads busy (`s_i2c_transaction_start()`
in `components/esp_driver_i2c/i2c_master.c`; `i2c_master_bus_reset()` does it on demand). The
clock module is read at start-up before touch starts (`panel_app.c`), so it should recover, but
nobody has tried it: it is not in docs/features/35's bench list.

**Other module notes** (docs/features/35): SQW and 32K stay unconnected; a ZS-042 charges its cell
through a diode and 200 Ohm, so with a CR2032 remove one of them (or fit an LIR2032).

## Places rejected, and why

| Place | Why not |
|---|---|
| UART1 connector (J2) | Looks ideal for a second bus (the same PH2.0-4P, 3V3, its own GPIO47/48), but both lines also go to the ESP32-C6's UART, which can drive them |
| UART3 IN | a 5 V power input; its data lines are on the 5 V side of BSS138 shifters |
| Camera connector | a separate I2C bus (GPIO12/13), but on a fine-pitch ribbon connector |
| Socket pads IO45/IO46 | the same touch bus as J13, and the socket belongs to the H2 |
| IO25 | the P4's USB D+; the project keeps it out (but see "Ideas not pursued") |
| Zone terminals for the DS3231 | work, but cost two wired zones while IO27/IO28 are free |

## Recommended layout: ESP32-H2 + CC1101 + DS3231

Pin budget: J7 has 11 GPIOs; with IO25 left alone, 10 are usable. The CC1101 needs 5 (SCK, MOSI,
MISO, CSn, GDO0), the DS3231 2 on its own bus (none on J13); what is left is wired zones.

| Part | Where | Wires |
|---|---|---|
| ESP32-H2 module | wireless-module socket | plugs in; its UART is GPIO53/54 (V1.1). Nothing else to wire |
| CC1101 | J7 | SCK to IO52, MOSI to IO51, MISO to IO50, CSn to IO49, GDO0 to IO5, VCC to 3V3, GND to GND; GDO2 unconnected; a 433 MHz antenna or 17.3 cm of wire |
| DS3231 (ZS-042) | J7 | SDA to IO27, SCL to IO28 (its own bus, I2C controller 1), VCC to 3V3, GND to GND; SQW and 32K unconnected |
| Wired zones | J7 | IO2, IO3, IO4 (a contact to GND) |
| Left free | | IO25; J13 (for another I2C add-on); the three 5V pins |

The CC1101 takes four neighbouring row A pins, the DS3231 the column next to the GND pins, and
the zones keep the left end of row B.

- **Leads:** Dupont, 10-15 cm; the CC1101's SPI runs at 4 MHz (`cc1101.c`). 3V3 only: nothing
  here takes 5 V.
- **Antennas:** the CC1101's away from the board, the display ribbon and the H2's; the H2 uses
  the IPEX antenna on its lead. 433 MHz and 2.4 GHz don't interfere much, the P4 and the display
  are the noise to keep away from.
- **Power:** tens of mA in all, far under the 3.3 V outputs' 1 A.

### Alternatives

- **Four wired zones:** the DS3231 on J13 (sharing the touch bus, as today) and the CC1101 on
  IO28 + IO49-IO52 (all of row A). Zones IO2-IO5; IO27 spare (it could become a fifth terminal
  on V1.1).
- **Seven wired zones:** a fixed receiver module (RX470-4, as on a development panel) instead of the
  CC1101: one J7 pin, moved there at runtime with `rf pin`, which works today. What the CC1101
  adds (docs/features/25 RF-001): tuning over 300-348 / 387-464 MHz (315 and 345 MHz sensors) and RSSI
  for jamming detection.

### Ideas not pursued (each needs a bench check first)

- **IO25 as the CC1101's CSn.** Its USB D+ pull-up at reset would keep the CC1101 deselected
  through a restart, and IO5 would go back to the zones (four zones with the DS3231 on its own
  bus). `board_desc.c` would have to stop listing IO25 as in use, and nothing may rely on the P4's
  USB Serial/JTAG.
- **The CC1101 on four pins.** Its SO pin (GDO1) can output the received data while CSn is high,
  so MISO and GDO0 could share one GPIO (SPI MISO and the RMT capture on the same pad). Firmware
  work, and the capture would see the SPI traffic.
- **The CC1101 on the socket's test pads beside the H2.** The schematic excerpt shows single pads
  on the socket's SPI, IO9 and IO10 nets (P14-P16, P22, P23; probably test points). Wires
  soldered there would share those nets with the H2's GPIO0/1/2/4/5 without using J7, but only
  while the H2's firmware leaves those pins as inputs. Not a header; the pads' places are unknown.

## Firmware work each layout needs

**DS3231 on IO27/IO28:**
- `components/boards/board_crowpanel_p4/src/board_desc.c`: `.i2c` to SDA 27, SCL 28, with a "J7
  header" `where`.
- A separate I2C master bus on controller 1 for the clock module instead of handing out the
  touch bus (`board_i2c_ext()` in `board_touch.c`).
- The Flasher: `tools/flasher/roboflash/boards.json` `rtc` (pins, where, wires), the note in
  `roboflash/web/app.js`, `tools/tests/test_flasher_safety.py`.
- docs/features/35 ("Where it plugs in", RTC-009's wording about the touch bus, bench step 8) and
  `components/drivers/rtc_ds3231/README.md`.

**CC1101 on J7:**
- Its SPI pins are set when the firmware is built (`CONFIG_ALARM_RF_SPI_*`), and
  `board_info_validate()` reports an SPI pin on a wired terminal as in use, so
  `start_zone_sources()` leaves the receiver off ("no safe wiring on this board"). Needed: a J7
  placement that takes all five pins out of the terminal list, the way `board_rf_data_on()`
  does for a moved data line, ideally stored and chosen at runtime like `rf pin`, so Flasher
  images can use it.
- The Flasher's `boards.json` `rf.cc1101` wiring and its tests.

**The H2 in the socket:**
- The 433 MHz service must stay off the socket's pads when the H2 is fitted: in auto mode it
  probes the socket's SPI every 30 s until a CC1101 answers, and those pads are the H2's
  GPIO0/1/2/4/5.
- docs/features/20's own work: the Zigbee stack and transport, the H2 image and its install path (the
  module's USB-C makes a one-time USB install possible), and H2 firmware that leaves GPIO12/14
  alone (the touch bus).

**V1.2 boards:** J7's IO27/IO28 places carry GPIO53/54 there and the socket's UART moves to
27/28: a board variant, not a pin tweak.

## To check on a bench

- J7's pad rows (which is 5V, which is 3V3) and the IO27/IO28 places on a V1.1 board.
- A DS3231 on J13: restart the panel during `rtc check` several times; touch must keep working.
- A CC1101 on 10-15 cm leads at 4 MHz: `rf` shows the register read-back passing (RF-017).
- That the socket's 5V pin really is unpowered (L4 not fitted).
- Whether the C6 drives IO47/IO48 (only if the UART1 connector is ever wanted).
- That GPIO27's pull-up at reset doesn't matter to I2C once the pin is configured.

## Sources

- `docs/hardware/CrowPanel7inScreen/WikiPage.html`, the saved Elecrow wiki page: the connector
  table and pins, schematic excerpts `WikiPage_files/P4-7-sch-2.webp` (touch FPC) and
  `P4-7-sch-3.webp` (wireless socket), the board photo `advance P4-7.webp` (V1.0 silkscreen: J2,
  J13, J7, the socket), "Version 1.2 Update".
- Elecrow's hardware driver guide, `Eagle_SCH&PCB/1.2/readme.md` in
  <https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen>
  (fetched 2026-09-24): the GPIO table, BSS138 shifting on I2C and UART3, GPIO47/48 to the C6,
  the camera's I2C on 12/13, discrepancies D-01 to D-09. Quoted through a web-fetch tool: re-read
  the original before relying on a fine detail.
- `docs/hardware/Wireless_Module-ESP32-H2_Datasheet.pdf` (Elecrow, V1.1): pinout, USB, buttons,
  power.
- ESP-IDF v6.1 (`C:\esp\v6.1\esp-idf`): `components/esp_driver_i2c/i2c_master.c` (bus clear),
  `components/soc/esp32p4/include/soc/soc_caps.h` (two HP I2C controllers and one LP),
  `components/soc/esp32p4/register/hw_ver*/soc/io_mux_reg.h` (USB PHY pins 24/25 and 26/27),
  `components/esp_hal_gpio/esp32p4/include/hal/gpio_ll.h` (the D+ pull-up at reset).
- This repository: `components/boards/board_crowpanel_p4/` (`include/board_pins.h`,
  `board_desc.c`, `board_touch.c`, `board_battery.c`), `components/core/board_info/src/board_info.c`,
  `components/app/panel_app/src/panel_app.c`, `components/services/rf_service/` (Kconfig, `rf pin`),
  `components/drivers/rf_rx/src/cc1101.c`, `tools/flasher/roboflash/boards.json` and
  `web/app.js`; docs/features/06, /20, /25, /35.
- The DS3231 datasheet (Analog Devices), I2C interface section: resynchronising after a host
  reset.
