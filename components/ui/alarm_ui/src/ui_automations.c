/*
 * ui_automations.c - Settings > Automations (docs/features/41, BL-168): the list, an automation's
 * page, the words and looks the editors share, and the saves (ui_automations.h, part A).
 *
 * Drawn from the approved screens (mockup screens/automations.js): the list
 * (screens 2-6) and an automation's page (16-20); BL-224's actions without devices (DESIGN.md
 * section 15) - an output, a reminder, "still armed", the door chime, a notification, the trouble
 * trigger - in the same rows, and "Set up by Installer · uses Alex's permission" for an Arm the
 * installer set up. Every rule is read through ui_backend_t's
 * auto_* hooks; the generation read when a page is built is the one an edit sends back
 * (AUT-008), so a page another device changed meanwhile is refused CHANGED and drawn again.
 *
 * Live words (UI-020, UI-023): the 1 s tick reads the rules' outcomes and rewrites a detail only
 * when its text changed; nothing counts down or moves. A change of shape - another generation,
 * a device that went missing or came back, the clock set - draws the page again in place.
 *
 * The Settings line stays hidden (DESIGN.md R4): ui_automations_available() says whether the
 * backend has the hooks, but subject_automations_avail stays 0 until finding 7 moves Event log
 * out of the column (BL-131). Until then the walk opens the section directly.
 */
#include <string.h>
#include <time.h>

#include "ui_automations.h"
#include "ui_backend/ui_dev_model.h"

#if ALARM_UI_AUTOMATIONS

/* Glyphs ui_text.h doesn't name: Lucide, in icons_20 (globals.xml) */
#define G_FAN    "\xEE\x8D\xB9" /* U+E379 fan */

#define DETAIL_LEN 192u /* a detail with a device's name in it (DEV_NAME_LEN 48) */

/* What is wrong with an automation, in the order its words win. */
typedef enum {
    PROB_NONE = 0,
    PROB_NEWER,      /* made by newer software: shown, only Delete works */
    PROB_BROKEN,     /* broken or unreadable: shown, only Delete works */
    PROB_ZONE,       /* its zone can't start an automation now (retyped) */
    PROB_KIND,       /* a device changed kind, or the runner said it can't run it */
    PROB_OPTION,     /* a less secure action while the installer option is off (AUT-015) */
    PROB_FIX,        /* anything else it can't run until it is opened (AUT-015), or a wired
                        output that isn't one for automations now (BL-224) */
    PROB_NO_SERVICE, /* BL-224: it notifies, and no phone service is set up: it runs, can't send */
} prob_t;

typedef struct {
    prob_t prob;
    uint8_t missing;  /* bit i: action i's device isn't on the panel now */
    bool clock_wait;  /* it needs the clock, which isn't set */
    uint8_t verb;     /* PROB_OPTION: 0 unlocking, 1 opening (an open or a stop), 2 disarming */
} rule_state_t;

static UI_SLOW_BSS struct {
    page_id_t page;    /* the page built last (PAGE_AUTOMATIONS or PAGE_AUTOMATION) */
    uint8_t slot;      /* PAGE_AUTOMATION's */
    uint16_t uid;
    uint16_t gen;      /* the store's generation when it was built: an edit sends it back */
    bool built;
    bool less_secure;  /* the installer option, read when the page was built (AUT-015) */
    uint32_t shape;    /* what the page's shape was drawn from */
    uint32_t ticked;
    lv_obj_t *use_row; /* the page's "Use this automation" */
    char use_text[DETAIL_LEN];
    uint8_t nrows;     /* the list's rows */
    lv_obj_t *rows[AUTO_MAX];
    uint8_t row_slot[AUTO_MAX];
    char subs[AUTO_MAX][96];
} A;

static bool notifies(const auto_rule_t *r);

static auto_rule_t *s_rules; /* AUTO_MAX, from ui_slow_alloc */
static size_t s_nrules;
static ui_dev_t *s_devs;     /* UI_DEVL_MAX, from ui_slow_alloc */
static size_t s_ndevs;
static bool s_devs_known;    /* the backend lists devices */

/* ---- reading ------------------------------------------------------------------------------- */

static bool read_rules(void)
{
    if (S.be.auto_list == NULL) {
        s_nrules = 0;
        return false;
    }
    if (s_rules == NULL && (s_rules = ui_slow_alloc(AUTO_MAX * sizeof(*s_rules))) == NULL) {
        s_nrules = 0;
        return false;
    }
    s_nrules = S.be.auto_list(S.be.ctx, s_rules, AUTO_MAX);
    if (s_nrules > AUTO_MAX) {
        s_nrules = AUTO_MAX;
    }
    return true;
}

static void read_devices(void)
{
    s_ndevs = 0;
    s_devs_known = false;
    if (S.be.dev_list == NULL) {
        return;
    }
    if (s_devs == NULL && (s_devs = ui_slow_alloc(UI_DEVL_MAX * sizeof(*s_devs))) == NULL) {
        return;
    }
    s_ndevs = S.be.dev_list(S.be.ctx, s_devs, UI_DEVL_MAX);
    if (s_ndevs > UI_DEVL_MAX) {
        s_ndevs = UI_DEVL_MAX;
    }
    s_devs_known = true;
}

static uint16_t store_gen(void)
{
    return S.be.auto_gen != NULL ? S.be.auto_gen(S.be.ctx) : 0u;
}

static const auto_rule_t *rule_in_slot(uint8_t slot)
{
    for (size_t i = 0; i < s_nrules; i++) {
        if (s_rules[i].slot == slot) {
            return &s_rules[i];
        }
    }
    return NULL;
}

static bool same_source(uint8_t dev_source, uint8_t act_source)
{
    return dev_source == act_source || dev_source == DSRC_NONE;
}

/* The device an action names, as the panel lists it now; NULL when it isn't listed. */
static const ui_dev_t *device_of(const auto_action_t *a)
{
    for (size_t i = 0; i < s_ndevs; i++) {
        if (same_source(s_devs[i].source, a->source) && strcmp(s_devs[i].ref, a->ref) == 0) {
            return &s_devs[i];
        }
    }
    return NULL;
}

/* Its source lists anything at all: a source that is still starting (Home Assistant not yet
 * connected) lists nothing, and then no device is called missing for it. */
static bool source_listing(uint8_t source)
{
    for (size_t i = 0; i < s_ndevs; i++) {
        if (same_source(s_devs[i].source, source)) {
            return true;
        }
    }
    return false;
}

static bool needs_clock(const auto_rule_t *r)
{
    return r->when.kind == AUTO_TRIG_TIME || (r->when.win_from != 0u && r->when.win_to != 0u);
}

static bool zone_trigger(uint8_t kind)
{
    return kind == AUTO_TRIG_ZONE_OPENED || kind == AUTO_TRIG_ZONE_CLOSED;
}

static bool failed_result(uint8_t res)
{
    switch (res) {
    case AUTO_RES_REFUSED:
    case AUTO_RES_NO_ANSWER:
    case AUTO_RES_UNREACHABLE:
    case AUTO_RES_MISSING:
    case AUTO_RES_BUSY:
    case AUTO_RES_HELD_BACK:
    case AUTO_RES_NOT_ALLOWED: return true;
    default: return false;
    }
}

static void status_of(const auto_rule_t *r, auto_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (S.be.auto_status == NULL || r->uid == 0u || !S.be.auto_status(S.be.ctx, r->uid, st)) {
        memset(st, 0, sizeof(*st));
    }
}

static void rule_state(const auto_rule_t *r, const auto_status_t *st, rule_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (r->rec == AUTO_REC_NEWER) {
        out->prob = PROB_NEWER;
        return;
    }
    if (r->rec != AUTO_REC_OK) {
        out->prob = PROB_BROKEN;
        return;
    }
    bool kind_bad = st->result == AUTO_RES_CANT_RUN;
    bool fix = st->result == AUTO_RES_MAKER_GONE;
    bool option = false;
    for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        const auto_action_t *a = &r->then[i];
        if (!option && (!A.less_secure || st->result == AUTO_RES_LESS_SECURE_OFF) &&
            auto_action_less_secure(a)) {
            option = true; /* AUT-015: kept as it is, never sent while the option is off */
            out->verb = a->kind == AUTO_ACT_DISARM ? 2u : a->dev_action == ACT_UNLOCK ? 0u : 1u;
        }
        if (a->kind == AUTO_ACT_OUTPUT && S.be.auto_outputs != NULL) {
            char detail[32];
            ui_auto_output_detail(a->output, detail, sizeof(detail));
            fix = fix || detail[0] == '\0'; /* not "For automations" now (AUTO_ERR_OUTPUT) */
        }
        if (a->kind != AUTO_ACT_DEVICE) {
            continue;
        }
        const ui_dev_t *d = device_of(a);
        if (d == NULL) {
            if (s_devs_known && source_listing(a->source)) {
                out->missing |= (uint8_t)(1u << i);
            }
        } else if (d->kind != a->dev_kind) {
            kind_bad = true;
        }
    }
    const uint16_t z = r->when.zone;
    if (zone_trigger(r->when.kind) && S.cfg != NULL &&
        (z == 0u || z > ALARM_MAX_ZONES ||
         !auto_zone_usable((uint8_t)S.cfg->zones[z - 1u].type, S.cfg->zones[z - 1u].device_class,
                           r->when.kind == AUTO_TRIG_ZONE_CLOSED))) {
        out->prob = PROB_ZONE;
    } else if (kind_bad) {
        out->prob = PROB_KIND;
    } else if (option) {
        out->prob = PROB_OPTION;
    } else if (fix || st->result == AUTO_RES_LESS_SECURE_OFF) {
        out->prob = PROB_FIX;
    } else if (notifies(r) && (st->result == AUTO_RES_NO_SERVICE || ui_auto_notify_services() == 0u)) {
        out->prob = PROB_NO_SERVICE;
    }
    out->clock_wait = needs_clock(r) && !ui_clock_known();
}

/* ---- the words ----------------------------------------------------------------------------- */

static void copy(char *out, size_t len, const char *text)
{
    i18n_utf8_copy(out, len, text);
}

/* An automation the installer option stops (AUT-015, drawing automations-option-off): only the
 * installer changes it, so the words never say to open it. */
static void option_words(uint8_t verb, char *out, size_t len)
{
    if (verb == 2u) {
        /* TR: an automation's state, in the list and on its page: the installer turned off
         * automations that disarm. At most 95 bytes */
        copy(out, len, TR("Can't run: your installer turned disarming off"));
    } else if (verb == 1u) {
        /* TR: an automation's state, in the list and on its page: the installer turned off
         * automations that open (or stop) a garage door, a cover or a valve. At most 95 bytes */
        copy(out, len, TR("Can't run: your installer turned opening off"));
    } else {
        /* TR: an automation's state, in the list and on its page: the installer turned off
         * automations that unlock. At most 95 bytes */
        copy(out, len, TR("Can't run: your installer turned unlocking off"));
    }
}

/* TR: an automation whose trigger or action this software can't read (D21), as its sentence. At
 * most 95 bytes */
#define CANT_SHOW() TR("Something this software can't show")

static int enabled_partitions(void)
{
    int n = 0;
    for (unsigned p = 0; S.cfg != NULL && p < ALARM_MAX_PARTITIONS; p++) {
        n += S.cfg->partitions[p].enabled ? 1 : 0;
    }
    return n;
}

/* The one partition a mask names, with more than one enabled: its name; NULL otherwise. */
static const char *partition_named(uint8_t mask, char *buf, size_t len)
{
    if (enabled_partitions() < 2 || mask == 0u || (mask & (uint8_t)(mask - 1u)) != 0u) {
        return NULL;
    }
    unsigned p = 1;
    while (p < ALARM_MAX_PARTITIONS && (mask & (1u << (p - 1u))) == 0u) {
        p++;
    }
    const char *name = S.cfg->partitions[p - 1u].name;
    if (name[0] != '\0') {
        copy(buf, len, name);
    } else {
        /* TR: a partition without a name, in an automation's sentence. At most 31 bytes */
        I18N_FORMAT(buf, len, TR_C("default partition name", "Partition {0}"), p);
    }
    return buf;
}

