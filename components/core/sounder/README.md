# sounder

**Tier:** core (portable C11) · **Features:** [08 Audio and annunciation](../../../docs/features/08-Audio-and-Annunciation/README.md) ·
**Tests:** [`test/`](test/) (2 suites: `sounder`, `speech_boost`)

The keypad sounder's pattern tables, sine synthesizer and mixer: the single copy of what the
panel sounds like. The caller owns a `snd_mixer_t`, sets the continuous pattern from the keypad
partition's status (exit, entry, trouble, aux, alarm, fire temporal-3, CO temporal-4), queues
one-shots (key click, ack, error, chime, walk test) and asks `snd_render()` for the next block
of stereo frames. The panel's sounder task (`components/services/platform_esp/src/sounder.c`) writes
those blocks to the I2S speakers, the PC simulator (`host/ui_sim/sim_audio.c`) to the sound
card, and the tests to a buffer, so a pattern heard at the desk is the pattern on the wall. It
owns no audio device, task or queue, and it does not choose between continuous patterns: the
engine's `status.sounder` already holds the most important one.

A background one-shot (`snd_play_background()`: the arming warning's chime, docs/features/41 D22)
starts only while nothing plays or waits and is cut off in the same call that asks for any other
sound - a continuous pattern, a one-shot or a key click - so it never plays over, interrupts or
delays an alarm, fire, CO, entry, exit or trouble sound. It is cosmetic (power saving drops it).

## Speech and tone tickets
Voice speech (docs/features/08 voice-first-build.md D04, D11) reaches the speakers through this
mixer only. `snd_render_pcm()` renders the tones exactly as `snd_render()` does (which is
`snd_render_pcm()` with no speech) and adds mono speech on top at unity (the Voice volume is
applied before the mixer, by `speech_boost`), 6 dB lower under the entry beeps and the exit
delay's fast ones, saturating at the int16 limits so only the speech gives way. The exit beeps
before the last 10 s wait for speech instead (AUD-034): a beep sounding finishes, the next one
waits at its start until the words end, `SND_EXIT_WAIT_MAX_MS` (10 s) at most. They wait too while
speech is due (`snd_set_speech_due()`, set by the caller each block: an utterance queued or
playing), so the first exit beep after the arming acknowledgement waits for the arming words that
follow it. It reports the samples it consumed: none in the amplifier's silent warm-up block, none over
alarm, fire, CO or aux, which reject it (`speech_rejected`). An utterance in progress counts as
sound for `snd_busy()`, `snd_idle()` and the amplifier's rest.

`snd_play_tracked()` is `snd_play()` with a ticket carried through the one-shot queue: every
nonzero ticket gets exactly one `{ticket, SND_TICKET_PLAYED | SND_TICKET_CANCELED}` in a
16-entry ring read with `snd_ticket_next()`. PLAYED comes with the block that holds the tone's
last sample, so a chime's spoken name started on it never overlaps the chime; CANCELED when the
sound is refused or dropped (an alarm, power saving, a full queue, a rate change). The panel's
sounder task (`components/services/platform_esp/src/sounder.c`) feeds the speech from a ring
panel_voice fills and passes the outcomes on; the bench silence (AUD-025) applies after the mix.

## The Voice volume on speech (AUD-033)
`speech_boost.h` applies the Voice volume to speech, before the mixer, and can make it louder than
the pack: the packs hold clear speech (each word levelled, nothing compressed or limited) and
every 10 % of the volume is 3 dB (`sb_gain()`: silent at 0 %, -6 dB at 50 %, the pack's own level
at `SB_UNITY_VOLUME`, 70 %, the default, and +9 dB of gain at 100 %). The words already peak near
-1 dBFS, so above 70 % the limiter takes back part of the gain: on pack revision 7, +9 dB is heard
as about +4.7 dB (+2.3 dB at 80 %, +3.9 dB at 90 %; measured 2026-10-10). The
speech's producer (panel_voice's worker and its audio bench, the simulator, the preview) runs each
utterance's 32 kHz samples through it: `sb_begin(sb, voice_volume)` at the start fixes the gain,
`sb_process()` changes the samples in place, and once the renderer has nothing left `sb_flush()`
gives back the 63 samples the look-ahead held. Where the gain lifts a peak over -1 dBFS, a limiter
with 2 ms of look-ahead holds it there and lets go over about 60 ms; at a gain that cannot (the
volume up to 66 %) the stage only scales the samples, with no delay, and `sb_flush()` gives
nothing back. The mixer then adds speech at unity (6 dB lower under the entry beeps and the exit
delay's fast ones).
The state (`speech_boost_t`, under 1 KiB) is the caller's; floats, no heap.

## Public API
| Header | What it offers |
|---|---|
| `include/sounder/speech_boost.h` | `speech_boost_t`; `sb_gain()`, `sb_begin()`, `sb_process()`, `sb_flush()`; `SB_LOOKAHEAD` (64), `SB_DELAY` (63), `SB_BOOST_DB` (+9 dB), `SB_CEILING_DB` (-1 dBFS) |
| `include/sounder/sounder_mix.h` | `snd_mixer_t`, `snd_block_t`; `snd_init()`, `snd_set_continuous()`, `snd_play()`, `snd_play_background()`, `snd_key_click()`, `snd_set_volume()`, `snd_set_chime_volume()`, `snd_set_voice_volume()`, `snd_set_power_saving()`, `snd_busy()`, `snd_idle()`, `snd_render()`; speech and tickets: `snd_pcm_block_t`, `snd_ticket_t`, `snd_render_pcm()`, `snd_play_tracked()`, `snd_ticket_next()`; `SND_SAMPLE_RATE` (32 kHz), `SND_BLOCK_FRAMES` (320, 10 ms), `SND_AMP_IDLE_MS` (1.5 s), `SND_KEY_CLICK` |

## Depends on
- Project components: `alarm_core` (`alarm_sound_t`)
- ESP-IDF: none (portable); the host build links `libm` on POSIX

## Options
None. The rate is fixed at 32 kHz; a rate per board is planned in docs/features/33 step 8.

## Rate
`snd_init()` renders at 32 kHz; `snd_set_rate()` picks the board's output rate (8-32 kHz in
whole hundreds of Hz, so a 10 ms block, `snd_block_frames()`, is a whole number of frames).
Steps, ramps and the amplifier's idle timer are counted in time, so every pattern sounds the
same at every rate; at 32 kHz the output is bit-identical to what it always was (AUD-006,
docs/features/33 step 8).

