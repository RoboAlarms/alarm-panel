# 08 · Audio and annunciation

**Status:** In progress - the tones (`components/core/sounder`) are host-tested and play on the
panel and in the PC simulator; Left: the listening checks on a panel (BL-032) and voice (en-US
words and phrases recorded on a computer, and imported files; no speech is made on the panel,
ADR-052), planned not built: A0, the voice and the word list, started 2026-10-08 (BL-015); the
first build, spoken zone names from the library, built 2026-10-09 - with word forms (AUD-031, unused by the current pack), the Chime, Voice and
Beeps volumes (AUD-032), the Voice volume made on the panel, up to louder than the pack (AUD-033), and
the exit beeps waiting while the panel speaks (AUD-034);
its pack, revision 8 (each word spoken alone, 60 ms between words), chosen by ear on a bench panel
(2026-10-10) - and waiting for the listening check of its chime, walk test and status messages (BL-202,
[voice-first-build.md](voice-first-build.md)); custom recordings and A5 after it (BL-044). · **Milestone:** M2 (tones), A0-A5 below (voice) ·
**Depends on:** 01, 06, 29, 33; coordinates with 07, 14, 17, 26, 31, 36, 37

## Goal
Keypad sounds that mean the same thing as on a classic alarm keypad: exit and entry beeps, chime,
trouble, burglary, fire (temporal-3) and CO (temporal-4). They play through the CrowPanel's two
I2S speakers. Add optional spoken status and zone-name chimes without weakening the tone
path. Once installed, playback must work with no internet, Home Assistant or computer running.
The speech is made on a computer, never on the panel (ADR-052): a text-to-speech model on the
production computer speaks a fixed list of words and short status parts, and the accepted
recordings ship compressed in flash. An installer builds a zone's spoken name from those words
on the panel, with no computer or model; an advanced user can import a recording for a name
outside them. Choose the source per zone or supported system prompt. Missing or not-ready
audio uses the existing tone behavior. American English (en-US) is the first voice; another
language's recordings will come with its language pack (docs/features/37), later. The recorded library is the proposed default once
voice is enabled; fresh installations still start with tones only.

This document owns the current plan. [Audio storage](audio-storage.md) defines the proposed
pack, flash/SD placement and update lifecycle. The earlier
[offline TTS research](../../research/AnnunciationResearch.md) looked for an engine to run on
the panel; since ADR-052 it is history.
The [built-in library production plan](voice-library.md) proposes 221 selectable words and 16
status and walk-test parts, joined into those messages, for en-US, with an editable [recording
script](recording-script.csv), the text-to-speech engines to audition on the production computer,
approved recordings stored in the repo, locale release rules and a compression budget. The voice
(Qwen3-TTS `designed_female`) and the codec (Opus 32 kbps, mono 16 kHz: 575 KiB for the 237 clips)
were chosen by ear on 2026-10-08; the takes themselves still await acceptance.

## What exists and what is missing

| Area | Present in source | Still required for voice |
|---|---|---|
| Output | Board I2S, amplifier control, portable tone mixer, independent sounder task, PC audio | Buffered speech input, codec/resampling, arbitration, underrun and cancellation handling |
| Event inputs | `alarm_service` event/snapshot/sound listeners; `on_sound` carries partition and zone | Bounded announcement policy, current name/config copy, stale-request cancellation; today's `panel_app` sound hook discards the zone |
| Storage | FAT SD worker and exclusive file leases; 2560 KiB flash partition named `assets` | No voice pack in the pack store (ADR-045), clip loader, voice manifest, importer or cache exists |
| Audio sources | Research only | Original word/phrase pack recorded on a computer, recipe editor, custom-file importer and durable readiness index; voice audition and measurement |
| Settings | Tone volume, key clicks, chime and power-saving behavior | Per-assignment source selection, sample playback, pack management, preparation status and browser parity |
| Z-Wave | Device-owned keypad feedback has its own implementation in Feature 36 | No voice dependency: Indicator commands trigger the remote device's own sounds, not this panel's audio files |

