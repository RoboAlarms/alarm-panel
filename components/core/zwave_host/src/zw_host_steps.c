/*
 * zw_host_steps.c - the start-up sequence's commands, steps 2-18 (docs/features/36 serial-api.md
 * §5), and §4's checks on the way. Every payload is built and parsed with zwave_proto's codecs
 * (zw_api.h); the cursor is (h->step, h->sub), see zw_host_internal.h.
 */
#include "zw_host_internal.h"

#include <string.h>

/* §6.2: the panel's own node information - what it supports whatever the asker's security:
 * Z-Wave Plus Info, Security 2, Supervision, CRC-16 Encapsulation, Multi Command (Security 98
 * only with S0, phase 3). A generic controller, always listening. */
static const uint8_t PANEL_CCS[] = {0x5Eu, 0x9Fu, 0x6Cu, 0x56u, 0x8Fu};
#define ZW_PANEL_GENERIC  0x01u
#define ZW_PANEL_SPECIFIC 0x00u
/* §6.6: ACK 1600 ms, byte 150 ms, in tens of ms (A0 0F). */
#define ZW_PANEL_ACK_X10MS  0xA0u
#define ZW_PANEL_BYTE_X10MS 0x0Fu
/* §4: SDKs before 7.22.1 get the "update the radio's software" note. */
#define ZW_SDK_GOOD_MAJOR 7u
#define ZW_SDK_GOOD_MINOR 22u
#define ZW_SDK_GOOD_PATCH 1u
/* §4: the chip and the library types the panel accepts. */
#define ZW_CHIP_800_SERIES      0x08u
#define ZW_LIBRARY_STATIC       0x01u
#define ZW_LIBRARY_BRIDGE       0x07u
/* §6.9: SetSUCNodeId's transmit options, and its capabilities byte 01 = SIS. */
#define ZW_SUC_TX_OPTIONS ZW_TX_OPT_DEFAULT
/* Step 13 asks for Long Range segments while "more" follows, this many at most. */
#define ZW_LR_SEGMENTS_MAX 32u

/* ---- Helpers ------------------------------------------------------------------------------ */

static zw_nid_width_t nid_width(const zw_host_t *h)
{
    return h->facts.node_id_bits == 16u ? ZW_NID_16 : ZW_NID_8;
}

static bool setup_supported(const zw_host_t *h, uint8_t sub)
{
    const zw_api_setup_supported_cmds_t cmds = {
        .legacy_mask = h->facts.setup_legacy,
        .extended_mask = h->facts.setup_extended_len > 0u ? h->facts.setup_extended : NULL,
        .extended_mask_len = h->facts.setup_extended_len,
    };
    return zw_api_setup_supports(&cmds, sub);
}

static bool has_function(const zw_host_t *h, uint8_t func)
{
    return zw_func_mask_test(h->facts.function_mask, func);
}

static bool is_primary(const zw_host_t *h)
{
    return !h->facts.is_secondary && !h->facts.controller.is_secondary;
}

/* A request for `func` with `len` bytes of payload in buf: what it waits for from §6's table
 * (zw_api_func_info), where its callback ID sits (zw_api_callback_id_offset). */
static bool request(zw_request_t *req, uint8_t func, uint8_t *buf, size_t len)
{
    zw_api_func_info_t info;
    if (!zw_api_func_info(func, &info)) {
        return false;
    }
    req->func = func;
    req->payload = len > 0u ? buf : NULL;
    req->payload_len = len;
    req->expect = (uint8_t)((info.has_response ? ZW_EXPECT_RESPONSE : 0u) |
                            (info.callback == ZW_CB_ONE_FINAL ? ZW_EXPECT_CALLBACK : 0u));
    if (info.callback == ZW_CB_ONE_FINAL) {
        size_t at = 0u;
        if (!zw_api_callback_id_offset(func, buf, len, &at) || at > 0xFFu) {
            return false;
        }
        req->callback_id_at = (uint8_t)at;
    }
    return true;
}

