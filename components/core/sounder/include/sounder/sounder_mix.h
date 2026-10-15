/*
 * sounder_mix.h - the keypad sounder's patterns and mixer, as portable C11.
 *
 * Turns the alarm_core sounds into PCM: one continuous pattern (the keypad partition's
 * status "sounder": exit, entry, trouble, alarm, fire T3, CO T4) plus one-shot sounds (key
 * click, ack, error, chime, walk test) that briefly interrupt the quieter patterns. Alarm,
 * fire, CO and aux always play at full volume and suppress one-shots.
 *
 * No heap, no RTOS, no audio device: the caller owns a snd_mixer_t, calls the setters from
 * one thread and asks snd_render() for the next block of frames, which it writes wherever it
 * likes - the panel's I2S speakers (components/services/platform_esp/src/sounder.c), the PC simulator's
 * sound card (host/ui_sim/sim_audio.c) or a test's buffer (components/core/sounder/test/test_sounder.c).
 *
 * The functions are prefixed snd_ so they cannot collide with the panel's own sounder_*
 * wrapper, which posts to a queue and renders from its task.
 *
 * Patterns, timings and priorities: docs/features/08-Audio-and-Annunciation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SND_SAMPLE_RATE   32000u /* the default and highest rate; mono, on both channels */
#define SND_RATE_MIN      8000u  /* the lowest rate a board may ask for (snd_set_rate) */
#define SND_BLOCK_FRAMES  320u   /* 10 ms at the default rate: the largest block to render */
#define SND_AMP_IDLE_MS   1500u  /* silence after which the speaker amplifier may rest */
#define SND_ONESHOT_QUEUE 4u     /* one-shots waiting to play */
#define SND_TICKET_RING   16u    /* outcomes of tracked one-shots waiting to be read */

/* Touch feedback. Not an alarm_sound_t: the engine has no opinion about key clicks, and
 * alarm_sound_t values are an external contract (MQTT, the event log). */
#define SND_KEY_CLICK 0xFFu

struct snd_pattern; /* the private pattern tables in sounder_mix.c */

/* Where a voice is inside its pattern. Private; here so the mixer needs no heap. */
typedef struct {
    const struct snd_pattern *pat; /* NULL = this voice is silent */
    uint32_t pos;                  /* samples into the step */
    uint32_t len;                  /* samples in the step */
    float phase;                   /* sine phase, 0..2pi */
    float decay;                   /* ENV_DECAY level, 1 down to about 0.05 */
    float decay_mul;
    uint8_t step;
} snd_voice_t;

/* What became of a tracked one-shot (snd_play_tracked). */
typedef enum {
    SND_TICKET_PLAYED = 1,   /* the tone's final block has been rendered */
    SND_TICKET_CANCELED = 2, /* never queued, or dropped before its end */
} snd_ticket_outcome_t;

typedef struct {
    uint32_t ticket;
    uint8_t outcome; /* snd_ticket_outcome_t */
} snd_ticket_t;

typedef struct {
    snd_voice_t cont; /* the continuous pattern */
    snd_voice_t shot; /* the one-shot playing over it */
    uint8_t cont_sound;
    uint8_t pending[SND_ONESHOT_QUEUE];
    uint32_t pending_ticket[SND_ONESHOT_QUEUE]; /* 0 = untracked */
    uint32_t shot_ticket;                       /* the one-shot playing; 0 = untracked */
    snd_ticket_t done[SND_TICKET_RING];         /* outcomes, oldest at done_head */
    uint8_t done_head;
    uint8_t done_n;
    uint32_t done_lost; /* outcomes dropped because nobody read the ring (oldest go first) */
    uint8_t pending_n;
    uint8_t volume;       /* 0..100, Beeps: every everyday sound but the chime and speech */
    uint8_t chime_volume; /* 0..100, the door chime alone (AUD-032) */
    uint8_t voice_volume; /* 0..100, speech alone (AUD-032) */
    uint32_t rate;     /* samples a second (snd_set_rate) */
    uint32_t ramp;     /* samples in a 3 ms attack or release at that rate */
    uint32_t idle_ms;  /* silence rendered since the last sound */
    bool power_saving; /* drop the cosmetic sounds: key clicks and chime */
    bool amp_on;
    bool background;   /* the one-shot playing came from snd_play_background: it gives way */
    bool speech;       /* the last block was given speech to play (snd_render_pcm) */
    bool speech_due;   /* speech is on its way (snd_set_speech_due): the exit beeps wait for it */
    uint32_t exit_held; /* samples the exit beeps have waited for speech, in a row (AUD-034) */
} snd_mixer_t;

