/*
 * notify_words.c - what the panel says about an event, in words (English, v1).
 *
 * Moved here from components/ui/alarm_ui/src/ui_text.c so that the Activity list on the panel and a
 * push notification on a phone read the same sentences. The wording is unchanged from the
 * version that shipped on the panel: the UI preview renders the same pixels before and after.
 *
 * Every word goes through the catalog (docs/features/37): cat NULL is English, the text as
 * written. Whole sentences only, {0}.. for values, a context where a short word is ambiguous. The
 * household's own names (zones, users) are values and are never translated. The text lands in
 * title[NOTIFY_TITLE_LEN = 64] or body[NOTIFY_BODY_LEN = 96]; a household name is at most 23 bytes.
 */
#include "notify/notify_words.h"

#include <stdio.h>
#include <string.h>

#include "i18n/i18n.h"
#include "notify/notify_auto.h"

/* A copy that cuts on a whole UTF-8 character (a translation need not be ASCII). */
static void put(char *dst, size_t len, const char *src)
{
    (void)i18n_utf8_copy(dst, len, src);
}

/* "a · b", or just a when b is NULL: two independent facts on one line, joined the same way in
 * every language. */
static void join2(char *dst, size_t len, const char *a, const char *b)
{
    if (b == NULL) {
        put(dst, len, a);
    } else {
        (void)I18N_FORMAT(dst, len, "{0} \xC2\xB7 {1}", a, b);
    }
}

notify_accent_t notify_accent_for_level(alarm_arm_level_t level)
{
    switch (level) {
    case ALARM_LEVEL_AWAY: return NOTIFY_ACCENT_ROSE;
    case ALARM_LEVEL_STAY: return NOTIFY_ACCENT_BLUE;
    case ALARM_LEVEL_NIGHT: return NOTIFY_ACCENT_VIOLET;
    default: return NOTIFY_ACCENT_GREEN;
    }
}

const char *notify_zone_name(const i18n_catalog_t *cat, const alarm_config_t *cfg, uint16_t zone,
                             char *buf, size_t len)
{
    if (cfg != NULL && zone >= 1u && zone <= ALARM_MAX_ZONES &&
        cfg->zones[zone - 1u].name[0] != '\0') {
        put(buf, len, cfg->zones[zone - 1u].name);
    } else {
        /* TR: The name of a zone nobody has named; {0} is its number. */
        (void)I18N_FORMAT(buf, len, i18n_text(cat, "default zone name", "Zone {0}"),
                          (unsigned)zone);
    }
    return buf;
}

const char *notify_user_name(const i18n_catalog_t *cat, const alarm_config_t *cfg, uint8_t user,
                             char *buf, size_t len)
{
    if (user == 0u) {
        buf[0] = '\0';
    } else if (cfg != NULL && user <= ALARM_MAX_USERS && cfg->users[user - 1u].name[0] != '\0') {
        put(buf, len, cfg->users[user - 1u].name);
    } else {
        char num[8];
        snprintf(num, sizeof(num), "%02u", (unsigned)user);
        /* TR: The name of a user nobody has named; {0} is a two-digit number ("05"). */
        (void)I18N_FORMAT(buf, len, i18n_text(cat, "default user name", "User {0}"), num);
    }
    return buf;
}

const char *notify_cause_title(const i18n_catalog_t *cat, alarm_cause_t cause)
{
    /* TR: The title of an alarm, by what set it off. At most 63 bytes. */
    switch (cause) {
    case ALARM_CAUSE_BURGLARY:
    case ALARM_CAUSE_SILENT_BURGLARY: return i18n_text(cat, NULL, "Burglary alarm");
    case ALARM_CAUSE_EXIT_ERROR: return i18n_text(cat, NULL, "Exit alarm");
    case ALARM_CAUSE_TAMPER: return i18n_text(cat, NULL, "Tamper alarm");
    case ALARM_CAUSE_24H_SILENT:
    case ALARM_CAUSE_24H_AUDIBLE: return i18n_text(cat, "alarm cause", "Alarm");
    case ALARM_CAUSE_24H_AUX: return i18n_text(cat, NULL, "Emergency");
    case ALARM_CAUSE_FIRE: return i18n_text(cat, NULL, "Fire alarm");
    case ALARM_CAUSE_CO: return i18n_text(cat, "alarm cause", "Carbon monoxide");
    case ALARM_CAUSE_PANIC_POLICE:
    case ALARM_CAUSE_PANIC_SILENT: return i18n_text(cat, NULL, "Panic alarm");
    case ALARM_CAUSE_PANIC_FIRE: return i18n_text(cat, NULL, "Fire panic");
    case ALARM_CAUSE_PANIC_MEDICAL: return i18n_text(cat, NULL, "Medical emergency");
    default: return i18n_text(cat, "alarm cause", "Alarm");
    }
}

const char *notify_result_text(const i18n_catalog_t *cat, alarm_result_t r)
{
    /* TR: What the panel says when a command was not carried out (a toast on the keypad). */
    switch (r) {
    case ALARM_OK: return i18n_text(cat, "command result", "Done");
    case ALARM_ERR_INVALID_CODE: return i18n_text(cat, NULL, "Wrong code, try again");
    case ALARM_ERR_CODE_REQUIRED: return i18n_text(cat, NULL, "Enter your code");
    case ALARM_ERR_LOCKED_OUT: return i18n_text(cat, NULL, "Keypad locked. Try again later");
    case ALARM_ERR_NOT_AUTHORIZED: return i18n_text(cat, NULL, "This code can't do that");
    case ALARM_ERR_NOT_READY: return i18n_text(cat, NULL, "Not ready: a zone is open");
    case ALARM_ERR_NOT_BYPASSABLE: return i18n_text(cat, NULL, "That zone can't be bypassed");
    case ALARM_ERR_BAD_STATE: return i18n_text(cat, NULL, "Not possible right now");
    case ALARM_ERR_CODE_IN_USE: return i18n_text(cat, NULL, "Someone already uses that code");
    case ALARM_ERR_DISABLED: return i18n_text(cat, NULL, "Not enabled");
    case ALARM_ERR_UNAVAILABLE:
        return i18n_text(cat, NULL, "Codes can't be checked right now. Try again");
    case ALARM_ERR_HELD: return i18n_text(cat, NULL, "Wait: a sensor radio is updating");
    case ALARM_ERR_BAD_ARG:
    default: return i18n_text(cat, NULL, "Not possible");
    }
}

const char *notify_authority_name(const i18n_catalog_t *cat, alarm_authority_t a)
{
    /* TR: What a user's code may do (its level). At most 23 bytes. */
    switch (a) {
    case ALARM_AUTH_INSTALLER: return i18n_text(cat, "user level", "Installer");
    case ALARM_AUTH_MASTER: return i18n_text(cat, "user level", "System master");
    case ALARM_AUTH_PARTITION_MASTER: return i18n_text(cat, "user level", "Partition master");
    case ALARM_AUTH_STANDARD: return i18n_text(cat, "user level", "Standard");
    case ALARM_AUTH_ARM_ONLY: return i18n_text(cat, "user level", "Arm only");
    case ALARM_AUTH_GUEST: return i18n_text(cat, "user level", "Guest");
    case ALARM_AUTH_DURESS: return i18n_text(cat, "user level", "Duress");
    default: return i18n_text(cat, "user level", "Empty");
    }
}

static notify_icon_t cause_glyph(alarm_cause_t cause)
{
    switch (cause) {
    case ALARM_CAUSE_FIRE:
    case ALARM_CAUSE_PANIC_FIRE: return NOTIFY_ICON_FLAME;
    case ALARM_CAUSE_CO: return NOTIFY_ICON_CLOUD_FOG;
    case ALARM_CAUSE_24H_AUX:
    case ALARM_CAUSE_PANIC_MEDICAL: return NOTIFY_ICON_HEART_PULSE;
    default: return NOTIFY_ICON_SIREN;
    }
}

