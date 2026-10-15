/*
 * sounder_mix.c - the pattern tables and the sine synthesizer behind them.
 *
 * Tones sit in a musical, speaker-friendly range rather than a piezo's 3 kHz; only fire and
 * CO use the high, standard alarm pitch. Timing is what makes a pattern recognisable, and the
 * fire (temporal-3, NFPA 72) and CO (temporal-4, NFPA 720) timings follow the standards
 * exactly. The table below is the one in docs/features/08-Audio-and-Annunciation.
 */
#include "sounder/sounder_mix.h"

#include <math.h>
#include <string.h>

#define RAMP_MS      3u /* attack / release: no clicks */
#define TWO_PI       6.28318531f
#define FULL_SCALE   30000.0f /* headroom below int16 so the ramps stay clean */
#define SPEECH_UNITY 32768 /* Q15 speech gain of 1.0 */

typedef enum { ENV_FLAT = 0, ENV_DECAY } env_t;

typedef struct {
    uint16_t hz; /* 0 = silence */
    uint16_t ms;
    uint8_t env;
} step_t;

struct snd_pattern {
    const step_t *steps;
    uint8_t count;
    bool loop;
    bool loud; /* full volume, suppresses one-shots */
};

#define PAT(arr, loop, loud) {arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])), loop, loud}

/* One-shots */
static const step_t S_KEY[] = {{2200, 30, ENV_DECAY}};
static const step_t S_ACK[] = {{1480, 150, ENV_FLAT}};
static const step_t S_ERROR[] = {{440, 800, ENV_FLAT}}; /* one long beep */
static const step_t S_CHIME[] = {{1319, 280, ENV_DECAY}, {1047, 700, ENV_DECAY}};
static const step_t S_WALK[] = {{1760, 70, ENV_FLAT}, {0, 60, ENV_FLAT}, {1760, 70, ENV_FLAT},
                                {0, 60, ENV_FLAT}, {1760, 70, ENV_FLAT}};
/* The walk test ends in 5 or 1 minutes (docs/features/40 WALK-004): a double beep, longer and
 * lower than the walk test's three, so a tester tells them apart. */
static const step_t S_WALK_WARN[] = {{1480, 200, ENV_FLAT}, {0, 120, ENV_FLAT},
                                     {1480, 200, ENV_FLAT}};
/* Continuous */
static const step_t S_TROUBLE[] = {{1175, 100, ENV_FLAT}, {0, 100, ENV_FLAT},
                                   {1175, 100, ENV_FLAT}, {0, 9700, ENV_FLAT}};
static const step_t S_EXIT[] = {{1320, 80, ENV_FLAT}, {0, 920, ENV_FLAT}};
static const step_t S_EXIT_FAST[] = {{1320, 80, ENV_FLAT}, {0, 170, ENV_FLAT}};
static const step_t S_ENTRY[] = {{1568, 250, ENV_FLAT}, {0, 250, ENV_FLAT}};
static const step_t S_ENTRY_MEM[] = {{1568, 80, ENV_FLAT}, {0, 80, ENV_FLAT}};
static const step_t S_AUX[] = {{1760, 400, ENV_FLAT}, {0, 200, ENV_FLAT}};
static const step_t S_ALARM[] = {{880, 220, ENV_FLAT}, {1245, 220, ENV_FLAT}};
/* Temporal-3 (fire, NFPA 72): 3 x (0.5 s on, 0.5 s off), then 1.5 s off. */
static const step_t S_FIRE[] = {{3100, 500, ENV_FLAT}, {0, 500, ENV_FLAT},
                                {3100, 500, ENV_FLAT}, {0, 500, ENV_FLAT},
                                {3100, 500, ENV_FLAT}, {0, 1500, ENV_FLAT}};
/* Temporal-4 (CO, NFPA 720): 4 x (100 ms on, 100 ms off), then 5 s off. */
static const step_t S_CO[] = {{3100, 100, ENV_FLAT}, {0, 100, ENV_FLAT},
                              {3100, 100, ENV_FLAT}, {0, 100, ENV_FLAT},
                              {3100, 100, ENV_FLAT}, {0, 100, ENV_FLAT},
                              {3100, 100, ENV_FLAT}, {0, 5100, ENV_FLAT}};

