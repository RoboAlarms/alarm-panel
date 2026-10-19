/*
 * ui_walk.c - the walk test at the touchscreen (docs/features/40): Settings > Walk test, where the
 * installer, the system master or a partition master starts one and follows it - the count, the
 * time left, Extend, End and the list of the partition's zones - and Home's card while one runs
 * (ui_app.c owns the code pad Home's Extend opens; End asks a question here, WALK-025). Without a
 * code the page shows this keypad's test or its summary (WALK-009). Also a test started elsewhere
 * (WALK-023), the end warning (WALK-004), the summary after the end (WALK-010, on the page and as a
 * status-bar banner until it is seen or the partition arms) and the installer menu's entry.
 * Everything shown comes from the engine's snapshot; every change goes through the backend's
 * command, as an arming does.
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

/* A zone moves in the list at most once a second (WALK-013, UI-023). */
#define WALK_REBUILD_MS 1000u
/* Home's count and time left change at most once a second (UI-020, UI-023). */
#define WALK_WORDS_MS 1000u
/* The engine's end warnings come at 5 and 1 minutes left (WALK-004): from 5 on, the screen says
 * so. */
#define WALK_WARN_MIN 5u

static uint8_t s_part;    /* the partition the page shows, 1-based */
static bool s_view;       /* the page was opened without a code: it shows, Extend and Start ask */
static bool s_life24;     /* the start's "Include panic and 24-hour zones" (WALK-007) */
static uint8_t s_end_part; /* the partition End's question is about */
static bool s_end_coded;  /* ...asked on the page opened with a code, which the log then names */
static uint32_t s_sig;    /* what the page was last built from */
static uint32_t s_built_at;

/* This keypad's partition, followed every tick (WALK-023, WALK-004, WALK-010). */
static alarm_ms_t s_seen_start;   /* walk_started_ms of the last test seen starting */
static bool s_own_start;          /* a start sent from this screen is on its way */
static bool s_from_here;          /* the running test was started at this screen */
static bool s_pending;            /* started elsewhere while someone was busy here */
static bool s_after_installer;    /* the start is the installer menu's, installer mode just ended */
static bool s_unsaved;            /* ...and leaving it showed a "Not saved" toast */
static alarm_ms_t s_summary_seen; /* walk_started_ms whose summary was seen or put away */
static uint32_t s_words_at;       /* Home's words last written */
static bool s_words_due;          /* a new test: write them now */

static const alarm_partition_status_t *walk_part(uint8_t p)
{
    return &S.snap.part[p - 1u];
}

bool ui_walk_running(void)
{
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if (S.snap.part[i].walk_test) {
            return true;
        }
    }
    return false;
}

static uint32_t minutes_left(const alarm_partition_status_t *ps)
{
    const alarm_ms_t now = S.be.now_ms(S.be.ctx);
    const alarm_ms_t elapsed = now > S.snap.taken_ms ? now - S.snap.taken_ms : 0u;
    const alarm_ms_t left =
        ps->walk_test_remaining_ms > elapsed ? ps->walk_test_remaining_ms - elapsed : 0u;
    return (uint32_t)((left + 59999u) / 60000u);
}

static void count_text(const alarm_partition_status_t *ps, char *buf, size_t len)
{
    /* TR: the walk test's progress on Home's card and the Walk test page: {0} zones tested of
     * {1} (each up to 3 bytes; the form follows {0}). At most 40 bytes */
    I18N_FORMAT(buf, len, TRN("{0} of {1} tested", "{0} of {1} tested", ps->walk_tested),
                (unsigned)ps->walk_tested, (unsigned)ps->walk_total);
}

/* The end warning's words (WALK-004): on Home's card and the Walk test page. */
static void warn_text(uint32_t m, char *buf, size_t len)
{
    /* TR: the walk test's end warning, steady on Home's card and the Walk test page, beside
     * Extend; {0} is 1 to 5. At most 60 bytes */
    I18N_FORMAT(buf, len, TRN("Walk test ends in {0} minute", "Walk test ends in {0} minutes", m),
                (unsigned)m);
}

static void left_text(const alarm_partition_status_t *ps, char *buf, size_t len, bool warn)
{
    const uint32_t m = minutes_left(ps);
    if (warn && m <= WALK_WARN_MIN) {
        warn_text(m, buf, len);
        return;
    }
    /* TR: how long the walk test still runs, on Home's card and the Walk test page; {0} is up
     * to 4 bytes. At most 40 bytes */
    I18N_FORMAT(buf, len, TRN("Ends in {0} min", "Ends in {0} min", m), (unsigned)m);
}

/* ---- started elsewhere (WALK-023) ---------------------------------------------------------- */

typedef enum {
    FROM_HERE,
    FROM_PANEL_MANAGER,
    FROM_KEYPAD,
    FROM_CONSOLE,
    FROM_ELSEWHERE,
} walk_from_t;

static walk_from_t started_from(const alarm_partition_status_t *ps)
{
    const alarm_source_t *src = &ps->walk_started_src;
    if (src->kind == (uint8_t)ALARM_SRC_KEYPAD) {
        return src->id == ALARM_KEYPAD_LOCAL      ? FROM_HERE
               : src->id == ALARM_KEYPAD_CONSOLE ? FROM_CONSOLE
                                                : FROM_KEYPAD;
    }
    if (src->kind == (uint8_t)ALARM_SRC_REMOTE && src->id == ALARM_SRC_ID_PANEL_MANAGER) {
        return FROM_PANEL_MANAGER;
    }
    return FROM_ELSEWHERE;
}

/* "Walk test started by Alex from the Panel Manager": who (the user's name, as Home's "By Alex")
 * and from where (as the Activity list names the source). Whole sentences, one per place. */
static void notice_text(const alarm_partition_status_t *ps, char *buf, size_t len)
{
    char name[ALARM_NAME_LEN + 8];
    const bool who = ps->walk_started_by != 0u;
    if (who) {
        ui_user_name(S.cfg, ps->walk_started_by, name, sizeof(name));
    }
    /* TR: the steady notice heading the walk test (Home's card, the Walk test page) when it was
     * started away from this panel; {0} is the user's name, up to 31 bytes. At most 127 bytes */
    switch (started_from(ps)) {
    case FROM_PANEL_MANAGER:
        if (who) {
            I18N_FORMAT(buf, len, TR("Walk test started by {0} from the Panel Manager"), name);
        } else {
            i18n_utf8_copy(buf, len, TR("Walk test started from the Panel Manager"));
        }
        break;
    case FROM_KEYPAD:
        if (who) {
            I18N_FORMAT(buf, len, TR("Walk test started by {0} at another keypad"), name);
        } else {
            i18n_utf8_copy(buf, len, TR("Walk test started at another keypad"));
        }
        break;
    case FROM_CONSOLE:
        if (who) {
            I18N_FORMAT(buf, len, TR("Walk test started by {0} from the service console"), name);
        } else {
            i18n_utf8_copy(buf, len, TR("Walk test started from the service console"));
        }
        break;
    default:
        if (who) {
            I18N_FORMAT(buf, len, TR("Walk test started by {0} remotely"), name);
        } else {
            i18n_utf8_copy(buf, len, TR("Walk test started remotely"));
        }
        break;
    }
}

/* ---- Home, every tick ---------------------------------------------------------------------- */

static bool page_open(void)
{
    return lv_subject_get_int(&subject_view) == VIEW_SETTINGS && S.depth > 0u &&
           S.pages[S.depth - 1u].id == PAGE_WALK;
}

/* The last test's summary (WALK-010): kept by the engine until the next start or a restart. */
static bool has_summary(const alarm_partition_status_t *ps)
{
    return !ps->walk_test && ps->walk_end_how != (uint8_t)ALARM_WALK_END_NONE &&
           ps->walk_started_ms != 0u;
}

void ui_walk_home(void)
{
    if (!S.have_snap) {
        return;
    }
    const alarm_partition_status_t *ps = walk_part(S.opt.partition);
    if (ps->walk_test && ps->walk_started_ms != s_seen_start) {
        /* A new test on this keypad's partition: from here, or from elsewhere (WALK-023). */
        s_seen_start = ps->walk_started_ms;
        s_from_here = s_own_start || started_from(ps) == FROM_HERE;
        s_pending = !s_from_here;
        s_words_due = true;
    }
    if (!ps->walk_test) {
        s_pending = false;
    }
    /* Started elsewhere: the screen wakes and shows the test, unless someone is in the middle of
     * something here; then the banner says so, and the test opens when they finish. */
    if (s_pending && !ui_app_busy()) {
        s_pending = false;
        ui_app_walk_show();
    }
    if (has_summary(ps) && ps->state != ALARM_STATE_DISARMED) {
        s_summary_seen = ps->walk_started_ms; /* the next arm puts the summary away */
    }
    if (ps->walk_test && (s_words_due || lv_tick_elaps(s_words_at) >= WALK_WORDS_MS)) {
        s_words_due = false;
        s_words_at = lv_tick_get();
        char text[UI_SUBJECT_STRING_LENGTH];
        count_text(ps, text, sizeof(text));
        ui_set_str(&subject_walk_count, text);
        left_text(ps, text, sizeof(text), true);
        ui_set_str(&subject_walk_left, text); /* changes once a minute */
        ui_set_int(&subject_walk_warn, minutes_left(ps) <= WALK_WARN_MIN ? 1 : 0);
        text[0] = '\0';
        if (!s_from_here) {
            notice_text(ps, text, sizeof(text)); /* the whole test long */
        }
        ui_set_str(&subject_walk_from, text);
        ui_set_int(&subject_walk_from_on, s_from_here ? 0 : 1);
    }
    ui_bar_banner_update();
}

