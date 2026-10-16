# Voice, first build: spoken zone names from the library (BL-202)

**Status:** designed 2026-10-08, built (2026-10-09). The design
below was decided in one round and adopted as final; deviations made while building are marked *Built:*.
Requirements are in the [README](README.md) (AUD-008 to AUD-029), the configuration ones in
[01](../01-Core-Alarm-Engine/README.md) (CORE-CFG-020, CORE-CFG-021), the editor in
[07](../07-UI-LVGL-Pro/README.md) (UI-045) and the walk test in [40](../40-Walk-Test/README.md)
(WALK-016).

The first build plays the supplied library only ([voice-library.md](voice-library.md),
ADR-052): an installer picks each zone's spoken words from the voice pack's vocabulary, and the
panel says them after the chime and the walk test's beeps, and says the status messages. No
speech is made on the panel; custom recordings (SD) and the browser's word editor come later.

We make no speech on the panel (ADR-052), so a spoken name is chosen from the library's words,
while the zone keeps its free text name for screens, events and MQTT.

## A spoken name for each zone

**Decision (D01):** Store an independent ordered spoken-name recipe per zone. Suggest words from the display name, but activate a recipe only through explicit approval. Never derive speech from the display name at event time.

*Why:* This combines a vocabulary selection with RoboAlarms’ separate free-text display name. AUD-016 and Feature 08’s rename contract require review before an obsolete label can play.

The display name remains unchanged: 47 bytes plus NUL, used by screens, events and MQTT. A new zone starts with no approved spoken name; opening its editor can offer a matching draft. Changing the display name, partition or assigned sensor preserves the recipe but assigns a new binding revision and clears approval. Renaming back to the original text does not restore approval automatically. A type change preserves the recipe and approval when the zone, sensor, partition and display name remain the same; type_chosen must explicitly preserve these fields around its defaults reset. Changing to Not used removes the recipe and approval. Reusing that slot receives a new identity. These rules run in shared configuration/binding transactions, including browser changes, rather than only in the touchscreen editor. No automatic replacement words or device-type suffix are added.

## How long a spoken name may be

**Decision (D02):** Allow eight selectable vocabulary entries and at most 10 seconds of spoken audio, including joins.

*Why:* Eight entries and 10 seconds already define the recipe budget in audio-storage.md. Keeping that bound supports long names without a new storage or policy limit.

A recorded compound such as Living room counts as one entry. Repeated entries are allowed. Duration is calculated from exact valid sample counts: sum(samples) + the pack's word gap × (entry_count − 1) must not exceed 160000 at 16 kHz (60 ms, 960 samples, in the supplied packs from revision 8). When the pack has the last word's ending (AUD-031), that ending's samples count instead of the word's, as it plays. Empty recipes mean no spoken name. Reject an overlong selection before approval; never truncate it. Every automatic status or walk utterance also has a 10-second ceiling, including its 300 ms part pauses.

## The touchscreen editor

**Decision (D03):** Add Spoken name immediately after Name in Installer > Zones > zone. Use existing sheets and submitted search; build no new visual component or drag control.

*Why:* ui_form_sheet_open, ui_pick_sheet and the sensor-search pattern already supply the necessary controls. Categories contain at most 84 current entries, below the picker’s 128-row limit. Feature 14 explicitly reserves zone-word assignment for installer programming.

The zone row shows the selected phrase, ellipsized only in its summary. Empty: Choose words. Invalidated approval: Review spoken name. Voice off: Announcements are off, while retaining the saved phrase. Missing pack: Voice pack not installed. An installed pack that is not ready: Voice is unavailable. These states do not clear the recipe.

The editor uses ui_form_sheet_open with Cancel and Done. Show the display name, an ordered list of up to eight word rows, Add word, Use zone name and Preview. Tap a word row to open ui_pick_sheet with Move earlier, Move later and Remove; dim impossible moves with ui_pick_sheet_dim. Add word offers Find a word and the categories Position, Rooms and places, Things and Numbers. Category lists are alphabetical. Find a word reuses ui_text_sheet_kind and submitted prefix search from ui_sources; return at most 64 choices per page with Previous/Next. No live keyboard suggestion strip in the first build. Disable Add at eight entries. The search prompt is “Enter the start of a word, then tap Search.”; an empty submitted search lists all selectable words (221 now, 64 a page, from the validated active pack); a search with no match says No words match “zz”. The editor and picker show “Voice language: English (United States)”, translated with the locale name; the recorded words stay in the pack’s language.

Use zone name performs case-insensitive ASCII matching after trimming and collapsing whitespace and treating punctuation/hyphens as separators. Match the longest complete vocabulary phrase at each position. Convert cardinal digit runs 0–999 using the recorded number entries, without and; 100 becomes One + Hundred, 128 becomes One + Hundred + Twenty + Eight. Leading-zero runs of at most three digits are read digit by digit. Unsupported text, numbers, excess entries or duration leave the current recipe unchanged and identify the unmatched spans in one complete translated message with the span as a value (The panel can’t say “2nd”. The words stay as they are. Choose words yourself, or rename the zone.); never silently omit them. A complete match replaces only the editor’s draft until Done.

Done approves against the current binding and pack revisions when audio is valid; otherwise it saves a draft without approval. An overlength draft is saved the same way, never approved, truncated or previewed, and the editor says inline: This name is too long to say. Remove a word. Without a validated active-pack vocabulary, Add word, Use zone name and Preview are dimmed and the editor says Voice pack not installed. (or Voice is unavailable.); saved words keep their move and remove actions. Cancel discards changes. Preview plays the complete draft without approving it. Explicit Preview may audition valid audio while automatic announcements are off; it still respects volume, power saving, alarms and AUD-025. No Saved toast is needed because the resulting row shows the outcome.

Installer access uses the existing zone-programming gate. Masters manage announcement preferences and packs, but do not edit zone recipes in this build. Draw all empty/full/off/missing/review/error states in the BL-044 touchscreen proposal and record this delegated design approval before implementation, preserving UI-044’s mockup-first workflow.

## The chime

**Decision (D04):** For chime, play the existing ding-dong once, then the approved spoken name. Do not replace the tone and do not append open or another word.

*Why:* The engine already approves chime eligibility. Keeping its immediate tone avoids a race in which a queued decoder job suppresses the tone but never produces speech. This deliberately changes Feature 08’s proposed chime-replacement behavior.

For a named zone, the exact sequence is the existing ALARM_SOUND_CHIME followed by its approved token recipe. For an empty, unapproved, stale or unavailable recipe, voice off, missing pack or decode failure, the existing tone alone remains. Chime still requires the engine’s disarmed/chime-enabled/zone-chime/non-bypassed conditions. A spoken recipe does not enable chime. Speech begins after the actual chime completion, rather than after a guessed 980 ms timer. A cancelled or dropped chime cancels its associated speech. An underrun after speech starts stops speech; it does not add another tone. A new alarm cancels the optional name immediately.

