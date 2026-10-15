/*
 * ui_auto_warn.c - an automation's arming warning at the wall (docs/features/41 phase C, task C5;
 * AUT-012, AUT-013, D19, D20, D26-D28; DESIGN.md section 9 items 28-30).
 *
 * Drawn from the approved screens (mockup screens/home.js): the steady amber
 * status-bar banner while a warning is on (home-arm-warning), Home's short card above the arming
 * tiles with one Put off button (arm_strip; the tiles keep their look and stay usable, D19), its
 * not-ready line (home-arm-warning-not-ready, -bypass), the choice and the code
 * (home-arm-put-off-choice, home-arm-put-off) and the quiet hold note in the card's place after a
 * put-off (home-arm-put-off-home, home-arm-skipped-home).
 *
 * The alarm comes first (D22): nothing here hides, moves or dims the arming tiles, an alarm or a
 * trouble; the banner yields to every other banner but the factory-code reminder, and to an
 * alarm. Live words (UI-020): the warnings are read once a second, and a subject is written only
 * when its text changed; nothing counts down, blinks or moves. The UI plays nothing: the chime
 * is the automation service's.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_internal.h"

#define POLL_MS            1000u
#define NIGHT_ENDS_MINUTE  360  /* "Not tonight" holds until 6:00 AM ... */
#define DAY_STARTS_MINUTE  360  /* ... and an arm time from 6 AM to 6 PM reads "Not today" */
#define DAY_ENDS_MINUTE    1080

static struct {
    bool ready;          /* the backend has the hooks */
    bool polled_once;
    uint32_t polled;
    bool banner;         /* a warning is on: the bar may show banner 5 */
    uint16_t poff_uid;   /* the warning the choice or the code pad is about; 0 = none */
    uint8_t poff_choice; /* auto_put_off_choice_t */
    size_t n;            /* s_warn's count at the last read */
} W;

static UI_SLOW_BSS auto_warning_t s_warn[AUTO_MAX];
static UI_SLOW_BSS auto_rule_t s_rules[AUTO_MAX];

static bool poff_code(const char *code);

/* ---- words ------------------------------------------------------------------------------- */

static void local_minute_of(int64_t epoch_ms, int *minute)
{
    const time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    *minute = tm.tm_hour * 60 + tm.tm_min;
}

/* The planned time is a whole minute; the copy the runner sends may be a few ms short of it. */
static int64_t whole_minute(int64_t epoch_ms)
{
    return (epoch_ms + 30000) / 60000 * 60000;
}

static const char *partition_name(uint8_t p, char *buf, size_t len)
{
    if (p >= 1u && p <= ALARM_MAX_PARTITIONS && S.cfg->partitions[p - 1u].name[0] != '\0') {
        i18n_utf8_copy(buf, len, S.cfg->partitions[p - 1u].name);
    } else {
        I18N_FORMAT(buf, len, TR_C("default partition name", "Partition {0}"), (unsigned)p);
    }
    return buf;
}

static const alarm_partition_status_t *part_of(uint8_t p)
{
    return p >= 1u && p <= ALARM_MAX_PARTITIONS ? &S.snap.part[p - 1u] : NULL;
}

/* A warning or a hold shows only while its partition is disarmed (AUT-013: hidden while armed;
 * someone arming by hand ends the warning, AUT-014). */
static bool disarmed(uint8_t p)
{
    const alarm_partition_status_t *ps = part_of(p);
    return ps != NULL && S.have_snap && ps->state == ALARM_STATE_DISARMED;
}

/* "Arming Night at 10:30 PM": the banner, the card's title and the choice's line. At most 63
 * bytes with the time. */
static void arming_text(const auto_warning_t *w, char *out, size_t len)
{
    char at[24];
    if (w->arm_at_epoch_ms <= 0) {
        /* TR: Home's card and the status bar while an automation will arm this partition soon;
         * the clock isn't set, so no time. At most 63 bytes */
        i18n_utf8_copy(out, len, w->level == ALARM_LEVEL_STAY    ? TR("Arming Stay soon")
                                 : w->level == ALARM_LEVEL_NIGHT ? TR("Arming Night soon")
                                                                 : TR("Arming Away soon"));
        return;
    }
    ui_clock_text(whole_minute(w->arm_at_epoch_ms), at, sizeof(at), true);
    /* TR: Home's card and the status bar while an automation will arm this partition; {0} is
     * the time it arms, such as 10:30 PM. At most 54 bytes */
    I18N_FORMAT(out, len, w->level == ALARM_LEVEL_STAY    ? TR("Arming Stay at {0}")
                          : w->level == ALARM_LEVEL_NIGHT ? TR("Arming Night at {0}")
                                                          : TR("Arming Away at {0}"),
                at);
}

