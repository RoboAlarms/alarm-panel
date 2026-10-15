/*
 * test_sounder.c - what the panel sounds like (components/core/sounder).
 *
 * The mixer is the one copy of the patterns: the panel's speakers, the PC simulator and these
 * tests all render the same blocks, so a pattern that is wrong here is wrong on the wall.
 * The tests listen to the rendered PCM rather than reading the tables back, which is the only
 * way to catch a ramp, an envelope or a step boundary that eats part of a beep.
 *
 * Requirements: AUD-001..006, AUD-010 and AUD-028 for speech in the mix and tone tickets, AUD-032
 * for the three everyday volumes
 * (docs/features/08-Audio-and-Annunciation, voice-first-build.md D04, D11).
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "sounder/sounder_mix.h"
#include "unity.h"

#define MAX_SECONDS 8u
#define MAX_MS      (MAX_SECONDS * 1000u)

static snd_mixer_t M;
static int16_t s_block[SND_BLOCK_FRAMES * 2u];
/* The left channel of everything rendered so far, one entry per sample. */
static int16_t s_pcm[MAX_SECONDS * SND_SAMPLE_RATE];
static size_t s_n;
/* The amplifier's state at the end of the last rendered block. */
static bool s_amp;
/* The rate the mixer renders at: the default, or a board's (AUD-006). */
static uint32_t s_rate = SND_SAMPLE_RATE;

void setUp(void);
void tearDown(void);

void setUp(void)
{
    snd_init(&M);
    TEST_ASSERT_TRUE(snd_set_rate(&M, s_rate));
    s_n = 0;
    s_amp = false;
}

void tearDown(void)
{
    s_rate = SND_SAMPLE_RATE; /* a failed rate test must not leak its rate into the next */
}

/* ---- rendering and listening ------------------------------------------------------------ */

/* Render one 10 ms block, appending its left channel to s_pcm. Returns the block report. */
static snd_block_t render_block(void)
{
    const size_t frames = snd_block_frames(&M);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(SND_BLOCK_FRAMES, frames);
    const snd_block_t b = snd_render(&M, s_block, frames);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(s_pcm) / sizeof(s_pcm[0]), s_n + frames);
    for (size_t i = 0; i < frames; i++) {
        /* Both speakers get the same signal: check it once here rather than in every test. */
        TEST_ASSERT_EQUAL_INT16(s_block[2u * i], s_block[2u * i + 1u]);
        s_pcm[s_n++] = s_block[2u * i];
    }
    s_amp = b.amp;
    return b;
}

static void render_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10u) {
        (void)render_block();
    }
}

/* Forget what has been heard so far, so the next measurement starts at 0 ms. */
static void listen_again(void)
{
    s_n = 0;
}

static int16_t peak(void)
{
    int32_t top = 0;
    for (size_t i = 0; i < s_n; i++) {
        const int32_t v = s_pcm[i] < 0 ? -(int32_t)s_pcm[i] : s_pcm[i];
        if (v > top) {
            top = v;
        }
    }
    return (int16_t)top;
}

/* Is there any sound in the 2 ms starting at this millisecond? A single sample is no use:
 * every tone crosses zero, and the 3 ms ramps start below one quantization step. */
#define WINDOW_MS 2u

static bool sounding_at(uint32_t ms)
{
    const size_t from = (size_t)ms * s_rate / 1000u;
    size_t to = from + (size_t)WINDOW_MS * s_rate / 1000u;
    if (to > s_n) {
        to = s_n;
    }
    for (size_t i = from; i < to; i++) {
        if (s_pcm[i] != 0) {
            return true;
        }
    }
    return false;
}

/* The beeps and gaps heard, in milliseconds: seg[0] is the first run of sound. A run shorter
 * than the 2 ms window is not a beep anybody could hear, so the detector never reports one. */
#define MAX_SEGS 32u

typedef struct {
    uint32_t ms[MAX_SEGS];
    bool on[MAX_SEGS];
    size_t count;
} segments_t;

static void segments(segments_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint32_t total = (uint32_t)(s_n * 1000u / s_rate);
    bool state = sounding_at(0);
    uint32_t start = 0;
    for (uint32_t ms = 1; ms <= total; ms++) {
        const bool now = ms == total ? !state : sounding_at(ms);
        if (now == state) {
            continue;
        }
        if (out->count < MAX_SEGS) {
            out->on[out->count] = state;
            out->ms[out->count++] = ms - start;
        }
        state = now;
        start = ms;
    }
}

/* The ramps and the window above blur each edge by a millisecond or two; a pattern is wrong
 * long before it is 8 ms wrong. */
#define EDGE_MS 8u

static void assert_segment(const segments_t *s, size_t i, bool on, uint32_t ms, const char *what)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: segment %u", what, (unsigned)i);
    TEST_ASSERT_TRUE_MESSAGE(i < s->count, msg);
    TEST_ASSERT_EQUAL_MESSAGE(on, s->on[i], msg);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(EDGE_MS, ms, s->ms[i], msg);
}

/* The tone in the steady middle of a beep, from its zero crossings. */
static uint32_t tone_hz(uint32_t from_ms, uint32_t to_ms)
{
    const size_t from = (size_t)from_ms * s_rate / 1000u;
    const size_t to = (size_t)to_ms * s_rate / 1000u;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(s_n, to);
    uint32_t crossings = 0;
    int sign = 0;
    for (size_t i = from; i < to; i++) {
        const int s = s_pcm[i] > 0 ? 1 : (s_pcm[i] < 0 ? -1 : 0);
        if (s != 0 && sign != 0 && s != sign) {
            crossings++;
        }
        if (s != 0) {
            sign = s;
        }
    }
    return crossings * s_rate / (2u * (uint32_t)(to - from));
}

/* ---- AUD-002: the life-safety patterns --------------------------------------------------- */

/* Temporal-3 (NFPA 72). Getting this wrong means a fire alarm a person does not recognise. */
static void test_fire_is_temporal_3(void)
{
    snd_set_continuous(&M, ALARM_SOUND_FIRE);
    render_ms(10u); /* the silent block that wakes the amplifier */
    listen_again();
    render_ms(4500u); /* one full 4 s round and the start of the next */

    segments_t s;
    segments(&s);
    assert_segment(&s, 0, true, 500u, "fire");
    assert_segment(&s, 1, false, 500u, "fire");
    assert_segment(&s, 2, true, 500u, "fire");
    assert_segment(&s, 3, false, 500u, "fire");
    assert_segment(&s, 4, true, 500u, "fire");
    assert_segment(&s, 5, false, 1500u, "fire"); /* the long gap that makes it temporal-3 */
    assert_segment(&s, 6, true, 500u, "fire");   /* and round again */
    TEST_ASSERT_UINT32_WITHIN(155u, 3100u, tone_hz(100u, 400u));
}

/* Temporal-4 (NFPA 720): four short bursts and a five-second gap, not to be confused with
 * the fire pattern by anybody woken up by it. */
static void test_co_is_temporal_4(void)
{
    snd_set_continuous(&M, ALARM_SOUND_CO);
    render_ms(10u);
    listen_again();
    render_ms(6000u);

    segments_t s;
    segments(&s);
    for (size_t i = 0; i < 7u; i++) {
        assert_segment(&s, i, (i % 2u) == 0u, 100u, "CO");
    }
    assert_segment(&s, 7, false, 5100u, "CO");
    assert_segment(&s, 8, true, 100u, "CO");
    TEST_ASSERT_UINT32_WITHIN(155u, 3100u, tone_hz(20u, 80u));
}

/* ---- AUD-001: the continuous pattern is whatever the partition status says ---------------- */

static void test_every_continuous_sound_plays(void)
{
    static const alarm_sound_t SOUNDS[] = {
        ALARM_SOUND_TROUBLE, ALARM_SOUND_EXIT, ALARM_SOUND_EXIT_FAST,
        ALARM_SOUND_ENTRY,   ALARM_SOUND_ENTRY_MEMORY, ALARM_SOUND_AUX,
        ALARM_SOUND_ALARM,   ALARM_SOUND_CO, ALARM_SOUND_FIRE,
    };
    for (size_t i = 0; i < sizeof(SOUNDS) / sizeof(SOUNDS[0]); i++) {
        setUp();
        snd_set_continuous(&M, SOUNDS[i]);
        render_ms(600u); /* longer than the longest first beep */
        TEST_ASSERT_TRUE_MESSAGE(peak() > 1000, alarm_sound_name(SOUNDS[i]));
        TEST_ASSERT_TRUE(snd_busy(&M));
    }
}

static void test_no_sound_is_silence(void)
{
    snd_set_continuous(&M, ALARM_SOUND_NONE);
    render_ms(1000u);
    TEST_ASSERT_EQUAL_INT16(0, peak());
    TEST_ASSERT_FALSE(snd_busy(&M));
}

/* Exit becoming entry has to be heard at once: the new pattern starts from its beginning
 * rather than dropping into the old one's gap. */
static void test_a_new_pattern_starts_from_its_beginning(void)
{
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(500u); /* 80 ms beep, then well into the 920 ms gap */
    listen_again();
    snd_set_continuous(&M, ALARM_SOUND_ENTRY);
    render_ms(300u);

    segments_t s;
    segments(&s);
    assert_segment(&s, 0, true, 250u, "entry"); /* heard immediately, not 420 ms later */
}

/* The same status arriving again (every snapshot repeats it) must not restart the pattern,
 * which would stutter the beep for as long as the status keeps being republished. */
static void test_the_same_pattern_is_not_restarted(void)
{
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(500u);
    listen_again();
    for (int i = 0; i < 10; i++) {
        snd_set_continuous(&M, ALARM_SOUND_EXIT); /* as a fresh snapshot would */
        render_ms(30u);
    }
    TEST_ASSERT_EQUAL_INT16(0, peak()); /* still inside the 920 ms gap */
}

/* ---- AUD-003: alarms ignore the volume --------------------------------------------------- */

static void test_alarms_play_at_full_volume(void)
{
    static const alarm_sound_t LOUD[] = {ALARM_SOUND_ALARM, ALARM_SOUND_FIRE, ALARM_SOUND_CO,
                                         ALARM_SOUND_AUX};
    for (size_t i = 0; i < sizeof(LOUD) / sizeof(LOUD[0]); i++) {
        setUp();
        snd_set_volume(&M, 0u); /* the owner turned the keypad right down */
        snd_set_continuous(&M, LOUD[i]);
        render_ms(600u);
        TEST_ASSERT_TRUE_MESSAGE(peak() > 25000, alarm_sound_name(LOUD[i]));
    }
}

