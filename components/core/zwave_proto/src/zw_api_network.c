/*
 * zw_api_network.c - network traffic and membership (serial-api.md §6.11-§6.12, §6.16-§6.17):
 * ApplicationCommandHandler / BridgeApplicationCommandHandler, ApplicationUpdate,
 * AddNodeToNetwork, RemoveNodeFromNetwork and SendData.
 *
 * What the panel hears from the module (app_cmd, app_update, every _rsp/_cb) is parsed with a
 * minimum length only: trailing bytes a newer module firmware might append are accepted and
 * ignored. What the panel sends (every _req) stays strict, since only a fake module ever parses
 * it, to check exactly what the host built.
 */
#include "zwave_proto/zw_api.h"

/* ---- `04`/`A8` ApplicationCommandHandler (§6.11) ------------------------------------------------ */

size_t zw_api_app_cmd_build(uint8_t *out, size_t cap, zw_nid_width_t w, const zw_api_app_cmd_t *in)
{
    if (out == NULL || in == NULL || (in->command == NULL && in->command_len > 0u) ||
        in->command_len > 0xFFu) {
        return 0u;
    }
    if (!in->is_bridge) {
        const size_t total = 1u + (size_t)w + 1u + in->command_len + (in->has_rssi ? 1u : 0u);
        if (cap < total) {
            return 0u;
        }
        out[0] = in->receive_status;
        size_t pos = 1u;
        const size_t n = zw_api_node_id_build(&out[pos], cap - pos, w, in->source_node_id);
        if (n == 0u) {
            return 0u;
        }
        pos += n;
        out[pos] = (uint8_t)in->command_len;
        pos += 1u;
        for (size_t i = 0; i < in->command_len; i++) {
            out[pos + i] = in->command[i];
        }
        pos += in->command_len;
        if (in->has_rssi) {
            out[pos] = (uint8_t)in->rssi;
            pos += 1u;
        }
        return pos;
    }
    if ((in->multicast_mask == NULL && in->multicast_mask_len > 0u) || in->multicast_mask_len > 0xFFu) {
        return 0u;
    }
    const size_t total = 1u + 2u * (size_t)w + 1u + in->command_len + 1u + in->multicast_mask_len +
                          (in->has_rssi ? 1u : 0u);
    if (cap < total) {
        return 0u;
    }
    out[0] = in->receive_status;
    size_t pos = 1u;
    size_t n = zw_api_node_id_build(&out[pos], cap - pos, w, in->destination_node_id);
    if (n == 0u) {
        return 0u;
    }
    pos += n;
    n = zw_api_node_id_build(&out[pos], cap - pos, w, in->source_node_id);
    if (n == 0u) {
        return 0u;
    }
    pos += n;
    out[pos] = (uint8_t)in->command_len;
    pos += 1u;
    for (size_t i = 0; i < in->command_len; i++) {
        out[pos + i] = in->command[i];
    }
    pos += in->command_len;
    out[pos] = (uint8_t)in->multicast_mask_len;
    pos += 1u;
    for (size_t i = 0; i < in->multicast_mask_len; i++) {
        out[pos + i] = in->multicast_mask[i];
    }
    pos += in->multicast_mask_len;
    if (in->has_rssi) {
        out[pos] = (uint8_t)in->rssi;
        pos += 1u;
    }
    return pos;
}

/* The module's own frame: parse with a minimum length. RSSI, when present, is always the very
 * next byte after the mandatory part - anything past that is a newer firmware's own business. */
