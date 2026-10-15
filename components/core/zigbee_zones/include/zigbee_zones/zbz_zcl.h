/*
 * zbz_zcl.h - ZCL frames as bytes (docs/features/20 testing.md §3; ZB-012, ZB-022, ZB-023,
 * ZB-024): the header every cluster command carries, and the IAS Zone commands the panel sends
 * and receives. The siren (zbz_wd.h) and keypad (zbz_ace.h) codecs build on it.
 *
 * Why bytes and not the stack's structs: the frames are what the golden tests freeze (decode, then
 * encode, must give the same bytes back), what the fake radio decodes to answer as a device, and
 * what a bench capture records. The firmware's radio_port hands the stack the payload and the
 * header's fields (zsrc_zb_zcl_t); the stack writes the header itself.
 *
 * A frame: frame control (u8) | manufacturer code (u16, only with ZBZ_ZCL_FC_MANUFACTURER) |
 * sequence number (u8) | command (u8) | payload. Little-endian throughout (ZCL 2.4).
 *
 * Built in ZB-M2 (src/zbz_zcl.c: the header, the global commands' records, IAS Zone; golden
 * frames in test/test_zbz_zcl.c); the siren and keypad codecs come with ZB-M10/ZB-M11.
 * Portable C11, no heap; every function is pure.
 */
#ifndef ZBZ_ZCL_H
#define ZBZ_ZCL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame control (ZCL 2.4.1.1). */
#define ZBZ_ZCL_FC_GLOBAL           0x00u /* frame type: a global (profile-wide) command */
#define ZBZ_ZCL_FC_CLUSTER          0x01u /* frame type: a command of the cluster */
#define ZBZ_ZCL_FC_MANUFACTURER     0x04u /* a manufacturer code follows */
#define ZBZ_ZCL_FC_SERVER_TO_CLIENT 0x08u
#define ZBZ_ZCL_FC_NO_DEFAULT_RSP   0x10u

#define ZBZ_ZCL_HEADER_MIN 3u /* frame control, sequence number, command */
#define ZBZ_ZCL_HEADER_MAX 5u /* ... with a manufacturer code */
#define ZBZ_ZCL_FRAME_MAX  (ZBZ_ZCL_HEADER_MAX + ZSRC_ZB_ZCL_PAYLOAD_MAX)

/* Global commands the panel uses. */
#define ZBZ_ZCL_READ_ATTR      0x00u
#define ZBZ_ZCL_READ_ATTR_RSP  0x01u
#define ZBZ_ZCL_WRITE_ATTR     0x02u
#define ZBZ_ZCL_WRITE_ATTR_RSP 0x04u
#define ZBZ_ZCL_CONFIG_REPORT  0x06u
#define ZBZ_ZCL_REPORT_ATTR    0x0Au
#define ZBZ_ZCL_DEFAULT_RSP    0x0Bu
/* ... and their relatives the record layouts below also read (ZB-M2). */
#define ZBZ_ZCL_WRITE_ATTR_UNDIV 0x03u /* Write Attributes Undivided: the write layout */
#define ZBZ_ZCL_WRITE_ATTR_NORSP 0x05u /* Write Attributes No Response: the write layout */
#define ZBZ_ZCL_CONFIG_REPORT_RSP 0x07u

#define ZBZ_ZCL_FC_TYPE_MASK 0x03u /* frame type bits: 00 global, 01 cluster, 1x reserved */
#define ZBZ_ZCL_FC_RESERVED  0xE0u /* reserved bits: a frame with any of them set is refused */
#define ZBZ_ZCL_ATTR_ALL     0xFFFFu /* a lone SUCCESS status: every attribute (write and
                                      * configure reporting responses) */

/* ZCL status codes the panel tells apart. */
#define ZBZ_ZCL_SUCCESS          0x00u
#define ZBZ_ZCL_FAILURE          0x01u
#define ZBZ_ZCL_NOT_AUTHORIZED   0x7Eu
#define ZBZ_ZCL_UNSUP_COMMAND    0x81u
#define ZBZ_ZCL_INVALID_VALUE    0x87u
#define ZBZ_ZCL_UNSUP_ATTRIBUTE  0x86u