static const char *level_words(const i18n_catalog_t *cat, uint8_t level, uint8_t flags)
{
    const bool ned = (flags & ALARM_ARM_NO_ENTRY_DELAY) != 0u;
    /* TR: The title of an arming entry in the event list. At most 63 bytes. */
    switch ((alarm_arm_level_t)level) {
    case ALARM_LEVEL_AWAY:
        return ned ? i18n_text(cat, NULL, "Armed Away · No entry delay") : i18n_text(cat, NULL, "Armed Away");
    case ALARM_LEVEL_STAY:
        return ned ? i18n_text(cat, NULL, "Armed Stay · No entry delay") : i18n_text(cat, NULL, "Armed Stay");
    case ALARM_LEVEL_NIGHT:
        return ned ? i18n_text(cat, NULL, "Armed Night · No entry delay")
                   : i18n_text(cat, NULL, "Armed Night");
    default: return i18n_text(cat, "arming level", "Armed");
    }
}

/* How a Z-Wave device joined, in the add pages' words (docs/features/36 network.md §4.1). */
static const char *zwave_join_words(const i18n_catalog_t *cat, uint8_t join)
{
    switch ((alarm_zw_join_t)join) {
    case ALARM_ZW_JOIN_S2_UNAUTH: return i18n_text(cat, NULL, "Secure (no PIN)");
    case ALARM_ZW_JOIN_S2_AUTH:
    case ALARM_ZW_JOIN_S2_ACCESS: return i18n_text(cat, "Z-Wave join", "Secure");
    case ALARM_ZW_JOIN_S0: return i18n_text(cat, NULL, "Older security");
    case ALARM_ZW_JOIN_FAILED:
        return i18n_text(cat, NULL, "The secure connection failed: remove it and add it again");
    case ALARM_ZW_JOIN_DOWNGRADED:
        return i18n_text(cat, NULL, "Security was downgraded: remove it and add it again");
    default: return i18n_text(cat, NULL, "Not encrypted");
    }
}

/* The Z-Wave device an event names: its zone ("Front Door"), else "Device 7" (its node ID, as the
 * device page shows it), else "A device" (a Long Range node ID that does not fit). */
static void zwave_device_words(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                               const alarm_event_t *ev, char *buf, size_t len)
{
    if (ev->zone != 0u) {
        notify_zone_name(cat, cfg, ev->zone, buf, len);
    } else if (ev->arg != 0u) {
        /* TR: A Z-Wave device with no zone; {0} is its node number. */
        (void)I18N_FORMAT(buf, len, i18n_text(cat, "Z-Wave device name", "Device {0}"),
                          (unsigned)ev->arg);
    } else {
        put(buf, len, i18n_text(cat, "Z-Wave device name", "A device"));
    }
}

/* How a Zigbee device joined, in its device page's words (docs/features/20 devices.md §11). */
static const char *zigbee_join_words(const i18n_catalog_t *cat, uint8_t join)
{
    switch ((alarm_zb_join_t)join) {
    case ALARM_ZB_JOIN_INSTALL_CODE: return i18n_text(cat, NULL, "Joined with its install code");
    case ALARM_ZB_JOIN_STANDARD_KEY: return i18n_text(cat, NULL, "Joined with the standard key");
    default: return i18n_text(cat, NULL, "How it joined isn't known");
    }
}

/* The Zigbee device an event names: its zone ("Front Door"), else "Device 12" (its number on the
 * panel's Zigbee list, as its device page shows it), else "A device" (a number over 255). */
static void zigbee_device_words(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                                const alarm_event_t *ev, char *buf, size_t len)
{
    if (ev->zone != 0u) {
        notify_zone_name(cat, cfg, ev->zone, buf, len);
    } else if (ev->arg != 0u) {
        /* TR: A Zigbee device with no zone; {0} is its number on the panel's list of devices. */
        (void)I18N_FORMAT(buf, len, i18n_text(cat, "Zigbee device name", "Device {0}"),
                          (unsigned)ev->arg);
    } else {
        put(buf, len, i18n_text(cat, "Zigbee device name", "A device"));
    }
}

/* An automation as the one who did it (docs/features/41 D30, AUT-009), never the person who
 * saved it: "By automation · At 10:00 AM on weekdays" (with_clause and the hook knows it), "By
 * automation · changed since" (edited after the event), "By a deleted automation", or "By
 * automation" alone (no hook, or with_clause false). */
static void auto_who(const i18n_catalog_t *cat, const alarm_config_t *cfg, const alarm_event_t *ev,
                     bool with_clause, char *buf, size_t len)
{
    notify_auto_info_t info;
    const notify_auto_found_t found = notify_auto_lookup(cat, cfg, ev->source.id, &info);
    /* TR: Who did it, in the line under an event title: one of the panel's automations
     * (rules the installer set up), not a person. At most 48 bytes. */
    const char *by = i18n_text(cat, NULL, "By automation");
    if (found == NOTIFY_AUTO_GONE) {
        /* TR: Who did it, in the line under an event title: an automation that has been deleted
         * since. At most 48 bytes. */
        put(buf, len, i18n_text(cat, NULL, "By a deleted automation"));
    } else if (found != NOTIFY_AUTO_FOUND || !with_clause) {
        put(buf, len, by);
    } else if (info.changed_at != 0u && ev->epoch_ms > 0 &&
               ev->epoch_ms / 1000 < (int64_t)info.changed_at) {
        /* TR: After "By automation" ("By automation · changed since"): the automation was
         * edited after this event, so its words now would not be what it did. At most 40
         * bytes. */
        join2(buf, len, by, i18n_text(cat, NULL, "changed since"));
    } else {
        join2(buf, len, by, info.clause[0] != '\0' ? info.clause : NULL);
    }
}

/* "By Master", "Remotely", "By Front Door" (a keyfob zone), "By Alex · Panel Manager"... */
static void by_whom(const i18n_catalog_t *cat, const alarm_config_t *cfg, const alarm_event_t *ev,
                    char *buf, size_t len)
{
    char name[ALARM_NAME_LEN + 8];
    /* The browser Panel Manager is a remote source that names itself, so a disarm from it reads
     * apart from one at a keypad (docs/features/31, BL-113). */
    const bool panel_manager = ev->source.kind == (uint8_t)ALARM_SRC_REMOTE &&
                               ev->source.id == (uint16_t)ALARM_SRC_ID_PANEL_MANAGER;
    /* TR: Where it was done from, the browser app's name: alone, or after "By {0}" in the line
     * under an event title ("By Alex · Panel Manager"). At most 48 bytes. */
    const char *pm = i18n_text(cat, NULL, "Panel Manager");
    /* An automation says so even when it acted as a user (docs/features/41 AUT-009): the
     * words name the automation, the JSON keeps the user's name. */
    if (ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION) {
        auto_who(cat, cfg, ev, true, buf, len);
        return;
    }
    if (ev->user != 0u) {
        char who[NOTIFY_BODY_LEN];
        /* TR: Who did it, in the line under an event title; {0} is a user's or a zone's name. */
        (void)I18N_FORMAT(who, sizeof(who), i18n_text(cat, NULL, "By {0}"),
                          notify_user_name(cat, cfg, ev->user, name, sizeof(name)));
        join2(buf, len, who, panel_manager ? pm : NULL);
        return;
    }
    if (panel_manager) {
        put(buf, len, pm);
        return;
    }
    switch ((alarm_source_kind_t)ev->source.kind) {
    case ALARM_SRC_KEYPAD: put(buf, len, i18n_text(cat, NULL, "From a keypad")); break;
    case ALARM_SRC_REMOTE: put(buf, len, i18n_text(cat, NULL, "Remotely")); break;
    case ALARM_SRC_ZONE:
        (void)I18N_FORMAT(buf, len, i18n_text(cat, NULL, "By {0}"),
                          notify_zone_name(cat, cfg, ev->source.id, name, sizeof(name)));
        break;
    case ALARM_SRC_SCHEDULE: put(buf, len, i18n_text(cat, NULL, "By schedule")); break;
    case ALARM_SRC_AUTOMATION: put(buf, len, i18n_text(cat, NULL, "By automation")); break;
    default: buf[0] = '\0'; break;
    }
}

