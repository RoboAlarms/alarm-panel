/*
 * test_zw_crypto.c - the Z-Wave host's S2/S0 cryptography (components/core/zwave_host,
 * docs/features/36 ZW-008): the three primitives through the PC port, and the portable modes,
 * key derivations and nonces on top of them, against the standards' vectors (FIPS-197, NIST
 * SP 800-38A, RFC 4493, RFC 3610, RFC 7748, NIST CAVP CTR_DRBG) and security.md section 11's
 * known answers (Z-Wave JS's own code). Every standards vector was read from the published text
 * (the CAVP ones from NIST's response file) and checked against Python's `cryptography` (OpenSSL)
 * before it went in; the extra CCM lengths were computed there, and the multi-block CTR_DRBG
 * draws by vectors/s2_reference.py.
 *
 * Requirements: ZW-008, as ZW-SEC-001 AES block, ZW-SEC-002 CMAC, ZW-SEC-003 CCM,
 * ZW-SEC-004 X25519 and key pairs, ZW-SEC-005 CTR_DRBG, ZW-SEC-006 OFB and S0's CBC-MAC,
 * ZW-SEC-007 network key expansion, ZW-SEC-008 temporary key, ZW-SEC-009 SPAN,
 * ZW-SEC-010 S2 encapsulation, ZW-SEC-011 S0.
 */
#include <string.h>

#include "unity.h"
#include "zw_host_crypto.h"
#include "zwave_host/zw_crypto.h"

static zw_host_crypto_t hc;
static zw_crypto_port_t port;

void setUp(void)
{
    zw_host_crypto_init(&hc, 0x5A17u);
    port = zw_host_crypto_port(&hc);
}

void tearDown(void) {}

/* ---- helpers ------------------------------------------------------------------------------ */

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Hex text (spaces allowed) to bytes; the test fails on bad text or a full buffer. */
static size_t unhex(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;
    for (const char *p = hex; *p != '\0';) {
        if (*p == ' ') {
            p++;
            continue;
        }
        const int hi = nibble(p[0]);
        const int lo = hi < 0 ? -1 : nibble(p[1]);
        TEST_ASSERT_TRUE_MESSAGE(hi >= 0 && lo >= 0, hex);
        TEST_ASSERT_TRUE_MESSAGE(n < max, hex);
        out[n++] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return n;
}

/* The n bytes at got are exactly the hex text. */
static void expect_hex(const char *hex, const uint8_t *got, size_t n)
{
    uint8_t want[512];
    const size_t len = unhex(hex, want, sizeof want);
    TEST_ASSERT_EQUAL_UINT(len, n);
    if (n > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, n);
    }
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

/* ---- ZW-SEC-001 AES-128, one block through the PC port -------------------------------------- */

/* The first 64 bytes of NIST SP 800-38A's example plaintext; RFC 4493 uses its prefixes. */
static const char SP800_38A_PT[] =
    "6bc1bee22e409f96e93d7e117393172a ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef f69f2445df4f9b17ad2b417be66c3710";
static const char SP800_38A_KEY[] = "2b7e151628aed2a6abf7158809cf4f3c";

/* ZW-SEC-001: FIPS-197 (2001) Appendix C.1, AES-128. */
static void test_aes_fips197_c1(void)
{
    uint8_t key[16], in[16], out[16];
    unhex("000102030405060708090a0b0c0d0e0f", key, sizeof key);
    unhex("00112233445566778899aabbccddeeff", in, sizeof in);
    TEST_ASSERT_TRUE(port.aes128_encrypt(port.ctx, key, in, out));
    expect_hex("69c4e0d86a7b0430d8cdb78070b4c55a", out, sizeof out);
}

/* ZW-SEC-001: NIST SP 800-38A F.1.1 ECB-AES128.Encrypt, its four blocks. */
static void test_aes_sp800_38a_ecb(void)
{
    static const char *const ct[4] = {
        "3ad77bb40d7a3660a89ecaf32466ef97", "f5d3d58503b9699de785895a96fdbaaf",
        "43b1cd7f598ece23881b00e3ed030688", "7b0c785e27e8ad3f8223207104725dd4",
    };
    uint8_t key[16], pt[64], out[16];
    unhex(SP800_38A_KEY, key, sizeof key);
    unhex(SP800_38A_PT, pt, sizeof pt);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(port.aes128_encrypt(port.ctx, key, &pt[16 * i], out));
        expect_hex(ct[i], out, sizeof out);
    }
}

/* ---- ZW-SEC-002 AES-CMAC ------------------------------------------------------------------ */

/* ZW-SEC-002: RFC 4493 section 4, all four examples (the empty one is security.md section 2's
 * NIST vector): a padded last block (K2) with 0 and 40 bytes, a complete one (K1) with 16 and
 * 64. The empty message may come as NULL. */
static void test_cmac_rfc4493(void)
{
    static const struct {
        size_t len;
        const char *mac;
    } ex[] = {
        {0u, "bb1d6929e95937287fa37d129b756746"},
        {16u, "070a16b46b4d4144f79bdd9dd04a287c"},
        {40u, "dfa66747de9ae63030ca32611497c827"},
        {64u, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    uint8_t key[16], msg[64], mac[16];
    unhex(SP800_38A_KEY, key, sizeof key);
    unhex(SP800_38A_PT, msg, sizeof msg);
    for (size_t i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        TEST_ASSERT_TRUE(zw_aes_cmac(&port, key, msg, ex[i].len, mac));
        expect_hex(ex[i].mac, mac, sizeof mac);
    }
    TEST_ASSERT_TRUE(zw_aes_cmac(&port, key, NULL, 0u, mac));
    expect_hex(ex[0].mac, mac, sizeof mac);
    /* The MAC may overwrite the message it covers. */
    TEST_ASSERT_TRUE(zw_aes_cmac(&port, key, msg, 16u, msg));
    expect_hex(ex[1].mac, msg, 16u);
}

/* ZW-SEC-002: refused arguments, and the MAC zeroed. */
static void test_cmac_refuses_bad_arguments(void)
{
    uint8_t key[16] = {0}, msg[4] = {0}, mac[16];
    memset(mac, 0xEE, sizeof mac);
    TEST_ASSERT_FALSE(zw_aes_cmac(&port, key, NULL, 1u, mac));
    TEST_ASSERT_TRUE(all_zero(mac, sizeof mac));
    memset(mac, 0xEE, sizeof mac);
    TEST_ASSERT_FALSE(zw_aes_cmac(&port, NULL, msg, sizeof msg, mac));
    TEST_ASSERT_TRUE(all_zero(mac, sizeof mac));
    memset(mac, 0xEE, sizeof mac);
    TEST_ASSERT_FALSE(zw_aes_cmac(NULL, key, msg, sizeof msg, mac));
    TEST_ASSERT_TRUE(all_zero(mac, sizeof mac));
    TEST_ASSERT_FALSE(zw_aes_cmac(&port, key, msg, sizeof msg, NULL));
}

/* ---- ZW-SEC-003 AES-CCM (M = 8, L = 2) ---------------------------------------------------- */

/* RFC 3610 section 8's packets with an 8-byte tag: #1-#6 and #13-#18 (#7-#12 and #19-#24 carry
 * 10-byte tags). The input packet is the associated data (aad_len bytes) and the message; the
 * output packet the same associated data, the ciphertext and the tag. */
static const struct {
    int packet;
    const char *key;
    const char *nonce;
    size_t aad_len;
    const char *in;
    const char *out;
} RFC3610[] = {
    {1, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", "00000003020100a0a1a2a3a4a5", 8u,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e",
     "0001020304050607588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0"},
    {2, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", "00000004030201a0a1a2a3a4a5", 8u,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "000102030405060772c91a36e135f8cf291ca894085c87e3cc15c439c9e43a3ba091d56e10400916"},
    {3, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", "00000005040302a0a1a2a3a4a5", 8u,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
     "000102030405060751b1e5f44a197d1da46b0f8e2d282ae871e838bb64da8596574adaa76fbd9fb0c5"},
    {4, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", "00000006050403a0a1a2a3a4a5", 12u,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e",
     "000102030405060708090a0ba28c6865939a9a79faaa5c4c2a9d4a91cdac8c96c861b9c9e61ef1"},
    {5, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", "00000007060504a0a1a2a3a4a5", 12u,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "000102030405060708090a0bdcf1fb7b5d9e23fb9d4e131253658ad86ebdca3e51e83f077d9c2d93"},
    {6, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", "00000008070605a0a1a2a3a4a5", 12u,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
     "000102030405060708090a0b6fc1b011f006568b5171a42d953d469b2570a4bd87405a0443ac91cb94"},
    {13, "d7828d13b2b0bdc325a76236df93cc6b", "00412b4ea9cdbe3c9696766cfa", 8u,
     "0be1a88bace018b108e8cf97d820ea258460e96ad9cf5289054d895ceac47c",
     "0be1a88bace018b14cb97f86a2a4689a877947ab8091ef5386a6ffbdd080f8e78cf7cb0cddd7b3"},
    {14, "d7828d13b2b0bdc325a76236df93cc6b", "0033568ef7b2633c9696766cfa", 8u,
     "63018f76dc8a1bcb9020ea6f91bdd85afa0039ba4baff9bfb79c7028949cd0ec",
     "63018f76dc8a1bcb4ccb1e7ca981befaa0726c55d378061298c85c92814abc33c52ee81d7d77c08a"},
    {15, "d7828d13b2b0bdc325a76236df93cc6b", "00103fe41336713c9696766cfa", 8u,
     "aa6cfa36cae86b40b916e0eacc1c00d7dcec68ec0b3bbb1a02de8a2d1aa346132e",
     "aa6cfa36cae86b40b1d23a2220ddc0ac900d9aa03c61fcf4a559a4417767089708a776796edb723506"},
    {16, "d7828d13b2b0bdc325a76236df93cc6b", "00764c63b8058e3c9696766cfa", 12u,
     "d0d0735c531e1becf049c24412daac5630efa5396f770ce1a66b21f7b2101c",
     "d0d0735c531e1becf049c24414d253c3967b70609b7cbb7c499160283245269a6f49975bcadeaf"},
    {17, "d7828d13b2b0bdc325a76236df93cc6b", "00f8b678094e3b3c9696766cfa", 12u,
     "77b60f011c03e1525899bcaee88b6a46c78d63e52eb8c546efb5de6f75e9cc0d",
     "77b60f011c03e1525899bcae5545ff1a085ee2efbf52b2e04bee1e2336c73e3f762c0c7744fe7e3c"},
    {18, "d7828d13b2b0bdc325a76236df93cc6b", "00d560912d3f703c9696766cfa", 12u,
     "cd9044d2b71fdb8120ea60c06435acbafb11a82e2f071d7ca4a5ebd93a803ba87f",
     "cd9044d2b71fdb8120ea60c0009769ecabdf48625594c59251e6035722675e04c847099e5ae0704551"},
};

/* ZW-SEC-003: RFC 3610's twelve 8-byte-tag packets, encrypted, then decrypted back. */
static void test_ccm_rfc3610_packets(void)
{
    for (size_t v = 0; v < sizeof RFC3610 / sizeof RFC3610[0]; v++) {
        uint8_t key[16], nonce[13], in[64], out[64], ct[64], tag[8], back[64];
        unhex(RFC3610[v].key, key, sizeof key);
        TEST_ASSERT_EQUAL_UINT(13u, unhex(RFC3610[v].nonce, nonce, sizeof nonce));
        const size_t in_len = unhex(RFC3610[v].in, in, sizeof in);
        const size_t out_len = unhex(RFC3610[v].out, out, sizeof out);
        const size_t a = RFC3610[v].aad_len;
        const size_t m = in_len - a;
        TEST_ASSERT_EQUAL_UINT(in_len + 8u, out_len);

        TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, key, nonce, in, a, &in[a], m, ct, tag));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(&out[a], ct, m);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(&out[a + m], tag, 8);

        TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, key, nonce, out, a, &out[a], m, &out[a + m],
                                            back));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(&in[a], back, m);
    }
}

