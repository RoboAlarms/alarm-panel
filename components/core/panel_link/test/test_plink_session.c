/*
 * test_plink_session.c - the handshake at every connection and the sessions it makes
 * (components/core/panel_link plink_session.h, docs/features/11 "Sessions"): HELLO, WELCOME, the
 * session keys and id, and the first sealed frame each way byte for byte against vectors made
 * independently with Python's `cryptography` (vectors_plink_session.py beside this file); then
 * handshakes and data both ways, a wrong pairing key, a tampered HELLO and one with an appended
 * field stripped, replayed WELCOMEs, the replay window, the sequence limit, one key per
 * direction, and no AES-CCM key and nonce ever used twice on either side's port.
 *
 * Requirements: LINK-003 (data frames sealed with AES-128-CCM under per-session, per-direction
 * keys; seq from 1; the 64-frame window moved only after the tag; frames of another session or
 * direction refused), LINK-015 (a handshake per connection: ephemeral X25519 keyed by the pairing
 * key, fresh keys and a session id, mac1 checked before any X25519, no seq >= 2^31 sent),
 * LINK-017 (the lower protocol version and the common capabilities; fields a newer side appends
 * before the MAC are skipped but covered by it).
 */
#include <string.h>

#include "panel_link/plink_pair.h"
#include "panel_link/plink_session.h"
#include "plink_test_port.h"
#include "unity.h"

#define NET 0x5A17u
#define DEV 3u

static const plink_caps_t CAP = {4u, 128u, 32u, 16u};

typedef struct {
    plink_test_port_t tpm, tpk, tpx;
    plink_crypto_t cm, ck, cx;
    uint8_t k_pair[PLINK_KEY_LEN];
    plink_hs_k_t hs;         /* the keypad's handshake */
    plink_session_t m;       /* the master's session for the keypad */
    plink_session_t k;       /* the keypad's */
    plink_hello_t hello;     /* the HELLO as the master decoded it */
    uint8_t welcome[128];
    size_t welcome_len;
} rig_t;

static rig_t R;
static uint8_t frame[PLINK_FRAME_MAX], pt[PLINK_FRAME_MAX];

static void rig(uint64_t seed)
{
    memset(&R, 0, sizeof R);
    plink_test_port_init(&R.tpm, seed);
    plink_test_port_init(&R.tpk, seed ^ 0x5555AAAAu);
    plink_test_port_init(&R.tpx, seed ^ 0x0F0F0F0Fu);
    R.cm = plink_test_port(&R.tpm);
    R.ck = plink_test_port(&R.tpk);
    R.cx = plink_test_port(&R.tpx);
    for (size_t i = 0; i < sizeof R.k_pair; i++) {
        R.k_pair[i] = (uint8_t)(0xC0u + i);
    }
}

void setUp(void)
{
    rig(0x5E55u);
}

void tearDown(void)
{
}

/* ---- helpers ------------------------------------------------------------------------------ */

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0u) {
            return false;
        }
    }
    return true;
}

static void fill(uint8_t *p, size_t n, unsigned mul, unsigned add)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(mul * i + add);
    }
}

static plink_hdr_t hdr_of(const uint8_t *f, size_t len)
{
    plink_hdr_t h;
    TEST_ASSERT_TRUE_MESSAGE(plink_hdr_read(f, len, &h), "a frame the receiver would drop");
    return h;
}

/* The keypad's HELLO answered by the master (pending into R.m). */
static bool answer(const uint8_t k_pair[PLINK_KEY_LEN])
{
    const plink_hdr_t h = hdr_of(R.hs.hello, R.hs.hello_len);
    return plink_hs_answer(&R.cm, k_pair, &h, R.hs.hello, R.hs.hello_len, &R.m, &R.hello,
                           R.welcome, sizeof R.welcome, &R.welcome_len);
}

/* The WELCOME in R.welcome taken by the keypad. */
static bool finish(const uint8_t k_pair[PLINK_KEY_LEN])
{
    const plink_hdr_t h = hdr_of(R.welcome, R.welcome_len);
    return plink_hs_finish(&R.ck, k_pair, &R.hs, &h, R.welcome, R.welcome_len, &R.k);
}

/* One whole handshake with R.k_pair. */
static void handshake(void)
{
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    TEST_ASSERT_TRUE(answer(R.k_pair));
    TEST_ASSERT_TRUE(finish(R.k_pair));
    TEST_ASSERT_EQUAL_UINT32(R.m.sid, R.k.sid);
}

/* A payload sealed by the keypad into frame (its length returned; 0 when refused). */
static size_t k_seal(const uint8_t *payload, size_t len)
{
    size_t n = 0;
    if (!plink_session_seal(&R.ck, &R.k, PLINK_T_HEARTBEAT, NET, DEV, false, payload, len, frame,
                            sizeof frame, &n)) {
        return 0u;
    }
    return n;
}

static size_t m_seal(const uint8_t *payload, size_t len)
{
    size_t n = 0;
    if (!plink_session_seal(&R.cm, &R.m, PLINK_T_STATUS, NET, PLINK_SENDER_MASTER, true, payload,
                            len, frame, sizeof frame, &n)) {
        return 0u;
    }
    return n;
}

/* A frame opened by the master (or the keypad) with its session; the payload into pt. */
static bool m_open(const uint8_t *f, size_t len, size_t *pt_len)
{
    const plink_hdr_t h = hdr_of(f, len);
    return plink_session_open(&R.cm, &R.m, &h, f, len, pt, sizeof pt, pt_len);
}

static bool k_open(const uint8_t *f, size_t len, size_t *pt_len)
{
    const plink_hdr_t h = hdr_of(f, len);
    return plink_session_open(&R.ck, &R.k, &h, f, len, pt, sizeof pt, pt_len);
}

/* K_hs, worked out here to build HELLOs by hand. */
static void hello_key(const uint8_t k_pair[PLINK_KEY_LEN], uint8_t k_hs[PLINK_KEY_LEN])
{
    static const char label[] = "plink v1 hello";
    const plink_iov_t info = {label, sizeof label - 1u};
    TEST_ASSERT_TRUE(plink_hkdf_expand(k_pair, &info, 1u, k_hs, PLINK_KEY_LEN));
}

/* A HELLO made by hand from m (with `extra` bytes a newer keypad appended before mac1) into out;
 * returns its length. */
static size_t hand_hello(const plink_hello_t *m, size_t extra, uint8_t *out, size_t cap)
{
    size_t plen = 0, len = 0;
    TEST_ASSERT_TRUE(plink_enc_hello(m, true, &out[PLINK_HDR_LEN], cap - PLINK_HDR_LEN, &plen));
    /* The appended bytes go between the fields and the MAC. */
    const size_t fields = plen - PLINK_MAC_LEN;
    memset(&out[PLINK_HDR_LEN + fields], 0, PLINK_MAC_LEN);
    for (size_t i = 0; i < extra; i++) {
        out[PLINK_HDR_LEN + fields + i] = (uint8_t)(0xE0u + i);
    }
    plen += extra;
    const plink_hdr_t h = {PLINK_T_HELLO, 0u, NET, DEV, 0u, 0u};
    TEST_ASSERT_TRUE(plink_frame_plain(&h, &out[PLINK_HDR_LEN], plen, out, cap, &len));
    uint8_t k_hs[PLINK_KEY_LEN];
    hello_key(R.k_pair, k_hs);
    plink_mac_fill(k_hs, out, len);
    return len;
}

