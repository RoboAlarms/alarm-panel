/*
 * plink_auto_fixture_v1.h - the payload of LINK-056's AUTO_WARN (0x3E, M->K), as v1 of
 * PLINK_CAP_AUTO (0x0020) sends it (docs/features/11 "Arming warnings on keypads"): count u8,
 * rec_len u8 (26), then count records of uid u16, partition u8, level u8, state u8, warn_min u8,
 * bypass u8, put_off_by u8, arm_at_epoch_ms i64, arm_in_ms u32, until_day i32, until_minute u16,
 * all little endian. test_plink_msg.c reads it with today's decoder as the values below and encodes
 * it again to exactly these bytes (test_auto_fixture_v1). Built from that published layout, never
 * from the encoder. PLINK_OP_PUT_OFF is 6.
 * Frozen: never edit it; a new layout adds a new fixture beside it.
 */
#define PLINK_AUTO_V1_TYPE   0x3Eu
#define PLINK_AUTO_V1_CAP    0x0020u
#define PLINK_AUTO_V1_OP     6u

/* One warning: automation 5 arms partition 1 Night (level 3), ON (state 1), a 5 minute warning,
 * no bypass, not put off, at epoch ms 0x0000019A2B3C4D00, 240000 ms from now; until_day -1,
 * until_minute 360. */
static const uint8_t PLINK_AUTO_V1[28] = {
    0x01, 0x1A,
    0x05, 0x00, 0x01, 0x03, 0x01, 0x05, 0x00, 0x00,
    0x00, 0x4D, 0x3C, 0x2B, 0x9A, 0x01, 0x00, 0x00,
    0x80, 0xA9, 0x03, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x68, 0x01,
};