static void armed_text(const auto_trigger_t *w, char *out, size_t len)
{
    char part[ALARM_NAME_LEN + 16];
    const char *name = partition_named(w->partitions, part, sizeof(part));
    const uint8_t away = ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY);
    const uint8_t stay = ALARM_LEVEL_BIT(ALARM_LEVEL_STAY);
    const uint8_t night = ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT);
    /* TR: the trigger of an automation, the list's row and its page (When). {0} is a
     * partition's name. At most 63 bytes */
    if (w->levels == away) {
        name != NULL ? (void)I18N_FORMAT(out, len, TR("When {0} is armed Away"), name)
                     : copy(out, len, TR("When the panel is armed Away"));
    } else if (w->levels == stay) {
        name != NULL ? (void)I18N_FORMAT(out, len, TR("When {0} is armed Stay"), name) /* TR: an automation's trigger, its list row and page; {0} a partition. At most 63 bytes */
                     : copy(out, len, TR("When the panel is armed Stay")); /* TR: an automation's trigger, its list row and page; {0} a partition. At most 63 bytes */
    } else if (w->levels == night) {
        name != NULL ? (void)I18N_FORMAT(out, len, TR("When {0} is armed Night"), name) /* TR: an automation's trigger, its list row and page; {0} a partition. At most 63 bytes */
                     : copy(out, len, TR("When the panel is armed Night")); /* TR: an automation's trigger, its list row and page; {0} a partition. At most 63 bytes */
    } else {
        name != NULL ? (void)I18N_FORMAT(out, len, TR("When {0} is armed"), name) /* TR: an automation's trigger, its list row and page; {0} a partition. At most 63 bytes */
                     : copy(out, len, TR("When the panel is armed")); /* TR: an automation's trigger, its list row and page; {0} a partition. At most 63 bytes */
    }
}

/* The chosen days in the region's week order, by their short names: "Mon, Wed, Fri". */
static void days_text(uint8_t days, char *out, size_t len)
{
    out[0] = '\0';
    const unsigned first = ui_region()->first_day;
    i18n_civil_t c;
    memset(&c, 0, sizeof(c));
    c.year = 2026;
    c.month = 1;
    c.day = 1;
    size_t used = 0;
    for (unsigned col = 0; col < 7u; col++) {
        const unsigned weekday = (first + col) % 7u; /* 0 Sunday */
        const unsigned bit = (weekday + 6u) % 7u;    /* bit 0 Monday */
        if ((days & (1u << bit)) == 0u) {
            continue;
        }
        char name[24];
        c.weekday = (uint8_t)weekday;
        i18n_date(name, sizeof(name), S.catalog, NOTR("EEE"), &c);
        if (used > 0u) {
            /* TR: between the short day names an automation runs on ("Mon, Wed, Fri"). At most 7 bytes */
            copy(out + used, len - used, TR_C("list separator", ", "));
            used = strlen(out);
        }
        copy(out + used, len - used, name);
        used = strlen(out);
        if (used + 1u >= len) {
            break;
        }
    }
}

void ui_auto_when_text(const auto_trigger_t *w, char *out, size_t len)
{
    if (out == NULL || len == 0u) {
        return;
    }
    out[0] = '\0';
    if (w == NULL) {
        return;
    }
    char name[ALARM_NAME_LEN + 16];
    switch (w->kind) {
    case AUTO_TRIG_ARMED: armed_text(w, out, len); break;
    case AUTO_TRIG_DISARMED: {
        const char *part = partition_named(w->partitions, name, sizeof(name));
        /* TR: the trigger of an automation. {0} is a partition's name. At most 63 bytes */
        part != NULL ? (void)I18N_FORMAT(out, len, TR("When {0} is disarmed"), part)
                     : copy(out, len, TR("When the panel is disarmed"));
        break;
    }
    /* TR: the trigger of an automation. At most 63 bytes */
    case AUTO_TRIG_ALARM: copy(out, len, TR("When an alarm sounds")); break;
    /* TR: the trigger of an automation. At most 63 bytes */
    case AUTO_TRIG_FIRE: copy(out, len, TR("When a fire or CO alarm sounds")); break;
    case AUTO_TRIG_ZONE_OPENED:
    case AUTO_TRIG_ZONE_CLOSED: {
        if (S.cfg == NULL || w->zone == 0u || w->zone > ALARM_MAX_ZONES) {
            copy(out, len, CANT_SHOW());
            break;
        }
        ui_zone_name(S.cfg, w->zone, name, sizeof(name));
        const uint8_t cls = S.cfg->zones[w->zone - 1u].device_class;
        /* TR: the trigger of an automation; {0} is a zone's name. At most 63 bytes */
        if (w->kind == AUTO_TRIG_ZONE_CLOSED) {
            I18N_FORMAT(out, len, TR("When {0} closes"), name);
        } else if (cls == ALARM_DEVCLASS_DOOR || cls == ALARM_DEVCLASS_WINDOW ||
                   cls == ALARM_DEVCLASS_GARAGE_DOOR) {
            I18N_FORMAT(out, len, TR("When {0} opens"), name); /* TR: an automation's trigger; {0} is a zone's name. At most 63 bytes */
        } else if (cls == ALARM_DEVCLASS_MOTION) {
            I18N_FORMAT(out, len, TR("When {0} sees motion"), name); /* TR: an automation's trigger; {0} is a zone's name. At most 63 bytes */
        } else if (cls == ALARM_DEVCLASS_WATER) {
            I18N_FORMAT(out, len, TR("When {0} detects water"), name); /* TR: an automation's trigger; {0} is a zone's name. At most 63 bytes */
        } else {
            I18N_FORMAT(out, len, TR("When {0} is triggered"), name); /* TR: an automation's trigger; {0} is a zone's name. At most 63 bytes */
        }
        break;
    }
    case AUTO_TRIG_TIME: {
        char at[24];
        ui_minute_text((int)(w->minute % AUTO_MINUTES_PER_DAY), at, sizeof(at));
        const uint8_t days = (uint8_t)(w->days & AUTO_DAYS_ALL);
        const uint8_t weekdays = 0x1Fu, weekend = 0x60u;
        /* TR: the trigger of an automation; {0} is a time of day ("11:00 PM"). At most 47
         * bytes */
        if (days == 0u || days == AUTO_DAYS_ALL) {
            I18N_FORMAT(out, len, TR("At {0} every day"), at);
        } else if (days == weekdays) {
            I18N_FORMAT(out, len, TR("At {0} on weekdays"), at); /* TR: an automation's trigger; {0} is a time of day. At most 47 bytes */
        } else if (days == weekend) {
            I18N_FORMAT(out, len, TR("At {0} on weekends"), at); /* TR: an automation's trigger; {0} is a time of day. At most 47 bytes */
        } else {
            char list[96];
            days_text(days, list, sizeof(list));
            /* TR: the trigger of an automation; {0} is a time of day, {1} the short names of
             * its days ("Mon, Wed, Fri"). At most 47 bytes */
            I18N_FORMAT(out, len, TR("At {0} on {1}"), at, list);
        }
        break;
    }
    case AUTO_TRIG_TROUBLE:
        /* TR: the trigger of an automation (BL-224): which trouble starts it. At most 63 bytes */
        switch (w->trouble) {
        case AUTO_TROUBLE_ANY: copy(out, len, TR("When there's a trouble")); break;
        case AUTO_TROUBLE_AC: copy(out, len, TR("When the power goes off")); break;
        case AUTO_TROUBLE_BATTERY: copy(out, len, TR("When the panel's battery is low")); break;
        case AUTO_TROUBLE_SENSOR: copy(out, len, TR("When a sensor stops reporting")); break;
        case AUTO_TROUBLE_SIREN: copy(out, len, TR("When there's a siren problem")); break;
        default: copy(out, len, CANT_SHOW()); break;
        }
        break;
    default: copy(out, len, CANT_SHOW()); break;
    }
}

void ui_auto_window_text(const auto_trigger_t *w, char *out, size_t len)
{
    if (out == NULL || len == 0u) {
        return;
    }
    if (w == NULL || w->win_from == 0u || w->win_to == 0u) {
        /* TR: an automation's "Only between" when it has no window. At most 47 bytes */
        copy(out, len, TR("Any time"));
        return;
    }
    char from[24], to[24];
    ui_minute_text((int)((w->win_from - 1u) % AUTO_MINUTES_PER_DAY), from, sizeof(from));
    ui_minute_text((int)((w->win_to - 1u) % AUTO_MINUTES_PER_DAY), to, sizeof(to));
    /* TR: an automation's "Only between" ("6:00 PM to 6:00 AM"); {0} and {1} are times of
     * day. At most 31 bytes */
    I18N_FORMAT(out, len, TR("{0} to {1}"), from, to);
}

void ui_auto_off_after_text(uint16_t minutes, char *out, size_t len)
{
    if (out == NULL || len == 0u) {
        return;
    }
    if (minutes == 0u) {
        /* TR: "Turn it off after" when nothing is turned off. At most 31 bytes */
        copy(out, len, TR("Leave it on"));
    } else if (minutes % 60u == 0u) {
        /* TR: how long until an automation turns a device off again. At most 31 bytes */
        I18N_FORMAT(out, len, TRN("{0} hour", "{0} hours", minutes / 60u), (unsigned)(minutes / 60u));
    } else {
        /* TR: how long until an automation turns a device off again. At most 31 bytes */
        I18N_FORMAT(out, len, TRN("{0} minute", "{0} minutes", minutes), (unsigned)minutes);
    }
}

static void arm_text(const auto_action_t *a, char *out, size_t len)
{
    char part[ALARM_NAME_LEN + 16];
    const char *name = a->partition >= 1u && a->partition <= ALARM_MAX_PARTITIONS
                           ? partition_named((uint8_t)(1u << (a->partition - 1u)), part, sizeof(part))
                           : NULL;
    /* TR: an arming automation's action (phase C); {0} is a partition's name. At most 47
     * bytes */
    switch (a->arm_level) {
    case ALARM_LEVEL_AWAY:
        name != NULL ? (void)I18N_FORMAT(out, len, TR("Arm {0} Away"), name) : copy(out, len, TR("Arm Away"));
        break;
    case ALARM_LEVEL_STAY:
        name != NULL ? (void)I18N_FORMAT(out, len, TR("Arm {0} Stay"), name) : copy(out, len, TR("Arm Stay"));
        break;
    case ALARM_LEVEL_NIGHT:
        name != NULL ? (void)I18N_FORMAT(out, len, TR("Arm {0} Night"), name) : copy(out, len, TR("Arm Night"));
        break;
    default: copy(out, len, CANT_SHOW()); break;
    }
}

/* ---- automations without devices (BL-224): outputs, notifications, the words they share ---- */

size_t ui_auto_outputs(ui_auto_output_t *out, size_t max)
{
    if (S.be.auto_outputs == NULL || out == NULL || max == 0u) {
        return 0u;
    }
    const size_t n = S.be.auto_outputs(S.be.ctx, out, max);
    return n < max ? n : max;
}

static bool output_of(uint8_t number, ui_auto_output_t *out)
{
    ui_auto_output_t list[AUTO_OUTPUTS];
    const size_t n = ui_auto_outputs(list, AUTO_OUTPUTS);
    for (size_t i = 0; i < n; i++) {
        if (list[i].number == number) {
            *out = list[i];
            return true;
        }
    }
    return false;
}

void ui_auto_output_name(uint8_t number, char *out, size_t len)
{
    ui_auto_output_t o;
    if (output_of(number, &o) && o.name[0] != '\0') {
        copy(out, len, o.name); /* a household name: a value */
        return;
    }
    /* TR: a wired output for automations without a name ({0} its number). At most 31 bytes */
    I18N_FORMAT(out, len, TR("Output {0}"), (unsigned)number);
}

void ui_auto_output_detail(uint8_t number, char *out, size_t len)
{
    ui_auto_output_t o;
    out[0] = '\0';
    if (output_of(number, &o)) {
        /* TR: under a wired output in "What should it do?" and on an automation's page ({0} its
         * terminal's GPIO number). At most 31 bytes */
        I18N_FORMAT(out, len, TR("Output on GPIO {0}"), (unsigned)o.gpio);
    }
}

uint8_t ui_auto_notify_services(void)
{
    return S.be.auto_notify_services != NULL ? S.be.auto_notify_services(S.be.ctx) : 0u;
}

void ui_auto_services_text(uint8_t mask, char *out, size_t len)
{
    out[0] = '\0';
    /* the services' own names, never translated */
    static const char *const NAME[UI_NOTIFY_PROVIDERS] = {NOTR("ntfy"), NOTR("Pushover")};
    for (unsigned p = 0; p < UI_NOTIFY_PROVIDERS; p++) {
        if ((mask & (1u << p)) == 0u) {
            continue;
        }
        const size_t used = strlen(out);
        if (used > 0u) {
            /* TR: between the phone services a notification goes to ("ntfy, Pushover"). At most 7
             * bytes */
            copy(out + used, len - used, TR_C("list separator", ", "));
        }
        const size_t at = strlen(out);
        copy(out + at, len - at, NAME[p]);
    }
}

