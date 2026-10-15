/*
 * board_touch.c - the E32R40T's XPT2046 resistive touch under LVGL, with its calibration.
 *
 * Polled from LVGL's read callback (like the 7"'s GT911: interrupts lost releases there), each
 * poll's raw samples go through touch_calib's filter and calibration (host-tested). The
 * calibration is three targets touched on the screen (docs/features/19 CYD-008): it runs by itself
 * when none is stored, again from the console (`touch cal`), and when a finger is held on the
 * screen for 3 s in the first 10 s after power-up (the way back when a bad calibration makes
 * the screen unusable - there is no button to press). The result is kept in NVS ("touch").
 * While it runs, LVGL sees no touches: nothing under the targets can be pressed by accident.
 * The targets are drawn still and move only when touched (nothing blinks, UI-020).
 */
#include "board.h"

#include <string.h>

#include "board_internal.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "touch_xpt2046/touch_xpt2046.h"

static const char *TAG = "board_touch";

#define CAL_NVS_NS      "touch"
#define CAL_NVS_KEY     "cal"
#define CAL_VERSION     1u
#define CAL_MIN_SAMPLES 4u       /* polls of a steady press before a target counts */
#define RECOVERY_WINDOW_US 10000000 /* a finger held in the first 10 s ... */
#define RECOVERY_HOLD_US   3000000  /* ... for 3 s: calibrate again */

typedef struct {
    uint32_t version;
    int32_t a, b, c, d, e, f;
} stored_cal_t;

static touch_xpt2046_t s_xpt;
static touch_calib_t s_cal;
static bool s_cal_stored;
static touch_filter_t s_filter;
static int64_t s_started_us;

/* statistics (LVGL task) */
static uint32_t s_polls, s_presses, s_errors, s_max_gap_us, s_longest_press_ms;
static int64_t s_last_poll_us, s_press_start_us;
static bool s_pressed;
static board_touch_raw_t s_last_raw;

/* the calibration run (LVGL task) */
static const uint16_t TARGET_X[3] = {48, 432, 240};
static const uint16_t TARGET_Y[3] = {32, 160, 288};
static struct {
    bool active;
    uint8_t step;
    uint32_t n;
    uint32_t sum_x, sum_y;
    bool down;
    touch_calib_pair_t pairs[3];
    lv_obj_t *layer, *target, *title, *note;
} s_run;

static void defaults(void)
{
    touch_calib_linear(&s_cal, BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_TOUCH_RAW_X_MIN,
                       BOARD_TOUCH_RAW_X_MAX, BOARD_TOUCH_RAW_Y_MIN, BOARD_TOUCH_RAW_Y_MAX,
                       BOARD_TOUCH_SWAP_XY, BOARD_TOUCH_MIRROR_X, BOARD_TOUCH_MIRROR_Y);
}

