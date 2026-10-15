/*
 * board_desc.c - the CrowPanel Advanced 10.1" (rev V1.2) described in data (board_info_t).
 *
 * Portable: the host tests compile this file and validate it (test/test_board_desc.c), so a
 * pin clash or a terminal on a strapping pin is caught on the PC. Every number comes from
 * board_pins.h, the single source of truth for this board's wiring.
 */
#include "board_desc.h"

#include "board_pins.h"

#define OUT BOARD_PIN_OUT
#define IN BOARD_PIN_IN
#define IO (BOARD_PIN_OUT | BOARD_PIN_IN)
#define BUS BOARD_PIN_SHARED

/* Every pin the board's own parts use, connected to the firmware or not (BRD-019): a pin
 * missing here could be offered to an add-on. Sources: board_pins.h, sdkconfig.defaults
 * (CONFIG_ESP_HOSTED_SDIO_*) and the V1.2 schematic for the 10.1", net by net. A bus line names
 * its bus and signal; the rest leave both 0. */
static const board_pin_t PINS[] = {
    {"backlight PWM", BOARD_LCD_BACKLIGHT_GPIO, OUT, 0, 0},
    {"LCD reset", BOARD_LCD_RESET_GPIO, OUT, 0, 0},
    /* I2C0: the GT911, the STC8 and the J13 connector, and the ES8311 codec where one is fitted;
     * the wireless socket's IO12/IO14 too. */
    {"I2C0 SDA", BOARD_I2C_SDA_GPIO, IO | BUS, BOARD_BUS_I2C(0), BOARD_SIG_SDA},
    {"I2C0 SCL", BOARD_I2C_SCL_GPIO, OUT | BUS, BOARD_BUS_I2C(0), BOARD_SIG_SCL},
    {"touch reset", BOARD_TOUCH_RST_GPIO, OUT, 0, 0},
    {"touch interrupt", BOARD_TOUCH_INT_GPIO, IN, 0, 0},
    {"audio LRCLK", BOARD_AUDIO_LRCLK_GPIO, OUT, 0, 0},
    {"audio BCLK", BOARD_AUDIO_BCLK_GPIO, OUT, 0, 0},
    {"audio DOUT", BOARD_AUDIO_DOUT_GPIO, OUT, 0, 0},
    {"amplifier enable", BOARD_AUDIO_AMP_EN_GPIO, OUT, 0, 0},
    /* The ES8311's MCLK where one is fitted; otherwise the PDM microphone's clock, as on the 7"
     * (Elecrow's V1.2 examples). No microphone is used. */
    {"codec MCLK / microphone CLK", BOARD_AUDIO_MCLK_GPIO, OUT, 0, 0},
    {"microphone DATA", 26, IN, 0, 0},
    /* A 10K pull-up to 5 V and an unfitted backlight power switch: never driven (board_pins.h). */
    {"backlight power (unfitted)", BOARD_LCD_BK_POWER_GPIO, IN, 0, 0},
    /* ESP32-C6 over SDIO slot 1 (pins in sdkconfig.defaults, CONFIG_ESP_HOSTED_SDIO_*). */
    {"C6 SDIO CLK", 18, OUT, 0, 0},
    {"C6 SDIO CMD", 19, IO, 0, 0},
    {"C6 SDIO D0", 17, IO, 0, 0},
    {"C6 SDIO D1", 16, IO, 0, 0},
    {"C6 SDIO D2", 15, IO, 0, 0},
    {"C6 SDIO D3", 14, IO, 0, 0},
    {"C6 enable", BOARD_C6_EN_GPIO, OUT, 0, 0},
    {"UART0 TX (CH340K)", BOARD_UART0_TX_GPIO, OUT, 0, 0},
    {"UART0 RX (CH340K)", BOARD_UART0_RX_GPIO, IN, 0, 0},
    {"USB D+", 25, IO, 0, 0},
    /* The UART1 connector (J2, HY2.0: RX1, TX1, 3V3, GND; not used). */
    {"UART1 connector TX1", 47, OUT, 0, 0},
    {"UART1 connector RX1", 48, IN, 0, 0},
    /* The UART3 IN connector (RX3/TX3, a 5 V power input behind level shifters; not used). */
    {"UART3 IN connector RX3", 33, IN, 0, 0},
    {"UART3 IN connector TX3", 34, OUT, 0, 0},
    /* The camera connector (CIS-CAM, not used): its I2C and the CSI reset. */
    {"camera I2C SDA", 12, IO, 0, 0},
    {"camera I2C SCL", 13, OUT, 0, 0},
    {"camera reset", 11, OUT, 0, 0},
};

/* The pins a person can reach for an add-on of their own (BRD-018): J7's IO pins, as printed.
 * IO25 is the P4's USB D+ and stays out. Tools read this table (tools/tests): keep one entry per
 * line in this shape. */