void notify_sys_trouble_words(const i18n_catalog_t *cat, alarm_sys_trouble_t t, bool restored,
                              notify_words_t *out)
{
    /* TR: A system trouble: its title (the Troubles list, the Activity list and a phone
     * notification, where "restored" can follow it) and the line under it. The title is cut to
     * 48 bytes; the line under it shares that limit to stay short. At most 48 bytes. */
    static const struct {
        uint8_t icon;
        const char *title;
        const char *detail;
    } table[ALARM_TRBL_COUNT] = {
        [ALARM_TRBL_AC_LOSS] = {NOTIFY_ICON_ZAP, I18N_NOOP("Mains power lost"),
                                I18N_NOOP("Running on battery")},
        [ALARM_TRBL_LOW_BATTERY] = {NOTIFY_ICON_BATTERY_WARNING, I18N_NOOP("Panel battery low"),
                                    I18N_NOOP("Replace or recharge the backup battery")},
        [ALARM_TRBL_COMM_FAIL] = {NOTIFY_ICON_CLOUD_OFF, I18N_NOOP("Reports not getting through"),
                                  I18N_NOOP("A connected service can't be reached")},
        [ALARM_TRBL_NETWORK] = {NOTIFY_ICON_WIFI_OFF, I18N_NOOP("Network down"),
                                I18N_NOOP("Wi-Fi is disconnected")},
        [ALARM_TRBL_CLOCK_NOT_SET] = {NOTIFY_ICON_CLOCK, I18N_NOOP("Clock not set"),
                                      I18N_NOOP("Connect Wi-Fi so the clock can sync")},
        [ALARM_TRBL_COPROCESSOR] = {NOTIFY_ICON_CPU, I18N_NOOP("Wi-Fi module not responding"),
                                    I18N_NOOP("The panel keeps protecting without the network")},
        [ALARM_TRBL_SENSOR_RADIO] = {NOTIFY_ICON_RADIO_TOWER, I18N_NOOP("Sensor radio problem"),
                                     I18N_NOOP("Wireless sensors may not be heard")},
        [ALARM_TRBL_BELL] = {NOTIFY_ICON_BELL_OFF, I18N_NOOP("Siren problem"),
                             I18N_NOOP("Check the siren wiring")},
        [ALARM_TRBL_PANEL_TAMPER] = {NOTIFY_ICON_SHIELD_X, I18N_NOOP("Panel tamper"),
                                     I18N_NOOP("The panel was opened")},
        [ALARM_TRBL_STORAGE] = {NOTIFY_ICON_HARD_DRIVE, I18N_NOOP("Storage problem"),
                                I18N_NOOP("Settings may not be saved")},
    };
    if ((unsigned)t < (unsigned)ALARM_TRBL_COUNT && table[t].title != NULL) {
        out->icon = table[t].icon;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, table[t].title));
        put(out->body, sizeof(out->body),
            restored ? i18n_text(cat, NULL, "Restored \xC2\xB7 acknowledge to clear")
                     : i18n_text(cat, NULL, table[t].detail));
    } else {
        out->icon = NOTIFY_ICON_TRIANGLE_ALERT;
        /* TR: A trouble the panel has no words for; {0} is its number. */
        (void)I18N_FORMAT(out->title, sizeof(out->title),
                          i18n_text(cat, NULL, "System trouble {0}"), (unsigned)t);
        put(out->body, sizeof(out->body), restored ? i18n_text(cat, NULL, "Restored") : "");
    }
    out->accent = restored ? NOTIFY_ACCENT_GRAY : NOTIFY_ACCENT_AMBER;
    out->cat = NOTIFY_CAT_TROUBLE;
    out->silent = false;
}

void notify_zone_trouble_words(const i18n_catalog_t *cat, const alarm_config_t *cfg, uint16_t zone,
                               alarm_zone_trouble_t t, bool restored, notify_words_t *out)
{
    static const uint8_t glyphs[ALARM_ZTRBL_COUNT] = {
        [ALARM_ZTRBL_LOOP] = NOTIFY_ICON_TRIANGLE_ALERT,
        [ALARM_ZTRBL_TAMPER] = NOTIFY_ICON_SHIELD_X,
        [ALARM_ZTRBL_LOW_BATTERY] = NOTIFY_ICON_BATTERY_WARNING,
        [ALARM_ZTRBL_SUPERVISION] = NOTIFY_ICON_RADIO_TOWER,
        [ALARM_ZTRBL_DAY_FAULT] = NOTIFY_ICON_DOOR_OPEN,
    };
    static const char *const words[ALARM_ZTRBL_COUNT] = {
        [ALARM_ZTRBL_LOOP] = I18N_NOOP_C("zone trouble", "Sensor fault"),
        [ALARM_ZTRBL_TAMPER] = I18N_NOOP_C("zone trouble", "Tamper"),
        [ALARM_ZTRBL_LOW_BATTERY] = I18N_NOOP_C("zone trouble", "Low battery"),
        [ALARM_ZTRBL_SUPERVISION] = I18N_NOOP_C("zone trouble", "Not reporting"),
        [ALARM_ZTRBL_DAY_FAULT] = I18N_NOOP_C("zone trouble", "Open during the day"),
    };
    char zname[ALARM_NAME_LEN + 8];
    const unsigned i = ((unsigned)t < (unsigned)ALARM_ZTRBL_COUNT) ? (unsigned)t : 0u;
    out->icon = glyphs[i];
    join2(out->title, sizeof(out->title),
          notify_zone_name(cat, cfg, zone, zname, sizeof(zname)),
          i18n_text(cat, "zone trouble", words[i]));
    put(out->body, sizeof(out->body),
        restored ? i18n_text(cat, NULL, "Restored \xC2\xB7 acknowledge to clear")
                 : i18n_text(cat, NULL, "Check the sensor"));
    out->accent = restored ? NOTIFY_ACCENT_GRAY : NOTIFY_ACCENT_AMBER;
    out->cat = NOTIFY_CAT_TROUBLE;
    out->silent = false;
}

