/*
 * test_pack_store.c - the pack store: table format 1, mount, first fit, commits, power cuts after
 * every step and the frozen table fixture (components/core/pack_store).
 * Requirement: I18N-044 (docs/features/37-Globalization, language-packs.md); ps_envelope and
 * ps_put_blob, the one install transaction language and voice packs share: AUD-027
 * (docs/features/08-Audio-and-Annunciation, voice-first-build.md "Installing a voice pack").
 */
#include <stdint.h>
#include <string.h>

#include "pack_store/pack_store.h"
#include "pack_store_fixture_v1.h"
#include "unity.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------ a NOR flash in RAM */

#define MODEL_SECTORS 640u
#define SMALL 40u /* the partition the power-cut tests use */

typedef struct {
    uint8_t mem[MODEL_SECTORS * PS_SECTOR];
    uint32_t sectors;
    uint32_t ops;     /* erases and writes so far */
    uint32_t dirty;   /* writes that needed a 0 bit to become 1: a bug in the code under test */
    uint32_t fail_at; /* the operation (1-based) that loses power; 0 never */
    int mode;         /* 0 nothing done, 1 half done, 2 all done but reported failed,
                         3 a write torn after 16 bytes (inside a table's header) */
    bool cut;         /* power is gone: every operation fails */
    uint32_t corrupt_at; /* the operation (1-based) whose write reports success but stores a wrong byte */
} model_t;

static model_t M;

static bool m_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    if ((uint64_t)off + len > (uint64_t)M.sectors * PS_SECTOR) {
        return false;
    }
    memcpy(buf, M.mem + off, len);
    return true;
}

static bool m_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    if (off % PS_SECTOR != 0u || len % PS_SECTOR != 0u ||
        (uint64_t)off + len > (uint64_t)M.sectors * PS_SECTOR || M.cut) {
        return false;
    }
    M.ops++;
    if (M.fail_at != 0u && M.ops == M.fail_at) {
        uint32_t done = M.mode == 0 ? 0u : (M.mode == 1 ? len / 2u : len);
        memset(M.mem + off, 0xFF, done);
        M.cut = true;
        return false;
    }
    memset(M.mem + off, 0xFF, len);
    return true;
}

static bool m_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    (void)ctx;
    if ((uint64_t)off + len > (uint64_t)M.sectors * PS_SECTOR || M.cut) {
        return false;
    }
    M.ops++;
    size_t n = len;
    bool fail = M.fail_at != 0u && M.ops == M.fail_at;
    if (fail) {
        n = M.mode == 0 ? 0u : (M.mode == 1 ? len / 2u : (M.mode == 3 ? 16u : len));
        if (n > len) {
            n = len;
        }
        M.cut = true;
    }
    for (size_t i = 0; i < n; i++) {
        if ((uint8_t)(M.mem[off + i] & p[i]) != p[i]) {
            M.dirty++;
        }
        M.mem[off + i] = (uint8_t)(M.mem[off + i] & p[i]);
    }
    if (M.corrupt_at != 0u && M.ops == M.corrupt_at && n > 0u) {
        M.mem[off + n - 1u] = (uint8_t)(M.mem[off + n - 1u] ^ 0x01u); /* a bit the write lost */
    }
    return !fail;
}

static void model_init(uint32_t sectors)
{
    M.sectors = sectors;
    memset(M.mem, 0xFF, (size_t)sectors * PS_SECTOR);
    M.ops = 0;
    M.dirty = 0;
    M.fail_at = 0;
    M.mode = 0;
    M.cut = false;
    M.corrupt_at = 0;
}

static ps_flash_t flash(void)
{
    ps_flash_t f = {NULL, M.sectors, m_read, m_erase, m_write};
    return f;
}

static ps_store_t S; /* the store under test (7 KB) */

static void mount_fresh(uint32_t sectors)
{
    ps_flash_t f;
    model_init(sectors);
    f = flash();
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_mount(&S, &f));
}

/* ------------------------------------------------------------------ helpers */

static uint8_t pat(uint8_t seed, uint32_t i)
{
    return (uint8_t)(seed * 31u + i * 7u + (i >> 8));
}

static ps_entry_t make_entry(uint8_t kind, const char *name, uint8_t flags, uint8_t seed,
                             uint32_t first, uint32_t nsec)
{
    ps_entry_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.flags = flags;
    e.first_sector = first;
    e.length = nsec * PS_SECTOR;
    e.payload_length = e.length - 100u;
    e.sha256[0] = seed;
    e.sha256[1] = (uint8_t)nsec;
    strncpy(e.name, name, PS_NAME_MAX);
    return e;
}

/* Finds room, erases, writes a blob whose bytes follow `seed`, and commits it. */
static ps_result_t put_blob(ps_store_t *s, uint8_t kind, const char *name, uint8_t flags,
                            uint8_t seed, uint32_t nsec)
{
    uint32_t first;
    uint8_t chunk[PS_SECTOR];
    ps_result_t r = ps_alloc(s, nsec * PS_SECTOR, &first);
    if (r != PS_OK) {
        return r;
    }
    r = ps_erase_run(s, first, nsec * PS_SECTOR);
    for (uint32_t k = 0; r == PS_OK && k < nsec; k++) {
        for (uint32_t i = 0; i < PS_SECTOR; i++) {
            chunk[i] = pat(seed, k * PS_SECTOR + i);
        }
        r = ps_write_at(s, first, nsec * PS_SECTOR, k * PS_SECTOR, chunk, PS_SECTOR);
    }
    if (r != PS_OK) {
        return r;
    }
    ps_entry_t e = make_entry(kind, name, flags, seed, first, nsec);
    return ps_commit_put(s, &e);
}

static bool entry_eq(const ps_entry_t *a, const ps_entry_t *b)
{
    return a->kind == b->kind && a->flags == b->flags && a->first_sector == b->first_sector &&
           a->length == b->length && a->payload_length == b->payload_length &&
           memcmp(a->sha256, b->sha256, 32) == 0 && strcmp(a->name, b->name) == 0;
}

static bool store_eq(const ps_store_t *a, const ps_store_t *b)
{
    if (a->count != b->count) {
        return false;
    }
    for (uint16_t i = 0; i < a->count; i++) {
        if (!entry_eq(&a->entries[i], &b->entries[i])) {
            return false;
        }
    }
    return true;
}

/* Every blob the current table names holds what was written (its seed is sha256[0]). */
static bool blobs_intact(const ps_store_t *s)
{
    for (uint16_t i = 0; i < s->count; i++) {
        const ps_entry_t *e = &s->entries[i];
        for (uint32_t b = 0; b < e->length; b++) {
            if (M.mem[e->first_sector * PS_SECTOR + b] != pat(e->sha256[0], b)) {
                return false;
            }
        }
    }
    return true;
}

static uint32_t tcrc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* Recomputes a table sector's CRC after a test changed a field. */
static void fix_crc(uint8_t *sec)
{
    uint8_t tmp[PS_SECTOR];
    uint32_t n = (uint32_t)sec[12] | ((uint32_t)sec[13] << 8);
    memcpy(tmp, sec, 28);
    memcpy(tmp + 28, sec + 32, n * 64u);
    uint32_t crc = tcrc(tmp, 28u + n * 64u);
    sec[28] = (uint8_t)(crc & 0xFFu);
    sec[29] = (uint8_t)((crc >> 8) & 0xFFu);
    sec[30] = (uint8_t)((crc >> 16) & 0xFFu);
    sec[31] = (uint8_t)((crc >> 24) & 0xFFu);
}

static void put_table(uint32_t copy, const ps_entry_t *e, uint16_t n, uint32_t seq)
{
    uint8_t sec[PS_SECTOR];
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(e, n, seq, M.sectors, sec));
    memcpy(M.mem + copy * PS_SECTOR, sec, PS_SECTOR);
}

static ps_result_t remount(void)
{
    ps_flash_t f = flash();
    return ps_mount(&S, &f);
}

/* ------------------------------------------------------------------ table format */

static void test_crc_is_iso_hdlc(void)
{
    /* I18N-044: the table's CRC is zlib's crc32; the standard check value */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, tcrc((const uint8_t *)"123456789", 9));
}

