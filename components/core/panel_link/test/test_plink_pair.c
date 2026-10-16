/*
 * test_plink_pair.c - pairing a keypad with its master (components/core/panel_link plink_pair.h,
 * docs/features/11 "Pairing"): the commitment, the code, the keys and the frame MACs against
 * vectors made independently with Python's `cryptography` (vectors_plink_pair.py beside this file),
 * one honest pairing byte for byte with scripted random numbers, then the master's and the
 * keypad's state machines against each other, against a device in the middle and against an
 * attacker: every value set once per attempt, attempts counted when a secret goes, three per
 * window at least 3 s apart, one address per window, repeats answered with the same bytes, every
 * timer, a lost PAIR_DONE, the capacity check, cancelling, and the secrets wiped.
 *
 * Requirements: LINK-004 (commit-then-reveal, the same 6-digit code on both screens with the other
 * side's name and address, confirmed on both), LINK-016 (each value set once, attempts counted
 * when disclosed and limited to 3 at least 3 s apart, the window and step timers, two keypads or
 * two masters end the attempt, nothing kept before both confirmation MACs verify).
 */
#include <string.h>

#include "panel_link/plink_pair.h"
#include "plink_test_port.h"
#include "unity.h"

#define NET   0x5A17u
#define ZONES 24u /* the master's zones in use */

static const plink_caps_t CAP = {4u, 128u, 32u, 16u};
static const uint8_t IP_M[4] = {192u, 0u, 2u, 10u};
static const uint8_t IP_K[4] = {192u, 0u, 2u, 20u};
static const uint8_t IP_X[4] = {192u, 0u, 2u, 66u};

/* Both sides with their own ports, their addresses, and a clock. */
typedef struct {
    plink_test_port_t tpm, tpk, tpx;
    plink_crypto_t cm, ck, cx;
    plink_pair_m_t pm;
    plink_pair_k_t pk;
    plink_addr_t am, ak, ax; /* the master, the keypad, another device */
    uint64_t now;
} rig_t;

static rig_t R;
static plink_out_t A, B, C; /* frames in flight */

static void rig(uint64_t seed)
{
    memset(&R, 0, sizeof R);
    plink_test_port_init(&R.tpm, seed);
    plink_test_port_init(&R.tpk, seed ^ 0x5555AAAAu);
    plink_test_port_init(&R.tpx, seed ^ 0x0F0F0F0Fu);
    R.cm = plink_test_port(&R.tpm);
    R.ck = plink_test_port(&R.tpk);
    R.cx = plink_test_port(&R.tpx);
    R.am = plink_addr_udp(IP_M, PLINK_UDP_PORT);
    R.ak = plink_addr_udp(IP_K, PLINK_UDP_PORT);
    R.ax = plink_addr_udp(IP_X, PLINK_UDP_PORT);
    R.now = 1000000u;
    plink_pm_init(&R.pm, &R.cm);
    plink_pm_identity(&R.pm, NET, "Harbor House", "a1b2c3", ZONES);
    plink_pk_init(&R.pk, &R.ck, PLINK_KIND_PANEL, &CAP, "Hall keypad", "1.4.0");
}

void setUp(void)
{
    rig(0x9A1Bu);
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    memset(&C, 0, sizeof C);
}

void tearDown(void)
{
}

/* ---- helpers ------------------------------------------------------------------------------ */

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void expect_hex(const char *hex, const uint8_t *got, size_t n)
{
    uint8_t want[64];
    size_t len = 0;
    for (const char *p = hex; p[0] != '\0' && p[1] != '\0'; p += 2) {
        TEST_ASSERT_TRUE(nibble(p[0]) >= 0 && nibble(p[1]) >= 0 && len < sizeof want);
        want[len++] = (uint8_t)((unsigned)nibble(p[0]) << 4 | (unsigned)nibble(p[1]));
    }
    TEST_ASSERT_EQUAL_UINT(len, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, n);
}

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

static uint8_t type_of(const plink_out_t *o)
{
    return o->len > 3u ? o->buf[3] : 0u;
}

static uint32_t attempt_of(const uint8_t *frame)
{
    const uint8_t *p = &frame[PLINK_HDR_LEN];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* The reason in a PAIR_FAIL (the test fails when o isn't one). */
static uint8_t fail_reason(const plink_out_t *o)
{
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_FAIL, type_of(o));
    plink_pair_fail_t f;
    TEST_ASSERT_TRUE(plink_dec_pair_fail(&o->buf[PLINK_HDR_LEN], o->len - PLINK_HDR_LEN, &f));
    return f.reason;
}

static plink_pair_info_t m_info(void)
{
    plink_pair_info_t i;
    plink_pm_info(&R.pm, R.now, &i);
    return i;
}

static plink_pair_info_t k_info(void)
{
    plink_pair_info_t i;
    plink_pk_info(&R.pk, R.now, &i);
    return i;
}

/* A frame to the master from `from`; its answer into ans. */
static void master_gets(const plink_addr_t *from, const uint8_t *frame, size_t len,
                        plink_out_t *ans)
{
    plink_hdr_t h;
    TEST_ASSERT_TRUE_MESSAGE(plink_hdr_read(frame, len, &h), "a frame the master would drop");
    plink_pm_input(&R.pm, R.now, from, &h, frame, len, ans);
}

/* A frame to a keypad from `from`; its answer into ans. */
static void keypad_gets(plink_pair_k_t *pk, const plink_addr_t *from, const uint8_t *frame,
                        size_t len, plink_out_t *ans)
{
    plink_hdr_t h;
    TEST_ASSERT_TRUE_MESSAGE(plink_hdr_read(frame, len, &h), "a frame the keypad would drop");
    plink_pk_input(pk, R.now, from, &h, frame, len, ans);
}

/* The keypad's frame o goes to the master over the network. */
static void to_master(const plink_out_t *o, plink_out_t *ans)
{
    TEST_ASSERT_NOT_EQUAL(0u, o->len);
    TEST_ASSERT_TRUE(plink_addr_equal(&o->to, &R.am));
    master_gets(&R.ak, o->buf, o->len, ans);
}

/* The master's frame o goes to the keypad. */
static void to_keypad(const plink_out_t *o, plink_out_t *ans)
{
    TEST_ASSERT_NOT_EQUAL(0u, o->len);
    TEST_ASSERT_TRUE(plink_addr_equal(&o->to, &R.ak));
    keypad_gets(&R.pk, &R.am, o->buf, o->len, ans);
}

/* Window open, one attempt up to the code on both screens; A holds the REVEAL the keypad took. */
static void to_compare(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REQ, type_of(&A));
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&C));
    to_master(&C, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REVEAL, type_of(&A));
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);
}

/* From the code on both screens to paired on both: B holds PAIR_DONE. */
static void compare_to_done(uint8_t device, uint8_t home)
{
    plink_pk_confirm(&R.pk, R.now, true, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_CONFIRM, type_of(&A));
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len); /* the installer hasn't allowed it yet */
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));
    plink_pm_decide(&R.pm, R.now, true, &C);
    TEST_ASSERT_EQUAL_UINT(0u, C.len);
    TEST_ASSERT_TRUE(plink_pm_take(&R.pm, NULL));
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, device, home, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_DONE, type_of(&B));
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT(0u, C.len);
}

/* Nothing of an attempt's secrets is left on either side. */
static void expect_master_wiped(const plink_pair_m_t *pm)
{
    TEST_ASSERT_TRUE(all_zero(pm->sk, sizeof pm->sk));
    TEST_ASSERT_TRUE(all_zero(pm->dh, sizeof pm->dh));
    TEST_ASSERT_TRUE(all_zero(pm->n_m, sizeof pm->n_m));
    TEST_ASSERT_TRUE(all_zero(pm->n_k, sizeof pm->n_k));
    TEST_ASSERT_TRUE(all_zero(pm->k_pair, sizeof pm->k_pair));
    TEST_ASSERT_TRUE(all_zero(pm->k_conf, sizeof pm->k_conf));
    TEST_ASSERT_TRUE(all_zero(pm->code, sizeof pm->code));
}

static void expect_keypad_wiped(const plink_pair_k_t *pk)
{
    TEST_ASSERT_TRUE(all_zero(pk->sk, sizeof pk->sk));
    TEST_ASSERT_TRUE(all_zero(pk->dh, sizeof pk->dh));
    TEST_ASSERT_TRUE(all_zero(pk->n_k, sizeof pk->n_k));
    TEST_ASSERT_TRUE(all_zero(pk->n_m, sizeof pk->n_m));
    TEST_ASSERT_TRUE(all_zero(pk->k_pair, sizeof pk->k_pair));
    TEST_ASSERT_TRUE(all_zero(pk->k_conf, sizeof pk->k_conf));
    TEST_ASSERT_TRUE(all_zero(pk->code, sizeof pk->code));
    TEST_ASSERT_TRUE(all_zero(&pk->result, sizeof pk->result));
}

/* ---- scripted random numbers ---------------------------------------------------------------- */

/* The test port's AES and X25519 with random numbers from a script (the golden pairing); a draw
 * past its end fails. */
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

/* ---- the code, the commitment, the keys, the MACs ----------------------------------------- */

/* LINK-004: the commitment, the 6-digit code and K_pair / K_conf from fixed inputs are what Python
 * computed (vectors_plink_pair.py: pk_k 00..1f, pk_m 20..3f, n_k 40..4f, n_m 50..5f, DH 60..7f, a
 * 50-byte PAIR_REQ payload a0 + i, a 60-byte PAIR_COMMIT payload b0 + 3 i, net id 0x5A17). */
static void test_vectors(void)
{
    uint8_t pk_k[32], pk_m[32], n_k[16], n_m[16], dh[32], req[50], commit[60];
    fill(pk_k, sizeof pk_k, 1u, 0x00u);
    fill(pk_m, sizeof pk_m, 1u, 0x20u);
    fill(n_k, sizeof n_k, 1u, 0x40u);
    fill(n_m, sizeof n_m, 1u, 0x50u);
    fill(dh, sizeof dh, 1u, 0x60u);
    fill(req, sizeof req, 1u, 0xA0u);
    fill(commit, sizeof commit, 3u, 0xB0u);

    uint8_t c[32];
    plink_pair_commitment(pk_m, pk_k, n_m, c);
    expect_hex("679945ac08f0af29319a9520c42d68f781a97e93a0f999b3eedaeaba71e1f8bc", c, sizeof c);

    char code[PLINK_CODE_LEN];
    plink_pair_code(pk_k, pk_m, n_k, n_m, code);
    TEST_ASSERT_EQUAL_STRING("800244", code);

    uint8_t k_pair[32], k_conf[32];
    TEST_ASSERT_TRUE(plink_pair_keys(dh, req, sizeof req, commit, sizeof commit, NET, n_k, n_m,
                                     k_pair, k_conf));
    expect_hex("eff14f5774ca652f6aa25d6c8443f1b3cd3e0e11570f39d114fc659371c9dc65", k_pair, 32);
    expect_hex("360a8fb45f2e6c526d1cbb0538ec84f5c64207df07d4d0e4ba07576f0c60c08b", k_conf, 32);
}

/* LINK-004: the code keeps its leading zeros - "000585" for n_m = 468 (Python searched for the
 * first n_m giving a code below 1000) - and is always six digits. */
static void test_code_leading_zeros(void)
{
    uint8_t pk_k[32], pk_m[32], n_k[16], n_m[16] = {0};
    fill(pk_k, sizeof pk_k, 1u, 0x00u);
    fill(pk_m, sizeof pk_m, 1u, 0x20u);
    fill(n_k, sizeof n_k, 1u, 0x40u);
    n_m[0] = (uint8_t)(468u & 0xFFu);
    n_m[1] = (uint8_t)(468u >> 8);
    char code[PLINK_CODE_LEN];
    plink_pair_code(pk_k, pk_m, n_k, n_m, code);
    TEST_ASSERT_EQUAL_STRING("000585", code);
    for (unsigned i = 0; i < 200u; i++) {
        n_m[2] = (uint8_t)i;
        plink_pair_code(pk_k, pk_m, n_k, n_m, code);
        TEST_ASSERT_EQUAL_size_t(6u, strlen(code));
        for (size_t d = 0; d < 6u; d++) {
            TEST_ASSERT_TRUE(code[d] >= '0' && code[d] <= '9');
        }
    }
    plink_pair_code(NULL, pk_m, n_k, n_m, code);
    TEST_ASSERT_EQUAL_STRING("", code);
}

