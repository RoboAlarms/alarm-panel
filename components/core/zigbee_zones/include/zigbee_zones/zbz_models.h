/*
 * zbz_models.h - what the panel knows about particular models (docs/features/20 devices.md §10;
 * ZB-007, ZB-023, ZB-024): the name the pages give them, how to wake them or put them into
 * pairing mode, and how a siren or keypad of that model wants its frames. Data, not code: a new
 * model is a row.
 *
 * This is the device-management side of the quirks: zone_hub's zb_quirks.c (zsrc_zb_quirk_t)
 * stays the sensor-behaviour table (enrollment, alarm bits, supervision), unchanged. Rows match
 * like it does - manufacturer then model, each a prefix, NULL or "" anything, first match wins -
 * and testing.md §5's whole-table checks apply to both tables (no row hidden by an earlier one,
 * near misses refused, every row's words non-empty).
 *
 * Every row is provisional until a real unit is captured on the bench (docs/sensors/): the
 * siren layouts above all, where open implementations disagree (testing.md §4).
 *
 * Declarations only until ZB-M3 (names and words) and ZB-M10/ZB-M11 (sirens, keypads) fill it.
 */
#ifndef ZBZ_MODELS_H
#define ZBZ_MODELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How a model departs from the IAS WD and IAS ACE frames (zbz_wd.h, zbz_ace.h). */
#define ZBZ_MODEL_WD_REVERSED     0x0001u /* Warning Info and Squawk bits in the reversed layout
                                           * (mode in the low nibble; Zigbee2MQTT's Develco
                                           * packing) - unconfirmed, see testing.md §4 */
#define ZBZ_MODEL_WD_ONE_TONE     0x0002u /* only "emergency" and stop: no burglar or fire sound
                                           * (Heiman HS2WD) - fire still sounds, on its one tone */
#define ZBZ_MODEL_WD_NO_LEVEL     0x0004u /* ignores the siren level: the volume row is hidden */
#define ZBZ_MODEL_NO_DEFAULT_RSP  0x0008u /* never sends Default Responses: a Start Warning is
                                           * confirmed by nothing but its supervision */
#define ZBZ_MODEL_ACE_ARMED_80    0x0010u /* panel status 0x80 for any armed state (Centralite
                                           * 3400-D) */
#define ZBZ_MODEL_ACE_TOUCH_WAKE  0x0020u /* Get Panel Status means "someone touched it": answer
                                           * at once, it sleeps again within seconds */
#define ZBZ_MODEL_ACE_AUDIBLE     0x0040u /* wants audible notification 1 on delay frames to beep
                                           * (Frient KEPZB-110) */
#define ZBZ_MODEL_ACE_FOB         0x0080u /* an ACE client with no keypad: a key fob */
#define ZBZ_MODEL_ACE_INIT        0x0100u /* must be sent one Panel Status Changed after joining
                                           * before it takes Arm Responses (Linkind keypad) */

typedef struct {
    const char *manufacturer;  /* prefix; NULL or "" any */
    const char *model;         /* prefix; NULL or "" any */
    const char *name;          /* what the pages call it ("Door sensor"); never a brand alone */
    const char *add_words;     /* how to put it into pairing mode; NULL not known */
    const char *wake_words;    /* how to wake it; NULL: "Press its button to wake it" */
    uint8_t role;              /* zbz_role_t when the clusters alone can't tell; 0 from them */
    uint16_t flags;            /* ZBZ_MODEL_* */
    uint16_t max_duration_s;   /* a siren's longest warning it takes: caps the MaxDuration its
                                * setup reads (the shorter wins), alone when none was read
                                * (zbw_warn_seconds); 0 none, the read one or 240 */
} zbz_model_t;

const zbz_model_t *zbz_model_table(size_t *count);
const zbz_model_t *zbz_model_for(const char *manufacturer, const char *model);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_MODELS_H */