## Reference behaviour
Keypads beep slowly during the exit time of Away (and Maximum) and faster for the last 10 s;
Stay, Night and Instant give the arming beeps and a quiet exit time. They beep during entry time
(rapidly if an alarm happened while away), sound continuously for burglary alarms, use the
temporal-3 pattern for fire, chime on door opens when chime mode is on, and beep for troubles
until acknowledged. Touchscreen panels also speak status ("Armed away. You may exit now.") and zone
names for chime.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| AUD-001 | Plays the partition's continuous pattern (`status.sounder`) and one-shot sounds (`on_sound`) with priority fire > CO > alarm > aux > entry > exit > trouble | `sounder` host suite + simulator + HIL |
| AUD-002 | Fire uses temporal-3 (0.5 s on/0.5 s off x3, 1.5 s off); CO uses temporal-4 (0.1 s on/0.1 s off x4, 5 s off) | `sounder` host suite (measured off the rendered PCM) |
| AUD-003 | Alarm, aux, fire and CO always play at full volume; key, chime and trouble volume is user-adjustable | `sounder` host suite + HIL |
| AUD-004 | No pop at power-up: amps enabled only after silent I2S frames are flowing (GPIO30 active low) | `sounder` host suite (the silent first block) + HIL (the pop itself) |
| AUD-005 | One-shot sounds (key click, ack, error, chime, walk test) are dropped while alarm, aux, fire or CO sound; over quieter patterns they briefly pause the pattern, which then resumes. A background one-shot (the arming warning's chime, docs/features/41 AUT-012) plays only over silence and stops as soon as any other sound is asked for: it never delays or pauses an entry, exit, trouble or alarm sound | `sounder` host suite + HIL |
| AUD-006 | The sounder renders at the rate the board's output plays (8-32 kHz in whole hundreds of Hz, `board_info_t.audio_rate_hz`; 32 kHz on the 7"): every pattern keeps its timing and its tones, fire and CO included, at every such rate (docs/features/33 step 8) | `test_sounder.c` (8, 12, 16, 24 and 32 kHz) |
| AUD-007 | Audio keeps working when the UI task is busy (own task, own buffers) | stress test |
| AUD-008 | An engine-approved chime plays its existing tone once, then the zone's current approved spoken name; a zone without one, voice off, a missing pack or failed speech leaves that tone alone ([voice-first-build.md](voice-first-build.md) "The chime") | policy: `components/core/annunciator/test/test_annunciator.c` - the name after the tone's PLAYED, a canceled or unadmitted tone, every eligibility failure (no name, unapproved, stale binding or pack, bad token, missing clip, over 10 s, switches, no pack); planned: sounder tone-ticket sequencing (BL-202) |
| AUD-009 | Withdrawn 2026-10-08 (ADR-052): no speech is made on the panel, ahead of use or during an event; the library's recordings are made on a computer (AUD-023) | - |
| AUD-010 | Fire, CO, burglary and aux cancel speech. Optional speech never changes essential tone samples or timing, never pauses, retimes or reduces alarm or entry tones or the exit delay's last 10 s (the exit beeps before them wait while the panel speaks, AUD-034), and cannot expose a silent alarm, a hidden report or duress (duress sounds like the ordinary action) | policy: `components/core/annunciator/test/test_annunciator.c` - a loud pattern on any partition cancels all speech (new generation) and queues nothing, a silent alarm changes nothing, paired ordinary/duress arm and disarm give identical speech; planned: rendered tone comparison, paired engine scenarios (BL-202) |
| AUD-011 | One owner renders audio and controls I2S/amps. Event callbacks only copy bounded data; filesystem work and decoding run outside alarm/UI/audio callbacks | planned: architecture review, fault injection, stress |
| AUD-012 | One active and four waiting requests carry identity, state, configuration and pack generations and a bounded expiry (2 s for chime, trip and refusal, 5 s for status and walk control); repeats coalesce and superseded speech never plays | `components/core/annunciator/test/test_annunciator.c` - one active and four waiting, overflow and eviction, coalescing, 2 s / 5 s expiry, supersession after the tone, configuration and pack generations, the state-action generation (not ready dropped by a bypass arm, armed dropped by an instant mode change or a new silent cycle, a disarm kept), loud cancellation, the 10 s ceiling |
| AUD-013 | Packs and imported files have bounded, versioned metadata and integrity checks. Install/replace preserves a verified working generation or falls back to tones; never repartition or autoformat on a mount failure | planned: storage/parser fixtures and interrupted-install tests |
| AUD-014 | Missing/full/corrupt/removed storage, incompatible packs, exhausted RAM and a failed import cannot block alarm startup or tones | planned: simulator failures and disposable-media HIL |
| AUD-015 | Automatic voice defaults on, as on modern panels (D06, chosen 2026-10-12), speaks only once a valid pack is installed and can be turned on only with one; it uses the Voice volume (AUD-032) and the power-saving policy; the packs carry speech levelled for the panel (`tools/voice/speaker.py`; up to revision 6 raised to -6 dB of voiced RMS with the peaks limited under -1 dBFS, a few times over, which squeezed it at every volume; from revision 7 clear speech, each word levelled with nothing limited, and the panel makes it louder with the Voice volume, AUD-033 - chosen 2026-10-10). Preview plays with the voice off and never turns it on | level: `tools/tests/test_voice_tools.py` `SpeakerStageTests` (the pack's settings: -15 dB of voiced RMS, nothing limited, 10 and 20 ms kept at the ends; a master raised by one gain capped at the peak; a sharp peak holding the gain back; the quiet ends trimmed to the settings; on a tracked master the voice between -18 and -14.9 dB with no peak over -1 dBFS; the resampler's overshoot held at the ceiling; digital silence left silent; revision 6's limited -6 dB, still made by the bench's `--limit`); policy: `components/core/annunciator/test/test_annunciator.c` - voice on by default, each switch, zero volume, power saving, no pack, Preview with the voice off and its refusals; Panel Manager: `panel_pages.test.cjs` (AUD-015 tests) - the switches' defaults, anyone may set them, turning on refused without a ready voice (no pack, unavailable) and in words; planned: preference persistence on the panel (BL-202) |
| AUD-016 | Durable binding, approval and pack revisions keep a zone from speaking an obsolete name: a rename, partition move, sensor reassignment or slot reuse needs a new approval; a type change of the same zone keeps it (CORE-CFG-021) | `test_ui_voice` (rename, partition move, sensor change, slot reuse and pack replacement refuse; the installer's own rename or move approves; someone else's save refuses the approval, not the save), `test_config` (CORE-CFG-021), zone_hub `test_voice_epoch` (a sensor changed during an invalidation) |
| AUD-017 | Pack import/activation, removal and cache clearing follow existing master/installer access; local and browser controls enforce the same permissions | planned: authorization and UI/API tests |
| AUD-018 | Prepare only the selected source: validate recorded recipes or import/convert custom files. Reuse unchanged audio, reject stale results and never delay saving configuration or alarm operation; missing user recordings require reimport | planned: preparation, restart, interruption and storage-failure tests |
| AUD-019 | Local setup finds, orders, previews and approves at most eight vocabulary entries and 10 s of speech per zone (joins included) without a model, a computer or the internet; drafts and approvals survive a restart | `test_voice_pack` `test_recipe`, `test_ui_voice` (unknown IDs and an overlong recipe refuse), the preview's editor walk (`walk_voice`: Cancel, Done, limits, Find a word, leases) |
| AUD-020 | Custom audio is validated, previewed and explicitly assigned before use. Uploaded recordings are durable user assets, never disposable generated cache; removal respects active references | planned: import/authorization/format/retention tests |
| AUD-021 | Each zone or permitted system prompt selects one source, independently of other assignments. Switching sources is explicit; a missing SD card cannot disable library playback, and nothing silently substitutes another voice | planned: mixed-source installation and source-switch tests |
| AUD-022 | The supplied word/status library works from internal flash without SD. Custom audio lives on SD only; no hidden flash spillover. Missing/full/removed SD leaves built-in library assignments and essential tones usable | planned: no-card setup, upload/generation destination checks, full/removable-media tests |
| AUD-023 | Generate the supplied library on a computer; the repository tracks the reference, the accepted masters, the canonical Opus packets and their provenance, so any clone rebuilds the identical pack payload without a model (`voice.py verify`, `pack`). Regenerating the neural audio exactly needs the recorded inference stack; changed takes need listening acceptance; a draft pack is refused by release tooling. Only the recordings ship, never a model | `tools/tests/test_voice_tools.py`: `TrackedEnUsTests` (the tracked voice verifies and packs twice to the golden payload hash), `TrackedVoiceTests` (tampered master or packets, missing files, a DRAFT refused without `--allow-draft`, a changed payload under a recorded revision refused, review kept only for an unchanged take), `OggOpusTests`; locked reproduction by hand on the recording PC: `voice.py encode --reproduce`, `record --reproduce` (BL-202) |
| AUD-024 | Packs identify locale and voice explicitly; the voice never follows the screen language or mixes packs; one selected supplied pack in flash. The first build accepts the en-US voice only | planned: en-US voice with an es-US screen, incompatible-pack rejection (BL-202) |
| AUD-025 | Development builds only (`CONFIG_ALARM_BUILD_DEV`): a bench silence switch keeps this panel's speaker amplifier off and writes silence, alarm, fire and CO included, while the mixer still decides every sound, so a bench panel can be tested where its speaker may not be heard. Paired keypads and sirens are not covered: a bench that must stay quiet has none paired. Set at the console (`speaker off`/`on`) or over the developer link (`/dev/v1/speaker`, `panelctl speaker`); kept in NVS across restarts and dev updates; a factory reset, or flashing an image of another profile (USB without `-Profile dev`), brings the sound back. Said at start, by `status` and in `/dev/v1/info`. Release and legacy builds have no such switch (`check_release_profile.py` checks the image) | `test_panelctl.py` (the tool) + HIL |
| AUD-026 | RVOC format 1 ([voice-first-build.md](voice-first-build.md) "The voice pack") carries a bounded, immutable vocabulary with numeric token IDs and the status and walk-test clips; a token ID is never renumbered or reused | reader: `components/core/voice_pack/test/test_voice_pack.c` - frozen `voice_pack_fixture_v1.h`, registry IDs, every rejection (header, layout, index, text, keys, framing, Opus TOC, samples, SHA-256, coverage), the pack's own gaps within bounds (`test_gaps_within_bounds`); words: `components/core/voice_pack/test/test_vp_words.c` - Use zone name (longest phrase, number entries, leading zeros, unmatched spans, 8 entries, 10 s) and the alphabetical word lists, over the whole recording script (`vp_vocab_en_us.h`); planned: the registry check against recording-script.csv (BL-202) |
| AUD-027 | Language and voice packs share one transactional owner of the `assets` store, which keeps the working pack through a failed or interrupted replacement | host: `pack_store` (`test_envelope`, `test_put_blob_alternating_kinds`, `_needs_room_for_both_copies`, `_faults_keep_the_working_copy`, `_gate` (asked before every sector; closed after 0, 1, some or all sectors the old entry and table stay), `_refuses_bad_arguments`); `tools/tests/test_panelctl.py` (`test_voice_*`: the dev-link install and status commands); the ESP owner `lang_packs/pack_assets` and its voice user `panel_voice_install` (validate, state gate, swap by generation) build; host: `voice_pack_accept` (one voice, no DRAFT in release, the frozen identity record and its key, other audio under a known identity refused); the install reserves the spare copy slot before it writes, records the identity in NVS before the commit, and leaves a pack committed while locked pending until the panel opens; a signed pack found at boot with no identity record (one the Flasher wrote over USB) has its identity recorded then, and a known identity with other audio stays refused; on a panel: not yet (BL-202) |
| AUD-028 | Buffered decoding keeps each clip's exact trim and can never stall the sounder: cancellation or an underrun ends speech within a block; the bench silence (AUD-025) applies after speech and tones are mixed | `sounder` host suite (speech consumed and rejected, the warm-up block, idle and amplifier, tone tickets); `panel_voice` host suites `voice_codec`, `voice_render` (trim, reset, upsampling, pull sizes); the worker (`panel_voice.c`: ring generations, cancellation, underrun, the stuck-utterance limit) builds; on a panel: not yet - first speech, cutoff, underruns and muted final output (BL-202) |
| AUD-029 | The developer link and the Panel Manager install a voice pack through the same signature, size and state checks, and never change whether voice is on or the bench silence | the shared transaction `panel_voice_install` builds (size, signature, pack check, refused while armed, in alarm, in a walk test or installer mode, or an update runs; never touches the preferences); Panel Manager: `components/services/panel_web/test/panel_pages.test.cjs` (AUD-029 tests) against the mock - authorization, wrong size before reading, bad signature, armed, busy and the store's failures, preferences kept, each answer in words; planned: the developer link's refusals, draft and profile, OTA refusal, the firmware's own checks on a panel (BL-202) |
| AUD-030 | Development builds only (`CONFIG_ALARM_BUILD_DEV`): an audio bench plays audio from the PC (16 kHz mono, up to 30 s) through the panel's own speech path - the worker, D11's upsampler, the Voice volume's boost (AUD-033), the sounder's speech ring, the mixer and the bench silence - with no pack, policy or approval, so a voice's words, joins and levels can be tried on the panel in seconds; it plays between utterances, a newer one replaces it, and an alarm refuses it. The voice bench (`tools/voice/bench_server.py`) has the panel say words from its own installed pack instead: it sends only their clip IDs and the gap between words (`POST /dev/v1/say`, 1-250 ms, kept until the next one, a restart or a new pack), the panel joins and plays them as a Preview at its own Voice volume, which the bench reads and sets (`GET`/`POST /dev/v1/voice`, saved as the Settings slider saves it) | `tools/tests/test_panelctl.py` (the WAV's shape refused before contact, the request, the refusals and old firmware in words; `voice say`'s IDs and gap refused before contact, its request and refusals; `voice volume`'s request and range), `tools/tests/test_voice_tools.py` `BenchTests` (a phrase's words, the longest first); the firmware builds; heard on a panel (2026-10-09, `voice.py try --device`; 2026-10-10, the voice bench with pack revision 7) |
| AUD-031 | A spoken name ends in its last word's ending - the word as it sounds at the end of a name, longer and falling - and the words before it play in their own clips, cut from inside a name, when the pack has that ending (vocabulary schema 2: category 7, ID 0xC000 + the word's, for words up to 0x3FFF). An ending is never selectable, its word must be in the same pack, and the 10-second budget counts it as it plays. A pack without endings plays as before (the supplied pack has none from revision 7: each word was chosen spoken alone, 2026-10-10); readers before schema 2 refuse one, so the firmware goes first | `components/core/voice_pack/test/test_voice_pack.c` (the frozen fixture v2: the endings found by their word, never listed or accepted as words, the budget; refused: an ending without its word, outside its namespace, in a schema 1 pack); `components/core/annunciator/test/test_annunciator.c` `test_name_ends_in_its_ending` (chime, Preview, a word without one, status and walk unchanged); `tools/tests/test_voice_tools.py` `EndingPayloadTests`, `FormsTests`, `LibraryTests` (the pack writer and reader, the fixture read by both, the carriers) |
| AUD-032 | The everyday sounds have three volumes, each 0-100 % and 70 % at first: Chime (the door chime), Voice (announcements and Preview) and Beeps (entry and exit, trouble, the walk test, key clicks). Alarms, fire, CO and aux sound at full whatever they are set to (AUD-003). A panel updated in place starts the Chime and Voice volumes at its old volume. Anyone signed in sets them on the touchscreen (Screen and sound: Chime, Voice, Beeps; Voice only with a voice) and in the Panel Manager (`config/display`: `chime_volume`, `voice_volume`, `volume`) | `components/core/sounder/test/test_sounder.c` (AUD-032: each sound at its own volume, the other two held at their extremes; speech joins at unity, its Voice volume applied before the mixer (AUD-033), -6 dB under the entry beeps and the fast exit beeps; alarms at full); `panel_pages.test.cjs` (AUD-032: the three in order, Voice only with a voice, saved and kept, 101 refused, the Announcements words); planned: the upgrade default on a panel (`panel_prefs.c`, ESP only) |
| AUD-033 | The panel applies the Voice volume to speech before the mixer and can make it louder than the pack: every 10 % of the volume is 3 dB (silent at 0 %, -6 dB at 50 %, the pack's own level at 70 %, the default, +9 dB of gain at 100 %), and where it lifts a peak over -1 dBFS a limiter with 2 ms of look-ahead holds it there; at a gain that cannot (up to 66 %) speech is only scaled, with no delay. The words already peak near -1 dBFS, so the limiter takes back part of a gain over 70 %: on pack revision 7, +9 dB is heard as about +4.7 dB (chosen 2026-10-10, after the volume as a proportional gain, the pack's level at 35 %, left 50 % and 100 % only about 2.4 dB apart and sounding the same on a panel). The packs hold clear speech, so a low volume plays it unsqueezed (chosen 2026-10-10, after revision 6 with its loudness in the pack and a low-volume clarity stage sounded harsh at every volume). `components/core/sounder` `speech_boost`, after the renderer on the panel and its audio bench (AUD-030), in the simulator and in the preview; the mixer adds speech at unity | `speech_boost` host suite (the gain at each volume, 3 dB for every 10 %, only scaled up to 66 %, under the ceiling only louder, the look-ahead given back, pulls of any size, the ceiling at every volume, the limiter letting go); `sounder` host suite (speech joins at unity); the preview's voice walk renders each utterance through the stage and compares it with what was played; on a panel: heard on a panel with pack revision 7, each step of the volume clearly apart (listening check 2026-10-10: "perfect") |
| AUD-034 | While the panel speaks during the exit delay, or has words on their way (an utterance queued or playing, `ann_due()`), the exit beeps wait: a beep already sounding finishes, the next one waits until the words end and then sounds, the beeps going on every 1 s from it; they wait 10 s at most (`SND_EXIT_WAIT_MAX_MS`), then one beep sounds over the words and the next waits again. So arming with an exit delay sounds as on a modern panel: the acknowledgement, "Arming away. You may exit now.", then the exit beeps; the first beep doesn't slip in between the acknowledgement and the words. The exit delay keeps counting; its last 10 s fast beeps, the entry beeps and every alarm never wait. Speech over the waiting beeps is at unity (6 dB lower under the entry beeps and the fast exit beeps). Chosen 2026-10-10, after hearing the arming words spoken under the exit beeps on a panel, and a modern panel's order for the first beep; it is made in the panel's mixer (`components/core/sounder`, `snd_set_speech_due()`), which the voice worker tells when speech is due (`sounder_speech_due()`) | `sounder` host suite (the exit beeps wait for the words - the beep sounding finishes sample for sample, the next sounds as the words end and then every 1 s; the first exit beep waits for arming words that come 1-4 blocks after the acknowledgement; words still filling hold them; speech due alone holds each 10 s at most, and a new exit delay waits its full 10 s; speech due holds nothing else - every other pattern and the one-shots sample for sample, an alarm taking over in its first block; a one-shot sounds while they wait; the fast beeps don't wait; speech at unity over the waiting beeps); `annunciator` suite (speech due from the arm to the words' end, never past a request's deadline); the voice walk (`host/ui_preview/voice_walk.c`: no exit beep under the arming words); on a panel: not yet - arming and listening is still to do (BL-202) |

