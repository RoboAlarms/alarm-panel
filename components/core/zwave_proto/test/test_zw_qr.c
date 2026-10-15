/*
 * test_zw_qr.c - the SmartStart QR code, SHA-1 and the DSK helpers SmartStart uses
 * (docs/features/36 network.md §4.2, security.md §4; ZW-024).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_dsk.h"
#include "zwave_proto/zw_qr.h"
#include "zwave_proto/zw_sha1.h"

void setUp(void) {}
void tearDown(void) {}

/* A label's code, its checksum checked independently (Python hashlib) before it was used here. */
static const char LABEL[] =
    "900132782003515253545541424344453132333435212223242500100435301537022065520001000000300578";

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        snprintf(&out[i * 2u], 3u, "%02x", (unsigned)b[i]);
    }
}

/* A code with a correct checksum around 'tail' (requested keys + DSK + blocks). */
static void make_code(unsigned version, const char *tail, char *out, size_t cap)
{
    uint8_t h[20];
    zw_sha1(tail, strlen(tail), h);
    snprintf(out, cap, "90%02u%05u%s", version, ((unsigned)h[0] << 8) | (unsigned)h[1], tail);
}

#define DSK_DIGITS "5152535455414243444531323334352122232425"
#define PRODUCT "0010" "0435301537" "0220" "65520001000000300578"

/* ZW-024: SHA-1 against FIPS 180 examples */
static void test_sha1_vectors(void)
{
    char s[41];
    uint8_t h[20];
    zw_sha1("abc", 3u, h);
    hex(h, 20u, s);
    TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", s);
    zw_sha1("", 0u, h);
    hex(h, 20u, s);
    TEST_ASSERT_EQUAL_STRING("da39a3ee5e6b4b0d3255bfef95601890afd80709", s);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    zw_sha1(two, strlen(two), h);
    hex(h, 20u, s);
    TEST_ASSERT_EQUAL_STRING("84983e441c3bd26ebaae4aa1f95129e5e54670f1", s);
    zw_sha1_t st;
    zw_sha1_init(&st);
    char a[1000];
    memset(a, 'a', sizeof a);
    for (int i = 0; i < 1000; i++) {
        zw_sha1_update(&st, a, sizeof a);
    }
    zw_sha1_final(&st, h);
    hex(h, 20u, s);
    TEST_ASSERT_EQUAL_STRING("34aa973cd4c4daa4f61eeb2bdbad27316534016f", s);
}

/* ZW-024: a label's SmartStart code, every field */
static void test_label_code(void)
{
    zw_qr_t q;
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(LABEL, &q));
    TEST_ASSERT_EQUAL_UINT8(ZW_QR_VERSION_SMARTSTART, q.version);
    TEST_ASSERT_EQUAL_HEX8(0x03, q.requested_keys);
    char dsk[ZW_DSK_TEXT_SIZE];
    TEST_ASSERT_TRUE(zw_dsk_to_text(q.dsk, dsk, sizeof dsk));
    TEST_ASSERT_EQUAL_STRING("51525-35455-41424-34445-31323-33435-21222-32425", dsk);
    TEST_ASSERT_EQUAL_HEX8(0x11, q.generic);
    TEST_ASSERT_EQUAL_HEX8(0x01, q.specific);
    TEST_ASSERT_EQUAL_HEX16(0x0601, q.icon);
    TEST_ASSERT_EQUAL_HEX16(0xFFF0, q.manufacturer);
    TEST_ASSERT_EQUAL_HEX16(0x0064, q.product_type);
    TEST_ASSERT_EQUAL_HEX16(0x0003, q.product_id);
    TEST_ASSERT_EQUAL_HEX16(0x0242, q.app_version);
    TEST_ASSERT_EQUAL_HEX8(ZW_QR_PROTO_CLASSIC, q.protocols);
    TEST_ASSERT_FALSE(q.has_uuid);
    TEST_ASSERT_FALSE(q.has_max_interval);
}

