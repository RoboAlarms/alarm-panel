/*
 * ui_settings.c - the Settings view: the sections in the left column, who may open them
 * (docs/features/14, "Who can change what"), a stack of pages on the right, and the pages that
 * don't need installer mode: display and sound (anyone; a change of them from elsewhere comes
 * through here too, UI-046), users (a master code) and about. The
 * event log is Activity's (ui_app.c, BL-131 finding 7). Wi-Fi and date and time live in ui_net.c,
 * integrations in ui_integrations.c, installer pages in ui_installer.c.
 *
 * A code typed here opens every section it allows. It stays in RAM only while Settings is open
 * (S.auth_code) and is wiped when Settings closes or after five idle minutes (the view goes
 * back to Home).
 */
#include <stdio.h>
#include <string.h>

#include "ui_backend/ui_prefs.h"
#include "ui_internal.h"

#define SAVE_AFTER_MS 1500u

static bool s_opts_dirty;
static uint32_t s_opts_changed;
static uint32_t s_page_refreshed;
static bool s_auto_ask; /* a locked section opens the code pad once, when it's chosen */
static ui_access_t s_ask_access;
static char s_ask_title[32];

/* ---- page stack ---------------------------------------------------------------------------- */

void ui_page_note(const char *note)
{
    ui_set_str(&subject_page_note, note);
    /* A page without a note has none: no empty line under its title (BL-131 rule 6). */
    lv_obj_t *label = ui_find(S.main, "page_note");
    if (label != NULL) {
        lv_obj_set_flag(label, LV_OBJ_FLAG_HIDDEN, note == NULL || note[0] == '\0');
    }
}

static void build_page(void);

/* A page made again (its state changed) stays where the person scrolled it; a page opened or gone
 * back to starts at its top (B015 UX-028). */
static bool s_keep_scroll;
static int32_t s_kept_y;

void ui_page_rebuild(void)
{
    if (S.depth == 0u || lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return;
    }
    s_kept_y = lv_obj_get_scroll_y(S.page_body);
    s_keep_scroll = true;
    build_page();
    s_keep_scroll = false;
}

void ui_page_open(page_id_t id, int32_t arg)
{
    if (S.depth >= UI_MAX_PAGES) {
        return;
    }
    S.pages[S.depth].id = id;
    S.pages[S.depth].arg = arg;
    S.depth++;
    S.page_fresh = true;
    build_page();
    S.page_fresh = false;
}

void ui_page_back(void)
{
    if (S.depth <= 1u) {
        return;
    }
    if (S.program) {
        ui_installer_apply(); /* leaving an installer page saves it */
    }
    S.depth--;
    build_page();
}

static void page_root(page_id_t id)
{
    S.depth = 0;
    ui_page_open(id, 0);
}

/* ---- sections ------------------------------------------------------------------------------ */

static void forget_codes(void)
{
    S.auth_user = 0;
    memset(S.auth_code, 0, sizeof(S.auth_code));
}

static void save_options_now(void)
{
    if (s_opts_dirty) {
        s_opts_dirty = false;
        ui_options_changed();
    }
}

static void select_section(int32_t section)
{
    /* installer mode lives under About (BL-189); on a keypad, in its own section (BL-223) */
    if (S.program && section != (ui_link_keypad() ? SECTION_INSTALLER : SECTION_ABOUT)) {
        ui_installer_leave();
    }
    save_options_now();
    ui_pad_cancel();
    ui_sheet_close();
    ui_set_int(&subject_settings_section, section);
    s_auto_ask = true;
    switch (section) {
    case SECTION_USERS: page_root(PAGE_USERS); break;
    case SECTION_WIFI: page_root(PAGE_WIFI); break;
    case SECTION_TIME: page_root(PAGE_TIME); break;
    case SECTION_INTEGRATIONS: page_root(PAGE_INTEGRATIONS); break;
    case SECTION_ABOUT: page_root(PAGE_ABOUT); break;
    case SECTION_INSTALLER: page_root(PAGE_INSTALLER); break; /* a keypad's (BL-223) */
    case SECTION_WALK: page_root(PAGE_WALK); break;
#if ALARM_UI_AUTOMATIONS
    case SECTION_AUTOMATIONS: page_root(PAGE_AUTOMATIONS); break;
#endif
#if ALARM_UI_DEVICES
    case SECTION_SMART_HOME: page_root(PAGE_SMART_HOME); break;
#endif
    default: page_root(PAGE_DISPLAY); break;
    }
}

void ui_settings_init(void)
{
    S.depth = 0;
}

/* The role gate (BL-048): a keypad holds no users, integrations, automations or walk test - they
 * are the alarm panel's. Its alarm panel's Users and Installer show under that panel's name once
 * the alarm panel serves them (BL-223, LINK-057; the others not yet); on an alarm panel, Installer
 * is About's row (BL-189). */
static bool section_gated(int32_t section)
{
    if (!ui_link_keypad()) {
        return false;
    }
    switch (section) {
    case SECTION_USERS: return !ui_link_remote(UI_LINK_AREA_USERS);
    case SECTION_INSTALLER: return !ui_link_remote(UI_LINK_AREA_SESSION | UI_LINK_AREA_CONFIG);
    case SECTION_INTEGRATIONS:
    case SECTION_WALK:
    case SECTION_AUTOMATIONS:
    case SECTION_SMART_HOME: return true;
    default: return false;
    }
}

unsigned ui_installer_root(void)
{
    return ui_link_keypad() ? 0u : 1u;
}

/* A keypad's list (BL-223): its own four under "This keypad", then its alarm panel's under that
 * panel's name; the lines are moved once, the headings shown with something under them. */
static void keypad_lines(bool remote)
{
    static const char *const order[] = {
        NOTR("set_head_local"), NOTR("set_display"),     NOTR("set_wifi"),
        NOTR("set_time"),       NOTR("set_about"),       NOTR("set_head_master"),
        NOTR("set_users"),      NOTR("set_installer"),   NOTR("set_integrations"),
        NOTR("set_automations"), NOTR("set_smarthome"),  NOTR("set_walk"),
    };
    static bool ordered;
    lv_obj_t *nav = ui_find(S.main, "settings_nav");
    if (nav != NULL && !ordered) {
        ordered = true;
        for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
            lv_obj_t *line = ui_find(nav, order[i]);
            if (line != NULL) {
                lv_obj_move_to_index(line, (int32_t)i);
            }
        }
    }
    lv_obj_t *local = ui_find(S.main, "set_head_local");
    lv_obj_t *master = ui_find(S.main, "set_head_master");
    if (local != NULL) {
        lv_obj_set_flag(local, LV_OBJ_FLAG_HIDDEN, !remote);
    }
    if (master != NULL) {
        lv_obj_set_flag(master, LV_OBJ_FLAG_HIDDEN, !remote);
        const char *name = ui_link_master_name(); /* a value, never translated */
        /* TR: a keypad's Settings list, the heading over its alarm panel's sections when the
         * panel's name isn't known. At most 47 bytes */
        lv_label_set_text(master, name[0] != '\0' ? name : TR("Alarm panel"));
    }
}

void ui_settings_gate(void)
{
    static const struct {
        const char *name;
        int32_t section;
    } lines[] = {
        {NOTR("set_users"), SECTION_USERS},
        {NOTR("set_integrations"), SECTION_INTEGRATIONS},
        {NOTR("set_walk"), SECTION_WALK},
        {NOTR("set_installer"), SECTION_INSTALLER},
    };
    if (S.main == NULL) {
        return;
    }
    bool remote = false;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        lv_obj_t *line = ui_find(S.main, lines[i].name);
        bool hide = section_gated(lines[i].section);
        if (lines[i].section == SECTION_INSTALLER) {
            hide = hide || !ui_link_keypad(); /* an alarm panel's is About's row (BL-189) */
        }
#if !ALARM_UI_INTEGRATIONS && !ALARM_UI_NOTIFY
        /* Nothing to integrate with in this firmware: no Integrations section (docs/features/33).
         */
        hide = hide || lines[i].section == SECTION_INTEGRATIONS;
#endif
        remote = remote || (ui_link_keypad() && !hide);
        if (line != NULL) {
            lv_obj_set_flag(line, LV_OBJ_FLAG_HIDDEN, hide);
        }
    }
    if (ui_link_keypad()) {
        keypad_lines(remote);
    }
}

/* A keypad's page of its alarm panel's: the open section is the alarm panel's. */
static bool remote_page(void)
{
    if (!ui_link_keypad()) {
        return false;
    }
    const int32_t section = lv_subject_get_int(&subject_settings_section);
    return section == SECTION_USERS || section == SECTION_INSTALLER;
}

void ui_settings_remote_changed(void)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS || S.depth == 0u || !remote_page()) {
        return;
    }
    if (section_gated(lv_subject_get_int(&subject_settings_section))) {
        select_section(SECTION_DISPLAY); /* the alarm panel no longer serves it */
        return;
    }
    if (lv_subject_get_int(&subject_sheet) == SHEET_NONE &&
        lv_subject_get_int(&subject_settings_pad) == 0) {
        ui_page_rebuild(); /* its answers changed, or the link went or came back (LINK-063) */
    }
}

void ui_settings_open(void)
{
    ui_update_tag(); /* the list is right from the start */
#if ALARM_UI_AUTOMATIONS
    /* The Automations line, once the backend has them (docs/features/41); with Event log gone
     * to Activity there is room for it (BL-131 finding 7). Not on a keypad (BL-048). */
    ui_set_int(&subject_automations_avail, ui_automations_available() && !ui_link_keypad() ? 1 : 0);
#endif
#if ALARM_UI_DEVICES
    /* Smart Home's line (BL-220): the alarm panel's, while it has a device; not on a keypad */
    ui_set_int(&subject_smarthome_avail, ui_devices_settings_available() && !ui_link_keypad() ? 1 : 0);
#endif
    ui_settings_gate();
    ui_set_int(&subject_view, VIEW_SETTINGS);
    int32_t section = lv_subject_get_int(&subject_settings_section);
    if (section_gated(section)) {
        section = SECTION_DISPLAY;
    }
    if (S.program && ui_link_keypad()) { /* a keypad's installer section (BL-223) */
        select_section(SECTION_INSTALLER);
        return;
    }
    if (S.program) { /* About, then the Installer menu on top of it */
        select_section(SECTION_ABOUT);
        ui_page_open(PAGE_INSTALLER, 0);
        return;
    }
    select_section(section == SECTION_ABOUT ? section : SECTION_DISPLAY);
}

void ui_settings_section(const char *which)
{
    static const struct {
        const char *name;
        int32_t section;
    } names[] = {
        /* ids from the caller, not words on the screen; the language is under Time and
         * language (BL-131) */
        {NOTR("users"), SECTION_USERS},     {NOTR("wifi"), SECTION_WIFI},
        {NOTR("time"), SECTION_TIME},       {NOTR("integrations"), SECTION_INTEGRATIONS},
        {NOTR("installer"), SECTION_INSTALLER},
        {NOTR("about"), SECTION_ABOUT},     {NOTR("language"), SECTION_TIME},
        {NOTR("walk"), SECTION_WALK},
#if ALARM_UI_AUTOMATIONS
        {NOTR("automations"), SECTION_AUTOMATIONS},
#endif
#if ALARM_UI_DEVICES
        {NOTR("smarthome"), SECTION_SMART_HOME},
#endif
    };
    int32_t section = SECTION_DISPLAY;
    for (size_t i = 0; which != NULL && i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(which, names[i].name) == 0) {
            section = names[i].section;
        }
    }
    if (section_gated(section)) {
        section = section == SECTION_INSTALLER ? SECTION_ABOUT : SECTION_DISPLAY; /* a keypad's */
    }
    if (section == SECTION_INSTALLER && ui_link_keypad()) { /* a section of a keypad's (BL-223) */
        select_section(SECTION_INSTALLER);
        return;
    }
    if (section == SECTION_INSTALLER) { /* the Installer page is About's (BL-189) */
        select_section(SECTION_ABOUT);
        ui_page_open(PAGE_INSTALLER, 0);
        return;
    }
    select_section(section);
}

