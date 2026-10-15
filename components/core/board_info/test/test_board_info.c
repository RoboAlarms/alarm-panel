/* Board descriptions and their validation (docs/features/33 step 4, BRD-NNN). */
#include "board_info/board_info.h"
#include "unity.h"

#include <string.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* A small, safe classic-ESP32 board: an SPI display and touch on one bus, SD on another. */
#define SPI2 BOARD_BUS_SPI(2)
static const board_pin_t PINS[] = {
    {"LCD SCK", 14, BOARD_PIN_OUT | BOARD_PIN_SHARED, SPI2, BOARD_SIG_SCK},
    {"LCD MOSI", 13, BOARD_PIN_OUT | BOARD_PIN_SHARED, SPI2, BOARD_SIG_MOSI},
    {"LCD MISO", 12, BOARD_PIN_IN | BOARD_PIN_SHARED, SPI2, BOARD_SIG_MISO},
    {"LCD CS", 15, BOARD_PIN_OUT, 0, 0},
    {"LCD DC", 2, BOARD_PIN_OUT, 0, 0},
    {"touch SCK", 14, BOARD_PIN_OUT | BOARD_PIN_SHARED, SPI2, BOARD_SIG_SCK},
    {"touch MOSI", 13, BOARD_PIN_OUT | BOARD_PIN_SHARED, SPI2, BOARD_SIG_MOSI},
    {"touch MISO", 12, BOARD_PIN_IN | BOARD_PIN_SHARED, SPI2, BOARD_SIG_MISO},
    {"touch CS", 33, BOARD_PIN_OUT, 0, 0},
    {"touch IRQ", 36, BOARD_PIN_IN, 0, 0},
    {"backlight", 27, BOARD_PIN_OUT, 0, 0},
};
static const board_terminal_t TERMINALS[] = {{35, true}, {25, false}};

static board_info_t good(void)
{
    board_info_t b;
    memset(&b, 0, sizeof(b));
    b.id = "test_board";
    b.name = "Test board";
    b.chip = BOARD_CHIP_ESP32;
    b.width = 480;
    b.height = 320;
    b.pins = PINS;
    b.pin_count = sizeof(PINS) / sizeof(PINS[0]);
    b.terminals = TERMINALS;
    b.terminal_count = 2;
    b.terminals_where = "header";
    b.sd = (board_sd_t){.bus = BOARD_SD_SPI, .spi_host = 3, .clk = BOARD_NO_PIN,
                        .cmd = BOARD_NO_PIN, .d0 = BOARD_NO_PIN,
                        .sck = 18, .mosi = 23, .miso = 19, .cs = 5};
    b.rf = (board_rf_t){.spi_host = 3, .sck = 18, .mosi = 23, .miso = 19, .cs = 21, .data = 39,
                        .where = "SPI header"};
    b.radio = (board_uart_t){.tx = BOARD_NO_PIN, .rx = BOARD_NO_PIN};
    b.i2c = (board_i2c_t){.sda = BOARD_NO_PIN, .scl = BOARD_NO_PIN, .where = NULL};
    return b;
}

static size_t validate(const board_info_t *b, board_issue_t *issues, size_t max)
{
    return board_info_validate(b, issues, max);
}

static void assert_one_issue(const board_info_t *b, board_issue_code_t code, int16_t gpio)
{
    board_issue_t issues[4];
    const size_t n = validate(b, issues, 4);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT(code, issues[0].code);
    TEST_ASSERT_EQUAL_INT(gpio, issues[0].gpio);
    TEST_ASSERT_NOT_NULL(issues[0].use);
}

/* ---- chip facts ------------------------------------------------------------------------ */

/* BRD-001: the pins each chip has, and which can drive, pull up, or strap. */
static void test_esp32_pin_facts(void)
{
    TEST_ASSERT_TRUE(board_chip_has_pin(BOARD_CHIP_ESP32, 0));
    TEST_ASSERT_TRUE(board_chip_has_pin(BOARD_CHIP_ESP32, 39));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32, 24));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32, 28));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32, 31));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32, 40));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32, -1));
    for (int16_t g = 34; g <= 39; g++) {
        TEST_ASSERT_FALSE(board_chip_pin_can_output(BOARD_CHIP_ESP32, g));
        TEST_ASSERT_FALSE(board_chip_pin_has_pullup(BOARD_CHIP_ESP32, g));
    }
    TEST_ASSERT_TRUE(board_chip_pin_can_output(BOARD_CHIP_ESP32, 33));
    TEST_ASSERT_TRUE(board_chip_pin_has_pullup(BOARD_CHIP_ESP32, 25));
    const int16_t straps[] = {0, 2, 5, 12, 15};
    for (size_t i = 0; i < sizeof(straps) / sizeof(straps[0]); i++) {
        TEST_ASSERT_TRUE(board_chip_pin_is_strapping(BOARD_CHIP_ESP32, straps[i]));
    }
    TEST_ASSERT_FALSE(board_chip_pin_is_strapping(BOARD_CHIP_ESP32, 4));
}

/* BRD-002: flash always, PSRAM only on modules that carry it. */
static void test_reserved_pins(void)
{
    for (int16_t g = 6; g <= 11; g++) {
        TEST_ASSERT_TRUE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32, false, false, g));
    }
    TEST_ASSERT_FALSE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32, false, false, 16));
    TEST_ASSERT_TRUE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32, true, false, 16));
    TEST_ASSERT_TRUE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32, true, false, 17));
    TEST_ASSERT_TRUE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32S3, false, false, 26));
    TEST_ASSERT_TRUE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32S3, false, false, 32));
    TEST_ASSERT_FALSE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32S3, true, false, 35));
    TEST_ASSERT_TRUE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32S3, true, true, 35));
    for (int16_t g = 0; g <= 54; g++) {
        TEST_ASSERT_FALSE(board_chip_pin_is_reserved(BOARD_CHIP_ESP32P4, true, false, g));
    }
}

