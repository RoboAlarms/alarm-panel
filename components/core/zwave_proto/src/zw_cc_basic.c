/*
 * zw_cc_basic.c - No Operation, Basic, Binary Sensor, Multilevel Sensor and Battery (see
 * zw_cc_basic.h).
 */
#include "zwave_proto/zw_cc_basic.h"

#include "zw_cc_internal.h"

/* ---- §4.1 No Operation ------------------------------------------------------------------------ */

size_t zw_cc_nop_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NO_OPERATION;
    return 1u;
}

bool zw_cc_nop_parse(const uint8_t *p, size_t len)
{
    return p != NULL && len >= 1u && p[0] == (uint8_t)ZW_CC_NO_OPERATION;
}

/* ---- §4.2 Basic --------------------------------------------------------------------------------- */

size_t zw_cc_basic_set_build(uint8_t *out, size_t cap, const zw_cc_basic_set_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BASIC;
    out[1] = ZW_CMD_BASIC_SET;
    out[2] = in->value;
    return 3u;
}

bool zw_cc_basic_set_parse(const uint8_t *p, size_t len, zw_cc_basic_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_SET, 3u) || out == NULL) {
        return false;
    }
    out->value = p[2];
    return true;
}

size_t zw_cc_basic_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BASIC;
    out[1] = ZW_CMD_BASIC_GET;
    return 2u;
}

bool zw_cc_basic_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET, 2u);
}

size_t zw_cc_basic_report_build(uint8_t *out, size_t cap, const zw_cc_basic_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_target ? 5u : 3u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BASIC;
    out[1] = ZW_CMD_BASIC_REPORT;
    out[2] = in->current_value;
    if (in->has_target) {
        out[3] = in->target_value;
        out[4] = in->duration;
    }
    return total;
}

bool zw_cc_basic_report_parse(const uint8_t *p, size_t len, zw_cc_basic_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_REPORT, 3u) || out == NULL) {
        return false;
    }
    out->current_value = p[2];
    out->has_target = len >= 5u;
    out->target_value = out->has_target ? (unsigned)p[3] : 0u;
    out->duration = out->has_target ? (unsigned)p[4] : 0u;
    return true;
}

/* ---- §4.3 Binary Sensor -------------------------------------------------------------------------- */

size_t zw_cc_binary_sensor_supported_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BINARY_SENSOR;
    out[1] = ZW_CMD_BINARY_SENSOR_SUPPORTED_GET;
    return 2u;
}

bool zw_cc_binary_sensor_supported_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BINARY_SENSOR, ZW_CMD_BINARY_SENSOR_SUPPORTED_GET, 2u);
}

size_t zw_cc_binary_sensor_supported_report_build(uint8_t *out, size_t cap,
                                                   const zw_cc_binary_sensor_supported_report_t *in)
{
    if (out == NULL || in == NULL || (in->mask == NULL && in->mask_len > 0u)) {
        return 0u;
    }
    const size_t total = 2u + in->mask_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BINARY_SENSOR;
    out[1] = ZW_CMD_BINARY_SENSOR_SUPPORTED_REPORT;
    for (size_t i = 0; i < in->mask_len; i++) {
        out[2u + i] = in->mask[i];
    }
    return total;
}

bool zw_cc_binary_sensor_supported_report_parse(const uint8_t *p, size_t len,
                                                 zw_cc_binary_sensor_supported_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BINARY_SENSOR, ZW_CMD_BINARY_SENSOR_SUPPORTED_REPORT, 2u) ||
        out == NULL) {
        return false;
    }
    out->mask_len = len - 2u;
    out->mask = out->mask_len > 0u ? &p[2] : NULL;
    return true;
}

bool zw_cc_binary_sensor_type_supported(const uint8_t *mask, size_t mask_len, uint8_t sensor_type)
{
    if (mask == NULL || sensor_type == 0u) {
        return false;
    }
    const unsigned b = (unsigned)sensor_type / 8u;
    const unsigned n = (unsigned)sensor_type % 8u;
    if (b >= mask_len) {
        return false;
    }
    return (((unsigned)mask[b] >> n) & 1u) != 0u;
}

size_t zw_cc_binary_sensor_get_build(uint8_t *out, size_t cap, const zw_cc_binary_sensor_get_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_type ? 3u : 2u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BINARY_SENSOR;
    out[1] = ZW_CMD_BINARY_SENSOR_GET;
    if (in->has_type) {
        out[2] = in->sensor_type;
    }
    return total;
}

bool zw_cc_binary_sensor_get_parse(const uint8_t *p, size_t len, zw_cc_binary_sensor_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BINARY_SENSOR, ZW_CMD_BINARY_SENSOR_GET, 2u) || out == NULL) {
        return false;
    }
    out->has_type = len >= 3u;
    out->sensor_type = out->has_type ? (unsigned)p[2] : 0u;
    return true;
}

