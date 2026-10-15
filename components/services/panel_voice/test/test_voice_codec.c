/*
 * test_voice_codec.c - the voice's Opus adapter (docs/features/08 voice-first-build.md D08, D10).
 * Requirements: AUD-028
 *
 * The clips are made here with libopus's own encoder (fixed point, deterministic): a 0.5 s tone
 * at 16 kHz mono, 32 kbps VBR, 20 ms frames, padded with silence to whole frames as the voice
 * tools do, then decoded through voice_codec with the pack's pre-skip and final trim.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"
#include "unity.h"
#include "voice_codec.h"

#define CLIP_VALID 8000u /* 0.5 s */
#define MAX_PACKETS 32u

typedef struct {
    uint8_t data[MAX_PACKETS][VC_MAX_PACKET_BYTES];
    size_t len[MAX_PACKETS];
    unsigned count;
    uint32_t pre_skip;
    uint32_t valid;
    int16_t source[MAX_PACKETS * VC_FRAME_SAMPLES];
} clip_t;

static void *g_state;
static clip_t g_tone440;
static clip_t g_tone660;
static int16_t g_out[MAX_PACKETS * VC_FRAME_SAMPLES];

/* A sine without libm: y[n] = 2cos(w)y[n-1] - y[n-2], from y[-1] = -sin(w), y[0] = 0. */
static void make_tone(int16_t *pcm, size_t n, size_t tone, double two_cos, double sin_w)
{
    double prev = -sin_w;
    double cur = 0.0;
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (i < tone) ? (int16_t)(cur * 12000.0) : 0;
        double next = two_cos * cur - prev;
        prev = cur;
        cur = next;
    }
}

static void encode_clip(clip_t *clip, double two_cos, double sin_w)
{
    int esize = opus_encoder_get_size(1);
    TEST_ASSERT_GREATER_THAN_INT(0, esize);
    OpusEncoder *enc = malloc((size_t)esize);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_EQUAL_INT(OPUS_OK, opus_encoder_init(enc, VC_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO));
    TEST_ASSERT_EQUAL_INT(OPUS_OK, opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000)));
    TEST_ASSERT_EQUAL_INT(OPUS_OK, opus_encoder_ctl(enc, OPUS_SET_VBR(1)));
    opus_int32 lookahead = 0;
    TEST_ASSERT_EQUAL_INT(OPUS_OK, opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead)));
    TEST_ASSERT_GREATER_THAN_INT(0, lookahead);
    TEST_ASSERT_LESS_OR_EQUAL_INT(VC_FRAME_SAMPLES, lookahead);

    clip->pre_skip = (uint32_t)lookahead;
    clip->valid = CLIP_VALID;
    /* D08: pre_skip + valid <= 320 x packets, and fewer than 320 samples of final padding. */
    clip->count = (unsigned)((clip->pre_skip + clip->valid + VC_FRAME_SAMPLES - 1) / VC_FRAME_SAMPLES);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(MAX_PACKETS, clip->count);
    size_t total = (size_t)clip->count * VC_FRAME_SAMPLES;
    make_tone(clip->source, total, CLIP_VALID, two_cos, sin_w);
    for (unsigned p = 0; p < clip->count; p++) {
        opus_int32 n = opus_encode(enc, &clip->source[p * VC_FRAME_SAMPLES], VC_FRAME_SAMPLES,
                                   clip->data[p], VC_MAX_PACKET_BYTES);
        TEST_ASSERT_GREATER_THAN_INT(0, n);
        clip->len[p] = (size_t)n;
    }
    free(enc);
}

/* Decode a whole clip from a reset state with D08's trim; returns the samples played. */
static size_t decode_clip(void *state, const clip_t *clip, int16_t *out)
{
    TEST_ASSERT_EQUAL_INT(0, vc_reset(state));
    vc_trim_t trim;
    vc_trim_begin(&trim, clip->pre_skip, clip->valid);
    size_t played = 0;
    size_t discarded_tail = 0;
    for (unsigned p = 0; p < clip->count; p++) {
        int16_t pcm[VC_FRAME_SAMPLES];
        int n = vc_decode_20ms(state, clip->data[p], clip->len[p], pcm);
        TEST_ASSERT_EQUAL_INT(VC_FRAME_SAMPLES, n);
        size_t first = 99;
        size_t count = 99;
        vc_trim_take(&trim, (size_t)n, &first, &count);
        TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)n, first + count);
        memcpy(&out[played], &pcm[first], count * sizeof(int16_t));
        played += count;
        if (vc_trim_done(&trim)) {
            discarded_tail += (size_t)n - first - count;
        }
    }
    TEST_ASSERT_TRUE(vc_trim_done(&trim));
    TEST_ASSERT_LESS_THAN_size_t(VC_FRAME_SAMPLES, discarded_tail);
    return played;
}