static void test_s3_and_p4_pin_facts(void)
{
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32S3, 22));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32S3, 25));
    TEST_ASSERT_TRUE(board_chip_has_pin(BOARD_CHIP_ESP32S3, 48));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32S3, 49));
    TEST_ASSERT_TRUE(board_chip_pin_is_strapping(BOARD_CHIP_ESP32S3, 46));
    TEST_ASSERT_TRUE(board_chip_has_pin(BOARD_CHIP_ESP32P4, 54));
    TEST_ASSERT_FALSE(board_chip_has_pin(BOARD_CHIP_ESP32P4, 55));
    for (int16_t g = 34; g <= 38; g++) {
        TEST_ASSERT_TRUE(board_chip_pin_is_strapping(BOARD_CHIP_ESP32P4, g));
    }
    TEST_ASSERT_FALSE(board_chip_pin_is_strapping(BOARD_CHIP_ESP32P4, 10));
    TEST_ASSERT_FALSE(board_chip_has_pin((board_chip_t)99, 1));
    TEST_ASSERT_EQUAL_STRING("ESP32-P4", board_chip_name(BOARD_CHIP_ESP32P4));
    TEST_ASSERT_EQUAL_STRING("unknown chip", board_chip_name((board_chip_t)99));
}

/* ---- validation ------------------------------------------------------------------------ */

/* BRD-001, BRD-006: shared bus lines (LCD + touch, SD + RF) are fine; a safe board passes. */
static void test_a_safe_board_passes(void)
{
    const board_info_t b = good();
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
}

/* BRD-002: the 7" panel's 433 MHz socket pins (6-10) are the classic ESP32's flash pins. Copying
 * that wiring to an ESP32 board must be refused, pin by pin. */
static void test_crowpanel_rf_pins_on_an_esp32_are_refused(void)
{
    board_info_t b = good();
    b.rf = (board_rf_t){.spi_host = 2, .mosi = 6, .miso = 7, .sck = 8, .cs = 9, .data = 10};
    board_issue_t issues[8];
    const size_t n = validate(&b, issues, 8);
    TEST_ASSERT_EQUAL_UINT(5, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_RESERVED, issues[i].code);
    }
}

/* BRD-001: a pin the chip doesn't have. */
static void test_a_missing_gpio_is_refused(void)
{
    board_info_t b = good();
    b.rf.cs = 24;
    assert_one_issue(&b, BOARD_ISSUE_NO_SUCH_PIN, 24);
    b.rf.cs = 40;
    assert_one_issue(&b, BOARD_ISSUE_NO_SUCH_PIN, 40);
}

/* BRD-002: PSRAM pins count only on a module with PSRAM. */
static void test_psram_pins_on_a_wrover(void)
{
    static const board_pin_t led[] = {{"LED green", 16, BOARD_PIN_OUT, 0, 0}};
    board_info_t b = good();
    b.pins = led;
    b.pin_count = 1;
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    b.psram = true;
    assert_one_issue(&b, BOARD_ISSUE_RESERVED, 16);
}

/* BRD-003: an output on an input-only pin. */
static void test_an_output_on_an_input_only_pin_is_refused(void)
{
    board_info_t b = good();
    b.rf.cs = 34;
    assert_one_issue(&b, BOARD_ISSUE_INPUT_ONLY, 34);
}

/* BRD-004: a contact needs a pull-up; GPIO34-39 have none, so one must be fitted. */
static void test_a_terminal_without_a_pullup_is_refused(void)
{
    static const board_terminal_t bare[] = {{35, false}};
    board_info_t b = good();
    b.terminals = bare;
    b.terminal_count = 1;
    assert_one_issue(&b, BOARD_ISSUE_NO_PULLUP, 35);
    static const board_terminal_t fitted[] = {{35, true}};
    b.terminals = fitted;
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
}

/* BRD-005: a door contact or a receiver's data line could hold a strapping pin at reset and
 * stop the board from booting; the board's own parts may use strapping pins (LCD DC on IO2). */
static void test_outside_signals_on_strapping_pins_are_refused(void)
{
    static const board_terminal_t on_boot_pin[] = {{0, false}};
    board_info_t b = good();
    b.terminals = on_boot_pin;
    b.terminal_count = 1;
    assert_one_issue(&b, BOARD_ISSUE_STRAPPING, 0);

    b = good();
    b.rf.data = 4;  /* fine */
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    b.rf.data = 2;  /* the LCD's DC: in use, and a strapping pin */
    board_issue_t issues[4];
    TEST_ASSERT_EQUAL_UINT(2, validate(&b, issues, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_STRAPPING, issues[0].code);
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issues[1].code);
}

