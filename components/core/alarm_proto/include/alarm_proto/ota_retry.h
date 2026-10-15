/*
 * ota_retry.h - when the panel looks for a release again after a failed check (OTA-015).
 *
 * Plain C11, part of alarm_proto's firmware-update helpers since docs/features/33 step 8 (it was
 * inside components/services/panel_ota), so test/test_fw.c checks the schedule without a
 * panel. panel_ota.c is the only user in the firmware; the names keep their panel_ota_ prefix.
 */
#ifndef PANEL_OTA_RETRY_H
#define PANEL_OTA_RETRY_H

#include <stdint.h>

#define PANEL_OTA_RETRY_MIN_MS 600000u   /* 10 min after the first failed check */
#define PANEL_OTA_RETRY_MAX_MS 21600000u /* 6 h: the longest a retry is ever put off */

/* Milliseconds until the next automatic check when 'fails' checks in a row have failed
 * (1 = the first failure, 0 = none yet). The delay doubles from PANEL_OTA_RETRY_MIN_MS and
 * stops at PANEL_OTA_RETRY_MAX_MS, or at the scheduled interval when that is shorter: a
 * failure never makes the panel look less often than it would have anyway.
 * interval_ms 0 = automatic checks are switched off; retries still use the 10 min .. 6 h
 * ladder, because the failure may have been a manual check. */
static inline uint32_t panel_ota_retry_delay_ms(uint32_t fails, uint32_t interval_ms)
{
    uint32_t cap = PANEL_OTA_RETRY_MAX_MS;
    if (interval_ms != 0u && interval_ms < cap) {
        cap = interval_ms;
    }
    if (cap < PANEL_OTA_RETRY_MIN_MS) {
        cap = PANEL_OTA_RETRY_MIN_MS;
    }
    uint32_t delay = PANEL_OTA_RETRY_MIN_MS;
    for (uint32_t i = 1u; i < fails && delay < cap; i++) {
        delay *= 2u;
    }
    return delay > cap ? cap : delay;
}

#endif /* PANEL_OTA_RETRY_H */
