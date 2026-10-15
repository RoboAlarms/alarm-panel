/*
 * test_zbz_codes.c - install codes (zbz_codes.h; docs/features/20 devices.md §8, testing.md §9):
 * the CRC, what may be typed or scanned, the codes waiting and their stored blob. ZB-015.
 */
#include <string.h>

#include "codes_fixture_v1.h"
#include "unity.h"
#include "zigbee_zones/zbz_codes.h"

#define IEEE_1 0x02ABCD0000000001ull
#define IEEE_2 0x02ABCD0000000002ull

/* The Zigbee specification's example code and its CRC (low byte first). */
static const uint8_t SPEC_CODE[18] = {0x83, 0xFE, 0xD3, 0x40, 0x7A, 0x93, 0x97, 0x23, 0xA5,
                                      0xC6, 0x39, 0xB2, 0x69, 0x16, 0xD5, 0x05, 0xC3, 0xB5};
static const uint8_t CODE_6[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x5A, 0x60};

void setUp(void) {}
void tearDown(void) {}

/* ZB-015: CRC-16/X.25 - the catalogue's check value, and the specification's example code. */
static void test_crc(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x906E, zbz_code_crc((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX16(0xB5C3, zbz_code_crc(SPEC_CODE, 16));
    TEST_ASSERT_EQUAL_HEX16(0x0000, zbz_code_crc(NULL, 0)); /* the xors cancel */
}

/* ZB-015: a code is typed in either case, in groups, with spaces, dashes or colons. */
static void test_code_parse(void)
{
    uint8_t out[ZBZ_CODE_MAX];
    uint8_t len = 0;
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK,
                      zbz_code_parse("83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5", out, &len));
    TEST_ASSERT_EQUAL_UINT8(18, len);
    TEST_ASSERT_EQUAL_MEMORY(SPEC_CODE, out, 18);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK,
                      zbz_code_parse("83fed340-7a939723-a5c639b2-6916d505-c3b5", out, &len));
    TEST_ASSERT_EQUAL_MEMORY(SPEC_CODE, out, 18);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_code_parse("11:22:33:44:55:66:5A:60", out, &len));
    TEST_ASSERT_EQUAL_UINT8(8, len);
    TEST_ASSERT_EQUAL_MEMORY(CODE_6, out, 8);
}

/* ZB-015: a typo is CHECKSUM; the wrong length, an odd digit or a letter beyond F is BAD_LENGTH,
 * and nothing is written on a refusal. */
static void test_code_refusals(void)
{
    uint8_t out[ZBZ_CODE_MAX];
    uint8_t len = 0x55;
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL(ZBZ_CODE_CHECKSUM,
                      zbz_code_parse("83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B4", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_CHECKSUM, zbz_code_parse("1122334455675A60", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse("112233445566778899AABBCC", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse("1122334455665A6", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse("1122334455665A6G", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse("", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH,
                      zbz_code_parse("83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5 00", out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse(NULL, out, &len));
    TEST_ASSERT_EQUAL_UINT8(0x55, len);
    TEST_ASSERT_EACH_EQUAL_HEX8(0xEE, out, sizeof(out));
}

/* ZB-015: an address is 16 hex digits, separators and a 0x ignored; zero and all-F refused. */
static void test_ieee_parse(void)
{
    uint64_t ieee = 0;
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_ieee_parse("02AB:CD00:0000:0001", &ieee));
    TEST_ASSERT_TRUE(ieee == IEEE_1);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_ieee_parse(" 0x02abcd0000000002", &ieee));
    TEST_ASSERT_TRUE(ieee == IEEE_2);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_ieee_parse("02-AB-CD-00-00-00-00-01", &ieee));
    TEST_ASSERT_TRUE(ieee == IEEE_1);
    ieee = 7;
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_ieee_parse("02ABCD000000001", &ieee));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_ieee_parse("02ABCD00000000011", &ieee));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_ieee_parse("0000000000000000", &ieee));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_ieee_parse("FFFFFFFFFFFFFFFF", &ieee));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_ieee_parse("02ABCD000000000Z", &ieee));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_ieee_parse(NULL, &ieee));
    TEST_ASSERT_TRUE(ieee == 7u);
}

