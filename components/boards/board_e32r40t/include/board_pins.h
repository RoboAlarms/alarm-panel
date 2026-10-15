/*
 * board_pins.h - the single source of truth for the LCDWiki 4.0" ESP32-32E display's wiring
 * (SKU E32R40T, with resistive touch; E32N40T without). Source: the LCDWiki page's "ESP32 Pin
 * Assignment" table, https://www.lcdwiki.com/4.0inch_ESP32-32E_Display. See docs/features/19.
 */
#pragma once

/* Display: 3.95" TN, ST7796S, 320x480 portrait, 4-wire SPI on SPI2 (HSPI) shared with the
 * touch controller. The panel's reset is tied to EN: every chip reset resets it. */
#define BOARD_LCD_H_RES            480 /* as the UI draws it (landscape, docs/features/19) */
#define BOARD_LCD_V_RES            320
#define BOARD_LCD_SPI_HOST         2
#define BOARD_LCD_SCK_GPIO         14
#define BOARD_LCD_MOSI_GPIO        13
#define BOARD_LCD_MISO_GPIO        12 /* the flash-voltage strapping pin (MTDI) */
#define BOARD_LCD_CS_GPIO          15
#define BOARD_LCD_DC_GPIO          2
#define BOARD_LCD_BACKLIGHT_GPIO   27 /* high = on */
#define BOARD_LCD_PCLK_HZ          40000000 /* SPI write clock */
#define BOARD_LCD_BUF_LINES        5        /* LVGL draws 5 lines at a time, two 4.8 KB buffers */
/* How this panel is glued and wired (the console's `lcd` changes them live for bring-up):
 * a 320x480 portrait ST7796S used in landscape, blue-green-red, TN (no inversion). */
#define BOARD_LCD_BGR              1
#define BOARD_LCD_INVERT           0
#define BOARD_LCD_SWAP_XY          1
#define BOARD_LCD_MIRROR_X         0
#define BOARD_LCD_MIRROR_Y         0

/* Resistive touch: XPT2046 on the display's bus. */
#define BOARD_TOUCH_CS_GPIO        33
#define BOARD_TOUCH_IRQ_GPIO       36 /* PENIRQ, low while pressed; input only (unused: pressure decides) */
/* Until the three-target calibration has run (it does at first start, and `touch cal`):
 * a raw range per axis, typical for these films. */
#define BOARD_TOUCH_RAW_X_MIN      300
#define BOARD_TOUCH_RAW_X_MAX      3800
#define BOARD_TOUCH_RAW_Y_MIN      300
#define BOARD_TOUCH_RAW_Y_MAX      3800
#define BOARD_TOUCH_SWAP_XY        1
#define BOARD_TOUCH_MIRROR_X       0
#define BOARD_TOUCH_MIRROR_Y       0
#define BOARD_TOUCH_PRESS_MIN      400 /* touch_pressure() of a finger, not a brush */

/* RGB LED, common anode: low = on. */
#define BOARD_LED_RED_GPIO         22
#define BOARD_LED_GREEN_GPIO       16
#define BOARD_LED_BLUE_GPIO        17

/* microSD on SPI3 (VSPI), shared with the 4-pin SPI header. */
#define BOARD_SD_SPI_HOST          3
#define BOARD_SD_SCK_GPIO          18
#define BOARD_SD_MOSI_GPIO         23
#define BOARD_SD_MISO_GPIO         19
#define BOARD_SD_CS_GPIO           5
#define BOARD_SPI_HEADER_CS_GPIO   21 /* the SPI header's own chip select */

/* Audio: DAC channel 1 into an FM8002E amplifier. */
#define BOARD_AUDIO_DAC_GPIO       26
#define BOARD_AUDIO_AMP_EN_GPIO    4  /* low = amplifier on */

/* Battery voltage (TP4054 charger, 3.7 V LiPo socket) on ADC1. */
#define BOARD_BATTERY_ADC_GPIO     34

/* Debug/download UART0 through the CH340C (USB-C). */
#define BOARD_UART0_TX_GPIO        1
#define BOARD_UART0_RX_GPIO        3

/* BOOT button (download mode when held at reset): a strapping pin. */
#define BOARD_BOOT_BUTTON_GPIO     0

/* The headers: I2C (IO25 SCL, IO32 SDA) and two input-only pins (IO35, IO39, no pull
 * resistors inside the chip). */
#define BOARD_I2C_SCL_GPIO         25
#define BOARD_I2C_SDA_GPIO         32
#define BOARD_INPUT_A_GPIO         35
#define BOARD_INPUT_B_GPIO         39

/* Where things attach (board_desc.c): a CC1101 on the SPI header (CS IO21) with its data line
 * on IO35, or a receiver module's data line on IO35; wired zone terminals on IO39 (an external
 * pull-up must be fitted), IO25 and IO32 (internal pull-ups; the I2C header is then not used
 * for I2C). */
#define BOARD_WIRED_TERMINALS_WHERE "input and I2C headers"
