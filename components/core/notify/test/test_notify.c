/*
 * test_notify.c - what the panel says about an event, in words (components/core/notify).
 *
 * The golden table below is the wording contract: the Activity list on the panel and a push
 * notification to a phone both read these sentences, so a reword shows up here as a visible
 * diff in the same change that makes it.
 * Requirements: NOTE-001..006 (the words), I18N-006 and I18N-030 (a catalog reaches them), NOTE-030..034 and NOTE-037..039 (the gate;
 * NOTE-035/036 are net_notify's; NOTE-040 a put-off), NOTE-041 (an automation's words), AUT-009/AUT-013 and D30 (an automation named, docs/features/41), NTFY-001/-009 and PUSH-001/-004/-005/-009 (the requests)
 * (docs/features/26-Notifications)
 */
#include <stdio.h>
#include <string.h>

#include "notify/notify_auto.h"
#include "notify/notify_gate.h"
#include "notify/notify_http.h"
#include "notify/notify_words.h"
#include "unity.h"

static alarm_config_t cfg;

void setUp(void);
void tearDown(void);

void setUp(void)
{
    alarm_config_defaults(&cfg);
    snprintf(cfg.zones[0].name, sizeof(cfg.zones[0].name), "Front Door");
    snprintf(cfg.zones[4].name, sizeof(cfg.zones[4].name), "Hallway Motion");
    snprintf(cfg.users[0].name, sizeof(cfg.users[0].name), "Master");
}

void tearDown(void)
{
    notify_set_hooks(NULL);
}

static alarm_event_t ev_of(alarm_event_type_t type)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.partition = 1u;
    return ev;
}

static void words_of(const alarm_event_t *ev, notify_words_t *w)
{
    memset(w, 0, sizeof(*w));
    TEST_ASSERT_TRUE_MESSAGE(notify_words(NULL, &cfg, ev, true, w), "no words for that event");
}

/* NOTE-001: every event type the engine can log has words. A new type added to
 * alarm_event_type_t without words here would otherwise reach a phone as nothing at all. */
static void test_every_event_type_has_words(void)
{
    for (unsigned t = 0; t < (unsigned)ALARM_EV_COUNT; t++) {
        alarm_event_t ev = ev_of((alarm_event_type_t)t);
        ev.zone = 1u;
        ev.user = 1u;
        if (t == ALARM_EV_TROUBLE || t == ALARM_EV_TROUBLE_RESTORE) {
            ev.arg2 = ALARM_TRBL_FAMILY_SYSTEM;
            ev.arg = ALARM_TRBL_AC_LOSS;
            ev.zone = 0u;
        }
        notify_words_t w;
        memset(&w, 0, sizeof(w));
        char why[96];
        snprintf(why, sizeof(why), "event type %u has no words", t);
        TEST_ASSERT_TRUE_MESSAGE(notify_words(NULL, &cfg, &ev, true, &w), why);
        TEST_ASSERT_TRUE_MESSAGE(w.title[0] != '\0', why);
        TEST_ASSERT_TRUE_MESSAGE(w.cat < NOTIFY_CAT_COUNT, why);
        TEST_ASSERT_TRUE_MESSAGE(w.icon < NOTIFY_ICON_COUNT, why);
        TEST_ASSERT_TRUE_MESSAGE(w.accent < NOTIFY_ACCENT_COUNT, why);
    }
}

/* NOTE-002: the golden wording. Change a line here only when you mean to change what the panel
 * and the phone both say. */
static void test_golden_wording(void)
{
    notify_words_t w;

    alarm_event_t ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.zone = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Burglary alarm", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_ALARM, w.cat);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_RED, w.accent);

    ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_FIRE;
    ev.zone = 5u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Fire alarm", w.title);
    TEST_ASSERT_EQUAL_STRING("Hallway Motion", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_FLAME, w.icon);

    ev = ev_of(ALARM_EV_ARMED);
    ev.arg = ALARM_LEVEL_AWAY;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Armed Away", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_ARMING, w.cat);

    ev = ev_of(ALARM_EV_ARMED);
    ev.arg = ALARM_LEVEL_STAY;
    ev.arg2 = ALARM_ARM_NO_ENTRY_DELAY;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Armed Stay · No entry delay", w.title);

    ev.arg = ALARM_LEVEL_AWAY; /* "Maximum" (away, no entry delay): plain words instead */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Armed Away · No entry delay", w.title);

    ev = ev_of(ALARM_EV_DISARMED);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Disarmed", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);

    /* docs/features/14 CFG-005, CFG-006: the backup file's audit trail, and who did it. */
    ev = ev_of(ALARM_EV_BACKUP_MADE);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Backup made", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_SYSTEM, w.cat);
    ev = ev_of(ALARM_EV_BACKUP_RESTORED);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Backup restored", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);

    /* docs/features/35 RTC-014: the clock set by hand, and who set it. */
    ev = ev_of(ALARM_EV_CLOCK_SET);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Clock set", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_SYSTEM, w.cat);

    /* An unnamed zone falls back to its number, never to nothing. */
    ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.zone = 7u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zone 7", w.body);

    /* Remote and keypad sources name themselves when no user did it. */
    ev = ev_of(ALARM_EV_DISARMED);
    ev.source.kind = ALARM_SRC_REMOTE;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Remotely", w.body);

    /* CORE-USR-027: the browser Panel Manager names itself, so a disarm from it reads apart
     * from one at a keypad or another remote source; the person who signed in comes first. */
    ev = ev_of(ALARM_EV_DISARMED);
    ev.source.kind = ALARM_SRC_REMOTE;
    ev.source.id = ALARM_SRC_ID_PANEL_MANAGER;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Master \xC2\xB7 Panel Manager", w.body);
    ev.user = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Panel Manager", w.body);
    ev.source.id = (uint16_t)(ALARM_SRC_ID_PANEL_MANAGER + 1u); /* any other remote client */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Remotely", w.body);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev.source.kind = ALARM_SRC_KEYPAD; /* a keypad whose device number happens to be 31 */
    ev.source.id = ALARM_SRC_ID_PANEL_MANAGER;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);

    /* AUT-009, CORE-USR-029: an automation reads "By automation", before any user it acted as. */
    ev = ev_of(ALARM_EV_ARMED);
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = 3u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation", w.body);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation", w.body);

    /* WALK-010: the walk test's words; the end carries what was tested. */
    ev = ev_of(ALARM_EV_WALK_TEST_START);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Walk test started", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev = ev_of(ALARM_EV_WALK_TEST_END);
    ev.arg = 14u;
    ev.arg2 = 16u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Walk test ended", w.title);
    TEST_ASSERT_EQUAL_STRING("14 of 16 zones tested", w.body);
    ev.arg = 16u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("All 16 zones tested", w.body);
    ev.arg = 0u;
    ev.arg2 = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("0 of 1 zone tested", w.body);
    ev.arg = 0u;
    ev.arg2 = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("0 of 0 zones tested", w.body);
    ev = ev_of(ALARM_EV_WALK_TEST_EXTEND);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Walk test extended", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev = ev_of(ALARM_EV_WALK_TEST_WARNING);
    ev.arg = 5u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Walk test ends in 5 minutes", w.title);
    ev.arg = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Walk test ends in 1 minute", w.title);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_FOOTPRINTS, w.icon);

    /* PWR-007: the line under "Panel started" says why the panel restarted. */
    static const struct {
        alarm_start_reason_t reason;
        const char *body;
    } starts[] = {
        {ALARM_START_POWER_ON, "Protection resumed after power-up"},
        {ALARM_START_SOFTWARE, "Protection resumed after a restart"},
        {ALARM_START_PANIC, "Protection resumed after a fault restart"},
        {ALARM_START_WATCHDOG, "Protection resumed after a fault restart"},
        {ALARM_START_BROWNOUT, "Protection resumed after a power dip"},
        {ALARM_START_EXTERNAL, "Protection resumed after a reset"},
        {ALARM_START_UNKNOWN, "Protection resumed"},
        {ALARM_START_OTHER, "Protection resumed"},
    };
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        ev = ev_of(ALARM_EV_SYSTEM_START);
        ev.arg = (uint8_t)starts[i].reason;
        words_of(&ev, &w);
        TEST_ASSERT_EQUAL_STRING("Panel started", w.title);
        TEST_ASSERT_EQUAL_STRING(starts[i].body, w.body);
        TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_SYSTEM, w.cat);
    }
}

/* NOTE-003: a silent alarm is hidden on a keypad in a hallway, but it is exactly what has to
 * leave the building. notify_words tells them apart with show_silent rather than deciding. */
