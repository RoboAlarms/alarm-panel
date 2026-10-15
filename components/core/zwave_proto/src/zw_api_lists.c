/*
 * zw_api_lists.c - the §3 bit-mask and list helpers (node lists, Long Range segments, the
 * function mask, CC-list iteration), the node-ID codec, the TX report and the two payload shapes
 * ApplicationUpdate shares with AddNodeToNetwork's callback (see zw_api.h).
 */
#include "zwave_proto/zw_api.h"

/* ---- node IDs ----------------------------------------------------------------------------- */

size_t zw_api_node_id_build(uint8_t *out, size_t cap, zw_nid_width_t w, uint16_t node_id)
{
    if (out == NULL || cap < (size_t)w) {
        return 0u;
    }
    if (w == ZW_NID_8) {
        if (node_id > 0xFFu) {
            return 0u;
        }
        out[0] = (uint8_t)node_id;
        return 1u;
    }
    out[0] = (uint8_t)(node_id >> 8);
    out[1] = (uint8_t)node_id;
    return 2u;
}

bool zw_api_node_id_parse(const uint8_t *p, size_t len, zw_nid_width_t w, uint16_t *node_id)
{
    if (p == NULL || node_id == NULL || len < (size_t)w) {
        return false;
    }
    if (w == ZW_NID_8) {
        *node_id = p[0];
    } else {
        *node_id = (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
    }
    return true;
}

/* ---- classic node list (29 bytes, nodes 1..232) -------------------------------------------- */

bool zw_node_list_test(const uint8_t list[ZW_NODE_LIST_BYTES], uint16_t node_id)
{
    if (list == NULL || node_id < 1u || node_id > 232u) {
        return false;
    }
    const unsigned local = (unsigned)node_id - 1u;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    return (((unsigned)list[b] >> n) & 1u) != 0u;
}

void zw_node_list_set(uint8_t list[ZW_NODE_LIST_BYTES], uint16_t node_id, bool present)
{
    if (list == NULL || node_id < 1u || node_id > 232u) {
        return;
    }
    const unsigned local = (unsigned)node_id - 1u;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    if (present) {
        list[b] = (uint8_t)((unsigned)list[b] | (1u << n));
    } else {
        list[b] = (uint8_t)((unsigned)list[b] & ~(1u << n));
    }
}

size_t zw_node_list_iterate(const uint8_t list[ZW_NODE_LIST_BYTES], uint16_t *out, size_t max)
{
    if (list == NULL) {
        return 0u;
    }
    size_t count = 0u;
    for (uint16_t node = 1u; node <= 232u; node++) {
        if (zw_node_list_test(list, node)) {
            if (out != NULL && count < max) {
                out[count] = node;
            }
            count++;
        }
    }
    return count;
}

/* ---- Long Range segments (16 bytes = 128 nodes; segment s starts at node 256+128*s) --------- */

bool zw_lr_segment_test(const uint8_t seg[ZW_LR_SEGMENT_BYTES], unsigned seg_index, uint16_t node_id)
{
    if (seg == NULL) {
        return false;
    }
    const unsigned base = 256u + 128u * seg_index;
    if ((unsigned)node_id < base || (unsigned)node_id >= base + 128u) {
        return false;
    }
    const unsigned local = (unsigned)node_id - base;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    return (((unsigned)seg[b] >> n) & 1u) != 0u;
}

void zw_lr_segment_set(uint8_t seg[ZW_LR_SEGMENT_BYTES], unsigned seg_index, uint16_t node_id, bool present)
{
    if (seg == NULL) {
        return;
    }
    const unsigned base = 256u + 128u * seg_index;
    if ((unsigned)node_id < base || (unsigned)node_id >= base + 128u) {
        return;
    }
    const unsigned local = (unsigned)node_id - base;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    if (present) {
        seg[b] = (uint8_t)((unsigned)seg[b] | (1u << n));
    } else {
        seg[b] = (uint8_t)((unsigned)seg[b] & ~(1u << n));
    }
}

size_t zw_lr_segment_iterate(const uint8_t seg[ZW_LR_SEGMENT_BYTES], unsigned seg_index,
                              uint16_t *out, size_t max)
{
    if (seg == NULL) {
        return 0u;
    }
    const unsigned base = 256u + 128u * seg_index;
    size_t count = 0u;
    for (unsigned local = 0u; local < 128u; local++) {
        const unsigned node = base + local;
        if (node > 0xFFFFu) {
            break; /* past uint16_t range: no valid node ID reaches here */
        }
        if (zw_lr_segment_test(seg, seg_index, (uint16_t)node)) {
            if (out != NULL && count < max) {
                out[count] = (uint16_t)node;
            }
            count++;
        }
    }
    return count;
}

/* ---- function mask (32 bytes) --------------------------------------------------------------- */

bool zw_func_mask_test(const uint8_t mask[ZW_FUNC_MASK_BYTES], uint8_t func)
{
    if (mask == NULL || func == 0u) {
        return false;
    }
    const unsigned local = (unsigned)func - 1u;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    if (b >= ZW_FUNC_MASK_BYTES) {
        return false;
    }
    return (((unsigned)mask[b] >> n) & 1u) != 0u;
}

/* ---- command-class list iteration ------------------------------------------------------------ */

void zw_cc_iter_init(zw_cc_iter_t *it, const uint8_t *list, size_t len)
{
    it->pos = list;
    it->end = len != 0u ? list + len : list;
    it->controlled = false;
}

bool zw_cc_iter_next(zw_cc_iter_t *it, uint16_t *cc_id, bool *controlled)
{
    while (it->pos < it->end && *it->pos == 0xEFu) {
        it->controlled = true;
        it->pos++;
    }
    if (it->pos >= it->end) {
        return false;
    }
    const uint8_t b0 = *it->pos;
    if (b0 >= 0xF1u) {
        if (it->pos + 1 >= it->end) {
            return false; /* a two-byte identifier truncated by the list's own length */
        }
        *cc_id = (uint16_t)(((unsigned)b0 << 8) | (unsigned)it->pos[1]);
        it->pos += 2;
    } else {
        *cc_id = b0;
        it->pos += 1;
    }
    *controlled = it->controlled;
    return true;
}

bool zw_cc_list_is_well_formed(const uint8_t *list, size_t len)
{
    zw_cc_iter_t it;
    zw_cc_iter_init(&it, list, len);
    uint16_t id;
    bool controlled;
    while (zw_cc_iter_next(&it, &id, &controlled)) {
        /* just walking it */
    }
    return it.pos == it.end;
}

/* ---- SendData's TX report --------------------------------------------------------------------- */

size_t zw_api_tx_report_build(uint8_t *out, size_t cap, const zw_api_tx_report_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    if (in->has_first_failing_node && !in->has_last_working_node) {
        return 0u;
    }
    if (in->has_long_range && !(in->has_last_working_node && in->has_first_failing_node)) {
        return 0u;
    }
    size_t total = 17u;
    if (in->has_last_working_node) {
        total = 18u;
    }
    if (in->has_first_failing_node) {
        total = 19u;
    }
    if (in->has_long_range) {
        total = 24u;
    }
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)(in->transmit_ticks >> 8);
    out[1] = (uint8_t)in->transmit_ticks;
    out[2] = in->repeater_count;
    out[3] = in->ack_rssi;
    for (size_t i = 0; i < 4u; i++) {
        out[4u + i] = in->ack_rssi_per_repeater[i];
    }
    out[8] = in->ack_channel;
    out[9] = in->tx_channel;
    out[10] = in->route_scheme_state;
    for (size_t i = 0; i < 4u; i++) {
        out[11u + i] = in->repeater_node_ids[i];
    }
    out[15] = in->beam_and_route_speed;
    out[16] = in->routing_attempts;
    if (total >= 18u) {
        out[17] = in->last_working_node;
    }
    if (total >= 19u) {
        out[18] = in->first_failing_node;
    }
    if (total == 24u) {
        out[19] = (uint8_t)in->lr_tx_power;
        out[20] = in->lr_noise_floor;
        out[21] = (uint8_t)in->lr_dest_ack_tx_power;
        out[22] = (uint8_t)in->lr_dest_ack_rssi;
        out[23] = in->lr_dest_noise_floor;
    }
    return total;
}

