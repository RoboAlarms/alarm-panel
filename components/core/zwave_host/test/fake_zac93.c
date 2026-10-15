/*
 * fake_zac93.c - a pretend ZAC93 and the wire it hangs on (see fake_zac93.h; serial-api.md §1,
 * §5, §6, §8). Its answers to the start-up commands are built with zwave_proto's codecs.
 */
#include "fake_zac93.h"

#include <stdio.h>
#include <string.h>

#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_proto/zw_gbl.h"
#include "zwave_proto/zw_dsk.h"

#define FAKE_ACK_TIMEOUT_MS 1600u /* the module waits as long for the host's ACK */
#define FAKE_TRANSMISSIONS     3u

enum { FAKE_T_FRAME = 1, FAKE_T_RAW, FAKE_T_RESTART, FAKE_T_DELIVER };

/* AddNode on the module's side. */
enum { ADD_IDLE = 0, ADD_LISTENING, ADD_JOINING };

/* What the Gecko bootloader prints (serial-api.md §8 step 1). */
static const char FAKE_BOOT_MENU[] =
    "\r\nGecko Bootloader v2.4.2\r\n1. upload gbl\r\n2. run\r\n3. ebl info\r\nBL > ";
static const char FAKE_BOOT_MENU_NO_UPLOAD[] =
    "\r\nGecko Bootloader v2.4.2\r\n2. run\r\n3. ebl info\r\nBL > ";
#define FAKE_BOOT_RUN_KEY 0x32u    /* '2' */
#define FAKE_BOOT_UPLOAD_KEY 0x31u /* '1' */

/* SerialAPIStarted's wake-up reasons (§6.8). */
#define FAKE_REASON_RESET    0x00u
#define FAKE_REASON_SOFT_RESET 0x07u

/* ---- The module's output ------------------------------------------------------------------ */

/* Hung, dead or held in reset: it hears nothing and sends nothing. */
static bool out_of_it(const fake_zac93_t *f)
{
    return f->fault.silent || f->fault.dead || f->reset_held;
}

/* One byte onto the wire, corrupted when a fault says so. */
static void emit(fake_zac93_t *f, uint8_t b)
{
    if (out_of_it(f)) {
        return;
    }
    if (f->fault.corrupt_in == 0) {
        b = (uint8_t)(b ^ (f->fault.corrupt_xor != 0u ? f->fault.corrupt_xor : 0x01u));
        f->fault.corrupt_in = -1;
    } else if (f->fault.corrupt_in > 0) {
        f->fault.corrupt_in--;
    }
    if (f->out_len < sizeof f->out) {
        f->out[f->out_len++] = b;
    } else {
        f->overflow++;
    }
}

static void emit_all(fake_zac93_t *f, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        emit(f, b[i]);
    }
}

static bool plan(fake_zac93_t *f, uint64_t at, uint8_t kind, uint8_t reason, const uint8_t *bytes,
                 size_t len);

/* The bootloader's menu: in one read, or - fault.menu_split - in two, menu_gap_ms apart. */
static void emit_menu(fake_zac93_t *f, uint64_t now)
{
    const uint8_t *menu = (const uint8_t *)(f->fault.otw_no_upload ? FAKE_BOOT_MENU_NO_UPLOAD
                                                                    : FAKE_BOOT_MENU);
    const size_t len = (f->fault.otw_no_upload ? sizeof FAKE_BOOT_MENU_NO_UPLOAD
                                               : sizeof FAKE_BOOT_MENU) - 1u;
    const size_t first = f->fault.menu_split > 0u && f->fault.menu_split < len
                             ? (size_t)f->fault.menu_split
                             : len;
    emit_all(f, menu, first);
    if (first < len) {
        (void)plan(f, now + f->fault.menu_gap_ms, FAKE_T_RAW, 0u, &menu[first], len - first);
    }
}

/* The outbox's head goes out, when nothing else waits for its ACK. */
static void send_next(fake_zac93_t *f, uint64_t now)
{
    if (f->in_flight || f->outbox_count == 0u) {
        return;
    }
    const fake_frame_buf_t *fr = &f->outbox[f->outbox_head];
    emit_all(f, fr->bytes, fr->len);
    f->in_flight = true;
    f->in_flight_tx = 1u;
    f->in_flight_due = now + FAKE_ACK_TIMEOUT_MS;
}

static void pop_in_flight(fake_zac93_t *f, uint64_t now)
{
    f->in_flight = false;
    f->outbox_head = (f->outbox_head + 1u) % FAKE_ZAC93_OUTBOX;
    f->outbox_count--;
    send_next(f, now);
}

/* A NAK or no ACK for the frame in flight: again, or give up after the 3rd. */
static void resend_or_drop(fake_zac93_t *f, uint64_t now)
{
    if (!f->in_flight) {
        return;
    }
    if (f->in_flight_tx >= FAKE_TRANSMISSIONS) {
        f->dropped++;
        pop_in_flight(f, now);
        return;
    }
    const fake_frame_buf_t *fr = &f->outbox[f->outbox_head];
    emit_all(f, fr->bytes, fr->len);
    f->in_flight_tx++;
    f->in_flight_due = now + FAKE_ACK_TIMEOUT_MS;
    f->resent++;
}

static void queue_frame(fake_zac93_t *f, uint64_t now, const uint8_t *bytes, size_t len)
{
    if (f->outbox_count >= FAKE_ZAC93_OUTBOX || len > ZW_FRAME_MAX) {
        f->overflow++;
        return;
    }
    fake_frame_buf_t *fr = &f->outbox[(f->outbox_head + f->outbox_count) % FAKE_ZAC93_OUTBOX];
    memcpy(fr->bytes, bytes, len);
    fr->len = len;
    f->outbox_count++;
    send_next(f, now);
}

/* ---- Planned outputs ---------------------------------------------------------------------- */

/* The entry planned, or NULL when there was no room (counted as an overflow). */
static fake_timed_t *plan_entry(fake_zac93_t *f, uint64_t at, uint8_t kind, uint8_t reason,
                                const uint8_t *bytes, size_t len)
{
    if (len > ZW_FRAME_MAX) {
        f->overflow++;
        return NULL;
    }
    for (size_t i = 0; i < FAKE_ZAC93_TIMED; i++) {
        fake_timed_t *e = &f->timed[i];
        if (!e->used) {
            e->used = true;
            e->scripted = false;
            e->kind = kind;
            e->reason = reason;
            e->at = at;
            e->seq = f->timed_seq++;
            e->len = len;
            if (len > 0u) {
                memcpy(e->bytes, bytes, len);
            }
            return e;
        }
    }
    f->overflow++;
    return NULL;
}

static bool plan(fake_zac93_t *f, uint64_t at, uint8_t kind, uint8_t reason, const uint8_t *bytes,
                 size_t len)
{
    return plan_entry(f, at, kind, reason, bytes, len) != NULL;
}

static fake_timed_t *plan_frame_entry(fake_zac93_t *f, uint64_t at, uint8_t type, uint8_t func,
                                      const uint8_t *payload, size_t len)
{
    uint8_t frame[ZW_FRAME_MAX];
    const size_t n = zw_frame_build(frame, sizeof frame, type, func, payload, len);
    if (n == 0u) {
        f->overflow++;
        return NULL;
    }
    return plan_entry(f, at, FAKE_T_FRAME, 0u, frame, n);
}

static bool plan_frame(fake_zac93_t *f, uint64_t at, uint8_t type, uint8_t func,
                       const uint8_t *payload, size_t len)
{
    return plan_frame_entry(f, at, type, func, payload, len) != NULL;
}

/* A test's script survives restarts; the module's own pending answers do not. */
static bool scripted(fake_timed_t *e)
{
    if (e != NULL) {
        e->scripted = true;
    }
    return e != NULL;
}

static void forget_own_plans(fake_zac93_t *f)
{
    for (size_t i = 0; i < FAKE_ZAC93_TIMED; i++) {
        if (!f->timed[i].scripted) {
            f->timed[i].used = false;
        }
    }
}

/* A planned callback: the request's callback ID, then the scripted bytes. */
static void plan_callback(fake_zac93_t *f, uint64_t at, uint8_t func, uint8_t cb_id,
                          const fake_answer_t *a)
{
    uint8_t payload[1u + FAKE_ZAC93_ANSWER_MAX];
    const size_t n = a->len <= FAKE_ZAC93_ANSWER_MAX ? a->len : FAKE_ZAC93_ANSWER_MAX;
    payload[0] = cb_id;
    memcpy(&payload[1], a->bytes, n);
    (void)plan_frame(f, at, ZW_TYPE_REQUEST, func, payload, n + 1u);
}

static bool region_has_long_range(uint8_t region)
{
    return region == 0x09u || region == 0x0Bu; /* USA and Europe Long Range (§6.7) */
}

/* The module restarts: whatever it was doing is forgotten, a region set since takes effect, the
 * node-ID width is 8 bits again; SerialAPIStarted follows (§6.8), or the bootloader's menu. */