/* LINK-016: the keys bind the whole transcript - any change of the payloads, the net id or a nonce
 * changes both, and so does moving a byte from one payload to the other (the length prefixes);
 * an all-zero shared secret is refused and leaves the outputs zero. */
static void test_keys_bind_transcript(void)
{
    uint8_t n_k[16], n_m[16], dh[32], buf[110], kp0[32], kc0[32], kp[32], kc[32];
    fill(n_k, sizeof n_k, 1u, 0x40u);
    fill(n_m, sizeof n_m, 1u, 0x50u);
    fill(dh, sizeof dh, 1u, 0x60u);
    fill(buf, sizeof buf, 7u, 3u);
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET, n_k, n_m, kp0, kc0));
    TEST_ASSERT_FALSE(all_zero(kp0, 32u));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp0, kc0, 32u));

    /* The same 110 bytes split 49 / 61. */
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 49u, &buf[49], 61u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp0, kp, 32u));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kc0, kc, 32u));
    /* Another net id, another n_k, another n_m, another payload byte. */
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET + 1u, n_k, n_m, kp, kc));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp0, kp, 32u));
    n_k[15] ^= 1u;
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp0, kp, 32u));
    n_k[15] ^= 1u;
    n_m[0] ^= 0x80u;
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kc0, kc, 32u));
    n_m[0] ^= 0x80u;
    buf[100] ^= 1u;
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp0, kp, 32u));
    buf[100] ^= 1u;
    TEST_ASSERT_TRUE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kp0, kp, 32u);

    /* X25519 refuses all zero; so do the keys, as a second line. */
    uint8_t zero[32] = {0};
    memset(kp, 0xEE, sizeof kp);
    memset(kc, 0xEE, sizeof kc);
    TEST_ASSERT_FALSE(plink_pair_keys(zero, buf, 50u, &buf[50], 60u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_TRUE(all_zero(kp, 32u));
    TEST_ASSERT_TRUE(all_zero(kc, 32u));
    TEST_ASSERT_FALSE(plink_pair_keys(dh, NULL, 50u, &buf[50], 60u, NET, n_k, n_m, kp, kc));
    TEST_ASSERT_FALSE(plink_pair_keys(dh, buf, 50u, &buf[50], 60u, NET, NULL, n_m, kp, kc));
}

/* LINK-004: a frame's MAC is HMAC(key, everything before its last 16 bytes) cut to 16 (Python:
 * key 70..8f over a 37-byte frame of 3 i + 1), checked in constant time; every flipped bit of the
 * frame or the MAC fails, and a frame shorter than a MAC is never filled nor accepted. */
static void test_mac_fill_check(void)
{
    uint8_t key[32], frame[37], other[32];
    fill(key, sizeof key, 1u, 0x70u);
    fill(frame, sizeof frame, 3u, 1u);
    plink_mac_fill(key, frame, sizeof frame);
    expect_hex("a6ed31392610d516aabcf7d833ae853e", &frame[21], 16u);
    TEST_ASSERT_TRUE(plink_mac_check(key, frame, sizeof frame));
    for (size_t i = 0; i < sizeof frame; i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            frame[i] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_FALSE(plink_mac_check(key, frame, sizeof frame));
            frame[i] ^= (uint8_t)(1u << bit);
        }
    }
    memcpy(other, key, sizeof other);
    other[31] ^= 1u;
    TEST_ASSERT_FALSE(plink_mac_check(other, frame, sizeof frame));
    uint8_t shortf[15];
    fill(shortf, sizeof shortf, 1u, 0u);
    plink_mac_fill(key, shortf, sizeof shortf);
    for (size_t i = 0; i < sizeof shortf; i++) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)i, shortf[i]);
    }
    TEST_ASSERT_FALSE(plink_mac_check(key, shortf, sizeof shortf));
    TEST_ASSERT_FALSE(plink_mac_check(NULL, frame, sizeof frame));
}

/* ---- an honest pairing -------------------------------------------------------------------- */

/* vectors_plink_pair.py: the keypad's random numbers are (0x31 i + 7) mod 256, i = 0..51 (attempt
 * id, private key, n_k), the master's (0x57 i + 0x13) mod 256, i = 0..47 (private key, n_m). */
static const uint8_t GOLD_REQ[81] = {
    0x41, 0x4c, 0x01, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x38, 0x69, 0x9a, 0x01, 0x00, 0x00, 0x01, 0x05, 0x04, 0x80, 0x00, 0x20, 0x10, 0x50,
    0xcc, 0x4d, 0xa0, 0x40, 0x07, 0xe0, 0x37, 0xc1, 0x7b, 0x96, 0x80, 0xd5, 0x6b, 0x92, 0x67, 0x0e,
    0x5a, 0x7e, 0x25, 0xd6, 0xec, 0xd0, 0x4f, 0xde, 0xfb, 0x05, 0xbb, 0x51, 0x58, 0x0c, 0x5e, 0x0b,
    0x48, 0x61, 0x6c, 0x6c, 0x20, 0x6b, 0x65, 0x79, 0x70, 0x61, 0x64, 0x05, 0x31, 0x2e, 0x34, 0x2e,
    0x30,
};
static const uint8_t GOLD_COMMIT[108] = {
    0x41, 0x4c, 0x01, 0x02, 0x02, 0x17, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x38, 0x69, 0x9a, 0x01, 0x00, 0x00, 0xe7, 0x85, 0xfe, 0x83, 0x2a, 0x35, 0xe2, 0x32,
    0xbd, 0x33, 0xaa, 0x84, 0x28, 0xd4, 0x9f, 0x5f, 0xf4, 0xc1, 0x79, 0x68, 0xf9, 0x09, 0x37, 0x0a,
    0xc0, 0xee, 0x90, 0x5b, 0x4c, 0x3d, 0x69, 0x42, 0x04, 0xf9, 0x82, 0xfc, 0x7f, 0x1f, 0x6a, 0x7e,
    0xd5, 0x36, 0x3f, 0x60, 0x4f, 0xfc, 0x0f, 0x3e, 0xee, 0xc1, 0x82, 0xfa, 0x24, 0x51, 0x05, 0x16,
    0x39, 0x7d, 0x96, 0xc3, 0xb9, 0xff, 0x46, 0x4d, 0x0c, 0x48, 0x61, 0x72, 0x62, 0x6f, 0x72, 0x20,
    0x48, 0x6f, 0x75, 0x73, 0x65, 0x06, 0x61, 0x31, 0x62, 0x32, 0x63, 0x33,
};
static const uint8_t GOLD_NONCE[37] = {
    0x41, 0x4c, 0x01, 0x03, 0x00, 0x17, 0x5a, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x38, 0x69, 0x9a, 0xeb, 0x1c, 0x4d, 0x7e, 0xaf, 0xe0, 0x11, 0x42, 0x73, 0xa4, 0xd5,
    0x06, 0x37, 0x68, 0x99, 0xca,
};
static const uint8_t GOLD_REVEAL[37] = {
    0x41, 0x4c, 0x01, 0x04, 0x02, 0x17, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x38, 0x69, 0x9a, 0xf3, 0x4a, 0xa1, 0xf8, 0x4f, 0xa6, 0xfd, 0x54, 0xab, 0x02, 0x59,
    0xb0, 0x07, 0x5e, 0xb5, 0x0c,
};
static const uint8_t GOLD_CONFIRM[37] = {
    0x41, 0x4c, 0x01, 0x05, 0x00, 0x17, 0x5a, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x38, 0x69, 0x9a, 0x0b, 0xda, 0xa0, 0x19, 0xf6, 0xaa, 0x99, 0xd2, 0xaf, 0x60, 0xb0,
    0x25, 0xb3, 0xba, 0xcc, 0xe2,
};
static const uint8_t GOLD_DONE[39] = {
    0x41, 0x4c, 0x01, 0x06, 0x02, 0x17, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x38, 0x69, 0x9a, 0x03, 0x01, 0xf3, 0x09, 0x3b, 0x8f, 0x0d, 0x09, 0x7c, 0x41, 0xc4,
    0xee, 0xa4, 0xe8, 0x3f, 0x7c, 0xdb, 0x4f,
};
static const uint8_t GOLD_K_PAIR[32] = {
    0x75, 0x8a, 0x19, 0xfc, 0xb8, 0x54, 0x3b, 0x40, 0xf2, 0xec, 0xcb, 0x39, 0x28, 0x1a, 0x25, 0xac,
    0x7f, 0x9d, 0xb0, 0x3d, 0x2d, 0x9b, 0x8d, 0xab, 0xb4, 0x44, 0xc8, 0x50, 0x0e, 0x29, 0x10, 0xa1,
};

static void expect_frame(const uint8_t *want, size_t len, const plink_out_t *o)
{
    TEST_ASSERT_EQUAL_UINT(len, o->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, o->buf, len);
}

/* LINK-004: one honest pairing, every frame byte for byte as Python built it from the same random
 * numbers - PAIR_REQ, PAIR_COMMIT, PAIR_NONCE, PAIR_REVEAL, PAIR_CONFIRM (mac_k), PAIR_DONE
 * (mac_m) - the same code "473309" on both screens with the other side's name and address, the
 * same K_pair on both sides, and every scripted random byte drawn (no draw more, none less). */
static void test_golden_pairing(void)
{
    static uint8_t kp_bytes[52], ms_bytes[48];
    static script_t ks, ms;
    fill(kp_bytes, sizeof kp_bytes, 0x31u, 7u);
    fill(ms_bytes, sizeof ms_bytes, 0x57u, 0x13u);
    R.ck = script_port(&ks, &R.tpk, kp_bytes, sizeof kp_bytes);
    R.cm = script_port(&ms, &R.tpm, ms_bytes, sizeof ms_bytes);
    plink_pm_init(&R.pm, &R.cm);
    plink_pm_identity(&R.pm, NET, "Harbor House", "a1b2c3", ZONES);
    plink_pk_init(&R.pk, &R.ck, PLINK_KIND_PANEL, &CAP, "Hall keypad", "1.4.0");

    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    expect_frame(GOLD_REQ, sizeof GOLD_REQ, &A);
    to_master(&A, &B);
    expect_frame(GOLD_COMMIT, sizeof GOLD_COMMIT, &B);
    to_keypad(&B, &C);
    expect_frame(GOLD_NONCE, sizeof GOLD_NONCE, &C);
    to_master(&C, &A);
    expect_frame(GOLD_REVEAL, sizeof GOLD_REVEAL, &A);
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);

    const plink_pair_info_t mi = m_info(), ki = k_info();
    TEST_ASSERT_EQUAL_STRING("473309", mi.code);
    TEST_ASSERT_EQUAL_STRING("473309", ki.code);
    TEST_ASSERT_EQUAL_STRING("Hall keypad", mi.peer_name);
    TEST_ASSERT_EQUAL_STRING("Harbor House", ki.peer_name);
    TEST_ASSERT_TRUE(plink_addr_equal(&mi.peer, &R.ak));
    TEST_ASSERT_TRUE(plink_addr_equal(&ki.peer, &R.am));
    TEST_ASSERT_EQUAL_UINT8(1u, mi.attempts);
    TEST_ASSERT_EQUAL_UINT8(1u, ki.attempts);

    plink_pk_confirm(&R.pk, R.now, true, &A);
    expect_frame(GOLD_CONFIRM, sizeof GOLD_CONFIRM, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    plink_pm_decide(&R.pm, R.now, true, &C);
    plink_pair_new_t nk;
    TEST_ASSERT_TRUE(plink_pm_take(&R.pm, &nk));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_K_PAIR, nk.k_pair, 32u);
    TEST_ASSERT_EQUAL_STRING("Hall keypad", nk.name);
    TEST_ASSERT_EQUAL_UINT8(PLINK_KIND_PANEL, nk.kind);
    TEST_ASSERT_EQUAL_UINT16(128u, nk.cap.zones);
    TEST_ASSERT_EQUAL_UINT8(4u, nk.cap.partitions);
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, 3u, 1u, &B);
    expect_frame(GOLD_DONE, sizeof GOLD_DONE, &B);
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT(0u, C.len);

    plink_keypad_rec_t rec;
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLD_K_PAIR, rec.k_pair, 32u);
    TEST_ASSERT_EQUAL_UINT16(NET, rec.net_id);
    TEST_ASSERT_EQUAL_UINT8(3u, rec.device);
    TEST_ASSERT_EQUAL_UINT8(1u, rec.home);
    TEST_ASSERT_EQUAL_STRING("Harbor House", rec.master_name);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", rec.panel_id);
    TEST_ASSERT_FALSE(plink_pk_take(&R.pk, &rec)); /* once */
    TEST_ASSERT_EQUAL_size_t(sizeof kp_bytes, ks.pos);
    TEST_ASSERT_EQUAL_size_t(sizeof ms_bytes, ms.pos);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(3u, m_info().device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(3u, k_info().device);
}