void setUp(void)
{
    if (g_tone440.count == 0) {
        /* 2cos(w), sin(w) for w = 2*pi*f/16000. */
        encode_clip(&g_tone440, 1.9702186523, 0.1719291003);
        encode_clip(&g_tone660, 1.9332002040, 0.2562893731);
    }
    size_t bytes = vc_state_bytes();
    g_state = malloc(bytes);
    TEST_ASSERT_NOT_NULL(g_state);
    TEST_ASSERT_EQUAL_INT(0, vc_init(g_state, bytes));
}

void tearDown(void)
{
    free(g_state);
    g_state = NULL;
}

/* AUD-028 (D10): caller-owned state of opus_decoder_get_size(1) bytes, at most 64 KiB. */
static void test_state_size_is_bounded(void)
{
    size_t bytes = vc_state_bytes();
    TEST_ASSERT_GREATER_THAN_size_t(0, bytes);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(VC_MAX_STATE_BYTES, bytes);
    TEST_ASSERT_EQUAL_size_t((size_t)opus_decoder_get_size(1), bytes);
}

/* AUD-028: a state too small, or none, is refused. */
static void test_init_refuses_bad_state(void)
{
    size_t bytes = vc_state_bytes();
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_init(NULL, bytes));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_init(g_state, bytes - 1));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_init(g_state, 0));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_reset(NULL));
}

/* AUD-028 (D08): exactly valid_samples come out - pre-skip dropped, final padding dropped - and
 * they are the tone, in place: the trim lines the decoded audio up with what was encoded. */
static void test_clip_decodes_to_exact_length(void)
{
    size_t played = decode_clip(g_state, &g_tone440, g_out);
    TEST_ASSERT_EQUAL_size_t(CLIP_VALID, played);

    /* Not silent, and in phase with the source: correlation^2 >= 0.81 x energies (r >= 0.9). */
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (size_t i = 400; i < CLIP_VALID - 400; i++) {
        double x = g_tone440.source[i];
        double y = g_out[i];
        xy += x * y;
        xx += x * x;
        yy += y * y;
    }
    TEST_ASSERT_TRUE(yy > 1000.0 * 1000.0 * (double)(CLIP_VALID - 800));
    TEST_ASSERT_TRUE(xy > 0.0);
    TEST_ASSERT_TRUE(xy * xy >= 0.81 * xx * yy);
}

/* AUD-028 (D08): a clip after a reset decodes exactly as on a fresh decoder, whatever came
 * before it. */
static void test_reset_between_clips(void)
{
    static int16_t fresh[MAX_PACKETS * VC_FRAME_SAMPLES];
    TEST_ASSERT_EQUAL_size_t(CLIP_VALID, decode_clip(g_state, &g_tone660, fresh));

    TEST_ASSERT_EQUAL_size_t(CLIP_VALID, decode_clip(g_state, &g_tone440, g_out));
    TEST_ASSERT_EQUAL_size_t(CLIP_VALID, decode_clip(g_state, &g_tone660, g_out));
    TEST_ASSERT_EQUAL_INT16_ARRAY(fresh, g_out, CLIP_VALID);
}

/* AUD-028 (D08): each packet of a pack describes exactly 20 ms at 16 kHz. */
static void test_packet_check(void)
{
    for (unsigned p = 0; p < g_tone440.count; p++) {
        TEST_ASSERT_EQUAL_INT(0, vc_packet_check(g_tone440.data[p], g_tone440.len[p]));
    }
    static uint8_t big[VC_MAX_PACKET_BYTES + 1];
    memcpy(big, g_tone440.data[0], g_tone440.len[0]);
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_packet_check(NULL, 10));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_packet_check(big, 0));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_packet_check(big, sizeof big));

    /* TOC config 1 = SILK NB 20 ms, code 0: 320 samples. */
    const uint8_t one20[] = { (1u << 3) | 0u, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, vc_packet_check(one20, sizeof one20));
    /* Config 0 = SILK NB 10 ms, code 0: one 10 ms frame. */
    const uint8_t one10[] = { 0x00, 0x00 };
    TEST_ASSERT_EQUAL_INT(VC_ERR_PACKET, vc_packet_check(one10, sizeof one10));
    /* Config 2 = SILK NB 40 ms. */
    const uint8_t one40[] = { (2u << 3), 0x00 };
    TEST_ASSERT_EQUAL_INT(VC_ERR_PACKET, vc_packet_check(one40, sizeof one40));
    /* Config 1, code 1: two 20 ms frames = 40 ms. */
    const uint8_t two20[] = { (1u << 3) | 1u, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_INT(VC_ERR_PACKET, vc_packet_check(two20, sizeof two20));
    /* Code 3 without its frame-count byte: malformed. */
    const uint8_t code3[] = { (1u << 3) | 3u };
    TEST_ASSERT_EQUAL_INT(VC_ERR_PACKET, vc_packet_check(code3, sizeof code3));
}

/* AUD-028: malformed packets are refused without crashing (the sanitizer build checks the
 * reads), the output buffer is left alone when the packet is refused before decoding, and a
 * reset afterwards gives a clean decoder. */