/* ZB-015: a scanned label - the Zigbee 3.0 form with other fields and a %... tail, either case of
 * its keys, and a bare address and code. */
static void test_qr_parse(void)
{
    uint64_t ieee = 0;
    uint8_t out[ZBZ_CODE_MAX];
    uint8_t len = 0;
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK,
                      zbz_code_parse_qr("Z:02ABCD0000000001$I:83FED3407A939723A5C639B26916D505C3B5"
                                        "%G$M:EXAMPLE",
                                        &ieee, out, &len));
    TEST_ASSERT_TRUE(ieee == IEEE_1);
    TEST_ASSERT_EQUAL_UINT8(18, len);
    TEST_ASSERT_EQUAL_MEMORY(SPEC_CODE, out, 18);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK,
                      zbz_code_parse_qr("M:EXAMPLE$i:1122334455665A60$z:02ABCD0000000002", &ieee,
                                        out, &len));
    TEST_ASSERT_TRUE(ieee == IEEE_2);
    TEST_ASSERT_EQUAL_MEMORY(CODE_6, out, 8);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK,
                      zbz_code_parse_qr("02ABCD0000000001 83FE D340 7A93 9723 A5C6 39B2 6916 D505 "
                                        "C3B5",
                                        &ieee, out, &len));
    TEST_ASSERT_TRUE(ieee == IEEE_1);
    TEST_ASSERT_EQUAL_MEMORY(SPEC_CODE, out, 18);
}

/* ZB-015: a label that isn't an install code, or a damaged one, is refused and leaves the
 * address alone. */
static void test_qr_refusals(void)
{
    uint64_t ieee = 9;
    uint8_t out[ZBZ_CODE_MAX];
    uint8_t len = 0;
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse_qr("WIFI:S:Harbor House;T:WPA;;", &ieee,
                                                             out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_code_parse_qr("Z:02ABCD0000000001", &ieee, out,
                                                             &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS,
                      zbz_code_parse_qr("Z:0000000000000000$I:1122334455665A60", &ieee, out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS,
                      zbz_code_parse_qr("Z:02ABCD00000001$I:1122334455665A60", &ieee, out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_CHECKSUM,
                      zbz_code_parse_qr("Z:02ABCD0000000001$I:1122334455665A61", &ieee, out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH,
                      zbz_code_parse_qr("02ABCD0000000001", &ieee, out, &len));
    TEST_ASSERT_EQUAL(ZBZ_CODE_CHECKSUM,
                      zbz_code_parse_qr("02ABCD0000000001 1122334455665A61", &ieee, out, &len));
    TEST_ASSERT_TRUE(ieee == 9u);
}

/* ZB-015: codes wait until used: a second code for an address replaces the first, at most 16
 * wait, and only a checked code for a usable address is kept. */
static void test_codes_add(void)
{
    zbz_codes_t c;
    zbz_codes_init(&c);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_1, CODE_6, 8, 100));
    TEST_ASSERT_TRUE(c.dirty);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_1, SPEC_CODE, 18, 200));
    TEST_ASSERT_EQUAL_size_t(1, c.count);
    const zbz_code_t *e = zbz_codes_find(&c, IEEE_1);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(18, e->len);
    TEST_ASSERT_EQUAL_MEMORY(SPEC_CODE, e->code, 18);
    TEST_ASSERT_EQUAL_UINT32(200, e->added_at);

    uint8_t bad[8];
    memcpy(bad, CODE_6, sizeof(bad));
    bad[7] ^= 1u;
    TEST_ASSERT_EQUAL(ZBZ_CODE_CHECKSUM, zbz_codes_add(&c, IEEE_2, bad, 8, 0));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_codes_add(&c, IEEE_2, CODE_6, 6, 0));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_LENGTH, zbz_codes_add(&c, IEEE_2, NULL, 8, 0));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_codes_add(&c, 0, CODE_6, 8, 0));
    TEST_ASSERT_EQUAL(ZBZ_CODE_BAD_ADDRESS, zbz_codes_add(&c, UINT64_MAX, CODE_6, 8, 0));
    TEST_ASSERT_EQUAL_size_t(1, c.count);

    for (uint64_t i = 2; i <= ZBZ_CODES_MAX; i++) {
        TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_1 + i - 1u, CODE_6, 8, 0));
    }
    TEST_ASSERT_EQUAL_size_t(ZBZ_CODES_MAX, c.count);
    TEST_ASSERT_EQUAL(ZBZ_CODE_FULL, zbz_codes_add(&c, IEEE_1 + 100u, CODE_6, 8, 0));
    /* A waiting address still takes a new code when full. */
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_2, SPEC_CODE, 18, 5));
}