/* The patterns of the extra CCM vectors: aad[i] = i, msg[i] = 31 i + 7 (mod 256). */
static void pattern_aad(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)i;
    }
}

static void pattern_msg(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(31u * i + 7u);
    }
}

/* ZW-SEC-003: lengths the packets don't reach - no message, no associated data, neither, the
 * first associated-data block exactly full (2 + 14) and one byte over, a message of exactly one
 * block and one byte over, seven blocks - computed with Python's cryptography (AESCCM, 8-byte
 * tag): key 000102..0f, nonce 101112..1c, the patterns above. */
static void test_ccm_edge_lengths(void)
{
    static const struct {
        size_t aad_len, len;
        const char *ct, *tag;
    } ev[] = {
        {0u, 0u, "", "5e5234e976e983a6"},
        {5u, 0u, "", "ed002f9f7b6f90c9"},
        {0u, 1u, "7b", "60fc50ccecf2457e"},
        {14u, 16u, "7bc735253bfe2f34444775cd549dd559", "5a32f2ef3978418d"},
        {15u, 17u, "7bc735253bfe2f34444775cd549dd559dd", "fda1b61987978082"},
        {40u, 100u,
         "7bc735253bfe2f34444775cd549dd559ddc5602d0c052ffad243379fe5322c8be27cba9a4212055ae0"
         "92be02c4010fd95b6af0f148c5f1bd500cdaba6ca97ea79b777ff6508bcc8a77a27dc4604d09353cdb"
         "a3a2d29da7a12907e7782238aa86b850abfc",
         "e7872f3ff53416f3"},
    };
    uint8_t key[16], nonce[13], aad[40], msg[100], ct[100], tag[8], back[100];
    unhex("000102030405060708090a0b0c0d0e0f", key, sizeof key);
    unhex("101112131415161718191a1b1c", nonce, sizeof nonce);
    pattern_aad(aad, sizeof aad);
    pattern_msg(msg, sizeof msg);
    for (size_t v = 0; v < sizeof ev / sizeof ev[0]; v++) {
        const size_t a = ev[v].aad_len, m = ev[v].len;
        TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, key, nonce, a ? aad : NULL, a, m ? msg : NULL,
                                            m, m ? ct : NULL, tag));
        expect_hex(ev[v].ct, ct, m);
        expect_hex(ev[v].tag, tag, sizeof tag);
        memset(back, 0xEE, sizeof back);
        TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, key, nonce, a ? aad : NULL, a, m ? ct : NULL,
                                            m, tag, m ? back : NULL));
        if (m > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, back, m);
        }
    }
}

/* ZW-SEC-003: the longest the two-byte encodings carry - 0xFEFF bytes of associated data and a
 * 0xFFFF-byte message (4096 counter blocks), again against Python's cryptography - and one
 * byte more of either refused, with the output zeroed. */
static void test_ccm_longest_lengths(void)
{
    static uint8_t aad[0xFF00], msg[0x10000], ct[0x10000], back[0x10000];
    uint8_t key[16], nonce[13], tag[8];
    unhex("000102030405060708090a0b0c0d0e0f", key, sizeof key);
    unhex("101112131415161718191a1b1c", nonce, sizeof nonce);
    pattern_aad(aad, sizeof aad);
    pattern_msg(msg, sizeof msg);

    TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, key, nonce, aad, 0xFEFFu, msg, 0xFFFFu, ct, tag));
    expect_hex("7bc735253bfe2f34444775cd549dd559", ct, 16u);
    expect_hex("28b5c938eec5bdb7c5a97fd696ccd1eb", &ct[0xFFFFu - 16u], 16u);
    expect_hex("81b57b5c848f6e8d", tag, sizeof tag);
    TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, key, nonce, aad, 0xFEFFu, ct, 0xFFFFu, tag, back));
    TEST_ASSERT_EQUAL_MEMORY(msg, back, 0xFFFFu);

    memset(ct, 0xEE, sizeof ct);
    TEST_ASSERT_FALSE(zw_aes_ccm_encrypt(&port, key, nonce, aad, 0xFF00u, msg, 16u, ct, tag));
    TEST_ASSERT_TRUE(all_zero(ct, 16u) && all_zero(tag, sizeof tag));
    memset(ct, 0xEE, sizeof ct);
    TEST_ASSERT_FALSE(zw_aes_ccm_encrypt(&port, key, nonce, aad, 16u, msg, 0x10000u, ct, tag));
    TEST_ASSERT_TRUE(all_zero(ct, sizeof ct));
    memset(back, 0xEE, sizeof back);
    TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(&port, key, nonce, aad, 0xFF00u, ct, 16u, tag, back));
    TEST_ASSERT_TRUE(all_zero(back, 16u));
    memset(back, 0xEE, sizeof back);
    TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(&port, key, nonce, aad, 16u, ct, 0x10000u, tag, back));
    TEST_ASSERT_TRUE(all_zero(back, sizeof back));
}

/* security.md section 11's first S2 frame, as CCM sees it: the network KeyCCM, SPAN nonce 1,
 * the AAD, the inner command (Notification Report: Access Control, door open) and what it
 * encrypts to. */
#define S11_PT_LEN 9u
static const char S11_KEY_CCM[] = "27f90dd6d74697cba54eb11a0ee80617";
static const char S11_PSTRING[] =
    "aa3b035d8f802f368d6482f4299a2b1255eed934805c397bb72c47c61ff7acfa";
static const char S11_NONCE1[] = "f8648b2a31d81dea7bb3e02cef";
static const char S11_AAD[] = "0201c0ffee01002755011241101112131415161718191a1b1c1d1e1f";
static const char S11_INNER[] = "7105000000ff061600";
static const char S11_CT[] = "ee50c2d601c41ad285";
static const char S11_TAG[] = "02add758929a40ad";

typedef struct {
    uint8_t key[16], nonce[13], aad[28], pt[S11_PT_LEN], ct[S11_PT_LEN], tag[8];
} s11_frame_t;

static void load_s11_frame(s11_frame_t *f)
{
    unhex(S11_KEY_CCM, f->key, sizeof f->key);
    unhex(S11_NONCE1, f->nonce, sizeof f->nonce);
    unhex(S11_AAD, f->aad, sizeof f->aad);
    unhex(S11_INNER, f->pt, sizeof f->pt);
    unhex(S11_CT, f->ct, sizeof f->ct);
    unhex(S11_TAG, f->tag, sizeof f->tag);
}

/* Decrypting f with one bit of `target` (len bytes) flipped must fail and leave pt all zero. */
static void expect_refused_after_flip(s11_frame_t *f, uint8_t *target, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            uint8_t pt[S11_PT_LEN];
            memset(pt, 0xEE, sizeof pt);
            target[i] = (uint8_t)(target[i] ^ (1u << bit));
            TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(&port, f->key, f->nonce, f->aad, sizeof f->aad,
                                                 f->ct, sizeof f->ct, f->tag, pt));
            TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
            target[i] = (uint8_t)(target[i] ^ (1u << bit));
        }
    }
}

/* ZW-SEC-003: decryption refuses the frame with any bit of its tag, ciphertext, associated data
 * or nonce changed, or with a length changed, and no byte of plaintext is left behind. */