void ui_settings_leave(void)
{
    if (S.program) {
        ui_installer_leave();
    }
    save_options_now();
    ui_pad_cancel();
    ui_sheet_close();
    ui_rows_reset();
    ui_notify_page_left(); /* the QR code never outlives the screen it was asked for */
    ui_voice_page_preview_stop(); /* Settings left, timed out or taken by an alarm */
    forget_codes();
    S.depth = 0;
    ui_fwsrv_stop(); /* a check of an update server is forgotten */
}

static void announcements_tick(void);
static void prefs_remote_tick(void);

void ui_settings_tick(void)
{
    ui_storage_tick();
    ui_voice_tick();
    announcements_tick();
    if (s_opts_dirty && lv_tick_elaps(s_opts_changed) >= SAVE_AFTER_MS) {
        save_options_now();
    }
    prefs_remote_tick();
    if (S.program && S.be.program_keepalive != NULL &&
        lv_display_get_inactive_time(S.disp) < 5000u && lv_tick_elaps(S.keepalive_at) >= 30000u) {
        S.keepalive_at = lv_tick_get();
        S.be.program_keepalive(S.be.ctx); /* the installer is active on this screen */
    }
    /* The locked installer page counts the recovery window down (and redraws once when it
     * closes). */
    static bool counting;
    const alarm_ms_t now = S.be.now_ms(S.be.ctx);
    const alarm_ms_t elapsed = now > S.snap.taken_ms ? now - S.snap.taken_ms : 0u;
    const bool open = S.snap.recovery_left_ms > elapsed;
    if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS && S.depth == 2u &&
        S.pages[1].id == PAGE_INSTALLER && !S.program && (open || counting) &&
        lv_subject_get_int(&subject_settings_pad) == 0 &&
        lv_subject_get_int(&subject_sheet) == SHEET_NONE &&
        lv_tick_elaps(s_page_refreshed) >= 1000u) {
        counting = open;
        build_page();
    }
}

void ui_settings_language_changed(void)
{
    if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS && S.depth > 0u) {
        build_page();
    }
}

void ui_settings_refresh(void)
{
    /* Installer mode ended elsewhere (timeout, or left from another keypad): don't leave its
     * pages open - and not its menu either, which was built unlocked. The menu is rebuilt
     * once, at the transition; deeper pages whenever they are found. */
    static bool was_program;
    const unsigned root = ui_installer_root();
    if (!S.program && S.depth > 0u && lv_subject_get_int(&subject_view) == VIEW_SETTINGS &&
        S.depth >= root + 1u && S.pages[root].id == PAGE_INSTALLER &&
        (S.depth > root + 1u || was_program)) {
        S.depth = root; /* About (on a keypad: nothing); the locked Installer page on top */
        ui_page_open(PAGE_INSTALLER, 0);
    }
    was_program = S.program;
}

/* ---- who may open what ---------------------------------------------------------------------- */

static bool user_allowed(ui_access_t access, uint8_t user)
{
    if (access == ACCESS_ANYONE) {
        return true;
    }
    if (user == 0u || user > ALARM_MAX_USERS) {
        return false;
    }
    const alarm_authority_t a = S.cfg->users[user - 1u].authority;
    switch (access) {
    case ACCESS_USERS:
    case ACCESS_SHARED:
        return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_PARTITION_MASTER ||
               a == ALARM_AUTH_INSTALLER;
    case ACCESS_SYSTEM: return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_INSTALLER;
    default: return false;
    }
}

bool ui_settings_allowed(ui_access_t access)
{
    if (access != ACCESS_USERS && access != ACCESS_ANYONE && S.program) {
        return true; /* installer mode counts as the installer code */
    }
    return user_allowed(access, S.auth_user);
}

void ui_settings_remember(uint8_t user, const char *code)
{
    if (code == NULL || code[0] == '\0') {
        return; /* the power-up recovery opens installer mode without one: keep what we have */
    }
    /* The installer code opens everything a master code does (Users too, since 2026-09-23),
     * so the code typed last is the one Settings works with. */
    S.auth_user = user;
    snprintf(S.auth_code, sizeof(S.auth_code), "%s", code);
}

static const char *access_hint(ui_access_t access)
{
    switch (access) {
    case ACCESS_SYSTEM: /* TR: the code pad's hint and the row that opens it, on a locked page */
        return TR("Enter the master or installer code");
    default: /* TR: the code pad's hint and the row that opens it, on a locked page */
        return TR("Enter a master or installer code");
    }
}

static bool unlock_with(const char *code);

static void unlock_checked(const ui_call_t *c)
{
    if (!ui_pad_asks(unlock_with)) {
        return; /* the pad closed while the code was checked */
    }
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(c->r == ALARM_ERR_INVALID_CODE ? TR("Wrong code, try again")
                                                    : ui_result_text(c->r));
        return;
    }
    if (!user_allowed(s_ask_access, c->user)) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(TR("This code can't open this section"));
        return;
    }
    S.auth_user = c->user;
    snprintf(S.auth_code, sizeof(S.auth_code), "%s", c->code);
    ui_pad_cancel();
    build_page();
}

/* The pad stays open; the answer opens the section (unlock_checked). */
static bool unlock_with(const char *code)
{
    if (!ui_call_authenticate(code, unlock_checked, 0u)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

/* The section's icon, as on its line in the Settings list (screen_main.xml), and a colour: the
 * pad that unlocks a section carries them, so it reads as that section's door. */
static void section_look(const char **glyph, lv_color_t *tone)
{
    switch (lv_subject_get_int(&subject_settings_section)) {
    case SECTION_USERS: *glyph = UI_ICON_USERS; *tone = ui_tone_violet(); break;
    case SECTION_WIFI: *glyph = UI_ICON_WIFI; *tone = ui_tone_blue(); break;
    case SECTION_TIME: *glyph = UI_ICON_CALENDAR_CLOCK; *tone = ui_tone_green(); break;
    case SECTION_INTEGRATIONS: *glyph = UI_ICON_PUZZLE; *tone = ui_tone_violet(); break;
    case SECTION_ABOUT: *glyph = UI_ICON_INFO; *tone = ui_tone_rose(); break;
    case SECTION_WALK: *glyph = UI_ICON_FOOTPRINTS; *tone = ui_tone_blue(); break;
#if ALARM_UI_AUTOMATIONS
    case SECTION_AUTOMATIONS: *glyph = UI_ICON_ZAP; *tone = ui_tone_amber(); break;
#endif
#if ALARM_UI_DEVICES
    case SECTION_SMART_HOME: *glyph = UI_ICON_HOUSE_WIFI; *tone = ui_tone_blue(); break;
#endif
    default: *glyph = UI_ICON_SUN; *tone = ui_tone_blue(); break;
    }
}

static void ask_unlock(void *ctx)
{
    s_ask_access = (ui_access_t)(intptr_t)ctx;
    const char *glyph;
    lv_color_t tone;
    section_look(&glyph, &tone);
    ui_pad_ask(HOST_SETTINGS, glyph, s_ask_title, access_hint(s_ask_access), tone, unlock_with);
}

void ui_settings_locked(lv_obj_t *body, ui_access_t access, const char *title, const char *why,
                        char *note, size_t nlen)
{
    i18n_utf8_copy(s_ask_title, sizeof(s_ask_title), title); /* the title is cut to 31 bytes */
    i18n_utf8_copy(note, nlen, why);
    ui_row_action(body, UI_ICON_KEY, access_hint(access), ui_tone_violet(), ask_unlock,
                  (void *)(intptr_t)access);
    if (s_auto_ask && S.pad == PAD_NONE) {
        s_auto_ask = false;
        ask_unlock((void *)(intptr_t)access);
    }
}

void ui_settings_unlock_row(lv_obj_t *body, ui_access_t access, const char *title)
{
    i18n_utf8_copy(s_ask_title, sizeof(s_ask_title), title); /* the title is cut to 31 bytes */
    ui_row_action(body, UI_ICON_KEY, access_hint(access), ui_tone_violet(), ask_unlock,
                  (void *)(intptr_t)access);
}

/* ---- display and sound --------------------------------------------------------------------- */

static void options_dirty(void)
{
    s_opts_dirty = true;
    s_opts_changed = lv_tick_get();
}

static void on_brightness(int32_t v, void *ctx)
{
    (void)ctx;
    S.opt.brightness = (uint8_t)v;
    ui_light_preview(S.opt.brightness);
    options_dirty();
}

static void on_volume(int32_t v, void *ctx)
{
    (void)ctx;
    S.opt.volume = (uint8_t)v;
    if (S.be.set_volume != NULL) {
        S.be.set_volume(S.be.ctx, S.opt.volume);
    }
    options_dirty();
}

static void on_chime_volume(int32_t v, void *ctx)
{
    (void)ctx;
    S.opt.chime_volume = (uint8_t)v;
    if (S.be.set_chime_volume != NULL) {
        S.be.set_chime_volume(S.be.ctx, S.opt.chime_volume);
    }
    options_dirty();
}

static void on_voice_volume(int32_t v, void *ctx)
{
    (void)ctx;
    S.opt.voice_volume = (uint8_t)v;
    if (S.be.set_voice_volume != NULL) {
        S.be.set_voice_volume(S.be.ctx, S.opt.voice_volume);
    }
    options_dirty();
}

static void on_saver_brightness(int32_t v, void *ctx)
{
    (void)ctx;
    S.opt.saver_brightness = (uint8_t)v;
    options_dirty();
}

static void on_click_toggle(bool on, void *ctx)
{
    (void)ctx;
    S.opt.key_click = on;
    options_dirty();
}

/* Night hours (UI-043): the page is built again, so their rows come and go with the switch. */
static void on_night_toggle(bool on, void *ctx)
{
    (void)ctx;
    S.opt.night_on = on;
    s_opts_dirty = true;
    save_options_now();
    ui_page_rebuild(); /* where the person scrolled it */
}

static void on_night_brightness(int32_t v, void *ctx)
{
    (void)ctx;
    S.opt.night_brightness = (uint8_t)v;
    ui_light_preview(S.opt.night_brightness); /* how dim the night is, seen in daylight too */
    options_dirty();
}

/* The same choices as the Panel Manager's Display and sound page, for the screen saver and for
 * the screen going dark (by day and at night, UI-043). */
static const uint32_t SAVER_MS[] = {30000u,  60000u,   120000u,  300000u, 600000u,
                                    900000u, 1800000u, 3600000u, 0u};
static const char *const SAVER_TEXT[] = {
    /* TR: the screen saver's delay and the screen's before it goes dark, a choice in a list and
     * the value of its row */
    TR_NOOP_C("screen saver delay", "After 30 seconds"), TR_NOOP_C("screen saver delay", "After 1 minute"),
    TR_NOOP_C("screen saver delay", "After 2 minutes"),  TR_NOOP_C("screen saver delay", "After 5 minutes"),
    TR_NOOP_C("screen saver delay", "After 10 minutes"), TR_NOOP_C("screen saver delay", "After 15 minutes"),
    TR_NOOP_C("screen saver delay", "After 30 minutes"), TR_NOOP_C("screen saver delay", "After 1 hour"),
    TR_NOOP_C("screen saver delay", "Never")};
#define SAVER_CHOICES (sizeof(SAVER_MS) / sizeof(SAVER_MS[0]))

/* The three delays chosen from that list: the screen saver's, the screen's going dark, and the
 * same at night. */
enum { DELAY_SAVER = 0, DELAY_OFF, DELAY_NIGHT_OFF };
static int s_delay; /* the one whose list is open */

static uint32_t *delay_value(int which)
{
    return which == DELAY_OFF         ? &S.opt.off_after_ms
           : which == DELAY_NIGHT_OFF ? &S.opt.night_off_after_ms
                                      : &S.opt.saver_after_ms;
}

/* The choice a delay shows; one the Panel Manager set off the list shows as the next longer one
 * (or "Never"), which a pick then makes true. */
static int32_t delay_index(int which)
{
    const uint32_t ms = *delay_value(which);
    if (ms == 0u) {
        return (int32_t)(SAVER_CHOICES - 1u);
    }
    for (size_t i = 0; i + 1u < SAVER_CHOICES; i++) {
        if (SAVER_MS[i] >= ms) {
            return (int32_t)i;
        }
    }
    return (int32_t)(SAVER_CHOICES - 1u);
}

static void delay_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < SAVER_CHOICES) {
        *delay_value(s_delay) = SAVER_MS[index];
        s_opts_dirty = true;
        save_options_now();
    }
    ui_page_rebuild(); /* where the person scrolled it: the night's rows are far down */
}

