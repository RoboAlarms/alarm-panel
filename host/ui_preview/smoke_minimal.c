/*
 * smoke_minimal.c - the touch UI built with every optional page left out (docs/features/33 step 6).
 *
 * The same UI logic and layout as the preview, compiled without the notifications, integrations,
 * storage and photo-saver pages (the choices a small board's feature options make), started
 * against the same simulated panel. It proves the gated build links, starts, reaches Settings
 * and About, and shows none of the missing pages. Exits non-zero on the first failed check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_panel.h"
#include "lvgl.h"
#include "ui_app.h"

#ifndef ALARM_SCREEN_W
#define ALARM_SCREEN_W 1024
#define ALARM_SCREEN_H 600
#endif
#define W ALARM_SCREEN_W
#define H ALARM_SCREEN_H

static uint16_t s_fb[W * H];
static uint32_t s_ms;
static int32_t s_px;
static int32_t s_py;
static bool s_pressed;
static int s_checks;

static uint32_t tick_cb(void)
{
    return s_ms;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_px;
    data->point.y = s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10u) {
        s_ms += 10u;
        host_panel_advance(10u);
        lv_timer_handler();
    }
}

static bool visible(lv_obj_t *o)
{
    for (; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

static lv_obj_t *find_visible(lv_obj_t *root, const char *name)
{
    const char *n = lv_obj_get_name(root);
    if (n != NULL && strcmp(n, name) == 0 && visible(root)) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = find_visible(lv_obj_get_child(root, (int32_t)i), name);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

static bool label_shown(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && visible(root) &&
        strcmp(lv_label_get_text(root), text) == 0) {
        return true;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        if (label_shown(lv_obj_get_child(root, (int32_t)i), text)) {
            return true;
        }
    }
    return false;
}

static void check(bool ok, const char *what)
{
    s_checks++;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        exit(1);
    }
}

static void tap_name(const char *name)
{
    lv_obj_t *o = find_visible(lv_screen_active(), name);
    if (o == NULL) {
        fprintf(stderr, "no visible object named '%s'\n", name);
        exit(1);
    }
    /* A finger presses only what is on the screen: a Settings section below the fold is
     * scrolled into view first, as a person would. */
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    run(60);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(60);
    s_pressed = false;
    run(300);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, pointer_read);

    host_panel_init(1789857660000LL);
    ui_backend_t be;
    host_panel_backend(&be);
    ui_app_start(&be, NULL);
    run(400);

    check(find_visible(lv_screen_active(), "bar_settings") != NULL, "Home is up");
    /* HCTL-003, HCTL-010: without the Smart Home screen built in, the rail has no Smart Home,
     * though the backend lists devices */
    check(find_visible(lv_screen_active(), "nav_devices") == NULL, "no Smart Home without its screen");
    tap_name("bar_settings");
    check(find_visible(lv_screen_active(), "set_display") != NULL, "Settings opened");
    check(find_visible(lv_screen_active(), "set_integrations") == NULL,
          "no Integrations section without integrations or notifications");
    tap_name("set_about");
    check(label_shown(lv_screen_active(), "Software update"), "About lists Software update");
    check(!label_shown(lv_screen_active(), "Storage"), "About has no Storage row");
    /* The saver never starts over Settings; from Home it comes after a minute of quiet, here
     * the plain one, with no picture to decode. */
    tap_name("nav_home");
    run(62000);
    check(find_visible(lv_screen_active(), "saver_bg") != NULL,
          "the plain screen saver shows without a picture");

    printf("%d checks passed: the UI without its optional pages\n", s_checks);
    return 0;
}