/* The longest the exit beeps wait for speech (AUD-034): past it one beep sounds over the words. */
#define SND_EXIT_WAIT_MAX_MS 10000u

/* What the caller should do with the block snd_render() just filled. */
typedef struct {
    bool amp;    /* the speaker amplifier should be powered while this block plays */
    bool silent; /* every sample in the block is zero */
} snd_block_t;

/* What snd_render_pcm() did with the block, and with the speech it was offered. */
typedef struct {
    snd_block_t block;
    size_t speech_consumed; /* speech samples mixed in: the caller drops these from its buffer */
    bool speech_rejected;   /* speech was offered over alarm, fire, CO or aux and refused */
} snd_pcm_block_t;

/* Reset to silence at the default volumes (beeps, chime and voice 70 %) and rate
 * (SND_SAMPLE_RATE). */
void snd_init(snd_mixer_t *m);

/* The rate the board's output plays, SND_RATE_MIN..SND_SAMPLE_RATE in whole hundreds of Hz so
 * a 10 ms block is a whole number of frames (8, 12, 16, 24 or 32 kHz; docs/features/33 step 8): every
 * pattern keeps its timing and its tones at any of them. Call before the first sound; a change
 * while a pattern plays restarts it. Returns false (rate unchanged) for any other rate. */
bool snd_set_rate(snd_mixer_t *m, uint32_t hz);

/* The rate, and the frames in a 10 ms block at it (at most SND_BLOCK_FRAMES). */
uint32_t snd_rate(const snd_mixer_t *m);
size_t snd_block_frames(const snd_mixer_t *m);

/* The continuous pattern, from the keypad partition's status: ALARM_SOUND_NONE..FIRE.
 * A different sound restarts the pattern from its beginning; the same sound is ignored, so
 * repeating the current status does not stutter it. */
void snd_set_continuous(snd_mixer_t *m, alarm_sound_t sound);

/* Queue a one-shot: ALARM_SOUND_ACK, _ERROR, _CHIME, _WALK_TEST. Ignored while a loud
 * pattern plays, when the queue is full, or - for the chime - while saving power. */
void snd_play(snd_mixer_t *m, alarm_sound_t sound);

/* snd_play() that reports what became of the sound: exactly one snd_ticket_t {ticket, PLAYED or
 * CANCELED} reaches the outcome ring for every nonzero ticket (snd_ticket_next). PLAYED once the
 * block holding the tone's last sample has been rendered; CANCELED when it is refused (not a
 * one-shot, the queue is full, a cosmetic sound while saving power) or dropped before its end
 * (alarm, fire, CO or aux; power saving; a rate change). Returns whether it was queued.
 * Ticket 0 is untracked, as snd_play(). Voice speech that follows a tone waits for its PLAYED
 * (docs/features/08 voice-first-build.md D04, D11). */
bool snd_play_tracked(snd_mixer_t *m, alarm_sound_t sound, uint32_t ticket);

/* The oldest unread outcome; false when there is none. Read it at least every
 * SND_TICKET_RING one-shots: past that the oldest is lost (counted in done_lost). */
bool snd_ticket_next(snd_mixer_t *m, snd_ticket_t *out);

/* The block just rendered never reached the speaker (its write failed): no tone in it may count
 * as PLAYED. 'n_before' and 'lost_before' are done_n and done_lost read just before that render;
 * the outcomes it appended are the newest done_n + (done_lost - lost_before) - n_before (at most
 * done_n of them still in the ring), and each PLAYED among them becomes CANCELED. Older outcomes
 * are left alone (AUD-028, voice-first-build.md D11). */
void snd_tickets_cancel_newest(snd_mixer_t *m, uint32_t lost_before, uint8_t n_before);

/* With snd_tickets_cancel_newest() after a failed write: the one-shot still playing has lost a
 * block, so its ticket is CANCELED now and it never reports PLAYED. Only the ticket goes: the
 * tone plays on exactly as before. No-op when nothing tracked is playing. */
void snd_ticket_abandon_active(snd_mixer_t *m);

/* Free speech samples in a ring of 'slots' slots of 'slot_samples' each, whose head and tail
 * cursors run 0..2*slots-1 (full and empty differ). Cursors read separately from another task
 * may be inconsistent (used > slots): that reports 0, never a wrapped huge capacity (AUD-028). */
size_t snd_speech_free(uint32_t head, uint32_t tail, uint32_t slots, size_t slot_samples);

