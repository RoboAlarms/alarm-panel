/*
 * zw_api.h - the Z-Wave Serial API's commands, values and frame payloads (docs/features/36,
 * serial-api.md §2-§6): function IDs, the §3 value tables and list helpers, and a build/parse
 * pair for every phase-1 message's payload (plus DA GetLongRangeNodes and the SerialAPISetup
 * sub-commands the start-up sequence uses). Phase-2 and phase-3 messages (§6.22, §6.23 and the
 * commands serial-api.md marks phase 2) get only their zw_func_t value here; their payloads
 * arrive in a later milestone.
 *
 * How to use:
 *   - A codec works on the frame PAYLOAD only: the bytes zw_rx_frame() hands back as
 *     zw_frame_t.payload, or the bytes zw_frame_build() wants as its own payload argument. FUNC
 *     and TYPE (request 00 / response 01) are chosen by the caller from zw_func_t and are not
 *     part of what these functions read or write.
 *   - `zw_api_<msg>_parse(p, len, [w,] out)` never reads past `len` and returns false on
 *     anything malformed; `zw_api_<msg>_build(out, cap, [w,] in)` returns the payload length
 *     written, or 0 when it does not fit or an input is invalid. `<msg>` is `<command>_req` /
 *     `_rsp` / `_cb`; a frame the module sends unprompted uses its own name alone (`app_cmd`,
 *     `app_update`, `started`); AddNode/RemoveNode's status reports are `_cb`. A message with an
 *     empty payload (SoftReset's request, SendDataAbort, every plain "no payload" request) has
 *     no codec: build it with zw_frame_build(..., NULL, 0).
 *   - `w` (zw_nid_width_t) is the node-ID width currently in effect (SerialAPISetup
 *     SetNodeIDType, §6.7): every field serial-api.md marks `nid` is 1 or 2 bytes accordingly.
 *     Node IDs are always handed back as uint16_t regardless of wire width.
 *   - Variable-length parts (node lists, the function mask, CC lists, command bytes, the
 *     GetControllerVersion library string, a SerialAPISetup sub-command's own data) are views:
 *     a pointer into the payload just parsed, or a caller-owned pointer to splice into a payload
 *     being built. No copies, no heap; a parsed view is valid only as long as the buffer it
 *     points into. Small fixed-size fields (home IDs, TX report sub-fields) are copied by value.
 *   - Trailing bytes: every parser for what the panel *hears* from the module (every `_rsp`,
 *     `_cb`, `app_cmd`, `app_update`, `started`, and the shared shapes they use - node_update,
 *     smartstart_join, tx_report) accepts extra bytes after its own content and ignores them, so
 *     newer module firmware that appends fields cannot break the panel. Minimum lengths, internal
 *     length fields (one that says how many more bytes follow must still fit inside what was
 *     given) and values that pick a payload shape stay strict. The `_req` parsers - only a fake
 *     module uses them, to check exactly what the host sent - stay strict on length throughout.
 *     `zw_api_node_id_parse()` itself only enforces the minimum (`len >= w`); a `_req` wrapper
 *     that is nothing but a node ID adds its own exact-length check on top.
 *   - These names are the contract components/core/zwave_host codes its transport and start-up
 *     sequence against: stable once written.
 *
 * Portable C11, no heap.
 */