## What the panel says, and when

**Decision (D05):** Include chime, walk-test names/start/end/five-minute warning, accepted arming, actual armed state, state-changing disarm, entry start and not-ready refusal. Keep the one-minute walk warning tone-only.

*Why:* Feature 08 specifies the status semantics; WALK-016 specifies names after countable beeps and the recorded walk phrases. The script has no one-minute sentence. arming.c emits ALARM_EV_ARMED at arm acceptance, so that event alone cannot justify saying Armed.

All sequences use one pack. Join the words of one phrase - a zone's entries, “Armed” “Away”, “Walk test” “Started” - with the pack's word gap (60 ms from revision 8) and put its part gap (300 ms) only before a new sentence (“You may exit now.”). Listening, 2026-10-09: the first build's words, each spoken as a sentence with its own trailing silence, 60 ms apart and 300 ms between “Armed” and “Away”, sounded like a list, not a phrase; with the words cut from carrier sentences (revision 4), 20 ms was too short (“Front” “Door”), so revision 5 puts 60 ms between them again; revision 6 has 40 ms, chosen by ear on the audio bench with the word forms (2026-10-09); revision 7, the original takes alone with their quiet ends trimmed, had 100 ms, and revision 8 has 60 ms, more natural - chosen on the voice bench with the panel joining its own pack's words (2026-10-10).

Chime: existing ding-dong, then approved zone tokens; requires voice_chime and an engine-approved chime.

