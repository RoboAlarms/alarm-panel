# board_crowpanel_10

**Tier:** boards (board support) · **Features:** [06 CrowPanel P4 platform](../../../docs/features/06-CrowPanel-P4-Platform/README.md), [33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md) ·
**Tests:** `test/` on the PC; on hardware (see docs/features/06)

Board support for the Elecrow CrowPanel Advanced 10.1", board rev V1.2: the same ESP32-P4 main
board as the 7". The board contract comes from the CrowPanel Advanced P4 family library,
[`crowpanel_p4_family`](../crowpanel_p4_family), with its ES8311 option selected by this
component's `Kconfig`; this component is what is the 10.1"'s own: the board described in data
(`src/board_desc.c`, `board_info()` and `board_name()` in `src/board_id.c`), its wiring
(`include/board_pins.h`) and its names in MQTT and Home Assistant (`include/board.h`). Its
firmware is `firmware/crowpanel_10`, built on the 7"'s (`ALARM_BOARD_BASE crowpanel_p4`).

The wiring was read net by net from Elecrow's V1.2 schematic for the 10.1" (docs/hardware/).
For the sound the schematic and the boards disagree: it draws an ES8311 codec into an NS4263B
and marks the 7"'s NS4168 amplifiers as not fitted, but Elecrow's V1.2 examples drive the
sound as on the 7" (plain I2S, amplifier enable on GPIO30, a PDM microphone clocked on GPIO24)
and the board this was tried on answers no codec at 0x18. So the family looks for the codec
at start and plays plain I2S where none answers; which amplifier the board carries is not
verified.

## Board facts
| Part | Detail (`include/board_pins.h`; the shared part in the family's `crowpanel_p4_pins.h`) |
|---|---|
| Display | 10.1" IPS 1024x600, the 7"'s timing; the UI uses the 7"'s 1024x600 layout |
| Audio | plain I2S into the amplifier, enable GPIO30 (low: on), as on the 7". Where an ES8311 answers at I2C0 0x18: the codec (MCLK GPIO24) into an NS4263B |
| Microphones | a PDM microphone on GPIO24 (CLK) and 26 (DATA) in Elecrow's examples; the schematic's ES7210 too: no microphone is used |
| I2C connector | J13, HY2.0, 4 pins (SCL, SDA, 3V3, GND): I2C0 behind the level shifters, shared with the GT911, the STC8 and a codec where fitted |
| Wireless socket | UART 27 (to the module) / 28 (from it), SPI 6/7/8, IO 9/10 (V1.2: swapped with J7's 53/54) |
| J7 header | GPIO 2, 3, 4, 5, 25 (USB D+), 49-54 |
| Wired terminals | J7: GPIO 2, 3, 4, 5, 49, 50, 51, 52, as on the 7" |
| Add-on places (`board_desc.c`) | CC1101 `j7`: 52/51/50/49 + IO5; clock `j7`: IO53 SDA, IO54 SCL on I2C1; Z-Wave `socket` (27/28) or `j7` (IO53 TX, IO54 RX); the same header positions as the 7"'s 27/28 |
| GPIO29 | 10K pull-up to 5 V, gate of a backlight power switch the board doesn't fit: listed as the board's own, never driven or offered |
| MQTT / HA model | `CrowPanel Advanced 10.1" (ESP32-P4)` / `CrowPanel Advanced 10.1` |

## Public API
| Header | What it offers |
|---|---|
| `include/board.h` | the family's `crowpanel_p4.h` (the board contract and its extras), this board's pins, `BOARD_MQTT_MODEL` and `BOARD_HA_MODEL` |
| `include/board_pins.h` | the 10.1"'s own GPIOs (socket, J7, places, GPIO29) after the family's `crowpanel_p4_pins.h` |

## Depends on
- Project components: `board_api`, `board_info`, `crowpanel_p4_family`

## Options
`Kconfig` defines the hidden `CONFIG_BOARD_CROWPANEL_10`, which selects the family's
`CONFIG_BOARD_CROWPANEL_P4_ES8311` (look for the codec at start).

## Tests
`test/test_board_desc.c` checks the description on the PC with the 7"'s checks on this board's
pins (BRD-008 and the place and terminal requirements it names): `pwsh tools/host_test.ps1`. On
the panel: the family's README, "Tests"; and the sound once by ear (the boot log says
`no ES8311 at 0x18: plain I2S`, or `ES8311 codec ready` on a board with the codec).
