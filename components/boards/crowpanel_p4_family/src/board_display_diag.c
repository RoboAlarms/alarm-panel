/*
 * board_display_diag.c - display self-check for investigation builds (docs/bugs/B023):
 * whether the frame buffer holds exactly what LVGL drew, whether LVGL paints every pixel it
 * flushes, and when after start-up anything differs. CONFIG_BOARD_DISPLAY_DIAG only.
 *
 * Every flush is compared with the frame buffer at the start of the next one: LVGL has waited
 * for its copy by then (double buffering) and hasn't drawn into its buffer again yet. Before an
 * area is drawn, its draw buffer is filled with a position-keyed marker; a marker pixel still
 * there at the flush is one no object painted.
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_DISPLAY_DIAG
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "crowpanel_p4.h"
#include "board_internal.h"
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"
#include "lvgl_private.h" /* lv_display_t's buffers */
#include "soc/mipi_dsi_host_struct.h"

static const char *TAG = "disp_diag";

static uint16_t *s_fb;
static lv_display_t *s_disp;
static temperature_sensor_handle_t s_tsens;
static bool s_fb_aligned;

/* The previous flush: its area and the draw buffer it was copied from. */
static lv_area_t s_prev_area;
static const uint16_t *s_prev_px;
static uint32_t s_prev_stride_px;
static bool s_prev_valid;

static struct {
    uint32_t flushes;
    uint32_t checked;
    uint32_t bad;
    uint64_t bad_px;
    uint32_t unpainted;
    uint64_t unpainted_px;
    int64_t last_bad_us;
    lv_area_t last_bad_area;
    uint32_t last_bad_px;
    int32_t last_bad_x;
    int32_t last_bad_y;
    uint16_t last_want;
    uint16_t last_got;
    int64_t last_unpainted_us;
    lv_area_t last_unpainted_area;
    uint32_t last_unpainted_px;
    int64_t first_flush_us;
    int64_t first_lit_us;
    /* The DSI host's error flags (clear on read), polled each second. */
    uint32_t link_polls_bad;
    uint32_t link_st0;
    uint32_t link_st1;
    int64_t last_link_us;
} s_c;

static inline uint16_t marker(uint32_t i)
{
    return (uint16_t)((i * 2654435761u) >> 16);
}

static void fill_marker(lv_draw_buf_t *buf)
{
    if (buf == NULL || buf->data == NULL) {
        return;
    }
    uint16_t *p = (uint16_t *)buf->data;
    const uint32_t n = buf->data_size / 2u;
    for (uint32_t i = 0; i < n; i++) {
        p[i] = marker(i);
    }
}

static lv_draw_buf_t *other_buf(lv_display_t *d)
{
    return d->buf_act == d->buf_1 ? d->buf_2 : d->buf_1;
}

