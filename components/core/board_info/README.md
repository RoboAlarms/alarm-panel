# board_info

**Tier:** core (portable C11) · **Features:**
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 4, ADR-035) · **Tests:** [`test/`](test/) (1 suite: `board_info`); each board's own
`test/` validates its description

A board described in data: the chip, whether the module has PSRAM, the screen, the kind of
touch and audio, every pin the board's own parts use, and the places services may attach
things (wired zone terminals, the SD card, a 433 MHz receiver, a sensor radio's UART, a Z-Wave
radio's UART and reset line), and the pins a person can reach for add-ons of their own
(`reach`, BRD-018). The
app hands the description to the services, so no service needs to name a board.
`board_info_validate()` refuses a description that could harm the board: a pin the chip
doesn't have, flash or PSRAM pins, an output on an input-only pin, a contact without a
pull-up, an outside signal on a strapping pin, one pin used twice (only lines of the same bus
carrying the same signal may share one, BRD-020). Pins chosen in Kconfig are checked the same way: the app copies the description,
puts them in and validates the copy before configuring anything.

No heap, no globals, no ESP-IDF: the chip facts are constant tables.

## Public API
| Header | What it offers |
|---|---|
| `include/board_info/board_info.h` | `board_info_t` and its parts (`board_pin_t`, `board_terminal_t`, `board_sd_t`, `board_rf_t`, `board_uart_t`, `board_i2c_t`, `board_zwave_t`, and the other places `board_rf_place_t` / `board_i2c_place_t` / `board_zwave_place_t`), `board_info_validate`, `board_issue_text`, `board_rf_data_on` / `board_rf_place_on` / `board_i2c_place_on` / `board_zwave_place_on` / `board_zwave_reset_on` and `board_zwave_place` (the board as wired with a runtime choice, BRD-013..017), the custom places `board_rf_custom_on` / `board_i2c_custom_on` / `board_zwave_custom_on` and `board_reach` (BRD-018, BRD-021), the console's argument parser `board_custom_parse` (BRD-022), the chip facts (`board_chip_has_pin`, `_pin_can_output`, `_pin_has_pullup`, `_pin_is_strapping`, `_pin_is_reserved`, `board_chip_name`) |

## Depends on
- Project components: none
- ESP-IDF: none (portable)

## Options
None.

## Rules
- `audio_rate_hz` is what the board's audio output plays (0 = the sounder's 32 kHz); the
  validator accepts only rates the sounder can play (8-32 kHz in whole hundreds of Hz).
- The chip facts follow ESP-IDF v6.1's `SOC_GPIO_VALID_GPIO_MASK` and
  `SOC_GPIO_VALID_OUTPUT_GPIO_MASK`; strapping, flash and PSRAM pins follow the datasheets.
  Adding a chip means adding its row to `CHIPS` in `src/board_info.c` and its facts to the
  tests.
- SPI host numbers are the peripheral's own (2 = SPI2, 3 = SPI3); the app converts them to
  ESP-IDF's `spi_host_device_t`.
- A board's description lives in the board component as a portable file (`board_desc.c`) so
  its test can validate it on the PC (BRD-008).
- `BOARD_NO_PIN` (-1) means "not wired"; the validator skips it.
- `pins` lists every pin wired to a part of the board, used by the firmware or not (BRD-019): a
  pin missing there could be offered to an add-on. A `BOARD_PIN_SHARED` pin names its bus
  (`BOARD_BUS_SPI(host)`, `BOARD_BUS_I2C(port)`) and signal (`BOARD_SIG_*`); other pins leave both
  0. The add-on I2C connector's bus is `i2c.port` (0 for the board's own connector).
- Custom places (BRD-021) accept only `reach` pins, each signal on its own pin, none of the
  board's own or another placed part's; a wired terminal they take leaves the offered list. On a
  refusal `why` names the GPIO and signal and the caller turns the part off, never back to its
  default pins. Issue codes are appended, never renumbered.

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite with `ctest --test-dir build/host -R board_info`.
- Chip facts for the ESP32, ESP32-S3 and ESP32-P4 (BRD-001, BRD-002).
- Every refusal: reserved pins including the 7" panel's RF wiring copied onto a classic ESP32
  (BRD-002), input-only outputs (BRD-003), missing pull-ups (BRD-004), strapping pins
  (BRD-005), pins used twice and shared buses (BRD-006), incomplete descriptions (BRD-007).
- A Z-Wave radio (docs/features/36 hardware.md §4): wired at a place the board offers (its pins
  leave the terminals; the sensor radio's socket freed when it takes that UART; a pin the board
  uses, a strapping pin or the clock module's place refused; an unreadable place refused,
  BRD-016) and its reset line on a wired terminal only, with the radio wired, never an input-only
  one (BRD-017).
- Reachable pins described (BRD-018); bus lines shared only on one bus for one signal (BRD-020).
- Custom places for the receiver, the I2C modules and the Z-Wave radio: accepted, and refused for
  an unreachable pin, a pin used twice, a pin another part holds, half a CC1101, a strapping pin
  (the validator's last word), with `*out` untouched (BRD-021).
- The `key=value` parser: order, optional keys, 0-63, and every refusal's words (BRD-022).
