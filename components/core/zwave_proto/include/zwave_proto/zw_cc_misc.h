/*
 * zw_cc_misc.h - Indicator, Device Reset Locally, Configuration, Central Scene, Alarm Sensor
 * (legacy), Hail (legacy), Time and Application Status (command-classes.md §4.18-§4.25).
 */
#ifndef ZW_CC_MISC_H
#define ZW_CC_MISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- §4.18 Indicator (87, phase 2) --------------------------------------------------------------- */
#define ZW_CMD_INDICATOR_SET             0x01u
#define ZW_CMD_INDICATOR_SUPPORTED_GET   0x04u
#define ZW_CMD_INDICATOR_SUPPORTED_REPORT 0x05u
#define ZW_INDICATOR_ID_FIRST 0x00u
#define ZW_INDICATOR_ID_NODE_IDENTIFY 0x50u

/* One (indicator ID, property ID, value) entry of a v2+ Set. */
typedef struct {
    uint8_t indicator_id;
    uint8_t property_id;
    uint8_t value;
} zw_cc_indicator_entry_t;
/* Reads entry 'index' (0-based) out of a v2+ Set's raw entries view. */
bool zw_cc_indicator_entry_get(const uint8_t *entries, size_t entries_len, size_t index,
                                zw_cc_indicator_entry_t *out);

/* Distinguished purely by length: a v1 Set is exactly 1 payload byte (has_entries false); v2+ is
 * at least 2 (indicator0_value, always 00, then a count byte and that many 3-byte entries). */
typedef struct {
    bool has_entries;
    uint8_t v1_value;
    uint8_t indicator0_value;
    const uint8_t *entries; /* entry_count * 3 bytes */
    uint8_t entry_count;
} zw_cc_indicator_set_t;
size_t zw_cc_indicator_set_build(uint8_t *out, size_t cap, const zw_cc_indicator_set_t *in);
bool zw_cc_indicator_set_parse(const uint8_t *p, size_t len, zw_cc_indicator_set_t *out);

typedef struct {
    uint8_t indicator_id; /* 00 = first */
} zw_cc_indicator_supported_get_t;
size_t zw_cc_indicator_supported_get_build(uint8_t *out, size_t cap,
                                            const zw_cc_indicator_supported_get_t *in);
bool zw_cc_indicator_supported_get_parse(const uint8_t *p, size_t len,
                                          zw_cc_indicator_supported_get_t *out);

typedef struct {
    uint8_t indicator_id;
    uint8_t next_indicator_id;
    const uint8_t *property_mask; /* mask_length (bits 4..0) bytes */
    size_t property_mask_len;
} zw_cc_indicator_supported_report_t;
size_t zw_cc_indicator_supported_report_build(uint8_t *out, size_t cap,
                                               const zw_cc_indicator_supported_report_t *in);
bool zw_cc_indicator_supported_report_parse(const uint8_t *p, size_t len,
                                             zw_cc_indicator_supported_report_t *out);

/* ---- §4.19 Device Reset Locally (5A): the device left the network by hand. --------------------- */
#define ZW_CMD_DEVICE_RESET_LOCALLY_NOTIFICATION 0x01u
size_t zw_cc_device_reset_locally_notification_build(uint8_t *out, size_t cap);
bool zw_cc_device_reset_locally_notification_parse(const uint8_t *p, size_t len);

/* ---- §4.20 Configuration (70, phase 2, quirks only) ---------------------------------------------- */
#define ZW_CMD_CONFIGURATION_SET    0x04u
#define ZW_CMD_CONFIGURATION_GET    0x05u
#define ZW_CMD_CONFIGURATION_REPORT 0x06u

typedef struct {
    uint8_t parameter;
    bool restore_default; /* flags bit 7 */
    uint8_t size;           /* 1, 2 or 4 */
    int32_t value;
} zw_cc_configuration_set_t;
size_t zw_cc_configuration_set_build(uint8_t *out, size_t cap, const zw_cc_configuration_set_t *in);
bool zw_cc_configuration_set_parse(const uint8_t *p, size_t len, zw_cc_configuration_set_t *out);

