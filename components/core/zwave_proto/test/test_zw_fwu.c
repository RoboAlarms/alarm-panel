/* ZW-033 (docs/features/36): the Firmware Update Meta Data command class and firmware files -
 * every version's shape, fragments and their CRC, statuses, and the file formats the panel takes
 * or refuses. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_cc_fwu.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_proto/zw_fwfile.h"

void setUp(void) {}
void tearDown(void) {}

static void test_md_report_versions(void)
{
    const uint8_t v1[] = {0x7A, 0x02, 0x01, 0x5A, 0x00, 0x03, 0xAB, 0xCD};
    zw_fwu_md_t md;
    TEST_ASSERT_TRUE(zw_fwu_md_report_parse(v1, sizeof v1, &md));
    TEST_ASSERT_EQUAL_HEX16(0x015A, md.manufacturer_id);
    TEST_ASSERT_EQUAL_HEX16(0x0003, md.firmware_id);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, md.checksum);
    TEST_ASSERT_TRUE(md.upgradable); /* v1 doesn't say */
    TEST_ASSERT_EQUAL_UINT8(1, md.version_seen);
    TEST_ASSERT_EQUAL_UINT8(0xFF, md.hardware);
    const uint8_t v7[] = {0x7A, 0x02, 0x03, 0x46, 0x01, 0x01, 0x12, 0x34, 0xFF, 1, 0x00, 0x28,
                          0x00, 0x02, 0x05, 0x03};
    TEST_ASSERT_TRUE(zw_fwu_md_report_parse(v7, sizeof v7, &md));
    TEST_ASSERT_EQUAL_UINT8(6, md.version_seen);
    TEST_ASSERT_TRUE(md.upgradable);
    TEST_ASSERT_EQUAL_UINT16(40, md.max_fragment);
    TEST_ASSERT_EQUAL_UINT8(1, md.target_count);
    TEST_ASSERT_EQUAL_HEX16(0x0002, md.target_ids[0]);
    TEST_ASSERT_EQUAL_UINT8(5, md.hardware);
    TEST_ASSERT_TRUE(md.continues_to_function);
    TEST_ASSERT_TRUE(md.supports_activation);
    TEST_ASSERT_FALSE(md.supports_resume);
    uint8_t out[32];
    const size_t n = zw_fwu_md_report_build(out, sizeof out, &md);
    TEST_ASSERT_EQUAL_UINT(sizeof v7, n);
    TEST_ASSERT_EQUAL_MEMORY(v7, out, n);
    const uint8_t lying[] = {0x7A, 0x02, 0, 1, 0, 2, 0, 3, 0xFF, 5, 0, 40, 0, 1};
    TEST_ASSERT_FALSE(zw_fwu_md_report_parse(lying, sizeof lying, &md)); /* 5 targets, 1 there */
    const uint8_t not_up[] = {0x7A, 0x02, 0, 1, 0, 2, 0, 3, 0x00, 0, 0, 40};
    TEST_ASSERT_TRUE(zw_fwu_md_report_parse(not_up, sizeof not_up, &md));
    TEST_ASSERT_FALSE(md.upgradable);
    TEST_ASSERT_FALSE(zw_fwu_md_report_parse(v1, 7u, &md));
}

static void test_request_get_by_version(void)
{
    zw_fwu_request_t r = {0x015A, 0x0003, 0xBEEF, 0, 40, true, 5, 1};
    uint8_t out[16];
    TEST_ASSERT_EQUAL_UINT(8, zw_fwu_request_get_build(out, sizeof out, &r));
    r.cc_version = 3;
    TEST_ASSERT_EQUAL_UINT(11, zw_fwu_request_get_build(out, sizeof out, &r));
    r.cc_version = 5;
    const size_t n = zw_fwu_request_get_build(out, sizeof out, &r);
    const uint8_t want[] = {0x7A, 0x03, 0x01, 0x5A, 0x00, 0x03, 0xBE, 0xEF, 0x00, 0x00, 40, 1, 5};
    TEST_ASSERT_EQUAL_UINT(sizeof want, n);
    TEST_ASSERT_EQUAL_MEMORY(want, out, n);
    zw_fwu_request_t back;
    TEST_ASSERT_TRUE(zw_fwu_request_get_parse(out, n, &back));
    TEST_ASSERT_EQUAL_UINT16(40, back.fragment_size);
    TEST_ASSERT_TRUE(back.activation);
    TEST_ASSERT_EQUAL_UINT8(5, back.hardware);
    uint8_t status = 0;
    const uint8_t rr[] = {0x7A, 0x04, 0x06};
    TEST_ASSERT_TRUE(zw_fwu_request_report_parse(rr, sizeof rr, &status));
    TEST_ASSERT_EQUAL_HEX8(ZW_FWU_REQ_BATTERY_LOW, status);
}