bool notify_words(const i18n_catalog_t *cat, const alarm_config_t *cfg, const alarm_event_t *ev,
                  bool show_silent, notify_words_t *out)
{
    if ((ev->flags & ALARM_EVF_SILENT) && !show_silent) {
        return false; /* duress, silent panic: never shown on a keypad */
    }
    char zname[ALARM_NAME_LEN + 8];
    out->icon = NOTIFY_ICON_INFO;
    out->accent = NOTIFY_ACCENT_GRAY;
    out->cat = (uint8_t)notify_category(ev->type);
    out->silent = (ev->flags & ALARM_EVF_SILENT) != 0u;
    out->title[0] = '\0';
    out->body[0] = '\0';
    if (ev->zone != 0u && ev->type != ALARM_EV_TROUBLE && ev->type != ALARM_EV_TROUBLE_RESTORE) {
        notify_zone_name(cat, cfg, ev->zone, zname, sizeof(zname));
    } else {
        zname[0] = '\0';
    }

    /* TR: An event in the Activity list (title, and the line under it), and in a phone
     * notification. A title is at most 63 bytes, the line under it 95. {0} is a household name
     * (cut to 23 bytes). */
    switch (ev->type) {
    case ALARM_EV_SYSTEM_START:
        out->icon = NOTIFY_ICON_SHIELD;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Panel started"));
        /* PWR-007: the line under says why, by the reason in arg (alarm_start_reason_t). */
        switch ((alarm_start_reason_t)ev->arg) {
        case ALARM_START_POWER_ON:
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "Protection resumed after power-up"));
            break;
        case ALARM_START_SOFTWARE:
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "Protection resumed after a restart"));
            break;
        case ALARM_START_PANIC:
        case ALARM_START_WATCHDOG:
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "Protection resumed after a fault restart"));
            break;
        case ALARM_START_BROWNOUT:
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "Protection resumed after a power dip"));
            break;
        case ALARM_START_EXTERNAL:
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "Protection resumed after a reset"));
            break;
        case ALARM_START_UNKNOWN:
        case ALARM_START_OTHER:
        default:
            put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Protection resumed"));
            break;
        }
        break;
    case ALARM_EV_ARMED:
        out->icon = NOTIFY_ICON_LOCK;
        out->accent = (uint8_t)notify_accent_for_level((alarm_arm_level_t)ev->arg);
        put(out->title, sizeof(out->title), level_words(cat, ev->arg, ev->arg2));
        if (ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION &&
            ALARM_ARMED_BYPASSED(ev->arg2) > 0u) {
            /* AUT-009, AUT-014, CORE-USR-032: "By automation · 1 zone bypassed" */
            char who[NOTIFY_BODY_LEN];
            char n[40];
            const unsigned nb = ALARM_ARMED_BYPASSED(ev->arg2);
            auto_who(cat, cfg, ev, false, who, sizeof(who));
            /* TR: After "By automation" under "Armed Away": how many open zones the automation
             * bypassed to arm ("1 zone bypassed"). At most 32 bytes. */
            (void)I18N_FORMAT(n, sizeof(n),
                              i18n_plural(cat, NULL, "{0} zone bypassed", "{0} zones bypassed", nb),
                              nb);
            join2(out->body, sizeof(out->body), who, n);
        } else {
            by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        }
        break;
    case ALARM_EV_ARM_FAILED:
        out->icon = NOTIFY_ICON_SHIELD_ALERT;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Arming failed"));
        if (ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION) {
            /* AUT-009, CORE-USR-030: nobody was at the panel - which zone, and who:
             * "Kitchen Window · By automation" */
            char who[NOTIFY_BODY_LEN];
            auto_who(cat, cfg, ev, false, who, sizeof(who));
            join2(out->body, sizeof(out->body),
                  zname[0] != '\0' ? zname : notify_result_text(cat, (alarm_result_t)ev->arg),
                  who);
        } else {
            put(out->body, sizeof(out->body), notify_result_text(cat, (alarm_result_t)ev->arg));
        }
        break;
    case ALARM_EV_DISARMED:
        out->icon = NOTIFY_ICON_LOCK_OPEN;
        out->accent = NOTIFY_ACCENT_GREEN;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Disarmed"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_AUTO_STAY:
        out->icon = NOTIFY_ICON_HOUSE;
        out->accent = NOTIFY_ACCENT_BLUE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Armed Stay instead of Away"));
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Nobody left through an exit door"));
        break;
    case ALARM_EV_EXIT_RESTART:
        out->icon = NOTIFY_ICON_CLOCK;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Exit time restarted"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_ENTRY_DELAY:
        out->icon = NOTIFY_ICON_DOOR_OPEN;
        out->accent = NOTIFY_ACCENT_ORANGE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Entry time started"));
        put(out->body, sizeof(out->body), zname);
        break;
    case ALARM_EV_ALARM:
        out->icon = (uint8_t)cause_glyph((alarm_cause_t)ev->arg);
        out->accent = NOTIFY_ACCENT_RED;
        put(out->title, sizeof(out->title), notify_cause_title(cat, (alarm_cause_t)ev->arg));
        if (zname[0] != '\0') {
            put(out->body, sizeof(out->body), zname);
        } else {
            by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        }
        break;
    case ALARM_EV_ALARM_RESTORE:
        out->icon = NOTIFY_ICON_CIRCLE_CHECK;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Back to normal after alarm"));
        put(out->body, sizeof(out->body), zname);
        break;
    case ALARM_EV_EXIT_ERROR:
        out->icon = NOTIFY_ICON_SIREN;
        out->accent = NOTIFY_ACCENT_RED;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Exit error"));
        (void)I18N_FORMAT(out->body, sizeof(out->body),
                          i18n_text(cat, NULL, "{0} left open after exit time"), zname);
        break;
    case ALARM_EV_RECENT_CLOSING:
        out->icon = NOTIFY_ICON_SIREN;
        out->accent = NOTIFY_ACCENT_RED;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Alarm right after arming"));
        put(out->body, sizeof(out->body),
            i18n_text(cat, NULL, "Within 2 minutes of the exit time"));
        break;
    case ALARM_EV_ALARM_ABORTED:
        out->icon = NOTIFY_ICON_CIRCLE_CHECK;
        out->accent = NOTIFY_ACCENT_GREEN;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Alarm aborted"));
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Disarmed before it was reported"));
        break;
    case ALARM_EV_ALARM_CANCELED:
        out->icon = NOTIFY_ICON_CIRCLE_CHECK;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Alarm canceled"));
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Disarmed after it was reported"));
        break;
    case ALARM_EV_ALARM_SILENCED:
        out->icon = NOTIFY_ICON_BELL_OFF;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Siren silenced"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_BELL_TIMEOUT:
        out->icon = NOTIFY_ICON_BELL_OFF;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Siren timed out"));
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "The alarm stays on the display"));
        break;
    case ALARM_EV_SWINGER_SHUTDOWN:
        out->icon = NOTIFY_ICON_CIRCLE_SLASH;
        out->accent = NOTIFY_ACCENT_AMBER;
        (void)I18N_FORMAT(out->title, sizeof(out->title), i18n_text(cat, NULL, "{0} shut down"),
                          zname);
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Too many alarms from this zone"));
        break;
    case ALARM_EV_MEMORY_CLEARED:
        out->icon = NOTIFY_ICON_CHECK;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Alarm memory cleared"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_BYPASS:
        out->icon = NOTIFY_ICON_CIRCLE_SLASH;
        (void)I18N_FORMAT(out->title, sizeof(out->title), i18n_text(cat, NULL, "{0} bypassed"),
                          zname);
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_UNBYPASS:
        out->icon = NOTIFY_ICON_CIRCLE_CHECK;
        if (ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION) {
            /* AUT-009, CORE-USR-033: an automation's bypass ends only by being watched again
             * (nothing else unbypasses as an automation). */
            char who[NOTIFY_BODY_LEN];
            /* TR: The title when a zone an automation bypassed to arm is watched again, now
             * that it has closed; {0} is the zone's name. At most 63 bytes. */
            (void)I18N_FORMAT(out->title, sizeof(out->title),
                              i18n_text(cat, NULL, "{0} watched again"), zname);
            auto_who(cat, cfg, ev, false, who, sizeof(who));
            /* TR: Why a zone is watched again, before "By automation" ("Closed after arming ·
             * By automation"). At most 40 bytes. */
            join2(out->body, sizeof(out->body), i18n_text(cat, NULL, "Closed after arming"), who);
            break;
        }
        (void)I18N_FORMAT(out->title, sizeof(out->title),
                          i18n_text(cat, NULL, "{0} no longer bypassed"), zname);
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_TROUBLE:
    case ALARM_EV_TROUBLE_RESTORE: {
        const bool restore = ev->type == ALARM_EV_TROUBLE_RESTORE;
        notify_words_t t;
        /* The restore has to reach the words, not just the title: "Mains power lost · restored"
         * with "Running on battery" under it says two opposite things, which is nonsense on a
         * phone's lock screen. Found on the panel, 2026-09-21. */
        if (ev->arg2 == ALARM_TRBL_FAMILY_SYSTEM) {
            notify_sys_trouble_words(cat, (alarm_sys_trouble_t)ev->arg, restore, &t);
        } else if (ev->arg2 == ALARM_TRBL_FAMILY_ZONE) {
            notify_zone_trouble_words(cat, cfg, ev->zone, (alarm_zone_trouble_t)ev->arg, restore,
                                      &t);
        } else {
            t.icon = NOTIFY_ICON_RADIO_TOWER;
            (void)I18N_FORMAT(t.title, sizeof(t.title),
                              i18n_text(cat, NULL, "Keypad {0} not responding"),
                              (unsigned)ev->zone);
            t.body[0] = '\0';
        }
        out->icon = restore ? NOTIFY_ICON_CIRCLE_CHECK : t.icon;
        out->accent = restore ? NOTIFY_ACCENT_GREEN : NOTIFY_ACCENT_AMBER;
        if (restore) {
            char head[49];
            (void)i18n_utf8_copy(head, sizeof(head), t.title); /* the title, cut to 48 bytes */
            /* TR: A trouble title with "restored" after it; {0} is the trouble's title.
             * The status word stands alone after the separator, so gendered languages should
             * choose a neutral form. At most 15 bytes. */
            (void)I18N_FORMAT(out->title, sizeof(out->title),
                              i18n_text(cat, NULL, "{0} \xC2\xB7 restored"), head);
        } else {
            (void)i18n_utf8_copy(out->title, 49, t.title); /* cut to 48 bytes, as when restored */
        }
        /* A restore in the log or on a phone is history: "acknowledge to clear" belongs to the
         * Troubles list, where it can still be done. */
        put(out->body, sizeof(out->body),
            restore ? i18n_text(cat, NULL, "Back to normal") : t.body);
        break;
    }
    case ALARM_EV_TROUBLES_ACKED:
        out->icon = NOTIFY_ICON_CHECK;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Troubles acknowledged"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_INVALID_CODE:
        out->icon = NOTIFY_ICON_KEY;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Wrong code entered"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_LOCKOUT:
        out->icon = NOTIFY_ICON_LOCK;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Keypad locked"));
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Too many wrong codes"));
        break;
    case ALARM_EV_CHIME_ON:
    case ALARM_EV_CHIME_OFF:
        out->icon = ev->type == ALARM_EV_CHIME_ON ? NOTIFY_ICON_BELL : NOTIFY_ICON_BELL_OFF;
        put(out->title, sizeof(out->title),
            ev->type == ALARM_EV_CHIME_ON ? i18n_text(cat, NULL, "Chime on")
                                          : i18n_text(cat, NULL, "Chime off"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_WALK_TEST_START:
        out->icon = NOTIFY_ICON_FOOTPRINTS;
        out->accent = NOTIFY_ACCENT_BLUE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Walk test started"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_WALK_TEST_END:
        /* WALK-010: what the test found, from the event's counts (arg tested, arg2 covered). */
        out->icon = NOTIFY_ICON_FOOTPRINTS;
        out->accent = NOTIFY_ACCENT_BLUE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Walk test ended"));
        if (ev->arg == ev->arg2 && ev->arg2 != 0u) {
            /* TR: The walk test's result when every zone it covers was tested; {0} is how many
             * zones that is, 2 or more in practice. Whole sentence. At most 40 bytes. */
            (void)I18N_FORMAT(out->body, sizeof(out->body),
                              i18n_plural(cat, NULL, "All {0} zone tested", "All {0} zones tested",
                                          ev->arg2),
                              (unsigned)ev->arg2);
        } else {
            /* TR: The walk test's result; {0} is how many zones were tested, {1} how many the
             * test covers ("14 of 16 zones tested"). Whole sentence. At most 40 bytes. */
            (void)I18N_FORMAT(out->body, sizeof(out->body),
                              i18n_plural(cat, NULL, "{0} of {1} zone tested",
                                          "{0} of {1} zones tested", ev->arg2),
                              (unsigned)ev->arg, (unsigned)ev->arg2);
        }
        break;
    case ALARM_EV_WALK_TEST_ZONE:
        out->icon = NOTIFY_ICON_FOOTPRINTS;
        out->accent = NOTIFY_ACCENT_BLUE;
        (void)I18N_FORMAT(out->title, sizeof(out->title), i18n_text(cat, NULL, "{0} tested"),
                          zname);
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Walk test"));
        break;
    case ALARM_EV_WALK_TEST_EXTEND:
        out->icon = NOTIFY_ICON_FOOTPRINTS;
        out->accent = NOTIFY_ACCENT_BLUE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Walk test extended"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_WALK_TEST_WARNING:
        /* arg = the minutes left (5 or 1). */
        out->icon = NOTIFY_ICON_FOOTPRINTS;
        out->accent = NOTIFY_ACCENT_BLUE;
        /* TR: The walk test is about to time out; {0} is the minutes left. Whole sentence. At
         * most 40 bytes. */
        (void)I18N_FORMAT(out->title, sizeof(out->title),
                          i18n_plural(cat, NULL, "Walk test ends in {0} minute",
                                      "Walk test ends in {0} minutes", ev->arg),
                          (unsigned)ev->arg);
        break;
    case ALARM_EV_MONITOR_FAULT:
    case ALARM_EV_MONITOR_RESTORE:
        out->icon = NOTIFY_ICON_ACTIVITY;
        (void)I18N_FORMAT(out->title, sizeof(out->title),
                          ev->type == ALARM_EV_MONITOR_FAULT
                              ? i18n_text(cat, NULL, "{0} active")
                              : i18n_text(cat, NULL, "{0} back to normal"),
                          zname);
        break;
    case ALARM_EV_USER_CHANGED:
    case ALARM_EV_USER_DELETED: {
        char uname[ALARM_NAME_LEN + 8];
        out->icon = NOTIFY_ICON_USER;
        notify_user_name(cat, cfg, ev->arg, uname, sizeof(uname));
        if (ev->type == ALARM_EV_USER_CHANGED) {
            (void)I18N_FORMAT(out->title, sizeof(out->title),
                              i18n_text(cat, NULL, "Code changed for {0}"), uname);
        } else {
            (void)I18N_FORMAT(out->title, sizeof(out->title), i18n_text(cat, NULL, "{0} deleted"),
                              uname);
        }
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    }
    case ALARM_EV_CONFIG_CHANGED:
        out->icon = NOTIFY_ICON_SETTINGS;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Settings changed"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_USER_UPDATED: {
        char uname[ALARM_NAME_LEN + 8];
        out->icon = NOTIFY_ICON_USER;
        (void)I18N_FORMAT(out->title, sizeof(out->title), i18n_text(cat, NULL, "{0} changed"),
                          notify_user_name(cat, cfg, ev->arg, uname, sizeof(uname)));
        /* TR: The line under "{0} changed"; {0} is the user's new level ("Guest"). */
        (void)I18N_FORMAT(out->body, sizeof(out->body), i18n_text(cat, NULL, "Now {0}"),
                          notify_authority_name(cat, (alarm_authority_t)ev->arg2));
        break;
    }
    case ALARM_EV_PROGRAM_ENTER:
        out->icon = NOTIFY_ICON_WRENCH;
        out->accent = ev->arg == 1u ? NOTIFY_ACCENT_AMBER : NOTIFY_ACCENT_BLUE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Installer mode"));
        put(out->body, sizeof(out->body),
            ev->arg == 1u ? i18n_text(cat, NULL, "Opened without a code after power-up")
                          : i18n_text(cat, NULL, "Opened with the installer code"));
        break;
    case ALARM_EV_PROGRAM_EXIT:
        out->icon = NOTIFY_ICON_WRENCH;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Installer mode ended"));
        put(out->body, sizeof(out->body),
            ev->arg == 1u ? i18n_text(cat, NULL, "After 15 minutes without activity")
                          : i18n_text(cat, NULL, "The installer left"));
        break;
    case ALARM_EV_FACTORY_RESET:
        out->icon = NOTIFY_ICON_ROTATE_CCW;
        out->accent = NOTIFY_ACCENT_RED;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Factory reset"));
        put(out->body, sizeof(out->body),
            (ev->arg & 1u) ? i18n_text(cat, NULL, "Everything erased, event log too")
                           : i18n_text(cat, NULL, "Everything erased"));
        break;
    case ALARM_EV_FIRMWARE_UPDATE: {
        static const char *const titles[] = {
            I18N_NOOP("Installing an update"), I18N_NOOP("Firmware updated"),
            I18N_NOOP("Update rolled back"), I18N_NOOP("Wi-Fi module updated"),
            I18N_NOOP("Wi-Fi module update failed"), I18N_NOOP("Update server changed"),
            I18N_NOOP("Update stopped"), I18N_NOOP("Update going back"),
            I18N_NOOP("Update put off")};
        static const char *const details[] = {
            I18N_NOOP("The panel restarts with the new version"),
            I18N_NOOP("The new version passed its self-test"),
            I18N_NOOP("The new version failed; the previous one runs"),
            I18N_NOOP("It runs its new firmware"), I18N_NOOP("It keeps its old firmware"),
            I18N_NOOP("Updates come from another server now"),
            I18N_NOOP("The panel keeps the version it runs"),
            I18N_NOOP("The new version failed its self-test; the previous one returns"),
            I18N_NOOP("It installs once everything is quiet")};
        const unsigned stage = ev->arg <= ALARM_FW_RESTART_PUT_OFF ? ev->arg : ALARM_FW_UPDATED;
        out->icon = NOTIFY_ICON_REFRESH;
        out->accent = (stage == ALARM_FW_ROLLED_BACK || stage == ALARM_FW_CP_FAILED ||
                       stage == ALARM_FW_SERVER_CHANGED || stage == ALARM_FW_ROLLING_BACK)
                          ? NOTIFY_ACCENT_AMBER
                          : NOTIFY_ACCENT_BLUE;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, titles[stage]));
        put(out->body, sizeof(out->body), i18n_text(cat, NULL, details[stage]));
        break;
    }
    case ALARM_EV_DURESS:
        out->icon = NOTIFY_ICON_SIREN;
        out->accent = NOTIFY_ACCENT_RED;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Duress code used"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    /* The Z-Wave network (docs/features/36 network.md §15). */
    case ALARM_EV_ZWAVE_ADDED:
    case ALARM_EV_ZWAVE_ADDED_INSECURE: {
        const bool secure = ev->type == ALARM_EV_ZWAVE_ADDED;
        char dev[ALARM_NAME_LEN + 8];
        zwave_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = secure ? NOTIFY_ICON_RADIO_TOWER : NOTIFY_ICON_SHIELD_ALERT;
        out->accent = secure ? NOTIFY_ACCENT_GREEN : NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title),
            secure ? i18n_text(cat, NULL, "Z-Wave device added")
                   : i18n_text(cat, NULL, "Z-Wave device added without encryption"));
        join2(out->body, sizeof(out->body), dev, zwave_join_words(cat, ev->arg2));
        break;
    }
    case ALARM_EV_ZWAVE_REMOVED: {
        char dev[ALARM_NAME_LEN + 8];
        zwave_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_CIRCLE_SLASH;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Z-Wave device removed"));
        join2(out->body, sizeof(out->body), dev,
              ev->arg2 == ALARM_ZW_REMOVED_FAILED
                  ? i18n_text(cat, NULL, "Removed as broken or gone")
                  : NULL);
        break;
    }
    case ALARM_EV_ZWAVE_LEFT: {
        char dev[ALARM_NAME_LEN + 8];
        zwave_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_TRIANGLE_ALERT;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Z-Wave device left the network"));
        join2(out->body, sizeof(out->body), dev,
              ev->arg2 == ALARM_ZW_LEFT_RESET_LOCALLY
                  ? i18n_text(cat, NULL, "It was reset with its own button")
                  : i18n_text(cat, NULL, "The radio no longer lists it"));
        break;
    }
    case ALARM_EV_ZWAVE_NETWORK_RESET:
        out->icon = NOTIFY_ICON_ROTATE_CCW;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Z-Wave network reset"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        if (out->body[0] == '\0') {
            put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Every device must be added again"));
        }
        break;
    case ALARM_EV_ZWAVE_RADIO_RESTARTED:
        out->icon = NOTIFY_ICON_REFRESH;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Z-Wave radio restarted"));
        if (ev->arg2 == 0xFFu) {
            /* ZW-041: the count stops at 255 - a brown-out loop may have restarted it more. */
            /* TR: The line under "Z-Wave radio restarted" when it restarted 255 times or more
             * within an hour. Whole sentence. At most 90 bytes. */
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL,
                          "It restarted by itself 255 times or more; the panel set it up again"));
        } else if (ev->arg2 > 1u) {
            /* ZW-041: one notice an hour at most stands for every restart since the last. */
            /* TR: The line under "Z-Wave radio restarted" when it restarted more than once; {0}
             * is how many times, 2 or more. Whole sentence. At most 90 bytes. */
            const char *said = i18n_plural(
                cat, NULL, "It restarted by itself {0} time; the panel set it up again",
                "It restarted by itself {0} times; the panel set it up again", ev->arg2);
            (void)I18N_FORMAT(out->body, sizeof(out->body), said, (unsigned)ev->arg2);
        } else {
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "It restarted by itself; the panel set it up again"));
        }
        break;
    case ALARM_EV_ZWAVE_SOFTWARE: {
        if (ev->arg2 >= ALARM_ZW_SW_RADIO_STARTED) { /* the radio itself (network.md §14) */
            const bool failed = ev->arg2 == ALARM_ZW_SW_RADIO_FAILED;
            out->icon = failed ? NOTIFY_ICON_TRIANGLE_ALERT : NOTIFY_ICON_REFRESH;
            out->accent = failed                                   ? NOTIFY_ACCENT_AMBER
                          : ev->arg2 == ALARM_ZW_SW_RADIO_UPDATED ? NOTIFY_ACCENT_GREEN
                                                                  : out->accent;
            put(out->title, sizeof(out->title),
                ev->arg2 == ALARM_ZW_SW_RADIO_STARTED
                    ? i18n_text(cat, NULL, "Z-Wave radio software update started")
                : ev->arg2 == ALARM_ZW_SW_RADIO_UPDATED
                    ? i18n_text(cat, NULL, "Z-Wave radio software updated")
                    : i18n_text(cat, NULL, "Z-Wave radio software update failed"));
            if (ev->arg2 == ALARM_ZW_SW_RADIO_STARTED) {
                by_whom(cat, cfg, ev, out->body, sizeof(out->body));
            }
            break;
        }
        char dev[ALARM_NAME_LEN + 8];
        zwave_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = ev->arg2 == ALARM_ZW_SW_FAILED ? NOTIFY_ICON_TRIANGLE_ALERT : NOTIFY_ICON_REFRESH;
        out->accent = ev->arg2 == ALARM_ZW_SW_FAILED   ? NOTIFY_ACCENT_AMBER
                      : ev->arg2 == ALARM_ZW_SW_UPDATED ? NOTIFY_ACCENT_GREEN
                                                        : out->accent;
        put(out->title, sizeof(out->title),
            ev->arg2 == ALARM_ZW_SW_STARTED
                ? i18n_text(cat, NULL, "Z-Wave device software update started")
            : ev->arg2 == ALARM_ZW_SW_UPDATED
                ? i18n_text(cat, NULL, "Z-Wave device software updated")
                : i18n_text(cat, NULL, "Z-Wave device software update failed"));
        join2(out->body, sizeof(out->body), dev,
              ev->arg2 == ALARM_ZW_SW_FAILED ? i18n_text(cat, NULL, "It keeps its old software")
                                             : NULL);
        break;
    }
    case ALARM_EV_ZWAVE_GARAGE: {
        static const char *const T[] = {
            I18N_NOOP("Garage door opening"), I18N_NOOP("Garage door closing"),
            I18N_NOOP("Garage door opened"), I18N_NOOP("Garage door closed"),
            I18N_NOOP("Garage door didn't move")};
        out->icon = NOTIFY_ICON_DOOR_OPEN;
        out->accent = ev->arg2 == ALARM_ZW_GARAGE_FAILED ? NOTIFY_ACCENT_AMBER : out->accent;
        put(out->title, sizeof(out->title),
            ev->arg2 <= ALARM_ZW_GARAGE_FAILED ? i18n_text(cat, NULL, T[ev->arg2])
                                               : i18n_text(cat, NULL, "Garage door"));
        if (ev->arg2 == ALARM_ZW_GARAGE_OPEN_ASKED || ev->arg2 == ALARM_ZW_GARAGE_CLOSE_ASKED) {
            by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        } else if (ev->arg2 == ALARM_ZW_GARAGE_FAILED) {
            put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Check the door and its opener"));
        }
        break;
    }
    case ALARM_EV_BACKUP_MADE:
        out->icon = NOTIFY_ICON_HARD_DRIVE;
        out->accent = NOTIFY_ACCENT_BLUE;
        /* TR: The installer downloaded the panel's encrypted backup file. */
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Backup made"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_BACKUP_RESTORED:
        out->icon = NOTIFY_ICON_HARD_DRIVE;
        out->accent = NOTIFY_ACCENT_AMBER;
        /* TR: A backup file replaced the panel's settings; the panel restarts. */
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Backup restored"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_CLOCK_SET:
        out->icon = NOTIFY_ICON_CLOCK;
        out->accent = NOTIFY_ACCENT_BLUE;
        /* TR: The date and time were set by hand (the line under it says by whom). */
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Clock set"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        break;
    case ALARM_EV_ZWAVE_JAMMING: {
        char dev[ALARM_NAME_LEN + 8];
        zwave_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_TRIANGLE_ALERT;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Radio interference"));
        join2(out->body, sizeof(out->body), dev,
              i18n_text(cat, NULL, "It reports something drowning out its radio"));
        break;
    }
    /* The Zigbee network (docs/features/20 devices.md §11, ZB-019). */
    case ALARM_EV_ZIGBEE_ADDED: {
        char dev[ALARM_NAME_LEN + 8];
        zigbee_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_RADIO_TOWER;
        out->accent = NOTIFY_ACCENT_GREEN;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee device added"));
        join2(out->body, sizeof(out->body), dev, zigbee_join_words(cat, ev->arg2));
        break;
    }
    case ALARM_EV_ZIGBEE_REMOVED:
    case ALARM_EV_ZIGBEE_FORGOTTEN: {
        const bool forgotten = ev->type == ALARM_EV_ZIGBEE_FORGOTTEN;
        char dev[ALARM_NAME_LEN + 8];
        zigbee_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_CIRCLE_SLASH;
        /* TR: Forgotten: the panel removed the device from its list without the device's
         * answer (it was broken or gone), so the device itself may still think it belongs. */
        put(out->title, sizeof(out->title),
            forgotten ? i18n_text(cat, NULL, "Zigbee device forgotten")
                      : i18n_text(cat, NULL, "Zigbee device removed"));
        join2(out->body, sizeof(out->body), dev,
              forgotten ? i18n_text(cat, NULL, "Reset it before adding it again") : NULL);
        break;
    }
    case ALARM_EV_ZIGBEE_LEFT: {
        char dev[ALARM_NAME_LEN + 8];
        zigbee_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_TRIANGLE_ALERT;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee device left the network"));
        join2(out->body, sizeof(out->body), dev, i18n_text(cat, NULL, "It left by itself"));
        break;
    }
    case ALARM_EV_ZIGBEE_REPLACED: {
        char dev[ALARM_NAME_LEN + 8];
        zigbee_device_words(cat, cfg, ev, dev, sizeof(dev));
        out->icon = NOTIFY_ICON_REFRESH;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee device replaced"));
        join2(out->body, sizeof(out->body), dev,
              i18n_text(cat, NULL, "A new device took its place"));
        break;
    }
    case ALARM_EV_ZIGBEE_JOIN_REFUSED: {
        static const char *const WHY[] = {
            I18N_NOOP("The radio didn't say why"),
            I18N_NOOP("Its install code isn't on the panel's list"),
            I18N_NOOP("A device the panel doesn't know tried to rejoin")};
        out->icon = NOTIFY_ICON_SHIELD_ALERT;
        out->accent = NOTIFY_ACCENT_AMBER;
        /* TR: The panel's Zigbee network refused a device that tried to join it. */
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee device turned away"));
        put(out->body, sizeof(out->body),
            i18n_text(cat, NULL, WHY[ev->arg2 < ALARM_ZB_REFUSED_COUNT ? ev->arg2 : 0u]));
        break;
    }
    case ALARM_EV_ZIGBEE_CHANNEL: {
        char channel[32];
        char who[NOTIFY_BODY_LEN];
        out->icon = NOTIFY_ICON_RADIO_TOWER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee channel changed"));
        /* TR: The radio channel the Zigbee network now uses; {0} is its number (11 to 26). */
        (void)I18N_FORMAT(channel, sizeof(channel), i18n_text(cat, NULL, "Channel {0}"),
                          (unsigned)ev->arg2);
        by_whom(cat, cfg, ev, who, sizeof(who));
        join2(out->body, sizeof(out->body), channel, who[0] != '\0' ? who : NULL);
        break;
    }
    case ALARM_EV_ZIGBEE_NETWORK_RESET:
        out->icon = NOTIFY_ICON_ROTATE_CCW;
        out->accent = NOTIFY_ACCENT_AMBER;
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee network reset"));
        by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        if (out->body[0] == '\0') {
            put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Every device must be added again"));
        }
        break;
    case ALARM_EV_AUTOMATION_FAILED: {
        /* docs/features/41 AUT-009: arg the reason (alarm_auto_fail_t), arg2 the action. The
         * device is named by its zone: notify sees only the configuration. */
        char line[NOTIFY_BODY_LEN];
        char who[NOTIFY_BODY_LEN];
        const char *dev = zname[0] != '\0' ? zname
                          /* TR: Stands for the device an automation tried to work, where the
                           * panel has no zone to name it by: "The device didn't answer". */
                          : i18n_text(cat, "automation failed", "The device");
        out->icon = NOTIFY_ICON_TRIANGLE_ALERT;
        out->accent = NOTIFY_ACCENT_AMBER;
        /* TR: The title when one of the panel's automations (rules the installer set up) failed
         * to lock a lock, close a garage door or cover, or arm the system. */
        put(out->title, sizeof(out->title),
            ev->arg2 == ALARM_AUTO_ACT_LOCK    ? i18n_text(cat, NULL, "An automation didn't lock")
            : ev->arg2 == ALARM_AUTO_ACT_CLOSE ? i18n_text(cat, NULL, "An automation didn't close")
                                               : i18n_text(cat, NULL, "An automation didn't arm"));
        /* TR: Why an automation's action didn't happen; {0} is the device's zone name or "The
         * device". At most 60 bytes with the name. */
        switch ((alarm_auto_fail_t)ev->arg) {
        case ALARM_AUTO_FAIL_NO_ANSWER:
            (void)I18N_FORMAT(line, sizeof(line), i18n_text(cat, "automation failed", "{0} didn't answer"), dev);
            break;
        case ALARM_AUTO_FAIL_UNREACHABLE:
            (void)I18N_FORMAT(line, sizeof(line), i18n_text(cat, "automation failed", "{0} wasn't reachable"),
                              dev);
            break;
        case ALARM_AUTO_FAIL_REFUSED:
        case ALARM_AUTO_FAIL_NOT_ALLOWED:
            (void)I18N_FORMAT(line, sizeof(line), i18n_text(cat, "automation failed", "{0} said no"), dev);
            break;
        case ALARM_AUTO_FAIL_MISSING:
            (void)I18N_FORMAT(line, sizeof(line),
                              i18n_text(cat, "automation failed", "{0} isn't on the panel now"), dev);
            break;
        case ALARM_AUTO_FAIL_BUSY:
            (void)I18N_FORMAT(line, sizeof(line), i18n_text(cat, "automation failed", "{0} was busy"), dev);
            break;
        case ALARM_AUTO_FAIL_NO_WARNING:
            /* TR: Why an automation didn't arm: it must warn the household before arming and
             * had no time left to. */
            put(line, sizeof(line), i18n_text(cat, NULL, "It had no time to warn first"));
            break;
        default: line[0] = '\0'; break;
        }
        /* the short form: the device's line already says what failed */
        if (ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION) {
            auto_who(cat, cfg, ev, false, who, sizeof(who));
        } else {
            by_whom(cat, cfg, ev, who, sizeof(who));
        }
        if (line[0] == '\0') {
            put(out->body, sizeof(out->body), who);
        } else {
            join2(out->body, sizeof(out->body), line, who[0] != '\0' ? who : NULL);
        }
        break;
    }
    case ALARM_EV_ARMING_PUT_OFF: {
        /* AUT-013, D26, D30: whose code put it off; never "By automation". */
        char who[NOTIFY_BODY_LEN];
        char name[ALARM_NAME_LEN + 8];
        out->icon = NOTIFY_ICON_CLOCK;
        /* TR: The title when someone put an automation's arming off (docs/features/41): an hour
         * later, no arming tonight (before 6 AM), or none today (before midnight). At most 63
         * bytes. */
        put(out->title, sizeof(out->title),
            ev->arg == ALARM_PUT_OFF_HOUR    ? i18n_text(cat, NULL, "Arming put off an hour")
            : ev->arg == ALARM_PUT_OFF_DAY   ? i18n_text(cat, NULL, "Arming skipped today")
                                             : i18n_text(cat, NULL, "Night arming skipped tonight"));
        (void)I18N_FORMAT(who, sizeof(who), i18n_text(cat, NULL, "By {0}"),
                          notify_user_name(cat, cfg, ev->user, name, sizeof(name)));
        char at[24];
        char until[48];
        if (ev->arg == ALARM_PUT_OFF_HOUR &&
            notify_time_of_day(cat, alarm_put_off_until(ev), at, sizeof(at))) {
            /* TR: After "By Pat" under "Arming put off an hour": when it now arms ("By Pat ·
             * until 11:30 PM"); {0} is a time of day. At most 32 bytes. */
            (void)I18N_FORMAT(until, sizeof(until), i18n_text(cat, NULL, "until {0}"), at);
            join2(out->body, sizeof(out->body), who, until);
        } else {
            put(out->body, sizeof(out->body), who);
        }
        break;
    }
    case ALARM_EV_ZIGBEE_RADIO_RESTARTED:
        out->icon = NOTIFY_ICON_REFRESH;
        out->accent = NOTIFY_ACCENT_AMBER;
        if (ev->arg2 == ALARM_ZB_RESTART_PAUSED) {
            out->icon = NOTIFY_ICON_TRIANGLE_ALERT;
            put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee radio paused"));
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "It restarted too often; the installer can try again"));
            break;
        }
        put(out->title, sizeof(out->title), i18n_text(cat, NULL, "Zigbee radio restarted"));
        if (ev->arg2 == ALARM_ZB_RESTART_ASKED) {
            out->accent = NOTIFY_ACCENT_GRAY;
            by_whom(cat, cfg, ev, out->body, sizeof(out->body));
        } else {
            put(out->body, sizeof(out->body),
                i18n_text(cat, NULL, "It restarted by itself; the panel set it up again"));
        }
        break;
    default:
        return false;
    }
    if (ev->flags & ALARM_EVF_SILENT) {
        const size_t n = strlen(out->body);
        /* TR: Added at the end of the line under an event that was silent (duress, silent panic). */
        if (n > 0u) {
            (void)I18N_FORMAT(out->body + n, sizeof(out->body) - n, " \xC2\xB7 {0}",
                              i18n_text(cat, NULL, "Silent"));
        } else {
            put(out->body, sizeof(out->body), i18n_text(cat, NULL, "Silent"));
        }
    }
    return true;
}