/* The first partition a mask names, or 1. */
static uint8_t first_of(uint8_t mask)
{
    for (unsigned p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if ((mask & (1u << (p - 1u))) != 0u) {
            return (uint8_t)p;
        }
    }
    return 1u;
}

/* A moment at this local minute of the day (a mid-month day, as the clock words need only its
 * time): what the notification's words read their time from. */
static int64_t epoch_at_minute(uint16_t minute)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 15;
    tm.tm_hour = (int)(minute / 60u % 24u);
    tm.tm_min = (int)(minute % 60u);
    tm.tm_isdst = -1;
    const time_t t = mktime(&tm);
    return t == (time_t)-1 ? 0 : (int64_t)t * 1000;
}

bool ui_auto_notice_title(const auto_trigger_t *w, uint8_t what, uint8_t p, char *out, size_t len)
{
    if (w == NULL || out == NULL || len == 0u || S.cfg == NULL) {
        return false;
    }
    out[0] = '\0';
    /* the event the notify port raises (the build contract's mapping) */
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ALARM_EV_AUTOMATION_NOTICE;
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.flags = ALARM_EVF_REPORT;
    if (what == AUTO_NOTICE_REMIND || what == AUTO_NOTICE_STILL_ARMED) {
        ev.arg = what == AUTO_NOTICE_REMIND ? ALARM_AUTO_NOTICE_REMIND : ALARM_AUTO_NOTICE_STILL_ARMED;
        ev.partition = p >= 1u && p <= ALARM_MAX_PARTITIONS ? p : 1u;
        ev.epoch_ms = epoch_at_minute(w->minute);
        w = NULL;
    }
    switch (w != NULL ? w->kind : AUTO_TRIG_NONE) {
    case AUTO_TRIG_NONE: break; /* a reminder's or "still armed"'s, above */
    case AUTO_TRIG_TIME:
        ev.arg = ALARM_AUTO_NOTICE_TIME;
        ev.epoch_ms = epoch_at_minute(w->minute);
        break;
    case AUTO_TRIG_ZONE_OPENED:
    case AUTO_TRIG_ZONE_CLOSED:
        if (w->zone == 0u || w->zone > ALARM_MAX_ZONES) {
            return false;
        }
        ev.arg = w->kind == AUTO_TRIG_ZONE_OPENED ? ALARM_AUTO_NOTICE_OPENED : ALARM_AUTO_NOTICE_CLOSED;
        ev.zone = w->zone;
        ev.partition = S.cfg->zones[w->zone - 1u].partition;
        break;
    case AUTO_TRIG_ARMED:
        if (w->levels != ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY) && w->levels != ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) &&
            w->levels != ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT)) {
            return false; /* "any level": the words name the level it was armed at */
        }
        ev.arg = ALARM_AUTO_NOTICE_ARMED;
        ev.partition = first_of(w->partitions);
        ev.arg2 = w->levels == ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY)   ? ALARM_LEVEL_AWAY
                  : w->levels == ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) ? ALARM_LEVEL_STAY
                                                                   : ALARM_LEVEL_NIGHT;
        break;
    case AUTO_TRIG_DISARMED:
        ev.arg = ALARM_AUTO_NOTICE_DISARMED;
        ev.partition = first_of(w->partitions);
        break;
    case AUTO_TRIG_ALARM: ev.arg = ALARM_AUTO_NOTICE_ALARM; break;
    case AUTO_TRIG_FIRE: ev.arg = ALARM_AUTO_NOTICE_FIRE; break;
    case AUTO_TRIG_TROUBLE:
        ev.arg = ALARM_AUTO_NOTICE_TROUBLE;
        ev.arg2 = w->trouble == AUTO_TROUBLE_ANY ? (uint8_t)AUTO_TROUBLE_OTHER : w->trouble;
        break;
    default: return false;
    }
    ui_line_t line;
    memset(&line, 0, sizeof(line));
    if (!ui_event_line(S.cfg, &ev, true, &line) || line.title[0] == '\0') {
        return false;
    }
    copy(out, len, line.title);
    return true;
}

/* The tone and words of a partition-wide action: the partition named only with more than one. */
static const char *action_partition(const auto_action_t *a, char *buf, size_t len)
{
    return a->partition >= 1u && a->partition <= ALARM_MAX_PARTITIONS
               ? partition_named((uint8_t)(1u << (a->partition - 1u)), buf, len)
               : NULL;
}

/* BL-224's actions in words; false for any other kind. */
static bool panel_then_text(const auto_action_t *a, bool with_time, char *out, size_t len)
{
    char part[ALARM_NAME_LEN + 16];
    char name[48];
    switch (a->kind) {
    case AUTO_ACT_OUTPUT:
        ui_auto_output_name(a->output, name, sizeof(name));
        if (a->dev_action == ACT_OFF) {
            I18N_FORMAT(out, len, TR("Turn off {0}"), name);
        } else if (with_time && a->off_after_min > 0u) {
            char span[32];
            ui_auto_off_after_text(a->off_after_min, span, sizeof(span));
            I18N_FORMAT(out, len, TR("Turn on {0} for {1}"), name, span);
        } else {
            I18N_FORMAT(out, len, TR("Turn on {0}"), name);
        }
        return true;
    case AUTO_ACT_REMIND: {
        const char *p = action_partition(a, part, sizeof(part));
        /* TR: an automation's action (BL-224): a reminder to arm at its time; {0} the partition.
         * At most 47 bytes */
        p != NULL ? (void)I18N_FORMAT(out, len, TR("Remind to arm {0}"), p) : copy(out, len, TR("Remind to arm"));
        return true;
    }
    case AUTO_ACT_CHECK_ARMED: {
        const char *p = action_partition(a, part, sizeof(part));
        /* TR: an automation's action (BL-224): a notification if it is still armed at its time;
         * {0} the partition. At most 47 bytes */
        p != NULL ? (void)I18N_FORMAT(out, len, TR("Tell me if {0} is still armed"), p)
                  : copy(out, len, TR("Tell me if it's still armed"));
        return true;
    }
    case AUTO_ACT_CHIME: {
        const char *p = action_partition(a, part, sizeof(part));
        /* TR: an automation's action (BL-224): the door chime on or off; {0} the partition. At
         * most 47 bytes */
        if (a->option != 0u) {
            p != NULL ? (void)I18N_FORMAT(out, len, TR("Turn the door chime on in {0}"), p)
                      : copy(out, len, TR("Turn the door chime on"));
        } else {
            p != NULL ? (void)I18N_FORMAT(out, len, TR("Turn the door chime off in {0}"), p)
                      : copy(out, len, TR("Turn the door chime off"));
        }
        return true;
    }
    case AUTO_ACT_NOTIFY:
        /* TR: an automation's action (BL-224): a notification to the phones. At most 47 bytes */
        copy(out, len, TR("Send a notification"));
        return true;
    default: return false;
    }
}

/* The name an action's reasons say: its device's, or its output's. */
static void action_name(const auto_action_t *a, char *out, size_t len)
{
    if (a->kind == AUTO_ACT_OUTPUT) {
        ui_auto_output_name(a->output, out, len);
    } else {
        copy(out, len, a->name[0] != '\0' ? a->name : a->ref);
    }
}

/* It sends a notification (or "still armed" does). */
static bool notifies(const auto_rule_t *r)
{
    for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        if (r->then[i].kind == AUTO_ACT_NOTIFY || r->then[i].kind == AUTO_ACT_CHECK_ARMED) {
            return true;
        }
    }
    return false;
}

/* An action's sentence; with_time: "for 10 minutes" after a Turn on that turns off again (the
 * list), else without (the page shows it on its own row). */
static void then_text(const auto_action_t *a, bool with_time, char *out, size_t len)
{
    if (out == NULL || len == 0u) {
        return;
    }
    out[0] = '\0';
    if (a == NULL) {
        return;
    }
    if (a->kind == AUTO_ACT_ARM) {
        arm_text(a, out, len);
        return;
    }
    if (a->kind == AUTO_ACT_DISARM) {
        char part[ALARM_NAME_LEN + 16];
        const char *name = a->partition >= 1u && a->partition <= ALARM_MAX_PARTITIONS
                               ? partition_named((uint8_t)(1u << (a->partition - 1u)), part, sizeof(part))
                               : NULL;
        /* TR: a disarming automation's action (phase C); {0} is a partition's name. At most 47
         * bytes */
        name != NULL ? (void)I18N_FORMAT(out, len, TR("Disarm {0}"), name) : copy(out, len, TR("Disarm the panel"));
        return;
    }
    if (panel_then_text(a, with_time, out, len)) {
        return; /* BL-224: an output, a reminder, "still armed", the chime, a notification */
    }
    if (a->kind != AUTO_ACT_DEVICE) {
        copy(out, len, CANT_SHOW());
        return;
    }
    const char *n = a->name[0] != '\0' ? a->name : a->ref; /* the name it had when picked */
    /* TR: an automation's action; {0} is a device's name. At most 47 bytes */
    switch (a->dev_action) {
    case ACT_ON:
        if (with_time && a->off_after_min > 0u) {
            char span[32];
            ui_auto_off_after_text(a->off_after_min, span, sizeof(span));
            /* TR: an automation's action; {0} is a device's name, {1} how long ("10 minutes").
             * At most 47 bytes */
            I18N_FORMAT(out, len, TR("Turn on {0} for {1}"), n, span);
        } else {
            I18N_FORMAT(out, len, TR("Turn on {0}"), n);
        }
        break;
    case ACT_OFF: I18N_FORMAT(out, len, TR("Turn off {0}"), n); break;
    case ACT_TOGGLE: I18N_FORMAT(out, len, TR("Toggle {0}"), n); break;
    case ACT_LOCK: I18N_FORMAT(out, len, TR("Lock {0}"), n); break;
    case ACT_UNLOCK: I18N_FORMAT(out, len, TR("Unlock {0}"), n); break;
    case ACT_OPEN: I18N_FORMAT(out, len, TR("Open {0}"), n); break;
    case ACT_CLOSE: I18N_FORMAT(out, len, TR("Close {0}"), n); break;
    case ACT_STOP: I18N_FORMAT(out, len, TR("Stop {0}"), n); break;
    case ACT_RUN: I18N_FORMAT(out, len, TR("Run {0}"), n); break;
    case ACT_SET_MODE:
        if (strcmp(a->mode, NOTR("off")) == 0) { /* a thermostat is only ever turned off (D16) */
            I18N_FORMAT(out, len, TR("Turn off {0}"), n);
        } else {
            copy(out, len, CANT_SHOW());
        }
        break;
    default: copy(out, len, CANT_SHOW()); break;
    }
}

void ui_auto_then_text(const auto_action_t *a, char *out, size_t len)
{
    then_text(a, true, out, len);
}

/* Every action's sentence, joined by the house separator. */
static void thens_text(const auto_rule_t *r, char *out, size_t len)
{
    out[0] = '\0';
    for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        char one[96];
        then_text(&r->then[i], true, one, sizeof(one));
        const size_t used = strlen(out);
        if (used > 0u) {
            copy(out + used, len - used, NOTR(" \xC2\xB7 "));
        }
        const size_t at = strlen(out);
        copy(out + at, len - at, one);
    }
}

/* ---- the looks ----------------------------------------------------------------------------- */

const char *ui_auto_kind_glyph(uint8_t dev_kind, const char *ref)
{
    switch (dev_kind) {
    case DEV_LIGHT: return UI_ICON_LIGHTBULB;
    case DEV_SWITCH: return UI_ICON_PLUG; /* D21 */
    case DEV_LOCK: return UI_ICON_LOCK;
    case DEV_CLIMATE: return UI_ICON_THERMOMETER;
    case DEV_COVER:
        /* a garage door, as Smart Home draws it; any other cover is a door that closes */
        return ref != NULL && strstr(ref, NOTR("garage")) != NULL ? UI_ICON_WAREHOUSE : UI_ICON_DOOR_CLOSED;
    case DEV_FAN: return G_FAN;
    case DEV_VALVE: return UI_ICON_DROPLET;
    case DEV_SCENE:
    case DEV_SCRIPT: return UI_ICON_SPARKLES;
    default: return UI_ICON_ZAP;
    }
}

lv_color_t ui_auto_kind_tone(uint8_t k)
{
    switch (k) {
    case DEV_LOCK:
    case DEV_COVER: return ui_tone_green();
    case DEV_CLIMATE: return ui_tone_orange();
    case DEV_SCENE:
    case DEV_SCRIPT: return ui_tone_violet();
    case DEV_LIGHT:
    case DEV_SWITCH:
    case DEV_FAN:
    case DEV_VALVE: return ui_tone_blue();
    default: return ui_tone_gray();
    }
}