/* BRD-006: two uses of one pin, naming both; chip selects are never shared. */
static void test_a_pin_used_twice_is_refused(void)
{
    board_info_t b = good();
    b.rf.cs = 5; /* the SD card's CS */
    board_issue_t issues[4];
    TEST_ASSERT_EQUAL_UINT(1, validate(&b, issues, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issues[0].code);
    TEST_ASSERT_EQUAL_STRING("RF CS", issues[0].use);
    TEST_ASSERT_EQUAL_STRING("SD CS", issues[0].other);

    b = good();
    b.rf.data = 25; /* a wired zone terminal */
    assert_one_issue(&b, BOARD_ISSUE_IN_USE, 25);
}

/* BRD-006: a bus line is shared only when both uses say so. */
static void test_a_bus_line_and_a_plain_pin_do_not_share(void)
{
    board_info_t b = good();
    b.rf.cs = 14; /* the display bus's SCK */
    assert_one_issue(&b, BOARD_ISSUE_IN_USE, 14);
}

/* BRD-001: SDMMC wiring is checked like any other. */
static void test_sdmmc_pins(void)
{
    board_info_t b = good();
    b.sd = (board_sd_t){.bus = BOARD_SD_SDMMC_1BIT, .spi_host = -1, .clk = 14, .cmd = 26, .d0 = 4,
                        .sck = BOARD_NO_PIN, .mosi = BOARD_NO_PIN, .miso = BOARD_NO_PIN,
                        .cs = BOARD_NO_PIN};
    assert_one_issue(&b, BOARD_ISSUE_IN_USE, 14); /* SD CLK on the display's SCK */
}

/* BRD-006 (docs/features/35): the add-on I2C connector is a bus - it may share its lines with the
 * board's own I2C parts, never with anything else. */
static void test_the_i2c_connector(void)
{
    static const board_pin_t WITH_I2C[] = {
        {"LCD SCK", 14, BOARD_PIN_OUT | BOARD_PIN_SHARED, SPI2, BOARD_SIG_SCK},
        {"I2C SDA", 26, BOARD_PIN_OUT | BOARD_PIN_IN | BOARD_PIN_SHARED, BOARD_BUS_I2C(0),
         BOARD_SIG_SDA},
        {"I2C SCL", 32, BOARD_PIN_OUT | BOARD_PIN_SHARED, BOARD_BUS_I2C(0), BOARD_SIG_SCL},
    };
    board_info_t b = good();
    b.pins = WITH_I2C;
    b.pin_count = sizeof(WITH_I2C) / sizeof(WITH_I2C[0]);
    b.i2c = (board_i2c_t){.sda = 26, .scl = 32, .where = "I2C connector"};
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    b.i2c.scl = 16; /* a line of its own is fine too */
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    /* BRD-020: the board's I2C0 lines are not the connector's once it has a controller of its own */
    b.i2c = (board_i2c_t){.sda = 26, .scl = 32, .where = "I2C connector", .port = 1};
    board_issue_t two[4];
    TEST_ASSERT_EQUAL_UINT(2, validate(&b, two, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_OTHER_BUS, two[0].code);
    TEST_ASSERT_EQUAL_INT(26, two[0].gpio);
    /* ... and SDA is never the bus's SCL */
    b.i2c = (board_i2c_t){.sda = 32, .scl = 26, .where = "I2C connector"};
    TEST_ASSERT_EQUAL_UINT(2, validate(&b, two, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_OTHER_SIGNAL, two[0].code);
    b = good();
    b.i2c = (board_i2c_t){.sda = 33, .scl = BOARD_NO_PIN, .where = "I2C connector"};
    assert_one_issue(&b, BOARD_ISSUE_IN_USE, 33); /* the touch CS */
    b.i2c.sda = 40;
    assert_one_issue(&b, BOARD_ISSUE_NO_SUCH_PIN, 40);
}

/* BRD-007: descriptions missing what the validator needs. */
static void test_incomplete_descriptions_are_refused(void)
{
    TEST_ASSERT_EQUAL_UINT(1, validate(NULL, NULL, 0));
    board_info_t b = good();
    b.id = NULL;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    b = good();
    b.chip = (board_chip_t)0;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    b = good();
    b.pins = NULL;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    b = good();
    b.sd.bus = (board_sd_bus_t)7;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    /* An audio rate the sounder can't play (AUD-006): too low, too high, or a 10 ms block that
     * isn't a whole number of frames. 0 means the default. */
    const uint32_t bad_rates[] = {4000u, 44100u, 11025u};
    for (size_t i = 0; i < sizeof(bad_rates) / sizeof(bad_rates[0]); i++) {
        b = good();
        b.audio_rate_hz = bad_rates[i];
        assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    }
    b = good();
    b.audio_rate_hz = 16000u;
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
}

/* The count is the total even when the caller's array is shorter. */
static void test_issues_beyond_the_array_are_counted(void)
{
    board_info_t b = good();
    b.rf = (board_rf_t){.spi_host = 2, .mosi = 6, .miso = 7, .sck = 8, .cs = 9, .data = 10};
    board_issue_t issues[2];
    memset(issues, 0, sizeof(issues));
    TEST_ASSERT_EQUAL_UINT(5, validate(&b, issues, 2));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_RESERVED, issues[1].code);
}

/* BRD-013, RF-008: a receiver module's data line moves to a wired terminal; the terminal is no
 * longer offered for zones, and the result passes the validator. Only pins the board offers. */
static void test_receiver_data_moves_to_a_terminal(void)
{
    const board_info_t b = good();
    board_info_t moved;
    board_terminal_t terms[4];
    TEST_ASSERT_TRUE(board_rf_data_on(&b, 25, &moved, terms, 4));
    TEST_ASSERT_EQUAL_INT(25, moved.rf.data);
    TEST_ASSERT_EQUAL_UINT(1, moved.terminal_count);
    TEST_ASSERT_EQUAL_INT(35, moved.terminals[0].gpio);
    TEST_ASSERT_TRUE(moved.terminals[0].external_pullup);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));
    /* the description itself is untouched */
    TEST_ASSERT_EQUAL_INT(39, b.rf.data);
    TEST_ASSERT_EQUAL_UINT(2, b.terminal_count);

    /* the board's own data pin: nothing moves */
    TEST_ASSERT_TRUE(board_rf_data_on(&b, 39, &moved, terms, 4));
    TEST_ASSERT_EQUAL_UINT(2, moved.terminal_count);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));

    /* a pin the board does not offer outside, or no pin */
    TEST_ASSERT_FALSE(board_rf_data_on(&b, 27, &moved, terms, 4)); /* the backlight */
    TEST_ASSERT_FALSE(board_rf_data_on(&b, 22, &moved, terms, 4)); /* offered to nobody */
    TEST_ASSERT_FALSE(board_rf_data_on(&b, BOARD_NO_PIN, &moved, terms, 4));
    TEST_ASSERT_FALSE(board_rf_data_on(NULL, 25, &moved, terms, 4));
    TEST_ASSERT_FALSE(board_rf_data_on(&b, 25, &moved, terms, 0)); /* no room for the rest */

    /* the last terminal taken: an empty list the validator accepts */
    board_info_t one = good();
    one.terminal_count = 1;
    TEST_ASSERT_TRUE(board_rf_data_on(&one, 35, &moved, terms, 4));
    TEST_ASSERT_EQUAL_UINT(0, moved.terminal_count);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));
}

/* BRD-014, RF-019: the receiver moves to another place the board offers (`rf place`): every
 * terminal that place takes leaves the list, the others stay, and the result is validated like
 * any description. Only places the board names. */
