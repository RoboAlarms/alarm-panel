/*
 * zw_cc_notification.h - Notification (command-classes.md §4.5): the single biggest CC a
 * security sensor uses. Event tables are zones.md's job; this file is only the wire format.
 */
#ifndef ZW_CC_NOTIFICATION_H
#define ZW_CC_NOTIFICATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_GET    0x01u
#define ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_REPORT 0x02u
#define ZW_CMD_NOTIFICATION_GET                    0x04u
#define ZW_CMD_NOTIFICATION_REPORT                 0x05u
#define ZW_CMD_NOTIFICATION_SET                    0x06u
#define ZW_CMD_NOTIFICATION_SUPPORTED_GET          0x07u
#define ZW_CMD_NOTIFICATION_SUPPORTED_REPORT       0x08u

#define ZW_NOTIFICATION_TYPE_SMOKE            0x01u
#define ZW_NOTIFICATION_TYPE_CO               0x02u
#define ZW_NOTIFICATION_TYPE_HEAT             0x04u
#define ZW_NOTIFICATION_TYPE_WATER            0x05u
#define ZW_NOTIFICATION_TYPE_ACCESS_CONTROL   0x06u
#define ZW_NOTIFICATION_TYPE_HOME_SECURITY    0x07u
#define ZW_NOTIFICATION_TYPE_POWER_MANAGEMENT 0x08u
#define ZW_NOTIFICATION_TYPE_SYSTEM           0x09u
#define ZW_NOTIFICATION_TYPE_EMERGENCY        0x0Au
#define ZW_NOTIFICATION_TYPE_WEATHER          0x10u
#define ZW_NOTIFICATION_TYPE_GAS              0x12u

#define ZW_NOTIFICATION_EVENT_IDLE       0x00u
#define ZW_NOTIFICATION_STATUS_ON        0xFFu
#define ZW_NOTIFICATION_STATUS_OFF       0x00u
#define ZW_NOTIFICATION_STATUS_NO_PENDING 0xFEu /* answering a Get: nothing pending right now */

size_t zw_cc_notification_supported_get_build(uint8_t *out, size_t cap);
bool zw_cc_notification_supported_get_parse(const uint8_t *p, size_t len);

/* flags: bit 7 = the node also sends v1 alarms, bits 4..0 = the type mask's length in bytes.
 * type_mask: bit 0 of byte 0 unused, bit t = notification type t. */
typedef struct {
    bool sends_v1_alarms_too;
    const uint8_t *type_mask;
    size_t type_mask_len;
} zw_cc_notification_supported_report_t;
size_t zw_cc_notification_supported_report_build(uint8_t *out, size_t cap,
                                                  const zw_cc_notification_supported_report_t *in);
bool zw_cc_notification_supported_report_parse(const uint8_t *p, size_t len,
                                                zw_cc_notification_supported_report_t *out);
bool zw_cc_notification_type_supported(const uint8_t *type_mask, size_t type_mask_len, uint8_t type);

typedef struct {
    uint8_t notification_type;
} zw_cc_notification_event_supported_get_t;
size_t zw_cc_notification_event_supported_get_build(uint8_t *out, size_t cap,
                                                     const zw_cc_notification_event_supported_get_t *in);
bool zw_cc_notification_event_supported_get_parse(const uint8_t *p, size_t len,
                                                   zw_cc_notification_event_supported_get_t *out);

/* mask_length (bits 4..0 of its own byte); event_mask: bit 0 unused, bit e = event e. */
typedef struct {
    uint8_t notification_type;
    const uint8_t *event_mask;
    size_t event_mask_len;
} zw_cc_notification_event_supported_report_t;
size_t zw_cc_notification_event_supported_report_build(
    uint8_t *out, size_t cap, const zw_cc_notification_event_supported_report_t *in);
bool zw_cc_notification_event_supported_report_parse(
    const uint8_t *p, size_t len, zw_cc_notification_event_supported_report_t *out);

/* the panel always asks `00 . type . 00` ("the current state of this type"); has_event is v3+. */
typedef struct {
    uint8_t v1_alarm_type;
    uint8_t notification_type;
    bool has_event;
    uint8_t event;
} zw_cc_notification_get_t;
size_t zw_cc_notification_get_build(uint8_t *out, size_t cap, const zw_cc_notification_get_t *in);
bool zw_cc_notification_get_parse(const uint8_t *p, size_t len, zw_cc_notification_get_t *out);

typedef struct {
    uint8_t notification_type;
    uint8_t status; /* ZW_NOTIFICATION_STATUS_ON / _OFF */
} zw_cc_notification_set_t;
size_t zw_cc_notification_set_build(uint8_t *out, size_t cap, const zw_cc_notification_set_t *in);
bool zw_cc_notification_set_parse(const uint8_t *p, size_t len, zw_cc_notification_set_t *out);

/* A report shorter than 7 payload bytes (is_v1 true) means only v1_alarm_type/v1_alarm_level mean
 * anything (§4.5: "a v1 alarm"); the rest of the fields are then 0/NULL/false. event_params and
 * the sequence number are read only when the command is long enough to carry them. */
typedef struct {
    bool is_v1;
    uint8_t v1_alarm_type;
    uint8_t v1_alarm_level;
    uint8_t status;              /* v2+: FF on, 00 off, FE no pending */
    uint8_t notification_type;
    uint8_t event;                /* 00 = idle */
    const uint8_t *event_params;
    size_t event_params_len;
    bool has_sequence_number;
    uint8_t sequence_number;
} zw_cc_notification_report_t;
size_t zw_cc_notification_report_build(uint8_t *out, size_t cap, const zw_cc_notification_report_t *in);
bool zw_cc_notification_report_parse(const uint8_t *p, size_t len, zw_cc_notification_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_NOTIFICATION_H */
