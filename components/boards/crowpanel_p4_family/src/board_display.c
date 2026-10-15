/*
 * board_display.c - MIPI-DSI bus, EK79007 panel and LVGL registration, and the refresh
 * monitor behind the flicker guard.
 * Timings from Elecrow's validated V1.1 example, adapted to the ESP-IDF 6.x DPI API.
 *
 * The DPI driver restarts the frame DMA from an interrupt after every frame. When that
 * interrupt can't run in time (the flash cache is off during flash writes unless
 * CONFIG_LCD_DSI_ISR_CACHE_SAFE), the picture drops out; repeated flash writes then look like
 * a strobe. The monitor times every frame; the guard turns the backlight off at the first
 * late frame (photosensitivity safety, docs/features/06 P4-012).
 *
 * The panel is started and stopped in the order its datasheet gives (docs/bugs/B023): a hardware
 * reset once the DSI lanes are idle, the backlight only over a picture drawn since it was dark
 * and after 200 ms of video, and black, sleep and reset before a planned restart. Its glass
 * otherwise keeps a ghost of an earlier picture for minutes after a power-on.
 */
#include "crowpanel_p4.h"
#include <string.h>

#include "board_internal.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_private.h" /* lv_display_t's buf_1/buf_2 */
#include "sdkconfig.h"

static const char *TAG = "board_disp";

#define LCD_HBP 160
#define LCD_HPW 70
#define LCD_HFP 160
#define LCD_VBP 23
#define LCD_VPW 10
#define LCD_VFP 12
/* One frame: (1024 + 390) x (600 + 45) pixels at 51 MHz = 17.9 ms. */
#define FRAME_US                                                                              \
    ((uint32_t)((BOARD_LCD_H_RES + LCD_HBP + LCD_HPW + LCD_HFP) *                             \
                (BOARD_LCD_V_RES + LCD_VBP + LCD_VPW + LCD_VFP) / BOARD_LCD_DPI_CLOCK_MHZ))
#define LATE_US      (2u * FRAME_US) /* a frame this late means at least one was lost */
#define STALL_US     100000          /* no frame at all for this long: stalled */
#define TICK_US      20000
#define FB_BYTES     ((size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t))

/* The panel's power sequence (EK79007 datasheet 10.1 and 14.1; docs/bugs/B023). */
/* GRB low: at least 1 ms (TGRB). Kept short, and busy-waited: a CPU-only reset while it is low
 * keeps the pin low, and an older firmware after a rollback never drives it. */
#define RESET_LOW_US      2000
#define RESET_WAIT_MS     60     /* GRB high before the first command */
#define VIDEO_FIRST_US    200000 /* video this long before the backlight first comes on */
#define REVEAL_FRAMES     3u     /* after a redraw: its last copy, then two whole frames */
#define REVEAL_IDLE_US    100000 /* nothing redrawn in this long: what is shown is current */
#define REVEAL_MAX_US     500000 /* the backlight never waits longer */
#define SLEEP_IN_MS       120    /* frames keep coming while the panel goes to sleep */