static void test_the_volume_moves_everything_else(void)
{
    snd_set_volume(&M, 100u);
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(200u);
    const int16_t full = peak();

    setUp();
    snd_set_volume(&M, 50u);
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(200u);
    const int16_t half = peak();

    setUp();
    snd_set_volume(&M, 0u);
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(200u);

    TEST_ASSERT_TRUE(full > 25000);
    TEST_ASSERT_INT16_WITHIN(full / 10, full / 2, half);
    TEST_ASSERT_EQUAL_INT16(0, peak()); /* the trouble beeps really do go quiet */
}

static void test_the_volume_is_clamped(void)
{
    snd_set_volume(&M, 240u);
    TEST_ASSERT_EQUAL_UINT8(100u, M.volume);
}

/* ---- AUD-005: one-shots over patterns ---------------------------------------------------- */

/* An acknowledgement beep on top of the exit beeps: the exit pattern waits where it stood
 * rather than restarting, so the beep after it still lands on the second. */
static void test_a_one_shot_pauses_the_pattern_and_it_resumes(void)
{
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(300u);
    const uint32_t stood_at = M.cont.pos;
    const uint8_t step = M.cont.step;

    snd_play(&M, ALARM_SOUND_ACK); /* 150 ms */
    render_ms(100u);
    TEST_ASSERT_EQUAL_UINT32(stood_at, M.cont.pos); /* the exit pattern is holding */
    TEST_ASSERT_EQUAL_UINT8(step, M.cont.step);

    render_ms(100u); /* the ack finishes */
    TEST_ASSERT_TRUE(M.cont.pos > stood_at); /* and the exit pattern carries on */
    TEST_ASSERT_EQUAL_UINT8(step, M.cont.step);
}

/* Nothing cosmetic may be heard over a burglary, fire or CO alarm. */
static void test_alarms_drown_one_shots(void)
{
    snd_set_continuous(&M, ALARM_SOUND_ALARM);
    render_ms(20u);
    snd_play(&M, ALARM_SOUND_ACK);
    snd_key_click(&M);
    snd_play(&M, ALARM_SOUND_CHIME);
    render_ms(20u);
    TEST_ASSERT_EQUAL_UINT8(0u, M.pending_n);
    TEST_ASSERT_NULL(M.shot.pat);

    /* And the alarm itself is unbroken: two tones, 220 ms each, no gap between them. */
    listen_again();
    render_ms(600u);
    segments_t s;
    segments(&s);
    TEST_ASSERT_EQUAL_size_t(1u, s.count);
    TEST_ASSERT_TRUE(s.on[0]);
}

static void test_a_one_shot_is_heard_over_a_quiet_pattern(void)
{
    snd_set_continuous(&M, ALARM_SOUND_TROUBLE);
    render_ms(400u); /* the two chirps are over; 9.7 s of gap to come */
    listen_again();
    snd_play(&M, ALARM_SOUND_ERROR); /* 800 ms */
    render_ms(1000u);

    segments_t s;
    segments(&s);
    assert_segment(&s, 0, true, 800u, "error");
    TEST_ASSERT_UINT32_WITHIN(22u, 440u, tone_hz(100u, 700u));
}

static void test_one_shots_queue_and_then_drop(void)
{
    snd_play(&M, ALARM_SOUND_ACK);
    snd_play(&M, ALARM_SOUND_ACK);
    snd_play(&M, ALARM_SOUND_ACK);
    snd_play(&M, ALARM_SOUND_ACK);
    snd_play(&M, ALARM_SOUND_ACK); /* the fifth has nowhere to go */
    TEST_ASSERT_EQUAL_UINT8(SND_ONESHOT_QUEUE, M.pending_n);
}

/* A finger sliding over the keypad must not queue a second of clicking. */
static void test_key_clicks_stop_at_two_waiting(void)
{
    for (int i = 0; i < 10; i++) {
        snd_key_click(&M);
    }
    TEST_ASSERT_EQUAL_UINT8(2u, M.pending_n);
}

/* Two sounds asked for at once are heard one after the other, not mixed into a chord. */
static void test_one_shots_play_in_turn(void)
{
    snd_play(&M, ALARM_SOUND_ACK);       /* 150 ms */
    snd_play(&M, ALARM_SOUND_WALK_TEST); /* 330 ms */
    render_ms(100u);
    TEST_ASSERT_EQUAL_UINT8(1u, M.pending_n); /* the walk test is still waiting its turn */
    render_ms(100u);
    TEST_ASSERT_EQUAL_UINT8(0u, M.pending_n); /* the ack is over and it has started */
    TEST_ASSERT_NOT_NULL(M.shot.pat);
    render_ms(400u);
    TEST_ASSERT_FALSE(snd_busy(&M));
}

/* The walk test's three beeps: one per zone tripped, countable by whoever is walking. */
static void test_the_walk_test_is_three_beeps(void)
{
    snd_play(&M, ALARM_SOUND_WALK_TEST);
    render_ms(10u);
    listen_again();
    render_ms(400u);

    segments_t s;
    segments(&s);
    assert_segment(&s, 0, true, 70u, "walk test");
    assert_segment(&s, 1, false, 60u, "walk test");
    assert_segment(&s, 2, true, 70u, "walk test");
    assert_segment(&s, 3, false, 60u, "walk test");
    assert_segment(&s, 4, true, 70u, "walk test");
    assert_segment(&s, 5, false, 70u, "walk test"); /* and quiet: three beeps, not four */
    TEST_ASSERT_EQUAL_size_t(6u, s.count);
}

/* WALK-004: the walk test's end warning is a double beep, two 200 ms beeps 120 ms apart, once. */
static void test_the_walk_test_warning_is_a_double_beep(void)
{
    snd_play(&M, ALARM_SOUND_WALK_TEST_WARNING);
    render_ms(10u);
    listen_again();
    render_ms(800u);

    segments_t s;
    segments(&s);
    assert_segment(&s, 0, true, 200u, "walk test warning");
    assert_segment(&s, 1, false, 120u, "walk test warning");
    assert_segment(&s, 2, true, 200u, "walk test warning");
    TEST_ASSERT_EQUAL_size_t(4u, s.count); /* and quiet after: two beeps, not three */
    TEST_ASSERT_UINT32_WITHIN(74u, 1480u, tone_hz(20u, 180u));
    TEST_ASSERT_FALSE(snd_busy(&M));
}

/* The chime is a ding-dong, not a beep: two notes, the second lower and longer. */
static void test_the_chime_is_two_notes(void)
{
    snd_play(&M, ALARM_SOUND_CHIME);
    render_ms(10u);
    listen_again();
    render_ms(1200u);
    TEST_ASSERT_UINT32_WITHIN(66u, 1319u, tone_hz(20u, 200u)); /* E6 */
    TEST_ASSERT_UINT32_WITHIN(52u, 1047u, tone_hz(320u, 800u)); /* C6 */
    TEST_ASSERT_FALSE(snd_busy(&M));
}

/* ---- AUD-005, docs/features/41 D22: the arming warning's chime gives way to everything ------ */

/* It starts only over silence: never over a pattern, a one-shot or one that waits. */
static void test_a_background_chime_plays_only_over_silence(void)
{
    static const alarm_sound_t pats[] = {ALARM_SOUND_ALARM, ALARM_SOUND_FIRE, ALARM_SOUND_CO,
                                         ALARM_SOUND_ENTRY, ALARM_SOUND_EXIT, ALARM_SOUND_TROUBLE};
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        snd_init(&M);
        snd_set_continuous(&M, pats[i]);
        render_ms(20u);
        TEST_ASSERT_FALSE_MESSAGE(snd_play_background(&M, ALARM_SOUND_CHIME),
                                  alarm_sound_name(pats[i]));
        TEST_ASSERT_EQUAL_UINT8(0u, M.pending_n);
    }
    snd_init(&M);
    snd_play(&M, ALARM_SOUND_ACK); /* waiting, not playing yet */
    TEST_ASSERT_FALSE(snd_play_background(&M, ALARM_SOUND_CHIME));
    render_ms(20u); /* playing */
    TEST_ASSERT_FALSE(snd_play_background(&M, ALARM_SOUND_CHIME));
    render_ms(300u);
    TEST_ASSERT_FALSE(snd_busy(&M));
    TEST_ASSERT_TRUE(snd_play_background(&M, ALARM_SOUND_CHIME)); /* silence: it plays */
    render_ms(10u);
    listen_again();
    render_ms(1200u);
    TEST_ASSERT_UINT32_WITHIN(66u, 1319u, tone_hz(20u, 200u)); /* the chime's E6, C6 */
    TEST_ASSERT_UINT32_WITHIN(52u, 1047u, tone_hz(320u, 800u));
    TEST_ASSERT_FALSE(snd_busy(&M));
}

/* A pattern that starts while it plays starts in that block, from its beginning, as if the
 * chime had never been: the chime never interrupts or delays it. */
static void test_a_background_chime_gives_way_to_every_pattern(void)
{
    static const alarm_sound_t pats[] = {ALARM_SOUND_ALARM, ALARM_SOUND_FIRE, ALARM_SOUND_CO,
                                         ALARM_SOUND_ENTRY, ALARM_SOUND_EXIT, ALARM_SOUND_TROUBLE};
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        /* the pattern alone, as heard from silence */
        snd_init(&M);
        TEST_ASSERT_TRUE(snd_set_rate(&M, s_rate));
        render_ms(20u);
        snd_set_continuous(&M, pats[i]);
        listen_again();
        render_ms(1500u);
        segments_t alone;
        segments(&alone);
        /* the same pattern 200 ms into a background chime */
        snd_init(&M);
        TEST_ASSERT_TRUE(snd_set_rate(&M, s_rate));
        render_ms(20u);
        TEST_ASSERT_TRUE(snd_play_background(&M, ALARM_SOUND_CHIME));
        render_ms(200u);
        TEST_ASSERT_NOT_NULL(M.shot.pat);
        snd_set_continuous(&M, pats[i]);
        TEST_ASSERT_NULL_MESSAGE(M.shot.pat, alarm_sound_name(pats[i])); /* cut in that call */
        listen_again();
        render_ms(1500u);
        segments_t after;
        segments(&after);
        /* From silence the first block is silent while the amplifier wakes (AUD-004); over the
         * chime it is already on, so the pattern comes that block sooner, never later. The last
         * segment is cut by the end of the listening and is left out. */
        const size_t lead = (alone.count > 0u && !alone.on[0]) ? 1u : 0u;
        TEST_ASSERT_TRUE_MESSAGE(lead == 0u || alone.ms[0] <= 10u + EDGE_MS,
                                 alarm_sound_name(pats[i]));
        TEST_ASSERT_TRUE_MESSAGE(after.count > 0u && after.on[0], alarm_sound_name(pats[i]));
        TEST_ASSERT_EQUAL_size_t_MESSAGE(alone.count - lead, after.count, alarm_sound_name(pats[i]));
        for (size_t k = 0; k + 1u < after.count; k++) {
            assert_segment(&after, k, alone.on[k + lead], alone.ms[k + lead],
                           alarm_sound_name(pats[i]));
        }
    }
}

