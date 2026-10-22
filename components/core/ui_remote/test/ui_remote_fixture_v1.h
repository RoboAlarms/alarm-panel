/*
 * ui_remote_fixture_v1.h - the remote backend's v1 bytes (docs/features/11, LINK-058; BL-223):
 * the call ids, and the arguments and answers of a few calls as ur_serve.c's layout comment
 * describes them. A record is a u16 length, then its fields in its table's order; integers little
 * endian at their C size, bools one byte, strings a u16 length and the bytes. test_ui_remote.c
 * decodes these with today's code to the values below and encodes the values back to exactly
 * these bytes. Built by hand from that layout, never from the encoder.
 * Frozen: never edit it; a new layout adds a new fixture beside it.
 */
#define UR_V1_GRANT_END         0x0001u
#define UR_V1_PROGRAM_ENTER     0x0002u
#define UR_V1_PROGRAM_EXIT      0x0003u
#define UR_V1_PROGRAM_KEEPALIVE 0x0004u
#define UR_V1_PROGRAM_SET_CODE  0x0005u
#define UR_V1_SET_CONFIG        0x0010u
#define UR_V1_USER_SET          0x0020u
#define UR_V1_USER_UPDATE       0x0021u
#define UR_V1_USER_DELETE       0x0022u
#define UR_V1_WEB_LOGIN_GET     0x0023u
#define UR_V1_WEB_LOGIN_SET     0x0024u
#define UR_V1_WEB_LOGIN_CLEAR   0x0025u
#define UR_V1_ZONE_SOURCES      0x0030u
#define UR_V1_ZONE_LEARN        0x0031u
#define UR_V1_ZONE_CANDIDATES   0x0032u
#define UR_V1_ZONE_BIND         0x0033u
#define UR_V1_ZONE_UNBIND       0x0034u
#define UR_V1_ZONE_INFO         0x0035u

/* PROGRAM_ENTER's arguments: the code "1234". Its answer: result 0 (OK), user 1. */
static const uint8_t UR_V1_ENTER_ARGS[6] = {0x04, 0x00, '1', '2', '3', '4'};
static const uint8_t UR_V1_ENTER_ANSWER[2] = {0x00, 0x01};

/* ZONE_BIND's arguments: zone 5, then a binding whose main input is kind 2, input 0, id 4, no
 * ref, mapped binary (mode 0) inverted, threshold and hysteresis 0, no states, grace 60 s,
 * response 350 ms, hold 0; the tamper and battery inputs empty (all zero). */
#define UR_V1_ADDR(kind, id) 0x0C, 0x00, (kind), 0x00, (id), 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00
#define UR_V1_MAP(inv, grace_lo, resp_lo, resp_hi) \
    0x12, 0x00, 0x00, (inv), 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, (grace_lo), 0x00, (resp_lo), \
    (resp_hi), 0x00, 0x00
static const uint8_t UR_V1_BIND_ARGS[112] = {
    0x05, 0x00,                     /* zone 5 */
    0x6C, 0x00,                     /* the binding: 108 bytes */
    0x22, 0x00, UR_V1_ADDR(2, 4), UR_V1_MAP(1, 0x3C, 0x5E, 0x01), /* main */
    0x22, 0x00, UR_V1_ADDR(0, 0), UR_V1_MAP(0, 0, 0, 0),          /* tamper */
    0x22, 0x00, UR_V1_ADDR(0, 0), UR_V1_MAP(0, 0, 0, 0),          /* battery */
};