static void test_silent_events(void)
{
    alarm_event_t ev = ev_of(ALARM_EV_DURESS);
    ev.flags = ALARM_EVF_SILENT;
    ev.user = 1u;

    notify_words_t w;
    memset(&w, 0, sizeof(w));
    TEST_ASSERT_FALSE(notify_words(NULL, &cfg, &ev, false, &w)); /* the keypad never shows it */

    words_of(&ev, &w); /* a notification asks for it */
    TEST_ASSERT_EQUAL_STRING("Duress code used", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master · Silent", w.body);
    TEST_ASSERT_TRUE(w.silent);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_ALARM, w.cat);

    /* CORE-USR-027: a duress user signed in to the Panel Manager still reads as duress. */
    ev.source.kind = ALARM_SRC_REMOTE;
    ev.source.id = ALARM_SRC_ID_PANEL_MANAGER;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Master \xC2\xB7 Panel Manager \xC2\xB7 Silent", w.body);

    /* With nothing else to say, the suffix stands alone without a leading separator. */
    ev = ev_of(ALARM_EV_DURESS);
    ev.flags = ALARM_EVF_SILENT;
    ev.source.kind = ALARM_SRC_SYSTEM;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Silent", w.body);
}

/* NOTE-004: troubles say what is wrong and, on the way back, that it is over. */
static void test_trouble_words(void)
{
    notify_words_t w;

    notify_sys_trouble_words(NULL, ALARM_TRBL_AC_LOSS, false, &w);
    TEST_ASSERT_EQUAL_STRING("Mains power lost", w.title);
    TEST_ASSERT_EQUAL_STRING("Running on battery", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, w.accent);

    notify_sys_trouble_words(NULL, ALARM_TRBL_AC_LOSS, true, &w);
    TEST_ASSERT_EQUAL_STRING("Restored · acknowledge to clear", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GRAY, w.accent);

    notify_zone_trouble_words(NULL, &cfg, 1u, ALARM_ZTRBL_LOW_BATTERY, false, &w);
    TEST_ASSERT_EQUAL_STRING("Front Door · Low battery", w.title);
    TEST_ASSERT_EQUAL_STRING("Check the sensor", w.body);

    /* An event carrying a trouble puts the restore in the title, and the "%.48s" cut keeps a
     * long zone name from pushing " · restored" off the end. */
    alarm_event_t ev = ev_of(ALARM_EV_TROUBLE_RESTORE);
    ev.arg2 = ALARM_TRBL_FAMILY_SYSTEM;
    ev.arg = ALARM_TRBL_AC_LOSS;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Mains power lost · restored", w.title);
    /* The words under it have to agree with it. "· restored" over "Running on battery" says two
     * opposite things, which is what a phone puts on a lock screen (found on the panel). And
     * it is history: "acknowledge to clear" is for the Troubles list, not the log. */
    TEST_ASSERT_EQUAL_STRING("Back to normal", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GREEN, w.accent);

    ev = ev_of(ALARM_EV_TROUBLE);
    ev.arg2 = ALARM_TRBL_FAMILY_ZONE;
    ev.arg = ALARM_ZTRBL_TAMPER;
    ev.zone = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Front Door · Tamper", w.title);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_TROUBLE, w.cat);
}

/* NOTE-006: a zone card's words, the same on the touchscreen's Zones view and in the Panel
 * Manager: what the sensor is doing in its own words, then bypass, trouble and alarm on top. */
static void test_zone_state_words(void)
{
    char words[48];
    alarm_zone_status_t zs;
    memset(&zs, 0, sizeof(zs));
    cfg.zones[0].device_class = ALARM_DEVCLASS_DOOR;
    cfg.zones[4].device_class = ALARM_DEVCLASS_MOTION;
    cfg.zones[5].device_class = ALARM_DEVCLASS_SMOKE;

    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GREEN, notify_zone_state(NULL, &cfg, 1u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Closed", words);
    notify_zone_state(NULL, &cfg, 5u, &zs, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("No motion", words);

    zs.flags = ALARM_ZS_FAULT;
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, notify_zone_state(NULL, &cfg, 1u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Open", words);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_RED, notify_zone_state(NULL, &cfg, 6u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Smoke detected", words);

    zs.flags = ALARM_ZS_FAULT | ALARM_ZS_BYPASSED;
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GRAY, notify_zone_state(NULL, &cfg, 1u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Bypassed · Open", words);

    zs.flags = ALARM_ZS_BYPASSED;
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GRAY, notify_zone_state(NULL, &cfg, 1u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Bypassed · Closed", words);
    notify_zone_state(NULL, &cfg, 5u, &zs, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("Bypassed · No motion", words);

    zs.flags = ALARM_ZS_LOW_BATTERY;
    zs.trouble = (uint8_t)(1u << ALARM_ZTRBL_LOW_BATTERY);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, notify_zone_state(NULL, &cfg, 1u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Low battery", words);

    zs.flags = ALARM_ZS_ALARM | ALARM_ZS_FAULT;
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_RED, notify_zone_state(NULL, &cfg, 1u, &zs, words, sizeof(words)));
    TEST_ASSERT_EQUAL_STRING("Alarm · Open", words);
}

/* ZW-022 (docs/features/36 network.md §15): the Z-Wave network's events in words - the device by
 * its zone when it had one, else "Device 7"; how it joined in the add pages' words; system
 * events, which a notification leaves off by default. */
static void test_zwave_words(void)
{
    notify_words_t w;
    alarm_event_t ev = ev_of(ALARM_EV_ZWAVE_ADDED);
    ev.partition = 0u;
    ev.arg = 7u;
    ev.arg2 = ALARM_ZW_JOIN_S2_AUTH;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device added", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 7 · Secure", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GREEN, w.accent);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_SYSTEM, w.cat);
    ev.arg2 = ALARM_ZW_JOIN_S2_UNAUTH;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Device 7 · Secure (no PIN)", w.body);

    ev = ev_of(ALARM_EV_ZWAVE_ADDED_INSECURE);
    ev.arg = 8u;
    ev.arg2 = ALARM_ZW_JOIN_NOT_SECURE;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device added without encryption", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 8 · Not encrypted", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, w.accent);
    ev.arg2 = ALARM_ZW_JOIN_FAILED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Device 8 · The secure connection failed: remove it and add it again",
                             w.body);
    ev.arg2 = ALARM_ZW_JOIN_DOWNGRADED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Device 8 · Security was downgraded: remove it and add it again",
                             w.body);

    ev = ev_of(ALARM_EV_ZWAVE_REMOVED);
    ev.zone = 1u;
    ev.arg = 7u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device removed", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door", w.body);
    ev.arg2 = ALARM_ZW_REMOVED_FAILED;
    ev.zone = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Device 7 · Removed as broken or gone", w.body);

    ev = ev_of(ALARM_EV_ZWAVE_LEFT);
    ev.zone = 1u;
    ev.arg = 7u;
    ev.arg2 = ALARM_ZW_LEFT_RESET_LOCALLY;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device left the network", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door · It was reset with its own button", w.body);
    ev.arg2 = ALARM_ZW_LEFT_MISSING;
    ev.zone = 0u;
    ev.arg = 0u; /* a Long Range node ID does not fit the event */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("A device · The radio no longer lists it", w.body);

    ev = ev_of(ALARM_EV_ZWAVE_NETWORK_RESET);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave network reset", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev.user = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Every device must be added again", w.body);

    ev = ev_of(ALARM_EV_ZWAVE_RADIO_RESTARTED);
    ev.arg = 0x09u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave radio restarted", w.title);
    TEST_ASSERT_EQUAL_STRING("It restarted by itself; the panel set it up again", w.body);
    /* ZW-041: an event for several restarts (logged once an hour at most) says how many; one, or
     * a record from before the count (0), keeps the sentence above */
    ev.arg2 = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("It restarted by itself; the panel set it up again", w.body);
    ev.arg2 = 2u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("It restarted by itself 2 times; the panel set it up again", w.body);
    ev.arg2 = 254u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("It restarted by itself 254 times; the panel set it up again", w.body);
    /* the count stops at 255: that many says "or more" */
    ev.arg2 = 255u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave radio restarted", w.title);
    TEST_ASSERT_EQUAL_STRING("It restarted by itself 255 times or more; the panel set it up again",
                             w.body);

    /* ZW-033: a device's software update */
    ev = ev_of(ALARM_EV_ZWAVE_SOFTWARE);
    ev.arg = 7u;
    ev.arg2 = ALARM_ZW_SW_STARTED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device software update started", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 7", w.body);
    ev.arg2 = ALARM_ZW_SW_UPDATED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device software updated", w.title);
    ev.arg2 = ALARM_ZW_SW_FAILED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave device software update failed", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 7 · It keeps its old software", w.body);
    /* ZW-025: the radio's own firmware */
    ev.arg = 0u;
    ev.arg2 = ALARM_ZW_SW_RADIO_STARTED;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave radio software update started", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev.arg2 = ALARM_ZW_SW_RADIO_FAILED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Z-Wave radio software update failed", w.title);
    ev.user = 0u;

    /* ZW-035: a garage door, who asked and how it ended */
    ev = ev_of(ALARM_EV_ZWAVE_GARAGE);
    ev.arg2 = ALARM_ZW_GARAGE_OPEN_ASKED;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Garage door opening", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev.arg2 = ALARM_ZW_GARAGE_FAILED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Garage door didn't move", w.title);
    TEST_ASSERT_EQUAL_STRING("Check the door and its opener", w.body);
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_SYSTEM, notify_category(ALARM_EV_ZWAVE_LEFT));

    /* ZW-024 (network.md §9): a device's own jamming report */
    ev = ev_of(ALARM_EV_ZWAVE_JAMMING);
    ev.arg = 7u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Radio interference", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 7 · It reports something drowning out its radio", w.body);
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_SYSTEM, notify_category(ALARM_EV_ZWAVE_JAMMING));
}