The former duplicate `AUD-006` voice-chime row is now `AUD-008`; `AUD-006` remains the
existing sample-rate requirement and its tests retain their meaning. Voice rows describe
future acceptance, not tests that have already passed.

## Pattern specification (as built)

Tones sit in a musical, speaker-friendly range rather than a piezo's 3 kHz; only fire and CO
use the high, standard alarm pitch. Timing is what makes a pattern recognisable, and the fire and
CO timings follow NFPA 72 / NFPA 720 exactly.

| Pattern | Timing | Tone | Volume |
|---|---|---|---|
| `exit` | 80 ms beep every 1 s; the next beep waits while the panel speaks (AUD-034) | 1320 Hz | beeps |
| `exit_fast` (last 10 s) | 80 ms beep every 250 ms | 1320 Hz | beeps |
| `entry` | 250 ms on / 250 ms off | 1568 Hz | beeps |
| `entry_memory` (alarm happened while away) | 80 ms on / 80 ms off | 1568 Hz | beeps |
| `trouble` | two 100 ms chirps every 10 s until acknowledged | 1175 Hz | beeps |
| `aux` (24 h aux, medical) | 400 ms on / 200 ms off | 1760 Hz | full |
| `alarm` (burglary, audible panic) | continuous two-tone, 220 ms each | 880 / 1245 Hz | full |
| `fire` | temporal-3: 3 x (0.5 s on, 0.5 s off), then 1.5 s off | 3100 Hz | full |
| `carbon_monoxide` | temporal-4: 4 x (0.1 s on, 0.1 s off), then 5 s off | 3100 Hz | full |
| `ack` | single 150 ms beep | 1480 Hz | beeps |
| `error` | one long 800 ms tone (a "long beep") | 440 Hz | beeps |
| `chime` | "ding-dong", 280 ms + 700 ms, bell-like decay | 1319 / 1047 Hz (E6, C6) | chime |
| `walk_test` | three 70 ms beeps | 1760 Hz | beeps |
| `walk_test_warning` (5 min and 1 min before a walk test ends, docs/features/40 WALK-004) | two 200 ms beeps, 120 ms apart | 1480 Hz | beeps |
| key click | 30 ms tick with decay | 2200 Hz | beeps |