static const board_rf_place_t RF_PLACES[] = {
    {"header", {.spi_host = 3, .sck = 18, .mosi = 23, .miso = 19, .cs = 22, .data = 25,
                .where = "extension header"}},
    {"clash", {.spi_host = 3, .sck = 18, .mosi = 23, .miso = 19, .cs = 27, .data = 26,
               .where = "on the backlight"}},
};

static void test_receiver_moves_to_another_place(void)
{
    board_info_t b = good();
    b.rf_places = RF_PLACES;
    b.rf_place_count = 2;
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    TEST_ASSERT_EQUAL_PTR(&RF_PLACES[0], board_rf_place(&b, "header"));
    TEST_ASSERT_NULL(board_rf_place(&b, "socket")); /* the board's own place is not a place */

    board_info_t moved;
    board_terminal_t terms[4];
    TEST_ASSERT_TRUE(board_rf_place_on(&b, "header", &moved, terms, 4));
    TEST_ASSERT_EQUAL_INT(22, moved.rf.cs);
    TEST_ASSERT_EQUAL_INT(25, moved.rf.data);
    TEST_ASSERT_EQUAL_STRING("extension header", moved.rf.where);
    TEST_ASSERT_EQUAL_UINT(1, moved.terminal_count); /* 25 is the data line now */
    TEST_ASSERT_EQUAL_INT(35, moved.terminals[0].gpio);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));
    TEST_ASSERT_EQUAL_INT(39, b.rf.data); /* the description itself is untouched */
    TEST_ASSERT_EQUAL_UINT(2, b.terminal_count);

    /* a place whose chip select is the backlight: moving is data, the validator refuses it */
    TEST_ASSERT_TRUE(board_rf_place_on(&b, "clash", &moved, terms, 4));
    board_issue_t issue;
    TEST_ASSERT_EQUAL_UINT(1, validate(&moved, &issue, 1));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issue.code);
    TEST_ASSERT_EQUAL_INT(27, issue.gpio);

    TEST_ASSERT_FALSE(board_rf_place_on(&b, "nowhere", &moved, terms, 4));
    TEST_ASSERT_FALSE(board_rf_place_on(&b, NULL, &moved, terms, 4));
    TEST_ASSERT_FALSE(board_rf_place_on(NULL, "header", &moved, terms, 4));
    TEST_ASSERT_FALSE(board_rf_place_on(&b, "header", &moved, terms, 0)); /* 35 has no room */
    const board_info_t plain = good();
    TEST_ASSERT_FALSE(board_rf_place_on(&plain, "header", &moved, terms, 4));
}

/* BRD-015, RTC-013: the add-on I2C modules move to another place with a controller of their
 * own (`rtc place`); a terminal the place takes leaves the list. */
static const board_i2c_place_t I2C_PLACES[] = {
    {"ext", 1, 32, 26, "extension header, 32 and 26"},
    {"zones", 1, 25, 26, "a zone terminal"},
};

static void test_i2c_modules_move_to_another_place(void)
{
    board_info_t b = good();
    b.i2c_places = I2C_PLACES;
    b.i2c_place_count = 2;
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    TEST_ASSERT_EQUAL_INT(1, board_i2c_place(&b, "ext")->port);
    TEST_ASSERT_NULL(board_i2c_place(&b, "j13"));

    board_info_t moved;
    board_terminal_t terms[4];
    TEST_ASSERT_TRUE(board_i2c_place_on(&b, "ext", &moved, terms, 4));
    TEST_ASSERT_EQUAL_INT(32, moved.i2c.sda);
    TEST_ASSERT_EQUAL_INT(26, moved.i2c.scl);
    TEST_ASSERT_EQUAL_STRING("extension header, 32 and 26", moved.i2c.where);
    TEST_ASSERT_EQUAL_UINT(2, moved.terminal_count);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));

    TEST_ASSERT_TRUE(board_i2c_place_on(&b, "zones", &moved, terms, 4));
    TEST_ASSERT_EQUAL_UINT(1, moved.terminal_count); /* 25 carries SDA now */
    TEST_ASSERT_EQUAL_INT(35, moved.terminals[0].gpio);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));

    TEST_ASSERT_FALSE(board_i2c_place_on(&b, "nowhere", &moved, terms, 4));
    TEST_ASSERT_FALSE(board_i2c_place_on(&b, NULL, &moved, terms, 4));
    TEST_ASSERT_FALSE(board_i2c_place_on(NULL, "ext", &moved, terms, 4));
}

/* BRD-014, BRD-015: a description whose places can't be read is refused. */
static void test_unreadable_places_are_refused(void)
{
    static const board_rf_place_t no_id[] = {{"", {.spi_host = 3, .sck = 18, .mosi = 23,
                                                   .miso = 19, .cs = 22, .data = 25}}};
    static const board_i2c_place_t no_port[] = {{"ext", -1, 32, 26, "x"}};
    board_info_t b = good();
    b.rf_place_count = 1; /* no array */
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    b = good();
    b.rf_places = no_id;
    b.rf_place_count = 1;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    b = good();
    b.i2c_places = no_port;
    b.i2c_place_count = 1;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
}

/* BRD-016 (docs/features/36 hardware.md §4): the Z-Wave radio moves to a place the board offers
 * (`zwave place`). A description leaves it off; at a place its UART's pins are checked like any
 * other and a terminal it takes leaves the list; a place on the sensor-radio socket's UART takes
 * the socket; a place another place in use holds (the clock module's) is refused. */
static const board_zwave_place_t ZW_PLACES[] = {
    {"hdr", 17, 16, "extension header"},
    {"term", 32, 25, "a wired terminal"},
    {"clash", 27, 16, "on the backlight"},
    {"strap", 4, 0, "RX on a strapping pin"},
    {"i2c", 32, 26, "the clock module's pins"},
};

static board_info_t with_zwave_places(void)
{
    board_info_t b = good();
    b.zwave_places = ZW_PLACES;
    b.zwave_place_count = sizeof(ZW_PLACES) / sizeof(ZW_PLACES[0]);
    return b;
}

