/*
 * auto_names.h - the automations' names for the event words and the JSON (docs/features/41 D30,
 * AUT-009): registers notify's hook (notify/notify_auto.h), uid -> trigger clause, changed_at and
 * who saved it, built only on automation_service's public readers (which copy under the
 * service's own lock), and the local time of day for a put-off's "until 11:30 PM".
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the hooks, once, before the tasks that read words start (panel_app). clock_mode,
 * region and lang are the panel's stored preferences (panel_prefs_t), passed in: the hook runs
 * on tasks whose stacks may be in PSRAM, which must not read NVS. Without
 * CONFIG_ALARM_FEATURE_AUTOMATIONS it registers nothing: an automation's events read "By
 * automation". */
void auto_names_register(uint8_t clock_mode, const char *region, const char *lang);

/* The preferences changed (saved at the touchscreen or in the Panel Manager): the next words
 * use the new clock and region. */
void auto_names_set_clock(uint8_t clock_mode, const char *region, const char *lang);

#ifdef __cplusplus
}
#endif