static const struct snd_pattern P_KEY = PAT(S_KEY, false, false);
static const struct snd_pattern P_ACK = PAT(S_ACK, false, false);
static const struct snd_pattern P_ERROR = PAT(S_ERROR, false, false);
static const struct snd_pattern P_CHIME = PAT(S_CHIME, false, false);
static const struct snd_pattern P_WALK = PAT(S_WALK, false, false);
static const struct snd_pattern P_WALK_WARN = PAT(S_WALK_WARN, false, false);
static const struct snd_pattern P_TROUBLE = PAT(S_TROUBLE, true, false);
static const struct snd_pattern P_EXIT = PAT(S_EXIT, true, false);
static const struct snd_pattern P_EXIT_FAST = PAT(S_EXIT_FAST, true, false);
static const struct snd_pattern P_ENTRY = PAT(S_ENTRY, true, false);
static const struct snd_pattern P_ENTRY_MEM = PAT(S_ENTRY_MEM, true, false);
static const struct snd_pattern P_AUX = PAT(S_AUX, true, true);
static const struct snd_pattern P_ALARM = PAT(S_ALARM, true, true);
static const struct snd_pattern P_FIRE = PAT(S_FIRE, true, true);
static const struct snd_pattern P_CO = PAT(S_CO, true, true);

static const struct snd_pattern *pattern_for(uint8_t sound)
{
    switch (sound) {
    case SND_KEY_CLICK: return &P_KEY;
    case ALARM_SOUND_ACK: return &P_ACK;
    case ALARM_SOUND_ERROR: return &P_ERROR;
    case ALARM_SOUND_CHIME: return &P_CHIME;
    case ALARM_SOUND_WALK_TEST: return &P_WALK;
    case ALARM_SOUND_WALK_TEST_WARNING: return &P_WALK_WARN;
    case ALARM_SOUND_TROUBLE: return &P_TROUBLE;
    case ALARM_SOUND_EXIT: return &P_EXIT;
    case ALARM_SOUND_EXIT_FAST: return &P_EXIT_FAST;
    case ALARM_SOUND_ENTRY: return &P_ENTRY;
    case ALARM_SOUND_ENTRY_MEMORY: return &P_ENTRY_MEM;
    case ALARM_SOUND_AUX: return &P_AUX;
    case ALARM_SOUND_ALARM: return &P_ALARM;
    case ALARM_SOUND_CO: return &P_CO;
    case ALARM_SOUND_FIRE: return &P_FIRE;
    default: return NULL;
    }
}

/* Cosmetic: dropped while the panel is saving power, and while an alarm sounds. */
static bool is_cosmetic(uint8_t sound)
{
    return sound == SND_KEY_CLICK || sound == ALARM_SOUND_CHIME;
}

/* ---- one voice ------------------------------------------------------------------------- */

static void voice_step(snd_voice_t *v, uint32_t rate)
{
    const step_t *st = &v->pat->steps[v->step];
    v->pos = 0;
    v->len = (uint32_t)st->ms * rate / 1000u;
    v->decay = 1.0f;
    /* Decay to about 5 % over the step (3 time constants). */
    v->decay_mul = (st->env == ENV_DECAY && v->len > 0u) ? expf(-3.0f / (float)v->len) : 1.0f;
}

static void voice_start(snd_voice_t *v, const struct snd_pattern *pat, uint32_t rate)
{
    memset(v, 0, sizeof(*v));
    v->pat = pat;
    if (pat != NULL) {
        voice_step(v, rate);
    }
}

