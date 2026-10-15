#pragma once
#include "i18n/i18n.h"
#include "web_config.h"
#include <stdint.h>
/* partitions: the viewer's partitions (bit 0 = partition 1); the zone list shows only theirs,
 * as the Zones view of a keypad shows its own. words: the language the page asked for
 * (X-Panel-Language; NULL is English): the sentences the panel makes are in it
 * (docs/features/37 I18N-031); keys, names and states stay as the API defines them. */
cJSON *web_extra_get(const ui_backend_t *backend, const char *path, uint8_t partitions,
                     const i18n_catalog_t *words);
cJSON *web_extra_action(const ui_backend_t *backend,const char *path,const cJSON *data,
                       uint8_t user,alarm_authority_t authority, const i18n_catalog_t *words);
/* A count of wrong guesses (panel_web.c's wrong passwords): five refusals are free, then
 * every guess waits a minute, doubling with each further refusal (15 minutes at most); an hour
 * without one forgets them, and a right guess does not reset it. Zero-initialised. HTTP
 * worker only. */
typedef struct {
    unsigned fails;
    int64_t last_fail_us, hold_until_us;
} web_gate_t;
bool web_gate_open(web_gate_t *g);
void web_gate_failed(web_gate_t *g);