typedef struct {
    uint8_t parameter;
} zw_cc_configuration_get_t;
size_t zw_cc_configuration_get_build(uint8_t *out, size_t cap, const zw_cc_configuration_get_t *in);
bool zw_cc_configuration_get_parse(const uint8_t *p, size_t len, zw_cc_configuration_get_t *out);

typedef struct {
    uint8_t parameter;
    uint8_t size;
    int32_t value;
} zw_cc_configuration_report_t;
size_t zw_cc_configuration_report_build(uint8_t *out, size_t cap, const zw_cc_configuration_report_t *in);
bool zw_cc_configuration_report_parse(const uint8_t *p, size_t len, zw_cc_configuration_report_t *out);

/* ---- §4.21 Central Scene (5B, phase 2): key fobs and panic buttons. ------------------------------ */
#define ZW_CMD_CENTRAL_SCENE_NOTIFICATION 0x03u
#define ZW_CENTRAL_SCENE_KEY_PRESSED_1X  0u
#define ZW_CENTRAL_SCENE_KEY_RELEASED    1u
#define ZW_CENTRAL_SCENE_KEY_HELD        2u

typedef struct {
    uint8_t sequence_number;
    uint8_t key_attribute; /* bits 2..0: ZW_CENTRAL_SCENE_KEY_*, or 3..6 = pressed 2..5 times */
    uint8_t scene_number;
} zw_cc_central_scene_notification_t;
size_t zw_cc_central_scene_notification_build(uint8_t *out, size_t cap,
                                               const zw_cc_central_scene_notification_t *in);
bool zw_cc_central_scene_notification_parse(const uint8_t *p, size_t len,
                                             zw_cc_central_scene_notification_t *out);

/* ---- §4.22 Alarm Sensor (9C, legacy, phase 2) ------------------------------------------------------ */
#define ZW_CMD_ALARM_SENSOR_GET    0x01u
#define ZW_CMD_ALARM_SENSOR_REPORT 0x02u
#define ZW_ALARM_SENSOR_TYPE_GENERAL 0x00u
#define ZW_ALARM_SENSOR_STATE_IDLE   0x00u
#define ZW_ALARM_SENSOR_STATE_ALARM  0xFFu

typedef struct {
    uint8_t sensor_type;
} zw_cc_alarm_sensor_get_t;
size_t zw_cc_alarm_sensor_get_build(uint8_t *out, size_t cap, const zw_cc_alarm_sensor_get_t *in);
bool zw_cc_alarm_sensor_get_parse(const uint8_t *p, size_t len, zw_cc_alarm_sensor_get_t *out);

typedef struct {
    uint8_t source_node;
    uint8_t sensor_type;
    uint8_t state; /* 00 idle, 01..64 severity, FF alarm */
    uint16_t seconds;
} zw_cc_alarm_sensor_report_t;
size_t zw_cc_alarm_sensor_report_build(uint8_t *out, size_t cap, const zw_cc_alarm_sensor_report_t *in);
bool zw_cc_alarm_sensor_report_parse(const uint8_t *p, size_t len, zw_cc_alarm_sensor_report_t *out);

/* Supported Get `9C 03`; Supported Report `9C 04` · `number of mask bytes` · mask. Unlike Binary
 * Sensor's, bit 0 of byte 0 is a real type (general purpose): bit t = sensor type t. A report
 * shorter than its own count is refused; bytes after the mask are ignored. */
#define ZW_CMD_ALARM_SENSOR_SUPPORTED_GET    0x03u
#define ZW_CMD_ALARM_SENSOR_SUPPORTED_REPORT 0x04u
#define ZW_ALARM_SENSOR_TYPE_SMOKE 0x01u
#define ZW_ALARM_SENSOR_TYPE_CO    0x02u
#define ZW_ALARM_SENSOR_TYPE_CO2   0x03u
#define ZW_ALARM_SENSOR_TYPE_HEAT  0x04u
#define ZW_ALARM_SENSOR_TYPE_WATER 0x05u