Sine tones with 3 ms attack and release ramps (no clicks), synthesized at 32 kHz, identical on
both speakers. Volume "full" ignores every setting (AUD-003); the three everyday volumes stand
on their own and default to 70 % (AUD-032): "beeps" (`sounder_set_volume()`, console `volume`),
"chime" (`sounder_set_chime_volume()`) and speech (`sounder_set_voice_volume()`, -6 dB more
under the entry beeps and the exit delay's fast ones).

`components/core/sounder/test/test_sounder.c` checks this table by *listening*: it renders the mixer and measures
the beeps and gaps off the PCM, so a ramp or a step boundary that eats part of a beep fails the
build. Changing a timing or a tone here means changing that test in the same commit.

## Design
- **The sounds are portable** (`components/core/sounder`, C11, no heap, no RTOS, no audio device):
  the pattern tables, the sine synthesizer and the mixing rules. The caller owns a
  `snd_mixer_t`, sets what should be playing and asks `snd_render()` for the next block of
  frames. It is the single copy of what the panel sounds like: the board, the PC simulator and
  the host tests all render the same blocks, so a pattern proved at the desk is the pattern on
  the wall.
- **Sounder task** (`components/services/platform_esp/src/sounder.c`, priority 6): one mixer, rendered 10 ms
  at a time and written through a sink (`sounder_sink_t`: `write()` + optional `amp()`), so the
  same code can drive other boards. The blocking I2S write paces it; DMA holds about 32 ms
  (4 x 256 frames). Nothing but the queue, the task and the amplifier is left here.
- **On the PC** (`host/ui_sim/sim_audio.c`): the same mixer through waveOut, wired to the
  simulated world exactly as `panel_app` wires the board (continuous from every status
  snapshot, one-shots from `on_sound`, key clicks from the UI). Everything it plays is trimmed
  by one figure (`SIM_AUDIO_TRIM_PCT`, 50 %) so a full-scale alarm does not hurt through
  headphones; the patterns and the balance between them are the panel's exactly.
- **Inputs**: `sounder_set_continuous(sound)` from every status snapshot (the local keypad's
  partition), `sounder_play(sound)` from `on_sound`, `sounder_key_click()` from the touch UI.
  Requests go through a queue, so callers never block. The continuous pattern is state, not a
  request: the latest one wins and the task reads it every block (the queue only wakes it), so
  a queue full of clicks and beeps can never drop the start or the end of an alarm.
- **Priority** comes from the engine: `status.sounder` already holds the most important
  continuous pattern (fire > CO > alarm > aux > entry > exit > trouble). The sounder plays it and
  restarts it from the beginning whenever it changes.
- **One-shots**: up to 4 queued (key clicks are dropped when 2 are waiting). They are dropped
  while a loud pattern plays (alarm, aux, fire, CO); otherwise they pause the continuous pattern,
  which resumes where it stopped.
- **Amp control**: on as soon as anything plays, after one 10 ms block of silence; off after
  1.5 s with nothing playing, and never while a pattern is still running (the trouble chirps
  leave 9.7 s of gap, which must not click the amplifier off and on). The mixer decides this
  and reports it per block (`snd_block_t.amp`); the I2S channel keeps clocking silence
  (`auto_clear`) in between.
- **Voice (planned)**: a recorded-word recipe or an imported complete clip supplies
  persistent files to one bounded speech player. The library provides simple field setup;
  imports cover labels beyond its vocabulary. See the lifecycle and
  [storage contract](audio-storage.md); the library's clips are Opus 32 kbps, mono 16 kHz
  (decided by ear 2026-10-08, over Opus 24 kbps and LC3 32 kbps), decoded on the panel by a
  portable decode-only libopus; see the [production comparison](voice-library.md).
- **Keypad role**: identical; the pattern comes from the master's status snapshot.

## Two ways to create an announcement

**Direction:** support both together, selected per zone or permitted system prompt.
They are authoring choices feeding one player, not two global operating modes. A third, text
to speech run on the panel, was dropped on 2026-10-08 (ADR-052).

| Source shown to the user | Setup workflow | Preparation and tradeoff |
|---|---|---|
| Library words | On the panel, choose and order “Front” + “Door” or “Upstairs” + “Bedroom” + “Window”; preview and save | Supplied recordings in internal flash, stable word IDs and a saved recipe. No SD, model or generation wait; limited vocabulary and joins need audition |
| Audio file | With SD fitted, import a personal recording or externally generated clip through local Panel Manager or from the card; preview and assign | Validate/convert once and keep a managed copy on SD. Broad voice choice without model cost; the user supplies a replacement when its meaning changes |

Build the status messages from short recorded parts: the words of one phrase joined closely,
“Armed Away”, and a natural, pack-defined pause before “No entry delay.” only when it applies
(decided 2026-10-08: flash is tight; 2026-10-09: a phrase's words 60 ms apart, cut from carrier
sentences so they don't each end like a sentence, and the pause only before a new sentence; 40 ms
in pack revision 6, with the word forms; 100 ms in revision 7, each word spoken alone, and 60 ms
again from revision 8, more natural, 2026-10-10).
A message nothing else shares, such as “Please disarm the system.”, stays one recording. The zone vocabulary should cover
common locations, modifiers, objects and numbers, with short phrase tokens such as “Living
room” where they sound better. A0 must freeze the actual inventory and audition combinations,
including repeated words and awkward joins. Proposed recipe limits: eight tokens and ten
seconds total, including pack-defined gaps; validate before activation.
The current draft inventory and its 237-clip script live in [voice-library.md](voice-library.md).
Use full-quality source exports for editing, then compress the shipped pack. Generate on the
production computer (or through a TTS API) and check approved recordings, scripts and
provenance into the repo. The engines to audition there are Qwen3-TTS 1.7B and VoxCPM2, with
Kokoro-82M as the baseline ([voice-library.md](voice-library.md) "Production engines"); API
providers remain eligible. A supplied, measured pack created this way lives in flash. CI
encodes the saved masters without invoking synthesis. Plan per-locale release bundles from one
codebase; whether application binaries differ depends on UI/font resources.
Future locales need their own messages and label grammar, not a word-for-word English list;
see the [production and globalization contract](voice-library.md).

Each assignment records its semantic target, source kind, revision and source-specific
reference: ordered token IDs and pack revision, or imported asset ID/hash. The spoken label is shown beside the display name and has a
whole-announcement Preview. A library recipe uses one compatible voice pack; v1 does not
silently mix library words with imported fragments. A user may deliberately choose different
voices for different assignments. Changing the library voice does not replace custom files. Switching source retains the previous draft for editing, subject
to storage limits, but only the explicitly selected current binding can play.

The recorded library plus custom-file playback is the whole plan: nothing is synthesized on
the panel, or on a server at run time. Creating a file elsewhere and importing it is an
explicitly supported workflow; no computer needs to remain connected for playback.

**Storage decision:** the supplied library has a fixed, measured internal-flash budget.
Custom recordings require an installed healthy SD card, even if an individual file could fit
flash. This keeps user-controlled capacity out of the
hardware's built-in audio budget. Small settings/recipes remain internal; RAM buffering is
temporary. Without SD, an imported file shows “SD card required”; its saved binding stays
intact but uses tones until the correct file is available. Built-in library assignments
continue normally. Do not silently switch an affected zone to different spoken content.

**Voice-quality gate (listening feedback):** the voices presented from the initial
sanoTTS/Inflect/Flite shortlist were unacceptable. There is currently no accepted voice.
The accepted quality reference is **a commercial touchscreen panel's voice**. Use its
annunciator delivery as the listening benchmark. Do not select a voice merely because it is convenient. A0 auditions on the production computer against
that reference; A5 then listens to the compressed result through the panel's own speakers.
The historical shortlist remains research evidence, not an approved implementation choice.
Commercial panels speak preprogrammed zone descriptors.
That establishes a vocabulary-based interface, not any particular speech engine or rights
to recordings. Plan an original consistent voice pack with status parts and
common zone words. Imports cover names outside that vocabulary. The project has
approved the combined approach, but no particular recordings or synthesized voice yet.
Pin any newly selected engine/voice and verify
their terms, including generated-output distribution, before producing a distributable pack.
The earlier target of two selectable voices is an audition goal; v1 voice count remains open.

The voice comparison records each engine's model and revision, licence, voice or reference,
settings and seed, the clip byte counts after compression, intelligibility and the listening
preference. Include “Arming away. You may exit now”, “Disarmed”, “Upstairs bedroom window”
(whole, and joined from words) and “Zone one hundred twenty-eight”; the full audition set is
[voice-library.md](voice-library.md) "Small audition before recording everything".

## Prepare once, play many times

Preparation and playback are separate work queues with different deadlines:

1. Resolve each selected source ahead of use. A library recipe references verified recorded
   clips in order, possibly indexed within one pack file; it need not duplicate them into a
   new file for every zone. An import is
   converted/validated once. Each produces a bounded ready-to-play clip or playlist. No arbitrary live sentences or spoken
   countdown dependency in v1.
2. Save a zone/configuration change immediately under its existing access rules; enqueue
   optional preparation separately. The worker stages, verifies and commits files and their
   current binding identity. Never mark RAM-only or partly written audio as durably ready.
   Imports stay user-owned assets; nothing regenerates a missing recording. Offer reimport
   or an explicit source change instead.
3. By default prepare while all partitions are disarmed/silent, on mains and outside OTA or
   storage maintenance. Coalesce rapid edits; cancel/supersede obsolete jobs and pause when
   arming, an alarm, power saving or maintenance begins.
4. At an event, play the current verified clip/playlist through the shared player, with
   bounded buffering outside the audio task. A missing token, unavailable file or unready
   buffer leaves tones in place immediately. Do not substitute another source,
   write audio or speak late when preparation completes. Eligible repair runs separately.
5. On reboot, reconcile the manifest with current configuration and reuse verified files.
   Resume only missing/invalid preparation jobs. Removing SD makes imported files unavailable.
6. A zone name change marks its spoken binding for review in the same editor. A word recipe
   or recording needs preview/confirmation against the new name. The user can explicitly
   keep it after a cosmetic rename. Until then use tones, preserving the audio asset/draft.
   Reusing a zone slot for a different zone always invalidates the binding. For a pack
   change, stage the affected set before activation and show it as pending; retain
   the existing voice only while its bindings remain current. Show progress, missing words
   and failures; never block arming while waiting for speech.

Recorded words and imported clips persist across reboot. The RAM cache is expendable;
clearing it never deletes user recordings or library packs.

## Proposed first-release behavior

The first build (BL-202) decided this part differently in places - the chime keeps its tone
and says the name after it, the walk test speaks now: [voice-first-build.md](voice-first-build.md)
is the design of record where the two differ.

Initial target: the 7-inch P4 and its PC simulator. Other boards expose no voice controls
until their own resource qualification. American English (en-US) first. This is spoken status and chime,
not arbitrary network TTS, a music player, an assistant or replacement life-safety speech.

| Trigger | Spoken behavior | Cancellation/fallback |
|---|---|---|
| Accepted arm with exit delay | Once: “Arming away/home/night. You may exit now”; use the actual mode and silent-exit policy | Refused arms never say armed; disarm/mode change cancels pending speech |
| Actually armed, including instant modes | Once: “Armed away/home/night”; add “No entry delay” where appropriate | Match actual engine state; snapshots/reconnect do not repeat it |
| Accepted disarm that changes state | Once: “Disarmed” | No “Disarmed” for a refused command or an already-disarmed code entry |
| Entry starts | Once: “Please disarm the system” | Entry beeps retain timing; cancel on disarm/alarm/expired entry |
| Engine emits chime | Current zone label, e.g. “Front door”; optional voice mode replaces that chime tone | Disarmed/chime-enabled/non-bypassed rules come from the engine, regardless of sensor source |
| Not-ready arming refusal | Once: “Not ready. Check the zones” | Short expiry; no automatic reading of every open zone |
| Wrong code | Existing error tone only in v1 | Never speak digits, identify a user or announce a duress condition |
| Trouble, walk test, alarms | Existing tones in v1; the walk test's spoken zone names and phrases follow as one more target ([40](../40-Walk-Test/README.md) W6, WALK-016) | No spoken alarm loop or trouble schedule in this release |
| Startup, history replay, reconnection | No unsolicited state announcement | Establish the current baseline; explicit Sample is separate |

Source selection changes the audio for these existing semantic targets, not their triggers.
An imported file for a system prompt requires preview and confirmation of its stated purpose
(for example, the actual Away-armed state). A filename/transcript is not proof of its spoken
meaning. Customization does not add speech to hidden events or replace essential alarm tones.

Silent panic produces no new speech. Duress looks and sounds like the equivalent ordinary
accepted action: suppress the hidden duress report, not the normal disarm/arm confirmation.
Silent exit suppresses exit-related speech as well as the applicable beeps. Battery saving
suppresses optional speech initially, matching the cosmetic chime policy; essential tones
retain their existing rules. These combinations need engine-driven tests.

Use engine events/status and accepted sound requests, not phone-notification delivery/replay
or raw sensor packets. Event-to-phrase mapping uses semantic IDs; do not speak every Activity
sentence (some contain user identity or hidden events). Reuse shared terms from Feature 26
where appropriate, with speech-specific text normalization in one place.

## Proposed implementation boundaries

The first build (BL-202) decided this part differently in places - the chime keeps its tone
and says the name after it, the walk test speaks now: [voice-first-build.md](voice-first-build.md)
is the design of record where the two differ.

- A portable `annunciator` core component owns policy, bounded requests, phrase/clip validation
  and cancellation; no heap, globals or filesystem in core.
- A feature-gated `panel_voice` service owns recipe resolution, import/preparation/storage
  and decode workers. Proposed option
  `ALARM_FEATURE_VOICE` needs board audio and the qualified resource profile. These names/APIs
  are proposed, not existing components. Follow Feature 33, including host sources and stubs.
- `panel_app` wires engine listeners once. Copy zone/partition, text/config generation and
  the current speech eligibility at the alarm boundary; never call synchronous alarm APIs
  back from a listener. Copy only the fields needed, not a full snapshot into each request.
- Start with four waiting requests and one active; status/entry supersedes chime. Coalesce
  same-zone repeats; chimes/refusals expire after 2 s and ordinary status after 5 s, also
  invalidated by state changes. Drop excess low-priority work instead of playing a backlog.
  No utterance may exceed 10 s in v1. Tune only from recorded listening/latency evidence.
- The existing sounder task remains the only I2S/amp owner. It consumes already-buffered
  PCM without filesystem locks or synthesis. Track speech in its wake/idle/amp decisions;
  do not pass long clips to `snd_play()`, whose one-shot behavior pauses tone time.
- Alarm/aux discards queued speech immediately. Entry tones and the exit delay's last 10 s
  continue at their normal gain and sample timeline, with speech mixed below them and limited
  to remaining headroom. Voice must not clip or duck those tones; if intelligibility fails,
  defer that voice prompt instead of changing tone timing. The exit beeps before the last 10 s
  wait at a beep's start while speech is due or playing, 10 s at most (AUD-034). Retain the
  existing short key/ack/error behavior.
- A chime replacement is committed only when valid current-name audio is ready to start.
  Otherwise emit the existing chime immediately; late generation only warms the cache. A
  failure during voice chime emits at most one fallback tone if still eligible and no alarm
  has started. Do not produce both a successful voice chime and the tone accidentally.
- Worker output uses a bounded PCM ring; the audio task can discard it by generation.
  Speech cancellation cannot wait for an SD read to return. Buffers
  in PSRAM, DMA/internal scratch sized separately; flash-writing tasks use internal stacks.

## Settings and delivery

Proposed Settings > Display and sound > Announcements: voice on/off, system announcements,
voice chime, library voice, preparation progress, Sample and a clear availability reason. Existing
user volume applies; no alarm volume control is added. Voice starts off. Chime's existing
code requirement remains authoritative; a voice-mode setting cannot turn chime on or bypass
that gate. Ordinary display/audio access follows Feature 14 and lockdown settings.

System-master/installer pack and file management shows installed version, location, bytes,
references and compatibility. Import/activate/remove requires disarmed/silent
status and rechecks it before activation; an alarm interrupts optional work without delaying
the engine. Browser controls have the same policy (Feature 31). There is no raw filesystem
browser or arbitrary-path upload. Storage initialization/erasure is a separate explicit action.

Installer > Zones > Spoken label offers **Library words / Audio file**,
the display name, complete preview, readiness and the selected source's editor. The library
editor supports search/categories, add/remove and reorder directly on the panel. Installed
files can also be selected there; importing a file is separate from changing zone programming.
Permitted system-prompt overrides are likewise installer assignments. This preserves
[Feature 14](../14-Configuration-and-Installer-Programming/README.md)'s zone authority; the
system master can manage assets without acquiring zone-programming permission.

Provide SD import and bounded authenticated Panel Manager uploads for packs and custom files,
using the same installer/validator, so ordinary file transfer needs no access behind the case.
Plan PCM16 WAV as the first import format; define exact input bounds/conversion in
[audio-storage.md](audio-storage.md). MP3/AAC and microphone recording are not promised for
v1. A local browser connection suffices for uploads; internet is not required. Custom uploads
are written to SD only and are refused before transfer if no writable card is available.
Provision the supplied core library in internal flash during initial voice setup and size
its inventory to the fixed budget; its word selection/playback must work without SD. Later
firmware updates preserve the installed library and SD assets.

Playing an imported clip needs readable media; an import also needs space for safe staging.
Boot repair reports missing library or imported audio for reinstall rather than inventing
replacements.

## Test and acceptance

- **Tones on the PC:** `components/core/sounder/test/test_sounder.c` renders the mixer and
  measures the PCM: the life-safety timings (AUD-002), the continuous pattern (AUD-001), the
  volume (AUD-003), one-shots over patterns (AUD-005), the silent first block and the
  amplifier's rest (AUD-004), power saving, and every board rate (AUD-006). The panel simulator
  (`host/ui_sim/sim_audio.c`) plays the same mixer through the PC's speakers.
- **Tones on a panel:** levels and the power-up pop by ear (AUD-003, AUD-004), the HIL parts
  of AUD-001 and AUD-005 and the stress test of AUD-007 (sound stays smooth while the UI is
  busy) are still to do (BL-032, Tasks).
- **Voice:** nothing is built yet; each step's evidence is in the table below.

## Acceptance gates and work order

Implementation is scheduled **after Z-Wave**. Feature 36 has several phases; the maintainers
must identify the completion checkpoint at handoff (accepted current sensor/keypad scope or
all Z1-Z11). Do not silently require every future Z-Wave extension.
Globalization (docs/features/37, M22) went ahead of A0 (docs/features/37 "Goal"): the voice
packs take their locale IDs from it. A0 itself was started on 2026-10-08, ahead of Z9. A0's decisions are backlog row BL-015,
A1-A5 are BL-044 ([backlog](../../BACKLOG.md)).

| Step | Deliverable | Completion evidence |
|---|---|---|
| A0 | On the production computer, audition TTS engines, voices and word joins (en-US); define vocabulary, repo masters, locale release contract and media bounds | Quality acceptance against a commercial touchscreen panel's voice, recorded producer configuration and accepted sample masters, rights and measured storage budget |
| A1 | Portable request policy, recipe handling and shared PCM player in PC simulator | Event matrix, queue/expiry/silence and mixed-source fixtures, word ordering, audible joins and measured tone preservation |
| A2 | Flash library installer, SD-only custom storage and durable index/lifecycle | Frozen schema, no-card/full/interrupted/rollback/reboot tests, no flash spillover and measured built-in budget; no partition changes |
| A3 | Local word selector, file assignment, source/readiness UI and Panel Manager parity | Field setup without computer/model, file upload/preview, rename and permission tests |
| A4 | Withdrawn 2026-10-08 (ADR-052): no text to speech on the panel | - |
| A5 | Authorized hardware qualification and release readiness | Actual-speaker audition, missing/removable-media tests, priorities, offline reboot and 24-hour mixed-load soak |

A0-A3 and A5 are the whole voice feature: the recorded library and imported files. A4, text
to speech on the panel, was withdrawn on 2026-10-08 (ADR-052); its number stays unused.

**Provisional playback targets to measure at A1/A5:** prepared prompts begin within 150 ms
when eligible; audible speech preemption within 50 ms, including the existing roughly 32 ms
DMA queue. Measure preparation separately: one changed label and a full voice-pack install,
including verification. CPU/RAM contention, safe pause and recovery still must pass. Record a failed target/tradeoff before release.

No missed alarm/zone deadlines, tone-timing changes (other than the exit beeps waiting for
speech, AUD-034), watchdog resets, ongoing heap loss or
display-refresh regressions are acceptable. Measure minimum/largest internal and PSRAM
blocks, task stack high-water marks, underruns, cold/warm load and switching peaks with
Z-Wave, Wi-Fi/TLS, UI/saver and storage activity present. Delay pack writes during
OTA; essential tone playback continues. Firmware must retain the 85%
OTA-slot ceiling. Test power interruption only on authorized disposable media; the current
normal-image flashing routine applies, not the historical dark-first procedure.

## Decisions still needed

1. Accepted word joins and the final inventory. Decided 2026-10-08: the voice is Qwen3-TTS's
   designed `designed_female`, chosen by ear from six (`tools/voice/library.json`), and status
   messages are joined from parts with a pause; the codec is Opus 32 kbps. Still open: the
   added words, splitting compounds such as “Living room” into reused words, and the joins by
   ear. (BL-015)
2. Decided 2026-10-08 (ADR-052): no text-to-speech engine on the panel; the production
   computer's engine is chosen by listening in A0. Import conversion bounds must preserve
   quality within measured storage/RAM budgets (BL-015).
3. Exact built-in vocabulary/flash fit (BL-015) and SD quotas/staging reserve (frozen in A2,
   BL-044). Decided in this document ("Storage decision" above): the media split
   puts the supplied library in flash; custom audio requires SD.
4. Z-Wave handoff checkpoint and any change to the proposed first-release event matrix.
   Decided: A0 starts once Z-Wave's Z9 acceptance has passed on a panel
   (a real sensor and the keypad, BL-019, BL-020) and globalization's G3 is done (the language
   setting and locale tags exist, so packs are per locale from the start); the proposed
   first-release event matrix stands. A0 was started on 2026-10-08, ahead of Z9.