/* ---- scripted random numbers ---------------------------------------------------------------- */

typedef struct {
    plink_crypto_t inner;
    const uint8_t *bytes;
    size_t len;
    size_t pos;
} script_t;

static bool s_aes(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    script_t *s = ctx;
    return s->inner.aes128_encrypt(s->inner.ctx, key, in, out);
}

static bool s_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    script_t *s = ctx;
    return s->inner.x25519(s->inner.ctx, out, scalar, point);
}

static bool s_random(void *ctx, uint8_t *buf, size_t len)
{
    script_t *s = ctx;
    if (s->len - s->pos < len) {
        return false;
    }
    memcpy(buf, &s->bytes[s->pos], len);
    s->pos += len;
    return true;
}

static plink_crypto_t script_port(script_t *s, plink_test_port_t *tp, const uint8_t *bytes,
                                  size_t len)
{
    s->inner = plink_test_port(tp);
    s->bytes = bytes;
    s->len = len;
    s->pos = 0;
    const plink_crypto_t c = {
        .ctx = s, .aes128_encrypt = s_aes, .x25519 = s_x25519, .random = s_random,
        .insecure = true};
    return c;
}

/* ---- golden ------------------------------------------------------------------------------- */

/* vectors_plink_session.py: K_pair c0..df, the keypad's random numbers (0x2D i + 0x61) mod 256 and
 * the master's (0x6B i + 0x05) mod 256, i = 0..47 (ephemeral key, nonce), net id 0x5A17, keypad 3
 * with capacities 4 / 128 / 32 / 16. */
static const uint8_t GOLD_HELLO[90] = {
    0x41, 0x4c, 0x01, 0x10, 0x00, 0x17, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x05, 0x04, 0x80, 0x00, 0x20, 0x10, 0x01, 0x2e, 0x5b, 0x88, 0xb5, 0xe2,
    0x0f, 0x3c, 0x69, 0x96, 0xc3, 0xf0, 0x1d, 0x4a, 0x77, 0xa4, 0x99, 0xe0, 0x2a, 0x17, 0xc2, 0xe7,
    0xb6, 0x5e, 0x01, 0x51, 0x10, 0x77, 0xcf, 0x51, 0xb5, 0xa7, 0x76, 0x70, 0xdb, 0x24, 0x4f, 0x11,
    0x2e, 0x38, 0xad, 0xf6, 0x44, 0x2d, 0xc1, 0x46, 0x3b, 0x38, 0x9f, 0x54, 0x29, 0x73, 0x2e, 0xc6,
    0xb1, 0xae, 0x8e, 0x29, 0x24, 0x83, 0x1f, 0x1f, 0xcf, 0xb7,
};
static const uint8_t GOLD_WELCOME[84] = {
    0x41, 0x4c, 0x01, 0x11, 0x02, 0x17, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x65, 0xd0, 0x3b, 0xa6, 0x11, 0x7c, 0xe7, 0x52, 0xbd, 0x28, 0x93, 0xfe,
    0x69, 0xd4, 0x3f, 0xaa, 0x29, 0x20, 0x38, 0x45, 0x3f, 0x04, 0xb2, 0xa0, 0x3d, 0xbf, 0xd0, 0xcc,
    0x6c, 0x73, 0x21, 0x28, 0x6a, 0x15, 0x0f, 0x53, 0x2d, 0x2c, 0x0b, 0x6b, 0x05, 0x93, 0x3f, 0x72,
    0xfd, 0xdc, 0xf6, 0x06, 0xd6, 0x12, 0x47, 0x53, 0x09, 0xd1, 0x1c, 0x36, 0x52, 0xb5, 0x58, 0xe4,
    0x5a, 0xa8, 0xa4, 0x6d,
};
static const uint8_t GOLD_M2K[16] = {
    0x83, 0x8c, 0x4f, 0x0c, 0xc7, 0x0d, 0x2c, 0x84, 0x4e, 0xe9, 0x38, 0x85, 0x01, 0xe4, 0xf1, 0x85,
};
static const uint8_t GOLD_K2M[16] = {
    0xc7, 0x75, 0xfb, 0xdd, 0xf8, 0x2d, 0x80, 0x83, 0x7f, 0xa0, 0x58, 0xbd, 0xbf, 0x1c, 0xe1, 0x92,
};
#define GOLD_SID 0x73869561u
/* The keypad's first frame: HEARTBEAT, seq 1, payload 90..98. */
static const uint8_t GOLD_K1[34] = {
    0x41, 0x4c, 0x01, 0x2a, 0x01, 0x17, 0x5a, 0x03, 0x00, 0x61, 0x95, 0x86, 0x73, 0x01, 0x00, 0x00,
    0x00, 0x98, 0xf1, 0x33, 0x17, 0xcc, 0x19, 0xc0, 0x6c, 0x0a, 0xe3, 0x0e, 0x94, 0x17, 0xb3, 0x83,
    0x43, 0x02,
};
/* The master's first frame: TIME, seq 1, payload a0..ab. */
static const uint8_t GOLD_M1[37] = {
    0x41, 0x4c, 0x01, 0x2b, 0x03, 0x17, 0x5a, 0x00, 0x00, 0x61, 0x95, 0x86, 0x73, 0x01, 0x00, 0x00,
    0x00, 0x92, 0x5b, 0x89, 0x37, 0x74, 0x63, 0xc2, 0x27, 0xa2, 0x51, 0xd8, 0x67, 0x40, 0xe5, 0xaf,
    0x24, 0x45, 0xbe, 0x59, 0x6d,
};

/* LINK-015, LINK-003: with the random numbers Python used, the HELLO (mac1) and the WELCOME (sid
 * 0, mac2) are its bytes, both sides derive its K_m2k, K_k2m and session id - mirrored, one key
 * per direction - and the first sealed frame each way is its AES-CCM output; each side opens the
 * other's. Exactly the scripted random bytes are drawn. */