#ifndef ZW_API_H
#define ZW_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================================
 * Function IDs (serial-api.md §6's table, phases 1-3; values only). Phase per README "Phases":
 * everything here is phase 1 unless commented otherwise. Phase-2/3 functions carry no codec yet.
 * ================================================================================================ */
typedef enum {
    ZW_FUNC_GET_SERIAL_API_INIT_DATA          = 0x02u,
    ZW_FUNC_SET_APPLICATION_NODE_INFORMATION  = 0x03u,
    ZW_FUNC_APPLICATION_COMMAND_HANDLER       = 0x04u, /* from the module */
    ZW_FUNC_GET_CONTROLLER_CAPABILITIES       = 0x05u,
    ZW_FUNC_SET_SERIAL_API_TIMEOUTS           = 0x06u,
    ZW_FUNC_GET_SERIAL_API_CAPABILITIES       = 0x07u,
    ZW_FUNC_SOFT_RESET                        = 0x08u,
    ZW_FUNC_GET_PROTOCOL_VERSION              = 0x09u,
    ZW_FUNC_SERIAL_API_STARTED                = 0x0Au, /* from the module */
    ZW_FUNC_SERIAL_API_SETUP                  = 0x0Bu,
    ZW_FUNC_SET_RF_RECEIVE_MODE               = 0x10u, /* phase 3 */
    ZW_FUNC_SEND_DATA                         = 0x13u,
    ZW_FUNC_GET_CONTROLLER_VERSION            = 0x15u,
    ZW_FUNC_SEND_DATA_ABORT                   = 0x16u,
    ZW_FUNC_GET_CONTROLLER_ID                 = 0x20u,
    ZW_FUNC_ENTER_BOOTLOADER                  = 0x27u, /* phase 3, §6.23 */
    ZW_FUNC_NVM_OPERATIONS                    = 0x2Eu, /* phase 3, §6.22 */
    ZW_FUNC_GET_BACKGROUND_RSSI               = 0x3Bu, /* phase 2 */
    ZW_FUNC_EXTENDED_NVM_OPERATIONS           = 0x3Du, /* phase 3, §6.22 */
    ZW_FUNC_GET_NODE_PROTOCOL_INFO            = 0x41u,
    ZW_FUNC_HARD_RESET                        = 0x42u,
    ZW_FUNC_ASSIGN_RETURN_ROUTE               = 0x46u, /* phase 2 */
    ZW_FUNC_DELETE_RETURN_ROUTE               = 0x47u, /* phase 2 */
    ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE      = 0x48u, /* phase 2 */
    ZW_FUNC_APPLICATION_UPDATE                = 0x49u, /* from the module */
    ZW_FUNC_ADD_NODE_TO_NETWORK               = 0x4Au,
    ZW_FUNC_REMOVE_NODE_FROM_NETWORK          = 0x4Bu,
    ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE           = 0x51u,
    ZW_FUNC_SET_SUC_NODE_ID                   = 0x54u,
    ZW_FUNC_DELETE_SUC_RETURN_ROUTE           = 0x55u, /* phase 2 */
    ZW_FUNC_GET_SUC_NODE_ID                   = 0x56u,
    ZW_FUNC_REQUEST_NODE_INFO                 = 0x60u,
    ZW_FUNC_REMOVE_FAILED_NODE                = 0x61u, /* phase 2 */
    ZW_FUNC_IS_FAILED_NODE                    = 0x62u, /* phase 2 */
    ZW_FUNC_REPLACE_FAILED_NODE               = 0x63u, /* phase 2 */
    ZW_FUNC_GET_ROUTING_INFO                  = 0x80u, /* phase 2 */
    ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER = 0xA8u, /* from the module */
    ZW_FUNC_SHUTDOWN                          = 0xD9u, /* phase 3 */
    ZW_FUNC_GET_LONG_RANGE_NODES              = 0xDAu, /* used at start-up (§5 step 13) */
    ZW_FUNC_GET_LONG_RANGE_CHANNEL            = 0xDBu, /* phase 3 */
    ZW_FUNC_SET_LONG_RANGE_CHANNEL            = 0xDCu, /* phase 3 */
} zw_func_t;

/* ================================================================================================
 * Node IDs: 1 byte, or 2 after SerialAPISetup SetNodeIDType(16-bit) (Long Range). A soft reset
 * always returns to 8-bit (§5 step 14). Always handed back as uint16_t.
 * ================================================================================================ */
typedef enum {
    ZW_NID_8 = 1,
    ZW_NID_16 = 2,
} zw_nid_width_t;

/* Writes a node ID as 'w' bytes, big-endian. Returns 0 (writes nothing) when 'cap' is too small
 * or node_id does not fit in 8 bits while w is ZW_NID_8. */
size_t zw_api_node_id_build(uint8_t *out, size_t cap, zw_nid_width_t w, uint16_t node_id);
/* A building block, not a whole message: only the minimum is enforced ('len' >= 'w'; anything
 * past the first 'w' bytes is left for the caller to decide about). A message that is nothing
 * but a node ID adds its own exact-length check when it must stay strict (a `_req`). */
bool zw_api_node_id_parse(const uint8_t *p, size_t len, zw_nid_width_t w, uint16_t *node_id);

/* ================================================================================================
 * §3. Values used throughout.
 * ================================================================================================ */

/* ---- node lists (bit masks) --------------------------------------------------------------- */

/* Classic node list: 29 bytes, nodes 1..232 (bit n of byte b = node 8b+n+1). */
#define ZW_NODE_LIST_BYTES 29u
bool zw_node_list_test(const uint8_t list[ZW_NODE_LIST_BYTES], uint16_t node_id);
void zw_node_list_set(uint8_t list[ZW_NODE_LIST_BYTES], uint16_t node_id, bool present);
/* Every present node, ascending, into 'out' (up to 'max'); returns how many are actually
 * present (which may exceed 'max' - the count is not clamped, only what is written is). */
size_t zw_node_list_iterate(const uint8_t list[ZW_NODE_LIST_BYTES], uint16_t *out, size_t max);

/* Long Range: segments of 16 bytes (128 nodes); segment s starts at node 256+128*s (network.md
 * "Long Range"). A segment alone does not know its own index, so every call is given one. */
#define ZW_LR_SEGMENT_BYTES 16u
bool zw_lr_segment_test(const uint8_t seg[ZW_LR_SEGMENT_BYTES], unsigned seg_index, uint16_t node_id);
void zw_lr_segment_set(uint8_t seg[ZW_LR_SEGMENT_BYTES], unsigned seg_index, uint16_t node_id, bool present);
size_t zw_lr_segment_iterate(const uint8_t seg[ZW_LR_SEGMENT_BYTES], unsigned seg_index,
                              uint16_t *out, size_t max);

/* Function mask (capabilities, §6.1): 32 bytes; bit n of byte b = function 8b+n+1. The panel only
 * ever reads one the module sent, so only a test is offered. */
#define ZW_FUNC_MASK_BYTES 32u
bool zw_func_mask_test(const uint8_t mask[ZW_FUNC_MASK_BYTES], uint8_t func);

/* ---- command-class lists ------------------------------------------------------------------- */

/* A list of CC identifiers: a lead byte >= F1 opens a two-byte identifier ((lead<<8)|next); every
 * other byte is a one-byte identifier. EF ("support/control mark", < F1 so never a two-byte lead)
 * separates supported CCs (before it) from controlled ones (after). Iterate with
 * zw_cc_iter_init() then zw_cc_iter_next() until it returns false; 'controlled' distinguishes a
 * clean end from a two-byte identifier truncated by the list's own length (pos < end). */
typedef struct {
    const uint8_t *pos;
    const uint8_t *end;
    bool controlled;
} zw_cc_iter_t;

void zw_cc_iter_init(zw_cc_iter_t *it, const uint8_t *list, size_t len);
bool zw_cc_iter_next(zw_cc_iter_t *it, uint16_t *cc_id, bool *controlled);
/* True when the list parses to its own end with no truncated two-byte identifier. */
bool zw_cc_list_is_well_formed(const uint8_t *list, size_t len);

/* ---- transmit options (SendData, §3) -------------------------------------------------------- */
#define ZW_TX_OPT_ACK        0x01u
#define ZW_TX_OPT_LOW_POWER  0x02u
#define ZW_TX_OPT_AUTO_ROUTE 0x04u
#define ZW_TX_OPT_NO_ROUTE   0x10u
#define ZW_TX_OPT_EXPLORE    0x20u
/* ACK | AutoRoute | Explore: every SendData except a nonce report. */
#define ZW_TX_OPT_DEFAULT (ZW_TX_OPT_ACK | ZW_TX_OPT_AUTO_ROUTE | ZW_TX_OPT_EXPLORE)
/* ACK | AutoRoute, no Explore: nonce reports (security.md §6). */
#define ZW_TX_OPT_NONCE_REPORT (ZW_TX_OPT_ACK | ZW_TX_OPT_AUTO_ROUTE)

/* ---- transmit status (SendData and route callbacks, §3) ------------------------------------- */
typedef enum {
    ZW_TX_STATUS_OK        = 0x00u,
    ZW_TX_STATUS_NO_ACK    = 0x01u,
    ZW_TX_STATUS_FAIL      = 0x02u,
    ZW_TX_STATUS_NOT_IDLE  = 0x03u,
    ZW_TX_STATUS_NO_ROUTE  = 0x04u,
} zw_tx_status_t;

/* ---- RSSI (§3): signed dBm, with three reserved "not a reading" values ----------------------- */
#define ZW_RSSI_NOT_AVAILABLE ((int8_t)0x7F)
#define ZW_RSSI_SATURATED     ((int8_t)0x7E)
#define ZW_RSSI_NO_SIGNAL     ((int8_t)0x7D)

/* ================================================================================================
 * SendData's TX report (§6.12): present when SerialAPISetup SetTxStatusReport is on, but a
 * SendData callback is valid with or without one - never fail the callback over its report.
 * Tolerant by length, not by an exact set of sizes: below 17 bytes there is no report at all (the
 * caller does not even call zw_api_tx_report_parse() - see zw_api_send_data_cb_parse()); 17 or
 * more gives the base fields; 18 or more adds the last working node; 19 or more the first failing
 * node; 24 or more the five Long Range fields. Anything past 24 bytes is ignored, so newer
 * firmware appending fields cannot break this parser.
 * ================================================================================================ */
typedef struct {
    uint16_t transmit_ticks;            /* 10 ms units; 0 with status Fail = never transmitted */
    uint8_t repeater_count;
    uint8_t ack_rssi;                   /* meaningful only when the node acknowledged */
    uint8_t ack_rssi_per_repeater[4];
    uint8_t ack_channel;
    uint8_t tx_channel;                 /* >= 3 = Long Range */
    uint8_t route_scheme_state;
    uint8_t repeater_node_ids[4];
    uint8_t beam_and_route_speed;       /* bit 6 1000 ms beam, bit 5 250 ms beam, bits 2..0 speed */
    uint8_t routing_attempts;
    bool has_last_working_node;
    uint8_t last_working_node;          /* of a failed route */
    bool has_first_failing_node;
    uint8_t first_failing_node;
    bool has_long_range;
    int8_t lr_tx_power;
    uint8_t lr_noise_floor;
    int8_t lr_dest_ack_tx_power;
    int8_t lr_dest_ack_rssi;
    uint8_t lr_dest_noise_floor;
} zw_api_tx_report_t;

/* Writes 17, 18, 19 or 24 bytes depending on the has_* flags. Returns 0 when 'cap' is too small
 * or the flags are inconsistent (first_failing_node without last_working_node, or long_range
 * without both). */
size_t zw_api_tx_report_build(uint8_t *out, size_t cap, const zw_api_tx_report_t *in);
/* False only below 17 bytes (see zw_api_tx_report_t's comment) - callers that already treat
 * "too short" as "no report" (zw_api_send_data_cb_parse()) never see that false. Trailing bytes
 * beyond 24 are always accepted and ignored. */
bool zw_api_tx_report_parse(const uint8_t *p, size_t len, zw_api_tx_report_t *out);

/* ================================================================================================
 * A shape ApplicationUpdate types 84/40 share with AddNodeToNetwork's and RemoveNodeFromNetwork's
 * status reports (§6.13, §6.16, §6.17): `node ID (nid) | length L | basic | generic | specific |
 * CC list (L-3)`. This is data the module sends, so parse() only enforces the minimum: 'L' must
 * still fit inside what was given, but bytes past the CC list are accepted and ignored.
 * ================================================================================================ */
typedef struct {
    uint16_t node_id;
    uint8_t basic_class;
    uint8_t generic_class;
    uint8_t specific_class;
    const uint8_t *cc_list;
    size_t cc_list_len;
} zw_api_node_update_t;

size_t zw_api_node_update_build(uint8_t *out, size_t cap, zw_nid_width_t w, const zw_api_node_update_t *in);
bool zw_api_node_update_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_api_node_update_t *out);

