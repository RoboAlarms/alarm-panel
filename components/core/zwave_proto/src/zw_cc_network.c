/*
 * zw_cc_network.c - the interview/network command classes (see zw_cc_network.h,
 * command-classes.md §4.7-§4.13).
 */
#include "zwave_proto/zw_cc_network.h"

#include "zw_cc_internal.h"

/* 3-byte big-endian fields (Wake Up's interval): local to this file only. */
static uint32_t read_u24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static void write_u24(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 16);
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)v;
}

/* ---- §4.7 Wake Up ------------------------------------------------------------------------------ */

size_t zw_cc_wake_up_interval_set_build(uint8_t *out, size_t cap, const zw_cc_wake_up_interval_set_t *in)
{
    if (out == NULL || in == NULL || cap < 6u || in->interval_seconds > 0xFFFFFFu) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_INTERVAL_SET;
    write_u24(&out[2], in->interval_seconds);
    out[5] = in->node_id_to_notify;
    return 6u;
}

bool zw_cc_wake_up_interval_set_parse(const uint8_t *p, size_t len, zw_cc_wake_up_interval_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_SET, 6u) || out == NULL) {
        return false;
    }
    out->interval_seconds = read_u24(&p[2]);
    out->node_id_to_notify = p[5];
    return true;
}

size_t zw_cc_wake_up_interval_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_INTERVAL_GET;
    return 2u;
}

bool zw_cc_wake_up_interval_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_GET, 2u);
}

size_t zw_cc_wake_up_interval_report_build(uint8_t *out, size_t cap,
                                            const zw_cc_wake_up_interval_report_t *in)
{
    if (out == NULL || in == NULL || cap < 6u || in->interval_seconds > 0xFFFFFFu) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_INTERVAL_REPORT;
    write_u24(&out[2], in->interval_seconds);
    out[5] = in->node_id;
    return 6u;
}

bool zw_cc_wake_up_interval_report_parse(const uint8_t *p, size_t len,
                                          zw_cc_wake_up_interval_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_REPORT, 6u) || out == NULL) {
        return false;
    }
    out->interval_seconds = read_u24(&p[2]);
    out->node_id = p[5];
    return true;
}

size_t zw_cc_wake_up_notification_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_NOTIFICATION;
    return 2u;
}

bool zw_cc_wake_up_notification_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_NOTIFICATION, 2u);
}

size_t zw_cc_wake_up_no_more_information_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_NO_MORE_INFORMATION;
    return 2u;
}

bool zw_cc_wake_up_no_more_information_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_NO_MORE_INFORMATION, 2u);
}

size_t zw_cc_wake_up_interval_capabilities_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_GET;
    return 2u;
}

bool zw_cc_wake_up_interval_capabilities_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_GET, 2u);
}

size_t zw_cc_wake_up_interval_capabilities_report_build(
    uint8_t *out, size_t cap, const zw_cc_wake_up_interval_capabilities_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    if (in->min_seconds > 0xFFFFFFu || in->max_seconds > 0xFFFFFFu || in->default_seconds > 0xFFFFFFu ||
        in->step_seconds > 0xFFFFFFu) {
        return 0u;
    }
    const size_t total = in->has_v3 ? 15u : 14u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_WAKE_UP;
    out[1] = ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_REPORT;
    write_u24(&out[2], in->min_seconds);
    write_u24(&out[5], in->max_seconds);
    write_u24(&out[8], in->default_seconds);
    write_u24(&out[11], in->step_seconds);
    if (in->has_v3) {
        out[14] = in->wake_up_on_demand ? 0x01u : 0x00u;
    }
    return total;
}

bool zw_cc_wake_up_interval_capabilities_report_parse(
    const uint8_t *p, size_t len, zw_cc_wake_up_interval_capabilities_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_REPORT, 14u) ||
        out == NULL) {
        return false;
    }
    out->min_seconds = read_u24(&p[2]);
    out->max_seconds = read_u24(&p[5]);
    out->default_seconds = read_u24(&p[8]);
    out->step_seconds = read_u24(&p[11]);
    out->has_v3 = len >= 15u;
    out->wake_up_on_demand = out->has_v3 && ((p[14] & 0x01u) != 0u);
    return true;
}