/* ZB-019, NOTE-002 (docs/features/20 devices.md §11): the Zigbee network's events in words - the
 * device by its zone, else by its number on the panel's list, else "A device"; how it joined;
 * why a stranger was turned away; who changed the channel or reset the network. */
static void test_zigbee_words(void)
{
    notify_words_t w;
    alarm_event_t ev = ev_of(ALARM_EV_ZIGBEE_ADDED);
    ev.partition = 0u;
    ev.arg = 12u;
    ev.arg2 = ALARM_ZB_JOIN_INSTALL_CODE;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee device added", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 12 \xC2\xB7 Joined with its install code", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_RADIO_TOWER, w.icon);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GREEN, w.accent);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_SYSTEM, w.cat);
    ev.arg2 = ALARM_ZB_JOIN_STANDARD_KEY;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Device 12 \xC2\xB7 Joined with the standard key", w.body);
    ev.arg2 = ALARM_ZB_JOIN_UNKNOWN;
    ev.zone = 1u; /* found again, already feeding a zone */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Front Door \xC2\xB7 How it joined isn't known", w.body);
    ev.arg2 = 0xFFu; /* a newer reader's way: words, not a crash */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Front Door \xC2\xB7 How it joined isn't known", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_REMOVED);
    ev.zone = 1u;
    ev.arg = 12u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee device removed", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_CIRCLE_SLASH, w.icon);

    ev = ev_of(ALARM_EV_ZIGBEE_FORGOTTEN);
    ev.arg = 12u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee device forgotten", w.title);
    TEST_ASSERT_EQUAL_STRING("Device 12 \xC2\xB7 Reset it before adding it again", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_LEFT);
    ev.zone = 5u;
    ev.arg = 9u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee device left the network", w.title);
    TEST_ASSERT_EQUAL_STRING("Hallway Motion \xC2\xB7 It left by itself", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, w.accent);
    ev.zone = 0u;
    ev.arg = 0u; /* a registry id over 255 does not fit the event */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("A device \xC2\xB7 It left by itself", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_REPLACED);
    ev.zone = 1u;
    ev.arg = 14u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee device replaced", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door \xC2\xB7 A new device took its place", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_JOIN_REFUSED);
    ev.arg2 = ALARM_ZB_REFUSED_NO_CODE;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee device turned away", w.title);
    TEST_ASSERT_EQUAL_STRING("Its install code isn't on the panel's list", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_SHIELD_ALERT, w.icon);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, w.accent);
    ev.arg2 = ALARM_ZB_REFUSED_REJOIN;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("A device the panel doesn't know tried to rejoin", w.body);
    ev.arg2 = ALARM_ZB_REFUSED_NOT_SAID;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("The radio didn't say why", w.body);
    ev.arg2 = ALARM_ZB_REFUSED_COUNT; /* out of range: never past the table */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("The radio didn't say why", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_CHANNEL);
    ev.arg2 = 20u;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee channel changed", w.title);
    TEST_ASSERT_EQUAL_STRING("Channel 20 \xC2\xB7 By Master", w.body);
    ev.user = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Channel 20", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_NETWORK_RESET);
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee network reset", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    ev.user = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Every device must be added again", w.body);

    ev = ev_of(ALARM_EV_ZIGBEE_RADIO_RESTARTED);
    ev.arg2 = ALARM_ZB_RESTART_BY_ITSELF;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee radio restarted", w.title);
    TEST_ASSERT_EQUAL_STRING("It restarted by itself; the panel set it up again", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, w.accent);
    ev.arg2 = ALARM_ZB_RESTART_ASKED;
    ev.user = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee radio restarted", w.title);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_GRAY, w.accent);
    ev.arg2 = ALARM_ZB_RESTART_PAUSED;
    ev.user = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Zigbee radio paused", w.title);
    TEST_ASSERT_EQUAL_STRING("It restarted too often; the installer can try again", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_TRIANGLE_ALERT, w.icon);

    /* The longest zone name a household may type still leaves every line whole (NOTE-002). */
    memset(cfg.zones[0].name, 'W', ALARM_NAME_LEN - 1u);
    cfg.zones[0].name[ALARM_NAME_LEN - 1u] = '\0';
    for (unsigned t = ALARM_EV_ZIGBEE_ADDED; t <= ALARM_EV_ZIGBEE_RADIO_RESTARTED; t++) {
        ev = ev_of((alarm_event_type_t)t);
        ev.zone = 1u;
        ev.arg = 255u;
        ev.arg2 = t == ALARM_EV_ZIGBEE_CHANNEL ? 26u : 1u;
        ev.user = 1u;
        words_of(&ev, &w);
        TEST_ASSERT_TRUE(strlen(w.title) < NOTIFY_TITLE_LEN - 1u);
        TEST_ASSERT_TRUE(strlen(w.body) < NOTIFY_BODY_LEN - 1u);
        TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_SYSTEM, notify_category((alarm_event_type_t)t));
    }
}

/* AUT-009, CORE-PROG-012 (docs/features/41 section 7): an automation that didn't lock, close or
 * arm reads its action in the title and its reason in the line, the device by its zone or "The
 * device", then "By automation"; it is a trouble, which a phone gets by default (NOTE-037). */
static void test_automation_failed_words(void)
{
    notify_words_t w;
    alarm_event_t ev = ev_of(ALARM_EV_AUTOMATION_FAILED);
    ev.partition = 0u;
    ev.zone = 1u;
    ev.user = 1u; /* the maker: the words name the automation, not the user */
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = 12u;
    ev.arg = ALARM_AUTO_FAIL_NO_ANSWER;
    ev.arg2 = ALARM_AUTO_ACT_LOCK;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("An automation didn't lock", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door didn't answer \xC2\xB7 By automation", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_TROUBLE, w.cat);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_AMBER, w.accent);

    ev.arg2 = ALARM_AUTO_ACT_CLOSE;
    ev.zone = 0u;
    static const struct {
        uint8_t reason;
        const char *line;
    } LINES[] = {
        {ALARM_AUTO_FAIL_UNREACHABLE, "The device wasn't reachable \xC2\xB7 By automation"},
        {ALARM_AUTO_FAIL_NO_ANSWER, "The device didn't answer \xC2\xB7 By automation"},
        {ALARM_AUTO_FAIL_REFUSED, "The device said no \xC2\xB7 By automation"},
        {ALARM_AUTO_FAIL_MISSING, "The device isn't on the panel now \xC2\xB7 By automation"},
        {ALARM_AUTO_FAIL_BUSY, "The device was busy \xC2\xB7 By automation"},
        {ALARM_AUTO_FAIL_NOT_ALLOWED, "The device said no \xC2\xB7 By automation"},
    };
    for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
        ev.arg = LINES[i].reason;
        words_of(&ev, &w);
        TEST_ASSERT_EQUAL_STRING("An automation didn't close", w.title);
        TEST_ASSERT_EQUAL_STRING(LINES[i].line, w.body);
    }

    ev.arg = ALARM_AUTO_FAIL_NO_WARNING;
    ev.arg2 = ALARM_AUTO_ACT_ARM;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("An automation didn't arm", w.title);
    TEST_ASSERT_EQUAL_STRING("It had no time to warn first \xC2\xB7 By automation", w.body);

    /* A reason this firmware doesn't know (a newer writer's record) still reads, without a line. */
    ev.arg = 200u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("An automation didn't arm", w.title);
    TEST_ASSERT_EQUAL_STRING("By automation", w.body);

    /* NOTE-002: the longest zone name leaves the line whole. */
    memset(cfg.zones[0].name, 'W', ALARM_NAME_LEN - 1u);
    cfg.zones[0].name[ALARM_NAME_LEN - 1u] = '\0';
    ev.zone = 1u;
    ev.arg = ALARM_AUTO_FAIL_MISSING;
    ev.arg2 = ALARM_AUTO_ACT_CLOSE;
    words_of(&ev, &w);
    TEST_ASSERT_TRUE(strlen(w.body) < NOTIFY_BODY_LEN - 1u);
    TEST_ASSERT_NOT_NULL(strstr(w.body, "isn't on the panel now \xC2\xB7 By automation"));

    /* NOTE-037: on by default, as a trouble */
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_TROUBLE, notify_category(ALARM_EV_AUTOMATION_FAILED));
    notify_gate_t gate;
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, 1000u));
}

/* OTA-010, docs/features/11 "Updates through the link" (What people see, the event log) and
 * proposal P2's Activity: a keypad's update reads on its master by the keypad's number (P2 decision
 * 6), a failure with its why in the words of the master's Keypads rows, a key accepted at a keypad
 * in amber as a server change (decision 8); a why this firmware doesn't know reads by its decade
 * (LINK-039). The panel's own stages read as before. */