const char *ui_auto_when_glyph(const auto_trigger_t *w)
{
    if (w == NULL) {
        return UI_ICON_ZAP;
    }
    switch (w->kind) {
    case AUTO_TRIG_ARMED: return UI_ICON_SHIELD;
    case AUTO_TRIG_DISARMED: return UI_ICON_HOUSE;
    case AUTO_TRIG_ALARM: return UI_ICON_SIREN;
    case AUTO_TRIG_FIRE: return UI_ICON_FLAME;
    case AUTO_TRIG_ZONE_CLOSED: return UI_ICON_DOOR_CLOSED;
    case AUTO_TRIG_ZONE_OPENED: {
        const uint8_t cls = S.cfg != NULL && w->zone >= 1u && w->zone <= ALARM_MAX_ZONES
                                ? S.cfg->zones[w->zone - 1u].device_class
                                : ALARM_DEVCLASS_DOOR;
        if (cls == ALARM_DEVCLASS_DOOR || cls == ALARM_DEVCLASS_WINDOW || cls == ALARM_DEVCLASS_GARAGE_DOOR) {
            return UI_ICON_DOOR_OPEN;
        }
        return cls == ALARM_DEVCLASS_WATER ? UI_ICON_DROPLETS : ui_device_class_glyph((alarm_device_class_t)cls);
    }
    case AUTO_TRIG_TIME: return UI_ICON_CLOCK;
    case AUTO_TRIG_TROUBLE: /* BL-224 (drawing bl-224-trouble-which) */
        switch (w->trouble) {
        case AUTO_TROUBLE_AC: return UI_AUTO_G_UNPLUG;
        case AUTO_TROUBLE_BATTERY: return UI_AUTO_G_BATTERY_LOW;
        case AUTO_TROUBLE_SENSOR: return UI_AUTO_G_RADIO;
        case AUTO_TROUBLE_SIREN: return UI_ICON_SIREN;
        default: return UI_ICON_TRIANGLE_ALERT;
        }
    default: return UI_ICON_ZAP;
    }
}

lv_color_t ui_auto_when_tone(const auto_trigger_t *w)
{
    if (w == NULL) {
        return ui_tone_gray();
    }
    switch (w->kind) {
    case AUTO_TRIG_DISARMED:
    case AUTO_TRIG_ZONE_CLOSED: return ui_tone_green();
    case AUTO_TRIG_ALARM:
    case AUTO_TRIG_FIRE: return ui_tone_red();
    case AUTO_TRIG_ZONE_OPENED:
    case AUTO_TRIG_TROUBLE: return ui_tone_amber();
    case AUTO_TRIG_ARMED:
    case AUTO_TRIG_TIME: return ui_tone_blue();
    default: return ui_tone_gray();
    }
}

/* An action's chip: its device's (an arming's shield or moon). */
static const char *action_glyph(const auto_action_t *a)
{
    if (a->kind == AUTO_ACT_ARM) {
        return a->arm_level == ALARM_LEVEL_NIGHT ? UI_ICON_MOON : UI_ICON_SHIELD;
    }
    if (a->kind == AUTO_ACT_DISARM) {
        return UI_ICON_SHIELD_OFF;
    }
    switch (a->kind) { /* BL-224 (drawing bl-224-list) */
    case AUTO_ACT_OUTPUT: return a->dev_action == ACT_OFF ? UI_AUTO_G_TOGGLE_LEFT : UI_AUTO_G_TOGGLE_RIGHT;
    case AUTO_ACT_REMIND: return UI_ICON_BELL;
    case AUTO_ACT_CHECK_ARMED:
    case AUTO_ACT_NOTIFY: return UI_ICON_SEND;
    case AUTO_ACT_CHIME: return a->option != 0u ? UI_ICON_BELL_RING : UI_ICON_BELL_OFF;
    default: break;
    }
    return a->kind == AUTO_ACT_DEVICE ? ui_auto_kind_glyph(a->dev_kind, a->ref) : UI_ICON_ZAP;
}

static lv_color_t action_tone(const auto_action_t *a)
{
    if (a->kind == AUTO_ACT_ARM) {
        /* the arming tiles' tones (drawing automations-arm) */
        return a->arm_level == ALARM_LEVEL_AWAY   ? ui_tone_rose()
               : a->arm_level == ALARM_LEVEL_STAY ? ui_tone_blue()
                                                  : ui_tone_violet();
    }
    if (a->kind == AUTO_ACT_DISARM) {
        return ui_tone_amber(); /* less secure (drawing automation-disarm-page) */
    }
    switch (a->kind) { /* BL-224 */
    case AUTO_ACT_OUTPUT: return a->dev_action == ACT_OFF ? ui_tone_gray() : ui_tone_blue();
    case AUTO_ACT_REMIND: return ui_tone_amber();
    case AUTO_ACT_CHECK_ARMED:
    case AUTO_ACT_NOTIFY: return ui_tone_blue();
    case AUTO_ACT_CHIME: return a->option != 0u ? ui_tone_blue() : ui_tone_gray();
    default: break;
    }
    return ui_auto_kind_tone(a->dev_kind);
}

/* A rule's chip in the list: its first action's. */
static const char *rule_glyph(const auto_rule_t *r)
{
    return r->rec != AUTO_REC_OK || r->n == 0u ? UI_ICON_ZAP : action_glyph(&r->then[0]);
}

static lv_color_t rule_tone(const auto_rule_t *r)
{
    return action_tone(&r->then[0]);
}

/* ---- the outcome in words ------------------------------------------------------------------ */

/* "8:02 AM" for today, else the day; whether it is a time of day (then "at", else "on"). */
static bool when_text(int64_t epoch_ms, char *out, size_t len)
{
    char clock[24];
    ui_time_or_day_text(epoch_ms, out, len);
    ui_clock_text(epoch_ms, clock, sizeof(clock), true);
    return strcmp(out, clock) == 0;
}

/* Why an action didn't work, a whole sentence (DESIGN.md section 9 "Reasons"). */
static void reason_text(const auto_rule_t *r, const auto_status_t *st, char *out, size_t len)
{
    const uint8_t i = st->action_index < AUTO_ACTIONS ? st->action_index : 0u;
    char n[DEV_NAME_LEN];
    action_name(&r->then[i], n, sizeof(n)); /* a device's, or an output's (BL-224) */
    /* TR: why an automation's last run didn't work, after "didn't work at 8:02 AM: "; {0} is a
     * device's name. At most 47 bytes */
    switch (st->result) {
    case AUTO_RES_NO_ANSWER: I18N_FORMAT(out, len, TR("{0} didn't answer"), n); break;
    case AUTO_RES_UNREACHABLE: I18N_FORMAT(out, len, TR("{0} wasn't reachable"), n); break;
    case AUTO_RES_MISSING: I18N_FORMAT(out, len, TR("{0} isn't on the panel now"), n); break;
    case AUTO_RES_BUSY: I18N_FORMAT(out, len, TR("{0} was busy"), n); break;
    /* TR: why an automation's last run didn't work: the panel held it back (AUT-007). At most
     * 47 bytes */
    case AUTO_RES_HELD_BACK: copy(out, len, TR("Too many ran at once")); break;
    default: I18N_FORMAT(out, len, TR("{0} said no"), n); break;
    }
}

/* ---- an arming's or a disarming's outcome (phase C, BL-170; drawings automation-arm-*) ------ */

/* The rule's alarm action (Arm or Disarm), or NULL. */
static const auto_action_t *alarm_action_of(const auto_rule_t *r)
{
    for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        if (r->then[i].kind == AUTO_ACT_ARM || r->then[i].kind == AUTO_ACT_DISARM) {
            return &r->then[i];
        }
    }
    return NULL;
}

/* An arming or a disarming that didn't happen and should have: amber. */
static bool arm_failed(const auto_rule_t *r, const auto_status_t *st)
{
    return alarm_action_of(r) != NULL &&
           (st->result == AUTO_RES_NOT_READY || st->result == AUTO_RES_NO_WARNING ||
            st->result == AUTO_RES_ARM_REFUSED);
}

/* Who made it (D30: "Set up by Alex", never "Arms as Alex"); false when nobody is stored. */
static bool maker_name(const auto_rule_t *r, char *out, size_t len)
{
    if (r->made_by == 0u || S.cfg == NULL) {
        return false;
    }
    ui_user_name(S.cfg, r->made_by, out, len);
    return out[0] != '\0';
}

/* Who put it off (D20). */
static void putter_name(const auto_status_t *st, char *out, size_t len)
{
    out[0] = '\0';
    if (st->user != 0u && S.cfg != NULL) {
        ui_user_name(S.cfg, st->user, out, len);
    }
}

/* A put-off nobody is named for (st->user 0, or a user since deleted): the same words without
 * "by", never "put off by" and an empty name (the page's switch, D20). again: its arm time, or
 * NULL. */
static void put_off_nobody(const auto_rule_t *r, const auto_status_t *st, const char *again, char *out,
                           size_t len)
{
    (void)r;
    if (st->choice == AUTO_PUT_OFF_HOUR) {
        if (st->until_epoch_ms > 0) {
            char until[24];
            ui_clock_text(st->until_epoch_ms, until, sizeof(until), true);
            /* TR: an arming automation's state on its page: put off an hour, by nobody named;
             * {0} the new arm time. At most 95 bytes */
            I18N_FORMAT(out, len, TR("On \xC2\xB7 put off until {0}"), until);
        } else {
            /* TR: an arming automation's state on its page: put off an hour, by nobody named.
             * At most 95 bytes */
            copy(out, len, TR("On \xC2\xB7 put off for an hour"));
        }
    } else if (st->night != 0u) {
        /* TR: an arming automation's state on its page after "Not tonight", by nobody named;
         * {0} its arm time. At most 95 bytes */
        again != NULL ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 not tonight \xC2\xB7 arms again tomorrow at {0}"), again)
                      : copy(out, len, TR("On \xC2\xB7 not tonight"));
    } else {
        /* TR: an arming automation's state on its page after "Not today", by nobody named; {0}
         * its arm time. At most 95 bytes */
        again != NULL ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 not today \xC2\xB7 arms again tomorrow at {0}"), again)
                      : copy(out, len, TR("On \xC2\xB7 not today"));
    }
}

/* May the person who saved it still disarm partition p, as the panel's settings say now? A
 * refused disarm (NOT_AUTHORIZED) of someone who may was the alarm's (D22): the engine never
 * disarms after an alarm in that armed period. */
static bool maker_may_disarm(const auto_rule_t *r, uint8_t p)
{
    if (S.cfg == NULL || r->made_by == 0u || r->made_by > ALARM_MAX_USERS || p == 0u || p > ALARM_MAX_PARTITIONS) {
        return false;
    }
    const alarm_user_t *u = &S.cfg->users[r->made_by - 1u];
    if (u->authority == ALARM_AUTH_MASTER || u->authority == ALARM_AUTH_INSTALLER) {
        return true; /* they reach every partition, whatever their list (eng_user_in_partition) */
    }
    if (u->authority == ALARM_AUTH_NONE || u->authority == ALARM_AUTH_ARM_ONLY || u->authority == ALARM_AUTH_GUEST) {
        return false;
    }
    return u->partitions == 0u || (u->partitions & (1u << (p - 1u))) != 0u;
}

/* The page's switch detail for an arming's or a disarming's last outcome; false when this outcome
 * isn't one of theirs (the device words then apply). *alert: in amber. */