/* The previous flush against the frame buffer (its copy has finished). */
static void check_prev(void)
{
    if (!s_prev_valid || s_fb == NULL) {
        return;
    }
    s_prev_valid = false;
    const lv_area_t a = s_prev_area;
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (a.x1 < 0 || a.y1 < 0 || a.x2 >= BOARD_LCD_H_RES || a.y2 >= BOARD_LCD_V_RES || w <= 0 ||
        h <= 0) {
        return;
    }
    if (s_fb_aligned) {
        esp_cache_msync(s_fb + (size_t)a.y1 * BOARD_LCD_H_RES,
                        (size_t)h * BOARD_LCD_H_RES * sizeof(uint16_t),
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    }
    uint32_t bad = 0;
    int32_t bx = -1;
    int32_t by = -1;
    uint16_t want = 0;
    uint16_t got = 0;
    for (int32_t r = 0; r < h; r++) {
        const uint16_t *src = s_prev_px + (size_t)r * s_prev_stride_px;
        const uint16_t *dst = s_fb + (size_t)(a.y1 + r) * BOARD_LCD_H_RES + a.x1;
        if (memcmp(src, dst, (size_t)w * sizeof(uint16_t)) == 0) {
            continue;
        }
        for (int32_t x = 0; x < w; x++) {
            if (src[x] != dst[x]) {
                if (bad == 0u) {
                    bx = a.x1 + x;
                    by = a.y1 + r;
                    want = src[x];
                    got = dst[x];
                }
                bad++;
            }
        }
    }
    s_c.checked++;
    if (bad != 0u) {
        s_c.bad++;
        s_c.bad_px += bad;
        s_c.last_bad_us = esp_timer_get_time();
        s_c.last_bad_area = a;
        s_c.last_bad_px = bad;
        s_c.last_bad_x = bx;
        s_c.last_bad_y = by;
        s_c.last_want = want;
        s_c.last_got = got;
        ESP_LOGW(TAG, "frame buffer differs from the draw buffer: %" PRIu32 " px in (%d,%d)-(%d,%d), "
                 "first at (%d,%d) want %04x got %04x, at %.1f s",
                 bad, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, (int)bx, (int)by, want, got,
                 (double)s_c.last_bad_us / 1e6);
    }
}

/* Pixels of the area about to be flushed that still hold the marker. */
static void check_unpainted(lv_display_t *d, const lv_area_t *a)
{
    lv_draw_buf_t *buf = d->buf_act;
    if (buf == NULL || buf->data == NULL) {
        return;
    }
    const int32_t w = lv_area_get_width(a);
    const int32_t h = lv_area_get_height(a);
    const uint32_t stride_px = buf->header.stride / 2u;
    const uint16_t *p = (const uint16_t *)buf->data;
    uint32_t n = 0;
    for (int32_t r = 0; r < h; r++) {
        const uint32_t row = (uint32_t)r * stride_px;
        for (int32_t x = 0; x < w; x++) {
            if (p[row + (uint32_t)x] == marker(row + (uint32_t)x)) {
                n++;
            }
        }
    }
    /* One pixel in 65536 matches by chance: count an area only well above that. */
    const uint32_t chance = (uint32_t)((uint64_t)w * (uint64_t)h / 8192u) + 4u;
    if (n > chance) {
        s_c.unpainted++;
        s_c.unpainted_px += n;
        s_c.last_unpainted_us = esp_timer_get_time();
        s_c.last_unpainted_area = *a;
        s_c.last_unpainted_px = n;
        ESP_LOGW(TAG, "%" PRIu32 " px not painted in (%d,%d)-(%d,%d) at %.1f s", n, (int)a->x1,
                 (int)a->y1, (int)a->x2, (int)a->y2, (double)s_c.last_unpainted_us / 1e6);
    }
}

static void on_event(lv_event_t *e)
{
    lv_display_t *d = lv_event_get_target(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        fill_marker(d->buf_act);
        break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *a = lv_event_get_param(e);
        s_c.flushes++;
        check_prev();
        check_unpainted(d, a);
        s_prev_area = *a;
        s_prev_px = (const uint16_t *)d->buf_act->data;
        s_prev_stride_px = d->buf_act->header.stride / 2u;
        s_prev_valid = true;
        break;
    }
    case LV_EVENT_FLUSH_FINISH:
        /* The next area is drawn into the other buffer; its copy finished before this flush. */
        fill_marker(other_buf(d));
        break;
    default:
        break;
    }
}

void IRAM_ATTR board_display_diag_flush_done(void)
{
    if (s_c.first_flush_us == 0) {
        s_c.first_flush_us = esp_timer_get_time();
    }
}

void board_display_diag_lit(void)
{
    if (s_c.first_lit_us == 0) {
        s_c.first_lit_us = esp_timer_get_time();
    }
}

/* Link errors since the last poll: acknowledge errors reported by the panel, D-PHY contention,
 * timeouts, ECC/CRC errors and the DPI payload buffer running dry. */
static void poll_link(void)
{
    const uint32_t st0 = MIPI_DSI_HOST.int_st0.val;
    const uint32_t st1 = MIPI_DSI_HOST.int_st1.val;
    if ((st0 | st1) == 0u) {
        return;
    }
    s_c.link_polls_bad++;
    s_c.link_st0 |= st0;
    s_c.link_st1 |= st1;
    s_c.last_link_us = esp_timer_get_time();
    ESP_LOGW(TAG, "DSI link errors: int_st0 %08" PRIx32 " int_st1 %08" PRIx32 " at %.1f s", st0, st1,
             (double)s_c.last_link_us / 1e6);
}

void board_display_diag_link_step(const char *step)
{
    const uint32_t st0 = MIPI_DSI_HOST.int_st0.val;
    const uint32_t st1 = MIPI_DSI_HOST.int_st1.val;
    if ((st0 | st1) != 0u) {
        s_c.link_polls_bad++;
        s_c.link_st0 |= st0;
        s_c.link_st1 |= st1;
        s_c.last_link_us = esp_timer_get_time();
    }
    ESP_LOGI(TAG, "%s: int_st0 %08" PRIx32 " int_st1 %08" PRIx32, step, st0, st1);
}

static int temp_tenths(void)
{
    float t = 0.0f;
    if (s_tsens == NULL || temperature_sensor_get_celsius(s_tsens, &t) != ESP_OK) {
        return -9999;
    }
    return (int)(t * 10.0f);
}