static void test_golden_handshake(void)
{
    static uint8_t kb[48], mb[48];
    static script_t ks, ms;
    fill(kb, sizeof kb, 0x2Du, 0x61u);
    fill(mb, sizeof mb, 0x6Bu, 0x05u);
    R.ck = script_port(&ks, &R.tpk, kb, sizeof kb);
    R.cm = script_port(&ms, &R.tpm, mb, sizeof mb);

    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    TEST_ASSERT_EQUAL_size_t(sizeof GOLD_HELLO, R.hs.hello_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_HELLO, R.hs.hello, sizeof GOLD_HELLO);
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_TRUE(plink_hs_hello_valid(R.k_pair, R.hs.hello, R.hs.hello_len));

    TEST_ASSERT_TRUE(answer(R.k_pair));
    TEST_ASSERT_EQUAL_size_t(sizeof GOLD_WELCOME, R.welcome_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_WELCOME, R.welcome, sizeof GOLD_WELCOME);
    TEST_ASSERT_TRUE(R.m.valid);
    TEST_ASSERT_EQUAL_HEX32(GOLD_SID, R.m.sid);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_M2K, R.m.k_tx, 16u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_K2M, R.m.k_rx, 16u);
    TEST_ASSERT_EQUAL_UINT32(0u, R.m.tx_seq);
    TEST_ASSERT_EQUAL_UINT8(1u, R.m.proto);
    TEST_ASSERT_EQUAL_HEX16(0u, R.m.caps);
    TEST_ASSERT_EQUAL_UINT16(128u, R.m.peer_cap.zones);
    TEST_ASSERT_EQUAL_UINT8(4u, R.m.peer_cap.partitions);
    TEST_ASSERT_EQUAL_UINT8(32u, R.hello.cap.users);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kb[32], R.hello.n, 16u); /* the HELLO's n_k, for the seen list */

    TEST_ASSERT_TRUE(finish(R.k_pair));
    TEST_ASSERT_TRUE(R.k.valid);
    TEST_ASSERT_EQUAL_HEX32(GOLD_SID, R.k.sid);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_K2M, R.k.k_tx, 16u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_M2K, R.k.k_rx, 16u);
    TEST_ASSERT_EQUAL_UINT8(1u, R.k.proto);

    uint8_t hb[9], tm[12];
    fill(hb, sizeof hb, 1u, 0x90u);
    fill(tm, sizeof tm, 1u, 0xA0u);
    size_t n = k_seal(hb, sizeof hb), got = 0;
    TEST_ASSERT_EQUAL_size_t(sizeof GOLD_K1, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_K1, frame, n);
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    TEST_ASSERT_EQUAL_size_t(sizeof hb, got);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hb, pt, got);
    n = 0;
    TEST_ASSERT_TRUE(plink_session_seal(&R.cm, &R.m, PLINK_T_TIME, NET, PLINK_SENDER_MASTER, true,
                                        tm, sizeof tm, frame, sizeof frame, &n));
    TEST_ASSERT_EQUAL_size_t(sizeof GOLD_M1, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_M1, frame, n);
    TEST_ASSERT_TRUE(k_open(frame, n, &got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tm, pt, sizeof tm);
    TEST_ASSERT_EQUAL_size_t(sizeof kb, ks.pos);
    TEST_ASSERT_EQUAL_size_t(sizeof mb, ms.pos);
}

/* ---- handshakes and data ------------------------------------------------------------------ */

/* LINK-003, LINK-015: a handshake on the seeded ports, then data both ways - payloads of 0 to 600
 * bytes, every one opened intact, seq counted from 1 on each side, the receive window following;
 * a second handshake gives a new session id and new keys. */
static void test_handshake_and_data_both_ways(void)
{
    handshake();
    TEST_ASSERT_EQUAL_HEX8_ARRAY(R.m.k_tx, R.k.k_rx, 16u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(R.m.k_rx, R.k.k_tx, 16u);
    TEST_ASSERT_NOT_EQUAL(0u, R.k.sid);
    uint8_t payload[600];
    for (uint32_t i = 1; i <= 40u; i++) {
        const size_t len = (size_t)(i * 15u) % sizeof payload;
        fill(payload, len, i, 7u);
        size_t n = k_seal(payload, len), got = 99;
        TEST_ASSERT_EQUAL_size_t(PLINK_SEALED_OVERHEAD + len, n);
        TEST_ASSERT_EQUAL_UINT32(i, hdr_of(frame, n).seq);
        TEST_ASSERT_EQUAL_HEX32(R.k.sid, hdr_of(frame, n).sid);
        TEST_ASSERT_TRUE(m_open(frame, n, &got));
        TEST_ASSERT_EQUAL_size_t(len, got);
        if (len > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, pt, len);
        }
        n = m_seal(payload, len);
        TEST_ASSERT_EQUAL_UINT32(i, hdr_of(frame, n).seq);
        TEST_ASSERT_TRUE(k_open(frame, n, &got));
        TEST_ASSERT_EQUAL_size_t(len, got);
    }
    TEST_ASSERT_EQUAL_UINT32(40u, R.k.tx_seq);
    TEST_ASSERT_EQUAL_UINT32(40u, R.m.rx.top);
    TEST_ASSERT_EQUAL_UINT32(40u, R.k.rx.top);

    const uint32_t sid1 = R.k.sid;
    uint8_t k1[16];
    memcpy(k1, R.k.k_tx, sizeof k1);
    handshake();
    TEST_ASSERT_NOT_EQUAL(sid1, R.k.sid);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(k1, R.k.k_tx, sizeof k1));
    TEST_ASSERT_EQUAL_UINT32(0u, R.k.tx_seq);
    const size_t n = k_seal(payload, 10u);
    size_t got = 0;
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
}

/* LINK-015: a HELLO made with another pairing key fails mac1 at the master before any X25519 (the
 * port isn't called; nothing made, the WELCOME buffer untouched); a WELCOME made with the right key
 * fails at a keypad holding another (out untouched, still waiting). */
static void test_wrong_k_pair(void)
{
    uint8_t other[PLINK_KEY_LEN];
    memcpy(other, R.k_pair, sizeof other);
    other[0] ^= 0x01u;
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    TEST_ASSERT_FALSE(plink_hs_hello_valid(other, R.hs.hello, R.hs.hello_len));
    memset(&R.m, 0xA5, sizeof R.m);
    const uint32_t calls = R.tpm.hc.calls;
    TEST_ASSERT_FALSE(answer(other));
    TEST_ASSERT_EQUAL_UINT32(calls, R.tpm.hc.calls);
    TEST_ASSERT_EQUAL_size_t(0u, R.welcome_len);
    uint8_t a5[sizeof R.m];
    memset(a5, 0xA5, sizeof a5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a5, &R.m, sizeof R.m);

    TEST_ASSERT_TRUE(answer(R.k_pair));
    memset(&R.k, 0x5A, sizeof R.k);
    TEST_ASSERT_FALSE(finish(other));
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_EQUAL_HEX8(0x5Au, ((const uint8_t *)&R.k)[0]);
    TEST_ASSERT_TRUE(finish(R.k_pair));
    TEST_ASSERT_EQUAL_UINT32(R.m.sid, R.k.sid);
}

/* LINK-015, LINK-017: every single-bit change of a HELLO fails mac1. A newer keypad's HELLO, with
 * bytes appended before mac1, is answered (the fields this firmware knows are read, the rest
 * skipped) and the session works both ways; the same HELLO with the appended bytes stripped by a
 * device in the middle, mac1 kept, fails. */