static bool arm_use_text(const auto_rule_t *r, const auto_status_t *st, char *out, size_t len,
                         bool *alert)
{
    const auto_action_t *a = alarm_action_of(r);
    if (a == NULL) {
        return false;
    }
    const bool arm = a->kind == AUTO_ACT_ARM;
    char at[32], who[ALARM_NAME_LEN + 16], why[96];
    const bool dated = st->at_epoch_ms > 0;
    const bool today = dated && when_text(st->at_epoch_ms, at, sizeof(at));
    switch (st->result) {
    case AUTO_RES_PUT_OFF: {
        putter_name(st, who, sizeof(who));
        char again[24];
        const bool timed = r->when.kind == AUTO_TRIG_TIME;
        if (timed) {
            ui_minute_text((int)r->when.minute, again, sizeof(again));
        }
        if (who[0] == '\0') {
            put_off_nobody(r, st, timed ? again : NULL, out, len);
        } else if (st->choice == AUTO_PUT_OFF_HOUR) {
            char until[24];
            if (st->until_epoch_ms > 0) {
                ui_clock_text(st->until_epoch_ms, until, sizeof(until), true);
                /* TR: an arming automation's state on its page: put off an hour (D20); {0} who,
                 * {1} the new arm time. At most 95 bytes */
                I18N_FORMAT(out, len, TR("On \xC2\xB7 put off by {0} until {1}"), who, until);
            } else {
                /* TR: an arming automation's state on its page: put off an hour, while the clock
                 * isn't set; {0} who. At most 95 bytes */
                I18N_FORMAT(out, len, TR("On \xC2\xB7 put off by {0} for an hour"), who);
            }
        } else if (st->night != 0u) {
            if (timed) {
                /* TR: an arming automation's state on its page after "Not tonight" (D20); {0}
                 * who, {1} its arm time. At most 95 bytes */
                I18N_FORMAT(out, len, TR("On \xC2\xB7 not tonight, put off by {0} \xC2\xB7 arms again tomorrow at {1}"),
                            who, again);
            } else {
                /* TR: an arming automation's state on its page after "Not tonight"; {0} who. At
                 * most 95 bytes */
                I18N_FORMAT(out, len, TR("On \xC2\xB7 not tonight, put off by {0}"), who);
            }
        } else if (timed) {
            /* TR: an arming automation's state on its page after "Not today" (a daytime arm
             * time); {0} who, {1} its arm time. At most 95 bytes */
            I18N_FORMAT(out, len, TR("On \xC2\xB7 not today, put off by {0} \xC2\xB7 arms again tomorrow at {1}"),
                        who, again);
        } else {
            /* TR: an arming automation's state on its page after "Not today"; {0} who. At most 95
             * bytes */
            I18N_FORMAT(out, len, TR("On \xC2\xB7 not today, put off by {0}"), who);
        }
        return true;
    }
    case AUTO_RES_ALREADY:
        /* Not a problem, so not amber (drawing automation-arm-already) */
        if (arm) {
            /* TR: an arming automation's state on its page: the partition was armed already;
             * {0} a time of day or a day. At most 79 bytes */
            !dated ? copy(out, len, TR("On \xC2\xB7 already armed"))
            : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already armed at {0}"), at)
                    : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already armed on {0}"), at);
        } else {
            /* TR: a disarming automation's state on its page: the partition was disarmed
             * already; {0} a time of day or a day. At most 79 bytes */
            !dated ? copy(out, len, TR("On \xC2\xB7 already disarmed"))
            : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already disarmed at {0}"), at)
                    : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already disarmed on {0}"), at);
        }
        return true;
    case AUTO_RES_ARM_REFUSED:
        if (arm && st->alarm_res == (uint8_t)ALARM_ERR_NOT_AUTHORIZED && maker_name(r, who, sizeof(who))) {
            *alert = true;
            /* TR: an arming automation's state on its page: the person who set it up may no
             * longer arm (deleted, demoted or moved); {0} that person. At most 95 bytes */
            I18N_FORMAT(out, len, TR("Can't run: {0} can no longer arm this partition"), who);
            return true;
        }
        if (!arm && st->alarm_res == (uint8_t)ALARM_ERR_NOT_AUTHORIZED) {
            char part[ALARM_NAME_LEN + 16];
            const bool named = a->partition >= 1u && a->partition <= ALARM_MAX_PARTITIONS &&
                               partition_named((uint8_t)(1u << (a->partition - 1u)), part, sizeof(part)) != NULL;
            if (!maker_may_disarm(r, a->partition)) {
                if (!maker_name(r, who, sizeof(who))) {
                    /* nobody named, and not the alarm's either */
                    copy(why, sizeof(why), TR("the panel said no"));
                    break;
                }
                *alert = true;
                /* TR: a disarming automation's state on its page: the person who set it up may no
                 * longer disarm that partition; {0} that person, {1} the partition. At most 95
                 * bytes */
                named ? (void)I18N_FORMAT(out, len, TR("Can't run: {0} can no longer disarm {1}"), who, part)
                      /* TR: the same with one partition; {0} that person. At most 95 bytes */
                      : (void)I18N_FORMAT(out, len, TR("Can't run: {0} can no longer disarm the panel"), who);
                return true;
            }
            /* TR: why an automation didn't disarm, after "didn't disarm at 10:00 AM: ": an alarm
             * was sounding, and an automation never silences one (D22); {0} the partition. At
             * most 63 bytes */
            named ? (void)I18N_FORMAT(why, sizeof(why), TR("{0}'s alarm was sounding"), part)
                  /* TR: the same with one partition. At most 63 bytes */
                  : copy(why, sizeof(why), TR("the alarm was sounding"));
            break;
        }
        /* TR: why an automation didn't arm or disarm, after "didn't arm at 10:30 PM: ": the
         * panel refused it. At most 47 bytes */
        copy(why, sizeof(why), TR("the panel said no"));
        break;
    case AUTO_RES_NOT_READY:
        if (st->zone >= 1u && st->zone <= ALARM_MAX_ZONES && S.cfg != NULL) {
            ui_zone_name(S.cfg, st->zone, who, sizeof(who));
            /* TR: why an automation didn't arm, after "didn't arm at 10:30 PM: "; {0} the zone.
             * At most 63 bytes */
            I18N_FORMAT(why, sizeof(why), TR("{0} was open"), who);
        } else {
            /* TR: why an automation didn't arm, after "didn't arm at 10:30 PM: ". At most 47
             * bytes */
            copy(why, sizeof(why), TR("a zone was open"));
        }
        break;
    case AUTO_RES_NO_WARNING:
        /* TR: why an automation didn't arm, after "didn't arm at 10:30 PM: ": its time came
         * without a whole warning (AUT-012). At most 47 bytes */
        copy(why, sizeof(why), TR("no time to warn first"));
        break;
    default: return false;
    }
    *alert = true;
    if (arm) {
        /* TR: an arming automation's state on its page; {0} a time of day ("10:30 PM") or a
         * day, {1} why ("Kitchen Window was open"). At most 95 bytes */
        !dated ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't arm: {0}"), why)
        : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't arm at {0}: {1}"), at, why)
                : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't arm on {0}: {1}"), at, why);
    } else {
        /* TR: a disarming automation's state on its page; {0} a time of day or a day, {1} why.
         * At most 95 bytes */
        !dated ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't disarm: {0}"), why)
        : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't disarm at {0}: {1}"), at, why)
                : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't disarm on {0}: {1}"), at, why);
    }
    return true;
}

/* BL-224: the page's switch detail for a reminder's, "still armed"'s or the door chime's own
 * outcomes (drawing bl-224-page-remind); false when it isn't one of theirs. */
static bool panel_use_text(const auto_rule_t *r, const auto_status_t *st, char *out, size_t len, bool *alert)
{
    const uint8_t i = st->action_index < AUTO_ACTIONS ? st->action_index : 0u;
    if (r->n == 0u || i >= r->n) {
        return false;
    }
    const auto_action_t *a = &r->then[i];
    char at[32];
    const bool dated = st->at_epoch_ms > 0;
    const bool today = dated && when_text(st->at_epoch_ms, at, sizeof(at));
    switch (a->kind) {
    case AUTO_ACT_REMIND:
        if (st->result == AUTO_RES_DONE) {
            /* TR: a reminder's state on its page: it reminded (a chime and Home's note); {0} a
             * time of day or a day. At most 79 bytes */
            !dated ? copy(out, len, TR("On \xC2\xB7 reminded"))
            : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 reminded at {0}"), at)
                    : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 reminded on {0}"), at);
            return true;
        }
        if (st->result == AUTO_RES_ALREADY) { /* not amber: nothing was needed */
            !dated ? copy(out, len, TR("On \xC2\xB7 already armed"))
            : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already armed at {0}"), at)
                    : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already armed on {0}"), at);
            return true;
        }
        return false;
    case AUTO_ACT_CHECK_ARMED:
        if (st->result == AUTO_RES_ALREADY) { /* disarmed by then: nobody needed telling */
            !dated ? copy(out, len, TR("On \xC2\xB7 already disarmed"))
            : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already disarmed at {0}"), at)
                    : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 already disarmed on {0}"), at);
            return true;
        }
        return false;
    case AUTO_ACT_CHIME:
        if (st->result == AUTO_RES_ALREADY) {
            /* TR: a door chime automation's state on its page: the chime was so already; {0} a
             * time of day or a day. At most 79 bytes */
            if (a->option != 0u) {
                today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 the door chime was already on at {0}"), at)
                      : copy(out, len, TR("On \xC2\xB7 the door chime was already on"));
            } else {
                today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 the door chime was already off at {0}"), at)
                      : copy(out, len, TR("On \xC2\xB7 the door chime was already off"));
            }
            return true;
        }
        if (st->result == AUTO_RES_ARM_REFUSED) {
            *alert = true;
            const char *why = TR("the panel said no");
            !dated ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't work: {0}"), why)
            : today ? (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't work at {0}: {1}"), at, why)
                    : (void)I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't work on {0}: {1}"), at, why);
            return true;
        }
        return false;
    default: return false;
    }
}

/* The list row's line for a put-off (grey: nothing is wrong, D20); false otherwise. */
static bool put_off_line(const auto_rule_t *r, const auto_status_t *st, char *out, size_t len)
{
    const bool put_off = st->result == AUTO_RES_PUT_OFF && alarm_action_of(r) != NULL;
    if (!put_off || out == NULL) {
        return put_off;
    }
    char who[ALARM_NAME_LEN + 16], when[24];
    putter_name(st, who, sizeof(who));
    const bool nobody = who[0] == '\0'; /* no name to say: the words without "by" */
    if (st->choice == AUTO_PUT_OFF_HOUR && st->until_epoch_ms > 0) {
        ui_clock_text(st->until_epoch_ms, when, sizeof(when), true);
        /* TR: an arming automation's line in the list, put off an hour (D20); {0} who, {1} the
         * new arm time. At most 95 bytes */
        nobody ? (void)I18N_FORMAT(out, len, TR("Put off until {0}"), when)
               : (void)I18N_FORMAT(out, len, TR("Put off by {0} until {1}"), who, when);
    } else if (st->choice != AUTO_PUT_OFF_HOUR && r->when.kind == AUTO_TRIG_TIME) {
        ui_minute_text((int)r->when.minute, when, sizeof(when));
        /* TR: an arming automation's line in the list after "Not tonight" or "Not today"; {0}
         * its arm time. At most 95 bytes */
        I18N_FORMAT(out, len, TR("Arms again tomorrow at {0}"), when);
    } else {
        /* TR: an arming automation's line in the list, put off; {0} who. At most 95 bytes */
        nobody ? copy(out, len, TR("Put off"))
               : (void)I18N_FORMAT(out, len, TR("Put off by {0}"), who);
    }
    return true;
}

/* The list row's line: grey for a put-off, amber for a problem. */
static lv_color_t sub_tone(const auto_rule_t *r, const auto_status_t *st)
{
    return put_off_line(r, st, NULL, 0u) ? ui_tone_gray() : ui_tone_amber();
}

/* The page's "Use this automation" detail (D21's words); *alert: in amber; *usable: false when
 * only Delete works. */
static void use_text(const auto_rule_t *r, const auto_status_t *st, const rule_state_t *rs,
                     char *out, size_t len, bool *alert, bool *usable)
{
    *alert = false;
    *usable = true;
    char at[32], why[96];
    switch (rs->prob) {
    case PROB_NEWER:
        *usable = false;
        /* TR: an automation's state on its page. At most 95 bytes */
        copy(out, len, TR("Can't run: made by newer software"));
        return;
    case PROB_BROKEN:
        *usable = false;
        /* TR: an automation's state on its page: it can't be read whole, so only Delete works.
         * At most 95 bytes */
        copy(out, len, TR("Can't run: it can only be deleted"));
        return;
    default: break;
    }
    if (r->off) {
        /* TR: an automation's state on its page, while it is turned off. At most 95 bytes */
        copy(out, len, TR("Off \xC2\xB7 it won't run"));
        return;
    }
    switch (rs->prob) {
    case PROB_ZONE: {
        char zone[ALARM_NAME_LEN + 16];
        ui_zone_name(S.cfg, r->when.zone, zone, sizeof(zone));
        *alert = true;
        /* TR: an automation's state on its page: its zone was made a kind that can't start one;
         * {0} is the zone's name. At most 79 bytes */
        I18N_FORMAT(out, len, TR("Can't run: {0} can't start an automation now"), zone);
        return;
    }
    case PROB_KIND:
        *alert = true;
        /* TR: an automation's state on its page: a device changed kind. At most 95 bytes */
        copy(out, len, TR("Can't run: open it and pick the device again"));
        return;
    case PROB_OPTION:
        *alert = true;
        option_words(rs->verb, out, len);
        return;
    case PROB_FIX:
        *alert = true;
        /* TR: an automation's state, on its page and in the list. At most 95 bytes */
        copy(out, len, TR("Can't run: open it to fix it"));
        return;
    case PROB_NO_SERVICE:
        *alert = true;
        /* TR: an automation's state, on its page and in the list (BL-224): it sends a
         * notification and no phone service is set up in Integrations. At most 95 bytes */
        copy(out, len, TR("Can't send: no phone notifications are set up"));
        return;
    default: break;
    }
    if (rs->clock_wait) {
        /* TR: an automation's state on its page: it runs at a time or in a window, and the
         * clock isn't set. At most 95 bytes */
        copy(out, len, TR("On \xC2\xB7 waits until the clock is set"));
        return;
    }
    if (arm_use_text(r, st, out, len, alert)) {
        return; /* an arming's or a disarming's outcome (phase C) */
    }
    if (panel_use_text(r, st, out, len, alert)) {
        return; /* a reminder's, "still armed"'s or the chime's (BL-224) */
    }
    const bool dated = st->at_epoch_ms > 0;
    const bool today = dated && when_text(st->at_epoch_ms, at, sizeof(at));
    if (failed_result(st->result)) {
        *alert = true;
        reason_text(r, st, why, sizeof(why));
        /* TR: an automation's state on its page; {0} is a time of day ("8:02 AM") or a day
         * ("Sep 19"), {1} why ("Front Door didn't answer"). At most 79 bytes */
        if (!dated) {
            I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't work: {0}"), why);
        } else if (today) {
            I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't work at {0}: {1}"), at, why); /* TR: an automation's state; {0} a time or a day, {1} why. At most 79 bytes */
        } else {
            I18N_FORMAT(out, len, TR("On \xC2\xB7 didn't work on {0}: {1}"), at, why); /* TR: an automation's state; {0} a time or a day, {1} why. At most 79 bytes */
        }
        return;
    }
    if (st->result == AUTO_RES_DONE) {
        /* TR: an automation's state on its page; {0} is a time of day ("7:42 PM") or a day
         * ("Sep 19"). At most 79 bytes */
        if (!dated) {
            copy(out, len, st->tried ? TR("On \xC2\xB7 tried") : TR("On \xC2\xB7 ran"));
        } else if (st->tried) {
            I18N_FORMAT(out, len, today ? TR("On \xC2\xB7 tried at {0}") : TR("On \xC2\xB7 tried on {0}"), at); /* TR: an automation's state; {0} a time or a day. At most 79 bytes */
        } else {
            I18N_FORMAT(out, len, today ? TR("On \xC2\xB7 ran at {0}") : TR("On \xC2\xB7 ran on {0}"), at); /* TR: an automation's state; {0} a time or a day. At most 79 bytes */
        }
        return;
    }
    /* TR: an automation's state on its page, before its first run. At most 95 bytes */
    copy(out, len, TR("On \xC2\xB7 hasn't run since the panel started"));
}

/* The list row's amber line (screen 2 and 6): "" when nothing is wrong. */
static void row_sub_text(const auto_rule_t *r, const auto_status_t *st, const rule_state_t *rs,
                         char *out, size_t len)
{
    out[0] = '\0';
    switch (rs->prob) {
    /* TR: the amber line of an automation's row in the list. At most 95 bytes */
    case PROB_NEWER: copy(out, len, TR("Made by newer software")); return;
    case PROB_BROKEN: copy(out, len, TR("Can't run: it can only be deleted")); return;
    case PROB_KIND: copy(out, len, TR("Can't run: open it and pick the device again")); return;
    case PROB_OPTION: option_words(rs->verb, out, len); return;
    case PROB_ZONE:
    case PROB_FIX: copy(out, len, TR("Can't run: open it to fix it")); return;
    case PROB_NO_SERVICE: copy(out, len, TR("Can't send: no phone notifications are set up")); return;
    default: break;
    }
    for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        if (rs->missing & (1u << i)) {
            const char *n = r->then[i].name[0] != '\0' ? r->then[i].name : r->then[i].ref;
            I18N_FORMAT(out, len, TR("{0} isn't on the panel now"), n);
            return;
        }
    }
    if (put_off_line(r, st, out, len)) {
        return;
    }
    const bool chime_refused = st->result == AUTO_RES_ARM_REFUSED && st->action_index < r->n &&
                               st->action_index < AUTO_ACTIONS && r->then[st->action_index].kind == AUTO_ACT_CHIME;
    if (failed_result(st->result) || arm_failed(r, st) || chime_refused) {
        char at[32];
        if (st->at_epoch_ms <= 0) {
            /* TR: the amber line of an automation's row: its last run didn't work. At most 95
             * bytes */
            copy(out, len, TR("Didn't work"));
        } else if (when_text(st->at_epoch_ms, at, sizeof(at))) {
            /* TR: the amber line of an automation's row; {0} is a time of day. At most 79 bytes */
            I18N_FORMAT(out, len, TR("Didn't work at {0}"), at);
        } else {
            /* TR: the amber line of an automation's row; {0} is a day ("Sep 19"). At most 79
             * bytes */
            I18N_FORMAT(out, len, TR("Didn't work on {0}"), at);
        }
    }
}