static void test_encode_layout_and_round_trip(void)
{
    /* I18N-044 */
    ps_entry_t in[3], out[PS_MAX_ENTRIES];
    uint8_t sec[PS_SECTOR];
    uint32_t seq = 0;
    uint16_t n = 0;

    in[0] = make_entry(PS_KIND_LANGUAGE, "es-US", PS_FLAG_DEFAULT, 1, 2, 3);
    in[1] = make_entry(PS_KIND_VOICE, "en-US", 0, 2, 5, 10);
    in[2] = make_entry(PS_KIND_LANGUAGE, "abcdefghijklmno", 0, 3, 20, 1); /* 15 characters */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(in, 3, 0xFFFF0001u, 100, sec));

    TEST_ASSERT_EQUAL_MEMORY("RAPS", sec, 4);
    TEST_ASSERT_EQUAL_UINT8(1, sec[4]);
    TEST_ASSERT_EQUAL_UINT8(0, sec[5]);
    TEST_ASSERT_EQUAL_UINT8(64, sec[6]);
    TEST_ASSERT_EQUAL_UINT8(0, sec[7]);
    TEST_ASSERT_EQUAL_HEX8(0x01, sec[8]);
    TEST_ASSERT_EQUAL_HEX8(0x00, sec[9]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, sec[10]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, sec[11]);
    TEST_ASSERT_EQUAL_UINT8(3, sec[12]);
    TEST_ASSERT_EQUAL_UINT8(0, sec[14]);
    TEST_ASSERT_EQUAL_UINT8(100, sec[16]);
    for (int i = 20; i < 28; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, sec[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(1, sec[32]); /* first entry: kind, flags */
    TEST_ASSERT_EQUAL_UINT8(1, sec[33]);
    TEST_ASSERT_EQUAL_UINT8(2, sec[36]); /* first sector 2 */
    TEST_ASSERT_EQUAL_UINT8(0x30, sec[32 + 9]); /* length 3 * 4096 = 0x3000 */
    TEST_ASSERT_EQUAL_STRING("es-US", (const char *)(sec + 32 + 48));
    TEST_ASSERT_EQUAL_UINT8(0, sec[32 + 48 + 15]);
    for (size_t i = 32u + 3u * 64u; i < PS_SECTOR; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, sec[i]);
    }
    {
        uint8_t tmp[28 + 3 * 64];
        memcpy(tmp, sec, 28);
        memcpy(tmp + 28, sec + 32, 3 * 64);
        uint32_t crc = tcrc(tmp, sizeof tmp);
        TEST_ASSERT_EQUAL_HEX32(crc, (uint32_t)sec[28] | ((uint32_t)sec[29] << 8) |
                                         ((uint32_t)sec[30] << 16) | ((uint32_t)sec[31] << 24));
    }

    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_decode(sec, 100, &seq, out, &n));
    TEST_ASSERT_EQUAL_HEX32(0xFFFF0001u, seq);
    TEST_ASSERT_EQUAL_UINT16(3, n);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(entry_eq(&in[i], &out[i]));
    }
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_decode(sec, 100, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_ERR_NOT_FOUND, ps_table_decode(sec, 101, &seq, out, &n));

    /* an empty table is valid too */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(NULL, 0, 1, 10, sec));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_decode(sec, 10, &seq, out, &n));
    TEST_ASSERT_EQUAL_UINT16(0, n);
}

static void test_encode_refuses_bad_tables(void)
{
    /* I18N-044 */
    ps_entry_t e[PS_MAX_ENTRIES + 1];
    uint8_t sec[PS_SECTOR];

    e[0] = make_entry(1, "a", 0, 1, 2, 2);
    e[1] = make_entry(1, "b", 0, 1, 3, 2); /* overlaps the first */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(e, 2, 1, 20, sec));
    e[1] = make_entry(1, "b", 0, 1, 4, 2);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(e, 2, 1, 20, sec));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(e, 2, 1, 5, sec)); /* outside */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(e, 2, 1, 2, sec));  /* < 3 sectors */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(e, PS_MAX_ENTRIES + 1u, 1, 900, sec));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(NULL, 1, 1, 20, sec));
    e[1] = make_entry(1, "b", 0, 1, 1, 1); /* first sector 1 */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(e, 2, 1, 20, sec));
}

/* ------------------------------------------------------------------ mount */

static void test_mount_reads_only_and_starts_empty(void)
{
    /* I18N-044: a blank partition is an empty store; mounting writes nothing */
    mount_fresh(SMALL);
    TEST_ASSERT_EQUAL_INT(-1, S.current);
    TEST_ASSERT_EQUAL_UINT32(0, S.sequence);
    TEST_ASSERT_EQUAL_UINT16(0, S.count);
    TEST_ASSERT_EQUAL_UINT32(0, M.ops);
    TEST_ASSERT_EQUAL_UINT32(SMALL - 2u, ps_free_sectors(&S));
    TEST_ASSERT_NULL(ps_find(&S, 1, "es-US"));

    ps_flash_t f = flash();
    f.sectors = 2;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_mount(&S, &f));
    f = flash();
    f.read = NULL;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_mount(&S, &f));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_mount(NULL, &f));
}

static void test_mount_a_and_b(void)
{
    /* I18N-044: both valid -> the higher sequence; equal -> A; one valid -> it */
    ps_entry_t ea = make_entry(1, "aaa", 0, 1, 2, 1);
    ps_entry_t eb = make_entry(1, "bbb", 0, 1, 3, 1);

    mount_fresh(SMALL);
    put_table(0, &ea, 1, 5);
    put_table(1, &eb, 1, 9);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(1, S.current);
    TEST_ASSERT_EQUAL_UINT32(9, S.sequence);
    TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "bbb"));
    TEST_ASSERT_NULL(ps_find(&S, 1, "aaa"));

    put_table(0, &ea, 1, 10);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(0, S.current);
    TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "aaa"));

    put_table(0, &ea, 1, 9); /* equal: A */
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(0, S.current);
    TEST_ASSERT_EQUAL_UINT32(9, S.sequence);
    TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "aaa"));
    TEST_ASSERT_NULL(ps_find(&S, 1, "bbb"));

    memset(M.mem, 0xFF, PS_SECTOR); /* only B */
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(1, S.current);
    TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "bbb"));
    /* the first commit goes to the copy that is not current: A */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_remove(&S, 1, "bbb"));
    TEST_ASSERT_EQUAL_INT(0, S.current);
    TEST_ASSERT_EQUAL_UINT32(10, S.sequence);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(0, S.current);
    TEST_ASSERT_EQUAL_UINT16(0, S.count);

    put_table(0, &ea, 1, 4); /* only A, and a blank B */
    memset(M.mem + PS_SECTOR, 0xFF, PS_SECTOR);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(0, S.current);
    TEST_ASSERT_EQUAL_UINT32(4, S.sequence);
}

typedef void (*mangle_fn)(uint8_t *sec);

static void m_magic(uint8_t *s) { s[2] ^= 0x01; }
static void m_format(uint8_t *s) { s[4] = 2; fix_crc(s); }
static void m_esize(uint8_t *s) { s[6] = 32; fix_crc(s); }
static void m_count64(uint8_t *s) { s[12] = 64; }
static void m_crc(uint8_t *s) { s[28] ^= 0x10; }
static void m_body(uint8_t *s) { s[32 + 20] ^= 0x01; }
static void m_sectors(uint8_t *s) { s[16] = (uint8_t)(s[16] + 1u); fix_crc(s); }
static void m_first1(uint8_t *s) { s[32 + 4] = 1; fix_crc(s); }
static void m_first0(uint8_t *s) { s[32 + 4] = 0; fix_crc(s); }
static void m_len_odd(uint8_t *s) { s[32 + 8] = 1; fix_crc(s); }
static void m_len_zero(uint8_t *s) { memset(s + 32 + 8, 0, 4); fix_crc(s); }
static void m_len_big(uint8_t *s) { s[32 + 11] = 1; fix_crc(s); } /* 16 MiB more */
static void m_end(uint8_t *s) { s[32 + 4] = SMALL - 1u; fix_crc(s); } /* runs past the end */
static void m_name_full(uint8_t *s) { memset(s + 32 + 48, 'x', 16); fix_crc(s); }
static void m_name_empty(uint8_t *s) { memset(s + 32 + 48, 0, 16); fix_crc(s); }
static void m_name_ctrl(uint8_t *s) { s[32 + 48 + 1] = 0x07; fix_crc(s); }
static void m_name_high(uint8_t *s) { s[32 + 48 + 1] = 0xC3; fix_crc(s); }
static void m_name_tail(uint8_t *s) { s[32 + 48 + 10] = 'z'; fix_crc(s); }
static void m_overlap(uint8_t *s)
{
    /* a second entry over the first one's sector */
    memcpy(s + 32 + 64, s + 32, 64);
    s[32 + 64 + 48] = 'Z';
    s[12] = 2;
    fix_crc(s);
}

