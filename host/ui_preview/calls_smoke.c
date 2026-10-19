/*
 * calls_smoke.c - the screen's calls that wait for another panel (LINK-054): the real touch UI
 * against the simulated panel, with a backend whose run_later holds each call until the test lets
 * its "master" answer, as a keypad's link does for up to 3 s.
 *
 * It proves that a code typed on Home's pad is handed over and the pad doesn't close before the
 * answer; that the screen goes on meanwhile and a tap does nothing; that the answer, once in,
 * disarms and closes the pad; that a call no one waits for takes no taps and a person's call goes
 * before it; that a fifth waiting call is refused; and that without run_later a call and its
 * continuation run at once, as on the alarm panel itself. Exits non-zero on the first failed check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_panel.h"
#include "lvgl.h"
#include "ui_app.h"
#include "ui_internal.h"

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
static ui_backend_t s_be;

/* The calls handed over and not yet answered, oldest first. */
#define HELD_MAX 8
static struct {
    void (*work)(void *arg);
    void *arg;
} s_held[HELD_MAX];
static int s_nheld;
static bool s_later = true; /* run_later takes calls (false: it says no, the UI runs them) */

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

static void check(bool ok, const char *what)
{
    s_checks++;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        exit(1);
    }
}

static bool held_run_later(void *ctx, void (*work)(void *arg), void *arg)
{
    (void)ctx;
    if (!s_later || s_nheld >= HELD_MAX) {
        return false;
    }
    s_held[s_nheld].work = work;
    s_held[s_nheld].arg = arg;
    s_nheld++;
    return true;
}

/* The master answers the oldest call (on this thread: the work touches no LVGL). */
static void answer_one(void)
{
    if (s_nheld == 0) {
        return;
    }
    s_held[0].work(s_held[0].arg);
    memmove(&s_held[0], &s_held[1], (size_t)(s_nheld - 1) * sizeof(s_held[0]));
    s_nheld--;
}

static alarm_arm_state_t state(void)
{
    alarm_snapshot_t snap;
    s_be.get_snapshot(s_be.ctx, &snap);
    return snap.part[0].state;
}

static lv_obj_t *shield(void)
{
    lv_obj_t *top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
        lv_obj_t *o = lv_obj_get_child(top, (int32_t)i);
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_width(o) == W &&
            lv_obj_get_height(o) == H && lv_obj_get_style_bg_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP) {
            return o;
        }
    }
    return NULL;
}

static void tap(int32_t x, int32_t y)
{
    s_px = x;
    s_py = y;
    s_pressed = true;
    run(60);
    s_pressed = false;
    run(100);
}

static int s_done;
static uintptr_t s_order[4];
static int s_norder;

static void counted(const ui_call_t *c)
{
    s_done++;
    if (s_norder < 4) {
        s_order[s_norder++] = c->tag;
    }
}

static void type_code(const char *code)
{
    ui_app_remote_text(code);
    ui_app_remote_key(UI_REMOTE_KEY_ENTER);
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
    host_panel_backend(&s_be);
    s_be.run_later = held_run_later;
    ui_app_start(&s_be, NULL);
    run(400);
    /* the code is typed through the remote keypad's entries, as the Panel Manager does */
    const ui_remote_session_t session = {.active = true, .who = "Alex"};
    ui_app_remote_session(&session);
    run(100);

    alarm_cmd_t arm;
    memset(&arm, 0, sizeof(arm));
    arm.type = ALARM_CMD_ARM;
    arm.level = ALARM_LEVEL_AWAY;
    arm.partition = 1;
    snprintf(arm.code, sizeof(arm.code), "123456"); /* the factory master code */
    check(host_panel_remote_command(&arm) == ALARM_OK, "armed Away from elsewhere");
    run(500);
    check(state() != ALARM_STATE_DISARMED, "the partition is arming");

    /* LINK-054: the disarm is handed over; nothing is decided until the master answers */
    type_code("123456");
    run(50);
    check(s_nheld == 1, "the disarm was handed to run_later");
    check(state() != ALARM_STATE_DISARMED, "...and nothing ran on the screen's task");
    check(ui_call_waiting(), "a person's call waits");
    check(shield() != NULL, "a layer that draws nothing takes the taps");
    const uint32_t before = s_ms;
    run(2000); /* 2 s without an answer: the screen's timers go on */
    check(s_ms - before == 2000u && s_nheld == 1, "the screen ran 2 s while the call waited");
    tap(W / 2, H / 2);
    check(s_nheld == 1, "a tap while it waits starts nothing");

    answer_one();
    check(state() == ALARM_STATE_DISARMED, "the master's answer disarmed");
    check(ui_call_waiting(), "...and the screen hasn't picked it up yet");
    run(30);
    check(!ui_call_waiting(), "the continuation ran on the screen's task");
    check(shield() == NULL, "the layer went with it");

    /* a call no one waits for takes no taps; a person's call goes before it */
    s_done = 0;
    s_norder = 0;
    alarm_cmd_t chime;
    memset(&chime, 0, sizeof(chime));
    chime.type = ALARM_CMD_CHIME;
    chime.partition = 1;
    check(ui_call_partition(1, true, counted, 1u), "the partition's activity is queued");
    check(!ui_call_waiting() && shield() == NULL, "...and takes no taps");
    check(ui_call_partition(1, true, counted, 2u), "a second activity waits behind it");
    check(ui_call_command(&chime, counted, 3u), "a person's call is queued");
    run(20); /* laid out */
    check(shield() != NULL, "...and takes the taps");
    check(ui_call_command(&chime, counted, 4u), "a second person's call is queued");
    check(!ui_call_command(&chime, counted, 9u), "a fifth waiting call is refused");
    for (int i = 0; i < 4; i++) {
        answer_one();
        run(30);
    }
    check(s_done == 4 && s_norder == 4, "four continuations ran, one after the other");
    check(s_order[0] == 1u && s_order[1] == 3u && s_order[2] == 4u && s_order[3] == 2u,
          "the person's calls went before the activity still waiting");
    check(!ui_call_waiting() && shield() == NULL && s_nheld == 0, "nothing left waiting");

    /* without run_later (the alarm panel's own engine): at once, as before */
    s_later = false;
    s_done = 0;
    s_norder = 0;
    s_order[0] = 0u;
    check(ui_call_command(&chime, counted, 5u) && s_done == 1 && s_order[0] == 5u,
          "without run_later the call and its continuation ran at once");
    check(!ui_call_waiting() && shield() == NULL, "...with no layer");

    printf("%d checks passed: the screen's calls to another panel\n", s_checks);
    return 0;
}
