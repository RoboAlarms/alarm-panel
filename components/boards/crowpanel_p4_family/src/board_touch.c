/*
 * board_touch.c - GT911 capacitive touch on I2C0, registered with LVGL.
 *
 * The touch is polled by LVGL's input timer (every LV_DEF_REFR_PERIOD), not read on the GT911's
 * interrupt: the GT911 writes its next report only after the host has cleared the previous one,
 * so when the UI was busy at the moment a finger lifted, the "released" report was lost and no
 * interrupt followed. LVGL then kept the finger "pressed" at its last spot, the next tap looked
 * like a drag from there (no click) and scrolling jumped. Polling clears the report every time.
 * The INT pin is still used at start-up, where it selects the GT911's I2C address.
 */
#include "crowpanel_p4.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

static const char *TAG = "board_touch";

static i2c_master_bus_handle_t s_i2c;

/* Diagnostics (read with board_touch_stats; written by the LVGL task only). */
static uint32_t s_polls;
static uint32_t s_presses;
static uint32_t s_errors;
static uint32_t s_error_run; /* consecutive failed polls */
#define ERROR_RUN_RELEASE 3u /* polls without an answer before a held finger counts as lifted */
static uint32_t s_max_gap_us;
static uint32_t s_longest_press_ms;
static int64_t s_last_poll_us;
static int64_t s_press_start_us;
static bool s_pressed;

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

/* Start-up creates the bus once, from one task: the clock service asks for it before the
 * display starts (board_i2c_ext), board_touch_init after. */
static esp_err_t i2c_init(void)
{
    if (s_i2c != NULL) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

i2c_master_bus_handle_t board_i2c_ext(void)
{
    const esp_err_t err = i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 for the I2C connector: %s", esp_err_to_name(err));
        return NULL;
    }
    return s_i2c;
}

/* Try one GT911 address; on failure release what was created. */
static esp_err_t try_gt911(uint8_t addr, esp_lcd_touch_handle_t *out)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = BOARD_I2C_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io");

    esp_lcd_touch_io_gt911_config_t gt911 = {.dev_addr = addr};
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO, /* address selection during the reset */
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .driver_data = &gt911,
    };
    const esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, out);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(io);
    }
    return err;
}

/* LVGL input timer: one GT911 poll. */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = lv_indev_get_driver_data(indev);
    const int64_t now = esp_timer_get_time();
    if (s_last_poll_us != 0) {
        const uint32_t gap = (uint32_t)(now - s_last_poll_us);
        if (gap > s_max_gap_us) {
            s_max_gap_us = gap;
        }
    }
    s_last_poll_us = now;
    s_polls++;

    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;
#if ALARM_BENCH_FAULT_NO_TOUCH
    /* Bench fault image (cmake/alarm_firmware.cmake): the controller never answers, as if its
     * cable were out; the screen still draws, and the self-test must not accept that. */
    const bool ok = false;
    (void)tp;
#else
    const bool ok = esp_lcd_touch_read_data(tp) == ESP_OK &&
                    esp_lcd_touch_get_data(tp, &point, &count, 1) == ESP_OK;
#endif
    if (!ok) {
        s_errors++;
        /* An I2C hiccup changes nothing - but a controller that stops answering while a finger
         * is down must not leave that finger down for good: the panic buttons and the
         * hold-to-confirm rows complete on time alone. Three polls (about 50 ms) of silence
         * count as a lift. */
        s_error_run++;
        count = s_pressed && s_error_run < ERROR_RUN_RELEASE ? 1u : 0u;
        point.x = (uint16_t)data->point.x;
        point.y = (uint16_t)data->point.y;
    } else {
        s_error_run = 0;
    }
    if (count > 0u) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!s_pressed) {
            s_presses++;
            s_press_start_us = now;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED; /* at the last point */
        if (s_pressed) {
            const uint32_t ms = (uint32_t)((now - s_press_start_us) / 1000);
            if (ms > s_longest_press_ms) {
                s_longest_press_ms = ms;
            }
        }
    }
    s_pressed = count > 0u;
}

void board_touch_stats(board_touch_stats_t *out, bool reset_max)
{
    out->polls = s_polls;
    out->presses = s_presses;
    out->errors = s_errors;
    out->max_gap_us = s_max_gap_us;
    out->longest_press_ms = s_longest_press_ms;
    out->pressed = s_pressed;
    if (reset_max) {
        s_max_gap_us = 0;
        s_longest_press_ms = 0;
    }
}

esp_err_t board_touch_init(lv_display_t *disp, lv_indev_t **out_indev)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C0");

    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t err = try_gt911(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, &tp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 not at 0x%02X, trying 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
        err = try_gt911(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, &tp);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "GT911 not found");
    gpio_intr_disable(BOARD_TOUCH_INT_GPIO); /* polled, see the top of this file */

    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_ERR_TIMEOUT, TAG, "LVGL lock");
    lv_indev_t *indev = lv_indev_create();
    if (indev != NULL) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read);
        lv_indev_set_driver_data(indev, tp);
        lv_indev_set_display(indev, disp);
        /* A finger jitters by a few pixels during a tap: LVGL's default of 10 px turned taps
         * into tiny scrolls (no click) on the 7" panel. */
        lv_indev_set_scroll_limit(indev, 20);
    }
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "LVGL touch");
    if (out_indev != NULL) {
        *out_indev = indev;
    }
    ESP_LOGI(TAG, "GT911 touch ready (polled)");
    return ESP_OK;
}
