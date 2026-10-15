/*
 * pack_store.h - the pack store: a small table of blobs (language packs, later voice libraries) in
 * a flash partition of 4096-byte sectors (docs/features/37 language-packs.md "The pack store
 * (format 1)"). Portable C11: no heap, no globals; the flash comes in through ps_flash_t.
 *
 * Sectors 0 and 1 are the table's two copies (A and B), sectors 2 and up hold blobs. A table is
 * only ever written into the copy that is not current, and a blob only into sectors no current
 * entry covers, so a power cut at any moment leaves either the old state or the new one
 * (I18N-044). The byte layout is an external contract (the Flasher reads it too): never change
 * it; a new format gets a new frozen fixture (test/pack_store_fixture_v1.h).
 */
#ifndef PACK_STORE_H
#define PACK_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS_SECTOR 4096u
#define PS_MAX_ENTRIES 63u
#define PS_NAME_MAX 15u

/* Store kinds: 1 a language pack, 2 a voice library, 3-6 asset packs (docs/features/39
 * asset-packs.md, include/pack_store/asset_pack.h). Never renumber; others are carried as they are. */
enum {
    PS_KIND_LANGUAGE = 1,
    PS_KIND_VOICE = 2,
    PS_KIND_LETTERS = 3,
    PS_KIND_PICTURE = 4,
    PS_KIND_CERTS = 5,
    PS_KIND_WEB = 6
};
#define PS_FLAG_DEFAULT 0x01u

typedef enum {
    PS_OK,
    PS_ERR_ARG,       /* a bad argument or field, or a run that a current entry covers */
    PS_ERR_IO,        /* the flash failed (or, from ps_table_decode, see PS_ERR_NOT_FOUND) */
    PS_ERR_NO_SPACE,  /* no free run long enough */
    PS_ERR_NOT_FOUND, /* no such entry (ps_table_decode: not a valid table) */
    PS_ERR_FULL,      /* the table already holds PS_MAX_ENTRIES */
    PS_ERR_CLOSED     /* ps_put_blob: the caller's gate closed; nothing was committed */
} ps_result_t;

typedef struct {
    void *ctx;
    uint32_t sectors; /* partition size / 4096, at least 3 (and at most 0xFFFFF) */
    bool (*read)(void *ctx, uint32_t offset, void *buf, size_t len);
    bool (*erase)(void *ctx, uint32_t offset, uint32_t len); /* whole sectors */
    bool (*write)(void *ctx, uint32_t offset, const void *buf, size_t len);
} ps_flash_t;

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint32_t first_sector; /* 2 or more */
    uint32_t length;       /* the blob as stored: a non-zero multiple of 4096 */
    uint32_t payload_length;
    uint8_t sha256[32];           /* of the whole stored blob */
    char name[PS_NAME_MAX + 1];   /* 1..15 printable ASCII characters, NUL-terminated */
} ps_entry_t;

typedef struct {
    ps_flash_t flash;
    uint32_t sequence; /* of the current table; 0 for an empty store */
    int8_t current;    /* 0 copy A, 1 copy B, -1 none */
    uint16_t count;
    ps_entry_t entries[PS_MAX_ENTRIES];
    uint8_t buf[PS_SECTOR];
    bool broken; /* a failed commit could not re-read the flash: writes refuse until a mount works */
} ps_store_t;

/* Reads both table copies and picks the current one (both valid: the higher sequence, equal: A;
 * one valid: it; none: an empty store, sequence 0 - a partition holding anything else is simply
 * overwritten sector by sector as blobs are written). Reads only, never writes. PS_ERR_IO when a
 * read fails. */
ps_result_t ps_mount(ps_store_t *s, const ps_flash_t *flash);

/* The entry of this kind and name, or NULL. */
const ps_entry_t *ps_find(const ps_store_t *s, uint8_t kind, const char *name);

/* Sectors no current entry covers (blob sectors only). */
uint32_t ps_free_sectors(const ps_store_t *s);

/* The lowest run of free blob sectors that holds `length` bytes (a non-zero multiple of 4096):
 * first fit. PS_ERR_NO_SPACE when there is none. */
ps_result_t ps_alloc(const ps_store_t *s, uint32_t length, uint32_t *first_sector);

/* Erases a run (whole sectors). PS_ERR_ARG when any current entry covers part of it. */
ps_result_t ps_erase_run(ps_store_t *s, uint32_t first_sector, uint32_t length);

/* Writes `len` bytes at `offset` inside a run of `length` bytes from first_sector. PS_ERR_ARG
 * when it leaves the run or any current entry covers part of the run. */
