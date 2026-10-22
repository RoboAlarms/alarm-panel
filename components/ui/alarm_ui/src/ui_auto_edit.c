/*
 * ui_auto_edit.c - Settings > Automations' editors (docs/features/41, BL-168; ui_automations.h,
 * part B): a new automation's steps, changing its trigger, window, actions and "Turn it off
 * after", and the delete question; phase C (BL-170): "The alarm" first in "What should it do?",
 * "Arm the panel" after any trigger (D23) as the automation's only action, on its own sheet
 * (form_sheet: the level, the partition, the warning, "Bypass open zones" and what it will do;
 * AUT-011 to AUT-014, D29), and "Disarm the panel" after any trigger but an alarm (D22), its
 * partition, and its confirm.
 *
 * BL-224 (DESIGN.md section 15, drawings bl-224-*): ideas that need no device, for the installer
 * too (whose Arm acts as the system master: auto_store makes the maker; the installer never
 * disarms); "When there's a trouble" and "Which trouble?"; "What should it do?" in sections - The
 * alarm (Arm, Disarm, Remind to arm and "Tell me if it's still armed" after a time only, Door
 * chime), Tell someone (Send a notification), Outputs (the wired outputs "For automations"),
 * Devices - with a quiet note while no device is connected; and those actions' sheets.
 *
 * Every editor works on a draft (E.rule) and stores nothing before its last pick: the draft goes
 * to ui_auto_save once, and Cancel at any step leaves the store as it was (AUT-001). The pickers
 * offer only what hc_auto_offer gives for the device's live kind and source, with the installer
 * option read when each sheet is built (AUT-002, AUT-015). The confirms are asked before the
 * save: a scene's or a switch's once when picked (D8, D9), and a less secure action after a
 * door, window, motion or alarm trigger - or an unlock, open, stop or disarm after an armed
 * trigger (decided 2026-10-05) - with the sentence that says what will happen (D24, AUT-016),
 * asked again whenever that trigger or that action changes. A refused save is ui_auto_save's to tell (beep
 * and toast); the editor then stays where it was, or goes back to the list when the store
 * changed underneath it.
 *
 * Everything runs on the LVGL thread. Nothing here moves or counts down (UI-020).
 */
#include <stdio.h>
#include <string.h>

#include "home_control/home_control.h"
#include "ui_backend/ui_dev_model.h"
#include "ui_automations.h"

#if ALARM_UI_AUTOMATIONS

typedef enum {
    ED_ADD = 1,  /* a new automation */
    ED_WHEN,     /* its trigger */
    ED_WINDOW,   /* its "Only between" */
    ED_ACTION,   /* one of its actions */
    ED_SECOND,   /* a second action */
    ED_OFF_AFTER,
    ED_DELETE,
} ed_mode_t;

/* The ideas a new automation starts from (DESIGN.md section 9, screen 7). */
typedef enum {
    IDEA_NONE = 0,
    IDEA_DOOR_LIGHT,  /* a door or window, then a light: 6:00 PM to 6:00 AM, 10 minutes */
    IDEA_AWAY_LOCK,   /* a lock, when armed Away */
    IDEA_ALARM_LIGHT, /* a light, when an alarm sounds */
    IDEA_LEAK_VALVE,  /* a water sensor, then a valve */
    IDEA_SCRATCH,
    IDEA_BEDTIME,     /* Arm at bedtime: a time (10:30 PM every day), then the arm sheet, Night */
    /* BL-224: ideas that need no device (drawings bl-224-ideas, bl-224-ideas-installer) */
    IDEA_MORNING,     /* Arm Away on weekday mornings: 8:30 AM on weekdays, the arm sheet, Away */
    IDEA_REMIND,      /* Remind me to arm: 11:00 PM every day, then the reminder's sheet */
    IDEA_STILL_ARMED, /* Tell me if it's still armed: 4:00 PM on weekdays, then its sheet */
    IDEA_OPENING,     /* Disarm at opening time: 9:00 AM on weekdays, then the Disarm */
    IDEA_DOOR_NOTIFY, /* a door or window, 10:00 PM to 6:00 AM, then Send a notification */
    IDEA_CHIME_NIGHT, /* Quiet the door chime at night: 10:00 PM every day, the chime off */
    IDEA_OUTPUT_ALARM, /* an alarm sounds: an output for automations on, for 30 minutes */
} idea_t;

/* Which zones the zone sheet lists. */
typedef enum {
    ZF_OPENS = 0, /* "opens": doors, windows, motion and water sensors (auto_zone_usable) */
    ZF_CLOSES,    /* "closes": doors, windows and garage doors */
    ZF_DOOR,      /* an idea's door or window ("opens", by its class) */
    ZF_WATER,     /* an idea's water sensor */
} zone_filter_t;

#define ED_ROWS   (ALARM_MAX_ZONES + 4u) /* the longest sheet: every zone (devices: UI_DEVL_MAX) */
#define ROW_NONE  (-1)                   /* a dimmed row that says there is nothing to pick */
#define ROW_OTHER (-2)                   /* "Choose another device" */
#define ROW_DROP  (-3)                   /* "Remove this action" */
#define ROW_DISARM (-4)                  /* "Disarm the panel" (BL-170, D23) */
#define ROW_ARM   (-5)                   /* "Arm the panel" (BL-170, D23) */
#define ROW_HEADING (-6)                 /* a heading ("The alarm", "Devices"): never picked */
/* BL-224's actions (drawing bl-224-then-time) */
#define ROW_REMIND (-7)                  /* "Remind to arm" */
#define ROW_CHECK  (-8)                  /* "Tell me if it's still armed" */
#define ROW_CHIME  (-9)                  /* "Door chime" */
#define ROW_NOTIFY (-10)                 /* "Send a notification" */
#define ROW_OUTPUT(n) (-20 - (int32_t)(n)) /* wired output n "For automations" (1..AUTO_OUTPUTS) */
#define ROW_IS_OUTPUT(m) ((m) <= ROW_OUTPUT(1) && (m) >= ROW_OUTPUT(AUTO_OUTPUTS))
#define ROW_OUTPUT_OF(m) ((uint8_t)(-20 - (m)))

#define MIN_6PM 1080
#define MIN_6AM 360
#define MIN_11PM 1380
#define MIN_1030PM 1350
#define MIN_10PM 1320
#define MIN_830AM 510
#define MIN_9AM 540
#define MIN_4PM 960
#define OUTPUT_IDEA_MIN 30u /* "Switch an output when an alarm sounds": for 30 minutes */
#define WARN_MIN_DEFAULT 5u /* AUT-012: 1 to 15 minutes, 5 unless changed */
#define WARN_MIN_MAX 15u
#define DAYS_WEEKDAYS 0x1Fu /* Monday .. Friday */
#define DAYS_WEEKENDS 0x60u /* Saturday, Sunday */

_Static_assert(ED_ROWS >= UI_DEVL_MAX + 4u, "a sheet's rows hold every device, after The alarm's");

static UI_SLOW_BSS struct {
    bool active;          /* an editor is open: a late pick from a dropped one does nothing */
    uint8_t mode;         /* ed_mode_t */
    uint8_t idea;         /* idea_t while adding */
    uint8_t slot;         /* the automation changed (0 while adding) */
    uint8_t index;        /* the action changed or added */
    uint8_t asking;       /* the action whose confirm is open */
    uint8_t dev_filter;   /* an idea's device kind; DEV_NONE: any */
    uint8_t idea_action;  /* an idea's action (dev_action_t); ACT_NONE: the person picks */
    uint16_t gen;         /* the store's generation when the editor read it */
    auto_rule_t rule;     /* the draft */
    auto_trigger_t when;  /* the trigger being picked */
    auto_action_t act;    /* the action being picked */
    bool ask_dev[AUTO_ACTIONS];  /* the scene's or switch's confirm still to ask (D8, D9) */
    bool ask_trig[AUTO_ACTIONS]; /* the trigger's confirm still to ask (D24, AUT-016) */
    ui_choice_t choice[ED_ROWS];
    int32_t map[ED_ROWS]; /* what each row stands for: a zone, a partition, a device, an offer */
    char title[ED_ROWS][ALARM_NAME_LEN + 16];
    char detail[ED_ROWS][32];
    char sheet_title[128];
    char note[UI_SHEET_NOTE_LENGTH];
    ui_dev_t devs[UI_DEVL_MAX];
    size_t ndev;
    hc_auto_offer_t offer[HC_AUTO_OFFER_MAX];
    size_t noffer;
    auto_rule_t list[AUTO_MAX];
    /* The arm sheet (its draft is E.act): the parts it rewrites as its choices change */
    lv_obj_t *arm_levels;
    lv_obj_t *arm_away;   /* "Inside sensors will be on.", with Away only */
    lv_obj_t *arm_warn;   /* the warning's value */
    lv_obj_t *arm_bypass; /* the switch row; NULL while the installer option is off */
    lv_obj_t *arm_result;
    char arm_part[ALARM_NAME_LEN + 16];
    char arm_text[192];
    /* BL-224: the outputs for automations, and the sheets of the actions without devices (their
     * draft is E.act too) */
    ui_auto_output_t outs[AUTO_OUTPUTS];
    size_t nout;
    lv_obj_t *form_result;  /* the sheet's result line */
    void (*form_reopen)(void); /* the sheet its partition row goes back to */
    char form_text[192];
} E;

/* ---- reading ------------------------------------------------------------------------------- */

static bool have_store(void)
{
    return S.be.auto_list != NULL && S.be.auto_save != NULL && S.cfg != NULL;
}

static uint16_t store_gen(void)
{
    return S.be.auto_gen != NULL ? S.be.auto_gen(S.be.ctx) : 0u;
}

/* The installer option, read when a sheet is built (never from a tick: on the panel it asks the
 * alarm task). */
static bool less_secure_on(void)
{
    return S.be.auto_less_secure != NULL && S.be.auto_less_secure(S.be.ctx);
}

/* An editor starts: the store's generation, and the automation in this slot as its draft. */
static bool begin(ed_mode_t mode, uint8_t slot)
{
    if (!have_store()) {
        return false;
    }
    E.active = false;
    E.mode = (uint8_t)mode;
    E.idea = IDEA_NONE;
    E.slot = slot;
    E.index = 0;
    E.dev_filter = DEV_NONE;
    E.idea_action = ACT_NONE;
    memset(E.ask_dev, 0, sizeof(E.ask_dev));
    memset(E.ask_trig, 0, sizeof(E.ask_trig));
    memset(&E.rule, 0, sizeof(E.rule));
    memset(&E.when, 0, sizeof(E.when));
    memset(&E.act, 0, sizeof(E.act));
    E.gen = ui_auto_page_gen(); /* the one ui_auto_save sends (AUT-008) */
    if (slot != 0u) {
        const size_t n = S.be.auto_list(S.be.ctx, E.list, AUTO_MAX);
        bool found = false;
        for (size_t i = 0; i < n && !found; i++) {
            if (E.list[i].slot == slot) {
                E.rule = E.list[i];
                found = true;
            }
        }
        /* One from newer software, or one that can't be read, is only deleted (AUT-008) */
        if (!found || E.rule.rec == AUTO_REC_NEWER || E.rule.rec == AUTO_REC_BAD) {
            return false;
        }
    }
    E.active = true;
    return true;
}

static bool on_page(page_id_t id)
{
    return S.depth > 0u && S.pages[S.depth - 1u].id == id;
}

/* Stored: the page shows it (the list, or the automation's page). */
static void saved(void)
{
    E.active = false;
    if (on_page(PAGE_AUTOMATIONS) || on_page(PAGE_AUTOMATION)) {
        ui_page_rebuild();
    }
}

/* Refused: ui_auto_save beeped and said why. The editor stays where it was - unless the store
 * changed underneath it (CHANGED: it sent the generation its page was built from, E.gen), when
 * the list is read again. */
static void refused(void)
{
    E.active = false;
    if (store_gen() == E.gen) {
        return;
    }
    if (on_page(PAGE_AUTOMATION)) {
        ui_page_back();
    } else if (on_page(PAGE_AUTOMATIONS)) {
        ui_page_rebuild();
    }
}

static void save(void)
{
    /* D31: while the installer option is off nothing bypasses, and the validator refuses a
     * stored "Bypass open zones" (LESS_SECURE_OFF): any change - its time, its window, the
     * arm sheet - saves an Arm without it */
    if (!less_secure_on()) {
        for (unsigned i = 0; i < E.rule.n && i < AUTO_ACTIONS; i++) {
            if (E.rule.then[i].kind == AUTO_ACT_ARM) {
                E.rule.then[i].bypass_open = 0u;
            }
        }
    }
    uint16_t uid = 0;
    if (ui_auto_save(&E.rule, &uid)) {
        saved();
    } else {
        refused();
    }
}

static uint8_t partitions_enabled(void)
{
    uint8_t mask = 0u;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (S.cfg->partitions[p].enabled) {
            mask = (uint8_t)(mask | (1u << p));
        }
    }
    return mask;
}

static unsigned bit_count(unsigned v)
{
    unsigned n = 0;
    for (; v != 0u; v &= v - 1u) {
        n++;
    }
    return n;
}

static bool zone_listed(uint16_t zone, zone_filter_t f)
{
    const alarm_zone_config_t *z = &S.cfg->zones[zone - 1u];
    if (z->type == 0) {
        return false;
    }
    const bool closes = f == ZF_CLOSES;
    if (!auto_zone_usable((uint8_t)z->type, z->device_class, closes)) {
        return false;
    }
    if (f == ZF_DOOR) {
        return z->device_class == ALARM_DEVCLASS_DOOR || z->device_class == ALARM_DEVCLASS_WINDOW ||
               z->device_class == ALARM_DEVCLASS_GARAGE_DOOR;
    }
    if (f == ZF_WATER) {
        return z->device_class == ALARM_DEVCLASS_WATER;
    }
    return true;
}

static bool any_zone(zone_filter_t f)
{
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (zone_listed(z, f)) {
            return true;
        }
    }
    return false;
}

static void read_devices(void)
{
    E.ndev = S.be.dev_list != NULL ? S.be.dev_list(S.be.ctx, E.devs, UI_DEVL_MAX) : 0u;
}

/* The wired outputs "For automations" now (BL-224). */
static void read_outputs(void)
{
    E.nout = ui_auto_outputs(E.outs, AUTO_OUTPUTS);
}

/* The rows an automation may use for this device now (AUT-002, AUT-015); none for a script. */
static size_t offers_for(uint8_t kind, uint8_t source, hc_auto_offer_t out[HC_AUTO_OFFER_MAX],
                         bool less_secure)
{
    if (kind == DEV_SCRIPT || kind == DEV_NONE || kind >= DEV_KIND_COUNT) {
        return 0u;
    }
    return hc_auto_offer((dev_kind_t)kind, source, less_secure, out);
}

static bool device_offers(const ui_dev_t *d, uint8_t action, bool less_secure)
{
    hc_auto_offer_t o[HC_AUTO_OFFER_MAX];
    const size_t n = offers_for(d->kind, d->source, o, less_secure);
    for (size_t i = 0; i < n; i++) {
        if (action == ACT_NONE || o[i].action == action) {
            return true;
        }
    }
    return false;
}

static bool any_device(uint8_t kind, uint8_t action, bool less_secure)
{
    for (size_t i = 0; i < E.ndev; i++) {
        if (E.devs[i].kind == kind && device_offers(&E.devs[i], action, less_secure)) {
            return true;
        }
    }
    return false;
}

/* ---- the confirms (D8, D9, D24, AUT-002, AUT-016) ------------------------------------------ */

/* The trigger's confirm: what the validator asks (a less secure action after a door, window,
 * motion or alarm trigger, or an unlock, open or stop after an armed trigger), and a disarm after
 * an armed trigger of another partition (decided 2026-10-05: an unlock, open, stop or disarm
 * after an armed trigger asks the plain confirm). */
static bool trigger_asks(uint8_t trig, const auto_action_t *a)
{
    if (auto_action_needs_trigger_confirm(trig, a)) {
        return true;
    }
    return trig == AUTO_TRIG_ARMED && a->kind == AUTO_ACT_DISARM;
}

/* The sentences, by what starts it and what it does: whole sentences, {0} the zone, {1} the
 * device or the partition. */
enum { TG_OPENS = 0, TG_MOTION, TG_WATER, TG_TRIGGERED, TG_CLOSES, TG_ALARM, TG_FIRE, TG_TABLE };
#define TG_ARMED TG_TABLE /* the armed trigger's sentences are tables of their own */
enum { TA_UNLOCK = 0, TA_OPEN, TA_STOP, TA_DISARM, TA_COUNT };

