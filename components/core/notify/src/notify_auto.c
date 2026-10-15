/*
 * notify_auto.c - the automation hook and an automation's trigger clause (docs/features/41 D30).
 *
 * The clause words are the same as the touchscreen's (components/ui/alarm_ui/src/
 * ui_automations.c ui_auto_when_text): the same message ids and contexts, so a catalog
 * translates them once. They live here too because the firmware's hook runs outside the UI
 * thread and must not read the UI's state.
 */
#include "notify/notify_auto.h"

#include <string.h>

#include "i18n/i18n.h"
#include "notify/notify_words.h"

/* The one registered port (see the header): written once at start, read afterwards. */
static notify_hooks_t s_hooks;

void notify_set_hooks(const notify_hooks_t *hooks)
{
    if (hooks == NULL) {
        memset(&s_hooks, 0, sizeof(s_hooks));
    } else {
        s_hooks = *hooks;
    }
}

notify_auto_found_t notify_auto_lookup(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                                       uint16_t uid, notify_auto_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_hooks.automation == NULL || uid == 0u) {
        return NOTIFY_AUTO_UNKNOWN;
    }
    if (!s_hooks.automation(s_hooks.ctx, cat, cfg, uid, out)) {
        memset(out, 0, sizeof(*out));
        return NOTIFY_AUTO_GONE;
    }
    out->clause[sizeof(out->clause) - 1u] = '\0';
    out->sentence[sizeof(out->sentence) - 1u] = '\0';
    return NOTIFY_AUTO_FOUND;
}

bool notify_time_of_day(const i18n_catalog_t *cat, int64_t epoch_ms, char *out, size_t len)
{
    if (len == 0u) {
        return false;
    }
    out[0] = '\0';
    if (s_hooks.time_of_day == NULL || epoch_ms <= 0) {
        return false;
    }
    s_hooks.time_of_day(s_hooks.ctx, cat, epoch_ms, out, len);
    out[len - 1u] = '\0';
    return out[0] != '\0';
}

static void copy(char *out, size_t len, const char *text)
{
    (void)i18n_utf8_copy(out, len, text);
}

void notify_clock_text(const i18n_catalog_t *cat, bool clock24, unsigned hour, unsigned minute,
                       char *out, size_t len)
{
    i18n_civil_t c;
    memset(&c, 0, sizeof(c));
    c.year = 2026; /* any day: only the hour and minute show */
    c.month = 1;
    c.day = 1;
    c.hour = (uint8_t)(hour % 24u);
    c.minute = (uint8_t)(minute % 60u);
    /* The touchscreen's patterns (ui_app.c clock_tm), translated there. */
    const char *pattern = clock24 ? i18n_text(cat, "date pattern", "HH:mm")
                                  : i18n_text(cat, "date pattern", "h:mm a");
    (void)i18n_date(out, len, cat, pattern, &c);
}

static int enabled_partitions(const alarm_config_t *cfg)
{
    int n = 0;
    for (unsigned p = 0; cfg != NULL && p < ALARM_MAX_PARTITIONS; p++) {
        n += cfg->partitions[p].enabled ? 1 : 0;
    }
    return n;
}

/* The one partition a mask names, with more than one enabled: its name; NULL otherwise. */
static const char *partition_named(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                                   uint8_t mask, char *buf, size_t len)
{
    if (enabled_partitions(cfg) < 2 || mask == 0u || (mask & (uint8_t)(mask - 1u)) != 0u) {
        return NULL;
    }
    unsigned p = 1;
    while (p < ALARM_MAX_PARTITIONS && (mask & (1u << (p - 1u))) == 0u) {
        p++;
    }
    const char *name = cfg->partitions[p - 1u].name;
    if (name[0] != '\0') {
        copy(buf, len, name);
    } else {
        /* TR: a partition without a name, in an automation's sentence. At most 31 bytes */
        (void)I18N_FORMAT(buf, len, i18n_text(cat, "default partition name", "Partition {0}"), p);
    }
    return buf;
}

static void armed_text(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                       const auto_trigger_t *w, char *out, size_t len)
{
    char part[ALARM_NAME_LEN + 16];
    const char *name = partition_named(cat, cfg, w->partitions, part, sizeof(part));
    const char *one;
    const char *all;
    /* TR: the trigger of an automation, the list's row and its page (When). {0} is a
     * partition's name. At most 63 bytes */
    if (w->levels == ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY)) {
        one = i18n_text(cat, NULL, "When {0} is armed Away");
        all = i18n_text(cat, NULL, "When the panel is armed Away");
    } else if (w->levels == ALARM_LEVEL_BIT(ALARM_LEVEL_STAY)) {
        one = i18n_text(cat, NULL, "When {0} is armed Stay");
        all = i18n_text(cat, NULL, "When the panel is armed Stay");
    } else if (w->levels == ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT)) {
        one = i18n_text(cat, NULL, "When {0} is armed Night");
        all = i18n_text(cat, NULL, "When the panel is armed Night");
    } else {
        one = i18n_text(cat, NULL, "When {0} is armed");
        all = i18n_text(cat, NULL, "When the panel is armed");
    }
    if (name != NULL) {
        (void)I18N_FORMAT(out, len, one, name);
    } else {
        copy(out, len, all);
    }
}

