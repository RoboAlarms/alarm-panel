/*
 * ui_words_test.c - the touchscreen's words that are not event sentences (BL-066, KPD-004).
 *
 * Three parts, against the same headless UI and simulated panel as smoke_minimal.c:
 *  1. ui_text.c directly: the tones, the glyphs of the icon table (reached through the lines it
 *     fills) and the zone card's words for each state of a zone.
 *  2. Home's headline, detail and note (apply_mode() in ui_app.c), read from the subjects the
 *     layout binds, with the panel driven into each state through host_panel.
 *  3. ui_when_text() (ui_app.c), a device's time or date (HCTL-006, BL-179), on a clock of its
 *     own in the panel's time zone (the preview's walk runs in UTC): local midnight, New Year,
 *     a time ahead of the clock, no clock, a time the C library can't convert, and the longest
 *     sentence a translation may make with it.
 * The English is compared whole where it is fixed; a time of day is compared only in part 3.
 * Exits non-zero on the first failed check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm_ui_gen.h"
#include "host_panel.h"
#include "i18n/i18n.h"
#include "lvgl.h"
#include "ui_app.h"
#include "ui_internal.h"
#include "ui_text.h"

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

static void check_str(const char *got, const char *want, const char *what)
{
    const bool ok = got != NULL && strcmp(got, want) == 0;
    s_checks++;
    if (ok) {
        printf("ok    %s: \"%s\"\n", what, want);
    } else {
        printf("FAIL  %s: wanted \"%s\", got \"%s\"\n", what, want, got != NULL ? got : "(null)");
        exit(1);
    }
}

static bool ends_with(const char *s, const char *tail)
{
    const size_t n = strlen(s);
    const size_t t = strlen(tail);
    return n >= t && strcmp(s + n - t, tail) == 0;
}

/* ---- part 1: ui_text.c ------------------------------------------------------------------- */

static uint32_t rgb(lv_color_t c)
{
    return lv_color_to_u32(c) & 0xFFFFFFu;
}

/* A glyph the icon fonts can draw: one private-use code point (U+E000..U+F8FF) in UTF-8. */
static bool glyph_ok(const char *g)
{
    return g != NULL && strlen(g) == 3u && (unsigned char)g[0] == 0xEEu;
}

static void test_tones(void)
{
    /* UI-003: the tones are the design's colours (docs/features/07). */
    check(rgb(ui_tone_green()) == 0x34D399u, "UI-003: green tone");
    check(rgb(ui_tone_amber()) == 0xFBBF24u, "UI-003: amber tone");
    check(rgb(ui_tone_blue()) == 0x60A5FAu, "UI-003: blue tone");
    check(rgb(ui_tone_violet()) == 0xA78BFAu, "UI-003: violet tone");
    check(rgb(ui_tone_rose()) == 0xFB7185u, "UI-003: rose tone");
    check(rgb(ui_tone_orange()) == 0xFB923Cu, "UI-003: orange tone");
    check(rgb(ui_tone_red()) == 0xF87171u, "UI-003: red tone");
    check(rgb(ui_tone_gray()) == 0x94A3B8u, "UI-003: gray tone");
    check(rgb(ui_tone_for_level(ALARM_LEVEL_AWAY)) == rgb(ui_tone_rose()),
          "KPD-004: Away is rose");
    check(rgb(ui_tone_for_level(ALARM_LEVEL_STAY)) == rgb(ui_tone_blue()),
          "KPD-004: Stay is blue");
    check(rgb(ui_tone_for_level(ALARM_LEVEL_NIGHT)) == rgb(ui_tone_violet()),
          "KPD-004: Night is violet");
    check(rgb(ui_tone_for_level(ALARM_LEVEL_NONE)) == rgb(ui_tone_green()),
          "KPD-004: disarmed is green");
    check(rgb(ui_authority_tone(ALARM_AUTH_DURESS)) == rgb(ui_tone_red()),
          "a duress user is red");
    check(rgb(ui_authority_tone(ALARM_AUTH_MASTER)) == rgb(ui_tone_violet()),
          "a master user is violet");
}