bool zw_api_app_cmd_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_func_t func,
                           zw_api_app_cmd_t *out)
{
    if (p == NULL || out == NULL) {
        return false;
    }
    if (func == ZW_FUNC_APPLICATION_COMMAND_HANDLER) {
        if (len < 1u + (size_t)w + 1u) {
            return false;
        }
        uint16_t src;
        if (!zw_api_node_id_parse(&p[1], (size_t)w, w, &src)) {
            return false;
        }
        size_t off = 1u + (size_t)w;
        const uint8_t n = p[off];
        off += 1u;
        const size_t base = off + (size_t)n;
        if (len < base) {
            return false; /* the CC length must still fit inside what was given */
        }
        out->is_bridge = false;
        out->receive_status = p[0];
        out->source_node_id = src;
        out->destination_node_id = 0u;
        out->command = n > 0u ? &p[off] : NULL;
        out->command_len = n;
        out->multicast_mask = NULL;
        out->multicast_mask_len = 0u;
        out->has_rssi = (len > base);
        out->rssi = out->has_rssi ? (int8_t)p[base] : 0;
        return true;
    }
    if (func == ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER) {
        if (len < 1u + 2u * (size_t)w + 1u) {
            return false;
        }
        uint16_t dst;
        uint16_t src;
        if (!zw_api_node_id_parse(&p[1], (size_t)w, w, &dst) ||
            !zw_api_node_id_parse(&p[1u + (size_t)w], (size_t)w, w, &src)) {
            return false;
        }
        size_t off = 1u + 2u * (size_t)w;
        const uint8_t n = p[off];
        off += 1u;
        if (len < off + (size_t)n + 1u) {
            return false;
        }
        const uint8_t *command = n > 0u ? &p[off] : NULL;
        off += (size_t)n;
        const uint8_t m = p[off];
        off += 1u;
        const size_t base = off + (size_t)m;
        if (len < base) {
            return false; /* the multicast mask length must still fit inside what was given */
        }
        out->is_bridge = true;
        out->receive_status = p[0];
        out->destination_node_id = dst;
        out->source_node_id = src;
        out->command = command;
        out->command_len = n;
        out->multicast_mask = m > 0u ? &p[off] : NULL;
        out->multicast_mask_len = m;
        out->has_rssi = (len > base);
        out->rssi = out->has_rssi ? (int8_t)p[base] : 0;
        return true;
    }
    return false;
}

/* ---- `49` ApplicationUpdate (§6.13) --------------------------------------------------------------- */

size_t zw_api_app_update_build(uint8_t *out, size_t cap, const zw_api_app_update_t *in)
{
    if (out == NULL || in == NULL || (in->data == NULL && in->data_len > 0u)) {
        return 0u;
    }
    const size_t total = 1u + in->data_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->type;
    for (size_t i = 0; i < in->data_len; i++) {
        out[1u + i] = in->data[i];
    }
    return total;
}

bool zw_api_app_update_parse(const uint8_t *p, size_t len, zw_api_app_update_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->type = p[0];
    out->data_len = len - 1u;
    out->data = out->data_len > 0u ? &p[1] : NULL;
    return true;
}

/* ---- `4A` AddNodeToNetwork, `4B` RemoveNodeFromNetwork status reports (§6.16, §6.17) ------------- */

/* The layout both callbacks share for every status (see zw_api_add_node_cb_t's comment):
 * callback ID | status | source node ID (nid) | L | L bytes of node info. Parsed here without any
 * status-specific requiredness - the two public wrappers below add that. */
static bool node_status_cb_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_api_add_node_cb_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    out->callback_id = p[0];
    out->status = p[1];
    out->has_node_info = false;
    out->node.node_id = 0u;
    out->node.basic_class = 0u;
    out->node.generic_class = 0u;
    out->node.specific_class = 0u;
    out->node.cc_list = NULL;
    out->node.cc_list_len = 0u;
    const uint8_t *rest = &p[2];
    const size_t rest_len = len - 2u;
    if (rest_len == 0u) {
        return true; /* ends right after the status byte: some firmware, and simple mocks */
    }
    if (rest_len < (size_t)w + 1u) {
        return false;
    }
    uint16_t node_id;
    if (!zw_api_node_id_parse(rest, (size_t)w, w, &node_id)) {
        return false;
    }
    out->node.node_id = node_id;
    const uint8_t l = rest[(size_t)w];
    if (l == 0u) {
        return true;
    }
    if (l < 3u) {
        return false; /* an internal length field must describe a real node-info shape */
    }
    if (!zw_api_node_update_parse(rest, rest_len, w, &out->node)) {
        return false; /* L must still fit inside what was given */
    }
    out->has_node_info = true;
    return true;
}

