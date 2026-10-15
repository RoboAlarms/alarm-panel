/*
 * test_zbz_ace.c - Zigbee keypads and key fobs arming and disarming the panel (zbz_ace.h;
 * docs/features/20 alarm-devices.md §3-§4, testing.md §3, §9 and §12). Every test proves ZB-024.
 *
 * The policy runs against the real engine (alarm_sim): the tests are the "alarm task", running
 * each post with alarm_engine_command (a keypad, its code) or alarm_engine_command_as (a fob, its
 * user), then giving the result back by ticket - so the codes, the lockout and duress are the
 * engine's own, never compared in zbz_ace.c.
 *
 * Where the frame bytes come from, per vector (cross-checked against the open projects on
 * 2026-10-03, BL-143; the links are permalinks to the commits named):
 * - "herdsman": zigbee-herdsman's zcl test frame for IAS ACE Arm (testing.md §3), verified
 *   (bytes): the test "ZclFrame from buffer ssIasAce arm command" at 03495312e131,
 *   https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts
 * - "testing.md §3": the table there. "verified (layout)": the command ids, field order, widths
 *   and header bits match zigpy's security.py at fb63170d520e
 *   (https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py) and what the
 *   other projects send or parse, but no project holds the exact byte string. The panel's frames
 *   carry the frame control 0x19, not 0x09: they ask for no Default Response (decided in ZB-M11,
 *   alarm-devices.md §3 "Frames"). Confirmed: deCONZ's Arm Response is ClusterCommand |
 *   ServerToClient | DisableDefaultResponse (ias_ace.cpp at a4c17adfa04a), herdsman's
 *   Endpoint.commandResponse defaults to server to client with the Default Response disabled
 *   (src/controller/model/endpoint.ts at 03495312e131) and zigpy's Cluster.reply sets
 *   disable_default_response (zigpy/zcl/__init__.py). The exception is deCONZ's Get Panel Status
 *   Response, sent with 0x09 (the Default Response not disabled).
 * - "derived": built here from the ZCL specification's IAS ACE fields (ZCL rev 8, 07-5123, 8.3:
 *   the commands' payloads; 2.6.2 the string type). To be cross-checked against Wireshark's ZCL
 *   dissector on the bench, as testing.md's derived rows.
 * Addresses are fictional and locally administered (0x02AB_CD00_0000_00xx); users Alex (3), Pat
 * (4) as the simulator's users 3 and 4.
 */
#include <stdint.h>
#include <string.h>

#include "sim.h"
#include "zigbee_zones/zbz_ace.h"
#include "zigbee_zones/zbz_models.h"
#include "zigbee_zones/zbz_zcl.h"

#include "ace_fixture_v1.h"

#define KP1_IEEE 0x02ABCD0000000011ull
#define KP2_IEEE 0x02ABCD0000000012ull
#define FOB_IEEE 0x02ABCD0000000021ull
#define ODD_IEEE 0x02ABCD00000000EEull
#define KP1_EP   44u
#define FOB_EP   1u
#define KP1_ID   3u
#define KP2_ID   7u
#define FOB_ID   9u
#define ZWAVE_KEYPAD_DEVICE 9u /* zw_keyctl.h ZWKC_DEVICE(0) */

typedef struct {
    uint64_t ieee;
    uint8_t endpoint;
    zsrc_zb_zcl_t f;
    uint32_t tag;
} sent_t;

typedef struct {
    uint64_t ieee;
    uint8_t endpoint, tsn, command, status;
} drsp_t;

static sim_t S;
static zba_t A;
static sent_t SENT[64];
static unsigned NSENT;
static drsp_t DRSP[64];
static unsigned NDRSP;
static bool SEND_FAILS;
static uint64_t NOW;

static bool port_send(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                      uint32_t tag)
{
    (void)ctx;
    if (SEND_FAILS) {
        return false;
    }
    TEST_ASSERT_TRUE(NSENT < sizeof(SENT) / sizeof(SENT[0]));
    TEST_ASSERT_EQUAL_HEX32(ZBA_TAG, tag & ZBA_TAG_MASK);
    SENT[NSENT++] = (sent_t){ieee, endpoint, *cmd, tag};
    return true;
}

static bool port_default_rsp(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t tsn,
                             uint8_t command, uint8_t status, uint32_t tag)
{
    (void)ctx;
    TEST_ASSERT_TRUE(NDRSP < sizeof(DRSP) / sizeof(DRSP[0]));
    TEST_ASSERT_EQUAL_HEX32(ZBA_TAG, tag & ZBA_TAG_MASK);
    DRSP[NDRSP++] = (drsp_t){ieee, endpoint, tsn, command, status};
    return true;
}

static const zba_port_t PORT = {.ctx = NULL, .send = port_send, .default_rsp = port_default_rsp};

static zba_keypad_t keypad1(void)
{
    return (zba_keypad_t){.id = KP1_ID, .ieee = KP1_IEEE, .endpoint = KP1_EP, .partition = 1,
                          .panic_mask = (uint8_t)((1u << ALARM_PANIC_POLICE) |
                                                  (1u << ALARM_PANIC_FIRE)),
                          .show_state = true, .device = 1};
}

static zba_fob_t fob1(void)
{
    return (zba_fob_t){.id = FOB_ID, .ieee = FOB_IEEE, .endpoint = FOB_EP, .user = 3,
                       .partition = 1};
}

/* The alarm task's news: the partition's view as the engine has it now. */
static void push(uint8_t p)
{
    alarm_partition_status_t st = sim_status(&S, p);
    zba_view_t v = {.state = (uint8_t)st.state, .level = (uint8_t)st.level, .ready = st.ready,
                    .delay_s = (uint16_t)((st.delay_remaining_ms + 999u) / 1000u), .at = NOW};
    zba_panel(&A, NOW, p, &v);
}

void setUp(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    sim_start(&S, &cfg, NULL);
    zba_init(&A, &PORT);
    NSENT = 0;
    NDRSP = 0;
    SEND_FAILS = false;
    NOW = 1000000u;
    zba_keypad_t k = keypad1();
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    zba_fob_t f = fob1();
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &f));
    push(1);
    NSENT = 0;
}

void tearDown(void) {}

/* ---- helpers ------------------------------------------------------------------------------------ */

static zsrc_zb_zcl_t decode(const uint8_t *b, size_t n)
{
    zsrc_zb_zcl_t f;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(b, n, ZSRC_ZB_ZCL_IAS_ACE, &f));
    return f;
}

/* Encodes `f` with `tsn` (for a new sequence number): the golden bytes. */
static void encodes_to(const zsrc_zb_zcl_t *f, uint8_t tsn, const uint8_t *b, size_t n)
{
    uint8_t out[ZBZ_ZCL_FRAME_MAX];
    size_t m = zbz_zcl_encode(f, tsn, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(n, m);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, out, n);
}

/* A client frame of `command` with `payload`. */
static zsrc_zb_zcl_t client(uint8_t tsn, uint8_t command, const uint8_t *payload, uint8_t len)
{
    zsrc_zb_zcl_t f;
    memset(&f, 0, sizeof(f));
    f.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    f.command = command;
    f.want_response = true;
    f.tsn = tsn;
    f.len = len;
    if (len > 0u) {
        memcpy(f.payload, payload, len);
    }
    return f;
}

/* An Arm with `code` (NULL: none), zone id 0. */
static zsrc_zb_zcl_t arm(uint8_t tsn, uint8_t mode, const char *code)
{
    uint8_t p[ZSRC_ZB_ZCL_PAYLOAD_MAX];
    size_t n = code != NULL ? strlen(code) : 0u;
    p[0] = mode;
    p[1] = (uint8_t)n;
    if (n > 0u) {
        memcpy(&p[2], code, n);
    }
    p[2u + n] = 0;
    return client(tsn, ZBA_CMD_ARM, p, (uint8_t)(3u + n));
}

static const sent_t *last_sent(uint8_t command)
{
    for (unsigned i = NSENT; i > 0u; i--) {
        if (SENT[i - 1u].f.command == command) {
            return &SENT[i - 1u];
        }
    }
    return NULL;
}

static unsigned count_sent(uint8_t command)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NSENT; i++) {
        n += SENT[i].f.command == command ? 1u : 0u;
    }
    return n;
}

/* The Arm Response last sent: its note, with the request's sequence number checked. */
static uint8_t arm_note(uint8_t tsn)
{
    const sent_t *s = last_sent(ZBA_RSP_ARM);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->f.server_to_client);
    TEST_ASSERT_FALSE(s->f.want_response);
    TEST_ASSERT_EQUAL_UINT16(tsn, s->f.tsn);
    TEST_ASSERT_EQUAL_UINT8(1, s->f.len);
    return s->f.payload[0];
}

static zba_post_t POST;

/* Runs the post as the alarm task does, checks the code it carried, wipes it (the owner's job). */
static alarm_result_t run_post(const char *expect_code)
{
    alarm_cmd_result_t res;
    alarm_result_t r;
    TEST_ASSERT_EQUAL_STRING(expect_code != NULL ? expect_code : "", POST.cmd.code);
    if (POST.user != 0u) {
        r = alarm_engine_command_as(&S.eng, &POST.cmd, &POST.source, POST.user, S.now, &res);
    } else {
        r = alarm_engine_command(&S.eng, &POST.cmd, &POST.source, S.now, &res);
    }
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
    return r;
}

/* A frame from `ieee` that must be posted; run, answered, then the partition's news. */
static alarm_result_t act(uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *f,
                          const char *expect_code)
{
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, ieee, endpoint, f, &POST));
    alarm_result_t r = run_post(expect_code);
    alarm_cmd_result_t res = {0};
    TEST_ASSERT_TRUE(zba_result(&A, NOW, POST.ticket, r, &res));
    push(1);
    return r;
}