static void test_icon_table(void)
{
    /* Every entry of the icon table is reached through a line it fills: each event type, each
     * system and zone trouble, each sensor kind. None may come back empty or as plain text. */
    static alarm_config_t cfg;
    alarm_config_defaults(&cfg);
    unsigned lines = 0;
    for (unsigned t = 0; t < (unsigned)ALARM_EV_COUNT; t++) {
        alarm_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = (alarm_event_type_t)t;
        ev.partition = 1;
        ev.zone = 1;
        ui_line_t line;
        if (ui_event_line(&cfg, &ev, true, &line)) {
            lines++;
            if (!glyph_ok(line.glyph) || line.title[0] == '\0') {
                printf("FAIL  event type %u has no glyph or no title\n", t);
                exit(1);
            }
        }
    }
    check(lines > 40u, "KPD-004: every event type that has a line has a glyph and a title");
    {
        /* AUT-009, CORE-PROG-012: an automation that didn't lock, in the Activity list */
        alarm_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ALARM_EV_AUTOMATION_FAILED;
        ev.source.kind = ALARM_SRC_AUTOMATION;
        ev.source.id = 3u;
        ev.arg = ALARM_AUTO_FAIL_UNREACHABLE;
        ev.arg2 = ALARM_AUTO_ACT_LOCK;
        ui_line_t line;
        check(ui_event_line(&cfg, &ev, true, &line) && glyph_ok(line.glyph),
              "AUT-009: a failed automation has a line and a glyph");
        check_str(line.title, "An automation didn't lock", "AUT-009: its title");
        check_str(line.detail, "The device wasn't reachable \xC2\xB7 By automation",
                  "AUT-009: its line");
    }
    {
        /* docs/features/11 (What people see) and P2's Activity: a keypad's failed update on its
         * master, by the keypad's number, with its why in words */
        alarm_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ALARM_EV_FIRMWARE_UPDATE;
        ev.source.kind = ALARM_SRC_KEYPAD;
        ev.source.id = 2u;
        ev.arg = ALARM_FW_KEYPAD_FAILED;
        ev.arg2 = 24u; /* STALLED */
        ui_line_t line;
        check(ui_event_line(&cfg, &ev, true, &line) && glyph_ok(line.glyph),
              "a keypad's failed update has a line and a glyph");
        check_str(line.title, "Keypad 2 update failed", "a keypad's failed update: its title");
        check_str(line.detail, "No data for a minute", "a keypad's failed update: its why");
        check(rgb(line.tone) == rgb(ui_tone_amber()), "a keypad's failed update is amber");
    }
    for (unsigned t = 0; t < (unsigned)ALARM_TRBL_COUNT; t++) {
        ui_line_t line;
        ui_sys_trouble_line((alarm_sys_trouble_t)t, false, &line);
        check(glyph_ok(line.glyph) && line.title[0] != '\0', "a system trouble has glyph and words");
    }
    for (unsigned t = 0; t < (unsigned)ALARM_ZTRBL_COUNT; t++) {
        ui_line_t line;
        ui_zone_trouble_line(&cfg, 1, (alarm_zone_trouble_t)t, false, &line);
        check(glyph_ok(line.glyph) && line.title[0] != '\0', "a zone trouble has glyph and words");
    }
    for (unsigned c = 0; c < UI_DEVICE_CLASS_COUNT; c++) {
        const char *g = ui_device_class_glyph((alarm_device_class_t)c);
        check(glyph_ok(g) && ui_device_class_name((alarm_device_class_t)c)[0] != '\0',
              "a sensor kind has a glyph and a name");
    }
    for (size_t i = 0; i < UI_ZONE_TYPE_COUNT; i++) {
        check(UI_ZONE_TYPES[i].name[0] != '\0' && UI_ZONE_TYPES[i].detail[0] != '\0',
              "a zone type has a name and a line");
    }
    check_str(ui_cause_title(ALARM_CAUSE_BURGLARY), "Burglary alarm", "cause title");
    check(ui_top_cause((1u << ALARM_CAUSE_BURGLARY) | (1u << ALARM_CAUSE_FIRE)) == ALARM_CAUSE_FIRE,
          "KPD-004: fire outranks burglary in the memory");
    check(ui_top_cause(0) == ALARM_CAUSE_NONE, "an empty memory has no cause");
}

