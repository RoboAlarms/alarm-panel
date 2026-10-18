/*
 * ui_wizard.c - first-boot setup: welcome, Wi-Fi, time zone, the factory codes, the system
 * name, done. Runs on its own screen (screen_wizard); every step can be skipped, and anything
 * set here can be changed later in Settings. An alarm takes the main screen over and the
 * setup continues afterwards. Run again from the installer menu, on a panel that is already
 * set up, every step also offers "Leave setup" (ui_wizard_exit).
 *
 * The panel link (docs/features/11, BL-048): on a first start, in a firmware with the link, the
 * role comes after Welcome - the alarm panel (the steps above) or a keypad for one, whose setup is
 * nearby discovery or Select Wi-Fi, pairing with the alarm panel (both codes compared,
 * LINK-004, LINK-016), authenticated Wi-Fi provisioning and Done (LINK-053). Each branch
 * counts its own dots. A keypad that isn't paired comes back to the pairing step (Pair again).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

/* The language comes first when the firmware has more than one (docs/features/37 I18N-012);
 * the dots at the top count from the role (BL-048), or from Wi-Fi without it. A keypad's branch is
 * the role, Wi-Fi, the pairing and its own Done. */
enum {
    STEP_LANGUAGE = 0,
    STEP_WELCOME,
    STEP_ROLE,
    STEP_WIFI,
    STEP_TIME,
    STEP_CLOCK,
    STEP_CODES,
    STEP_NAME,
    STEP_DONE,
    STEP_PAIR,
    STEP_KP_DONE,
    STEP_NEAR
};

static int32_t s_step;
static bool s_can_exit; /* the panel is already set up: every step offers "Leave setup" */
static bool s_role;     /* a first start with the panel link: the role step after Welcome */
static bool s_enrolled;
static bool s_keypad;   /* the keypad's branch (chosen on the role step, or Pair again) */
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
static void pair_start(void);

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
    if (s_role) {
        /* TR: the setup's Welcome on a panel's first start, the row of the role step (BL-048) */
        ui_row_link(b, UI_ICON_SHIELD, ui_tone_violet(), TR("Choose its role"),
                    TR("The alarm panel, or a keypad for one"), NULL, NULL, NULL, NULL);
    }
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

/* ---- the role (BL-048) ---------------------------------------------------------------------- */

static uint8_t pair_ask(uint8_t kind, bool allow);

/* The keypad branch is left (Back from its pairing step, the alarm panel chosen instead, the
 * alarm panel's Done): whatever its pairing made is forgotten on this panel (KP_PAIR_STOP), so
 * nothing of another alarm panel stays behind. Harmless when nothing runs. */
static void keypad_branch_left(void)
{
    (void)pair_ask(UI_LINK_REQ_KP_PAIR_STOP, false);
}

static void role_chosen(void *ctx)
{
    const bool was_keypad = s_keypad;
    s_keypad = (uintptr_t)ctx != 0u;
    if (was_keypad && !s_keypad) {
        keypad_branch_left();
    }
    build_step(); /* the dots count the branch's steps at once (not animated) */
}

/* A choice's row, its check in blue when chosen (as the time zones). */
static void role_row(lv_obj_t *b, const char *glyph, lv_color_t tone, const char *title,
                     const char *detail, bool chosen, uintptr_t keypad)
{
    lv_obj_t *row = ui_row_link(b, glyph, tone, title, detail, NULL, chosen ? UI_ICON_CHECK : NULL,
                                role_chosen, (void *)keypad);
    if (chosen) {
        lv_obj_set_style_text_color(ui_find(row, "trail_label"), ui_tone_blue(), 0);
    }
}

static void build_role(lv_obj_t *b)
{
    side(UI_ICON_SHIELD, TR("How will this panel be used?"),
         TR("Each home has one alarm panel. More panels can be keypads for it."));
    /* TR: the setup's role step, the alarm panel's row (the default) */
    role_row(b, UI_ICON_SHIELD_CHECK, ui_tone_green(), TR("The alarm panel"),
             TR("Sensors and the alarm live here"), !s_keypad, 0u);
    /* TR: the setup's role step, a keypad's row */
    role_row(b, UI_ICON_KEYBOARD, ui_tone_blue(), TR("A keypad"),
             TR("For an alarm panel elsewhere in the house"), s_keypad, 1u);
    ui_paragraph(b, TR("To change this later, the panel must be reset to its factory settings."),
                 ui_tone_gray());
    footer(TR("Next"), true);
}