/* Next sample in -1..1; returns false when a one-shot pattern has finished. */
static bool voice_sample(snd_voice_t *v, float *out, uint32_t rate, uint32_t ramp)
{
    if (v->pat == NULL) {
        *out = 0.0f;
        return false;
    }
    while (v->pos >= v->len) {
        v->step++;
        if (v->step >= v->pat->count) {
            if (!v->pat->loop) {
                v->pat = NULL;
                *out = 0.0f;
                return false;
            }
            v->step = 0;
        }
        voice_step(v, rate);
    }
    const step_t *st = &v->pat->steps[v->step];
    float s = 0.0f;
    if (st->hz != 0u) {
        float env = 1.0f;
        if (v->pos < ramp) {
            env = (float)v->pos / (float)ramp;
        } else if (v->len - v->pos < ramp) {
            env = (float)(v->len - v->pos) / (float)ramp;
        }
        s = sinf(v->phase) * env * v->decay;
        v->phase += TWO_PI * (float)st->hz / (float)rate;
        if (v->phase >= TWO_PI) {
            v->phase -= TWO_PI;
        }
        v->decay *= v->decay_mul;
    } else {
        v->phase = 0.0f;
    }
    v->pos++;
    *out = s;
    return true;
}

/* ---- tickets --------------------------------------------------------------------------- */

/* Every nonzero ticket gets exactly one outcome. A full ring drops its oldest: a reader that
 * stopped reading has stopped waiting too. */
static void ticket_done(snd_mixer_t *m, uint32_t ticket, uint8_t outcome)
{
    if (ticket == 0u) {
        return;
    }
    if (m->done_n >= SND_TICKET_RING) {
        m->done_head = (uint8_t)((m->done_head + 1u) % SND_TICKET_RING);
        m->done_n--;
        m->done_lost++;
    }
    const uint8_t at = (uint8_t)((m->done_head + m->done_n) % SND_TICKET_RING);
    m->done[at].ticket = ticket;
    m->done[at].outcome = outcome;
    m->done_n++;
}

/* The one-shot playing stops before its end. */
static void shot_kill(snd_mixer_t *m)
{
    ticket_done(m, m->shot_ticket, SND_TICKET_CANCELED);
    m->shot_ticket = 0u;
    voice_start(&m->shot, NULL, m->rate);
}

/* ---- the mixer ------------------------------------------------------------------------- */

void snd_init(snd_mixer_t *m)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->cont_sound = ALARM_SOUND_NONE;
    m->volume = 70u;
    m->chime_volume = 70u;
    m->voice_volume = 70u;
    m->rate = SND_SAMPLE_RATE;
    m->ramp = SND_SAMPLE_RATE * RAMP_MS / 1000u;
}

bool snd_set_rate(snd_mixer_t *m, uint32_t hz)
{
    if (m == NULL || hz < SND_RATE_MIN || hz > SND_SAMPLE_RATE || hz % 100u != 0u) {
        return false;
    }
    if (hz != m->rate) {
        m->rate = hz;
        m->ramp = hz * RAMP_MS / 1000u;
        shot_kill(m);
        voice_start(&m->cont, pattern_for(m->cont_sound), hz); /* from the pattern's start */
        m->exit_held = 0u;
    }
    return true;
}

uint32_t snd_rate(const snd_mixer_t *m)
{
    return m != NULL ? m->rate : SND_SAMPLE_RATE;
}

size_t snd_block_frames(const snd_mixer_t *m)
{
    return (size_t)(snd_rate(m) / 100u);
}

/* Any other sound asked for: a background one-shot stops at once (snd_play_background, D22). */
static void background_yield(snd_mixer_t *m)
{
    if (m->background) {
        m->background = false;
        voice_start(&m->shot, NULL, m->rate);
    }
}

void snd_set_continuous(snd_mixer_t *m, alarm_sound_t sound)
{
    if (m == NULL) {
        return;
    }
    const uint8_t want = (sound <= ALARM_SOUND_FIRE) ? (uint8_t)sound : (uint8_t)ALARM_SOUND_NONE;
    if (want != m->cont_sound) {
        if (want != (uint8_t)ALARM_SOUND_NONE) {
            background_yield(m);
        }
        m->cont_sound = want;
        m->exit_held = 0u;
        voice_start(&m->cont, pattern_for(want), m->rate);
    }
}