bool zw_api_tx_report_parse(const uint8_t *p, size_t len, zw_api_tx_report_t *out)
{
    if (p == NULL || out == NULL || len < 17u) {
        return false;
    }
    out->transmit_ticks = (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
    out->repeater_count = p[2];
    out->ack_rssi = p[3];
    for (size_t i = 0; i < 4u; i++) {
        out->ack_rssi_per_repeater[i] = p[4u + i];
    }
    out->ack_channel = p[8];
    out->tx_channel = p[9];
    out->route_scheme_state = p[10];
    for (size_t i = 0; i < 4u; i++) {
        out->repeater_node_ids[i] = p[11u + i];
    }
    out->beam_and_route_speed = p[15];
    out->routing_attempts = p[16];
    out->has_last_working_node = len >= 18u;
    out->last_working_node = len >= 18u ? (unsigned)p[17] : 0u;
    out->has_first_failing_node = len >= 19u;
    out->first_failing_node = len >= 19u ? (unsigned)p[18] : 0u;
    out->has_long_range = len >= 24u; /* bytes past 24 are further, still-unknown fields: ignored */
    if (len >= 24u) {
        out->lr_tx_power = (int8_t)p[19];
        out->lr_noise_floor = p[20];
        out->lr_dest_ack_tx_power = (int8_t)p[21];
        out->lr_dest_ack_rssi = (int8_t)p[22];
        out->lr_dest_noise_floor = p[23];
    } else {
        out->lr_tx_power = 0;
        out->lr_noise_floor = 0;
        out->lr_dest_ack_tx_power = 0;
        out->lr_dest_ack_rssi = 0;
        out->lr_dest_noise_floor = 0;
    }
    return true;
}

/* ---- the node_update shape (ApplicationUpdate 84/40, AddNode callback 03/04) ----------------- */

size_t zw_api_node_update_build(uint8_t *out, size_t cap, zw_nid_width_t w, const zw_api_node_update_t *in)
{
    if (out == NULL || in == NULL || (in->cc_list == NULL && in->cc_list_len > 0u) ||
        in->cc_list_len > 0xFFu - 3u) {
        return 0u;
    }
    const size_t l = 3u + in->cc_list_len;
    const size_t total = (size_t)w + 1u + l;
    if (cap < total) {
        return 0u;
    }
    size_t pos = zw_api_node_id_build(out, cap, w, in->node_id);
    if (pos == 0u) {
        return 0u;
    }
    out[pos] = (uint8_t)l;
    pos += 1u;
    out[pos] = in->basic_class;
    pos += 1u;
    out[pos] = in->generic_class;
    pos += 1u;
    out[pos] = in->specific_class;
    pos += 1u;
    for (size_t i = 0; i < in->cc_list_len; i++) {
        out[pos + i] = in->cc_list[i];
    }
    return total;
}

bool zw_api_node_update_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_api_node_update_t *out)
{
    if (p == NULL || out == NULL || len < (size_t)w + 4u) {
        return false;
    }
    uint16_t node_id;
    if (!zw_api_node_id_parse(p, (size_t)w, w, &node_id)) {
        return false;
    }
    const uint8_t l = p[(size_t)w];
    if (l < 3u || len < (size_t)w + 1u + (size_t)l) {
        return false;
    }
    out->node_id = node_id;
    out->basic_class = p[(size_t)w + 1u];
    out->generic_class = p[(size_t)w + 2u];
    out->specific_class = p[(size_t)w + 3u];
    const size_t cc_len = (size_t)l - 3u;
    out->cc_list_len = cc_len;
    out->cc_list = cc_len > 0u ? &p[(size_t)w + 4u] : NULL;
    return true;
}