/* A SerialAPISetup request: the sub-command, with one data byte when `with_data`. */
static bool setup_request(zw_request_t *req, uint8_t *buf, uint8_t sub, bool with_data,
                          uint8_t data)
{
    const zw_api_api_setup_req_t s = {.sub = sub, .data = with_data ? &data : NULL,
                                      .data_len = with_data ? 1u : 0u};
    const size_t n = zw_api_api_setup_req_build(buf, ZW_PAYLOAD_MAX, &s);
    return n > 0u && request(req, (uint8_t)ZW_FUNC_SERIAL_API_SETUP, buf, n);
}

/* ---- The commands ------------------------------------------------------------------------- */

/* How many positions (sub) step `step` has; each is a command or nothing. */
static uint8_t step_positions(const zw_host_t *h, uint8_t step)
{
    switch (step) {
    case 8:
        return 2u;  /* 0B 10, and 0B 11 for Long Range */
    case 9:
        return (uint8_t)((unsigned)h->facts.region_count + 2u); /* 0B 15, 0B 16 per region, 0B 20 */
    case 13:
        return ZW_LR_SEGMENTS_MAX; /* DA per segment while "more" */
    case 17:
        return 2u;  /* 56, then 54 when there is no SUC */
    default:
        return 1u;
    }
}