static void test_ccm_refuses_tampering(void)
{
    s11_frame_t f;
    uint8_t pt[S11_PT_LEN];
    load_s11_frame(&f);
    TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, f.key, f.nonce, f.aad, sizeof f.aad, f.ct,
                                        sizeof f.ct, f.tag, pt));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f.pt, pt, sizeof pt);

    expect_refused_after_flip(&f, f.tag, sizeof f.tag);
    expect_refused_after_flip(&f, f.ct, sizeof f.ct);
    expect_refused_after_flip(&f, f.aad, sizeof f.aad);
    expect_refused_after_flip(&f, f.nonce, sizeof f.nonce);
    expect_refused_after_flip(&f, f.key, sizeof f.key);

    memset(pt, 0xEE, sizeof pt);
    TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(&port, f.key, f.nonce, f.aad, sizeof f.aad - 1u, f.ct,
                                         sizeof f.ct, f.tag, pt));
    TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
    memset(pt, 0xEE, sizeof pt);
    TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(&port, f.key, f.nonce, f.aad, sizeof f.aad, f.ct,
                                         sizeof f.ct - 1u, f.tag, pt));
    TEST_ASSERT_TRUE(all_zero(pt, sizeof f.ct - 1u));
    /* Still intact after all that: the flips were undone. */
    TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, f.key, f.nonce, f.aad, sizeof f.aad, f.ct,
                                        sizeof f.ct, f.tag, pt));
}

/* ZW-SEC-003: in place - a message encrypted where it lies, and decrypted there again; the tag
 * may sit right after the ciphertext in the same frame buffer. */
static void test_ccm_in_place(void)
{
    s11_frame_t f;
    uint8_t frame[S11_PT_LEN + 8u];
    load_s11_frame(&f);
    memcpy(frame, f.pt, sizeof f.pt);
    TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, f.key, f.nonce, f.aad, sizeof f.aad, frame, S11_PT_LEN,
                                        frame, &frame[S11_PT_LEN]));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f.ct, frame, S11_PT_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f.tag, &frame[S11_PT_LEN], 8u);
    TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, f.key, f.nonce, f.aad, sizeof f.aad, frame, S11_PT_LEN,
                                        &frame[S11_PT_LEN], frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f.pt, frame, S11_PT_LEN);

    /* RFC 3610 packet #3 (two blocks, the second partial), in place. */
    uint8_t key[16], nonce[13], buf[64], out[64], tag[8];
    unhex(RFC3610[2].key, key, sizeof key);
    unhex(RFC3610[2].nonce, nonce, sizeof nonce);
    const size_t in_len = unhex(RFC3610[2].in, buf, sizeof buf);
    unhex(RFC3610[2].out, out, sizeof out);
    const size_t a = RFC3610[2].aad_len, m = in_len - a;
    TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, key, nonce, buf, a, &buf[a], m, &buf[a], tag));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(out, buf, in_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&out[in_len], tag, 8u);
}

/* Encryption of the section 11 frame with one argument spoiled: refused, every output it was
 * given zeroed (an output passed as NULL stays untouched). */
static void expect_encrypt_refused(const zw_crypto_port_t *p, const uint8_t *key,
                                   const uint8_t *nonce, const uint8_t *aad, const uint8_t *pt,
                                   bool give_ct, bool give_tag)
{
    uint8_t ct[S11_PT_LEN], tag[8];
    memset(ct, 0xEE, sizeof ct);
    memset(tag, 0xEE, sizeof tag);
    TEST_ASSERT_FALSE(zw_aes_ccm_encrypt(p, key, nonce, aad, 28u, pt, S11_PT_LEN, give_ct ? ct : NULL,
                                         give_tag ? tag : NULL));
    TEST_ASSERT_TRUE(give_ct ? all_zero(ct, sizeof ct) : ct[0] == 0xEE);
    TEST_ASSERT_TRUE(give_tag ? all_zero(tag, sizeof tag) : tag[0] == 0xEE);
}

/* The same for decryption: refused, and the plaintext zeroed. */
static void expect_decrypt_refused(const zw_crypto_port_t *p, const uint8_t *key,
                                   const uint8_t *nonce, const uint8_t *aad, const uint8_t *ct,
                                   const uint8_t *tag)
{
    uint8_t pt[S11_PT_LEN];
    memset(pt, 0xEE, sizeof pt);
    TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(p, key, nonce, aad, 28u, ct, S11_PT_LEN, tag, pt));
    TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
}

/* ZW-SEC-003: refused arguments - no port or no AES in it, no key, nonce or tag, no buffer
 * where a length says there is one - and what a refused call leaves: zeroed outputs. Empty
 * parts may come as NULL. */
static void test_ccm_refuses_bad_arguments(void)
{
    s11_frame_t f;
    zw_crypto_port_t no_aes = port;
    no_aes.aes128_encrypt = NULL;
    load_s11_frame(&f);

    expect_encrypt_refused(NULL, f.key, f.nonce, f.aad, f.pt, true, true);
    expect_encrypt_refused(&no_aes, f.key, f.nonce, f.aad, f.pt, true, true);
    expect_encrypt_refused(&port, NULL, f.nonce, f.aad, f.pt, true, true);
    expect_encrypt_refused(&port, f.key, NULL, f.aad, f.pt, true, true);
    expect_encrypt_refused(&port, f.key, f.nonce, NULL, f.pt, true, true);
    expect_encrypt_refused(&port, f.key, f.nonce, f.aad, NULL, true, true);
    expect_encrypt_refused(&port, f.key, f.nonce, f.aad, f.pt, false, true);
    expect_encrypt_refused(&port, f.key, f.nonce, f.aad, f.pt, true, false);

    expect_decrypt_refused(NULL, f.key, f.nonce, f.aad, f.ct, f.tag);
    expect_decrypt_refused(&no_aes, f.key, f.nonce, f.aad, f.ct, f.tag);
    expect_decrypt_refused(&port, NULL, f.nonce, f.aad, f.ct, f.tag);
    expect_decrypt_refused(&port, f.key, NULL, f.aad, f.ct, f.tag);
    expect_decrypt_refused(&port, f.key, f.nonce, NULL, f.ct, f.tag);
    expect_decrypt_refused(&port, f.key, f.nonce, f.aad, NULL, f.tag);
    expect_decrypt_refused(&port, f.key, f.nonce, f.aad, f.ct, NULL);
    TEST_ASSERT_FALSE(zw_aes_ccm_decrypt(&port, f.key, f.nonce, f.aad, 28u, f.ct, S11_PT_LEN, f.tag,
                                         NULL));
}

/* ZW-SEC-003: the tag comparison sees a difference in any byte and any bit. */
static void test_ct_equal(void)
{
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b[8];
    memcpy(b, a, sizeof b);
    TEST_ASSERT_TRUE(zw_ct_equal(a, b, sizeof a));
    TEST_ASSERT_TRUE(zw_ct_equal(a, b, 0u));
    for (size_t i = 0; i < sizeof b; i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            b[i] = (uint8_t)(b[i] ^ (1u << bit));
            TEST_ASSERT_FALSE(zw_ct_equal(a, b, sizeof a));
            b[i] = (uint8_t)(b[i] ^ (1u << bit));
        }
    }
}

/* ---- ZW-SEC-004 X25519 and key pairs ------------------------------------------------------ */

static void x25519_hex(const char *k_hex, const char *u_hex, uint8_t out[32])
{
    uint8_t k[32], u[32];
    TEST_ASSERT_EQUAL_UINT(32u, unhex(k_hex, k, sizeof k));
    TEST_ASSERT_EQUAL_UINT(32u, unhex(u_hex, u, sizeof u));
    TEST_ASSERT_TRUE(port.x25519(port.ctx, out, k, u));
}

/* ZW-SEC-004: RFC 7748 section 5.2's two X25519 vectors through the PC port (the second
 * u-coordinate has its top bit set, which X25519 must ignore). */
static void test_x25519_rfc7748_vectors(void)
{
    uint8_t out[32];
    x25519_hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
               "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", out);
    expect_hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", out, 32u);
    x25519_hex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
               "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", out);
    expect_hex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", out, 32u);
}

/* ZW-SEC-004: RFC 7748 section 5.2 iterated - k = X25519(k, u), u = the old k, from
 * k = u = 9 - after one iteration (the RFC's value) and after 100 (computed with Python's
 * cryptography, OpenSSL: the RFC publishes 1,000, which this port reproduces but which takes
 * about 2 s in a Debug build, too long for every run). */
static void test_x25519_rfc7748_iterated(void)
{
    uint8_t k[32] = {9}, u[32] = {9}, next[32];
    for (int i = 1; i <= 100; i++) {
        TEST_ASSERT_TRUE(port.x25519(port.ctx, next, k, u));
        memcpy(u, k, sizeof u);
        memcpy(k, next, sizeof k);
        if (i == 1) {
            expect_hex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", k,
                       32u);
        }
    }
    expect_hex("b1a5a73158904c020866c13939dd7e1aa26852ee1d2609c92e5a8f1debe2150a", k, 32u);
}