static void zone_case(alarm_device_class_t cls, uint16_t flags, uint8_t trouble,
                      const char *detail, uint32_t tone, const char *glyph, const char *what)
{
    static alarm_config_t cfg;
    alarm_config_defaults(&cfg);
    cfg.zones[0].device_class = (uint8_t)cls;
    snprintf(cfg.zones[0].name, ALARM_NAME_LEN, "%s", "Front Door");
    alarm_zone_status_t zs;
    memset(&zs, 0, sizeof(zs));
    zs.flags = flags;
    zs.trouble = trouble;
    ui_line_t line;
    ui_zone_state(&cfg, 1, &zs, &line);
    check_str(line.title, "Front Door", what);
    check_str(line.detail, detail, what);
    check(rgb(line.tone) == tone, what);
    if (glyph != NULL) {
        check(strcmp(line.glyph, glyph) == 0, what);
    }
}

static void test_zone_card(void)
{
    /* UI-003: the zone card says what a zone is doing in words and a colour, not a colour alone. */
    const uint32_t green = 0x34D399u;
    const uint32_t amber = 0xFBBF24u;
    const uint32_t red = 0xF87171u;
    const uint32_t gray = 0x94A3B8u;
    zone_case(ALARM_DEVCLASS_DOOR, 0, 0, "Closed", green, UI_ICON_DOOR_CLOSED, "UI-003: door closed");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_FAULT, 0, "Open", amber, UI_ICON_DOOR_OPEN,
              "UI-003: door open");
    zone_case(ALARM_DEVCLASS_WINDOW, 0, 0, "Closed", green, UI_ICON_APP_WINDOW, "UI-003: window");
    zone_case(ALARM_DEVCLASS_GARAGE_DOOR, ALARM_ZS_FAULT, 0, "Open", amber, UI_ICON_WAREHOUSE,
              "UI-003: garage door open");
    zone_case(ALARM_DEVCLASS_MOTION, 0, 0, "No motion", green, UI_ICON_FOOTPRINTS,
              "UI-003: motion at rest");
    zone_case(ALARM_DEVCLASS_MOTION, ALARM_ZS_FAULT, 0, "Motion", amber, UI_ICON_FOOTPRINTS,
              "UI-003: motion seen");
    zone_case(ALARM_DEVCLASS_WATER, 0, 0, "Dry", green, UI_ICON_DROPLETS, "UI-003: water dry");
    zone_case(ALARM_DEVCLASS_WATER, ALARM_ZS_FAULT, 0, "Water detected", amber, UI_ICON_DROPLETS,
              "UI-003: water detected");
    zone_case(ALARM_DEVCLASS_SMOKE, 0, 0, "Normal", green, UI_ICON_ALARM_SMOKE,
              "UI-003: smoke normal");
    zone_case(ALARM_DEVCLASS_SMOKE, ALARM_ZS_FAULT, 0, "Smoke detected", red, UI_ICON_ALARM_SMOKE,
              "UI-003: smoke is red, not amber");
    zone_case(ALARM_DEVCLASS_CO, ALARM_ZS_FAULT, 0, "CO detected", red, UI_ICON_CLOUD_FOG,
              "UI-003: CO is red");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_BYPASSED, 0, "Bypassed \xC2\xB7 Closed", gray, NULL,
              "UI-003: bypassed, closed");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_BYPASSED | ALARM_ZS_FAULT, 0, "Bypassed \xC2\xB7 Open",
              gray, NULL, "UI-003: bypassed, open");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_ALARM | ALARM_ZS_FAULT, 0, "Alarm \xC2\xB7 Open", red,
              NULL, "UI-003: alarm, open");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_ALARM, 0, "Alarm", red, NULL, "UI-003: alarm, closed");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_TROUBLE, 1u << ALARM_ZTRBL_LOOP, "Sensor fault", amber,
              NULL, "UI-003: sensor fault");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_TAMPER, 1u << ALARM_ZTRBL_TAMPER, "Tamper", amber, NULL,
              "UI-003: tamper");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_LOW_BATTERY, 1u << ALARM_ZTRBL_LOW_BATTERY,
              "Low battery", amber, NULL, "UI-003: low battery");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_SUPERVISION, 1u << ALARM_ZTRBL_SUPERVISION,
              "Not reporting", amber, NULL, "UI-003: not reporting");
    zone_case(ALARM_DEVCLASS_DOOR, ALARM_ZS_SWINGER, 0, "Shut down until disarmed", gray, NULL,
              "UI-003: shut down by swinger suppression");

    static alarm_config_t cfg;
    alarm_config_defaults(&cfg);
    char buf[ALARM_NAME_LEN + 8];
    check_str(ui_zone_name(&cfg, 12, buf, sizeof(buf)), "Zone 12", "an unnamed zone");
    check_str(ui_user_name(&cfg, 0, buf, sizeof(buf)), "", "user 0 has no name");
}

/* ---- part 2: Home ------------------------------------------------------------------------ */

static const char *headline(void)
{
    return lv_subject_get_string(&subject_headline);
}
static const char *detail(void)
{
    return lv_subject_get_string(&subject_detail);
}
static const char *note(void)
{
    return lv_subject_get_string(&subject_hero_note);
}

static void home(const char *want_head, const char *want_detail, const char *want_note,
                 const char *glyph, const char *what)
{
    char w[160];
    snprintf(w, sizeof(w), "%s (headline)", what);
    check_str(headline(), want_head, w);
    snprintf(w, sizeof(w), "%s (detail)", what);
    check_str(detail(), want_detail, w);
    if (want_note != NULL) {
        snprintf(w, sizeof(w), "%s (note)", what);
        check_str(note(), want_note, w);
    }
    if (glyph != NULL) {
        check(strcmp(lv_subject_get_string(&subject_hero_icon), glyph) == 0, what);
    }
}

/* A quiet, disarmed house again. */
static void fresh(void)
{
    host_panel_init(EPOCH_MS);
    run(500);
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

static void test_home(void)
{
    /* KPD-004, UI-003: Home's headline, detail and note for each state of the panel. */
    fresh();
    home("Disarmed", "Ready to arm", "All zones closed", UI_ICON_SHIELD_CHECK,
         "KPD-004: disarmed and ready");

    fresh();
    host_panel_zone(4, ALARM_ZEV_FAULT);
    run(500);
    home("Not ready", "Back Door is open", "Close it, or bypass when arming", UI_ICON_SHIELD_ALERT,
         "KPD-004: one zone open, named");
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(500);
    home("Not ready", "Front Door and Back Door are open", "Close them, or bypass when arming",
         NULL, "KPD-004: two zones open, both named");
    host_panel_zone(3, ALARM_ZEV_FAULT);
    run(500);
    home("Not ready", "3 zones are open", "Close them, or bypass when arming", NULL,
         "KPD-004: three zones open, counted");

    fresh();
    host_panel_zone(3, ALARM_ZEV_TAMPER);
    run(500);
    check_str(headline(), "Not ready", "KPD-004: a tamper holds arming back (headline)");
    check_str(detail(), "Living Room Window is not ready",
              "KPD-004: a shut zone with a tamper is not called open");
    check_str(note(), "Check it, or bypass when arming", "KPD-004: a tamper says check, not close");

    fresh();
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "arming Away");
    run(500);
    home("Arming Away", "Leave now through the exit door", "Exit time", UI_ICON_LOG_OUT,
         "KPD-004: exit delay, Away");
    run(70000);
    check_str(headline(), "Armed Away", "KPD-004: armed Away (headline)");
    check_str(detail(), "The whole house is protected", "KPD-004: armed Away (detail)");
    check(strstr(note(), "Since ") == note() && strstr(note(), "By Master") != NULL,
          "KPD-004: armed Away's note says since when and by whom");
    check(strcmp(lv_subject_get_string(&subject_hero_icon), UI_ICON_LOCK) == 0,
          "KPD-004: armed Away shows the lock");

    /* Entry delay: the front door on the way in. */
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(500);
    home("Disarm now", "Entry through Front Door", "The alarm sounds when time runs out",
         UI_ICON_DOOR_OPEN, "KPD-004: entry delay");

    /* The entry time runs out: burglary alarm, then alarm memory after the disarm. */
    run(70000);
    check_str(headline(), "Burglary alarm", "KPD-004: burglary alarm (headline)");
    check_str(detail(), "Front Door", "KPD-004: burglary alarm (detail)");
    check_str(note(), "Enter your code to disarm", "KPD-004: burglary alarm (note)");
    check(strcmp(lv_subject_get_string(&subject_hero_icon), UI_ICON_SIREN) == 0,
          "KPD-004: burglary alarm shows the siren");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming after the alarm");
    run(1000);
    /* Disarmed inside the cancel window (300 s): the report is canceled. */
    home("Alarm canceled", "Burglary alarm: Front Door", "No alert was sent. Clear it with your code",
         UI_ICON_BELL_RING, "KPD-004: alarm memory after a disarm inside the cancel window");

    /* Later than that, the alarm occurred and was reported. */
    fresh();
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "arming Away again");
    run(70000);
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(400000);
    check_str(note(), "The siren timed out", "KPD-004: the siren timed out (note)");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming after the siren");
    run(1000);
    home("Alarm occurred", "Burglary alarm: Front Door", "Review it, then clear it with your code",
         UI_ICON_BELL_RING, "KPD-004: alarm memory after the disarm");

    fresh();
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "arming Stay");
    run(500);
    home("Arming Stay", "Stay inside, arming shortly", "Quiet exit time", NULL,
         "KPD-004: exit delay, Stay");
    run(70000);
    check_str(headline(), "Armed Stay", "KPD-004: armed Stay (headline)");
    check_str(detail(), "Doors and windows are protected", "KPD-004: armed Stay (detail)");
    check(ends_with(note(), " \xC2\xB7 By Master") || strcmp(note(), "By Master") == 0,
          "KPD-004: armed Stay's note says by whom");
    check(strcmp(lv_subject_get_string(&subject_hero_icon), UI_ICON_HOUSE) == 0,
          "KPD-004: armed Stay shows the house");

    fresh();
    host_panel_zone(6, ALARM_ZEV_FAULT);
    run(500);
    home("Fire alarm", "Kitchen Smoke", "Get everyone out. Silence stops the siren",
         UI_ICON_FLAME, "KPD-004, UI-003: fire alarm");

    fresh();
    host_panel_zone(7, ALARM_ZEV_FAULT);
    run(500);
    home("Carbon monoxide", "Hallway CO", "Get everyone into fresh air", UI_ICON_CLOUD_FOG,
         "KPD-004: carbon monoxide alarm");

    /* A trouble does not take Home's card over: the house is still disarmed and ready. */
    fresh();
    host_panel_trouble(ALARM_TRBL_AC_LOSS, true);
    run(500);
    check_str(headline(), "Disarmed", "KPD-004: a panel trouble leaves Home's headline alone");
    host_panel_trouble(ALARM_TRBL_AC_LOSS, false);
    host_panel_zone(8, ALARM_ZEV_LOW_BATTERY);
    run(500);
    check_str(headline(), "Disarmed", "KPD-004: a zone's low battery leaves Home's headline alone");

    /* WALK-013: Home while a walk test runs (docs/features/40). */
    fresh();
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = 1;
    cmd.walk_op = (uint8_t)ALARM_WALK_START;
    snprintf(cmd.code, sizeof(cmd.code), "123456");
    check(host_panel_manager_command(&cmd) == ALARM_OK, "WALK-002: the walk test starts");
    run(500);
    home("Walk test", "Open and close each zone", "The panel beeps for every zone it sees",
         UI_ICON_FOOTPRINTS, "WALK-013: Home during a walk test");
}

