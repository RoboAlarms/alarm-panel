#pragma once
#include "cJSON.h"
#include "i18n/i18n.h"
#include "ui_backend/ui_backend.h"

/* Typed, allow-listed serialization. Secret values never appear in GET results.
 * The service supplies authorization; these adapters share the touchscreen's validation.
 * `words` is the page's language (X-Panel-Language; NULL is English): the choices the panel
 * names itself (its languages, regions, clock) are named in it. NULL with *busy false: a domain
 * this firmware doesn't have (404); with *busy true: an allocation failed (503 "Memory busy"). */
cJSON *web_config_get(const ui_backend_t *be, const char *domain, const i18n_catalog_t *words,
                      bool *busy);
bool web_config_set(const ui_backend_t *be, const char *domain, const cJSON *data,
                    const char *operation, char *why, size_t cap);
cJSON *web_alarm_config_get(const alarm_config_t *cfg);
bool web_alarm_config_patch(alarm_config_t *cfg, const cJSON *data, char *why, size_t cap);