static void test_mount_rejects_invalid_copies(void)
{
    /* I18N-044 */
    static const mangle_fn cases[] = {
        m_magic,    m_format,   m_esize,    m_count64,  m_crc,      m_body,
        m_sectors,  m_first1,   m_first0,   m_len_odd,  m_len_zero, m_len_big,
        m_end,      m_name_full, m_name_empty, m_name_ctrl, m_name_high, m_name_tail,
        m_overlap};
    ps_entry_t e = make_entry(1, "es-US", 0, 1, 2, 2);

    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        for (uint32_t copy = 0; copy < 2u; copy++) {
            uint8_t sec[PS_SECTOR];
            mount_fresh(SMALL);
            put_table(copy, &e, 1, 3);
            memcpy(sec, M.mem + copy * PS_SECTOR, PS_SECTOR);
            cases[c](sec);
            memcpy(M.mem + copy * PS_SECTOR, sec, PS_SECTOR);
            TEST_ASSERT_EQUAL_INT(PS_OK, remount());
            TEST_ASSERT_EQUAL_INT_MESSAGE(-1, S.current, "a mangled copy must not mount");
            TEST_ASSERT_EQUAL_UINT16(0, S.count);
            TEST_ASSERT_EQUAL_UINT32(0, S.sequence);
        }
    }
    /* the invalid copy loses to a valid one whatever its sequence */
    {
        uint8_t sec[PS_SECTOR];
        ps_entry_t good = make_entry(1, "en-GB", 0, 2, 4, 1);
        mount_fresh(SMALL);
        put_table(0, &e, 1, 50);
        memcpy(sec, M.mem, PS_SECTOR);
        m_body(sec);
        memcpy(M.mem, sec, PS_SECTOR);
        put_table(1, &good, 1, 2);
        TEST_ASSERT_EQUAL_INT(PS_OK, remount());
        TEST_ASSERT_EQUAL_INT(1, S.current);
        TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "en-GB"));
    }
}

static void test_mount_garbage_is_an_empty_store(void)
{
    /* I18N-044: a new panel's partition may hold anything */
    uint32_t x = 0x12345678u;
    for (int pass = 0; pass < 4; pass++) {
        model_init(SMALL);
        for (size_t i = 0; i < (size_t)SMALL * PS_SECTOR; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            M.mem[i] = pass == 0 ? (uint8_t)(x >> 24) : (pass == 1 ? 0x00 : (pass == 2 ? 0xFF : 0xA5));
        }
        TEST_ASSERT_EQUAL_INT(PS_OK, remount());
        TEST_ASSERT_EQUAL_INT(-1, S.current);
        TEST_ASSERT_EQUAL_UINT16(0, S.count);
        TEST_ASSERT_EQUAL_UINT32(0, S.sequence);
        TEST_ASSERT_EQUAL_UINT32(0, M.ops);
        /* the first blob goes over whatever was there, and the first table into A */
        TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", 0, 7, 2));
        TEST_ASSERT_EQUAL_INT(0, S.current);
        TEST_ASSERT_EQUAL_UINT32(1, S.sequence);
        TEST_ASSERT_EQUAL_INT(PS_OK, remount());
        TEST_ASSERT_EQUAL_UINT16(1, S.count);
        TEST_ASSERT_TRUE(blobs_intact(&S));
        TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
    }
}

/* ------------------------------------------------------------------ first fit and space */

static void test_first_fit_with_holes(void)
{
    /* I18N-044 */
    uint32_t first = 0;
    mount_fresh(16);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "a", 0, 1, 2)); /* 2..3 */
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "b", 0, 2, 2)); /* 4..5 */
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "c", 0, 3, 2)); /* 6..7 */
    TEST_ASSERT_EQUAL_UINT32(2, ps_find(&S, 1, "a")->first_sector);
    TEST_ASSERT_EQUAL_UINT32(4, ps_find(&S, 1, "b")->first_sector);
    TEST_ASSERT_EQUAL_UINT32(6, ps_find(&S, 1, "c")->first_sector);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_remove(&S, 1, "b"));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 2 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(4, first);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 1 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(4, first);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 3 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(8, first);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "d", 0, 4, 1)); /* into the hole: 4 */
    TEST_ASSERT_EQUAL_UINT32(4, ps_find(&S, 1, "d")->first_sector);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 2 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(8, first); /* sector 5 is alone */
    TEST_ASSERT_EQUAL_UINT32(9, ps_free_sectors(&S));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 8 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(8, first);
    /* 9 sectors are free, but not in one run */
    TEST_ASSERT_EQUAL_INT(PS_ERR_NO_SPACE, ps_alloc(&S, 9 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_INT(PS_ERR_NO_SPACE, put_blob(&S, 1, "big", 0, 5, 9));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_alloc(&S, 0, &first));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_alloc(&S, 100, &first));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_alloc(&S, PS_SECTOR, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
}

static void test_runs_under_entries_are_refused(void)
{
    /* I18N-044: a blob is written only into sectors no current entry covers */
    uint8_t b[16] = {0};
    mount_fresh(16);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "a", 0, 1, 2)); /* 2..3 */
    uint32_t ops = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_erase_run(&S, 2, PS_SECTOR));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_erase_run(&S, 3, 2 * PS_SECTOR));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_erase_run(&S, 1, PS_SECTOR)); /* a table sector */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_erase_run(&S, 15, 2 * PS_SECTOR)); /* past the end */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_erase_run(&S, 5, 100));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_write_at(&S, 3, 2 * PS_SECTOR, 0, b, 16));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_write_at(&S, 4, PS_SECTOR, PS_SECTOR - 8u, b, 16));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_write_at(&S, 4, PS_SECTOR, 0, NULL, 16));
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_erase_run(&S, 4, PS_SECTOR));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_write_at(&S, 4, PS_SECTOR, PS_SECTOR - 16u, b, 16));
}

static void test_full_table(void)
{
    /* I18N-044: 63 entries, the 64th is refused, a replace still works */
    char name[8];
    ps_entry_t e;
    mount_fresh(70);
    for (unsigned i = 0; i < PS_MAX_ENTRIES; i++) {
        name[0] = 'p';
        name[1] = (char)('0' + i / 10u);
        name[2] = (char)('0' + i % 10u);
        name[3] = '\0';
        TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, name, 0, (uint8_t)(i + 1u), 1));
    }
    TEST_ASSERT_EQUAL_UINT16(PS_MAX_ENTRIES, S.count);
    e = make_entry(1, "extra", 0, 99, 65, 1);
    uint32_t seq = S.sequence;
    TEST_ASSERT_EQUAL_INT(PS_ERR_FULL, ps_commit_put(&S, &e));
    TEST_ASSERT_EQUAL_UINT32(seq, S.sequence);
    e = make_entry(1, "p00", 0, 99, 65, 1); /* replaces p00 into free sector 65 */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_put(&S, &e));
    TEST_ASSERT_EQUAL_UINT16(PS_MAX_ENTRIES, S.count);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_UINT16(PS_MAX_ENTRIES, S.count);
    TEST_ASSERT_EQUAL_UINT32(65, ps_find(&S, 1, "p00")->first_sector);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_remove(&S, 1, "p05"));
    TEST_ASSERT_EQUAL_UINT16(PS_MAX_ENTRIES - 1u, S.count);
    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
}

/* ------------------------------------------------------------------ puts, names, flags */

static void test_put_rules(void)
{
    /* I18N-044 */
    ps_entry_t e;
    mount_fresh(20);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", 0, 1, 3)); /* 2..4 */
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "en-GB", 0, 2, 2)); /* 5..6 */
    uint32_t seq = S.sequence, ops = M.ops;

    e = make_entry(1, "es-US", 0, 3, 2, 3); /* the replaced blob's own sectors */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e = make_entry(1, "es-US", 0, 3, 3, 3); /* partly over it */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e = make_entry(1, "es-US", 0, 3, 6, 3); /* partly over another */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e = make_entry(0, "x", 0, 3, 8, 1); /* kind 0 */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e = make_entry(1, "x", 0, 3, 1, 1); /* a table sector */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e = make_entry(1, "x", 0, 3, 8, 1);
    e.length = 0;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e.length = 100;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    e = make_entry(1, "x", 0, 3, 19, 2); /* past the end */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, NULL));
    TEST_ASSERT_EQUAL_UINT32(seq, S.sequence);
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);

    /* a replace goes to other sectors; the old ones are free afterwards */
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", 0, 4, 3));
    TEST_ASSERT_EQUAL_UINT32(7, ps_find(&S, 1, "es-US")->first_sector);
    TEST_ASSERT_EQUAL_UINT16(2, S.count);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "fr-FR", 0, 5, 3));
    TEST_ASSERT_EQUAL_UINT32(2, ps_find(&S, 1, "fr-FR")->first_sector);
    /* the same name under another kind is another entry */
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, PS_KIND_VOICE, "es-US", 0, 6, 1));
    TEST_ASSERT_EQUAL_UINT16(4, S.count);
    TEST_ASSERT_NOT_NULL(ps_find(&S, PS_KIND_VOICE, "es-US"));
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_UINT16(4, S.count);
    TEST_ASSERT_TRUE(blobs_intact(&S));
    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
}