static const char ALICE_PRIV[] = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
static const char ALICE_PUB[] = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
static const char BOB_PRIV[] = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
static const char BOB_PUB[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
static const char SHARED[] = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

/* ZW-SEC-004: RFC 7748 section 6.1 - Alice's and Bob's public keys are X25519(priv, 9), and
 * both reach the same shared secret through zw_s2_shared_secret. */
static void test_x25519_rfc7748_ecdh(void)
{
    static const uint8_t base[32] = {9};
    uint8_t a[32], b[32], apub[32], bpub[32], s[32];
    unhex(ALICE_PRIV, a, sizeof a);
    unhex(BOB_PRIV, b, sizeof b);
    TEST_ASSERT_TRUE(port.x25519(port.ctx, apub, a, base));
    expect_hex(ALICE_PUB, apub, 32u);
    TEST_ASSERT_TRUE(port.x25519(port.ctx, bpub, b, base));
    expect_hex(BOB_PUB, bpub, 32u);
    TEST_ASSERT_TRUE(zw_s2_shared_secret(&port, a, bpub, s));
    expect_hex(SHARED, s, 32u);
    TEST_ASSERT_TRUE(zw_s2_shared_secret(&port, b, apub, s));
    expect_hex(SHARED, s, 32u);

    /* A u-coordinate that is not reduced counts mod p (RFC 7748 section 5): p + 9 is the base
     * point again (OpenSSL agrees). */
    uint8_t u[32];
    unhex("f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", u, sizeof u);
    TEST_ASSERT_TRUE(port.x25519(port.ctx, s, a, u));
    expect_hex(ALICE_PUB, s, 32u);
}

/* ZW-SEC-004: zw_s2_shared_secret refuses a peer point that makes the result all zero whatever
 * the private key (RFC 7748 section 6.1): u = 0, 1, p (0 again), p + 1 (1 again) and a point of
 * order 8 - OpenSSL refuses each of them too - and leaves its output zeroed. */
static void test_shared_secret_refuses_small_order_points(void)
{
    static const char *const low[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    };
    uint8_t a[32], u[32], s[32], raw[32];
    unhex(ALICE_PRIV, a, sizeof a);
    for (size_t i = 0; i < sizeof low / sizeof low[0]; i++) {
        unhex(low[i], u, sizeof u);
        TEST_ASSERT_TRUE(port.x25519(port.ctx, raw, a, u)); /* the primitive answers ... */
        TEST_ASSERT_TRUE(all_zero(raw, sizeof raw));       /* ... all zero */
        memset(s, 0xEE, sizeof s);
        TEST_ASSERT_FALSE(zw_s2_shared_secret(&port, a, u, s));
        TEST_ASSERT_TRUE(all_zero(s, sizeof s));
    }
}

/* ZW-SEC-004: a key pair from the seeded port: 32 random bytes and pub = X25519(priv, 9); the
 * same seed gives the same pair, the next draw another, and two pairs agree on a secret. */
static void test_keypair(void)
{
    static const uint8_t base[32] = {9};
    uint8_t priv[32], pub[32], again[32], priv2[32], pub2[32], s1[32], s2[32];
    TEST_ASSERT_TRUE(zw_s2_keypair(&port, priv, pub));
    TEST_ASSERT_TRUE(port.x25519(port.ctx, again, priv, base));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(again, pub, 32u);
    TEST_ASSERT_FALSE(all_zero(priv, sizeof priv));

    TEST_ASSERT_TRUE(zw_s2_keypair(&port, priv2, pub2));
    TEST_ASSERT_FALSE(zw_ct_equal(priv, priv2, 32u));
    TEST_ASSERT_FALSE(zw_ct_equal(pub, pub2, 32u));
    TEST_ASSERT_TRUE(zw_s2_shared_secret(&port, priv, pub2, s1));
    TEST_ASSERT_TRUE(zw_s2_shared_secret(&port, priv2, pub, s2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s1, s2, 32u);

    zw_host_crypto_init(&hc, 0x5A17u); /* the seed of setUp: the first pair again */
    TEST_ASSERT_TRUE(zw_s2_keypair(&port, priv2, pub2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(priv, priv2, 32u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pub, pub2, 32u);
}

/* ---- ZW-SEC-005 CTR_DRBG ------------------------------------------------------------------ */

/* ZW-SEC-005: security.md section 2's CTR_DRBG beyond one block: Instantiate(00..1f, 40..5f),
 * then Generate(40) twice (three blocks, the last partial, and the Update between calls), and
 * Generate(16) with no personalization string - computed by vectors/s2_reference.py's
 * CtrDrbg (Python's cryptography underneath). The SPAN nonces of ZW-SEC-009 are the Z-Wave JS
 * check; NIST's own CAVP vectors are test_ctr_drbg_nist_cavp's. */
static void test_ctr_drbg_multi_block(void)
{
    zw_ctr_drbg_t d;
    uint8_t entropy[32], perso[32], out[40];
    for (size_t i = 0; i < 32u; i++) {
        entropy[i] = (uint8_t)i;
        perso[i] = (uint8_t)(0x40u + i);
    }
    TEST_ASSERT_TRUE(zw_ctr_drbg_instantiate(&port, &d, entropy, perso));
    TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, out, sizeof out));
    expect_hex("fa1df7435039c6493b14d8c8f9715ba5ce6ee31220eedee365a4b6b72fd68554fd9e5a450fbeacba",
               out, sizeof out);
    TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, out, sizeof out));
    expect_hex("51371ba45a35873bbdfae8ba054b0a49fa651ad01abb6ec19a41b5d00a2e91f1e2a4b473fe940506",
               out, sizeof out);

    TEST_ASSERT_TRUE(zw_ctr_drbg_instantiate(&port, &d, entropy, NULL));
    TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, out, 16u));
    expect_hex("1686ffcf9f358be74452e647ba156aab", out, 16u);
    zw_wipe(&d, sizeof d);
}

/* ZW-SEC-005: V is one 128-bit big-endian counter: from V = ff..ff it wraps to 0 (the first
 * block is E(Key, 0)), and from 00..00 ff..ff (7 bytes of ff) the carry runs into the middle.
 * Generate(32) and the state after it, from a hand-set state, as vectors/s2_reference.py's
 * CtrDrbg computes them. */
static void test_ctr_drbg_counter_carries(void)
{
    static const struct {
        const char *v, *out, *key_after, *v_after;
    } cv[] = {
        {"ffffffffffffffffffffffffffffffff",
         "c6a13b37878f5b826f4f8162a1c8d8797346139595c0b41e497bbde365f42d0a",
         "49d68753999ba68ce3897a686081b09d", "b9ad2b2e346ac238505d365e9cb7fc56"},
        {"000000000000000000ffffffffffffff",
         "5f2c80d352d3e8fcb4aea438188d77c87032fe5be91f0159c9c4ae28b81bdbed",
         "73643dad01fb0db96182e37e40719eb5", "19822f0f9f5bc8d0f48c33d397f6ed83"},
    };
    for (size_t i = 0; i < sizeof cv / sizeof cv[0]; i++) {
        zw_ctr_drbg_t d;
        uint8_t out[32];
        unhex("000102030405060708090a0b0c0d0e0f", d.key, sizeof d.key);
        unhex(cv[i].v, d.v, sizeof d.v);
        TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, out, sizeof out));
        expect_hex(cv[i].out, out, sizeof out);
        expect_hex(cv[i].key_after, d.key, sizeof d.key);
        expect_hex(cv[i].v_after, d.v, sizeof d.v);
    }
}

/* ZW-SEC-005: a failed call leaves the generator as it was, so the next draw is the one the
 * failed call would have made (no nonce skipped, none repeated); refused: n = 0, n above
 * 2^19 bits, no output, no generator, no entropy. */
static void test_ctr_drbg_failure_keeps_state(void)
{
    zw_ctr_drbg_t d, saved;
    uint8_t entropy[32] = {0}, want[16], out[16];
    TEST_ASSERT_TRUE(zw_ctr_drbg_instantiate(&port, &d, entropy, NULL));
    saved = d;
    TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, want, sizeof want));
    d = saved;

    zw_host_crypto_fail_at(&hc, 2u); /* call 0 is the output block, 1 and 2 the Update's */
    memset(out, 0xEE, sizeof out);
    TEST_ASSERT_FALSE(zw_ctr_drbg_generate(&port, &d, out, sizeof out));
    TEST_ASSERT_TRUE(all_zero(out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY(&saved, &d, sizeof d);
    zw_host_crypto_fail_at(&hc, ZW_HOST_NEVER);
    TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, sizeof out);

    d = saved;
    TEST_ASSERT_FALSE(zw_ctr_drbg_generate(&port, &d, out, 0u));
    TEST_ASSERT_FALSE(zw_ctr_drbg_generate(&port, &d, NULL, 16u));
    TEST_ASSERT_FALSE(zw_ctr_drbg_generate(&port, NULL, out, 16u));
    static uint8_t big[ZW_CTR_DRBG_MAX_REQUEST + 1u];
    TEST_ASSERT_FALSE(zw_ctr_drbg_generate(&port, &d, big, sizeof big));
    TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, big, ZW_CTR_DRBG_MAX_REQUEST));
    d = saved;
    TEST_ASSERT_FALSE(zw_ctr_drbg_instantiate(&port, &d, NULL, NULL));
    TEST_ASSERT_FALSE(zw_ctr_drbg_instantiate(&port, NULL, entropy, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&saved, &d, sizeof d);
}

/* SP 800-90A 10.2.1.4 Reseed with no derivation function and no additional input: CTR_DRBG_Update
 * (entropy_input, Key, V) on the running state - V+1, T1 = E(Key, V), V+1, T2 = E(Key, V),
 * Key | V = (T1 | T2) XOR entropy - written here on the port's AES block: the panel never
 * reseeds its generators (security.md section 2), so the library has no reseed of its own. */
static void cavp_reseed(zw_ctr_drbg_t *d, const uint8_t entropy[32])
{
    uint8_t t[32];
    for (size_t b = 0; b < 2u; b++) {
        unsigned carry = 1u;
        for (size_t i = 16u; i-- > 0u;) {
            const unsigned s = (unsigned)d->v[i] + carry;
            d->v[i] = (uint8_t)s;
            carry = s >> 8;
        }
        TEST_ASSERT_TRUE(port.aes128_encrypt(port.ctx, d->key, d->v, &t[16u * b]));
    }
    for (size_t i = 0; i < 32u; i++) {
        t[i] = (uint8_t)(t[i] ^ entropy[i]);
    }
    memcpy(d->key, &t[0], 16u);
    memcpy(d->v, &t[16], 16u);
    zw_wipe(t, sizeof t);
}

/* ZW-SEC-005: the CTR_DRBG against NIST's own test vectors - the NIST CAVP CTR_DRBG (SP 800-90A)
 * response file, [AES-128 no df] [PredictionResistance = False] [EntropyInputLen = 256]
 * [NonceLen = 0] [AdditionalInputLen = 0] [ReturnedBitsLen = 512], as Z-Wave JS carries it
 * (packages/core/src/security/ctr_drbg.test.vectors.txt, commit fac3872; MIT, NIST's data is
 * public). S2's generators are exactly that: AES-128, no derivation function, no additional input,
 * and a personalization string for the SPANs (Instantiate(MEI, PString), security.md section 6),
 * none for the panel's own generator - so both PersonalizationStringLen = 0 and = 256 are taken.
 * CAVP's procedure: Instantiate(EntropyInput, PersonalizationString), Reseed(EntropyInputReseed),
 * Generate(512 bits) twice; the second output is ReturnedBits. The file holds four such blocks of
 * each kind, fifteen counts each; COUNT 0 and 14 of the first and last of each kind are here (the
 * file lines are its section headers). All 120 counts of those eight blocks were checked first
 * against a CTR_DRBG written separately on Python's cryptography (OpenSSL's AES). */