/* A one-shot only reaches the queue if it could still be heard. */
static bool queue_oneshot(snd_mixer_t *m, uint8_t sound, uint32_t ticket)
{
    background_yield(m);
    bool ok = !(m->power_saving && is_cosmetic(sound)) && m->pending_n < SND_ONESHOT_QUEUE;
    if (ok && sound == SND_KEY_CLICK && m->pending_n >= 2u) {
        ok = false; /* a fast typist is ahead of the speaker: drop the extra clicks */
    }
    if (!ok) {
        ticket_done(m, ticket, SND_TICKET_CANCELED);
        return false;
    }
    m->pending_ticket[m->pending_n] = ticket;
    m->pending[m->pending_n++] = sound;
    return true;
}

void snd_play(snd_mixer_t *m, alarm_sound_t sound)
{
    (void)snd_play_tracked(m, sound, 0u);
}

bool snd_play_tracked(snd_mixer_t *m, alarm_sound_t sound, uint32_t ticket)
{
    if (m == NULL) {
        return false;
    }
    if (sound < ALARM_SOUND_ACK || pattern_for((uint8_t)sound) == NULL) {
        ticket_done(m, ticket, SND_TICKET_CANCELED);
        return false;
    }
    return queue_oneshot(m, (uint8_t)sound, ticket);
}

bool snd_ticket_next(snd_mixer_t *m, snd_ticket_t *out)
{
    if (m == NULL || out == NULL || m->done_n == 0u) {
        return false;
    }
    *out = m->done[m->done_head];
    m->done_head = (uint8_t)((m->done_head + 1u) % SND_TICKET_RING);
    m->done_n--;
    return true;
}

void snd_tickets_cancel_newest(snd_mixer_t *m, uint32_t lost_before, uint8_t n_before)
{
    if (m == NULL) {
        return;
    }
    const uint32_t appended = (uint32_t)m->done_n + (m->done_lost - lost_before) - (uint32_t)n_before;
    const uint32_t k = appended < m->done_n ? appended : m->done_n;
    for (uint32_t i = m->done_n - k; i < m->done_n; i++) {
        snd_ticket_t *t = &m->done[(m->done_head + i) % SND_TICKET_RING];
        if (t->outcome == SND_TICKET_PLAYED) {
            t->outcome = SND_TICKET_CANCELED;
        }
    }
}

void snd_ticket_abandon_active(snd_mixer_t *m)
{
    if (m != NULL && m->shot_ticket != 0u) {
        ticket_done(m, m->shot_ticket, SND_TICKET_CANCELED);
        m->shot_ticket = 0u; /* the voice is untouched: the tone plays on */
    }
}

size_t snd_speech_free(uint32_t head, uint32_t tail, uint32_t slots, size_t slot_samples)
{
    if (slots == 0u || slots > UINT32_MAX / 2u) {
        return 0u;
    }
    const uint32_t wrap = 2u * slots;
    const uint32_t used = (head % wrap + wrap - tail % wrap) % wrap;
    return used <= slots ? (size_t)(slots - used) * slot_samples : 0u;
}

bool snd_play_background(snd_mixer_t *m, alarm_sound_t sound)
{
    if (m == NULL || sound < ALARM_SOUND_ACK || pattern_for((uint8_t)sound) == NULL) {
        return false;
    }
    if (m->power_saving || snd_busy(m)) {
        return false; /* cosmetic; and only over silence */
    }
    voice_start(&m->shot, pattern_for((uint8_t)sound), m->rate);
    m->background = true;
    return true;
}

void snd_key_click(snd_mixer_t *m)
{
    if (m != NULL) {
        (void)queue_oneshot(m, SND_KEY_CLICK, 0u);
    }
}

void snd_set_volume(snd_mixer_t *m, uint8_t percent)
{
    if (m != NULL) {
        m->volume = percent > 100u ? 100u : percent;
    }
}

void snd_set_chime_volume(snd_mixer_t *m, uint8_t percent)
{
    if (m != NULL) {
        m->chime_volume = percent > 100u ? 100u : percent;
    }
}

void snd_set_voice_volume(snd_mixer_t *m, uint8_t percent)
{
    if (m != NULL) {
        m->voice_volume = percent > 100u ? 100u : percent;
    }
}

void snd_set_power_saving(snd_mixer_t *m, bool saving)
{
    if (m != NULL) {
        m->power_saving = saving;
    }
}

