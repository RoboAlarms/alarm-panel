# annunciator

**Tier:** core (portable C11) · **Features:** [08 Audio and annunciation](../../../docs/features/08-Audio-and-Annunciation/README.md), [40 Walk test](../../../docs/features/40-Walk-Test/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `annunciator`)

What the panel says, and when: the voice's request policy (docs/features/08
[voice-first-build.md](../../../docs/features/08-Audio-and-Annunciation/voice-first-build.md) D04
"The chime", D05 "What the panel says, and when", D06 "Settings", D11 "Playback"; WALK-016). It
turns the engine's events, one-shot tones and snapshots into utterances - up to eight clip IDs with
the silence before each (the pack's word gap, 20 ms, between the words of a phrase - a zone's
words, "Armed" "Away" - and its part gap, 300 ms, before a new sentence), never
over 10 s - and decides which one plays: one active and four waiting, by priority (entry > state
and walk control > refusal and walk trip > chime > Preview), with coalescing, expiry (2 s / 5 s),
supersession and cancellation by generation. It decodes nothing and owns no audio: the voice
worker decodes and the sounder mixes.

How the platform drives it, on the one thread that owns it:
1. `ann_init()`, then `ann_set_prefs()` (the four switches, the user volume, power saving) and
   `ann_set_pack()` (ready, revision, gaps) whenever they change.
2. For each engine call: `ann_event()` for its events, `ann_sound()` for its one-shots with the
   ticket `sounder_play_tracked()` returned (0 when the tone wasn't admitted), `ann_output()` for the
   bell, then `ann_commit()` with `ann_state_from_snapshot()` of the snapshot after the call. The
   first commit (and the first after `ann_rebaseline()`) is a silent baseline. A panel-link
   keypad speaks for its home partition, which its master may move: `ann_set_keypad()` sets it
   and rebaselines (docs/features/11 LINK-028); with `ann_join_exit()` on (a link keypad), a
   baseline taken in an exit time joins that arming, so its end says the armed mode as the
   master's voice does; an alarm panel leaves it off (a restored exit time is no known cycle).
3. `ann_ticket()` for each tracked tone's PLAYED / CANCELED, `ann_bell_test_done()` when the bell
   test pulse has ended.
4. The worker: `ann_next()` for the next utterance, `ann_current()` (or `ann_generation()` against
   the utterance's generation) at each block, `ann_done()` when it ends, and `ann_due()` for the
   sounder each step (speech queued or playing: the exit beeps wait for it, AUD-034).
   `ann_preview()` for the Preview buttons.

Zone names come through `ann_ports_t.zone_words` as the configuration stores them (token IDs and
the binding, approval and pack revisions); only a name approved against the zone's current binding
and the active pack speaks. Clip lengths come through `ann_ports_t.clip_samples`
(`ann_pack_clip_samples()` reads them from a `vp_pack_t`).

## Public API
| Header | What it offers |
|---|---|
| `include/annunciator/annunciator.h` | `ann_init()`, `ann_rebaseline()`, `ann_set_keypad()`, `ann_join_exit()`, `ann_set_prefs()`, `ann_set_pack()`, `ann_event()`, `ann_sound()`, `ann_output()`, `ann_commit()`, `ann_ticket()`, `ann_bell_test_done()`, `ann_preview()`, `ann_next()`, `ann_current()`, `ann_done()`, `ann_cancel_all()`, `ann_generation()`, `ann_waiting()`, `ann_due()`, `ann_priority()`, `ann_state_from_snapshot()`, `ann_pack_clip_samples()`; `ann_t`, `ann_utt_t`, `ann_ports_t`, `ann_prefs_t`, `ann_pack_t`, `ann_state_t`, `ann_zone_words_t`, the limits (`ANN_*`) |

## Depends on
- Project components: `alarm_core` (event, sound and snapshot types), `voice_pack` (clip IDs, `vp_find`)
- ESP-IDF: none (portable)

## Options
None.

## Rules
- Speech never changes a tone's samples: the engine's tones play as before, except the exit beeps
  before the last 10 s, which the sounder holds at a beep's start while speech is due or playing
  (`ann_due()`, AUD-034, 10 s at most). Speech that follows a tone starts only after that tone's
  PLAYED; a canceled or unadmitted tone takes its speech with it.
- Nothing a person can't otherwise hear changes what is said: duress and silent or hidden events are
  ignored; an audible fire, CO, burglary or aux pattern on any partition cancels all speech.
- Only the keypad's partition (1) speaks; no speech for state the panel only restored.
- Portable rules (components/core/AGENTS.md): C11, no heap, no globals; warning-free (`STRICT`).
  Callbacks never call back into the policy.

## Tests
`ctest --test-dir build/host -R annunciator` after the host build (CONTRIBUTING.md "Build and
test").
- Every D05 sequence: chime and walk-trip names after their tones, walk start (after the
  acknowledgement and bell test), end (all / some / zero tested), the five-minute warning, arming,
  armed (each mode, instant, auto-stay), disarmed, entry, not ready (AUD-008, WALK-016).
- Every suppression: voice, sub-switches, volume 0, power saving, no pack, silent exit latched,
  exit sounds off, refused and already-disarmed actions, entry that ends, partition 2, the
  baseline; duress identical to the ordinary action (AUD-010, AUD-015).
- The queue: tickets, overflow, coalescing, expiry, supersession, configuration and pack
  generations, cancellation by a loud pattern, the 10-second ceiling, Preview (AUD-012, AUD-015);
  speech due from the arm to the words' end and not past a request's deadline (AUD-034).