static void test_name_rules(void)
{
    /* I18N-044: 1..15 printable ASCII characters */
    static const char *const bad[] = {"", "a\x01", "\x7F", "caf\xC3\xA9", "a\x1F" "b", "\x80"};
    ps_entry_t e;
    mount_fresh(20);
    uint32_t ops = M.ops;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        e = make_entry(1, "tmp", 0, 1, 2, 1);
        memset(e.name, 0, sizeof e.name);
        memcpy(e.name, bad[i], strlen(bad[i]));
        TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    }
    e = make_entry(1, "tmp", 0, 1, 2, 1);
    memset(e.name, 'x', sizeof e.name); /* 16 characters, no NUL */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_commit_put(&S, &e));
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);

    e = make_entry(1, "abcdefghijklmno", 0, 1, 2, 1); /* 15: the longest */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_put(&S, &e));
    TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "abcdefghijklmno"));
    TEST_ASSERT_NULL(ps_find(&S, 1, "abcdefghijklmn"));
    TEST_ASSERT_NULL(ps_find(&S, 1, NULL));
    /* bytes after the NUL are not part of the name */
    e = make_entry(1, "ab", 0, 1, 3, 1);
    e.name[5] = 'z';
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_put(&S, &e));
    TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "ab"));
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_UINT16(2, S.count);
    TEST_ASSERT_EQUAL_STRING("ab", ps_find(&S, 1, "ab")->name);
}

static void test_unknown_kinds_and_flags_are_carried(void)
{
    /* I18N-044 */
    mount_fresh(30);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 9, "future", 0x80, 1, 1));
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", (uint8_t)(0x80 | PS_FLAG_DEFAULT), 2, 1));
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "en-GB", 0x40, 3, 1));
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 9, "other", 0x01, 4, 1)); /* kind 9's own flag 0 */

    TEST_ASSERT_EQUAL_INT(PS_ERR_NOT_FOUND, ps_commit_default(&S, 1, "fr-FR"));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, 1, "en-GB"));
    TEST_ASSERT_EQUAL_HEX8(0x80, ps_find(&S, 1, "es-US")->flags);
    TEST_ASSERT_EQUAL_HEX8(0x41, ps_find(&S, 1, "en-GB")->flags);
    TEST_ASSERT_EQUAL_HEX8(0x80, ps_find(&S, 9, "future")->flags); /* another kind: untouched */
    TEST_ASSERT_EQUAL_HEX8(0x01, ps_find(&S, 9, "other")->flags);

    uint32_t seq = S.sequence, ops = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, 1, "en-GB")); /* nothing changes */
    TEST_ASSERT_EQUAL_UINT32(seq, S.sequence);
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);

    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, 1, NULL)); /* clears */
    TEST_ASSERT_EQUAL_HEX8(0x40, ps_find(&S, 1, "en-GB")->flags);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, 1, "es-US"));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, 1, ""));
    TEST_ASSERT_EQUAL_HEX8(0x80, ps_find(&S, 1, "es-US")->flags);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_remove(&S, 1, "en-GB"));
    TEST_ASSERT_EQUAL_INT(PS_ERR_NOT_FOUND, ps_commit_remove(&S, 1, "en-GB"));

    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_UINT16(3, S.count);
    TEST_ASSERT_EQUAL_HEX8(0x80, ps_find(&S, 9, "future")->flags);
    TEST_ASSERT_EQUAL_HEX8(0x01, ps_find(&S, 9, "other")->flags);
    TEST_ASSERT_EQUAL_HEX8(0x80, ps_find(&S, 1, "es-US")->flags);
    TEST_ASSERT_TRUE(blobs_intact(&S));
}

static void two_blobs(ps_store_t *before)
{
    mount_fresh(20);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", 1, 1, 2));
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "en-GB", 0, 2, 2));
    *before = S;
}

static void test_failed_commit_keeps_the_state(void)
{
    /* I18N-044: after a failed commit memory matches the flash: the old state, or, when the whole
     * table reached flash although the call reported a failure (mode 2, the write), the new one */
    static ps_store_t before;
    for (int mode = 0; mode < 3; mode++) {
        for (int which = 0; which < 2; which++) { /* the erase, then the write, of the table */
            bool lands = mode >= 1 && which == 1; /* half of 4096 bytes holds the whole table */
            two_blobs(&before);
            M.mode = mode;
            M.fail_at = M.ops + 1u + (uint32_t)which;
            ps_entry_t e = make_entry(1, "fr-FR", 0, 3, 8, 1);
            TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_commit_put(&S, &e));
            if (lands) {
                TEST_ASSERT_EQUAL_UINT32(before.sequence + 1u, S.sequence);
                TEST_ASSERT_NOT_NULL(ps_find(&S, 1, "fr-FR"));
            } else {
                TEST_ASSERT_TRUE(store_eq(&S, &before));
                TEST_ASSERT_EQUAL_UINT32(before.sequence, S.sequence);
                TEST_ASSERT_EQUAL_INT(before.current, S.current);
            }
            TEST_ASSERT_FALSE(S.broken);

            two_blobs(&before);
            M.mode = mode;
            M.fail_at = M.ops + 1u + (uint32_t)which;
            TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_commit_default(&S, 1, "en-GB"));
            if (lands) {
                TEST_ASSERT_EQUAL_HEX8(1, ps_find(&S, 1, "en-GB")->flags);
                TEST_ASSERT_EQUAL_HEX8(0, ps_find(&S, 1, "es-US")->flags);
            } else {
                TEST_ASSERT_TRUE(store_eq(&S, &before));
            }

            two_blobs(&before);
            M.mode = mode;
            M.fail_at = M.ops + 1u + (uint32_t)which;
            TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_commit_remove(&S, 1, "es-US"));
            if (lands) {
                TEST_ASSERT_NULL(ps_find(&S, 1, "es-US"));
            } else {
                TEST_ASSERT_TRUE(store_eq(&S, &before));
            }
        }
    }
}

/* ------------------------------------------------------------------ power cuts */

enum { STEPS = 8 };

typedef struct {
    uint16_t count;
    ps_entry_t e[PS_MAX_ENTRIES];
} snap_t;

static snap_t g_snap[STEPS + 1];

static ps_result_t do_step(ps_store_t *s, int step)
{
    switch (step) {
    case 0: return put_blob(s, 1, "es-US", PS_FLAG_DEFAULT, 1, 3);
    case 1: return put_blob(s, 1, "en-GB", 0, 2, 2);
    case 2: return put_blob(s, PS_KIND_VOICE, "en-US", 0, 3, 4);
    case 3: return put_blob(s, 1, "es-US", PS_FLAG_DEFAULT, 4, 5); /* a replace */
    case 4: return ps_commit_default(s, 1, "en-GB");
    case 5: return put_blob(s, 9, "future", 0x80, 5, 1);
    case 6: return ps_commit_remove(s, 1, "en-GB");
    default: return put_blob(s, 1, "fr-FR", 0, 6, 2); /* into the freed room */
    }
}

static void take_snap(snap_t *t, const ps_store_t *s)
{
    t->count = s->count;
    memset(t->e, 0, sizeof t->e);
    for (uint16_t i = 0; i < s->count; i++) {
        t->e[i] = s->entries[i];
    }
}

static bool snap_matches(const snap_t *t, const ps_store_t *s)
{
    if (t->count != s->count) {
        return false;
    }
    for (uint16_t i = 0; i < s->count; i++) {
        if (!entry_eq(&t->e[i], &s->entries[i])) {
            return false;
        }
    }
    return true;
}