/* The SmartStart join shape ApplicationUpdate types 85/86/87 share (§6.13):
 * `node ID (nid, 0) | receive status | NWI home ID (4) | CC length | basic | generic | specific |
 * CC list`. Unlike node_update above, the CC length byte here is the CC list's own length (no
 * -3). Module-sent data: parse() enforces only the minimum length, ignoring anything past it. */
typedef struct {
    uint16_t node_id;            /* 0 for a classic SmartStart request per serial-api.md */
    uint8_t receive_status;
    uint8_t nwi_home_id[4];
    uint8_t basic_class;
    uint8_t generic_class;
    uint8_t specific_class;
    const uint8_t *cc_list;
    size_t cc_list_len;
} zw_api_smartstart_join_t;

size_t zw_api_smartstart_join_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                     const zw_api_smartstart_join_t *in);
bool zw_api_smartstart_join_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                   zw_api_smartstart_join_t *out);

/* ================================================================================================
 * §6.7 `0B` SerialAPISetup: `sub-command | data...` both ways (a response's sub-command 00 means
 * "unsupported", carrying the refused sub-command as data[0]). Each sub-command's own data is a
 * view - its shape is for the caller to interpret per §6.7's table; only the envelope is decoded
 * here.
 * ================================================================================================ */
#define ZW_SETUP_UNSUPPORTED                    0x00u
#define ZW_SETUP_GET_SUPPORTED_COMMANDS         0x01u
#define ZW_SETUP_SET_TX_STATUS_REPORT           0x02u
#define ZW_SETUP_SET_LONG_RANGE_MAXIMUM_TX_POWER 0x03u /* phase 3: power (network.md §12) */
#define ZW_SETUP_SET_POWERLEVEL                 0x04u
#define ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER 0x05u
#define ZW_SETUP_GET_POWERLEVEL                 0x08u
#define ZW_SETUP_GET_MAXIMUM_PAYLOAD_SIZE       0x10u
#define ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE 0x11u
#define ZW_SETUP_SET_POWERLEVEL_16BIT           0x12u
#define ZW_SETUP_GET_POWERLEVEL_16BIT           0x13u
#define ZW_SETUP_GET_SUPPORTED_REGIONS          0x15u
#define ZW_SETUP_GET_REGION_INFO                0x16u
#define ZW_SETUP_GET_RF_REGION                  0x20u
#define ZW_SETUP_SET_RF_REGION                  0x40u
#define ZW_SETUP_SET_NODE_ID_TYPE               0x80u

typedef struct {
    uint8_t sub;
    const uint8_t *data;
    size_t data_len;
} zw_api_api_setup_req_t;
typedef zw_api_api_setup_req_t zw_api_api_setup_rsp_t;