static void test_ctr_drbg_nist_cavp(void)
{
    static const struct {
        const char *what, *entropy, *perso, *reseed, *returned;
    } V[] = {
        {"line 2529 (PersonalizationStringLen = 0), COUNT = 0",
         "ed1e7f21ef66ea5d8e2a85b9337245445b71d6393a4eecb0e63c193d0f72f9a9", "",
         "303fb519f0a4e17d6df0b6426aa0ecb2a36079bd48be47ad2a8dbfe48da3efad",
         "f80111d08e874672f32f42997133a5210f7a9375e22cea70587f9cfafebe0f6a"
         "6aa2eb68e7dd9164536d53fa020fcab20f54caddfab7d6d91e5ffec1dfd8deaa"},
        {"line 2529 (PersonalizationStringLen = 0), COUNT = 14",
         "8ecdbf1cba26eae45f70ccfec0e42d6139be57f131ff60898a3b63968acf28ac", "",
         "8d860dcf67fbee47f33ed5273ff81956335d9152085f184f8427ad4234f95661",
         "8049f3fe2e62883f71cc43873b9775bf60a97c070370f9757c51488b050c0095"
         "9d085ddd8f8e3702aa4cd6ff19b6c62685afb7792eb003c07bbcc9f4a026d138"},
        {"line 2845 (PersonalizationStringLen = 256), COUNT = 0",
         "34cbc2b217f3d907fa2ad6a0d7a813b0fda1e17fbeed94b0e0a0abfbec947146",
         "e8fa4c5de825791e68180f2ba107e829c48299cb01be939cd0be76da120a91f2",
         "8326f8e9cfbd02eb076bbb9819d96a02386f80bf913c8e4a80361d82cafad52e",
         "52f5e718bf48d99e498775c00378e545799bb2059aef0b74be573d8283f02b52"
         "93917913bc8f26fc23760a1c86c3f5c844857419868eafeb17c9248227d026b8"},
        {"line 2845 (PersonalizationStringLen = 256), COUNT = 14",
         "f3ca53475eab617258e397151ab3c10a6356d18efa225a4342da13fa62aff99c",
         "48882b0a263582f0c6bb8288ba894d7141e665aa0c084c7d99eaa0c1f07e23c0",
         "34cc10a3d71398efec3c5854ef82a77a0b6bfcaf841ad9a1d5c7bf05aeed9082",
         "cac26b07204eea9a41e4289302d0da8f36ea200dacd2435f1bb1c1235cb36249"
         "7eeb87aea2889aa1093d13d2cee25eb11e28229c39acdf894122425b5948cae2"},
        {"line 4425 (PersonalizationStringLen = 0), COUNT = 0",
         "6eb2e533b902545c31cdd09a062cf59346594f34b563b090dfbd632aeadbd191", "",
         "2cd1cec6242ac25e7518b98da8a22bd13acfaf5fa45fee6021ba283926de1922",
         "ba5cab62fddf8c6ed16733c5bf0e39336c189d80a9669043ac892cea63d7e70d"
         "390e91741840bf7ee14c078cbbe97d114992ae781d9966a5f6c704e5c01811b5"},
        {"line 4425 (PersonalizationStringLen = 0), COUNT = 14",
         "e3d8fcb8c049e442d2bd07104c46f0602a1f60f87bdc02dbecdcfcf4006b5b0a", "",
         "e25327867ff27456eff9f4ae4375c7a85788b400dcae03ae8c892472c8a05221",
         "754063c679269931fdab8f90deaa967969f20b1805d93fe5b1928512cd2fe989"
         "84974b0bb1d7494d81f53e073f1a3a9378ea27307a154dc8a1fb1d3e17998f85"},
        {"line 4741 (PersonalizationStringLen = 256), COUNT = 0",
         "9680b338563c37df37d9b87c39a9f7d16e580e031063995123837ca7c5bb9a16",
         "7bd84c5397f2b0ddd4a63a359949f1966806d903885690c49f2a7032e3888bb8",
         "ba53153340c58900d4e4cec74707a1fb32c93108e26615b0e0096db62ec1999b",
         "2856d079fe2539db400e4289306bf0420c4cbce326b38b8cac7ac44954f65090"
         "00b4af2f50f22c8f2c0a897e9896681ae4c47a83688c7cfac86d6a1637e498ad"},
        {"line 4741 (PersonalizationStringLen = 256), COUNT = 14",
         "9d45bbae1356c67ba83136e8ef04d52cc58d1ba39d987a80a572e62a11e09152",
         "345b74cf00947dc1eedbbacd5e4030d5639f5e3c0b9fb986fdc0c3c6cbacb58b",
         "3150b6ab1fb005ceb8323c6982fc3af3092077241dcf4993fe91696bd5b8b747",
         "bff83f45e0a4223489b08409497471427e7b82f834082137497bc552127446a8"
         "a3f52a730914650cd753098162f7d253bbd625983430d1f16fcdb0f78a1348a8"},
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        uint8_t entropy[32], perso[32], reseed[32], out[64];
        TEST_ASSERT_EQUAL_UINT_MESSAGE(32u, unhex(V[i].entropy, entropy, sizeof entropy), V[i].what);
        const bool has_perso = V[i].perso[0] != '\0';
        if (has_perso) {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(32u, unhex(V[i].perso, perso, sizeof perso), V[i].what);
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(32u, unhex(V[i].reseed, reseed, sizeof reseed), V[i].what);
        zw_ctr_drbg_t d;
        TEST_ASSERT_TRUE_MESSAGE(
            zw_ctr_drbg_instantiate(&port, &d, entropy, has_perso ? perso : NULL), V[i].what);
        cavp_reseed(&d, reseed);
        TEST_ASSERT_TRUE_MESSAGE(zw_ctr_drbg_generate(&port, &d, out, sizeof out), V[i].what);
        TEST_ASSERT_TRUE_MESSAGE(zw_ctr_drbg_generate(&port, &d, out, sizeof out), V[i].what);
        uint8_t want[64];
        TEST_ASSERT_EQUAL_UINT_MESSAGE(64u, unhex(V[i].returned, want, sizeof want), V[i].what);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, out, sizeof out, V[i].what);
        zw_wipe(&d, sizeof d);
    }
}

/* ---- ZW-SEC-006 AES-OFB and S0's CBC-MAC -------------------------------------------------- */

/* ZW-SEC-006: NIST SP 800-38A F.4.1 OFB-AES128.Encrypt, four blocks; the same call decrypts,
 * in place too; a partial last block is truncated. */
