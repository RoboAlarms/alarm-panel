/*
 * zw_cc_supervision.h - Supervision (command-classes.md §4.14): a Get wraps one inner command and
 * asks for a Report acknowledging how it went. Used directly (build a Get/Report yourself) or via
 * zw_cc_encap.h's zw_cc_wrap_supervision()/zw_cc_unwrap() for the §1 encapsulation itself.
 */
#ifndef ZW_CC_SUPERVISION_H
#define ZW_CC_SUPERVISION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_CMD_SUPERVISION_GET    0x01u
#define ZW_CMD_SUPERVISION_REPORT 0x02u

#define ZW_SUPERVISION_STATUS_NO_SUPPORT 0x00u
#define ZW_SUPERVISION_STATUS_WORKING    0x01u
#define ZW_SUPERVISION_STATUS_FAIL       0x02u
#define ZW_SUPERVISION_STATUS_SUCCESS    0xFFu

#define ZW_SUPERVISION_SESSION_ID_MASK 0x3Fu /* bits 5..0 */

typedef struct {
    bool status_updates_wanted; /* bit 7 */
    uint8_t session_id;          /* bits 5..0 */
    const uint8_t *command;
    size_t command_len;
} zw_cc_supervision_get_t;
size_t zw_cc_supervision_get_build(uint8_t *out, size_t cap, const zw_cc_supervision_get_t *in);
bool zw_cc_supervision_get_parse(const uint8_t *p, size_t len, zw_cc_supervision_get_t *out);

typedef struct {
    bool more_updates_follow; /* bit 7 */
    bool wake_up_request;      /* bit 6 */
    uint8_t session_id;         /* bits 5..0 */
    uint8_t status;              /* ZW_SUPERVISION_STATUS_* */
    uint8_t duration;
} zw_cc_supervision_report_t;
size_t zw_cc_supervision_report_build(uint8_t *out, size_t cap, const zw_cc_supervision_report_t *in);
bool zw_cc_supervision_report_parse(const uint8_t *p, size_t len, zw_cc_supervision_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_SUPERVISION_H */