/* TR: Settings > Automations, the confirm's title before an automation that unlocks, opens, stops
 * or disarms something by itself is kept ({0} the zone that starts it, {1} the device, or the
 * partition it disarms). At most 127 bytes */
static const char *const CONFIRM_TITLE[TG_TABLE][TA_COUNT] = {
    {TR_NOOP("Unlock {1} when {0} opens?"), TR_NOOP("Open {1} when {0} opens?"),
     TR_NOOP("Stop {1} when {0} opens?"), TR_NOOP("Disarm {1} when {0} opens?")},
    {TR_NOOP("Unlock {1} when {0} sees motion?"), TR_NOOP("Open {1} when {0} sees motion?"),
     TR_NOOP("Stop {1} when {0} sees motion?"), TR_NOOP("Disarm {1} when {0} sees motion?")},
    {TR_NOOP("Unlock {1} when {0} detects water?"), TR_NOOP("Open {1} when {0} detects water?"),
     TR_NOOP("Stop {1} when {0} detects water?"), TR_NOOP("Disarm {1} when {0} detects water?")},
    {TR_NOOP("Unlock {1} when {0} is triggered?"), TR_NOOP("Open {1} when {0} is triggered?"),
     TR_NOOP("Stop {1} when {0} is triggered?"), TR_NOOP("Disarm {1} when {0} is triggered?")},
    {TR_NOOP("Unlock {1} when {0} closes?"), TR_NOOP("Open {1} when {0} closes?"),
     TR_NOOP("Stop {1} when {0} closes?"), TR_NOOP("Disarm {1} when {0} closes?")},
    {TR_NOOP("Unlock {1} when an alarm sounds?"), TR_NOOP("Open {1} when an alarm sounds?"),
     TR_NOOP("Stop {1} when an alarm sounds?"), NULL},
    {TR_NOOP("Unlock {1} when a fire or CO alarm sounds?"),
     TR_NOOP("Open {1} when a fire or CO alarm sounds?"),
     TR_NOOP("Stop {1} when a fire or CO alarm sounds?"), NULL},
};

/* TR: Settings > Automations, the confirm's sentence: what the automation will do and who can make
 * it happen ({0} the zone, {1} the device, or the partition it disarms). At most 191 bytes */
static const char *const CONFIRM_NOTE[TG_TABLE][TA_COUNT] = {
    {TR_NOOP("Anyone who opens {0} will unlock {1}, even when the alarm is on."),
     TR_NOOP("Anyone who opens {0} will open {1}, even when the alarm is on."),
     TR_NOOP("Anyone who opens {0} will stop {1}, even when the alarm is on."),
     TR_NOOP("Anyone who opens {0} will disarm {1}.")},
    {TR_NOOP("Anyone who walks past {0} will unlock {1}, even when the alarm is on."),
     TR_NOOP("Anyone who walks past {0} will open {1}, even when the alarm is on."),
     TR_NOOP("Anyone who walks past {0} will stop {1}, even when the alarm is on."),
     TR_NOOP("Anyone who walks past {0} will disarm {1}.")},
    {TR_NOOP("Water at {0} will unlock {1}, even when the alarm is on."),
     TR_NOOP("Water at {0} will open {1}, even when the alarm is on."),
     TR_NOOP("Water at {0} will stop {1}, even when the alarm is on."),
     TR_NOOP("Water at {0} will disarm {1}.")},
    {TR_NOOP("Anything that triggers {0} will unlock {1}, even when the alarm is on."),
     TR_NOOP("Anything that triggers {0} will open {1}, even when the alarm is on."),
     TR_NOOP("Anything that triggers {0} will stop {1}, even when the alarm is on."),
     TR_NOOP("Anything that triggers {0} will disarm {1}.")},
    {TR_NOOP("Anyone who closes {0} will unlock {1}, even when the alarm is on."),
     TR_NOOP("Anyone who closes {0} will open {1}, even when the alarm is on."),
     TR_NOOP("Anyone who closes {0} will stop {1}, even when the alarm is on."),
     TR_NOOP("Anyone who closes {0} will disarm {1}.")},
    {TR_NOOP("Whenever an alarm sounds, it will unlock {1}, even a false alarm."),
     TR_NOOP("Whenever an alarm sounds, it will open {1}, even a false alarm."),
     TR_NOOP("Whenever an alarm sounds, it will stop {1}, even a false alarm."), NULL},
    {TR_NOOP("Whenever a fire or CO alarm sounds, it will unlock {1}."),
     TR_NOOP("Whenever a fire or CO alarm sounds, it will open {1}."),
     TR_NOOP("Whenever a fire or CO alarm sounds, it will stop {1}."), NULL},
};

/* A Disarm with one partition, which has no name in the sentences ("the panel"). */
/* TR: Settings > Automations, the confirm's title before an automation that disarms by itself is
 * kept, with one partition ({0} the zone that starts it). At most 127 bytes */
static const char *const DISARM_PANEL_TITLE[TG_ALARM] = {
    TR_NOOP("Disarm the panel when {0} opens?"), TR_NOOP("Disarm the panel when {0} sees motion?"),
    TR_NOOP("Disarm the panel when {0} detects water?"),
    TR_NOOP("Disarm the panel when {0} is triggered?"), TR_NOOP("Disarm the panel when {0} closes?"),
};
/* TR: Settings > Automations, the confirm's sentence for an automation that disarms by itself,
 * with one partition ({0} the zone). At most 191 bytes */
static const char *const DISARM_PANEL_NOTE[TG_ALARM] = {
    TR_NOOP("Anyone who opens {0} will disarm the panel."),
    TR_NOOP("Anyone who walks past {0} will disarm the panel."),
    TR_NOOP("Water at {0} will disarm the panel."),
    TR_NOOP("Anything that triggers {0} will disarm the panel."),
    TR_NOOP("Anyone who closes {0} will disarm the panel."),
};

/* After an armed trigger: the level it watches (Away, Stay, Night, any) and whether it names its
 * partition ({0}; else "the panel"). The example of 2026-10-05: "Every time Home is armed
 * Away, Garage Door will open". */
enum { AL_AWAY = 0, AL_STAY, AL_NIGHT, AL_ANY, AL_COUNT };

/* TR: Settings > Automations, the confirm's title before an automation that unlocks, opens or
 * stops something every time the panel is armed is kept ({1} the device). At most 127 bytes */
static const char *const ARMED_TITLE[TA_DISARM] = {
    TR_NOOP("Unlock {1} when the panel is armed?"),
    TR_NOOP("Open {1} when the panel is armed?"),
    TR_NOOP("Stop {1} when the panel is armed?"),
};
/* TR: Settings > Automations, the confirm's title before an automation that unlocks, opens or
 * stops something every time one partition is armed is kept ({0} the partition, {1} the device).
 * At most 127 bytes */
static const char *const ARMED_TITLE_NAMED[TA_DISARM] = {
    TR_NOOP("Unlock {1} when {0} is armed?"),
    TR_NOOP("Open {1} when {0} is armed?"),
    TR_NOOP("Stop {1} when {0} is armed?"),
};

/* TR: Settings > Automations, the confirm's sentence after "When the panel is armed" ({1} the
 * device). At most 191 bytes */
static const char *const ARMED_NOTE_PANEL[AL_COUNT][TA_DISARM] = {
    {TR_NOOP("Every time the panel is armed Away, {1} will unlock."),
     TR_NOOP("Every time the panel is armed Away, {1} will open."),
     TR_NOOP("Every time the panel is armed Away, {1} will stop.")},
    {TR_NOOP("Every time the panel is armed Stay, {1} will unlock."),
     TR_NOOP("Every time the panel is armed Stay, {1} will open."),
     TR_NOOP("Every time the panel is armed Stay, {1} will stop.")},
    {TR_NOOP("Every time the panel is armed Night, {1} will unlock."),
     TR_NOOP("Every time the panel is armed Night, {1} will open."),
     TR_NOOP("Every time the panel is armed Night, {1} will stop.")},
    {TR_NOOP("Every time the panel is armed, {1} will unlock."),
     TR_NOOP("Every time the panel is armed, {1} will open."),
     TR_NOOP("Every time the panel is armed, {1} will stop.")},
};

/* TR: Settings > Automations, the confirm's sentence after "When {0} is armed" ({0} the
 * partition, {1} the device, or for Disarm the other partition). At most 191 bytes */
static const char *const ARMED_NOTE_NAMED[AL_COUNT][TA_COUNT] = {
    {TR_NOOP("Every time {0} is armed Away, {1} will unlock."),
     TR_NOOP("Every time {0} is armed Away, {1} will open."),
     TR_NOOP("Every time {0} is armed Away, {1} will stop."),
     TR_NOOP("Every time {0} is armed Away, it will disarm {1}.")},
    {TR_NOOP("Every time {0} is armed Stay, {1} will unlock."),
     TR_NOOP("Every time {0} is armed Stay, {1} will open."),
     TR_NOOP("Every time {0} is armed Stay, {1} will stop."),
     TR_NOOP("Every time {0} is armed Stay, it will disarm {1}.")},
    {TR_NOOP("Every time {0} is armed Night, {1} will unlock."),
     TR_NOOP("Every time {0} is armed Night, {1} will open."),
     TR_NOOP("Every time {0} is armed Night, {1} will stop."),
     TR_NOOP("Every time {0} is armed Night, it will disarm {1}.")},
    {TR_NOOP("Every time {0} is armed, {1} will unlock."),
     TR_NOOP("Every time {0} is armed, {1} will open."),
     TR_NOOP("Every time {0} is armed, {1} will stop."),
     TR_NOOP("Every time {0} is armed, it will disarm {1}.")},
};

static int trigger_group(const auto_trigger_t *w)
{
    switch (w->kind) {
    case AUTO_TRIG_ALARM: return TG_ALARM;
    case AUTO_TRIG_FIRE: return TG_FIRE;
    case AUTO_TRIG_ARMED: return TG_ARMED;
    case AUTO_TRIG_ZONE_CLOSED: return TG_CLOSES;
    case AUTO_TRIG_ZONE_OPENED: break;
    default: return -1;
    }
    if (w->zone == 0u || w->zone > ALARM_MAX_ZONES) {
        return TG_TRIGGERED;
    }
    switch (S.cfg->zones[w->zone - 1u].device_class) {
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
    case ALARM_DEVCLASS_GARAGE_DOOR: return TG_OPENS;
    case ALARM_DEVCLASS_MOTION: return TG_MOTION;
    case ALARM_DEVCLASS_WATER: return TG_WATER;
    default: return TG_TRIGGERED;
    }
}

static int action_group(const auto_action_t *a)
{
    if (a->kind == AUTO_ACT_DISARM) {
        return TA_DISARM;
    }
    if (a->kind != AUTO_ACT_DEVICE) {
        return -1;
    }
    switch (a->dev_action) {
    case ACT_UNLOCK: return TA_UNLOCK;
    case ACT_OPEN: return TA_OPEN;
    case ACT_STOP: return TA_STOP;
    default: return -1;
    }
}

static int armed_level(uint8_t levels)
{
    if (levels == 0u) {
        return AL_ANY;
    }
    if (levels == ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY)) {
        return AL_AWAY;
    }
    if (levels == ALARM_LEVEL_BIT(ALARM_LEVEL_STAY)) {
        return AL_STAY;
    }
    return levels == ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT) ? AL_NIGHT : -1;
}

/* Partition p's name (1-based), or "Partition 2" for one without a name. */
static void partition_name(unsigned p, char *out, size_t len)
{
    if (S.cfg->partitions[p - 1u].name[0] != '\0') {
        i18n_utf8_copy(out, len, S.cfg->partitions[p - 1u].name);
    } else {
        I18N_FORMAT(out, len, TR_C("default partition name", "Partition {0}"), p);
    }
}

/* The one partition a mask names, with more than one enabled (the sentences then name it). */
static bool partition_named(uint8_t mask, char *out, size_t len)
{
    if (bit_count(partitions_enabled()) < 2u || mask == 0u || (mask & (uint8_t)(mask - 1u)) != 0u) {
        return false;
    }
    unsigned p = 1u;
    while (p < ALARM_MAX_PARTITIONS && (mask & (1u << (p - 1u))) == 0u) {
        p++;
    }
    partition_name(p, out, len);
    return true;
}

static void confirm_chain(void);

static void on_chain_ok(void)
{
    if (!E.active || E.asking >= AUTO_ACTIONS) {
        return;
    }
    E.rule.then[E.asking].confirmed = 1u; /* stored with the action (AUT-016) */
    confirm_chain();
}

/* A scene's or a switch's confirm (D8, D9): asked once, when it is picked. */
static void ask_device(const auto_action_t *a)
{
    /* TR: Settings > Automations, the confirm's title when a scene or a switch is picked for an
     * automation ({0} its name). At most 127 bytes */
    I18N_FORMAT(E.sheet_title, sizeof(E.sheet_title), TR("Use {0} in an automation?"), a->name);
    const char *note;
    if (a->dev_kind == DEV_SCENE) {
        /* TR: Settings > Automations, the scene's confirm: the panel can't see inside a scene
         * (D8). At most 191 bytes */
        note = TR("The panel can't see what a scene does. If it unlocks or opens anything, it will "
                  "do so every time this runs.");
    } else {
        /* TR: Settings > Automations, the confirm when a switch is to be turned on by an
         * automation (D9). At most 191 bytes */
        note = TR("Use it only for a light or a plug, never for a switch that opens a door, a gate "
                  "or a garage.");
    }
    /* TR: the confirm's button that keeps a scene, a switch or an action that needs it. At most
     * 127 bytes */
    ui_confirm_primary(E.sheet_title, note, TR("Use it"), on_chain_ok);
}

/* The trigger's confirm (D24, AUT-016): the sentence says what will happen. A pair without a
 * sentence of its own still asks, in general words: never skipped, or the save would be refused
 * NEEDS_CONFIRM with nothing to answer. */
static void ask_trigger(const auto_action_t *a)
{
    const int g = trigger_group(&E.rule.when);
    const int k = action_group(a);
    char zone[ALARM_NAME_LEN + 16] = "";
    char what[DEV_NAME_LEN + ALARM_NAME_LEN] = ""; /* {1}: the device, or the partition disarmed */
    bool named = true;                            /* a Disarm's partition has a name to say */
    if (a->kind == AUTO_ACT_DISARM) {
        named = a->partition >= 1u && a->partition <= ALARM_MAX_PARTITIONS &&
                partition_named((uint8_t)(1u << (a->partition - 1u)), what, sizeof(what));
    } else {
        i18n_utf8_copy(what, sizeof(what), a->name);
    }
    const char *title_pattern = NULL;
    const char *note_pattern = NULL;
    if (g == TG_ARMED && k >= 0) {
        /* {0}: the partition the trigger watches, when it names one */
        const bool trig_named = partition_named(E.rule.when.partitions, zone, sizeof(zone));
        const int lv = armed_level(E.rule.when.levels);
        if (k == TA_DISARM) {
            /* TR: Settings > Automations, the confirm's title before an automation that disarms
             * a partition every time another one is armed is kept ({0} the partition armed, {1}
             * the one it disarms). At most 127 bytes */
            title_pattern = named && trig_named ? TR_NOOP("Disarm {1} when {0} is armed?") : NULL;
            note_pattern = named && trig_named && lv >= 0 ? ARMED_NOTE_NAMED[lv][k] : NULL;
        } else {
            title_pattern = trig_named ? ARMED_TITLE_NAMED[k] : ARMED_TITLE[k]; /* as its note */
            note_pattern = lv < 0 ? NULL : trig_named ? ARMED_NOTE_NAMED[lv][k] : ARMED_NOTE_PANEL[lv][k];
        }
    } else if (g >= 0 && g < TG_TABLE && k >= 0) {
        if (E.rule.when.zone != 0u && E.rule.when.zone <= ALARM_MAX_ZONES) {
            ui_zone_name(S.cfg, E.rule.when.zone, zone, sizeof(zone));
        }
        if (k == TA_DISARM && !named) {
            title_pattern = g < TG_ALARM ? DISARM_PANEL_TITLE[g] : NULL;
            note_pattern = g < TG_ALARM ? DISARM_PANEL_NOTE[g] : NULL;
        } else {
            title_pattern = CONFIRM_TITLE[g][k];
            note_pattern = CONFIRM_NOTE[g][k];
        }
    }
    if (title_pattern != NULL && note_pattern != NULL) {
        I18N_FORMAT(E.sheet_title, sizeof(E.sheet_title), TR(title_pattern), zone, what);
        I18N_FORMAT(E.note, sizeof(E.note), TR(note_pattern), zone, what);
    } else {
        /* TR: Settings > Automations, the confirm's title for a less secure action whose own
         * sentence this software doesn't have. At most 127 bytes */
        i18n_utf8_copy(E.sheet_title, sizeof(E.sheet_title), TR("Keep this automation?"));
        /* TR: Settings > Automations, that confirm's sentence. At most 191 bytes */
        i18n_utf8_copy(E.note, sizeof(E.note),
                       TR("It will unlock, open, stop or disarm by itself every time it runs, even "
                          "when nobody is home."));
    }
    ui_confirm_primary(E.sheet_title, E.note, TR("Use it"), on_chain_ok);
}

