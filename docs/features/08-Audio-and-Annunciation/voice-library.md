# Built-in voice library: recording script and production plan

**Status: planning baseline agreed 2026-09-27; word list extended and engines researched
2026-10-08.** Detailed vocabulary, voice, codec
and resource estimates remain provisional until audition and measurement. This defines A0; it does not
select a voice or claim that any audio has been generated, encoded or tested. The aim is
a commercial touchscreen panel's annunciator quality. Direction (decided 2026-10-08, ADR-052): the
speech is made on the production computer with a text-to-speech model (or through a TTS API),
never on the panel, and only the accepted recordings ship, never a model. American English
(en-US) is the first and, for now, only voice; other languages' recordings will come with their
language packs, later. Generation in GitHub Actions is optional. A0 started 2026-10-08; the
rest of the implementation remains after Z-Wave. [Feature 08](README.md) owns event/permission rules;
[audio-storage.md](audio-storage.md) owns media and recovery.

## Recommended first pack

One consistent American English (en-US) voice, **221 selectable tokens, 10 status parts and
6 walk-test parts** (237 clips).
The exact editable production list, IDs and duration assumptions are in
[recording-script.csv](recording-script.csv). IDs are proposed semantic IDs, not filenames.
This is a draft to audition, not a closed vocabulary commitment before listening.

| Group | Count | Record these tokens |
|---|---:|---|
| Position/modifier | 31 | Front, Back, Side, Left, Right, Upper, Lower, Upstairs, Downstairs, Inside, Outside, North, South, East, West, Main, First, Second, Third, Rear; *added:* Center, Middle, Top, Bottom, Fourth, Interior, Exterior; *researched:* Detached, High, Low, Service |
| Rooms/places | 77 | Basement, Attic, Garage, Kitchen, Bedroom, Bathroom, Living room, Dining room, Family room, Laundry room, Utility room, Hallway, Entry, Foyer, Office, Den, Closet, Pantry, Guest, Primary, Master, Patio, Porch, Deck, Balcony, Driveway, Shed, Workshop, Storage, Sunroom, Mudroom, Pool, Yard; *added:* Hall, Stairway, Loft, Library, Study, Nursery, Playroom, Game room, Media room, Bonus room, Great room, Gym, Cellar, Crawl space, Furnace, Mechanical room, Breezeway, Carport, Barn, Studio, Apartment, Lobby, Garden, Spa, Entrance, Suite, House; *researched:* Area, Baby, Courtyard, Kids, Level, Powder, Shop, Stairs, Theater, Wing, Roof, Ceiling; *forums:* Greenhouse, Coop, Well, Spare, Landing |
| Objects/types | 84 | Door, Window, Gate, Motion, Sensor, Glass, Smoke, Heat, Water, Leak, Freeze, Carbon monoxide, Zone, Room, Floor, Sliding, Overhead, Glass break; *added:* Doors, Windows, French, Skylight, Hatch, Pet, Tilt, Detector, Pendant, Button, Keypad, Key fob, Panic, Medical, Fire, Emergency, Flood, Temperature, Vibration, Gas, Safe, Cabinet, Freezer, Sump pump, Water heater; *researched:* Boiler, Contact, Dishwasher, Drawer, Gun, Jewelry, Liquor, Medicine, Refrigerator, Shock, Sink, Toilet, Tool, Valve, Washer, Fence, Sprinkler; *forums:* Mailbox, Dryer, Fridge, Chest, Air conditioner, Pump, Septic, Tank, Bay, Aquarium, Doorbell, Presence, Pipe, Overflow, Server, Hot tub, Screen, Storm, Cat, Dog, Box, Litter; *requested:* Network, Internet |
| Numbers | 29 | Zero through Twenty individually; Thirty, Forty, Fifty, Sixty, Seventy, Eighty, Ninety, Hundred |

The 59 words marked *added* (2026-10-08) are a proposal to strike or extend:
with no speech made on the panel, these words carry every zone name the library can say. They
cover the walk test too, which speaks every zone's name (docs/features/40 W6), panic, medical
and fire buttons included. The `notes` column of the script holds en-US pronunciations where a
model may pick another (Foyer, Garage).

The 33 words marked *researched* (2026-10-08, also to strike or extend) come from
the lists installers pick zone names from on commercial panels that speak them. Kept: words homes name zones with that our list
couldn't say, alone or joined (Powder + Room, Medicine + Cabinet, Water + Valve, Kitchen + Sink + Leak). Left out,
with the reason:

- Alarm, trouble and menu words (Alarm, Trouble, Bypass, Battery, Tamper, Police, Silent): this
  release speaks no alarm or trouble events.
- Shops and offices (Building, Factory, Warehouse, Store, Dock, Loading, Cash, Vault, Reception):
  for a later commercial pack.
- Family names (Son, Daughter, Boys, Girls, Mother's, Father's, Maid's): Kids, Guest and Primary
  cover them; a name goes in a custom recording on SD.
- Smart-home devices that aren't zones (Light, Lights, Fan, Lock, Thermostat, Siren, Doorbell,
  Camera): automations name them on the screen.