notify_cat_t notify_category(alarm_event_type_t type)
{
    switch (type) {
    case ALARM_EV_ALARM:
    case ALARM_EV_ALARM_RESTORE:
    case ALARM_EV_EXIT_ERROR:
    case ALARM_EV_RECENT_CLOSING:
    case ALARM_EV_ALARM_ABORTED:
    case ALARM_EV_ALARM_CANCELED:
    case ALARM_EV_ALARM_SILENCED:
    case ALARM_EV_BELL_TIMEOUT:
    case ALARM_EV_SWINGER_SHUTDOWN:
    case ALARM_EV_MEMORY_CLEARED:
    case ALARM_EV_DURESS: return NOTIFY_CAT_ALARM;
    case ALARM_EV_TROUBLE:
    case ALARM_EV_TROUBLE_RESTORE:
    case ALARM_EV_TROUBLES_ACKED:
    case ALARM_EV_AUTOMATION_FAILED: return NOTIFY_CAT_TROUBLE; /* AUT-009, NOTE-037: on by default */
    case ALARM_EV_ARMED:
    case ALARM_EV_ARM_FAILED:
    case ALARM_EV_DISARMED:
    case ALARM_EV_AUTO_STAY:
    case ALARM_EV_EXIT_RESTART:
    case ALARM_EV_ENTRY_DELAY:
    case ALARM_EV_ARMING_PUT_OFF: return NOTIFY_CAT_ARMING; /* AUT-013 */
    case ALARM_EV_BYPASS:
    case ALARM_EV_UNBYPASS: return NOTIFY_CAT_BYPASS;
    default: return NOTIFY_CAT_SYSTEM;
    }
}

