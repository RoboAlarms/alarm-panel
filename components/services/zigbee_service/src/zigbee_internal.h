/*
 * zigbee_internal.h - what zigbee_service's files share (BL-143, ZB-M6): the service's panel
 * (zbz_panel, zigbee_service.c) for the console (zigbee_console.c) and the developer link
 * (zigbee_dev.c).
 */
#pragma once

#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zigbee_service.h"

#if CONFIG_ALARM_ZSRC_ZIGBEE

#include "zigbee_zones/zbz_panel.h"

typedef struct {
    uint32_t stores, store_failures; /* NVS writes of zbpanel */
    uint32_t events, events_failed;  /* events logged */
    uint32_t requests, request_timeouts;
    uint32_t ace_posts, ace_failed;  /* keypad and fob commands run through the alarm task */
    uint32_t nvs_free;               /* NVS's free entries as last read (zbz_panel_nvs_free) */
    uint32_t stack_free;             /* the zigbee task's stack never used, bytes (its high-water
                                      * mark, uxTaskGetStackHighWaterMark) */
    uint32_t stack_size;             /* that task's stack, bytes */
    /* radio_port's queue since start (radio_port_queue_counts): events of any kind dropped, and
     * alarm-path events (a zone status, an enroll request, an ACE command) lost */
    uint32_t radio_dropped, alarm_lost;
} zbsvc_stats_t;

/* The service runs (its source registered, its task started). */
bool zbsvc_started(void);
/* The Zigbee stack is up (a restart stops it: only through the gated RESTART request, ZB21). */
bool zbsvc_stack_up(void);
void zbsvc_stats(zbsvc_stats_t *out);
/* The panel and the source: read them with zone_service_lock() held. NULL before start. */
zbz_panel_t *zbsvc_panel(void);
zsrc_zb_t *zbsvc_source(void);
uint64_t zbsvc_now_ms(void);
/* A request on the zigbee task, waiting for its answer: 2 s while it waits to be taken, 2 s more
 * once the task has taken it (its answer is the truth, never a BUSY while it goes ahead); BUSY
 * when none came even so. The
 * caller decided `allowed` (zbz_panel_request's) and, for RESET, checked the installer code. */
ui_zb_req_result_t zbsvc_submit(const ui_zb_request_t *req, bool allowed);

/* zba_receive's POST (zbz_ace.h): the keypad's or fob's command for the alarm task, copied here
 * (the zone lock held, in the zigbee task) and run after unlocking - alarm_service_command with
 * the keypad's code, alarm_service_command_as for a fob's user - its answer handed back with
 * zbz_panel_ace_result. The code is wiped once posted. False: no room (ZBA_PENDING_MAX wait
 * already). zbz_panel_port_t.ace_post (ctx unused). */
bool zbsvc_ace_post(void *ctx, const zba_post_t *post);

/* RESET's installer code (ZB-017), checked with the engine as `src` - a wrong one counts toward
 * its lockout - and mapped with zbz_reset_code_result; `code` is wiped. Not from the zigbee task,
 * never under the zone lock. */
ui_zb_req_result_t zbsvc_reset_code(const alarm_source_t *src, char code[ALARM_CODE_MAX_LEN + 1]);

/* The bench's tools (zigbee_bench.c; devices.md §12, testing.md §2 and §7). The capture and the
 * report trace run in the zigbee task with the zone lock held; the signature is printed from the
 * console (it takes the lock itself). */
void zbsvc_sig_capture(const zsrc_zb_report_t *r);
int zbsvc_print_signature(uint16_t id);
void zbsvc_trace_set(bool on);
bool zbsvc_trace_on(void);
void zbsvc_trace_out(const char *what, uint64_t ieee, uint8_t ep, uint32_t a, uint32_t tag);
void zbsvc_trace_zcl(char dir, uint64_t ieee, uint8_t ep, const zsrc_zb_zcl_t *z, uint32_t tag);
void zbsvc_trace_report(uint64_t now, const zsrc_zb_report_t *r);

/* The developer link's words (zigbee_dev.c), shared with the console. */
typedef enum {
    ZBSVC_W_RADIO = 0, /* ui_zb_radio_t */
    ZBSVC_W_PAUSE,     /* ui_zb_pause_t */
    ZBSVC_W_DEVICE,    /* ui_zb_dev_state_t */
    ZBSVC_W_KIND,      /* ui_zb_kind_t */
    ZBSVC_W_POWER,     /* ui_zb_power_t */
    ZBSVC_W_SIGNAL,    /* ui_zb_signal_t */
    ZBSVC_W_JOIN,      /* ui_zb_join_t */
    ZBSVC_W_OP,        /* ui_zb_op_t */
    ZBSVC_W_STEP,      /* ui_zb_step_t */
    ZBSVC_W_RESULT,    /* ui_zb_result_t */
    ZBSVC_W_ANSWER,    /* ui_zb_req_result_t */
    ZBSVC_W_COUNT,
} zbsvc_words_t;
/* "" for a value it has no word for. */
const char *zbsvc_word(zbsvc_words_t table, uint8_t v);
/* 16 hex digits, most significant first (':' '-' allowed between them), into *out; false else. */
bool zbsvc_parse_ieee(const char *s, uint64_t *out);

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */
