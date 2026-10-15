# Annunciation audio storage (proposed)

**Status:** Design for Feature 08, 2026-09-27; no voice player, assets filesystem, pack tooling or TTS
engine has been implemented by this review. Direction: recorded word/phrase recipes and
custom imported clips coexist and use the same player. Since 2026-10-08 (ADR-052) nothing is
synthesized on the panel: the library is recorded on a computer, and what this document says
about TTS models, language data and generated speech no longer applies. Media split is decided:
supplied library in flash; custom audio on SD only. Codec, voice count and capacity budgets
need qualification.

## Stored data and ownership

| Data | Purpose | Proposed home |
|---|---|---|
| Essential tones | Existing synthesized beeps, chime, fire/CO patterns | Firmware code/tables, unchanged; never depend on a file or SD card |
| Recorded library | Original status parts plus reusable words/short phrases with stable token IDs | Fixed-budget supplied pack in internal `assets`; optional extensions/alternate packs on SD; never generated cache |
| Imported audio | The user's recordings or externally generated complete clips | Managed durable copies on SD only; never evicted or regenerated as TTS cache |
| Preferences and selectors | Enable, library/TTS voice IDs, active/previous manifest and installation identity | Small versioned NVS records; never PCM or a model blob |
| Bindings and recipes | Target zone/prompt, selected source, revision, ordered token IDs or asset/text references | Bounded versioned metadata alongside configuration/manifests; exact schema and NVS size budget at A2 |

Hot verified samples may be cached in PSRAM for any source. That cache is expendable; the
underlying library/imported assets are not. “Clear generated audio” applies only to TTS
output. Library removal, user-file deletion and model removal are separate explicit actions.
No custom file, model or generated speech is installed or persistently cached in internal
flash, even if it is small. Only bounded control metadata remains internal. TTS runtime code
still belongs in the qualified firmware image; SD capacity does not increase working RAM.

A voice model is not a collection of WAV files. Compressing clips with ADPCM does not make a
TTS model smaller. The panel needs a model to prepare new files, but not to replay existing
verified files. SD is physically on the device and needs no network; it remains removable
and fallible. Models and generated output use the installed SD card; valid generated files
still play if the model alone is uninstalled, but not if their card/files are unavailable.

Resolve the **selected binding first**, then use a matching RAM cache or its immutable
flash/SD files. Storage location is not a priority rule between different voices or sources.
Any identical-copy recovery must respect the source's media rule. A recipe uses one compatible
library voice/pack and verifies every token, its order and gaps. Missing audio falls back
to existing tones; there is no automatic cross-source substitution or event-time synthesis.
Flash and SD portions of the same library must agree on language, voice and prompt schema.
Different assignments can deliberately use different sources/voices.

## Current storage facts

`firmware/crowpanel_p4/partitions.csv` reserves 16 MiB as fixed regions, including two
4608 KiB application slots and **2560 KiB (2,621,440 bytes)** of `assets` (`data,littlefs`).
Other reservations include NVS, C6 firmware, update cache, event history and crash data.
They are not voice capacity. App headroom is for code, not an unallocated file area.

Since 2026-09-30 (ADR-045) `assets` holds the **pack store** of docs/features/37
[language-packs.md](../37-Globalization/language-packs.md), not LittleFS: two table sectors and
whole signed blobs, a `kind` per blob - language packs are kind 1, and kind 2 is kept for a voice
library, which this design now stores as one signed blob (its index and clips inside) written
through `pack_store`. Where the text below says LittleFS or files in `assets`, read a blob of the
store. The store is read through its own code before anything is written. Never change the partition table
for voice, and never add an assets image to ordinary flash/OTA commands that would overwrite
the installed library during a firmware update.

