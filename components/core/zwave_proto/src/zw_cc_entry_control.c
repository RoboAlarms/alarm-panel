/* Entry Control v1 codecs. No credential copies survive outside caller-owned buffers. */
#include "zwave_proto/zw_cc.h"
#include "zw_cc_internal.h"
#include <string.h>

static bool data_valid(uint8_t type, const uint8_t *data, size_t n)
{
    if (type > ZW_ENTRY_DATA_MD5 || n > ZW_ENTRY_DATA_MAX || (n != 0u && data == NULL)) {
        return false;
    }
    if (type == ZW_ENTRY_DATA_NONE) { return n == 0u; }
    if (type == ZW_ENTRY_DATA_MD5) { return n == 16u; }
    if (type == ZW_ENTRY_DATA_RAW) { return n != 0u; }
    bool padding = false;
    for (size_t i = 0u; i < n; i++) {
        if (data[i] == 0xFFu) { padding = true; }
        else if (padding || data[i] > 0x7Fu) { return false; }
    }
    return true;
}

bool zw_cc_entry_notification_parse(const uint8_t *p, size_t len, zw_cc_entry_notification_t *out)
{
    if (out == NULL || !zwi_cc_header_ok(p, len, ZW_CC_ENTRY_CONTROL, ZW_CMD_ENTRY_NOTIFICATION, 6u)) {
        return false;
    }
    const size_t n = p[5];
    const uint8_t type = (uint8_t)(p[3] & 3u);
    if (n > len - 6u || !data_valid(type, p + 6u, n)) { return false; }
    *out = (zw_cc_entry_notification_t){p[2], type, p[4], p + 6u, n};
    return true;
}

size_t zw_cc_entry_notification_build(uint8_t *out, size_t cap, const zw_cc_entry_notification_t *in)
{
    if (out == NULL || in == NULL || !data_valid(in->data_type, in->data, in->data_len)) { return 0u; }
    const size_t n = in->data_type == ZW_ENTRY_DATA_ASCII ? ((in->data_len + 15u) / 16u) * 16u : in->data_len;
    if (cap < 6u + n) { return 0u; }
    out[0] = ZW_CC_ENTRY_CONTROL; out[1] = ZW_CMD_ENTRY_NOTIFICATION;
    out[2] = in->sequence; out[3] = in->data_type; out[4] = in->event_type; out[5] = (uint8_t)n;
    if (in->data_len != 0u) { memcpy(out + 6u, in->data, in->data_len); }
    if (n > in->data_len) { memset(out + 6u + in->data_len, 0xFF, n - in->data_len); }
    return 6u + n;
}

bool zw_cc_entry_keys_parse(const uint8_t *p, size_t len, zw_cc_entry_keys_t *out)
{
    if (out == NULL || !zwi_cc_header_ok(p, len, ZW_CC_ENTRY_CONTROL, ZW_CMD_ENTRY_KEY_SUPPORTED_REPORT, 3u) ||
        p[2] > 32u || (size_t)p[2] > len - 3u) { return false; }
    *out = (zw_cc_entry_keys_t){p + 3u, p[2]};
    return true;
}

size_t zw_cc_entry_keys_build(uint8_t *out, size_t cap, const zw_cc_entry_keys_t *in)
{
    if (out == NULL || in == NULL || in->mask_len > 32u ||
        (in->mask_len != 0u && in->mask == NULL) || cap < 3u + in->mask_len) { return 0u; }
    out[0] = ZW_CC_ENTRY_CONTROL; out[1] = ZW_CMD_ENTRY_KEY_SUPPORTED_REPORT; out[2] = (uint8_t)in->mask_len;
    if (in->mask_len != 0u) { memcpy(out + 3u, in->mask, in->mask_len); }
    return 3u + in->mask_len;
}

static bool limits_valid(const zw_cc_entry_events_t *in)
{
    return in->min_cache >= 1u && in->min_cache <= in->max_cache && in->max_cache <= 32u &&
           in->min_timeout >= 1u && in->min_timeout <= in->max_timeout && in->max_timeout <= 10u;
}