/* A code's acknowledgement or error, and a key click, are not held behind it either; and an
 * ordinary chime (a zone's door chime) still queues and plays as before. */
static void test_a_background_chime_gives_way_to_one_shots(void)
{
    TEST_ASSERT_TRUE(snd_play_background(&M, ALARM_SOUND_CHIME));
    render_ms(200u);
    snd_play(&M, ALARM_SOUND_ERROR); /* 800 ms of 440 Hz */
    TEST_ASSERT_NULL(M.shot.pat);
    listen_again();
    render_ms(1000u);
    segments_t s;
    segments(&s);
    assert_segment(&s, 0, true, 800u, "error after the background chime");
    TEST_ASSERT_UINT32_WITHIN(22u, 440u, tone_hz(100u, 700u));

    snd_init(&M);
    TEST_ASSERT_TRUE(snd_set_rate(&M, s_rate));
    TEST_ASSERT_TRUE(snd_play_background(&M, ALARM_SOUND_CHIME));
    render_ms(100u);
    snd_key_click(&M);
    TEST_ASSERT_NULL(M.shot.pat);
    TEST_ASSERT_EQUAL_UINT8(1u, M.pending_n);

    snd_init(&M); /* the engine's own chime is unchanged: queued, played whole */
    snd_play(&M, ALARM_SOUND_CHIME);
    TEST_ASSERT_EQUAL_UINT8(1u, M.pending_n);
    TEST_ASSERT_FALSE(M.background);
}

/* Cosmetic: power saving drops it; and the end of a pattern (NONE) doesn't cut it. */
static void test_a_background_chime_is_cosmetic(void)
{
    snd_set_power_saving(&M, true);
    TEST_ASSERT_FALSE(snd_play_background(&M, ALARM_SOUND_CHIME));
    snd_set_power_saving(&M, false);
    TEST_ASSERT_FALSE(snd_play_background(&M, ALARM_SOUND_NONE));
    TEST_ASSERT_FALSE(snd_play_background(&M, ALARM_SOUND_EXIT)); /* a pattern, not a one-shot */
    TEST_ASSERT_TRUE(snd_play_background(&M, ALARM_SOUND_CHIME));
    render_ms(100u);
    snd_set_continuous(&M, ALARM_SOUND_NONE);
    TEST_ASSERT_NOT_NULL(M.shot.pat);
    snd_set_power_saving(&M, true);
    render_ms(20u);
    TEST_ASSERT_NULL(M.shot.pat);
}

/* ---- AUD-004: no pop, and the amplifier rests -------------------------------------------- */

static void test_the_first_block_is_silent(void)
{
    TEST_ASSERT_TRUE(snd_idle(&M));
    snd_play(&M, ALARM_SOUND_ACK);

    const snd_block_t first = render_block();
    TEST_ASSERT_TRUE_MESSAGE(first.amp, "the amplifier comes on for the silent block");
    TEST_ASSERT_TRUE_MESSAGE(first.silent, "and nothing is played into it yet");
    TEST_ASSERT_EQUAL_INT16(0, peak());

    const snd_block_t second = render_block();
    TEST_ASSERT_TRUE(second.amp);
    TEST_ASSERT_FALSE(second.silent); /* only now does the beep start */
}

static void test_the_amplifier_rests_after_the_sound(void)
{
    snd_play(&M, ALARM_SOUND_ACK);
    render_ms(200u); /* the 150 ms beep, then 40 ms of quiet */
    TEST_ASSERT_FALSE(snd_busy(&M));
    TEST_ASSERT_TRUE_MESSAGE(s_amp, "still on: a second beep may be coming");

    render_ms(SND_AMP_IDLE_MS - 50u); /* 1490 ms of quiet: on the boundary, not yet */
    TEST_ASSERT_TRUE(s_amp);
    render_ms(20u);
    TEST_ASSERT_FALSE(s_amp);
    TEST_ASSERT_TRUE(snd_idle(&M)); /* the caller may now sleep until something happens */
}

/* A pattern that never stops keeps the amplifier on however long it is silent between beeps
 * (the trouble chirps leave 9.7 s of gap - the amplifier must not click off in it). */
static void test_a_waiting_pattern_keeps_the_amplifier_on(void)
{
    snd_set_continuous(&M, ALARM_SOUND_TROUBLE);
    render_ms(5000u);
    TEST_ASSERT_TRUE(s_amp);
    TEST_ASSERT_FALSE(snd_idle(&M));
}

/* ---- power saving (docs/features/17) ----------------------------------------------------------- */

static void test_power_saving_drops_the_cosmetic_sounds(void)
{
    snd_set_power_saving(&M, true);
    snd_key_click(&M);
    snd_play(&M, ALARM_SOUND_CHIME);
    TEST_ASSERT_EQUAL_UINT8(0u, M.pending_n);

    snd_play(&M, ALARM_SOUND_ACK); /* a code was accepted: still worth hearing */
    snd_play(&M, ALARM_SOUND_ERROR);
    TEST_ASSERT_EQUAL_UINT8(2u, M.pending_n);
    render_ms(1200u);
    TEST_ASSERT_TRUE(peak() > 1000);
}

static void test_power_saving_silences_a_chime_already_playing(void)
{
    snd_play(&M, ALARM_SOUND_CHIME);
    render_ms(100u);
    TEST_ASSERT_NOT_NULL(M.shot.pat);
    snd_set_power_saving(&M, true);
    listen_again();
    render_ms(500u);
    TEST_ASSERT_EQUAL_INT16(0, peak());
    TEST_ASSERT_FALSE(snd_busy(&M));
}

static void test_power_saving_never_touches_an_alarm(void)
{
    snd_set_power_saving(&M, true);
    snd_set_continuous(&M, ALARM_SOUND_ALARM);
    render_ms(500u);
    TEST_ASSERT_TRUE(peak() > 25000);
}

/* ---- AUD-006: the board's rate ------------------------------------------------------------ */

/* The rates a board may pick below the default: a DAC played from a timer, a small I2S amp. */
static const uint32_t RATES[] = {8000u, 12000u, 16000u, 24000u};
#define RATE_COUNT (sizeof(RATES) / sizeof(RATES[0]))

/* Fire and CO keep their standard timings and their pitch at every rate: the same patterns,
 * listened to the same way, whatever the board plays at. */
static void test_life_safety_at_every_rate(void)
{
    for (size_t i = 0; i < RATE_COUNT; i++) {
        s_rate = RATES[i];
        setUp();
        TEST_ASSERT_EQUAL_UINT32(RATES[i], snd_rate(&M));
        test_fire_is_temporal_3();
        setUp();
        test_co_is_temporal_4();
    }
    s_rate = SND_SAMPLE_RATE;
}

/* The everyday sounds too: the entry beeps (1568 Hz, 250 ms on and off) at every rate. */
static void test_the_entry_beeps_at_every_rate(void)
{
    for (size_t i = 0; i < RATE_COUNT; i++) {
        s_rate = RATES[i];
        setUp();
        snd_set_continuous(&M, ALARM_SOUND_ENTRY);
        render_ms(10u);
        listen_again();
        render_ms(1100u);
        segments_t seg;
        segments(&seg);
        assert_segment(&seg, 0, true, 250u, "entry");
        assert_segment(&seg, 1, false, 250u, "entry");
        assert_segment(&seg, 2, true, 250u, "entry");
        TEST_ASSERT_UINT32_WITHIN(79u, 1568u, tone_hz(50u, 200u));
    }
    s_rate = SND_SAMPLE_RATE;
}

/* A 10 ms block is rate / 100 frames, and the amplifier rests after the same 1.5 s of quiet
 * at any rate (it counts time, not samples). */
static void test_blocks_and_the_amplifier_follow_the_rate(void)
{
    s_rate = 8000u;
    setUp();
    TEST_ASSERT_EQUAL_size_t(80u, snd_block_frames(&M));
    snd_play(&M, ALARM_SOUND_ACK);
    render_ms(200u);
    TEST_ASSERT_TRUE(s_amp);
    render_ms(SND_AMP_IDLE_MS - 50u);
    TEST_ASSERT_TRUE(s_amp);
    render_ms(20u);
    TEST_ASSERT_FALSE(s_amp);
    s_rate = SND_SAMPLE_RATE;
    setUp();
    TEST_ASSERT_EQUAL_size_t(SND_BLOCK_FRAMES, snd_block_frames(&M));
}

/* Rates outside 8-32 kHz, and rates whose 10 ms block is not a whole number of frames
 * (11025, 22050: the blocks and the amplifier's timer would drift), are refused and change
 * nothing. */
static void test_rates_outside_the_range_are_refused(void)
{
    TEST_ASSERT_FALSE(snd_set_rate(&M, 11025u));
    TEST_ASSERT_FALSE(snd_set_rate(&M, 22050u));
    TEST_ASSERT_FALSE(snd_set_rate(&M, 0u));
    TEST_ASSERT_FALSE(snd_set_rate(&M, SND_RATE_MIN - 1u));
    TEST_ASSERT_FALSE(snd_set_rate(&M, SND_SAMPLE_RATE + 1u));
    TEST_ASSERT_FALSE(snd_set_rate(NULL, 16000u));
    TEST_ASSERT_EQUAL_UINT32(SND_SAMPLE_RATE, snd_rate(&M));
    TEST_ASSERT_EQUAL_UINT32(SND_SAMPLE_RATE, snd_rate(NULL));
    TEST_ASSERT_TRUE(snd_set_rate(&M, SND_RATE_MIN));
    TEST_ASSERT_TRUE(snd_set_rate(&M, SND_SAMPLE_RATE));
}

/* A rate change while a pattern plays starts it again from its beginning at the new rate,
 * rather than finishing a step measured in the old rate's samples. */
static void test_a_rate_change_restarts_the_pattern(void)
{
    snd_set_continuous(&M, ALARM_SOUND_EXIT);
    render_ms(500u); /* into the 920 ms gap */
    s_rate = 16000u;
    TEST_ASSERT_TRUE(snd_set_rate(&M, s_rate));
    listen_again();
    render_ms(300u);
    segments_t seg;
    segments(&seg);
    assert_segment(&seg, 0, true, 80u, "exit after a rate change");
    s_rate = SND_SAMPLE_RATE;
}