`panel_storage` already mounts supported FAT12/16/32 cards at `/sdcard`; exFAT is not enabled
in this project's configuration. Its `panel_storage_acquire()` is a **nonblocking exclusive
mutex lease**, not a persistent handle or a multi-reader API. Maintenance and the five-second
card probe use that same mutex. Holding it for an entire model lifetime would starve both.
Close every file before `panel_storage_release()`. The lease prevents software unmount while
held; it cannot prevent physical removal. Feature 29's file-integrity, power-loss and
destructive-media tests remain unfinished.

## Capacity before choosing a codec

The editing/reference format is PCM; the first compressed clip candidate is mono 16 kHz
IMA-ADPCM WAV. The [production plan](voice-library.md) also compares Opus at 24/32 kbps for
the fixed library. Audition encoded output before fixing the supported codec set; this is
not a general media decoder. A library using Opus need not force on-panel Opus encoding for
imports/TTS: SD files may use qualified PCM/IMA, with explicit reader capabilities in the
manifest. The board output stays 32 kHz PCM16 stereo; resample once and duplicate mono only
at the output stage. Do not store stereo duplicates.

| Representation | Approximate payload per second | Five seconds | Entire 2.5 MiB reservation, theoretical |
|---|---:|---:|---:|
| PCM16 mono, 16 kHz | 32,000 B | 160,000 B | 81.9 s |
| IMA-ADPCM mono, 16 kHz, 4 bits/sample | 8,000 B | 40,000 B | 327.7 s |
| PCM16 mono, 32 kHz | 64,000 B | 320,000 B | 41.0 s |

These are arithmetic estimates. WAV headers, ADPCM block headers, manifests, filesystem
metadata, allocation rounding and update headroom reduce them. They are not measured pack
sizes. A 250 ms buffer at 16 kHz mono PCM16 is 8,000 bytes; at 32 kHz it is 16,000 bytes.
These buffers can absorb worker jitter without requiring the I2S owner to queue 250 ms of
uncancellable speech: discard unsubmitted samples on cancellation; retain only the existing
short hardware queue. TTS scratch space and whole-utterance output are additional costs.

**Planning budget:** the supplied library is one signed blob, replaced whole: an update writes
the new copy before the old one goes, so the store holds it twice for that moment. Beside one
language pack (es-US, 216 KiB, the largest) and the planned asset packs (about 420 KiB,
docs/features/39), that allows at most about **950 KiB** per copy. Decided 2026-10-08: keep this
whole-blob plan (a chunked library that updates a few words at a time was weighed and set aside
as more code for space the store has); the earlier 1 MiB + 1 MiB + 512 KiB reserve predates the
language and asset packs sharing the store. This is the supplied library's budget. Imports, TTS models and generated files are excluded
because they require SD, not because they can use another internal partition.
This is an upper bound to verify with the real filesystem, not a promise that all that space
is usable or dedicated forever to audio. Admit an install only when measured free space can
hold its complete staged data and reserve. Never delete the working pack to make it fit.

At ideal ADPCM density, one 1 MiB pack holds roughly 131 s before overhead. Library recipes
reuse samples across zones: “Front door” and “Back door” share “Door”, with gaps/order in
metadata. There is no per-zone audio duplicate or 128-zone limit baked into the pack format.

The [recording script](recording-script.csv) proposes 219 tokens and 16 status and walk-test
parts (en-US, 2026-10-08). Assumed durations total 190.90 s. At 16 kHz mono IMA with 256-byte
blocks and canonical 60-byte WAV headers, the calculated clip total is 1,576,724 bytes, over
1 MiB before the 64 KiB of index/notices/pack headroom; Opus at 24 or 32 kbps (572,700 or
763,600 bytes) fits with room to spare. This is a planning calculation,
not a measured artifact. [voice-library.md](voice-library.md) records the formula, actual
text, Opus comparison and audition gates. Do not force speech unnaturally fast to meet it.

The supplied library must support card-free playback and word selection. Custom imports,
generated sets and models always require SD; no flash-only TTS mode or automatic spillover.
Optional larger/alternate recorded packs also use SD. No model is needed for word selection
or imported playback. Measure model stored/loaded sizes separately from the flash library.