bool zw_cc_entry_events_parse(const uint8_t *p, size_t len, zw_cc_entry_events_t *out)
{
    if (out == NULL || !zwi_cc_header_ok(p, len, ZW_CC_ENTRY_CONTROL, ZW_CMD_ENTRY_EVENT_SUPPORTED_REPORT, 3u)) {
        return false;
    }
    zw_cc_entry_events_t v = {0};
    v.data_types_len = p[2] & 3u;
    size_t at = 3u + v.data_types_len;
    if (at >= len) { return false; }
    v.data_types = p + 3u;
    v.event_types_len = p[at++] & 0x1Fu;
    if (v.event_types_len > len - at || len - at - v.event_types_len < 4u) { return false; }
    v.event_types = p + at;
    at += v.event_types_len;
    v.min_cache = p[at]; v.max_cache = p[at + 1u];
    v.min_timeout = p[at + 2u]; v.max_timeout = p[at + 3u];
    if (!limits_valid(&v)) { return false; }
    *out = v;
    return true;
}

size_t zw_cc_entry_events_build(uint8_t *out, size_t cap, const zw_cc_entry_events_t *in)
{
    if (out == NULL || in == NULL || in->data_types_len > 3u || in->event_types_len > 31u ||
        (in->data_types_len != 0u && in->data_types == NULL) ||
        (in->event_types_len != 0u && in->event_types == NULL) || !limits_valid(in)) { return 0u; }
    const size_t n = 8u + in->data_types_len + in->event_types_len;
    if (cap < n) { return 0u; }
    out[0] = ZW_CC_ENTRY_CONTROL; out[1] = ZW_CMD_ENTRY_EVENT_SUPPORTED_REPORT;
    out[2] = (uint8_t)in->data_types_len;
    if (in->data_types_len != 0u) { memcpy(out + 3u, in->data_types, in->data_types_len); }
    size_t at = 3u + in->data_types_len;
    out[at++] = (uint8_t)in->event_types_len;
    if (in->event_types_len != 0u) { memcpy(out + at, in->event_types, in->event_types_len); }
    at += in->event_types_len;
    out[at] = in->min_cache; out[at + 1u] = in->max_cache;
    out[at + 2u] = in->min_timeout; out[at + 3u] = in->max_timeout;
    return n;
}

static bool config_valid(const zw_cc_entry_configuration_t *in)
{
    return in->cache_size >= 1u && in->cache_size <= 32u && in->timeout_s >= 1u && in->timeout_s <= 10u;
}

#define CONFIG_CODEC(name, cmd) \
bool zw_cc_entry_configuration_##name##_parse(const uint8_t *p, size_t len, zw_cc_entry_configuration_t *out) \
{ \
    if (out == NULL || !zwi_cc_header_ok(p, len, ZW_CC_ENTRY_CONTROL, cmd, 4u)) { return false; } \
    const zw_cc_entry_configuration_t v = {p[2], p[3]}; \
    if (!config_valid(&v)) { return false; } \
    *out = v; return true; \
} \
size_t zw_cc_entry_configuration_##name##_build(uint8_t *out, size_t cap, const zw_cc_entry_configuration_t *in) \
{ \
    if (out == NULL || in == NULL || cap < 4u || !config_valid(in)) { return 0u; } \
    out[0] = ZW_CC_ENTRY_CONTROL; out[1] = cmd; out[2] = in->cache_size; out[3] = in->timeout_s; return 4u; \
}
CONFIG_CODEC(set, ZW_CMD_ENTRY_CONFIGURATION_SET)
CONFIG_CODEC(report, ZW_CMD_ENTRY_CONFIGURATION_REPORT)
#undef CONFIG_CODEC

#define GET_CODEC(name, cmd) \
bool zw_cc_entry_##name##_get_parse(const uint8_t *p, size_t len) \
{ return zwi_cc_header_ok(p, len, ZW_CC_ENTRY_CONTROL, cmd, 2u); } \
size_t zw_cc_entry_##name##_get_build(uint8_t *out, size_t cap) \
{ \
    if (out == NULL || cap < 2u) { return 0u; } \
    out[0] = ZW_CC_ENTRY_CONTROL; out[1] = cmd; return 2u; \
}
GET_CODEC(key_supported, ZW_CMD_ENTRY_KEY_SUPPORTED_GET)
GET_CODEC(event_supported, ZW_CMD_ENTRY_EVENT_SUPPORTED_GET)
GET_CODEC(configuration, ZW_CMD_ENTRY_CONFIGURATION_GET)
#undef GET_CODEC