static void test_hello_tampered_or_stripped(void)
{
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    uint8_t f[128];
    memcpy(f, R.hs.hello, R.hs.hello_len);
    for (size_t i = 0; i < R.hs.hello_len; i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            f[i] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_FALSE(plink_hs_hello_valid(R.k_pair, f, R.hs.hello_len));
            f[i] ^= (uint8_t)(1u << bit);
        }
    }
    TEST_ASSERT_TRUE(plink_hs_hello_valid(R.k_pair, f, R.hs.hello_len));
    TEST_ASSERT_FALSE(plink_hs_hello_valid(R.k_pair, f, R.hs.hello_len - 1u));
    TEST_ASSERT_FALSE(plink_hs_hello_valid(R.k_pair, f, PLINK_HDR_LEN + PLINK_MAC_LEN - 1u));

    /* A newer keypad: the same fields, 3 more bytes before mac1. Its handshake state is set by
     * hand so its transcript is that HELLO. */
    plink_hello_t m;
    memset(&m, 0, sizeof m);
    m.proto = PLINK_PROTO;
    m.caps = PLINK_CAPS;
    m.cap = CAP;
    plink_hs_k_wipe(&R.hs);
    TEST_ASSERT_TRUE(plink_x25519_keypair(&R.ck, R.hs.e, m.e));
    TEST_ASSERT_TRUE(plink_random(&R.ck, R.hs.n_k, 16u));
    memcpy(m.n, R.hs.n_k, 16u);
    R.hs.hello_len = hand_hello(&m, 3u, R.hs.hello, sizeof R.hs.hello);
    R.hs.waiting = true;
    TEST_ASSERT_TRUE(answer(R.k_pair));
    TEST_ASSERT_EQUAL_UINT16(128u, R.hello.cap.zones);
    TEST_ASSERT_TRUE(finish(R.k_pair));
    uint8_t p[5] = {1, 2, 3, 4, 5};
    size_t n = k_seal(p, sizeof p), got = 0;
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    n = m_seal(p, sizeof p);
    TEST_ASSERT_TRUE(k_open(frame, n, &got));

    /* Stripped: the 3 bytes cut out, mac1 moved up. */
    uint8_t s[128];
    const size_t len = R.hs.hello_len;
    memcpy(s, R.hs.hello, len - PLINK_MAC_LEN - 3u);
    memcpy(&s[len - PLINK_MAC_LEN - 3u], &R.hs.hello[len - PLINK_MAC_LEN], PLINK_MAC_LEN);
    TEST_ASSERT_FALSE(plink_hs_hello_valid(R.k_pair, s, len - 3u));
    const plink_hdr_t h = hdr_of(s, len - 3u);
    plink_session_t pending;
    TEST_ASSERT_FALSE(plink_hs_answer(&R.cm, R.k_pair, &h, s, len - 3u, &pending, &R.hello,
                                      R.welcome, sizeof R.welcome, &R.welcome_len));
}

/* LINK-015: a WELCOME is taken once, and only for the latest HELLO: the same WELCOME again is
 * refused, a WELCOME made for an earlier HELLO is refused while a new one waits (its transcript
 * differs), as is one carrying another network id or a tampered byte; the new HELLO's own WELCOME
 * then gives a new session. A WELCOME whose e_m is of small order is refused. */
static void test_replayed_welcome(void)
{
    handshake();
    uint8_t old[128];
    const size_t old_len = R.welcome_len;
    memcpy(old, R.welcome, old_len);
    const plink_session_t first = R.k;
    TEST_ASSERT_FALSE(finish(R.k_pair)); /* the same again: no HELLO waits */
    TEST_ASSERT_EQUAL_HEX32(first.sid, R.k.sid);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first.k_tx, R.k.k_tx, 16u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first.k_rx, R.k.k_rx, 16u);

    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    plink_hdr_t h = hdr_of(old, old_len);
    TEST_ASSERT_FALSE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, old, old_len, &R.k));
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_TRUE(answer(R.k_pair));
    uint8_t w[128];
    memcpy(w, R.welcome, R.welcome_len);
    w[5] ^= 0x01u; /* another net id */
    h = hdr_of(w, R.welcome_len);
    TEST_ASSERT_FALSE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, w, R.welcome_len, &R.k));
    for (size_t i = PLINK_HDR_LEN; i < R.welcome_len; i++) {
        memcpy(w, R.welcome, R.welcome_len);
        w[i] ^= 0x10u;
        h = hdr_of(w, R.welcome_len);
        TEST_ASSERT_FALSE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, w, R.welcome_len, &R.k));
    }
    memcpy(w, R.welcome, R.welcome_len);
    memset(&w[PLINK_HDR_LEN + 3u + 16u], 0, 32u); /* e_m = 0, a point of small order */
    h = hdr_of(w, R.welcome_len);
    TEST_ASSERT_FALSE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, w, R.welcome_len, &R.k));
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_TRUE(finish(R.k_pair));
    TEST_ASSERT_NOT_EQUAL(first.sid, R.k.sid);
    TEST_ASSERT_EQUAL_UINT32(R.m.sid, R.k.sid);
}

/* A WELCOME made by hand for the HELLO waiting in R.hs: proto, e_m and the DH the master's side
 * would hold, its mac2 under the K_fin derived as docs/features/11 "Sessions" says (TH_s over the
 * HELLO and the WELCOME up to mac2; PRK_s from n_k || n_m and K_pair || DH; K_fin the third
 * part of the 68 bytes). Returns its length. */
/* mac2 of the WELCOME w (len bytes, its MAC the last 16) for the HELLO waiting in R.hs, with the
 * master's n_m and the DH its side would hold. */
static void mac2_fill(uint8_t *w, size_t len, const uint8_t n_m[16], const uint8_t dh[32])
{
    static const char label[] = "plink v1 session";
    uint8_t th[32], prk[32], okm[68], salt[32];
    const plink_iov_t tr[] = {{R.hs.hello, R.hs.hello_len}, {w, len - PLINK_MAC_LEN}};
    plink_sha256(tr, 2u, th);
    memcpy(salt, R.hs.n_k, 16u);
    memcpy(&salt[16], n_m, 16u);
    const plink_iov_t ikm[] = {{R.k_pair, PLINK_KEY_LEN}, {dh, 32u}};
    plink_hkdf_extract(salt, sizeof salt, ikm, 2u, prk);
    const plink_iov_t info[] = {{label, sizeof label - 1u}, {th, sizeof th}};
    TEST_ASSERT_TRUE(plink_hkdf_expand(prk, info, 2u, okm, sizeof okm));
    plink_mac_fill(&okm[32], w, len);
}

static size_t hand_welcome(uint8_t proto, const uint8_t e_m[32], const uint8_t dh[32],
                           uint8_t *out, size_t cap)
{
    plink_hello_t w;
    memset(&w, 0, sizeof w);
    w.proto = proto;
    fill(w.n, sizeof w.n, 5u, 0x31u);
    memcpy(w.e, e_m, 32u);
    size_t plen = 0, len = 0;
    TEST_ASSERT_TRUE(plink_enc_hello(&w, false, &out[PLINK_HDR_LEN], cap - PLINK_HDR_LEN, &plen));
    const plink_hdr_t h = {PLINK_T_WELCOME, PLINK_FLAG_FROM_MASTER, NET, PLINK_SENDER_MASTER, 0u,
                           0u};
    TEST_ASSERT_TRUE(plink_frame_plain(&h, &out[PLINK_HDR_LEN], plen, out, cap, &len));
    mac2_fill(out, len, w.n, dh);
    return len;
}

