/*
 * test_zbz_zcl.c - ZCL frames as bytes (zbz_zcl.h; docs/features/20 testing.md §3; ZB-003,
 * ZB-012, ZB-022). Golden frames both ways: each decodes to the fields named, and the fields
 * encode back to the same bytes. Then every way a frame can be wrong.
 *
 * Where the bytes come from, per vector (cross-checked against the open projects on 2026-10-03,
 * BL-143; the links are permalinks to the commits named, so they still show what was read):
 * - "testing.md §3": the table there. "verified (bytes)": the same frame is in a project's own
 *   test. "verified (layout)": the command ids, field order, widths and header bits match the
 *   projects' definitions and what their code sends or parses, but no project holds this exact
 *   byte string. "derived": the ZCL field rules only (ZCL rev 8, 07-5123: the header 2.4.1, the
 *   global commands 2.5, the data types 2.6.2, IAS Zone 8.2); still to be cross-checked against
 *   Wireshark's ZCL dissector on the bench.
 * - "herdsman": Koenkk/zigbee-herdsman at 03495312e131 (2026-10-02).
 * - "zigpy": zigpy/zigpy at fb63170d520e (2026-09-29).
 * - "zha": zigpy/zha at f3eb54c0fa42 (2026-10-01).
 * - "deconz": dresden-elektronik/deconz-rest-plugin at a4c17adfa04a (2026-09-10).
 * Vectors under "from the projects' tests" are copied byte for byte, each with its link; a byte
 * string is a fact, no code was copied. The tsn differs from ours in most of them (herdsman's
 * 0x0A); nothing here depends on it.
 * Addresses are fictional and locally administered (0x02AB_CD00_0000_0001).
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "zigbee_zones/zbz_zcl.h"

#define PANEL_IEEE 0x02ABCD0000000001ull

void setUp(void) {}
void tearDown(void) {}

/* Decodes `b` (a cluster or global frame), then encodes the result: the same bytes. */
static zsrc_zb_zcl_t round_trip(const uint8_t *b, size_t n, uint16_t cluster, bool global)
{
    zsrc_zb_zcl_t f;
    zbz_zcl_result_t r = global ? zbz_zcl_decode_global(b, n, cluster, &f)
                                : zbz_zcl_decode(b, n, cluster, &f);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, r);
    TEST_ASSERT_EQUAL(global, zbz_zcl_is_global(b, n));
    uint8_t out[ZBZ_ZCL_FRAME_MAX];
    size_t m = global ? zbz_zcl_encode_global(&f, 0xEEu, out, sizeof(out))
                      : zbz_zcl_encode(&f, 0xEEu, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(n, m);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, out, n);
    return f;
}

/* Encodes a built frame with `tsn`: the golden bytes. */
static void encodes_to(const zsrc_zb_zcl_t *f, bool global, uint8_t tsn, const uint8_t *b,
                       size_t n)
{
    uint8_t out[ZBZ_ZCL_FRAME_MAX];
    size_t m = global ? zbz_zcl_encode_global(f, tsn, out, sizeof(out))
                      : zbz_zcl_encode(f, tsn, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(n, m);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, out, n);
}

/* ---- IAS Zone golden frames -------------------------------------------------------------------- */

/* ZB-003: Zone Enroll Request, a contact (0x0015) of manufacturer 0x1015 (testing.md §3).
 * verified (layout): command 0x01 is zone type u16 then manufacturer code u16 (zigpy's IasZone
 * client command "enroll"; deCONZ reads the same two u16); Contact_Switch is 0x0015; frame control
 * 0x09 is a cluster command from the server with the Default Response not disabled (herdsman's
 * "replies to IAS Zone Enroll request" builds exactly that header).
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/device.test.ts
 * https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/ias_zone.cpp */
static void test_golden_enroll_request(void)
{
    static const uint8_t b[] = {0x09, 0x57, 0x01, 0x15, 0x00, 0x15, 0x10};
    zsrc_zb_zcl_t f = round_trip(b, sizeof(b), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, f.cluster);
    TEST_ASSERT_TRUE(f.server_to_client);
    TEST_ASSERT_TRUE(f.want_response);
    TEST_ASSERT_EQUAL_UINT16(0x57u, f.tsn);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_IAS_ENROLL_REQUEST, f.command);
    TEST_ASSERT_EQUAL_UINT8(4u, f.len);
    zbz_ias_enroll_req_t req;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_enroll_request(&f, &req));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, req.zone_type);
    TEST_ASSERT_EQUAL_HEX16(0x1015u, req.manufacturer);
}

/* ZB-003: Zone Enroll Response "enrolled, zone id 5" (testing.md §3).
 * verified (layout), frame control differs: the code, then the zone id (zigpy's IasZone server
 * command "enroll_response" 0x00). zigbee-herdsman (device.ts) and deCONZ (ias_zone.cpp) both send
 * it with the Default Response disabled, frame control 0x11, and herdsman echoes the request's
 * tsn; ours is 0x01 (asks for a Default Response, which the golden "default response" below
 * expects). Both wire forms are valid ZCL; 0x11 may be preferred (BL-143 notes).
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/src/controller/model/device.ts
 * https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/ias_zone.cpp */
static void test_golden_enroll_response(void)
{
    static const uint8_t b[] = {0x01, 0x57, 0x00, 0x00, 0x05};
    zsrc_zb_zcl_t f;
    zbz_ias_enroll_response(ZBZ_IAS_ENROLL_OK, 5, &f);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_ZB_TSN_NEW, f.tsn);
    encodes_to(&f, false, 0x57u, b, sizeof(b));
    zsrc_zb_zcl_t g = round_trip(b, sizeof(b), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_FALSE(g.server_to_client);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_IAS_ENROLL_RESPONSE, g.command);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_IAS_ENROLL_OK, g.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(5u, g.payload[1]);

    /* A refusal: the code is the first byte (zigpy's IasZone.EnrollResponse: 0 Success,
     * 1 Not_supported, 2 No_enroll_permit, 3 Too_many_zones). */
    zbz_ias_enroll_response(ZBZ_IAS_ENROLL_NO_PERMIT, 0, &f);
    static const uint8_t refused[] = {0x01, 0x09, 0x00, 0x02, 0x00};
    encodes_to(&f, false, 0x09u, refused, sizeof(refused));
    zbz_ias_enroll_response(ZBZ_IAS_ENROLL_TOO_MANY, 0, &f);
    static const uint8_t full[] = {0x01, 0x09, 0x00, 0x03, 0x00};
    encodes_to(&f, false, 0x09u, full, sizeof(full));
}