/* ---- AUD-010, AUD-028: speech in the mix (voice-first-build.md D11) ------------------------ */

static snd_mixer_t A; /* tones only: the reference */
static snd_mixer_t B; /* the same tones, with speech offered */
static int16_t s_block_b[SND_BLOCK_FRAMES * 2u];
static int16_t s_speech[SND_BLOCK_FRAMES];

/* A deterministic test signal of about +-8000, different in every sample. */
static void fill_speech(size_t from, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const int32_t k = (int32_t)(((from + i) * 37u) % 2001u) - 1000;
        s_speech[i] = (int16_t)(k * 8);
    }
}

static void fill_speech_const(int16_t v)
{
    for (size_t i = 0; i < SND_BLOCK_FRAMES; i++) {
        s_speech[i] = v;
    }
}

/* Speech joins the tones at unity: its Voice volume came before the mixer (speech_boost, AUD-033). */
static int32_t expect_mix(int16_t tone, int16_t speech, bool entry_exit)
{
    int32_t g = 32768;
    if (entry_exit) {
        g /= 2;
    }
    int32_t v = tone + (int32_t)speech * g / 32768;
    return v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v);
}

/* Render one block on A (tones) and B (tones + the speech now in s_speech) and check that B is
 * A's tone plus the speech, sample for sample. Returns B's report. */
static snd_pcm_block_t render_pair(size_t avail, bool entry_exit)
{
    const size_t frames = snd_block_frames(&A);
    const snd_block_t a = snd_render(&A, s_block, frames);
    const snd_pcm_block_t b = snd_render_pcm(&B, s_block_b, frames, s_speech, avail, true);
    TEST_ASSERT_EQUAL(a.amp, b.block.amp);
    for (size_t i = 0; i < frames; i++) {
        const int32_t want = i < b.speech_consumed
                                 ? expect_mix(s_block[2u * i], s_speech[i], entry_exit)
                                 : s_block[2u * i];
        TEST_ASSERT_EQUAL_INT32(want, s_block_b[2u * i]);
        TEST_ASSERT_EQUAL_INT16(s_block_b[2u * i], s_block_b[2u * i + 1u]);
    }
    return b;
}

static void pair_init(void)
{
    snd_init(&A);
    snd_init(&B);
}

/* A scripted mix of patterns and one-shots, the same on any mixer given the same block number. */
static void script(snd_mixer_t *m, uint32_t blk)
{
    static const alarm_sound_t cont[] = {ALARM_SOUND_TROUBLE, ALARM_SOUND_EXIT,
                                         ALARM_SOUND_EXIT_FAST, ALARM_SOUND_ENTRY,
                                         ALARM_SOUND_ENTRY_MEMORY, ALARM_SOUND_AUX,
                                         ALARM_SOUND_ALARM, ALARM_SOUND_CO, ALARM_SOUND_FIRE,
                                         ALARM_SOUND_NONE};
    if (blk % 97u == 0u) {
        snd_set_continuous(m, cont[(blk / 97u) % 10u]);
    }
    if (blk % 41u == 5u) {
        snd_play(m, ALARM_SOUND_ACK);
    }
    if (blk % 59u == 7u) {
        snd_key_click(m);
        snd_play(m, ALARM_SOUND_CHIME);
    }
    if (blk % 211u == 13u) {
        snd_set_volume(m, (uint8_t)(blk % 101u));
    }
    if (blk % 223u == 17u) { /* AUD-032: the chime's own volume, on both paths alike */
        snd_set_chime_volume(m, (uint8_t)(blk % 101u));
        snd_set_voice_volume(m, (uint8_t)(blk % 89u));
    }
}

/* AUD-010, AUD-028: with no speech, snd_render_pcm() is snd_render() bit for bit - the tone path
 * is the one the panel always had (a one-time check against the old code gave the same hash). */
static void test_no_speech_renders_exactly_the_tones(void)
{
    static const uint32_t rates[] = {8000u, 16000u, 32000u};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        pair_init();
        TEST_ASSERT_TRUE(snd_set_rate(&A, rates[r]));
        TEST_ASSERT_TRUE(snd_set_rate(&B, rates[r]));
        const size_t frames = snd_block_frames(&A);
        for (uint32_t blk = 0; blk < 1200u; blk++) {
            script(&A, blk);
            script(&B, blk);
            const snd_block_t a = snd_render(&A, s_block, frames);
            /* a buffer offered with no samples and no utterance is no speech either */
            const snd_pcm_block_t b =
                snd_render_pcm(&B, s_block_b, frames, (blk & 1u) ? s_speech : NULL, 0u, false);
            TEST_ASSERT_EQUAL(a.amp, b.block.amp);
            TEST_ASSERT_EQUAL(a.silent, b.block.silent);
            TEST_ASSERT_EQUAL_size_t(0u, b.speech_consumed);
            TEST_ASSERT_FALSE(b.speech_rejected);
            TEST_ASSERT_EQUAL_INT16_ARRAY(s_block, s_block_b, frames * 2u);
        }
    }
}

/* AUD-010, AUD-034: speech is added on top of quiet patterns and one-shots; the tones keep their
 * gain, phase and timing, during the speech and after it (the -6 dB under the entry beeps and the
 * exit delay's fast ones). The exit beeps before the last 10 s wait instead (below). */
static void test_speech_rides_on_top_of_the_tones(void)
{
    static const alarm_sound_t quiet[] = {ALARM_SOUND_TROUBLE, ALARM_SOUND_EXIT_FAST,
                                          ALARM_SOUND_ENTRY, ALARM_SOUND_ENTRY_MEMORY};
    for (size_t p = 0; p < sizeof(quiet) / sizeof(quiet[0]); p++) {
        pair_init();
        snd_set_volume(&A, 80u);
        snd_set_volume(&B, 80u);
        snd_set_voice_volume(&B, 60u); /* AUD-032: its own volume, not the tones' */
        snd_set_continuous(&A, quiet[p]);
        snd_set_continuous(&B, quiet[p]);
        const bool entry_exit = quiet[p] != ALARM_SOUND_TROUBLE;
        size_t said = 0;
        for (uint32_t blk = 0; blk < 300u; blk++) {
            if (blk == 50u) {
                snd_play(&A, ALARM_SOUND_CHIME); /* a one-shot pausing the pattern, spoken over */
                snd_play(&B, ALARM_SOUND_CHIME);
            }
            if (blk < 200u) {
                fill_speech(said, SND_BLOCK_FRAMES);
                const snd_pcm_block_t b = render_pair(SND_BLOCK_FRAMES, entry_exit);
                TEST_ASSERT_EQUAL_size_t(blk == 0u ? 0u : SND_BLOCK_FRAMES, b.speech_consumed);
                said += b.speech_consumed;
            } else {
                /* speech over: B is A again, sample for sample (no phase or timing moved) */
                const size_t frames = snd_block_frames(&A);
                (void)snd_render(&A, s_block, frames);
                (void)snd_render(&B, s_block_b, frames);
                TEST_ASSERT_EQUAL_INT16_ARRAY(s_block, s_block_b, frames * 2u);
            }
        }
    }
}

/* AUD-028, AUD-033, AUD-034: speech joins at unity, whatever the voice volume (speech_boost
 * applied it before the mixer); the entry beeps and the exit delay's fast ones take it 6 dB lower,
 * the exit beeps that wait for it don't. */
static void test_speech_joins_at_unity(void)
{
    pair_init();
    snd_set_voice_volume(&B, 50u);
    fill_speech_const(10000);
    (void)snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    snd_pcm_block_t b =
        snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    TEST_ASSERT_EQUAL_size_t(SND_BLOCK_FRAMES, b.speech_consumed);
    TEST_ASSERT_EQUAL_INT16(10000, s_block_b[0]);
    TEST_ASSERT_EQUAL_INT16(10000, s_block_b[2u * (SND_BLOCK_FRAMES - 1u) + 1u]);

    snd_set_continuous(&B, ALARM_SOUND_EXIT_FAST);
    for (uint32_t ms = 0; ms < 200u; ms += 10u) { /* into the fast pattern's 170 ms gap */
        b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    }
    TEST_ASSERT_EQUAL_INT16(5000, s_block_b[0]);

    snd_set_continuous(&B, ALARM_SOUND_EXIT); /* its beeps wait: the words at unity */
    b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    TEST_ASSERT_EQUAL_INT16(10000, s_block_b[0]);
    TEST_ASSERT_EQUAL_INT16(10000, s_block_b[2u * (SND_BLOCK_FRAMES - 1u)]);
    snd_set_continuous(&B, ALARM_SOUND_EXIT_FAST);

    snd_set_voice_volume(&B, 0u); /* the stage silences speech at 0 %; the mixer doesn't again */
    b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    TEST_ASSERT_EQUAL_size_t(SND_BLOCK_FRAMES, b.speech_consumed);
    TEST_ASSERT_EQUAL_INT16(5000, s_block_b[0]);
}

/* One 10 ms block of B with 'avail' samples of speech offered and an utterance open or not; true
 * when any of its samples is not zero. */
static bool block_with(size_t avail, bool active)
{
    (void)snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, avail, active);
    for (size_t i = 0; i < 2u * SND_BLOCK_FRAMES; i++) {
        if (s_block_b[i] != 0) {
            return true;
        }
    }
    return false;
}

/* One 10 ms block of B, speaking (a full block of speech) or not. */
static bool block_sounds(bool speaking)
{
    return block_with(speaking ? SND_BLOCK_FRAMES : 0u, speaking);
}

/* AUD-034: while the panel speaks, the exit beeps wait. The beep already sounding finishes (B
 * stays A, sample for sample, to its end); the next one waits as long as the words go on, and
 * sounds as soon as they end, the beeps then keeping their 1 s rhythm. Silent speech, so only the
 * tones are heard. */
