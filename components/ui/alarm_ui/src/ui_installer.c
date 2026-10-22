/*
 * ui_installer.c - installer mode (programming mode) on the touchscreen: unlock
 * with the installer code or the power-up recovery, then system options, zones, the system
 * name, the installer and master codes, the setup wizard and the factory reset. Wi-Fi and the
 * date and time are shared with the master in their own sections (docs/features/14).
 *
 * Pages edit a working copy (S.edit). Leaving a page or installer mode applies it: the options,
 * partitions and zones of the copy on top of the current configuration (so user code changes
 * made meanwhile are kept), validated first, then alarm_engine_set_config through the backend.
 */
#include <stdio.h>
#include <string.h>

#include "notify/notify_words.h"
#include "ui_internal.h"

static uint16_t s_zone;           /* zone being edited */
/* The zone whose spoken name the editor's Done asked to approve once it is saved (D03, D07), 0 =
 * none, and the identity that Done saw: the page's save and the approval are one transaction
 * (ui_backend_t.voice_apply_approve), never part of the configuration. */
static uint16_t s_voice_zone;
static ui_voice_seen_t s_voice_seen;
static bool s_reset_log;          /* factory reset: event log too */
static alarm_config_t *s_scratch; /* what gets applied */

static uint8_t part_index(void)
{
    return (uint8_t)(S.opt.partition - 1u);
}

/* ---- entering and leaving ------------------------------------------------------------------ */

static void enter_program(uint8_t user, bool recovery)
{
    (void)user;
    S.program = true;
    S.keepalive_at = lv_tick_get();
    if (S.edit == NULL) {
        S.edit = lv_malloc(sizeof(alarm_config_t));
        LV_ASSERT_MALLOC(S.edit);
    }
    ui_refresh(true);
    memcpy(S.edit, S.cfg, sizeof(alarm_config_t));
    S.edit_dirty = false;
    if (recovery) {
        ui_toast(TR("Installer mode without a code: set a new installer code"));
        ui_page_open(PAGE_CODES, 0);
    } else {
        ui_page_rebuild();
    }
}

static bool installer_code(const char *code);

/* The installer code's answer (on a keypad, from its alarm panel: LINK-054, LINK-060). */
static void installer_checked(const ui_call_t *c)
{
    if (!ui_pad_asks(installer_code)) {
        return; /* the pad closed while the code was checked */
    }
    const alarm_result_t r = c->r;
    const uint8_t user = c->user;
    if (r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        /* Only a real BAD_STATE means the alarm is busy; everything else keeps its own words.
         * "Disarm first" used to be shown for any failure that was not one of the first two,
         * so an unavailable hash told the owner to disarm an already disarmed panel (B001). */
        ui_pad_error(r == ALARM_ERR_INVALID_CODE     ? TR("Wrong code, try again")
                     : r == ALARM_ERR_NOT_AUTHORIZED ? TR("That isn't the installer code")
                     : r == ALARM_ERR_BAD_STATE      ? TR("Disarm first, and silence any alarm")
                                                     : ui_result_text(r));
        return;
    }
    ui_pad_cancel();
    ui_settings_remember(user, c->code); /* opens Wi-Fi, time, integrations and the log too */
    enter_program(user, false);
}

/* The pad stays open; the answer opens installer mode (installer_checked). */
static bool installer_code(const char *code)
{
    if (!ui_call_program_enter(code, installer_checked, 0u)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

static void ask_code(void *ctx)
{
    (void)ctx;
    ui_pad_ask(HOST_SETTINGS, UI_ICON_WRENCH, TR("Installer mode"), TR("Enter the installer code"),
               ui_tone_amber(), installer_code);
}

static void recovery_confirmed(void)
{
    uint8_t user = 0;
    const alarm_result_t r = S.be.program_enter(S.be.ctx, NULL, &user);
    if (r != ALARM_OK) {
        ui_toast(r == ALARM_ERR_CODE_REQUIRED ? TR("The two minutes after power-up are over")
                 : r == ALARM_ERR_BAD_STATE   ? TR("Disarm first, and silence any alarm")
                                              : ui_result_text(r));
        ui_page_rebuild();
        return;
    }
    enter_program(user, true);
}

static void open_lost_code(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_INSTALLER_LOST, 0);
}

static void ask_recovery(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Open installer mode without the code?"),
               TR("For a lost installer code, right after power-up only. It is logged and "
                  "reported. Choose a new code next."),
               TR("Open installer mode"), recovery_confirmed);
}

/* Why the installer's changes were not saved (CORE-CFG-019), in the screen's language. Only what
 * these pages can change reaches here; the rest is worded generally. */
static void config_problem_text(char *out, size_t len, const alarm_config_why_t *w)
{
    const unsigned n = w->number;
    /* TR: the toasts below say why the installer's changes were not saved; {0} is a number */
    switch (w->problem) {
    case ALARM_CFG_CODE_LENGTH:
        i18n_utf8_copy(out, len, TR("Not saved: codes must be 4 to 10 digits long"));
        break;
    case ALARM_CFG_ABORT_WINDOW:
        i18n_utf8_copy(out, len, TR("Not saved: the abort window can be at most 45 seconds"));
        break;
    case ALARM_CFG_CANCEL_WINDOW:
        i18n_utf8_copy(out, len, TR("Not saved: the cancel window must be at least 5 minutes"));
        break;
    case ALARM_CFG_SWINGER:
        i18n_utf8_copy(out, len, TR("Not saved: the swinger limit must be 1 to 6 trips"));
        break;
    case ALARM_CFG_FIRE_TIMES:
        i18n_utf8_copy(out, len, TR("Not saved: the fire verify and smoke reset times can't be 0"));
        break;
    case ALARM_CFG_PANIC_KEY:
        i18n_utf8_copy(out, len, TR("Not saved: a panic key has no valid kind"));
        break;
    case ALARM_CFG_EXIT_DELAY:
        I18N_FORMAT(out, len, TR("Not saved: partition {0}'s exit delay must be 45 to 255 seconds"), n);
        break;
    case ALARM_CFG_ENTRY_DELAY:
        I18N_FORMAT(out, len, TR("Not saved: partition {0}'s entry delays must be 30 to 240 seconds"), n);
        break;
    case ALARM_CFG_DELAY_TOO_LONG:
        I18N_FORMAT(out, len, TR("Not saved: partition {0}'s delays can be at most 10 minutes"), n);
        break;
    case ALARM_CFG_NO_PARTITION:
        i18n_utf8_copy(out, len, TR("Not saved: at least one partition must be on"));
        break;
    case ALARM_CFG_ZONE_TYPE:
        I18N_FORMAT(out, len, TR("Not saved: zone {0} has no valid type"), n);
        break;
    case ALARM_CFG_ZONE_PARTITION:
        I18N_FORMAT(out, len, TR("Not saved: zone {0} is in a partition that's off"), n);
        break;
    case ALARM_CFG_ZONE_CROSS:
        I18N_FORMAT(out, len, TR("Not saved: zone {0}'s cross zone isn't a zone in use"), n);
        break;
    case ALARM_CFG_ZONE_USER:
        I18N_FORMAT(out, len, TR("Not saved: zone {0} arms or disarms, so it needs a user"), n);
        break;
    case ALARM_CFG_DEVICE_PARTITION:
        I18N_FORMAT(out, len, TR("Not saved: device {0} is in no valid partition"), n);
        break;
    default:
        /* a name, a user, the version: nothing these pages change */
        i18n_utf8_copy(out, len, TR("Not saved: the settings don't pass the panel's check"));
        break;
    }
}

/* The words the spoken-name editor's Done asked to approve, from the page's draft (D07): the
 * zone, 0 when none is pending or it can't be (no words, not used, no voice). */
static uint16_t voice_pending(uint16_t ids[ALARM_ZONE_WORDS], size_t *n)
{
    const uint16_t z = s_voice_zone;
    *n = 0u;
    if (z == 0u || z > ALARM_MAX_ZONES || S.be.voice_apply_approve == NULL) {
        return 0u;
    }
    const alarm_zone_config_t *zc = &S.edit->zones[z - 1u];
    const unsigned count = alarm_zone_word_count(zc);
    if (zc->type == ALARM_ZT_NOT_USED || count == 0u) {
        return 0u;
    }
    for (unsigned i = 0; i < count; i++) {
        ids[i] = alarm_zone_word(zc, i);
    }
    *n = count;
    return z;
}

/* The approval's outcome: the zone row says it when next drawn (Review spoken name if it was
 * refused, no toast); a refusal beeps. */
static void voice_outcome(uint8_t r)
{
    if (r != UI_VOICE_APPROVE_OK) {
        ui_beep(UI_BEEP_ERROR);
    }
}

void ui_installer_apply(void)
{
    if (!S.program || S.edit == NULL) {
        s_voice_zone = 0;
        return;
    }
    uint16_t ids[ALARM_ZONE_WORDS];
    size_t n = 0u;
    const uint16_t vz = voice_pending(ids, &n);
    if (!S.edit_dirty || S.be.set_config == NULL) {
        s_voice_zone = 0;
        if (vz != 0u && !S.edit_dirty) {
            /* Done on words already saved (a review): only the approval, in one transaction */
            voice_outcome(S.be.voice_apply_approve(S.be.ctx, NULL, vz, &s_voice_seen, ids, n, NULL));
            ui_refresh(true);
            memcpy(S.edit, S.cfg, sizeof(alarm_config_t)); /* the approval, as stored */
        }
        return;
    }
    if (s_scratch == NULL) {
        s_scratch = lv_malloc(sizeof(alarm_config_t));
        LV_ASSERT_MALLOC(s_scratch);
    }
    ui_refresh(true);
    memcpy(s_scratch, S.cfg, sizeof(alarm_config_t));
    s_scratch->opt = S.edit->opt;
    memcpy(s_scratch->partitions, S.edit->partitions, sizeof(s_scratch->partitions));
    memcpy(s_scratch->zones, S.edit->zones, sizeof(s_scratch->zones));
    memcpy(s_scratch->devices, S.edit->devices, sizeof(s_scratch->devices));
    alarm_config_why_t why;
    char text[128];
    uint8_t approved = UI_VOICE_APPROVE_REFUSED;
    alarm_result_t r = alarm_config_check(s_scratch, &why);
    if (r != ALARM_OK) {
        config_problem_text(text, sizeof(text), &why);
    } else {
        if (vz != 0u) {
            /* D07: the save and the approval Done asked for, one transaction; the save applies
             * whatever the approval's fate, and reports its own result */
            approved = S.be.voice_apply_approve(S.be.ctx, s_scratch, vz, &s_voice_seen, ids, n, &r);
        } else {
            r = S.be.set_config(S.be.ctx, s_scratch);
        }
        /* TR: a toast; {0} is the reason (cut to 95 bytes). At most 32 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Not saved: {0}"), ui_result_text(r));
    }
    if (r == ALARM_OK) {
        S.edit_dirty = false;
        s_voice_zone = 0; /* saved: the approval had its one chance (a failed save keeps it) */
        ui_refresh(true);
        memcpy(S.edit, S.cfg, sizeof(alarm_config_t));
        /* no toast: the edited rows show their new values (UI-028) */
        if (vz != 0u) {
            voice_outcome(approved);
        }
    } else {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(text);
    }
}