bool ui_walk_banner(void)
{
    if (!S.have_snap) {
        return false;
    }
    const alarm_partition_status_t *ps = walk_part(S.opt.partition);
    char text[96];
    if (ps->walk_test && s_pending) {
        /* TR: the status bar while someone is busy at the panel and a walk test started
         * elsewhere; Show opens it. At most 63 bytes */
        switch (started_from(ps)) {
        case FROM_PANEL_MANAGER:
            i18n_utf8_copy(text, sizeof(text), TR("Walk test from the Panel Manager · Show"));
            break;
        case FROM_KEYPAD:
            i18n_utf8_copy(text, sizeof(text), TR("Walk test from another keypad · Show"));
            break;
        case FROM_CONSOLE:
            i18n_utf8_copy(text, sizeof(text), TR("Walk test from the service console · Show"));
            break;
        default: i18n_utf8_copy(text, sizeof(text), TR("Walk test started remotely · Show")); break;
        }
    } else if (ps->walk_test) {
        const uint32_t m = minutes_left(ps);
        const bool card = lv_subject_get_int(&subject_view) == VIEW_HOME &&
                          lv_subject_get_int(&subject_panel) == PANEL_WALK;
        if (m > WALK_WARN_MIN || card || page_open()) {
            return false; /* Home's card or the page says it, with Extend beside it */
        }
        /* TR: the status bar in the walk test's last minutes, away from Home's card (WALK-004);
         * {0} is 1 to 5; Show opens the card with Extend. At most 63 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("Walk test ends in {0} min · Show", "Walk test ends in {0} min · Show", m),
                    (unsigned)m);
    } else if (has_summary(ps) && ps->walk_started_ms != s_summary_seen &&
               ps->state == ALARM_STATE_DISARMED && !page_open()) {
        /* TR: the status bar after a walk test, until the summary is seen or the partition arms
         * (WALK-010); {0} zones tested of {1}, each up to 3 bytes. At most 63 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("Walk test ended: {0} of {1} tested · Show",
                        "Walk test ended: {0} of {1} tested · Show", ps->walk_tested),
                    (unsigned)ps->walk_tested, (unsigned)ps->walk_total);
    } else {
        return false;
    }
    ui_set_str(&subject_walk_banner, text);
    return true;
}

void ui_walk_banner_pressed(void)
{
    if (!S.have_snap) {
        return;
    }
    const alarm_partition_status_t *ps = walk_part(S.opt.partition);
    if (ps->walk_test) {
        s_pending = false;
        ui_app_walk_show();
    } else if (has_summary(ps)) {
        /* Seen when the page shows it (ui_walk_build), which needs no code for it (WALK-010). */
        ui_settings_open();
        ui_settings_section(NOTR("walk"));
    }
    ui_bar_banner_update();
}

/* ---- who, and which partitions ------------------------------------------------------------- */

/* The partitions the code that opened Settings may test (WALK-001, 03: the installer and the
 * system master reach every partition, a partition master only its own). */
static uint8_t allowed_parts(void)
{
    uint8_t enabled = 0u;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if (S.cfg->partitions[i].enabled) {
            enabled = (uint8_t)(enabled | (1u << i));
        }
    }
    if (S.program || S.auth_user == 0u || S.auth_user > ALARM_MAX_USERS) {
        return enabled;
    }
    const alarm_user_t *u = &S.cfg->users[S.auth_user - 1u];
    if (u->authority == ALARM_AUTH_PARTITION_MASTER) {
        return (uint8_t)(enabled & u->partitions);
    }
    return enabled;
}

static unsigned bits(uint8_t v)
{
    unsigned n = 0;
    for (; v != 0u; v = (uint8_t)(v & (v - 1u))) {
        n++;
    }
    return n;
}

static const char *part_name(uint8_t p, char *buf, size_t len)
{
    if (S.cfg->partitions[p - 1u].name[0] != '\0') {
        i18n_utf8_copy(buf, len, S.cfg->partitions[p - 1u].name);
    } else {
        /* TR: the name of a partition nobody has named; {0} is its number */
        I18N_FORMAT(buf, len, TR_C("default partition name", "Partition {0}"), (unsigned)p);
    }
    return buf;
}

/* This keypad's partition when the code reaches it, else the first one it does. */
static void choose_part(void)
{
    const uint8_t mask = allowed_parts();
    if (s_part >= 1u && s_part <= ALARM_MAX_PARTITIONS && (mask & (1u << (s_part - 1u)))) {
        return;
    }
    s_part = S.opt.partition;
    if (mask & (1u << (S.opt.partition - 1u))) {
        return;
    }
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (mask & (1u << (p - 1u))) {
            s_part = p;
            return;
        }
    }
}

static uint8_t s_pick_part[ALARM_MAX_PARTITIONS];

