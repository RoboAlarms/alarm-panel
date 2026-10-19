#pragma once
/* CP-005: fill the result before ending the run (which clears target/previous).
 * Called with the service lock held. Portable so stale P4 results and mixed
 * P4/C6 version reporting can be regression-tested on the host. */
#include "panel_ota.h"
#include <stdio.h>
#include <string.h>

static inline void cp_ota_failure_result(panel_ota_status_t *st, const char *why,
                                         const alarm_source_t *src, uint8_t user, uint32_t at)
{
    st->phase = PANEL_OTA_FAILED;
    st->percent = -1;
    st->waiting_for[0] = '\0';
    st->failed_install = true;
    snprintf(st->error, sizeof st->error, "%s", why);
    panel_ota_last_t *last = &st->last;
    memset(last, 0, sizeof *last);
    last->result = PANEL_OTA_LAST_FAILED;
    snprintf(last->version, sizeof last->version, "%s", st->target);
    snprintf(last->from, sizeof last->from, "%s", st->previous);
    snprintf(last->reason, sizeof last->reason, "%s", st->error);
    last->at = at;
    last->by_src = src ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    last->by_user = user;
}