/* say: the toast that arming is on again (not when a walk test starts next: arming stays off) */
static void leave_mode(bool say)
{
    if (!S.program) {
        return;
    }
    ui_installer_apply();
    S.edit_dirty = false;
    s_voice_zone = 0; /* an approval that wasn't saved is dropped with the edits */
    S.program = false;
    if (S.be.program_exit != NULL) {
        S.be.program_exit(S.be.ctx);
    }
    ui_refresh(true);
    if (say) {
        ui_toast(TR("Installer mode ended · arming is on again"));
    }
    const unsigned root = ui_installer_root();
    if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS &&
        S.depth >= root + 1u && S.pages[root].id == PAGE_INSTALLER) {
        S.depth = root + 1u; /* About, then the Installer page (BL-189); a keypad's own (BL-223) */
        ui_page_rebuild();
    }
}

void ui_installer_leave(void)
{
    leave_mode(true);
}

static void leave_clicked(void *ctx)
{
    (void)ctx;
    ui_installer_leave();
}

static void dirty(void)
{
    S.edit_dirty = true;
}

/* ---- locked page and menu ------------------------------------------------------------------- */

static void open_page(void *ctx)
{
    ui_page_open((page_id_t)(intptr_t)ctx, 0);
}

static void run_setup(void *ctx)
{
    (void)ctx;
    if (!S.program) { /* the mode ended under the finger */
        ui_page_rebuild();
        return;
    }
    ui_installer_leave();
    ui_app_run_setup();
}

/* The walk test can't run in installer mode (CORE-PROG-002/003): this leaves it and starts the
 * test in one tap (docs/features/40, "Touchscreen"). */
static void run_walk_test(void *ctx)
{
    (void)ctx;
    if (!S.program) { /* the mode ended under the finger */
        ui_page_rebuild();
        return;
    }
    ui_installer_apply(); /* a failure toasts "Not saved": leave_mode drops the edits after it */
    const bool unsaved = S.edit_dirty;
    S.edit_dirty = false;
    leave_mode(false);
    ui_walk_from_installer(unsaved);
}

/* A keypad's Installer before the code (BL-223, LINK-060): its alarm panel's, the code checked
 * there; installer mode open in one place at a time; no power-up recovery (it is for whoever is at
 * the alarm panel) and no lost-code row (unplugging a keypad doesn't open it). */
static void build_locked_keypad(lv_obj_t *body, char *note, size_t nlen)
{
    const char *master = ui_link_master_name();
    /* TR: a keypad's Installer, the line under the title; {0} is its alarm panel's name (up to
     * 47 bytes). At most 127 bytes */
    I18N_FORMAT(note, nlen, TR("Changes how {0} works"), master);
    const uint8_t holder = ui_link_remote_holder();
    const bool elsewhere = S.snap.program && holder != UI_LINK_HOLDER_NONE;
    if (elsewhere) {
        char text[128];
        if (holder == UI_LINK_HOLDER_PANEL) {
            /* TR: a keypad's Installer while installer mode is held at the alarm panel itself or
             * its Panel Manager; {0} is the alarm panel's name. At most 127 bytes */
            I18N_FORMAT(text, sizeof(text),
                        TR("Installer mode is open on {0}. It can be open in one place at a time."),
                        master);
        } else {
            /* TR: a keypad's Installer while another keypad holds installer mode; {0} is that
             * keypad's number. At most 127 bytes */
            I18N_FORMAT(text, sizeof(text),
                        TR("Installer mode is open at Keypad {0}. It can be open in one place at a time."),
                        (unsigned)holder);
        }
        ui_paragraph(body, text, ui_tone_amber());
    }
    lv_obj_t *enter = ui_row_action(body, UI_ICON_KEY, TR("Enter the installer code"),
                                    ui_tone_amber(), ask_code, NULL);
    if (elsewhere) {
        ui_row_disable(enter);
    }
    char line[160];
    /* TR: a keypad's Installer, under the code; {0} is its alarm panel's name. At most 159 bytes */
    I18N_FORMAT(line, sizeof(line),
                TR("While you change settings nothing can be armed, here or on {0}. Fire, CO and "
                   "panic buttons keep working."),
                master);
    ui_paragraph(body, line, ui_tone_gray());
}

static void build_locked(lv_obj_t *body, char *note, size_t nlen)
{
    if (ui_link_keypad()) {
        build_locked_keypad(body, note, nlen);
        return;
    }
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Installer settings change how the alarm works"));
    ui_row_action(body, UI_ICON_KEY, TR("Enter the installer code"), ui_tone_amber(), ask_code,
                  NULL);
    const alarm_ms_t now = S.be.now_ms(S.be.ctx);
    const alarm_ms_t elapsed = now > S.snap.taken_ms ? now - S.snap.taken_ms : 0u;
    const alarm_ms_t left =
        S.snap.recovery_left_ms > elapsed ? S.snap.recovery_left_ms - elapsed : 0u;
    if (left > 0u && S.be.program_enter != NULL) {
        char text[80];
        const unsigned secs = (unsigned)((left + 999u) / 1000u);
        /* TR: a locked page, a row; {0} is the seconds left. At most 70 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("Forgot the code? Open without it ({0} s left)",
                        "Forgot the code? Open without it ({0} s left)", secs),
                    secs);
        ui_row_action(body, UI_ICON_LIFE_BUOY, text, ui_tone_orange(), ask_recovery, NULL);
    } else {
        /* a row that opens the explanation, not a paragraph under the code button (BL-131) */
        /* TR: Installer, before the code: the row that says what to do about a lost code. At most
         * 47 bytes */
        ui_row_link(body, UI_ICON_LIFE_BUOY, ui_tone_gray(), TR("Lost the installer code?"), NULL, NULL,
                    UI_ICON_CHEVRON_RIGHT, open_lost_code, NULL);
    }
    ui_paragraph(body,
                 TR("While you change settings nothing can be armed. Fire, CO and panic "
                 "buttons keep working."),
                 ui_tone_gray());
}

static void remote_rebuild(void *unused)
{
    (void)unused;
    ui_page_rebuild();
}
static void remote_toggle(bool on, void *ctx)
{
    if (!S.program) { /* the mode ended under the finger: no change without the code */
        ui_page_rebuild();
        return;
    }
    bool web = false, developer = false;
    S.be.remote_get(S.be.ctx, &web, &developer);
    if ((intptr_t)ctx == 0) {
        web = on;
        if (!on)
            developer = false;
    } else {
        developer = on;
        if (on)
            web = true;
    }
    if (!S.be.remote_set(S.be.ctx, web, developer))
        ui_toast(TR("Could not save remote access"));
    lv_async_call(remote_rebuild, NULL);
}

/* "8 in use", or "None" (BL-131 finding 25). */
static void in_use_words(unsigned n, char *text, size_t len)
{
    if (n == 0u) {
        /* TR: the Installer menu, the value of the Zones, Partitions or Keypads and devices row
         * when there are none. At most 40 bytes */
        i18n_utf8_copy(text, len, TR_C("installer menu count", "None"));
        return;
    }
    /* TR: the Installer menu, the value of the Zones, Partitions or Keypads and devices row; {0}
     * is how many. At most 40 bytes */
    I18N_FORMAT(text, len, TRN("{0} in use", "{0} in use", n), n);
}

/* The menu in four groups, the risky ones last (BL-131 n-installer-menu). No note: the status
 * bar's banner already says installer mode is on and arming is off. */
static void build_menu(lv_obj_t *body, char *note, size_t nlen)
{
    char text[64];
    unsigned zones = 0, parts = 0, devices = 0;
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        zones += S.edit->zones[z].type != ALARM_ZT_NOT_USED;
    }
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        parts += S.edit->partitions[i].enabled;
    }
    for (unsigned i = 0; i < ALARM_MAX_DEVICES; i++) {
        devices += S.edit->devices[i].enabled;
    }
    devices += ui_link_keypad_count(); /* the keypads on Wi-Fi too (BL-048) */
    ui_section(body, TR("Sensors and devices"));
    in_use_words(zones, text, sizeof(text));
    ui_row_link(body, UI_ICON_DOOR_CLOSED, ui_tone_green(), TR("Zones"), TR("Sensors, types, names"),
                text, UI_ICON_CHEVRON_RIGHT, open_page, (void *)(intptr_t)PAGE_ZONES);
#if ALARM_UI_ZWAVE
    ui_zwave_menu_row(body); /* docs/features/36 screens.md §2: after Zones */
#endif
#if ALARM_UI_ZIGBEE
    ui_zigbee_menu_row(body); /* docs/features/20 devices.md §9, BL-144: after Z-Wave */
#endif
    ui_sirens_rf_menu_row(body); /* BL-037: 433 MHz, after Zigbee (only with something on it) */
    in_use_words(parts, text, sizeof(text));
    ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR("Partitions"),
                TR("Areas that arm separately"), text, UI_ICON_CHEVRON_RIGHT, open_page,
                (void *)(intptr_t)PAGE_PARTITIONS);
    in_use_words(devices, text, sizeof(text));
    ui_row_link(body, UI_ICON_CPU, ui_tone_blue(), TR("Keypads and devices"),
                TR("Supervised keypads and expanders"), text, UI_ICON_CHEVRON_RIGHT, open_page,
                (void *)(intptr_t)PAGE_DEVICES);
    ui_sirens_menu_row(body); /* BL-037: every siren the panel sounds */

    ui_section(body, TR("How the alarm works"));
    ui_row_link(body, UI_ICON_SLIDERS, ui_tone_blue(), TR("Delays and arming"),
                TR("Entry and exit times, sirens, panic keys, codes"), NULL, UI_ICON_CHEVRON_RIGHT,
                open_page, (void *)(intptr_t)PAGE_OPTIONS);

    ui_section(body, TR("This panel"));
    ui_row_link(body, UI_ICON_HOUSE, ui_tone_violet(), TR("System name"),
                TR("Shown on the screen and in the event log"), S.edit->partitions[part_index()].name,
                UI_ICON_CHEVRON_RIGHT, open_page, (void *)(intptr_t)PAGE_PARTITION);
    ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_amber(), TR("Installer and master codes"),
                S.snap.default_codes != 0u ? TR("Factory codes still in use")
                                           : TR("Changed from the factory codes"),
                NULL, UI_ICON_CHEVRON_RIGHT, open_page, (void *)(intptr_t)PAGE_CODES);
    if (S.be.remote_get && S.be.remote_set) {
        /* The Panel Manager in one place: the switch, the installer's own sign-in under it
         * (docs/features/31), then Developer mode (it needs the Panel Manager running). */
        bool web = false, developer = false;
        S.be.remote_get(S.be.ctx, &web, &developer);
        char info[200] = "";
        if (web && S.be.remote_info) {
            S.be.remote_info(S.be.ctx, info, sizeof(info)); /* its address and certificate (WEB-002) */
        }
        ui_row_toggle_icon(body, UI_ICON_MONITOR, ui_tone_blue(), TR("Panel Manager"),
                           info[0] != '\0' ? info
                                           : TR("Change settings from a browser on your network"),
                           web, remote_toggle, (void *)(intptr_t)0);
        /* The installer signs in with their name on the Users screen ("Installer" from the
         * factory) and a password of their own, never the installer code. */
        /* right under the Panel Manager's switch: no heading of its own (BL-131 finding 18) */
        ui_login_rows(body, ALARM_USER_INSTALLER, ui_login_name(S.cfg, ALARM_USER_INSTALLER),
                      false);
        ui_row_toggle_icon(body, UI_ICON_CODE_XML, ui_tone_gray(), TR("Developer mode"),
                           TR("Enrolled development tools and signed network updates"), developer,
                           remote_toggle, (void *)(intptr_t)1);
    }
    if (ui_link_keypad()) {
        /* its alarm panel's menu (BL-223): the alarm panel's name under the title; its updates,
         * developer mode, setup and factory reset stay on it and its Panel Manager */
        snprintf(note, nlen, "%s", ui_link_master_name());
        ui_section(body, TR("Test and start over"));
        /* TR: the Installer menu's walk test row and the line under it (docs/features/40) */
        ui_row_link(body, UI_ICON_FOOTPRINTS, ui_tone_blue(), TR("Walk test"),
                    TR("Leaves installer mode and starts one"), NULL, UI_ICON_CHEVRON_RIGHT,
                    run_walk_test, NULL);
        char line[160];
        /* TR: a keypad's Installer menu, what it doesn't hold; {0} is its alarm panel's name (up
         * to 47 bytes). At most 159 bytes */
        I18N_FORMAT(line, sizeof(line),
                    TR("Updates, developer mode, the setup and a factory reset of {0} are on its "
                       "Panel Manager."),
                    ui_link_master_name());
        ui_paragraph(body, line, ui_tone_gray());
        ui_row_action(body, UI_ICON_LOG_OUT, TR("Leave installer mode"), ui_tone_blue(),
                      leave_clicked, NULL);
        return;
    }
    ui_fwsrv_menu(body); /* Updates: the update server and the channel (BL-186) */

    ui_section(body, TR("Test and start over"));
    /* TR: the Installer menu's walk test row and the line under it (docs/features/40) */
    ui_row_link(body, UI_ICON_FOOTPRINTS, ui_tone_blue(), TR("Walk test"),
                TR("Leaves installer mode and starts one"), NULL, UI_ICON_CHEVRON_RIGHT,
                run_walk_test, NULL);
    ui_row_link(body, UI_ICON_SPARKLES, ui_tone_rose(), TR("Run the setup again"),
                TR("Wi-Fi, time zone, codes and name, step by step"), NULL, UI_ICON_CHEVRON_RIGHT,
                run_setup, NULL);
    ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_red(), TR("Factory reset"), TR("Erase everything"), NULL,
                UI_ICON_CHEVRON_RIGHT, open_page, (void *)(intptr_t)PAGE_RESET);
    ui_row_action(body, UI_ICON_LOG_OUT, TR("Leave installer mode"), ui_tone_blue(), leave_clicked,
                  NULL);
}