static void load(void)
{
    defaults();
    nvs_handle_t h;
    if (nvs_open(CAL_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    stored_cal_t st;
    size_t len = sizeof(st);
    if (nvs_get_blob(h, CAL_NVS_KEY, &st, &len) == ESP_OK && len == sizeof(st) &&
        st.version == CAL_VERSION) {
        s_cal = (touch_calib_t){st.a, st.b, st.c, st.d, st.e, st.f, BOARD_LCD_H_RES,
                                BOARD_LCD_V_RES};
        s_cal_stored = true;
    }
    nvs_close(h);
}

static esp_err_t store(const touch_calib_t *c)
{
    const stored_cal_t st = {CAL_VERSION, c->a, c->b, c->c, c->d, c->e, c->f};
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(CAL_NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(h, CAL_NVS_KEY, &st, sizeof(st));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool board_touch_calibrated(void)
{
    return s_cal_stored;
}

void board_touch_last_raw(board_touch_raw_t *out)
{
    *out = s_last_raw;
}

/* ---- the calibration run -------------------------------------------------------------- */

static void run_show(void)
{
    lv_obj_align(s_run.target, LV_ALIGN_TOP_LEFT, TARGET_X[s_run.step] - 20,
                 TARGET_Y[s_run.step] - 20);
    lv_label_set_text_fmt(s_run.title, "Touch calibration  %u of 3", (unsigned)(s_run.step + 1u));
}

static lv_obj_t *line(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xF1F5F9), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_center(o);
    return o;
}

static void light_targets(lv_timer_t *t);

/* LVGL task, lock held */
static void run_begin(void)
{
    if (s_run.active) {
        return;
    }
    memset(&s_run, 0, sizeof(s_run));
    s_run.active = true;
    s_run.layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_run.layer);
    lv_obj_set_size(s_run.layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_run.layer, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_bg_opa(s_run.layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_run.layer, LV_OBJ_FLAG_CLICKABLE); /* nothing underneath is reached */

    s_run.title = lv_label_create(s_run.layer);
    lv_obj_set_style_text_color(s_run.title, lv_color_hex(0xF1F5F9), 0);
    lv_obj_align(s_run.title, LV_ALIGN_CENTER, 0, -14);
    s_run.note = lv_label_create(s_run.layer);
    lv_obj_set_style_text_color(s_run.note, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_run.note, "Press the centre of the cross firmly");
    lv_obj_align(s_run.note, LV_ALIGN_CENTER, 0, 14);

    /* A ring with a cross: 40 px, steady. */
    s_run.target = lv_obj_create(s_run.layer);
    lv_obj_remove_style_all(s_run.target);
    lv_obj_set_size(s_run.target, 40, 40);
    lv_obj_set_style_radius(s_run.target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_run.target, 2, 0);
    lv_obj_set_style_border_color(s_run.target, lv_color_hex(0x60A5FA), 0);
    line(s_run.target, 40, 2);
    line(s_run.target, 2, 40);
    run_show();
    lv_timer_create(light_targets, 200, NULL);
    ESP_LOGI(TAG, "touch calibration: touch the three targets");
}

static void run_end(void)
{
    if (s_run.layer != NULL) {
        lv_obj_delete(s_run.layer);
    }
    memset(&s_run, 0, sizeof(s_run));
    board_backlight_hold_on(false); /* back to the UI's own level (off without a UI) */
}

/* The targets' first frame is in the panel's memory by now (a whole screen takes about 60 ms
 * at 40 MHz): light them even when no UI has lit the screen. */
static void light_targets(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_run.active) {
        board_backlight_hold_on(true);
    }
}

/* One poll during the run: collect a steady press on the target, move on at its release. */
static void run_poll(const touch_raw_t *raw)
{
    const bool pressed = touch_pressure(raw) >= BOARD_TOUCH_PRESS_MIN;
    if (pressed) {
        if (!s_run.down) {
            s_run.down = true; /* the first poll after touch-down isn't settled */
            s_run.n = 0;
            s_run.sum_x = s_run.sum_y = 0;
            return;
        }
        s_run.sum_x += touch_median(raw->x, TOUCH_SAMPLES);
        s_run.sum_y += touch_median(raw->y, TOUCH_SAMPLES);
        s_run.n++;
        return;
    }
    if (!s_run.down) {
        return;
    }
    s_run.down = false;
    if (s_run.n < CAL_MIN_SAMPLES) {
        return; /* too brief to trust: the same target again */
    }
    s_run.pairs[s_run.step] = (touch_calib_pair_t){
        (uint16_t)(s_run.sum_x / s_run.n), (uint16_t)(s_run.sum_y / s_run.n),
        TARGET_X[s_run.step], TARGET_Y[s_run.step]};
    if (s_run.step < 2u) {
        s_run.step++;
        run_show();
        return;
    }
    touch_calib_t c;
    if (!touch_calib_solve(&c, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_run.pairs)) {
        ESP_LOGW(TAG, "touch calibration: the targets didn't make sense, once more");
        lv_label_set_text(s_run.note, "That didn't work: once more, on the centre");
        s_run.step = 0;
        run_show();
        return;
    }
    s_cal = c;
    const esp_err_t err = store(&c);
    s_cal_stored = err == ESP_OK;
    ESP_LOGI(TAG, "touch calibrated (%s): %d %d %d / %d %d %d", err == ESP_OK ? "stored" : "not stored",
             (int)c.a, (int)c.b, (int)c.c, (int)c.d, (int)c.e, (int)c.f);
    run_end();
    touch_filter_init(&s_filter, &s_filter.cfg); /* start clean: the finger just lifted */
}

esp_err_t board_touch_calibrate(void)
{
    ESP_RETURN_ON_FALSE(s_xpt.dev != NULL, ESP_ERR_INVALID_STATE, TAG, "no touch");
    ESP_RETURN_ON_FALSE(board_ui_lock(1000), ESP_ERR_TIMEOUT, TAG, "UI lock");
    run_begin();
    board_ui_unlock();
    return ESP_OK;
}

/* ---- LVGL ----------------------------------------------------------------------------- */

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const int64_t now = esp_timer_get_time();
    if (s_last_poll_us != 0) {
        const uint32_t gap = (uint32_t)(now - s_last_poll_us);
        if (gap > s_max_gap_us) {
            s_max_gap_us = gap;
        }
    }
    s_last_poll_us = now;
    s_polls++;

    touch_raw_t raw;
    if (touch_xpt2046_read(&s_xpt, &raw) != ESP_OK) {
        s_errors++;
        memset(&raw, 0, sizeof(raw)); /* no pressure: a controller that stops answering lifts */
        s_last_raw.ok = false;
    } else {
        s_last_raw = (board_touch_raw_t){true, touch_median(raw.x, TOUCH_SAMPLES),
                                         touch_median(raw.y, TOUCH_SAMPLES), touch_pressure(&raw)};
    }

    if (s_run.active) {
        run_poll(&raw);
        data->state = LV_INDEV_STATE_RELEASED;
        s_pressed = false;
        return;
    }

    int16_t x, y;
    const bool pressed = touch_filter_step(&s_filter, &s_cal, &raw, &x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed && !s_pressed) {
        s_presses++;
        s_press_start_us = now;
    } else if (!pressed && s_pressed) {
        const uint32_t ms = (uint32_t)((now - s_press_start_us) / 1000);
        if (ms > s_longest_press_ms) {
            s_longest_press_ms = ms;
        }
    }
    s_pressed = pressed;
    /* The way back from a bad calibration: a finger held right after power-up. */
    if (pressed && now - s_started_us < RECOVERY_WINDOW_US &&
        now - s_press_start_us >= RECOVERY_HOLD_US) {
        ESP_LOGW(TAG, "finger held at start-up: calibrating again");
        s_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
        run_begin();
    }
}

void board_touch_stats(board_touch_stats_t *out, bool reset_max)
{
    *out = (board_touch_stats_t){
        .polls = s_polls,
        .presses = s_presses,
        .errors = s_errors,
        .max_gap_us = s_max_gap_us,
        .longest_press_ms = s_longest_press_ms,
        .pressed = s_pressed,
    };
    if (reset_max) {
        s_max_gap_us = 0;
        s_longest_press_ms = 0;
    }
}

esp_err_t board_touch_init(lv_display_t *disp, lv_indev_t **out_indev)
{
    const spi_host_device_t host = (spi_host_device_t)(SPI2_HOST + (BOARD_LCD_SPI_HOST - 2));
    ESP_RETURN_ON_ERROR(touch_xpt2046_open(&s_xpt, host, BOARD_TOUCH_CS_GPIO, TOUCH_XPT2046_CLOCK_HZ),
                        TAG, "XPT2046");
    load();
    const touch_filter_cfg_t cfg = {.press_min = BOARD_TOUCH_PRESS_MIN, .release_polls = 3,
                                    .dead_band = 4};
    touch_filter_init(&s_filter, &cfg);
    s_started_us = esp_timer_get_time();

    ESP_RETURN_ON_FALSE(board_ui_lock(1000), ESP_ERR_TIMEOUT, TAG, "UI lock");
    lv_indev_t *indev = lv_indev_create();
    if (indev != NULL) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read);
        lv_indev_set_display(indev, disp);
        /* A resistive film jitters: a drag starts only after 20 px. */
        lv_indev_set_scroll_limit(indev, 20);
        if (!s_cal_stored) {
            run_begin(); /* first start: nothing stored yet */
        }
    }
    board_ui_unlock();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_ERR_NO_MEM, TAG, "LVGL input");
    ESP_LOGI(TAG, "touch ready (%s calibration)", s_cal_stored ? "stored" : "default");
    if (out_indev != NULL) {
        *out_indev = indev;
    }
    return ESP_OK;
}
