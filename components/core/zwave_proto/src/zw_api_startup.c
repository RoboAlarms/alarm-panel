/*
 * zw_api_startup.c - the start-up commands (serial-api.md §6.1-§6.10, §5): GetSerialApiInitData,
 * SetApplicationNodeInformation, GetControllerCapabilities, SetSerialApiTimeouts,
 * GetSerialApiCapabilities, GetProtocolVersion, SerialAPIStarted, GetControllerVersion,
 * GetControllerId, GetSUCNodeId, SetSUCNodeId, AssignSUCReturnRoute, GetNodeProtocolInfo,
 * HardReset, RequestNodeInfo, and GetLongRangeNodes (used at start-up, §5 step 13).
 */
#include "zwave_proto/zw_api.h"

/* ---- `02` GetSerialApiInitData (§6.3) --------------------------------------------------------- */

size_t zw_api_init_data_rsp_build(uint8_t *out, size_t cap, const zw_api_init_data_rsp_t *in)
{
    if (out == NULL || in == NULL || in->node_list == NULL) {
        return 0u;
    }
    const size_t total = 3u + ZW_NODE_LIST_BYTES + 2u;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->serial_api_version;
    uint8_t caps = 0u;
    if (!in->is_controller) {
        caps |= 0x01u;
    }
    if (in->has_timers) {
        caps |= 0x02u;
    }
    if (in->is_secondary) {
        caps |= 0x04u;
    }
    if (in->is_sis) {
        caps |= 0x08u;
    }
    out[1] = caps;
    out[2] = (uint8_t)ZW_NODE_LIST_BYTES;
    for (size_t i = 0; i < ZW_NODE_LIST_BYTES; i++) {
        out[3u + i] = in->node_list[i];
    }
    out[3u + ZW_NODE_LIST_BYTES] = in->chip_type;
    out[3u + ZW_NODE_LIST_BYTES + 1u] = in->chip_version;
    return total;
}

bool zw_api_init_data_rsp_parse(const uint8_t *p, size_t len, zw_api_init_data_rsp_t *out)
{
    const size_t total = 3u + ZW_NODE_LIST_BYTES + 2u;
    if (p == NULL || out == NULL || len < total || p[2] != (uint8_t)ZW_NODE_LIST_BYTES) {
        return false;
    }
    out->serial_api_version = p[0];
    const uint8_t caps = p[1];
    out->is_controller = (caps & 0x01u) == 0u;
    out->has_timers = (caps & 0x02u) != 0u;
    out->is_secondary = (caps & 0x04u) != 0u;
    out->is_sis = (caps & 0x08u) != 0u;
    out->node_list = &p[3];
    out->chip_type = p[3u + ZW_NODE_LIST_BYTES];
    out->chip_version = p[3u + ZW_NODE_LIST_BYTES + 1u];
    return true;
}

/* ---- `03` SetApplicationNodeInformation (§6.2) ------------------------------------------------ */

size_t zw_api_app_node_info_req_build(uint8_t *out, size_t cap, const zw_api_app_node_info_req_t *in)
{
    if (out == NULL || in == NULL || (in->cc_list == NULL && in->cc_list_len > 0u) ||
        in->cc_list_len > 35u) {
        return 0u;
    }
    const size_t total = 4u + in->cc_list_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->always_listening ? 0x01u : 0x00u;
    out[1] = in->generic_class;
    out[2] = in->specific_class;
    out[3] = (uint8_t)in->cc_list_len;
    for (size_t i = 0; i < in->cc_list_len; i++) {
        out[4u + i] = in->cc_list[i];
    }
    return total;
}