/* ---- system options ------------------------------------------------------------------------ */

typedef enum {
    OPT_EXIT,
    OPT_ENTRY1,
    OPT_ENTRY2,
    OPT_ABORT,
    OPT_BELL,
    OPT_MIN_LEN,
    OPT_LOCKOUT_TRIES,
    OPT_LOCKOUT_TIME,
    OPT_CANCEL,
    OPT_SWINGER,
    OPT_CROSS_WINDOW,
    OPT_FIRE_VERIFY,
    OPT_SMOKE_RESET,
    OPT_EXIT_RESTARTS,
    OPT_RECENT_CLOSING,
    OPT_SETTLE,
    OPT_WALK_TEST,
} num_opt_t;

static void on_number(int32_t v, void *ctx)
{
    alarm_partition_config_t *p = &S.edit->partitions[part_index()];
    alarm_options_t *o = &S.edit->opt;
    switch ((num_opt_t)(intptr_t)ctx) {
    case OPT_EXIT: p->exit_delay_s = (uint16_t)v; break;
    case OPT_ENTRY1: p->entry_delay1_s = (uint16_t)v; break;
    case OPT_ENTRY2: p->entry_delay2_s = (uint16_t)v; break;
    case OPT_ABORT: o->abort_window_s = (uint8_t)v; break;
    case OPT_BELL: o->bell_timeout_s = (uint16_t)v; break;
    case OPT_MIN_LEN: o->code_min_len = (uint8_t)v; break;
    case OPT_LOCKOUT_TRIES: o->lockout_attempts = (uint8_t)v; break;
    case OPT_LOCKOUT_TIME: o->lockout_s = (uint16_t)v; break;
    case OPT_CANCEL: o->cancel_window_s = (uint16_t)v; break;
    case OPT_SWINGER: o->swinger_trips = (uint8_t)v; break;
    case OPT_CROSS_WINDOW: o->cross_zone_window_s = (uint16_t)v; break;
    case OPT_FIRE_VERIFY: o->fire_verify_window_s = (uint16_t)v; break;
    case OPT_SMOKE_RESET: o->smoke_reset_s = (uint8_t)v; break;
    case OPT_EXIT_RESTARTS: o->exit_restart_limit = (uint8_t)v; break;
    case OPT_RECENT_CLOSING: o->recent_closing_s = (uint16_t)v; break;
    case OPT_SETTLE: o->power_up_settle_s = (uint16_t)v; break;
    case OPT_WALK_TEST: o->walk_test_timeout_s = (uint16_t)v; break;
    }
    dirty();
}

typedef enum {
    FLAG_AUTO_STAY,
    FLAG_EXIT_SOUNDS,
    FLAG_QUICK_ARM,
    FLAG_QUICK_EXIT,
    FLAG_FIRE_TIMEOUT,
    FLAG_POWER_UP_STATE,
    FLAG_RECOVERY,
    FLAG_REMOTE_ARM,
    FLAG_CHIME_CODE,
    FLAG_RELAXED,
    FLAG_AUTO_LESS_SECURE,
} flag_opt_t;

static void on_flag(bool on, void *ctx)
{
    alarm_options_t *o = &S.edit->opt;
    switch ((flag_opt_t)(intptr_t)ctx) {
    case FLAG_AUTO_STAY: o->auto_stay = on; break;
    case FLAG_EXIT_SOUNDS: o->exit_sounds = on; break;
    case FLAG_QUICK_ARM: o->quick_arm = on; break;
    case FLAG_QUICK_EXIT: o->quick_exit = on; break;
    case FLAG_FIRE_TIMEOUT: o->fire_bell_timeout = on; break;
    case FLAG_POWER_UP_STATE: o->power_up_previous_state = on; break;
    case FLAG_RECOVERY: o->power_up_program = on; break;
    case FLAG_REMOTE_ARM: o->remote_arm_without_code = on; break;
    case FLAG_CHIME_CODE: o->chime_needs_code = on; break;
    case FLAG_RELAXED: o->relaxed_limits = on; break;
    case FLAG_AUTO_LESS_SECURE: o->auto_less_secure = on; break;
    }
    dirty();
    if ((flag_opt_t)(intptr_t)ctx == FLAG_RELAXED) {
        ui_page_rebuild(); /* the steppers' ranges follow */
    }
}

static void build_options(lv_obj_t *body, char *note, size_t nlen)
{
    const alarm_partition_config_t *p = &S.edit->partitions[part_index()];
    const alarm_options_t *o = &S.edit->opt;
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Saved when you go back · SIA CP-01 limits apply"));
    /* Validation warnings (CFG-008). */
    if (p->entry_delay1_s + o->abort_window_s > 60u) {
        ui_paragraph(body,
                     TR("Entry time plus the report delay is over 60 seconds. SIA CP-01 asks "
                     "for 60 at most to keep false alarms and late reports down."),
                     ui_tone_amber());
    }
    bool night = false;
    for (unsigned z = 0; z < ALARM_MAX_ZONES && !night; z++) {
        night =
            S.edit->zones[z].type != ALARM_ZT_NOT_USED && (S.edit->zones[z].flags & ALARM_ZF_NIGHT);
    }
    if (!night) {
        ui_paragraph(body, TR("No zone is a night zone yet, so Night arms like Stay."), ui_tone_gray());
    }
    ui_section(body, TR("Delays"));
    ui_row_number(body, TR("Exit time"), TR("Time to leave after arming"), p->exit_delay_s, 45, 255, 5,
                  UNIT_SECONDS, NULL, on_number, (void *)(intptr_t)OPT_EXIT);
    ui_row_number(body, TR("Entry time 1"), TR("Time to disarm after the main door opens"),
                  p->entry_delay1_s, 30, 240, 5, UNIT_SECONDS, NULL, on_number,
                  (void *)(intptr_t)OPT_ENTRY1);
    ui_row_number(body, TR("Entry time 2"), TR("For a second door, like the garage"), p->entry_delay2_s, 30,
                  240, 5, UNIT_SECONDS, NULL, on_number, (void *)(intptr_t)OPT_ENTRY2);
    ui_row_number(body, TR("Report delay"), TR("Disarm within this time and no burglary alarm is sent"),
                  o->abort_window_s, 0, 45, 5, UNIT_SECONDS, TR("Report at once"), on_number,
                  (void *)(intptr_t)OPT_ABORT);
    ui_row_number(body, TR("Siren time"), TR("How long a burglary alarm sounds"), o->bell_timeout_s, 0,
                  1800, 60, UNIT_MINUTES, TR("Until disarmed"), on_number, (void *)(intptr_t)OPT_BELL);
    ui_row_toggle(body, TR("Fire and CO sirens stop by themselves"),
                  TR("Off: they sound until silenced (recommended)"), o->fire_bell_timeout, on_flag,
                  (void *)(intptr_t)FLAG_FIRE_TIMEOUT);
    const bool strict = !o->relaxed_limits;
    ui_row_number(body, TR("Cancel window"), TR("Disarming this soon after an alarm was reported sends a cancel"),
                  o->cancel_window_s, strict ? 300 : 0, 3600, 60, UNIT_MINUTES, TR("No cancel"),
                  on_number, (void *)(intptr_t)OPT_CANCEL);
    ui_section(body, TR("Alarms"));
    ui_row_number(body, TR("Alarm limit"), TR("A zone that goes off this often while armed is shut down "
                                       "until disarmed"),
                  o->swinger_trips, 1, strict ? 6 : 15, 1, UNIT_TIMES, NULL, on_number,
                  (void *)(intptr_t)OPT_SWINGER);
    ui_row_number(body, TR("Cross-zone time"), TR("Two cross-zoned sensors must both trip within this"),
                  o->cross_zone_window_s, 10, 900, 10, UNIT_SECONDS, NULL, on_number,
                  (void *)(intptr_t)OPT_CROSS_WINDOW);
    ui_row_number(body, TR("Fire verification"), TR("A verified smoke detector must trip again within this"),
                  o->fire_verify_window_s, 30, 300, 10, UNIT_SECONDS, NULL, on_number,
                  (void *)(intptr_t)OPT_FIRE_VERIFY);
    ui_row_number(body, TR("Smoke detector reset"), TR("How long smoke detectors lose power to reset"),
                  o->smoke_reset_s, 1, 30, 1, UNIT_SECONDS, NULL, on_number,
                  (void *)(intptr_t)OPT_SMOKE_RESET);
    ui_row_link(body, UI_ICON_SIREN, ui_tone_red(), TR("Panic keys"),
                TR("What the keypad's A, B and C keys send"), NULL, UI_ICON_CHEVRON_RIGHT, open_page,
                (void *)(intptr_t)PAGE_PANIC_KEYS);
    ui_row_link(body, UI_ICON_BELL, ui_tone_amber(), TR("Trouble beeps"),
                TR("Which troubles make the keypad beep"), NULL, UI_ICON_CHEVRON_RIGHT, open_page,
                (void *)(intptr_t)PAGE_TROUBLE_BEEPS);
    ui_section(body, TR("Arming"));
    ui_row_toggle(body, TR("Auto-stay"), TR("Away becomes Stay when nobody leaves through a door"),
                  o->auto_stay, on_flag, (void *)(intptr_t)FLAG_AUTO_STAY);
    ui_row_toggle(body, TR("Exit beeps"), TR("Beep through Away exit time (faster in the last 10 seconds)"),
                  o->exit_sounds, on_flag, (void *)(intptr_t)FLAG_EXIT_SOUNDS);
    ui_row_toggle(body, TR("Arm without a code"), TR("Quick arm. Disarming always needs a code"),
                  o->quick_arm, on_flag, (void *)(intptr_t)FLAG_QUICK_ARM);
#if ALARM_UI_AUTOMATIONS
    if (ui_automations_available()) {
        /* AUT-015, D25: the installer alone decides whether automations may disarm, unlock,
         * open or stop (drawing installer-options-automations). Off, the editors don't offer
         * them and a stored one can't run. */
        /* TR: an installer option's title, in System options > Arming. At most 63 bytes */
        ui_row_toggle(body, TR("Allow less secure automations"),
                      /* TR: the line under Allow less secure automations. At most 95 bytes */
                      TR("Automations may disarm, unlock and open"), o->auto_less_secure, on_flag,
                      (void *)(intptr_t)FLAG_AUTO_LESS_SECURE);
    }
#endif
    ui_row_toggle(body, TR("Arm remotely without a code"),
                  TR("Connected services may arm without a code. Disarming always needs one"),
                  o->remote_arm_without_code, on_flag, (void *)(intptr_t)FLAG_REMOTE_ARM);
    ui_row_toggle(body, TR("Quick exit"), TR("Restart exit time without a code while armed Stay or Night"),
                  o->quick_exit, on_flag, (void *)(intptr_t)FLAG_QUICK_EXIT);
    ui_row_number(body, TR("Exit time restarts"), TR("Away exit time starts over when someone comes back "
                                              "in before it ends"),
                  o->exit_restart_limit, 0, 3, 1, UNIT_TIMES, TR_C("setting value", "Never"), on_number,
                  (void *)(intptr_t)OPT_EXIT_RESTARTS);
    ui_row_number(body, TR("Recent closing"), TR("An alarm this soon after arming is reported as "
                                          "a recent closing"),
                  o->recent_closing_s, 0, 600, 30, UNIT_SECONDS, TR_C("setting value", "Off"), on_number,
                  (void *)(intptr_t)OPT_RECENT_CLOSING);
    ui_section(body, TR("Chime"));
    ui_row_toggle(body, TR("Chime needs a code"),
                  TR("On: switching the chime on or off takes a code. Off: one touch turns it on "
                  "or off. Either way it changes only while the system is disarmed"),
                  o->chime_needs_code, on_flag, (void *)(intptr_t)FLAG_CHIME_CODE);
    ui_section(body, TR("Codes"));
    ui_row_number(body, TR("Shortest new code"), TR("Existing shorter codes keep working"), o->code_min_len,
                  ALARM_CODE_MIN_LEN, ALARM_CODE_MAX_LEN, 1, UNIT_DIGITS, NULL, on_number,
                  (void *)(intptr_t)OPT_MIN_LEN);
    ui_row_number(body, TR("Wrong codes before lockout"), TR("The keypad locks for a while"),
                  o->lockout_attempts, 0, 10, 1, UNIT_PLAIN, TR_C("setting value", "Never lock"), on_number,
                  (void *)(intptr_t)OPT_LOCKOUT_TRIES);
    ui_row_number(body, TR("Lockout time"), NULL, o->lockout_s, 60, 1800, 60, UNIT_MINUTES, NULL,
                  on_number, (void *)(intptr_t)OPT_LOCKOUT_TIME);
    ui_section(body, TR("Power"));
    ui_row_toggle(body, TR("Come back armed after a power cut"), TR("Otherwise the panel starts disarmed"),
                  o->power_up_previous_state, on_flag, (void *)(intptr_t)FLAG_POWER_UP_STATE);
    ui_row_toggle(body, TR("Installer code recovery"),
                  TR("For two minutes after power-up, Installer opens without the code. When off, "
                  "a lost installer code can't be recovered"),
                  o->power_up_program, on_flag, (void *)(intptr_t)FLAG_RECOVERY);
    ui_row_number(body, TR("Settling time"), TR("After power-up, zone alarms wait this long for the "
                                         "sensors to settle"),
                  o->power_up_settle_s, 0, 300, 10, UNIT_SECONDS, TR_C("setting value", "None"), on_number,
                  (void *)(intptr_t)OPT_SETTLE);
    ui_section(body, TR("Walk test"));
    ui_row_number(body, TR("Walk test ends after"), NULL, o->walk_test_timeout_s, 600, 14400, 600,
                  UNIT_MINUTES, NULL, on_number, (void *)(intptr_t)OPT_WALK_TEST);
    ui_section(body, TR("Special cases"));
    ui_row_toggle(body, TR("Allow values outside SIA CP-01"),
                  TR("For special installations only, never a monitored one: shorter delays, more "
                  "alarms per zone, no cancel window"),
                  o->relaxed_limits, on_flag, (void *)(intptr_t)FLAG_RELAXED);
}