/* The command at (step, sub), when there is one. */
static bool step_command(zw_host_t *h, uint8_t step, uint8_t sub, zw_request_t *req,
                         uint8_t *buf)
{
    const zw_module_facts_t *f = &h->facts;
    switch (step) {
    case 2:
        return request(req, (uint8_t)ZW_FUNC_GET_SERIAL_API_CAPABILITIES, buf, 0u);
    case 3:
        return request(req, (uint8_t)ZW_FUNC_GET_SERIAL_API_INIT_DATA, buf, 0u);
    case 4:
        return request(req, (uint8_t)ZW_FUNC_GET_CONTROLLER_VERSION, buf, 0u);
    case 5:
        return request(req, (uint8_t)ZW_FUNC_GET_PROTOCOL_VERSION, buf, 0u);
    case 6:
        return request(req, (uint8_t)ZW_FUNC_GET_CONTROLLER_CAPABILITIES, buf, 0u);
    case 7:
        return setup_request(req, buf, ZW_SETUP_GET_SUPPORTED_COMMANDS, false, 0u);
    case 8:
        if (sub == 0u) {
            return setup_supported(h, ZW_SETUP_GET_MAXIMUM_PAYLOAD_SIZE) &&
                   setup_request(req, buf, ZW_SETUP_GET_MAXIMUM_PAYLOAD_SIZE, false, 0u);
        }
        return h->cfg.long_range &&
               setup_supported(h, ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE) &&
               setup_request(req, buf, ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE, false, 0u);
    case 9:
        if (sub == 0u) {
            return setup_supported(h, ZW_SETUP_GET_SUPPORTED_REGIONS) &&
                   setup_request(req, buf, ZW_SETUP_GET_SUPPORTED_REGIONS, false, 0u);
        }
        if (sub <= f->region_count) {
            return setup_supported(h, ZW_SETUP_GET_REGION_INFO) &&
                   setup_request(req, buf, ZW_SETUP_GET_REGION_INFO, true,
                                 f->regions[(unsigned)sub - 1u]);
        }
        return setup_supported(h, ZW_SETUP_GET_RF_REGION) &&
               setup_request(req, buf, ZW_SETUP_GET_RF_REGION, false, 0u);
    case 10: /* only when the installer's region differs from the module's */
        return h->cfg.set_region && !(f->region_known && f->region == h->cfg.region) &&
               setup_supported(h, ZW_SETUP_SET_RF_REGION) &&
               setup_request(req, buf, ZW_SETUP_SET_RF_REGION, true, h->cfg.region);
    case 11: {
        const zw_api_app_node_info_req_t nif = {
            .always_listening = true,
            .generic_class = ZW_PANEL_GENERIC,
            .specific_class = ZW_PANEL_SPECIFIC,
            .cc_list = PANEL_CCS,
            .cc_list_len = sizeof PANEL_CCS,
        };
        const size_t n = zw_api_app_node_info_req_build(buf, ZW_PAYLOAD_MAX, &nif);
        return n > 0u && request(req, (uint8_t)ZW_FUNC_SET_APPLICATION_NODE_INFORMATION, buf, n);
    }
    case 12:
        return request(req, (uint8_t)ZW_FUNC_SOFT_RESET, buf, 0u);
    case 13: { /* only with Long Range wanted and available */
        const zw_api_lr_nodes_req_t lr = {.segment = sub};
        if (!h->cfg.long_range || !f->long_range_available ||
            !has_function(h, (uint8_t)ZW_FUNC_GET_LONG_RANGE_NODES)) {
            return false;
        }
        const size_t n = zw_api_lr_nodes_req_build(buf, ZW_PAYLOAD_MAX, &lr);
        return n > 0u && request(req, (uint8_t)ZW_FUNC_GET_LONG_RANGE_NODES, buf, n);
    }
    case 14: { /* 16-bit node IDs with Long Range, else 8-bit */
        const zw_nid_width_t w =
            h->cfg.long_range && f->long_range_available ? ZW_NID_16 : ZW_NID_8;
        const size_t n = setup_supported(h, ZW_SETUP_SET_NODE_ID_TYPE)
                             ? zw_api_setup_set_node_id_type_req_build(buf, ZW_PAYLOAD_MAX, w)
                             : 0u;
        return n > 0u && request(req, (uint8_t)ZW_FUNC_SERIAL_API_SETUP, buf, n);
    }
    case 15:
        return request(req, (uint8_t)ZW_FUNC_GET_CONTROLLER_ID, buf, 0u);
    case 16: {
        const size_t n =
            setup_supported(h, ZW_SETUP_SET_TX_STATUS_REPORT)
                ? zw_api_setup_set_tx_status_report_req_build(buf, ZW_PAYLOAD_MAX, true)
                : 0u;
        return n > 0u && request(req, (uint8_t)ZW_FUNC_SERIAL_API_SETUP, buf, n);
    }
    case 17:
        if (sub == 0u) {
            return request(req, (uint8_t)ZW_FUNC_GET_SUC_NODE_ID, buf, 0u);
        }
        /* §5: no SUC, and the panel the primary with no SIS: name itself SUC and SIS. */
        if (f->suc_node_id == 0u && is_primary(h) && !f->controller.sis_present) {
            const zw_api_set_suc_node_id_req_t suc = {
                .node_id = f->node_id,
                .enable_suc = true,
                .transmit_options = ZW_SUC_TX_OPTIONS,
                .is_sis = true,
                .callback_id = 0u, /* own node: response only, no callback */
            };
            const size_t n = zw_api_set_suc_node_id_req_build(buf, ZW_PAYLOAD_MAX, nid_width(h),
                                                              &suc);
            if (n == 0u || !request(req, (uint8_t)ZW_FUNC_SET_SUC_NODE_ID, buf, n)) {
                return false;
            }
            /* Naming the controller itself is synchronous. Real ZAC93 firmware sends no
             * callback here; waiting for one caused retries and a needless recovery reset. */
            req->expect = ZW_EXPECT_RESPONSE;
            return true;
        }
        return false;
    case 18: {
        const zw_api_api_timeouts_req_t to = {.ack_timeout_x10ms = ZW_PANEL_ACK_X10MS,
                                              .byte_timeout_x10ms = ZW_PANEL_BYTE_X10MS};
        const size_t n = has_function(h, (uint8_t)ZW_FUNC_SET_SERIAL_API_TIMEOUTS)
                             ? zw_api_api_timeouts_req_build(buf, ZW_PAYLOAD_MAX, &to)
                             : 0u;
        return n > 0u && request(req, (uint8_t)ZW_FUNC_SET_SERIAL_API_TIMEOUTS, buf, n);
    }
    default:
        return false;
    }
}