/* Whether `needle` is anywhere in the policy's state. */
static bool state_holds(const char *needle)
{
    const uint8_t *p = (const uint8_t *)&A;
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= sizeof(A); i++) {
        if (memcmp(p + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- golden frames (testing.md §3) ------------------------------------------------------------- */

/* ZB-024: the IAS ACE Arm, both ways (herdsman). verified (bytes): 1, 87, 0, 0, 6, 49..54, 0 is
 * herdsman's test frame, read as frame type 1, direction 0, Default Response not disabled, tsn 87,
 * {armmode 0, code "123456", zoneid 0}.
 * https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131/test/zcl.test.ts */
static void test_golden_arm(void)
{
    static const uint8_t b[] = {0x01, 0x57, 0x00, 0x00, 0x06, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
                                0x00};
    zsrc_zb_zcl_t f = decode(b, sizeof(b));
    zba_arm_t a;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_EQUAL_UINT8(ZBA_ARM_DISARM, a.mode);
    TEST_ASSERT_EQUAL_STRING("123456", a.code);
    TEST_ASSERT_FALSE(a.code_bad);
    TEST_ASSERT_EQUAL_UINT8(0, a.zone_id);
    TEST_ASSERT_EQUAL_UINT16(0x57, f.tsn);
    encodes_to(&f, 0x00, b, sizeof(b));
    /* A fob's Arm all zones with no code (testing.md §3). verified (layout): zigpy's "arm" is
     * arm_mode (enum8), arm_disarm_code (character string, length byte first, so empty is 00) and
     * zone_id (u8); zigpy/zha's alarm panel test sends [Arm_All_Zones, '', 0]; deCONZ reads the
     * code as a string "allowed to be empty". The exact bytes are in no project's test. A logged
     * zigbee-herdsman message in the ioBroker forum's IAS ACE thread (topic 49756, Linkind and
     * Immax keypads and fobs) reads {"armmode":3,"code":"","zoneid":23}: a real device carries
     * the zone id the CIE enrolled it with (herdsman assigns 23), not 0, so the policy must not
     * need it to be 0 (it doesn't: zba_decode_arm only reads it).
     * https://github.com/zigpy/zha/blob/f3eb54c0fa42/tests/test_alarm_control_panel.py */
    static const uint8_t fob[] = {0x01, 0x57, 0x00, 0x03, 0x00, 0x00};
    f = decode(fob, sizeof(fob));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_EQUAL_UINT8(ZBA_ARM_ALL, a.mode);
    TEST_ASSERT_EQUAL_STRING("", a.code);
    TEST_ASSERT_FALSE(a.code_bad);
    encodes_to(&f, 0x00, fob, sizeof(fob));
}

/* ZB-024: the panel's Arm Response and panel status frames (testing.md §3, frame control 0x19).
 * verified (layout), command by command (zigpy's client commands): 0x00 arm_response is one
 * ArmNotification byte (3 All_Zones_Armed, 4 Invalid_Arm_Disarm_Code; deCONZ writes one byte);
 * 0x04 panel_status_changed and 0x05 panel_status_response are panel status, seconds remaining,
 * audible notification, alarm status (PanelStatus Exit_Delay 4, In_Alarm 7; AudibleNotification
 * Default_Sound 1; AlarmStatus Emergency 3; zigpy/zha's alarm panel test calls command 4 with
 * seconds 0); 0x07 get_panel_status and the Emergency 0x02, Fire 0x03, Panic 0x04 requests have
 * no payload (zha's test sends command 4 with an empty list). deCONZ answers a Get Panel Status
 * with these four bytes, 0x01 audible and, in alarm, 0x03 (Emergency) - 07 00 01 03 as below - but
 * with frame control 0x09. No project's test holds a Panel Status Changed as bytes.
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py
 * https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/ias_ace.cpp
 * https://github.com/zigpy/zha/blob/f3eb54c0fa42/tests/test_alarm_control_panel.py */
static void test_golden_server_frames(void)
{
    zsrc_zb_zcl_t f;
    static const uint8_t armed[] = {0x19, 0x57, 0x00, 0x03};
    static const uint8_t invalid[] = {0x19, 0x57, 0x00, 0x04};
    zba_arm_response(ZBA_NOTE_ALL_ARMED, 0x57, &f);
    encodes_to(&f, 0x00, armed, sizeof(armed));
    zba_arm_response(ZBA_NOTE_INVALID_CODE, 0x57, &f);
    encodes_to(&f, 0x00, invalid, sizeof(invalid));

    static const uint8_t exit30[] = {0x19, 0x58, 0x04, 0x04, 0x1E, 0x01, 0x00};
    const zba_status_t s = {ZBA_PS_EXIT_DELAY, 30, ZBA_AUDIBLE_DEFAULT, ZBA_ALARM_NONE};
    zba_panel_status_changed(&s, &f);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_ZB_TSN_NEW, f.tsn);
    encodes_to(&f, 0x58, exit30, sizeof(exit30));
    zba_status_t back;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_panel_status(&f, &back));
    TEST_ASSERT_EQUAL_MEMORY(&s, &back, sizeof(s));

    static const uint8_t get[] = {0x01, 0x57, 0x07};
    f = decode(get, sizeof(get));
    zba_request_t q;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_request(&f, &q));
    TEST_ASSERT_EQUAL_UINT8(ZBA_CMD_GET_PANEL_STATUS, q.command);
    encodes_to(&f, 0x00, get, sizeof(get));

    static const uint8_t alarm[] = {0x19, 0x57, 0x05, 0x07, 0x00, 0x01, 0x03};
    const zba_status_t in_alarm = {ZBA_PS_IN_ALARM, 0, ZBA_AUDIBLE_DEFAULT, ZBA_ALARM_EMERGENCY};
    zba_panel_status_response(&in_alarm, 0x57, &f);
    encodes_to(&f, 0x00, alarm, sizeof(alarm));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_panel_status(&f, &back));
    TEST_ASSERT_EQUAL_MEMORY(&in_alarm, &back, sizeof(in_alarm));

    static const uint8_t buttons[3][3] = {{0x01, 0x57, 0x02}, {0x01, 0x57, 0x03},
                                          {0x01, 0x57, 0x04}};
    static const uint8_t ids[3] = {ZBA_CMD_EMERGENCY, ZBA_CMD_FIRE, ZBA_CMD_PANIC};
    for (size_t i = 0; i < 3u; i++) {
        f = decode(buttons[i], 3);
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_request(&f, &q));
        TEST_ASSERT_EQUAL_UINT8(ids[i], q.command);
    }
}

/* ZB-024: the questions and their answers (derived from ZCL 8.3's payloads). verified (layout):
 * zigpy's command table has the same order and widths for every one of them - bypass is a u8
 * count, the zone ids, then the code string; get_zone_info a u8 zone id; get_zone_status the
 * starting id, the maximum count, a boolean mask flag and a u16 mask; the zone id map response a
 * list of u16 sections (16 in the ZCL); zone info response zone id, zone type u16, EUI64, label
 * string; zone status
 * changed zone id, status u16, audible, label; set bypassed zone list a u8 count and the ids;
 * bypass response a u8 count and the results (Not_allowed is 0x02); zone status response a
 * boolean, a u8 count and (zone id, status u16) pairs. No project's test holds these as bytes.
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py */
static void test_golden_question_frames(void)
{
    zsrc_zb_zcl_t f;
    zba_request_t q;
    /* Bypass zones 5 and 6 with code 1234: the zones read, the code stepped over. */
    static const uint8_t bypass[] = {0x01, 0x30, 0x01, 0x02, 0x05, 0x06, 0x04, 0x31, 0x32, 0x33,
                                     0x34};
    f = decode(bypass, sizeof(bypass));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_request(&f, &q));
    TEST_ASSERT_EQUAL_UINT8(2, q.count);
    TEST_ASSERT_EQUAL_UINT8(5, q.zones[0]);
    TEST_ASSERT_EQUAL_UINT8(6, q.zones[1]);
    static const uint8_t info[] = {0x01, 0x31, 0x06, 0x05};
    f = decode(info, sizeof(info));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_request(&f, &q));
    TEST_ASSERT_EQUAL_UINT8(5, q.zone_id);
    static const uint8_t zstat[] = {0x01, 0x32, 0x09, 0x01, 0x10, 0x01, 0x01, 0x00};
    f = decode(zstat, sizeof(zstat));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_request(&f, &q));
    TEST_ASSERT_EQUAL_UINT8(1, q.zone_id);
    TEST_ASSERT_EQUAL_UINT8(16, q.max_zones);
    TEST_ASSERT_TRUE(q.mask_flag);
    TEST_ASSERT_EQUAL_HEX16(0x0001, q.status_mask);

    uint8_t map[3 + 32];
    memset(map, 0, sizeof(map));
    map[0] = 0x19;
    map[1] = 0x33;
    map[2] = 0x01;
    zba_zone_id_map_response(NULL, 0x33, &f);
    encodes_to(&f, 0x00, map, sizeof(map));
    const uint16_t sections[ZBA_ZONE_MAP_SECTIONS] = {0x0003};
    zba_zone_id_map_response(sections, 0x33, &f);
    TEST_ASSERT_EQUAL_HEX8(0x03, f.payload[0]);

    static const uint8_t none[] = {0x19, 0x31, 0x02, 0x05, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    const zba_zone_info_t z = {5, ZBA_ZONE_TYPE_NONE, ZBA_IEEE_NONE, NULL};
    zba_zone_info_response(&z, 0x31, &f);
    encodes_to(&f, 0x00, none, sizeof(none));

    static const uint8_t changed[] = {0x19, 0x60, 0x03, 0x01, 0x01, 0x00, 0x01, 0x04, 0x48, 0x61,
                                      0x6C, 0x6C};
    zba_zone_status_changed(1, 0x0001, ZBA_AUDIBLE_DEFAULT, "Hall", &f);
    encodes_to(&f, 0x60, changed, sizeof(changed));

    static const uint8_t bypassed[] = {0x19, 0x34, 0x06, 0x00};
    zba_set_bypassed(NULL, 0, 0x34, &f);
    encodes_to(&f, 0x00, bypassed, sizeof(bypassed));
    static const uint8_t refused[] = {0x19, 0x30, 0x07, 0x02, 0x02, 0x02};
    const uint8_t results[2] = {ZBA_BYPASS_NOT_ALLOWED, ZBA_BYPASS_NOT_ALLOWED};
    zba_bypass_response(results, 2, 0x30, &f);
    encodes_to(&f, 0x00, refused, sizeof(refused));
    static const uint8_t status_none[] = {0x19, 0x32, 0x08, 0x01, 0x00};
    zba_zone_status_response(true, NULL, 0, 0x32, &f);
    encodes_to(&f, 0x00, status_none, sizeof(status_none));
    static const uint8_t status_one[] = {0x19, 0x32, 0x08, 0x00, 0x01, 0x07, 0x05, 0x00};
    const zba_zone_status_t one = {7, 0x0005};
    zba_zone_status_response(false, &one, 1, 0x32, &f);
    encodes_to(&f, 0x00, status_one, sizeof(status_one));
}

