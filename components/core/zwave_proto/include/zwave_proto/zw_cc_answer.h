/*
 * zw_cc_answer.h - command-classes.md §5: what the panel answers, and with what. Operates on one
 * already-unwrapped command addressed to the panel (endpoint 0); if it arrived through Multi
 * Channel or Supervision, re-wrapping the reply the same way (§1: "same encapsulation the
 * request had") is the caller's job - Supervision's own Report (status FF/00, §4.14) is also the
 * caller's job, using zw_cc_answer_result_t's NOT_HANDLED/NO_REPLY/REPLY to know which status.
 */
#ifndef ZW_CC_ANSWER_H
#define ZW_CC_ANSWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything §5's answers need that is not fixed by the doc itself. Manufacturer Specific:
 * §5 says FFFF/FFFF/FFFF "until the project has a manufacturer ID" - that placeholder is the
 * caller's to pass, not hardcoded here, so a real ID later needs no change to this file. Time/
 * Date: the panel's own clock (docs/features/35), read fresh before every call - this is call
 * context, not fixed identity, but travels in the same struct for one simple signature. */
typedef struct {
    uint8_t protocol_major, protocol_minor;   /* Version Report: from GetProtocolVersion */
    uint8_t firmware_major, firmware_minor;   /* Version Report: the panel's firmware version */
    uint16_t manufacturer_id, product_type, product_id; /* Manufacturer Specific Report */
    uint8_t zwave_plus_role_type;              /* Z-Wave Plus Info Report */
    uint8_t zwave_plus_node_type;
    uint16_t installer_icon, user_icon;
    bool clock_trusted;                          /* Time/Date Report: bit 7 "clock not set" is !this */
    uint8_t time_hour, time_minute, time_second;
    uint16_t date_year;
    uint8_t date_month, date_day;
} zw_cc_panel_identity_t;

typedef enum {
    ZW_CC_ANSWER_NOT_HANDLED = 0, /* §5 "anything else": a wrapping Supervision should say 00 */
    ZW_CC_ANSWER_NO_REPLY,         /* recognised, no command-level reply (Association Set/Remove):
                                       a wrapping Supervision should say FF */
    ZW_CC_ANSWER_REPLY,            /* a reply command was written to 'out'/'out_len' */
} zw_cc_answer_result_t;

/* 'command' is one already-unwrapped command (CC | command | parameters) addressed to the panel. */
zw_cc_answer_result_t zw_cc_panel_answer(const uint8_t *command, size_t command_len,
                                          const zw_cc_panel_identity_t *identity, uint8_t *out,
                                          size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_ANSWER_H */