/* ---- part 3: ui_when_text() ---------------------------------------------------------------- */

static int64_t s_now_ms; /* the clock part 3 hands the UI */

static int64_t when_clock(void *ctx)
{
    (void)ctx;
    return s_now_ms;
}

/* The epoch of a wall-clock time in the panel's time zone (TZ, set in main). */
static int64_t local_ms(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm) * 1000;
}

/* The day of the year in UTC, to show a case tells the panel's zone from UTC. */
static int utc_yday(int64_t ms)
{
    const time_t t = (time_t)(ms / 1000);
    struct tm tm;
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm.tm_yday;
}

static bool converts(int64_t ms)
{
    const time_t t = (time_t)(ms / 1000);
    struct tm tm;
#if defined(_WIN32)
    return localtime_s(&tm, &t) == 0;
#else
    return localtime_r(&t, &tm) != NULL;
#endif
}

static void when(int64_t then_ms, ui_when_t want, const char *want_text, const char *what)
{
    char buf[32];
    memset(buf, 'x', sizeof(buf));
    const ui_when_t got = ui_when_text(then_ms, buf, sizeof(buf));
    if (got != want) {
        printf("FAIL  %s: wanted kind %d, got %d (\"%s\")\n", what, (int)want, (int)got, buf);
        exit(1);
    }
    check_str(buf, want_text, what);
}

