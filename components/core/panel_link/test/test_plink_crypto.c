/*
 * test_plink_crypto.c - the panel link's cryptography (components/core/panel_link plink_crypto.h,
 * docs/features/11 "Pairing", "Sessions"): SHA-256 in pieces (alarm_proto's streaming SHA-256),
 * HMAC-SHA256, HKDF-SHA256, X25519 with the all-zero check, AES-128-CCM through the port, random
 * numbers, constant-time comparison and wiping, and the start-up self-check.
 *
 * The standards' vectors (RFC 4231, RFC 5869, RFC 7748, RFC 3610, FIPS 180-2) were typed from the
 * published texts and recomputed with Python's `cryptography` (OpenSSL) by vectors_plink_crypto.py
 * beside this file, which also computed the vectors no standard gives (the 200-byte message, the
 * keys around the block size, HKDF's longest output, the top-bit forms of the low-order points).
 *
 * Requirements: LINK-003 (AES-128-CCM with an 8-byte tag for every data frame), LINK-004 (pairing:
 * SHA-256 commitments and code, HMAC confirmations, HKDF keys, X25519 with the all-zero answer
 * refused), LINK-015 (the handshake: ephemeral X25519, HKDF session keys).
 */
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_sha256.h"
#include "panel_link/plink_crypto.h"
#include "plink_test_port.h"
#include "unity.h"

static plink_test_port_t tp;
static plink_crypto_t port;

void setUp(void)
{
    plink_test_port_init(&tp, 0x11C0FFEEu);
    port = plink_test_port(&tp);
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
        out[n++] = (uint8_t)((unsigned)hi << 4 | (unsigned)lo);
        p += 2;
    }
    return n;
}

/* The n bytes at got are exactly the hex text. */
static void expect_hex(const char *hex, const uint8_t *got, size_t n)
{
    uint8_t want[256];
    const size_t len = unhex(hex, want, sizeof want);
    TEST_ASSERT_EQUAL_UINT(len, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, n);
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

static plink_iov_t iov(const void *p, size_t len)
{
    const plink_iov_t v = {p, len};
    return v;
}

/* ---- SHA-256 in pieces -------------------------------------------------------------------- */

/* The streaming test's message: byte i = 7 i + 3 (mod 256), 200 bytes (3 blocks and a part). */
static void pattern200(uint8_t msg[200])
{
    for (size_t i = 0; i < 200u; i++) {
        msg[i] = (uint8_t)(7u * i + 3u);
    }
}

static const char SHA_PATTERN200[] =
    "2c7e18c942ef065b526a2d4e5546283749cd3ddfb51d8fc71f42717363685f46";

/* LINK-004: the streaming SHA-256 (proto_sha256.h) gives the one-shot digest whatever the pieces:
 * every split of a 200-byte message into two and into three pieces (empty pieces included, every
 * block boundary crossed), and one byte at a time - and both equal the digest Python computed. */
static void test_sha256_streaming_every_split(void)
{
    uint8_t msg[200], want[32], got[32];
    pattern200(msg);
    proto_sha256(msg, sizeof msg, want);
    expect_hex(SHA_PATTERN200, want, sizeof want);

    proto_sha256_ctx_t s;
    for (size_t i = 0; i <= sizeof msg; i++) {
        for (size_t j = i; j <= sizeof msg; j++) {
            proto_sha256_init(&s);
            proto_sha256_update(&s, msg, i);
            proto_sha256_update(&s, &msg[i], j - i);
            proto_sha256_update(&s, &msg[j], sizeof msg - j);
            proto_sha256_final(&s, got);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, sizeof got);
        }
    }
    proto_sha256_init(&s);
    for (size_t i = 0; i < sizeof msg; i++) {
        proto_sha256_update(&s, &msg[i], 1u);
    }
    proto_sha256_update(&s, NULL, 0u); /* an empty piece may be NULL */
    proto_sha256_final(&s, got);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, sizeof got);
}

/* LINK-004: plink_sha256 over pieces (transcripts are never copied into one buffer): FIPS 180-2's
 * "abc" as "a", nothing, "bc"; the empty message with no pieces at all; the 200-byte message in
 * pieces of 1, 63, 0 (NULL) and 136 bytes. */