/* ZB-024: every way a frame can be wrong (testing.md §3's negative cases). */
static void test_decode_failures(void)
{
    zba_arm_t a;
    /* A code length past the frame: "invalid code", never truncated. */
    static const uint8_t past[] = {0x00, 0x0A, 0x31, 0x32};
    zsrc_zb_zcl_t f = client(1, ZBA_CMD_ARM, past, sizeof(past));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_TRUE(a.code_bad);
    TEST_ASSERT_EQUAL_STRING("", a.code);
    /* Longer than any code the panel has, and not digits. */
    f = arm(1, ZBA_ARM_ALL, "12345678901");
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_TRUE(a.code_bad);
    TEST_ASSERT_EQUAL_STRING("", a.code);
    f = arm(1, ZBA_ARM_ALL, "12A4");
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_TRUE(a.code_bad);
    TEST_ASSERT_EQUAL_STRING("", a.code);
    f = arm(1, ZBA_ARM_ALL, "1234567890"); /* ALARM_CODE_MAX_LEN digits: fine */
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_FALSE(a.code_bad);
    /* An invalid string (0xFF): no code. A mode above 3 is read; the policy refuses it. */
    static const uint8_t ff[] = {0x07, 0xFF, 0x02};
    f = client(1, ZBA_CMD_ARM, ff, sizeof(ff));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_arm(&f, &a));
    TEST_ASSERT_EQUAL_UINT8(7, a.mode);
    TEST_ASSERT_EQUAL_STRING("", a.code);
    TEST_ASSERT_EQUAL_UINT8(2, a.zone_id);
    /* Shorter than its fields. */
    f = client(1, ZBA_CMD_ARM, past, 0);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_arm(&f, &a));
    f = client(1, ZBA_CMD_ARM, past, 1);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_arm(&f, &a));
    /* Not an ACE client's standard Arm. */
    f = arm(1, 0, "1234");
    f.manufacturer = 0x1015;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zba_decode_arm(&f, &a));
    f = arm(1, 0, "1234");
    f.server_to_client = true;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zba_decode_arm(&f, &a));
    f = arm(1, 0, "1234");
    f.cluster = ZSRC_ZB_ZCL_IAS_WD;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zba_decode_arm(&f, &a));
    f = arm(1, 0, "1234");
    f.len = ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG, zba_decode_arm(&f, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zba_decode_arm(NULL, &a));
    /* Longer than a payload never reaches this as bytes either (zbz_zcl). */
    uint8_t big[3 + ZSRC_ZB_ZCL_PAYLOAD_MAX + 1];
    memset(big, 0, sizeof(big));
    big[0] = 0x01;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG, zbz_zcl_decode(big, sizeof(big), ZSRC_ZB_ZCL_IAS_ACE,
                                                           &f));

    zba_request_t q;
    static const uint8_t bp_cut[] = {0x03, 0x05};          /* three zones, one there */
    static const uint8_t bp_nocode[] = {0x01, 0x05};       /* the code's length byte missing */
    static const uint8_t bp_code_past[] = {0x01, 0x05, 0x04, 0x31};
    f = client(2, ZBA_CMD_BYPASS, bp_cut, sizeof(bp_cut));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_request(&f, &q));
    f = client(2, ZBA_CMD_BYPASS, bp_nocode, sizeof(bp_nocode));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_request(&f, &q));
    f = client(2, ZBA_CMD_BYPASS, bp_code_past, sizeof(bp_code_past));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_request(&f, &q));
    f = client(2, ZBA_CMD_BYPASS, bp_cut, 0);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_request(&f, &q));
    f = client(2, ZBA_CMD_GET_ZONE_INFO, bp_cut, 0);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_request(&f, &q));
    f = client(2, ZBA_CMD_GET_ZONE_STATUS, bp_code_past, 4);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_request(&f, &q));
    f = client(2, ZBA_CMD_ARM, bp_code_past, 4);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zba_decode_request(&f, &q));
    f = client(2, 0x0A, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zba_decode_request(&f, &q));

    zba_status_t s;
    static const uint8_t bad_status[] = {0x0B, 0x00, 0x00, 0x00};
    static const uint8_t bad_alarm[] = {0x07, 0x00, 0x01, 0x07};
    static const uint8_t armed80[] = {0x80, 0x00, 0x00, 0x00};
    zsrc_zb_zcl_t g = client(3, ZBA_RSP_PANEL_STATUS, bad_status, 4);
    g.server_to_client = true;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zba_decode_panel_status(&g, &s));
    memcpy(g.payload, bad_alarm, 4);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zba_decode_panel_status(&g, &s));
    memcpy(g.payload, armed80, 4);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_panel_status(&g, &s));
    TEST_ASSERT_EQUAL_HEX8(ZBA_PS_ARMED_80, s.status);
    g.len = 3;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zba_decode_panel_status(&g, &s));
    g.len = 4;
    g.server_to_client = false;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zba_decode_panel_status(&g, &s));

    /* A label longer than ZBA_LABEL_MAX is cut, never past the payload. */
    const zba_zone_info_t z = {1, 0x0015, 0x02ABCD0000000099ull,
                               "A label far longer than any keypad shows on its screen"};
    zba_zone_info_response(&z, 1, &f);
    TEST_ASSERT_EQUAL_UINT8(ZBA_LABEL_MAX, f.payload[11]);
    TEST_ASSERT_EQUAL_UINT8(12u + ZBA_LABEL_MAX, f.len);
    uint8_t many[60];
    memset(many, 4, sizeof(many));
    zba_set_bypassed(many, 60, ZSRC_ZB_TSN_NEW, &f);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_ZCL_PAYLOAD_MAX, f.len);
    zba_zone_status_t recs[20];
    memset(recs, 0, sizeof(recs));
    zba_zone_status_response(false, recs, 20, 1, &f);
    TEST_ASSERT_EQUAL_UINT8(ZBA_ZONE_STATUS_MAX, f.payload[1]);
    TEST_ASSERT_TRUE(f.len <= ZSRC_ZB_ZCL_PAYLOAD_MAX);
}

/* ZB-024 (testing.md §12): the enums against an embedded copy of zigpy's security.py (IasAce
 * ArmMode, ArmNotification, PanelStatus, AlarmStatus, AudibleNotification, BypassResult and the
 * command ids), and Espressif's ezbee ias_ace.h which numbers them the same.
 * Checked on 2026-10-03 (BL-143) against zigpy's security.py at fb63170d520e: every row agrees
 * (the same numbers in IasAce's ArmMode, ArmNotification, PanelStatus, AlarmStatus,
 * AudibleNotification, BypassResponse, its server and client command ids and the cluster id
 * 0x0501), except ZBA_ZCL_MALFORMED (0x80, a ZCL status of the foundation, not read there).
 * Espressif's header was not reachable (its documentation pages answered 404 for the IAS ACE
 * and IAS WD headers), so that half of the sentence above stays unchecked.
 * https://github.com/zigpy/zigpy/blob/fb63170d520e/zigpy/zcl/clusters/security.py */
static void test_constants_cross_check(void)
{
    static const struct {
        unsigned ours, theirs;
    } t[] = {
        {ZBA_ARM_DISARM, 0x00}, {ZBA_ARM_DAY, 0x01}, {ZBA_ARM_NIGHT, 0x02}, {ZBA_ARM_ALL, 0x03},
        {ZBA_NOTE_DISARMED, 0x00}, {ZBA_NOTE_DAY_ARMED, 0x01}, {ZBA_NOTE_NIGHT_ARMED, 0x02},
        {ZBA_NOTE_ALL_ARMED, 0x03}, {ZBA_NOTE_INVALID_CODE, 0x04}, {ZBA_NOTE_NOT_READY, 0x05},
        {ZBA_NOTE_ALREADY_DISARMED, 0x06},
        {ZBA_PS_DISARMED, 0x00}, {ZBA_PS_ARMED_STAY, 0x01}, {ZBA_PS_ARMED_NIGHT, 0x02},
        {ZBA_PS_ARMED_AWAY, 0x03}, {ZBA_PS_EXIT_DELAY, 0x04}, {ZBA_PS_ENTRY_DELAY, 0x05},
        {ZBA_PS_NOT_READY, 0x06}, {ZBA_PS_IN_ALARM, 0x07}, {ZBA_PS_ARMING_STAY, 0x08},
        {ZBA_PS_ARMING_NIGHT, 0x09}, {ZBA_PS_ARMING_AWAY, 0x0A},
        {ZBA_ALARM_NONE, 0x00}, {ZBA_ALARM_BURGLAR, 0x01}, {ZBA_ALARM_FIRE, 0x02},
        {ZBA_ALARM_EMERGENCY, 0x03}, {ZBA_ALARM_POLICE_PANIC, 0x04}, {ZBA_ALARM_FIRE_PANIC, 0x05},
        {ZBA_ALARM_EMERGENCY_PANIC, 0x06},
        {ZBA_AUDIBLE_MUTE, 0x00}, {ZBA_AUDIBLE_DEFAULT, 0x01}, {ZBA_BYPASS_NOT_ALLOWED, 0x02},
        {ZBA_CMD_ARM, 0x00}, {ZBA_CMD_BYPASS, 0x01}, {ZBA_CMD_EMERGENCY, 0x02},
        {ZBA_CMD_FIRE, 0x03}, {ZBA_CMD_PANIC, 0x04}, {ZBA_CMD_GET_ZONE_ID_MAP, 0x05},
        {ZBA_CMD_GET_ZONE_INFO, 0x06}, {ZBA_CMD_GET_PANEL_STATUS, 0x07},
        {ZBA_CMD_GET_BYPASSED, 0x08}, {ZBA_CMD_GET_ZONE_STATUS, 0x09},
        {ZBA_RSP_ARM, 0x00}, {ZBA_RSP_ZONE_ID_MAP, 0x01}, {ZBA_RSP_ZONE_INFO, 0x02},
        {ZBA_RSP_ZONE_STATUS_CHANGED, 0x03}, {ZBA_RSP_PANEL_STATUS_CHANGED, 0x04},
        {ZBA_RSP_PANEL_STATUS, 0x05}, {ZBA_RSP_SET_BYPASSED, 0x06}, {ZBA_RSP_BYPASS, 0x07},
        {ZBA_RSP_ZONE_STATUS, 0x08}, {ZSRC_ZB_ZCL_IAS_ACE, 0x0501}, {ZBA_ZCL_MALFORMED, 0x80},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        TEST_ASSERT_EQUAL_HEX_MESSAGE(t[i].theirs, t[i].ours, "row of the embedded table");
    }
}

/* ---- the panel's state as a keypad shows it ---------------------------------------------------- */

/* ZB-024: the status table, with seconds counting down from `at`. */
static void test_status_for_table(void)
{
    static const struct {
        zba_view_t v;
        uint64_t later_ms;
        uint16_t flags;
        uint8_t status, seconds, audible, alarm;
    } t[] = {
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, false, 0}, 0, 0,
         ZBA_PS_DISARMED, 0, 0, 0},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, false, 0, 0, false, 0}, 0, 0,
         ZBA_PS_NOT_READY, 0, 0, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_STAY, false, 0, 0, false, 0}, 0, 0,
         ZBA_PS_ARMED_STAY, 0, 0, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_NIGHT, false, 0, 0, false, 0}, 0, 0,
         ZBA_PS_ARMED_NIGHT, 0, 0, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, false, 0, 0, false, 0}, 0, 0,
         ZBA_PS_ARMED_AWAY, 0, 0, 0},
        {{ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, false, 30, 0, false, 0}, 0, 0,
         ZBA_PS_EXIT_DELAY, 30, 1, 0},
        {{ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, false, 30, 0, false, 0}, 10000, 0,
         ZBA_PS_EXIT_DELAY, 20, 1, 0},
        {{ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, false, 30, 0, false, 0}, 10500, 0,
         ZBA_PS_EXIT_DELAY, 20, 1, 0},
        {{ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, false, 30, 0, false, 0}, 40000, 0,
         ZBA_PS_EXIT_DELAY, 0, 1, 0},
        {{ALARM_STATE_ENTRY_DELAY, ALARM_LEVEL_AWAY, false, 400, 0, false, 0}, 0, 0,
         ZBA_PS_ENTRY_DELAY, 255, 1, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, false, 0, 0, true, ALARM_CAUSE_BURGLARY}, 0, 0,
         ZBA_PS_IN_ALARM, 0, 1, ZBA_ALARM_BURGLAR},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, true, ALARM_CAUSE_FIRE}, 0, 0,
         ZBA_PS_IN_ALARM, 0, 1, ZBA_ALARM_FIRE},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, true, ALARM_CAUSE_CO}, 0, 0,
         ZBA_PS_IN_ALARM, 0, 1, ZBA_ALARM_EMERGENCY},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, true, ALARM_CAUSE_PANIC_POLICE}, 0,
         0, ZBA_PS_IN_ALARM, 0, 1, ZBA_ALARM_POLICE_PANIC},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, true, ALARM_CAUSE_PANIC_FIRE}, 0, 0,
         ZBA_PS_IN_ALARM, 0, 1, ZBA_ALARM_FIRE_PANIC},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, true, ALARM_CAUSE_PANIC_MEDICAL}, 0,
         0, ZBA_PS_IN_ALARM, 0, 1, ZBA_ALARM_EMERGENCY_PANIC},
        /* A silent alarm never shows: the keypad keeps showing the arm state. */
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, true, ALARM_CAUSE_PANIC_SILENT}, 0,
         0, ZBA_PS_DISARMED, 0, 0, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, false, 0, 0, true, ALARM_CAUSE_SILENT_BURGLARY}, 0,
         0, ZBA_PS_ARMED_AWAY, 0, 0, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, false, 0, 0, true, ALARM_CAUSE_24H_SILENT}, 0, 0,
         ZBA_PS_ARMED_AWAY, 0, 0, 0},
        /* Model rows: Centralite's 0x80, Frient's audible on every frame. */
        {{ALARM_STATE_ARMED, ALARM_LEVEL_STAY, false, 0, 0, false, 0}, 0, ZBZ_MODEL_ACE_ARMED_80,
         ZBA_PS_ARMED_80, 0, 0, 0},
        {{ALARM_STATE_ENTRY_DELAY, ALARM_LEVEL_AWAY, false, 30, 0, false, 0}, 0,
         ZBZ_MODEL_ACE_ARMED_80, ZBA_PS_ARMED_80, 0, 1, 0},
        {{ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, false, 30, 0, false, 0}, 0,
         ZBZ_MODEL_ACE_ARMED_80, ZBA_PS_DISARMED, 0, 1, 0},
        {{ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, true, 0, 0, false, 0}, 0,
         ZBZ_MODEL_ACE_ARMED_80, ZBA_PS_DISARMED, 0, 0, 0},
        {{ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, false, 0, 0, false, 0}, 0, ZBZ_MODEL_ACE_AUDIBLE,
         ZBA_PS_ARMED_AWAY, 0, 1, 0},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        zba_view_t v = t[i].v;
        v.at = 5000000u;
        zba_status_t s;
        zba_status_for(&v, v.at + t[i].later_ms, t[i].flags, &s);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(t[i].status, s.status, "status");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(t[i].seconds, s.seconds, "seconds");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(t[i].audible, s.audible, "audible");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(t[i].alarm, s.alarm, "alarm");
    }
    zba_status_t s;
    zba_status_for(NULL, 0, 0, &s);
    TEST_ASSERT_EQUAL_UINT8(ZBA_PS_NOT_READY, s.status); /* nothing known: never "ready" */
}

