/*
 * test_plink_frame.c - the panel link's v1 frame (components/core/panel_link plink_frame.h,
 * docs/features/11 "Frame format (v1)", "Transports"): the 17-byte header byte for byte, every
 * rule plink_hdr_read drops a frame by, the type table, the CCM nonce's layout, sealing and
 * opening against golden frames made independently with Python's `cryptography` (AESCCM, 8-byte
 * tag; vectors_plink_frame.py beside this file), every single-byte change of a sealed frame
 * refused, the RFC 4303 replay window case by case and against a set model over 100 000 random
 * steps, and peer addresses.
 *
 * Requirements: LINK-003 (data frames sealed with AES-128-CCM, the nonce, the replay window
 * updated only after the tag, frames of another session or net refused), LINK-005 (1200 bytes at
 * most), LINK-017 (v1's header never changes: unknown types and flag bits dropped), LINK-021 (UDP
 * peer addresses on port 6056), LINK-043 and LINK-052 (the update, Smart Home and settings types,
 * each one way only: PREFS_SET and PREFS_REQ from the master alone).
 */
#include <string.h>

#include "panel_link/plink_frame.h"
#include "plink_test_port.h"
#include "unity.h"

static plink_test_port_t tp;
static plink_crypto_t port;

void setUp(void)
{
    plink_test_port_init(&tp, 0xF4A3E5u);
    port = plink_test_port(&tp);
}

void tearDown(void)
{
}

/* ---- golden bytes (vectors_plink_frame.py) ------------------------------------------------- */

/* PAIR_REQ from a keypad not paired yet: flags 0, net id 0, sender 0xFFFF, sid and seq 0. */
static const uint8_t HDR_PAIR_REQ[17] = {
    0x41, 0x4c, 0x01, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* STATUS from the master: SEALED | FROM_MASTER, net id 0x1234, sid 0xA1B2C3D4, seq 0x01020304. */
static const uint8_t HDR_STATUS[17] = {
    0x41, 0x4c, 0x01, 0x20, 0x03, 0x34, 0x12, 0x00, 0x00,
    0xd4, 0xc3, 0xb2, 0xa1, 0x04, 0x03, 0x02, 0x01,
};

/* Frame 1: that STATUS sealed under key 40..4f, 40 bytes of payload (byte i = 5 i + 1). */
static const uint8_t NONCE_1[13] = {
    0x4d, 0xd4, 0xc3, 0xb2, 0xa1, 0x00, 0x00, 0x34, 0x12, 0x04, 0x03, 0x02, 0x01,
};
static const uint8_t FRAME_1[65] = {
    0x41, 0x4c, 0x01, 0x20, 0x03, 0x34, 0x12, 0x00, 0x00, 0xd4, 0xc3, 0xb2, 0xa1,
    0x04, 0x03, 0x02, 0x01, 0xe2, 0x1a, 0x4e, 0xf1, 0xb4, 0x6e, 0xce, 0x88, 0x4f,
    0xed, 0x2f, 0x1b, 0x37, 0xa7, 0xe6, 0x34, 0x81, 0x2d, 0xae, 0xc9, 0x18, 0x97,
    0x54, 0x66, 0xe7, 0x3f, 0xdb, 0x1f, 0x9a, 0x46, 0x1a, 0x87, 0x9f, 0x29, 0x99,
    0x2a, 0xcd, 0x24, 0xe3, 0xae, 0xf4, 0x11, 0xe8, 0x3b, 0x2c, 0xa5, 0xb2, 0xb0,
};
/* Frame 2: HEARTBEAT from keypad 3 (SEALED), net id 0x1234, sid 0xA1B2C3D4, seq 7, key 50..5f,
 * 9 bytes of payload 90..98. */
static const uint8_t NONCE_2[13] = {
    0x4b, 0xd4, 0xc3, 0xb2, 0xa1, 0x03, 0x00, 0x34, 0x12, 0x07, 0x00, 0x00, 0x00,
};
static const uint8_t FRAME_2[34] = {
    0x41, 0x4c, 0x01, 0x2a, 0x01, 0x34, 0x12, 0x03, 0x00, 0xd4, 0xc3, 0xb2,
    0xa1, 0x07, 0x00, 0x00, 0x00, 0xe8, 0xec, 0x81, 0xcc, 0x36, 0xe9, 0x54,
    0x27, 0xf4, 0x6a, 0xff, 0xbf, 0x15, 0xda, 0xec, 0x02, 0xea,
};
/* Frame 3: ZONES_REQ from keypad 8 with no payload (the tag alone), net id 0xFFFE, sid 1,
 * seq 2^31 - 1, key 60..6f. */
static const uint8_t FRAME_3[25] = {
    0x41, 0x4c, 0x01, 0x22, 0x01, 0xfe, 0xff, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0x7f, 0xdc, 0x00, 0xb5, 0x09, 0xbd, 0xcb, 0x52, 0xeb,
};

typedef struct {
    const uint8_t *frame;
    size_t len;
    plink_hdr_t h;
    uint8_t key0; /* key = key0 .. key0 + 15 */
    uint8_t payload[40];
    size_t payload_len;
} golden_t;

static void fill_range(uint8_t *p, size_t n, unsigned first)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(first + i);
    }
}

static plink_hdr_t hdr(uint8_t type, uint8_t flags, uint16_t net_id, uint16_t sender, uint32_t sid,
                       uint32_t seq)
{
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.flags = flags;
    h.net_id = net_id;
    h.sender = sender;
    h.sid = sid;
    h.seq = seq;
    return h;
}

static void load_golden(size_t i, golden_t *g)
{
    memset(g, 0, sizeof *g);
    if (i == 0u) {
        g->frame = FRAME_1;
        g->len = sizeof FRAME_1;
        g->h = hdr(PLINK_T_STATUS, PLINK_FLAG_SEALED | PLINK_FLAG_FROM_MASTER, 0x1234u, 0u,
                   0xA1B2C3D4u, 0x01020304u);
        g->key0 = 0x40u;
        for (size_t k = 0; k < 40u; k++) {
            g->payload[k] = (uint8_t)(5u * k + 1u);
        }
        g->payload_len = 40u;
    } else if (i == 1u) {
        g->frame = FRAME_2;
        g->len = sizeof FRAME_2;
        g->h = hdr(PLINK_T_HEARTBEAT, PLINK_FLAG_SEALED, 0x1234u, 3u, 0xA1B2C3D4u, 7u);
        g->key0 = 0x50u;
        fill_range(g->payload, 9u, 0x90u);
        g->payload_len = 9u;
    } else {
        g->frame = FRAME_3;
        g->len = sizeof FRAME_3;
        g->h = hdr(PLINK_T_ZONES_REQ, PLINK_FLAG_SEALED, 0xFFFEu, 8u, 1u, 0x7FFFFFFFu);
        g->key0 = 0x60u;
        g->payload_len = 0u;
    }
}

