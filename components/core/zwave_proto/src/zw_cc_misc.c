/*
 * zw_cc_misc.c - Indicator, Device Reset Locally, Configuration, Central Scene, Alarm Sensor,
 * Hail, Time and Application Status (see zw_cc_misc.h, command-classes.md §4.18-§4.25).
 */
#include "zwave_proto/zw_cc_misc.h"

#include "zw_cc_internal.h"

/* ---- §4.18 Indicator ------------------------------------------------------------------------------ */

bool zw_cc_indicator_entry_get(const uint8_t *entries, size_t entries_len, size_t index,
                                zw_cc_indicator_entry_t *out)
{
    if (entries == NULL || out == NULL) {
        return false;
    }
    if (index >= entries_len / 3u) {
        return false;
    }
    const size_t off = index * 3u;
    out->indicator_id = entries[off];
    out->property_id = entries[off + 1u];
    out->value = entries[off + 2u];
    return true;
}

size_t zw_cc_indicator_set_build(uint8_t *out, size_t cap, const zw_cc_indicator_set_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    if (!in->has_entries) {
        if (cap < 3u) {
            return 0u;
        }
        out[0] = (uint8_t)ZW_CC_INDICATOR;
        out[1] = ZW_CMD_INDICATOR_SET;
        out[2] = in->v1_value;
        return 3u;
    }
    if (in->entry_count > 0x1Fu || (in->entries == NULL && in->entry_count > 0u)) {
        return 0u;
    }
    const size_t total = 4u + (size_t)in->entry_count * 3u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_INDICATOR;
    out[1] = ZW_CMD_INDICATOR_SET;
    out[2] = in->indicator0_value;
    out[3] = (uint8_t)(in->entry_count & 0x1Fu);
    for (size_t i = 0; i < (size_t)in->entry_count * 3u; i++) {
        out[4u + i] = in->entries[i];
    }
    return total;
}

bool zw_cc_indicator_set_parse(const uint8_t *p, size_t len, zw_cc_indicator_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_INDICATOR, ZW_CMD_INDICATOR_SET, 3u) || out == NULL) {
        return false;
    }
    if (len == 3u) {
        out->has_entries = false;
        out->v1_value = p[2];
        out->indicator0_value = 0u;
        out->entries = NULL;
        out->entry_count = 0u;
        return true;
    }
    /* v2+ needs indicator0_value plus a count byte. */
    if (len < 4u) {
        return false;
    }
    const uint8_t count = (uint8_t)(p[3] & 0x1Fu);
    const size_t need = 4u + (size_t)count * 3u;
    if (len < need) {
        return false;
    }
    out->has_entries = true;
    out->v1_value = 0u;
    out->indicator0_value = p[2];
    out->entry_count = count;
    out->entries = count > 0u ? &p[4] : NULL;
    return true;
}

size_t zw_cc_indicator_supported_get_build(uint8_t *out, size_t cap,
                                            const zw_cc_indicator_supported_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_INDICATOR;
    out[1] = ZW_CMD_INDICATOR_SUPPORTED_GET;
    out[2] = in->indicator_id;
    return 3u;
}

bool zw_cc_indicator_supported_get_parse(const uint8_t *p, size_t len,
                                          zw_cc_indicator_supported_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_INDICATOR, ZW_CMD_INDICATOR_SUPPORTED_GET, 3u) || out == NULL) {
        return false;
    }
    out->indicator_id = p[2];
    return true;
}

size_t zw_cc_indicator_supported_report_build(uint8_t *out, size_t cap,
                                               const zw_cc_indicator_supported_report_t *in)
{
    if (out == NULL || in == NULL || (in->property_mask == NULL && in->property_mask_len > 0u) ||
        in->property_mask_len > 0x1Fu) {
        return 0u;
    }
    const size_t total = 5u + in->property_mask_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_INDICATOR;
    out[1] = ZW_CMD_INDICATOR_SUPPORTED_REPORT;
    out[2] = in->indicator_id;
    out[3] = in->next_indicator_id;
    out[4] = (uint8_t)(in->property_mask_len & 0x1Fu);
    for (size_t i = 0; i < in->property_mask_len; i++) {
        out[5u + i] = in->property_mask[i];
    }
    return total;
}