static void test_keypad_update_words(void)
{
    notify_words_t w;
    alarm_event_t ev = ev_of(ALARM_EV_FIRMWARE_UPDATE);
    ev.partition = 0u;
    ev.source.kind = ALARM_SRC_KEYPAD;
    ev.source.id = 3u;
    static const struct {
        uint8_t stage;
        uint8_t why;
        const char *title;
        const char *body;
        notify_accent_t accent;
    } LINES[] = {
        {ALARM_FW_KEYPAD_INSTALLING, 0u, "Keypad 3 installing an update",
         "It restarts with the new version", NOTIFY_ACCENT_BLUE},
        {ALARM_FW_KEYPAD_UPDATED, 0u, "Keypad 3 updated", "The new version passed its self-test",
         NOTIFY_ACCENT_BLUE},
        {ALARM_FW_KEYPAD_ROLLED_BACK, 0u, "Keypad 3 update rolled back",
         "The new version failed; the previous one runs", NOTIFY_ACCENT_AMBER},
        {ALARM_FW_KEYPAD_FAILED, 24u, "Keypad 3 update failed", "No data for a minute",
         NOTIFY_ACCENT_AMBER},
        {ALARM_FW_KEYPAD_KEY_ACCEPTED, 0u, "Keypad 3 accepted a new signing key",
         "It takes updates signed with that key now", NOTIFY_ACCENT_AMBER},
    };
    for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
        ev.arg = LINES[i].stage;
        ev.arg2 = LINES[i].why;
        words_of(&ev, &w);
        TEST_ASSERT_EQUAL_STRING(LINES[i].title, w.title);
        TEST_ASSERT_EQUAL_STRING(LINES[i].body, w.body);
        TEST_ASSERT_EQUAL_UINT8(LINES[i].accent, w.accent);
        TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_REFRESH, w.icon);
        TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_SYSTEM, w.cat);
    }

    /* every why, in the master's rows' words; others by their decade; 70 and up, no line */
    static const struct {
        uint8_t why;
        const char *line;
    } WHYS[] = {
        {1u, "It can't reach the update server"},
        {5u, "It can't reach the update server"},
        {9u, "It can't reach the update server"},
        {10u, "The update server has nothing for it"},
        {11u, "The update server holds it back for now"},
        {12u, "It needs another version first"},
        {13u, "The update server won't offer it"},
        {15u, "The update server has no release of this version for it"},
        {16u, "The update server doesn't offer this version"},
        {17u, "The update server won't offer it"},
        {20u, "The download broke off"},
        {21u, "The download was too slow"},
        {22u, "It got the wrong size"},
        {23u, "It arrived damaged"},
        {24u, "No data for a minute"},
        {25u, "It waited too long"},
        {26u, "Not enough memory"},
        {27u, "It couldn't store it"},
        {29u, "The transfer failed"},
        {30u, "The new signing key must be accepted at the keypad"},
        {31u, "It isn't for this keypad's chip"},
        {32u, "It isn't from a project it accepts"},
        {33u, "It isn't newer than the version it runs"},
        {34u, "The signature check failed"},
        {35u, "It's too big for this keypad"},
        {36u, "It can't take this kind of update"},
        {37u, "It doesn't know this order"},
        {38u, "3 tries failed, next try tomorrow"},
        {39u, "The keypad refused it"},
        {40u, "It refused the order"},
        {41u, "It got another version"},
        {49u, "The keypad refused it"},
        {50u, "The system is busy"},
        {51u, "It waits until everything is quiet"},
        {52u, "A panic is being sent from it"},
        {53u, "Its fire or CO alarm still sounds"},
        {54u, "Its link is down"},
        {55u, "Another update runs on it"},
        {59u, "It waits"},
        {60u, "Stopped at the keypad"},
        {61u, "Stopped"},
        {69u, "Stopped"},
        {70u, ""},
        {255u, ""},
    };
    ev.arg = ALARM_FW_KEYPAD_FAILED;
    ev.source.id = (uint16_t)ALARM_MAX_DEVICES; /* the longest number */
    for (size_t i = 0; i < sizeof(WHYS) / sizeof(WHYS[0]); i++) {
        ev.arg2 = WHYS[i].why;
        words_of(&ev, &w);
        TEST_ASSERT_EQUAL_STRING("Keypad 16 update failed", w.title);
        TEST_ASSERT_EQUAL_STRING(WHYS[i].line, w.body);
        TEST_ASSERT_TRUE(strlen(w.body) < NOTIFY_BODY_LEN - 1u);
    }
    for (uint8_t s = ALARM_FW_KEYPAD_INSTALLING; s <= ALARM_FW_KEYPAD_KEY_ACCEPTED; s++) {
        ev.arg = s;
        ev.arg2 = (uint8_t)(s == ALARM_FW_KEYPAD_FAILED ? 15u : 0u);
        words_of(&ev, &w);
        TEST_ASSERT_TRUE(strlen(w.title) < NOTIFY_TITLE_LEN - 1u);
        TEST_ASSERT_NOT_NULL(strstr(w.title, "Keypad 16 "));
    }

    /* this panel's own stages keep their words, whatever the source; a stage this firmware
     * doesn't know (a newer writer's record) reads as an update */
    ev.arg = ALARM_FW_UPDATED;
    ev.arg2 = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Firmware updated", w.title);
    TEST_ASSERT_EQUAL_STRING("The new version passed its self-test", w.body);
    ev.arg = ALARM_FW_RESTART_PUT_OFF;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Update put off", w.title);
    TEST_ASSERT_EQUAL_STRING("It installs once everything is quiet", w.body);
    ev.arg = (uint8_t)(ALARM_FW_KEYPAD_KEY_ACCEPTED + 1u);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Firmware updated", w.title);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ACCENT_BLUE, w.accent);
}

/* NOTE-005: the buckets the event log filters by and the ones a notification is switched on by
 * are the same, and anything new falls to the system bucket, which is off by default. */
static void test_categories(void)
{
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_ALARM, notify_category(ALARM_EV_ALARM));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_ALARM, notify_category(ALARM_EV_DURESS));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_ALARM, notify_category(ALARM_EV_EXIT_ERROR));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_TROUBLE, notify_category(ALARM_EV_TROUBLE));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_TROUBLE, notify_category(ALARM_EV_TROUBLE_RESTORE));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_ARMING, notify_category(ALARM_EV_ARMED));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_ARMING, notify_category(ALARM_EV_DISARMED));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_BYPASS, notify_category(ALARM_EV_BYPASS));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_SYSTEM, notify_category(ALARM_EV_SYSTEM_START));
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_SYSTEM, notify_category(ALARM_EV_PROGRAM_ENTER));
    /* A type this firmware has never heard of. */
    TEST_ASSERT_EQUAL_INT(NOTIFY_CAT_SYSTEM, notify_category((alarm_event_type_t)200));
}

/* ---- the gate: what actually reaches a phone (NOTE-030..038) ---- */

static notify_gate_t gate;

static alarm_event_t alarm_in(uint16_t zone)
{
    alarm_event_t ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.zone = zone;
    return ev;
}

static alarm_event_t trouble_in(uint8_t family, uint8_t kind, uint16_t zone, bool restore)
{
    alarm_event_t ev = ev_of(restore ? ALARM_EV_TROUBLE_RESTORE : ALARM_EV_TROUBLE);
    ev.arg2 = family;
    ev.arg = kind;
    ev.zone = zone;
    return ev;
}

/* NOTE-030: a burst of ten, then one every 30 s. */
static void test_bucket(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t ev = ev_of(ALARM_EV_ARMED);
    ev.user = 1u;
    for (unsigned i = 0; i < NOTIFY_BUCKET_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, 1000u));
    }
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &ev, 1000u));
    /* ... and one more once the bucket has refilled. */
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND,
                          notify_gate_offer(&gate, &ev, 1000u + NOTIFY_BUCKET_REFILL_MS));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP,
                          notify_gate_offer(&gate, &ev, 1000u + NOTIFY_BUCKET_REFILL_MS));
}

/* NOTE-032: an alarm is never held back by a bucket a quiet evening emptied. */
static void test_alarms_bypass_the_bucket(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t armed = ev_of(ALARM_EV_ARMED);
    armed.user = 1u;
    for (unsigned i = 0; i < NOTIFY_BUCKET_MAX; i++) {
        (void)notify_gate_offer(&gate, &armed, 1000u);
    }
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &armed, 1000u));

    alarm_event_t ev = alarm_in(1u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, 1000u));
}

/* NOTE-033: eight zones tripping in one burglary is two notifications, not eight. */
static void test_eight_zones_are_two_notifications(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    uint64_t now = 100000u;

    alarm_event_t first = alarm_in(1u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &first, now));

    for (uint16_t z = 2u; z <= 8u; z++) {
        alarm_event_t more = alarm_in(z);
        now += 1000u;
        TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_HOLD, notify_gate_offer(&gate, &more, now));
    }

    notify_rollup_t r;
    memset(&r, 0, sizeof(r));
    TEST_ASSERT_FALSE(notify_gate_tick(&gate, now, &r)); /* the minute is not up */
    TEST_ASSERT_TRUE(notify_gate_tick(&gate, 100000u + NOTIFY_ROLLUP_MS, &r));
    TEST_ASSERT_EQUAL_UINT16(8u, r.total);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ROLLUP_ZONES, r.n); /* three named, the rest counted */
    TEST_ASSERT_EQUAL_UINT16(1u, r.zones[0]);
    TEST_ASSERT_EQUAL_UINT8(ALARM_CAUSE_BURGLARY, r.cause);
    /* ... and only once. */
    TEST_ASSERT_FALSE(notify_gate_tick(&gate, 100000u + NOTIFY_ROLLUP_MS, &r));
}