/* ---- zone cards ---------------------------------------------------------------------------- */

static alarm_device_class_t zone_class(const alarm_config_t *cfg, uint16_t zone)
{
    if (cfg == NULL || zone < 1u || zone > ALARM_MAX_ZONES) {
        return ALARM_DEVCLASS_GENERIC;
    }
    return (alarm_device_class_t)cfg->zones[zone - 1u].device_class;
}

static bool life_safety(alarm_device_class_t cls)
{
    return cls == ALARM_DEVCLASS_SMOKE || cls == ALARM_DEVCLASS_HEAT || cls == ALARM_DEVCLASS_CO ||
           cls == ALARM_DEVCLASS_GAS;
}

static const char *active_words(const i18n_catalog_t *cat, alarm_device_class_t cls)
{
    /* TR: A zone card's state when its sensor is triggered. At most 40 bytes. */
    switch (cls) {
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
    case ALARM_DEVCLASS_GARAGE_DOOR: return i18n_text(cat, "zone state", "Open");
    case ALARM_DEVCLASS_MOTION: return i18n_text(cat, "zone state", "Motion");
    case ALARM_DEVCLASS_GLASS_BREAK: return i18n_text(cat, "zone state", "Glass break");
    case ALARM_DEVCLASS_SMOKE: return i18n_text(cat, "zone state", "Smoke detected");
    case ALARM_DEVCLASS_HEAT: return i18n_text(cat, "zone state", "Heat detected");
    case ALARM_DEVCLASS_CO: return i18n_text(cat, "zone state", "CO detected");
    case ALARM_DEVCLASS_GAS: return i18n_text(cat, "zone state", "Gas detected");
    case ALARM_DEVCLASS_WATER: return i18n_text(cat, "zone state", "Water detected");
    case ALARM_DEVCLASS_TEMPERATURE: return i18n_text(cat, "zone state", "Out of range");
    case ALARM_DEVCLASS_VIBRATION: return i18n_text(cat, "zone state", "Vibration");
    case ALARM_DEVCLASS_PANIC_BUTTON:
    case ALARM_DEVCLASS_KEYFOB:
    case ALARM_DEVCLASS_KEYSWITCH: return i18n_text(cat, "zone state", "Pressed");
    default: return i18n_text(cat, "zone state", "Active");
    }
}