static void restart(fake_zac93_t *f, uint64_t now, uint8_t reason)
{
    zw_rx_init(&f->rx);
    forget_own_plans(f);
    f->outbox_head = 0u;
    f->outbox_count = 0u;
    f->in_flight = false;
    f->send_data_cb = 0u;
    f->restarts++;
    f->add_state = ADD_IDLE; /* no longer listening */
    f->smart_listen = false; /* nor for SmartStart: the host sets the mode again */
    f->joining = NULL;
    f->id.region = f->next_region;
    f->power_dbm10 = f->next_power_dbm10;
    f->lr_power_dbm10 = f->next_lr_power_dbm10;
    f->node_id_bits = 8u;
    f->tx_report = false;
    if (f->fault.bootloader) {
        emit_menu(f, now);
        return;
    }
    const zw_api_started_t started = {
        .wake_up_reason = reason,
        .watchdog_on = false,
        .always_listening = true,
        .generic_class = f->id.generic,
        .specific_class = f->id.specific,
        .cc_list = f->node_info_len > 0u ? f->node_info : NULL,
        .cc_list_len = f->node_info_len,
        .has_supported_protocols = true,
        .supported_protocols = region_has_long_range(f->id.region) ? 0x01u : 0x00u,
    };
    uint8_t payload[ZW_PAYLOAD_MAX];
    const size_t n = zw_api_started_build(payload, sizeof payload, &started);
    uint8_t frame[ZW_FRAME_MAX];
    const size_t fn = zw_frame_build(frame, sizeof frame, ZW_TYPE_REQUEST,
                                     (uint8_t)ZW_FUNC_SERIAL_API_STARTED, payload, n);
    if (n == 0u || fn == 0u) {
        f->overflow++;
        return;
    }
    queue_frame(f, now, frame, fn);
}

/* ---- The identity's answers (§5, §6), built with zwave_proto's codecs ---------------------- */

static void respond(fake_zac93_t *f, uint64_t at, uint8_t func, const uint8_t *payload, size_t n)
{
    if (n == 0u) {
        f->overflow++; /* a builder refused: the identity does not fit its codec */
        return;
    }
    (void)plan_frame(f, at, ZW_TYPE_RESPONSE, func, payload, n);
}

static bool setup_has(const fake_zac93_t *f, uint8_t sub)
{
    return ((unsigned)f->id.setup_supported[sub / 8u] >> (sub % 8u) & 1u) != 0u;
}

/* SerialAPISetup (§6.7): the sub-commands the identity has, else "unsupported". */
static void answer_setup(fake_zac93_t *f, uint64_t at, const zw_frame_t *fr)
{
    zw_api_api_setup_req_t req;
    uint8_t data[1u + 16u + FAKE_REGIONS_MAX];
    size_t dn = 0u;
    uint8_t sub;
    if (!zw_api_api_setup_req_parse(fr->payload, fr->payload_len, &req)) {
        f->overflow++;
        return;
    }
    sub = req.sub;
    if (sub == 0u || !setup_has(f, sub)) {
        data[dn++] = req.sub; /* 00 | the refused sub-command */
        sub = ZW_SETUP_UNSUPPORTED;
    } else if (sub == f->fault.refuse_setup) {
        data[dn++] = 0x00u; /* listed, but "false" */
    } else {
        switch (sub) {
        case ZW_SETUP_GET_SUPPORTED_COMMANDS: /* the legacy byte, then sub-commands 1..128 */
            data[dn++] = 0u;
            for (unsigned s = 1u; s <= 0x80u; s <<= 1) {
                if (setup_has(f, (uint8_t)s)) {
                    data[0] = (uint8_t)(data[0] | s);
                }
            }
            memset(&data[1], 0, 16u);
            for (unsigned s = 1u; s <= 128u; s++) {
                if (setup_has(f, (uint8_t)s)) {
                    data[1u + (s - 1u) / 8u] = (uint8_t)(data[1u + (s - 1u) / 8u] |
                                                         1u << ((s - 1u) % 8u));
                }
            }
            dn = 17u;
            break;
        case ZW_SETUP_SET_TX_STATUS_REPORT:
            f->tx_report = req.data_len > 0u && req.data[0] != 0u;
            data[dn++] = 0x01u;
            break;
        case ZW_SETUP_GET_MAXIMUM_PAYLOAD_SIZE:
            data[dn++] = f->id.max_payload;
            break;
        case ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE:
            data[dn++] = f->id.lr_max_payload;
            break;
        case ZW_SETUP_GET_SUPPORTED_REGIONS:
            data[dn++] = f->id.region_count;
            memcpy(&data[dn], f->id.regions, f->id.region_count);
            dn += f->id.region_count;
            break;
        case ZW_SETUP_GET_REGION_INFO: {
            const uint8_t r = req.data_len > 0u ? req.data[0] : 0xFEu;
            data[dn++] = r;
            data[dn++] = (uint8_t)(0x01u | (region_has_long_range(r) ? 0x02u : 0u));
            if (region_has_long_range(r)) {
                data[dn++] = r == 0x09u ? 0x01u : 0x00u; /* the classic region under it */
            }
            break;
        }
        case ZW_SETUP_GET_RF_REGION:
            data[dn++] = f->id.region;
            break;
        case ZW_SETUP_SET_RF_REGION: { /* only a region it has */
            const uint8_t r = req.data_len > 0u ? req.data[0] : 0xFEu;
            const bool has = memchr(f->id.regions, r, f->id.region_count) != NULL;
            if (has) {
                f->next_region = r;
            }
            data[dn++] = has ? 0x01u : 0x00u;
            break;
        }
        case ZW_SETUP_SET_NODE_ID_TYPE:
            f->node_id_bits = req.data_len > 0u && req.data[0] == 0x02u ? 16u : 8u;
            data[dn++] = 0x01u;
            break;
        case ZW_SETUP_GET_POWERLEVEL:
            data[dn++] = (uint8_t)(int8_t)f->power_dbm10;
            data[dn++] = (uint8_t)(int8_t)f->power_cal_dbm10;
            break;
        case ZW_SETUP_GET_POWERLEVEL_16BIT:
            data[dn++] = (uint8_t)((uint16_t)f->power_dbm10 >> 8);
            data[dn++] = (uint8_t)((uint16_t)f->power_dbm10 & 0xFFu);
            data[dn++] = (uint8_t)((uint16_t)f->power_cal_dbm10 >> 8);
            data[dn++] = (uint8_t)((uint16_t)f->power_cal_dbm10 & 0xFFu);
            break;
        case ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER:
            data[dn++] = (uint8_t)((uint16_t)f->lr_power_dbm10 >> 8);
            data[dn++] = (uint8_t)((uint16_t)f->lr_power_dbm10 & 0xFFu);
            break;
        case ZW_SETUP_SET_POWERLEVEL:
        case ZW_SETUP_SET_POWERLEVEL_16BIT: {
            const bool wide = sub == ZW_SETUP_SET_POWERLEVEL_16BIT;
            const bool ok = req.data_len >= (wide ? 4u : 2u);
            if (ok) {
                f->next_power_dbm10 = wide ? (int16_t)(uint16_t)((unsigned)req.data[0] << 8 |
                                                                 (unsigned)req.data[1])
                                           : (int16_t)(int8_t)req.data[0];
                f->power_sets++;
            }
            data[dn++] = ok ? 0x01u : 0x00u;
            break;
        }
        case ZW_SETUP_SET_LONG_RANGE_MAXIMUM_TX_POWER: {
            const bool ok = req.data_len >= 2u;
            if (ok) {
                f->next_lr_power_dbm10 = (int16_t)(uint16_t)((unsigned)req.data[0] << 8 |
                                                             (unsigned)req.data[1]);
                f->power_sets++;
            }
            data[dn++] = ok ? 0x01u : 0x00u;
            break;
        }
        default:
            data[dn++] = 0x01u;
            break;
        }
    }
    const zw_api_api_setup_rsp_t rsp = {.sub = sub, .data = data, .data_len = dn};
    uint8_t out[ZW_PAYLOAD_MAX];
    respond(f, at, (uint8_t)ZW_FUNC_SERIAL_API_SETUP, out,
            zw_api_api_setup_rsp_build(out, sizeof out, &rsp));
}

