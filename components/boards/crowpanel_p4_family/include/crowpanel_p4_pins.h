/*
 * crowpanel_p4_pins.h - the wiring every CrowPanel Advanced ESP32-P4 board shares.
 *
 * Elecrow builds its 7" and 10.1" ESP32-P4 panels on one main board: the same
 * P4, display interface, touch bus, audio lines, SD card and C6 co-processor. What differs between
 * models and board revisions (the wireless-module socket's UART, the J7 header, the parts on the
 * audio lines) is in each board's own board_pins.h, which includes this file.
 * Sources: the V1.1 and V1.2 schematics and Elecrow's hardware driver guides (docs/hardware/),
 * docs/features/06-CrowPanel-P4-Platform.
 */
#pragma once

/* Display: IPS 1024x600, EK79007 controller, MIPI-DSI 2 lanes (dedicated pins). The 7" and the
 * 10.1" use the same panel timing (Elecrow's examples for both). */
#define BOARD_LCD_H_RES            1024
#define BOARD_LCD_V_RES            600
#define BOARD_LCD_DSI_LANES        2
#define BOARD_LCD_LANE_MBPS        900
#define BOARD_LCD_DPI_CLOCK_MHZ    51
#define BOARD_LCD_BACKLIGHT_GPIO   31 /* LEDC PWM, 30 kHz, to the MT9201 */
/* The EK79007's reset GRB, active low, with a 2K pull-up to VDDPST_5 (LDO4, below). */
#define BOARD_LCD_RESET_GPIO       41

/* Internal LDOs the board relies on (as in Elecrow's validated examples). */
#define BOARD_LDO_MIPI_PHY_CHAN    3 /* MIPI DSI PHY supply */
#define BOARD_LDO_MIPI_PHY_MV      2500
#define BOARD_LDO_AUX_CHAN         4
#define BOARD_LDO_AUX_MV           3300

/* I2C0: GT911 touch + the I2C connector (level shifted to 3.3 V); an ES8311 codec too where the
 * board has one. */
#define BOARD_I2C_SDA_GPIO         45
#define BOARD_I2C_SCL_GPIO         46
#define BOARD_I2C_HZ               400000
#define BOARD_TOUCH_RST_GPIO       40 /* active low */
#define BOARD_TOUCH_INT_GPIO       42

/* Audio out on I2S1, and the amplifier enable: active LOW, 0 = on. The 7" V1.1 has two NS4168
 * I2S amplifiers on these lines (GPIO30 through a P-MOSFET to their CTRL), and the 10.1" V1.2
 * plays the same way (Elecrow's V1.2 examples; its board answers no codec). The 10.1"'s
 * schematic also draws an ES8311 codec on them (CONFIG_BOARD_CROWPANEL_P4_ES8311) feeding an
 * NS4263B amplifier, whose SD pin GPIO30 drives directly (high > 100 us: shut down; a falling
 * edge: class AB). */
#define BOARD_AUDIO_LRCLK_GPIO     21
#define BOARD_AUDIO_BCLK_GPIO      22
#define BOARD_AUDIO_DOUT_GPIO      23
#define BOARD_AUDIO_AMP_EN_GPIO    30
/* ES8311 boards: the codec's MCLK (otherwise the PDM microphone's clock pin) and its I2C
 * address (CE tied low). */
#define BOARD_AUDIO_MCLK_GPIO      24
#define BOARD_AUDIO_CODEC_ADDR     0x18

/* ESP32-C6 co-processor (ESP-Hosted over SDIO slot 1, 4-bit). Configured in sdkconfig:
 * CLK 18, CMD 19, D0 17, D1 16, D2 15, D3 14, EN on GPIO32 (active high). */
#define BOARD_C6_EN_GPIO           32

/* microSD: SDMMC slot 0, 1-bit; no verified card-detect or power-control pin. */
#define BOARD_SD_CLK_GPIO          43
#define BOARD_SD_CMD_GPIO          44
#define BOARD_SD_D0_GPIO           39

/* Debug/download UART0 through the CH340K (USB-C "UART0" port). */
#define BOARD_UART0_TX_GPIO        37
#define BOARD_UART0_RX_GPIO        38