/* What a page's shape is drawn from: the rules' problems and missing devices, the clock. */
static uint32_t shape_mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

static uint32_t rule_shape(uint32_t h, const auto_rule_t *r, const rule_state_t *rs)
{
    h = shape_mix(h, r->slot);
    h = shape_mix(h, (uint32_t)rs->prob);
    h = shape_mix(h, rs->verb);
    h = shape_mix(h, rs->missing);
    return shape_mix(h, rs->clock_wait ? 1u : 0u);
}

/* ---- refusals ------------------------------------------------------------------------------ */

static void again_soon(void *unused);

static const char *check_words(auto_check_t why)
{
    /* TR: a toast when an automation can't be saved. At most 127 bytes */
    switch (why) {
    case AUTO_ERR_INCOMPLETE: return TR("Pick when it runs and what it does first");
    case AUTO_ERR_NOT_ALLOWED: return TR("An automation can't do that");
    case AUTO_ERR_ZONE: return TR("That sensor can't start an automation");
    case AUTO_ERR_PARTITION: return TR("That partition isn't in use");
    case AUTO_ERR_TIME: return TR("Pick a time and at least one day");
    case AUTO_ERR_WINDOW: return TR("From and To can't be the same time");
    case AUTO_ERR_ARM_ALONE: return TR("Arming has to be its only action");
    case AUTO_ERR_MAKER: return TR("The master or installer code saves this one");
    case AUTO_ERR_SAME_DEVICE: return TR("It already does something with that device");
    case AUTO_ERR_UNSUPPORTED: return TR("This software can't run that yet");
    case AUTO_ERR_NEEDS_CONFIRM: return TR("It has to be confirmed first");
    case AUTO_ERR_LESS_SECURE_OFF:
        return TR("Your installer has turned off automations that unlock, open or disarm");
    case AUTO_ERR_UNDOES_ARMING: return TR("It can't disarm what its own trigger just armed");
    case AUTO_ERR_OUTPUT: return TR("That output isn't one for automations now");
    case AUTO_ERR_NEEDS_TIME: return TR("A reminder runs only at a time of day");
    default: return TR("An automation can't do that");
    }
}

static const char *save_words(auto_save_t res, auto_check_t why)
{
    /* TR: a toast when a change to the automations is refused. At most 127 bytes */
    switch (res) {
    case AUTO_SAVE_INVALID: return check_words(why);
    case AUTO_SAVE_CHANGED: return TR("Changed somewhere else meanwhile: here it is now");
    case AUTO_SAVE_FULL: return TR("16 is the most. Delete one to add another.");
    case AUTO_SAVE_NO_ROOM: return TR("The panel has no room to store another");
    case AUTO_SAVE_READ_ONLY: return TR("The automations couldn't all be read, so they can't be changed");
    case AUTO_SAVE_NOT_EDITABLE: return TR("This one can only be deleted");
    case AUTO_SAVE_NOT_FOUND: return TR("That automation is gone");
    case AUTO_SAVE_LOCKED: return TR("Automations can be changed once the system is disarmed");
    default: return TR("Couldn't save it. Try again.");
    }
}

/* A refusal (AUT-002, AUT-008, AUT-010): the error beep and its reason; the page is drawn again
 * from what is stored now. */
static void refused(auto_save_t res, auto_check_t why)
{
    ui_beep(UI_BEEP_ERROR);
    ui_toast(save_words(res, why));
    lv_async_call(again_soon, NULL);
}

static uint8_t maker(void)
{
    if (S.auth_user != 0u) {
        return S.auth_user;
    }
    return S.program ? (uint8_t)ALARM_USER_INSTALLER : 0u;
}

bool ui_auto_save(const auto_rule_t *r, uint16_t *uid_out)
{
    if (r == NULL || S.be.auto_save == NULL) {
        return false;
    }
    auto_check_t why = AUTO_OK;
    const auto_save_t res = S.be.auto_save(S.be.ctx, r, maker(), A.gen, &why, uid_out);
    if (res != AUTO_SAVE_OK) {
        refused(res, why);
        return false;
    }
    /* The rebuild reads the rules and the generation together, so this change shows and an edit
     * saved elsewhere meanwhile isn't taken in unseen (AUT-008). */
    lv_async_call(again_soon, NULL);
    return true;
}

static bool on_page_of(uint8_t slot);

bool ui_auto_delete(uint8_t slot)
{
    if (S.be.auto_delete_slot == NULL) {
        return false;
    }
    const auto_save_t res = S.be.auto_delete_slot(S.be.ctx, slot, A.gen);
    if (res != AUTO_SAVE_OK) {
        refused(res, AUTO_OK);
        return false;
    }
    lv_async_call(again_soon, NULL); /* the rebuild takes the generation with the rules */
    if (on_page_of(slot)) {
        ui_page_back(); /* screen 23: back to the list, no toast */
    }
    return true;
}

/* ---- the list (screens 2-6) ----------------------------------------------------------------- */

static void open_rule(void *ctx)
{
    ui_page_open(PAGE_AUTOMATION, (int32_t)(intptr_t)ctx);
}

static void add_one(void *ctx)
{
    (void)ctx;
    ui_auto_add_start();
}

static void build_list(lv_obj_t *body, char *note, size_t nlen)
{
    read_devices();
    /* TR: Settings > Automations, the row that adds one */
    lv_obj_t *add = ui_row_action(body, UI_ICON_PLUS, TR("Add an automation"), ui_tone_blue(), add_one, NULL);
    if (s_nrules == 0u) {
        /* TR: Settings > Automations with none yet: what an automation is. At most 127 bytes */
        ui_paragraph(body,
                     TR("The panel can do things by itself, like arming at bedtime or telling your phone when a door "
                        "opens at night."),
                     ui_tone_gray());
        return;
    }
    /* TR: Settings > Automations, the line under the title; {0} how many there are, {1} the most
     * (16). At most 120 bytes */
    I18N_FORMAT(note, nlen, TR("{0} of {1} automations"), (unsigned)s_nrules, (unsigned)AUTO_MAX);
    if (s_nrules >= AUTO_MAX) {
        ui_row_disable(add);
        /* TR: Settings > Automations, under the dimmed Add row at 16. At most 127 bytes */
        ui_paragraph(body, TR("16 is the most. Delete one to add another."), ui_tone_gray());
    }
    bool clock = false;
    for (size_t i = 0; i < s_nrules; i++) {
        clock = clock || (s_rules[i].rec == AUTO_REC_OK && needs_clock(&s_rules[i]));
    }
    if (clock && !ui_clock_known()) {
        /* TR: Settings > Automations, the amber line above the list while the clock isn't set.
         * At most 127 bytes */
        ui_paragraph(body, TR("The clock isn't set, so times wait until it is."), ui_tone_amber());
    }
    uint32_t shape = 2166136261u;
    shape = shape_mix(shape, (clock && !ui_clock_known()) ? 1u : 0u);
    for (size_t i = 0; i < s_nrules && i < AUTO_MAX; i++) {
        const auto_rule_t *r = &s_rules[i];
        auto_status_t st;
        rule_state_t rs;
        status_of(r, &st);
        rule_state(r, &st, &rs);
        shape = rule_shape(shape, r, &rs);
        char title[96], detail[160];
        if (rs.prob == PROB_NEWER || rs.prob == PROB_BROKEN) {
            if (r->when.kind > AUTO_TRIG_NONE && r->when.kind < AUTO_TRIG_COUNT) {
                ui_auto_when_text(&r->when, title, sizeof(title));
            } else {
                copy(title, sizeof(title), CANT_SHOW());
            }
            copy(detail, sizeof(detail), CANT_SHOW());
        } else {
            ui_auto_when_text(&r->when, title, sizeof(title));
            thens_text(r, detail, sizeof(detail));
        }
        /* grey: it can't run; one that runs but can't send keeps its tone (drawing bl-224-list) */
        const bool gray = r->off || (rs.prob != PROB_NONE && rs.prob != PROB_NO_SERVICE) || rs.missing != 0u;
        lv_obj_t *row = ui_row_link(body, rule_glyph(r), gray ? ui_tone_gray() : rule_tone(r), title, detail,
                                    /* TR: an automation's row in the list while it is turned off */
                                    r->off ? TR_C("automation state", "Off") : NULL, UI_ICON_CHEVRON_RIGHT,
                                    open_rule, (void *)(intptr_t)r->slot);
        row_sub_text(r, &st, &rs, A.subs[i], sizeof(A.subs[i]));
        ui_row_sub(row, A.subs[i], sub_tone(r, &st));
        A.rows[i] = row;
        A.row_slot[i] = r->slot;
        A.nrows = (uint8_t)(i + 1u);
    }
    A.shape = shape;
}