/* ---- §4.8 Association ---------------------------------------------------------------------------- */

size_t zw_cc_association_set_build(uint8_t *out, size_t cap, const zw_cc_association_set_t *in)
{
    if (out == NULL || in == NULL || (in->node_ids == NULL && in->node_id_count > 0u)) {
        return 0u;
    }
    const size_t total = 3u + in->node_id_count;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SET;
    out[2] = in->group;
    for (size_t i = 0; i < in->node_id_count; i++) {
        out[3u + i] = in->node_ids[i];
    }
    return total;
}

bool zw_cc_association_set_parse(const uint8_t *p, size_t len, zw_cc_association_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_SET, 3u) || out == NULL) {
        return false;
    }
    out->group = p[2];
    out->node_id_count = len - 3u;
    out->node_ids = out->node_id_count > 0u ? &p[3] : NULL;
    return true;
}

size_t zw_cc_association_get_build(uint8_t *out, size_t cap, const zw_cc_association_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_GET;
    out[2] = in->group;
    return 3u;
}

bool zw_cc_association_get_parse(const uint8_t *p, size_t len, zw_cc_association_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_GET, 3u) || out == NULL) {
        return false;
    }
    out->group = p[2];
    return true;
}

size_t zw_cc_association_report_build(uint8_t *out, size_t cap, const zw_cc_association_report_t *in)
{
    if (out == NULL || in == NULL || (in->node_ids == NULL && in->node_id_count > 0u)) {
        return 0u;
    }
    const size_t total = 5u + in->node_id_count;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_REPORT;
    out[2] = in->group;
    out[3] = in->max_nodes;
    out[4] = in->reports_to_follow;
    for (size_t i = 0; i < in->node_id_count; i++) {
        out[5u + i] = in->node_ids[i];
    }
    return total;
}

bool zw_cc_association_report_parse(const uint8_t *p, size_t len, zw_cc_association_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_REPORT, 5u) || out == NULL) {
        return false;
    }
    out->group = p[2];
    out->max_nodes = p[3];
    out->reports_to_follow = p[4];
    out->node_id_count = len - 5u;
    out->node_ids = out->node_id_count > 0u ? &p[5] : NULL;
    return true;
}

size_t zw_cc_association_remove_build(uint8_t *out, size_t cap, const zw_cc_association_remove_t *in)
{
    if (out == NULL || in == NULL || (in->node_ids == NULL && in->node_id_count > 0u)) {
        return 0u;
    }
    const size_t total = 3u + in->node_id_count;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_REMOVE;
    out[2] = in->group;
    for (size_t i = 0; i < in->node_id_count; i++) {
        out[3u + i] = in->node_ids[i];
    }
    return total;
}

bool zw_cc_association_remove_parse(const uint8_t *p, size_t len, zw_cc_association_remove_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_REMOVE, 3u) || out == NULL) {
        return false;
    }
    out->group = p[2];
    out->node_id_count = len - 3u;
    out->node_ids = out->node_id_count > 0u ? &p[3] : NULL;
    return true;
}

size_t zw_cc_association_supported_groupings_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET;
    return 2u;
}

bool zw_cc_association_supported_groupings_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET, 2u);
}

size_t zw_cc_association_supported_groupings_report_build(
    uint8_t *out, size_t cap, const zw_cc_association_supported_groupings_report_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT;
    out[2] = in->group_count;
    return 3u;
}

bool zw_cc_association_supported_groupings_report_parse(
    const uint8_t *p, size_t len, zw_cc_association_supported_groupings_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    out->group_count = p[2];
    return true;
}

size_t zw_cc_association_specific_group_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_GET;
    return 2u;
}

bool zw_cc_association_specific_group_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_GET, 2u);
}

size_t zw_cc_association_specific_group_report_build(
    uint8_t *out, size_t cap, const zw_cc_association_specific_group_report_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_REPORT;
    out[2] = in->group;
    return 3u;
}

