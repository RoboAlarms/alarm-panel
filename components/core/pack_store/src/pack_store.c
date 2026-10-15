/*
 * pack_store.c - the pack store's table format 1, mount, first fit and power-safe commits
 * (docs/features/37 language-packs.md "The pack store (format 1)", I18N-044).
 */
#include "pack_store/pack_store.h"

#include <string.h>

#define HDR_SIZE 32u
#define ENTRY_SIZE 64u
#define FORMAT 1u
#define MAX_SECTORS 0xFFFFFu

static const uint8_t k_magic[4] = {0x52, 0x41, 0x50, 0x53}; /* "RAPS" */

/* ------------------------------------------------------------------ bytes and CRC */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* CRC-32 ISO-HDLC (zlib's crc32), bitwise; `crc` is the running value (start 0xFFFFFFFF). */
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ entries */

/* 1..15 printable ASCII characters and a NUL inside the 16 bytes. */
static bool name_valid(const char name[PS_NAME_MAX + 1])
{
    size_t n = 0;
    while (n <= PS_NAME_MAX && name[n] != '\0') {
        unsigned c = (unsigned char)name[n];
        if (c < 0x20u || c > 0x7Eu) {
            return false;
        }
        n++;
    }
    return n >= 1u && n <= PS_NAME_MAX;
}

static bool run_valid(uint32_t first, uint32_t length, uint32_t sectors)
{
    if (first < 2u || length == 0u || (length % PS_SECTOR) != 0u) {
        return false;
    }
    return (uint64_t)first + (length / PS_SECTOR) <= (uint64_t)sectors;
}

static bool runs_overlap(uint32_t f1, uint32_t l1, uint32_t f2, uint32_t l2)
{
    uint64_t e1 = (uint64_t)f1 + (l1 / PS_SECTOR);
    uint64_t e2 = (uint64_t)f2 + (l2 / PS_SECTOR);
    return (uint64_t)f1 < e2 && (uint64_t)f2 < e1;
}

static bool names_equal(const char *a, const char *b)
{
    return strncmp(a, b, PS_NAME_MAX + 1u) == 0;
}

/* A stored entry: false when its name is not NUL-padded ASCII. */
static bool parse_entry(const uint8_t *p, ps_entry_t *e)
{
    size_t n = 0;
    while (n < 16u && p[48 + n] != 0u) {
        unsigned c = (unsigned)p[48 + n];
        if (c < 0x20u || c > 0x7Eu) {
            return false;
        }
        n++;
    }
    if (n == 0u || n == 16u) {
        return false;
    }
    for (size_t i = n; i < 16u; i++) {
        if (p[48 + i] != 0u) {
            return false;
        }
    }
    memset(e, 0, sizeof *e);
    e->kind = p[0];
    e->flags = p[1];
    e->first_sector = rd32(p + 4);
    e->length = rd32(p + 8);
    e->payload_length = rd32(p + 12);
    memcpy(e->sha256, p + 16, 32);
    memcpy(e->name, p + 48, n);
    return true;
}

static void write_entry(uint8_t *p, const ps_entry_t *e)
{
    size_t n = 0;
    memset(p, 0, ENTRY_SIZE);
    p[0] = e->kind;
    p[1] = e->flags;
    wr32(p + 4, e->first_sector);
    wr32(p + 8, e->length);
    wr32(p + 12, e->payload_length);
    memcpy(p + 16, e->sha256, 32);
    while (n < PS_NAME_MAX && e->name[n] != '\0') {
        p[48 + n] = (uint8_t)e->name[n];
        n++;
    }
}

/* ------------------------------------------------------------------ table sector */

static void encode_list(uint8_t *out, const ps_entry_t *entries, uint16_t count, int skip,
                        const ps_entry_t *extra, uint32_t sequence, uint32_t sectors)
{
    uint32_t n = 0;
    memset(out, 0xFF, PS_SECTOR);
    memset(out + HDR_SIZE, 0, (size_t)PS_MAX_ENTRIES * ENTRY_SIZE);
    for (uint16_t i = 0; i < count; i++) {
        if ((int)i == skip) {
            continue;
        }
        write_entry(out + HDR_SIZE + (size_t)n * ENTRY_SIZE, &entries[i]);
        n++;
    }
    if (extra != NULL) {
        write_entry(out + HDR_SIZE + (size_t)n * ENTRY_SIZE, extra);
        n++;
    }
    /* what follows the last entry is erased flash */
    memset(out + HDR_SIZE + (size_t)n * ENTRY_SIZE, 0xFF,
           ((size_t)PS_MAX_ENTRIES - n) * ENTRY_SIZE);
    memset(out, 0, HDR_SIZE);
    memcpy(out, k_magic, 4);
    wr16(out + 4, FORMAT);
    wr16(out + 6, ENTRY_SIZE);
    wr32(out + 8, sequence);
    wr16(out + 12, n);
    wr32(out + 16, sectors);
    uint32_t crc = crc_update(0xFFFFFFFFu, out, 28);
    crc = crc_update(crc, out + HDR_SIZE, (size_t)n * ENTRY_SIZE);
    wr32(out + 28, ~crc);
}

ps_result_t ps_table_encode(const ps_entry_t *entries, uint16_t count, uint32_t sequence,
                            uint32_t sectors, uint8_t out[PS_SECTOR])
{
    if (out == NULL || (entries == NULL && count != 0u) || count > PS_MAX_ENTRIES ||
        sectors < 3u || sectors > MAX_SECTORS) {
        return PS_ERR_ARG;
    }
    for (uint16_t i = 0; i < count; i++) {
        if (!name_valid(entries[i].name) || !run_valid(entries[i].first_sector, entries[i].length,
                                                        sectors)) {
            return PS_ERR_ARG;
        }
        for (uint16_t j = 0; j < i; j++) {
            if (runs_overlap(entries[i].first_sector, entries[i].length, entries[j].first_sector,
                             entries[j].length) ||
                (entries[i].kind == entries[j].kind &&
                 names_equal(entries[i].name, entries[j].name))) {
                return PS_ERR_ARG;
            }
        }
    }
    encode_list(out, entries, count, -1, NULL, sequence, sectors);
    return PS_OK;
}

ps_result_t ps_table_decode(const uint8_t in[PS_SECTOR], uint32_t sectors, uint32_t *sequence,
                            ps_entry_t *entries, uint16_t *count)
{
    if (in == NULL) {
        return PS_ERR_ARG;
    }
    if (memcmp(in, k_magic, 4) != 0 || rd16(in + 4) != FORMAT || rd16(in + 6) != ENTRY_SIZE) {
        return PS_ERR_NOT_FOUND;
    }
    uint32_t n = rd16(in + 12);
    if (n > PS_MAX_ENTRIES || rd32(in + 16) != sectors) {
        return PS_ERR_NOT_FOUND;
    }
    uint32_t crc = crc_update(0xFFFFFFFFu, in, 28);
    crc = crc_update(crc, in + HDR_SIZE, (size_t)n * ENTRY_SIZE);
    if (~crc != rd32(in + 28)) {
        return PS_ERR_NOT_FOUND;
    }
    for (uint32_t i = 0; i < n; i++) {
        ps_entry_t a;
        if (!parse_entry(in + HDR_SIZE + (size_t)i * ENTRY_SIZE, &a) ||
            !run_valid(a.first_sector, a.length, sectors)) {
            return PS_ERR_NOT_FOUND;
        }
        for (uint32_t j = 0; j < i; j++) {
            ps_entry_t b;
            if (!parse_entry(in + HDR_SIZE + (size_t)j * ENTRY_SIZE, &b) ||
                runs_overlap(a.first_sector, a.length, b.first_sector, b.length) ||
                (a.kind == b.kind && names_equal(a.name, b.name))) {
                return PS_ERR_NOT_FOUND;
            }
        }
        if (entries != NULL) {
            entries[i] = a;
        }
    }
    if (sequence != NULL) {
        *sequence = rd32(in + 8);
    }
    if (count != NULL) {
        *count = (uint16_t)n;
    }
    return PS_OK;
}

/* ------------------------------------------------------------------ mount and queries */

/* Reads both copies from s->flash and adopts the current one (or an empty store). Reads only. */
static ps_result_t load(ps_store_t *s)
{
    uint32_t seq[2] = {0, 0};
    bool ok[2] = {false, false};
    int pick = -1;

    s->sequence = 0;
    s->current = -1;
    s->count = 0;
    for (uint32_t i = 0; i < 2u; i++) {
        if (!s->flash.read(s->flash.ctx, i * PS_SECTOR, s->buf, PS_SECTOR)) {
            return PS_ERR_IO;
        }
        ok[i] = ps_table_decode(s->buf, s->flash.sectors, &seq[i], NULL, NULL) == PS_OK;
    }
    if (ok[0] && ok[1]) {
        pick = (seq[1] > seq[0]) ? 1 : 0;
    } else if (ok[0]) {
        pick = 0;
    } else if (ok[1]) {
        pick = 1;
    }
    if (pick < 0) {
        memset(s->buf, 0, sizeof s->buf);
        return PS_OK;
    }
    if (!s->flash.read(s->flash.ctx, (uint32_t)pick * PS_SECTOR, s->buf, PS_SECTOR)) {
        return PS_ERR_IO;
    }
    if (ps_table_decode(s->buf, s->flash.sectors, &s->sequence, s->entries, &s->count) != PS_OK) {
        return PS_ERR_IO; /* it was valid a moment ago: the flash is misbehaving */
    }
    s->current = (int8_t)pick;
    return PS_OK;
}

ps_result_t ps_mount(ps_store_t *s, const ps_flash_t *flash)
{
    if (s == NULL || flash == NULL || flash->read == NULL || flash->erase == NULL ||
        flash->write == NULL || flash->sectors < 3u || flash->sectors > MAX_SECTORS) {
        return PS_ERR_ARG;
    }
    memset(s, 0, sizeof *s);
    s->flash = *flash;
    s->current = -1;
    return load(s);
}

static int find_index(const ps_store_t *s, uint8_t kind, const char *name)
{
    for (uint16_t i = 0; i < s->count; i++) {
        if (s->entries[i].kind == kind && names_equal(s->entries[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

const ps_entry_t *ps_find(const ps_store_t *s, uint8_t kind, const char *name)
{
    if (s == NULL || name == NULL) {
        return NULL;
    }
    int i = find_index(s, kind, name);
    return i < 0 ? NULL : &s->entries[i];
}

uint32_t ps_free_sectors(const ps_store_t *s)
{
    if (s == NULL) {
        return 0;
    }
    uint32_t used = 0;
    for (uint16_t i = 0; i < s->count; i++) {
        used += s->entries[i].length / PS_SECTOR;
    }
    uint32_t total = s->flash.sectors - 2u;
    return used >= total ? 0u : total - used;
}

static bool covered(const ps_store_t *s, uint32_t first, uint32_t length)
{
    for (uint16_t i = 0; i < s->count; i++) {
        if (runs_overlap(first, length, s->entries[i].first_sector, s->entries[i].length)) {
            return true;
        }
    }
    return false;
}

ps_result_t ps_alloc(const ps_store_t *s, uint32_t length, uint32_t *first_sector)
{
    if (s == NULL || first_sector == NULL || length == 0u || (length % PS_SECTOR) != 0u) {
        return PS_ERR_ARG;
    }
    uint64_t need = length / PS_SECTOR;
    uint64_t cand = 2;
    bool moved = true;
    while (moved) {
        moved = false;
        if (cand + need > s->flash.sectors) {
            return PS_ERR_NO_SPACE;
        }
        for (uint16_t i = 0; i < s->count; i++) {
            uint64_t f = s->entries[i].first_sector;
            uint64_t e = f + s->entries[i].length / PS_SECTOR;
            if (cand < e && f < cand + need) {
                cand = e;
                moved = true;
            }
        }
    }
    *first_sector = (uint32_t)cand;
    return PS_OK;
}

ps_result_t ps_erase_run(ps_store_t *s, uint32_t first_sector, uint32_t length)
{
    if (s == NULL || !run_valid(first_sector, length, s->flash.sectors) ||
        covered(s, first_sector, length)) {
        return PS_ERR_ARG;
    }
    if (s->broken) {
        return PS_ERR_IO;
    }
    return s->flash.erase(s->flash.ctx, first_sector * PS_SECTOR, length) ? PS_OK : PS_ERR_IO;
}

ps_result_t ps_write_at(ps_store_t *s, uint32_t first_sector, uint32_t length, uint32_t offset,
                        const void *data, size_t len)
{
    if (s == NULL || data == NULL || !run_valid(first_sector, length, s->flash.sectors) ||
        covered(s, first_sector, length) || (uint64_t)offset + len > length) {
        return PS_ERR_ARG;
    }
    if (s->broken) {
        return PS_ERR_IO;
    }
    if (len == 0u) {
        return PS_OK;
    }
    return s->flash.write(s->flash.ctx, first_sector * PS_SECTOR + offset, data, len) ? PS_OK
                                                                                      : PS_ERR_IO;
}

/* ------------------------------------------------------------------ commits */

/* After a failed erase or write the bytes may still have reached flash (a timeout, a verify
 * error): the target copy can now be a valid table. Re-read the truth so memory matches whichever
 * copy is current; when even that fails, refuse every write until a mount succeeds. */
static ps_result_t failed_io(ps_store_t *s)
{
    s->broken = load(s) != PS_OK;
    return PS_ERR_IO;
}

/* Writes the table (the entries without `skip`, then `extra`) with sequence + 1 into the copy
 * that is not current, and only then adopts it in memory. A current sequence of UINT32_MAX (a
 * foreign writer) restarts at 1 and then erases the old copy. */
static ps_result_t commit(ps_store_t *s, int skip, const ps_entry_t *extra)
{
    if (s->broken) {
        return PS_ERR_IO;
    }
    bool wrap = s->sequence == UINT32_MAX;
    uint32_t seq = wrap ? 1u : s->sequence + 1u;
    uint32_t target = (s->current == 0) ? 1u : 0u;
    uint32_t old = (uint32_t)(s->current < 0 ? 0 : s->current);

    encode_list(s->buf, s->entries, s->count, skip, extra, seq, s->flash.sectors);
    if (!s->flash.erase(s->flash.ctx, target * PS_SECTOR, PS_SECTOR)) {
        return failed_io(s);
    }
    if (!s->flash.write(s->flash.ctx, target * PS_SECTOR, s->buf, PS_SECTOR)) {
        return failed_io(s);
    }
    if (skip >= 0) {
        for (uint16_t i = (uint16_t)skip; (uint16_t)((unsigned)i + 1u) < s->count; i++) {
            s->entries[i] = s->entries[(unsigned)i + 1u];
        }
        s->count--;
    }
    if (extra != NULL) {
        s->entries[s->count] = *extra;
        s->count++;
    }
    s->sequence = seq;
    s->current = (int8_t)target;
    if (wrap && !s->flash.erase(s->flash.ctx, old * PS_SECTOR, PS_SECTOR)) {
        return failed_io(s);
    }
    return PS_OK;
}

ps_result_t ps_commit_put(ps_store_t *s, const ps_entry_t *e)
{
    ps_entry_t ne;
    size_t n;

    if (s == NULL || e == NULL || e->kind == 0u || !name_valid(e->name) ||
        !run_valid(e->first_sector, e->length, s->flash.sectors)) {
        return PS_ERR_ARG;
    }
    ne = *e;
    n = strlen(ne.name);
    memset(ne.name + n, 0, sizeof ne.name - n);
    int idx = find_index(s, ne.kind, ne.name);
    if (idx < 0 && s->count >= PS_MAX_ENTRIES) {
        return PS_ERR_FULL;
    }
    if (covered(s, ne.first_sector, ne.length)) {
        return PS_ERR_ARG;
    }
    return commit(s, idx, &ne);
}

ps_result_t ps_commit_remove(ps_store_t *s, uint8_t kind, const char *name)
{
    if (s == NULL || name == NULL) {
        return PS_ERR_ARG;
    }
    if (s->broken) {
        return PS_ERR_IO;
    }
    int idx = find_index(s, kind, name);
    if (idx < 0) {
        return PS_ERR_NOT_FOUND;
    }
    return commit(s, idx, NULL);
}

ps_result_t ps_commit_default(ps_store_t *s, uint8_t kind, const char *name)
{
    uint8_t saved[PS_MAX_ENTRIES];
    bool change = false;
    int target = -1;

    if (s == NULL) {
        return PS_ERR_ARG;
    }
    if (s->broken) {
        return PS_ERR_IO;
    }
    if (name != NULL && name[0] != '\0') {
        target = find_index(s, kind, name);
        if (target < 0) {
            return PS_ERR_NOT_FOUND;
        }
    }
    for (uint16_t i = 0; i < s->count; i++) {
        saved[i] = s->entries[i].flags;
        if (s->entries[i].kind != kind) {
            continue;
        }
        uint8_t f = s->entries[i].flags;
        f = ((int)i == target) ? (uint8_t)((unsigned)f | PS_FLAG_DEFAULT)
                               : (uint8_t)((unsigned)f & ~PS_FLAG_DEFAULT);
        if (f != s->entries[i].flags) {
            s->entries[i].flags = f;
            change = true;
        }
    }
    if (!change) {
        return PS_OK;
    }
    uint32_t seq_before = s->sequence;
    ps_result_t r = commit(s, -1, NULL);
    if (r != PS_OK && s->sequence == seq_before) { /* else a re-read adopted the new table */
        for (uint16_t i = 0; i < s->count; i++) {
            s->entries[i].flags = saved[i];
        }
    }
    return r;
}

/* ------------------------------------------------------------------ one whole install */

ps_result_t ps_envelope(const uint8_t *file, size_t len, bool ff_padding, uint32_t *payload_length)
{
    if (file == NULL || payload_length == NULL || len < 2u * PS_SECTOR || (len % PS_SECTOR) != 0u) {
        return PS_ERR_ARG;
    }
    const uint32_t pl = rd32(file + 8);
    if ((uint64_t)pl > (uint64_t)len - PS_SECTOR ||
        ((uint64_t)pl + PS_SECTOR - 1u) / PS_SECTOR + 1u != (uint64_t)len / PS_SECTOR) {
        return PS_ERR_ARG;
    }
    if (ff_padding) {
        for (size_t i = pl; i < len - PS_SECTOR; i++) {
            if (file[i] != 0xFFu) {
                return PS_ERR_ARG;
            }
        }
    }
    *payload_length = pl;
    return PS_OK;
}

ps_result_t ps_put_blob(ps_store_t *s, ps_entry_t *e, const uint8_t *file, size_t len, ps_gate_fn gate,
                        void *gate_ctx)
{
    if (s == NULL || e == NULL || file == NULL || len == 0u || (len % PS_SECTOR) != 0u || e->kind == 0u ||
        !name_valid(e->name)) {
        return PS_ERR_ARG;
    }
    if (s->broken) {
        return PS_ERR_IO;
    }
    if (len / PS_SECTOR > s->flash.sectors) { /* bigger than the partition (and than 32 bits) */
        return PS_ERR_NO_SPACE;
    }
    const uint32_t length = (uint32_t)len;
    const int old = find_index(s, e->kind, e->name);
    if (old < 0 && s->count >= PS_MAX_ENTRIES) {
        return PS_ERR_FULL;
    }
    uint32_t first = 0;
    ps_result_t r = ps_alloc(s, length, &first);
    if (r != PS_OK) {
        return r;
    }
    /* One sector at a time - erase, write, read back and compare - with the gate asked before
     * each (D12: optional work stops as soon as the panel needs itself). A closed gate or a
     * failure leaves a half-written run that no entry names: free space, the table untouched. */
    for (uint32_t off = 0; off < length; off += PS_SECTOR) {
        if (gate != NULL && !gate(gate_ctx)) {
            return PS_ERR_CLOSED;
        }
        const uint32_t sec = first + off / PS_SECTOR;
        if (ps_erase_run(s, sec, PS_SECTOR) != PS_OK ||
            ps_write_at(s, sec, PS_SECTOR, 0, file + off, PS_SECTOR) != PS_OK) {
            return PS_ERR_IO;
        }
        /* the flash must hold exactly what was written before the table names it */
        if (!s->flash.read(s->flash.ctx, sec * PS_SECTOR, s->buf, PS_SECTOR) ||
            memcmp(s->buf, file + off, PS_SECTOR) != 0) {
            return PS_ERR_IO;
        }
    }
    if (gate != NULL && !gate(gate_ctx)) {
        return PS_ERR_CLOSED;
    }
    ps_entry_t ne = *e;
    ne.flags = old >= 0 ? s->entries[old].flags : 0u; /* the default flag and unknown bits stay */
    ne.first_sector = first;
    ne.length = length;
    r = ps_commit_put(s, &ne);
    if (r != PS_OK) {
        /* A failed commit may still have landed: the store re-read its flash, so ask it. */
        const ps_entry_t *now = ps_find(s, ne.kind, ne.name);
        if (now == NULL || now->first_sector != first || memcmp(now->sha256, ne.sha256, 32) != 0) {
            return r;
        }
    }
    *e = ne;
    return PS_OK;
}