/* ZCL data types the panel writes. */
#define ZBZ_ZCL_TYPE_BOOL   0x10u
#define ZBZ_ZCL_TYPE_U8     0x20u
#define ZBZ_ZCL_TYPE_U16    0x21u
#define ZBZ_ZCL_TYPE_S16    0x29u
#define ZBZ_ZCL_TYPE_ENUM8  0x30u
#define ZBZ_ZCL_TYPE_MAP16  0x19u
#define ZBZ_ZCL_TYPE_EUI64  0xF0u
/* ... and ones the records name (ZB-M2). The codec knows every fixed-size and string type of
 * ZCL 2.6.2; arrays, structures, sets and bags are refused (ZBZ_ZCL_ERR_DATA_TYPE). */
#define ZBZ_ZCL_TYPE_NONE   0x00u
#define ZBZ_ZCL_TYPE_MAP8   0x18u
#define ZBZ_ZCL_TYPE_U32    0x23u
#define ZBZ_ZCL_TYPE_S8     0x28u
#define ZBZ_ZCL_TYPE_ENUM16 0x31u
#define ZBZ_ZCL_TYPE_OCTETS 0x41u /* octet string: a length byte, then the bytes */
#define ZBZ_ZCL_TYPE_STRING 0x42u /* character string: a length byte, then the text */

/* IAS Zone (0x0500): attributes and commands. */
#define ZBZ_IAS_ATTR_ZONE_STATE  0x0000u /* 0 not enrolled, 1 enrolled */
#define ZBZ_IAS_ATTR_ZONE_TYPE   0x0001u
#define ZBZ_IAS_ATTR_ZONE_STATUS 0x0002u
#define ZBZ_IAS_ATTR_CIE_ADDRESS 0x0010u
#define ZBZ_IAS_ATTR_ZONE_ID     0x0011u
/* client to server (the panel to the sensor) */
#define ZBZ_IAS_ENROLL_RESPONSE  0x00u
#define ZBZ_IAS_NORMAL_MODE      0x01u /* Initiate Normal Operation Mode */
#define ZBZ_IAS_TEST_MODE        0x02u /* Initiate Test Mode: duration (s) u8, sensitivity u8 */
/* server to client (the sensor to the panel) */
#define ZBZ_IAS_STATUS_CHANGE    0x00u
#define ZBZ_IAS_ENROLL_REQUEST   0x01u

/* Enroll Response codes. */
#define ZBZ_IAS_ENROLL_OK            0x00u
#define ZBZ_IAS_ENROLL_NOT_SUPPORTED 0x01u
#define ZBZ_IAS_ENROLL_NO_PERMIT     0x02u /* "No enroll permit" */
#define ZBZ_IAS_ENROLL_TOO_MANY      0x03u /* "Too many zones" */

/* What a codec answers. */
typedef enum {
    ZBZ_ZCL_OK = 0,
    ZBZ_ZCL_ERR_SHORT,     /* fewer bytes than its fields need */
    ZBZ_ZCL_ERR_LONG,      /* more than ZSRC_ZB_ZCL_PAYLOAD_MAX, or than `cap` */
    ZBZ_ZCL_ERR_TYPE,      /* not the frame type or direction asked for */
    ZBZ_ZCL_ERR_VALUE,     /* a field out of its range (a mode, a length byte past the end) */
    ZBZ_ZCL_ERR_DATA_TYPE, /* an attribute's data type the codec can't size (ZB-M2) */
} zbz_zcl_result_t;

/* A whole frame (header and payload, as an APS payload) into `out`, whose cluster is the APS
 * frame's (`cluster`); trailing bytes past a command's fields are kept in the payload.
 * zsrc_zb_zcl_t is a cluster command and has no frame-type field, so this takes cluster frames
 * only: a global frame is ERR_TYPE here and is read with zbz_zcl_decode_global. A reserved
 * frame type is ERR_TYPE, a reserved frame-control bit ERR_VALUE, and so is a manufacturer-specific
 * frame whose manufacturer code is 0 (zsrc_zb_zcl_t would carry it as a standard command). */