/* A one-shot heard only over silence, that gives way to everything (the arming warning's chime,
 * docs/features/41 DESIGN.md section 3 "Arming" item 2, D22): it starts only while nothing plays
 * or waits (no continuous pattern, no one-shot) and is cut off in the same call that asks for
 * any other sound - a continuous pattern (alarm, fire, CO, entry, exit, trouble), a one-shot or a
 * key click - so it never plays over, interrupts or delays one. Cosmetic like the chime (power
 * saving drops it). Returns whether it started. */
bool snd_play_background(snd_mixer_t *m, alarm_sound_t sound);

/* Queue touch feedback. Dropped while saving power, and when two clicks already wait. */
void snd_key_click(snd_mixer_t *m);

/* The three everyday volumes (AUD-032), each 0..100 (clamped) and on its own - none scales
 * another. Alarm, fire, CO and aux ignore all three and always play at full volume (AUD-003).
 * snd_set_volume: Beeps - entry and exit, trouble, the walk test, key clicks and every other
 *   everyday tone but the chime.
 * snd_set_chime_volume: the door chime (P_CHIME), as a one-shot or from snd_play_background.
 * snd_set_voice_volume: speech. The mixer only keeps it (voice_volume) for the speech's renderer
 *   to read: the volume is applied before the mixer, by speech_boost.h, which can make speech
 *   louder than the pack at the top (AUD-033); snd_render_pcm adds speech at unity. */
void snd_set_volume(snd_mixer_t *m, uint8_t percent);
void snd_set_chime_volume(snd_mixer_t *m, uint8_t percent);
void snd_set_voice_volume(snd_mixer_t *m, uint8_t percent);

/* On battery: drop key clicks and the chime, keep every alarm and code sound (docs/features/17). */
void snd_set_power_saving(snd_mixer_t *m, bool saving);

/* Speech is on its way: an utterance is queued (waiting, perhaps, for the tone it follows) or
 * playing. While it is, the exit beeps wait at a beep's start as they do under speech (AUD-034),
 * so the words that follow the arming tone come first and the beeps after them. It changes
 * nothing else: no gain, no other pattern, not snd_busy() or snd_idle(). The
 * caller (the speech's producer) sets it every block; SND_EXIT_WAIT_MAX_MS bounds a stuck one. */
void snd_set_speech_due(snd_mixer_t *m, bool due);

/* Something is playing or waiting to play (speech given to the last block included). */
bool snd_busy(const snd_mixer_t *m);

/* Nothing to play and the amplifier is off: the caller may sleep until a setter is called. */
bool snd_idle(const snd_mixer_t *m);

/* Fill 'frames' stereo frames (interleaved left, right - the two speakers get the same
 * signal) and report what to do with them. 'stereo' holds 2 * frames samples.
 *
 * The first block after silence is always silent, so the amplifier is powered before any
 * tone reaches it and never pops (AUD-004). */
snd_block_t snd_render(snd_mixer_t *m, int16_t *stereo, size_t frames);

/* snd_render() with speech mixed in (docs/features/08 voice-first-build.md D11): 'speech' holds
 * 'speech_avail' mono samples at snd_rate(), 'speech_active' says the caller has an utterance to
 * play. The tones render exactly as snd_render() renders them - gain, phase, timing - and speech
 * is added on top as it comes, at the Voice volume speech_boost gave it (-6 dB while an entry
 * pattern or the exit delay's fast beeps are set), clamped to the int16 range so only the speech
 * gives way. One exception (AUD-034): while there is speech, or speech is due
 * (snd_set_speech_due), the exit beeps (ALARM_SOUND_EXIT, not the last 10 s) wait - a beep
 * already sounding finishes, the pattern stops at the start of its next beep and sounds it once
 * the speech is over, or after SND_EXIT_WAIT_MAX_MS of waiting, and speech over them is at
 * unity. At most 'frames' samples are consumed, none:
 * - in the amplifier's silent warm-up block (speech wakes the amplifier like a tone, AUD-004),
 * - over alarm, fire, CO or aux, which reject it (speech_rejected; AUD-010).
 * Speech counts as sound for snd_busy(), snd_idle() and the amplifier's rest. With no speech
 * (NULL, 0, false) this is snd_render(), sample for sample (AUD-010, AUD-028). */
snd_pcm_block_t snd_render_pcm(snd_mixer_t *m, int16_t *stereo, size_t frames,
                               const int16_t *speech, size_t speech_avail, bool speech_active);

#ifdef __cplusplus
}
#endif