/* ZB-024: the Arm Response for each engine answer. */
static void test_note_for_table(void)
{
    const zba_view_t disarmed = {.state = ALARM_STATE_DISARMED, .ready = true};
    const zba_view_t armed = {.state = ALARM_STATE_ARMED, .level = ALARM_LEVEL_AWAY};
    const zba_view_t alarm = {.state = ALARM_STATE_DISARMED, .in_alarm = true,
                              .cause = ALARM_CAUSE_FIRE};
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, zba_note_for(0, ALARM_OK, &armed));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALREADY_DISARMED, zba_note_for(0, ALARM_OK, &disarmed));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, zba_note_for(0, ALARM_OK, &alarm));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, zba_note_for(0, ALARM_OK, NULL));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DAY_ARMED, zba_note_for(1, ALARM_OK, &disarmed));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NIGHT_ARMED, zba_note_for(2, ALARM_OK, &disarmed));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, zba_note_for(3, ALARM_OK, &disarmed));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NOT_READY, zba_note_for(4, ALARM_OK, &disarmed));
    const alarm_result_t code[] = {ALARM_ERR_INVALID_CODE, ALARM_ERR_CODE_REQUIRED,
                                   ALARM_ERR_LOCKED_OUT, ALARM_ERR_NOT_AUTHORIZED,
                                   ALARM_ERR_CODE_IN_USE};
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
        for (uint8_t m = 0; m <= 3u; m++) {
            TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, zba_note_for(m, code[i], &armed));
        }
    }
    const alarm_result_t other[] = {ALARM_ERR_NOT_READY, ALARM_ERR_HELD, ALARM_ERR_BAD_STATE,
                                    ALARM_ERR_UNAVAILABLE, ALARM_ERR_BAD_ARG,
                                    ALARM_ERR_DISABLED, ALARM_ERR_NOT_BYPASSABLE};
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++) {
        TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NOT_READY, zba_note_for(3, other[i], &disarmed));
    }
}

/* ---- the policy against the engine ------------------------------------------------------------- */

/* ZB-024: a keypad arms and disarms through the engine with the code it typed; the answer carries
 * the request's sequence number; its source is ALARM_SRC_KEYPAD with its device number. */
static void test_keypad_arms_and_disarms(void)
{
    zsrc_zb_zcl_t f = arm(0x21, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, CODE_USER));
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, POST.source.kind);
    TEST_ASSERT_EQUAL_UINT16(1, POST.source.id);
    TEST_ASSERT_EQUAL_UINT8(0, POST.user);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, POST.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, POST.cmd.level);
    TEST_ASSERT_EQUAL_UINT8(0, POST.cmd.arm_flags);
    TEST_ASSERT_EQUAL_UINT8(1, POST.cmd.partition);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, arm_note(0x21));
    TEST_ASSERT_EQUAL_UINT64(KP1_IEEE, last_sent(ZBA_RSP_ARM)->ieee);
    TEST_ASSERT_EQUAL_UINT8(KP1_EP, last_sent(ZBA_RSP_ARM)->endpoint);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ARMED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(1, ev->source.id);

    f = arm(0x22, ZBA_ARM_DISARM, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, CODE_USER));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_DISARM, POST.cmd.type);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, arm_note(0x22));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    /* Disarming a disarmed partition: "already disarmed". */
    f = arm(0x23, ZBA_ARM_DISARM, CODE_USER);
    act(KP1_IEEE, KP1_EP, &f, CODE_USER);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALREADY_DISARMED, arm_note(0x23));
    /* Day zones: STAY. Night zones map to NIGHT. */
    f = arm(0x24, ZBA_ARM_DAY, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, CODE_USER));
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_STAY, POST.cmd.level);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DAY_ARMED, arm_note(0x24));
    /* An arm-only user's code never disarms: the engine says so, the keypad shows "invalid". */
    f = arm(0x25, ZBA_ARM_DISARM, CODE_ARMONLY);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_NOT_AUTHORIZED, act(KP1_IEEE, KP1_EP, &f, CODE_ARMONLY));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x25));
    f = arm(0x26, ZBA_ARM_DISARM, CODE_USER);
    act(KP1_IEEE, KP1_EP, &f, CODE_USER);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, arm_note(0x26));
    /* Not ready: a window open. */
    sim_fault(&S, Z_WINDOW);
    push(1);
    f = arm(0x27, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_NOT_READY, act(KP1_IEEE, KP1_EP, &f, CODE_USER));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NOT_READY, arm_note(0x27));
    f = arm(0x28, ZBA_ARM_NIGHT, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_NIGHT, POST.cmd.level);
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
}

/* ZB-024: codes are the engine's to check - any code is passed on, a wrong one included, and
 * nothing of it stays in the policy's state once posted. */
static void test_codes_never_compared_here(void)
{
    zsrc_zb_zcl_t f = arm(0x31, ZBA_ARM_ALL, "8642");
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_STRING("8642", POST.cmd.code);
    TEST_ASSERT_FALSE(state_holds("8642"));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_INVALID_CODE, run_post("8642"));
    TEST_ASSERT_TRUE(zba_result(&A, NOW, POST.ticket, ALARM_ERR_INVALID_CODE, NULL));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x31));
    TEST_ASSERT_FALSE(state_holds("8642"));
    /* A real code: the same path, and gone the same way. */
    f = arm(0x32, ZBA_ARM_ALL, CODE_MASTER);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, CODE_MASTER));
    TEST_ASSERT_FALSE(state_holds(CODE_MASTER));
    /* A Bypass's code is never copied either. */
    static const uint8_t bypass[] = {0x01, 0x05, 0x06, 0x37, 0x35, 0x33, 0x31, 0x39, 0x37};
    f = client(0x33, ZBA_CMD_BYPASS, bypass, sizeof(bypass));
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_FALSE(state_holds("753197"));
    zba_post_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &POST, sizeof(POST)); /* nothing handed out */
}

/* ZB-024: five wrong codes lock that keypad out - the engine's counter, its device number the
 * source - a locked-out Arm answers "invalid code"; another Zigbee keypad and a Z-Wave keypad
 * (device 9) are counted apart. */
static void test_lockout_is_the_keypads(void)
{
    zba_keypad_t k2 = {.id = KP2_ID, .ieee = KP2_IEEE, .endpoint = 1, .partition = 1,
                       .device = zba_free_device(&A, 0)};
    TEST_ASSERT_EQUAL_UINT8(2, k2.device);
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k2));
    for (uint8_t i = 0; i < 5u; i++) {
        zsrc_zb_zcl_t f = arm((uint8_t)(0x40u + i), ZBA_ARM_ALL, "0000");
        TEST_ASSERT_EQUAL_INT(ALARM_ERR_INVALID_CODE, act(KP1_IEEE, KP1_EP, &f, "0000"));
        TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note((uint8_t)(0x40u + i)));
    }
    zsrc_zb_zcl_t f = arm(0x45, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_LOCKED_OUT, act(KP1_IEEE, KP1_EP, &f, CODE_USER));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x45));
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    /* A Z-Wave keypad's number has its own counter. */
    alarm_cmd_t c = {.type = ALARM_CMD_ARM, .partition = 1, .level = ALARM_LEVEL_STAY};
    strcpy(c.code, "0000");
    const alarm_source_t zw = {ALARM_SRC_KEYPAD, ZWAVE_KEYPAD_DEVICE};
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_INVALID_CODE, alarm_engine_command(&S.eng, &c, &zw, S.now,
                                                                       NULL));
    /* The second Zigbee keypad, device 2, still arms. */
    f = arm(0x46, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP2_IEEE, 1, &f, CODE_USER));
    TEST_ASSERT_EQUAL_UINT16(2, POST.source.id);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, arm_note(0x46));
    /* A wrong code repeated as a retransmission counts once (it isn't run again). */
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW, KP2_IEEE, 1, &f, &POST));
}

/* ZB-024: a fob acts as its user without a code - ALARM_SRC_ZONE id 0 - disarms only when the
 * installer turned that on, and an arm-only user's fob never disarms. */
static void test_fob_acts_as_its_user(void)
{
    zsrc_zb_zcl_t f = arm(0x51, ZBA_ARM_ALL, NULL);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(FOB_IEEE, FOB_EP, &f, ""));
    TEST_ASSERT_EQUAL_UINT8(ZBA_FOB_SOURCE_KIND, POST.source.kind);
    TEST_ASSERT_EQUAL_UINT16(ZBA_FOB_SOURCE_ID, POST.source.id);
    TEST_ASSERT_EQUAL_UINT8(3, POST.user);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, arm_note(0x51));
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_ARMED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(3, ev->user); /* "By Alex" */
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_ZONE, ev->source.kind);
    TEST_ASSERT_EQUAL_UINT16(0, ev->source.id);

    /* Disarm: off by default - "invalid code" at once, nothing posted, still armed. */
    unsigned events = S.n_events;
    f = arm(0x52, ZBA_ARM_DISARM, NULL);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x52));
    TEST_ASSERT_EQUAL_UINT(events, S.n_events);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    /* A code a fob sends is never passed on: it acts as its user alone. */
    zba_fob_t fb = fob1();
    fb.may_disarm = true;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &fb));
    f = arm(0x53, ZBA_ARM_DISARM, "1111");
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(FOB_IEEE, FOB_EP, &f, ""));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, arm_note(0x53));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    /* Pat's (arm-only) fob, may disarm on: the engine refuses the disarm. */
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_clear(&A, FOB_ID));
    fb.user = 4;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &fb));
    f = arm(0x54, ZBA_ARM_ALL, NULL);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(FOB_IEEE, FOB_EP, &f, ""));
    f = arm(0x55, ZBA_ARM_DISARM, NULL);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_NOT_AUTHORIZED, act(FOB_IEEE, FOB_EP, &f, ""));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x55));
    /* A fob assigned with defaults may neither disarm nor panic. */
    zba_fob_t def = {.id = 12, .ieee = 0x02ABCD0000000023ull, .endpoint = 1, .user = 5,
                     .partition = 1};
    TEST_ASSERT_FALSE(def.may_disarm);
    TEST_ASSERT_FALSE(def.panic);
}