/* ZB-003: the panel's address written as the sensor's CIE address: a global Write Attributes of
 * 0x0010, EUI64, low byte first (testing.md §3).
 * verified (layout): attribute 0x0010 of IAS Zone is cie_addr, EUI64 (zigpy security.py; herdsman
 * writes iasCieAddr, deCONZ attribute 0x0010 of type 0xF0); a Write Attributes record is attribute
 * id u16, data type, value (herdsman's foundation "write" frame, tested below); an EUI64 goes low
 * byte first (zigpy's EUI64 test: wire bytes 01..08 are "08:07:...:01"). herdsman asks for no
 * Default Response on a write (frame control 0x10, its default for a command that has a
 * response); ours is 0x00.
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_types.py
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/src/controller/model/device.ts */
static void test_golden_cie_write(void)
{
    static const uint8_t b[] = {0x00, 0x57, 0x02, 0x10, 0x00, 0xF0, 0x01, 0x00,
                                0x00, 0x00, 0x00, 0xCD, 0xAB, 0x02};
    zsrc_zb_zcl_t f;
    zbz_ias_cie_write(PANEL_IEEE, &f);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, f.cluster);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_WRITE_ATTR, f.command);
    encodes_to(&f, true, 0x57u, b, sizeof(b));

    zsrc_zb_zcl_t g = round_trip(b, sizeof(b), ZSRC_ZB_ZCL_IAS_ZONE, true);
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zbz_zcl_iter(&it, &g);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(ZBZ_IAS_ATTR_CIE_ADDRESS, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_EUI64, a.type);
    TEST_ASSERT_EQUAL_UINT8(8u, a.len);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == PANEL_IEEE);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* ZB-003: a global frame is not a cluster command. */
    zsrc_zb_zcl_t h;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE,
                          zbz_zcl_decode(b, sizeof(b), ZSRC_ZB_ZCL_IAS_ZONE, &h));
}

/* ZB-003: Zone Status Change Notification, whole and in the shorter forms older devices send
 * (derived: IAS Zone 8.2.2.4.1 - status u16, extended u8, zone id u8, delay u16). The whole form's
 * layout is verified: zigpy's "status_change_notification" has the same four fields, deCONZ reads
 * them in that order, and zigpy's own test frame is below (test_golden_project_vectors). The
 * shorter forms are older devices' habit, not in the projects' tests.
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py
 * https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/ias_zone.cpp */
static void test_golden_status_change(void)
{
    static const uint8_t whole[] = {0x09, 0x0A, 0x00, 0x05, 0x00, 0x00, 0x03, 0x08, 0x00};
    zsrc_zb_zcl_t f = round_trip(whole, sizeof(whole), ZSRC_ZB_ZCL_IAS_ZONE, false);
    zbz_ias_status_t s;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&f, &s));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_IAS_ALARM1 | ZSRC_ZB_IAS_TAMPER, s.status);
    TEST_ASSERT_EQUAL_UINT8(0u, s.extended);
    TEST_ASSERT_EQUAL_UINT8(3u, s.zone_id);
    TEST_ASSERT_EQUAL_UINT16(8u, s.delay_qs);

    /* Status only: zone id 0xFF, delay 0. */
    static const uint8_t two[] = {0x09, 0x0B, 0x00, 0x21, 0x00};
    f = round_trip(two, sizeof(two), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&f, &s));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_IAS_ALARM1 | ZSRC_ZB_IAS_RESTORE, s.status);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, s.zone_id);
    TEST_ASSERT_EQUAL_UINT16(0u, s.delay_qs);

    /* Status and extended status. */
    static const uint8_t three[] = {0x19, 0x0C, 0x00, 0x00, 0x00, 0x00};
    f = round_trip(three, sizeof(three), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_FALSE(f.want_response);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&f, &s));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, s.zone_id);

    /* With the zone id, no delay. */
    static const uint8_t four[] = {0x09, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x07};
    f = round_trip(four, sizeof(four), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&f, &s));
    TEST_ASSERT_EQUAL_UINT8(7u, s.zone_id);
    TEST_ASSERT_EQUAL_UINT16(0u, s.delay_qs);

    /* A cut delay, and a status cut short. */
    static const uint8_t five[] = {0x09, 0x0E, 0x00, 0x01, 0x00, 0x00, 0x07, 0x08};
    f = round_trip(five, sizeof(five), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_ias_decode_status_change(&f, &s));
    static const uint8_t one[] = {0x09, 0x0F, 0x00, 0x01};
    f = round_trip(one, sizeof(one), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_ias_decode_status_change(&f, &s));
}

/* ZB-022: Initiate Test Mode for 5 seconds at the device's sensitivity, and back to normal
 * (derived: IAS Zone 8.2.2.3.2 and 8.2.2.3.3). verified (layout): zigpy's IasZone server commands
 * "init_normal_op_mode" 0x01 (no payload) and "init_test_mode" 0x02 (test_mode_duration u8,
 * current_zone_sensitivity_level u8).
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py */
static void test_golden_test_mode(void)
{
    zsrc_zb_zcl_t f;
    zbz_ias_test_mode(5, 0, &f);
    static const uint8_t test[] = {0x01, 0x57, 0x02, 0x05, 0x00};
    encodes_to(&f, false, 0x57u, test, sizeof(test));
    round_trip(test, sizeof(test), ZSRC_ZB_ZCL_IAS_ZONE, false);

    zbz_ias_test_mode(0, 9, &f);
    static const uint8_t normal[] = {0x01, 0x58, 0x01};
    encodes_to(&f, false, 0x58u, normal, sizeof(normal));
    zsrc_zb_zcl_t g = round_trip(normal, sizeof(normal), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_UINT8(0u, g.len);
}

/* ZB-022 (and ZB-024's codec): a frame of another cluster through the header codec - the IAS ACE
 * Arm (disarm, code 123456, zone 0) from zigbee-herdsman's zcl tests (testing.md §3). The payload
 * is kept whole.
 * verified (bytes): the array 1, 87, 0, 0, 6, 49, 50, 51, 52, 53, 54, 0 of herdsman's test
 * "ZclFrame from buffer ssIasAce arm command", read as frame type 1 (cluster), direction 0,
 * Default Response not disabled, tsn 87, command 0, payload {armmode 0, code "123456", zoneid 0}.
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
static void test_golden_ace_arm_header(void)
{
    static const uint8_t b[] = {0x01, 0x57, 0x00, 0x00, 0x06, 0x31, 0x32,
                                0x33, 0x34, 0x35, 0x36, 0x00};
    zsrc_zb_zcl_t f = round_trip(b, sizeof(b), ZSRC_ZB_ZCL_IAS_ACE, false);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ACE, f.cluster);
    TEST_ASSERT_EQUAL_UINT8(9u, f.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&b[3], f.payload, 9);
    /* ZB-003: not IAS Zone's. */
    zbz_ias_status_t s;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_status_change(&f, &s));
}

/* ---- global commands' golden frames ------------------------------------------------------------ */

/* ZB-012: Read Attributes of Basic's manufacturer and model, and its answer: the manufacturer
 * as a character string, the model unsupported (derived: ZCL 2.5.1, 2.5.2). verified (layout):
 * a Read Attributes is a list of attribute ids u16 and its response a list of attribute id,
 * status, then data type and value only when the status is success; herdsman's foundation "read"
 * and "readRsp" frames and zcl.test.ts' "readRsp failed" have the same shape (tested below). Theirs
 * ask for no Default Response (0x10 / 0x18) where ours is 0x00; the response's 0x18 is the same.
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
static void test_golden_read_attributes(void)
{
    static const uint8_t req[] = {0x00, 0x01, 0x00, 0x04, 0x00, 0x05, 0x00};
    zsrc_zb_zcl_t f;
    zbz_zcl_global(ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_READ_ATTR, false, &f);
    zbz_zcl_attr_t a = {.attr = 0x0004u};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &a));
    a.attr = 0x0005u;
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &a));
    encodes_to(&f, true, 0x01u, req, sizeof(req));

    zsrc_zb_zcl_t g = round_trip(req, sizeof(req), ZSRC_ZB_ZCL_BASIC, true);
    zbz_zcl_iter_t it;
    zbz_zcl_iter(&it, &g);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0004u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0005u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    static const uint8_t rsp[] = {0x18, 0x01, 0x01, 0x04, 0x00, 0x00, 0x42, 0x04,
                                  0x41, 0x43, 0x4D, 0x45, 0x05, 0x00, 0x86};
    g = round_trip(rsp, sizeof(rsp), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_TRUE(g.server_to_client);
    TEST_ASSERT_FALSE(g.want_response);
    zbz_zcl_iter(&it, &g);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0004u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, a.status);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_STRING, a.type);
    TEST_ASSERT_EQUAL_UINT8(4u, a.len);
    TEST_ASSERT_EQUAL_MEMORY("ACME", a.value, 4);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0005u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_UNSUP_ATTRIBUTE, a.status);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_NONE, a.type);
    TEST_ASSERT_NULL(a.value);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* The fake radio's side: the same answer built record by record. */
    zbz_zcl_global(ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_READ_ATTR_RSP, true, &f);
    f.want_response = false;
    const zbz_zcl_attr_t m = {.attr = 0x0004u, .type = ZBZ_ZCL_TYPE_STRING, .len = 4,
                              .value = (const uint8_t *)"ACME"};
    const zbz_zcl_attr_t x = {.attr = 0x0005u, .status = ZBZ_ZCL_UNSUP_ATTRIBUTE};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &m));
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &x));
    encodes_to(&f, true, 0x01u, rsp, sizeof(rsp));
}

