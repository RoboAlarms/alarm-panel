# pack_store

**Tier:** core (portable C11) · **Features:** [37 Globalization](../../../docs/features/37-Globalization/README.md)
(design: `language-packs.md`, "The pack store (format 1)"), [39 Optimizations](../../../docs/features/39-Optimizations/README.md)
(design: `asset-packs.md`) · **Tests:** [`test/`](test/) (2 suites: `pack_store`, `asset_pack`)

A small table of blobs (language packs, voice libraries, asset packs) in a flash partition of 4096-byte
sectors. Sectors 0 and 1 are the table's two copies, A and B; sectors 2 and up hold blobs. The
flash comes in through `ps_flash_t` (read, erase, write), so the same code runs on the panel and
in the tests, and the Flasher's Python mirror reads the same bytes.

- **Table:** magic `RAPS`, format 1, entry size 64, sequence, count (at most 63), the partition's
  sectors, a CRC-32 (ISO-HDLC) over the header and the entries; an entry is kind, flags, first
  sector, length, payload length, SHA-256 and a name of 1-15 printable ASCII characters.
  `ps_table_encode()` / `ps_table_decode()` are the single copy of that layout.
- **Mount:** `ps_mount()` reads both copies and never writes: both valid, the higher sequence
  (equal: A); one valid, that one; none, an empty store (a new panel's partition may hold
  anything).
- **Writing is power-safe:** a blob only goes into sectors no current entry covers
  (`ps_alloc()` is first fit; `ps_erase_run()` and `ps_write_at()` refuse anything else), and a
  table only into the copy that is not current, with sequence + 1 (`ps_commit_put()`,
  `ps_commit_remove()`, `ps_commit_default()`). A cut at any moment leaves the old state or the
  new one. The in-memory state changes only after the write succeeded. After a failed erase or
  write (the bytes may still have reached flash) the store re-reads its flash and adopts the
  current copy; if that fails too it is `broken` and refuses writes until `ps_mount()` works. A
  current sequence of 0xFFFFFFFF restarts at 1 in the other copy and the old one is then erased.
- **One whole install** (AUD-027): `ps_envelope()` checks a signed file's shape (payload length
  at offset 8, padding to whole sectors, one signature sector, optionally 0xFF padding);
  `ps_put_blob()` runs the install in the only safe order - refuse a 64th entry, admit only when a
  free run holds the whole new copy beside the working one, ask the caller's gate, erase, write,
  read every sector back and compare, ask the gate again, commit - so any failure before the
  commit leaves the working copy and every other entry. Language and voice packs both use it
  (`lang_packs/pack_assets`).
- **Carried unchanged:** entries of a kind this firmware does not know, and unknown flag bits.
  Flag bit 0 is *default* (at most one language has it; a stored table with several is read, the
  first wins). Two entries with the same kind and name make a table invalid. Kinds: 1 language,
  2 voice, 3 letters, 4 picture, 5 certificates, 6 web (`PS_KIND_*`).
- **Asset packs** (`asset_pack.h`, OPT-012): `asset_pack_open()` is the strict reader of an asset
  pack's payload, `RAST` format 1 (docs/features/39 asset-packs.md): a 128-byte header, 1-16 part
  records of 32 bytes, the parts in a canonical layout, each refusal with its own result
  (`asset_pack_result_name()`); the parts are used in place, nothing is copied or allocated.
  `tools/asset_pack.py` writes the file and the Flasher's `packstore.py` `read_asset` reads it.

## Public API
| Header | What it offers |
|---|---|
| `include/pack_store/pack_store.h` | `ps_mount`, `ps_find`, `ps_free_sectors`, `ps_alloc`, `ps_erase_run`, `ps_write_at`, `ps_commit_*`, `ps_envelope`, `ps_put_blob`, `ps_table_encode/decode` |
| `include/pack_store/asset_pack.h` | `asset_pack_open`, `asset_pack_result_name`, `asset_pack_part`, `asset_pack_find`, `asset_pack_is_draft`, `asset_pack_made_for`, `asset_pack_store_name` |

## Depends on
- Project components: none (its own small CRC-32).
- ESP-IDF: none (portable).

## Options
None.

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; the host build stays
  warning-free.
- The byte layout is an external contract (the Flasher reads it): never change it; a new format
  adds `test/pack_store_fixture_v2.h`, and `pack_store_fixture_v1.h` is never edited. The same for
  the asset pack file: `test/asset_pack_fixture_v1.h` is never edited, and the C reader and
  `packstore.py` change together.

## Tests
`pack_store` (`test_pack_store.c`, tag `I18N-044`): the CRC check value, the layout and round trip,
refused tables, mount of every A/B combination (equal sequences, one copy, 19 kinds of invalid
copy, random / zero / erased garbage), first fit with holes, no space, the full table, put, name
and flag rules, unknown kinds and flags carried, a failed commit leaving the state alone, **a
power cut at every erase and write of a sequence of puts, a replace, a default change and a
remove (four ways each: nothing done, half done, done but reported failed, a write torn inside the
table's header), then again without a remount, carrying on in the same store**, a failed re-read
breaking the store until a mount, the sequence wrap with a cut at every step, duplicate entries,
on a RAM flash with NOR semantics, and the frozen table fixture decoded field by field and re-encoded identical.

`asset_pack` (`test_asset_pack.c`, tag `OPT-012`): the frozen fixture field by field, every
truncation, a mutation of every header and part field, gaps, overlaps, misplaced parts, alignment
bytes, every kind and flag value, names and versions, part counts 0, 16 and 17, overflowing
offsets and sizes, and `asset_pack_made_for` (OPT-013).