bool zw_host_step_plan(zw_host_t *h, zw_request_t *req, uint8_t buf[ZW_PAYLOAD_MAX])
{
    while (h->step <= 18u) {
        while (h->sub < step_positions(h, h->step)) {
            if (step_command(h, h->step, h->sub, req, buf)) {
                return true;
            }
            h->sub++;
        }
        h->step++;
        h->sub = 0u;
    }
    return false;
}

/* ---- The answers -------------------------------------------------------------------------- */

static zw_step_verdict_t refuse(zw_link_down_t *refusal, zw_down_reason_t reason, uint8_t detail)
{
    refusal->reason = reason;
    refusal->detail = detail;
    return ZW_STEP_REFUSE;
}

/* §4: every function §6 marks required must be in the mask; the first missing one is named. */
static zw_step_verdict_t check_functions(const zw_host_t *h, zw_link_down_t *refusal)
{
    for (unsigned func = 1u; func <= 0xFFu; func++) {
        zw_api_func_info_t info;
        if (zw_api_func_info((uint8_t)func, &info) && info.required &&
            !has_function(h, (uint8_t)func)) {
            return refuse(refusal, ZW_DOWN_MISSING_FUNCTION, (uint8_t)func);
        }
    }
    return ZW_STEP_NEXT;
}

static bool before_7_22_1(const zw_module_facts_t *f)
{
    if (f->protocol_major != ZW_SDK_GOOD_MAJOR) {
        return f->protocol_major < ZW_SDK_GOOD_MAJOR;
    }
    if (f->protocol_minor != ZW_SDK_GOOD_MINOR) {
        return f->protocol_minor < ZW_SDK_GOOD_MINOR;
    }
    return f->protocol_patch < ZW_SDK_GOOD_PATCH;
}

/* A SerialAPISetup answer (step 7-10, 14, 16): the envelope, or false when unusable. Sets
 * *unsupported when the module refused the sub-command. */
static bool setup_answer(const uint8_t *p, size_t len, zw_api_api_setup_rsp_t *rsp,
                         bool *unsupported)
{
    uint8_t refused = 0u;
    if (!zw_api_api_setup_rsp_parse(p, len, rsp)) {
        return false;
    }
    *unsupported = zw_api_setup_unsupported(rsp, &refused);
    return true;
}