bool zw_api_app_node_info_req_parse(const uint8_t *p, size_t len, zw_api_app_node_info_req_t *out)
{
    if (p == NULL || out == NULL || len < 4u) {
        return false;
    }
    const uint8_t n = p[3];
    if (n > 35u || len != 4u + (size_t)n) {
        return false;
    }
    out->always_listening = (p[0] & 0x01u) != 0u;
    out->generic_class = p[1];
    out->specific_class = p[2];
    out->cc_list_len = n;
    out->cc_list = n > 0u ? &p[4] : NULL;
    return true;
}

/* ---- `05` GetControllerCapabilities (§6.6) ----------------------------------------------------- */

size_t zw_api_controller_caps_rsp_build(uint8_t *out, size_t cap, const zw_api_controller_caps_rsp_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    uint8_t f = 0u;
    if (in->is_secondary) {
        f |= 0x01u;
    }
    if (in->on_other_network) {
        f |= 0x02u;
    }
    if (in->sis_present) {
        f |= 0x04u;
    }
    if (in->was_real_primary) {
        f |= 0x08u;
    }
    if (in->is_suc) {
        f |= 0x10u;
    }
    if (in->no_nodes_included) {
        f |= 0x20u;
    }
    out[0] = f;
    return 1u;
}

bool zw_api_controller_caps_rsp_parse(const uint8_t *p, size_t len, zw_api_controller_caps_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    const uint8_t f = p[0];
    out->is_secondary = (f & 0x01u) != 0u;
    out->on_other_network = (f & 0x02u) != 0u;
    out->sis_present = (f & 0x04u) != 0u;
    out->was_real_primary = (f & 0x08u) != 0u;
    out->is_suc = (f & 0x10u) != 0u;
    out->no_nodes_included = (f & 0x20u) != 0u;
    return true;
}

/* ---- `06` SetSerialApiTimeouts (§6.6) ----------------------------------------------------------- */