size_t zw_api_api_setup_req_build(uint8_t *out, size_t cap, const zw_api_api_setup_req_t *in);
bool zw_api_api_setup_req_parse(const uint8_t *p, size_t len, zw_api_api_setup_req_t *out);
size_t zw_api_api_setup_rsp_build(uint8_t *out, size_t cap, const zw_api_api_setup_rsp_t *in);
bool zw_api_api_setup_rsp_parse(const uint8_t *p, size_t len, zw_api_api_setup_rsp_t *out);

/* ---- typed helpers for individual sub-commands, on top of the envelope above -------------------
 * Every parse helper checks 'rsp->sub' itself and refuses a response for a different sub-command;
 * every length check is a minimum (module-sent data), so extra trailing bytes are ignored. */

/* Any response: sub-command 00 means "unsupported", carrying the refused sub-command as data[0]. */
bool zw_api_setup_unsupported(const zw_api_api_setup_rsp_t *rsp, uint8_t *refused_sub);

/* `01` GetSupportedCommands: byte 0 is the legacy mask (the bit *value* is the sub-command: 01,
 * 02, 04, 08, 10, 20, 40, 80); an optional extended bit mask follows (bit n of byte b =
 * sub-command 8b+n+1). zw_api_setup_supports() tests either form. */
typedef struct {
    uint8_t legacy_mask;
    const uint8_t *extended_mask; /* NULL when the response is exactly one byte */
    size_t extended_mask_len;
} zw_api_setup_supported_cmds_t;
bool zw_api_setup_supported_cmds_parse(const zw_api_api_setup_rsp_t *rsp,
                                        zw_api_setup_supported_cmds_t *out);
bool zw_api_setup_supports(const zw_api_setup_supported_cmds_t *cmds, uint8_t sub);

/* `15` GetSupportedRegions: a count, then that many region bytes (the count must fit inside what
 * was given; further trailing bytes are ignored). */
typedef struct {
    const uint8_t *regions;
    size_t count;
} zw_api_setup_regions_t;
bool zw_api_setup_regions_parse(const zw_api_api_setup_rsp_t *rsp, zw_api_setup_regions_t *out);

/* `16` GetRegionInfo: region, flags (bit 0 classic Z-Wave, bit 1 Long Range), and - only when a
 * third byte is present - the classic region this one is a superset of. */
typedef struct {
    uint8_t region;
    bool supports_classic;
    bool supports_long_range;
    bool has_classic_superset;
    uint8_t classic_superset_region;
} zw_api_setup_region_info_t;
bool zw_api_setup_region_info_parse(const zw_api_api_setup_rsp_t *rsp, zw_api_setup_region_info_t *out);

/* One-byte results: `02` SetTxStatusReport's status, `10`/`11` a payload size, `20` GetRFRegion's
 * region, `40`/`80` an OK byte. Checks 'rsp->sub' against 'expected_sub' itself. */
bool zw_api_setup_rsp_u8(const zw_api_api_setup_rsp_t *rsp, uint8_t expected_sub, uint8_t *value);

/* Builders for the requests that carry data. */
size_t zw_api_setup_set_tx_status_report_req_build(uint8_t *out, size_t cap, bool on);
size_t zw_api_setup_get_region_info_req_build(uint8_t *out, size_t cap, uint8_t region);
size_t zw_api_setup_set_rf_region_req_build(uint8_t *out, size_t cap, uint8_t region);
size_t zw_api_setup_set_node_id_type_req_build(uint8_t *out, size_t cap, zw_nid_width_t w);

/* §6.7 power (phase 3, network.md §12), in tenths of a dBm: the module's transmit power and its
 * measured output at 0 dBm (calibration), sent back unchanged with a new power. GetPowerlevel
 * (08) and SetPowerlevel (04) carry signed bytes, the 16-bit forms (13, 12) signed big-endian
 * words; the Long Range maximum (05, 03) is one signed big-endian word. */
typedef struct {
    int16_t dbm10;
    int16_t cal_dbm10;
} zw_api_setup_power_t;
/* 08 or 13: false for another sub-command or a short answer. */
bool zw_api_setup_power_parse(const zw_api_api_setup_rsp_t *rsp, zw_api_setup_power_t *out);
/* 05. */
bool zw_api_setup_lr_power_parse(const zw_api_api_setup_rsp_t *rsp, int16_t *dbm10);
/* 12 when `sixteen`, else 04 (0 when a value does not fit a signed byte). */
size_t zw_api_setup_set_power_req_build(uint8_t *out, size_t cap, bool sixteen,
                                        const zw_api_setup_power_t *p);
/* 03. */
size_t zw_api_setup_set_lr_power_req_build(uint8_t *out, size_t cap, int16_t dbm10);

/* ================================================================================================
 * §6.1-§6.10: the start-up commands (GetSerialApiCapabilities through GetControllerId), plus
 * GetNodeProtocolInfo, RequestNodeInfo, HardReset and GetLongRangeNodes.
 * ================================================================================================ */

/* `02` GetSerialApiInitData (§6.3). Request has no payload. */
typedef struct {
    uint8_t serial_api_version;
    bool is_controller;      /* capabilities bit 0 clear */
    bool has_timers;         /* bit 1 */
    bool is_secondary;       /* bit 2 */
    bool is_sis;             /* bit 3 */
    const uint8_t *node_list; /* ZW_NODE_LIST_BYTES bytes, a view into the payload */
    uint8_t chip_type;       /* 08 = 800 series */
    uint8_t chip_version;
} zw_api_init_data_rsp_t;
size_t zw_api_init_data_rsp_build(uint8_t *out, size_t cap, const zw_api_init_data_rsp_t *in);
bool zw_api_init_data_rsp_parse(const uint8_t *p, size_t len, zw_api_init_data_rsp_t *out);

/* `03` SetApplicationNodeInformation (§6.2). No response, no callback. */
typedef struct {
    bool always_listening;   /* device options bit 0 */
    uint8_t generic_class;
    uint8_t specific_class;
    const uint8_t *cc_list;  /* supported only, at most 35 bytes */
    size_t cc_list_len;
} zw_api_app_node_info_req_t;
size_t zw_api_app_node_info_req_build(uint8_t *out, size_t cap, const zw_api_app_node_info_req_t *in);
bool zw_api_app_node_info_req_parse(const uint8_t *p, size_t len, zw_api_app_node_info_req_t *out);