/* ZB-012: the setup's other answers as numbers: Basic's power source (enum8 battery), IAS Zone's
 * zone type (enum16 contact) and a temperature report, positive and negative (int16, 0.01 °C)
 * (derived: ZCL 2.5.2, 2.5.11, 2.6.2). The Report Attributes header 0x18 | tsn | 0x0A and the
 * record "attribute id, data type, value" are verified by herdsman's foundation "report" frame
 * and zcl.test.ts' "ZclFrame from buffer report" (both tested below); the values here are ours. */
static void test_golden_numbers(void)
{
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;

    static const uint8_t power[] = {0x18, 0x02, 0x01, 0x07, 0x00, 0x00, 0x30, 0x03};
    zsrc_zb_zcl_t f = round_trip(power, sizeof(power), ZSRC_ZB_ZCL_BASIC, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_ENUM8, a.type);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 3u);

    static const uint8_t zone[] = {0x18, 0x03, 0x01, 0x01, 0x00, 0x00, 0x31, 0x15, 0x00};
    f = round_trip(zone, sizeof(zone), ZSRC_ZB_ZCL_IAS_ZONE, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(ZBZ_IAS_ATTR_ZONE_TYPE, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == ZSRC_ZB_ZONE_TYPE_CONTACT);

    static const uint8_t warm[] = {0x18, 0x22, 0x0A, 0x00, 0x00, 0x29, 0x66, 0x08};
    f = round_trip(warm, sizeof(warm), ZSRC_ZB_ZCL_TEMP_MEAS, true);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_REPORT_ATTR, f.command);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&a) == 2150);

    static const uint8_t cold[] = {0x18, 0x23, 0x0A, 0x00, 0x00, 0x29, 0x06, 0xFF};
    f = round_trip(cold, sizeof(cold), ZSRC_ZB_ZCL_TEMP_MEAS, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&a) == -250);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 0xFF06u);

    /* Sign extension of the other widths; unsigned types stay positive; strings are 0. */
    const uint8_t ff = 0xFFu;
    const uint8_t s24[3] = {0x00, 0x00, 0x80};
    zbz_zcl_attr_t v = {.type = ZBZ_ZCL_TYPE_S8, .len = 1, .value = &ff};
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == -1);
    v.type = ZBZ_ZCL_TYPE_U8;
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == 255);
    v = (zbz_zcl_attr_t){.type = 0x2Au, .len = 3, .value = s24};
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == -8388608);
    v = (zbz_zcl_attr_t){.type = ZBZ_ZCL_TYPE_STRING, .len = 1, .value = &ff};
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&v) == 0u);
    v = (zbz_zcl_attr_t){.type = ZBZ_ZCL_TYPE_U16, .len = 1, .value = &ff};
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&v) == 0u); /* a length that isn't its type's */
    /* ZB-003: a signed value of length 0 or of another length than its type's reads as 0 (no
     * sign extension, no shift past the width). */
    v = (zbz_zcl_attr_t){.type = ZBZ_ZCL_TYPE_S8, .len = 0, .value = &ff};
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == 0);
    v = (zbz_zcl_attr_t){.type = ZBZ_ZCL_TYPE_S16, .len = 0, .value = s24};
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == 0);
    v = (zbz_zcl_attr_t){.type = ZBZ_ZCL_TYPE_S16, .len = 1, .value = &ff};
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == 0);
    v = (zbz_zcl_attr_t){.type = ZBZ_ZCL_TYPE_S8, .len = 1, .value = NULL};
    TEST_ASSERT_TRUE(zbz_zcl_attr_int(&v) == 0);
}

/* ZB-003: Write Attributes Response, all written (a lone SUCCESS) and one refused (derived: ZCL
 * 2.5.5). verified (bytes) for the lone SUCCESS: herdsman's foundation "writeRsp ... handles
 * SUCCESS payload" is 18 0A 04 00, ours with another tsn; the refusals' records (status, attribute
 * id u16, no direction byte) match its two-record frame, tested below, and zigpy's
 * "test_write_attrs_response_deserialize" payloads 86 34 12 87 35 12.
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_zcl_foundation.py */
static void test_golden_write_response(void)
{
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    static const uint8_t ok[] = {0x18, 0x57, 0x04, 0x00};
    zsrc_zb_zcl_t f = round_trip(ok, sizeof(ok), ZSRC_ZB_ZCL_IAS_ZONE, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(ZBZ_ZCL_ATTR_ALL, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, a.status);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    zsrc_zb_zcl_t g;
    zbz_zcl_global(ZSRC_ZB_ZCL_IAS_ZONE, ZBZ_ZCL_WRITE_ATTR_RSP, true, &g);
    g.want_response = false;
    const zbz_zcl_attr_t all = {.attr = ZBZ_ZCL_ATTR_ALL};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&g, &all));
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&g, &all)); /* a lone SUCCESS is the whole answer */
    encodes_to(&g, true, 0x57u, ok, sizeof(ok));

    static const uint8_t refused[] = {0x18, 0x58, 0x04, 0x87, 0x10, 0x00};
    f = round_trip(refused, sizeof(refused), ZSRC_ZB_ZCL_IAS_ZONE, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(ZBZ_IAS_ATTR_CIE_ADDRESS, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_INVALID_VALUE, a.status);
    zbz_zcl_global(ZSRC_ZB_ZCL_IAS_ZONE, ZBZ_ZCL_WRITE_ATTR_RSP, true, &g);
    g.want_response = false;
    const zbz_zcl_attr_t bad = {.attr = ZBZ_IAS_ATTR_CIE_ADDRESS, .status = ZBZ_ZCL_INVALID_VALUE};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&g, &bad));
    encodes_to(&g, true, 0x58u, refused, sizeof(refused));
}