static void test_sha256_pieces(void)
{
    uint8_t out[32];
    const plink_iov_t abc[] = {iov("a", 1u), iov("", 0u), iov("bc", 2u)};
    plink_sha256(abc, 3u, out);
    expect_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", out, 32u);

    plink_sha256(NULL, 0u, out);
    expect_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", out, 32u);

    uint8_t msg[200];
    pattern200(msg);
    const plink_iov_t parts[] = {iov(msg, 1u), iov(&msg[1], 63u), iov(NULL, 0u),
                                 iov(&msg[64], 136u)};
    plink_sha256(parts, 4u, out);
    expect_hex(SHA_PATTERN200, out, 32u);
}

/* ---- HMAC-SHA256 -------------------------------------------------------------------------- */

static const struct {
    int tc;
    const char *key;  /* hex */
    const char *data; /* hex, or text when text is set */
    bool text;
    const char *mac; /* 32 bytes, or 16 for case 5 (truncated) */
} RFC4231[] = {
    {1, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "Hi There", true,
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
    {2, "4a656665", "what do ya want for nothing?", true,
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
    {3, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
     "dddddddddd",
     false, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
    {4, "0102030405060708090a0b0c0d0e0f10111213141516171819",
     "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
     "cdcdcdcdcd",
     false, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
    {5, "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c", "Test With Truncation", true,
     "a3b6167473100ee06e0c796c2955552b"},
    {6,
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "Test Using Larger Than Block-Size Key - Hash Key First", true,
     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
    {7,
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "This is a test using a larger than block-size key and a larger than block-size data. The "
     "key needs to be hashed before being used by the HMAC algorithm.",
     true, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
};

/* LINK-004, LINK-015: RFC 4231's test cases 1-7 through plink_hmac - a 4-byte key, 20-byte keys,
 * a 25-byte key and 131-byte keys (hashed first), short and long data; case 5 is cut to 128 bits
 * as the link's MACs are. Every case is also hashed with its data split at every byte: the
 * pieces don't change the MAC. */
static void test_hmac_rfc4231(void)
{
    for (size_t v = 0; v < sizeof RFC4231 / sizeof RFC4231[0]; v++) {
        uint8_t key[160], data[200], want[32], mac[32];
        const size_t key_len = unhex(RFC4231[v].key, key, sizeof key);
        size_t len;
        if (RFC4231[v].text) {
            len = strlen(RFC4231[v].data);
            memcpy(data, RFC4231[v].data, len);
        } else {
            len = unhex(RFC4231[v].data, data, sizeof data);
        }
        const size_t mac_len = unhex(RFC4231[v].mac, want, sizeof want);
        const plink_iov_t whole = iov(data, len);
        plink_hmac(key, key_len, &whole, 1u, mac);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, mac, mac_len, "RFC 4231 test case");
        for (size_t cut = 0; cut <= len; cut++) {
            const plink_iov_t two[] = {iov(data, cut), iov(&data[cut], len - cut)};
            plink_hmac(key, key_len, two, 2u, mac);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, mac, mac_len, "RFC 4231, data split");
        }
    }
}

/* LINK-004: keys around SHA-256's 64-byte block - 63 and 64 bytes are padded, 65 is hashed
 * first - and the empty key (NULL), which is HashLen zeros once padded, as HKDF's default salt
 * needs; values computed with Python's hmac (vectors_plink_crypto.py). The output may be the
 * key's own buffer. */
static void test_hmac_key_lengths(void)
{
    static const char *const want[] = {
        "b248985dc380d2caa80ea29cfddb8451781e453bd247b552c323728a90e6bad0", /* 0 */
        "be0ae1960b34e4a739a48d0ee9d5ca40c8c7e9277c9716ded8687c465c430a91", /* 63 */
        "6447d78a8266c9071aace275e0881ea0d9a55c4f9456b71ae41b01a326017eae", /* 64 */
        "9f2fbcc8556e5f53646e72e0a4479985aad444770888f4184c744d17e4840d41", /* 65 */
    };
    static const size_t lens[] = {0u, 63u, 64u, 65u};
    const plink_iov_t data = iov("plink v1 hmac", 13u);
    uint8_t key[65], mac[32];
    for (size_t i = 0; i < 4u; i++) {
        memset(key, 0xa5, sizeof key);
        plink_hmac(lens[i] == 0u ? NULL : key, lens[i], &data, 1u, mac);
        expect_hex(want[i], mac, sizeof mac);
    }
    uint8_t zeros[32] = {0}, again[32];
    plink_hmac(zeros, sizeof zeros, &data, 1u, again);
    expect_hex(want[0], again, sizeof again);

    /* In place: the MAC written over its 32-byte key. */
    uint8_t k[32];
    memset(k, 0, sizeof k);
    plink_hmac(k, sizeof k, &data, 1u, k);
    expect_hex(want[0], k, sizeof k);
}

/* ---- HKDF-SHA256 -------------------------------------------------------------------------- */

static void fill_range(uint8_t *p, size_t n, unsigned first)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(first + i);
    }
}

/* LINK-004, LINK-015: RFC 5869 appendix A.1-A.3 - Extract, then Expand to 42 or 82 bytes. A.2's
 * 80-byte salt, IKM and info cross a block; A.3 has no salt (NULL: HashLen zeros) and no info
 * (no pieces). A.2's info also goes in three pieces. */
static void test_hkdf_rfc5869(void)
{
    uint8_t ikm[80], salt[80], info[80], prk[32], okm[82];

    /* A.1 */
    memset(ikm, 0x0b, 22u);
    fill_range(salt, 13u, 0x00u);
    fill_range(info, 10u, 0xf0u);
    plink_iov_t in = iov(ikm, 22u), inf = iov(info, 10u);
    plink_hkdf_extract(salt, 13u, &in, 1u, prk);
    expect_hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk, 32u);
    TEST_ASSERT_TRUE(plink_hkdf_expand(prk, &inf, 1u, okm, 42u));
    expect_hex(
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", okm,
        42u);

    /* A.2 */
    fill_range(ikm, 80u, 0x00u);
    fill_range(salt, 80u, 0x60u);
    fill_range(info, 80u, 0xb0u);
    in = iov(ikm, 80u);
    plink_hkdf_extract(salt, 80u, &in, 1u, prk);
    expect_hex("06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244", prk, 32u);
    static const char A2_OKM[] = "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
                                 "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
                                 "cc30c58179ec3e87c14c01d5c1f3434f1d87";
    inf = iov(info, 80u);
    TEST_ASSERT_TRUE(plink_hkdf_expand(prk, &inf, 1u, okm, 82u));
    expect_hex(A2_OKM, okm, 82u);
    const plink_iov_t three[] = {iov(info, 5u), iov(&info[5], 0u), iov(&info[5], 75u)};
    memset(okm, 0, sizeof okm);
    TEST_ASSERT_TRUE(plink_hkdf_expand(prk, three, 3u, okm, 82u));
    expect_hex(A2_OKM, okm, 82u);

    /* A.3 */
    memset(ikm, 0x0b, 22u);
    in = iov(ikm, 22u);
    plink_hkdf_extract(NULL, 0u, &in, 1u, prk);
    expect_hex("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", prk, 32u);
    TEST_ASSERT_TRUE(plink_hkdf_expand(prk, NULL, 0u, okm, 42u));
    expect_hex(
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", okm,
        42u);
}

/* LINK-015: HKDF-Expand's range - 255 x 32 bytes is the longest (its last block computed with
 * Python's cryptography), one more is refused with the output zeroed, so is 0 and a missing
 * PRK; the output may be the PRK's own buffer (the session's keys are expanded from a PRK held in
 * the same scratch). */
static void test_hkdf_expand_limits(void)
{
    static uint8_t big[255u * 32u + 1u];
    uint8_t prk[32], info[10];
    unhex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk, sizeof prk);
    fill_range(info, sizeof info, 0xf0u);
    const plink_iov_t inf = iov(info, sizeof info);

    TEST_ASSERT_TRUE(plink_hkdf_expand(prk, &inf, 1u, big, 255u * 32u));
    expect_hex("76a3f78bcffe95fecf91923c22ad6ee64d48a6d1b981d7e523d5c0f22154ee88",
               &big[255u * 32u - 32u], 32u);

    memset(big, 0xEE, sizeof big);
    TEST_ASSERT_FALSE(plink_hkdf_expand(prk, &inf, 1u, big, sizeof big));
    TEST_ASSERT_TRUE(all_zero(big, sizeof big));
    memset(big, 0xEE, 32u);
    TEST_ASSERT_FALSE(plink_hkdf_expand(prk, &inf, 1u, big, 0u));
    TEST_ASSERT_EQUAL_HEX8(0xEE, big[0]); /* nothing to zero */
    TEST_ASSERT_FALSE(plink_hkdf_expand(NULL, &inf, 1u, big, 32u));
    TEST_ASSERT_TRUE(all_zero(big, 32u));

    /* In place over the PRK: A.1's first 32 bytes of OKM. */
    uint8_t buf[42];
    memcpy(buf, prk, sizeof prk);
    TEST_ASSERT_TRUE(plink_hkdf_expand(buf, &inf, 1u, buf, sizeof buf));
    expect_hex(
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", buf,
        sizeof buf);
}