/* Runs the steps until one fails (the power cut); returns how many finished. */
static int run_steps(bool record)
{
    int done = 0;
    ps_flash_t f = flash();
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_mount(&S, &f));
    if (record) {
        take_snap(&g_snap[0], &S);
    }
    for (int i = 0; i < STEPS; i++) {
        if (do_step(&S, i) != PS_OK) {
            break;
        }
        done++;
        if (record) {
            take_snap(&g_snap[done], &S);
        }
    }
    return done;
}

static void test_power_cut_after_every_operation(void)
{
    /* I18N-044: a cut at any erase or write, in any of three ways, leaves exactly the old or the
     * new state and every blob the table names intact */
    model_init(SMALL);
    TEST_ASSERT_EQUAL_INT(STEPS, run_steps(true));
    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
    uint32_t total = M.ops;
    TEST_ASSERT_TRUE(total > 30u);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_TRUE(snap_matches(&g_snap[STEPS], &S));
    TEST_ASSERT_TRUE(blobs_intact(&S));

    for (int mode = 0; mode < 4; mode++) {
        for (uint32_t k = 1; k <= total; k++) {
            model_init(SMALL);
            M.fail_at = k;
            M.mode = mode;
            int done = run_steps(false);
            TEST_ASSERT_TRUE_MESSAGE(done < STEPS, "the cut must interrupt a step");
            TEST_ASSERT_TRUE(M.cut);

            M.cut = false; /* power is back */
            M.fail_at = 0;
            TEST_ASSERT_EQUAL_INT(PS_OK, remount());
            bool old_state = snap_matches(&g_snap[done], &S);
            bool new_state = snap_matches(&g_snap[done + 1], &S);
            if (!old_state && !new_state) {
                TEST_FAIL_MESSAGE("not the old state nor the new one");
            }
            TEST_ASSERT_TRUE_MESSAGE(blobs_intact(&S), "a blob the table names was damaged");
            TEST_ASSERT_EQUAL_UINT32(0, M.dirty);

            /* and the store works on: the step that was cut is simply run again */
            ps_result_t r = PS_OK;
            for (int i = new_state ? done + 1 : done; i < STEPS && r == PS_OK; i++) {
                r = do_step(&S, i);
            }
            TEST_ASSERT_EQUAL_INT(PS_OK, r);
            TEST_ASSERT_TRUE(snap_matches(&g_snap[STEPS], &S));
            TEST_ASSERT_EQUAL_INT(PS_OK, remount());
            TEST_ASSERT_TRUE(snap_matches(&g_snap[STEPS], &S));
            TEST_ASSERT_TRUE(blobs_intact(&S));
            TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
        }
    }
}


/* The power is back but the store is NOT remounted: it carries on with what it holds in memory.
 * Variant 0: a blob-sized alloc and erase first (the danger: a table that reached flash although
 * the commit said it failed must not have its blob erased), then a remount must show the old or
 * the new state with every named blob intact. Variant 1: the steps run on in the same store. */
static void test_power_cut_without_remount(void)
{
    /* I18N-044 */
    model_init(SMALL);
    TEST_ASSERT_EQUAL_INT(STEPS, run_steps(true));
    uint32_t total = M.ops;

    for (int variant = 0; variant < 2; variant++) {
        for (int mode = 0; mode < 4; mode++) {
            for (uint32_t k = 1; k <= total; k++) {
                model_init(SMALL);
                M.fail_at = k;
                M.mode = mode;
                int done = run_steps(false);
                TEST_ASSERT_TRUE(done < STEPS);
                M.cut = false; /* power is back; the store is not told */
                M.fail_at = 0;

                if (variant == 0) {
                    uint32_t first = 0;
                    if (ps_alloc(&S, PS_SECTOR, &first) == PS_OK) {
                        (void)ps_erase_run(&S, first, PS_SECTOR);
                    }
                    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
                    TEST_ASSERT_TRUE(snap_matches(&g_snap[done], &S) ||
                                     snap_matches(&g_snap[done + 1], &S));
                    TEST_ASSERT_TRUE_MESSAGE(blobs_intact(&S), "a blob the table names was erased");
                    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
                } else {
                    /* memory must already agree with the flash */
                    bool old_state = snap_matches(&g_snap[done], &S);
                    bool new_state = snap_matches(&g_snap[done + 1], &S);
                    TEST_ASSERT_TRUE_MESSAGE(old_state || new_state, "memory is neither state");
                    ps_result_t r = PS_OK;
                    for (int i = new_state ? done + 1 : done; i < STEPS && r == PS_OK; i++) {
                        r = do_step(&S, i);
                    }
                    TEST_ASSERT_EQUAL_INT(PS_OK, r);
                    TEST_ASSERT_TRUE(snap_matches(&g_snap[STEPS], &S));
                    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
                    TEST_ASSERT_TRUE(snap_matches(&g_snap[STEPS], &S));
                    TEST_ASSERT_TRUE(blobs_intact(&S));
                    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
                }
            }
        }
    }
}

static bool g_read_fails;

static bool m_read_switch(void *ctx, uint32_t off, void *buf, size_t len)
{
    return g_read_fails ? false : m_read(ctx, off, buf, len);
}

static void test_failed_reread_breaks_the_store_until_mount(void)
{
    /* I18N-044: when even the re-read after a failed commit fails, nothing more is written */
    ps_flash_t f;
    mount_fresh(20);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", 1, 1, 2));
    f = flash();
    f.read = m_read_switch;
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_mount(&S, &f));
    M.mode = 2;
    M.fail_at = M.ops + 2u; /* the table's write */
    g_read_fails = true;
    ps_entry_t e = make_entry(1, "fr-FR", 0, 3, 8, 1);
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_commit_put(&S, &e));
    g_read_fails = false;
    M.cut = false;
    M.fail_at = 0;
    TEST_ASSERT_TRUE(S.broken);
    uint32_t ops = M.ops;
    uint8_t b = 0;
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_erase_run(&S, 8, PS_SECTOR));
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_write_at(&S, 8, PS_SECTOR, 0, &b, 1));
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_commit_put(&S, &e));
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, ps_commit_remove(&S, 1, "es-US"));
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops); /* nothing reached the flash */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_mount(&S, &f));
    TEST_ASSERT_FALSE(S.broken);
    e = make_entry(1, "de-DE", 0, 4, 12, 1);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_put(&S, &e));
}

/* A table whose sequence is 0xFFFFFFFF is made by hand (a foreign writer). */
static void wrap_setup(void)
{
    mount_fresh(20);
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "es-US", 1, 1, 2));
    TEST_ASSERT_EQUAL_INT(0, S.current);
    put_table(0, S.entries, S.count, UINT32_MAX);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, S.sequence);
}

static void test_sequence_wrap(void)
{
    /* I18N-044: sequence 0xFFFFFFFF restarts at 1 in the other copy, then the old one is erased */
    wrap_setup();
    uint32_t base = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "en-GB", 0, 2, 2));
    uint32_t total = M.ops - base;
    TEST_ASSERT_EQUAL_UINT32(1, S.sequence);
    TEST_ASSERT_EQUAL_INT(1, S.current);
    for (uint32_t i = 0; i < PS_SECTOR; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, M.mem[i]); /* the old copy is erased */
    }
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_UINT16(2, S.count);
    TEST_ASSERT_TRUE(blobs_intact(&S));
    TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "fr-FR", 0, 3, 1)); /* and it goes on */
    TEST_ASSERT_EQUAL_UINT32(2, S.sequence);
    TEST_ASSERT_EQUAL_INT(0, S.current);

    /* a cut at every operation, in every way: the old state or the new one, then it goes on */
    for (int mode = 0; mode < 4; mode++) {
        for (uint32_t k = 1; k <= total; k++) {
            wrap_setup();
            M.mode = mode;
            M.fail_at = M.ops + k;
            TEST_ASSERT_TRUE(put_blob(&S, 1, "en-GB", 0, 2, 2) != PS_OK);
            M.cut = false;
            M.fail_at = 0;
            TEST_ASSERT_EQUAL_INT(PS_OK, remount());
            TEST_ASSERT_TRUE(S.count == 1u || S.count == 2u);
            TEST_ASSERT_TRUE_MESSAGE(blobs_intact(&S), "a blob the table names was damaged");
            TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
            if (S.count == 1u) {
                TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "en-GB", 0, 2, 2));
            }
            TEST_ASSERT_EQUAL_INT(PS_OK, put_blob(&S, 1, "fr-FR", 0, 3, 1));
            TEST_ASSERT_EQUAL_INT(PS_OK, remount());
            TEST_ASSERT_EQUAL_UINT16(3, S.count);
            TEST_ASSERT_TRUE(blobs_intact(&S));
        }
    }
}