static void test_zwave_radio_at_a_place(void)
{
    const board_info_t b = with_zwave_places();
    TEST_ASSERT_FALSE(b.zwave.wired); /* off: nothing of it is checked */
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    TEST_ASSERT_EQUAL_PTR(&ZW_PLACES[0], board_zwave_place(&b, "hdr"));
    TEST_ASSERT_NULL(board_zwave_place(&b, "socket"));
    TEST_ASSERT_NULL(board_zwave_place(&b, NULL));

    board_info_t moved;
    board_terminal_t terms[4];
    TEST_ASSERT_TRUE(board_zwave_place_on(&b, "hdr", &moved, terms, 4));
    TEST_ASSERT_TRUE(moved.zwave.wired);
    TEST_ASSERT_EQUAL_STRING("hdr", moved.zwave.place);
    TEST_ASSERT_EQUAL_INT(17, moved.zwave.tx);
    TEST_ASSERT_EQUAL_INT(16, moved.zwave.rx);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, moved.zwave.reset);
    TEST_ASSERT_EQUAL_STRING("extension header", moved.zwave.where);
    TEST_ASSERT_EQUAL_UINT(2, moved.terminal_count);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));
    TEST_ASSERT_FALSE(b.zwave.wired); /* the description itself is untouched */

    /* its RX on a wired terminal: the terminal is the radio's now */
    TEST_ASSERT_TRUE(board_zwave_place_on(&b, "term", &moved, terms, 4));
    TEST_ASSERT_EQUAL_UINT(1, moved.terminal_count);
    TEST_ASSERT_EQUAL_INT(35, moved.terminals[0].gpio);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));

    /* its TX on the backlight, its RX on a strapping pin */
    TEST_ASSERT_TRUE(board_zwave_place_on(&b, "clash", &moved, terms, 4));
    assert_one_issue(&moved, BOARD_ISSUE_IN_USE, 27);
    TEST_ASSERT_TRUE(board_zwave_place_on(&b, "strap", &moved, terms, 4));
    assert_one_issue(&moved, BOARD_ISSUE_STRAPPING, 0);

    /* the clock module's place and the radio's on the same pins: refused */
    board_info_t clock;
    board_info_t both;
    board_terminal_t clock_terms[4];
    board_info_t with_clock = b;
    with_clock.i2c_places = I2C_PLACES;
    with_clock.i2c_place_count = 2;
    TEST_ASSERT_TRUE(board_i2c_place_on(&with_clock, "ext", &clock, clock_terms, 4));
    TEST_ASSERT_EQUAL_UINT(0, validate(&clock, NULL, 0));
    TEST_ASSERT_TRUE(board_zwave_place_on(&clock, "i2c", &both, terms, 4));
    board_issue_t issues[4];
    TEST_ASSERT_EQUAL_UINT(2, validate(&both, issues, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issues[0].code);
    TEST_ASSERT_EQUAL_STRING("Z-Wave radio TX", issues[0].use);
    TEST_ASSERT_EQUAL_STRING("I2C connector SDA", issues[0].other);
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issues[1].code);

    /* the sensor-radio socket's UART: the module takes the socket */
    board_info_t socket = b;
    socket.radio = (board_uart_t){.tx = 17, .rx = 16, .where = "socket"};
    TEST_ASSERT_EQUAL_UINT(0, validate(&socket, NULL, 0));
    TEST_ASSERT_TRUE(board_zwave_place_on(&socket, "hdr", &moved, terms, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, moved.radio.tx);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, moved.radio.rx);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));
    TEST_ASSERT_EQUAL_INT(17, socket.radio.tx);

    TEST_ASSERT_FALSE(board_zwave_place_on(&b, "nowhere", &moved, terms, 4));
    TEST_ASSERT_FALSE(board_zwave_place_on(NULL, "hdr", &moved, terms, 4));
    TEST_ASSERT_FALSE(board_zwave_place_on(&b, "hdr", NULL, terms, 4));
    TEST_ASSERT_FALSE(board_zwave_place_on(&b, "hdr", &moved, terms, 0)); /* no room for 35, 25 */
    const board_info_t plain = good();
    TEST_ASSERT_FALSE(board_zwave_place_on(&plain, "hdr", &moved, terms, 4));

    /* unreadable places, a radio in use without both pins */
    static const board_zwave_place_t no_id[] = {{"", 17, 16, "x"}};
    board_info_t bad = good();
    bad.zwave_place_count = 1; /* no array */
    assert_one_issue(&bad, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    bad.zwave_places = no_id;
    assert_one_issue(&bad, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    bad = good();
    bad.zwave = (board_zwave_t){.wired = true, .tx = 17, .rx = BOARD_NO_PIN, .reset = BOARD_NO_PIN};
    assert_one_issue(&bad, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
}

/* BRD-017 (hardware.md §5): the module's reset line on one of the board's wired terminals, which
 * then is no zone's; only with the radio in use, only a terminal; an output the module pulls up,
 * so never an input-only pin. */
static void test_zwave_reset_line(void)
{
    const board_info_t b = with_zwave_places();
    board_info_t wired;
    board_info_t moved;
    board_terminal_t terms[4];
    board_terminal_t reset_terms[4];
    TEST_ASSERT_FALSE(board_zwave_reset_on(&b, 25, &moved, terms, 4)); /* the radio is off */
    TEST_ASSERT_TRUE(board_zwave_place_on(&b, "hdr", &wired, terms, 4));
    TEST_ASSERT_TRUE(board_zwave_reset_on(&wired, 25, &moved, reset_terms, 4));
    TEST_ASSERT_EQUAL_INT(25, moved.zwave.reset);
    TEST_ASSERT_EQUAL_INT(17, moved.zwave.tx);
    TEST_ASSERT_EQUAL_UINT(1, moved.terminal_count);
    TEST_ASSERT_EQUAL_INT(35, moved.terminals[0].gpio);
    TEST_ASSERT_EQUAL_UINT(0, validate(&moved, NULL, 0));
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, wired.zwave.reset); /* the board it came from: unchanged */

    /* GPIO35 is a terminal (with a pull-up fitted) but can't drive the line */
    TEST_ASSERT_TRUE(board_zwave_reset_on(&wired, 35, &moved, reset_terms, 4));
    assert_one_issue(&moved, BOARD_ISSUE_INPUT_ONLY, 35);

    TEST_ASSERT_FALSE(board_zwave_reset_on(&wired, 22, &moved, reset_terms, 4)); /* no terminal */
    TEST_ASSERT_FALSE(board_zwave_reset_on(&wired, BOARD_NO_PIN, &moved, reset_terms, 4));
    TEST_ASSERT_FALSE(board_zwave_reset_on(&wired, 25, &moved, reset_terms, 0));
    TEST_ASSERT_FALSE(board_zwave_reset_on(NULL, 25, &moved, reset_terms, 4));
}

static void test_every_issue_has_words(void)
{
    for (int code = BOARD_ISSUE_NONE; code <= BOARD_ISSUE_OTHER_SIGNAL; code++) {
        const char *text = board_issue_text((board_issue_code_t)code);
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_TRUE(strlen(text) > 3);
    }
    TEST_ASSERT_EQUAL_STRING("unknown issue", board_issue_text((board_issue_code_t)99));
    /* BRD-020: the codes are appended, never renumbered (a log or a message may carry them) */
    TEST_ASSERT_EQUAL_INT(7, BOARD_ISSUE_BAD_DESCRIPTION);
    TEST_ASSERT_EQUAL_INT(8, BOARD_ISSUE_OTHER_BUS);
    TEST_ASSERT_EQUAL_INT(9, BOARD_ISSUE_OTHER_SIGNAL);
}

/* ---- sharing that knows each bus (BRD-020) ----------------------------------------------- */

/* BRD-020: the display and touch share SPI2 line for line, the SD card and the receiver SPI3. */
static void test_a_bus_is_shared_line_for_line(void)
{
    board_info_t b = good();
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    b.rf.mosi = 18; /* the SD card's SCK */
    board_issue_t issues[4];
    TEST_ASSERT_EQUAL_UINT(1, validate(&b, issues, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_OTHER_SIGNAL, issues[0].code);
    TEST_ASSERT_EQUAL_INT(18, issues[0].gpio);
    TEST_ASSERT_EQUAL_STRING("RF MOSI", issues[0].use);
    TEST_ASSERT_EQUAL_STRING("SD SCK", issues[0].other);

    /* each signal of a part gets its own pin: SCK and MOSI on one pin */
    b = good();
    b.rf.mosi = b.rf.sck;
    TEST_ASSERT_EQUAL_UINT(1, validate(&b, issues, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_OTHER_SIGNAL, issues[0].code);

    /* the same line, but the receiver on another SPI host */
    b = good();
    b.rf.spi_host = 2;
    TEST_ASSERT_EQUAL_UINT(3, validate(&b, issues, 4));
    TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_OTHER_BUS, issues[0].code);
    TEST_ASSERT_EQUAL_INT(18, issues[0].gpio);

    /* a bus line on the display's SCK from another bus */
    b = good();
    b.rf.sck = 14;
    assert_one_issue(&b, BOARD_ISSUE_OTHER_BUS, 14);

    /* a shared line that names no bus shares with nothing */
    static const board_pin_t unnamed[] = {
        {"LCD SCK", 14, BOARD_PIN_OUT | BOARD_PIN_SHARED, 0, 0},
        {"touch SCK", 14, BOARD_PIN_OUT | BOARD_PIN_SHARED, 0, 0},
    };
    b = good();
    b.pins = unnamed;
    b.pin_count = 2;
    assert_one_issue(&b, BOARD_ISSUE_OTHER_BUS, 14);
}

/* ---- the pins a person can reach (BRD-018) and custom places (BRD-021) -------------------- */

static const board_reach_t TEST_REACH[] = {
    {0, "IO0", "P3"},  {4, "IO4", "P3"},   {16, "IO16", "P3"}, {17, "IO17", "P3"},
    {22, "IO22", "P3"}, {25, "IO25", "P3"}, {26, "IO26", "P3"}, {32, "IO32", "P3"},
    {27, "IO27", "P3"}, /* the backlight: reachable, but the board's own */
};
static const board_i2c_place_t TEST_I2C_PLACES[] = {{"p3", 1, 26, 32, "P3 header"}};

static board_info_t reachable(void)
{
    board_info_t b = good();
    b.reach = TEST_REACH;
    b.reach_count = sizeof(TEST_REACH) / sizeof(TEST_REACH[0]);
    b.i2c_places = TEST_I2C_PLACES;
    b.i2c_place_count = 1;
    return b;
}

/* BRD-018: a description's reachable pins are real pins with their printed names. */
static void test_reachable_pins_are_described(void)
{
    board_info_t b = reachable();
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
    TEST_ASSERT_NOT_NULL(board_reach(&b, 16));
    TEST_ASSERT_EQUAL_STRING("IO16", board_reach(&b, 16)->label);
    TEST_ASSERT_NULL(board_reach(&b, 5));
    TEST_ASSERT_NULL(board_reach(&b, BOARD_NO_PIN));
    TEST_ASSERT_NULL(board_reach(NULL, 16));
    b.reach = NULL;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    static const board_reach_t no_such_pin[] = {{24, "IO24", "P3"}};
    b.reach = no_such_pin;
    b.reach_count = 1;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    static const board_reach_t unnamed[] = {{16, NULL, "P3"}};
    b.reach = unnamed;
    assert_one_issue(&b, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN);
    b = good(); /* none at all is fine: the E32R40T */
    TEST_ASSERT_EQUAL_UINT(0, b.reach_count);
    TEST_ASSERT_EQUAL_UINT(0, validate(&b, NULL, 0));
}

static board_rf_t rf_pins(int16_t sck, int16_t mosi, int16_t miso, int16_t cs, int16_t data)
{
    return (board_rf_t){.spi_host = -1, .sck = sck, .mosi = mosi, .miso = miso, .cs = cs,
                        .data = data, .where = NULL};
}

/* BRD-021: the receiver on pins the person chose: a CC1101 or a receiver module. */
static void test_rf_on_custom_pins(void)
{
    const board_info_t b = reachable();
    board_info_t out;
    memset(&out, 0, sizeof(out));
    board_terminal_t terms[4];
    char why[96];
    board_rf_t cc = rf_pins(16, 17, 22, 4, 26);
    TEST_ASSERT_TRUE(board_rf_custom_on(&b, &cc, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(16, out.rf.sck);
    TEST_ASSERT_EQUAL_INT(26, out.rf.data);
    TEST_ASSERT_EQUAL_INT(3, out.rf.spi_host); /* the board's receiver host */
    TEST_ASSERT_EQUAL_STRING("your own pins", out.rf.where);
    TEST_ASSERT_EQUAL_UINT(0, validate(&out, NULL, 0));

    board_rf_t module = rf_pins(BOARD_NO_PIN, BOARD_NO_PIN, BOARD_NO_PIN, BOARD_NO_PIN, 25);
    TEST_ASSERT_TRUE(board_rf_custom_on(&b, &module, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(25, out.rf.data);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, out.rf.sck);
    /* a zone's terminal may be taken: it is no longer offered (the zone shows Check later) */
    TEST_ASSERT_EQUAL_UINT(1, out.terminal_count);
    TEST_ASSERT_EQUAL_INT(35, out.terminals[0].gpio);
    TEST_ASSERT_EQUAL_UINT(0, validate(&out, NULL, 0));

    /* refused, with the GPIO and the signal, and *out untouched */
    board_info_t before = out;
    cc = rf_pins(16, 17, 22, 17, 26);
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &cc, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 17 is used twice (MOSI and CS)", why);
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof(out));
    cc = rf_pins(16, 17, 22, 5, 26);
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &cc, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 5 is not a pin you can reach on this board (P3's IO pins only)",
                             why);
    cc = rf_pins(16, 17, 22, 27, 26);
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &cc, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 27 (CS) is already in use: backlight", why);
    cc = rf_pins(16, BOARD_NO_PIN, 22, 4, 26); /* half a CC1101 */
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &cc, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "CC1101"));
    cc = rf_pins(16, 17, 22, 4, BOARD_NO_PIN);
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &cc, &out, terms, 4, why, sizeof(why)));
    /* the validator still has the last word: GPIO0 is a strapping pin, the data line outside */
    module.data = 0;
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &module, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "GPIO 0"));
    TEST_ASSERT_NOT_NULL(strstr(why, "strapping"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof(out));
    /* a NULL why, no board, too few terminal slots */
    module.data = 25;
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, &module, &out, terms, 0, NULL, 0));
    TEST_ASSERT_FALSE(board_rf_custom_on(NULL, &module, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_FALSE(board_rf_custom_on(&b, NULL, &out, terms, 4, why, sizeof(why)));
    const board_info_t none = good(); /* nothing to reach */
    TEST_ASSERT_FALSE(board_rf_custom_on(&none, &module, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("this board has no pins for an add-on of your own", why);
}

/* BRD-021: the I2C modules on two pins with a controller of their own; not on another part. */
static void test_i2c_on_custom_pins(void)
{
    board_info_t b = reachable();
    board_info_t out;
    board_terminal_t terms[4];
    char why[96];
    TEST_ASSERT_TRUE(board_i2c_custom_on(&b, 16, 17, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(16, out.i2c.sda);
    TEST_ASSERT_EQUAL_INT(17, out.i2c.scl);
    TEST_ASSERT_EQUAL_INT(1, out.i2c.port);
    TEST_ASSERT_EQUAL_UINT(2, out.terminal_count);
    TEST_ASSERT_FALSE(board_i2c_custom_on(&b, 16, 16, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 16 is used twice (SDA and SCL)", why);
    TEST_ASSERT_FALSE(board_i2c_custom_on(&b, 16, BOARD_NO_PIN, &out, terms, 4, why, sizeof(why)));
    /* the receiver already holds 4 once moved there */
    board_info_t with_rf;
    board_rf_t cc = rf_pins(16, 17, 22, 4, 26);
    TEST_ASSERT_TRUE(board_rf_custom_on(&b, &cc, &with_rf, terms, 4, why, sizeof(why)));
    board_terminal_t terms2[4];
    TEST_ASSERT_FALSE(board_i2c_custom_on(&with_rf, 25, 4, &out, terms2, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 4 (SCL) is already in use: the 433 MHz receiver", why);
    TEST_ASSERT_TRUE(board_i2c_custom_on(&with_rf, 25, 32, &out, terms2, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(16, out.rf.sck); /* the receiver stays where it is */
    /* a board without an I2C place has no controller to give */
    b.i2c_places = NULL;
    b.i2c_place_count = 0;
    TEST_ASSERT_FALSE(board_i2c_custom_on(&b, 16, 17, &out, terms, 4, why, sizeof(why)));
}

/* BRD-021: the Z-Wave radio on pins the person chose, with or without its reset line. */
static void test_zwave_on_custom_pins(void)
{
    const board_info_t b = reachable();
    board_info_t out, i2c;
    board_terminal_t terms[4], t2[4];
    char why[96];
    TEST_ASSERT_TRUE(board_zwave_custom_on(&b, 16, 17, BOARD_NO_PIN, &out, terms, 4, why,
                                           sizeof(why)));
    TEST_ASSERT_TRUE(out.zwave.wired);
    TEST_ASSERT_EQUAL_STRING("custom", out.zwave.place);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, out.zwave.reset);
    TEST_ASSERT_TRUE(board_zwave_custom_on(&b, 16, 17, 25, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(25, out.zwave.reset);
    TEST_ASSERT_EQUAL_UINT(1, out.terminal_count);
    TEST_ASSERT_FALSE(board_zwave_custom_on(&b, 16, 17, 16, &out, terms, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 16 is used twice (TX and reset)", why);
    TEST_ASSERT_FALSE(board_zwave_custom_on(&b, BOARD_NO_PIN, 17, 25, &out, terms, 4, why,
                                            sizeof(why)));
    TEST_ASSERT_TRUE(board_i2c_custom_on(&b, 16, 17, &i2c, t2, 4, why, sizeof(why)));
    TEST_ASSERT_FALSE(board_zwave_custom_on(&i2c, 22, 17, BOARD_NO_PIN, &out, terms, 4, why,
                                            sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 17 (RX) is already in use: the I2C modules", why);
    /* a radio already wired is the part being moved: its own pins are free to reuse */
    board_info_t wired;
    TEST_ASSERT_TRUE(board_zwave_custom_on(&b, 16, 17, BOARD_NO_PIN, &wired, terms, 4, why,
                                           sizeof(why)));
    TEST_ASSERT_TRUE(board_zwave_custom_on(&wired, 17, 16, BOARD_NO_PIN, &out, t2, 4, why,
                                           sizeof(why)));
}

/* ---- the console's key=value arguments (BRD-022) ------------------------------------------ */

static bool parse(const char *line, int16_t *out, char *why, size_t len)
{
    char buf[128];
    char *argv[8];
    int argc = 0;
    strncpy(buf, line, sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = '\0';
    for (char *t = strtok(buf, " "); t != NULL && argc < 8; t = strtok(NULL, " ")) {
        argv[argc++] = t;
    }
    /* data is required; the SPI lines are optional (a receiver module has none) */
    static const char *const order[] = {"data", "sck", "mosi", "miso", "cs"};
    return board_custom_parse(argc, argv, order, 5, 1, out, why, len);
}

/* BRD-022: key=value in any order, required and optional keys, decimal GPIO numbers 0-63. */
static void test_custom_arguments_are_parsed(void)
{
    int16_t v[5];
    char why[96];
    TEST_ASSERT_TRUE(parse("sck=52 mosi=51 miso=50 cs=49 data=5", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(5, v[0]);
    TEST_ASSERT_EQUAL_INT(52, v[1]);
    TEST_ASSERT_EQUAL_INT(49, v[4]);
    TEST_ASSERT_TRUE(parse("data=0", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(0, v[0]);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, v[1]);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, v[4]);
    TEST_ASSERT_TRUE(parse("data=63", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(63, v[0]);

    TEST_ASSERT_FALSE(parse("sck=52", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("data= is missing", why);
    TEST_ASSERT_FALSE(parse("data=5 cs=x", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("cs=x is not a GPIO number", why);
    TEST_ASSERT_FALSE(parse("data=64", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("data=64 is not a GPIO number", why);
    TEST_ASSERT_FALSE(parse("data=-1", v, why, sizeof(why)));
    TEST_ASSERT_FALSE(parse("data=0x5", v, why, sizeof(why)));
    TEST_ASSERT_FALSE(parse("data=", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("data= is not a GPIO number", why);
    TEST_ASSERT_FALSE(parse("data=99999999999999999999", v, why, sizeof(why)));
    TEST_ASSERT_FALSE(parse("data=5 data=6", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("data= is given twice", why);
    TEST_ASSERT_FALSE(parse("data=5 gdo0=6", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("gdo0= is not one of: data= sck= mosi= miso= cs=", why);
    TEST_ASSERT_FALSE(parse("data=5 52", v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("52 is not key=value", why);
    TEST_ASSERT_FALSE(parse("=5", v, why, sizeof(why)));
    TEST_ASSERT_FALSE(parse("DATA=5", v, why, sizeof(why))); /* keys as written */
    /* a short why is cut, never overrun; a NULL why is fine */
    char tiny[6];
    TEST_ASSERT_FALSE(parse("sck=1", v, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("data=", tiny);
    TEST_ASSERT_FALSE(parse("sck=1", v, NULL, 0));
    /* nothing required and nothing given */
    static const char *const optional[] = {"reset"};
    TEST_ASSERT_TRUE(board_custom_parse(0, NULL, optional, 1, 0, v, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, v[0]);
    TEST_ASSERT_FALSE(board_custom_parse(0, NULL, optional, 1, 2, v, why, sizeof(why)));
    TEST_ASSERT_FALSE(board_custom_parse(1, NULL, optional, 1, 0, v, why, sizeof(why)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_esp32_pin_facts);
    RUN_TEST(test_reserved_pins);
    RUN_TEST(test_s3_and_p4_pin_facts);
    RUN_TEST(test_a_safe_board_passes);
    RUN_TEST(test_crowpanel_rf_pins_on_an_esp32_are_refused);
    RUN_TEST(test_a_missing_gpio_is_refused);
    RUN_TEST(test_psram_pins_on_a_wrover);
    RUN_TEST(test_an_output_on_an_input_only_pin_is_refused);
    RUN_TEST(test_a_terminal_without_a_pullup_is_refused);
    RUN_TEST(test_outside_signals_on_strapping_pins_are_refused);
    RUN_TEST(test_a_pin_used_twice_is_refused);
    RUN_TEST(test_a_bus_line_and_a_plain_pin_do_not_share);
    RUN_TEST(test_sdmmc_pins);
    RUN_TEST(test_the_i2c_connector);
    RUN_TEST(test_incomplete_descriptions_are_refused);
    RUN_TEST(test_issues_beyond_the_array_are_counted);
    RUN_TEST(test_receiver_data_moves_to_a_terminal);
    RUN_TEST(test_receiver_moves_to_another_place);
    RUN_TEST(test_i2c_modules_move_to_another_place);
    RUN_TEST(test_unreadable_places_are_refused);
    RUN_TEST(test_zwave_radio_at_a_place);
    RUN_TEST(test_zwave_reset_line);
    RUN_TEST(test_every_issue_has_words);
    RUN_TEST(test_a_bus_is_shared_line_for_line);
    RUN_TEST(test_reachable_pins_are_described);
    RUN_TEST(test_rf_on_custom_pins);
    RUN_TEST(test_i2c_on_custom_pins);
    RUN_TEST(test_zwave_on_custom_pins);
    RUN_TEST(test_custom_arguments_are_parsed);
    return UNITY_END();
}