/* ---- X25519 ------------------------------------------------------------------------------- */

static const char ALICE_PRIV[] = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
static const char ALICE_PUB[] = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
static const char BOB_PRIV[] = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
static const char BOB_PUB[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
static const char SHARED[] = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

/* LINK-004, LINK-015: RFC 7748 section 5.2's two vectors through plink_x25519_shared (the second
 * u-coordinate has its top bit set, which X25519 ignores). */
static void test_x25519_rfc7748_5_2(void)
{
    uint8_t k[32], u[32], out[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, sizeof k);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, sizeof u);
    TEST_ASSERT_TRUE(plink_x25519_shared(&port, k, u, out));
    expect_hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", out, 32u);
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k, sizeof k);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u, sizeof u);
    TEST_ASSERT_TRUE(plink_x25519_shared(&port, k, u, out));
    expect_hex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", out, 32u);
}

/* A port whose random numbers are given bytes: plink_x25519_keypair then makes a known key. */
static uint8_t given[32];

static bool given_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT(sizeof given, len);
    memcpy(buf, given, len);
    return true;
}

/* LINK-004, LINK-015: RFC 7748 section 6.1 - a key pair's public key is X25519(private, 9):
 * Alice's and Bob's from their private keys, and both reach the same shared secret. */
static void test_x25519_rfc7748_6_1(void)
{
    plink_crypto_t fixed = port;
    fixed.random = given_random;
    uint8_t a[32], apub[32], b[32], bpub[32], s[32];

    unhex(ALICE_PRIV, given, sizeof given);
    TEST_ASSERT_TRUE(plink_x25519_keypair(&fixed, a, apub));
    expect_hex(ALICE_PRIV, a, 32u);
    expect_hex(ALICE_PUB, apub, 32u);
    unhex(BOB_PRIV, given, sizeof given);
    TEST_ASSERT_TRUE(plink_x25519_keypair(&fixed, b, bpub));
    expect_hex(BOB_PUB, bpub, 32u);

    TEST_ASSERT_TRUE(plink_x25519_shared(&port, a, bpub, s));
    expect_hex(SHARED, s, 32u);
    TEST_ASSERT_TRUE(plink_x25519_shared(&port, b, apub, s));
    expect_hex(SHARED, s, 32u);
}