static void key_of(const golden_t *g, uint8_t key[16])
{
    fill_range(key, 16u, g->key0);
}

static void expect_hdr(const plink_hdr_t *want, const plink_hdr_t *got)
{
    TEST_ASSERT_EQUAL_HEX8(want->type, got->type);
    TEST_ASSERT_EQUAL_HEX8(want->flags, got->flags);
    TEST_ASSERT_EQUAL_HEX16(want->net_id, got->net_id);
    TEST_ASSERT_EQUAL_HEX16(want->sender, got->sender);
    TEST_ASSERT_EQUAL_HEX32(want->sid, got->sid);
    TEST_ASSERT_EQUAL_HEX32(want->seq, got->seq);
}

static bool hdr_is_zero(const plink_hdr_t *h)
{
    return h->type == 0u && h->flags == 0u && h->net_id == 0u && h->sender == 0u && h->sid == 0u &&
           h->seq == 0u;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* ---- the header ---------------------------------------------------------------------------- */

/* LINK-017: v1's header byte for byte - 'A' 'L', version 1, type, flags, then net_id, sender,
 * sid and seq little-endian - for a plain PAIR_REQ and a sealed STATUS (laid out independently
 * by vectors_plink_frame.py), and read back field for field. */
static void test_header_golden(void)
{
    uint8_t out[PLINK_HDR_LEN];
    const plink_hdr_t req = hdr(PLINK_T_PAIR_REQ, 0u, 0u, PLINK_SENDER_UNPAIRED, 0u, 0u);
    plink_hdr_write(&req, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(HDR_PAIR_REQ, out, PLINK_HDR_LEN);
    const plink_hdr_t st = hdr(PLINK_T_STATUS, PLINK_FLAG_SEALED | PLINK_FLAG_FROM_MASTER, 0x1234u,
                               PLINK_SENDER_MASTER, 0xA1B2C3D4u, 0x01020304u);
    plink_hdr_write(&st, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(HDR_STATUS, out, PLINK_HDR_LEN);

    plink_hdr_t got;
    TEST_ASSERT_TRUE(plink_hdr_read(HDR_PAIR_REQ, sizeof HDR_PAIR_REQ, &got));
    expect_hdr(&req, &got);
    TEST_ASSERT_TRUE(plink_hdr_read(FRAME_1, sizeof FRAME_1, &got));
    expect_hdr(&st, &got);
    TEST_ASSERT_TRUE(plink_hdr_read(FRAME_1, sizeof FRAME_1, NULL)); /* a check alone */
}

/* What docs/features/11 "Messages" says of every type: its class and who sends it ('M' the
 * master, 'K' a keypad, 'B' both). Written from the document, not from the code. */
static const struct {
    uint8_t type;
    bool sealed;
    char dir;
} TYPES[] = {
    {0x01, false, 'K'}, {0x02, false, 'M'}, {0x03, false, 'K'}, {0x04, false, 'M'},
    {0x05, false, 'K'}, {0x06, false, 'M'}, {0x07, false, 'B'}, {0x10, false, 'K'},
    {0x11, false, 'M'}, {0x12, false, 'M'}, {0x13, false, 'M'}, {0x18, false, 'M'},
    {0x20, true, 'M'},  {0x21, true, 'M'},  {0x22, true, 'K'},  {0x23, true, 'K'},
    {0x24, true, 'M'},  {0x25, true, 'K'},  {0x26, true, 'M'},  {0x27, true, 'K'},
    {0x28, true, 'M'},  {0x29, true, 'M'},  {0x2A, true, 'K'},  {0x2B, true, 'M'},
    {0x2C, true, 'M'},  {0x2D, true, 'K'},  {0x2E, true, 'M'},  {0x2F, true, 'K'},
    {0x30, true, 'K'},  {0x31, true, 'M'},  {0x32, true, 'K'},  {0x33, true, 'M'},
    {0x34, true, 'M'},  {0x35, true, 'K'},  {0x36, true, 'K'},  {0x37, true, 'M'},
    {0x38, true, 'K'},  {0x39, true, 'M'},  {0x3A, true, 'M'},
    {0x3B, true, 'K'},  {0x3C, true, 'M'},
};
#define TYPE_COUNT (sizeof TYPES / sizeof TYPES[0])

static int type_index(unsigned t)
{
    for (size_t i = 0; i < TYPE_COUNT; i++) {
        if (TYPES[i].type == t) {
            return (int)i;
        }
    }
    return -1;
}

/* LINK-017: the type table for all 256 values - known exactly for the document's 39 types,
 * sealed from 0x20 up, sent by the master for the master's types and PAIR_FAIL (either way). */
static void test_type_table(void)
{
    for (unsigned t = 0; t < 256u; t++) {
        const int i = type_index(t);
        const uint8_t type = (uint8_t)t;
        TEST_ASSERT_EQUAL_MESSAGE(i >= 0, plink_type_known(type), "known");
        TEST_ASSERT_EQUAL_MESSAGE(t >= 0x20u, plink_type_sealed(type), "sealed");
        if (i >= 0) {
            TEST_ASSERT_EQUAL(TYPES[i].sealed, plink_type_sealed(type));
            TEST_ASSERT_EQUAL_MESSAGE(TYPES[i].dir != 'K', plink_type_from_master(type),
                                      "from the master");
        } else {
            TEST_ASSERT_FALSE(plink_type_from_master(type));
        }
    }
}

/* ---- plink_hdr_read's rules ---------------------------------------------------------------- */

static uint8_t fbuf[PLINK_FRAME_MAX + 16u];

/* Writes h at the start of a zeroed frame of len bytes and reads it back; a refused header
 * leaves *out zeroed, an accepted one reads as written. */
static bool reads(const plink_hdr_t *h, size_t len)
{
    memset(fbuf, 0, sizeof fbuf);
    plink_hdr_write(h, fbuf);
    plink_hdr_t out;
    memset(&out, 0xEE, sizeof out);
    const bool ok = plink_hdr_read(fbuf, len, &out);
    if (ok) {
        expect_hdr(h, &out);
    } else {
        TEST_ASSERT_TRUE(hdr_is_zero(&out));
    }
    return ok;
}

/* A header the rules accept, for type entry e sent from side dir ('M' or 'K'). */
static plink_hdr_t valid_hdr(size_t e, char dir)
{
    const bool sealed = TYPES[e].sealed;
    const bool master = dir == 'M';
    const uint16_t sender =
        master ? PLINK_SENDER_MASTER : (TYPES[e].type <= 0x07u ? PLINK_SENDER_UNPAIRED : 3u);
    return hdr(
        TYPES[e].type,
        (uint8_t)((sealed ? PLINK_FLAG_SEALED : 0u) | (master ? PLINK_FLAG_FROM_MASTER : 0u)),
        0x1234u, sender, sealed ? 0xA1B2C3D4u : 0u, sealed ? 5u : 0u);
}

/* LINK-003, LINK-005, LINK-017: every rule plink_hdr_read drops a frame by, for every known type
 * from each side that sends it - a header the rules accept is read; then, one change at a time:
 * an unknown flag bit (each of bits 2-7); SEALED not matching the type's class (with sid and seq
 * as the wrong class would have them, so only the class is wrong); FROM_MASTER not matching the
 * sender, and the sender not matching FROM_MASTER; flag and sender agreeing but sent the wrong
 * way for the type (only PAIR_FAIL goes both ways); sid or seq not 0 on a plain frame, 0 on a
 * sealed one; a sealed frame too short for its tag (17-24 bytes); 1200 bytes read and 1201 not;
 * anything shorter than a header. */
static void test_hdr_read_rules(void)
{
    for (size_t e = 0; e < TYPE_COUNT; e++) {
        const bool sealed = TYPES[e].sealed;
        const size_t len = sealed ? PLINK_SEALED_OVERHEAD + 10u : PLINK_HDR_LEN + 10u;
        for (int side = 0; side < 2; side++) {
            const char dir = side == 0 ? 'M' : 'K';
            if (TYPES[e].dir != 'B' && TYPES[e].dir != dir) {
                continue;
            }
            const plink_hdr_t h = valid_hdr(e, dir);
            const bool master = dir == 'M';
            TEST_ASSERT_TRUE(reads(&h, len));

            plink_hdr_t x;
            for (unsigned bit = 2; bit < 8u; bit++) {
                x = h;
                x.flags = (uint8_t)(x.flags | (1u << bit));
                TEST_ASSERT_FALSE(reads(&x, len));
            }

            x = h;
            x.flags ^= PLINK_FLAG_SEALED;
            x.sid = sealed ? 0u : 9u;
            x.seq = sealed ? 0u : 9u;
            TEST_ASSERT_FALSE(reads(&x, PLINK_SEALED_OVERHEAD + 10u));

            x = h;
            x.flags ^= PLINK_FLAG_FROM_MASTER;
            TEST_ASSERT_FALSE(reads(&x, len));
            x = h;
            x.sender = master ? 3u : PLINK_SENDER_MASTER;
            TEST_ASSERT_FALSE(reads(&x, len));
            x = h;
            x.flags ^= PLINK_FLAG_FROM_MASTER;
            x.sender = master ? 3u : PLINK_SENDER_MASTER;
            TEST_ASSERT_EQUAL(TYPES[e].dir == 'B', reads(&x, len));

            if (sealed) {
                x = h;
                x.sid = 0u;
                TEST_ASSERT_FALSE(reads(&x, len));
                x = h;
                x.seq = 0u;
                TEST_ASSERT_FALSE(reads(&x, len));
                for (size_t n = PLINK_HDR_LEN; n < PLINK_SEALED_OVERHEAD; n++) {
                    TEST_ASSERT_FALSE(reads(&h, n));
                }
                TEST_ASSERT_TRUE(reads(&h, PLINK_SEALED_OVERHEAD)); /* an empty payload */
            } else {
                x = h;
                x.sid = 1u;
                TEST_ASSERT_FALSE(reads(&x, len));
                x = h;
                x.seq = 1u;
                TEST_ASSERT_FALSE(reads(&x, len));
                TEST_ASSERT_TRUE(reads(&h, PLINK_HDR_LEN)); /* the header alone */
            }
            TEST_ASSERT_TRUE(reads(&h, PLINK_FRAME_MAX));
            TEST_ASSERT_FALSE(reads(&h, PLINK_FRAME_MAX + 1u));
            for (size_t n = 0; n < PLINK_HDR_LEN; n++) {
                TEST_ASSERT_FALSE(reads(&h, n));
            }
        }
    }
}

/* LINK-043, LINK-052: the types of updates, Smart Home and settings go one way only, the
 * directions written from the document here once more: a keypad's PREFS_SET or PREFS_REQ (a keypad
 * never sets another panel's settings), a master's DEV_ACT or PREFS, a keypad's FW_ORDER, FW_PAGE,
 * DEV_STATE, DEV_ACK or DEV_LIST_PAGE, a master's FW_INFO, FW_STATE, FW_REQ, DEV_LIST_REQ or
 * DEV_STATE_REQ: each dropped, its right way read. 0x3B, the next free type, is unknown. */
static void test_new_types_one_way(void)
{
    static const uint8_t from_master[] = {
        PLINK_T_FW_ORDER, PLINK_T_FW_PAGE,  PLINK_T_DEV_LIST_PAGE, PLINK_T_DEV_STATE,
        PLINK_T_DEV_ACK,  PLINK_T_PREFS_REQ, PLINK_T_PREFS_SET,
    };
    static const uint8_t from_keypad[] = {
        PLINK_T_FW_INFO,       PLINK_T_FW_STATE, PLINK_T_FW_REQ, PLINK_T_DEV_LIST_REQ,
        PLINK_T_DEV_STATE_REQ, PLINK_T_DEV_ACT,  PLINK_T_PREFS,
    };
    const uint8_t sm = PLINK_FLAG_SEALED | PLINK_FLAG_FROM_MASTER;
    const size_t len = PLINK_SEALED_OVERHEAD + 4u;
    for (size_t i = 0; i < sizeof from_master; i++) {
        const plink_hdr_t ok = hdr(from_master[i], sm, 0x1234u, PLINK_SENDER_MASTER, 7u, 1u);
        const plink_hdr_t no = hdr(from_master[i], PLINK_FLAG_SEALED, 0x1234u, 3u, 7u, 1u);
        TEST_ASSERT_TRUE(plink_type_known(from_master[i]));
        TEST_ASSERT_TRUE(plink_type_sealed(from_master[i]));
        TEST_ASSERT_TRUE(reads(&ok, len));
        TEST_ASSERT_FALSE(reads(&no, len));
    }
    for (size_t i = 0; i < sizeof from_keypad; i++) {
        const plink_hdr_t ok = hdr(from_keypad[i], PLINK_FLAG_SEALED, 0x1234u, 3u, 7u, 1u);
        const plink_hdr_t no = hdr(from_keypad[i], sm, 0x1234u, PLINK_SENDER_MASTER, 7u, 1u);
        TEST_ASSERT_TRUE(plink_type_known(from_keypad[i]));
        TEST_ASSERT_TRUE(plink_type_sealed(from_keypad[i]));
        TEST_ASSERT_TRUE(reads(&ok, len));
        TEST_ASSERT_FALSE(reads(&no, len));
    }
    /* Named once more: a keypad's PREFS_SET, and a DEV_ACT sent as the master's. */
    const plink_hdr_t set_k = hdr(PLINK_T_PREFS_SET, PLINK_FLAG_SEALED, 0x1234u, 3u, 7u, 1u);
    TEST_ASSERT_FALSE(reads(&set_k, len));
    const plink_hdr_t act_m = hdr(PLINK_T_DEV_ACT, sm, 0x1234u, PLINK_SENDER_MASTER, 7u, 1u);
    TEST_ASSERT_FALSE(reads(&act_m, len));
    const plink_hdr_t next = hdr(0x3Du, sm, 0x1234u, PLINK_SENDER_MASTER, 7u, 1u);
    TEST_ASSERT_FALSE(plink_type_known(0x3Du));
    TEST_ASSERT_FALSE(reads(&next, len));
}

/* LINK-017: a header that is not v1's - either magic byte wrong, a version other than 1 - and an
 * unknown type, whatever its flags, sender, sid and seq, is dropped; so is no frame at all. */
static void test_hdr_read_not_v1(void)
{
    plink_hdr_t out;
    uint8_t f[sizeof FRAME_1];
    for (size_t at = 0; at < 3u; at++) {
        for (unsigned v = 0; v < 256u; v++) {
            memcpy(f, FRAME_1, sizeof f);
            if (v == f[at]) {
                continue;
            }
            f[at] = (uint8_t)v;
            memset(&out, 0xEE, sizeof out);
            TEST_ASSERT_FALSE(plink_hdr_read(f, sizeof f, &out));
            TEST_ASSERT_TRUE(hdr_is_zero(&out));
        }
    }
    for (unsigned t = 0; t < 256u; t++) {
        if (type_index(t) >= 0) {
            continue;
        }
        for (unsigned flags = 0; flags < 4u; flags++) {
            const bool sealed = (flags & PLINK_FLAG_SEALED) != 0u;
            const bool master = (flags & PLINK_FLAG_FROM_MASTER) != 0u;
            const plink_hdr_t h = hdr((uint8_t)t, (uint8_t)flags, 0x1234u, master ? 0u : 3u,
                                      sealed ? 1u : 0u, sealed ? 1u : 0u);
            TEST_ASSERT_FALSE(reads(&h, PLINK_SEALED_OVERHEAD + 4u));
        }
    }
    TEST_ASSERT_FALSE(plink_hdr_read(NULL, 100u, &out));
}

/* LINK-003: the CCM nonce is dir || sid || sender || net_id || seq, little-endian, dir 'M' (0x4D)
 * from the master and 'K' (0x4B) from a keypad: the golden frames' nonces, and a header whose
 * every field is distinct, laid out byte by byte. */
static void test_nonce_layout(void)
{
    uint8_t n[PLINK_CCM_NONCE_LEN];
    golden_t g;
    load_golden(0u, &g);
    plink_nonce(&g.h, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(NONCE_1, n, sizeof n);
    load_golden(1u, &g);
    plink_nonce(&g.h, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(NONCE_2, n, sizeof n);

    const plink_hdr_t k =
        hdr(PLINK_T_CMD, PLINK_FLAG_SEALED, 0x0201u, 0x0403u, 0x08070605u, 0x0C0B0A09u);
    static const uint8_t want_k[13] = {0x4B, 0x05, 0x06, 0x07, 0x08, 0x03, 0x04,
                                       0x01, 0x02, 0x09, 0x0A, 0x0B, 0x0C};
    plink_nonce(&k, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_k, n, sizeof n);
    const plink_hdr_t m = hdr(PLINK_T_RESULT, PLINK_FLAG_SEALED | PLINK_FLAG_FROM_MASTER, 0x0201u,
                              0u, 0x08070605u, 0x0C0B0A09u);
    static const uint8_t want_m[13] = {0x4D, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00,
                                       0x01, 0x02, 0x09, 0x0A, 0x0B, 0x0C};
    plink_nonce(&m, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_m, n, sizeof n);
}

/* ---- sealed and plain frames --------------------------------------------------------------- */

/* LINK-003: plink_frame_seal makes the golden frames exactly - a 40-byte STATUS from the master,
 * a 9-byte HEARTBEAT from keypad 3, an empty ZONES_REQ from keypad 8 - sealed independently with
 * Python's AESCCM (8-byte tag, the header the associated data); each reads and opens back to its
 * payload. Sealed where the payload already lies (out + 17) it is the same frame. */
static void test_sealed_golden(void)
{
    for (size_t i = 0; i < 3u; i++) {
        golden_t g;
        load_golden(i, &g);
        uint8_t key[16], out[PLINK_FRAME_MAX], pt[PLINK_FRAME_MAX];
        key_of(&g, key);
        size_t len = 0u, pt_len = 99u;
        TEST_ASSERT_TRUE(
            plink_frame_seal(&port, key, &g.h, g.payload, g.payload_len, out, sizeof out, &len));
        TEST_ASSERT_EQUAL_UINT(g.len, len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(g.frame, out, len);

        plink_hdr_t h;
        TEST_ASSERT_TRUE(plink_hdr_read(g.frame, g.len, &h));
        expect_hdr(&g.h, &h);
        TEST_ASSERT_TRUE(plink_frame_open(&port, key, &h, g.frame, g.len, pt, sizeof pt, &pt_len));
        TEST_ASSERT_EQUAL_UINT(g.payload_len, pt_len);
        if (pt_len > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(g.payload, pt, pt_len);
        }

        memset(out, 0, sizeof out);
        memcpy(&out[PLINK_HDR_LEN], g.payload, g.payload_len);
        TEST_ASSERT_TRUE(plink_frame_seal(&port, key, &g.h, &out[PLINK_HDR_LEN], g.payload_len, out,
                                          sizeof out, &len));
        TEST_ASSERT_EQUAL_UINT(g.len, len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(g.frame, out, len);
    }
}

/* LINK-003: a sealed frame opens only as it was sealed - every byte of the golden frames changed
 * to each of its 255 other values is either dropped by plink_hdr_read or refused by
 * plink_frame_open (with nothing decrypted left), and so is the frame cut short at every length
 * or with a byte added. */
static void test_sealed_every_byte_flip(void)
{
    uint8_t f[PLINK_FRAME_MAX], pt[PLINK_FRAME_MAX];
    for (size_t i = 0; i < 3u; i++) {
        golden_t g;
        load_golden(i, &g);
        uint8_t key[16];
        key_of(&g, key);
        size_t refused_by_header = 0u, refused_by_tag = 0u;
        for (size_t at = 0; at < g.len; at++) {
            for (unsigned x = 1; x < 256u; x++) {
                memcpy(f, g.frame, g.len);
                f[at] ^= (uint8_t)x;
                plink_hdr_t h;
                if (!plink_hdr_read(f, g.len, &h)) {
                    refused_by_header++;
                    continue;
                }
                size_t pt_len = 99u;
                memset(pt, 0xEE, sizeof pt);
                TEST_ASSERT_FALSE(
                    plink_frame_open(&port, key, &h, f, g.len, pt, sizeof pt, &pt_len));
                TEST_ASSERT_EQUAL_UINT(0u, pt_len);
                TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
                refused_by_tag++;
            }
        }
        /* Both guards were exercised: the header's fields and the payload, tag and the header's
         * free fields (net id, sid, seq) under the tag. */
        TEST_ASSERT_TRUE(refused_by_header > 0u && refused_by_tag > 0u);
        TEST_ASSERT_EQUAL_UINT(g.len * 255u, refused_by_header + refused_by_tag);

        for (size_t n = 0; n <= g.len + 1u; n++) {
            if (n == g.len) {
                continue;
            }
            memset(f, 0, sizeof f);
            memcpy(f, g.frame, n < g.len ? n : g.len);
            plink_hdr_t h;
            size_t pt_len = 99u;
            const bool hdr_ok = plink_hdr_read(f, n, &h);
            TEST_ASSERT_FALSE(hdr_ok &&
                              plink_frame_open(&port, key, &h, f, n, pt, sizeof pt, &pt_len));
        }
    }
}

/* LINK-003: frames of another session, net or sequence number are refused: the golden STATUS
 * with its header rewritten to another sid, net id or seq (each a header plink_hdr_read accepts)
 * fails its tag, and so does opening it under the other direction's key. */
static void test_open_other_session_net_seq(void)
{
    golden_t g;
    load_golden(0u, &g);
    uint8_t key[16], other[16], f[sizeof FRAME_1], pt[64];
    key_of(&g, key);
    fill_range(other, sizeof other, 0x50u);
    size_t pt_len;
    plink_hdr_t h;

    for (int change = 0; change < 3; change++) {
        plink_hdr_t x = g.h;
        if (change == 0) {
            x.sid++;
        } else if (change == 1) {
            x.net_id ^= 0x0100u;
        } else {
            x.seq++;
        }
        memcpy(f, FRAME_1, sizeof f);
        plink_hdr_write(&x, f);
        TEST_ASSERT_TRUE(plink_hdr_read(f, sizeof f, &h));
        TEST_ASSERT_FALSE(plink_frame_open(&port, key, &h, f, sizeof f, pt, sizeof pt, &pt_len));
    }
    TEST_ASSERT_TRUE(plink_hdr_read(FRAME_1, sizeof FRAME_1, &h));
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, other, &h, FRAME_1, sizeof FRAME_1, pt, sizeof pt, &pt_len));
    TEST_ASSERT_TRUE(
        plink_frame_open(&port, key, &h, FRAME_1, sizeof FRAME_1, pt, sizeof pt, &pt_len));
}

/* LINK-003, LINK-005: round trips from both sides at payload sizes from 0 to the largest that
 * fits 1200 bytes (1175); one byte more is refused, as is a buffer one byte short. */
static void test_sealed_sizes(void)
{
    static const size_t sizes[] = {0u, 1u, 15u, 16u, 17u, 100u, 1174u, PLINK_PAYLOAD_MAX};
    static uint8_t payload[PLINK_FRAME_MAX], out[PLINK_FRAME_MAX + 8u], pt[PLINK_FRAME_MAX];
    uint8_t key[16];
    fill_range(key, sizeof key, 0x21u);
    fill_range(payload, sizeof payload, 0x33u);
    TEST_ASSERT_EQUAL_UINT(1175u, PLINK_PAYLOAD_MAX);
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (int side = 0; side < 2; side++) {
            const plink_hdr_t h =
                side == 0 ? hdr(PLINK_T_CFG_PAGE, PLINK_FLAG_SEALED | PLINK_FLAG_FROM_MASTER,
                                0x0BADu, 0u, 77u, (uint32_t)i + 1u)
                          : hdr(PLINK_T_CMD, PLINK_FLAG_SEALED, 0x0BADu, 8u, 77u, (uint32_t)i + 1u);
            size_t len = 0u, pt_len = 0u;
            TEST_ASSERT_TRUE(
                plink_frame_seal(&port, key, &h, payload, sizes[i], out, sizeof out, &len));
            TEST_ASSERT_EQUAL_UINT(sizes[i] + PLINK_SEALED_OVERHEAD, len);
            plink_hdr_t r;
            TEST_ASSERT_TRUE(plink_hdr_read(out, len, &r));
            TEST_ASSERT_TRUE(plink_frame_open(&port, key, &r, out, len, pt, sizeof pt, &pt_len));
            TEST_ASSERT_EQUAL_UINT(sizes[i], pt_len);
            if (sizes[i] > 0u) {
                TEST_ASSERT_EQUAL_MEMORY(payload, pt, sizes[i]);
                /* A buffer one byte short. */
                memset(pt, 0xEE, sizes[i]);
                TEST_ASSERT_FALSE(
                    plink_frame_open(&port, key, &r, out, len, pt, sizes[i] - 1u, &pt_len));
                TEST_ASSERT_EQUAL_UINT(0u, pt_len);
                TEST_ASSERT_TRUE(all_zero(pt, sizes[i] - 1u));
            }
            TEST_ASSERT_FALSE(plink_frame_seal(&port, key, &h, payload, sizes[i], out,
                                               sizes[i] + PLINK_SEALED_OVERHEAD - 1u, &len));
            TEST_ASSERT_EQUAL_UINT(0u, len);
        }
    }
    size_t len = 99u;
    const plink_hdr_t h = hdr(PLINK_T_CMD, PLINK_FLAG_SEALED, 1u, 1u, 1u, 1u);
    TEST_ASSERT_FALSE(
        plink_frame_seal(&port, key, &h, payload, PLINK_PAYLOAD_MAX + 1u, out, sizeof out, &len));
    TEST_ASSERT_EQUAL_UINT(0u, len);
}

/* LINK-003, LINK-017: plink_frame_seal builds nothing a receiver would drop - a plain type, the
 * SEALED flag missing, sid or seq 0, FROM_MASTER not matching the sender or the type's direction,
 * an unknown flag bit - and nothing without its arguments; a port call failing while sealing
 * (each of them) leaves no frame and no plaintext in out. */
static void test_seal_refusals(void)
{
    uint8_t key[16], out[128], payload[20];
    fill_range(key, sizeof key, 0x01u);
    fill_range(payload, sizeof payload, 0xA0u);
    const plink_hdr_t good = hdr(PLINK_T_HEARTBEAT, PLINK_FLAG_SEALED, 9u, 2u, 5u, 6u);
    size_t len = 0u;
    TEST_ASSERT_TRUE(
        plink_frame_seal(&port, key, &good, payload, sizeof payload, out, sizeof out, &len));
    const plink_hdr_t bad[] = {
        hdr(PLINK_T_HELLO, PLINK_FLAG_SEALED, 9u, 2u, 5u, 6u),     /* a plain type */
        hdr(PLINK_T_HEARTBEAT, 0u, 9u, 2u, 5u, 6u),                /* SEALED missing */
        hdr(PLINK_T_HEARTBEAT, PLINK_FLAG_SEALED, 9u, 2u, 0u, 6u), /* sid 0 */
        hdr(PLINK_T_HEARTBEAT, PLINK_FLAG_SEALED, 9u, 2u, 5u, 0u), /* seq 0 */
        hdr(PLINK_T_HEARTBEAT, 0x03u, 9u, 2u, 5u, 6u),             /* FROM_MASTER, sender 2 */
        hdr(PLINK_T_HEARTBEAT, PLINK_FLAG_SEALED, 9u, 0u, 5u, 6u), /* sender 0, no flag */
        hdr(PLINK_T_HEARTBEAT, 0x03u, 9u, 0u, 5u, 6u),             /* the master's heartbeat */
        hdr(PLINK_T_STATUS, PLINK_FLAG_SEALED, 9u, 2u, 5u, 6u),    /* a keypad's STATUS */
        hdr(PLINK_T_HEARTBEAT, PLINK_FLAG_SEALED | 0x80u, 9u, 2u, 5u, 6u), /* unknown flag */
        hdr(0x3Du, PLINK_FLAG_SEALED, 9u, 2u, 5u, 6u),                     /* unknown type */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        len = 99u;
        TEST_ASSERT_FALSE(
            plink_frame_seal(&port, key, &bad[i], payload, sizeof payload, out, sizeof out, &len));
        TEST_ASSERT_EQUAL_UINT(0u, len);
    }
    TEST_ASSERT_FALSE(
        plink_frame_seal(NULL, key, &good, payload, sizeof payload, out, sizeof out, &len));
    TEST_ASSERT_FALSE(
        plink_frame_seal(&port, NULL, &good, payload, sizeof payload, out, sizeof out, &len));
    TEST_ASSERT_FALSE(
        plink_frame_seal(&port, key, NULL, payload, sizeof payload, out, sizeof out, &len));
    TEST_ASSERT_FALSE(plink_frame_seal(&port, key, &good, NULL, 4u, out, sizeof out, &len));
    TEST_ASSERT_FALSE(
        plink_frame_seal(&port, key, &good, payload, sizeof payload, NULL, sizeof out, &len));
    TEST_ASSERT_FALSE(
        plink_frame_seal(&port, key, &good, payload, sizeof payload, out, sizeof out, NULL));

    /* Every AES call of the seal failing in turn: B_0, the header's two blocks, 2 x 2 for the
     * payload, A_0. */
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);
    TEST_ASSERT_TRUE(
        plink_frame_seal(&port, key, &good, payload, sizeof payload, out, sizeof out, &len));
    const uint32_t calls = tp.hc.calls;
    TEST_ASSERT_EQUAL_UINT32(8u, calls);
    for (uint32_t k = 0; k < calls; k++) {
        plink_test_port_fail_at(&tp, k);
        memset(out, 0xEE, sizeof out);
        len = 99u;
        TEST_ASSERT_FALSE(
            plink_frame_seal(&port, key, &good, payload, sizeof payload, out, sizeof out, &len));
        TEST_ASSERT_EQUAL_UINT(0u, len);
        TEST_ASSERT_TRUE(all_zero(out, PLINK_SEALED_OVERHEAD + sizeof payload));
    }
}

/* LINK-003: plink_frame_open takes only a sealed header it can check, and a frame long enough for
 * its tag; anything else is refused with pt zeroed. */
static void test_open_refusals(void)
{
    golden_t g;
    load_golden(1u, &g);
    uint8_t key[16], pt[64];
    key_of(&g, key);
    size_t pt_len = 99u;
    plink_hdr_t plain = hdr(PLINK_T_HELLO, 0u, 0x1234u, 3u, 0u, 0u);
    memset(pt, 0xEE, sizeof pt);
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, key, &plain, FRAME_2, sizeof FRAME_2, pt, sizeof pt, &pt_len));
    TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
    TEST_ASSERT_EQUAL_UINT(0u, pt_len);
    TEST_ASSERT_FALSE(plink_frame_open(&port, key, &g.h, FRAME_2, PLINK_SEALED_OVERHEAD - 1u, pt,
                                       sizeof pt, &pt_len));
    TEST_ASSERT_FALSE(
        plink_frame_open(NULL, key, &g.h, FRAME_2, sizeof FRAME_2, pt, sizeof pt, &pt_len));
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, NULL, &g.h, FRAME_2, sizeof FRAME_2, pt, sizeof pt, &pt_len));
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, key, NULL, FRAME_2, sizeof FRAME_2, pt, sizeof pt, &pt_len));
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, key, &g.h, NULL, sizeof FRAME_2, pt, sizeof pt, &pt_len));
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, key, &g.h, FRAME_2, sizeof FRAME_2, NULL, sizeof pt, &pt_len));
    TEST_ASSERT_FALSE(
        plink_frame_open(&port, key, &g.h, FRAME_2, sizeof FRAME_2, pt, sizeof pt, NULL));
    TEST_ASSERT_TRUE(
        plink_frame_open(&port, key, &g.h, FRAME_2, sizeof FRAME_2, pt, sizeof pt, &pt_len));
    TEST_ASSERT_EQUAL_UINT(9u, pt_len);
}

