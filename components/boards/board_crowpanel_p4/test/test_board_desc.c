/* The CrowPanel Advanced 7" described in data (BRD-008, docs/features/33 step 4). */
#include "board_desc.h"
#include "board_pins.h"
#include "unity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* BRD-008: the board's own description passes the validator. */
static void test_the_description_is_safe(void)
{
    board_issue_t issues[8];
    const size_t n = board_info_validate(&board_crowpanel_p4_desc, issues, 8);
    for (size_t i = 0; i < n && i < 8; i++) {
        printf("issue: GPIO%d %s: %s%s%s\n", issues[i].gpio, issues[i].use,
               board_issue_text(issues[i].code), issues[i].other ? " by " : "",
               issues[i].other ? issues[i].other : "");
    }
    TEST_ASSERT_EQUAL_UINT(0, n);
}

/* The description is the same board as board_pins.h and the firmware's defaults. */
static void test_it_matches_the_wiring(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    TEST_ASSERT_EQUAL_STRING("crowpanel_p4", b->id);
    TEST_ASSERT_EQUAL_INT(BOARD_CHIP_ESP32P4, b->chip);
    TEST_ASSERT_TRUE(b->psram);
    TEST_ASSERT_EQUAL_UINT(BOARD_LCD_H_RES, b->width);
    TEST_ASSERT_EQUAL_UINT(BOARD_LCD_V_RES, b->height);
    TEST_ASSERT_EQUAL_UINT32(32000u, b->audio_rate_hz); /* what the sounder has always played */

    static const uint8_t terminals[] = BOARD_WIRED_TERMINALS;
    TEST_ASSERT_EQUAL_UINT(sizeof(terminals), b->terminal_count);
    for (size_t i = 0; i < sizeof(terminals); i++) {
        TEST_ASSERT_EQUAL_INT(terminals[i], b->terminals[i].gpio);
    }
    TEST_ASSERT_EQUAL_STRING(BOARD_WIRED_TERMINALS_WHERE, b->terminals_where);

    TEST_ASSERT_EQUAL_INT(BOARD_SD_SDMMC_1BIT, b->sd.bus);
    TEST_ASSERT_EQUAL_INT(BOARD_SD_CLK_GPIO, b->sd.clk);
    TEST_ASSERT_EQUAL_INT(BOARD_SD_CMD_GPIO, b->sd.cmd);
    TEST_ASSERT_EQUAL_INT(BOARD_SD_D0_GPIO, b->sd.d0);

    /* The 433 MHz wiring the firmware used before board_info existed (Kconfig 6-10). */
    TEST_ASSERT_EQUAL_INT(2, b->rf.spi_host);
    TEST_ASSERT_EQUAL_INT(6, b->rf.mosi);
    TEST_ASSERT_EQUAL_INT(7, b->rf.miso);
    TEST_ASSERT_EQUAL_INT(8, b->rf.sck);
    TEST_ASSERT_EQUAL_INT(9, b->rf.cs);
    TEST_ASSERT_EQUAL_INT(10, b->rf.data);

    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_TX_GPIO, b->radio.tx);
    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_RX_GPIO, b->radio.rx);
}

/* The places this board offers are free on it: nothing of its own sits on a terminal. */
static void test_terminals_are_free(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    for (size_t t = 0; t < b->terminal_count; t++) {
        for (size_t p = 0; p < b->pin_count; p++) {
            TEST_ASSERT_NOT_EQUAL(b->pins[p].gpio, b->terminals[t].gpio);
        }
    }
}

/* BRD-013, RF-008: a receiver module on J7 (the second panel: DATA on IO3). Every J7
 * terminal can take it and the board stays safe; J7 pins that are not terminals (25 is USB D+,
 * 27/28 the V1.2 radio UART) and the P4's own pins cannot. */