## Rules
- The pattern table in `src/sounder_mix.c` is the sound of the panel. The table "Pattern
  specification (as built)" in docs/features/08 is the same table in words, and `test/test_sounder.c`
  measures it: change one, change the other two in the same commit (CONTRIBUTING.md).
- Fire is temporal-3 (NFPA 72) and CO temporal-4 (NFPA 720). These timings are standards, not
  preferences.
- Alarm, aux, fire and CO play at full volume whatever the volume settings and drop one-shots.
- Three everyday volumes, each 0..100 (default 70) and on its own (AUD-032): `snd_set_volume()`
  is Beeps (every tone but the chime), `snd_set_chime_volume()` the chime (one-shot or
  background), `snd_set_voice_volume()` speech.
  Power saving drops only key clicks and the chime (docs/features/17 PWR-005).
- The first block after silence is always silent, so the amplifier is on before any tone
  reaches it (AUD-004). `SND_KEY_CLICK` is not an `alarm_sound_t`, whose values are an external
  contract (MQTT, event log).
- Portable rules (CONTRIBUTING.md): C11, no heap, no globals, no RTOS or ESP-IDF headers; setters from
  one thread; warning-free host build and test (`STRICT`).
- Hear a change in the simulator (`pwsh tools/ui_sim.ps1`) before asking for a panel.

## Tests
Run all with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite alone with `ctest --test-dir build/host -R sounder`. The tests listen to the
rendered PCM and measure beeps and gaps.
- Fire temporal-3 and CO temporal-4 timings (AUD-002).
- Every continuous sound plays; a new pattern starts from its beginning, the same one is not
  restarted (AUD-001).
- Alarms ignore the volume; the volume moves everything else and is clamped (AUD-003).
- The three volumes (AUD-032): default 70 and clamp; the chime follows its own volume alone (one-shot
  and background), neither the beeps nor the chime volume touches speech in the mixer, a key click and the trouble beeps the beeps volume
  alone; alarms ignore all three.
- One-shots pause and resume a quiet pattern, are dropped under alarms, queue and then drop; the
  walk test is three beeps and the chime two notes (AUD-005).
- The silent first block, and the amplifier resting after the sound but not in a pattern's gap
  (AUD-004); power saving drops the cosmetic sounds and never touches an alarm. Levels and the
  pop by ear on the board are still open (docs/features/08).
- Speech in the mix (AUD-010, AUD-028): with none, `snd_render_pcm()` is `snd_render()` bit for
  bit; speech rides on the tones without moving their samples, phase or timing, joins at
  unity whatever the voice volume (-6 dB under the entry beeps and the exit delay's fast ones),
  saturates, is rejected by alarm, fire, CO and aux, waits out the warm-up block and keeps the
  amplifier on.
- The exit beeps wait for speech (AUD-034): the beep sounding finishes sample for sample, the next
  one sounds as the words end and the beeps then keep their 1 s rhythm; 10 s of waiting at most;
  the fast beeps of the last 10 s don't wait. The first exit beep waits for arming words that come
  1-4 blocks after the acknowledgement's PLAYED (speech due); words still filling hold the beeps;
  speech due alone holds each 10 s at most, and a new exit delay waits its full 10 s; a one-shot
  sounds while they wait. Speech due holds nothing else: every other pattern and the one-shots
  render sample for sample as with nothing due, and an alarm taking over from held exit beeps
  sounds in its first block.
- Tone tickets (AUD-028, AUD-005): PLAYED with the tone's last block at every rate, in turn;
  CANCELED by an alarm, power saving, a full queue, a rate change or a sound that is no
  one-shot; tracking changes no tone; the ring keeps the newest 16.
- The Voice volume on speech (AUD-033, `test/test_speech_boost.c`): the gain at each volume, 3 dB
  for every 10 %; up to 66 % only scaled, sample for sample, nothing held, and limited from 67 %; speech under the ceiling only louder; the
  look-ahead given back exactly; pulls of any size give the same samples; the ceiling held on a
  full-scale burst at every volume; the limiter letting go after a peak.