/* Always writes the full layout (callback ID | status | node ID | L | node info); L is 0 when
 * !has_node_info. */
static size_t node_status_cb_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                    const zw_api_add_node_cb_t *in)
{
    if (out == NULL || in == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = in->callback_id;
    out[1] = in->status;
    size_t pos = 2u;
    if (!in->has_node_info) {
        const size_t n = zw_api_node_id_build(&out[pos], cap - pos, w, in->node.node_id);
        if (n == 0u) {
            return 0u;
        }
        pos += n;
        if (cap - pos < 1u) {
            return 0u;
        }
        out[pos] = 0u; /* L */
        pos += 1u;
        return pos;
    }
    const size_t n = zw_api_node_update_build(&out[pos], cap - pos, w, &in->node);
    if (n == 0u) {
        return 0u;
    }
    pos += n;
    return pos;
}

size_t zw_api_add_node_req_build(uint8_t *out, size_t cap, const zw_api_add_node_req_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const bool dsk = (in->mode & ZW_ADD_NODE_TYPE_MASK) == ZW_ADD_NODE_TYPE_SMARTSTART_DSK;
    if (dsk != in->has_smartstart_dsk) {
        return 0u; /* the mode's type and the flag must agree */
    }
    const size_t total = dsk ? 2u + 4u + 4u : 2u;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->mode;
    out[1] = in->callback_id;
    if (dsk) {
        for (size_t i = 0; i < 4u; i++) {
            out[2u + i] = in->nwi_home_id[i];
        }
        for (size_t i = 0; i < 4u; i++) {
            out[6u + i] = in->auth_home_id[i];
        }
    }
    return total;
}

bool zw_api_add_node_req_parse(const uint8_t *p, size_t len, zw_api_add_node_req_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    const uint8_t mode = p[0];
    const bool dsk = (mode & ZW_ADD_NODE_TYPE_MASK) == ZW_ADD_NODE_TYPE_SMARTSTART_DSK;
    if (dsk) {
        if (len != 10u) {
            return false;
        }
        for (size_t i = 0; i < 4u; i++) {
            out->nwi_home_id[i] = p[2u + i];
        }
        for (size_t i = 0; i < 4u; i++) {
            out->auth_home_id[i] = p[6u + i];
        }
    } else {
        if (len != 2u) {
            return false;
        }
        for (size_t i = 0; i < 4u; i++) {
            out->nwi_home_id[i] = 0u;
            out->auth_home_id[i] = 0u;
        }
    }
    out->mode = mode;
    out->callback_id = p[1];
    out->has_smartstart_dsk = dsk;
    return true;
}

size_t zw_api_add_node_cb_build(uint8_t *out, size_t cap, zw_nid_width_t w, const zw_api_add_node_cb_t *in)
{
    return node_status_cb_build(out, cap, w, in);
}

bool zw_api_add_node_cb_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_api_add_node_cb_t *out)
{
    if (!node_status_cb_parse(p, len, w, out)) {
        return false;
    }
    /* Once anything follows the status byte, AddingEndNode/AddingController must carry real node
     * info - a report that stops right after the status byte is still accepted (see
     * node_status_cb_parse()). */
    if ((out->status == ZW_ADD_NODE_STATUS_ADDING_END_NODE ||
         out->status == ZW_ADD_NODE_STATUS_ADDING_CONTROLLER) &&
        len > 2u && !out->has_node_info) {
        return false;
    }
    return true;
}

/* ---- `4B` RemoveNodeFromNetwork (§6.17) -------------------------------------------------------------- */