/* LINK-004, LINK-015: a peer key of small order gives an all-zero secret whatever the private
 * key (RFC 7748 section 6.1), and plink_x25519_shared refuses it with its output zeroed: u = 0,
 * 1, the two points of order 8, p - 1, p (0 again), p + 1 (1 again) and the order-8 points with
 * their ignored top bit set - each refused by OpenSSL too (vectors_plink_crypto.py) - with
 * Alice's key and with random ones. */
static void test_x25519_low_order_refused(void)
{
    static const char *const low[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
    };
    uint8_t a[32], u[32], s[32], raw[32], pub[32];
    for (size_t i = 0; i < sizeof low / sizeof low[0]; i++) {
        unhex(low[i], u, sizeof u);
        unhex(ALICE_PRIV, a, sizeof a);
        TEST_ASSERT_TRUE(port.x25519(port.ctx, raw, a, u));          /* the primitive answers ... */
        TEST_ASSERT_TRUE_MESSAGE(all_zero(raw, sizeof raw), low[i]); /* ... all zero */
        memset(s, 0xEE, sizeof s);
        TEST_ASSERT_FALSE_MESSAGE(plink_x25519_shared(&port, a, u, s), low[i]);
        TEST_ASSERT_TRUE(all_zero(s, sizeof s));
        for (int k = 0; k < 3; k++) {
            TEST_ASSERT_TRUE(plink_x25519_keypair(&port, a, pub));
            memset(s, 0xEE, sizeof s);
            TEST_ASSERT_FALSE_MESSAGE(plink_x25519_shared(&port, a, u, s), low[i]);
            TEST_ASSERT_TRUE(all_zero(s, sizeof s));
        }
    }
}

