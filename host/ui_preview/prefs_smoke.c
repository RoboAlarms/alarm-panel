/*
 * prefs_smoke.c - a change of the Screen and sound settings from elsewhere goes through the screen
 * (docs/features/07 UI-046; docs/features/11 "Applying a change from elsewhere", LINK-051, LINK-052;
 * the owner's decisions 4, 6 and 10 for BL-216). The real touch UI over the simulated panel, a
 * patch handed in as the panel's mailbox hands it (host_panel_prefs_submit: the same bookkeeping
 * and conversion as platform_esp's panel_prefs_remote_* and ui_port_esp's prefs_remote_*):
 *  - merged into the screen's own copy and saved through it: the fields named and nothing else,
 *    the partition, the language and the region as they were; the backlight follows at once; a
 *    patch that changes nothing saves nothing; nothing on the screen says it happened;
 *  - IN_USE while the setup runs or a page that shows these settings is open (Screen and sound and
 *    each of its pages, Time and language), never for the other pages or Home;
 *  - HELD during exit time, entry time and an alarm; applied while armed (decision 4);
 *  - VOICE_NOT_READY for Announcements turned on while the voice isn't ready, that field alone;
 *    RANGE for a value out of range, that field alone;
 *  - the look changes from elsewhere at most once per 2 s: Appearance flipped every 0.5 s for 30 s;
 *    a patch that would change it sooner waits whole, one that doesn't is applied at once;
 *  - so does the backlight of a screen nobody touches: the screen-off time flipped every 0.5 s for
 *    30 s with the screen saver on (dark, the saver's level, dark...); and the clock's format;
 *  - the mailbox: one change at a time (BUSY), a patch the screen didn't take within 3 s answered
 *    BUSY and never applied, a cancel before the screen took it answered by nobody.
 * Windowless; exits non-zero on the first failed check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_ui_gen.h"
#include "host_panel.h"
#include "lvgl.h"
#include "ui_app.h"
#include "ui_backend/ui_prefs.h"
#include "ui_internal.h"

#ifndef ALARM_SCREEN_W
#define ALARM_SCREEN_W 1024
#define ALARM_SCREEN_H 600
#endif
#define W ALARM_SCREEN_W
#define H ALARM_SCREEN_H

#define EPOCH_MS 1789857660000LL

static uint16_t s_fb[W * H];
static uint32_t s_ms;
static int s_checks;

/* The screen's look, watched at every step of run(): how often and when it changed. */
static ui_theme_t s_theme;
static int s_light;
static unsigned s_look_changes;
static uint32_t s_look_at[64];

static struct {
    unsigned calls;
    uint32_t ticket;
    uint8_t why[PROTO_PREFS_F_LAST];
} s_ans;

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
    data->point.x = 0;
    data->point.y = 0;
    data->state = LV_INDEV_STATE_RELEASED;
}

static void watch_look(void)
{
    const ui_theme_t t = ui_theme_current();
    const int light = host_panel_backlight();
    if (t != s_theme || light != s_light) {
        if (s_look_changes < sizeof(s_look_at) / sizeof(s_look_at[0])) {
            s_look_at[s_look_changes] = s_ms;
        }
        s_look_changes++;
        s_theme = t;
        s_light = light;
    }
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10u) {
        s_ms += 10u;
        host_panel_advance(10u);
        lv_timer_handler();
        watch_look();
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

static void on_done(void *ctx, uint32_t ticket, const uint8_t why[PROTO_PREFS_F_LAST])
{
    (void)ctx;
    s_ans.calls++;
    s_ans.ticket = ticket;
    memcpy(s_ans.why, why, sizeof(s_ans.why));
}

static proto_prefs_values_t patch(void)
{
    proto_prefs_values_t v;
    memset(&v, 0, sizeof(v));
    return v;
}

static void put(proto_prefs_values_t *v, unsigned id, uint32_t value)
{
    if (proto_prefs_put(v, id, value) != PROTO_PREFS_WHY_NONE) {
        printf("FAIL  the test's own patch: field %u, %lu\n", id, (unsigned long)value);
        exit(1);
    }
}

static uint32_t submit(const proto_prefs_values_t *v)
{
    memset(&s_ans, 0, sizeof(s_ans));
    return host_panel_prefs_submit(v, on_done, NULL);
}

/* Submitted and answered within 100 ms (two ticks of the screen), with this ticket. */
static bool answered(const proto_prefs_values_t *v)
{
    const uint32_t t = submit(v);
    run(100);
    return t != 0u && s_ans.calls == 1u && s_ans.ticket == t;
}

/* Every field of the answer: `want` for those in `fields`, 0 for the rest. */
static bool why_is(uint32_t fields, uint8_t want)
{
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        const uint8_t expect = (fields & PROTO_PREFS_BIT(id)) != 0u ? want : 0u;
        if (s_ans.why[id - 1u] != expect) {
            printf("      field %u: why %u, wanted %u\n", id, s_ans.why[id - 1u], expect);
            return false;
        }
    }
    return true;
}

