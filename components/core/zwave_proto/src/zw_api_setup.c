/*
 * zw_api_setup.c - `0B` SerialAPISetup's envelope (serial-api.md §6.7): sub-command byte plus a
 * raw view of whatever data belongs to it. Each sub-command's own shape (§6.7's table) is for the
 * caller to interpret; only the envelope common to all of them is decoded here.
 */
#include "zwave_proto/zw_api.h"

size_t zw_api_api_setup_req_build(uint8_t *out, size_t cap, const zw_api_api_setup_req_t *in)
{
    if (out == NULL || in == NULL || (in->data == NULL && in->data_len > 0u)) {
        return 0u;
    }
    const size_t total = 1u + in->data_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = in->sub;
    for (size_t i = 0; i < in->data_len; i++) {
        out[1u + i] = in->data[i];
    }
    return total;
}

bool zw_api_api_setup_req_parse(const uint8_t *p, size_t len, zw_api_api_setup_req_t *out)
{
    if (p == NULL || out == NULL || len < 1u) {
        return false;
    }
    out->sub = p[0];
    out->data_len = len - 1u;
    out->data = out->data_len > 0u ? &p[1] : NULL;
    return true;
}

size_t zw_api_api_setup_rsp_build(uint8_t *out, size_t cap, const zw_api_api_setup_rsp_t *in)
{
    return zw_api_api_setup_req_build(out, cap, in); /* identical envelope */
}

bool zw_api_api_setup_rsp_parse(const uint8_t *p, size_t len, zw_api_api_setup_rsp_t *out)
{
    return zw_api_api_setup_req_parse(p, len, out); /* identical envelope */
}

/* ---- typed helpers on top of the envelope -------------------------------------------------------
 * The module's own answers: every length check here is a minimum, so trailing bytes a newer
 * firmware appends are accepted and ignored. Each helper checks 'rsp->sub' itself.
 * ------------------------------------------------------------------------------------------------- */

bool zw_api_setup_unsupported(const zw_api_api_setup_rsp_t *rsp, uint8_t *refused_sub)
{
    if (rsp == NULL || refused_sub == NULL || rsp->sub != ZW_SETUP_UNSUPPORTED || rsp->data_len < 1u) {
        return false;
    }
    *refused_sub = rsp->data[0];
    return true;
}

bool zw_api_setup_supported_cmds_parse(const zw_api_api_setup_rsp_t *rsp,
                                        zw_api_setup_supported_cmds_t *out)
{
    if (rsp == NULL || out == NULL || rsp->sub != ZW_SETUP_GET_SUPPORTED_COMMANDS || rsp->data_len < 1u) {
        return false;
    }
    out->legacy_mask = rsp->data[0];
    out->extended_mask = rsp->data_len > 1u ? &rsp->data[1] : NULL;
    out->extended_mask_len = rsp->data_len > 1u ? rsp->data_len - 1u : 0u;
    return true;
}

bool zw_api_setup_supports(const zw_api_setup_supported_cmds_t *cmds, uint8_t sub)
{
    if (cmds == NULL || sub == 0u) {
        return false;
    }
    /* the eight legacy sub-commands (01, 02, 04, 08, 10, 20, 40, 80) are exactly the single-bit
     * byte values, and the legacy mask's matching bit says whether each is supported (§6.7). */
    if (((unsigned)sub & ((unsigned)sub - 1u)) == 0u && (cmds->legacy_mask & sub) != 0u) {
        return true;
    }
    if (cmds->extended_mask == NULL) {
        return false;
    }
    const unsigned local = (unsigned)sub - 1u;
    const unsigned b = local / 8u;
    const unsigned n = local % 8u;
    if (b >= cmds->extended_mask_len) {
        return false;
    }
    return (((unsigned)cmds->extended_mask[b] >> n) & 1u) != 0u;
}

bool zw_api_setup_regions_parse(const zw_api_api_setup_rsp_t *rsp, zw_api_setup_regions_t *out)
{
    if (rsp == NULL || out == NULL || rsp->sub != ZW_SETUP_GET_SUPPORTED_REGIONS || rsp->data_len < 1u) {
        return false;
    }
    const uint8_t count = rsp->data[0];
    if (rsp->data_len < 1u + (size_t)count) {
        return false; /* the count must fit inside what was given */
    }
    out->count = count;
    out->regions = count > 0u ? &rsp->data[1] : NULL;
    return true;
}

bool zw_api_setup_region_info_parse(const zw_api_api_setup_rsp_t *rsp, zw_api_setup_region_info_t *out)
{
    if (rsp == NULL || out == NULL || rsp->sub != ZW_SETUP_GET_REGION_INFO || rsp->data_len < 2u) {
        return false;
    }
    out->region = rsp->data[0];
    const uint8_t flags = rsp->data[1];
    out->supports_classic = (flags & 0x01u) != 0u;
    out->supports_long_range = (flags & 0x02u) != 0u;
    out->has_classic_superset = rsp->data_len > 2u;
    out->classic_superset_region = out->has_classic_superset ? (unsigned)rsp->data[2] : 0u;
    return true;
}