/* The list's amber lines, once a second: rewritten only when their words changed (UI-023). */
static void tick_list(void)
{
    uint32_t shape = 2166136261u;
    bool clock = false;
    for (size_t i = 0; i < s_nrules; i++) {
        clock = clock || (s_rules[i].rec == AUTO_REC_OK && needs_clock(&s_rules[i]));
    }
    shape = shape_mix(shape, (clock && !ui_clock_known()) ? 1u : 0u);
    static UI_SLOW_BSS char subs[AUTO_MAX][96]; /* LVGL task only */
    static UI_SLOW_BSS bool grey[AUTO_MAX];
    for (size_t i = 0; i < s_nrules && i < AUTO_MAX; i++) {
        auto_status_t st;
        rule_state_t rs;
        status_of(&s_rules[i], &st);
        rule_state(&s_rules[i], &st, &rs);
        shape = rule_shape(shape, &s_rules[i], &rs);
        row_sub_text(&s_rules[i], &st, &rs, subs[i], sizeof(subs[i]));
        grey[i] = put_off_line(&s_rules[i], &st, NULL, 0u);
    }
    if (shape != A.shape || s_nrules != A.nrows) {
        ui_page_rebuild();
        return;
    }
    for (size_t i = 0; i < s_nrules && i < AUTO_MAX; i++) {
        if (strcmp(subs[i], A.subs[i]) != 0 && A.rows[i] != NULL) {
            copy(A.subs[i], sizeof(A.subs[i]), subs[i]);
            ui_row_sub(A.rows[i], A.subs[i], grey[i] ? ui_tone_gray() : ui_tone_amber());
        }
    }
}

/* ---- an automation's page (screens 16-20) --------------------------------------------------- */

static void on_use(bool on, void *ctx)
{
    (void)ctx;
    if (S.be.auto_set_off == NULL) {
        return;
    }
    const auto_save_t res = S.be.auto_set_off(S.be.ctx, A.uid, !on, A.gen);
    if (res != AUTO_SAVE_OK) {
        refused(res, AUTO_OK); /* the switch goes back to what is stored */
        return;
    }
    lv_async_call(again_soon, NULL); /* its detail says what it does now; the generation with it */
}

static void on_when(void *ctx)
{
    (void)ctx;
    ui_auto_edit_when(A.slot);
}

static void on_window(void *ctx)
{
    (void)ctx;
    ui_auto_edit_window(A.slot);
}

static void on_action(void *ctx)
{
    ui_auto_edit_action(A.slot, (uint8_t)(intptr_t)ctx);
}

static void on_off_after(void *ctx)
{
    ui_auto_edit_off_after(A.slot, (uint8_t)(intptr_t)ctx);
}

static void on_add_action(void *ctx)
{
    (void)ctx;
    ui_auto_add_action(A.slot);
}

static void on_delete(void *ctx)
{
    (void)ctx;
    ui_auto_delete_ask(A.slot);
}

static void on_try(void *ctx)
{
    (void)ctx;
    if (S.be.auto_try == NULL) {
        return;
    }
    const auto_try_t res = S.be.auto_try(S.be.ctx, A.uid);
    if (res == AUTO_TRY_OK) {
        A.ticked = lv_tick_get() - 1000u; /* its outcome goes into the switch's detail, no toast */
        return;
    }
    ui_beep(UI_BEEP_ERROR);
    /* TR: a toast when "Try it now" can't start. At most 127 bytes */
    ui_toast(res == UI_AUTO_TRY_BUSY ? TR("The panel is busy. Try again in a moment.")
             : res == AUTO_TRY_UNKNOWN ? TR("That automation is gone")
                                       : TR("Can't run: open it to fix it"));
}

static void when_detail(const auto_trigger_t *w, char *out, size_t len)
{
    out[0] = '\0';
    switch (w->kind) {
    case AUTO_TRIG_ZONE_OPENED:
    case AUTO_TRIG_ZONE_CLOSED:
        /* TR: under an automation's trigger on its page; {0} the zone's number. At most 31
         * bytes */
        I18N_FORMAT(out, len, TR_C("default zone name", "Zone {0}"), (unsigned)w->zone);
        break;
    /* TR: under an automation's trigger on its page. At most 63 bytes */
    case AUTO_TRIG_ARMED: copy(out, len, TR("Once exit time is over")); break;
    /* TR: under an automation's trigger on its page. At most 63 bytes */
    case AUTO_TRIG_ALARM: copy(out, len, TR("Never for a silent alarm")); break;
    default: break;
    }
}

/* Under an Arm or a Disarm on its page: who set it up (D30: never "Arms as Alex"; it acts with
 * that person's permission, re-checked each time) and, for an Arm, its warning and whether it
 * bypasses open zones (only while the installer option allows it, D29). */
static void alarm_detail(const auto_rule_t *r, const auto_action_t *a, char *out, size_t len)
{
    out[0] = '\0';
    char who[ALARM_NAME_LEN + 16];
    if (!maker_name(r, who, sizeof(who))) {
        return;
    }
    char saver[ALARM_NAME_LEN + 16];
    saver[0] = '\0';
    if (r->saved_by != 0u && r->saved_by != r->made_by && S.cfg != NULL) {
        ui_user_name(S.cfg, r->saved_by, saver, sizeof(saver));
    }
    if (a->kind == AUTO_ACT_DISARM) {
        /* TR: under a disarming automation's action on its page; {0} who saved it. At most 79
         * bytes */
        I18N_FORMAT(out, len, TR("Set up by {0}"), who);
    } else if (saver[0] != '\0' && a->bypass_open != 0u && A.less_secure) {
        /* TR: under an arming automation the installer set up (BL-224): it uses the system
         * master's permission; {0} who set it up, {1} the master, {2} the minutes it warns first.
         * At most 127 bytes */
        I18N_FORMAT(out, len,
                    TRN("Set up by {0} \xC2\xB7 uses {1}'s permission \xC2\xB7 warns {2} minute before \xC2\xB7 bypasses open zones",
                        "Set up by {0} \xC2\xB7 uses {1}'s permission \xC2\xB7 warns {2} minutes before \xC2\xB7 bypasses open zones",
                        a->warn_min),
                    saver, who, (unsigned)a->warn_min);
    } else if (saver[0] != '\0') {
        /* TR: under an arming automation the installer set up (BL-224, drawing
         * bl-224-page-installer-arm); {0} who set it up, {1} the master, {2} the minutes it warns
         * first. At most 127 bytes */
        I18N_FORMAT(out, len,
                    TRN("Set up by {0} \xC2\xB7 uses {1}'s permission \xC2\xB7 warns {2} minute before",
                        "Set up by {0} \xC2\xB7 uses {1}'s permission \xC2\xB7 warns {2} minutes before", a->warn_min),
                    saver, who, (unsigned)a->warn_min);
    } else if (a->bypass_open != 0u && A.less_secure) {
        /* TR: under an arming automation's action on its page; {0} who saved it, {1} the minutes
         * it warns first. At most 95 bytes */
        I18N_FORMAT(out, len,
                    TRN("Set up by {0} \xC2\xB7 warns {1} minute before \xC2\xB7 bypasses open zones",
                        "Set up by {0} \xC2\xB7 warns {1} minutes before \xC2\xB7 bypasses open zones", a->warn_min),
                    who, (unsigned)a->warn_min);
    } else {
        /* TR: under an arming automation's action on its page; {0} who saved it, {1} the minutes
         * it warns first. At most 79 bytes */
        I18N_FORMAT(out, len,
                    TRN("Set up by {0} \xC2\xB7 warns {1} minute before", "Set up by {0} \xC2\xB7 warns {1} minutes before",
                        a->warn_min),
                    who, (unsigned)a->warn_min);
    }
}

/* A time-of-day Disarm with no automation that arms its partition again (DESIGN.md "Disarming"
 * 5, drawing automation-disarm-page). */
static bool disarm_alone(const auto_rule_t *r)
{
    const auto_action_t *a = alarm_action_of(r);
    if (r->when.kind != AUTO_TRIG_TIME || a == NULL || a->kind != AUTO_ACT_DISARM) {
        return false;
    }
    for (size_t i = 0; i < s_nrules; i++) {
        const auto_action_t *b = alarm_action_of(&s_rules[i]);
        if (s_rules[i].rec == AUTO_REC_OK && !s_rules[i].off && b != NULL && b->kind == AUTO_ACT_ARM &&
            b->partition == a->partition) {
            return false;
        }
    }
    return true;
}

/* A time-of-day "Turn the door chime off" with no automation that turns that partition's chime on
 * again (BL-224, drawing bl-224-chime). */
static bool chime_off_alone(const auto_rule_t *r)
{
    const auto_action_t *off = NULL;
    for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        if (r->then[i].kind == AUTO_ACT_CHIME && r->then[i].option == 0u) {
            off = &r->then[i];
        }
    }
    if (r->when.kind != AUTO_TRIG_TIME || off == NULL) {
        return false;
    }
    for (size_t k = 0; k < s_nrules; k++) {
        const auto_rule_t *o = &s_rules[k];
        for (uint8_t i = 0; o->rec == AUTO_REC_OK && !o->off && i < o->n && i < AUTO_ACTIONS; i++) {
            if (o->then[i].kind == AUTO_ACT_CHIME && o->then[i].option != 0u && o->then[i].partition == off->partition) {
                return false;
            }
        }
    }
    return true;
}

bool ui_auto_may_add_second(const auto_action_t *first)
{
    switch (first->kind) {
    case AUTO_ACT_DEVICE:
    case AUTO_ACT_DISARM:
    case AUTO_ACT_OUTPUT:
    case AUTO_ACT_CHIME:
    case AUTO_ACT_NOTIFY: return true;
    default: return false;
    }
}

/* Under BL-224's actions on the page (drawings bl-224-page-*): an output's terminal, what a
 * reminder does, where a notification goes (amber when nowhere); false for any other kind. */
static bool panel_detail(const auto_rule_t *r, const auto_action_t *a, char *out, size_t len, bool *alert)
{
    *alert = false;
    out[0] = '\0';
    char services[48];
    char quote[64];
    switch (a->kind) {
    case AUTO_ACT_OUTPUT: ui_auto_output_detail(a->output, out, len); return true;
    case AUTO_ACT_REMIND:
        /* TR: under "Remind to arm" on an automation's page (BL-224). At most 95 bytes */
        copy(out, len, a->option != 0u ? TR("A chime and a note on Home \xC2\xB7 also a notification")
                                       : TR("A chime and a note on Home"));
        return true;
    case AUTO_ACT_CHECK_ARMED:
    case AUTO_ACT_NOTIFY: {
        const uint8_t mask = ui_auto_notify_services();
        if (mask == 0u) {
            *alert = true;
            copy(out, len, TR("Can't send: no phone notifications are set up"));
            return true;
        }
        ui_auto_services_text(mask, services, sizeof(services));
        if (a->kind == AUTO_ACT_NOTIFY && ui_auto_notice_title(&r->when, AUTO_NOTICE_EVENT, 0u, quote, sizeof(quote))) {
            if (services[0] != '\0') {
                /* TR: under "Send a notification" on an automation's page (BL-224): what it says
                 * and where it goes ({0} its words, "Front Door opened"; {1} the services, "ntfy,
                 * Pushover"). At most 127 bytes */
                I18N_FORMAT(out, len, TR("\"{0}\" \xC2\xB7 {1}"), quote, services);
            } else {
                /* TR: the same without the services' names ({0} its words). At most 95 bytes */
                I18N_FORMAT(out, len, TR("\"{0}\""), quote);
            }
        } else {
            copy(out, len, services);
        }
        return true;
    }
    case AUTO_ACT_CHIME: return true;
    default: return false;
    }
}

