/*
 * zw_cc_notification.c - Notification (see zw_cc_notification.h, command-classes.md §4.5).
 */
#include "zwave_proto/zw_cc_notification.h"

#include "zw_cc_internal.h"

size_t zw_cc_notification_supported_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_SUPPORTED_GET;
    return 2u;
}

bool zw_cc_notification_supported_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_SUPPORTED_GET, 2u);
}

size_t zw_cc_notification_supported_report_build(uint8_t *out, size_t cap,
                                                  const zw_cc_notification_supported_report_t *in)
{
    if (out == NULL || in == NULL || (in->type_mask == NULL && in->type_mask_len > 0u) ||
        in->type_mask_len > 0x1Fu) {
        return 0u;
    }
    const size_t total = 3u + in->type_mask_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_SUPPORTED_REPORT;
    out[2] = (uint8_t)((in->sends_v1_alarms_too ? 0x80u : 0x00u) | (in->type_mask_len & 0x1Fu));
    for (size_t i = 0; i < in->type_mask_len; i++) {
        out[3u + i] = in->type_mask[i];
    }
    return total;
}

bool zw_cc_notification_supported_report_parse(const uint8_t *p, size_t len,
                                                zw_cc_notification_supported_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_SUPPORTED_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    const uint8_t flags = p[2];
    const size_t mlen = (size_t)(flags & 0x1Fu);
    if (len < 3u + mlen) {
        return false;
    }
    out->sends_v1_alarms_too = ((unsigned)flags & 0x80u) != 0u;
    out->type_mask_len = mlen;
    out->type_mask = mlen > 0u ? &p[3] : NULL;
    return true;
}

bool zw_cc_notification_type_supported(const uint8_t *type_mask, size_t type_mask_len, uint8_t type)
{
    if (type_mask == NULL || type == 0u) {
        return false;
    }
    const unsigned b = (unsigned)type / 8u;
    const unsigned n = (unsigned)type % 8u;
    if (b >= type_mask_len) {
        return false;
    }
    return (((unsigned)type_mask[b] >> n) & 1u) != 0u;
}

size_t zw_cc_notification_event_supported_get_build(uint8_t *out, size_t cap,
                                                     const zw_cc_notification_event_supported_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_GET;
    out[2] = in->notification_type;
    return 3u;
}

bool zw_cc_notification_event_supported_get_parse(const uint8_t *p, size_t len,
                                                   zw_cc_notification_event_supported_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_GET, 3u) ||
        out == NULL) {
        return false;
    }
    out->notification_type = p[2];
    return true;
}

size_t zw_cc_notification_event_supported_report_build(
    uint8_t *out, size_t cap, const zw_cc_notification_event_supported_report_t *in)
{
    if (out == NULL || in == NULL || (in->event_mask == NULL && in->event_mask_len > 0u) ||
        in->event_mask_len > 0x1Fu) {
        return 0u;
    }
    const size_t total = 4u + in->event_mask_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_REPORT;
    out[2] = in->notification_type;
    out[3] = (uint8_t)(in->event_mask_len & 0x1Fu);
    for (size_t i = 0; i < in->event_mask_len; i++) {
        out[4u + i] = in->event_mask[i];
    }
    return total;
}

bool zw_cc_notification_event_supported_report_parse(
    const uint8_t *p, size_t len, zw_cc_notification_event_supported_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_REPORT, 4u) ||
        out == NULL) {
        return false;
    }
    const size_t mlen = (size_t)(p[3] & 0x1Fu);
    if (len < 4u + mlen) {
        return false;
    }
    out->notification_type = p[2];
    out->event_mask_len = mlen;
    out->event_mask = mlen > 0u ? &p[4] : NULL;
    return true;
}

size_t zw_cc_notification_get_build(uint8_t *out, size_t cap, const zw_cc_notification_get_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_event ? 5u : 4u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_GET;
    out[2] = in->v1_alarm_type;
    out[3] = in->notification_type;
    if (in->has_event) {
        out[4] = in->event;
    }
    return total;
}