/* LINK-015, LINK-017: the keypad's own refusals, each proved with a WELCOME whose mac2 is valid -
 * so it is the refusal that stops it, not the MAC: an e_m of small order (0, whose DH is 0) and a
 * protocol version of 0 (with an honest e_m and DH) are refused, the HELLO still waiting; the
 * honest WELCOME made the same way is taken. */
static void test_finish_refusals_with_a_valid_mac2(void)
{
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    /* HELLO: proto, caps, capacities section (6), n_k (16), then e_k */
    const uint8_t *e_k = &R.hs.hello[PLINK_HDR_LEN + 25u];
    uint8_t sk_m[32], e_m[32], dh[32], zero[32], w[128];
    memset(zero, 0, sizeof zero);
    TEST_ASSERT_TRUE(plink_x25519_keypair(&R.cx, sk_m, e_m));
    TEST_ASSERT_TRUE(plink_x25519_shared(&R.cx, sk_m, e_k, dh));
    plink_session_t s;
    memset(&s, 0, sizeof s);

    size_t len = hand_welcome(PLINK_PROTO, zero, zero, w, sizeof w);
    plink_hdr_t h = hdr_of(w, len);
    TEST_ASSERT_FALSE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, w, len, &s));
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_FALSE(s.valid);

    len = hand_welcome(0u, e_m, dh, w, sizeof w);
    h = hdr_of(w, len);
    TEST_ASSERT_FALSE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, w, len, &s));
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_FALSE(s.valid);

    len = hand_welcome(PLINK_PROTO, e_m, dh, w, sizeof w);
    h = hdr_of(w, len);
    TEST_ASSERT_TRUE(plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, w, len, &s));
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_FALSE(R.hs.waiting);
    plink_session_wipe(&s);
}

/* ---- the replay window -------------------------------------------------------------------- */

/* LINK-003: frames reordered within the 64-frame window are all taken; each replay is refused
 * before it costs an AES call; a frame 64 or more below the newest is refused; a frame of another
 * session, one with seq >= 2^31 and one whose tag fails are refused and don't move the window. */
static void test_replay_window(void)
{
    handshake();
    static uint8_t frames[200][64];
    static size_t lens[200];
    uint8_t p[8];
    for (size_t i = 0; i < 200u; i++) {
        fill(p, sizeof p, (unsigned)i, 1u);
        lens[i] = k_seal(p, sizeof p);
        TEST_ASSERT_EQUAL_size_t(PLINK_SEALED_OVERHEAD + sizeof p, lens[i]);
        memcpy(frames[i], frame, lens[i]);
    }
    size_t got = 0;
    /* seq 1..96 in blocks of 32, each block newest first. */
    for (size_t b = 0; b < 3u; b++) {
        for (size_t j = 32u; j-- > 0u;) {
            const size_t i = b * 32u + j;
            TEST_ASSERT_TRUE(m_open(frames[i], lens[i], &got));
            fill(p, sizeof p, (unsigned)i, 1u);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(p, pt, sizeof p);
        }
    }
    const uint32_t aes = R.tpm.aes_calls;
    for (size_t i = 0; i < 96u; i++) {
        TEST_ASSERT_FALSE(m_open(frames[i], lens[i], &got));
    }
    TEST_ASSERT_EQUAL_UINT32(aes, R.tpm.aes_calls); /* the window refused them, not the tag */

    TEST_ASSERT_TRUE(m_open(frames[199], lens[199], &got)); /* seq 200 */
    TEST_ASSERT_FALSE(m_open(frames[135], lens[135], &got)); /* seq 136: 64 below */
    TEST_ASSERT_TRUE(m_open(frames[136], lens[136], &got));  /* seq 137: 63 below */
    TEST_ASSERT_FALSE(m_open(frames[136], lens[136], &got));
    TEST_ASSERT_TRUE(m_open(frames[198], lens[198], &got));

    /* A frame whose tag fails leaves the window as it was: the real one still opens. */
    uint8_t bad[64];
    memcpy(bad, frames[150], lens[150]);
    bad[lens[150] - 1u] ^= 0x01u;
    const plink_window_t before = R.m.rx;
    TEST_ASSERT_FALSE(m_open(bad, lens[150], &got));
    TEST_ASSERT_EQUAL_UINT32(before.top, R.m.rx.top);
    TEST_ASSERT_TRUE(before.bits == R.m.rx.bits);
    TEST_ASSERT_TRUE(m_open(frames[150], lens[150], &got));

    /* Another session's frame; a seq of 2^31. */
    plink_hdr_t h = hdr_of(frames[160], lens[160]);
    h.sid ^= 1u;
    TEST_ASSERT_FALSE(plink_session_open(&R.cm, &R.m, &h, frames[160], lens[160], pt, sizeof pt,
                                         &got));
    h = hdr_of(frames[160], lens[160]);
    h.seq = PLINK_SEQ_LIMIT;
    TEST_ASSERT_FALSE(plink_session_open(&R.cm, &R.m, &h, frames[160], lens[160], pt, sizeof pt,
                                         &got));
    TEST_ASSERT_TRUE(m_open(frames[160], lens[160], &got));
    /* No room for the payload: refused, not counted. */
    h = hdr_of(frames[161], lens[161]);
    TEST_ASSERT_FALSE(plink_session_open(&R.cm, &R.m, &h, frames[161], lens[161], pt, 4u, &got));
    TEST_ASSERT_TRUE(m_open(frames[161], lens[161], &got));
    plink_session_t off = R.m;
    off.valid = false;
    h = hdr_of(frames[162], lens[162]);
    TEST_ASSERT_FALSE(plink_session_open(&R.cm, &off, &h, frames[162], lens[162], pt, sizeof pt,
                                         &got));
}

/* LINK-015: no side seals with seq >= 2^31: the frame at 2^31 - 1 goes and opens; the next seal
 * is refused with nothing written and tx_seq unchanged. A session not made (valid false) seals
 * nothing. */
static void test_seq_limit(void)
{
    handshake();
    R.k.tx_seq = PLINK_SEQ_LIMIT - 2u;
    const uint8_t p[3] = {7, 8, 9};
    size_t n = k_seal(p, sizeof p), got = 0;
    TEST_ASSERT_NOT_EQUAL(0u, n);
    TEST_ASSERT_EQUAL_HEX32(PLINK_SEQ_LIMIT - 1u, hdr_of(frame, n).seq);
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    size_t out_len = 77u;
    TEST_ASSERT_FALSE(plink_session_seal(&R.ck, &R.k, PLINK_T_HEARTBEAT, NET, DEV, false, p,
                                         sizeof p, frame, sizeof frame, &out_len));
    TEST_ASSERT_EQUAL_size_t(0u, out_len);
    TEST_ASSERT_EQUAL_HEX32(PLINK_SEQ_LIMIT - 1u, R.k.tx_seq);
    plink_session_t none;
    memset(&none, 0, sizeof none);
    TEST_ASSERT_FALSE(plink_session_seal(&R.ck, &none, PLINK_T_HEARTBEAT, NET, DEV, false, p,
                                         sizeof p, frame, sizeof frame, &out_len));
    TEST_ASSERT_EQUAL_UINT32(0u, none.tx_seq);
}