/* LINK-004: with the seeded port, both orders of the two confirmations end paired - the installer
 * first, then the keypad's PAIR_CONFIRM; and the keypad first (compare_to_done) - with both codes
 * equal and six digits, both K_pair equal, and the master's window closed. */
static void test_honest_pairing_both_orders(void)
{
    to_compare();
    TEST_ASSERT_EQUAL_STRING(m_info().code, k_info().code);
    TEST_ASSERT_EQUAL_size_t(6u, strlen(m_info().code));
    plink_pm_decide(&R.pm, R.now, true, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_WAITING, m_info().state);
    TEST_ASSERT_EQUAL_STRING(k_info().code, m_info().code); /* still shown while waiting */
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));
    plink_pk_confirm(&R.pk, R.now, true, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_WAITING, k_info().state);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    plink_pair_new_t nk;
    TEST_ASSERT_TRUE(plink_pm_take(&R.pm, &nk));
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, 5u, 2u, &B);
    to_keypad(&B, &C);
    plink_keypad_rec_t rec;
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(nk.k_pair, rec.k_pair, 32u);
    TEST_ASSERT_FALSE(all_zero(rec.k_pair, 32u));
    TEST_ASSERT_EQUAL_UINT8(5u, rec.device);
    TEST_ASSERT_EQUAL_UINT8(2u, rec.home);
    TEST_ASSERT_EQUAL_UINT32(0u, m_info().left_s);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 0u, plink_pk_next(&R.pk));

    rig(0x77u);
    to_compare();
    TEST_ASSERT_EQUAL_STRING(m_info().code, k_info().code);
    compare_to_done(1u, 1u);
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_UINT8(1u, rec.device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, m_info().state);
}

/* ---- a device in the middle --------------------------------------------------------------- */

/* LINK-004, LINK-016: a device in the middle swaps pk_k in PAIR_REQ for its own key and passes
 * everything else on. The master commits to the swapped key, so the keypad finds n_m doesn't
 * match the commitment: it ends the attempt (MISMATCH) without showing a code and tells the
 * master, which ends too. No code on either screen, nothing to take, both attempts counted. */
static void test_mitm_substitutes_pk_k(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    uint8_t sk_x[32], pk_x[32];
    TEST_ASSERT_TRUE(plink_x25519_keypair(&R.cx, sk_x, pk_x));
    memcpy(&A.buf[31], pk_x, 32u); /* PAIR_REQ's pk_k: after attempt, proto, caps, kind, caps */
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));
    to_keypad(&B, &C); /* PAIR_NONCE */
    to_master(&C, &A); /* PAIR_REVEAL */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REVEAL, type_of(&A));
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, fail_reason(&B));
    plink_pair_info_t ki = k_info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, ki.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, ki.fail);
    TEST_ASSERT_EQUAL_STRING("", ki.code);
    TEST_ASSERT_EQUAL_UINT8(1u, ki.attempts);
    to_master(&B, &C);
    TEST_ASSERT_EQUAL_UINT(0u, C.len);
    const plink_pair_info_t mi = m_info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, mi.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, mi.fail);
    TEST_ASSERT_EQUAL_STRING("", mi.code);
    TEST_ASSERT_EQUAL_UINT8(1u, mi.attempts);
    plink_pair_new_t nk;
    plink_keypad_rec_t rec;
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, &nk));
    TEST_ASSERT_FALSE(plink_pk_take(&R.pk, &rec));
    expect_master_wiped(&R.pm);
    expect_keypad_wiped(&R.pk);
}

/* LINK-004: a device in the middle that runs a whole pairing with each side (its own keys and
 * nonces both ways) gets each side to show a code, but it must choose its values before it learns
 * the other side's nonce, so the two codes differ (a match is a one-in-a-million chance) and the
 * people comparing them say no. Its confirmation on the master is valid (it holds that side's
 * keys): only the installer's comparison stops it. */
static void test_mitm_full_codes_differ(void)
{
    static plink_pair_m_t xm; /* the device's master side, facing the keypad */
    static plink_pair_k_t xk; /* its keypad side, facing the master */
    plink_pm_init(&xm, &R.cx);
    plink_pm_identity(&xm, NET, "Harbor House", "a1b2c3", ZONES);
    plink_pk_init(&xk, &R.cx, PLINK_KIND_PANEL, &CAP, "Hall keypad", "1.4.0");
    plink_pm_open(&R.pm, R.now);
    plink_pm_open(&xm, R.now);
    plink_hdr_t h;

    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A); /* the keypad's PAIR_REQ, intercepted */
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&xm, R.now, &R.ak, &h, A.buf, A.len, &B); /* the device commits to the keypad */
    plink_pk_start(&xk, R.now, &R.am, 0u, &C);                  /* and asks the master itself */
    master_gets(&R.ak, C.buf, C.len, &A);                       /* spoofing the keypad's address */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&A));
    keypad_gets(&xk, &R.am, A.buf, A.len, &C); /* its n_x for the master */
    master_gets(&R.ak, C.buf, C.len, &A);      /* the master reveals n_m */
    keypad_gets(&xk, &R.am, A.buf, A.len, &C);
    keypad_gets(&R.pk, &R.am, B.buf, B.len, &A); /* the keypad takes the device's commitment */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&A));
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&xm, R.now, &R.ak, &h, A.buf, A.len, &B);
    keypad_gets(&R.pk, &R.am, B.buf, B.len, &A);

    const plink_pair_info_t mi = m_info(), ki = k_info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, mi.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, ki.state);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(mi.code, ki.code));
    /* The people compare and refuse on both sides: nothing is kept. */
    plink_pk_confirm(&R.pk, R.now, false, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_REFUSED, fail_reason(&A));
    plink_pk_confirm(&xk, R.now, true, &C); /* the device confirms with the master... */
    master_gets(&R.ak, C.buf, C.len, &B);
    plink_pm_decide(&R.pm, R.now, false, &B); /* ...whose installer saw another code */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_REFUSED, fail_reason(&B));
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));
    plink_keypad_rec_t rec;
    TEST_ASSERT_FALSE(plink_pk_take(&R.pk, &rec));
    expect_master_wiped(&R.pm);
}

/* LINK-016: a second, different PAIR_COMMIT for the attempt after the keypad sent n_k (a device
 * in the middle trying to change the commitment once it knows n_k) ends the attempt: INTERRUPTED,
 * told to the master, counted (n_k was disclosed), no code, nothing to take - on both sides. */
static void test_commit_after_nonce(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B); /* PAIR_COMMIT */
    static plink_out_t commit;
    commit = B;
    to_keypad(&B, &C); /* PAIR_NONCE */
    to_master(&C, &A); /* PAIR_REVEAL, held back by the device */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REVEAL, type_of(&A));
    commit.buf[60] ^= 0x01u; /* another commitment */
    keypad_gets(&R.pk, &R.am, commit.buf, commit.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, fail_reason(&B));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(1u, k_info().attempts);
    to_keypad(&A, &C); /* the REVEAL now: the attempt is over */
    TEST_ASSERT_EQUAL_UINT(0u, C.len);
    TEST_ASSERT_EQUAL_STRING("", k_info().code);
    to_master(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, m_info().fail);
    TEST_ASSERT_EQUAL_STRING("", m_info().code);
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));
    plink_keypad_rec_t rec;
    TEST_ASSERT_FALSE(plink_pk_take(&R.pk, &rec));
    expect_keypad_wiped(&R.pk);
    expect_master_wiped(&R.pm);
}

/* LINK-016: a second, different PAIR_NONCE for the attempt after the master revealed n_m (a device
 * in the middle trying a new n_k once it knows n_m) ends the attempt on the master: INTERRUPTED,
 * told to the keypad, which drops its code; counted; nothing to take on either side. */
static void test_nonce_after_reveal(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    to_keypad(&B, &C); /* PAIR_NONCE */
    static plink_out_t nonce;
    nonce = C;
    to_master(&C, &A); /* PAIR_REVEAL */
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);
    nonce.buf[30] ^= 0x40u; /* another n_k */
    master_gets(&R.ak, nonce.buf, nonce.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, fail_reason(&B));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(1u, m_info().attempts);
    TEST_ASSERT_EQUAL_STRING("", m_info().code);
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, k_info().fail);
    TEST_ASSERT_EQUAL_STRING("", k_info().code);
    plink_pk_confirm(&R.pk, R.now, true, &A); /* too late: nothing to confirm */
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));
    expect_master_wiped(&R.pm);
    expect_keypad_wiped(&R.pk);
}

/* LINK-004: forged confirmations are refused and end the attempt: a PAIR_CONFIRM whose mac_k
 * isn't K_conf's (MISMATCH at the master, nothing to take even if the installer allowed), and a
 * PAIR_DONE whose mac_m isn't (MISMATCH at the keypad, nothing stored). */
static void test_forged_confirmations(void)
{
    to_compare();
    plink_pm_decide(&R.pm, R.now, true, &A);
    plink_pk_confirm(&R.pk, R.now, true, &A);
    A.buf[A.len - 1u] ^= 0x01u;
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, fail_reason(&B));
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, m_info().fail);

    rig(0x4242u);
    to_compare();
    plink_pk_confirm(&R.pk, R.now, true, &A);
    to_master(&A, &B);
    plink_pm_decide(&R.pm, R.now, true, &B);
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, 2u, 1u, &B);
    B.buf[PLINK_HDR_LEN + 4u] = 4u; /* the device number changed on the way */
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, fail_reason(&C));
    plink_keypad_rec_t rec;
    TEST_ASSERT_FALSE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_MISMATCH, k_info().fail);
    expect_keypad_wiped(&R.pk);
}

/* ---- the attempt limit, the gap, the window ----------------------------------------------- */

/* LINK-016: an attacker (one address) that runs each attempt up to PAIR_REVEAL - learning n_m for
 * a code it can then try offline - and aborts is stopped after 3: each counts on the master as it
 * reveals, a request within 3 s of the last attempt isn't answered, and after the third the window
 * is closed (LIMIT): further requests, the honest keypad's too, get CLOSED. */