static void pick_delay(void *ctx)
{
    s_delay = (int)(intptr_t)ctx;
    static ui_choice_t choices[SAVER_CHOICES];
    for (size_t i = 0; i < SAVER_CHOICES; i++) {
        choices[i].glyph = NULL;
        choices[i].tone = ui_tone_blue();
        choices[i].title = TR_C("screen saver delay", SAVER_TEXT[i]);
        choices[i].detail = NULL;
    }
    const char *title;
    const char *note;
    if (s_delay == DELAY_OFF) {
        /* TR: Settings > Display and sound, the title of the list of times before the screen
         * goes dark */
        title = TR("Screen off");
        /* TR: the line under that list's title. At most 127 bytes */
        note = TR("When nobody touches the screen, it goes dark; a touch wakes it");
    } else if (s_delay == DELAY_NIGHT_OFF) {
        /* TR: Settings > Display and sound, the title of the list of times before the screen
         * goes dark during the night hours */
        title = TR("Screen off at night");
        /* TR: the line under that list's title. At most 127 bytes */
        note = TR("During the night hours, the screen goes dark this soon after a touch");
    } else {
        title = TR("Screen saver");
        note = TR("When nothing happens, the screen dims to a photo with the time "
                  "and the alarm state");
    }
    ui_pick_sheet(title, note, choices, SAVER_CHOICES, delay_index(s_delay), delay_chosen);
}

static void delay_row(lv_obj_t *body, const char *glyph, int which, const char *title, const char *detail)
{
    ui_row_link(body, glyph, ui_tone_blue(), title, detail,
                TR_C("screen saver delay", SAVER_TEXT[delay_index(which)]), UI_ICON_CHEVRON_RIGHT,
                pick_delay, (void *)(intptr_t)which);
}

/* "10:00 PM to 6:00 AM": the night's hours as a person reads them. */
static void night_range_text(int32_t start, int32_t end, char *buf, size_t len)
{
    char a[24];
    char b[24];
    ui_minute_text((int)start, a, sizeof(a));
    ui_minute_text((int)end, b, sizeof(b));
    /* TR: a span of the day: its start {0} and end {1}, both times of day ("10:00 PM to 6:00 AM").
     * At most 63 bytes */
    I18N_FORMAT(buf, len, TR_C("night hours", "{0} to {1}"), a, b);
}

/* How the screen looks (UI-010, BL-132): Dark, Light or Automatic, and High contrast with any of
 * them. No code, like the brightness (UI-015). A change applies at once, in one step (UI-020):
 * ui_options_changed() switches the palette when the one asked for is new. */
static const char *const APPEARANCE_TEXT[] = {
    /* TR: Settings > Screen and sound, the appearances: a choice in the list and the value of
     * the Appearance row */
    TR_NOOP_C("appearance", "Dark"), TR_NOOP_C("appearance", "Light"), TR_NOOP_C("appearance", "Automatic")};
#define APPEARANCE_CHOICES (sizeof(APPEARANCE_TEXT) / sizeof(APPEARANCE_TEXT[0]))

static size_t appearance_index(void)
{
    return S.opt.appearance < APPEARANCE_CHOICES ? S.opt.appearance : (size_t)UI_APPEARANCE_DARK;
}

static void appearance_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < APPEARANCE_CHOICES) {
        S.opt.appearance = (uint8_t)index;
        s_opts_dirty = true;
        save_options_now(); /* the new appearance, at once */
    }
    ui_page_rebuild(); /* the row says the new choice */
}

#if ALARM_UI_DEVICES
/* Settings > Screen and sound > Start screen (BL-220 R8): Security or Smart Home. */
static const char *const START_TEXT[] = {
    /* TR: Settings > Screen and sound, the start screen's choices: the rail's places. At most 23
     * bytes */
    TR_NOOP_C("start screen", "Security"), TR_NOOP_C("start screen", "Smart Home")};
#define START_CHOICES (sizeof(START_TEXT) / sizeof(START_TEXT[0]))

static size_t start_index(void)
{
    return S.opt.start_view == UI_START_SMART_HOME ? 1u : 0u;
}

static void start_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < START_CHOICES) {
        S.opt.start_view = index == 1 ? UI_START_SMART_HOME : UI_START_SECURITY;
        s_opts_dirty = true;
        save_options_now();
    }
    ui_page_rebuild(); /* the row says the new choice */
}

static void pick_start(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[START_CHOICES];
    const ui_choice_t all[START_CHOICES] = {
        {UI_ICON_SHIELD, ui_tone_gray(), TR_C("start screen", START_TEXT[0]),
         /* TR: Settings > Screen and sound > Start screen, the line under "Security". At most 127
          * bytes */
         TR("The alarm state and arming")},
        {UI_ICON_HOUSE_WIFI, ui_tone_blue(), TR_C("start screen", START_TEXT[1]),
         /* TR: the line under "Smart Home". At most 127 bytes */
         TR("Your devices; Security while armed")},
    };
    memcpy(choices, all, sizeof(choices));
    /* TR: Settings > Screen and sound, the title of the start screen's choices, and the line under
     * it. At most 127 bytes each */
    ui_pick_sheet(TR("Start screen"), TR("An alarm or entry time always shows Security"), choices,
                  START_CHOICES, (int32_t)start_index(), start_chosen);
}
#endif

static void pick_appearance(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[APPEARANCE_CHOICES];
    static char automatic[192]; /* room for a long translation and the hours */
    if (S.opt.night_on) {
        char range[64];
        night_range_text(S.opt.night_start, S.opt.night_end, range, sizeof(range));
        /* TR: Settings > Screen and sound > Appearance, the line under "Automatic": the night hours
         * {0} ("10:00 PM to 6:00 AM"). At most 127 bytes */
        I18N_FORMAT(automatic, sizeof(automatic), TR("Dark during the night hours ({0}), light by day"), range);
    } else {
        /* TR: Settings > Screen and sound, the line under Automatic while the night hours are off.
         * At most 127 bytes */
        i18n_utf8_copy(automatic, sizeof(automatic), TR("Night hours are off: stays light"));
    }
    const ui_choice_t all[APPEARANCE_CHOICES] = {
        {UI_ICON_MOON, ui_tone_violet(), TR_C("appearance", APPEARANCE_TEXT[0]),
         /* TR: Settings > Screen and sound > Appearance, the line under "Dark". At most 127
          * bytes */
         TR("Light words on a dark screen: easy on the eyes at night")},
        {UI_ICON_SUN, ui_tone_amber(), TR_C("appearance", APPEARANCE_TEXT[1]),
         /* TR: Settings > Screen and sound > Appearance, the line under "Light". At most 127
          * bytes */
         TR("Dark words on a light screen, for a bright room")},
        {UI_ICON_SUN_MOON, ui_tone_blue(), TR_C("appearance", APPEARANCE_TEXT[2]), automatic},
    };
    memcpy(choices, all, sizeof(choices));
    /* TR: Settings > Screen and sound, the title of the list of appearances, and the line under it.
     * At most 127 bytes each */
    ui_pick_sheet(TR("Appearance"), TR("How the screen looks"), choices, APPEARANCE_CHOICES,
                  (int32_t)appearance_index(), appearance_chosen);
}

static void on_contrast_toggle(bool on, void *ctx)
{
    (void)ctx;
    S.opt.high_contrast = on;
    s_opts_dirty = true;
    save_options_now(); /* high contrast, at once */
}

static void open_saver_page(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_SAVER, 0);
}

static void open_night_page(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_NIGHT, 0);
}

/* ---- Screen and sound > Announcements (BL-202, docs/features/08 voice-first-build.md D06) ---- */

static bool s_voice_refused; /* the main switch was refused: the page says why, until it is left */
static ui_voice_info_t s_ann_shown; /* the voice as the page shows it (build_announcements) */
static uint32_t s_ann_polled;

/* The voice the page was built for: its readiness (what it shows), and its language, pack revision
 * and draft, whose change stops a Preview from the old pack. */
static bool announcements_same(const ui_voice_info_t *a, const ui_voice_info_t *b)
{
    return a->state == b->state && a->pack_revision == b->pack_revision && a->draft == b->draft &&
           strncmp(a->locale, b->locale, sizeof(a->locale)) == 0;
}

/* While Announcements is open, the voice is looked at once a second: a pack installed, removed or
 * failing while the page is open shows without leaving it. The page is built again only when what
 * it shows changed and has settled (not while the pack is being checked at start-up); the
 * Preview it played stops with a voice that changed. */
static void announcements_tick(void)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS || S.depth == 0u ||
        S.pages[S.depth - 1u].id != PAGE_ANNOUNCEMENTS || lv_tick_elaps(s_ann_polled) < 1000u) {
        return;
    }
    s_ann_polled = lv_tick_get();
    ui_voice_info_t info;
    ui_voice_info(&info);
    if (info.state == UI_VOICE_LOADING || announcements_same(&info, &s_ann_shown)) {
        return;
    }
    s_voice_refused = false; /* its reason was the voice before it changed */
    ui_voice_page_preview_stop();
    ui_page_rebuild();
}

static void open_announcements(void *ctx)
{
    (void)ctx;
    s_voice_refused = false;
    ui_page_open(PAGE_ANNOUNCEMENTS, 0);
}

/* The main switch: on as shipped (D06), on again only by a person and only while the voice is
 * ready (on a link keypad too: its own voice, LINK-028); a
 * refusal says why in a line (no toast). The page is built again: the three kinds below follow it. */
static void on_voice_toggle(bool on, void *ctx)
{
    (void)ctx;
    ui_voice_info_t info;
    ui_voice_info(&info);
    s_voice_refused = on && info.state != UI_VOICE_READY;
    if (!s_voice_refused) {
        S.opt.voice_enabled = on;
        s_opts_dirty = true;
        save_options_now();
    }
    ui_page_rebuild();
}

static void on_voice_kind(bool on, void *ctx)
{
    if ((uintptr_t)ctx == 0u) {
        S.opt.voice_status = on;
    } else {
        S.opt.voice_chime = on;
    }
    s_opts_dirty = true;
    save_options_now();
}

static void voice_preview_clicked(void *ctx)
{
    (void)ctx;
    ui_voice_preview_sample(); /* with Announcements off too; it changes nothing */
}

