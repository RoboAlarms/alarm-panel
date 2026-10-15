/*
 * board_pins.h - the single source of truth for CrowPanel Advanced 7" (rev V1.1) wiring.
 * Sources: Elecrow wiki (docs/hardware/), Elecrow V1.2 hardware driver guide and the V1.1
 * ESP-IDF examples. See docs/features/06-CrowPanel-P4-Platform.
 *
 * What every CrowPanel Advanced P4 board shares (display, touch, audio lines, SD, C6, UART0) is
 * in the family's crowpanel_p4_pins.h; this file adds what is this board's own: its I2C
 * connector, the wireless-module socket, the J7 header and the places for add-ons.
 */
#pragma once

#include "crowpanel_p4_pins.h"

/* Its connector for add-on modules on I2C0 (the vendor wiki: PH2.0-4P, 3.3 V out, SDA IO45,
 * SCL IO46). */
#define BOARD_I2C_WHERE            "I2C connector (PH2.0, 4 pins)"

/* Wireless-module socket (docs/features/20): where the ESP32-H2 sensor radio plugs in - or a C6,
 * an nRF24L01+ or an SX1262. Taken from the V1.1 schematic (J9 + J11), not from the wiki:
 *
 *   module pin   board net      P4          what it is to us
 *   U1TXD        P4_RXD2        GPIO54      the radio talks to the panel
 *   U1RXD        P4_TXD2        GPIO53      the panel talks to the radio
 *   IO0/IO1/IO2  SPI2 MOSI/MISO/SCK  6/7/8  the SPI the LoRa and nRF modules use
 *   IO5 / IO4    IO9_W / IO10_W_BUSY 9/10   their spare signals
 *   IO12 / IO14  I2C1_SDA/SCL_3V3   45/46   ** the touch panel's bus **
 *   3V3 / GND / 5V                          power
 *
 * Two things follow. The module's RST and BOOT are buttons on the module and reach no P4 pin,
 * so the panel can only put the radio into its bootloader by asking it politely over the UART
 * (components/core/zigbee_host, ZB-001). And the module's IO12 and IO14 sit on the GT911's I2C bus:
 * radio firmware must leave those two pins alone, or the touchscreen stops answering.
 *
 * Board V1.2 swapped this UART pair with J7's 27/28 (the vendor's "Version 1.2 Update" note),
 * so on a V1.2 board the radio is on GPIO27/28 and J7 gains 53/54 (board_crowpanel_10). */
#define BOARD_RADIO_UART_TX_GPIO   53 /* -> module U1RXD (GPIO27 on board V1.2) */
#define BOARD_RADIO_UART_RX_GPIO   54 /* <- module U1TXD (GPIO28 on board V1.2) */
#define BOARD_RADIO_SPI_MOSI_GPIO  6
#define BOARD_RADIO_SPI_MISO_GPIO  7
#define BOARD_RADIO_SPI_CLK_GPIO   8
#define BOARD_RADIO_IO1_GPIO       9  /* SX1262 BUSY / nRF24 IRQ */
#define BOARD_RADIO_IO2_GPIO       10 /* SX1262 NSS / nRF24 CE */

/* A CC1101 breakout for the 433 MHz sensors (docs/features/25) goes on the same socket pads while
 * the socket is empty: SI 6, SO 7, SCLK 8, CSn on IO9, GDO0 (the demodulated data) on IO10,
 * 3V3 and GND. The pins are Kconfig (CONFIG_ALARM_RF_*), these are their defaults; IO10 is
 * not a strapping pin, which GDO0 must avoid (it idles high on noise). */

/* J7 expansion header GPIOs free for wired zones / outputs (3.3 V, not 5 V tolerant).
 * From the V1.1 schematic: 53 and 54 are NOT on J7, they are the wireless-module socket's UART
 * above; J7 carries 27 and 28 in their place (the other way round on board V1.2). */
#define BOARD_J7_GPIOS             {2, 3, 4, 5, 25, 27, 28, 49, 50, 51, 52}

/* Wired zone terminals (docs/features/22): a contact between the GPIO and GND, internal pull-up.
 * Only J7 pins that belong to nothing else are offered, because picking a terminal configures
 * the pin: 25 is the P4's USB D+ and 27/28 are the pair that becomes the radio's UART on a
 * V1.2 board, so they are left out (add them here if your board has nothing on them). */
#define BOARD_WIRED_TERMINALS       {2, 3, 4, 5, 49, 50, 51, 52}
#define BOARD_WIRED_TERMINALS_WHERE "J7 header"

/* The J7 places for add-ons (docs/research/AddOnWiringResearch.md), chosen at runtime:
 * - a CC1101 on four neighbouring row-A pins and IO5 (`rf place j7`, docs/features/25 RF-019), so
 *   the wireless-module socket stays free for the sensor radio; those five stop being wired
 *   zone terminals while it is there (IO2, IO3, IO4 remain);
 * - the clock module on IO27/IO28 with the P4's second I2C controller (`rtc place j7`,
 *   docs/features/35 RTC-013), away from the touch screen's bus. Free on a V1.1 board only: V1.2
 *   carries the socket's UART there. IO27 is also the second USB PHY's D+; ESP-IDF turns the
 *   pad and its pull-up off when the pin becomes a GPIO (gpio_ll_func_sel / pullup_dis). */
#define BOARD_J7_RF_SCK_GPIO   52
#define BOARD_J7_RF_MOSI_GPIO  51
#define BOARD_J7_RF_MISO_GPIO  50
#define BOARD_J7_RF_CS_GPIO    49
#define BOARD_J7_RF_GDO0_GPIO  5
#define BOARD_J7_I2C_SDA_GPIO  27
#define BOARD_J7_I2C_SCL_GPIO  28
#define BOARD_J7_I2C_PORT      1
#define BOARD_J7_I2C_WHERE     "J7 header, IO27 (SDA) and IO28 (SCL)"

/* The Z-Wave radio, a Zooz ZAC93 on a UART (docs/features/36 hardware.md §4), is off until a place
 * is chosen at runtime (`zwave place`):
 * - "socket": the wireless-module socket's UART above (TX 53, RX 54), while no ESP32-H2 is fitted;
 * - "j7": IO27 (TX, to the module's RX) and IO28 (RX) on J7 - the clock module's J7 place, which
 *   then goes to J13. Free on a V1.1 board only (V1.2 carries the socket's UART there).
 * Its reset line (the module's hardware version 2) goes on a J7 terminal, IO4 in the wiring
 * table (`zwave reset-pin 4`), which is then no zone's. */
#define BOARD_J7_ZWAVE_TX_GPIO 27
#define BOARD_J7_ZWAVE_RX_GPIO 28
#define BOARD_J7_ZWAVE_WHERE   "J7 header, IO27 (TX) and IO28 (RX)"