/* A start-up command answered from the identity. False: not one of them. */
static bool answer_from_identity(fake_zac93_t *f, uint64_t at, const zw_frame_t *fr)
{
    static const uint8_t no_git_hash[16] = {0};
    const fake_identity_t *id = &f->id;
    const zw_nid_width_t w = f->node_id_bits == 16u ? ZW_NID_16 : ZW_NID_8;
    uint8_t out[ZW_PAYLOAD_MAX];
    size_t n = 0u;
    switch (fr->func) {
    case ZW_FUNC_GET_SERIAL_API_CAPABILITIES: {
        const zw_api_api_caps_rsp_t caps = {
            .app_version_major = id->firmware[0],
            .app_version_minor = id->firmware[1],
            .manufacturer_id = id->manufacturer,
            .product_type = id->product_type,
            .product_id = id->product_id,
            .function_mask = id->function_mask,
        };
        n = zw_api_api_caps_rsp_build(out, sizeof out, &caps);
        break;
    }
    case ZW_FUNC_GET_SERIAL_API_INIT_DATA: {
        const zw_api_init_data_rsp_t init = {
            .serial_api_version = id->api_version,
            .is_controller = (id->api_capabilities & 0x01u) == 0u,
            .has_timers = (id->api_capabilities & 0x02u) != 0u,
            .is_secondary = (id->api_capabilities & 0x04u) != 0u,
            .is_sis = (id->api_capabilities & 0x08u) != 0u,
            .node_list = id->node_list,
            .chip_type = id->chip_type,
            .chip_version = id->chip_version,
        };
        n = zw_api_init_data_rsp_build(out, sizeof out, &init);
        break;
    }
    case ZW_FUNC_GET_CONTROLLER_VERSION: {
        const zw_api_controller_version_rsp_t ver = {
            .library = id->library,
            .library_len = strlen(id->library),
            .library_type = id->library_type,
        };
        n = zw_api_controller_version_rsp_build(out, sizeof out, &ver);
        break;
    }
    case ZW_FUNC_GET_PROTOCOL_VERSION: {
        const zw_api_protocol_version_rsp_t pv = {
            .protocol_type = 0x00u,
            .major = id->protocol[0],
            .minor = id->protocol[1],
            .patch = id->protocol[2],
            .app_framework_build = 0x0120u,
            .git_hash = no_git_hash,
        };
        n = zw_api_protocol_version_rsp_build(out, sizeof out, &pv);
        break;
    }
    case ZW_FUNC_GET_CONTROLLER_CAPABILITIES: {
        const zw_api_controller_caps_rsp_t caps = {
            .is_secondary = (id->controller_caps & 0x01u) != 0u,
            .on_other_network = (id->controller_caps & 0x02u) != 0u,
            .sis_present = (id->controller_caps & 0x04u) != 0u,
            .was_real_primary = (id->controller_caps & 0x08u) != 0u,
            .is_suc = (id->controller_caps & 0x10u) != 0u,
            .no_nodes_included = (id->controller_caps & 0x20u) != 0u,
        };
        n = zw_api_controller_caps_rsp_build(out, sizeof out, &caps);
        break;
    }
    case ZW_FUNC_SERIAL_API_SETUP:
        answer_setup(f, at, fr);
        return true;
    case ZW_FUNC_SET_APPLICATION_NODE_INFORMATION: { /* no response; its CCs go in the next
                                                     * SerialAPIStarted */
        zw_api_app_node_info_req_t nif;
        if (!zw_api_app_node_info_req_parse(fr->payload, fr->payload_len, &nif) ||
            nif.cc_list_len > sizeof f->node_info) {
            f->overflow++;
            return true;
        }
        f->node_info_len = nif.cc_list_len;
        if (nif.cc_list_len > 0u) {
            memcpy(f->node_info, nif.cc_list, nif.cc_list_len);
        }
        return true;
    }
    case ZW_FUNC_GET_CONTROLLER_ID: {
        zw_api_controller_id_rsp_t cid = {.own_node_id = id->node_id};
        cid.home_id[0] = (uint8_t)(id->home_id >> 24);
        cid.home_id[1] = (uint8_t)(id->home_id >> 16);
        cid.home_id[2] = (uint8_t)(id->home_id >> 8);
        cid.home_id[3] = (uint8_t)id->home_id;
        n = zw_api_controller_id_rsp_build(out, sizeof out, w, &cid);
        break;
    }
    case ZW_FUNC_GET_SUC_NODE_ID: {
        const zw_api_get_suc_node_id_rsp_t suc = {.node_id = id->suc_node_id};
        n = zw_api_get_suc_node_id_rsp_build(out, sizeof out, w, &suc);
        break;
    }
    case ZW_FUNC_SET_SUC_NODE_ID: { /* its own node: at once, no radio traffic (§6.9) */
        zw_api_set_suc_node_id_req_t req;
        if (!zw_api_set_suc_node_id_req_parse(fr->payload, fr->payload_len, w, &req)) {
            f->overflow++;
            return true;
        }
        const zw_api_set_suc_node_id_rsp_t rsp = {.started = true};
        respond(f, at, fr->func, out, zw_api_set_suc_node_id_rsp_build(out, sizeof out, &rsp));
        if (req.node_id != id->node_id && req.callback_id != 0u) {
            const zw_api_set_suc_node_id_cb_t cb = {.callback_id = req.callback_id,
                                                    .status = ZW_SUC_STATUS_SUCCEEDED};
            n = zw_api_set_suc_node_id_cb_build(out, sizeof out, &cb);
            (void)plan_frame(f, at, ZW_TYPE_REQUEST, fr->func, out, n);
        }
        f->id.suc_node_id = req.enable_suc ? req.node_id : (uint16_t)0u;
        return true;
    }
    case ZW_FUNC_SET_SERIAL_API_TIMEOUTS: {
        zw_api_api_timeouts_req_t req;
        if (!zw_api_api_timeouts_req_parse(fr->payload, fr->payload_len, &req)) {
            f->overflow++;
            return true;
        }
        const zw_api_api_timeouts_rsp_t old = {.ack_timeout_x10ms = id->old_timeouts[0],
                                               .byte_timeout_x10ms = id->old_timeouts[1]};
        n = zw_api_api_timeouts_rsp_build(out, sizeof out, &old);
        f->id.old_timeouts[0] = req.ack_timeout_x10ms;
        f->id.old_timeouts[1] = req.byte_timeout_x10ms;
        break;
    }
    case ZW_FUNC_GET_LONG_RANGE_NODES: { /* no Long Range nodes: one empty segment */
        static const uint8_t empty[ZW_LR_SEGMENT_BYTES] = {0};
        zw_api_lr_nodes_req_t req;
        if (!zw_api_lr_nodes_req_parse(fr->payload, fr->payload_len, &req)) {
            f->overflow++;
            return true;
        }
        const zw_api_lr_nodes_rsp_t rsp = {.more_segments = false, .segment = req.segment,
                                           .node_list = empty};
        n = zw_api_lr_nodes_rsp_build(out, sizeof out, &rsp);
        break;
    }
    default:
        return false;
    }
    respond(f, at, fr->func, out, n);
    return true;
}

/* ---- Hearing the host --------------------------------------------------------------------- */

static fake_rule_t *find_rule(fake_zac93_t *f, uint8_t func)
{
    for (size_t i = 0; i < f->n_rules; i++) {
        if (f->rules[i].func == func) {
            return &f->rules[i];
        }
    }
    return NULL;
}

/* SendDataAbort: the pending SendData callback now, if the script says so. */
static void abort_send_data(fake_zac93_t *f, uint64_t base)
{
    fake_rule_t *r = find_rule(f, (uint8_t)ZW_FUNC_SEND_DATA);
    if (r == NULL || !r->callback_on_abort || f->send_data_cb == 0u) {
        return;
    }
    for (size_t i = 0; i < FAKE_ZAC93_TIMED; i++) {
        fake_timed_t *e = &f->timed[i];
        if (e->used && e->kind == FAKE_T_FRAME && e->len >= 6u && e->bytes[2] == ZW_TYPE_REQUEST &&
            e->bytes[3] == (uint8_t)ZW_FUNC_SEND_DATA && e->bytes[4] == f->send_data_cb) {
            e->used = false;
        }
    }
    plan_callback(f, base + r->abort_callback.after_ms, (uint8_t)ZW_FUNC_SEND_DATA,
                  f->send_data_cb, &r->abort_callback);
    f->send_data_cb = 0u;
}

/* ---- The radio network (network.md §4) ------------------------------------------------------ */

static zw_nid_width_t module_nid(const fake_zac93_t *f)
{
    return f->node_id_bits == 16u ? ZW_NID_16 : ZW_NID_8;
}

static fake_zdev_t *device_at(fake_zac93_t *f, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < f->n_devices; i++) {
        if (f->devices[i]->node == node && f->devices[i]->status != FAKE_ZDEV_OUT) {
            return f->devices[i];
        }
    }
    return NULL;
}

/* An AddNode status report (§6.16), the device's node information with 03 and 04. */
static void plan_add_status(fake_zac93_t *f, uint64_t at, uint8_t cb_id, uint8_t status,
                            uint16_t node, const fake_zdev_t *d)
{
    zw_api_add_node_cb_t cb;
    memset(&cb, 0, sizeof cb);
    cb.callback_id = cb_id;
    cb.status = status;
    cb.node.node_id = node;
    if (d != NULL && (status == ZW_ADD_NODE_STATUS_ADDING_END_NODE ||
                      status == ZW_ADD_NODE_STATUS_ADDING_CONTROLLER)) {
        cb.has_node_info = true;
        cb.node.basic_class = d->basic;
        cb.node.generic_class = d->generic;
        cb.node.specific_class = d->specific;
        cb.node.cc_list = d->nif;
        cb.node.cc_list_len = d->nif_len;
    }
    uint8_t p[ZW_PAYLOAD_MAX];
    const size_t n = zw_api_add_node_cb_build(p, sizeof p, module_nid(f), &cb);
    if (n == 0u) {
        f->overflow++;
        return;
    }
    (void)plan_frame(f, at, ZW_TYPE_REQUEST, (uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK, p, n);
}

/* AddNodeToNetwork: start listening (Ready), or stop - after ProtocolDone, with its Done. */
static void answer_add_node(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_add_node_req_t req;
    if (!zw_api_add_node_req_parse(fr->payload, fr->payload_len, &req)) {
        f->overflow++;
        return;
    }
    const uint8_t type = req.mode & ZW_ADD_NODE_TYPE_MASK;
    f->smart_listen = type == ZW_ADD_NODE_TYPE_SMARTSTART_LISTEN; /* any other ends it */
    if (type == ZW_ADD_NODE_TYPE_STOP) {
        f->replace_ready = false; /* the controller stops waiting for a replacement too */
    }
    if (type == ZW_ADD_NODE_TYPE_SMARTSTART_DSK) {
        f->smart_dsk_adds++;
        fake_zdev_t *d = NULL;
        for (size_t i = 0; i < f->n_devices && d == NULL; i++) {
            uint8_t nwi[4], auth[4];
            zw_dsk_home_ids(f->devices[i]->pub, nwi, auth);
            if (f->devices[i]->status == FAKE_ZDEV_OUT &&
                memcmp(nwi, req.nwi_home_id, sizeof nwi) == 0 &&
                memcmp(auth, req.auth_home_id, sizeof auth) == 0) {
                d = f->devices[i];
            }
        }
        f->add_state = ADD_LISTENING;
        f->add_cb = req.callback_id;
        f->joining = NULL;
        if (d == NULL) {
            plan_add_status(f, base + 5u, req.callback_id, ZW_ADD_NODE_STATUS_FAILED, 0u, NULL);
            f->add_state = ADD_IDLE;
            return;
        }
        if (!f->skip_ready) {
            plan_add_status(f, base + 5u, req.callback_id, ZW_ADD_NODE_STATUS_READY, 0u, NULL);
        }
        (void)fake_zac93_press(f, base + 10u, d);
    } else if (type == ZW_ADD_NODE_TYPE_ANY) {
        f->add_state = ADD_LISTENING;
        f->add_cb = req.callback_id;
        f->joining = NULL;
        if (!f->skip_ready) {
            plan_add_status(f, base + 5u, req.callback_id, ZW_ADD_NODE_STATUS_READY, 0u, NULL);
        }
    } else if (type == ZW_ADD_NODE_TYPE_STOP) {
        if (req.callback_id != 0u && f->add_state == ADD_JOINING) {
            plan_add_status(f, base + 20u, req.callback_id, ZW_ADD_NODE_STATUS_DONE, f->join_id,
                            NULL);
        }
        f->add_state = ADD_IDLE;
        f->joining = NULL;
    }
}

/* Any device that ever had this node ID in this network (one that left is still listed). */
static fake_zdev_t *device_any(fake_zac93_t *f, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < f->n_devices; i++) {
        if (f->devices[i]->node == node) {
            return f->devices[i];
        }
    }
    return NULL;
}