size_t zw_api_api_timeouts_req_build(uint8_t *out, size_t cap, const zw_api_api_timeouts_req_t *in)
{
    if (out == NULL || in == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = in->ack_timeout_x10ms;
    out[1] = in->byte_timeout_x10ms;
    return 2u;
}

/* The two values, from a minimum of 2 bytes; a `_req` wrapper adds its own exact-length check. */
static bool api_timeouts_parse(const uint8_t *p, size_t len, zw_api_api_timeouts_req_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    out->ack_timeout_x10ms = p[0];
    out->byte_timeout_x10ms = p[1];
    return true;
}

bool zw_api_api_timeouts_req_parse(const uint8_t *p, size_t len, zw_api_api_timeouts_req_t *out)
{
    return len == 2u && api_timeouts_parse(p, len, out);
}

size_t zw_api_api_timeouts_rsp_build(uint8_t *out, size_t cap, const zw_api_api_timeouts_rsp_t *in)
{
    return zw_api_api_timeouts_req_build(out, cap, in);
}

bool zw_api_api_timeouts_rsp_parse(const uint8_t *p, size_t len, zw_api_api_timeouts_rsp_t *out)
{
    return api_timeouts_parse(p, len, out); /* the module's answer: trailing bytes are ignored */
}

/* ---- `07` GetSerialApiCapabilities (§6.1) -------------------------------------------------------- */

size_t zw_api_api_caps_rsp_build(uint8_t *out, size_t cap, const zw_api_api_caps_rsp_t *in)
{
    if (out == NULL || in == NULL || in->function_mask == NULL) {
        return 0u;
    }
    const size_t total = 8u + ZW_FUNC_MASK_BYTES;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->app_version_major;
    out[1] = in->app_version_minor;
    out[2] = (uint8_t)(in->manufacturer_id >> 8);
    out[3] = (uint8_t)in->manufacturer_id;
    out[4] = (uint8_t)(in->product_type >> 8);
    out[5] = (uint8_t)in->product_type;
    out[6] = (uint8_t)(in->product_id >> 8);
    out[7] = (uint8_t)in->product_id;
    for (size_t i = 0; i < ZW_FUNC_MASK_BYTES; i++) {
        out[8u + i] = in->function_mask[i];
    }
    return total;
}

bool zw_api_api_caps_rsp_parse(const uint8_t *p, size_t len, zw_api_api_caps_rsp_t *out)
{
    const size_t total = 8u + ZW_FUNC_MASK_BYTES;
    if (p == NULL || out == NULL || len < total) {
        return false;
    }
    out->app_version_major = p[0];
    out->app_version_minor = p[1];
    out->manufacturer_id = (uint16_t)(((unsigned)p[2] << 8) | (unsigned)p[3]);
    out->product_type = (uint16_t)(((unsigned)p[4] << 8) | (unsigned)p[5]);
    out->product_id = (uint16_t)(((unsigned)p[6] << 8) | (unsigned)p[7]);
    out->function_mask = &p[8];
    return true;
}

/* ---- `09` GetProtocolVersion (§6.5) --------------------------------------------------------------- */

size_t zw_api_protocol_version_rsp_build(uint8_t *out, size_t cap, const zw_api_protocol_version_rsp_t *in)
{
    if (out == NULL || in == NULL || in->git_hash == NULL) {
        return 0u;
    }
    const size_t total = 6u + 16u;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->protocol_type;
    out[1] = in->major;
    out[2] = in->minor;
    out[3] = in->patch;
    out[4] = (uint8_t)(in->app_framework_build >> 8);
    out[5] = (uint8_t)in->app_framework_build;
    for (size_t i = 0; i < 16u; i++) {
        out[6u + i] = in->git_hash[i];
    }
    return total;
}

bool zw_api_protocol_version_rsp_parse(const uint8_t *p, size_t len, zw_api_protocol_version_rsp_t *out)
{
    const size_t total = 6u + 16u;
    if (p == NULL || out == NULL || len < total) {
        return false;
    }
    out->protocol_type = p[0];
    out->major = p[1];
    out->minor = p[2];
    out->patch = p[3];
    out->app_framework_build = (uint16_t)(((unsigned)p[4] << 8) | (unsigned)p[5]);
    out->git_hash = &p[6];
    return true;
}

/* ---- `0A` SerialAPIStarted (§6.8) ------------------------------------------------------------------ */

size_t zw_api_started_build(uint8_t *out, size_t cap, const zw_api_started_t *in)
{
    if (out == NULL || in == NULL || (in->cc_list == NULL && in->cc_list_len > 0u) ||
        in->cc_list_len > 0xFFu) {
        return 0u;
    }
    size_t total = 6u + in->cc_list_len;
    if (in->has_supported_protocols) {
        total += 1u;
    }
    if (cap < total) {
        return 0u;
    }
    out[0] = in->wake_up_reason;
    out[1] = in->watchdog_on ? 0x01u : 0x00u;
    out[2] = in->always_listening ? 0x01u : 0x00u;
    out[3] = in->generic_class;
    out[4] = in->specific_class;
    out[5] = (uint8_t)in->cc_list_len;
    for (size_t i = 0; i < in->cc_list_len; i++) {
        out[6u + i] = in->cc_list[i];
    }
    if (in->has_supported_protocols) {
        out[6u + in->cc_list_len] = in->supported_protocols;
    }
    return total;
}

bool zw_api_started_parse(const uint8_t *p, size_t len, zw_api_started_t *out)
{
    if (p == NULL || out == NULL || len < 6u) {
        return false;
    }
    const uint8_t n = p[5];
    const size_t base = 6u + (size_t)n;
    if (len < base) {
        return false; /* the CC length must still fit inside what was given */
    }
    out->wake_up_reason = p[0];
    out->watchdog_on = (p[1] & 0x01u) != 0u;
    out->always_listening = (p[2] & 0x01u) != 0u;
    out->generic_class = p[3];
    out->specific_class = p[4];
    out->cc_list_len = n;
    out->cc_list = n > 0u ? &p[6] : NULL;
    out->has_supported_protocols = (len > base); /* one more byte follows; further ones ignored */
    out->supported_protocols = out->has_supported_protocols ? (unsigned)p[base] : 0u;
    return true;
}

/* ---- `15` GetControllerVersion (§6.4) --------------------------------------------------------------- */

size_t zw_api_controller_version_rsp_build(uint8_t *out, size_t cap,
                                            const zw_api_controller_version_rsp_t *in)
{
    if (out == NULL || in == NULL || (in->library == NULL && in->library_len > 0u)) {
        return 0u;
    }
    const size_t total = in->library_len + 2u;
    if (cap < total) {
        return 0u;
    }
    for (size_t i = 0; i < in->library_len; i++) {
        out[i] = (uint8_t)in->library[i];
    }
    out[in->library_len] = '\0';
    out[in->library_len + 1u] = in->library_type;
    return total;
}

bool zw_api_controller_version_rsp_parse(const uint8_t *p, size_t len,
                                          zw_api_controller_version_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    size_t nul = 0u;
    bool found = false;
    for (size_t i = 0; i < len - 1u; i++) {
        if (p[i] == 0u) {
            nul = i;
            found = true;
            break;
        }
    }
    /* At least one byte (the library type) must follow the NUL; bytes past it are a newer
     * firmware's own business and are ignored. */
    if (!found || nul + 2u > len) {
        return false;
    }
    out->library = (const char *)p;
    out->library_len = nul;
    out->library_type = p[nul + 1u]; /* the byte right after the NUL; anything further is trailing */
    return true;
}

/* ---- `20` GetControllerId (§6.9) ------------------------------------------------------------------- */

size_t zw_api_controller_id_rsp_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                       const zw_api_controller_id_rsp_t *in)
{
    if (out == NULL || in == NULL || cap < 4u) {
        return 0u;
    }
    for (size_t i = 0; i < 4u; i++) {
        out[i] = in->home_id[i];
    }
    return zw_api_node_id_build(&out[4], cap - 4u, w, in->own_node_id) == 0u ? 0u : 4u + (size_t)w;
}