/* ZB-015: a code is forgotten when its device joins or by the installer; the others keep their
 * order and no copy of it is left in the table. */
static void test_codes_forget(void)
{
    zbz_codes_t c;
    zbz_codes_init(&c);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_1, SPEC_CODE, 18, 1));
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_2, CODE_6, 8, 2));
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(&c, IEEE_2 + 1u, CODE_6, 8, 3));
    c.dirty = false;
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_forget(&c, IEEE_1));
    TEST_ASSERT_TRUE(c.dirty);
    TEST_ASSERT_EQUAL_size_t(2, c.count);
    TEST_ASSERT_TRUE(c.codes[0].ieee == IEEE_2);
    TEST_ASSERT_TRUE(c.codes[1].ieee == IEEE_2 + 1u);
    TEST_ASSERT_EACH_EQUAL_HEX8(0, (const uint8_t *)&c.codes[2], sizeof(c.codes[2]));
    TEST_ASSERT_NULL(zbz_codes_find(&c, IEEE_1));
    TEST_ASSERT_EQUAL(ZBZ_CODE_NOT_FOUND, zbz_codes_forget(&c, IEEE_1));
}

static void fill_fixture_content(zbz_codes_t *c)
{
    zbz_codes_init(c);
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(c, IEEE_1, SPEC_CODE, 18, 1790000000u));
    TEST_ASSERT_EQUAL(ZBZ_CODE_OK, zbz_codes_add(c, IEEE_2, CODE_6, 8, 0));
}

/* ZB-015, ZB-020: the frozen v1 blob reads field by field, and today's encoder writes the same
 * bytes for the same codes. */
static void test_codes_frozen_fixture(void)
{
    zbz_codes_t c;
    TEST_ASSERT_TRUE(zbz_codes_decode(&c, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1)));
    TEST_ASSERT_FALSE(c.dirty);
    TEST_ASSERT_EQUAL_size_t(2, c.count);
    TEST_ASSERT_TRUE(c.codes[0].ieee == IEEE_1);
    TEST_ASSERT_EQUAL_UINT8(18, c.codes[0].len);
    TEST_ASSERT_EQUAL_MEMORY(SPEC_CODE, c.codes[0].code, 18);
    TEST_ASSERT_EQUAL_UINT32(1790000000u, c.codes[0].added_at);
    TEST_ASSERT_TRUE(c.codes[1].ieee == IEEE_2);
    TEST_ASSERT_EQUAL_UINT8(8, c.codes[1].len);
    TEST_ASSERT_EQUAL_MEMORY(CODE_6, c.codes[1].code, 8);
    TEST_ASSERT_EQUAL_UINT32(0, c.codes[1].added_at);

    zbz_codes_t same;
    fill_fixture_content(&same);
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    size_t n = zbz_codes_encode(&same, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(CODES_FIXTURE_V1), n);
    TEST_ASSERT_EQUAL_MEMORY(CODES_FIXTURE_V1, buf, n);
}

/* ZB-015: a full table round-trips within ZBZ_CODES_BLOB_MAX; a buffer too small writes nothing;
 * an empty table is the two-byte header. */