size_t zw_cc_binary_sensor_report_build(uint8_t *out, size_t cap, const zw_cc_binary_sensor_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_type ? 4u : 3u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BINARY_SENSOR;
    out[1] = ZW_CMD_BINARY_SENSOR_REPORT;
    out[2] = in->detected ? 0xFFu : 0x00u;
    if (in->has_type) {
        out[3] = in->sensor_type;
    }
    return total;
}

bool zw_cc_binary_sensor_report_parse(const uint8_t *p, size_t len, zw_cc_binary_sensor_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BINARY_SENSOR, ZW_CMD_BINARY_SENSOR_REPORT, 3u) || out == NULL) {
        return false;
    }
    if (p[2] != 0x00u && p[2] != 0xFFu) {
        return false;
    }
    out->detected = (p[2] == 0xFFu);
    out->has_type = len >= 4u;
    out->sensor_type = out->has_type ? (unsigned)p[3] : 0u;
    return true;
}

/* ---- §4.4 Multilevel Sensor ---------------------------------------------------------------------- */

size_t zw_cc_multilevel_sensor_supported_sensor_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTILEVEL_SENSOR;
    out[1] = ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_GET;
    return 2u;
}

bool zw_cc_multilevel_sensor_supported_sensor_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTILEVEL_SENSOR,
                             ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_GET, 2u);
}

size_t zw_cc_multilevel_sensor_supported_sensor_report_build(
    uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_supported_sensor_report_t *in)
{
    if (out == NULL || in == NULL || (in->mask == NULL && in->mask_len > 0u)) {
        return 0u;
    }
    const size_t total = 2u + in->mask_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTILEVEL_SENSOR;
    out[1] = ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_REPORT;
    for (size_t i = 0; i < in->mask_len; i++) {
        out[2u + i] = in->mask[i];
    }
    return total;
}

bool zw_cc_multilevel_sensor_supported_sensor_report_parse(
    const uint8_t *p, size_t len, zw_cc_multilevel_sensor_supported_sensor_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTILEVEL_SENSOR,
                           ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_REPORT, 2u) ||
        out == NULL) {
        return false;
    }
    out->mask_len = len - 2u;
    out->mask = out->mask_len > 0u ? &p[2] : NULL;
    return true;
}

bool zw_cc_multilevel_sensor_type_supported(const uint8_t *mask, size_t mask_len, uint8_t sensor_type)
{
    if (mask == NULL || sensor_type == 0u) {
        return false;
    }
    const unsigned local = (unsigned)sensor_type - 1u;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    if (b >= mask_len) {
        return false;
    }
    return (((unsigned)mask[b] >> n) & 1u) != 0u;
}

size_t zw_cc_multilevel_sensor_supported_scale_get_build(
    uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_supported_scale_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTILEVEL_SENSOR;
    out[1] = ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_GET;
    out[2] = in->sensor_type;
    return 3u;
}

bool zw_cc_multilevel_sensor_supported_scale_get_parse(
    const uint8_t *p, size_t len, zw_cc_multilevel_sensor_supported_scale_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTILEVEL_SENSOR,
                           ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_GET, 3u) ||
        out == NULL) {
        return false;
    }
    out->sensor_type = p[2];
    return true;
}

size_t zw_cc_multilevel_sensor_supported_scale_report_build(
    uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_supported_scale_report_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTILEVEL_SENSOR;
    out[1] = ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_REPORT;
    out[2] = in->sensor_type;
    out[3] = (uint8_t)(in->scale_mask & 0x0Fu);
    return 4u;
}

bool zw_cc_multilevel_sensor_supported_scale_report_parse(
    const uint8_t *p, size_t len, zw_cc_multilevel_sensor_supported_scale_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTILEVEL_SENSOR,
                           ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_REPORT, 4u) ||
        out == NULL) {
        return false;
    }
    out->sensor_type = p[2];
    out->scale_mask = (uint8_t)(p[3] & 0x0Fu);
    return true;
}

size_t zw_cc_multilevel_sensor_get_build(uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_get_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_type ? 4u : 2u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTILEVEL_SENSOR;
    out[1] = ZW_CMD_MULTILEVEL_SENSOR_GET;
    if (in->has_type) {
        out[2] = in->sensor_type;
        out[3] = (uint8_t)((unsigned)(in->scale & 0x03u) << 3);
    }
    return total;
}

bool zw_cc_multilevel_sensor_get_parse(const uint8_t *p, size_t len, zw_cc_multilevel_sensor_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTILEVEL_SENSOR, ZW_CMD_MULTILEVEL_SENSOR_GET, 2u) ||
        out == NULL) {
        return false;
    }
    out->has_type = len >= 4u;
    if (out->has_type) {
        out->sensor_type = p[2];
        out->scale = (uint8_t)((unsigned)(p[3] >> 3) & 0x03u);
    } else {
        out->sensor_type = 0u;
        out->scale = 0u;
    }
    return true;
}