static void test_duplicate_entries_are_invalid(void)
{
    /* I18N-044: one entry per kind and name; several DEFAULT flags are still readable */
    ps_entry_t e[3];
    ps_entry_t out[PS_MAX_ENTRIES];
    uint8_t sec[PS_SECTOR];
    uint32_t seq = 0;
    uint16_t n = 0;

    e[0] = make_entry(1, "es-US", 1, 1, 2, 1);
    e[1] = make_entry(1, "en-GB", 1, 2, 3, 1);
    e[2] = make_entry(2, "es-US", 0, 3, 4, 1); /* same name, another kind: fine */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(e, 3, 5, 20, sec));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_decode(sec, 20, &seq, out, &n));
    TEST_ASSERT_EQUAL_UINT16(3, n);
    TEST_ASSERT_EQUAL_HEX8(1, out[0].flags);
    TEST_ASSERT_EQUAL_HEX8(1, out[1].flags);

    e[1] = make_entry(1, "es-US", 0, 2, 3, 1); /* the same kind and name as e[0] */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_table_encode(e, 2, 5, 20, sec));

    e[1] = make_entry(1, "en-GB", 0, 2, 3, 1);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(e, 2, 5, 20, sec));
    memset(sec + 32 + 64 + 48, 0, 16);
    memcpy(sec + 32 + 64 + 48, "es-US", 5);
    fix_crc(sec);
    TEST_ASSERT_EQUAL_INT(PS_ERR_NOT_FOUND, ps_table_decode(sec, 20, NULL, NULL, NULL));

    /* and a mount treats it as no table */
    model_init(20);
    memcpy(M.mem, sec, PS_SECTOR);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(-1, S.current);
    TEST_ASSERT_EQUAL_UINT16(0, S.count);
}

/* ------------------------------------------------------------------ the frozen fixture */

static void test_frozen_fixture(void)
{
    /* I18N-044: a table written by the first firmware must read on every later one */
    ps_entry_t e[PS_MAX_ENTRIES];
    uint8_t again[PS_SECTOR];
    uint32_t seq = 0;
    uint16_t n = 0;

    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_decode(PACK_STORE_FIXTURE_V1,
                                                 PACK_STORE_FIXTURE_V1_SECTORS, &seq, e, &n));
    TEST_ASSERT_EQUAL_UINT32(PACK_STORE_FIXTURE_V1_SEQUENCE, seq);
    TEST_ASSERT_EQUAL_UINT16(4, n);

    TEST_ASSERT_EQUAL_UINT8(1, e[0].kind);
    TEST_ASSERT_EQUAL_HEX8(0x01, e[0].flags);
    TEST_ASSERT_EQUAL_UINT32(2, e[0].first_sector);
    TEST_ASSERT_EQUAL_UINT32(5u * 4096u, e[0].length);
    TEST_ASSERT_EQUAL_UINT32(19877, e[0].payload_length);
    TEST_ASSERT_EQUAL_HEX8(0xAA, e[0].sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x41, e[0].sha256[1]);
    TEST_ASSERT_EQUAL_STRING("es-US", e[0].name);

    TEST_ASSERT_EQUAL_UINT8(1, e[1].kind);
    TEST_ASSERT_EQUAL_HEX8(0x00, e[1].flags);
    TEST_ASSERT_EQUAL_UINT32(7, e[1].first_sector);
    TEST_ASSERT_EQUAL_UINT32(3u * 4096u, e[1].length);
    TEST_ASSERT_EQUAL_UINT32(11234, e[1].payload_length);
    TEST_ASSERT_EQUAL_HEX8(0x99, e[1].sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, e[1].sha256[1]);
    TEST_ASSERT_EQUAL_STRING("en-GB", e[1].name);

    TEST_ASSERT_EQUAL_UINT8(PS_KIND_VOICE, e[2].kind);
    TEST_ASSERT_EQUAL_HEX8(0x00, e[2].flags);
    TEST_ASSERT_EQUAL_UINT32(12, e[2].first_sector);
    TEST_ASSERT_EQUAL_UINT32(100u * 4096u, e[2].length);
    TEST_ASSERT_EQUAL_UINT32(401234, e[2].payload_length);
    TEST_ASSERT_EQUAL_HEX8(0x4B, e[2].sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1C, e[2].sha256[1]);
    TEST_ASSERT_EQUAL_STRING("en-US", e[2].name);

    TEST_ASSERT_EQUAL_UINT8(9, e[3].kind);
    TEST_ASSERT_EQUAL_HEX8(0x80, e[3].flags);
    TEST_ASSERT_EQUAL_UINT32(200, e[3].first_sector);
    TEST_ASSERT_EQUAL_UINT32(2u * 4096u, e[3].length);
    TEST_ASSERT_EQUAL_UINT32(8000, e[3].payload_length);
    TEST_ASSERT_EQUAL_HEX8(0x22, e[3].sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, e[3].sha256[1]);
    TEST_ASSERT_EQUAL_STRING("future", e[3].name);

    TEST_ASSERT_EQUAL_INT(PS_OK, ps_table_encode(e, n, seq, PACK_STORE_FIXTURE_V1_SECTORS, again));
    TEST_ASSERT_EQUAL_MEMORY(PACK_STORE_FIXTURE_V1, again, PS_SECTOR);

    /* as the partition's copy B it mounts, and the next commit goes to A and keeps "future" */
    model_init(PACK_STORE_FIXTURE_V1_SECTORS);
    memcpy(M.mem + PS_SECTOR, PACK_STORE_FIXTURE_V1, PS_SECTOR);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_INT(1, S.current);
    TEST_ASSERT_EQUAL_UINT32(7, S.sequence);
    TEST_ASSERT_EQUAL_UINT32(PACK_STORE_FIXTURE_V1_SECTORS - 2u - 110u, ps_free_sectors(&S));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, PS_KIND_LANGUAGE, "en-GB"));
    TEST_ASSERT_EQUAL_INT(0, S.current);
    TEST_ASSERT_EQUAL_UINT32(8, S.sequence);
    TEST_ASSERT_EQUAL_INT(PS_OK, remount());
    TEST_ASSERT_EQUAL_HEX8(0x00, ps_find(&S, 1, "es-US")->flags);
    TEST_ASSERT_EQUAL_HEX8(0x01, ps_find(&S, 1, "en-GB")->flags);
    TEST_ASSERT_TRUE(entry_eq(&e[3], ps_find(&S, 9, "future")));
    TEST_ASSERT_TRUE(entry_eq(&e[2], ps_find(&S, 2, "en-US")));
    /* the first hole of the fixture: sectors 10 and 11, then 112.. */
    uint32_t first = 0;
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 2 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(10, first);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_alloc(&S, 3 * PS_SECTOR, &first));
    TEST_ASSERT_EQUAL_UINT32(112, first);
}

/* ------------------------------------------------------------------ one whole install (AUD-027) */

static uint8_t FILEBUF[64u * PS_SECTOR];

/* A pack file whose bytes follow `seed` (the blob checks read sha256[0] as the seed). */
static size_t make_file(uint8_t seed, uint32_t nsec)
{
    for (uint32_t i = 0; i < nsec * PS_SECTOR; i++) {
        FILEBUF[i] = pat(seed, i);
    }
    return (size_t)nsec * PS_SECTOR;
}

static ps_entry_t wanted(uint8_t kind, const char *name, uint8_t seed)
{
    ps_entry_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    strncpy(e.name, name, PS_NAME_MAX);
    e.payload_length = 100u;
    e.sha256[0] = seed;
    return e;
}

static ps_result_t install(uint8_t kind, const char *name, uint8_t seed, uint32_t nsec, ps_gate_fn gate,
                           void *ctx)
{
    ps_entry_t e = wanted(kind, name, seed);
    size_t len = make_file(seed, nsec);
    return ps_put_blob(&S, &e, FILEBUF, len, gate, ctx);
}

/* What a restart would read: the same entries, every blob whole. */
static void assert_remount_matches(const ps_store_t *want)
{
    static ps_store_t T;
    ps_flash_t f = flash();
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_mount(&T, &f));
    TEST_ASSERT_TRUE(store_eq(&T, want));
    TEST_ASSERT_TRUE(blobs_intact(&T));
}

static void put_file_header(uint8_t *f, uint32_t pl)
{
    f[8] = (uint8_t)pl;
    f[9] = (uint8_t)(pl >> 8);
    f[10] = (uint8_t)(pl >> 16);
    f[11] = (uint8_t)(pl >> 24);
}