Walk trip: existing three beeps, then approved zone tokens, independently of the zone’s chime flag. The walk test has no switch: its trips, start, end and five-minute warning speak whenever the voice is ready, Announcements off too (decided 2026-10-12: the walk test is the installer's own test, and a zone with words should say them); a zone without approved words keeps its beeps alone. Trigger from ALARM_SOUND_WALK_TEST, which occurs on repeated trips, rather than the first-mark-only WALK_TEST_ZONE event.

Walk start: walk.walk_test + walk.started, after the existing bell test pulse and any acknowledgement finish. Do not change the bell test.

Walk end: walk.walk_test + walk.ended + walk.all_tested when tested equals total, otherwise walk.some_untested. A zero-total test uses All zones tested because no counted zones remain untested. Cancel pending trip names when the test ends.

Five-minute warning: existing double beep, then walk.walk_test + walk.ends_soon. Require the current test and warning generation still to match.

One-minute warning: existing double beep only. Do not reuse Ends in five minutes or synthesize a sentence.

Accepted arm with exit delay: acknowledgement, then status.arming + actual mode part + status.exit_now, then the exit beeps, which wait for the words (AUD-034). Modes map Away to status.away, Stay to status.home and Night to status.night. No arming speech on refusal. An instantaneous arm omits this sequence.

Actual new armed state: status.armed + actual mode, then status.no_entry_delay when the actual flags disable entry delay. Use the matching post-command snapshot and a known new arming cycle; announce the actual auto-stay result. Do not announce an entry-expiry transition as a new arming.

Disarm: status.disarmed only when an accepted action moves the partition from exit/armed/entry state to disarmed. An already-disarmed code entry does not speak Disarmed. Ending a walk test uses its walk-end sequence.

Entry start: status.entry once, mixed below unchanged entry beeps; cancel on disarm, alarm, expiry or changed entry generation.

Not-ready arming refusal: existing error tone, then status.not_ready, only for the engine’s not-ready result. Do not enumerate open zones.

Explicit silent exit suppresses the arming and resulting armed confirmations for that entire arming cycle. Latch the choice at acceptance because auto-stay clears the flag later. exit_sounds=false suppresses the exit-phase arming sequence; an otherwise ordinary final armed confirmation remains eligible. Home/Night’s normally quiet progress tones alone do not suppress their single optional confirmations.

Automatic speech requires voice_enabled, its relevant sub-switch, valid audio, nonzero user volume and no power saving; the walk test's needs only the last three. No automatic speech for startup, restored state, history replay, reconnection, wrong codes, troubles, alarm loops, walk extension or hidden events. The first snapshot establishes a silent baseline. On a panel-link keypad a baseline taken in an exit time joins that arming cycle (its silent exit latched from the snapshot), so the exit's end is announced as on its master: the keypad came to the cycle late, the cycle isn't old (docs/features/11 LINK-028).

Speak only for the local keypad partition: partition 1 on an alarm panel; on a panel-link keypad, its home partition, from what its master sends, with its own Announcements switches and Voice volume (docs/features/11 LINK-028, decided 2026-10-16). Additional enabled partitions do not globally disable voice; their events are not spoken and no partition prefix is added. Currently audible fire, CO, burglary or auxiliary sounders cancel optional speech, including when another partition is sounding. Ignore silent-event flags and hidden reports rather than creating a distinctive cancellation. A duress arm/disarm produces exactly the ordinary confirmation for the same accepted action.

## Settings

**Decision (D06):** Store voice_enabled=true, voice_status=true and voice_chime=true (Announcements on, as on modern panels: decided 2026-10-12; it was off before; a panel without the `voice` key reads it as on) (a fourth, voice_walk, was dropped on 2026-10-12: the walk test always speaks; the key an older firmware stored, `voice_wk`, is no longer read). Put them under Settings > Screen and sound > Announcements and the equivalent Panel Manager section.

*Why:* every release installs its accepted voice (ADR-053), so announcements work from the first start, as on modern panels; nothing is spoken until a voice is ready, and the existing volume/power policy applies (AUD-015). Feature 14 gives ordinary announcement controls Screen and sound access, with pack maintenance reserved for the system master or installer.

Use the existing keypad preferences/NVS path rather than alarm behavior options for these three switches. The page contains Announcements, Status messages and Zone chime switches, availability and Preview; no voice language or pack rows, since Time and language sets the language (decided 2026-10-12). Sub-switches retain their choices while the main switch is off. Readiness, not presence, decides: enabling Announcements and Preview need the signed pack, decoder resources and mandatory clips to be valid, so an installed pack that is not ready shows Voice is unavailable. and enabling is refused with a sentence (…because the voice pack is not installed. / …because voice is unavailable.); installing a pack never enables it automatically. On the touchscreen the page is a child of Screen and sound, titled Announcements and reached by a link under Sound; Volume stays on Screen and sound and the page says Uses the volume set in Screen and sound. Use the existing user volume, including zero, without changing its current value or adding a separate voice-volume setting. Explicit Preview can audition with the main switch off and never changes preferences.

Ordinary controls and Preview follow existing Screen and sound access and lockdown, including the repository’s disarmed/silent settings gate, and are not limited to masters or installers. Pack install/removal requires system-master or installer authorization. Zone recipes remain installer-only. Standard chime code requirements remain unchanged. AUD-025 is an independent developer switch: no preference, Preview, pack installation or factory-default migration may clear it during a dev update.

## Stored format: the zone words and their revisions

**Decision (D07):** Add eight little-endian token IDs plus durable binding, approval and pack revisions to the existing configuration codec. Use a global non-reusing revision counter instead of a name fingerprint.

*Why:* The codec supports exact-length K_BYTES and additive fields with old readers skipping unknown IDs. A monotonic identity avoids hash collisions and prevents a rename-back or reused zone slot from reviving approval.

Add system field 0x1E, F_UINT width 4: voice_revision_counter. Add zone field 0x0B, K_BYTES exactly 16: spoken_words_le[16], eight LE u16 IDs. Entries form a nonzero prefix followed by zeros; holes are invalid and repetitions are allowed. Add zone 0x0C, F_UINT width 4: voice_binding_revision; 0x0D, F_UINT width 4: voice_approved_revision; 0x0E, F_UINT width 4: voice_pack_revision. Use explicit byte accessors, never native-endian uint16_t arrays under K_BYTES. Empty recipe implies source NONE; a nonempty v1 recipe explicitly has the only supported source, LIBRARY/en-US/vocabulary-schema-1. Future imported sources receive new fields rather than reinterpreting these IDs.

Allocate a new nonzero binding revision from the persisted counter on creation, first voice edit of a migrated zone, rename, partition move or sensor reassignment. Approval records the current binding revision and active pack revision. Same-identity type changes preserve these values. Removal zeroes the voice fields; the system counter survives. Never wrap the counter: exhaustion disables optional approval and reports unavailable voice while alarm configuration remains usable. Shared setters compare actual old/new identity fields and cannot trust a client-supplied approval. Source reassignment must durably invalidate approval before committing the new source binding; a failed invalidation write leaves that source change unapplied. On the panel `zone_service` does this for every bind and unbind whose sensors differ (`zone_hub_same_sensors`): the zone's source-change epoch moves on and the zone is settling (`zone_service_voice_settling`: approval and automatic zone speech refuse it) until that epoch's invalidation and binding are both durably stored; the invalidation is one alarm-task transaction on the running configuration (`alarm_service_voice_invalidate`, so a concurrent edit is never reverted) that clears only the epochs it captured, the bindings are encoded only when no zone owes one, and a failed invalidation refuses the bind as busy - or, for a replacement made under a zone source's own hold of the zone lock (Zigbee), leaves the bindings unstored until it succeeds. Runtime requests also carry zone number, configuration generation and active-pack generation. Approval and withdrawal are their own alarm-task transactions, `alarm_engine_voice_approve(engine, zone, expected_generation, expected_binding, words[8], pack_revision, now)` and `alarm_engine_voice_withdraw(engine, zone, expected_generation, expected_binding, now)`, refused as stale unless the configuration generation, binding revision and words are still the ones shown; a configuration write keeps the stored approval whatever it carries, and a backup restore stores the file's configuration only through `alarm_config_voice_restore`, which clears every approval and allocates fresh binding revisions above both configurations. The touchscreen's Done captures the generation, the zone's stored binding revision and the revision of the pack whose words its editor showed, and the zone page's save and that approval are one transaction (`ui_backend_t.voice_apply_approve`, `ui_backend/ui_voice.h`): the save always applies and reports its own result, while approval needs a leased pack of that revision for which `vp_check_recipe` passes, the generation moved by exactly that save (or unmoved, with the binding unchanged, when nothing was saved), and the stored words unchanged - the installer's own rename or move in that save approves against the binding it produced.

Define numeric_id in recording-script.csv as the single registry. Assign the 221 selectable token rows IDs 1–221 in their current token-row order. Zero is none; 1–0x7FFF is the selectable namespace. Never renumber, reuse or change an ID’s meaning; retain retired IDs as disabled registry rows. Status IDs: 0x8001 arming, 0x8002 armed, 0x8003 away, 0x8004 home, 0x8005 night, 0x8006 exit_now, 0x8007 no_entry_delay, 0x8008 disarmed, 0x8009 entry, 0x800A not_ready. Walk IDs: 0x8101 walk_test, 0x8102 started, 0x8103 ended, 0x8104 ends_soon, 0x8105 all_tested, 0x8106 some_untested. Generate constants from that registry and check them in CI.

Keep ACFG format=1 and compat=1. Add frozen codec_fixture_v6.h without editing v1–v5. The extra TLV allowance is seven system bytes and 40 bytes per used zone: change the bound’s 272u to 279u and zone term 80u to 120u, leaving ALARM_NAME_BLOB_EXTRA unchanged. Missing fields default to zero. Unknown or unavailable pack tokens invalidate speech readiness, not the alarm configuration or stored draft.

## The voice pack: RVOC format 1

**Decision (D08):** Use RVOC format 1: a 128-byte header, 64-byte sorted clip index, strings and independently decodable length-prefixed Opus clips. Store one signed .robovoice blob as kind 2/name primary.

*Why:* This preserves ADR-045’s whole-blob replacement and the budget. payload_length at offset 8 permits shared signed-envelope handling with language packs. Raw Opus requires explicit encoder-delay and final-trim metadata that the current size-report encoder does not retain.

All integers are little-endian. Header offsets: 0 magic RVOC[4]; 4 u16 format=1; 6 u16 header_size=128; 8 u32 payload_length, including header/index/strings/audio but excluding sector padding/signature; 12 u32 pack_revision, nonzero; 16 u32 vocabulary_schema=1, or 2 with endings; 20 u32 flags, bit 0 DRAFT and all other bits zero; 24 locale[16], NUL-terminated and zero-padded, en-US; 40 voice_id[32], qwen3-designed_female; 72 u32 sample_rate=16000; 76 u16 codec=1 for Opus; 78 u8 channels=1; 79 u8 frame_ms=20; 80 u16 clip_count, 1–512; 82 u16 index_entry_size=64; 84 u32 index_offset=128; 88 u32 strings_offset; 92 u32 strings_length, at most 32768; 96 u32 audio_offset; 100 u32 audio_length; 104 u16 word_gap_ms, 0–100 (60 in the supplied packs from revision 8; 100 in revision 7, 40 in revision 6, 60 in revision 5, 20 in revision 4); 106 u16 part_gap_ms, 100–1000 (300); 108 u32 nominal_bitrate=32000, VBR; 112–127 reserved zero.

Index entry offsets: 0 u16 numeric clip ID; 2 u8 category, 1 position/2 location/3 object/4 number/5 status/6 walk/7 ending (schema 2); 3 reserved zero; 4 u32 absolute display-text offset; 8 u32 absolute script-key offset; 12 u32 absolute clip-data offset; 16 u32 clip-data length; 20 u32 valid sample count at 16 kHz; 24 u16 packet count; 26 u16 pre-skip at 16 kHz; 28–59 SHA-256 of the complete length-prefixed clip data; 60–63 reserved zero. Display text is valid UTF-8, 1–47 bytes plus NUL, without control characters. Script keys are printable ASCII, 1–63 bytes plus NUL, such as word.front. Numeric IDs and script keys are unique; index IDs ascend. Categories must match their ID namespace.

Index, strings, audio and clip starts are four-byte aligned; alignment bytes are zero. Clip data is repeated u16 packet_length followed by packet bytes, with no Ogg headers. Each packet is 1–1275 bytes and must describe exactly 320 decoded samples at 16 kHz. Allow at most 501 packets, pre_skip<=320 and valid_samples 1–160000 per clip. Require pre_skip+valid_samples<=320×packet_count and trailing discarded samples<320. Decode from a fresh/reset state, discard pre-skip, output exactly valid_samples and discard final padding. Obtain pre-skip from OpusHead’s 48 kHz count divided by three; require exact divisibility. Verify the resampled input length against the final Ogg granule metadata before extracting packets.

The signed file is a multiple of 4096 bytes, 8192–970752 bytes inclusive: 948 KiB, the largest sector-aligned file below 950 KiB. Maximum unpadded payload is 966656 bytes. Pad the unsigned payload with 0xFF to a 4096-byte boundary, then append one SBV2 signature sector using espsecure sign_data --version 2 and the existing OTA signing key. The pack-store entry is PS_KIND_VOICE=2, name primary, guaranteeing replacement rather than accumulating alternate voices.

Before activation, verify the running firmware’s trusted signature key, signed envelope and exact padding, store digest, header/reader compatibility, non-overflowing ranges, region/clip nonoverlap, zero reserved bytes, text bounds, ID ordering, hashes, exact packet framing/counts and Opus packet sample counts. Require all 16 status/walk IDs and at least one selectable token. Producer verification fully decodes every clip; a runtime decode error still cancels speech safely. Dev accepts DRAFT; release rejects it. Add independent voice_pack_fixture_v1.h and malformed fixtures.

**Vocabulary schema 2 (AUD-031, 2026-10-09):** endings. A word sounds different at the end of a
spoken name (longer, falling) from inside one ("Back patio MOTION"), so a pack may carry, for a
selectable word, its ending: index category 7, ID 0xC000 + the word's ID (words up to 0x3FFF; the
namespace 0xC001-0xFFFF), script key `<word key>.end`, the word's own display text. An ending is
never selectable (never in the picker, never in a recipe); its word must be a selectable entry of
the same pack; a word may have none. A pack with any ending is vocabulary_schema 2, which a reader
before it refuses (VP_ERR_VERSION), so the firmware that reads it ships first; schema 1 packs still
read. The playback (D11) swaps a spoken name's last word for its ending where the pack has one, and
nothing else: status and walk phrases play as recorded. Frozen fixture:
`components/core/voice_pack/test/voice_pack_fixture_v2.h`, made independently of both readers.