/* "Arms Night at 11:30 PM": what an hour's put-off does, and the hold note after it. */
static void arms_text(uint8_t level, int64_t epoch_ms, char *out, size_t len)
{
    char at[24];
    if (epoch_ms <= 0) {
        /* TR: Put off arming?, An hour later's line, and Home's note after it, when the clock
         * isn't set. At most 63 bytes */
        i18n_utf8_copy(out, len, TR("Put off an hour"));
        return;
    }
    ui_clock_text(whole_minute(epoch_ms), at, sizeof(at), true);
    /* TR: Put off arming?, An hour later's line, and Home's note after it; {0} is the time it
     * will arm, such as 11:30 PM. At most 54 bytes */
    I18N_FORMAT(out, len, level == ALARM_LEVEL_STAY    ? TR("Arms Stay at {0}")
                          : level == ALARM_LEVEL_NIGHT ? TR("Arms Night at {0}")
                                                       : TR("Arms Away at {0}"),
                at);
}

/* The arming's time of day: the rule's own for a time-of-day trigger (*timed; an hour's put-off
 * moves only today's arm time), else the planned time's; -1 when neither is known. */
static int arm_minute(const auto_warning_t *w, bool *timed)
{
    *timed = false;
    if (S.be.auto_list != NULL) {
        const size_t n = S.be.auto_list(S.be.ctx, s_rules, AUTO_MAX);
        for (size_t i = 0; i < n; i++) {
            if (s_rules[i].uid == w->uid && s_rules[i].rec == AUTO_REC_OK &&
                s_rules[i].when.kind == AUTO_TRIG_TIME) {
                *timed = true;
                return (int)(s_rules[i].when.minute % AUTO_MINUTES_PER_DAY);
            }
        }
    }
    if (w->arm_at_epoch_ms > 0) {
        int m = 0;
        local_minute_of(whole_minute(w->arm_at_epoch_ms), &m);
        return m;
    }
    return -1;
}

static bool daytime(int minute)
{
    return minute >= DAY_STARTS_MINUTE && minute < DAY_ENDS_MINUTE;
}

/* Not today or Not tonight: decided by the planned arm time, as the runner decides it
 * (auto_arm.c), which an hour's put-off has moved (a 5:30 PM arming put off to 6:30 PM is held
 * until 6:00 AM); the rule's own minute (from arm_minute) only when the plan has no time. */
static bool planned_day(const auto_warning_t *w, int minute)
{
    if (w->arm_at_epoch_ms > 0) {
        int m = 0;
        local_minute_of(whole_minute(w->arm_at_epoch_ms), &m);
        return daytime(m);
    }
    return minute >= 0 && daytime(minute);
}

/* ---- reading the warnings ------------------------------------------------------------------ */

/* The screen's partition's warning or hold (the runner keeps one at a time per automation; the
 * one that warns now first). */
static const auto_warning_t *mine(size_t n)
{
    const auto_warning_t *hold = NULL;
    for (size_t i = 0; i < n; i++) {
        const auto_warning_t *w = &s_warn[i];
        if (w->partition != S.opt.partition) {
            continue;
        }
        if (w->state == AUTO_WARN_ON) {
            return w;
        }
        if ((w->state == AUTO_WARN_HOUR || w->state == AUTO_WARN_NIGHT ||
             w->state == AUTO_WARN_DAY) &&
            hold == NULL) {
            hold = w;
        }
    }
    return hold;
}

static const auto_warning_t *by_uid(size_t n, uint16_t uid)
{
    for (size_t i = 0; i < n; i++) {
        if (s_warn[i].uid == uid) {
            return &s_warn[i];
        }
    }
    return NULL;
}

/* The status bar: the screen's partition's warning, else another partition's, named. */
static void banner_update(size_t n, const auto_warning_t *own)
{
    const auto_warning_t *w = own != NULL && own->state == AUTO_WARN_ON && disarmed(own->partition)
                                  ? own
                                  : NULL;
    for (size_t i = 0; i < n && w == NULL; i++) {
        if (s_warn[i].state == AUTO_WARN_ON && disarmed(s_warn[i].partition)) {
            w = &s_warn[i];
        }
    }
    const bool on = w != NULL;
    if (on) {
        char arming[64];
        char name[ALARM_NAME_LEN + 16];
        char text[sizeof(name) + sizeof(arming) + 4]; /* ui_set_str cuts it at 127 bytes */
        arming_text(w, arming, sizeof(arming));
        if (w->partition == S.opt.partition) {
            i18n_utf8_copy(text, sizeof(text), arming);
        } else {
            /* two independent facts: the partition, and the arming */
            snprintf(text, sizeof(text), "%s \xC2\xB7 %s",
                     partition_name(w->partition, name, sizeof(name)), arming);
        }
        ui_set_str(&subject_arm_banner, text);
    }
    if (on != W.banner) {
        W.banner = on;
        ui_bar_banner_update();
    }
}