static void test_the_exit_beeps_wait_for_the_words(void)
{
    pair_init();
    snd_set_continuous(&A, ALARM_SOUND_EXIT);
    snd_set_continuous(&B, ALARM_SOUND_EXIT);
    fill_speech_const(0);
    const size_t beep = SND_SAMPLE_RATE * 80u / 1000u; /* S_EXIT: 80 ms on, 920 ms off */
    size_t at = 0;                                     /* samples since the pattern began */
    for (uint32_t blk = 0; blk < 151u; blk++) {
        const bool speaking = blk >= 5u; /* from 40 ms into the first beep to 1.5 s */
        const snd_block_t a = snd_render(&A, s_block, SND_BLOCK_FRAMES);
        (void)block_sounds(speaking);
        if (blk == 0u) {
            TEST_ASSERT_TRUE(a.silent); /* the amplifier's warm-up: the pattern begins after it */
            continue;
        }
        for (size_t i = 0; i < SND_BLOCK_FRAMES; i++, at++) {
            const int16_t want = at < beep ? s_block[2u * i] : 0;
            TEST_ASSERT_EQUAL_INT16(want, s_block_b[2u * i]);
        }
    }
    /* A beeped again at 1 s; B waited. The words end: B's beep comes at once, then every 1 s
     * from it, not on the old grid. */
    uint32_t onsets[4] = {0}, n = 0;
    bool was = false;
    for (uint32_t blk = 0; blk < 250u; blk++) {
        const bool now = block_sounds(false);
        if (now && !was && n < 4u) {
            onsets[n++] = blk;
        }
        was = now;
    }
    TEST_ASSERT_EQUAL_UINT32(3u, n);
    TEST_ASSERT_EQUAL_UINT32(0u, onsets[0]);
    TEST_ASSERT_EQUAL_UINT32(100u, onsets[1]);
    TEST_ASSERT_EQUAL_UINT32(200u, onsets[2]);
}

/* AUD-034, the arming flow in its order: the acknowledgement, the words, then
 * the exit beeps. The acknowledgement plays (the exit pattern paused under it, AUD-005) and the
 * arming words follow its PLAYED a block or a few later, once the voice worker has decoded them.
 * Speech is due from the arm (snd_set_speech_due), so the first exit beep waits for the words
 * too: nothing sounds between the acknowledgement and the words, nothing under them, and the
 * beeps start as the words end and nothing more is due. Silent speech: only the tones are heard. */
static void test_the_first_exit_beep_waits_for_the_arming_words(void)
{
    for (uint32_t late = 1u; late <= 4u; late++) { /* the words' first block after PLAYED's */
        pair_init();
        snd_set_continuous(&B, ALARM_SOUND_EXIT);
        TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_ACK, 9u));
        snd_set_speech_due(&B, true);
        fill_speech_const(0);
        bool played = false;
        for (uint32_t blk = 0; blk < 40u && !played; blk++) {
            (void)block_sounds(false);
            snd_ticket_t t;
            played = snd_ticket_next(&B, &t) && t.ticket == 9u && t.outcome == SND_TICKET_PLAYED;
        }
        TEST_ASSERT_TRUE(played);
        for (uint32_t blk = 1u; blk < late; blk++) {
            TEST_ASSERT_FALSE(block_sounds(false)); /* the words are being decoded: no beep */
        }
        for (uint32_t blk = 0; blk < 150u; blk++) {
            TEST_ASSERT_FALSE(block_sounds(true)); /* 1.5 s of words: no beep under them */
        }
        snd_set_speech_due(&B, false); /* the words have ended */
        TEST_ASSERT_TRUE(block_sounds(false)); /* the first exit beep, at once */
    }
}

/* AUD-010, AUD-034: speech due holds the slow exit beeps and nothing else. Every other pattern -
 * the entry beeps, the fast exit beeps, trouble, aux and the alarms - and the one-shots over them
 * render sample for sample as with nothing due, the loud ones refusing the speech offered; and an
 * alarm that takes over from exit beeps held for words sounds in its first block. */
static void test_speech_due_changes_no_other_sound(void)
{
    static const alarm_sound_t other[] = {ALARM_SOUND_NONE, ALARM_SOUND_TROUBLE,
                                          ALARM_SOUND_EXIT_FAST, ALARM_SOUND_ENTRY,
                                          ALARM_SOUND_ENTRY_MEMORY, ALARM_SOUND_AUX,
                                          ALARM_SOUND_ALARM, ALARM_SOUND_CO, ALARM_SOUND_FIRE};
    for (size_t p = 0; p < sizeof(other) / sizeof(other[0]); p++) {
        const bool loud = other[p] == ALARM_SOUND_AUX || other[p] == ALARM_SOUND_ALARM ||
                          other[p] == ALARM_SOUND_CO || other[p] == ALARM_SOUND_FIRE;
        pair_init();
        snd_set_continuous(&A, other[p]);
        snd_set_continuous(&B, other[p]);
        fill_speech(0u, SND_BLOCK_FRAMES);
        for (uint32_t blk = 0; blk < 400u; blk++) {
            if (blk == 50u || blk == 230u) {
                snd_play(&A, ALARM_SOUND_ACK);
                snd_play(&B, ALARM_SOUND_ACK);
            }
            if (blk == 120u) {
                snd_key_click(&A);
                snd_key_click(&B);
                snd_play(&A, ALARM_SOUND_ERROR);
                snd_play(&B, ALARM_SOUND_ERROR);
            }
            snd_set_speech_due(&B, true);
            const snd_block_t a = snd_render(&A, s_block, SND_BLOCK_FRAMES);
            /* nothing to say yet, or - over the loud patterns - words offered and refused */
            const snd_pcm_block_t b =
                snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, loud ? s_speech : NULL,
                               loud ? SND_BLOCK_FRAMES : 0u, loud);
            TEST_ASSERT_EQUAL(a.amp, b.block.amp);
            TEST_ASSERT_EQUAL(a.silent, b.block.silent);
            TEST_ASSERT_EQUAL(loud, b.speech_rejected);
            TEST_ASSERT_EQUAL_size_t(0u, b.speech_consumed);
            TEST_ASSERT_EQUAL_INT16_ARRAY(s_block, s_block_b, SND_BLOCK_FRAMES * 2u);
            TEST_ASSERT_EQUAL(snd_busy(&A), snd_busy(&B));
            TEST_ASSERT_EQUAL(snd_idle(&A), snd_idle(&B));
        }
    }

    static const alarm_sound_t alarms[] = {ALARM_SOUND_ALARM, ALARM_SOUND_FIRE, ALARM_SOUND_CO};
    for (size_t p = 0; p < sizeof(alarms) / sizeof(alarms[0]); p++) {
        pair_init();
        snd_set_continuous(&A, ALARM_SOUND_EXIT); /* the same exit delay, nothing due: beeping */
        snd_set_continuous(&B, ALARM_SOUND_EXIT);
        fill_speech_const(0);
        for (uint32_t blk = 0; blk < 150u; blk++) { /* due from the start: the first beep waits */
            snd_set_speech_due(&B, true);
            (void)snd_render(&A, s_block, SND_BLOCK_FRAMES);
            TEST_ASSERT_FALSE(block_sounds(false));
        }
        snd_set_continuous(&A, alarms[p]);
        snd_set_continuous(&B, alarms[p]); /* an alarm, the flag still set */
        for (uint32_t blk = 0; blk < 200u; blk++) {
            snd_set_speech_due(&B, true);
            (void)snd_render(&A, s_block, SND_BLOCK_FRAMES);
            (void)snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, NULL, 0u, false);
            TEST_ASSERT_EQUAL_INT16_ARRAY(s_block, s_block_b, SND_BLOCK_FRAMES * 2u);
            if (blk == 0u) { /* sounding in its first block */
                bool any = false;
                for (size_t i = 0; i < 2u * SND_BLOCK_FRAMES; i++) {
                    any = any || s_block_b[i] != 0;
                }
                TEST_ASSERT_TRUE(any);
            }
        }
    }
}

/* AUD-034: speech due that never comes (a stuck worker) holds each exit beep 10 s at most. */
static void test_speech_due_alone_holds_the_exit_beeps_10_s_at_most(void)
{
    pair_init();
    snd_set_continuous(&B, ALARM_SOUND_EXIT);
    snd_set_speech_due(&B, true);
    fill_speech_const(0);
    uint32_t first = 0;
    bool found = false;
    for (uint32_t blk = 0; blk < 1100u && !found; blk++) {
        if (block_sounds(false)) {
            first = blk;
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT32(1u + SND_EXIT_WAIT_MAX_MS / 10u, first); /* after the warm-up */
}

/* AUD-034: an utterance still filling - open, nothing buffered yet - holds the exit beeps too;
 * once it is gone the held beep sounds at once. */
static void test_the_exit_beeps_wait_for_words_still_filling(void)
{
    pair_init();
    snd_set_continuous(&B, ALARM_SOUND_EXIT);
    fill_speech_const(0);
    for (uint32_t blk = 0; blk < 95u; blk++) { /* the warm-up, the first beep, into the gap */
        (void)block_sounds(false);
    }
    for (uint32_t blk = 0; blk < 20u; blk++) { /* past the 1 s mark, no samples yet */
        TEST_ASSERT_FALSE(block_with(0u, true));
    }
    TEST_ASSERT_TRUE(block_with(0u, false));
}

/* AUD-034: a one-shot (an error beep, a key click) while the exit beeps wait sounds at once, over
 * the words, and after it the next exit beep still waits for their end. */
static void test_a_one_shot_sounds_while_the_exit_beeps_wait(void)
{
    pair_init();
    snd_set_continuous(&B, ALARM_SOUND_EXIT);
    fill_speech_const(0);
    for (uint32_t blk = 0; blk < 150u; blk++) { /* words from the start: every beep held */
        TEST_ASSERT_FALSE(block_sounds(true));
    }
    TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_ERROR, 0u));
    TEST_ASSERT_TRUE(block_sounds(true)); /* the one-shot, at once */
    for (uint32_t blk = 0; blk < 100u; blk++) { /* its 800 ms, and on */
        (void)block_sounds(true);
    }
    for (uint32_t blk = 0; blk < 50u; blk++) {
        TEST_ASSERT_FALSE(block_sounds(true)); /* the exit beep still waits */
    }
    TEST_ASSERT_TRUE(block_sounds(false)); /* the words end: the beep */
}

/* AUD-034: an exit delay ended while words held its beeps (a disarm); the next one waits its full
 * 10 s again - the count starts over. */
static void test_a_new_exit_delay_waits_its_full_10_s(void)
{
    pair_init();
    snd_set_continuous(&B, ALARM_SOUND_EXIT);
    fill_speech_const(0);
    for (uint32_t blk = 0; blk < 901u; blk++) { /* the warm-up, then 9 s of held beeps */
        TEST_ASSERT_FALSE(block_sounds(true));
    }
    snd_set_continuous(&B, ALARM_SOUND_NONE); /* disarmed */
    (void)block_sounds(true);
    snd_set_continuous(&B, ALARM_SOUND_EXIT); /* armed again, the words still going */
    for (uint32_t blk = 0; blk < 500u; blk++) { /* past what was left of the first 10 s */
        TEST_ASSERT_FALSE(block_sounds(true));
    }
}

