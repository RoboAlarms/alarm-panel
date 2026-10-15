# crowpanel_p4_family

**Tier:** boards (a family library) · **Features:** [06 CrowPanel P4 platform](../../../docs/features/06-CrowPanel-P4-Platform/README.md), [17 Power and supervision](../../../docs/features/17-Power-and-Supervision/README.md), [33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md) ·
**Tests:** on hardware (see docs/features/06); each board's description on the PC

What every board on Elecrow's ESP32-P4 main board shares: the power rails, the backlight, the
MIPI-DSI display into LVGL with its refresh monitor and flicker guard, polled GT911 touch, I2S
audio out (straight to the amplifiers, or through an ES8311 codec where one answers), the STC
battery helper,
and `crowpanel_p4_pins.h`, the wiring those boards have in common. It implements the board
contract, `components/boards/board_api/include/board_api.h` (docs/features/33 step 4), except
`board_info()` and `board_name()`: those, the wiring that differs and the board's names are each
board's own component, which requires this library.

| Board | Component | Firmware |
|---|---|---|
| CrowPanel Advanced 7", rev V1.1 | [`board_crowpanel_p4`](../board_crowpanel_p4) | `firmware/crowpanel_p4` |
| CrowPanel Advanced 10.1", rev V1.2 | [`board_crowpanel_10`](../board_crowpanel_10) | `firmware/crowpanel_10`, built on `crowpanel_p4` |

