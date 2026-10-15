# board_crowpanel_p4

**Tier:** boards (board support) · **Features:** [06 CrowPanel P4 platform](../../../docs/features/06-CrowPanel-P4-Platform/README.md), [33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md) ·
**Tests:** `test/` on the PC; on hardware (see docs/features/06)

Board support for the Elecrow CrowPanel Advanced 7", board rev V1.1. The board contract comes
from the CrowPanel Advanced P4 family library,
[`crowpanel_p4_family`](../crowpanel_p4_family) (display, touch, audio, battery: its README
has the shared facts and the rules); this component is what is the 7"'s own: the board described
in data (`src/board_desc.c`, `board_info()` and `board_name()` in `src/board_id.c`), its wiring
(`include/board_pins.h`) and its names in MQTT and Home Assistant (`include/board.h`). It holds no
alarm logic.

## Board facts
| Part | Detail (`include/board_pins.h`; the shared part in the family's `crowpanel_p4_pins.h`) |
|---|---|
| Display | 7" IPS 1024x600 (the family's) |
| Audio | two NS4168 I2S amplifiers on the family's I2S lines (no codec) |
| I2C connector | PH2.0, 4 pins: I2C0, shared with the GT911 |
| Wireless socket | UART 53 (to the module) / 54 (from it), SPI 6/7/8, IO 9/10 |
| J7 header | GPIO 2, 3, 4, 5, 25 (USB D+), 27, 28, 49-52 |
| Wired terminals | J7: GPIO 2, 3, 4, 5, 49, 50, 51, 52 (`BOARD_WIRED_TERMINALS`) |
| Add-on places (`board_desc.c`) | CC1101 `j7`: 52/51/50/49 + IO5; clock `j7`: IO27 SDA, IO28 SCL on I2C1; Z-Wave `socket` (53/54) or `j7` (IO27 TX, IO28 RX); its reset line on a wired terminal |
| MQTT / HA model | `CrowPanel Advanced 7" (ESP32-P4)` / `CrowPanel Advanced 7` (what Home Assistant has always shown) |

## Public API
| Header | What it offers |
|---|---|
| `include/board.h` | the family's `crowpanel_p4.h` (the board contract and its extras), this board's pins, `BOARD_MQTT_MODEL` and `BOARD_HA_MODEL` |
| `include/board_pins.h` | the 7"'s own GPIOs (socket, J7, places) after the family's `crowpanel_p4_pins.h` |

## Depends on
- Project components: `board_api`, `board_info`, `crowpanel_p4_family`

## Tests
`test/test_board_desc.c` checks the description on the PC (BRD-008 and the place and terminal
requirements it names): `pwsh tools/host_test.ps1`. On the panel: the family's README, "Tests".