bool zw_api_controller_id_rsp_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                     zw_api_controller_id_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 4u + (size_t)w) {
        return false;
    }
    for (size_t i = 0; i < 4u; i++) {
        out->home_id[i] = p[i];
    }
    return zw_api_node_id_parse(&p[4], (size_t)w, w, &out->own_node_id);
}

/* ---- `56` GetSUCNodeId, `54` SetSUCNodeId (§6.9) ----------------------------------------------------- */

size_t zw_api_get_suc_node_id_rsp_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                         const zw_api_get_suc_node_id_rsp_t *in)
{
    if (in == NULL) {
        return 0u;
    }
    return zw_api_node_id_build(out, cap, w, in->node_id);
}

bool zw_api_get_suc_node_id_rsp_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                       zw_api_get_suc_node_id_rsp_t *out)
{
    if (out == NULL) {
        return false;
    }
    return zw_api_node_id_parse(p, len, w, &out->node_id);
}

size_t zw_api_set_suc_node_id_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                         const zw_api_set_suc_node_id_req_t *in)
{
    if (out == NULL || in == NULL || cap < (size_t)w + 4u) {
        return 0u;
    }
    size_t pos = zw_api_node_id_build(out, cap, w, in->node_id);
    if (pos == 0u) {
        return 0u;
    }
    out[pos] = in->enable_suc ? 0x01u : 0x00u;
    pos += 1u;
    out[pos] = in->transmit_options;
    pos += 1u;
    out[pos] = in->is_sis ? 0x01u : 0x00u;
    pos += 1u;
    out[pos] = in->callback_id;
    pos += 1u;
    return pos;
}