static void test_ofb_sp800_38a(void)
{
    static const char CT[] =
        "3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed825"
        "9740051e9c5fecf64344f7a82260edcc304c6528f659c77866a510d9c1d6ae5e";
    uint8_t key[16], iv[16], pt[64], out[64];
    unhex(SP800_38A_KEY, key, sizeof key);
    unhex("000102030405060708090a0b0c0d0e0f", iv, sizeof iv);
    unhex(SP800_38A_PT, pt, sizeof pt);
    TEST_ASSERT_TRUE(zw_aes_ofb(&port, key, iv, pt, sizeof pt, out));
    expect_hex(CT, out, sizeof out);
    TEST_ASSERT_TRUE(zw_aes_ofb(&port, key, iv, out, sizeof out, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pt, out, sizeof pt);

    uint8_t ct[64];
    unhex(CT, ct, sizeof ct);
    memset(out, 0xEE, sizeof out);
    TEST_ASSERT_TRUE(zw_aes_ofb(&port, key, iv, pt, 21u, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ct, out, 21u);
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[21]);
    TEST_ASSERT_TRUE(zw_aes_ofb(&port, key, iv, NULL, 0u, NULL));
    TEST_ASSERT_FALSE(zw_aes_ofb(&port, key, iv, NULL, 1u, out));
    TEST_ASSERT_TRUE(all_zero(out, 1u));
}

/* ZW-SEC-006: S0's CBC-MAC on real traffic (security.md section 11, from Z-Wave JS's tests):
 * two whole blocks; an empty input is refused. */
static void test_s0_cbc_mac_real_traffic(void)
{
    uint8_t key[16], data[32], mac[8];
    unhex("c5fe1ca17d36c992731a0c0c468c1ef9", key, sizeof key);
    unhex("ddd360c382a437514392826cbba0b3128114010cf3fb762d6e82126681c18597", data, sizeof data);
    TEST_ASSERT_TRUE(zw_s0_cbc_mac(&port, key, data, sizeof data, mac));
    expect_hex("2bc20a8aa9bbb371", mac, sizeof mac);
    memset(mac, 0xEE, sizeof mac);
    TEST_ASSERT_FALSE(zw_s0_cbc_mac(&port, key, data, 0u, mac));
    TEST_ASSERT_TRUE(all_zero(mac, sizeof mac));
}

/* ---- ZW-SEC-007..011 security.md section 11 ------------------------------------------------ */

/* ZW-SEC-007: network key expansion of PNK 000102..0f. */
static void test_network_key_expansion(void)
{
    uint8_t pnk[16], key_ccm[16], pstring[32], key_mpan[16];
    unhex("000102030405060708090a0b0c0d0e0f", pnk, sizeof pnk);
    TEST_ASSERT_TRUE(zw_s2_network_key_expand(&port, pnk, key_ccm, pstring, key_mpan));
    expect_hex(S11_KEY_CCM, key_ccm, sizeof key_ccm);
    expect_hex("4a46b6e046c129bc72f737e57e9b5786", key_mpan, sizeof key_mpan);
    expect_hex(S11_PSTRING, pstring, sizeof pstring);
}

/* ZW-SEC-008: the temporary key - the panel's and the device's key pairs (RFC 7748 section 6.1's
 * Alice and Bob), the shared secret from both sides, PRK with the including node's (the
 * panel's) public key first on both sides, temp KeyCCM and temp PString. */
static void test_temporary_key(void)
{
    static const uint8_t base[32] = {9};
    uint8_t panel[32], device[32], panel_pub[32], device_pub[32], s_panel[32], s_device[32];
    uint8_t prk[16], prk_device[16], key_ccm[16], pstring[32];
    unhex(ALICE_PRIV, panel, sizeof panel);
    unhex(BOB_PRIV, device, sizeof device);
    TEST_ASSERT_TRUE(port.x25519(port.ctx, panel_pub, panel, base));
    TEST_ASSERT_TRUE(port.x25519(port.ctx, device_pub, device, base));
    expect_hex(ALICE_PUB, panel_pub, 32u);
    expect_hex(BOB_PUB, device_pub, 32u);
    TEST_ASSERT_TRUE(zw_s2_shared_secret(&port, panel, device_pub, s_panel));
    TEST_ASSERT_TRUE(zw_s2_shared_secret(&port, device, panel_pub, s_device));
    expect_hex(SHARED, s_panel, 32u);
    expect_hex(SHARED, s_device, 32u);

    TEST_ASSERT_TRUE(zw_s2_temp_key_extract(&port, s_panel, panel_pub, device_pub, prk));
    expect_hex("c7219805a4bd07bbe45915e8530c5975", prk, sizeof prk);
    TEST_ASSERT_TRUE(zw_s2_temp_key_extract(&port, s_device, panel_pub, device_pub, prk_device));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(prk, prk_device, sizeof prk);
    TEST_ASSERT_TRUE(zw_s2_temp_key_expand(&port, prk, key_ccm, pstring));
    expect_hex("7dd067ed9c4fa4e9db2ef5bb0ad9a7b2", key_ccm, sizeof key_ccm);
    expect_hex("d62e2993be086f2ad3aba4d45cef3418533fe70df5139984871b5cc2d6d2c6df", pstring,
               sizeof pstring);

    /* The order of the public keys matters: the joining node's first is another key. */
    TEST_ASSERT_TRUE(zw_s2_temp_key_extract(&port, s_panel, device_pub, panel_pub, prk_device));
    TEST_ASSERT_FALSE(zw_ct_equal(prk, prk_device, sizeof prk));
}

static const char SENDER_EI[] = "101112131415161718191a1b1c1d1e1f";
static const char RECEIVER_EI[] = "202122232425262728292a2b2c2d2e2f";

/* ZW-SEC-009: a SPAN - NoncePRK and MEI of the two entropy inputs, then nonces 1-3 from the
 * generator instantiated with MEI and the network key's PString, step by step and through
 * zw_s2_span_instantiate / zw_s2_span_next_nonce. */
static void test_span(void)
{
    uint8_t sei[16], rei[16], pstring[32], prk[16], mei[32], block[16], nonce[13];
    zw_ctr_drbg_t d;
    unhex(SENDER_EI, sei, sizeof sei);
    unhex(RECEIVER_EI, rei, sizeof rei);
    unhex(S11_PSTRING, pstring, sizeof pstring);
    TEST_ASSERT_TRUE(zw_s2_nonce_prk(&port, sei, rei, prk));
    expect_hex("91d5df52e46bb3fe8673a199e839cbc8", prk, sizeof prk);
    TEST_ASSERT_TRUE(zw_s2_mei(&port, prk, mei));
    expect_hex("f15b458d77005caa77e7ca5eb120e60793b1e2c78e751d71cd23eee80a8a83d6", mei,
               sizeof mei);

    static const char *const nonces[3] = {
        "f8648b2a31d81dea7bb3e02cef", "4122a17ea64ebab751c63fa29c", "edd75c79138b6bf058474a9193",
    };
    TEST_ASSERT_TRUE(zw_ctr_drbg_instantiate(&port, &d, mei, pstring));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(zw_ctr_drbg_generate(&port, &d, block, sizeof block));
        expect_hex(nonces[i], block, 13u);
    }
    TEST_ASSERT_TRUE(zw_s2_span_instantiate(&port, &d, sei, rei, pstring));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(zw_s2_span_next_nonce(&port, &d, nonce));
        expect_hex(nonces[i], nonce, sizeof nonce);
    }
    /* A call refused for want of an output draws nothing. */
    TEST_ASSERT_TRUE(zw_s2_span_instantiate(&port, &d, sei, rei, pstring));
    TEST_ASSERT_FALSE(zw_s2_span_next_nonce(&port, &d, NULL));
    TEST_ASSERT_TRUE(zw_s2_span_next_nonce(&port, &d, nonce));
    expect_hex(nonces[0], nonce, sizeof nonce);
    zw_wipe(&d, sizeof d);
}

/* Section 7's associated data: sender | destination | home ID (4) | message length (2) | seq |
 * flags | unencrypted extensions, with 1-byte node IDs. */
static size_t s2_aad(uint8_t *aad, uint8_t src, uint8_t dst, uint32_t home, uint16_t msg_len,
                     uint8_t seq, uint8_t flags, const uint8_t *ext, size_t ext_len)
{
    size_t n = 0;
    aad[n++] = src;
    aad[n++] = dst;
    for (int shift = 24; shift >= 0; shift -= 8) {
        aad[n++] = (uint8_t)(home >> shift);
    }
    aad[n++] = (uint8_t)(msg_len >> 8);
    aad[n++] = (uint8_t)msg_len;
    aad[n++] = seq;
    aad[n++] = flags;
    if (ext_len > 0) {
        memcpy(&aad[n], ext, ext_len);
    }
    return n + ext_len;
}

/* ZW-SEC-010: S2 Message Encapsulation from node 2 to the panel (node 1) in home C0FFEE01 with
 * the network key of ZW-SEC-007: the sender establishes the SPAN (its EI goes in the SPAN
 * extension 12 41 | EI), the AAD from section 7's layout, CCM of the Notification Report with
 * nonce 1, the frame 9F 03 | seq | flags | extension | ciphertext | tag; then the next frame
 * (seq 56, no extension, nonce 2, Battery 80 03 FF). The panel, with its own SPAN from the same
 * entropy inputs, decrypts both. */
static void test_s2_encapsulation(void)
{
    uint8_t pnk[16], key_ccm[16], pstring[32], key_mpan[16], sei[16], rei[16], nonce[13];
    uint8_t ext[18], aad[64], pt[16], frame[64], back[16];
    zw_ctr_drbg_t device, panel;
    unhex("000102030405060708090a0b0c0d0e0f", pnk, sizeof pnk);
    TEST_ASSERT_TRUE(zw_s2_network_key_expand(&port, pnk, key_ccm, pstring, key_mpan));
    unhex(SENDER_EI, sei, sizeof sei);
    unhex(RECEIVER_EI, rei, sizeof rei);
    TEST_ASSERT_TRUE(zw_s2_span_instantiate(&port, &device, sei, rei, pstring));
    TEST_ASSERT_TRUE(zw_s2_span_instantiate(&port, &panel, sei, rei, pstring));

    /* The first frame: SPAN extension (length 18, critical, type 1) with the sender's EI. */
    ext[0] = 18u;
    ext[1] = 0x41u;
    memcpy(&ext[2], sei, sizeof sei);
    const size_t pt_len = unhex(S11_INNER, pt, sizeof pt);
    const uint16_t msg_len = (uint16_t)(2u + 2u + sizeof ext + pt_len + 8u);
    TEST_ASSERT_EQUAL_UINT16(39u, msg_len);
    const size_t aad_len = s2_aad(aad, 2u, 1u, 0xC0FFEE01u, msg_len, 0x55u, 0x01u, ext, sizeof ext);
    expect_hex(S11_AAD, aad, aad_len);

    TEST_ASSERT_TRUE(zw_s2_span_next_nonce(&port, &device, nonce));
    expect_hex(S11_NONCE1, nonce, sizeof nonce);
    size_t n = 0;
    frame[n++] = 0x9Fu;
    frame[n++] = 0x03u;
    frame[n++] = 0x55u;
    frame[n++] = 0x01u;
    memcpy(&frame[n], ext, sizeof ext);
    n += sizeof ext;
    TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, key_ccm, nonce, aad, aad_len, pt, pt_len, &frame[n],
                                        &frame[n + pt_len]));
    expect_hex(S11_CT, &frame[n], pt_len);
    expect_hex(S11_TAG, &frame[n + pt_len], 8u);
    expect_hex("9f0355011241101112131415161718191a1b1c1d1e1fee50c2d601c41ad28502add758929a40ad",
               frame, n + pt_len + 8u);
    TEST_ASSERT_EQUAL_UINT(msg_len, n + pt_len + 8u);

    TEST_ASSERT_TRUE(zw_s2_span_next_nonce(&port, &panel, nonce));
    TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, key_ccm, nonce, aad, aad_len, &frame[n], pt_len,
                                        &frame[n + pt_len], back));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pt, back, pt_len);

    /* The next frame: seq 56, flags 00, no extension, nonce 2. */
    const uint8_t battery[3] = {0x80u, 0x03u, 0xFFu};
    const uint16_t len2 = (uint16_t)(2u + 2u + sizeof battery + 8u);
    const size_t aad2_len = s2_aad(aad, 2u, 1u, 0xC0FFEE01u, len2, 0x56u, 0x00u, NULL, 0u);
    TEST_ASSERT_TRUE(zw_s2_span_next_nonce(&port, &device, nonce));
    expect_hex("4122a17ea64ebab751c63fa29c", nonce, sizeof nonce);
    frame[0] = 0x9Fu;
    frame[1] = 0x03u;
    frame[2] = 0x56u;
    frame[3] = 0x00u;
    TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&port, key_ccm, nonce, aad, aad2_len, battery,
                                        sizeof battery, &frame[4], &frame[4 + sizeof battery]));
    expect_hex("9f035600c7d38ef47c7b1463532a4b", frame, len2);

    TEST_ASSERT_TRUE(zw_s2_span_next_nonce(&port, &panel, nonce));
    TEST_ASSERT_TRUE(zw_aes_ccm_decrypt(&port, key_ccm, nonce, aad, aad2_len, &frame[4],
                                        sizeof battery, &frame[4 + sizeof battery], back));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(battery, back, sizeof battery);
    zw_wipe(&device, sizeof device);
    zw_wipe(&panel, sizeof panel);
}