static void build_announcements(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Settings > Screen and sound > Announcements, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Announcements"));
    /* TR: the line under a page's title: the page it belongs to. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Screen and sound"));
    ui_voice_info_t info;
    ui_voice_info(&info);
    s_ann_shown = info; /* what announcements_tick compares with */
    s_ann_polled = lv_tick_get();
    if (!ui_voice_present()) {
        return;
    }
    const bool ready = info.state == UI_VOICE_READY;
    const char *detail;
    if (ready) {
        /* TR: Screen and sound > Announcements, the line under the main switch. At most 95 bytes */
        detail = TR("The panel says what is happening, in words");
    } else if (info.state == UI_VOICE_NONE) {
        /* TR: Screen and sound > Announcements, the line under the main switch without a voice
         * pack. At most 95 bytes */
        detail = TR("Needs a voice pack");
    } else {
        /* TR: Screen and sound > Announcements, the line under the main switch while the voice
         * can't be used. At most 95 bytes */
        detail = TR("Needs working voice");
    }
    ui_row_toggle_icon(body, UI_ICON_MEGAPHONE, ui_tone_blue(), TR("Announcements"), detail,
                       S.opt.voice_enabled, on_voice_toggle, NULL);
    if (s_voice_refused) {
        /* TR: Screen and sound > Announcements, in amber when turning them on was refused. At most
         * 127 bytes each */
        ui_paragraph(body,
                     info.state == UI_VOICE_NONE
                         ? TR("Announcements cannot be turned on because the voice pack is not installed.")
                         : TR("Announcements cannot be turned on because voice is unavailable."),
                     ui_tone_amber());
    }
    /* No Walk test switch: the walk test always says a zone's words (WALK-016). No voice language
     * or pack rows either: Time and language sets the language. */
    /* TR: Screen and sound > Announcements, the two kinds and their lines. At most 95 bytes each */
    lv_obj_t *kinds[2] = {
        ui_row_toggle(body, TR("Status messages"),
                      TR("Says arming, armed, disarmed, entry and not ready messages."), S.opt.voice_status,
                      on_voice_kind, (void *)(uintptr_t)0),
        ui_row_toggle(body, TR("Zone chime"), TR("Says the zone’s spoken name after the chime."),
                      S.opt.voice_chime, on_voice_kind, (void *)(uintptr_t)1),
    };
    for (size_t i = 0; i < 2u && !S.opt.voice_enabled; i++) {
        ui_row_disable(kinds[i]); /* they keep their choice while Announcements are off */
    }
    if (!ready && info.state != UI_VOICE_NONE) {
        ui_paragraph(body, TR("Voice is unavailable."), ui_tone_amber());
    }
    lv_obj_t *preview = ui_row_action(body, UI_ICON_VOLUME, TR_C("voice", "Preview"), ui_tone_blue(),
                                      voice_preview_clicked, NULL);
    if (!ready) {
        ui_row_disable(preview);
    }
    /* TR: Screen and sound > Announcements, the last line. At most 127 bytes */
    ui_paragraph(body, TR("Uses the Voice volume in Screen and sound. Alarms, fire and CO always sound at full volume."),
                 ui_tone_gray());
}

/* Settings > Screen and sound (BL-131 finding 4): the sound on top, since it is changed far more
 * often than the screen, then the screen. The chime is the status bar's own switch (one place),
 * the night hours and the screen saver have pages. */
static void build_display(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Settings, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Screen and sound"));
    (void)note; /* no line under the title (BL-131, rule 6): the page says it itself */
    (void)nlen;
    /* TR: Settings > Screen and sound, a section heading */
    ui_section(body, TR("Sound"));
    /* TR: Settings > Screen and sound, the slider for the door chime's loudness. At most 47 bytes */
    ui_row_level(body, UI_ICON_BELL, TR("Chime volume"), S.opt.chime_volume, 0, 100, on_chime_volume, NULL, true);
    if (ui_voice_present()) {
        /* TR: Settings > Screen and sound, the slider for the spoken announcements' loudness. At most
         * 47 bytes */
        ui_row_level(body, UI_ICON_MEGAPHONE, TR("Voice volume"), S.opt.voice_volume, 0, 100,
                     on_voice_volume, NULL, true);
    }
    /* TR: Settings > Screen and sound, the slider for the beeps' loudness (entry and exit, trouble,
     * key clicks). At most 47 bytes */
    ui_row_level(body, UI_ICON_VOLUME, TR("Beeps volume"), S.opt.volume, 0, 100, on_volume, NULL, true);
    ui_paragraph(body, TR("Alarms, fire and CO always sound at full volume."), ui_tone_gray());
    ui_row_toggle(body, TR("Key clicks"), TR("A short click for every touch"), S.opt.key_click,
                  on_click_toggle, NULL);
    if (ui_voice_present()) {
        /* TR: Settings > Screen and sound, the row that opens Announcements (the voice, BL-202) */
        ui_row_link(body, UI_ICON_MEGAPHONE, ui_tone_blue(), TR("Announcements"), NULL,
                    S.opt.voice_enabled ? TR_C("switch state", "On") : TR_C("switch state", "Off"),
                    UI_ICON_CHEVRON_RIGHT, open_announcements, NULL);
    }

    /* TR: Settings > Screen and sound, a section heading */
    ui_section(body, TR("Screen"));
#if ALARM_UI_DEVICES
    /* BL-220 R8: where this screen rests, while Smart Home is in the rail */
    if (lv_subject_get_int(&subject_devices_avail) == 1) {
        /* TR: Settings > Screen and sound: the row that picks where the screen rests, and the line
         * under it. At most 47 and 127 bytes */
        ui_row_link(body, UI_ICON_HOUSE_WIFI, ui_tone_blue(), TR("Start screen"),
                    TR("Where this screen rests; an alarm always shows Security"),
                    TR_C("start screen", START_TEXT[start_index()]), UI_ICON_CHEVRON_RIGHT,
                    pick_start, NULL);
    }
#endif
    /* The appearance first, where people look for how the screen looks (BL-132) */
    const char *look = NULL;
    if (appearance_index() == UI_APPEARANCE_AUTO) {
        /* TR: Settings > Screen and sound, the line under Appearance when it is Automatic. At most
         * 127 bytes */
        look = S.opt.night_on ? TR("Dark during the night hours, light by day")
                              : TR("Night hours are off: stays light");
    }
    /* TR: Settings > Screen and sound, the row that opens the list of appearances */
    ui_row_link(body, UI_ICON_SUN_MOON, ui_tone_blue(), TR("Appearance"), look,
                TR_C("appearance", APPEARANCE_TEXT[appearance_index()]), UI_ICON_CHEVRON_RIGHT,
                pick_appearance, NULL);
    /* TR: Settings > Screen and sound, a switch and the line under it */
    ui_row_toggle_icon(body, UI_ICON_CONTRAST, ui_tone_blue(), TR("High contrast"),
                       TR("Stronger colors, every button outlined"), S.opt.high_contrast,
                       on_contrast_toggle, NULL);
    ui_row_level(body, UI_ICON_SUN, TR("Brightness"), S.opt.brightness, 10, 100, on_brightness, NULL,
                 false);
    ui_row_link(body, UI_ICON_IMAGE, ui_tone_blue(), TR("Screen saver"),
                TR("A dimmed photo with the time and the alarm state"),
                TR_C("screen saver delay", SAVER_TEXT[delay_index(DELAY_SAVER)]), UI_ICON_CHEVRON_RIGHT,
                open_saver_page, NULL);
    /* TR: Settings > Screen and sound, the row's line under "Screen off" */
    delay_row(body, UI_ICON_MONITOR_OFF, DELAY_OFF, TR("Screen off"), TR("Dark until a touch wakes it"));
    /* Night hours (UI-043): for a keypad in a bedroom. */
    char range[64];
    char detail[127];
    night_range_text(S.opt.night_start, S.opt.night_end, range, sizeof(range));
    /* TR: Settings > Screen and sound, the line under "Night hours": the hours {0} ("10:00 PM to
     * 6:00 AM"). At most 120 bytes */
    I18N_FORMAT(detail, sizeof(detail), TR("{0} · dimmer, and dark sooner"), range);
    ui_row_link(body, UI_ICON_MOON, ui_tone_violet(), TR("Night hours"), detail,
                S.opt.night_on ? TR_C("switch state", "On") : TR_C("switch state", "Off"),
                UI_ICON_CHEVRON_RIGHT, open_night_page, NULL);
}

/* Settings > Screen and sound > Screen saver: when it starts and how bright it is (its rows
 * from the page they came from; the page has nothing else). */
