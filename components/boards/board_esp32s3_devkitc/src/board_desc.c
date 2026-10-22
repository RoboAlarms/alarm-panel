/*
 * board_desc.c - the ESP32-S3-DevKitC-1 (N16R8) as a headless panel, described in data
 * (board_info_t).
 *
 * Portable: the host tests compile this file and validate it (test/test_board_desc.c), so a
 * pin clash or a terminal on a strapping pin is caught on the PC. Every number comes from
 * board_pins.h.
 */
#include "board_desc.h"

#include "board_pins.h"

#define OUT BOARD_PIN_OUT
#define IN BOARD_PIN_IN
#define IO (BOARD_PIN_OUT | BOARD_PIN_IN)

/* Every pin the board's own parts use (BRD-019). The add-on places (SPI, I2C, the Z-Wave UART)
 * are in the fields below, not here. */
static const board_pin_t PINS[] = {
    {"USB D-", BOARD_USB_DM_GPIO, IO, 0, 0},
    {"USB D+", BOARD_USB_DP_GPIO, IO, 0, 0},
    {"UART0 TX (USB-to-UART)", BOARD_UART0_TX_GPIO, OUT, 0, 0},
    {"UART0 RX (USB-to-UART)", BOARD_UART0_RX_GPIO, IN, 0, 0},
    {"BOOT button", BOARD_BOOT_BUTTON_GPIO, IN, 0, 0},
    {"RGB LED", BOARD_RGB_LED_GPIO, OUT, 0, 0},
};

/* The pins a person can reach for an add-on of their own (BRD-018), as printed on the board.
 * Tools read this table: keep one entry per line in this shape. */
static const board_reach_t REACHABLE[] = {
    {38, "38", "J3"},
    {39, "39", "J3"},
    {40, "40", "J3"},
    {41, "41", "J3"},
    {42, "42", "J3"},
    {47, "47", "J3"},
};

static const board_terminal_t TERMINALS[] = {
    {BOARD_TERM_1_GPIO, false}, {BOARD_TERM_2_GPIO, false}, {BOARD_TERM_3_GPIO, false},
    {BOARD_TERM_4_GPIO, false}, {BOARD_TERM_5_GPIO, false}, {BOARD_TERM_6_GPIO, false},
    {BOARD_TERM_7_GPIO, false}, {BOARD_TERM_8_GPIO, false},
};

/* The Z-Wave radio's one place: UART1 on IO1 (TX) and IO2 (RX). */
static const board_zwave_place_t ZWAVE_PLACES[] = {
    {"header", BOARD_ZWAVE_TX_GPIO, BOARD_ZWAVE_RX_GPIO, "header J3, IO1 (TX) and IO2 (RX)"},
};

const board_info_t board_esp32s3_devkitc_desc = {
    .id = "esp32s3_devkitc",
    .name = "ESP32-S3-DevKitC-1 (headless)",
    .chip = BOARD_CHIP_ESP32S3,
    .psram = true,
    .octal_psram = true,
    .width = 0, /* no screen */
    .height = 0,
    .touch = BOARD_TOUCH_NONE,
    .audio = BOARD_AUDIO_NONE,
    .audio_rate_hz = 0,
    .battery = false,
    .pins = PINS,
    .pin_count = sizeof(PINS) / sizeof(PINS[0]),
    .reach = REACHABLE,
    .reach_count = sizeof(REACHABLE) / sizeof(REACHABLE[0]),
    .terminals = TERMINALS,
    .terminal_count = sizeof(TERMINALS) / sizeof(TERMINALS[0]),
    .terminals_where = BOARD_WIRED_TERMINALS_WHERE,
    /* A microSD module on SPI2, beside the 433 MHz radio. */
    .sd = {.bus = BOARD_SD_SPI,
           .spi_host = BOARD_SPI_HOST,
           .clk = BOARD_NO_PIN,
           .cmd = BOARD_NO_PIN,
           .d0 = BOARD_NO_PIN,
           .sck = BOARD_SPI_SCK_GPIO,
           .mosi = BOARD_SPI_MOSI_GPIO,
           .miso = BOARD_SPI_MISO_GPIO,
           .cs = BOARD_SD_CS_GPIO},
    /* A CC1101 (GDO0 on IO14), or a receiver module's data line on IO14. */
    .rf = {.spi_host = BOARD_SPI_HOST,
           .sck = BOARD_SPI_SCK_GPIO,
           .mosi = BOARD_SPI_MOSI_GPIO,
           .miso = BOARD_SPI_MISO_GPIO,
           .cs = BOARD_RF_CS_GPIO,
           .data = BOARD_RF_DATA_GPIO,
           .where = "header J1, SPI on IO10-13, data on IO14"},
    .radio = {.tx = BOARD_NO_PIN, .rx = BOARD_NO_PIN, .where = NULL},
    .i2c = {.sda = BOARD_I2C_SDA_GPIO,
            .scl = BOARD_I2C_SCL_GPIO,
            .where = "header J1, IO8 (SDA) and IO9 (SCL)",
            .port = 0},
    .zwave_places = ZWAVE_PLACES,
    .zwave_place_count = sizeof(ZWAVE_PLACES) / sizeof(ZWAVE_PLACES[0]),
};