/* AUD-034: words that go on and on hold the exit beeps 10 s at most (SND_EXIT_WAIT_MAX_MS): then
 * one beep sounds over them and the next waits again. When the exit delay's last 10 s begin, its
 * fast beeps sound at once, under the words. */
static void test_the_exit_beeps_wait_10_s_at_most(void)
{
    pair_init();
    snd_set_continuous(&B, ALARM_SOUND_EXIT);
    fill_speech_const(0);
    uint32_t onsets[3] = {0}, n = 0;
    bool was = false;
    for (uint32_t blk = 0; blk < 2500u; blk++) { /* 25 s of words, from the start of the delay */
        const bool now = block_sounds(true);
        if (now && !was && n < 3u) {
            onsets[n++] = blk;
        }
        was = now;
    }
    TEST_ASSERT_EQUAL_UINT32(2u, n);
    TEST_ASSERT_EQUAL_UINT32(1u + SND_EXIT_WAIT_MAX_MS / 10u, onsets[0]); /* after the warm-up */
    TEST_ASSERT_EQUAL_UINT32(onsets[0] + 100u + SND_EXIT_WAIT_MAX_MS / 10u, onsets[1]);
    snd_set_continuous(&B, ALARM_SOUND_EXIT_FAST);
    bool fast = false;
    for (uint32_t blk = 0; blk < 3u; blk++) {
        fast = block_sounds(true) || fast;
    }
    TEST_ASSERT_TRUE(fast);
}

/* AUD-010: full-scale speech over a full-volume beep saturates instead of wrapping, and only
 * the speech gives way: the beep's own samples survive wherever there is room. */
static void test_speech_is_clamped_to_the_headroom(void)
{
    static const int16_t loudest[] = {INT16_MAX, INT16_MIN};
    for (size_t k = 0; k < 2u; k++) {
        pair_init();
        snd_set_volume(&A, 100u);
        snd_set_volume(&B, 100u);
        snd_set_voice_volume(&B, 100u);
        snd_play(&A, ALARM_SOUND_ERROR);
        snd_play(&B, ALARM_SOUND_ERROR);
        fill_speech_const(loudest[k]);
        bool clipped = false;
        for (uint32_t blk = 0; blk < 40u; blk++) {
            (void)render_pair(SND_BLOCK_FRAMES, false); /* checks every sample against expect_mix */
            for (size_t i = 0; i < SND_BLOCK_FRAMES; i++) {
                clipped = clipped || s_block_b[2u * i] == loudest[k];
            }
        }
        TEST_ASSERT_TRUE(clipped);
    }
}

/* AUD-010: alarm, fire, CO and aux reject speech outright - nothing consumed, the alarm's
 * samples untouched - from the very block the pattern starts. */
static void test_loud_patterns_reject_speech(void)
{
    static const alarm_sound_t loud[] = {ALARM_SOUND_AUX, ALARM_SOUND_ALARM, ALARM_SOUND_CO,
                                         ALARM_SOUND_FIRE};
    for (size_t p = 0; p < sizeof(loud) / sizeof(loud[0]); p++) {
        pair_init();
        snd_set_continuous(&A, ALARM_SOUND_TROUBLE);
        snd_set_continuous(&B, ALARM_SOUND_TROUBLE);
        fill_speech(0u, SND_BLOCK_FRAMES);
        for (uint32_t blk = 0; blk < 5u; blk++) {
            (void)render_pair(SND_BLOCK_FRAMES, false);
        }
        snd_set_continuous(&A, loud[p]);
        snd_set_continuous(&B, loud[p]);
        for (uint32_t blk = 0; blk < 300u; blk++) {
            const snd_pcm_block_t b = render_pair(SND_BLOCK_FRAMES, false);
            TEST_ASSERT_EQUAL_size_t(0u, b.speech_consumed);
            TEST_ASSERT_TRUE(b.speech_rejected);
        }
        /* an utterance still open but out of samples is refused too */
        const snd_pcm_block_t b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, NULL, 0u, true);
        TEST_ASSERT_TRUE(b.speech_rejected);
        /* the alarm ends: speech is accepted again */
        snd_set_continuous(&B, ALARM_SOUND_NONE);
        const snd_pcm_block_t c =
            snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
        TEST_ASSERT_FALSE(c.speech_rejected);
        TEST_ASSERT_EQUAL_size_t(SND_BLOCK_FRAMES, c.speech_consumed);
    }
}

/* AUD-028, AUD-004: speech wakes the amplifier like a tone, and its silent warm-up block
 * consumes none of it. */
static void test_speech_waits_for_the_warm_up(void)
{
    pair_init();
    TEST_ASSERT_TRUE(snd_idle(&B));
    fill_speech_const(1000);
    snd_pcm_block_t b =
        snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    TEST_ASSERT_TRUE(b.block.amp);
    TEST_ASSERT_TRUE(b.block.silent);
    TEST_ASSERT_EQUAL_size_t(0u, b.speech_consumed);
    for (size_t i = 0; i < 2u * SND_BLOCK_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, s_block_b[i]);
    }
    b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
    TEST_ASSERT_TRUE(b.block.amp);
    TEST_ASSERT_FALSE(b.block.silent);
    TEST_ASSERT_EQUAL_size_t(SND_BLOCK_FRAMES, b.speech_consumed);
}

/* AUD-028: a short tail is consumed as far as it goes, and the rest of the block is tones. */
static void test_a_short_tail_is_consumed_exactly(void)
{
    pair_init();
    snd_set_continuous(&A, ALARM_SOUND_ENTRY);
    snd_set_continuous(&B, ALARM_SOUND_ENTRY);
    fill_speech(0u, SND_BLOCK_FRAMES);
    (void)render_pair(SND_BLOCK_FRAMES, true);
    const snd_pcm_block_t b = render_pair(123u, true);
    TEST_ASSERT_EQUAL_size_t(123u, b.speech_consumed);
    /* a smaller block than the speech offered: no more than the block */
    const snd_pcm_block_t c = snd_render_pcm(&B, s_block_b, 100u, s_speech, SND_BLOCK_FRAMES, true);
    TEST_ASSERT_EQUAL_size_t(100u, c.speech_consumed);
}

/* AUD-028: an utterance keeps the amplifier on and the mixer busy, even through silent samples
 * or a block with none buffered; once it ends the amplifier rests as after a tone. */
static void test_speech_keeps_the_amplifier_on(void)
{
    pair_init();
    fill_speech_const(0);
    for (uint32_t ms = 0; ms < 3000u; ms += 10u) {
        const snd_pcm_block_t b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech,
                                                 (ms / 10u) % 3u == 2u ? 0u : SND_BLOCK_FRAMES,
                                                 true);
        TEST_ASSERT_TRUE(b.block.amp);
        TEST_ASSERT_TRUE(snd_busy(&B));
        TEST_ASSERT_FALSE(snd_idle(&B));
    }
    TEST_ASSERT_FALSE_MESSAGE(snd_play_background(&B, ALARM_SOUND_CHIME),
                              "a background chime plays only over silence (AUD-005)");
    snd_pcm_block_t b = {0};
    for (uint32_t ms = 0; ms < SND_AMP_IDLE_MS - 10u; ms += 10u) {
        b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, NULL, 0u, false);
    }
    TEST_ASSERT_TRUE(b.block.amp);
    TEST_ASSERT_FALSE(snd_busy(&B));
    b = snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, NULL, 0u, false);
    TEST_ASSERT_FALSE(b.block.amp);
    TEST_ASSERT_TRUE(snd_idle(&B));
}

/* ---- AUD-028, AUD-005: tone tickets (D04: speech starts after the tone's final block) ----- */

static void expect_ticket(snd_mixer_t *m, uint32_t ticket, uint8_t outcome)
{
    snd_ticket_t t;
    TEST_ASSERT_TRUE_MESSAGE(snd_ticket_next(m, &t), "an outcome is waiting");
    TEST_ASSERT_EQUAL_UINT32(ticket, t.ticket);
    TEST_ASSERT_EQUAL_UINT8(outcome, t.outcome);
}

static void expect_no_ticket(snd_mixer_t *m)
{
    snd_ticket_t t;
    TEST_ASSERT_FALSE(snd_ticket_next(m, &t));
}

/* PLAYED arrives with the block that holds the tone's last sample: never before, and nothing of
 * the tone follows it, so speech started then never overlaps the chime. */
static void test_a_ticket_is_played_with_the_tones_last_block(void)
{
    static const alarm_sound_t shots[] = {ALARM_SOUND_ACK, ALARM_SOUND_CHIME, ALARM_SOUND_ERROR,
                                          ALARM_SOUND_WALK_TEST,
                                          ALARM_SOUND_WALK_TEST_WARNING};
    static const uint32_t rates[] = {8000u, 22100u, 32000u};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (size_t k = 0; k < sizeof(shots) / sizeof(shots[0]); k++) {
            s_rate = rates[r];
            setUp();
            TEST_ASSERT_TRUE(snd_play_tracked(&M, shots[k], 7u + (uint32_t)k));
            size_t last_sound = 0;
            size_t played_at = 0;
            for (uint32_t blk = 0; blk < 200u; blk++) {
                (void)render_block();
                for (size_t i = s_n - snd_block_frames(&M); i < s_n; i++) {
                    if (s_pcm[i] != 0) {
                        last_sound = i;
                    }
                }
                snd_ticket_t t;
                if (snd_ticket_next(&M, &t)) {
                    TEST_ASSERT_EQUAL_UINT32(7u + (uint32_t)k, t.ticket);
                    TEST_ASSERT_EQUAL_UINT8(SND_TICKET_PLAYED, t.outcome);
                    TEST_ASSERT_EQUAL_size_t(0u, played_at);
                    played_at = s_n;
                }
            }
            TEST_ASSERT_NOT_EQUAL(0u, played_at);
            TEST_ASSERT_LESS_THAN_size_t(played_at, last_sound); /* the tone was over */
            /* and its last sample was in that block, not one before */
            TEST_ASSERT_GREATER_OR_EQUAL_size_t(played_at - snd_block_frames(&M), last_sound);
            tearDown();
        }
    }
}

