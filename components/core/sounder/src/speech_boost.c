/*
 * speech_boost.c - the Voice volume on speech, up to louder than the pack at the top (AUD-033);
 * see speech_boost.h.
 */
#include "sounder/speech_boost.h"

#include <math.h>
#include <string.h>

#define SB_LN10_20 0.115129255f /* ln(10) / 20: dB to a gain with expf */
#define SB_RELEASE_MS 60.0f

float sb_gain(uint8_t voice_volume)
{
    if (voice_volume == 0u) {
        return 0.0f;
    }
    const float v = (float)(voice_volume > 100u ? 100u : voice_volume);
    const float db = SB_BOOST_DB * (v - (float)SB_UNITY_VOLUME) / (100.0f - (float)SB_UNITY_VOLUME);
    return expf(db * SB_LN10_20);
}

void sb_begin(speech_boost_t *b, uint8_t voice_volume)
{
    memset(b, 0, sizeof *b);
    b->gain = sb_gain(voice_volume);
    b->ceiling = expf(SB_CEILING_DB * SB_LN10_20);
    /* An int16 sample is at most 1.0 of full scale: at this gain or less none reaches the ceiling. */
    b->limiting = b->gain > b->ceiling;
    b->lim_g = 1.0f;
    b->lim_kr = 1.0f - expf(-1.0f / ((float)SB_RATE * SB_RELEASE_MS / 1000.0f));
    for (unsigned i = 0; i < SB_LOOKAHEAD; i++) {
        b->req[i] = 1.0f;
        b->mins[i] = 1.0f;
    }
}

/* The limiter: takes one sample in and gives out the one SB_DELAY before it. The gain a sample
 * gets is the mean, over the look-ahead, of the least gain any sample in reach needs, so it is
 * never more than its own need and falls smoothly into a peak; it comes back over about 60 ms. */
static float limit(speech_boost_t *b, float x)
{
    const uint32_t k = b->pos % SB_LOOKAHEAD;
    const float mag = fabsf(x);
    b->req[k] = mag > b->ceiling ? b->ceiling / mag : 1.0f;
    const float out = b->held[(k + 1u) % SB_LOOKAHEAD]; /* the oldest, SB_DELAY back */
    b->held[k] = x;
    float least = 1.0f;
    for (unsigned i = 0; i < SB_LOOKAHEAD; i++) {
        least = b->req[i] < least ? b->req[i] : least;
    }
    b->mins[k] = least;
    float sum = 0.0f;
    for (unsigned i = 0; i < SB_LOOKAHEAD; i++) {
        sum += b->mins[i];
    }
    const float g = sum / (float)SB_LOOKAHEAD;
    b->lim_g = g < b->lim_g ? g : b->lim_g + (g - b->lim_g) * b->lim_kr;
    b->pos++;
    return out * b->lim_g;
}

static int16_t to_pcm(float y)
{
    float v = y * 32768.0f;
    v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
    return (int16_t)lrintf(v);
}

void sb_process(speech_boost_t *b, int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const float y = (float)pcm[i] / 32768.0f * b->gain;
        pcm[i] = to_pcm(b->limiting ? limit(b, y) : y);
    }
}

size_t sb_flush(speech_boost_t *b, int16_t *out, size_t max)
{
    if (!b->limiting) {
        return 0u;
    }
    size_t w = 0;
    while (w < max && b->flushed < SB_DELAY) {
        out[w++] = to_pcm(limit(b, 0.0f));
        b->flushed++;
    }
    return w;
}