/* One zone, one alert: a single-zone alarm never produces a pointless follow-up. */
static void test_one_zone_has_no_follow_up(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t ev = alarm_in(1u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, 100000u));

    notify_rollup_t r;
    TEST_ASSERT_FALSE(notify_gate_tick(&gate, 100000u + NOTIFY_ROLLUP_MS, &r));

    /* The same zone faulting again inside the window is still one zone, and a zone the owner
     * has already been told about is not worth a second message. */
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    (void)notify_gate_offer(&gate, &ev, 100000u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_HOLD, notify_gate_offer(&gate, &ev, 101000u));
    TEST_ASSERT_FALSE(notify_gate_tick(&gate, 100000u + NOTIFY_ROLLUP_MS, &r));

    /* A different zone, though, is news: one follow-up naming both. */
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    (void)notify_gate_offer(&gate, &ev, 100000u);
    alarm_event_t other = alarm_in(5u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_HOLD, notify_gate_offer(&gate, &other, 101000u));
    TEST_ASSERT_TRUE(notify_gate_tick(&gate, 100000u + NOTIFY_ROLLUP_MS, &r));
    TEST_ASSERT_EQUAL_UINT16(2u, r.total);
    TEST_ASSERT_EQUAL_UINT16(5u, r.zones[1]);
}

/* A new alarm after the window has closed starts its own conversation. */
static void test_alarm_after_the_window(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t ev = alarm_in(1u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, 100000u));
    const uint64_t later = 100000u + NOTIFY_ROLLUP_MS + 1000u;
    alarm_event_t ev2 = alarm_in(2u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev2, later));
}

/* NOTE-034: a flapping sensor says it once an hour, and "all clear" only follows a report. */
static void test_trouble_collapse(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t t1 = trouble_in(ALARM_TRBL_FAMILY_ZONE, ALARM_ZTRBL_LOW_BATTERY, 1u, false);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &t1, 1000u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &t1, 2000u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP,
                          notify_gate_offer(&gate, &t1, 1000u + NOTIFY_TRBL_COLLAPSE_MS - 1u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND,
                          notify_gate_offer(&gate, &t1, 1000u + NOTIFY_TRBL_COLLAPSE_MS));

    /* The same trouble on a different zone is a different thing. */
    alarm_event_t t2 = trouble_in(ALARM_TRBL_FAMILY_ZONE, ALARM_ZTRBL_LOW_BATTERY, 2u, false);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &t2, 2000u));
}

static void test_trouble_restore_needs_its_trouble(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t restore = trouble_in(ALARM_TRBL_FAMILY_SYSTEM, ALARM_TRBL_AC_LOSS, 0u, true);
    /* Nobody was told the power went: nobody needs telling it came back. */
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &restore, 1000u));

    alarm_event_t lost = trouble_in(ALARM_TRBL_FAMILY_SYSTEM, ALARM_TRBL_AC_LOSS, 0u, false);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &lost, 2000u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &restore, 3000u));
    /* ... and the restore clears it, so the next loss is news again. */
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &lost, 4000u));
}

/* NOTE-031: past the soft cap only alarms get out; past the hard cap the page says it stopped. */
static void test_daily_cap(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t ev = alarm_in(1u);
    uint64_t now = 1000u;
    for (unsigned i = 0; i < NOTIFY_DAY_SOFT; i++) {
        now += NOTIFY_ROLLUP_MS + 1000u; /* each its own alarm */
        TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, now));
    }
    alarm_event_t armed = ev_of(ALARM_EV_ARMED);
    armed.user = 1u;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &armed, now));
    TEST_ASSERT_FALSE(notify_gate_paused(&gate));

    for (unsigned i = NOTIFY_DAY_SOFT; i < NOTIFY_DAY_HARD; i++) {
        now += NOTIFY_ROLLUP_MS + 1000u;
        TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, now));
    }
    now += NOTIFY_ROLLUP_MS + 1000u;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &ev, now));
    TEST_ASSERT_TRUE(notify_gate_paused(&gate));

    /* A new day starts again. */
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &ev, now + NOTIFY_DAY_MS));
    TEST_ASSERT_FALSE(notify_gate_paused(&gate));
}

/* NOTE-037: what the owner switched off costs nothing - not a token, not a slot. */
static void test_categories_filter_first(void)
{
    notify_gate_init(&gate, NOTIFY_CAT_BIT(NOTIFY_CAT_ALARM), false, 0u);
    alarm_event_t armed = ev_of(ALARM_EV_ARMED);
    armed.user = 1u;
    for (unsigned i = 0; i < 50u; i++) {
        TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &armed, 1000u));
    }
    /* The bucket was never touched, so a trouble would still get through if it were wanted. */
    notify_gate_set(&gate, NOTIFY_CATS_DEFAULT, false);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &armed, 1000u));
}

/* NOTE-038: a silent alarm leaves the building only when the owner asked for that, because a
 * push lands on a lock screen someone else may be looking at. */
static void test_silent_is_opt_in(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t duress = ev_of(ALARM_EV_DURESS);
    duress.flags = ALARM_EVF_SILENT;
    duress.user = 1u;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &duress, 1000u));

    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, true, 0u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&gate, &duress, 1000u));
}

/* A clock that steps backwards (SNTP correcting a drifting panel) must not wedge the bucket. */
static void test_clock_stepping_back(void)
{
    notify_gate_init(&gate, NOTIFY_CATS_DEFAULT, false, 1000000u);
    alarm_event_t armed = ev_of(ALARM_EV_ARMED);
    armed.user = 1u;
    for (unsigned i = 0; i < NOTIFY_BUCKET_MAX; i++) {
        (void)notify_gate_offer(&gate, &armed, 1000000u);
    }
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&gate, &armed, 1000u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND,
                          notify_gate_offer(&gate, &armed, 1000u + NOTIFY_BUCKET_REFILL_MS));
}

/* ---- the requests each service wants (NTFY-001, NTFY-009, PUSH-001, PUSH-005, PUSH-009) ---- */

static notify_words_t words_for(alarm_event_type_t type, uint8_t cause, uint16_t zone)
{
    alarm_event_t ev = ev_of(type);
    ev.arg = cause;
    ev.zone = zone;
    notify_words_t w;
    words_of(&ev, &w);
    return w;
}

/* NTFY-001: a publish is JSON to the base URL, so a title with a "·" in it needs no header
 * encoding. The body must be valid JSON and carry the UTF-8 through untouched. */
static void test_ntfy_request(void)
{
    const notify_ntfy_cfg_t hosted = {"https://ntfy.sh", "roboalarms-a6efk9p7", NULL};
    notify_words_t w = words_for(ALARM_EV_ALARM, ALARM_CAUSE_BURGLARY, 1u);
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_ntfy(&hosted, &w, NOTIFY_PRIO_URGENT, &r));
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/", r.url);
    TEST_ASSERT_EQUAL_STRING("application/json", r.content_type);
    TEST_ASSERT_EQUAL_STRING("", r.auth);
    TEST_ASSERT_EQUAL_STRING(
        "{\"topic\":\"roboalarms-a6efk9p7\",\"title\":\"Burglary alarm\","
        "\"message\":\"Front Door\",\"priority\":5,\"tags\":[\"rotating_light\"]}",
        r.body);
    TEST_ASSERT_EQUAL_UINT(strlen(r.body), r.body_len);

    /* A self-hosted server with a token. */
    const notify_ntfy_cfg_t own = {"https://ntfy.example.com", "house", "tk_abc123"};
    TEST_ASSERT_TRUE(notify_build_ntfy(&own, &w, NOTIFY_PRIO_URGENT, &r));
    TEST_ASSERT_EQUAL_STRING("https://ntfy.example.com/", r.url);
    TEST_ASSERT_EQUAL_STRING("Bearer tk_abc123", r.auth);

    /* Nothing to publish to. */
    const notify_ntfy_cfg_t none = {"https://ntfy.sh", "", NULL};
    TEST_ASSERT_FALSE(notify_build_ntfy(&none, &w, NOTIFY_PRIO_URGENT, &r));
}