/* ---- panic keys and trouble beeps ------------------------------------------------------------ */

static const char *const PANIC_NAMES[] = {TR_NOOP_C("panic key", "Police"),
                                          TR_NOOP_C("panic key", "Silent panic"),
                                          TR_NOOP_C("panic key", "Fire"),
                                          TR_NOOP_C("panic key", "Medical")};
static uint8_t s_panic_key;

static void panic_chosen(int32_t index)
{
    if (index >= 0 && index <= (int32_t)ALARM_PANIC_MEDICAL) {
        S.edit->opt.panic_keys[s_panic_key] = (alarm_panic_t)index;
        dirty();
    }
    ui_page_rebuild();
}

static void pick_panic(void *ctx)
{
    s_panic_key = (uint8_t)(uintptr_t)ctx;
    static ui_choice_t choices[4];
    for (size_t i = 0; i < 4u; i++) {
        choices[i].glyph = UI_ICON_SIREN;
        choices[i].tone = i == ALARM_PANIC_FIRE ? ui_tone_orange() : ui_tone_red();
        choices[i].title = TR_C("panic key", PANIC_NAMES[i]);
        choices[i].detail = i == ALARM_PANIC_SILENT ? TR("Reported, nothing sounds") : NULL;
    }
    ui_pick_sheet(TR("Panic key"), TR("What the key sends when it is held"), choices, 4,
                  (int32_t)S.edit->opt.panic_keys[s_panic_key], panic_chosen);
}

static void build_panic_keys(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("The keypad keys A, B and C, held for two seconds"));
    static const char *const KEY_NAMES[] = {TR_NOOP("Key A"), TR_NOOP("Key B"), TR_NOOP("Key C")};
    for (uintptr_t k = 0; k < 3u; k++) {
        const unsigned p = (unsigned)S.edit->opt.panic_keys[k];
        ui_row_link(body, UI_ICON_SIREN, ui_tone_red(), TR(KEY_NAMES[k]), NULL,
                    p < 4u ? TR_C("panic key", PANIC_NAMES[p]) : "?", UI_ICON_CHEVRON_RIGHT, pick_panic, (void *)k);
    }
    ui_paragraph(body, TR("The Panic page on this screen always offers police, fire and medical."),
                 ui_tone_gray());
}

static void on_beep(bool on, void *ctx)
{
    const uint32_t bit = 1u << (unsigned)(uintptr_t)ctx;
    S.edit->opt.trouble_beep_mask = on ? (S.edit->opt.trouble_beep_mask | bit)
                                       : (S.edit->opt.trouble_beep_mask & ~bit);
    dirty();
}

static void build_trouble_beeps(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Troubles still show on the screen; these also beep until acknowledged"));
    for (uintptr_t t = 0; t < ALARM_TRBL_COUNT; t++) {
        notify_words_t w;
        notify_sys_trouble_words(S.catalog, (alarm_sys_trouble_t)t, false, &w);
        ui_row_toggle(body, w.title, NULL, (S.edit->opt.trouble_beep_mask >> t) & 1u, on_beep,
                      (void *)t);
    }
}

/* ---- partitions ------------------------------------------------------------------------------- */

static unsigned partitions_used(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        n += S.edit->partitions[i].enabled;
    }
    return n;
}

static const char *partition_label(uint8_t p, char *buf, size_t len)
{
    if (p < 1u || p > ALARM_MAX_PARTITIONS) {
        i18n_utf8_copy(buf, len, TR_C("partition", "None"));
        return buf;
    }
    const char *name = S.edit->partitions[p - 1u].name;
    snprintf(buf, len, "%s", name[0] != '\0' ? name : "");
    if (buf[0] == '\0') {
        /* TR: the name of a partition nobody has named; {0} is its number */
        I18N_FORMAT(buf, len, TR_C("default partition name", "Partition {0}"), (unsigned)p);
    }
    return buf;
}

static void open_partition(void *ctx)
{
    ui_page_open(PAGE_PARTITION_EDIT, (int32_t)(intptr_t)ctx);
}

static void build_partitions(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Areas that arm and disarm separately · saved when you go back"));
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        const alarm_partition_config_t *pc = &S.edit->partitions[p - 1u];
        char name[ALARM_NAME_LEN + 16], detail[64];
        if (pc->enabled) {
            char secs[3][16];
            for (unsigned i = 0; i < 3u; i++) {
                const unsigned v = i == 0u   ? (unsigned)pc->exit_delay_s
                                   : i == 1u ? (unsigned)pc->entry_delay1_s
                                             : (unsigned)pc->entry_delay2_s;
                /* TR: a number of seconds; {0} is the number. At most 15 bytes */
                I18N_FORMAT(secs[i], sizeof(secs[i]), TRN("{0} s", "{0} s", v), v);
            }
            char times[48];
            /* TR: Installer > Partitions, a row's line: exit time {0}, entry times {1} and {2}.
             * At most 30 bytes */
            I18N_FORMAT(times, sizeof(times), TR("Exit {0} · entry {1} / {2}"), secs[0], secs[1],
                        secs[2]);
            snprintf(detail, sizeof(detail), "%s%s%s", times,
                     p == S.opt.partition ? " · " : "",
                     p == S.opt.partition ? TR("this keypad") : "");
        } else {
            i18n_utf8_copy(detail, sizeof(detail), TR("Not in use"));
        }
        ui_row_link(body, UI_ICON_WAREHOUSE, pc->enabled ? ui_tone_violet() : ui_tone_gray(),
                    partition_label(p, name, sizeof(name)), detail, NULL, UI_ICON_CHEVRON_RIGHT,
                    open_partition, (void *)(intptr_t)p);
    }
    ui_paragraph(body, TR("This keypad shows and arms its own partition. Zones and users belong to "
                       "partitions on their own pages."),
                 ui_tone_gray());
}

static uint8_t s_partition;

static void on_partition_number(int32_t v, void *ctx)
{
    alarm_partition_config_t *p = &S.edit->partitions[s_partition - 1u];
    switch ((num_opt_t)(intptr_t)ctx) {
    case OPT_EXIT: p->exit_delay_s = (uint16_t)v; break;
    case OPT_ENTRY1: p->entry_delay1_s = (uint16_t)v; break;
    case OPT_ENTRY2: p->entry_delay2_s = (uint16_t)v; break;
    default: break;
    }
    dirty();
}

static void on_partition_used(bool on, void *ctx)
{
    (void)ctx;
    alarm_partition_config_t *p = &S.edit->partitions[s_partition - 1u];
    if (!on) {
        bool zones = false;
        for (unsigned z = 0; z < ALARM_MAX_ZONES && !zones; z++) {
            zones = S.edit->zones[z].type != ALARM_ZT_NOT_USED &&
                    S.edit->zones[z].partition == s_partition;
        }
        if (s_partition == S.opt.partition || zones) {
            ui_beep(UI_BEEP_ERROR);
            ui_toast(zones ? TR("Move its zones to another partition first")
                           : TR("This keypad's own partition stays in use"));
            ui_page_rebuild();
            return;
        }
    } else if (p->exit_delay_s == 0u) {
        p->exit_delay_s = 60; /* a partition switched on for the first time: the defaults */
        p->entry_delay1_s = 30;
        p->entry_delay2_s = 45;
    }
    p->enabled = on;
    dirty();
    ui_page_rebuild();
}