/* LINK-004, LINK-015: key pairs from the port are fresh (two differ) and agree on a secret; a
 * failing draw or X25519 refuses the pair with both halves zeroed; a failing agreement, a missing
 * X25519 or no port refuses the secret, zeroed. */
static void test_x25519_keypairs_and_failures(void)
{
    uint8_t a[32], apub[32], b[32], bpub[32], s1[32], s2[32];
    TEST_ASSERT_TRUE(plink_x25519_keypair(&port, a, apub));
    TEST_ASSERT_TRUE(plink_x25519_keypair(&port, b, bpub));
    TEST_ASSERT_FALSE(plink_ct_equal(a, b, 32u));
    TEST_ASSERT_FALSE(plink_ct_equal(apub, bpub, 32u));
    TEST_ASSERT_TRUE(plink_x25519_shared(&port, a, bpub, s1));
    TEST_ASSERT_TRUE(plink_x25519_shared(&port, b, apub, s2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s1, s2, 32u);

    for (uint32_t call = 0; call < 2u; call++) { /* the draw, then X25519 */
        plink_test_port_fail_at(&tp, call);
        memset(a, 0xEE, sizeof a);
        memset(apub, 0xEE, sizeof apub);
        TEST_ASSERT_FALSE(plink_x25519_keypair(&port, a, apub));
        TEST_ASSERT_TRUE(all_zero(a, sizeof a) && all_zero(apub, sizeof apub));
        TEST_ASSERT_EQUAL_UINT32(call + 1u, tp.hc.calls); /* nothing after the failure */
    }
    plink_test_port_fail_at(&tp, 0u);
    memset(s1, 0xEE, sizeof s1);
    TEST_ASSERT_FALSE(plink_x25519_shared(&port, b, bpub, s1));
    TEST_ASSERT_TRUE(all_zero(s1, sizeof s1));
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);

    plink_crypto_t broken = port;
    broken.x25519 = NULL;
    memset(s1, 0xEE, sizeof s1);
    TEST_ASSERT_FALSE(plink_x25519_shared(&broken, b, apub, s1));
    TEST_ASSERT_TRUE(all_zero(s1, sizeof s1));
    TEST_ASSERT_FALSE(plink_x25519_keypair(&broken, a, apub));
    broken = port;
    broken.random = NULL;
    TEST_ASSERT_FALSE(plink_x25519_keypair(&broken, a, apub));
    TEST_ASSERT_FALSE(plink_x25519_keypair(NULL, a, apub));
    TEST_ASSERT_FALSE(plink_x25519_shared(NULL, b, apub, s1));
}

/* ---- AES-128-CCM -------------------------------------------------------------------------- */

typedef struct {
    uint8_t key[16], nonce[13], aad[8], pt[23], ct[23], tag[8];
} ccm1_t;

/* RFC 3610 packet vector #1: 8 bytes of header, 23 of message, M = 8, L = 2. */
static void load_ccm1(ccm1_t *v)
{
    unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", v->key, sizeof v->key);
    unhex("00000003020100a0a1a2a3a4a5", v->nonce, sizeof v->nonce);
    fill_range(v->aad, sizeof v->aad, 0x00u);
    fill_range(v->pt, sizeof v->pt, 0x08u);
    unhex("588c979a61c663d2f066d0c2c0f989806d5f6b61dac384", v->ct, sizeof v->ct);
    unhex("17e8d12cfdf926e0", v->tag, sizeof v->tag);
}

/* The vector opens only as it is: after flipping bit `bit` of byte i of field f it is refused,
 * with the output zeroed; the flip is then undone. */