/* A catalog of a few entries (i18n.h's layout: sorted by hash), for the longest sentence a
 * translation may make: a date pattern whose result is 31 bytes, and sentences of 16 bytes of
 * words (their notes' budgets). */
typedef struct {
    const char *ctx;
    const char *english;
    const char *text;
} when_entry_t;

static const when_entry_t WHEN_WORDS[] = {
    /* "el 31 de December del año 2025": 31 bytes (no month names here: English ones) */
    {"date pattern", "MMM d, y", "'el' d 'de' MMMM 'del a\xC3\xB1o' y"},
    {NULL, "Ran on {0}", "\xC3\x9Altima vez, el {0}"},            /* 16 bytes of words */
    {"since a date", "Offline since {0}", "Desconectado el {0}"}, /* 16 bytes of words */
};
#define WHEN_N (sizeof(WHEN_WORDS) / sizeof(WHEN_WORDS[0]))

static i18n_entry_t s_when_entries[WHEN_N];
static char s_when_strings[256];

static void when_catalog(i18n_catalog_t *cat)
{
    size_t at = 0;
    for (size_t i = 0; i < WHEN_N; i++) {
        const i18n_entry_t e = {i18n_hash(WHEN_WORDS[i].ctx, WHEN_WORDS[i].english), (uint32_t)at};
        const size_t n = strlen(WHEN_WORDS[i].text) + 1u;
        memcpy(s_when_strings + at, WHEN_WORDS[i].text, n);
        at += n;
        size_t k = i;
        while (k > 0u && s_when_entries[k - 1u].hash > e.hash) {
            s_when_entries[k] = s_when_entries[k - 1u];
            k--;
        }
        s_when_entries[k] = e;
    }
    const i18n_catalog_t c = {"xx", (uint8_t)I18N_PLURALS_EN, 0, (uint32_t)WHEN_N, s_when_entries,
                              s_when_strings};
    *cat = c;
}

/* As ui_devices.c's tiles say it, into their 48-byte buffers (state_words, the Add list's row). */
static void when_sentence(const char *template_en, const char *ctx, int64_t then_ms, char *out,
                          size_t len)
{
    char t[32];
    const ui_when_t k = ui_when_text(then_ms, t, sizeof(t));
    check(k == UI_WHEN_DAY || k == UI_WHEN_YEAR, "BL-179: the long sentence's time is a date");
    I18N_FORMAT(out, len, i18n_text(S.catalog, ctx, template_en), t);
}