/* ---- the SmartStart join shape (ApplicationUpdate 85/86/87) ---------------------------------- */

size_t zw_api_smartstart_join_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                     const zw_api_smartstart_join_t *in)
{
    if (out == NULL || in == NULL || (in->cc_list == NULL && in->cc_list_len > 0u) ||
        in->cc_list_len > 0xFFu) {
        return 0u;
    }
    const size_t total = (size_t)w + 9u + in->cc_list_len;
    if (cap < total) {
        return 0u;
    }
    size_t pos = zw_api_node_id_build(out, cap, w, in->node_id);
    if (pos == 0u) {
        return 0u;
    }
    out[pos] = in->receive_status;
    pos += 1u;
    for (size_t i = 0; i < 4u; i++) {
        out[pos + i] = in->nwi_home_id[i];
    }
    pos += 4u;
    out[pos] = (uint8_t)in->cc_list_len;
    pos += 1u;
    out[pos] = in->basic_class;
    pos += 1u;
    out[pos] = in->generic_class;
    pos += 1u;
    out[pos] = in->specific_class;
    pos += 1u;
    for (size_t i = 0; i < in->cc_list_len; i++) {
        out[pos + i] = in->cc_list[i];
    }
    return total;
}

bool zw_api_smartstart_join_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                   zw_api_smartstart_join_t *out)
{
    if (p == NULL || out == NULL || len < (size_t)w + 9u) {
        return false;
    }
    uint16_t node_id;
    if (!zw_api_node_id_parse(p, (size_t)w, w, &node_id)) {
        return false;
    }
    size_t off = (size_t)w;
    const uint8_t status = p[off];
    off += 1u;
    uint8_t nwi[4];
    for (size_t i = 0; i < 4u; i++) {
        nwi[i] = p[off + i];
    }
    off += 4u;
    const uint8_t cclen = p[off];
    off += 1u;
    if (len < (size_t)w + 9u + (size_t)cclen) {
        return false;
    }
    const uint8_t basic = p[off];
    off += 1u;
    const uint8_t generic = p[off];
    off += 1u;
    const uint8_t specific = p[off];
    off += 1u;
    out->node_id = node_id;
    out->receive_status = status;
    for (size_t i = 0; i < 4u; i++) {
        out->nwi_home_id[i] = nwi[i];
    }
    out->basic_class = basic;
    out->generic_class = generic;
    out->specific_class = specific;
    out->cc_list_len = cclen;
    out->cc_list = cclen > 0u ? &p[off] : NULL;
    return true;
}