bool zw_api_setup_rsp_u8(const zw_api_api_setup_rsp_t *rsp, uint8_t expected_sub, uint8_t *value)
{
    if (rsp == NULL || value == NULL || rsp->sub != expected_sub || rsp->data_len < 1u) {
        return false;
    }
    *value = rsp->data[0];
    return true;
}

/* ---- builders for the requests that carry data --------------------------------------------------- */

size_t zw_api_setup_set_tx_status_report_req_build(uint8_t *out, size_t cap, bool on)
{
    const uint8_t data = on ? 0xFFu : 0x00u;
    const zw_api_api_setup_req_t req = {.sub = ZW_SETUP_SET_TX_STATUS_REPORT, .data = &data, .data_len = 1u};
    return zw_api_api_setup_req_build(out, cap, &req);
}

size_t zw_api_setup_get_region_info_req_build(uint8_t *out, size_t cap, uint8_t region)
{
    const zw_api_api_setup_req_t req = {.sub = ZW_SETUP_GET_REGION_INFO, .data = &region, .data_len = 1u};
    return zw_api_api_setup_req_build(out, cap, &req);
}

size_t zw_api_setup_set_rf_region_req_build(uint8_t *out, size_t cap, uint8_t region)
{
    const zw_api_api_setup_req_t req = {.sub = ZW_SETUP_SET_RF_REGION, .data = &region, .data_len = 1u};
    return zw_api_api_setup_req_build(out, cap, &req);
}

size_t zw_api_setup_set_node_id_type_req_build(uint8_t *out, size_t cap, zw_nid_width_t w)
{
    const uint8_t v = w == ZW_NID_8 ? 0x01u : 0x02u;
    const zw_api_api_setup_req_t req = {.sub = ZW_SETUP_SET_NODE_ID_TYPE, .data = &v, .data_len = 1u};
    return zw_api_api_setup_req_build(out, cap, &req);
}

/* ---- power (phase 3) ------------------------------------------------------------------------------ */

static int16_t s16_be(const uint8_t *p)
{
    return (int16_t)(uint16_t)((unsigned)p[0] << 8 | (unsigned)p[1]);
}

static void put_s16_be(uint8_t *p, int16_t v)
{
    const uint16_t u = (uint16_t)v;
    p[0] = (uint8_t)(u >> 8);
    p[1] = (uint8_t)(u & 0xFFu);
}

bool zw_api_setup_power_parse(const zw_api_api_setup_rsp_t *rsp, zw_api_setup_power_t *out)
{
    if (rsp == NULL || out == NULL || rsp->data == NULL) {
        return false;
    }
    if (rsp->sub == ZW_SETUP_GET_POWERLEVEL && rsp->data_len >= 2u) {
        out->dbm10 = (int16_t)(int8_t)rsp->data[0];
        out->cal_dbm10 = (int16_t)(int8_t)rsp->data[1];
        return true;
    }
    if (rsp->sub == ZW_SETUP_GET_POWERLEVEL_16BIT && rsp->data_len >= 4u) {
        out->dbm10 = s16_be(&rsp->data[0]);
        out->cal_dbm10 = s16_be(&rsp->data[2]);
        return true;
    }
    return false;
}

bool zw_api_setup_lr_power_parse(const zw_api_api_setup_rsp_t *rsp, int16_t *dbm10)
{
    if (rsp == NULL || dbm10 == NULL || rsp->data == NULL ||
        rsp->sub != ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER || rsp->data_len < 2u) {
        return false;
    }
    *dbm10 = s16_be(rsp->data);
    return true;
}

size_t zw_api_setup_set_power_req_build(uint8_t *out, size_t cap, bool sixteen,
                                        const zw_api_setup_power_t *p)
{
    uint8_t d[4];
    zw_api_api_setup_req_t req = {.data = d};
    if (p == NULL) {
        return 0u;
    }
    if (sixteen) {
        put_s16_be(&d[0], p->dbm10);
        put_s16_be(&d[2], p->cal_dbm10);
        req.sub = ZW_SETUP_SET_POWERLEVEL_16BIT;
        req.data_len = 4u;
    } else {
        if (p->dbm10 < -128 || p->dbm10 > 127 || p->cal_dbm10 < -128 || p->cal_dbm10 > 127) {
            return 0u;
        }
        d[0] = (uint8_t)(int8_t)p->dbm10;
        d[1] = (uint8_t)(int8_t)p->cal_dbm10;
        req.sub = ZW_SETUP_SET_POWERLEVEL;
        req.data_len = 2u;
    }
    return zw_api_api_setup_req_build(out, cap, &req);
}

size_t zw_api_setup_set_lr_power_req_build(uint8_t *out, size_t cap, int16_t dbm10)
{
    uint8_t d[2];
    put_s16_be(d, dbm10);
    const zw_api_api_setup_req_t req = {.sub = ZW_SETUP_SET_LONG_RANGE_MAXIMUM_TX_POWER, .data = d,
                                        .data_len = 2u};
    return zw_api_api_setup_req_build(out, cap, &req);
}