static void select_wifi(void *ctx)
{
    (void)ctx;
    (void)pair_ask(UI_LINK_REQ_KP_WIFI, false);
    if (!s_enrolled) keypad_branch_left();
    s_step = STEP_WIFI;
    build_step();
}

static void build_near(lv_obj_t *b)
{
    side(UI_ICON_HOUSE, TR("Find your alarm panel"),
         TR("Pair nearby to get Wi-Fi settings automatically."));
    ui_row_link(b, UI_ICON_SEARCH, ui_tone_blue(), TR("Find panel"),
                TR("Open Add keypad in Panel Manager or on the alarm panel."), NULL, NULL, NULL, NULL);
    ui_row_link(b, UI_ICON_WIFI, ui_tone_blue(), TR("Select Wi-Fi"),
                TR("If the alarm panel is out of range"), NULL, UI_ICON_CHEVRON_RIGHT, select_wifi, NULL);
    footer(TR("Find panel"), true);
}

/* A keypad reaches the alarm panel over Wi-Fi: its step can't be skipped, Next waits for it. */
static void build_keypad_wifi(lv_obj_t *b)
{
    side(UI_ICON_WIFI, TR("Join Wi-Fi"),
         TR("Join the network the alarm panel uses."));
    const bool connected = ui_net_connected();
    if (!connected) {
        ui_paragraph(b, TR("Join the network the alarm panel uses."), ui_tone_gray());
    }
    ui_net_build(b, true);
    footer(TR("Next"), true);
    ui_set_int(&subject_wiz_next_on, connected ? 1 : 0);
}

/* ---- pairing with the alarm panel (BL-048, LINK-004, LINK-016) ------------------------------- */

/* What the pairing step shows, from the keypad side's pairing (ui_link_pair_t). */
typedef enum {
    PAIR_LOOKING = 0,
    PAIR_NOT_FOUND, /* 30 s with no alarm panel open for pairing: what to check; it keeps looking */
    PAIR_FOUND,     /* the two exchange keys */
    PAIR_COMPARE,   /* the code, Yes or No */
    PAIR_WAITING,   /* Yes here: the installer allows it at the alarm panel */
    PAIR_DONE,
    PAIR_FAILED, /* why: s_pair.p.fail; Try again */
} pair_view_t;

#define PAIR_NOT_FOUND_S 30u

static struct {
    ui_link_pair_t p;
    uint32_t polled;
    uint32_t shown; /* the view and words on the step, to build it again only when they change */
} s_pair;

static uint8_t pair_ask(uint8_t kind, bool allow)
{
    if (S.be.link_request == NULL) {
        return UI_LINK_REQ_REFUSED;
    }
    ui_link_req_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.allow = allow;
    return S.be.link_request(S.be.ctx, &req);
}

static void pair_read(void)
{
    memset(&s_pair.p, 0, sizeof(s_pair.p));
    if (S.be.link_pair_status == NULL || !S.be.link_pair_status(S.be.ctx, &s_pair.p)) {
        memset(&s_pair.p, 0, sizeof(s_pair.p));
    }
    s_pair.p.code[sizeof(s_pair.p.code) - 1u] = '\0';
    s_pair.p.peer_name[sizeof(s_pair.p.peer_name) - 1u] = '\0';
    s_pair.p.peer_addr[sizeof(s_pair.p.peer_addr) - 1u] = '\0';
    s_pair.polled = lv_tick_get();
}

/* A person asks: looking starts (LINK-016: a keypad starts an attempt only when a person asks). */
static void pair_start(void)
{
    (void)pair_ask(UI_LINK_REQ_KP_PAIR_START, false);
    pair_read();
}