/* Each confirm still to ask, in the order of the actions; then the save. Cancel on any of them
 * ends the editor with nothing stored. */
static void confirm_chain(void)
{
    for (uint8_t i = 0; i < E.rule.n && i < AUTO_ACTIONS; i++) {
        if (E.ask_dev[i]) {
            E.ask_dev[i] = false;
            E.asking = i;
            ask_device(&E.rule.then[i]);
            return;
        }
        if (E.ask_trig[i]) {
            E.ask_trig[i] = false;
            E.asking = i;
            ask_trigger(&E.rule.then[i]);
            return;
        }
    }
    save();
}

/* ---- the sheets: when ---------------------------------------------------------------------- */

static void then_sheet(void);
static void trigger_done(void);
static void arm_start(uint8_t level);
static void disarm_pick(void);
/* BL-224's actions */
static void remind_start(void);
static void still_start(void);
static void notify_start(void);
static void chime_set(uint8_t on);
static void chime_sheet(void);
static void output_pick(void);
static void output_do_sheet(uint8_t output, bool editing);

static void choice_set(size_t i, const char *glyph, lv_color_t tone, const char *title,
                       const char *detail, int32_t map)
{
    E.choice[i].glyph = glyph;
    E.choice[i].tone = tone;
    E.choice[i].title = title;
    E.choice[i].detail = detail;
    E.map[i] = map;
}

/* Changing the trigger of an automation that disarms: the partition its Disarm names (0: none).
 * An armed trigger of that partition, or of any, is refused at save (AUT-016, UNDOES_ARMING), so
 * the trigger sheets never offer it. */
static uint8_t disarm_of_rule(void)
{
    for (uint8_t i = 0; E.mode == ED_WHEN && i < E.rule.n && i < AUTO_ACTIONS; i++) {
        if (E.rule.then[i].kind == AUTO_ACT_DISARM) {
            return E.rule.then[i].partition;
        }
    }
    return 0u;
}

static void on_partition(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS) {
        return;
    }
    const int32_t p = E.map[index];
    E.when.partitions = p > 0 ? (uint8_t)(1u << (unsigned)(p - 1)) : 0u;
    trigger_done();
}

/* "Which partition?": only with more than one enabled; the sentences then name it. */
static void partition_or_done(void)
{
    const uint8_t enabled = partitions_enabled();
    if (bit_count(enabled) < 2u) {
        E.when.partitions = 0u;
        trigger_done();
        return;
    }
    size_t n = 0;
    int32_t selected = -1;
    int32_t own = -1; /* an armed trigger's row for the Disarm's own partition */
    const uint8_t disarms = E.when.kind == AUTO_TRIG_ARMED ? disarm_of_rule() : 0u;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((enabled & (1u << p)) == 0u) {
            continue;
        }
        if (disarms == p + 1u) {
            own = (int32_t)n;
        }
        if (S.cfg->partitions[p].name[0] != '\0') {
            i18n_utf8_copy(E.title[n], sizeof(E.title[0]), S.cfg->partitions[p].name);
        } else {
            I18N_FORMAT(E.title[n], sizeof(E.title[0]),
                        TR_C("default partition name", "Partition {0}"), (unsigned)(p + 1u));
        }
        if (E.mode == ED_WHEN && E.rule.when.kind == E.when.kind &&
            E.rule.when.partitions == (uint8_t)(1u << p)) {
            selected = (int32_t)n;
        }
        choice_set(n, UI_ICON_HOUSE, ui_tone_blue(), E.title[n], NULL, (int32_t)(p + 1u));
        n++;
    }
    if (E.mode == ED_WHEN && E.rule.when.kind == E.when.kind && E.rule.when.partitions == 0u) {
        selected = (int32_t)n;
    }
    /* TR: Settings > Automations, the last row of "Which partition?" and "Armed how?": whichever
     * it is. At most 47 bytes */
    choice_set(n, UI_ICON_SHIELD, ui_tone_gray(), TR("Any of them"), NULL, 0);
    n++;
    /* TR: Settings > Automations, the sheet that picks the partition a trigger watches. At most
     * 47 bytes */
    ui_pick_sheet(TR("Which partition?"), NULL, E.choice, n, selected, on_partition);
    if (disarms != 0u) { /* AUT-016: its own partition, or any of them, would undo the arming */
        ui_pick_sheet_dim(own);
        ui_pick_sheet_dim((int32_t)n - 1);
    }
}

static void on_armed_how(int32_t index)
{
    static const uint8_t LEVEL[4] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY, ALARM_LEVEL_NIGHT,
                                     ALARM_LEVEL_NONE};
    if (!E.active || index < 0 || index > 3) {
        return;
    }
    E.when.levels = LEVEL[index] != ALARM_LEVEL_NONE ? ALARM_LEVEL_BIT(LEVEL[index]) : 0u;
    partition_or_done();
}

static void armed_how_sheet(void)
{
    /* TR: Settings > Automations, "Armed how?": the arming levels, as the arming buttons say
     * them. At most 47 bytes each */
    choice_set(0, UI_ICON_LOG_OUT, ui_tone_rose(), TR_C("arming mode", "Away"), NULL, 0);
    choice_set(1, UI_ICON_HOUSE, ui_tone_blue(), TR_C("arming mode", "Stay"), NULL, 0);
    choice_set(2, UI_ICON_MOON, ui_tone_violet(), TR_C("arming mode", "Night"), NULL, 0);
    choice_set(3, UI_ICON_SHIELD, ui_tone_gray(), TR("Any of them"), NULL, 0);
    int32_t selected = -1;
    if (E.mode == ED_WHEN && E.rule.when.kind == AUTO_TRIG_ARMED) {
        const uint8_t lv = E.rule.when.levels;
        selected = lv == 0u                                  ? 3
                   : lv == ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY)  ? 0
                   : lv == ALARM_LEVEL_BIT(ALARM_LEVEL_STAY)  ? 1
                   : lv == ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT) ? 2
                                                              : -1;
    }
    /* TR: Settings > Automations, the sheet after "When the panel is armed": which level. At
     * most 47 bytes */
    ui_pick_sheet(TR("Armed how?"), NULL, E.choice, 4, selected, on_armed_how);
}

static void on_zone(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS || E.map[index] <= 0) {
        return;
    }
    E.when.zone = (uint16_t)E.map[index];
    trigger_done();
}

static void zone_sheet(zone_filter_t f)
{
    size_t n = 0;
    int32_t selected = -1;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES && n + 1u < ED_ROWS; z++) {
        if (!zone_listed(z, f)) {
            continue;
        }
        ui_zone_name(S.cfg, z, E.title[n], sizeof(E.title[0]));
        /* TR: Settings > Automations, a zone's detail on "Which sensor?" ({0} its number). At most
         * 31 bytes */
        I18N_FORMAT(E.detail[n], sizeof(E.detail[0]), TR_C("zone number", "Zone {0}"), (unsigned)z);
        if (E.mode == ED_WHEN && E.rule.when.kind == E.when.kind && E.rule.when.zone == z) {
            selected = (int32_t)n;
        }
        choice_set(n, ui_device_class_glyph((alarm_device_class_t)S.cfg->zones[z - 1u].device_class),
                   ui_tone_blue(), E.title[n], E.detail[n], (int32_t)z);
        n++;
    }
    const bool door = f == ZF_CLOSES || f == ZF_DOOR;
    if (n == 0u) {
        if (door) {
            /* TR: Settings > Automations, the dimmed row when no door or window can start an
             * automation. At most 47 bytes */
            choice_set(0, UI_ICON_DOOR_CLOSED, ui_tone_gray(), TR("No doors or windows to choose"),
                       NULL, ROW_NONE);
        } else {
            /* TR: Settings > Automations, the dimmed row when no sensor can start an automation.
             * At most 47 bytes */
            choice_set(0, UI_ICON_DOOR_CLOSED, ui_tone_gray(), TR("No sensors to choose"), NULL,
                       ROW_NONE);
        }
        n = 1;
    }
    /* TR: Settings > Automations, the note under the zone pickers: which zones never start an
     * automation. At most 127 bytes */
    const char *note = TR("Silent, fire and button zones aren't listed.");
    /* TR: Settings > Automations, the zone picker's title for "closes" (and a door's idea). At
     * most 47 bytes */
    const char *title_door = TR("Which door or window?");
    /* TR: Settings > Automations, the zone picker's title for "opens". At most 47 bytes */
    const char *title_any = TR("Which sensor?");
    ui_pick_sheet(door ? title_door : title_any, note, E.choice, n, selected, on_zone);
    if (E.map[0] == ROW_NONE) {
        ui_pick_sheet_dim(0);
    }
}

static void days_text(uint8_t days, char *out, size_t len)
{
    out[0] = '\0';
    const int32_t first = (int32_t)ui_region()->first_day;
    i18n_civil_t c;
    memset(&c, 0, sizeof(c));
    c.year = 2026;
    c.month = 1;
    c.day = 1;
    unsigned left = bit_count(days);
    for (int32_t col = 0; col < 7; col++) {
        const int32_t weekday = (first + col) % 7; /* 0 Sunday */
        const unsigned bit = (unsigned)((weekday + 6) % 7);
        if (((unsigned)days >> bit & 1u) == 0u) {
            continue;
        }
        c.weekday = (uint8_t)weekday;
        char name[24];
        i18n_date(name, sizeof(name), S.catalog, NOTR("EEE"), &c);
        char so_far[96];
        i18n_utf8_copy(so_far, sizeof(so_far), out);
        left--;
        if (so_far[0] == '\0') {
            i18n_utf8_copy(out, len, name);
        } else if (left > 0u) {
            /* TR: Settings > Automations, days in a list ({0} the ones so far, {1} the next): "Mon,
             * Wed". At most 15 bytes */
            I18N_FORMAT(out, len, TR_C("list of days", "{0}, {1}"), so_far, name);
        } else {
            /* TR: Settings > Automations, the last day in a list ({0} the ones before, {1} the
             * last): "Mon, Wed and Fri". At most 15 bytes */
            I18N_FORMAT(out, len, TR_C("list of days", "{0} and {1}"), so_far, name);
        }
    }
}

static void on_time_changed(int32_t minute, uint8_t days)
{
    char at[24];
    ui_minute_text((int)minute, at, sizeof(at));
    char line[128];
    if (days == 0u) {
        /* TR: Settings > Automations, the time sheet's line while no day is picked (its OK is
         * dimmed). At most 127 bytes */
        ui_time_sheet_result(TR("Pick at least one day"), true, false);
        return;
    }
    if (days == AUTO_DAYS_ALL) {
        /* TR: Settings > Automations, the time sheet's line ({0} the time). At most 127 bytes */
        I18N_FORMAT(line, sizeof(line), TR("Runs every day at {0}"), at);
    } else if (days == DAYS_WEEKDAYS) {
        /* TR: Settings > Automations, the time sheet's line ({0} the time). At most 127 bytes */
        I18N_FORMAT(line, sizeof(line), TR("Runs on weekdays at {0}"), at);
    } else if (days == DAYS_WEEKENDS) {
        /* TR: Settings > Automations, the time sheet's line ({0} the time). At most 127 bytes */
        I18N_FORMAT(line, sizeof(line), TR("Runs on weekends at {0}"), at);
    } else {
        char list[96];
        days_text(days, list, sizeof(list));
        /* TR: Settings > Automations, the time sheet's line ({0} the days, short names in a list,
         * {1} the time). At most 127 bytes */
        I18N_FORMAT(line, sizeof(line), TR("Runs on {0} at {1}"), list, at);
    }
    ui_time_sheet_result(line, false, true);
}

static bool on_time_ok(void)
{
    if (!E.active) {
        return true;
    }
    const uint8_t days = ui_time_sheet_days();
    if (days == 0u) {
        return false; /* OK is dimmed; nothing to keep */
    }
    E.when.minute = (uint16_t)ui_time_sheet_minute();
    E.when.days = days == AUTO_DAYS_ALL ? 0u : days; /* 0 is every day */
    trigger_done();
    return true;
}

static void time_sheet(void)
{
    int32_t minute = MIN_11PM;
    uint8_t days = DAYS_WEEKDAYS;
    if (E.mode == ED_WHEN && E.rule.when.kind == AUTO_TRIG_TIME) {
        minute = E.rule.when.minute;
        days = E.rule.when.days == 0u ? (uint8_t)AUTO_DAYS_ALL : E.rule.when.days;
    } else if (E.mode == ED_ADD) {
        /* An idea's time to start from (drawings automation-add-arm, bl-224-ideas) */
        switch (E.idea) {
        case IDEA_BEDTIME: minute = MIN_1030PM; days = (uint8_t)AUTO_DAYS_ALL; break;
        case IDEA_MORNING: minute = MIN_830AM; break;
        case IDEA_REMIND: minute = MIN_11PM; days = (uint8_t)AUTO_DAYS_ALL; break;
        case IDEA_STILL_ARMED: minute = MIN_4PM; break;
        case IDEA_OPENING: minute = MIN_9AM; break;
        case IDEA_CHIME_NIGHT: minute = MIN_10PM; days = (uint8_t)AUTO_DAYS_ALL; break;
        default: break;
        }
    }
    /* TR: Settings > Automations, the time sheet's note. At most 127 bytes */
    const char *note = TR("It runs once a day at most, and waits while the clock isn't set.");
    /* TR: Settings > Automations, the title of the sheet that sets a time-of-day trigger. At most
     * 47 bytes */
    ui_time_sheet_open(TR("Run at"), note, minute, days, 5, on_time_changed, on_time_ok);
    on_time_changed(ui_time_sheet_minute(), ui_time_sheet_days());
}

/* The seven triggers, in the order "When should it run?" lists them. */
static const uint8_t WHEN_KIND[] = {AUTO_TRIG_ARMED, AUTO_TRIG_DISARMED, AUTO_TRIG_ALARM,
                                    AUTO_TRIG_FIRE, AUTO_TRIG_ZONE_OPENED, AUTO_TRIG_ZONE_CLOSED,
                                    AUTO_TRIG_TIME, AUTO_TRIG_TROUBLE};
#define WHEN_ROWS (sizeof(WHEN_KIND) / sizeof(WHEN_KIND[0]))
#define WHEN_ROW_TIME 6
#define WHEN_ROW_TROUBLE 7

/* "Which trouble?" (BL-224, drawing bl-224-trouble-which), in the order it lists them. */
static const uint8_t TROUBLE_KIND[] = {AUTO_TROUBLE_ANY, AUTO_TROUBLE_AC, AUTO_TROUBLE_BATTERY,
                                       AUTO_TROUBLE_SENSOR, AUTO_TROUBLE_SIREN};
#define TROUBLE_ROWS (sizeof(TROUBLE_KIND) / sizeof(TROUBLE_KIND[0]))

static void on_trouble(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)TROUBLE_ROWS) {
        return;
    }
    E.when.trouble = TROUBLE_KIND[index];
    trigger_done();
}

static void trouble_sheet(void)
{
    /* TR: Settings > Automations, "Which trouble?" (BL-224): the troubles an automation may follow.
     * At most 63 bytes each */
    choice_set(0, UI_ICON_TRIANGLE_ALERT, ui_tone_amber(), TR("Any trouble"), NULL, 0);
    /* TR: "Which trouble?": the power went off, and the row's detail. At most 63 bytes each */
    choice_set(1, UI_AUTO_G_UNPLUG, ui_tone_amber(), TR("The power goes off"), TR("The panel runs on its battery"), 0);
    choice_set(2, UI_AUTO_G_BATTERY_LOW, ui_tone_amber(), TR("The panel's battery is low"), NULL, 0);
    /* TR: "Which trouble?": a sensor stopped reporting, and the row's detail. At most 63 bytes */
    choice_set(3, UI_AUTO_G_RADIO, ui_tone_amber(), TR("A sensor stops reporting"), TR("Or its battery is low"), 0);
    choice_set(4, UI_ICON_SIREN, ui_tone_amber(), TR("A siren problem"), NULL, 0);
    int32_t selected = -1;
    for (size_t i = 0; i < TROUBLE_ROWS && E.mode == ED_WHEN && E.rule.when.kind == AUTO_TRIG_TROUBLE; i++) {
        if (TROUBLE_KIND[i] == E.rule.when.trouble) {
            selected = (int32_t)i;
        }
    }
    /* TR: Settings > Automations, the sheet after "When there's a trouble". At most 47 bytes */
    ui_pick_sheet(TR("Which trouble?"), NULL, E.choice, TROUBLE_ROWS, selected, on_trouble);
}