/* ZW-024: a scanner's or a paste's whitespace is removed first; nothing else is */
static void test_whitespace(void)
{
    char spaced[160];
    snprintf(spaced, sizeof spaced, "  %.20s \r\n%.30s\t%s\n", LABEL, &LABEL[20], &LABEL[50]);
    zw_qr_t q;
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(spaced, &q));
    TEST_ASSERT_EQUAL_HEX16(0xFFF0, q.manufacturer);
    char dashed[160];
    snprintf(dashed, sizeof dashed, "%.20s-%s", LABEL, &LABEL[20]);
    TEST_ASSERT_EQUAL(ZW_QR_NOT_ZWAVE, zw_qr_parse(dashed, &q));
    TEST_ASSERT_EQUAL_HEX16(0, q.manufacturer); /* a refusal leaves it zeroed */
}

/* ZW-024: damaged, foreign, short and long codes are refused, each for its reason */
static void test_refusals(void)
{
    zw_qr_t q;
    char bad[160];
    strcpy(bad, LABEL);
    bad[40] = bad[40] == '9' ? '8' : '9';
    TEST_ASSERT_EQUAL(ZW_QR_CHECKSUM, zw_qr_parse(bad, &q));
    TEST_ASSERT_EQUAL(ZW_QR_NOT_ZWAVE, zw_qr_parse("https://example.com/", &q));
    TEST_ASSERT_EQUAL(ZW_QR_NOT_ZWAVE, zw_qr_parse("", &q));
    TEST_ASSERT_EQUAL(ZW_QR_NOT_ZWAVE, zw_qr_parse(NULL, &q));
    TEST_ASSERT_EQUAL(ZW_QR_NOT_ZWAVE, zw_qr_parse("1201327820035152535455", &q));
    TEST_ASSERT_EQUAL(ZW_QR_TOO_SHORT, zw_qr_parse("9", &q));
    TEST_ASSERT_EQUAL(ZW_QR_TOO_SHORT, zw_qr_parse("900132782003515253545541424344", &q));
    char big[ZW_QR_MAX_DIGITS + 8u];
    memset(big, '9', sizeof big - 1u);
    big[0] = '9';
    big[1] = '0';
    big[sizeof big - 1u] = '\0';
    TEST_ASSERT_EQUAL(ZW_QR_TOO_LONG, zw_qr_parse(big, &q));

    char code[200];
    make_code(2u, "003" DSK_DIGITS PRODUCT, code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_VERSION, zw_qr_parse(code, &q));
    make_code(1u, "003" DSK_DIGITS "0010" "0435301537", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_MISSING, zw_qr_parse(code, &q)); /* no product ID */
    make_code(1u, "003" DSK_DIGITS "0220" "65520001000000300578", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_MISSING, zw_qr_parse(code, &q)); /* no product type */
    make_code(1u, "003" "99999" "35455414243444531323334352122232425" PRODUCT, code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_BAD_NUMBER, zw_qr_parse(code, &q)); /* a DSK group > 65535 */
    make_code(1u, "003" DSK_DIGITS PRODUCT "0005123", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_TOO_SHORT, zw_qr_parse(code, &q)); /* a block longer than the rest */
    make_code(1u, "003" DSK_DIGITS PRODUCT "00", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_TOO_SHORT, zw_qr_parse(code, &q)); /* a block header cut short */
    make_code(1u, "003" DSK_DIGITS "0009" "043530153" "0220" "65520001000000300578", code,
              sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_BAD_NUMBER, zw_qr_parse(code, &q)); /* product type of 9 digits */
    make_code(1u, "999" DSK_DIGITS PRODUCT, code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_BAD_NUMBER, zw_qr_parse(code, &q)); /* requested keys > 255 */
}

