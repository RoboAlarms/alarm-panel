/*
 * web_automations.h - the Panel Manager's Automations page (private to panel_web; docs/features/41
 * DESIGN.md section 10): GET /api/v1/automations, GET /api/v1/automations/devices and POST
 * /api/v1/automations, all the system master's and the installer's (panel_web's access table,
 * ACC_SYSTEM). The rules live in automation_service; this file only turns them into JSON and back.
 */
#ifndef WEB_AUTOMATIONS_H
#define WEB_AUTOMATIONS_H

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "cJSON.h"

/* Whether this firmware has automations (CONFIG_ALARM_FEATURE_AUTOMATIONS): status's
 * capabilities.automations, and panel_web answers 404 to the three paths without it. */
bool web_automations_available(void);

/* GET automations: {maximum, used, gen, clock_set, read_only, less_secure, automations[], zones[],
 * partitions[], outputs[] ({number, name, gpio}: the wired outputs "For automations"), notify[]
 * (the phone services set up: "ntfy", "pushover"), master (the system master's name), warnings[],
 * reminders[]}. NULL: no memory, or the configuration couldn't be read (503). */
cJSON *web_automations_get(void);

/* The arming warnings and holds now (automation_service_warnings; RAM only), of the partitions in
 * `partitions` (bit p-1), as o[key] = [{uid, partition, level, at (epoch s, or null), state:
 * "on"|"hour"|"night"|"day", by (who put it off, or null), bypass}]: GET automations's `warnings`
 * and status's `automation_warnings`, the Overview's card and note (docs/features/41 D19, D20,
 * D27). Names from cfg. Without the feature it adds nothing. */
bool web_automations_add_warnings(cJSON *o, const char *key, const alarm_config_t *cfg,
                                  uint8_t partitions); /* false: no memory for all of it */

/* The reminders' notes now (automation_service_reminders; RAM only, BL-224), of the partitions in
 * `partitions` (bit p-1), as o[key] = [{uid, partition, minute}]: GET automations's `reminders`
 * and status's `automation_reminders`, the Overview's steady line "Not armed yet · reminder at
 * 11:00 PM". Without the feature it adds nothing. */
bool web_automations_add_reminders(cJSON *o, const char *key,
                                   uint8_t partitions); /* false: no memory for all of it */

/* GET automations/devices: {devices:[{source, ref, name, area, kind, online,
 * actions:[{action, confirm}]}]} - only what an automation may do (hc_auto_offer with the
 * installer option as it is now). NULL: no memory (503). */
cJSON *web_automations_devices(void);

/* POST automations {action:'save'|'switch'|'delete'|'try', slot, gen, automation | on}, as the
 * signed-in `user` (the service makes them the maker). Returns the answer to send ({result, ...}
 * or {error, index?}) and its HTTP status in *http (200, 400, 409, 503); NULL: no memory (503).
 * Runs on the HTTPS worker, whose stack is internal: the service writes NVS on it. */
cJSON *web_automations_post(const cJSON *body, uint8_t user, int *http);

#endif /* WEB_AUTOMATIONS_H */