bool zw_api_set_suc_node_id_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                       zw_api_set_suc_node_id_req_t *out)
{
    if (p == NULL || out == NULL || len != (size_t)w + 4u) {
        return false;
    }
    uint16_t nid;
    if (!zw_api_node_id_parse(p, (size_t)w, w, &nid)) {
        return false;
    }
    size_t off = (size_t)w;
    out->node_id = nid;
    out->enable_suc = (p[off] & 0x01u) != 0u;
    off += 1u;
    out->transmit_options = p[off];
    off += 1u;
    out->is_sis = (p[off] & 0x01u) != 0u;
    off += 1u;
    out->callback_id = p[off];
    return true;
}

size_t zw_api_set_suc_node_id_rsp_build(uint8_t *out, size_t cap, const zw_api_set_suc_node_id_rsp_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = in->started ? 0x01u : 0x00u;
    return 1u;
}

bool zw_api_set_suc_node_id_rsp_parse(const uint8_t *p, size_t len, zw_api_set_suc_node_id_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->started = (p[0] != 0x00u); /* 00 = no, anything else = yes */
    return true;
}

size_t zw_api_set_suc_node_id_cb_build(uint8_t *out, size_t cap, const zw_api_set_suc_node_id_cb_t *in)
{
    if (out == NULL || in == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = in->callback_id;
    out[1] = in->status;
    return 2u;
}

bool zw_api_set_suc_node_id_cb_parse(const uint8_t *p, size_t len, zw_api_set_suc_node_id_cb_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    out->callback_id = p[0];
    out->status = p[1];
    return true;
}

/* ---- `51` AssignSUCReturnRoute (§6.15) ------------------------------------------------------------- */

size_t zw_api_assign_suc_return_route_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                                 const zw_api_assign_suc_return_route_req_t *in)
{
    if (out == NULL || in == NULL || cap < (size_t)w + 1u) {
        return 0u;
    }
    size_t pos = zw_api_node_id_build(out, cap, w, in->node_id);
    if (pos == 0u) {
        return 0u;
    }
    out[pos] = in->callback_id;
    return pos + 1u;
}

bool zw_api_assign_suc_return_route_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                               zw_api_assign_suc_return_route_req_t *out)
{
    if (p == NULL || out == NULL || len != (size_t)w + 1u) {
        return false;
    }
    uint16_t nid;
    if (!zw_api_node_id_parse(p, (size_t)w, w, &nid)) {
        return false;
    }
    out->node_id = nid;
    out->callback_id = p[(size_t)w];
    return true;
}

size_t zw_api_assign_suc_return_route_rsp_build(uint8_t *out, size_t cap,
                                                 const zw_api_assign_suc_return_route_rsp_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = in->started ? 0x01u : 0x00u;
    return 1u;
}

bool zw_api_assign_suc_return_route_rsp_parse(const uint8_t *p, size_t len,
                                               zw_api_assign_suc_return_route_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->started = (p[0] != 0x00u); /* 00 = no, anything else = yes */
    return true;
}

size_t zw_api_assign_suc_return_route_cb_build(uint8_t *out, size_t cap,
                                                const zw_api_assign_suc_return_route_cb_t *in)
{
    if (out == NULL || in == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = in->callback_id;
    out[1] = in->transmit_status;
    return 2u;
}

bool zw_api_assign_suc_return_route_cb_parse(const uint8_t *p, size_t len,
                                              zw_api_assign_suc_return_route_cb_t *out)
{
    if (p == NULL || out == NULL || len < 2u) {
        return false;
    }
    out->callback_id = p[0];
    out->transmit_status = p[1];
    return true;
}

/* ---- `41` GetNodeProtocolInfo (§6.10) ------------------------------------------------------------- */

size_t zw_api_node_protocol_info_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                            const zw_api_node_protocol_info_req_t *in)
{
    if (in == NULL) {
        return 0u;
    }
    return zw_api_node_id_build(out, cap, w, in->node_id);
}