/* LINK-015, LINK-003: a frame with seq >= 2^31 is refused on arrival though it is genuine - sealed
 * under the session's own key, sid, net and sender by a peer that ignored the limit - before any
 * AES call, the window unmoved; the same bytes sealed at 2^31 - 1 open. A frame sealed under the
 * right key with another sid is refused the same way. */
static void test_seq_limit_on_arrival(void)
{
    handshake();
    const uint8_t p[5] = {1, 2, 3, 4, 5};
    plink_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = PLINK_T_HEARTBEAT;
    h.flags = PLINK_FLAG_SEALED;
    h.net_id = NET;
    h.sender = DEV;
    h.sid = R.k.sid;
    size_t n = 0, got = 0;
    const uint32_t refused[] = {PLINK_SEQ_LIMIT, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        h.seq = refused[i];
        TEST_ASSERT_TRUE(plink_frame_seal(&R.ck, R.k.k_tx, &h, p, sizeof p, frame, sizeof frame,
                                          &n));
        const uint32_t aes = R.tpm.aes_calls;
        const plink_window_t before = R.m.rx;
        TEST_ASSERT_FALSE(m_open(frame, n, &got));
        TEST_ASSERT_EQUAL_UINT32(aes, R.tpm.aes_calls);
        TEST_ASSERT_EQUAL_UINT32(before.top, R.m.rx.top);
        TEST_ASSERT_TRUE(before.bits == R.m.rx.bits);
    }
    h.seq = PLINK_SEQ_LIMIT - 1u;
    TEST_ASSERT_TRUE(plink_frame_seal(&R.ck, R.k.k_tx, &h, p, sizeof p, frame, sizeof frame, &n));
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p, pt, sizeof p);

    h.sid = R.k.sid ^ 0x00010000u;
    h.seq = 7u;
    TEST_ASSERT_TRUE(plink_frame_seal(&R.ck, R.k.k_tx, &h, p, sizeof p, frame, sizeof frame, &n));
    const uint32_t aes = R.tpm.aes_calls;
    const plink_window_t before = R.m.rx;
    TEST_ASSERT_FALSE(m_open(frame, n, &got));
    TEST_ASSERT_EQUAL_UINT32(aes, R.tpm.aes_calls);
    TEST_ASSERT_TRUE(before.bits == R.m.rx.bits && before.top == R.m.rx.top);
}

/* LINK-015, docs/features/11 "Sessions": a seal that fails inside the port still spends its seq,
 * so no nonce is tried twice under the key: the next frame goes with the seq after it (the port's
 * nonce watch sees no repeat), and the master opens it. */
static void test_seal_failure_spends_its_seq(void)
{
    handshake();
    const uint8_t p[6] = {9, 8, 7, 6, 5, 4};
    size_t n = k_seal(p, sizeof p), got = 0;
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    const uint32_t seq0 = R.k.tx_seq;
    plink_test_port_fail_at(&R.tpk, 1u); /* an AES call inside the seal, after its A_0 */
    TEST_ASSERT_EQUAL_size_t(0u, k_seal(p, sizeof p));
    TEST_ASSERT_EQUAL_UINT32(seq0 + 1u, R.k.tx_seq);
    TEST_ASSERT_TRUE(all_zero(frame, PLINK_SEALED_OVERHEAD + sizeof p)); /* nothing left there */
    n = k_seal(p, sizeof p);
    TEST_ASSERT_NOT_EQUAL(0u, n);
    TEST_ASSERT_EQUAL_HEX32(seq0 + 2u, hdr_of(frame, n).seq);
    TEST_ASSERT_FALSE(R.tpk.reused);
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p, pt, sizeof p);
}

/* LINK-003: each direction has its own key: the keypad's frames can't be opened with the keys it
 * receives with (a frame sent back to its sender is refused), nor the master's; a frame of one
 * session is refused by the next. */
static void test_directions_use_different_keys(void)
{
    handshake();
    TEST_ASSERT_NOT_EQUAL(0, memcmp(R.k.k_tx, R.k.k_rx, 16u));
    const uint8_t p[6] = {1, 1, 2, 3, 5, 8};
    size_t n = k_seal(p, sizeof p), got = 0;
    TEST_ASSERT_FALSE(k_open(frame, n, &got)); /* reflected to the keypad */
    TEST_ASSERT_TRUE(m_open(frame, n, &got));
    n = m_seal(p, sizeof p);
    TEST_ASSERT_FALSE(m_open(frame, n, &got)); /* reflected to the master */
    TEST_ASSERT_TRUE(k_open(frame, n, &got));
    uint8_t old[64];
    n = k_seal(p, sizeof p);
    memcpy(old, frame, n);
    handshake();
    const plink_hdr_t h = hdr_of(old, n);
    TEST_ASSERT_FALSE(plink_session_open(&R.cm, &R.m, &h, old, n, pt, sizeof pt, &got));
}

/* LINK-003: over 1000 frames each way and five handshakes, neither side's port ever sees the same
 * AES-CCM key and nonce twice (plink_test_port_t.reused); the watch is real - resealing a seq
 * already used trips it. */
static void test_no_key_nonce_reused(void)
{
    uint8_t p[40];
    for (unsigned hs = 0; hs < 5u; hs++) {
        handshake();
        for (unsigned i = 0; i < 200u; i++) {
            fill(p, sizeof p, i + hs, 3u);
            size_t n = k_seal(p, (size_t)(i % 41u)), got = 0;
            TEST_ASSERT_TRUE(m_open(frame, n, &got));
            n = m_seal(p, (size_t)(i % 37u));
            TEST_ASSERT_TRUE(k_open(frame, n, &got));
        }
    }
    TEST_ASSERT_FALSE(R.tpk.reused);
    TEST_ASSERT_FALSE(R.tpm.reused);
    TEST_ASSERT_TRUE(R.tpk.seen_count >= 2000u); /* the watch saw 1000 sealed, 1000 opened */
    TEST_ASSERT_TRUE(R.tpm.seen_count >= 2000u);
    R.k.tx_seq = 10u; /* a counter set back - what must never happen */
    TEST_ASSERT_NOT_EQUAL(0u, k_seal(p, 4u));
    TEST_ASSERT_TRUE(R.tpk.reused);
}

/* LINK-017: the session takes the lower protocol version and the common capabilities: a HELLO
 * from a newer keypad (proto 3, capabilities 0xFFFF) makes a v1 session with v1's capabilities; a
 * HELLO with proto 0 (no version in common) is refused. */