*Built:* the reader is `components/core/voice_pack` (its own component, not core `annunciator`). Where D08 leaves a choice it takes the strict one: the layout is canonical - header, index, strings, audio, with fewer than four zero alignment bytes between regions, between clips and after the last clip, and clips stored in index order; locale and voice_id are printable ASCII without spaces; a script key's printable ASCII includes the space; nominal_bitrate must hold its listed value and the two gaps stay within their bounds (the reader once required 60 and 300); display texts may be shared, script keys may not. A packet's sample count comes from its TOC byte (RFC 6716 3.1): code 0 only, 20 ms SILK, hybrid or CELT, mono or stereo.

Pack revisions are immutable identities. Persist the active voice’s locale, voice ID, revision and full payload SHA-256. Reject a different payload reusing the same identity, including after removal/reboot. A new revision invalidates old recipe approvals; never silently switch their voice.

## The words in the picker

**Decision (D09):** Populate the picker from the validated active pack index. Translate interface text, but show vocabulary entries exactly as recorded in the voice’s language.

*Why:* The pack must define what it can actually speak. Translating Front into a Spanish label while playing the English clip would misrepresent the selection and violate AUD-024’s explicit locale contract.

Expose only categories 1–4 as selectable words; status/walk clips never appear in the word picker. Use TR for headings, actions, availability and category labels; web equivalents use t/ht. Runtime pack display strings are data values and bypass translation; fixed literals referring to recorded words in examples/tests use NOTR or nt. Escape browser strings and copy bounded picker pages under a short pack lease so UI objects never retain pointers into a retired pack.

With an es-US interface and the en-US pack, interface chrome remains Spanish and the vocabulary/Preview remains English. Show a translated Voice language: English (United States) disclosure in Announcements and the editor. Changing the screen language neither changes the voice nor invalidates a recipe. Unsupported Spanish display-name words fail Use zone name visibly and require manual vocabulary selection. V1 accepts only the en-US/schema-1 voice; other voice locales are later work.

## The Opus decoder

**Decision (D10):** Use esphome/micro-opus 0.4.1, pinned to commit 8354085908683c6130e32a832aeec8a7ca115c51, with fixed-point generic C decoding and caller-owned state. Use alloca scratch on a preallocated 256 KiB PSRAM worker stack.