zbz_zcl_result_t zbz_zcl_decode(const uint8_t *bytes, size_t len, uint16_t cluster,
                                zsrc_zb_zcl_t *out);
/* `f` as bytes, with `tsn` when f->tsn is ZSRC_ZB_TSN_NEW; the length, 0 when cap is too small
 * (or f->len is past ZSRC_ZB_ZCL_PAYLOAD_MAX, or f->tsn is neither 0..255 nor NEW). A cluster
 * frame: a global command's is zbz_zcl_encode_global. */
size_t zbz_zcl_encode(const zsrc_zb_zcl_t *f, uint8_t tsn, uint8_t *out, size_t cap);

/* ---- global (profile-wide) commands (ZCL 2.5; ZB-003, ZB-012) --------------------------------- */

/* The same two for frame type 00: the global command travels in a zsrc_zb_zcl_t whose `command`
 * is ZBZ_ZCL_READ_ATTR..., and the frame type is the function's. Each pair refuses the other's
 * frames (ERR_TYPE). */
zbz_zcl_result_t zbz_zcl_decode_global(const uint8_t *bytes, size_t len, uint16_t cluster,
                                       zsrc_zb_zcl_t *out);
size_t zbz_zcl_encode_global(const zsrc_zb_zcl_t *f, uint8_t tsn, uint8_t *out, size_t cap);
/* A capture's frame type: true when `bytes` start a global frame. */
bool zbz_zcl_is_global(const uint8_t *bytes, size_t len);

/* An empty global command of `cluster`: a Default Response asked for, ZSRC_ZB_TSN_NEW, no
 * manufacturer. Records are added with zbz_zcl_put_attr. */
void zbz_zcl_global(uint16_t cluster, uint8_t command, bool server_to_client,
                    zsrc_zb_zcl_t *out);

/* One attribute record, in the layout of the frame's command:
 *   READ_ATTR                         attr
 *   READ_ATTR_RSP                     attr | status | (SUCCESS: type | value)
 *   WRITE_ATTR, _UNDIV, _NORSP;
 *   REPORT_ATTR                       attr | type | value
 *   WRITE_ATTR_RSP, CONFIG_REPORT_RSP status | (CONFIG_REPORT_RSP: direction) | attr, or a
 *                                     lone SUCCESS: attr ZBZ_ZCL_ATTR_ALL
 * A value is little-endian; a string's `len` is its characters, without the length byte (an
 * invalid string, length 0xFF, reads as ""). */
typedef struct {
    uint16_t attr;
    uint8_t status;          /* ZBZ_ZCL_SUCCESS in layouts without one */
    uint8_t type;            /* ZBZ_ZCL_TYPE_NONE in layouts without a value */
    uint8_t len;             /* the value's bytes */
    const uint8_t *value;    /* into the frame's payload (valid while it is); NULL when len 0 */
} zbz_zcl_attr_t;

typedef struct {
    const zsrc_zb_zcl_t *f;
    uint8_t pos;
    zbz_zcl_result_t err;    /* why zbz_zcl_next_attr stopped: OK at the end of the records */
} zbz_zcl_iter_t;
/* Walks `f`'s records; `f` must come from zbz_zcl_decode_global (or zbz_zcl_global). */
void zbz_zcl_iter(zbz_zcl_iter_t *it, const zsrc_zb_zcl_t *f);
/* The next record: false at the end (it->err OK) or at a record that doesn't fit or can't be
 * sized (it->err SHORT, DATA_TYPE; TYPE when the command has no attribute records). */
bool zbz_zcl_next_attr(zbz_zcl_iter_t *it, zbz_zcl_attr_t *out);
/* Appends `a` in the layout of f->command; false when it doesn't fit the payload, its type is
 * one the codec can't size, a fixed-size value's `len` isn't its type's, or the command has no
 * records. */