/* The screen's copy and the stored one equal `want` in every field (ui_prefs_get), the partition,
 * the language and the region. */
static bool opt_is(const ui_options_t *want)
{
    ui_options_t saved;
    if (!host_panel_saved_options(&saved)) {
        return false;
    }
    const ui_options_t *both[2] = {&S.opt, &saved};
    for (size_t k = 0; k < 2u; k++) {
        const ui_options_t *o = both[k];
        if (o->partition != want->partition || strcmp(o->lang, want->lang) != 0 ||
            strcmp(o->region, want->region) != 0) {
            printf("      %s: partition %u, \"%s\", \"%s\"\n", k == 0 ? "screen" : "stored",
                   o->partition, o->lang, o->region);
            return false;
        }
        for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
            uint32_t a = 0u;
            uint32_t b = 1u;
            (void)ui_prefs_get(o, id, &a);
            (void)ui_prefs_get(want, id, &b);
            if (a != b) {
                printf("      %s: field %u is %lu, wanted %lu\n", k == 0 ? "screen" : "stored", id,
                       (unsigned long)a, (unsigned long)b);
                return false;
            }
        }
    }
    return true;
}

static alarm_result_t remote(alarm_cmd_type_t type, alarm_arm_level_t level)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = 1;
    cmd.level = level;
    snprintf(cmd.code, sizeof(cmd.code), "123456"); /* the factory master code */
    return host_panel_remote_command(&cmd);
}

static void home(void)
{
    ui_set_int(&subject_view, VIEW_HOME);
    run(100);
}

/* UI-046, LINK-052: the fields named and nothing else, through the screen's own copy and save. */
static void test_merge(void)
{
    /* A keypad that shows partition 2 and formats for Canada: none of it is a settings field. */
    ui_options_t start = S.opt;
    start.partition = 2;
    memcpy(start.region, "CA", 3);
    ui_app_apply_options(&start);
    run(100);
    ui_options_t want = S.opt;
    check(want.partition == 2 && strcmp(want.region, "CA") == 0, "the screen shows partition 2");
    const unsigned saves = host_panel_options_saves();

    proto_prefs_values_t v = patch();
    put(&v, PROTO_PREFS_F_BRIGHTNESS, 40u);
    put(&v, PROTO_PREFS_F_VOLUME, 0u); /* decision 6: Beeps may be 0 */
    put(&v, PROTO_PREFS_F_NIGHT_END, 300u);
    put(&v, PROTO_PREFS_F_SAVER_AFTER, 0u);
    check(answered(&v), "UI-046: answered on the screen's task, within two ticks");
    check(why_is(0u, 0u), "UI-046: nothing refused");
    want.brightness = 40u;
    want.volume = 0u;
    want.night_end = 300u;
    want.saver_after_ms = 0u;
    check(opt_is(&want), "UI-046, LINK-052: the four fields changed on the screen and stored, "
                         "the rest, the partition, the language and the region as they were");
    check(host_panel_options_saves() == saves + 1u, "UI-046: saved once, through the screen's save");
    check(host_panel_backlight() == 40, "UI-046: the screen is lit at the new brightness at once");
    check(lv_subject_get_int(&subject_toast_visible) == 0,
          "decision 10: nothing on the screen says a setting changed");

    check(answered(&v), "the same patch again: answered");
    check(why_is(0u, 0u) && opt_is(&want), "the same patch again: nothing refused, nothing moved");
    check(host_panel_options_saves() == saves + 1u, "a patch that changes nothing saves nothing");

    start = S.opt;
    start.partition = 1;
    start.region[0] = '\0';
    ui_app_apply_options(&start);
    run(100);
}

