/*
 * sim_audio.h - the panel's sounder, out of the PC's speakers.
 *
 * The simulator renders the same components/core/sounder mixer the panel does, so the exit beeps,
 * the chime, the trouble chirps and the temporal-3 fire pattern can be heard at the desk
 * instead of guessed at. Only the level differs: the panel plays alarms at full scale on a
 * wall, which is unkind through headphones, so the simulator trims everything it plays by the
 * same amount (see SIM_AUDIO_TRIM_PCT). Patterns, timings and relative loudness are the
 * panel's exactly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

#include "alarm/alarm_types.h"
#include "voice_render.h"

/* How loud the simulator plays, against the panel's own level. The patterns and the balance
 * between them are untouched: a key click is still far quieter than a burglary alarm. */
#define SIM_AUDIO_TRIM_PCT 50

/* Open the sound card and start playing. false: no sound card, and the simulator runs on
 * silently - a PC without audio must not stop anybody testing the screen. */
bool sim_audio_start(void);
void sim_audio_stop(void);

/* Wired to host_panel_sound() in sim_main.c: the ctx argument is unused. */
void sim_audio_continuous(void *ctx, alarm_sound_t sound);
void sim_audio_one_shot(void *ctx, alarm_sound_t sound);
void sim_audio_key_click(void *ctx);
/* The three everyday volumes (docs/features/08 AUD-032): Beeps, the chime, the voice. */
void sim_audio_volume(void *ctx, uint8_t percent);
void sim_audio_chime_volume(void *ctx, uint8_t percent);
void sim_audio_voice_volume(void *ctx, uint8_t percent);
/* On battery (power_saving_update): the mixer drops key clicks and the chime. */
void sim_audio_power_saving(void *ctx, bool saving);

/* The mute button beside the screen. Mute stops the sound reaching the speakers; the panel
 * carries on sounding underneath, so unmuting drops back into the pattern where it is. */
void sim_audio_mute(bool mute);
bool sim_audio_muted(void);

/* The continuous pattern playing now, for the simulator's status line. */
alarm_sound_t sim_audio_pattern(void);

/* ---- The voice (docs/features/08 voice-first-build.md D11, D15) ----
 * Tones go through the mixer's tracked one-shots and speech is rendered with voice_render into
 * a small ring that the audio thread mixes with snd_render_pcm - the panel's mixer: speech -6 dB
 * under the entry beeps and the fast exit beeps, and the slow exit beeps waiting while speech is
 * due or playing (AUD-034). Wired to host_panel_sound() in sim_main.c; the ctx argument is unused.
 * Mute and volume apply to speech as to tones. */

/* A tone with the policy's ticket (snd_play_tracked); false: not queued (its outcome is
 * CANCELED, read back with sim_audio_ticket). */
bool sim_audio_tone(void *ctx, alarm_sound_t sound, uint32_t ticket);
/* The next tracked tone's end: its ticket and whether it played to its last block. */
bool sim_audio_ticket(void *ctx, uint32_t *ticket, bool *played);
/* Starts utterance `id` (ann_utt_t.id) over `items`; the pack must stay as it is until the
 * utterance ends. false: it could not start. Replaces an utterance still playing. */
bool sim_audio_speak(void *ctx, const vp_pack_t *pack, const vr_item_t *items, size_t n,
                     uint32_t id);
void sim_audio_speech_cancel(void *ctx);
/* Speech is queued or playing (ann_due): the exit beeps wait for it (AUD-034). */
void sim_audio_speech_due(void *ctx, bool due);
/* The end of an utterance: played to its last sample (completed) or cancelled. */
bool sim_audio_speech_done(void *ctx, uint32_t *id, bool *completed);

/* The mixer without a sound card, driven by the caller (the preview proves the simulator's
 * speech this way on any host), and one render of `blocks` 10 ms stereo blocks, trimmed and
 * muted as the card's would be. */
bool sim_audio_start_offline(void);
void sim_audio_render(int16_t *stereo, unsigned blocks);