/* ZW-SEC-011: S0 from the panel (1) to node 2 - AuthKey and EncKey of the key 000102..0f and of
 * the all-zero inclusion key; ciphertext = OFB(EncKey, sender nonce | receiver nonce, frame
 * control 00 | Basic Get 20 02); MAC = CBC-MAC(AuthKey, sender nonce | receiver nonce | 81 | 01 |
 * 02 | 03 | ciphertext); the frame 98 81 | sender nonce | ciphertext | receiver nonce ID | MAC;
 * and decrypted back. */
static void test_s0_encapsulation(void)
{
    static const uint8_t zero_key[16] = {0};
    uint8_t key[16], auth[16], enc[16], sn[8], rn[8], iv[16], ct[3], mac_in[32], mac[8];
    uint8_t frame[32], back[3];
    const uint8_t inner[3] = {0x00u, 0x20u, 0x02u};
    unhex("000102030405060708090a0b0c0d0e0f", key, sizeof key);
    TEST_ASSERT_TRUE(zw_s0_keys(&port, key, auth, enc));
    expect_hex("a5548f94150184202f8200aaa9d02d56", auth, sizeof auth);
    expect_hex("fbd49b091a38f87ad5c3ee2580ba414c", enc, sizeof enc);
    uint8_t auth0[16], enc0[16];
    TEST_ASSERT_TRUE(zw_s0_keys(&port, zero_key, auth0, enc0));
    expect_hex("9adae054f63dfaff5ea18e45edf6ea6f", auth0, sizeof auth0);
    expect_hex("8522717d3ad1fbfeafa1ceaafdf56565", enc0, sizeof enc0);

    unhex("0102030405060708", sn, sizeof sn);
    unhex("a1a2a3a4a5a6a7a8", rn, sizeof rn);
    memcpy(iv, sn, 8);
    memcpy(&iv[8], rn, 8);
    TEST_ASSERT_TRUE(zw_aes_ofb(&port, enc, iv, inner, sizeof inner, ct));
    expect_hex("7ec72f", ct, sizeof ct);

    size_t n = 0;
    memcpy(&mac_in[n], sn, 8);
    n += 8u;
    memcpy(&mac_in[n], rn, 8);
    n += 8u;
    mac_in[n++] = 0x81u;             /* the command */
    mac_in[n++] = 0x01u;             /* source: the panel */
    mac_in[n++] = 0x02u;             /* destination */
    mac_in[n++] = (uint8_t)sizeof ct; /* ciphertext length */
    memcpy(&mac_in[n], ct, sizeof ct);
    n += sizeof ct;
    TEST_ASSERT_TRUE(zw_s0_cbc_mac(&port, auth, mac_in, n, mac));
    expect_hex("d55d5cac51d9831c", mac, sizeof mac);

    size_t f = 0;
    frame[f++] = 0x98u;
    frame[f++] = 0x81u;
    memcpy(&frame[f], sn, 8);
    f += 8u;
    memcpy(&frame[f], ct, sizeof ct);
    f += sizeof ct;
    frame[f++] = rn[0];
    memcpy(&frame[f], mac, sizeof mac);
    f += sizeof mac;
    expect_hex("988101020304050607087ec72fa1d55d5cac51d9831c", frame, f);

    TEST_ASSERT_TRUE(zw_aes_ofb(&port, enc, iv, ct, sizeof ct, back));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(inner, back, sizeof inner);
}

/* ---- ZW-008 the port failing -------------------------------------------------------------- */

/* Inputs for the calls below (values from the tests above). */
static struct {
    uint8_t k16[16], msg[64], iv[16], e32[32], p32[32], shared[32], priv[32], peer[32];
    uint8_t sei[16], rei[16];
    s11_frame_t frame;
} fx;

static void load_fixtures(void)
{
    unhex("000102030405060708090a0b0c0d0e0f", fx.k16, sizeof fx.k16);
    unhex(SP800_38A_PT, fx.msg, sizeof fx.msg);
    unhex("000102030405060708090a0b0c0d0e0f", fx.iv, sizeof fx.iv);
    unhex("f15b458d77005caa77e7ca5eb120e60793b1e2c78e751d71cd23eee80a8a83d6", fx.e32,
          sizeof fx.e32);
    unhex(S11_PSTRING, fx.p32, sizeof fx.p32);
    unhex(SHARED, fx.shared, sizeof fx.shared);
    unhex(ALICE_PRIV, fx.priv, sizeof fx.priv);
    unhex(BOB_PUB, fx.peer, sizeof fx.peer);
    unhex(SENDER_EI, fx.sei, sizeof fx.sei);
    unhex(RECEIVER_EI, fx.rei, sizeof fx.rei);
    load_s11_frame(&fx.frame);
}

/* One call of the API: its outputs land in out[0..n), a generator it uses in drbg. */
typedef struct {
    uint8_t out[80];
    size_t n;
    zw_ctr_drbg_t drbg;
} call_t;

typedef bool (*call_fn_t)(const zw_crypto_port_t *p, call_t *c);

static bool call_cmac(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 16u;
    return zw_aes_cmac(p, fx.k16, fx.msg, 40u, c->out);
}

static bool call_ccm_encrypt(const zw_crypto_port_t *p, call_t *c)
{
    c->n = S11_PT_LEN + 8u;
    return zw_aes_ccm_encrypt(p, fx.frame.key, fx.frame.nonce, fx.frame.aad, 28u, fx.frame.pt,
                              S11_PT_LEN, c->out, &c->out[S11_PT_LEN]);
}

static bool call_ccm_decrypt(const zw_crypto_port_t *p, call_t *c)
{
    c->n = S11_PT_LEN;
    return zw_aes_ccm_decrypt(p, fx.frame.key, fx.frame.nonce, fx.frame.aad, 28u, fx.frame.ct,
                              S11_PT_LEN, fx.frame.tag, c->out);
}

static bool call_ofb(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 40u;
    return zw_aes_ofb(p, fx.k16, fx.iv, fx.msg, 40u, c->out);
}

static bool call_cbc_mac(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 8u;
    return zw_s0_cbc_mac(p, fx.k16, fx.msg, 40u, c->out);
}

static bool call_drbg_instantiate(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 0u;
    return zw_ctr_drbg_instantiate(p, &c->drbg, fx.e32, fx.p32);
}

static bool call_drbg_generate(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 40u;
    return zw_ctr_drbg_generate(p, &c->drbg, c->out, 40u);
}

static bool call_network_key(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 64u;
    return zw_s2_network_key_expand(p, fx.k16, c->out, &c->out[16], &c->out[48]);
}

static bool call_temp_extract(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 16u;
    return zw_s2_temp_key_extract(p, fx.shared, fx.priv, fx.peer, c->out);
}

static bool call_temp_expand(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 48u;
    return zw_s2_temp_key_expand(p, fx.k16, c->out, &c->out[16]);
}

static bool call_nonce_prk(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 16u;
    return zw_s2_nonce_prk(p, fx.sei, fx.rei, c->out);
}

static bool call_mei(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 32u;
    return zw_s2_mei(p, fx.k16, c->out);
}

static bool call_span_instantiate(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 0u;
    return zw_s2_span_instantiate(p, &c->drbg, fx.sei, fx.rei, fx.p32);
}

static bool call_span_next(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 13u;
    return zw_s2_span_next_nonce(p, &c->drbg, c->out);
}

static bool call_keypair(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 64u;
    return zw_s2_keypair(p, c->out, &c->out[32]);
}

static bool call_shared(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 32u;
    return zw_s2_shared_secret(p, fx.priv, fx.peer, c->out);
}

static bool call_s0_keys(const zw_crypto_port_t *p, call_t *c)
{
    c->n = 32u;
    return zw_s0_keys(p, fx.k16, c->out, &c->out[16]);
}

static const struct {
    const char *name;
    call_fn_t fn;
} CALLS[] = {
    {"zw_aes_cmac", call_cmac},
    {"zw_aes_ccm_encrypt", call_ccm_encrypt},
    {"zw_aes_ccm_decrypt", call_ccm_decrypt},
    {"zw_aes_ofb", call_ofb},
    {"zw_s0_cbc_mac", call_cbc_mac},
    {"zw_ctr_drbg_instantiate", call_drbg_instantiate},
    {"zw_ctr_drbg_generate", call_drbg_generate},
    {"zw_s2_network_key_expand", call_network_key},
    {"zw_s2_temp_key_extract", call_temp_extract},
    {"zw_s2_temp_key_expand", call_temp_expand},
    {"zw_s2_nonce_prk", call_nonce_prk},
    {"zw_s2_mei", call_mei},
    {"zw_s2_span_instantiate", call_span_instantiate},
    {"zw_s2_span_next_nonce", call_span_next},
    {"zw_s2_keypair", call_keypair},
    {"zw_s2_shared_secret", call_shared},
    {"zw_s0_keys", call_s0_keys},
};

/* The state a call starts from: outputs full of junk, a generator in a known state. */
static void prepare(call_t *c)
{
    memset(c->out, 0xEE, sizeof c->out);
    c->n = 0u;
    memset(c->drbg.key, 0x11, sizeof c->drbg.key);
    memset(c->drbg.v, 0x22, sizeof c->drbg.v);
}

/* What a failed call must leave: its outputs zeroed, nothing past them touched, the generator
 * as it was. */
static void expect_failed_cleanly(const call_t *c, const char *name)
{
    call_t start;
    prepare(&start);
    TEST_ASSERT_TRUE_MESSAGE(all_zero(c->out, c->n), name);
    for (size_t i = c->n; i < sizeof c->out; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xEE, c->out[i], name);
    }
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&start.drbg, &c->drbg, sizeof c->drbg, name);
}