static void test_receiver_data_on_j7(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    board_info_t moved;
    board_terminal_t terms[16];
    static const uint8_t terminals[] = BOARD_WIRED_TERMINALS;
    for (size_t i = 0; i < sizeof(terminals); i++) {
        TEST_ASSERT_TRUE(board_rf_data_on(b, terminals[i], &moved, terms, 16));
        TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&moved, NULL, 0));
        TEST_ASSERT_EQUAL_UINT(b->terminal_count - 1u, moved.terminal_count);
        for (size_t t = 0; t < moved.terminal_count; t++) {
            TEST_ASSERT_NOT_EQUAL(terminals[i], moved.terminals[t].gpio);
        }
    }
    TEST_ASSERT_TRUE(board_rf_data_on(b, 3, &moved, terms, 16));
    TEST_ASSERT_EQUAL_INT(3, moved.rf.data);
    TEST_ASSERT_TRUE(board_rf_data_on(b, 10, &moved, terms, 16)); /* back on the socket */
    TEST_ASSERT_EQUAL_UINT(b->terminal_count, moved.terminal_count);
    const int16_t refused[] = {25, 27, 28, 38, BOARD_UART0_TX_GPIO, BOARD_I2C_SDA_GPIO, 60};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        TEST_ASSERT_FALSE(board_rf_data_on(b, refused[i], &moved, terms, 16));
    }
}

/* BRD-014, BRD-015, RF-019, RTC-013: the J7 places (docs/research/AddOnWiringResearch.md).
 * A CC1101 on IO52/51/50/49 + IO5 and the clock module on IO27/IO28 are each safe, and so are
 * both at once with the wireless socket left to the sensor radio: the layout the research
 * recommends, with IO2, IO3 and IO4 still wired zone terminals. */
static void test_j7_places(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    const board_rf_place_t *rf = board_rf_place(b, "j7");
    TEST_ASSERT_NOT_NULL(rf);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_RF_SCK_GPIO, rf->rf.sck);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_RF_MOSI_GPIO, rf->rf.mosi);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_RF_MISO_GPIO, rf->rf.miso);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_RF_CS_GPIO, rf->rf.cs);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_RF_GDO0_GPIO, rf->rf.data);
    TEST_ASSERT_EQUAL_INT(2, rf->rf.spi_host);
    const board_i2c_place_t *i2c = board_i2c_place(b, "j7");
    TEST_ASSERT_NOT_NULL(i2c);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_I2C_SDA_GPIO, i2c->sda);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_I2C_SCL_GPIO, i2c->scl);
    TEST_ASSERT_EQUAL_INT(1, i2c->port); /* the touch screen has controller 0 */
    TEST_ASSERT_NULL(board_rf_place(b, "socket")); /* the socket is the board's own place */

    board_info_t cc, clock, both;
    board_terminal_t cc_terms[16], clock_terms[16], both_terms[16];
    TEST_ASSERT_TRUE(board_rf_place_on(b, "j7", &cc, cc_terms, 16));
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&cc, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(3, cc.terminal_count);
    TEST_ASSERT_EQUAL_INT(2, cc.terminals[0].gpio);
    TEST_ASSERT_EQUAL_INT(3, cc.terminals[1].gpio);
    TEST_ASSERT_EQUAL_INT(4, cc.terminals[2].gpio);

    TEST_ASSERT_TRUE(board_i2c_place_on(b, "j7", &clock, clock_terms, 16));
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&clock, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(b->terminal_count, clock.terminal_count); /* 27/28 are no terminals */
    TEST_ASSERT_EQUAL_STRING(BOARD_J7_I2C_WHERE, clock.i2c.where);

    TEST_ASSERT_TRUE(board_i2c_place_on(&cc, "j7", &both, both_terms, 16));
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&both, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(3, both.terminal_count);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_RF_CS_GPIO, both.rf.cs);
    TEST_ASSERT_EQUAL_INT(BOARD_J7_I2C_SDA_GPIO, both.i2c.sda);

    /* the J7 place's data line is not a strapping pin (GDO0 idles high on noise) */
    TEST_ASSERT_FALSE(board_chip_pin_is_strapping(BOARD_CHIP_ESP32P4, BOARD_J7_RF_GDO0_GPIO));
}

/* BRD-016, BRD-017 (docs/features/36 hardware.md §4): the Z-Wave radio's places. Off in the
 * description; in the socket it takes the sensor radio's UART; on J7 it takes the clock module's
 * J7 place (refused together); and "with everything fitted" - the ESP32-H2 in the socket, the
 * CC1101 on J7, the ZAC93 on J7 with its reset on IO4, the clock on J13 - is safe, with IO2 and
 * IO3 left for wired zones. */
