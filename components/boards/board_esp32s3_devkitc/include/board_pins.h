/*
 * board_pins.h - the single source of truth for the ESP32-S3-DevKitC-1 (N16R8: ESP32-S3 with
 * 16 MB quad flash and 8 MB octal PSRAM) as a headless panel. Sources: Espressif's DevKitC-1
 * user guide (v1.0 and v1.1; header J1/J3 pin tables) and the ESP32-S3 datasheet (strapping,
 * flash and PSRAM pins). See docs/features/42.
 *
 * The board has no screen, touch or speaker of its own: what it offers are header pins. The
 * plan below gives each add-on a fixed place, so a person wires it once from this table.
 */
#pragma once

/* Taken by the module and the board itself, never offered:
 * GPIO26-32 the quad flash (and the PSRAM's chip select), GPIO33-37 the octal PSRAM,
 * GPIO19/20 the USB port (the console, USB-Serial/JTAG), GPIO43/44 UART0 behind the
 * USB-to-UART bridge (the "COM"/"UART" port), GPIO0/3/45/46 strapping pins. */
#define BOARD_USB_DM_GPIO          19
#define BOARD_USB_DP_GPIO          20
#define BOARD_UART0_TX_GPIO        43
#define BOARD_UART0_RX_GPIO        44

/* BOOT button (to GND, pulled up on the board): download mode when held at reset; after a normal
 * start, hold for five seconds and release to open setup recovery (docs/features/31
 * headless-setup.md). */
#define BOARD_BOOT_BUTTON_GPIO     0

/* The RGB LED (one WS2812): GPIO48 on DevKitC-1 v1.0 (the N16R8 clones), GPIO38 on v1.1. */
#define BOARD_RGB_LED_GPIO         48

/* Wired zone terminals (a contact between the pin and GND, internal pull-up; or a wired output
 * driving a relay module, docs/features/16): eight pins along header J1. */
#define BOARD_TERM_1_GPIO          4
#define BOARD_TERM_2_GPIO          5
#define BOARD_TERM_3_GPIO          6
#define BOARD_TERM_4_GPIO          7
#define BOARD_TERM_5_GPIO          15
#define BOARD_TERM_6_GPIO          16
#define BOARD_TERM_7_GPIO          17
#define BOARD_TERM_8_GPIO          18
#define BOARD_WIRED_TERMINALS_WHERE "header J1, IO4-7 and IO15-18"

/* SPI2 (FSPI's own pins): a CC1101 433 MHz radio (docs/features/25) and a microSD card module
 * share SCK/MOSI/MISO, each with its own chip select. */
#define BOARD_SPI_HOST             2
#define BOARD_SPI_SCK_GPIO         12
#define BOARD_SPI_MOSI_GPIO        11
#define BOARD_SPI_MISO_GPIO        13
#define BOARD_RF_CS_GPIO           10
#define BOARD_RF_DATA_GPIO         14 /* the CC1101's GDO0, or a receiver module's data line */
#define BOARD_SD_CS_GPIO           21

/* The add-on I2C modules (docs/features/35: a DS3231 clock). */
#define BOARD_I2C_SDA_GPIO         8
#define BOARD_I2C_SCL_GPIO         9

/* A Z-Wave radio (Zooz ZAC93, docs/features/36) on UART1, header J3: the panel's TX to the module's RX. */
#define BOARD_ZWAVE_TX_GPIO        1
#define BOARD_ZWAVE_RX_GPIO        2

/* Pins left for a person's own add-ons (BRD-018), on header J3. GPIO38 is the RGB LED on a
 * DevKitC-1 v1.1; on a v1.0 it is free. */
#define BOARD_FREE_1_GPIO          38
#define BOARD_FREE_2_GPIO          39
#define BOARD_FREE_3_GPIO          40
#define BOARD_FREE_4_GPIO          41
#define BOARD_FREE_5_GPIO          42
#define BOARD_FREE_6_GPIO          47
