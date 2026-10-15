/*
 * board_display.c - the E32R40T's ST7796S (320x480 SPI, used as 480x320 landscape) under LVGL.
 *
 * SPI2 at 40 MHz shared with the touch controller; esp_lvgl_port runs LVGL on its own task and
 * sends each drawn strip (5 lines, two 4.8 KB DMA buffers in internal RAM) to the panel. The
 * panel keeps its own frame memory, so there is no refresh to go wrong and nothing to flicker:
 * the only rule is the backlight's - it stays off until the UI's first full frame is in that
 * memory, which holds noise at power-up (CONTRIBUTING.md, "Display safety"; the UI turns it on
 * through board_backlight_set).
 */
#include "board.h"

#include "board_internal.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

static const char *TAG = "board_disp";

/* MIPI DCS memory access control */
#define MADCTL     0x36
#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_BGR 0x08

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static board_lcd_settings_t s_lcd = {
    .invert = BOARD_LCD_INVERT,
    .bgr = BOARD_LCD_BGR,
    .swap_xy = BOARD_LCD_SWAP_XY,
    .mirror_x = BOARD_LCD_MIRROR_X,
    .mirror_y = BOARD_LCD_MIRROR_Y,
};

static uint32_t s_flushes;
static uint32_t s_refreshes;
static uint32_t s_slow_refreshes;
static uint32_t s_max_refresh_us;
static int64_t s_refr_start_us;
static bool s_rendered;

esp_lcd_panel_handle_t board_lcd_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t board_lcd_io(void) { return s_io; }
lv_display_t *board_lcd_display(void) { return s_disp; }
const board_lcd_settings_t *board_lcd_settings(void) { return &s_lcd; }

esp_err_t board_lcd_apply(const board_lcd_settings_t *s)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "no panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, s->invert), TAG, "invert");
    /* Orientation and colour order in one register, written directly so the colour order can
     * change without making the panel again. */
    const uint8_t madctl = (uint8_t)((s->mirror_y ? MADCTL_MY : 0) | (s->mirror_x ? MADCTL_MX : 0) |
                                     (s->swap_xy ? MADCTL_MV : 0) | (s->bgr ? MADCTL_BGR : 0));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, MADCTL, &madctl, 1), TAG, "MADCTL");
    s_lcd = *s;
    if (s_disp != NULL) {
        lv_obj_invalidate(lv_screen_active()); /* draw everything again the new way */
    }
    return ESP_OK;
}

static void on_refresh_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        s_refr_start_us = esp_timer_get_time();
        s_rendered = false;
        break;
    case LV_EVENT_RENDER_START:
        s_rendered = true;
        break;
    case LV_EVENT_FLUSH_FINISH:
        s_flushes++;
        break;
    case LV_EVENT_REFR_READY:
        if (s_rendered) {
            const uint32_t us = (uint32_t)(esp_timer_get_time() - s_refr_start_us);
            s_refreshes++;
            if (us > 50000u) {
                s_slow_refreshes++;
            }
            if (us > s_max_refresh_us) {
                s_max_refresh_us = us;
            }
        }
        break;
    default:
        break;
    }
}

void board_display_stats(board_display_stats_t *out, bool reset_max)
{
    /* An SPI panel keeps its picture itself: there are no frames to lose, only redraws. */
    *out = (board_display_stats_t){
        .frames = s_refreshes,
        .flushes = s_flushes,
        .refreshes = s_refreshes,
        .slow_refreshes = s_slow_refreshes,
        .max_refresh_us = s_max_refresh_us,
        .backlight = board_backlight_requested(),
    };
    if (reset_max) {
        s_max_refresh_us = 0;
    }
}

bool board_ui_lock(uint32_t timeout_ms)
{
    return s_disp == NULL || lvgl_port_lock(timeout_ms);
}

void board_ui_unlock(void)
{
    if (s_disp != NULL) {
        lvgl_port_unlock();
    }
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    const spi_host_device_t host = (spi_host_device_t)(SPI2_HOST + (BOARD_LCD_SPI_HOST - 2));
    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK_GPIO,
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
        .miso_io_num = BOARD_LCD_MISO_GPIO, /* the touch controller answers on it */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_BUF_LINES * 2 + 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io_cfg, &s_io),
                        TAG, "panel IO");
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1, /* the panel's reset is the chip's EN: a software reset here */
        .rgb_ele_order = s_lcd.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io, &dev_cfg, &s_panel), TAG, "ST7796");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(board_lcd_apply(&s_lcd), TAG, "panel settings");
    /* On, while the backlight is still off: what the panel shows now is noise nobody sees. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 6 * 1024, /* the status screen and the calibration; the full UI needs more */
        .task_affinity = 1, /* Wi-Fi keeps core 0 */
        .task_max_sleep_ms = 50,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .control_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* The orientation is already in MADCTL (board_lcd_apply); the port leaves it alone. */
        .rotation = {.swap_xy = s_lcd.swap_xy, .mirror_x = s_lcd.mirror_x, .mirror_y = s_lcd.mirror_y},
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true, /* RGB565 goes out on SPI most significant byte first */
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "LVGL display");
    if (lvgl_port_lock(0)) {
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_REFR_READY, NULL);
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_FLUSH_FINISH, NULL);
        lvgl_port_unlock();
    }
    s_disp = disp;
    /* The port applied its own view of the orientation; ours (with the colour order) wins. */
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_ERR_TIMEOUT, TAG, "LVGL lock");
    const esp_err_t applied = board_lcd_apply(&s_lcd);
    lvgl_port_unlock();
    ESP_RETURN_ON_ERROR(applied, TAG, "panel settings");
    ESP_LOGI(TAG, "display %dx%d ready (SPI %d MHz, %d-line buffers)", BOARD_LCD_H_RES,
             BOARD_LCD_V_RES, BOARD_LCD_PCLK_HZ / 1000000, BOARD_LCD_BUF_LINES);
    *out_disp = disp;
    return ESP_OK;
}