/* ZB-024: duress codes behave as on the panel - the disarm happens, the silent duress event is
 * the engine's, the keypad shows a plain "disarmed". */
static void test_duress(void)
{
    zsrc_zb_zcl_t f = arm(0x61, ZBA_ARM_ALL, CODE_USER);
    act(KP1_IEEE, KP1_EP, &f, CODE_USER);
    f = arm(0x62, ZBA_ARM_DISARM, CODE_DURESS);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, CODE_DURESS));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_DISARMED, arm_note(0x62));
    EXPECT_EVENTS(&S, ALARM_EV_DURESS, 1);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* ZB-024: every command from a device not assigned - another address, or the right address at
 * another endpoint - gets NOT_AUTHORIZED and nothing else. */
static void test_unassigned_refused(void)
{
    static const uint8_t payload[] = {0x00, 0x04, 0x31, 0x31, 0x31, 0x31, 0x00, 0x00};
    for (uint8_t cmd = 0; cmd <= ZBA_CMD_GET_ZONE_STATUS; cmd++) {
        const uint64_t who[2] = {ODD_IEEE, KP1_IEEE};
        const uint8_t ep[2] = {KP1_EP, 2};
        for (size_t i = 0; i < 2u; i++) {
            NSENT = 0;
            NDRSP = 0;
            zsrc_zb_zcl_t f = client((uint8_t)(0x70u + cmd), cmd, payload, sizeof(payload));
            TEST_ASSERT_EQUAL_INT(ZBA_NOT_ASSIGNED, zba_receive(&A, NOW, who[i], ep[i], &f,
                                                                &POST));
            TEST_ASSERT_EQUAL_UINT(0, NSENT);
            TEST_ASSERT_EQUAL_UINT(1, NDRSP);
            TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_NOT_AUTHORIZED, DRSP[0].status);
            TEST_ASSERT_EQUAL_UINT8(cmd, DRSP[0].command);
            TEST_ASSERT_EQUAL_UINT8(0x70u + cmd, DRSP[0].tsn);
        }
    }
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        TEST_ASSERT_FALSE(A.pending[i].used);
    }
    for (size_t i = 0; i < ZBA_RECENT; i++) {
        TEST_ASSERT_FALSE(A.recent[i].used);
    }
    /* A cleared keypad is not heard any more. */
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_clear(&A, KP1_ID));
    zsrc_zb_zcl_t f = arm(0x7F, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_NOT_ASSIGNED, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    /* Not an ACE client frame at all: not answered. */
    NDRSP = 0;
    f.server_to_client = true;
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
    f.server_to_client = false;
    f.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(0, NDRSP);
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, NULL, &POST));
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, NULL));
}

/* ZB-024: damaged frames from an assigned keypad - refused, never guessed at. */
static void test_damaged_frames(void)
{
    static const uint8_t one[] = {0x03};
    zsrc_zb_zcl_t f = client(0x81, ZBA_CMD_ARM, one, sizeof(one));
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(1, NDRSP);
    TEST_ASSERT_EQUAL_HEX8(ZBA_ZCL_MALFORMED, DRSP[0].status);
    f = arm(0x82, ZBA_ARM_ALL, CODE_USER);
    f.manufacturer = 0x1015;
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_UNSUP_COMMAND, DRSP[1].status);
    f = client(0x83, 0x0A, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_UNSUP_COMMAND, DRSP[2].status);
    f = arm(0x84, ZBA_ARM_ALL, CODE_USER);
    f.len = ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_HEX8(ZBA_ZCL_MALFORMED, DRSP[3].status);
    static const uint8_t cut[] = {0x02, 0x05};
    f = client(0x85, ZBA_CMD_BYPASS, cut, sizeof(cut));
    TEST_ASSERT_EQUAL_INT(ZBA_INVALID, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_HEX8(ZBA_ZCL_MALFORMED, DRSP[4].status);
    TEST_ASSERT_EQUAL_UINT(0, count_sent(ZBA_RSP_ARM));
    /* A code length past the frame: "invalid code", nothing posted. */
    static const uint8_t past[] = {0x03, 0x09, 0x31, 0x31};
    f = client(0x86, ZBA_CMD_ARM, past, sizeof(past));
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x86));
    /* A code longer than any the panel has: the same. */
    f = arm(0x87, ZBA_ARM_ALL, "11111111111");
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x87));
    /* A mode above 3: "not ready". */
    f = arm(0x88, 7, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NOT_READY, arm_note(0x88));
    /* A keypad with no code: "invalid code" (only a fob arms without one). */
    f = arm(0x89, ZBA_ARM_ALL, NULL);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x89));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_events(&S, ALARM_EV_ARMED));
}

/* ZB-024: a retransmission isn't run again - waiting, nothing is sent; answered within 60 s, the
 * first answer again; after the window it is a new command. */
static void test_retransmission(void)
{
    zsrc_zb_zcl_t f = arm(0x91, ZBA_ARM_ALL, NULL);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
    zba_post_t first = POST;
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 500u, FOB_IEEE, FOB_EP, &f,
                                                     &POST));
    TEST_ASSERT_EQUAL_UINT(0, count_sent(ZBA_RSP_ARM));
    POST = first;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, run_post(""));
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 800u, POST.ticket, ALARM_OK, NULL));
    TEST_ASSERT_EQUAL_UINT(1, count_sent(ZBA_RSP_ARM));
    unsigned armed = sim_count_events(&S, ALARM_EV_ARMED);
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 30000u, FOB_IEEE, FOB_EP, &f,
                                                     &POST));
    TEST_ASSERT_EQUAL_UINT(2, count_sent(ZBA_RSP_ARM));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, arm_note(0x91));
    TEST_ASSERT_EQUAL_UINT(armed, sim_count_events(&S, ALARM_EV_ARMED));
    /* The same number from another device is its own command. */
    f = arm(0x91, ZBA_ARM_DISARM, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + 31000u, KP1_IEEE, KP1_EP, &f, &POST));
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 31000u, POST.ticket, ALARM_ERR_INVALID_CODE, NULL));
    /* Past the window: run again. */
    f = arm(0x91, ZBA_ARM_ALL, NULL);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + 61000u, FOB_IEEE, FOB_EP, &f, &POST));
}

/* ZB-024: at most ZBA_PENDING_MAX posts wait; an answer later than ZBA_ANSWER_MS sends nothing;
 * a ticket is answered once. */
static void test_busy_late_and_deadlines(void)
{
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zba_next_deadline(&A));
    uint32_t tickets[ZBA_PENDING_MAX];
    for (uint8_t i = 0; i < ZBA_PENDING_MAX; i++) {
        zsrc_zb_zcl_t f = arm((uint8_t)(0xA0u + i), ZBA_ARM_ALL, CODE_USER);
        TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + i, KP1_IEEE, KP1_EP, &f, &POST));
        tickets[i] = POST.ticket;
        TEST_ASSERT_TRUE(tickets[i] != 0u);
        memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
    }
    zsrc_zb_zcl_t f = arm(0xAF, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_BUSY, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_STRING("", POST.cmd.code);
    TEST_ASSERT_EQUAL_UINT(0, count_sent(ZBA_RSP_ARM));
    TEST_ASSERT_EQUAL_UINT64(NOW + ZBA_ANSWER_MS, zba_next_deadline(&A));
    /* Answered in time: sent; again: no such ticket. */
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 100u, tickets[0], ALARM_OK, NULL));
    TEST_ASSERT_FALSE(zba_result(&A, NOW + 100u, tickets[0], ALARM_OK, NULL));
    TEST_ASSERT_FALSE(zba_result(&A, NOW + 100u, 0u, ALARM_OK, NULL));
    TEST_ASSERT_FALSE(zba_result(&A, NOW + 100u, 0xDEADu, ALARM_OK, NULL));
    TEST_ASSERT_EQUAL_UINT(1, count_sent(ZBA_RSP_ARM));
    /* Too late: nothing sent (the keypad has shown its own error). */
    TEST_ASSERT_FALSE(zba_result(&A, NOW + 1u + ZBA_ANSWER_MS, tickets[1], ALARM_OK, NULL));
    TEST_ASSERT_EQUAL_UINT(1, count_sent(ZBA_RSP_ARM));
    /* The poll forgets the rest. */
    zba_poll(&A, NOW + 10u + ZBA_ANSWER_MS);
    TEST_ASSERT_FALSE(zba_result(&A, NOW + 10u + ZBA_ANSWER_MS, tickets[2], ALARM_OK, NULL));
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        TEST_ASSERT_FALSE(A.pending[i].used);
    }
    TEST_ASSERT_EQUAL_UINT64(NOW + 100u + ZBA_RETRY_WINDOW_MS, zba_next_deadline(&A));
    zba_poll(&A, NOW + 100u + ZBA_RETRY_WINDOW_MS);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zba_next_deadline(&A));
}

/* ZB-024 (BL-143, the ZB-M11 wiring): a panic always finds room - with every slot taken by Arms
 * the oldest Arm gives its slot up (its command still runs in the alarm task; its answer finds no
 * ticket, and its retransmission is neither answered nor run again); BUSY only when panics fill
 * every slot. */
static void test_panic_finds_room(void)
{
    uint32_t tickets[ZBA_PENDING_MAX];
    for (uint8_t i = 0; i < ZBA_PENDING_MAX; i++) {
        zsrc_zb_zcl_t f = arm((uint8_t)(0xC0u + i), ZBA_ARM_ALL, CODE_USER);
        TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + i, KP1_IEEE, KP1_EP, &f, &POST));
        tickets[i] = POST.ticket;
        memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
    }
    zsrc_zb_zcl_t f = arm(0xCF, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_BUSY, zba_receive(&A, NOW + 10u, KP1_IEEE, KP1_EP, &f, &POST));
    /* The panic gets through and runs. */
    f = client(0xD0, ZBA_CMD_FIRE, NULL, 0);
    f.want_response = false;
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + 20u, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, POST.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, run_post(""));
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 30u, POST.ticket, ALARM_OK, NULL));
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, 0, ALARM_CAUSE_PANIC_FIRE));
    /* The oldest Arm's place was taken: its answer has no ticket, its retry is not run again. */
    TEST_ASSERT_FALSE(zba_result(&A, NOW + 40u, tickets[0], ALARM_OK, NULL));
    f = arm(0xC0, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 50u, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(0, count_sent(ZBA_RSP_ARM));
    /* The other three still wait for their answers. */
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 60u, tickets[1], ALARM_OK, NULL));
    TEST_ASSERT_EQUAL_UINT(1, count_sent(ZBA_RSP_ARM));

    /* Panics in every slot: one more is BUSY (it retries). */
    zba_init(&A, &PORT);
    zba_keypad_t k = keypad1();
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    push(1);
    for (uint8_t i = 0; i < ZBA_PENDING_MAX; i++) {
        f = client((uint8_t)(0xE0u + i), ZBA_CMD_PANIC, NULL, 0);
        f.want_response = false;
        TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + i, KP1_IEEE, KP1_EP, &f, &POST));
    }
    f = client(0xEF, ZBA_CMD_PANIC, NULL, 0);
    f.want_response = false;
    TEST_ASSERT_EQUAL_INT(ZBA_BUSY, zba_receive(&A, NOW + 9u, KP1_IEEE, KP1_EP, &f, &POST));
}

/* ZB-024 (BL-143): a retransmission is the same device, sequence number and command - an Arm also
 * its arm mode: an Arm with the number of the last one but another mode is a new command, waiting
 * or answered. */