static void part_chosen(int32_t index)
{
    if (index >= 0 && index < ALARM_MAX_PARTITIONS && s_pick_part[index] != 0u) {
        s_part = s_pick_part[index];
    }
    ui_page_rebuild();
}

static void pick_part(void *ctx)
{
    (void)ctx;
    static char names[ALARM_MAX_PARTITIONS][ALARM_NAME_LEN + 16];
    ui_choice_t choices[ALARM_MAX_PARTITIONS];
    const uint8_t mask = allowed_parts();
    size_t n = 0;
    int32_t selected = -1;
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!(mask & (1u << (p - 1u)))) {
            continue;
        }
        s_pick_part[n] = p;
        choices[n].glyph = UI_ICON_HOUSE;
        choices[n].tone = ui_tone_blue();
        choices[n].title = part_name(p, names[n], sizeof(names[n]));
        /* TR: the walk test's partition list, beside a partition that is testing now */
        choices[n].detail = walk_part(p)->walk_test ? TR("Walk test running") : NULL;
        if (p == s_part) {
            selected = (int32_t)n;
        }
        n++;
    }
    /* TR: the title of the walk test's partition list */
    ui_pick_sheet(TR("Which partition?"), NULL, choices, n, selected, part_chosen);
}

/* ---- the commands ------------------------------------------------------------------------- */

/* code "": an End the engine takes as the system's (WALK-025); anything else asks for one. The
 * answer goes to done (LINK-054); false: not sent (done gets nothing). */
static bool walk_command(uint8_t p, alarm_walk_op_t op, uint8_t flags, const char *code,
                         ui_call_done_fn done)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = p;
    cmd.walk_op = (uint8_t)op;
    cmd.walk_flags = flags;
    snprintf(cmd.code, sizeof(cmd.code), "%s", code);
    const bool queued = ui_call_command(&cmd, done, 0u);
    memset(cmd.code, 0, sizeof(cmd.code));
    return queued;
}

/* Why the panel said no (WALK-001, WALK-002), in words that say what to do. */
static const char *refusal(alarm_result_t r, alarm_walk_op_t op)
{
    if (r == ALARM_ERR_NOT_AUTHORIZED) {
        /* TR: a toast when a code may not start or extend a walk test. At most 127 bytes */
        return TR("Only the installer or a master code can do that");
    }
    if (r != ALARM_ERR_BAD_STATE) {
        return ui_result_text(r);
    }
    if (op != ALARM_WALK_START) {
        /* TR: a toast: Extend or End was pressed just as the walk test ended. At most 127 bytes */
        return TR("The walk test has already ended");
    }
    const alarm_partition_status_t *ps = walk_part(s_part);
    if (ps->walk_test) {
        /* TR: a toast when Start is pressed while a walk test runs. At most 127 bytes */
        return TR("A walk test is already running");
    }
    if (S.snap.program) {
        /* TR: a toast when Start is pressed in installer mode. At most 127 bytes */
        return TR("Leave installer mode first");
    }
    if (ps->state != ALARM_STATE_DISARMED) {
        /* TR: a toast when Start is pressed while the partition is armed. At most 127 bytes */
        return TR("Disarm first to start a walk test");
    }
    /* TR: a toast when Start is pressed while an alarm sounds. At most 127 bytes */
    return TR("Silence the alarm first");
}

static void started(const ui_call_t *c);