bool zw_cc_indicator_supported_report_parse(const uint8_t *p, size_t len,
                                             zw_cc_indicator_supported_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_INDICATOR, ZW_CMD_INDICATOR_SUPPORTED_REPORT, 5u) ||
        out == NULL) {
        return false;
    }
    const size_t mlen = (size_t)(p[4] & 0x1Fu);
    if (len < 5u + mlen) {
        return false;
    }
    out->indicator_id = p[2];
    out->next_indicator_id = p[3];
    out->property_mask_len = mlen;
    out->property_mask = mlen > 0u ? &p[5] : NULL;
    return true;
}

/* ---- §4.19 Device Reset Locally -------------------------------------------------------------------- */

size_t zw_cc_device_reset_locally_notification_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_DEVICE_RESET_LOCALLY;
    out[1] = ZW_CMD_DEVICE_RESET_LOCALLY_NOTIFICATION;
    return 2u;
}

bool zw_cc_device_reset_locally_notification_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_DEVICE_RESET_LOCALLY,
                             ZW_CMD_DEVICE_RESET_LOCALLY_NOTIFICATION, 2u);
}

/* ---- §4.20 Configuration --------------------------------------------------------------------------- */

size_t zw_cc_configuration_set_build(uint8_t *out, size_t cap, const zw_cc_configuration_set_t *in)
{
    if (out == NULL || in == NULL || (in->size != 1u && in->size != 2u && in->size != 4u)) {
        return 0u;
    }
    const size_t total = 4u + (unsigned)in->size;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_CONFIGURATION;
    out[1] = ZW_CMD_CONFIGURATION_SET;
    out[2] = in->parameter;
    out[3] = (uint8_t)((in->restore_default ? 0x80u : 0x00u) | (in->size & 0x07u));
    if (!zwi_signed_encode(&out[4], in->size, in->value)) {
        return 0u;
    }
    return total;
}

bool zw_cc_configuration_set_parse(const uint8_t *p, size_t len, zw_cc_configuration_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_CONFIGURATION, ZW_CMD_CONFIGURATION_SET, 4u) || out == NULL) {
        return false;
    }
    const uint8_t flags = p[3];
    const uint8_t size = (uint8_t)(flags & 0x07u);
    if (size != 1u && size != 2u && size != 4u) {
        return false;
    }
    if (len < 4u + (size_t)size) {
        return false;
    }
    out->parameter = p[2];
    out->restore_default = ((unsigned)flags & 0x80u) != 0u;
    out->size = size;
    return zwi_signed_decode(&p[4], size, &out->value);
}

size_t zw_cc_configuration_get_build(uint8_t *out, size_t cap, const zw_cc_configuration_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_CONFIGURATION;
    out[1] = ZW_CMD_CONFIGURATION_GET;
    out[2] = in->parameter;
    return 3u;
}

bool zw_cc_configuration_get_parse(const uint8_t *p, size_t len, zw_cc_configuration_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_CONFIGURATION, ZW_CMD_CONFIGURATION_GET, 3u) || out == NULL) {
        return false;
    }
    out->parameter = p[2];
    return true;
}

size_t zw_cc_configuration_report_build(uint8_t *out, size_t cap, const zw_cc_configuration_report_t *in)
{
    if (out == NULL || in == NULL || (in->size != 1u && in->size != 2u && in->size != 4u)) {
        return 0u;
    }
    const size_t total = 4u + (unsigned)in->size;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_CONFIGURATION;
    out[1] = ZW_CMD_CONFIGURATION_REPORT;
    out[2] = in->parameter;
    out[3] = (uint8_t)(in->size & 0x07u);
    if (!zwi_signed_encode(&out[4], in->size, in->value)) {
        return 0u;
    }
    return total;
}

bool zw_cc_configuration_report_parse(const uint8_t *p, size_t len, zw_cc_configuration_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_CONFIGURATION, ZW_CMD_CONFIGURATION_REPORT, 4u) || out == NULL) {
        return false;
    }
    const uint8_t size = (uint8_t)(p[3] & 0x07u);
    if (size != 1u && size != 2u && size != 4u) {
        return false;
    }
    if (len < 4u + (size_t)size) {
        return false;
    }
    out->parameter = p[2];
    out->size = size;
    return zwi_signed_decode(&p[4], size, &out->value);
}