static void partition_renamed(const char *text)
{
    alarm_partition_config_t *p = &S.edit->partitions[s_partition - 1u];
    memset(p->name, 0, ALARM_NAME_LEN);
    i18n_utf8_copy(p->name, ALARM_NAME_LEN, text);
    dirty();
    ui_page_rebuild();
}

static void rename_partition(void *ctx)
{
    (void)ctx;
    ui_text_sheet(TR("Partition name"), TR("Upstairs, Garage, Shop..."),
                  S.edit->partitions[s_partition - 1u].name, false, ALARM_NAME_LEN - 1u,
                  partition_renamed);
}

static void build_partition_edit(lv_obj_t *body, int32_t arg, char *title, size_t tlen,
                                 char *note, size_t nlen)
{
    s_partition = (uint8_t)arg;
    if (s_partition < 1u || s_partition > ALARM_MAX_PARTITIONS) {
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Partition"));
        return;
    }
    const alarm_partition_config_t *p = &S.edit->partitions[s_partition - 1u];
    char name[ALARM_NAME_LEN + 16];
    i18n_utf8_copy(title, tlen, partition_label(s_partition, name, sizeof(name)));
    /* TR: the line under a partition's title; {0} is its number. At most 100 bytes */
    I18N_FORMAT(note, nlen, TR("Partition {0} · saved when you go back"), (unsigned)s_partition);
    ui_row_toggle(body, TR("In use"), TR("Its zones and users arm and disarm together"), p->enabled,
                  on_partition_used, NULL);
    if (!p->enabled) {
        return;
    }
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL, p->name, UI_ICON_CHEVRON_RIGHT,
                rename_partition, NULL);
    const bool strict = !S.edit->opt.relaxed_limits;
    ui_row_number(body, TR("Exit time"), TR("Time to leave after arming"), p->exit_delay_s, strict ? 45 : 0,
                  strict ? 255 : 600, 5, UNIT_SECONDS, NULL, on_partition_number,
                  (void *)(intptr_t)OPT_EXIT);
    ui_row_number(body, TR("Entry time 1"), TR("Time to disarm after the main door opens"),
                  p->entry_delay1_s, strict ? 30 : 0, strict ? 240 : 600, 5, UNIT_SECONDS, NULL,
                  on_partition_number, (void *)(intptr_t)OPT_ENTRY1);
    ui_row_number(body, TR("Entry time 2"), TR("For a second door, like the garage"), p->entry_delay2_s,
                  strict ? 30 : 0, strict ? 240 : 600, 5, UNIT_SECONDS, NULL, on_partition_number,
                  (void *)(intptr_t)OPT_ENTRY2);
}

/* ---- keypads and devices ---------------------------------------------------------------------- */

static uint8_t s_device;

static const char *device_label(uint8_t d, char *buf, size_t len)
{
    const char *name = S.edit->devices[d - 1u].name;
    snprintf(buf, len, "%s", name);
    if (buf[0] == '\0') {
        /* TR: the name of a device (a keypad or a module) nobody has named; {0} is its number */
        I18N_FORMAT(buf, len, TR_C("default device name", "Device {0}"), (unsigned)d);
    }
    return buf;
}

static void open_device(void *ctx)
{
    ui_page_open(PAGE_DEVICE, (int32_t)(intptr_t)ctx);
}

static void build_devices(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("A supervised device that stops answering shows a trouble"));
    /* The keypads on Wi-Fi on top, in a firmware with the panel link (BL-048); the numbers they
     * hold are left out of the table under them (LINK-001). */
    uint16_t held = 0;
    ui_link_devices_section(body, &held);
    for (uint8_t d = 1; d <= ALARM_MAX_DEVICES; d++) {
        if ((held & (1u << (d - 1u))) != 0u) {
            continue;
        }
        const alarm_device_config_t *dc = &S.edit->devices[d - 1u];
        char name[ALARM_NAME_LEN + 16], detail[64], part[ALARM_NAME_LEN + 16];
        if (dc->enabled) {
            i18n_utf8_copy(detail, sizeof(detail), dc->partition == 0u
                                                       ? TR("All partitions")
                                                       : partition_label(dc->partition, part,
                                                                         sizeof(part)));
        } else {
            i18n_utf8_copy(detail, sizeof(detail), TR("Not in use"));
        }
        ui_row_link(body, UI_ICON_CPU, dc->enabled ? ui_tone_blue() : ui_tone_gray(),
                    device_label(d, name, sizeof(name)), detail, NULL, UI_ICON_CHEVRON_RIGHT,
                    open_device, (void *)(intptr_t)d);
    }
}

static void on_device_used(bool on, void *ctx)
{
    (void)ctx;
    S.edit->devices[s_device - 1u].enabled = on;
    dirty();
    ui_page_rebuild();
}

static void device_renamed(const char *text)
{
    alarm_device_config_t *d = &S.edit->devices[s_device - 1u];
    memset(d->name, 0, ALARM_NAME_LEN);
    i18n_utf8_copy(d->name, ALARM_NAME_LEN, text);
    dirty();
    ui_page_rebuild();
}

static void rename_device(void *ctx)
{
    (void)ctx;
    ui_text_sheet(TR("Device name"), TR("Hall keypad, Garage expander..."),
                  S.edit->devices[s_device - 1u].name, false, ALARM_NAME_LEN - 1u, device_renamed);
}

/* The partitions to choose from, in pick-sheet order: [0] = the first choice. all: "All
 * partitions" first (0). Returns how many. */
static size_t partition_choices(ui_choice_t *choices, uint8_t *numbers, bool all, const char **names,
                                char (*bufs)[ALARM_NAME_LEN + 16])
{
    size_t n = 0;
    if (all) {
        choices[n].glyph = UI_ICON_WAREHOUSE;
        choices[n].tone = ui_tone_violet();
        choices[n].title = TR("All partitions");
        choices[n].detail = NULL;
        numbers[n++] = 0;
    }
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!S.edit->partitions[p - 1u].enabled) {
            continue;
        }
        names[p - 1u] = partition_label(p, bufs[p - 1u], sizeof(bufs[0]));
        choices[n].glyph = UI_ICON_WAREHOUSE;
        choices[n].tone = ui_tone_violet();
        choices[n].title = names[p - 1u];
        choices[n].detail = NULL;
        numbers[n++] = p;
    }
    return n;
}

static ui_choice_t s_part_choices[ALARM_MAX_PARTITIONS + 1];
static uint8_t s_part_numbers[ALARM_MAX_PARTITIONS + 1];
static const char *s_part_names[ALARM_MAX_PARTITIONS];
static char s_part_bufs[ALARM_MAX_PARTITIONS][ALARM_NAME_LEN + 16];

static void device_partition_chosen(int32_t index)
{
    if (index >= 0 && index <= ALARM_MAX_PARTITIONS) {
        S.edit->devices[s_device - 1u].partition = s_part_numbers[index];
        dirty();
    }
    ui_page_rebuild();
}

static void pick_device_partition(void *ctx)
{
    (void)ctx;
    const size_t n = partition_choices(s_part_choices, s_part_numbers, true, s_part_names, s_part_bufs);
    int32_t selected = 0;
    for (size_t i = 0; i < n; i++) {
        if (s_part_numbers[i] == S.edit->devices[s_device - 1u].partition) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Home partition"), TR("The partition a keypad shows and arms"), s_part_choices,
                  n,
                  selected, device_partition_chosen);
}

static void supervision_row(lv_obj_t *body, uint16_t zone, uint8_t device);

static void build_device(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    s_device = (uint8_t)arg;
    if (s_device < 1u || s_device > ALARM_MAX_DEVICES) {
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Device"));
        return;
    }
    const alarm_device_config_t *d = &S.edit->devices[s_device - 1u];
    char name[ALARM_NAME_LEN + 16], part[ALARM_NAME_LEN + 16];
    i18n_utf8_copy(title, tlen, device_label(s_device, name, sizeof(name)));
    /* TR: the line under a device's title; {0} is its number. At most 100 bytes */
    I18N_FORMAT(note, nlen, TR("Device {0} · saved when you go back"), (unsigned)s_device);
    ui_row_toggle(body, TR("In use"), TR("Supervised: the panel expects it to answer"), d->enabled,
                  on_device_used, NULL);
    if (!d->enabled) {
        return;
    }
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL, d->name, UI_ICON_CHEVRON_RIGHT,
                rename_device, NULL);
    ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR("Home partition"), NULL,
                d->partition == 0u ? TR("All partitions") : partition_label(d->partition, part, sizeof(part)),
                UI_ICON_CHEVRON_RIGHT, pick_device_partition, NULL);
    supervision_row(body, 0u, s_device); /* a picker, as a zone's (BL-131, n-zone-sensor) */
}

/* ---- zones --------------------------------------------------------------------------------- */

static bool zone_used(uint16_t z)
{
    return S.edit->zones[z - 1u].type != ALARM_ZT_NOT_USED;
}

static void open_zone(void *ctx)
{
    ui_page_open(PAGE_ZONE, (int32_t)(intptr_t)ctx);
}

void ui_installer_create_zone_for(uint16_t z, alarm_device_class_t cls)
{
    if (S.edit == NULL || z < 1u || z > ALARM_MAX_ZONES) {
        return;
    }
    alarm_zone_config_defaults(&S.edit->zones[z - 1u], z, zsrc_suggest_zone_type(cls),
                               S.opt.partition);
    S.edit->zones[z - 1u].device_class = (uint8_t)cls;
    /* TR: the name of a zone nobody has named; {0} is its number */
    I18N_FORMAT(S.edit->zones[z - 1u].name, ALARM_NAME_LEN,
                TR_C("default zone name", "Zone {0}"), (unsigned)z);
    dirty();
}

void ui_installer_create_zone(uint16_t z)
{
    ui_installer_create_zone_for(z, ALARM_DEVCLASS_DOOR);
}

static void add_zone(void *ctx)
{
    (void)ctx;
    uint16_t z = 0;
    for (uint16_t i = 1; i <= ALARM_MAX_ZONES && z == 0u; i++) {
        if (!zone_used(i)) {
            z = i;
        }
    }
    if (z == 0u) {
        ui_toast(TR("Every zone is in use"));
        return;
    }
    if (ui_sources_available()) {
        ui_sources_pick(z, true); /* the zone is made once its sensor is picked */
        return;
    }
    ui_installer_create_zone(z);
    ui_page_open(PAGE_ZONE, z);
}

static void build_zones(lv_obj_t *body, char *note, size_t nlen)
{
    unsigned n = 0;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        n += zone_used(z);
    }
    /* TR: Installer > Zones, the line under the title; {0} is how many zones are set up, {1} how
     * many the panel has. At most 127 bytes */
    I18N_FORMAT(note, nlen, TRN("{0} of {1} zones in use", "{0} of {1} zones in use", n), n,
                (unsigned)ALARM_MAX_ZONES);
    ui_row_action(body, UI_ICON_PLUS, TR("Add a zone"), ui_tone_blue(), add_zone, NULL);
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (!zone_used(z)) {
            continue;
        }
        const alarm_zone_config_t *zc = &S.edit->zones[z - 1u];
        char detail[144];
        char what[112];
        char source[48];
        const char *where = ui_sources_short(z, source, sizeof(source));
        snprintf(what, sizeof(what), "%s%s%s", ui_zone_type_name(zc->type),
                 where != NULL ? " · " : "", where != NULL ? where : "");
        /* TR: Installer > Zones, a row's line: the zone's number {0}, then its type and where its
         * sensor is {1}. At most 29 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("Zone {0} · {1}"), (unsigned)z, what);
        char name[ALARM_NAME_LEN + 8];
        ui_row_link(body, ui_device_class_glyph((alarm_device_class_t)zc->device_class),
                    ui_tone_green(), ui_zone_name(S.edit, z, name, sizeof(name)), detail, NULL,
                    UI_ICON_CHEVRON_RIGHT, open_zone, (void *)(intptr_t)z);
    }
}

static alarm_zone_config_t *zone_cfg(void)
{
    return &S.edit->zones[s_zone - 1u];
}

/* The zone as saved (NULL: not saved yet), beside its draft: what its spoken name's approval is
 * for. */