void snd_set_speech_due(snd_mixer_t *m, bool due)
{
    if (m != NULL) {
        m->speech_due = due;
    }
}

bool snd_busy(const snd_mixer_t *m)
{
    return m != NULL &&
           (m->cont.pat != NULL || m->shot.pat != NULL || m->pending_n > 0u || m->speech);
}

bool snd_idle(const snd_mixer_t *m)
{
    return !snd_busy(m) && (m == NULL || !m->amp_on);
}

/* Anything cosmetic that was already playing or waiting when power saving came on. */
static void drop_cosmetic(snd_mixer_t *m)
{
    if (m->shot.pat == &P_KEY || m->shot.pat == &P_CHIME) {
        shot_kill(m);
    }
    uint8_t kept = 0;
    for (uint8_t i = 0; i < m->pending_n; ++i) {
        if (!is_cosmetic(m->pending[i])) {
            m->pending_ticket[kept] = m->pending_ticket[i];
            m->pending[kept++] = m->pending[i];
        } else {
            ticket_done(m, m->pending_ticket[i], SND_TICKET_CANCELED);
        }
    }
    m->pending_n = kept;
}

/* The one-shot has rendered its last sample: its pattern has ended, or is on its last step with
 * nothing left of it (the voice notices only at its next sample, which may be a block later). */
static bool shot_finished(const snd_voice_t *v)
{
    return v->pat == NULL ||
           (!v->pat->loop && v->step + 1u >= v->pat->count && v->pos >= v->len);
}

/* An entry pattern or the exit delay's fast beeps: speech over it is 6 dB quieter, so the beeps
 * stay in front. The exit beeps before the last 10 s wait for speech instead (AUD-034). */
static bool beeps_over_speech(uint8_t sound)
{
    return sound == ALARM_SOUND_ENTRY || sound == ALARM_SOUND_ENTRY_MEMORY ||
           sound == ALARM_SOUND_EXIT_FAST;
}

/* The voice's next sample would be the first of a beep (a tone step): where the exit pattern
 * waits for speech, so a beep is never cut and never starts under the words (AUD-034). */
static bool at_beep_start(const snd_voice_t *v)
{
    if (v->pos < v->len) {
        return v->pos == 0u && v->pat->steps[v->step].hz != 0u;
    }
    const uint8_t next = (uint8_t)(v->step + 1u < v->pat->count ? v->step + 1u : 0u);
    return v->pat->loop && v->pat->steps[next].hz != 0u;
}

snd_block_t snd_render(snd_mixer_t *m, int16_t *stereo, size_t frames)
{
    return snd_render_pcm(m, stereo, frames, NULL, 0u, false).block;
}