/* SendData to a node: queued; its device hears it half-way through radio_ms - unless the frame
 * is lost, the device sleeps, or nobody is there - and the callback at the end says OK or NoAck
 * (a NoAck puts the node on the failed list, serial-api.md 6.18). */
static bool answer_send_data(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_send_data_req_t req;
    if (!zw_api_send_data_req_parse(fr->payload, fr->payload_len, module_nid(f), &req)) {
        return false;
    }
    fake_zdev_t *d = device_at(f, req.node_id);
    const uint8_t queued = 0x01u;
    (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, &queued, 1u);
    bool heard = d != NULL && fake_zdev_awake(d, base + f->radio_ms / 2u);
    if (d != NULL && d->lose_in > 0u) {
        d->lose_in--;
        heard = false;
    } else if (d != NULL && d->lose_in_after > 0u && --d->lose_in_after == 0u) {
        d->lose_in = 1u; /* this one is heard, the next is not */
    }
    if (d != NULL && d->deaf_cc != 0u && req.command_len > 0u && req.command[0] == d->deaf_cc) {
        heard = false;
    }
    if (req.node_id >= 1u && req.node_id <= 232u) {
        zw_node_list_set(f->failed, req.node_id, !heard && !f->failed_off);
    }
    bool blocked = f->air_blocked;
    if (!blocked && f->jam_sends > 0u) {
        f->jam_sends--;
        blocked = true; /* jammed this once: the module never had the air */
    }
    if (blocked) {
        heard = false;
        f->jammed_sends++;
    }
    if (heard) {
        fake_timed_t *e = plan_entry(f, base + f->radio_ms / 2u, FAKE_T_DELIVER, 0u, req.command,
                                     req.command_len);
        if (e != NULL) {
            e->node = req.node_id;
        }
    }
    const uint8_t status = blocked ? (uint8_t)ZW_TX_STATUS_FAIL
                           : heard ? (uint8_t)ZW_TX_STATUS_OK
                                   : (uint8_t)ZW_TX_STATUS_NO_ACK;
    zw_api_send_data_cb_t cb = {req.callback_id, status, false, {0}};
    if (f->tx_report || blocked) {
        cb.has_tx_report = true; /* the transmit time: none when the air was never had */
        cb.tx_report.transmit_ticks = (uint16_t)(blocked ? 0u : f->radio_ms / 10u + 1u);
        cb.tx_report.ack_rssi = (uint8_t)f->ack_rssi;
        cb.tx_report.repeater_count = f->ack_repeaters;
        for (size_t i = 0; i < 4u; i++) {
            cb.tx_report.repeater_node_ids[i] = f->ack_repeater_ids[i];
        }
    }
    uint8_t p[40];
    const size_t n = zw_api_send_data_cb_build(p, sizeof p, &cb);
    (void)plan_frame(f, base + f->radio_ms, ZW_TYPE_REQUEST, fr->func, p, n);
    return true;
}

/* ReplaceFailedNode (§6.18): `node | callback`; response 00 started or 08 not in the failed
 * list; then "the node answered" or "ready". */
static void answer_replace(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_assign_suc_return_route_req_t req;
    if (!zw_api_assign_suc_return_route_req_parse(fr->payload, fr->payload_len, module_nid(f),
                                                  &req)) {
        f->overflow++;
        return;
    }
    f->replace_asks++;
    const bool listed = req.node_id >= 1u && req.node_id <= 232u &&
                        zw_node_list_test(f->failed, req.node_id);
    const uint8_t response = listed ? 0x00u : 0x08u;
    (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, &response, 1u);
    if (!listed) {
        return;
    }
    const fake_zdev_t *d = device_at(f, req.node_id);
    const bool answers = d != NULL && fake_zdev_awake(d, base + 20u);
    const zw_api_assign_suc_return_route_cb_t cb = {req.callback_id, answers ? 0x00u : 0x03u};
    uint8_t p[4];
    (void)plan_frame(f, base + 40u, ZW_TYPE_REQUEST, fr->func, p,
                     zw_api_assign_suc_return_route_cb_build(p, sizeof p, &cb));
    f->replace_ready = !answers;
    f->replace_node = req.node_id;
    f->replace_cb = req.callback_id;
}

bool fake_zac93_press_replace(fake_zac93_t *f, uint64_t now, fake_zdev_t *d)
{
    if (!f->replace_ready) {
        return false;
    }
    f->replace_ready = false;
    for (size_t i = 0; i < f->n_devices; i++) {
        if (f->devices[i] != d && f->devices[i]->node == f->replace_node) {
            f->devices[i]->node = 0u; /* the old one is out of the network */
            f->devices[i]->status = FAKE_ZDEV_OUT;
        }
    }
    zw_node_list_set(f->failed, f->replace_node, false);
    d->now = now;
    fake_zdev_joined(d, f->id.home_id, f->replace_node);
    const zw_api_assign_suc_return_route_cb_t cb = {f->replace_cb, 0x04u};
    uint8_t p[4];
    return plan_frame(f, now + 2u * f->join_step_ms, ZW_TYPE_REQUEST,
                      (uint8_t)ZW_FUNC_REPLACE_FAILED_NODE, p,
                      zw_api_assign_suc_return_route_cb_build(p, sizeof p, &cb));
}

/* RequestNodeNeighborUpdate and DeleteReturnRoute (route repair). */
static void answer_routing(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_assign_suc_return_route_req_t req;
    if (!zw_api_assign_suc_return_route_req_parse(fr->payload, fr->payload_len, module_nid(f),
                                                  &req)) {
        f->overflow++;
        return;
    }
    const fake_zdev_t *d = device_at(f, req.node_id);
    const bool hears = d != NULL && fake_zdev_awake(d, base + 20u);
    uint8_t p[4];
    if (fr->func == (uint8_t)ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE) {
        f->neighbor_asks++;
        if (f->routed_n < sizeof f->routed / sizeof f->routed[0]) {
            f->routed[f->routed_n++] = req.node_id;
        }
        zw_api_assign_suc_return_route_cb_t cb = {req.callback_id, ZW_NEIGHBOR_UPDATE_STARTED};
        (void)plan_frame(f, base + 5u, ZW_TYPE_REQUEST, fr->func, p,
                         zw_api_assign_suc_return_route_cb_build(p, sizeof p, &cb));
        const bool fail = f->neighbor_fails > 0u || !hears;
        if (f->neighbor_fails > 0u) {
            f->neighbor_fails--;
        }
        cb.transmit_status = fail ? ZW_NEIGHBOR_UPDATE_FAILED : ZW_NEIGHBOR_UPDATE_DONE;
        (void)plan_frame(f, base + 200u, ZW_TYPE_REQUEST, fr->func, p,
                         zw_api_assign_suc_return_route_cb_build(p, sizeof p, &cb));
        return;
    }
    f->delete_route_asks++;
    const uint8_t started = 0x01u;
    (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, &started, 1u);
    const zw_api_assign_suc_return_route_cb_t cb = {
        req.callback_id, hears ? (uint8_t)ZW_TX_STATUS_OK : (uint8_t)ZW_TX_STATUS_NO_ACK};
    (void)plan_frame(f, base + 30u, ZW_TYPE_REQUEST, fr->func, p,
                     zw_api_assign_suc_return_route_cb_build(p, sizeof p, &cb));
}

static void answer_route(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_assign_suc_return_route_req_t req;
    if (!zw_api_assign_suc_return_route_req_parse(fr->payload, fr->payload_len, module_nid(f),
                                                  &req)) {
        f->overflow++;
        return;
    }
    f->suc_route_asks++;
    const uint8_t started = 0x01u;
    (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, &started, 1u);
    const zw_api_assign_suc_return_route_cb_t cb = {
        req.callback_id, f->route_fails ? (uint8_t)ZW_TX_STATUS_FAIL : (uint8_t)ZW_TX_STATUS_OK};
    uint8_t p[4];
    (void)plan_frame(f, base + 30u, ZW_TYPE_REQUEST, fr->func, p,
                     zw_api_assign_suc_return_route_cb_build(p, sizeof p, &cb));
}

