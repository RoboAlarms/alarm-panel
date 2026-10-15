# Offline TTS for the CrowPanel annunciator

**Current-plan note (2026-09-27):** This is the historical 2026-09-20 engine investigation,
not a selected engine. The current direction combines **recorded word/phrase
selection, imported audio and on-panel TTS into persistent files ahead of use**, selected per
zone or supported system prompt. All use local file playback; TTS is optional for the recorded
routes. Generated files are reused after reboot; playback never waits for TTS and missing
audio retains tones. This supersedes both the original vocabulary-only plan and the later
TTS-only interpretation. Personal/external audio import is now an explicit supported route.
The media split was also fixed: supplied recorded library in internal flash; custom
audio, all TTS models/language data and generated files require SD, with no flash spillover.
This controls persistent storage, not synthesis working RAM or voice quality.
For local/API producers, approved repo recordings and locale release contracts, the local
Qwen3-TTS/Chatterbox/Kokoro shortlist, 112-clip script and IMA/Opus flash budget, see
[built-in library production](../features/08-Audio-and-Annunciation/voice-library.md).
[Feature 08](../features/08-Audio-and-Annunciation/README.md)
owns that lifecycle, proposed scope and work order after Z-Wave;
[audio-storage.md](../features/08-Audio-and-Annunciation/audio-storage.md) owns storage.
Since this research, Feature 29 has implemented the SD worker and file leases; the `assets`
filesystem and all voice playback/generation remain unimplemented. Image sizes, resource
figures and external model results below are dated evidence, not current full-application
measurements. No voice-quality or P4 TTS benchmark was performed in the planning review.
The live/uncached synthesis latency and RTF targets below are historical, superseded by
separate preparation-time and file-playback budgets. Engines slower than real time may be
acceptable if preparation waits and resource use are acceptable; this still needs measurement.

## Listening links and candidate check (2026-09-27)

**Subsequent listening feedback:** all voices presented below sounded bad. None passes the
quality gate, and the prior suggestion to begin by choosing one of them is superseded.
The voice of a commercial alarm keypad was subsequently identified as the accepted
voice-quality reference. Assess candidates with short alarm phrases against that reference,
then assess hardware feasibility. On-panel prepare/persist/play
remains the TTS route; library selection and personal-file import can be implemented without
waiting for a successful engine port.

Commercial alarm keypads let the installer select preprogrammed zone descriptions and hear
the selected descriptor. This confirms a built-in spoken vocabulary; it does not identify the
exact synthesis engine or prove that the voice is produced by a particular recording/synthesis
technology. An original recorded library of complete status phrases and common zone words is
now part of the approved combined direction. It plays entirely on the panel; imports and TTS
cover names outside its vocabulary. The reference does not select a pack or authorize copying
anyone's recordings.