/* The middle dot in a trouble title is exactly what would have needed RFC 2047 in a header. */
static void test_ntfy_utf8_and_quotes(void)
{
    snprintf(cfg.zones[2].name, sizeof(cfg.zones[2].name), "Dad's \"Office\"");
    alarm_event_t ev = ev_of(ALARM_EV_TROUBLE);
    ev.arg2 = ALARM_TRBL_FAMILY_ZONE;
    ev.arg = ALARM_ZTRBL_TAMPER;
    ev.zone = 3u;
    notify_words_t w;
    words_of(&ev, &w);

    const notify_ntfy_cfg_t c = {"https://ntfy.sh", "t", NULL};
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_ntfy(&c, &w, NOTIFY_PRIO_HIGH, &r));
    /* The quotes are escaped, the apostrophe is left alone and the "·" survives as UTF-8. */
    TEST_ASSERT_NOT_NULL(strstr(r.body, "Dad's \\\"Office\\\" \xC2\xB7 Tamper"));
    TEST_ASSERT_NULL(strstr(r.body, "\"Office\"\","));
}

/* NTFY-009 / PUSH-009: an alarm is loudest, and the messages saying it is over are not. */
static void test_priorities(void)
{
    alarm_event_t ev = ev_of(ALARM_EV_ALARM);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_URGENT, notify_priority(&ev));
    ev = ev_of(ALARM_EV_DURESS);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_URGENT, notify_priority(&ev));
    ev = ev_of(ALARM_EV_ALARM_CANCELED);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_HIGH, notify_priority(&ev));
    ev = ev_of(ALARM_EV_TROUBLE);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_HIGH, notify_priority(&ev));
    ev = ev_of(ALARM_EV_ARMED);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_NORMAL, notify_priority(&ev));
    ev = ev_of(ALARM_EV_SYSTEM_START);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_LOW, notify_priority(&ev));
}

/* PUSH-001: a form POST, with everything that isn't unreserved percent-encoded. */
static void test_pushover_request(void)
{
    const notify_pushover_cfg_t c = {"uQiRzpo4DXghDmr9QzzfQu27cmVRsG",
                                     "azGDORePK8gMaC0QOYAMyEEuzJnyUi",
                                     "siren", NULL, true, 60u, 1800u};
    notify_words_t w = words_for(ALARM_EV_ALARM, ALARM_CAUSE_BURGLARY, 1u);
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_pushover(&c, &w, NOTIFY_PRIO_URGENT, "alarm-1", &r));
    TEST_ASSERT_EQUAL_STRING("https://api.pushover.net/1/messages.json", r.url);
    TEST_ASSERT_EQUAL_STRING("application/x-www-form-urlencoded", r.content_type);
    TEST_ASSERT_NOT_NULL(strstr(r.body, "token=azGDORePK8gMaC0QOYAMyEEuzJnyUi"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "user=uQiRzpo4DXghDmr9QzzfQu27cmVRsG"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "title=Burglary%20alarm"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "message=Front%20Door"));
    /* PUSH-004: an alarm repeats, and Pushover refuses priority 2 without both of these. */
    TEST_ASSERT_NOT_NULL(strstr(r.body, "priority=2"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "retry=60"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "expire=1800"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "tag=alarm-1"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "sound=siren"));

    /* With the repeat switched off an alarm is still loud, but it doesn't keep going. */
    notify_pushover_cfg_t quiet = c;
    quiet.emergency = false;
    TEST_ASSERT_TRUE(notify_build_pushover(&quiet, &w, NOTIFY_PRIO_URGENT, "alarm-1", &r));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "priority=1"));
    TEST_ASSERT_NULL(strstr(r.body, "retry="));
    TEST_ASSERT_NULL(strstr(r.body, "tag="));

    /* Arming is ordinary, and doesn't take the alarm sound. */
    notify_words_t armed = words_for(ALARM_EV_ARMED, ALARM_LEVEL_AWAY, 0u);
    TEST_ASSERT_TRUE(notify_build_pushover(&c, &armed, NOTIFY_PRIO_NORMAL, NULL, &r));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "priority=0"));
    TEST_ASSERT_NULL(strstr(r.body, "sound="));

    /* Both credentials are required. */
    const notify_pushover_cfg_t half = {"uQiRzpo4DXghDmr9QzzfQu27cmVRsG", "", NULL, NULL,
                                        false, 0u, 0u};
    TEST_ASSERT_FALSE(notify_build_pushover(&half, &w, NOTIFY_PRIO_URGENT, NULL, &r));
}

/* A name with an ampersand in it must not turn into two form fields. */
static void test_pushover_escaping(void)
{
    snprintf(cfg.zones[1].name, sizeof(cfg.zones[1].name), "Bed&Bath=1");
    const notify_pushover_cfg_t c = {"u", "a", NULL, NULL, false, 0u, 0u};
    notify_words_t w = words_for(ALARM_EV_ALARM, ALARM_CAUSE_BURGLARY, 2u);
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_pushover(&c, &w, NOTIFY_PRIO_HIGH, NULL, &r));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "message=Bed%26Bath%3D1"));
    /* Exactly the fields we put there, and no more. */
    unsigned amps = 0u;
    for (const char *p = r.body; *p != '\0'; p++) {
        if (*p == '&') {
            amps++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(4u, amps); /* token, user, title, message, priority */
}

/* PUSH-005: the alarm is over, so the phones must stop repeating. */
static void test_pushover_cancel(void)
{
    const notify_pushover_cfg_t c = {"u", "azGDORePK8gMaC0QOYAMyEEuzJnyUi", NULL, NULL, true,
                                     60u, 1800u};
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_pushover_cancel(&c, "alarm-1", &r));
    TEST_ASSERT_EQUAL_STRING("https://api.pushover.net/1/receipts/cancel_by_tag/alarm-1.json",
                             r.url);
    TEST_ASSERT_NOT_NULL(strstr(r.body, "token=azGDORePK8gMaC0QOYAMyEEuzJnyUi"));
    TEST_ASSERT_FALSE(notify_build_pushover_cancel(&c, "", &r));
}

/* A very long zone name must be cut without splitting a UTF-8 character in half. */
static void test_long_names_are_cut_cleanly(void)
{
    /* Twenty-one "e acute" characters: 42 bytes, so any byte-wise cut lands mid-character. */
    char name[ALARM_NAME_LEN];
    size_t n = 0u;
    while (n + 2u < sizeof(name) - 1u) {
        name[n++] = (char)0xC3;
        name[n++] = (char)0xA9;
    }
    name[n] = '\0';
    snprintf(cfg.zones[0].name, sizeof(cfg.zones[0].name), "%s", name);

    notify_words_t w = words_for(ALARM_EV_ALARM, ALARM_CAUSE_BURGLARY, 1u);
    const notify_ntfy_cfg_t c = {"https://ntfy.sh", "t", NULL};
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_ntfy(&c, &w, NOTIFY_PRIO_URGENT, &r));
    /* Every byte after a lead byte must be a continuation byte: no half characters. */
    for (const char *p = r.body; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        if ((ch & 0xE0u) == 0xC0u) {
            TEST_ASSERT_TRUE_MESSAGE(((unsigned char)p[1] & 0xC0u) == 0x80u,
                                     "a UTF-8 character was cut in half");
            p++;
        }
    }
}

/* I18N-006, I18N-030: a catalog changes the sentences and nothing else. The household's own
 * names go in unchanged, a sentence the catalog lacks stays English, and no catalog is English. */
typedef struct {
    i18n_entry_t entries[8];
    char strings[512];
    uint32_t count;
    uint32_t used;
} es_table_t;

static void es_add(es_table_t *t, const char *ctx, const char *english, const char *spanish)
{
    i18n_entry_t e;
    e.hash = i18n_hash(ctx, english);
    e.text = t->used;
    memcpy(t->strings + t->used, spanish, strlen(spanish) + 1u);
    t->used += (uint32_t)strlen(spanish) + 1u;
    uint32_t at = t->count++;
    while (at > 0u && t->entries[at - 1u].hash > e.hash) { /* entries stay sorted by hash */
        t->entries[at] = t->entries[at - 1u];
        at--;
    }
    t->entries[at] = e;
}

static void test_a_catalog_translates_the_sentences(void)
{
    static es_table_t t;
    memset(&t, 0, sizeof(t));
    es_add(&t, NULL, "Burglary alarm", "Alarma de robo");
    es_add(&t, NULL, "{0} bypassed", "{0} omitida");
    es_add(&t, NULL, "By {0}", "Por {0}");
    es_add(&t, "default zone name", "Zone {0}", "Zona {0}");
    es_add(&t, "zone trouble", "Tamper", "Manipulacion");
    es_add(&t, NULL, "Tamper", "WRONG: no context"); /* a NULL-context "Tamper" must not be used */
    es_add(&t, NULL, "Mains power lost", "Sin corriente"); /* an untagged table's title */
    const i18n_catalog_t es = {"es", (uint8_t)I18N_PLURALS_ES, 0, t.count, t.entries, t.strings};

    notify_words_t tw;
    notify_zone_trouble_words(&es, &cfg, 1u, ALARM_ZTRBL_TAMPER, false, &tw);
    TEST_ASSERT_NOT_NULL(strstr(tw.title, "Manipulacion")); /* found with its context */
    TEST_ASSERT_NULL(strstr(tw.title, "WRONG"));
    notify_sys_trouble_words(&es, ALARM_TRBL_AC_LOSS, false, &tw);
    TEST_ASSERT_EQUAL_STRING("Sin corriente", tw.title); /* found without one */

    TEST_ASSERT_EQUAL_STRING("Alarma de robo", notify_cause_title(&es, ALARM_CAUSE_BURGLARY));
    TEST_ASSERT_EQUAL_STRING("Burglary alarm", notify_cause_title(NULL, ALARM_CAUSE_BURGLARY));
    TEST_ASSERT_EQUAL_STRING("Fire alarm", notify_cause_title(&es, ALARM_CAUSE_FIRE)); /* absent */

    alarm_event_t ev = ev_of(ALARM_EV_BYPASS);
    ev.zone = 1u;
    ev.user = 1u;
    notify_words_t w;
    TEST_ASSERT_TRUE(notify_words(&es, &cfg, &ev, true, &w));
    TEST_ASSERT_EQUAL_STRING("Front Door omitida", w.title); /* the zone's name is not translated */
    TEST_ASSERT_EQUAL_STRING("Por Master", w.body);

    ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.zone = 2u; /* nobody named it */
    TEST_ASSERT_TRUE(notify_words(&es, &cfg, &ev, true, &w));
    TEST_ASSERT_EQUAL_STRING("Alarma de robo", w.title);
    TEST_ASSERT_EQUAL_STRING("Zona 2", w.body);
    TEST_ASSERT_TRUE(notify_words(NULL, &cfg, &ev, true, &w));
    TEST_ASSERT_EQUAL_STRING("Zone 2", w.body);
}