/* Changing the trigger of an automation whose action means something only at a time (a reminder,
 * "still armed"): only "At a time of day" can be picked (BL-224, AUTO_ERR_NEEDS_TIME). */
static bool rule_time_only(void)
{
    for (uint8_t i = 0; E.mode == ED_WHEN && i < E.rule.n && i < AUTO_ACTIONS; i++) {
        if (E.rule.then[i].kind == AUTO_ACT_REMIND || E.rule.then[i].kind == AUTO_ACT_CHECK_ARMED) {
            return true;
        }
    }
    return false;
}

static void on_when(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)WHEN_ROWS) {
        return;
    }
    memset(&E.when, 0, sizeof(E.when));
    E.when.kind = WHEN_KIND[index];
    switch (E.when.kind) {
    case AUTO_TRIG_ARMED: armed_how_sheet(); break;
    case AUTO_TRIG_ZONE_OPENED: zone_sheet(ZF_OPENS); break;
    case AUTO_TRIG_ZONE_CLOSED: zone_sheet(ZF_CLOSES); break;
    case AUTO_TRIG_TIME: time_sheet(); break;
    case AUTO_TRIG_TROUBLE: trouble_sheet(); break; /* system-wide: no partition */
    default: partition_or_done(); break;
    }
}

static void when_sheet(void)
{
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *armed = TR("When the panel is armed");
    /* TR: Settings > Automations, "When should it run?": the armed trigger's detail. At most 63
     * bytes */
    const char *armed_detail = TR("Once exit time is over");
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *disarmed = TR("When the panel is disarmed");
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *alarm = TR("When an alarm sounds");
    /* TR: Settings > Automations, "When should it run?": the alarm trigger's detail. At most 63
     * bytes */
    const char *alarm_detail = TR("Never for a silent alarm");
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *fire = TR("When a fire or CO alarm sounds");
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *opens = TR("When a door or window opens");
    /* TR: Settings > Automations, "When should it run?": the opens trigger's detail. At most 63
     * bytes */
    const char *opens_detail = TR("Or a motion or water sensor");
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *closes = TR("When a door or window closes");
    /* TR: Settings > Automations, "When should it run?": a trigger. At most 63 bytes */
    const char *at_time = TR("At a time of day");
    /* TR: Settings > Automations, "When should it run?": the time trigger's detail. At most 63
     * bytes */
    const char *at_time_detail = TR("On the days you pick");
    choice_set(0, UI_ICON_SHIELD, ui_tone_blue(), armed, armed_detail, 0);
    choice_set(1, UI_ICON_HOUSE, ui_tone_green(), disarmed, NULL, 0);
    choice_set(2, UI_ICON_SIREN, ui_tone_red(), alarm, alarm_detail, 0);
    choice_set(3, UI_ICON_FLAME, ui_tone_red(), fire, NULL, 0);
    choice_set(4, UI_ICON_DOOR_OPEN, ui_tone_amber(), opens, opens_detail, 0);
    choice_set(5, UI_ICON_DOOR_CLOSED, ui_tone_green(), closes, NULL, 0);
    choice_set(6, UI_ICON_CLOCK, ui_tone_blue(), at_time, at_time_detail, 0);
    /* TR: Settings > Automations, "When should it run?": a trigger (BL-224), and its detail. At
     * most 63 bytes each */
    choice_set(7, UI_ICON_TRIANGLE_ALERT, ui_tone_amber(), TR("When there's a trouble"),
               TR("Power lost, a low battery, a sensor gone quiet"), 0);
    int32_t selected = -1;
    for (size_t i = 0; i < WHEN_ROWS && E.mode == ED_WHEN; i++) {
        if (WHEN_KIND[i] == E.rule.when.kind) {
            selected = (int32_t)i;
        }
    }
    /* TR: Settings > Automations, the trigger picker's note. At most 127 bytes */
    const char *note = TR("Silent alarms and duress never start an automation.");
    /* TR: Settings > Automations, the title of the trigger picker. At most 47 bytes */
    ui_pick_sheet(TR("When should it run?"), note, E.choice, WHEN_ROWS, selected, on_when);
    /* An automation never disarms when an alarm sounds (D22, AUT-011): changing a Disarm's
     * trigger, the alarm rows can't be picked; with one partition neither can "armed", which
     * could only undo the arming (AUT-016; with more, the partition step dims its own one) */
    if (disarm_of_rule() != 0u) {
        ui_pick_sheet_dim(2);
        ui_pick_sheet_dim(3);
        ui_pick_sheet_dim(WHEN_ROW_TROUBLE); /* BL-224: a power cut never disarms */
        if (bit_count(partitions_enabled()) < 2u) {
            ui_pick_sheet_dim(0);
        }
    }
    if (rule_time_only()) {
        for (int32_t i = 0; i < (int32_t)WHEN_ROWS; i++) {
            if (i != WHEN_ROW_TIME) {
                ui_pick_sheet_dim(i);
            }
        }
    }
}

/* The trigger is picked: a new automation goes on to its action; a changed one asks again for
 * each less secure action's confirm, then saves. */
static void trigger_done(void)
{
    if (E.mode == ED_ADD) {
        E.rule.when = E.when;
        switch (E.idea) { /* an idea knows what it does */
        case IDEA_BEDTIME: arm_start(ALARM_LEVEL_NIGHT); break; /* the arm sheet, Night chosen */
        case IDEA_MORNING: arm_start(ALARM_LEVEL_AWAY); break;
        case IDEA_REMIND: remind_start(); break;
        case IDEA_STILL_ARMED: still_start(); break;
        case IDEA_OPENING: disarm_pick(); break;
        case IDEA_DOOR_NOTIFY: notify_start(); break;
        case IDEA_CHIME_NIGHT: chime_set(0u); break; /* off at bedtime */
        case IDEA_OUTPUT_ALARM: output_pick(); break;
        default: then_sheet(); break;
        }
        return;
    }
    if (E.mode != ED_WHEN) {
        return;
    }
    /* "Only between" stays, except under a time of day, which has none */
    if (E.when.kind != AUTO_TRIG_TIME) {
        E.when.win_from = E.rule.when.win_from;
        E.when.win_to = E.rule.when.win_to;
    }
    const bool same = memcmp(&E.when, &E.rule.when, sizeof(E.when)) == 0;
    if (same) {
        E.active = false;
        return; /* nothing changed: nothing stored */
    }
    E.rule.when = E.when;
    for (uint8_t i = 0; i < E.rule.n && i < AUTO_ACTIONS; i++) {
        auto_action_t *a = &E.rule.then[i];
        if ((a->kind == AUTO_ACT_DEVICE || a->kind == AUTO_ACT_DISARM) && auto_action_less_secure(a)) {
            a->confirmed = 0u; /* a new pairing is confirmed again (AUT-016) */
            E.ask_trig[i] = trigger_asks(E.rule.when.kind, a);
        }
    }
    confirm_chain();
}

/* ---- the sheets: disarm (BL-170, D23) ------------------------------------------------------ */

/* The partitions a Disarm may name after this trigger (drawings automation-then-arm,
 * automation-disarm-partition): the enabled ones the person saving it may disarm; none after an
 * alarm (D22: an automation never silences one), none while the installer option is off
 * (AUT-015), and never one an armed trigger watches (AUT-016: it could only undo the arming; an
 * armed trigger of any partition rules them all out). */
/* The installer code never disarms (USR-001, BL-187), so the installer is offered no Disarm and
 * no "Disarm at opening time"; an Arm the installer saves acts as the system master (BL-224,
 * decided 2026-10-21: auto_store makes them its maker). */
static bool maker_is_installer(void)
{
    const uint8_t u = S.auth_user;
    return u >= 1u && u <= ALARM_MAX_USERS && S.cfg->users[u - 1u].authority == ALARM_AUTH_INSTALLER;
}

static uint8_t disarm_partitions(const auto_trigger_t *w, bool less_secure)
{
    /* never after an alarm (D22), nor after a trouble (BL-224: a power cut never disarms) */
    if (!less_secure || maker_is_installer() || w->kind == AUTO_TRIG_ALARM || w->kind == AUTO_TRIG_FIRE ||
        w->kind == AUTO_TRIG_TROUBLE) {
        return 0u;
    }
    uint8_t mask = partitions_enabled();
    if (w->kind == AUTO_TRIG_ARMED) {
        mask = w->partitions == 0u ? 0u : (uint8_t)(mask & (uint8_t)~w->partitions);
    }
    const uint8_t u = S.auth_user;
    if (u >= 1u && u <= ALARM_MAX_USERS && S.cfg->users[u - 1u].authority != ALARM_AUTH_NONE &&
        S.cfg->users[u - 1u].partitions != 0u) {
        mask = (uint8_t)(mask & S.cfg->users[u - 1u].partitions); /* only what they may disarm */
    }
    return mask;
}

/* The Disarm of partition p into the draft, then its confirm (D24, AUT-016), then the save. */
static void disarm_set(uint8_t p)
{
    memset(&E.act, 0, sizeof(E.act));
    E.act.kind = AUTO_ACT_DISARM;
    E.act.partition = p;
    E.act.value = -1;
    uint8_t i = 0;
    if (E.mode == ED_ADD) {
        E.rule.n = 1;
    } else if (E.mode == ED_ACTION && E.index < E.rule.n && E.index < AUTO_ACTIONS) {
        i = E.index;
        if (E.rule.then[i].kind == AUTO_ACT_DISARM && E.rule.then[i].partition == p) {
            E.active = false;
            return; /* the same partition again: nothing changed, nothing stored */
        }
    } else {
        return;
    }
    E.rule.then[i] = E.act;
    E.ask_dev[i] = false;
    E.ask_trig[i] = trigger_asks(E.rule.when.kind, &E.act);
    confirm_chain();
}

static void on_disarm_partition(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS || E.map[index] <= 0 ||
        E.map[index] > (int32_t)ALARM_MAX_PARTITIONS) {
        return;
    }
    disarm_set((uint8_t)E.map[index]);
}

/* "Disarm the panel" picked (or its row on the page): with one partition it is that one; with
 * more, "Which partition?" lists the ones it may disarm, never "Any of them" (AUT-011). */
static void disarm_pick(void)
{
    const uint8_t mask = disarm_partitions(&E.rule.when, less_secure_on());
    if (mask == 0u) {
        E.active = false;
        return;
    }
    if (bit_count(partitions_enabled()) < 2u) {
        unsigned p = 1u;
        while (p < ALARM_MAX_PARTITIONS && (mask & (1u << (p - 1u))) == 0u) {
            p++;
        }
        disarm_set((uint8_t)p);
        return;
    }
    size_t n = 0;
    int32_t selected = -1;
    for (unsigned p = 1u; p <= ALARM_MAX_PARTITIONS && n < ED_ROWS; p++) {
        if ((mask & (1u << (p - 1u))) == 0u) {
            continue;
        }
        partition_name(p, E.title[n], sizeof(E.title[0]));
        if (E.mode == ED_ACTION && E.rule.then[E.index].kind == AUTO_ACT_DISARM &&
            E.rule.then[E.index].partition == p) {
            selected = (int32_t)n;
        }
        choice_set(n, UI_ICON_HOUSE, ui_tone_blue(), E.title[n], NULL, (int32_t)p);
        n++;
    }
    /* TR: Settings > Automations, the sheet that picks the partition a trigger watches, or the
     * one an automation disarms. At most 47 bytes */
    ui_pick_sheet(TR("Which partition?"), NULL, E.choice, n, selected, on_disarm_partition);
}

/* ---- the sheets: arm (BL-170, D23, D29; drawings automation-arm, -bypass, -away) ------------ */

static void arm_sheet(void);

/* The partitions an Arm may name: the enabled ones the person saving it holds. */
static uint8_t arm_partitions(void)
{
    uint8_t mask = partitions_enabled();
    const uint8_t u = S.auth_user;
    if (u >= 1u && u <= ALARM_MAX_USERS && S.cfg->users[u - 1u].authority != ALARM_AUTH_NONE &&
        S.cfg->users[u - 1u].partitions != 0u) {
        const uint8_t held = (uint8_t)(mask & S.cfg->users[u - 1u].partitions);
        mask = held != 0u ? held : mask;
    }
    return mask;
}

static uint8_t first_partition(uint8_t mask)
{
    for (unsigned p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if ((mask & (1u << (p - 1u))) != 0u) {
            return (uint8_t)p;
        }
    }
    return 1u;
}

/* "Arm the panel" picked (or an arming idea): a new Arm at that level (Night, or Away for the
 * mornings), 5 minutes' warning, no bypass (AUT-012, D29: off by default), of the first partition
 * the person saving it holds. */
static void arm_start(uint8_t level)
{
    memset(&E.act, 0, sizeof(E.act));
    E.act.kind = AUTO_ACT_ARM;
    E.act.partition = first_partition(arm_partitions());
    E.act.arm_level = level;
    E.act.warn_min = WARN_MIN_DEFAULT;
    E.act.value = -1;
    arm_sheet();
}

/* What it will do, one sentence (the kit's result_line): {0} when it warns (a time, or for how
 * long), {1} when it arms, {2} the partition. Whole sentences: [level][named][at a time][bypass]. */
/* TR: Settings > Automations, the "Arm the panel" sheet's line under a time-of-day trigger ({0}
 * the time it warns, {1} the time it arms, {2} the partition). At most 191 bytes */
static const char *const ARM_AT[3][2][2] = {
    {{TR_NOOP("Warns at {0}, arms Away at {1}."),
      TR_NOOP("Warns at {0}, arms Away at {1}, bypassing open zones.")},
     {TR_NOOP("Warns at {0}, arms {2} Away at {1}."),
      TR_NOOP("Warns at {0}, arms {2} Away at {1}, bypassing open zones.")}},
    {{TR_NOOP("Warns at {0}, arms Stay at {1}."),
      TR_NOOP("Warns at {0}, arms Stay at {1}, bypassing open zones.")},
     {TR_NOOP("Warns at {0}, arms {2} Stay at {1}."),
      TR_NOOP("Warns at {0}, arms {2} Stay at {1}, bypassing open zones.")}},
    {{TR_NOOP("Warns at {0}, arms Night at {1}."),
      TR_NOOP("Warns at {0}, arms Night at {1}, bypassing open zones.")},
     {TR_NOOP("Warns at {0}, arms {2} Night at {1}."),
      TR_NOOP("Warns at {0}, arms {2} Night at {1}, bypassing open zones.")}},
};
/* TR: Settings > Automations, the "Arm the panel" sheet's line under any other trigger ({0} how
 * long it warns: "5 minutes", {2} the partition). At most 191 bytes */
static const char *const ARM_AFTER[3][2][2] = {
    {{TR_NOOP("Warns for {0}, then arms Away."),
      TR_NOOP("Warns for {0}, then arms Away, bypassing open zones.")},
     {TR_NOOP("Warns for {0}, then arms {2} Away."),
      TR_NOOP("Warns for {0}, then arms {2} Away, bypassing open zones.")}},
    {{TR_NOOP("Warns for {0}, then arms Stay."),
      TR_NOOP("Warns for {0}, then arms Stay, bypassing open zones.")},
     {TR_NOOP("Warns for {0}, then arms {2} Stay."),
      TR_NOOP("Warns for {0}, then arms {2} Stay, bypassing open zones.")}},
    {{TR_NOOP("Warns for {0}, then arms Night."),
      TR_NOOP("Warns for {0}, then arms Night, bypassing open zones.")},
     {TR_NOOP("Warns for {0}, then arms {2} Night."),
      TR_NOOP("Warns for {0}, then arms {2} Night, bypassing open zones.")}},
};

static void warn_text(unsigned minutes, char *out, size_t len)
{
    /* TR: Settings > Automations, how long an arming automation warns first ("5 minutes"). At
     * most 31 bytes */
    I18N_FORMAT(out, len, TRN("{0} minute", "{0} minutes", minutes), minutes);
}

/* The sheet's words that follow its choices: the keys, Away's line, the warning, the switch's
 * detail and the sentence. Nothing moves; each changes only on a tap (UI-020, UI-023). */