/* `05` GetControllerCapabilities (§6.6). Request has no payload. */
typedef struct {
    bool is_secondary;
    bool on_other_network;
    bool sis_present;
    bool was_real_primary;
    bool is_suc;
    bool no_nodes_included;
} zw_api_controller_caps_rsp_t;
size_t zw_api_controller_caps_rsp_build(uint8_t *out, size_t cap, const zw_api_controller_caps_rsp_t *in);
bool zw_api_controller_caps_rsp_parse(const uint8_t *p, size_t len, zw_api_controller_caps_rsp_t *out);

/* `06` SetSerialApiTimeouts (§6.6): request carries the new values, response the old ones - same
 * shape both ways. Each field is one byte, tens of milliseconds (the panel sends A0 0F: 1600 ms,
 * 150 ms). */
typedef struct {
    uint8_t ack_timeout_x10ms;
    uint8_t byte_timeout_x10ms;
} zw_api_api_timeouts_req_t;
typedef zw_api_api_timeouts_req_t zw_api_api_timeouts_rsp_t;
size_t zw_api_api_timeouts_req_build(uint8_t *out, size_t cap, const zw_api_api_timeouts_req_t *in);
bool zw_api_api_timeouts_req_parse(const uint8_t *p, size_t len, zw_api_api_timeouts_req_t *out);
size_t zw_api_api_timeouts_rsp_build(uint8_t *out, size_t cap, const zw_api_api_timeouts_rsp_t *in);
bool zw_api_api_timeouts_rsp_parse(const uint8_t *p, size_t len, zw_api_api_timeouts_rsp_t *out);

/* `07` GetSerialApiCapabilities (§6.1). Request has no payload. */
typedef struct {
    uint8_t app_version_major;
    uint8_t app_version_minor;
    uint16_t manufacturer_id;
    uint16_t product_type;
    uint16_t product_id;
    const uint8_t *function_mask; /* ZW_FUNC_MASK_BYTES bytes, a view into the payload */
} zw_api_api_caps_rsp_t;
size_t zw_api_api_caps_rsp_build(uint8_t *out, size_t cap, const zw_api_api_caps_rsp_t *in);
bool zw_api_api_caps_rsp_parse(const uint8_t *p, size_t len, zw_api_api_caps_rsp_t *out);

/* `09` GetProtocolVersion (§6.5). Request has no payload. */
typedef struct {
    uint8_t protocol_type;  /* 00 = Z-Wave */
    uint8_t major, minor, patch;
    uint16_t app_framework_build;
    const uint8_t *git_hash; /* 16 bytes, a view; all-zero = absent */
} zw_api_protocol_version_rsp_t;
size_t zw_api_protocol_version_rsp_build(uint8_t *out, size_t cap, const zw_api_protocol_version_rsp_t *in);
bool zw_api_protocol_version_rsp_parse(const uint8_t *p, size_t len, zw_api_protocol_version_rsp_t *out);

/* `0A` SerialAPIStarted (§6.8): the module's own frame, sent unprompted after any restart.
 * 'has_supported_protocols' is true whenever a byte follows the CC list at all - trailing bytes
 * beyond that one are accepted and ignored, so a firmware that appends more here still parses. */
typedef struct {
    uint8_t wake_up_reason;
    bool watchdog_on;
    bool always_listening;
    uint8_t generic_class;
    uint8_t specific_class;
    const uint8_t *cc_list;
    size_t cc_list_len;
    bool has_supported_protocols;
    uint8_t supported_protocols; /* bit 0 = Long Range available */
} zw_api_started_t;
size_t zw_api_started_build(uint8_t *out, size_t cap, const zw_api_started_t *in);
bool zw_api_started_parse(const uint8_t *p, size_t len, zw_api_started_t *out);

/* `15` GetControllerVersion (§6.4). Request has no payload. 'library' is a view of the library
 * string, NOT including its NUL; 'library_len' is its length. parse() finds the first NUL and
 * reads the byte right after it as the library type; anything past that is accepted and ignored. */
typedef struct {
    const char *library;
    size_t library_len;
    uint8_t library_type; /* 01 static controller, 02 controller, 07 bridge controller */
} zw_api_controller_version_rsp_t;
size_t zw_api_controller_version_rsp_build(uint8_t *out, size_t cap,
                                            const zw_api_controller_version_rsp_t *in);
bool zw_api_controller_version_rsp_parse(const uint8_t *p, size_t len,
                                          zw_api_controller_version_rsp_t *out);

/* `20` GetControllerId (§6.9). Request has no payload. */
typedef struct {
    uint8_t home_id[4];
    uint16_t own_node_id;
} zw_api_controller_id_rsp_t;
size_t zw_api_controller_id_rsp_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                       const zw_api_controller_id_rsp_t *in);
bool zw_api_controller_id_rsp_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                     zw_api_controller_id_rsp_t *out);

/* `56` GetSUCNodeId (§6.9). Request has no payload. */
typedef struct {
    uint16_t node_id; /* 0 = none */
} zw_api_get_suc_node_id_rsp_t;
size_t zw_api_get_suc_node_id_rsp_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                         const zw_api_get_suc_node_id_rsp_t *in);
bool zw_api_get_suc_node_id_rsp_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                       zw_api_get_suc_node_id_rsp_t *out);

/* `54` SetSUCNodeId (§6.9). */
#define ZW_SUC_STATUS_SUCCEEDED 0x05u
#define ZW_SUC_STATUS_FAILED    0x06u
typedef struct {
    uint16_t node_id;
    bool enable_suc;
    uint8_t transmit_options;
    bool is_sis;         /* capabilities 01 = SIS */
    uint8_t callback_id;
} zw_api_set_suc_node_id_req_t;
typedef struct {
    bool started;
} zw_api_set_suc_node_id_rsp_t;
typedef struct {
    uint8_t callback_id;
    uint8_t status; /* ZW_SUC_STATUS_* */
} zw_api_set_suc_node_id_cb_t;
size_t zw_api_set_suc_node_id_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                         const zw_api_set_suc_node_id_req_t *in);
bool zw_api_set_suc_node_id_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                       zw_api_set_suc_node_id_req_t *out);
size_t zw_api_set_suc_node_id_rsp_build(uint8_t *out, size_t cap, const zw_api_set_suc_node_id_rsp_t *in);
bool zw_api_set_suc_node_id_rsp_parse(const uint8_t *p, size_t len, zw_api_set_suc_node_id_rsp_t *out);
size_t zw_api_set_suc_node_id_cb_build(uint8_t *out, size_t cap, const zw_api_set_suc_node_id_cb_t *in);
bool zw_api_set_suc_node_id_cb_parse(const uint8_t *p, size_t len, zw_api_set_suc_node_id_cb_t *out);

