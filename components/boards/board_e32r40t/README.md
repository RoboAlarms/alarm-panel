# board_e32r40t

**Tier:** boards (board support) · **Features:**
[19 4" ESP32 panel](../../../docs/features/19-CYD-ESP32-Port/README.md),
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 10) · **Tests:** [`test/`](test/) (1 suite: `board_e32r40t`, the description); the
drivers on the board itself (docs/features/19 Phase 2 and 4)

The LCDWiki "4.0inch ESP32-32E Display" (SKU E32R40T, with resistive touch), a classic ESP32
without PSRAM. It implements the board contract (`board_api.h`) and describes itself in
`board_desc.c`, validated on the PC. At power-up the backlight (a 25 kHz PWM at 0 %), the
amplifier and the RGB LED are off, so the display's uninitialised memory is never lit and the
speaker never hums.

- `board_display.c`: the ST7796S under LVGL (esp_lcd, esp_lvgl_port; SPI2 at 40 MHz, two
  5-line DMA buffers, a 6 KB LVGL task). Colour order and orientation are one register
  (MADCTL), written directly so they can change live.
- `board_touch.c`: the XPT2046 (`touch_xpt2046`), filtered and calibrated (`touch_calib`), as a
  polled LVGL input. The three-target calibration is drawn on LVGL's top layer and stored in
  NVS (`touch`/`cal`); it runs when nothing is stored, on `touch cal`, or when a finger is held
  for 3 s within the first 10 s after start.
- `board_console.c`: bring-up commands - `lcd` (show) and `lcd invert|bgr|swap|mx|my on|off`
  (change the panel now; bake what works into `board_pins.h`), `touch` (raw reading, pressure,
  counters) and `touch cal`.
- Audio (the DAC) and the battery ADC answer `ESP_ERR_NOT_SUPPORTED` until docs/features/19
  Phase 2 brings them.

## Board facts
| Part | Detail |
|---|---|
| SoC | ESP32-D0WD-V3 (silicon v3.1 on the unit tried), 520 KB SRAM, no PSRAM, 4 MB flash |
| Display | ST7796S 320x480 on SPI2: SCK 14, MOSI 13, MISO 12, CS 15, DC 2; reset on EN; backlight 27 |
| Touch | XPT2046 on the same bus: CS 33, PENIRQ 36 |
| Audio | DAC IO26 into an FM8002E, enable IO4 (low = on); the description says 24 kHz |
| microSD | SPI3: SCK 18, MOSI 23, MISO 19, CS 5 (shared with the SPI header, CS 21) |
| RGB LED | red 22, green 16, blue 17 (low = on) |
| Battery | ADC1 on IO34 |
| Offered | wired terminals IO39 (fit a pull-up), IO25, IO32; a CC1101 on the SPI header with its data on IO35 |

## Public API
| Header | What it offers |
|---|---|
| `include/board.h` | the board contract (`board_api.h`), this board's pins, `board_backlight_requested()`, `board_e32r40t_console()` (the firmware's console hook) |
| `include/board_pins.h` | every pin, from the LCDWiki pin table, and the panel's and touch's defaults (`BOARD_LCD_*`, `BOARD_TOUCH_*`) |

## Depends on
- Project components: `board_api`, `board_info`, `touch_calib`, `touch_xpt2046`
- ESP-IDF: `esp_lcd`, `esp_driver_gpio`, `esp_driver_ledc`, `esp_driver_spi`, `esp_timer`,
  `nvs_flash`, `console`; managed: `esp_lcd_st7796`, `esp_lvgl_port`, `lvgl`

## Options
None in Kconfig: the panel's colour order, inversion and orientation, the SPI clock, the
buffer height and the touch's raw range are `#define`s in `include/board_pins.h`, tried live
first with the console's `lcd` command.

## Rules
- The backlight stays off until a first full frame is in the panel's memory: the ST7796S keeps
  its own frame memory, which holds noise at power-up (CONTRIBUTING.md, "Display safety").
- IO12 (the display's MISO) is the flash-voltage strapping pin: the board boots correctly as
  shipped. Never burn the flash-voltage eFuse to "fix" it without the owner.
- Resistive touch needs calibration and filtering (docs/features/19, "Touch and screen design").