bool zw_cc_association_specific_group_report_parse(
    const uint8_t *p, size_t len, zw_cc_association_specific_group_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION, ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    out->group = p[2];
    return true;
}

/* ---- §4.9 Multi Channel Association: same command IDs as Association, its own CC. ---------------- */

void zw_cc_mc_dest_iter_init(zw_cc_mc_dest_iter_t *it, const uint8_t *destinations, size_t len)
{
    if (it == NULL) {
        return;
    }
    it->pos = destinations;
    it->end = (destinations != NULL) ? destinations + len : destinations;
    it->past_marker = false;
}

bool zw_cc_mc_dest_iter_next(zw_cc_mc_dest_iter_t *it, zw_cc_mc_destination_t *out)
{
    if (it == NULL || out == NULL || it->pos == NULL || it->end == NULL) {
        return false;
    }
    while (it->pos < it->end) {
        if (!it->past_marker) {
            const uint8_t b = *it->pos;
            it->pos++;
            if (b == 0x00u) {
                it->past_marker = true;
                continue;
            }
            out->node_id = b;
            out->is_endpoint = false;
            out->is_mask = false;
            out->endpoint = 0u;
            out->endpoint_mask = 0u;
            return true;
        }
        if ((size_t)(it->end - it->pos) < 2u) {
            it->pos = it->end; /* a stray trailing byte after the marker: stop cleanly */
            return false;
        }
        const uint8_t node_id = it->pos[0];
        const uint8_t epb = it->pos[1];
        it->pos += 2;
        out->node_id = node_id;
        out->is_endpoint = true;
        out->is_mask = ((unsigned)epb & 0x80u) != 0u;
        out->endpoint = out->is_mask ? 0u : (uint8_t)(epb & 0x7Fu);
        out->endpoint_mask = out->is_mask ? (uint8_t)(epb & 0x7Fu) : 0u;
        return true;
    }
    return false;
}

size_t zw_cc_mc_association_set_build(uint8_t *out, size_t cap, const zw_cc_mc_association_set_t *in)
{
    if (out == NULL || in == NULL || (in->destinations == NULL && in->destinations_len > 0u)) {
        return 0u;
    }
    const size_t total = 3u + in->destinations_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SET;
    out[2] = in->group;
    for (size_t i = 0; i < in->destinations_len; i++) {
        out[3u + i] = in->destinations[i];
    }
    return total;
}

bool zw_cc_mc_association_set_parse(const uint8_t *p, size_t len, zw_cc_mc_association_set_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, ZW_CMD_ASSOCIATION_SET, 3u) ||
        out == NULL) {
        return false;
    }
    out->group = p[2];
    out->destinations_len = len - 3u;
    out->destinations = out->destinations_len > 0u ? &p[3] : NULL;
    return true;
}

size_t zw_cc_mc_association_get_build(uint8_t *out, size_t cap, const zw_cc_mc_association_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_GET;
    out[2] = in->group;
    return 3u;
}

bool zw_cc_mc_association_get_parse(const uint8_t *p, size_t len, zw_cc_mc_association_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, ZW_CMD_ASSOCIATION_GET, 3u) ||
        out == NULL) {
        return false;
    }
    out->group = p[2];
    return true;
}

size_t zw_cc_mc_association_report_build(uint8_t *out, size_t cap,
                                          const zw_cc_mc_association_report_t *in)
{
    if (out == NULL || in == NULL || (in->destinations == NULL && in->destinations_len > 0u)) {
        return 0u;
    }
    const size_t total = 5u + in->destinations_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_REPORT;
    out[2] = in->group;
    out[3] = in->max_nodes;
    out[4] = in->reports_to_follow;
    for (size_t i = 0; i < in->destinations_len; i++) {
        out[5u + i] = in->destinations[i];
    }
    return total;
}

bool zw_cc_mc_association_report_parse(const uint8_t *p, size_t len,
                                        zw_cc_mc_association_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, ZW_CMD_ASSOCIATION_REPORT, 5u) ||
        out == NULL) {
        return false;
    }
    out->group = p[2];
    out->max_nodes = p[3];
    out->reports_to_follow = p[4];
    out->destinations_len = len - 5u;
    out->destinations = out->destinations_len > 0u ? &p[5] : NULL;
    return true;
}