static void build_saver(lv_obj_t *body, char *title, size_t tlen)
{
    /* TR: Settings > Screen and sound, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Screen saver"));
    delay_row(body, UI_ICON_IMAGE, DELAY_SAVER, TR("Screen saver"),
              TR("A dimmed photo with the time and the alarm state"));
    ui_row_level(body, UI_ICON_MOON, TR("Screen saver brightness"), S.opt.saver_brightness, 0, 50,
                 on_saver_brightness, NULL, false);
    if (S.opt.saver_brightness == 0u) {
        ui_paragraph(body, TR("At 0 the screen goes dark; a touch wakes it."), ui_tone_gray());
    }
}

/* ---- the night hours: the sheet with both ends -------------------------------------------- */

#define NIGHT_STEP 15 /* minutes: the rollers' steps (the Panel Manager's too) */

/* "8 hours" or "8 h 15 min": how long the night lasts, 24 hours when both ends are the same. */
static void night_lasts_text(int32_t start, int32_t end, char *buf, size_t len)
{
    int32_t span = ((end - start) % 1440 + 1440) % 1440;
    if (span == 0) {
        span = 1440;
    }
    if (span % 60 == 0) {
        /* TR: how long the night lasts, whole hours. At most 30 bytes */
        I18N_FORMAT(buf, len, TRN("{0} hour", "{0} hours", (uint32_t)(span / 60)), (int)(span / 60));
    } else {
        /* TR: how long the night lasts: hours {0} and minutes {1}. At most 30 bytes */
        I18N_FORMAT(buf, len, TRN("{0} h {1} min", "{0} h {1} min", (uint32_t)(span % 60)), (int)(span / 60),
                    (int)(span % 60));
    }
}

static void night_sheet_refresh(lv_obj_t *sheet)
{
    const int32_t start = ui_time_rollers_get(ui_find(sheet, "night_start"));
    const int32_t end = ui_time_rollers_get(ui_find(sheet, "night_end"));
    char lasts[48];
    char range[64];
    char text[127];
    night_lasts_text(start, end, lasts, sizeof(lasts));
    night_range_text(start, end, range, sizeof(range));
    /* TR: Night hours sheet, under the rollers: how long the night lasts {0} and its hours {1}
     * ("8 hours · 10:00 PM to 6:00 AM"). At most 100 bytes */
    I18N_FORMAT(text, sizeof(text), TR("{0} · {1}"), lasts, range);
    ui_set_label(sheet, "night_result", text);
}

static void on_night_rollers(lv_event_t *e)
{
    night_sheet_refresh((lv_obj_t *)lv_event_get_user_data(e));
}

static bool night_sheet_ok(void)
{
    lv_obj_t *sheet = ui_find(S.main, "night_sheet");
    if (sheet == NULL) {
        return true;
    }
    S.opt.night_start = (uint16_t)ui_time_rollers_get(ui_find(sheet, "night_start"));
    S.opt.night_end = (uint16_t)ui_time_rollers_get(ui_find(sheet, "night_end"));
    s_opts_dirty = true;
    save_options_now();
    ui_page_rebuild(); /* the Hours row says the new hours */
    return true;
}

static void open_night_hours(void *ctx)
{
    (void)ctx;
    lv_obj_t *sheet = ui_find(S.main, "night_sheet");
    if (sheet == NULL) {
        return;
    }
    ui_time_rollers_setup(ui_find(sheet, "night_start"), S.opt.night_start, NIGHT_STEP, on_night_rollers,
                          sheet);
    ui_time_rollers_setup(ui_find(sheet, "night_end"), S.opt.night_end, NIGHT_STEP, on_night_rollers, sheet);
    night_sheet_refresh(sheet);
    ui_sheet_open(SHEET_NIGHT, night_sheet_ok, NULL);
}

/* Settings > Screen and sound > Night hours: the switch, then the hours (one row that opens the
 * sheet), the brightness and the screen-off time while it is night. */
static void build_night(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Settings > Screen and sound, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Night hours"));
    /* TR: Settings > Night hours, the line under the title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("A dimmer screen at night that goes dark sooner"));
    /* TR: Settings > Night hours, a switch and the line under it */
    ui_row_toggle(body, TR("Night hours"), TR("Every night, on this keypad"), S.opt.night_on,
                  on_night_toggle, NULL);
    if (S.opt.night_on) {
        char range[64];
        night_range_text(S.opt.night_start, S.opt.night_end, range, sizeof(range));
        /* TR: Settings > Night hours, the row that opens the start and end of the night (its value
         * is the hours) */
        ui_row_link(body, UI_ICON_CLOCK, ui_tone_blue(), TR_C("night hours", "Hours"), NULL, range,
                    UI_ICON_CHEVRON_RIGHT, open_night_hours, NULL);
        ui_row_level(body, UI_ICON_MOON, TR("Brightness at night"), S.opt.night_brightness, 10, 100,
                     on_night_brightness, NULL, false);
        delay_row(body, UI_ICON_MONITOR_OFF, DELAY_NIGHT_OFF, TR("Screen off at night"), NULL);
        if (!ui_clock_known()) {
            /* TR: Settings > Night hours, under the rows while the panel doesn't know the time.
             * At most 127 bytes */
            ui_paragraph(body, TR("Night hours begin once the panel knows the time (Time and language)."),
                         ui_tone_amber());
        }
    }
    /* TR: Settings > Night hours, at the foot of the page. At most 127 bytes */
    ui_paragraph(body, TR("A touch, a change of state or an alarm wakes the screen. An alarm shows at "
                          "the normal brightness, at night too."),
                 ui_tone_gray());
}

/* ---- a change from elsewhere (docs/features/07 UI-046; docs/features/11 "Applying a change from
 * elsewhere") -------------------------------------------------------------------------------------
 * Home Assistant (through the master) or the master's console changes this panel's Screen and
 * sound settings, field by field. The patch comes through the backend's mailbox and goes through
 * the screen's own copy (S.opt) and its normal save (ui_app_apply_options), on this task: what the
 * screen shows and what is stored never part, and the voice follows as after a change made here.
 * It is refused whole while a person is changing these settings here (the setup, a page that shows
 * them, a change not saved yet: in use) and while an alarm, entry time or exit time is on the
 * screen (held). What the screen shows changes from elsewhere at most once per UI_PREFS_LOOK_MS -
 * the look, and the screen-off and saver delays and the clock's format, whose change shows at once
 * too (UI_PREFS_SHOWN: a new delay takes a screen nobody touches from the saver to dark and back):
 * a patch that would change one sooner waits, whole, and is judged again then, so no flood of
 * changes can make the screen or its backlight flicker (WCAG 2.3.1). Nothing on the screen says it
 * happened: no word, no toast (the owner's decision 10, BL-216). */
static struct {
    bool held;         /* a patch taken, waiting for the 2 s */
    uint32_t ticket;   /* its ticket, answered with prefs_remote_done */
    ui_prefs_patch_t patch;
    bool shown_recent; /* shown_at is less than UI_PREFS_LOOK_MS ago */
    uint32_t shown_at; /* when a change from elsewhere last changed what the screen shows */
} s_remote;

/* A page that shows these settings is open: Screen and sound and its pages, and Time and language,
 * whose Clock row is a settings field too. What it shows is never changed under a finger. */
static bool prefs_pages_open(void)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return false;
    }
    for (uint8_t i = 0; i < S.depth; i++) {
        switch (S.pages[i].id) {
        case PAGE_DISPLAY:
        case PAGE_SAVER:
        case PAGE_NIGHT:
        case PAGE_ANNOUNCEMENTS:
        case PAGE_TIME: return true;
        default: break;
        }
    }
    return false;
}

/* Judges a patch and applies what it may: true when it is answered (why filled in), false while it
 * waits for the 2 s (judged again then, from the start). */
static bool prefs_remote_apply(const ui_prefs_patch_t *p, uint8_t why[UI_PREFS_F_LAST])
{
    memset(why, 0, UI_PREFS_F_LAST);
    if (S.wizard_on || prefs_pages_open() || s_opts_dirty) {
        ui_prefs_refuse_all(p, (uint8_t)UI_PREFS_WHY_IN_USE, why);
        return true;
    }
    if (ui_app_held()) {
        ui_prefs_refuse_all(p, (uint8_t)UI_PREFS_WHY_HELD, why);
        return true;
    }
    ui_prefs_patch_t take = *p;
    /* Announcements go on only while the voice is ready, as from this screen (D06); off, or on
     * while already on, always. */
    const uint32_t voice = UI_PREFS_BIT(UI_PREFS_F_VOICE_ENABLED);
    if ((take.have & voice) != 0u && take.value[UI_PREFS_F_VOICE_ENABLED - 1u] == 1u &&
        !S.opt.voice_enabled) {
        ui_voice_info_t info;
        ui_voice_info(&info);
        if (info.state != UI_VOICE_READY) {
            why[UI_PREFS_F_VOICE_ENABLED - 1u] = (uint8_t)UI_PREFS_WHY_VOICE_NOT_READY;
            take.have &= ~voice;
        }
    }
    ui_options_t opt = S.opt; /* the partition, the language and the region stay as they are */
    const uint32_t changed = ui_prefs_merge(&opt, &take, why);
    if ((changed & UI_PREFS_SHOWN) != 0u && s_remote.shown_recent) {
        return false;
    }
    if (changed != 0u) {
        if ((changed & UI_PREFS_SHOWN) != 0u) {
            s_remote.shown_recent = true;
            s_remote.shown_at = lv_tick_get();
        }
        ui_app_apply_options(&opt); /* shown and saved as a change made here is */
    }
    return true;
}

static void prefs_remote_tick(void)
{
    if (S.be.prefs_remote_take == NULL || S.be.prefs_remote_done == NULL) {
        return;
    }
    if (s_remote.shown_recent && lv_tick_elaps(s_remote.shown_at) >= UI_PREFS_LOOK_MS) {
        s_remote.shown_recent = false;
    }
    if (!s_remote.held) {
        if (!S.be.prefs_remote_take(S.be.ctx, &s_remote.patch, &s_remote.ticket)) {
            return;
        }
    } else if (s_remote.shown_recent) {
        return; /* what the screen shows changed from elsewhere less than 2 s ago */
    }
    uint8_t why[UI_PREFS_F_LAST];
    s_remote.held = !prefs_remote_apply(&s_remote.patch, why);
    if (!s_remote.held) {
        S.be.prefs_remote_done(S.be.ctx, s_remote.ticket, why);
    }
}

/* ---- users --------------------------------------------------------------------------------- */

static uint8_t s_user; /* user being edited */

/* The installer and the system master see everyone; a partition master itself and the people
 * of its partitions (the Panel Manager lists the same). */
static bool actor_sees(uint8_t u)
{
    const alarm_user_t *actor = &S.cfg->users[S.auth_user - 1u];
    const alarm_user_t *usr = &S.cfg->users[u - 1u];
    if (actor->authority == ALARM_AUTH_MASTER || actor->authority == ALARM_AUTH_INSTALLER) {
        return true;
    }
    return u == S.auth_user || (usr->partitions & actor->partitions) != 0u;
}

/* The engine's own rule, the one the Panel Manager asks too (alarm_config_user_manages). */
static bool actor_may_edit(uint8_t u)
{
    return alarm_config_user_manages(S.cfg, S.auth_user, u);
}

/* Who can have a browser sign-in (components/services/panel_web/src/web_login.c): the installer,
 * system and partition masters, standard users - not arm-only, guest or duress codes. */
static bool login_possible(uint8_t u)
{
    const alarm_authority_t a = S.cfg->users[u - 1u].authority;
    return u == ALARM_USER_INSTALLER ? a == ALARM_AUTH_INSTALLER
                                     : (a == ALARM_AUTH_MASTER || a == ALARM_AUTH_PARTITION_MASTER ||
                                        a == ALARM_AUTH_STANDARD);
}

static void user_detail(uint8_t u, char *buf, size_t len)
{
    char num[8];
    snprintf(num, sizeof(num), "%02u", (unsigned)u); /* the user's number, two digits */
    /* TR: a user's number {0} and what their code can do {1}, the line under a user's name.
     * At most 25 bytes */
    I18N_FORMAT(buf, len, TR("User {0} · {1}"), num, ui_authority_name(S.cfg->users[u - 1u].authority));
}

static void open_user(void *ctx)
{
    ui_page_open(PAGE_USER, (int32_t)(uintptr_t)ctx);
}

static bool add_user_code(const char *code)
{
    uint8_t slot = 0;
    for (uint8_t u = ALARM_USER_MASTER + 1u; u <= ALARM_MAX_USERS && slot == 0u; u++) {
        if (S.cfg->users[u - 1u].authority == ALARM_AUTH_NONE) {
            slot = u;
        }
    }
    if (slot == 0u) {
        ui_pad_error(TR("Every user slot is taken"));
        return false;
    }
    char name[ALARM_NAME_LEN];
    char num[8];
    snprintf(num, sizeof(num), "%02u", (unsigned)slot);
    /* TR: the name of a user nobody has named; {0} is a two-digit number ("05") */
    I18N_FORMAT(name, sizeof(name), TR_C("default user name", "User {0}"), num);
    const uint8_t parts = (uint8_t)(1u << (S.opt.partition - 1u));
    const alarm_result_t r = S.be.user_set(S.be.ctx, S.auth_code, slot, code, ALARM_AUTH_STANDARD,
                                           parts, name);
    if (r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(ui_result_text(r));
        return false;
    }
    ui_pad_cancel();
    ui_refresh(true);
    ui_toast(TR("User added. Give them a name"));
    ui_page_open(PAGE_USER, slot);
    return true;
}

static void add_user(void *ctx)
{
    (void)ctx;
    ui_pad_ask_new(HOST_SETTINGS, UI_ICON_USER_PLUS, TR("Code for the new user"),
                    add_user_code);
}

static void build_users(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Settings, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Users"));
    if (!ui_settings_allowed(ACCESS_USERS)) {
        /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
        ui_settings_locked(body, ACCESS_USERS, TR("Users"),
                           TR("Enter a master or installer code to add, change or remove users"), note,
                           nlen);
        return;
    }
    char name[ALARM_NAME_LEN + 8];
    char who[ALARM_NAME_LEN + 40];
    /* TR: Settings > Users, the line under the title: who is signed in ({0} is a user's name).
     * At most 40 bytes */
    I18N_FORMAT(who, sizeof(who), TR("Signed in as {0}"),
                ui_user_name(S.cfg, S.auth_user, name, sizeof(name)));
    /* TR: Settings > Users, the line under the title, after who is signed in. At most 60 bytes */
    snprintf(note, nlen, "%s · %s", who, TR("Codes are never shown"));
    ui_row_action(body, UI_ICON_USER_PLUS, TR("Add a user"), ui_tone_blue(), add_user, NULL);
    for (uint8_t u = 1; u <= ALARM_MAX_USERS; u++) {
        const alarm_user_t *usr = &S.cfg->users[u - 1u];
        if (usr->authority == ALARM_AUTH_NONE || !actor_sees(u)) {
            continue;
        }
        char detail[48];
        user_detail(u, detail, sizeof(detail));
        const bool editable = actor_may_edit(u);
        ui_row_link(body, UI_ICON_USER, ui_authority_tone(usr->authority),
                    ui_user_name(S.cfg, u, name, sizeof(name)), detail,
                    u == S.auth_user ? TR_C("user list", "You") : "", editable ? UI_ICON_CHEVRON_RIGHT : NULL,
                    editable ? open_user : NULL, (void *)(uintptr_t)u);
    }
}