/* I18N-006, I18N-030 with the committed Spanish catalog (es-US, docs/features/37 G4): every event
 * has Spanish words that end on a whole character, every alarm's title is Spanish, and both
 * services carry them to a phone - ntfy's JSON as UTF-8, Pushover's form percent-encoded. */
static void test_spanish_reaches_a_phone(void)
{
    const i18n_catalog_t *es = i18n_catalog("es-US");
    TEST_ASSERT_NOT_NULL(es);
    for (unsigned t = 0; t < (unsigned)ALARM_EV_COUNT; t++) {
        alarm_event_t ev = ev_of((alarm_event_type_t)t);
        ev.zone = 1u;
        ev.user = 1u;
        notify_words_t w;
        if (!notify_words(es, &cfg, &ev, true, &w)) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT(strlen(w.title), i18n_utf8_fit(w.title, strlen(w.title)));
        TEST_ASSERT_EQUAL_UINT(strlen(w.body), i18n_utf8_fit(w.body, strlen(w.body)));
    }
    for (unsigned c = 1; c < (unsigned)ALARM_CAUSE_COUNT; c++) {
        TEST_ASSERT_TRUE_MESSAGE(strcmp(notify_cause_title(es, (alarm_cause_t)c),
                                        notify_cause_title(NULL, (alarm_cause_t)c)) != 0,
                                 "an alarm's title stayed English");
    }

    alarm_event_t ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_FIRE;
    ev.zone = 2u; /* nobody named it */
    notify_words_t w;
    TEST_ASSERT_TRUE(notify_words(es, &cfg, &ev, true, &w));
    const notify_ntfy_cfg_t hosted = {"https://ntfy.sh", "roboalarms-a6efk9p7", NULL};
    notify_request_t r;
    TEST_ASSERT_TRUE(notify_build_ntfy(&hosted, &w, NOTIFY_PRIO_URGENT, &r));
    TEST_ASSERT_EQUAL_STRING("{\"topic\":\"roboalarms-a6efk9p7\",\"title\":\"Alarma de incendio\","
                             "\"message\":\"Zona 2\",\"priority\":5,\"tags\":[\"rotating_light\"]}",
                             r.body);

    notify_words_t tw;
    notify_sys_trouble_words(es, ALARM_TRBL_AC_LOSS, false, &tw);
    TEST_ASSERT_TRUE(notify_build_ntfy(&hosted, &tw, NOTIFY_PRIO_HIGH, &r));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "\"title\":\"Sin corriente el\xC3\xA9" "ctrica\""));
    const notify_pushover_cfg_t po = {"uQiRzpo4DXghDmr9QzzfQu27cmVRsG",
                                      "azGDORePK8gMaC0QOYAMyEEuzJnyUi", NULL, NULL, false, 0u, 0u};
    TEST_ASSERT_TRUE(notify_build_pushover(&po, &tw, NOTIFY_PRIO_HIGH, NULL, &r));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "title=Sin%20corriente%20el%C3%A9ctrica"));

    /* AUT-009: "By automation" is translated too. */
    ev = ev_of(ALARM_EV_DISARMED);
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.user = 1u;
    TEST_ASSERT_TRUE(notify_words(es, &cfg, &ev, true, &w));
    TEST_ASSERT_EQUAL_STRING("Por automatizaci\xC3\xB3n", w.body);
}

/* ---- docs/features/41 D30: who did it ------------------------------------------------- */

/* The PC's fake automations: uid 7 "At 10:00 AM on weekdays", edited at FAKE_CHANGED_AT, made
 * by user 2; every other uid is gone. */
#define FAKE_CHANGED_AT 1790000000u
static unsigned s_lookups;

static bool fake_lookup(void *ctx, const i18n_catalog_t *c, const alarm_config_t *conf,
                        uint16_t uid, notify_auto_info_t *out)
{
    (void)ctx;
    s_lookups++;
    if (uid != 7u) {
        return false;
    }
    auto_trigger_t w;
    memset(&w, 0, sizeof(w));
    w.kind = AUTO_TRIG_TIME;
    w.minute = 600u;
    w.days = 0x1Fu;
    notify_auto_when(c, conf, NULL, false, &w, out->clause, sizeof(out->clause));
    snprintf(out->sentence, sizeof(out->sentence), "%s, disarm", out->clause);
    out->changed_at = FAKE_CHANGED_AT;
    out->maker = 2u;
    return true;
}

/* UTC is the PC test's local time. */
static void fake_time(void *ctx, const i18n_catalog_t *c, int64_t epoch_ms, char *out, size_t len)
{
    (void)ctx;
    const unsigned m = (unsigned)((epoch_ms / 60000) % 1440);
    notify_clock_text(c, false, m / 60u, m % 60u, out, len);
}

static void use_fake_hooks(bool with_time)
{
    const notify_hooks_t h = {fake_lookup, with_time ? fake_time : NULL, NULL};
    notify_set_hooks(&h);
}

static alarm_event_t auto_ev(alarm_event_type_t type, uint16_t uid)
{
    alarm_event_t ev = ev_of(type);
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = uid;
    ev.user = 2u; /* the maker it acted as: never named as the one who did it */
    ev.epoch_ms = ((int64_t)FAKE_CHANGED_AT + 3600) * 1000; /* after the last edit */
    return ev;
}

/* NOTE-041, AUT-009, D30: Activity's rows for what an automation did (screen 32 activity-automation). */
static void test_automation_named_in_words(void)
{
    notify_words_t w;
    use_fake_hooks(true);
    alarm_event_t ev = auto_ev(ALARM_EV_DISARMED, 7u);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Disarmed", w.title);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 At 10:00 AM on weekdays", w.body);

    ev = auto_ev(ALARM_EV_ARMED, 7u);
    ev.arg = ALARM_LEVEL_NIGHT;
    ev.arg2 = (uint8_t)(1u << ALARM_ARMED_BYPASSED_SHIFT);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Armed Night", w.title);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 1 zone bypassed", w.body);
    ev.arg2 = (uint8_t)((3u << ALARM_ARMED_BYPASSED_SHIFT) | ALARM_ARM_NO_ENTRY_DELAY);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 3 zones bypassed", w.body);
    ev.arg2 = 0u; /* nothing bypassed: which one */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 At 10:00 AM on weekdays", w.body);

    ev = auto_ev(ALARM_EV_BYPASS, 7u);
    ev.zone = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Front Door bypassed", w.title);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 At 10:00 AM on weekdays", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_BYPASS, w.cat);

    ev = auto_ev(ALARM_EV_UNBYPASS, 7u);
    ev.zone = 1u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Front Door watched again", w.title);
    TEST_ASSERT_EQUAL_STRING("Closed after arming \xC2\xB7 By automation", w.body);

    /* ARM_FAILED names the open zone, then who (CORE-USR-030). */
    ev = auto_ev(ALARM_EV_ARM_FAILED, 7u);
    ev.zone = 1u;
    ev.arg = ALARM_ERR_NOT_READY;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Arming failed", w.title);
    TEST_ASSERT_EQUAL_STRING("Front Door \xC2\xB7 By automation", w.body);
    ev.zone = 0u; /* no zone: the result */
    ev.arg = ALARM_ERR_NOT_AUTHORIZED;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("This code can't do that \xC2\xB7 By automation", w.body);
    ev.source.kind = ALARM_SRC_KEYPAD; /* a person's: as before */
    ev.zone = 1u;
    ev.arg = ALARM_ERR_NOT_READY;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Not ready: a zone is open", w.body);

    /* A person's row beside them is unchanged. */
    ev = ev_of(ALARM_EV_DISARMED);
    ev.user = 1u;
    ev.source.kind = ALARM_SRC_KEYPAD;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Master", w.body);
}