/* What the answer at (step, sub) says. False: it could not be used (bad answer). */
static bool step_answer(zw_host_t *h, const uint8_t *p, size_t len, zw_link_down_t *refusal,
                        zw_step_verdict_t *verdict)
{
    zw_module_facts_t *f = &h->facts;
    zw_api_api_setup_rsp_t rsp;
    bool unsupported = false;
    uint8_t v = 0u;
    *verdict = ZW_STEP_NEXT;
    switch (h->step) {
    case 2: {
        zw_api_api_caps_rsp_t caps;
        if (!zw_api_api_caps_rsp_parse(p, len, &caps)) {
            return false;
        }
        f->firmware_major = caps.app_version_major;
        f->firmware_minor = caps.app_version_minor;
        f->manufacturer = caps.manufacturer_id;
        f->product_type = caps.product_type;
        f->product_id = caps.product_id;
        memcpy(f->function_mask, caps.function_mask, sizeof f->function_mask);
        *verdict = check_functions(h, refusal);
        return true;
    }
    case 3: {
        zw_api_init_data_rsp_t init;
        if (!zw_api_init_data_rsp_parse(p, len, &init)) {
            return false;
        }
        f->api_version = init.serial_api_version;
        f->is_secondary = init.is_secondary;
        f->is_sis = init.is_sis;
        memcpy(f->node_list, init.node_list, sizeof f->node_list);
        f->chip_type = init.chip_type;
        f->chip_version = init.chip_version;
        if (!init.is_controller) {
            *verdict = refuse(refusal, ZW_DOWN_NOT_CONTROLLER, 0u);
        } else if (init.chip_type != ZW_CHIP_800_SERIES) {
            *verdict = refuse(refusal, ZW_DOWN_WRONG_CHIP, init.chip_type);
        }
        return true;
    }
    case 4: {
        zw_api_controller_version_rsp_t ver;
        if (!zw_api_controller_version_rsp_parse(p, len, &ver)) {
            return false;
        }
        const size_t n = ver.library_len < sizeof f->library - 1u ? ver.library_len
                                                                   : sizeof f->library - 1u;
        memcpy(f->library, ver.library, n);
        f->library[n] = '\0';
        f->library_type = ver.library_type;
        if (ver.library_type != ZW_LIBRARY_STATIC && ver.library_type != ZW_LIBRARY_BRIDGE) {
            *verdict = refuse(refusal, ZW_DOWN_WRONG_LIBRARY, ver.library_type);
        }
        return true;
    }
    case 5: {
        zw_api_protocol_version_rsp_t pv;
        if (!zw_api_protocol_version_rsp_parse(p, len, &pv)) {
            return false;
        }
        f->protocol_major = pv.major;
        f->protocol_minor = pv.minor;
        f->protocol_patch = pv.patch;
        f->update_note = before_7_22_1(f);
        return true;
    }
    case 6:
        return zw_api_controller_caps_rsp_parse(p, len, &f->controller);
    case 7: {
        zw_api_setup_supported_cmds_t cmds;
        if (!setup_answer(p, len, &rsp, &unsupported)) {
            return false;
        }
        if (unsupported) {
            return true; /* no sub-command known: the steps that need one are skipped */
        }
        if (!zw_api_setup_supported_cmds_parse(&rsp, &cmds)) {
            return false;
        }
        const size_t n = cmds.extended_mask_len < sizeof f->setup_extended
                             ? cmds.extended_mask_len
                             : sizeof f->setup_extended;
        f->setup_legacy = cmds.legacy_mask;
        f->setup_extended_len = (uint8_t)n;
        if (n > 0u) {
            memcpy(f->setup_extended, cmds.extended_mask, n);
        }
        return true;
    }
    case 8:
        if (!setup_answer(p, len, &rsp, &unsupported)) {
            return false;
        }
        if (unsupported) {
            return true;
        }
        if (h->sub == 0u) {
            return zw_api_setup_rsp_u8(&rsp, ZW_SETUP_GET_MAXIMUM_PAYLOAD_SIZE, &f->max_payload);
        }
        return zw_api_setup_rsp_u8(&rsp, ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE,
                                   &f->lr_max_payload);
    case 9:
        if (!setup_answer(p, len, &rsp, &unsupported)) {
            return false;
        }
        if (unsupported) {
            return true;
        }
        if (h->sub == 0u) {
            zw_api_setup_regions_t regions;
            if (!zw_api_setup_regions_parse(&rsp, &regions)) {
                return false;
            }
            const size_t n = regions.count < ZW_HOST_REGIONS_MAX ? regions.count
                                                                 : ZW_HOST_REGIONS_MAX;
            f->region_count = (uint8_t)n;
            if (n > 0u) {
                memcpy(f->regions, regions.regions, n);
            }
            return true;
        }
        if (h->sub <= f->region_count) {
            zw_api_setup_region_info_t info;
            if (!zw_api_setup_region_info_parse(&rsp, &info)) {
                return false;
            }
            f->region_flags[(unsigned)h->sub - 1u] = (uint8_t)((info.supports_classic ? 0x01u : 0u) |
                                                     (info.supports_long_range ? 0x02u : 0u));
            return true;
        }
        if (!zw_api_setup_rsp_u8(&rsp, ZW_SETUP_GET_RF_REGION, &f->region)) {
            return false;
        }
        f->region_known = true;
        return true;
    case 10: /* refused (not a region it has): it keeps its own, the facts say so */
        if (!setup_answer(p, len, &rsp, &unsupported)) {
            return false;
        }
        if (!unsupported && zw_api_setup_rsp_u8(&rsp, ZW_SETUP_SET_RF_REGION, &v) && v != 0u) {
            f->region = h->cfg.region; /* in effect after the soft reset of step 12 */
            f->region_known = true;
        }
        return true;
    case 11:
        return true; /* ACK only */
    case 12:
        zw_host_step_started(h, p, len);
        return true;
    case 13: {
        zw_api_lr_nodes_rsp_t lr;
        uint16_t ids[1];
        if (!zw_api_lr_nodes_rsp_parse(p, len, &lr)) {
            return false;
        }
        f->lr_node_count = (uint16_t)((unsigned)f->lr_node_count +
                                      zw_lr_segment_iterate(lr.node_list, lr.segment, ids, 0u));
        if (!lr.more_segments) {
            h->sub = (uint8_t)(ZW_LR_SEGMENTS_MAX - 1u); /* the last position: the step ends */
        }
        return true;
    }
    case 14: /* refused: 8-bit node IDs, as after every soft reset; no Long Range then */
        if (!setup_answer(p, len, &rsp, &unsupported)) {
            return false;
        }
        f->node_id_bits = 8u;
        if (!unsupported && zw_api_setup_rsp_u8(&rsp, ZW_SETUP_SET_NODE_ID_TYPE, &v) && v != 0u &&
            h->cfg.long_range && f->long_range_available) {
            f->node_id_bits = 16u;
        }
        return true;
    case 15: {
        zw_api_controller_id_rsp_t id;
        if (!zw_api_controller_id_rsp_parse(p, len, nid_width(h), &id)) {
            return false;
        }
        f->home_id = (uint32_t)id.home_id[0] << 24 | (uint32_t)id.home_id[1] << 16 |
                     (uint32_t)id.home_id[2] << 8 | (uint32_t)id.home_id[3];
        f->node_id = id.own_node_id;
        return true;
    }
    case 16: /* refused: no TX reports (phase 2's jam detection reads them) */
        if (!setup_answer(p, len, &rsp, &unsupported)) {
            return false;
        }
        f->tx_status_report = !unsupported &&
                              zw_api_setup_rsp_u8(&rsp, ZW_SETUP_SET_TX_STATUS_REPORT, &v) &&
                              v != 0u;
        return true;
    case 17:
        if (h->sub == 0u) {
            zw_api_get_suc_node_id_rsp_t suc;
            if (!zw_api_get_suc_node_id_rsp_parse(p, len, nid_width(h), &suc)) {
                return false;
            }
            f->suc_node_id = suc.node_id;
            return true;
        } else {
            zw_api_set_suc_node_id_rsp_t suc;
            if (!zw_api_set_suc_node_id_rsp_parse(p, len, &suc) || !suc.started) {
                return false; /* the module could not: tried again, as any failing step */
            }
            f->suc_node_id = f->node_id;
            return true;
        }
    case 18: {
        zw_api_api_timeouts_rsp_t old;
        if (!zw_api_api_timeouts_rsp_parse(p, len, &old)) {
            return false;
        }
        f->old_ack_timeout = old.ack_timeout_x10ms;
        f->old_byte_timeout = old.byte_timeout_x10ms;
        return true;
    }
    default:
        return false;
    }
}

zw_step_verdict_t zw_host_step_result(zw_host_t *h, zw_tx_outcome_t status, const uint8_t *payload,
                                      size_t len, zw_link_down_t *refusal, bool *bad_answer)
{
    zw_step_verdict_t verdict = ZW_STEP_NEXT;
    *bad_answer = false;
    if (status != ZW_TX_OK) {
        return ZW_STEP_RETRY; /* no answer: §5's tries, then §7 */
    }
    if (!step_answer(h, payload, len, refusal, &verdict)) {
        *bad_answer = true;
        return ZW_STEP_RETRY;
    }
    if (verdict == ZW_STEP_NEXT) {
        h->sub++;
    }
    return verdict;
}

void zw_host_step_started(zw_host_t *h, const uint8_t *payload, size_t len)
{
    zw_api_started_t started;
    h->facts.node_id_bits = 8u; /* a restart always returns to 8-bit node IDs (§5 step 14) */
    h->facts.long_range_available = zw_api_started_parse(payload, len, &started) &&
                                    started.has_supported_protocols &&
                                    (started.supported_protocols & 0x01u) != 0u;
}