/* Home's card: the warning with Put off, or the quiet hold note in its place. Only beside the
 * arming tiles (subject_panel 0): never over the code pad, the disarm tile or a walk test. */
static void card_update(const auto_warning_t *w)
{
    if (w == NULL || !disarmed(w->partition) || lv_subject_get_int(&subject_panel) != 0) {
        ui_set_int(&subject_arm_strip, 0);
        return;
    }
    char title[64];
    char detail[96];
    if (w->state == AUTO_WARN_ON) {
        arming_text(w, title, sizeof(title));
        ui_set_str(&subject_arm_strip_title, title);
        /* TR: Home's card while an automation will arm, the line under "Arming Night at 10:30
         * PM" */
        ui_set_str(&subject_arm_strip_detail, TR("Set by an automation"));
        const alarm_partition_status_t *ps = part_of(w->partition);
        const bool not_ready = ps != NULL && !ps->ready;
        if (not_ready) {
            /* TR: Home's card while an automation will arm and a zone is open (D28): it will
             * bypass the open zones, or it won't arm */
            ui_set_str(&subject_arm_strip_note, w->bypass ? TR("Open zones will be bypassed")
                                                          : TR("Not ready: it won't arm yet"));
        }
        ui_set_int(&subject_arm_strip_note_on, not_ready ? 1 : 0);
        ui_set_str(&subject_arm_strip_glyph, w->level == ALARM_LEVEL_STAY    ? UI_ICON_HOUSE
                                             : w->level == ALARM_LEVEL_NIGHT ? UI_ICON_MOON
                                                                             : UI_ICON_LOG_OUT);
        ui_set_color(&subject_arm_strip_color, ui_tone_for_level((alarm_arm_level_t)w->level));
        ui_set_int(&subject_arm_strip, 1);
        return;
    }
    if (w->state == AUTO_WARN_HOUR) {
        arms_text(w->level, w->arm_at_epoch_ms, title, sizeof(title));
    } else {
        /* TR: Home's quiet note after "Not tonight" or "Not today" put an automation's arming
         * off. At most 63 bytes */
        i18n_utf8_copy(title, sizeof(title),
                       w->state == AUTO_WARN_DAY ? TR("Won't arm today") : TR("Won't arm tonight"));
    }
    ui_set_str(&subject_arm_strip_title, title);
    detail[0] = '\0';
    if (w->put_off_by != 0u) {
        char name[ALARM_NAME_LEN + 16];
        /* TR: Home's quiet note after an arming was put off, its line; {0} is the person's
         * name. At most 79 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("Put off by {0}"),
                    ui_user_name(S.cfg, w->put_off_by, name, sizeof(name)));
    }
    ui_set_str(&subject_arm_strip_detail, detail);
    ui_set_int(&subject_arm_strip_note_on, 0);
    ui_set_str(&subject_arm_strip_glyph, UI_ICON_CLOCK);
    ui_set_color(&subject_arm_strip_color, ui_tone_gray());
    ui_set_int(&subject_arm_strip, 2);
}

/* The choice and the code pad close when their warning ends or its partition is armed. */
static void poff_check(size_t n)
{
    if (W.poff_uid == 0u) {
        return;
    }
    const auto_warning_t *w = by_uid(n, W.poff_uid);
    if (w != NULL && w->state == AUTO_WARN_ON && disarmed(w->partition)) {
        return;
    }
    W.poff_uid = 0u;
    if (lv_subject_get_int(&subject_sheet) == SHEET_PUT_OFF) {
        ui_sheet_close();
    }
    if (S.pad == PAD_ASK && S.pad_fn == poff_code) {
        ui_pad_cancel();
    }
}

/* A copy of the warning mailbox, into s_warn: the count. */
static size_t read_warnings(void)
{
    const size_t got = S.be.auto_warnings(S.be.ctx, s_warn, AUTO_MAX);
    W.n = got < AUTO_MAX ? got : AUTO_MAX;
    return W.n;
}

static void refresh(void)
{
    W.polled = lv_tick_get();
    W.polled_once = true;
    const size_t n = read_warnings();
    const auto_warning_t *own = mine(n);
    banner_update(n, own);
    card_update(own);
    poff_check(n);
}

void ui_auto_warn_tick(void)
{
    if (!W.ready || (W.polled_once && lv_tick_elaps(W.polled) < POLL_MS)) {
        return;
    }
    refresh();
}

bool ui_auto_warn_banner(void)
{
    return W.banner;
}

void ui_auto_warn_language_changed(void)
{
    if (W.ready) {
        refresh();
    }
}

/* ---- Put off (D26): the choice, then the code ------------------------------------------- */

static const auto_warning_t *poff_warning(void)
{
    const auto_warning_t *w = by_uid(read_warnings(), W.poff_uid);
    return w != NULL && w->state == AUTO_WARN_ON && disarmed(w->partition) ? w : NULL;
}

/* "Arms again tomorrow at 10:30 PM"; for an arming started by something else than a time of
 * day "Not before 6:00 AM" (midnight for a daytime one). */
static void again_text(int minute, bool timed, bool day, char *out, size_t len)
{
    char at[24];
    if (minute < 0) {
        /* TR: Put off arming?, the second choice's line while the clock isn't set */
        i18n_utf8_copy(out, len, TR("Set the clock first"));
        return;
    }
    if (!timed) {
        ui_minute_text(day ? 0 : NIGHT_ENDS_MINUTE, at, sizeof(at));
        /* TR: Put off arming?, the line under "Not tonight" or "Not today" for an arming that
         * isn't at a time of day; {0} is 6:00 AM, or 12:00 AM for midnight. At most 70 bytes */
        I18N_FORMAT(out, len, TR("Not before {0}"), at);
        return;
    }
    ui_minute_text(minute, at, sizeof(at));
    /* TR: Put off arming?, the line under "Not tonight" or "Not today"; {0} is the arm time,
     * such as 10:30 PM. At most 70 bytes */
    I18N_FORMAT(out, len, TR("Arms again tomorrow at {0}"), at);
}

static void open_choice(void)
{
    const auto_warning_t *w = mine(read_warnings());
    if (w == NULL || w->state != AUTO_WARN_ON || !disarmed(w->partition)) {
        refresh(); /* it ended meanwhile: the card goes */
        return;
    }
    const auto_warning_t warn = *w;
    char arming[64];
    char hour[64];
    char again[96];
    arming_text(&warn, arming, sizeof(arming));
    arms_text(warn.level, warn.arm_at_epoch_ms > 0 ? warn.arm_at_epoch_ms + 3600000 : 0, hour,
              sizeof(hour));
    bool timed = false;
    const int minute = arm_minute(&warn, &timed);
    const bool day = planned_day(&warn, minute);
    again_text(minute, timed, day, again, sizeof(again));
    ui_sheet_open(SHEET_PUT_OFF, NULL, NULL);
    W.poff_uid = warn.uid;
    /* TR: the sheet that opens from Home's Put off: an automation's arming */
    ui_set_str(&subject_sheet_title, TR("Put off arming?"));
    ui_set_str(&subject_sheet_note, arming);
    ui_set_str(&subject_poff_hour, hour);
    /* TR: Put off arming?, the second choice: no arming before 6:00 AM, or until midnight for
     * an arm time from 6 AM to 6 PM */
    ui_set_str(&subject_poff_night_title, day ? TR("Not today") : TR("Not tonight"));
    ui_set_str(&subject_poff_night, again);
}

static void ask_code(uint8_t choice)
{
    const auto_warning_t *w = poff_warning();
    if (w == NULL) {
        ui_sheet_close();
        W.poff_uid = 0u;
        refresh();
        return;
    }
    const auto_warning_t warn = *w;
    bool timed = false;
    const int minute = arm_minute(&warn, &timed);
    const bool day = planned_day(&warn, minute);
    char note[128];
    char at[24];
    if (choice == AUTO_PUT_OFF_HOUR) {
        if (warn.arm_at_epoch_ms > 0) {
            ui_clock_text(whole_minute(warn.arm_at_epoch_ms + 3600000), at, sizeof(at), true);
            /* TR: the code pad over the screen after "An hour later"; {0} is the new arm time,
             * such as 11:30 PM. At most 119 bytes */
            I18N_FORMAT(note, sizeof(note),
                        warn.level == ALARM_LEVEL_STAY
                            ? TR("Any code puts it off an hour. Arms Stay at {0}.")
                        : warn.level == ALARM_LEVEL_NIGHT
                            ? TR("Any code puts it off an hour. Arms Night at {0}.")
                            : TR("Any code puts it off an hour. Arms Away at {0}."),
                        at);
        } else {
            /* TR: the code pad over the screen after "An hour later", while the clock isn't
             * set. At most 127 bytes */
            i18n_utf8_copy(note, sizeof(note), TR("Any code puts it off an hour."));
        }
    } else {
        char name[ALARM_NAME_LEN + 16];
        partition_name(warn.partition, name, sizeof(name));
        if (timed) {
            ui_minute_text(minute, at, sizeof(at));
            /* TR: the code pad over the screen after "Not tonight" or "Not today"; {0} is the
             * arm time, such as 10:30 PM, {1} the partition's name. At most 85 bytes */
            I18N_FORMAT(note, sizeof(note),
                        TR("Arms again tomorrow at {0}. Needs a code that may disarm {1}."), at,
                        name);
        } else {
            ui_minute_text(day ? 0 : NIGHT_ENDS_MINUTE, at, sizeof(at));
            /* TR: the code pad over the screen after "Not tonight" or "Not today" for an arming
             * that isn't at a time of day; {0} is 6:00 AM or 12:00 AM, {1} the partition's
             * name. At most 85 bytes */
            I18N_FORMAT(note, sizeof(note),
                        TR("Not before {0}. Needs a code that may disarm {1}."), at, name);
        }
    }
    ui_sheet_close();
    W.poff_uid = warn.uid;
    W.poff_choice = choice;
    ui_pad_ask(HOST_MODAL, UI_ICON_CLOCK,
               choice == AUTO_PUT_OFF_HOUR ? TR("Put off an hour")
               : day                       ? TR("Not today")
                                           : TR("Not tonight"),
               NULL, ui_tone_amber(), poff_code);
    ui_pad_ask_note(note);
}

/* The code's answer (AUT-013): a refused code says what every code pad says and counts toward
 * the lockout (the backend checks it as the keypad's); a code that may not disarm gets only the
 * hour (decided 2026-10-05). */
static bool poff_code(const char *code)
{
    const uint16_t uid = W.poff_uid;
    if (uid == 0u || S.be.auto_put_off == NULL) {
        return true;
    }
    alarm_result_t cr = ALARM_ERR_BAD_ARG;
    const int r = (int)S.be.auto_put_off(S.be.ctx, uid, W.poff_choice, code, &cr);
    if (r == (int)AUTO_POFF_OK || r == (int)AUTO_POFF_NO_WARNING) {
        /* No toast: Home shows the hold note (or, if it ended meanwhile, nothing) */
        W.poff_uid = 0u;
        refresh();
        return true;
    }
    if (r == (int)UI_AUTO_POFF_BUSY) {
        ui_pad_error(TR("The panel is busy. Try again in a moment."));
        return false;
    }
    ui_beep(UI_BEEP_ERROR);
    if (r == (int)UI_AUTO_POFF_CODE) {
        ui_pad_error(ui_result_text(cr)); /* the same words as every code pad */
    } else if (r == (int)AUTO_POFF_NOT_ALLOWED) {
        ui_pad_error(TR("That code can only put it off an hour"));
    } else if (r == (int)AUTO_POFF_NO_CLOCK) {
        ui_pad_error(TR("Set the clock first"));
    } else {
        ui_pad_error(ui_result_text(ALARM_ERR_BAD_ARG));
    }
    return false;
}

/* ---- start ------------------------------------------------------------------------------- */

static void on_put_off(lv_event_t *e)
{
    (void)e;
    open_choice();
}

static void on_choice(lv_event_t *e)
{
    const uintptr_t choice = (uintptr_t)lv_event_get_user_data(e);
    ask_code((uint8_t)choice);
}

static void on_click(lv_obj_t *o, lv_event_cb_t cb, uintptr_t ud)
{
    if (o != NULL) {
        lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, (void *)ud);
    }
}

/* Home's right side changed (the arming pad opened or closed): the card comes or goes with the
 * tiles at once, without waiting for the next read. */
static void panel_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    if (W.ready && W.polled_once) {
        card_update(mine(W.n));
    }
}

void ui_auto_warn_init(void)
{
    memset(&W, 0, sizeof(W));
    W.ready = S.be.auto_warnings != NULL && S.be.auto_put_off != NULL;
    ui_set_int(&subject_arm_strip, 0);
    if (!W.ready) {
        return;
    }
    lv_subject_add_observer(&subject_panel, panel_changed, NULL);
    on_click(ui_find(S.main, "arm_strip_put_off"), on_put_off, 0u);
    on_click(ui_find(S.main, "poff_hour"), on_choice, (uintptr_t)AUTO_PUT_OFF_HOUR);
    on_click(ui_find(S.main, "poff_night"), on_choice, (uintptr_t)AUTO_PUT_OFF_NIGHT);
}
