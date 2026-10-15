/*
 * test_zw_gbl.c - a Gecko bootloader file checked before the radio is touched, and the XMODEM-CRC
 * blocks of its upload (docs/features/36 serial-api.md §8, network.md §14; ZW-025).
 */
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_proto/zw_gbl.h"

void setUp(void) {}
void tearDown(void) {}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8 & 0xFFu);
    p[2] = (uint8_t)(v >> 16 & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

/* A small GBL: header, one program-data tag of `data_len` bytes, the end tag with its CRC. */
static size_t make_gbl(uint8_t *out, size_t data_len)
{
    size_t at = 0u;
    put_u32(&out[at], ZW_GBL_TAG_HEADER);
    put_u32(&out[at + 4u], 8u);
    put_u32(&out[at + 8u], 0x03000000u); /* version */
    put_u32(&out[at + 12u], 0u);         /* type: plain */
    at += 16u;
    put_u32(&out[at], 0xF40A0AF4u);       /* program data */
    put_u32(&out[at + 4u], (uint32_t)(4u + data_len));
    put_u32(&out[at + 8u], 0u);           /* its flash address */
    for (size_t i = 0; i < data_len; i++) {
        out[at + 12u + i] = (uint8_t)(i * 7u + 3u);
    }
    at += 12u + data_len;
    put_u32(&out[at], ZW_GBL_TAG_END);
    put_u32(&out[at + 4u], 4u);
    put_u32(&out[at + 8u], zw_crc32(0u, out, at + 8u));
    return at + 12u;
}

/* The catalogue's check values: CRC-32 and CRC-16/XMODEM of "123456789" */
static void test_crc_check_values(void)
{
    const uint8_t s[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, zw_crc32(0u, s, 9u));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, zw_crc32(zw_crc32(0u, s, 4u), &s[4], 5u));
    TEST_ASSERT_EQUAL_HEX16(0x31C3u, zw_crc16(0x0000u, s, 9u)); /* CRC-16/XMODEM */
}

/* ZW-025: a whole file passes; damaged, cut, extended or foreign ones don't */
static void test_gbl_check(void)
{
    static uint8_t f[600];
    const size_t n = make_gbl(f, 500u);
    uint32_t tags = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_GBL_OK, zw_gbl_check(f, n, &tags));
    TEST_ASSERT_EQUAL_UINT32(3u, tags);
    f[100] ^= 0x01u; /* one bit changed in the download */
    TEST_ASSERT_EQUAL_INT(ZW_GBL_CRC, zw_gbl_check(f, n, NULL));
    f[100] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(ZW_GBL_DAMAGED, zw_gbl_check(f, n - 1u, NULL)); /* cut short */
    TEST_ASSERT_EQUAL_INT(ZW_GBL_DAMAGED, zw_gbl_check(f, n - 12u, NULL)); /* no end tag */
    f[n] = 0xFFu;
    TEST_ASSERT_EQUAL_INT(ZW_GBL_DAMAGED, zw_gbl_check(f, n + 1u, NULL)); /* bytes after it */
    static const uint8_t hex[] = ":10000000...";
    TEST_ASSERT_EQUAL_INT(ZW_GBL_NOT_GBL, zw_gbl_check(hex, sizeof hex, NULL));
    TEST_ASSERT_EQUAL_INT(ZW_GBL_NOT_GBL, zw_gbl_check(f, 4u, NULL));
    put_u32(&f[20], 0xFFFFFFF0u); /* a tag longer than the file */
    TEST_ASSERT_EQUAL_INT(ZW_GBL_DAMAGED, zw_gbl_check(f, n, NULL));
}

/* ZW-025 (serial-api.md §8 step 3): 128-byte blocks numbered from 1 mod 256, the last padded
 * with FF, CRC-16/XMODEM big-endian */
static void test_xmodem_frames(void)
{
    static uint8_t f[300];
    for (size_t i = 0; i < sizeof f; i++) {
        f[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_UINT32(3u, zw_xmodem_blocks(sizeof f));
    TEST_ASSERT_EQUAL_UINT32(1u, zw_xmodem_blocks(128u));
    uint8_t b[ZW_XMODEM_FRAME];
    TEST_ASSERT_TRUE(zw_xmodem_frame(f, sizeof f, 2u, b));
    TEST_ASSERT_EQUAL_HEX8(0x01, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFC, b[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, b[3]);   /* byte 256 of the file */
    TEST_ASSERT_EQUAL_HEX8(0x2B, b[3 + 43]); /* its last: 299 */
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[3 + 44]); /* padding */
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[3 + 127]);
    TEST_ASSERT_FALSE(zw_xmodem_frame(f, sizeof f, 3u, b));
    /* a short file is one block, padded; its CRC covers the padding */
    static const uint8_t nine[] = "123456789";
    static uint8_t one[128];
    memset(one, 0xFF, sizeof one);
    memcpy(one, nine, 9u);
    TEST_ASSERT_TRUE(zw_xmodem_frame(nine, 9u, 0u, b));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(one, &b[3], 128u);
    const uint16_t crc = zw_crc16(0x0000u, one, sizeof one);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(crc >> 8), b[131]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(crc & 0xFFu), b[132]);
    /* block 256 is numbered 0 again */
    static uint8_t big[257u * 128u];
    TEST_ASSERT_TRUE(zw_xmodem_frame(big, sizeof big, 255u, b));
    TEST_ASSERT_EQUAL_HEX8(0x00, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_check_values);
    RUN_TEST(test_gbl_check);
    RUN_TEST(test_xmodem_frames);
    return UNITY_END();
}
