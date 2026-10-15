/*
 * zw_cc_multichannel.c - Multi Channel (see zw_cc_multichannel.h, command-classes.md §4.15).
 */
#include "zwave_proto/zw_cc_multichannel.h"

#include "zw_cc_internal.h"

size_t zw_cc_multi_channel_end_point_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL;
    out[1] = ZW_CMD_MULTI_CHANNEL_END_POINT_GET;
    return 2u;
}

bool zw_cc_multi_channel_end_point_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL, ZW_CMD_MULTI_CHANNEL_END_POINT_GET, 2u);
}

/* flags (byte 0: bit 7 dynamic, bit 6 identical), individual_count (byte 1, bits 6..0),
 * [aggregated_count (byte 2), when the command is long enough to carry it]. */
size_t zw_cc_multi_channel_end_point_report_build(uint8_t *out, size_t cap,
                                                   const zw_cc_multi_channel_end_point_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_aggregated_count ? 5u : 4u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL;
    out[1] = ZW_CMD_MULTI_CHANNEL_END_POINT_REPORT;
    out[2] = (uint8_t)((in->is_dynamic ? 0x80u : 0x00u) | (in->identical_capabilities ? 0x40u : 0x00u));
    out[3] = (uint8_t)(in->individual_count & 0x7Fu);
    if (in->has_aggregated_count) {
        out[4] = in->aggregated_count;
    }
    return total;
}

bool zw_cc_multi_channel_end_point_report_parse(const uint8_t *p, size_t len,
                                                 zw_cc_multi_channel_end_point_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL, ZW_CMD_MULTI_CHANNEL_END_POINT_REPORT, 4u) ||
        out == NULL) {
        return false;
    }
    out->is_dynamic = ((unsigned)p[2] & 0x80u) != 0u;
    out->identical_capabilities = (p[2] & 0x40u) != 0u;
    out->individual_count = (uint8_t)(p[3] & 0x7Fu);
    out->has_aggregated_count = len >= 5u;
    out->aggregated_count = out->has_aggregated_count ? (unsigned)p[4] : 0u;
    return true;
}

size_t zw_cc_multi_channel_capability_get_build(uint8_t *out, size_t cap,
                                                 const zw_cc_multi_channel_capability_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL;
    out[1] = ZW_CMD_MULTI_CHANNEL_CAPABILITY_GET;
    out[2] = in->endpoint;
    return 3u;
}

bool zw_cc_multi_channel_capability_get_parse(const uint8_t *p, size_t len,
                                               zw_cc_multi_channel_capability_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL, ZW_CMD_MULTI_CHANNEL_CAPABILITY_GET, 3u) ||
        out == NULL) {
        return false;
    }
    out->endpoint = p[2];
    return true;
}

/* byte 0: bit 7 dynamic, bits 6..0 endpoint; byte 1 generic class; byte 2 specific class; the
 * rest of the command is the CC list (no length field of its own - it is everything left). */
size_t zw_cc_multi_channel_capability_report_build(uint8_t *out, size_t cap,
                                                    const zw_cc_multi_channel_capability_report_t *in)
{
    if (out == NULL || in == NULL || (in->cc_list == NULL && in->cc_list_len > 0u)) {
        return 0u;
    }
    const size_t total = 5u + in->cc_list_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL;
    out[1] = ZW_CMD_MULTI_CHANNEL_CAPABILITY_REPORT;
    out[2] = (uint8_t)((in->is_dynamic ? 0x80u : 0x00u) | (in->endpoint & 0x7Fu));
    out[3] = in->generic_class;
    out[4] = in->specific_class;
    for (size_t i = 0; i < in->cc_list_len; i++) {
        out[5u + i] = in->cc_list[i];
    }
    return total;
}

bool zw_cc_multi_channel_capability_report_parse(const uint8_t *p, size_t len,
                                                  zw_cc_multi_channel_capability_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL, ZW_CMD_MULTI_CHANNEL_CAPABILITY_REPORT, 5u) ||
        out == NULL) {
        return false;
    }
    out->is_dynamic = ((unsigned)p[2] & 0x80u) != 0u;
    out->endpoint = (uint8_t)(p[2] & 0x7Fu);
    out->generic_class = p[3];
    out->specific_class = p[4];
    out->cc_list_len = len - 5u;
    out->cc_list = out->cc_list_len > 0u ? &p[5] : NULL;
    return true;
}

/* source_endpoint (bits 6..0), destination (bit 7 is-mask, bits 6..0), then the encapsulated
 * command (everything left - also §1's Multi Channel layer, see zw_cc_encap.h). */
size_t zw_cc_multi_channel_command_encapsulation_build(
    uint8_t *out, size_t cap, const zw_cc_multi_channel_command_encapsulation_t *in)
{
    if (out == NULL || in == NULL || (in->command == NULL && in->command_len > 0u)) {
        return 0u;
    }
    const size_t total = 4u + in->command_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL;
    out[1] = ZW_CMD_MULTI_CHANNEL_COMMAND_ENCAPSULATION;
    out[2] = (uint8_t)(in->source_endpoint & 0x7Fu);
    out[3] = (uint8_t)((in->destination_is_mask ? 0x80u : 0x00u) | (in->destination & 0x7Fu));
    for (size_t i = 0; i < in->command_len; i++) {
        out[4u + i] = in->command[i];
    }
    return total;
}

bool zw_cc_multi_channel_command_encapsulation_parse(
    const uint8_t *p, size_t len, zw_cc_multi_channel_command_encapsulation_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL, ZW_CMD_MULTI_CHANNEL_COMMAND_ENCAPSULATION, 4u) ||
        out == NULL) {
        return false;
    }
    out->source_endpoint = (uint8_t)(p[2] & 0x7Fu);
    out->destination_is_mask = ((unsigned)p[3] & 0x80u) != 0u;
    out->destination = (uint8_t)(p[3] & 0x7Fu);
    out->command_len = len - 4u;
    out->command = out->command_len > 0u ? &p[4] : NULL;
    return true;
}