bool zw_api_node_protocol_info_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                          zw_api_node_protocol_info_req_t *out)
{
    /* a `_req`: the whole payload is the node ID, so this checks the exact length itself rather
     * than relying on zw_api_node_id_parse()'s own minimum-only contract. */
    if (out == NULL || len != (size_t)w) {
        return false;
    }
    return zw_api_node_id_parse(p, len, w, &out->node_id);
}

size_t zw_api_node_protocol_info_rsp_build(uint8_t *out, size_t cap,
                                            const zw_api_node_protocol_info_rsp_t *in)
{
    if (out == NULL || in == NULL) {
        return 0u;
    }
    const size_t total = in->has_specific_device_class ? 6u : 5u;
    if (cap < total) {
        return 0u;
    }
    uint8_t b0 = (uint8_t)(in->protocol_version & 0x07u);
    if (in->always_listening) {
        b0 = (uint8_t)((unsigned)b0 | 0x80u);
    }
    if (in->routing) {
        b0 |= 0x40u;
    }
    if (in->speed_40k) {
        b0 |= 0x10u;
    }
    if (in->speed_9_6k) {
        b0 |= 0x08u;
    }
    out[0] = b0;
    uint8_t b1 = (uint8_t)((unsigned)(in->flirs & 0x03u) << 5);
    if (in->optional_functionality) {
        b1 = (uint8_t)((unsigned)b1 | 0x80u);
    }
    if (in->supports_beaming) {
        b1 |= 0x10u;
    }
    if (in->is_controller_type) {
        b1 |= 0x02u;
    }
    if (in->has_specific_device_class) {
        b1 |= 0x04u;
    }
    if (in->supports_s0) {
        b1 |= 0x01u;
    }
    out[1] = b1;
    uint8_t b2 = 0u;
    if (in->speed_100k_classic) {
        b2 |= 0x01u;
    }
    if (in->speed_100k_long_range) {
        b2 |= 0x02u;
    }
    out[2] = b2;
    out[3] = in->basic_device_class;
    out[4] = in->generic_device_class;
    if (total == 6u) {
        out[5] = in->specific_device_class;
    }
    return total;
}

bool zw_api_node_protocol_info_rsp_parse(const uint8_t *p, size_t len,
                                          zw_api_node_protocol_info_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 5u) {
        return false;
    }
    const uint8_t b0 = p[0];
    const uint8_t b1 = p[1];
    const uint8_t b2 = p[2];
    /* byte 1's flag, not the length, decides whether byte 5 is read; trailing bytes past what is
     * needed (5 or 6) are accepted and ignored either way. */
    const bool has_specific = (b1 & 0x04u) != 0u;
    if (has_specific && len < 6u) {
        return false;
    }
    out->always_listening = ((unsigned)b0 & 0x80u) != 0u;
    out->routing = (b0 & 0x40u) != 0u;
    out->speed_40k = (b0 & 0x10u) != 0u;
    out->speed_9_6k = (b0 & 0x08u) != 0u;
    out->protocol_version = (uint8_t)(b0 & 0x07u);
    out->optional_functionality = ((unsigned)b1 & 0x80u) != 0u;
    out->flirs = (uint8_t)((unsigned)(b1 >> 5) & 0x03u);
    out->supports_beaming = (b1 & 0x10u) != 0u;
    out->is_controller_type = (b1 & 0x02u) != 0u;
    out->has_specific_device_class = has_specific;
    out->supports_s0 = (b1 & 0x01u) != 0u;
    out->speed_100k_classic = (b2 & 0x01u) != 0u;
    out->speed_100k_long_range = (b2 & 0x02u) != 0u;
    out->basic_device_class = p[3];
    out->generic_device_class = p[4];
    out->specific_device_class = has_specific ? (unsigned)p[5] : 0u;
    return true;
}

/* ---- `42` HardReset (§6.14) --------------------------------------------------------------------- */

size_t zw_api_hard_reset_req_build(uint8_t *out, size_t cap, const zw_api_hard_reset_req_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = in->callback_id;
    return 1u;
}

