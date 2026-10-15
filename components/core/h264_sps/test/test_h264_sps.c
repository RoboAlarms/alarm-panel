/*
 * test_h264_sps.c - the remote keypad's key frames are rewritten to show at once in a browser.
 * The SPS under test is written exactly as esp_h264 1.4.1 writes it (hw/src/h264_nal.c,
 * esp_h264_enc_set_sps) for the P4's 1024x600 screen: constrained baseline, 64x38 macroblocks
 * cropped by 8 lines, picture order count type 2, one reference frame, a VUI with timing only.
 * Requirements: WEB-020 (docs/features/31).
 */
#include <string.h>

#include "h264_sps/h264_sps.h"
#include "unity.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* ---- a writer like esp_h264's: bits, exp-Golomb, emulation prevention as it goes ---------- */

typedef struct {
    uint8_t buf[128];
    size_t n;       /* bytes emitted */
    uint8_t cur;    /* the byte being filled */
    unsigned bits;  /* bits in cur */
    unsigned zeros; /* 0x00 bytes just emitted */
} enc_t;

static void emit(enc_t *e, uint8_t byte)
{
    if (e->zeros >= 2u && byte <= 3u) {
        e->buf[e->n++] = 3u;
        e->zeros = 0;
    }
    e->buf[e->n++] = byte;
    e->zeros = byte == 0u ? e->zeros + 1u : 0u;
}

static void put(enc_t *e, uint32_t v, unsigned n)
{
    for (unsigned i = n; i-- > 0u;) {
        e->cur = (uint8_t)(((unsigned)e->cur << 1) | ((v >> i) & 1u));
        if (++e->bits == 8u) {
            emit(e, e->cur);
            e->cur = 0;
            e->bits = 0;
        }
    }
}

static void put_ue(enc_t *e, uint32_t v)
{
    const uint64_t x = (uint64_t)v + 1u;
    unsigned len = 0;
    while ((x >> len) > 1u) {
        len++;
    }
    put(e, 0, len);
    put(e, (uint32_t)x, len + 1u);
}

/* The encoder's SPS NAL unit for width x height at fps, without its start code. */
static size_t esp_h264_sps(uint8_t *out, uint16_t width, uint16_t height, uint8_t fps)
{
    enc_t e;
    memset(&e, 0, sizeof(e));
    put(&e, 0x67, 8);          /* forbidden 0, nal_ref_idc 3, type 7 */
    put(&e, 66, 8);            /* baseline */
    put(&e, 0xC0, 8);          /* constraint_set0 and 1 */
    put(&e, 31, 8);            /* level 3.1 */
    put_ue(&e, 0);             /* seq_parameter_set_id */
    put_ue(&e, 4);             /* log2_max_frame_num_minus4 (LOG_MAX_FRAME_NUM 8) */
    put_ue(&e, 2);             /* pic_order_cnt_type */
    put_ue(&e, 1);             /* num_ref_frames */
    put(&e, 0, 1);             /* gaps */
    const unsigned mbw = (width + 15u) / 16u, mbh = (height + 15u) / 16u;
    put_ue(&e, mbw - 1u);
    put_ue(&e, mbh - 1u);
    put(&e, 1, 1);             /* frame_mbs_only_flag */
    put(&e, 0, 1);             /* direct_8x8_inference_flag */
    const bool crop = (width % 16u) != 0u || (height % 16u) != 0u;
    put(&e, crop ? 1u : 0u, 1);
    if (crop) {
        put_ue(&e, 0);
        put_ue(&e, (mbw * 16u - width) / 2u);
        put_ue(&e, 0);
        put_ue(&e, (mbh * 16u - height) / 2u);
    }
    put(&e, 1, 1);             /* vui_parameters_present_flag */
    put(&e, 0, 4);             /* aspect, overscan, video signal, chroma location: none */
    put(&e, 1, 1);             /* timing_info_present_flag */
    put(&e, 1, 32);            /* num_units_in_tick */
    put(&e, (uint32_t)fps * 2u, 32); /* time_scale */
    put(&e, 1, 1);             /* fixed_frame_rate_flag */
    put(&e, 0, 4);             /* no HRD, no pic_struct, no bitstream_restriction */
    put(&e, 1, 1);             /* rbsp_stop_one_bit */
    while (e.bits != 0u) {
        put(&e, 0, 1);
    }
    memcpy(out, e.buf, e.n);
    return e.n;
}