- Ordinals past Fourth (Fifth to Ninth), and words a join already says (Swimming: Pool; Fire
  detection: Fire + Detector; Telephone, Phone).
- Rarer ones: Wine, Art, Painting, Computer, Desk, Money, Sun, Chest. Of this group
  Roof, Ceiling, Fence and Sprinkler were kept.

The 27 words marked *forums* (2026-10-08, picked) are what people on the Home
Assistant forums name their sensors, from
[area naming](https://community.home-assistant.io/t/areas-examples-how-did-you-define-you-areas/559900),
[mailbox sensors](https://community.home-assistant.io/t/sensor-to-check-if-mail-is-in-the-mailbox/320024),
[leak sensors](https://community.home-assistant.io/t/water-leaks-and-shut-off-valve-management-looking-for-inspiration-ideas-examples/417480),
[vibration sensors](https://community.home-assistant.io/t/what-are-the-use-cases-of-xiaomi-aqara-vibration-sensor/203223),
[septic](https://community.home-assistant.io/t/septic-system-alert-monitoring-system-project/687477),
[sump and well pumps](https://community.home-assistant.io/t/well-pump-controller-design-help-float-switch-and-water-meter-cistern/632446),
[chicken coops](https://community.home-assistant.io/t/chicken-coop-door-brainstorming/601641)
and Home Assistant's own [binary sensor classes](https://www.home-assistant.io/integrations/binary_sensor/).
Screen, Storm (doors) and Landing are common American words the forums didn't show; Cat, Dog,
Box and Litter are our own, for pet doors and litter boxes. Not taken: British words, for a
later en-GB pack (Wardrobe, Ensuite, Lounge, Letterbox, Conservatory); beds, chairs, rain gauges
and meters, which aren't zones; and Moisture, Cold and Occupancy, Home Assistant's names for
what Leak, Freeze and Motion say.

Multiword entries such as “Living room” are **one recorded token**, so the natural join is
preserved. Reuse “Door” across labels instead of storing “Front door”, “Back door”, etc.
Examples: Front + Door; Upstairs + Guest + Bedroom + Window; Garage + Overhead + Door;
Basement + Water + Leak; Zone + One + Hundred + Twenty + Eight. Digits or number words are
UI choices; the recordings are spoken words. This number set supports conventional American
English labels through 999, within the eight-token recipe bound for simple zone-number labels.
Larger/unusual identifiers can use explicit digit sequences within the same bounds or SD audio;
the runtime must not assume the current default of 128 zones is permanent.

The vocabulary names sensors; it does not enable new spoken alarm/trouble events. Personal
names, uncommon rooms and additional simultaneously installed languages/voices belong in SD
extensions or custom files. Future official localized packs can replace the supplied flash
pack within the same budget; English is the first inventory, not a permanent format constraint.
Do not spend the first pack's budget on every possible future feature.

The system messages are **joined from parts**: the words of one phrase 60 ms apart (from pack
revision 8; 100 ms in revision 7, 40 ms in revision 6, 60 ms before it), the pack's word gap (“Armed Away”), and the pack's pause (·, 300 ms) only before a new sentence. Decided
2026-10-08: flash is tight, and the joined parts sound as good as the sentence recorded whole;
2026-10-09: 300 ms between “Armed” and “Away” sounded like two sentences, so the words of
a phrase now run together. A message nothing else shares stays one recording. Ten recordings
make the twelve status messages:

| Semantic target | Played as |
|---|---|
| Arming Away with audible exit | Arming Away · You may exit now. |
| Arming Home with audible exit | Arming Home · You may exit now. |
| Arming Night with audible exit | Arming Night · You may exit now. |
| Armed Away | Armed Away |
| Armed Home | Armed Home |
| Armed Night | Armed Night |
| Armed Away, no entry delay | Armed Away · No entry delay. |
| Armed Home, no entry delay | Armed Home · No entry delay. |
| Armed Night, no entry delay | Armed Night · No entry delay. |
| Disarmed | Disarmed. |
| Entry starts | Please disarm the system. |
| Not-ready refusal | Not ready. Check the zones. |

The walk test's messages (WALK-016, [docs/features/40](../40-Walk-Test/README.md)) are made the
same way, six recordings for five messages:

| Semantic target | Played as |
|---|---|
| Walk test starts | Walk test Started |
| Walk test ends | Walk test Ended |
| Five minutes left | Walk test · Ends in five minutes. |
| Summary, every zone seen | All zones tested. |
| Summary, some zones not seen | Some zones were not tested. |

The parts are `status.*` and `walk.*` rows of the script, kind `part`: cut from their own
phrases like the words (“Short words” below), but not offered for zone names.

Having a clip does not authorize playing it: silent exit, accepted state transitions,
duress concealment and alarm priority still follow Feature 08. “Home” matches current panel
terminology. No boot welcome, user names, PINs, alarm voice loops or countdown recordings.

## Production format and fit

Keep the best available original export and a lossless PCM editing master on the development
computer. Prefer native-rate PCM/WAV when the provider offers it; save its actual provenance.
Converting an MP3 download to WAV does not restore lost detail. Do not repeatedly transcode.
The shipped pack is compressed separately; a WAV container can contain PCM **or** ADPCM.

**First candidate: mono 16 kHz, 4-bit IMA-ADPCM (the WAV/DVI variant).** It has a simple
block decoder and nearly four-to-one reduction against PCM16 at the same rate. Use a fixed
256-byte block for the initial comparison; each mono block represents 505 frames. Preserve
the exact valid frame count so block padding does not add an audible gap after every word.
This is a candidate to audition, not a claim about any other product's codec.
The IMA block arithmetic is independently checkable against
[FFmpeg's encoder](https://ffmpeg.org/doxygen/8.0/adpcmenc_8c_source.html).

The CSV assumes 0.75 s per single-word token or part, 1.10 s for each of 18 multiword tokens,
12.85 s for the 10 status parts and 7.50 s for the 6 walk-test parts: **192.40 s** for 237
clips (137.70 s before the researched and forum words and the walk test; 105.95 s before the 2026-10-08
words). These are planning estimates, not
instructions to rush speech or trim consonants to hit a time limit.

**Measured (2026-10-08, the chosen voice, `voice.py record` and `encode`):** 161.6 s of speech in
the 237 clips (status parts 9.7 s, walk-test parts 6.5 s), trimmed to 30 ms before and 80 ms
after each. Opus at 32 kbps (libopus, VBR, mono 16 kHz, 20 ms frames) stores them in
**588,884 bytes (575 KiB)** as raw packets with a 2-byte length each: 311 KiB of the 950 KiB
budget ([audio-storage.md](audio-storage.md)) is left after the 64 KiB headroom. The 33 researched words, the 27 forum words and the walk
test's 6 parts add 44.9 s and 159 KiB of that; Network and Internet 1.3 s and 4 KiB. The codec is chosen by ear; the takes are not yet accepted.

| Encoding | Estimated bytes at the planning seconds | Practical consequence |
|---|---:|---|
| PCM16 mono 16 kHz | 6,156,800 payload (~5.87 MiB) | Exceeds even the entire assets reservation |
| IMA-ADPCM mono 16 kHz | 1,589,132 (~1.52 MiB), including assumed headers/block rounding | Over the 950 KiB budget at the planning seconds |
| IMA-ADPCM mono 12 kHz | 1,199,756 (~1172 KiB), same assumptions | Over by ~286 KiB with the headroom, at reduced bandwidth |
| Opus mono 24 kbps | 577,200 payload (~564 KiB), plus framing | Measured: 448,126 bytes (438 KiB), 448 KiB left |
| **Opus mono 32 kbps (chosen)** | 769,600 payload (~752 KiB), plus framing | Measured: 588,884 bytes (575 KiB) stored, 311 KiB left. Chosen by ear, 2026-10-08 |
| LC3 mono 32 kbps (Bluetooth LE Audio) | 769,600 (~752 KiB), fixed 40-byte frames | Measured: 655,960 bytes (641 KiB), 245 KiB left. Google's liblc3 (Apache-2.0) builds for any chip; LC3's patents are licensed to Bluetooth products, so use in this panel needs a licence check before shipping |

IMA calculation per clip: `60 + 256 * ceil(16000 * planning_seconds / 505)` bytes, summed
over 237 clips. The 60-byte WAV header is a canonical packing assumption, not a promise about
arbitrary encoder exports. Allow **64 KiB additional pack/index/notice headroom**: at 16 kHz IMA
that makes 1,654,668 bytes, 681,868 over the 950 KiB budget. Filesystem overhead and the
previous/staged generation remain covered by the separate storage plan, which still needs
measurement. The real gate is the complete artifact's bytes and listening acceptance: change the codec or
the inventory, never slow, rush or trim the speech to fit.

**The codec is Opus at 32 kbps** (chosen by ear, 2026-10-08, over 24 kbps and LC3 at
32 kbps; `tools/voice/library.json` holds the setting). It is a supported upstream
speech/music format with a permissive reference implementation, not a neural model running
on the panel. See [official listening examples](https://opus-codec.org/examples/) and
[license](https://opus-codec.org/license/). For the first comparison use mono 16 kHz decoded
output, then the existing 32 kHz output conversion. The
[decoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html) supports
16 kHz but not direct 32 kHz decode. Preserve pre-skip/end trim and reset state at independent
clip boundaries; concatenating encoded packets blindly can lose beginnings or create gaps.
If 16 kHz noticeably changes the accepted voice, also audition higher-bandwidth Opus rather
than assuming stronger compression always sounds worse than ADPCM.

Espressif offers an Opus decoder (`esp_audio_codec`, every ESP32 target), but its
[2.6.0 changelog](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.0/changelog)
requires P4 revision >=3.0 for assembly optimizations, and 2.6.2 (the latest, checked
2026-10-08) adds a CMake check that refuses a P4 below 3.0 outright: it can't serve the v1.x
panels. Do not assume a version works on both supported silicon revisions or substitute
upstream benchmark numbers for this app. Compare a portable
libopus decode-only build or a verified compatible codec release, including license, firmware
size, CPU, memory and cancellability. Encoding happens on the production computer. No codec
dependency has been installed or selected by this research.

Prefer one immutable flash `library.bin` plus an index/manifest instead of 235 filesystem
files. The index maps token/phrase IDs to bounded byte offsets, lengths, format, valid frames
and hashes; each clip remains independently decodable. Individual WAVs are convenient editing
and test fixtures, not a requirement for the flash layout. Packing avoids repeated tiny-file
allocation waste; it does not remove filesystem overhead. Validate out-of-range/overlapping
offsets and arithmetic overflow before activation. Keep the active and replacement packs
within the existing 1 MiB + 1 MiB + 512 KiB-reserve plan. No partition change.

This supplied, measured production pack is allowed in flash even when its recordings were
created using an external TTS service. User imports still require SD. The distinction is the controlled shipped library, not whether a human or a
computer originally spoke the words. Alternate voices are SD packs unless a future fixed
flash inventory is explicitly budgeted; do not promise two full flash voices.

## Production engines (2026-10-08)

The engine runs on the production computer, and only its accepted output ships: an NVIDIA GPU
with 16 GB and 32 GB of RAM run every candidate below, so its size and speed hardly matter. Two
things do: how the voice sounds against the quality reference, and whether the licence lets its output
ship in this Apache-2.0 firmware, which anyone may sell. Listening decides; this is the
shortlist, checked against the model cards and licences on 2026-10-08.

| Engine | Licence | Voices | Why audition it |
|---|---|---|---|
| **Qwen3-TTS 1.7B** (Alibaba, 2026-01): CustomVoice, VoiceDesign and Base | Apache-2.0 | Presets **Aiden** and **Ryan** (American English, male); or an original voice designed from a description, then reused for every clip through Base's voice-clone prompt | First choice: style instructions ("calm, clear, even pace"), one frozen voice for all clips, batch generation; `pip install qwen-tts`, FlashAttention optional |
| **VoxCPM2** (OpenBMB, 2026) | Apache-2.0 | A voice designed from a description, or a reference clip | 48 kHz output, about 8 GB of GPU memory, 30 languages (useful for the later language packs); `pip install voxcpm` |
| **Kokoro-82M** v1.0 | Apache-2.0 | **af_heart**, **af_bella** (US female), **am_michael** (US male) and more | Baseline: tiny and fast, yet rated with models fifty times its size (1065 on the open-weights speech arena); its own notes warn that very short inputs can suffer |
| Chatterbox (Resemble AI) | MIT | A reference clip, or its default speaker | Fallback, rated below Kokoro (1028) |

Rated higher but not used, because their licences don't let the output ship in this firmware:

| Model | Arena rating | Licence |
|---|---|---|
| Breeze TTS 2 (BreezeBlue) | 1222, first | Research and non-commercial; it names self-hosted output as non-commercial too |
| Fish Audio S2 Pro | 1116 | Fish Audio Research License: the output is yours, but using the model to make or run a product is a commercial purpose that needs a paid licence. If it clearly wins a listening test, Fish Audio sells one |
| Voxtral TTS (Mistral) | 1085 | Its voices are CC BY-NC 4.0, which the model inherits |
| Higgs Audio V3 (Boson AI) | 1038 | Research and non-commercial; putting it in a product needs a licence (its free grant covers podcasts and videos) |
| F5-TTS | - | CC BY-NC 4.0 weights |

Also passed over: Step Audio EditX (Apache-2.0 code, but the weights' licence isn't stated, it is
tested on Linux only and needs a reference clip), NVIDIA Magpie-Multilingual 357M (NVIDIA Open
Model License; five conversational English voices; runs in NeMo) and Supertonic 3 (OpenRAIL-M,
whose use rules include saying that content is machine-made). Qwen's newer Qwen-Audio 3.1
(2026-09) is a hosted API only. Hosted APIs stay eligible ("API/hosted production alternatives"
below).

**Short words.** These models generate speech token by token, and a word on its own can come
out clipped, rushed, silent or with an extra sound (reported for single words on other TTS
systems); it also ends the way a sentence does, so a name joined from such words sounds like a
list (heard on the first build). So the tooling speaks each word inside a short carrier
sentence - a short name, “___ door.” for a position, “Back ___ door.” for a place, “Back ___ two.”
for an object, “Zone ___ door.” for a number, the status parts in their own phrases (“Armed
away.”) - cuts it out by forced alignment (torchaudio's MMS_FA) at the quiet point before the next
word (the closure of the “d” in “door” or the “t” in “two” makes one), and checks every cut with
Whisper; the sentences (“You may exit now.”) are spoken as written. The cut is recorded with the
clip, so `record --reproduce` repeats it ([tools/voice/README.md](../../../tools/voice/README.md)
“The carriers”). This was chosen on 2026-10-09 (pack revisions 4-6); revision 7 went back to
each word spoken alone (“Revisions 7 and 8” below).

**Word forms (AUD-031).** A word sounds different inside a name (“BACK patio motion”: short, its
pitch going on) from at its end (“back patio MOTION”: longer, falling). Spoken in a short name, a
word's own clip is the first form; the voice also has each word's ending, cut from the end of an
announcement after “Back deck” (“Not ready. Check the zones. Back deck motion.”; the “k” leaves a
clean place to start the cut). The panel plays a spoken name's last word as its ending
([voice-first-build.md](voice-first-build.md) D08, vocabulary schema 2). Each form is spoken with
eight seeds; the take kept is one Whisper hears as written, starting cleanly (a cut that starts
mid-sound plays choppy), whose length is nearest a share of the word's length spoken alone (a
word inside a name 0.6-0.75 of it with one syllable, 0.72-0.82 with more, an ending 0.85-1.0).
The forms were chosen by ear on the panel's audio bench (2026-10-09), with 40 ms between a
name's words. `voice.py forms` makes them
([tools/voice/README.md](../../../tools/voice/README.md) “Word forms”).

Both forms of every word don't fit the voice's room at those lengths. The pack holds at most
966,656 bytes of payload (948 KiB signed, [audio-storage.md](audio-storage.md)); at 32 kbps a
second of speech takes about 3.9 KB, and the header, index, strings and the 16 status and
walk-test clips take about 100 KB, which leaves about 221 s for the 442 words and endings.
Their lengths' targets come to 222 s at their low ends and 242 s in their middles (2026-10-09,
measured on the first takes). So the pick keeps the pack under the limit less 8 KiB by taking
shorter takes, heard as well and starting as cleanly, where they cost least: the cost of a take
is the square of how far it falls under its target's middle, as a share of it, so long words
lose a little before a short one loses as much. Every word and ending stays (ADR-046). More room
would come from a larger voice budget, taken from the asset packs' planned share of the store,
or a lower bitrate (the reader accepts 32 kbps only): both are the maintainers' decision.

**Revisions 7 and 8 (2026-10-10).** Listening on a panel, the panel joining the words from its own pack
on the voice bench (AUD-030), the original takes were chosen - each word spoken alone, as
recorded before the carriers - over the word forms: an announcement has to be understood at
once from another room, so the clearest words beat the more natural joins, and both forms of
every word filled the voice's room (revision 6's payload was 966,656 bytes). The words are 60 ms
apart (revision 8; 100 ms in revision 7), and the pack carries them clear - each levelled to -15 dB of voiced RMS, the gain capped at
a -1 dBFS peak, nothing limited, their quiet ends trimmed to 10 ms and 20 ms - while the panel makes
them louder with its Voice volume, 3 dB for every 10 % (AUD-033). The pack is 593,920 bytes
signed, under the goal of about 600 KB, which leaves room for more words. The carriers
and the word forms stay in the tooling as research.

**One voice for every clip.** A preset keeps its voice by itself. A designed voice changes with
each generation, so design it once, accept one reference take and its transcript, and make every
clip from that reference (Qwen's Base voice-clone prompt, VoxCPM2's reference clip). Keep the
reference recording, not just the prompt.

Suggested **original design prompt**, to audition rather than assume it works:

> An adult American English female announcer. Calm, confident and matter-of-fact. Clear
> consonants, steady moderate pace, restrained pitch movement and firm sentence endings.
> Clean studio recording, without whispering, breathiness, dramatic acting or sing-song delivery.

Make a male version too if wanted. The aim is intelligibility and assurance, not a booming voice.
For preset voices, use the same restrained style instruction for every clip. None of these model
licences gives rights to an unrelated person's recorded voice: never clone a real person,
a celebrity or another product's recordings, and keep the selected voice's source with the pack.

Sources: [Qwen3-TTS](https://github.com/QwenLM/Qwen3-TTS) and its
[model card](https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-CustomVoice),
[VoxCPM2](https://huggingface.co/openbmb/VoxCPM2),
[Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M) and its
[voice notes](https://huggingface.co/hexgrad/Kokoro-82M/blob/main/VOICES.md),
[Chatterbox](https://github.com/resemble-ai/chatterbox); the ratings from Artificial Analysis'
[open-weights speech arena](https://artificialanalysis.ai/text-to-speech/leaderboard/provider-voice/open-weights)
(undated, newest entry 2026-08); the licences of
[Breeze TTS 2](https://huggingface.co/BreezeBlue/Breeze-TTS-2),
[Fish Audio S2 Pro](https://huggingface.co/fishaudio/s2-pro/blob/main/LICENSE.md),
[Voxtral TTS](https://huggingface.co/mistralai/voxtral-4b-tts-2603) and
[Higgs Audio V3](https://huggingface.co/bosonai/higgs-audio-v3-tts-4b); Hugging Face's
[Open TTS Leaderboard](https://huggingface.co/blog/open-tts-leaderboard) (2026-09-30), where
Kokoro-82M, Supertonic 3 and Fish S2 Pro lead on English word errors.

## Production, repository and build contract (planned)

Provide a batch authoring tool supporting a selected local model or API provider, with
optional future GitHub Actions generation. Voice quality and language coverage take priority
over fitting a hosted runner. Inputs are a reviewed locale script, voice profile, recorded
producer configuration and encoding profile; outputs are lossless
masters, listening samples, compressed library/index and a machine-readable quality/size
report. Source adapters allow replacing the producer without changing panel playback.
Installation would use an isolated environment; do not add PyTorch/model packages to the
ESP-IDF Python environment. The production computer is a Windows PC with a CUDA GPU; each engine's
Windows (or WSL) setup and actual memory/runtime are recorded when it is installed. Successful
GitHub-hosted generation is not an A0 acceptance condition.

The current host and firmware workflows run on `ubuntu-latest` without a configured GPU.
Normal CPU CI can validate/repack already accepted assets without loading a speech model.
A separate generation job may be added later if useful; runner eligibility, resource needs
and cost would need qualification then. There is no selected or required GPU runner and no
requirement to turn a PC into a self-hosted runner.
Synthesis should run when source text, a voice profile or the producer changes, not on every
firmware build. A failed synthesis job must not replace the last accepted library.

For local generation, pin model/tokenizer/dictionary files by revision and hash, dependency/container versions,
locale, pronunciation settings, voice/reference hash, prompt, seed, sampling options and
postprocessing/encoder versions. Provision all resources first and test with network disabled;
`from_pretrained` defaults must not silently fetch missing components during an offline run.
Record the actual inference backend and dtype as part of provenance.
For API generation, retain provider/model/voice identifiers, request settings, generation date,
available version identifiers and source/export format. An API may change behind a model name;
the approved file hash is the authoritative build input. Credentials stay outside the repo
and release build, supplied only to the authoring tool when generation is requested.

Seeds do not guarantee identical waveforms across machines or package versions; see
[PyTorch reproducibility](https://docs.pytorch.org/docs/2.14/notes/randomness.html). Preserve
accepted lossless takes as versioned repository files. The normal pipeline verifies hashes and
builds from those takes; a newly synthesized result is a candidate requiring listening
acceptance before becoming the release source. Pin encoding/metadata/order for repeatable
packing and test that separately from inference. Do not silently regenerate every accepted
word when changing an unrelated firmware file.

Automated gates check expected IDs, valid nonempty audio, decoded duration/sample rate,
clipping/silence, exact pack bytes, required locale coverage and metadata/provenance. Optional
transcription/forced alignment helps flag missing/repeated words but is not a substitute for
listening, especially on single-word clips. Produce a playlist of all changed clips and
representative joins. Budget original takes/context outside flash; only accepted clips ship.

### Checked-in audio and language releases

**Tracked audio (BL-202, D13 in [voice-first-build.md](voice-first-build.md)).** The first voice
is tracked in `assets/voice/en-US/qwen3-designed_female/`: `reference.wav`, the 237 script
masters (PCM16 mono 24 kHz, as recorded), each clip's canonical Opus packets
(`packets/*.opuspkt`, encoded from the master at the speaker stage's level, `tools/voice/speaker.py`), `voice.json` (numeric IDs, seeds, hashes, trim, listening review, the
payload hash of each pack revision), `provenance.json` and `NOTICE.md`. The numeric IDs live in
recording-script.csv's `numeric_id` column. `voice.py export` writes the folder from a
recording; `voice.py verify` and `pack` check it and build the RVOC pack from it with the
standard library alone, so any clone rebuilds the identical payload; `encode --reproduce` and
`record --reproduce` compare the encoder and the model against it and never replace it
([tools/voice/README.md](../../../tools/voice/README.md)). Every take is pending
listening review, so the pack is a DRAFT. That layout replaces the proposal below.

Proposed source layout, to create with the first accepted recordings:

```text
assets/annunciation/<locale>/
  script.csv          # semantic IDs, spoken text, pronunciation and clip roles
  voice.json          # producer/voice settings, revisions and accepted file hashes
  LICENSE.txt         # source and redistribution notices for this pack
  masters/*.wav       # accepted, edited PCM masters before panel compression
```

Retain the original-quality approved recordings and encode the panel format from them.
Commit selected takes and their metadata together; do not regenerate unchanged clips.
Model weights, credentials, personal recordings and rejected auditions do not belong in this
source library. Mark WAV files as binary in Git attributes when adding the first pack.
Track the initial library with ordinary Git; reconsider Git LFS if measured
catalog/history growth warrants it, not as a prerequisite. At the draft 105.95 s duration,
mono PCM16 payload is about 4.85 MiB at 24 kHz or 9.70 MiB at 48 kHz per voice/locale, plus
headers; these are estimates, not generated file sizes. Native export quality decides the
master rate; do not upsample solely to meet a chosen rate.

CI reads the selected locale's checked-in masters, validates coverage/hashes and uses a pinned
encoder to produce its compressed library/index and size report. Generated packs are build/
release outputs; the repository masters remain the source of truth. A clean checkout must
build the pack without contacting a TTS provider, downloading a speech model or having a warm
Actions cache. New audio still requires listening acceptance before it becomes a release input.

Offer **release bundles per locale from one codebase**, each with one compatible flash voice
pack and matching UI locale resources. Speech alone does not require different application
binaries: the proposed audio pack lives in the separate assets partition. Compile per-locale
firmware only if translated strings/font coverage or measured flash limits make that useful;
otherwise reuse a compatible application binary across locale bundles. Record board, firmware
version, UI locale, voice-pack revision/hash and format compatibility together. Adding an audio
language does not by itself deliver a translated UI. Locale-aware bundling/installation and
UI localization are future work, not supported behavior in the current firmware workflow.
UI localization is planned as [Globalization](../37-Globalization/README.md) (docs/features/37,
M22): it defines the BCP 47 locale tags the packs share, the language setting and the fonts, and
one application image per board carries every Latin-script UI language, so a Latin-script bundle
differs only in its voice pack (a wide script adds its font).
Ordinary application OTA must retain the installed audio; a locale change uses the planned
verified pack installer and explicit binding review, preserving rollback and user configuration.

## Globalization and avoiding choppy speech

One good voice does not make arbitrary word joins natural. Use three separate listening
checks: complete generated phrases; the raw recorded tokens joined into labels; and the same
material after compression. This distinguishes generation/prosody, cutting/gaps and codec
damage. No generation, pronunciation or join-quality result has been measured yet.

Keep useful compound tokens; the status messages are joined from parts (decided 2026-10-08;
a phrase's words 60 ms apart and the pause only before a new sentence, 2026-10-09; 40 ms
in pack revision 6, with the word forms; 100 ms in revision 7, each word spoken alone, and 60 ms
from revision 8). Some
words need contextual takes
for use before another word versus at a phrase ending; store only the variants that earn
their space in the audition. Never cut at a fixed timestamp or drop a final consonant to meet
the duration estimate. If useful words need more natural room than ADPCM allows, compare
Opus or revise the inventory before accepting rushed/choppy output.

Use stable semantic event IDs with **locale-specific full messages and label recipes**, not
English sentence fragments translated one by one. Word order, agreement, compounds and number
reading vary by language. The en-US list is a starting inventory, not a universal
schema. Each locale can define additional forms or compound tokens within its own
pack and must pass complete-label tests. Keep display labels, spoken text and pronunciation
overrides separate. Explicit locale IDs such as `en-US`, `es-MX` and `pt-BR` belong in metadata;
a model supporting “Spanish” alone does not prove regional pronunciation quality.

English is the first accepted-pack target; additional locales are future deliveries. Budget
**one selected supplied locale/voice in flash**, with the current/staged-generation reserve.
Choosing another language replaces that selection with the language's own voice, measured to
fit (audio-storage.md, BL-205). Additional
simultaneous packs remain on SD. Do not multiply the English size estimate by a language
count and assume it fits, or silently mix languages when a token is absent. Missing audio
retains tones; any alternate-language fallback must be explicit. Custom files still require SD.

## API/hosted production alternatives

Prices and capabilities below were checked against provider pages on 2026-09-27 (ElevenLabs
again on 2026-10-08). These are **production-computer tools**, like the local engines above.
Qwen3-TTS, VoxCPM2 and Kokoro remain the local audition candidates; an API producer is equally
acceptable if its output passes the quality and redistribution checks. Neither is a release-build
dependency.

| Option | Cost relevant to this pack | Assessment / audition |
|---|---|---|
| ElevenLabs, Eleven v4 (released 2026-09-28) | API pay-as-you-go $0.08 per 1K characters ($0.022 until 2026-10-12); Starter $6/month ($1 the first month) includes about 273,000 v4 characters and commercial rights | Sensible first paid audition if its voice meets the project's target. Generate the final pack during a paid month; no subscription needed for local playback. Pay-as-you-go on the free plan may not carry commercial rights (not confirmed): use Starter. [API pricing](https://elevenlabs.io/pricing/api) |
| Kokoro-82M | Free model/local generation; Apache-2.0 model weights | Try Heart and Bella in the [official demo](https://huggingface.co/spaces/hexgrad/Kokoro-TTS). Local PC batch generation avoids per-character charges. Quality for these short tokens is unaccepted. [Model card](https://huggingface.co/hexgrad/Kokoro-82M), [voice notes](https://huggingface.co/hexgrad/Kokoro-82M/blob/main/VOICES.md) |
| Azure neural TTS | Free F0 tier lists 500,000 neural characters/month; Azure account/resource setup required | Audition standard English voices in [Voice Gallery](https://speech.microsoft.com/portal/voicegallery). More setup, useful pronunciation/prosody controls and export. [Pricing](https://azure.microsoft.com/en-us/pricing/details/speech/), [audio-content tool](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/how-to-audio-content-creation) |

The script contains **1,557 text characters**, or 1,792 including one newline per clip. One
plain-text pass costs about $0.12 at v4's standard rate; five takes of every clip, each word
alone and in a carrier sentence, come to about 30,000 characters (about $2.40). Actual use includes retakes,
context, selected model/voice rates and any added text; do not equate 235 separate calls with
a guaranteed invoice. Audition a small set before paying for bulk production. Current
ElevenLabs terms say paid-plan generated content retains commercial use after cancellation;
free-plan output is noncommercial and needs attribution when published, so upgrading later
does not retroactively cover free takes. Use production, non-beta services.
[Publication rights](https://help.elevenlabs.io/hc/en-us/articles/13313564601361-Can-I-publish-the-content-I-generate-on-the-platform),
[subscription end](https://elevenlabs.io/docs/help-center/account/general/what-happens-to-my-content-after-my-subscription-ends).

ElevenLabs' [export API](https://elevenlabs.io/docs/api-reference/text-to-speech/convert)
gates 44.1 kHz PCM/WAV to Pro, while 128 kbps MP3 is the default; WAV at 16, 22.05 and 24 kHz
is listed with no plan required. Prefer such a lower-rate PCM export, or preserve/decode the
original MP3 once for the audition; do not buy Pro merely to assume higher export rate is necessary on these speakers.
Higher-cost exports are a listening decision. Download and archive accepted masters locally.

For a public firmware asset, record the selected provider/model/voice, generation date,
settings and applicable terms/notices with the pack. ElevenLabs' output permission is distinct
from ownership of its models; a paid commercial-use statement should not be relabeled as an
Apache license on the voice model. Check the exact voice/output terms when packaging; do not
use a celebrity/unauthorized cloned reference or copy another product's audio. These are source
provenance tasks, not a reason to delay drafting or auditions. No purchase, account operation
or audio submission was made in this review.

## Small audition before recording everything

Compare voices using the same words and complete messages: Front, Back, Door, Garage,
Upstairs, Bedroom, Window, Living room, Twenty, Eight; “Armed away”, “Disarmed”, “Please
disarm the system”, and “Not ready. Check the zones.” Also compare whole generated examples
“Front door” and “Upstairs bedroom window” against sequences assembled from the individual
word recordings. Judge clarity, tone, consistency and joins separately from compression.

[tools/voice](../../../tools/voice/README.md) makes these auditions with the local engines: the
words and labels are in its `audition.json`, the samples land in `build/voice/samples/` (never
committed) and Whisper flags any clip it doesn't hear as written. The first round (2026-10-08:
10 words, 5 labels, two voices per engine, each word recorded alone as "Word.") was heard as
written in 115 of 120 clips. Whisper heard the joined labels as separate sentences ("Upstairs.
Bedroom. Window."), so a word recorded alone may end like a sentence; whether that sounds choppy
is for the ear to judge, and contextual takes are the next thing to try if it does.

Qwen3-TTS's `designed_female` was chosen by ear (2026-10-08). `voice.py record` speaks
the whole script in that voice (`library.json`; its `reference.wav` is the voice, so it is
copied, never designed again), with Whisper retakes, and `voice.py encode` makes the Opus
24 kbps version to hear and measure.

Ask for a calm, clear, neutral annunciator delivery with even pace, no music, breathy effects
or dramatic emphasis. Lock one voice/model/settings set after listening. Keep token takes
consistent; do not paste 100 punctuated words into a single request and assume automatic
splitting will produce interchangeable words. Kokoro's own voice notes warn of weakness on
very short utterances; use context/batched takes if needed and manually verify every cut.
Some words may need a second contextual take or a compound-token substitution; count every
extra shipped sample in the budget. ElevenLabs' previous/next-text controls can assist
continuity but do not guarantee natural arbitrary joins.

Trim excess silence without clipping consonants, match perceived levels across the pack,
retain a short clean boundary/fade, and represent inter-token gaps in recipe metadata instead
of storing long silence. Proposed initial master peak ceiling is -3 dBFS; final perceived
level and mixer headroom are listening/measurement gates, not a promise based on peak alone.
Archive original takes and lossless edits; create PCM/IMA/Opus comparison files from the same
edit. Audition on headphones first, then through the actual panel speakers at normal room
distance after hardware testing is authorized. Small size cannot rescue an unacceptable voice.

Before freezing A0/A2, the pack report must give exact clip count/durations/bytes, missing IDs,
accepted joins, voice/provenance, peak/loudness checks, decoder/firmware footprint and actual
LittleFS free space with both generations present. Reject oversized packs; never silently
drop required words. A0 is not complete until the voice, joins and compressed output pass.