size_t zw_cc_mc_association_remove_build(uint8_t *out, size_t cap,
                                          const zw_cc_mc_association_remove_t *in)
{
    if (out == NULL || in == NULL || (in->destinations == NULL && in->destinations_len > 0u)) {
        return 0u;
    }
    const size_t total = 3u + in->destinations_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_REMOVE;
    out[2] = in->group;
    for (size_t i = 0; i < in->destinations_len; i++) {
        out[3u + i] = in->destinations[i];
    }
    return total;
}

bool zw_cc_mc_association_remove_parse(const uint8_t *p, size_t len,
                                        zw_cc_mc_association_remove_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, ZW_CMD_ASSOCIATION_REMOVE, 3u) ||
        out == NULL) {
        return false;
    }
    out->group = p[2];
    out->destinations_len = len - 3u;
    out->destinations = out->destinations_len > 0u ? &p[3] : NULL;
    return true;
}

size_t zw_cc_mc_association_supported_groupings_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET;
    return 2u;
}

bool zw_cc_mc_association_supported_groupings_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION,
                             ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET, 2u);
}

size_t zw_cc_mc_association_supported_groupings_report_build(
    uint8_t *out, size_t cap, const zw_cc_mc_association_supported_groupings_report_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    out[1] = ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT;
    out[2] = in->group_count;
    return 3u;
}

bool zw_cc_mc_association_supported_groupings_report_parse(
    const uint8_t *p, size_t len, zw_cc_mc_association_supported_groupings_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION,
                           ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    out->group_count = p[2];
    return true;
}

/* ---- §4.10 Association Group Information ---------------------------------------------------------- */

size_t zw_cc_agi_name_get_build(uint8_t *out, size_t cap, const zw_cc_agi_name_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION;
    out[1] = ZW_CMD_AGI_NAME_GET;
    out[2] = in->group;
    return 3u;
}

bool zw_cc_agi_name_get_parse(const uint8_t *p, size_t len, zw_cc_agi_name_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, ZW_CMD_AGI_NAME_GET, 3u) ||
        out == NULL) {
        return false;
    }
    out->group = p[2];
    return true;
}

size_t zw_cc_agi_name_report_build(uint8_t *out, size_t cap, const zw_cc_agi_name_report_t *in)
{
    if (out == NULL || in == NULL || (in->name == NULL && in->name_len > 0u) || in->name_len > 0xFFu) {
        return 0u;
    }
    const size_t total = 4u + in->name_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION;
    out[1] = ZW_CMD_AGI_NAME_REPORT;
    out[2] = in->group;
    out[3] = (uint8_t)in->name_len;
    for (size_t i = 0; i < in->name_len; i++) {
        out[4u + i] = (uint8_t)in->name[i];
    }
    return total;
}

bool zw_cc_agi_name_report_parse(const uint8_t *p, size_t len, zw_cc_agi_name_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, ZW_CMD_AGI_NAME_REPORT, 4u) ||
        out == NULL) {
        return false;
    }
    const size_t nlen = p[3];
    if (len < 4u + nlen) {
        return false;
    }
    out->group = p[2];
    out->name_len = nlen;
    out->name = nlen > 0u ? (const char *)&p[4] : NULL;
    return true;
}

size_t zw_cc_agi_info_get_build(uint8_t *out, size_t cap, const zw_cc_agi_info_get_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION;
    out[1] = ZW_CMD_AGI_INFO_GET;
    out[2] = (uint8_t)((in->refresh_cache ? 0x80u : 0x00u) | (in->list_mode ? 0x40u : 0x00u));
    out[3] = in->group;
    return 4u;
}

bool zw_cc_agi_info_get_parse(const uint8_t *p, size_t len, zw_cc_agi_info_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, ZW_CMD_AGI_INFO_GET, 4u) ||
        out == NULL) {
        return false;
    }
    out->refresh_cache = ((unsigned)p[2] & 0x80u) != 0u;
    out->list_mode = (p[2] & 0x40u) != 0u;
    out->group = p[3];
    return true;
}