size_t zw_cc_multilevel_sensor_report_build(uint8_t *out, size_t cap,
                                             const zw_cc_multilevel_sensor_report_t *in)
{
    if (out == NULL || in == NULL || in->size < 1u || in->size > 4u) {
        return 0u;
    }
    const size_t total = 4u + (unsigned)in->size;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTILEVEL_SENSOR;
    out[1] = ZW_CMD_MULTILEVEL_SENSOR_REPORT;
    out[2] = in->sensor_type;
    out[3] = (uint8_t)(((unsigned)(in->precision & 0x07u) << 5) | ((unsigned)(in->scale & 0x03u) << 3) |
                        (unsigned)(in->size & 0x07u));
    if (!zwi_signed_encode(&out[4], in->size, in->raw_value)) {
        return 0u;
    }
    return total;
}

bool zw_cc_multilevel_sensor_report_parse(const uint8_t *p, size_t len,
                                           zw_cc_multilevel_sensor_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTILEVEL_SENSOR, ZW_CMD_MULTILEVEL_SENSOR_REPORT, 4u) ||
        out == NULL) {
        return false;
    }
    const uint8_t psz = p[3];
    const uint8_t size = (uint8_t)(psz & 0x07u);
    if (size < 1u || size > 4u) { /* 1..4 bytes, 3 included (Z-Wave JS: any width 1..4) */
        return false;
    }
    if (len < 4u + (size_t)size) {
        return false;
    }
    out->sensor_type = p[2];
    out->precision = (uint8_t)((psz >> 5) & 0x07u);
    out->scale = (uint8_t)((psz >> 3) & 0x03u);
    out->size = size;
    return zwi_signed_decode(&p[4], size, &out->raw_value);
}

/* ---- §4.6 Battery ----------------------------------------------------------------------------- */

size_t zw_cc_battery_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BATTERY;
    out[1] = ZW_CMD_BATTERY_GET;
    return 2u;
}

bool zw_cc_battery_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BATTERY, ZW_CMD_BATTERY_GET, 2u);
}

size_t zw_cc_battery_report_build(uint8_t *out, size_t cap, const zw_cc_battery_report_t *in)
{
    if (out == NULL || in == NULL || (in->level > 0x64u && in->level != ZW_BATTERY_LOW)) {
        return 0u;
    }
    const size_t total = in->has_v2 ? 5u : 3u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_BATTERY;
    out[1] = ZW_CMD_BATTERY_REPORT;
    out[2] = in->level;
    if (in->has_v2) {
        uint8_t f1 = (uint8_t)((unsigned)(in->charging_state & 0x03u) << 6);
        if (in->rechargeable) {
            f1 |= 0x20u;
        }
        if (in->is_backup) {
            f1 |= 0x10u;
        }
        if (in->overheating) {
            f1 |= 0x08u;
        }
        if (in->low_fluid) {
            f1 |= 0x04u;
        }
        f1 |= (uint8_t)(in->replace_or_recharge & 0x03u);
        uint8_t f2 = 0u;
        if (in->low_temperature) {
            f2 |= 0x02u;
        }
        if (in->disconnected) {
            f2 |= 0x01u;
        }
        out[3] = f1;
        out[4] = f2;
    }
    return total;
}

bool zw_cc_battery_report_parse(const uint8_t *p, size_t len, zw_cc_battery_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_BATTERY, ZW_CMD_BATTERY_REPORT, 3u) || out == NULL) {
        return false;
    }
    const uint8_t level = p[2];
    if (level > 0x64u && level != ZW_BATTERY_LOW) {
        return false; /* §4.6: "a level above 64 other than FF is invalid (drop the report)" */
    }
    out->level = level;
    out->has_v2 = len >= 5u;
    if (out->has_v2) {
        const uint8_t f1 = p[3];
        const uint8_t f2 = p[4];
        out->charging_state = (uint8_t)((f1 >> 6) & 0x03u);
        out->rechargeable = (f1 & 0x20u) != 0u;
        out->is_backup = (f1 & 0x10u) != 0u;
        out->overheating = (f1 & 0x08u) != 0u;
        out->low_fluid = (f1 & 0x04u) != 0u;
        out->replace_or_recharge = (uint8_t)(f1 & 0x03u);
        out->low_temperature = (f2 & 0x02u) != 0u;
        out->disconnected = (f2 & 0x01u) != 0u;
    } else {
        out->charging_state = 0u;
        out->rechargeable = false;
        out->is_backup = false;
        out->overheating = false;
        out->low_fluid = false;
        out->replace_or_recharge = 0u;
        out->low_temperature = false;
        out->disconnected = false;
    }
    return true;
}