static void expect_refused_after_flips(ccm1_t *v, uint8_t *field, size_t n)
{
    uint8_t pt[23];
    for (size_t i = 0; i < n; i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            field[i] ^= (uint8_t)(1u << bit);
            memset(pt, 0xEE, sizeof pt);
            TEST_ASSERT_FALSE(plink_ccm_open(&port, v->key, v->nonce, v->aad, sizeof v->aad, v->ct,
                                             sizeof v->ct, v->tag, pt));
            TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
            field[i] ^= (uint8_t)(1u << bit);
        }
    }
}

/* LINK-003: RFC 3610 packet vector #1 through plink_ccm_seal (the ciphertext and the 8-byte tag),
 * opened back by plink_ccm_open, also in place; every bit of the ciphertext, tag, associated data,
 * nonce and key flipped is refused with nothing decrypted left; a short associated data or
 * message is refused too. */
static void test_ccm_rfc3610_packet1(void)
{
    ccm1_t v;
    load_ccm1(&v);
    uint8_t ct[23], tag[8], pt[23];
    TEST_ASSERT_TRUE(
        plink_ccm_seal(&port, v.key, v.nonce, v.aad, sizeof v.aad, v.pt, sizeof v.pt, ct, tag));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v.ct, ct, sizeof ct);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v.tag, tag, sizeof tag);
    TEST_ASSERT_TRUE(
        plink_ccm_open(&port, v.key, v.nonce, v.aad, sizeof v.aad, v.ct, sizeof v.ct, v.tag, pt));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v.pt, pt, sizeof pt);

    /* In place: one buffer, the message turned into the ciphertext and back. */
    uint8_t buf[23];
    memcpy(buf, v.pt, sizeof buf);
    TEST_ASSERT_TRUE(
        plink_ccm_seal(&port, v.key, v.nonce, v.aad, sizeof v.aad, buf, sizeof buf, buf, tag));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v.ct, buf, sizeof buf);
    TEST_ASSERT_TRUE(
        plink_ccm_open(&port, v.key, v.nonce, v.aad, sizeof v.aad, buf, sizeof buf, tag, buf));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v.pt, buf, sizeof buf);

    expect_refused_after_flips(&v, v.ct, sizeof v.ct);
    expect_refused_after_flips(&v, v.tag, sizeof v.tag);
    expect_refused_after_flips(&v, v.aad, sizeof v.aad);
    expect_refused_after_flips(&v, v.nonce, sizeof v.nonce);
    expect_refused_after_flips(&v, v.key, sizeof v.key);
    memset(pt, 0xEE, sizeof pt);
    TEST_ASSERT_FALSE(plink_ccm_open(&port, v.key, v.nonce, v.aad, sizeof v.aad - 1u, v.ct,
                                     sizeof v.ct, v.tag, pt));
    TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
    TEST_ASSERT_FALSE(plink_ccm_open(&port, v.key, v.nonce, v.aad, sizeof v.aad, v.ct,
                                     sizeof v.ct - 1u, v.tag, pt));
    /* Still whole after all that: every flip was undone. */
    TEST_ASSERT_TRUE(
        plink_ccm_open(&port, v.key, v.nonce, v.aad, sizeof v.aad, v.ct, sizeof v.ct, v.tag, pt));
}

/* LINK-003: a failing AES call while sealing leaves nothing to send (ciphertext and tag zeroed),
 * whichever call it is; no port, or one without AES, is refused the same way. */