/* UI-046: a value out of range is refused for that field alone; the rest is applied. */
static void test_range(void)
{
    ui_options_t want = S.opt;
    proto_prefs_values_t v = patch();
    v.brightness = 5u; /* past proto_prefs_put's own check: as a newer or a broken sender might */
    v.have |= PROTO_PREFS_BIT(PROTO_PREFS_F_BRIGHTNESS);
    v.saver_after_ms = 5000u;
    v.have |= PROTO_PREFS_BIT(PROTO_PREFS_F_SAVER_AFTER);
    put(&v, PROTO_PREFS_F_CHIME_VOLUME, 21u);
    check(answered(&v), "a patch with values out of range: answered");
    check(why_is(PROTO_PREFS_BIT(PROTO_PREFS_F_BRIGHTNESS) | PROTO_PREFS_BIT(PROTO_PREFS_F_SAVER_AFTER),
                 PROTO_PREFS_WHY_RANGE),
          "the brightness and the saver's delay refused RANGE, alone");
    want.chime_volume = 21u;
    check(opt_is(&want), "the chime's volume applied, the two refused as they were");
}

/* UI-046: refused whole while a person is changing these settings here. (Fields that aren't the
 * look, so the look's 2 s play no part: test_look_spacing.) */
static void test_in_use(void)
{
    proto_prefs_values_t v = patch();
    put(&v, PROTO_PREFS_F_VOLUME, 60u);
    put(&v, PROTO_PREFS_F_KEY_CLICK, 0u);
    const uint32_t both = PROTO_PREFS_BIT(PROTO_PREFS_F_VOLUME) | PROTO_PREFS_BIT(PROTO_PREFS_F_KEY_CLICK);
    const ui_options_t was = S.opt;

    ui_settings_open(); /* Screen and sound */
    run(100);
    check(S.depth == 1u && S.pages[0].id == PAGE_DISPLAY, "Settings opens on Screen and sound");
    check(answered(&v) && why_is(both, PROTO_PREFS_WHY_IN_USE),
          "UI-046: Screen and sound open: every field IN_USE");
    static const struct {
        page_id_t page;
        const char *what;
    } pages[] = {
        {PAGE_SAVER, "UI-046: Screen saver open: IN_USE"},
        {PAGE_NIGHT, "UI-046: Night hours open: IN_USE"},
        {PAGE_ANNOUNCEMENTS, "UI-046: Announcements open: IN_USE"},
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        ui_page_open(pages[i].page, 0);
        run(100);
        check(answered(&v) && why_is(both, PROTO_PREFS_WHY_IN_USE), pages[i].what);
        ui_page_back();
        run(100);
    }
    ui_settings_section("time");
    run(100);
    check(S.pages[0].id == PAGE_TIME, "Time and language is open");
    check(answered(&v) && why_is(both, PROTO_PREFS_WHY_IN_USE),
          "UI-046: Time and language (its Clock row) open: IN_USE");
    check(opt_is(&was), "UI-046: nothing changed under the open pages");

    ui_settings_section("about");
    run(100);
    check(lv_subject_get_int(&subject_view) == VIEW_SETTINGS && S.pages[0].id == PAGE_ABOUT,
          "About is open");
    check(answered(&v) && why_is(0u, 0u), "UI-046: another page of Settings: applied");
    ui_options_t want = was;
    want.volume = 60u;
    want.key_click = false;
    check(opt_is(&want), "applied with About open");
    home();

    /* the setup, run again from the installer menu */
    put(&v, PROTO_PREFS_F_VOLUME, 70u);
    ui_app_run_setup();
    run(100);
    check(S.wizard_on, "the setup runs");
    check(answered(&v) && why_is(both, PROTO_PREFS_WHY_IN_USE), "UI-046: the setup runs: IN_USE");
    check(opt_is(&want), "nothing changed under the setup");
    ui_wizard_exit();
    run(100);
    check(!S.wizard_on, "the setup left");
    check(answered(&v) && why_is(0u, 0u), "Home again: applied");
    want.volume = 70u;
    check(opt_is(&want), "applied after the setup");
}