void board_display_diag_print(void)
{
    board_display_stats_t st;
    board_display_stats(&st, false);
    const int t = temp_tenths();
    printf("uptime %.1f s, chip %d.%d C, reset reason %d\n", (double)esp_timer_get_time() / 1e6,
           t / 10, (t < 0 ? -t : t) % 10, (int)esp_reset_reason());
    printf("first copy done at %.3f s, backlight first lit at %.3f s\n",
           (double)s_c.first_flush_us / 1e6, (double)s_c.first_lit_us / 1e6);
    printf("flushes %" PRIu32 ", compared %" PRIu32 ", differing %" PRIu32 " (%" PRIu64 " px)\n",
           s_c.flushes, s_c.checked, s_c.bad, s_c.bad_px);
    if (s_c.bad != 0u) {
        printf("  last at %.1f s: %" PRIu32 " px in (%d,%d)-(%d,%d), first (%d,%d) want %04x got "
               "%04x\n",
               (double)s_c.last_bad_us / 1e6, s_c.last_bad_px, (int)s_c.last_bad_area.x1,
               (int)s_c.last_bad_area.y1, (int)s_c.last_bad_area.x2, (int)s_c.last_bad_area.y2,
               (int)s_c.last_bad_x, (int)s_c.last_bad_y, s_c.last_want, s_c.last_got);
    }
    printf("areas with unpainted pixels %" PRIu32 " (%" PRIu64 " px)\n", s_c.unpainted,
           s_c.unpainted_px);
    if (s_c.unpainted != 0u) {
        printf("  last at %.1f s: %" PRIu32 " px in (%d,%d)-(%d,%d)\n",
               (double)s_c.last_unpainted_us / 1e6, s_c.last_unpainted_px,
               (int)s_c.last_unpainted_area.x1, (int)s_c.last_unpainted_area.y1,
               (int)s_c.last_unpainted_area.x2, (int)s_c.last_unpainted_area.y2);
    }
    printf("frames %" PRIu32 ", late %" PRIu32 ", guard trips %" PRIu32 ", backlight %u %%\n",
           st.frames, st.late, st.guard_trips, (unsigned)st.backlight);
    poll_link();
    printf("DSI link: seconds with errors %" PRIu32 ", int_st0 %08" PRIx32 ", int_st1 %08" PRIx32
           ", last at %.1f s\n",
           s_c.link_polls_bad, s_c.link_st0, s_c.link_st1, (double)s_c.last_link_us / 1e6);
}

void board_display_diag_dark(uint32_t ms, bool redraw)
{
    const uint8_t level = board_backlight_requested();
    board_backlight_set(0);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ESP_LOGI(TAG, "backlight back to %u %%%s", (unsigned)level, redraw ? ", screen redrawn" : "");
    /* As the UI does on waking: the level asked for in the LVGL task, before the redraw. */
    if (redraw && board_ui_lock(1000)) {
        lv_obj_invalidate(lv_screen_active());
        board_backlight_set(level);
        board_ui_unlock();
    } else {
        board_backlight_set(level);
    }
}

/* Polls the link each second; a line every 30 s for the first 30 minutes, then every 5 minutes. */
static void report_task(void *arg)
{
    (void)arg;
    poll_link(); /* whatever start-up left behind */
    int64_t next = esp_timer_get_time() + 30LL * 1000000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        poll_link();
        const int64_t now = esp_timer_get_time();
        if (now < next) {
            continue;
        }
        next = now + (now < 1800LL * 1000000 ? 30LL : 300LL) * 1000000;
        board_display_stats_t st;
        board_display_stats(&st, false);
        const int t = temp_tenths();
        ESP_LOGI(TAG, "up %" PRId64 " s, %d.%d C: compared %" PRIu32 ", differing %" PRIu32
                 " (%" PRIu64 " px), unpainted areas %" PRIu32 ", late frames %" PRIu32
                 ", link errors %" PRIu32,
                 now / 1000000, t / 10, (t < 0 ? -t : t) % 10, s_c.checked, s_c.bad, s_c.bad_px,
                 s_c.unpainted, st.late, s_c.link_polls_bad);
    }
}

void board_display_diag_start(lv_display_t *disp, void *fb)
{
    s_disp = disp;
    s_fb = (uint16_t *)fb;
    s_fb_aligned = ((uintptr_t)fb % 128u) == 0u;
    if (!s_fb_aligned) {
        ESP_LOGW(TAG, "frame buffer not cache-line aligned: compared without invalidating");
    }
    const temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }
    lv_display_add_event_cb(disp, on_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, on_event, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, on_event, LV_EVENT_FLUSH_FINISH, NULL);
    xTaskCreateWithCaps(report_task, "disp_diag", 4096, NULL, 1, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "display self-check on (investigation build)");
}
#endif