/* A bare callback ID, from a minimum of 1 byte; a `_req` wrapper adds its own exact-length check. */
static bool hard_reset_parse(const uint8_t *p, size_t len, zw_api_hard_reset_req_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->callback_id = p[0];
    return true;
}

bool zw_api_hard_reset_req_parse(const uint8_t *p, size_t len, zw_api_hard_reset_req_t *out)
{
    return len == 1u && hard_reset_parse(p, len, out);
}

size_t zw_api_hard_reset_cb_build(uint8_t *out, size_t cap, const zw_api_hard_reset_cb_t *in)
{
    return zw_api_hard_reset_req_build(out, cap, in);
}

bool zw_api_hard_reset_cb_parse(const uint8_t *p, size_t len, zw_api_hard_reset_cb_t *out)
{
    return hard_reset_parse(p, len, out); /* the module's callback: trailing bytes are ignored */
}

/* ---- `60` RequestNodeInfo (§6.13) --------------------------------------------------------------- */

size_t zw_api_request_node_info_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                           const zw_api_request_node_info_req_t *in)
{
    if (in == NULL) {
        return 0u;
    }
    return zw_api_node_id_build(out, cap, w, in->node_id);
}

bool zw_api_request_node_info_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                         zw_api_request_node_info_req_t *out)
{
    /* a `_req`: the whole payload is the node ID, so this checks the exact length itself. */
    if (out == NULL || len != (size_t)w) {
        return false;
    }
    return zw_api_node_id_parse(p, len, w, &out->node_id);
}

size_t zw_api_request_node_info_rsp_build(uint8_t *out, size_t cap,
                                           const zw_api_request_node_info_rsp_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = in->sent ? 0x01u : 0x00u;
    return 1u;
}

bool zw_api_request_node_info_rsp_parse(const uint8_t *p, size_t len,
                                         zw_api_request_node_info_rsp_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->sent = (p[0] != 0x00u); /* 00 = no, anything else = yes */
    return true;
}

/* ---- `DA` GetLongRangeNodes (§6.21) --------------------------------------------------------------- */

size_t zw_api_lr_nodes_req_build(uint8_t *out, size_t cap, const zw_api_lr_nodes_req_t *in)
{
    if (out == NULL || in == NULL || cap < 1u) {
        return 0u;
    }
    out[0] = in->segment;
    return 1u;
}

bool zw_api_lr_nodes_req_parse(const uint8_t *p, size_t len, zw_api_lr_nodes_req_t *out)
{
    if (p == NULL || out == NULL || len != 1u) {
        return false;
    }
    out->segment = p[0];
    return true;
}

size_t zw_api_lr_nodes_rsp_build(uint8_t *out, size_t cap, const zw_api_lr_nodes_rsp_t *in)
{
    if (out == NULL || in == NULL || in->node_list == NULL) {
        return 0u;
    }
    const size_t total = 3u + ZW_LR_SEGMENT_BYTES;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->more_segments ? 0x01u : 0x00u;
    out[1] = in->segment;
    out[2] = (uint8_t)ZW_LR_SEGMENT_BYTES;
    for (size_t i = 0; i < ZW_LR_SEGMENT_BYTES; i++) {
        out[3u + i] = in->node_list[i];
    }
    return total;
}

bool zw_api_lr_nodes_rsp_parse(const uint8_t *p, size_t len, zw_api_lr_nodes_rsp_t *out)
{
    const size_t total = 3u + ZW_LR_SEGMENT_BYTES;
    if (p == NULL || out == NULL || len < total || p[2] != (uint8_t)ZW_LR_SEGMENT_BYTES) {
        return false;
    }
    out->more_segments = (p[0] != 0x00u); /* 00 = no, anything else = yes */
    out->segment = p[1];
    out->node_list = &p[3];
    return true;
}
