/*
 * speech_boost.h - the Voice volume on speech, up to louder than the pack at the top (AUD-033,
 * docs/features/08).
 *
 * The voice packs hold clear speech: each word levelled to the same voiced loudness, its peaks
 * under -1 dBFS, nothing compressed or limited (tools/voice/speaker.py). The panel makes it as loud
 * as the Voice volume asks, as decided (2026-10-10):
 *   - every 10 % of the volume is 3 dB: at SB_UNITY_VOLUME (70 %, the default) the speech plays
 *     as the pack has it, below that only quieter (-6 dB at 50 %, -18 dB at 10 %), and above it
 *     louder, up to SB_BOOST_DB (+9 dB) of gain at 100 %; 0 % is silent;
 *   - where the gain lifts a peak over -1 dBFS, a limiter with 2 ms of look-ahead holds it there.
 *     At a gain that cannot lift any sample over it (-1 dB and less: the volume up to 66 %), the
 *     stage only scales the samples, with no delay.
 * The packs' words already peak near -1 dBFS, so the limiter takes back part of a gain over the
 * pack's level: on pack revision 7's words, +3 dB of gain (80 %) is heard as about +2.3 dB, +6 dB
 * (90 %) as +3.9 dB and +9 dB (100 %) as +4.7 dB (the median, measured 2026-10-10). The volume was
 * the gain in proportion before (the pack's level at 35 %): 50 % and 100 % were then heard only
 * about 2.4 dB apart.
 * The mixer passes speech at unity (6 dB under the entry beeps and the fast exit beeps): the Voice
 * volume is applied here, once per utterance, before the mixer.
 *
 * Plain C11 floats, no heap. 32 kHz mono int16, as voice_render makes it. One utterance at a time:
 * sb_begin() at its start (the gain stays until the next), sb_process() on every pull in order,
 * then sb_flush() until it returns 0: the limiter holds back SB_DELAY samples. Pulls of any size
 * give the same samples. The audio bench's audio (AUD-030) goes through it as an utterance does.
 */
#ifndef SOUNDER_SPEECH_BOOST_H
#define SOUNDER_SPEECH_BOOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_RATE 32000u
#define SB_LOOKAHEAD 64u               /* 2 ms at 32 kHz */
#define SB_DELAY (SB_LOOKAHEAD - 1u)   /* samples the limiter holds back, given out by sb_flush */
#define SB_BOOST_DB 9.0f               /* the gain at a Voice volume of 100 % */
#define SB_UNITY_VOLUME 70u            /* the Voice volume that plays the pack as it is */
#define SB_CEILING_DB (-1.0f)

typedef struct {
    float gain;     /* linear: sb_gain() of the Voice volume */
    bool limiting;  /* the gain can lift a sample over the ceiling: the limiter runs */
    float ceiling, lim_g, lim_kr;
    float req[SB_LOOKAHEAD];   /* the gain each of the last samples needs */
    float mins[SB_LOOKAHEAD];  /* the least of req over the window, for the last samples */
    float held[SB_LOOKAHEAD];  /* the last samples, waiting for their gain */
    uint32_t pos;              /* samples taken in */
    uint32_t flushed;          /* of SB_DELAY */
} speech_boost_t;

/* The gain a Voice volume gives speech (linear): 3 dB for every 10 %, 1.0 at SB_UNITY_VOLUME,
 * +9 dB at 100 %; 0 at 0 %; a volume over 100 counts as 100. */
float sb_gain(uint8_t voice_volume);

/* Start an utterance at this Voice volume. */
void sb_begin(speech_boost_t *b, uint8_t voice_volume);

/* The next n samples of the utterance, in place. While the limiter runs, what comes out is
 * SB_DELAY samples late (silence first). */
void sb_process(speech_boost_t *b, int16_t *pcm, size_t n);

/* After the utterance's last sample: up to max of the samples still held back. Returns how many
 * were written; 0 once all are out (always 0 when the limiter doesn't run). */
size_t sb_flush(speech_boost_t *b, int16_t *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* SOUNDER_SPEECH_BOOST_H */