static void arm_refresh(void)
{
    const auto_action_t *a = &E.act;
    static const char *const KEY[3] = {"level_away", "level_stay", "level_night"};
    for (unsigned l = 0; l < 3u; l++) {
        lv_obj_t *key = ui_find(E.arm_levels, KEY[l]);
        if (key != NULL) {
            lv_obj_set_state(key, LV_STATE_CHECKED, a->arm_level == l + (unsigned)ALARM_LEVEL_AWAY);
        }
    }
    if (E.arm_away != NULL) {
        lv_obj_set_flag(E.arm_away, LV_OBJ_FLAG_HIDDEN, a->arm_level != ALARM_LEVEL_AWAY);
    }
    char span[32];
    warn_text(a->warn_min, span, sizeof(span));
    if (E.arm_warn != NULL) {
        lv_label_set_text(E.arm_warn, span);
    }
    const bool bypass = E.arm_bypass != NULL && a->bypass_open != 0u; /* only while it is offered */
    if (E.arm_bypass != NULL) {
        /* TR: Settings > Automations, the detail of "Bypass open zones" on the "Arm the panel"
         * sheet, off and on (D29). At most 79 bytes each */
        ui_row_set_detail(E.arm_bypass,
                          bypass ? TR("Arms anyway; each one is watched again once it closes")
                                 : TR("Off: it won't arm while a door or window is open"),
                          false, ui_tone_gray());
    }
    const unsigned level = a->arm_level >= ALARM_LEVEL_AWAY && a->arm_level <= ALARM_LEVEL_NIGHT
                               ? (unsigned)(a->arm_level - ALARM_LEVEL_AWAY)
                               : 2u;
    const bool named = a->partition >= 1u && a->partition <= ALARM_MAX_PARTITIONS &&
                       partition_named((uint8_t)(1u << (a->partition - 1u)), E.arm_part, sizeof(E.arm_part));
    if (E.rule.when.kind == AUTO_TRIG_TIME) {
        char warns[24], arms[24];
        const int at = (int)E.rule.when.minute;
        ui_minute_text(((at - (int)a->warn_min) % 1440 + 1440) % 1440, warns, sizeof(warns));
        ui_minute_text(at, arms, sizeof(arms));
        I18N_FORMAT(E.arm_text, sizeof(E.arm_text), TR(ARM_AT[level][named][bypass]), warns, arms,
                    named ? E.arm_part : "");
    } else {
        I18N_FORMAT(E.arm_text, sizeof(E.arm_text), TR(ARM_AFTER[level][named][bypass]), span, "",
                    named ? E.arm_part : "");
    }
    lv_label_set_text(ui_find(E.arm_result, "result_label"), E.arm_text);
}

static void on_arm_level(lv_event_t *e)
{
    const uintptr_t level = (uintptr_t)lv_event_get_user_data(e);
    if (!E.active || level < ALARM_LEVEL_AWAY || level > ALARM_LEVEL_NIGHT) {
        return;
    }
    E.act.arm_level = (uint8_t)level;
    arm_refresh();
    ui_form_sheet_fit(); /* Away's line came or went */
}

static void on_arm_warn(lv_event_t *e)
{
    const bool plus = (uintptr_t)lv_event_get_user_data(e) != 0u;
    if (!E.active) {
        return;
    }
    if (plus && E.act.warn_min < WARN_MIN_MAX) {
        E.act.warn_min++;
    } else if (!plus && E.act.warn_min > 1u) {
        E.act.warn_min--;
    } else {
        return;
    }
    arm_refresh();
}

static void on_arm_bypass(lv_event_t *e)
{
    if (!E.active) {
        return;
    }
    E.act.bypass_open = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? 1u : 0u;
    arm_refresh();
}

static void on_arm_partition(int32_t index)
{
    if (E.active && index >= 0 && index < (int32_t)ED_ROWS && E.map[index] > 0 &&
        E.map[index] <= (int32_t)ALARM_MAX_PARTITIONS) {
        E.act.partition = (uint8_t)E.map[index];
    }
    arm_sheet(); /* back to the sheet, from the draft */
}

static void arm_back(void)
{
    if (E.active) {
        arm_sheet();
    }
}

/* The partition row: a pick sheet in the arm sheet's place (one sheet at a time, subject_sheet),
 * and the arm sheet again from the draft after it, picked or cancelled. */
static void on_arm_partition_row(lv_event_t *e)
{
    (void)e;
    if (!E.active) {
        return;
    }
    const uint8_t mask = arm_partitions();
    size_t n = 0;
    int32_t selected = -1;
    for (unsigned p = 1u; p <= ALARM_MAX_PARTITIONS && n < ED_ROWS; p++) {
        if ((mask & (1u << (p - 1u))) == 0u) {
            continue;
        }
        partition_name(p, E.title[n], sizeof(E.title[0]));
        if (E.act.partition == p) {
            selected = (int32_t)n;
        }
        choice_set(n, UI_ICON_HOUSE, ui_tone_blue(), E.title[n], NULL, (int32_t)p);
        n++;
    }
    ui_sheet_close();
    /* TR: Settings > Automations, the sheet that picks the partition a trigger watches, or the
     * one an automation disarms. At most 47 bytes */
    ui_pick_sheet(TR("Which partition?"), NULL, E.choice, n, selected, on_arm_partition);
    ui_pick_sheet_on_cancel(arm_back);
}

/* Done: the Arm is the automation's only action (AUT-011); a change saves it, none stores
 * nothing. With the installer option off "Bypass open zones" isn't offered and the automation
 * arms without bypassing (D31): a changed one is saved without it, as the Panel Manager does
 * and as the validator requires (LESS_SECURE_OFF otherwise); an unchanged one keeps it. */
static bool on_arm_ok(void)
{
    if (!E.active) {
        return true;
    }
    uint8_t i = 0;
    if (E.mode == ED_ACTION && E.index < E.rule.n && E.index < AUTO_ACTIONS) {
        i = E.index;
        if (memcmp(&E.rule.then[i], &E.act, sizeof(E.act)) == 0) {
            E.active = false;
            return true; /* nothing changed: nothing stored */
        }
    } else if (E.mode != ED_ADD) {
        return true;
    }
    if (E.arm_bypass == NULL) {
        E.act.bypass_open = 0u;
    }
    memset(E.rule.then, 0, sizeof(E.rule.then));
    E.rule.then[0] = E.act;
    E.rule.n = 1;
    memset(E.ask_dev, 0, sizeof(E.ask_dev));
    memset(E.ask_trig, 0, sizeof(E.ask_trig));
    E.ask_trig[0] = trigger_asks(E.rule.when.kind, &E.act);
    confirm_chain();
    return true;
}

/* "Arm the panel" (drawings automation-arm, -bypass, -away), from the draft E.act: the level as
 * three keys; Away's amber line; the partition with more than one; how long it warns first
 * (1-15 minutes, AUT-012); "Bypass open zones" only while the installer allows less secure
 * automations (D29, D31); the sentence of what will happen; and what it never does. */