static void test_fragments_and_crc(void)
{
    const uint8_t data[] = {1, 2, 3, 4, 5};
    uint8_t out[16];
    const size_t n = zw_fwu_report_build(out, sizeof out, 3, true, data, sizeof data, true);
    TEST_ASSERT_EQUAL_UINT(4u + 5u + 2u, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[3]);
    const uint16_t crc = zw_crc16(ZW_CRC16_INIT, out, n - 2u);
    TEST_ASSERT_EQUAL_HEX16(crc, (uint16_t)(out[n - 2u] << 8 | out[n - 1u]));
    uint16_t number = 0;
    bool last = false;
    const uint8_t *d = NULL;
    size_t dn = 0;
    TEST_ASSERT_TRUE(zw_fwu_report_parse(out, n, true, &number, &last, &d, &dn));
    TEST_ASSERT_EQUAL_UINT16(3, number);
    TEST_ASSERT_TRUE(last);
    TEST_ASSERT_EQUAL_MEMORY(data, d, dn);
    out[5] ^= 1u; /* a byte damaged on the way: its CRC says so */
    TEST_ASSERT_FALSE(zw_fwu_report_parse(out, n, true, &number, &last, &d, &dn));
    TEST_ASSERT_EQUAL_UINT(0u, zw_fwu_report_build(out, sizeof out, 0, false, data, 1, false));
    TEST_ASSERT_EQUAL_UINT(0u, zw_fwu_report_build(out, sizeof out, 0x8000u, false, data, 1, false));
    zw_fwu_get_t g;
    const uint8_t get[] = {0x7A, 0x05, 2, 0x80, 0x07}; /* its reserved bit ignored */
    TEST_ASSERT_TRUE(zw_fwu_get_parse(get, sizeof get, &g));
    TEST_ASSERT_EQUAL_UINT16(7, g.report_number);
    const uint8_t get0[] = {0x7A, 0x05, 2, 0x00, 0x00};
    TEST_ASSERT_FALSE(zw_fwu_get_parse(get0, sizeof get0, &g));
}

static void test_status_and_activation(void)
{
    uint8_t st = 0;
    bool has = false;
    uint16_t wait = 0;
    const uint8_t s1[] = {0x7A, 0x07, 0xFF};
    TEST_ASSERT_TRUE(zw_fwu_status_report_parse(s1, sizeof s1, &st, &has, &wait));
    TEST_ASSERT_FALSE(has);
    const uint8_t s3[] = {0x7A, 0x07, 0xFF, 0x00, 0x1E};
    TEST_ASSERT_TRUE(zw_fwu_status_report_parse(s3, sizeof s3, &st, &has, &wait));
    TEST_ASSERT_TRUE(has);
    TEST_ASSERT_EQUAL_UINT16(30, wait);
    zw_fwu_activation_t a = {0x015A, 3, 0xBEEF, 0, 0, 0xFF};
    uint8_t out[16];
    TEST_ASSERT_EQUAL_UINT(9, zw_fwu_activation_set_build(out, sizeof out, &a));
    const uint8_t rep[] = {0x7A, 0x09, 0x01, 0x5A, 0, 3, 0xBE, 0xEF, 0, 0xFF};
    zw_fwu_activation_t back;
    TEST_ASSERT_TRUE(zw_fwu_activation_report_parse(rep, sizeof rep, &back));
    TEST_ASSERT_EQUAL_HEX8(0xFF, back.status);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, back.checksum);
}