bool zbz_zcl_put_attr(zsrc_zb_zcl_t *f, const zbz_zcl_attr_t *a);
/* A fixed-size value as a number (0 for strings and longer types); _int sign-extends the signed
 * integer types (0x28-0x2F). */
uint64_t zbz_zcl_attr_uint(const zbz_zcl_attr_t *a);
int64_t zbz_zcl_attr_int(const zbz_zcl_attr_t *a);

/* Configure Reporting (0x06), the panel's records only (direction 0: the device reports), all of
 * one cluster (r[0].cluster); the reportable change is written for analog types (integers,
 * floats, time) in its type's size. false: empty, mixed clusters, a type the codec can't size, or
 * more than a payload holds. */
bool zbz_zcl_config_report(const zsrc_zb_reporting_t *r, uint8_t count, zsrc_zb_zcl_t *out);
/* Its records back (the fake radio's side): VALUE for direction 1 or a change past 32 bits,
 * DATA_TYPE, SHORT, LONG when more than `cap`; TYPE when `f` isn't a Configure Reporting. */
zbz_zcl_result_t zbz_zcl_decode_config_report(const zsrc_zb_zcl_t *f, zsrc_zb_reporting_t *out,
                                              uint8_t cap, uint8_t *count);

/* Default Response (0x0B): the command it answers and its status. Built without asking for a
 * Default Response itself. */
void zbz_zcl_default_rsp(uint16_t cluster, uint8_t command, uint8_t status,
                         bool server_to_client, zsrc_zb_zcl_t *out);
zbz_zcl_result_t zbz_zcl_decode_default_rsp(const zsrc_zb_zcl_t *f, uint8_t *command,
                                            uint8_t *status);

/* ---- IAS Zone (ZB-003, ZB-022) ---------------------------------------------------------------- */

/* The panel's address as the sensor's CIE address: a global Write Attributes of 0x0010 (EUI64).
 * Golden: 00 57 02 | 10 00 F0 + the 8 address bytes, low first (testing.md §3). A global
 * command: its bytes are zbz_zcl_encode_global's.
 * The decoders below answer ERR_TYPE for another cluster, command or direction, and for a
 * manufacturer-specific frame (its command ids are the manufacturer's). */
void zbz_ias_cie_write(uint64_t panel_ieee, zsrc_zb_zcl_t *out);

typedef struct {
    uint16_t zone_type;      /* ZSRC_ZB_ZONE_TYPE_* */
    uint16_t manufacturer;   /* the device's manufacturer code */
} zbz_ias_enroll_req_t;
/* Golden: 09 57 01 | 15 00 15 10 is a contact (0x0015) of manufacturer 0x1015. */
zbz_zcl_result_t zbz_ias_decode_enroll_request(const zsrc_zb_zcl_t *f, zbz_ias_enroll_req_t *out);
/* Golden: 01 57 00 | 00 05 is "enrolled, zone id 5". */
void zbz_ias_enroll_response(uint8_t code, uint8_t zone_id, zsrc_zb_zcl_t *out);

typedef struct {
    uint16_t status;         /* ZSRC_ZB_IAS_* bits */
    uint8_t extended;
    uint8_t zone_id;
    uint16_t delay_qs;       /* quarter seconds the change waited in the device */
} zbz_ias_status_t;
/* A Zone Status Change Notification; the zone id and delay are optional in older devices (absent:
 * 0xFF and 0; the extended status absent: 0). 2, 3, 4 or 6 bytes or more; 5 is a cut delay
 * (SHORT). */
zbz_zcl_result_t zbz_ias_decode_status_change(const zsrc_zb_zcl_t *f, zbz_ias_status_t *out);

/* Initiate Test Mode for `seconds` (the ZCL field is one byte of seconds) at `sensitivity` (0: the
 * device's default), or with seconds 0 Initiate Normal Operation Mode. */
void zbz_ias_test_mode(uint8_t seconds, uint8_t sensitivity, zsrc_zb_zcl_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_ZCL_H */