bool zw_cc_agi_group_info_get(const uint8_t *groups, size_t groups_len, size_t index,
                               zw_cc_agi_group_info_t *out)
{
    if (groups == NULL || out == NULL) {
        return false;
    }
    const size_t off = index * 7u;
    if (off + 7u > groups_len) {
        return false;
    }
    const uint8_t *g = &groups[off];
    out->group = g[0];
    /* g[1] = mode (00); g[2..3] = profile BE; g[4] = reserved; g[5..6] = event code BE. */
    out->profile = (uint16_t)(((uint16_t)g[2] << 8) | g[3]);
    out->event_code = (uint16_t)(((uint16_t)g[5] << 8) | g[6]);
    return true;
}

size_t zw_cc_agi_info_report_build(uint8_t *out, size_t cap, const zw_cc_agi_info_report_t *in)
{
    if (out == NULL || in == NULL || in->count > 0x3Fu) {
        return 0u;
    }
    if (in->groups_len != (size_t)in->count * 7u || (in->groups == NULL && in->groups_len > 0u)) {
        return 0u;
    }
    const size_t total = 3u + in->groups_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION;
    out[1] = ZW_CMD_AGI_INFO_REPORT;
    out[2] = (uint8_t)((in->list_mode ? 0x80u : 0x00u) | (in->dynamic ? 0x40u : 0x00u) | (in->count & 0x3Fu));
    for (size_t i = 0; i < in->groups_len; i++) {
        out[3u + i] = in->groups[i];
    }
    return total;
}

bool zw_cc_agi_info_report_parse(const uint8_t *p, size_t len, zw_cc_agi_info_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, ZW_CMD_AGI_INFO_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    const uint8_t flags = p[2];
    const uint8_t count = (uint8_t)(flags & 0x3Fu);
    const size_t need = 3u + (size_t)count * 7u;
    if (len < need) {
        return false;
    }
    out->list_mode = ((unsigned)flags & 0x80u) != 0u;
    out->dynamic = (flags & 0x40u) != 0u;
    out->count = count;
    out->groups_len = (size_t)count * 7u;
    out->groups = out->groups_len > 0u ? &p[3] : NULL;
    return true;
}

size_t zw_cc_agi_command_list_get_build(uint8_t *out, size_t cap, const zw_cc_agi_command_list_get_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION;
    out[1] = ZW_CMD_AGI_COMMAND_LIST_GET;
    out[2] = in->flags;
    out[3] = in->group;
    return 4u;
}

bool zw_cc_agi_command_list_get_parse(const uint8_t *p, size_t len, zw_cc_agi_command_list_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, ZW_CMD_AGI_COMMAND_LIST_GET, 4u) ||
        out == NULL) {
        return false;
    }
    out->flags = p[2];
    out->group = p[3];
    return true;
}

size_t zw_cc_agi_command_list_report_build(uint8_t *out, size_t cap,
                                            const zw_cc_agi_command_list_report_t *in)
{
    if (out == NULL || in == NULL || (in->cc_command_pairs == NULL && in->cc_command_pairs_len > 0u) ||
        in->cc_command_pairs_len > 0xFFu) {
        return 0u;
    }
    const size_t total = 4u + in->cc_command_pairs_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION;
    out[1] = ZW_CMD_AGI_COMMAND_LIST_REPORT;
    out[2] = in->group;
    out[3] = (uint8_t)in->cc_command_pairs_len;
    for (size_t i = 0; i < in->cc_command_pairs_len; i++) {
        out[4u + i] = in->cc_command_pairs[i];
    }
    return total;
}

bool zw_cc_agi_command_list_report_parse(const uint8_t *p, size_t len,
                                          zw_cc_agi_command_list_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION, ZW_CMD_AGI_COMMAND_LIST_REPORT,
                           4u) ||
        out == NULL) {
        return false;
    }
    const size_t plen = p[3];
    if (len < 4u + plen) {
        return false;
    }
    out->group = p[2];
    out->cc_command_pairs_len = plen;
    out->cc_command_pairs = plen > 0u ? &p[4] : NULL;
    return true;
}