/* LINK-005, LINK-017: plain frames carry the pairing and handshake messages and the hints:
 * header and payload as given, up to 1200 bytes; a sealed type, the SEALED flag, sid or seq not
 * 0, a FROM_MASTER that doesn't match, no room are refused; built where the payload already
 * lies, it is the same frame. */
static void test_plain_frames(void)
{
    static uint8_t payload[PLINK_FRAME_MAX], out[PLINK_FRAME_MAX + 8u];
    fill_range(payload, sizeof payload, 0x10u);
    const plink_hdr_t req = hdr(PLINK_T_PAIR_REQ, 0u, 0u, PLINK_SENDER_UNPAIRED, 0u, 0u);
    size_t len = 0u;
    TEST_ASSERT_TRUE(plink_frame_plain(&req, payload, 5u, out, sizeof out, &len));
    TEST_ASSERT_EQUAL_UINT(PLINK_HDR_LEN + 5u, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(HDR_PAIR_REQ, out, PLINK_HDR_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &out[PLINK_HDR_LEN], 5u);
    plink_hdr_t r;
    TEST_ASSERT_TRUE(plink_hdr_read(out, len, &r));
    expect_hdr(&req, &r);

    const size_t most = PLINK_FRAME_MAX - PLINK_HDR_LEN;
    TEST_ASSERT_TRUE(plink_frame_plain(&req, payload, most, out, sizeof out, &len));
    TEST_ASSERT_EQUAL_UINT(PLINK_FRAME_MAX, len);
    TEST_ASSERT_FALSE(plink_frame_plain(&req, payload, most + 1u, out, sizeof out, &len));
    TEST_ASSERT_EQUAL_UINT(0u, len);
    TEST_ASSERT_FALSE(plink_frame_plain(&req, payload, 5u, out, PLINK_HDR_LEN + 4u, &len));
    TEST_ASSERT_TRUE(plink_frame_plain(&req, NULL, 0u, out, PLINK_HDR_LEN, &len));
    TEST_ASSERT_EQUAL_UINT(PLINK_HDR_LEN, len);

    const plink_hdr_t bad[] = {
        hdr(PLINK_T_STATUS, 0x03u, 1u, 0u, 1u, 1u),                         /* a sealed type */
        hdr(PLINK_T_PAIR_REQ, PLINK_FLAG_SEALED, 0u, 0xFFFFu, 1u, 1u),      /* SEALED */
        hdr(PLINK_T_PAIR_REQ, 0u, 0u, 0xFFFFu, 1u, 0u),                     /* sid */
        hdr(PLINK_T_PAIR_REQ, 0u, 0u, 0xFFFFu, 0u, 1u),                     /* seq */
        hdr(PLINK_T_PAIR_REQ, PLINK_FLAG_FROM_MASTER, 0u, 0xFFFFu, 0u, 0u), /* flag vs sender */
        hdr(PLINK_T_WELCOME, 0u, 1u, 3u, 0u, 0u),                           /* a keypad's WELCOME */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        len = 99u;
        TEST_ASSERT_FALSE(plink_frame_plain(&bad[i], payload, 5u, out, sizeof out, &len));
        TEST_ASSERT_EQUAL_UINT(0u, len);
    }

    /* In place: the payload encoded after the header's room. */
    memset(out, 0, sizeof out);
    memcpy(&out[PLINK_HDR_LEN], payload, 40u);
    TEST_ASSERT_TRUE(plink_frame_plain(&req, &out[PLINK_HDR_LEN], 40u, out, sizeof out, &len));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(HDR_PAIR_REQ, out, PLINK_HDR_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, &out[PLINK_HDR_LEN], 40u);
}

/* ---- the replay window --------------------------------------------------------------------- */

/* Checks seq, which must give `want`, and that the check changed nothing. */
static void expect_check(const plink_window_t *w, uint32_t seq, bool want)
{
    const plink_window_t before = *w;
    TEST_ASSERT_EQUAL_MESSAGE(want, plink_window_check(w, seq), "window check");
    TEST_ASSERT_EQUAL_UINT32(before.top, w->top);
    TEST_ASSERT_TRUE(before.bits == w->bits);
}

/* A frame that verified: checked as new, then committed. */
static void accept_seq(plink_window_t *w, uint32_t seq)
{
    expect_check(w, seq, true);
    plink_window_commit(w, seq);
    expect_check(w, seq, false);
}

/* LINK-003: RFC 4303 section 3.4.3's window of 64, case by case - a zeroed window takes any seq
 * from 1, never 0; a seq once committed is refused; seqs below the top but within 64 are taken
 * once each, in any order; 64 or more below the top are refused even if never seen; a jump of
 * exactly 64 or more forgets everything, a jump of 63 keeps the old top as the oldest bit; a
 * commit of 0 or of a seq outside the window changes nothing; the check never changes the
 * window; up to 2^31 - 1. */
static void test_window_rfc4303(void)
{
    plink_window_t w;
    memset(&w, 0, sizeof w);
    expect_check(&w, 0u, false);
    expect_check(&w, 1u, true);
    expect_check(&w, 0x7FFFFFFFu, true);
    accept_seq(&w, 1u);
    accept_seq(&w, 3u);
    accept_seq(&w, 2u); /* late, within the window */
    expect_check(&w, 1u, false);
    expect_check(&w, 4u, true);

    accept_seq(&w, 70u);         /* top 70: 7..70 is the window */
    expect_check(&w, 7u, true);  /* 63 below, never seen */
    expect_check(&w, 6u, false); /* 64 below */
    expect_check(&w, 3u, false); /* seen, and out of the window anyway */
    accept_seq(&w, 7u);
    accept_seq(&w, 69u);
    accept_seq(&w, 40u);

    plink_window_commit(&w, 0u);
    plink_window_commit(&w, 5u); /* outside: ignored */
    expect_check(&w, 41u, true);
    expect_check(&w, 40u, false);
    TEST_ASSERT_EQUAL_UINT32(70u, w.top);

    accept_seq(&w, 133u); /* a jump of 63: 70 becomes bit 63 */
    expect_check(&w, 70u, false);
    expect_check(&w, 69u, false); /* 64 below now */
    expect_check(&w, 71u, true);
    accept_seq(&w, 197u);          /* a jump of exactly 64: only 197 is known */
    expect_check(&w, 133u, false); /* 64 below */
    expect_check(&w, 134u, true);
    accept_seq(&w, 134u);
    accept_seq(&w, 196u);

    accept_seq(&w, 0x7FFFFFFFu);
    expect_check(&w, 0x7FFFFFFFu - 63u, true);
    expect_check(&w, 0x7FFFFFFFu - 64u, false);
    expect_check(&w, 197u, false);
}

/* A seeded SplitMix64 for the differential test. */
static uint64_t rng;

static uint64_t rnd(void)
{
    uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#define MODEL_MAX (1u << 22) /* seqs the model tracks */
static uint8_t model_seen[MODEL_MAX / 8u];

static bool model_has(uint32_t seq)
{
    return (model_seen[seq / 8u] & (1u << (seq % 8u))) != 0u;
}

/* LINK-003: the window against the simplest model of RFC 4303 - a seq is new when it is not 0,
 * was never committed, and is above the highest committed or less than 64 below it - over
 * 100 000 random steps: seqs around the top (replays, late frames, frames just too old), small
 * steps ahead, jumps past the window, and 0. Every check agrees with the model and changes
 * nothing; 9 in 10 frames found new are committed (the 10th failed its tag), and now and then a
 * refused one is committed by mistake, which must change nothing the model can see. */
static void test_window_differential(void)
{
    plink_window_t w;
    memset(&w, 0, sizeof w);
    memset(model_seen, 0, sizeof model_seen);
    uint32_t top = 0u;
    rng = 0x5EEDu;
    size_t fresh = 0u, replays = 0u, too_old = 0u, zeros = 0u;
    for (uint32_t step = 0; step < 100000u; step++) {
        const uint64_t r = rnd() % 100u;
        uint32_t seq;
        if (r < 2u) {
            seq = 0u;
        } else if (r < 60u) {
            const uint32_t back = (uint32_t)(rnd() % 80u);
            seq = back < top ? top - back : 1u;
        } else if (r < 95u) {
            seq = top + 1u + (uint32_t)(rnd() % 8u);
        } else {
            seq = top + 1u + (uint32_t)(rnd() % 200u);
        }
        TEST_ASSERT_TRUE(seq < MODEL_MAX);

        const bool in_window = seq > top || top - seq < 64u;
        const bool want = seq != 0u && !model_has(seq) && in_window;
        expect_check(&w, seq, want);
        if (seq == 0u) {
            zeros++;
        } else if (want) {
            fresh++;
        } else if (model_has(seq) && in_window) {
            replays++;
        } else {
            too_old++;
        }

        const bool commit = want ? rnd() % 10u != 0u : rnd() % 50u == 0u;
        if (commit) {
            plink_window_commit(&w, seq);
            if (seq != 0u) {
                model_seen[seq / 8u] = (uint8_t)(model_seen[seq / 8u] | (1u << (seq % 8u)));
                if (seq > top) {
                    top = seq;
                }
            }
        }
        TEST_ASSERT_EQUAL_UINT32(top, w.top);
    }
    /* Every kind of answer came up often. */
    TEST_ASSERT_TRUE(fresh > 20000u && replays > 5000u && too_old > 5000u && zeros > 1000u);
}

/* ---- addresses ----------------------------------------------------------------------------- */

/* LINK-021: a UDP peer address is the IPv4 address and the port in network order (6056 = 17 a8);
 * two addresses are equal only with the same transport, length and bytes; as text "a.b.c.d:port"
 * for UDP, "aa:bb:cc:dd:ee:ff" for ESP-NOW, "rf:" and hex for 433 MHz (at most 10 bytes shown),
 * "-" for none, nothing or a malformed one. */
static void test_addresses(void)
{
    static const uint8_t ip[4] = {192, 0, 2, 10};
    static const uint8_t ip2[4] = {192, 0, 2, 11};
    const plink_addr_t a = plink_addr_udp(ip, (uint16_t)PLINK_UDP_PORT);
    TEST_ASSERT_EQUAL_UINT8(PLINK_TP_UDP, a.tp);
    TEST_ASSERT_EQUAL_UINT8(6u, a.len);
    static const uint8_t want[6] = {192, 0, 2, 10, 0x17, 0xa8};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, a.a, 6u);
    TEST_ASSERT_TRUE(all_zero(&a.a[6], PLINK_ADDR_MAX - 6u));

    char text[24];
    plink_addr_text(&a, text);
    TEST_ASSERT_EQUAL_STRING("192.0.2.10:6056", text);
    const plink_addr_t widest = plink_addr_udp((const uint8_t[4]){255, 255, 255, 255}, 65535u);
    plink_addr_text(&widest, text);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255:65535", text);
    const plink_addr_t zero = plink_addr_udp((const uint8_t[4]){0, 0, 0, 0}, 0u);
    plink_addr_text(&zero, text);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0:0", text);

    const plink_addr_t same = plink_addr_udp(ip, 6056u);
    const plink_addr_t other_ip = plink_addr_udp(ip2, 6056u);
    const plink_addr_t other_port = plink_addr_udp(ip, 6057u);
    TEST_ASSERT_TRUE(plink_addr_equal(&a, &same));
    TEST_ASSERT_FALSE(plink_addr_equal(&a, &other_ip));
    TEST_ASSERT_FALSE(plink_addr_equal(&a, &other_port));
    plink_addr_t espnow = a;
    espnow.tp = PLINK_TP_ESPNOW;
    TEST_ASSERT_FALSE(plink_addr_equal(&a, &espnow));
    plink_addr_t shorter = a;
    shorter.len = 4u;
    TEST_ASSERT_FALSE(plink_addr_equal(&a, &shorter));
    plink_addr_t junk = a;
    junk.a[10] = 0x55u; /* past len: not part of the address */
    TEST_ASSERT_TRUE(plink_addr_equal(&a, &junk));
    TEST_ASSERT_FALSE(plink_addr_equal(&a, NULL));
    TEST_ASSERT_FALSE(plink_addr_equal(NULL, &a));

    plink_addr_t mac;
    memset(&mac, 0, sizeof mac);
    mac.tp = PLINK_TP_ESPNOW;
    mac.len = 6u;
    static const uint8_t m[6] = {0xaa, 0xbb, 0xcc, 0x0d, 0xe0, 0xff};
    memcpy(mac.a, m, sizeof m);
    plink_addr_text(&mac, text);
    TEST_ASSERT_EQUAL_STRING("aa:bb:cc:0d:e0:ff", text);

    plink_addr_t rf;
    memset(&rf, 0, sizeof rf);
    rf.tp = PLINK_TP_RF;
    rf.len = PLINK_ADDR_MAX;
    fill_range(rf.a, PLINK_ADDR_MAX, 0x01u);
    plink_addr_text(&rf, text);
    TEST_ASSERT_EQUAL_STRING("rf:0102030405060708090a", text);

    plink_addr_t none;
    memset(&none, 0, sizeof none);
    plink_addr_text(&none, text);
    TEST_ASSERT_EQUAL_STRING("-", text);
    plink_addr_text(NULL, text);
    TEST_ASSERT_EQUAL_STRING("-", text);
    plink_addr_text(&shorter, text);
    TEST_ASSERT_EQUAL_STRING("-", text);
    plink_addr_text(&a, NULL); /* nowhere to write: nothing happens */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_golden);
    RUN_TEST(test_type_table);
    RUN_TEST(test_hdr_read_rules);
    RUN_TEST(test_hdr_read_not_v1);
    RUN_TEST(test_new_types_one_way);
    RUN_TEST(test_nonce_layout);
    RUN_TEST(test_sealed_golden);
    RUN_TEST(test_sealed_every_byte_flip);
    RUN_TEST(test_open_other_session_net_seq);
    RUN_TEST(test_sealed_sizes);
    RUN_TEST(test_seal_refusals);
    RUN_TEST(test_open_refusals);
    RUN_TEST(test_plain_frames);
    RUN_TEST(test_window_rfc4303);
    RUN_TEST(test_window_differential);
    RUN_TEST(test_addresses);
    return UNITY_END();
}