/* ---- §4.21 Central Scene --------------------------------------------------------------------------- */

size_t zw_cc_central_scene_notification_build(uint8_t *out, size_t cap,
                                               const zw_cc_central_scene_notification_t *in)
{
    if (out == NULL || in == NULL || cap < 5u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_CENTRAL_SCENE;
    out[1] = ZW_CMD_CENTRAL_SCENE_NOTIFICATION;
    out[2] = in->sequence_number;
    out[3] = in->key_attribute;
    out[4] = in->scene_number;
    return 5u;
}

bool zw_cc_central_scene_notification_parse(const uint8_t *p, size_t len,
                                             zw_cc_central_scene_notification_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_CENTRAL_SCENE, ZW_CMD_CENTRAL_SCENE_NOTIFICATION, 5u) ||
        out == NULL) {
        return false;
    }
    out->sequence_number = p[2];
    out->key_attribute = p[3];
    out->scene_number = p[4];
    return true;
}

/* ---- §4.22 Alarm Sensor (legacy) ------------------------------------------------------------------- */

size_t zw_cc_alarm_sensor_get_build(uint8_t *out, size_t cap, const zw_cc_alarm_sensor_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ALARM_SENSOR;
    out[1] = ZW_CMD_ALARM_SENSOR_GET;
    out[2] = in->sensor_type;
    return 3u;
}

bool zw_cc_alarm_sensor_get_parse(const uint8_t *p, size_t len, zw_cc_alarm_sensor_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ALARM_SENSOR, ZW_CMD_ALARM_SENSOR_GET, 3u) || out == NULL) {
        return false;
    }
    out->sensor_type = p[2];
    return true;
}

size_t zw_cc_alarm_sensor_report_build(uint8_t *out, size_t cap, const zw_cc_alarm_sensor_report_t *in)
{
    if (out == NULL || in == NULL || cap < 7u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ALARM_SENSOR;
    out[1] = ZW_CMD_ALARM_SENSOR_REPORT;
    out[2] = in->source_node;
    out[3] = in->sensor_type;
    out[4] = in->state;
    out[5] = (uint8_t)(in->seconds >> 8);
    out[6] = (uint8_t)in->seconds;
    return 7u;
}

bool zw_cc_alarm_sensor_report_parse(const uint8_t *p, size_t len, zw_cc_alarm_sensor_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ALARM_SENSOR, ZW_CMD_ALARM_SENSOR_REPORT, 7u) || out == NULL) {
        return false;
    }
    out->source_node = p[2];
    out->sensor_type = p[3];
    out->state = p[4];
    out->seconds = (uint16_t)(((uint16_t)p[5] << 8) | p[6]);
    return true;
}

size_t zw_cc_alarm_sensor_supported_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ALARM_SENSOR;
    out[1] = ZW_CMD_ALARM_SENSOR_SUPPORTED_GET;
    return 2u;
}

bool zw_cc_alarm_sensor_supported_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ALARM_SENSOR, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET, 2u);
}

size_t zw_cc_alarm_sensor_supported_report_build(uint8_t *out, size_t cap,
                                                  const zw_cc_alarm_sensor_supported_report_t *in)
{
    if (out == NULL || in == NULL || (in->mask == NULL && in->mask_len > 0u) || in->mask_len > 0xFFu) {
        return 0u;
    }
    const size_t total = 3u + in->mask_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ALARM_SENSOR;
    out[1] = ZW_CMD_ALARM_SENSOR_SUPPORTED_REPORT;
    out[2] = (uint8_t)in->mask_len;
    for (size_t i = 0; i < in->mask_len; i++) {
        out[3u + i] = in->mask[i];
    }
    return total;
}

bool zw_cc_alarm_sensor_supported_report_parse(const uint8_t *p, size_t len,
                                                zw_cc_alarm_sensor_supported_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ALARM_SENSOR, ZW_CMD_ALARM_SENSOR_SUPPORTED_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    const size_t mlen = (size_t)p[2];
    if (len < 3u + mlen) {
        return false; /* shorter than its own count */
    }
    out->mask_len = mlen;
    out->mask = mlen > 0u ? &p[3] : NULL;
    return true;
}

