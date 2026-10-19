#pragma once
#include "alarm/alarm_config.h"
#include "cJSON.h"
#include <stdint.h>

/* HTTPS worker only, including tick (queued work). The authenticated installer owns an
 * enrollment by user AND random attempt id; no touchscreen is required at the master. */
cJSON *web_keypads_get(uint8_t user);
const char *web_keypads_post(uint8_t user, const cJSON *body);
void web_keypads_tick(void *unused);
/* Guard generic configuration writes against editing paired slots or orphaning keypads. */
bool web_keypads_config_valid(const cJSON *changes, const alarm_config_t *cfg);