/* UI-046, decision 4: held during an alarm, entry time and exit time; applied while armed. */
static void test_held(void)
{
    proto_prefs_values_t v = patch();
    put(&v, PROTO_PREFS_F_VOICE_VOLUME, 41u);
    const uint32_t one = PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_VOLUME);
    ui_options_t want = S.opt;

    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "arming Away from elsewhere");
    run(500);
    check(answered(&v) && why_is(one, PROTO_PREFS_WHY_HELD), "UI-046: exit time: HELD");
    check(opt_is(&want), "nothing changed during exit time");
    run(70000);
    check(!ui_app_held(), "armed Away");
    check(answered(&v) && why_is(0u, 0u), "decision 4: armed: applied");
    want.voice_volume = 41u;
    check(opt_is(&want), "applied while armed");

    put(&v, PROTO_PREFS_F_VOICE_VOLUME, 42u);
    host_panel_zone(1, ALARM_ZEV_FAULT); /* the front door on the way in */
    run(500);
    check(answered(&v) && why_is(one, PROTO_PREFS_WHY_HELD), "UI-046: entry time: HELD");
    run(70000);
    check(ui_app_held(), "the entry time ran out: an alarm");
    check(answered(&v) && why_is(one, PROTO_PREFS_WHY_HELD), "UI-046: an alarm: HELD");
    check(opt_is(&want), "nothing changed during entry time or the alarm");
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarmed");
    run(1000);
    check(answered(&v) && why_is(0u, 0u), "disarmed: applied");
    want.voice_volume = 42u;
    check(opt_is(&want), "applied after the disarm");
}

#if CONFIG_ALARM_FEATURE_VOICE
/* UI-046, D06: Announcements go on only while the voice is ready; that field alone. */
static void test_voice(void)
{
    ui_options_t want = S.opt;
    proto_prefs_values_t off = patch();
    put(&off, PROTO_PREFS_F_VOICE_ENABLED, 0u);
    check(answered(&off) && why_is(0u, 0u), "Announcements off from elsewhere");
    want.voice_enabled = false;
    check(opt_is(&want), "Announcements are off");

    proto_prefs_values_t on = patch();
    put(&on, PROTO_PREFS_F_VOICE_ENABLED, 1u);
    put(&on, PROTO_PREFS_F_VOLUME, 50u);
    const int states[] = {UI_VOICE_NONE, UI_VOICE_LOADING, UI_VOICE_UNAVAILABLE};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        host_panel_voice_pretend(states[i]);
        check(answered(&on) &&
                  why_is(PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_ENABLED), PROTO_PREFS_WHY_VOICE_NOT_READY),
              "UI-046: Announcements on while the voice isn't ready: VOICE_NOT_READY, alone");
        want.volume = 50u;
        check(opt_is(&want), "the Beeps volume applied, Announcements still off");
    }
    host_panel_voice_pretend(UI_VOICE_READY);
    check(answered(&on) && why_is(0u, 0u), "the voice ready: Announcements on");
    want.voice_enabled = true;
    check(opt_is(&want), "Announcements are on");
    host_panel_voice_pretend(UI_VOICE_NONE);
    check(answered(&on) && why_is(0u, 0u), "on while already on: nothing turned on, not refused");
    check(answered(&off) && why_is(0u, 0u), "off while the voice isn't ready: applied");
    want.voice_enabled = false;
    check(opt_is(&want), "Announcements are off again");
    host_panel_voice_pretend(-1);
    check(answered(&on) && (why_is(0u, 0u) || why_is(PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_ENABLED),
                                                     PROTO_PREFS_WHY_VOICE_NOT_READY)),
          "the preview's own voice: answered as it is");
    run(100);
}
#endif

/* a and b in turn, one every 0.5 s for 30 s, then 2.1 s more: each patch taken is answered once
 * and never refused. Returns when it began; the screen's changes are in s_look_*. */
static uint32_t flood(const proto_prefs_values_t *a, const proto_prefs_values_t *b, unsigned *busy)
{
    run(2100); /* the 2 s from any change before */
    s_look_changes = 0u;
    memset(&s_ans, 0, sizeof(s_ans)); /* counted across the run: host_panel_prefs_submit itself */
    unsigned sent = 0u;
    *busy = 0u;
    const uint32_t t0 = s_ms;
    for (unsigned i = 0; i < 60u; i++) { /* every 0.5 s for 30 s */
        const uint32_t t = host_panel_prefs_submit(i % 2u == 0u ? a : b, on_done, NULL);
        if (t == 0u) {
            (*busy)++; /* one is in the box: BUSY at once */
        } else {
            sent++;
        }
        for (unsigned k = 0; k < 50u; k++) {
            const unsigned before = s_ans.calls;
            run(10);
            if (s_ans.calls != before) {
                check(why_is(0u, 0u), "UI-046: a held change is applied, never refused, once the 2 s pass");
            }
        }
    }
    run(2100);
    check(s_ans.calls == sent, "LINK-051: every patch taken is answered, once");
    return t0;
}