size_t zw_cc_alarm_sensor_supported_get_build(uint8_t *out, size_t cap);
bool zw_cc_alarm_sensor_supported_get_parse(const uint8_t *p, size_t len);

typedef struct {
    const uint8_t *mask; /* into the parsed buffer; NULL when mask_len is 0 */
    size_t mask_len;
} zw_cc_alarm_sensor_supported_report_t;
size_t zw_cc_alarm_sensor_supported_report_build(uint8_t *out, size_t cap,
                                                  const zw_cc_alarm_sensor_supported_report_t *in);
bool zw_cc_alarm_sensor_supported_report_parse(const uint8_t *p, size_t len,
                                                zw_cc_alarm_sensor_supported_report_t *out);
bool zw_cc_alarm_sensor_type_supported(const uint8_t *mask, size_t mask_len, uint8_t sensor_type);

/* ---- §4.23 Hail (82, legacy): "something changed, ask me" - no payload either way. --------------- */
#define ZW_CMD_HAIL 0x01u
size_t zw_cc_hail_build(uint8_t *out, size_t cap);
bool zw_cc_hail_parse(const uint8_t *p, size_t len);

/* ---- §4.24 Time (8A, phase 2, answered only): the panel's own clock, local time. ------------------- */
#define ZW_CMD_TIME_GET    0x01u
#define ZW_CMD_TIME_REPORT 0x02u
#define ZW_CMD_DATE_GET    0x03u
#define ZW_CMD_DATE_REPORT 0x04u

size_t zw_cc_time_get_build(uint8_t *out, size_t cap);
bool zw_cc_time_get_parse(const uint8_t *p, size_t len);

typedef struct {
    bool clock_not_set; /* bit 7 of hour */
    uint8_t hour;         /* bits 4..0 */
    uint8_t minute;
    uint8_t second;
} zw_cc_time_report_t;
size_t zw_cc_time_report_build(uint8_t *out, size_t cap, const zw_cc_time_report_t *in);
bool zw_cc_time_report_parse(const uint8_t *p, size_t len, zw_cc_time_report_t *out);

size_t zw_cc_date_get_build(uint8_t *out, size_t cap);
bool zw_cc_date_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
} zw_cc_date_report_t;
size_t zw_cc_date_report_build(uint8_t *out, size_t cap, const zw_cc_date_report_t *in);
bool zw_cc_date_report_parse(const uint8_t *p, size_t len, zw_cc_date_report_t *out);

/* ---- §4.25 Application Status (22, received only): a node's word that it did not carry out a
 * request now - busy, or refused. ------------------------------------------------------------------ */
#define ZW_CMD_APPLICATION_BUSY             0x01u
#define ZW_CMD_APPLICATION_REJECTED_REQUEST 0x02u
#define ZW_APPLICATION_BUSY_TRY_LATER       0x00u /* try again later */
#define ZW_APPLICATION_BUSY_WAIT            0x01u /* try again in `wait_s` seconds */
#define ZW_APPLICATION_BUSY_QUEUED          0x02u /* queued: carried out later */

/* Application Busy: `status` · `wait time` (seconds, what ZW_APPLICATION_BUSY_WAIT names). One
 * that ends after its status byte parses with wait_s 0. */
typedef struct {
    uint8_t status;
    uint8_t wait_s;
} zw_cc_application_busy_t;
size_t zw_cc_application_busy_build(uint8_t *out, size_t cap, const zw_cc_application_busy_t *in);
bool zw_cc_application_busy_parse(const uint8_t *p, size_t len, zw_cc_application_busy_t *out);

/* Application Rejected Request: `status` (00; another value is reserved and kept as it came). */
typedef struct {
    uint8_t status;
} zw_cc_application_rejected_request_t;
size_t zw_cc_application_rejected_request_build(uint8_t *out, size_t cap,
                                                const zw_cc_application_rejected_request_t *in);
bool zw_cc_application_rejected_request_parse(const uint8_t *p, size_t len,
                                              zw_cc_application_rejected_request_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_MISC_H */