static void test_retransmission_matches_mode(void)
{
    zsrc_zb_zcl_t f = arm(0x93, ZBA_ARM_DAY, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    zba_post_t day = POST;
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 1u, KP1_IEEE, KP1_EP, &f, &POST));
    /* Waiting: another mode with that number is posted on its own. */
    f = arm(0x93, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + 2u, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_TRUE(POST.ticket != day.ticket);
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
    memset(day.cmd.code, 0, sizeof(day.cmd.code));
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 3u, day.ticket, ALARM_OK, NULL));
    TEST_ASSERT_TRUE(zba_result(&A, NOW + 3u, POST.ticket, ALARM_OK, NULL));
    TEST_ASSERT_EQUAL_UINT(2, count_sent(ZBA_RSP_ARM));
    /* Answered: the same mode is the first answer again; a Disarm with that number runs. */
    f = arm(0x93, ZBA_ARM_ALL, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 4u, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(3, count_sent(ZBA_RSP_ARM));
    f = arm(0x93, ZBA_ARM_DISARM, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + 5u, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_DISARM, POST.cmd.type);
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
}

/* A fresh policy (keypad 1, the fob with `may_disarm`), nothing sent yet. */
static void fresh_policy(bool may_disarm)
{
    zba_init(&A, &PORT);
    zba_keypad_t k = keypad1();
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    zba_fob_t fb = fob1();
    fb.may_disarm = may_disarm;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &fb));
    push(1);
    NSENT = 0;
    NDRSP = 0;
}

/* `f` from `ieee` is posted; its code wiped (the owner's job), its ticket left waiting. */
static void posted(uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *f, uint8_t mode)
{
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW + 3u, ieee, endpoint, f, &POST));
    TEST_ASSERT_EQUAL_INT(mode == ZBA_ARM_DISARM ? ALARM_CMD_DISARM : ALARM_CMD_ARM,
                          POST.cmd.type);
    memset(POST.cmd.code, 0, sizeof(POST.cmd.code));
}

/* ZB-024 (ZB02): an Arm refused at once - code too long, not digits, its length past the frame,
 * no code from a keypad, a mode above 3, a fob's disarm off - is remembered with its own mode:
 * its retransmission gets the first answer again and isn't evaluated again; another mode with
 * that sequence number is a new command (a valid Disarm runs); a fresh number is new too. Every
 * code copy is still wiped. */
static void test_refused_arm_remembers_its_mode(void)
{
    static const uint8_t past[] = {0x00, 0x09, 0x31, 0x31}; /* mode set per case */
    const char *const codes[] = {"11111111111", "12a4", NULL};
    for (uint8_t kind = 0; kind < 4u; kind++) {
        for (uint8_t mode = ZBA_ARM_DAY; mode <= ZBA_ARM_ALL; mode++) {
            fresh_policy(false);
            const uint8_t tsn = (uint8_t)(0x10u + kind * 4u + mode);
            zsrc_zb_zcl_t f;
            if (kind < 3u) {
                f = arm(tsn, mode, codes[kind]);
            } else {
                f = client(tsn, ZBA_CMD_ARM, past, sizeof(past));
                f.payload[0] = mode;
            }
            TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
            TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(tsn));
            TEST_ASSERT_EQUAL_UINT(1, count_sent(ZBA_RSP_ARM));
            TEST_ASSERT_FALSE(state_holds("12a4"));
            /* Its retransmission: the first answer again, not evaluated again. */
            TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE,
                                  zba_receive(&A, NOW + 1u, KP1_IEEE, KP1_EP, &f, &POST));
            TEST_ASSERT_EQUAL_UINT(2, count_sent(ZBA_RSP_ARM));
            TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(tsn));
            /* Another arm mode with the same refusal and number: evaluated as new. */
            zsrc_zb_zcl_t other = f;
            other.payload[0] = mode == ZBA_ARM_ALL ? ZBA_ARM_DAY : (uint8_t)(mode + 1u);
            TEST_ASSERT_EQUAL_INT(ZBA_NOTHING,
                                  zba_receive(&A, NOW + 2u, KP1_IEEE, KP1_EP, &other, &POST));
            TEST_ASSERT_EQUAL_UINT(3, count_sent(ZBA_RSP_ARM));
            /* A valid Disarm with that number runs. */
            zsrc_zb_zcl_t dis = arm(tsn, ZBA_ARM_DISARM, CODE_USER);
            posted(KP1_IEEE, KP1_EP, &dis, ZBA_ARM_DISARM);
            /* A fresh number: a new command. */
            zsrc_zb_zcl_t fresh = arm((uint8_t)(tsn + 0x80u), mode, CODE_USER);
            posted(KP1_IEEE, KP1_EP, &fresh, mode);
            TEST_ASSERT_FALSE(state_holds(CODE_USER));
        }
    }
    /* A mode above 3 ("not ready"): its retry matches, a valid mode with that number runs. */
    const uint8_t bad_modes[] = {4, 7, 0xFF};
    for (size_t i = 0; i < sizeof(bad_modes); i++) {
        fresh_policy(false);
        const uint8_t tsn = (uint8_t)(0x40u + i);
        zsrc_zb_zcl_t f = arm(tsn, bad_modes[i], "1357");
        TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
        TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NOT_READY, arm_note(tsn));
        TEST_ASSERT_FALSE(state_holds("1357"));
        TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 1u, KP1_IEEE, KP1_EP, &f, &POST));
        TEST_ASSERT_EQUAL_UINT(2, count_sent(ZBA_RSP_ARM));
        TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_NOT_READY, arm_note(tsn));
        for (uint8_t mode = ZBA_ARM_DISARM; mode <= ZBA_ARM_ALL; mode++) {
            zsrc_zb_zcl_t ok = arm(tsn, mode, CODE_USER);
            posted(KP1_IEEE, KP1_EP, &ok, mode);
            TEST_ASSERT_TRUE(zba_result(&A, NOW + 4u, POST.ticket, ALARM_OK, NULL));
        }
        zsrc_zb_zcl_t fresh = arm((uint8_t)(tsn + 0x80u), ZBA_ARM_ALL, CODE_USER);
        posted(KP1_IEEE, KP1_EP, &fresh, ZBA_ARM_ALL);
    }
    /* A fob's Disarm while its disarm is off: its retry matches; an arm mode with that number
     * runs; a fresh number is refused anew. */
    fresh_policy(false);
    zsrc_zb_zcl_t f = arm(0x50, ZBA_ARM_DISARM, NULL);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x50));
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW + 1u, FOB_IEEE, FOB_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(2, count_sent(ZBA_RSP_ARM));
    for (uint8_t mode = ZBA_ARM_DAY; mode <= ZBA_ARM_ALL; mode++) {
        zsrc_zb_zcl_t ok = arm(0x50, mode, NULL);
        posted(FOB_IEEE, FOB_EP, &ok, mode);
        TEST_ASSERT_TRUE(zba_result(&A, NOW + 4u, POST.ticket, ALARM_OK, NULL));
    }
    f = arm(0x51, ZBA_ARM_DISARM, NULL);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW + 5u, FOB_IEEE, FOB_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, arm_note(0x51));
}

/* ZB-024: Emergency, Fire and Panic raise medical, fire and police panic within panic_mask; a
 * fob's panic button raises police panic only when turned on. */
static void test_panic_buttons(void)
{
    /* Keypad 1 may raise police and fire, not medical. */
    zsrc_zb_zcl_t f = client(0xB1, ZBA_CMD_EMERGENCY, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(1, NDRSP);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_NOT_AUTHORIZED, DRSP[0].status);
    f = client(0xB2, ZBA_CMD_FIRE, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, ""));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, POST.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_FIRE, POST.cmd.panic);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, POST.source.kind);
    TEST_ASSERT_EQUAL_UINT(2, NDRSP); /* it asked for a Default Response: SUCCESS */
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, DRSP[1].status);
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, 0, ALARM_CAUSE_PANIC_FIRE));
    f = client(0xB3, ZBA_CMD_PANIC, NULL, 0);
    f.want_response = false;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, act(KP1_IEEE, KP1_EP, &f, ""));
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_POLICE, POST.cmd.panic);
    TEST_ASSERT_EQUAL_UINT(2, NDRSP); /* none asked, none sent */
    TEST_ASSERT_EQUAL_INT(ZBA_DUPLICATE, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(2, NDRSP);
    /* A refusal from the engine is always answered. */
    f = client(0xB4, ZBA_CMD_PANIC, NULL, 0);
    f.want_response = false;
    TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_TRUE(zba_result(&A, NOW, POST.ticket, ALARM_ERR_NOT_AUTHORIZED, NULL));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_NOT_AUTHORIZED, DRSP[NDRSP - 1u].status);

    /* The fob: off by default. */
    unsigned before = NDRSP;
    f = client(0xB5, ZBA_CMD_EMERGENCY, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_NOT_AUTHORIZED, DRSP[before].status);
    zba_fob_t fb = fob1();
    fb.panic = true;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &fb));
    const uint8_t buttons[3] = {ZBA_CMD_EMERGENCY, ZBA_CMD_FIRE, ZBA_CMD_PANIC};
    for (uint8_t i = 0; i < 3u; i++) {
        f = client((uint8_t)(0xB6u + i), buttons[i], NULL, 0);
        TEST_ASSERT_EQUAL_INT(ZBA_POST, zba_receive(&A, NOW, FOB_IEEE, FOB_EP, &f, &POST));
        TEST_ASSERT_EQUAL_INT(ALARM_PANIC_POLICE, POST.cmd.panic); /* its one panic button */
        TEST_ASSERT_EQUAL_UINT8(3, POST.user);
        TEST_ASSERT_EQUAL_UINT8(ZBA_FOB_SOURCE_KIND, POST.source.kind);
        TEST_ASSERT_EQUAL_INT(ALARM_OK, run_post(""));
        TEST_ASSERT_TRUE(zba_result(&A, NOW, POST.ticket, ALARM_OK, NULL));
    }
    TEST_ASSERT_TRUE(sim_alarm_logged(&S, 0, ALARM_CAUSE_PANIC_POLICE));
}

/* ZB-024: the questions are answered at once, never ignored, never posted. */
static void test_questions_answered(void)
{
    static const struct {
        uint8_t command;
        uint8_t payload[8];
        uint8_t len;
        uint8_t answer;
        uint8_t answer_len;
    } t[] = {
        {ZBA_CMD_GET_ZONE_ID_MAP, {0}, 0, ZBA_RSP_ZONE_ID_MAP, 32},
        {ZBA_CMD_GET_ZONE_INFO, {9}, 1, ZBA_RSP_ZONE_INFO, 12},
        {ZBA_CMD_BYPASS, {3, 4, 5, 6, 0}, 5, ZBA_RSP_BYPASS, 4},
        {ZBA_CMD_GET_BYPASSED, {0}, 0, ZBA_RSP_SET_BYPASSED, 1},
        {ZBA_CMD_GET_ZONE_STATUS, {1, 8, 0, 0, 0}, 5, ZBA_RSP_ZONE_STATUS, 2},
        {ZBA_CMD_GET_PANEL_STATUS, {0}, 0, ZBA_RSP_PANEL_STATUS, 4},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        for (size_t who = 0; who < 2u; who++) {
            NSENT = 0;
            zsrc_zb_zcl_t f = client((uint8_t)(0xC0u + i), t[i].command, t[i].payload, t[i].len);
            uint64_t ieee = who == 0u ? KP1_IEEE : FOB_IEEE;
            uint8_t ep = who == 0u ? KP1_EP : FOB_EP;
            TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, ieee, ep, &f, &POST));
            TEST_ASSERT_EQUAL_UINT(1, NSENT);
            TEST_ASSERT_EQUAL_UINT8(t[i].answer, SENT[0].f.command);
            TEST_ASSERT_EQUAL_UINT8(t[i].answer_len, SENT[0].f.len);
            TEST_ASSERT_EQUAL_UINT16(0xC0u + i, SENT[0].f.tsn);
            TEST_ASSERT_TRUE(SENT[0].f.server_to_client);
            TEST_ASSERT_EQUAL_UINT64(ieee, SENT[0].ieee);
        }
    }
    /* Get Zone Info: "no such zone" for the zone asked; Bypass: "not allowed" for each zone. */
    zsrc_zb_zcl_t f = client(0xCA, ZBA_CMD_GET_ZONE_INFO, (const uint8_t[]){9}, 1);
    zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST);
    const sent_t *s = last_sent(ZBA_RSP_ZONE_INFO);
    TEST_ASSERT_EQUAL_UINT8(9, s->f.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, s->f.payload[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, s->f.payload[2]);
    f = client(0xCB, ZBA_CMD_BYPASS, (const uint8_t[]){2, 4, 5, 0}, 4);
    zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST);
    s = last_sent(ZBA_RSP_BYPASS);
    TEST_ASSERT_EQUAL_UINT8(2, s->f.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(ZBA_BYPASS_NOT_ALLOWED, s->f.payload[1]);
    TEST_ASSERT_EQUAL_HEX8(ZBA_BYPASS_NOT_ALLOWED, s->f.payload[2]);
    TEST_ASSERT_EQUAL_UINT(0, sim_count_events(&S, ALARM_EV_BYPASS));
}