/* ZB-012: Configure Reporting of a battery (uint8, change 2), a temperature (int16, change 10)
 * and an on/off state (bool: discrete, no change), and the answers (derived: ZCL 2.5.7, 2.5.8).
 * verified (layout): herdsman's foundation "configReport" frame has the same record - direction 0,
 * attribute id u16, data type, minimum u16, maximum u16, then the change only for an analog type
 * (a bool record is followed at once by the next record) - and "configReportRsp" the same
 * status, direction, attribute id u16 records with a lone 00 for all-success (all tested below).
 * Herdsman sends such requests with the Default Response disabled (frame control 0x10); ours
 * is 0x00.
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
static void test_golden_configure_reporting(void)
{
    const zsrc_zb_reporting_t batt = {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x0021u,
                                      .type = ZBZ_ZCL_TYPE_U8, .min_s = 3600, .max_s = 21600,
                                      .change = 2};
    static const uint8_t b1[] = {0x00, 0x10, 0x06, 0x00, 0x21, 0x00, 0x20,
                                 0x10, 0x0E, 0x60, 0x54, 0x02};
    const zsrc_zb_reporting_t temp = {.cluster = ZSRC_ZB_ZCL_TEMP_MEAS, .attr = 0x0000u,
                                      .type = ZBZ_ZCL_TYPE_S16, .min_s = 30, .max_s = 3600,
                                      .change = 10};
    static const uint8_t b2[] = {0x00, 0x11, 0x06, 0x00, 0x00, 0x00, 0x29,
                                 0x1E, 0x00, 0x10, 0x0E, 0x0A, 0x00};
    const zsrc_zb_reporting_t onoff = {.cluster = ZSRC_ZB_ZCL_ON_OFF, .attr = 0x0000u,
                                       .type = ZBZ_ZCL_TYPE_BOOL, .min_s = 0, .max_s = 3600,
                                       .change = 7 /* ignored: discrete */};
    static const uint8_t b3[] = {0x00, 0x12, 0x06, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x0E};
    const zsrc_zb_reporting_t *rs[] = {&batt, &temp, &onoff};
    const uint8_t *bs[] = {b1, b2, b3};
    const size_t ns[] = {sizeof(b1), sizeof(b2), sizeof(b3)};
    for (size_t i = 0; i < 3; i++) {
        zsrc_zb_zcl_t f;
        TEST_ASSERT_TRUE(zbz_zcl_config_report(rs[i], 1, &f));
        encodes_to(&f, true, (uint8_t)(0x10u + i), bs[i], ns[i]);
        zsrc_zb_zcl_t g = round_trip(bs[i], ns[i], rs[i]->cluster, true);
        zsrc_zb_reporting_t back[2];
        uint8_t n = 0;
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_config_report(&g, back, 2, &n));
        TEST_ASSERT_EQUAL_UINT8(1u, n);
        TEST_ASSERT_EQUAL_HEX16(rs[i]->cluster, back[0].cluster);
        TEST_ASSERT_EQUAL_HEX16(rs[i]->attr, back[0].attr);
        TEST_ASSERT_EQUAL_HEX8(rs[i]->type, back[0].type);
        TEST_ASSERT_EQUAL_UINT16(rs[i]->min_s, back[0].min_s);
        TEST_ASSERT_EQUAL_UINT16(rs[i]->max_s, back[0].max_s);
        TEST_ASSERT_EQUAL_UINT32(i == 2 ? 0u : rs[i]->change, back[0].change);
    }

    /* Two records of one cluster in one frame. */
    const zsrc_zb_reporting_t two[2] = {
        batt, {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x0020u, .type = ZBZ_ZCL_TYPE_U8,
               .min_s = 3600, .max_s = 21600, .change = 1}};
    zsrc_zb_zcl_t f;
    TEST_ASSERT_TRUE(zbz_zcl_config_report(two, 2, &f));
    TEST_ASSERT_EQUAL_UINT8(18u, f.len);
    zsrc_zb_reporting_t back[2];
    uint8_t n = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_config_report(&f, back, 2, &n));
    TEST_ASSERT_EQUAL_UINT8(2u, n);
    TEST_ASSERT_EQUAL_HEX16(0x0020u, back[1].attr);

    /* The answers: all configured, and one the device can't report. */
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    static const uint8_t ok[] = {0x18, 0x10, 0x07, 0x00};
    zsrc_zb_zcl_t g = round_trip(ok, sizeof(ok), ZSRC_ZB_ZCL_POWER_CFG, true);
    zbz_zcl_iter(&it, &g);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(ZBZ_ZCL_ATTR_ALL, a.attr);
    static const uint8_t no[] = {0x18, 0x11, 0x07, 0x8C, 0x00, 0x21, 0x00};
    g = round_trip(no, sizeof(no), ZSRC_ZB_ZCL_POWER_CFG, true);
    zbz_zcl_iter(&it, &g);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(0x8Cu, a.status);
    TEST_ASSERT_EQUAL_HEX16(0x0021u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    zsrc_zb_zcl_t h;
    zbz_zcl_global(ZSRC_ZB_ZCL_POWER_CFG, ZBZ_ZCL_CONFIG_REPORT_RSP, true, &h);
    h.want_response = false;
    const zbz_zcl_attr_t rec = {.attr = 0x0021u, .status = 0x8Cu};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&h, &rec));
    encodes_to(&h, true, 0x11u, no, sizeof(no));
}

/* ZB-003: Default Response from a sensor to the panel's Enroll Response (derived: ZCL 2.5.12).
 * verified (bytes): herdsman's foundation "defaultRsp" is 18 0A 0B 00 00 - this frame with
 * another tsn - and zcl.test.ts' 18 04 0B 0C 82 has the same layout (command id, status).
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
static void test_golden_default_response(void)
{
    static const uint8_t b[] = {0x18, 0x05, 0x0B, 0x00, 0x00};
    zsrc_zb_zcl_t f;
    zbz_zcl_default_rsp(ZSRC_ZB_ZCL_IAS_ZONE, ZBZ_IAS_ENROLL_RESPONSE, ZBZ_ZCL_SUCCESS, true, &f);
    encodes_to(&f, true, 0x05u, b, sizeof(b));
    zsrc_zb_zcl_t g = round_trip(b, sizeof(b), ZSRC_ZB_ZCL_IAS_ZONE, true);
    uint8_t cmd = 0xAAu;
    uint8_t st = 0xAAu;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_default_rsp(&g, &cmd, &st));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_IAS_ENROLL_RESPONSE, cmd);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, st);

    /* An unsupported cluster command. */
    static const uint8_t u[] = {0x18, 0x06, 0x0B, 0x02, 0x81};
    g = round_trip(u, sizeof(u), ZSRC_ZB_ZCL_IAS_ZONE, true);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_default_rsp(&g, &cmd, &st));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_UNSUP_COMMAND, st);
}

/* ZB-003: manufacturer-specific frames keep their code through the header codec, and the IAS
 * decoders refuse them (their command ids are the manufacturer's) (derived: ZCL 2.4.1.1.3).
 * verified (layout): the header is frame control (0x04 = manufacturer specific), manufacturer
 * code u16 low byte first, tsn, command, as in herdsman's zcl.test.ts "ZclFrame to buffer write
 * rsp" (tested below); 0x115F is Lumi's code there too.
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
static void test_golden_manufacturer_specific(void)
{
    static const uint8_t rep[] = {0x1C, 0x5F, 0x11, 0x22, 0x0A, 0x05, 0xFF, 0x20, 0x01};
    zsrc_zb_zcl_t f = round_trip(rep, sizeof(rep), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_EQUAL_HEX16(0x115Fu, f.manufacturer);
    TEST_ASSERT_EQUAL_UINT16(0x22u, f.tsn);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_REPORT_ATTR, f.command);
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0xFF05u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 1u);

    static const uint8_t st[] = {0x0D, 0x5F, 0x11, 0x07, 0x00, 0x01, 0x00};
    f = round_trip(st, sizeof(st), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_HEX16(0x115Fu, f.manufacturer);
    zbz_ias_status_t s;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_status_change(&f, &s));
    zbz_ias_enroll_req_t req;
    f.command = ZBZ_IAS_ENROLL_REQUEST;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_enroll_request(&f, &req));
}

/* ---- frames copied from the open projects' own tests ------------------------------------------- */