/* AUT-009, D30: "By a deleted automation" once the uid is gone; "changed since" for a line older
 * than the last edit; "By automation" alone without a hook, or for uid 0 (an arming from before
 * a restart), and never the maker's name. */
static void test_automation_gone_changed_or_unknown(void)
{
    notify_words_t w;
    use_fake_hooks(false);
    alarm_event_t ev = auto_ev(ALARM_EV_DISARMED, 9u);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By a deleted automation", w.body);
    ev = auto_ev(ALARM_EV_ARMED, 9u);
    ev.arg = ALARM_LEVEL_AWAY;
    ev.arg2 = (uint8_t)(2u << ALARM_ARMED_BYPASSED_SHIFT);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By a deleted automation \xC2\xB7 2 zones bypassed", w.body);

    ev = auto_ev(ALARM_EV_DISARMED, 7u);
    ev.epoch_ms = ((int64_t)FAKE_CHANGED_AT - 1) * 1000; /* the second before the edit */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 changed since", w.body);
    ev.epoch_ms = (int64_t)FAKE_CHANGED_AT * 1000; /* the edit's own second: still its words */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 At 10:00 AM on weekdays", w.body);
    ev.epoch_ms = 0; /* no clock then: can't tell, the words now */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation \xC2\xB7 At 10:00 AM on weekdays", w.body);

    s_lookups = 0u;
    ev.source.id = 0u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation", w.body);
    TEST_ASSERT_EQUAL_UINT(0u, s_lookups); /* uid 0 is never looked up */

    notify_set_hooks(NULL);
    ev.source.id = 7u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By automation", w.body);
    TEST_ASSERT_NULL(strstr(w.body, "Master"));
    notify_auto_info_t info;
    TEST_ASSERT_EQUAL_INT(NOTIFY_AUTO_UNKNOWN, notify_auto_lookup(NULL, &cfg, 7u, &info));
    TEST_ASSERT_EQUAL_STRING("", info.clause);
    char t[16] = "x";
    TEST_ASSERT_FALSE(notify_time_of_day(NULL, 1, t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("", t);
}

/* AUT-013, D26, D30: a put-off names whose code did it and, for an hour, until when; it is an
 * arming event, never silent. */
static void test_put_off_words(void)
{
    notify_words_t w;
    snprintf(cfg.users[3].name, sizeof(cfg.users[3].name), "Pat");
    use_fake_hooks(true);
    alarm_event_t ev = auto_ev(ALARM_EV_ARMING_PUT_OFF, 7u);
    ev.user = 4u;
    ev.arg = ALARM_PUT_OFF_HOUR;
    /* 10:27 PM UTC, put off to 11:30 PM: 63 minutes on */
    ev.epoch_ms = (1790035200LL /* a midnight UTC */ + 22 * 3600 + 27 * 60 + 13) * 1000;
    ev.arg2 = alarm_put_off_minutes(ev.epoch_ms, (1790035200LL + 23 * 3600 + 30 * 60) * 1000);
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Arming put off an hour", w.title);
    TEST_ASSERT_EQUAL_STRING("By Pat \xC2\xB7 until 11:30 PM", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_ARMING, w.cat);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ICON_CLOCK, w.icon);
    TEST_ASSERT_FALSE(w.silent);
    ev.arg2 = 0u; /* the clock wasn't set */
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Pat", w.body);
    use_fake_hooks(false); /* no time hook */
    ev.arg2 = 63u;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("By Pat", w.body);

    ev.arg = ALARM_PUT_OFF_NIGHT;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Night arming skipped tonight", w.title);
    TEST_ASSERT_EQUAL_STRING("By Pat", w.body);
    ev.arg = ALARM_PUT_OFF_DAY;
    words_of(&ev, &w);
    TEST_ASSERT_EQUAL_STRING("Arming skipped today", w.title);
    TEST_ASSERT_EQUAL_STRING("By Pat", w.body);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_CAT_ARMING, notify_category(ALARM_EV_ARMING_PUT_OFF));
}

/* NOTE-040, AUT-013: only a skip reaches a phone; an hour later is said by what follows. */
static void test_put_off_push_rule(void)
{
    notify_gate_t g;
    notify_gate_init(&g, NOTIFY_CATS_DEFAULT, false, 0u);
    alarm_event_t ev = auto_ev(ALARM_EV_ARMING_PUT_OFF, 7u);
    ev.user = 4u;
    ev.arg = ALARM_PUT_OFF_HOUR;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&g, &ev, 1000u));
    ev.arg = ALARM_PUT_OFF_NIGHT;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&g, &ev, 2000u));
    ev.arg = ALARM_PUT_OFF_DAY;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&g, &ev, 3000u));
    /* the owner's categories still decide: arming off, nothing */
    notify_gate_set(&g, NOTIFY_CAT_BIT(NOTIFY_CAT_ALARM), false);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&g, &ev, 4000u));
}

/* AUT-009, D30: the trigger clause, as the automation's row says it (the portable copy). */
static void test_automation_clause(void)
{
    char out[NOTIFY_AUTO_CLAUSE_LEN];
    auto_trigger_t w;
    memset(&w, 0, sizeof(w));
    w.kind = AUTO_TRIG_TIME;
    w.minute = 22u * 60u + 30u;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("At 10:30 PM every day", out);
    notify_auto_when(NULL, &cfg, NULL, true, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("At 22:30 every day", out);
    w.days = 0x60u;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("At 10:30 PM on weekends", out);
    w.days = (uint8_t)(AUTO_DAY_BIT(1) | AUTO_DAY_BIT(3) | AUTO_DAY_BIT(7)); /* Mon, Wed, Sun */
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("At 10:30 PM on Sun, Mon, Wed", out);
    notify_auto_when(NULL, &cfg, i18n_region("GB"), false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("At 10:30 PM on Mon, Wed, Sun", out);

    memset(&w, 0, sizeof(w));
    w.kind = AUTO_TRIG_ARMED;
    w.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT);
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("When the panel is armed Night", out);
    cfg.partitions[1].enabled = true;
    snprintf(cfg.partitions[1].name, sizeof(cfg.partitions[1].name), "Garage");
    w.partitions = 0x02u;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("When Garage is armed Night", out);
    w.kind = AUTO_TRIG_DISARMED;
    w.partitions = 0x01u;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("When Partition 1 is disarmed", out);

    memset(&w, 0, sizeof(w));
    w.kind = AUTO_TRIG_ZONE_OPENED;
    w.zone = 1u;
    cfg.zones[0].device_class = ALARM_DEVCLASS_DOOR;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("When Front Door opens", out);
    cfg.zones[4].device_class = ALARM_DEVCLASS_MOTION;
    w.zone = 5u;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("When Hallway Motion sees motion", out);
    w.kind = AUTO_TRIG_ZONE_CLOSED;
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("When Hallway Motion closes", out);
    w.zone = ALARM_MAX_ZONES + 1u; /* out of range: never read past the table */
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Something this software can't show", out);
    w.kind = AUTO_TRIG_COUNT; /* a newer kind */
    notify_auto_when(NULL, &cfg, NULL, false, &w, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Something this software can't show", out);
    w.kind = AUTO_TRIG_FIRE;
    char tiny[8];
    notify_auto_when(NULL, &cfg, NULL, false, &w, tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_STRING("When a ", tiny); /* cut to fit, NUL-terminated */
    notify_auto_when(NULL, &cfg, NULL, false, NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_event_type_has_words);
    RUN_TEST(test_golden_wording);
    RUN_TEST(test_automation_named_in_words);
    RUN_TEST(test_automation_gone_changed_or_unknown);
    RUN_TEST(test_put_off_words);
    RUN_TEST(test_put_off_push_rule);
    RUN_TEST(test_automation_clause);
    RUN_TEST(test_automation_failed_words);
    RUN_TEST(test_keypad_update_words);
    RUN_TEST(test_silent_events);
    RUN_TEST(test_a_catalog_translates_the_sentences);
    RUN_TEST(test_spanish_reaches_a_phone);
    RUN_TEST(test_trouble_words);
    RUN_TEST(test_zwave_words);
    RUN_TEST(test_zigbee_words);
    RUN_TEST(test_zone_state_words);
    RUN_TEST(test_categories);
    RUN_TEST(test_bucket);
    RUN_TEST(test_alarms_bypass_the_bucket);
    RUN_TEST(test_eight_zones_are_two_notifications);
    RUN_TEST(test_one_zone_has_no_follow_up);
    RUN_TEST(test_alarm_after_the_window);
    RUN_TEST(test_trouble_collapse);
    RUN_TEST(test_trouble_restore_needs_its_trouble);
    RUN_TEST(test_daily_cap);
    RUN_TEST(test_categories_filter_first);
    RUN_TEST(test_silent_is_opt_in);
    RUN_TEST(test_clock_stepping_back);
    RUN_TEST(test_ntfy_request);
    RUN_TEST(test_ntfy_utf8_and_quotes);
    RUN_TEST(test_priorities);
    RUN_TEST(test_pushover_request);
    RUN_TEST(test_pushover_escaping);
    RUN_TEST(test_pushover_cancel);
    RUN_TEST(test_long_names_are_cut_cleanly);
    return UNITY_END();
}