/* GetNodeProtocolInfo (serial-api.md 6.10), from the device that has the node ID. */
static void answer_protocol_info(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_node_protocol_info_req_t q;
    uint8_t out[8];
    if (!zw_api_node_protocol_info_req_parse(fr->payload, fr->payload_len, module_nid(f), &q)) {
        f->overflow++;
        return;
    }
    const fake_zdev_t *d = device_any(f, q.node_id);
    zw_api_node_protocol_info_rsp_t r;
    memset(&r, 0, sizeof r);
    if (d != NULL) {
        r.always_listening = d->m.listening;
        r.routing = true;
        r.speed_40k = true;
        r.protocol_version = 3u;
        r.flirs = d->m.flirs ? ZW_FLIRS_1000MS : ZW_FLIRS_NONE;
        r.supports_beaming = true;
        r.is_controller_type = d->controller;
        r.has_specific_device_class = true;
        r.speed_100k_classic = true;
        r.basic_device_class = d->basic;
        r.generic_device_class = d->generic;
        r.specific_device_class = d->specific;
    }
    respond(f, base, fr->func, out, zw_api_node_protocol_info_rsp_build(out, sizeof out, &r));
}

/* RequestNodeInfo (6.13): "sent", then ApplicationUpdate 84 with its node information - or 81
 * when it did not answer (asleep, gone). */
static void answer_node_info(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_request_node_info_req_t q;
    if (!zw_api_request_node_info_req_parse(fr->payload, fr->payload_len, module_nid(f), &q)) {
        f->overflow++;
        return;
    }
    const uint8_t sent = 0x01u;
    (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, &sent, 1u);
    const fake_zdev_t *d = device_at(f, q.node_id);
    const uint64_t at = base + f->radio_ms;
    uint8_t data[ZW_PAYLOAD_MAX];
    uint8_t out[ZW_PAYLOAD_MAX];
    zw_api_app_update_t u = {ZW_APP_UPDATE_NODE_INFO_REQUEST_FAILED, NULL, 0u};
    if (d != NULL && fake_zdev_awake(d, base + f->radio_ms / 2u)) {
        const zw_api_node_update_t nu = {q.node_id, d->basic, d->generic, d->specific, d->nif,
                                         d->nif_len};
        u.type = ZW_APP_UPDATE_NODE_INFO_RECEIVED;
        u.data = data;
        u.data_len = zw_api_node_update_build(data, sizeof data, module_nid(f), &nu);
    }
    const size_t n = zw_api_app_update_build(out, sizeof out, &u);
    if (n == 0u) {
        f->overflow++;
        return;
    }
    (void)plan_frame(f, at, ZW_TYPE_REQUEST, (uint8_t)ZW_FUNC_APPLICATION_UPDATE, out, n);
}

static void plan_remove_status(fake_zac93_t *f, uint64_t at, uint8_t status, uint16_t node)
{
    zw_api_remove_node_cb_t cb;
    memset(&cb, 0, sizeof cb);
    cb.callback_id = f->remove_cb;
    cb.status = status;
    cb.node.node_id = node;
    uint8_t p[8];
    const size_t n = zw_api_remove_node_cb_build(p, sizeof p, module_nid(f), &cb);
    if (n == 0u) {
        f->overflow++;
        return;
    }
    (void)plan_frame(f, at, ZW_TYPE_REQUEST, (uint8_t)ZW_FUNC_REMOVE_NODE_FROM_NETWORK, p, n);
}

/* RemoveNodeFromNetwork (6.17): listen (Ready), or stop. */
static void answer_remove_node(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    zw_api_remove_node_req_t req;
    if (!zw_api_remove_node_req_parse(fr->payload, fr->payload_len, &req)) {
        f->overflow++;
        return;
    }
    if ((req.mode & 0x0Fu) == ZW_REMOVE_NODE_TYPE_ANY) {
        f->smart_listen = false;
        f->remove_state = 1u;
        f->remove_cb = req.callback_id;
        if (!f->skip_remove_ready) {
            plan_remove_status(f, base + 5u, ZW_REMOVE_NODE_STATUS_READY, 0u);
        }
    } else {
        f->remove_state = 0u;
    }
}

/* IsFailedNode / RemoveFailedNode (6.18). */
static void answer_failed(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    uint16_t node = 0u;
    if (!zw_api_node_id_parse(fr->payload, fr->payload_len, module_nid(f), &node)) {
        f->overflow++;
        return;
    }
    const bool failed = node >= 1u && node <= 232u && zw_node_list_test(f->failed, node);
    if (fr->func == (uint8_t)ZW_FUNC_IS_FAILED_NODE) {
        const uint8_t r = failed ? 0x01u : 0x00u;
        respond(f, base, fr->func, &r, 1u);
        return;
    }
    const uint8_t started = failed ? 0x00u : 0x08u; /* 08: not in the failed list */
    respond(f, base, fr->func, &started, 1u);
    if (!failed) {
        return;
    }
    const fake_zdev_t *d = device_at(f, node);
    const bool answers = d != NULL && fake_zdev_awake(d, base + 25u);
    const uint8_t cb[2] = {fr->payload[fr->payload_len - 1u], answers ? 0x00u : 0x01u};
    (void)plan_frame(f, base + 50u, ZW_TYPE_REQUEST, fr->func, cb, sizeof cb);
    if (!answers) {
        zw_node_list_set(f->id.node_list, node, false);
        zw_node_list_set(f->failed, node, false);
    }
}

/* HardReset (6.14): its callback, then a new network - a new home ID, no nodes - and a restart. */
static void answer_hard_reset(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    const uint8_t cb = fr->payload_len > 0u ? fr->payload[0] : 0u;
    (void)plan_frame(f, base + 50u, ZW_TYPE_REQUEST, fr->func, &cb, 1u);
    f->hard_resets++;
    f->id.home_id = f->next_home_id != 0u ? f->next_home_id : f->id.home_id + 0x00010101u;
    memset(f->id.node_list, 0, sizeof f->id.node_list);
    zw_node_list_set(f->id.node_list, f->id.node_id, true);
    memset(f->failed, 0, sizeof f->failed);
    f->id.suc_node_id = 0u;
    f->next_node = 2u;
    for (size_t i = 0; i < f->n_devices; i++) {
        f->devices[i]->status = FAKE_ZDEV_OUT; /* each must be reset and added again */
        f->devices[i]->node = 0u;
    }
    (void)plan(f, base + 100u, FAKE_T_RESTART, FAKE_REASON_RESET, NULL, 0u);
}

/* The network's functions; false when it is not one of them. */
static bool answer_network(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    switch (fr->func) {
    case ZW_FUNC_ADD_NODE_TO_NETWORK:
        answer_add_node(f, base, fr);
        return true;
    case ZW_FUNC_REMOVE_NODE_FROM_NETWORK:
        answer_remove_node(f, base, fr);
        return true;
    case ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE:
        answer_route(f, base, fr);
        return true;
    case ZW_FUNC_SEND_DATA:
        return answer_send_data(f, base, fr);
    case ZW_FUNC_GET_NODE_PROTOCOL_INFO:
        answer_protocol_info(f, base, fr);
        return true;
    case ZW_FUNC_REQUEST_NODE_INFO:
        answer_node_info(f, base, fr);
        return true;
    case ZW_FUNC_IS_FAILED_NODE:
    case ZW_FUNC_REMOVE_FAILED_NODE:
        answer_failed(f, base, fr);
        return true;
    case ZW_FUNC_HARD_RESET:
        answer_hard_reset(f, base, fr);
        return true;
    case ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE:
    case ZW_FUNC_DELETE_RETURN_ROUTE:
        answer_routing(f, base, fr);
        return true;
    case ZW_FUNC_REPLACE_FAILED_NODE:
        answer_replace(f, base, fr);
        return true;
    case ZW_FUNC_GET_BACKGROUND_RSSI:
        f->bg_rssi_asks++;
        (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, (const uint8_t *)f->bg_rssi,
                         (size_t)(unsigned)f->bg_rssi_n);
        return true;
    default:
        return false;
    }
}

/* What the module does with a request, its ACK sent (or lost) at `base`: a function muted by a
 * fault gets nothing, a garbled one a response too short to be its answer; SoftReset and
 * SendDataAbort their own; then the test's script for that FUNC, else the network's functions,
 * else the identity's answer. */
static void act(fake_zac93_t *f, uint64_t base, const zw_frame_t *fr)
{
    if (fr->type != ZW_TYPE_REQUEST) {
        return;
    }
    if (f->fault.mute_count > 0u && fr->func == f->fault.mute_func) {
        if (f->fault.mute_count != FAKE_MUTE_FOREVER) {
            f->fault.mute_count--;
        }
        return;
    }
    if (f->fault.garble_count > 0u && fr->func == f->fault.garble_func) {
        const uint8_t garbled = 0x00u;
        f->fault.garble_count--;
        (void)plan_frame(f, base, ZW_TYPE_RESPONSE, fr->func, &garbled, 1u);
        return;
    }
    if (fr->func == (uint8_t)ZW_FUNC_SOFT_RESET) {
        if (!f->fault.ignore_soft_reset) {
            (void)plan(f, base + f->soft_reset_ms, FAKE_T_RESTART, FAKE_REASON_SOFT_RESET, NULL,
                       0u);
        }
        return;
    }
    if (fr->func == (uint8_t)ZW_FUNC_SEND_DATA_ABORT) {
        abort_send_data(f, base);
        return;
    }
    if (fr->func == (uint8_t)ZW_FUNC_ENTER_BOOTLOADER) { /* §6.23: no answer, its menu */
        f->fault.bootloader = true;
        f->xm_receiving = false;
        f->outbox_count = 0u;
        f->in_flight = false;
        emit_menu(f, base);
        return;
    }
    fake_rule_t *r = find_rule(f, fr->func);
    if (r == NULL) {
        if (!answer_network(f, base, fr) && f->identity_answers) {
            (void)answer_from_identity(f, base, fr);
        }
        return;
    }
    if (r->not_queued > 0u) {
        const uint8_t busy = 0x00u;
        r->not_queued--;
        (void)plan_frame(f, base + r->response.after_ms, ZW_TYPE_RESPONSE, fr->func, &busy, 1u);
        return;
    }
    if (r->respond) {
        (void)plan_frame(f, base + r->response.after_ms, ZW_TYPE_RESPONSE, fr->func,
                         r->response.bytes, r->response.len);
    }
    const uint8_t cb_id = fr->payload_len > r->callback_id_from_end
                              ? fr->payload[fr->payload_len - 1u - r->callback_id_from_end]
                              : 0u;
    for (size_t i = 0; i < r->callbacks && i < FAKE_ZAC93_CALLBACKS; i++) {
        plan_callback(f, base + r->callback[i].after_ms, fr->func, cb_id, &r->callback[i]);
    }
    if (fr->func == (uint8_t)ZW_FUNC_SEND_DATA && (r->callbacks > 0u || r->callback_on_abort)) {
        f->send_data_cb = cb_id;
    }
}

