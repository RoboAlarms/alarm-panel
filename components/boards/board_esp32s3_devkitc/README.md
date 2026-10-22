# board_esp32s3_devkitc

**Tier:** boards (board support) · **Features:**
[42 Headless panel](../../../docs/features/42-Headless-Panel-ESP32-S3/README.md),
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `board_esp32s3_devkitc`, the description)

The ESP32-S3-DevKitC-1 with an N16R8 module (16 MB quad flash, 8 MB octal PSRAM) as an alarm panel
without a screen. It implements the board contract (`board_api.h`): the screen, touch, speaker and
battery calls answer `ESP_ERR_NOT_SUPPORTED`, so the panel runs without a UI; it describes itself in
`board_desc.c`, validated on the PC.

- `board.c`: identification, the BOOT button (`board_boot_button_down()`, setup recovery), the
  add-on I2C bus for a clock module (`board_i2c_ext()`, created on first use).
- `board_desc.c`: the module's flash (IO26-32), octal PSRAM (IO33-37), USB (IO19/20) and UART0
  (IO43/44) pins and the strapping pins kept out; the add-ons' places (`board_pins.h`).

## Board facts
| Part | Detail |
|---|---|
| SoC | ESP32-S3 (QFN56), silicon v0.2 on the unit tried; 16 MB quad flash, 8 MB octal PSRAM |
| USB | "USB" port: the chip's USB-Serial/JTAG, the console; "COM"/"UART" port: a USB-to-UART bridge on UART0 (boot messages) |
| Buttons, LED | BOOT on IO0 (to GND), RESET on EN; one WS2812 on IO48 (v1.0; IO38 on v1.1), left off |
| Wired terminals | IO4-7, IO15-18 (internal pull-ups): zones, or wired outputs (docs/features/16) |
| 433 MHz | a CC1101 on SPI2 (SCK 12, MOSI 11, MISO 13, CS 10, GDO0 14), or a receiver's data on IO14 |
| microSD | an SPI module on the same bus, CS IO21 |
| Clock module | DS3231 on IO8 (SDA) / IO9 (SCL) |
| Z-Wave | a ZAC93 on UART1, header J3: IO1 (the panel's TX), IO2 (RX) |
| Free for a person's parts | IO38-42, IO47 |

## Public API
| Header | What it offers |
|---|---|
| `include/board.h` | the board contract (`board_api.h`), this board's pins, `board_boot_button_down()` |
| `include/board_pins.h` | every pin of the plan above |

## Depends on
- Project components: `board_api`, `board_info`
- ESP-IDF: `esp_driver_gpio`, `esp_driver_i2c`; managed: `lvgl` (the contract names its types)

## Options
None in Kconfig.

## Rules
- The strapping pins (IO0, IO3, IO45, IO46) take nothing a person wires; IO0 is the BOOT button.
- Holding BOOT while the board resets enters the chip's download mode: it is not the recovery
  gesture, which is BOOT held five seconds after a normal start (docs/features/31 headless-setup.md).