static const board_reach_t REACHABLE[] = {
    {2, "IO2", "J7"},
    {3, "IO3", "J7"},
    {4, "IO4", "J7"},
    {5, "IO5", "J7"},
    {49, "IO49", "J7"},
    {50, "IO50", "J7"},
    {51, "IO51", "J7"},
    {52, "IO52", "J7"},
    {53, "IO53", "J7"},
    {54, "IO54", "J7"},
};

static const board_terminal_t TERMINALS[] = {
    {2, false}, {3, false}, {4, false}, {5, false},
    {49, false}, {50, false}, {51, false}, {52, false},
};

/* The J7 places for add-ons (board_pins.h): a CC1101 there instead of on the socket, and the
 * clock module on an I2C controller of its own. */
static const board_rf_place_t RF_PLACES[] = {
    {"j7", {.spi_host = 2,
            .sck = BOARD_J7_RF_SCK_GPIO,
            .mosi = BOARD_J7_RF_MOSI_GPIO,
            .miso = BOARD_J7_RF_MISO_GPIO,
            .cs = BOARD_J7_RF_CS_GPIO,
            .data = BOARD_J7_RF_GDO0_GPIO,
            .where = "J7 header"}},
};
static const board_i2c_place_t I2C_PLACES[] = {
    {"j7", BOARD_J7_I2C_PORT, BOARD_J7_I2C_SDA_GPIO, BOARD_J7_I2C_SCL_GPIO, BOARD_J7_I2C_WHERE},
};
/* The Z-Wave radio's places (docs/features/36 hardware.md §4): the socket's UART while no sensor
 * radio is fitted, or two J7 pins. */
static const board_zwave_place_t ZWAVE_PLACES[] = {
    {"socket", BOARD_RADIO_UART_TX_GPIO, BOARD_RADIO_UART_RX_GPIO, "wireless-module socket"},
    {"j7", BOARD_J7_ZWAVE_TX_GPIO, BOARD_J7_ZWAVE_RX_GPIO, BOARD_J7_ZWAVE_WHERE},
};

const board_info_t board_crowpanel_10_desc = {
    .id = "crowpanel_10",
    .name = "CrowPanel Advanced 10.1in (ESP32-P4) V1.2",
    .chip = BOARD_CHIP_ESP32P4,
    .psram = true,
    .octal_psram = false,
    .width = BOARD_LCD_H_RES,
    .height = BOARD_LCD_V_RES,
    .touch = BOARD_TOUCH_CAPACITIVE,
    .audio = BOARD_AUDIO_I2S,
    .audio_rate_hz = 32000, /* as the 7" plays */
    .battery = true,
    .pins = PINS,
    .pin_count = sizeof(PINS) / sizeof(PINS[0]),
    .reach = REACHABLE,
    .reach_count = sizeof(REACHABLE) / sizeof(REACHABLE[0]),
    .terminals = TERMINALS,
    .terminal_count = sizeof(TERMINALS) / sizeof(TERMINALS[0]),
    .terminals_where = BOARD_WIRED_TERMINALS_WHERE,
    .sd = {.bus = BOARD_SD_SDMMC_1BIT,
           .spi_host = -1,
           .clk = BOARD_SD_CLK_GPIO,
           .cmd = BOARD_SD_CMD_GPIO,
           .d0 = BOARD_SD_D0_GPIO,
           .sck = BOARD_NO_PIN,
           .mosi = BOARD_NO_PIN,
           .miso = BOARD_NO_PIN,
           .cs = BOARD_NO_PIN},
    /* A CC1101 or a receiver module on the wireless socket while it is empty (docs/features/25). */
    .rf = {.spi_host = 2,
           .sck = BOARD_RADIO_SPI_CLK_GPIO,
           .mosi = BOARD_RADIO_SPI_MOSI_GPIO,
           .miso = BOARD_RADIO_SPI_MISO_GPIO,
           .cs = BOARD_RADIO_IO1_GPIO,
           .data = BOARD_RADIO_IO2_GPIO,
           .where = "wireless-module socket"},
    .radio = {.tx = BOARD_RADIO_UART_TX_GPIO,
              .rx = BOARD_RADIO_UART_RX_GPIO,
              .where = "wireless-module socket"},
    /* The I2C connector J13 (HY2.0, 4 pins: SCL, SDA, 3V3, GND) is I2C0, shared with the GT911.
     * A DS3231 clock module plugs in here (docs/features/35). */
    .i2c = {.sda = BOARD_I2C_SDA_GPIO, .scl = BOARD_I2C_SCL_GPIO, .where = BOARD_I2C_WHERE},
    .rf_places = RF_PLACES,
    .rf_place_count = sizeof(RF_PLACES) / sizeof(RF_PLACES[0]),
    .i2c_places = I2C_PLACES,
    .i2c_place_count = sizeof(I2C_PLACES) / sizeof(I2C_PLACES[0]),
    .zwave_places = ZWAVE_PLACES,
    .zwave_place_count = sizeof(ZWAVE_PLACES) / sizeof(ZWAVE_PLACES[0]),
};