Night-time trouble suppression/schedules (Feature 18), spoken life-safety alarm instructions,
other languages, arbitrary incoming TTS, remote panel-link audio distribution and voice on
the 4-inch board are later work, not hidden prerequisites of v1.

## Tasks

### Tones (M2, M4)
- [x] I2S driver + amp control in the BSP (M2)
- [x] Sounder task, patterns and priorities (M2)
- [x] Split the pattern tables and renderer into a portable component (`components/core/sounder`)
      and host-test the T3/T4 timing
- [x] Play the sounder in the PC simulator, so a pattern can be heard without a panel
      (`host/ui_sim/sim_audio.c`)
- [x] Volume and key clicks in Settings > Display and sound, stored per keypad (M4)
- [ ] On the board, check by ear the levels and the power-up pop (AUD-003, AUD-004) and the
      priorities and one-shots over patterns (AUD-001, AUD-005), and run AUD-007's stress test
      (sound stays smooth while the UI is kept busy) (BL-032)

### Voice planning (2026-09-27)
- [x] Reconcile the tone implementation, offline-TTS research and current storage code (planning only)
- [x] Record the three coexisting sources: library words, custom files and on-panel
      TTS prepared into persistent files; define assignments, retention and reboot behavior
- [x] Draft 100 tokens + 12 full messages, recording CSV, provider comparison and calculated
      IMA/Opus pack sizes; no audio production or acceptance claimed
