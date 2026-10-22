/*
 * plink_io_fixture_v1.h - the payload of LINK-055's IO_STATE (0x3D, K->M), as v1 of PLINK_CAP_IO
 * (0x0010) sends it (docs/features/11 "A keypad's own inputs"): count u8, rec_len u8 (4), then
 * count records of gpio u8, flags u8 (0x01 active, 0x02 known), changes u16 little endian.
 * test_plink_msg.c reads it with today's decoder as the values below and encodes it again to
 * exactly these bytes (test_io_fixture_v1). Built from that published layout, never from the
 * encoder.
 * Frozen: never edit it; a new layout adds a new fixture beside it.
 */
#define PLINK_IO_V1_TYPE 0x3Du
#define PLINK_IO_V1_CAP  0x0010u

/* Two terminals: IO4 closed and known, changed 258 times; IO18 open and known, changed 7 times. */
static const uint8_t PLINK_IO_V1[10] = {
    0x02, 0x04,
    0x04, 0x02, 0x02, 0x01,
    0x12, 0x03, 0x07, 0x00,
};