SD capacity is not unbounded allocation permission: A2 defines separate admitted budgets for
installed packs/models, retained user files and generated sets, plus staging/free-space
reserve shared with other storage consumers. Show used/required/free space before import or
preparation; preserve existing assets if space runs out. Readable current clips can still
play on a full card. No card means no import or TTS preparation and no new SD-dependent
speech, even from an expendable RAM cache once removal is detected. Built-in speech and
essential tones continue; keep bindings so reinserting the verified card restores readiness.

## Custom-file import and recorded recipes

Proposed initial upload format: uncompressed PCM16 WAV, mono or stereo, 8-48 kHz, at most
ten seconds decoded duration and 2 MiB input size. These bounds cover the largest proposed
PCM payload (10 s x 48,000 x 2 channels x 2 bytes = 1,920,000 bytes) with bounded header room.
A0 must qualify conversion/quality; A2 freezes the exact accepted WAV tags/chunks/rates.
Reject unsupported/compressed WAV, malformed sizes, excess channels and oversized duration
before activation. MP3/AAC and in-panel microphone recording are later work. The UI must
show accepted formats and a useful conversion error, not silently truncate an upload.

An optional worker converts to the canonical mono/rate/codec chosen at A0, checking decoded
bounds, gain and clipping; it never converts during event playback. Preview the stored result
at normal user volume before assignment and confirm its zone/prompt purpose. Keep a managed,
immutable canonical file on SD as the authoritative user asset. Do not assume the panel retains
the original uploaded bytes; tell the user to retain their original. No server or external
computer is required after import. Installed clips can be assigned from the touchscreen too.

A library pack supplies consistent levels, compatible leading/trailing silence and defined
join gaps. A recipe stores stable token IDs and ordering, bounded to eight tokens and ten
seconds including gaps. Resolve/validate the complete playlist during preparation; workers
prefetch/decode outside the audio task. No runtime TTS or per-event concatenation write is
needed. Whether to materialize selected recipes into combined files is a later measured
optimization; the first design reuses samples. The fixed flash library is proposed as one
indexed file containing independent clips to avoid tiny-file allocation waste. Audition
complete sequences, not just words. Production-computer TTS output in the supplied measured
library may live in flash; arbitrary user/TTS assets still require SD.

## Pack and cache contract

Freeze schema v1 at A2 after A0 chooses codecs/voices. Proposed fields:

- Format version, pack ID/version, explicit locale (for example `en-US`), voice ID/revision,
  locale script/recipe-grammar revision, producer provenance and license notices, minimum
  reader capabilities, total stored/decoded byte limits and file count. Production records
  retain pinned model/runtime/reference hashes and accepted master hashes; the panel index
  only needs bounded metadata sufficient to identify its compatible pack.
- Each clip: immutable asset ID, source class (library/imported/generated), codec, sample
  rate, channels, frames/duration, byte length and SHA-256; language/voice/provenance where
  known. A user's description of a recording is metadata, not verified spoken content.
- Packed-library clips also carry bounded offsets into their immutable container; validate
  the file hash, each record, nonoverlap and overflow-safe bounds. Reset decoding and trim
  padding/pre-skip per clip. Loose WAV editing fixtures need not become separate flash files.
- Each binding: semantic prompt or zone installation identity, approved zone-name revision,
  source kind, binding revision and explicit source reference. A library reference includes
  pack revision, ordered token IDs and gaps; an import references its asset ID/hash; TTS
  includes normalized text/pronunciation and model identity. Zone number alone is insufficient.
  Keep a full identity check, not a short filename hash as the only comparison.
- Separate model metadata: engine/ABI version, model/frontend/dictionary versions,
  hashes, file sizes and measured load/scratch/output budget. Models are data for the selected
  engine, never executable plugins or downloaded code.