static void test_attacker_aborts_after_reveal(void)
{
    static plink_pair_k_t atk;
    plink_pk_init(&atk, &R.cx, PLINK_KIND_PC, &CAP, "x", "0");
    plink_pm_open(&R.pm, R.now);
    for (unsigned i = 0; i < 3u; i++) {
        plink_pk_start(&atk, R.now, &R.am, 0u, &A); /* a new pairing each time */
        master_gets(&R.ax, A.buf, A.len, &B);
        TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));
        keypad_gets(&atk, &R.am, B.buf, B.len, &C);
        master_gets(&R.ax, C.buf, C.len, &A);
        TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REVEAL, type_of(&A));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1u), m_info().attempts);
        keypad_gets(&atk, &R.am, A.buf, A.len, &B);
        plink_pk_cancel(&atk, R.now, &C); /* aborts */
        master_gets(&R.ax, C.buf, C.len, &A);
        TEST_ASSERT_EQUAL_UINT(0u, A.len);
        TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, m_info().state);
        if (i < 2u) {
            TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CANCELLED, m_info().fail);
            TEST_ASSERT_NOT_EQUAL(0u, m_info().left_s);
            /* 2.999 s later: not answered. */
            R.now += PLINK_PAIR_GAP_MS - 1u;
            plink_pk_start(&atk, R.now, &R.am, 0u, &A);
            master_gets(&R.ax, A.buf, A.len, &B);
            TEST_ASSERT_EQUAL_UINT(0u, B.len);
            R.now += 1u;
        }
    }
    const plink_pair_info_t mi = m_info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_LIMIT, mi.fail);
    TEST_ASSERT_EQUAL_UINT8(3u, mi.attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, mi.left_s);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_pm_next(&R.pm));
    R.now += 10000u;
    plink_pk_start(&atk, R.now, &R.am, 0u, &A);
    master_gets(&R.ax, A.buf, A.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&B));
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&B));
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts); /* nothing disclosed: not counted */
}

/* LINK-016: the keypad's own side: 3 attempts per pairing a person started, counted when n_k goes;
 * a retry sooner than 3 s after the last attempt waits for it; after the third, retry refuses
 * (LIMIT) until a person starts a new pairing. */
static void test_keypad_attempt_limit(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    for (unsigned i = 0; i < 3u; i++) {
        to_master(&A, &B);
        to_keypad(&B, &C); /* n_k goes: counted */
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1u), k_info().attempts);
        to_master(&C, &A);
        to_keypad(&A, &B);
        plink_pk_confirm(&R.pk, R.now, false, &A); /* the codes didn't match */
        to_master(&A, &B);
        if (i < 2u) {
            R.now += 1000u;
            plink_pk_retry(&R.pk, R.now, &A); /* too soon: held */
            TEST_ASSERT_EQUAL_UINT(0u, A.len);
            TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, k_info().state);
            TEST_ASSERT_EQUAL_UINT64(R.now + 2000u, plink_pk_next(&R.pk));
            R.now += 2000u;
            plink_pk_tick(&R.pk, R.now, &A); /* 3 s after: it goes */
            TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REQ, type_of(&A));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_LIMIT, m_info().fail); /* the master's window closed too */
    R.now += 5000u;
    plink_pk_retry(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_LIMIT, k_info().fail);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A); /* a person starts again */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REQ, type_of(&A));
    TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
}

/* ---- repeats ------------------------------------------------------------------------------ */

/* LINK-016: repeats byte for byte get the stored answer byte for byte and change nothing: the
 * keypad's resent PAIR_REQ gets the same PAIR_COMMIT, a repeated PAIR_COMMIT the same PAIR_NONCE,
 * a repeated PAIR_NONCE the same PAIR_REVEAL, a repeated PAIR_REVEAL nothing before the person
 * confirms and the same PAIR_CONFIRM after; a repeated PAIR_CONFIRM waits for the installer. The
 * attempt is counted once. */
static void test_repeats_same_bytes(void)
{
    static plink_out_t req, commit, nonce, reveal, confirm;
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &req);
    to_master(&req, &commit);
    R.now += PLINK_PAIR_RESEND_MS;
    plink_pk_tick(&R.pk, R.now, &A); /* the keypad resends: PAIR_COMMIT was lost */
    expect_frame(req.buf, req.len, &A);
    to_master(&A, &B);
    expect_frame(commit.buf, commit.len, &B);
    to_keypad(&commit, &nonce);
    to_keypad(&commit, &A); /* the network repeated PAIR_COMMIT */
    expect_frame(nonce.buf, nonce.len, &A);
    to_master(&nonce, &reveal);
    to_master(&nonce, &A);
    expect_frame(reveal.buf, reveal.len, &A);
    to_master(&req, &A); /* a late copy of the request */
    expect_frame(commit.buf, commit.len, &A);
    to_keypad(&reveal, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    to_keypad(&reveal, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    plink_pk_confirm(&R.pk, R.now, true, &confirm);
    to_keypad(&reveal, &A);
    expect_frame(confirm.buf, confirm.len, &A);
    to_master(&confirm, &A);
    to_master(&confirm, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(1u, m_info().attempts);
    TEST_ASSERT_EQUAL_UINT8(1u, k_info().attempts);
    TEST_ASSERT_EQUAL_STRING(m_info().code, k_info().code);
    plink_pm_decide(&R.pm, R.now, true, &A);
    TEST_ASSERT_TRUE(plink_pm_take(&R.pm, NULL));
}

/* LINK-016: within an attempt, a changed message for a step already taken ends it, whichever
 * step: a changed PAIR_REQ (same attempt id), a changed PAIR_CONFIRM after a good one, a changed
 * PAIR_REVEAL at the keypad. Each is INTERRUPTED and replaces nothing. */
static void test_changed_repeats_end_it(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    static plink_out_t req;
    req = A;
    to_master(&A, &B);
    req.buf[req.len - 1u] ^= 0x01u; /* the fw string's last byte */
    to_master(&req, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, fail_reason(&B));
    TEST_ASSERT_EQUAL_UINT8(0u, m_info().attempts); /* nothing disclosed yet */

    rig(0x1234u);
    to_compare();
    plink_pk_confirm(&R.pk, R.now, true, &A);
    static plink_out_t confirm;
    confirm = A;
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    confirm.buf[PLINK_HDR_LEN + 5u] ^= 0x10u;
    to_master(&confirm, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, fail_reason(&B));
    TEST_ASSERT_FALSE(plink_pm_take(&R.pm, NULL));

    rig(0x5678u);
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    to_keypad(&B, &C);
    to_master(&C, &A); /* PAIR_REVEAL */
    static plink_out_t reveal;
    reveal = A;
    to_keypad(&A, &B);
    reveal.buf[PLINK_HDR_LEN + 7u] ^= 0x01u;
    keypad_gets(&R.pk, &R.am, reveal.buf, reveal.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_INTERRUPTED, fail_reason(&B));
    TEST_ASSERT_EQUAL_STRING("", k_info().code);
}

/* ---- overlap ------------------------------------------------------------------------------ */

/* LINK-016: two keypads at once - a PAIR_REQ from a second address while one is pairing - end the
 * attempt (its keypad is told OVERLAP) and close the window; the second gets CLOSED next. When no
 * attempt is under way the newcomer is told OVERLAP and the window closes all the same: every
 * attempt of a window comes from one address. */
static void test_overlap_two_keypads(void)
{
    static plink_pair_k_t other;
    plink_pk_init(&other, &R.cx, PLINK_KIND_PANEL, &CAP, "Porch keypad", "1.4.0");
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    to_keypad(&B, &C);
    plink_pk_start(&other, R.now, &R.am, 0u, &A);
    master_gets(&R.ax, A.buf, A.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail_reason(&B));
    TEST_ASSERT_TRUE(plink_addr_equal(&B.to, &R.ak)); /* told to the keypad under way */
    TEST_ASSERT_EQUAL_UINT32(attempt_of(C.buf), attempt_of(B.buf));
    to_keypad(&B, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, k_info().fail);
    const plink_pair_info_t mi = m_info();
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, mi.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, mi.fail);
    TEST_ASSERT_EQUAL_UINT32(0u, mi.left_s);
    R.now += 1000u;
    plink_pk_tick(&other, R.now, &A);
    master_gets(&R.ax, A.buf, A.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&B));
    TEST_ASSERT_TRUE(plink_addr_equal(&B.to, &R.ax));

    /* No attempt under way: a refused request (capacity) holds the window's address. */
    rig(0x2468u);
    static const plink_caps_t small = {4u, 8u, 32u, 16u};
    plink_pk_init(&other, &R.cx, PLINK_KIND_SMALL, &small, "Small", "1.0");
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&other, R.now, &R.am, 0u, &A);
    master_gets(&R.ax, A.buf, A.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CAPACITY, fail_reason(&B));
    R.now += PLINK_PAIR_GAP_MS;
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail_reason(&B));
    TEST_ASSERT_TRUE(plink_addr_equal(&B.to, &R.ak));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, m_info().fail);
    TEST_ASSERT_EQUAL_UINT32(0u, m_info().left_s);
}

/* LINK-016: two masters at once - a PAIR_COMMIT for the keypad's attempt from a second address -
 * end the keypad's attempt (OVERLAP, told to its master), counted only if n_k had gone. */
static void test_overlap_two_masters(void)
{
    static plink_pair_m_t second;
    plink_pm_init(&second, &R.cx);
    plink_pm_identity(&second, 0x2222u, "Garage", "ffeedd", 4u);
    plink_pm_open(&R.pm, R.now);
    plink_pm_open(&second, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    plink_hdr_t h;
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&second, R.now, &R.ak, &h, A.buf, A.len, &C); /* it heard the request too */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&C));
    to_keypad(&B, &A); /* the real one first: n_k goes */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&A));
    keypad_gets(&R.pk, &R.ax, C.buf, C.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail_reason(&B));
    TEST_ASSERT_TRUE(plink_addr_equal(&B.to, &R.am));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(1u, k_info().attempts);
    to_master(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, m_info().fail);

    /* The second master's first: nothing disclosed, not counted. */
    rig(0x1357u);
    plink_pm_init(&second, &R.cx);
    plink_pm_identity(&second, 0x2222u, "Garage", "ffeedd", 4u);
    plink_pm_open(&second, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&second, R.now, &R.ak, &h, A.buf, A.len, &C);
    keypad_gets(&R.pk, &R.ax, C.buf, C.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail_reason(&B));
    TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
}

/* ---- timers ------------------------------------------------------------------------------- */

/* LINK-016: the master's timers. The window closes after 2 min with nobody (TIMEOUT) and counts
 * down meanwhile; a step waits 30 s (PAIR_NONCE after PAIR_COMMIT: TIMEOUT, not counted); the
 * comparison 2 min (TIMEOUT, counted); plink_pm_next names each deadline; an attempt under way
 * outlives the window and the window closes when it ends. */
static void test_master_timeouts(void)
{
    const uint64_t t0 = R.now;
    plink_pm_open(&R.pm, t0);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, m_info().state);
    TEST_ASSERT_EQUAL_UINT32(120u, m_info().left_s);
    TEST_ASSERT_EQUAL_UINT64(t0 + PLINK_PAIR_WINDOW_MS, plink_pm_next(&R.pm));
    R.now = t0 + PLINK_PAIR_WINDOW_MS - 1u;
    plink_pm_tick(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, m_info().state);
    TEST_ASSERT_EQUAL_UINT32(1u, m_info().left_s);
    R.now += 1u;
    plink_pm_tick(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, m_info().fail);
    TEST_ASSERT_EQUAL_UINT32(0u, m_info().left_s);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_pm_next(&R.pm));

    /* A step: no PAIR_NONCE within 30 s of PAIR_COMMIT. */
    rig(0x31u);
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT64(R.now + PLINK_PAIR_STEP_MS, plink_pm_next(&R.pm));
    R.now += PLINK_PAIR_STEP_MS - 1u;
    plink_pm_tick(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    R.now += 1u;
    plink_pm_tick(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, m_info().fail);
    TEST_ASSERT_EQUAL_UINT8(0u, m_info().attempts);
    TEST_ASSERT_NOT_EQUAL(0u, m_info().left_s); /* the window is still open */

    /* The comparison: 2 min from PAIR_REVEAL. */
    rig(0x32u);
    to_compare();
    TEST_ASSERT_EQUAL_UINT64(R.now + PLINK_PAIR_COMPARE_MS, plink_pm_next(&R.pm));
    R.now += PLINK_PAIR_COMPARE_MS;
    plink_pm_tick(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT8(1u, m_info().attempts);
    TEST_ASSERT_EQUAL_STRING("", m_info().code);

    /* An attempt started near the window's end runs on; the window closes behind it. */
    rig(0x33u);
    const uint64_t t1 = R.now;
    plink_pm_open(&R.pm, t1);
    R.now = t1 + 110000u;
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    to_keypad(&B, &C);
    to_master(&C, &A);
    to_keypad(&A, &B);
    R.now = t1 + 125000u;
    plink_pm_tick(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, m_info().state);
    TEST_ASSERT_EQUAL_UINT32(0u, m_info().left_s);
    plink_pk_confirm(&R.pk, R.now, false, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_REFUSED, m_info().fail);
    R.now += PLINK_PAIR_GAP_MS;
    plink_pk_retry(&R.pk, R.now, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&B));
}