static void arm_sheet(void)
{
    /* TR: Settings > Automations, the title of the sheet that sets an arming automation. At most
     * 47 bytes */
    lv_obj_t *body = ui_form_sheet_open(TR("Arm the panel"), NULL, on_arm_ok);
    E.arm_levels = E.arm_away = E.arm_warn = E.arm_bypass = E.arm_result = NULL;
    if (body == NULL) {
        return;
    }
    E.arm_levels = arm_levels_create(body);
    static const char *const KEY[3] = {"level_away", "level_stay", "level_night"};
    for (unsigned l = 0; l < 3u; l++) {
        lv_obj_add_event_cb(ui_find(E.arm_levels, KEY[l]), on_arm_level, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(l + (unsigned)ALARM_LEVEL_AWAY));
    }
    /* TR: Settings > Automations, the "Arm the panel" sheet's amber line with Away: anyone still
     * at home sets the inside sensors off. At most 127 bytes */
    E.arm_away = ui_paragraph(body, TR("Inside sensors will be on."), ui_tone_amber());
    if (bit_count(partitions_enabled()) >= 2u) {
        char name[ALARM_NAME_LEN + 16];
        partition_name(E.act.partition, name, sizeof(name));
        /* TR: Settings > Automations, the "Arm the panel" sheet's row of the partition it arms.
         * At most 31 bytes */
        lv_obj_t *row = list_row_create(body, UI_ICON_HOUSE, ui_tone_blue(), TR_C("arm sheet", "Partition"),
                                        "", name, UI_ICON_CHEVRON_RIGHT);
        lv_obj_set_flag(ui_find(row, "detail_label"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(ui_find(row, "level"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_add_event_cb(row, on_arm_partition_row, LV_EVENT_CLICKED, NULL);
    }
    /* TR: Settings > Automations, the "Arm the panel" sheet: how long it warns before it arms
     * (AUT-012), and the row's detail. At most 47 bytes each */
    lv_obj_t *warn = row_stepper_create(body, TR("Warn first"), TR("1 to 15 minutes"), "");
    E.arm_warn = ui_find(warn, "value_label");
    lv_obj_t *minus = ui_find(warn, "minus");
    lv_obj_t *plus = ui_find(warn, "plus");
    lv_obj_add_event_cb(minus, on_arm_warn, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)0u);
    lv_obj_add_event_cb(minus, on_arm_warn, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(uintptr_t)0u);
    lv_obj_add_event_cb(plus, on_arm_warn, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)1u);
    lv_obj_add_event_cb(plus, on_arm_warn, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(uintptr_t)1u);
    if (less_secure_on()) {
        /* TR: Settings > Automations, the "Arm the panel" sheet's switch (D29). At most 47 bytes */
        E.arm_bypass = row_switch_icon_create(body, UI_ICON_CIRCLE_SLASH, ui_tone_amber(),
                                              TR("Bypass open zones"), "");
        lv_obj_t *sw = ui_find(E.arm_bypass, "toggle");
        lv_obj_set_state(sw, LV_STATE_CHECKED, E.act.bypass_open != 0u);
        lv_obj_add_event_cb(sw, on_arm_bypass, LV_EVENT_VALUE_CHANGED, NULL);
    }
    E.arm_result = result_line_create(body, "");
    /* TR: Settings > Automations, the "Arm the panel" sheet's paragraph: what it never bypasses,
     * never does, and that anyone may put it off (D26, D29). At most 191 bytes */
    ui_paragraph(body,
                 TR("Fire, CO and 24-hour zones are never bypassed. It does nothing if the panel is "
                    "already armed. Anyone can put it off with their code."),
                 ui_tone_gray());
    arm_refresh();
    ui_form_sheet_fit();
}

/* ---- the sheets: without devices (BL-224; drawings bl-224-remind, -still-armed, -chime,
 * -notify, -notify-none, -output-do) ---------------------------------------------------------- */

/* The new or changed action into the draft where the editor puts it, then the confirms it needs,
 * then the save; the same action again stores nothing. */
static void put_action(const auto_action_t *a)
{
    uint8_t i = 0;
    if (E.mode == ED_ADD) {
        memset(E.rule.then, 0, sizeof(E.rule.then));
        E.rule.n = 1;
    } else if (E.mode == ED_SECOND) {
        i = 1;
        E.rule.n = 2;
    } else if (E.mode == ED_ACTION && E.index < E.rule.n && E.index < AUTO_ACTIONS) {
        i = E.index;
        if (memcmp(&E.rule.then[i], a, sizeof(*a)) == 0) {
            E.active = false;
            return; /* nothing changed: nothing stored */
        }
    } else {
        return;
    }
    E.rule.then[i] = *a;
    E.ask_dev[i] = a->kind == AUTO_ACT_DEVICE && auto_action_needs_confirm(a->dev_kind, a->dev_action);
    E.ask_trig[i] = trigger_asks(E.rule.when.kind, a);
    confirm_chain();
}

/* "Remove this action", with two (as a device's): the other stays. */
static void drop_action(void)
{
    if (E.rule.n == 2u && E.index < 2u) {
        if (E.index == 0u) {
            E.rule.then[0] = E.rule.then[1];
        }
        memset(&E.rule.then[1], 0, sizeof(E.rule.then[1]));
        E.rule.n = 1;
        save();
    }
}

/* One of two being changed: "Remove this action" is offered. */
static bool may_drop(void)
{
    return E.mode == ED_ACTION && E.rule.n == 2u;
}

/* A new action that needs no device, of the first partition the person saving it holds. */
static void panel_act(uint8_t kind)
{
    memset(&E.act, 0, sizeof(E.act));
    E.act.kind = kind;
    E.act.value = -1;
    if (kind == AUTO_ACT_REMIND || kind == AUTO_ACT_CHECK_ARMED || kind == AUTO_ACT_CHIME) {
        E.act.partition = first_partition(arm_partitions());
    }
}

static bool on_form_ok(void)
{
    if (E.active) {
        put_action(&E.act);
    }
    return true;
}

/* A sheet's partition row (more than one partition): "Which partition?" in the sheet's place,
 * then the sheet again from the draft, picked or cancelled (as the arm sheet's). */
static void on_form_partition(int32_t index)
{
    if (E.active && index >= 0 && index < (int32_t)ED_ROWS && E.map[index] > 0 &&
        E.map[index] <= (int32_t)ALARM_MAX_PARTITIONS) {
        E.act.partition = (uint8_t)E.map[index];
    }
    if (E.active && E.form_reopen != NULL) {
        E.form_reopen();
    }
}

static void form_back(void)
{
    if (E.active && E.form_reopen != NULL) {
        E.form_reopen();
    }
}

/* The partitions the person saving it may pick, as "Which partition?" rows (the selected one
 * checked): the count. */
static size_t partition_rows(int32_t *selected)
{
    const uint8_t mask = arm_partitions();
    size_t n = 0;
    *selected = -1;
    for (unsigned p = 1u; p <= ALARM_MAX_PARTITIONS && n < ED_ROWS; p++) {
        if ((mask & (1u << (p - 1u))) == 0u) {
            continue;
        }
        partition_name(p, E.title[n], sizeof(E.title[0]));
        if (E.act.partition == p) {
            *selected = (int32_t)n;
        }
        choice_set(n, UI_ICON_HOUSE, ui_tone_blue(), E.title[n], NULL, (int32_t)p);
        n++;
    }
    return n;
}

static void on_form_partition_row(lv_event_t *e)
{
    (void)e;
    if (!E.active) {
        return;
    }
    int32_t selected = -1;
    const size_t n = partition_rows(&selected);
    ui_sheet_close();
    /* TR: Settings > Automations, the sheet that picks the partition a trigger watches, or the
     * one an automation disarms. At most 47 bytes */
    ui_pick_sheet(TR("Which partition?"), NULL, E.choice, n, selected, on_form_partition);
    ui_pick_sheet_on_cancel(form_back);
}

/* A row of a form sheet that shows a value and opens nothing. */
static lv_obj_t *value_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title, const char *value)
{
    lv_obj_t *row = list_row_create(body, glyph, tone, title, "", value, "");
    lv_obj_set_flag(ui_find(row, "detail_label"), LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui_find(row, "level"), LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui_find(row, "trail_label"), LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static void form_partition_row(lv_obj_t *body, void (*reopen)(void))
{
    E.form_reopen = reopen;
    if (bit_count(partitions_enabled()) < 2u) {
        return;
    }
    char name[ALARM_NAME_LEN + 16];
    partition_name(E.act.partition, name, sizeof(name));
    /* TR: Settings > Automations, the "Arm the panel" sheet's row of the partition it arms.
     * At most 31 bytes */
    lv_obj_t *row = list_row_create(body, UI_ICON_HOUSE, ui_tone_blue(), TR_C("arm sheet", "Partition"), "", name,
                                    UI_ICON_CHEVRON_RIGHT);
    lv_obj_set_flag(ui_find(row, "detail_label"), LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui_find(row, "level"), LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_add_event_cb(row, on_form_partition_row, LV_EVENT_CLICKED, NULL);
}

/* "Goes to": the phone services set up in Integrations, or in amber "Nothing yet"
 * (bl-224-notify-none). The mask. */
static uint8_t goes_to_row(lv_obj_t *body)
{
    const uint8_t mask = ui_auto_notify_services();
    char value[48];
    if (mask == 0u) {
        /* TR: Settings > Automations, "Goes to" while no phone service is set up (BL-224). At
         * most 31 bytes */
        i18n_utf8_copy(value, sizeof(value), TR("Nothing yet"));
    } else {
        ui_auto_services_text(mask, value, sizeof(value));
    }
    /* TR: Settings > Automations, the row of where a notification goes (BL-224). At most 31
     * bytes */
    lv_obj_t *row = value_row(body, UI_ICON_SEND, mask == 0u ? ui_tone_amber() : ui_tone_blue(), TR("Goes to"),
                              value);
    if (mask == 0u) {
        lv_obj_set_style_text_color(ui_find(row, "value_label"), ui_tone_amber(), 0);
    }
    return mask;
}

static void no_service_note(lv_obj_t *body)
{
    /* TR: Settings > Automations, a notification's sheet while no phone service is set up
     * (BL-224, drawing bl-224-notify-none). At most 127 bytes */
    ui_paragraph(body, TR("No phone notifications are set up. Set them up in Integrations; until then it can't send."),
                 ui_tone_amber());
}

static void form_result_set(const char *text)
{
    if (E.form_result != NULL) {
        lv_label_set_text(ui_find(E.form_result, "result_label"), text);
    }
}

/* ---- Remind to arm (bl-224-remind) ---- */

/* TR: Settings > Automations, the "Remind to arm" sheet's line ({0} the time, {1} the partition):
 * [the partition named][with a notification]. At most 191 bytes */
static const char *const REMIND_LINE[2][2] = {
    {TR_NOOP("At {0}, if the panel isn't armed: a chime and a note on Home."),
     TR_NOOP("At {0}, if the panel isn't armed: a chime, a note on Home and a notification.")},
    {TR_NOOP("At {0}, if {1} isn't armed: a chime and a note on Home."),
     TR_NOOP("At {0}, if {1} isn't armed: a chime, a note on Home and a notification.")},
};

static void remind_refresh(void)
{
    char at[24];
    ui_minute_text((int)(E.rule.when.minute % AUTO_MINUTES_PER_DAY), at, sizeof(at));
    char part[ALARM_NAME_LEN + 16] = "";
    const bool named = E.act.partition >= 1u && E.act.partition <= ALARM_MAX_PARTITIONS &&
                       partition_named((uint8_t)(1u << (E.act.partition - 1u)), part, sizeof(part));
    I18N_FORMAT(E.form_text, sizeof(E.form_text), TR(REMIND_LINE[named][E.act.option != 0u]), at, part);
    form_result_set(E.form_text);
}

static void on_remind_option(lv_event_t *e)
{
    if (!E.active) {
        return;
    }
    E.act.option = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? 1u : 0u;
    remind_refresh();
}

static void remind_sheet(void)
{
    /* TR: Settings > Automations, the title of the reminder's sheet (BL-224). At most 47 bytes */
    lv_obj_t *body = ui_form_sheet_open(TR("Remind to arm"), NULL, on_form_ok);
    E.form_result = NULL;
    if (body == NULL) {
        return;
    }
    form_partition_row(body, remind_sheet);
    /* TR: Settings > Automations, the reminder's switch and its detail (BL-224). At most 47 bytes
     * each */
    lv_obj_t *row = row_switch_icon_create(body, UI_ICON_SEND, ui_tone_blue(), TR("Also send a notification"),
                                           TR("To the phones set up in Integrations"));
    lv_obj_t *sw = ui_find(row, "toggle");
    lv_obj_set_state(sw, LV_STATE_CHECKED, E.act.option != 0u);
    lv_obj_add_event_cb(sw, on_remind_option, LV_EVENT_VALUE_CHANGED, NULL);
    E.form_result = result_line_create(body, "");
    /* TR: Settings > Automations, the reminder's sheet: what it never does, and when its note goes
     * (BL-224). At most 191 bytes */
    ui_paragraph(body, TR("It never arms by itself. The note goes once the panel is armed, or at 6:00 AM."),
                 ui_tone_gray());
    remind_refresh();
    ui_form_sheet_fit();
}

/* "Remind to arm" picked (or the idea): with a notification while a phone service is set up. */
static void remind_start(void)
{
    panel_act(AUTO_ACT_REMIND);
    E.act.option = ui_auto_notify_services() != 0u ? 1u : 0u;
    remind_sheet();
}

/* ---- Tell me if it's still armed (bl-224-still-armed) ---- */

static void still_sheet(void)
{
    /* TR: Settings > Automations, the title of "still armed"'s sheet (BL-224). At most 47 bytes */
    lv_obj_t *body = ui_form_sheet_open(TR("Tell me if it's still armed"), NULL, on_form_ok);
    E.form_result = NULL;
    if (body == NULL) {
        return;
    }
    form_partition_row(body, still_sheet);
    if (goes_to_row(body) == 0u) {
        no_service_note(body);
        ui_form_sheet_fit();
        return;
    }
    char when[96], quote[64], part[ALARM_NAME_LEN + 16] = "";
    ui_auto_when_text(&E.rule.when, when, sizeof(when));
    if (ui_auto_notice_title(&E.rule.when, AUTO_NOTICE_STILL_ARMED, E.act.partition, quote, sizeof(quote))) {
        const bool named = E.act.partition >= 1u && E.act.partition <= ALARM_MAX_PARTITIONS &&
                           partition_named((uint8_t)(1u << (E.act.partition - 1u)), part, sizeof(part));
        /* TR: Settings > Automations, "still armed"'s line ({0} when it runs, "At 4:00 PM on
         * weekdays"; {1} the notification's words; {2} the partition). At most 191 bytes */
        I18N_FORMAT(E.form_text, sizeof(E.form_text),
                    named ? TR("{0}, if {2} is still armed: \"{1}\".") : TR("{0}, if the panel is still armed: \"{1}\"."),
                    when, quote, part);
        E.form_result = result_line_create(body, E.form_text);
    }
    /* TR: Settings > Automations, "still armed"'s sheet: it only tells (BL-224). At most 127
     * bytes */
    ui_paragraph(body, TR("Nothing changes at the panel."), ui_tone_gray());
    ui_form_sheet_fit();
}

static void still_start(void)
{
    panel_act(AUTO_ACT_CHECK_ARMED);
    still_sheet();
}

/* ---- Send a notification (bl-224-notify, -notify-none) ---- */

static void on_form_drop(lv_event_t *e)
{
    (void)e;
    if (!E.active) {
        return;
    }
    ui_sheet_close();
    drop_action();
}

static void notify_sheet(void)
{
    /* TR: Settings > Automations, the title of the notification's sheet (BL-224). At most 47
     * bytes */
    lv_obj_t *body = ui_form_sheet_open(TR("Send a notification"), NULL, on_form_ok);
    E.form_result = NULL;
    if (body == NULL) {
        return;
    }
    E.form_reopen = NULL;
    if (goes_to_row(body) == 0u) {
        no_service_note(body);
    } else {
        char quote[64];
        if (ui_auto_notice_title(&E.rule.when, AUTO_NOTICE_EVENT, 0u, quote, sizeof(quote))) {
            /* TR: Settings > Automations, the notification's sheet: what it says ({0} its words,
             * from the trigger: "Front Door opened"). At most 127 bytes */
            I18N_FORMAT(E.form_text, sizeof(E.form_text), TR("Sends \"{0}\"."), quote);
            E.form_result = result_line_create(body, E.form_text);
        }
        if (E.rule.when.kind != AUTO_TRIG_TIME && E.rule.when.win_from != 0u && E.rule.when.win_to != 0u) {
            char from[24], to[24], line[128];
            ui_minute_text((int)((E.rule.when.win_from - 1u) % AUTO_MINUTES_PER_DAY), from, sizeof(from));
            ui_minute_text((int)((E.rule.when.win_to - 1u) % AUTO_MINUTES_PER_DAY), to, sizeof(to));
            /* TR: Settings > Automations, the notification's sheet under an "Only between" ({0}
             * from, {1} to). At most 127 bytes */
            I18N_FORMAT(line, sizeof(line), TR("Only between {0} and {1}, as the automation says."), from, to);
            ui_paragraph(body, line, ui_tone_gray());
        }
    }
    if (may_drop()) {
        /* TR: Settings > Automations, changing one of two actions: remove it. At most 47 bytes */
        lv_obj_t *row = list_row_create(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove this action"), "", "", "");
        lv_obj_set_flag(ui_find(row, "detail_label"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(ui_find(row, "level"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(ui_find(row, "value_label"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(ui_find(row, "trail_label"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_add_event_cb(row, on_form_drop, LV_EVENT_CLICKED, NULL);
    }
    ui_form_sheet_fit();
}

/* "Send a notification" picked (or the door idea): its words come from the trigger (AUT-001). */
static void notify_start(void)
{
    panel_act(AUTO_ACT_NOTIFY);
    notify_sheet();
}

/* ---- Door chime (bl-224-chime) ---- */

static void on_chime_partition(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS || E.map[index] <= 0 ||
        E.map[index] > (int32_t)ALARM_MAX_PARTITIONS) {
        return;
    }
    E.act.partition = (uint8_t)E.map[index];
    put_action(&E.act);
}

/* The chime on (1) or off (0): with more than one partition, which one next. */
static void chime_set(uint8_t on)
{
    if (E.mode == ED_ACTION && E.index < E.rule.n && E.rule.then[E.index].kind == AUTO_ACT_CHIME) {
        E.act = E.rule.then[E.index]; /* its partition stays */
    } else {
        panel_act(AUTO_ACT_CHIME);
    }
    E.act.option = on;
    if (bit_count(partitions_enabled()) < 2u) {
        put_action(&E.act);
        return;
    }
    int32_t selected = -1;
    const size_t n = partition_rows(&selected);
    /* TR: Settings > Automations, the sheet that picks the partition a trigger watches, or the
     * one an automation disarms. At most 47 bytes */
    ui_pick_sheet(TR("Which partition?"), NULL, E.choice, n, selected, on_chime_partition);
}

static void on_chime(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS) {
        return;
    }
    if (E.map[index] == ROW_DROP) {
        drop_action();
    } else if (E.map[index] == 0 || E.map[index] == 1) {
        chime_set((uint8_t)E.map[index]);
    }
}

static void chime_sheet(void)
{
    /* TR: Settings > Automations, the door chime's sheet (BL-224): its two rows. At most 47 bytes
     * each */
    choice_set(0, UI_ICON_BELL_RING, ui_tone_blue(), TR("Turn the door chime on"), NULL, 1);
    choice_set(1, UI_ICON_BELL_OFF, ui_tone_gray(), TR("Turn the door chime off"), NULL, 0);
    size_t n = 2;
    int32_t selected = -1;
    if (E.mode == ED_ACTION && E.index < E.rule.n && E.rule.then[E.index].kind == AUTO_ACT_CHIME) {
        selected = E.rule.then[E.index].option != 0u ? 0 : 1;
    }
    if (may_drop()) {
        /* TR: Settings > Automations, changing one of two actions: remove it. At most 47 bytes */
        choice_set(n++, UI_ICON_TRASH, ui_tone_red(), TR("Remove this action"), NULL, ROW_DROP);
    }
    /* TR: Settings > Automations, the title of the door chime's sheet (BL-224). At most 47 bytes */
    ui_pick_sheet(TR("Door chime"), NULL, E.choice, n, selected, on_chime);
}

/* ---- a wired output (bl-224-output-do) ---- */

/* Output n on or off; a Turn on keeps the "Turn it off after" it had, and the idea's is 30
 * minutes. */
static void output_set(uint8_t output, uint8_t action)
{
    uint16_t off_after = 0u;
    if (E.mode == ED_ACTION && E.index < E.rule.n) {
        const auto_action_t *old = &E.rule.then[E.index];
        if (old->kind == AUTO_ACT_OUTPUT && old->output == output && old->dev_action == ACT_ON) {
            off_after = old->off_after_min;
        }
    } else if (E.mode == ED_ADD && E.idea == IDEA_OUTPUT_ALARM) {
        off_after = OUTPUT_IDEA_MIN;
    }
    panel_act(AUTO_ACT_OUTPUT);
    E.act.output = output;
    E.act.dev_action = action;
    E.act.off_after_min = action == ACT_ON ? off_after : 0u;
    put_action(&E.act);
}

static void on_output_do(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS) {
        return;
    }
    const int32_t m = E.map[index];
    if (m == ROW_DROP) {
        drop_action();
    } else if (m == ACT_ON || m == ACT_OFF) {
        output_set(E.act.output, (uint8_t)m);
    }
}

/* "What should Garden Lights do?": Turn on or Turn off; while changing one of two, "Remove this
 * action". */
static void output_do_sheet(uint8_t output, bool editing)
{
    if (!editing) {
        panel_act(AUTO_ACT_OUTPUT);
    }
    E.act.output = output;
    choice_set(0, UI_AUTO_G_TOGGLE_RIGHT, ui_tone_blue(), TR_C("automation action", "Turn on"), NULL, ACT_ON);
    choice_set(1, UI_AUTO_G_TOGGLE_LEFT, ui_tone_gray(), TR_C("automation action", "Turn off"), NULL, ACT_OFF);
    size_t n = 2;
    int32_t selected = -1;
    if (editing) {
        selected = E.act.dev_action == ACT_ON ? 0 : E.act.dev_action == ACT_OFF ? 1 : -1;
        if (may_drop()) {
            /* TR: Settings > Automations, changing one of two actions: remove it. At most 47 bytes */
            choice_set(n++, UI_ICON_TRASH, ui_tone_red(), TR("Remove this action"), NULL, ROW_DROP);
        }
    }
    char name[48];
    ui_auto_output_name(output, name, sizeof(name));
    /* TR: Settings > Automations, the title of the sheet that picks what a device does ({0} its
     * name). At most 127 bytes */
    I18N_FORMAT(E.sheet_title, sizeof(E.sheet_title), TR("What should {0} do?"), name);
    ui_pick_sheet(E.sheet_title, NULL, E.choice, n, selected, on_output_do);
}

static void on_output_idea(int32_t index)
{
    if (E.active && index >= 0 && index < (int32_t)ED_ROWS && ROW_IS_OUTPUT(E.map[index])) {
        output_set(ROW_OUTPUT_OF(E.map[index]), ACT_ON);
    }
}

/* The output idea: Turn on its output (the one, or "Which output?"), for 30 minutes. */
static void output_pick(void)
{
    read_outputs();
    if (E.nout == 0u) {
        E.active = false;
        return;
    }
    if (E.nout == 1u) {
        output_set(E.outs[0].number, ACT_ON);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < E.nout && n < ED_ROWS; i++) {
        ui_auto_output_name(E.outs[i].number, E.title[n], sizeof(E.title[0]));
        ui_auto_output_detail(E.outs[i].number, E.detail[n], sizeof(E.detail[0]));
        choice_set(n, UI_AUTO_G_TOGGLE_RIGHT, ui_tone_blue(), E.title[n], E.detail[n], ROW_OUTPUT(E.outs[i].number));
        n++;
    }
    /* TR: Settings > Automations, the sheet that picks which wired output an idea switches
     * (BL-224). At most 47 bytes */
    ui_pick_sheet(TR("Which output?"), NULL, E.choice, n, -1, on_output_idea);
}

/* ---- the sheets: then ---------------------------------------------------------------------- */

static void device_do_sheet(const char *name, int32_t selected, bool editing);

static const char *other_ref(void)
{
    /* The device the other action uses: never offered twice in one automation (AUT-007) */
    if (E.mode == ED_SECOND && E.rule.n >= 1u) {
        return E.rule.then[0].ref;
    }
    if (E.mode == ED_ACTION && E.rule.n == 2u) {
        return E.rule.then[E.index == 0u ? 1u : 0u].ref;
    }
    return NULL;
}

static uint8_t other_source(void)
{
    if (E.mode == ED_SECOND) {
        return E.rule.then[0].source;
    }
    return E.rule.then[E.index == 0u ? 1u : 0u].source;
}

static bool device_listed(const ui_dev_t *d, bool less_secure)
{
    if (E.dev_filter != DEV_NONE && d->kind != E.dev_filter) {
        return false;
    }
    if (!device_offers(d, E.idea_action, less_secure)) {
        return false; /* nothing an automation may do with it (a script never) */
    }
    const char *other = other_ref();
    return other == NULL || d->source != other_source() || strncmp(d->ref, other, DEV_REF_LEN) != 0;
}

static void disarm_pick(void);

static void on_device(int32_t index)
{
    if (E.active && index >= 0 && index < (int32_t)ED_ROWS && E.map[index] == ROW_DISARM) {
        disarm_pick();
        return;
    }
    if (E.active && index >= 0 && index < (int32_t)ED_ROWS && E.map[index] == ROW_ARM) {
        arm_start(ALARM_LEVEL_NIGHT);
        return;
    }
    if (E.active && index >= 0 && index < (int32_t)ED_ROWS) { /* BL-224's actions */
        const int32_t m = E.map[index];
        if (m == ROW_REMIND) {
            remind_start();
            return;
        }
        if (m == ROW_CHECK) {
            still_start();
            return;
        }
        if (m == ROW_NOTIFY) {
            notify_start();
            return;
        }
        if (m == ROW_CHIME) {
            chime_sheet();
            return;
        }
        if (ROW_IS_OUTPUT(m)) {
            output_do_sheet(ROW_OUTPUT_OF(m), false);
            return;
        }
    }
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS || E.map[index] < 0 ||
        (size_t)E.map[index] >= E.ndev) {
        return;
    }
    const ui_dev_t *d = &E.devs[E.map[index]];
    E.noffer = offers_for(d->kind, d->source, E.offer, less_secure_on());
    if (E.noffer == 0u) {
        return;
    }
    memset(&E.act, 0, sizeof(E.act));
    E.act.kind = AUTO_ACT_DEVICE;
    E.act.source = d->source;
    E.act.dev_kind = d->kind;
    E.act.value = -1;
    i18n_utf8_copy(E.act.ref, sizeof(E.act.ref), d->ref);
    i18n_utf8_copy(E.act.name, sizeof(E.act.name), d->name);
    device_do_sheet(d->name, -1, false);
}

/* The other action of a two-action automation, while the second is added or one of two changed;
 * NULL otherwise. */
static const auto_action_t *other_action(void)
{
    if (E.mode == ED_SECOND && E.rule.n >= 1u) {
        return &E.rule.then[0];
    }
    if (E.mode == ED_ACTION && E.rule.n == 2u && E.index < 2u) {
        return &E.rule.then[E.index == 0u ? 1u : 0u];
    }
    return NULL;
}

/* Beside this other action (or alone, NULL), may an action of this kind be picked? Arm, a
 * reminder and "still armed" are always alone (AUT-011, BL-224); one chime, one notification and
 * one use of an output at most (AUT-007). */
static bool kind_beside(uint8_t kind, uint8_t output, const auto_action_t *other)
{
    if (other == NULL) {
        return true;
    }
    if (kind == AUTO_ACT_ARM || kind == AUTO_ACT_REMIND || kind == AUTO_ACT_CHECK_ARMED) {
        return false;
    }
    if (other->kind == kind && (kind == AUTO_ACT_CHIME || kind == AUTO_ACT_NOTIFY)) {
        return false;
    }
    return kind != AUTO_ACT_OUTPUT || other->kind != AUTO_ACT_OUTPUT || other->output != output;
}

/* "What should it do?" (drawings automation-then, bl-224-then-time, bl-224-then-door), in
 * sections:
 * - The alarm, for a new automation's first action: Arm the panel after any trigger and whatever
 *   the option (D4, D23; the installer's acts as the system master, BL-224), always after a
 *   warning, as the automation's only action (AUT-011); Disarm at once as the person saving it
 *   (D23), never after an alarm (D22) or a trouble, nor with the option off (AUT-015), nor for
 *   the installer (BL-187); Remind to arm and "Tell me if it's still armed" after a time of day
 *   only; then the Door chime, beside another action too.
 * - Tell someone: Send a notification.
 * - Outputs: the wired outputs "For automations" (none: no heading).
 * - Devices by area (as "Add to Favorites"), each with its area or "Not reachable now" (still
 *   pickable); never a script, nor one with nothing allowed (AUT-002). With none, a quiet note
 *   at the end instead of a dead row.
 * An idea that names a device kind lists only those devices. */
static void then_sheet(void)
{
    read_devices();
    read_outputs();
    const bool less = less_secure_on();
    const bool by_idea = E.dev_filter != DEV_NONE || E.idea_action != ACT_NONE;
    const auto_action_t *other = other_action();
    const bool first = E.mode == ED_ADD && !by_idea;
    /* changing the only action, or one of two: the actions that need no device too */
    const bool panel = !by_idea && (first || E.mode == ED_SECOND || E.mode == ED_ACTION);
    size_t n = 0;
    bool headed = false; /* a section other than Devices is listed: every section gets its heading */
    if (panel) {
        const size_t head = n++;
        if (first) {
            /* TR: Settings > Automations, "What should it do?": the action that arms the panel.
             * At most 47 bytes */
            const char *arm = TR("Arm the panel");
            /* TR: Settings > Automations, that row's detail: it always warns first (AUT-012). At
             * most 31 bytes */
            const char *arm_detail = TR("After a warning");
            choice_set(n++, UI_ICON_SHIELD, ui_tone_blue(), arm, arm_detail, ROW_ARM);
            if (disarm_partitions(&E.rule.when, less) != 0u) {
                /* TR: Settings > Automations, "What should it do?": the action that disarms the
                 * panel. At most 47 bytes */
                const char *title = TR("Disarm the panel");
                /* TR: Settings > Automations, that row's detail: it disarms without a warning.
                 * At most 31 bytes */
                const char *detail = TR("Right away");
                choice_set(n++, UI_ICON_SHIELD_OFF, ui_tone_amber(), title, detail, ROW_DISARM);
            }
            if (E.rule.when.kind == AUTO_TRIG_TIME) {
                /* TR: Settings > Automations, "What should it do?" after a time of day (BL-224):
                 * a reminder to arm, and its detail. At most 47 bytes each */
                choice_set(n++, UI_ICON_BELL, ui_tone_amber(), TR("Remind to arm"), TR("If it's still disarmed then"),
                           ROW_REMIND);
                /* TR: the same: a notification if nobody has disarmed it, and its detail. At
                 * most 47 bytes each */
                choice_set(n++, UI_ICON_SEND, ui_tone_blue(), TR("Tell me if it's still armed"),
                           TR("If nobody has disarmed it by then"), ROW_CHECK);
            }
        }
        if (kind_beside(AUTO_ACT_CHIME, 0u, other)) {
            /* TR: Settings > Automations, "What should it do?": the door chime (BL-224), and its
             * detail. At most 47 bytes each */
            choice_set(n++, UI_ICON_BELL_RING, ui_tone_blue(), TR("Door chime"), TR("Turn it on or off"), ROW_CHIME);
        }
        if (n > head + 1u) {
            /* TR: Settings > Automations, "What should it do?": the heading over arming,
             * disarming, the reminder and the chime. At most 47 bytes */
            choice_set(head, UI_CHOICE_HEADING, ui_tone_gray(), TR("The alarm"), NULL, ROW_HEADING);
            headed = true;
        } else {
            n = head;
        }
        if (kind_beside(AUTO_ACT_NOTIFY, 0u, other)) {
            /* TR: Settings > Automations, "What should it do?": the heading over a notification
             * (BL-224). At most 47 bytes */
            choice_set(n++, UI_CHOICE_HEADING, ui_tone_gray(), TR("Tell someone"), NULL, ROW_HEADING);
            /* TR: the same: a notification to the phones, and its detail. At most 47 bytes each */
            choice_set(n++, UI_ICON_SEND, ui_tone_blue(), TR("Send a notification"),
                       TR("To the phones set up in Integrations"), ROW_NOTIFY);
            headed = true;
        }
        const size_t outs_at = n++;
        for (size_t i = 0; i < E.nout && n + 1u < ED_ROWS; i++) {
            const ui_auto_output_t *o = &E.outs[i];
            if (o->number < 1u || o->number > AUTO_OUTPUTS || !kind_beside(AUTO_ACT_OUTPUT, o->number, other)) {
                continue;
            }
            ui_auto_output_name(o->number, E.title[n], sizeof(E.title[0]));
            ui_auto_output_detail(o->number, E.detail[n], sizeof(E.detail[0]));
            choice_set(n, UI_AUTO_G_TOGGLE_RIGHT, ui_tone_blue(), E.title[n], E.detail[n], ROW_OUTPUT(o->number));
            n++;
        }
        if (n > outs_at + 1u) {
            /* TR: Settings > Automations, "What should it do?": the heading over the wired
             * outputs for automations (BL-224). At most 47 bytes */
            choice_set(outs_at, UI_CHOICE_HEADING, ui_tone_gray(), TR("Outputs"), NULL, ROW_HEADING);
            headed = true;
        } else {
            n = outs_at;
        }
    }
    const size_t devices_at = n; /* "Devices" goes here, once one is listed */
    if (headed) {
        n++;
    }
    for (size_t i = 0; i < E.ndev; i++) {
        bool seen = false; /* this area came already */
        for (size_t j = 0; j < i && !seen; j++) {
            seen = strcmp(E.devs[j].area, E.devs[i].area) == 0;
        }
        for (size_t k = i; k < E.ndev && !seen && n + 1u < ED_ROWS; k++) {
            const ui_dev_t *d = &E.devs[k];
            if (strcmp(d->area, E.devs[i].area) != 0 || !device_listed(d, less)) {
                continue;
            }
            /* TR: Settings > Automations, a device's detail on "What should it do?" when its
             * source can't reach it now (it can still be picked). At most 31 bytes */
            const char *detail = d->status == UI_DEV_OFFLINE ? TR("Not reachable now") : d->area;
            choice_set(n, ui_auto_kind_glyph(d->kind, d->ref),
                       d->status == UI_DEV_OFFLINE ? ui_tone_gray() : ui_auto_kind_tone(d->kind),
                       d->name, detail, (int32_t)k);
            n++;
        }
    }
    const bool no_device = n == devices_at + (headed ? 1u : 0u);
    if (headed && no_device) {
        n = devices_at; /* no device: no heading over nothing */
    } else if (headed) {
        /* TR: Settings > Automations, "What should it do?": the heading over the devices. At
         * most 47 bytes */
        choice_set(devices_at, UI_CHOICE_HEADING, ui_tone_gray(), TR_C("automation section", "Devices"), NULL,
                   ROW_HEADING);
    }
    const bool none = n == 0u;
    if (none) { /* nothing at all to pick */
        /* TR: Settings > Automations, the dimmed row when no device can be picked. At most 47
         * bytes */
        const char *title = TR("No devices to choose");
        /* TR: Settings > Automations, that row's detail. At most 63 bytes */
        const char *detail = TR("Devices appear here once one is connected.");
        choice_set(0, UI_ICON_PLUG, ui_tone_gray(), title, detail, ROW_NONE);
        n = 1;
    }
    /* TR: Settings > Automations, the note on "What should it do?" while the installer has turned
     * "Allow less secure automations" off (AUT-015). At most 127 bytes */
    const char *note = less ? NULL : TR("Your installer has turned off automations that unlock, open or disarm.");
    /* TR: Settings > Automations, the title of the action picker. At most 47 bytes */
    ui_pick_sheet(TR("What should it do?"), note, E.choice, n, -1, on_device);
    if (none) {
        ui_pick_sheet_dim(0);
    } else if (headed && no_device && E.ndev == 0u && S.be.dev_list != NULL) {
        /* TR: Settings > Automations, the quiet note at the end of "What should it do?" while no
         * device is connected (BL-224, drawing bl-224-then-time). At most 127 bytes */
        ui_pick_sheet_tail(TR("Lights, locks and other devices show here once one is connected."));
    }
}

static void action_picked(size_t oi);

static void on_do(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS) {
        return;
    }
    const int32_t m = E.map[index];
    if (m == ROW_OTHER) {
        then_sheet();
    } else if (m == ROW_DROP) {
        drop_action();
    } else if (m >= 0 && (size_t)m < E.noffer) {
        action_picked((size_t)m);
    }
}

static void offer_row(size_t row, const hc_auto_offer_t *o)
{
    const char *glyph = UI_ICON_POWER;
    lv_color_t tone = ui_tone_green();
    const char *title = "";
    const char *detail = NULL;
    /* TR: Settings > Automations, "What should {0} do?": what an automation may do with a device.
     * At most 47 bytes each */
    switch (o->action) {
    case ACT_ON: title = TR_C("automation action", "Turn on"); break;
    case ACT_OFF:
    case ACT_SET_MODE:
        tone = ui_tone_gray();
        title = TR_C("automation action", "Turn off");
        break;
    case ACT_TOGGLE:
        glyph = UI_ICON_REFRESH;
        tone = ui_tone_blue();
        title = TR_C("automation action", "Toggle");
        /* TR: Settings > Automations, Toggle's detail. At most 63 bytes */
        detail = TR("On if it is off, off if it is on");
        break;
    case ACT_LOCK:
        glyph = UI_ICON_LOCK;
        title = TR_C("automation action", "Lock");
        break;
    case ACT_UNLOCK:
        glyph = UI_ICON_LOCK_OPEN;
        tone = ui_tone_amber();
        title = TR_C("automation action", "Unlock");
        break;
    case ACT_CLOSE:
        glyph = UI_ICON_DOOR_CLOSED;
        title = TR_C("automation action", "Close");
        break;
    case ACT_OPEN:
        glyph = UI_ICON_DOOR_OPEN;
        tone = ui_tone_amber();
        title = TR_C("automation action", "Open");
        break;
    case ACT_STOP:
        glyph = UI_ICON_CIRCLE_SLASH;
        tone = ui_tone_amber();
        title = TR_C("automation action", "Stop");
        break;
    case ACT_RUN:
        glyph = UI_ICON_PLAY;
        tone = ui_tone_violet();
        title = TR_C("automation action", "Run");
        break;
    default: break;
    }
    choice_set(row, glyph, tone, title, detail, (int32_t)row);
}

/* "What should {0} do?": only the allowed rows; while changing an action, also "Choose another
 * device" and, with two actions, "Remove this action". Adding, a device with one row skips it,
 * and so does an idea, which knows what it does. */
static void device_do_sheet(const char *name, int32_t selected, bool editing)
{
    if (!editing && E.idea_action != ACT_NONE) {
        for (size_t i = 0; i < E.noffer; i++) {
            if (E.offer[i].action == E.idea_action) {
                action_picked(i);
                return;
            }
        }
        return;
    }
    if (!editing && E.noffer == 1u) {
        action_picked(0);
        return;
    }
    size_t n = 0;
    for (; n < E.noffer; n++) {
        offer_row(n, &E.offer[n]);
    }
    if (editing) {
        /* TR: Settings > Automations, changing an action: pick another device for it. At most 47
         * bytes */
        choice_set(n++, UI_ICON_ARROW_LEFT_RIGHT, ui_tone_blue(), TR("Choose another device"), NULL,
                   ROW_OTHER);
        if (E.rule.n == 2u) {
            /* TR: Settings > Automations, changing one of two actions: remove it. At most 47
             * bytes */
            choice_set(n++, UI_ICON_TRASH, ui_tone_red(), TR("Remove this action"), NULL, ROW_DROP);
        }
    }
    /* TR: Settings > Automations, the title of the sheet that picks what a device does ({0} its
     * name). At most 127 bytes */
    I18N_FORMAT(E.sheet_title, sizeof(E.sheet_title), TR("What should {0} do?"), name);
    ui_pick_sheet(E.sheet_title, NULL, E.choice, n, selected, on_do);
}

static bool same_device(const auto_action_t *a, const auto_action_t *b)
{
    return a->source == b->source && strncmp(a->ref, b->ref, DEV_REF_LEN) == 0;
}

/* The action is picked: into the draft, then each confirm it needs, then the save. */
static void action_picked(size_t oi)
{
    if (oi >= E.noffer) {
        return;
    }
    const hc_auto_offer_t *o = &E.offer[oi];
    E.act.dev_action = o->action;
    E.act.value = (int16_t)o->value;
    i18n_utf8_copy(E.act.mode, sizeof(E.act.mode), o->mode);
    E.act.confirmed = 0u;
    E.act.off_after_min = 0u;
    uint8_t i = 0;
    if (E.mode == ED_ADD) {
        if (E.idea == IDEA_DOOR_LIGHT && E.act.dev_action == ACT_ON) {
            E.act.off_after_min = 10u;
        }
        E.rule.n = 1;
    } else if (E.mode == ED_SECOND) {
        i = 1;
        E.rule.n = 2;
    } else if (E.mode == ED_ACTION) {
        i = E.index;
        const auto_action_t *old = &E.rule.then[i];
        if (same_device(old, &E.act) && old->dev_action == E.act.dev_action &&
            old->value == E.act.value && strcmp(old->mode, E.act.mode) == 0 &&
            old->dev_kind == E.act.dev_kind) {
            E.active = false;
            return; /* the same action again: nothing changed, nothing stored */
        }
        if (same_device(old, &E.act) && old->dev_action == ACT_ON && E.act.dev_action == ACT_ON) {
            E.act.off_after_min = old->off_after_min;
        }
    } else {
        return;
    }
    E.rule.then[i] = E.act;
    E.ask_dev[i] = auto_action_needs_confirm(E.act.dev_kind, E.act.dev_action);
    E.ask_trig[i] = trigger_asks(E.rule.when.kind, &E.act);
    confirm_chain();
}

/* ---- adding: the ideas --------------------------------------------------------------------- */

static void on_idea(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)ED_ROWS) {
        return;
    }
    E.idea = (uint8_t)E.map[index];
    memset(&E.when, 0, sizeof(E.when));
    switch (E.idea) {
    case IDEA_DOOR_LIGHT:
        /* In the evening: 6:00 PM to 6:00 AM, and 10 minutes */
        E.when.kind = AUTO_TRIG_ZONE_OPENED;
        E.when.win_from = MIN_6PM + 1u;
        E.when.win_to = MIN_6AM + 1u;
        E.dev_filter = DEV_LIGHT;
        E.idea_action = ACT_ON;
        zone_sheet(ZF_DOOR);
        break;
    case IDEA_AWAY_LOCK:
        E.when.kind = AUTO_TRIG_ARMED;
        E.when.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY);
        E.dev_filter = DEV_LOCK;
        E.idea_action = ACT_LOCK;
        trigger_done();
        break;
    case IDEA_ALARM_LIGHT:
        E.when.kind = AUTO_TRIG_ALARM;
        E.dev_filter = DEV_LIGHT;
        E.idea_action = ACT_ON;
        trigger_done();
        break;
    case IDEA_LEAK_VALVE:
        E.when.kind = AUTO_TRIG_ZONE_OPENED;
        E.dev_filter = DEV_VALVE;
        E.idea_action = ACT_CLOSE;
        zone_sheet(ZF_WATER);
        break;
    case IDEA_BEDTIME:
    case IDEA_MORNING:
    case IDEA_REMIND:
    case IDEA_STILL_ARMED:
    case IDEA_OPENING:
    case IDEA_CHIME_NIGHT:
        E.when.kind = AUTO_TRIG_TIME;
        time_sheet(); /* the idea's time, then what it does (trigger_done) */
        break;
    case IDEA_DOOR_NOTIFY:
        /* At night: 10:00 PM to 6:00 AM (drawing bl-224-ideas) */
        E.when.kind = AUTO_TRIG_ZONE_OPENED;
        E.when.win_from = MIN_10PM + 1u;
        E.when.win_to = MIN_6AM + 1u;
        zone_sheet(ZF_DOOR);
        break;
    case IDEA_OUTPUT_ALARM:
        E.when.kind = AUTO_TRIG_ALARM;
        trigger_done();
        break;
    default:
        E.idea = IDEA_SCRATCH;
        when_sheet();
        break;
    }
}

void ui_auto_add_start(void)
{
    if (!begin(ED_ADD, 0)) {
        return;
    }
    read_devices();
    read_outputs();
    const bool less = less_secure_on();
    size_t n = 0;
    /* Each idea only when the house has its parts (DESIGN.md section 9 screen 7, section 15):
     * first those that need only the panel (drawings bl-224-ideas, bl-224-ideas-installer), for
     * the installer too, who never disarms (BL-187) */
    /* TR: Settings > Automations, "Add an automation": an idea, and its detail. At most 63 bytes
     * each */
    choice_set(n++, UI_ICON_MOON, ui_tone_violet(), TR("Arm at bedtime"),
               TR("Every night at a time you choose, after a warning"), IDEA_BEDTIME);
    /* TR: the same, an idea and its detail. At most 63 bytes each */
    choice_set(n++, UI_ICON_LOG_OUT, ui_tone_rose(), TR("Arm Away on weekday mornings"),
               TR("After everyone has left, with a warning"), IDEA_MORNING);
    /* TR: the same, an idea and its detail. At most 63 bytes each */
    choice_set(n++, UI_ICON_BELL, ui_tone_amber(), TR("Remind me to arm"),
               TR("A chime and a note if it's still disarmed at bedtime"), IDEA_REMIND);
    /* TR: the same, an idea and its detail. At most 63 bytes each */
    choice_set(n++, UI_ICON_SEND, ui_tone_blue(), TR("Tell me if it's still armed"),
               TR("Like nobody home from school by 4:00 PM"), IDEA_STILL_ARMED);
    if (!maker_is_installer() && less) {
        const auto_trigger_t at_time = {.kind = AUTO_TRIG_TIME};
        if (disarm_partitions(&at_time, less) != 0u) {
            /* TR: the same, an idea and its detail: shown only while the installer allows
             * disarming automations, to a code that may disarm. At most 63 bytes each */
            choice_set(n++, UI_ICON_SHIELD_OFF, ui_tone_amber(), TR("Disarm at opening time"),
                       TR("For a shop or an office, on weekdays"), IDEA_OPENING);
        }
    }
    if (any_zone(ZF_DOOR)) {
        /* TR: the same, an idea and its detail. At most 63 bytes each */
        choice_set(n++, UI_ICON_DOOR_OPEN, ui_tone_amber(), TR("Tell me when a door opens at night"),
                   TR("A notification to your phone, 10:00 PM to 6:00 AM"), IDEA_DOOR_NOTIFY);
    }
    if (any_zone(ZF_DOOR)) { /* the chime sounds for doors and windows */
        /* TR: the same, an idea and its detail. At most 63 bytes each */
        choice_set(n++, UI_ICON_BELL_OFF, ui_tone_gray(), TR("Quiet the door chime at night"),
                   TR("Off at bedtime; another can turn it on in the morning"), IDEA_CHIME_NIGHT);
    }
    if (E.nout > 0u) {
        /* TR: the same, an idea and its detail. At most 63 bytes each */
        choice_set(n++, UI_AUTO_G_TOGGLE_RIGHT, ui_tone_blue(), TR("Switch an output when an alarm sounds"),
                   TR("Like the garden lights, for 30 minutes"), IDEA_OUTPUT_ALARM);
    }
    /* then today's ideas, each with its device */
    if (any_zone(ZF_DOOR) && any_device(DEV_LIGHT, ACT_ON, less)) {
        /* TR: Settings > Automations, "Add an automation": an idea. At most 63 bytes */
        const char *title = TR("Turn on a light when a door opens");
        /* TR: Settings > Automations, "Add an automation": the idea's detail. At most 63 bytes */
        const char *detail = TR("Like the porch light, in the evening");
        choice_set(n++, UI_ICON_DOOR_OPEN, ui_tone_amber(), title, detail, IDEA_DOOR_LIGHT);
    }
    if (any_device(DEV_LOCK, ACT_LOCK, less)) {
        /* TR: Settings > Automations, "Add an automation": an idea. At most 63 bytes */
        const char *title = TR("Lock up when you arm Away");
        /* TR: Settings > Automations, "Add an automation": the idea's detail. At most 63 bytes */
        const char *detail = TR("Lock a door when everyone has left");
        choice_set(n++, UI_ICON_LOCK, ui_tone_green(), title, detail, IDEA_AWAY_LOCK);
    }
    if (any_device(DEV_LIGHT, ACT_ON, less)) {
        /* TR: Settings > Automations, "Add an automation": an idea. At most 63 bytes */
        const char *title = TR("Turn on lights when an alarm sounds");
        /* TR: Settings > Automations, "Add an automation": the idea's detail. At most 63 bytes */
        const char *detail = TR("So you can see, and be seen");
        choice_set(n++, UI_ICON_SIREN, ui_tone_red(), title, detail, IDEA_ALARM_LIGHT);
    }
    if (any_zone(ZF_WATER) && any_device(DEV_VALVE, ACT_CLOSE, less)) {
        /* TR: Settings > Automations, "Add an automation": an idea. At most 63 bytes */
        const char *title = TR("Close the water when a leak is found");
        /* TR: Settings > Automations, "Add an automation": the idea's detail. At most 63 bytes */
        const char *detail = TR("Shut the water main");
        choice_set(n++, UI_ICON_DROPLETS, ui_tone_blue(), title, detail, IDEA_LEAK_VALVE);
    }
    /* TR: Settings > Automations, "Add an automation": the last row. At most 63 bytes */
    const char *scratch = TR("Start from scratch");
    /* TR: Settings > Automations, "Add an automation": the last row's detail. At most 63 bytes */
    const char *scratch_detail = TR("Choose when it runs and what it does");
    choice_set(n++, UI_ICON_PLUS, ui_tone_blue(), scratch, scratch_detail, IDEA_SCRATCH);
    /* TR: Settings > Automations, the title of the ideas sheet. At most 47 bytes */
    ui_pick_sheet(TR("Add an automation"), NULL, E.choice, n, -1, on_idea);
}

/* ---- changing one ------------------------------------------------------------------------ */

void ui_auto_edit_when(uint8_t slot)
{
    if (!begin(ED_WHEN, slot)) {
        return;
    }
    when_sheet();
}

static void on_window_changed(int32_t from, int32_t to)
{
    char a[24], b[24], line[96];
    ui_minute_text((int)from, a, sizeof(a));
    ui_minute_text((int)to, b, sizeof(b));
    /* TR: Settings > Automations, the line under "Only between" ({0} from, {1} to). At most 95
     * bytes */
    I18N_FORMAT(line, sizeof(line), TR("From {0} to {1}"), a, b);
    ui_window_sheet_result(line, from != to);
}

static bool on_window_ok(void)
{
    if (!E.active) {
        return true;
    }
    const int32_t from = ui_window_sheet_from();
    const int32_t to = ui_window_sheet_to();
    if (from == to) {
        return false; /* Done is dimmed: from == to is no window */
    }
    E.rule.when.win_from = (uint16_t)(from + 1);
    E.rule.when.win_to = (uint16_t)(to + 1);
    save();
    return true;
}

static void on_window_any(void)
{
    if (!E.active) {
        return;
    }
    if (E.rule.when.win_from == 0u && E.rule.when.win_to == 0u) {
        E.active = false;
        return;
    }
    E.rule.when.win_from = 0u;
    E.rule.when.win_to = 0u;
    save();
}

void ui_auto_edit_window(uint8_t slot)
{
    if (!begin(ED_WINDOW, slot) || E.rule.when.kind == AUTO_TRIG_TIME) {
        E.active = false;
        return;
    }
    const auto_trigger_t *w = &E.rule.when;
    const int32_t from = w->win_from != 0u ? (int32_t)w->win_from - 1 : MIN_6PM;
    const int32_t to = w->win_to != 0u ? (int32_t)w->win_to - 1 : MIN_6AM;
    /* TR: Settings > Automations, the title of the window sheet (and its row). At most 47 bytes */
    ui_window_sheet_open(TR("Only between"), from, to, 5, on_window_changed, on_window_ok,
                         on_window_any);
    on_window_changed(ui_window_sheet_from(), ui_window_sheet_to());
}

void ui_auto_edit_action(uint8_t slot, uint8_t index)
{
    if (!begin(ED_ACTION, slot) || index >= E.rule.n || index >= AUTO_ACTIONS) {
        E.active = false;
        return;
    }
    E.index = index;
    const auto_action_t *a = &E.rule.then[index];
    if (a->kind == AUTO_ACT_DISARM) {
        /* Its partition, with more than one (automation-disarm-partition); one has nothing to
         * change */
        if (bit_count(partitions_enabled()) < 2u) {
            E.active = false;
            return;
        }
        disarm_pick();
        return;
    }
    if (a->kind == AUTO_ACT_ARM) {
        E.act = *a; /* the draft: Done stores only a change */
        arm_sheet();
        return;
    }
    /* BL-224: each action that needs no device opens its own sheet, from the draft */
    E.act = *a;
    switch (a->kind) {
    case AUTO_ACT_REMIND: remind_sheet(); return;
    case AUTO_ACT_CHECK_ARMED: still_sheet(); return;
    case AUTO_ACT_NOTIFY: notify_sheet(); return;
    case AUTO_ACT_CHIME: chime_sheet(); return;
    case AUTO_ACT_OUTPUT: output_do_sheet(a->output, true); return;
    default: break;
    }
    if (a->kind != AUTO_ACT_DEVICE) {
        E.active = false;
        return;
    }
    /* The device as it is now; a missing one goes to the device picker (automation-missing) */
    read_devices();
    uint8_t kind = a->dev_kind;
    uint8_t source = a->source;
    bool found = false;
    for (size_t i = 0; i < E.ndev; i++) {
        if (E.devs[i].source == a->source && strncmp(E.devs[i].ref, a->ref, DEV_REF_LEN) == 0) {
            kind = E.devs[i].kind;
            source = E.devs[i].source;
            found = true;
            break;
        }
    }
    if (!found) {
        then_sheet();
        return;
    }
    E.act = *a;
    E.act.dev_kind = kind;
    E.noffer = offers_for(kind, source, E.offer, less_secure_on());
    int32_t selected = -1;
    for (size_t i = 0; i < E.noffer; i++) {
        if (E.offer[i].action == a->dev_action && E.offer[i].value == a->value &&
            strcmp(E.offer[i].mode, a->mode) == 0 && kind == a->dev_kind) {
            selected = (int32_t)i;
        }
    }
    device_do_sheet(a->name, selected, true);
}

void ui_auto_add_action(uint8_t slot)
{
    /* After a device action, a Disarm (D23: one device action may follow it), an output, the
     * chime or a notification (BL-224); never after an Arm, a reminder or "still armed", each
     * always the only action (AUT-011) */
    if (!begin(ED_SECOND, slot) || E.rule.n != 1u || !ui_auto_may_add_second(&E.rule.then[0])) {
        E.active = false;
        return;
    }
    E.index = 1;
    then_sheet();
}

static const uint16_t OFF_AFTER_MIN[] = {0u, 5u, 10u, 15u, 30u, 60u};
#define OFF_AFTER_ROWS (sizeof(OFF_AFTER_MIN) / sizeof(OFF_AFTER_MIN[0]))

static void on_off_after(int32_t index)
{
    if (!E.active || index < 0 || index >= (int32_t)OFF_AFTER_ROWS) {
        return;
    }
    auto_action_t *a = &E.rule.then[E.index];
    if (a->off_after_min == OFF_AFTER_MIN[index]) {
        E.active = false;
        return;
    }
    a->off_after_min = OFF_AFTER_MIN[index];
    save();
}

void ui_auto_edit_off_after(uint8_t slot, uint8_t index)
{
    if (!begin(ED_OFF_AFTER, slot) || index >= E.rule.n || index >= AUTO_ACTIONS ||
        (E.rule.then[index].kind != AUTO_ACT_DEVICE && E.rule.then[index].kind != AUTO_ACT_OUTPUT) ||
        E.rule.then[index].dev_action != ACT_ON) {
        E.active = false; /* only under a Turn on, of a device or an output (BL-224) */
        return;
    }
    E.index = index;
    int32_t selected = -1;
    for (size_t i = 0; i < OFF_AFTER_ROWS; i++) {
        const uint16_t m = OFF_AFTER_MIN[i];
        if (m == 0u) {
            /* TR: Settings > Automations, "Turn it off after": never. At most 47 bytes */
            i18n_utf8_copy(E.title[i], sizeof(E.title[0]), TR("Leave it on"));
        } else if (m == 60u) {
            /* TR: Settings > Automations, "Turn it off after": an hour. At most 47 bytes */
            i18n_utf8_copy(E.title[i], sizeof(E.title[0]), TR("1 hour"));
        } else {
            /* TR: Settings > Automations, "Turn it off after": a number of minutes. At most 47
             * bytes */
            I18N_FORMAT(E.title[i], sizeof(E.title[0]), TRN("{0} minute", "{0} minutes", m),
                        (unsigned)m);
        }
        if (E.rule.then[index].off_after_min == m) {
            selected = (int32_t)i;
        }
        choice_set(i, NULL, ui_tone_gray(), E.title[i], NULL, (int32_t)m);
    }
    /* TR: Settings > Automations, the title of the sheet (and its row) that ends a Turn on after a
     * time. At most 47 bytes */
    ui_pick_sheet(TR("Turn it off after"), NULL, E.choice, OFF_AFTER_ROWS, selected, on_off_after);
}

/* ---- deleting ---------------------------------------------------------------------------- */

static void on_delete_ok(void)
{
    if (!E.active) {
        return;
    }
    if (ui_auto_delete(E.slot)) {
        E.active = false;
        if (on_page(PAGE_AUTOMATION)) {
            ui_page_back(); /* the list without it; no toast */
        } else if (on_page(PAGE_AUTOMATIONS)) {
            ui_page_rebuild();
        }
    } else {
        refused();
    }
}

void ui_auto_delete_ask(uint8_t slot)
{
    if (!have_store() || slot == 0u || slot > AUTO_MAX) {
        return;
    }
    E.mode = ED_DELETE;
    E.slot = slot;
    E.gen = ui_auto_page_gen();
    E.active = true;
    /* TR: Settings > Automations, the delete question's title, note and red button. At most 127
     * bytes each */
    ui_confirm(TR("Delete this automation?"), TR("It stops at once. This can't be undone."),
               TR_C("confirm button", "Delete"), on_delete_ok);
}

void ui_auto_edit_stop(void)
{
    E.active = false;
}

#endif /* ALARM_UI_AUTOMATIONS */