static void test_file_formats(void)
{
    const uint8_t gbl[] = {0xEB, 0x17, 0xA6, 0x03, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_GECKO, zw_fwfile_guess("x.gbl", gbl, sizeof gbl));
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_GECKO, zw_fwfile_guess("firmware", gbl, sizeof gbl));
    const uint8_t junk[] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_UNKNOWN, zw_fwfile_guess("x.gbl", junk, sizeof junk));
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_BINARY, zw_fwfile_guess("X.BIN", junk, sizeof junk));
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_UNSUPPORTED, zw_fwfile_guess("x.otz", junk, sizeof junk));
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_UNSUPPORTED, zw_fwfile_guess("x.hec", junk, sizeof junk));
    const uint8_t zip[] = {'P', 'K', 3, 4};
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_UNSUPPORTED, zw_fwfile_guess("x.bin", zip, sizeof zip));
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_UNKNOWN, zw_fwfile_guess("x.exe", junk, sizeof junk));
}

/* Intel HEX: two records with a gap (filled FF), an extended linear address, the EOF record. */
static void test_hex_to_bin(void)
{
    const char *hex = ":020000040001F9\r\n"   /* base 0x10000 */
                      ":03000000010203F7\n"   /* 0x10000: 01 02 03 */
                      ":02000500AABB94\n"     /* 0x10005: AA BB */
                      ":00000001FF\n";
    uint8_t out[16];
    const size_t n = zw_fwfile_hex_to_bin((const uint8_t *)hex, strlen(hex), out, sizeof out);
    const uint8_t want[] = {1, 2, 3, 0xFF, 0xFF, 0xAA, 0xBB};
    TEST_ASSERT_EQUAL_UINT(sizeof want, n);
    TEST_ASSERT_EQUAL_MEMORY(want, out, n);
    TEST_ASSERT_EQUAL_INT(ZW_FWFILE_HEX, zw_fwfile_guess("f.hex", (const uint8_t *)hex, 4));
    /* a wrong line checksum, no EOF, too big, not HEX: refused */
    const char *bad = ":03000000010203F6\n:00000001FF\n";
    TEST_ASSERT_EQUAL_UINT(0u, zw_fwfile_hex_to_bin((const uint8_t *)bad, strlen(bad), out, 16));
    const char *no_eof = ":03000000010203F7\n";
    TEST_ASSERT_EQUAL_UINT(0u, zw_fwfile_hex_to_bin((const uint8_t *)no_eof, strlen(no_eof), out,
                                                    16));
    TEST_ASSERT_EQUAL_UINT(0u, zw_fwfile_hex_to_bin((const uint8_t *)hex, strlen(hex), out, 6));
    const char *text = "hello\n";
    TEST_ASSERT_EQUAL_UINT(0u, zw_fwfile_hex_to_bin((const uint8_t *)text, strlen(text), out, 16));
}

/* ZW-023: random bytes never crash the parsers. */
static void test_random(void)
{
    srand(77u);
    for (unsigned r = 0; r < 20000u; r++) {
        uint8_t b[64];
        const size_t n = (size_t)rand() % sizeof b;
        for (size_t i = 0; i < n; i++) {
            b[i] = (uint8_t)rand();
        }
        if (n > 1u) {
            b[0] = 0x7A;
        }
        zw_fwu_md_t md;
        zw_fwu_request_t rq;
        zw_fwu_get_t g;
        zw_fwu_activation_t a;
        uint8_t st;
        bool has, last;
        uint16_t w, num;
        const uint8_t *d;
        size_t dn;
        (void)zw_fwu_md_report_parse(b, n, &md);
        (void)zw_fwu_request_get_parse(b, n, &rq);
        (void)zw_fwu_request_report_parse(b, n, &st);
        (void)zw_fwu_get_parse(b, n, &g);
        (void)zw_fwu_status_report_parse(b, n, &st, &has, &w);
        (void)zw_fwu_activation_report_parse(b, n, &a);
        (void)zw_fwu_report_parse(b, n, true, &num, &last, &d, &dn);
        uint8_t out[64];
        (void)zw_fwfile_hex_to_bin(b, n, out, sizeof out);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_md_report_versions);
    RUN_TEST(test_request_get_by_version);
    RUN_TEST(test_fragments_and_crc);
    RUN_TEST(test_status_and_activation);
    RUN_TEST(test_file_formats);
    RUN_TEST(test_hex_to_bin);
    RUN_TEST(test_random);
    return UNITY_END();
}
