/*
 * test_speech_boost.c - the Voice volume on speech, up to louder than the pack at the top (AUD-033,
 * components/core/sounder/include/sounder/speech_boost.h). The tests listen to what comes out:
 * tones and clicks at a known level through the stage.
 */
#include <math.h>
#include <string.h>

#include "sounder/speech_boost.h"
#include "unity.h"

#define N (SB_RATE / 2u) /* 500 ms */
#define PI_F 3.14159265f

static speech_boost_t B;
static int16_t in[N], out[N + SB_DELAY];

void setUp(void) {}
void tearDown(void) {}

static void tone(int16_t *x, size_t n, float hz, float rms_db)
{
    const float amp = 1.41421356f * powf(10.0f, rms_db / 20.0f) * 32767.0f;
    for (size_t i = 0; i < n; i++) {
        x[i] = (int16_t)lrintf(amp * sinf(2.0f * PI_F * hz * (float)i / (float)SB_RATE));
    }
}

/* The whole utterance through the stage in pulls of `chunk`, flush included. Returns its length. */
static size_t run(uint8_t volume, const int16_t *x, size_t n, size_t chunk)
{
    sb_begin(&B, volume);
    memcpy(out, x, n * sizeof *x);
    for (size_t at = 0; at < n; at += chunk) {
        sb_process(&B, out + at, n - at < chunk ? n - at : chunk);
    }
    size_t m = n, got;
    while ((got = sb_flush(&B, out + m, chunk)) > 0u) {
        m += got;
    }
    return m;
}

static float rms_db(const int16_t *x, size_t from, size_t to)
{
    double s = 0.0;
    for (size_t i = from; i < to; i++) {
        s += (double)x[i] * (double)x[i];
    }
    return (float)(10.0 * log10(s / (double)(to - from) / (32768.0 * 32768.0)));
}

/* AUD-033: every 10 % of the Voice volume is 3 dB: none at 0 %, the pack's own level at 70 %,
 * -6 dB at 50 %, +9 dB at 100 % (and no more above it). */
static void test_the_gain_follows_the_voice_volume(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sb_gain(0u));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sb_gain(70u));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -6.0f, 20.0f * log10f(sb_gain(50u)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -18.0f, 20.0f * log10f(sb_gain(10u)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, SB_BOOST_DB, 20.0f * log10f(sb_gain(100u)));
    TEST_ASSERT_EQUAL_FLOAT(sb_gain(100u), sb_gain(255u));
    for (unsigned v = 1u; v + 10u <= 100u; v++) {
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f,
                                 20.0f * log10f(sb_gain((uint8_t)(v + 10u)) / sb_gain((uint8_t)v)));
    }
}

/* AUD-033: up to the volume whose gain cannot lift a sample over -1 dBFS (66 %; 67 % can), the
 * stage only scales the speech: no delay, nothing held back, sample for sample. At 0 % nothing is
 * heard. */
static void test_low_volumes_only_scale_the_speech(void)
{
    for (size_t i = 0; i < N; i++) {
        in[i] = (int16_t)((int32_t)(i * 7919u % 65536u) - 32768);
    }
    const uint8_t volumes[] = {0u, 10u, 50u, 66u};
    for (size_t k = 0; k < sizeof volumes; k++) {
        TEST_ASSERT_EQUAL_size_t(N, run(volumes[k], in, N, 640u));
        const float g = sb_gain(volumes[k]);
        for (size_t i = 0; i < N; i++) {
            TEST_ASSERT_EQUAL_INT16((int16_t)lrintf((float)in[i] / 32768.0f * g * 32768.0f), out[i]);
        }
    }
    TEST_ASSERT_EQUAL_size_t(N + SB_DELAY, run(67u, in, N, 640u));
}

/* AUD-033: speech whose peaks stay under the ceiling at the gain keeps its shape: a -15 dB tone
 * (peaks at -12 dBFS) comes out exactly the gain louder at 100 %, only SB_DELAY samples late. */