static void test_codes_round_trip(void)
{
    zbz_codes_t c, back;
    zbz_codes_init(&c);
    for (uint64_t i = 0; i < ZBZ_CODES_MAX; i++) {
        TEST_ASSERT_EQUAL(ZBZ_CODE_OK,
                          zbz_codes_add(&c, IEEE_1 + i, SPEC_CODE, 18, (uint32_t)(1000u + i)));
    }
    uint8_t buf[ZBZ_CODES_BLOB_MAX];
    size_t n = zbz_codes_encode(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(ZBZ_CODES_BLOB_MAX, n);
    TEST_ASSERT_TRUE(zbz_codes_decode(&back, buf, n));
    TEST_ASSERT_EQUAL_size_t(ZBZ_CODES_MAX, back.count);
    TEST_ASSERT_EQUAL_MEMORY(&c.codes, &back.codes, sizeof(c.codes));
    TEST_ASSERT_EQUAL_size_t(0, zbz_codes_encode(&c, buf, n - 1u));

    zbz_codes_init(&c);
    TEST_ASSERT_EQUAL_size_t(2, zbz_codes_encode(&c, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(zbz_codes_decode(&back, buf, 2));
    TEST_ASSERT_EQUAL_size_t(0, back.count);
}

/* ZB-015, ZB-020: a damaged blob or one of a later compat reads as no codes, never as half of
 * them: every truncation, a stray byte, the wrong compat, too many entries, a code whose CRC
 * fails, a zero address, the same address twice. */
static void test_codes_damaged(void)
{
    zbz_codes_t c;
    uint8_t buf[sizeof(CODES_FIXTURE_V1) + 1u];
    for (size_t n = 0; n < sizeof(CODES_FIXTURE_V1); n++) {
        TEST_ASSERT_FALSE(zbz_codes_decode(&c, CODES_FIXTURE_V1, n));
        TEST_ASSERT_EQUAL_size_t(0, c.count);
    }
    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    buf[sizeof(CODES_FIXTURE_V1)] = 0;
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(buf)));

    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    buf[0] = 0;
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));
    buf[0] = 2;
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));

    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    buf[1] = ZBZ_CODES_MAX + 1u;
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));

    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    buf[13] ^= 0x01u; /* a byte of the first code */
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));

    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    memset(&buf[3], 0, 8); /* the first address */
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));

    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    buf[35] = 0x01; /* the second address becomes the first's */
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));

    memcpy(buf, CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    buf[2] = 0x1F + 0x30; /* an entry claiming more than the blob holds */
    TEST_ASSERT_FALSE(zbz_codes_decode(&c, buf, sizeof(CODES_FIXTURE_V1)));
    TEST_ASSERT_EQUAL_size_t(0, c.count);
}

/* ZB-015, ZB-020: a later format's extra fields inside an entry are skipped by this reader. */
static void test_codes_later_fields_skipped(void)
{
    /* One entry for 02AB:CD00:0000:0002 with the 6-byte code, then two unknown bytes. */
    static const uint8_t later[] = {0x01, 0x01, 0x17, 0x02, 0x00, 0x00, 0x00, 0x00, 0xCD,
                                    0xAB, 0x02, 0x08, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                    0x5A, 0x60, 0x2A, 0x00, 0x00, 0x00, 0xAA, 0xBB};
    zbz_codes_t c;
    TEST_ASSERT_TRUE(zbz_codes_decode(&c, later, sizeof(later)));
    TEST_ASSERT_EQUAL_size_t(1, c.count);
    TEST_ASSERT_TRUE(c.codes[0].ieee == IEEE_2);
    TEST_ASSERT_EQUAL_MEMORY(CODE_6, c.codes[0].code, 8);
    TEST_ASSERT_EQUAL_UINT32(42, c.codes[0].added_at);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc);
    RUN_TEST(test_code_parse);
    RUN_TEST(test_code_refusals);
    RUN_TEST(test_ieee_parse);
    RUN_TEST(test_qr_parse);
    RUN_TEST(test_qr_refusals);
    RUN_TEST(test_codes_add);
    RUN_TEST(test_codes_forget);
    RUN_TEST(test_codes_frozen_fixture);
    RUN_TEST(test_codes_round_trip);
    RUN_TEST(test_codes_damaged);
    RUN_TEST(test_codes_later_fields_skipped);
    return UNITY_END();
}