/* The shortest time between two of the screen's changes s_look_* saw. */
static uint32_t shortest_gap(void)
{
    const unsigned n = s_look_changes < 64u ? s_look_changes : 64u;
    uint32_t shortest = UINT32_MAX;
    for (unsigned i = 1; i < n; i++) {
        const uint32_t gap = s_look_at[i] - s_look_at[i - 1u];
        shortest = gap < shortest ? gap : shortest;
    }
    return shortest;
}

/* UI-046, WCAG 2.3.1: the look changes from elsewhere at most once per 2 s, whatever is sent. */
static void test_look_spacing(void)
{
    proto_prefs_values_t light = patch();
    put(&light, PROTO_PREFS_F_APPEARANCE, 1u);
    proto_prefs_values_t dark = patch();
    put(&dark, PROTO_PREFS_F_APPEARANCE, 0u);
    unsigned busy = 0u;
    const uint32_t t0 = flood(&light, &dark, &busy);
    const uint32_t shortest = shortest_gap();
    printf("      30 s of flips: %u busy, %u looks, shortest gap %lu ms\n", busy, s_look_changes,
           (unsigned long)shortest);
    check(s_look_changes >= 10u, "the flips do change the screen");
    check(s_look_changes <= 1u + (s_ms - t0) / UI_PREFS_LOOK_MS && shortest >= UI_PREFS_LOOK_MS,
          "UI-046: the look changed at most once per 2 s");
    check(s_look_at[0] - t0 < 200u, "the first change is shown at once");
    check(busy > 0u, "LINK-051: one change at a time: the others BUSY at once");

    /* A change that isn't the look is never held. */
    proto_prefs_values_t flip = patch();
    put(&flip, PROTO_PREFS_F_APPEARANCE, S.opt.appearance == 0u ? 1u : 0u);
    check(answered(&flip) && why_is(0u, 0u), "a look change");
    ui_options_t want = S.opt;
    proto_prefs_values_t beeps = patch();
    put(&beeps, PROTO_PREFS_F_VOLUME, 33u);
    check(answered(&beeps) && why_is(0u, 0u), "UI-046: the Beeps volume right after it: at once");
    want.volume = 33u;
    check(opt_is(&want), "the Beeps volume applied");

    /* A patch with the look and something else waits whole. */
    proto_prefs_values_t mixed = patch();
    put(&mixed, PROTO_PREFS_F_HIGH_CONTRAST, S.opt.high_contrast ? 0u : 1u);
    put(&mixed, PROTO_PREFS_F_CHIME_VOLUME, 34u);
    const uint32_t t = submit(&mixed);
    run(500);
    check(t != 0u && s_ans.calls == 0u, "UI-046: high contrast within 2 s: the patch waits");
    check(opt_is(&want), "UI-046: waiting whole: the chime's volume not applied either");
    check(submit(&beeps) == 0u, "LINK-051: another change while one waits: BUSY");
    run(2000);
    check(s_ans.calls == 1u && s_ans.ticket == t && why_is(0u, 0u), "applied once the 2 s passed");
    want.high_contrast = !want.high_contrast;
    want.chime_volume = 34u;
    check(opt_is(&want), "high contrast and the chime's volume applied together");
}

/* docs/features/11: a patch the screen doesn't take within 3 s is answered BUSY and never applied;
 * one withdrawn before the screen took it is answered by nobody. */
static void test_mailbox(void)
{
    const ui_options_t want = S.opt;
    proto_prefs_values_t v = patch();
    put(&v, PROTO_PREFS_F_VOICE_STATUS, want.voice_status ? 0u : 1u);
    put(&v, PROTO_PREFS_F_NIGHT_ON, want.night_on ? 0u : 1u);
    const uint32_t a = submit(&v);
    check(a != 0u && submit(&v) == 0u, "LINK-051: a second patch while one waits: BUSY at once");
    host_panel_advance(UI_PREFS_TAKE_MS); /* the screen's task doesn't run for 3 s */
    run(100);
    check(s_ans.calls == 1u && s_ans.ticket == a, "the patch not taken within 3 s: answered");
    check(why_is(PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_STATUS) | PROTO_PREFS_BIT(PROTO_PREFS_F_NIGHT_ON),
                 PROTO_PREFS_WHY_BUSY),
          "docs/features/11: answered BUSY");
    check(opt_is(&want), "and never applied");
    check(!host_panel_prefs_cancel(a), "answered: nothing to withdraw");

    const uint32_t b = submit(&v);
    check(b != 0u && b != a, "the box is free again, with a new ticket");
    check(host_panel_prefs_cancel(b), "withdrawn before the screen took it");
    run(200);
    check(s_ans.calls == 0u, "a withdrawn patch is answered by nobody");
    check(opt_is(&want), "and never applied");
    check(!host_panel_prefs_cancel(b), "withdrawn once");
}