static void test_when(void)
{
    /* HCTL-006 (BL-179): days and years in the panel's zone, set in main (not UTC). */
    const ui_backend_t saved = S.be;
    S.be.epoch_ms = when_clock;

    /* the clock isn't set: no date, whatever the time */
    s_now_ms = 0;
    when(local_ms(2026, 9, 19, 20, 30, 0), UI_WHEN_UNKNOWN, "", "BL-179: no clock, no time");
    /* the time isn't known */
    s_now_ms = local_ms(2026, 9, 19, 0, 30, 0);
    when(0, UI_WHEN_UNKNOWN, "", "BL-179: a time of 0 is none");
    when(-5000, UI_WHEN_UNKNOWN, "", "BL-179: a time before 1970 is none");

    /* local midnight, not UTC's: at 12:30 AM, 11:59 PM was yesterday though UTC's day is the same */
    const int64_t before = local_ms(2026, 9, 18, 23, 59, 0);
    const int64_t after = local_ms(2026, 9, 19, 0, 1, 0);
    check(utc_yday(before) == utc_yday(s_now_ms), "BL-179: the zone's midnight is not UTC's here");
    when(before, UI_WHEN_DAY, "Sep 18", "BL-179: yesterday 11:59 PM is a date");
    when(after, UI_WHEN_TODAY, "12:01 AM", "BL-179: today 12:01 AM is a time");
    /* late in the evening UTC is on the next day already: 12:01 AM is still today */
    s_now_ms = local_ms(2026, 9, 19, 22, 41, 0);
    check(utc_yday(after) != utc_yday(s_now_ms), "BL-179: UTC's day has changed here");
    when(after, UI_WHEN_TODAY, "12:01 AM", "BL-179: this morning is today in the evening");
    when(local_ms(2026, 1, 2, 9, 5, 0), UI_WHEN_DAY, "Jan 2", "BL-179: earlier this year: no year");

    /* New Year: Dec 31 is an earlier year, Jan 1 just after midnight today */
    s_now_ms = local_ms(2026, 1, 1, 0, 1, 0);
    when(local_ms(2025, 12, 31, 23, 59, 0), UI_WHEN_YEAR, "Dec 31, 2025",
         "BL-179: Dec 31 seen on Jan 1 has its year");
    when(local_ms(2026, 1, 1, 0, 0, 30), UI_WHEN_TODAY, "12:00 AM", "BL-179: Jan 1 12:00 AM is today");
    when(local_ms(2025, 1, 1, 0, 0, 0), UI_WHEN_YEAR, "Jan 1, 2025", "BL-179: a year ago has its year");

    /* ahead of the clock: up to a minute is now; more is no time */
    s_now_ms = local_ms(2026, 9, 19, 22, 41, 0);
    when(s_now_ms + 60000, UI_WHEN_TODAY, "10:41 PM", "BL-179: a minute ahead reads as now");
    when(s_now_ms + 60001, UI_WHEN_UNKNOWN, "", "BL-179: more than a minute ahead has no time");
    when(s_now_ms + 24LL * 3600000LL, UI_WHEN_UNKNOWN, "", "BL-179: tomorrow has no time");

    /* a time the C library can't convert (Windows: after the year 3000) is none; a library that
     * converts it gives its time */
    s_now_ms = 100000000000000000LL; /* the year 3 million or so */
    if (!converts(s_now_ms)) {
        when(s_now_ms, UI_WHEN_UNKNOWN, "", "BL-179: a time that can't be converted is none");
    } else {
        char buf[32];
        check(ui_when_text(s_now_ms, buf, sizeof(buf)) == UI_WHEN_TODAY,
              "BL-179: a time this C library converts reads (it can't fail here)");
    }

    /* the longest sentence: a 31-byte date in 16 bytes of words is 47 bytes, whole in 48 */
    const i18n_catalog_t *const english = S.catalog;
    i18n_catalog_t cat;
    when_catalog(&cat);
    S.catalog = &cat;
    s_now_ms = local_ms(2026, 1, 1, 0, 1, 0);
    char t[32];
    check(ui_when_text(local_ms(2025, 12, 31, 23, 59, 0), t, sizeof(t)) == UI_WHEN_YEAR &&
              strlen(t) == 31u,
          "BL-179: the translated date with its year is 31 bytes");
    check_str(t, "el 31 de December del a\xC3\xB1o 2025", "BL-179: the translated date, whole");
    char s[48];
    when_sentence("Ran on {0}", NULL, local_ms(2025, 12, 31, 23, 59, 0), s, sizeof(s));
    check(strlen(s) == 47u, "BL-179: the longest \"Ran on\" sentence is 47 bytes");
    check_str(s, "\xC3\x9Altima vez, el el 31 de December del a\xC3\xB1o 2025",
              "BL-179: the longest \"Ran on\" sentence, whole");
    when_sentence("Offline since {0}", "since a date", local_ms(2025, 12, 31, 23, 59, 0), s,
                  sizeof(s));
    check(strlen(s) == 47u, "BL-179: the longest \"Offline since\" sentence is 47 bytes");
    check_str(s, "Desconectado el el 31 de December del a\xC3\xB1o 2025",
              "BL-179: the longest \"Offline since\" sentence, whole");
    S.catalog = english;
    S.be = saved;
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

    test_tones();
    test_icon_table();
    test_zone_card();
    test_home();

    /* The panel's fixed time zone, as the PC simulator sets it (sim_main.c): part 3's days. */
#if defined(_WIN32)
    _putenv("TZ=EST5EDT,M3.2.0,M11.1.0");
    _tzset();
#else
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
#endif
    test_when();

    printf("%d checks passed: ui_text.c, Home's words and a device's time\n", s_checks);
    return 0;
}