/* ZB-024: the panel's state goes to each keypad of the partition that shows it; delays carry
 * the seconds left, re-sent by Get Panel Status; an unchanged state isn't sent again. */
static void test_panel_status_to_keypads(void)
{
    zba_keypad_t k2 = {.id = KP2_ID, .ieee = KP2_IEEE, .endpoint = 1, .partition = 1,
                       .device = 2, .show_state = false};
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k2));
    zba_keypad_t k3 = {.id = 8, .ieee = 0x02ABCD0000000013ull, .endpoint = 1, .partition = 2,
                       .device = 3, .show_state = true};
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k3));
    NSENT = 0;
    const zba_view_t exit = {.state = ALARM_STATE_EXIT_DELAY, .level = ALARM_LEVEL_AWAY,
                             .delay_s = 30, .at = NOW};
    zba_panel(&A, NOW, 1, &exit);
    TEST_ASSERT_EQUAL_UINT(1, NSENT); /* keypad 1 only: not 2 (no state), 3 (partition 2), fob */
    TEST_ASSERT_EQUAL_UINT64(KP1_IEEE, SENT[0].ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBA_RSP_PANEL_STATUS_CHANGED, SENT[0].f.command);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_ZB_TSN_NEW, SENT[0].f.tsn);
    TEST_ASSERT_FALSE(SENT[0].f.want_response);
    TEST_ASSERT_EQUAL_UINT8(ZBA_PS_EXIT_DELAY, SENT[0].f.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(30, SENT[0].f.payload[1]);
    TEST_ASSERT_EQUAL_UINT8(ZBA_AUDIBLE_DEFAULT, SENT[0].f.payload[2]);
    /* Ten seconds on, Get Panel Status: 20 left. */
    zsrc_zb_zcl_t f = client(0xD1, ZBA_CMD_GET_PANEL_STATUS, NULL, 0);
    zba_receive(&A, NOW + 10000u, KP1_IEEE, KP1_EP, &f, &POST);
    const sent_t *s = last_sent(ZBA_RSP_PANEL_STATUS);
    TEST_ASSERT_EQUAL_UINT8(ZBA_PS_EXIT_DELAY, s->f.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(20, s->f.payload[1]);
    /* Armed, twice: sent once. */
    const zba_view_t armed = {.state = ALARM_STATE_ARMED, .level = ALARM_LEVEL_AWAY,
                              .at = NOW + 30000u};
    NSENT = 0;
    zba_panel(&A, NOW + 30000u, 1, &armed);
    zba_panel(&A, NOW + 31000u, 1, &armed);
    TEST_ASSERT_EQUAL_UINT(1, NSENT);
    TEST_ASSERT_EQUAL_UINT8(ZBA_PS_ARMED_AWAY, SENT[0].f.payload[0]);
    /* The radio didn't take it: tried again at the next news. */
    const zba_view_t entry = {.state = ALARM_STATE_ENTRY_DELAY, .level = ALARM_LEVEL_AWAY,
                              .delay_s = 45, .at = NOW + 40000u};
    SEND_FAILS = true;
    zba_panel(&A, NOW + 40000u, 1, &entry);
    SEND_FAILS = false;
    zba_panel(&A, NOW + 41000u, 1, &entry);
    TEST_ASSERT_EQUAL_UINT(2, NSENT);
    TEST_ASSERT_EQUAL_UINT8(ZBA_PS_ENTRY_DELAY, SENT[1].f.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(44, SENT[1].f.payload[1]);
    /* Partitions out of range change nothing. */
    zba_panel(&A, NOW, 0, &armed);
    zba_panel(&A, NOW, ALARM_MAX_PARTITIONS + 1u, &armed);
    TEST_ASSERT_EQUAL_UINT(2, NSENT);
}

/* ZB-024: the model rows - 0x80 armed, audible, touch wake, and the init frame a Linkind keypad
 * needs before it takes Arm Responses. */
static void test_model_rows(void)
{
    zba_keypad_t k = keypad1();
    k.model_flags = ZBZ_MODEL_ACE_ARMED_80 | ZBZ_MODEL_ACE_AUDIBLE;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    NSENT = 0;
    const zba_view_t armed = {.state = ALARM_STATE_ARMED, .level = ALARM_LEVEL_STAY, .at = NOW};
    zba_panel(&A, NOW, 1, &armed);
    TEST_ASSERT_EQUAL_HEX8(ZBA_PS_ARMED_80, SENT[0].f.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(ZBA_AUDIBLE_DEFAULT, SENT[0].f.payload[2]);

    /* Touch wake: Get Panel Status is answered at once, with what it would be shown. */
    k.model_flags = ZBZ_MODEL_ACE_TOUCH_WAKE;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    NSENT = 0;
    zsrc_zb_zcl_t f = client(0xE1, ZBA_CMD_GET_PANEL_STATUS, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(1, NSENT);
    TEST_ASSERT_EQUAL_UINT8(ZBA_PS_ARMED_STAY, SENT[0].f.payload[0]);

    /* Init: a Linkind keypad hears one Panel Status Changed before its first Arm Response. */
    k.model_flags = ZBZ_MODEL_ACE_INIT;
    k.show_state = false;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    NSENT = 0;
    f = arm(0xE2, ZBA_ARM_DISARM, NULL); /* no code: answered here, after the greeting */
    TEST_ASSERT_EQUAL_INT(ZBA_NOTHING, zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST));
    TEST_ASSERT_EQUAL_UINT(2, NSENT);
    TEST_ASSERT_EQUAL_UINT8(ZBA_RSP_PANEL_STATUS_CHANGED, SENT[0].f.command);
    TEST_ASSERT_EQUAL_UINT8(ZBA_RSP_ARM, SENT[1].f.command);
    f = arm(0xE3, ZBA_ARM_DISARM, NULL);
    zba_receive(&A, NOW, KP1_IEEE, KP1_EP, &f, &POST);
    TEST_ASSERT_EQUAL_UINT(3, NSENT); /* greeted once */
    /* zba_greet: the owner's call once assigned or rejoined, whatever show_state says. */
    TEST_ASSERT_TRUE(zba_greet(&A, NOW, KP1_ID));
    TEST_ASSERT_EQUAL_UINT8(ZBA_RSP_PANEL_STATUS_CHANGED, SENT[3].f.command);
    TEST_ASSERT_FALSE(zba_greet(&A, NOW, 999));
    TEST_ASSERT_FALSE(zba_greet(&A, NOW, FOB_ID)); /* a fob shows nothing */
}

/* ---- assignments and device numbers ------------------------------------------------------------ */

/* ZB-024 (alarm-devices.md §4): the lowest free number of 1..8, never a Z-Wave keypad's. */
static void test_device_numbers(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x01u, zba_devices_held(&A));
    TEST_ASSERT_EQUAL_UINT8(2, zba_free_device(&A, 0));
    TEST_ASSERT_EQUAL_UINT8(4, zba_free_device(&A, 0x06u)); /* 2 and 3: panel-link keypads */
    TEST_ASSERT_EQUAL_UINT8(0, zba_free_device(&A, 0xFEu)); /* every number held */
    TEST_ASSERT_EQUAL_UINT8(0, zba_free_device(&A, 0xFFFFFFFEu));
    zba_keypad_t k = {.id = 20, .ieee = 0x02ABCD0000000030ull, .endpoint = 1, .partition = 1};
    const uint8_t refused[] = {0, ZBA_DEVICE_LAST + 1u, ZWAVE_KEYPAD_DEVICE, 16, 255};
    for (size_t i = 0; i < sizeof(refused); i++) {
        k.device = refused[i];
        TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_keypad(&A, &k));
    }
    k.device = 1;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_IN_USE, zba_assign_keypad(&A, &k)); /* keypad 1's */
    /* Fill 2..8: then none is free, and a ninth keypad has no number. */
    for (uint8_t d = 2; d <= ZBA_DEVICE_LAST; d++) {
        k.id = (uint16_t)(20u + d);
        k.ieee = 0x02ABCD0000000030ull + d;
        k.device = zba_free_device(&A, 0);
        TEST_ASSERT_EQUAL_UINT8(d, k.device);
        TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &k));
    }
    TEST_ASSERT_EQUAL_UINT8(0, zba_free_device(&A, 0));
    TEST_ASSERT_EQUAL_UINT32(0xFFu, zba_devices_held(&A));
    /* Re-assigned, it keeps its number; another keypad's number is refused. */
    zba_keypad_t again = keypad1();
    again.partition = 2;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&A, &again));
    again.device = 2;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_IN_USE, zba_assign_keypad(&A, &again));
    /* Cleared, its number is free again. */
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_clear(&A, 25));
    TEST_ASSERT_EQUAL_UINT8(5, zba_free_device(&A, 0));
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_UNKNOWN, zba_clear(&A, 25));
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_UNKNOWN, zba_clear(&A, 0));
}

/* ZB-024: what an assignment may be - one fob for each user, one assignment for each device,
 * the arrays' limits, and a device that becomes the other kind. */
static void test_assignment_rules(void)
{
    zba_fob_t f = fob1();
    f.id = 30;
    f.ieee = 0x02ABCD0000000040ull;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_IN_USE, zba_assign_fob(&A, &f)); /* Alex has a fob */
    f.user = 4;
    f.ieee = KP1_IEEE;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_IN_USE, zba_assign_fob(&A, &f)); /* keypad 1's address */
    f.ieee = 0x02ABCD0000000040ull;
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &f));
    zba_fob_t bad = f;
    bad.user = 0;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    bad = f;
    bad.user = ALARM_MAX_USERS + 1u;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    bad = f;
    bad.partition = 0;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    bad = f;
    bad.partition = ALARM_MAX_PARTITIONS + 1u;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    bad = f;
    bad.endpoint = 0;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    bad = f;
    bad.ieee = 0;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    bad = f;
    bad.id = 0;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, &bad));
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_fob(&A, NULL));
    zba_keypad_t kb = keypad1();
    kb.panic_mask = 0x10;
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_ARG, zba_assign_keypad(&A, &kb));
    /* Fobs to the limit. */
    for (uint8_t i = 2; i < ZBA_FOBS_MAX; i++) {
        zba_fob_t g = {.id = (uint16_t)(40u + i), .ieee = 0x02ABCD0000000100ull + i,
                       .endpoint = 1, .user = (uint8_t)(10u + i), .partition = 1};
        TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &g));
    }
    zba_fob_t extra = {.id = 99, .ieee = 0x02ABCD00000001FFull, .endpoint = 1, .user = 60,
                       .partition = 1};
    TEST_ASSERT_EQUAL_INT(ZBA_ERR_FULL, zba_assign_fob(&A, &extra));
    /* Keypad 1 becomes a fob: its keypad assignment and its number go. */
    zba_fob_t now_fob = {.id = KP1_ID, .ieee = KP1_IEEE, .endpoint = KP1_EP, .user = 61,
                         .partition = 1};
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_clear(&A, 30));
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&A, &now_fob));
    TEST_ASSERT_EQUAL_UINT32(0, zba_devices_held(&A));
    TEST_ASSERT_TRUE(A.dirty);
}