/* LINK-016: the keypad resends its last message after 1 s, then 2 s, then every 4 s, the same bytes
 * each time, and gives up after 30 s (TIMEOUT, told to the master; not counted before n_k). Its
 * PAIR_NONCE waits 30 s for PAIR_REVEAL (counted), its comparison 2 min, and its PAIR_CONFIRM is
 * resent the same way until PAIR_DONE or the comparison's end. */
static void test_keypad_timeouts_and_resends(void)
{
    const uint64_t t0 = R.now;
    plink_pk_start(&R.pk, t0, &R.am, 0u, &A);
    static plink_out_t req;
    req = A;
    static const uint32_t at[] = {1000u, 3000u, 7000u, 11000u, 15000u, 19000u, 23000u, 27000u};
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        TEST_ASSERT_EQUAL_UINT64(t0 + at[i], plink_pk_next(&R.pk));
        R.now = t0 + at[i] - 1u;
        plink_pk_tick(&R.pk, R.now, &A);
        TEST_ASSERT_EQUAL_UINT(0u, A.len);
        R.now += 1u;
        plink_pk_tick(&R.pk, R.now, &A);
        expect_frame(req.buf, req.len, &A);
        TEST_ASSERT_TRUE(plink_addr_equal(&A.to, &R.am));
    }
    TEST_ASSERT_EQUAL_UINT64(t0 + PLINK_PAIR_STEP_MS, plink_pk_next(&R.pk));
    TEST_ASSERT_EQUAL_UINT32(3u, k_info().left_s);
    R.now = t0 + PLINK_PAIR_STEP_MS;
    plink_pk_tick(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT32(attempt_of(req.buf), attempt_of(A.buf));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_pk_next(&R.pk));

    /* PAIR_NONCE sent, PAIR_REVEAL never comes. */
    rig(0x41u);
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    to_keypad(&B, &C);
    const uint64_t t1 = R.now;
    R.now = t1 + 1000u;
    plink_pk_tick(&R.pk, R.now, &A);
    expect_frame(C.buf, C.len, &A); /* PAIR_NONCE again */
    R.now = t1 + PLINK_PAIR_STEP_MS;
    plink_pk_tick(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT8(1u, k_info().attempts);

    /* The comparison: nothing resent while the person looks, 2 min at most. */
    rig(0x42u);
    to_compare();
    const uint64_t t2 = R.now;
    TEST_ASSERT_EQUAL_UINT64(t2 + PLINK_PAIR_COMPARE_MS, plink_pk_next(&R.pk));
    TEST_ASSERT_EQUAL_UINT32(120u, k_info().left_s);
    R.now = t2 + 60000u;
    plink_pk_tick(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    plink_pk_confirm(&R.pk, R.now, true, &C);
    R.now += 1000u;
    plink_pk_tick(&R.pk, R.now, &A);
    expect_frame(C.buf, C.len, &A); /* PAIR_CONFIRM again */
    R.now += 2000u;
    plink_pk_tick(&R.pk, R.now, &A);
    expect_frame(C.buf, C.len, &A);
    TEST_ASSERT_EQUAL_UINT64(R.now + PLINK_PAIR_RESEND_MAX_MS, plink_pk_next(&R.pk));
    R.now = t2 + PLINK_PAIR_COMPARE_MS;
    plink_pk_tick(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, fail_reason(&A));
    expect_keypad_wiped(&R.pk);
}

/* LINK-016: PAIR_DONE lost - the keypad's identical PAIR_CONFIRM (resent) gets the very same
 * PAIR_DONE, after the window closed, from the keypad's address only, for 60 s; a changed
 * PAIR_CONFIRM gets nothing; after 60 s nothing either. */
static void test_done_lost_cached(void)
{
    to_compare();
    compare_to_done(4u, 1u); /* B: PAIR_DONE, delivered */
    plink_keypad_rec_t rec;
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));

    rig(0x51u);
    to_compare();
    plink_pk_confirm(&R.pk, R.now, true, &A);
    static plink_out_t confirm, done;
    confirm = A;
    to_master(&A, &B);
    plink_pm_decide(&R.pm, R.now, true, &B);
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, 4u, 1u, &done); /* lost */
    const uint64_t t_done = R.now;
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, m_info().state);
    TEST_ASSERT_EQUAL_UINT32(0u, m_info().left_s); /* the window closed */
    TEST_ASSERT_EQUAL_UINT64(t_done + PLINK_PAIR_DONE_KEEP_MS, plink_pm_next(&R.pm));
    R.now += 1000u;
    plink_pk_tick(&R.pk, R.now, &A); /* the keypad resends PAIR_CONFIRM */
    expect_frame(confirm.buf, confirm.len, &A);
    to_master(&A, &B);
    expect_frame(done.buf, done.len, &B);
    master_gets(&R.ax, confirm.buf, confirm.len, &B); /* not from another address */
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    static plink_out_t changed;
    changed = confirm;
    changed.buf[changed.len - 2u] ^= 0x01u;
    master_gets(&R.ak, changed.buf, changed.len, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    R.now = t_done + PLINK_PAIR_DONE_KEEP_MS - 1u;
    plink_pm_tick(&R.pm, R.now, &B);
    to_master(&confirm, &B);
    expect_frame(done.buf, done.len, &B);
    to_keypad(&B, &C);
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_UINT8(4u, rec.device);
    R.now = t_done + PLINK_PAIR_DONE_KEEP_MS;
    plink_pm_tick(&R.pm, R.now, &B);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_pm_next(&R.pm));
    to_master(&confirm, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
}

/* ---- refusals, cancelling, PAIR_FAIL ------------------------------------------------------ */

/* LINK-016: a keypad whose configuration copy can't hold the master's zones is refused at its
 * request (CAPACITY), before anything is disclosed: not counted on either side, the window stays
 * open, nothing to take. A keypad with exactly the room pairs. The installer's no is REFUSED. */
static void test_capacity_and_refusals(void)
{
    static const plink_caps_t small = {4u, ZONES - 1u, 32u, 16u};
    plink_pk_init(&R.pk, &R.ck, PLINK_KIND_SMALL, &small, "Small keypad", "1.0");
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CAPACITY, fail_reason(&B));
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CAPACITY, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CAPACITY, m_info().fail);
    TEST_ASSERT_EQUAL_UINT8(0u, m_info().attempts);
    TEST_ASSERT_NOT_EQUAL(0u, m_info().left_s);
    TEST_ASSERT_EQUAL_STRING("Small keypad", m_info().peer_name);
    to_master(&A, &B); /* its request again: the same answer */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CAPACITY, fail_reason(&B));

    rig(0x61u);
    static const plink_caps_t exact = {4u, ZONES, 32u, 16u};
    plink_pk_init(&R.pk, &R.ck, PLINK_KIND_SMALL, &exact, "Small keypad", "1.0");
    to_compare();
    plink_pm_decide(&R.pm, R.now, false, &A); /* the installer says no */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_REFUSED, fail_reason(&A));
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_REFUSED, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(1u, m_info().attempts);
}

/* LINK-016: cancelling on either side ends the attempt and tells the other (CANCELLED): the
 * master's cancel closes its window; the keypad's leaves the master's window open, the attempt
 * counted. Both go back to IDLE on the side that cancelled, secrets wiped. */
static void test_cancel_both_sides(void)
{
    to_compare();
    plink_pm_cancel(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CANCELLED, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_IDLE, m_info().state);
    TEST_ASSERT_EQUAL_UINT32(0u, m_info().left_s);
    expect_master_wiped(&R.pm);
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CANCELLED, k_info().fail);
    expect_keypad_wiped(&R.pk);
    R.now += PLINK_PAIR_GAP_MS;
    plink_pk_retry(&R.pk, R.now, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&B));

    rig(0x71u);
    to_compare();
    plink_pk_cancel(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CANCELLED, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_IDLE, k_info().state);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_pk_next(&R.pk));
    expect_keypad_wiped(&R.pk);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, m_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CANCELLED, m_info().fail);
    TEST_ASSERT_EQUAL_UINT8(1u, m_info().attempts);
    TEST_ASSERT_NOT_EQUAL(0u, m_info().left_s);
    expect_master_wiped(&R.pm);
    plink_pk_cancel(&R.pk, R.now, &A); /* nothing under way: nothing sent */
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    plink_pm_cancel(&R.pm, R.now, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_IDLE, m_info().state);
}

/* LINK-016: PAIR_FAIL is honoured only for the attempt under way and from its peer: one for
 * another attempt, or from another address, changes nothing on either side. */
static void test_fail_only_current_from_peer(void)
{
    to_compare();
    const uint32_t attempt = R.pk.attempt;
    plink_pair_fail_t f = {attempt + 1u, PLINK_FAIL_REFUSED};
    static plink_out_t fr;
    size_t n = 0;
    plink_hdr_t h = {PLINK_T_PAIR_FAIL, 0u, NET, PLINK_SENDER_UNPAIRED, 0u, 0u};
    TEST_ASSERT_TRUE(plink_enc_pair_fail(&f, &fr.buf[PLINK_HDR_LEN], 64u, &n));
    TEST_ASSERT_TRUE(plink_frame_plain(&h, &fr.buf[PLINK_HDR_LEN], n, fr.buf, sizeof fr.buf,
                                       &fr.len));
    master_gets(&R.ak, fr.buf, fr.len, &A); /* another attempt */
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, m_info().state);
    f.attempt = attempt;
    TEST_ASSERT_TRUE(plink_enc_pair_fail(&f, &fr.buf[PLINK_HDR_LEN], 64u, &n));
    TEST_ASSERT_TRUE(plink_frame_plain(&h, &fr.buf[PLINK_HDR_LEN], n, fr.buf, sizeof fr.buf,
                                       &fr.len));
    master_gets(&R.ax, fr.buf, fr.len, &A); /* another address */
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, m_info().state);

    /* The keypad side: from the master's own number but another address, or another attempt. */
    const plink_hdr_t hm = {PLINK_T_PAIR_FAIL, PLINK_FLAG_FROM_MASTER, NET, 0u, 0u, 0u};
    TEST_ASSERT_TRUE(plink_frame_plain(&hm, &fr.buf[PLINK_HDR_LEN], n, fr.buf, sizeof fr.buf,
                                       &fr.len));
    keypad_gets(&R.pk, &R.ax, fr.buf, fr.len, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);
    f.attempt = attempt - 1u;
    TEST_ASSERT_TRUE(plink_enc_pair_fail(&f, &fr.buf[PLINK_HDR_LEN], 64u, &n));
    TEST_ASSERT_TRUE(plink_frame_plain(&hm, &fr.buf[PLINK_HDR_LEN], n, fr.buf, sizeof fr.buf,
                                       &fr.len));
    keypad_gets(&R.pk, &R.am, fr.buf, fr.len, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);
    TEST_ASSERT_EQUAL_STRING(m_info().code, k_info().code);

    /* The right one, from the master: honoured. */
    f.attempt = attempt;
    TEST_ASSERT_TRUE(plink_enc_pair_fail(&f, &fr.buf[PLINK_HDR_LEN], 64u, &n));
    TEST_ASSERT_TRUE(plink_frame_plain(&hm, &fr.buf[PLINK_HDR_LEN], n, fr.buf, sizeof fr.buf,
                                       &fr.len));
    keypad_gets(&R.pk, &R.am, fr.buf, fr.len, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_REFUSED, k_info().fail);
    TEST_ASSERT_EQUAL_UINT(0u, A.len); /* a PAIR_FAIL is never answered */
}