static void on_start(void *ctx)
{
    (void)ctx;
    s_own_start = true; /* the snapshot that shows it is this screen's start, not a notice */
    if (!walk_command(s_part, ALARM_WALK_START, s_life24 ? ALARM_WALK_F_LIFE24 : 0u, S.auth_code,
                      started)) {
        s_own_start = false;
        ui_beep(UI_BEEP_ERROR);
        ui_toast(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
}

static void started(const ui_call_t *c)
{
    const alarm_result_t r = c->r;
    if (r != ALARM_OK) {
        s_own_start = false;
        ui_beep(UI_BEEP_ERROR);
        if (s_after_installer) {
            if (!s_unsaved) { /* a "Not saved" toast stays: it says more */
                char text[128];
                /* TR: a toast when the installer menu's Walk test left installer mode and the
                 * start was then refused; {0} is the reason, a sentence (up to 90 bytes). At most
                 * 127 bytes */
                I18N_FORMAT(text, sizeof(text), TR("Installer mode ended · {0}"),
                            refusal(r, ALARM_WALK_START));
                ui_toast(text);
            }
        } else {
            ui_toast(refusal(r, ALARM_WALK_START));
        }
        return;
    }
    ui_refresh(true);
    s_own_start = false;
    if (s_part == S.opt.partition) {
        ui_set_int(&subject_view, VIEW_HOME); /* Home follows the test (WALK-013) */
    } else {
        ui_page_rebuild();
    }
}

/* Extend on the page opened without a code: the code comes with it (WALK-001), as on Home's pad,
 * and is used for this one command. */
static bool extend_with(const char *code);

static void extended_with(const ui_call_t *c)
{
    if (!ui_pad_asks(extend_with)) {
        ui_refresh(true);
        return; /* the pad closed meanwhile */
    }
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(c->r == ALARM_ERR_INVALID_CODE ? TR("Wrong code, try again")
                                                    : refusal(c->r, ALARM_WALK_EXTEND));
        return;
    }
    ui_pad_cancel();
    ui_refresh(true);
    ui_page_rebuild();
}

/* The pad stays open; the answer closes it (extended_with). */
static bool extend_with(const char *code)
{
    if (!walk_command(s_part, ALARM_WALK_EXTEND, 0u, code, extended_with)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

static void extended(const ui_call_t *c)
{
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(refusal(c->r, ALARM_WALK_EXTEND));
    }
    ui_refresh(true);
    ui_page_rebuild();
}

static void on_extend(void *ctx)
{
    (void)ctx;
    if (s_view) {
        /* TR: the code pad's hint when Extend is pressed on the walk test's page opened
         * without a code. At most 95 bytes */
        ui_pad_ask(HOST_SETTINGS, UI_ICON_CLOCK, TR("Extend walk test"),
                   TR("Enter a master or installer code"), ui_tone_blue(), extend_with);
        return;
    }
    if (!walk_command(s_part, ALARM_WALK_EXTEND, 0u, S.auth_code, extended)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
}

static void ended(const ui_call_t *c)
{
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(refusal(c->r, ALARM_WALK_END));
    }
    ui_refresh(true);
    if (page_open()) {
        ui_page_rebuild();
    }
}

static void end_confirmed(void)
{
    /* Never a toggle (WALK-002). With the page's code the log names who ended it; without one,
     * the panel ends it (WALK-025). */
    const char *code = s_end_coded && ui_settings_allowed(ACCESS_SHARED) ? S.auth_code : "";
    if (!walk_command(s_end_part, ALARM_WALK_END, 0u, code, ended)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
}

/* End asks instead of wanting a code (WALK-025): ending gives the protection back, so the
 * question only guards against a stray tap. */
static void ask_end(uint8_t p, bool coded)
{
    s_end_part = p;
    s_end_coded = coded;
    /* TR: the question when End is pressed during a walk test (Home's card or its page), its
     * explanation, and its two buttons. At most 127 bytes each */
    ui_confirm_choice(TR("End the walk test?"),
                      TR("The sensors stop beeping and work as usual again"),
                      TR("End test"), end_confirmed, TR("Keep testing"), NULL);
}

void ui_walk_end_ask(void)
{
    ask_end(S.opt.partition, false);
}

static void on_end(void *ctx)
{
    (void)ctx;
    ask_end(s_part, !s_view);
}

static void on_life24(bool on, void *ctx)
{
    (void)ctx;
    s_life24 = on;
}

/* ---- the list (WALK-009) ------------------------------------------------------------------ */

static bool in_part(uint16_t z)
{
    const alarm_zone_config_t *zc = &S.cfg->zones[z - 1u];
    return zc->type != ALARM_ZT_NOT_USED && zc->partition == s_part;
}

/* "2:16 PM": when the zone was first marked, on this keypad's clock; empty without the time. */
static void marked_at(const alarm_partition_status_t *ps, const alarm_zone_status_t *zs, char *buf,
                      size_t len)
{
    buf[0] = '\0';
    const int64_t epoch = S.be.epoch_ms(S.be.ctx);
    const alarm_ms_t now = S.be.now_ms(S.be.ctx);
    const alarm_ms_t at = ps->walk_started_ms + (alarm_ms_t)zs->walk_first_s * 1000u;
    if (epoch <= 0 || ps->walk_started_ms == 0u || at > now) {
        return;
    }
    ui_clock_text(epoch - (int64_t)(now - at), buf, len, true);
}

static void add_fact(char *line, size_t len, const char *fact)
{
    size_t n = strlen(line);
    if (n > 0u && n + 1u < len) {
        n += i18n_utf8_copy(line + n, len - n, " · "); /* independent facts (ui_internal.h) */
    }
    if (n + 1u < len) {
        i18n_utf8_copy(line + n, len - n, fact);
    }
}

static void zone_row(lv_obj_t *body, const alarm_partition_status_t *ps, uint16_t z, bool counted)
{
    const alarm_zone_status_t *zs = &S.snap.zone[z - 1u];
    const alarm_zone_config_t *zc = &S.cfg->zones[z - 1u];
    const bool tested = (zs->walk & ALARM_WALK_ZS_TESTED) != 0u;
    const bool cover = (zs->walk & ALARM_WALK_ZS_COVER) != 0u;
    const bool unreachable = (zs->flags & ALARM_ZS_SUPERVISION) != 0u;
    const bool fault = (zs->flags & ALARM_ZS_TROUBLE) != 0u;
    const bool open = (zs->flags & ALARM_ZS_FAULT) != 0u;
    char line[160] = "";
    char fact[64];

    if (unreachable) {
        /* TR: a zone in the walk test's list whose sensor doesn't answer (CHECK). At most 40 bytes */
        add_fact(line, sizeof(line), TR("Can't be reached"));
    } else if (fault) {
        add_fact(line, sizeof(line), TR_C("zone trouble", "Sensor fault"));
    }
    if (open) {
        /* TR: a zone in the walk test's list that is open (or sees motion) right now. At most 40
         * bytes */
        add_fact(line, sizeof(line), TR("Open now"));
    }
    if (tested) {
        char at[24];
        marked_at(ps, zs, at, sizeof(at));
        if (at[0] != '\0') {
            /* TR: a zone in the walk test's list: when it was first tested ({0} "2:16 PM").
             * At most 55 bytes */
            I18N_FORMAT(fact, sizeof(fact), TR("Tested {0}"), at);
        } else {
            /* TR: a zone in the walk test's list, tested (the panel doesn't know the time). At most
             * 40 bytes */
            i18n_utf8_copy(fact, sizeof(fact), TR("Tested"));
        }
        add_fact(line, sizeof(line), fact);
        if (zs->walk_trips > 1u) {
            /* TR: a zone in the walk test's list: how many times it beeped; {0} up to 3 bytes.
             * At most 60 bytes */
            I18N_FORMAT(fact, sizeof(fact), TRN("{0} time", "{0} times", zs->walk_trips),
                        (unsigned)zs->walk_trips);
            add_fact(line, sizeof(line), fact);
        }
        if (cover) {
            /* TR: a zone in the walk test's list: its cover (tamper) was tested as well. At most 40
             * bytes */
            add_fact(line, sizeof(line), TR("cover too"));
        }
    } else if (cover) {
        /* TR: a zone in the walk test's list: only its cover (tamper) was tested. At most 40 bytes */
        add_fact(line, sizeof(line), TR("Cover tested"));
    } else if (!counted) {
        const alarm_zone_type_t t = (alarm_zone_type_t)zc->type;
        /* TR: a zone in the walk test's list that stays on: a fire or CO zone, whose own test
         * button marks it tested, or a 24-hour zone left out of the test. At most 60 bytes */
        add_fact(line, sizeof(line), alarm_zone_type_is_24h(t) ? TR("Sets off a real alarm")
                                                               : TR("Its test button marks it"));
    } else if (line[0] == '\0') {
        /* TR: a zone in the walk test's list not tested yet. At most 40 bytes */
        add_fact(line, sizeof(line), TR("Not tested"));
    }

    const char *glyph;
    lv_color_t tone;
    if (unreachable || fault) {
        glyph = UI_ICON_TRIANGLE_ALERT;
        tone = ui_tone_amber(); /* the trouble colour */
    } else if (open) {
        glyph = ui_device_class_glyph((alarm_device_class_t)zc->device_class);
        tone = ui_tone_amber();
    } else if (tested) {
        glyph = UI_ICON_CHECK;
        tone = ui_tone_green();
    } else {
        glyph = ui_device_class_glyph((alarm_device_class_t)zc->device_class);
        tone = ui_tone_gray();
    }
    char name[ALARM_NAME_LEN + 8];
    char detail[200];
    /* TR: a zone's line: its number, then what it is or does ({1}) */
    I18N_FORMAT(detail, sizeof(detail), TR("Zone {0} · {1}"), (unsigned)z, line);
    ui_row_link(body, glyph, tone, ui_zone_name(S.cfg, z, name, sizeof(name)), detail, NULL, NULL,
                NULL, NULL);
}

static void list_zones(lv_obj_t *body, const alarm_partition_status_t *ps)
{
    /* Not tested first, then tested, then the zones that stay on (WALK-013, WALK-007). */
    for (int group = 0; group < 3; group++) {
        bool headed = false;
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (!in_part(z)) {
                continue;
            }
            const bool counted = alarm_zone_type_walk_counts(
                (alarm_zone_type_t)S.cfg->zones[z - 1u].type, ps->walk_flags);
            const bool tested = (S.snap.zone[z - 1u].walk & ALARM_WALK_ZS_TESTED) != 0u;
            const int g = !counted ? 2 : tested ? 1 : 0;
            if (g != group) {
                continue;
            }
            if (!headed) {
                headed = true;
                /* TR: the headings of the walk test's list */
                ui_section(body, group == 0   ? TR("Not tested")
                                 : group == 1 ? TR("Tested")
                                              : TR("Stay on during the test"));
            }
            zone_row(body, ps, z, counted);
        }
    }
}

/* What the page shows, in one number: it is built again only when this changes. */
static uint32_t signature(void)
{
    const alarm_partition_status_t *ps = walk_part(s_part);
    uint32_t h = 2166136261u;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIX(s_part);
    MIX(ps->walk_test);
    MIX(ps->walk_seq);
    MIX(ps->walk_tested);
    MIX(ps->walk_total);
    MIX(ps->state);
    MIX(S.snap.program);
    MIX(S.cfg_seq);
    MIX(ps->walk_test ? minutes_left(ps) : 0u);
    MIX(ps->walk_end_how);
    MIX(ps->walk_started_ms);
    MIX(ps->walk_ended_by);
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (in_part(z)) {
            MIX(S.snap.zone[z - 1u].flags &
                (ALARM_ZS_FAULT | ALARM_ZS_TROUBLE | ALARM_ZS_SUPERVISION));
        }
    }
#undef MIX
    return h;
}

/* ---- the summary (WALK-010) ---------------------------------------------------------------- */

/* "Last walk test: 3 of 5 tested · Ended by Pat": the count, then who ended it and how. */
static void summary_note(const alarm_partition_status_t *ps, char *note, size_t nlen)
{
    char count[64];
    char ended[96];
    char name[ALARM_NAME_LEN + 8];
    const bool who = ps->walk_ended_by != 0u;
    if (who) {
        ui_user_name(S.cfg, ps->walk_ended_by, name, sizeof(name));
    }
    /* TR: Settings > Walk test, under the title after a test: {0} zones tested of {1} (each up
     * to 3 bytes). At most 60 bytes */
    I18N_FORMAT(count, sizeof(count),
                TRN("Last walk test: {0} of {1} tested", "Last walk test: {0} of {1} tested",
                    ps->walk_tested),
                (unsigned)ps->walk_tested, (unsigned)ps->walk_total);
    /* TR: Settings > Walk test, after the count of the last test: how it ended; {0} is a user's
     * name, up to 31 bytes. At most 60 bytes */
    switch ((alarm_walk_end_t)ps->walk_end_how) {
    case ALARM_WALK_END_TIME_LIMIT:
        i18n_utf8_copy(ended, sizeof(ended), TR("Ended when its time ran out"));
        break;
    case ALARM_WALK_END_DISARM:
        if (who) {
            I18N_FORMAT(ended, sizeof(ended), TR("Ended when {0} disarmed"), name);
        } else {
            i18n_utf8_copy(ended, sizeof(ended), TR("Ended by a disarm"));
        }
        break;
    default:
        if (who) {
            I18N_FORMAT(ended, sizeof(ended), TR("Ended by {0}"), name);
        } else {
            i18n_utf8_copy(ended, sizeof(ended), TR("Ended before its time ran out"));
        }
        break;
    }
    snprintf(note, nlen, "%s · %s", count, ended); /* independent facts (ui_internal.h) */
}

/* ---- the page ----------------------------------------------------------------------------- */

bool ui_walk_viewable(void)
{
    if (!S.have_snap) {
        return false;
    }
    const alarm_partition_status_t *ps = walk_part(S.opt.partition);
    return ps->walk_test || has_summary(ps);
}

void ui_walk_build(lv_obj_t *body, char *note, size_t nlen)
{
    if (S.page_fresh) {
        s_part = 0u; /* this keypad's partition again, and the option off (WALK-007) */
        s_life24 = false;
    }
    /* Without a code (ui_walk_viewable): this keypad's partition, to follow and to end (WALK-009,
     * WALK-025); Extend and Start ask for the code that may (WALK-001). */
    s_view = !ui_settings_allowed(ACCESS_SHARED);
    if (s_view) {
        s_part = S.opt.partition;
    } else {
        choose_part();
    }
    s_sig = signature();
    s_built_at = lv_tick_get();
    if (!s_view && (s_part < 1u || s_part > ALARM_MAX_PARTITIONS ||
                    !(allowed_parts() & (1u << (s_part - 1u))))) {
        /* TR: Settings > Walk test, under the title, when the code reaches no partition. At most
         * 127 bytes */
        i18n_utf8_copy(note, nlen, TR("This code has no partition to test"));
        return;
    }
    const alarm_partition_status_t *ps = walk_part(s_part);
    char text[96];
    if (!s_view && bits(allowed_parts()) > 1u) {
        char name[ALARM_NAME_LEN + 16];
        /* TR: Settings > Walk test, the row that picks the partition */
        ui_row_link(body, UI_ICON_HOUSE, ui_tone_blue(), TR("Partition"), NULL,
                    part_name(s_part, name, sizeof(name)), NULL, pick_part, NULL);
    }

    if (!ps->walk_test) {
        const bool summary = has_summary(ps);
        if (summary) {
            summary_note(ps, note, nlen);
            if (s_part == S.opt.partition) {
                s_summary_seen = ps->walk_started_ms; /* the status bar's banner goes */
            }
        } else {
            /* TR: Settings > Walk test, under the title, before a test. At most 127 bytes */
            i18n_utf8_copy(note, nlen, TR("Open and close every door and window, walk past every "
                                          "motion sensor: each one beeps"));
        }
        if (s_view) {
            /* The summary was shown without a code; a new test needs one (WALK-001). */
            ui_paragraph(body, TR("A master or installer code starts a walk test"), ui_tone_gray());
            ui_settings_unlock_row(body, ACCESS_SHARED, TR("Walk test"));
            if (summary) {
                list_zones(body, ps);
            }
            return;
        }
        /* TR: Settings > Walk test, the start's option and its explanation */
        ui_row_toggle(body, TR("Include panic and 24-hour zones"),
                      TR("They beep instead of alarming"), s_life24, on_life24, NULL);
        const uint32_t m = (S.cfg->opt.walk_test_timeout_s + 59u) / 60u;
        /* TR: Settings > Walk test, under the option: {0} is the time limit in minutes (up to 4
         * bytes). At most 91 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("The siren sounds for 1 s at the start · ends after {0} min",
                        "The siren sounds for 1 s at the start · ends after {0} min", m),
                    (unsigned)m);
        ui_paragraph(body, text, ui_tone_gray());
        /* TR: Settings > Walk test, the button that starts it */
        ui_row_action(body, UI_ICON_FOOTPRINTS, TR("Start walk test"), ui_tone_blue(), on_start,
                      NULL);
        if (summary) {
            list_zones(body, ps); /* the last test's list: the untested ones first (WALK-010) */
        }
        return;
    }

    char count[48];
    char left[48];
    count_text(ps, count, sizeof(count));
    left_text(ps, left, sizeof(left), false);
    snprintf(note, nlen, "%s · %s", count, left); /* independent facts (ui_internal.h) */
    if (s_part == S.opt.partition && !s_from_here) {
        char from[UI_SUBJECT_STRING_LENGTH];
        notice_text(ps, from, sizeof(from)); /* WALK-023: heads the test wherever it shows */
        ui_paragraph(body, from, ui_tone_blue());
    }
    const uint32_t m = minutes_left(ps);
    if (m <= WALK_WARN_MIN) {
        warn_text(m, text, sizeof(text)); /* WALK-004: steady, with Extend just below */
        ui_paragraph(body, text, ui_tone_amber());
    }
    if (ps->walk_flags & ALARM_WALK_F_LIFE24) {
        /* TR: Settings > Walk test while a test runs with panic and 24-hour zones included */
        ui_paragraph(body, TR("Panic and 24-hour zones beep instead of alarming"), ui_tone_amber());
    }
    /* TR: Settings > Walk test: Extend starts the time limit again */
    ui_row_action(body, UI_ICON_CLOCK, TR("Extend"), ui_tone_blue(), on_extend, NULL);
    /* TR: Settings > Walk test: ends the test */
    ui_row_action(body, UI_ICON_CHECK, TR("End test"), ui_tone_green(), on_end, NULL);
    list_zones(body, ps);
}

/* The open page follows the test: at most once a second, and not under a finger, a sheet or a
 * code pad (UI-020, UI-023). */
void ui_walk_tick(void)
{
    const bool coded = ui_settings_allowed(ACCESS_SHARED);
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS || S.depth == 0u ||
        S.pages[S.depth - 1u].id != PAGE_WALK || (!coded && !s_view) ||
        lv_subject_get_int(&subject_settings_pad) != 0 ||
        lv_subject_get_int(&subject_sheet) != SHEET_NONE ||
        lv_tick_elaps(s_built_at) < WALK_REBUILD_MS ||
        lv_display_get_inactive_time(S.disp) < 300u) {
        return;
    }
    if (!coded && !ui_walk_viewable()) {
        s_view = false; /* nothing left to show without a code: the page locks */
        ui_page_rebuild();
    } else if (signature() != s_sig) {
        ui_page_rebuild();
    }
}

/* ---- Installer > Walk test ---------------------------------------------------------------- */

/* The engine refuses a start in installer mode (CORE-PROG-002/003): the installer menu's Walk
 * test leaves the mode first and starts it on this keypad's partition in the same tap; with
 * several partitions to choose from, Settings > Walk test opens with the partition to pick. The
 * code that opened installer mode carries on (ui_settings_remember); after the power-up recovery,
 * which had none, the page asks for one. */
void ui_walk_from_installer(bool unsaved)
{
    ui_settings_section(NOTR("walk")); /* installer mode is left by now */
    if (!page_open() || !ui_settings_allowed(ACCESS_SHARED) || bits(allowed_parts()) != 1u ||
        s_part < 1u || s_part > ALARM_MAX_PARTITIONS || walk_part(s_part)->walk_test) {
        return; /* the page: a code, a partition to pick, or the test already running */
    }
    s_after_installer = true; /* a refusal says that installer mode has ended */
    s_unsaved = unsaved;
    on_start(NULL);
    s_after_installer = false;
    s_unsaved = false;
}