static void test_ccm_failures(void)
{
    ccm1_t v;
    load_ccm1(&v);
    uint8_t ct[23], tag[8];
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);
    TEST_ASSERT_TRUE(
        plink_ccm_seal(&port, v.key, v.nonce, v.aad, sizeof v.aad, v.pt, sizeof v.pt, ct, tag));
    const uint32_t calls = tp.hc.calls;
    TEST_ASSERT_EQUAL_UINT32(7u, calls); /* B_0, the header block, 2 x 2 message blocks, A_0 */
    for (uint32_t k = 0; k < calls; k++) {
        plink_test_port_fail_at(&tp, k);
        memset(ct, 0xEE, sizeof ct);
        memset(tag, 0xEE, sizeof tag);
        TEST_ASSERT_FALSE(
            plink_ccm_seal(&port, v.key, v.nonce, v.aad, sizeof v.aad, v.pt, sizeof v.pt, ct, tag));
        TEST_ASSERT_TRUE(all_zero(ct, sizeof ct) && all_zero(tag, sizeof tag));
    }
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);

    plink_crypto_t no_aes = port;
    no_aes.aes128_encrypt = NULL;
    memset(ct, 0xEE, sizeof ct);
    memset(tag, 0xEE, sizeof tag);
    TEST_ASSERT_FALSE(
        plink_ccm_seal(&no_aes, v.key, v.nonce, v.aad, sizeof v.aad, v.pt, sizeof v.pt, ct, tag));
    TEST_ASSERT_TRUE(all_zero(ct, sizeof ct) && all_zero(tag, sizeof tag));
    memset(ct, 0xEE, sizeof ct);
    memset(tag, 0xEE, sizeof tag);
    TEST_ASSERT_FALSE(
        plink_ccm_seal(NULL, v.key, v.nonce, v.aad, sizeof v.aad, v.pt, sizeof v.pt, ct, tag));
    TEST_ASSERT_TRUE(all_zero(ct, sizeof ct) && all_zero(tag, sizeof tag));
    uint8_t pt[23];
    memset(pt, 0xEE, sizeof pt);
    TEST_ASSERT_FALSE(
        plink_ccm_open(NULL, v.key, v.nonce, v.aad, sizeof v.aad, v.ct, sizeof v.ct, v.tag, pt));
    TEST_ASSERT_TRUE(all_zero(pt, sizeof pt));
}

/* ---- random, comparison, wiping ----------------------------------------------------------- */

/* LINK-004, LINK-015: random bytes come from the port (two draws differ); a failing draw is
 * refused with the buffer zeroed; nothing to draw is fine; no generator is refused. */
static void test_random(void)
{
    uint8_t a[16], b[16];
    TEST_ASSERT_TRUE(plink_random(&port, a, sizeof a));
    TEST_ASSERT_TRUE(plink_random(&port, b, sizeof b));
    TEST_ASSERT_FALSE(plink_ct_equal(a, b, sizeof a));
    plink_test_port_fail_at(&tp, 0u);
    memset(a, 0xEE, sizeof a);
    TEST_ASSERT_FALSE(plink_random(&port, a, sizeof a));
    TEST_ASSERT_TRUE(all_zero(a, sizeof a));
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);
    TEST_ASSERT_TRUE(plink_random(&port, NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, tp.hc.calls); /* nothing drawn for nothing */
    plink_crypto_t none = port;
    none.random = NULL;
    memset(a, 0xEE, sizeof a);
    TEST_ASSERT_FALSE(plink_random(&none, a, sizeof a));
    TEST_ASSERT_TRUE(all_zero(a, sizeof a));
    TEST_ASSERT_FALSE(plink_random(NULL, a, sizeof a));
    TEST_ASSERT_FALSE(plink_random(&port, NULL, 4u));
}

/* LINK-003, LINK-004: MACs, tags and commitments are compared by plink_ct_equal (every byte read,
 * a difference in any byte or bit found), and secrets wiped by plink_wipe (exactly the bytes
 * given). */
static void test_ct_equal_and_wipe(void)
{
    uint8_t a[32], b[32];
    fill_range(a, sizeof a, 1u);
    memcpy(b, a, sizeof b);
    TEST_ASSERT_TRUE(plink_ct_equal(a, b, sizeof a));
    for (size_t i = 0; i < sizeof b; i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            b[i] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_FALSE(plink_ct_equal(a, b, sizeof a));
            TEST_ASSERT_TRUE(plink_ct_equal(a, b, i)); /* the bytes before are still equal */
            b[i] ^= (uint8_t)(1u << bit);
        }
    }
    TEST_ASSERT_TRUE(plink_ct_equal(NULL, NULL, 0u));
    TEST_ASSERT_FALSE(plink_ct_equal(a, NULL, 1u));

    uint8_t buf[8];
    memset(buf, 0xEE, sizeof buf);
    plink_wipe(&buf[1], 6u);
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[0]);
    TEST_ASSERT_TRUE(all_zero(&buf[1], 6u));
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[7]);
    plink_wipe(NULL, 0u);
    plink_wipe(NULL, 5u);
}

/* ---- the start-up self-check -------------------------------------------------------------- */