static void build_rule(lv_obj_t *body, uint8_t slot)
{
    A.slot = slot;
    const auto_rule_t *r = rule_in_slot(slot);
    if (r == NULL) {
        lv_async_call(again_soon, NULL); /* gone: the tick takes the page back to the list */
        return;
    }
    read_devices();
    A.slot = slot;
    A.uid = r->uid;
    auto_status_t st;
    rule_state_t rs;
    status_of(r, &st);
    rule_state(r, &st, &rs);
    A.shape = rule_shape(2166136261u, r, &rs);
    bool alert = false, usable = true;
    use_text(r, &st, &rs, A.use_text, sizeof(A.use_text), &alert, &usable);
    /* TR: an automation's page, the switch at the top */
    A.use_row = ui_row_toggle_icon(body, UI_ICON_ZAP, ui_tone_blue(), TR("Use this automation"), A.use_text,
                                   usable && !r->off, on_use, NULL);
    if (alert) {
        ui_row_set_detail(A.use_row, A.use_text, true, ui_tone_amber());
    }
    if (!usable) {
        ui_row_disable(A.use_row);
    }

    /* TR: an automation's page, the heading over its trigger */
    ui_section(body, TR_C("automation section", "When"));
    char title[96], detail[DETAIL_LEN], value[48];
    const bool known_when = r->when.kind > AUTO_TRIG_NONE && r->when.kind < AUTO_TRIG_COUNT;
    known_when ? ui_auto_when_text(&r->when, title, sizeof(title)) : copy(title, sizeof(title), CANT_SHOW());
    if (!usable) {
        lv_obj_t *row = ui_row_link(body, ui_auto_when_glyph(&r->when), ui_tone_gray(), title, NULL, NULL, NULL,
                                    NULL, NULL);
        ui_row_disable(row);
        /* TR: an automation's page, the heading over its actions */
        ui_section(body, TR_C("automation section", "Then"));
        row = ui_row_link(body, UI_ICON_ZAP, ui_tone_gray(), CANT_SHOW(), NULL, NULL, NULL, NULL, NULL);
        ui_row_disable(row);
    } else {
        when_detail(&r->when, detail, sizeof(detail));
        ui_row_link(body, ui_auto_when_glyph(&r->when), ui_auto_when_tone(&r->when), title, detail, NULL,
                    UI_ICON_CHEVRON_RIGHT, on_when, NULL);
        if (r->when.kind != AUTO_TRIG_TIME) {
            ui_auto_window_text(&r->when, value, sizeof(value));
            /* TR: an automation's page: the hours it may run in */
            ui_row_link(body, UI_ICON_CLOCK, ui_tone_blue(), TR("Only between"), NULL, value, UI_ICON_CHEVRON_RIGHT,
                        on_window, NULL);
        }
        ui_section(body, TR_C("automation section", "Then"));
        /* Try it now: only what may run outside its trigger (AUT-006; never an arm, a disarm, a
         * reminder or "still armed": AUTO_TRY_ARM) */
        bool try_it = true;
        for (uint8_t i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
            const auto_action_t *a = &r->then[i];
            then_text(a, false, title, sizeof(title));
            const ui_dev_t *d = a->kind == AUTO_ACT_DEVICE ? device_of(a) : NULL;
            const bool missing = (rs.missing & (1u << i)) != 0u;
            const bool gray = missing || r->off;
            const char *under = d != NULL ? d->area : NULL;
            if (a->kind == AUTO_ACT_ARM || a->kind == AUTO_ACT_DISARM) {
                alarm_detail(r, a, detail, sizeof(detail));
                under = detail[0] != '\0' ? detail : NULL;
            }
            bool under_alert = false;
            if (a->kind != AUTO_ACT_ARM && a->kind != AUTO_ACT_DISARM &&
                panel_detail(r, a, detail, sizeof(detail), &under_alert)) {
                under = detail[0] != '\0' ? detail : NULL; /* BL-224's actions */
            }
            try_it = try_it && (a->kind == AUTO_ACT_DEVICE || a->kind == AUTO_ACT_OUTPUT ||
                                a->kind == AUTO_ACT_CHIME || a->kind == AUTO_ACT_NOTIFY);
            /* A Disarm with one partition has nothing to change: no chevron, no tap (the Arm's
             * row opens its sheet) */
            const bool opens = a->kind != AUTO_ACT_DISARM || enabled_partitions() >= 2;
            lv_obj_t *row = ui_row_link(body, action_glyph(a), gray ? ui_tone_gray() : action_tone(a), title, under, NULL,
                                        opens ? UI_ICON_CHEVRON_RIGHT : NULL, opens ? on_action : NULL,
                                        (void *)(intptr_t)i);
            if (under_alert && under != NULL) {
                ui_row_set_detail(row, under, true, ui_tone_amber());
            }
            if (!missing && (!A.less_secure || st.result == AUTO_RES_LESS_SECURE_OFF) &&
                auto_action_less_secure(a)) {
                /* TR: an automation's action the installer option stops (AUT-015): only the
                 * installer can allow it again. At most 79 bytes */
                ui_row_set_detail(row, TR("Ask your installer to allow less secure automations"), true,
                                  ui_tone_amber());
            }
            if (missing) {
                /* TR: an automation's action whose device left the panel; {0} the name it had.
                 * At most 79 bytes */
                I18N_FORMAT(detail, sizeof(detail), TR("Missing: it was called {0} \xC2\xB7 pick another device"),
                            a->name[0] != '\0' ? a->name : a->ref);
                ui_row_set_detail(row, detail, true, ui_tone_amber());
            }
            if ((a->kind == AUTO_ACT_DEVICE || a->kind == AUTO_ACT_OUTPUT) && a->dev_action == ACT_ON) {
                ui_auto_off_after_text(a->off_after_min, value, sizeof(value));
                /* TR: an automation's page: when a device it turned on is turned off again */
                ui_row_link(body, UI_ICON_TIMER, ui_tone_blue(), TR("Turn it off after"), NULL, value,
                            UI_ICON_CHEVRON_RIGHT, on_off_after, (void *)(intptr_t)i);
            }
        }
        /* A second action after a device action, a Disarm (D23), an output, the chime or a
         * notification (BL-224); never after an Arm, a reminder or "still armed", each the
         * automation's only action (AUT-011) */
        const bool second = r->n == 1u && ui_auto_may_add_second(&r->then[0]);
        if (second) {
            /* TR: an automation's page, the row that adds its second action */
            ui_row_action(body, UI_ICON_PLUS, TR("Add a second action"), ui_tone_blue(), on_add_action, NULL);
        }
        if (try_it) {
            /* TR: an automation's page: runs its actions now (AUT-006) */
            ui_row_action(body, UI_ICON_PLAY, TR("Try it now"), ui_tone_blue(), on_try, NULL);
        }
        if (disarm_alone(r)) {
            /* TR: an automation's page, under a time-of-day Disarm when no automation arms that
             * partition (DESIGN.md "Disarming" 5): a nudge, not a rule. At most 127 bytes */
            ui_paragraph(body, TR("Nothing arms it again. Add one that arms in the evening."), ui_tone_gray());
        }
        if (chime_off_alone(r)) {
            /* TR: an automation's page, under a time-of-day "Turn the door chime off" when no
             * automation turns it on again (BL-224, drawing bl-224-chime): a nudge. At most 127
             * bytes */
            ui_paragraph(body, TR("Nothing turns it on again. Add one for the morning."), ui_tone_gray());
        }
    }
    ui_section(body, "");
    /* TR: an automation's page, the last row */
    ui_row_action(body, UI_ICON_TRASH, TR("Delete this automation"), ui_tone_red(), on_delete, NULL);
}

/* The page's switch detail, once a second: rewritten only when its words changed (UI-023). */
static void tick_rule(void)
{
    const auto_rule_t *r = rule_in_slot(A.slot);
    if (r == NULL || r->uid != A.uid) {
        ui_page_back(); /* deleted elsewhere: back to the list */
        return;
    }
    auto_status_t st;
    rule_state_t rs;
    status_of(r, &st);
    rule_state(r, &st, &rs);
    if (rule_shape(2166136261u, r, &rs) != A.shape) {
        ui_page_rebuild();
        return;
    }
    char text[DETAIL_LEN];
    bool alert = false, usable = true;
    use_text(r, &st, &rs, text, sizeof(text), &alert, &usable);
    if (strcmp(text, A.use_text) != 0 && A.use_row != NULL) {
        copy(A.use_text, sizeof(A.use_text), text);
        ui_row_set_detail(A.use_row, A.use_text, alert, ui_tone_amber());
    }
}

/* ---- the section ---------------------------------------------------------------------------- */

static bool ours_on_screen(void)
{
    if (!A.built || S.depth == 0u || lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return false;
    }
    const ui_page_t *pg = &S.pages[S.depth - 1u];
    return pg->id == A.page && (pg->id == PAGE_AUTOMATIONS || (uint8_t)pg->arg == A.slot);
}

static bool on_page_of(uint8_t slot)
{
    return ours_on_screen() && A.page == PAGE_AUTOMATION && A.slot == slot;
}

/* After an edit or a refusal: the page drawn again from what is stored now, outside the
 * handler that asked (its row is about to go). */
static void again_soon(void *unused)
{
    (void)unused;
    if (!ours_on_screen()) {
        return;
    }
    if (A.page == PAGE_AUTOMATION && (!read_rules() || rule_in_slot(A.slot) == NULL)) {
        ui_page_back();
        return;
    }
    ui_page_rebuild();
}

void ui_automations_init(void)
{
    memset(&A, 0, sizeof(A));
    /* The Settings line stays hidden until BL-131 finding 7 moves Event log out of the column
     * (DESIGN.md R4): that change sets this to ui_automations_available(). */
    ui_set_int(&subject_automations_avail, 0);
}

uint16_t ui_auto_page_gen(void)
{
    return A.gen;
}

bool ui_automations_available(void)
{
    return S.be.auto_list != NULL;
}

void ui_automations_language_changed(void)
{
    A.use_text[0] = '\0'; /* ui_settings_language_changed draws the page again in the new words */
}

void ui_automations_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen,
                          char *note, size_t nlen)
{
    const bool list = id == PAGE_AUTOMATIONS;
    A.built = false;
    A.use_row = NULL;
    A.nrows = 0;
    if (list) {
        /* TR: Settings, the page title. At most 47 bytes */
        copy(title, tlen, TR("Automations"));
    } else {
        /* TR: one automation's page, the title. At most 47 bytes */
        copy(title, tlen, TR("Automation"));
    }
    if (!ui_settings_allowed(ACCESS_SYSTEM)) {
        /* AUT-010, D3: the system master or the installer (screen 6) */
        /* TR: a locked page: its title (cut to 31 bytes), then the explanation. At most 127 bytes */
        ui_settings_locked(body, ACCESS_SYSTEM, TR("Automations"),
                           TR("Automations are changed with the master or installer code."), note, nlen);
        return;
    }
    if (!read_rules()) {
        return;
    }
    A.gen = store_gen(); /* the generation an edit sends back (AUT-008) */
    /* AUT-015: asked of the alarm task on the panel, so only when a page is built */
    A.less_secure = S.be.auto_less_secure == NULL || S.be.auto_less_secure(S.be.ctx);
    A.page = id;
    if (list) {
        build_list(body, note, nlen);
    } else {
        build_rule(body, (uint8_t)arg);
    }
    A.built = true;
    A.ticked = lv_tick_get();
}

void ui_automations_tick(void)
{
    if (!ours_on_screen() || lv_tick_elaps(A.ticked) < 1000u) {
        return;
    }
    A.ticked = lv_tick_get();
    /* A sheet or a code pad over the page: its shape waits until it closes. */
    const bool covered = lv_subject_get_int(&subject_sheet) != SHEET_NONE ||
                         lv_subject_get_int(&subject_settings_pad) != 0;
    if (store_gen() != A.gen) {
        if (!covered) {
            again_soon(NULL); /* saved elsewhere (another keypad, the Panel Manager) */
        }
        return;
    }
    if (!read_rules()) {
        return;
    }
    read_devices();
    if (covered) {
        return;
    }
    if (A.page == PAGE_AUTOMATIONS) {
        tick_list();
    } else {
        tick_rule();
    }
}

#endif /* ALARM_UI_AUTOMATIONS */
