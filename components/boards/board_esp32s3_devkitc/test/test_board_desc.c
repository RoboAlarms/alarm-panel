/* The ESP32-S3-DevKitC-1 described in data (BRD-008, docs/features/42). */
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
    const size_t n = board_info_validate(&board_esp32s3_devkitc_desc, issues, 8);
    for (size_t i = 0; i < n && i < 8; i++) {
        printf("issue: GPIO%d %s: %s%s%s\n", issues[i].gpio, issues[i].use,
               board_issue_text(issues[i].code), issues[i].other ? " by " : "",
               issues[i].other ? issues[i].other : "");
    }
    TEST_ASSERT_EQUAL_UINT(0, n);
}

/* The facts docs/features/42 relies on: an S3 with octal PSRAM, no screen, touch, speaker or
 * battery; the card and the 433 MHz radio share SPI2. */
static void test_it_is_the_board_features_42_describes(void)
{
    const board_info_t *b = &board_esp32s3_devkitc_desc;
    TEST_ASSERT_EQUAL_STRING("esp32s3_devkitc", b->id);
    TEST_ASSERT_EQUAL_INT(BOARD_CHIP_ESP32S3, b->chip);
    TEST_ASSERT_TRUE(b->psram);
    TEST_ASSERT_TRUE(b->octal_psram);
    TEST_ASSERT_EQUAL_UINT(0, b->width);
    TEST_ASSERT_EQUAL_INT(BOARD_TOUCH_NONE, b->touch);
    TEST_ASSERT_EQUAL_INT(BOARD_AUDIO_NONE, b->audio);
    TEST_ASSERT_FALSE(b->battery);
    TEST_ASSERT_EQUAL_INT(BOARD_SD_SPI, b->sd.bus);
    TEST_ASSERT_EQUAL_INT(b->sd.sck, b->rf.sck);
    TEST_ASSERT_NOT_EQUAL(b->sd.cs, b->rf.cs);
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, b->radio.tx);
    TEST_ASSERT_EQUAL_UINT(1, b->zwave_place_count);
}

/* Nothing a person wires sits on the module's flash, PSRAM, USB or UART0 pins or on a strapping
 * pin: the board starts whatever is connected to its headers. */
static void test_the_attachment_points(void)
{
    const board_info_t *b = &board_esp32s3_devkitc_desc;
    TEST_ASSERT_EQUAL_UINT(8, b->terminal_count);
    int16_t used[64];
    size_t n = 0;
    for (size_t i = 0; i < b->terminal_count; i++) {
        used[n++] = b->terminals[i].gpio;
    }
    for (size_t i = 0; i < b->reach_count; i++) {
        used[n++] = b->reach[i].gpio;
    }
    const int16_t parts[] = {b->rf.sck, b->rf.mosi, b->rf.miso, b->rf.cs, b->rf.data, b->sd.cs,
                             b->i2c.sda, b->i2c.scl, b->zwave_places[0].tx, b->zwave_places[0].rx};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        used[n++] = parts[i];
    }
    for (size_t i = 0; i < n; i++) {
        const int16_t g = used[i];
        TEST_ASSERT_FALSE(board_chip_pin_is_strapping(b->chip, g));
        TEST_ASSERT_TRUE_MESSAGE(g < 26 || g > 37, "flash or octal PSRAM pin");
        TEST_ASSERT_TRUE_MESSAGE(g != 19 && g != 20 && g != 43 && g != 44, "USB or UART0 pin");
        TEST_ASSERT_TRUE(board_chip_pin_has_pullup(b->chip, g));
        for (size_t j = i + 1; j < n; j++) {
            if (used[j] == g && !(g == b->rf.sck || g == b->rf.mosi || g == b->rf.miso)) {
                TEST_FAIL_MESSAGE("a pin offered twice");
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_description_is_safe);
    RUN_TEST(test_it_is_the_board_features_42_describes);
    RUN_TEST(test_the_attachment_points);
    return UNITY_END();
}