static pair_view_t pair_view(const ui_link_pair_t *p)
{
    switch (p->state) {
    case UI_LINK_PAIR_WORKING: return PAIR_FOUND;
    case UI_LINK_PAIR_COMPARE: return PAIR_COMPARE;
    case UI_LINK_PAIR_WAITING: return PAIR_WAITING;
    case UI_LINK_PAIR_DONE: return PAIR_DONE;
    case UI_LINK_PAIR_FAILED:
        /* 3 failed attempts close the alarm panel's window: then the looking screen says what to
         * check (as nothing found) */
        return p->fail == UI_LINK_FAIL_NOT_FOUND || p->fail == UI_LINK_FAIL_LIMIT ? PAIR_NOT_FOUND
                                                                                  : PAIR_FAILED;
    default: return p->left_s >= PAIR_NOT_FOUND_S ? PAIR_NOT_FOUND : PAIR_LOOKING;
    }
}

static uint32_t pair_sig(const ui_link_pair_t *p)
{
    uint32_t h = 2166136261u;
    const uint32_t v[] = {(uint32_t)pair_view(p), p->fail,
                          p->state == UI_LINK_PAIR_FAILED ? 1u : 0u};
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        h = (h ^ v[i]) * 16777619u;
    }
    for (const char *c = p->code; *c != '\0'; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    for (const char *c = p->peer_name; *c != '\0'; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    h = (h ^ p->wifi_setup) * 16777619u;
    h = (h ^ (unsigned)p->online) * 16777619u;
    return h;
}

/* "588 460": two groups of three are easier to compare (as the Home Assistant page). */
static void pair_code(char *out, size_t len)
{
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; s_pair.p.code[i] != '\0' && n + 2u < len; i++) {
        if (i == 3u) {
            out[n++] = ' ';
        }
        out[n++] = s_pair.p.code[i];
        out[n] = '\0';
    }
}

static void pair_confirm(bool allow)
{
    (void)pair_ask(UI_LINK_REQ_KP_PAIR_CONFIRM, allow);
    pair_read();
    if (pair_view(&s_pair.p) == PAIR_DONE) {
        s_enrolled = true;
        (void)pair_ask(UI_LINK_REQ_KP_WIFI, true);
        pair_read();
        s_step = STEP_KP_DONE;
    }
    build_step();
}

static void pair_no(void *ctx)
{
    (void)ctx;
    pair_confirm(false);
}

static void pair_yes(void *ctx)
{
    (void)ctx;
    pair_confirm(true);
}

/* "Found Harbor House", the alarm panel's address under it. */
static void found_row(lv_obj_t *b)
{
    char title[96];
    char detail[96];
    /* TR: the setup's pairing step: an alarm panel open for pairing was found; {0} is its name.
     * At most 60 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Found {0}"),
                s_pair.p.peer_name[0] != '\0' ? s_pair.p.peer_name : ui_link_master_name());
    /* TR: the setup's pairing step: what was found (its address follows). At most 47 bytes */
    const char *what = TR("The alarm panel");
    i18n_utf8_copy(detail, sizeof(detail), what);
    if (s_pair.p.peer_addr[0] != '\0') {
        const size_t n = strlen(detail);
        /* two facts, " · " between them (the same in every language) */
        i18n_utf8_copy(detail + n, sizeof(detail) - n, " \xC2\xB7 ");
        const size_t m = strlen(detail);
        i18n_utf8_copy(detail + m, sizeof(detail) - m, s_pair.p.peer_addr);
    }
    ui_row_link(b, UI_ICON_HOUSE, ui_tone_green(), title, detail, NULL, NULL, NULL, NULL);
}