/* ZW-024: unknown blocks: non-critical skipped, critical refuses the code; the optional blocks */
static void test_blocks(void)
{
    zw_qr_t q;
    char code[260];
    make_code(1u, "003" DSK_DIGITS PRODUCT "9804" "1234", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(code, &q)); /* type 49, not critical */
    make_code(1u, "003" DSK_DIGITS PRODUCT "9904" "1234", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_CRITICAL, zw_qr_parse(code, &q)); /* type 49, critical */
    make_code(1u, "003" DSK_DIGITS PRODUCT "0802" "03" "0402" "10", code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(code, &q));
    TEST_ASSERT_EQUAL_HEX8(ZW_QR_PROTO_CLASSIC | ZW_QR_PROTO_LONG_RANGE, q.protocols);
    TEST_ASSERT_TRUE(q.has_max_interval);
    TEST_ASSERT_EQUAL_UINT16(10u * 128u, q.max_interval_s);
    make_code(1u,
              "003" DSK_DIGITS PRODUCT "0642" "02" "00001000020000300004000050000600007"
              "00008",
              code, sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(code, &q));
    TEST_ASSERT_TRUE(q.has_uuid);
    TEST_ASSERT_EQUAL_UINT8(2u, q.uuid_format);
    TEST_ASSERT_EQUAL_HEX8(0x01, q.uuid[1]);
    TEST_ASSERT_EQUAL_HEX8(0x08, q.uuid[15]);
    /* a known block marked critical is read as usual */
    make_code(0u, "135" DSK_DIGITS "0110" "0435301537" "0220" "65520001000000300578", code,
              sizeof code);
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(code, &q));
    TEST_ASSERT_EQUAL_UINT8(ZW_QR_VERSION_S2, q.version);
    TEST_ASSERT_EQUAL_HEX8(135, q.requested_keys);
}

/* ZW-024: the DSK typed from a label, with or without separators */
static void test_dsk_digits(void)
{
    uint8_t d[16];
    uint8_t want[16];
    TEST_ASSERT_TRUE(zw_dsk_from_text("51525-35455-41424-34445-31323-33435-21222-32425", want));
    TEST_ASSERT_TRUE(zw_dsk_from_digits(DSK_DIGITS, d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 16);
    memset(d, 0, sizeof d);
    TEST_ASSERT_TRUE(zw_dsk_from_digits("51525 35455 41424-34445 3132333435-2122232425", d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 16);
    uint8_t keep[16];
    memset(keep, 0xAA, sizeof keep);
    memcpy(d, keep, sizeof d);
    TEST_ASSERT_FALSE(zw_dsk_from_digits("515253545541424344453132333435212223242", d));
    TEST_ASSERT_FALSE(zw_dsk_from_digits(DSK_DIGITS "1", d));
    TEST_ASSERT_FALSE(zw_dsk_from_digits("6553" "6" "35455414243444531323334352122232425", d));
    TEST_ASSERT_FALSE(zw_dsk_from_digits("51525.35455414243444531323334352122232425", d));
    TEST_ASSERT_FALSE(zw_dsk_from_digits(NULL, d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(keep, d, 16);
}

/* ZW-024: SmartStart's home IDs (security.md §4) */
static void test_home_ids(void)
{
    uint8_t dsk[16];
    for (size_t i = 0; i < 16u; i++) {
        dsk[i] = (uint8_t)(0x11u * i);
    }
    dsk[8] = 0x01; /* bits 7-6 clear: the NWI sets them */
    dsk[11] = 0xFF; /* bit 0 set: the NWI clears it */
    dsk[12] = 0xFF; /* the Auth clears bits 7-6 */
    dsk[15] = 0x00; /* the Auth sets bit 0 */
    uint8_t nwi[4], auth[4];
    zw_dsk_home_ids(dsk, nwi, auth);
    const uint8_t want_nwi[4] = {0xC1, 0x99, 0xAA, 0xFE};
    const uint8_t want_auth[4] = {0x3F, 0xDD, 0xEE, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_nwi, nwi, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_auth, auth, 4);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha1_vectors);
    RUN_TEST(test_label_code);
    RUN_TEST(test_whitespace);
    RUN_TEST(test_refusals);
    RUN_TEST(test_blocks);
    RUN_TEST(test_dsk_digits);
    RUN_TEST(test_home_ids);
    return UNITY_END();
}
