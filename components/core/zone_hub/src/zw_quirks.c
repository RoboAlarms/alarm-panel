/*
 * zw_quirks.c - the Z-Wave quirks table (docs/features/36 zones.md §8): data, not code, like
 * zb_quirks.c. A row matches a model by manufacturer ID, product type and product ID (FFFF = any)
 * and says what the standard command classes don't: the model's name for people, a better device
 * class, slots to add, hide or move, how to add and wake it. A device without a row works when it
 * follows the standard CCs.
 *
 * The starter rows are the IDs Z-Wave JS's device files give for these models (2026-09) and are
 * to be verified on the bench before anything relies on them.
 *
 * The rows' IDs, alarm maps and idle resets are taken from Z-Wave JS's device configuration
 * files (https://github.com/zwave-js/zwave-js, MIT License, Copyright (c) 2018-2024 AlCalzone;
 * see components/core/zwave_host/LICENSE-zwave-js.txt and NOTICE).
 */
#include "zone_hub/zsrc_zwave.h"
#include "i18n/i18n.h"

#define SLOT(s) (1u << (s))

/* V1 Alarm Type, Level -> Notification type, event (zones.md §8's alarm map): smoke 01 02 smoke
 * detected, 01 03 its test, CO 02 02 CO detected, event 00 idle. Alarm type 0C is the test
 * button, 0D its end. */
#define ZSMOKE_ALARM_MAP                                                                           \
    {0x01u, 0xFFu, 0x01u, 0x02u}, {0x01u, 0x00u, 0x01u, 0x00u}, {0x0Cu, 0xFFu, 0x01u, 0x03u},      \
        {0x0Cu, 0x00u, 0x01u, 0x00u}, {0x0Du, 0xFFu, 0x01u, 0x00u}
#define ZCOMBO_ALARM_MAP                                                                           \
    ZSMOKE_ALARM_MAP, {0x02u, 0xFFu, 0x02u, 0x02u}, {0x02u, 0x00u, 0x02u, 0x00u}