/* `51` AssignSUCReturnRoute (§6.15). */
typedef struct {
    uint16_t node_id;
    uint8_t callback_id;
} zw_api_assign_suc_return_route_req_t;
typedef struct {
    bool started;
} zw_api_assign_suc_return_route_rsp_t;
typedef struct {
    uint8_t callback_id;
    uint8_t transmit_status; /* zw_tx_status_t */
} zw_api_assign_suc_return_route_cb_t;
size_t zw_api_assign_suc_return_route_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                                 const zw_api_assign_suc_return_route_req_t *in);
bool zw_api_assign_suc_return_route_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                               zw_api_assign_suc_return_route_req_t *out);
size_t zw_api_assign_suc_return_route_rsp_build(uint8_t *out, size_t cap,
                                                 const zw_api_assign_suc_return_route_rsp_t *in);
bool zw_api_assign_suc_return_route_rsp_parse(const uint8_t *p, size_t len,
                                               zw_api_assign_suc_return_route_rsp_t *out);
size_t zw_api_assign_suc_return_route_cb_build(uint8_t *out, size_t cap,
                                                const zw_api_assign_suc_return_route_cb_t *in);
bool zw_api_assign_suc_return_route_cb_parse(const uint8_t *p, size_t len,
                                              zw_api_assign_suc_return_route_cb_t *out);

/* `47` DeleteReturnRoute and `48` RequestNodeNeighborUpdate (§6.15, §6.19; route repair) take
 * `51`'s request (`node ID (nid) | callback ID`: zw_api_assign_suc_return_route_req_build) and
 * send its two-byte callback (`callback ID | status`: ..._cb_parse). 47's status is a transmit
 * status (and it answers a response first, as 51); 48 answers no response, and its status is one
 * of these - STARTED an intermediate one. */
#define ZW_NEIGHBOR_UPDATE_STARTED 0x21u
#define ZW_NEIGHBOR_UPDATE_DONE    0x22u
#define ZW_NEIGHBOR_UPDATE_FAILED  0x23u

/* `41` GetNodeProtocolInfo (§6.10). */
typedef struct {
    uint16_t node_id;
} zw_api_node_protocol_info_req_t;
size_t zw_api_node_protocol_info_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                            const zw_api_node_protocol_info_req_t *in);
bool zw_api_node_protocol_info_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                          zw_api_node_protocol_info_req_t *out);

#define ZW_FLIRS_NONE    0u
#define ZW_FLIRS_250MS   1u
#define ZW_FLIRS_1000MS  2u

/* Response is at least 5 bytes, no node ID echoed; byte 5 (specific_device_class) is required only
 * when has_specific_device_class is set (§6.10's own table) - that flag bit, not the length,
 * decides whether byte 5 is read; bytes past it are accepted and ignored either way. "node type"
 * per this document's rule (§6.10's note on bits 3 and 1): bit 1 alone decides it. */
typedef struct {
    bool always_listening;
    bool routing;
    bool speed_40k;
    bool speed_9_6k;
    uint8_t protocol_version;       /* byte 0 bits 2..0 */
    bool optional_functionality;
    uint8_t flirs;                  /* ZW_FLIRS_* */
    bool supports_beaming;
    bool is_controller_type;        /* byte 1 bit 1 */
    bool has_specific_device_class; /* byte 1 bit 2 */
    bool supports_s0;               /* byte 1 bit 0, legacy flag */
    bool speed_100k_classic;        /* byte 2 bit 0 */
    bool speed_100k_long_range;     /* byte 2 bit 1 */
    uint8_t basic_device_class;
    uint8_t generic_device_class;
    uint8_t specific_device_class;  /* valid only when has_specific_device_class */
} zw_api_node_protocol_info_rsp_t;
size_t zw_api_node_protocol_info_rsp_build(uint8_t *out, size_t cap,
                                            const zw_api_node_protocol_info_rsp_t *in);
bool zw_api_node_protocol_info_rsp_parse(const uint8_t *p, size_t len,
                                          zw_api_node_protocol_info_rsp_t *out);

/* `42` HardReset (§6.14): erases the module's network. Request and callback are both a bare
 * callback ID. */
typedef struct {
    uint8_t callback_id;
} zw_api_hard_reset_req_t;
typedef zw_api_hard_reset_req_t zw_api_hard_reset_cb_t;
size_t zw_api_hard_reset_req_build(uint8_t *out, size_t cap, const zw_api_hard_reset_req_t *in);
bool zw_api_hard_reset_req_parse(const uint8_t *p, size_t len, zw_api_hard_reset_req_t *out);
size_t zw_api_hard_reset_cb_build(uint8_t *out, size_t cap, const zw_api_hard_reset_cb_t *in);
bool zw_api_hard_reset_cb_parse(const uint8_t *p, size_t len, zw_api_hard_reset_cb_t *out);

/* `60` RequestNodeInfo (§6.13). The answer is an ApplicationUpdate frame (zw_api_app_update_t),
 * not a separate callback here. */
typedef struct {
    uint16_t node_id;
} zw_api_request_node_info_req_t;
typedef struct {
    bool sent;
} zw_api_request_node_info_rsp_t;
size_t zw_api_request_node_info_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                           const zw_api_request_node_info_req_t *in);
bool zw_api_request_node_info_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                         zw_api_request_node_info_req_t *out);
size_t zw_api_request_node_info_rsp_build(uint8_t *out, size_t cap,
                                           const zw_api_request_node_info_rsp_t *in);
bool zw_api_request_node_info_rsp_parse(const uint8_t *p, size_t len,
                                         zw_api_request_node_info_rsp_t *out);

/* `DA` GetLongRangeNodes (§6.21; used at start-up, §5 step 13, though §6's table files it under
 * phase 3). */