As an optional higher-capacity model comparison, the official
[Kokoro demo](https://huggingface.co/spaces/hexgrad/Kokoro-TTS) offers voices including Heart
and Bella (see its [voice list](https://huggingface.co/hexgrad/Kokoro-82M/blob/main/VOICES.md)).
This is a quality reference/possible source of imported files, not a P4 port recommendation.
The [model card](https://huggingface.co/hexgrad/Kokoro-82M) specifies 82 million parameters:
even one byte per parameter would be about 82 MB before runtime buffers, beyond this board's
32 MiB PSRAM for a conventional resident model. This calculation does not prove every
possible streaming/compression design impossible, but no such usable P4 implementation is
verified here. Pre-generating files changes timing, not that memory budget. No Kokoro voice
has been auditioned/accepted or installed during this check.

The question was where to audition a candidate before putting it on the panel. None is yet
integrated or qualified in RoboAlarms. These are listening/porting candidates, not supported
panel features. Pages/source documentation were inspected; no audio synthesis or subjective
listening test was performed in this check.

- **[sanoTTS browser demo](https://ampixa.github.io/sanoTTS/):** type a phrase and choose
  Synthesize & play. Compare **heart-nano**, **heart** and **amy**. The demo states that
  heart-nano runs the microcontroller build's int8 arithmetic; the other voices may use
  different runtime/model paths. This makes heart-nano a useful first constrained-device
  audition and Amy a quality comparison, not proof that Amy is ready for this panel.
  The upstream [board results](https://github.com/Ampixa/sanoTTS/blob/master/BOARDS.md)
  still list the P4 as build-verified, with English P4 performance not measured there.
- **[Inflect-Nano-v2 P4 sample and deployment](https://huggingface.co/inlanger/inflect-nano-v2-esp32p4-espdl):**
  its model card links an actual P4 capture of “Ready.” It supports raw English text and
  reports one male voice. The measured complete request is about 3.77 s for 0.469 s of
  audio on P4 v1.3/360 MHz/32 MiB PSRAM. This is more relevant now that generation happens
  ahead of use; the old real-time objection is no longer disqualifying. Model size, patched
  runtime/IDF migration, longer phrases and coexistence with the alarm application still
  need evaluation. One short recording is not a voice-quality qualification.
- **[Flite SLT/RMS](https://github.com/festvox/flite):** retain as the conventional C baseline
  (SLT female, RMS male); upstream supports WAV output for a local PC audition. Its official
  voice/sample directory could not be fetched during this check, so no working hosted
  SLT/RMS demo is claimed. Festival or Flite+HTS demos are not identical Flite voice builds.

Before bundling either neural candidate, inspect the exact dependency licenses: sanoTTS's
[documented boundary](https://github.com/Ampixa/sanoTTS#license) separates the MIT runtime
from its GPL text frontend; the Inflect P4 deployment likewise documents separate model and
firmware terms. No licensing/packaging decision has been made.

Suggested audition text: “Armed away. You may exit now.”, “Front door is open.”,
“Upstairs bedroom window.” and an unusual room name. Listen for clarity and pronunciation
as well as pleasantness. Browser speed does not predict P4 preparation speed. Keep the engine
decision open until the actual selected voice and frontend pass Feature 08 A0/A4.

## Original investigation (2026-09-20)

Research date: 2026-09-20. Repository reviewed: `8aafed1`, plus the existing local build configuration and binaries. English is assumed from the requested annunciation and the project's current interface.

## Answer and recommendation

**Yes: the ESP32-P4 can synthesize previously unknown text completely offline. Our board has the necessary processor, memory and audio output. However, two pleasant, clear voices running alongside the entire alarm application are feasible candidates for development, not a capability this research has verified on our panel.**

There are two separate acceptance questions: can a voice pronounce arbitrary text intelligibly, and does it sound pleasant enough to hear every day? A small engine can meet the first without meeting the second. Neither a model's parameter count nor a standalone demonstration establishes that it will coexist with our display, networking and alarm tasks.

My recommendation is to **audition Flite's SLT and RMS voices first**, then benchmark the selected voice builds on this P4 with the full application. Evaluate compact neural TTS as a second candidate if Flite's sound is unacceptable. Do not commit to either engine before listening and measuring. A microSD voice store is a sensible development option if the chosen packages exceed existing flash allocations; it is additional firmware work, not an existing storage service.

If natural neural speech is essential and neither candidate passes on the P4, a separate local computer running TTS is the stronger fallback. That can work without internet, but adds hardware or a LAN dependency. It should remain optional for this project's standalone panel.

This is research only. No engine was installed, no firmware was built or flashed, no device was accessed, and no listening or runtime benchmark was performed. Only this document was created.

## What our particular panel has available

These findings come from the current repository, rather than a generic ESP32-P4 specification.

| Item | Current project evidence | Consequence for speech |
|---|---|---|
| Board | Elecrow CrowPanel Advanced 7-inch, V1.1; reference P4 silicon v1.3 | Evaluate against this hardware revision. |
| CPU | Local `sdkconfig.v1` selects **360 MHz**; two high-performance cores | Do not budget against an advertised 400 MHz or assume one core is unused. |
| Memory | **32 MiB PSRAM**, configured at 200 MHz | Substantial capacity for models, utterances and caches, shared with the UI. |
| Internal memory | Espressif documents 768 KB HP L2 memory, with other small memory regions | This is not 768 KB of free application heap. Stacks, DMA and fast TTS scratch space need separate accounting. [P4 datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.html) |
| Flash | **16 MiB** total; two **4.5 MiB** application slots; **2.5 MiB** `assets` partition | Total flash is not freely available for voices. Keep OTA and other allocated functions. |
| Existing image | Both existing local P4 binaries are **2,363,392 bytes**, approximately 2.254 MiB | Each application slot has about **2.246 MiB** remaining in these artifacts, before adding TTS. This is not an asset partition or a fresh build result. |
| Audio | I2S1, **32 kHz, 16-bit stereo**, two NS4168 amplifiers and speakers | Speech can use the present audio hardware; no new DAC is required. |
| Audio buffering | 10 ms sounder blocks; four 256-frame DMA buffers, approximately 32 ms at 32 kHz | Speech must feed the existing audio owner and remain interruptible. |
| Display | 1024 x 600 RGB565, one scanout buffer and two full-screen LVGL draw buffers | Approximately **3.516 MiB** for those three pixel buffers alone. |
| Screen saver | A decoded full-screen RGB565 photograph is retained in PSRAM | Approximately another **1.172 MiB**; about **4.688 MiB** combined pixel storage, excluding UI objects and other allocations. |
| Networking | C6 handles radio through ESP-Hosted SDIO; P4 still runs host networking and application protocols | The C6 does not eliminate the P4 cost of TLS, MQTT or the Home Assistant link. |
| microSD | Hardware documented; no SD mounting or LittleFS mounting implementation found in the application/components searched | A partition label or card socket alone does not provide a usable voice filesystem. |

The partition also reserves space for C6 updates, firmware relay/cache, event log and crash dumps. I do not recommend repurposing these as a shortcut. Repository guidance says changing the partition layout requires an erase/reflash with loss of stored configuration and logs.

The source includes alarm logic, wired zone processing, LVGL/touch, sound generation, logging, Wi-Fi/time, MQTT/TLS, OTA, a Home Assistant link and a home-control registry. Project notes distinguish newer code that builds from the firmware already exercised on hardware. A future TTS test must use the **intended complete application**, including those newer services.

No current CPU-utilization profile, minimum free heap or largest PSRAM block was measured. Subtracting the pixel buffers from 32 MiB does **not** establish the remaining usable memory budget.

## Engine choices and strength of evidence

| Option | Offline arbitrary English text? | Selectable voices and sound | Assessment for this project |
|---|---|---|---|
| **Flite** | Yes | SLT female and RMS male are concrete audition candidates | First conventional candidate; porting and full-load measurements still required. |
| **eSpeak NG** | Yes | Configurable voices; deliberately synthetic sound | Compact fallback/reference, unlikely to satisfy the desired naturalness. |
| **sanoTTS** | Possible with a complete on-device text frontend | Several English models exist; tiny MCU models differ from larger quality models | Promising experiment; English P4 performance and two suitable voices remain unverified. |
| **Inflect-Nano-v2 P4 deployment** | Demonstrated on P4 | Released voice/model route; no demonstrated two-voice panel configuration | Strong feasibility evidence, but its measured delay and storage are poor fits here. |
| **Espressif ESP-SR TTS** | Not for English in current documentation | Chinese speech synthesis | Does not meet the assumed language requirement. |
| **Piper on a local companion** | Yes, once installed locally | Broad voice selection | Alternative if added hardware/server dependence is acceptable. |

**Flite.** The upstream C engine supports complete text synthesis and externally loaded voice files, including US English SLT and RMS. Its small default KAL diphone voice is cheaper computationally but more robotic; do not use KAL's speed as evidence for the better-sounding candidates. Voice choice must precede performance conclusions. [Flite upstream](https://github.com/festvox/flite)

An existing ESP32 port demonstrates local synthesis and I2S playback, but targets **ESP-IDF 3.2.2**, uses KAL, and explicitly notes flash limitations for other voices. Our **ESP-IDF 6.1 / RISC-V P4** integration is therefore a porting task, not a dependency we can simply enable. Its streaming example is useful architectural evidence, not a benchmark of SLT/RMS under our workload. [ESP32 Flite port](https://github.com/alkhimey/esp32-flite)

The exact selected SLT/RMS package sizes and expanded heap usage were not verified: upstream's packed-voice download directory could not be fetched during research. Do not assume two voices fit in 2.5 MiB, or that file size equals loaded RAM. Flite's upstream runtime license is permissive; retain notices and check the chosen voice files' terms separately. [Flite licensing](https://github.com/festvox/flite/blob/master/COPYING)

**eSpeak NG.** Its own documentation describes compact formant synthesis, multiple voice characteristics and clear speech that is less natural than larger recorded-speech-based synthesizers. This supports it as a functional fallback, not a promise of the sound quality requested here. [eSpeak NG upstream](https://github.com/espeak-ng/espeak-ng)

**sanoTTS needs careful separation of results.** The project lists English voices such as Amy and Kristin, a small int8 MCU model, and a newer nano family. The better-quality desktop/browser voices are not interchangeable with the benchmarked MCU voice. Its maintainers distinguish the MIT inference runtime from the GPL-covered text frontend/project; including the complete eSpeak-based frontend introduces a licensing decision for this Apache-2.0 project. [sanoTTS overview and license boundaries](https://github.com/Ampixa/sanoTTS)

The dated board-results page reports **RTF 0.383 on an ESP32-S3** for a particular nano model using Xtensa SIMD, versus **1.583** for scalar C. It lists **P4 as build-verified**, not as a measured English result. RTF means synthesis seconds per second of output; less than one is faster than playback. Neither the S3 instruction-specific speed nor its memory placement transfers automatically to P4. [sanoTTS board results](https://github.com/Ampixa/sanoTTS/blob/master/BOARDS.md)

The Arduino documentation says its API takes **phoneme IDs, not raw text**, with on-chip eSpeak integration separate. It gives **679,832 bytes of model weights** for the R7 package; that excludes the complete text frontend and other application costs. That documentation also contains older build/SIMD statements inconsistent with the newer board-results page. Pin a revision and test the actual code, model and frontend together. Precomputing phonemes on a PC would not meet arbitrary new text entirely on the panel. [sanoTTS Arduino integration](https://github.com/Ampixa/sanoTTS/blob/master/arduino/README.md)

A separate **Japanese** sanoTTS derivative reports P4 speech at **RTF 0.834 at 400 MHz** with P4 SIMD and a display. This shows the potential of compact neural synthesis, but uses a different language, model, frontend, clock and application. It is not an English two-voice result for our 360 MHz panel. [Japanese P4 implementation](https://github.com/nnn112358/SanoTTS-jp-Tab5)

**Inflect provides unusually relevant P4 evidence.** Its model card describes raw-text English synthesis on **P4 v1.3 at 360 MHz, 32 MiB PSRAM and 16 MiB flash**. For “Ready.” it reports **0.469 seconds of audio**, **1.656 seconds for learned stages**, and **3.767 seconds for the complete measured request**. Those are RTF 3.53 and 8.03 respectively. The decoder produces a complete buffer. This proves offline English synthesis is possible, while also showing why “runs on P4” is insufficient for prompt annunciation. [Inflect P4 measurements](https://huggingface.co/inlanger/inflect-nano-v2-esp32p4-espdl)

The release's two model packages total **9,755,264 bytes, approximately 9.303 MiB**, before frontend/support data. That exceeds our assets allocation and either app slot. It also requires a patched ESP-DL and a pinned IDF 6.0.2 environment, rather than our IDF 6.1 build. I would not choose this release as the first integration candidate. [Artifact sizes](https://github.com/inlanger/esp32-p4-inflect-tts/blob/main/artifact-manifest.json), [runtime requirements](https://github.com/inlanger/esp32-p4-inflect-tts)

**Espressif's own TTS** still explicitly documents Chinese-only synthesis. English command recognition in ESP-SR is a different capability. [ESP-SR P4 speech synthesis](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/speech_synthesis/readme.html)

**Full Piper belongs in the companion comparison.** Piper is a local neural engine; one representative Lessac medium ONNX voice is **63.2 MB**, already larger than our entire PSRAM. That single example is not a size claim about every model, but it rules out treating ordinary Piper voice downloads as small ESP-IDF assets. A Linux companion could synthesize locally and send audio to the panel without internet; a direct wired connection could also avoid reliance on a router. This is an architectural fallback, not a verified implementation here. [Piper](https://github.com/OHF-Voice/piper1-gpl), [Lessac model files](https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/lessac/medium)

## Proposed integration if development proceeds

The following is a design recommendation, not existing functionality or measured performance.

1. **Keep synthesis outside the alarm and UI callbacks.** Events enqueue bounded speech requests containing text, voice ID, priority and an expiry/state token. A background worker performs text normalization, pronunciation and synthesis. It must never hold the engine/UI lock while synthesizing or reading a voice file.
2. **Keep one owner of I2S and amplifier control.** Extend the sounder/audio path to consume buffered speech. A second TTS task must not independently write the same I2S channel. Retain the current 32 kHz stereo output initially; resample each voice's native output correctly and duplicate mono samples to both speakers.
3. **Give alarm sounds priority.** Fire, CO, burglary and auxiliary alarm patterns preempt speech. Entry/exit timing remains deterministic. Cancel obsolete announcements, such as an unfinished arming message after disarming. Do not let long speech pause the required tone patterns through the existing one-shot mechanism.
4. **Budget background work explicitly.** Current priorities are alarm 10, zones 9, sounder 6 and LVGL 4. Start synthesis below these, use bounded work and blocking waits, and measure starvation and watchdog behavior. Core affinity can be tuned after profiling; it does not reserve memory bandwidth or make an entire core free.
5. **Store two voices; load one when practical.** Share language resources when supported, release the old model before loading another, and handle failed loads without breaking tones. Verify peak memory during switching, not just steady-state memory.
6. **Cache speech generated locally.** At idle time, synthesize common prompts and current zone names for the selected voice. Key the cache by text, voice/version, pronunciation and rate. A renamed zone or entirely new sentence is still synthesized on the panel; the cache is a latency optimization, not a fixed word list. Invalidate it when its inputs change.
7. **Make offline operation complete.** Include the pronunciation dictionary/rules, models and any license material locally. No first-use model download, cloud phonemizer, online authorization or Home Assistant request may be needed. Cold boot with Wi-Fi unavailable must still permit novel text.

Five seconds of 32 kHz mono PCM16 consumes **320,000 bytes**; stereo consumes **640,000 bytes**. A 250 ms mono buffer is **16,000 bytes**. These are calculated audio sizes, not total TTS memory. Streaming reduces buffering only when the chosen synthesis path actually supports it. A whole-utterance decoder must budget its complete output and intermediate tensors.

Voice data on SD could preserve the current partitions. Load or buffer it before playback where practical, avoid small synchronous reads in the audio task, and keep essential tones functional if the card is missing. Flash caching should be bounded and infrequent; use RAM for frequently changing text. Delay optional synthesis/cache writes during OTA or other heavy work.

Custom names and abbreviations need pronunciation handling. For example, expand “CO” to “carbon monoxide” in speech text and provide an optional pronunciation override for unusual room names. Arbitrary text support does not guarantee correct pronunciation of every name, language or symbol.

## Measurements needed before choosing an engine

A useful experiment is small but must answer the actual requirement:

- **Listen first:** compare two distinct candidate voices with “Armed away. You may exit now,” “Upstairs bedroom window is open,” numbers, unfamiliar names, short labels and a new sentence. Listen through the actual speakers at ordinary volume and distance. Rate intelligibility and pleasantness separately; no listening result is claimed here.
- **Measure the complete path:** cold/warm voice load, text frontend time, time to first audible output, total synthesis time, output duration and RTF. Repeat for both voices and uncached text. Do not report neural-kernel time as end-to-end latency.
- **Measure memory:** internal and PSRAM free/minimum heap, largest free blocks, stack high-water marks, model-switch peak and repeated-use fragmentation. Include TLS reconnection and first screen-saver decode.
- **Stress the intended application:** scrolling, touch, screen-saver transitions, wired zone events, MQTT, HA catalog/control activity, log/NVS writes and update checks while speech runs. Verify sensor deadlines, tone timing, audio underruns and display refresh counters.
- **Verify interruption and failure:** trigger each priority alarm during long speech; test rapid door events, stale announcements, corrupt/missing voice data, allocation failure, card removal and power cycling. Speech failure must leave alarm logic and tones working.
- **Verify true independence:** power-cycle with internet and Wi-Fi unavailable, change a zone name, and synthesize text absent from all caches using both voices.

Suggested engineering targets, **not measured results or formal requirements**: common cached prompts start within 150 ms; ordinary uncached short prompts start within 500 ms; sustained synthesis aims for RTF at or below 0.5 under representative load; speech yields to alarm output within 50 ms; no audio underruns, missed alarm deadlines, watchdog resets, display late-frame regressions or continuing heap loss during a 24-hour soak. Whole-utterance engines must meet startup latency as well as aggregate RTF. Adjust the targets explicitly if the chosen quality/latency tradeoff requires it.

The current DMA queue itself accounts for roughly 32 ms of possible output latency, so a 50 ms preemption target requires bounded buffering and verification. Preserve the display's cache-safe ISR and flicker guard. Follow the current CONTRIBUTING.md display-safety and flashing routine: a normal image when flashing is authorized; dark builds/display diagnostics are for requested or suspected refresh investigations, not a mandatory first flash.

## Decision to carry forward

Proceed with a **two-voice audition and bounded prototype**, starting with Flite and retaining compact neural TTS as an evaluated alternative. Choose an engine only after it passes the voice-quality and full-application tests. On current evidence, I would promise **offline arbitrary-text feasibility**, but would not yet promise **two pleasant voices with acceptable latency on the existing complete firmware**.

At the time of this research, Feature 08 specified a prerecorded descriptor vocabulary and
was deliberately left unchanged. The 2026-09-27 planning review supersedes that paragraph:
Feature 08 now specifies recorded word/phrase selection, imported clips and genuine local
synthesis **ahead of use into durable files**. Engine/voice selection remains for audition
and measurement. Local file playback is the required event path for every source, rather
than an optional optimization ahead of event-time synthesis. See its current A0-A5 sequence;
the original recommendations in this investigation do not override that plan.

Repository evidence: `firmware/crowpanel_p4/sdkconfig.defaults`, local `sdkconfig.v1`, `partitions.csv`, existing `build_v1`/`build_v3` application binaries; `components/boards/board_crowpanel_p4/src/board_audio.c` and `board_display.c`; `components/services/platform_esp/src/sounder.c`, `Kconfig`, `zone_service.c` and `alarm_service.c`; `components/ui/alarm_ui/src/ui_saver.c`; `docs/features/README.md`; Features 06 and 08. External links above are primary documentation, upstream repositories or model-author reports, accessed on the research date. Reported external benchmarks have not been independently reproduced here.
