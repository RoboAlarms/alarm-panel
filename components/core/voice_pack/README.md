# voice_pack

**Tier:** core (portable C11) · **Features:** [08 Audio and annunciation](../../../docs/features/08-Audio-and-Annunciation/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `voice_pack`)

The reader of an RVOC format 1 voice pack (docs/features/08
[voice-first-build.md](../../../docs/features/08-Audio-and-Annunciation/voice-first-build.md)
"The voice pack: RVOC format 1", D08): the recorded words the panel can speak, with numeric IDs
from the registry in `recording-script.csv` (D07). The caller hands over the pack's memory (the
signed file as stored, or its payload); `vp_open_signed()` / `vp_open()` check every rule of D08
and fill a small `vp_pack_t` that points into that memory. The accessors then find a clip by ID,
walk the picker's words by category, walk a clip's Opus packets, and check a spoken name against
the 8-entry, 10-second budget (D02). It owns no memory, task or decoder, does not check the
signature (the platform does, before) and does not decide policy: the DRAFT flag, the locale and
the voice are reported for the annunciator and the voice service to accept or refuse (AUD-024).

A deliberate deviation from the task's wording ("the RVOC reader ... in core `annunciator`"): the
reader is its own component, so the request policy and the reader don't share one CMakeLists.

## Public API
| Header | What it offers |
|---|---|
| `include/voice_pack/voice_pack.h` | `vp_open()`, `vp_open_signed()`, `vp_err_name()`, `vp_is_draft()`, `vp_category_selectable()`, `vp_clip_at()`, `vp_find()`, `vp_next_in_category()`, `vp_packets_begin()`/`vp_packet_next()`, `vp_opus_samples_16k()`, `vp_check_recipe()`; `vp_pack_t`, `vp_clip_t`, `vp_problem_t`, `vp_err_t`, the format's limits (`VP_*`) |
| `include/voice_pack/vp_words.h` | the spoken-name editor's logic (D03): `vp_match_name()` (Use zone name: a display name to a recipe, or the unmatched spans) and `vp_list_words()` (alphabetical pages of 64, prefix search); `vp_name_match_t`, `vp_word_page_t` |
| `include/voice_pack/vp_accept.h` | what the panel accepts (D08, D12): `vp_accept()` (en-US `qwen3-designed_female` only, DRAFT refused on a release build) and the immutable identity: `vp_identity_of()`, `vp_identity_key()`, `vp_identity_encode()`/`_decode()` (the 88-byte NVS record, a stored format), `vp_identity_judge()` (new, same, other audio, collision) |
| `include/voice_pack/voice_ids.h` | The fixed status and walk-test clip IDs (`VP_ID_ARMING` 0x8001 .. `VP_ID_WALK_SOME_UNTESTED` 0x8106) and the namespace bounds: the C side of the ID registry |

## Depends on
- Project components: `alarm_proto` (private: `proto_sha256`, the project's portable SHA-256)
- ESP-IDF: none (portable)

## Options
None.

## Rules
- RVOC format 1 is a stored format: a pack one producer writes must open on every later
  firmware. `test/voice_pack_fixture_v1.h` is frozen (never edit or regenerate it); a format change
  adds `voice_pack_fixture_v2.h` beside it and a new `format` value, never a reinterpretation.
- `voice_ids.h` is the registry's C side: an ID is never renumbered, reused or given another
  meaning. Keep one `#define VP_ID_<NAME> 0x....u` per line (a tool test reads them).
- A pack's layout is canonical: header, index, strings, audio, with only fewer than four zero
  alignment bytes between regions and between clips; clips follow the index's order.
- Portable rules (components/core/AGENTS.md): C11, no heap, no globals; warning-free (`STRICT`).

## Tests
`ctest --test-dir build/host -R voice_pack` after the host build (CONTRIBUTING.md "Build and
test").
- The frozen fixture opens and reads back; the signed file's padding and size (AUD-026, AUD-013).
- The registry's status and walk IDs (AUD-026); the locale and DRAFT flag reported (AUD-024).
- Every rejection reason on a byte-patched copy: header, layout, index, text, keys, clips,
  framing, Opus packets, sample metadata, SHA-256, coverage (AUD-026, AUD-013).
- Opus TOC sample counts (RFC 6716 3.1) and the spoken-name budget (D02).