static void test_zwave_places(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    TEST_ASSERT_FALSE(b->zwave.wired);
    const board_zwave_place_t *socket = board_zwave_place(b, "socket");
    const board_zwave_place_t *j7 = board_zwave_place(b, "j7");
    TEST_ASSERT_NOT_NULL(socket);
    TEST_ASSERT_NOT_NULL(j7);
    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_TX_GPIO, socket->tx);
    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_RX_GPIO, socket->rx);
    TEST_ASSERT_EQUAL_INT(27, j7->tx);
    TEST_ASSERT_EQUAL_INT(28, j7->rx);

    board_info_t in_socket, on_j7, clock, clash, cc, all, reset;
    board_terminal_t t1[16], t2[16], t3[16], t4[16], t5[16], t6[16], t7[16];
    TEST_ASSERT_TRUE(board_zwave_place_on(b, "socket", &in_socket, t1, 16));
    TEST_ASSERT_EQUAL_INT(BOARD_NO_PIN, in_socket.radio.tx); /* no ESP32-H2 with it */
    TEST_ASSERT_EQUAL_UINT(b->terminal_count, in_socket.terminal_count);
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&in_socket, NULL, 0));

    TEST_ASSERT_TRUE(board_zwave_place_on(b, "j7", &on_j7, t2, 16));
    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_TX_GPIO, on_j7.radio.tx); /* the socket stays */
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&on_j7, NULL, 0));
    TEST_ASSERT_TRUE(board_i2c_place_on(b, "j7", &clock, t3, 16));
    TEST_ASSERT_TRUE(board_zwave_place_on(&clock, "j7", &clash, t4, 16));
    TEST_ASSERT_EQUAL_UINT(2, board_info_validate(&clash, NULL, 0)); /* IO27, IO28 twice */

    TEST_ASSERT_TRUE(board_rf_place_on(b, "j7", &cc, t5, 16));
    TEST_ASSERT_TRUE(board_zwave_place_on(&cc, "j7", &all, t6, 16));
    TEST_ASSERT_TRUE(board_zwave_reset_on(&all, 4, &reset, t7, 16));
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&reset, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(2, reset.terminal_count);
    TEST_ASSERT_EQUAL_INT(2, reset.terminals[0].gpio);
    TEST_ASSERT_EQUAL_INT(3, reset.terminals[1].gpio);
    TEST_ASSERT_EQUAL_INT(4, reset.zwave.reset);
    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_TX_GPIO, reset.radio.tx);

    /* the reset line only on a terminal: not USB D+, not a strapping pin, not a pin of the P4's */
    TEST_ASSERT_FALSE(board_zwave_reset_on(&on_j7, 25, &reset, t7, 16));
    TEST_ASSERT_FALSE(board_zwave_reset_on(&on_j7, 38, &reset, t7, 16));
    TEST_ASSERT_FALSE(board_zwave_reset_on(&on_j7, BOARD_UART0_TX_GPIO, &reset, t7, 16));
    TEST_ASSERT_TRUE(board_zwave_reset_on(&on_j7, 52, &reset, t7, 16));
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&reset, NULL, 0));
}

/* BRD-018: the pins a person can reach are J7's ten IO pins, as printed; IO25 (USB D+) is not
 * one, and none of them is a pin of the board's own. */
static void test_reachable_pins(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    static const int16_t j7[] = {2, 3, 4, 5, 27, 28, 49, 50, 51, 52};
    TEST_ASSERT_EQUAL_UINT(sizeof(j7) / sizeof(j7[0]), b->reach_count);
    for (size_t i = 0; i < b->reach_count; i++) {
        char label[16];
        snprintf(label, sizeof(label), "IO%d", j7[i]);
        TEST_ASSERT_EQUAL_INT(j7[i], b->reach[i].gpio);
        TEST_ASSERT_EQUAL_STRING(label, b->reach[i].label);
        TEST_ASSERT_EQUAL_STRING("J7", b->reach[i].connector);
        for (size_t p = 0; p < b->pin_count; p++) {
            TEST_ASSERT_NOT_EQUAL(b->pins[p].gpio, b->reach[i].gpio);
        }
    }
    /* every reachable pin is on J7 (board_pins.h), and every wired terminal is reachable */
    static const int16_t on_j7[] = BOARD_J7_GPIOS;
    for (size_t i = 0; i < b->reach_count; i++) {
        bool found = false;
        for (size_t k = 0; k < sizeof(on_j7) / sizeof(on_j7[0]); k++) {
            found = found || on_j7[k] == b->reach[i].gpio;
        }
        TEST_ASSERT_TRUE(found);
    }
    for (size_t t = 0; t < b->terminal_count; t++) {
        TEST_ASSERT_NOT_NULL(board_reach(b, b->terminals[t].gpio));
    }
    TEST_ASSERT_NULL(board_reach(b, 25));
    TEST_ASSERT_NULL(board_reach(b, BOARD_RADIO_UART_TX_GPIO));
}