static fake_heard_t *log_heard(fake_zac93_t *f, uint64_t now, const zw_frame_t *fr)
{
    fake_heard_t *h = &f->log[f->heard % FAKE_ZAC93_LOG];
    f->heard++;
    f->by_func[fr->func]++;
    memset(h, 0, sizeof *h);
    h->at = now;
    h->type = fr->type;
    h->func = fr->func;
    h->payload_len = fr->payload_len;
    if (fr->payload != NULL && fr->payload_len > 0u) {
        memcpy(h->payload, fr->payload,
               fr->payload_len < sizeof h->payload ? fr->payload_len : sizeof h->payload);
    }
    h->raw_len = fr->raw_len;
    memcpy(h->raw, fr->raw, fr->raw_len < sizeof h->raw ? fr->raw_len : sizeof h->raw);
    return h;
}

/* A good frame from the host: the faults first, then an ACK and the answers. */
static void heard_frame(fake_zac93_t *f, uint64_t now, const zw_frame_t *fr)
{
    fake_heard_t *h = log_heard(f, now, fr);
    if (f->fault.nak_next > 0u) {
        f->fault.nak_next--;
        emit(f, ZW_NAK);
        h->answered = FAKE_NAKED;
        return;
    }
    if (f->fault.can_next) {
        f->fault.can_next = false;
        emit(f, ZW_CAN);
        h->answered = FAKE_CANNED;
        return;
    }
    uint64_t base = now;
    if (f->fault.drop_next_ack) {
        f->fault.drop_next_ack = false;
        h->answered = FAKE_ACK_LOST;
    } else if (f->fault.ack_delay_ms > 0u) {
        const uint8_t ack = ZW_ACK;
        base = now + f->fault.ack_delay_ms;
        f->fault.ack_delay_ms = 0u;
        (void)plan(f, base, FAKE_T_RAW, 0u, &ack, 1u);
        h->answered = FAKE_ACK_LATE;
    } else {
        emit(f, ZW_ACK);
        h->answered = FAKE_ACKED;
    }
    act(f, base, fr);
}

static void emit_text(fake_zac93_t *f, const char *s)
{
    emit_all(f, (const uint8_t *)s, strlen(s));
}

/* §8: the upload ended by the bootloader, with its reason, then its menu. */
static void xm_abort(fake_zac93_t *f, uint64_t now, uint8_t error)
{
    char line[48];
    f->xm_receiving = false;
    emit(f, ZW_CAN);
    snprintf(line, sizeof line, "\r\nSerial upload aborted\r\nerror 0x%02X\r\n", (unsigned)error);
    emit_text(f, line);
    emit_menu(f, now);
}

/* §8 step 3: one byte of the upload - a block's, or EOT / CAN between blocks. */
static void xm_byte(fake_zac93_t *f, uint64_t now, uint8_t b)
{
    if (f->xm_len == 0u && b == 0x04u) { /* EOT */
        f->xm_receiving = false;
        emit(f, ZW_ACK);
        if (f->xm_blocks > 0u) {
            f->uploads++;
        }
        emit_text(f, "\r\nSerial upload complete\r\n");
        emit_menu(f, now);
        return;
    }
    if (f->xm_len == 0u && b == ZW_CAN) { /* the host gave up */
        f->xm_receiving = false;
        emit_text(f, "\r\nSerial upload aborted\r\n");
        emit_menu(f, now);
        return;
    }
    if (f->xm_len == 0u && b != 0x01u) {
        return; /* line noise between blocks */
    }
    f->xm_buf[f->xm_len++] = b;
    if (f->xm_len < sizeof f->xm_buf) {
        return;
    }
    f->xm_len = 0u;
    const uint32_t number = f->xm_blocks + 1u;
    const uint8_t *d = &f->xm_buf[3];
    const uint16_t crc = zw_crc16(0x0000u, d, 128u);
    const bool good = f->xm_buf[1] == (uint8_t)(number & 0xFFu) &&
                      f->xm_buf[2] == (uint8_t)(0xFFu - (number & 0xFFu)) &&
                      f->xm_buf[131] == (uint8_t)(crc >> 8) && f->xm_buf[132] == (uint8_t)(crc & 0xFFu);
    if (number == f->fault.otw_silent_block) {
        return;
    }
    if (number == f->fault.otw_cancel_block &&
        (f->fault.otw_cancel_times == 0u || f->xm_cancels < f->fault.otw_cancel_times)) {
        f->xm_cancels++;
        xm_abort(f, now, f->fault.otw_cancel_error);
        return;
    }
    if (number == f->fault.otw_nak_block && f->xm_naks_left > 0u) {
        f->xm_naks_left--;
        emit(f, ZW_NAK);
        return;
    }
    if (!good) {
        emit(f, ZW_NAK);
        return;
    }
    f->xm_crc = zw_crc32(f->xm_crc, d, 128u);
    f->xm_blocks++;
    if (f->otw_block_ms > 0u) {
        static const uint8_t ack = ZW_ACK;
        (void)plan(f, now + f->otw_block_ms, FAKE_T_RAW, 0u, &ack, 1u);
        return;
    }
    emit(f, ZW_ACK);
}

/* In the bootloader, every byte is a key: "upload gbl" begins an upload, "run" starts the
 * application, anything else gets the menu again. */
static void bootloader_key(fake_zac93_t *f, uint64_t now, uint8_t key)
{
    if (f->xm_receiving) {
        xm_byte(f, now, key);
        return;
    }
    if (key == FAKE_BOOT_UPLOAD_KEY && !f->fault.otw_no_upload) {
        f->xm_receiving = true;
        f->xm_len = 0u;
        f->xm_blocks = 0u;
        f->xm_crc = 0u;
        f->xm_naks_left = f->fault.otw_nak_times != 0u ? f->fault.otw_nak_times : 1u;
        emit_text(f, "\r\nbegin upload\r\n");
        emit(f, 0x43u); /* 'C' */
        return;
    }
    if (key != FAKE_BOOT_RUN_KEY) {
        emit_menu(f, now);
        return;
    }
    if (f->uploads > 0u && f->fault.otw_bad_image) {
        emit_menu(f, now); /* the new firmware doesn't start: back in the bootloader */
        return;
    }
    if (f->uploads > 0u) {
        f->id.firmware[0] = f->next_firmware[0];
        f->id.firmware[1] = f->next_firmware[1];
    }
    f->fault.bootloader = false;
    f->falls_back = f->fault.bootloader_sticky;
    (void)plan(f, now + f->boot_ms, FAKE_T_RESTART, FAKE_REASON_RESET, NULL, 0u);
}

/* ---- API ---------------------------------------------------------------------------------- */

void fake_identity_default(fake_identity_t *id)
{
    static const uint8_t setup[] = {
        ZW_SETUP_GET_SUPPORTED_COMMANDS, ZW_SETUP_SET_TX_STATUS_REPORT,
        ZW_SETUP_GET_MAXIMUM_PAYLOAD_SIZE, ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE,
        ZW_SETUP_GET_SUPPORTED_REGIONS, ZW_SETUP_GET_REGION_INFO, ZW_SETUP_GET_RF_REGION,
        ZW_SETUP_SET_RF_REGION, ZW_SETUP_SET_NODE_ID_TYPE,
    };
    memset(id, 0, sizeof *id);
    id->firmware[0] = 1u;
    id->firmware[1] = 50u;
    id->manufacturer = 0x027Au;
    id->product_type = 0x0004u;
    id->product_id = 0x0611u;
    for (unsigned func = 1u; func <= 0xFFu; func++) { /* every function in §6's table */
        zw_api_func_info_t info;
        if (zw_api_func_info((uint8_t)func, &info)) {
            id->function_mask[(func - 1u) / 8u] =
                (uint8_t)(id->function_mask[(func - 1u) / 8u] | 1u << ((func - 1u) % 8u));
        }
    }
    id->api_version = 0x0Au;      /* Host API 1 */
    id->api_capabilities = 0x00u; /* a controller, primary, not SIS */
    zw_node_list_set(id->node_list, 1u, true);
    id->chip_type = 0x08u;
    id->chip_version = 0x00u;
    memcpy(id->library, "Z-Wave 7.22", sizeof "Z-Wave 7.22");
    id->library_type = 0x01u;     /* static controller */
    id->protocol[0] = 7u;
    id->protocol[1] = 22u;
    id->protocol[2] = 1u;
    id->controller_caps = 0x28u;  /* was the real primary, no nodes included yet */
    for (size_t i = 0; i < sizeof setup; i++) {
        id->setup_supported[setup[i] / 8u] =
            (uint8_t)(id->setup_supported[setup[i] / 8u] | 1u << (setup[i] % 8u));
    }
    id->max_payload = 46u;
    id->lr_max_payload = 150u;
    id->regions[0] = 0x01u; /* USA */
    id->regions[1] = 0x09u; /* USA Long Range */
    id->region_count = 2u;
    id->region = 0x01u;
    id->home_id = 0xC0FFEE01u;
    id->node_id = 1u;
    id->suc_node_id = 0u;
    id->old_timeouts[0] = 0x96u; /* 1500 ms */
    id->old_timeouts[1] = 0x0Fu; /* 150 ms */
    id->generic = 0x02u;         /* static controller */
    id->specific = 0x01u;
}