static const alarm_zone_config_t *zone_stored(uint16_t zone)
{
    if (S.cfg == NULL || zone < 1u || zone > ALARM_MAX_ZONES ||
        S.cfg->zones[zone - 1u].type == ALARM_ZT_NOT_USED) {
        return NULL;
    }
    return &S.cfg->zones[zone - 1u];
}

static void zone_renamed(const char *text)
{
    if (text[0] == '\0') {
        ui_toast(TR("A name can't be empty"));
        return;
    }
    char was[ALARM_NAME_LEN];
    memcpy(was, zone_cfg()->name, sizeof(was));
    memset(zone_cfg()->name, 0, ALARM_NAME_LEN);
    i18n_utf8_copy(zone_cfg()->name, ALARM_NAME_LEN, text);
    if (memcmp(was, zone_cfg()->name, sizeof(was)) != 0 && s_voice_zone == s_zone) {
        s_voice_zone = 0; /* a new name: its words are looked at again (D01) */
    }
    dirty();
    ui_page_rebuild();
}

static void rename_zone(void *ctx)
{
    (void)ctx;
    ui_text_sheet(TR("Zone name"), TR("Say where it is: Front Door, Kitchen Window"), zone_cfg()->name,
                  false, ALARM_NAME_LEN - 1u, zone_renamed);
}

static void type_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= UI_ZONE_TYPE_COUNT) {
        return;
    }
    alarm_zone_config_t *zc = zone_cfg();
    const alarm_zone_type_t t = UI_ZONE_TYPES[index].type;
    if (t != zc->type) {
        /* New defaults for the type (chime, sensor kind), keeping the name and partition, and
         * the spoken name with its revisions: the same zone keeps them (docs/features/08 D01,
         * CORE-CFG-021). */
        const alarm_zone_config_t kept = *zc;
        alarm_zone_config_defaults(zc, s_zone, t, kept.partition);
        memcpy(zc->name, kept.name, sizeof(zc->name));
        memcpy(zc->spoken_words, kept.spoken_words, sizeof(zc->spoken_words));
        zc->voice_binding_revision = kept.voice_binding_revision;
        zc->voice_approved_revision = kept.voice_approved_revision;
        zc->voice_pack_revision = kept.voice_pack_revision;
        const bool button = t == ALARM_ZT_ARM_STAY || t == ALARM_ZT_ARM_AWAY ||
                            t == ALARM_ZT_DISARM || t == ALARM_ZT_KEYSWITCH;
        if (button) {
            zc->user = ALARM_USER_MASTER; /* buttons act as a user; the master by default */
        }
        dirty();
    }
    ui_page_rebuild();
}

static void pick_type(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[32];
    int32_t selected = -1;
    for (size_t i = 0; i < UI_ZONE_TYPE_COUNT && i < 32u; i++) {
        choices[i].glyph = NULL;
        choices[i].tone = ui_tone_blue();
        choices[i].title = TR_C("zone type", UI_ZONE_TYPES[i].name);
        choices[i].detail = TR(UI_ZONE_TYPES[i].detail);
        if (UI_ZONE_TYPES[i].type == zone_cfg()->type) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Zone type"), TR("How the panel reacts when this sensor trips"), choices,
                  UI_ZONE_TYPE_COUNT, selected, type_chosen);
}

static void class_chosen(int32_t index)
{
    if (index >= 0 && index < UI_DEVICE_CLASS_COUNT) {
        zone_cfg()->device_class = (uint8_t)index;
        dirty();
    }
    ui_page_rebuild();
}

static void pick_class(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[UI_DEVICE_CLASS_COUNT];
    for (int i = 0; i < UI_DEVICE_CLASS_COUNT; i++) {
        choices[i].glyph = ui_device_class_glyph((alarm_device_class_t)i);
        choices[i].tone = ui_tone_green();
        choices[i].title = ui_device_class_name((alarm_device_class_t)i);
        choices[i].detail = NULL;
    }
    ui_pick_sheet(TR("Sensor"), TR("Sets the icon and the words for open and closed"), choices,
                  UI_DEVICE_CLASS_COUNT, zone_cfg()->device_class, class_chosen);
}

/* "Report this zone" says what its switch's state does (BL-131 finding 23). */
static lv_obj_t *s_report_row;

static const char *report_words(bool on)
{
    return on ? TR("Its events go to phones and integrations, not only the event log")
              : TR("Its events stay in the panel's event log only");
}

typedef enum { ZFLAG_CHIME, ZFLAG_NIGHT, ZFLAG_BYPASS, ZFLAG_REPORT, ZFLAG_SWINGER_EXEMPT } zone_flag_t;

static void on_zone_flag(bool on, void *ctx)
{
    alarm_zone_config_t *zc = zone_cfg();
    uint16_t bit = 0;
    bool set = on;
    switch ((zone_flag_t)(intptr_t)ctx) {
    case ZFLAG_CHIME: bit = ALARM_ZF_CHIME; break;
    case ZFLAG_NIGHT: bit = ALARM_ZF_NIGHT; break;
    case ZFLAG_BYPASS: bit = ALARM_ZF_BYPASSABLE; break;
    case ZFLAG_REPORT:
        bit = ALARM_ZF_NO_REPORT;
        set = !on;
        break;
    case ZFLAG_SWINGER_EXEMPT: bit = ALARM_ZF_SWINGER_EXEMPT; break;
    }
    zc->flags = set ? (uint16_t)(zc->flags | bit) : (uint16_t)(zc->flags & (uint16_t)~bit);
    dirty();
    if ((zone_flag_t)(intptr_t)ctx == ZFLAG_REPORT && s_report_row != NULL) {
        ui_set_label(s_report_row, "detail_label", report_words(on));
    }
}

static void zone_partition_chosen(int32_t index)
{
    if (index >= 0 && index < ALARM_MAX_PARTITIONS && s_part_numbers[index] != 0u) {
        if (zone_cfg()->partition != s_part_numbers[index] && s_voice_zone == s_zone) {
            s_voice_zone = 0; /* another partition: its words are looked at again (D01) */
        }
        zone_cfg()->partition = s_part_numbers[index];
        dirty();
    }
    ui_page_rebuild();
}

static void pick_zone_partition(void *ctx)
{
    (void)ctx;
    const size_t n = partition_choices(s_part_choices, s_part_numbers, false, s_part_names, s_part_bufs);
    int32_t selected = -1;
    for (size_t i = 0; i < n; i++) {
        if (s_part_numbers[i] == zone_cfg()->partition) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Partition"), TR("The area this zone belongs to"), s_part_choices, n, selected,
                  zone_partition_chosen);
}

/* The long pick lists (every user, every zone): one block, allocated on first use in slow
 * memory, so a board that never opens them never pays for them (the 4" has no PSRAM). */
#define PICK_MAX ((ALARM_MAX_ZONES > ALARM_MAX_USERS ? ALARM_MAX_ZONES : ALARM_MAX_USERS) + 1)
typedef struct {
    ui_choice_t choices[PICK_MAX];
    uint16_t numbers[PICK_MAX];
    char names[PICK_MAX][ALARM_NAME_LEN + 12];
} pick_list_t;
static pick_list_t *s_list;

static pick_list_t *pick_list(void)
{
    if (s_list == NULL) {
        s_list = ui_slow_alloc(sizeof(*s_list));
    }
    return s_list;
}

static void zone_user_chosen(int32_t index)
{
    if (s_list != NULL && index >= 0 && index < PICK_MAX && s_list->numbers[index] != 0u) {
        zone_cfg()->user = (uint8_t)s_list->numbers[index];
        dirty();
    }
    ui_page_rebuild();
}

static void pick_zone_user(void *ctx)
{
    (void)ctx;
    pick_list_t *L = pick_list();
    if (L == NULL) {
        return;
    }
    size_t n = 0;
    int32_t selected = -1;
    for (uint8_t u = 1; u <= ALARM_MAX_USERS; u++) {
        const alarm_user_t *usr = &S.cfg->users[u - 1u];
        if (usr->authority == ALARM_AUTH_NONE || usr->authority == ALARM_AUTH_DURESS) {
            continue; /* a button never acts as a duress code */
        }
        ui_user_name(S.cfg, u, L->names[n], sizeof(L->names[0]));
        L->choices[n].glyph = UI_ICON_USER;
        L->choices[n].tone = ui_authority_tone(usr->authority);
        L->choices[n].title = L->names[n];
        L->choices[n].detail = ui_authority_name(usr->authority);
        if (u == zone_cfg()->user) {
            selected = (int32_t)n;
        }
        L->numbers[n++] = u;
    }
    ui_pick_sheet(TR("Acts as"), TR("The button arms and disarms with this user's rights and partitions"),
                  L->choices, n, selected, zone_user_chosen);
}

static void cross_chosen(int32_t index)
{
    if (s_list != NULL && index >= 0 && index < PICK_MAX) {
        zone_cfg()->cross_zone = s_list->numbers[index];
        dirty();
    }
    ui_page_rebuild();
}

static void pick_cross(void *ctx)
{
    (void)ctx;
    pick_list_t *L = pick_list();
    if (L == NULL) {
        return;
    }
    size_t n = 0;
    int32_t selected = 0;
    L->choices[n].glyph = NULL;
    L->choices[n].tone = ui_tone_gray();
    L->choices[n].title = TR_C("cross-zone partner", "None");
    L->choices[n].detail = TR("An alarm from this zone alone");
    L->numbers[n++] = 0;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (z == s_zone || !zone_used(z)) {
            continue;
        }
        char name[ALARM_NAME_LEN + 8];
        snprintf(L->names[n], sizeof(L->names[0]), "%u · %s", (unsigned)z,
                 ui_zone_name(S.edit, z, name, sizeof(name)));
        L->choices[n].glyph = NULL;
        L->choices[n].tone = ui_tone_green();
        L->choices[n].title = L->names[n];
        L->choices[n].detail = NULL;
        if (z == zone_cfg()->cross_zone) {
            selected = (int32_t)n;
        }
        L->numbers[n++] = z;
    }
    ui_pick_sheet(TR("Cross-zone partner"), TR("Both must trip close together to cause an alarm"),
                  L->choices, n, selected, cross_chosen);
}

/* "Alarm limit": one picker for what were two rows, the stepper (swinger_trips, 0 = the system
 * setting) and the "Never shut down" switch (ALARM_ZF_SWINGER_EXEMPT). The stored fields and what
 * the engine does with them are unchanged: Never sets the flag and leaves the count as it was (the
 * flag wins, alarms.c); a count or the system setting clears it. Choices: system, 1..6, Never. */
#define LIMIT_MAX     6u
#define LIMIT_NEVER   (LIMIT_MAX + 1u)
static char s_limit_titles[LIMIT_MAX][24];

