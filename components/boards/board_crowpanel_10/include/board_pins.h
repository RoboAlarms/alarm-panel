/*
 * board_pins.h - the single source of truth for CrowPanel Advanced 10.1" (rev V1.2) wiring.
 * Source: Elecrow's V1.2 schematic for the 10.1" (docs/hardware/), read net by net. See
 * docs/features/06-CrowPanel-P4-Platform.
 *
 * What every CrowPanel Advanced P4 board shares (display, touch, audio lines, SD, C6, UART0) is
 * in the family's crowpanel_p4_pins.h; this file adds what is this board's own: its I2C
 * connector, the wireless-module socket, the J7 header and the places for add-ons. Against the
 * 7" (rev V1.1, board_crowpanel_p4) the socket's UART and J7's 53/54 swapped places with 27/28
 * (Elecrow's "Version 1.2 Update" note). The sound is plain I2S into the amplifier, as on the 7"
 * (Elecrow's V1.2 examples, and the board this was tried on); the schematic also draws an ES8311
 * codec with an NS4263B amplifier, which the family sets up where one answers
 * (CONFIG_BOARD_CROWPANEL_P4_ES8311).
 */
#pragma once

#include "crowpanel_p4_pins.h"

/* Its connector for add-on modules on I2C0 (J13, HY2.0-4P: SCL, SDA, 3V3, GND), behind the same
 * level shifters as the touch panel. */
#define BOARD_I2C_WHERE            "I2C connector J13 (HY2.0, 4 pins)"

/* Wireless-module socket (docs/features/20), J9 + J11 as on the 7", but its UART is GPIO27/28:
 *
 *   module pin   board net      P4          what it is to us
 *   U1RXD        P4_TXD2        GPIO27      the panel talks to the radio (J9 pin 1)
 *   U1TXD        P4_RXD2        GPIO28      the radio talks to the panel (J11 pin 1)
 *   IO0/IO1/IO2  SPI2 MOSI/MISO/SCK  6/7/8  the SPI the LoRa and nRF modules use
 *   IO5 / IO4    IO9_W / IO10_W_BUSY 9/10   their spare signals
 *   IO12 / IO14  I2C1_SDA/SCL_3V3   45/46   ** the touch panel's bus **
 *
 * As on the 7": the module's RST and BOOT reach no P4 pin, and radio firmware must leave its
 * IO12 and IO14 alone or the touchscreen stops answering. IO27 is also the second USB PHY's D+;
 * ESP-IDF turns the pad and its pull-up off when the pin becomes a GPIO. */
#define BOARD_RADIO_UART_TX_GPIO   27 /* -> module U1RXD */
#define BOARD_RADIO_UART_RX_GPIO   28 /* <- module U1TXD */
#define BOARD_RADIO_SPI_MOSI_GPIO  6
#define BOARD_RADIO_SPI_MISO_GPIO  7
#define BOARD_RADIO_SPI_CLK_GPIO   8
#define BOARD_RADIO_IO1_GPIO       9  /* SX1262 BUSY / nRF24 IRQ */
#define BOARD_RADIO_IO2_GPIO       10 /* SX1262 NSS / nRF24 CE */

/* A CC1101 breakout for the 433 MHz sensors (docs/features/25) goes on the socket's pads while
 * it is empty, as on the 7": SI 6, SO 7, SCLK 8, CSn on IO9, GDO0 on IO10, 3V3 and GND. */

/* J7 expansion header (2x12) GPIOs, 3.3 V, not 5 V tolerant: IO2-IO5, IO25 (the P4's USB D+),
 * IO49-IO54. */
#define BOARD_J7_GPIOS             {2, 3, 4, 5, 25, 49, 50, 51, 52, 53, 54}

/* Wired zone terminals (docs/features/22): a contact between the GPIO and GND, internal pull-up.
 * The same eight as the 7", so its wiring tables hold here too: 25 is USB D+, and 53/54 are
 * the J7 place of the clock module and the Z-Wave radio (where the 7" has 27/28). */
#define BOARD_WIRED_TERMINALS       {2, 3, 4, 5, 49, 50, 51, 52}
#define BOARD_WIRED_TERMINALS_WHERE "J7 header"

/* The J7 places for add-ons (docs/research/AddOnWiringResearch.md), chosen at runtime as on the
 * 7": a CC1101 on IO52/51/50/49 and IO5 (`rf place j7`, RF-019), and the clock module on
 * IO53/IO54 with the P4's second I2C controller (`rtc place j7`, RTC-013). */
#define BOARD_J7_RF_SCK_GPIO   52
#define BOARD_J7_RF_MOSI_GPIO  51
#define BOARD_J7_RF_MISO_GPIO  50
#define BOARD_J7_RF_CS_GPIO    49
#define BOARD_J7_RF_GDO0_GPIO  5
#define BOARD_J7_I2C_SDA_GPIO  53
#define BOARD_J7_I2C_SCL_GPIO  54
#define BOARD_J7_I2C_PORT      1
#define BOARD_J7_I2C_WHERE     "J7 header, IO53 (SDA) and IO54 (SCL)"

/* The Z-Wave radio (docs/features/36 hardware.md §4), off until a place is chosen (`zwave
 * place`): "socket", the socket's UART above while no ESP32-H2 is fitted, or "j7", IO53 (TX, to
 * the module's RX) and IO54 (RX) - the clock module's J7 place, which then goes to J13. Its
 * reset line goes on a J7 terminal (IO4 in the wiring table). */
#define BOARD_J7_ZWAVE_TX_GPIO 53
#define BOARD_J7_ZWAVE_RX_GPIO 54
#define BOARD_J7_ZWAVE_WHERE   "J7 header, IO53 (TX) and IO54 (RX)"

/* GPIO29 (LCD_BK_POWER) has a 10K pull-up to 5 V and goes to the gate of a backlight power
 * switch the board doesn't fit: the backlight runs from GPIO31 alone. Never drive it or offer it. */
#define BOARD_LCD_BK_POWER_GPIO    29