static void test_proto_and_caps(void)
{
    plink_hello_t m;
    memset(&m, 0, sizeof m);
    m.proto = 3u;
    m.caps = 0xFFFFu;
    m.cap = CAP;
    TEST_ASSERT_TRUE(plink_x25519_keypair(&R.ck, R.hs.e, m.e));
    TEST_ASSERT_TRUE(plink_random(&R.ck, R.hs.n_k, 16u));
    memcpy(m.n, R.hs.n_k, 16u);
    R.hs.hello_len = hand_hello(&m, 0u, R.hs.hello, sizeof R.hs.hello);
    R.hs.waiting = true;
    TEST_ASSERT_TRUE(answer(R.k_pair));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PROTO, R.m.proto);
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAPS, R.m.caps);
    TEST_ASSERT_EQUAL_UINT8(3u, R.hello.proto);
    TEST_ASSERT_TRUE(finish(R.k_pair));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PROTO, R.k.proto);

    m.proto = 0u;
    R.hs.hello_len = hand_hello(&m, 0u, R.hs.hello, sizeof R.hs.hello);
    TEST_ASSERT_TRUE(plink_hs_hello_valid(R.k_pair, R.hs.hello, R.hs.hello_len));
    TEST_ASSERT_FALSE(answer(R.k_pair));
}

/* LINK-015: handshake hygiene: each HELLO has a fresh e_k and n_k (a second HELLO replaces the
 * first, which can then no longer finish); finishing wipes e_k and n_k; a HELLO from a number
 * outside 1..8 or for net id 0 isn't made (and leaves nothing); a HELLO whose e_k is of small
 * order, even with a good mac1, is refused with the WELCOME buffer wiped; the wipes zero
 * everything. */
static void test_handshake_hygiene(void)
{
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    plink_hs_k_t first = R.hs;
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(first.e, R.hs.e, 32u));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(first.n_k, R.hs.n_k, 16u));
    /* The master answers the first HELLO: the keypad, now waiting on the second, refuses it. */
    const plink_hdr_t h1 = hdr_of(first.hello, first.hello_len);
    TEST_ASSERT_TRUE(plink_hs_answer(&R.cm, R.k_pair, &h1, first.hello, first.hello_len, &R.m,
                                     &R.hello, R.welcome, sizeof R.welcome, &R.welcome_len));
    TEST_ASSERT_FALSE(finish(R.k_pair));
    TEST_ASSERT_TRUE(answer(R.k_pair));
    TEST_ASSERT_TRUE(finish(R.k_pair));
    TEST_ASSERT_TRUE(all_zero(R.hs.e, sizeof R.hs.e));
    TEST_ASSERT_TRUE(all_zero(R.hs.n_k, sizeof R.hs.n_k));
    TEST_ASSERT_FALSE(R.hs.waiting);
    plink_hs_k_wipe(&first);

    TEST_ASSERT_FALSE(plink_hs_hello(&R.ck, R.k_pair, NET, 0u, &CAP, &R.hs));
    TEST_ASSERT_TRUE(all_zero(&R.hs, sizeof R.hs));
    TEST_ASSERT_FALSE(plink_hs_hello(&R.ck, R.k_pair, NET, 9u, &CAP, &R.hs));
    TEST_ASSERT_FALSE(plink_hs_hello(&R.ck, R.k_pair, 0u, DEV, &CAP, &R.hs));
    TEST_ASSERT_FALSE(R.hs.waiting);

    plink_hello_t m;
    memset(&m, 0, sizeof m);
    m.proto = PLINK_PROTO;
    m.cap = CAP; /* e = 0: a point of small order */
    const size_t len = hand_hello(&m, 0u, R.hs.hello, sizeof R.hs.hello);
    const plink_hdr_t h = hdr_of(R.hs.hello, len);
    plink_session_t pending;
    memset(&pending, 0, sizeof pending);
    memset(R.welcome, 0xCC, sizeof R.welcome);
    TEST_ASSERT_FALSE(plink_hs_answer(&R.cm, R.k_pair, &h, R.hs.hello, len, &pending, &R.hello,
                                      R.welcome, sizeof R.welcome, &R.welcome_len));
    TEST_ASSERT_FALSE(pending.valid);
    TEST_ASSERT_TRUE(all_zero(R.welcome, sizeof R.welcome));
    TEST_ASSERT_EQUAL_size_t(0u, R.welcome_len);
    /* No room for a WELCOME, or a header that isn't a HELLO's: refused. */
    m.e[0] = 9u; /* the base point: a good key */
    const size_t len2 = hand_hello(&m, 0u, R.hs.hello, sizeof R.hs.hello);
    plink_hdr_t h2 = hdr_of(R.hs.hello, len2);
    TEST_ASSERT_FALSE(plink_hs_answer(&R.cm, R.k_pair, &h2, R.hs.hello, len2, &pending, &R.hello,
                                      R.welcome, PLINK_HDR_LEN - 1u, &R.welcome_len));
    TEST_ASSERT_FALSE(plink_hs_answer(&R.cm, R.k_pair, &h2, R.hs.hello, len2, &pending, &R.hello,
                                      R.welcome, 40u, &R.welcome_len));
    h2.type = PLINK_T_WELCOME;
    TEST_ASSERT_FALSE(plink_hs_answer(&R.cm, R.k_pair, &h2, R.hs.hello, len2, &pending, &R.hello,
                                      R.welcome, sizeof R.welcome, &R.welcome_len));
    TEST_ASSERT_FALSE(pending.valid);
    h2.type = PLINK_T_HELLO;
    TEST_ASSERT_TRUE(plink_hs_answer(&R.cm, R.k_pair, &h2, R.hs.hello, len2, &pending, &R.hello,
                                     R.welcome, sizeof R.welcome, &R.welcome_len));
    TEST_ASSERT_TRUE(pending.valid);

    handshake();
    plink_session_wipe(&R.k);
    TEST_ASSERT_TRUE(all_zero(&R.k, sizeof R.k));
    plink_hs_k_wipe(&R.hs);
    TEST_ASSERT_TRUE(all_zero(&R.hs, sizeof R.hs));
}

/* LINK-015: a failing port leaves nothing half made: the HELLO, the WELCOME and the session are
 * not made, the keypad's handshake is wiped, a WELCOME it can't finish leaves it waiting. */
static void test_port_failures(void)
{
    plink_test_port_fail_at(&R.tpk, 0u);
    TEST_ASSERT_FALSE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    TEST_ASSERT_TRUE(all_zero(&R.hs, sizeof R.hs));
    TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
    for (uint32_t call = 0; call < 4u; call++) { /* the key pair (2 calls), n_m, the agreement */
        plink_test_port_fail_at(&R.tpm, call);
        memset(&R.m, 0, sizeof R.m);
        TEST_ASSERT_FALSE(answer(R.k_pair));
        TEST_ASSERT_FALSE(R.m.valid);
        TEST_ASSERT_EQUAL_size_t(0u, R.welcome_len);
    }
    TEST_ASSERT_TRUE(answer(R.k_pair));
    plink_test_port_fail_at(&R.tpk, 0u);
    TEST_ASSERT_FALSE(finish(R.k_pair));
    TEST_ASSERT_TRUE(R.hs.waiting);
    TEST_ASSERT_TRUE(finish(R.k_pair));
}

/* ---- random bytes into the handshake ------------------------------------------------------- */

static uint64_t RNG;

static uint32_t rnd(void)
{
    RNG ^= RNG << 13;
    RNG ^= RNG >> 7;
    RNG ^= RNG << 17;
    return (uint32_t)(RNG >> 32);
}

