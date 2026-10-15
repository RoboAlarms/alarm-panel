/*
 * zw_cc_switch.c - Binary Switch and Multilevel Switch (see zw_cc_switch.h).
 */
#include "zwave_proto/zw_cc_switch.h"

#include "zw_cc_internal.h"

static bool switch_cc(uint8_t cc)
{
    return cc == (uint8_t)ZW_CC_BINARY_SWITCH || cc == (uint8_t)ZW_CC_MULTILEVEL_SWITCH;
}

static bool set_value_ok(uint8_t cc, uint8_t v)
{
    if (cc == (uint8_t)ZW_CC_BINARY_SWITCH) {
        return v == ZW_SWITCH_OFF || v == ZW_SWITCH_ON;
    }
    return v <= ZW_SWITCH_LEVEL_MAX || v == ZW_SWITCH_ON;
}

size_t zw_cc_switch_set_build(uint8_t *out, size_t cap, uint8_t cc, const zw_cc_switch_set_t *in)
{
    const size_t n = in != NULL && in->has_duration ? 4u : 3u;
    if (out == NULL || in == NULL || cap < n || !switch_cc(cc) || !set_value_ok(cc, in->value)) {
        return 0u;
    }
    out[0] = cc;
    out[1] = ZW_CMD_SWITCH_SET;
    out[2] = in->value;
    if (in->has_duration) {
        out[3] = in->duration;
    }
    return n;
}

bool zw_cc_switch_set_parse(const uint8_t *p, size_t len, uint8_t cc, zw_cc_switch_set_t *out)
{
    if (!switch_cc(cc) || out == NULL || !zwi_cc_header_ok(p, len, cc, ZW_CMD_SWITCH_SET, 3u)) {
        return false;
    }
    out->value = p[2];
    out->has_duration = len >= 4u;
    out->duration = (uint8_t)(out->has_duration ? (unsigned)p[3] : 0u);
    return true;
}

size_t zw_cc_switch_get_build(uint8_t *out, size_t cap, uint8_t cc)
{
    if (out == NULL || cap < 2u || !switch_cc(cc)) {
        return 0u;
    }
    out[0] = cc;
    out[1] = ZW_CMD_SWITCH_GET;
    return 2u;
}

bool zw_cc_switch_get_parse(const uint8_t *p, size_t len, uint8_t cc)
{
    return switch_cc(cc) && zwi_cc_header_ok(p, len, cc, ZW_CMD_SWITCH_GET, 2u);
}

size_t zw_cc_switch_report_build(uint8_t *out, size_t cap, uint8_t cc,
                                 const zw_cc_switch_report_t *in)
{
    const size_t n = in != NULL && in->has_target ? 5u : 3u;
    if (out == NULL || in == NULL || cap < n || !switch_cc(cc)) {
        return 0u;
    }
    out[0] = cc;
    out[1] = ZW_CMD_SWITCH_REPORT;
    out[2] = in->current;
    if (in->has_target) {
        out[3] = in->target;
        out[4] = in->duration;
    }
    return n;
}

bool zw_cc_switch_report_parse(const uint8_t *p, size_t len, uint8_t cc,
                               zw_cc_switch_report_t *out)
{
    if (!switch_cc(cc) || out == NULL ||
        !zwi_cc_header_ok(p, len, cc, ZW_CMD_SWITCH_REPORT, 3u)) {
        return false;
    }
    out->current = p[2];
    out->has_target = len >= 5u;
    out->target = (uint8_t)(out->has_target ? (unsigned)p[3] : 0u);
    out->duration = (uint8_t)(out->has_target ? (unsigned)p[4] : 0u);
    return true;
}

int zw_cc_switch_percent(uint8_t cc, uint8_t value)
{
    if (value == ZW_SWITCH_UNKNOWN) {
        return -1;
    }
    if (cc == (uint8_t)ZW_CC_BINARY_SWITCH) {
        return value == ZW_SWITCH_OFF ? 0 : value <= ZW_SWITCH_LEVEL_MAX || value == ZW_SWITCH_ON
                                                ? 100
                                                : -1;
    }
    if (value == ZW_SWITCH_OFF) {
        return 0;
    }
    if (value == ZW_SWITCH_ON) {
        return 100; /* a Report never says FF; a device that does is on */
    }
    if (value > ZW_SWITCH_LEVEL_MAX) {
        return -1;
    }
    return value == ZW_SWITCH_LEVEL_MAX ? 100 : (int)value;
}