static void limit_words(const alarm_zone_config_t *zc, char *out, size_t len)
{
    if ((zc->flags & ALARM_ZF_SWINGER_EXEMPT) != 0u) {
        /* TR: a zone's Alarm limit when it keeps sounding however often it trips. At most 30 bytes */
        i18n_utf8_copy(out, len, TR_C("alarm limit", "Never"));
    } else if (zc->swinger_trips == 0u) {
        i18n_utf8_copy(out, len, TR("System setting"));
    } else {
        /* TR: a count in a number row, for example how many times a siren may sound; {0} is the
         * number. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} time", "{0} times", zc->swinger_trips),
                    (int)zc->swinger_trips);
    }
}

static void limit_chosen(int32_t index)
{
    alarm_zone_config_t *zc = zone_cfg();
    if (index == (int32_t)LIMIT_NEVER) {
        zc->flags = (uint16_t)(zc->flags | ALARM_ZF_SWINGER_EXEMPT);
        dirty();
    } else if (index >= 0 && index <= (int32_t)LIMIT_MAX) {
        zc->flags = (uint16_t)(zc->flags & (uint16_t)~ALARM_ZF_SWINGER_EXEMPT);
        zc->swinger_trips = (uint8_t)index;
        dirty();
    }
    ui_page_rebuild();
}

static void pick_limit(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[LIMIT_NEVER + 1u];
    choices[0] = (ui_choice_t){NULL, ui_tone_blue(), TR("System setting"),
                               TR("As Delays and arming sets it for every zone")};
    for (unsigned n = 1; n <= LIMIT_MAX; n++) {
        /* TR: a count in a number row, for example how many times a siren may sound; {0} is the
         * number. At most 20 bytes */
        I18N_FORMAT(s_limit_titles[n - 1u], sizeof(s_limit_titles[0]),
                    TRN("{0} time", "{0} times", n), (int)n);
        choices[n] = (ui_choice_t){NULL, ui_tone_blue(), s_limit_titles[n - 1u], NULL};
    }
    choices[LIMIT_NEVER] = (ui_choice_t){NULL, ui_tone_blue(), TR_C("alarm limit", "Never"),
                                         TR("Keeps sounding however often it trips")};
    const alarm_zone_config_t *zc = zone_cfg();
    const int32_t selected = (zc->flags & ALARM_ZF_SWINGER_EXEMPT) != 0u ? (int32_t)LIMIT_NEVER
                             : zc->swinger_trips <= LIMIT_MAX       ? (int32_t)zc->swinger_trips
                                                                    : -1;
    ui_pick_sheet(TR("Alarm limit"), TR("Stops sounding after this many alarms in one arming"),
                  choices, LIMIT_NEVER + 1u, selected, limit_chosen);
}

/* "Supervision": how long the sensor itself may stay silent (supervision_s, 0 = not supervised).
 * Choices in hours; a value set another way (the Panel Manager, the stepper before) stays offered
 * as it is. The field holds at most 18 hours (uint16_t seconds). */
static const uint16_t SUPERVISION_S[] = {0u, 3600u, 14400u, 43200u};
#define SUPERVISION_N (sizeof(SUPERVISION_S) / sizeof(SUPERVISION_S[0]))
static uint16_t s_sup_values[SUPERVISION_N + 1u];
static char s_sup_titles[SUPERVISION_N + 1u][24];
static uint16_t s_sup_zone;  /* the zone the list sets, 0 when it is a device's */
static uint8_t s_sup_device; /* the device (a keypad, a receiver) the list sets, 0 for a zone */

/* The field a Supervision row sets: a device's, or else a zone's; NULL when neither exists. */
static uint16_t *sup_field(uint16_t zone, uint8_t device)
{
    if (S.edit == NULL) {
        return NULL;
    }
    if (device >= 1u && device <= ALARM_MAX_DEVICES) {
        return &S.edit->devices[device - 1u].supervision_s;
    }
    if (zone >= 1u && zone <= ALARM_MAX_ZONES) {
        return &S.edit->zones[zone - 1u].supervision_s;
    }
    return NULL;
}

static void supervision_words(uint16_t s, char *out, size_t len)
{
    if (s == 0u) {
        i18n_utf8_copy(out, len, TR("Not supervised"));
    } else if (s % 3600u == 0u) {
        /* TR: a sensor's supervision time in hours; {0} is the number. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} hour", "{0} hours", (uint32_t)(s / 3600u)),
                    (unsigned)(s / 3600u));
    } else {
        /* TR: a number of minutes in a number row; {0} is the number. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} min", "{0} min", (uint32_t)(s / 60u)), (unsigned)(s / 60u));
    }
}

static void supervision_chosen(int32_t index)
{
    uint16_t *field = sup_field(s_sup_zone, s_sup_device);
    if (field != NULL && index >= 0 && (size_t)index <= SUPERVISION_N) {
        *field = s_sup_values[index];
        dirty();
    }
    ui_page_rebuild();
}

/* ctx: the zone in its low 16 bits, or the device above them. */
static void pick_supervision(void *ctx)
{
    s_sup_zone = (uint16_t)((uintptr_t)ctx & 0xFFFFu);
    s_sup_device = (uint8_t)(((uintptr_t)ctx >> 16) & 0xFFu);
    const uint16_t *field = sup_field(s_sup_zone, s_sup_device);
    if (field == NULL) {
        return;
    }
    const uint16_t now = *field;
    bool listed = false;
    for (size_t i = 0; i < SUPERVISION_N; i++) {
        listed = listed || SUPERVISION_S[i] == now;
    }
    /* The choices in order, today's value in its place when it isn't one of them. */
    size_t n = 0;
    for (size_t i = 0; i <= SUPERVISION_N; i++) {
        if (!listed && (i == SUPERVISION_N || now < SUPERVISION_S[i])) {
            s_sup_values[n++] = now;
            listed = true;
        }
        if (i < SUPERVISION_N) {
            s_sup_values[n++] = SUPERVISION_S[i];
        }
    }
    static ui_choice_t choices[SUPERVISION_N + 1u];
    int32_t selected = -1;
    for (size_t i = 0; i < n; i++) {
        supervision_words(s_sup_values[i], s_sup_titles[i], sizeof(s_sup_titles[0]));
        choices[i] = (ui_choice_t){NULL, ui_tone_blue(), s_sup_titles[i], NULL};
        if (s_sup_values[i] == now) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Supervision"),
                  s_sup_device != 0u ? TR("Silent this long: a trouble")
                                     : TR("Shows Check when it misses a check-in"),
                  choices, n, selected, supervision_chosen);
}

/* A zone's or a device's Supervision row (device 0: the zone's). */
static void supervision_row(lv_obj_t *body, uint16_t zone, uint8_t device)
{
    const uint16_t *field = sup_field(zone, device);
    if (field == NULL) {
        return;
    }
    char value[32];
    supervision_words(*field, value, sizeof(value));
    ui_row_link(body, UI_ICON_CLOCK_ALERT, ui_tone_blue(), TR("Supervision"),
                device != 0u ? TR("Silent this long: a trouble")
                             : TR("Shows Check when it misses a check-in"),
                value, UI_ICON_CHEVRON_RIGHT, pick_supervision,
                (void *)(uintptr_t)(device != 0u ? (uint32_t)device << 16 : (uint32_t)zone));
}

void ui_installer_supervision_row(lv_obj_t *body, uint16_t zone)
{
    supervision_row(body, zone, 0u);
}

const alarm_zone_config_t *ui_installer_zone_draft(uint16_t zone)
{
    if (!S.program || S.edit == NULL || zone < 1u || zone > ALARM_MAX_ZONES ||
        S.edit->zones[zone - 1u].type == ALARM_ZT_NOT_USED) {
        return NULL;
    }
    return &S.edit->zones[zone - 1u];
}

void ui_installer_spoken_done(uint16_t zone, const uint16_t *ids, size_t n,
                              const ui_voice_seen_t *seen)
{
    alarm_zone_config_t *zc = (alarm_zone_config_t *)ui_installer_zone_draft(zone);
    if (zc == NULL || n > ALARM_ZONE_WORDS) {
        return;
    }
    bool changed = false;
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        const uint16_t w = i < n ? ids[i] : 0u;
        if (alarm_zone_word(zc, i) != w) {
            alarm_zone_set_word(zc, i, w);
            changed = true;
        }
    }
    if (changed) {
        dirty();
    }
    if (seen != NULL && n > 0u) {
        s_voice_zone = zone; /* with the save, as the page is left */
        s_voice_seen = *seen;
    } else if (s_voice_zone == zone) {
        s_voice_zone = 0;
    }
    ui_page_rebuild(); /* the row says what Done did (no toast, D03) */
}

static void remove_confirmed(void)
{
    if (!ui_sources_zone_removed(s_zone)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Disarm first")); /* its sensor can't be let go while anything is armed */
        return;
    }
    memset(zone_cfg(), 0, sizeof(alarm_zone_config_t));
    dirty();
    ui_page_back();
}

static void remove_zone(void *ctx)
{
    (void)ctx;
    char title[64];
    char name[ALARM_NAME_LEN + 8];
    /* TR: a confirmation's title; {0} is the zone's name (cut to 31 bytes). At most 30 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Remove {0}?"), ui_zone_name(S.edit, s_zone, name, sizeof(name)));
    ui_confirm(title, TR("The panel stops watching this sensor. Its history stays in the event log."),
               TR("Remove"), remove_confirmed);
}

static void build_zone(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                       size_t nlen)
{
    s_zone = (uint16_t)arg;
    if (s_zone < 1u || s_zone > ALARM_MAX_ZONES || !zone_used(s_zone)) {
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Zone"));
        return;
    }
    const alarm_zone_config_t *zc = zone_cfg();
    char name[ALARM_NAME_LEN + 8];
    i18n_utf8_copy(title, tlen, ui_zone_name(S.edit, s_zone, name, sizeof(name)));
    /* TR: the line under a zone's title; {0} is its number. At most 100 bytes */
    I18N_FORMAT(note, nlen, TR("Zone {0} · saved when you go back"), (unsigned)s_zone);
    char type[48];
    char code[8];
    snprintf(code, sizeof(code), "%02u", (unsigned)zc->type);
    /* TR: a zone's type and its number, on the zone page's Type row: {0} the type, {1} the number
     * (two digits). At most 47 bytes */
    I18N_FORMAT(type, sizeof(type), TR_C("zone type and number", "{0} ({1})"),
                ui_zone_type_name(zc->type), code);
    /* BL-131 n-zone-edit: how the zone is right now on top, then four groups, Remove apart. */
    s_report_row = NULL;
    ui_sources_zone_status(body, s_zone);

    ui_section(body, TR("The zone"));
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL, zc->name, UI_ICON_CHEVRON_RIGHT,
                rename_zone, NULL);
    /* the words the panel says for it, right after its name (BL-202, D03) */
    ui_voice_zone_row(body, s_zone, zc, zone_stored(s_zone), s_voice_zone == s_zone);
    ui_row_link(body, UI_ICON_SHIELD, ui_tone_blue(), TR("Type"), NULL, type, UI_ICON_CHEVRON_RIGHT,
                pick_type, NULL);
    char value[ALARM_NAME_LEN + 16];
    if (partitions_used() > 1u || zc->partition != S.opt.partition) {
        ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR("Partition"), NULL,
                    partition_label(zc->partition, value, sizeof(value)), UI_ICON_CHEVRON_RIGHT,
                    pick_zone_partition, NULL);
    }
    const bool button = zc->type == ALARM_ZT_ARM_STAY || zc->type == ALARM_ZT_ARM_AWAY ||
                        zc->type == ALARM_ZT_DISARM || zc->type == ALARM_ZT_KEYSWITCH;
    if (button) {
        ui_row_link(body, UI_ICON_USER, ui_tone_blue(), TR("Acts as"),
                    TR("The user whose rights this button has"), ui_user_name(S.cfg, zc->user, value, sizeof(value)),
                    UI_ICON_CHEVRON_RIGHT, pick_zone_user, NULL);
    }

    ui_section(body, TR("Its sensor"));
    ui_row_link(body, ui_device_class_glyph((alarm_device_class_t)zc->device_class),
                ui_tone_green(), TR("Sensor"), NULL,
                ui_device_class_name((alarm_device_class_t)zc->device_class), UI_ICON_CHEVRON_RIGHT,
                pick_class, NULL);
    ui_sources_zone_rows(body, s_zone);
    if (!ui_sources_available()) {
        /* no Sensor settings page without zone sources: supervision stays here */
        ui_installer_supervision_row(body, s_zone);
    }

    ui_section(body, TR("What it does"));
    const bool burglary = alarm_zone_type_is_burglary(zc->type);
    const bool interior = zc->type == ALARM_ZT_INTERIOR_FOLLOWER ||
                          zc->type == ALARM_ZT_INTERIOR_DELAY || zc->type == ALARM_ZT_DAY_NIGHT;
    if (interior) {
        ui_row_toggle(body, TR("Night zone"), TR("Armed in Night too, not only Away"),
                      (zc->flags & ALARM_ZF_NIGHT) != 0u, on_zone_flag,
                      (void *)(intptr_t)ZFLAG_NIGHT);
    }
    if (burglary || zc->type == ALARM_ZT_NO_RESPONSE || zc->type == ALARM_ZT_MONITOR) {
        ui_row_toggle(body, TR("Chime"), TR("Ding when it opens while disarmed and chime is on"),
                      (zc->flags & ALARM_ZF_CHIME) != 0u, on_zone_flag,
                      (void *)(intptr_t)ZFLAG_CHIME);
    }
    if (alarm_zone_type_is_24h(zc->type)) {
        ui_row_toggle(body, TR("Can be bypassed"), TR("24-hour zones normally can't be"),
                      (zc->flags & ALARM_ZF_BYPASSABLE) != 0u, on_zone_flag,
                      (void *)(intptr_t)ZFLAG_BYPASS);
    }
    const bool report = (zc->flags & ALARM_ZF_NO_REPORT) == 0u;
    s_report_row = ui_row_toggle(body, TR("Report this zone"), report_words(report), report,
                                 on_zone_flag, (void *)(intptr_t)ZFLAG_REPORT);

    ui_section(body, TR("Advanced"));
    char partner[ALARM_NAME_LEN + 16];
    i18n_utf8_copy(partner, sizeof(partner), TR_C("cross-zone partner", "None"));
    if (zc->cross_zone != 0u) {
        char name[ALARM_NAME_LEN + 8];
        snprintf(partner, sizeof(partner), "%u · %s", (unsigned)zc->cross_zone,
                 ui_zone_name(S.edit, zc->cross_zone, name, sizeof(name)));
    }
    ui_row_link(body, UI_ICON_SHIELD, ui_tone_blue(), TR("Cross-zone partner"),
                TR("Both must trip close together to cause an alarm"),
                partner, UI_ICON_CHEVRON_RIGHT, pick_cross, NULL);
    char limit[32];
    limit_words(zc, limit, sizeof(limit));
    ui_row_link(body, UI_ICON_SIREN, ui_tone_blue(), TR("Alarm limit"),
                TR("Stops sounding after this many alarms in one arming"), limit,
                UI_ICON_CHEVRON_RIGHT, pick_limit, NULL);
    ui_section(body, ""); /* apart */
    ui_row_action(body, UI_ICON_TRASH, TR("Remove this zone"), ui_tone_red(), remove_zone, NULL);
}