static const char *normal_words(const i18n_catalog_t *cat, alarm_device_class_t cls)
{
    /* TR: A zone card's state when its sensor is at rest. At most 40 bytes. */
    switch (cls) {
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
    case ALARM_DEVCLASS_GARAGE_DOOR: return i18n_text(cat, "zone state", "Closed");
    case ALARM_DEVCLASS_MOTION: return i18n_text(cat, "zone state", "No motion");
    case ALARM_DEVCLASS_WATER: return i18n_text(cat, "zone state", "Dry");
    default: return i18n_text(cat, "zone state", "Normal");
    }
}

static const char *zone_trouble_words(const i18n_catalog_t *cat, uint8_t trouble)
{
    /* TR: A zone card's state when its sensor has a fault. At most 40 bytes. */
    if (trouble & (1u << ALARM_ZTRBL_TAMPER)) {
        return i18n_text(cat, "zone trouble", "Tamper");
    }
    if (trouble & (1u << ALARM_ZTRBL_SUPERVISION)) {
        return i18n_text(cat, "zone trouble", "Not reporting");
    }
    if (trouble & (1u << ALARM_ZTRBL_LOW_BATTERY)) {
        return i18n_text(cat, "zone trouble", "Low battery");
    }
    if (trouble & (1u << ALARM_ZTRBL_DAY_FAULT)) {
        return i18n_text(cat, "zone trouble", "Open during the day");
    }
    return i18n_text(cat, "zone trouble", "Sensor fault");
}

