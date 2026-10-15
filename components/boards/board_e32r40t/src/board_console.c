/*
 * board_console.c - the E32R40T's own console commands, for bringing a board up:
 *
 *   lcd                          the panel's settings
 *   lcd invert|bgr|swap|mx|my on|off   change one live (not stored: the defaults are in
 *                                board_pins.h once a board's are known)
 *   touch                        calibration, the last raw reading, statistics
 *   touch cal                    draw the three targets and calibrate again (stored)
 */
#include "board.h"

#include <stdio.h>
#include <string.h>

#include "board_internal.h"
#include "esp_check.h"
#include "esp_console.h"

static const char *TAG = "board_con";

static void print_lcd(void)
{
    const board_lcd_settings_t *s = board_lcd_settings();
    printf("lcd invert=%s bgr=%s swap=%s mx=%s my=%s\n", s->invert ? "on" : "off",
           s->bgr ? "on" : "off", s->swap_xy ? "on" : "off", s->mirror_x ? "on" : "off",
           s->mirror_y ? "on" : "off");
}

static int cmd_lcd(int argc, char **argv)
{
    if (board_lcd_panel() == NULL) {
        printf("no display\n");
        return 1;
    }
    if (argc == 3) {
        board_lcd_settings_t s = *board_lcd_settings();
        const bool on = strcmp(argv[2], "on") == 0;
        if (!on && strcmp(argv[2], "off") != 0) {
            printf("lcd %s on|off\n", argv[1]);
            return 1;
        }
        if (strcmp(argv[1], "invert") == 0) {
            s.invert = on;
        } else if (strcmp(argv[1], "bgr") == 0) {
            s.bgr = on;
        } else if (strcmp(argv[1], "swap") == 0) {
            s.swap_xy = on;
        } else if (strcmp(argv[1], "mx") == 0) {
            s.mirror_x = on;
        } else if (strcmp(argv[1], "my") == 0) {
            s.mirror_y = on;
        } else {
            printf("lcd [invert|bgr|swap|mx|my on|off]\n");
            return 1;
        }
        if (!board_ui_lock(1000)) {
            printf("the UI is busy\n");
            return 1;
        }
        const esp_err_t err = board_lcd_apply(&s);
        board_ui_unlock();
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    } else if (argc != 1) {
        printf("lcd [invert|bgr|swap|mx|my on|off]\n");
        return 1;
    }
    print_lcd();
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "cal") == 0) {
        const esp_err_t err = board_touch_calibrate();
        printf(err == ESP_OK ? "touch the three targets on the screen\n" : "failed: %s\n",
               esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc != 1) {
        printf("touch [cal]\n");
        return 1;
    }
    board_touch_raw_t raw;
    board_touch_last_raw(&raw);
    board_touch_stats_t st;
    board_touch_stats(&st, false);
    printf("touch calibration=%s\n", board_touch_calibrated() ? "stored" : "default (run touch cal)");
    printf("  last raw: %s x=%u y=%u pressure=%u (pressed from %u)\n", raw.ok ? "ok" : "error",
           raw.x, raw.y, raw.pressure, (unsigned)BOARD_TOUCH_PRESS_MIN);
    printf("  polls=%lu presses=%lu errors=%lu longest=%lu ms pressed=%s\n",
           (unsigned long)st.polls, (unsigned long)st.presses, (unsigned long)st.errors,
           (unsigned long)st.longest_press_ms, st.pressed ? "yes" : "no");
    return 0;
}

esp_err_t board_e32r40t_console(void)
{
    const esp_console_cmd_t lcd = {
        .command = "lcd",
        .help = "Panel settings: invert, bgr, swap, mx, my on|off (live, not stored)",
        .hint = "[invert|bgr|swap|mx|my on|off]",
        .func = cmd_lcd,
    };
    const esp_console_cmd_t touch = {
        .command = "touch",
        .help = "Touch: calibration, last raw reading; 'touch cal' draws the targets again",
        .hint = "[cal]",
        .func = cmd_touch,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&lcd), TAG, "lcd");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&touch), TAG, "touch");
    return ESP_OK;
}