A firmware sees this library only when it names it (`ALARM_BOARD_FAMILY crowpanel_p4_family` in
its `CMakeLists.txt`, `cmake/alarm_idf.cmake`); `tools/check_boards.py` checks that the firmware
and its board component agree. It holds no alarm logic. It does not start the Wi-Fi
co-processor (ESP-Hosted is set up by the firmware's `sdkconfig.defaults`), mount the SD card or
configure wired terminals: the firmware and services do that with pins from the board's
`board_pins.h`.

## Shared facts (`include/crowpanel_p4_pins.h`)
| Part | Detail |
|---|---|
| SoC | ESP32-P4NRW32, 32 MB PSRAM, 16 MB flash; silicon v1.x and v3 need different builds |
| Display | IPS 1024x600, EK79007, MIPI-DSI 2 lanes at 900 Mbps, DPI 51 MHz, RGB565 (a frame every ~17.9 ms); the same timing on the 7" and the 10.1" |
| Backlight | LEDC PWM at 30 kHz on GPIO31 to the MT9201; LDO3 2.5 V (MIPI PHY) and LDO4 3.3 V |
| Touch | GT911 on I2C0 (SDA 45, SCL 46, 400 kHz), RST 40, INT 42 (address selection only) |
| Audio | I2S1: LRCLK 21, BCLK 22, DOUT 23; amplifier enable GPIO30, active low. 7": two NS4168; the 10.1" plays the same way. The 10.1"'s schematic also draws an ES8311 at I2C 0x18 (MCLK GPIO24, 256 x fs) into an NS4263B, set up where it answers |
| Battery | STC8H1K08 helper at I2C address 0x2f on the touch bus |
| Other | microSD (SDMMC slot 0, 1-bit: CLK 43, CMD 44, D0 39); C6 enable GPIO32; UART0 37/38 |

## Public API
| Header | What it offers |
|---|---|
| `include/crowpanel_p4.h` | the board contract (`board_api.h`) plus the family's extras: `board_i2c_bus()`, `board_dev_frame()` (a copy of the frame buffer, with `CONFIG_BOARD_FRAME_CAPTURE`), the display self-check (`CONFIG_BOARD_DISPLAY_DIAG`). A board's `board.h` includes it |
| `include/crowpanel_p4_pins.h` | every GPIO, bus and LDO the family's boards share; a board's `board_pins.h` includes it |
| `include/board_battery.h` | the STC helper as a `power_monitor` source: `board_battery_poll()` (health task only, bounded I2C), `board_battery_read()` (cached sample, unavailable after 10 s), `board_battery_diagnostic()` (the raw 11-byte bank) |

## Depends on
- Project components: `board_api`, `board_info`, `power_monitor` (`power_sample_t` and the STC decoder)
- ESP-IDF: `esp_lcd`, `esp_driver_i2c`, `lvgl__lvgl` (public); `esp_driver_gpio`,
  `esp_driver_ledc`, `esp_driver_i2s`, `esp_driver_tsens`, `esp_hw_support`, `esp_mm`,
  `esp_timer` (private)
- Managed (`idf_component.yml`): `esp_lcd_ek79007` ~2.0.2, `esp_lcd_touch_gt911` ~1.2.1,
  `esp_lvgl_port` ~2.9.0, `lvgl` ~9.5.0, `esp_codec_dev` ~1.6.2 (the ES8311, Espressif's driver)

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `CONFIG_BOARD_FLICKER_GUARD` | y | backlight off at the first late or stalled frame (P4-012) |
| `CONFIG_BOARD_FLICKER_GUARD_HOLD_S` | 10 (2-600) | seconds of steady refresh before the backlight returns |
| `CONFIG_BOARD_FRAME_CAPTURE` | y | `board_dev_frame()` for the remote keypad and screenshots |
| `CONFIG_BOARD_BACKLIGHT_FORCE_OFF` | n | the backlight never turns on (the `-Dark` / `--dark` build) |
| `CONFIG_BOARD_DISPLAY_DIAG` | n | the display self-check of investigation builds (B023) |
| `CONFIG_BOARD_CROWPANEL_P4_ES8311` | hidden | look for an ES8311 at start and set it up where it answers: selected by `board_crowpanel_10`'s Kconfig, never by hand |

It also reads `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` (the DSI PHY clock for v1.x silicon) and
`CONFIG_LV_USE_CUSTOM_MALLOC` (links `board_lvgl_memory.c`, LVGL's PSRAM allocator).

## Rules
- Display safety (AGENTS.md): nothing may flash or strobe. Keep `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y`
  in the firmware's `sdkconfig.defaults` and the flicker guard on; the DPI callbacks here stay in
  IRAM with no flash code or data. The backlight level and the guard's hold share one mutex (B003).
- Touch is polled by LVGL's input timer: never go back to esp_lvgl_port's interrupt mode, which
  lost finger releases. Three unanswered polls count as a lift (B004).
- One LVGL render thread and whole-screen PSRAM draw buffers were measured fastest; keep them.
  LVGL calls from any other task take `board_ui_lock()`.
- Internal RAM is the reserve (docs/bugs/B001): LVGL allocates from PSRAM only, with no fallback.
- The amplifiers stay off until silent I2S frames are flowing, so there is no pop (P4-006).
  With the ES8311 option the I2S clocks (MCLK included) run before the codec is looked for; a
  codec that answers is set up with its DAC at 0 dB, and the sounder sets the volume in the
  samples, as on the 7". Where none answers at 0x18 the sound is plain I2S and GPIO24 is let go
  (logged). No board with the codec has been tried yet.
- What differs between the boards goes in their own components, never behind an `#if` on the
  board here; an `#if` on a part (`CONFIG_BOARD_CROWPANEL_P4_ES8311`) is how a difference in
  the parts is chosen.
- Boards tier (docs/features/33): no dependency on services or the UI, and nothing of a board of
  its own (`tools/check_architecture.py`).

## Tests
Each board's description is checked on the PC by its own component's `test/`. Everything else is
checked on the panel with the service console
(`python tools/panel_console.py -p <PORT> display`; the port is in `PANEL.local.md`):
- `display`: frames per second (55.9 expected), late frames, redraws, refresh times, touch
  polls, free RAM and flicker guard trips; `display stress 20` does the same during flash
  writes (P4-012 acceptance: no late frames).
- `display watch <s>`: a line a second while someone uses the screen.
- `power` (the STC bank and decode status) and `health` (LVGL task and GT911 poll heartbeats).
The display tools and the dark build are optional, for a suspected refresh problem or when
asked; a normal flash is one normal image. The portable parts it serves are host-tested
elsewhere: battery decoding in `power_monitor`, the audio it plays in `sounder`, the screens in
`pwsh tools/ui_preview.ps1` and `pwsh tools/ui_sim.ps1`.
