/* The LCDWiki E32R40T described in data (BRD-008, docs/features/33 step 10, docs/features/19). */
#include "board_desc.h"
#include "board_pins.h"
#include "unity.h"

#include <stdio.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* BRD-008: the board's own description passes the validator. */
static void test_the_description_is_safe(void)
{
    board_issue_t issues[8];
    const size_t n = board_info_validate(&board_e32r40t_desc, issues, 8);
    for (size_t i = 0; i < n && i < 8; i++) {
        printf("issue: GPIO%d %s: %s%s%s\n", issues[i].gpio, issues[i].use,
               board_issue_text(issues[i].code), issues[i].other ? " by " : "",
               issues[i].other ? issues[i].other : "");
    }
    TEST_ASSERT_EQUAL_UINT(0, n);
}

/* The facts docs/features/19 relies on: a classic ESP32 without PSRAM, a 480x320 resistive screen,
 * a DAC at a rate the sounder plays, the card on SPI3. */
static void test_it_is_the_board_features_19_describes(void)
{
    const board_info_t *b = &board_e32r40t_desc;
    TEST_ASSERT_EQUAL_STRING("e32r40t", b->id);
    TEST_ASSERT_EQUAL_INT(BOARD_CHIP_ESP32, b->chip);
    TEST_ASSERT_FALSE(b->psram);
    TEST_ASSERT_EQUAL_UINT(480, b->width);
    TEST_ASSERT_EQUAL_UINT(320, b->height);
    TEST_ASSERT_EQUAL_INT(BOARD_TOUCH_RESISTIVE, b->touch);
    TEST_ASSERT_EQUAL_INT(BOARD_AUDIO_DAC, b->audio);
    TEST_ASSERT_EQUAL_UINT32(24000u, b->audio_rate_hz);
    TEST_ASSERT_EQUAL_INT(BOARD_SD_SPI, b->sd.bus);
    TEST_ASSERT_EQUAL_INT(3, b->sd.spi_host);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, b->radio.tx);
}

/* The places services attach can't damage the board: the terminal on an input-only pin
 * declares its external pull-up, the receiver shares the card's bus but not its chip select,
 * and nothing that an outside signal drives sits on a strapping pin. */
static void test_the_attachment_points(void)
{
    const board_info_t *b = &board_e32r40t_desc;
    TEST_ASSERT_EQUAL_UINT(3, b->terminal_count);
    for (size_t i = 0; i < b->terminal_count; i++) {
        const board_terminal_t *t = &b->terminals[i];
        if (!board_chip_pin_has_pullup(b->chip, t->gpio)) {
            TEST_ASSERT_TRUE_MESSAGE(t->external_pullup, "input-only terminal without a pull-up");
        }
        TEST_ASSERT_FALSE(board_chip_pin_is_strapping(b->chip, t->gpio));
    }
    TEST_ASSERT_EQUAL_INT(b->sd.sck, b->rf.sck);
    TEST_ASSERT_NOT_EQUAL(b->sd.cs, b->rf.cs);
    TEST_ASSERT_FALSE(board_chip_pin_is_strapping(b->chip, b->rf.data));
    TEST_ASSERT_FALSE(board_chip_pin_can_output(b->chip, b->rf.data)); /* input only is fine */
}

/* The strapping pins it uses are its own parts (display MISO IO12, DC IO2, CS IO15, the BOOT
 * button IO0, the card's CS IO5), none an outside signal: the board boots whatever is plugged
 * into its headers. */
static void test_strapping_pins_are_only_the_boards_own(void)
{
    const board_info_t *b = &board_e32r40t_desc;
    for (size_t i = 0; i < b->pin_count; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, b->pins[i].flags & BOARD_PIN_EXTERNAL);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_description_is_safe);
    RUN_TEST(test_it_is_the_board_features_19_describes);
    RUN_TEST(test_the_attachment_points);
    RUN_TEST(test_strapping_pins_are_only_the_boards_own);
    return UNITY_END();
}
