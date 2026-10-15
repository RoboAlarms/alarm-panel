/*
 * test_voice_render.c - the utterance renderer (docs/features/08 voice-first-build.md D11).
 * Requirements: AUD-028
 *
 * The upsampler is tested on hand-worked vectors; the renderer on the tracked en-US voice,
 * packed by tools/voice/voicepack.py at build time (VOICE_TEST_PACK; without Python the pack
 * tests are ignored, never failed).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "voice_codec.h"
#include "voice_pack/voice_pack.h"
#include "voice_render.h"

#define ID_FRONT 1u
#define ID_BACK 2u
#define OUT_MAX (VR_TOTAL_MAX + 64u)

static uint8_t *g_file;
static size_t g_len;
static vp_pack_t g_pack;
static void *g_codec;
static size_t g_codec_bytes;
static int16_t g_ref[OUT_MAX];
static int16_t g_got[OUT_MAX];
static vr_t g_r;

void setUp(void) {}
void tearDown(void) {}

static int load_pack(void)
{
#ifdef VOICE_TEST_PACK
    if (g_file != NULL) {
        return 1;
    }
    FILE *f = fopen(VOICE_TEST_PACK, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = malloc((size_t)n);
    TEST_ASSERT_NOT_NULL(g_file);
    TEST_ASSERT_EQUAL_size_t((size_t)n, fread(g_file, 1, (size_t)n, f));
    fclose(f);
    g_len = (size_t)n;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(g_file, g_len, &g_pack, NULL));
    g_codec_bytes = vc_state_bytes();
    g_codec = malloc(g_codec_bytes);
    TEST_ASSERT_NOT_NULL(g_codec);
    return 1;
#else
    return 0;
#endif
}

#define NEED_PACK()                                  \
    do {                                             \
        if (!load_pack()) {                          \
            TEST_IGNORE_MESSAGE("no voice test pack"); \
        }                                            \
    } while (0)

/* Pull everything with a fixed pull size; returns the total, or the error. */
static int32_t render_all(const vr_item_t *items, size_t n, size_t pull, int16_t *dst)
{
    TEST_ASSERT_EQUAL_INT(0, vr_begin(&g_r, &g_pack, items, n, g_codec, g_codec_bytes));
    int32_t total = 0;
    for (;;) {
        int32_t got = vr_pull(&g_r, dst + total, pull);
        if (got < 0) {
            return got;
        }
        if (got == 0) {
            return total;
        }
        total += got;
        TEST_ASSERT_LESS_OR_EQUAL_INT32((int32_t)OUT_MAX - (int32_t)pull, total);
    }
}

/* ---- The upsampler (D11) on worked vectors ---- */

static void test_upsampler_vectors(void)
{
    vr_up_t up;
    int16_t o[2];
    vr_up_reset(&up);
    TEST_ASSERT_EQUAL_size_t(0, vr_up_push(&up, 10, o)); /* first sample: nothing yet */
    TEST_ASSERT_EQUAL_size_t(2, vr_up_push(&up, 13, o)); /* 10, (10+13)/2 = 11 */
    TEST_ASSERT_EQUAL_INT16(10, o[0]);
    TEST_ASSERT_EQUAL_INT16(11, o[1]);
    TEST_ASSERT_EQUAL_size_t(2, vr_up_push(&up, -4, o)); /* 13, (13-4)/2 = 4 */
    TEST_ASSERT_EQUAL_INT16(13, o[0]);
    TEST_ASSERT_EQUAL_INT16(4, o[1]);
    TEST_ASSERT_EQUAL_size_t(2, vr_up_push(&up, -5, o)); /* -4, (-4-5)/2 = -9/2 -> -4 (toward 0) */
    TEST_ASSERT_EQUAL_INT16(-4, o[0]);
    TEST_ASSERT_EQUAL_INT16(-4, o[1]);
    TEST_ASSERT_EQUAL_size_t(2, vr_up_push(&up, 2, o)); /* -5, (-5+2)/2 = -3/2 -> -1 */
    TEST_ASSERT_EQUAL_INT16(-5, o[0]);
    TEST_ASSERT_EQUAL_INT16(-1, o[1]);
    TEST_ASSERT_EQUAL_size_t(2, vr_up_flush(&up, o)); /* the last sample twice */
    TEST_ASSERT_EQUAL_INT16(2, o[0]);
    TEST_ASSERT_EQUAL_INT16(2, o[1]);
    TEST_ASSERT_EQUAL_size_t(0, vr_up_flush(&up, o)); /* reset: nothing carried to a next clip */
    TEST_ASSERT_EQUAL_size_t(0, vr_up_push(&up, 7, o));
}

