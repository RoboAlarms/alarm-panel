/*
 * web_zwave.h - the Panel Manager's Z-Wave page (private to panel_web; docs/features/36 screens.md
 * §11): GET /api/v1/zwave and POST /api/v1/zwave, both the installer's (panel_web's access table).
 */
#ifndef WEB_ZWAVE_H
#define WEB_ZWAVE_H

#include <stdint.h>

#include "cJSON.h"
#include "ui_backend/ui_backend.h"

/* {"available":false} without Z-Wave; else the radio, the devices, SmartStart's list, the
 * network operation running (`progress`) and the radio's diagnostics (`diag`). */
cJSON *web_zwave_get(const ui_backend_t *b);
/* `action` (a JSON body's): SmartStart's list - register (`code`, `name`), on, off, forget, grant
 * (`keys`) the entry named by `id`; a network operation - add, remove, keep_listening, stop, allow
 * (`keys`), pin (`pin`, the 5-digit number), insecure, dismiss, restart, remove_failed, ping,
 * routes (`node` 0: every device), setup (`node`, `generation`), as the touchscreen's. `user`: who
 * asks (the event log). The result word ("added", "ok", "busy", ...); NULL: no Z-Wave. */
const char *web_zwave_post(const ui_backend_t *b, const cJSON *body, uint8_t user);
/* PUT /api/v1/zwave-radio: the installer's own .gbl for the radio (network.md §14). The result
 * word ("ok", "invalid", "busy"...); NULL: no Z-Wave. */
const char *web_zwave_radio_file(const uint8_t *data, size_t len, uint8_t user);

#endif /* WEB_ZWAVE_H */