static void test_speech_under_the_ceiling_is_only_louder(void)
{
    tone(in, N, 1000.0f, -15.0f);
    TEST_ASSERT_EQUAL_size_t(N + SB_DELAY, run(100u, in, N, 640u));
    for (size_t i = 0; i < SB_DELAY; i++) {
        TEST_ASSERT_EQUAL_INT16(0, out[i]);
    }
    for (size_t i = 0; i < N; i++) {
        const float want = (float)in[i] / 32768.0f * sb_gain(100u) * 32768.0f;
        TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrintf(want), out[i + SB_DELAY]);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.05f, SB_BOOST_DB, rms_db(out, SB_DELAY, N + SB_DELAY) - rms_db(in, 0u, N));
}

/* AUD-033: turned up, the flush gives back exactly what was held, and pulls of any size give the
 * same samples. */
static void test_the_look_ahead_is_given_back_and_pulls_do_not_matter(void)
{
    tone(in, N, 1000.0f, -6.0f);
    static int16_t first[N + SB_DELAY];
    const size_t chunks[] = {1u, 37u, 320u, N};
    for (size_t k = 0; k < sizeof chunks / sizeof chunks[0]; k++) {
        TEST_ASSERT_EQUAL_size_t(N + SB_DELAY, run(80u, in, N, chunks[k]));
        if (k == 0u) {
            memcpy(first, out, sizeof first);
        } else {
            TEST_ASSERT_EQUAL_INT16_ARRAY(first, out, N + SB_DELAY);
        }
    }
    int16_t spare[4];
    TEST_ASSERT_EQUAL_size_t(0u, sb_flush(&B, spare, 4u)); /* all given back already */
}

/* AUD-033: whatever goes in and at whatever volume, nothing comes out over -1 dBFS: a full-scale
 * square wave with clicks, and a quiet start with a loud onset. */
static void test_the_peaks_stay_under_the_ceiling(void)
{
    const int16_t ceiling = (int16_t)(32768.0f * powf(10.0f, SB_CEILING_DB / 20.0f) + 1.0f);
    for (size_t i = 0; i < N; i++) {
        in[i] = (int16_t)((i / 40u) % 2u ? 32767 : -32768);
    }
    for (size_t i = N / 2u; i < N; i += 997u) {
        in[i - 200u] = 0;
    }
    for (unsigned v = 0u; v <= 100u; v += 9u) {
        const size_t m = run((uint8_t)v, in, N, 640u);
        for (size_t i = 0; i < m; i++) {
            TEST_ASSERT_TRUE_MESSAGE(out[i] <= ceiling && out[i] >= -ceiling, "over the ceiling");
        }
    }
    tone(in, N / 2u, 1000.0f, -40.0f);
    tone(in + N / 2u, N / 2u, 1000.0f, -4.0f);
    const size_t m = run(100u, in, N, 640u);
    for (size_t i = 0; i < m; i++) {
        TEST_ASSERT_TRUE(out[i] <= ceiling && out[i] >= -ceiling);
    }
}

/* AUD-033: the limiter only takes what the peaks need: after a loud onset it lets go, and a quiet
 * part 300 ms later is the full gain louder again. */
static void test_the_limiter_lets_go_after_a_peak(void)
{
    memset(in, 0, sizeof in);
    tone(in, 1600u, 1000.0f, -3.0f);                      /* 50 ms, limited at 100 % */
    tone(in + N / 2u, N / 2u, 1000.0f, -20.0f);           /* 250 ms quiet, under the ceiling */
    run(100u, in, N, 640u);
    const float quiet = rms_db(out, N / 2u + 1600u + SB_DELAY, N + SB_DELAY) -
                        rms_db(in, N / 2u + 1600u, N);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, SB_BOOST_DB, quiet);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_gain_follows_the_voice_volume);
    RUN_TEST(test_low_volumes_only_scale_the_speech);
    RUN_TEST(test_speech_under_the_ceiling_is_only_louder);
    RUN_TEST(test_the_look_ahead_is_given_back_and_pulls_do_not_matter);
    RUN_TEST(test_the_peaks_stay_under_the_ceiling);
    RUN_TEST(test_the_limiter_lets_go_after_a_peak);
    return UNITY_END();
}