static void test_upsampler_extremes_and_empty(void)
{
    vr_up_t up;
    int16_t o[2];
    vr_up_reset(&up);
    TEST_ASSERT_EQUAL_size_t(0, vr_up_flush(&up, o));
    (void)vr_up_push(&up, 32767, o);
    (void)vr_up_push(&up, 32766, o); /* sum 65533 in int32: 32766 */
    TEST_ASSERT_EQUAL_INT16(32767, o[0]);
    TEST_ASSERT_EQUAL_INT16(32766, o[1]);
    (void)vr_up_push(&up, -32768, o); /* (32766-32768)/2 = -1 */
    TEST_ASSERT_EQUAL_INT16(-1, o[1]);
    (void)vr_up_push(&up, -32767, o); /* -65535/2 = -32767 (toward 0) */
    TEST_ASSERT_EQUAL_INT16(-32768, o[0]);
    TEST_ASSERT_EQUAL_INT16(-32767, o[1]);
}

/* ---- The renderer on the real voice ---- */

/* The clip decoded by the codec alone (the documented loop), upsampled by hand across the
 * packet boundaries: the renderer must give exactly that. */
static void test_single_clip_matches_codec_loop(void)
{
    NEED_PACK();
    vp_clip_t clip;
    TEST_ASSERT_TRUE(vp_find(&g_pack, ID_FRONT, &clip));
    static int16_t x[VP_CLIP_SAMPLES_MAX];
    size_t nx = 0;
    TEST_ASSERT_EQUAL_INT(0, vc_init(g_codec, g_codec_bytes));
    TEST_ASSERT_EQUAL_INT(0, vc_reset(g_codec));
    vc_trim_t trim;
    vc_trim_begin(&trim, clip.pre_skip, clip.valid_samples);
    vp_packet_iter_t it;
    vp_packets_begin(&clip, &it);
    const uint8_t *pkt;
    uint16_t len;
    int16_t pcm[VC_FRAME_SAMPLES];
    while (!vc_trim_done(&trim) && vp_packet_next(&it, &pkt, &len)) {
        TEST_ASSERT_EQUAL_INT(VC_FRAME_SAMPLES, vc_decode_20ms(g_codec, pkt, len, pcm));
        size_t first, count;
        vc_trim_take(&trim, VC_FRAME_SAMPLES, &first, &count);
        memcpy(x + nx, pcm + first, count * sizeof(int16_t));
        nx += count;
    }
    TEST_ASSERT_EQUAL_size_t(clip.valid_samples, nx);

    const vr_item_t items[1] = {{ID_FRONT, 0}};
    int32_t n = render_all(items, 1, 640, g_got);
    TEST_ASSERT_EQUAL_INT32((int32_t)(2u * nx), n);
    for (size_t i = 0; i < nx; i++) {
        int32_t next = (i + 1 < nx) ? x[i + 1] : x[i];
        TEST_ASSERT_EQUAL_INT16(x[i], g_got[2 * i]);
        TEST_ASSERT_EQUAL_INT16((int16_t)((x[i] + next) / 2), g_got[2 * i + 1]);
    }
}