/* A client's request as herdsman's tests build it from the server side: the same bytes, the
 * direction bit cleared in the decoded frame (the payload is what these vectors check). */
static zsrc_zb_zcl_t as_request(zsrc_zb_zcl_t f)
{
    f.server_to_client = false;
    return f;
}

/* BL-143 (testing.md §3): byte strings taken from zigpy's and zigbee-herdsman's tests (permalinks
 * at each). Each decodes, re-encodes to the same bytes (round_trip) and reads back the fields the
 * project's test asserts. Where our decoder refuses on purpose it says so. */
static void test_golden_project_vectors(void)
{
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zbz_ias_status_t s;

    /* zigpy test_ias_zone: b"\tK\x00&\x00\x00\x00\x00\x00", a Zone Status Change Notification
     * with status 0x0026 (alarm 2, tamper, restore reports), no extended status, zone id 0,
     * delay 0, tsn 0x4B, Default Response not disabled.
     * https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_zcl_clusters.py */
    static const uint8_t zigpy_status[] = {0x09, 0x4B, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00};
    zsrc_zb_zcl_t f = round_trip(zigpy_status, sizeof(zigpy_status), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_TRUE(f.server_to_client);
    TEST_ASSERT_TRUE(f.want_response);
    TEST_ASSERT_EQUAL_UINT16(0x4Bu, f.tsn);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&f, &s));
    TEST_ASSERT_EQUAL_HEX16(0x0026u, s.status);
    TEST_ASSERT_EQUAL_UINT8(0u, s.extended);
    TEST_ASSERT_EQUAL_UINT8(0u, s.zone_id);
    TEST_ASSERT_EQUAL_UINT16(0u, s.delay_qs);

    /* herdsman foundation "read": zclVersion (0x0000) and modelId (0x0005), Default Response
     * disabled, tsn 0x0A.
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
    static const uint8_t h_read[] = {0x10, 0x0A, 0x00, 0x00, 0x00, 0x05, 0x00};
    f = round_trip(h_read, sizeof(h_read), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_FALSE(f.server_to_client);
    TEST_ASSERT_FALSE(f.want_response);
    TEST_ASSERT_EQUAL_UINT16(0x0Au, f.tsn);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_READ_ATTR, f.command);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0000u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0005u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman foundation "readRsp": zclVersion = uint8 8 and manufacturerName = "efgh".
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
    static const uint8_t h_rsp[] = {0x18, 0x0A, 0x01, 0x00, 0x00, 0x00, 0x20, 0x08, 0x04,
                                    0x00, 0x00, 0x42, 0x04, 0x65, 0x66, 0x67, 0x68};
    f = round_trip(h_rsp, sizeof(h_rsp), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_TRUE(f.server_to_client);
    TEST_ASSERT_FALSE(f.want_response);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0000u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, a.status);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_U8, a.type);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 8u);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0004u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_STRING, a.type);
    TEST_ASSERT_EQUAL_UINT8(4u, a.len);
    TEST_ASSERT_EQUAL_MEMORY("efgh", a.value, 4);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman zcl.test.ts "readRsp success" and "readRsp failed": attribute 1 = uint8 3, then
     * attribute 1 with status 2 and no type or value; Default Response not disabled (0x08).
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
    static const uint8_t h_ok[] = {0x08, 0x01, 0x01, 0x01, 0x00, 0x00, 0x20, 0x03};
    f = round_trip(h_ok, sizeof(h_ok), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_TRUE(f.want_response);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0001u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 3u);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);
    static const uint8_t h_fail[] = {0x08, 0x01, 0x01, 0x01, 0x00, 0x02};
    f = round_trip(h_fail, sizeof(h_fail), ZSRC_ZB_ZCL_BASIC, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0001u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(0x02u, a.status);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_NONE, a.type);
    TEST_ASSERT_NULL(a.value);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman foundation "report" (uint8 8 and "efgh") and zcl.test.ts "from buffer report"
     * (attribute 0x0055, data type 0x39 single precision, value 0).
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
    static const uint8_t h_rep[] = {0x18, 0x0A, 0x0A, 0x00, 0x00, 0x20, 0x08, 0x04,
                                    0x00, 0x42, 0x04, 0x65, 0x66, 0x67, 0x68};
    f = round_trip(h_rep, sizeof(h_rep), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_REPORT_ATTR, f.command);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0000u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 8u);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0004u, a.attr);
    TEST_ASSERT_EQUAL_MEMORY("efgh", a.value, 4);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);
    static const uint8_t h_float[] = {0x18, 0x4A, 0x0A, 0x55, 0x00, 0x39, 0x00, 0x00, 0x00, 0x00};
    f = round_trip(h_float, sizeof(h_float), ZSRC_ZB_ZCL_BASIC, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0055u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(0x39u, a.type);
    TEST_ASSERT_EQUAL_UINT8(4u, a.len);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman foundation "write": two uint16 records, 0x4001 = 8 and 0x4002 = 4.
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
    static const uint8_t h_write[] = {0x18, 0x0A, 0x02, 0x01, 0x40, 0x21, 0x08,
                                      0x00, 0x02, 0x40, 0x21, 0x04, 0x00};
    f = round_trip(h_write, sizeof(h_write), ZSRC_ZB_ZCL_ON_OFF, true);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_WRITE_ATTR, f.command);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x4001u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_U16, a.type);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 8u);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x4002u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_attr_uint(&a) == 4u);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman foundation "writeRsp": two refusals, status 0x86 for 0x4001 and 0x7E for 0x4002
     * (a Write Attributes Response record has no direction byte).
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
    static const uint8_t h_wrsp[] = {0x18, 0x0A, 0x04, 0x86, 0x01, 0x40, 0x7E, 0x02, 0x40};
    f = round_trip(h_wrsp, sizeof(h_wrsp), ZSRC_ZB_ZCL_ON_OFF, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(0x86u, a.status);
    TEST_ASSERT_EQUAL_HEX16(0x4001u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(0x7Eu, a.status);
    TEST_ASSERT_EQUAL_HEX16(0x4002u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman zcl.test.ts "to buffer write rsp": a manufacturer-specific frame (0x14: manufacturer
     * code, Default Response disabled, from the client) of Lumi's code 0x115F, tsn 0x0F, one
     * refusal, status 0x11 for attribute 0x0022.
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
    static const uint8_t h_mfr[] = {0x14, 0x5F, 0x11, 0x0F, 0x04, 0x11, 0x22, 0x00};
    f = round_trip(h_mfr, sizeof(h_mfr), ZSRC_ZB_ZCL_BASIC, true);
    TEST_ASSERT_EQUAL_HEX16(0x115Fu, f.manufacturer);
    TEST_ASSERT_EQUAL_UINT16(0x0Fu, f.tsn);
    TEST_ASSERT_FALSE(f.server_to_client);
    TEST_ASSERT_FALSE(f.want_response);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(0x11u, a.status);
    TEST_ASSERT_EQUAL_HEX16(0x0022u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman zcl.test.ts "defaultRsp": the answer to command 0x0C with status 0x82 (an
     * unsupported general command).
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
    static const uint8_t h_def[] = {0x18, 0x04, 0x0B, 0x0C, 0x82};
    f = round_trip(h_def, sizeof(h_def), ZSRC_ZB_ZCL_BASIC, true);
    uint8_t cmd = 0xAAu;
    uint8_t st = 0xAAu;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_default_rsp(&f, &cmd, &st));
    TEST_ASSERT_EQUAL_HEX8(0x0Cu, cmd);
    TEST_ASSERT_EQUAL_HEX8(0x82u, st);

    /* herdsman foundation "configReportRsp" and zcl.test.ts "configReportRsp - long": two
     * refusals, each status, direction 1 (the device's side), attribute id.
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
    static const uint8_t h_crsp[] = {0x08, 0x01, 0x07, 0x02, 0x01, 0x34,
                                     0x12, 0x01, 0x01, 0x35, 0x12};
    f = round_trip(h_crsp, sizeof(h_crsp), ZSRC_ZB_ZCL_ON_OFF, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(0x02u, a.status);
    TEST_ASSERT_EQUAL_HEX16(0x1234u, a.attr);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX8(0x01u, a.status);
    TEST_ASSERT_EQUAL_HEX16(0x1235u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);
    /* The same file's "handles SUCCESS payload": a lone 00 is the whole answer. */
    static const uint8_t h_csucc[] = {0x18, 0x0A, 0x07, 0x00};
    f = round_trip(h_csucc, sizeof(h_csucc), ZSRC_ZB_ZCL_ON_OFF, true);
    zbz_zcl_iter(&it, &f);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(ZBZ_ZCL_ATTR_ALL, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, a.status);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);

    /* herdsman foundation "configReport": three records in one frame - 0x4001 uint16 (data type
     * 0x21) every 300..4000 s with change 3; 0x0000 boolean (0x10) every 250..5290 s, no change
     * field; and a third of direction 1, a timeout of 3000 s for 0x4003. Our decoder reads the
     * panel's own requests only: the first two records decode, the third (a device's timeout, never
     * sent by the panel) refuses with ERR_VALUE after them.
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zspec/zcl/foundation.test.ts */
    static const uint8_t h_cfg[] = {0x18, 0x0A, 0x06, 0x00, 0x01, 0x40, 0x21, 0x2C, 0x01, 0xA0,
                                    0x0F, 0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0xFA, 0x00, 0xAA,
                                    0x14, 0x01, 0x03, 0x40, 0xB8, 0x0B};
    f = as_request(round_trip(h_cfg, sizeof(h_cfg), ZSRC_ZB_ZCL_ON_OFF, true));
    zsrc_zb_reporting_t back[4];
    uint8_t n = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zbz_zcl_decode_config_report(&f, back, 4, &n));
    TEST_ASSERT_EQUAL_UINT8(2u, n);
    TEST_ASSERT_EQUAL_HEX16(0x4001u, back[0].attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_U16, back[0].type);
    TEST_ASSERT_EQUAL_UINT16(300u, back[0].min_s);
    TEST_ASSERT_EQUAL_UINT16(4000u, back[0].max_s);
    TEST_ASSERT_EQUAL_UINT32(3u, back[0].change);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, back[1].attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_BOOL, back[1].type);
    TEST_ASSERT_EQUAL_UINT16(250u, back[1].min_s);
    TEST_ASSERT_EQUAL_UINT16(5290u, back[1].max_s);
    TEST_ASSERT_EQUAL_UINT32(0u, back[1].change);

    /* herdsman zcl.test.ts "configReportRsp client to server analog" (the title is the project's;
     * the command is 0x06, a Configure Reporting): uint8 attribute 0x0100 every 1..10 s with
     * change 20, and a data8 (discrete) one without a change.
     * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
    static const uint8_t h_an[] = {0x08, 0x01, 0x06, 0x00, 0x00, 0x01, 0x20, 0x01, 0x00, 0x0A,
                                   0x00, 0x14};
    f = as_request(round_trip(h_an, sizeof(h_an), ZSRC_ZB_ZCL_ON_OFF, true));
    n = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_config_report(&f, back, 4, &n));
    TEST_ASSERT_EQUAL_UINT8(1u, n);
    TEST_ASSERT_EQUAL_HEX16(0x0100u, back[0].attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_U8, back[0].type);
    TEST_ASSERT_EQUAL_UINT16(1u, back[0].min_s);
    TEST_ASSERT_EQUAL_UINT16(10u, back[0].max_s);
    TEST_ASSERT_EQUAL_UINT32(20u, back[0].change);
    static const uint8_t h_dis[] = {0x08, 0x01, 0x06, 0x00, 0x00, 0x01, 0x08, 0x01, 0x00, 0x0A,
                                    0x00};
    f = as_request(round_trip(h_dis, sizeof(h_dis), ZSRC_ZB_ZCL_ON_OFF, true));
    n = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_config_report(&f, back, 4, &n));
    TEST_ASSERT_EQUAL_UINT8(1u, n);
    TEST_ASSERT_EQUAL_HEX16(0x0100u, back[0].attr);
    TEST_ASSERT_EQUAL_HEX8(0x08u, back[0].type);
    TEST_ASSERT_EQUAL_UINT16(1u, back[0].min_s);
    TEST_ASSERT_EQUAL_UINT16(10u, back[0].max_s);
    TEST_ASSERT_EQUAL_UINT32(0u, back[0].change);
}

/* ---- what a frame can get wrong ---------------------------------------------------------------- */