/* ---- §4.11 Version -------------------------------------------------------------------------------- */

size_t zw_cc_version_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_VERSION;
    out[1] = ZW_CMD_VERSION_GET;
    return 2u;
}

bool zw_cc_version_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_VERSION, ZW_CMD_VERSION_GET, 2u);
}

size_t zw_cc_version_report_build(uint8_t *out, size_t cap, const zw_cc_version_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    if (in->has_v2 && in->other_target_count > 0u && in->other_targets == NULL) {
        return 0u;
    }
    const size_t total = in->has_v2 ? (9u + (size_t)in->other_target_count * 2u) : 7u;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_VERSION;
    out[1] = ZW_CMD_VERSION_REPORT;
    out[2] = in->library_type;
    out[3] = in->protocol_major;
    out[4] = in->protocol_minor;
    out[5] = in->firmware0_major;
    out[6] = in->firmware0_minor;
    if (in->has_v2) {
        out[7] = in->hardware_version;
        out[8] = in->other_target_count;
        for (size_t i = 0; i < (size_t)in->other_target_count * 2u; i++) {
            out[9u + i] = in->other_targets[i];
        }
    }
    return total;
}

bool zw_cc_version_report_parse(const uint8_t *p, size_t len, zw_cc_version_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_VERSION, ZW_CMD_VERSION_REPORT, 7u) || out == NULL) {
        return false;
    }
    out->library_type = p[2];
    out->protocol_major = p[3];
    out->protocol_minor = p[4];
    out->firmware0_major = p[5];
    out->firmware0_minor = p[6];
    out->has_v2 = len >= 9u;
    if (out->has_v2) {
        out->hardware_version = p[7];
        const uint8_t n = p[8];
        const size_t need = 9u + (size_t)n * 2u;
        if (len < need) {
            return false;
        }
        out->other_target_count = n;
        out->other_targets = n > 0u ? &p[9] : NULL;
    } else {
        out->hardware_version = 0u;
        out->other_targets = NULL;
        out->other_target_count = 0u;
    }
    return true;
}

size_t zw_cc_version_command_class_get_build(uint8_t *out, size_t cap,
                                              const zw_cc_version_command_class_get_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_VERSION;
    out[1] = ZW_CMD_VERSION_COMMAND_CLASS_GET;
    out[2] = in->cc;
    return 3u;
}

bool zw_cc_version_command_class_get_parse(const uint8_t *p, size_t len,
                                            zw_cc_version_command_class_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_VERSION, ZW_CMD_VERSION_COMMAND_CLASS_GET, 3u) || out == NULL) {
        return false;
    }
    out->cc = p[2];
    return true;
}

size_t zw_cc_version_command_class_report_build(uint8_t *out, size_t cap,
                                                 const zw_cc_version_command_class_report_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_VERSION;
    out[1] = ZW_CMD_VERSION_COMMAND_CLASS_REPORT;
    out[2] = in->cc;
    out[3] = in->version;
    return 4u;
}

bool zw_cc_version_command_class_report_parse(const uint8_t *p, size_t len,
                                               zw_cc_version_command_class_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_VERSION, ZW_CMD_VERSION_COMMAND_CLASS_REPORT, 4u) ||
        out == NULL) {
        return false;
    }
    out->cc = p[2];
    out->version = p[3];
    return true;
}

size_t zw_cc_version_capabilities_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_VERSION;
    out[1] = ZW_CMD_VERSION_CAPABILITIES_GET;
    return 2u;
}

bool zw_cc_version_capabilities_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_VERSION, ZW_CMD_VERSION_CAPABILITIES_GET, 2u);
}

