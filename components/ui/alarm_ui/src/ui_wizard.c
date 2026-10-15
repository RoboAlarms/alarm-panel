/*
 * ui_wizard.c - first-boot setup: welcome, Wi-Fi, time zone, the factory codes, the system
 * name, done. Runs on its own screen (screen_wizard); every step can be skipped, and anything
 * set here can be changed later in Settings. An alarm takes the main screen over and the
 * setup continues afterwards. Run again from the installer menu, on a panel that is already
 * set up, every step also offers "Leave setup" (ui_wizard_exit).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

/* The language comes first when the firmware has more than one (docs/features/37 I18N-012);
 * the dots at the top count from Wi-Fi. */
enum { STEP_LANGUAGE = 0, STEP_WELCOME, STEP_WIFI, STEP_TIME, STEP_CLOCK, STEP_CODES, STEP_NAME, STEP_DONE };

static int32_t s_step;
static bool s_can_exit; /* the panel is already set up: every step offers "Leave setup" */
static uint8_t s_code_user;
static alarm_config_t *s_scratch;

static lv_obj_t *body(void)
{
    return ui_find(S.wizard, "wiz_body");
}

static void side(const char *glyph, const char *title, const char *note)
{
    ui_set_str(&subject_wiz_icon, glyph);
    ui_set_str(&subject_wiz_title, title);
    ui_set_str(&subject_wiz_note, note);
    /* A language with longer words may need the picture's room for them (I18N-022): the
     * picture goes, every word stays. */
    lv_obj_t *column = ui_find(S.wizard, "wiz_side");
    lv_obj_t *disk = ui_find(S.wizard, "wiz_disk");
    if (column != NULL && disk != NULL) {
        lv_obj_remove_flag(disk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(column);
        if (lv_obj_get_scroll_bottom(column) > 0) {
            lv_obj_add_flag(disk, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void footer(const char *next, bool back)
{
    ui_set_str(&subject_wiz_next, next);
    ui_set_int(&subject_wiz_next_on, 1);
    ui_set_int(&subject_wiz_back, back ? 1 : 0);
}

static void build_step(void);

/* A still shade at the foot of the list while more of it runs on under the buttons; nothing moves. */
static void shade_update(void)
{
    lv_obj_t *list = body();
    lv_obj_t *shade = ui_find(S.wizard, "wiz_shade");
    if (list == NULL || shade == NULL) {
        return;
    }
    if (lv_obj_get_scroll_bottom(list) > 0) {
        lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_body_scroll(lv_event_t *e)
{
    (void)e;
    shade_update();
}

/* ---- steps --------------------------------------------------------------------------------- */

static void build_welcome(lv_obj_t *b)
{
    side(UI_ICON_SPARKLES, TR("Welcome"),
         TR("Let's get your alarm panel ready. It takes about two minutes, and you can change "
            "everything later in Settings."));
    ui_row_link(b, UI_ICON_WIFI, ui_tone_blue(), TR("Join Wi-Fi"),
                TR("For the clock and updates"), NULL, NULL, NULL, NULL);
    ui_row_link(b, UI_ICON_GLOBE, ui_tone_blue(), TR("Choose your time zone"),
                TR("So the event log shows your local time"), NULL, NULL, NULL, NULL);
    ui_row_link(b, UI_ICON_KEY_ROUND, ui_tone_amber(), TR("Change the factory codes"),
                TR("Your own installer and master codes"), NULL, NULL, NULL, NULL);
    ui_row_link(b, UI_ICON_TAG, ui_tone_violet(), TR("Name your system"),
                TR("How it shows up on the screen"), NULL, NULL, NULL, NULL);
    /* the progress bar counts the last step too: the list names all five */
    ui_row_link(b, UI_ICON_PARTY, ui_tone_green(), TR("Look it over"),
                TR("A last look at what you chose"), NULL, NULL, NULL, NULL);
    ui_paragraph(b, TR("The panel already protects your home: fire, CO and panic work right now."),
                 ui_tone_gray());
    /* TR: the setup's first button, which starts the steps */
    footer(TR("Start"), ui_lang_offered());
}

static void build_language(lv_obj_t *b)
{
    side(UI_ICON_GLOBE, TR("Language"),
         TR("The language of this screen. You can change it later in Settings."));
    ui_lang_setup_build(b, build_step);
    footer(TR("Next"), false);
}

static void build_wifi(lv_obj_t *b)
{
    side(UI_ICON_WIFI, TR("Join Wi-Fi"),
         TR("The alarm works without it. Wi-Fi sets the clock and keeps the panel up to "
            "date."));
    ui_net_build(b, true);
    /* TR: the setup's button to the next step; "Skip for now" while Wi-Fi isn't connected */
    footer(ui_net_connected() ? TR("Next") : TR("Skip for now"), true);
}

static void tz_row_clicked(void *ctx)
{
    const int i = (int)(intptr_t)ctx;
    if (S.be.set_timezone != NULL && i >= 0 && (size_t)i < UI_TIMEZONE_COUNT) {
        S.be.set_timezone(S.be.ctx, UI_TIMEZONES[i].posix);
        S.last_minute = -1;
    }
    build_step();
}

static void build_time(lv_obj_t *b)
{
    side(UI_ICON_GLOBE, TR("Time zone"),
         TR("So the clock and the event log show your local time. Daylight saving switches by "
            "itself."));
    char tz[64] = "UTC0";
    if (S.be.get_timezone != NULL) {
        S.be.get_timezone(S.be.ctx, tz, sizeof(tz));
    }
    const int current = ui_tz_find(tz);
    lv_obj_t *selected = NULL;
    for (size_t i = 0; i < UI_TIMEZONE_COUNT; i++) {
        lv_obj_t *row = ui_row_link(b, NULL, ui_tone_blue(), TR_C("time zone", UI_TIMEZONES[i].name),
                                    TR_C("time zone places", UI_TIMEZONES[i].places), NULL,
                                    (int)i == current ? UI_ICON_CHECK : NULL, tz_row_clicked,
                                    (void *)(intptr_t)i);
        if ((int)i == current) {
            lv_obj_set_style_text_color(ui_find(row, "trail_label"), ui_tone_blue(), 0);
            selected = row;
        }
    }
    lv_obj_update_layout(b);
    if (selected != NULL) {
        lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
    }
    footer(TR("Next"), true);
}

/* Only when a clock module lost the time and there is no internet to set it (RTC-015). The
 * hardware is not named; the step shares the time zone's dot at the top. A calendar and rollers
 * (BL-131 finding 3); Next sets the clock to what they show. */
static void build_clock(lv_obj_t *b)
{
    side(UI_ICON_CALENDAR_CLOCK, TR("Date and time"),
         TR("No internet yet, so set the clock here. The panel corrects it once it's online."));
    ui_clock_picker(b);
    footer(TR("Next"), true);
}

static bool code_done(const char *code)
{
    const char *current = s_code_user == ALARM_USER_INSTALLER ? ALARM_DEFAULT_INSTALLER_CODE
                                                              : ALARM_DEFAULT_MASTER_CODE;
    const alarm_user_t *u = &S.cfg->users[s_code_user - 1u];
    const alarm_result_t r = S.be.user_set != NULL
                                 ? S.be.user_set(S.be.ctx, current, s_code_user, code,
                                                 u->authority, u->partitions, NULL)
                                 : ALARM_ERR_DISABLED;
    if (r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(ui_result_text(r));
        return false;
    }
    ui_pad_cancel();
    ui_refresh(true);
    build_step(); /* the row now says "Changed": no toast (rule 7) */
    return true;
}

static void change_code(void *ctx)
{
    s_code_user = (uint8_t)(uintptr_t)ctx;
    ui_pad_ask_new(HOST_WIZARD, UI_ICON_KEY_ROUND,
                   s_code_user == ALARM_USER_INSTALLER ? TR("New installer code")
                                                       : TR("New master code"),
                   code_done);
}

static void build_codes(lv_obj_t *b)
{
    side(UI_ICON_KEY_ROUND, TR("Change the factory codes"),
         TR("Anyone can look up the factory codes, so choose your own for the installer and the "
            "master."));
    const bool inst = (S.snap.default_codes & ALARM_DEFAULT_CODE_INSTALLER) != 0u;
    const bool master = (S.snap.default_codes & ALARM_DEFAULT_CODE_MASTER) != 0u;
    /* TR: setup, the installer code row. At most 95 bytes */
    const char *inst_note = inst ? TR("Still the factory code · for programming the panel")
                                 /* TR: setup, the installer code row once it was changed */
                                 : TR("Changed · for programming the panel");
    /* TR: setup, the master code row. At most 95 bytes */
    const char *master_note = master ? TR("Still the factory code · arms, disarms, adds users")
                                     /* TR: setup, the master code row once it was changed */
                                     : TR("Changed · arms, disarms, adds users");
    ui_row_link(b, inst ? UI_ICON_KEY_ROUND : UI_ICON_SHIELD_CHECK, inst ? ui_tone_amber() : ui_tone_green(),
                TR("Installer code"), inst_note,
                inst ? TR_C("code row button", "Change") : TR_C("code row button", "Change again"),
                UI_ICON_CHEVRON_RIGHT, change_code, (void *)(uintptr_t)ALARM_USER_INSTALLER);
    ui_row_link(b, master ? UI_ICON_KEY_ROUND : UI_ICON_SHIELD_CHECK, master ? ui_tone_amber() : ui_tone_green(),
                TR("Master code"), master_note,
                master ? TR_C("code row button", "Change") : TR_C("code row button", "Change again"),
                UI_ICON_CHEVRON_RIGHT, change_code, (void *)(uintptr_t)ALARM_USER_MASTER);
    if (master) {
        ui_paragraph(b, TR("Skip it for now and the status bar reminds you until the master code is "
                           "changed."),
                     ui_tone_amber());
    }
    /* "Next", not "Keep for now": that read as keeping the installer code too */
    footer(TR("Next"), true);
}

static void name_done(const char *text)
{
    if (text[0] != '\0' && S.be.set_config != NULL) {
        if (s_scratch == NULL) {
            s_scratch = lv_malloc(sizeof(alarm_config_t));
            LV_ASSERT_MALLOC(s_scratch);
        }
        ui_refresh(true);
        memcpy(s_scratch, S.cfg, sizeof(alarm_config_t));
        alarm_partition_config_t *p = &s_scratch->partitions[S.opt.partition - 1u];
        memset(p->name, 0, ALARM_NAME_LEN);
        i18n_utf8_copy(p->name, ALARM_NAME_LEN, text);
        const alarm_result_t r = S.be.set_config(S.be.ctx, s_scratch);
        if (r != ALARM_OK) {
            ui_toast(ui_result_text(r));
        }
        /* the step shows the new name */
        ui_refresh(true);
    }
    build_step();
}

static void rename_system(void *ctx)
{
    (void)ctx;
    ui_text_sheet(TR("System name"),
                  /* TR: examples of names for the system; the sheet's note */
                  TR("Home, Cabin, Shop..."), S.cfg->partitions[S.opt.partition - 1u].name,
                  false, ALARM_NAME_LEN - 1u, name_done);
}

static void build_name(lv_obj_t *b)
{
    side(UI_ICON_TAG, TR("Name your system"),
         TR("It shows on the screen saver and in the event log."));
    ui_row_link(b, UI_ICON_HOUSE, ui_tone_violet(), TR("System name"), NULL,
                S.cfg->partitions[S.opt.partition - 1u].name, UI_ICON_CHEVRON_RIGHT, rename_system, NULL);
    footer(TR("Next"), true);
}

/* One line of the last step's summary: green, with a check, when it is in order. */
static void done_row(lv_obj_t *b, const char *glyph, bool ok, const char *title, const char *detail,
                     bool check)
{
    lv_obj_t *row = ui_row_link(b, glyph, ok ? ui_tone_green() : ui_tone_gray(), title, detail, NULL,
                                check ? UI_ICON_CHECK : NULL, NULL, NULL);
    if (check) {
        lv_obj_set_style_text_color(ui_find(row, "trail_label"), ui_tone_green(), 0);
    }
}

static void build_done(lv_obj_t *b)
{
    side(UI_ICON_PARTY, TR("You're all set"),
         TR("Your panel is protecting your home. Everything here can be changed later in "
            "Settings."));
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    char tz[64] = "UTC0";
    if (S.be.get_timezone != NULL) {
        S.be.get_timezone(S.be.ctx, tz, sizeof(tz));
    }
    const int zone = ui_tz_find(tz);
    const bool connected = w.state == UI_NET_CONNECTED;
    done_row(b, connected ? UI_ICON_WIFI : UI_ICON_WIFI_OFF, connected, TR("Wi-Fi"),
             connected ? w.ssid : TR("Not connected: join later in Settings > Wi-Fi"), connected);
    done_row(b, UI_ICON_GLOBE, true, TR("Time zone"),
             zone >= 0 ? TR_C("time zone", UI_TIMEZONES[zone].name) : tz, true);
    /* a factory code left: the row is amber and offers "Change"; the master's first */
    const uint8_t factory = S.snap.default_codes;
    if (factory == 0u) {
        done_row(b, UI_ICON_KEY_ROUND, true, TR_C("setup summary", "Codes"),
                 TR("Your own installer and master codes"), true);
    } else {
        ui_row_link(b, UI_ICON_KEY_ROUND, ui_tone_amber(), TR_C("setup summary", "Codes"),
                    factory == ALARM_DEFAULT_CODE_MASTER      ? TR("The master code is still the factory code")
                    : factory == ALARM_DEFAULT_CODE_INSTALLER ? TR("The installer code is still the factory code")
                                                              : TR("Factory codes still in use"),
                    TR_C("code row button", "Change"), NULL, change_code,
                    (void *)(uintptr_t)((factory & ALARM_DEFAULT_CODE_MASTER) != 0u ? ALARM_USER_MASTER
                                                                                    : ALARM_USER_INSTALLER));
    }
    done_row(b, UI_ICON_HOUSE, true, TR_C("setup summary", "Name"),
             S.cfg->partitions[S.opt.partition - 1u].name, true);
    footer(TR("Done"), true);
}

static void build_step(void)
{
    ui_rows_reset();
    ui_set_int(&subject_busy, 0);
    /* five dots: the date and time step shares the time zone's, the later steps count past it */
    const int32_t dot = s_step == STEP_CLOCK ? STEP_TIME : s_step > STEP_CLOCK ? s_step - 1 : s_step;
    ui_set_int(&subject_wiz_step, dot > STEP_WELCOME ? dot - STEP_WELCOME : 0);
    /* "Leave setup" is gone from the last step: Done ends the setup */
    ui_set_int(&subject_wiz_exit, s_can_exit && s_step != STEP_DONE ? 1 : 0);
    lv_obj_t *b = body();
    switch (s_step) {
    case STEP_LANGUAGE: build_language(b); break;
    case STEP_WIFI: build_wifi(b); break;
    case STEP_TIME: build_time(b); break;
    case STEP_CLOCK: build_clock(b); break;
    case STEP_CODES: build_codes(b); break;
    case STEP_NAME: build_name(b); break;
    case STEP_DONE: build_done(b); break;
    default: build_welcome(b); break;
    }
    if (s_step != STEP_TIME && s_step != STEP_CLOCK) {
        lv_obj_scroll_to_y(b, 0, LV_ANIM_OFF);
    }
    lv_obj_update_layout(b);
    shade_update();
}

/* ---- flow ---------------------------------------------------------------------------------- */

void ui_wizard_language_changed(void)
{
    if (S.wizard_on) {
        build_step();
    }
}

void ui_wizard_init(void)
{
    s_step = STEP_WELCOME;
    s_can_exit = false;
    ui_set_int(&subject_wiz_exit, 0);
    lv_obj_add_event_cb(body(), on_body_scroll, LV_EVENT_SCROLL, NULL);
}

void ui_wizard_start(bool can_exit)
{
    ui_pad_cancel();
    ui_sheet_close();
    if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS) {
        ui_set_int(&subject_view, VIEW_HOME); /* leaves Settings (and installer mode) */
    }
    S.wizard_on = true;
    S.wizard_paused = false;
    s_can_exit = can_exit;
    ui_set_int(&subject_wiz_exit, can_exit ? 1 : 0);
    s_step = ui_lang_offered() ? STEP_LANGUAGE : STEP_WELCOME;
    build_step();
    lv_screen_load(S.wizard);
}

static void finish(void)
{
    ui_rows_reset();
    S.wizard_on = false;
    S.wizard_paused = false;
    s_can_exit = false;
    ui_set_int(&subject_wiz_exit, 0);
    if (S.be.setup_done != NULL) {
        S.be.setup_done(S.be.ctx);
    }
    ui_show_main(); /* no toast: Home itself shows the setup is done (BL-131, rule 7) */
}

/* Moves one step on or back, passing the date and time step when it isn't offered. */
static void go(int32_t dir)
{
    s_step += dir;
    if (s_step == STEP_CLOCK && !ui_clock_setup_needed()) {
        s_step += dir;
    }
    if (s_step == STEP_CLOCK) {
        ui_clock_fields_start();
    }
    build_step();
}

void ui_wizard_next(void)
{
    if (!S.wizard_on || lv_subject_get_int(&subject_wiz_next_on) == 0) {
        return;
    }
    if (s_step >= STEP_DONE) {
        finish();
        return;
    }
    if (s_step == STEP_CLOCK && !ui_clock_set_chosen()) {
        return; /* the error beep and toast are up; the step stays */
    }
    go(1);
}

void ui_wizard_back(void)
{
    if (S.wizard_on && s_step > (ui_lang_offered() ? STEP_LANGUAGE : STEP_WELCOME)) {
        go(-1);
    }
}

/* "Leave setup" on a panel that is already set up: what the steps did is already saved, so
 * there is nothing to undo and nothing to confirm - go back to Home. */
void ui_wizard_exit(void)
{
    if (!S.wizard_on || !s_can_exit) {
        return;
    }
    ui_pad_cancel();
    ui_sheet_close();
    ui_rows_reset();
    S.wizard_on = false;
    S.wizard_paused = false;
    s_can_exit = false;
    s_step = STEP_WELCOME;
    ui_set_int(&subject_wiz_exit, 0);
    ui_show_main();
}

void ui_wizard_tick(void)
{
    if (S.wizard_on && !S.wizard_paused && s_step == STEP_WIFI) {
        ui_set_str(&subject_wiz_next, ui_net_connected() ? TR("Next") : TR("Skip for now"));
    }
}

void ui_wizard_pause(void)
{
    if (!S.wizard_on || S.wizard_paused) {
        return;
    }
    S.wizard_paused = true;
    ui_rows_reset();
    ui_show_main();
}

void ui_wizard_resume(void)
{
    if (!S.wizard_on || !S.wizard_paused) {
        return;
    }
    S.wizard_paused = false;
    build_step();
    lv_screen_load(S.wizard);
}