/* ---- system name ----------------------------------------------------------------------------- */

static void name_changed(const char *text)
{
    if (text[0] == '\0') {
        ui_toast(TR("A name can't be empty"));
        return;
    }
    alarm_partition_config_t *p = &S.edit->partitions[part_index()];
    memset(p->name, 0, ALARM_NAME_LEN);
    i18n_utf8_copy(p->name, ALARM_NAME_LEN, text);
    dirty();
    ui_installer_apply();
    ui_page_rebuild();
}

static void rename_system(void *ctx)
{
    (void)ctx;
    ui_text_sheet(TR("System name"), TR("Home, Cabin, Shop..."), S.edit->partitions[part_index()].name,
                  false, ALARM_NAME_LEN - 1u, name_changed);
}

static void build_partition(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("The name of what this panel protects"));
    ui_row_link(body, UI_ICON_HOUSE, ui_tone_violet(), TR("Name"), NULL,
                S.edit->partitions[part_index()].name, UI_ICON_CHEVRON_RIGHT, rename_system, NULL);
    ui_paragraph(body, TR("Shown on the screen saver and in the event log."), ui_tone_gray());
}

/* ---- installer and master codes ------------------------------------------------------------- */

static uint8_t s_code_user;

static bool new_code(const char *code)
{
    const alarm_result_t r = S.be.program_set_code(S.be.ctx, s_code_user, code);
    if (r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(ui_result_text(r));
        return false;
    }
    ui_pad_cancel();
    ui_refresh(true);
    ui_toast(s_code_user == ALARM_USER_INSTALLER ? TR("Installer code changed")
                                                 : TR("Master code changed"));
    ui_page_rebuild();
    return true;
}

static void change_code(void *ctx)
{
    s_code_user = (uint8_t)(uintptr_t)ctx;
    ui_pad_ask_new(HOST_SETTINGS, UI_ICON_KEY_ROUND,
                   s_code_user == ALARM_USER_INSTALLER ? TR("New installer code") : TR("New master code"),
                   new_code);
}

static void build_codes(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("The two codes that can't be deleted"));
    const bool inst_default = (S.snap.default_codes & ALARM_DEFAULT_CODE_INSTALLER) != 0u;
    const bool master_default = (S.snap.default_codes & ALARM_DEFAULT_CODE_MASTER) != 0u;
    char inst_text[48], master_text[48];
    /* TR: Installer > codes, under a code row that is still the factory code (no digits on
     * the screen: the factory codes are public, BL-187). At most 40 bytes */
    const char *still = TR("Still the factory code");
    /* TR: Installer > codes, under a code row that is no longer the factory code */
    const char *changed = TR_C("code state", "Changed");
    snprintf(inst_text, sizeof(inst_text), "%s", inst_default ? still : changed);
    snprintf(master_text, sizeof(master_text), "%s", master_default ? still : changed);
    ui_row_link(body, UI_ICON_KEY_ROUND, inst_default ? ui_tone_amber() : ui_tone_green(),
                TR("Installer code"), inst_text, NULL, UI_ICON_CHEVRON_RIGHT, change_code,
                (void *)(uintptr_t)ALARM_USER_INSTALLER);
    ui_row_link(body, UI_ICON_KEY_ROUND, master_default ? ui_tone_amber() : ui_tone_green(),
                TR("Master code"), master_text, NULL, UI_ICON_CHEVRON_RIGHT, change_code,
                (void *)(uintptr_t)ALARM_USER_MASTER);
    ui_paragraph(body,
                 TR("The installer code programs the panel; it never arms or disarms. The factory "
                    "codes are public: anyone can look them up. Everyone else's code is managed in "
                    "Settings > Users with the master code."),
                 ui_tone_gray());
}

/* ---- factory reset --------------------------------------------------------------------------- */

static void on_reset_log(bool on, void *ctx)
{
    (void)ctx;
    s_reset_log = on;
}

static void reset_now(void *ctx)
{
    (void)ctx;
    if (S.be.factory_reset == NULL) {
        ui_toast(TR("Not available here"));
        return;
    }
    ui_toast(TR("Erasing. The panel restarts with the setup"));
    S.be.factory_reset(S.be.ctx, s_reset_log);
}

static void build_reset(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under the page title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Start again from the box"));
    char erase[256];
    /* TR: Installer > Factory reset; {0} is the factory installer code and {1} the master code,
     * numbers. At most 230 bytes */
    I18N_FORMAT(erase, sizeof(erase),
                TR("Erases the zones, users and codes, Wi-Fi and every setting, then restarts "
                   "with the factory codes (installer {0}, master {1}) and the setup."),
                NOTR(ALARM_DEFAULT_INSTALLER_CODE), NOTR(ALARM_DEFAULT_MASTER_CODE));
    ui_paragraph(body, erase, ui_tone_gray());
    s_reset_log = false;
    ui_row_toggle(body, TR("Also erase the event log"), TR("Off keeps the history, with the reset in it"),
                  false, on_reset_log, NULL);
    ui_row_hold(body, TR("Hold to erase everything"), reset_now, NULL);
}

/* ---- pages ----------------------------------------------------------------------------------- */

bool ui_installer_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen,
                        char *note, size_t nlen)
{
    if (id == PAGE_INSTALLER_LOST) { /* before the code, so open without it */
        (void)note;
        (void)nlen;
        /* TR: Installer > Lost the installer code?, the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Lost the installer code?"));
        ui_paragraph(body,
                     TR("Unplug the panel for ten seconds. For two minutes after it starts, "
                        "Installer opens without the code (unless the installer turned that off)."),
                     ui_tone_gray());
        return true;
    }
    if (id == PAGE_INSTALLER) {
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Installer"));
        if (!S.program) {
            build_locked(body, note, nlen);
        } else {
            build_menu(body, note, nlen);
        }
        return true;
    }
    if (!S.program || S.edit == NULL) {
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Installer"));
        build_locked(body, note, nlen);
        return true;
    }
    switch (id) {
    case PAGE_OPTIONS:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Delays and arming"));
        build_options(body, note, nlen);
        return true;
    case PAGE_ZONES:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Zones"));
        build_zones(body, note, nlen);
        return true;
    case PAGE_ZONE: build_zone(body, arg, title, tlen, note, nlen); return true;
    case PAGE_ZONE_SOURCE: ui_sources_build_picker(body, arg, title, tlen, note, nlen); return true;
    case PAGE_ZONE_SENSOR: ui_sources_build_settings(body, arg, title, tlen, note, nlen); return true;
    case PAGE_PARTITION:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("System name"));
        build_partition(body, note, nlen);
        return true;
    case PAGE_CODES:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Installer and master codes"));
        build_codes(body, note, nlen);
        return true;
    case PAGE_RESET:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Factory reset"));
        build_reset(body, note, nlen);
        return true;
    case PAGE_PANIC_KEYS:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Panic keys"));
        build_panic_keys(body, note, nlen);
        return true;
    case PAGE_TROUBLE_BEEPS:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Trouble beeps"));
        build_trouble_beeps(body, note, nlen);
        return true;
    case PAGE_PARTITIONS:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Partitions"));
        build_partitions(body, note, nlen);
        return true;
    case PAGE_PARTITION_EDIT: build_partition_edit(body, arg, title, tlen, note, nlen); return true;
    case PAGE_DEVICES:
        /* TR: the page title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Keypads and devices"));
        build_devices(body, note, nlen);
        return true;
    case PAGE_DEVICE: build_device(body, arg, title, tlen, note, nlen); return true;
    case PAGE_LINK_ADD:
    case PAGE_LINK_KEYPAD:
        return ui_link_build(id, arg, body, title, tlen, note, nlen); /* BL-048 */
    case PAGE_FW_SERVER:
    case PAGE_FW_WARNING: return ui_fwsrv_build(id, body, title, tlen, note, nlen);
    default: return false;
    }
}