static void test_tickets_report_in_turn_and_tracking_changes_no_tone(void)
{
    pair_init();
    TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_ACK, 1u));
    TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_CHIME, 2u));
    TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_ERROR, 0u)); /* untracked: no outcome */
    snd_play(&A, ALARM_SOUND_ACK);
    snd_play(&A, ALARM_SOUND_CHIME);
    snd_play(&A, ALARM_SOUND_ERROR);
    for (uint32_t blk = 0; blk < 250u; blk++) {
        (void)snd_render(&A, s_block, SND_BLOCK_FRAMES);
        (void)snd_render(&B, s_block_b, SND_BLOCK_FRAMES);
        TEST_ASSERT_EQUAL_INT16_ARRAY(s_block, s_block_b, SND_BLOCK_FRAMES * 2u); /* AUD-005 */
    }
    expect_ticket(&B, 1u, SND_TICKET_PLAYED);
    expect_ticket(&B, 2u, SND_TICKET_PLAYED);
    expect_no_ticket(&B);
}

/* An alarm drops the one-shot playing and those waiting: each ticket is CANCELED at once. */
static void test_an_alarm_cancels_every_ticket(void)
{
    static const alarm_sound_t loud[] = {ALARM_SOUND_AUX, ALARM_SOUND_ALARM, ALARM_SOUND_CO,
                                         ALARM_SOUND_FIRE};
    for (size_t p = 0; p < sizeof(loud) / sizeof(loud[0]); p++) {
        setUp();
        TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ERROR, 10u));
        TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_CHIME, 11u));
        render_ms(100u);
        expect_no_ticket(&M);
        snd_set_continuous(&M, loud[p]);
        (void)render_block();
        expect_ticket(&M, 11u, SND_TICKET_CANCELED);
        expect_ticket(&M, 10u, SND_TICKET_CANCELED);
        expect_no_ticket(&M);
        /* one asked for during the alarm is canceled too, never heard */
        TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ACK, 12u));
        (void)render_block();
        expect_ticket(&M, 12u, SND_TICKET_CANCELED);
    }
}

static void test_refused_and_dropped_tickets_are_canceled(void)
{
    /* not a one-shot */
    TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_TROUBLE, 20u));
    expect_ticket(&M, 20u, SND_TICKET_CANCELED);
    /* the queue is full: the fifth waits nowhere */
    for (uint32_t i = 0; i < SND_ONESHOT_QUEUE; i++) {
        TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ACK, 21u + i));
    }
    TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_ACK, 99u));
    expect_ticket(&M, 99u, SND_TICKET_CANCELED);
    expect_no_ticket(&M);
    /* a rate change drops the one playing; the waiting ones play afterwards */
    render_ms(50u);
    TEST_ASSERT_TRUE(snd_set_rate(&M, 16000u));
    expect_ticket(&M, 21u, SND_TICKET_CANCELED);
    render_ms(1000u);
    expect_ticket(&M, 22u, SND_TICKET_PLAYED);
    expect_ticket(&M, 23u, SND_TICKET_PLAYED);
    expect_ticket(&M, 24u, SND_TICKET_PLAYED);
    expect_no_ticket(&M);
}

static void test_power_saving_cancels_the_chimes_ticket(void)
{
    TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_CHIME, 30u));
    TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_CHIME, 31u));
    TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ACK, 32u));
    render_ms(100u);
    snd_set_power_saving(&M, true);
    TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_CHIME, 33u)); /* refused at once */
    expect_ticket(&M, 33u, SND_TICKET_CANCELED);
    (void)render_block();
    expect_ticket(&M, 30u, SND_TICKET_CANCELED); /* the one playing */
    expect_ticket(&M, 31u, SND_TICKET_CANCELED); /* the one waiting */
    render_ms(300u);
    expect_ticket(&M, 32u, SND_TICKET_PLAYED); /* the ack is no cosmetic sound */
    expect_no_ticket(&M);
}

/* Nobody reading: the ring keeps the newest SND_TICKET_RING outcomes and counts the rest. */
static void test_the_ticket_ring_keeps_the_newest(void)
{
    for (uint32_t i = 1; i <= SND_TICKET_RING + 4u; i++) {
        TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_NONE, i));
    }
    TEST_ASSERT_EQUAL_UINT32(4u, M.done_lost);
    for (uint32_t i = 5; i <= SND_TICKET_RING + 4u; i++) {
        expect_ticket(&M, i, SND_TICKET_CANCELED);
    }
    expect_no_ticket(&M);
    TEST_ASSERT_FALSE(snd_ticket_next(NULL, NULL));
    TEST_ASSERT_FALSE(snd_play_tracked(NULL, ALARM_SOUND_ACK, 1u));
}

/* ---- AUD-028: a block the speaker never got (the panel's sink write failed, sounder.c) ---- */

/* Render one block on M as the panel's task does; 'fail' plays the sink refusing it. */
static void render_block_written(bool fail)
{
    const uint8_t n_before = M.done_n;
    const uint32_t lost_before = M.done_lost;
    (void)render_block();
    if (fail) {
        snd_tickets_cancel_newest(&M, lost_before, n_before);
        snd_ticket_abandon_active(&M);
    }
}

/* AUD-028: the tone's final block fails: its PLAYED becomes CANCELED, while an older tone that
 * was written in full keeps its PLAYED. */
static void test_a_failed_final_block_cancels_only_its_ticket(void)
{
    TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ACK, 40u));
    for (uint32_t blk = 0; blk < 100u && M.done_n == 0u; blk++) {
        render_block_written(false);
    }
    TEST_ASSERT_EQUAL_UINT8(1u, M.done_n); /* 40 PLAYED, unread */
    TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ACK, 41u));
    uint32_t blk = 0;
    for (; blk < 100u; blk++) {
        const uint8_t n_before = M.done_n;
        const uint32_t lost_before = M.done_lost;
        (void)render_block();
        if (M.done_n != n_before) { /* the final block: the write fails */
            snd_tickets_cancel_newest(&M, lost_before, n_before);
            snd_ticket_abandon_active(&M);
            break;
        }
    }
    TEST_ASSERT_LESS_THAN_UINT32(100u, blk);
    render_ms(200u);
    expect_ticket(&M, 40u, SND_TICKET_PLAYED);
    expect_ticket(&M, 41u, SND_TICKET_CANCELED);
    expect_no_ticket(&M);
}

/* AUD-028: a block in the middle of the tone fails: its ticket is CANCELED at once and never
 * becomes PLAYED, the tone itself plays on sample for sample, and the next tone is unaffected. */
static void test_a_failed_middle_block_cancels_the_tone_and_keeps_its_sound(void)
{
    TEST_ASSERT_TRUE(snd_play_tracked(&M, ALARM_SOUND_ACK, 49u)); /* an older one, played */
    for (uint32_t blk = 0; blk < 100u && M.done_n == 0u; blk++) {
        render_block_written(false);
    }
    pair_init();
    snd_play(&A, ALARM_SOUND_CHIME);
    snd_play(&A, ALARM_SOUND_ACK);
    TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_CHIME, 50u));
    TEST_ASSERT_TRUE(snd_play_tracked(&B, ALARM_SOUND_ACK, 51u));
    for (uint32_t blk = 0; blk < 200u; blk++) {
        const uint8_t n_before = B.done_n;
        const uint32_t lost_before = B.done_lost;
        (void)snd_render(&A, s_block, SND_BLOCK_FRAMES);
        (void)snd_render(&B, s_block_b, SND_BLOCK_FRAMES);
        TEST_ASSERT_EQUAL_INT16_ARRAY(s_block, s_block_b, SND_BLOCK_FRAMES * 2u);
        if (blk == 20u || blk == 21u) { /* the chime is sounding: two writes fail */
            snd_tickets_cancel_newest(&B, lost_before, n_before);
            snd_ticket_abandon_active(&B);
            TEST_ASSERT_EQUAL_UINT8(1u, B.done_n); /* CANCELED at once, and only once */
        }
    }
    expect_ticket(&B, 50u, SND_TICKET_CANCELED);
    expect_ticket(&B, 51u, SND_TICKET_PLAYED);
    expect_no_ticket(&B);
    expect_ticket(&M, 49u, SND_TICKET_PLAYED);
    /* nothing tracked is playing: abandoning is a no-op, and NULL is safe */
    snd_ticket_abandon_active(&B);
    expect_no_ticket(&B);
    snd_ticket_abandon_active(NULL);
    snd_tickets_cancel_newest(NULL, 0u, 0u);
}

/* AUD-028: the failed block pushed older outcomes out of a full ring (done_lost moved): only the
 * outcomes it appended are converted, never more than the ring holds. */
static void test_a_failed_block_counts_outcomes_lost_from_a_full_ring(void)
{
    for (uint32_t i = 0; i < SND_TICKET_RING; i++) {
        M.done[i].ticket = 60u + i;
        M.done[i].outcome = SND_TICKET_PLAYED;
    }
    M.done_head = 5u;
    M.done_n = SND_TICKET_RING;
    M.done_lost = 0xFFFFFFFEu; /* wraps across the block */
    const uint8_t n_before = M.done_n;
    const uint32_t lost_before = M.done_lost;
    TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_NONE, 90u)); /* appended CANCELED */
    M.done[(M.done_head + SND_TICKET_RING - 1u) % SND_TICKET_RING].outcome = SND_TICKET_PLAYED;
    TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_NONE, 91u));
    M.done[(M.done_head + SND_TICKET_RING - 1u) % SND_TICKET_RING].outcome = SND_TICKET_PLAYED;
    snd_tickets_cancel_newest(&M, lost_before, n_before);
    for (uint32_t i = 0; i < SND_TICKET_RING - 2u; i++) {
        snd_ticket_t t;
        TEST_ASSERT_TRUE(snd_ticket_next(&M, &t));
        TEST_ASSERT_EQUAL_UINT8(SND_TICKET_PLAYED, t.outcome); /* older ones untouched */
    }
    expect_ticket(&M, 90u, SND_TICKET_CANCELED);
    expect_ticket(&M, 91u, SND_TICKET_CANCELED);
    expect_no_ticket(&M);

    /* more appended than the ring holds: every entry converts, nothing outside it is touched */
    snd_init(&M);
    for (uint32_t i = 0; i < SND_TICKET_RING + 3u; i++) {
        TEST_ASSERT_FALSE(snd_play_tracked(&M, ALARM_SOUND_NONE, 100u + i));
        M.done[(M.done_head + M.done_n - 1u) % SND_TICKET_RING].outcome = SND_TICKET_PLAYED;
    }
    snd_tickets_cancel_newest(&M, 0u, 0u);
    for (uint32_t i = 3; i < SND_TICKET_RING + 3u; i++) {
        expect_ticket(&M, 100u + i, SND_TICKET_CANCELED);
    }
    expect_no_ticket(&M);
}