size_t zw_cc_version_capabilities_report_build(uint8_t *out, size_t cap,
                                                const zw_cc_version_capabilities_report_t *in)
{
    if (out == NULL || in == NULL || cap < 3u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_VERSION;
    out[1] = ZW_CMD_VERSION_CAPABILITIES_REPORT;
    out[2] = (uint8_t)((in->supports_version_command ? 0x01u : 0x00u) |
                        (in->supports_command_class_command ? 0x02u : 0x00u) |
                        (in->supports_zwave_software_get ? 0x04u : 0x00u));
    return 3u;
}

bool zw_cc_version_capabilities_report_parse(const uint8_t *p, size_t len,
                                              zw_cc_version_capabilities_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_VERSION, ZW_CMD_VERSION_CAPABILITIES_REPORT, 3u) ||
        out == NULL) {
        return false;
    }
    out->supports_version_command = (p[2] & 0x01u) != 0u;
    out->supports_command_class_command = (p[2] & 0x02u) != 0u;
    out->supports_zwave_software_get = (p[2] & 0x04u) != 0u;
    return true;
}

/* ---- §4.12 Manufacturer Specific ------------------------------------------------------------------ */

size_t zw_cc_manufacturer_specific_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC;
    out[1] = ZW_CMD_MANUFACTURER_SPECIFIC_GET;
    return 2u;
}

bool zw_cc_manufacturer_specific_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC, ZW_CMD_MANUFACTURER_SPECIFIC_GET, 2u);
}

size_t zw_cc_manufacturer_specific_report_build(uint8_t *out, size_t cap,
                                                 const zw_cc_manufacturer_specific_report_t *in)
{
    if (out == NULL || in == NULL || cap < 8u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC;
    out[1] = ZW_CMD_MANUFACTURER_SPECIFIC_REPORT;
    out[2] = (uint8_t)(in->manufacturer_id >> 8);
    out[3] = (uint8_t)in->manufacturer_id;
    out[4] = (uint8_t)(in->product_type >> 8);
    out[5] = (uint8_t)in->product_type;
    out[6] = (uint8_t)(in->product_id >> 8);
    out[7] = (uint8_t)in->product_id;
    return 8u;
}

bool zw_cc_manufacturer_specific_report_parse(const uint8_t *p, size_t len,
                                               zw_cc_manufacturer_specific_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC, ZW_CMD_MANUFACTURER_SPECIFIC_REPORT, 8u) ||
        out == NULL) {
        return false;
    }
    out->manufacturer_id = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    out->product_type = (uint16_t)(((uint16_t)p[4] << 8) | p[5]);
    out->product_id = (uint16_t)(((uint16_t)p[6] << 8) | p[7]);
    return true;
}

/* ---- §4.13 Z-Wave Plus Info ------------------------------------------------------------------------ */

size_t zw_cc_zwave_plus_info_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ZWAVE_PLUS_INFO;
    out[1] = ZW_CMD_ZWAVE_PLUS_INFO_GET;
    return 2u;
}

bool zw_cc_zwave_plus_info_get_parse(const uint8_t *p, size_t len)
{
    return zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ZWAVE_PLUS_INFO, ZW_CMD_ZWAVE_PLUS_INFO_GET, 2u);
}

size_t zw_cc_zwave_plus_info_report_build(uint8_t *out, size_t cap,
                                           const zw_cc_zwave_plus_info_report_t *in)
{
    if (out == NULL || in == NULL || cap < 9u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_ZWAVE_PLUS_INFO;
    out[1] = ZW_CMD_ZWAVE_PLUS_INFO_REPORT;
    out[2] = in->version;
    out[3] = in->role_type;
    out[4] = in->node_type;
    out[5] = (uint8_t)(in->installer_icon >> 8);
    out[6] = (uint8_t)in->installer_icon;
    out[7] = (uint8_t)(in->user_icon >> 8);
    out[8] = (uint8_t)in->user_icon;
    return 9u;
}

bool zw_cc_zwave_plus_info_report_parse(const uint8_t *p, size_t len,
                                         zw_cc_zwave_plus_info_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_ZWAVE_PLUS_INFO, ZW_CMD_ZWAVE_PLUS_INFO_REPORT, 9u) ||
        out == NULL) {
        return false;
    }
    out->version = p[2];
    out->role_type = p[3];
    out->node_type = p[4];
    out->installer_icon = (uint16_t)(((uint16_t)p[5] << 8) | p[6]);
    out->user_icon = (uint16_t)(((uint16_t)p[7] << 8) | p[8]);
    return true;
}