static void test_malformed_packet_rejected(void)
{
    int16_t pcm[VC_FRAME_SAMPLES];
    for (size_t i = 0; i < VC_FRAME_SAMPLES; i++) {
        pcm[i] = 0x5A5A;
    }
    /* Too short to say anything. */
    const uint8_t code3[] = { (1u << 3) | 3u };
    TEST_ASSERT_EQUAL_INT(VC_ERR_PACKET, vc_decode_20ms(g_state, code3, sizeof code3, pcm));
    TEST_ASSERT_EQUAL_INT16(0x5A5A, pcm[0]);
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_decode_20ms(g_state, NULL, 3, pcm));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_decode_20ms(NULL, code3, 1, pcm));
    TEST_ASSERT_EQUAL_INT(VC_ERR_ARG, vc_decode_20ms(g_state, code3, 1, NULL));

    /* 2 x 10 ms (code 2, passes the TOC check) whose first frame claims 200 bytes of 1. */
    const uint8_t lying[] = { 0x02, 200, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, vc_packet_check(lying, sizeof lying));
    TEST_ASSERT_EQUAL_INT(VC_ERR_DECODE, vc_decode_20ms(g_state, lying, sizeof lying, pcm));

    /* Garbage after a valid TOC, and truncated real packets: 320 samples or an error, nothing
     * else (a deterministic pseudo-random sweep). */
    uint32_t seed = 0x2468ACE1u;
    static uint8_t junk[VC_MAX_PACKET_BYTES];
    for (unsigned round = 0; round < 300; round++) {
        for (size_t i = 0; i < sizeof junk; i++) {
            seed = seed * 1664525u + 1013904223u;
            junk[i] = (uint8_t)(seed >> 24);
        }
        size_t len = 1u + (size_t)((seed >> 8) % VC_MAX_PACKET_BYTES);
        int n = vc_decode_20ms(g_state, junk, len, pcm);
        TEST_ASSERT_TRUE(n == VC_FRAME_SAMPLES || n < 0);
    }
    for (unsigned p = 0; p < g_tone440.count; p++) {
        for (size_t cut = 1; cut < g_tone440.len[p]; cut += 7) {
            int n = vc_decode_20ms(g_state, g_tone440.data[p], cut, pcm);
            TEST_ASSERT_TRUE(n == VC_FRAME_SAMPLES || n < 0);
        }
    }

    /* After all that, a reset decoder plays a clip exactly as a fresh one. */
    static int16_t fresh[MAX_PACKETS * VC_FRAME_SAMPLES];
    void *other = malloc(vc_state_bytes());
    TEST_ASSERT_NOT_NULL(other);
    TEST_ASSERT_EQUAL_INT(0, vc_init(other, vc_state_bytes()));
    TEST_ASSERT_EQUAL_size_t(CLIP_VALID, decode_clip(other, &g_tone440, fresh));
    free(other);
    TEST_ASSERT_EQUAL_size_t(CLIP_VALID, decode_clip(g_state, &g_tone440, g_out));
    TEST_ASSERT_EQUAL_INT16_ARRAY(fresh, g_out, CLIP_VALID);
}

/* AUD-028 (D08): the trim on its own - pre-skip across a whole frame, the end mid-frame. */
static void test_trim_rule(void)
{
    vc_trim_t t;
    size_t first = 0;
    size_t count = 0;

    vc_trim_begin(&t, 320, 500);
    vc_trim_take(&t, 320, &first, &count); /* all pre-skip */
    TEST_ASSERT_EQUAL_size_t(320, first);
    TEST_ASSERT_EQUAL_size_t(0, count);
    vc_trim_take(&t, 320, &first, &count);
    TEST_ASSERT_EQUAL_size_t(0, first);
    TEST_ASSERT_EQUAL_size_t(320, count);
    TEST_ASSERT_FALSE(vc_trim_done(&t));
    vc_trim_take(&t, 320, &first, &count); /* 180 valid, 140 padding */
    TEST_ASSERT_EQUAL_size_t(0, first);
    TEST_ASSERT_EQUAL_size_t(180, count);
    TEST_ASSERT_TRUE(vc_trim_done(&t));
    vc_trim_take(&t, 320, &first, &count); /* beyond the clip: nothing */
    TEST_ASSERT_EQUAL_size_t(0, count);

    vc_trim_begin(&t, 104, 10);
    vc_trim_take(&t, 320, &first, &count); /* pre-skip and the whole clip in one frame */
    TEST_ASSERT_EQUAL_size_t(104, first);
    TEST_ASSERT_EQUAL_size_t(10, count);
    TEST_ASSERT_TRUE(vc_trim_done(&t));
}

/* AUD-028: the libopus the build pinned (D10 provenance: printed for the record). */
static void test_version_string(void)
{
    const char *v = opus_get_version_string();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT(0, strncmp(v, "libopus ", 8));
    TEST_MESSAGE(v);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_state_size_is_bounded);
    RUN_TEST(test_init_refuses_bad_state);
    RUN_TEST(test_clip_decodes_to_exact_length);
    RUN_TEST(test_reset_between_clips);
    RUN_TEST(test_packet_check);
    RUN_TEST(test_malformed_packet_rejected);
    RUN_TEST(test_trim_rule);
    RUN_TEST(test_version_string);
    return UNITY_END();
}