/* BRD-019: the board's list of its own pins is complete - the ones the firmware drives and the
 * ones wired to parts it doesn't use yet (microphone, camera, UART1 and UART3 IN). */
static void test_every_pin_of_the_board_is_listed(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    static const int16_t used[] = {
        BOARD_LCD_BACKLIGHT_GPIO, BOARD_LCD_RESET_GPIO, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO,
        BOARD_TOUCH_RST_GPIO, BOARD_TOUCH_INT_GPIO, BOARD_AUDIO_LRCLK_GPIO, BOARD_AUDIO_BCLK_GPIO,
        BOARD_AUDIO_DOUT_GPIO, BOARD_AUDIO_AMP_EN_GPIO, 24, 26, 14, 15, 16, 17, 18, 19,
        BOARD_C6_EN_GPIO, BOARD_UART0_TX_GPIO, BOARD_UART0_RX_GPIO, 25, 47, 48, 33, 34, 11, 12, 13,
    };
    for (size_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        bool listed = false;
        for (size_t p = 0; p < b->pin_count; p++) {
            listed = listed || b->pins[p].gpio == used[i];
        }
        if (!listed) {
            printf("GPIO %d is not listed\n", used[i]);
        }
        TEST_ASSERT_TRUE(listed);
    }
    /* and the board's other parts: SD, the socket's receiver pins and UART */
    const int16_t parts[] = {b->sd.clk, b->sd.cmd, b->sd.d0, b->rf.sck, b->rf.mosi, b->rf.miso,
                             b->rf.cs, b->rf.data, b->radio.tx, b->radio.rx};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        TEST_ASSERT_NULL(board_reach(b, parts[i]));
    }
}

/* BRD-020: every layout today's firmware accepts still passes the stricter sharing check, and
 * the one it refuses is still refused: the clock module's J7 place and the Z-Wave radio's
 * (IO27, IO28 twice). Walked in the firmware's order (panel_app wired_board()): the clock's
 * place, then the receiver's place or `rf pin` on a terminal, then the Z-Wave radio's place. */