static void user_result(alarm_result_t r, const char *ok_text)
{
    if (r == ALARM_OK) {
        ui_refresh(true);
        if (ok_text != NULL && ok_text[0] != '\0') {
            ui_toast(ok_text); /* only for outcomes the rebuilt page doesn't show */
        }
    } else {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(ui_result_text(r));
    }
    build_page();
}

static void renamed(const char *text)
{
    const alarm_user_t *usr = &S.cfg->users[s_user - 1u];
    if (text[0] == '\0') {
        ui_toast(TR("A name can't be empty"));
        return;
    }
    user_result(S.be.user_update(S.be.ctx, S.auth_code, s_user, usr->authority, usr->partitions,
                                 text),
                NULL); /* the page shows the new name */
}

static void rename_user(void *ctx)
{
    (void)ctx;
    char name[ALARM_NAME_LEN + 8];
    ui_text_sheet(TR("Name"), TR("Shown in the event log"),
                  ui_user_name(S.cfg, s_user, name, sizeof(name)), false, ALARM_NAME_LEN - 1u,
                  renamed);
}

static const alarm_authority_t PICKABLE[] = {ALARM_AUTH_STANDARD, ALARM_AUTH_ARM_ONLY,
                                             ALARM_AUTH_GUEST, ALARM_AUTH_DURESS,
                                             ALARM_AUTH_PARTITION_MASTER};

static void authority_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= sizeof(PICKABLE) / sizeof(PICKABLE[0])) {
        return;
    }
    const alarm_user_t *usr = &S.cfg->users[s_user - 1u];
    user_result(S.be.user_update(S.be.ctx, S.auth_code, s_user, PICKABLE[index], usr->partitions,
                                 NULL),
                NULL); /* the page shows the new authority */
}

static void pick_authority(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[5];
    const alarm_authority_t a = S.cfg->users[S.auth_user - 1u].authority;
    /* only the system master and the installer make partition masters */
    const size_t n = (a == ALARM_AUTH_MASTER || a == ALARM_AUTH_INSTALLER) ? 5u : 4u;
    int32_t selected = -1;
    for (size_t i = 0; i < n; i++) {
        choices[i].glyph = UI_ICON_USER;
        choices[i].tone = ui_authority_tone(PICKABLE[i]);
        choices[i].title = ui_authority_name(PICKABLE[i]);
        choices[i].detail = ui_authority_detail(PICKABLE[i]);
        if (S.cfg->users[s_user - 1u].authority == PICKABLE[i]) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("What this code can do"), NULL, choices, n, selected, authority_chosen);
}

static bool new_code_done(const char *code)
{
    const alarm_user_t *usr = &S.cfg->users[s_user - 1u];
    const alarm_result_t r = S.be.user_set(S.be.ctx, S.auth_code, s_user, code, usr->authority,
                                           usr->partitions, NULL);
    if (r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(ui_result_text(r));
        return false;
    }
    if (s_user == S.auth_user) {
        snprintf(S.auth_code, sizeof(S.auth_code), "%s", code); /* keep working with the new one */
    }
    ui_pad_cancel();
    ui_refresh(true);
    ui_toast(TR("Code changed"));
    build_page();
    return true;
}

static void change_code(void *ctx)
{
    (void)ctx;
    char name[ALARM_NAME_LEN + 8];
    char title[64];
    /* TR: the code pad's title when changing a user's code; {0} is the user's name. At most 40 bytes */
    I18N_FORMAT(title, sizeof(title), TR("New code for {0}"),
                ui_user_name(S.cfg, s_user, name, sizeof(name)));
    ui_pad_ask_new(HOST_SETTINGS, UI_ICON_KEY_ROUND, title, new_code_done);
}

static void delete_confirmed(void)
{
    const alarm_result_t r = S.be.user_delete(S.be.ctx, S.auth_code, s_user);
    if (r == ALARM_OK) {
        ui_refresh(true);
        ui_toast(TR("User deleted"));
        ui_page_back();
    } else {
        user_result(r, "");
    }
}

static void delete_user(void *ctx)
{
    (void)ctx;
    char name[ALARM_NAME_LEN + 8];
    char title[80];
    /* TR: a confirmation's title; {0} is the user's name. At most 56 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Delete {0}?"),
                ui_user_name(S.cfg, s_user, name, sizeof(name)));
    ui_confirm(title, TR("Their code stops working right away. The event log keeps their name."),
               TR_C("confirm button", "Delete"), delete_confirmed);
}

/* ---- a user's partitions (docs/features/03: masters and the installer reach every partition) ---- */

static unsigned partitions_in_use(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        n += S.cfg->partitions[i].enabled;
    }
    return n;
}

static const char *partition_words(uint8_t mask, char *buf, size_t len)
{
    buf[0] = '\0';
    unsigned shown = 0, total = 0;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if (!S.cfg->partitions[i].enabled) {
            continue;
        }
        total++;
        if (!(mask & (1u << i))) {
            continue;
        }
        char item[ALARM_NAME_LEN + 16];
        if (S.cfg->partitions[i].name[0] != '\0') {
            i18n_utf8_copy(item, sizeof(item), S.cfg->partitions[i].name);
        } else {
            /* TR: the name of a partition nobody has named; {0} is its number */
            I18N_FORMAT(item, sizeof(item), TR_C("default partition name", "Partition {0}"),
                        (unsigned)(i + 1u));
        }
        if (shown) {
            char before[96];
            i18n_utf8_copy(before, sizeof(before), buf);
            /* TR: two partition names in a list ({0} the ones so far, {1} the next one). At most
             * 10 bytes */
            I18N_FORMAT(buf, len, TR_C("list of names", "{0}, {1}"), before, item);
        } else {
            i18n_utf8_copy(buf, len, item);
        }
        shown++;
    }
    if (shown == total && total > 1u) {
        i18n_utf8_copy(buf, len, TR("All partitions"));
    }
    return buf;
}

static void on_user_partition(bool on, void *ctx)
{
    const uint8_t bit = (uint8_t)(1u << (uintptr_t)ctx);
    const alarm_user_t *usr = &S.cfg->users[s_user - 1u];
    const uint8_t parts = on ? (uint8_t)(usr->partitions | bit) : (uint8_t)(usr->partitions & ~bit);
    if (parts == 0u) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("A user needs at least one partition"));
        build_page();
        return;
    }
    user_result(S.be.user_update(S.be.ctx, S.auth_code, s_user, usr->authority, parts, NULL),
                NULL); /* the switches show it */
}

static void open_user_partitions(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_USER_PARTITIONS, s_user);
}

static void build_user_partitions(lv_obj_t *body, int32_t arg, char *title, size_t tlen,
                                  char *note, size_t nlen)
{
    s_user = (uint8_t)arg;
    /* TR: a user's partitions page, the title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Partitions"));
    if (s_user <= ALARM_USER_MASTER || s_user > ALARM_MAX_USERS ||
        S.cfg->users[s_user - 1u].authority == ALARM_AUTH_NONE ||
        !ui_settings_allowed(ACCESS_USERS) || !actor_may_edit(s_user)) {
        return;
    }
    char name[ALARM_NAME_LEN + 8];
    /* TR: a user's partitions page, the line under the title; {0} is the user's name. At most 104 bytes */
    I18N_FORMAT(note, nlen, TR("Where {0}'s code works"),
                ui_user_name(S.cfg, s_user, name, sizeof(name)));
    for (uintptr_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if (!S.cfg->partitions[i].enabled) {
            continue;
        }
        char label[ALARM_NAME_LEN + 16];
        const char *pname = S.cfg->partitions[i].name;
        if (pname[0] != '\0') {
            i18n_utf8_copy(label, sizeof(label), pname);
        } else {
            /* TR: the name of a partition nobody has named; {0} is its number */
            I18N_FORMAT(label, sizeof(label), TR_C("default partition name", "Partition {0}"),
                        (unsigned)(i + 1u));
        }
        ui_row_toggle(body, label, NULL, (S.cfg->users[s_user - 1u].partitions >> i) & 1u,
                      on_user_partition, (void *)i);
    }
}

static void build_user(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                       size_t nlen)
{
    s_user = (uint8_t)arg;
    if (s_user < 1u || s_user > ALARM_MAX_USERS ||
        S.cfg->users[s_user - 1u].authority == ALARM_AUTH_NONE || !ui_settings_allowed(ACCESS_USERS) ||
        !actor_may_edit(s_user)) {
        /* TR: a user's page, the title when the user is gone. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("User"));
        return;
    }
    const alarm_user_t *usr = &S.cfg->users[s_user - 1u];
    char name[ALARM_NAME_LEN + 8];
    i18n_utf8_copy(title, tlen, ui_user_name(S.cfg, s_user, name, sizeof(name)));
    user_detail(s_user, note, nlen);
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL, name, UI_ICON_CHEVRON_RIGHT,
                rename_user, NULL);
    const bool fixed = s_user <= ALARM_USER_MASTER || s_user == S.auth_user;
    ui_row_link(body, UI_ICON_SHIELD, ui_authority_tone(usr->authority), TR("What this code can do"),
                ui_authority_detail(usr->authority), ui_authority_name(usr->authority),
                fixed ? NULL : UI_ICON_CHEVRON_RIGHT, fixed ? NULL : pick_authority, NULL);
    ui_row_link(body, UI_ICON_KEY, ui_tone_amber(), TR("Change the code"), ui_code_digits(), NULL,
                UI_ICON_CHEVRON_RIGHT, change_code, NULL);
    if (!fixed && partitions_in_use() > 1u) {
        char parts[96];
        ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR("Partitions"), NULL,
                    partition_words(usr->partitions, parts, sizeof(parts)), UI_ICON_CHEVRON_RIGHT,
                    open_user_partitions, NULL);
    }
    if (usr->authority == ALARM_AUTH_DURESS) {
        ui_paragraph(body, TR("A duress code disarms like any other code, and silently reports that "
                              "someone is being forced to disarm."),
                     ui_tone_gray());
    }
    if (login_possible(s_user)) {
        /* Everyone signs in with their name here, the installer too */
        ui_login_rows(body, s_user,
                      s_user == ALARM_USER_INSTALLER ? ui_login_name(S.cfg, s_user) : name, true);
    }
    if (!fixed) {
        ui_section(body, ""); /* a gap: Delete is last and apart */
        ui_row_action(body, UI_ICON_TRASH, TR("Delete this user"), ui_tone_red(), delete_user, NULL);
    }
}

/* ---- Panel Manager sign-in (docs/features/31) -------------------------------------------------- */

/* The same rule as the panel (components/services/panel_web/web_login.h): length is what matters. */
#define LOGIN_PASSWORD_MIN 12u
#define LOGIN_PASSWORD_MAX 64u

static uint8_t s_login_user;
static char s_login_first[LOGIN_PASSWORD_MAX + 1u];

static void login_forget(void)
{
    memset(s_login_first, 0, sizeof(s_login_first));
}