static lv_display_t *s_disp;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb; /* the DPI frame buffer: what the panel shows */
static volatile uint32_t s_flushes;
#if CONFIG_BOARD_FRAME_CAPTURE
static uint32_t s_dev_flush_target; /* UI lock; s_flushes is the DMA completion counter */
bool board_dev_frame(uint16_t *out, size_t count)
{
    if (!s_fb || s_dev_flush_target != s_flushes ||
        (count != (BOARD_LCD_H_RES/2)*(BOARD_LCD_V_RES/2) &&
         count != BOARD_LCD_H_RES*BOARD_LCD_V_RES)) return false;
    /* DMA2D writes the frame buffer past the CPU cache: read what is in memory. */
    esp_cache_msync(s_fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (count == BOARD_LCD_H_RES*BOARD_LCD_V_RES) {
        memcpy(out, s_fb, count*sizeof(*out));
        return true;
    }
    for (size_t y=0; y<BOARD_LCD_V_RES/2; y++)
        for (size_t x=0; x<BOARD_LCD_H_RES/2; x++)
            out[y*(BOARD_LCD_H_RES/2)+x]=s_fb[(y*2)*BOARD_LCD_H_RES+x*2];
    return true;
}
#endif
static volatile uint32_t s_frames;
static volatile uint32_t s_late;
static volatile uint32_t s_max_gap_us;
static volatile int64_t s_last_frame_us;
static bool s_guard_on;
static int64_t s_refr_start_us;
static bool s_rendered;
static volatile bool s_in_refresh;        /* LVGL is in a refresh pass */
static volatile uint32_t s_render_starts; /* refreshes that drew something, begun */
static volatile uint32_t s_refreshes;     /* ... and finished */
static uint32_t s_slow_refreshes;
static uint32_t s_max_refresh_us;
static uint32_t s_guard_trips;
static uint32_t s_late_seen;
static int64_t s_guard_until_us;
static int64_t s_video_us; /* when video started */

/* The backlight waiting to come on from dark (board_backlight_reveal). */
static portMUX_TYPE s_reveal_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_reveal_wanted;
static uint32_t s_reveal_id;     /* which request: an answer to an older one lights nothing */
static int64_t s_reveal_at_us;
static uint32_t s_reveal_starts; /* s_render_starts when asked */
static bool s_reveal_counting;   /* a redraw since has finished: counting frames */
static uint32_t s_reveal_frame;  /* the frame to light at */

/* The DSI PHY PLL reference differs by silicon: v1.x only has the legacy PLL_F20M source,
 * v3.x defaults to the crystal. */
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define BOARD_DSI_PHY_CLK MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY
#else
#define BOARD_DSI_PHY_CLK MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT
#endif

/* ---- refresh monitor and flicker guard ------------------------------------------------ */

/* The frame DMA finished and was restarted (DSI DMA interrupt; IRAM-safe with
 * CONFIG_LCD_DSI_ISR_CACHE_SAFE: no flash code, no flash data). */
static bool IRAM_ATTR on_frame_done(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    (void)panel;
    (void)edata;
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    if (s_last_frame_us != 0) {
        const uint32_t gap = (uint32_t)(now - s_last_frame_us);
        if (gap > s_max_gap_us) {
            s_max_gap_us = gap;
        }
        if (gap > LATE_US) {
            s_late = s_late + 1u;
        }
    }
    s_last_frame_us = now;
    s_frames = s_frames + 1u;
    return false;
}

/* A draw into the frame buffer finished: what esp_lvgl_port's own callback does. It runs from
 * the DMA2D interrupt, which is not cache-safe, so it never runs during a flash write. */
static bool IRAM_ATTR on_color_done(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    (void)panel;
    (void)edata;
    (void)ctx;
    s_flushes = s_flushes + 1u;
#if CONFIG_BOARD_DISPLAY_DIAG
    board_display_diag_flush_done();
#endif
    lv_display_flush_ready(s_disp);
    return false;
}

/* How long LVGL spends on a refresh that draws something (LVGL task). */
static void on_refresh_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
#if CONFIG_BOARD_FRAME_CAPTURE
    case LV_EVENT_FLUSH_START:
        s_dev_flush_target = s_flushes + 1u;
        break;
#endif
    case LV_EVENT_REFR_START:
        s_refr_start_us = esp_timer_get_time();
        s_rendered = false;
        s_in_refresh = true;
        break;
    case LV_EVENT_RENDER_START:
        s_rendered = true;
        s_render_starts = s_render_starts + 1u;
        break;
    case LV_EVENT_REFR_READY:
        s_in_refresh = false;
        if (s_rendered) {
            const uint32_t us = (uint32_t)(esp_timer_get_time() - s_refr_start_us);
            s_refreshes = s_refreshes + 1u;
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

#if CONFIG_BOARD_FLICKER_GUARD
static void guard_check(int64_t now)
{
    const uint32_t late = s_late;
    const int64_t last = s_last_frame_us;
    const bool stalled = last != 0 && now - last > STALL_US;
    if (late != s_late_seen || stalled) {
        s_late_seen = late;
        s_guard_until_us = now + (int64_t)CONFIG_BOARD_FLICKER_GUARD_HOLD_S * 1000000;
        if (!s_guard_on) {
            s_guard_on = true;
            s_guard_trips++;
            board_backlight_hold_off(true);
            ESP_LOGW(TAG, "display refresh unsteady (%s): backlight off", stalled ? "stalled" : "late frame");
        }
    } else if (s_guard_on && now >= s_guard_until_us) {
        s_guard_on = false;
        board_backlight_hold_off(false);
        ESP_LOGI(TAG, "display refresh steady for %d s: backlight on",
                 CONFIG_BOARD_FLICKER_GUARD_HOLD_S);
    }
}
#endif

uint32_t board_display_reveal_request(void)
{
    taskENTER_CRITICAL(&s_reveal_mux);
    const uint32_t id = ++s_reveal_id;
    s_reveal_wanted = true;
    s_reveal_at_us = esp_timer_get_time();
    s_reveal_starts = s_render_starts;
    s_reveal_counting = false;
    taskEXIT_CRITICAL(&s_reveal_mux);
    return id;
}

/* Nothing for LVGL to draw: no pass under way, and its refresh timer paused, which LVGL does
 * after every pass and undoes at any invalidation or change of layout. */
static bool lvgl_idle(void)
{
    return !s_in_refresh && s_disp != NULL && s_disp->refr_timer != NULL &&
           lv_timer_get_paused(s_disp->refr_timer);
}

/* Lights the backlight once the panel shows a picture drawn since it was asked for: a redraw
 * begun since has finished and REVEAL_FRAMES frames have gone out, or nothing was redrawn and
 * none is due (the picture shown is current). Never before VIDEO_FIRST_US of video. */
static void reveal_check(int64_t now)
{
    bool go = false;
    int64_t waited = 0;
    uint32_t id = 0;
    const char *why = "waited too long";
    const bool idle = lvgl_idle();
    taskENTER_CRITICAL(&s_reveal_mux);
    if (s_reveal_wanted && s_video_us != 0 && now - s_video_us >= VIDEO_FIRST_US) {
        waited = now - s_reveal_at_us;
        const uint32_t starts = s_render_starts;
        if (starts != s_reveal_starts && s_refreshes == starts) {
            if (!s_reveal_counting) {
                s_reveal_counting = true;
                s_reveal_frame = s_frames + REVEAL_FRAMES;
            } else if ((int32_t)(s_frames - s_reveal_frame) >= 0) {
                go = true;
                why = "redrawn";
            }
        } else if (starts == s_reveal_starts && s_refreshes == starts && idle &&
                   waited >= REVEAL_IDLE_US) {
            go = true;
            why = "nothing to redraw";
        }
        go = go || waited >= REVEAL_MAX_US;
        s_reveal_wanted = !go;
        id = s_reveal_id;
    }
    taskEXIT_CRITICAL(&s_reveal_mux);
    if (go) {
#if CONFIG_BOARD_DISPLAY_DIAG
        ESP_LOGI(TAG, "backlight on: %s, after %d ms", why, (int)(waited / 1000));
#else
        (void)why;
#endif
        board_backlight_reveal(id);
    }
}

static void display_tick(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
#if CONFIG_BOARD_FLICKER_GUARD
    guard_check(now);
#endif
    reveal_check(now);
}

/* Before a planned restart (an update, a restore, the console's restart; a panic skips this):
 * the panel goes dark and to sleep in its datasheet's order - backlight off, black, sleep-in
 * while frames still come, then a reset - so no picture is left on the glass while the P4
 * restarts (docs/bugs/B023). GRB ends high: the restart keeps GPIO states, and a firmware that
 * never drives it (an older one, after a rollback) must find the panel out of reset. The UI lock
 * is kept: nothing draws again. It waits up to a second for it: a pass that holds it longer
 * (a page rebuilt in another language) would draw over the black. */
static void display_shutdown(void)
{
    ESP_LOGI(TAG, "restart: panel to sleep");
    board_backlight_shutdown();
    const bool locked = board_ui_lock(1000);
    if (!locked) {
        ESP_LOGW(TAG, "restart: the UI is busy; the panel sleeps anyway");
    }
    for (int i = 0; i < 20 && s_disp != NULL && s_disp->flushing; i++) {
        vTaskDelay(pdMS_TO_TICKS(5)); /* the last copy into the frame buffer */
    }
    if (s_fb != NULL) {
        memset(s_fb, 0, FB_BYTES);
        esp_cache_msync(s_fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    const int64_t last = s_last_frame_us;
    if (s_io != NULL && last != 0 && esp_timer_get_time() - last < STALL_US) {
        vTaskDelay(pdMS_TO_TICKS(3u * FRAME_US / 1000u + 1u)); /* black on the glass */
        esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPIN, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(SLEEP_IN_MS));
    }
    gpio_set_level(BOARD_LCD_RESET_GPIO, 0);
    esp_rom_delay_us(RESET_LOW_US);
    gpio_set_level(BOARD_LCD_RESET_GPIO, 1);
}

void board_display_stats(board_display_stats_t *out, bool reset_max)
{
    out->frames = s_frames;
    out->flushes = s_flushes;
    out->late = s_late;
    out->max_gap_us = s_max_gap_us;
    out->refreshes = s_refreshes;
    out->slow_refreshes = s_slow_refreshes;
    out->max_refresh_us = s_max_refresh_us;
    if (reset_max) {
        s_max_gap_us = 0;
        s_max_refresh_us = 0;
    }
    out->guard_on = s_guard_on;
    out->guard_trips = s_guard_trips;
    out->backlight = board_backlight_requested();
#if CONFIG_BOARD_BACKLIGHT_FORCE_OFF
    out->forced_off = true;
#else
    out->forced_off = false;
#endif
}

/* ---- display --------------------------------------------------------------------------- */

/* Clears both draw buffers before anything is drawn (docs/bugs/B022). A reset leaves PSRAM as
 * it was (the boot test writes one word in eight), so after an install over another firmware
 * the buffers hold its pictures; LVGL doesn't clear an RGB565 buffer before drawing, and a pixel
 * no object paints would show them, flickering as the two buffers take turns. */
static void clear_draw_buffers(lv_display_t *disp)
{
    lv_draw_buf_t *const bufs[] = {disp->buf_1, disp->buf_2};
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
        if (bufs[i] != NULL && bufs[i]->data != NULL) {
            memset(bufs[i]->data, 0, bufs[i]->data_size);
        }
    }
}

bool board_ui_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void board_ui_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = BOARD_LCD_DSI_LANES,
        .phy_clk_src = BOARD_DSI_PHY_CLK,
        .lane_bit_rate_mbps = BOARD_LCD_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "DSI bus");
    DISPLAY_DIAG_STEP("DSI bus up");

    /* The hardware reset, now that the lanes are idle (LP11): the EK79007 wants them stopped
     * before GRB rises (datasheet 10.1). Its pull-up on LDO4's rail released it before the lanes
     * existed, and a P4 restart, which resets only the CPUs, didn't reset the panel at all. */
    const gpio_config_t grb = {
        .pin_bit_mask = 1ULL << BOARD_LCD_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(BOARD_LCD_RESET_GPIO, 0);
    ESP_RETURN_ON_ERROR(gpio_config(&grb), TAG, "LCD reset");
    gpio_set_level(BOARD_LCD_RESET_GPIO, 0);
    esp_rom_delay_us(RESET_LOW_US);
    gpio_set_level(BOARD_LCD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS));
    DISPLAY_DIAG_STEP("panel reset");

    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io), TAG, "DBI io");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_LCD_DPI_CLOCK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_back_porch = LCD_HBP,
            .hsync_pulse_width = LCD_HPW,
            .hsync_front_porch = LCD_HFP,
            .vsync_back_porch = LCD_VBP,
            .vsync_pulse_width = LCD_VPW,
            .vsync_front_porch = LCD_VFP,
        },
    };
    ek79007_vendor_config_t vendor = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = BOARD_LCD_DSI_LANES,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, /* reset above; the driver's own reset is a software one */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(dbi_io, &panel_cfg, &panel), TAG, "EK79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    DISPLAY_DIAG_STEP("software reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    s_video_us = esp_timer_get_time();
    DISPLAY_DIAG_STEP("panel init, video on");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **)&s_fb), TAG,
                        "frame buffer");
    s_io = dbi_io;
    if (esp_lcd_dpi_panel_enable_dma2d(panel) != ESP_OK) {
        ESP_LOGW(TAG, "DMA2D not available; using CPU copies");
    }

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 16 * 1024,
        .task_affinity = -1,
        .task_max_sleep_ms = 20,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = dbi_io,
        .panel_handle = panel,
        .control_handle = panel,
        /* Whole-screen draw buffers (PSRAM). Measured 2026-09-20: 64-line buffers were not faster
         * (a full redraw is about 60 ms either way) and much slower during flash writes; two
         * render threads were slower too. */
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
            .swap_bytes = false, /* DPI frame buffer is native little-endian RGB565 */
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {.avoid_tearing = false},
    };
    /* With CONFIG_LCD_DSI_ISR_CACHE_SAFE the driver refuses esp_lvgl_port's own callbacks (not
     * in IRAM) and logs an error; ours replace them below, so that one error is hidden. */
    const esp_log_level_t dsi_level = esp_log_level_get("lcd.dsi");
    esp_log_level_set("lcd.dsi", ESP_LOG_NONE);
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    esp_log_level_set("lcd.dsi", dsi_level);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "LVGL display");

    /* Our callbacks replace esp_lvgl_port's (it can't register non-IRAM ones when the DSI
     * interrupt is cache-safe): the same flush-ready plus the frame monitor. */
    s_disp = disp;
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_done,
        .on_frame_buf_complete = on_frame_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL), TAG,
                        "DPI callbacks");
    if (lvgl_port_lock(0)) {
        clear_draw_buffers(disp); /* the backlight is still off */
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_REFR_READY, NULL);
#if CONFIG_BOARD_FRAME_CAPTURE
        lv_display_add_event_cb(disp, on_refresh_event, LV_EVENT_FLUSH_START, NULL);
#endif
#if CONFIG_BOARD_DISPLAY_DIAG
        board_display_diag_start(disp, s_fb);
#endif
        lvgl_port_unlock();
    }
    const esp_timer_create_args_t tick = {.callback = display_tick, .name = "display"};
    esp_timer_handle_t timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick, &timer), TAG, "display timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, TICK_US), TAG, "display timer start");
    if (esp_register_shutdown_handler(display_shutdown) != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown handler: the panel won't sleep before a restart");
    }
    ESP_LOGI(TAG, "display %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    *out_disp = disp;
    return ESP_OK;
}