static bool fail_aes(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    (void)ctx;
    (void)key;
    (void)in;
    memset(out, 0xA5, 16);
    return false;
}

static bool fail_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32])
{
    (void)ctx;
    (void)scalar;
    (void)point;
    memset(out, 0xA5, 32);
    return false;
}

static bool fail_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    memset(buf, 0xA5, len);
    return false;
}

/* ZW-008: every function returns false when the port fails, zeroes its outputs and leaves a
 * generator as it was: with a port whose calls all fail, with no port at all, and with a port
 * that fails exactly one call - for each call the function makes, after which it makes no
 * other. */
static void test_port_failures(void)
{
    const zw_crypto_port_t failing = {NULL, fail_aes, fail_x25519, fail_random};
    call_t c;
    load_fixtures();
    for (size_t i = 0; i < sizeof CALLS / sizeof CALLS[0]; i++) {
        const char *name = CALLS[i].name;

        zw_host_crypto_init(&hc, 1u);
        prepare(&c);
        TEST_ASSERT_TRUE_MESSAGE(CALLS[i].fn(&port, &c), name);
        const uint32_t calls = hc.calls;
        TEST_ASSERT_TRUE_MESSAGE(calls > 0u, name);

        for (uint32_t k = 0; k < calls; k++) {
            zw_host_crypto_fail_at(&hc, k);
            prepare(&c);
            TEST_ASSERT_FALSE_MESSAGE(CALLS[i].fn(&port, &c), name);
            expect_failed_cleanly(&c, name);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(k + 1u, hc.calls, name);
        }

        prepare(&c);
        TEST_ASSERT_FALSE_MESSAGE(CALLS[i].fn(&failing, &c), name);
        expect_failed_cleanly(&c, name);
        prepare(&c);
        TEST_ASSERT_FALSE_MESSAGE(CALLS[i].fn(NULL, &c), name);
        expect_failed_cleanly(&c, name);
    }
}

/* ZW-008: a port missing the primitive a function needs is refused. */
static void test_port_missing_primitives(void)
{
    zw_crypto_port_t no_x25519 = port, no_random = port;
    no_x25519.x25519 = NULL;
    no_random.random = NULL;
    uint8_t priv[32], pub[32], s[32];
    load_fixtures();
    TEST_ASSERT_FALSE(zw_s2_keypair(&no_x25519, priv, pub));
    TEST_ASSERT_FALSE(zw_s2_keypair(&no_random, priv, pub));
    TEST_ASSERT_TRUE(all_zero(priv, sizeof priv) && all_zero(pub, sizeof pub));
    TEST_ASSERT_FALSE(zw_s2_shared_secret(&no_x25519, fx.priv, fx.peer, s));
    TEST_ASSERT_TRUE(all_zero(s, sizeof s));
}

/* ---- ZW-008 the port's own check ---------------------------------------------------------- */

/* A port with one fault: the PC port behind it, one kind of answer spoiled. */
typedef enum {
    FAULT_NONE = 0,
    FAULT_AES,           /* one bit of every AES block wrong */
    FAULT_X25519,        /* X25519 wrong for any point but the base point */
    FAULT_X25519_BASE,   /* X25519 wrong for the base point (public keys) alone */
    FAULT_RANDOM_STUCK,  /* the same bytes every draw */
    FAULT_RANDOM_ZERO,   /* all zero */
    FAULT_RANDOM_FIRST,  /* the first draw all zero */
    FAULT_RANDOM_SECOND, /* the second draw all zero */
    FAULT_RANDOM_SILENT  /* says yes, writes nothing */
} fault_t;

static fault_t fault;
static unsigned draws;

static bool faulty_aes(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    (void)ctx;
    const bool ok = port.aes128_encrypt(port.ctx, key, in, out);
    if (fault == FAULT_AES) {
        out[15] ^= 0x01u;
    }
    return ok;
}

static bool faulty_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32],
                          const uint8_t point[32])
{
    (void)ctx;
    const bool ok = port.x25519(port.ctx, out, scalar, point);
    const bool base = point[0] == 9u && all_zero(&point[1], 31u);
    if ((fault == FAULT_X25519 && !base) || (fault == FAULT_X25519_BASE && base)) {
        out[0] ^= 0x40u;
    }
    return ok;
}

static bool faulty_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    draws++;
    switch (fault) {
    case FAULT_RANDOM_STUCK: memset(buf, 0x42, len); return true;
    case FAULT_RANDOM_ZERO: memset(buf, 0, len); return true;
    case FAULT_RANDOM_SILENT: return true;
    case FAULT_RANDOM_FIRST:
    case FAULT_RANDOM_SECOND:
        if (draws == (fault == FAULT_RANDOM_FIRST ? 1u : 2u)) {
            memset(buf, 0, len);
            return true;
        }
        return port.random(port.ctx, buf, len);
    default: return port.random(port.ctx, buf, len);
    }
}

/* ZW-008: the port's self-test at start (zw_crypto_port_check) - the PC port passes: FIPS-197
 * C.1, RFC 7748 section 5.2's first vector and section 6.1's Alice from the base point, two
 * different random draws; five calls, no more. A wrong AES, an X25519 wrong on either vector, a
 * random source stuck at one value, all zero, zero on either draw or writing nothing is refused;
 * so is a failing call (each of the five, after which it makes no other), a missing primitive
 * and no port. */
static void test_port_check(void)
{
    TEST_ASSERT_TRUE(zw_crypto_port_check(&port));
    const uint32_t calls = hc.calls;
    TEST_ASSERT_EQUAL_UINT32(5u, calls); /* one AES block, two X25519, two draws */
    for (uint32_t k = 0; k < calls; k++) {
        zw_host_crypto_fail_at(&hc, k);
        TEST_ASSERT_FALSE(zw_crypto_port_check(&port));
        TEST_ASSERT_EQUAL_UINT32(k + 1u, hc.calls);
    }
    zw_host_crypto_fail_at(&hc, ZW_HOST_NEVER);

    const zw_crypto_port_t faulty = {NULL, faulty_aes, faulty_x25519, faulty_random};
    static const struct {
        fault_t fault;
        const char *name;
    } FAULTS[] = {
        {FAULT_AES, "a wrong AES"},
        {FAULT_X25519, "an X25519 wrong on RFC 7748 5.2"},
        {FAULT_X25519_BASE, "an X25519 wrong on the base point"},
        {FAULT_RANDOM_STUCK, "a stuck random source"},
        {FAULT_RANDOM_ZERO, "an all-zero random source"},
        {FAULT_RANDOM_FIRST, "a first draw all zero"},
        {FAULT_RANDOM_SECOND, "a second draw all zero"},
        {FAULT_RANDOM_SILENT, "a random source writing nothing"},
    };
    fault = FAULT_NONE;
    draws = 0u;
    TEST_ASSERT_TRUE(zw_crypto_port_check(&faulty)); /* the wrapper alone spoils nothing */
    for (size_t i = 0; i < sizeof FAULTS / sizeof FAULTS[0]; i++) {
        fault = FAULTS[i].fault;
        draws = 0u;
        TEST_ASSERT_FALSE_MESSAGE(zw_crypto_port_check(&faulty), FAULTS[i].name);
    }

    zw_crypto_port_t missing = port;
    missing.aes128_encrypt = NULL;
    TEST_ASSERT_FALSE(zw_crypto_port_check(&missing));
    missing = port;
    missing.x25519 = NULL;
    TEST_ASSERT_FALSE(zw_crypto_port_check(&missing));
    missing = port;
    missing.random = NULL;
    TEST_ASSERT_FALSE(zw_crypto_port_check(&missing));
    TEST_ASSERT_FALSE(zw_crypto_port_check(NULL));
}

/* ZW-008: zw_wipe zeroes exactly the bytes it is given. */
static void test_wipe(void)
{
    uint8_t buf[8];
    memset(buf, 0xEE, sizeof buf);
    zw_wipe(&buf[1], 6u);
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[0]);
    TEST_ASSERT_TRUE(all_zero(&buf[1], 6u));
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[7]);
    zw_wipe(NULL, 0u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_aes_fips197_c1);
    RUN_TEST(test_aes_sp800_38a_ecb);
    RUN_TEST(test_cmac_rfc4493);
    RUN_TEST(test_cmac_refuses_bad_arguments);
    RUN_TEST(test_ccm_rfc3610_packets);
    RUN_TEST(test_ccm_edge_lengths);
    RUN_TEST(test_ccm_longest_lengths);
    RUN_TEST(test_ccm_refuses_tampering);
    RUN_TEST(test_ccm_in_place);
    RUN_TEST(test_ccm_refuses_bad_arguments);
    RUN_TEST(test_ct_equal);
    RUN_TEST(test_x25519_rfc7748_vectors);
    RUN_TEST(test_x25519_rfc7748_iterated);
    RUN_TEST(test_x25519_rfc7748_ecdh);
    RUN_TEST(test_shared_secret_refuses_small_order_points);
    RUN_TEST(test_keypair);
    RUN_TEST(test_ctr_drbg_multi_block);
    RUN_TEST(test_ctr_drbg_counter_carries);
    RUN_TEST(test_ctr_drbg_failure_keeps_state);
    RUN_TEST(test_ctr_drbg_nist_cavp);
    RUN_TEST(test_ofb_sp800_38a);
    RUN_TEST(test_s0_cbc_mac_real_traffic);
    RUN_TEST(test_network_key_expansion);
    RUN_TEST(test_temporary_key);
    RUN_TEST(test_span);
    RUN_TEST(test_s2_encapsulation);
    RUN_TEST(test_s0_encapsulation);
    RUN_TEST(test_port_failures);
    RUN_TEST(test_port_missing_primitives);
    RUN_TEST(test_port_check);
    RUN_TEST(test_wipe);
    return UNITY_END();
}