static size_t rnd_below(size_t n)
{
    return n == 0u ? 0u : (size_t)rnd() % n;
}

/* Random edits of a plain frame's fields - the bytes between its header and its MAC: a bit
 * flipped, an edge value, a cut, bytes inserted, deleted or appended, now and then all of them
 * random. The MAC's place (the last 16 bytes) is kept, to be filled in again. */
static void mutate_fields(uint8_t *f, size_t *len, size_t cap)
{
    static const uint8_t edge[] = {0x00, 0x01, 0x02, 0x05, 0x06, 0x10, 0x20, 0x7F, 0x80, 0xFF};
    uint8_t fields[160];
    size_t n = *len - PLINK_HDR_LEN - PLINK_MAC_LEN;
    const size_t room = cap - PLINK_HDR_LEN - PLINK_MAC_LEN < sizeof fields
                            ? cap - PLINK_HDR_LEN - PLINK_MAC_LEN
                            : sizeof fields;
    memcpy(fields, &f[PLINK_HDR_LEN], n);
    if (rnd_below(8u) == 0u) {
        n = rnd_below(100u);
        for (size_t i = 0; i < n; i++) {
            fields[i] = (uint8_t)rnd();
        }
    }
    const size_t edits = 1u + rnd_below(3u);
    for (size_t e = 0; e < edits; e++) {
        const size_t at = rnd_below(n + 1u);
        switch (rnd_below(6u)) {
        case 0:
            if (at < n) {
                fields[at] = (uint8_t)(fields[at] ^ (1u << rnd_below(8u)));
            }
            break;
        case 1:
            if (at < n) {
                fields[at] = edge[rnd_below(sizeof edge)];
            }
            break;
        case 2: n = at; break;
        case 3:
            if (n < room) {
                memmove(&fields[at + 1u], &fields[at], n - at);
                fields[at] = edge[rnd_below(sizeof edge)];
                n++;
            }
            break;
        case 4:
            if (at < n) {
                memmove(&fields[at], &fields[at + 1u], n - at - 1u);
                n--;
            }
            break;
        default: {
            const size_t add = rnd_below(24u);
            for (size_t i = 0; i < add && n < room; i++) {
                fields[n++] = (uint8_t)rnd();
            }
            break;
        }
        }
    }
    memcpy(&f[PLINK_HDR_LEN], fields, n);
    memset(&f[PLINK_HDR_LEN + n], 0, PLINK_MAC_LEN);
    *len = PLINK_HDR_LEN + n + PLINK_MAC_LEN;
}

/* LINK-015, LINK-017 (docs/features/11 "Test and acceptance": random bytes into every parser):
 * 600 rounds of a HELLO and a WELCOME with random edits in their fields, each MAC made right
 * again - mac1 under K_hs; mac2 under the K_fin of whatever n_m and e_m the edited WELCOME now
 * carries - so the parsers behind the MACs read them, each frame ending at the end of its buffer.
 * plink_hs_answer and plink_hs_finish never crash or read outside; what they accept makes a
 * session (a WELCOME that fits its buffer, a proto of 1 at least), and what finish refuses leaves
 * the HELLO waiting. */
static void test_fuzz_handshake(void)
{
    static uint8_t arena[256];
    uint8_t k_hs[PLINK_KEY_LEN], f[192], w[128];
    hello_key(R.k_pair, k_hs);
    RNG = 0x5E5510Bu;
    unsigned answered = 0u, finished = 0u;
    for (uint32_t iter = 0; iter < 600u; iter++) {
        TEST_ASSERT_TRUE(plink_hs_hello(&R.ck, R.k_pair, NET, DEV, &CAP, &R.hs));
        size_t len = R.hs.hello_len;
        memcpy(f, R.hs.hello, len);
        mutate_fields(f, &len, sizeof f);
        plink_mac_fill(k_hs, f, len);
        uint8_t *p = arena + sizeof arena - len;
        memcpy(p, f, len);
        plink_hdr_t h;
        TEST_ASSERT_TRUE(plink_hdr_read(p, len, &h));
        plink_session_t pend;
        plink_hello_t hello;
        size_t wl = 0u;
        if (plink_hs_answer(&R.cm, R.k_pair, &h, p, len, &pend, &hello, w, sizeof w, &wl)) {
            answered++;
            TEST_ASSERT_TRUE(pend.valid);
            TEST_ASSERT_TRUE(hello.proto != 0u);
            TEST_ASSERT_TRUE(wl >= PLINK_HDR_LEN + PLINK_MAC_LEN && wl <= sizeof w);
        }

        /* The master's answer to the honest HELLO, edited and its mac2 made again. */
        TEST_ASSERT_TRUE(answer(R.k_pair));
        len = R.welcome_len;
        memcpy(f, R.welcome, len);
        mutate_fields(f, &len, sizeof f);
        plink_hello_t wm;
        uint8_t dh[32];
        memset(dh, 0, sizeof dh);
        if (plink_dec_hello(&f[PLINK_HDR_LEN], len - PLINK_HDR_LEN, false, &wm)) {
            if (!plink_x25519_shared(&R.cx, R.hs.e, wm.e, dh)) {
                memset(dh, 0, sizeof dh); /* a point of small order: the DH is all zero */
            }
            mac2_fill(f, len, wm.n, dh);
        }
        p = arena + sizeof arena - len;
        memcpy(p, f, len);
        TEST_ASSERT_TRUE(plink_hdr_read(p, len, &h));
        plink_session_t s;
        memset(&s, 0, sizeof s);
        if (plink_hs_finish(&R.ck, R.k_pair, &R.hs, &h, p, len, &s)) {
            finished++;
            TEST_ASSERT_TRUE(s.valid);
            TEST_ASSERT_TRUE(s.proto >= 1u);
            TEST_ASSERT_FALSE(R.hs.waiting);
        } else {
            TEST_ASSERT_TRUE(R.hs.waiting);
            TEST_ASSERT_FALSE(s.valid);
        }
    }
    /* Many edits leave frames that parse, so the checks above ran on real handshakes. */
    TEST_ASSERT_TRUE(answered > 60u);
    TEST_ASSERT_TRUE(finished > 60u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_handshake);
    RUN_TEST(test_handshake_and_data_both_ways);
    RUN_TEST(test_wrong_k_pair);
    RUN_TEST(test_hello_tampered_or_stripped);
    RUN_TEST(test_replayed_welcome);
    RUN_TEST(test_finish_refusals_with_a_valid_mac2);
    RUN_TEST(test_replay_window);
    RUN_TEST(test_seq_limit);
    RUN_TEST(test_seq_limit_on_arrival);
    RUN_TEST(test_seal_failure_spends_its_seq);
    RUN_TEST(test_directions_use_different_keys);
    RUN_TEST(test_no_key_nonce_reused);
    RUN_TEST(test_proto_and_caps);
    RUN_TEST(test_handshake_hygiene);
    RUN_TEST(test_port_failures);
    RUN_TEST(test_fuzz_handshake);
    return UNITY_END();
}
