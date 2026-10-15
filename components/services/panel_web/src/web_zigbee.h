/*
 * web_zigbee.h - the Panel Manager's Zigbee page (private to panel_web; docs/features/20
 * devices.md §14, ZB-025; the pages BL-145 draws): GET /api/v1/zigbee and POST /api/v1/zigbee,
 * both the installer's (panel_web's access table), the POST refused while the panel is armed.
 */
#ifndef WEB_ZIGBEE_H
#define WEB_ZIGBEE_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "ui_backend/ui_backend.h"

/* {"available":false} without Zigbee; else may_change, the radio, the devices, the operation
 * running (`progress`) and the install codes waiting (`codes`), with `device` (its page) when
 * `device` is non-zero. may_change: the panel isn't armed (changes are taken). *found: false when
 * `device` names no device (the caller answers 404). NULL: no memory, for the list or for
 * `device`'s page (the caller answers 503, never 404 or an empty list: BL-113). */
cJSON *web_zigbee_get(const ui_backend_t *b, uint32_t device, bool may_change, bool *found);
/* `op` (a JSON body's, devices.md §14): add, add_code, keep, stop, remove, forget, replace, ping,
 * identify, setup, rename, code_add, code_forget, survey, channel, reset, restart, retry, dismiss,
 * siren, siren_test, keypad, fob. The caller has checked the access table, the panel's lock and,
 * for reset, the installer's password again (never a keypad code). `user`: who asks (the log).
 * The result word ("ok", "busy", "wrong_code"...; "invalid" for a body it can't read); NULL: no
 * Zigbee on this panel. */
const char *web_zigbee_post(const ui_backend_t *b, const cJSON *body, uint8_t user);
/* The words web_zigbee_post answers, by ui_zb_req_result_t (every value has one). */
const char *web_zigbee_result_word(ui_zb_req_result_t r);

#endif /* WEB_ZIGBEE_H */