static void test_gaps_exact_and_total(void)
{
    NEED_PACK();
    vp_clip_t a, b;
    TEST_ASSERT_TRUE(vp_find(&g_pack, ID_FRONT, &a));
    TEST_ASSERT_TRUE(vp_find(&g_pack, ID_BACK, &b));
    const vr_item_t items[3] = {{ID_FRONT, 500}, {ID_BACK, VP_WORD_GAP_MS}, {ID_FRONT, VP_PART_GAP_MS}};
    uint32_t expect = 2u * (a.valid_samples + b.valid_samples + a.valid_samples) +
                      (VP_WORD_GAP_MS + VP_PART_GAP_MS) * 32u;
    TEST_ASSERT_EQUAL_INT(0, vr_begin(&g_r, &g_pack, items, 3, g_codec, g_codec_bytes));
    TEST_ASSERT_EQUAL_UINT32(expect, vr_total_samples_32k(&g_r)); /* first gap ignored */
    int32_t n = render_all(items, 3, 4096, g_got);
    TEST_ASSERT_EQUAL_INT32((int32_t)expect, n);

    /* the word gap (VP_WORD_GAP_MS at 32 kHz) is exact silence between the clips */
    size_t g0 = 2u * a.valid_samples;
    for (size_t i = 0; i < VP_WORD_GAP_MS * 32u; i++) {
        TEST_ASSERT_EQUAL_INT16(0, g_got[g0 + i]);
    }
    /* and the clips on both sides are the clips alone (reset at each boundary) */
    const vr_item_t solo_a[1] = {{ID_FRONT, 0}};
    const vr_item_t solo_b[1] = {{ID_BACK, 0}};
    TEST_ASSERT_EQUAL_INT32((int32_t)(2u * a.valid_samples), render_all(solo_a, 1, 320, g_ref));
    TEST_ASSERT_EQUAL_INT16_ARRAY(g_ref, g_got, 2u * a.valid_samples);
    int32_t nb = render_all(solo_b, 1, 320, g_ref);
    TEST_ASSERT_EQUAL_INT32((int32_t)(2u * b.valid_samples), nb);
    TEST_ASSERT_EQUAL_INT16_ARRAY(g_ref, g_got + g0 + VP_WORD_GAP_MS * 32u, (size_t)nb);
}

static void test_pull_sizes_identical(void)
{
    NEED_PACK();
    const vr_item_t items[3] = {{ID_FRONT, 0}, {ID_BACK, VP_WORD_GAP_MS}, {ID_FRONT, VP_PART_GAP_MS}};
    int32_t nref = render_all(items, 3, 4096, g_ref);
    TEST_ASSERT_GREATER_THAN_INT32(0, nref);
    static const size_t sizes[] = {1, 7, 320, 640};
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        memset(g_got, 0x5A, sizeof g_got);
        int32_t n = render_all(items, 3, sizes[s], g_got);
        TEST_ASSERT_EQUAL_INT32(nref, n);
        TEST_ASSERT_EQUAL_INT16_ARRAY(g_ref, g_got, (size_t)nref);
    }
    /* after the end it stays finished */
    TEST_ASSERT_EQUAL_INT32(0, vr_pull(&g_r, g_got, 16));
}

static void test_real_voice_not_silent(void)
{
    NEED_PACK();
    vp_clip_t a;
    TEST_ASSERT_TRUE(vp_find(&g_pack, ID_FRONT, &a));
    const vr_item_t items[1] = {{ID_FRONT, 0}};
    int32_t n = render_all(items, 1, 320, g_got);
    TEST_ASSERT_EQUAL_INT32((int32_t)(2u * a.valid_samples), n);
    double sum = 0.0;
    for (int32_t i = 0; i < n; i++) {
        sum += (double)g_got[i] * (double)g_got[i];
    }
    double rms = sum / (double)n;
    TEST_ASSERT_TRUE_MESSAGE(rms > 300.0 * 300.0, "decoded voice is silent");
}

static void test_every_clip_length(void)
{
    NEED_PACK();
    /* each of the first 40 clips renders exactly twice its valid samples */
    for (uint16_t i = 0; i < 40u && i < g_pack.clip_count; i++) {
        vp_clip_t c;
        TEST_ASSERT_TRUE(vp_clip_at(&g_pack, i, &c));
        const vr_item_t it[1] = {{c.id, 0}};
        TEST_ASSERT_EQUAL_INT32((int32_t)(2u * c.valid_samples), render_all(it, 1, 640, g_got));
    }
}