snd_pcm_block_t snd_render_pcm(snd_mixer_t *m, int16_t *stereo, size_t frames,
                               const int16_t *speech, size_t speech_avail, bool speech_active)
{
    snd_pcm_block_t res = {.block = {.amp = false, .silent = true},
                           .speech_consumed = 0u,
                           .speech_rejected = false};
    if (m == NULL || stereo == NULL || frames == 0u) {
        return res;
    }
    if (speech == NULL) {
        speech_avail = 0u;
    }
    if (m->power_saving) {
        drop_cosmetic(m);
    }
    const bool loud = m->cont.pat != NULL && m->cont.pat->loud;
    if (loud) {
        /* alarms drown everything else (AUD-005), speech included (AUD-010) */
        for (uint8_t i = 0; i < m->pending_n; ++i) {
            ticket_done(m, m->pending_ticket[i], SND_TICKET_CANCELED);
        }
        m->pending_n = 0;
        shot_kill(m);
        res.speech_rejected = speech_active || speech_avail > 0u;
        speech_active = false;
        speech_avail = 0u;
    }
    m->speech = speech_active || speech_avail > 0u;
    if (m->shot.pat == NULL && m->pending_n > 0u) {
        voice_start(&m->shot, pattern_for(m->pending[0]), m->rate);
        m->shot_ticket = m->pending_ticket[0];
        m->pending_n--;
        memmove(m->pending, m->pending + 1, m->pending_n);
        memmove(m->pending_ticket, m->pending_ticket + 1,
                m->pending_n * sizeof(m->pending_ticket[0]));
    }

    /* Waking up: let the amplifier settle on silence before any tone or word reaches it
     * (AUD-004); the speech waits for the next block. */
    if ((m->cont.pat != NULL || m->shot.pat != NULL || m->speech) && !m->amp_on) {
        m->amp_on = true;
        m->idle_ms = 0;
        memset(stereo, 0, frames * 2u * sizeof(stereo[0]));
        res.block.amp = true;
        return res;
    }

    const size_t take = speech_avail < frames ? speech_avail : frames;
    int32_t speech_gain = SPEECH_UNITY; /* the Voice volume is speech_boost's, before (AUD-033) */
    if (beeps_over_speech(m->cont_sound)) {
        speech_gain /= 2; /* -6 dB under the entry beeps and the exit delay's fast ones */
    }
    const uint32_t exit_wait_max = m->rate * SND_EXIT_WAIT_MAX_MS / 1000u;
    const float gain_user = (float)m->volume / 100.0f;        /* Beeps */
    const float gain_chime = (float)m->chime_volume / 100.0f; /* the chime alone (AUD-032) */
    bool sounding = false;
    for (size_t i = 0; i < frames; i++) {
        float s = 0.0f;
        float gain = gain_user;
        if (m->shot.pat != NULL) {
            /* Read before the sample: a one-shot ends as NULL in it. */
            const bool shot_chime = m->shot.pat == &P_CHIME;
            voice_sample(&m->shot, &s, m->rate, m->ramp); /* the continuous one pauses */
            gain = shot_chime ? gain_chime : gain_user;
        } else if (m->cont.pat != NULL) {
            /* Read before the sample: a pattern that doesn't loop ends as NULL in it (every
             * continuous one loops today; GCC's analyzer found the path, 2026-09-25). */
            const bool cont_loud = m->cont.pat->loud;
            const bool cont_chime = m->cont.pat == &P_CHIME;
            if ((m->speech || m->speech_due) && m->cont_sound == ALARM_SOUND_EXIT &&
                m->exit_held < exit_wait_max && at_beep_start(&m->cont)) {
                m->exit_held++; /* AUD-034: the exit beeps wait for the words, at most 10 s */
            } else {
                m->exit_held = 0u;
                voice_sample(&m->cont, &s, m->rate, m->ramp);
            }
            /* alarms ignore every volume (AUD-003) */
            gain = cont_loud ? 1.0f : (cont_chime ? gain_chime : gain_user);
        }
        int16_t v = (int16_t)(s * gain * FULL_SCALE);
        if (i < take) {
            /* The tone sample is final; the speech fills what is left of the int16 range. */
            int32_t mixed = (int32_t)v + (int32_t)speech[i] * speech_gain / SPEECH_UNITY;
            if (mixed > INT16_MAX) {
                mixed = INT16_MAX;
            } else if (mixed < INT16_MIN) {
                mixed = INT16_MIN;
            }
            v = (int16_t)mixed;
        }
        sounding = sounding || (v != 0);
        stereo[2u * i] = v;
        stereo[2u * i + 1u] = v;
    }
    res.speech_consumed = take;
    if (m->shot_ticket != 0u && shot_finished(&m->shot)) {
        ticket_done(m, m->shot_ticket, SND_TICKET_PLAYED);
        m->shot_ticket = 0u;
    }

    /* Stops counting once the amplifier has rested: a panel is quiet for months at a time, and
     * a counter that wrapped would switch it back on. */
    const uint32_t block_ms = (uint32_t)(frames * 1000u / m->rate);
    if (sounding || m->speech) { /* a pause inside an utterance is not the end of it */
        m->idle_ms = 0u;
    } else if (m->idle_ms < SND_AMP_IDLE_MS) {
        m->idle_ms += block_ms;
    }
    if (!snd_busy(m) && m->idle_ms >= SND_AMP_IDLE_MS) {
        m->amp_on = false;
    }
    res.block.amp = m->amp_on;
    res.block.silent = !sounding;
    return res;
}
