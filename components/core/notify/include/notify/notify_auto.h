/*
 * notify_auto.h - naming the automation that did something (docs/features/41 D30, AUT-009).
 *
 * An event an automation caused carries its source {ALARM_SRC_AUTOMATION, uid}, and the user it
 * acted as (the person who saved it). The words never name that person as the one who did it:
 * they say "By automation" and which one, by its trigger clause ("At 10:00 AM on weekdays"),
 * looked up by uid through the one hook below. The event words (notify_words), the MQTT and Home
 * Assistant JSON (alarm_proto) and the Panel Manager read it from here.
 *
 * The hook is the one piece of state in this component: set once where the services start,
 * before any task reads words (the firmware: automation_service's auto_names.c, registered by
 * panel_app; the PC preview: host_panel.c), never changed after. Without one, an automation's
 * events read "By automation" alone and the JSON names no automation.
 *
 * Portable C11: no heap, no RTOS. The hook may be called from several tasks at once (the UI
 * thread, the notification and MQTT tasks): it copies what it needs under its own lock.
 */
#ifndef NOTIFY_AUTO_H
#define NOTIFY_AUTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "automation/automation.h"
#include "i18n/i18n.h"
#include "i18n/i18n_date.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOTIFY_AUTO_CLAUSE_LEN   64  /* "At 10:00 AM on weekdays", "When Front Door opens" */
#define NOTIFY_AUTO_SENTENCE_LEN 160 /* the whole sentence, where there is room */

/* What the hook knows of one automation. */
typedef struct {
    char clause[NOTIFY_AUTO_CLAUSE_LEN];     /* its trigger clause in the catalog's language; ""
                                                when it can't say */
    char sentence[NOTIFY_AUTO_SENTENCE_LEN]; /* the whole sentence; "" = the clause is all */
    uint32_t changed_at;                     /* epoch s its sentence last changed; 0 = not known */
    uint8_t maker;                           /* who saved it (the user it acts as); 0 = none */
} notify_auto_info_t;

/* The automation with that uid now: true and *out filled, or false when there is none (deleted;
 * uids are never reused). cat: the language for the words (NULL English); cfg: the panel's
 * configuration for zone and partition names (may be NULL). */
typedef bool (*notify_auto_lookup_fn)(void *ctx, const i18n_catalog_t *cat,
                                      const alarm_config_t *cfg, uint16_t uid,
                                      notify_auto_info_t *out);
/* A wall-clock time as the panel shows a time of day ("11:30 PM", "23:30"), in local time;
 * out "" when it can't. */
typedef void (*notify_time_fn)(void *ctx, const i18n_catalog_t *cat, int64_t epoch_ms, char *out,
                               size_t len);

typedef struct {
    notify_auto_lookup_fn automation; /* NULL: "By automation" alone */
    notify_time_fn time_of_day;       /* NULL: a put-off's line names no time */
    void *ctx;
} notify_hooks_t;

/* Set the hooks (copied); NULL clears them. Once, before any task reads words. */
void notify_set_hooks(const notify_hooks_t *hooks);

typedef enum {
    NOTIFY_AUTO_UNKNOWN = 0, /* no hook, or uid 0 (an arming from before a restart) */
    NOTIFY_AUTO_FOUND,       /* *out filled */
    NOTIFY_AUTO_GONE,        /* the hook says there is no such automation now */
} notify_auto_found_t;

/* The automation uid through the hook. *out is always set (empty unless FOUND). */
notify_auto_found_t notify_auto_lookup(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                                       uint16_t uid, notify_auto_info_t *out);

/* A wall-clock time through the hook; false (out "") without one or a time. */
bool notify_time_of_day(const i18n_catalog_t *cat, int64_t epoch_ms, char *out, size_t len);

/* An automation's trigger clause, as its list row and page say it ("At 10:00 AM on weekdays",
 * "When Front Door opens", "When the panel is armed Night"): the portable copy of the words, for
 * a hook to fill notify_auto_info_t.clause with. region: the week's first day (NULL the US);
 * clock24: the panel's clock. */
void notify_auto_when(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                      const i18n_region_t *region, bool clock24, const auto_trigger_t *w,
                      char *out, size_t len);

/* A time of day as the panel writes one in a list ("10:00 AM", "22:00"). */
void notify_clock_text(const i18n_catalog_t *cat, bool clock24, unsigned hour, unsigned minute,
                       char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_AUTO_H */