/* ZB-003: shorter than the header. */
static void test_decode_short(void)
{
    static const uint8_t b[] = {0x05, 0x5F, 0x11, 0x07, 0x00};
    zsrc_zb_zcl_t f;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode(NULL, 0, 0, &f));
    for (size_t n = 0; n < ZBZ_ZCL_HEADER_MIN; n++) {
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode(b, n, 0, &f));
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode_global(b, n, 0, &f));
    }
    /* The manufacturer code takes two more. */
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode(b, 3, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode(b, 4, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(b, 5, 0, &f));
    TEST_ASSERT_EQUAL_UINT8(0u, f.len);
    TEST_ASSERT_FALSE(zbz_zcl_is_global(NULL, 0));
}

/* ZB-003: a payload past ZSRC_ZB_ZCL_PAYLOAD_MAX is refused; exactly the most is taken. */
static void test_decode_long(void)
{
    uint8_t b[ZBZ_ZCL_HEADER_MAX + ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u];
    memset(b, 0x5A, sizeof(b));
    b[0] = 0x01;
    zsrc_zb_zcl_t f;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(b, 3u + ZSRC_ZB_ZCL_PAYLOAD_MAX, 0, &f));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_ZCL_PAYLOAD_MAX, f.len);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG,
                          zbz_zcl_decode(b, 3u + ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u, 0, &f));
    TEST_ASSERT_EQUAL_UINT8(0u, f.len); /* nothing half-filled */
    b[0] = 0x05; /* with a manufacturer code: 5 + 48 is the longest */
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(b, sizeof(b) - 1u, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG, zbz_zcl_decode(b, sizeof(b), 0, &f));
}