typedef struct {
    uint8_t segment;
} zw_api_lr_nodes_req_t;
typedef struct {
    bool more_segments;
    uint8_t segment;
    const uint8_t *node_list; /* ZW_LR_SEGMENT_BYTES bytes, a view into the payload */
} zw_api_lr_nodes_rsp_t;
size_t zw_api_lr_nodes_req_build(uint8_t *out, size_t cap, const zw_api_lr_nodes_req_t *in);
bool zw_api_lr_nodes_req_parse(const uint8_t *p, size_t len, zw_api_lr_nodes_req_t *out);
size_t zw_api_lr_nodes_rsp_build(uint8_t *out, size_t cap, const zw_api_lr_nodes_rsp_t *in);
bool zw_api_lr_nodes_rsp_parse(const uint8_t *p, size_t len, zw_api_lr_nodes_rsp_t *out);

/* ================================================================================================
 * Network traffic and membership: ApplicationCommandHandler / BridgeApplicationCommandHandler,
 * ApplicationUpdate, AddNodeToNetwork, RemoveNodeFromNetwork, SendData.
 * ================================================================================================ */

/* `04` ApplicationCommandHandler / `A8` BridgeApplicationCommandHandler (§6.11): the module's own
 * frame carrying a command a node sent. 'is_bridge' says which shape - set it before build();
 * parse() is told which shape to expect via 'func' (the frame's own FUNC, which the caller
 * already has from zw_frame_t). destination_node_id and the multicast mask apply to the bridge
 * shape only (zero / NULL otherwise). 'has_rssi' is true whenever a byte follows the mandatory
 * part at all - trailing bytes beyond that one are accepted and ignored. */
typedef struct {
    bool is_bridge;
    uint8_t receive_status;
    uint16_t destination_node_id; /* bridge only */
    uint16_t source_node_id;
    const uint8_t *command;
    size_t command_len;
    const uint8_t *multicast_mask; /* bridge only; NULL when there is none */
    size_t multicast_mask_len;
    bool has_rssi;
    int8_t rssi;
} zw_api_app_cmd_t;
size_t zw_api_app_cmd_build(uint8_t *out, size_t cap, zw_nid_width_t w, const zw_api_app_cmd_t *in);
bool zw_api_app_cmd_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_func_t func,
                           zw_api_app_cmd_t *out);

/* `49` ApplicationUpdate (§6.13): the module's own frame. 'data' is everything after the type
 * byte; decode it further with zw_api_node_update_parse() (types 84, 40),
 * zw_api_smartstart_join_parse() (types 85, 86, 87) or zw_api_node_id_parse() (types 20, 10) as
 * 'type' calls for - types 82, 81, 80 carry no data. */
#define ZW_APP_UPDATE_NODE_INFO_RECEIVED       0x84u
#define ZW_APP_UPDATE_NODE_INFO_REQUEST_DONE   0x82u
#define ZW_APP_UPDATE_NODE_INFO_REQUEST_FAILED 0x81u
#define ZW_APP_UPDATE_ROUTING_PENDING          0x80u
#define ZW_APP_UPDATE_NODE_ADDED_BY_OTHER      0x40u
#define ZW_APP_UPDATE_NODE_REMOVED_BY_OTHER    0x20u
#define ZW_APP_UPDATE_SUC_ID_CHANGED           0x10u
#define ZW_APP_UPDATE_SMARTSTART_CLASSIC       0x85u
#define ZW_APP_UPDATE_SMARTSTART_LONG_RANGE    0x87u
#define ZW_APP_UPDATE_SMARTSTART_POWERED_UP    0x86u
typedef struct {
    uint8_t type;
    const uint8_t *data;
    size_t data_len;
} zw_api_app_update_t;
size_t zw_api_app_update_build(uint8_t *out, size_t cap, const zw_api_app_update_t *in);
bool zw_api_app_update_parse(const uint8_t *p, size_t len, zw_api_app_update_t *out);

/* `4A` AddNodeToNetwork (§6.16). 'mode' packs a type (low nibble, ZW_ADD_NODE_TYPE_*) and flags
 * (high nibble, ZW_ADD_NODE_FLAG_*); the panel always sends high power + network-wide. */
#define ZW_ADD_NODE_TYPE_MASK             0x0Fu
#define ZW_ADD_NODE_TYPE_ANY              0x01u
#define ZW_ADD_NODE_TYPE_STOP             0x05u
#define ZW_ADD_NODE_TYPE_SMARTSTART_DSK   0x08u
#define ZW_ADD_NODE_TYPE_SMARTSTART_LISTEN 0x09u
#define ZW_ADD_NODE_FLAG_HIGH_POWER       0x80u
#define ZW_ADD_NODE_FLAG_NETWORK_WIDE     0x40u
#define ZW_ADD_NODE_FLAG_LONG_RANGE       0x20u

#define ZW_ADD_NODE_STATUS_READY             0x01u
#define ZW_ADD_NODE_STATUS_NODE_FOUND        0x02u
#define ZW_ADD_NODE_STATUS_ADDING_END_NODE   0x03u
#define ZW_ADD_NODE_STATUS_ADDING_CONTROLLER 0x04u
#define ZW_ADD_NODE_STATUS_PROTOCOL_DONE     0x05u
#define ZW_ADD_NODE_STATUS_DONE              0x06u
#define ZW_ADD_NODE_STATUS_FAILED            0x07u

typedef struct {
    uint8_t mode;
    uint8_t callback_id;
    bool has_smartstart_dsk;   /* mode's type is ZW_ADD_NODE_TYPE_SMARTSTART_DSK */
    uint8_t nwi_home_id[4];
    uint8_t auth_home_id[4];
} zw_api_add_node_req_t;

/* AddNode's and RemoveNode's status reports share one wire layout for EVERY status (Silicon
 * Labs' actual Serial API, not serial-api.md's now-corrected "context -" rows):
 * `callback ID | status | source node ID (nid) | L | L bytes of node information (basic,
 * generic, specific, CC list)`. A real "Ready" arrives as `cb 01 00 00`: callback ID, status,
 * node ID 0, L 0. 'node.node_id' is always valid; the rest of 'node' only when has_node_info is
 * true (L > 0, which requires L >= 3: basic, generic, specific, then the CC list).
 * parse() also accepts a report that ends right after the status byte (no node ID or L at all -
 * some firmware, and simple mocks): has_node_info is then false and node.node_id is 0, whatever
 * the status. build() always writes the full layout. */
