/*
 * auto_names.c - notify's automation hook on the panel (docs/features/41 D30, AUT-009).
 *
 * Callers are the UI thread (Activity), the notification task and the MQTT task, at once: every
 * read goes through automation_service_get, which copies the rule under the service's own short
 * lock; the words are built on the caller's stack. Nothing here touches NVS or the alarm task.
 * This file uses only the service's public calls (automation_service.h).
 */
#include "auto_names.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_AUTOMATIONS

#include "automation_service.h"
#include "esp_heap_caps.h"
#include "i18n/i18n_date.h"
#include "notify/notify_auto.h"

/* The panel's clock (see auto_names.h): each is one aligned word, written by whoever saves the
 * preferences and read by the word builders; a reader between the two writes gets one old
 * and one new value for one line, never a torn one. */
static volatile bool s_clock24;
static const i18n_region_t *volatile s_region;

static bool lookup(void *ctx, const i18n_catalog_t *cat, const alarm_config_t *cfg, uint16_t uid,
                   notify_auto_info_t *out)
{
    (void)ctx;
    /* An auto_rule_t is about 300 bytes: off the caller's stack, which may be small. */
    auto_rule_t *r = heap_caps_malloc_prefer(sizeof(*r), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (r == NULL) {
        return true; /* can't tell: found, with no words ("By automation") */
    }
    bool found = automation_service_get(uid, r);
    if (found) {
        notify_auto_when(cat, cfg, s_region, s_clock24, &r->when, out->clause,
                         sizeof(out->clause));
        out->changed_at = r->changed_at;
        out->maker = r->made_by;
    } else if (automation_service_read_only()) {
        /* Not started yet, or a store it couldn't read: no word on whether it is gone. */
        found = true;
    }
    heap_caps_free(r);
    return found;
}

static void time_of_day(void *ctx, const i18n_catalog_t *cat, int64_t epoch_ms, char *out,
                        size_t len)
{
    (void)ctx;
    const time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL) {
        out[0] = '\0';
        return;
    }
    notify_clock_text(cat, s_clock24, (unsigned)tm.tm_hour, (unsigned)tm.tm_min, out, len);
}

void auto_names_set_clock(uint8_t clock_mode, const char *region, const char *lang)
{
    const i18n_region_t *reg = i18n_region_for(region, lang);
    s_region = reg;
    s_clock24 = i18n_clock_24h(clock_mode, reg);
}

void auto_names_register(uint8_t clock_mode, const char *region, const char *lang)
{
    auto_names_set_clock(clock_mode, region, lang);
    const notify_hooks_t hooks = {.automation = lookup, .time_of_day = time_of_day, .ctx = NULL};
    notify_set_hooks(&hooks);
}

#else /* !CONFIG_ALARM_FEATURE_AUTOMATIONS */

void auto_names_register(uint8_t clock_mode, const char *region, const char *lang)
{
    (void)clock_mode;
    (void)region;
    (void)lang;
}

void auto_names_set_clock(uint8_t clock_mode, const char *region, const char *lang)
{
    (void)clock_mode;
    (void)region;
    (void)lang;
}

#endif