static void test_envelope(void)
{
    /* AUD-027: the signed file's shape - payload length at offset 8, padded to whole sectors, one
     * signature sector - for both kinds; anything else is refused before a byte is written */
    static uint8_t f[4u * PS_SECTOR];
    uint32_t pl = 0;
    memset(f, 0xFF, sizeof f);
    memset(f, 0x41, 2u * PS_SECTOR + 10u);
    put_file_header(f, 2u * PS_SECTOR + 10u);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_envelope(f, 4u * PS_SECTOR, true, &pl));
    TEST_ASSERT_EQUAL_UINT32(2u * PS_SECTOR + 10u, pl);
    put_file_header(f, 3u * PS_SECTOR); /* exactly full: no padding at all */
    memset(f + 2u * PS_SECTOR + 10u, 0x41, PS_SECTOR - 10u);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_envelope(f, 4u * PS_SECTOR, true, &pl));
    TEST_ASSERT_EQUAL_UINT32(3u * PS_SECTOR, pl);
    /* the payload overruns the signature sector, or the file has a sector too many */
    put_file_header(f, 3u * PS_SECTOR + 1u);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 4u * PS_SECTOR, false, &pl));
    put_file_header(f, 2u * PS_SECTOR);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 4u * PS_SECTOR, false, &pl));
    put_file_header(f, 0u);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 4u * PS_SECTOR, false, &pl));
    put_file_header(f, 0xFFFFFFFFu); /* no wrap-around in the arithmetic */
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 4u * PS_SECTOR, false, &pl));
    /* truncated or ragged files, no file */
    put_file_header(f, 10u);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, PS_SECTOR, false, &pl));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_envelope(f, 2u * PS_SECTOR, false, &pl));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 2u * PS_SECTOR + 1u, false, &pl));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(NULL, 2u * PS_SECTOR, false, &pl));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 2u * PS_SECTOR, false, NULL));
    /* padding that is not 0xFF: refused only when the kind asks for it */
    memset(f, 0xFF, sizeof f);
    put_file_header(f, PS_SECTOR + 10u);
    f[2u * PS_SECTOR - 1u] = 0x00;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_envelope(f, 3u * PS_SECTOR, true, &pl));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_envelope(f, 3u * PS_SECTOR, false, &pl));
    f[2u * PS_SECTOR - 1u] = 0xFF;
    f[2u * PS_SECTOR] = 0x00; /* the signature sector is not padding */
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_envelope(f, 3u * PS_SECTOR, true, &pl));
}

static void test_put_blob_alternating_kinds(void)
{
    /* AUD-027: language (kind 1) and voice (kind 2) installs and replacements alternate through the
     * one owner; each keeps the other kind's entries, flags and bytes, and a restart reads it all */
    static ps_store_t before;
    mount_fresh(SMALL);
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_LANGUAGE, "de-DE", 11, 4, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "primary", 21, 6, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_LANGUAGE, "fr-FR", 12, 3, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, PS_KIND_LANGUAGE, "de-DE"));
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_commit_default(&S, PS_KIND_VOICE, "primary"));

    /* replace the voice: the languages stay exactly as they were */
    before = S;
    ps_entry_t e = wanted(PS_KIND_VOICE, "primary", 22);
    size_t len = make_file(22, 6);
    TEST_ASSERT_EQUAL_INT(PS_OK, ps_put_blob(&S, &e, FILEBUF, len, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(6u * PS_SECTOR, e.length);
    TEST_ASSERT_EQUAL_UINT32(100u, e.payload_length);
    TEST_ASSERT_EQUAL_HEX8(PS_FLAG_DEFAULT, e.flags); /* the replaced entry's flags */
    TEST_ASSERT_NOT_EQUAL(ps_find(&before, PS_KIND_VOICE, "primary")->first_sector, e.first_sector);
    TEST_ASSERT_TRUE(entry_eq(ps_find(&S, PS_KIND_VOICE, "primary"), &e));
    TEST_ASSERT_TRUE(entry_eq(ps_find(&S, PS_KIND_LANGUAGE, "de-DE"), ps_find(&before, PS_KIND_LANGUAGE, "de-DE")));
    TEST_ASSERT_TRUE(entry_eq(ps_find(&S, PS_KIND_LANGUAGE, "fr-FR"), ps_find(&before, PS_KIND_LANGUAGE, "fr-FR")));

    /* replace a language: the voice stays */
    before = S;
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_LANGUAGE, "de-DE", 13, 4, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(13, ps_find(&S, PS_KIND_LANGUAGE, "de-DE")->sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(PS_FLAG_DEFAULT, ps_find(&S, PS_KIND_LANGUAGE, "de-DE")->flags);
    TEST_ASSERT_TRUE(entry_eq(ps_find(&S, PS_KIND_VOICE, "primary"), ps_find(&before, PS_KIND_VOICE, "primary")));
    TEST_ASSERT_TRUE(entry_eq(ps_find(&S, PS_KIND_LANGUAGE, "fr-FR"), ps_find(&before, PS_KIND_LANGUAGE, "fr-FR")));

    /* the same name in both kinds is two entries */
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "de-DE", 31, 2, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(13, ps_find(&S, PS_KIND_LANGUAGE, "de-DE")->sha256[0]);
    TEST_ASSERT_EQUAL_UINT8(31, ps_find(&S, PS_KIND_VOICE, "de-DE")->sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0, ps_find(&S, PS_KIND_VOICE, "de-DE")->flags);
    TEST_ASSERT_EQUAL_UINT16(4, S.count);
    TEST_ASSERT_EQUAL_UINT32((SMALL - 2u) - (4u + 6u + 3u + 2u), ps_free_sectors(&S));
    TEST_ASSERT_TRUE(blobs_intact(&S));
    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
    assert_remount_matches(&S);
}

static void test_put_blob_needs_room_for_both_copies(void)
{
    /* AUD-027 (D12): admitted only when free sectors hold the whole new copy; the working copy is
     * never deleted to make room, and a refusal writes nothing */
    static ps_store_t before;
    mount_fresh(SMALL); /* 38 blob sectors */
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "primary", 41, 20, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_LANGUAGE, "es-US", 42, 10, NULL, NULL));
    before = S;
    uint32_t ops = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_ERR_NO_SPACE, install(PS_KIND_VOICE, "primary", 43, 20, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_ERR_NO_SPACE, install(PS_KIND_LANGUAGE, "es-US", 44, 9, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);
    TEST_ASSERT_TRUE(store_eq(&S, &before));
    assert_remount_matches(&before);
    /* a copy that fits beside the working one goes in, and the old run is free afterwards */
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "primary", 45, 8, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32((SMALL - 2u) - 18u, ps_free_sectors(&S));
    TEST_ASSERT_EQUAL_UINT8(42, ps_find(&S, PS_KIND_LANGUAGE, "es-US")->sha256[0]);
    assert_remount_matches(&S);

    /* a 64th entry is refused before anything is written; replacing one still works */
    mount_fresh(MODEL_SECTORS);
    char name[8];
    for (unsigned i = 0; i < PS_MAX_ENTRIES; i++) {
        name[0] = 'v';
        name[1] = (char)('0' + i / 10u);
        name[2] = (char)('0' + i % 10u);
        name[3] = '\0';
        TEST_ASSERT_EQUAL_INT(PS_OK, install((uint8_t)(i % 2u ? PS_KIND_VOICE : PS_KIND_LANGUAGE), name,
                                             (uint8_t)i, 1, NULL, NULL));
    }
    ops = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_ERR_FULL, install(PS_KIND_VOICE, "extra", 99, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "v01", 98, 2, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(PS_MAX_ENTRIES, S.count);
    assert_remount_matches(&S);
}

/* A language and a voice; the voice is then replaced (its blob: an erase and a write for each of
 * its 4 sectors; then the table's erase and write). */
static void lang_and_voice(ps_store_t *before)
{
    mount_fresh(SMALL);
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_LANGUAGE, "de-DE", 1, 3, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "primary", 2, 4, NULL, NULL));
    *before = S;
}