typedef struct {
    uint8_t callback_id;
    uint8_t status; /* ZW_ADD_NODE_STATUS_* / ZW_REMOVE_NODE_STATUS_* */
    bool has_node_info;
    zw_api_node_update_t node;
} zw_api_add_node_cb_t;
size_t zw_api_add_node_req_build(uint8_t *out, size_t cap, const zw_api_add_node_req_t *in);
bool zw_api_add_node_req_parse(const uint8_t *p, size_t len, zw_api_add_node_req_t *out);
/* AddNode's status 03 (AddingEndNode) and 04 (AddingController) must have node info once
 * anything follows the status byte; every other status leaves it optional. */
size_t zw_api_add_node_cb_build(uint8_t *out, size_t cap, zw_nid_width_t w, const zw_api_add_node_cb_t *in);
bool zw_api_add_node_cb_parse(const uint8_t *p, size_t len, zw_nid_width_t w, zw_api_add_node_cb_t *out);

/* `4B` RemoveNodeFromNetwork (§6.17): same request shape as AddNode, without the SmartStart
 * variant, and the same status-report layout as AddNode's zw_api_add_node_cb_t above - but node
 * info is always optional for every RemoveNode status, including 03/04. */
#define ZW_REMOVE_NODE_TYPE_ANY  0x01u
#define ZW_REMOVE_NODE_TYPE_STOP 0x05u

#define ZW_REMOVE_NODE_STATUS_READY               0x01u
#define ZW_REMOVE_NODE_STATUS_NODE_FOUND          0x02u
#define ZW_REMOVE_NODE_STATUS_REMOVING_END_NODE   0x03u
#define ZW_REMOVE_NODE_STATUS_REMOVING_CONTROLLER 0x04u
#define ZW_REMOVE_NODE_STATUS_RESERVED_STOPPED    0x05u
#define ZW_REMOVE_NODE_STATUS_DONE                0x06u
#define ZW_REMOVE_NODE_STATUS_FAILED              0x07u

typedef struct {
    uint8_t mode;
    uint8_t callback_id;
} zw_api_remove_node_req_t;
typedef zw_api_add_node_cb_t zw_api_remove_node_cb_t; /* identical wire shape */
size_t zw_api_remove_node_req_build(uint8_t *out, size_t cap, const zw_api_remove_node_req_t *in);
bool zw_api_remove_node_req_parse(const uint8_t *p, size_t len, zw_api_remove_node_req_t *out);
size_t zw_api_remove_node_cb_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                    const zw_api_remove_node_cb_t *in);
bool zw_api_remove_node_cb_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                  zw_api_remove_node_cb_t *out);

/* `13` SendData, `16` SendDataAbort (§6.12). SendDataAbort has no payload either way and shares
 * SendData's callback, so it has no codec of its own. */
typedef struct {
    uint16_t node_id;
    const uint8_t *command;
    size_t command_len;
    uint8_t transmit_options; /* ZW_TX_OPT_* */
    uint8_t callback_id;
} zw_api_send_data_req_t;
typedef struct {
    bool queued;
} zw_api_send_data_rsp_t;
/* 'has_tx_report' follows zw_api_tx_report_t's own tolerance: fewer than 17 bytes after
 * callback_id/transmit_status means no report at all, never a parse failure. */
typedef struct {
    uint8_t callback_id;
    uint8_t transmit_status; /* zw_tx_status_t */
    bool has_tx_report;
    zw_api_tx_report_t tx_report;
} zw_api_send_data_cb_t;
size_t zw_api_send_data_req_build(uint8_t *out, size_t cap, zw_nid_width_t w,
                                   const zw_api_send_data_req_t *in);
bool zw_api_send_data_req_parse(const uint8_t *p, size_t len, zw_nid_width_t w,
                                 zw_api_send_data_req_t *out);
size_t zw_api_send_data_rsp_build(uint8_t *out, size_t cap, const zw_api_send_data_rsp_t *in);
bool zw_api_send_data_rsp_parse(const uint8_t *p, size_t len, zw_api_send_data_rsp_t *out);
size_t zw_api_send_data_cb_build(uint8_t *out, size_t cap, const zw_api_send_data_cb_t *in);
bool zw_api_send_data_cb_parse(const uint8_t *p, size_t len, zw_api_send_data_cb_t *out);

/* ================================================================================================
 * What the transport (components/core/zwave_host) needs to run any command without a per-message
 * switch: §6's table itself, and where a built request's callback ID sits so the transport can
 * overwrite it with the ID it allocates just before sending.
 * ================================================================================================ */

/* How a function's exchange finishes, from §6's Callback column: none, a single terminating
 * callback with the request's own callback ID, or a stream of status reports (AddNode/RemoveNode)
 * that ends with Done or Failed. A function whose "callback" is really a different, uncorrelated
 * frame (SoftReset's SerialAPIStarted, SendDataAbort's SendData callback, RequestNodeInfo's
 * ApplicationUpdate) is ZW_CB_NONE: nothing here waits on a callback ID for it. */
typedef enum {
    ZW_CB_NONE = 0,
    ZW_CB_ONE_FINAL,
    ZW_CB_STATUS_REPORTS,
} zw_api_callback_kind_t;

typedef struct {
    bool has_response;         /* §6's Response column */
    zw_api_callback_kind_t callback;
    bool required;              /* §6's Req. column ("-" on an inbound-only frame counts as false) */
    uint8_t phase;               /* 1, 2 or 3 */
} zw_api_func_info_t;

/* False for a function not in §6's table. */
bool zw_api_func_info(uint8_t func, zw_api_func_info_t *out);

/* Where the callback ID sits in a request payload already built for 'func' (SendData's last
 * byte; AddNode/RemoveNode's byte 1, the same for the SmartStart-DSK variant; SetSUCNodeId's,
 * AssignSUCReturnRoute's and HardReset's last byte; every other callback-bearing request's last
 * byte). False when 'func' has no callback ID in its request at all, or 'len' is too short for
 * it. The transport pokes its freshly allocated callback ID in at '*offset' rather than
 * rebuilding the payload. */
bool zw_api_callback_id_offset(uint8_t func, const uint8_t *payload, size_t len, size_t *offset);

#ifdef __cplusplus
}
#endif

#endif /* ZW_API_H */