static void login_again(const char *text)
{
    char why[96] = "";
    const bool same = strcmp(text, s_login_first) == 0;
    /* Set for somebody else, it is theirs to replace at their first sign-in. */
    const bool temporary = S.auth_user != 0u && S.auth_user != s_login_user;
    const bool ok = same && S.be.web_login_set(S.be.ctx, s_login_user, text, temporary, why,
                                               sizeof(why));
    login_forget();
    if (!ok) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(same ? TR_C("why", why) : TR("The two passwords were different. Nothing changed"));
    } else if (temporary) {
        ui_toast(TR("They choose their own password when they first sign in"));
    }
    ui_page_rebuild(); /* the row shows the sign-in is on */
}

static void login_first(const char *text)
{
    if (strlen(text) < LOGIN_PASSWORD_MIN) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Use at least 12 characters. A few words are easy to remember"));
        return;
    }
    snprintf(s_login_first, sizeof(s_login_first), "%s", text);
    ui_text_sheet(TR("Type the password again"), TR("The same password, to be sure"), "", true,
                  LOGIN_PASSWORD_MAX, login_again);
}

static void login_password(void *ctx)
{
    s_login_user = (uint8_t)(uintptr_t)ctx;
    login_forget();
    ui_text_sheet(TR("Panel Manager password"),
                  TR("At least 12 characters. It is not the keypad code, and it is never shown"),
                  "", true, LOGIN_PASSWORD_MAX, login_first);
}

/* The browser sign-in switch of the page on screen: it shows what is set, so a password sheet or a
 * question that is cancelled leaves it where it was. */
static lv_obj_t *s_login_switch;

static void login_switch_show(bool on)
{
    if (s_login_switch != NULL) {
        lv_obj_set_state(s_login_switch, LV_STATE_CHECKED, on);
    }
}

static void login_off_confirmed(void)
{
    S.be.web_login_clear(S.be.ctx, s_login_user);
    ui_page_rebuild();
}

static void login_off(void *ctx)
{
    s_login_user = (uint8_t)(uintptr_t)ctx;
    login_switch_show(true); /* until the question is answered */
    ui_confirm(TR("Turn off browser sign-in?"),
               TR("Signs out of the Panel Manager at once. A new password turns it on again."),
               TR_C("confirm button", "Turn off"), login_off_confirmed);
}

static void login_toggled(bool on, void *ctx)
{
    if (on) {
        login_switch_show(false); /* on once a password is set; the page is rebuilt then */
        login_password(ctx);
    } else {
        login_off(ctx);
    }
}

/* The name a person signs in to the Panel Manager with: their name on the Users screen, the
 * installer's too ("Installer" from the factory; a blank installer's name still answers to it).
 * The same rule as the panel's own check (web_login.c login_name()); never translated, since it
 * is typed as a user name. */
const char *ui_login_name(const alarm_config_t *cfg, uint8_t user)
{
    const char *name = cfg->users[user - 1u].name;
    const char *p = name;
    while (*p == ' ') {
        p++;
    }
    return user == ALARM_USER_INSTALLER && *p == '\0' ? NOTR("Installer") : name;
}

void ui_login_rows(lv_obj_t *body, uint8_t user, const char *username, bool heading)
{
    if (S.be.web_login_get == NULL || S.be.web_login_set == NULL ||
        S.be.web_login_clear == NULL) {
        return;
    }
    const bool on = S.be.web_login_get(S.be.ctx, user);
    char detail[ALARM_NAME_LEN + 80];
    /* TR: the row under Panel Manager, under the switch: the sign-in name {0} and that the
     * password is not the keypad code. At most 100 bytes */
    I18N_FORMAT(detail, sizeof(detail), TR("User name: {0} · a password, not the keypad code"),
                username);
    if (heading) {
        ui_section(body, TR("Panel Manager"));
    }
    s_login_switch = ui_find(ui_row_toggle_icon(body, UI_ICON_GLOBE, ui_tone_blue(),
                                                TR("Browser sign-in"), detail, on, login_toggled,
                                                (void *)(uintptr_t)user),
                             "toggle");
    if (on) {
        ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_blue(), TR("Change the browser password"),
                    NULL, NULL, UI_ICON_CHEVRON_RIGHT, login_password, (void *)(uintptr_t)user);
    }
}

/* ---- about --------------------------------------------------------------------------------- */

/* "3 days 4 hours", "5 hours 12 minutes", "9 minutes": in words, two units at most. */
static void running_for(char *out, size_t len, uint32_t up)
{
    const unsigned d = up / 86400u;
    const unsigned h = up / 3600u % 24u;
    const unsigned m = up / 60u % 60u;
    char big[32];
    char small[32];
    if (d > 0u) {
        /* TR: About, Running for: days {0}. At most 31 bytes */
        I18N_FORMAT(big, sizeof(big), TRN("{0} day", "{0} days", d), d);
        if (h == 0u) {
            snprintf(out, len, "%s", big);
            return;
        }
        /* TR: About, Running for: hours {0}. At most 31 bytes */
        I18N_FORMAT(small, sizeof(small), TRN("{0} hour", "{0} hours", h), h);
    } else if (h > 0u) {
        I18N_FORMAT(big, sizeof(big), TRN("{0} hour", "{0} hours", h), h);
        if (m == 0u) {
            snprintf(out, len, "%s", big);
            return;
        }
        /* TR: About, Running for: minutes {0}. At most 31 bytes */
        I18N_FORMAT(small, sizeof(small), TRN("{0} minute", "{0} minutes", m), m);
    } else {
        I18N_FORMAT(out, len, TRN("{0} minute", "{0} minutes", m), m);
        return;
    }
    /* TR: About, Running for: a larger unit {0} then a smaller {1}, for example
     * "3 days 4 hours". At most 63 bytes */
    I18N_FORMAT(out, len, TR_C("running for", "{0} {1}"), big, small);
}

/* About's Set up section: zones, users and events in use. */
static void about_setup(lv_obj_t *body, char *buf, size_t blen, uint32_t log_count)
{
    unsigned zones = 0;
    unsigned users = 0;
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        zones += S.cfg->zones[z].type != ALARM_ZT_NOT_USED;
    }
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        users += S.cfg->users[u].authority != ALARM_AUTH_NONE;
    }
    /* TR: About, a heading: what is set up (zones, users, events). At most 47 bytes */
    ui_section(body, TR("Set up"));
    /* TR: About, zones in use {0} of the most the panel has {1}. At most 31 bytes */
    I18N_FORMAT(buf, blen, TR("{0} of {1}"), zones, (unsigned)ALARM_MAX_ZONES);
    ui_row_value(body, TR("Zones"), buf);
    I18N_FORMAT(buf, blen, TR("{0} of {1}"), users, (unsigned)ALARM_MAX_USERS);
    ui_row_value(body, TR("Users"), buf);
    /* TR: About, how many events the log holds. At most 31 bytes */
    I18N_FORMAT(buf, blen, TRN("{0} event", "{0} events", log_count), (unsigned)log_count);
    ui_row_value(body, TR("Event log"), buf);
}

/* What System details showed, now on About itself after SD card (BL-189): This panel, Network,
 * Set up (a keypad: its alarm panel, BL-048), Hardware. The panel's ID and version are in About's
 * top row. */