/* ---- a reader for the result: our own, not the one under test ------------------------------ */

typedef struct {
    uint8_t rbsp[128];
    size_t len;
    size_t pos;
} dec_t;

static void dec_init(dec_t *d, const uint8_t *nal, size_t len)
{
    memset(d, 0, sizeof(*d));
    unsigned zeros = 0;
    for (size_t i = 1; i < len; i++) { /* after the NAL header */
        if (zeros >= 2u && nal[i] == 3u) {
            zeros = 0;
            continue;
        }
        d->rbsp[d->len++] = nal[i];
        zeros = nal[i] == 0u ? zeros + 1u : 0u;
    }
}

static uint32_t get(dec_t *d, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, d->pos++) {
        TEST_ASSERT_LESS_THAN_size_t(d->len * 8u, d->pos);
        v = (v << 1) | (((unsigned)d->rbsp[d->pos / 8u] >> (7u - d->pos % 8u)) & 1u);
    }
    return v;
}

static uint32_t get_ue(dec_t *d)
{
    unsigned zeros = 0;
    while (get(d, 1) == 0u) {
        zeros++;
    }
    return (uint32_t)((1ull << zeros) - 1u + get(d, zeros));
}

/* An access unit as the encoder hands it over: SPS, PPS, IDR slice. */
static const uint8_t PPS[] = {0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80};
static const uint8_t IDR[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0x00, 0x00, 0x03, 0x01, 0x42};

static size_t access_unit(uint8_t *au, uint16_t width, uint16_t height, uint8_t fps)
{
    size_t n = 0;
    au[n++] = 0;
    au[n++] = 0;
    au[n++] = 0;
    au[n++] = 1;
    n += esp_h264_sps(au + n, width, height, fps);
    memcpy(au + n, PPS, sizeof(PPS));
    n += sizeof(PPS);
    memcpy(au + n, IDR, sizeof(IDR));
    return n + sizeof(IDR);
}

/* WEB-020: the rewritten SPS keeps every field the slices depend on, and adds no reordering,
 * one picture buffered and the BT.601 colour of the PPA's output. */
