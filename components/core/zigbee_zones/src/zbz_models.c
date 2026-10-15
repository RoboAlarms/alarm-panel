/*
 * zbz_models.c - the model table (docs/features/20 devices.md §10; ZB-007, ZB-023, ZB-024): the
 * name the pages give a model, how to put it into pairing mode and how to wake it, a role where
 * the clusters can't tell, and how a siren or keypad of that model wants its frames. See
 * zbz_models.h.
 *
 * Rows are seeded from testing.md §2's signature rows and alarm-devices.md's model notes, with
 * manufacturer and model as the devices report them in Basic (the zigpy/zha and deCONZ device
 * files). Every row is provisional until a
 * real unit is captured on the bench (docs/sensors/): the words above all, which come from the
 * makers' quick-start sheets as those files quote them.
 *
 * Rules the tests hold the whole table to (testing.md §5; test_zbz_models.c):
 * - each row names a model (never a brand alone: no row without a model prefix, no name that is
 *   only a brand), and its words are non-empty when given;
 * - no row is hidden behind an earlier one: a more specific prefix comes first ("3400-D" before
 *   "3400");
 * - siren flags only on siren rows, keypad flags only on keypad and fob rows.
 * The siren layout flag (ZBZ_MODEL_WD_REVERSED) is on no row yet: open implementations disagree
 * for Frient's sirens and one real siren decides (testing.md §4).
 */
#include "zigbee_zones/zbz_models.h"

#include <string.h>

#include "zigbee_zones/zbz_registry.h"
#include "i18n/i18n.h"

/* The pages translate these under their context, as zw_quirks.c's (docs/features/37); WAKE1
 * goes on after "To wake it:" and its kind, so it says only what to do. */
#define HOLD5 I18N_NOOP_C("pairing", "Hold its button for 5 seconds, until its light blinks")
#define WAKE1 I18N_NOOP_C("wake up", "Press its button once")

static const zbz_model_t MODELS[] = {
    /* ---- sensors ---- */
    {"SONOFF", "SNZB-04P", "Door sensor", HOLD5, NULL, 0u, 0u, 0u},
    {"SONOFF", "SNZB-02D", "Temperature sensor", HOLD5, NULL, 0u, 0u, 0u},
    {"Third Reality", "3RDS17BZ", "Door sensor", NULL, NULL, 0u, 0u, 0u},
    {"LUMI", "lumi.magnet.agl02", "Door sensor", HOLD5, WAKE1, 0u, 0u, 0u},
    {"LUMI", "lumi.motion.agl04", "Motion sensor", HOLD5, WAKE1, 0u, 0u, 0u},
    {"IKEA of Sweden", "PARASOLL", "Door sensor", NULL, NULL, 0u, 0u, 0u},
    {"CentraLite", "3300", "Door sensor", NULL, NULL, 0u, 0u, 0u},
    {"CentraLite", "3326", "Motion sensor", NULL, NULL, 0u, 0u, 0u},
    {"HEIMAN", "SmokeSensor", "Smoke alarm", NULL, NULL, 0u, 0u, 0u},

    /* ---- sirens (alarm-devices.md §2) ---- */
    /* Heiman HS2WD, also sold as Tuya TS0216: emergency and stop only. */
    {"HEIMAN", "WarningDevice", "Siren", NULL, NULL, (uint8_t)ZBZ_ROLE_SIREN,
     ZBZ_MODEL_WD_ONE_TONE, 0u},
    {NULL, "TS0216", "Siren", NULL, NULL, (uint8_t)ZBZ_ROLE_SIREN, ZBZ_MODEL_WD_ONE_TONE, 0u},
    {"frient A/S", "SIRZB-11", "Siren", NULL, NULL, (uint8_t)ZBZ_ROLE_SIREN, 0u, 0u},
    {"Develco Products A/S", "SIRZB-11", "Siren", NULL, NULL, (uint8_t)ZBZ_ROLE_SIREN, 0u, 0u},

    /* ---- keypads and key fobs (alarm-devices.md §3) ---- */
    {"frient A/S", "KEPZB-110", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD,
     ZBZ_MODEL_ACE_AUDIBLE, 0u},
    {"Develco Products A/S", "KEPZB-110", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD,
     ZBZ_MODEL_ACE_AUDIBLE, 0u},
    {"CentraLite", "3400-D", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD,
     ZBZ_MODEL_ACE_ARMED_80, 0u},
    {"CentraLite", "3400", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD, 0u, 0u},
    {"CentraLite", "3405", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD, 0u, 0u},
    {"Universal Electronics Inc", "URC4450BC0", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD,
     0u, 0u},
    {NULL, "ZB-KeypadGeneric", "Keypad", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYPAD,
     ZBZ_MODEL_ACE_INIT, 0u},
    /* Heiman HS1RC: an IAS ACE client with four buttons and no keypad. */
    {"HEIMAN", "RC-EF-3.0", "Key fob", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYFOB, ZBZ_MODEL_ACE_FOB,
     0u},
    {"HEIMAN", "RC_V14", "Key fob", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYFOB, ZBZ_MODEL_ACE_FOB, 0u},
    {"HEIMAN", "RC-EM", "Key fob", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYFOB, ZBZ_MODEL_ACE_FOB, 0u},
    {"HEIMAN", "RC-N", "Key fob", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYFOB, ZBZ_MODEL_ACE_FOB, 0u},
    {NULL, "ZB-KeyfobGeneric", "Key fob", NULL, NULL, (uint8_t)ZBZ_ROLE_KEYFOB,
     ZBZ_MODEL_ACE_FOB, 0u},
};

const zbz_model_t *zbz_model_table(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(MODELS) / sizeof(MODELS[0]);
    }
    return MODELS;
}

/* A NULL or empty pattern matches anything; otherwise `text` starts with it (zb_quirks.c's). */
static bool prefix(const char *pattern, const char *text)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    return strncmp(pattern, text, strlen(pattern)) == 0;
}

const zbz_model_t *zbz_model_for(const char *manufacturer, const char *model)
{
    size_t n = 0;
    const zbz_model_t *table = zbz_model_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (prefix(table[i].manufacturer, manufacturer) && prefix(table[i].model, model)) {
            return &table[i];
        }
    }
    return NULL;
}