notify_accent_t notify_zone_state(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                                  uint16_t zone, const alarm_zone_status_t *zs, char *detail,
                                  size_t len)
{
    const alarm_device_class_t cls = zone_class(cfg, zone);
    const uint16_t f = zs->flags;
    const bool active = (f & ALARM_ZS_FAULT) != 0u;
    const uint16_t trouble_flags = ALARM_ZS_TROUBLE | ALARM_ZS_TAMPER | ALARM_ZS_LOW_BATTERY |
                                   ALARM_ZS_SUPERVISION;
    if (f & ALARM_ZS_ALARM) {
        join2(detail, len, i18n_text(cat, "zone state", "Alarm"),
              active ? active_words(cat, cls) : NULL);
        return NOTIFY_ACCENT_RED;
    }
    if (f & ALARM_ZS_BYPASSED) {
        join2(detail, len, i18n_text(cat, "zone state", "Bypassed"),
              active ? active_words(cat, cls) : normal_words(cat, cls));
        return NOTIFY_ACCENT_GRAY;
    }
    if (f & trouble_flags) {
        put(detail, len, zone_trouble_words(cat, zs->trouble));
        return NOTIFY_ACCENT_AMBER;
    }
    if (f & ALARM_ZS_SWINGER) {
        put(detail, len, i18n_text(cat, "zone state", "Shut down until disarmed"));
        return NOTIFY_ACCENT_GRAY;
    }
    if (active) {
        put(detail, len, active_words(cat, cls));
        return life_safety(cls) ? NOTIFY_ACCENT_RED : NOTIFY_ACCENT_AMBER;
    }
    put(detail, len, normal_words(cat, cls));
    return NOTIFY_ACCENT_GREEN;
}