*Why:* micro-opus supplies the full Opus API, host support and allocator controls without esp_audio_codec’s P4 revision-3 restriction. Its documented allocator options support this design; P4 v1.3 performance remains a qualification task. Sources: [micro-opus 0.4.1](https://components.espressif.com/components/esphome/micro-opus/versions/0.4.1/readme), [pinned Kconfig](https://raw.githubusercontent.com/esphome-libs/micro-opus/8354085908683c6130e32a832aeec8a7ca115c51/Kconfig).

Firmware: exact component-manager dependency esphome/micro-opus =0.4.1 in panel_voice/idf_component.yml, with dependencies.lock committed. Set OPUS_FLOATING_POINT=n, OPUS_USE_ALLOCA=y and OPUS_ENABLE_XTENSA_OPTIMIZATIONS=n. Do not use lazy pseudostack allocation or fall back to internal RAM. Allocate one 262144-byte PSRAM task stack before marking voice ready; it covers the stated upper scratch estimate plus worker logic. The decode worker never writes flash/NVS. Installation uses a separate internal-stack worker.

Allocate opus_decoder_get_size(1) bytes in PSRAM and initialize with opus_decoder_init(state,16000,1); allocate no decoder per event. Reject a state size over 65536 bytes as unavailable voice. State/task allocation failure leaves tones operational. The caller-allocation API is documented by [Opus](https://www.opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html).

Host tests, simulator and preview use one cmake/alarm_opus.cmake helper fetching the same immutable commit with recursive submodules and upstream micro_opus target; OPUS_ALLOCATION_MODE=USE_ALLOCA. Enable CXX for the upstream build even though our calls use its C API. Third-party warnings remain separate from repository -Werror.

The enclosing commit pins its libopus gitlink; do not substitute a separately selected libopus version. Record the resolved gitlink and opus_get_version_string() in the checked dependency provenance. The exact nested version was not independently retrievable in this review, so no version is invented.

Put the shared codec adapter in components/services/panel_voice/src/voice_codec.c with a header providing state_bytes, init, reset, packet_check and decode_20ms. It takes caller buffers and contains no ESP-IDF calls, so the same source compiles on the host. ESP allocation/task glue stays in panel_voice. Core annunciator remains plain C11 without heap, globals or third-party allocator dependencies.

## Playback

**Decision (D11):** Decode on one panel_voice worker and feed bounded mono PCM to the existing sounder task. Keep the sounder as the sole I2S/amplifier owner and apply AUD-025 after all mixing.

*Why:* AUD-010/011 and the existing sounder architecture require decoding outside alarm/UI callbacks and preservation of tone timing. A shared final sink ensures speech cannot bypass the uncommitted bench-silence gate.

Worker priority 4; sounder retains priority 6, its 4 KiB stack and 10 ms blocks. Preallocate in PSRAM a 320-sample PCM16 decode buffer, 640-sample upsample buffer and single-producer/single-consumer ring of 12×320 PCM16 mono samples: 7680 bytes/120 ms at 32 kHz. Start with 40 ms prefetched. Allocate no event-time buffers. Decoder state resets per clip; interpolation state carries across packets within that clip.

Upsample 16→32 kHz by deterministic two-times linear interpolation: emit x[n], then (x[n]+x[n+1])/2 using int32 arithmetic and defined truncation toward zero. Carry one-sample lookahead between packets, repeat the final sample for the last interpolated position, and output exactly twice the valid input sample count. Reset at clip boundaries. The packets carry clear speech (pack revision 7, chosen 2026-10-10): the speaker stage trims each master's quiet ends to 10 ms before its first sound and 20 ms after its last, with 5 ms fades, and levels it to -15 dB of voiced RMS with the gain capped at a -1 dBFS peak, nothing limited (`tools/voice/speaker.py` `trimmed()`, `for_speaker()`). Up to revision 6 the words were raised to -6 dB with the peaks limited, which squeezed them at every volume. The panel applies the Voice volume after the renderer and before the mixer (AUD-033, `components/core/sounder` `speech_boost`): 3 dB for every 10 % (the pack's own level at 70 %, +9 dB of gain at 100 %), with a 2 ms look-ahead limiter holding the peaks at -1 dBFS wherever the gain can lift one over it (from 67 %); its held-back samples follow the utterance's last pull. Revision 6's low-volume clarity stage is gone. Use the pack's word gap (60 ms from revision 8) and its 300 ms status/walk-part gap, from library.json.

Add snd_render_pcm(m, stereo, frames, mono_pcm, available_frames, speech_active). snd_render delegates with no speech. Return speech consumption and cancellation status alongside amp/silent results. Render the existing tone timeline first, then add speech at unity (its Voice volume, AUD-032, is applied before the mixer by `speech_boost`, AUD-033). Under the entry beeps and the exit delay's fast (last 10 s) beeps use an additional -6 dB voice gain. The exit beeps before them wait at the start of a beep while there is speech or speech is due (snd_set_speech_due: an utterance queued or playing, ann_due), 10 s at most, and sound once it ends (AUD-034, chosen 2026-10-10); speech over them is at unity. Clamp only the speech contribution to available int16 headroom; otherwise never change tone gain, phase or timing. Speech participates in idle/amp decisions and wakes the task. The initial amplifier-warm-up block consumes zero speech samples.

Use sounder_play_tracked(sound), returning a nonzero ticket on admission, and a 16-entry PLAYED/CANCELED completion ring. Carry tickets through the portable one-shot queue. Associated speech starts only after the actual tone’s final block is submitted in the same output stream. Dropped/cancelled/expired tickets cancel their speech. Walk-start speech additionally waits for ALARM_PAT_TEST to finish.

Policy holds one active and four waiting requests. Priority: entry, state/walk-control confirmations, refusal/walk-trip names, chime, Preview. Higher priority supersedes lower optional speech. Coalesce repeated pending same-zone requests; preserve every engine tone. Chime/trip/refusal requests expire after 2 seconds; status/walk-control requests after 5 seconds, with earlier state invalidation. Preview expires after 5 seconds. Recheck binding/config/pack/state generations at admission and each output block. Disarm cancels arming/entry; walk end cancels trip names. Audible fire/CO/alarm/aux cancels all speech by atomic generation, and the mixer independently rejects speech over those patterns.

An underrun cancels the utterance without waiting, repeating audio or emitting an extra fallback tone. The existing tone is already preserved. Buffer cancellation takes effect in the next 10 ms block; target physical cutoff <=50 ms including the approximately 32 ms DMA queue. Target first speech <=150 ms after eligibility or its preceding tone finishes, and 20 ms decode frames <=10 ms P99. Measure these targets rather than claiming them achieved.

Finally apply the existing AUD-025 branch to the fully mixed stereo block: zero every sample and force the amp off while consumption/tickets continue normally. No second sink or direct board_audio_write path is permitted for voice.

## Installing a voice pack

**Withdrawn in part (ADR-053, 2026-10-12):** the Panel Manager's voice pack upload goes (BL-206): a language and its voice are chosen from a list and downloaded together (BL-205), and the Panel Manager's uploads will be only the user's own audio for SD (BL-044). The dev-link install stays, for the panel's language's voice only.

**Decision (D12):** Build dev-link and Panel Manager signed-pack uploads now. Defer Flasher integration and release-manifest voice download. (The Flasher's followed: BL-204, docs/features/34 FLS-031.) Load the selected pack from kind 2/name primary into PSRAM at boot.

*Why:* The dev mTLS link gives a developer a practical installation path on a bench panel; Panel Manager provides an ordinary user path without physical access. lang_packs currently owns the assets table, so voice cannot mount an independent competing store.

Add PUT /dev/v1/voice with application/octet-stream and panelctl voice install <file.robovoice> --device <explicit-enrollment>, preserving existing mTLS pins and ambiguity refusal. Add PUT /api/v1/voices using existing session, system-master/installer access and locked-state checks. Both call the same panel_voice_install transaction; neither changes voice_enabled or AUD-025.

Expose shared kind-generic operations as pack_assets.h/pack_assets.c inside the existing lang_packs component. That common broker owns the sole ps_store_t, mutex, partition access, signature envelope and panel_ota claim; language APIs become adapters. Initialize it when either language packs or voice is enabled. Do not independently mount assets in panel_voice. Keep language validation/cache behavior otherwise unchanged.

Uploads require bounded Content-Length and at most 970752 bytes in PSRAM. Flash work runs on an internal-stack worker. Refuse during armed/alarm/entry/exit, walk/fire tests, existing maintenance or OTA; recheck before activation and abort optional work when the gate closes. Validate first, admit only when actual free sectors can hold the complete new copy, write new sectors, read back and verify, then commit the table. Never delete the working copy to create room, change partitions, mount assets as a filesystem or overwrite it during ordinary firmware flashing.

At boot, after the alarm and tone path are running, read primary through the broker, verify it and copy the complete signed blob to PSRAM. Playback holds no flash mapping and needs no SD or network. Maintain one active immutable copy; replacement may temporarily hold two copies. Stop/cancel the worker, swap by generation, and free the retired copy after decoder/UI leases end. Do not adopt language packs’ append-only eight-load lifetime. Failure leaves tones and stored recipes available.

On a bench panel the tester uses the normal CrowPanel v1 build with -Profile dev, without -ChipRev v3; installs the signed draft pack through the enrolled dev link or Panel Manager; approves zone recipes; leaves installer mode; explicitly enables Announcements; and turns speaker on only when ready to hear it. Every build retains the existing dev silence work and its NVS value. 

Create separate follow-up rows for Flasher initial-install/update bundle support and release-manifest voice download/selection. Ordinary firmware updates preserve the installed voice blob.

## Reproducible audio

**Decision (D13):** Version-control the reference, canonical WAV masters, provenance and canonical framed Opus packets. Guarantee exact model-free playback-pack regeneration for every clone; restrict exact neural regeneration to the qualified identical inference stack.

*Why:* ADR-052/AUD-023 require recordings and provenance as build inputs, with no model dependency in normal builds. Seeds cannot guarantee identical neural output across platforms; [PyTorch’s reproducibility documentation](https://docs.pytorch.org/docs/2.14/notes/randomness.html) explicitly limits that guarantee.

Track assets/voice/en-US/qwen3-designed_female/reference.wav, masters/<clip_id>.wav for all 237 selected PCM16 mono 24 kHz takes, packets/<clip_id>.opuspkt, packet metadata, selected-seed manifest, complete provenance, listening-review status and redistribution notices. Keep the CSV and library.json as the canonical script/settings. Canonical framed packets total about 575 KiB and preserve exactly the selected encoded audio independently of platform encoder differences. Keep models, environments, rejected takes, audition reels, signed output packs and private signing keys ignored.

Add voice.py verify as a model-free command checking inventory/IDs, reference/master/packet SHA-256, PCM properties, nonempty audio, sample counts/pre-skip, coverage, joins, draft status and the full signed-size budget. Add voice.py pack --key <existing-key> --out <file.robovoice> to assemble deterministic header/index/strings/packet bytes and sign them; draft packing requires --allow-draft. Add verify --pack <file> for the resulting envelope. Add encode --reproduce to run the pinned existing PyAV 19.0.1 pipeline and compare packet hashes/trim metadata; add record --reproduce to compare regenerated PCM against the committed masters without adaptive retakes. Divergence fails rather than replacing canonical audio.

Every clone can recreate identical canonical packet streams and the unsigned RVOC payload with verify/pack using only the standard-library packaging tools. Re-signing is a separate operation and its envelope need not have identical bytes. Re-encoding must match the recorded encoder environment; neural reproduction additionally requires the qualified GPU/CUDA/runtime environment. There is no promise of identical Qwen waveforms across arbitrary GPUs, drivers or operating systems. The committed waveform and packet hashes are authoritative.

Record and enforce HF revisions in actual load calls for Base, tokenizer and any VoiceDesign resource used to create the reference; record full artifact hashes. Capture exact Python patch version, hash-locked transitive packages/wheels, qwen-tts, torch, transformers, CUDA/cuDNN, GPU model/architecture and driver, backend/dtype, reference text/hash, resolved generation parameters, actual per-clip seeds, spoken text/respellings, script/tool hashes, trim/level settings, and source/output hashes. Encoder provenance records PyAV, FFmpeg, libopus build/version/options and wheel hashes. Preserve word.dog seed 1334 and the current respellings. The recorded Base snapshot fd4b254389122332181a7c3db7f27e918eec64e3 is pinned; fill other revisions from the actual snapshots, never guessed main/latest values.

Qwen’s model card identifies an Apache-2.0 model license; retain the synthetic reference’s own provenance and the audio redistribution notice rather than treating the model license alone as proof of unrelated voice rights. Source: [Qwen model card](https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-Base/blob/main/README.md).

Mark all current takes listening_review=pending and the pack DRAFT. Existing machine accepted flags are duration/automated gates, not listening acceptance. A dev bench panel can audition this pack; release tooling refuses it until the maintainers accept the takes and representative joins. Firmware/release builds run verify and deterministic packaging from tracked audio/packets, never download a model or invoke TTS. Signed packs are build/release artifacts outside the application image.

## The Panel Manager

**Decision (D14):** Build Panel Manager parity for announcement preferences, availability, voice metadata and pack upload. Show per-zone spoken names read-only; defer the browser recipe editor.

*Why:* The existing web field system has no list kind. A complete browser token editor would enlarge the first build, while the touchscreen and existing Remote keypad provide the required selection workflow.

Display and sound gains the same three switches, existing volume relationship, voice-language disclosure, availability, draft status and signed .robovoice upload. The upload input accepts .robovoice and uses PUT /api/v1/voices. GET /api/v1/voices returns bounded active-pack metadata and readiness; Preview can use the existing remote keypad rather than adding a browser audio transport.

Each zone card, on Zones > All zones for every role that sees the zone and on the installer’s setup card, gains a read-only Spoken name with its joined words and empty/ready/off/missing/review state. Installer users see Choose words on the panel with a Remote keypad link. Other roles see the value only. Add a computed spoken object to zone responses, outside generic editable field descriptors: word IDs, display summary, state and binding/approval/pack revisions. Never accept that computed object as authority to approve a binding. Existing zone PUTs preserve voice fields unless their actual name/partition/source/removal change invokes shared invalidation.

Keep the same system-master/installer upload authorization and disarmed/silent gates as the panel, with the answers: too large, damaged or not signed for this panel, armed or alarm sounding, not allowed, busy. Ordinary switches and Preview follow the page’s existing access instead. Readiness, not presence, permits enabling and Preview: a failed metadata read shows Voice information could not be loaded. Try again. and keeps them off. No custom recording upload or system-prompt override in the first build. Draw the BL-044 web proposal beside today’s pages, covering roles, missing/off/draft states and upload errors, and record delegated approval before moving it into panel-pages.js. Create a separate row for full browser search/add/remove/reorder/approval/Preview parity.

## The simulator and the preview

**Decision (D15):** Run the same parser, policy, decoder, upsampler and mixer in the PC simulator. Make preview speech verification deterministic and silent by rendering WAV files and traces without opening an audio device.

*Why:* AGENTS.md requires PC simulation before panel audio. The existing waveOut simulator already uses the portable mixer; the preview can test sequencing and layouts without violating a silent bench (AUD-025).

Add a shared host voice player and compile the same voice_codec.c used by panel_voice. sim_audio remains the sole waveOut owner at 32 kHz stereo, preserving its existing desk trim/mute. host_panel forwards zone IDs and matched semantic events/snapshots. Add -VoicePack <path> to tools/ui_sim.ps1 and tools/ui_preview.ps1. The default builds/loads the canonical unsigned RVOC payload from tracked packet sources, with no model, panel connection or signing key. Host-only fixture loading is explicit; firmware never accepts unsigned packs.

Simulator scenarios exercise named/unnamed chimes, every status sequence, walk trip/start/end/five-minute/one-minute behavior, rename/reapproval, power saving, silent exit, duress equivalence and audible-alarm preemption. Listen to joins and levels on the PC before the panel audition.

Preview never opens waveOut. Use virtual time to render build/ui_preview/audio/*.wav and a JSON utterance trace containing clip IDs, joins, tone tickets, cancellation reason and generations. Verify expected sequences, sample lengths and no unexpected speech. Extend the zone/settings walk with empty/full/reordered/unknown-name/missing-pack/off/needs-review states, Cancel and Done, Spanish chrome and en-XA overflow checks. Inspect generated screenshots.

Host tests compare exact trim counts and fixed-decoder PCM fixtures, interpolation DC/impulse behavior, queue/coalescing/expiry, stale identities, tone-completion fences, underrun cancellation, unchanged tone-only PCM, unchanged entry and fast-exit timing, the exit beeps' wait for speech (AUD-034), initial-sample retention and final muted stereo/amp state. Hardware levels, P4 timing and actual speaker acceptance are A5's work on a panel.

## Requirements

**Decision (D16):** Amend existing audio/walk contracts and add AUD-026–AUD-029, CORE-CFG-020–CORE-CFG-021 and UI-045 with tagged tests.

*Why:* AUD-025 is already the bench switch, WALK-016 already owns panel walk speech, and the current last configuration/UI IDs are CORE-CFG-019 and UI-044. New tests must name the requirements they prove.

Amend AUD-008: An engine-approved chime plays its existing tone once, then a current approved spoken recipe; unavailable speech leaves that tone alone. Tests: annunciator chime eligibility/failure and sounder tone-ticket sequencing.

Amend AUD-010: Optional speech never changes essential tone samples/timing (except that the exit beeps before the last 10 s wait for speech, AUD-034) and cannot distinguish hidden alarm or duress actions. Tests: rendered tone comparison, audible preemption and paired ordinary/duress engine scenarios.

Amend AUD-012: One active/four waiting requests carry identity, state/config/pack generations and bounded expiry; superseded speech never plays. Tests: policy queue, coalescing, overflow, expiry and cancellation.

Amend AUD-015: Automatic voice defaults on (D06), speaks only with valid audio, requires valid audio to turn on and uses existing volume/power policy; explicit Preview does not enable it. Tests: preference persistence, missing resources, zero volume, power saving and preview.

Amend AUD-016: Durable binding/approval/pack revisions invalidate rename, move, reassignment and slot reuse while preserving same-identity type changes. Tests: rename-back, source change, type reset, removal/reuse, rollback and pack replacement.

Amend AUD-019: Local setup selects, orders, previews and explicitly approves at most eight vocabulary entries/10 seconds without a model or computer. Tests: recipe duration/shape and complete preview editor walk.

Amend AUD-023: Tracked reference, masters, canonical packets and provenance support hash-exact model-free packaging; changed takes require listening acceptance. Tests: clean-checkout verify/pack twice, golden payload hash, locked reproduction comparison and draft-release refusal.

Amend AUD-024: Voice locale/identity is explicit and never silently follows screen language or mixes packs. Tests: en-US voice with es-US interface and incompatible-pack rejection.

Amend AUD-025: Bench silence applies after speech/tone mixing and forces zero stereo output plus amp off without changing policy decisions. Tests: muted final-output harness, existing panelctl tests, release-profile check; hardware verification remains pending.

Add AUD-026: RVOC v1 supplies bounded immutable numeric vocabulary and status/walk clips; token IDs are never reused. Tests: frozen pack fixture, registry stability, malformed index/text/ranges and required-coverage cases.

Add AUD-027: Language and voice use one transactional assets-store owner that preserves the working pack through failed/interrupted replacement. Tests: alternating kind-1/kind-2 transactions, no-space, table-commit faults and preserved unrelated entries.

Add AUD-028: The buffered decoder preserves exact clip trimming and cannot stall the sounder on cancellation or underrun. Tests: Opus trim/reset, ring generation, underrun, wake and amplifier warm-up cases.

Add AUD-029: Dev and Panel Manager voice uploads share signature/size/state validation and never alter voice enablement or bench silence. Tests: authorization, oversized/bad signature/draft/profile/armed/OTA refusal and preference preservation.

Amend WALK-016: Panel speech uses the selected zone recipe after three beeps and the exact recorded start/end/five-minute/summary sequences; the one-minute warning remains double-beep only. Tests: all trip types/repeats, both warnings, start pulse, end results and alarm cancellation.

Add CORE-CFG-020: Zone recipes serialize eight stable LE token IDs and preserve drafts through old blobs, reboot and pack absence. Tests: config/codec suites, v1–v6 fixtures, exact v6 bytes and worst-case bound.

Add CORE-CFG-021: Non-reusing voice revisions prevent obsolete zone approvals across edits, source changes, removal/reuse and restoration. Tests: shared transaction tests, counter exhaustion and durable source-change ordering.

Add UI-045: The spoken-name editor searches, orders, previews and approves vocabulary words while explaining unsupported names and unavailable voice. Tests: preview workflow/layout, Cancel, limits, locale disclosure and armed closure.

Keep UI-044/WEB-026 mockup evidence and existing AUD-001–007 tone tests intact. Add matching browser/mock tests for switches, read-only rows, upload permissions and voice-field preservation.

## Build order

**Decision (D17):** Build the library-only first phase (BL-202, the first part of BL-044) in small contract-first units, with parallel work on configuration, packing, policy, codec and mixer. Finish host integration and a v1/dev build first; the panel audition follows.

*Why:* The feature spans independent portable, storage, UI and platform boundaries. Small units limit shared-file conflicts while keeping the complete bench workflow, rather than a standalone picker, as the completion bar.

Each code unit targets at most eight files; split it again if the actual patch exceeds that. Assign shared integration files to one owner. Audio population runs in manifest-driven batches of at most eight files, rather than one 474-file asset task.

1. Contract documents: Feature 08 README/storage, Feature 40, configuration requirements, UI requirement and authority notes; freeze this JSON’s formats/semantics first.

2. Mockups: BL-044 touchscreen/web proposals plus required scenarios; draw/check and record delegated approval before UI implementation.

3. Asset/provenance batches: reference, masters, packets, manifest, locks and notices; automated inventory/hashes, all takes draft.

4. Configuration: alarm_config.h, alarm_codec.h, codec.c, shared config validation/invalidation, config/codec tests and v6 fixture.

5. RVOC reader: core annunciator pack header/source, frozen fixture, parser tests, source manifest/CMake and README.

6. Pack tooling: pack/verify helpers, voice.py dispatch, packet extraction metadata, tool tests and tooling README.

7. Dependency lock: panel_voice manifest, firmware lock, shared host Opus helper and necessary build registrations.

8. Codec adapter: shared state/reset/decode/packet-check source/header and host trim tests.

9. PCM mixer: sounder header/source/tests and ESP wrapper API/source, including tickets, speech consumption and final silence gate.

10. Annunciator policy: bounded recipes/requests, semantic mapping, queue/cancellation tests and component build files.

11. Shared assets broker: pack_assets header/source, lang_packs adapter/CMake/README; preserve existing language tests.

12. panel_voice service: startup/resources, immutable pack leases, decode ring, installation handover, metrics and feature-off stubs.

13. Feature integration: ALARM_FEATURE_VOICE, P4 defaults/board qualification, component/source indexes and architecture checks. Reconfigure firmware after adding components/options; regenerate matching sdkconfig files where defaults changed.

14. Alarm/app hooks: extend existing sounder event/snapshot wiring and add a bounded callback-only context copy from alarm_service. Capture events before evaluating their matching snapshot; never call synchronous engine APIs inside listeners.

15. Dev upload: dev_remote route, panelctl command and tool tests.

16. Web upload/API: shared installer route, metadata/read-only spoken response, authorization tests.

17. UI backend contract: bounded voice metadata/vocabulary/Preview hooks and ESP/host implementations.

18. Touchscreen zone editor: Spoken name sheet plus type-reset preservation.

19. Touchscreen preferences: Announcements page and existing preference persistence.

20. Panel Manager UI: switches, pack upload and read-only zone row; mock and page tests.

21. Host player: shared decode/policy path and sim_audio/host_panel wiring.

22. Preview/wrappers: voice-pack arguments, editor walk, deterministic WAV/trace assertions.

23. Catalog batches: extract/build/check translations and generated contracts; inspect en-XA/es-US screenshots.

24. Final integration/evidence: targeted host/tool/web suites, architecture, generator/UI/i18n checks, normal CrowPanel v1/dev build, release-profile silence exclusion and image-size check. Listen in the PC simulator and inspect preview output. Require every existing feature, including Z-Wave, and the 85% OTA limit.

After contracts, units 3–10 can run independently with owned files; mockups can run alongside them. Broker work runs alongside policy/mixer. Service follows parser/codec/mixer/broker. UI follows approved proposals and backend/config contracts. Upload paths and host integration can run in parallel after service/policy interfaces freeze. Final integration is sequential.

Create separate backlog rows, allocating IDs with backlog.py next-id at write time, for browser recipe-editor parity; Flasher/bundle provisioning; release-manifest voice downloads; SD-only custom recordings/system overrides; other voice locales/boards; and take acceptance plus P4 hardware/soak qualification. Keep the existing feature’s future scope recorded. The first build's bar is installation, explicit enabling, zone selection and audible chime/walk/status on a bench panel, with no model, SD or ongoing computer dependency.

## Changes to the earlier plan

**Decision (D18):** Keep the whole-blob flash library and portable annunciator/panel_voice boundaries, but narrow this first build to recorded-library playback, preserve tones before chime speech, and remove obsolete runtime-TTS/storage material from the plan.

*Why:* ADR-052 already withdrew on-panel synthesis, while audio-storage.md still contains model/generated-cache/filesystem language. The first bench audition needs one complete supported path without pretending custom imports or hardware acceptance are finished.

Replace assets-filesystem paths with kind-2/primary pack-store operations and one common broker. Keep the fixed partition table, whole-copy replacement, existing signing key, one selected voice and SD-only policy for future custom recordings. Remove runtime model/TTS/cache-preparation claims. Library preparation is bounded metadata resolution plus prefetch; do not materialize duplicate audio per zone.

Change chime replacement to existing tone then name and remove the replacement/fallback state machine from this phase. Include WALK-016 now instead of leaving walk speech for later. Preserve the existing eight-token/10-second budget and current 60/300 ms audition joins. Treat browser editing/imports as explicit later rows rather than quietly claiming A2/A3 complete.

Avoid the language cache’s permanent retired-copy retention for voice. Use generation/lease handover with at most two pack copies. Use preallocated caller-owned decoder state and alloca scratch on the dedicated PSRAM stack instead of a first-event lazy pseudostack allocation. Keep all flash writers on internal stacks.

Add canonical packet sources and exact hash packaging to make clone reproducibility concrete. Keep neural regeneration and listening acceptance separate; a signed draft is usable only on the dev bench. Do not mark A0/A5 or release readiness complete from automated transcription, a successful build or the first sound on a panel.

The principal risks are fixed-point Opus performance/stack use on unqualified P4 v1.3, firmware size with all features, tonal headroom/intelligibility during entry/exit, word joins and pending take acceptance, shared-store transaction regressions, and stale identity paths outside the touchscreen. The specified host tests, metrics and a controlled audition on a bench panel address these without playing sound on a panel.

## Risks

- Current takes and joins have not received listening acceptance; keep the pack draft/dev-only.
- micro-opus is not yet qualified on P4 v1.3. Measure decoder deadlines, stack high-water marks and PSRAM peaks before release.
- Missing Opus pre-skip/end trimming would damage word boundaries even when packet hashes pass.
- Separate assets-store owners could lose language or voice table updates; one shared broker is mandatory.
- All configuration and source-binding paths must invalidate approvals, including durable ordering across separate stored blobs.
- Entry/exit intelligibility and interpolation quality require PC listening and later actual-speaker audition; essential tones retain precedence (the slow exit beeps wait for the words, AUD-034).
- The complete firmware must retain Z-Wave and remain below 85% of its OTA slot.
- Bit-identical neural inference across arbitrary machines cannot be guaranteed; committed audio/packets provide exact playback regeneration.