static void test_begin_refusals(void)
{
    NEED_PACK();
    const vr_item_t missing[2] = {{ID_FRONT, 0}, {0x7FFE, 60}};
    TEST_ASSERT_EQUAL_INT(VR_ERR_CLIP, vr_begin(&g_r, &g_pack, missing, 2, g_codec, g_codec_bytes));
    const vr_item_t ok[1] = {{ID_FRONT, 0}};
    TEST_ASSERT_EQUAL_INT(VR_ERR_ARG, vr_begin(&g_r, &g_pack, ok, 0, g_codec, g_codec_bytes));
    TEST_ASSERT_EQUAL_INT(VR_ERR_STATE, vr_begin(&g_r, &g_pack, ok, 1, g_codec, 16));
    vr_item_t many[VR_ITEMS_MAX + 1u];
    for (size_t i = 0; i < VR_ITEMS_MAX + 1u; i++) {
        many[i].clip_id = ID_FRONT;
        many[i].gap_before_ms = 0;
    }
    TEST_ASSERT_EQUAL_INT(VR_ERR_ARG, vr_begin(&g_r, &g_pack, many, VR_ITEMS_MAX + 1u, g_codec, g_codec_bytes));
    vr_item_t gap[2] = {{ID_FRONT, 0}, {ID_BACK, 5000}};
    TEST_ASSERT_EQUAL_INT(0, vr_begin(&g_r, &g_pack, gap, 2, g_codec, g_codec_bytes)); /* 5 s: fine */
    vr_item_t longest[16];
    for (size_t i = 0; i < 16u; i++) {
        longest[i].clip_id = ID_FRONT;
        longest[i].gap_before_ms = 65535;
    }
    TEST_ASSERT_EQUAL_INT(VR_ERR_TOO_LONG, vr_begin(&g_r, &g_pack, longest, 16, g_codec, g_codec_bytes));
    TEST_ASSERT_EQUAL_INT(VR_ERR_ARG, vr_pull(&g_r, g_got, 0));
}

/* A packet that is not one 20 ms frame, found mid-clip: the pull fails and so does every later one. */
static void test_decode_error_cancels(void)
{
    NEED_PACK();
    uint8_t *copy = malloc(g_len);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, g_file, g_len);
    vp_pack_t pack;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(copy, g_len, &pack, NULL));
    vp_clip_t clip;
    TEST_ASSERT_TRUE(vp_find(&pack, ID_FRONT, &clip));
    vp_packet_iter_t it;
    vp_packets_begin(&clip, &it);
    const uint8_t *pkt;
    uint16_t len;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(vp_packet_next(&it, &pkt, &len));
    }
    copy[pkt - pack.base] |= 3u; /* TOC code 3: a multi-frame packet */

    const vr_item_t items[2] = {{ID_FRONT, 0}, {ID_BACK, 60}};
    TEST_ASSERT_EQUAL_INT(0, vr_begin(&g_r, &pack, items, 2, g_codec, g_codec_bytes));
    int32_t got = 0;
    int32_t rc;
    do {
        rc = vr_pull(&g_r, g_got + got, 320);
        if (rc > 0) {
            got += rc;
        }
    } while (rc > 0);
    TEST_ASSERT_LESS_THAN_INT32(0, rc);
    TEST_ASSERT_LESS_THAN_INT32((int32_t)(2u * clip.valid_samples), got);
    TEST_ASSERT_LESS_THAN_INT32(0, vr_pull(&g_r, g_got, 320));
    TEST_ASSERT_LESS_THAN_INT32(0, vr_pull(&g_r, g_got, 1));
    free(copy);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_upsampler_vectors);
    RUN_TEST(test_upsampler_extremes_and_empty);
    RUN_TEST(test_single_clip_matches_codec_loop);
    RUN_TEST(test_gaps_exact_and_total);
    RUN_TEST(test_pull_sizes_identical);
    RUN_TEST(test_real_voice_not_silent);
    RUN_TEST(test_every_clip_length);
    RUN_TEST(test_begin_refusals);
    RUN_TEST(test_decode_error_cancels);
    return UNITY_END();
}