static const zsrc_zw_quirk_t QUIRKS[] = {
    {
        .manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE001u,
        .name = "Zooz ZSE41 open/close sensor",
        .device_class = ALARM_DEVCLASS_DOOR,
        .add_words = I18N_NOOP_C("pairing", "Click the Z-Wave button 3 times quickly"),
        .wake_words = I18N_NOOP_C("wake up", "Click the Z-Wave button 4 times quickly"),
    },
    {
        .manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE002u,
        .name = "Zooz ZSE42 water leak sensor",
        .device_class = ALARM_DEVCLASS_WATER,
        .add_words = I18N_NOOP_C("pairing", "Click the Z-Wave button 3 times quickly"),
        .wake_words = I18N_NOOP_C("wake up", "Click the Z-Wave button 4 times quickly"),
    },
    {
        /* Its tilt reports as a door's open/closed, its shock as "product moved". */
        .manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE003u,
        .name = "Zooz ZSE43 tilt and shock sensor",
        .device_class = ALARM_DEVCLASS_GARAGE_DOOR,
        .offer = {SLOT(ZSRC_ZW_SLOT_IMPACT)},
        .remap_from = ZSRC_ZW_SLOT_CONTACT,
        .remap_to = ZSRC_ZW_SLOT_TILT,
        .flags = ZSRC_ZW_QUIRK_MOVED_IS_IMPACT,
        .add_words = I18N_NOOP_C("pairing", "Click the Z-Wave button 3 times quickly"),
        .wake_words = I18N_NOOP_C("wake up", "Click the Z-Wave button 4 times quickly"),
    },
    {
        .manufacturer = 0x027Au, .product_type = 0x7000u, .product_id = 0xE004u,
        .name = "Zooz ZSE44 temperature sensor",
        .device_class = ALARM_DEVCLASS_TEMPERATURE,
        .note = "For freeze and heat zones",
        .add_words = I18N_NOOP_C("pairing", "Click the Z-Wave button 3 times quickly"),
        .wake_words = I18N_NOOP_C("wake up", "Click the Z-Wave button 4 times quickly"),
    },
    {
        .manufacturer = 0x027Au, .product_type = 0x0301u, .product_id = 0x0012u,
        .name = "Zooz ZSE18 motion sensor",
        .device_class = ALARM_DEVCLASS_MOTION,
        .wake_words = I18N_NOOP_C("wake up", "Press and hold the Z-Wave button for 5 seconds"),
    },
    {
        .manufacturer = 0x027Au, .product_type = 0x0201u, .product_id = 0x0006u,
        .name = "Zooz ZSE11 Q sensor",
        .device_class = ALARM_DEVCLASS_MOTION,
        .no_supervision_before = 0x0200u, /* its Wake Up Interval Set "fails" before 2.0 */
    },
    {
        .manufacturer = 0x027Au, .product_type = 0x0001u, .product_id = 0x0005u,
        .name = "Zooz ZSE29 outdoor motion sensor",
        .device_class = ALARM_DEVCLASS_MOTION,
        .no_supervision_before = 0x0202u, /* supervised commands always "fail" before 2.2 */
    },
    {
        .manufacturer = 0x027Au, .product_type = 0x0004u, .product_id = 0x0006u,
        .name = "Zooz ZSE70 outdoor motion sensor",
        .device_class = ALARM_DEVCLASS_MOTION,
        .note = "Long Range capable",
    },
    {
        /* Z-Wave JS's dwzwave25.json sets forceNotificationIdleReset: it never sends idle, so
         * its intrusion and cover tamper would stay set. Like Z-Wave JS, 5 minutes. Its device
         * file adds it by pulling the tab from the back (the LED goes from solid blue to off)
         * and wakes it by opening the top case: that reports a cover tamper, which the 5
         * minutes then end. Not yet verified on the bench. */
        .manufacturer = 0x014Au, .product_type = 0x0004u, .product_id = 0x0002u,
        .name = "Ecolink DWZWAVE25 door/window sensor",
        .device_class = ALARM_DEVCLASS_DOOR,
        .idle_after_s = 300u,
        .add_words = I18N_NOOP_C("pairing", "Pull the plastic tab from the back"),
        .wake_words = I18N_NOOP_C("wake up", "Open the top cover"),
    },
    /* First Alert (BRK) smoke and smoke/CO alarms report through the Notification CC's V1 Alarm
     * Type and Level only (ZW-043). The maps are Z-Wave JS's (0x0138 zcombo.json, zsmoke.json,
     * compat.alarmMapping) and wait for a bench check (BL-153). */
    {
        .manufacturer = 0x0138u, .product_type = 0x0001u, .product_id = 0x0002u,
        .name = "First Alert ZCOMBO smoke and CO alarm",
        .alarm_map = {ZCOMBO_ALARM_MAP},
    },
    {
        .manufacturer = 0x0138u, .product_type = 0x000Cu, .product_id = 0x0001u,
        .name = "First Alert ZCOMBO smoke and CO alarm",
        .alarm_map = {ZCOMBO_ALARM_MAP},
    },
    {
        .manufacturer = 0x0138u, .product_type = 0x0001u, .product_id = 0x0001u,
        .name = "First Alert ZSMOKE smoke alarm",
        .alarm_map = {ZSMOKE_ALARM_MAP},
    },
};

const zsrc_zw_quirk_t *zsrc_zw_quirk_table(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(QUIRKS) / sizeof(QUIRKS[0]);
    }
    return QUIRKS;
}

static bool id_matches(uint16_t want, uint16_t have)
{
    return want == ZSRC_ZW_QUIRK_ANY || want == have;
}

bool zsrc_zw_quirk_matches(const zsrc_zw_quirk_t *q, uint16_t manufacturer,
                           uint16_t product_type, uint16_t product_id)
{
    return id_matches(q->manufacturer, manufacturer) &&
           id_matches(q->product_type, product_type) && id_matches(q->product_id, product_id);
}

const zsrc_zw_quirk_t *zsrc_zw_quirk_for(uint16_t manufacturer, uint16_t product_type,
                                         uint16_t product_id)
{
    for (size_t i = 0; i < sizeof(QUIRKS) / sizeof(QUIRKS[0]); i++) {
        if (zsrc_zw_quirk_matches(&QUIRKS[i], manufacturer, product_type, product_id)) {
            return &QUIRKS[i];
        }
    }
    return NULL;
}

bool zsrc_zw_quirk_no_supervision(const zsrc_zw_quirk_t *q, bool has_firmware, uint16_t firmware)
{
    if (q == NULL || q->no_supervision_before == 0u) {
        return false;
    }
    return !has_firmware || firmware < q->no_supervision_before;
}