/* LINK-016: pairing traffic is taken only when it can be: the master answers a request with
 * CLOSED before its window opens and ignores one from a numbered sender or for another network;
 * a keypad no person is pairing ignores everything; a master without a network id opens nothing;
 * a frame of a step not reached yet (PAIR_CONFIRM before the code, PAIR_DONE before the person
 * said yes) is dropped. */
static void test_taken_only_when_it_can_be(void)
{
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&B));
    plink_pm_open(&R.pm, R.now);
    static plink_out_t f;
    f = A;
    f.buf[7] = 3u; /* sender 3: a paired keypad's number */
    f.buf[8] = 0u;
    master_gets(&R.ak, f.buf, f.len, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    f = A;
    f.buf[5] = 0x11u; /* net id 0x1111: another master's */
    f.buf[6] = 0x11u;
    master_gets(&R.ak, f.buf, f.len, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, m_info().state);
    to_master(&A, &B); /* the same with net id 0: taken */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));

    static plink_pair_k_t idle;
    plink_pk_init(&idle, &R.cx, PLINK_KIND_PANEL, &CAP, "Idle", "1");
    keypad_gets(&idle, &R.am, B.buf, B.len, &C);
    TEST_ASSERT_EQUAL_UINT(0u, C.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_IDLE, idle.state);

    to_keypad(&B, &C); /* PAIR_NONCE */
    plink_pair_confirm_t cf = {R.pk.attempt, {0}};
    size_t n = 0;
    const plink_hdr_t hc = {PLINK_T_PAIR_CONFIRM, 0u, NET, PLINK_SENDER_UNPAIRED, 0u, 0u};
    TEST_ASSERT_TRUE(plink_enc_pair_confirm(&cf, &f.buf[PLINK_HDR_LEN], 64u, &n));
    TEST_ASSERT_TRUE(plink_frame_plain(&hc, &f.buf[PLINK_HDR_LEN], n, f.buf, sizeof f.buf, &f.len));
    master_gets(&R.ak, f.buf, f.len, &A); /* before PAIR_NONCE was taken */
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_WORKING, m_info().state);
    to_master(&C, &A); /* PAIR_REVEAL */
    to_keypad(&A, &B);
    plink_pair_done_t d = {R.pk.attempt, 2u, 1u, {0}};
    const plink_hdr_t hd = {PLINK_T_PAIR_DONE, PLINK_FLAG_FROM_MASTER, NET, 0u, 0u, 0u};
    TEST_ASSERT_TRUE(plink_enc_pair_done(&d, &f.buf[PLINK_HDR_LEN], 64u, &n));
    TEST_ASSERT_TRUE(plink_frame_plain(&hd, &f.buf[PLINK_HDR_LEN], n, f.buf, sizeof f.buf, &f.len));
    keypad_gets(&R.pk, &R.am, f.buf, f.len, &A); /* before the person said yes */
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);

    static plink_pair_m_t nonet;
    plink_pm_init(&nonet, &R.cm);
    plink_pm_open(&nonet, R.now);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_IDLE, nonet.state);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_pm_next(&nonet));
}

/* ---- secrets ------------------------------------------------------------------------------ */

/* LINK-016 (nothing kept beyond its use): once paired, and once failed, neither side holds a
 * private key, a shared secret, a nonce, K_pair, K_conf or the code any more - the keypad's
 * record too once taken; the master keeps only what answers a lost PAIR_DONE. */
static void test_secrets_wiped(void)
{
    to_compare();
    TEST_ASSERT_FALSE(all_zero(R.pm.k_conf, 32u));
    TEST_ASSERT_FALSE(all_zero(R.pk.k_conf, 32u));
    TEST_ASSERT_FALSE(all_zero(R.pm.sk, 32u));
    TEST_ASSERT_FALSE(all_zero(R.pk.dh, 32u));
    compare_to_done(6u, 1u);
    expect_master_wiped(&R.pm);
    TEST_ASSERT_TRUE(R.pk.have_result);
    plink_keypad_rec_t rec;
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    expect_keypad_wiped(&R.pk);
    TEST_ASSERT_TRUE(all_zero(R.pm.req_raw, sizeof R.pm.req_raw));
    TEST_ASSERT_NOT_EQUAL(0u, R.pm.done_len);

    rig(0x81u);
    to_compare();
    plink_pk_confirm(&R.pk, R.now, false, &A);
    expect_keypad_wiped(&R.pk);
    to_master(&A, &B);
    expect_master_wiped(&R.pm);
}

/* LINK-016: a failing port leaves nothing half made: the master's key pair failing answers BUSY
 * (not counted, nothing kept) and the next request pairs; the keypad's generator failing sends
 * nothing and fails the attempt. */
static void test_port_failures(void)
{
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    plink_test_port_fail_at(&R.tpm, 0u); /* the master's next primitive call */
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_BUSY, fail_reason(&B));
    TEST_ASSERT_EQUAL_UINT8(0u, m_info().attempts);
    expect_master_wiped(&R.pm);
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_BUSY, k_info().fail);
    R.now += PLINK_PAIR_GAP_MS;
    plink_pk_retry(&R.pk, R.now, &A);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));

    rig(0x91u);
    plink_test_port_fail_at(&R.tpk, 1u); /* its private key, after the attempt id */
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    expect_keypad_wiped(&R.pk);
}

/* ---- asked by broadcast (LINK-021) ---------------------------------------------------------- */

static const uint8_t IP_BCAST[4] = {255u, 255u, 255u, 255u};

/* LINK-021: which addresses are broadcasts - 255.255.255.255 and a /24's x.y.z.255, on UDP only. */
static void test_broadcast_addresses(void)
{
    static const uint8_t subnet[4] = {192u, 0u, 2u, 255u};
    const plink_addr_t all = plink_addr_udp(IP_BCAST, PLINK_UDP_PORT);
    const plink_addr_t sub = plink_addr_udp(subnet, PLINK_UDP_PORT);
    TEST_ASSERT_TRUE(plink_addr_is_broadcast(&all));
    TEST_ASSERT_TRUE(plink_addr_is_broadcast(&sub));
    TEST_ASSERT_FALSE(plink_addr_is_broadcast(&R.am));
    TEST_ASSERT_FALSE(plink_addr_is_broadcast(NULL));
    plink_addr_t mac;
    memset(&mac, 0, sizeof mac);
    mac.tp = PLINK_TP_ESPNOW;
    mac.len = 6u;
    memset(mac.a, 0xFF, 6u);
    TEST_ASSERT_FALSE(plink_addr_is_broadcast(&mac));
}

/* LINK-021, LINK-016: a keypad that asks by broadcast (mDNS found nothing) pairs with the master
 * that answers. Its request, and the request's resends, go by broadcast; the attempt's first
 * PAIR_COMMIT fixes its master before n_k goes, and from then on everything goes to that address
 * and is taken only from it (a PAIR_REVEAL from elsewhere changes nothing). Both screens show the
 * same code, the keypad the master's own address; the record is the master's. */
static void test_broadcast_pairing_completes(void)
{
    const plink_addr_t bc = plink_addr_udp(IP_BCAST, PLINK_UDP_PORT);
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &bc, 0u, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REQ, type_of(&A));
    TEST_ASSERT_TRUE(plink_addr_equal(&A.to, &bc));
    R.now += PLINK_PAIR_RESEND_MS;
    plink_pk_tick(&R.pk, R.now, &C); /* no answer yet: asked again, by broadcast */
    expect_frame(A.buf, A.len, &C);
    TEST_ASSERT_TRUE(plink_addr_equal(&C.to, &bc));
    master_gets(&R.ak, A.buf, A.len, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&C));
    TEST_ASSERT_TRUE(plink_addr_equal(&C.to, &R.am)); /* n_k to the master that answered */
    plink_pair_info_t ki = k_info();
    TEST_ASSERT_TRUE(plink_addr_equal(&ki.peer, &R.am));
    TEST_ASSERT_EQUAL_UINT8(1u, k_info().attempts);
    R.now += PLINK_PAIR_RESEND_MS;
    plink_pk_tick(&R.pk, R.now, &A); /* its resend goes there too */
    expect_frame(C.buf, C.len, &A);
    TEST_ASSERT_TRUE(plink_addr_equal(&A.to, &R.am));
    to_master(&C, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REVEAL, type_of(&A));
    keypad_gets(&R.pk, &R.ax, A.buf, A.len, &B); /* the same REVEAL from another address */
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_WORKING, k_info().state);
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);
    TEST_ASSERT_EQUAL_STRING(m_info().code, k_info().code);
    compare_to_done(2u, 1u);
    plink_keypad_rec_t rec;
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_UINT8(2u, rec.device);
    TEST_ASSERT_EQUAL_HEX16(NET, rec.net_id);
    TEST_ASSERT_EQUAL_STRING("Harbor House", rec.master_name);
    ki = k_info();
    TEST_ASSERT_TRUE(plink_addr_equal(&ki.peer, &R.am));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, m_info().state);
}

/* LINK-021, LINK-016: two masters answer a broadcast request. The first PAIR_COMMIT fixes the
 * keypad's master and n_k goes to it alone; the second ends the attempt (OVERLAP), told by
 * broadcast so that both masters end theirs. Nothing was disclosed to the second: it never got
 * n_k, never revealed n_m, and counts nothing. A retry asks by broadcast again. */
static void test_broadcast_two_masters_overlap(void)
{
    static plink_pair_m_t second;
    const plink_addr_t bc = plink_addr_udp(IP_BCAST, PLINK_UDP_PORT);
    plink_pm_init(&second, &R.cx);
    plink_pm_identity(&second, 0x2222u, "Garage", "ffeedd", 4u);
    plink_pm_open(&R.pm, R.now);
    plink_pm_open(&second, R.now);
    plink_pk_start(&R.pk, R.now, &bc, 0u, &A);
    static plink_out_t commit2;
    master_gets(&R.ak, A.buf, A.len, &B); /* both hear the request */
    plink_hdr_t h;
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&second, R.now, &R.ak, &h, A.buf, A.len, &commit2);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&commit2));
    to_keypad(&B, &C); /* the first: n_k goes to it */
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&C));
    TEST_ASSERT_TRUE(plink_addr_equal(&C.to, &R.am));
    keypad_gets(&R.pk, &R.ax, commit2.buf, commit2.len, &A); /* the second */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail_reason(&A));
    TEST_ASSERT_TRUE(plink_addr_equal(&A.to, &bc));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(1u, k_info().attempts); /* n_k went to the first */
    expect_keypad_wiped(&R.pk);
    /* The PAIR_FAIL reaches both. */
    master_gets(&R.ak, A.buf, A.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, m_info().fail);
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&second, R.now, &R.ak, &h, A.buf, A.len, &B);
    plink_pair_info_t si;
    plink_pm_info(&second, R.now, &si);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, si.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, si.fail);
    TEST_ASSERT_EQUAL_UINT8(0u, si.attempts);
    TEST_ASSERT_FALSE(second.revealed);
    TEST_ASSERT_TRUE(all_zero(second.nonce_raw, sizeof second.nonce_raw));
    expect_master_wiped(&second);
    /* The person tries again: by broadcast. */
    R.now += PLINK_PAIR_GAP_MS;
    plink_pk_retry(&R.pk, R.now, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REQ, type_of(&A));
    TEST_ASSERT_TRUE(plink_addr_equal(&A.to, &bc));

    /* The second's COMMIT first: it is the one, and the first's ends the attempt before the
     * PAIR_NONCE reaches anyone else. */
    rig(0x2B2Bu);
    plink_pm_init(&second, &R.cx);
    plink_pm_identity(&second, 0x2222u, "Garage", "ffeedd", 4u);
    plink_pm_open(&R.pm, R.now);
    plink_pm_open(&second, R.now);
    plink_pk_start(&R.pk, R.now, &bc, 0u, &A);
    master_gets(&R.ak, A.buf, A.len, &B);
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&second, R.now, &R.ak, &h, A.buf, A.len, &commit2);
    keypad_gets(&R.pk, &R.ax, commit2.buf, commit2.len, &C);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&C));
    TEST_ASSERT_TRUE(plink_addr_equal(&C.to, &R.ax));
    to_keypad(&B, &A);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail_reason(&A));
    TEST_ASSERT_EQUAL_UINT8(0u, m_info().attempts); /* the first never got n_k */
}