/* The ways pairing ends without a keypad: the row says what happened, the line what to do. */
static void pair_failed(lv_obj_t *b)
{
    const char *glyph = UI_ICON_CIRCLE_X;
    lv_color_t tone = ui_tone_red();
    const char *title;
    const char *words;
    lv_color_t words_tone = ui_tone_gray();
    switch (s_pair.p.fail) {
    case UI_LINK_FAIL_MISMATCH:
        /* TR: the setup's pairing step after No on either screen. At most 63 bytes */
        title = TR("Not paired: the codes didn't match");
        words = TR("If they differ again, don't pair: something on your network is in between.");
        words_tone = ui_tone_amber();
        break;
    case UI_LINK_FAIL_REFUSED:
    case UI_LINK_FAIL_CANCELLED:
        /* TR: the setup's pairing step: the installer tapped No, reject at the alarm panel. At most
         * 63 bytes */
        title = TR("Not paired: rejected at the alarm panel");
        words = TR("Ask the installer to allow it there, then try again.");
        break;
    case UI_LINK_FAIL_TIMEOUT:
    case UI_LINK_FAIL_CLOSED:
    case UI_LINK_FAIL_INTERRUPTED:
        glyph = UI_ICON_CLOCK_ALERT;
        tone = ui_tone_amber();
        /* TR: the setup's pairing step: either side waited too long (LINK-016). At most 63 bytes */
        title = TR("Not paired: no answer in time");
        words = TR("Both screens must answer within 2 minutes. Open Add a keypad on the alarm "
                   "panel again, "
                   "then try again.");
        break;
    case UI_LINK_FAIL_FULL:
        /* TR: the setup's pairing step: the alarm panel has no keypad number free (LINK-001). At
         * most 63 bytes */
        title = TR("Not paired: no keypad number is free");
        words = TR(
            "Keypad numbers 1 to 8 are all in use on the alarm panel. Remove one there first, then "
            "try again.");
        break;
    case UI_LINK_FAIL_TWO_MASTERS:
        /* TR: the setup's pairing step: two alarm panels were open for pairing at once. At most 63
         * bytes */
        title = TR("Not paired: two alarm panels are open");
        words = TR("Close Add a keypad on the one this keypad isn't for, then try again.");
        break;
    case UI_LINK_FAIL_OVERLAP:
        /* TR: the setup's pairing step: another keypad was pairing at the same time. At most 63
         * bytes */
        title = TR("Not paired: another keypad was pairing too");
        words = TR("Pair one keypad at a time, then try again.");
        break;
    default:
        /* TR: the setup's pairing step: it ended for another reason. At most 63 bytes */
        title = TR("Not paired");
        words = TR("Open Add a keypad on the alarm panel again, then try again.");
        break;
    }
    /* TR: the setup's pairing step: nothing was stored on either side. At most 63 bytes */
    ui_row_link(b, glyph, tone, title, TR("Nothing was saved"), NULL, NULL, NULL, NULL);
    ui_paragraph(b, words, words_tone);
}

static void build_pair(lv_obj_t *b)
{
    side(UI_ICON_HOUSE, TR("Pair with the alarm panel"),
         TR("Find it nearby, or select Wi-Fi if it is out of range."));
    footer(TR("Next"), true);
    ui_set_int(&subject_wiz_next_on, 0); /* the step goes on by itself */
    s_pair.shown = pair_sig(&s_pair.p);
    char code[16];
    pair_code(code, sizeof(code));
    switch (pair_view(&s_pair.p)) {
    case PAIR_NOT_FOUND: {
        ui_row_link(b, UI_ICON_SEARCH, ui_tone_amber(), TR("Still looking"),
                    TR("No alarm panel with pairing open yet"), NULL, NULL, NULL, NULL);
        /* TR: the setup's pairing step after 30 s with nothing found: the heading over what to
         * check */
        ui_paragraph(b, TR("Open Add keypad in Panel Manager or on the alarm panel."), ui_tone_gray());
        if (s_pair.p.state == UI_LINK_PAIR_FAILED) {
            footer(TR("Try again"), true); /* it stopped looking: a person starts it again */
        }
        break;
    }
    case PAIR_FOUND: found_row(b); break;
    case PAIR_COMPARE:
        found_row(b);
        ui_big_value(b, TR("Does the alarm panel show this code?"), code);
        ui_action_pair(b, UI_ICON_CIRCLE_X, TR("No, cancel"), ui_tone_red(), pair_no,
                       UI_ICON_CIRCLE_CHECK, TR("Yes, pair"), ui_tone_green(), pair_yes, NULL);
        ui_paragraph(b, TR("If the codes differ, something on your network is in between: tap No."),
                     ui_tone_amber());
        break;
    case PAIR_WAITING:
    case PAIR_DONE:
        ui_row_link(b, UI_ICON_CIRCLE_CHECK, ui_tone_green(), TR("The codes match"),
                    TR("Now allow it on the alarm panel"), NULL, NULL, NULL, NULL);
        ui_big_value(b, TR("The code on both screens"), code);
        ui_paragraph(b, TR("The installer has 2 minutes to allow it."), ui_tone_gray());
        break;
    case PAIR_FAILED:
        pair_failed(b);
        footer(TR("Try again"), true);
        break;
    default:
        /* TR: the setup's pairing step while it looks (nothing moves: no spinner) */
        ui_row_link(b, UI_ICON_SEARCH, ui_tone_blue(), TR("Looking for the alarm panel"),
                    TR("Nearby or on your Wi-Fi"), NULL, NULL, NULL, NULL);
        ui_paragraph(b, TR("Open Add keypad in Panel Manager or on the alarm panel."), ui_tone_gray());
        break;
    }
    if (pair_view(&s_pair.p) == PAIR_LOOKING || pair_view(&s_pair.p) == PAIR_NOT_FOUND ||
        pair_view(&s_pair.p) == PAIR_FAILED)
        ui_row_link(b, UI_ICON_WIFI, ui_tone_blue(), TR("Select Wi-Fi"),
                    TR("If the alarm panel is out of range"), NULL, UI_ICON_CHEVRON_RIGHT, select_wifi, NULL);
}