- TTS generation key: normalized text + pronunciation + language + voice/model/frontend
  revision + sample format/rate. Deduplicate identical generated content. Imported audio is
  keyed by verified content, not by text. Recipe readiness includes the complete token/gap
  sequence and pack revision. Status messages are joined from parts too, with a pack-defined pause.

Budget one selected supplied locale/voice in flash, and it follows the language the user chooses
(decided 2026-10-12): choosing a language on the panel downloads that language's
pack and its voice from the update server, and the voice replaces the English one in flash, within
the same measured budget and rollback rules; choosing English again brings the English voice back
(BL-205). A language's voice is the only one it plays: never another language's voice with it.
Custom words and other added audio are uploaded through the Panel Manager to SD (AUD-020,
AUD-022). Additional simultaneous packs require SD.
The English draft token inventory is not a universal language schema: each locale owns full
messages, number reading and label recipes, including necessary word forms/compound tokens.
Validate required semantic coverage and locale compatibility before activation; never silently
mix languages to repair a missing word. See [production/globalization](voice-library.md).

Approved masters/scripts/provenance are repository source assets, generated locally or through
an API. Release tooling encodes the selected locale and records its pack identity beside the
compatible board/application and UI locale; these are planned per-locale bundles from shared
code. UI locales are [Globalization](../37-Globalization/README.md) (docs/features/37); a Chinese,
Japanese or Korean UI font may also live in `assets`, so its G5 measurement and this budget are
decided together. Audio lives outside the application OTA image. Ordinary app updates must not overwrite
it, and changing a bundle's locale must not imply erasing household configuration. A separate
verified pack-install/locale-review flow remains A2/A3 work; no locale installer exists yet.

Use generated safe filenames inside an owned directory and allow-list payload types. Reject
path traversal, absolute paths, duplicates, oversized indexes, unsupported formats, impossible
sample/block counts, truncated files and integer overflow before activation. Bound text to
the existing label limits and the policy's maximum utterance length. Hashes detect corruption;
they do not establish a publisher's identity. Curated distribution and authorized household
imports need explicit provenance; keep existing firmware signing separate.

Illustrative layout (paths are proposed; short IDs/names remain usable without assuming FAT
long-filename support):

```text
/assets/voice/<generation>/manifest.json     # supplied library only
/assets/voice/<generation>/library.bin       # indexed, independently decodable clips
/sdcard/roboalrm/voice/<pack-id>/pack.json    # larger immutable clip pack
/sdcard/roboalrm/voice/<pack-id>/<clip-id>.wav
/sdcard/roboalrm/imported/<asset-id>/...     # managed user clips, not generated cache
/sdcard/roboalrm/models/<model-id>/...        # engine data needed during preparation
/sdcard/roboalrm/prepared/<set-id>/...        # persistent locally generated clips/index
```

The loader uses the mount root returned by the storage service. It does not scatter hardcoded
card paths through the UI or audio code. On SD use bounded ASCII 8.3 names for IDs and files
unless long-filename support is explicitly enabled/tested; descriptive names live in metadata.
Clip IDs and manifest names above are placeholders, not a literal on-disk naming schema.

Persistent prepared audio is required for configured phrases/names; an optional RAM cache
accelerates their playback. Evict RAM by recency/generation, but pin current configured files
on disk within the storage quota. Do not use a small disk LRU that repeatedly evicts and
regenerates names during ordinary use. If the configured set cannot fit, report the missing
items/storage requirement and retain tones for them; never overrun the quota.