bool zw_cc_alarm_sensor_type_supported(const uint8_t *mask, size_t mask_len, uint8_t sensor_type)
{
    if (mask == NULL) {
        return false;
    }
    const unsigned b = (unsigned)sensor_type / 8u;
    const unsigned n = (unsigned)sensor_type % 8u;
    if (b >= mask_len) {
        return false;
    }
    return (((unsigned)mask[b] >> n) & 1u) != 0u;
}

/* ---- §4.23 Hail (legacy) --------------------------------------------------------------------------- */

size_t zw_cc_hail_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_HAIL;
    out[1] = ZW_CMD_HAIL;
    return 2u;
}

bool zw_cc_hail_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_HAIL, ZW_CMD_HAIL, 2u);
}

/* ---- §4.24 Time ------------------------------------------------------------------------------------ */

size_t zw_cc_time_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_TIME;
    out[1] = ZW_CMD_TIME_GET;
    return 2u;
}

bool zw_cc_time_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_TIME, ZW_CMD_TIME_GET, 2u);
}

size_t zw_cc_time_report_build(uint8_t *out, size_t cap, const zw_cc_time_report_t *in)
{
    if (out == NULL || in == NULL || cap < 5u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_TIME;
    out[1] = ZW_CMD_TIME_REPORT;
    out[2] = (uint8_t)((in->clock_not_set ? 0x80u : 0x00u) | (in->hour & 0x1Fu));
    out[3] = in->minute;
    out[4] = in->second;
    return 5u;
}

bool zw_cc_time_report_parse(const uint8_t *p, size_t len, zw_cc_time_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_TIME, ZW_CMD_TIME_REPORT, 5u) || out == NULL) {
        return false;
    }
    out->clock_not_set = ((unsigned)p[2] & 0x80u) != 0u;
    out->hour = (uint8_t)(p[2] & 0x1Fu);
    out->minute = p[3];
    out->second = p[4];
    return true;
}

size_t zw_cc_date_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_TIME;
    out[1] = ZW_CMD_DATE_GET;
    return 2u;
}

bool zw_cc_date_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_TIME, ZW_CMD_DATE_GET, 2u);
}

size_t zw_cc_date_report_build(uint8_t *out, size_t cap, const zw_cc_date_report_t *in)
{
    if (out == NULL || in == NULL || cap < 6u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_TIME;
    out[1] = ZW_CMD_DATE_REPORT;
    out[2] = (uint8_t)(in->year >> 8);
    out[3] = (uint8_t)in->year;
    out[4] = in->month;
    out[5] = in->day;
    return 6u;
}

bool zw_cc_date_report_parse(const uint8_t *p, size_t len, zw_cc_date_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_TIME, ZW_CMD_DATE_REPORT, 6u) || out == NULL) {
        return false;
    }
    out->year = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    out->month = p[4];
    out->day = p[5];
    return true;
}

/* ---- §4.25 Application Status ---------------------------------------------------------------------- */

size_t zw_cc_application_busy_build(uint8_t *out, size_t cap, const zw_cc_application_busy_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_APPLICATION_STATUS;
    out[1] = ZW_CMD_APPLICATION_BUSY;
    out[2] = in->status;
    out[3] = in->wait_s;
    return 4u;
}

bool zw_cc_application_busy_parse(const uint8_t *p, size_t len, zw_cc_application_busy_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_APPLICATION_STATUS, ZW_CMD_APPLICATION_BUSY, 3u) ||
        out == NULL) {
        return false;
    }
    out->status = p[2];
    out->wait_s = len > 3u ? p[3] : 0u;
    return true;
}

size_t zw_cc_application_rejected_request_build(uint8_t *out, size_t cap,
                                                const zw_cc_application_rejected_request_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_APPLICATION_STATUS;
    out[1] = ZW_CMD_APPLICATION_REJECTED_REQUEST;
    out[2] = in->status;
    return 3u;
}

bool zw_cc_application_rejected_request_parse(const uint8_t *p, size_t len,
                                              zw_cc_application_rejected_request_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_APPLICATION_STATUS,
                          ZW_CMD_APPLICATION_REJECTED_REQUEST, 3u) ||
        out == NULL) {
        return false;
    }
    out->status = p[2];
    return true;
}