static void test_every_named_layout_keeps_its_result(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    const char *clock_places[] = {NULL, "j7"};
    const char *rf_places[] = {NULL, "j7"};
    const char *zwave_places[] = {NULL, "socket", "j7"};
    size_t walked = 0;
    for (size_t c = 0; c < 2; c++) {
        for (size_t r = 0; r < 2; r++) {
            /* -1: the receiver's data where the place has it; else `rf pin` on terminal p */
            for (int p = -1; p < (int)b->terminal_count; p++) {
                if (r == 1 && p >= 0) {
                    continue; /* `rf pin` applies only at the receiver's own place */
                }
                for (size_t z = 0; z < 3; z++) {
                    board_info_t s1, s2, s3;
                    board_terminal_t t1[16], t2[16], t3[16];
                    const board_info_t *w = b;
                    if (clock_places[c] != NULL) {
                        TEST_ASSERT_TRUE(board_i2c_place_on(w, clock_places[c], &s1, t1, 16));
                        w = &s1;
                    }
                    if (rf_places[r] != NULL) {
                        TEST_ASSERT_TRUE(board_rf_place_on(w, rf_places[r], &s2, t2, 16));
                        w = &s2;
                    } else if (p >= 0) {
                        TEST_ASSERT_TRUE(
                            board_rf_data_on(w, b->terminals[p].gpio, &s2, t2, 16));
                        w = &s2;
                    }
                    if (zwave_places[z] != NULL) {
                        TEST_ASSERT_TRUE(board_zwave_place_on(w, zwave_places[z], &s3, t3, 16));
                        w = &s3;
                    }
                    board_issue_t issues[4];
                    const size_t n = board_info_validate(w, issues, 4);
                    const bool clash = c == 1 && z == 2;
                    if (n != (clash ? 2u : 0u)) {
                        printf("clock %s, rf %s, rf pin %d, zwave %s: %u issues\n",
                               clock_places[c] ? clock_places[c] : "own",
                               rf_places[r] ? rf_places[r] : "own",
                               p >= 0 ? b->terminals[p].gpio : -1,
                               zwave_places[z] ? zwave_places[z] : "off", (unsigned)n);
                    }
                    TEST_ASSERT_EQUAL_UINT(clash ? 2u : 0u, n);
                    if (clash) {
                        TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issues[0].code);
                        TEST_ASSERT_EQUAL_INT(27, issues[0].gpio);
                        TEST_ASSERT_EQUAL_INT(BOARD_ISSUE_IN_USE, issues[1].code);
                        TEST_ASSERT_EQUAL_INT(28, issues[1].gpio);
                    }
                    walked++;
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(60, walked);
}

/* BRD-021: custom places on the CrowPanel - the named J7 places' pins work as custom pins, the
 * clock gets the J7 place's I2C controller, and the refusals name the GPIO and the signal. */
static void test_custom_places(void)
{
    const board_info_t *b = &board_crowpanel_p4_desc;
    board_info_t rf, clock, zw, out;
    board_terminal_t t1[16], t2[16], t3[16], t4[16];
    char why[96];
    const board_rf_t cc = {.spi_host = -1, .sck = 52, .mosi = 51, .miso = 50, .cs = 49,
                           .data = 5, .where = NULL};
    TEST_ASSERT_TRUE(board_rf_custom_on(b, &cc, &rf, t1, 16, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(2, rf.rf.spi_host);
    TEST_ASSERT_EQUAL_UINT(3, rf.terminal_count); /* IO2, IO3, IO4 stay zones' */
    TEST_ASSERT_TRUE(board_i2c_custom_on(&rf, 28, 27, &clock, t2, 16, why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(BOARD_J7_I2C_PORT, clock.i2c.port);
    TEST_ASSERT_EQUAL_UINT(0, board_info_validate(&clock, NULL, 0));
    /* the Z-Wave radio can't have the clock's pins, but can have a free terminal */
    TEST_ASSERT_FALSE(board_zwave_custom_on(&clock, 27, 3, BOARD_NO_PIN, &zw, t3, 16, why,
                                            sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 27 (TX) is already in use: the I2C modules", why);
    TEST_ASSERT_TRUE(board_zwave_custom_on(&rf, 27, 28, 4, &zw, t3, 16, why, sizeof(why)));
    TEST_ASSERT_EQUAL_UINT(2, zw.terminal_count);
    TEST_ASSERT_EQUAL_INT(BOARD_RADIO_UART_TX_GPIO, zw.radio.tx); /* the socket stays the H2's */

    /* the socket's pins, USB D+ and the board's own are not for custom places */
    const board_rf_t on_socket = {.spi_host = -1, .sck = 8, .mosi = 6, .miso = 7, .cs = 9,
                                  .data = 10, .where = NULL};
    TEST_ASSERT_FALSE(board_rf_custom_on(b, &on_socket, &out, t4, 16, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 8 is not a pin you can reach on this board (J7's IO pins only)",
                             why);
    TEST_ASSERT_FALSE(board_i2c_custom_on(b, 25, 27, &out, t4, 16, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 25 is not a pin you can reach on this board (J7's IO pins only)",
                             why);
    TEST_ASSERT_FALSE(board_zwave_custom_on(b, 53, 54, BOARD_NO_PIN, &out, t4, 16, why,
                                            sizeof(why)));
    const board_rf_t twice = {.spi_host = -1, .sck = 52, .mosi = 50, .miso = 51, .cs = 50,
                              .data = 5, .where = NULL};
    TEST_ASSERT_FALSE(board_rf_custom_on(b, &twice, &out, t4, 16, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO 50 is used twice (MOSI and CS)", why);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_description_is_safe);
    RUN_TEST(test_it_matches_the_wiring);
    RUN_TEST(test_terminals_are_free);
    RUN_TEST(test_receiver_data_on_j7);
    RUN_TEST(test_j7_places);
    RUN_TEST(test_zwave_places);
    RUN_TEST(test_reachable_pins);
    RUN_TEST(test_every_pin_of_the_board_is_listed);
    RUN_TEST(test_every_named_layout_keeps_its_result);
    RUN_TEST(test_custom_places);
    return UNITY_END();
}