size_t zw_api_remove_node_req_build(uint8_t *out, size_t cap, const zw_api_remove_node_req_t *in)
{
    if (out == NULL || in == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = in->mode;
    out[1] = in->callback_id;
    return 2u;
}

bool zw_api_remove_node_req_parse(const uint8_t *p, size_t len, zw_api_remove_node_req_t *out)
{
    if (p == NULL || out == NULL || len != 2u) {
        return false;
    }
    out->mode = p[0];
    out->callback_id = p[1];
    return true;
}

size_t zw_api_remove_node_cb_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                    const zw_api_remove_node_cb_t *in)
{
    return node_status_cb_build(out, cap, w, in);
}

bool zw_api_remove_node_cb_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                  zw_api_remove_node_cb_t *out)
{
    return node_status_cb_parse(p, len, w, out); /* RemoveNode's node info is always optional */
}

/* ---- `13` SendData (§6.12) ------------------------------------------------------------------------------ */

size_t zw_api_send_data_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                   const zw_api_send_data_req_t *in)
{
    if (out == NULL || in == NULL || (in->command == NULL && in->command_len > 0u) ||
        in->command_len > 0xFFu) {
        return 0u;
    }
    const size_t total = (size_t)w + 1u + in->command_len + 2u;
    if (cap < total) {
        return 0u;
    }
    size_t pos = zw_api_node_id_build(out, cap, w, in->node_id);
    if (pos == 0u) {
        return 0u;
    }
    out[pos] = (uint8_t)in->command_len;
    pos += 1u;
    for (size_t i = 0; i < in->command_len; i++) {
        out[pos + i] = in->command[i];
    }
    pos += in->command_len;
    out[pos] = in->transmit_options;
    pos += 1u;
    out[pos] = in->callback_id;
    pos += 1u;
    return pos;
}

/* A request: only the fake module parses it, to check exactly what the host built. Stays strict. */
bool zw_api_send_data_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                 zw_api_send_data_req_t *out)
{
    if (p == NULL || out == NULL || len < (size_t)w + 3u) {
        return false;
    }
    uint16_t nid;
    if (!zw_api_node_id_parse(p, (size_t)w, w, &nid)) {
        return false;
    }
    size_t off = (size_t)w;
    const uint8_t n = p[off];
    off += 1u;
    if (len != off + (size_t)n + 2u) {
        return false;
    }
    out->node_id = nid;
    out->command = n > 0u ? &p[off] : NULL;
    out->command_len = n;
    off += (size_t)n;
    out->transmit_options = p[off];
    off += 1u;
    out->callback_id = p[off];
    return true;
}

size_t zw_api_send_data_rsp_build(uint8_t *out, size_t cap, const zw_api_send_data_rsp_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = in->queued ? 0x01u : 0x00u;
    return 1u;
}

bool zw_api_send_data_rsp_parse(const uint8_t *p, size_t len, zw_api_send_data_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->queued = (p[0] != 0x00u); /* 00 = no, anything else = yes */
    return true;
}

size_t zw_api_send_data_cb_build(uint8_t *out, size_t cap, const zw_api_send_data_cb_t *in)
{
    if (out == NULL || in == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = in->callback_id;
    out[1] = in->transmit_status;
    size_t pos = 2u;
    if (in->has_tx_report) {
        const size_t n = zw_api_tx_report_build(&out[pos], cap - pos, &in->tx_report);
        if (n == 0u) {
            return 0u;
        }
        pos += n;
    }
    return pos;
}

/* Never fails a SendData callback over its TX report: fewer than 17 bytes after
 * callback_id/transmit_status simply means there is no report (SetTxStatusReport is off). */
bool zw_api_send_data_cb_parse(const uint8_t *p, size_t len, zw_api_send_data_cb_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    out->callback_id = p[0];
    out->transmit_status = p[1];
    const size_t rem = len - 2u;
    if (rem < 17u) {
        out->has_tx_report = false;
        return true;
    }
    out->has_tx_report = true;
    return zw_api_tx_report_parse(&p[2], rem, &out->tx_report);
}