void fake_zac93_init(fake_zac93_t *f)
{
    memset(f, 0, sizeof *f);
    zw_rx_init(&f->rx);
    fake_identity_default(&f->id);
    f->identity_answers = true;
    f->next_region = f->id.region;
    f->node_id_bits = 8u;
    f->fault.corrupt_in = -1;
    f->soft_reset_ms = 20u;
    f->boot_ms = 200u;
    f->next_node = 2u;
    f->join_step_ms = 100u;
    f->bg_rssi[0] = f->bg_rssi[1] = f->bg_rssi[2] = -95; /* a quiet house */
    f->bg_rssi_n = 3u;
    f->rx_rssi = -60;
    f->ack_rssi = -60;
    f->power_dbm10 = f->next_power_dbm10 = 0;  /* 0 dBm, as a US ZAC93 ships (a guess) */
    f->power_cal_dbm10 = 33;
    f->lr_power_dbm10 = f->next_lr_power_dbm10 = 140;
    f->radio_ms = 10u;
    f->reply_ms = 8u;
}

bool fake_zac93_on(fake_zac93_t *f, const fake_rule_t *rule)
{
    fake_rule_t *r = find_rule(f, rule->func);
    if (r == NULL) {
        if (f->n_rules >= FAKE_ZAC93_RULES) {
            f->overflow++;
            return false;
        }
        r = &f->rules[f->n_rules++];
    }
    *r = *rule;
    return true;
}

bool fake_zac93_send_at(fake_zac93_t *f, uint64_t at_ms, uint8_t type, uint8_t func,
                        const uint8_t *payload, size_t len)
{
    return scripted(plan_frame_entry(f, at_ms, type, func, payload, len));
}

bool fake_zac93_send_raw_at(fake_zac93_t *f, uint64_t at_ms, const uint8_t *bytes, size_t len)
{
    return scripted(plan_entry(f, at_ms, FAKE_T_RAW, 0u, bytes, len));
}

bool fake_zac93_restart_at(fake_zac93_t *f, uint64_t at_ms, uint8_t reason)
{
    return scripted(plan_entry(f, at_ms, FAKE_T_RESTART, reason, NULL, 0u));
}

void fake_zac93_enter_bootloader(fake_zac93_t *f, uint64_t now_ms)
{
    zw_rx_init(&f->rx);
    forget_own_plans(f);
    f->outbox_head = 0u;
    f->outbox_count = 0u;
    f->in_flight = false;
    f->send_data_cb = 0u;
    f->add_state = ADD_IDLE;
    f->remove_state = 0u;
    f->joining = NULL;
    f->fault.bootloader = true;
    emit_menu(f, now_ms);
}

void fake_zac93_reset_line(fake_zac93_t *f, uint64_t now_ms, bool asserted)
{
    if (asserted) {
        f->reset_held = true;
        forget_own_plans(f);
        f->outbox_count = 0u;
        f->in_flight = false;
        return;
    }
    if (!f->reset_held) {
        return;
    }
    f->reset_held = false;
    f->fault.silent = false; /* a hung module comes back */
    (void)plan(f, now_ms + f->boot_ms, FAKE_T_RESTART, FAKE_REASON_RESET, NULL, 0u);
}

const fake_heard_t *fake_zac93_heard(const fake_zac93_t *f, size_t i)
{
    if (i >= f->heard || f->heard - i > FAKE_ZAC93_LOG) {
        return NULL;
    }
    return &f->log[i % FAKE_ZAC93_LOG];
}

void fake_zac93_receive(fake_zac93_t *f, uint64_t now_ms, const uint8_t *bytes, size_t len)
{
    if (out_of_it(f)) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (f->fault.bootloader) {
            bootloader_key(f, now_ms, bytes[i]);
            continue;
        }
        const zw_rx_event_t ev = zw_rx_feed(&f->rx, bytes[i], now_ms);
        switch (ev) {
        case ZW_RX_FRAME: {
            zw_frame_t fr;
            if (zw_rx_frame(&f->rx, &fr)) {
                heard_frame(f, now_ms, &fr);
            }
            break;
        }
        case ZW_RX_BAD_FRAME:
            emit(f, ZW_NAK);
            break;
        case ZW_RX_ACK:
            f->acks_in++;
            if (f->in_flight) {
                pop_in_flight(f, now_ms);
            }
            break;
        case ZW_RX_NAK:
            f->naks_in++;
            if (f->falls_back && !f->in_flight) {
                /* the application just started falls back into the bootloader */
                f->falls_back = false;
                f->fault.bootloader = true;
                emit_menu(f, now_ms);
                break;
            }
            resend_or_drop(f, now_ms);
            break;
        case ZW_RX_CAN:
            f->cans_in++;
            resend_or_drop(f, now_ms);
            break;
        default:
            break;
        }
    }
}

void fake_zac93_poll(fake_zac93_t *f, uint64_t now_ms)
{
    if (f->in_flight && now_ms >= f->in_flight_due) {
        resend_or_drop(f, now_ms);
    }
    for (size_t i = 0; i < f->n_devices; i++) {
        fake_zdev_poll(f->devices[i], now_ms);
    }
    for (;;) {
        /* the earliest due output, ties in the order they were planned */
        fake_timed_t *next = NULL;
        for (size_t i = 0; i < FAKE_ZAC93_TIMED; i++) {
            fake_timed_t *e = &f->timed[i];
            if (e->used && e->at <= now_ms &&
                (next == NULL || e->at < next->at || (e->at == next->at && e->seq < next->seq))) {
                next = e;
            }
        }
        if (next == NULL) {
            return;
        }
        next->used = false;
        if (out_of_it(f)) {
            continue; /* a hung or dead module sends nothing */
        }
        if (next->kind == FAKE_T_RESTART) {
            restart(f, now_ms, next->reason);
        } else if (next->kind == FAKE_T_DELIVER) {
            fake_zdev_t *d = device_at(f, next->node);
            if (d != NULL) {
                fake_zdev_hear(d, now_ms, next->bytes, next->len);
            }
        } else if (next->kind == FAKE_T_RAW) {
            emit_all(f, next->bytes, next->len);
        } else {
            if (next->len >= 6u && next->bytes[2] == ZW_TYPE_REQUEST &&
                next->bytes[3] == (uint8_t)ZW_FUNC_SEND_DATA &&
                next->bytes[4] == f->send_data_cb) {
                f->send_data_cb = 0u; /* its callback is on its way */
            }
            queue_frame(f, now_ms, next->bytes, next->len);
        }
    }
}

uint64_t fake_zac93_next_deadline(const fake_zac93_t *f)
{
    uint64_t d = f->in_flight ? f->in_flight_due : UINT64_MAX;
    for (size_t i = 0; i < FAKE_ZAC93_TIMED; i++) {
        if (f->timed[i].used && f->timed[i].at < d) {
            d = f->timed[i].at;
        }
    }
    for (size_t i = 0; i < f->n_devices; i++) {
        const uint64_t t = fake_zdev_next_deadline(f->devices[i]);
        if (t < d) {
            d = t;
        }
    }
    return d;
}

bool fake_zac93_add_device(fake_zac93_t *f, fake_zdev_t *d)
{
    if (f->n_devices >= FAKE_ZAC93_DEVICES) {
        f->overflow++;
        return false;
    }
    d->zac = f;
    f->devices[f->n_devices++] = d;
    return true;
}

bool fake_zac93_press(fake_zac93_t *f, uint64_t now, fake_zdev_t *d)
{
    if (f->add_state != ADD_LISTENING) {
        return false;
    }
    const uint16_t id = f->join_node_id != 0u ? f->join_node_id : f->next_node++;
    const uint8_t seq[3] = {ZW_ADD_NODE_STATUS_NODE_FOUND,
                            d->controller ? ZW_ADD_NODE_STATUS_ADDING_CONTROLLER
                                          : ZW_ADD_NODE_STATUS_ADDING_END_NODE,
                            ZW_ADD_NODE_STATUS_PROTOCOL_DONE};
    uint64_t t = now;
    for (size_t i = 0; i < 3u; i++) {
        t += f->join_step_ms;
        plan_add_status(f, t, f->add_cb, seq[i], i == 0u ? 0u : id, d);
        if (seq[i] == f->join_fail_after) {
            plan_add_status(f, t + f->join_step_ms, f->add_cb, ZW_ADD_NODE_STATUS_FAILED, 0u,
                            NULL);
            break;
        }
        if (seq[i] == f->join_stall_after) {
            break;
        }
    }
    f->add_state = ADD_JOINING;
    f->joining = d;
    f->join_id = id;
    zw_node_list_set(f->id.node_list, id, true);
    d->now = now;
    fake_zdev_joined(d, f->id.home_id, id);
    return true;
}