/* UI-046, Display safety (the backlight never flickers): when a screen nobody touches dims and
 * goes dark is not the look, yet a new screen-off time from elsewhere moves the backlight at once -
 * from the screen saver to dark and back, for no touch wakes it in between. So it is held the same
 * 2 s (ui_prefs.h UI_PREFS_SHOWN, with the saver's delay and the night's screen-off time, proven
 * against ui_light.h in test_ui_prefs.c), and so is the clock's format, whose words change. */
static void test_dark_spacing(void)
{
    run(2100);
    proto_prefs_values_t saver = patch();
    put(&saver, PROTO_PREFS_F_SAVER_AFTER, 10000u);
    put(&saver, PROTO_PREFS_F_SAVER_BRIGHTNESS, 25u);
    put(&saver, PROTO_PREFS_F_OFF_AFTER, 0u);
    put(&saver, PROTO_PREFS_F_NIGHT_ON, 0u);
    check(answered(&saver) && why_is(0u, 0u), "the screen saver after 10 s at 25 %, never dark");
    run(12000);
    check(S.saver_on && host_panel_backlight() == 25, "nobody touched it: the screen saver, at 25 %");

    proto_prefs_values_t dark = patch();
    put(&dark, PROTO_PREFS_F_OFF_AFTER, 10000u);
    proto_prefs_values_t never = patch();
    put(&never, PROTO_PREFS_F_OFF_AFTER, 0u);
    unsigned busy = 0u;
    const uint32_t t0 = flood(&dark, &never, &busy);
    const uint32_t shortest = shortest_gap();
    printf("      30 s of screen-off flips: %u busy, %u backlight changes, shortest gap %lu ms\n",
           busy, s_look_changes, (unsigned long)shortest);
    check(s_look_changes >= 10u, "the flips do darken the screen and light it again");
    check(s_look_changes <= 1u + (s_ms - t0) / UI_PREFS_LOOK_MS && shortest >= UI_PREFS_LOOK_MS,
          "UI-046: the backlight moved at most once per 2 s");
    check(s_look_at[0] - t0 < 200u, "the first change is shown at once");
    check(S.saver_on, "a change from elsewhere wakes nothing: still the screen saver");

    /* The clock's format right after a screen-off time: it waits, whole, with the Beeps volume. */
    ui_options_t want = S.opt;
    const uint32_t fmt_mode = want.clock_mode == 2u ? 1u : 2u;
    check(answered(S.opt.off_after_ms == 0u ? &dark : &never) && why_is(0u, 0u),
          "a new screen-off time: applied at once");
    want.off_after_ms = S.opt.off_after_ms;
    proto_prefs_values_t fmt = patch();
    put(&fmt, PROTO_PREFS_F_CLOCK_MODE, fmt_mode);
    put(&fmt, PROTO_PREFS_F_VOLUME, 37u);
    const uint32_t t = submit(&fmt);
    run(500);
    check(t != 0u && s_ans.calls == 0u, "UI-046: the clock's format within 2 s: the patch waits");
    check(opt_is(&want), "waiting whole: the Beeps volume not applied either");
    run(2000);
    check(s_ans.calls == 1u && s_ans.ticket == t && why_is(0u, 0u), "applied once the 2 s passed");
    want.clock_mode = (uint8_t)fmt_mode;
    want.volume = 37u;
    check(opt_is(&want), "the clock's format and the Beeps volume applied together");
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

    host_panel_init(EPOCH_MS);
    ui_backend_t be;
    host_panel_backend(&be);
    ui_app_start(&be, NULL);
    run(400);
    s_theme = ui_theme_current();
    s_light = host_panel_backlight();

    test_merge(); /* its first step saves: the stored copy opt_is() compares from then on */
    test_range();
    test_in_use();
    test_held();
#if CONFIG_ALARM_FEATURE_VOICE
    test_voice();
#endif
    test_look_spacing();
    test_mailbox();
    test_dark_spacing(); /* last: it leaves the screen dark, in the saver */

    printf("%d checks passed: settings from elsewhere go through the screen (UI-046)\n", s_checks);
    return 0;
}
