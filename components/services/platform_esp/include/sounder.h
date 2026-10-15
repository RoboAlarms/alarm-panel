/*
 * sounder.h - the panel's keypad sounder: an ESP-IDF task around the portable mixer.
 *
 * The sounds themselves - the patterns, their priorities and the synthesizer - live in
 * components/core/sounder (portable, host-tested, and what the PC simulator plays). This wrapper
 * owns one mixer, renders it from its own task and writes the blocks through a sink, so
 * callers on any task can ask for a sound without blocking.
 *
 * Patterns and priorities: docs/features/08-Audio-and-Annunciation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "esp_err.h"
#include "sounder/sounder_mix.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio output. write() blocks until the samples are queued (this paces the sounder). */
typedef struct {
    uint32_t sample_rate_hz; /* what the output plays (0: SND_SAMPLE_RATE); see snd_set_rate */
    void *ctx;
    esp_err_t (*write)(void *ctx, const int16_t *stereo_frames, size_t frames);
    void (*amp)(void *ctx, bool on); /* optional speaker amplifier control */
} sounder_sink_t;

esp_err_t sounder_start(const sounder_sink_t *sink);

/* Continuous pattern: ALARM_SOUND_NONE .. ALARM_SOUND_FIRE. */
void sounder_set_continuous(alarm_sound_t sound);

/* One-shot: ALARM_SOUND_ACK, _ERROR, _CHIME, _WALK_TEST, _WALK_TEST_WARNING. */
void sounder_play(alarm_sound_t sound);

/* A one-shot heard only over silence, that gives way to every other sound (snd_play_background:
 * the arming warning's chime, docs/features/41 D22). Safe from any task: posted, never waited
 * for; the "nothing else sounds" check is made on the sounder task, in the mixer. */
void sounder_play_background(alarm_sound_t sound);

/* ---- Tone tickets (docs/features/08 voice-first-build.md D04, D11) ---------------------------
 * sounder_play() that says what became of the sound, so speech that follows a tone (a chime's
 * zone name) starts only after the tone's final block has gone to the output. Returns a nonzero
 * ticket once the request is queued, 0 when refused here (not a one-shot, the chime while saving
 * power, the sounder not started or its queue full: nothing will follow). Every nonzero ticket
 * later yields exactly one outcome through sounder_ticket_next(): SND_TICKET_PLAYED after the
 * block holding the tone's last sample was written to the output, SND_TICKET_CANCELED if the
 * mixer refused or dropped it (alarm, fire, CO or aux; power saving; its queue full). */
typedef snd_ticket_t sounder_ticket_t;
#define SOUNDER_TICKET_RING 16u
uint32_t sounder_play_tracked(alarm_sound_t sound);
/* The oldest outcome not yet read; false when there is none. One reader (panel_voice), any
 * task; it only copies. Outcomes nobody reads wait in the mixer's ring (SND_TICKET_RING more),
 * whose oldest are lost after that. */
bool sounder_ticket_next(sounder_ticket_t *out);

/* ---- Speech (docs/features/08 voice-first-build.md D11; AUD-010, AUD-028) -------------------
 * One producer task (panel_voice) hands mono PCM16 at the output's rate (sounder_speech_status:
 * rate_hz) to the sounder task, which mixes it over the tones (snd_render_pcm) in the same
 * output stream; nothing else writes voice to the speakers. The buffer is SOUNDER_SPEECH_SLOTS
 * slots of up to SOUNDER_SPEECH_SLOT_SAMPLES samples (120 ms at 32 kHz), allocated once at
 * start; each write takes whole slots, so write in multiples of a slot.
 *
 * An utterance is a generation: a nonzero number, larger for each new utterance. Its samples
 * start playing once SOUNDER_SPEECH_PREFETCH_MS are buffered or it is finished, and from then on
 * must keep up: a block that finds too few samples of an unfinished utterance ends it (an
 * underrun - nothing is repeated or waited for). Alarm, fire, CO and aux reject it, and a
 * rejected, underrun, canceled or finished generation accepts no more samples. The bench silence
 * (AUD-025) applies to the mixed block: speech is consumed and reported as usual, unheard. */
