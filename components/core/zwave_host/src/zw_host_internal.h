/*
 * zw_host_internal.h - between the host's link state machine (zw_host.c) and its start-up steps
 * (zw_host_steps.c, serial-api.md §4 and §5 steps 2-18). Private to zwave_host.
 */
#ifndef ZW_HOST_INTERNAL_H
#define ZW_HOST_INTERNAL_H

#include "zwave_host/zw_host.h"

/* What the result of a start-up command means for the sequence. */
typedef enum {
    ZW_STEP_NEXT = 0, /* done: the cursor has moved on */
    ZW_STEP_RETRY,    /* no usable answer: try the same command again (serial-api.md §5) */
    ZW_STEP_REFUSE    /* §4: the module will not do; *refusal says why */
} zw_step_verdict_t;

/* The start-up command at the cursor (h->step, h->sub), steps 2-18, skipping what does not apply
 * (the cursor moves past it): its request in *req, its payload in buf. False when the sequence
 * is complete - step 19, the link is up. */
bool zw_host_step_plan(zw_host_t *h, zw_request_t *req, uint8_t buf[ZW_PAYLOAD_MAX]);

/* The result of the command planned last: `payload` is its callback's (SoftReset: the
 * SerialAPIStarted's) or else its response's. Keeps the facts, checks §4, moves the cursor on
 * NEXT. `bad_answer` tells a RETRY for an answer that could not be used from one for none. */
zw_step_verdict_t zw_host_step_result(zw_host_t *h, zw_tx_outcome_t status, const uint8_t *payload,
                                      size_t len, zw_link_down_t *refusal, bool *bad_answer);

/* A SerialAPIStarted taken as step 12's answer (an unexpected restart, a recovery): keeps what
 * it says about Long Range. */
void zw_host_step_started(zw_host_t *h, const uint8_t *payload, size_t len);

#endif /* ZW_HOST_INTERNAL_H */
