/*
 * plink_rb_fixture_v1.h - the remote backend's messages (LINK-058, PLINK_CAP_RB 0x0040;
 * docs/features/11 "The master's settings and installer menu on keypads"), as v1 sends them, all
 * little endian:
 *   RB_CALL   0x3F K->M  req u32, call u16, offset u32, total u32, len u16, len bytes
 *   RB_ANSWER 0x40 M->K  req u32, status u8, total u32, offset u32, len u16, len bytes
 *   RB_MORE   0x41 K->M  req u32, offset u32, max u16
 *   RB_GRANT  0x42 M->K  grant section (len u8; user u8, authority u8, partitions u8,
 *                        installer u8, idle_left_s u16), holder section (len u8; kind u8,
 *                        id u16), areas u32
 * test_plink_msg.c reads them with today's decoders as the values below and encodes them again to
 * exactly these bytes (test_rb_fixture_v1). Built from that published layout, never from the
 * encoders. Frozen: never edit it; a new layout adds a new fixture beside it.
 */
#define PLINK_RB_V1_CAP         0x0040u
#define PLINK_RB_V1_TYPE_CALL   0x3Fu
#define PLINK_RB_V1_TYPE_ANSWER 0x40u
#define PLINK_RB_V1_TYPE_MORE   0x41u
#define PLINK_RB_V1_TYPE_GRANT  0x42u

/* req 0x01020304, call 0x0033 (ZONE_BIND), offset 0, total 3, the bytes AA BB CC */
static const uint8_t PLINK_RB_V1_CALL[19] = {
    0x04, 0x03, 0x02, 0x01, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0xAA, 0xBB, 0xCC,
};
/* req 0x01020304, status 0, total 5, offset 2, the bytes 11 22 33 */
static const uint8_t PLINK_RB_V1_ANSWER[18] = {
    0x04, 0x03, 0x02, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x11, 0x22, 0x33,
};
/* req 0x01020304, offset 1024, max 0 (as much as fits) */
static const uint8_t PLINK_RB_V1_MORE[10] = {
    0x04, 0x03, 0x02, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
};
/* user 2, authority 2 (system master), partitions 0x01, installer mode, 300 s left; held by
 * source kind 2, id 1; areas 0x0000000F */
static const uint8_t PLINK_RB_V1_GRANT[15] = {
    0x06, 0x02, 0x02, 0x01, 0x01, 0x2C, 0x01,
    0x03, 0x02, 0x01, 0x00,
    0x0F, 0x00, 0x00, 0x00,
};