bool fake_zac93_press_remove(fake_zac93_t *f, uint64_t now, fake_zdev_t *d)
{
    if (f->remove_state != 1u) {
        return false;
    }
    const bool ours = d->status != FAKE_ZDEV_OUT && d->home_id == f->id.home_id && d->node != 0u &&
                      zw_node_list_test(f->id.node_list, d->node);
    const uint16_t node = ours ? d->node : 0u; /* 0: another network's device (reset anyway) */
    plan_remove_status(f, now + f->join_step_ms, ZW_REMOVE_NODE_STATUS_NODE_FOUND, 0u);
    plan_remove_status(f, now + 2u * f->join_step_ms, ZW_REMOVE_NODE_STATUS_REMOVING_END_NODE,
                       node);
    plan_remove_status(f, now + 3u * f->join_step_ms, ZW_REMOVE_NODE_STATUS_DONE, node);
    f->remove_state = 2u;
    if (node != 0u) {
        zw_node_list_set(f->id.node_list, node, false);
        zw_node_list_set(f->failed, node, false);
    }
    d->status = FAKE_ZDEV_OUT;
    d->home_id = 0u;
    d->have = 0u;
    d->node = 0u;
    return true;
}

bool fake_zac93_smartstart_ask(fake_zac93_t *f, uint64_t now, fake_zdev_t *d)
{
    if (!f->smart_listen || d->status != FAKE_ZDEV_OUT) {
        return false;
    }
    zw_api_smartstart_join_t j;
    memset(&j, 0, sizeof j);
    uint8_t auth[4];
    zw_dsk_home_ids(d->pub, j.nwi_home_id, auth);
    j.basic_class = d->basic;
    j.generic_class = d->generic;
    j.specific_class = d->specific;
    j.cc_list = d->nif;
    j.cc_list_len = d->nif_len;
    uint8_t data[ZW_PAYLOAD_MAX];
    uint8_t out[ZW_PAYLOAD_MAX];
    const zw_api_app_update_t u = {ZW_APP_UPDATE_SMARTSTART_CLASSIC, data,
                                   zw_api_smartstart_join_build(data, sizeof data, module_nid(f),
                                                                &j)};
    const size_t n = zw_api_app_update_build(out, sizeof out, &u);
    if (u.data_len == 0u || n == 0u) {
        f->overflow++;
        return false;
    }
    f->smart_asks++;
    d->now = now;
    return plan_frame(f, now + f->reply_ms, ZW_TYPE_REQUEST, (uint8_t)ZW_FUNC_APPLICATION_UPDATE,
                      out, n);
}

void fake_zac93_node_info(fake_zac93_t *f, uint64_t now, const fake_zdev_t *d)
{
    uint8_t data[ZW_PAYLOAD_MAX];
    uint8_t out[ZW_PAYLOAD_MAX];
    const zw_api_node_update_t nu = {d->node, d->basic, d->generic, d->specific, d->nif,
                                     d->nif_len};
    const zw_api_app_update_t u = {ZW_APP_UPDATE_NODE_INFO_RECEIVED, data,
                                   zw_api_node_update_build(data, sizeof data, module_nid(f), &nu)};
    const size_t n = zw_api_app_update_build(out, sizeof out, &u);
    if (n == 0u) {
        f->overflow++;
        return;
    }
    (void)plan_frame(f, now + f->reply_ms, ZW_TYPE_REQUEST,
                     (uint8_t)ZW_FUNC_APPLICATION_UPDATE, out, n);
}

void fake_zac93_join(fake_zac93_t *f, fake_zdev_t *d, uint16_t node)
{
    zw_node_list_set(f->id.node_list, node, true);
    if (node >= f->next_node) {
        f->next_node = (uint16_t)(node + 1u);
    }
    fake_zdev_joined(d, f->id.home_id, node);
}

void fake_zac93_from_device(fake_zac93_t *f, uint64_t now, const fake_zdev_t *d,
                            const uint8_t *cmd, size_t len)
{
    fake_zac93_from_node_as(f, now, d->node, f->rx_status, cmd, len);
}

void fake_zac93_from_node_as(fake_zac93_t *f, uint64_t now, uint16_t node, uint8_t rx_status,
                             const uint8_t *cmd, size_t len)
{
    const zw_api_app_cmd_t c = {false, rx_status, 0u, node, cmd, len, NULL, 0u, true, f->rx_rssi};
    uint8_t p[ZW_PAYLOAD_MAX];
    const size_t n = zw_api_app_cmd_build(p, sizeof p, module_nid(f), &c);
    if (n == 0u) {
        f->overflow++;
        return;
    }
    (void)plan_frame(f, now + f->reply_ms, ZW_TYPE_REQUEST,
                     (uint8_t)ZW_FUNC_APPLICATION_COMMAND_HANDLER, p, n);
}

size_t fake_zac93_take(fake_zac93_t *f, uint8_t *out, size_t cap)
{
    const size_t n = f->out_len < cap ? f->out_len : cap;
    memcpy(out, f->out, n);
    memmove(f->out, &f->out[n], f->out_len - n);
    f->out_len -= n;
    return n;
}

/* ---- The wire ----------------------------------------------------------------------------- */

void fake_wire_init(fake_wire_t *w, fake_zac93_t *zac, const fake_host_t *host, uint64_t now_ms)
{
    memset(w, 0, sizeof *w);
    w->zac = zac;
    w->host = *host;
    zw_rx_init(&w->rx);
    w->now = now_ms;
}

static bool wire_append(fake_wire_t *w, const uint8_t *bytes, size_t len)
{
    if (len > sizeof w->to_zac - w->to_zac_len) {
        w->stuck = true;
        return false;
    }
    if (w->tap.bytes != NULL) {
        w->tap.bytes(w->tap.ctx, true, w->now, bytes, len);
    }
    memcpy(&w->to_zac[w->to_zac_len], bytes, len);
    w->to_zac_len += len;
    return true;
}

bool fake_wire_write(fake_wire_t *w, const uint8_t *bytes, size_t len)
{
    w->written += (uint32_t)len;
    return wire_append(w, bytes, len);
}

/* zw_link's own bytes: the ACK or NAK for a frame it just heard. */
static void link_answer(fake_wire_t *w, uint8_t byte)
{
    (void)wire_append(w, &byte, 1u);
}

/* The host's (and zw_link's) bytes reach the module. */
static bool deliver(fake_wire_t *w)
{
    if (w->to_zac_len == 0u) {
        return false;
    }
    uint8_t bytes[FAKE_WIRE_BYTES];
    const size_t n = w->to_zac_len;
    memcpy(bytes, w->to_zac, n);
    w->to_zac_len = 0u;
    fake_zac93_receive(w->zac, w->now, bytes, n);
    return true;
}

/* The module's bytes through zw_link: ACK or NAK at once, then the host hears about it; bytes
 * the parser discards while idle reach the host as text, in runs, in their place in the
 * stream. */
static bool drain(fake_wire_t *w)
{
    uint8_t buf[512], text[512];
    bool any = false;
    size_t n;
    while ((n = fake_zac93_take(w->zac, buf, sizeof buf)) > 0u) {
        size_t tn = 0u;
        any = true;
        if (w->tap.bytes != NULL) {
            w->tap.bytes(w->tap.ctx, false, w->now, buf, n);
        }
        for (size_t i = 0; i < n; i++) {
            const uint32_t discarded = w->rx.discarded;
            const zw_rx_event_t ev = zw_rx_feed(&w->rx, buf[i], w->now);
            if (w->rx.discarded != discarded) {
                text[tn++] = buf[i];
                continue;
            }
            if (tn > 0u && w->host.text != NULL) {
                w->host.text(w->host.ctx, w->now, text, tn);
            }
            tn = 0u;
            switch (ev) {
            case ZW_RX_ACK:
            case ZW_RX_NAK:
            case ZW_RX_CAN:
                w->host.control(w->host.ctx, w->now, buf[i]);
                break;
            case ZW_RX_FRAME: {
                zw_frame_t fr;
                if (zw_rx_frame(&w->rx, &fr)) {
                    link_answer(w, ZW_ACK);
                    w->acks++;
                    w->host.frame(w->host.ctx, w->now, &fr);
                }
                break;
            }
            case ZW_RX_BAD_FRAME:
                link_answer(w, ZW_NAK);
                w->naks++;
                break;
            default:
                break;
            }
        }
        if (tn > 0u && w->host.text != NULL) {
            w->host.text(w->host.ctx, w->now, text, tn);
        }
    }
    return any;
}

void fake_wire_settle(fake_wire_t *w)
{
    for (unsigned guard = 0; guard < 100000u; guard++) {
        w->host.poll(w->host.ctx, w->now);
        bool moved = deliver(w);
        fake_zac93_poll(w->zac, w->now);
        moved = drain(w) || moved;
        if (!moved) {
            return;
        }
    }
    w->stuck = true;
}

static uint64_t wire_deadline(const fake_wire_t *w)
{
    const uint64_t h = w->host.next_deadline(w->host.ctx);
    const uint64_t m = fake_zac93_next_deadline(w->zac);
    return h < m ? h : m;
}

void fake_wire_run_until(fake_wire_t *w, uint64_t t_ms)
{
    fake_wire_settle(w);
    for (unsigned guard = 0; guard < 1000000u && !w->stuck; guard++) {
        const uint64_t d = wire_deadline(w);
        if (d > t_ms) {
            break;
        }
        if (d <= w->now) {
            /* due now, yet the last settle left it: once more, then call it stuck */
            fake_wire_settle(w);
            if (wire_deadline(w) <= w->now) {
                w->stuck = true;
            }
            continue;
        }
        w->now = d;
        fake_wire_settle(w);
    }
    if (!w->stuck && t_ms > w->now) {
        w->now = t_ms;
        fake_wire_settle(w);
    }
}
