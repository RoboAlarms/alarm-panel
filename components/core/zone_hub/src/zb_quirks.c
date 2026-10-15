/*
 * zb_quirks.c - the Zigbee quirks table (docs/features/20 ZB-007): what known brands get wrong,
 * kept as data so a new device is a row and not a patch.
 *
 * Rows are matched in order, manufacturer first then model, both as prefixes, so one row can
 * cover a whole family ("_TZE200_" is every Tuya radio of that generation). An empty or NULL
 * field matches anything. A device that matches nothing is treated as a well-behaved standard
 * device, which is what most of them are.
 *
 * The rows below are seeded from what the Zigbee2MQTT and ZHA converters do for these families;
 * each one is still to be confirmed against a real device on the bench, and the note is what the
 * zone page and the console say about it. Nothing here can make a device more trusted: a quirk
 * only relaxes what we expect of it, renames its class, or says outright that we cannot speak
 * to it (ZSRC_ZB_QUIRK_UNSUPPORTED, which sends the owner to Home Assistant instead).
 */
#include <stddef.h>
#include <string.h>

#include "zone_hub/zsrc_zigbee.h"

static const zsrc_zb_quirk_t QUIRKS[] = {
    /* Tuya's older radios answer on a manufacturer cluster (0xEF00) instead of the standard
     * ones, with a per-model payload. We do not speak it; Home Assistant does (docs/features/23). */
    {"_TZE200_", NULL, ZSRC_ZB_QUIRK_UNSUPPORTED, ALARM_DEVCLASS_GENERIC, 0,
     "Use Home Assistant: it speaks Tuya's protocol"},
    {"_TZE204_", NULL, ZSRC_ZB_QUIRK_UNSUPPORTED, ALARM_DEVCLASS_GENERIC, 0,
     "Use Home Assistant: it speaks Tuya's protocol"},

    /* Tuya's plain ZCL radios are standard IAS, but rarely send a Zone Enroll Request: they are
     * happy once the CIE address is written, and then just report. */
    {"_TZ3000_", NULL, ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_GENERIC, 0,
     "Reports once paired, without enrolling"},

    /* Sonoff's contacts and motion sensors behave the same way. */
    {"eWeLink", "DS01", ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_DOOR, 0,
     "Reports once paired, without enrolling"},
    {"eWeLink", "MS01", ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_MOTION, 0,
     "Reports once paired, without enrolling"},
    {"eWeLink", "SNZB-", ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_GENERIC, 0,
     "Reports once paired, without enrolling"},

    /* Aqara contacts are an On/Off endpoint rather than an IAS zone, and they sleep hard: the
     * enrollment steps only land inside a wake window. Their check-in is roughly hourly, so the
     * default sleepy supervision window suits them. */
    {"LUMI", "lumi.sensor_magnet", ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_DOOR, 0,
     "Reports as a switch, not as a security zone"},
    {"LUMI", "lumi.sensor_motion", ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_MOTION, 0,
     "Reports as occupancy, not as a security zone"},
    {"LUMI", "lumi.sensor_wleak", ZSRC_ZB_QUIRK_ENROLL_WAKE, ALARM_DEVCLASS_WATER, 0,
     "Answers only while awake: keep it awake to pair"},
    {"LUMI", NULL, ZSRC_ZB_QUIRK_ENROLL_WAKE, ALARM_DEVCLASS_GENERIC, 0,
     "Answers only while awake: keep it awake to pair"},

    /* IKEA's motion sensor sends On/Off commands to whatever it is bound to. */
    {"IKEA of Sweden", "TRADFRI motion sensor", ZSRC_ZB_QUIRK_NO_ENROLL, ALARM_DEVCLASS_MOTION, 0,
     "Reports as a switch, not as a security zone"},

    /* Heiman's life-safety sensors are standard IAS and mains- or long-battery powered; they
     * are slow to answer enrollment, so give it the wake-window treatment. */
    {"HEIMAN", NULL, ZSRC_ZB_QUIRK_ENROLL_WAKE, ALARM_DEVCLASS_GENERIC, 0,
     "Takes its time answering enrollment"},
};

const zsrc_zb_quirk_t *zsrc_zb_quirk_table(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(QUIRKS) / sizeof(QUIRKS[0]);
    }
    return QUIRKS;
}

/* A NULL or empty pattern matches anything; otherwise 'text' has to start with it. */
static bool prefix(const char *pattern, const char *text)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    const size_t n = strlen(pattern);
    return strncmp(pattern, text, n) == 0;
}

const zsrc_zb_quirk_t *zsrc_zb_quirk_for(const char *manufacturer, const char *model)
{
    size_t n = 0;
    const zsrc_zb_quirk_t *table = zsrc_zb_quirk_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (prefix(table[i].manufacturer, manufacturer) && prefix(table[i].model, model)) {
            return &table[i];
        }
    }
    return NULL;
}