/* ZB-003: the frame type and reserved bits. */
static void test_decode_frame_type(void)
{
    zsrc_zb_zcl_t f;
    static const uint8_t global[] = {0x00, 0x01, 0x00};
    static const uint8_t cluster[] = {0x01, 0x01, 0x00};
    static const uint8_t type2[] = {0x02, 0x01, 0x00};
    static const uint8_t type3[] = {0x03, 0x01, 0x00};
    static const uint8_t reserved[] = {0x21, 0x01, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode(global, 3, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode_global(cluster, 3, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode(type2, 3, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode_global(type2, 3, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode(type3, 3, 0, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zbz_zcl_decode(reserved, 3, 0, &f));
    TEST_ASSERT_FALSE(zbz_zcl_is_global(cluster, 3));
    TEST_ASSERT_FALSE(zbz_zcl_is_global(type2, 3));
}

/* ZB-003: the encoder never writes past `cap` or a payload's most, and keeps a request's
 * sequence number. */
static void test_encode_bounds(void)
{
    zsrc_zb_zcl_t f;
    zbz_ias_enroll_response(ZBZ_IAS_ENROLL_OK, 5, &f);
    uint8_t out[ZBZ_ZCL_FRAME_MAX];
    TEST_ASSERT_EQUAL_size_t(0u, zbz_zcl_encode(&f, 1, out, 4));
    TEST_ASSERT_EQUAL_size_t(5u, zbz_zcl_encode(&f, 1, out, 5));
    f.manufacturer = 0x1234u;
    TEST_ASSERT_EQUAL_size_t(0u, zbz_zcl_encode(&f, 1, out, 6));
    TEST_ASSERT_EQUAL_size_t(7u, zbz_zcl_encode(&f, 1, out, 7));
    TEST_ASSERT_EQUAL_HEX8(0x05u, out[0]);
    f.manufacturer = 0;
    f.tsn = 0x42u; /* an answer keeps the request's */
    TEST_ASSERT_EQUAL_size_t(5u, zbz_zcl_encode(&f, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x42u, out[1]);
    f.tsn = 0x100u;
    TEST_ASSERT_EQUAL_size_t(0u, zbz_zcl_encode(&f, 1, out, sizeof(out)));
    f.tsn = ZSRC_ZB_TSN_NEW;
    f.len = ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u;
    TEST_ASSERT_EQUAL_size_t(0u, zbz_zcl_encode(&f, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0u, zbz_zcl_encode_global(&f, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0u, zbz_zcl_encode(NULL, 1, out, sizeof(out)));
}

/* Walks `b`'s records to the end; the reason it stopped. */
static zbz_zcl_result_t walk(const uint8_t *b, size_t n, size_t *records)
{
    zsrc_zb_zcl_t f;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(b, n, 0, &f));
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zbz_zcl_iter(&it, &f);
    *records = 0;
    while (zbz_zcl_next_attr(&it, &a)) {
        (*records)++;
    }
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a)); /* and stays stopped */
    return it.err;
}

/* ZB-003, ZB-012: attribute records cut short, a length past the end, data types the codec can't
 * size, and commands without records. */
static void test_records_bad(void)
{
    size_t n = 0;
    static const uint8_t odd_read[] = {0x00, 0x01, 0x00, 0x04, 0x00, 0x05};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(odd_read, sizeof(odd_read), &n));
    TEST_ASSERT_EQUAL_size_t(1u, n);
    static const uint8_t no_status[] = {0x18, 0x01, 0x01, 0x04, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(no_status, sizeof(no_status), &n));
    static const uint8_t no_type[] = {0x18, 0x01, 0x01, 0x04, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(no_type, sizeof(no_type), &n));
    static const uint8_t cut_u16[] = {0x18, 0x01, 0x01, 0x04, 0x00, 0x00, 0x21, 0x01};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(cut_u16, sizeof(cut_u16), &n));
    static const uint8_t str_past[] = {0x18, 0x01, 0x01, 0x04, 0x00, 0x00, 0x42, 0x05, 0x41, 0x43};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(str_past, sizeof(str_past), &n));
    TEST_ASSERT_EQUAL_size_t(0u, n);
    static const uint8_t long_str_past[] = {0x18, 0x01, 0x0A, 0x04, 0x00, 0x44, 0x09, 0x00, 0x41};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(long_str_past, sizeof(long_str_past), &n));
    static const uint8_t report_cut[] = {0x18, 0x22, 0x0A, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(report_cut, sizeof(report_cut), &n));
    static const uint8_t lone_fail[] = {0x18, 0x57, 0x04, 0x87};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(lone_fail, sizeof(lone_fail), &n));
    static const uint8_t status_cut[] = {0x18, 0x57, 0x04, 0x87, 0x10};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, walk(status_cut, sizeof(status_cut), &n));

    /* An array (0x48), a structure (0x4C) and an unassigned id: the records after can't be
     * found, so the walk stops there. */
    static const uint8_t array[] = {0x18, 0x22, 0x0A, 0x00, 0x00, 0x20, 0x01,
                                    0x01, 0x00, 0x48, 0x20, 0x01, 0x00, 0x07};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_DATA_TYPE, walk(array, sizeof(array), &n));
    TEST_ASSERT_EQUAL_size_t(1u, n);
    static const uint8_t structure[] = {0x18, 0x22, 0x0A, 0x00, 0x00, 0x4C, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_DATA_TYPE, walk(structure, sizeof(structure), &n));
    static const uint8_t unassigned[] = {0x18, 0x22, 0x0A, 0x00, 0x00, 0x77, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_DATA_TYPE, walk(unassigned, sizeof(unassigned), &n));

    /* An invalid string (length 0xFF) is "", and the walk goes on. */
    static const uint8_t invalid_str[] = {0x18, 0x01, 0x01, 0x04, 0x00, 0x00, 0x42, 0xFF,
                                          0x05, 0x00, 0x00, 0x20, 0x09};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, walk(invalid_str, sizeof(invalid_str), &n));
    TEST_ASSERT_EQUAL_size_t(2u, n);

    /* Default Response and Configure Reporting have no attribute records of this layout. */
    static const uint8_t dflt[] = {0x18, 0x05, 0x0B, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, walk(dflt, sizeof(dflt), &n));
    TEST_ASSERT_EQUAL_size_t(0u, n);

    /* An empty Read Attributes has no records and no error. */
    static const uint8_t empty[] = {0x00, 0x01, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, walk(empty, sizeof(empty), &n));
    TEST_ASSERT_EQUAL_size_t(0u, n);
}

/* ZB-003: building records: wrong lengths, types the codec can't size, a full payload. */
static void test_put_attr_bad(void)
{
    zsrc_zb_zcl_t f;
    const uint8_t v[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    zbz_zcl_global(ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_WRITE_ATTR, false, &f);
    zbz_zcl_attr_t a = {.attr = 1, .type = ZBZ_ZCL_TYPE_U16, .len = 1, .value = v};
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a)); /* a uint16 is 2 bytes */
    a.type = 0x48u;
    a.len = 2;
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a)); /* an array */
    a.type = ZBZ_ZCL_TYPE_U16;
    a.value = NULL;
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a)); /* bytes promised, none given */
    TEST_ASSERT_EQUAL_UINT8(0u, f.len);

    /* 11-byte records (attr, type, 8 bytes): four fit in 48, the fifth doesn't. */
    a = (zbz_zcl_attr_t){.attr = 1, .type = ZBZ_ZCL_TYPE_EUI64, .len = 8, .value = v};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &a));
    }
    TEST_ASSERT_EQUAL_UINT8(44u, f.len);
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a));
    TEST_ASSERT_EQUAL_UINT8(44u, f.len);
    a = (zbz_zcl_attr_t){.attr = 2, .type = ZBZ_ZCL_TYPE_STRING, .len = 2, .value = v};
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a)); /* 2 + 1 + 1 + 2 > 4 left */
    a.len = 0;
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &a)); /* attr, type, length 0: 4 bytes */
    TEST_ASSERT_EQUAL_UINT8(48u, f.len);

    /* Commands without attribute records. */
    zbz_zcl_default_rsp(0, 0, 0, false, &f);
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a));
    zbz_ias_test_mode(1, 0, &f);
    f.command = ZBZ_ZCL_CONFIG_REPORT;
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &a));

    /* A lone SUCCESS only as the whole answer, and only a success. */
    zbz_zcl_global(0, ZBZ_ZCL_WRITE_ATTR_RSP, true, &f);
    const zbz_zcl_attr_t fail_all = {.attr = ZBZ_ZCL_ATTR_ALL, .status = ZBZ_ZCL_FAILURE};
    TEST_ASSERT_FALSE(zbz_zcl_put_attr(&f, &fail_all));
}