/* LINK-021: asked by broadcast, a refusal that comes before any PAIR_COMMIT (a master whose
 * window is closed) doesn't end the attempt - another master may still answer, and then pairing
 * goes on with it; with nobody else, the attempt times out naming that refusal. */
static void test_broadcast_refusal_before_commit(void)
{
    static plink_pair_m_t closed;
    const plink_addr_t bc = plink_addr_udp(IP_BCAST, PLINK_UDP_PORT);
    plink_pm_init(&closed, &R.cx);
    plink_pm_identity(&closed, 0x2222u, "Garage", "ffeedd", 4u);
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &bc, 0u, &A);
    plink_hdr_t h;
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&closed, R.now, &R.ak, &h, A.buf, A.len, &C);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, fail_reason(&C));
    keypad_gets(&R.pk, &R.ax, C.buf, C.len, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, k_info().state);
    master_gets(&R.ak, A.buf, A.len, &B);
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&C));
    TEST_ASSERT_TRUE(plink_addr_equal(&C.to, &R.am));

    /* Only the closed one: TIMEOUT at the step's end, shown as CLOSED, nothing counted. */
    rig(0x3C3Cu);
    plink_pm_init(&closed, &R.cx);
    plink_pm_identity(&closed, 0x2222u, "Garage", "ffeedd", 4u);
    const uint64_t t0 = R.now;
    plink_pk_start(&R.pk, R.now, &bc, 0u, &A);
    TEST_ASSERT_TRUE(plink_hdr_read(A.buf, A.len, &h));
    plink_pm_input(&closed, R.now, &R.ak, &h, A.buf, A.len, &C);
    keypad_gets(&R.pk, &R.ax, C.buf, C.len, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, k_info().state);
    R.now = t0 + PLINK_PAIR_STEP_MS;
    plink_pk_tick(&R.pk, R.now, &B);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_TIMEOUT, fail_reason(&B));
    TEST_ASSERT_TRUE(plink_addr_equal(&B.to, &bc));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_CLOSED, k_info().fail);
    TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
}

/* ---- payloads of a newer version (LINK-017) ------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The longest name and firmware version on the wire (PLINK_NAME_MAX, PLINK_FW_MAX). */
#define LONG_NAME "Keypad in the hall, by the door to the garage 1"
#define LONG_FW   "2.0.0-rc.1+build.20261015.12345"

/* A plain PAIR_* frame with payload p (n bytes) into o, to `to`. */
static void plain(plink_out_t *o, uint8_t type, bool from_master, const uint8_t *p, size_t n,
                  const plink_addr_t *to)
{
    const plink_hdr_t h = {type, from_master ? PLINK_FLAG_FROM_MASTER : 0u,
                           (uint16_t)(type == PLINK_T_PAIR_REQ ? 0u : NET),
                           from_master ? PLINK_SENDER_MASTER : PLINK_SENDER_UNPAIRED, 0u, 0u};
    TEST_ASSERT_TRUE(n <= PLINK_PAIR_PAYLOAD_MAX);
    memcpy(&o->buf[PLINK_HDR_LEN], p, n);
    TEST_ASSERT_TRUE(
        plink_frame_plain(&h, &o->buf[PLINK_HDR_LEN], n, o->buf, sizeof o->buf, &o->len));
    o->to = *to;
}

/* LINK-017, LINK-016: a newer keypad's longer payloads are taken whole and hashed into the
 * transcript as they travelled - a PAIR_REQ whose capacities section carries 40 bytes more and
 * with 40 more after fw (more than v1's 160 bytes of room), a PAIR_NONCE with 16 more, a
 * PAIR_CONFIRM with 16 more before its MAC: pairing completes, the master holding the K_pair the
 * keypad computed, and a repeated PAIR_CONFIRM gets the same PAIR_DONE. */
static void test_newer_keypad_payloads_are_taken(void)
{
    static uint8_t req[PLINK_PAIR_PAYLOAD_MAX], commit[PLINK_PAIR_PAYLOAD_MAX];
    uint8_t sk[32], pk[32], v1[200], n_k[16], np[36], cp[36], dh[32], k_pair[32], k_conf[32];
    plink_pm_open(&R.pm, R.now);
    TEST_ASSERT_TRUE(plink_x25519_keypair(&R.cx, sk, pk));
    plink_pair_req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.attempt = 0x0BADCAFEu;
    rq.proto = 2u; /* a newer version: the session takes the lower */
    rq.caps = 0x0003u;
    rq.kind = PLINK_KIND_PANEL;
    rq.cap = CAP;
    memcpy(rq.pk, pk, sizeof pk);
    strcpy(rq.name, LONG_NAME);
    strcpy(rq.fw, LONG_FW);
    size_t v1n = 0, rn = 0;
    TEST_ASSERT_TRUE(plink_enc_pair_req(&rq, v1, sizeof v1, &v1n));
    /* attempt, proto, caps, kind (8 bytes), then the capacities section: length 5 and 5 bytes */
    TEST_ASSERT_EQUAL_UINT8(5u, v1[8]);
    memcpy(req, v1, 14u);
    req[8] = 45u;
    fill(&req[14], 40u, 3u, 0x11u);
    memcpy(&req[54], &v1[14], v1n - 14u);
    rn = 54u + (v1n - 14u);
    fill(&req[rn], 40u, 5u, 0x22u);
    rn += 40u;
    TEST_ASSERT_TRUE(rn > 160u);
    plain(&A, PLINK_T_PAIR_REQ, false, req, rn, &R.am);
    to_master(&A, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));
    TEST_ASSERT_EQUAL_UINT16(CAP.zones, R.pm.req.cap.zones);
    TEST_ASSERT_EQUAL_STRING(LONG_FW, R.pm.req.fw);
    const size_t cn = B.len - PLINK_HDR_LEN;
    memcpy(commit, &B.buf[PLINK_HDR_LEN], cn);
    plink_pair_commit_t cm;
    TEST_ASSERT_TRUE(plink_dec_pair_commit(commit, cn, &cm));
    /* PAIR_NONCE: attempt, n_k and 16 bytes more. */
    fill(n_k, sizeof n_k, 7u, 0x33u);
    put32(np, rq.attempt);
    memcpy(&np[4], n_k, sizeof n_k);
    fill(&np[20], 16u, 9u, 0x44u);
    plain(&C, PLINK_T_PAIR_NONCE, false, np, sizeof np, &R.am);
    to_master(&C, &A);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_REVEAL, type_of(&A));
    plink_pair_nonce_t rv;
    TEST_ASSERT_TRUE(plink_dec_pair_nonce(&A.buf[PLINK_HDR_LEN], A.len - PLINK_HDR_LEN, &rv));
    uint8_t c2[32];
    plink_pair_commitment(cm.pk, pk, rv.n, c2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cm.commit, c2, 32u);
    TEST_ASSERT_EQUAL_UINT8(1u, m_info().attempts);
    /* The keys over the payloads as they travelled; the same code. */
    TEST_ASSERT_TRUE(plink_x25519_shared(&R.cx, sk, cm.pk, dh));
    TEST_ASSERT_TRUE(
        plink_pair_keys(dh, req, rn, commit, cn, NET, n_k, rv.n, k_pair, k_conf));
    char code[PLINK_CODE_LEN];
    plink_pair_code(pk, cm.pk, n_k, rv.n, code);
    TEST_ASSERT_EQUAL_STRING(code, m_info().code);
    /* PAIR_CONFIRM: attempt, 16 bytes more, mac_k. */
    put32(cp, rq.attempt);
    fill(&cp[4], 16u, 11u, 0x55u);
    memset(&cp[20], 0, 16u);
    plain(&C, PLINK_T_PAIR_CONFIRM, false, cp, sizeof cp, &R.am);
    plink_mac_fill(k_conf, C.buf, C.len);
    to_master(&C, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    plink_pm_decide(&R.pm, R.now, true, &A);
    plink_pair_new_t nw;
    TEST_ASSERT_TRUE(plink_pm_take(&R.pm, &nw));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_pair, nw.k_pair, 32u);
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, 2u, 1u, &B);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_DONE, type_of(&B));
    TEST_ASSERT_TRUE(plink_mac_check(k_conf, B.buf, B.len));
    to_master(&C, &A); /* PAIR_DONE lost: the same PAIR_CONFIRM again */
    expect_frame(B.buf, B.len, &A);
}

/* LINK-017, LINK-016: the keypad takes a newer master's longer payloads the same way - a
 * PAIR_COMMIT with 40 bytes after the panel id, a PAIR_REVEAL with 16 more, a PAIR_DONE with 16
 * more before its MAC - and pairs, holding the K_pair the master computed. */
static void test_newer_master_payloads_are_taken(void)
{
    static uint8_t req[PLINK_PAIR_PAYLOAD_MAX], commit[PLINK_PAIR_PAYLOAD_MAX];
    uint8_t sk[32], pk_m[32], n_m[16], v1[200], rp[36], dp[38], dh[32], k_pair[32], k_conf[32];
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
    const size_t rn = A.len - PLINK_HDR_LEN;
    memcpy(req, &A.buf[PLINK_HDR_LEN], rn);
    plink_pair_req_t rq;
    TEST_ASSERT_TRUE(plink_dec_pair_req(req, rn, &rq));
    TEST_ASSERT_TRUE(plink_x25519_keypair(&R.cx, sk, pk_m));
    fill(n_m, sizeof n_m, 13u, 0x66u);
    plink_pair_commit_t cm;
    memset(&cm, 0, sizeof cm);
    cm.attempt = rq.attempt;
    cm.proto = 2u;
    cm.caps = 0x0001u;
    memcpy(cm.pk, pk_m, sizeof pk_m);
    plink_pair_commitment(pk_m, rq.pk, n_m, cm.commit);
    strcpy(cm.name, LONG_NAME);
    strcpy(cm.panel_id, "a1b2c3");
    size_t cn = 0;
    TEST_ASSERT_TRUE(plink_enc_pair_commit(&cm, v1, sizeof v1, &cn));
    memcpy(commit, v1, cn);
    fill(&commit[cn], 40u, 7u, 0x77u);
    cn += 40u;
    TEST_ASSERT_TRUE(cn > 160u);
    plain(&B, PLINK_T_PAIR_COMMIT, true, commit, cn, &R.ak);
    to_keypad(&B, &C);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_NONCE, type_of(&C));
    plink_pair_nonce_t nk;
    TEST_ASSERT_TRUE(plink_dec_pair_nonce(&C.buf[PLINK_HDR_LEN], C.len - PLINK_HDR_LEN, &nk));
    /* PAIR_REVEAL: attempt, n_m and 16 bytes more. */
    put32(rp, rq.attempt);
    memcpy(&rp[4], n_m, sizeof n_m);
    fill(&rp[20], 16u, 3u, 0x88u);
    plain(&A, PLINK_T_PAIR_REVEAL, true, rp, sizeof rp, &R.ak);
    to_keypad(&A, &B);
    TEST_ASSERT_EQUAL_UINT(0u, B.len);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, k_info().state);
    TEST_ASSERT_TRUE(plink_x25519_shared(&R.cx, sk, rq.pk, dh));
    TEST_ASSERT_TRUE(plink_pair_keys(dh, req, rn, commit, cn, NET, nk.n, n_m, k_pair, k_conf));
    char code[PLINK_CODE_LEN];
    plink_pair_code(rq.pk, pk_m, nk.n, n_m, code);
    TEST_ASSERT_EQUAL_STRING(code, k_info().code);
    plink_pk_confirm(&R.pk, R.now, true, &C);
    TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_CONFIRM, type_of(&C));
    TEST_ASSERT_TRUE(plink_mac_check(k_conf, C.buf, C.len));
    /* PAIR_DONE: attempt, device 4, home 1, 16 bytes more, mac_m. */
    put32(dp, rq.attempt);
    dp[4] = 4u;
    dp[5] = 1u;
    fill(&dp[6], 16u, 5u, 0x99u);
    memset(&dp[22], 0, 16u);
    plain(&B, PLINK_T_PAIR_DONE, true, dp, sizeof dp, &R.ak);
    plink_mac_fill(k_conf, B.buf, B.len);
    to_keypad(&B, &A);
    TEST_ASSERT_EQUAL_UINT(0u, A.len);
    plink_keypad_rec_t rec;
    TEST_ASSERT_TRUE(plink_pk_take(&R.pk, &rec));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_pair, rec.k_pair, 32u);
    TEST_ASSERT_EQUAL_UINT8(4u, rec.device);
    TEST_ASSERT_EQUAL_STRING(LONG_NAME, rec.master_name);
}