bool zw_cc_notification_get_parse(const uint8_t *p, size_t len, zw_cc_notification_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_GET, 4u) || out == NULL) {
        return false;
    }
    out->v1_alarm_type = p[2];
    out->notification_type = p[3];
    out->has_event = len >= 5u;
    out->event = out->has_event ? (unsigned)p[4] : 0u;
    return true;
}

size_t zw_cc_notification_set_build(uint8_t *out, size_t cap, const zw_cc_notification_set_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_SET;
    out[2] = in->notification_type;
    out[3] = in->status;
    return 4u;
}

bool zw_cc_notification_set_parse(const uint8_t *p, size_t len, zw_cc_notification_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_SET, 4u) || out == NULL) {
        return false;
    }
    out->notification_type = p[2];
    out->status = p[3];
    return true;
}

/* Report: offsets (payload, i.e. p[2+offset]) 0=v1 type,1=v1 level,2=reserved,3=status,
 * 4=type,5=event,6=flags(bit7 has_seq, bits4..0=k),7..7+k-1=event params,7+k=sequence number.
 * "Shorter than 7 [payload] bytes is a v1 alarm" -> total len < 2+7 = 9. */
size_t zw_cc_notification_report_build(uint8_t *out, size_t cap, const zw_cc_notification_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    if (in->is_v1) {
        if (cap < 4u) {
            return 0u;
        }
        out[0] = (uint8_t)ZW_CC_NOTIFICATION;
        out[1] = ZW_CMD_NOTIFICATION_REPORT;
        out[2] = in->v1_alarm_type;
        out[3] = in->v1_alarm_level;
        return 4u;
    }
    if (in->event_params_len > 0x1Fu || (in->event_params == NULL && in->event_params_len > 0u)) {
        return 0u;
    }
    const size_t total = 9u + in->event_params_len + (in->has_sequence_number ? 1u : 0u);
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_NOTIFICATION;
    out[1] = ZW_CMD_NOTIFICATION_REPORT;
    out[2] = in->v1_alarm_type;
    out[3] = in->v1_alarm_level;
    out[4] = 0x00u; /* reserved: was a source node ID */
    out[5] = in->status;
    out[6] = in->notification_type;
    out[7] = in->event;
    out[8] = (uint8_t)((in->has_sequence_number ? 0x80u : 0x00u) | (in->event_params_len & 0x1Fu));
    for (size_t i = 0; i < in->event_params_len; i++) {
        out[9u + i] = in->event_params[i];
    }
    if (in->has_sequence_number) {
        out[9u + in->event_params_len] = in->sequence_number;
    }
    return total;
}

bool zw_cc_notification_report_parse(const uint8_t *p, size_t len, zw_cc_notification_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_NOTIFICATION, ZW_CMD_NOTIFICATION_REPORT, 4u) || out == NULL) {
        return false;
    }
    out->v1_alarm_type = p[2];
    out->v1_alarm_level = p[3];
    out->is_v1 = len < 9u;
    if (out->is_v1) {
        out->status = 0u;
        out->notification_type = 0u;
        out->event = 0u;
        out->event_params = NULL;
        out->event_params_len = 0u;
        out->has_sequence_number = false;
        out->sequence_number = 0u;
        return true;
    }
    out->status = p[5];
    out->notification_type = p[6];
    out->event = p[7];
    const uint8_t flags = p[8];
    const bool has_seq = ((unsigned)flags & 0x80u) != 0u;
    const size_t k = (size_t)(flags & 0x1Fu);
    if (len < 9u + k) {
        return false;
    }
    out->event_params = k > 0u ? &p[9] : NULL;
    out->event_params_len = k;
    out->has_sequence_number = has_seq;
    if (has_seq) {
        if (len < 9u + k + 1u) {
            return false;
        }
        out->sequence_number = p[9u + k];
    } else {
        out->sequence_number = 0u;
    }
    return true;
}