/* A port with one fault: the test port behind it, one kind of answer spoiled. */
typedef enum {
    FAULT_NONE = 0,
    FAULT_AES,          /* one bit of every AES block wrong */
    FAULT_X25519,       /* X25519 wrong for any point but the base point */
    FAULT_X25519_BASE,  /* X25519 wrong for the base point (public keys) alone */
    FAULT_RANDOM_STUCK, /* the same bytes every draw */
} fault_t;

static fault_t fault;

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
    if (fault == FAULT_RANDOM_STUCK) {
        memset(buf, 0x42, len);
        return true;
    }
    return port.random(port.ctx, buf, len);
}

/* LINK-003, LINK-004, LINK-015: the self-check the firmware runs before the link starts
 * (plink_crypto_check) passes on the test port - FIPS-197 C.1, RFC 7748 section 5.2 and 6.1, RFC
 * 3610 packet #1 sealed, RFC 4231 case 2, RFC 5869 A.1, two random draws - in 13 port calls, and
 * leaves the port's nonce watch clean (it seals once and never opens). It fails at every one of
 * those calls failing, making no call after; with a wrong AES, an X25519 wrong on either kind of
 * point, a stuck random source; with a primitive missing; with no port. */
static void test_crypto_check(void)
{
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);
    TEST_ASSERT_TRUE(plink_crypto_check(&port));
    TEST_ASSERT_FALSE(tp.reused);
    const uint32_t calls = tp.hc.calls;
    /* 1 AES block, 2 X25519 and 2 draws (zwave_host's check), 1 X25519 agreement, 7 AES (CCM). */
    TEST_ASSERT_EQUAL_UINT32(13u, calls);
    for (uint32_t k = 0; k < calls; k++) {
        plink_test_port_fail_at(&tp, k);
        TEST_ASSERT_FALSE(plink_crypto_check(&port));
        TEST_ASSERT_EQUAL_UINT32(k + 1u, tp.hc.calls);
    }
    plink_test_port_fail_at(&tp, ZW_HOST_NEVER);

    const plink_crypto_t faulty = {NULL, faulty_aes, faulty_x25519, faulty_random, true};
    static const struct {
        fault_t fault;
        const char *name;
    } FAULTS[] = {
        {FAULT_AES, "a wrong AES"},
        {FAULT_X25519, "an X25519 wrong on other points"},
        {FAULT_X25519_BASE, "an X25519 wrong on the base point"},
        {FAULT_RANDOM_STUCK, "a stuck random source"},
    };
    fault = FAULT_NONE;
    TEST_ASSERT_TRUE(plink_crypto_check(&faulty)); /* the wrapper alone spoils nothing */
    for (size_t i = 0; i < sizeof FAULTS / sizeof FAULTS[0]; i++) {
        fault = FAULTS[i].fault;
        TEST_ASSERT_FALSE_MESSAGE(plink_crypto_check(&faulty), FAULTS[i].name);
    }
    fault = FAULT_NONE;

    plink_crypto_t missing = port;
    missing.aes128_encrypt = NULL;
    TEST_ASSERT_FALSE(plink_crypto_check(&missing));
    missing = port;
    missing.x25519 = NULL;
    TEST_ASSERT_FALSE(plink_crypto_check(&missing));
    missing = port;
    missing.random = NULL;
    TEST_ASSERT_FALSE(plink_crypto_check(&missing));
    TEST_ASSERT_FALSE(plink_crypto_check(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha256_streaming_every_split);
    RUN_TEST(test_sha256_pieces);
    RUN_TEST(test_hmac_rfc4231);
    RUN_TEST(test_hmac_key_lengths);
    RUN_TEST(test_hkdf_rfc5869);
    RUN_TEST(test_hkdf_expand_limits);
    RUN_TEST(test_x25519_rfc7748_5_2);
    RUN_TEST(test_x25519_rfc7748_6_1);
    RUN_TEST(test_x25519_low_order_refused);
    RUN_TEST(test_x25519_keypairs_and_failures);
    RUN_TEST(test_ccm_rfc3610_packet1);
    RUN_TEST(test_ccm_failures);
    RUN_TEST(test_random);
    RUN_TEST(test_ct_equal_and_wipe);
    RUN_TEST(test_crypto_check);
    return UNITY_END();
}