Regenerate only affected **TTS** files when text/pronunciation/voice/model/template changes,
or when boot/maintenance finds missing/corrupt generated data. Missing imported recordings
need reimport; missing library files need pack repair. Deduplicate unchanged immutable
clips by full content identity; replacing one zone label need not copy/rewrite every other
file. Commit a new manifest referencing verified files and retain old referenced content until
no active/previous generation needs it. Admission accounting includes retained and staged
bytes. A library/TTS voice switch stages its affected set before activating it together;
unrelated sources retain their files and voices. Missing library tokens prevent that pack
switch, with a list of affected bindings; never guess substitute words or splice voices.
No flash write or generation is required per chime. The current v1 scope contains no arbitrary
transient speech; future such features must define their own preparation/persistence behavior.
Do not cache PINs or hidden duress details. Household labels/audio are private data; diagnostics
report IDs, durations and failures by default, not their text or a voice recording.

## Install, select, recover and remove

1. Install library packs, custom files and optional model data through SD import or bounded
   authenticated Panel Manager uploads. Both use the same validator/installer with system-
   master/installer authority and disarmed/silent checks; neither exposes arbitrary file
   operations. Custom/model upload and synthesis require writable SD with staging capacity;
   reject them before starting when it is absent/full, and never redirect writes to flash.
   Assigning zone labels/system prompts remains installer programming. Imports
   require explicit preview/assignment; background TTS repair derives only from current
   authorized settings and does not require a user to stay signed in.
2. Probe storage and report missing filesystem or insufficient space before writing. An
   uninitialized `assets` partition needs explicit first-use initialization scoped to that
   partition. A failed mount is never permission to format. Unknown/corrupt existing data
   remains intact until the owner chooses recovery; normal tone operation continues.
3. Copy/import/generate into bounded staging files; encode, validate sizes/hashes/schema,
   flush and close, and verify it from storage. Do not edit the live generation in place. Internal assets go into the
   pack store as whole blobs (ADR-045); do not assume FAT rename makes an SD
   installation power-fail-safe.
4. Commit a versioned NVS selector containing active and previous IDs plus integrity/version
   data only after the candidate is verified. Recheck state, media generation and the current
   source/binding/config identity; imported package activation also rechecks user authority.
   Failure to persist selection leaves the old pack active. Power loss yields an old/new
   verified generation or tones; never a partly written voice. Interrupted preparation is
   retried when its source permits; obsolete results are discarded. Test each interruption boundary.
5. At boot/use, revalidate compatibility, installation identity and payload identity. If the
   selected generation fails, try a compatible previous one only for the same current
   source/binding identity. No compatible audio means tones, not an old name or prior source.
6. Firmware OTA/rollback does not replace audio. Unknown newer formats are ignored while
   preserving data. Avoid destructive pack migration during firmware probation; retain a
   generation the previous firmware can read. Garbage-collect only unselected owned files
   after successful activation/probation; refuse a third generation if space is insufficient.
   Unassigned user imports stay in the user's library until explicitly deleted; they are not
   unreferenced generated garbage. Include retained drafts/imports in admission accounting.
7. Refuse removal of a referenced pack/file until the user explicitly reassigns/disables its
   affected bindings under the proper authority. Drain readers, then delete only that owned
   asset. Shared content, other imports and unrelated card files remain. Generated-audio
   clearing is separate and explains that rebuild needs the model and preparation time.
   Factory reset must invalidate household audio/indexes with a new installation identity so
   old names cannot reappear; it must not silently erase the whole SD card. Define explicit
   household-data cleanup alongside the reset implementation and document residual SD files.

If a card fails, an already playing buffered clip may finish only while current policy permits;
cancel it on IO failure/underrun or a higher-priority event. Stop new card-dependent speech,
show the availability reason and retain tones for those bindings; flash-library speech stays
available. Reinsert/check does not replay missed announcements. Only a supported supplied-
library update may install to flash through a checked installer; arbitrary SD packs, imports,
models and generated files cannot be copied there as a capacity or failure workaround.