/* ---- refusals before anything is disclosed (LINK-016, LINK-017) ----------------------------- */

/* A point of order 8 on Curve25519: X25519 of any clamped scalar with it is all zero. */
static const uint8_t ORDER8[32] = {
    0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
    0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00,
};

/* LINK-017, LINK-016: a PAIR_REQ with protocol version 0 is refused (VERSION), and one whose
 * pk_k is of small order - zero, or a point of order 8 - is refused (MISMATCH), both before any
 * PAIR_COMMIT goes: neither side counts an attempt, the master's window stays open, and nothing
 * of the attempt is kept. The keypad refuses a PAIR_COMMIT the same way - protocol 0 (VERSION),
 * pk_m zero or of order 8 (MISMATCH) - before its PAIR_NONCE goes, telling its master. */
static void test_version_0_and_small_order_keys_refused(void)
{
    for (unsigned c = 0; c < 3u; c++) {
        const uint8_t want = c == 0u ? (uint8_t)PLINK_FAIL_VERSION : (uint8_t)PLINK_FAIL_MISMATCH;
        rig(0xA0u + c);
        plink_pm_open(&R.pm, R.now);
        plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
        if (c == 0u) {
            A.buf[PLINK_HDR_LEN + 4u] = 0u; /* proto */
        } else if (c == 1u) {
            memset(&A.buf[31], 0, 32u); /* pk_k */
        } else {
            memcpy(&A.buf[31], ORDER8, 32u);
        }
        to_master(&A, &B);
        TEST_ASSERT_EQUAL_UINT8(want, fail_reason(&B));
        const plink_pair_info_t mi = m_info();
        TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, mi.state);
        TEST_ASSERT_EQUAL_UINT8(want, mi.fail);
        TEST_ASSERT_EQUAL_UINT8(0u, mi.attempts);
        TEST_ASSERT_NOT_EQUAL(0u, mi.left_s);
        expect_master_wiped(&R.pm);
        TEST_ASSERT_EQUAL_size_t(0u, R.pm.req_len);
        TEST_ASSERT_EQUAL_size_t(0u, R.pm.commit_len);
        to_keypad(&B, &C);
        TEST_ASSERT_EQUAL_UINT8(want, k_info().fail);
        TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
    }
    for (unsigned c = 0; c < 3u; c++) {
        const uint8_t want = c == 0u ? (uint8_t)PLINK_FAIL_VERSION : (uint8_t)PLINK_FAIL_MISMATCH;
        rig(0xB0u + c);
        plink_pm_open(&R.pm, R.now);
        plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
        to_master(&A, &B);
        TEST_ASSERT_EQUAL_HEX8(PLINK_T_PAIR_COMMIT, type_of(&B));
        /* attempt u32, proto u8, caps u16, then pk_m */
        if (c == 0u) {
            B.buf[PLINK_HDR_LEN + 4u] = 0u;
        } else if (c == 1u) {
            memset(&B.buf[PLINK_HDR_LEN + 7u], 0, 32u);
        } else {
            memcpy(&B.buf[PLINK_HDR_LEN + 7u], ORDER8, 32u);
        }
        to_keypad(&B, &C);
        TEST_ASSERT_EQUAL_UINT8(want, fail_reason(&C)); /* no PAIR_NONCE */
        TEST_ASSERT_TRUE(plink_addr_equal(&C.to, &R.am));
        TEST_ASSERT_EQUAL_UINT8(PLINK_PS_FAILED, k_info().state);
        TEST_ASSERT_EQUAL_UINT8(want, k_info().fail);
        TEST_ASSERT_EQUAL_UINT8(0u, k_info().attempts);
        expect_keypad_wiped(&R.pk);
        to_master(&C, &A);
        TEST_ASSERT_EQUAL_UINT8(want, m_info().fail);
        TEST_ASSERT_EQUAL_UINT8(0u, m_info().attempts);
        expect_master_wiped(&R.pm);
    }
}

/* ---- random bytes into both sides ------------------------------------------------------------ */

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

/* One honest pairing's six frames (PAIR_REQ .. PAIR_DONE) with rig(seed). */
static void honest_frames(uint64_t seed, plink_out_t t[6])
{
    rig(seed);
    plink_pm_open(&R.pm, R.now);
    plink_pk_start(&R.pk, R.now, &R.am, 0u, &t[0]);
    to_master(&t[0], &t[1]);
    to_keypad(&t[1], &t[2]);
    to_master(&t[2], &t[3]);
    to_keypad(&t[3], &A);
    plink_pk_confirm(&R.pk, R.now, true, &t[4]);
    to_master(&t[4], &A);
    plink_pm_decide(&R.pm, R.now, true, &A);
    plink_pm_stored(&R.pm, R.now, true, PLINK_FAIL_NONE, 2u, 1u, &t[5]);
}

/* An answer or a tick's frame: nothing, or a frame the other side's plink_hdr_read takes. */
static void expect_sane(const plink_out_t *o)
{
    if (o->len > 0u) {
        plink_hdr_t h;
        TEST_ASSERT_TRUE(o->len <= PLINK_FRAME_MAX);
        TEST_ASSERT_TRUE(plink_hdr_read(o->buf, o->len, &h));
    }
    TEST_ASSERT_TRUE(m_info().attempts <= PLINK_PAIR_ATTEMPTS);
    TEST_ASSERT_TRUE(k_info().attempts <= PLINK_PAIR_ATTEMPTS);
}

/* LINK-016, LINK-017 (docs/features/11 "Test and acceptance": random bytes into every parser):
 * 4000 PAIR_* frames - copies of the attempt's own honest frames (the same seed makes the same
 * attempt) with random edits in their type, net id and payload, and random payloads - go into
 * whichever side their FROM_MASTER flag names, at every step of an attempt (window open, request
 * taken, codes shown, both said yes), from the peer's address or another, with time jumps and
 * ticks between; each frame ends at the end of its buffer. Neither side crashes or reads outside
 * it, whatever either answers is a frame plink_hdr_read takes, and no side counts more than 3
 * attempts. */
static void test_fuzz_pairing_input(void)
{
    static plink_out_t t[6];
    static uint8_t arena[PLINK_FRAME_MAX];
    uint8_t work[PLINK_FRAME_MAX];
    RNG = 0xFA12EDu;
    unsigned answered = 0u;
    for (uint32_t block = 0; block < 80u; block++) {
        const uint64_t seed = 0xF100u + block;
        honest_frames(seed, t);
        rig(seed);
        switch (block % 4u) {
        case 0:
            plink_pm_open(&R.pm, R.now);
            plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
            break;
        case 1:
            plink_pm_open(&R.pm, R.now);
            plink_pk_start(&R.pk, R.now, &R.am, 0u, &A);
            to_master(&A, &B);
            break;
        case 2: to_compare(); break;
        default:
            to_compare();
            plink_pk_confirm(&R.pk, R.now, true, &A);
            to_master(&A, &B);
            plink_pm_decide(&R.pm, R.now, true, &C);
            break;
        }
        for (uint32_t i = 0; i < 50u; i++) {
            const plink_out_t *src = &t[rnd_below(6u)];
            size_t len = src->len;
            memcpy(work, src->buf, len);
            if (rnd_below(10u) == 0u) {
                len = PLINK_HDR_LEN + rnd_below(120u);
                for (size_t b = PLINK_HDR_LEN; b < len; b++) {
                    work[b] = (uint8_t)rnd();
                }
            }
            if (rnd_below(8u) == 0u) {
                work[3] = (uint8_t)(PLINK_T_PAIR_REQ + rnd_below(7u)); /* another step's type */
            }
            if (rnd_below(16u) == 0u) {
                work[5] = (uint8_t)(work[5] ^ (1u << rnd_below(8u))); /* another net id */
            }
            const size_t edits = rnd_below(4u);
            for (size_t e = 0; e < edits && len > PLINK_HDR_LEN; e++) {
                const size_t at = PLINK_HDR_LEN + rnd_below(len - PLINK_HDR_LEN);
                switch (rnd_below(4u)) {
                case 0: work[at] = (uint8_t)(work[at] ^ (1u << rnd_below(8u))); break;
                case 1: work[at] = (uint8_t)rnd(); break;
                case 2: len = at; break;
                default:
                    for (size_t b = rnd_below(40u); b > 0u && len < sizeof work; b--) {
                        work[len++] = (uint8_t)rnd();
                    }
                    break;
                }
            }
            uint8_t *p = arena + sizeof arena - len;
            memcpy(p, work, len);
            plink_hdr_t h;
            if (!plink_hdr_read(p, len, &h)) {
                continue;
            }
            const bool other = rnd_below(4u) == 0u;
            if ((h.flags & PLINK_FLAG_FROM_MASTER) != 0u) {
                plink_pk_input(&R.pk, R.now, other ? &R.ax : &R.am, &h, p, len, &B);
            } else {
                plink_pm_input(&R.pm, R.now, other ? &R.ax : &R.ak, &h, p, len, &B);
            }
            answered += B.len > 0u;
            expect_sane(&B);
            if (rnd_below(10u) == 0u) {
                R.now += rnd_below(40000u);
                plink_pm_tick(&R.pm, R.now, &B);
                expect_sane(&B);
                plink_pk_tick(&R.pk, R.now, &B);
                expect_sane(&B);
            }
        }
    }
    TEST_ASSERT_TRUE(answered > 200u); /* the edits reached the state machines */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vectors);
    RUN_TEST(test_code_leading_zeros);
    RUN_TEST(test_keys_bind_transcript);
    RUN_TEST(test_mac_fill_check);
    RUN_TEST(test_golden_pairing);
    RUN_TEST(test_honest_pairing_both_orders);
    RUN_TEST(test_mitm_substitutes_pk_k);
    RUN_TEST(test_mitm_full_codes_differ);
    RUN_TEST(test_commit_after_nonce);
    RUN_TEST(test_nonce_after_reveal);
    RUN_TEST(test_forged_confirmations);
    RUN_TEST(test_attacker_aborts_after_reveal);
    RUN_TEST(test_keypad_attempt_limit);
    RUN_TEST(test_repeats_same_bytes);
    RUN_TEST(test_changed_repeats_end_it);
    RUN_TEST(test_overlap_two_keypads);
    RUN_TEST(test_overlap_two_masters);
    RUN_TEST(test_master_timeouts);
    RUN_TEST(test_keypad_timeouts_and_resends);
    RUN_TEST(test_done_lost_cached);
    RUN_TEST(test_capacity_and_refusals);
    RUN_TEST(test_cancel_both_sides);
    RUN_TEST(test_fail_only_current_from_peer);
    RUN_TEST(test_taken_only_when_it_can_be);
    RUN_TEST(test_secrets_wiped);
    RUN_TEST(test_port_failures);
    RUN_TEST(test_broadcast_addresses);
    RUN_TEST(test_broadcast_pairing_completes);
    RUN_TEST(test_broadcast_two_masters_overlap);
    RUN_TEST(test_broadcast_refusal_before_commit);
    RUN_TEST(test_newer_keypad_payloads_are_taken);
    RUN_TEST(test_newer_master_payloads_are_taken);
    RUN_TEST(test_version_0_and_small_order_keys_refused);
    RUN_TEST(test_fuzz_pairing_input);
    return UNITY_END();
}