static void test_rewrites_the_encoders_sps(void)
{
    uint8_t au[256], out[256 + H264_SPS_GROWTH];
    const size_t len = access_unit(au, 1024, 600, 15);
    const size_t n = h264_sps_low_delay(au, len, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);

    /* The PPS and the slice follow byte for byte. */
    const size_t tail = sizeof(PPS) + sizeof(IDR);
    TEST_ASSERT_EQUAL_MEMORY(au + len - tail, out + n - tail, tail);
    static const uint8_t start[] = {0, 0, 0, 1, 0x67};
    TEST_ASSERT_EQUAL_MEMORY(start, out, sizeof(start));

    dec_t d;
    dec_init(&d, out + 4, n - 4 - tail);
    TEST_ASSERT_EQUAL_UINT32(66, get(&d, 8));
    TEST_ASSERT_EQUAL_UINT32(0xC0, get(&d, 8));
    TEST_ASSERT_EQUAL_UINT32(31, get(&d, 8));
    TEST_ASSERT_EQUAL_UINT32(0, get_ue(&d));  /* sps id */
    TEST_ASSERT_EQUAL_UINT32(4, get_ue(&d));  /* log2_max_frame_num_minus4 */
    TEST_ASSERT_EQUAL_UINT32(2, get_ue(&d));  /* poc type */
    TEST_ASSERT_EQUAL_UINT32(1, get_ue(&d));  /* reference frames */
    TEST_ASSERT_EQUAL_UINT32(0, get(&d, 1));
    TEST_ASSERT_EQUAL_UINT32(63, get_ue(&d)); /* 1024 / 16 - 1 */
    TEST_ASSERT_EQUAL_UINT32(37, get_ue(&d)); /* 608 / 16 - 1 */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));
    TEST_ASSERT_EQUAL_UINT32(0, get(&d, 1));
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));  /* cropped... */
    TEST_ASSERT_EQUAL_UINT32(0, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(0, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(0, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(4, get_ue(&d));  /* ...by 8 lines at the bottom */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));  /* VUI */
    TEST_ASSERT_EQUAL_UINT32(0, get(&d, 2));  /* aspect, overscan */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));  /* video signal type */
    TEST_ASSERT_EQUAL_UINT32(5, get(&d, 3));
    TEST_ASSERT_EQUAL_UINT32(0, get(&d, 1));  /* limited range */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));
    TEST_ASSERT_EQUAL_UINT32(6, get(&d, 8));  /* BT.601 primaries, transfer, matrix */
    TEST_ASSERT_EQUAL_UINT32(6, get(&d, 8));
    TEST_ASSERT_EQUAL_UINT32(6, get(&d, 8));
    TEST_ASSERT_EQUAL_UINT32(0, get(&d, 1));  /* chroma location */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));  /* the encoder's timing, kept */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 32));
    TEST_ASSERT_EQUAL_UINT32(30, get(&d, 32));
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));
    TEST_ASSERT_EQUAL_UINT32(0, get(&d, 3));  /* no HRD, no pic_struct */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));  /* bitstream_restriction_flag */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));
    TEST_ASSERT_EQUAL_UINT32(2, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(1, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(16, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(16, get_ue(&d));
    TEST_ASSERT_EQUAL_UINT32(0, get_ue(&d));  /* max_num_reorder_frames */
    TEST_ASSERT_EQUAL_UINT32(1, get_ue(&d));  /* max_dec_frame_buffering */
    TEST_ASSERT_EQUAL_UINT32(1, get(&d, 1));  /* stop bit, then zeros to the byte */
    while (d.pos % 8u != 0u) {
        TEST_ASSERT_EQUAL_UINT32(0, get(&d, 1));
    }
    TEST_ASSERT_EQUAL_size_t(d.len * 8u, d.pos);
}

/* WEB-020: no 00 00 0x (x <= 3) inside the rewritten NAL unit: the 32-bit timing fields are
 * mostly zeros and need emulation-prevention bytes. */
static void test_emulation_prevention(void)
{
    uint8_t au[256], out[256 + H264_SPS_GROWTH];
    const size_t len = access_unit(au, 1024, 600, 15);
    const size_t n = h264_sps_low_delay(au, len, out, sizeof(out));
    const size_t sps_end = n - sizeof(PPS) - sizeof(IDR);
    bool prevented = false;
    for (size_t i = 4; i + 2u < sps_end; i++) {
        TEST_ASSERT_FALSE(out[i] == 0u && out[i + 1u] == 0u && out[i + 2u] <= 2u);
        prevented |= out[i] == 0u && out[i + 1u] == 0u && out[i + 2u] == 3u;
    }
    TEST_ASSERT_TRUE(prevented);
}

/* WEB-020: other sizes and a short start code work too; a P frame has nothing to rewrite. */
static void test_other_access_units(void)
{
    uint8_t au[256], out[256 + H264_SPS_GROWTH];
    /* 480x320 (the 4" screen), SPS behind a three-byte start code. */
    size_t len = access_unit(au, 480, 320, 30);
    memmove(au, au + 1, len - 1u);
    len--;
    const size_t n = h264_sps_low_delay(au, len, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    TEST_ASSERT_EQUAL_MEMORY(IDR, out + n - sizeof(IDR), sizeof(IDR));
    TEST_ASSERT_TRUE(h264_has_idr(out, n));

    static const uint8_t p_frame[] = {0, 0, 0, 1, 0x41, 0x9A, 0x02, 0x04};
    TEST_ASSERT_EQUAL_size_t(0, h264_sps_low_delay(p_frame, sizeof(p_frame), out, sizeof(out)));
    TEST_ASSERT_FALSE(h264_has_idr(p_frame, sizeof(p_frame)));

    /* Too little room, or a damaged SPS: left alone. */
    len = access_unit(au, 1024, 600, 15);
    TEST_ASSERT_EQUAL_size_t(0, h264_sps_low_delay(au, len, out, len));
    static const uint8_t cut[] = {0, 0, 0, 1, 0x67, 0x42, 0xC0};
    TEST_ASSERT_EQUAL_size_t(0, h264_sps_low_delay(cut, sizeof(cut), out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, h264_sps_low_delay(NULL, 0, out, sizeof(out)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rewrites_the_encoders_sps);
    RUN_TEST(test_emulation_prevention);
    RUN_TEST(test_other_access_units);
    return UNITY_END();
}
