/*
 * ota_selftest.h - the self-test after an update (OTA-005): its times, the verdict on one part of
 * the panel, and what the update task does about the verdicts (docs/features/13; a master's
 * keypads, docs/features/11 LINK-041).
 *
 * Plain C11, header-only like ota_retry.h, so test/test_fw.c checks the rule without a panel.
 * panel_ota.c is the only user in the firmware (panel_ota.h includes this, so the times keep
 * their panel_ota_ names for everyone).
 */
#ifndef PANEL_OTA_SELFTEST_H
#define PANEL_OTA_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

#define PANEL_OTA_CONFIRM_MS 60000u /* shortest probation after an update (OTA-005) */

/* Probation is extended while every required part still reports but hasn't been reporting
 * long enough yet (a subsystem that comes up late). It never lasts beyond this. */
#define PANEL_OTA_CONFIRM_MAX_MS 180000u

/* The heartbeat contract for panel_ota_healthy() (OTA-005). */
#define PANEL_OTA_HEALTH_PERIOD_MS 5000u  /* report at least this often, from the working task */
#define PANEL_OTA_HEALTH_STALE_MS  15000u /* silent for longer = that part is not running */
#define PANEL_OTA_HEALTH_SPAN_MS   30000u /* reports must cover this much before confirming */
#define PANEL_OTA_HEALTH_GRACE_MS  30000u /* after the start: parts may still be coming up */

/* A master's keypads (PANEL_OTA_OK_KEYPADS, LINK-041) come back only after this panel has
 * restarted and joined its Wi-Fi again, and each keypad has noticed and handshaken again: well
 * past PANEL_OTA_CONFIRM_MS when the network is slow. One keypad's answer that lasts across its
 * next heartbeat (every 10 s) proves the part, so a keypad that first answers up to
 * PANEL_OTA_CONFIRM_MAX_MS - PANEL_OTA_KEYPADS_SPAN_MS (170 s) after the start still counts. */
#define PANEL_OTA_KEYPADS_SPAN_MS 10000u

/* One required part, as its heartbeats show it to the update task. */
typedef struct {
    bool reported;       /* it reported at least once since the start */
    bool fresh;          /* its last report is recent (PANEL_OTA_HEALTH_STALE_MS) and moving */
    uint32_t covered_ms; /* from its first report to its last */
    uint32_t span_ms;    /* how much its reports must cover: PANEL_OTA_HEALTH_SPAN_MS, or
                            PANEL_OTA_KEYPADS_SPAN_MS for a master's keypads */
    bool may_wait;       /* a part that needs another device back (a master's keypads), on
                            probation before PANEL_OTA_CONFIRM_MAX_MS: not answering yet, or quiet
                            for a while, keeps the self-test waiting instead of failing it */
} panel_ota_part_t;

typedef enum {
    PANEL_OTA_PART_OK = 0,   /* reporting, long enough */
    PANEL_OTA_PART_UNPROVEN, /* not proven yet: the self-test waits, up to its deadline */
    PANEL_OTA_PART_MISSING,  /* not running: the self-test fails at once; outside it, a fault */
} panel_ota_part_verdict_t;

static inline panel_ota_part_verdict_t panel_ota_part_judge(const panel_ota_part_t *p)
{
    if (!p->reported || !p->fresh) {
        return p->may_wait ? PANEL_OTA_PART_UNPROVEN : PANEL_OTA_PART_MISSING;
    }
    return p->covered_ms < p->span_ms ? PANEL_OTA_PART_UNPROVEN : PANEL_OTA_PART_OK;
}

typedef enum {
    PANEL_OTA_SELFTEST_WAIT = 0,  /* go on waiting */
    PANEL_OTA_SELFTEST_CONFIRM,   /* every required part runs: the new image is kept */
    PANEL_OTA_SELFTEST_ROLL_BACK, /* the previous image returns */
} panel_ota_selftest_t;

/* The self-test's step at now: missing and unproven are the required parts' verdicts as bit
 * masks; other_wait is something else it waits for (a remote install's confirmation). Nothing is
 * decided before until (the start + PANEL_OTA_CONFIRM_MS). Then it confirms once nothing is
 * missing or unproven; a missing part rolls back at once; an unproven one is waited for until
 * deadline (the start + PANEL_OTA_CONFIRM_MAX_MS), then rolls back. */
static inline panel_ota_selftest_t panel_ota_selftest_step(uint32_t missing, uint32_t unproven,
                                                           bool other_wait, uint64_t now,
                                                           uint64_t until, uint64_t deadline)
{
    if (now < until) {
        return PANEL_OTA_SELFTEST_WAIT;
    }
    if (missing == 0u && unproven == 0u && !other_wait) {
        return PANEL_OTA_SELFTEST_CONFIRM;
    }
    if (missing == 0u && now < deadline) {
        return PANEL_OTA_SELFTEST_WAIT;
    }
    return PANEL_OTA_SELFTEST_ROLL_BACK;
}

#endif /* PANEL_OTA_SELFTEST_H */