ps_result_t ps_write_at(ps_store_t *s, uint32_t first_sector, uint32_t length, uint32_t offset,
                        const void *data, size_t len);

/* Adds an entry, or replaces the one of the same kind and name (e's flags as given). The new
 * blob must not overlap ANY current entry, the one it replaces included (a cut must leave the
 * old blob whole). PS_ERR_FULL for a 64th entry, PS_ERR_ARG for bad fields. Writes the new table
 * with sequence + 1 into the copy that is not current; the in-memory state changes only after
 * the write succeeded. When the erase or write fails (PS_ERR_IO) the bytes may still have
 * reached flash, so the store re-reads both copies and adopts the current one: afterwards memory
 * is the old state or the new one, whichever the flash now holds. If that re-read fails too the
 * store is `broken`: ps_erase_run, ps_write_at and every commit return PS_ERR_IO until ps_mount
 * succeeds again. A current table with sequence 0xFFFFFFFF (only a foreign writer makes one) is
 * handled by writing sequence 1 into the other copy and then erasing the old one. */
ps_result_t ps_commit_put(ps_store_t *s, const ps_entry_t *e);

/* Removes an entry (a new table). PS_ERR_NOT_FOUND when there is none. */
ps_result_t ps_commit_remove(ps_store_t *s, uint8_t kind, const char *name);

/* Sets PS_FLAG_DEFAULT on the named entry and clears it from the kind's others (a stored table
 * with several DEFAULT flags in a kind is accepted: the first one wins for the reader); NULL or "" as
 * the name clears it from all of the kind. No table is written when nothing would change. */
ps_result_t ps_commit_default(ps_store_t *s, uint8_t kind, const char *name);

/* ---- one whole install (AUD-027, I18N-045) ---- */

/* Checks the envelope of a signed pack file (language or voice): `len` is a multiple of 4096 and
 * at least two sectors; the payload's length is the little-endian uint32 at offset 8; the file is
 * that payload padded to whole sectors plus exactly one signature sector. With `ff_padding` the
 * padding bytes must all be 0xFF. PS_OK with *payload_length set, else PS_ERR_ARG. Reads only
 * the buffer; checks no signature. */
ps_result_t ps_envelope(const uint8_t *file, size_t len, bool ff_padding, uint32_t *payload_length);

/* The gate a caller re-checks during ps_put_blob (the panel's state: armed, alarm, update...).
 * true: go on. */
typedef bool (*ps_gate_fn)(void *ctx);

/* Stores a blob and names it in the table, in the only safe order: refuse a 64th entry
 * (PS_ERR_FULL) before anything is written; admit only when a free run holds the whole new copy
 * (PS_ERR_NO_SPACE: the entry it replaces stays and counts as used, so room for both is needed:
 * the working copy is never deleted to make room); then one sector at a time: ask the gate
 * (PS_ERR_CLOSED at once), erase the sector, write it, read it back and compare it (PS_ERR_IO on
 * any failure or difference); ask the gate again (PS_ERR_CLOSED); then ps_commit_put(). `e` gives kind, name, payload_length
 * and sha256; on PS_OK it also holds first_sector, length and the flags (those of the entry it
 * replaced, else 0). When the commit reports an error the store has re-read its flash: if the
 * table now names this blob (same run and SHA-256) the result is PS_OK. Any failure before the
 * commit leaves the table and every entry as they were. `gate` may be NULL (always open). Uses
 * s->buf as scratch. */
ps_result_t ps_put_blob(ps_store_t *s, ps_entry_t *e, const uint8_t *file, size_t len, ps_gate_fn gate,
                        void *gate_ctx);

/* One table sector, as stored (the rest 0xFF). Checks the entries as a mount does. */
ps_result_t ps_table_encode(const ps_entry_t *entries, uint16_t count, uint32_t sequence,
                            uint32_t sectors, uint8_t out[PS_SECTOR]);

/* Reads and validates one table sector for a partition of `sectors` sectors. PS_ERR_NOT_FOUND
 * when it is not a valid table (two entries with the same kind and name make it invalid; the
 * encoder refuses them with PS_ERR_ARG). `entries` (room for PS_MAX_ENTRIES) and `count` may be NULL to
 * only validate; outputs are undefined on failure. */
ps_result_t ps_table_decode(const uint8_t in[PS_SECTOR], uint32_t sectors, uint32_t *sequence,
                            ps_entry_t *entries, uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif /* PACK_STORE_H */