- [x] Shortlist local production voices and define proposed CI provenance/acceptance and
      locale-specific pack/grammar rules; installation, tooling and multilingual packs remain work
- [x] Record local/API generation with approved audio checked into the repo; define per-locale
      bundles from a shared codebase and build-time encoding without synthesis dependencies

### Voice (A0-A5, "Acceptance gates and work order" above)
- [ ] A0: deliver the A0 row of the work-order table (library voice and joins, vocabulary,
      codec, pack budget, locale release contract) with auditions on the production computer
      and measured encoded packs; en-US first, started 2026-10-08 (BL-015)
- [ ] A1: portable request policy and speech playback in the PC simulator (BL-044); the
      library's part is BL-202 below
- [ ] A2: library/import tooling, durable file storage and recovery (BL-044); the library's
      pack and its store are BL-202, imports and SD stay here
- [ ] A3: per-assignment editing, word selection, uploads and browser parity (BL-044); the
      touchscreen editor and the pack uploads are BL-202, the browser editor BL-203

#### First build: spoken zone names from the library ([voice-first-build.md](voice-first-build.md))
- [x] Zone words and voice revisions in the configuration, fixture v6 (BL-202, de6f3f4, 8362659)
- [x] The RVOC reader and the request policy in core `annunciator` (BL-202, de6f3f4, b066644)
- [x] The masters, canonical packets and provenance tracked; `voice.py verify` and `pack` (52b7b2e)
- [x] The Opus decoder and speech in the sounder's mix (BL-202, de6f3f4, 52b7b2e, fd438d1)
- [x] One `assets` store owner for language and voice packs (BL-202, b066644, 152e5bf)
- [x] `panel_voice`: loading, decoding, the announcements and the uploads (BL-202, dfac687, 9b3209f, 83ced69)
- [x] The touchscreen's Spoken name editor and Announcements page (UI-045, AUD-019) (BL-202, 1ab0caf, d3765a0, 0e2be6f)
- [x] The Panel Manager's announcements, voice upload and read-only spoken names (BL-202, 34e74ac, b3f9861)
- [x] Speech in the PC simulator, and the preview's rendered speech checks (BL-202, 37b4b1d, ad8822d)
- [x] Chime, Voice and Beeps volumes instead of one Volume (AUD-032) (BL-207, cdee470)
- [x] A word's ending in the voice pack, played at the end of a spoken name (AUD-031) (BL-202, f1d9e59)
- [x] The Voice volume made on the panel, 3 dB for every 10 % up to +9 dB with a limiter, instead
      of the low-volume clarity stage of 76269c8 (AUD-033) (BL-202, b0793a7)
- [x] The audio bench in development builds (AUD-030) (BL-202, c6a08a2)
- [ ] The exit beeps wait while the panel speaks (AUD-034) (BL-202): built and host-tested; arming
      on a panel and listening is still to do
- [ ] On a bench panel: install the pack, turn voice on, hear the chime, the walk test and the
      status messages (BL-202, needs a listening check): on a panel; on the voice bench pack revision 8
      was chosen - the original takes, each word spoken alone, 60 ms between words, the
      loudness made on the panel (2026-10-10); its chime, walk test and status messages wait for
      their ears
- [ ] The browser's word editor: search, add, remove, reorder, approve and Preview (BL-203)
- [x] Voice packs in the Flasher and its bundles (BL-204, 1bf9d51; docs/features/34 FLS-031)
- [ ] Voice packs from the release manifest (download and selection) (BL-205)
- A4: withdrawn 2026-10-08 (ADR-052), no text to speech on the panel
- [ ] A5: hardware, offline, fault and soak acceptance of voice on a panel (BL-044); the tone
      listening checks are under Tones above (BL-032)