/* The chosen days in the region's week order, by their short names: "Mon, Wed, Fri". */
static void days_text(const i18n_catalog_t *cat, const i18n_region_t *region, uint8_t days,
                      char *out, size_t len)
{
    out[0] = '\0';
    const unsigned first = (region != NULL) ? region->first_day : 0u;
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
        (void)i18n_date(name, sizeof(name), cat, "EEE", &c);
        if (used > 0u) {
            /* TR: between the short day names an automation runs on ("Mon, Wed, Fri"). At most 7 bytes */
            copy(out + used, len - used, i18n_text(cat, "list separator", ", "));
            used = strlen(out);
        }
        copy(out + used, len - used, name);
        used = strlen(out);
        if (used + 1u >= len) {
            break;
        }
    }
}

void notify_auto_when(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                      const i18n_region_t *region, bool clock24, const auto_trigger_t *w,
                      char *out, size_t len)
{
    if (out == NULL || len == 0u) {
        return;
    }
    out[0] = '\0';
    if (w == NULL) {
        return;
    }
    /* TR: an automation whose trigger or action this software can't read (D21), as its
     * sentence. At most 95 bytes */
    const char *cant = i18n_text(cat, NULL, "Something this software can't show");
    char name[ALARM_NAME_LEN + 16];
    switch (w->kind) {
    case AUTO_TRIG_ARMED: armed_text(cat, cfg, w, out, len); break;
    case AUTO_TRIG_DISARMED: {
        const char *part = partition_named(cat, cfg, w->partitions, name, sizeof(name));
        /* TR: the trigger of an automation. {0} is a partition's name. At most 63 bytes */
        if (part != NULL) {
            (void)I18N_FORMAT(out, len, i18n_text(cat, NULL, "When {0} is disarmed"), part);
        } else {
            copy(out, len, i18n_text(cat, NULL, "When the panel is disarmed"));
        }
        break;
    }
    /* TR: the trigger of an automation. At most 63 bytes */
    case AUTO_TRIG_ALARM: copy(out, len, i18n_text(cat, NULL, "When an alarm sounds")); break;
    case AUTO_TRIG_FIRE:
        copy(out, len, i18n_text(cat, NULL, "When a fire or CO alarm sounds"));
        break;
    case AUTO_TRIG_ZONE_OPENED:
    case AUTO_TRIG_ZONE_CLOSED: {
        if (cfg == NULL || w->zone == 0u || w->zone > ALARM_MAX_ZONES) {
            copy(out, len, cant);
            break;
        }
        (void)notify_zone_name(cat, cfg, w->zone, name, sizeof(name));
        const uint8_t cls = cfg->zones[w->zone - 1u].device_class;
        const char *f;
        /* TR: the trigger of an automation; {0} is a zone's name. At most 63 bytes */
        if (w->kind == AUTO_TRIG_ZONE_CLOSED) {
            f = i18n_text(cat, NULL, "When {0} closes");
        } else if (cls == ALARM_DEVCLASS_DOOR || cls == ALARM_DEVCLASS_WINDOW ||
                   cls == ALARM_DEVCLASS_GARAGE_DOOR) {
            f = i18n_text(cat, NULL, "When {0} opens");
        } else if (cls == ALARM_DEVCLASS_MOTION) {
            f = i18n_text(cat, NULL, "When {0} sees motion");
        } else if (cls == ALARM_DEVCLASS_WATER) {
            f = i18n_text(cat, NULL, "When {0} detects water");
        } else {
            f = i18n_text(cat, NULL, "When {0} is triggered");
        }
        (void)I18N_FORMAT(out, len, f, name);
        break;
    }
    case AUTO_TRIG_TIME: {
        char at[24];
        const unsigned m = (unsigned)(w->minute % AUTO_MINUTES_PER_DAY);
        notify_clock_text(cat, clock24, m / 60u, m % 60u, at, sizeof(at));
        const uint8_t days = (uint8_t)(w->days & AUTO_DAYS_ALL);
        const uint8_t weekdays = 0x1Fu, weekend = 0x60u;
        /* TR: the trigger of an automation; {0} is a time of day ("11:00 PM"). At most 47
         * bytes */
        if (days == 0u || days == AUTO_DAYS_ALL) {
            (void)I18N_FORMAT(out, len, i18n_text(cat, NULL, "At {0} every day"), at);
        } else if (days == weekdays) {
            (void)I18N_FORMAT(out, len, i18n_text(cat, NULL, "At {0} on weekdays"), at);
        } else if (days == weekend) {
            (void)I18N_FORMAT(out, len, i18n_text(cat, NULL, "At {0} on weekends"), at);
        } else {
            char list[96];
            days_text(cat, region, days, list, sizeof(list));
            /* TR: the trigger of an automation; {0} is a time of day, {1} the short names of
             * its days ("Mon, Wed, Fri"). At most 47 bytes */
            (void)I18N_FORMAT(out, len, i18n_text(cat, NULL, "At {0} on {1}"), at, list);
        }
        break;
    }
    default: copy(out, len, cant); break;
    }
}