At reboot reconcile current config against the persisted manifest rather than regenerate
everything. All recorded/imported audio and existing generated clips must play with TTS
disabled/unloaded. Queue source-appropriate preparation only; failed writes leave clips
not ready, even if a synthesized buffer still exists in RAM. Separate “preparing”, “ready”,
“paused”, “needs review” and missing-file/model states. A name change needs reapproval of
explicit recipes/imports/text overrides, or regeneration for TTS linked to the display name;
the old asset remains but cannot speak until its binding is current. See Feature 08's lifecycle.

## Worker ownership and media maintenance

Preload a bounded clip or model section under an exclusive storage lease, close it, release
the lease, then decode/synthesize outside the lease. On retries/reopens, compare card generation
and immutable pack identity before using offsets. The existing API does not atomically return
a generation with the lease: adding a generation-bearing lease/read helper is A2 work, with
tests for swaps between reads. On IO failure, invalidate that load immediately.

For a model that needs an open file during synthesis, either prove it fits resident PSRAM
after a bounded load or extend the storage worker with cancellable session/read operations.
Do not quietly hold the current mutex through a long utterance or leave a FILE open after
release. A pre-read lease cannot impose a hard wall-clock limit on a failing SD driver; measure
driver timeouts and ensure only the optional worker can stall. The sounder never waits for it.

Safe removal/check/format must be able to stop optional speech IO and release its leases;
the UI reports busy until handles are closed. The first consumer needs to add this quiesce
coordination: today's storage service only rejects maintenance while leased. Formatting still
follows Feature 29's explicit card-identity confirmation and separate authorization. No model
or audio activity may tear down Wi-Fi's shared SDMMC controller.

## Verification and sources

Planned tests: parser/codec/import fixtures (including malformed WAV lengths, packed offsets,
ADPCM blocks and Opus trim metadata if selected), exact
decoded frame bounds, rate conversion, stale name/config/voice identity, full media, IO error,
card swap/removal, lease release, failed NVS commit, power cut at each install stage, firmware
downgrade, cache quota/wear behavior, unauthorized operations and factory-reset identity.
Also prove local word selection/order/preview -> reboot -> event with no TTS model or computer;
recipe gaps/duration/missing-token handling; mixed library/import/TTS assignments; upload
conversion/preview/assignment and unauthorized binding changes; generated-cache clearing
preserves custom/library assets; in-use removal and source-switch/stale-job rejection.
Assert all custom/model/generated writes target SD; missing/full/read-only cards cannot
redirect them to flash. Prove built-in setup and speech with no card and after card failure.
For TTS prove offline rename -> prepare -> durable file -> reboot -> event playback, with zero
TTS calls/writes across repeated events; unchanged files reused; TTS/model unload after
preparation; single-label regeneration; interrupted batch resume; pending voice-set activation;
and generation paused during alarm/arming/OTA/power saving without delaying the sounder.
SD filesystem damage may lose optional audio; pass means no incorrect speech and no effect
on alarm processing/tones, not a claim that FAT cannot be corrupted.

Repository evidence: `firmware/crowpanel_p4/partitions.csv`,
`components/services/panel_storage/include/panel_storage.h`, its `src/panel_storage.c`,
`components/services/platform_esp/src/sounder.c`, the board's `board_audio.c`, and Feature 29.

External primary references checked for this plan:

- [ESP-IDF 6.1 NVS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-reference/storage/nvs_flash.html):
  use small preference/selector values; filesystems suit larger blobs.
- [ESP-IDF 6.1 FatFs](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-reference/storage/fatfs.html):
  file APIs, close/unmount lifecycle, sync and filename configuration. Actual support depends
  on this firmware's selected options.
- [ESP LittleFS](https://github.com/joltwallet/esp_littlefs) and its
  [mount API](https://github.com/joltwallet/esp_littlefs/blob/master/include/esp_littlefs.h):
  candidate implementation for the existing flash reservation; pin and qualify during A2.

The capacities and allocation recommendations above are project calculations/design choices,
not upstream performance claims. No card data or internal asset bytes were read/written on a
panel during this review.