/* AUD-028: the speech ring's free space, from cursors that run 0..2*slots-1. */
static void test_speech_free_space_never_wraps(void)
{
    const uint32_t slots = 12u;
    const size_t per = SND_BLOCK_FRAMES;
    TEST_ASSERT_EQUAL_size_t(12u * per, snd_speech_free(0u, 0u, slots, per));   /* empty */
    TEST_ASSERT_EQUAL_size_t(7u * per, snd_speech_free(5u, 0u, slots, per));
    TEST_ASSERT_EQUAL_size_t(0u, snd_speech_free(12u, 0u, slots, per));         /* full */
    TEST_ASSERT_EQUAL_size_t(0u, snd_speech_free(3u, 15u, slots, per));         /* full, wrapped */
    TEST_ASSERT_EQUAL_size_t(10u * per, snd_speech_free(1u, 23u, slots, per));  /* wrapped */
    /* head and tail read apart from another task: tail seen ahead of head */
    TEST_ASSERT_EQUAL_size_t(0u, snd_speech_free(6u, 7u, slots, per));
    TEST_ASSERT_EQUAL_size_t(0u, snd_speech_free(0u, 11u, slots, per));         /* used 13 */
    TEST_ASSERT_EQUAL_size_t(0u, snd_speech_free(0u, 0u, 0u, per));             /* no ring */
}

/* ---- AUD-032: Beeps, Chime and Voice volumes, each on its own ---------------------------- */

/* AUD-032: the three start at 70 and clamp like the beeps volume. */
static void test_the_three_volumes_default_and_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT8(70u, M.volume);
    TEST_ASSERT_EQUAL_UINT8(70u, M.chime_volume);
    TEST_ASSERT_EQUAL_UINT8(70u, M.voice_volume);
    snd_set_chime_volume(&M, 255u);
    snd_set_voice_volume(&M, 101u);
    TEST_ASSERT_EQUAL_UINT8(100u, M.chime_volume);
    TEST_ASSERT_EQUAL_UINT8(100u, M.voice_volume);
    TEST_ASSERT_EQUAL_UINT8(70u, M.volume);
    snd_set_chime_volume(NULL, 10u); /* no mixer: nothing happens */
    snd_set_voice_volume(NULL, 10u);
}

/* The chime's loudest sample at these volumes, as a one-shot or from snd_play_background. */
static int16_t chime_peak(uint8_t beeps, uint8_t chime, uint8_t voice, bool background)
{
    setUp();
    snd_set_volume(&M, beeps);
    snd_set_chime_volume(&M, chime);
    snd_set_voice_volume(&M, voice);
    if (background) {
        TEST_ASSERT_TRUE(snd_play_background(&M, ALARM_SOUND_CHIME));
    } else {
        snd_play(&M, ALARM_SOUND_CHIME);
    }
    render_ms(1200u);
    return peak();
}

/* AUD-032: the door chime follows the chime volume, and neither the beeps nor the voice one. */
static void test_the_chime_follows_the_chime_volume_alone(void)
{
    for (int bg = 0; bg < 2; bg++) {
        const int16_t full = chime_peak(0u, 100u, 0u, bg != 0);
        const int16_t half = chime_peak(100u, 50u, 100u, bg != 0);
        TEST_ASSERT_TRUE(full > 25000);
        TEST_ASSERT_INT16_WITHIN(full / 10, full / 2, half);
        TEST_ASSERT_EQUAL_INT16(0, chime_peak(100u, 0u, 100u, bg != 0));
    }
}

/* AUD-032, AUD-033: neither the beeps nor the chime volume touches speech in the mixer. */
static void test_speech_follows_the_voice_volume_alone(void)
{
    static const uint8_t others[] = {0u, 100u};
    for (size_t k = 0; k < 2u; k++) {
        pair_init();
        snd_set_volume(&B, others[k]);
        snd_set_chime_volume(&B, others[k]);
        snd_set_voice_volume(&B, 40u);
        fill_speech_const(10000);
        (void)snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
        const snd_pcm_block_t b =
            snd_render_pcm(&B, s_block_b, SND_BLOCK_FRAMES, s_speech, SND_BLOCK_FRAMES, true);
        TEST_ASSERT_EQUAL_size_t(SND_BLOCK_FRAMES, b.speech_consumed);
        TEST_ASSERT_EQUAL_INT32(expect_mix(0, 10000, false), s_block_b[0]); /* at unity */
    }
}

/* AUD-032: a key click and the trouble beeps follow the beeps volume, not the chime or voice. */
static void test_beeps_follow_the_beeps_volume_alone(void)
{
    for (int click = 0; click < 2; click++) {
        int16_t got[2];
        for (int loud = 0; loud < 2; loud++) {
            setUp();
            snd_set_volume(&M, loud ? 100u : 0u);
            snd_set_chime_volume(&M, loud ? 0u : 100u);
            snd_set_voice_volume(&M, loud ? 0u : 100u);
            if (click) {
                snd_key_click(&M);
            } else {
                snd_set_continuous(&M, ALARM_SOUND_TROUBLE);
            }
            render_ms(300u);
            got[loud] = peak();
        }
        TEST_ASSERT_TRUE(got[1] > 10000);
        TEST_ASSERT_EQUAL_INT16(0, got[0]);
    }
}

/* AUD-003, AUD-032: alarm, fire, CO and aux ignore all three volumes. */
static void test_alarms_ignore_all_three_volumes(void)
{
    static const alarm_sound_t LOUD[] = {ALARM_SOUND_ALARM, ALARM_SOUND_FIRE, ALARM_SOUND_CO,
                                         ALARM_SOUND_AUX};
    for (size_t i = 0; i < sizeof(LOUD) / sizeof(LOUD[0]); i++) {
        setUp();
        snd_set_volume(&M, 0u);
        snd_set_chime_volume(&M, 0u);
        snd_set_voice_volume(&M, 0u);
        snd_set_continuous(&M, LOUD[i]);
        render_ms(600u);
        TEST_ASSERT_TRUE_MESSAGE(peak() > 25000, alarm_sound_name(LOUD[i]));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fire_is_temporal_3);
    RUN_TEST(test_co_is_temporal_4);
    RUN_TEST(test_every_continuous_sound_plays);
    RUN_TEST(test_no_sound_is_silence);
    RUN_TEST(test_a_new_pattern_starts_from_its_beginning);
    RUN_TEST(test_the_same_pattern_is_not_restarted);
    RUN_TEST(test_alarms_play_at_full_volume);
    RUN_TEST(test_the_volume_moves_everything_else);
    RUN_TEST(test_the_volume_is_clamped);
    RUN_TEST(test_a_one_shot_pauses_the_pattern_and_it_resumes);
    RUN_TEST(test_alarms_drown_one_shots);
    RUN_TEST(test_a_one_shot_is_heard_over_a_quiet_pattern);
    RUN_TEST(test_one_shots_queue_and_then_drop);
    RUN_TEST(test_key_clicks_stop_at_two_waiting);
    RUN_TEST(test_one_shots_play_in_turn);
    RUN_TEST(test_the_walk_test_is_three_beeps);
    RUN_TEST(test_the_walk_test_warning_is_a_double_beep);
    RUN_TEST(test_the_chime_is_two_notes);
    RUN_TEST(test_a_background_chime_plays_only_over_silence);
    RUN_TEST(test_a_background_chime_gives_way_to_every_pattern);
    RUN_TEST(test_a_background_chime_gives_way_to_one_shots);
    RUN_TEST(test_a_background_chime_is_cosmetic);
    RUN_TEST(test_the_first_block_is_silent);
    RUN_TEST(test_the_amplifier_rests_after_the_sound);
    RUN_TEST(test_a_waiting_pattern_keeps_the_amplifier_on);
    RUN_TEST(test_power_saving_drops_the_cosmetic_sounds);
    RUN_TEST(test_power_saving_silences_a_chime_already_playing);
    RUN_TEST(test_power_saving_never_touches_an_alarm);
    RUN_TEST(test_life_safety_at_every_rate);
    RUN_TEST(test_the_entry_beeps_at_every_rate);
    RUN_TEST(test_blocks_and_the_amplifier_follow_the_rate);
    RUN_TEST(test_rates_outside_the_range_are_refused);
    RUN_TEST(test_a_rate_change_restarts_the_pattern);
    RUN_TEST(test_no_speech_renders_exactly_the_tones);
    RUN_TEST(test_speech_rides_on_top_of_the_tones);
    RUN_TEST(test_speech_joins_at_unity);
    RUN_TEST(test_the_exit_beeps_wait_for_the_words);
    RUN_TEST(test_the_exit_beeps_wait_10_s_at_most);
    RUN_TEST(test_the_first_exit_beep_waits_for_the_arming_words);
    RUN_TEST(test_speech_due_changes_no_other_sound);
    RUN_TEST(test_speech_due_alone_holds_the_exit_beeps_10_s_at_most);
    RUN_TEST(test_the_exit_beeps_wait_for_words_still_filling);
    RUN_TEST(test_a_one_shot_sounds_while_the_exit_beeps_wait);
    RUN_TEST(test_a_new_exit_delay_waits_its_full_10_s);
    RUN_TEST(test_the_three_volumes_default_and_clamp);
    RUN_TEST(test_the_chime_follows_the_chime_volume_alone);
    RUN_TEST(test_speech_follows_the_voice_volume_alone);
    RUN_TEST(test_beeps_follow_the_beeps_volume_alone);
    RUN_TEST(test_alarms_ignore_all_three_volumes);
    RUN_TEST(test_speech_is_clamped_to_the_headroom);
    RUN_TEST(test_loud_patterns_reject_speech);
    RUN_TEST(test_speech_waits_for_the_warm_up);
    RUN_TEST(test_a_short_tail_is_consumed_exactly);
    RUN_TEST(test_speech_keeps_the_amplifier_on);
    RUN_TEST(test_a_ticket_is_played_with_the_tones_last_block);
    RUN_TEST(test_tickets_report_in_turn_and_tracking_changes_no_tone);
    RUN_TEST(test_an_alarm_cancels_every_ticket);
    RUN_TEST(test_refused_and_dropped_tickets_are_canceled);
    RUN_TEST(test_power_saving_cancels_the_chimes_ticket);
    RUN_TEST(test_the_ticket_ring_keeps_the_newest);
    RUN_TEST(test_a_failed_final_block_cancels_only_its_ticket);
    RUN_TEST(test_a_failed_middle_block_cancels_the_tone_and_keeps_its_sound);
    RUN_TEST(test_a_failed_block_counts_outcomes_lost_from_a_full_ring);
    RUN_TEST(test_speech_free_space_never_wraps);
    return UNITY_END();
}