/* The keypad branch's last step: both sides checked each other before anything was stored
 * (LINK-016). Done stores the role and restarts the panel as a keypad. */
static void retry_wifi(void *ctx)
{
    (void)ctx;
    (void)pair_ask(UI_LINK_REQ_KP_WIFI, true);
    pair_read();
    build_step();
}

static void build_kp_done(lv_obj_t *b)
{
    const bool connected = ui_net_connected();
    const bool waiting = s_pair.p.wifi_setup == UI_LINK_WIFI_REQUESTING || s_pair.p.wifi_setup == UI_LINK_WIFI_JOINING;
    side(UI_ICON_HOUSE, TR("Keypad paired"),
         TR("Your pairing is saved. Language, display and sound stay on this keypad."));
    ui_row_link(b, UI_ICON_HOUSE, s_pair.p.online ? ui_tone_green() : ui_tone_amber(),
                s_pair.p.online ? TR("Connected to the alarm panel") : TR("Reconnecting to the alarm panel"),
                NULL, NULL, NULL, NULL, NULL);
    ui_row_link(b, UI_ICON_WIFI, connected ? ui_tone_green() : ui_tone_blue(),
                connected ? TR("Wi-Fi connected") : waiting ? TR("Setting up Wi-Fi") : TR("Wi-Fi is not connected"),
                waiting ? TR("Getting settings from the alarm panel and joining its network.") : NULL,
                NULL, NULL, NULL, NULL);
    if (!connected) {
        ui_row_link(b, UI_ICON_WIFI, ui_tone_blue(), TR("Select Wi-Fi"), NULL, NULL,
                    UI_ICON_CHEVRON_RIGHT, select_wifi, NULL);
        if (!waiting) ui_row_link(b, UI_ICON_REFRESH, ui_tone_blue(), TR("Try again"),
                                 NULL, NULL, NULL, retry_wifi, NULL);
    }
    footer(connected ? TR("Done") : TR("Continue without Wi-Fi"), false);
    ui_set_int(&subject_wiz_next_on, s_pair.p.online && !waiting ? 1 : 0);
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

/* The dot a step lights: five for the alarm panel (the date and time shares the time zone's), six
 * with the role first; a keypad's four (BL-048: the role step is a counted dot). */
static int32_t dot_of(int32_t step)
{
    if (s_keypad) {
        switch (step) {
        case STEP_ROLE: return 1;
        case STEP_NEAR:
        case STEP_WIFI: return 2;
        case STEP_PAIR: return 3;
        case STEP_KP_DONE: return 4;
        default: return 0;
        }
    }
    const int32_t role = s_role ? 1 : 0;
    switch (step) {
    case STEP_ROLE: return 1;
    case STEP_WIFI: return 1 + role;
    case STEP_TIME:
    case STEP_CLOCK: return 2 + role;
    case STEP_CODES: return 3 + role;
    case STEP_NAME: return 4 + role;
    case STEP_DONE: return 5 + role;
    default: return 0;
    }
}

static void build_step(void)
{
    ui_rows_reset();
    ui_set_int(&subject_busy, 0);
    ui_set_int(&subject_wiz_steps, s_keypad ? 4 : s_role ? 6 : 5);
    ui_set_int(&subject_wiz_step, dot_of(s_step));
    /* "Leave setup" is gone from the last step: Done ends the setup */
    ui_set_int(&subject_wiz_exit,
               s_can_exit && !s_enrolled && s_step != STEP_DONE && s_step != STEP_KP_DONE ? 1 : 0);
    lv_obj_t *b = body();
    switch (s_step) {
    case STEP_LANGUAGE: build_language(b); break;
    case STEP_NEAR: build_near(b); break;
    case STEP_ROLE: build_role(b); break;
    case STEP_WIFI: s_keypad ? build_keypad_wifi(b) : build_wifi(b); break;
    case STEP_PAIR: build_pair(b); break;
    case STEP_KP_DONE: build_kp_done(b); break;
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
    s_role = false;
    s_keypad = false;
    s_enrolled = false;
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
    /* The role is chosen once, on a first start (changing it is a factory reset): run again from
     * Installer, the alarm panel's setup is as it was (BL-048). */
    s_role = !can_exit && ui_link_master();
    s_keypad = false;
    s_enrolled = false;
    ui_set_int(&subject_wiz_exit, can_exit ? 1 : 0);
    s_step = ui_lang_offered() ? STEP_LANGUAGE : STEP_WELCOME;
    if (ui_link_keypad()) {
        s_keypad = true;
        pair_read();
        s_enrolled = s_pair.p.state == UI_LINK_PAIR_DONE;
        if (s_enrolled) {
            s_step = STEP_KP_DONE;
            (void)pair_ask(UI_LINK_REQ_KP_WIFI, true);
        } else s_step = STEP_NEAR;
    }
    build_step();
    lv_screen_load(S.wizard);
}

void ui_wizard_pair_again(void)
{
    ui_pad_cancel();
    ui_sheet_close();
    if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS) {
        ui_set_int(&subject_view, VIEW_HOME);
    }
    S.wizard_on = true;
    S.wizard_paused = false;
    s_can_exit = true; /* Leave setup goes back to Home, still a keypad */
    s_role = false;
    s_keypad = true;
    s_enrolled = false;
    ui_set_int(&subject_wiz_exit, 1);
    s_step = STEP_PAIR;
    pair_start();
    build_step();
    lv_screen_load(S.wizard);
}

static void finish(void);

/* The keypad's Done (BL-048): the role is stored and the panel restarts as a keypad. */
static void finish_keypad(void)
{
    const uint8_t r = pair_ask(UI_LINK_REQ_KP_DONE, false);
    if (r != UI_LINK_REQ_OK) {
        ui_beep(UI_BEEP_ERROR);
        /* TR: a toast when the keypad's setup can't store its role; the step stays */
        ui_toast(TR("Not saved: try Done again"));
        return;
    }
    finish();
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

/* The step after this one, on the branch the setup is on: the role only on a first start with the
 * link, the date and time only where nothing else sets the clock (RTC-015). */
static int32_t next_step(int32_t step)
{
    switch (step) {
    case STEP_LANGUAGE: return STEP_WELCOME;
    case STEP_WELCOME: return s_role ? STEP_ROLE : STEP_WIFI;
    case STEP_ROLE: return s_keypad ? STEP_NEAR : STEP_WIFI;
    case STEP_NEAR: return STEP_PAIR;
    case STEP_WIFI: return s_keypad ? (s_enrolled ? STEP_KP_DONE : STEP_PAIR) : STEP_TIME;
    case STEP_TIME: return ui_clock_setup_needed() ? STEP_CLOCK : STEP_CODES;
    case STEP_CLOCK: return STEP_CODES;
    case STEP_CODES: return STEP_NAME;
    case STEP_NAME: return STEP_DONE;
    case STEP_PAIR: return STEP_KP_DONE;
    default: return step;
    }
}

static int32_t prev_step(int32_t step)
{
    switch (step) {
    case STEP_WELCOME: return STEP_LANGUAGE;
    case STEP_ROLE: return STEP_WELCOME;
    case STEP_NEAR: return s_role ? STEP_ROLE : STEP_WELCOME;
    case STEP_WIFI: return s_keypad ? (s_enrolled ? STEP_KP_DONE : STEP_NEAR) : (s_role ? STEP_ROLE : STEP_WELCOME);
    case STEP_TIME: return STEP_WIFI;
    case STEP_CLOCK: return STEP_TIME;
    case STEP_CODES: return ui_clock_setup_needed() ? STEP_CLOCK : STEP_TIME;
    case STEP_NAME: return STEP_CODES;
    case STEP_DONE: return STEP_NAME;
    case STEP_PAIR: return STEP_NEAR;
    default: return step; /* the keypad's Done has no Back */
    }
}

/* Moves one step on or back, passing the date and time step when it isn't offered. */
static void go(int32_t dir)
{
    const int32_t from = s_step;
    s_step = dir > 0 ? next_step(s_step) : prev_step(s_step);
    if (s_step == STEP_CLOCK) {
        ui_clock_fields_start();
    }
    if (from == STEP_PAIR && s_step != STEP_PAIR && !s_enrolled) {
        keypad_branch_left(); /* Back stops looking, and forgets a pairing that just finished */
    }
    if (s_step == STEP_PAIR && from != STEP_PAIR) {
        pair_start();
    }
    build_step();
}

void ui_wizard_next(void)
{
    if (!S.wizard_on || lv_subject_get_int(&subject_wiz_next_on) == 0) {
        return;
    }
    if (s_step == STEP_DONE) {
        if (s_role) {
            keypad_branch_left(); /* the alarm panel's Done: nothing of the keypad branch stays */
        }
        finish();
        return;
    }
    if (s_step == STEP_KP_DONE) {
        pair_read();
        if (!s_pair.p.online) { build_step(); return; }
        finish_keypad();
        return;
    }
    if (s_step == STEP_PAIR) {
        /* Next is lit only on a failure: Try again (the step goes on by itself otherwise) */
        (void)pair_ask(UI_LINK_REQ_KP_PAIR_RETRY, false);
        pair_read();
        build_step();
        return;
    }
    if (s_step == STEP_CLOCK && !ui_clock_set_chosen()) {
        return; /* the error beep and toast are up; the step stays */
    }
    go(1);
}

void ui_wizard_back(void)
{
    if (S.wizard_on && s_step != STEP_KP_DONE &&
        s_step > (ui_lang_offered() ? STEP_LANGUAGE : STEP_WELCOME)) {
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
    if (s_step == STEP_PAIR) {
        keypad_branch_left(); /* Pair again left: it stops looking */
    }
    S.wizard_on = false;
    S.wizard_paused = false;
    s_can_exit = false;
    s_keypad = false;
    s_enrolled = false;
    s_step = STEP_WELCOME;
    ui_set_int(&subject_wiz_exit, 0);
    ui_show_main();
}

void ui_wizard_tick(void)
{
    if (!S.wizard_on || S.wizard_paused) {
        return;
    }
    if (s_step == STEP_WIFI && s_keypad) {
        ui_set_int(&subject_wiz_next_on, ui_net_connected() ? 1 : 0); /* no skipping on a keypad */
    } else if (s_step == STEP_WIFI) {
        ui_set_str(&subject_wiz_next, ui_net_connected() ? TR("Next") : TR("Skip for now"));
    }
    /* A stored keypad may publish its pairing after the UI first starts. Resume
     * setup instead of asking it to enroll a second time after a power interruption. */
    if (s_step == STEP_NEAR && ui_link_keypad() && lv_tick_elaps(s_pair.polled) >= 1000u) {
        pair_read();
        if (s_pair.p.state == UI_LINK_PAIR_DONE) {
            s_enrolled = true;
            (void)pair_ask(UI_LINK_REQ_KP_WIFI, true);
            pair_read();
            s_step = STEP_KP_DONE;
            build_step();
        }
    }
    /* The pairing, at most once a second (UI-020): the step changes only when what it shows does */
    if (s_step == STEP_KP_DONE && lv_tick_elaps(s_pair.polled) >= 1000u) {
        const uint32_t before = pair_sig(&s_pair.p);
        pair_read();
        if (before != pair_sig(&s_pair.p)) build_step();
    }
    if (s_step == STEP_PAIR && lv_tick_elaps(s_pair.polled) >= 1000u) {
        pair_read();
        if (pair_view(&s_pair.p) == PAIR_DONE) {
            s_enrolled = true;
            (void)pair_ask(UI_LINK_REQ_KP_WIFI, true);
            pair_read();
            s_step = STEP_KP_DONE;
            build_step();
        } else if (pair_sig(&s_pair.p) != s_pair.shown &&
                   lv_subject_get_int(&subject_sheet) == SHEET_NONE) {
            build_step();
        }
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