static void test_put_blob_faults_keep_the_working_copy(void)
{
    /* AUD-027: a fault anywhere before the table commit - the blob's erase or write, a write that
     * silently lost a bit (caught by the read-back) - leaves the old entry and every other one, in
     * memory and after a restart; a fault in the commit itself leaves the old state or the new */
    static ps_store_t before;
    for (uint32_t step = 1; step <= 10; step++) {
        for (int mode = 0; mode < 3; mode++) {
            lang_and_voice(&before);
            const uint32_t base = M.ops;
            M.mode = mode;
            M.fail_at = base + step;
            const ps_result_t r = install(PS_KIND_VOICE, "primary", 3, 4, NULL, NULL);
            /* the table write (step 10) lands whole when half or all of it reached flash */
            const bool lands = step == 10u && mode >= 1;
            M.cut = false;
            M.fail_at = 0;
            if (lands) {
                TEST_ASSERT_EQUAL_INT(PS_OK, r);
                TEST_ASSERT_EQUAL_UINT8(3, ps_find(&S, PS_KIND_VOICE, "primary")->sha256[0]);
            } else {
                TEST_ASSERT_EQUAL_INT(PS_ERR_IO, r);
                TEST_ASSERT_TRUE(store_eq(&S, &before));
                TEST_ASSERT_EQUAL_UINT32(before.sequence, S.sequence);
            }
            TEST_ASSERT_TRUE(entry_eq(ps_find(&S, PS_KIND_LANGUAGE, "de-DE"), ps_find(&before, PS_KIND_LANGUAGE, "de-DE")));
            TEST_ASSERT_TRUE(blobs_intact(&S));
            assert_remount_matches(&S);
            /* and the store still works: the next install goes through */
            TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "primary", 4, 4, NULL, NULL));
            TEST_ASSERT_TRUE(blobs_intact(&S));
        }
    }
    /* a write that reported success but stored something else: never committed */
    lang_and_voice(&before);
    M.corrupt_at = M.ops + 2u;
    const uint32_t base = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, install(PS_KIND_VOICE, "primary", 3, 4, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(base + 2u, M.ops); /* the first sector's read-back stops it: no more */
    TEST_ASSERT_TRUE(store_eq(&S, &before));
    assert_remount_matches(&before);
    /* a broken store (a failed re-read) refuses until a mount works */
    S.broken = true;
    TEST_ASSERT_EQUAL_INT(PS_ERR_IO, install(PS_KIND_LANGUAGE, "it-IT", 5, 1, NULL, NULL));
    S.broken = false;
}

typedef struct {
    unsigned calls;
    unsigned open_calls; /* the gate is open for this many calls, then closed */
} gate_t;

static bool test_gate(void *ctx)
{
    gate_t *g = ctx;
    g->calls++;
    return g->calls <= g->open_calls;
}

static void test_put_blob_gate(void)
{
    /* AUD-027 (D12 "recheck before activation", "abort optional installation work when the gate
     * closes"): the gate is asked before every sector's erase, write and read-back, and again
     * just before the table commit; closed at any of them, the install stops at once and nothing
     * is committed: the old entry reads as before, in memory and after a restart */
    static ps_store_t before;
    const uint32_t nsec = 4;
    gate_t g = {0, 0};
    lang_and_voice(&before);
    uint32_t base = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_ERR_CLOSED, install(PS_KIND_VOICE, "primary", 3, nsec, test_gate, &g));
    TEST_ASSERT_EQUAL_UINT(1, g.calls);
    TEST_ASSERT_EQUAL_UINT32(base, M.ops); /* N = 0: nothing erased */
    TEST_ASSERT_TRUE(store_eq(&S, &before));

    /* N = 1 (closes after the first sector), mid, last (every sector written, the final check) */
    const unsigned open[] = {1u, 2u, nsec};
    for (size_t k = 0; k < sizeof open / sizeof open[0]; k++) {
        lang_and_voice(&before);
        base = M.ops;
        g.calls = 0;
        g.open_calls = open[k];
        TEST_ASSERT_EQUAL_INT(PS_ERR_CLOSED, install(PS_KIND_VOICE, "primary", 3, nsec, test_gate, &g));
        TEST_ASSERT_EQUAL_UINT(open[k] + 1u, g.calls); /* stopped at the first closed answer */
        TEST_ASSERT_EQUAL_UINT32(base + 2u * open[k], M.ops); /* N sectors' erase and write, no table */
        TEST_ASSERT_EQUAL_UINT32(before.sequence, S.sequence);
        TEST_ASSERT_TRUE(store_eq(&S, &before));
        TEST_ASSERT_EQUAL_UINT8(2, ps_find(&S, PS_KIND_VOICE, "primary")->sha256[0]);
        TEST_ASSERT_TRUE(blobs_intact(&S));
        assert_remount_matches(&before);
        TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
    }

    /* the half-written run is free space: the next install takes it and succeeds */
    g.calls = 0;
    g.open_calls = nsec + 1u;
    base = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_VOICE, "primary", 3, nsec, test_gate, &g));
    TEST_ASSERT_EQUAL_UINT(nsec + 1u, g.calls);
    TEST_ASSERT_EQUAL_UINT32(base + 2u * nsec + 2u, M.ops);
    TEST_ASSERT_EQUAL_UINT8(3, ps_find(&S, PS_KIND_VOICE, "primary")->sha256[0]);
    TEST_ASSERT_TRUE(blobs_intact(&S));
    TEST_ASSERT_EQUAL_UINT32(0, M.dirty);
    assert_remount_matches(&S);

    /* the language-pack path (no gate) is the same transaction, never stopped */
    base = M.ops;
    TEST_ASSERT_EQUAL_INT(PS_OK, install(PS_KIND_LANGUAGE, "de-DE", 5, 3, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(base + 2u * 3u + 2u, M.ops);
    TEST_ASSERT_EQUAL_UINT8(5, ps_find(&S, PS_KIND_LANGUAGE, "de-DE")->sha256[0]);
    TEST_ASSERT_TRUE(blobs_intact(&S));
    assert_remount_matches(&S);
}

static void test_put_blob_refuses_bad_arguments(void)
{
    /* AUD-027: a bad request writes nothing */
    ps_entry_t e;
    mount_fresh(SMALL);
    const uint32_t ops = M.ops;
    size_t len = make_file(7, 2);
    e = wanted(PS_KIND_VOICE, "primary", 7);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, &e, FILEBUF, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, &e, FILEBUF, len - 1u, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, &e, NULL, len, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, NULL, FILEBUF, len, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(NULL, &e, FILEBUF, len, NULL, NULL));
    e.kind = 0;
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, &e, FILEBUF, len, NULL, NULL));
    e = wanted(PS_KIND_VOICE, "", 7);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, &e, FILEBUF, len, NULL, NULL));
    e = wanted(PS_KIND_VOICE, "bad\tname", 7);
    TEST_ASSERT_EQUAL_INT(PS_ERR_ARG, ps_put_blob(&S, &e, FILEBUF, len, NULL, NULL));
    e = wanted(PS_KIND_VOICE, "primary", 7);
    TEST_ASSERT_EQUAL_INT(PS_ERR_NO_SPACE, ps_put_blob(&S, &e, FILEBUF, 39u * PS_SECTOR, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(ops, M.ops);
    TEST_ASSERT_EQUAL_UINT16(0, S.count);
}

int main(void);
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_is_iso_hdlc);
    RUN_TEST(test_encode_layout_and_round_trip);
    RUN_TEST(test_encode_refuses_bad_tables);
    RUN_TEST(test_mount_reads_only_and_starts_empty);
    RUN_TEST(test_mount_a_and_b);
    RUN_TEST(test_mount_rejects_invalid_copies);
    RUN_TEST(test_mount_garbage_is_an_empty_store);
    RUN_TEST(test_first_fit_with_holes);
    RUN_TEST(test_runs_under_entries_are_refused);
    RUN_TEST(test_full_table);
    RUN_TEST(test_put_rules);
    RUN_TEST(test_name_rules);
    RUN_TEST(test_unknown_kinds_and_flags_are_carried);
    RUN_TEST(test_failed_commit_keeps_the_state);
    RUN_TEST(test_power_cut_after_every_operation);
    RUN_TEST(test_power_cut_without_remount);
    RUN_TEST(test_failed_reread_breaks_the_store_until_mount);
    RUN_TEST(test_sequence_wrap);
    RUN_TEST(test_duplicate_entries_are_invalid);
    RUN_TEST(test_frozen_fixture);
    RUN_TEST(test_envelope);
    RUN_TEST(test_put_blob_alternating_kinds);
    RUN_TEST(test_put_blob_needs_room_for_both_copies);
    RUN_TEST(test_put_blob_faults_keep_the_working_copy);
    RUN_TEST(test_put_blob_gate);
    RUN_TEST(test_put_blob_refuses_bad_arguments);
    return UNITY_END();
}