/* ZB-012: Configure Reporting that can't be built or read. */
static void test_config_report_bad(void)
{
    zsrc_zb_zcl_t f;
    const zsrc_zb_reporting_t mixed[2] = {
        {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x21, .type = ZBZ_ZCL_TYPE_U8},
        {.cluster = ZSRC_ZB_ZCL_TEMP_MEAS, .attr = 0x00, .type = ZBZ_ZCL_TYPE_S16}};
    TEST_ASSERT_FALSE(zbz_zcl_config_report(mixed, 2, &f));
    TEST_ASSERT_FALSE(zbz_zcl_config_report(mixed, 0, &f));
    const zsrc_zb_reporting_t arr = {.cluster = 1, .type = 0x48u};
    TEST_ASSERT_FALSE(zbz_zcl_config_report(&arr, 1, &f));
    /* 12-byte records (uint32 and its change): four fit, five don't. */
    zsrc_zb_reporting_t many[5];
    for (size_t i = 0; i < 5; i++) {
        many[i] = (zsrc_zb_reporting_t){.cluster = 1, .attr = (uint16_t)i,
                                        .type = ZBZ_ZCL_TYPE_U32, .change = 1};
    }
    TEST_ASSERT_TRUE(zbz_zcl_config_report(many, 4, &f));
    TEST_ASSERT_EQUAL_UINT8(48u, f.len);
    TEST_ASSERT_FALSE(zbz_zcl_config_report(many, 5, &f));

    zsrc_zb_reporting_t back[4];
    uint8_t n = 0;
    TEST_ASSERT_TRUE(zbz_zcl_config_report(many, 2, &f));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG, zbz_zcl_decode_config_report(&f, back, 1, &n));
    TEST_ASSERT_EQUAL_UINT8(1u, n);

    zsrc_zb_zcl_t g;
    static const uint8_t dir1[] = {0x00, 0x01, 0x06, 0x01, 0x21, 0x00, 0x10, 0x0E};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(dir1, sizeof(dir1), 1, &g));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zbz_zcl_decode_config_report(&g, back, 4, &n));
    static const uint8_t bad_type[] = {0x00, 0x01, 0x06, 0x00, 0x21, 0x00, 0x48,
                                       0x10, 0x0E, 0x60, 0x54};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(bad_type, sizeof(bad_type), 1, &g));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_DATA_TYPE, zbz_zcl_decode_config_report(&g, back, 4, &n));
    static const uint8_t cut[] = {0x00, 0x01, 0x06, 0x00, 0x21, 0x00, 0x20, 0x10, 0x0E, 0x60, 0x54};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(cut, sizeof(cut), 1, &g));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode_config_report(&g, back, 4, &n));
    static const uint8_t cut_hdr[] = {0x00, 0x01, 0x06, 0x00, 0x21};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(cut_hdr, sizeof(cut_hdr), 1, &g));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode_config_report(&g, back, 4, &n));
    /* A uint40's change past 32 bits. */
    static const uint8_t wide[] = {0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x24, 0x01, 0x00,
                                   0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(wide, sizeof(wide), 1, &g));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zbz_zcl_decode_config_report(&g, back, 4, &n));
    /* Not a Configure Reporting, or the device's direction. */
    zbz_zcl_default_rsp(1, 0, 0, false, &g);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode_config_report(&g, back, 4, &n));
    TEST_ASSERT_TRUE(zbz_zcl_config_report(many, 1, &g));
    g.server_to_client = true;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode_config_report(&g, back, 4, &n));
}

/* ZB-003: the IAS decoders and the Default Response refuse what isn't theirs or is cut short. */
static void test_ias_bad(void)
{
    zsrc_zb_zcl_t f;
    zbz_ias_enroll_req_t req;
    zbz_ias_status_t s;

    /* An Enroll Request in the panel's direction is not one. */
    static const uint8_t wrong_dir[] = {0x01, 0x57, 0x01, 0x15, 0x00, 0x15, 0x10};
    f = round_trip(wrong_dir, sizeof(wrong_dir), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_enroll_request(&f, &req));
    /* ... nor one of another cluster, nor a status change. */
    static const uint8_t req_ok[] = {0x09, 0x57, 0x01, 0x15, 0x00, 0x15, 0x10};
    f = round_trip(req_ok, sizeof(req_ok), ZSRC_ZB_ZCL_IAS_WD, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_enroll_request(&f, &req));
    f = round_trip(req_ok, sizeof(req_ok), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_status_change(&f, &s));
    /* Cut short. */
    static const uint8_t req_cut[] = {0x09, 0x57, 0x01, 0x15, 0x00, 0x15};
    f = round_trip(req_cut, sizeof(req_cut), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_ias_decode_enroll_request(&f, &req));
    /* A status change in the panel's direction. */
    static const uint8_t st_dir[] = {0x01, 0x0A, 0x00, 0x01, 0x00};
    f = round_trip(st_dir, sizeof(st_dir), ZSRC_ZB_ZCL_IAS_ZONE, false);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_status_change(&f, &s));
    /* A length a decoder can't trust. */
    f = round_trip(req_ok, sizeof(req_ok), ZSRC_ZB_ZCL_IAS_ZONE, false);
    f.len = ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG, zbz_ias_decode_enroll_request(&f, &req));

    uint8_t cmd = 0;
    uint8_t st = 0;
    static const uint8_t dflt_cut[] = {0x18, 0x05, 0x0B, 0x00};
    f = round_trip(dflt_cut, sizeof(dflt_cut), 0, true);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbz_zcl_decode_default_rsp(&f, &cmd, &st));
    f.command = ZBZ_ZCL_REPORT_ATTR;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_zcl_decode_default_rsp(&f, &cmd, &st));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_enroll_request);
    RUN_TEST(test_golden_enroll_response);
    RUN_TEST(test_golden_cie_write);
    RUN_TEST(test_golden_status_change);
    RUN_TEST(test_golden_test_mode);
    RUN_TEST(test_golden_ace_arm_header);
    RUN_TEST(test_golden_read_attributes);
    RUN_TEST(test_golden_numbers);
    RUN_TEST(test_golden_write_response);
    RUN_TEST(test_golden_configure_reporting);
    RUN_TEST(test_golden_default_response);
    RUN_TEST(test_golden_manufacturer_specific);
    RUN_TEST(test_golden_project_vectors);
    RUN_TEST(test_decode_short);
    RUN_TEST(test_decode_long);
    RUN_TEST(test_decode_frame_type);
    RUN_TEST(test_encode_bounds);
    RUN_TEST(test_records_bad);
    RUN_TEST(test_put_attr_bad);
    RUN_TEST(test_config_report_bad);
    RUN_TEST(test_ias_bad);
    return UNITY_END();
}
