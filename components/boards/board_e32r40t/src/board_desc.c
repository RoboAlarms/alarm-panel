/*
 * board_desc.c - the LCDWiki E32R40T (4.0" ESP32-32E display) described in data (board_info_t).
 *
 * Portable: the host tests compile this file and validate it (test/test_board_desc.c), so a
 * pin clash or a contact on a strapping pin is caught on the PC. Every number comes from
 * board_pins.h.
 */
#include "board_desc.h"

#include "board_pins.h"

#define OUT BOARD_PIN_OUT
#define IN BOARD_PIN_IN
#define BUS BOARD_PIN_SHARED

static const board_pin_t PINS[] = {
    /* SPI2: the display and the touch controller share SCK/MOSI/MISO. */
    {"LCD SCK", BOARD_LCD_SCK_GPIO, OUT | BUS, BOARD_BUS_SPI(BOARD_LCD_SPI_HOST), BOARD_SIG_SCK},
    {"LCD MOSI", BOARD_LCD_MOSI_GPIO, OUT | BUS, BOARD_BUS_SPI(BOARD_LCD_SPI_HOST), BOARD_SIG_MOSI},
    {"LCD MISO", BOARD_LCD_MISO_GPIO, IN | BUS, BOARD_BUS_SPI(BOARD_LCD_SPI_HOST), BOARD_SIG_MISO},
    {"LCD CS", BOARD_LCD_CS_GPIO, OUT, 0, 0},
    {"LCD DC", BOARD_LCD_DC_GPIO, OUT, 0, 0},
    {"backlight", BOARD_LCD_BACKLIGHT_GPIO, OUT, 0, 0},
    {"touch SCK", BOARD_LCD_SCK_GPIO, OUT | BUS, BOARD_BUS_SPI(BOARD_LCD_SPI_HOST), BOARD_SIG_SCK},
    {"touch MOSI", BOARD_LCD_MOSI_GPIO, OUT | BUS, BOARD_BUS_SPI(BOARD_LCD_SPI_HOST), BOARD_SIG_MOSI},
    {"touch MISO", BOARD_LCD_MISO_GPIO, IN | BUS, BOARD_BUS_SPI(BOARD_LCD_SPI_HOST), BOARD_SIG_MISO},
    {"touch CS", BOARD_TOUCH_CS_GPIO, OUT, 0, 0},
    {"touch PENIRQ", BOARD_TOUCH_IRQ_GPIO, IN, 0, 0},
    {"LED red", BOARD_LED_RED_GPIO, OUT, 0, 0},
    {"LED green", BOARD_LED_GREEN_GPIO, OUT, 0, 0},
    {"LED blue", BOARD_LED_BLUE_GPIO, OUT, 0, 0},
    {"audio DAC", BOARD_AUDIO_DAC_GPIO, OUT, 0, 0},
    {"amplifier enable", BOARD_AUDIO_AMP_EN_GPIO, OUT, 0, 0},
    {"battery ADC", BOARD_BATTERY_ADC_GPIO, IN, 0, 0},
    {"UART0 TX (CH340C)", BOARD_UART0_TX_GPIO, OUT, 0, 0},
    {"UART0 RX (CH340C)", BOARD_UART0_RX_GPIO, IN, 0, 0},
    {"BOOT button", BOARD_BOOT_BUTTON_GPIO, IN, 0, 0},
};

static const board_terminal_t TERMINALS[] = {
    {BOARD_INPUT_B_GPIO, true}, /* input only, no internal pull-up: fit one */
    {BOARD_I2C_SCL_GPIO, false},
    {BOARD_I2C_SDA_GPIO, false},
};

const board_info_t board_e32r40t_desc = {
    .id = "e32r40t",
    .name = "LCDWiki 4.0in ESP32-32E (E32R40T)",
    .chip = BOARD_CHIP_ESP32,
    .psram = false,
    .octal_psram = false,
    .width = BOARD_LCD_H_RES,
    .height = BOARD_LCD_V_RES,
    .touch = BOARD_TOUCH_RESISTIVE,
    .audio = BOARD_AUDIO_DAC,
    .audio_rate_hz = 24000, /* the DAC's DMA needs 20 kHz or more on its default clock */
    .battery = true,
    .pins = PINS,
    .pin_count = sizeof(PINS) / sizeof(PINS[0]),
    .reach = NULL, /* no pins for add-ons of a person's own (BRD-018) */
    .reach_count = 0,
    .terminals = TERMINALS,
    .terminal_count = sizeof(TERMINALS) / sizeof(TERMINALS[0]),
    .terminals_where = BOARD_WIRED_TERMINALS_WHERE,
    .sd = {.bus = BOARD_SD_SPI,
           .spi_host = BOARD_SD_SPI_HOST,
           .clk = BOARD_NO_PIN,
           .cmd = BOARD_NO_PIN,
           .d0 = BOARD_NO_PIN,
           .sck = BOARD_SD_SCK_GPIO,
           .mosi = BOARD_SD_MOSI_GPIO,
           .miso = BOARD_SD_MISO_GPIO,
           .cs = BOARD_SD_CS_GPIO},
    /* A CC1101 on the SPI header (sharing the card's bus) or a receiver module's data line. */
    .rf = {.spi_host = BOARD_SD_SPI_HOST,
           .sck = BOARD_SD_SCK_GPIO,
           .mosi = BOARD_SD_MOSI_GPIO,
           .miso = BOARD_SD_MISO_GPIO,
           .cs = BOARD_SPI_HEADER_CS_GPIO,
           .data = BOARD_INPUT_A_GPIO,
           .where = "SPI header, data on IO35"},
    .radio = {.tx = BOARD_NO_PIN, .rx = BOARD_NO_PIN, .where = NULL},
    /* Its I2C header's two pins are wired zone terminals here, so no clock module (yet). */
    .i2c = {.sda = BOARD_NO_PIN, .scl = BOARD_NO_PIN, .where = NULL},
};