static void about_details(lv_obj_t *body)
{
    ui_about_t a;
    memset(&a, 0, sizeof(a));
    if (S.be.about != NULL) {
        S.be.about(S.be.ctx, &a);
    }
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    char buf[96];

    /* TR: About, a heading: when this panel was built and how long it has run. At most 47 bytes */
    ui_section(body, TR("This panel"));
    char day[32];
    /* "Sep 19 2026 22:41:00" (the compiler's __DATE__ and __TIME__) reads "Sep 19, 2026" */
    {
        static const char MONTHS[] = NOTR("JanFebMarAprMayJunJulAugSepOctNovDec"); /* __DATE__ */
        char mon[4] = "";
        unsigned d = 0;
        unsigned y = 0;
        i18n_civil_t c;
        memset(&c, 0, sizeof(c));
        if (sscanf(a.built, "%3s %u %u", mon, &d, &y) == 3 && strlen(mon) == 3u && d >= 1u &&
            d <= 31u && y >= 2000u && y < 3000u && strstr(MONTHS, mon) != NULL &&
            (size_t)(strstr(MONTHS, mon) - MONTHS) % 3u == 0u) {
            c.year = (int16_t)y;
            c.month = (uint8_t)((size_t)(strstr(MONTHS, mon) - MONTHS) / 3u + 1u);
            c.day = (uint8_t)d;
            /* TR: a date with its year ("Sep 19, 2026"), in CLDR pattern letters (MMM month name
             * short, d day, y year); About, the day the firmware was built. The
             * order may change. At most 31 bytes of result */
            i18n_date_in(day, sizeof(day), S.catalog, ui_region(),
                         TR_C("date pattern", "MMM d, y"), &c);
        } else {
            snprintf(day, sizeof(day), "%.11s", a.built);
        }
    }
    /* TR: About, the day this software was built, next to a date. At most 31 bytes */
    ui_row_value(body, TR("Built"), a.built[0] != '\0' ? day : "-");
    running_for(buf, sizeof(buf), a.uptime_s);
    ui_row_value(body, TR("Running for"), buf);

    ui_section(body, TR("Network"));
    ui_row_value(body, TR("Wi-Fi"),
                 w.state != UI_NET_CONNECTED ? TR("Not connected") : (w.ssid[0] ? w.ssid : "-"));
    ui_row_value(body, TR("IP address"), w.state == UI_NET_CONNECTED && w.ip[0] ? w.ip : "-");
    ui_row_value(body, TR("MAC address"), w.mac[0] ? w.mac : "-");

    if (ui_link_keypad()) {
        /* a keypad's zones, users and event log are the alarm panel's (BL-048) */
        ui_link_about_details(body);
    } else {
        about_setup(body, buf, sizeof(buf), (uint32_t)a.log_count);
    }

    ui_section(body, TR("Hardware"));
    ui_row_value(body, TR("Processor"), a.chip[0] ? a.chip : "-");
    ui_row_value(body, TR("Wi-Fi module"), w.radio_fw[0] ? w.radio_fw : "-");
    ui_row_value(body, NOTR("ESP-IDF"), a.idf[0] ? a.idf : "-"); /* a product name */
    char internal[28];
    char psram[28];
    char num[16];
    i18n_number(num, sizeof(num), ui_region(), (int64_t)a.heap_internal_kb, 0);
    /* TR: About, Free memory, the fast memory; {0} is a number (up to 11
     * bytes). At most 27 bytes */
    I18N_FORMAT(internal, sizeof(internal), TR("{0} KB internal"), num);
    i18n_number(num, sizeof(num), ui_region(), (int64_t)((uint64_t)a.heap_psram_kb * 10u / 1024u), 1);
    /* TR: About, Free memory, the large memory; {0} is a number of megabytes
     * with a decimal (up to 11 bytes). At most 27 bytes */
    I18N_FORMAT(psram, sizeof(psram), TR("{0} MB PSRAM"), num);
    snprintf(buf, sizeof(buf), "%s · %s", internal, psram);
    ui_row_value(body, TR("Free memory"), buf);
    storage_flash_t fl;
    memset(&fl, 0, sizeof(fl));
    if (S.be.storage_flash != NULL) {
        S.be.storage_flash(S.be.ctx, &fl);
    }
    if (fl.image_valid && fl.slot_bytes != 0u) {
        char used[32];
        char slot[32];
        ui_size_text(used, sizeof(used), fl.image_bytes);
        ui_size_text(slot, sizeof(slot), fl.slot_bytes);
        /* TR: About, Firmware space: the room the software takes {0} of the room
         * it has {1}, for example "2.3 MB of 4.5 MB used". At most 63 bytes */
        I18N_FORMAT(buf, sizeof(buf), TR("{0} of {1} used"), used, slot);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    ui_row_value(body, TR("Firmware space"), buf);
}

static void about_open(void *ctx) { ui_page_open((page_id_t)(intptr_t)ctx, 0); }
static void build_about(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Settings > About, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("About this panel"));
    note[0] = '\0';
    (void)nlen;
    ui_about_t a = {0};
    if (S.be.about) S.be.about(S.be.ctx, &a);
    /* the panel, its version and ID: not a button, it is what support asks for first */
    char detail[96];
    if (a.panel_id[0] != '\0') {
        /* TR: Settings > About, the panel's line: its version {0} and ID {1}. At most 63 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("Version {0} · Panel ID {1}"), a.firmware, a.panel_id);
    } else {
        /* TR: Settings > About, the panel's line: its version {0}. At most 63 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("Version {0}"), a.firmware[0] ? a.firmware : "-");
    }
    ui_row_link(body, UI_ICON_SHIELD_CHECK, ui_tone_green(), TR("RoboAlarms Panel"), detail, NULL, NULL,
                NULL, NULL);
    ui_link_about_row(body); /* a keypad: what it is a keypad of (BL-048) */
    ui_update_about_row(body, about_open, (void *)(intptr_t)PAGE_SOFTWARE);
    ui_storage_about_row(body, about_open, (void *)(intptr_t)PAGE_SD_CARD);
    about_details(body);
    if (ui_link_keypad()) {
        /* Installer is the alarm panel's; a keypad has the way back to a panel of its own */
        ui_link_about_end(body);
    } else {
        ui_section(body, ""); /* a gap before the Installer row */
        /* TR: Settings > About, the Installer row's line. At most 111 bytes */
        ui_row_link(body, UI_ICON_WRENCH, ui_tone_blue(), TR("Installer"),
                    TR("Changes how the alarm works · needs the installer code"), NULL,
                    UI_ICON_CHEVRON_RIGHT, about_open, (void *)(intptr_t)PAGE_INSTALLER);
    }
    ui_paragraph(body, TR("Made with LVGL (MIT), the Inter typeface (SIL Open Font License 1.1), "
                          "Lucide icons (ISC) and parts of Z-Wave JS (MIT)."),
                 ui_tone_gray());
}

/* ---- building a page ----------------------------------------------------------------------- */

static void build_page(void)
{
    ui_rows_reset();
    ui_set_int(&subject_busy, 0);
    lv_obj_t *body = S.page_body;
    const ui_page_t *pg = &S.pages[S.depth - 1u];
    if (pg->id != PAGE_NTFY) {
        ui_notify_page_left(); /* showing it is a per-visit decision (NTFY-011) */
    }
    if (pg->id != PAGE_ANNOUNCEMENTS) {
        ui_voice_page_preview_stop(); /* Announcements' Preview ends with the page */
    }
    char title[48] = "";
    char note[160] = "";
    switch (pg->id) {
    case PAGE_DISPLAY: build_display(body, title, sizeof(title), note, sizeof(note)); break;
    case PAGE_SAVER: build_saver(body, title, sizeof(title)); break;
    case PAGE_NIGHT: build_night(body, title, sizeof(title), note, sizeof(note)); break;
    case PAGE_ANNOUNCEMENTS: build_announcements(body, title, sizeof(title), note, sizeof(note)); break;
    case PAGE_USERS: build_users(body, title, sizeof(title), note, sizeof(note)); break;
    case PAGE_USER: build_user(body, pg->arg, title, sizeof(title), note, sizeof(note)); break;
    case PAGE_USER_PARTITIONS:
        build_user_partitions(body, pg->arg, title, sizeof(title), note, sizeof(note));
        break;
    case PAGE_ABOUT: build_about(body, title, sizeof(title), note, sizeof(note)); break;
    case PAGE_SOFTWARE:
        /* TR: About > Software update, the title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("Software update"));
        /* a keypad in LINK-025's maintenance, or one that needs this update to read its alarm
         * panel's settings (it can't check a code there), opens it without a code */
        if (!ui_settings_allowed(ACCESS_SYSTEM) && !ui_link_update_open()) {
            /* TR: a locked page: the line under the title. At most 127 bytes */
            ui_settings_locked(body, ACCESS_SYSTEM, TR("Software update"),
                               TR("The system master or installer code updates the software"),
                               note, sizeof(note));
            ui_update_stop();
        } else {
            ui_update_build(body);
        }
        break;
    case PAGE_SD_CARD:
        /* TR: About > SD card, the title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("SD card"));
        /* TR: About > SD card, the line under the title. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note), TR("Optional: nothing is kept on it yet"));
        ui_storage_build(body);
        break;
    case PAGE_WIFI:
        /* TR: Settings, the page title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("Wi-Fi and network"));
        /* a keypad without the alarm panel for 10 minutes opens it without a code (LINK-025) */
        if (!ui_settings_allowed(ACCESS_SHARED) && !ui_link_maintenance()) {
            /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
            ui_settings_locked(body, ACCESS_SHARED, TR("Wi-Fi and network"),
                               TR("A master or installer code changes the network"), note,
                               sizeof(note));
        } else {
            /* TR: Settings > Wi-Fi, the line under the title. At most 127 bytes */
            i18n_utf8_copy(note, sizeof(note), TR("The alarm keeps working without the network"));
            ui_net_build(body, false);
        }
        break;
    case PAGE_NETWORK:
        /* TR: Settings > Wi-Fi > Network, the page title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("Network"));
        if (!ui_settings_allowed(ACCESS_SHARED) && !ui_link_maintenance()) {
            /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
            ui_settings_locked(body, ACCESS_SHARED, TR("Network"),
                               TR("A master or installer code changes the network"), note,
                               sizeof(note));
        } else {
            ui_net_advanced_build(body, title, sizeof(title), note, sizeof(note));
        }
        break;
    case PAGE_TIME:
        /* What the clock says and the 12/24-hour clock (this keypad's) are anyone's; the time zone,
         * the language, the region and the time server need a master or installer code
         * (docs/features/14). */
        /* TR: Settings, the page title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("Time and language"));
        ui_time_build(body, ui_settings_allowed(ACCESS_SHARED), note, sizeof(note));
        break;
    case PAGE_SET_CLOCK:
        /* TR: Settings > Time and language > set the clock, the page title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("Set the date and time"));
        if (!ui_settings_allowed(ACCESS_SHARED)) {
            /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
            ui_settings_locked(body, ACCESS_SHARED, TR("Time and language"),
                               TR("A master or installer code sets the time"), note, sizeof(note));
        } else {
            ui_clock_build(body, note, sizeof(note));
        }
        break;
    case PAGE_WALK:
        /* TR: Settings, the page title. At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR("Walk test"));
        /* WALK-001: the installer, the system master or a partition master, as Wi-Fi asks; a test
         * on this keypad's partition, or its summary, shows without one (WALK-009, WALK-010). */
        if (!ui_settings_allowed(ACCESS_SHARED) && ui_walk_viewable()) {
            s_auto_ask = false; /* Extend and Start ask when they're pressed */
            ui_walk_build(body, note, sizeof(note));
        } else if (!ui_settings_allowed(ACCESS_SHARED)) {
            /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
            ui_settings_locked(body, ACCESS_SHARED, TR("Walk test"),
                               TR("A master or installer code starts a walk test"), note,
                               sizeof(note));
        } else {
            ui_walk_build(body, note, sizeof(note));
        }
        break;
    case PAGE_INTEGRATIONS:
    case PAGE_HOME_ASSISTANT:
    case PAGE_MQTT:
        ui_integrations_build(pg->id, body, title, sizeof(title), note, sizeof(note));
        break;
    case PAGE_NTFY:
    case PAGE_PUSHOVER:
        ui_notify_build(pg->id, body, title, sizeof(title), note, sizeof(note));
        break;
#if ALARM_UI_ZWAVE
    case PAGE_ZWAVE:
    case PAGE_ZWAVE_ADD:
    case PAGE_ZWAVE_REMOVE:
    case PAGE_ZWAVE_DEVICE:
    case PAGE_ZWAVE_REGION:
    case PAGE_ZWAVE_RADIO:
    case PAGE_ZWAVE_SMART:
    case PAGE_ZWAVE_SMART_ENTRY:
        ui_zwave_build(pg->id, pg->arg, body, title, sizeof(title), note, sizeof(note));
        break;
#endif
    case PAGE_LINK_STANDALONE: /* a keypad's (BL-048): no installer mode on a keypad */
        ui_link_build(pg->id, pg->arg, body, title, sizeof(title), note, sizeof(note));
        break;
#if ALARM_UI_AUTOMATIONS
    case PAGE_AUTOMATIONS:
    case PAGE_AUTOMATION:
        ui_automations_build(pg->id, pg->arg, body, title, sizeof(title), note, sizeof(note));
        break;
#endif
#if ALARM_UI_DEVICES
    case PAGE_SMART_HOME:
        /* TR: Settings, the page title (BL-220). At most 47 bytes */
        i18n_utf8_copy(title, sizeof(title), TR_C("settings", "Smart Home"));
        /* D6: a master's code (system or partition) or the installer's, as Wi-Fi asks */
        if (!ui_settings_allowed(ACCESS_SHARED)) {
            /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
            ui_settings_locked(body, ACCESS_SHARED, TR_C("settings", "Smart Home"),
                               TR("A master or installer code changes what asks for one"), note,
                               sizeof(note));
        } else {
            ui_devices_settings_build(body, note, sizeof(note));
        }
        break;
#endif
    case PAGE_SIRENS:
    case PAGE_OUT_WIRED:
    case PAGE_OUT_RF:
    case PAGE_OUT_RF_PAIR:
    case PAGE_RF:
        ui_sirens_build(pg->id, pg->arg, body, title, sizeof(title), note, sizeof(note));
        break;
#if ALARM_UI_ZIGBEE
    case PAGE_ZIGBEE:
    case PAGE_ZIGBEE_ADD:
    case PAGE_ZIGBEE_ADDED:
    case PAGE_ZIGBEE_DEVICE:
    case PAGE_ZIGBEE_CODE:
    case PAGE_ZIGBEE_CODES:
    case PAGE_ZIGBEE_REMOVE:
    case PAGE_ZIGBEE_RADIO:
    case PAGE_ZIGBEE_CHANNEL:
        ui_zigbee_build(pg->id, pg->arg, body, title, sizeof(title), note, sizeof(note));
        break;
#endif
    default:
        ui_installer_build(pg->id, pg->arg, body, title, sizeof(title), note, sizeof(note));
        break;
    }
    if (remote_page() && ui_link_remote_down()) {
        /* its alarm panel can't be reached: the page stays, dimmed, nothing sent (LINK-063) */
        /* TR: a keypad's page of its alarm panel's while the link is down, the line under the
         * title; {0} is the alarm panel's name (up to 47 bytes). At most 127 bytes */
        I18N_FORMAT(note, sizeof(note), TR("{0} can't be reached right now"), ui_link_master_name());
        const uint32_t n = lv_obj_get_child_count(body);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *row = lv_obj_get_child(body, (int32_t)i);
            if (lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE)) {
                ui_row_disable(row);
            }
        }
    }
    ui_set_str(&subject_page_title, title);
    ui_page_note(note);
    ui_set_int(&subject_page_back, S.depth > 1u ? 1 : 0);
    lv_obj_update_layout(body);
    lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    if (s_keep_scroll && s_kept_y > 0) {
        const int32_t room = lv_obj_get_scroll_bottom(body); /* how far it can go now */
        lv_obj_scroll_to_y(body, s_kept_y < room ? s_kept_y : room, LV_ANIM_OFF);
    }
    s_page_refreshed = lv_tick_get();
}