/* ---- the store --------------------------------------------------------------------------------- */

static void fill_fixture_content(zba_t *a)
{
    zba_init(a, &PORT);
    zba_keypad_t k1 = {.id = 3, .ieee = 0x02ABCD0000000011ull, .endpoint = 44, .partition = 1,
                       .panic_mask = 0x05, .show_state = true, .device = 1};
    zba_keypad_t k2 = {.id = 7, .ieee = 0x02ABCD0000000012ull, .endpoint = 1, .partition = 2,
                       .device = 2};
    zba_fob_t f1 = {.id = 9, .ieee = 0x02ABCD0000000021ull, .endpoint = 1, .user = 3,
                    .partition = 1, .may_disarm = true};
    zba_fob_t f2 = {.id = 10, .ieee = 0x02ABCD0000000022ull, .endpoint = 1, .user = 4,
                    .partition = 1, .panic = true};
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(a, &k1));
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(a, &k2));
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(a, &f1));
    TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(a, &f2));
}

/* ZB-024: the frozen v1 blob reads, and the same content writes the same bytes. */
static void test_store_fixture(void)
{
    zba_t got;
    zba_init(&got, &PORT);
    TEST_ASSERT_TRUE(zba_decode(&got, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1)));
    TEST_ASSERT_FALSE(got.dirty);
    TEST_ASSERT_TRUE(got.keypad[0].used);
    TEST_ASSERT_EQUAL_UINT16(3, got.keypad[0].id);
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD0000000011ull, got.keypad[0].ieee);
    TEST_ASSERT_EQUAL_UINT8(44, got.keypad[0].endpoint);
    TEST_ASSERT_EQUAL_UINT8(1, got.keypad[0].partition);
    TEST_ASSERT_EQUAL_HEX8(0x05, got.keypad[0].panic_mask);
    TEST_ASSERT_TRUE(got.keypad[0].show_state);
    TEST_ASSERT_EQUAL_UINT8(1, got.keypad[0].device);
    TEST_ASSERT_EQUAL_UINT16(7, got.keypad[1].id);
    TEST_ASSERT_EQUAL_UINT8(2, got.keypad[1].partition);
    TEST_ASSERT_FALSE(got.keypad[1].show_state);
    TEST_ASSERT_EQUAL_UINT8(2, got.keypad[1].device);
    TEST_ASSERT_EQUAL_UINT16(9, got.fob[0].id);
    TEST_ASSERT_EQUAL_UINT8(3, got.fob[0].user);
    TEST_ASSERT_TRUE(got.fob[0].may_disarm);
    TEST_ASSERT_FALSE(got.fob[0].panic);
    TEST_ASSERT_EQUAL_UINT16(10, got.fob[1].id);
    TEST_ASSERT_EQUAL_UINT8(4, got.fob[1].user);
    TEST_ASSERT_FALSE(got.fob[1].may_disarm);
    TEST_ASSERT_TRUE(got.fob[1].panic);
    TEST_ASSERT_EQUAL_UINT32(0x03u, zba_devices_held(&got));

    zba_t same;
    fill_fixture_content(&same);
    uint8_t buf[ZBA_BLOB_MAX];
    size_t n = zba_encode(&same, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(ACE_FIXTURE_V1), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ACE_FIXTURE_V1, buf, n);
    TEST_ASSERT_EQUAL_size_t(0, zba_encode(&same, buf, n - 1u));
    TEST_ASSERT_EQUAL_size_t(0, zba_encode(&same, NULL, sizeof(buf)));
    /* Full arrays: exactly ZBA_BLOB_MAX (the NVS budget's number). */
    zba_t full;
    zba_init(&full, &PORT);
    for (uint8_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        zba_keypad_t k = {.id = (uint16_t)(1u + i), .ieee = 0x02ABCD0000000200ull + i,
                          .endpoint = 1, .partition = 1, .device = (uint8_t)(1u + i)};
        TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_keypad(&full, &k));
    }
    for (uint8_t i = 0; i < ZBA_FOBS_MAX; i++) {
        zba_fob_t f = {.id = (uint16_t)(100u + i), .ieee = 0x02ABCD0000000300ull + i,
                       .endpoint = 1, .user = (uint8_t)(1u + i), .partition = 1};
        TEST_ASSERT_EQUAL_INT(ZBA_OK, zba_assign_fob(&full, &f));
    }
    TEST_ASSERT_EQUAL_size_t(ZBA_BLOB_MAX, zba_encode(&full, buf, sizeof(buf)));
    zba_t back;
    zba_init(&back, &PORT);
    TEST_ASSERT_TRUE(zba_decode(&back, buf, ZBA_BLOB_MAX));
    TEST_ASSERT_EQUAL_UINT32(0xFFu, zba_devices_held(&back));
}

/* Decodes a damaged copy of the fixture: the keypads' and fobs' ids that loaded. */
static bool decode_copy(const uint8_t *b, size_t n, zba_t *out)
{
    zba_init(out, &PORT);
    return zba_decode(out, b, n);
}

static unsigned assigned(const zba_t *a)
{
    unsigned n = 0;
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        n += a->keypad[i].used ? 1u : 0u;
    }
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        n += a->fob[i].used ? 1u : 0u;
    }
    return n;
}

/* ZB-024: damaged and future blobs - refused whole, or the one bad entry dropped. */
static void test_store_damaged(void)
{
    uint8_t b[sizeof(ACE_FIXTURE_V1) + 8];
    zba_t got;
    /* Every truncation is refused, and leaves nothing assigned. */
    for (size_t n = 0; n < sizeof(ACE_FIXTURE_V1); n++) {
        TEST_ASSERT_TRUE(decode_copy(ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1), &got));
        TEST_ASSERT_FALSE(zba_decode(&got, ACE_FIXTURE_V1, n));
        TEST_ASSERT_EQUAL_UINT(0, assigned(&got));
    }
    TEST_ASSERT_FALSE(zba_decode(&got, NULL, 10));
    /* A byte too many, compat 0, a later compat, counts past the arrays. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[sizeof(ACE_FIXTURE_V1)] = 0;
    TEST_ASSERT_FALSE(decode_copy(b, sizeof(ACE_FIXTURE_V1) + 1u, &got));
    b[0] = 0;
    TEST_ASSERT_FALSE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    b[0] = ZBA_COMPAT + 1u;
    TEST_ASSERT_FALSE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(0, assigned(&got));
    static const uint8_t many[] = {0x01, ZBA_KEYPADS_MAX + 1u, 0x00};
    TEST_ASSERT_FALSE(decode_copy(many, sizeof(many), &got));
    static const uint8_t empty[] = {0x01, 0x00, 0x00};
    TEST_ASSERT_TRUE(decode_copy(empty, sizeof(empty), &got));
    TEST_ASSERT_EQUAL_UINT(0, assigned(&got));
    /* An entry shorter than its kind's fields. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[3] = 0x0F; /* keypad 1 claims 15 bytes: its device byte becomes keypad 2's length */
    TEST_ASSERT_FALSE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    /* Keypad 1's device number 9 (a Z-Wave keypad's): dropped, the others load. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[19] = ZWAVE_KEYPAD_DEVICE;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    TEST_ASSERT_EQUAL_UINT32(0x02u, zba_devices_held(&got));
    b[19] = 0;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    /* Keypad 2 with keypad 1's number: the second dropped. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[36] = 1;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    TEST_ASSERT_EQUAL_UINT16(3, got.keypad[0].id);
    TEST_ASSERT_FALSE(got.keypad[1].used);
    /* Fob 2 with fob 1's user: dropped. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[66] = 3;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    /* Fob 1 with user 0, and with keypad 1's id: dropped. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[50] = 0;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[39] = 3;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    /* An unknown kind (a later format's): skipped, the rest load. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[38] = 7;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1), &got));
    TEST_ASSERT_EQUAL_UINT(3, assigned(&got));
    /* A later format's longer entry (a field appended to the last fob): read, the field skipped. */
    memcpy(b, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    b[53] = 0x10;
    b[sizeof(ACE_FIXTURE_V1)] = 0xAB;
    TEST_ASSERT_TRUE(decode_copy(b, sizeof(ACE_FIXTURE_V1) + 1u, &got));
    TEST_ASSERT_EQUAL_UINT(4, assigned(&got));
    TEST_ASSERT_TRUE(got.fob[1].panic);
}

/* ZB-024: a fob belongs to a master, partition master, standard, arm-only or guest user only
 * (alarm-devices.md §3) - the firmware and the simulator both refuse a fob whose user isn't. */
static void test_fob_may_be(void)
{
    TEST_ASSERT_TRUE(zba_fob_may_be(ALARM_AUTH_MASTER));
    TEST_ASSERT_TRUE(zba_fob_may_be(ALARM_AUTH_PARTITION_MASTER));
    TEST_ASSERT_TRUE(zba_fob_may_be(ALARM_AUTH_STANDARD));
    TEST_ASSERT_TRUE(zba_fob_may_be(ALARM_AUTH_ARM_ONLY));
    TEST_ASSERT_TRUE(zba_fob_may_be(ALARM_AUTH_GUEST));
    TEST_ASSERT_FALSE(zba_fob_may_be(ALARM_AUTH_INSTALLER));
    TEST_ASSERT_FALSE(zba_fob_may_be(ALARM_AUTH_DURESS));
    TEST_ASSERT_FALSE(zba_fob_may_be(ALARM_AUTH_NONE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_arm);
    RUN_TEST(test_golden_server_frames);
    RUN_TEST(test_golden_question_frames);
    RUN_TEST(test_decode_failures);
    RUN_TEST(test_constants_cross_check);
    RUN_TEST(test_status_for_table);
    RUN_TEST(test_note_for_table);
    RUN_TEST(test_keypad_arms_and_disarms);
    RUN_TEST(test_codes_never_compared_here);
    RUN_TEST(test_lockout_is_the_keypads);
    RUN_TEST(test_fob_acts_as_its_user);
    RUN_TEST(test_duress);
    RUN_TEST(test_unassigned_refused);
    RUN_TEST(test_damaged_frames);
    RUN_TEST(test_retransmission);
    RUN_TEST(test_busy_late_and_deadlines);
    RUN_TEST(test_panic_finds_room);
    RUN_TEST(test_retransmission_matches_mode);
    RUN_TEST(test_refused_arm_remembers_its_mode);
    RUN_TEST(test_panic_buttons);
    RUN_TEST(test_questions_answered);
    RUN_TEST(test_panel_status_to_keypads);
    RUN_TEST(test_model_rows);
    RUN_TEST(test_device_numbers);
    RUN_TEST(test_assignment_rules);
    RUN_TEST(test_store_fixture);
    RUN_TEST(test_store_damaged);
    RUN_TEST(test_fob_may_be);
    return UNITY_END();
}