#define SOUNDER_SPEECH_SLOTS        12u
#define SOUNDER_SPEECH_SLOT_SAMPLES SND_BLOCK_FRAMES
#define SOUNDER_SPEECH_PREFETCH_MS  40u

typedef struct {
    bool ready;            /* speech can be heard: the sounder task runs with its speech ring.
                            * false before sounder_start, when it failed, and when the ring could
                            * not be allocated ("no memory for the speech buffer": tones only);
                            * write only while it is true */
    uint32_t rate_hz;      /* the rate to write at: the output's; 0 until the task runs */
    size_t free_samples;   /* free slots x SOUNDER_SPEECH_SLOT_SAMPLES; 0 when not started */
    uint32_t consumed;     /* speech samples mixed, every generation (wraps) */
    uint32_t done_gen;     /* the newest generation finished and played to its last sample */
    uint32_t rejected_gen; /* the newest generation refused by alarm, fire, CO or aux */
    uint32_t underrun_gen; /* the newest generation ended by an underrun */
    uint32_t rejected;     /* how many were refused (wraps) */
    uint32_t underruns;    /* how many ran dry (wraps) */
} sounder_speech_status_t;

/* Queue up to n samples of generation 'gen'; returns how many were taken (0 when gen is 0,
 * canceled, finished, rejected or ran dry, when the buffer is full or the sounder not started).
 * Wakes the sounder task. Producer task only. */
size_t sounder_speech_write(const int16_t *pcm, size_t n, uint32_t gen);
/* No more samples for 'gen' (and older ones): what is buffered plays out, then done_gen = gen. */
void sounder_speech_finish(uint32_t gen);
/* Drop 'gen' and every older generation: what is buffered is discarded within the next block. */
void sounder_speech_cancel(uint32_t gen);
/* Any task. */
void sounder_speech_status(sounder_speech_status_t *out);
/* Speech is on its way (panel_voice: an utterance queued or playing, ann_due()): the exit beeps
 * wait for it at a beep's start (AUD-034). The latest value wins, read every block. Any task. */
void sounder_speech_due(bool due);

/* Touch feedback click. */
void sounder_key_click(void);
/* Suppress cosmetic key/chime sounds; preserve all alarm and code feedback. */
void sounder_set_power_saving(bool saving);

/* The everyday volumes, each 0..100 and on its own (docs/features/08 AUD-032); alarm, fire, CO
 * and aux are always full. Beeps: every keypad sound but the chime and speech. */
void sounder_set_volume(uint8_t percent);
/* The door chime. */
void sounder_set_chime_volume(uint8_t percent);
/* Speech: announcements, Preview and the audio bench. */
void sounder_set_voice_volume(uint8_t percent);

/* Bench silence, development builds only (CONFIG_ALARM_BUILD_DEV; docs/features/08 AUD-025):
 * this panel's speaker amplifier stays off and silence is written, alarms included, while the
 * mixer decides every sound as usual. Paired keypads and sirens are not covered: they still
 * sound. Kept in NVS ("sounder"/"silent") across restarts and dev updates; a factory reset, or
 * an image of another profile, brings the sound back. Said at start, by the console's status,
 * `speaker` and /dev/v1/info. Other builds have no such switch: sounder_silenced() is false
 * there and sounder_set_silenced() answers ESP_ERR_NOT_SUPPORTED. */
bool sounder_silenced(void);
/* Writes NVS: from a task whose stack is in internal RAM (the console, the developer link). Off
 * (silent) takes effect at once even when it can't be kept (the error says so); on only once
 * it is kept. */
esp_err_t sounder_set_silenced(bool silent);

#ifdef __cplusplus
}
#endif
